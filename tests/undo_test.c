/* Drives the real window through move / trash / create and their undo/redo,
 * checking the filesystem after each step. Run: build/undo_test DIR */
#include "../src/app.h"
#include <stdio.h>

static App app;
static const char *root;
static int step, fails;

static gboolean has(const char *rel) {
    g_autofree char *p = g_build_filename(root, rel, NULL);
    return g_file_test(p, G_FILE_TEST_EXISTS);
}

static void check(const char *what, gboolean ok) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    fails += !ok;
}

static void act(const char *name) { g_action_group_activate_action(G_ACTION_GROUP(app.win), name, NULL); }

static GList *one(const char *rel) {
    g_autofree char *p = g_build_filename(root, rel, NULL);
    return g_list_prepend(NULL, g_file_new_for_path(p));
}

static gboolean tick(gpointer data) {
    (void)data;
    g_autofree char *sub = g_build_filename(root, "sub", NULL);
    g_autoptr(GFile) subf = g_file_new_for_path(sub);
    GList *l;
    switch (step++) {
    case 0: l = one("a.txt"); transfer_files(&app, l, subf, FALSE); g_list_free_full(l, g_object_unref); break;
    case 1: check("move a.txt -> sub/", has("sub/a.txt") && !has("a.txt")); act("undo"); break;
    case 2: check("undo move", has("a.txt") && !has("sub/a.txt")); act("redo"); break;
    case 3: check("redo move", has("sub/a.txt") && !has("a.txt")); l = one("sub/a.txt"); trash_files(&app, l); g_list_free_full(l, g_object_unref); break;
    case 4: check("trash sub/a.txt", !has("sub/a.txt")); act("undo"); break;
    case 5: check("undo trash (restore)", has("sub/a.txt")); act("new-folder"); break;
    case 6: check("new folder", has("New folder")); act("undo"); break;
    case 7: check("undo new folder", !has("New folder")); act("redo"); break;
    case 8: check("redo new folder", has("New folder")); act("undo"); break;
    case 9: check("undo again", !has("New folder")); act("undo"); break; /* undo the trash-restore? stack: trash undone -> redo; so this undoes the move */
    case 10: check("undo move again (Ctrl+Z twice)", has("a.txt") && !has("sub/a.txt")); act("redo"); break;
    case 11: check("redo after second undo", has("sub/a.txt")); break;
    default:
        printf("%d failure(s)\n", fails);
        g_application_quit(G_APPLICATION(app.gapp));
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void on_activate(GtkApplication *gapp, gpointer d) {
    (void)d;
    app.gapp = gapp;
    theme_init(&app);
    g_autoptr(GFile) start = g_file_new_for_path(root);
    window_build(&app, start);
    gtk_window_present(app.win);
    g_timeout_add(900, tick, NULL);
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    root = argv[1];
    g_setenv("GSK_RENDERER", "cairo", TRUE);
    g_autoptr(GtkApplication) gapp = gtk_application_new("io.github.fanikalin.Files4Hyprland.Test", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(gapp, "activate", G_CALLBACK(on_activate), NULL);
    char *args[] = {argv[0], NULL};
    g_application_run(G_APPLICATION(gapp), 1, args);
    return fails ? 1 : 0;
}
