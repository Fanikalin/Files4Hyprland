/* Network page: scans the local subnets for machines that offer SMB, SFTP, FTP or NFS,
 * lists them with address and protocol, and connects to servers typed in by hand
 * (kept in ~/.config/files4hyprland/servers). Mounting goes through gvfs. */
#include "app.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct { int port; const char *scheme, *label; } PROTOS[] = {
    {445, "smb", "SMB"}, {22, "sftp", "SFTP"}, {21, "ftp", "FTP"}, {2049, "nfs", "NFS"},
};
#define NPROTO G_N_ELEMENTS(PROTOS)
#define MAX_HOSTS 1022
#define IN_FLIGHT 200
#define PROBE_MS 450

typedef struct {
    App *a;
    GCancellable *cancel;
    GPtrArray *targets; /* char* "ip" */
    guint next;         /* next (host, proto) pair to probe */
    guint pending, done_hosts;
    GSocketClient *client;
    GHashTable *names;  /* ip -> resolved name */
    char *subnets;
} Scan;

static GtkWidget *list, *status, *entry;
static App *app_ref;
static Scan *scan;
static GPtrArray *servers; /* manual URIs */
static guint found_count;

gboolean is_network(GFile *f) { return f && g_file_has_uri_scheme(f, "network"); }
guint network_count(void) { return found_count; }

/* ---------------------------------------------------------------- saved servers */
static char *servers_path(void) { return g_build_filename(g_get_user_config_dir(), "files4hyprland", "servers", NULL); }

static void servers_load(void) {
    if (servers) return;
    servers = g_ptr_array_new_with_free_func(g_free);
    g_autofree char *txt = NULL, *p = servers_path();
    if (!g_file_get_contents(p, &txt, NULL, NULL)) return;
    g_auto(GStrv) lines = g_strsplit(txt, "\n", -1);
    for (char **l = lines; *l; l++)
        if (**l) g_ptr_array_add(servers, g_strdup(*l));
}

static void servers_save(void) {
    g_autofree char *p = servers_path(), *dir = g_path_get_dirname(p);
    g_mkdir_with_parents(dir, 0755);
    GString *s = g_string_new(NULL);
    for (guint i = 0; i < servers->len; i++) g_string_append_printf(s, "%s\n", (char *)servers->pdata[i]);
    g_file_set_contents(p, s->str, -1, NULL);
    g_string_free(s, TRUE);
}

/* ---------------------------------------------------------------- rows */
static void rebuild_manual(void);

static void on_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer data) {
    const char *uri = g_object_get_data(G_OBJECT(row), "uri");
    if (!uri) return;
    g_autoptr(GFile) f = g_file_new_for_uri(uri);
    net_open(app_ref, f);
}

static void on_remove(GtkButton *b, gpointer data) {
    const char *uri = g_object_get_data(G_OBJECT(b), "uri");
    for (guint i = 0; i < servers->len; i++)
        if (g_str_equal(servers->pdata[i], uri)) {
            g_ptr_array_remove_index(servers, i);
            break;
        }
    servers_save();
    rebuild_manual();
}

static const char *proto_label(const char *uri) {
    for (guint i = 0; i < NPROTO; i++)
        if (g_str_has_prefix(uri, PROTOS[i].scheme) && uri[strlen(PROTOS[i].scheme)] == ':') return PROTOS[i].label;
    const char *c = strstr(uri, "://");
    static char other[16];
    if (!c) return "?";
    g_strlcpy(other, uri, MIN((size_t)(c - uri) + 1, sizeof other));
    for (char *p = other; *p; p++) *p = g_ascii_toupper(*p);
    return other;
}

