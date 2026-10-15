/* Window: tabs, location bar (back/forward/up, breadcrumbs, search), icon
 * toolbar, sidebar + file list panels, status line, actions and shortcuts,
 * clipboard (interoperable with other file managers) and chooser mode. */
#include "app.h"
#include <string.h>

Tab *cur_tab(App *a) { return a->tabs && a->cur < a->tabs->len ? a->tabs->pdata[a->cur] : NULL; }

static Tab *tab_new(GFile *dir) {
    Tab *t = g_new0(Tab, 1);
    t->dir = g_object_ref(dir);
    t->back = g_ptr_array_new_with_free_func(g_object_unref);
    t->fwd = g_ptr_array_new_with_free_func(g_object_unref);
    return t;
}

static void tab_free(Tab *t) {
    g_object_unref(t->dir);
    g_ptr_array_unref(t->back);
    g_ptr_array_unref(t->fwd);
    g_free(t);
}

static void action_enable(App *a, const char *name, gboolean on) {
    GAction *act = g_action_map_lookup_action(G_ACTION_MAP(a->win), name);
    if (act) g_simple_action_set_enabled(G_SIMPLE_ACTION(act), on);
}

/* ---------------------------------------------------------------- popup menus */
static gboolean unparent_idle(gpointer w) {
    gtk_widget_unparent(w);
    g_object_unref(w);
    return G_SOURCE_REMOVE;
}

static void on_popover_closed(GtkPopover *p, gpointer d) {
    (void)d;
    g_idle_add(unparent_idle, g_object_ref(p));
}

void popup_menu(App *a, GMenuModel *model, GtkWidget *rel, double x, double y) {
    (void)a;
    GtkWidget *pop = gtk_popover_menu_new_from_model(model);
    gtk_widget_set_parent(pop, rel);
    gtk_popover_set_has_arrow(GTK_POPOVER(pop), FALSE);
    gtk_popover_set_position(GTK_POPOVER(pop), GTK_POS_BOTTOM);
    gtk_widget_set_halign(pop, GTK_ALIGN_START);
    GdkRectangle r = {(int)x, (int)y, 1, 1};
    gtk_popover_set_pointing_to(GTK_POPOVER(pop), &r);
    g_signal_connect(pop, "closed", G_CALLBACK(on_popover_closed), NULL);
    gtk_popover_popup(GTK_POPOVER(pop));
}

/* ---------------------------------------------------------------- status */
static void fs_cb(GObject *src, GAsyncResult *res, gpointer data) {
    App *a = data;
    g_autoptr(GFileInfo) fi = g_file_query_filesystem_info_finish(G_FILE(src), res, NULL);
    g_clear_pointer(&a->fs_info, g_free);
    if (fi && g_file_info_has_attribute(fi, G_FILE_ATTRIBUTE_FILESYSTEM_FREE)) {
        g_autofree char *fr = human_size(g_file_info_get_attribute_uint64(fi, G_FILE_ATTRIBUTE_FILESYSTEM_FREE));
        g_autofree char *tot = human_size(g_file_info_get_attribute_uint64(fi, G_FILE_ATTRIBUTE_FILESYSTEM_SIZE));
        a->fs_info = g_strdup_printf("%s free of %s", fr, tot);
    }
    queue_status(a);
}

static gboolean status_cb(gpointer data) {
    App *a = data;
    a->status_idle = 0;
    GListModel *m = G_LIST_MODEL(a->sorted);
    guint n = g_list_model_get_n_items(m), dirs = 0;
    for (guint i = 0; i < n; i++) {
        g_autoptr(GFileInfo) fi = g_list_model_get_item(m, i);
        dirs += info_is_dir(fi);
    }
    g_autoptr(GtkBitset) set = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(a->sel));
    guint nsel = gtk_bitset_get_size(set);
    GString *s = g_string_new(NULL);
    g_string_append_printf(s, "%u items (%u folders)", n, dirs);
    if (nsel) {
        guint64 total = 0;
        GtkBitsetIter it;
        guint pos;
        for (gboolean ok = gtk_bitset_iter_init_first(&it, set, &pos); ok; ok = gtk_bitset_iter_next(&it, &pos)) {
            g_autoptr(GFileInfo) fi = info_at(a, pos);
            if (fi && !info_is_dir(fi)) total += g_file_info_get_size(fi);
        }
        g_autofree char *h = human_size(total);
        g_string_append_printf(s, "  ·  %u selected (%s)", nsel, h);
    }
    if (a->clip) g_string_append_printf(s, "  ·  %u %s", g_list_length(a->clip), a->clip_cut ? "cut" : "copied");
    if (a->fs_info) g_string_append_printf(s, "  ·  %s", a->fs_info);
    gtk_label_set_text(GTK_LABEL(a->status), s->str);
    g_string_free(s, TRUE);

    const char *needs_sel[] = {"open", "open-tab", "cut", "copy", "rename", "trash", "copy-path"};
    for (guint i = 0; i < G_N_ELEMENTS(needs_sel); i++) action_enable(a, needs_sel[i], nsel > 0);
    return G_SOURCE_REMOVE;
}

void queue_status(App *a) {
    if (!a->status_idle) a->status_idle = g_idle_add(status_cb, a);
}

/* ---------------------------------------------------------------- tabs bar */
static void render_tabs(App *a);

static void switch_tab(App *a, guint i) {
    if (i >= a->tabs->len || i == a->cur) return;
    a->cur = i;
    navigate(a, cur_tab(a)->dir, FALSE, NULL);
}

