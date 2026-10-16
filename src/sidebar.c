/* Sidebar: Places (XDG dirs, GTK bookmarks, Trash) and Devices (GIO volume
 * monitor: mounts, unmounted volumes, mount/unmount/eject). */
#include "app.h"
#include <glib/gstdio.h>
#include <math.h>
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
static void add_pins(App *a);
static guint rebuild_idle;

/* ---------------------------------------------------------------- disk colors */
/* A disk's color depends only on its name, so it stays the same on any machine.
 * The system disk takes the theme's second color. */
static guint32 name_hash(const char *s) {
    guint32 h = 2166136261u;
    for (; *s; s++) h = (h ^ (guchar)*s) * 16777619u;
    h ^= h >> 15; h *= 2246822519u; h ^= h >> 13;
    return h;
}

static void hsl_to_rgb(double h, double s, double l, double *r, double *g, double *b) {
    double c = (1 - fabs(2 * l - 1)) * s, hp = h / 60.0, x = c * (1 - fabs(fmod(hp, 2) - 1)), m = l - c / 2;
    double R = 0, G = 0, B = 0;
    if (hp < 1) { R = c; G = x; } else if (hp < 2) { R = x; G = c; } else if (hp < 3) { G = c; B = x; }
    else if (hp < 4) { G = x; B = c; } else if (hp < 5) { R = x; B = c; } else { R = c; B = x; }
    *r = R + m; *g = G + m; *b = B + m;
}

static double hue_of(const GdkRGBA *c, double *sat) {
    double mx = MAX(c->red, MAX(c->green, c->blue)), mn = MIN(c->red, MIN(c->green, c->blue)), d = mx - mn, h = 0;
    *sat = mx > 0 ? d / mx : 0;
    if (d == 0) return 0;
    if (mx == c->red) h = fmod((c->green - c->blue) / d, 6);
    else if (mx == c->green) h = (c->blue - c->red) / d + 2;
    else h = (c->red - c->green) / d + 4;
    h *= 60;
    return h < 0 ? h + 360 : h;
}

/* 20 hues 18° apart x 2 lightness levels; keeps clear of the system disk's hue */
static char *disk_color(App *a, const char *name, gboolean system) {
    GdkRGBA sys;
    if (!gdk_rgba_parse(&sys, a->accent2)) gdk_rgba_parse(&sys, "#8d8d8d");
    if (system) return g_strdup_printf("#%02x%02x%02x", (int)(sys.red * 255 + 0.5), (int)(sys.green * 255 + 0.5), (int)(sys.blue * 255 + 0.5));
    guint32 h = name_hash(name);
    guint idx = h % 20;
    double sysat, syshue = hue_of(&sys, &sysat);
    if (sysat > 0.25) {
        double d = fabs(fmod(idx * 18.0 - syshue + 540, 360) - 180);
        if (d < 14) idx = (idx + 1) % 20;
    }
    double l = ((h >> 8) & 1) ? 0.72 : 0.56, r, g, b;
    hsl_to_rgb(idx * 18.0, 0.72, l, &r, &g, &b);
    return g_strdup_printf("#%02x%02x%02x", (int)(r * 255 + 0.5), (int)(g * 255 + 0.5), (int)(b * 255 + 0.5));
}

static GtkCssProvider *dcss;
static GHashTable *dc_seen; /* hex -> 1 */

static void dcss_reload(App *a) {
    if (!dcss) {
        dcss = gtk_css_provider_new();
        gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(dcss),
                                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    }
    GString *c = g_string_new(NULL);
    GHashTableIter it;
    gpointer k;
    g_hash_table_iter_init(&it, dc_seen);
    while (g_hash_table_iter_next(&it, &k, NULL)) {
        const char *hex = k;
        g_string_append_printf(c,
            "list row.dc-%1$s, list row.dc-%1$s.current { box-shadow: inset 3px 0 #%1$s; }\n"
            "list row.dc-%1$s:hover { box-shadow: inset 3px 0 #%1$s, inset 0 0 0 1px alpha(#%1$s, 0.35); }\n"
            "list row.dc-%1$s progressbar.disk progress { background: #%1$s; }\n"
            "list row.dc-%1$s.low progressbar.disk progress { background: %2$s; }\n",
            hex, a->red[0] ? a->red : "#e5484d");
    }
    gtk_css_provider_load_from_string(dcss, c->str);
    g_string_free(c, TRUE);
}

