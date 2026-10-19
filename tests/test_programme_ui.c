#define _POSIX_C_SOURCE 200809L
#include "radios/window.h"
#include "radios/audio_ui.h"
#include "radios/audio.h"
#include "radios/media_jobs.h"
#include "radios/media_ui.h"
#include "radios/editors.h"
#include "radios/playout_ui.h"
#include "radios/programme_lifecycle.h"
#include <glib/gstdio.h>
#include <fcntl.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static RadioSConfig config;
static GtkApplication *application;
static RadioSDataClient *reader;
static const gchar *main_sink, *cue_sink, *monitor_sink;
typedef struct {
    RadioSWindow *window;
    RadioSRuntime *runtime;
    GtkWidget *root, *lifecycle_root;
    RadioSViewContext *ctx;
} Window;
typedef gboolean (*Condition)(gpointer);

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (GTK_IS_MENU_BUTTON(root)) {
        GtkWidget *popover = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(root)));
        GtkWidget *result = popover ? find(popover, name) : NULL;
        if (result) return result;
    }
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *dialog_find(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *result = NULL;
    for (GList *item = windows; item && !result; item = item->next)
        if (g_str_equal(gtk_widget_get_name(item->data), name)) result = item->data;
    g_list_free(windows); return result;
}
static void pump(void)
{ while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void run_for(guint milliseconds)
{
    gint64 deadline = g_get_monotonic_time() + (gint64)milliseconds * 1000;
    while (g_get_monotonic_time() < deadline) pump();
}
static void wait_until(Condition condition, gpointer data, const gchar *description)
{
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!condition(data) && g_get_monotonic_time() < deadline) pump();
    if (!condition(data)) g_error("Native programme acceptance timed out: %s", description);
}
static gboolean documents_ready(GtkWidget *root)
{
    GPtrArray *documents = g_object_get_data(G_OBJECT(root), "radios-data-documents");
    for (guint i = 0; documents && i < documents->len; ++i)
        if (!radios_data_document_ready(g_ptr_array_index(documents, i))) return FALSE;
    if (!GTK_IS_CONTAINER(root)) return TRUE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); gboolean ready = TRUE;
    for (GList *item = children; item && ready; item = item->next) ready = documents_ready(item->data);
    g_list_free(children); return ready;
}
static gboolean window_ready(gpointer value)
{
    Window *window = value;
    GtkWidget *main = find(window->root, "task-main-output"), *cue = find(window->root, "task-cue-output");
    return radios_data_connected(window->ctx) && documents_ready(window->root) && main && cue &&
        !g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(main)), main_sink) &&
        !g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(cue)), cue_sink) &&
        radios_audio_ui_ready(window->ctx, RADIOS_AUDIO_UI_MAIN) &&
        radios_audio_ui_ready(window->ctx, RADIOS_AUDIO_UI_CUE);
}
static Window window_open(void)
{
    Window window = {0}; GError *error = NULL; window.runtime = radios_runtime_new(64, 128);
    window.window = radios_window_new(application, &config, window.runtime, FALSE, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(window.window);
    window.ctx = radios_window_view_context(window.window); g_assert_nonnull(window.ctx); g_assert_nonnull(window.ctx->audio); g_assert_nonnull(window.ctx->playout);
    window.root = GTK_WIDGET(window.ctx->parent); radios_window_present(window.window);
    wait_until(window_ready, &window, "genuine DB identity, saved virtual outputs and engine readiness");
    /* The programme overflow menu was removed from the broadcast interface.
     * Exercise the lifecycle component in a test-owned window with its genuine
     * current document and broadcast owner, without adding a product entry. */
    GtkWidget *owner = find(window.root, "live-task-view"); g_assert_nonnull(owner);
    RadioSDocument **document = g_new(RadioSDocument *, 1);
    *document = radios_broadcast_programme_document(window.ctx); g_assert_nonnull(*document);
    window.lifecycle_root = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_transient_for(GTK_WINDOW(window.lifecycle_root), GTK_WINDOW(window.root));
    gtk_window_set_destroy_with_parent(GTK_WINDOW(window.lifecycle_root), TRUE);
    GtkWidget *panel = radios_programme_lifecycle_new(window.ctx, owner, document);
    g_object_set_data_full(G_OBJECT(panel), "acceptance-document-slot", document, g_free);
    gtk_container_add(GTK_CONTAINER(window.lifecycle_root), panel);
    gtk_widget_show_all(window.lifecycle_root);
    return window;
}
static void window_close(Window *window)
{
    gtk_widget_destroy(window->lifecycle_root); window->lifecycle_root = NULL;
    radios_window_free(window->window); radios_runtime_free(window->runtime);
    window->window = NULL; window->runtime = NULL; window->ctx = NULL; window->root = NULL;
    while (g_main_context_iteration(NULL, FALSE)) {}
}
static gboolean widget_enabled(gpointer widget) { return gtk_widget_is_sensitive(widget); }
static void click(GtkWidget *root, const gchar *name)
{
    GtkWidget *button = find(root, name); g_assert_true(GTK_IS_BUTTON(button));
    wait_until(widget_enabled, button, "native button becomes available after its status poll"); gtk_button_clicked(GTK_BUTTON(button));
}
typedef struct { RadioSDataClient *client; const gchar *path; JsonNode *body, *result; GError *error; gint done; } Request;
static gpointer request_work(gpointer value)
{
    Request *request = value;
    request->result = radios_data_client_request_sync(request->client, "POST", request->path, request->body, NULL, &request->error);
    g_atomic_int_set(&request->done, 1); return NULL;
}
static gboolean request_done(gpointer value) { return g_atomic_int_get(&((Request *)value)->done) != 0; }
static JsonNode *request(const gchar *path, JsonNode *body)
{
    Request task = {reader, path, body, NULL, NULL, 0}; GThread *thread = g_thread_new("playout-DB-read", request_work, &task);
    wait_until(request_done, &task, "authenticated DB readback"); g_thread_join(thread);
    g_assert_no_error(task.error); g_assert_nonnull(task.result); return task.result;
}
static JsonNode *body_new(void)
{
    JsonNode *body = json_node_new(JSON_NODE_OBJECT); json_node_take_object(body, json_object_new());
    json_object_set_string_member(json_node_get_object(body), "station_id", radios_data_client_station_id(reader)); return body;
}
static JsonNode *deep_copy(JsonNode *node)
{
    gchar *text = json_to_string(node, FALSE); JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));
    JsonNode *result = json_node_copy(json_parser_get_root(parser)); g_object_unref(parser); g_free(text); return result;
}
static void write_u16(guchar *bytes, guint16 value) { value = GUINT16_TO_LE(value); memcpy(bytes, &value, sizeof value); }
static void write_u32(guchar *bytes, guint32 value) { value = GUINT32_TO_LE(value); memcpy(bytes, &value, sizeof value); }
typedef struct { JsonNode *result; GError *error; guint callbacks, destroyed; } Import;
static void import_complete(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; (void)owner; Import *output = value; ++output->callbacks;
    if (result) output->result = deep_copy(result);
    if (error) output->error = g_error_copy(error);
}
static void import_destroy(gpointer value) { ++((Import *)value)->destroyed; }
static gboolean import_done(gpointer value) { return ((Import *)value)->destroyed != 0; }
static JsonNode *media_import(Window *window, const gchar *title, gdouble frequency, gdouble amplitude, const gchar *kind)
{
    guint frames = 48000 * 6 + 31, bytes = frames * 4; guchar *wav = g_malloc(44 + bytes);
    memcpy(wav, "RIFF", 4); write_u32(wav + 4, 36 + bytes); memcpy(wav + 8, "WAVEfmt ", 8); write_u32(wav + 16, 16);
    write_u16(wav + 20, 1); write_u16(wav + 22, 2); write_u32(wav + 24, 48000); write_u32(wav + 28, 48000 * 4);
    write_u16(wav + 32, 4); write_u16(wav + 34, 16); memcpy(wav + 36, "data", 4); write_u32(wav + 40, bytes);
    for (guint i = 0; i < frames; ++i) {
        gint16 sample = (gint16)lrint(sin(2 * G_PI * frequency * i / 48000) * amplitude * 32767);
        write_u16(wav + 44 + i * 4, (guint16)sample); write_u16(wav + 46 + i * 4, (guint16)sample);
    }
    gchar *uuid = g_uuid_string_random(), *name = g_strconcat(uuid, ".wav", NULL), *path = g_build_filename(config.project_root, name, NULL);
    g_assert_true(g_file_set_contents(path, (const gchar *)wav, 44 + bytes, NULL)); g_free(wav); g_free(uuid); g_free(name);
    Import output = {0}; GCancellable *handle = radios_media_import_async(window->ctx, window->root, path,
        kind, title, import_complete, &output, import_destroy);
    g_assert_nonnull(handle); wait_until(import_done, &output, "real SHA-bound PCM analysis/import");
    g_assert_no_error(output.error); g_assert_nonnull(output.result); g_assert_cmpuint(output.callbacks, ==, 1);
    JsonObject *media = json_object_get_object_member(json_node_get_object(output.result), "media");
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "media_id", radios_json_string(media, "_id", ""));
    JsonNode *detail = request("/v1/workflows/desktop-media/get", body); json_node_free(body); json_node_free(output.result);
    JsonObject *version = json_object_get_object_member(json_node_get_object(detail), "version");
    g_assert_true(json_object_get_boolean_member(version, "verified")); g_assert_true(json_object_get_boolean_member(version, "object_available"));
    g_assert_cmpint(radios_json_int(version, "duration_ms", 0), ==, 6001);
    g_assert_cmpstr(radios_json_string(version, "status", ""), ==, "draft");
    g_assert_true(g_file_test(radios_json_string(version, "managed_path", ""), G_FILE_TEST_IS_REGULAR));
    g_assert_cmpint(g_unlink(path), ==, 0); g_free(path); g_object_unref(handle); return detail;
}