static void close_tab(App *a, guint i) {
    if (a->tabs->len <= 1) {
        gtk_window_close(a->win);
        return;
    }
    tab_free(g_ptr_array_steal_index(a->tabs, i));
    if (a->cur > i || a->cur >= a->tabs->len) a->cur = a->cur ? a->cur - 1 : 0;
    navigate(a, cur_tab(a)->dir, FALSE, NULL);
}

void open_in_new_tab(App *a, GFile *dir) {
    g_ptr_array_insert(a->tabs, a->cur + 1, tab_new(dir));
    a->cur++;
    navigate(a, dir, FALSE, NULL);
}

static void on_tab_clicked(GtkButton *b, gpointer data) {
    switch_tab(data, GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "idx")));
}

static void on_tab_close(GtkButton *b, gpointer data) {
    close_tab(data, GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(b), "idx")));
}

static void on_tab_middle(GtkGestureClick *g, int n, double x, double y, gpointer data) {
    (void)n; (void)x; (void)y;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    close_tab(data, GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "idx")));
}

static GFile *tab_dest(App *a, GtkWidget *w) {
    guint i = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(w), "idx"));
    return i < a->tabs->len ? g_object_ref(((Tab *)a->tabs->pdata[i])->dir) : NULL;
}

static char *dir_label(GFile *f) {
    g_autofree char *path = g_file_get_path(f);
    if (path && g_str_equal(path, g_get_home_dir())) return g_strdup("Home");
    if (g_file_has_uri_scheme(f, "trash")) return g_strdup("Trash");
    g_autofree char *base = g_file_get_basename(f);
    return g_strdup(base ? base : "/");
}

static void render_tabs(App *a) {
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(a->tabbar))) gtk_box_remove(GTK_BOX(a->tabbar), c);
    for (guint i = 0; i < a->tabs->len; i++) {
        Tab *t = a->tabs->pdata[i];
        g_autofree char *name = dir_label(t->dir);
        g_autofree char *label = g_strdup_printf(" %s", name);
        GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(box, "tab");
        if (i == a->cur) gtk_widget_add_css_class(box, "active");
        GtkWidget *b = gtk_button_new_with_label(label);
        GtkWidget *x = gtk_button_new_with_label("×");
        gtk_widget_set_tooltip_text(x, "Close tab (Ctrl+W)");
        for (GtkWidget *w = box; w; w = w == box ? b : w == b ? x : NULL)
            g_object_set_data(G_OBJECT(w), "idx", GUINT_TO_POINTER(i));
        g_signal_connect(b, "clicked", G_CALLBACK(on_tab_clicked), a);
        g_signal_connect(x, "clicked", G_CALLBACK(on_tab_close), a);
        gtk_box_append(GTK_BOX(box), b);
        gtk_box_append(GTK_BOX(box), x);
        GtkGesture *g = gtk_gesture_click_new();
        gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), GDK_BUTTON_MIDDLE);
        g_signal_connect(g, "pressed", G_CALLBACK(on_tab_middle), a);
        gtk_widget_add_controller(box, GTK_EVENT_CONTROLLER(g));
        add_drop_target(a, box, tab_dest);
        gtk_box_append(GTK_BOX(a->tabbar), box);
    }
    GtkWidget *plus = gtk_button_new_with_label("+");
    gtk_widget_add_css_class(plus, "glyph");
    gtk_widget_set_tooltip_text(plus, "New tab (Ctrl+T)");
    gtk_actionable_set_action_name(GTK_ACTIONABLE(plus), "win.new-tab");
    gtk_box_append(GTK_BOX(a->tabbar), plus);
}

/* ---------------------------------------------------------------- breadcrumbs */
static void on_crumb(GtkButton *b, gpointer data) {
    navigate(data, g_object_get_data(G_OBJECT(b), "file"), TRUE, NULL);
}

static GFile *crumb_dest(App *a, GtkWidget *w) {
    (void)a;
    GFile *f = g_object_get_data(G_OBJECT(w), "file");
    return f ? g_object_ref(f) : NULL;
}

static gboolean scroll_end(gpointer data) {
    GtkAdjustment *adj = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(data));
    gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj));
    return G_SOURCE_REMOVE;
}

static void render_crumbs(App *a) {
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(a->crumbs))) gtk_box_remove(GTK_BOX(a->crumbs), c);
    GPtrArray *chain = g_ptr_array_new_with_free_func(g_object_unref);
    for (GFile *f = g_object_ref(cur_tab(a)->dir); f;) {
        g_ptr_array_insert(chain, 0, f);
        f = g_file_get_parent(f);
    }
    for (guint i = 0; i < chain->len; i++) {
        GFile *f = chain->pdata[i];
        g_autofree char *base = g_file_get_basename(f);
        const char *label = base;
        g_autofree char *alt = NULL;
        if (i == 0 && !g_file_is_native(f)) label = alt = dir_label(f);
        if (i) {
            GtkWidget *sep = gtk_label_new("›");
            gtk_widget_add_css_class(sep, "sep");
            gtk_box_append(GTK_BOX(a->crumbs), sep);
        }
        GtkWidget *b = gtk_button_new_with_label(label && *label ? label : "/");
        gtk_widget_add_css_class(b, "crumb");
        if (i == chain->len - 1) gtk_widget_add_css_class(b, "last");
        g_object_set_data_full(G_OBJECT(b), "file", g_object_ref(f), g_object_unref);
        g_signal_connect(b, "clicked", G_CALLBACK(on_crumb), a);
        add_drop_target(a, b, crumb_dest);
        gtk_box_append(GTK_BOX(a->crumbs), b);
    }
    g_ptr_array_unref(chain);
    g_idle_add(scroll_end, a->crumbs_sw);
}

