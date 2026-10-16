/* Context-menu entries contributed by other programs:
 *  - "Open with": applications associated with the file's type (GIO);
 *  - KDE service menus (kio/servicemenus/ in the data dirs), the format that
 *    Ark, VLC, KDE Connect, file-roller add-ons and many others install. */
#include "app.h"
#include <gio/gdesktopappinfo.h>
#include <string.h>

typedef struct {
    GAppInfo *app;     /* "Open with" entry, or */
    char *exec;        /* service-menu command line */
    char *desktop;     /* path of the .desktop file (%k) */
    char *name;
} Entry;

static GPtrArray *entries; /* Entry*, indices are the parameter of win.ext-run */

static void entry_free(Entry *e) {
    g_clear_object(&e->app);
    g_free(e->exec);
    g_free(e->desktop);
    g_free(e->name);
    g_free(e);
}

static char *content_type_of(GFile *f) {
    g_autoptr(GFileInfo) fi = g_file_query_info(f, "standard::content-type,standard::type", 0, NULL, NULL);
    if (!fi) return NULL;
    if (g_file_info_get_file_type(fi) == G_FILE_TYPE_DIRECTORY) return g_strdup("inode/directory");
    return g_strdup(g_file_info_get_content_type(fi));
}

static gboolean mime_matches(const char *pattern, const char *ct, gboolean is_dir) {
    if (g_str_equal(pattern, "all/all") || g_str_equal(pattern, "all/*")) return TRUE;
    if (g_str_equal(pattern, "all/allfiles")) return !is_dir;
    if (g_str_equal(pattern, "all/alldirectories")) return is_dir;
    size_t n = strlen(pattern);
    if (n > 2 && g_str_equal(pattern + n - 2, "/*")) return strncmp(ct, pattern, n - 1) == 0;
    return g_content_type_is_a(ct, pattern);
}

static GPtrArray *service_dirs(void) {
    GPtrArray *d = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(d, g_build_filename(g_get_user_data_dir(), "kio", "servicemenus", NULL));
    g_ptr_array_add(d, g_build_filename(g_get_user_data_dir(), "kservices5", "ServiceMenus", NULL));
    for (const char *const *s = g_get_system_data_dirs(); *s; s++) {
        g_ptr_array_add(d, g_build_filename(*s, "kio", "servicemenus", NULL));
        g_ptr_array_add(d, g_build_filename(*s, "kservices5", "ServiceMenus", NULL));
    }
    return d;
}

/* Adds the actions of one service-menu file that apply to all targets. */
static void load_service_file(const char *path, GList *targets, GPtrArray *cts, GPtrArray *dirs, GMenu *top,
                              GHashTable *subs) {
    g_autoptr(GKeyFile) kf = g_key_file_new();
    if (!g_key_file_load_from_file(kf, path, G_KEY_FILE_NONE, NULL)) return;
    const char *G = "Desktop Entry";
    if (g_key_file_get_boolean(kf, G, "Hidden", NULL) || g_key_file_get_boolean(kf, G, "NoDisplay", NULL)) return;
    g_auto(GStrv) types = g_key_file_get_string_list(kf, G, "X-KDE-ServiceTypes", NULL, NULL);
    if (types && !g_strv_contains((const char *const *)types, "KonqPopupMenu/Plugin") &&
        !g_strv_contains((const char *const *)types, "KonqPopupMenu/Information") &&
        !g_strv_contains((const char *const *)types, "KFileItemAction/Plugin"))
        return;
    g_auto(GStrv) mimes = g_key_file_get_string_list(kf, G, "MimeType", NULL, NULL);
    if (!mimes) return;
    guint ti = 0;
    for (GList *l = targets; l; l = l->next, ti++) {
        gboolean ok = FALSE;
        for (char **m = mimes; *m && !ok; m++)
            if (**m) ok = mime_matches(*m, cts->pdata[ti], GPOINTER_TO_INT(dirs->pdata[ti]));
        if (!ok) return;
    }
    int need = g_key_file_has_key(kf, G, "X-KDE-RequiredNumberOfUrls", NULL)
                   ? g_key_file_get_integer(kf, G, "X-KDE-RequiredNumberOfUrls", NULL) : 0;
    if (need > 0 && (int)g_list_length(targets) != need) return;
    g_autofree char *submenu = g_key_file_get_locale_string(kf, G, "X-KDE-Submenu", NULL, NULL);
    g_auto(GStrv) actions = g_key_file_get_string_list(kf, G, "Actions", NULL, NULL);
    for (char **act = actions; act && *act; act++) {
        g_autofree char *grp = g_strdup_printf("Desktop Action %s", *act);
        g_autofree char *exec = g_key_file_get_string(kf, grp, "Exec", NULL);
        g_autofree char *name = g_key_file_get_locale_string(kf, grp, "Name", NULL, NULL);
        if (!exec || !name || !*name) continue;
        Entry *e = g_new0(Entry, 1);
        e->exec = g_steal_pointer(&exec);
        e->name = g_strdup(name);
        e->desktop = g_strdup(path);
        g_ptr_array_add(entries, e);
        g_autoptr(GMenuItem) item = g_menu_item_new(name, NULL);
        g_menu_item_set_action_and_target(item, "win.ext-run", "u", entries->len - 1);
        if (submenu && *submenu) {
            GMenu *sm = g_hash_table_lookup(subs, submenu);
            if (!sm) {
                sm = g_menu_new();
                g_hash_table_insert(subs, g_strdup(submenu), sm);
                g_menu_append_submenu(top, submenu, G_MENU_MODEL(sm));
            }
            g_menu_append_item(sm, item);
        } else g_menu_append_item(top, item);
    }
}

