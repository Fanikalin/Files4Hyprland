/* Theme: colors from the current Omarchy theme, shape (rounding, border width)
 * from Hyprland, font from fontconfig's "monospace". Re-applied live when the
 * Omarchy theme changes or Hyprland reloads its config. */
#include "app.h"
#include <string.h>

typedef struct {
    char *bg, *surface, *panel, *fg, *muted, *accent, *red, *green;
    int rounding, border;
} Look;

static char *dup_or(GHashTable *c, const char *k, const char *k2, const char *def) {
    const char *v = g_hash_table_lookup(c, k);
    if (!v && k2) v = g_hash_table_lookup(c, k2);
    return g_strdup(v ? v : def);
}

static char *colors_path(void) {
    return g_build_filename(g_get_home_dir(), ".local/state/omarchy/current/theme/colors.toml", NULL);
}

/* Prefer omarchy-theme-color (resolves derived shades); fall back to parsing
 * simple `key = "value"` lines of colors.toml. */
static GHashTable *read_colors(void) {
    GHashTable *c = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_autofree char *path = colors_path();
    g_autofree char *out = NULL;
    const char *argv[] = {"omarchy-theme-color", "--file", path, "--all", NULL};
    if (g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                     NULL, NULL, &out, NULL, NULL, NULL) && out) {
        g_auto(GStrv) lines = g_strsplit(out, "\n", -1);
        for (char **l = lines; *l; l++) {
            char *tab = strchr(*l, '\t');
            if (!tab) continue;
            *tab = 0;
            g_hash_table_insert(c, g_strdup(*l), g_strdup(tab + 1));
        }
    }
    if (g_hash_table_size(c) == 0) {
        g_autofree char *txt = NULL;
        if (g_file_get_contents(path, &txt, NULL, NULL)) {
            g_auto(GStrv) lines = g_strsplit(txt, "\n", -1);
            for (char **l = lines; *l; l++) {
                g_auto(GStrv) kv = g_strsplit(*l, "=", 2);
                if (!kv[0] || !kv[1]) continue;
                char *k = g_strstrip(kv[0]), *v = g_strstrip(kv[1]);
                if (*v == '"') {
                    v++;
                    char *q = strchr(v, '"');
                    if (q) *q = 0;
                }
                if (*k && *v == '#') g_hash_table_insert(c, g_strdup(k), g_strdup(v));
            }
        }
    }
    return c;
}

static int hypr_int(const char *opt, int def) {
    g_autofree char *out = NULL;
    const char *argv[] = {"hyprctl", "-j", "getoption", opt, NULL};
    if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL,
                      NULL, NULL, &out, NULL, NULL, NULL) || !out)
        return def;
    const char *p = strstr(out, "\"int\":");
    return p ? atoi(p + 6) : def;
}

