/* File list: GtkDirectoryList (async, live-monitored) -> filter -> sort ->
 * multi-selection -> GtkColumnView. Rows are drag sources, folder rows are
 * drop targets, names are renamed in place. */
#include "app.h"
#include <string.h>

/* ---------------------------------------------------------------- glyphs */
static const char *glyph_for(GFileInfo *info) {
    if (info_is_dir(info)) return "";
    if (g_file_info_get_is_symlink(info)) return "";
    const char *ct = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
    if (ct) {
        if (g_str_has_prefix(ct, "image/")) return "";
        if (g_str_has_prefix(ct, "video/")) return "";
        if (g_str_has_prefix(ct, "audio/")) return "";
        if (g_str_equal(ct, "application/pdf")) return "";
        if (strstr(ct, "zip") || strstr(ct, "tar") || strstr(ct, "7z") || strstr(ct, "rar") || strstr(ct, "compressed") ||
            strstr(ct, "archive"))
            return "";
        if (g_str_has_prefix(ct, "text/") || strstr(ct, "script") || strstr(ct, "json") || strstr(ct, "xml"))
            return "";
    }
    return "";
}

/* ---------------------------------------------------------------- filter / sort */
static gboolean filter_func(gpointer item, gpointer data) {
    App *a = data;
    GFileInfo *info = item;
    const char *name = g_file_info_get_display_name(info);
    if (!a->show_hidden && (name[0] == '.' || g_file_info_get_is_hidden(info) || g_file_info_get_is_backup(info)))
        return FALSE;
    if (a->pick == PICK_DIR && !info_is_dir(info)) return FALSE;
    if (a->query) {
        g_autofree char *f = g_utf8_casefold(name, -1);
        if (!strstr(f, a->query)) return FALSE;
    }
    return TRUE;
}

static const char *collate_key(GFileInfo *info) {
    const char *k = g_object_get_data(G_OBJECT(info), "ck");
    if (!k) {
        char *nk = g_utf8_collate_key_for_filename(g_file_info_get_display_name(info), -1);
        g_object_set_data_full(G_OBJECT(info), "ck", nk, g_free);
        k = nk;
    }
    return k;
}

static int sort_func(gconstpointer x, gconstpointer y, gpointer data) {
    App *a = data;
    GFileInfo *i1 = (GFileInfo *)x, *i2 = (GFileInfo *)y;
    gboolean d1 = info_is_dir(i1), d2 = info_is_dir(i2);
    if (d1 != d2) return d1 ? -1 : 1;
    int r = 0;
    if (a->sort_col == SORT_SIZE && !d1) {
        goffset s1 = g_file_info_get_size(i1), s2 = g_file_info_get_size(i2);
        r = s1 < s2 ? -1 : s1 > s2;
    } else if (a->sort_col == SORT_MTIME) {
        guint64 t1 = g_file_info_get_attribute_uint64(i1, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        guint64 t2 = g_file_info_get_attribute_uint64(i2, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        r = t1 < t2 ? -1 : t1 > t2;
    }
    if (r == 0) r = strcmp(collate_key(i1), collate_key(i2));
    return a->sort_desc ? -r : r;
}

static void on_header_sort(GtkSorter *s, GtkSorterChange ch, gpointer data) {
    (void)ch;
    App *a = data;
    GtkColumnViewColumn *col = gtk_column_view_sorter_get_primary_sort_column(GTK_COLUMN_VIEW_SORTER(s));
    if (!col) return;
    a->sort_col = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(col), "sortcol"));
    a->sort_desc = gtk_column_view_sorter_get_primary_sort_order(GTK_COLUMN_VIEW_SORTER(s)) == GTK_SORT_DESCENDING;
    gtk_sorter_changed(GTK_SORTER(a->sorter), GTK_SORTER_CHANGE_DIFFERENT);
    view_sync_sort_actions(a);
    settings_save(a);
}

static GtkColumnViewColumn *sort_column(App *a, SortCol c) {
    return c == SORT_SIZE ? a->col_size : c == SORT_MTIME ? a->col_mod : a->col_name;
}

