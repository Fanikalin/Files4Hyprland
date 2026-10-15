/* Sidebar: Places (XDG dirs, GTK bookmarks, Trash) and Devices (GIO volume
 * monitor: mounts, unmounted volumes, mount/unmount/eject). */
#include "app.h"
#include <string.h>

#define G_HOME ""
#define G_DESKTOP ""
#define G_DOCS ""
#define G_DOWN ""
#define G_PICS ""
#define G_MUSIC ""
#define G_VIDEO ""
#define G_TRASH ""
#define G_DISK ""
#define G_USB ""
#define G_MARK ""
#define G_NET ""

static GtkListBoxRow *menu_row;
static guint rebuild_idle;

static GtkWidget *row_new(const char *glyph, const char *name, const char *extra) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *g = gtk_label_new(glyph);
    gtk_widget_set_size_request(g, 18, -1);
    GtkWidget *l = gtk_label_new(name);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(box), g);
    gtk_box_append(GTK_BOX(box), l);
    GtkWidget *x = gtk_label_new(extra ? extra : "");
    gtk_widget_add_css_class(x, "dim");
    gtk_widget_set_hexpand(x, TRUE);
    gtk_label_set_xalign(GTK_LABEL(x), 0);
    gtk_box_append(GTK_BOX(box), x);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    g_object_set_data(G_OBJECT(row), "extra", x);
    return row;
}

static void add_header(App *a, const char *title) {
    GtkWidget *l = gtk_label_new(title);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), l);
    gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
    gtk_widget_add_css_class(row, "header");
    gtk_list_box_append(GTK_LIST_BOX(a->sidebar), row);
}

static void set_size_cb(GObject *src, GAsyncResult *res, gpointer data) {
    GtkWidget *label = data;
    g_autoptr(GFileInfo) fi = g_file_query_filesystem_info_finish(G_FILE(src), res, NULL);
    if (fi && g_file_info_has_attribute(fi, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE)) {
        g_autofree char *s = human_size(g_file_info_get_attribute_uint64(fi, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE));
        gtk_label_set_text(GTK_LABEL(label), s);
    }
    g_object_unref(label);
}

static GFile *row_dest(App *a, GtkWidget *w) {
    (void)a;
    GFile *f = g_object_get_data(G_OBJECT(w), "file");
    return f ? g_object_ref(f) : NULL;
}

static GtkWidget *add_place(App *a, const char *glyph, const char *name, GFile *file, gboolean show_size) {
    GtkWidget *row = row_new(glyph, name, NULL);
    g_object_set_data_full(G_OBJECT(row), "file", g_object_ref(file), g_object_unref);
    gtk_list_box_append(GTK_LIST_BOX(a->sidebar), row);
    add_drop_target(a, row, row_dest);
    if (show_size)
        g_file_query_filesystem_info_async(file, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE, G_PRIORITY_LOW, NULL, set_size_cb,
                                           g_object_ref(g_object_get_data(G_OBJECT(row), "extra")));
    return row;
}

static void add_xdg(App *a, GHashTable *seen, GUserDirectory d, const char *glyph) {
    const char *p = g_get_user_special_dir(d);
    if (!p || g_str_equal(p, g_get_home_dir()) || !g_file_test(p, G_FILE_TEST_IS_DIR)) return;
    g_autoptr(GFile) f = g_file_new_for_path(p);
    g_autofree char *name = g_path_get_basename(p);
    add_place(a, glyph, name, f, FALSE);
    g_hash_table_add(seen, g_strdup(p));
}

static void add_bookmarks(App *a, GHashTable *seen) {
    g_autofree char *path = g_build_filename(g_get_user_config_dir(), "gtk-3.0", "bookmarks", NULL);
    g_autofree char *txt = NULL;
    if (!g_file_get_contents(path, &txt, NULL, NULL)) return;
    g_auto(GStrv) lines = g_strsplit(txt, "\n", -1);
    for (char **l = lines; *l; l++) {
        if (!**l) continue;
        char *sp = strchr(*l, ' ');
        g_autofree char *uri = sp ? g_strndup(*l, sp - *l) : g_strdup(*l);
        g_autoptr(GFile) f = g_file_new_for_uri(uri);
        g_autofree char *p = g_file_get_path(f);
        if (p && (g_hash_table_contains(seen, p) || !g_file_test(p, G_FILE_TEST_IS_DIR))) continue;
        g_autofree char *base = g_file_get_basename(f);
        add_place(a, p ? G_MARK : G_NET, sp ? sp + 1 : base, f, FALSE);
        if (p) g_hash_table_add(seen, g_strdup(p));
    }
}