static gint cmp_apps(gconstpointer x, gconstpointer y) {
    return g_utf8_collate(g_app_info_get_display_name((GAppInfo *)x), g_app_info_get_display_name((GAppInfo *)y));
}

static gboolean takes_files(GAppInfo *ai) {
    const char *cmd = g_app_info_get_commandline(ai);
    return cmd && (strstr(cmd, "%f") || strstr(cmd, "%F") || strstr(cmd, "%u") || strstr(cmd, "%U"));
}

static gboolean is_editor(GAppInfo *ai) {
    if (!G_IS_DESKTOP_APP_INFO(ai)) return FALSE;
    const char *cats = g_desktop_app_info_get_categories(G_DESKTOP_APP_INFO(ai));
    return cats && (strstr(cats, "TextEditor") || strstr(cats, "IDE"));
}

static void add_app(GMenu *menu, GAppInfo *ai) {
    Entry *e = g_new0(Entry, 1);
    e->app = g_object_ref(ai);
    g_ptr_array_add(entries, e);
    g_autoptr(GMenuItem) item = g_menu_item_new(g_app_info_get_display_name(ai), NULL);
    g_menu_item_set_action_and_target(item, "win.ext-run", "u", entries->len - 1);
    g_menu_append_item(menu, item);
}

GMenuModel *ext_menu_build(App *a, GList *targets) {
    (void)a;
    if (entries) g_ptr_array_unref(entries);
    entries = g_ptr_array_new_with_free_func((GDestroyNotify)entry_free);
    if (!targets) return NULL;

    GPtrArray *cts = g_ptr_array_new_with_free_func(g_free), *dirs = g_ptr_array_new();
    for (GList *l = targets; l; l = l->next) {
        char *ct = content_type_of(l->data);
        g_ptr_array_add(cts, ct ? ct : g_strdup("application/octet-stream"));
        g_ptr_array_add(dirs, GINT_TO_POINTER(ct && g_str_equal(ct, "inode/directory")));
    }

    GMenu *out = g_menu_new();
    GMenu *top = g_menu_new();

    /* Open with: applications associated with the type first, then editors and IDEs (VS Code and the like
     * often don't register every type), then every other application that accepts files */
    if (!targets->next) {
        GList *assoc = g_app_info_get_all_for_type(cts->pdata[0]);
        GHashTable *have = g_hash_table_new(g_str_hash, g_str_equal);
        GMenu *ow = g_menu_new(), *editors = g_menu_new(), *others = g_menu_new();
        for (GList *l = assoc; l; l = l->next) {
            GAppInfo *ai = l->data;
            if (!g_app_info_should_show(ai)) continue;
            g_hash_table_add(have, (gpointer)g_app_info_get_id(ai));
            add_app(ow, ai);
        }
        GList *all = g_app_info_get_all();
        all = g_list_sort(all, cmp_apps);
        for (GList *l = all; l; l = l->next) {
            GAppInfo *ai = l->data;
            const char *id = g_app_info_get_id(ai);
            if (!id || g_str_equal(id, "files4hyprland.desktop") || g_hash_table_contains(have, id) || !g_app_info_should_show(ai) || !takes_files(ai)) continue;
            if (is_editor(ai)) add_app(editors, ai);
            else add_app(others, ai);
        }
        if (g_menu_model_get_n_items(G_MENU_MODEL(editors))) g_menu_append_section(ow, NULL, G_MENU_MODEL(editors));
        if (g_menu_model_get_n_items(G_MENU_MODEL(others))) g_menu_append_submenu(ow, "Other applications", G_MENU_MODEL(others));
        g_list_free_full(all, g_object_unref);
        g_list_free_full(assoc, g_object_unref);
        g_hash_table_unref(have);
        if (g_menu_model_get_n_items(G_MENU_MODEL(ow))) g_menu_append_submenu(top, "Open with", G_MENU_MODEL(ow));
        g_object_unref(ow);
        g_object_unref(editors);
        g_object_unref(others);
    }

    GHashTable *subs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_object_unref);
    GPtrArray *sd = service_dirs();
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < sd->len; i++) {
        GDir *d = g_dir_open(sd->pdata[i], 0, NULL);
        if (!d) continue;
        const char *fn;
        while ((fn = g_dir_read_name(d)))
            if (g_str_has_suffix(fn, ".desktop") && !g_hash_table_contains(seen, fn)) {
                g_hash_table_add(seen, g_strdup(fn));
                g_autofree char *p = g_build_filename(sd->pdata[i], fn, NULL);
                load_service_file(p, targets, cts, dirs, top, subs);
            }
        g_dir_close(d);
    }
    g_hash_table_unref(seen);
    g_hash_table_unref(subs);
    g_ptr_array_unref(sd);
    g_ptr_array_unref(cts);
    g_ptr_array_free(dirs, TRUE);

    if (!g_menu_model_get_n_items(G_MENU_MODEL(top))) {
        g_object_unref(top);
        g_object_unref(out);
        return NULL;
    }
    g_menu_append_section(out, NULL, G_MENU_MODEL(top));
    g_object_unref(top);
    return G_MENU_MODEL(out);
}