void view_set_sort(App *a, SortCol col, gboolean desc) {
    gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(a->view), sort_column(a, col),
                                   desc ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
}

static void set_state(App *a, const char *name, const char *val) {
    GAction *act = g_action_map_lookup_action(G_ACTION_MAP(a->win), name);
    if (act) g_simple_action_set_state(G_SIMPLE_ACTION(act), g_variant_new_string(val));
}

void view_sync_sort_actions(App *a) {
    set_state(a, "sort-by", a->sort_col == SORT_SIZE ? "size" : a->sort_col == SORT_MTIME ? "time" : "name");
    set_state(a, "sort-order", a->sort_desc ? "desc" : "asc");
    set_state(a, "view-size", a->row_size);
}

void view_set_row_size(App *a, const char *size) {
    g_strlcpy(a->row_size, size, sizeof a->row_size);
    gtk_widget_remove_css_class(a->view, "rows-small");
    gtk_widget_remove_css_class(a->view, "rows-large");
    if (g_str_equal(size, "small")) gtk_widget_add_css_class(a->view, "rows-small");
    else if (g_str_equal(size, "large")) gtk_widget_add_css_class(a->view, "rows-large");
    view_sync_sort_actions(a);
    settings_save(a);
}

/* ---------------------------------------------------------------- selection helpers */
static void grab_view_focus(App *a);

GList *view_targets(App *a) {
    GList *out = NULL;
    g_autoptr(GtkBitset) set = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(a->sel));
    GtkBitsetIter it;
    guint pos;
    for (gboolean ok = gtk_bitset_iter_init_first(&it, set, &pos); ok; ok = gtk_bitset_iter_next(&it, &pos)) {
        g_autoptr(GFileInfo) fi = info_at(a, pos);
        if (fi) out = g_list_prepend(out, g_object_ref(info_file(fi)));
    }
    return g_list_reverse(out);
}

GFileInfo *view_focused(App *a, guint *pos) {
    g_autoptr(GtkBitset) set = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(a->sel));
    if (gtk_bitset_is_empty(set)) return NULL;
    guint p = gtk_bitset_get_minimum(set);
    if (pos) *pos = p;
    return info_at(a, p);
}

static gint find_name(App *a, const char *name) {
    guint n = g_list_model_get_n_items(G_LIST_MODEL(a->sorted));
    for (guint i = 0; i < n; i++) {
        g_autoptr(GFileInfo) fi = info_at(a, i);
        if (g_str_equal(g_file_info_get_name(fi), name)) return i;
    }
    return -1;
}

static gboolean rename_idle(gpointer data) {
    view_begin_rename(data);
    return G_SOURCE_REMOVE;
}

void view_select_name(App *a, const char *name, gboolean rename) {
    gint pos = name ? find_name(a, name) : -1;
    if (pos < 0) {
        g_free(a->pending_select);
        a->pending_select = g_strdup(name);
        a->pending_rename = rename;
        return;
    }
    g_clear_pointer(&a->pending_select, g_free);
    a->focus_first = FALSE;
    grab_view_focus(a);
    gtk_column_view_scroll_to(GTK_COLUMN_VIEW(a->view), pos, NULL, GTK_LIST_SCROLL_FOCUS | GTK_LIST_SCROLL_SELECT, NULL);
    if (rename) g_timeout_add(60, rename_idle, a);
}

/* Keep the keyboard on the file list: put focus there unless the user is typing in the path or search box. */
static void grab_view_focus(App *a) {
    GtkWidget *f = gtk_window_get_focus(a->win);
    if (f && (f == a->addr || f == a->search || gtk_widget_is_ancestor(f, a->addr) || gtk_widget_is_ancestor(f, a->search) ||
              (a->savename && (f == a->savename || gtk_widget_is_ancestor(f, a->savename)))))
        return;
    gtk_widget_grab_focus(a->view);
}