static void show_address(App *a, gboolean on) {
    gtk_stack_set_visible_child_name(GTK_STACK(a->loc_stack), on ? "addr" : "crumbs");
    if (on) {
        g_autofree char *p = g_file_get_parse_name(cur_tab(a)->dir);
        gtk_editable_set_text(GTK_EDITABLE(a->addr), p);
        gtk_widget_grab_focus(a->addr);
        gtk_editable_select_region(GTK_EDITABLE(a->addr), 0, -1);
    }
}

static void on_addr_activate(GtkEntry *e, gpointer data) {
    App *a = data;
    const char *txt = gtk_editable_get_text(GTK_EDITABLE(e));
    g_autofree char *exp = NULL;
    if (txt[0] == '~') exp = g_strconcat(g_get_home_dir(), txt + 1, NULL);
    else exp = g_strdup(txt);
    g_autoptr(GFile) f = NULL;
    if (exp[0] == '/' || strstr(exp, "://")) f = g_file_parse_name(exp);
    else f = g_file_resolve_relative_path(cur_tab(a)->dir, exp);
    show_address(a, FALSE);
    gtk_widget_grab_focus(a->view);
    GFileType t = g_file_query_file_type(f, 0, NULL);
    if (t == G_FILE_TYPE_DIRECTORY || !g_file_is_native(f)) navigate(a, f, TRUE, NULL);
    else if (t != G_FILE_TYPE_UNKNOWN) {
        g_autoptr(GFile) parent = g_file_get_parent(f);
        g_autofree char *name = g_file_get_basename(f);
        navigate(a, parent, TRUE, name);
    } else toast(a, "Not found: %s", exp);
}

static gboolean on_addr_key(GtkEventControllerKey *k, guint kv, guint c, GdkModifierType st, gpointer data) {
    (void)k; (void)c; (void)st;
    if (kv != GDK_KEY_Escape) return FALSE;
    show_address(data, FALSE);
    gtk_widget_grab_focus(((App *)data)->view);
    return TRUE;
}

static void on_addr_leave(GtkEventControllerFocus *f, gpointer data) {
    (void)f;
    show_address(data, FALSE);
}

static void on_crumbs_click(GtkGestureClick *g, int n, double x, double y, gpointer data) {
    (void)g; (void)n; (void)x; (void)y;
    show_address(data, TRUE);
}

/* ---------------------------------------------------------------- navigation */
void refresh_chrome(App *a) {
    Tab *t = cur_tab(a);
    render_tabs(a);
    render_crumbs(a);
    action_enable(a, "back", t->back->len > 0);
    action_enable(a, "forward", t->fwd->len > 0);
    g_autoptr(GFile) parent = g_file_get_parent(t->dir);
    action_enable(a, "up", parent != NULL);
    g_autofree char *pn = g_file_get_parse_name(t->dir);
    const char *pre = a->pick == PICK_FILE ? "Select a file · " : a->pick == PICK_FILES ? "Select files · "
                    : a->pick == PICK_DIR ? "Select a folder · " : a->pick == PICK_SAVE ? "Save as · " : "";
    g_autofree char *title = g_strdup_printf("%s %s", pre, pn);
    gtk_label_set_text(GTK_LABEL(a->main_title), title);
    g_autofree char *wt = dir_label(t->dir);
    gtk_window_set_title(a->win, wt);
    sidebar_mark_current(a);
    queue_status(a);
}

void navigate(App *a, GFile *dir, gboolean push, const char *select) {
    Tab *t = cur_tab(a);
    g_object_ref(dir);
    if (push && !g_file_equal(t->dir, dir)) {
        g_ptr_array_add(t->back, g_object_ref(t->dir));
        g_ptr_array_set_size(t->fwd, 0);
    }
    g_set_object(&t->dir, dir);
    g_object_unref(dir);
    if (a->renaming) gtk_widget_grab_focus(a->view);
    gtk_editable_set_text(GTK_EDITABLE(a->search), "");
    g_free(a->pending_select);
    a->pending_select = g_strdup(select);
    a->pending_rename = FALSE;
    GFile *old = gtk_directory_list_get_file(a->dirlist);
    if (!old || !g_file_equal(old, t->dir)) gtk_directory_list_set_file(a->dirlist, t->dir);
    else view_try_pending(a);
    g_clear_pointer(&a->fs_info, g_free);
    g_file_query_filesystem_info_async(t->dir, "filesystem::free,filesystem::size", G_PRIORITY_LOW, NULL, fs_cb, a);
    refresh_chrome(a);
}

