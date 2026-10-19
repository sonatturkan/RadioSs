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
    if (!condition(data)) g_error("Native DJ acceptance timed out: %s", description);
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
static gboolean microphone_open_ready(gpointer value)
{
    Window *window = value; GtkWidget *on_air = find(window->root, "dj-speech-on-air");
    /* A STOP can finish decoding before the physical output flush completes;
     * the button's previous polling result is not a live readiness fence. */
    return radios_audio_ui_ready(window->ctx, RADIOS_AUDIO_UI_MAIN) &&
        radios_audio_ui_ready(window->ctx, RADIOS_AUDIO_UI_MICROPHONE) &&
        on_air && gtk_widget_is_sensitive(on_air);
}
static Window window_create(void)
{
    Window window = {0}; GError *error = NULL; window.runtime = radios_runtime_new(64, 128);
    window.window = radios_window_new(application, &config, window.runtime, FALSE, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(window.window);
    window.ctx = radios_window_view_context(window.window); g_assert_nonnull(window.ctx); g_assert_nonnull(window.ctx->audio);
    window.root = GTK_WIDGET(window.ctx->parent); radios_window_present(window.window);
    return window;
}
static Window window_open(void)
{
    Window window = window_create();
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
    const gchar *names[] = {"task-microphone-gate", "header-headphone-cue", "header-output-cue", "dj-speech-on-air"};
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
    if (!deck_matches(&condition))
        g_error("Deck %u expected state %d/cursor >= %" G_GUINT64_FORMAT "; actual state %d/cursor %" G_GUINT64_FORMAT "/generation %" G_GUINT64_FORMAT ": %s",
            deck, state, position, snapshot.state, snapshot.position_ms, snapshot.generation, snapshot.error);
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
    Request task = {reader, path, body, NULL, NULL, 0}; GThread *thread = g_thread_new("DJ-DB-read", request_work, &task);
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
static JsonNode *media_import(Window *window, const gchar *title, gdouble frequency, gdouble amplitude)
{
    guint frames = 48000 * 20 + 31, bytes = frames * 4; guchar *wav = g_malloc(44 + bytes);
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
        "jingle", title, import_complete, &output, import_destroy);
    g_assert_nonnull(handle); wait_until(import_done, &output, "real SHA-bound PCM analysis/import");
    g_assert_no_error(output.error); g_assert_nonnull(output.result); g_assert_cmpuint(output.callbacks, ==, 1);
    JsonObject *media = json_object_get_object_member(json_node_get_object(output.result), "media");
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "media_id", radios_json_string(media, "_id", ""));
    JsonNode *detail = request("/v1/workflows/desktop-media/get", body); json_node_free(body); json_node_free(output.result);
    JsonObject *version = json_object_get_object_member(json_node_get_object(detail), "version");
    g_assert_true(json_object_get_boolean_member(version, "verified")); g_assert_true(json_object_get_boolean_member(version, "object_available"));
    g_assert_cmpint(radios_json_int(version, "duration_ms", 0), ==, 20001);
    g_assert_cmpstr(radios_json_string(version, "status", ""), ==, "draft");
    g_assert_true(g_file_test(radios_json_string(version, "managed_path", ""), G_FILE_TEST_IS_REGULAR));
    g_assert_cmpint(g_unlink(path), ==, 0); g_free(path); g_object_unref(handle); return detail;
}