/* the theme changed: recolor (the system disk's color and the "low space" red come from it) */
void sidebar_style(App *a) {
    if (dc_seen) dcss_reload(a);
}

static void style_disk(App *a, GtkWidget *row, const char *name, gboolean system) {
    if (!dc_seen) dc_seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autofree char *hex = disk_color(a, name, system);
    const char *h = hex + 1;
    if (!g_hash_table_contains(dc_seen, h)) {
        g_hash_table_add(dc_seen, g_strdup(h));
        dcss_reload(a);
    }
    g_autofree char *cls = g_strdup_printf("dc-%s", h);
    gtk_widget_add_css_class(row, cls);
}

static gboolean is_phone(GFile *f) { return g_file_has_uri_scheme(f, "mtp") || g_file_has_uri_scheme(f, "gphoto2"); }

/* first phone is "Phone", the next ones "Phone 1", "Phone 2" */
static char *phone_name(guint n) { return n ? g_strdup_printf("%s %u", tr("Phone"), n) : g_strdup(tr("Phone")); }

static GtkWidget *row_new(const char *glyph, const char *name, const char *extra) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *g = gtk_label_new(glyph);
    gtk_widget_set_size_request(g, 18, -1);
    GtkWidget *l = gtk_label_new(name);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_END);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_box_append(GTK_BOX(box), g);
    gtk_box_append(GTK_BOX(box), l);
    /* "used / total" or the state, on its own line under the name */
    GtkWidget *x = gtk_label_new(extra ? extra : "");
    gtk_widget_add_css_class(x, "dim");
    gtk_widget_add_css_class(x, "disk-info");
    gtk_label_set_xalign(GTK_LABEL(x), 0);
    gtk_widget_set_visible(x, extra != NULL);
    GtkWidget *bar = gtk_progress_bar_new();
    gtk_widget_add_css_class(bar, "disk");
    gtk_widget_set_visible(bar, FALSE);
    GtkWidget *col = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_box_append(GTK_BOX(col), box);
    gtk_box_append(GTK_BOX(col), x);
    gtk_box_append(GTK_BOX(col), bar);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), col);
    g_object_set_data(G_OBJECT(row), "extra", x);
    g_object_set_data(G_OBJECT(row), "bar", bar);
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

/* "used / total" under the disk name and a bar from blue (empty) to red (full) */
static void set_size_cb(GObject *src, GAsyncResult *res, gpointer data) {
    GtkWidget *row = data;
    g_autoptr(GFileInfo) fi = g_file_query_filesystem_info_finish(G_FILE(src), res, NULL);
    if (fi && g_file_info_has_attribute(fi, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE) &&
        g_file_info_has_attribute(fi, G_FILE_ATTRIBUTE_FILESYSTEM_FREE)) {
        guint64 total = g_file_info_get_attribute_uint64(fi, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE);
        guint64 free = g_file_info_get_attribute_uint64(fi, G_FILE_ATTRIBUTE_FILESYSTEM_FREE);
        guint64 used = total > free ? total - free : 0;
        if (total) {
            g_autofree char *u = human_size(used), *t = human_size(total);
            g_autofree char *txt = g_strdup_printf("%s / %s", u, t);
            GtkWidget *info = g_object_get_data(G_OBJECT(row), "extra");
            gtk_label_set_text(GTK_LABEL(info), txt);
            gtk_widget_set_visible(info, TRUE);
            GtkWidget *bar = g_object_get_data(G_OBJECT(row), "bar");
            double frac = (double)used / (double)total;
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(bar), frac);
            /* under 10% free the bar turns red (CSS .low on the row) */
            GtkWidget *rowbox = GTK_WIDGET(row);
            if ((double)free / (double)total < 0.10) gtk_widget_add_css_class(rowbox, "low");
            else gtk_widget_remove_css_class(rowbox, "low");
            gtk_widget_set_visible(bar, TRUE);
        }
    }
    g_object_unref(row);
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
        g_file_query_filesystem_info_async(file, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE "," G_FILE_ATTRIBUTE_FILESYSTEM_FREE,
                                           G_PRIORITY_LOW, NULL, set_size_cb, g_object_ref(row));
    return row;
}