/* ---------------------------------------------------------------- clipboard */
static void clip_set(App *a, GList *files, gboolean cut) {
    g_list_free_full(a->clip, g_object_unref);
    a->clip = files;
    a->clip_cut = cut;
    GString *gnome = g_string_new(cut ? "cut" : "copy");
    GString *text = g_string_new(NULL);
    GSList *sl = NULL;
    for (GList *l = files; l; l = l->next) {
        g_autofree char *uri = g_file_get_uri(l->data);
        g_autofree char *pn = g_file_get_parse_name(l->data);
        g_string_append_printf(gnome, "\n%s", uri);
        g_string_append_printf(text, "%s%s", text->len ? "\n" : "", pn);
        sl = g_slist_prepend(sl, l->data);
    }
    sl = g_slist_reverse(sl);
    GdkFileList *fl = gdk_file_list_new_from_list(sl);
    g_slist_free(sl);
    GBytes *gb = g_string_free_to_bytes(gnome);
    GBytes *tb = g_string_free_to_bytes(text);
    GdkContentProvider *ps[] = {
        gdk_content_provider_new_typed(GDK_TYPE_FILE_LIST, fl),
        gdk_content_provider_new_for_bytes("x-special/gnome-copied-files", gb),
        gdk_content_provider_new_for_bytes("text/plain;charset=utf-8", tb),
    };
    g_boxed_free(GDK_TYPE_FILE_LIST, fl);
    g_bytes_unref(gb);
    g_bytes_unref(tb);
    g_autoptr(GdkContentProvider) u = gdk_content_provider_new_union(ps, G_N_ELEMENTS(ps));
    gdk_clipboard_set_content(gtk_widget_get_clipboard(GTK_WIDGET(a->win)), u);
    toast(a, "%s %u item(s)", cut ? "Cut" : "Copied", g_list_length(files));
    queue_status(a);
}

typedef struct {
    App *a;
    GFile *dest;
} PasteReq;

static void paste_files(PasteReq *r, GList *files, gboolean cut) {
    if (files) transfer_files(r->a, files, r->dest, !cut);
    if (cut) {
        gdk_clipboard_set_content(gtk_widget_get_clipboard(GTK_WIDGET(r->a->win)), NULL);
        g_list_free_full(r->a->clip, g_object_unref);
        r->a->clip = NULL;
    }
}

static void paste_list_cb(GObject *src, GAsyncResult *res, gpointer data) {
    PasteReq *r = data;
    const GValue *v = gdk_clipboard_read_value_finish(GDK_CLIPBOARD(src), res, NULL);
    if (v && G_VALUE_HOLDS(v, GDK_TYPE_FILE_LIST)) {
        GSList *files = gdk_file_list_get_files(g_value_get_boxed(v));
        GList *l = NULL;
        for (GSList *s = files; s; s = s->next) l = g_list_prepend(l, s->data);
        l = g_list_reverse(l);
        paste_files(r, l, FALSE);
        g_list_free(l);
        g_slist_free(files);
    } else toast(r->a, "Nothing to paste");
    g_object_unref(r->dest);
    g_free(r);
}

static void paste_gnome_cb(GObject *src, GAsyncResult *res, gpointer data) {
    PasteReq *r = data;
    GdkClipboard *cb = GDK_CLIPBOARD(src);
    g_autoptr(GInputStream) in = gdk_clipboard_read_finish(cb, res, NULL, NULL);
    if (in) {
        g_autoptr(GOutputStream) out = g_memory_output_stream_new_resizable();
        g_output_stream_splice(out, in, G_OUTPUT_STREAM_SPLICE_CLOSE_SOURCE | G_OUTPUT_STREAM_SPLICE_CLOSE_TARGET, NULL, NULL);
        g_autoptr(GBytes) b = g_memory_output_stream_steal_as_bytes(G_MEMORY_OUTPUT_STREAM(out));
        g_autofree char *txt = g_strndup(g_bytes_get_data(b, NULL), g_bytes_get_size(b));
        g_auto(GStrv) lines = g_strsplit(txt, "\n", -1);
        gboolean cut = lines[0] && g_str_equal(g_strstrip(lines[0]), "cut");
        GList *l = NULL;
        for (char **p = lines + (lines[0] ? 1 : 0); *p; p++) {
            g_strstrip(*p);
            if (**p) l = g_list_prepend(l, g_file_new_for_uri(*p));
        }
        l = g_list_reverse(l);
        paste_files(r, l, cut);
        g_list_free_full(l, g_object_unref);
        g_object_unref(r->dest);
        g_free(r);
        return;
    }
    gdk_clipboard_read_value_async(cb, GDK_TYPE_FILE_LIST, G_PRIORITY_DEFAULT, NULL, paste_list_cb, r);
}

static void do_paste(App *a) {
    PasteReq *r = g_new0(PasteReq, 1);
    r->a = a;
    r->dest = g_object_ref(cur_tab(a)->dir);
    GdkClipboard *cb = gtk_widget_get_clipboard(GTK_WIDGET(a->win));
    GdkContentFormats *f = gdk_clipboard_get_formats(cb);
    if (gdk_content_formats_contain_mime_type(f, "x-special/gnome-copied-files")) {
        const char *mimes[] = {"x-special/gnome-copied-files", NULL};
        gdk_clipboard_read_async(cb, mimes, G_PRIORITY_DEFAULT, NULL, paste_gnome_cb, r);
    } else {
        gdk_clipboard_read_value_async(cb, GDK_TYPE_FILE_LIST, G_PRIORITY_DEFAULT, NULL, paste_list_cb, r);
    }
}

/* ---------------------------------------------------------------- chooser mode */
void finish_pick(App *a, GList *files) {
    if (a->pick_out) {
        GString *s = g_string_new(NULL);
        for (GList *l = files; l; l = l->next) {
            g_autofree char *p = g_file_get_path(l->data);
            if (p) g_string_append_printf(s, "%s\n", p);
        }
        g_file_set_contents(a->pick_out, s->str, -1, NULL);
        g_string_free(s, TRUE);
    } else {
        for (GList *l = files; l; l = l->next) {
            g_autofree char *p = g_file_get_path(l->data);
            if (p) g_print("%s\n", p);
        }
    }
    g_application_quit(G_APPLICATION(a->gapp));
}

