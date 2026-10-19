/* Ayarlar › Medya arşivi against an isolated genuine SDBASE store: a scan
 * hashes the archive, imports only new files through the media library and
 * files a subfolder into a playlist of the same name. */
#include "radios/data.h"
#include "radios/media_archive.h"
#include "radios/media_archive_ui.h"
#include "radios/media_ui.h"
#include <glib/gstdio.h>
#include <string.h>

static RadioSConfig config;
static RadioSDataClient *reader;
typedef struct { RadioSViewContext ctx; GtkWidget *window; } Harness;
typedef gboolean (*Condition)(gpointer);
static void pump(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void wait_for(Condition condition, gpointer value)
{
    gint64 end = g_get_monotonic_time() + 90 * G_TIME_SPAN_SECOND;
    while (!condition(value) && g_get_monotonic_time() < end) pump();
    g_assert_true(condition(value));
}
static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *required(GtkWidget *root, const gchar *name)
{ GtkWidget *widget = find(root, name); g_assert_nonnull(widget); return widget; }
static gboolean connected(gpointer value) { return radios_data_connected(&((Harness *)value)->ctx); }
static Harness *open_harness(gboolean connect)
{
    Harness *h = g_new0(Harness, 1); h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(h->window);
    h->ctx = (RadioSViewContext){ .config = &config, .runtime = radios_runtime_new(32, 64), .parent = GTK_WINDOW(h->window) };
    gtk_widget_show_all(h->window);
    if (connect) { h->ctx.data = radios_data_new(&h->ctx); radios_data_connect(&h->ctx); wait_for(connected, h); }
    return h;
}
static void close_harness(Harness *h)
{
    gtk_widget_destroy(h->window);
    if (h->ctx.data) radios_data_close(h->ctx.data);
    radios_runtime_free(h->ctx.runtime);
    while (g_main_context_iteration(NULL, FALSE)) {}
    if (h->ctx.data) radios_data_unref(h->ctx.data);
    g_object_unref(h->window); g_free(h);
}
typedef struct { const gchar *path; JsonNode *body, *result; GError *error; gint done; } Request;
static gpointer request_worker(gpointer value)
{
    Request *r = value; r->result = radios_data_client_request_sync(reader, "POST", r->path, r->body, NULL, &r->error);
    g_atomic_int_set(&r->done, 1); return NULL;
}
static gboolean request_done(gpointer value) { return g_atomic_int_get(&((Request *)value)->done); }
static JsonNode *request(const gchar *path, JsonNode *body)
{
    Request work = {.path = path, .body = body}; GThread *thread = g_thread_new("archive-read", request_worker, &work);
    wait_for(request_done, &work); g_thread_join(thread); g_assert_no_error(work.error); g_assert_nonnull(work.result); return work.result;
}
static JsonNode *body(void)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, json_object_new());
    json_object_set_string_member(json_node_get_object(node), "station_id", radios_data_client_station_id(reader)); return node;
}
static guint media_count(void)
{
    JsonNode *input = body(), *result = request("/v1/workflows/desktop-media/list", input);
    guint count = (guint)radios_json_int(json_node_get_object(result), "total", -1);
    json_node_free(input); json_node_free(result); return count;
}
static gchar *media_digest(const gchar *title)
{
    JsonNode *input = body(); json_object_set_string_member(json_node_get_object(input), "query", title);
    JsonNode *rows = request("/v1/workflows/desktop-media/list", input); json_node_free(input);
    JsonArray *records = json_object_get_array_member(json_node_get_object(rows), "records");
    g_assert_cmpuint(json_array_get_length(records), ==, 1);
    JsonObject *version = json_object_get_object_member(json_array_get_object_element(records, 0), "version");
    gchar *digest = g_strdup(radios_json_string(version, "sha256", "")); json_node_free(rows); return digest;
}
/* The playlist named like the subfolder, with the digests of its tracks. */
static GPtrArray *playlist_digests(const gchar *name, guint *playlists)
{
    JsonNode *input = body(), *result = request("/v1/workflows/desktop-playlists/list", input); json_node_free(input);
    JsonArray *rows = json_object_get_array_member(json_node_get_object(result), "playlists");
    GPtrArray *digests = NULL; *playlists = json_array_get_length(rows);
    for (guint i = 0; i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        if (g_strcmp0(radios_json_string(row, "name", ""), name)) continue;
        g_assert_null(digests); digests = g_ptr_array_new_with_free_func(g_free);
        JsonArray *tracks = json_object_get_array_member(row, "tracks");
        for (guint t = 0; t < json_array_get_length(tracks); ++t)
            g_ptr_array_add(digests, g_strdup(radios_json_string(json_array_get_object_element(tracks, t), "media_sha256", "")));
        g_assert_cmpstr(radios_json_string(row, "purpose", ""), ==, "music");
    }
    json_node_free(result); return digests;
}
static void write16(guchar *p, guint16 n) { p[0] = n & 255; p[1] = n >> 8; }
static void write32(guchar *p, guint32 n) { for (guint i = 0; i < 4; ++i) p[i] = (n >> (8 * i)) & 255; }
static gchar *audio(const gchar *relative, guint amplitude)
{
    const guint bytes = 32000; guchar *wav = g_malloc0(44 + bytes);
    memcpy(wav, "RIFF", 4); write32(wav + 4, 36 + bytes); memcpy(wav + 8, "WAVEfmt ", 8); write32(wav + 16, 16);
    write16(wav + 20, 1); write16(wav + 22, 1); write32(wav + 24, 8000); write32(wav + 28, 16000);
    write16(wav + 32, 2); write16(wav + 34, 16); memcpy(wav + 36, "data", 4); write32(wav + 40, bytes);
    for (guint i = 0; i < bytes / 2; ++i) write16(wav + 44 + 2 * i, (guint16)(i % 8 < 4 ? (gint)amplitude : -(gint)amplitude));
    gchar *path = g_build_filename(config.project_root, relative, NULL), *directory = g_path_get_dirname(path);
    g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0); g_free(directory);
    g_assert_true(g_file_set_contents(path, (const gchar *)wav, 44 + bytes, NULL)); g_free(wav); return path;
}
static gchar *sha(const gchar *path)
{
    gchar *bytes = NULL; gsize size = 0; g_assert_true(g_file_get_contents(path, &bytes, &size, NULL));
    gchar *result = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)bytes, size); g_free(bytes); return result;
}
static const gchar *progress(Harness *h)
{ return gtk_label_get_text(GTK_LABEL(required(h->window, "preferences-archive-progress"))); }
static gboolean run_finished(gpointer value)
{
    Harness *h = value;
    return g_str_has_prefix(progress(h), "Arşiv:") && !radios_media_import_busy(&h->ctx) &&
        gtk_widget_is_sensitive(required(h->window, "preferences-archive-scan"));
}
static gboolean scan_ready(gpointer value)
{ return gtk_widget_is_sensitive(required(((Harness *)value)->window, "preferences-archive-scan")); }

