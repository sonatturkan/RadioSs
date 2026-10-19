#define _POSIX_C_SOURCE 200809L
#include "radios/window.h"
#include "radios/audio_ui.h"
#include "radios/audio.h"
#include "radios/media_jobs.h"
#include "radios/media_ui.h"
#include "radios/editors.h"
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
typedef struct { RadioSWindow *window; RadioSRuntime *runtime; GtkWidget *root; RadioSViewContext *ctx; } Window;
typedef gboolean (*Condition)(gpointer);

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
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
    if (!condition(data)) g_error("Native audio acceptance timed out: %s", description);
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
    window.ctx = radios_window_view_context(window.window); g_assert_nonnull(window.ctx); g_assert_nonnull(window.ctx->audio);
    window.root = GTK_WIDGET(window.ctx->parent); radios_window_present(window.window);
    wait_until(window_ready, &window, "genuine DB identity, saved virtual outputs and engine readiness");
    return window;
}
static void window_close(Window *window)
{
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
static void combo(GtkWidget *root, const gchar *name, const gchar *id)
{
    GtkWidget *widget = find(root, name); g_assert_true(GTK_IS_COMBO_BOX(widget));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), id));
}
static GtkAdjustment *adjustment(GtkWidget *root, const gchar *name)
{
    GtkWidget *widget = find(root, name); g_assert_nonnull(widget);
    GtkAdjustment *result = g_object_get_data(G_OBJECT(widget), "adjustment"); g_assert_true(GTK_IS_ADJUSTMENT(result)); return result;
}
static void toggle(GtkWidget *root, const gchar *name, gboolean enabled)
{
    GtkWidget *widget = find(root, name); g_assert_true(GTK_IS_TOGGLE_BUTTON(widget));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widget), enabled);
}
static void assert_gates_off(Window *window)
{
    const gchar *names[] = {"task-microphone-gate", "header-headphone-cue", "header-output-cue"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkWidget *widget = find(window->root, names[i]); g_assert_true(GTK_IS_TOGGLE_BUTTON(widget));
        g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
    }
    RadioSAudioUiSnapshot snapshot;
    for (guint deck = 0; deck < RADIOS_AUDIO_DECKS; ++deck) {
        g_assert_true(radios_audio_ui_get_snapshot(window->ctx, deck, &snapshot));
        g_assert_cmpint(snapshot.state, ==, RADIOS_AUDIO_UI_EMPTY);
    }
}
typedef struct { RadioSViewContext *ctx; guint deck; RadioSAudioUiState state; guint64 minimum_position; } DeckCondition;
static gboolean deck_matches(gpointer value)
{
    DeckCondition *condition = value; RadioSAudioUiSnapshot snapshot;
    return radios_audio_ui_get_snapshot(condition->ctx, condition->deck, &snapshot) &&
        snapshot.state == condition->state && snapshot.position_ms >= condition->minimum_position;
}
static RadioSAudioUiSnapshot deck_wait(Window *window, guint deck, RadioSAudioUiState state, guint64 position)
{
    DeckCondition condition = {window->ctx, deck, state, position};
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!deck_matches(&condition) && g_get_monotonic_time() < deadline) pump();
    RadioSAudioUiSnapshot snapshot; g_assert_true(radios_audio_ui_get_snapshot(window->ctx, deck, &snapshot));
    if (!deck_matches(&condition)) {
        GtkWidget *editor = dialog_find("queue-waveform-dialog");
        GtkWidget *status = editor ? find(editor, "editor-status") : NULL;
        g_error("Deck %u expected state %d/cursor >= %" G_GUINT64_FORMAT "; actual state %d/cursor %" G_GUINT64_FORMAT "/generation %" G_GUINT64_FORMAT
            "; CUE ready %d; editor status: %s; %s", deck, state, position, snapshot.state, snapshot.position_ms, snapshot.generation,
            radios_audio_ui_ready(window->ctx, RADIOS_AUDIO_UI_CUE), GTK_IS_LABEL(status) ? gtk_label_get_text(GTK_LABEL(status)) : "none", snapshot.error);
    }
    return snapshot;
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
    Request task = {reader, path, body, NULL, NULL, 0}; GThread *thread = g_thread_new("audio-DB-read", request_work, &task);
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
static JsonNode *preferences_read(void)
{
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "kind", "audio_preferences");
    JsonNode *result = request("/v1/workflows/desktop/list", body); json_node_free(body);
    JsonArray *rows = json_object_get_array_member(json_node_get_object(result), "records"); JsonNode *record = NULL;
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        if (!strcmp(radios_json_string(row, "name", ""), "Ses aygıtları")) { record = deep_copy(json_array_get_element(rows, i)); break; }
    }
    json_node_free(result); g_assert_nonnull(record); return record;
}
static JsonNode *preferences_remote_save(JsonNode *record, JsonObject *preferences)
{
    JsonObject *row = json_node_get_object(record); JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", "audio_preferences"); json_object_set_string_member(object, "name", "Ses aygıtları");
    json_object_set_string_member(object, "document_id", radios_json_string(row, "_id", ""));
    json_object_set_int_member(object, "expected_revision", radios_json_int(row, "_rev", 0));
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    json_object_set_object_member(object, "config", json_object_ref(preferences));
    JsonNode *result = request("/v1/workflows/desktop/put", body); json_node_free(body); return result;
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
static JsonNode *media_import(Window *window)
{
    guint frames = 48000 * 8, bytes = frames * 4; guchar *wav = g_malloc(44 + bytes);
    memcpy(wav, "RIFF", 4); write_u32(wav + 4, 36 + bytes); memcpy(wav + 8, "WAVEfmt ", 8); write_u32(wav + 16, 16);
    write_u16(wav + 20, 1); write_u16(wav + 22, 2); write_u32(wav + 24, 48000); write_u32(wav + 28, 48000 * 4);
    write_u16(wav + 32, 4); write_u16(wav + 34, 16); memcpy(wav + 36, "data", 4); write_u32(wav + 40, bytes);
    for (guint i = 0; i < frames; ++i) {
        gint16 sample = (gint16)lrint(sin(2 * G_PI * 997 * i / 48000) * 0.4 * 32767);
        write_u16(wav + 44 + i * 4, (guint16)sample); write_u16(wav + 46 + i * 4, (guint16)sample);
    }
    gchar *uuid = g_uuid_string_random(), *name = g_strconcat(uuid, ".wav", NULL), *path = g_build_filename(config.project_root, name, NULL);
    g_assert_true(g_file_set_contents(path, (const gchar *)wav, 44 + bytes, NULL)); g_free(wav); g_free(uuid); g_free(name);
    Import output = {0}; GCancellable *handle = radios_media_import_async(window->ctx, window->root, path,
        "music", "Gerçek Sanal Çıkış Kabul Sesi", import_complete, &output, import_destroy);
    g_assert_nonnull(handle); wait_until(import_done, &output, "real SHA-bound PCM analysis/import");
    g_assert_no_error(output.error); g_assert_nonnull(output.result); g_assert_cmpuint(output.callbacks, ==, 1);
    JsonObject *media = json_object_get_object_member(json_node_get_object(output.result), "media");
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "media_id", radios_json_string(media, "_id", ""));
    JsonNode *detail = request("/v1/workflows/desktop-media/get", body); json_node_free(body); json_node_free(output.result);
    JsonObject *version = json_object_get_object_member(json_node_get_object(detail), "version");
    g_assert_true(json_object_get_boolean_member(version, "verified")); g_assert_true(json_object_get_boolean_member(version, "object_available"));
    g_assert_cmpint(radios_json_int(version, "duration_ms", 0), ==, 8000);
    g_assert_cmpstr(radios_json_string(version, "status", ""), ==, "draft");
    g_assert_true(g_file_test(radios_json_string(version, "managed_path", ""), G_FILE_TEST_IS_REGULAR));
    g_assert_cmpint(g_unlink(path), ==, 0); g_free(path); g_object_unref(handle); return detail;
}
static goffset capture_offset(const gchar *bus)
{
    gchar *key = g_strdup_printf("RADIOS_TEST_%s_CAPTURE", bus); const gchar *path = g_getenv(key); g_assert_nonnull(path);
    struct stat status; g_assert_cmpint(g_stat(path, &status), ==, 0); g_free(key); return status.st_size - status.st_size % 8;
}
static gdouble capture_rms(const gchar *bus, goffset offset, gdouble *peak)
{
    gchar *key = g_strdup_printf("RADIOS_TEST_%s_CAPTURE", bus); const gchar *path = g_getenv(key); g_assert_nonnull(path);
    gint fd = open(path, O_RDONLY); g_assert_cmpint(fd, >=, 0); g_free(key);
    struct stat status; g_assert_cmpint(fstat(fd, &status), ==, 0);
    g_assert_cmpint(status.st_size - offset, >, 4800 * 8);
    guint64 count = 0; long double squares = 0; *peak = 0; gfloat buffer[4096];
    while (offset + (goffset)sizeof(gfloat) <= status.st_size) {
        ssize_t bytes = pread(fd, buffer, (size_t)MIN((goffset)sizeof buffer, status.st_size - offset), offset);
        g_assert_cmpint(bytes, >, 0); g_assert_cmpint(bytes % 4, ==, 0);
        for (guint i = 0; i < (guint)bytes / 4; ++i) {
            g_assert_true(isfinite(buffer[i])); squares += (long double)buffer[i] * buffer[i]; *peak = MAX(*peak, fabs(buffer[i])); ++count;
        }
        offset += bytes;
    }
    close(fd); return sqrt((gdouble)(squares / count));
}
static gdouble capture_min_window_rms(const gchar *bus, goffset offset)
{
    gchar *key = g_strdup_printf("RADIOS_TEST_%s_CAPTURE", bus); const gchar *path = g_getenv(key);
    g_assert_nonnull(path); gint fd = open(path, O_RDONLY); g_assert_cmpint(fd, >=, 0); g_free(key);
    struct stat status; g_assert_cmpint(fstat(fd, &status), ==, 0);
    gfloat buffer[480 * 2]; gdouble minimum = G_MAXDOUBLE;
    while (offset + (goffset)sizeof buffer <= status.st_size) {
        g_assert_cmpint(pread(fd, buffer, sizeof buffer, offset), ==, sizeof buffer);
        gdouble squares = 0;
        for (guint i = 0; i < G_N_ELEMENTS(buffer); ++i) squares += buffer[i] * buffer[i];
        minimum = MIN(minimum, sqrt(squares / G_N_ELEMENTS(buffer))); offset += sizeof buffer;
    }
    close(fd); g_assert_cmpfloat(minimum, !=, G_MAXDOUBLE); return minimum;
}
static void require_call(gboolean result, GError **error)
{ g_assert_no_error(*error); g_assert_true(result); }
static gboolean library_audio_ready(gpointer unused)
{
    (void)unused; GtkWidget *dialog = dialog_find("media-library-dialog");
    GtkWidget *button = dialog ? find(dialog, "media-pick-accept") : NULL;
    return button && gtk_widget_is_sensitive(button);
}
static gboolean deck_play_enabled(gpointer value)
{
    GtkWidget *button = find(value, "deck-0-play"); return button && gtk_widget_is_sensitive(button);
}
static void seek_widget(Window *window, guint64 milliseconds)
{
    GtkWidget *scale = find(window->root, "deck-0-seek"); g_assert_true(GTK_IS_RANGE(scale));
    wait_until(widget_enabled, scale, "native seek slider enables after decoder completion"); gboolean handled = FALSE;
    g_signal_emit_by_name(scale, "change-value", GTK_SCROLL_JUMP, (gdouble)milliseconds, &handled);
}