typedef struct { gdouble rms[2], peak[2]; } Capture;
static goffset capture_offset(const gchar *bus)
{
    gchar *key = g_strdup_printf("RADIOS_TEST_%s_CAPTURE", bus); const gchar *path = g_getenv(key);
    g_assert_nonnull(path); struct stat status; g_assert_cmpint(g_stat(path, &status), ==, 0);
    g_free(key); return status.st_size - status.st_size % 8;
}
static Capture capture(const gchar *bus, goffset offset)
{
    Capture result = {{0, 0}, {0, 0}}; long double squares[2] = {0, 0}; guint64 frames = 0;
    gchar *key = g_strdup_printf("RADIOS_TEST_%s_CAPTURE", bus); const gchar *path = g_getenv(key);
    g_assert_nonnull(path); gint fd = open(path, O_RDONLY); g_assert_cmpint(fd, >=, 0); g_free(key);
    struct stat status; g_assert_cmpint(fstat(fd, &status), ==, 0);
    g_assert_cmpint(status.st_size - offset, >, 4800 * 8); gfloat buffer[4096];
    while (offset + 8 <= status.st_size) {
        ssize_t bytes = pread(fd, buffer, (size_t)MIN((goffset)sizeof buffer, status.st_size - offset), offset);
        g_assert_cmpint(bytes, >, 0); g_assert_cmpint(bytes % 8, ==, 0);
        for (guint i = 0; i < (guint)bytes / 8; ++i) for (guint channel = 0; channel < 2; ++channel) {
            gfloat sample = buffer[i * 2 + channel]; g_assert_true(isfinite(sample));
            squares[channel] += (long double)sample * sample; result.peak[channel] = MAX(result.peak[channel], fabs(sample));
        }
        frames += (guint64)bytes / 8; offset += bytes;
    }
    close(fd); for (guint i = 0; i < 2; ++i) result.rms[i] = sqrt((gdouble)(squares[i] / frames)); return result;
}
static Capture measure(const gchar *bus)
{ goffset offset = capture_offset(bus); run_for(300); return capture(bus, offset); }
static void wait_main_effect_capture(Window *window, guint delay_frames)
{
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window->ctx); g_assert_nonnull(engine);
    RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot); radios_audio_engine_unref(engine);
    const RadioSAudioBusSnapshot *main = &snapshot.buses[RADIOS_AUDIO_BUS_MAIN];
    g_assert_true(main->ready); g_assert_cmpuint(main->output_sample_rate, >, 0);
    g_assert_cmpuint(main->output_buffer_frames, >, 0);
    /* The recorder requests 48 kHz even when Pulse plays at another rate. */
    guint64 output_capture_frames = ((guint64)main->output_buffer_frames * 48000 +
        main->output_sample_rate - 1) / main->output_sample_rate;
    /* A route change resets DELAY history. Discard its source-frame delay,
     * the actual Pulse output buffer and the recorder's 10 ms buffer before
     * measuring steady RETURN gain. Capture growth proves real output frames;
     * a wall-clock sleep alone can still sample the reset's dry-only prefix. */
    guint64 needed = MAX(G_GUINT64_CONSTANT(24000),
        (guint64)delay_frames + output_capture_frames + 480);
    goffset start = capture_offset("MAIN");
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while ((guint64)(capture_offset("MAIN") - start) / 8 < needed &&
           g_get_monotonic_time() < deadline) pump();
    guint64 captured = (guint64)(capture_offset("MAIN") - start) / 8;
    g_test_message("RETURN warmup discarded %" G_GUINT64_FORMAT
        " actual MAIN frames (delay=%u, output=%u Hz/%u frames, capture buffer=%"
        G_GUINT64_FORMAT ", minimum=%" G_GUINT64_FORMAT ")",
        captured, delay_frames, main->output_sample_rate, main->output_buffer_frames,
        output_capture_frames, needed);
    g_assert_cmpuint(captured, >=, needed);
}
static guint64 capture_signal_span(const gchar *bus, goffset offset)
{
    gchar *key = g_strdup_printf("RADIOS_TEST_%s_CAPTURE", bus); const gchar *path = g_getenv(key);
    g_assert_nonnull(path); gint fd = open(path, O_RDONLY); g_assert_cmpint(fd, >=, 0); g_free(key);
    struct stat status; g_assert_cmpint(fstat(fd, &status), ==, 0); gfloat buffer[4096];
    guint64 frame = 0, first = G_MAXUINT64, last = 0;
    while (offset + 8 <= status.st_size) {
        ssize_t bytes = pread(fd, buffer, (size_t)MIN((goffset)sizeof buffer, status.st_size - offset), offset);
        g_assert_cmpint(bytes, >, 0); g_assert_cmpint(bytes % 8, ==, 0);
        for (guint i = 0; i < (guint)bytes / 8; ++i, ++frame) {
            g_assert_true(isfinite(buffer[i * 2])); g_assert_true(isfinite(buffer[i * 2 + 1]));
            if (fabs(buffer[i * 2]) > 0.001 || fabs(buffer[i * 2 + 1]) > 0.001) {
                if (first == G_MAXUINT64) first = frame;
                last = frame;
            }
        }
        offset += bytes;
    }
    close(fd); return first == G_MAXUINT64 ? 0 : last - first + 1;
}
static void require_call(gboolean result, GError **error)
{ g_assert_no_error(*error); g_assert_true(result); }
static void knob(GtkWidget *root, const gchar *name, gdouble value)
{ gtk_adjustment_set_value(adjustment(root, name), value); }
static void range(GtkWidget *root, const gchar *name, gdouble value)
{ GtkWidget *widget = find(root, name); g_assert_true(GTK_IS_RANGE(widget)); gtk_range_set_value(GTK_RANGE(widget), value); }
static void combo_index(GtkWidget *root, const gchar *name, gint index)
{ GtkWidget *widget = find(root, name); g_assert_true(GTK_IS_COMBO_BOX(widget)); gtk_combo_box_set_active(GTK_COMBO_BOX(widget), index); }
static void entry(GtkWidget *root, const gchar *name, const gchar *text)
{ GtkWidget *widget = find(root, name); g_assert_true(GTK_IS_ENTRY(widget)); gtk_entry_set_text(GTK_ENTRY(widget), text); }
static gboolean dialog_exists(gpointer name) { return dialog_find(name) != NULL; }
static gboolean dialog_closed(gpointer name) { return dialog_find(name) == NULL; }
static JsonNode *document_read(const gchar *kind, const gchar *name)
{
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "kind", kind);
    JsonNode *result = request("/v1/workflows/desktop/list", body); json_node_free(body);
    JsonArray *records = json_object_get_array_member(json_node_get_object(result), "records"); JsonNode *found = NULL;
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        JsonObject *record = json_array_get_object_element(records, i);
        if (!g_strcmp0(radios_json_string(record, "name", ""), name)) { found = deep_copy(json_array_get_element(records, i)); break; }
    }
    json_node_free(result); return found;
}
typedef gboolean (*RecordCondition)(JsonObject *config);
static JsonNode *document_wait(const gchar *kind, const gchar *name, RecordCondition condition)
{
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND; JsonNode *record = NULL;
    do {
        g_clear_pointer(&record, json_node_free); record = document_read(kind, name);
        if (record && (!condition || condition(json_object_get_object_member(json_node_get_object(record), "config")))) return record;
        run_for(10);
    } while (g_get_monotonic_time() < deadline);
    g_error("DJ database readback timed out: %s/%s", kind, name); return NULL;
}
static JsonNode *document_remote_save(JsonNode *record, JsonObject *configuration)
{
    JsonObject *row = json_node_get_object(record); JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", radios_json_string(row, "kind", ""));
    json_object_set_string_member(object, "name", radios_json_string(row, "name", ""));
    json_object_set_string_member(object, "document_id", radios_json_string(row, "_id", ""));
    json_object_set_int_member(object, "expected_revision", radios_json_int(row, "_rev", 0));
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    json_object_set_object_member(object, "config", json_object_ref(configuration));
    JsonNode *result = request("/v1/workflows/desktop/put", body); json_node_free(body); return result;
}
typedef struct { GtkWidget *root; const gchar *id; gint64 revision; } DocumentAcknowledgement;
static gboolean document_acknowledged_in(GtkWidget *root, const gchar *id, gint64 revision)
{
    GPtrArray *documents = g_object_get_data(G_OBJECT(root), "radios-data-documents");
    for (guint i = 0; documents && i < documents->len; ++i) {
        RadioSDocument *document = g_ptr_array_index(documents, i);
        if (!g_strcmp0(radios_data_document_id(document), id) &&
            radios_data_document_revision(document) >= revision) return TRUE;
    }
    if (!GTK_IS_CONTAINER(root)) return FALSE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); gboolean accepted = FALSE;
    for (GList *item = children; item && !accepted; item = item->next)
        accepted = document_acknowledged_in(item->data, id, revision);
    g_list_free(children); return accepted;
}
static gboolean document_acknowledged(gpointer value)
{
    DocumentAcknowledgement *expected = value;
    return document_acknowledged_in(expected->root, expected->id, expected->revision);
}
static void document_ack_wait(Window *window, JsonNode *record)
{
    JsonObject *row = json_node_get_object(record);
    DocumentAcknowledgement expected = {window->root, radios_json_string(row, "_id", ""), radios_json_int(row, "_rev", 0)};
    g_assert_cmpint(expected.revision, >, 0);
    wait_until(document_acknowledged, &expected, "native form accepts the persisted DB revision and latest pending save");
}
static JsonObject *channel_config(JsonObject *config, guint channel)
{ return json_array_get_object_element(json_object_get_array_member(config, "channels"), channel); }
static gboolean initial_saved(JsonObject *configuration)
{
    if (fabs(json_object_get_double_member(configuration, "crossfader")) > 0.001) return FALSE;
    for (guint i = 0; i < RADIOS_AUDIO_MIXER_CHANNELS; ++i) {
        JsonObject *channel = channel_config(configuration, i);
        const gchar *controls[] = {"gain_db", "eq_low_db", "eq_mid_db", "eq_high_db", "pan"};
        for (guint j = 0; j < G_N_ELEMENTS(controls); ++j)
            if (fabs(json_object_get_double_member(channel, controls[j])) > 0.001) return FALSE;
        if (json_object_get_boolean_member(channel, "mute") || json_object_get_boolean_member(channel, "solo")) return FALSE;
    }
    JsonObject *speech = json_object_get_object_member(configuration, "speech");
    return json_object_get_boolean_member(speech, "ducking") &&
        fabs(json_object_get_double_member(speech, "bed_db") + 18) < 0.001;
}
static gboolean initial_master_saved(JsonObject *configuration)
{ return fabs(json_object_get_double_member(configuration, "main_gain_db")) < 0.001; }
static void reset_mixer(Window *window)
{
    RadioSAudioMixerConfig configuration; radios_audio_mixer_config_default(&configuration); GError *error = NULL;
    require_call(radios_audio_ui_set_mixer(window->ctx, &configuration, &error), &error);
    require_call(radios_audio_ui_set_master_gain(window->ctx, 0, &error), &error);
    /* Real controller mutations must persist without a manual save action. */
    JsonNode *record = document_wait("dj_mixer", "DJ mixer", initial_saved);
    document_ack_wait(window, record); json_node_free(record);
    record = document_wait("audio_preferences", "Ses aygıtları", initial_master_saved);
    document_ack_wait(window, record); json_node_free(record);
}
static gboolean persisted_controls(JsonObject *config)
{
    JsonObject *channel = channel_config(config, 0); JsonObject *fx = json_array_get_object_element(json_object_get_array_member(config, "effects"), 3);
    return fabs(json_object_get_double_member(channel, "gain_db") + 8.5) < 0.001 &&
        fabs(json_object_get_double_member(channel, "eq_low_db") + 12) < 0.001 &&
        json_object_get_boolean_member(channel, "loudness_bypass") &&
        fabs(json_object_get_double_member(config, "crossfader") - 0.25) < 0.001 &&
        json_object_get_int_member(fx, "type") == 6 && json_object_get_boolean_member(fx, "enabled");
}
static gboolean master_gain_saved(JsonObject *config)
{ return fabs(json_object_get_double_member(config, "main_gain_db") + 4) < 0.001; }
static gboolean pending_device_mixer_saved(JsonObject *config)
{ return fabs(json_object_get_double_member(channel_config(config, 0), "eq_mid_db") - 4) < 0.001; }
typedef struct { Window *window; gboolean cue; } EffectorDisplayExpectation;
static void capture_dj(Window *window, const gchar *name);
static gboolean effector_display_ready(gpointer value)
{
    EffectorDisplayExpectation *expected = value; GtkWidget *root = expected->window->root;
    GtkWidget *display = find(root, "dj-fx-4-display"), *bpm = find(root, "dj-fx-4-display-bpm");
    if (!GTK_IS_BOX(display) || !gtk_widget_get_mapped(display) || !GTK_IS_DRAWING_AREA(bpm) ||
        g_strcmp0(atk_object_get_name(gtk_widget_get_accessible(bpm)), "142.0 BPM")) return FALSE;
    const gchar *names[] = {"effect", "depth", "state", "cue", "beat"};
    const gchar *texts[] = {"FLANGER", "75%", "ON", expected->cue ? "CUE" : "CUE -", "423 ms"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        gchar *name = g_strdup_printf("dj-fx-4-display-%s", names[i]);
        GtkWidget *label = find(root, name); g_free(name);
        if (!GTK_IS_LABEL(label) || g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), texts[i])) return FALSE;
    }
    return TRUE;
}
static void assert_effector_display(Window *window, gboolean cue)
{
    EffectorDisplayExpectation expected = {window, cue};
    wait_until(effector_display_ready, &expected,
        cue ? "mapped LCD reflects accepted live FX4 controls and runtime CUE" :
              "mapped LCD restores persisted FX4 controls and clears runtime CUE");
}
static void test_persistence(void)
{
    Window window = window_open(); assert_gates_off(&window);
    radios_window_select_profile(window.window, "dj");
    knob(window.root, "dj-channel-1-eq-low", -12); knob(window.root, "dj-channel-1-eq-mid", 3);
    knob(window.root, "dj-channel-1-pan", -50); range(window.root, "dj-channel-1-gain", -8.5);
    combo(window.root, "dj-channel-1-effector", "3"); combo(window.root, "dj-channel-1-cross-group", "1");
    toggle(window.root, "dj-channel-1-normalize", FALSE);
    knob(window.root, "dj-channel-1-send", -9); knob(window.root, "dj-channel-1-return", -10);
    combo_index(window.root, "dj-fx-4-type", 6); knob(window.root, "dj-fx-4-depth", 75);
    GtkWidget *tempo = find(window.root, "dj-fx-4-tempo"); g_assert_true(GTK_IS_SPIN_BUTTON(tempo));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(tempo), 142); toggle(window.root, "dj-fx-4-power", TRUE);
    knob(window.root, "dj-channel-0-eq-high", -2); range(window.root, "dj-channel-0-gain", -4);
    combo(window.root, "dj-channel-0-effector", "2");
    range(window.root, "dj-crossfader", 0.25); range(window.root, "dj-speech-bed", -21);
    toggle(window.root, "dj-speech-ducking", FALSE); toggle(window.root, "dj-channel-1-cue", TRUE);
    toggle(window.root, "dj-fx-4-cue", TRUE); assert_effector_display(&window, TRUE);
    capture_dj(&window, "effector-lcd-active.png");
    g_assert_null(find(window.root, "dj-mixer-save"));
    JsonNode *record = document_wait("dj_mixer", "DJ mixer", persisted_controls);
    JsonObject *configuration = json_object_get_object_member(json_node_get_object(record), "config");
    g_assert_false(json_object_has_member(channel_config(configuration, 0), "pfl"));
    g_assert_false(json_object_has_member(json_array_get_object_element(json_object_get_array_member(configuration, "effects"), 3), "cue"));
    JsonNode *preferences = document_wait("audio_preferences", "Ses aygıtları", master_gain_saved);
    RadioSAudioMixerConfig live; g_assert_true(radios_audio_ui_get_mixer(window.ctx, &live, NULL, NULL));
    g_assert_true(live.channels[0].pfl); g_assert_true(live.effects[3].cue);
    /* DJ autosave must preserve a pending Technical device choice. */
    combo(window.root, "task-cue-output", monitor_sink); knob(window.root, "dj-channel-1-eq-mid", 4);
    JsonNode *second = document_wait("dj_mixer", "DJ mixer", pending_device_mixer_saved);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(window.root, "task-cue-output"))), ==, monitor_sink);
    JsonNode *preferences_after = document_read("audio_preferences", "Ses aygıtları"); g_assert_nonnull(preferences_after);
    g_assert_cmpint(radios_json_int(json_node_get_object(preferences_after), "_rev", 0), ==,
                    radios_json_int(json_node_get_object(preferences), "_rev", 0));
    g_assert_cmpstr(radios_json_string(json_object_get_object_member(json_node_get_object(preferences_after), "config"), "cue_sink", ""), ==, cue_sink);
    json_node_free(preferences_after); json_node_free(second);
    json_node_free(preferences); json_node_free(record); window_close(&window); window = window_open(); assert_gates_off(&window);
    radios_window_select_profile(window.window, "dj"); assert_effector_display(&window, FALSE);
    capture_dj(&window, "effector-lcd-restored.png");
    RadioSAudioMixerConfig mixer; RadioSAudioDspMeters meters; gdouble master_gain;
    g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, &meters, &master_gain));
    g_assert_cmpfloat_with_epsilon(mixer.channels[0].gain_db, -8.5, 0.001);
    g_assert_true(mixer.channels[0].loudness_bypass);
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(window.root, "dj-channel-1-normalize"))));
    g_assert_cmpfloat_with_epsilon(mixer.channels[0].eq_low_db, -12, 0.001);
    g_assert_cmpfloat_with_epsilon(mixer.channels[0].eq_mid_db, 4, 0.001);
    g_assert_cmpfloat_with_epsilon(mixer.channels[0].pan, -0.5, 0.001);
    g_assert_cmpfloat_with_epsilon(mixer.effects[3].depth, 0.75, 0.001);
    g_assert_cmpfloat_with_epsilon(mixer.effects[3].tempo_bpm, 142, 0.001);
    g_assert_cmpint(mixer.master.fx_slot, ==, 2);
    g_assert_cmpfloat_with_epsilon(master_gain, -4, 0.001);
    g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(adjustment(window.root, "header-main-gain")), -4, 0.001);
    for (guint i = 0; i < 32; ++i) g_assert_false(mixer.channels[i].pfl);
    for (guint i = 0; i < RADIOS_AUDIO_FX_SLOTS; ++i) g_assert_false(mixer.effects[i].cue);
    g_assert_false(mixer.master_fx.cue); reset_mixer(&window); window_close(&window);
}
static void test_cas_preserves_input(void)
{
    Window window = window_open(); reset_mixer(&window);
    JsonNode *record = document_wait("dj_mixer", "DJ mixer", initial_saved);
    JsonNode *configuration = deep_copy(json_object_get_member(json_node_get_object(record), "config"));
    json_object_set_double_member(channel_config(json_node_get_object(configuration), 0), "gain_db", -3);
    JsonNode *changed = document_remote_save(record, json_node_get_object(configuration)); json_node_free(configuration);
    range(window.root, "dj-channel-1-gain", -11);
    wait_until(dialog_exists, "database-reload-dialog", "stale automatic DJ CAS preserves local controls before explicit reload");
    RadioSAudioMixerConfig mixer; g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, NULL, NULL));
    g_assert_cmpfloat_with_epsilon(mixer.channels[0].gain_db, -11, 0.001);
    gtk_dialog_response(GTK_DIALOG(dialog_find("database-reload-dialog")), GTK_RESPONSE_CANCEL); run_for(100);
    JsonNode *readback = document_read("dj_mixer", "DJ mixer");
    g_assert_cmpint(radios_json_int(json_node_get_object(readback), "_rev", 0), ==, radios_json_int(json_node_get_object(changed), "_rev", 0));
    g_assert_cmpfloat_with_epsilon(json_object_get_double_member(channel_config(json_object_get_object_member(json_node_get_object(readback), "config"), 0), "gain_db"), -3, 0.001);
    g_assert_cmpfloat_with_epsilon(gtk_range_get_value(GTK_RANGE(find(window.root, "dj-channel-1-gain"))), -11, 0.001);
    click(window.root, "dj-mixer-reload"); wait_until(dialog_exists, "database-reload-dialog", "explicit reload confirms dirty mixer replacement");
    gtk_dialog_response(GTK_DIALOG(dialog_find("database-reload-dialog")), GTK_RESPONSE_ACCEPT);
    run_for(200); wait_until(window_ready, &window, "explicit DJ reload completes");
    g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, NULL, NULL)); g_assert_cmpfloat_with_epsilon(mixer.channels[0].gain_db, -3, 0.001);
    json_node_free(record); json_node_free(changed); json_node_free(readback); reset_mixer(&window); window_close(&window);
}
typedef struct { GtkWidget *root; const gchar *name, *text; } EntryExpectation;
static gboolean entry_matches(gpointer value)
{
    EntryExpectation *expected = value; GtkWidget *widget = find(expected->root, expected->name);
    return GTK_IS_ENTRY(widget) && !g_strcmp0(gtk_entry_get_text(GTK_ENTRY(widget)), expected->text);
}
static void entry_wait(Window *window, const gchar *name, const gchar *text)
{
    EntryExpectation expected = {window->root, name, text};
    wait_until(entry_matches, &expected, "channel name restores into its editable native entry");
}
static gboolean channel_names_saved(JsonObject *configuration)
{
    JsonNode *node = json_object_get_member(configuration, "names");
    if (!node || !JSON_NODE_HOLDS_OBJECT(node)) return FALSE;
    JsonObject *names = json_node_get_object(node);
    return !g_strcmp0(radios_json_string(names, "0", ""), "Ana yayın") &&
        !g_strcmp0(radios_json_string(names, "1", ""), "Müzik 🎵") &&
        !g_strcmp0(radios_json_string(names, "2", ""), "Konuk");
}
static gboolean channel_name_cleared(JsonObject *configuration)
{
    JsonNode *node = json_object_get_member(configuration, "names");
    if (!node || !JSON_NODE_HOLDS_OBJECT(node)) return FALSE;
    JsonObject *names = json_node_get_object(node);
    return json_object_has_member(names, "1") && !g_strcmp0(radios_json_string(names, "1", NULL), "");
}
static gboolean sparse_channel_name_saved(JsonObject *configuration)
{
    JsonObject *names = json_object_get_object_member(configuration, "names");
    JsonArray *slots = json_object_get_array_member(configuration, "slots");
    if (!names || !slots || json_object_has_member(names, "1") ||
        g_strcmp0(radios_json_string(names, "2", ""), "İkinci kanal")) return FALSE;
    gboolean second_present = FALSE;
    for (guint i = 0; i < json_array_get_length(slots); ++i) {
        gint64 slot = json_array_get_int_element(slots, i);
        if (slot == 0) return FALSE;
        if (slot == 1) second_present = TRUE;
    }
    return second_present;
}
static void test_channel_names_autosave(void)
{
    Window window = window_open(); radios_window_select_profile(window.window, "dj"); run_for(100);
    g_assert_null(find(window.root, "dj-mixer-save"));
    for (guint number = 0; number <= 2; ++number) {
        gchar *name = g_strdup_printf("dj-channel-%u-name", number);
        GtkWidget *widget = find(window.root, name); g_assert_true(GTK_IS_ENTRY(widget)); g_free(name);
        g_assert_cmpint(gtk_entry_get_max_length(GTK_ENTRY(widget)), ==, 64);
        name = g_strdup_printf("dj-channel-%u", number);
        GtkWidget *channel = find(window.root, name); g_assert_true(GTK_IS_CONTAINER(channel)); g_free(name);
        GList *children = gtk_container_get_children(GTK_CONTAINER(channel)); GList *last = g_list_last(children);
        g_assert_nonnull(last);
        g_assert_true(widget == last->data || gtk_widget_is_ancestor(widget, last->data));
        g_list_free(children);
    }
    /* Later text survives callbacks from the earlier in-flight name save. */
    entry(window.root, "dj-channel-1-name", "M");
    entry(window.root, "dj-channel-1-name", "Müz");
    entry(window.root, "dj-channel-1-name", "Müzik 🎵");
    entry(window.root, "dj-channel-0-name", "Ana yayın");
    entry(window.root, "dj-channel-2-name", "Konuk");
    JsonNode *record = document_wait("channel", "DJ kanalları", channel_names_saved);
    document_ack_wait(&window, record);
    entry_wait(&window, "dj-channel-1-name", "Müzik 🎵");

    /* A genuine clean DB refresh restores remote names into the same entries. */
    JsonNode *configuration = deep_copy(json_object_get_member(json_node_get_object(record), "config"));
    JsonObject *names = json_object_get_object_member(json_node_get_object(configuration), "names");
    json_object_set_string_member(names, "2", "Konuk uzaktan");
    JsonNode *changed = document_remote_save(record, json_node_get_object(configuration));
    json_node_free(configuration); json_node_free(changed); json_node_free(record);
    radios_data_refresh(window.ctx); entry_wait(&window, "dj-channel-2-name", "Konuk uzaktan");
    entry(window.root, "dj-channel-2-name", "Konuk");
    record = document_wait("channel", "DJ kanalları", channel_names_saved);
    document_ack_wait(&window, record); json_node_free(record);
    capture_dj(&window, "channel-names-autosaved.png");
    window_close(&window); window = window_open(); radios_window_select_profile(window.window, "dj");
    entry_wait(&window, "dj-channel-0-name", "Ana yayın");
    entry_wait(&window, "dj-channel-1-name", "Müzik 🎵");
    entry_wait(&window, "dj-channel-2-name", "Konuk");
    entry(window.root, "dj-channel-1-name", "");
    record = document_wait("channel", "DJ kanalları", channel_name_cleared);
    document_ack_wait(&window, record);
    window_close(&window); window = window_open(); radios_window_select_profile(window.window, "dj");
    entry_wait(&window, "dj-channel-1-name", ""); entry_wait(&window, "dj-channel-2-name", "Konuk");

    /* Clearing the remote map restores defaults, including MASTER. */
    json_node_free(record); record = document_read("channel", "DJ kanalları");
    g_assert_nonnull(record); document_ack_wait(&window, record);
    configuration = deep_copy(json_object_get_member(json_node_get_object(record), "config"));
    json_object_set_object_member(json_node_get_object(configuration), "names", json_object_new());
    changed = document_remote_save(record, json_node_get_object(configuration));
    json_node_free(configuration); json_node_free(changed); json_node_free(record);
    radios_data_refresh(window.ctx);
    entry_wait(&window, "dj-channel-0-name", "MASTER");
    entry_wait(&window, "dj-channel-1-name", "Player 1");
    entry_wait(&window, "dj-channel-2-name", "Player 2");

    /* Channel two keeps its own identity and name after channel one is removed. */
    entry(window.root, "dj-channel-2-name", "İkinci kanal"); click(window.root, "dj-channel-1-remove");
    record = document_wait("channel", "DJ kanalları", sparse_channel_name_saved);
    document_ack_wait(&window, record); json_node_free(record);
    window_close(&window); window = window_open(); radios_window_select_profile(window.window, "dj");
    g_assert_null(find(window.root, "dj-channel-1-name"));
    entry_wait(&window, "dj-channel-2-name", "İkinci kanal");
    window_close(&window);
}
static gboolean immediate_close_mixer_saved(JsonObject *configuration)
{
    JsonObject *channel = channel_config(configuration, 0);
    return fabs(json_object_get_double_member(channel, "gain_db") + 14.5) < 0.001 &&
        fabs(json_object_get_double_member(channel, "eq_mid_db") - 4.5) < 0.001 &&
        fabs(json_object_get_double_member(configuration, "crossfader") - .5) < 0.001;
}
static gboolean immediate_close_master_saved(JsonObject *configuration)
{ return fabs(json_object_get_double_member(configuration, "main_gain_db") + 6.5) < 0.001; }
static gboolean immediate_close_name_saved(JsonObject *configuration)
{
    JsonNode *node = json_object_get_member(configuration, "names");
    return node && JSON_NODE_HOLDS_OBJECT(node) &&
        !g_strcmp0(radios_json_string(json_node_get_object(node), "1", ""), "Son kanal adı");
}
static gboolean immediate_close_mixer_restored(gpointer value)
{
    Window *window = value; RadioSAudioMixerConfig mixer; gdouble master_gain;
    return radios_audio_ui_get_mixer(window->ctx, &mixer, NULL, &master_gain) &&
        fabs(mixer.channels[0].gain_db + 14.5) < .001 &&
        fabs(mixer.channels[0].eq_mid_db - 4.5) < .001 &&
        fabs(mixer.crossfader - .5) < .001 && fabs(master_gain + 6.5) < .001;
}
static void test_autosave_latest_immediate_close(void)
{
    Window window = window_open(); reset_mixer(&window);
    radios_window_select_profile(window.window, "dj");
    /* None of these changes gets a debounce/main-loop tick before close. */
    for (guint i = 0; i < 30; ++i) {
        range(window.root, "dj-channel-1-gain", -(gdouble)i / 2);
        knob(window.root, "dj-channel-1-eq-mid", (gdouble)i / 10);
        range(window.root, "dj-crossfader", (gdouble)i / 60);
    }
    range(window.root, "dj-channel-1-gain", -14.5);
    knob(window.root, "dj-channel-1-eq-mid", 4.5); range(window.root, "dj-crossfader", .5);
    range(window.root, "dj-channel-0-gain", -6.5);
    entry(window.root, "dj-channel-1-name", "İlk kanal adı");
    entry(window.root, "dj-channel-1-name", "Son kanal adı");
    window_close(&window); window = window_open();
    JsonNode *record = document_wait("dj_mixer", "DJ mixer", immediate_close_mixer_saved); json_node_free(record);
    record = document_wait("audio_preferences", "Ses aygıtları", immediate_close_master_saved); json_node_free(record);
    record = document_wait("channel", "DJ kanalları", immediate_close_name_saved); json_node_free(record);
    wait_until(immediate_close_mixer_restored, &window, "latest automatic mixer and MASTER changes recover after immediate close");
    entry_wait(&window, "dj-channel-1-name", "Son kanal adı"); assert_gates_off(&window);
    reset_mixer(&window); window_close(&window);
}
static gboolean cold_load_edit_saved(JsonObject *configuration)
{
    return fabs(json_object_get_double_member(channel_config(configuration, 0), "gain_db") + 10) < .001 &&
        fabs(json_object_get_double_member(channel_config(configuration, 1), "eq_high_db") + 9) < .001;
}
static void test_cold_load_preserves_saved_mixer(void)
{
    Window window = window_open(); reset_mixer(&window);
    JsonNode *record = document_wait("dj_mixer", "DJ mixer", initial_saved); document_ack_wait(&window, record);
    JsonNode *configuration = deep_copy(json_object_get_member(json_node_get_object(record), "config"));
    json_object_set_double_member(channel_config(json_node_get_object(configuration), 1), "eq_high_db", -9);
    window_close(&window);
    JsonNode *changed = document_remote_save(record, json_node_get_object(configuration));
    json_node_free(changed); json_node_free(configuration); json_node_free(record);

    /* No main-loop iteration runs before the first saved mixer read. */
    window = window_create(); g_assert_false(radios_audio_ui_mixer_ready(window.ctx));
    RadioSAudioMixerConfig edit; radios_audio_mixer_config_default(&edit); edit.channels[0].gain_db = -10;
    GError *error = NULL;
    g_assert_false(radios_audio_ui_set_mixer(window.ctx, &edit, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PENDING); g_clear_error(&error);
    RadioSAudioMixerConfig live; g_assert_true(radios_audio_ui_get_mixer(window.ctx, &live, NULL, NULL));
    g_assert_cmpfloat_with_epsilon(live.channels[0].gain_db, 0, .001);
    wait_until(window_ready, &window, "cold window loads the authenticated saved mixer baseline");
    g_assert_true(radios_audio_ui_mixer_ready(window.ctx));
    g_assert_true(radios_audio_ui_get_mixer(window.ctx, &live, NULL, NULL));
    g_assert_cmpfloat_with_epsilon(live.channels[0].gain_db, 0, .001);
    g_assert_cmpfloat_with_epsilon(live.channels[1].eq_high_db, -9, .001);
    live.channels[0].gain_db = -10;
    require_call(radios_audio_ui_set_mixer(window.ctx, &live, &error), &error);
    record = document_wait("dj_mixer", "DJ mixer", cold_load_edit_saved);
    document_ack_wait(&window, record); json_node_free(record);
    window_close(&window);
}
static gboolean library_load_ready(gpointer unused)
{
    (void)unused; GtkWidget *dialog = dialog_find("media-library-dialog"); GtkWidget *load = dialog ? find(dialog, "media-pick-accept") : NULL;
    return load && gtk_widget_is_sensitive(load);
}
static void native_load(Window *window, JsonNode *detail, guint deck)
{
    JsonObject *media = json_object_get_object_member(json_node_get_object(detail), "media");
    radios_media_library_open_for_deck(window->ctx, deck);
    radios_media_library_open_selected(window->ctx, radios_json_string(media, "_id", "")); wait_until(library_load_ready, NULL, "verified library detail enables load");
    GtkWidget *dialog = dialog_find("media-library-dialog"); click(dialog, "media-pick-accept");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); deck_wait(window, deck, RADIOS_AUDIO_UI_READY, 0);
}
static void crossfade_readback(Window *window, const gchar *phase, gdouble expected)
{
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window->ctx); g_assert_nonnull(engine);
    RadioSAudioSnapshot snap; radios_audio_engine_snapshot(engine, &snap); radios_audio_engine_unref(engine);
    const RadioSAudioChannelDsp *strip = &snap.mixer.channels[0];
    g_test_message("XFADE %s group=%d position=%.3f source=%d gain=%.3f pan=%.3f eq=%.3f/%.3f/%.3f "
        "fx=%d FX4=%d/type%d masterFX=%d mainReady=%d buffer=%u consumed=%" G_GUINT64_FORMAT
        " cursor=%" G_GUINT64_FORMAT " sourceBuffered=%" G_GUINT64_FORMAT
        " DSPmain=%.6f/%.6f captureOffset=%" G_GOFFSET_FORMAT,
        phase, strip->crossfade_group, snap.mixer.crossfader, strip->source_deck, strip->gain_db, strip->pan,
        strip->eq_low_db, strip->eq_mid_db, strip->eq_high_db, strip->fx_slot,
        snap.mixer.effects[3].enabled, snap.mixer.effects[3].type, snap.mixer.master_fx.enabled,
        snap.buses[RADIOS_AUDIO_BUS_MAIN].ready, snap.buses[RADIOS_AUDIO_BUS_MAIN].output_buffer_frames,
        snap.buses[RADIOS_AUDIO_BUS_MAIN].consumed_frames, snap.decks[0].cursor_frames,
        snap.decks[0].buffered_frames, snap.dsp_meters.master_peak[0], snap.dsp_meters.master_peak[1],
        capture_offset("MAIN"));
    /* The capture assertion must test the configuration the native controls
     * actually applied, including after asynchronous UI/document polling. */
    g_assert_cmpint(strip->crossfade_group, ==, 1);
    g_assert_cmpfloat_with_epsilon(snap.mixer.crossfader, expected, 0.000001);
}
static gboolean channel_one_levelling_off(gpointer value)
{
    Window *window = value; RadioSAudioMixerConfig mixer;
    return radios_audio_ui_get_mixer(window->ctx, &mixer, NULL, NULL) && mixer.channels[0].loudness_bypass;
}
static void test_real_pcm_controls(void)
{
    Window window = window_open(); reset_mixer(&window); JsonNode *detail = media_import(&window, "DJ MAIN kaynağı", 997, 0.4);
    /* OTO SES (dj.c, the normalize button's tooltip) is the designed takeover
     * of a measured channel's fader: while it is on, the fader follows the LUFS
     * target and a manual move is taken back, so -12 dB never reaches MAIN.
     * This case measures the fader, pan, mute, PFL and crossfade themselves,
     * so automatic levelling is switched off on channel 1 first. */
    toggle(window.root, "dj-channel-1-normalize", FALSE);
    wait_until(channel_one_levelling_off, &window, "channel 1 automatic levelling is off before its fader is measured");
    native_load(&window, detail, 0); click(window.root, "dj-channel-1-play"); deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
    run_for(150); Capture full = measure("MAIN"); g_assert_cmpfloat(full.rms[0], >, 0.05); g_assert_cmpfloat(full.rms[1], >, 0.05);
    knob(window.root, "dj-channel-1-pan", -100); run_for(150); Capture left = measure("MAIN");
    g_assert_cmpfloat(left.rms[0], >, 0.05); g_assert_cmpfloat(left.peak[1], <, 0.0001);
    range(window.root, "dj-channel-1-gain", -12); run_for(150); Capture reduced = measure("MAIN");
    g_assert_cmpfloat(reduced.rms[0] / left.rms[0], >, 0.20); g_assert_cmpfloat(reduced.rms[0] / left.rms[0], <, 0.31);
    range(window.root, "dj-channel-1-gain", 0); knob(window.root, "dj-channel-1-pan", 0);
    toggle(window.root, "dj-channel-1-mute", TRUE); toggle(window.root, "dj-channel-1-cue", TRUE);
    wait_until(window_ready, &window, "native PFL connects selected virtual CUE"); run_for(150);
    Capture cue = measure("CUE"), main = measure("MAIN");
    g_assert_cmpfloat(cue.rms[0], >, 0.05); g_assert_cmpfloat(main.peak[0], <, 0.0001); g_assert_cmpfloat(main.peak[1], <, 0.0001);
    RadioSAudioUiSnapshot source; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 0, &source));
    guint64 before = source.position_ms; run_for(250); g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 0, &source));
    g_assert_cmpuint(source.position_ms - before, <, 500); /* Multiple buses must consume one source clock. */
    JsonNode *preview = media_import(&window, "DJ ayrı ön dinleme", 1600, 0.1); GError *error = NULL;
    GtkWidget *owner = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_transient_for(GTK_WINDOW(owner), window.ctx->parent);
    require_call(radios_audio_ui_preview(window.ctx, owner, json_node_get_object(preview), 0, 0, &error), &error);
    deck_wait(&window, 32, RADIOS_AUDIO_UI_PLAYING, 100); run_for(200); cue = measure("CUE");
    /* The audition plays at the level loudness normalization gives deck 32
     * (0 dB when unmeasured or out of scope): 0.1 × the applied gain. */
    RadioSAudioUiSnapshot audition; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 32, &audition));
    gdouble audition_peak = 0.1 * pow(10, audition.loudness_gain_db / 20);
    g_test_message("Preview loudness %s %.2f dB, CUE peak %.4f, expected %.4f", audition.loudness_measured ? "measured" : "unmeasured",
        audition.loudness_gain_db, cue.peak[0], audition_peak);
    g_assert_cmpfloat(cue.peak[0], >, audition_peak * 0.8); g_assert_cmpfloat(cue.peak[0], <, audition_peak * 1.2);
    gtk_widget_destroy(owner); deck_wait(&window, 32, RADIOS_AUDIO_UI_READY, 0);
    toggle(window.root, "dj-channel-1-cue", FALSE); toggle(window.root, "dj-channel-1-mute", FALSE);
    combo(window.root, "dj-channel-1-cross-group", "1"); range(window.root, "dj-crossfader", 1);
    crossfade_readback(&window, "fade-input-ack", 1);
    wait_until(window_ready, &window, "previous preview/CUE flush finishes before crossfade PCM capture");
    run_for(200); crossfade_readback(&window, "fade-capture-start", 1);
    Capture faded = measure("MAIN"); crossfade_readback(&window, "fade-capture-end", 1);
    g_test_message("XFADE faded capture RMS=%.6f/%.6f peak=%.6f/%.6f", faded.rms[0], faded.rms[1], faded.peak[0], faded.peak[1]);
    g_assert_cmpfloat(faded.peak[0], <, 0.0001); g_assert_cmpfloat(faded.peak[1], <, 0.0001);
    range(window.root, "dj-crossfader", -1); crossfade_readback(&window, "restore-input-ack", -1);
    run_for(150); Capture restored = measure("MAIN"); crossfade_readback(&window, "restore-capture-end", -1);
    g_assert_cmpfloat(restored.rms[0], >, 0.05);
    click(window.root, "dj-channel-1-stop"); deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    json_node_free(detail); json_node_free(preview); reset_mixer(&window); window_close(&window);
}
typedef struct { const gchar *id; } Pick;
static gboolean picker_select(gpointer data)
{
    Pick *pick = data; GtkWidget *dialog = dialog_find("media-library-dialog"); GtkWidget *tree = dialog ? find(dialog, "media-table") : NULL;
    if (!GTK_IS_TREE_VIEW(tree)) return FALSE;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree)); GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_first(model, &iter)) return FALSE;
    do {
        gchar *id = NULL; gtk_tree_model_get(model, &iter, 4, &id, -1); gboolean match = !g_strcmp0(id, pick->id); g_free(id);
        if (match) { gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), &iter);
            GtkWidget *button = find(dialog, "media-pick-accept"); return button && gtk_widget_is_sensitive(button); }
    } while (gtk_tree_model_iter_next(model, &iter)); return FALSE;
}
static void select_media(JsonNode *detail)
{
    JsonObject *media = json_object_get_object_member(json_node_get_object(detail), "media"); Pick pick = {radios_json_string(media, "_id", "")};
    wait_until(picker_select, &pick, "fresh verified media becomes selectable in native picker");
    click(dialog_find("media-library-dialog"), "media-pick-accept");
    /* The shared library remains available after returning a selection. */
    GtkWidget *dialog = dialog_find("media-library-dialog");
    if (dialog) gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    wait_until(dialog_closed, "media-library-dialog", "selected library closes before editing the destination");
}
static gboolean sampler_bound(JsonObject *configuration)
{
    JsonArray *pads = json_object_get_array_member(configuration, "pads");
    for (guint i = 0; i < json_array_get_length(pads); ++i) {
        JsonObject *pad = json_array_get_object_element(pads, i);
        if (!strcmp(radios_json_string(pad, "key", ""), "channel-1-pad-1")) return json_object_has_member(pad, "media_sha256");
    }
    return FALSE;
}
static GPtrArray *dj_documents(GtkWidget *root)
{
    if (g_object_get_data(G_OBJECT(root), "dj-state"))
        return g_object_get_data(G_OBJECT(root), "radios-data-documents");
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GPtrArray *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = dj_documents(item->data);
    g_list_free(children); return result;
}
static gboolean dj_removal_acknowledged(gpointer value)
{
    Window *window = value; GtkWidget *scope = find(window->root, "dj-task-view");
    GPtrArray *documents = scope ? dj_documents(scope) : NULL;
    /* Only this page's channel + sampler documents must have accepted saves;
     * unrelated default documents can legitimately have revision zero. */
    if (!documents || documents->len != 2) return FALSE;
    for (guint i = 0; i < documents->len; ++i) {
        RadioSDocument *document = g_ptr_array_index(documents, i);
        if (!document || radios_data_document_revision(document) <= 0) return FALSE;
    }
    return TRUE;
}
static gboolean channel_one_removed(JsonObject *configuration)
{
    JsonNode *node = json_object_get_member(configuration, "slots");
    if (!node || !JSON_NODE_HOLDS_ARRAY(node)) return FALSE;
    JsonArray *slots = json_node_get_array(node);
    if (!json_array_get_length(slots)) return FALSE;
    for (guint i = 0; i < json_array_get_length(slots); ++i)
        if (json_array_get_int_element(slots, i) == 0) return FALSE;
    return TRUE;
}
static gboolean sampler_channel_one_removed(JsonObject *configuration)
{
    JsonNode *node = json_object_get_member(configuration, "pads");
    if (!node || !JSON_NODE_HOLDS_ARRAY(node)) return FALSE;
    JsonArray *pads = json_node_get_array(node);
    for (guint i = 0; i < json_array_get_length(pads); ++i) {
        JsonObject *pad = json_array_get_object_element(pads, i);
        if (g_str_has_prefix(radios_json_string(pad, "key", ""), "channel-1-pad-")) return FALSE;
    }
    return TRUE;
}
static void test_verified_sampler(void)
{
    Window window = window_open(); reset_mixer(&window); JsonNode *detail = media_import(&window, "DJ doğrulanmış pad", 730, 0.3);
    click(window.root, "dj-sampler-channel-1-pad-1-edit"); GtkWidget *dialog = dialog_find("dj-sampler-dialog"); g_assert_nonnull(dialog);
    click(dialog, "dj-sampler-item"); select_media(detail); dialog = dialog_find("dj-sampler-dialog"); g_assert_nonnull(dialog);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); wait_until(dialog_closed, "dj-sampler-dialog", "verified sampler assignment finishes");
    JsonNode *record = document_wait("sampler", "DJ sampler", sampler_bound); json_node_free(record);
    toggle(window.root, "dj-channel-1-mute", TRUE); toggle(window.root, "dj-channel-1-cue", TRUE);
    click(window.root, "dj-sampler-channel-1-pad-1"); RadioSAudioUiSnapshot first = deck_wait(&window, 33, RADIOS_AUDIO_UI_PLAYING, 100);
    JsonObject *media = json_object_get_object_member(json_node_get_object(detail), "media");
    g_assert_cmpstr(first.media_id, ==, radios_json_string(media, "_id", "")); run_for(150);
    Capture cue = measure("CUE"), main = measure("MAIN"); g_assert_cmpfloat(cue.rms[0], >, 0.04); g_assert_cmpfloat(main.peak[0], <, 0.0001);
    click(window.root, "dj-sampler-channel-1-pad-1"); deck_wait(&window, 33, RADIOS_AUDIO_UI_READY, 0);
    click(window.root, "dj-sampler-channel-1-pad-1"); RadioSAudioUiSnapshot second = deck_wait(&window, 33, RADIOS_AUDIO_UI_PLAYING, 100);
    g_assert_cmpuint(second.generation, >, first.generation); g_assert_cmpuint(second.position_ms, <, 2000);
    click(window.root, "dj-channel-1-remove"); run_for(300); g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 33, &second));
    g_assert_cmpint(second.state, !=, RADIOS_AUDIO_UI_PLAYING); g_assert_null(find(window.root, "dj-channel-1"));
    /* Reopen verifies durable removal, after both independent documents have
     * actually accepted it; a fixed delay cannot acknowledge an async save. */
    JsonNode *channels_saved = document_wait("channel", "DJ kanalları", channel_one_removed);
    JsonNode *sampler_saved = document_wait("sampler", "DJ sampler", sampler_channel_one_removed);
    json_node_free(channels_saved); json_node_free(sampler_saved);
    wait_until(dj_removal_acknowledged, &window, "channel and sampler saves settle with accepted revisions before reopen");
    window_close(&window); window = window_open(); g_assert_null(find(window.root, "dj-channel-1")); g_assert_nonnull(find(window.root, "dj-channel-2"));
    assert_gates_off(&window); json_node_free(detail); reset_mixer(&window); window_close(&window);
}
static gboolean transition_bound(JsonObject *configuration)
{ return json_object_has_member(configuration, "media_a") && json_object_has_member(configuration, "media_b"); }
static gboolean transition_ready(gpointer unused)
{
    (void)unused; GtkWidget *dialog = dialog_find("transition-editor");
    GtkWidget *play = dialog ? find(dialog, "transition-preview-play") : NULL; return play && gtk_widget_is_sensitive(play);
}
typedef struct { RadioSViewContext *ctx; guint64 maximum_position; gboolean eof, error; } Completion;
static gboolean completion_sample(gpointer value)
{
    Completion *completion = value; RadioSAudioUiSnapshot snapshot;
    g_assert_true(radios_audio_ui_get_snapshot(completion->ctx, 32, &snapshot));
    completion->maximum_position = MAX(completion->maximum_position, snapshot.position_ms);
    completion->eof |= snapshot.state == RADIOS_AUDIO_UI_EOF;
    completion->error |= snapshot.state == RADIOS_AUDIO_UI_ERROR || snapshot.error[0] != '\0';
    return G_SOURCE_CONTINUE;
}
static gboolean transition_finished(gpointer value)
{
    Completion *completion = value; RadioSAudioUiSnapshot snapshot;
    g_assert_true(radios_audio_ui_get_snapshot(completion->ctx, 32, &snapshot));
    return completion->eof || (completion->maximum_position >= 1000 && snapshot.state == RADIOS_AUDIO_UI_EMPTY);
}
static void test_transition_pair(void)
{
    Window window = window_open(); reset_mixer(&window); GError *error = NULL;
    JsonNode *detail_a = media_import(&window, "Geçiş gerçek A", 600, 0.3);
    JsonNode *detail_b = media_import(&window, "Geçiş gerçek B", 1800, 0.2);
    radios_transition_editor_open(window.ctx); GtkWidget *dialog = dialog_find("transition-editor"); g_assert_nonnull(dialog);
    wait_until(window_ready, &window, "transition configuration loads before editing");
    click(dialog, "transition-pick-a"); select_media(detail_a); dialog = dialog_find("transition-editor");
    click(dialog, "transition-pick-b"); select_media(detail_b); dialog = dialog_find("transition-editor");
    const gchar *fields[] = {"transition-cue-in", "transition-intro", "transition-out", "transition-b-in",
                            "transition-fade-in", "transition-overlap", "transition-ducking"};
    const gchar *values[] = {"19", "", "20.001", "19", "0.5", "0.5", "-6"};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) {
        GtkWidget *entry = find(dialog, fields[i]); g_assert_true(GTK_IS_ENTRY(entry)); gtk_entry_set_text(GTK_ENTRY(entry), values[i]);
    }
    range(dialog, "transition-preview-level", -9); wait_until(transition_ready, NULL, "both fresh verified transition sources are ready");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    JsonNode *record = document_wait("transition", "Geçiş ayarları", transition_bound);
    JsonObject *configuration = json_object_get_object_member(json_node_get_object(record), "config");
    JsonObject *binding_a = json_object_get_object_member(configuration, "media_a");
    JsonObject *binding_b = json_object_get_object_member(configuration, "media_b");
    g_assert_cmpstr(radios_json_string(binding_a, "media_id", ""), ==,
                    radios_json_string(json_object_get_object_member(json_node_get_object(detail_a), "media"), "_id", ""));
    g_assert_cmpstr(radios_json_string(binding_b, "media_id", ""), ==,
                    radios_json_string(json_object_get_object_member(json_node_get_object(detail_b), "media"), "_id", ""));
    json_node_free(record); gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    window_close(&window); window = window_open();
    radios_transition_editor_open(window.ctx); wait_until(transition_ready, NULL, "reopened A/B bindings are fetched and verified");
    dialog = dialog_find("transition-editor");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(dialog, "transition-out"))), ==, "20.001");
    Completion completion = {window.ctx, 0, FALSE, FALSE};
    guint observer = g_timeout_add_full(G_PRIORITY_HIGH, 1, completion_sample, &completion, NULL);
    goffset complete_capture = capture_offset("CUE");
    click(dialog, "transition-preview-play"); deck_wait(&window, 32, RADIOS_AUDIO_UI_PLAYING, 100); run_for(150);
    Capture cue = measure("CUE"), main = measure("MAIN");
    g_assert_cmpfloat(cue.rms[0], >, 0.01); g_assert_cmpfloat(cue.peak[0], <, 0.25);
    g_assert_cmpfloat(main.peak[0], <, 0.0001); g_assert_cmpfloat(main.peak[1], <, 0.0001);
    wait_until(transition_finished, &completion, "fractional-frame A/B preview reaches real EOF");
    g_source_remove(observer); g_assert_false(completion.error); g_assert_cmpuint(completion.maximum_position, >=, 1400);
    deck_wait(&window, 32, RADIOS_AUDIO_UI_EMPTY, 0); run_for(200);
    guint64 signal_frames = capture_signal_span("CUE", complete_capture);
    g_assert_cmpuint(signal_frames, >=, 70000); g_assert_cmpuint(signal_frames, <=, 76000);
    g_test_message("Actual A/B CUE span: %" G_GUINT64_FORMAT " frames; final cursor %" G_GUINT64_FORMAT " ms; EOF sampled=%d",
                   signal_frames, completion.maximum_position, completion.eof);
    wait_until(window_ready, &window, "completed short audition flushes before replay");
    click(dialog, "transition-preview-play"); deck_wait(&window, 32, RADIOS_AUDIO_UI_PLAYING, 100);
    click(dialog, "transition-preview-stop"); deck_wait(&window, 32, RADIOS_AUDIO_UI_EMPTY, 0);
    wait_until(window_ready, &window, "transition stop flushes CUE before new intent");
    /* Closing a just-started preparation invalidates its response. A newer
     * verified owner must keep the CUE stream even if the old worker finishes. */
    click(dialog, "transition-preview-play"); gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    GtkWidget *newer = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_transient_for(GTK_WINDOW(newer), window.ctx->parent);
    require_call(radios_audio_ui_preview(window.ctx, newer, json_node_get_object(detail_b), 0, 0, &error), &error);
    RadioSAudioUiSnapshot snapshot = deck_wait(&window, 32, RADIOS_AUDIO_UI_PLAYING, 100); guint64 generation = snapshot.generation;
    run_for(500); g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 32, &snapshot));
    g_assert_cmpint(snapshot.state, ==, RADIOS_AUDIO_UI_PLAYING); g_assert_cmpuint(snapshot.generation, ==, generation);
    g_assert_cmpstr(snapshot.media_id, ==, radios_json_string(json_object_get_object_member(json_node_get_object(detail_b), "media"), "_id", ""));
    g_assert_true(radios_audio_ui_preview_owned(window.ctx, newer));
    gtk_widget_destroy(newer); deck_wait(&window, 32, RADIOS_AUDIO_UI_READY, 0);
    json_node_free(detail_a); json_node_free(detail_b); window_close(&window);
}
static gboolean channel_routes_saved(JsonObject *configuration)
{
    JsonObject *source = channel_config(configuration, 1), *target = channel_config(configuration, 7);
    return json_object_get_int_member(source, "send_channel") == 7 &&
        json_object_get_int_member(source, "return_channel") == -1 &&
        json_object_get_int_member(source, "return_effect") == 0 &&
        json_object_get_int_member(target, "return_channel") == -1 &&
        json_object_get_int_member(target, "return_effect") == 0 &&
        fabs(json_object_get_double_member(source, "send_db")) < 0.001 &&
        fabs(json_object_get_double_member(target, "return_db") + 6.0206) < 0.001;
}
static void capture_dj(Window *window, const gchar *name)
{
    const gchar *directory = g_getenv("RADIOS_SHOT_DIR"); if (!directory) return;
    g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
    GtkWidget *mixer = find(window->root, "dj-mixer"); g_assert_nonnull(mixer);
    gtk_container_check_resize(GTK_CONTAINER(window->root));
    GtkAllocation allocation; gtk_widget_get_allocation(mixer, &allocation);
    gtk_widget_size_allocate(mixer, &allocation);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
    cairo_t *context = cairo_create(surface); gtk_widget_draw(mixer, context);
    g_assert_cmpint(cairo_status(context), ==, CAIRO_STATUS_SUCCESS); cairo_destroy(context);
    gchar *path = g_build_filename(directory, name, NULL);
    g_assert_cmpint(cairo_surface_write_to_png(surface, path), ==, CAIRO_STATUS_SUCCESS);
    g_free(path); cairo_surface_destroy(surface);
}
static gboolean master_routes_saved(JsonObject *configuration)
{
    JsonObject *master = json_object_get_object_member(configuration, "master");
    return json_object_get_int_member(master, "send_channel") == 7 &&
        json_object_get_int_member(master, "return_channel") == -1 &&
        json_object_get_int_member(master, "return_effect") == 0 &&
        fabs(json_object_get_double_member(master, "send_db")) < 0.001 &&
        fabs(json_object_get_double_member(master, "return_db") + 6.0206) < 0.001;
}
static void test_master_channel_routing(void)
{
    Window window = window_open(); assert_gates_off(&window); reset_mixer(&window);
    radios_window_select_profile(window.window, "dj"); run_for(250);
    GtkWidget *send = find(window.root, "dj-channel-0-send-target");
    for (guint attempt = 0; !find(window.root, "dj-channel-8") && attempt < 32; ++attempt) {
        click(window.root, "dj-add-channel"); run_for(100);
    }
    g_assert_nonnull(find(window.root, "dj-channel-8"));
    GtkWidget *receive = find(window.root, "dj-channel-0-return-source");
    g_assert_true(GTK_IS_COMBO_BOX(send)); g_assert_true(GTK_IS_COMBO_BOX(receive));
    g_assert_true(gtk_widget_get_visible(send)); g_assert_true(gtk_widget_get_visible(receive));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(send)), ==, "-1");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(receive)), ==, "-1");
    JsonNode *detail = media_import(&window, "DJ MASTER kanal bağlantısı", 1000, 0.2);
    native_load(&window, detail, 1); click(window.root, "dj-channel-2-play");
    deck_wait(&window, 1, RADIOS_AUDIO_UI_PLAYING, 100); run_for(200); Capture dry = measure("MAIN");
    g_assert_cmpfloat(dry.rms[0], >, 0.04);
    combo(window.root, "dj-channel-8-source", "-1"); range(window.root, "dj-channel-8-gain", -6.0206);
    knob(window.root, "dj-channel-8-route-send", 0); knob(window.root, "dj-channel-0-route-send", 0);
    combo(window.root, "dj-channel-0-send-target", "7"); run_for(250);
    Capture sent = measure("MAIN");
    g_assert_cmpfloat(sent.rms[0] / dry.rms[0], >, 1.45); g_assert_cmpfloat(sent.rms[0] / dry.rms[0], <, 1.55);
    /* The effect receives channel 2, independently of MASTER's channel SEND.
     * At 1000 Hz, a 300 BPM full-depth Delay is exactly 200 periods. */
    combo(window.root, "dj-channel-0-send-target", "-1");
    RadioSAudioMixerConfig mixer; GError *error = NULL;
    g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, NULL, NULL));
    mixer.effects[0].type = RADIOS_FX_DELAY; mixer.effects[0].enabled = TRUE;
    mixer.effects[0].depth = 1; mixer.effects[0].tempo_bpm = 300; mixer.effects[0].return_db = 0;
    require_call(radios_audio_ui_set_mixer(window.ctx, &mixer, &error), &error);
    combo(window.root, "dj-channel-2-effector", "0");
    combo(window.root, "dj-channel-0-return-source", "0"); run_for(350);
    Capture received = measure("MAIN");
    g_assert_cmpfloat(received.rms[0] / dry.rms[0], >, 1.95); g_assert_cmpfloat(received.rms[0] / dry.rms[0], <, 2.05);
    knob(window.root, "dj-channel-0-route-return", -6.0206); run_for(250);
    Capture reduced = measure("MAIN");
    g_assert_cmpfloat(reduced.rms[0] / dry.rms[0], >, 1.45); g_assert_cmpfloat(reduced.rms[0] / dry.rms[0], <, 1.55);
    toggle(window.root, "dj-channel-0-mute", TRUE); run_for(250); Capture muted = measure("MAIN");
    g_assert_cmpfloat(muted.peak[0], <, 0.0001); g_assert_cmpfloat(muted.peak[1], <, 0.0001);
    toggle(window.root, "dj-channel-0-mute", FALSE); click(window.root, "dj-channel-2-stop");
    deck_wait(&window, 1, RADIOS_AUDIO_UI_READY, 0);
    run_for(250);
    combo(window.root, "dj-channel-0-send-target", "7"); run_for(100);
    capture_dj(&window, "master-send-return.png");
    JsonNode *record = document_wait("dj_mixer", "DJ mixer", master_routes_saved);
    json_node_free(record); json_node_free(detail); window_close(&window); window = window_open(); assert_gates_off(&window);
    g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, NULL, NULL));
    g_assert_cmpint(mixer.master.send_channel, ==, 7); g_assert_cmpint(mixer.master.return_channel, ==, -1);
    g_assert_cmpint(mixer.master.return_effect, ==, 0); g_assert_cmpint(mixer.master.fx_slot, ==, -1);
    g_assert_cmpfloat_with_epsilon(mixer.master.send_db, 0, 0.001);
    g_assert_cmpfloat_with_epsilon(mixer.master.return_db, -6.0206, 0.001);
    run_for(250);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(window.root, "dj-channel-0-send-target"))), ==, "7");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(window.root, "dj-channel-0-return-source"))), ==, "0");
    g_assert_cmpfloat_with_epsilon(gtk_adjustment_get_value(adjustment(window.root, "dj-channel-0-route-return")), -6.0206, 0.001);
    click(window.root, "dj-channel-8-remove"); run_for(250);
    g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, NULL, NULL));
    g_assert_cmpint(mixer.master.send_channel, ==, -1); g_assert_cmpint(mixer.master.return_channel, ==, -1);
    g_assert_cmpint(mixer.master.return_effect, ==, 0);
    send = find(window.root, "dj-channel-0-send-target"); receive = find(window.root, "dj-channel-0-return-source");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(send)), ==, "-1");
    g_assert_false(gtk_combo_box_set_active_id(GTK_COMBO_BOX(send), "7"));
    g_assert_false(gtk_combo_box_set_active_id(GTK_COMBO_BOX(receive), "7"));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(receive)), ==, "0");
    reset_mixer(&window); window_close(&window);
}
static void test_channel_routing_and_on_air(void)
{
    Window window = window_open(); assert_gates_off(&window); reset_mixer(&window);
    radios_window_select_profile(window.window, "dj"); run_for(250);
    for (guint slot = 1; slot <= 4; ++slot) {
        const gchar *removed[] = {"send", "return", "monitor"};
        for (guint control = 0; control < G_N_ELEMENTS(removed); ++control) {
            gchar *name = g_strdup_printf("dj-fx-%u-%s", slot, removed[control]);
            g_assert_null(find(window.root, name)); g_free(name);
        }
    }
    for (guint number = 1; number <= 4; ++number) {
        gchar *name = g_strdup_printf("dj-sampler-channel-2-pad-%u", number);
        GtkWidget *pad = find(window.root, name); g_assert_true(GTK_IS_BUTTON(pad));
        GtkAllocation allocation; gtk_widget_get_allocation(pad, &allocation);
        g_assert_cmpint(allocation.width, >=, 80); g_assert_cmpint(allocation.width, ==, allocation.height);
        GtkWidget *overlay = gtk_widget_get_parent(pad), *grid = gtk_widget_get_parent(overlay);
        g_assert_true(GTK_IS_OVERLAY(overlay)); g_assert_true(GTK_IS_GRID(grid));
        gint column, row; gtk_container_child_get(GTK_CONTAINER(grid), overlay,
            "left-attach", &column, "top-attach", &row, NULL);
        g_assert_cmpint(column, ==, (gint)(number - 1) % 2); g_assert_cmpint(row, ==, (gint)(number - 1) / 2);
        gchar *edit = g_strconcat(name, "-edit", NULL); g_assert_nonnull(find(overlay, edit)); g_free(edit); g_free(name);
    }
    capture_dj(&window, "mixer-layout.png");
    JsonNode *detail = media_import(&window, "DJ kanal yönlendirme", 1000, 0.2);
    native_load(&window, detail, 1); click(window.root, "dj-channel-2-play");
    deck_wait(&window, 1, RADIOS_AUDIO_UI_PLAYING, 100); run_for(200); Capture dry = measure("MAIN");
    g_assert_cmpfloat(dry.rms[0], >, 0.04);
    knob(window.root, "dj-channel-2-route-send", 0); knob(window.root, "dj-channel-8-route-return", -6.0206);
    range(window.root, "dj-channel-8-gain", -6.0206); combo(window.root, "dj-channel-2-send-target", "7");
    run_for(200); Capture sent = measure("MAIN");
    g_assert_cmpfloat(sent.rms[0] / dry.rms[0], >, 1.20); g_assert_cmpfloat(sent.rms[0] / dry.rms[0], <, 1.30);
    g_assert_false(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(window.root, "dj-channel-2-send-target")), "1"));
    combo(window.root, "dj-channel-2-send-target", "7"); run_for(100); Capture duplicate = measure("MAIN");
    g_assert_cmpfloat(duplicate.rms[0] / sent.rms[0], >, 0.96); g_assert_cmpfloat(duplicate.rms[0] / sent.rms[0], <, 1.04);
    RadioSAudioMixerConfig mixer; GError *error = NULL;
    g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, NULL, NULL));
    mixer.effects[0].type = RADIOS_FX_DELAY; mixer.effects[0].enabled = TRUE;
    mixer.effects[0].depth = 1; mixer.effects[0].tempo_bpm = 300; mixer.effects[0].return_db = 0;
    require_call(radios_audio_ui_set_mixer(window.ctx, &mixer, &error), &error);
    combo(window.root, "dj-channel-2-effector", "0");
    combo(window.root, "dj-channel-8-return-source", "0"); run_for(350); Capture both = measure("MAIN");
    g_assert_cmpfloat(both.rms[0] / dry.rms[0], >, 1.45); g_assert_cmpfloat(both.rms[0] / dry.rms[0], <, 1.55);
    combo(window.root, "dj-channel-8-send-target", "1"); run_for(250);
    g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, NULL, NULL));
    g_assert_cmpint(mixer.channels[7].send_channel, ==, -1); g_assert_cmpint(mixer.channels[1].send_channel, ==, 7);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(window.root, "dj-channel-8-send-target"))), ==, "-1");
    combo(window.root, "dj-channel-2-send-target", "-1");
    wait_main_effect_capture(&window, 9600); /* 300 BPM, full-depth DELAY: 200 ms. */
    Capture returned = measure("MAIN");
    g_test_message("Steady RETURN RMS ratio %.6f", returned.rms[0] / dry.rms[0]);
    g_assert_cmpfloat(returned.rms[0] / dry.rms[0], >, 1.20); g_assert_cmpfloat(returned.rms[0] / dry.rms[0], <, 1.30);
    combo(window.root, "dj-channel-2-send-target", "7"); click(window.root, "dj-channel-2-stop");
    deck_wait(&window, 1, RADIOS_AUDIO_UI_READY, 0);
    combo(window.root, "dj-channel-2-return-source", "0");
    GtkWidget *on_air = find(window.root, "dj-speech-on-air");
    wait_until(microphone_open_ready, &window, "ON AIR requires live MAIN and private microphone readiness after STOP flush");
    click(window.root, "dj-speech-on-air"); run_for(250);
    g_assert_true(radios_audio_ui_microphone_active(window.ctx));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(window.root, "dj-microphone-gate"))));
    click(window.root, "dj-microphone-gate"); run_for(250);
    g_assert_false(radios_audio_ui_microphone_active(window.ctx)); g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(on_air)));
    wait_until(microphone_open_ready, &window, "MAIN and microphone are ready after the microphone-close flush");
    click(window.root, "dj-speech-on-air"); run_for(250); g_assert_true(radios_audio_ui_microphone_active(window.ctx));
    capture_dj(&window, "mixer-on-air.png");
    JsonNode *record = document_wait("dj_mixer", "DJ mixer", channel_routes_saved);
    json_node_free(record); json_node_free(detail); window_close(&window); window = window_open(); assert_gates_off(&window);
    g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, NULL, NULL));
    g_assert_cmpint(mixer.channels[1].send_channel, ==, 7); g_assert_cmpint(mixer.channels[7].return_channel, ==, -1);
    g_assert_cmpint(mixer.channels[1].return_effect, ==, 0); g_assert_cmpint(mixer.channels[7].return_effect, ==, 0);
    g_assert_cmpfloat_with_epsilon(mixer.channels[1].send_db, 0, 0.001);
    g_assert_cmpfloat_with_epsilon(mixer.channels[7].return_db, -6.0206, 0.001);
    run_for(250); g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(window.root, "dj-channel-2-send-target"))), ==, "7");
    click(window.root, "dj-channel-8-remove"); run_for(250);
    g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, NULL, NULL));
    g_assert_cmpint(mixer.channels[1].send_channel, ==, -1); g_assert_cmpint(mixer.channels[7].return_channel, ==, -1);
    g_assert_cmpint(mixer.channels[1].return_effect, ==, 0); g_assert_cmpint(mixer.channels[7].return_effect, ==, -1);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(window.root, "dj-channel-2-return-source"))), ==, "0");
    g_assert_null(find(window.root, "dj-channel-8"));
    GtkWidget *choices = find(window.root, "dj-channel-2-send-target");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(choices)), ==, "-1");
    g_assert_false(gtk_combo_box_set_active_id(GTK_COMBO_BOX(choices), "7"));
    reset_mixer(&window); window_close(&window);
}

