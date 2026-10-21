/* Helpers: sizes, unique names, toasts, opening files and background file jobs. */
#include "app.h"
#include <glib/gstdio.h>
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
    /* an archive nothing else opens: extract it into a folder next to it */
    g_autofree char *name = g_file_get_basename(file);
    if (g_file_is_native(file) && name_is_archive(name)) {
        g_autoptr(GFileInfo) info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_CONTENT_TYPE, 0, NULL, NULL);
        const char *type = info ? g_file_info_get_content_type(info) : NULL;
        g_autoptr(GAppInfo) app = type ? g_app_info_get_default_for_type(type, FALSE) : NULL;
        if (!app) {
            extract_archive(a, file, TRUE, TRUE);
            return;
        }
    }
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
    Op *rec; /* what actually happened, for undo */
} Job;

static void job_free(Job *j) {
    g_list_free_full(j->files, g_object_unref);
    g_clear_object(&j->dest);
    g_string_free(j->errors, TRUE);
    if (j->rec) op_free(j->rec);
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

gboolean fs_move(GFile *src, GFile *dst, GError **err) {
    if (g_file_is_native(src) && g_file_is_native(dst)) return run_tool("mv", src, dst, err);
    g_autoptr(GError) e = NULL;
    if (g_file_move(src, dst, G_FILE_COPY_NOFOLLOW_SYMLINKS | G_FILE_COPY_ALL_METADATA, NULL, NULL, NULL, &e))
        return TRUE;
    if (!copy_tree(src, dst, err)) return FALSE;
    return delete_tree(src, err);
}

gboolean fs_copy(GFile *src, GFile *dst, GError **err) {
    if (g_file_is_native(src) && g_file_is_native(dst)) return run_tool("cp", src, dst, err);
    return copy_tree(src, dst, err);
}

gboolean fs_delete(GFile *f, GError **err) { return delete_tree(f, err); }

static gboolean do_one(Job *j, GFile *src, GError **err) {
    if (j->kind == JOB_TRASH) {
        if (!g_file_trash(src, NULL, err)) return FALSE;
        op_add(j->rec, src, NULL);
        return TRUE;
    }

    g_autofree char *name = g_file_get_basename(src);
    g_autoptr(GFile) parent = g_file_get_parent(src);
    if (j->kind == JOB_MOVE && parent && g_file_equal(parent, j->dest)) return TRUE; /* already here */
    if (g_file_equal(src, j->dest) || g_file_has_prefix(j->dest, src)) {
        g_set_error(err, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "can't put “%s” into itself", name);
        return FALSE;
    }
    g_autoptr(GFile) dst = unique_child(j->dest, name);
    if (!(j->kind == JOB_COPY ? fs_copy(src, dst, err) : fs_move(src, dst, err))) return FALSE;
    op_add(j->rec, src, dst);
    return TRUE;
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
    if (j->rec->src->len) {
        undo_push(j->a, j->rec);
        j->rec = NULL;
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
    j->rec = op_new(kind == JOB_COPY ? OP_COPY : kind == JOB_MOVE ? OP_MOVE : OP_TRASH, FALSE);
    g_autoptr(GTask) t = g_task_new(NULL, NULL, job_done, j);
    g_task_set_task_data(t, j, NULL);
    g_task_run_in_thread(t, job_thread);
}

void transfer_files(App *a, GList *files, GFile *dest, gboolean copy) {
    run_job(a, copy ? JOB_COPY : JOB_MOVE, files, dest);
}

void trash_files(App *a, GList *files) { run_job(a, JOB_TRASH, files, NULL); }

/* ---------------------------------------------------------------- system language */
/* Only the words the app shows for default folders and sidebar sections are
 * translated; columns: ru uk de fr es. */
static const struct {
    const char *en, *t[5];
} L10N[] = {
    {"Home", {"Домашняя папка", "Домашня тека", "Persönlicher Ordner", "Dossier personnel", "Carpeta personal"}},
    {"Trash", {"Корзина", "Смітник", "Papierkorb", "Corbeille", "Papelera"}},
    {"System", {"Система", "Система", "System", "Système", "Sistema"}},
    {"Places", {"Места", "Місця", "Orte", "Emplacements", "Lugares"}},
    {"Network", {"Сеть", "Мережа", "Netzwerk", "Réseau", "Red"}},
    {"Phone", {"Телефон", "Телефон", "Telefon", "Téléphone", "Teléfono"}},
    {"Pinned", {"Закреплённые", "Закріплені", "Angeheftet", "Épinglés", "Fijados"}},
    {"Devices", {"Устройства", "Пристрої", "Geräte", "Périphériques", "Dispositivos"}},
    {"not mounted", {"не подключён", "не змонтовано", "nicht eingebunden", "non monté", "no montado"}},
    {"Desktop", {"Рабочий стол", "Робочий стіл", "Schreibtisch", "Bureau", "Escritorio"}},
    {"Documents", {"Документы", "Документи", "Dokumente", "Documents", "Documentos"}},
    {"Downloads", {"Загрузки", "Завантаження", "Downloads", "Téléchargements", "Descargas"}},
    {"Music", {"Музыка", "Музика", "Musik", "Musique", "Música"}},
    {"Pictures", {"Изображения", "Зображення", "Bilder", "Images", "Imágenes"}},
    {"Public", {"Общедоступные", "Загальнодоступні", "Öffentlich", "Public", "Público"}},
    {"Templates", {"Шаблоны", "Шаблони", "Vorlagen", "Modèles", "Plantillas"}},
    {"Videos", {"Видео", "Відео", "Videos", "Vidéos", "Vídeos"}},
};

static int lang_index(void) {
    static int idx = -2;
    if (idx != -2) return idx;
    static const char *langs[] = {"ru", "uk", "de", "fr", "es"};
    idx = -1;
    for (const char *const *n = g_get_language_names(); *n && idx < 0; n++)
        for (guint i = 0; i < G_N_ELEMENTS(langs); i++)
            if (g_str_has_prefix(*n, langs[i]) && ((*n)[2] == '\0' || (*n)[2] == '_' || (*n)[2] == '.' || (*n)[2] == '@')) idx = i;
    return idx;
}

const char *tr(const char *english) {
    int l = lang_index();
    if (l < 0) return english;
    for (guint i = 0; i < G_N_ELEMENTS(L10N); i++)
        if (g_str_equal(L10N[i].en, english)) return L10N[i].t[l];
    return english;
}

/* Default folders keep English names on disk (xdg-user-dirs made them before the
 * language was chosen); show them in the system language. User-renamed folders stay as they are. */
char *dir_display_name(GFile *f) {
    if (lang_index() < 0) return NULL;
    g_autofree char *path = g_file_get_path(f);
    if (!path) return NULL;
    if (g_str_equal(path, g_get_home_dir())) return g_strdup(tr("Home"));
    static const struct { GUserDirectory d; const char *en; } dirs[] = {
        {G_USER_DIRECTORY_DESKTOP, "Desktop"}, {G_USER_DIRECTORY_DOCUMENTS, "Documents"},
        {G_USER_DIRECTORY_DOWNLOAD, "Downloads"}, {G_USER_DIRECTORY_MUSIC, "Music"},
        {G_USER_DIRECTORY_PICTURES, "Pictures"}, {G_USER_DIRECTORY_PUBLIC_SHARE, "Public"},
        {G_USER_DIRECTORY_TEMPLATES, "Templates"}, {G_USER_DIRECTORY_VIDEOS, "Videos"},
    };
    g_autofree char *base = g_path_get_basename(path);
    for (guint i = 0; i < G_N_ELEMENTS(dirs); i++) {
        const char *p = g_get_user_special_dir(dirs[i].d);
        if (p && g_str_equal(p, path) && g_str_equal(base, dirs[i].en)) return g_strdup(tr(dirs[i].en));
    }
    return NULL;
}

/* ---------------------------------------------------------------- scripts */
/* "Open as program": run a shell script in a terminal that stays open afterwards. */
void run_script(App *a, GFile *file) {
    g_autofree char *path = g_file_get_path(file);
    g_autoptr(GFile) parent = g_file_get_parent(file);
    g_autofree char *dir = parent ? g_file_get_path(parent) : NULL;
    if (!path) return;
    const char *argv[] = {"xdg-terminal-exec", "bash", "-c",
                          "if [ -x \"$1\" ]; then \"$1\"; else bash \"$1\"; fi; s=$?; "
                          "printf '\\n[exit status %s] press Enter to close ' \"$s\"; read -r _",
                          "f4h-script", path, NULL};
    g_autoptr(GError) err = NULL;
    if (!g_spawn_async(dir, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err)) toast_error(a, err->message);
}

/* ---------------------------------------------------------------- archive extraction */
static gboolean tool_avail(const char *name) {
    g_autofree char *p = g_find_program_in_path(name);
    return p != NULL;
}

gboolean name_is_archive(const char *name) {
    if (!name) return FALSE;
    static const char *exts[] = {
        ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lz",
        ".tar.lzma", ".tar.Z", ".tar.lz4",
        ".zip", ".tar", ".tgz", ".tbz", ".tbz2", ".txz", ".tzst",
        ".gz", ".bz2", ".xz", ".zst", ".lz", ".lzma",
        ".rar", ".7z", ".Z", NULL
    };
    for (const char **e = exts; *e; e++)
        if (g_str_has_suffix(name, *e)) return TRUE;
    return FALSE;
}

static char *archive_stem(const char *name) {
    static const char *double_exts[] = {
        ".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lz",
        ".tar.lzma", ".tar.Z", ".tar.lz4", NULL
    };
    for (int i = 0; double_exts[i]; i++) {
        if (g_str_has_suffix(name, double_exts[i]))
            return g_strndup(name, strlen(name) - strlen(double_exts[i]));
    }
    const char *dot = strrchr(name, '.');
    if (dot && dot != name) return g_strndup(name, dot - name);
    return g_strdup(name);
}

typedef struct {
    App *a;
    GFile *archive;
    GFile *dest;
    gboolean into_folder;
    gboolean open_after;
    gboolean ok;
    gboolean tried;     /* some extraction tool was installed */
    gboolean encrypted; /* a tool said the archive needs a password */
    char *password;     /* NULL until the user typed one */
    char *folder_name;  /* stem, set when into_folder */
} ExtractJob;

static void extract_free(ExtractJob *j) {
    g_object_unref(j->archive);
    g_object_unref(j->dest);
    g_free(j->password);
    g_free(j->folder_name);
    g_free(j);
}

/* runs argv if its program is installed; *tried records that some tool was there.
 * stdin is /dev/null so a password prompt fails instead of hanging. */
static gboolean try_tool(ExtractJob *j, const char **argv) {
    if (!tool_avail(argv[0])) return FALSE;
    j->tried = TRUE;
    int st = -1;
    g_autofree char *out = NULL, *err = NULL;
    g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDIN_FROM_DEV_NULL,
                 NULL, NULL, &out, &err, &st, NULL);
    if (st == 0) return TRUE;
    g_autofree char *msg = g_strconcat(out ? out : "", err ? err : "", NULL);
    g_autofree char *low = g_ascii_strdown(msg, -1);
    static const char *hints[] = {"password", "passphrase", "encrypted", NULL};
    for (const char **h = hints; *h; h++)
        if (strstr(low, *h)) j->encrypted = TRUE;
    return FALSE;
}

static gboolean run_extract(ExtractJob *j, const char *path, const char *dest) {
    /* without a password give the tools one that can't match: they fail fast instead of prompting */
    const char *pw = j->password ? j->password : "\x01";
    g_autofree char *oarg = g_strdup_printf("-o%s", dest);
    g_autofree char *parg = g_strdup_printf("-p%s", pw);
    const char *unar[] = {"unar", "-D", "-force-overwrite", "-p", pw, "-o", dest, path, NULL};
    const char *sz[] = {"7z", "x", "-y", "-bso0", "-bsp0", parg, path, oarg, NULL};
    const char *szz[] = {"7zz", "x", "-y", "-bso0", "-bsp0", parg, path, oarg, NULL};
    /* libarchive: zip, 7z, rar (incl. v5), iso, cpio and every tar flavour; decrypts zip only */
    const char *bsdtar[] = {"bsdtar", "-xf", path, "-C", dest, "--passphrase", pw, NULL};
    const char *tar[] = {"tar", "-xf", path, "-C", dest, NULL};
    const char *unzip[] = {"unzip", "-o", "-P", pw, path, "-d", dest, NULL};
    const char **tools[] = {unar, sz, szz, bsdtar, tar, unzip, NULL};
    for (const char ***t = tools; *t; t++)
        if (try_tool(j, *t)) return TRUE;
    return FALSE;
}

/* only these decrypt rar and 7z; zip also works with unzip and bsdtar */
static gboolean can_decrypt(GFile *archive) {
    if (tool_avail("unar") || tool_avail("7z") || tool_avail("7zz")) return TRUE;
    g_autofree char *name = g_file_get_basename(archive);
    return g_str_has_suffix(name, ".zip") && (tool_avail("unzip") || tool_avail("bsdtar"));
}

static void extract_thread(GTask *task, gpointer src, gpointer data, GCancellable *c) {
    (void)src; (void)c;
    ExtractJob *j = data;
    g_autofree char *path = g_file_get_path(j->archive);
    g_autofree char *dest = g_file_get_path(j->dest);
    if (!path || !dest) {
        g_task_return_boolean(task, FALSE);
        return;
    }
    g_autoptr(GError) err = NULL;
    g_file_make_directory_with_parents(j->dest, NULL, &err);
    if (err && err->code != G_IO_ERROR_EXISTS) {
        g_task_return_boolean(task, FALSE);
        return;
    }
    j->ok = j->tried = j->encrypted = FALSE;
    j->ok = run_extract(j, path, dest);
    if (!j->ok && j->into_folder) g_file_delete(j->dest, NULL, NULL); /* only goes if left empty */
    g_task_return_boolean(task, TRUE);
}

static void extract_start(ExtractJob *j);

/* ---------------------------------------------------------------- password prompt */
static void pw_close(GtkWidget *win, ExtractJob *j, gboolean go) {
    if (go) {
        GtkWidget *e = g_object_get_data(G_OBJECT(win), "entry");
        g_free(j->password);
        j->password = g_strdup(gtk_editable_get_text(GTK_EDITABLE(e)));
        extract_start(j);
    } else {
        extract_free(j);
    }
    g_object_set_data(G_OBJECT(win), "job", NULL);
    gtk_window_destroy(GTK_WINDOW(win));
}

static void pw_ok(GtkWidget *w, gpointer win) { pw_close(win, g_object_get_data(win, "job"), TRUE); }
static void pw_cancel(GtkWidget *w, gpointer win) { pw_close(win, g_object_get_data(win, "job"), FALSE); }

static gboolean pw_close_request(GtkWindow *win, gpointer data) {
    ExtractJob *j = g_object_get_data(G_OBJECT(win), "job");
    if (j) {
        g_object_set_data(G_OBJECT(win), "job", NULL);
        extract_free(j);
    }
    return FALSE;
}

static void ask_password(ExtractJob *j, gboolean retry) {
    GtkWidget *win = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(win), "Password");
    gtk_window_set_modal(GTK_WINDOW(win), TRUE);
    gtk_window_set_transient_for(GTK_WINDOW(win), j->a->win);
    gtk_window_set_resizable(GTK_WINDOW(win), FALSE);
    g_object_set_data(G_OBJECT(win), "job", j);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);
    g_autofree char *name = g_file_get_basename(j->archive);
    g_autofree char *text = g_strdup_printf(retry ? "Wrong password for “%s”. Try again:" : "“%s” is password-protected:", name);
    GtkWidget *l = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_wrap(GTK_LABEL(l), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(l), 40);
    GtkWidget *e = gtk_password_entry_new();
    gtk_password_entry_set_show_peek_icon(GTK_PASSWORD_ENTRY(e), TRUE);
    g_object_set_data(G_OBJECT(win), "entry", e);
    g_signal_connect(e, "activate", G_CALLBACK(pw_ok), win);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_add_css_class(bar, "actions");
    gtk_widget_set_halign(bar, GTK_ALIGN_END);
    GtkWidget *cancel = gtk_button_new_with_label("Cancel");
    GtkWidget *ok = gtk_button_new_with_label("Extract");
    gtk_widget_add_css_class(ok, "suggested");
    g_signal_connect(cancel, "clicked", G_CALLBACK(pw_cancel), win);
    g_signal_connect(ok, "clicked", G_CALLBACK(pw_ok), win);
    gtk_box_append(GTK_BOX(bar), cancel);
    gtk_box_append(GTK_BOX(bar), ok);

    gtk_box_append(GTK_BOX(box), l);
    gtk_box_append(GTK_BOX(box), e);
    gtk_box_append(GTK_BOX(box), bar);
    gtk_window_set_child(GTK_WINDOW(win), box);
    g_signal_connect(win, "close-request", G_CALLBACK(pw_close_request), NULL);
    gtk_window_present(GTK_WINDOW(win));
    gtk_widget_grab_focus(e);
}