static GtkWidget *row_new(const char *name, const char *uri, gboolean manual) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *g = gtk_label_new(manual ? "\uf233" : "\uf0e8");
    gtk_widget_add_css_class(g, "glyph-dir");
    gtk_widget_set_size_request(g, 20, -1);
    GtkWidget *n = gtk_label_new(name);
    gtk_label_set_xalign(GTK_LABEL(n), 0);
    gtk_widget_set_hexpand(n, TRUE);
    gtk_label_set_ellipsize(GTK_LABEL(n), PANGO_ELLIPSIZE_END);
    g_autofree char *addr = g_strdup(strstr(uri, "://") ? strstr(uri, "://") + 3 : uri);
    GtkWidget *ad = gtk_label_new(addr);
    gtk_widget_add_css_class(ad, "dim");
    gtk_widget_set_size_request(ad, 220, -1);
    gtk_label_set_xalign(GTK_LABEL(ad), 0);
    GtkWidget *pr = gtk_label_new(proto_label(uri));
    gtk_widget_set_size_request(pr, 60, -1);
    gtk_label_set_xalign(GTK_LABEL(pr), 0);
    gtk_box_append(GTK_BOX(box), g);
    gtk_box_append(GTK_BOX(box), n);
    gtk_box_append(GTK_BOX(box), ad);
    gtk_box_append(GTK_BOX(box), pr);
    GtkWidget *row = gtk_list_box_row_new();
    gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), box);
    g_object_set_data_full(G_OBJECT(row), "uri", g_strdup(uri), g_free);
    g_object_set_data(G_OBJECT(row), "name-label", n);
    g_object_set_data(G_OBJECT(row), "manual", GINT_TO_POINTER(manual));
    if (manual) {
        GtkWidget *x = gtk_button_new_with_label("×");
        gtk_widget_set_tooltip_text(x, "Forget this server");
        g_object_set_data_full(G_OBJECT(x), "uri", g_strdup(uri), g_free);
        g_signal_connect(x, "clicked", G_CALLBACK(on_remove), NULL);
        gtk_box_append(GTK_BOX(box), x);
    }
    return row;
}

static void remove_rows(gboolean manual) {
    GtkWidget *c = gtk_widget_get_first_child(list), *next;
    for (; c; c = next) {
        next = gtk_widget_get_next_sibling(c);
        if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(c), "manual")) == manual) gtk_list_box_remove(GTK_LIST_BOX(list), c);
    }
}

static void rebuild_manual(void) {
    servers_load();
    remove_rows(TRUE);
    for (guint i = 0; i < servers->len; i++) {
        const char *uri = servers->pdata[i];
        g_autofree char *name = g_strdup(strstr(uri, "://") ? strstr(uri, "://") + 3 : uri);
        gtk_list_box_insert(GTK_LIST_BOX(list), row_new(name, uri, TRUE), 0);
    }
}

static GtkWidget *find_row(const char *uri) {
    for (GtkWidget *c = gtk_widget_get_first_child(list); c; c = gtk_widget_get_next_sibling(c)) {
        const char *u = g_object_get_data(G_OBJECT(c), "uri");
        if (u && g_str_equal(u, uri)) return c;
    }
    return NULL;
}

static void update_status(const char *prefix) {
    g_autofree char *t = g_strdup_printf("%s%u found", prefix, found_count);
    gtk_label_set_text(GTK_LABEL(status), t);
}

/* ---------------------------------------------------------------- scanning */
static void scan_free(Scan *s) {
    g_object_unref(s->cancel);
    g_ptr_array_unref(s->targets);
    g_object_unref(s->client);
    g_hash_table_unref(s->names);
    g_free(s->subnets);
    g_free(s);
}

static void pump(Scan *s);

typedef struct {
    char *ip;
    GCancellable *cancel;
} Lookup;

static void on_resolved(GObject *src, GAsyncResult *res, gpointer data) {
    Lookup *lk = data;
    const char *ip = lk->ip;
    g_autofree char *name = g_resolver_lookup_by_address_finish(G_RESOLVER(src), res, NULL);
    if (name && !g_cancellable_is_cancelled(lk->cancel)) {
        g_autofree char *host = g_strdup(name);
        char *dot = strchr(host, '.');
        if (dot && !g_hostname_is_ip_address(host)) *dot = 0;
        for (guint i = 0; i < NPROTO; i++) {
            g_autofree char *uri = g_strdup_printf("%s://%s/", PROTOS[i].scheme, ip);
            GtkWidget *row = find_row(uri);
            if (row) gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(row), "name-label")), host);
        }
    }
    g_object_unref(lk->cancel);
    g_free(lk->ip);
    g_free(lk);
}

typedef struct {
    Scan *s;
    guint host, proto;
    GCancellable *pc; /* cancelled after PROBE_MS: a closed or filtered port must not hold up the scan */
    guint tid;
} Probe;

static gboolean probe_timeout(gpointer data) {
    Probe *p = data;
    p->tid = 0;
    g_cancellable_cancel(p->pc);
    return G_SOURCE_REMOVE;
}

