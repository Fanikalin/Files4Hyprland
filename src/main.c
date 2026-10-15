/* Files4Hyprland — a light native file manager for Hyprland / Omarchy. */
#include "app.h"
#include <glib-unix.h>
#include <string.h>

static App app;
static char *start_arg;
static char *search_arg;

static gboolean on_signal(gpointer data) {
    g_application_quit(G_APPLICATION(data));
    return G_SOURCE_REMOVE;
}

static gboolean set_renderer;

/* The cairo renderer is plenty for lists and uses ~40% less memory than GL;
 * don't leak the choice to apps we launch. */
static void drop_renderer_env(GtkWidget *w, gpointer d) {
    (void)w; (void)d;
    if (set_renderer) g_unsetenv("GSK_RENDERER");
}

static void on_activate(GtkApplication *gapp, gpointer data) {
    (void)data;
    if (app.win) {
        gtk_window_present(app.win);
        return;
    }
    app.gapp = gapp;
    theme_init(&app);

    g_autoptr(GFile) start = NULL;
    g_autofree char *select = NULL;
    if (start_arg) {
        g_autoptr(GFile) f = g_file_new_for_commandline_arg(start_arg);
        GFileType t = g_file_query_file_type(f, 0, NULL);
        if (t == G_FILE_TYPE_DIRECTORY || (!g_file_is_native(f) && t != G_FILE_TYPE_REGULAR)) start = g_object_ref(f);
        else if (t != G_FILE_TYPE_UNKNOWN) {
            start = g_file_get_parent(f);
            select = g_file_get_basename(f);
        } else if (app.pick == PICK_SAVE) {
            start = g_file_get_parent(f);
            if (!app.pick_name) app.pick_name = g_file_get_basename(f);
        }
    }
    if (!start || g_file_query_file_type(start, 0, NULL) != G_FILE_TYPE_DIRECTORY) {
        g_clear_object(&start);
        start = g_file_new_for_path(g_get_home_dir());
    }
    window_build(&app, start);
    g_signal_connect_after(app.win, "realize", G_CALLBACK(drop_renderer_env), NULL);
    if (select) view_select_name(&app, select, FALSE);
    if (search_arg) gtk_editable_set_text(GTK_EDITABLE(app.search), search_arg);
    gtk_window_present(app.win);
}

int main(int argc, char **argv) {
    const char *pick = NULL;
    gboolean version = FALSE;
    GOptionEntry entries[] = {
        {"pick", 0, 0, G_OPTION_ARG_STRING, &pick, "File chooser mode: file, files, dir or save", "MODE"},
        {"out", 0, 0, G_OPTION_ARG_FILENAME, &app.pick_out, "Write chosen paths here, one per line", "FILE"},
        {"name", 0, 0, G_OPTION_ARG_STRING, &app.pick_name, "Suggested file name for --pick save", "NAME"},
        {"search", 0, 0, G_OPTION_ARG_STRING, &search_arg, "Search this folder and its subfolders", "TEXT"},
        {"version", 0, 0, G_OPTION_ARG_NONE, &version, "Print the version", NULL},
        {NULL, 0, 0, 0, NULL, NULL, NULL},
    };
    g_autoptr(GOptionContext) ctx = g_option_context_new("[PATH]");
    g_option_context_add_main_entries(ctx, entries, NULL);
    g_option_context_set_summary(ctx, "Files4Hyprland: a light file manager for Hyprland/Omarchy.");
    g_autoptr(GError) err = NULL;
    if (!g_option_context_parse(ctx, &argc, &argv, &err)) {
        g_printerr("%s\n", err->message);
        return 2;
    }
    if (version) {
        g_print("files4hyprland %s\n", F4H_VERSION);
        return 0;
    }
    if (pick) {
        if (g_str_equal(pick, "file")) app.pick = PICK_FILE;
        else if (g_str_equal(pick, "files")) app.pick = PICK_FILES;
        else if (g_str_equal(pick, "dir")) app.pick = PICK_DIR;
        else if (g_str_equal(pick, "save")) app.pick = PICK_SAVE;
        else {
            g_printerr("--pick must be file, files, dir or save\n");
            return 2;
        }
    }
    if (argc > 1) start_arg = argv[1];
    if (app.pick_out) g_file_set_contents(app.pick_out, "", 0, NULL); /* empty = cancelled */

    if (!g_getenv("GSK_RENDERER")) {
        g_setenv("GSK_RENDERER", "cairo", TRUE);
        set_renderer = TRUE;
    }
    const char *id = app.pick ? "org.omarchy.files-picker.Chooser" : "io.github.fanikalin.Files4Hyprland";
    g_autoptr(GtkApplication) gapp = gtk_application_new(id, G_APPLICATION_NON_UNIQUE);
    g_signal_connect(gapp, "activate", G_CALLBACK(on_activate), NULL);
    g_unix_signal_add(SIGTERM, on_signal, gapp);
    g_unix_signal_add(SIGHUP, on_signal, gapp);
    g_unix_signal_add(SIGINT, on_signal, gapp);
    char *args[] = {argv[0], NULL};
    return g_application_run(G_APPLICATION(gapp), 1, args);
}