static void test_transport(void)
{
    Window window = window_open(); assert_gates_off(&window); JsonNode *detail = media_import(&window); GError *error = NULL;
    JsonObject *media = json_object_get_object_member(json_node_get_object(detail), "media");
    radios_media_library_open_for_deck(window.ctx, 0);
    radios_media_library_open_selected(window.ctx, radios_json_string(media, "_id", ""));
    wait_until(library_audio_ready, NULL, "selected library detail enables native audio load");
    GtkWidget *library = dialog_find("media-library-dialog");
    click(library, "media-pick-accept");
    RadioSAudioUiSnapshot snapshot = deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    g_assert_cmpuint(snapshot.position_ms, ==, 0); g_assert_cmpuint(snapshot.duration_ms, ==, 8000);
    g_assert_cmpstr(snapshot.media_id, ==, radios_json_string(media, "_id", ""));
    GtkWidget *card = g_object_get_data(G_OBJECT(window.root), "radios-deck-owner-0"); g_assert_nonnull(card);
    GtkWidget *title = g_object_get_data(G_OBJECT(card), "radios-deck-title-widget"); g_assert_true(GTK_IS_LABEL(title));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(title)), ==, radios_json_string(media, "title", ""));
    gtk_dialog_response(GTK_DIALOG(library), GTK_RESPONSE_CANCEL); wait_until(deck_play_enabled, window.root, "loaded native player controls enable");
    require_call(radios_audio_ui_set_deck_main(window.ctx, 0, TRUE, &error), &error);
    click(window.root, "deck-0-play"); deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 150);
    run_for(200); goffset start = capture_offset("MAIN"); run_for(300); gdouble peak;
    gdouble full_rms = capture_rms("MAIN", start, &peak); g_assert_cmpfloat(peak, >, 0.1); g_assert_cmpfloat(full_rms, >, 0.05);
    click(window.root, "deck-0-pause"); snapshot = deck_wait(&window, 0, RADIOS_AUDIO_UI_PAUSED, 0);
    guint64 paused = snapshot.position_ms; run_for(200); g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 0, &snapshot));
    g_assert_cmpuint(snapshot.position_ms, ==, paused);
    start = capture_offset("MAIN"); run_for(150); capture_rms("MAIN", start, &peak); g_assert_cmpfloat(peak, <, 0.00001);
    seek_widget(&window, 1000);
    /* R8.3: a seek moves a paused deck to the chosen point; it stays paused
     * there instead of reappearing as freshly loaded (READY) or LOADING. */
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 0, &snapshot));
    g_assert_cmpint(snapshot.state, ==, RADIOS_AUDIO_UI_PAUSED); g_assert_cmpuint(snapshot.position_ms, ==, 1000);
    snapshot = deck_wait(&window, 0, RADIOS_AUDIO_UI_PAUSED, 1000); g_assert_cmpuint(snapshot.position_ms, ==, 1000);
    wait_until(window_ready, &window, "seek output flush acknowledges the configured virtual devices");
    wait_until(deck_play_enabled, window.root, "native Play enables after seek and physical output acknowledgement");
    require_call(radios_audio_ui_set_deck_gain(window.ctx, 0, -12, &error), &error);
    click(window.root, "deck-0-play"); deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 1100);
    run_for(200); start = capture_offset("MAIN"); run_for(300); gdouble quiet_rms = capture_rms("MAIN", start, &peak);
    g_assert_cmpfloat(quiet_rms / full_rms, >, 0.20); g_assert_cmpfloat(quiet_rms / full_rms, <, 0.31);
    click(window.root, "deck-0-stop"); snapshot = deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    g_assert_cmpuint(snapshot.position_ms, ==, 0);
    seek_widget(&window, 7600); deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 7600);
    wait_until(window_ready, &window, "near-EOF seek output flush completes");
    wait_until(deck_play_enabled, window.root, "native Play enables after the near-EOF seek");
    click(window.root, "deck-0-play"); snapshot = deck_wait(&window, 0, RADIOS_AUDIO_UI_EOF, 8000);
    g_assert_cmpuint(snapshot.position_ms, ==, 8000); json_node_free(detail); window_close(&window);
}
static void test_preview_routing(void)
{
    Window window = window_open(); JsonNode *detail = media_import(&window); GError *error = NULL;
    run_for(250); goffset main_start = capture_offset("MAIN"), cue_start = capture_offset("CUE");
    GtkWidget *owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(owner);
    require_call(radios_audio_ui_preview(window.ctx, owner, json_node_get_object(detail), 0, 2000, &error), &error);
    RadioSAudioUiSnapshot snapshot = deck_wait(&window, RADIOS_AUDIO_PREVIEW_DECK, RADIOS_AUDIO_UI_PLAYING, 100);
    g_assert_false(snapshot.main_enabled); run_for(400); gdouble main_peak, cue_peak;
    gdouble main_rms = capture_rms("MAIN", main_start, &main_peak), cue_rms = capture_rms("CUE", cue_start, &cue_peak);
    g_assert_cmpfloat(cue_peak, >, 0.1); g_assert_cmpfloat(cue_rms, >, 0.05);
    g_assert_cmpfloat(main_peak, <, 0.00001); g_assert_cmpfloat(main_rms, <, 0.00001);
    if (!radios_audio_ui_set_deck_main(window.ctx, RADIOS_AUDIO_PREVIEW_DECK, TRUE, &error)) {
        g_assert_nonnull(error); g_clear_error(&error);
    }
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, RADIOS_AUDIO_PREVIEW_DECK, &snapshot)); g_assert_false(snapshot.main_enabled);
    radios_audio_ui_preview_stop(window.ctx, owner);
    wait_until(window_ready, &window, "previous preview Stop acknowledges physical CUE output before opening the editor");
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, RADIOS_AUDIO_PREVIEW_DECK, &snapshot));
    g_assert_cmpint(snapshot.state, !=, RADIOS_AUDIO_UI_PLAYING);
    radios_marker_editor_open_media(window.ctx, json_node_get_object(detail));
    GtkWidget *editor = dialog_find("queue-waveform-dialog"); g_assert_nonnull(editor);
    click(editor, "qwf-preview-play"); deck_wait(&window, RADIOS_AUDIO_PREVIEW_DECK, RADIOS_AUDIO_UI_PLAYING, 100);
    g_assert_true(radios_audio_ui_preview_owned(window.ctx, editor));
    click(editor, "qwf-preview-pause"); deck_wait(&window, RADIOS_AUDIO_PREVIEW_DECK, RADIOS_AUDIO_UI_PAUSED, 0);
    wait_until(window_ready, &window, "preview pause output flush completes before resume");
    click(editor, "qwf-preview-play"); deck_wait(&window, RADIOS_AUDIO_PREVIEW_DECK, RADIOS_AUDIO_UI_PLAYING, 150);
    click(editor, "qwf-preview-stop");
    wait_until(window_ready, &window, "editor preview Stop acknowledges physical CUE output before replacement");
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, RADIOS_AUDIO_PREVIEW_DECK, &snapshot));
    g_assert_cmpint(snapshot.state, !=, RADIOS_AUDIO_UI_PLAYING);
    click(editor, "qwf-preview-play"); g_assert_true(radios_audio_ui_preview_owned(window.ctx, editor));
    gtk_dialog_response(GTK_DIALOG(editor), GTK_RESPONSE_CANCEL); run_for(250);
    g_assert_null(dialog_find("queue-waveform-dialog"));
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, RADIOS_AUDIO_PREVIEW_DECK, &snapshot));
    g_assert_cmpint(snapshot.state, !=, RADIOS_AUDIO_UI_PLAYING);
    wait_until(window_ready, &window, "destroyed editor releases its CUE route before a new preview owner");
    require_call(radios_audio_ui_preview(window.ctx, owner, json_node_get_object(detail), 0, 2000, &error), &error);
    gtk_widget_destroy(owner); g_object_unref(owner); run_for(400);
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, RADIOS_AUDIO_PREVIEW_DECK, &snapshot));
    g_assert_cmpint(snapshot.state, !=, RADIOS_AUDIO_UI_PLAYING);
    json_node_free(detail); window_close(&window);
}
static void test_preview_output_gain_isolation(void)
{
    Window window = window_open(); JsonNode *detail = media_import(&window); GError *error = NULL;
    GtkWidget *owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0), *other = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    g_object_ref_sink(owner); g_object_ref_sink(other);
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(detail), 0, TRUE, &error), &error);
    RadioSAudioUiSnapshot main_before = deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 200);
    run_for(200); goffset main_start = capture_offset("MAIN");
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); RadioSAudioSnapshot before;
    radios_audio_engine_snapshot(engine, &before); gdouble master_before = 0;
    g_assert_true(radios_audio_ui_get_mixer(window.ctx, NULL, NULL, &master_before));
    require_call(radios_audio_ui_preview_set_output(window.ctx, owner, RADIOS_AUDIO_UI_MONITOR, 0, &error), &error);
    require_call(radios_audio_ui_preview(window.ctx, owner, json_node_get_object(detail), 0, 0, &error), &error);
    deck_wait(&window, 32, RADIOS_AUDIO_UI_PLAYING, 200); run_for(150);
    goffset monitor_start = capture_offset("MONITOR"), cue_start = capture_offset("CUE"); run_for(250);
    gdouble peak = 0, full = capture_rms("MONITOR", monitor_start, &peak);
    g_assert_cmpfloat(full, >, 0.05); capture_rms("CUE", cue_start, &peak); g_assert_cmpfloat(peak, <, 0.00001);
    require_call(radios_audio_ui_preview_set_output(window.ctx, owner, RADIOS_AUDIO_UI_MONITOR, -6.020599913279624, &error), &error);
    run_for(150); monitor_start = capture_offset("MONITOR"); run_for(250);
    gdouble quiet = capture_rms("MONITOR", monitor_start, &peak);
    g_assert_cmpfloat(quiet / full, >, 0.45); g_assert_cmpfloat(quiet / full, <, 0.55);
    require_call(radios_audio_ui_preview_set_output(window.ctx, other, RADIOS_AUDIO_UI_CUE, -18, &error), &error);
    radios_audio_ui_preview_stop(window.ctx, other);
    g_assert_true(radios_audio_ui_preview_owned(window.ctx, owner));
    require_call(radios_audio_ui_preview_pause(window.ctx, owner, &error), &error);
    deck_wait(&window, 32, RADIOS_AUDIO_UI_PAUSED, 0);
    require_call(radios_audio_ui_preview_seek(window.ctx, owner, json_node_get_object(detail), 2800, &error), &error);
    deck_wait(&window, 32, RADIOS_AUDIO_UI_PAUSED, 2800);
    require_call(radios_audio_ui_preview_play(window.ctx, owner, json_node_get_object(detail), 2800, &error), &error);
    deck_wait(&window, 32, RADIOS_AUDIO_UI_PLAYING, 2900);
    wait_until(window_ready, &window, "CUE remains configured before switching the owned audition");
    require_call(radios_audio_ui_preview_set_output(window.ctx, owner, RADIOS_AUDIO_UI_CUE, -6.020599913279624, &error), &error);
    run_for(250); cue_start = capture_offset("CUE"); monitor_start = capture_offset("MONITOR"); run_for(250);
    capture_rms("MONITOR", monitor_start, &peak); g_assert_cmpfloat(peak, <, 0.00001);
    g_assert_cmpfloat(capture_rms("CUE", cue_start, &peak), >, 0.025);
    radios_audio_ui_preview_stop(window.ctx, owner);
    RadioSAudioUiBus bus = RADIOS_AUDIO_UI_MAIN; gdouble gain = 0;
    g_assert_true(radios_audio_ui_preview_get_output(window.ctx, owner, &bus, &gain));
    g_assert_cmpint(bus, ==, RADIOS_AUDIO_UI_CUE); g_assert_cmpfloat(gain, ==, -6.020599913279624);
    run_for(250); RadioSAudioSnapshot after; radios_audio_engine_snapshot(engine, &after);
    g_assert_cmpuint(after.decks[0].generation, ==, main_before.generation);
    g_assert_cmpint(after.decks[0].state, ==, RADIOS_AUDIO_PLAYING);
    g_assert_cmpuint(after.decks[0].cursor_frames, >, before.decks[0].cursor_frames);
    g_assert_cmpuint(after.buses[RADIOS_AUDIO_BUS_MAIN].consumed_frames, >, before.buses[RADIOS_AUDIO_BUS_MAIN].consumed_frames);
    gdouble master_after = 0; g_assert_true(radios_audio_ui_get_mixer(window.ctx, NULL, NULL, &master_after));
    g_assert_cmpfloat(master_after, ==, master_before);
    /* A ten-millisecond PCM window exposes any MAIN queue reset/dropout from
     * preview loading, gain, pause, seek, destination change or Stop. */
    g_assert_cmpfloat(capture_min_window_rms("MAIN", main_start), >, 0.05);
    gtk_widget_destroy(owner);
    if (radios_audio_ui_preview_get_output(window.ctx, owner, &bus, &gain)) {
        g_assert_cmpint(bus, ==, RADIOS_AUDIO_UI_CUE); g_assert_cmpfloat(gain, ==, 0);
    }
    gtk_widget_destroy(other); g_object_unref(owner); g_object_unref(other);
    radios_audio_engine_unref(engine); json_node_free(detail); window_close(&window);
}
typedef struct { Window *window; GtkWidget *owner; guint64 cue_in_ms, end_ms, furthest_ms; gboolean eof; } RangeEnd;
static gboolean range_end_reached(gpointer value)
{
    RangeEnd *range = value; RadioSAudioUiSnapshot snapshot = {0};
    if (!radios_audio_ui_get_snapshot(range->window->ctx, RADIOS_AUDIO_PREVIEW_DECK, &snapshot)) return FALSE;
    if (snapshot.state == RADIOS_AUDIO_UI_PLAYING) range->furthest_ms = MAX(range->furthest_ms, snapshot.position_ms);
    if (snapshot.state == RADIOS_AUDIO_UI_EOF && snapshot.position_ms >= range->end_ms) return range->eof = TRUE;
    /* The controller's 50 ms poll releases an ended audition (stop: READY at
     * the interval's cue-in, no error, no longer owned) and may see the end
     * before this test's poll does; having played up to the end proves it. */
    return snapshot.state == RADIOS_AUDIO_UI_READY && snapshot.position_ms == range->cue_in_ms && !snapshot.error[0] &&
        !radios_audio_ui_preview_owned(range->window->ctx, range->owner) && range->furthest_ms + 30 >= range->end_ms;
}
static void range_end_wait(Window *window, GtkWidget *owner, guint64 cue_in_ms, guint64 end_ms)
{
    RangeEnd range = {window, owner, cue_in_ms, end_ms, 0, FALSE};
    wait_until(range_end_reached, &range, "the audition interval plays to its end and ends there");
    RadioSAudioUiSnapshot snapshot; g_assert_true(radios_audio_ui_get_snapshot(window->ctx, RADIOS_AUDIO_PREVIEW_DECK, &snapshot));
    if (range.eof) g_assert_cmpuint(snapshot.position_ms, ==, end_ms);
}
static void test_preview_nondestructive_range(void)
{
    Window window = window_open(); JsonNode *detail = media_import(&window); GError *error = NULL;
    GtkWidget *owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(owner);
    JsonObject *version = json_object_get_object_member(json_node_get_object(detail), "version");
    const gchar *path = radios_json_string(version, "managed_path", ""), *expected = radios_json_string(version, "sha256", "");
    require_call(radios_audio_ui_preview_play_range(window.ctx, owner, json_node_get_object(detail), 0, 1000, 1600, &error), &error);
    RadioSAudioUiSnapshot snapshot = deck_wait(&window, 32, RADIOS_AUDIO_UI_PLAYING, 1100);
    g_assert_cmpuint(snapshot.cue_in_ms, ==, 1000); g_assert_cmpuint(snapshot.cue_out_ms, ==, 1600);
    require_call(radios_audio_ui_preview_pause(window.ctx, owner, &error), &error); deck_wait(&window, 32, RADIOS_AUDIO_UI_PAUSED, 0);
    require_call(radios_audio_ui_preview_seek(window.ctx, owner, json_node_get_object(detail), 0, &error), &error);
    snapshot = deck_wait(&window, 32, RADIOS_AUDIO_UI_PAUSED, 1000); g_assert_cmpuint(snapshot.position_ms, ==, 1000);
    require_call(radios_audio_ui_preview_play_range(window.ctx, owner, json_node_get_object(detail), 1000, 1000, 1600, &error), &error);
    range_end_wait(&window, owner, 1000, 1600);
    run_for(200); wait_until(window_ready, &window, "range EOF releases preview output before changing the interval");
    require_call(radios_audio_ui_preview_play_range(window.ctx, owner, json_node_get_object(detail), 2400, 2000, 2500, &error), &error);
    snapshot = deck_wait(&window, 32, RADIOS_AUDIO_UI_PLAYING, 2400);
    g_assert_cmpuint(snapshot.cue_in_ms, ==, 2000); g_assert_cmpuint(snapshot.cue_out_ms, ==, 2500);
    range_end_wait(&window, owner, 2000, 2500);
    gchar *bytes = NULL; gsize length = 0; g_assert_true(g_file_get_contents(path, &bytes, &length, &error)); g_assert_no_error(error);
    gchar *actual = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (guchar *)bytes, length);
    g_assert_cmpstr(actual, ==, expected); g_free(actual); g_free(bytes);
    gtk_widget_destroy(owner); g_object_unref(owner); json_node_free(detail); window_close(&window);
}
static void inline_waveform_seek(GtkWidget *panel, guint64 milliseconds)
{
    GtkWidget *canvas = find(panel, "qwf-timeline"); g_assert_true(GTK_IS_DRAWING_AREA(canvas));
    gdouble width = gtk_widget_get_allocated_width(canvas) - 32;
    gdouble x = 16 + width * milliseconds / 8000.0;
    GdkEventButton event = {0}; event.type = GDK_BUTTON_PRESS; event.button = 1;
    event.x = x;
    event.y = gtk_widget_get_allocated_height(canvas) - 35;
    gboolean handled = FALSE; g_signal_emit_by_name(canvas, "button-press-event", &event, &handled); g_assert_true(handled);
    /* A click seeks. Dragging the waveform now selects a nondestructive crop
     * interval, which is covered by the editor selection/crop UI test. */
    event.type = GDK_BUTTON_RELEASE; event.x = x; handled = FALSE;
    g_signal_emit_by_name(canvas, "button-release-event", &event, &handled); g_assert_true(handled);
}
typedef struct { RadioSViewContext *ctx; guint64 prior_generation; } BackwardSeek;
static gboolean backward_seek_complete(gpointer value)
{
    BackwardSeek *seek = value; RadioSAudioUiSnapshot snapshot = {0};
    return radios_audio_ui_get_snapshot(seek->ctx, RADIOS_AUDIO_PREVIEW_DECK, &snapshot) &&
        snapshot.generation > seek->prior_generation && snapshot.state == RADIOS_AUDIO_UI_PLAYING &&
        snapshot.position_ms >= 500 && snapshot.position_ms < 1500;
}
static void test_inline_preview_transport(void)
{
    Window window = window_open(); JsonNode *detail = media_import(&window);
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Embedded marker transport", GTK_WINDOW(window.root),
        GTK_DIALOG_DESTROY_WITH_PARENT, "Kapat", GTK_RESPONSE_CLOSE, NULL);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 1000, 900);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), scroll);
    GtkWidget *panel = radios_marker_editor_embed(window.ctx, dialog, json_node_get_object(detail));
    gtk_container_add(GTK_CONTAINER(scroll), panel); gtk_widget_show_all(dialog); run_for(250);
    g_assert_false(radios_marker_editor_dirty(panel));
    g_assert_nonnull(radios_marker_editor_detail(panel));
    click(panel, "qwf-preview-play"); deck_wait(&window, 32, RADIOS_AUDIO_UI_PLAYING, 150);
    g_assert_true(radios_audio_ui_preview_owned(window.ctx, dialog));
    click(panel, "qwf-preview-pause"); RadioSAudioUiSnapshot snapshot = deck_wait(&window, 32, RADIOS_AUDIO_UI_PAUSED, 0);
    guint64 paused = snapshot.position_ms; run_for(200);
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 32, &snapshot)); g_assert_cmpuint(snapshot.position_ms, ==, paused);
    inline_waveform_seek(panel, 2400); snapshot = deck_wait(&window, 32, RADIOS_AUDIO_UI_PAUSED, 2400);
    g_assert_cmpuint(snapshot.position_ms, ==, 2400);
    wait_until(window_ready, &window, "paused waveform seek output acknowledgement");
    click(panel, "qwf-preview-play"); snapshot = deck_wait(&window, 32, RADIOS_AUDIO_UI_PLAYING, 2500);
    BackwardSeek backwards = {window.ctx, snapshot.generation};
    inline_waveform_seek(panel, 500);
    /* A pre-seek playing snapshot at 2.5s also satisfies position >= 500.
     * Wait for the new decoder generation and its actual backward position. */
    wait_until(backward_seek_complete, &backwards, "playing waveform seek acknowledges a new generation at 500ms");
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 32, &snapshot));
    g_assert_cmpuint(snapshot.position_ms, >=, 500); g_assert_cmpuint(snapshot.position_ms, <, 1500);
    gtk_entry_set_text(GTK_ENTRY(find(panel, "qwf-cue-in")), "1.000"); g_assert_true(radios_marker_editor_dirty(panel));
    click(panel, "qwf-preview-cue"); snapshot = deck_wait(&window, 32, RADIOS_AUDIO_UI_PAUSED, 1000);
    g_assert_cmpuint(snapshot.position_ms, ==, 1000); run_for(180);
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 32, &snapshot)); g_assert_cmpuint(snapshot.position_ms, ==, 1000);
    wait_until(window_ready, &window, "Cue pauses without leaking to MAIN");
    goffset main_start = capture_offset("MAIN"); run_for(150); gdouble peak;
    capture_rms("MAIN", main_start, &peak); g_assert_cmpfloat(peak, <, 0.00001);
    GtkWidget *other = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(other); GError *error = NULL;
    require_call(radios_audio_ui_preview(window.ctx, other, json_node_get_object(detail), 0, 0, &error), &error);
    deck_wait(&window, 32, RADIOS_AUDIO_UI_PLAYING, 100);
    inline_waveform_seek(panel, 4000); click(panel, "qwf-preview-pause"); click(panel, "qwf-preview-cue"); run_for(100);
    g_assert_true(radios_audio_ui_preview_owned(window.ctx, other));
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 32, &snapshot));
    g_assert_cmpint(snapshot.state, ==, RADIOS_AUDIO_UI_PLAYING); g_assert_cmpuint(snapshot.position_ms, <, 2000);
    gtk_widget_destroy(dialog); run_for(100); g_assert_true(radios_audio_ui_preview_owned(window.ctx, other));
    gtk_widget_destroy(other); g_object_unref(other); json_node_free(detail); window_close(&window);
}

