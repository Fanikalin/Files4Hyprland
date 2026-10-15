/* System drag and drop: rows are drag sources (text/uri-list via GdkFileList,
 * works with any app); folders, sidebar places, tabs and path segments are
 * drop targets for files coming from this or any other app. */
#include "app.h"

typedef struct {
    App *a;
    DropDestFn fn;
} DropCtx;

static gboolean is_trash(GFile *f) {
    g_autofree char *uri = g_file_get_uri(f);
    return g_str_has_prefix(uri, "trash:");
}

/* Ctrl copies, Shift moves; otherwise move inside this app, copy from other apps. */
static GdkDragAction pick_action(GtkDropTarget *t) {
    GdkModifierType st = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(t));
    if (st & GDK_CONTROL_MASK) return GDK_ACTION_COPY;
    if (st & GDK_SHIFT_MASK) return GDK_ACTION_MOVE;
    GdkDrop *drop = gtk_drop_target_get_current_drop(t);
    gboolean internal = drop && gdk_drop_get_drag(drop) != NULL;
    return internal ? GDK_ACTION_MOVE : GDK_ACTION_COPY;
}

static gboolean on_accept(GtkDropTarget *t, GdkDrop *drop, gpointer data) {
    DropCtx *c = data;
    if (!gdk_content_formats_contain_gtype(gdk_drop_get_formats(drop), GDK_TYPE_FILE_LIST)) return FALSE;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(t));
    g_autoptr(GFile) dest = c->fn(c->a, w);
    return dest != NULL;
}

static GdkDragAction on_motion(GtkDropTarget *t, double x, double y, gpointer data) {
    (void)x; (void)y; (void)data;
    return pick_action(t);
}

static gboolean on_drop(GtkDropTarget *t, const GValue *value, double x, double y, gpointer data) {
    (void)x; (void)y;
    DropCtx *c = data;
    if (!G_VALUE_HOLDS(value, GDK_TYPE_FILE_LIST)) return FALSE;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(t));
    g_autoptr(GFile) dest = c->fn(c->a, w);
    if (!dest) return FALSE;
    GSList *files = gdk_file_list_get_files(g_value_get_boxed(value));
    GList *list = NULL;
    for (GSList *l = files; l; l = l->next) list = g_list_prepend(list, l->data);
    list = g_list_reverse(list);
    if (is_trash(dest)) trash_files(c->a, list);
    else transfer_files(c->a, list, dest, pick_action(t) == GDK_ACTION_COPY);
    g_list_free(list);
    g_slist_free(files);
    return TRUE;
}

static void free_ctx(gpointer data, GClosure *c) {
    (void)c;
    g_free(data);
}

void add_drop_target(App *a, GtkWidget *w, DropDestFn dest) {
    GtkDropTarget *t = gtk_drop_target_new(GDK_TYPE_FILE_LIST, GDK_ACTION_COPY | GDK_ACTION_MOVE);
    DropCtx *c = g_new0(DropCtx, 1);
    c->a = a;
    c->fn = dest;
    g_signal_connect_data(t, "accept", G_CALLBACK(on_accept), c, free_ctx, 0);
    g_signal_connect(t, "motion", G_CALLBACK(on_motion), c);
    g_signal_connect(t, "enter", G_CALLBACK(on_motion), c);
    g_signal_connect(t, "drop", G_CALLBACK(on_drop), c);
    gtk_widget_add_controller(w, GTK_EVENT_CONTROLLER(t));
}

/* ---------------------------------------------------------------- drag source */
static GdkContentProvider *on_prepare(GtkDragSource *s, double x, double y, gpointer data) {
    (void)x; (void)y;
    App *a = data;
    GtkWidget *w = gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(s));
    GtkListItem *li = g_object_get_data(G_OBJECT(w), "li");
    GFileInfo *info = li ? gtk_list_item_get_item(li) : NULL;
    if (!info || a->renaming) return NULL;

    GSList *files = NULL;
    guint n = 0;
    if (gtk_list_item_get_selected(li)) {
        g_autoptr(GtkBitset) set = gtk_selection_model_get_selection(GTK_SELECTION_MODEL(a->sel));
        GtkBitsetIter it;
        guint pos;
        for (gboolean ok = gtk_bitset_iter_init_first(&it, set, &pos); ok; ok = gtk_bitset_iter_next(&it, &pos)) {
            g_autoptr(GFileInfo) fi = info_at(a, pos);
            if (fi) files = g_slist_prepend(files, info_file(fi)), n++;
        }
        files = g_slist_reverse(files);
    } else {
        files = g_slist_prepend(NULL, info_file(info));
        n = 1;
    }
    if (!files) return NULL;

    /* drag icon from the system icon theme */
    GIcon *gicon = g_file_info_get_icon(info);
    if (gicon) {
        GtkIconTheme *theme = gtk_icon_theme_get_for_display(gtk_widget_get_display(w));
        g_autoptr(GtkIconPaintable) ip = gtk_icon_theme_lookup_by_gicon(theme, gicon, 32, gtk_widget_get_scale_factor(w),
                                                                        GTK_TEXT_DIR_NONE, 0);
        if (ip) gtk_drag_source_set_icon(s, GDK_PAINTABLE(ip), 8, 8);
    }
    (void)n;
    GdkFileList *fl = gdk_file_list_new_from_list(files);
    g_slist_free(files);
    GdkContentProvider *p = gdk_content_provider_new_typed(GDK_TYPE_FILE_LIST, fl);
    g_boxed_free(GDK_TYPE_FILE_LIST, fl);
    return p;
}

void add_drag_source(App *a, GtkWidget *cell) {
    GtkDragSource *s = gtk_drag_source_new();
    gtk_drag_source_set_actions(s, GDK_ACTION_COPY | GDK_ACTION_MOVE);
    g_signal_connect(s, "prepare", G_CALLBACK(on_prepare), a);
    gtk_widget_add_controller(cell, GTK_EVENT_CONTROLLER(s));
}