void view_focus_first(App *a) {
    a->focus_first = FALSE;
    if (a->renaming || a->pending_select) return;
    grab_view_focus(a);
    if (g_list_model_get_n_items(G_LIST_MODEL(a->sorted)) == 0) return;
    g_autoptr(GtkBitset) set = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(a->sel));
    if (!gtk_bitset_is_empty(set)) {
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(a->view), gtk_bitset_get_minimum(set), NULL, GTK_LIST_SCROLL_FOCUS, NULL);
        return;
    }
    gtk_column_view_scroll_to(GTK_COLUMN_VIEW(a->view), 0, NULL, GTK_LIST_SCROLL_FOCUS | GTK_LIST_SCROLL_SELECT, NULL);
}

void view_try_pending(App *a) {
    if (!a->pending_select) return;
    g_autofree char *name = g_strdup(a->pending_select);
    if (find_name(a, name) >= 0) view_select_name(a, name, a->pending_rename);
}

/* ---------------------------------------------------------------- rename in place */
static void stack_show(GFileInfo *info, const char *page) {
    GtkWidget *cell = g_object_get_data(G_OBJECT(info), "cell");
    if (cell) gtk_stack_set_visible_child_name(GTK_STACK(g_object_get_data(G_OBJECT(cell), "stack")), page);
}

void view_begin_rename(App *a) {
    if (a->renaming || a->pick) return;
    guint pos;
    g_autoptr(GFileInfo) info = view_focused(a, &pos);
    if (!info) return;
    GtkWidget *cell = g_object_get_data(G_OBJECT(info), "cell");
    if (!cell) {
        gtk_column_view_scroll_to(GTK_COLUMN_VIEW(a->view), pos, NULL, GTK_LIST_SCROLL_FOCUS, NULL);
        g_timeout_add(60, rename_idle, a);
        return;
    }
    GtkWidget *entry = g_object_get_data(G_OBJECT(cell), "entry");
    const char *name = g_file_info_get_display_name(info);
    gtk_editable_set_text(GTK_EDITABLE(entry), name);
    a->renaming = g_object_ref(info);
    stack_show(info, "entry");
    gtk_widget_grab_focus(entry);
    const char *dot = info_is_dir(info) ? NULL : strrchr(name, '.');
    int end = dot && dot != name ? (int)g_utf8_pointer_to_offset(name, dot) : -1;
    gtk_editable_select_region(GTK_EDITABLE(entry), 0, end);
}

static void renamed_cb(GObject *src, GAsyncResult *res, gpointer data) {
    App *a = data;
    g_autoptr(GError) err = NULL;
    g_autoptr(GFile) nf = g_file_set_display_name_finish(G_FILE(src), res, &err);
    if (nf) {
        Op *op = op_new(OP_RENAME, FALSE);
        op_add(op, G_FILE(src), nf);
        undo_push(a, op);
    }
    if (!nf) {
        toast(a, "Rename failed: %s", err->message);
        return;
    }
    g_autofree char *name = g_file_get_basename(nf);
    view_select_name(a, name, FALSE);
}

static void finish_rename(App *a, gboolean commit) {
    GFileInfo *info = a->renaming;
    if (!info) return;
    a->renaming = NULL;
    GtkWidget *cell = g_object_get_data(G_OBJECT(info), "cell");
    g_autofree char *name = NULL;
    if (cell) name = g_strdup(gtk_editable_get_text(GTK_EDITABLE(g_object_get_data(G_OBJECT(cell), "entry"))));
    stack_show(info, "label");
    gtk_widget_grab_focus(a->view);
    if (commit && name) g_strstrip(name);
    if (commit && name && *name && !g_str_equal(name, g_file_info_get_display_name(info))) {
        if (strchr(name, '/')) toast(a, "A name can't contain “/”");
        else g_file_set_display_name_async(info_file(info), name, G_PRIORITY_DEFAULT, NULL, renamed_cb, a);
    }
    g_object_unref(info);
}

static void on_entry_activate(GtkEntry *e, gpointer data) {
    (void)e;
    finish_rename(data, TRUE);
}

static gboolean on_entry_key(GtkEventControllerKey *k, guint kv, guint code, GdkModifierType st, gpointer data) {
    (void)k; (void)code; (void)st;
    if (kv == GDK_KEY_Escape) {
        finish_rename(data, FALSE);
        return TRUE;
    }
    return FALSE;
}

