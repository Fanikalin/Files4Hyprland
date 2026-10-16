/* Undo / redo for file operations: rename, move, copy, trash and create.
 * Every finished operation is recorded as an Op (pairs of src -> dst);
 * undo/redo replay it backwards/forwards in a worker thread. */
#include "app.h"

#define MAX_UNDO 100

Op *op_new(OpKind kind, gboolean is_dir) {
    Op *op = g_new0(Op, 1);
    op->kind = kind;
    op->is_dir = is_dir;
    op->src = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
    op->dst = g_ptr_array_new_with_free_func((GDestroyNotify)g_object_unref);
    return op;
}

static void unref0(gpointer p) {
    if (p) g_object_unref(p);
}

void op_add(Op *op, GFile *src, GFile *dst) {
    g_ptr_array_set_free_func(op->src, unref0);
    g_ptr_array_set_free_func(op->dst, unref0);
    g_ptr_array_add(op->src, src ? g_object_ref(src) : NULL);
    g_ptr_array_add(op->dst, dst ? g_object_ref(dst) : NULL);
}

void op_free(Op *op) {
    g_ptr_array_unref(op->src);
    g_ptr_array_unref(op->dst);
    g_free(op);
}

static char *op_desc(Op *op) {
    guint n = op->src->len;
    switch (op->kind) {
    case OP_RENAME: return g_strdup("rename");
    case OP_MOVE: return g_strdup_printf("move of %u item(s)", n);
    case OP_COPY: return g_strdup_printf("copy of %u item(s)", n);
    case OP_TRASH: return g_strdup_printf("trash of %u item(s)", n);
    case OP_CREATE: return g_strdup(op->is_dir ? "new folder" : "new file");
    }
    return g_strdup("operation");
}

static void set_enabled(App *a, const char *name, gboolean on, Op *op, const char *verb) {
    GAction *act = g_action_map_lookup_action(G_ACTION_MAP(a->win), name);
    if (act) g_simple_action_set_enabled(G_SIMPLE_ACTION(act), on);
    GtkWidget *b = g_hash_table_lookup(a->buttons, name);
    if (b) {
        g_autofree char *d = op ? op_desc(op) : NULL;
        g_autofree char *tip = d ? g_strdup_printf("%s %s (%s)", verb, d, g_str_equal(name, "undo") ? "Ctrl+Z" : "Ctrl+Shift+Z / Ctrl+Y")
                                 : g_strdup_printf("%s (%s)", verb, g_str_equal(name, "undo") ? "Ctrl+Z" : "Ctrl+Shift+Z / Ctrl+Y");
        gtk_widget_set_tooltip_text(b, tip);
    }
}

void undo_update(App *a) {
    set_enabled(a, "undo", !g_queue_is_empty(&a->undo), g_queue_peek_head(&a->undo), "Undo");
    set_enabled(a, "redo", !g_queue_is_empty(&a->redo), g_queue_peek_head(&a->redo), "Redo");
}

static void clear_queue(GQueue *q) {
    Op *op;
    while ((op = g_queue_pop_head(q))) op_free(op);
}

static void push(GQueue *q, Op *op) {
    g_queue_push_head(q, op);
    while (g_queue_get_length(q) > MAX_UNDO) op_free(g_queue_pop_tail(q));
}

void undo_push(App *a, Op *op) {
    push(&a->undo, op);
    clear_queue(&a->redo);
    undo_update(a);
}

/* ---------------------------------------------------------------- replay */
/* Find the newest trash entry whose original path is `orig` and move it back. */
static gboolean restore_from_trash(GFile *orig, GError **err) {
    g_autofree char *path = g_file_get_path(orig);
    g_autoptr(GFile) trash = g_file_new_for_uri("trash:///");
    g_autoptr(GFileEnumerator) e = g_file_enumerate_children(trash, "standard::name,trash::orig-path,trash::deletion-date",
                                                             G_FILE_QUERY_INFO_NONE, NULL, err);
    if (!e) return FALSE;
    g_autofree char *best = NULL, *best_date = NULL;
    GFileInfo *fi;
    while ((fi = g_file_enumerator_next_file(e, NULL, NULL))) {
        const char *op = g_file_info_get_attribute_byte_string(fi, "trash::orig-path");
        const char *date = g_file_info_get_attribute_string(fi, "trash::deletion-date");
        if (op && path && g_str_equal(op, path) && (!best_date || g_strcmp0(date, best_date) > 0)) {
            g_free(best);
            g_free(best_date);
            best = g_strdup(g_file_info_get_name(fi));
            best_date = g_strdup(date);
        }
        g_object_unref(fi);
    }
    if (!best) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "“%s” is no longer in the Trash", path);
        return FALSE;
    }
    g_autoptr(GFile) item = g_file_get_child(trash, best);
    g_autoptr(GFile) parent = g_file_get_parent(orig);
    g_autofree char *name = g_file_get_basename(orig);
    g_file_make_directory_with_parents(parent, NULL, NULL);
    g_autoptr(GFile) target = unique_child(parent, name);
    return g_file_move(item, target, G_FILE_COPY_NOFOLLOW_SYMLINKS, NULL, NULL, NULL, err);
}