static void extract_done(GObject *src, GAsyncResult *res, gpointer data) {
    (void)src;
    ExtractJob *j = data;
    gboolean completed = g_task_propagate_boolean(G_TASK(res), NULL);
    if (completed && !j->ok && j->encrypted) {
        if (can_decrypt(j->archive)) {
            ask_password(j, j->password != NULL);
            return;
        }
        toast(j->a, "The archive is password-protected — install 7zip or unar to extract it");
    } else if (!completed || !j->ok) {
        toast(j->a, !completed || j->tried ? "Extraction failed" : "No extraction tool found — install unar or 7zip");
    } else {
        toast(j->a, "Extracted successfully");
        if (j->open_after) navigate(j->a, j->dest, TRUE, NULL);
        else if (j->folder_name) view_select_name(j->a, j->folder_name, FALSE);
    }
    extract_free(j);
}

static void extract_start(ExtractJob *j) {
    g_autoptr(GTask) task = g_task_new(NULL, NULL, extract_done, j);
    g_task_set_task_data(task, j, NULL);
    g_task_run_in_thread(task, extract_thread);
}

void extract_archive(App *a, GFile *archive, gboolean into_folder, gboolean open_after) {
    Tab *t = cur_tab(a);
    if (!t) return;
    ExtractJob *j = g_new0(ExtractJob, 1);
    j->a = a;
    j->archive = g_object_ref(archive);
    j->into_folder = into_folder;
    j->open_after = open_after;
    if (into_folder) {
        g_autofree char *name = g_file_get_basename(archive);
        g_autofree char *stem = archive_stem(name);
        j->folder_name = g_strdup(stem);
        g_autoptr(GFile) parent = g_file_get_parent(archive);
        j->dest = g_file_get_child(parent ? parent : t->dir, stem);
    } else {
        g_autoptr(GFile) parent = g_file_get_parent(archive);
        j->dest = g_object_ref(parent ? parent : t->dir);
    }
    extract_start(j);
}