static void on_entry_leave(GtkEventControllerFocus *f, gpointer data) {
    (void)f;
    finish_rename(data, FALSE);
}

/* ---------------------------------------------------------------- context menu */
static GMenuItem *radio(const char *label, const char *action, const char *target) {
    GMenuItem *it = g_menu_item_new(label, NULL);
    g_menu_item_set_action_and_target(it, action, "s", target);
    return it;
}

static void append_radio(GMenu *m, const char *label, const char *action, const char *target) {
    GMenuItem *it = radio(label, action, target);
    g_menu_append_item(m, it);
    g_object_unref(it);
}

static void add_section(GMenu *m, GMenu *sec) {
    if (g_menu_model_get_n_items(G_MENU_MODEL(sec))) g_menu_append_section(m, NULL, G_MENU_MODEL(sec));
    g_object_unref(sec);
}

static gboolean all_pinned(GList *targets) {
    for (GList *l = targets; l; l = l->next)
        if (!sidebar_is_pinned(l->data)) return FALSE;
    return targets != NULL;
}

/* Built on every right-click: it depends on what is selected and on what other programs offer. */
static GMenuModel *build_item_menu(App *a, GList *targets) {
    GMenu *m = g_menu_new();
    GMenu *s1 = g_menu_new(), *s2 = g_menu_new(), *s3 = g_menu_new();
    g_menu_append(s1, "Open", "win.open");
    g_menu_append(s1, "Open in new tab", "win.open-tab");
    if (targets && !targets->next) {
        g_autofree char *name = g_file_get_basename(targets->data);
        if (name && g_str_has_suffix(name, ".sh")) g_menu_append(s1, "Open as program", "win.run-script");
    }
    g_menu_append(s2, "Cut", "win.cut");
    g_menu_append(s2, "Copy", "win.copy");
    g_menu_append(s2, "Paste", "win.paste");
    g_menu_append(s3, "Rename", "win.rename");
    g_menu_append(s3, "Copy path", "win.copy-path");
    if (all_pinned(targets)) g_menu_append(s3, "Unpin from sidebar", "win.unpin");
    else g_menu_append(s3, "Pin to sidebar", "win.pin");
    g_menu_append(s3, "Move to Trash", "win.trash");
    add_section(m, s1);
    add_section(m, s2);
    add_section(m, s3);
    GMenuModel *ext = ext_menu_build(a, targets);
    if (ext) {
        g_menu_append_section(m, NULL, ext);
        g_object_unref(ext);
    }
    return G_MENU_MODEL(m);
}

static GMenuModel *build_empty_menu(App *a) {
    GMenu *m = g_menu_new();
    GMenu *s0 = g_menu_new(), *s1 = g_menu_new(), *s2 = g_menu_new(), *s3 = g_menu_new();
    GMenu *view = g_menu_new(), *sort = g_menu_new();
    append_radio(view, "Large rows", "win.view-size", "large");
    append_radio(view, "Medium rows", "win.view-size", "medium");
    append_radio(view, "Small rows", "win.view-size", "small");
    g_menu_append_submenu(s0, "View", G_MENU_MODEL(view));
    append_radio(sort, "Name", "win.sort-by", "name");
    append_radio(sort, "Size", "win.sort-by", "size");
    append_radio(sort, "Date modified", "win.sort-by", "time");
    GMenu *order = g_menu_new();
    append_radio(order, "Ascending", "win.sort-order", "asc");
    append_radio(order, "Descending", "win.sort-order", "desc");
    g_menu_append_section(sort, NULL, G_MENU_MODEL(order));
    g_menu_append_submenu(s0, "Sort by", G_MENU_MODEL(sort));
    g_menu_append(s1, "Paste", "win.paste");
    g_menu_append(s1, "Undo", "win.undo");
    g_menu_append(s1, "Redo", "win.redo");
    g_menu_append(s2, "New folder", "win.new-folder");
    g_menu_append(s2, "New file", "win.new-file");
    Tab *t = cur_tab(a);
    g_menu_append(s2, t && sidebar_is_pinned(t->dir) ? "Unpin this folder" : "Pin this folder", t && sidebar_is_pinned(t->dir) ? "win.unpin" : "win.pin");
    g_menu_append(s3, "Open terminal here", "win.terminal");
    g_menu_append(s3, "Show hidden files", "win.toggle-hidden");
    g_menu_append(s3, "Refresh", "win.refresh");
    add_section(m, s0);
    add_section(m, s1);
    add_section(m, s2);
    add_section(m, s3);
    g_object_unref(view);
    g_object_unref(sort);
    g_object_unref(order);
    return G_MENU_MODEL(m);
}

