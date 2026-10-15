/* Files4Hyprland — shared state and module interfaces. */
#pragma once
#include <gtk/gtk.h>

#define F4H_ATTRS                                                                                    \
    "standard::name,standard::display-name,standard::type,standard::size,standard::is-hidden,"     \
    "standard::is-backup,standard::is-symlink,standard::icon,standard::fast-content-type,time::modified"

typedef enum { PICK_NONE, PICK_FILE, PICK_FILES, PICK_DIR, PICK_SAVE } PickMode;
typedef enum { SORT_NAME, SORT_SIZE, SORT_MTIME } SortCol;

typedef struct {
    GFile *dir;
    GPtrArray *back; /* GFile* history */
    GPtrArray *fwd;
} Tab;

typedef struct {
    GtkApplication *gapp;
    GtkWindow *win;

    /* chrome */
    GtkWidget *tabbar;
    GtkWidget *btn_back, *btn_fwd, *btn_up;
    GtkWidget *loc_stack, *crumbs_sw, *crumbs, *addr, *search;
    GtkWidget *actions, *savename;
    GtkWidget *side_panel, *main_panel, *main_title;
    GtkWidget *sidebar; /* GtkListBox */
    GtkWidget *view;    /* GtkColumnView */
    GtkWidget *status, *toast_rev, *toast_label;
    GtkWidget *item_menu, *empty_menu, *side_menu;
    GHashTable *buttons; /* action name -> GtkWidget* (sensitivity) */

    /* model chain: dirlist -> filtered -> sorted -> sel */
    GtkDirectoryList *dirlist;
    GtkCustomFilter *filter;
    GtkFilterListModel *filtered;
    GtkCustomSorter *sorter;
    GtkSortListModel *sorted;
    GtkMultiSelection *sel;

    /* state */
    GPtrArray *tabs; /* Tab* */
    guint cur;
    gboolean show_hidden;
    char *query; /* casefolded search text, or NULL */
    GListStore *results;     /* recursive search results (GFileInfo) */
    GCancellable *search_cancel;
    guint search_timer;
    SortCol sort_col;
    gboolean sort_desc;
    GList *clip; /* GFile* */
    gboolean clip_cut;
    char *pending_select;
    gboolean pending_rename;
    GFileInfo *renaming;
    char *fs_info;
    guint status_idle;
    char *side_target; /* sidebar row id under the context menu */

    /* devices */
    GVolumeMonitor *vm;

    /* file chooser mode */
    PickMode pick;
    char *pick_out;
    char *pick_name;

    /* theme */
    GtkCssProvider *css;
    GFileMonitor *theme_mon;
    char accent[32];
} App;

/* theme.c */
void theme_init(App *a);

/* util.c */
char *human_size(guint64 n);
GFile *unique_child(GFile *dir, const char *name);
void toast(App *a, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
void transfer_files(App *a, GList *files, GFile *dest, gboolean copy);
void trash_files(App *a, GList *files);
void open_file(App *a, GFile *file);
GFileInfo *info_at(App *a, guint pos);
GFile *info_file(GFileInfo *info);
gboolean info_is_dir(GFileInfo *info);

/* dnd.c */
typedef GFile *(*DropDestFn)(App *a, GtkWidget *w);
void add_drop_target(App *a, GtkWidget *w, DropDestFn dest);
void add_drag_source(App *a, GtkWidget *cell);

/* sidebar.c */
GtkWidget *sidebar_new(App *a);
void sidebar_rebuild(App *a);
void sidebar_mark_current(App *a);

/* search.c */
void search_set(App *a, const char *text);

/* view.c */
GtkWidget *view_new(App *a);
GList *view_targets(App *a); /* selected files, or the focused one; free with g_list_free_full(.., g_object_unref) */
GFileInfo *view_focused(App *a, guint *pos);
void view_select_name(App *a, const char *name, gboolean rename);
void view_begin_rename(App *a);
void view_try_pending(App *a);

/* window.c */
void window_build(App *a, GFile *start);
void navigate(App *a, GFile *dir, gboolean push, const char *select);
void refresh_chrome(App *a);
void queue_status(App *a);
void open_in_new_tab(App *a, GFile *dir);
void finish_pick(App *a, GList *files);
void popup_menu(App *a, GMenuModel *model, GtkWidget *rel, double x, double y);
Tab *cur_tab(App *a);