static gboolean retired_effect_saved(JsonObject *configuration)
{
    JsonArray *effects = json_object_get_array_member(configuration, "effects");
    if (json_array_get_length(effects) != 5) return FALSE;
    JsonObject *retired = json_array_get_object_element(effects, 4);
    if (json_object_get_boolean_member(retired, "enabled") || json_object_has_member(retired, "cue")) return FALSE;
    for (guint i = 0; i < RADIOS_AUDIO_MIXER_CHANNELS; ++i)
        if (json_object_get_int_member(channel_config(configuration, i), "fx_slot") == 4) return FALSE;
    return json_object_get_int_member(json_object_get_object_member(configuration, "master"), "fx_slot") != 4;
}
static gboolean push_to_talk_preference_saved(JsonObject *configuration)
{
    return retired_effect_saved(configuration) &&
        fabs(json_object_get_double_member(configuration, "crossfader") - .625) < .001;
}
static void assert_retired_effect(const RadioSAudioMixerConfig *mixer)
{
    g_assert_false(mixer->effects[4].enabled); g_assert_false(mixer->effects[4].cue);
    for (guint i = 0; i < RADIOS_AUDIO_MIXER_CHANNELS; ++i) g_assert_cmpint(mixer->channels[i].fx_slot, !=, 4);
    g_assert_cmpint(mixer->master.fx_slot, !=, 4);
}
static void test_four_pedals_legacy_migration(void)
{
    Window window = window_open(); reset_mixer(&window);
    radios_window_select_profile(window.window, "dj"); run_for(250);
    GtkWidget *rack = find(window.root, "dj-effects-pedals"); g_assert_true(GTK_IS_BOX(rack));
    GList *pedals = gtk_container_get_children(GTK_CONTAINER(rack));
    g_assert_cmpuint(g_list_length(pedals), ==, 4); g_list_free(pedals);
    gint previous_right = -1;
    for (guint slot = 1; slot <= 4; ++slot) {
        gchar *name = g_strdup_printf("dj-fx-%u-pedal", slot); GtkWidget *pedal = find(window.root, name);
        g_assert_nonnull(pedal); g_assert_true(gtk_widget_get_parent(pedal) == rack);
        GtkAllocation allocation; gtk_widget_get_allocation(pedal, &allocation);
        g_assert_cmpint(allocation.width, >, 100); g_assert_cmpint(allocation.height, >, 100);
        g_assert_cmpint(allocation.x, >=, previous_right); previous_right = allocation.x + allocation.width;
        g_free(name); name = g_strdup_printf("dj-fx-%u-display", slot);
        GtkWidget *display = find(window.root, name); g_assert_true(GTK_IS_BOX(display));
        g_assert_true(gtk_widget_get_mapped(display)); gtk_widget_get_allocation(display, &allocation);
        g_assert_cmpint(allocation.width, >, 100); g_assert_cmpint(allocation.height, >, 40);
        g_free(name);
    }
    g_assert_null(find(window.root, "dj-fx-5-pedal")); g_assert_null(find(window.root, "dj-fx-5-type"));
    GtkWidget *selector = find(window.root, "dj-channel-1-effector");
    g_assert_false(gtk_combo_box_set_active_id(GTK_COMBO_BOX(selector), "4"));
    selector = find(window.root, "dj-channel-0-effector");
    g_assert_false(gtk_combo_box_set_active_id(GTK_COMBO_BOX(selector), "4"));
    capture_dj(&window, "prompt6-four-pedals-idle.png");

    /* A valid old five-slot record is migrated in memory, without rewriting
     * user storage during a load or accidentally assigning slot five to four. */
    JsonNode *record = document_read("dj_mixer", "DJ mixer"); g_assert_nonnull(record);
    JsonNode *legacy = deep_copy(json_object_get_member(json_node_get_object(record), "config"));
    JsonObject *old = json_node_get_object(legacy);
    json_object_set_int_member(channel_config(old, 0), "fx_slot", 4);
    json_object_set_int_member(json_object_get_object_member(old, "master"), "fx_slot", 4);
    JsonObject *fifth = json_array_get_object_element(json_object_get_array_member(old, "effects"), 4);
    json_object_set_boolean_member(fifth, "enabled", TRUE); json_object_set_int_member(fifth, "type", RADIOS_FX_FILTER);
    json_object_set_double_member(fifth, "depth", 1);
    window_close(&window);
    JsonNode *saved = document_remote_save(record, old); json_node_free(saved); json_node_free(legacy); json_node_free(record);
    window = window_open(); assert_gates_off(&window);
    RadioSAudioMixerConfig mixer; g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, NULL, NULL));
    assert_retired_effect(&mixer); g_assert_cmpint(mixer.channels[0].fx_slot, ==, -1); g_assert_cmpint(mixer.master.fx_slot, ==, -1);
    record = document_read("dj_mixer", "DJ mixer");
    fifth = json_array_get_object_element(json_object_get_array_member(json_object_get_object_member(json_node_get_object(record), "config"), "effects"), 4);
    g_assert_true(json_object_get_boolean_member(fifth, "enabled")); json_node_free(record);

    JsonNode *detail = media_import(&window, "Dört pedal legacy migration", 997, .2);
    native_load(&window, detail, 0); click(window.root, "dj-channel-1-play");
    deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100); run_for(250); Capture dry = measure("MAIN");
    g_assert_cmpfloat(dry.rms[0], >, .1);
    /* Every controller mutation also retires a caller's legacy fifth slot. */
    mixer.channels[0].fx_slot = 4; mixer.master.fx_slot = 4;
    mixer.effects[4].enabled = mixer.effects[4].cue = TRUE;
    mixer.effects[4].type = RADIOS_FX_FILTER; mixer.effects[4].depth = 1;
    mixer.effects[4].return_db = mixer.channels[0].fx_return_db = mixer.master.fx_return_db = 0;
    GError *error = NULL; require_call(radios_audio_ui_set_mixer(window.ctx, &mixer, &error), &error);
    g_assert_true(mixer.effects[4].enabled); /* Input ownership is preserved. */
    RadioSAudioMixerConfig live; g_assert_true(radios_audio_ui_get_mixer(window.ctx, &live, NULL, NULL)); assert_retired_effect(&live);
    run_for(250); Capture migrated = measure("MAIN"), cue = measure("CUE");
    g_assert_cmpfloat(migrated.rms[0] / dry.rms[0], >, .97); g_assert_cmpfloat(migrated.rms[0] / dry.rms[0], <, 1.03);
    g_assert_cmpfloat(cue.peak[0], <, .00001);
    /* Retirement must not mask malformed configurations. */
    mixer.effects[4].depth = NAN;
    g_assert_false(radios_audio_ui_set_mixer(window.ctx, &mixer, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
    mixer.effects[4].depth = 1; mixer.channels[0].fx_slot = 5;
    g_assert_false(radios_audio_ui_set_mixer(window.ctx, &mixer, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
    /* A durable user change automatically commits the migrated safe record. */
    knob(window.root, "dj-channel-1-eq-high", 1);
    record = document_wait("dj_mixer", "DJ mixer", retired_effect_saved);
    json_node_free(record); json_node_free(detail); reset_mixer(&window); window_close(&window);
}

static gboolean private_monitor_source_saved(JsonObject *configuration)
{ return radios_json_int(configuration, "monitor_source", -1) == 1; }
static void private_microphone_monitor(Window *window)
{
    /* MONITOR is the private microphone's actual Pulse input. Configure it
     * through the controller so later CUE changes and saved-preference
     * acknowledgements preserve this route. Engine-only preferences are
     * intentionally replaced by the next controller preference application. */
    combo(window->root, "header-output-source", "1");
    JsonNode *saved = document_wait("audio_preferences", "Ses aygıtları", private_monitor_source_saved);
    document_ack_wait(window, saved); json_node_free(saved);
    wait_until(window_ready, window, "private microphone MONITOR source and output configuration settle");
    toggle(window->root, "header-output-cue", TRUE);
}
static void talk_press(Window *window)
{
    GtkWidget *button = find(window->root, "dj-speech-push-to-talk"); g_assert_true(GTK_IS_BUTTON(button));
    wait_until(widget_enabled, button, "Bas konuş requires ready microphone and MAIN");
    g_signal_emit_by_name(button, "pressed"); g_assert_true(radios_audio_ui_microphone_active(window->ctx));
    run_for(150); g_assert_true(gtk_widget_is_sensitive(button));
    g_assert_true(gtk_style_context_has_class(gtk_widget_get_style_context(button), "is-active"));
}
static void talk_assert_off(Window *window)
{
    run_for(150); g_assert_false(radios_audio_ui_microphone_active(window->ctx));
    GtkWidget *button = find(window->root, "dj-speech-push-to-talk");
    if (button) g_assert_false(gtk_style_context_has_class(gtk_widget_get_style_context(button), "is-active"));
}
static void talk_event(GtkWidget *widget, GdkEventType type, const gchar *signal, guint keyval)
{
    GdkEvent *event = gdk_event_new(type);
    event->any.window = g_object_ref(gtk_widget_get_window(widget)); event->any.send_event = TRUE;
    if (type == GDK_KEY_PRESS || type == GDK_KEY_RELEASE) { event->key.keyval = keyval; event->key.time = GDK_CURRENT_TIME; }
    if (type == GDK_FOCUS_CHANGE) event->focus_change.in = FALSE;
    if (type == GDK_LEAVE_NOTIFY) event->crossing.detail = GDK_NOTIFY_NONLINEAR;
    gboolean handled = FALSE; g_signal_emit_by_name(widget, signal, event, &handled);
    if (type == GDK_KEY_PRESS || type == GDK_KEY_RELEASE) g_assert_true(handled);
    gdk_event_free(event);
}
static void test_push_to_talk_lifecycle(void)
{
    Window window = window_open(); assert_gates_off(&window); reset_mixer(&window);
    radios_window_select_profile(window.window, "dj"); run_for(250);
    GtkWidget *button = find(window.root, "dj-speech-push-to-talk");
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(button)), ==, "Bas konuş");
    /* An isolated deck drives the private MONITOR capture, used as the private
     * microphone input. MAIN receives this real PCM only while PTT is held. */
    JsonNode *detail = media_import(&window, "Bas konuş özel mikrofon", 631, .125);
    native_load(&window, detail, 1);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    RadioSAudioPreferences prefs; radios_audio_preferences_default(&prefs);
    g_strlcpy(prefs.main_sink, main_sink, sizeof prefs.main_sink); g_strlcpy(prefs.cue_sink, cue_sink, sizeof prefs.cue_sink);
    g_strlcpy(prefs.monitor_sink, monitor_sink, sizeof prefs.monitor_sink);
    g_strlcpy(prefs.microphone_source, g_getenv("RADIOS_TEST_MIC_SOURCE"), sizeof prefs.microphone_source);
    prefs.monitor_source = 1; prefs.cue_source = -1;
    private_microphone_monitor(&window);
    GError *error = NULL;
    require_call(radios_audio_engine_set_deck_main(engine, 1, FALSE, &error), &error);
    require_call(radios_audio_ui_play(window.ctx, 1, &error), &error);
    deck_wait(&window, 1, RADIOS_AUDIO_UI_PLAYING, 100); run_for(300);
    Capture mic = measure("MONITOR"), silent = measure("MAIN");
    g_assert_cmpfloat(mic.rms[0], >, .05); g_assert_cmpfloat(silent.peak[0], <, .00001);

    talk_press(&window); run_for(150); Capture held = measure("MAIN");
    g_assert_cmpfloat(held.rms[0], >, .05); capture_dj(&window, "prompt6-push-to-talk-held.png");
    g_signal_emit_by_name(button, "released"); talk_assert_off(&window);
    silent = measure("MAIN"); g_assert_cmpfloat(silent.peak[0], <, .00001);
    const guint keys[] = {GDK_KEY_space, GDK_KEY_Return, GDK_KEY_KP_Enter};
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) {
        /* Every gate change flushes the outputs; under load MAIN may report
         * not ready for longer than talk_assert_off's 150 ms. A real key press
         * cannot reach the insensitive button either, so wait for it. */
        wait_until(widget_enabled, button, "Bas konuş is available again after the previous release");
        talk_event(button, GDK_KEY_PRESS, "key-press-event", keys[i]);
        g_assert_true(radios_audio_ui_microphone_active(window.ctx));
        talk_event(button, GDK_KEY_PRESS, "key-press-event", keys[i]); /* key repeat */
        talk_event(button, GDK_KEY_RELEASE, "key-release-event", keys[i]); talk_assert_off(&window);
    }
    const struct { GdkEventType type; const gchar *signal; } losses[] = {
        {GDK_LEAVE_NOTIFY, "leave-notify-event"}, {GDK_FOCUS_CHANGE, "focus-out-event"},
        {GDK_GRAB_BROKEN, "grab-broken-event"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(losses); ++i) {
        talk_press(&window); talk_event(button, losses[i].type, losses[i].signal, 0); talk_assert_off(&window);
    }
    talk_press(&window);
    GtkWidget *modal_owner = find(window.root, "dj-mixer-reload"); g_assert_nonnull(modal_owner);
    gtk_grab_add(modal_owner); talk_assert_off(&window); gtk_grab_remove(modal_owner);
    talk_press(&window); g_assert_true(radios_window_select_profile(window.window, "playlist")); talk_assert_off(&window);
    g_assert_true(radios_window_select_profile(window.window, "dj")); run_for(150);
    talk_press(&window); talk_event(window.root, GDK_FOCUS_CHANGE, "focus-out-event", 0); talk_assert_off(&window);

    /* PTT does not take ownership of a microphone latched on by ON AIR. */
    wait_until(microphone_open_ready, &window, "MAIN and microphone recover after final PTT focus-loss flush");
    require_call(radios_audio_ui_microphone(window.ctx, TRUE, &error), &error); run_for(150);
    g_assert_false(gtk_widget_is_sensitive(button)); g_signal_emit_by_name(button, "pressed"); g_signal_emit_by_name(button, "released");
    g_assert_true(radios_audio_ui_microphone_active(window.ctx));
    require_call(radios_audio_ui_microphone(window.ctx, FALSE, &error), &error); talk_assert_off(&window);

    /* Device loss cannot grant a gate, even when a stale input event arrives. */
    RadioSAudioPreferences missing = prefs; missing.main_sink[0] = '\0';
    require_call(radios_audio_engine_apply_preferences(engine, &missing, &error), &error); run_for(250);
    g_assert_false(gtk_widget_is_sensitive(button)); g_signal_emit_by_name(button, "pressed"); talk_assert_off(&window);
    require_call(radios_audio_engine_apply_preferences(engine, &prefs, &error), &error); run_for(250);

    talk_press(&window); range(window.root, "dj-crossfader", .625);
    JsonNode *record = document_wait("dj_mixer", "DJ mixer", push_to_talk_preference_saved);
    JsonObject *saved = json_object_get_object_member(json_node_get_object(record), "config");
    g_assert_false(json_object_has_member(saved, "microphone_gate")); g_assert_false(json_object_has_member(saved, "push_to_talk"));
    g_assert_false(json_object_has_member(json_object_get_object_member(saved, "speech"), "microphone_gate")); json_node_free(record);
    g_assert_true(radios_audio_ui_microphone_active(window.ctx));
    gtk_widget_destroy(button); talk_assert_off(&window);
    json_node_free(detail); window_close(&window); radios_audio_engine_unref(engine);

    window = window_open(); assert_gates_off(&window); radios_window_select_profile(window.window, "dj"); run_for(150);
    engine = radios_audio_ui_engine_acquire(window.ctx); talk_press(&window); window_close(&window);
    RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot); g_assert_false(snapshot.microphone_gate);
    radios_audio_engine_unref(engine);
    window = window_open(); assert_gates_off(&window); window_close(&window);
}
typedef struct { Window *window; gint slot; } SpeechEffectorExpectation;
static gboolean speech_effector_ready(gpointer data)
{
    SpeechEffectorExpectation *expected = data;
    RadioSAudioMixerConfig mixer;
    GtkWidget *selector = find(expected->window->root, "dj-speech-effector");
    if (!GTK_IS_COMBO_BOX(selector) ||
        !radios_audio_ui_get_mixer(expected->window->ctx, &mixer, NULL, NULL)) return FALSE;
    gchar id[12]; g_snprintf(id, sizeof id, "%d", expected->slot);
    return mixer.speech_fx_slot == expected->slot &&
        !g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(selector)), id);
}
static void speech_effector_wait(Window *window, gint slot)
{
    SpeechEffectorExpectation expected = {window, slot};
    wait_until(speech_effector_ready, &expected, "speech selector and genuine engine mixer accept the same effect route");
}
static gboolean speech_effector_saved(JsonObject *configuration)
{
    JsonObject *speech = json_object_get_object_member(configuration, "speech");
    return speech && json_object_has_member(speech, "fx_slot") &&
        json_object_get_int_member(speech, "fx_slot") == 3 &&
        !json_object_get_boolean_member(speech, "ducking") &&
        fabs(json_object_get_double_member(speech, "bed_db") + 25) < 0.001;
}
static void test_speech_effector_route(void)
{
    Window window = window_open(); assert_gates_off(&window); reset_mixer(&window);
    g_assert_true(radios_window_select_profile(window.window, "dj")); run_for(150);
    GtkWidget *selector = find(window.root, "dj-speech-effector"); g_assert_true(GTK_IS_COMBO_BOX(selector));
    GtkTreeModel *choices = gtk_combo_box_get_model(GTK_COMBO_BOX(selector));
    g_assert_cmpint(gtk_tree_model_iter_n_children(choices, NULL), ==, 5);
    speech_effector_wait(&window, -1);
    g_assert_false(radios_audio_ui_microphone_active(window.ctx));

    /* Picking a speech effect preserves unrelated channel, MASTER and ducking
     * controls, and does not enable either the pedal or the microphone gate. */
    knob(window.root, "dj-channel-1-eq-low", -7.5);
    knob(window.root, "dj-channel-0-eq-high", 2.5);
    range(window.root, "dj-crossfader", .375);
    range(window.root, "dj-speech-bed", -24);
    toggle(window.root, "dj-speech-ducking", FALSE); run_for(150);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    const gint slots[] = {0, 1, 2, -1, 3};
    for (guint i = 0; i < G_N_ELEMENTS(slots); ++i) {
        gchar id[12]; g_snprintf(id, sizeof id, "%d", slots[i]);
        combo(window.root, "dj-speech-effector", id); speech_effector_wait(&window, slots[i]);
        RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
        g_assert_cmpint(snapshot.mixer.speech_fx_slot, ==, slots[i]);
        g_assert_cmpfloat_with_epsilon(snapshot.mixer.channels[0].eq_low_db, -7.5, .001);
        g_assert_cmpfloat_with_epsilon(snapshot.mixer.master.eq_high_db, 2.5, .001);
        g_assert_cmpfloat_with_epsilon(snapshot.mixer.crossfader, .375, .001);
        g_assert_cmpfloat_with_epsilon(snapshot.mixer.speech_bed_db, -24, .001);
        g_assert_false(snapshot.mixer.speech_ducking);
        g_assert_false(snapshot.microphone_gate);
        for (guint slot = 0; slot < RADIOS_AUDIO_FX_SLOTS; ++slot)
            g_assert_false(snapshot.mixer.effects[slot].enabled);
    }

    /* The route is a durable preference even when changed while PTT is held;
     * opening the next window never restores that live microphone state. */
    talk_press(&window); range(window.root, "dj-speech-bed", -25);
    JsonNode *record = document_wait("dj_mixer", "DJ mixer", speech_effector_saved);
    JsonObject *saved = json_object_get_object_member(json_node_get_object(record), "config");
    JsonObject *speech = json_object_get_object_member(saved, "speech");
    const gchar *runtime_fields[] = {"microphone_gate", "push_to_talk", "on_air"};
    for (guint i = 0; i < G_N_ELEMENTS(runtime_fields); ++i) {
        g_assert_false(json_object_has_member(saved, runtime_fields[i]));
        g_assert_false(json_object_has_member(speech, runtime_fields[i]));
    }
    g_assert_true(radios_audio_ui_microphone_active(window.ctx));
    g_signal_emit_by_name(find(window.root, "dj-speech-push-to-talk"), "released"); talk_assert_off(&window);
    window_close(&window); radios_audio_engine_unref(engine);

    window = window_open(); assert_gates_off(&window);
    g_assert_true(radios_window_select_profile(window.window, "dj")); speech_effector_wait(&window, 3);
    RadioSAudioMixerConfig restored; g_assert_true(radios_audio_ui_get_mixer(window.ctx, &restored, NULL, NULL));
    g_assert_cmpfloat_with_epsilon(restored.channels[0].eq_low_db, -7.5, .001);
    g_assert_cmpfloat_with_epsilon(restored.master.eq_high_db, 2.5, .001);
    g_assert_cmpfloat_with_epsilon(restored.crossfader, .375, .001);
    g_assert_cmpfloat_with_epsilon(restored.speech_bed_db, -25, .001); g_assert_false(restored.speech_ducking);
    window_close(&window);

    /* A pre-feature record omits fx_slot. Its remaining preferences survive
     * reopening, while the speech effect is explicitly off rather than slot 0. */
    JsonNode *legacy = deep_copy(json_object_get_member(json_node_get_object(record), "config"));
    json_object_remove_member(json_object_get_object_member(json_node_get_object(legacy), "speech"), "fx_slot");
    JsonNode *changed = document_remote_save(record, json_node_get_object(legacy));
    json_node_free(changed); json_node_free(legacy); json_node_free(record);
    window = window_open(); assert_gates_off(&window);
    g_assert_true(radios_window_select_profile(window.window, "dj")); speech_effector_wait(&window, -1);
    g_assert_true(radios_audio_ui_get_mixer(window.ctx, &restored, NULL, NULL));
    g_assert_cmpfloat_with_epsilon(restored.speech_bed_db, -25, .001); g_assert_false(restored.speech_ducking);
    reset_mixer(&window); window_close(&window);
}