static void on_cell_click(GtkGestureClick *g, int n, double x, double y, gpointer data) {
    (void)n;
    App *a = data;
    GtkWidget *cell = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    GtkListItem *li = g_object_get_data(G_OBJECT(cell), "li");
    if (!li || !gtk_list_item_get_item(li)) return;
    gtk_gesture_set_state(GTK_GESTURE(g), GTK_EVENT_SEQUENCE_CLAIMED);
    guint pos = gtk_list_item_get_position(li);
    if (!gtk_list_item_get_selected(li)) gtk_selection_model_select_item(GTK_SELECTION_MODEL(a->sel), pos, TRUE);
    GList *targets = view_targets(a);
    g_autoptr(GMenuModel) menu = build_item_menu(a, targets);
    g_list_free_full(targets, g_object_unref);
    popup_menu(a, menu, cell, x, y);
}

static void on_empty_click(GtkGestureClick *g, int n, double x, double y, gpointer data) {
    (void)n;
    App *a = data;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(g));
    gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(a->sel));
    g_autoptr(GMenuModel) menu = build_empty_menu(a);
    popup_menu(a, menu, w, x, y);
}

/* ---------------------------------------------------------------- cells */
static GFile *cell_dest(App *a, GtkWidget *w) {
    (void)a;
    GtkListItem *li = g_object_get_data(G_OBJECT(w), "li");
    GFileInfo *info = li ? gtk_list_item_get_item(li) : NULL;
    return info && info_is_dir(info) ? g_object_ref(info_file(info)) : NULL;
}

static GFile *view_dest(App *a, GtkWidget *w) {
    (void)w;
    Tab *t = cur_tab(a);
    return t ? g_object_ref(t->dir) : NULL;
}

static void cell_common(App *a, GtkListItem *li, GtkWidget *cell) {
    g_object_set_data(G_OBJECT(cell), "li", li);
    add_drag_source(a, cell);
    add_drop_target(a, cell, cell_dest);
    GtkGesture *g = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), GDK_BUTTON_SECONDARY);
    g_signal_connect(g, "pressed", G_CALLBACK(on_cell_click), a);
    gtk_widget_add_controller(cell, GTK_EVENT_CONTROLLER(g));
}

static void name_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f;
    App *a = data;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *g = gtk_label_new(NULL);
    gtk_widget_set_size_request(g, 16, -1);
    GtkWidget *stack = gtk_stack_new();
    gtk_widget_set_hexpand(stack, TRUE);
    GtkWidget *l = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(l), 0);
    gtk_label_set_ellipsize(GTK_LABEL(l), PANGO_ELLIPSIZE_MIDDLE);
    GtkWidget *e = gtk_entry_new();
    gtk_stack_add_named(GTK_STACK(stack), l, "label");
    gtk_stack_add_named(GTK_STACK(stack), e, "entry");
    gtk_box_append(GTK_BOX(box), g);
    gtk_box_append(GTK_BOX(box), stack);
    g_object_set_data(G_OBJECT(box), "glyph", g);
    g_object_set_data(G_OBJECT(box), "label", l);
    g_object_set_data(G_OBJECT(box), "stack", stack);
    g_object_set_data(G_OBJECT(box), "entry", e);
    g_signal_connect(e, "activate", G_CALLBACK(on_entry_activate), a);
    GtkEventController *k = gtk_event_controller_key_new();
    g_signal_connect(k, "key-pressed", G_CALLBACK(on_entry_key), a);
    gtk_widget_add_controller(e, k);
    GtkEventController *fc = gtk_event_controller_focus_new();
    g_signal_connect(fc, "leave", G_CALLBACK(on_entry_leave), a);
    gtk_widget_add_controller(e, fc);
    cell_common(a, li, box);
    gtk_list_item_set_child(li, box);
}