static void save_settings(const gchar *folder, gboolean folder_playlists)
{
    RadioSMediaArchiveSettings settings; radios_media_archive_settings_init(&settings);
    g_ptr_array_add(settings.folders, g_strdup(folder)); settings.folder_playlists = folder_playlists;
    GError *error = NULL; g_assert_true(radios_media_archive_settings_save(config.project_root, &settings, &error)); g_assert_no_error(error);
    radios_media_archive_settings_clear(&settings);
}

static void test_scan_imports_new_files_and_folder_playlist(void)
{
    Harness *h = open_harness(TRUE); guint before = media_count();
    gchar *top = audio("arşiv/üst parça.wav", 9000), *jingle = audio("arşiv/Jingle Seti/jingle bir.wav", 7000);
    gchar *same = audio("arşiv/Jingle Seti/kopya.wav", 7000); /* identical bytes: imported once */
    gchar *top_sha = sha(top), *jingle_sha = sha(jingle);
    gchar *archive = g_build_filename(config.project_root, "arşiv", NULL);
    save_settings(archive, TRUE);
    GtkWidget *page = radios_media_archive_ui_page_new(&h->ctx); gtk_container_add(GTK_CONTAINER(h->window), page);
    gtk_widget_show_all(h->window);
    wait_for(scan_ready, h);
    g_assert_cmpuint(g_list_length(gtk_container_get_children(GTK_CONTAINER(required(h->window, "preferences-archive-folders")))), ==, 1);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(required(h->window, "preferences-archive-folder-playlists"))));
    gtk_button_clicked(GTK_BUTTON(required(h->window, "preferences-archive-scan")));
    pump(); g_assert_false(gtk_widget_is_sensitive(required(h->window, "preferences-archive-scan")));
    wait_for(run_finished, h);
    const gchar *summary = progress(h);
    g_assert_nonnull(strstr(summary, "3 ses dosyası")); g_assert_nonnull(strstr(summary, "2 yeni"));
    g_assert_nonnull(strstr(summary, "2 içe aktarıldı")); g_assert_nonnull(strstr(summary, "0 doğrulanamadı"));
    g_assert_nonnull(strstr(summary, "1 kopya atlandı"));
    g_assert_cmpuint(media_count(), ==, before + 2);
    gchar *stored_top = media_digest("üst parça"), *stored_jingle = media_digest("jingle bir");
    g_assert_cmpstr(stored_top, ==, top_sha); g_assert_cmpstr(stored_jingle, ==, jingle_sha);
    guint playlists = 0; GPtrArray *tracks = playlist_digests("Jingle Seti", &playlists);
    g_assert_nonnull(tracks); g_assert_cmpuint(tracks->len, ==, 1); g_assert_cmpstr(g_ptr_array_index(tracks, 0), ==, jingle_sha);
    g_ptr_array_unref(tracks);
    /* A second run finds nothing new and reuses the playlist. */
    gchar *late = audio("arşiv/Jingle Seti/jingle iki.wav", 5000); gchar *late_sha = sha(late);
    gtk_button_clicked(GTK_BUTTON(required(h->window, "preferences-archive-scan")));
    pump(); wait_for(run_finished, h);
    summary = progress(h);
    g_assert_nonnull(strstr(summary, "4 ses dosyası")); g_assert_nonnull(strstr(summary, "1 yeni")); g_assert_nonnull(strstr(summary, "1 içe aktarıldı"));
    g_assert_cmpuint(media_count(), ==, before + 3);
    guint playlists_after = 0; tracks = playlist_digests("Jingle Seti", &playlists_after);
    g_assert_cmpuint(playlists_after, ==, playlists); g_assert_nonnull(tracks); g_assert_cmpuint(tracks->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(tracks, 1), ==, late_sha); g_ptr_array_unref(tracks);
    gtk_button_clicked(GTK_BUTTON(required(h->window, "preferences-archive-scan")));
    pump(); wait_for(run_finished, h);
    g_assert_nonnull(strstr(progress(h), "0 yeni")); g_assert_cmpuint(media_count(), ==, before + 3);
    close_harness(h);
    g_free(top); g_free(jingle); g_free(same); g_free(late); g_free(top_sha); g_free(jingle_sha); g_free(late_sha);
    g_free(stored_top); g_free(stored_jingle); g_free(archive);
}

static void test_disconnected_page_waits(void)
{
    Harness *h = open_harness(FALSE);
    GtkWidget *page = radios_media_archive_ui_page_new(&h->ctx); gtk_container_add(GTK_CONTAINER(h->window), page);
    gtk_widget_show_all(h->window); pump();
    g_assert_false(gtk_widget_is_sensitive(required(h->window, "preferences-archive-scan")));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(required(h->window, "preferences-archive-status"))), "veri servisi"));
    close_harness(h);
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *project_root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc; g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    GError *error = NULL; g_assert_true(radios_config_load(project_root, NULL, &config, &error)); g_assert_no_error(error); g_free(project_root);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    Harness *starter = open_harness(TRUE); close_harness(starter);
    g_test_add_func("/media-archive-ui/scan-imports-new-files-and-folder-playlist", test_scan_imports_new_files_and_folder_playlist);
    g_test_add_func("/media-archive-ui/disconnected-page-waits", test_disconnected_page_waits);
    gint result = g_test_run(); radios_data_client_unref(reader); radios_config_clear(&config); return result;
}