typedef struct { Window *window; gboolean to_master; gint channel; } SpeechDestinationExpectation;
static gboolean speech_destination_ready(gpointer data)
{
    SpeechDestinationExpectation *expected = data;
    RadioSAudioMixerConfig mixer;
    GtkWidget *master = find(expected->window->root, "dj-speech-to-master");
    GtkWidget *channel = find(expected->window->root, "dj-speech-channel");
    if (!GTK_IS_TOGGLE_BUTTON(master) || !GTK_IS_COMBO_BOX(channel) ||
        !radios_audio_ui_get_mixer(expected->window->ctx, &mixer, NULL, NULL)) return FALSE;
    gchar id[12]; g_snprintf(id, sizeof id, "%d", expected->channel);
    return mixer.speech_to_master == expected->to_master && mixer.speech_channel == expected->channel &&
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(master)) == expected->to_master &&
        gtk_widget_is_sensitive(channel) == !expected->to_master &&
        !g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(channel)), id);
}
static void speech_destination_wait(Window *window, gboolean to_master, gint channel)
{
    SpeechDestinationExpectation expected = {window, to_master, channel};
    wait_until(speech_destination_ready, &expected, "microphone destination widgets match the genuine engine mixer");
}
static gboolean speech_destination_saved(JsonObject *configuration, gboolean to_master)
{
    JsonObject *speech = json_object_get_object_member(configuration, "speech");
    return speech && json_object_has_member(speech, "to_master") && json_object_has_member(speech, "channel") &&
        json_object_get_boolean_member(speech, "to_master") == to_master &&
        json_object_get_int_member(speech, "channel") == 7 && json_object_get_int_member(speech, "fx_slot") == 3;
}
static gboolean speech_destination_channel_saved(JsonObject *configuration)
{ return speech_destination_saved(configuration, FALSE); }
static gboolean speech_destination_master_saved(JsonObject *configuration)
{ return speech_destination_saved(configuration, TRUE); }
static void speech_destination_sources_unchanged(Window *window, const RadioSAudioMixerConfig *before)
{
    RadioSAudioMixerConfig after; g_assert_true(radios_audio_ui_get_mixer(window->ctx, &after, NULL, NULL));
    for (guint i = 0; i < RADIOS_AUDIO_MIXER_CHANNELS; ++i)
        g_assert_cmpint(after.channels[i].source_deck, ==, before->channels[i].source_deck);
    g_assert_cmpint(after.speech_fx_slot, ==, 3);
}
static void test_speech_master_channel_route(void)
{
    Window window = window_open(); assert_gates_off(&window); reset_mixer(&window);
    g_assert_true(radios_window_select_profile(window.window, "dj")); run_for(250);
    GtkWidget *master = find(window.root, "dj-speech-to-master");
    GtkWidget *channel = find(window.root, "dj-speech-channel");
    g_assert_true(GTK_IS_CHECK_BUTTON(master)); g_assert_true(GTK_IS_COMBO_BOX(channel));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(master)), ==, "Master’a gönder");
    speech_destination_wait(&window, TRUE, -1);
    g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_combo_box_get_model(GTK_COMBO_BOX(channel)), NULL), ==, 9);
    g_assert_false(gtk_combo_box_set_active_id(GTK_COMBO_BOX(channel), "31"));
    combo(window.root, "dj-speech-effector", "3"); speech_effector_wait(&window, 3);
    RadioSAudioMixerConfig before; g_assert_true(radios_audio_ui_get_mixer(window.ctx, &before, NULL, NULL));
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);

    /* Every displayed channel can be selected without changing its source or
     * opening the microphone. MASTER remembers the independent last choice. */
    toggle(window.root, "dj-speech-to-master", FALSE); speech_destination_wait(&window, FALSE, -1);
    for (gint i = 0; i < 8; ++i) {
        gchar id[12]; g_snprintf(id, sizeof id, "%d", i);
        combo(window.root, "dj-speech-channel", id); speech_destination_wait(&window, FALSE, i);
        speech_destination_sources_unchanged(&window, &before);
        RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
        g_assert_false(snapshot.microphone_gate);
    }
    toggle(window.root, "dj-speech-to-master", TRUE); speech_destination_wait(&window, TRUE, 7);
    toggle(window.root, "dj-speech-to-master", FALSE); speech_destination_wait(&window, FALSE, 7);
    combo(window.root, "dj-speech-effector", "0"); speech_effector_wait(&window, 0);
    speech_destination_wait(&window, FALSE, 7);
    combo(window.root, "dj-speech-effector", "3"); speech_effector_wait(&window, 3);
    speech_destination_sources_unchanged(&window, &before); run_for(350);

    /* A private MONITOR signal is the genuine microphone input. A second
     * legacy microphone strip cannot bypass the selected target's fader. */
    JsonNode *detail = media_import(&window, "Mikrofon MASTER ve kanal yönlendirme", 631, .125);
    native_load(&window, detail, 1);
    private_microphone_monitor(&window);
    GError *error = NULL;
    require_call(radios_audio_engine_set_deck_main(engine, 1, FALSE, &error), &error);
    require_call(radios_audio_ui_play(window.ctx, 1, &error), &error);
    deck_wait(&window, 1, RADIOS_AUDIO_UI_PLAYING, 100); run_for(300);
    Capture mic = measure("MONITOR"), silent = measure("MAIN");
    g_assert_cmpfloat(mic.rms[0], >, .05); g_assert_cmpfloat(silent.peak[0], <, .00001);
    combo(window.root, "dj-channel-1-source", "-2");
    range(window.root, "dj-channel-8-gain", -6.0206);
    toggle(window.root, "dj-speech-to-master", TRUE); speech_destination_wait(&window, TRUE, 7); run_for(350);
    wait_until(microphone_open_ready, &window, "MAIN and private microphone readiness before ON AIR");
    click(window.root, "dj-speech-on-air"); run_for(250);
    g_assert_true(radios_audio_ui_microphone_active(window.ctx)); Capture dry_master = measure("MAIN");
    g_assert_cmpfloat(dry_master.rms[0], >, .05);
    g_assert_true(radios_audio_ui_get_mixer(window.ctx, &before, NULL, NULL));
    toggle(window.root, "dj-speech-to-master", FALSE); speech_destination_wait(&window, FALSE, 7); run_for(250);
    g_assert_true(radios_audio_ui_microphone_active(window.ctx)); speech_destination_sources_unchanged(&window, &before);
    Capture through_channel = measure("MAIN");
    g_assert_cmpfloat(through_channel.rms[0] / dry_master.rms[0], >, .47);
    g_assert_cmpfloat(through_channel.rms[0] / dry_master.rms[0], <, .53);
    toggle(window.root, "dj-channel-8-mute", TRUE); run_for(250); silent = measure("MAIN");
    g_assert_cmpfloat(silent.peak[0], <, .00001);
    toggle(window.root, "dj-channel-8-mute", FALSE); combo(window.root, "dj-speech-channel", "-1");
    speech_destination_wait(&window, FALSE, -1); run_for(250); silent = measure("MAIN");
    g_assert_cmpfloat(silent.peak[0], <, .00001);
    g_assert_true(radios_audio_ui_microphone_active(window.ctx));
    combo(window.root, "dj-speech-channel", "7"); speech_destination_wait(&window, FALSE, 7);
    toggle(window.root, "dj-speech-to-master", TRUE); speech_destination_wait(&window, TRUE, 7); run_for(250);
    Capture restored_master = measure("MAIN");
    g_assert_cmpfloat(restored_master.rms[0] / dry_master.rms[0], >, .97);
    g_assert_cmpfloat(restored_master.rms[0] / dry_master.rms[0], <, 1.03);
    toggle(window.root, "dj-speech-to-master", FALSE); speech_destination_wait(&window, FALSE, 7);

    /* Routing autosave never saves the live gate; reopening preserves both the
     * remembered channel and the independent effect preference with gates off. */
    JsonNode *record = document_wait("dj_mixer", "DJ mixer", speech_destination_channel_saved);
    document_ack_wait(&window, record);
    JsonObject *saved = json_object_get_object_member(json_node_get_object(record), "config");
    JsonObject *speech = json_object_get_object_member(saved, "speech");
    const gchar *runtime_fields[] = {"microphone_gate", "push_to_talk", "on_air"};
    for (guint i = 0; i < G_N_ELEMENTS(runtime_fields); ++i) {
        g_assert_false(json_object_has_member(saved, runtime_fields[i]));
        g_assert_false(json_object_has_member(speech, runtime_fields[i]));
    }
    require_call(radios_audio_ui_microphone(window.ctx, FALSE, &error), &error);
    json_node_free(record); json_node_free(detail); window_close(&window); radios_audio_engine_unref(engine);
    window = window_open(); assert_gates_off(&window); g_assert_true(radios_window_select_profile(window.window, "dj"));
    speech_destination_wait(&window, FALSE, 7); speech_effector_wait(&window, 3);
    toggle(window.root, "dj-speech-to-master", TRUE); speech_destination_wait(&window, TRUE, 7);
    record = document_wait("dj_mixer", "DJ mixer", speech_destination_master_saved);
    document_ack_wait(&window, record); json_node_free(record); window_close(&window);
    window = window_open(); assert_gates_off(&window); g_assert_true(radios_window_select_profile(window.window, "dj"));
    speech_destination_wait(&window, TRUE, 7); speech_effector_wait(&window, 3);
    record = document_read("dj_mixer", "DJ mixer"); g_assert_nonnull(record);
    JsonNode *legacy = deep_copy(json_object_get_member(json_node_get_object(record), "config"));
    speech = json_object_get_object_member(json_node_get_object(legacy), "speech");
    json_object_remove_member(speech, "to_master"); json_object_remove_member(speech, "channel");
    window_close(&window);
    JsonNode *changed = document_remote_save(record, json_node_get_object(legacy));
    json_node_free(changed); json_node_free(legacy); json_node_free(record);
    window = window_open(); assert_gates_off(&window); g_assert_true(radios_window_select_profile(window.window, "dj"));
    speech_destination_wait(&window, TRUE, -1); speech_effector_wait(&window, 3); run_for(350);
    record = document_read("dj_mixer", "DJ mixer");
    speech = json_object_get_object_member(json_object_get_object_member(json_node_get_object(record), "config"), "speech");
    g_assert_false(json_object_has_member(speech, "to_master")); g_assert_false(json_object_has_member(speech, "channel"));
    json_node_free(record);

    /* Removing a chosen target clears only that destination and leaves the
     * unchecked state intact; the microphone never falls back to MASTER. */
    toggle(window.root, "dj-speech-to-master", FALSE); combo(window.root, "dj-speech-channel", "7");
    speech_destination_wait(&window, FALSE, 7); click(window.root, "dj-channel-8-remove");
    speech_destination_wait(&window, FALSE, -1); speech_effector_wait(&window, 3);
    channel = find(window.root, "dj-speech-channel");
    g_assert_false(gtk_combo_box_set_active_id(GTK_COMBO_BOX(channel), "7"));
    g_assert_false(radios_audio_ui_microphone_active(window.ctx));
    reset_mixer(&window); window_close(&window);
}


