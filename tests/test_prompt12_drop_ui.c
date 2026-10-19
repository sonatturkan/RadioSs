#include "radios/data.h"
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
    gint64 end = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;
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
static GtkWidget *dialog(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *result = NULL;
    for (GList *item = windows; item && !result; item = item->next)
        if (g_str_equal(gtk_widget_get_name(item->data), name)) result = item->data;
    g_list_free(windows); return result;
}
static gboolean connected(gpointer value) { return radios_data_connected(&((Harness *)value)->ctx); }
static Harness *open_harness(void)
{
    Harness *h = g_new0(Harness, 1); h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(h->window);
    h->ctx = (RadioSViewContext){ .config = &config, .runtime = radios_runtime_new(32, 64), .parent = GTK_WINDOW(h->window) };
    radios_ui_bind_media_drop(h->window, &h->ctx); gtk_widget_show_all(h->window);
    h->ctx.data = radios_data_new(&h->ctx); radios_data_connect(&h->ctx); wait_for(connected, h); return h;
}
static void close_harness(Harness *h)
{
    gtk_widget_destroy(h->window); radios_data_close(h->ctx.data); radios_runtime_free(h->ctx.runtime);
    while (g_main_context_iteration(NULL, FALSE)) {}
    radios_data_unref(h->ctx.data); g_object_unref(h->window); g_free(h);
}
typedef struct { const gchar *path; JsonNode *body, *result; GError *error; gint done; } Request;
static gpointer request_worker(gpointer value)
{
    Request *r = value; r->result = radios_data_client_request_sync(reader, "POST", r->path, r->body, NULL, &r->error);
    g_atomic_int_set(&r->done, 1); return NULL;
}
static gboolean request_done(gpointer value) { return g_atomic_int_get(&((Request *)value)->done); }
static JsonNode *request(const gchar *path, JsonNode *input)
{
    Request work = {.path = path, .body = input}; GThread *thread = g_thread_new("prompt12-drop-read", request_worker, &work);
    wait_for(request_done, &work); g_thread_join(thread); g_assert_no_error(work.error); g_assert_nonnull(work.result); return work.result;
}
static JsonNode *body(void)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, json_object_new());
    json_object_set_string_member(json_node_get_object(node), "station_id", radios_data_client_station_id(reader)); return node;
}
static JsonNode *create_playlist(const gchar *name)
{
    JsonNode *input = body(); JsonObject *object = json_node_get_object(input); gchar *key = g_uuid_string_random();
    json_object_set_string_member(object, "name", name); json_object_set_string_member(object, "idempotency_key", key);
    JsonNode *result = request("/v1/workflows/desktop-playlists/create", input); json_node_free(input); g_free(key); return result;
}
static JsonNode *playlist(const gchar *id)
{
    JsonNode *input = body(), *result = request("/v1/workflows/desktop-playlists/list", input); json_node_free(input);
    JsonArray *rows = json_object_get_array_member(json_node_get_object(result), "playlists"); JsonNode *found = NULL;
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        if (!g_strcmp0(radios_json_string(row, "_id", ""), id)) { found = json_node_copy(json_array_get_element(rows, i)); break; }
    }
    json_node_free(result); g_assert_nonnull(found); return found;
}
static guint media_count(void)
{
    JsonNode *input = body(), *result = request("/v1/workflows/desktop-media/list", input);
    guint count = (guint)radios_json_int(json_node_get_object(result), "total", -1);
    json_node_free(input); json_node_free(result); return count;
}
static guint staged_files(void)
{
    GDir *directory = g_dir_open(config.import_root, 0, NULL); g_assert_nonnull(directory); guint count = 0;
    while (g_dir_read_name(directory)) ++count;
    g_dir_close(directory); return count;
}
static void write16(guchar *p, guint16 n) { p[0] = n & 255; p[1] = n >> 8; }
static void write32(guchar *p, guint32 n) { for (guint i = 0; i < 4; ++i) p[i] = (n >> (8 * i)) & 255; }
static gchar *audio(const gchar *name, guint amplitude)
{
    const guint bytes = 32000; guchar *wav = g_malloc0(44 + bytes);
    memcpy(wav, "RIFF", 4); write32(wav + 4, 36 + bytes); memcpy(wav + 8, "WAVEfmt ", 8); write32(wav + 16, 16);
    write16(wav + 20, 1); write16(wav + 22, 1); write32(wav + 24, 8000); write32(wav + 28, 16000);
    write16(wav + 32, 2); write16(wav + 34, 16); memcpy(wav + 36, "data", 4); write32(wav + 40, bytes);
    for (guint i = 0; i < bytes / 2; ++i) write16(wav + 44 + 2 * i, (guint16)(i % 8 < 4 ? (gint)amplitude : -(gint)amplitude));
    gchar *path = g_build_filename(config.project_root, name, NULL);
    g_assert_true(g_file_set_contents(path, (const gchar *)wav, 44 + bytes, NULL)); g_free(wav); return path;
}
static gchar *sha(const gchar *path)
{
    gchar *bytes = NULL; gsize size = 0; g_assert_true(g_file_get_contents(path, &bytes, &size, NULL));
    gchar *result = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)bytes, size); g_free(bytes); return result;
}
typedef struct { const guchar *data; gint length; } ClipboardPayload;
static void clipboard_get(GtkClipboard *clipboard, GtkSelectionData *selection, guint info, gpointer value)
{
    (void)clipboard; (void)info; ClipboardPayload *payload = value;
    gtk_selection_data_set(selection, gdk_atom_intern_static_string("text/uri-list"), 8, payload->data, payload->length);
}
static void drop(GtkWidget *target, const gchar *const *uris)
{
    static const GtkTargetEntry targets[] = {{"text/uri-list", 0, 0}};
    GString *text = g_string_new(NULL);
    for (guint i = 0; uris[i]; ++i) g_string_append_printf(text, "%s\r\n", uris[i]);
    ClipboardPayload payload = {(const guchar *)text->str, (gint)text->len}; GtkClipboard *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
    g_assert_true(gtk_clipboard_set_with_data(clipboard, targets, G_N_ELEMENTS(targets), clipboard_get, NULL, &payload));
    GtkSelectionData *selection = gtk_clipboard_wait_for_contents(clipboard, gdk_atom_intern_static_string("text/uri-list"));
    g_assert_nonnull(selection); g_signal_emit_by_name(target, "drag-data-received", NULL, 0, 0, selection, 0, GDK_CURRENT_TIME);
    gtk_selection_data_free(selection); gtk_clipboard_clear(clipboard); g_string_free(text, TRUE);
}
static gboolean target_ready(gpointer value)
{
    GtkWidget *accept = gtk_dialog_get_widget_for_response(GTK_DIALOG(value), GTK_RESPONSE_ACCEPT);
    return gtk_widget_is_sensitive(accept) && gtk_combo_box_get_active(GTK_COMBO_BOX(find(value, "media-playlist-target"))) >= 0;
}
static gboolean two_added(gpointer value)
{
    GtkWidget *label = find(value, "media-playlist-add-status");
    return label && strstr(gtk_label_get_text(GTK_LABEL(label)), "2 şarkı playliste eklendi");
}
static void assert_entry(JsonObject *entry, const gchar *title, const gchar *source, const gchar *digest)
{
    g_assert_cmpstr(radios_json_string(entry, "title", ""), ==, title);
    const gchar *keys[] = {"media_id", "version_id", "media_file_id"};
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) g_assert_true(g_uuid_string_is_valid(radios_json_string(entry, keys[i], "")));
    g_assert_cmpstr(radios_json_string(entry, "media_sha256", ""), ==, digest);
    JsonNode *input = body(); json_object_set_string_member(json_node_get_object(input), "media_id", radios_json_string(entry, "media_id", ""));
    JsonNode *detail = request("/v1/workflows/desktop-media/get", input); json_node_free(input);
    JsonObject *version = json_object_get_object_member(json_node_get_object(detail), "version");
    g_assert_cmpstr(radios_json_string(version, "_id", ""), ==, radios_json_string(entry, "version_id", ""));
    g_assert_cmpstr(radios_json_string(version, "media_file_id", ""), ==, radios_json_string(entry, "media_file_id", ""));
    g_assert_true(json_object_get_boolean_member(version, "verified"));
    const gchar *managed = radios_json_string(version, "managed_path", ""); g_assert_cmpstr(managed, !=, source);
    gchar *managed_sha = sha(managed), *source_sha = sha(source);
    g_assert_cmpstr(managed_sha, ==, digest); g_assert_cmpstr(source_sha, ==, digest);
    g_free(managed_sha); g_free(source_sha); json_node_free(detail);
}
static void test_drop_target_multi_confirmed(void)
{
    Harness *h = open_harness(); JsonNode *left = create_playlist("Prompt12 dokunulmayan hedef"), *right = create_playlist("Prompt12 seçilen hedef");
    gchar *left_id = g_strdup(radios_json_string(json_node_get_object(left), "_id", ""));
    gchar *right_id = g_strdup(radios_json_string(json_node_get_object(right), "_id", ""));
    gint64 left_rev = radios_json_int(json_node_get_object(left), "_rev", 0), right_rev = radios_json_int(json_node_get_object(right), "_rev", 0);
    guint before = media_count(), staged = staged_files();
    gchar *first = audio("prompt12 yüzde % bir.wav", 10000), *second = audio("prompt12 Türkçe iki.wav", 13000);
    gchar *first_sha = sha(first), *second_sha = sha(second), *first_uri = g_filename_to_uri(first, NULL, NULL), *second_uri = g_filename_to_uri(second, NULL, NULL);
    const gchar *uris[] = {first_uri, second_uri, NULL}; drop(h->window, uris);
    GtkWidget *picker = dialog("media-playlist-add-dialog"); g_assert_nonnull(picker); wait_for(target_ready, picker);
    g_assert_cmpuint(media_count(), ==, before); g_assert_cmpuint(staged_files(), ==, staged);
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(picker, "media-playlist-target")), right_id));
    gtk_dialog_response(GTK_DIALOG(picker), GTK_RESPONSE_ACCEPT); wait_for(two_added, picker);
    g_assert_cmpuint(media_count(), ==, before + 2); g_assert_cmpuint(staged_files(), ==, staged);
    JsonNode *saved = playlist(right_id), *untouched = playlist(left_id);
    JsonObject *saved_object = json_node_get_object(saved);
    g_assert_cmpint(radios_json_int(saved_object, "_rev", 0), >, right_rev);
    g_assert_cmpint(radios_json_int(json_node_get_object(untouched), "_rev", 0), ==, left_rev);
    JsonArray *entries = json_object_get_array_member(saved_object, "tracks"); g_assert_cmpuint(json_array_get_length(entries), ==, 2);
    assert_entry(json_array_get_object_element(entries, 0), "prompt12 yüzde % bir", first, first_sha);
    assert_entry(json_array_get_object_element(entries, 1), "prompt12 Türkçe iki", second, second_sha);
    gtk_dialog_response(GTK_DIALOG(picker), GTK_RESPONSE_CANCEL); close_harness(h);
    h = open_harness(); JsonNode *reopened = playlist(right_id);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(reopened), "tracks")), ==, 2); close_harness(h);
    json_node_free(left); json_node_free(right); json_node_free(saved); json_node_free(untouched); json_node_free(reopened);
    g_unlink(first); g_unlink(second); g_free(first); g_free(second); g_free(first_sha); g_free(second_sha); g_free(first_uri); g_free(second_uri); g_free(left_id); g_free(right_id);
}
static void test_cancel_before_import_and_repeat_drop(void)
{
    Harness *h = open_harness(); JsonNode *target = create_playlist("Prompt12 iptal hedefi");
    gchar *id = g_strdup(radios_json_string(json_node_get_object(target), "_id", ""));
    guint before = media_count(), staged = staged_files();
    gchar *first = audio("prompt12 iptal bir.wav", 14000), *second = audio("prompt12 iptal iki.wav", 15000);
    gchar *digest = sha(first), *first_uri = g_filename_to_uri(first, NULL, NULL), *second_uri = g_filename_to_uri(second, NULL, NULL);
    const gchar *uris[] = {first_uri, NULL}, *other[] = {second_uri, NULL}; drop(h->window, uris);
    GtkWidget *picker = dialog("media-playlist-add-dialog"); g_assert_nonnull(picker); wait_for(target_ready, picker);
    drop(h->window, other); g_assert_true(dialog("media-playlist-add-dialog") == picker);
    GtkWidget *library = dialog("media-library-dialog");
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(library, "media-import-status"))), "Playliste ekle penceresi açık"));
    gtk_dialog_response(GTK_DIALOG(picker), GTK_RESPONSE_CANCEL); g_assert_null(dialog("media-playlist-add-dialog"));
    g_assert_cmpuint(media_count(), ==, before); g_assert_cmpuint(staged_files(), ==, staged);
    JsonNode *saved = playlist(id); g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(saved), "tracks")), ==, 0);
    gchar *after = sha(first); g_assert_cmpstr(after, ==, digest); g_free(after);
    close_harness(h); json_node_free(target); json_node_free(saved); g_free(id);
    g_unlink(first); g_unlink(second); g_free(first); g_free(second); g_free(digest); g_free(first_uri); g_free(second_uri);
}
typedef struct { gint started; } Block;
static gchar *blocked_worker(gpointer value, const RadioSCancelToken *cancel, GError **error)
{
    (void)error; Block *block = value; g_atomic_int_set(&block->started, 1);
    while (!radios_cancelled(cancel)) g_usleep(1000);
    return g_strdup("cancelled");
}
static gboolean block_started(gpointer value) { return g_atomic_int_get(&((Block *)value)->started); }
static void test_close_pending_import_preserves_sources(void)
{
    Harness *h = open_harness(); JsonNode *target = create_playlist("Prompt12 kapanış hedefi");
    gchar *id = g_strdup(radios_json_string(json_node_get_object(target), "_id", ""));
    guint before = media_count(), staged = staged_files(); gchar *source = audio("prompt12 kapanış.wav", 17000);
    gchar *digest = sha(source), *uri = g_filename_to_uri(source, NULL, NULL); const gchar *uris[] = {uri, NULL};
    drop(h->window, uris); GtkWidget *picker = dialog("media-playlist-add-dialog"); wait_for(target_ready, picker);
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(picker, "media-playlist-target")), id));
    Block block = {0}; GError *error = NULL;
    g_assert_cmpuint(radios_runtime_submit(h->ctx.runtime, blocked_worker, &block, NULL, &error), >, 0); g_assert_no_error(error); wait_for(block_started, &block);
    gtk_dialog_response(GTK_DIALOG(picker), GTK_RESPONSE_ACCEPT); close_harness(h);
    g_assert_cmpuint(media_count(), ==, before); g_assert_cmpuint(staged_files(), ==, staged);
    JsonNode *saved = playlist(id); g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(saved), "tracks")), ==, 0);
    gchar *after = sha(source); g_assert_cmpstr(after, ==, digest); g_free(after);
    json_node_free(target); json_node_free(saved); g_free(id); g_unlink(source); g_free(source); g_free(digest); g_free(uri);
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *project_root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc; g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    GError *error = NULL; g_assert_true(radios_config_load(project_root, NULL, &config, &error)); g_assert_no_error(error); g_free(project_root);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/prompt12-drop/target-multi-binding-confirmed", test_drop_target_multi_confirmed);
    g_test_add_func("/prompt12-drop/cancel-repeated-drop-preserves", test_cancel_before_import_and_repeat_drop);
    g_test_add_func("/prompt12-drop/owner-close-pending-import", test_close_pending_import_preserves_sources);
    gint result = g_test_run(); radios_data_client_unref(reader); radios_config_clear(&config); return result;
}