static void on_connected(GObject *src, GAsyncResult *res, gpointer data) {
    Probe *p = data;
    Scan *s = p->s;
    g_autoptr(GSocketConnection) c = g_socket_client_connect_to_host_finish(G_SOCKET_CLIENT(src), res, NULL);
    s->pending--;
    if (p->tid) g_source_remove(p->tid);
    if (c && !g_cancellable_is_cancelled(s->cancel)) {
        g_io_stream_close(G_IO_STREAM(c), NULL, NULL);
        const char *ip = s->targets->pdata[p->host];
        g_autofree char *uri = g_strdup_printf("%s://%s/", PROTOS[p->proto].scheme, ip);
        if (!find_row(uri)) {
            GtkWidget *row = row_new(ip, uri, FALSE);
            gtk_list_box_append(GTK_LIST_BOX(list), row);
            found_count++;
            if (!g_hash_table_contains(s->names, ip)) {
                g_hash_table_add(s->names, g_strdup(ip));
                g_autoptr(GInetAddress) ia = g_inet_address_new_from_string(ip);
                Lookup *lk = g_new0(Lookup, 1);
                lk->ip = g_strdup(ip);
                lk->cancel = g_object_ref(s->cancel);
                g_autoptr(GResolver) r = g_resolver_get_default();
                g_resolver_lookup_by_address_async(r, ia, s->cancel, on_resolved, lk);
            }
            update_status("Scanning… ");
        }
    }
    g_object_unref(p->pc);
    g_object_unref(src);
    g_free(p);
    pump(s);
}

static void pump(Scan *s) {
    guint total = s->targets->len * NPROTO;
    while (!g_cancellable_is_cancelled(s->cancel) && s->pending < IN_FLIGHT && s->next < total) {
        guint code = s->next++;
        Probe *p = g_new0(Probe, 1);
        p->s = s;
        p->host = code / NPROTO;
        p->proto = code % NPROTO;
        p->pc = g_cancellable_new();
        GSocketClient *c = g_socket_client_new();
        s->pending++;
        p->tid = g_timeout_add(PROBE_MS, probe_timeout, p);
        g_socket_client_connect_to_host_async(c, s->targets->pdata[p->host], PROTOS[p->proto].port, p->pc, on_connected, p);
    }
    if (s->pending == 0 && (s->next >= total || g_cancellable_is_cancelled(s->cancel))) {
        if (!g_cancellable_is_cancelled(s->cancel)) {
            g_autofree char *t = g_strdup_printf("%u found  ·  scanned %s", found_count, s->subnets ? s->subnets : "");
            gtk_label_set_text(GTK_LABEL(status), t);
        }
        if (scan == s) scan = NULL;
        scan_free(s);
    }
}

/* local IPv4 networks from `ip`: every host of the subnet (at most a /22, wider ones are cut to the /24 around us) */
static void collect_targets(Scan *s) {
    g_autofree char *out = NULL;
    const char *argv[] = {"ip", "-4", "-o", "addr", "show", "scope", "global", NULL};
    if (!g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDERR_TO_DEV_NULL, NULL, NULL, &out, NULL, NULL, NULL) || !out)
        return;
    GString *nets = g_string_new(NULL);
    g_auto(GStrv) lines = g_strsplit(out, "\n", -1);
    for (char **l = lines; *l; l++) {
        g_auto(GStrv) f = g_strsplit_set(*l, " \t", -1);
        const char *ifname = NULL, *cidr = NULL;
        int k = 0;
        for (char **t = f; *t; t++) {
            if (!**t) continue;
            if (k == 1) ifname = *t;
            if (g_str_equal(*t, "inet") && t[1]) cidr = t[1];
            k++;
        }
        if (!ifname || !cidr) continue;
        if (g_str_has_prefix(ifname, "docker") || g_str_has_prefix(ifname, "veth") || g_str_has_prefix(ifname, "br-") ||
            g_str_has_prefix(ifname, "virbr") || g_str_has_prefix(ifname, "lo"))
            continue;
        g_auto(GStrv) parts = g_strsplit(cidr, "/", 2);
        if (!parts[0] || !parts[1]) continue;
        guint a, b, c, d;
        if (sscanf(parts[0], "%u.%u.%u.%u", &a, &b, &c, &d) != 4) continue;
        int bits = atoi(parts[1]);
        if (bits < 22) bits = 24;
        if (bits > 30) continue;
        guint32 ip = (a << 24) | (b << 16) | (c << 8) | d;
        guint32 mask = ~((1u << (32 - bits)) - 1);
        guint32 net = ip & mask, bc = net | ~mask;
        for (guint32 h = net + 1; h < bc && s->targets->len < MAX_HOSTS; h++) {
            if (h == ip) continue;
            g_ptr_array_add(s->targets, g_strdup_printf("%u.%u.%u.%u", h >> 24, (h >> 16) & 255, (h >> 8) & 255, h & 255));
        }
        g_string_append_printf(nets, "%s%u.%u.%u.0/%d", nets->len ? ", " : "", net >> 24, (net >> 16) & 255, (net >> 8) & 255, bits);
    }
    s->subnets = g_string_free(nets, FALSE);
}