/* %f %u: one file per run; %F %U: all files; %c name, %k desktop file, %i/%m dropped. */
static char *file_arg(GFile *f, gboolean uri) {
    char *p = uri ? NULL : g_file_get_path(f);
    return p ? p : g_file_get_uri(f);
}

static void spawn_exec(App *a, Entry *e, GList *files) {
    g_autoptr(GError) err = NULL;
    gint argc;
    g_auto(GStrv) tpl = NULL;
    if (!g_shell_parse_argv(e->exec, &argc, &tpl, &err)) {
        toast(a, "%s", err->message);
        return;
    }
    gboolean per_file = FALSE;
    for (char **t = tpl; *t; t++)
        if (strstr(*t, "%f") || strstr(*t, "%u")) per_file = TRUE;
    g_autofree char *cwd = g_file_get_path(cur_tab(a)->dir);
    for (GList *r = files; r; r = per_file ? r->next : NULL) {
        GPtrArray *argv = g_ptr_array_new_with_free_func(g_free);
        for (char **t = tpl; *t; t++) {
            if (g_str_equal(*t, "%F") || g_str_equal(*t, "%U")) {
                for (GList *l = files; l; l = l->next) g_ptr_array_add(argv, file_arg(l->data, (*t)[1] == 'U'));
                continue;
            }
            GString *s = g_string_new(NULL);
            for (const char *c = *t; *c; c++) {
                if (*c != '%' || !c[1]) { g_string_append_c(s, *c); continue; }
                c++;
                g_autofree char *v = NULL;
                switch (*c) {
                case 'f': case 'u': v = r ? file_arg(r->data, *c == 'u') : NULL; break;
                case 'F': case 'U': v = files ? file_arg(files->data, *c == 'U') : NULL; break;
                case 'c': v = g_strdup(e->name); break;
                case 'k': v = g_strdup(e->desktop); break;
                case '%': v = g_strdup("%"); break;
                default: break;
                }
                if (v) g_string_append(s, v);
            }
            g_ptr_array_add(argv, g_string_free(s, FALSE));
        }
        g_ptr_array_add(argv, NULL);
        if (!g_spawn_async(cwd, (char **)argv->pdata, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err)) {
            toast(a, "%s", err->message);
            g_clear_error(&err);
        }
        g_ptr_array_free(argv, TRUE);
        if (!per_file) break;
    }
}

void ext_run(App *a, guint index) {
    if (!entries || index >= entries->len) return;
    Entry *e = entries->pdata[index];
    GList *files = view_targets(a);
    if (!files) return;
    if (e->app) {
        g_autoptr(GdkAppLaunchContext) ctx = gdk_display_get_app_launch_context(gdk_display_get_default());
        g_autoptr(GError) err = NULL;
        if (!g_app_info_launch(e->app, files, G_APP_LAUNCH_CONTEXT(ctx), &err)) toast(a, "%s", err->message);
    } else {
        spawn_exec(a, e, files);
    }
    g_list_free_full(files, g_object_unref);
}