static guint mixer_named_widgets(GtkWidget *root, const gchar *name)
{
    guint count = g_str_equal(gtk_widget_get_name(root), name) ? 1 : 0;
    if (!GTK_IS_CONTAINER(root)) return count;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    for (GList *item = children; item; item = item->next) count += mixer_named_widgets(item->data, name);
    g_list_free(children); return count;
}
static guint mixer_named_toplevels(const gchar *name, gboolean descendants)
{
    GList *windows = gtk_window_list_toplevels(); guint count = 0;
    for (GList *item = windows; item; item = item->next)
        count += descendants ? mixer_named_widgets(item->data, name) :
            (g_str_equal(gtk_widget_get_name(item->data), name) ? 1 : 0);
    g_list_free(windows); return count;
}
static GtkWidget *assert_mixer_detached(Window *window, GtkWidget *mixer)
{
    GtkWidget *floating = dialog_find("dj-mixer-window"); g_assert_true(GTK_IS_WINDOW(floating));
    g_assert_true(gtk_window_get_transient_for(GTK_WINDOW(floating)) == GTK_WINDOW(window->root));
    g_assert_true(find(floating, "dj-mixer") == mixer); g_assert_true(gtk_widget_is_ancestor(mixer, floating));
    g_assert_null(find(window->root, "dj-mixer"));
    g_assert_cmpuint(mixer_named_toplevels("dj-mixer-window", FALSE), ==, 1);
    g_assert_cmpuint(mixer_named_toplevels("dj-mixer", TRUE), ==, 1);
    GtkWidget *placeholder = find(window->root, "dj-mixer-detached-placeholder"); g_assert_nonnull(placeholder);
    g_assert_true(gtk_widget_get_visible(placeholder)); return floating;
}
static void assert_mixer_redocked(Window *window, GtkWidget *mixer, GtkWidget *original_parent)
{
    g_assert_null(dialog_find("dj-mixer-window")); g_assert_true(find(window->root, "dj-mixer") == mixer);
    g_assert_true(gtk_widget_get_parent(mixer) == original_parent);
    g_assert_cmpuint(mixer_named_toplevels("dj-mixer", TRUE), ==, 1);
    GtkWidget *placeholder = find(window->root, "dj-mixer-detached-placeholder"); g_assert_nonnull(placeholder);
    g_assert_false(gtk_widget_get_visible(placeholder));
}
static void assert_detached_mixer_controls(Window *window)
{
    RadioSAudioMixerConfig live; g_assert_true(radios_audio_ui_get_mixer(window->ctx, &live, NULL, NULL));
    g_assert_cmpfloat_with_epsilon(live.channels[0].gain_db, -6, .001);
    g_assert_cmpfloat_with_epsilon(live.channels[0].pan, -1, .001);
    g_assert_cmpfloat_with_epsilon(live.crossfader, .25, .001);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window->ctx); g_assert_nonnull(engine);
    RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot); radios_audio_engine_unref(engine);
    g_assert_cmpfloat_with_epsilon(snapshot.mixer.channels[0].gain_db, -6, .001);
    g_assert_cmpfloat_with_epsilon(snapshot.mixer.channels[0].pan, -1, .001);
    g_assert_cmpfloat_with_epsilon(snapshot.mixer.crossfader, .25, .001);
}
typedef struct { GtkWidget *combo; const gchar *name; } DetachedOutputName;
static gboolean detached_output_name_ready(gpointer value)
{
    DetachedOutputName *expected = value;
    gchar *text = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(expected->combo));
    gboolean ready = text && strstr(text, expected->name); g_free(text); return ready;
}
static void capture_mixer_window(GtkWidget *widget, const gchar *name)
{
    const gchar *directory = g_getenv("RADIOS_SHOT_DIR"); if (!directory) return;
    g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
    gtk_container_check_resize(GTK_CONTAINER(widget)); GtkAllocation allocation; gtk_widget_get_allocation(widget, &allocation);
    g_assert_cmpint(allocation.width, >, 0); g_assert_cmpint(allocation.height, >, 0);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
    cairo_t *context = cairo_create(surface); gtk_widget_draw(widget, context);
    g_assert_cmpint(cairo_status(context), ==, CAIRO_STATUS_SUCCESS); cairo_destroy(context);
    gchar *path = g_build_filename(directory, name, NULL);
    g_assert_cmpint(cairo_surface_write_to_png(surface, path), ==, CAIRO_STATUS_SUCCESS);
    g_free(path); cairo_surface_destroy(surface);
}
static gboolean mixer_handle_event(GtkWidget *handle, GdkEventType type, gdouble x, gdouble y)
{
    GdkEvent *event = gdk_event_new(type); GdkWindow *window = gtk_widget_get_window(handle); g_assert_nonnull(window);
    event->any.window = g_object_ref(window); event->any.send_event = TRUE;
    gint root_x, root_y; gdk_window_get_origin(window, &root_x, &root_y);
    if (type == GDK_MOTION_NOTIFY) {
        event->motion.time = GDK_CURRENT_TIME; event->motion.state = GDK_BUTTON1_MASK;
        event->motion.x = x; event->motion.y = y; event->motion.x_root = root_x + x; event->motion.y_root = root_y + y;
    } else {
        event->button.time = GDK_CURRENT_TIME; event->button.button = 1;
        event->button.x = x; event->button.y = y; event->button.x_root = root_x + x; event->button.y_root = root_y + y;
        event->button.state = type == GDK_BUTTON_RELEASE ? GDK_BUTTON1_MASK : 0;
    }
    GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(handle));
    gdk_event_set_device(event, gdk_seat_get_pointer(seat));
    gboolean handled = gtk_widget_event(handle, event); gdk_event_free(event); return handled;
}
static Capture mixer_wait_main_signal(void)
{
    /* A decoded cursor can advance before Pulse's first output reaches the
     * recorder. Require real stereo PCM before measuring relocation behavior. */
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    Capture signal;
    do {
        signal = measure("MAIN");
        if (signal.rms[0] > .05 && signal.rms[1] > .05) return signal;
    } while (g_get_monotonic_time() < deadline);
    g_test_message("Mixer startup MAIN PCM fence expired: RMS %.6f/%.6f", signal.rms[0], signal.rms[1]);
    return signal;
}
static void test_mixer_detach_lifecycle(void)
{
    Window window = window_open(); reset_mixer(&window);
    g_assert_true(radios_window_select_profile(window.window, "dj")); run_for(100);
    /* This relocation test asserts a manual fader value. Automatic loudness
     * owns the effective fader while measured media plays, so bypass it here. */
    toggle(window.root, "dj-channel-1-normalize", FALSE);
    GtkWidget *mixer = find(window.root, "dj-mixer"); g_assert_nonnull(mixer);
    GtkWidget *original_parent = gtk_widget_get_parent(mixer); g_assert_nonnull(original_parent);
    GtkWidget *gain = find(mixer, "dj-channel-1-gain"), *channel_name = find(mixer, "dj-channel-1-name");
    g_assert_true(GTK_IS_RANGE(gain)); g_assert_true(GTK_IS_ENTRY(channel_name));
    g_assert_true(GTK_IS_EVENT_BOX(find(mixer, "dj-mixer-drag-handle")));
    gchar *original_name = g_strdup(gtk_entry_get_text(GTK_ENTRY(channel_name)));
    JsonNode *detail = media_import(&window, "Ayrılabilir mixer kesintisiz PCM", 997, .25);
    native_load(&window, detail, 0); click(window.root, "dj-channel-1-play");
    RadioSAudioUiSnapshot before = deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
    Capture original = mixer_wait_main_signal(); g_assert_cmpfloat(original.rms[0], >, .05); g_assert_cmpfloat(original.rms[1], >, .05);

    click(window.root, "dj-mixer-window-toggle"); run_for(100);
    GtkWidget *floating = assert_mixer_detached(&window, mixer);
    g_assert_true(find(floating, "dj-channel-1-gain") == gain);
    g_assert_true(find(floating, "dj-channel-1-name") == channel_name);
    g_assert_true(gtk_widget_get_mapped(floating));
    for (guint i = 0; i < 3; ++i) {
        click(window.root, "dj-mixer-window-show");
        g_assert_true(assert_mixer_detached(&window, mixer) == floating);
    }
    range(floating, "dj-channel-1-gain", -6); knob(floating, "dj-channel-1-pan", -100);
    range(floating, "dj-crossfader", .25); entry(floating, "dj-channel-1-name", "Ayrı pencere canlı kanal");
    run_for(150); assert_detached_mixer_controls(&window);
    GtkWidget *output = find(window.root, "deck-0-output-channel"); g_assert_true(GTK_IS_COMBO_BOX_TEXT(output));
    DetachedOutputName expected_name = {output, "Ayrı pencere canlı kanal"};
    wait_until(detached_output_name_ready, &expected_name, "broadcast output selector keeps the detached mixer's custom channel name");
    Capture detached = measure("MAIN"); g_assert_cmpfloat(detached.rms[0], >, .02); g_assert_cmpfloat(detached.peak[1], <, .00001);
    const gchar *profiles[] = {"broadcast", "technical", "playlist", "dj"};
    for (guint i = 0; i < G_N_ELEMENTS(profiles); ++i) {
        g_assert_true(radios_window_select_profile(window.window, profiles[i])); run_for(60);
        g_assert_true(assert_mixer_detached(&window, mixer) == floating);
    }
    capture_mixer_window(window.root, "mixer-detached-main-placeholder.png");
    capture_mixer_window(floating, "mixer-detached-floating-window.png");

    GWeakRef first_float; g_weak_ref_init(&first_float, floating);
    click(window.root, "dj-mixer-window-restore"); run_for(100);
    assert_mixer_redocked(&window, mixer, original_parent); assert_detached_mixer_controls(&window);
    g_assert_null(g_weak_ref_get(&first_float)); g_weak_ref_clear(&first_float);
    g_assert_true(find(window.root, "dj-channel-1-gain") == gain);
    g_assert_cmpfloat_with_epsilon(gtk_range_get_value(GTK_RANGE(gain)), -6, .001);
    Capture restored = measure("MAIN"); g_assert_cmpfloat(restored.rms[0], >, .02); g_assert_cmpfloat(restored.peak[1], <, .00001);
    RadioSAudioUiSnapshot after = deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, before.position_ms + 100);
    g_assert_cmpuint(after.generation, ==, before.generation); g_assert_cmpstr(after.media_id, ==, before.media_id);

    click(window.root, "dj-mixer-window-toggle"); floating = assert_mixer_detached(&window, mixer);
    GWeakRef closed_float; g_weak_ref_init(&closed_float, floating);
    gtk_window_close(GTK_WINDOW(floating)); wait_until(dialog_closed, "dj-mixer-window", "window-manager close restores the same mixer");
    assert_mixer_redocked(&window, mixer, original_parent); assert_detached_mixer_controls(&window);
    g_assert_null(g_weak_ref_get(&closed_float)); g_weak_ref_clear(&closed_float);

    /* Direct GTK destruction also returns the live mixer before the floating
     * window destroys its children; this exercises the destroy signal path. */
    click(window.root, "dj-mixer-window-toggle"); floating = assert_mixer_detached(&window, mixer);
    GWeakRef destroyed_float; g_weak_ref_init(&destroyed_float, floating);
    gtk_widget_destroy(floating); run_for(100);
    assert_mixer_redocked(&window, mixer, original_parent); assert_detached_mixer_controls(&window);
    g_assert_null(g_weak_ref_get(&destroyed_float)); g_weak_ref_clear(&destroyed_float);

    click(window.root, "dj-channel-1-stop"); deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    entry(window.root, "dj-channel-1-name", original_name); g_free(original_name); json_node_free(detail);
    reset_mixer(&window); run_for(100);
    GtkWidget *handle = find(mixer, "dj-mixer-drag-handle"); g_assert_true(gtk_widget_get_mapped(handle));
    g_assert_true(mixer_handle_event(handle, GDK_BUTTON_PRESS, 20, 10));
    mixer_handle_event(handle, GDK_MOTION_NOTIFY, 22, 11);
    g_assert_null(dialog_find("dj-mixer-window")); g_assert_true(find(window.root, "dj-mixer") == mixer);
    g_assert_true(mixer_handle_event(handle, GDK_MOTION_NOTIFY, 50, 25));
    floating = assert_mixer_detached(&window, mixer);
    g_assert_true(mixer_handle_event(handle, GDK_BUTTON_RELEASE, 50, 25));
    GWeakRef final_float, final_mixer;
    g_weak_ref_init(&final_float, floating); g_weak_ref_init(&final_mixer, mixer);
    window_close(&window); run_for(150);
    g_assert_null(g_weak_ref_get(&final_float)); g_assert_null(g_weak_ref_get(&final_mixer));
    g_weak_ref_clear(&final_float); g_weak_ref_clear(&final_mixer);
    g_assert_cmpuint(mixer_named_toplevels("dj-mixer-window", FALSE), ==, 0);
    g_assert_cmpuint(mixer_named_toplevels("dj-mixer", TRUE), ==, 0);
}