static void name_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f; (void)data;
    GtkWidget *box = gtk_list_item_get_child(li);
    GFileInfo *info = gtk_list_item_get_item(li);
    GtkWidget *g = g_object_get_data(G_OBJECT(box), "glyph");
    gtk_label_set_text(GTK_LABEL(g), glyph_for(info));
    if (info_is_dir(info)) gtk_widget_add_css_class(g, "glyph-dir");
    else gtk_widget_remove_css_class(g, "glyph-dir");
    GtkWidget *label = g_object_get_data(G_OBJECT(box), "label");
    const char *rel = g_file_info_get_attribute_string(info, "f4h::rel");
    g_autofree char *shown = info_is_dir(info) ? dir_display_name(info_file(info)) : NULL;
    const char *dname = shown ? shown : g_file_info_get_display_name(info);
    if (rel && *rel) {
        g_autofree char *m = g_markup_printf_escaped("%s  <span alpha=\"50%%\">%s</span>", dname, rel);
        gtk_label_set_markup(GTK_LABEL(label), m);
    } else {
        gtk_label_set_text(GTK_LABEL(label), dname);
    }
    gtk_stack_set_visible_child_name(GTK_STACK(g_object_get_data(G_OBJECT(box), "stack")), "label");
    g_object_set_data(G_OBJECT(info), "cell", box);
}

static void name_unbind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f;
    App *a = data;
    GFileInfo *info = gtk_list_item_get_item(li);
    if (!info) return;
    if (a->renaming == info) finish_rename(a, FALSE);
    if (g_object_get_data(G_OBJECT(info), "cell") == gtk_list_item_get_child(li))
        g_object_set_data(G_OBJECT(info), "cell", NULL);
}

static void text_setup(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *l = gtk_label_new(NULL);
    gtk_widget_set_hexpand(l, TRUE);
    gtk_label_set_xalign(GTK_LABEL(l), GPOINTER_TO_INT(g_object_get_data(G_OBJECT(f), "right")) ? 1 : 0);
    gtk_box_append(GTK_BOX(box), l);
    g_object_set_data(G_OBJECT(box), "label", l);
    cell_common(data, li, box);
    gtk_list_item_set_child(li, box);
}

static void size_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f; (void)data;
    GFileInfo *info = gtk_list_item_get_item(li);
    GtkWidget *l = g_object_get_data(G_OBJECT(gtk_list_item_get_child(li)), "label");
    if (info_is_dir(info)) gtk_label_set_text(GTK_LABEL(l), "");
    else {
        g_autofree char *s = human_size(g_file_info_get_size(info));
        gtk_label_set_text(GTK_LABEL(l), s);
    }
}

static void mtime_bind(GtkSignalListItemFactory *f, GtkListItem *li, gpointer data) {
    (void)f; (void)data;
    GFileInfo *info = gtk_list_item_get_item(li);
    GtkWidget *l = g_object_get_data(G_OBJECT(gtk_list_item_get_child(li)), "label");
    g_autoptr(GDateTime) dt = g_file_info_get_modification_date_time(info);
    g_autoptr(GDateTime) local = dt ? g_date_time_to_local(dt) : NULL;
    g_autofree char *s = local ? g_date_time_format(local, "%Y-%m-%d %H:%M") : g_strdup("");
    gtk_label_set_text(GTK_LABEL(l), s);
}

