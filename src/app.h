/* Files4Hyprland — shared state and module interfaces. */
#pragma once
#include <gtk/gtk.h>

#define F4H_ATTRS                                                                                    \
    "standard::name,standard::display-name,standard::type,standard::size,standard::is-hidden,"     \
    "standard::is-backup,standard::is-symlink,standard::icon,standard::fast-content-type,time::modified"

/* undoable operations: pairs of src -> dst files (dst NULL for trash) */
typedef enum { OP_RENAME, OP_MOVE, OP_COPY, OP_TRASH, OP_CREATE } OpKind;
typedef struct {
    OpKind kind;
    gboolean is_dir; /* OP_CREATE: folder or file */
    GPtrArray *src;  /* GFile* (NULL entries allowed) */
    GPtrArray *dst;
} Op;

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
    GtkWidget *tabbar;     /* inner GtkBox with tab buttons */
    GtkWidget *tabbar_sw;  /* GtkScrolledWindow wrapping tabbar */
    GtkWidget *btn_back, *btn_fwd, *btn_up;
    GtkWidget *loc_stack, *crumbs_sw, *crumbs, *addr, *search;
    GtkWidget *actions, *savename;
    GtkWidget *side_panel, *main_panel, *main_title;
    GtkWidget *sidebar; /* GtkListBox */
    GtkWidget *view;    /* GtkColumnView */
    GtkColumnViewColumn *col_name, *col_size, *col_mod;
    GtkWidget *grid;       /* GtkGridView: tiles mode, same model as the column view */
    GtkWidget *view_stack; /* details | tiles */
    GtkWidget *pages;      /* files | network */
    GtkWidget *net_page;
    gboolean tiles;
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
    GHashTable *dir_views; /* URI -> row_size string, per-directory view override */
    char *query; /* casefolded search text, or NULL */
    GListStore *results;     /* recursive search results (GFileInfo) */
    GCancellable *search_cancel;
    guint search_timer;
    SortCol sort_col;
    char row_size[16]; /* small | medium | large */
    gboolean sort_desc;
    GList *clip; /* GFile* */
    gboolean clip_cut;
    char *pending_select;
    gboolean pending_rename;
    gboolean focus_first; /* after loading a folder: select its first row and focus the list */
    GFileInfo *renaming;
    guint status_idle;
    char *side_target; /* sidebar row id under the context menu */
    GQueue undo, redo; /* Op* */

    /* devices */
    GVolumeMonitor *vm;

    /* file chooser mode */
    PickMode pick;
    char *pick_out;
    char *pick_name;

    /* theme */
    GtkCssProvider *css;
    GFileMonitor *theme_mon;
    char accent[32], accent2[32], red[32];
    gint64 theme_sig;
} App;

/* theme.c */
void theme_init(App *a);
void theme_check(App *a);

/* util.c */
char *human_size(guint64 n);
gboolean fs_move(GFile *src, GFile *dst, GError **err);
gboolean fs_copy(GFile *src, GFile *dst, GError **err);
gboolean fs_delete(GFile *f, GError **err);
GFile *unique_child(GFile *dir, const char *name);
void toast(App *a, const char *fmt, ...) G_GNUC_PRINTF(2, 3);
void transfer_files(App *a, GList *files, GFile *dest, gboolean copy);
void trash_files(App *a, GList *files);
void open_file(App *a, GFile *file);
GFileInfo *info_at(App *a, guint pos);
GFile *info_file(GFileInfo *info);
gboolean info_is_dir(GFileInfo *info);
const char *tr(const char *english);   /* UI words of the sidebar, translated to the system language */
char *dir_display_name(GFile *f);      /* translated name of Home / Downloads / ... or NULL */
void run_script(App *a, GFile *file);
void settings_load(App *a);
void settings_save(App *a);
gboolean name_is_archive(const char *filename); /* check by extension */
void extract_archive(App *a, GFile *archive, gboolean into_folder);

/* dnd.c */
typedef GFile *(*DropDestFn)(App *a, GtkWidget *w);
void add_drop_target(App *a, GtkWidget *w, DropDestFn dest);
void add_drag_source(App *a, GtkWidget *cell);

/* sidebar.c */
GtkWidget *sidebar_new(App *a);
void sidebar_rebuild(App *a);
void sidebar_mark_current(App *a);
gboolean sidebar_is_pinned(GFile *f);
void sidebar_pin(App *a, GFile *f);
void sidebar_unpin(App *a, GFile *f);
void sidebar_style(App *a); /* disk colors follow the theme */

/* thumb.c: previews for tiles (images, videos, PDFs, text); the tile must carry "prev", "pic", "text" data */
void thumb_load(GtkWidget *tile, GFileInfo *info, int size);

/* net.c: the Network page and opening remote locations */
GtkWidget *network_new(App *a);
void network_scan(App *a);
gboolean is_network(GFile *f);
void net_open(App *a, GFile *f);   /* mounts if needed, then navigates */
guint network_count(void);

/* fm1.c: headless org.freedesktop.FileManager1 service ("Show in folder" from other apps) */
int fm1_service_run(void);

/* ext.c: entries other programs add to the context menu */
GMenuModel *ext_menu_build(App *a, GList *targets); /* NULL when there is nothing to add */
void ext_run(App *a, guint index);

/* undo.c */
Op *op_new(OpKind kind, gboolean is_dir);
void op_add(Op *op, GFile *src, GFile *dst);
void op_free(Op *op);
void undo_push(App *a, Op *op);
void undo_run(App *a, gboolean redo);
void undo_update(App *a);

/* search.c */
void search_set(App *a, const char *text);

/* view.c */
GtkWidget *view_new(App *a);
GList *view_targets(App *a); /* selected files, or the focused one; free with g_list_free_full(.., g_object_unref) */
GFileInfo *view_focused(App *a, guint *pos);
void view_select_name(App *a, const char *name, gboolean rename);
void view_begin_rename(App *a);
void view_try_pending(App *a);
void view_focus_first(App *a);
void view_sync_sort_actions(App *a);
void view_focus(App *a);
void view_set_row_size(App *a, const char *size); /* user action: apply + save for current dir + update default */
void view_apply_size(App *a, const char *size);   /* programmatic: apply without saving */
void view_set_sort(App *a, SortCol col, gboolean desc);

/* window.c */
void window_build(App *a, GFile *start);
void navigate(App *a, GFile *dir, gboolean push, const char *select);
void refresh_chrome(App *a);
void queue_status(App *a);
void open_in_new_tab(App *a, GFile *dir);
void finish_pick(App *a, GList *files);
void popup_menu(App *a, GMenuModel *model, GtkWidget *rel, double x, double y);
Tab *cur_tab(App *a);