typedef struct { GtkWidget *root; const gchar *name, *value; } StripExpectation;
static gboolean combo_has(GtkWidget *widget, const gchar *id)
{
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(widget)); gint column = gtk_combo_box_get_id_column(GTK_COMBO_BOX(widget));
    GtkTreeIter iter; gboolean found = FALSE;
    for (gboolean valid = gtk_tree_model_get_iter_first(model, &iter); valid && !found; valid = gtk_tree_model_iter_next(model, &iter)) {
        gchar *value = NULL; gtk_tree_model_get(model, &iter, column, &value, -1); found = !g_strcmp0(value, id); g_free(value);
    }
    return found;
}
static gboolean strip_combo_active(gpointer value)
{
    StripExpectation *expected = value; GtkWidget *widget = find(expected->root, expected->name);
    return GTK_IS_COMBO_BOX(widget) && !g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(widget)), expected->value);
}
static gboolean strip_combo_offers(gpointer value)
{
    StripExpectation *expected = value; GtkWidget *widget = find(expected->root, expected->name);
    return GTK_IS_COMBO_BOX(widget) && combo_has(widget, expected->value);
}
static gboolean strip_text_starts(gpointer value)
{
    StripExpectation *expected = value; GtkWidget *widget = find(expected->root, expected->name);
    return GTK_IS_LABEL(widget) && g_str_has_prefix(gtk_label_get_text(GTK_LABEL(widget)), expected->value);
}
static gboolean strip_toggle_active(gpointer value)
{
    StripExpectation *expected = value; GtkWidget *widget = find(expected->root, expected->name);
    return GTK_IS_TOGGLE_BUTTON(widget) && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)) == (expected->value != NULL);
}
static void strip_wait(Window *window, Condition condition, const gchar *name, const gchar *value, const gchar *description)
{ StripExpectation expected = {window->root, name, value}; wait_until(condition, &expected, description); }
static const gchar *strip_label(JsonObject *configuration, guint channel)
{ return radios_json_string(channel_config(configuration, channel), "label", NULL); }
static gboolean labels_saved(JsonObject *configuration)
{
    return !g_strcmp0(strip_label(configuration, 0), "dj") && !g_strcmp0(strip_label(configuration, 1), "talk") &&
        !g_strcmp0(strip_label(configuration, 2), "music") && !json_object_has_member(channel_config(configuration, 3), "label");
}
static gboolean third_label_cleared(JsonObject *configuration)
{ return !json_object_has_member(channel_config(configuration, 2), "label") && !g_strcmp0(strip_label(configuration, 0), "dj"); }
/* R5.2: each strip carries a broadcast label (DJ / Konuşma / Müzik) right
 * below its source. It autosaves into dj_mixer channels[i].label (absent for
 * none), follows a database refresh without saving back and restores on
 * reopen; the controller API refuses unknown labels and channels. */