static gboolean exists_error(GFile *f, GError **err) {
    if (!g_file_query_exists(f, NULL)) return FALSE;
    g_autofree char *p = g_file_get_parse_name(f);
    g_set_error(err, G_IO_ERROR, G_IO_ERROR_EXISTS, "“%s” already exists", p);
    return TRUE;
}

static gboolean replay_one(Op *op, guint i, gboolean undo, GError **err) {
    GFile *src = op->src->pdata[i], *dst = op->dst->pdata[i];
    switch (op->kind) {
    case OP_RENAME:
    case OP_MOVE: {
        GFile *from = undo ? dst : src, *to = undo ? src : dst;
        if (exists_error(to, err)) return FALSE;
        return fs_move(from, to, err);
    }
    case OP_COPY:
        if (undo) return g_file_trash(dst, NULL, err);
        if (exists_error(dst, err)) return FALSE;
        return fs_copy(src, dst, err);
    case OP_TRASH:
        return undo ? restore_from_trash(src, err) : g_file_trash(src, NULL, err);
    case OP_CREATE:
        if (undo) {
            g_autoptr(GError) e = NULL;
            if (g_file_delete(dst, NULL, &e)) return TRUE; /* empty: no need to clutter the Trash */
            return g_file_trash(dst, NULL, err);
        }
        if (exists_error(dst, err)) return FALSE;
        if (op->is_dir) return g_file_make_directory(dst, NULL, err);
        g_autoptr(GFileOutputStream) os = g_file_create(dst, G_FILE_CREATE_NONE, NULL, err);
        return os && g_output_stream_close(G_OUTPUT_STREAM(os), NULL, err);
    }
    return FALSE;
}

typedef struct {
    App *a;
    Op *op;
    gboolean undo;
    guint failed;
    GString *errors;
} Replay;

static void replay_thread(GTask *t, gpointer s, gpointer data, GCancellable *c) {
    (void)t; (void)s; (void)c;
    Replay *r = data;
    /* undo in reverse order so nested moves unwind correctly */
    guint n = r->op->src->len;
    for (guint k = 0; k < n; k++) {
        guint i = r->undo ? n - 1 - k : k;
        g_autoptr(GError) err = NULL;
        if (!replay_one(r->op, i, r->undo, &err)) {
            r->failed++;
            if (r->errors->len < 300)
                g_string_append_printf(r->errors, "%s%s", r->errors->len ? "; " : "", err ? err->message : "error");
        }
    }
}

static void replay_done(GObject *s, GAsyncResult *res, gpointer data) {
    (void)s; (void)res;
    Replay *r = data;
    App *a = r->a;
    g_autofree char *d = op_desc(r->op);
    if (r->failed) toast(a, "%s %s: %u failed: %s", r->undo ? "Undo" : "Redo", d, r->failed, r->errors->str);
    else toast(a, "%s %s", r->undo ? "Undid" : "Redid", d);
    /* the op moves to the other stack even if partly failed, so it can be retried */
    push(r->undo ? &a->redo : &a->undo, r->op);
    undo_update(a);
    queue_status(a);
    g_string_free(r->errors, TRUE);
    g_free(r);
}

void undo_run(App *a, gboolean redo) {
    if (a->renaming) view_focus(a); /* cancels the in-place rename */
    GQueue *from = redo ? &a->redo : &a->undo;
    Op *op = g_queue_pop_head(from);
    if (!op) {
        toast(a, "Nothing to %s", redo ? "redo" : "undo");
        return;
    }
    undo_update(a);
    Replay *r = g_new0(Replay, 1);
    r->a = a;
    r->op = op;
    r->undo = !redo;
    r->errors = g_string_new(NULL);
    g_autoptr(GTask) t = g_task_new(NULL, NULL, replay_done, r);
    g_task_set_task_data(t, r, NULL);
    g_task_run_in_thread(t, replay_thread);
}