static void replace_cb(GObject *src, GAsyncResult *res, gpointer data) {
    App *a = data;
    int btn = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(src), res, NULL);
    GFile *target = g_object_get_data(src, "target");
    if (btn == 1) {
        GList *l = g_list_prepend(NULL, target);
        finish_pick(a, l);
        g_list_free(l);
    }
}

static void choose(App *a) {
    Tab *t = cur_tab(a);
    GList *sel = view_targets(a);
    if (a->pick == PICK_DIR) {
        GList *one = NULL;
        if (sel && !sel->next && g_file_query_file_type(sel->data, 0, NULL) == G_FILE_TYPE_DIRECTORY)
            one = g_list_prepend(NULL, g_object_ref(sel->data));
        else one = g_list_prepend(NULL, g_object_ref(t->dir));
        finish_pick(a, one);
        g_list_free_full(one, g_object_unref);
    } else if (a->pick == PICK_SAVE) {
        g_autofree char *name = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(a->savename))));
        if (!*name || strchr(name, '/')) {
            toast(a, "Enter a file name");
        } else {
            GFile *target = g_file_get_child(t->dir, name);
            if (g_file_query_exists(target, NULL)) {
                const char *btns[] = {"Cancel", "Replace", NULL};
                g_autoptr(GtkAlertDialog) d = gtk_alert_dialog_new("Replace “%s”?", name);
                gtk_alert_dialog_set_buttons(d, btns);
                gtk_alert_dialog_set_cancel_button(d, 0);
                gtk_alert_dialog_set_default_button(d, 1);
                g_object_set_data_full(G_OBJECT(d), "target", target, g_object_unref);
                gtk_alert_dialog_choose(d, a->win, NULL, replace_cb, a);
            } else {
                GList *l = g_list_prepend(NULL, target);
                finish_pick(a, l);
                g_list_free_full(l, g_object_unref);
            }
        }
    } else {
        GList *files = NULL;
        for (GList *l = sel; l; l = l->next)
            if (g_file_query_file_type(l->data, 0, NULL) != G_FILE_TYPE_DIRECTORY) files = g_list_append(files, l->data);
        if (!files) toast(a, "Select a file first");
        else {
            if (a->pick == PICK_FILE && files->next) {
                g_list_free(files->next);
                files->next = NULL;
            }
            finish_pick(a, files);
        }
        g_list_free(files);
    }
    g_list_free_full(sel, g_object_unref);
}

/* ---------------------------------------------------------------- actions */
static void mkdir_cb(GObject *src, GAsyncResult *res, gpointer data) {
    g_autoptr(GError) err = NULL;
    if (!g_file_make_directory_finish(G_FILE(src), res, &err)) {
        toast(data, "%s", err->message);
        return;
    }
    Op *op = op_new(OP_CREATE, TRUE);
    op_add(op, NULL, G_FILE(src));
    undo_push(data, op);
    g_autofree char *name = g_file_get_basename(G_FILE(src));
    view_select_name(data, name, TRUE);
}

static void create_cb(GObject *src, GAsyncResult *res, gpointer data) {
    g_autoptr(GError) err = NULL;
    g_autoptr(GFileOutputStream) os = g_file_create_finish(G_FILE(src), res, &err);
    if (!os) {
        toast(data, "%s", err->message);
        return;
    }
    g_output_stream_close(G_OUTPUT_STREAM(os), NULL, NULL);
    Op *op = op_new(OP_CREATE, FALSE);
    op_add(op, NULL, G_FILE(src));
    undo_push(data, op);
    g_autofree char *name = g_file_get_basename(G_FILE(src));
    view_select_name(data, name, TRUE);
}