static void apply(App *a) {
    g_autoptr(GHashTable) c = read_colors();
    Look l = {
        .bg = dup_or(c, "background", NULL, "#101010"),
        .fg = dup_or(c, "foreground", NULL, "#e0e0e0"),
        .muted = dup_or(c, "muted", "dark_foreground", "#808080"),
        .accent = dup_or(c, "accent", "blue", "#8d8d8d"),
        .red = dup_or(c, "red", NULL, "#e06c75"),
        .green = dup_or(c, "green", NULL, "#98c379"),
    };
    l.surface = dup_or(c, "dark_background", NULL, l.bg);
    l.panel = dup_or(c, "selection", "lighter_background", "#2a2a2a");
    l.rounding = CLAMP(hypr_int("decoration:rounding", 8), 0, 24);
    l.border = CLAMP(hypr_int("general:border_size", 1), 1, 3);
    g_strlcpy(a->accent, l.accent, sizeof a->accent);

    int r = l.rounding, rs = MIN(r, 6);
    g_autofree char *css = g_strdup_printf(
        "window.f4h { background: %1$s; color: %4$s; font-family: monospace; }\n"
        "window.f4h * { -gtk-icon-shadow: none; }\n"
        ".f4h .tabbar { background: %2$s; padding: 2px 6px 0 6px; }\n"
        ".f4h .tab { border-radius: %9$dpx %9$dpx 0 0; padding: 0 2px 0 8px; color: %5$s; }\n"
        ".f4h .tab.active { background: %3$s; color: %6$s; font-weight: bold; }\n"
        ".f4h .tab button { padding: 1px 4px; min-height: 0; min-width: 0; }\n"
        ".f4h .panel { border: %10$dpx solid alpha(%6$s, 0.45); border-radius: %8$dpx; background: %1$s; }\n"
        ".f4h .panel:focus-within, .f4h .panel:hover { border-color: %6$s; }\n"
        ".f4h .panel-title { color: %6$s; padding: 0 6px; }\n"
        ".f4h button { background: none; border: none; box-shadow: none; outline: none; color: %4$s;"
        "  padding: 1px 6px; min-height: 0; min-width: 0; border-radius: %9$dpx; text-shadow: none; }\n"
        ".f4h button:hover { background: alpha(%6$s, 0.18); }\n"
        ".f4h button:active, .f4h button:checked { background: alpha(%6$s, 0.3); }\n"
        ".f4h button:disabled { color: alpha(%5$s, 0.5); }\n"
        ".f4h button.glyph { color: %6$s; font-weight: bold; }\n"
        ".f4h .actions button { background: %3$s; color: %6$s; margin-right: 4px; padding: 1px 8px; }\n"
        ".f4h .actions button:hover { background: alpha(%6$s, 0.3); }\n"
        ".f4h .actions button.suggested { background: %6$s; color: %1$s; font-weight: bold; }\n"
        ".f4h .crumb { padding: 1px 4px; }\n"
        ".f4h .crumb.last { font-weight: bold; }\n"
        ".f4h .sep { color: %5$s; }\n"
        ".f4h entry, .f4h text { background: %3$s; color: %4$s; border: none; box-shadow: none; outline: none;"
        "  border-radius: %9$dpx; min-height: 22px; caret-color: %6$s; }\n"
        ".f4h entry:focus-within { box-shadow: inset 0 0 0 1px %6$s; }\n"
        ".f4h entry selection, .f4h text selection { background: alpha(%6$s, 0.45); color: %4$s; }\n"
        ".f4h columnview, .f4h columnview > listview { background: %1$s; color: %4$s; }\n"
        ".f4h columnview > header > button { background: %2$s; color: %6$s; font-weight: bold; border: none;"
        "  border-radius: 0; padding: 2px 8px; }\n"
        ".f4h columnview > header > button sort-indicator { color: %6$s; }\n"
        ".f4h columnview row { padding: 0; border-radius: 0; }\n"
        ".f4h columnview row:hover { background: none; box-shadow: inset 0 0 0 1px alpha(%6$s, 0.35); }\n"
        ".f4h columnview row:selected { background: alpha(%6$s, 0.28); color: %4$s; }\n"
        ".f4h columnview row:selected:focus-within { background: alpha(%6$s, 0.38); }\n"
        ".f4h columnview row cell { padding: 1px 8px; }\n"
        ".f4h .glyph-dir { color: %6$s; font-weight: bold; }\n"
        ".f4h .dim { color: %5$s; }\n"
        ".f4h *:drop(active) { background: alpha(%6$s, 0.35); box-shadow: inset 0 0 0 1px %6$s; }\n"
        ".f4h listbox, .f4h scrolledwindow, .f4h viewport, .f4h stack { background: none; color: %4$s; }\n"
        ".f4h listbox row.activatable:hover, .f4h listbox row:selected { background: none; }\n"
        ".f4h .panel listbox, .f4h .panel listbox > row { background-color: transparent; }\n"
        ".f4h listbox row { padding: 1px 6px; border-radius: 0; }\n"
        ".f4h listbox row:hover { background: none; box-shadow: inset 0 0 0 1px alpha(%6$s, 0.35); }\n"
        ".f4h listbox row.current { box-shadow: inset 3px 0 %6$s; color: %6$s; font-weight: bold; }\n"
        ".f4h listbox row.header { color: %5$s; font-weight: bold; padding-top: 6px; }\n"
        ".f4h listbox row.header:hover { box-shadow: none; }\n"
        ".f4h .status { color: %5$s; padding: 2px 10px; }\n"
        ".f4h .toast { background: %3$s; color: %4$s; border: 1px solid %6$s; border-radius: %9$dpx;"
        "  padding: 4px 12px; margin: 10px; }\n"
        ".f4h .toast.error { border-color: %7$s; }\n"
        ".f4h scrollbar { background: none; border: none; }\n"
        ".f4h scrollbar slider { background: alpha(%6$s, 0.5); min-width: 4px; min-height: 4px; border-radius: 2px; }\n"
        ".f4h popover > contents { background: %2$s; color: %4$s; border: 1px solid %6$s; border-radius: %9$dpx;"
        "  padding: 2px; }\n"
        ".f4h popover modelbutton { padding: 3px 10px; border-radius: %9$dpx; }\n"
        ".f4h popover modelbutton:hover { background: alpha(%6$s, 0.3); }\n"
        ".f4h popover separator { background: alpha(%5$s, 0.4); margin: 2px 4px; }\n"
        ".f4h tooltip { background: %2$s; color: %4$s; border: 1px solid alpha(%6$s, 0.5); }\n",
        l.bg, l.surface, l.panel, l.fg, l.muted, l.accent, l.red, r, rs, l.border);
    gtk_css_provider_load_from_string(a->css, css);

    g_free(l.bg); g_free(l.surface); g_free(l.panel); g_free(l.fg);
    g_free(l.muted); g_free(l.accent); g_free(l.red); g_free(l.green);
}

