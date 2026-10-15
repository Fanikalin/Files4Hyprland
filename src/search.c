/* Recursive search: a worker thread walks the current folder and its
 * subfolders breadth-first and streams matches into a GListStore that
 * temporarily replaces the directory listing. */
#include "app.h"
#include <string.h>

#define MAX_RESULTS 20000
#define BATCH 256

typedef struct {
    App *a;
    GFile *root;
    char *query; /* casefolded */
    gboolean hidden;
    GCancellable *cancel;
    GListStore *store;
} Search;

typedef struct {
    Search *s;
    GPtrArray *items; /* GFileInfo* */
} Batch;

static void search_free(Search *s) {
    g_object_unref(s->root);
    g_object_unref(s->cancel);
    g_object_unref(s->store);
    g_free(s->query);
    g_free(s);
}

static gboolean flush_cb(gpointer data) {
    Batch *b = data;
    if (!g_cancellable_is_cancelled(b->s->cancel))
        g_list_store_splice(b->s->store, g_list_model_get_n_items(G_LIST_MODEL(b->s->store)), 0, b->items->pdata,
                            b->items->len);
    g_ptr_array_unref(b->items);
    g_free(b);
    return G_SOURCE_REMOVE;
}

static void flush(Search *s, GPtrArray **items) {
    if (!(*items)->len) return;
    Batch *b = g_new0(Batch, 1);
    b->s = s;
    b->items = *items;
    g_main_context_invoke(NULL, flush_cb, b);
    *items = g_ptr_array_new_with_free_func(g_object_unref);
}

static gboolean done_cb(gpointer data) {
    Search *s = data;
    if (!g_cancellable_is_cancelled(s->cancel)) queue_status(s->a);
    search_free(s);
    return G_SOURCE_REMOVE;
}

static gpointer search_thread(gpointer data) {
    Search *s = data;
    GQueue dirs = G_QUEUE_INIT;
    g_queue_push_tail(&dirs, g_object_ref(s->root));
    GPtrArray *items = g_ptr_array_new_with_free_func(g_object_unref);
    guint found = 0;
    gint64 last = g_get_monotonic_time();
    GFile *dir;
    while ((dir = g_queue_pop_head(&dirs))) {
        if (g_cancellable_is_cancelled(s->cancel) || found >= MAX_RESULTS) {
            g_object_unref(dir);
            continue;
        }
        g_autofree char *rel = g_file_get_relative_path(s->root, dir);
        GFileEnumerator *e = g_file_enumerate_children(dir, F4H_ATTRS, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, s->cancel, NULL);
        GFileInfo *fi;
        while (e && (fi = g_file_enumerator_next_file(e, s->cancel, NULL))) {
            const char *name = g_file_info_get_display_name(fi);
            gboolean hidden = name[0] == '.' || g_file_info_get_is_hidden(fi) || g_file_info_get_is_backup(fi);
            if (hidden && !s->hidden) {
                g_object_unref(fi);
                continue;
            }
            GFile *child = g_file_get_child(dir, g_file_info_get_name(fi));
            if (g_file_info_get_file_type(fi) == G_FILE_TYPE_DIRECTORY) g_queue_push_tail(&dirs, g_object_ref(child));
            g_autofree char *f = g_utf8_casefold(name, -1);
            if (strstr(f, s->query) && found < MAX_RESULTS) {
                g_file_info_set_attribute_object(fi, "standard::file", G_OBJECT(child));
                if (rel) g_file_info_set_attribute_string(fi, "f4h::rel", rel);
                g_ptr_array_add(items, fi);
                found++;
            } else {
                g_object_unref(fi);
            }
            g_object_unref(child);
            gint64 now = g_get_monotonic_time();
            if (items->len >= BATCH || (items->len && now - last > 80000)) {
                flush(s, &items);
                last = now;
            }
        }
        if (e) g_object_unref(e);
        g_object_unref(dir);
    }
    flush(s, &items);
    g_ptr_array_unref(items);
    g_main_context_invoke(NULL, done_cb, s);
    return NULL;
}

static gboolean start_cb(gpointer data) {
    App *a = data;
    a->search_timer = 0;
    Search *s = g_new0(Search, 1);
    s->a = a;
    s->root = g_object_ref(cur_tab(a)->dir);
    s->query = g_strdup(a->query);
    s->hidden = a->show_hidden;
    s->cancel = g_object_ref(a->search_cancel);
    s->store = g_object_ref(a->results);
    gtk_filter_list_model_set_model(a->filtered, G_LIST_MODEL(a->results));
    g_thread_unref(g_thread_new("f4h-search", search_thread, s));
    return G_SOURCE_REMOVE;
}

void search_set(App *a, const char *text) {
    if (a->search_cancel) {
        g_cancellable_cancel(a->search_cancel);
        g_clear_object(&a->search_cancel);
    }
    if (a->search_timer) {
        g_source_remove(a->search_timer);
        a->search_timer = 0;
    }
    g_free(a->query);
    a->query = text && *text ? g_utf8_casefold(text, -1) : NULL;

    if (!a->query) {
        if (gtk_filter_list_model_get_model(a->filtered) != G_LIST_MODEL(a->dirlist))
            gtk_filter_list_model_set_model(a->filtered, G_LIST_MODEL(a->dirlist));
        g_clear_object(&a->results);
        gtk_filter_changed(GTK_FILTER(a->filter), GTK_FILTER_CHANGE_DIFFERENT);
        return;
    }
    /* filter the current folder instantly, then replace it with the recursive results */
    gtk_filter_changed(GTK_FILTER(a->filter), GTK_FILTER_CHANGE_DIFFERENT);
    a->search_cancel = g_cancellable_new();
    g_clear_object(&a->results);
    a->results = g_list_store_new(G_TYPE_FILE_INFO);
    a->search_timer = g_timeout_add(250, start_cb, a);
}