static GtkColumnViewColumn *add_column(App *a, const char *title, GCallback setup, GCallback bind, GCallback unbind,
                                       SortCol sc, gboolean right) {
    GtkListItemFactory *f = gtk_signal_list_item_factory_new();
    g_object_set_data(G_OBJECT(f), "right", GINT_TO_POINTER(right));
    g_signal_connect(f, "setup", setup, a);
    g_signal_connect(f, "bind", bind, a);
    if (unbind) g_signal_connect(f, "unbind", unbind, a);
    GtkColumnViewColumn *c = gtk_column_view_column_new(title, f);
    g_object_set_data(G_OBJECT(c), "sortcol", GINT_TO_POINTER(sc));
    GtkCustomSorter *dummy = gtk_custom_sorter_new(NULL, NULL, NULL);
    gtk_column_view_column_set_sorter(c, GTK_SORTER(dummy));
    g_object_unref(dummy);
    gtk_column_view_column_set_resizable(c, TRUE);
    gtk_column_view_append_column(GTK_COLUMN_VIEW(a->view), c);
    g_object_unref(c);
    return c;
}

/* ---------------------------------------------------------------- activation / keys */
static void on_activate(GtkColumnView *v, guint pos, gpointer data) {
    (void)v;
    App *a = data;
    g_autoptr(GFileInfo) info = info_at(a, pos);
    if (!info) return;
    GFile *f = info_file(info);
    if (info_is_dir(info)) {
        navigate(a, f, TRUE, NULL);
    } else if (a->pick == PICK_FILE || a->pick == PICK_FILES) {
        GList *files = a->pick == PICK_FILES ? view_targets(a) : g_list_prepend(NULL, g_object_ref(f));
        finish_pick(a, files);
        g_list_free_full(files, g_object_unref);
    } else if (a->pick == PICK_SAVE) {
        gtk_editable_set_text(GTK_EDITABLE(a->savename), g_file_info_get_display_name(info));
        g_action_group_activate_action(G_ACTION_GROUP(a->win), "choose", NULL);
    } else if (!a->pick) {
        open_file(a, f);
    }
}

static gboolean on_view_key(GtkEventControllerKey *k, guint kv, guint code, GdkModifierType st, gpointer data) {
    (void)k; (void)code;
    App *a = data;
    if (a->renaming) return FALSE;
    GActionGroup *ag = G_ACTION_GROUP(a->win);
    gboolean ctrl = st & GDK_CONTROL_MASK;
    const char *act = NULL;
    switch (kv) {
    case GDK_KEY_BackSpace: act = "up"; break;
    case GDK_KEY_Delete: case GDK_KEY_KP_Delete: act = "trash"; break;
    case GDK_KEY_F2: act = "rename"; break;
    case GDK_KEY_slash: act = "search"; break;
    case GDK_KEY_Menu: act = "menu"; break;
    case GDK_KEY_c: case GDK_KEY_C: case GDK_KEY_Insert: if (ctrl) act = "copy"; break;
    case GDK_KEY_x: case GDK_KEY_X: if (ctrl) act = "cut"; break;
    case GDK_KEY_v: case GDK_KEY_V: if (ctrl) act = "paste"; break;
    case GDK_KEY_Escape:
        if (a->pick) act = "cancel-pick";
        else gtk_selection_model_unselect_all(GTK_SELECTION_MODEL(a->sel));
        break;
    default: break;
    }
    if (kv == GDK_KEY_Insert && (st & GDK_SHIFT_MASK)) act = "paste"; /* Omarchy's SUPER+V in terminals */
    if (kv == GDK_KEY_F10 && (st & GDK_SHIFT_MASK)) act = "menu";
    if (!act) return kv == GDK_KEY_Escape;
    g_action_group_activate_action(ag, act, NULL);
    return TRUE;
}

static void on_items_changed(GListModel *m, guint p, guint r, guint added, gpointer data) {
    (void)m; (void)p; (void)r; (void)added;
    App *a = data;
    view_try_pending(a);
    queue_status(a);
}

static void on_sel_changed(GtkSelectionModel *m, guint p, guint n, gpointer data) {
    (void)m; (void)p; (void)n;
    queue_status(data);
}

static void on_dir_error(GObject *o, GParamSpec *ps, gpointer data) {
    (void)ps;
    const GError *e = gtk_directory_list_get_error(GTK_DIRECTORY_LIST(o));
    if (e) toast(data, "%s", e->message);
}