static void act(GSimpleAction *action, GVariant *param, gpointer data) {
    (void)param;
    App *a = data;
    const char *n = g_action_get_name(G_ACTION(action));
    Tab *t = cur_tab(a);

    if (g_str_equal(n, "back") && t->back->len) {
        g_ptr_array_add(t->fwd, g_object_ref(t->dir));
        g_autoptr(GFile) d = g_ptr_array_steal_index(t->back, t->back->len - 1);
        navigate(a, d, FALSE, NULL);
    } else if (g_str_equal(n, "forward") && t->fwd->len) {
        g_ptr_array_add(t->back, g_object_ref(t->dir));
        g_autoptr(GFile) d = g_ptr_array_steal_index(t->fwd, t->fwd->len - 1);
        navigate(a, d, FALSE, NULL);
    } else if (g_str_equal(n, "up")) {
        g_autoptr(GFile) p = g_file_get_parent(t->dir);
        g_autofree char *name = g_file_get_basename(t->dir);
        if (p) navigate(a, p, TRUE, name);
    } else if (g_str_equal(n, "refresh")) {
        g_autoptr(GFile) d = g_object_ref(t->dir);
        gtk_directory_list_set_file(a->dirlist, NULL);
        gtk_directory_list_set_file(a->dirlist, d);
        sidebar_rebuild(a);
    } else if (g_str_equal(n, "new-tab")) {
        open_in_new_tab(a, t->dir);
    } else if (g_str_equal(n, "close-tab")) {
        close_tab(a, a->cur);
    } else if (g_str_equal(n, "next-tab")) {
        switch_tab(a, (a->cur + 1) % a->tabs->len);
    } else if (g_str_equal(n, "prev-tab")) {
        switch_tab(a, (a->cur + a->tabs->len - 1) % a->tabs->len);
    } else if (g_str_equal(n, "address")) {
        show_address(a, TRUE);
    } else if (g_str_equal(n, "search")) {
        gtk_widget_grab_focus(a->search);
    } else if (g_str_equal(n, "toggle-hidden")) {
        a->show_hidden = !a->show_hidden;
        gtk_filter_changed(GTK_FILTER(a->filter), a->show_hidden ? GTK_FILTER_CHANGE_LESS_STRICT : GTK_FILTER_CHANGE_MORE_STRICT);
        GtkWidget *b = g_hash_table_lookup(a->buttons, "toggle-hidden");
        if (b) gtk_button_set_label(GTK_BUTTON(b), a->show_hidden ? "" : "");
    } else if (g_str_equal(n, "open") || g_str_equal(n, "open-tab")) {
        GList *sel = view_targets(a);
        gboolean tabs = g_str_equal(n, "open-tab");
        for (GList *l = sel; l; l = l->next) {
            gboolean dir = g_file_query_file_type(l->data, 0, NULL) == G_FILE_TYPE_DIRECTORY;
            if (dir && (tabs || sel->next)) open_in_new_tab(a, l->data);
            else if (dir) navigate(a, l->data, TRUE, NULL);
            else if (!tabs) open_file(a, l->data);
        }
        g_list_free_full(sel, g_object_unref);
    } else if (g_str_equal(n, "cut") || g_str_equal(n, "copy")) {
        GList *sel = view_targets(a);
        if (sel) clip_set(a, sel, g_str_equal(n, "cut"));
    } else if (g_str_equal(n, "paste")) {
        do_paste(a);
    } else if (g_str_equal(n, "copy-path")) {
        GList *sel = view_targets(a);
        GString *s = g_string_new(NULL);
        for (GList *l = sel; l; l = l->next) {
            g_autofree char *p = g_file_get_parse_name(l->data);
            g_string_append_printf(s, "%s%s", s->len ? "\n" : "", p);
        }
        gdk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(a->win)), s->str);
        g_string_free(s, TRUE);
        g_list_free_full(sel, g_object_unref);
        toast(a, "Path copied");
    } else if (g_str_equal(n, "rename")) {
        view_begin_rename(a);
    } else if (g_str_equal(n, "trash")) {
        GList *sel = view_targets(a);
        trash_files(a, sel);
        g_list_free_full(sel, g_object_unref);
    } else if (g_str_equal(n, "new-folder")) {
        g_autoptr(GFile) f = unique_child(t->dir, "New folder");
        g_file_make_directory_async(f, G_PRIORITY_DEFAULT, NULL, mkdir_cb, a);
    } else if (g_str_equal(n, "new-file")) {
        g_autoptr(GFile) f = unique_child(t->dir, "New file");
        g_file_create_async(f, G_FILE_CREATE_NONE, G_PRIORITY_DEFAULT, NULL, create_cb, a);
    } else if (g_str_equal(n, "terminal")) {
        g_autofree char *p = g_file_get_path(t->dir);
        const char *argv[] = {"xdg-terminal-exec", NULL};
        g_autoptr(GError) err = NULL;
        if (p && !g_spawn_async(p, (char **)argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err))
            toast(a, "%s", err->message);
    } else if (g_str_equal(n, "undo") || g_str_equal(n, "redo")) {
        undo_run(a, g_str_equal(n, "redo"));
    } else if (g_str_equal(n, "choose")) {
        choose(a);
    } else if (g_str_equal(n, "cancel-pick") || g_str_equal(n, "quit")) {
        gtk_window_close(a->win);
    }
}

static const char *ACTIONS[] = {"back", "forward", "up", "refresh", "new-tab", "close-tab", "next-tab", "prev-tab",
                                "address", "search", "toggle-hidden", "open", "open-tab", "cut", "copy", "paste",
                                "copy-path", "rename", "trash", "new-folder", "new-file", "terminal", "undo", "redo", "choose",
                                "cancel-pick", "quit"};

static void setup_actions(App *a) {
    for (guint i = 0; i < G_N_ELEMENTS(ACTIONS); i++) {
        GSimpleAction *s = g_simple_action_new(ACTIONS[i], NULL);
        g_signal_connect(s, "activate", G_CALLBACK(act), a);
        g_action_map_add_action(G_ACTION_MAP(a->win), G_ACTION(s));
        g_object_unref(s);
    }
    struct { const char *act; const char *keys[3]; } accels[] = {
        {"win.back", {"<Alt>Left", NULL}},
        {"win.forward", {"<Alt>Right", NULL}},
        {"win.up", {"<Alt>Up", NULL}},
        {"win.refresh", {"F5", "<Control>r", NULL}},
        {"win.new-tab", {"<Control>t", NULL}},
        {"win.close-tab", {"<Control>w", NULL}},
        {"win.next-tab", {"<Control>Page_Down", "<Control>Tab", NULL}},
        {"win.prev-tab", {"<Control>Page_Up", "<Control><Shift>Tab", NULL}},
        {"win.address", {"<Control>l", NULL}},
        {"win.search", {"<Control>f", NULL}},
        {"win.toggle-hidden", {"<Control>h", NULL}},
        {"win.new-folder", {"<Control><Shift>n", NULL}},
        {"win.terminal", {"<Control><Alt>t", NULL}},
        {"win.quit", {"<Control>q", NULL}},
        {"win.undo", {"<Control>z", NULL}},
        {"win.redo", {"<Control><Shift>z", "<Control>y", NULL}},
    };
    for (guint i = 0; i < G_N_ELEMENTS(accels); i++)
        gtk_application_set_accels_for_action(a->gapp, accels[i].act, accels[i].keys);
}

