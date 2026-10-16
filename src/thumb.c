/* Tile previews: images and SVG (gdk-pixbuf), videos (ffmpegthumbnailer / ffmpeg),
 * PDFs (pdftoppm) and the first lines of text files. Work happens in a small thread
 * pool; results are kept in memory and rendered images of videos/PDFs on disk. */
#include "app.h"
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    GdkTexture *tex;
    char *text;
} Thumb;

typedef struct {
    GtkWidget *tile;
    char *key;
    GFile *file;
    char *ctype;
    int size;
    Thumb *result;
} Job;

static GHashTable *cache; /* key -> Thumb* (NULL thumb = nothing to show) */
static GThreadPool *pool;

static void thumb_free(Thumb *t) {
    if (!t) return;
    g_clear_object(&t->tex);
    g_free(t->text);
    g_free(t);
}

static void job_free(Job *j) {
    g_object_unref(j->tile);
    g_free(j->key);
    g_object_unref(j->file);
    g_free(j->ctype);
    g_free(j);
}

static void show(GtkWidget *tile, Thumb *t) {
    GtkWidget *prev = g_object_get_data(G_OBJECT(tile), "prev");
    if (!t) return;
    if (t->tex) {
        gtk_picture_set_paintable(GTK_PICTURE(g_object_get_data(G_OBJECT(tile), "pic")), GDK_PAINTABLE(t->tex));
        gtk_stack_set_visible_child_name(GTK_STACK(prev), "pic");
    } else if (t->text) {
        gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(tile), "text")), t->text);
        gtk_stack_set_visible_child_name(GTK_STACK(prev), "text");
    }
}

static gboolean done_cb(gpointer data) {
    Job *j = data;
    if (g_hash_table_size(cache) > 800) g_hash_table_remove_all(cache);
    g_hash_table_replace(cache, g_strdup(j->key), j->result);
    const char *cur = g_object_get_data(G_OBJECT(j->tile), "tkey");
    if (cur && g_str_equal(cur, j->key)) show(j->tile, j->result); /* the tile may have been recycled meanwhile */
    job_free(j);
    return G_SOURCE_REMOVE;
}

static GdkTexture *texture_from(const char *path, int size) {
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(path, size, size, TRUE, NULL);
    if (!pb) return NULL;
    gsize len = (gsize)gdk_pixbuf_get_rowstride(pb) * gdk_pixbuf_get_height(pb);
    GBytes *bytes = g_bytes_new_with_free_func(gdk_pixbuf_read_pixels(pb), len, g_object_unref, pb); /* keeps the pixbuf alive */
    GdkTexture *tex = gdk_memory_texture_new(gdk_pixbuf_get_width(pb), gdk_pixbuf_get_height(pb),
                                             gdk_pixbuf_get_has_alpha(pb) ? GDK_MEMORY_R8G8B8A8 : GDK_MEMORY_R8G8B8, bytes,
                                             gdk_pixbuf_get_rowstride(pb));
    g_bytes_unref(bytes);
    return tex;
}

static gboolean tool_ok(const char *name) {
    g_autofree char *p = g_find_program_in_path(name);
    return p != NULL;
}

static GdkTexture *render_cached(Job *j, const char *path, const char *kind) {
    g_autofree char *dir = g_build_filename(g_get_user_cache_dir(), "files4hyprland", "thumbs", NULL);
    g_mkdir_with_parents(dir, 0755);
    g_autofree char *sum = g_compute_checksum_for_string(G_CHECKSUM_SHA1, j->key, -1);
    g_autofree char *png = g_build_filename(dir, sum, NULL);
    g_autofree char *out = g_strconcat(png, ".png", NULL);
    if (!g_file_test(out, G_FILE_TEST_EXISTS)) {
        g_autofree char *sz = g_strdup_printf("%d", j->size);
        g_autofree char *tmp = g_strconcat(png, ".tmp.png", NULL);
        int st = -1;
        if (g_str_equal(kind, "video")) {
            if (tool_ok("ffmpegthumbnailer")) {
                const char *argv[] = {"ffmpegthumbnailer", "-i", path, "-o", tmp, "-s", sz, "-c", "png", NULL};
                g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                             NULL, NULL, NULL, NULL, &st, NULL);
            } else if (tool_ok("ffmpeg")) {
                g_autofree char *vf = g_strdup_printf("scale=%d:-2", j->size);
                const char *argv[] = {"ffmpeg", "-v", "quiet", "-y", "-ss", "1", "-i", path, "-frames:v", "1", "-vf", vf, tmp, NULL};
                g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                             NULL, NULL, NULL, NULL, &st, NULL);
            }
        } else if (tool_ok("pdftoppm")) {
            g_autofree char *base = g_strconcat(png, ".tmp", NULL);
            const char *argv[] = {"pdftoppm", "-png", "-singlefile", "-f", "1", "-l", "1", "-scale-to", sz, path, base, NULL};
            g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                         NULL, NULL, NULL, NULL, &st, NULL);
        }
        if (st == 0 && g_file_test(tmp, G_FILE_TEST_EXISTS)) g_rename(tmp, out);
        else g_unlink(tmp);
    }
    return g_file_test(out, G_FILE_TEST_EXISTS) ? texture_from(out, j->size) : NULL;
}

