/* org.freedesktop.FileManager1: "Show in folder" from other apps and the OpenURI portal.
 * Runs headless (D-Bus activated); each request opens a window via the normal command line,
 * which already selects a file passed as the start path. */
#include "app.h"

static const char introspection[] =
    "<node>"
    "  <interface name='org.freedesktop.FileManager1'>"
    "    <method name='ShowFolders'><arg type='as' name='URIs' direction='in'/><arg type='s' name='StartupId' direction='in'/></method>"
    "    <method name='ShowItems'><arg type='as' name='URIs' direction='in'/><arg type='s' name='StartupId' direction='in'/></method>"
    "    <method name='ShowItemProperties'><arg type='as' name='URIs' direction='in'/><arg type='s' name='StartupId' direction='in'/></method>"
    "  </interface>"
    "</node>";

#define IDLE_EXIT_SECONDS 60

static GMainLoop *loop;
static guint idle_id;

static gboolean on_idle_exit(gpointer data) {
    (void)data;
    idle_id = 0;
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static void touch(void) {
    if (idle_id) g_source_remove(idle_id);
    idle_id = g_timeout_add_seconds(IDLE_EXIT_SECONDS, on_idle_exit, NULL);
}

static void open_window(const char *uri, const char *startup_id) {
    g_autofree char *exe = g_file_read_link("/proc/self/exe", NULL);
    char *argv[] = {exe ? exe : "files4hyprland", (char *)uri, NULL};
    g_auto(GStrv) env = g_get_environ();
    if (startup_id && *startup_id) {
        env = g_environ_setenv(env, "XDG_ACTIVATION_TOKEN", startup_id, TRUE);
        env = g_environ_setenv(env, "DESKTOP_STARTUP_ID", startup_id, TRUE);
    }
    g_autoptr(GError) err = NULL;
    if (!g_spawn_async(NULL, argv, env, exe ? 0 : G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &err))
        g_warning("FileManager1: %s", err->message);
}

static void on_method(GDBusConnection *conn, const char *sender, const char *path, const char *iface,
                      const char *method, GVariant *params, GDBusMethodInvocation *inv, gpointer data) {
    (void)conn; (void)sender; (void)path; (void)iface; (void)data;
    g_autofree const char **uris = NULL;
    const char *startup_id = NULL;
    g_variant_get(params, "(^a&s&s)", &uris, &startup_id);
    gboolean folders = g_str_equal(method, "ShowFolders");
    /* one window per folder; the command line selects a single item, so the first one wins */
    g_autoptr(GHashTable) seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (int i = 0; uris && uris[i]; i++) {
        g_autoptr(GFile) f = g_file_new_for_uri(uris[i]);
        g_autoptr(GFile) parent = folders ? NULL : g_file_get_parent(f);
        g_autofree char *key = g_file_get_uri(parent ? parent : f);
        if (g_hash_table_contains(seen, key)) continue;
        g_hash_table_add(seen, g_steal_pointer(&key));
        open_window(uris[i], startup_id);
    }
    g_dbus_method_invocation_return_value(inv, NULL);
    touch();
}

static void on_bus(GDBusConnection *conn, const char *name, gpointer data) {
    (void)name; (void)data;
    static const GDBusInterfaceVTable vtable = {on_method, NULL, NULL, {0}};
    g_autoptr(GDBusNodeInfo) node = g_dbus_node_info_new_for_xml(introspection, NULL);
    g_autoptr(GError) err = NULL;
    if (!g_dbus_connection_register_object(conn, "/org/freedesktop/FileManager1", node->interfaces[0], &vtable,
                                           NULL, NULL, &err)) {
        g_warning("FileManager1: %s", err->message);
        g_main_loop_quit(loop);
    }
}

static void on_name_lost(GDBusConnection *conn, const char *name, gpointer data) {
    (void)conn; (void)data;
    g_warning("FileManager1: could not own %s", name);
    g_main_loop_quit(loop);
}

int fm1_service_run(void) {
    loop = g_main_loop_new(NULL, FALSE);
    guint owner = g_bus_own_name(G_BUS_TYPE_SESSION, "org.freedesktop.FileManager1", G_BUS_NAME_OWNER_FLAGS_DO_NOT_QUEUE,
                                 on_bus, NULL, on_name_lost, NULL, NULL);
    touch();
    g_main_loop_run(loop);
    g_bus_unown_name(owner);
    g_main_loop_unref(loop);
    return 0;
}