static void test_channel_labels(void)
{
    Window window = window_open(); reset_mixer(&window); radios_window_select_profile(window.window, "dj"); run_for(100);
    g_assert_null(find(window.root, "dj-channel-0-label"));
    GtkWidget *label = find(window.root, "dj-channel-1-label"), *source = find(window.root, "dj-channel-1-source");
    g_assert_true(GTK_IS_COMBO_BOX(label)); g_assert_true(gtk_widget_get_parent(label) == gtk_widget_get_parent(source));
    const gchar *ids[] = {"none", "dj", "talk", "music"};
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) g_assert_true(combo_has(label, ids[i]));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(label)), ==, "none");
    combo(window.root, "dj-channel-1-label", "dj"); combo(window.root, "dj-channel-2-label", "talk"); combo(window.root, "dj-channel-3-label", "music");
    g_assert_cmpstr(radios_audio_ui_get_channel_label(window.ctx, 1), ==, "dj");
    g_assert_cmpstr(radios_audio_ui_get_channel_label(window.ctx, 2), ==, "talk");
    g_assert_cmpstr(radios_audio_ui_get_channel_label(window.ctx, 3), ==, "music");
    g_assert_null(radios_audio_ui_get_channel_label(window.ctx, 4));
    JsonNode *record = document_wait("dj_mixer", "DJ mixer", labels_saved); document_ack_wait(&window, record);
    JsonNode *configuration = deep_copy(json_object_get_member(json_node_get_object(record), "config"));
    json_object_remove_member(channel_config(json_node_get_object(configuration), 1), "label");
    json_object_set_string_member(channel_config(json_node_get_object(configuration), 3), "label", "talk");
    JsonNode *changed = document_remote_save(record, json_node_get_object(configuration));
    gint64 revision = radios_json_int(json_node_get_object(changed), "_rev", 0); g_assert_cmpint(revision, >, 0);
    json_node_free(configuration); json_node_free(changed); json_node_free(record);
    radios_data_refresh(window.ctx);
    strip_wait(&window, strip_combo_active, "dj-channel-2-label", "none", "a remote label removal reaches its strip");
    strip_wait(&window, strip_combo_active, "dj-channel-4-label", "talk", "a remote label reaches its strip");
    run_for(800);
    record = document_read("dj_mixer", "DJ mixer"); g_assert_nonnull(record);
    g_assert_cmpint(radios_json_int(json_node_get_object(record), "_rev", 0), ==, revision); json_node_free(record);
    window_close(&window); window = window_open(); radios_window_select_profile(window.window, "dj");
    strip_wait(&window, strip_combo_active, "dj-channel-1-label", "dj", "DJ label restores on reopen");
    strip_wait(&window, strip_combo_active, "dj-channel-2-label", "none", "cleared label restores on reopen");
    strip_wait(&window, strip_combo_active, "dj-channel-3-label", "music", "Müzik label restores on reopen");
    strip_wait(&window, strip_combo_active, "dj-channel-4-label", "talk", "Konuşma label restores on reopen");
    GError *error = NULL;
    g_assert_false(radios_audio_ui_set_channel_label(window.ctx, 1, "DJ", &error)); g_assert_nonnull(error); g_clear_error(&error);
    g_assert_false(radios_audio_ui_set_channel_label(window.ctx, 0, "dj", &error)); g_assert_nonnull(error); g_clear_error(&error);
    g_assert_false(radios_audio_ui_set_channel_label(window.ctx, 33, "dj", &error)); g_assert_nonnull(error); g_clear_error(&error);
    require_call(radios_audio_ui_set_channel_label(window.ctx, 3, NULL, &error), &error);
    strip_wait(&window, strip_combo_active, "dj-channel-3-label", "none", "an API label change shows on its strip");
    record = document_wait("dj_mixer", "DJ mixer", third_label_cleared); document_ack_wait(&window, record); json_node_free(record);
    window_close(&window);
}
#define DJ_TEST_NETWORK_URL "http://127.0.0.1:9/canli"
static JsonObject *strip_input(JsonObject *configuration, guint channel)
{
    JsonObject *strip = channel_config(configuration, channel);
    return json_object_has_member(strip, "input") ? json_object_get_object_member(strip, "input") : NULL;
}
static gboolean capture_input_saved(JsonObject *configuration)
{
    JsonObject *input = strip_input(configuration, 2); gchar *device = g_strconcat(monitor_sink, ".monitor", NULL);
    gboolean saved = input && !g_strcmp0(radios_json_string(input, "kind", ""), "capture") &&
        !g_strcmp0(radios_json_string(input, "device", ""), device) && *radios_json_string(input, "name", "") &&
        json_object_get_int_member(channel_config(configuration, 2), "source_deck") == -1;
    g_free(device); return saved;
}
static gboolean network_input_saved(JsonObject *configuration)
{
    JsonObject *input = strip_input(configuration, 3);
    JsonArray *sources = json_object_has_member(configuration, "network_sources") ? json_object_get_array_member(configuration, "network_sources") : NULL;
    JsonObject *source = sources && json_array_get_length(sources) == 1 ? json_array_get_object_element(sources, 0) : NULL;
    return input && !g_strcmp0(radios_json_string(input, "kind", ""), "network") &&
        !g_strcmp0(radios_json_string(input, "device", ""), DJ_TEST_NETWORK_URL) && !g_strcmp0(radios_json_string(input, "name", ""), "Yerel yayın") &&
        json_object_get_int_member(channel_config(configuration, 3), "source_deck") == -1 && source &&
        !g_strcmp0(radios_json_string(source, "name", ""), "Yerel yayın") && !g_strcmp0(radios_json_string(source, "url", ""), DJ_TEST_NETWORK_URL);
}
static gboolean capture_input_cleared(JsonObject *configuration)
{ return !strip_input(configuration, 2) && json_object_get_int_member(channel_config(configuration, 2), "source_deck") == 2 && strip_input(configuration, 3); }
typedef struct { RadioSViewContext *ctx; guint channel; } InputExpectation;
static gboolean input_ready(gpointer value)
{
    InputExpectation *expected = value; RadioSAudioChannelInputStatus status;
    return radios_audio_ui_channel_input_status(expected->ctx, expected->channel, &status) && status.state == RADIOS_CHANNEL_INPUT_READY;
}
/* R5.1: after the players, the source menu offers the engine's sound-card
 * inputs and the saved network sources, plus "Ağ kaynağı…" for a small window
 * that saves a new one. A live input feeds the strip through the engine line
 * (source_deck -1), shows a short status line, persists in dj_mixer and
 * restores on reopen; choosing a player ends it. */