/* first lines of a text file, cut to what fits a tile */
static char *text_snippet(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char buf[900];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (!n || memchr(buf, 0, n)) return NULL;
    buf[n] = 0;
    g_autofree char *valid = g_utf8_make_valid(buf, n);
    g_auto(GStrv) lines = g_strsplit(valid, "\n", -1);
    GString *s = g_string_new(NULL);
    int shown = 0;
    for (char **l = lines; *l && shown < 10; l++, shown++) {
        g_strdelimit(*l, "\t\r", ' ');
        if (g_utf8_strlen(*l, -1) > 26) {
            char *cut = g_utf8_offset_to_pointer(*l, 26);
            *cut = 0;
        }
        g_string_append(s, *l);
        g_string_append_c(s, '\n');
    }
    return g_string_free(s, FALSE);
}

static void work(gpointer data, gpointer user) {
    (void)user;
    Job *j = data;
    g_autofree char *path = g_file_get_path(j->file);
    Thumb *t = NULL;
    if (j->ctype && g_str_equal(j->ctype, "inode/symlink")) { /* preview what the link points to */
        g_autoptr(GFileInfo) target = g_file_query_info(j->file, "standard::content-type", 0, NULL, NULL);
        g_free(j->ctype);
        j->ctype = target ? g_strdup(g_file_info_get_content_type(target)) : NULL;
    }
    if (path && j->ctype) {
        GdkTexture *tex = NULL;
        char *text = NULL;
        if (g_str_has_prefix(j->ctype, "image/")) tex = texture_from(path, j->size);
        else if (g_str_has_prefix(j->ctype, "video/")) tex = render_cached(j, path, "video");
        else if (g_str_equal(j->ctype, "application/pdf")) tex = render_cached(j, path, "pdf");
        else if (g_content_type_is_a(j->ctype, "text/plain") || g_str_equal(j->ctype, "application/json") ||
                 g_str_equal(j->ctype, "application/xml") || g_str_equal(j->ctype, "application/x-shellscript"))
            text = text_snippet(path);
        if (tex || text) {
            t = g_new0(Thumb, 1);
            t->tex = tex;
            t->text = text;
        }
    }
    j->result = t;
    g_main_context_invoke(NULL, done_cb, j);
}

void thumb_load(GtkWidget *tile, GFileInfo *info, int size) {
    if (!cache) {
        cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)thumb_free);
        pool = g_thread_pool_new(work, NULL, 3, FALSE, NULL);
    }
    GFile *file = info_file(info);
    const char *ct = g_file_info_get_attribute_string(info, G_FILE_ATTRIBUTE_STANDARD_FAST_CONTENT_TYPE);
    gboolean able = ct && !info_is_dir(info) && g_file_is_native(file) &&
                    (g_str_equal(ct, "inode/symlink") || g_str_has_prefix(ct, "image/") || g_str_has_prefix(ct, "video/") || g_str_equal(ct, "application/pdf") ||
                     g_content_type_is_a(ct, "text/plain") || g_str_equal(ct, "application/json") ||
                     g_str_equal(ct, "application/xml") || g_str_equal(ct, "application/x-shellscript"));
    if (!able) {
        g_object_set_data(G_OBJECT(tile), "tkey", NULL);
        return;
    }
    g_autofree char *uri = g_file_get_uri(file);
    char *key = g_strdup_printf("%s|%" G_GUINT64_FORMAT "|%" G_GOFFSET_FORMAT "|%d", uri,
                                g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED), g_file_info_get_size(info), size);
    g_object_set_data_full(G_OBJECT(tile), "tkey", key, g_free);
    gpointer hit;
    if (g_hash_table_lookup_extended(cache, key, NULL, &hit)) {
        show(tile, hit);
        return;
    }
    Job *j = g_new0(Job, 1);
    j->tile = g_object_ref(tile);
    j->key = g_strdup(key);
    j->file = g_object_ref(file);
    j->ctype = g_strdup(ct);
    j->size = size;
    g_thread_pool_push(pool, j, NULL);
}
