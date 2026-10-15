/* Helpers: sizes, unique names, toasts, opening files and background file jobs. */
#include "app.h"
#include <string.h>

char *human_size(guint64 n) {
    const char *u[] = {"B", "K", "M", "G", "T"};
    double s = n;
    int i = 0;
    while (s >= 1024 && i < 4) {
        s /= 1024;
        i++;
    }
    return i == 0 ? g_strdup_printf("%" G_GUINT64_FORMAT " B", n) : g_strdup_printf("%.1f %s", s, u[i]);
}

GFile *unique_child(GFile *dir, const char *name) {
    GFile *f = g_file_get_child(dir, name);
    if (!g_file_query_exists(f, NULL)) return f;
    g_object_unref(f);
    const char *dot = strrchr(name, '.');
    if (dot == name) dot = NULL; /* hidden file, no extension */
    g_autofree char *stem = dot ? g_strndup(name, dot - name) : g_strdup(name);
    const char *ext = dot ? dot : "";
    for (int i = 1;; i++) {
        g_autofree char *n = g_strdup_printf("%s (%d)%s", stem, i, ext);
        f = g_file_get_child(dir, n);
        if (!g_file_query_exists(f, NULL)) return f;
        g_object_unref(f);
    }
}

/* ---------------------------------------------------------------- toasts */
static guint toast_timer;

static gboolean toast_hide(gpointer data) {
    App *a = data;
    gtk_revealer_set_reveal_child(GTK_REVEALER(a->toast_rev), FALSE);
    toast_timer = 0;
    return G_SOURCE_REMOVE;
}

static void toast_show(App *a, const char *msg, gboolean error) {
    gtk_label_set_text(GTK_LABEL(a->toast_label), msg);
    if (error) gtk_widget_add_css_class(a->toast_label, "error");
    else gtk_widget_remove_css_class(a->toast_label, "error");
    gtk_revealer_set_reveal_child(GTK_REVEALER(a->toast_rev), TRUE);
    if (toast_timer) g_source_remove(toast_timer);
    toast_timer = g_timeout_add(error ? 4500 : 2500, toast_hide, a);
}

void toast(App *a, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    g_autofree char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    toast_show(a, msg, FALSE);
}

static void toast_error(App *a, const char *msg) { toast_show(a, msg, TRUE); }

/* ---------------------------------------------------------------- items */
GFileInfo *info_at(App *a, guint pos) { return g_list_model_get_item(G_LIST_MODEL(a->sorted), pos); }

GFile *info_file(GFileInfo *info) { return G_FILE(g_file_info_get_attribute_object(info, "standard::file")); }

gboolean info_is_dir(GFileInfo *info) { return g_file_info_get_file_type(info) == G_FILE_TYPE_DIRECTORY; }

/* ---------------------------------------------------------------- opening */
static void launched(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    g_autoptr(GError) err = NULL;
    if (!g_app_info_launch_default_for_uri_finish(res, &err)) toast_error(data, err->message);
}

void open_file(App *a, GFile *file) {
    g_autofree char *uri = g_file_get_uri(file);
    g_autoptr(GdkAppLaunchContext) ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
    g_app_info_launch_default_for_uri_async(uri, G_APP_LAUNCH_CONTEXT(ctx), NULL, launched, a);
}

/* ---------------------------------------------------------------- jobs */
typedef enum { JOB_COPY, JOB_MOVE, JOB_TRASH } JobKind;

typedef struct {
    App *a;
    JobKind kind;
    GList *files; /* GFile* */
    GFile *dest;
    int done, failed;
    GString *errors;
} Job;

static void job_free(Job *j) {
    g_list_free_full(j->files, g_object_unref);
    g_clear_object(&j->dest);
    g_string_free(j->errors, TRUE);
    g_free(j);
}

static gboolean run_tool(const char *tool, GFile *src, GFile *dst, GError **err) {
    g_autofree char *s = g_file_get_path(src), *d = g_file_get_path(dst);
    const char *cp[] = {"cp", "-a", "--reflink=auto", "--", s, d, NULL};
    const char *mv[] = {"mv", "-n", "--", s, d, NULL};
    int status = 0;
    g_autofree char *errout = NULL;
    if (!g_spawn_sync(NULL, (char **)(g_str_equal(tool, "cp") ? cp : mv), NULL, G_SPAWN_SEARCH_PATH, NULL,
                      NULL, NULL, &errout, &status, err))
        return FALSE;
    if (!g_spawn_check_wait_status(status, NULL)) {
        g_set_error_literal(err, G_IO_ERROR, G_IO_ERROR_FAILED, errout && *errout ? g_strstrip(errout) : "failed");
        return FALSE;
    }
    return TRUE;
}