void network_scan(App *a) {
    if (!list) return;
    if (scan) {
        g_cancellable_cancel(scan->cancel);
        scan = NULL; /* its last callback frees it */
    }
    remove_rows(FALSE);
    found_count = 0;
    Scan *s = g_new0(Scan, 1);
    s->a = a;
    s->cancel = g_cancellable_new();
    s->targets = g_ptr_array_new_with_free_func(g_free);
    s->client = g_socket_client_new();
    s->names = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    collect_targets(s);
    if (!s->targets->len) {
        gtk_label_set_text(GTK_LABEL(status), "No network connection found; enter a server address below");
        scan_free(s);
        return;
    }
    scan = s;
    update_status("Scanning… ");
    pump(s);
}

/* ---------------------------------------------------------------- opening */
typedef struct {
    App *a;
    GFile *f;
} OpenReq;

static void mount_cb(GObject *src, GAsyncResult *res, gpointer data) {
    OpenReq *r = data;
    g_autoptr(GError) err = NULL;
    if (g_file_mount_enclosing_volume_finish(G_FILE(src), res, &err) || g_error_matches(err, G_IO_ERROR, G_IO_ERROR_ALREADY_MOUNTED))
        navigate(r->a, r->f, TRUE, NULL);
    else if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_FAILED_HANDLED))
        toast(r->a, "Can't open: %s", err->message);
    g_object_unref(r->f);
    g_free(r);
}

void net_open(App *a, GFile *f) {
    if (g_file_is_native(f) || g_file_has_uri_scheme(f, "trash") || is_network(f)) {
        navigate(a, f, TRUE, NULL);
        return;
    }
    OpenReq *r = g_new0(OpenReq, 1);
    r->a = a;
    r->f = g_object_ref(f);
    g_autoptr(GMountOperation) op = gtk_mount_operation_new(a->win);
    toast(a, "Connecting…");
    g_file_mount_enclosing_volume(f, G_MOUNT_MOUNT_NONE, op, NULL, mount_cb, r);
}

static void connect_typed(App *a) {
    g_autofree char *t = g_strstrip(g_strdup(gtk_editable_get_text(GTK_EDITABLE(entry))));
    if (!*t) return;
    g_autofree char *uri = strstr(t, "://") ? g_strdup(t) : g_strconcat("smb://", t, NULL);
    if (!g_str_has_suffix(uri, "/") && !strchr(strstr(uri, "://") + 3, '/')) {
        char *u = g_strconcat(uri, "/", NULL);
        g_free(uri);
        uri = u;
    }
    servers_load();
    gboolean known = FALSE;
    for (guint i = 0; i < servers->len; i++) known |= g_str_equal(servers->pdata[i], uri);
    if (!known) {
        g_ptr_array_add(servers, g_strdup(uri));
        servers_save();
        rebuild_manual();
    }
    g_autoptr(GFile) f = g_file_new_for_uri(uri);
    net_open(a, f);
}

static void on_connect(GtkWidget *w, gpointer data) { connect_typed(data); }
static void on_rescan(GtkWidget *w, gpointer data) { network_scan(data); }

GtkWidget *network_new(App *a) {
    app_ref = a;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(box, 8);
    gtk_widget_set_margin_end(box, 8);
    gtk_widget_set_margin_top(box, 6);

    GtkWidget *bar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    entry = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "Server: 192.168.1.10, smb://host/share, sftp://user@host");
    gtk_widget_set_hexpand(entry, TRUE);
    g_signal_connect_swapped(entry, "activate", G_CALLBACK(connect_typed), a);
    GtkWidget *go = gtk_button_new_with_label("Connect");
    GtkWidget *rescan = gtk_button_new_with_label("Rescan");
    gtk_widget_add_css_class(go, "suggested");
    g_signal_connect(go, "clicked", G_CALLBACK(on_connect), a);
    g_signal_connect(rescan, "clicked", G_CALLBACK(on_rescan), a);
    gtk_box_append(GTK_BOX(bar), entry);
    gtk_box_append(GTK_BOX(bar), go);
    gtk_box_append(GTK_BOX(bar), rescan);
    gtk_box_append(GTK_BOX(box), bar);

    status = gtk_label_new("");
    gtk_widget_add_css_class(status, "dim");
    gtk_label_set_xalign(GTK_LABEL(status), 0);
    gtk_box_append(GTK_BOX(box), status);

    list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    g_signal_connect(list, "row-activated", G_CALLBACK(on_row_activated), NULL);
    GtkWidget *sw = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(sw), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(sw), list);
    gtk_widget_set_vexpand(sw, TRUE);
    gtk_box_append(GTK_BOX(box), sw);
    rebuild_manual();
    return box;
}