static void test_channel_live_inputs(void)
{
    Window window = window_open(); reset_mixer(&window); radios_window_select_profile(window.window, "dj"); run_for(100);
    gchar *capture_id = g_strconcat("in:", monitor_sink, ".monitor", NULL);
    strip_wait(&window, strip_combo_offers, "dj-channel-3-source", capture_id, "engine sound-card inputs fill the source menu");
    GtkWidget *source = find(window.root, "dj-channel-3-source");
    const gchar *kept[] = {"-1", "-2", "0", "31", "net-new"};
    for (guint i = 0; i < G_N_ELEMENTS(kept); ++i) g_assert_true(combo_has(source, kept[i]));
    combo(window.root, "dj-channel-3-source", capture_id);
    JsonNode *record = document_wait("dj_mixer", "DJ mixer", capture_input_saved); document_ack_wait(&window, record); json_node_free(record);
    RadioSAudioMixerConfig mixer; g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, NULL, NULL));
    g_assert_cmpint(mixer.channels[2].source_deck, ==, -1);
    RadioSAudioChannelInputSpec spec; g_assert_true(radios_audio_ui_get_channel_input(window.ctx, 3, &spec));
    g_assert_cmpint(spec.kind, ==, RADIOS_CHANNEL_INPUT_CAPTURE);
    InputExpectation capture_ready = {window.ctx, 3}; wait_until(input_ready, &capture_ready, "capture input connects to the virtual source");
    gchar *status = g_strdup_printf("Giriş · %s · Hazır", spec.name);
    strip_wait(&window, strip_text_starts, "dj-channel-3-position", status, "capture status line"); g_free(status);
    g_assert_false(gtk_widget_get_sensitive(find(window.root, "dj-channel-3-load")));
    combo(window.root, "dj-channel-4-source", "net-new");
    wait_until(dialog_exists, "dj-network-source-dialog", "network source window opens");
    GtkWidget *dialog = dialog_find("dj-network-source-dialog");
    entry(dialog, "dj-network-source-name", "Yerel yayın"); entry(dialog, "dj-network-source-url", "file:///etc/passwd");
    click(dialog, "dj-network-source-accept"); run_for(50);
    g_assert_true(dialog_find("dj-network-source-dialog") == dialog);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(dialog, "dj-network-source-error"))), !=, "");
    g_assert_cmpuint(radios_audio_ui_network_sources(window.ctx, NULL, 0), ==, 0);
    entry(dialog, "dj-network-source-url", DJ_TEST_NETWORK_URL); click(dialog, "dj-network-source-accept");
    wait_until(dialog_closed, "dj-network-source-dialog", "accepted network source closes its window");
    record = document_wait("dj_mixer", "DJ mixer", network_input_saved); document_ack_wait(&window, record); json_node_free(record);
    strip_wait(&window, strip_combo_offers, "dj-channel-1-source", "net:" DJ_TEST_NETWORK_URL, "a saved network source is offered by every strip");
    strip_wait(&window, strip_text_starts, "dj-channel-4-position", "Ağ · Yerel yayın · ", "network status line");
    combo(window.root, "dj-channel-5-source", "net-new");
    wait_until(dialog_exists, "dj-network-source-dialog", "network source window opens again");
    gtk_dialog_response(GTK_DIALOG(dialog_find("dj-network-source-dialog")), GTK_RESPONSE_CANCEL);
    wait_until(dialog_closed, "dj-network-source-dialog", "cancelled network window closes");
    strip_wait(&window, strip_combo_active, "dj-channel-5-source", "4", "a cancelled window keeps the strip's player");
    g_assert_cmpuint(radios_audio_ui_network_sources(window.ctx, NULL, 0), ==, 1);
    g_assert_false(radios_audio_ui_get_channel_input(window.ctx, 5, NULL));
    window_close(&window); window = window_open(); radios_window_select_profile(window.window, "dj");
    strip_wait(&window, strip_combo_active, "dj-channel-3-source", capture_id, "capture input restores on reopen");
    strip_wait(&window, strip_combo_active, "dj-channel-4-source", "net:" DJ_TEST_NETWORK_URL, "network input restores on reopen");
    combo(window.root, "dj-channel-3-source", "2");
    record = document_wait("dj_mixer", "DJ mixer", capture_input_cleared); document_ack_wait(&window, record); json_node_free(record);
    g_assert_false(radios_audio_ui_get_channel_input(window.ctx, 3, NULL));
    window_close(&window); g_free(capture_id);
}
static gboolean manual_gain_saved(JsonObject *configuration)
{
    JsonObject *strip = channel_config(configuration, 2);
    return fabs(json_object_get_double_member(strip, "gain_db") + 12) < 0.001 && !json_object_get_boolean_member(strip, "mute");
}
static gboolean off_air_saved(JsonObject *configuration)
{
    JsonObject *strip = channel_config(configuration, 2);
    return fabs(json_object_get_double_member(strip, "gain_db") + 12) < 0.001 && json_object_get_boolean_member(strip, "mute");
}
/* R5.3: putting a channel on or off air is the strip's mute only. The change
 * shows on the strip, autosaves, keeps the manual fader and moves no deck. */
static void test_channel_on_air_strip(void)
{
    Window window = window_open(); reset_mixer(&window); radios_window_select_profile(window.window, "dj"); run_for(100);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    RadioSAudioMixerConfig manual; radios_audio_engine_get_manual_mixer(engine, &manual); manual.channels[2].gain_db = -12;
    GError *error = NULL; require_call(radios_audio_ui_set_mixer(window.ctx, &manual, &error), &error);
    JsonNode *record = document_wait("dj_mixer", "DJ mixer", manual_gain_saved); document_ack_wait(&window, record); json_node_free(record);
    g_assert_true(radios_audio_ui_channel_on_air(window.ctx, 3));
    require_call(radios_audio_ui_set_channel_on_air(window.ctx, 3, FALSE, &error), &error);
    g_assert_false(radios_audio_ui_channel_on_air(window.ctx, 3));
    strip_wait(&window, strip_toggle_active, "dj-channel-3-mute", "", "an engine-side mute shows on its strip");
    record = document_wait("dj_mixer", "DJ mixer", off_air_saved); document_ack_wait(&window, record); json_node_free(record);
    radios_audio_engine_get_manual_mixer(engine, &manual); g_assert_cmpfloat(fabs(manual.channels[2].gain_db + 12), <, .001);
    require_call(radios_audio_ui_set_channel_on_air(window.ctx, 3, TRUE, &error), &error);
    require_call(radios_audio_ui_set_channel_on_air(window.ctx, 3, TRUE, &error), &error);
    g_assert_true(radios_audio_ui_channel_on_air(window.ctx, 3));
    strip_wait(&window, strip_toggle_active, "dj-channel-3-mute", NULL, "putting the channel on air clears its mute");
    record = document_wait("dj_mixer", "DJ mixer", manual_gain_saved); document_ack_wait(&window, record); json_node_free(record);
    radios_audio_engine_get_manual_mixer(engine, &manual); g_assert_cmpfloat(fabs(manual.channels[2].gain_db + 12), <, .001);
    g_assert_false(radios_audio_ui_set_channel_on_air(window.ctx, 0, TRUE, &error)); g_assert_nonnull(error); g_clear_error(&error);
    g_assert_false(radios_audio_ui_set_channel_on_air(window.ctx, 33, TRUE, &error)); g_assert_nonnull(error); g_clear_error(&error);
    g_assert_false(radios_audio_ui_channel_on_air(window.ctx, 0));
    RadioSAudioUiSnapshot snapshot;
    for (guint deck = 0; deck < RADIOS_AUDIO_DECKS; ++deck) {
        g_assert_true(radios_audio_ui_get_snapshot(window.ctx, deck, &snapshot)); g_assert_cmpint(snapshot.state, ==, RADIOS_AUDIO_UI_EMPTY);
    }
    radios_audio_engine_unref(engine); window_close(&window);
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    main_sink = g_getenv("RADIOS_TEST_MAIN_SINK"); cue_sink = g_getenv("RADIOS_TEST_CUE_SINK"); monitor_sink = g_getenv("RADIOS_TEST_MONITOR_SINK");
    g_assert_nonnull(main_sink); g_assert_nonnull(cue_sink); g_assert_nonnull(monitor_sink);
    g_assert_true(g_str_has_prefix(main_sink, "radios_gui_")); g_assert_true(g_str_has_prefix(cue_sink, "radios_gui_"));
    GError *error = NULL; g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    application = gtk_application_new("org.radios.NativeDjAcceptance", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/native-dj/mixer-detach-reparent-live-controls-close-shutdown", test_mixer_detach_lifecycle);
    g_test_add_func("/native-dj/configuration-CAS-reopen", test_persistence);
    g_test_add_func("/native-dj/stale-CAS-preserves-controls", test_cas_preserves_input);
    g_test_add_func("/native-dj/channel-names-autosave-reload-reopen", test_channel_names_autosave);
    g_test_add_func("/native-dj/autosave-latest-immediate-close", test_autosave_latest_immediate_close);
    g_test_add_func("/native-dj/cold-load-preserves-saved-mixer", test_cold_load_preserves_saved_mixer);
    g_test_add_func("/native-dj/PCM-pan-gain-mute-PFL-crossfade", test_real_pcm_controls);
    g_test_add_func("/native-dj/verified-sampler-retrigger-owner", test_verified_sampler);
    g_test_add_func("/native-dj/transition-pair-PCM-persistence-owner", test_transition_pair);
    g_test_add_func("/native-dj/channel-routing-squares-ON-AIR", test_channel_routing_and_on_air);
    g_test_add_func("/native-dj/master-channel-routing-PCM-persistence-cleanup", test_master_channel_routing);
    g_test_add_func("/native-dj/four-pedals-legacy-fifth-migration", test_four_pedals_legacy_migration);
    g_test_add_func("/native-dj/push-to-talk-PCM-keyboard-loss-lifecycle", test_push_to_talk_lifecycle);
    g_test_add_func("/native-dj/speech-effector-route", test_speech_effector_route);
    g_test_add_func("/native-dj/speech-master-channel-route", test_speech_master_channel_route);
    g_test_add_func("/native-dj/channel-labels-autosave-refresh-reopen", test_channel_labels);
    g_test_add_func("/native-dj/channel-live-inputs-network-window-reopen", test_channel_live_inputs);
    g_test_add_func("/native-dj/channel-on-air-mute-strip-keeps-fader", test_channel_on_air_strip);
    gint result = g_test_run(); radios_data_client_unref(reader); g_object_unref(application); radios_config_clear(&config); return result;
}