static gboolean inline_markers_saved(gpointer panel)
{
    JsonObject *detail = radios_marker_editor_detail(panel);
    JsonNode *node = detail ? json_object_get_member(detail, "markers") : NULL;
    return !radios_marker_editor_dirty(panel) && node && JSON_NODE_HOLDS_OBJECT(node) &&
        radios_json_int(json_node_get_object(node), "cue_in_ms", 0) == 1000;
}
static void test_inline_marker_picker_latest(void)
{
    Window window = window_open(); JsonNode *detail = media_import(&window);
    JsonObject *media = json_object_get_object_member(json_node_get_object(detail), "media");
    radios_media_library_open_for_deck(window.ctx, 0);
    radios_media_library_open_selected(window.ctx, radios_json_string(media, "_id", ""));
    wait_until(library_audio_ready, NULL, "native picker waits for a verified media detail");
    GtkWidget *library = dialog_find("media-library-dialog"), *panel = find(library, "media-marker-panel");
    g_assert_nonnull(panel); gtk_entry_set_text(GTK_ENTRY(find(panel, "qwf-cue-in")), "1.000");
    g_assert_true(radios_marker_editor_dirty(panel));
    gtk_button_clicked(GTK_BUTTON(find(library, "media-pick-accept"))); run_for(100);
    RadioSAudioUiSnapshot snapshot = {0}; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 0, &snapshot));
    g_assert_cmpint(snapshot.state, ==, RADIOS_AUDIO_UI_EMPTY);
    click(panel, "qwf-markers-save"); wait_until(inline_markers_saved, panel, "real marker CAS save acknowledged before picking");
    JsonObject *latest = radios_marker_editor_detail(panel);
    g_assert_cmpint(radios_json_int(json_object_get_object_member(latest, "media"), "_rev", 0), >, radios_json_int(media, "_rev", 0));
    click(library, "media-pick-accept"); snapshot = deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 1000);
    g_assert_cmpuint(snapshot.position_ms, ==, 1000);
    g_assert_cmpstr(snapshot.media_id, ==, radios_json_string(media, "_id", ""));
    gtk_widget_destroy(library); json_node_free(detail); window_close(&window);
}