static void add_xdg(App *a, GHashTable *seen, GUserDirectory d, const char *glyph) {
    const char *p = g_get_user_special_dir(d);
    if (!p || g_str_equal(p, g_get_home_dir()) || !g_file_test(p, G_FILE_TEST_IS_DIR)) return;
    g_autoptr(GFile) f = g_file_new_for_path(p);
    g_autofree char *name = dir_display_name(f);
    if (!name) name = g_path_get_basename(p);
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


/* ---------------------------------------------------------------- pinned files and folders */
static GPtrArray *pins; /* URIs */

static char *pins_path(void) { return g_build_filename(g_get_user_config_dir(), "files4hyprland", "pins", NULL); }

static void pins_load(void) {
    if (pins) return;
    pins = g_ptr_array_new_with_free_func(g_free);
    g_autofree char *txt = NULL;
    g_autofree char *p = pins_path();
    if (!g_file_get_contents(p, &txt, NULL, NULL)) return;
    g_auto(GStrv) lines = g_strsplit(txt, "\n", -1);
    for (char **l = lines; *l; l++)
        if (**l) g_ptr_array_add(pins, g_strdup(*l));
}

static void pins_save(void) {
    g_autofree char *p = pins_path();
    g_autofree char *dir = g_path_get_dirname(p);
    g_mkdir_with_parents(dir, 0755);
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < pins->len; i++) g_string_append_printf(s, "%s\n", (char *)pins->pdata[i]);
    g_file_set_contents(p, s->str, -1, NULL);
    g_string_free(s, TRUE);
}

static int pin_index(GFile *f) {
    pins_load();
    g_autofree char *uri = g_file_get_uri(f);
    for (guint i = 0; i < pins->len; i++)
        if (g_str_equal(pins->pdata[i], uri)) return i;
    return -1;
}

gboolean sidebar_is_pinned(GFile *f) { return pin_index(f) >= 0; }

void sidebar_pin(App *a, GFile *f) {
    if (pin_index(f) >= 0) return;
    g_ptr_array_add(pins, g_file_get_uri(f));
    pins_save();
    sidebar_rebuild(a);
}

void sidebar_unpin(App *a, GFile *f) {
    int i = pin_index(f);
    if (i < 0) return;
    g_ptr_array_remove_index(pins, i);
    pins_save();
    sidebar_rebuild(a);
}

static void add_pins(App *a) {
    pins_load();
    gboolean header = FALSE;
    for (guint i = 0; i < pins->len; i++) {
        g_autoptr(GFile) f = g_file_new_for_uri(pins->pdata[i]);
        GFileType t = g_file_query_file_type(f, 0, NULL);
        if (t == G_FILE_TYPE_UNKNOWN && g_file_is_native(f)) continue; /* gone; stays in the list for when it comes back */
        if (!header) {
            add_header(a, tr("Pinned"));
            header = TRUE;
        }
        g_autofree char *name = dir_display_name(f);
        if (!name) name = g_file_get_basename(f);
        GtkWidget *row;
        if (t == G_FILE_TYPE_DIRECTORY || !g_file_is_native(f)) {
            row = add_place(a, G_MARK, name, f, FALSE);
        } else {
            row = row_new("", name, NULL);
            g_object_set_data_full(G_OBJECT(row), "file", g_object_ref(f), g_object_unref);
            g_object_set_data(G_OBJECT(row), "pinfile", GINT_TO_POINTER(1));
            gtk_list_box_append(GTK_LIST_BOX(a->sidebar), row);
        }
        g_object_set_data(G_OBJECT(row), "pinned", GINT_TO_POINTER(1));
    }
}