/* ---------------------------------------------------------------- settings */
static char *settings_path(void) { return g_build_filename(g_get_user_config_dir(), "files4hyprland", "settings.ini", NULL); }

static gboolean valid_row_size(const char *s) {
    return s && (g_str_equal(s, "small") || g_str_equal(s, "medium") || g_str_equal(s, "large") ||
                 g_str_equal(s, "tiles") || g_str_equal(s, "tiles-large"));
}

void settings_load(App *a) {
    g_strlcpy(a->row_size, "medium", sizeof a->row_size);
    a->sort_col = SORT_NAME;
    if (!a->dir_views)
        a->dir_views = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_autofree char *p = settings_path();
    g_autoptr(GKeyFile) kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, p, G_KEY_FILE_NONE, NULL)) return;
    g_autofree char *sz = g_key_file_get_string(kf, "view", "rows", NULL);
    if (valid_row_size(sz)) g_strlcpy(a->row_size, sz, sizeof a->row_size);
    int col = g_key_file_get_integer(kf, "view", "sort", NULL);
    if (col >= SORT_NAME && col <= SORT_MTIME) a->sort_col = col;
    a->sort_desc = g_key_file_get_boolean(kf, "view", "descending", NULL);
    g_auto(GStrv) keys = g_key_file_get_keys(kf, "dirs", NULL, NULL);
    if (keys) {
        for (char **k = keys; *k; k++) {
            char *v = g_key_file_get_string(kf, "dirs", *k, NULL);
            if (valid_row_size(v)) g_hash_table_replace(a->dir_views, g_strdup(*k), v);
            else g_free(v);
        }
    }
}

void settings_save(App *a) {
    g_autofree char *p = settings_path();
    g_autofree char *dir = g_path_get_dirname(p);
    g_mkdir_with_parents(dir, 0755);
    g_autoptr(GKeyFile) kf = g_key_file_new();
    g_key_file_set_string(kf, "view", "rows", a->row_size);
    g_key_file_set_integer(kf, "view", "sort", a->sort_col);
    g_key_file_set_boolean(kf, "view", "descending", a->sort_desc);
    if (a->dir_views) {
        GHashTableIter it;
        gpointer k, v;
        g_hash_table_iter_init(&it, a->dir_views);
        while (g_hash_table_iter_next(&it, &k, &v))
            g_key_file_set_string(kf, "dirs", k, v);
    }
    g_key_file_save_to_file(kf, p, NULL);
}