static guint apply_idle;

static gboolean apply_cb(gpointer data) {
    apply_idle = 0;
    apply((App *)data);
    return G_SOURCE_REMOVE;
}

static void schedule(App *a) {
    if (apply_idle) g_source_remove(apply_idle);
    apply_idle = g_timeout_add(250, apply_cb, a);
}

static void on_theme_changed(GFileMonitor *m, GFile *f, GFile *o, GFileMonitorEvent ev, gpointer data) {
    (void)m; (void)f; (void)o; (void)ev;
    schedule(data);
}

/* Hyprland event socket: re-read rounding/border when the config reloads. */
static void read_hypr_line(GObject *src, GAsyncResult *res, gpointer data);

static void watch_line(GDataInputStream *in, App *a) {
    g_data_input_stream_read_line_async(in, G_PRIORITY_LOW, NULL, read_hypr_line, a);
}

static void read_hypr_line(GObject *src, GAsyncResult *res, gpointer data) {
    GDataInputStream *in = G_DATA_INPUT_STREAM(src);
    g_autofree char *line = g_data_input_stream_read_line_finish(in, res, NULL, NULL);
    if (!line) return; /* socket closed */
    if (g_str_has_prefix(line, "configreloaded")) schedule(data);
    watch_line(in, data);
}

static void watch_hyprland(App *a) {
    const char *sig = g_getenv("HYPRLAND_INSTANCE_SIGNATURE");
    const char *rt = g_getenv("XDG_RUNTIME_DIR");
    if (!sig || !rt) return;
    g_autofree char *path = g_build_filename(rt, "hypr", sig, ".socket2.sock", NULL);
    g_autoptr(GSocketAddress) addr = g_unix_socket_address_new(path);
    g_autoptr(GSocketClient) client = g_socket_client_new();
    GSocketConnection *conn = g_socket_client_connect(client, G_SOCKET_CONNECTABLE(addr), NULL, NULL);
    if (!conn) return;
    GInputStream *raw = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    GDataInputStream *in = g_data_input_stream_new(raw);
    g_object_set_data_full(G_OBJECT(in), "conn", conn, g_object_unref);
    watch_line(in, a);
}

void theme_init(App *a) {
    a->css = gtk_css_provider_new();
    gtk_style_context_add_provider_for_display(gdk_display_get_default(), GTK_STYLE_PROVIDER(a->css),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    apply(a);
    /* the theme dir is swapped on theme change; watch its parent */
    g_autofree char *dir = g_build_filename(g_get_home_dir(), ".local/state/omarchy/current", NULL);
    g_autoptr(GFile) f = g_file_new_for_path(dir);
    a->theme_mon = g_file_monitor_directory(f, G_FILE_MONITOR_WATCH_MOVES, NULL, NULL);
    if (a->theme_mon) g_signal_connect(a->theme_mon, "changed", G_CALLBACK(on_theme_changed), a);
    watch_hyprland(a);
}