void sidebar_rebuild(App *a) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(a->sidebar))) gtk_list_box_remove(GTK_LIST_BOX(a->sidebar), child);

    add_header(a, "Places");
    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GFile) home = g_file_new_for_path(g_get_home_dir());
    add_place(a, G_HOME, "Home", home, FALSE);
    g_hash_table_add(seen, g_strdup(g_get_home_dir()));
    add_xdg(a, seen, G_USER_DIRECTORY_DESKTOP, G_DESKTOP);
    add_xdg(a, seen, G_USER_DIRECTORY_DOCUMENTS, G_DOCS);
    add_xdg(a, seen, G_USER_DIRECTORY_DOWNLOAD, G_DOWN);
    add_xdg(a, seen, G_USER_DIRECTORY_PICTURES, G_PICS);
    add_xdg(a, seen, G_USER_DIRECTORY_MUSIC, G_MUSIC);
    add_xdg(a, seen, G_USER_DIRECTORY_VIDEOS, G_VIDEO);
    add_bookmarks(a, seen);
    g_autoptr(GFile) trash = g_file_new_for_uri("trash:///");
    add_place(a, G_TRASH, "Trash", trash, FALSE);

    add_header(a, "Devices");
    g_autoptr(GFile) root = g_file_new_for_path("/");
    add_place(a, G_DISK, "System", root, TRUE);

    GList *mounts = g_volume_monitor_get_mounts(a->vm);
    for (GList *l = mounts; l; l = l->next) {
        GMount *m = l->data;
        if (g_mount_is_shadowed(m)) continue;
        g_autoptr(GFile) mr = g_mount_get_root(m);
        g_autofree char *p = g_file_get_path(mr);
        if (p && g_str_equal(p, "/")) continue;
        g_autofree char *name = g_mount_get_name(m);
        gboolean removable = g_mount_can_eject(m) || (p && (g_str_has_prefix(p, "/run/media") || g_str_has_prefix(p, "/media")));
        GtkWidget *row = add_place(a, !p ? G_NET : removable ? G_USB : G_DISK, name, mr, p != NULL);
        g_object_set_data_full(G_OBJECT(row), "mount", g_object_ref(m), g_object_unref);
    }
    g_list_free_full(mounts, g_object_unref);

    GList *vols = g_volume_monitor_get_volumes(a->vm);
    for (GList *l = vols; l; l = l->next) {
        GVolume *v = l->data;
        g_autoptr(GMount) m = g_volume_get_mount(v);
        if (m || !g_volume_can_mount(v)) continue;
        g_autofree char *name = g_volume_get_name(v);
        GtkWidget *row = row_new(G_DISK, name, "not mounted");
        g_object_set_data_full(G_OBJECT(row), "volume", g_object_ref(v), g_object_unref);
        gtk_list_box_append(GTK_LIST_BOX(a->sidebar), row);
    }
    g_list_free_full(vols, g_object_unref);
    sidebar_mark_current(a);
}

void sidebar_mark_current(App *a) {
    Tab *t = cur_tab(a);
    for (GtkWidget *r = gtk_widget_get_first_child(a->sidebar); r; r = gtk_widget_get_next_sibling(r)) {
        GFile *f = g_object_get_data(G_OBJECT(r), "file");
        if (f && t && g_file_equal(f, t->dir)) gtk_widget_add_css_class(r, "current");
        else gtk_widget_remove_css_class(r, "current");
    }
}

static gboolean rebuild_cb(gpointer data) {
    rebuild_idle = 0;
    sidebar_rebuild(data);
    return G_SOURCE_REMOVE;
}

static void on_vm_changed(GVolumeMonitor *vm, gpointer obj, gpointer data) {
    (void)vm; (void)obj;
    if (!rebuild_idle) rebuild_idle = g_timeout_add(150, rebuild_cb, data);
}

/* ---------------------------------------------------------------- mounting */
typedef struct {
    App *a;
    gboolean new_tab;
} MountReq;

static void mounted_cb(GObject *src, GAsyncResult *res, gpointer data) {
    MountReq *r = data;
    g_autoptr(GError) err = NULL;
    if (!g_volume_mount_finish(G_VOLUME(src), res, &err)) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)) toast(r->a, "Mount failed: %s", err->message);
    } else {
        g_autoptr(GMount) m = g_volume_get_mount(G_VOLUME(src));
        if (m) {
            g_autoptr(GFile) root = g_mount_get_root(m);
            if (r->new_tab) open_in_new_tab(r->a, root);
            else navigate(r->a, root, TRUE, NULL);
        }
    }
    g_free(r);
}

static void open_row(App *a, GtkListBoxRow *row, gboolean new_tab) {
    GFile *f = g_object_get_data(G_OBJECT(row), "file");
    GVolume *v = g_object_get_data(G_OBJECT(row), "volume");
    if (f) {
        if (new_tab) open_in_new_tab(a, f);
        else navigate(a, f, TRUE, NULL);
        gtk_widget_grab_focus(a->view);
    } else if (v) {
        MountReq *r = g_new0(MountReq, 1);
        r->a = a;
        r->new_tab = new_tab;
        g_autoptr(GMountOperation) op = gtk_mount_operation_new(a->win);
        g_volume_mount(v, G_MOUNT_MOUNT_NONE, op, NULL, mounted_cb, r);
    }
}

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    (void)box;
    open_row(data, row, FALSE);
}