static void on_loading(GObject *o, GParamSpec *ps, gpointer data) {
    (void)ps;
    if (!gtk_directory_list_is_loading(GTK_DIRECTORY_LIST(o))) {
        view_try_pending(data);
        if (((App *)data)->focus_first) view_focus_first(data);
        queue_status(data);
    }
}

GtkWidget *view_new(App *a) {
    settings_load(a);
    a->dirlist = gtk_directory_list_new(F4H_ATTRS, NULL); /* ours; search swaps it out of the filter model */
    gtk_directory_list_set_monitored(a->dirlist, TRUE);
    gtk_directory_list_set_io_priority(a->dirlist, G_PRIORITY_DEFAULT_IDLE);
    g_signal_connect(a->dirlist, "notify::error", G_CALLBACK(on_dir_error), a);
    g_signal_connect(a->dirlist, "notify::loading", G_CALLBACK(on_loading), a);

    a->filter = gtk_custom_filter_new(filter_func, a, NULL);
    a->filtered = gtk_filter_list_model_new(g_object_ref(G_LIST_MODEL(a->dirlist)), GTK_FILTER(a->filter));
    a->sorter = gtk_custom_sorter_new(sort_func, a, NULL);
    a->sorted = gtk_sort_list_model_new(G_LIST_MODEL(a->filtered), GTK_SORTER(a->sorter));
    a->sel = gtk_multi_selection_new(G_LIST_MODEL(a->sorted));
    g_object_ref(a->sorted); /* also referenced by the selection model */
    g_signal_connect(a->sorted, "items-changed", G_CALLBACK(on_items_changed), a);
    g_signal_connect(a->sel, "selection-changed", G_CALLBACK(on_sel_changed), a);

    a->view = gtk_column_view_new(GTK_SELECTION_MODEL(a->sel));
    gtk_column_view_set_show_column_separators(GTK_COLUMN_VIEW(a->view), FALSE);
    gtk_column_view_set_reorderable(GTK_COLUMN_VIEW(a->view), FALSE);
    gtk_column_view_set_enable_rubberband(GTK_COLUMN_VIEW(a->view), FALSE);
    g_signal_connect(a->view, "activate", G_CALLBACK(on_activate), a);

    GtkColumnViewColumn *name = add_column(a, "Name", G_CALLBACK(name_setup), G_CALLBACK(name_bind),
                                           G_CALLBACK(name_unbind), SORT_NAME, FALSE);
    a->col_name = name;
    gtk_column_view_column_set_expand(name, TRUE);
    GtkColumnViewColumn *size = add_column(a, "Size", G_CALLBACK(text_setup), G_CALLBACK(size_bind), NULL, SORT_SIZE, TRUE);
    a->col_size = size;
    gtk_column_view_column_set_fixed_width(size, 100);
    GtkColumnViewColumn *mod = add_column(a, "Modified", G_CALLBACK(text_setup), G_CALLBACK(mtime_bind), NULL, SORT_MTIME, FALSE);
    a->col_mod = mod;
    gtk_column_view_column_set_fixed_width(mod, 170);
    gtk_column_view_sort_by_column(GTK_COLUMN_VIEW(a->view), sort_column(a, a->sort_col),
                                   a->sort_desc ? GTK_SORT_DESCENDING : GTK_SORT_ASCENDING);
    g_signal_connect(gtk_column_view_get_sorter(GTK_COLUMN_VIEW(a->view)), "changed", G_CALLBACK(on_header_sort), a);
    view_set_row_size(a, a->row_size);

    GtkEventController *k = gtk_event_controller_key_new();
    g_signal_connect(k, "key-pressed", G_CALLBACK(on_view_key), a);
    gtk_widget_add_controller(a->view, k);

    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), a->view);
    gtk_widget_set_vexpand(sw, TRUE);
    add_drop_target(a, sw, view_dest);
    GtkGesture *g = gtk_gesture_click_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(g), GDK_BUTTON_SECONDARY);
    g_signal_connect(g, "pressed", G_CALLBACK(on_empty_click), a);
    gtk_widget_add_controller(sw, GTK_EVENT_CONTROLLER(g));
    return sw;
}