/* ---------------------------------------------------------------- building */
static GtkWidget *glyph_button(App *a, const char *glyph, const char *action, const char *tip) {
    GtkWidget *b = gtk_button_new_with_label(glyph);
    gtk_widget_add_css_class(b, "glyph");
    gtk_widget_set_tooltip_text(b, tip);
    gtk_actionable_set_action_name(GTK_ACTIONABLE(b), action);
    g_hash_table_insert(a->buttons, g_strdup(action + 4), b);
    return b;
}

static void on_search_changed(GtkSearchEntry *e, gpointer data) {
    search_set(data, gtk_editable_get_text(GTK_EDITABLE(e)));
}

static void on_search_stop(GtkSearchEntry *e, gpointer data) {
    gtk_editable_set_text(GTK_EDITABLE(e), "");
    gtk_widget_grab_focus(((App *)data)->view);
}

static void on_search_activate(GtkSearchEntry *e, gpointer data) {
    (void)e;
    gtk_widget_grab_focus(((App *)data)->view);
}

static void on_mouse_nav(GtkGestureClick *g, int n, double x, double y, gpointer data) {
    (void)n; (void)x; (void)y;
    guint b = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
    if (b == 8) g_action_group_activate_action(G_ACTION_GROUP(((App *)data)->win), "back", NULL);
    else if (b == 9) g_action_group_activate_action(G_ACTION_GROUP(((App *)data)->win), "forward", NULL);
}

static GtkWidget *build_actions(App *a) {
    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(bar, "actions");
    gtk_widget_set_margin_start(bar, 6);
    gtk_widget_set_margin_end(bar, 6);
    if (a->pick) {
        const char *label = a->pick == PICK_SAVE ? "  Save" : a->pick == PICK_DIR ? "  Choose this folder" : "  Choose";
        GtkWidget *ok = gtk_button_new_with_label(label);
        gtk_widget_add_css_class(ok, "suggested");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(ok), "win.choose");
        GtkWidget *cancel = gtk_button_new_with_label("  Cancel");
        gtk_actionable_set_action_name(GTK_ACTIONABLE(cancel), "win.cancel-pick");
        if (a->pick == PICK_SAVE) {
            a->savename = gtk_entry_new();
            gtk_widget_set_size_request(a->savename, 300, -1);
            gtk_widget_set_margin_end(a->savename, 6);
            gtk_editable_set_text(GTK_EDITABLE(a->savename), a->pick_name ? a->pick_name : "");
            gtk_actionable_set_action_name(GTK_ACTIONABLE(a->savename), NULL);
            g_signal_connect_swapped(a->savename, "activate", G_CALLBACK(choose), a);
            gtk_box_append(GTK_BOX(bar), a->savename);
        }
        gtk_box_append(GTK_BOX(bar), ok);
        gtk_box_append(GTK_BOX(bar), cancel);
    }
    struct { const char *g, *act, *tip; gboolean picker; } items[] = {
        {"+", "win.new-folder", "New folder (Ctrl+Shift+N)", TRUE},
        {"+", "win.new-file", "New file", FALSE},
        {"", "win.cut", "Cut (Ctrl+X)", FALSE},
        {"", "win.copy", "Copy (Ctrl+C)", FALSE},
        {"", "win.paste", "Paste (Ctrl+V)", FALSE},
        {"", "win.rename", "Rename (F2)", FALSE},
        {"", "win.trash", "Move to Trash (Delete)", FALSE},
        {"\uf0e2", "win.undo", "Undo (Ctrl+Z)", FALSE},
        {"\uf01e", "win.redo", "Redo (Ctrl+Shift+Z / Ctrl+Y)", FALSE},
        {"", "win.toggle-hidden", "Show hidden files (Ctrl+H)", TRUE},
    };
    for (guint i = 0; i < G_N_ELEMENTS(items); i++) {
        if (a->pick && !items[i].picker) continue;
        gtk_box_append(GTK_BOX(bar), glyph_button(a, items[i].g, items[i].act, items[i].tip));
    }
    return bar;
}