static JsonNode *document_save(JsonObject *cfg)
{
    JsonNode *body = body_new(); JsonObject *o = json_node_get_object(body);
    json_object_set_string_member(o, "kind", "programme"); json_object_set_string_member(o, "name", "Mevcut program");
    json_object_set_object_member(o, "config", json_object_ref(cfg)); gchar *key = g_uuid_string_random(); json_object_set_string_member(o, "idempotency_key", key); g_free(key);
    JsonNode *records_body = body_new(); json_object_set_string_member(json_node_get_object(records_body), "kind", "programme");
    JsonNode *records = request("/v1/workflows/desktop/list", records_body); json_node_free(records_body);
    JsonArray *rows = json_object_get_array_member(json_node_get_object(records), "records");
    if (json_array_get_length(rows)) {
        JsonObject *record = json_array_get_object_element(rows, 0); json_object_set_string_member(o, "document_id", radios_json_string(record, "_id", ""));
        json_object_set_int_member(o, "expected_revision", radios_json_int(record, "_rev", 0));
    }
    JsonNode *result = request("/v1/workflows/desktop/put", body); json_node_free(body); json_node_free(records); return result;
}
static JsonNode *programme(JsonNode *detail)
{
    JsonObject *root = json_node_get_object(detail), *media = json_object_get_object_member(root, "media"), *version = json_object_get_object_member(root, "version");
    JsonObject *cfg = json_object_new(), *event = json_object_new(); JsonArray *events = json_array_new();
    json_object_set_string_member(cfg, "title", "Gerçek native onay"); json_object_set_string_member(cfg, "date", "2026-10-06"); json_object_set_string_member(cfg, "note", "");
    gchar *id = g_uuid_string_random(); json_object_set_string_member(event, "id", id); g_free(id);
    const gchar *keys[] = {"title", "artist", "album", "kind"};
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) json_object_set_string_member(event, keys[i], radios_json_string(media, keys[i], ""));
    json_object_set_string_member(event, "start_time", "08:30"); json_object_set_int_member(event, "duration_ms", radios_json_int(version, "duration_ms", 0));
    json_object_set_string_member(event, "version_label", "v1"); json_object_set_string_member(event, "primary_file", ""); json_object_set_string_member(event, "backup_file", ""); json_object_set_string_member(event, "note", "");
    json_object_set_string_member(event, "media_id", radios_json_string(media, "_id", "")); json_object_set_string_member(event, "version_id", radios_json_string(version, "_id", ""));
    json_object_set_string_member(event, "media_file_id", radios_json_string(version, "media_file_id", "")); json_object_set_string_member(event, "media_sha256", radios_json_string(version, "sha256", ""));
    json_array_add_object_element(events, event); json_object_set_array_member(cfg, "events", events); JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, cfg); return node;
}
static gboolean dialog_loaded(gpointer value)
{
    GtkWidget *dialog = value, *choice = find(dialog, "programme-prerequisite-version");
    return choice && gtk_combo_box_get_active_id(GTK_COMBO_BOX(choice)) != NULL && gtk_widget_is_sensitive(choice);
}
static gboolean approved(gpointer value) { return gtk_widget_is_sensitive(find(((Window *)value)->lifecycle_root, "programme-load")); }
static gboolean file_exists(gpointer path) { return g_file_test(path, G_FILE_TEST_EXISTS); }
static gboolean lifecycle_idle(gpointer value) { return gtk_widget_is_sensitive(find(((Window *)value)->lifecycle_root, "programme-review")); }
static gboolean programme_saved(gpointer value)
{ return radios_data_document_revision(radios_broadcast_programme_document(((Window *)value)->ctx)) > 0; }
static void prerequisite_click(GtkWidget *dialog, const gchar *name)
{
    wait_until(dialog_loaded, dialog, "fresh prerequisite records");
    GtkWidget *confirm = find(dialog, "programme-prerequisite-confirm"); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(confirm), TRUE);
    click(dialog, name); wait_until(dialog_loaded, dialog, "genuine content authorization receipt and reload");
}
static JsonNode *read_overview(JsonNode *document)
{
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "document_id", radios_json_string(json_node_get_object(document), "_id", ""));
    JsonNode *result = request("/v1/workflows/desktop-programme/overview", body); json_node_free(body); return result;
}
static void assert_real_pcm(void)
{
    const gchar *path = g_getenv("RADIOS_TEST_MAIN_CAPTURE"); gchar *bytes = NULL; gsize size = 0; g_assert_nonnull(path);
    g_assert_true(g_file_get_contents(path, &bytes, &size, NULL)); gdouble peak = 0;
    for (gsize i = 0; i + sizeof(float) <= size; i += sizeof(float)) { float sample; memcpy(&sample, bytes + i, sizeof sample); peak = MAX(peak, fabs(sample)); }
    g_free(bytes); g_assert_cmpfloat(peak, >, 0.03);
}
static void test_review_prerequisites_frozen_load_and_stale_reply(void)
{
    Window window = window_open(); JsonNode *detail = media_import(&window, "Native formal PCM", 440, 0.2, "music"); JsonNode *cfg = programme(detail);
    JsonNode *document = document_save(json_node_get_object(cfg)); radios_data_refresh(window.ctx);
    wait_until(widget_enabled, find(window.lifecycle_root, "programme-submit"), "saved programme lifecycle ready");
    click(window.lifecycle_root, "programme-submit"); wait_until(lifecycle_idle, &window, "formal draft submitted and reviewed");
    g_assert_false(gtk_widget_is_sensitive(find(window.lifecycle_root, "programme-approve")));
    JsonNode *view = read_overview(document); JsonObject *o = json_node_get_object(view), *rev = json_object_get_object_member(o, "revision");
    g_assert_cmpstr(radios_json_string(rev, "status", ""), ==, "draft"); json_node_free(view);
    click(window.lifecycle_root, "programme-prerequisites"); GtkWidget *dialog = dialog_find("programme-prerequisites-dialog"); g_assert_nonnull(dialog);
    prerequisite_click(dialog, "programme-output-create");
    prerequisite_click(dialog, "programme-version-approve"); prerequisite_click(dialog, "programme-item-activate"); prerequisite_click(dialog, "programme-item-eligible");
    gtk_entry_set_text(GTK_ENTRY(find(dialog, "programme-rights-territory")), "*");
    gtk_entry_set_text(GTK_ENTRY(find(dialog, "programme-rights-starts")), "2026-10-06T00:00:00Z"); gtk_entry_set_text(GTK_ENTRY(find(dialog, "programme-rights-ends")), "2026-10-07T00:00:00Z");
    gtk_entry_set_text(GTK_ENTRY(find(dialog, "programme-rights-source")), "Private fixture explicit rights");
    prerequisite_click(dialog, "programme-rights-save"); gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
    click(window.lifecycle_root, "programme-review"); wait_until(widget_enabled, find(window.lifecycle_root, "programme-approve"), "separate version, lifecycle, eligibility and rights accepted");
    click(window.lifecycle_root, "programme-approve"); wait_until(approved, &window, "formal approved immutable revision");
    view = read_overview(document); o = json_node_get_object(view); rev = json_object_get_object_member(o, "revision");
    gchar *approved_id = g_strdup(radios_json_string(rev, "_id", "")); g_assert_cmpstr(radios_json_string(rev, "status", ""), ==, "approved"); json_node_free(view);
    click(window.lifecycle_root, "programme-load"); run_for(700); wait_until(lifecycle_idle, &window, "formal snapshot actual native load"); assert_real_pcm();
    RadioSPlayoutSnapshot snapshot; g_assert_true(radios_playout_ui_get_snapshot(window.ctx, &snapshot)); g_assert_cmpint(snapshot.state, ==, RADIOS_PLAYOUT_PLAYING);
    GError *error = NULL; g_assert_true(radios_playout_ui_action(window.ctx, RADIOS_PLAYOUT_STOP, &error)); g_assert_no_error(error);
    gchar *directory = g_path_get_dirname(config.database_file);
    gchar *hold = g_build_filename(directory, ".acceptance-programme-prepare-hold", NULL);
    gchar *ready = g_build_filename(directory, ".acceptance-programme-prepare-ready", NULL);
    g_free(directory);
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL)); click(window.lifecycle_root, "programme-load"); wait_until(file_exists, ready, "snapshot held before native owner close");
    window_close(&window); g_assert_cmpint(g_unlink(hold), ==, 0); run_for(700); g_assert_cmpint(g_unlink(ready), ==, 0);
    window = window_open(); wait_until(approved, &window, "approved programme restored after pending owner close");
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL)); click(window.lifecycle_root, "programme-load"); wait_until(file_exists, ready, "actual server snapshot held");
    /* Mutate the real native source while formal prepare is pending; the
     * removed toolbar editor is not needed to exercise stale-reply safety. */
    JsonNode *changed = deep_copy(cfg);
    json_object_set_string_member(json_node_get_object(changed), "note", "Changed while response held");
    RadioSDocument *source = radios_broadcast_programme_document(window.ctx); g_assert_nonnull(source);
    radios_data_document_dirty(source, TRUE);
    g_assert_true(radios_data_document_save(source, changed)); json_node_free(changed);
    g_assert_cmpint(radios_data_document_revision(radios_broadcast_programme_document(window.ctx)), ==, 0);
    g_assert_cmpint(g_unlink(hold), ==, 0); run_for(700); wait_until(lifecycle_idle, &window, "delayed snapshot completed");
    wait_until(programme_saved, &window, "changed native programme acknowledged");
    g_assert_true(radios_playout_ui_get_snapshot(window.ctx, &snapshot)); g_assert_cmpint(snapshot.state, !=, RADIOS_PLAYOUT_PLAYING);
    view = read_overview(document); rev = json_object_get_object_member(json_node_get_object(view), "revision"); g_assert_cmpstr(radios_json_string(rev, "_id", ""), ==, approved_id);
    g_assert_false(json_object_get_boolean_member(json_node_get_object(view), "source_matches")); json_node_free(view);
    click(window.lifecycle_root, "programme-fork"); wait_until(lifecycle_idle, &window, "new draft without changing approved source");
    click(window.lifecycle_root, "programme-submit"); wait_until(lifecycle_idle, &window, "changed acknowledged native source submitted");
    view = read_overview(document); rev = json_object_get_object_member(json_node_get_object(view), "revision"); g_assert_cmpstr(radios_json_string(rev, "status", ""), ==, "draft");
    g_assert_cmpstr(radios_json_string(rev, "_id", ""), !=, approved_id); json_node_free(view);
    g_unlink(ready); g_free(hold); g_free(ready); g_free(approved_id); json_node_free(detail); json_node_free(document); json_node_free(cfg); window_close(&window);
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]); for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1]; --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    main_sink = g_getenv("RADIOS_TEST_MAIN_SINK"); cue_sink = g_getenv("RADIOS_TEST_CUE_SINK"); monitor_sink = g_getenv("RADIOS_TEST_MONITOR_SINK");
    g_assert_nonnull(main_sink); g_assert_true(g_str_has_prefix(main_sink, "radios_gui_"));
    GError *error = NULL; g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    application = gtk_application_new("org.radios.NativeProgrammeAcceptance", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error);
    g_test_add_func("/native-programme/prerequisites-immutable-approval-PCM-stale-reply-fork", test_review_prerequisites_frozen_load_and_stale_reply);
    gint result = g_test_run(); radios_data_client_unref(reader); g_object_unref(application); radios_config_clear(&config); return result;
}