/* Recursive GIO copy, for non-local locations (MTP, SMB, trash:// ...). */
static gboolean copy_tree(GFile *src, GFile *dst, GError **err) {
    GFileType t = g_file_query_file_type(src, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL);
    if (t != G_FILE_TYPE_DIRECTORY)
        return g_file_copy(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA, NULL, NULL, NULL, err);
    if (!g_file_make_directory(dst, NULL, err)) return FALSE;
    g_autoptr(GFileEnumerator) e = g_file_enumerate_children(src, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, err);
    if (!e) return FALSE;
    GFileInfo *fi;
    while ((fi = g_file_enumerator_next_file(e, NULL, err))) {
        g_autoptr(GFile) cs = g_file_get_child(src, g_file_info_get_name(fi));
        g_autoptr(GFile) cd = g_file_get_child(dst, g_file_info_get_name(fi));
        g_object_unref(fi);
        if (!copy_tree(cs, cd, err)) return FALSE;
    }
    return err == NULL || *err == NULL;
}

static gboolean delete_tree(GFile *f, GError **err) {
    GFileType t = g_file_query_file_type(f, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL);
    if (t == G_FILE_TYPE_DIRECTORY) {
        g_autoptr(GFileEnumerator) e = g_file_enumerate_children(f, "standard::name", G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, err);
        if (!e) return FALSE;
        GFileInfo *fi;
        while ((fi = g_file_enumerator_next_file(e, NULL, NULL))) {
            g_autoptr(GFile) c = g_file_get_child(f, g_file_info_get_name(fi));
            g_object_unref(fi);
            if (!delete_tree(c, err)) return FALSE;
        }
    }
    return g_file_delete(f, NULL, err);
}

static gboolean do_one(Job *j, GFile *src, GError **err) {
    if (j->kind == JOB_TRASH) return g_file_trash(src, NULL, err);

    g_autofree char *name = g_file_get_basename(src);
    g_autoptr(GFile) parent = g_file_get_parent(src);
    if (j->kind == JOB_MOVE && parent && g_file_equal(parent, j->dest)) return TRUE; /* already here */
    if (g_file_equal(src, j->dest) || g_file_has_prefix(j->dest, src)) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "can't put “%s” into itself", name);
        return FALSE;
    }
    g_autoptr(GFile) dst = unique_child(j->dest, name);
    gboolean local = g_file_is_native(src) && g_file_is_native(dst);
    if (local) return run_tool(j->kind == JOB_COPY ? "cp" : "mv", src, dst, err);
    if (j->kind == JOB_MOVE) {
        g_autoptr(GError) e = NULL;
        if (g_file_move(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA, NULL, NULL, NULL, &e))
            return TRUE;
        if (!copy_tree(src, dst, err)) return FALSE;
        return delete_tree(src, err);
    }
    return copy_tree(src, dst, err);
}

static void job_thread(GTask *task, gpointer src, gpointer data, GCancellable *c) {
    (void)task; (void)src; (void)c;
    Job *j = data;
    for (GList *l = j->files; l; l = l->next) {
        g_autoptr(GError) err = NULL;
        if (do_one(j, l->data, &err)) j->done++;
        else {
            j->failed++;
            if (j->errors->len < 300) g_string_append_printf(j->errors, "%s%s", j->errors->len ? "; " : "", err ? err->message : "error");
        }
    }
}

static void job_done(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src; (void)res;
    Job *j = data;
    const char *verb = j->kind == JOB_COPY ? "Copied" : j->kind == JOB_MOVE ? "Moved" : "Moved to Trash";
    if (j->failed) {
        g_autofree char *m = g_strdup_printf("%s %d, failed %d: %s", verb, j->done, j->failed, j->errors->str);
        toast_error(j->a, m);
    } else if (j->kind == JOB_TRASH) {
        toast(j->a, "Moved %d item(s) to Trash", j->done);
    } else {
        toast(j->a, "%s %d item(s)", verb, j->done);
    }
    queue_status(j->a);
    job_free(j);
}

static gpointer ref_copy(gconstpointer src, gpointer data) {
    (void)data;
    return g_object_ref((gpointer)src);
}

static void run_job(App *a, JobKind kind, GList *files, GFile *dest) {
    if (!files) return;
    Job *j = g_new0(Job, 1);
    j->a = a;
    j->kind = kind;
    j->files = g_list_copy_deep(files, ref_copy, NULL);
    j->dest = dest ? g_object_ref(dest) : NULL;
    j->errors = g_string_new(NULL);
    g_autoptr(GTask) t = g_task_new(NULL, NULL, job_done, j);
    g_task_set_task_data(t, j, NULL);
    g_task_run_in_thread(t, job_thread);
}

void transfer_files(App *a, GList *files, GFile *dest, gboolean copy) {
    run_job(a, copy ? JOB_COPY : JOB_MOVE, files, dest);
}

void trash_files(App *a, GList *files) { run_job(a, JOB_TRASH, files, NULL); }