void window_build(App *a, GFile *start) {
    a->buttons = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    a->tabs = g_ptr_array_new();
    g_ptr_array_add(a->tabs, tab_new(start));
    a->cur = 0;

    GtkWidget *win = gtk_application_window_new(a->gapp);
    a->win = GTK_WINDOW(win);
    gtk_widget_add_css_class(win, "f4h");
    gtk_window_set_default_size(a->win, a->pick ? 900 : 1100, a->pick ? 600 : 680);
    setup_actions(a);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    a->tabbar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(a->tabbar, "tabbar");
    gtk_box_append(GTK_BOX(root), a->tabbar);

    /* location bar */
    GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_add_css_class(nav, "panel");
    gtk_widget_set_margin_start(nav, 6);
    gtk_widget_set_margin_end(nav, 6);
    gtk_box_append(GTK_BOX(nav), glyph_button(a, "", "win.back", "Back (Alt+Left)"));
    gtk_box_append(GTK_BOX(nav), glyph_button(a, "", "win.forward", "Forward (Alt+Right)"));
    gtk_box_append(GTK_BOX(nav), glyph_button(a, "", "win.up", "Up (Alt+Up / Backspace)"));
    gtk_box_append(GTK_BOX(nav), glyph_button(a, "", "win.refresh", "Refresh (F5)"));

    a->crumbs = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    a->crumbs_sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(a->crumbs_sw), GTK_POLICY_EXTERNAL, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(a->crumbs_sw), a->crumbs);
    gtk_widget_set_hexpand(a->crumbs_sw, TRUE);
    GtkGesture *cg = gtk_gesture_click_new();
    g_signal_connect(cg, "released", G_CALLBACK(on_crumbs_click), a);
    gtk_widget_add_controller(a->crumbs_sw, GTK_EVENT_CONTROLLER(cg));

    a->addr = gtk_entry_new();
    g_signal_connect(a->addr, "activate", G_CALLBACK(on_addr_activate), a);
    GtkEventController *ak = gtk_event_controller_key_new();
    g_signal_connect(ak, "key-pressed", G_CALLBACK(on_addr_key), a);
    gtk_widget_add_controller(a->addr, ak);
    GtkEventController *af = gtk_event_controller_focus_new();
    g_signal_connect(af, "leave", G_CALLBACK(on_addr_leave), a);
    gtk_widget_add_controller(a->addr, af);

    a->loc_stack = gtk_stack_new();
    gtk_stack_add_named(GTK_STACK(a->loc_stack), a->crumbs_sw, "crumbs");
    gtk_stack_add_named(GTK_STACK(a->loc_stack), a->addr, "addr");
    gtk_widget_set_hexpand(a->loc_stack, TRUE);
    gtk_widget_set_margin_start(a->loc_stack, 6);
    gtk_box_append(GTK_BOX(nav), a->loc_stack);

    a->search = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(a->search), "Search");
    gtk_widget_set_size_request(a->search, 220, -1);
    gtk_widget_set_margin_start(a->search, 6);
    g_signal_connect(a->search, "search-changed", G_CALLBACK(on_search_changed), a);
    g_signal_connect(a->search, "stop-search", G_CALLBACK(on_search_stop), a);
    g_signal_connect(a->search, "activate", G_CALLBACK(on_search_activate), a);
    gtk_box_append(GTK_BOX(nav), a->search);
    gtk_box_append(GTK_BOX(root), nav);

    a->actions = build_actions(a);
    gtk_box_append(GTK_BOX(root), a->actions);

    /* body: sidebar | files */
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_widget_set_margin_start(body, 6);
    gtk_widget_set_margin_end(body, 6);
    gtk_widget_set_vexpand(body, TRUE);

    a->side_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(a->side_panel, "panel");
    gtk_widget_set_size_request(a->side_panel, 220, -1);
    gtk_box_append(GTK_BOX(a->side_panel), sidebar_new(a));
    gtk_box_append(GTK_BOX(body), a->side_panel);

    a->main_panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(a->main_panel, "panel");
    gtk_widget_set_hexpand(a->main_panel, TRUE);
    a->main_title = gtk_label_new(NULL);
    gtk_widget_add_css_class(a->main_title, "panel-title");
    gtk_label_set_xalign(GTK_LABEL(a->main_title), 0);
    gtk_label_set_ellipsize(GTK_LABEL(a->main_title), PANGO_ELLIPSIZE_START);
    gtk_box_append(GTK_BOX(a->main_panel), a->main_title);

    GtkWidget *overlay = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(overlay), view_new(a));
    a->toast_rev = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(a->toast_rev), GTK_REVEALER_TRANSITION_TYPE_CROSSFADE);
    gtk_widget_set_halign(a->toast_rev, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(a->toast_rev, GTK_ALIGN_END);
    gtk_widget_set_can_target(a->toast_rev, FALSE);
    a->toast_label = gtk_label_new(NULL);
    gtk_label_set_wrap(GTK_LABEL(a->toast_label), TRUE);
    gtk_widget_add_css_class(a->toast_label, "toast");
    gtk_revealer_set_child(GTK_REVEALER(a->toast_rev), a->toast_label);
    gtk_overlay_add_overlay(GTK_OVERLAY(overlay), a->toast_rev);
    gtk_box_append(GTK_BOX(a->main_panel), overlay);
    gtk_box_append(GTK_BOX(body), a->main_panel);
    gtk_box_append(GTK_BOX(root), body);

    a->status = gtk_label_new(NULL);
    gtk_widget_add_css_class(a->status, "status");
    gtk_label_set_xalign(GTK_LABEL(a->status), 0);
    gtk_label_set_ellipsize(GTK_LABEL(a->status), PANGO_ELLIPSIZE_END);
    gtk_box_append(GTK_BOX(root), a->status);

    GtkGesture *mg = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(mg), 0);
    gtk_event_controller_set_propagation_phase(GTK_EVENT_CONTROLLER(mg), GTK_PHASE_CAPTURE);
    g_signal_connect(mg, "pressed", G_CALLBACK(on_mouse_nav), a);
    gtk_widget_add_controller(win, GTK_EVENT_CONTROLLER(mg));

    gtk_window_set_child(a->win, root);
    undo_update(a);
    navigate(a, start, FALSE, NULL);
    gtk_widget_grab_focus(a->view);
}