void sidebar_rebuild(App *a) {
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(a->sidebar))) gtk_list_box_remove(GTK_LIST_BOX(a->sidebar), child);

    add_header(a, tr("Places"));
    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_autoptr(GFile) home = g_file_new_for_path(g_get_home_dir());
    add_place(a, G_HOME, tr("Home"), home, FALSE);
    g_hash_table_add(seen, g_strdup(g_get_home_dir()));
    add_xdg(a, seen, G_USER_DIRECTORY_DESKTOP, G_DESKTOP);
    add_xdg(a, seen, G_USER_DIRECTORY_DOCUMENTS, G_DOCS);
    add_xdg(a, seen, G_USER_DIRECTORY_DOWNLOAD, G_DOWN);
    add_xdg(a, seen, G_USER_DIRECTORY_PICTURES, G_PICS);
    add_xdg(a, seen, G_USER_DIRECTORY_MUSIC, G_MUSIC);
    add_xdg(a, seen, G_USER_DIRECTORY_VIDEOS, G_VIDEO);
    add_bookmarks(a, seen);
    g_autoptr(GFile) trash = g_file_new_for_uri("trash:///");
    add_place(a, G_TRASH, tr("Trash"), trash, FALSE);
    GtkWidget *netrow = row_new(G_NET, tr("Network"), NULL);
    g_object_set_data_full(G_OBJECT(netrow), "file", g_file_new_for_uri("network:///"), g_object_unref);
    gtk_list_box_append(GTK_LIST_BOX(a->sidebar), netrow);
    add_pins(a);

    add_header(a, tr("Devices"));
    g_autoptr(GFile) root = g_file_new_for_path("/");
    GtkWidget *sysrow = add_place(a, G_DISK, tr("System"), root, TRUE);
    style_disk(a, sysrow, "System", TRUE);

    /* phones (MTP / PTP) are shown as "Phone", "Phone 1", ... instead of the cryptic device name */
    guint phones = 0;
    g_autoptr(GHashTable) phone_uris = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GList *mounts = g_volume_monitor_get_mounts(a->vm);
    for (GList *l = mounts; l; l = l->next) {
        GMount *m = l->data;
        if (g_mount_is_shadowed(m)) continue;
        g_autoptr(GFile) mr = g_mount_get_root(m);
        g_autofree char *p = g_file_get_path(mr);
        if (p && g_str_equal(p, "/")) continue;
        g_autofree char *name = g_mount_get_name(m);
        g_autofree char *shown = NULL;
        if (is_phone(mr)) {
            g_autofree char *uri = g_file_get_uri(mr);
            if (g_hash_table_contains(phone_uris, uri)) continue;
            g_hash_table_add(phone_uris, g_steal_pointer(&uri));
            shown = phone_name(phones++);
        }
        gboolean removable = g_mount_can_eject(m) || (p && (g_str_has_prefix(p, "/run/media") || g_str_has_prefix(p, "/media")));
        GtkWidget *row = add_place(a, shown ? G_USB : !p ? G_NET : removable ? G_USB : G_DISK, shown ? shown : name, mr, p != NULL);
        style_disk(a, row, name, FALSE);
        g_object_set_data_full(G_OBJECT(row), "mount", g_object_ref(m), g_object_unref);
    }
    g_list_free_full(mounts, g_object_unref);

    GList *vols = g_volume_monitor_get_volumes(a->vm);
    for (GList *l = vols; l; l = l->next) {
        GVolume *v = l->data;
        g_autoptr(GMount) m = g_volume_get_mount(v);
        if (m || !g_volume_can_mount(v)) continue;
        g_autofree char *name = g_volume_get_name(v);
        g_autoptr(GFile) act = g_volume_get_activation_root(v);
        g_autofree char *shown = act && is_phone(act) ? phone_name(phones++) : NULL;
        GtkWidget *row = row_new(shown ? G_USB : G_DISK, shown ? shown : name, tr("not mounted"));
        style_disk(a, row, name, FALSE);
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
        if (f && t && !g_object_get_data(G_OBJECT(r), "pinfile") && g_file_equal(f, t->dir)) gtk_widget_add_css_class(r, "current");
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
    if (f && g_object_get_data(G_OBJECT(row), "pinfile")) {
        open_file(a, f);
    } else if (f) {
        if (new_tab) open_in_new_tab(a, f);
        else navigate(a, f, TRUE, NULL);
        view_focus(a);
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
    if (g_str_equal(name, "side-unpin")) {
        GFile *f = g_object_get_data(G_OBJECT(menu_row), "file");
        if (f) sidebar_unpin(a, f);
    } else if (g_str_equal(name, "side-open")) open_row(a, menu_row, FALSE);
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
    if (!g_object_get_data(G_OBJECT(row), "pinfile")) g_menu_append(menu, "Open in new tab", "win.side-open-tab");
    if (g_object_get_data(G_OBJECT(row), "pinned")) g_menu_append(menu, "Unpin", "win.side-unpin");
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
        {"side-unpin", side_action, NULL, NULL, NULL, {0}},
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