static void unmounted_cb(GObject *src, GAsyncResult *res, gpointer data) {
    g_autoptr(GError) err = NULL;
    gboolean ok = g_object_get_data(src, "eject") ? g_mount_eject_with_operation_finish(G_MOUNT(src), res, &err)
                                                  : g_mount_unmount_with_operation_finish(G_MOUNT(src), res, &err);
    if (!ok && !g_error_matches(err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED)) toast(data, "Unmount failed: %s", err->message);
    else if (ok) toast(data, "Unmounted");
}

/* Leave the mount in every tab before unmounting it. */
static void leave_mount(App *a, GFile *root) {
    g_autoptr(GFile) home = g_file_new_for_path(g_get_home_dir());
    for (guint i = 0; i < a->tabs->len; i++) {
        Tab *t = a->tabs->pdata[i];
        if (g_file_equal(t->dir, root) || g_file_has_prefix(t->dir, root)) {
            if (i == a->cur) navigate(a, home, TRUE, NULL);
            else g_set_object(&t->dir, home);
        }
    }
}

static void side_action(GSimpleAction *act, GVariant *p, gpointer data) {
    (void)p;
    App *a = data;
    if (!menu_row) return;
    const char *name = g_action_get_name(G_ACTION(act));
    if (g_str_equal(name, "side-open")) open_row(a, menu_row, FALSE);
    else if (g_str_equal(name, "side-open-tab")) open_row(a, menu_row, TRUE);
    else {
        GMount *m = g_object_get_data(G_OBJECT(menu_row), "mount");
        if (!m) return;
        g_autoptr(GFile) root = g_mount_get_root(m);
        leave_mount(a, root);
        g_autoptr(GMountOperation) op = gtk_mount_operation_new(a->win);
        if (g_str_equal(name, "side-eject")) {
            g_object_set_data(G_OBJECT(m), "eject", GINT_TO_POINTER(1));
            g_mount_eject_with_operation(m, G_MOUNT_UNMOUNT_NONE, op, NULL, unmounted_cb, a);
        } else {
            g_object_set_data(G_OBJECT(m), "eject", NULL);
            g_mount_unmount_with_operation(m, G_MOUNT_UNMOUNT_NONE, op, NULL, unmounted_cb, a);
        }
    }
}

static void on_click(GtkGestureClick *g, int n, double x, double y, gpointer data) {
    (void)n;
    App *a = data;
    guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    GtkListBoxRow *row = gtk_list_box_get_row_at_y(GTK_LIST_BOX(a->sidebar), (int)y);
    if (button == GDK_BUTTON_PRIMARY || !row || !gtk_list_box_row_get_activatable(row)) return;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    if (button == GDK_BUTTON_MIDDLE) {
        open_row(a, row, TRUE);
        return;
    }
    menu_row = row;
    g_autoptr(GMenu) menu = g_menu_new();
    g_menu_append(menu, "Open", "win.side-open");
    g_menu_append(menu, "Open in new tab", "win.side-open-tab");
    GMount *m = g_object_get_data(G_OBJECT(row), "mount");
    if (m && g_mount_can_unmount(m)) g_menu_append(menu, "Unmount", "win.side-unmount");
    if (m && g_mount_can_eject(m)) g_menu_append(menu, "Eject", "win.side-eject");
    popup_menu(a, G_MENU_MODEL(menu), a->sidebar, x, y);
}

GtkWidget *sidebar_new(App *a) {
    a->sidebar = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(a->sidebar), GTK_SELECTION_NONE);
    g_signal_connect(a->sidebar, "row-activated", G_CALLBACK(on_row_activated), a);

    GtkGesture *g = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), 0);
    g_signal_connect(g, "pressed", G_CALLBACK(on_click), a);
    /* only right/middle buttons; left is handled by row activation */
    gtk_gesture_single_set_exclusive(GTK_GESTURE_SINGLE(g), FALSE);
    gtk_widget_add_controller(a->sidebar, GTK_EVENT_CONTROLLER(g));

    static const GActionEntry entries[] = {
        {"side-open", side_action, NULL, NULL, NULL, {0}},
        {"side-open-tab", side_action, NULL, NULL, NULL, {0}},
        {"side-unmount", side_action, NULL, NULL, NULL, {0}},
        {"side-eject", side_action, NULL, NULL, NULL, {0}},
    };
    g_action_map_add_action_entries(G_ACTION_MAP(a->win), entries, G_N_ELEMENTS(entries), a);

    a->vm = g_volume_monitor_get();
    const char *sigs[] = {"mount-added", "mount-removed", "mount-changed", "volume-added", "volume-removed", "volume-changed"};
    for (guint i = 0; i < G_N_ELEMENTS(sigs); i++) g_signal_connect(a->vm, sigs[i], G_CALLBACK(on_vm_changed), a);

    sidebar_rebuild(a);
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), a->sidebar);
    gtk_widget_set_vexpand(sw, TRUE);
    return sw;
}