static void expect_rejected(Window *window, JsonNode *detail)
{
    GError *error = NULL;
    if (radios_audio_ui_load_media(window->ctx, window->root, json_node_get_object(detail), 0, FALSE, &error))
        deck_wait(window, 0, RADIOS_AUDIO_UI_ERROR, 0);
    else { g_assert_nonnull(error); g_clear_error(&error); }
}
static void test_identity_path_hash(void)
{
    Window window = window_open(); JsonNode *detail = media_import(&window), *bad = deep_copy(detail);
    JsonObject *media = json_object_get_object_member(json_node_get_object(bad), "media");
    json_object_set_string_member(media, "station_id", "00000000-0000-4000-8000-000000000001"); expect_rejected(&window, bad); json_node_free(bad);
    bad = deep_copy(detail); JsonObject *version = json_object_get_object_member(json_node_get_object(bad), "version");
    json_object_set_boolean_member(version, "verified", FALSE); expect_rejected(&window, bad); json_node_free(bad);
    bad = deep_copy(detail); version = json_object_get_object_member(json_node_get_object(bad), "version");
    json_object_set_string_member(version, "managed_path", config.database_file); expect_rejected(&window, bad); json_node_free(bad);
    bad = deep_copy(detail); version = json_object_get_object_member(json_node_get_object(bad), "version");
    json_object_set_string_member(version, "sha256", "0000000000000000000000000000000000000000000000000000000000000000");
    expect_rejected(&window, bad); json_node_free(bad);
    /* A correct-looking managed path cannot smuggle a symlink into the reader. */
    version = json_object_get_object_member(json_node_get_object(detail), "version");
    gchar *objects = g_path_get_dirname(radios_json_string(version, "managed_path", ""));
    gchar *link = g_build_filename(objects, "audio-ui-link.wav", NULL);
    g_assert_cmpint(symlink(radios_json_string(version, "managed_path", ""), link), ==, 0);
    bad = deep_copy(detail); version = json_object_get_object_member(json_node_get_object(bad), "version");
    json_object_set_string_member(version, "managed_path", link); expect_rejected(&window, bad); json_node_free(bad);
    g_assert_cmpint(g_unlink(link), ==, 0); g_free(link); g_free(objects);
    json_node_free(detail); window_close(&window);
}
static gboolean reload_dialog(gpointer unused) { (void)unused; return dialog_find("database-reload-dialog") != NULL; }
static gboolean microphone_gain_saved(gpointer unused)
{
    (void)unused; JsonNode *record = preferences_read(); JsonObject *preferences = json_object_get_object_member(json_node_get_object(record), "config");
    gboolean result = fabs(json_object_get_double_member(preferences, "microphone_gain_db") - 7.0) < 0.001;
    json_node_free(record); return result;
}
static void test_preferences_cas_reopen(void)
{
    Window window = window_open(); JsonNode *before = preferences_read(), *changed = deep_copy(before);
    JsonObject *preferences = json_object_get_object_member(json_node_get_object(changed), "config");
    combo(window.root, "task-cue-output", monitor_sink); /* Unsaved local device choice. */
    json_object_set_double_member(preferences, "main_gain_db", -3.0);
    JsonNode *remote = preferences_remote_save(before, preferences); json_node_free(changed);
    click(window.root, "task-audio-apply"); wait_until(reload_dialog, NULL, "stale preference revision is rejected");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(window.root, "task-cue-output"))), ==, monitor_sink);
    JsonNode *current = preferences_read(); JsonObject *row = json_node_get_object(current);
    g_assert_cmpint(radios_json_int(row, "_rev", 0), ==, radios_json_int(json_node_get_object(remote), "_rev", 0));
    g_assert_cmpstr(radios_json_string(json_object_get_object_member(row, "config"), "cue_sink", ""), ==, cue_sink);
    json_node_free(current); gtk_dialog_response(GTK_DIALOG(dialog_find("database-reload-dialog")), GTK_RESPONSE_CANCEL);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(window.root, "task-cue-output"))), ==, monitor_sink);
    click(window.root, "task-audio-apply"); wait_until(reload_dialog, NULL, "explicit preference reload is offered");
    gtk_dialog_response(GTK_DIALOG(dialog_find("database-reload-dialog")), GTK_RESPONSE_ACCEPT);
    wait_until(window_ready, &window, "explicit reload restores the remote revision and device choices");
    g_assert_cmpfloat(gtk_adjustment_get_value(adjustment(window.root, "header-main-gain")), ==, -3.0);
    gtk_adjustment_set_value(adjustment(window.root, "task-microphone-gain"), 7.0);
    click(window.root, "task-audio-apply"); wait_until(microphone_gain_saved, NULL, "microphone gain persists through a successful preference CAS");
    toggle(window.root, "task-microphone-gate", TRUE); toggle(window.root, "header-headphone-cue", TRUE);
    toggle(window.root, "header-output-cue", TRUE); run_for(150);
    current = preferences_read(); preferences = json_object_get_object_member(json_node_get_object(current), "config");
    g_assert_false(json_object_has_member(preferences, "microphone_gate")); g_assert_false(json_object_has_member(preferences, "playing"));
    json_node_free(current); window_close(&window); window = window_open(); assert_gates_off(&window);
    g_assert_cmpfloat(gtk_adjustment_get_value(adjustment(window.root, "task-microphone-gain")), ==, 7.0);
    g_assert_cmpfloat(gtk_adjustment_get_value(adjustment(window.root, "header-main-gain")), ==, -3.0);
    window_close(&window); json_node_free(before); json_node_free(remote);
}
static void test_generation_close(void)
{
    Window window = window_open(); JsonNode *detail = media_import(&window); GError *error = NULL;
    RadioSAudioUiSnapshot before; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 0, &before));
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(detail), 0, TRUE, &error), &error);
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(detail), 0, FALSE, &error), &error);
    RadioSAudioUiSnapshot final = deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    g_assert_cmpuint(final.generation, >, before.generation); run_for(200);
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 0, &final)); g_assert_cmpint(final.state, ==, RADIOS_AUDIO_UI_READY);
    g_assert_cmpuint(final.position_ms, ==, 0);
    /* A normal player's transport belongs to the controller (audio_ui.c
     * owner_destroyed): rebuilding or hiding its card destroys the owner
     * widget, but a song on air must not stop. The owner link is dropped while
     * the Play intent and the transport stay; the controller still stops it. */
    GtkWidget *owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(owner);
    require_call(radios_audio_ui_load_media(window.ctx, owner, json_node_get_object(detail), 1, TRUE, &error), &error);
    gtk_widget_destroy(owner); g_object_unref(owner);
    final = deck_wait(&window, 1, RADIOS_AUDIO_UI_PLAYING, 100); guint64 position = final.position_ms; run_for(250);
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 1, &final)); g_assert_cmpint(final.state, ==, RADIOS_AUDIO_UI_PLAYING);
    g_assert_cmpuint(final.position_ms, >, position);
    require_call(radios_audio_ui_stop(window.ctx, 1, &error), &error); deck_wait(&window, 1, RADIOS_AUDIO_UI_READY, 0);
    owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(owner);
    require_call(radios_audio_ui_load_media(window.ctx, owner, json_node_get_object(detail), 1, FALSE, &error), &error);
    deck_wait(&window, 1, RADIOS_AUDIO_UI_READY, 0);
    wait_until(window_ready, &window, "manually started owner deck has an acknowledged virtual output");
    require_call(radios_audio_ui_play(window.ctx, 1, &error), &error); final = deck_wait(&window, 1, RADIOS_AUDIO_UI_PLAYING, 100);
    position = final.position_ms; gtk_widget_destroy(owner); g_object_unref(owner); run_for(250);
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 1, &final)); g_assert_cmpint(final.state, ==, RADIOS_AUDIO_UI_PLAYING);
    g_assert_cmpuint(final.position_ms, >, position);
    require_call(radios_audio_ui_stop(window.ctx, 1, &error), &error); deck_wait(&window, 1, RADIOS_AUDIO_UI_READY, 0);
    /* An auxiliary sampler voice and the preview deck exist only for their
     * owner widget: destroying the owner stops them. */
    owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(owner);
    require_call(radios_audio_ui_load_media(window.ctx, owner, json_node_get_object(detail), RADIOS_AUDIO_SAMPLER_BASE, TRUE, &error), &error);
    deck_wait(&window, RADIOS_AUDIO_SAMPLER_BASE, RADIOS_AUDIO_UI_PLAYING, 100);
    gtk_widget_destroy(owner); g_object_unref(owner); run_for(250);
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, RADIOS_AUDIO_SAMPLER_BASE, &final)); g_assert_cmpint(final.state, !=, RADIOS_AUDIO_UI_PLAYING);
    wait_until(window_ready, &window, "outputs settle before the preview owner case");
    owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(owner);
    require_call(radios_audio_ui_preview(window.ctx, owner, json_node_get_object(detail), 0, 2000, &error), &error);
    deck_wait(&window, RADIOS_AUDIO_PREVIEW_DECK, RADIOS_AUDIO_UI_PLAYING, 100);
    gtk_widget_destroy(owner); g_object_unref(owner); run_for(250);
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, RADIOS_AUDIO_PREVIEW_DECK, &final)); g_assert_cmpint(final.state, !=, RADIOS_AUDIO_UI_PLAYING);
    RadioSAudioController *retained = radios_audio_ui_ref(window.ctx->audio);
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(detail), 0, TRUE, &error), &error);
    window_close(&window); radios_audio_ui_close(retained); radios_audio_ui_unref(retained); run_for(250);
    window = window_open(); assert_gates_off(&window); window_close(&window); json_node_free(detail);
}
static GtkWidget *card_remove_button(GtkWidget *root, GtkWidget *card)
{
    if (GTK_IS_BUTTON(root) && g_object_get_data(G_OBJECT(root), "radios-player-card") == card) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = card_remove_button(item->data, card);
    g_list_free(children); return result;
}
static gboolean toggle_active(gpointer widget)
{ return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)); }
static void test_card_cue_owner(void)
{
    Window window = window_open(); JsonNode *detail = media_import(&window), *newer = media_import(&window);
    JsonObject *media = json_object_get_object_member(json_node_get_object(detail), "media");
    JsonObject *newer_media = json_object_get_object_member(json_node_get_object(newer), "media");
    g_assert_cmpstr(radios_json_string(media, "_id", ""), !=, radios_json_string(newer_media, "_id", ""));
    radios_media_library_open_for_deck(window.ctx, 0);
    radios_media_library_open_selected(window.ctx, radios_json_string(media, "_id", ""));
    wait_until(library_audio_ready, NULL, "selected library verified detail enables card load");
    GtkWidget *library = dialog_find("media-library-dialog");
    click(library, "media-pick-accept");
    deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    gtk_dialog_response(GTK_DIALOG(library), GTK_RESPONSE_CANCEL);
    wait_until(window_ready, &window, "card load acknowledges the configured virtual outputs");
    GtkWidget *card = g_object_get_data(G_OBJECT(window.root), "radios-deck-owner-0"); g_assert_nonnull(card);
    run_for(150); goffset main_start = capture_offset("MAIN"), cue_start = capture_offset("CUE");
    click(window.root, "deck-0-cue");
    RadioSAudioUiSnapshot snapshot = deck_wait(&window, 32, RADIOS_AUDIO_UI_PLAYING, 100);
    g_assert_cmpstr(snapshot.media_id, ==, radios_json_string(media, "_id", ""));
    g_assert_true(radios_audio_ui_preview_owned(window.ctx, card)); g_assert_false(snapshot.main_enabled);
    run_for(250); gdouble main_peak, cue_peak;
    gdouble main_rms = capture_rms("MAIN", main_start, &main_peak), cue_rms = capture_rms("CUE", cue_start, &cue_peak);
    g_assert_cmpfloat(cue_peak, >, 0.1); g_assert_cmpfloat(cue_rms, >, 0.05);
    g_assert_cmpfloat(main_peak, <, 0.00001); g_assert_cmpfloat(main_rms, <, 0.00001);

    /* A re-click must not leave a late GET able to replace a newer owner's
     * preview or undo the user's subsequent CUE-off action. The private HTTP
     * fixture holds any such request until after that action. No pending GET
     * is required for the valid implementation to satisfy these behaviors. */
    gchar *database_dir = g_path_get_dirname(config.database_file);
    gchar *gate = g_build_filename(database_dir, ".acceptance-media-get-hold", NULL); g_free(database_dir);
    g_assert_true(g_file_set_contents(gate, "hold", -1, NULL)); g_assert_cmpint(g_chmod(gate, 0600), ==, 0);
    click(window.root, "deck-0-cue"); run_for(50);
    wait_until(window_ready, &window, "card CUE replacement acknowledges its virtual output before the newer preview");
    radios_marker_editor_open_media(window.ctx, json_node_get_object(newer));
    GtkWidget *editor = dialog_find("queue-waveform-dialog"); g_assert_nonnull(editor);
    click(editor, "qwf-preview-play"); snapshot = deck_wait(&window, 32, RADIOS_AUDIO_UI_PLAYING, 100);
    g_assert_cmpstr(snapshot.media_id, ==, radios_json_string(newer_media, "_id", ""));
    g_assert_true(radios_audio_ui_preview_owned(window.ctx, editor));
    GtkWidget *cue_toggle = find(window.root, "header-headphone-cue"); g_assert_true(GTK_IS_TOGGLE_BUTTON(cue_toggle));
    wait_until(toggle_active, cue_toggle, "CUE gate reflects the active preview before user-off");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cue_toggle), FALSE);
    g_assert_cmpint(g_unlink(gate), ==, 0); g_free(gate); run_for(500);
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 32, &snapshot));
    g_assert_cmpint(snapshot.state, !=, RADIOS_AUDIO_UI_PLAYING);
    g_assert_false(radios_audio_ui_preview_owned(window.ctx, editor)); g_assert_false(toggle_active(cue_toggle));

    /* Removing an old card must leave a newer editor's actual CUE stream alive.
     * This also exercises the normal native remove action and owner teardown. */
    wait_until(window_ready, &window, "CUE-off output flush completes before a new editor preview");
    click(editor, "qwf-preview-play"); snapshot = deck_wait(&window, 32, RADIOS_AUDIO_UI_PLAYING, 100);
    guint64 generation = snapshot.generation;
    GtkWidget *remove = card_remove_button(card, card); g_assert_nonnull(remove);
    gtk_button_clicked(GTK_BUTTON(remove)); run_for(250);
    g_assert_null(g_object_get_data(G_OBJECT(window.root), "radios-deck-owner-0"));
    snapshot = deck_wait(&window, 32, RADIOS_AUDIO_UI_PLAYING, 100);
    g_assert_cmpuint(snapshot.generation, ==, generation);
    g_assert_cmpstr(snapshot.media_id, ==, radios_json_string(newer_media, "_id", ""));
    g_assert_true(radios_audio_ui_preview_owned(window.ctx, editor));
    cue_start = capture_offset("CUE"); run_for(250); cue_rms = capture_rms("CUE", cue_start, &cue_peak);
    g_assert_cmpfloat(cue_peak, >, 0.1); g_assert_cmpfloat(cue_rms, >, 0.05);
    wait_until(toggle_active, cue_toggle, "new owner's CUE gate remains active after old card removal");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(cue_toggle), FALSE); run_for(250);
    cue_start = capture_offset("CUE"); run_for(500); capture_rms("CUE", cue_start, &cue_peak);
    g_assert_cmpfloat(cue_peak, <, 0.00001); g_assert_false(toggle_active(cue_toggle));
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 32, &snapshot)); g_assert_cmpint(snapshot.state, !=, RADIOS_AUDIO_UI_PLAYING);
    gtk_dialog_response(GTK_DIALOG(editor), GTK_RESPONSE_CANCEL);
    json_node_free(detail); json_node_free(newer); window_close(&window);
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc; g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    main_sink = g_getenv("RADIOS_TEST_MAIN_SINK"); cue_sink = g_getenv("RADIOS_TEST_CUE_SINK"); monitor_sink = g_getenv("RADIOS_TEST_MONITOR_SINK");
    g_assert_nonnull(main_sink); g_assert_nonnull(cue_sink); g_assert_nonnull(monitor_sink);
    g_assert_true(g_str_has_prefix(main_sink, "radios_gui_")); g_assert_true(g_str_has_prefix(cue_sink, "radios_gui_"));
    GError *error = NULL; g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    application = gtk_application_new("org.radios.NativeAudioAcceptance", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/native-audio/transport-real-PCM", test_transport);
    g_test_add_func("/native-audio/preview-CUE-main-silence", test_preview_routing);
    g_test_add_func("/native-audio/preview-output-gain-main-isolation", test_preview_output_gain_isolation);
    g_test_add_func("/native-audio/preview-nondestructive-cue-range", test_preview_nondestructive_range);
    g_test_add_func("/native-audio/inline-waveform-transport-owner", test_inline_preview_transport);
    g_test_add_func("/native-audio/inline-markers-picker-latest", test_inline_marker_picker_latest);
    g_test_add_func("/native-audio/station-path-hash-rejection", test_identity_path_hash);
    g_test_add_func("/native-audio/preferences-CAS-reopen", test_preferences_cas_reopen);
    g_test_add_func("/native-audio/generation-owner-close", test_generation_close);
    g_test_add_func("/native-audio/card-CUE-loaded-owner", test_card_cue_owner);
    gint result = g_test_run(); radios_data_client_unref(reader); g_object_unref(application); radios_config_clear(&config); return result;
}
