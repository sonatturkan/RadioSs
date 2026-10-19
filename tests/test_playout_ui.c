#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include "radios/window.h"
#include "radios/audio_ui.h"
#include "radios/audio.h"
#include "radios/audio_runner.h"
#include "radios/player_workspace.h"
#include "radios/player_scope_execution.h"
#include "radios/player_tree_ui.h"
#include "radios/media_jobs.h"
#include "radios/media.h"
#include "radios/media_ui.h"
#include "radios/editors.h"
#include "radios/programme_queue.h"
#include "radios/playout_ui.h"
#include "radios/automation_calendar.h"
#include "radios/player_rotation.h"
#include "radios/neon_progress.h"
#include "radios/weekly_programme_plan.h"
#include <glib/gstdio.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
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
    /* Menu entries live in the popover of their menu button. */
    GtkPopover *menu = GTK_IS_MENU_BUTTON(root) ? gtk_menu_button_get_popover(GTK_MENU_BUTTON(root)) : NULL;
    GtkWidget *entry = menu ? find(GTK_WIDGET(menu), name) : NULL;
    if (entry) return entry;
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
    if (!condition(data)) g_error("Native playout acceptance timed out: %s", description);
}
typedef struct { GtkWidget *title, *metadata, *next; const gchar *expected_title, *expected_metadata, *expected_next; } CardMetadata;
static gboolean card_metadata_matches(gpointer data)
{
    CardMetadata *card = data;
    return g_str_equal(gtk_label_get_text(GTK_LABEL(card->title)), card->expected_title) &&
        strstr(gtk_label_get_text(GTK_LABEL(card->metadata)), card->expected_metadata) &&
        g_str_equal(gtk_label_get_text(GTK_LABEL(card->next)), card->expected_next);
}
static void card_metadata_wait(Window *window, guint deck, const gchar *title, const gchar *metadata, const gchar *next)
{
    gchar *name = g_strdup_printf("deck-%u-title", deck); GtkWidget *heading = find(window->root, name); g_free(name);
    name = g_strdup_printf("deck-%u-metadata", deck); GtkWidget *information = find(window->root, name); g_free(name);
    name = g_strdup_printf("deck-%u-next-title", deck); GtkWidget *following = find(window->root, name); g_free(name);
    g_assert_true(GTK_IS_LABEL(heading)); g_assert_true(GTK_IS_LABEL(information)); g_assert_true(GTK_IS_LABEL(following));
    CardMetadata expected = {heading, information, following, title, metadata, next};
    gint64 deadline = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND;
    while (!card_metadata_matches(&expected) && g_get_monotonic_time() < deadline) pump();
    if (!card_metadata_matches(&expected)) g_error("Deck %u metadata expected '%s' / '%s' / '%s'; actual '%s' / '%s' / '%s'",
        deck, title, metadata, next, gtk_label_get_text(GTK_LABEL(heading)), gtk_label_get_text(GTK_LABEL(information)), gtk_label_get_text(GTK_LABEL(following)));
    if (g_str_equal(title, "—")) { g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(information)), ==, ""); g_assert_true(gtk_widget_get_visible(information)); }
    else {
        g_assert_true(gtk_widget_get_visible(information)); gchar *tooltip = gtk_widget_get_tooltip_text(heading);
        g_assert_cmpstr(tooltip, ==, title); g_free(tooltip);
    }
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
static Window window_open_raw(void)
{
    Window window = {0}; GError *error = NULL; window.runtime = radios_runtime_new(64, 128);
    window.window = radios_window_new(application, &config, window.runtime, FALSE, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(window.window);
    window.ctx = radios_window_view_context(window.window); g_assert_nonnull(window.ctx); g_assert_nonnull(window.ctx->audio); g_assert_nonnull(window.ctx->playout);
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
static gboolean player_channels_constructed(gpointer value)
{
    Window *window = value;
    for (guint deck = 0; deck < 6; ++deck) {
        gchar *name = g_strdup_printf("deck-%u-output-channel", deck);
        GtkWidget *channel = find(window->root, name); g_free(name);
        if (!GTK_IS_COMBO_BOX(channel)) return FALSE;
    }
    return TRUE;
}
/* Retained PCM cases model the operator's explicit channel assignment.
 * Raw construction and persistence cases bypass this helper deliberately. */
static Window window_open(void)
{
    Window window = window_open_raw();
    wait_until(player_channels_constructed, &window, "saved player definitions reconciled into physical card controls");
    for (guint deck = 0; deck < 6; ++deck) {
        gchar *name = g_strdup_printf("deck-%u-output-channel", deck), *id = g_strdup_printf("%u", deck);
        GtkWidget *output = find(window.root, name); g_assert_true(GTK_IS_COMBO_BOX(output));
        wait_until(widget_enabled, output, "native channel selector ready after saved mixer baseline");
        combo(window.root, name, "-1"); combo(window.root, name, id); g_free(name); g_free(id);
    }
    run_for(400); return window;
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
    Request task = {reader, path, body, NULL, NULL, 0};
    /* The window's own background saves may advance the database between a
     * fixture workflow's read and its sequence-fenced commit. That is the
     * server's "reload and retry" conflict; the same idempotent body is resent. */
    for (guint attempt = 0; attempt < 4; ++attempt) {
        g_clear_error(&task.error); task.done = 0;
        GThread *thread = g_thread_new("playout-DB-read", request_work, &task);
        wait_until(request_done, &task, "authenticated DB readback"); g_thread_join(thread);
        if (!task.error || !g_error_matches(task.error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_CONFLICT) ||
            !strstr(task.error->message, "database changed since preview")) break;
        run_for(150);
    }
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
typedef struct { const gchar *path; RadioSMediaAnalysis *analysis; GError *error; gint done; } LegacyAnalysis;
static gpointer legacy_analysis_work(gpointer value)
{
    LegacyAnalysis *task = value;
    task->analysis = radios_media_stage_analyze(&config, task->path, NULL, NULL, &task->error);
    g_atomic_int_set(&task->done, 1); return NULL;
}
static gboolean legacy_analysis_done(gpointer value)
{ return g_atomic_int_get(&((LegacyAnalysis *)value)->done) != 0; }
/* The import contract accepts old measured waveforms without the newer rhythm
 * member. Build that genuine DB fixture through its authenticated HTTP API;
 * loading it must repair the missing rhythm without a manual Analyze action. */
static JsonNode *legacy_media_import(const gchar *path, const gchar *title, const gchar *kind)
{
    LegacyAnalysis task = {path, NULL, NULL, 0};
    GThread *thread = g_thread_new("legacy-import-analysis", legacy_analysis_work, &task);
    wait_until(legacy_analysis_done, &task, "legacy fixture measures actual staged PCM"); g_thread_join(thread);
    g_assert_no_error(task.error); g_assert_nonnull(task.analysis);
    JsonNode *analysis = radios_media_analysis_to_json(task.analysis);
    json_object_remove_member(json_node_get_object(analysis), "rhythm");
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body), *metadata = json_object_new();
    gchar *filename = g_path_get_basename(task.analysis->path), *key = g_uuid_string_random();
    json_object_set_string_member(object, "import_path", filename);
    json_object_set_string_member(object, "source_name", task.analysis->source_name);
    json_object_set_string_member(object, "idempotency_key", key);
    json_object_set_string_member(metadata, "title", title); json_object_set_string_member(metadata, "kind", kind);
    json_object_set_object_member(object, "metadata", metadata); json_object_set_member(object, "analysis", analysis);
    JsonNode *result = request("/v1/workflows/desktop-media/import", body);
    g_assert_cmpint(g_unlink(task.analysis->path), ==, 0);
    json_node_free(body); radios_media_analysis_free(task.analysis); g_free(filename); g_free(key); return result;
}
static JsonNode *media_import_rhythm_format(Window *window, const gchar *title, gdouble frequency, gdouble amplitude, const gchar *kind, gdouble bpm, gboolean legacy)
{
    guint seconds = bpm ? 12 : 6;
    guint frames = 48000 * seconds + 31, bytes = frames * 4; guchar *wav = g_malloc(44 + bytes);
    memcpy(wav, "RIFF", 4); write_u32(wav + 4, 36 + bytes); memcpy(wav + 8, "WAVEfmt ", 8); write_u32(wav + 16, 16);
    write_u16(wav + 20, 1); write_u16(wav + 22, 2); write_u32(wav + 24, 48000); write_u32(wav + 28, 48000 * 4);
    write_u16(wav + 32, 4); write_u16(wav + 34, 16); memcpy(wav + 36, "data", 4); write_u32(wav + 40, bytes);
    for (guint i = 0; i < frames; ++i) {
        gdouble position = i / 48000.0 - 0.125, phase = bpm && position >= 0 ? fmod(position, 60 / bpm) : 1;
        gdouble envelope = bpm ? 0.4 + (phase < 0.015 ? 0.6 * exp(-phase * 200) : 0) : 1;
        gint16 sample = (gint16)lrint(sin(2 * G_PI * frequency * i / 48000) * amplitude * envelope * 32767);
        write_u16(wav + 44 + i * 4, (guint16)sample); write_u16(wav + 46 + i * 4, (guint16)sample);
    }
    gchar *uuid = g_uuid_string_random(), *name = g_strconcat(uuid, ".wav", NULL), *path = g_build_filename(config.project_root, name, NULL);
    g_assert_true(g_file_set_contents(path, (const gchar *)wav, 44 + bytes, NULL)); g_free(wav); g_free(uuid); g_free(name);
    Import output = {0}; GCancellable *handle = NULL;
    if (legacy) output.result = legacy_media_import(path, title, kind);
    else {
        handle = radios_media_import_async(window->ctx, window->root, path,
            kind, title, import_complete, &output, import_destroy);
        g_assert_nonnull(handle); wait_until(import_done, &output, "real SHA-bound PCM analysis/import");
        g_assert_no_error(output.error); g_assert_nonnull(output.result); g_assert_cmpuint(output.callbacks, ==, 1);
    }
    JsonObject *media = json_object_get_object_member(json_node_get_object(output.result), "media");
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "media_id", radios_json_string(media, "_id", ""));
    JsonNode *detail = request("/v1/workflows/desktop-media/get", body); json_node_free(body); json_node_free(output.result);
    JsonObject *version = json_object_get_object_member(json_node_get_object(detail), "version");
    g_assert_true(json_object_get_boolean_member(version, "verified")); g_assert_true(json_object_get_boolean_member(version, "object_available"));
    g_assert_cmpint(radios_json_int(version, "duration_ms", 0), ==, seconds * 1000 + 1);
    g_assert_cmpstr(radios_json_string(version, "status", ""), ==, "draft");
    g_assert_true(g_file_test(radios_json_string(version, "managed_path", ""), G_FILE_TEST_IS_REGULAR));
    g_assert_cmpint(g_unlink(path), ==, 0); g_free(path); g_clear_object(&handle); return detail;
}
static JsonNode *media_import_rhythm(Window *window, const gchar *title, gdouble frequency, gdouble amplitude, const gchar *kind, gdouble bpm)
{ return media_import_rhythm_format(window, title, frequency, amplitude, kind, bpm, FALSE); }
static JsonNode *media_import(Window *window, const gchar *title, gdouble frequency, gdouble amplitude, const gchar *kind)
{ return media_import_rhythm(window, title, frequency, amplitude, kind, 0); }

static JsonNode *media_metadata_save(JsonNode *detail, const gchar *artist, const gchar *album)
{
    JsonObject *media = json_object_get_object_member(json_node_get_object(detail), "media"), *metadata = json_object_new();
    const gchar *keys[] = {"title", "kind", "artist", "album", "genre", "tags", "year", "comment"};
    for (guint index = 0; index < G_N_ELEMENTS(keys); ++index)
        json_object_set_member(metadata, keys[index], json_node_copy(json_object_get_member(media, keys[index])));
    json_object_set_string_member(metadata, "artist", artist); json_object_set_string_member(metadata, "album", album);
    JsonNode *body = body_new(); JsonObject *request_body = json_node_get_object(body);
    json_object_set_object_member(request_body, "metadata", metadata);
    json_object_set_string_member(request_body, "media_id", radios_json_string(media, "_id", ""));
    json_object_set_int_member(request_body, "expected_revision", radios_json_int(media, "_rev", 0));
    gchar *key = g_uuid_string_random(); json_object_set_string_member(request_body, "idempotency_key", key); g_free(key);
    JsonNode *updated = request("/v1/workflows/desktop-media/metadata-save", body); json_node_free(body); json_node_free(detail); return updated;
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
static guint64 capture_signal_span(const gchar *bus, goffset offset, guint64 *longest_gap)
{
    gchar *key = g_strdup_printf("RADIOS_TEST_%s_CAPTURE", bus); const gchar *path = g_getenv(key);
    g_assert_nonnull(path); gint fd = open(path, O_RDONLY); g_assert_cmpint(fd, >=, 0); g_free(key);
    struct stat status; g_assert_cmpint(fstat(fd, &status), ==, 0); gfloat buffer[4096];
    guint64 frame = 0, first = G_MAXUINT64, last = 0, silence = 0, gap = 0;
    while (offset + 8 <= status.st_size) {
        ssize_t bytes = pread(fd, buffer, (size_t)MIN((goffset)sizeof buffer, status.st_size - offset), offset);
        g_assert_cmpint(bytes, >, 0); g_assert_cmpint(bytes % 8, ==, 0);
        for (guint i = 0; i < (guint)bytes / 8; ++i, ++frame) {
            g_assert_true(isfinite(buffer[i * 2])); g_assert_true(isfinite(buffer[i * 2 + 1]));
            if (fabs(buffer[i * 2]) > 0.001 || fabs(buffer[i * 2 + 1]) > 0.001) {
                if (first == G_MAXUINT64) first = frame;
                else gap = MAX(gap, silence);
                silence = 0;
                last = frame;
            } else if (first != G_MAXUINT64) ++silence;
        }
        offset += bytes;
    }
    close(fd); if (longest_gap) *longest_gap = gap;
    return first == G_MAXUINT64 ? 0 : last - first + 1;
}

static void require_call(gboolean result, GError **error)
{ g_assert_no_error(*error); g_assert_true(result); }
static void set(GtkWidget *root, const gchar *name, const gchar *text)
{ GtkWidget *entry = find(root, name); g_assert_true(GTK_IS_ENTRY(entry)); gtk_entry_set_text(GTK_ENTRY(entry), text); }
static void spin(GtkWidget *root, const gchar *name, gdouble value)
{ GtkWidget *widget = find(root, name); g_assert_true(GTK_IS_SPIN_BUTTON(widget)); gtk_spin_button_set_value(GTK_SPIN_BUTTON(widget), value); }
static void combo_index(GtkWidget *root, const gchar *name, gint index)
{ GtkWidget *widget = find(root, name); g_assert_true(GTK_IS_COMBO_BOX(widget)); gtk_combo_box_set_active(GTK_COMBO_BOX(widget), index); }
static void navigate(Window *window, const gchar *profile)
{ gchar *id = g_strdup_printf("profile-%s-tab", profile); click(window->root, id); g_free(id); run_for(20); }
static GtkWidget *find_label(GtkWidget *root, const gchar *text)
{
    if (GTK_IS_LABEL(root) && !g_strcmp0(gtk_label_get_text(GTK_LABEL(root)), text)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = find_label(item->data, text);
    g_list_free(children); return found;
}
static GtkWidget *find_labeled_button(GtkWidget *root, const gchar *text)
{
    if (GTK_IS_BUTTON(root) && find_label(root, text)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = find_labeled_button(item->data, text);
    g_list_free(children); return found;
}
static void click_label(GtkWidget *root, const gchar *text)
{
    GtkWidget *widget = find_labeled_button(root, text);
    if (!widget) g_error("Native labeled control is absent: %s", text);
    g_assert_true(GTK_IS_BUTTON(widget)); wait_until(widget_enabled, widget, "native labeled button ready"); gtk_button_clicked(GTK_BUTTON(widget));
}
/* With the player workspace the broadcast toolbar has no legacy
 * "+ Oynatıcı ekle"; players are added in the player tree, into the selected
 * folder. Kümesizler receives an independent player in the first free slot. */
static void add_independent_player(GtkWidget *root)
{
    GtkWidget *tree = find(root, "live-player-definitions"); g_assert_nonnull(tree);
    RadioSAutomationScope independent = {.kind = RADIOS_AUTOMATION_SCOPE_UNCLUSTERED};
    g_assert_true(radios_player_tree_ui_select(tree, &independent)); run_for(50);
    click(tree, "player-tree-add-player");
}
typedef struct { GtkWidget *parent; guint deck; } DeckCard;
static gboolean deck_card_present(gpointer value)
{
    DeckCard *card = value; gchar *key = g_strdup_printf("radios-deck-owner-%u", card->deck);
    gboolean present = g_object_get_data(G_OBJECT(card->parent), key) != NULL; g_free(key); return present;
}
static JsonNode *document_read(const gchar *kind, const gchar *name)
{
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "kind", kind);
    JsonNode *result = request("/v1/workflows/desktop/list", body); json_node_free(body);
    JsonArray *records = json_object_get_array_member(json_node_get_object(result), "records"); JsonNode *found = NULL;
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        JsonObject *row = json_array_get_object_element(records, i);
        if (!g_strcmp0(radios_json_string(row, "name", ""), name)) { found = deep_copy(json_array_get_element(records, i)); break; }
    }
    json_node_free(result); return found;
}
static JsonNode *document_save(const gchar *kind, const gchar *name, JsonObject *configuration)
{
    JsonNode *existing = document_read(kind, name), *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", kind); json_object_set_string_member(object, "name", name);
    json_object_set_object_member(object, "config", json_object_ref(configuration));
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    if (existing) {
        JsonObject *record = json_node_get_object(existing);
        json_object_set_string_member(object, "document_id", radios_json_string(record, "_id", ""));
        json_object_set_int_member(object, "expected_revision", radios_json_int(record, "_rev", 0));
    }
    JsonNode *saved = request("/v1/workflows/desktop/put", body);
    json_node_free(body); g_clear_pointer(&existing, json_node_free); return saved;
}
static void bind(JsonObject *object, JsonObject *detail)
{
    JsonObject *media = json_object_get_object_member(detail, "media"), *version = json_object_get_object_member(detail, "version");
    json_object_set_string_member(object, "media_id", radios_json_string(media, "_id", ""));
    json_object_set_string_member(object, "version_id", radios_json_string(version, "_id", ""));
    json_object_set_string_member(object, "media_file_id", radios_json_string(version, "media_file_id", ""));
    json_object_set_string_member(object, "media_sha256", radios_json_string(version, "sha256", ""));
}
static JsonNode *playlist_create(const gchar *name, JsonNode *first, JsonNode *second)
{
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "name", name); gchar *key = g_uuid_string_random();
    json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    JsonNode *playlist = request("/v1/workflows/desktop-playlists/create", body); json_node_free(body);
    JsonNode *details[] = {first, second};
    for (guint i = 0; i < G_N_ELEMENTS(details); ++i) {
        if (!details[i]) continue;
        JsonObject *row = json_node_get_object(playlist), *detail = json_node_get_object(details[i]);
        JsonObject *media = json_object_get_object_member(detail, "media"), *version = json_object_get_object_member(detail, "version");
        body = body_new(); object = json_node_get_object(body);
        json_object_set_string_member(object, "playlist_id", radios_json_string(row, "_id", ""));
        json_object_set_int_member(object, "expected_revision", radios_json_int(row, "_rev", 0));
        JsonObject *metadata = json_object_new();
        const gchar *names[] = {"title", "artist", "album", "genre"};
        for (guint j = 0; j < G_N_ELEMENTS(names); ++j) json_object_set_string_member(metadata, names[j], radios_json_string(media, names[j], ""));
        json_object_set_string_member(metadata, "key", ""); json_object_set_string_member(metadata, "location", "/not-trusted.wav");
        json_object_set_string_member(metadata, "comment", "Genuine isolated verified media binding");
        json_object_set_int_member(metadata, "duration_ms", radios_json_int(version, "duration_ms", 0)); json_object_set_double_member(metadata, "bpm", 120 + i * 10);
        bind(metadata, detail); json_object_set_object_member(object, "metadata", metadata);
        key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
        JsonNode *next = request("/v1/workflows/desktop-playlists/entry-save", body); json_node_free(body); json_node_free(playlist); playlist = next;
    }
    return playlist;
}
static JsonNode *programme_config(JsonNode *first, JsonNode *second)
{
    JsonNode *result = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new(); json_node_take_object(result, object);
    GDateTime *now = g_date_time_new_now_utc(); gchar *date = g_date_time_format(now, "%Y-%m-%d"); g_date_time_unref(now);
    json_object_set_string_member(object, "title", "Yerel native program"); json_object_set_string_member(object, "date", date); g_free(date);
    json_object_set_string_member(object, "note", "Native verified queue, no on-air claim");
    JsonArray *events = json_array_new(); JsonNode *details[] = {first, second};
    for (guint i = 0; i < G_N_ELEMENTS(details); ++i) {
        if (!details[i]) continue;
        JsonObject *detail = json_node_get_object(details[i]), *media = json_object_get_object_member(detail, "media");
        JsonObject *version = json_object_get_object_member(detail, "version"), *event = json_object_new(); gchar *id = g_uuid_string_random();
        json_object_set_string_member(event, "id", id); g_free(id);
        const gchar *keys[] = {"title", "artist", "album", "kind"};
        for (guint j = 0; j < G_N_ELEMENTS(keys); ++j) json_object_set_string_member(event, keys[j], radios_json_string(media, keys[j], ""));
        json_object_set_string_member(event, "start_time", ""); json_object_set_int_member(event, "duration_ms", radios_json_int(version, "duration_ms", 0));
        json_object_set_string_member(event, "version_label", "Yerel taslak"); json_object_set_string_member(event, "primary_file", "");
        json_object_set_string_member(event, "backup_file", ""); json_object_set_string_member(event, "note", ""); bind(event, detail);
        json_array_add_object_element(events, event);
    }
    json_object_set_array_member(object, "events", events); return result;
}
typedef struct { Window *window; RadioSPlayoutState state; guint index; guint64 position; } RunCondition;
static gboolean run_matches(gpointer data)
{
    RunCondition *condition = data; RadioSPlayoutSnapshot snapshot = {0};
    return radios_playout_ui_get_snapshot(condition->window->ctx, &snapshot) && snapshot.state == condition->state &&
        snapshot.index == condition->index && snapshot.position_ms >= condition->position;
}
static RadioSPlayoutSnapshot run_wait(Window *window, RadioSPlayoutState state, guint index, guint64 position)
{
    RunCondition condition = {window, state, index, position}; gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!run_matches(&condition) && g_get_monotonic_time() < deadline) pump();
    RadioSPlayoutSnapshot snapshot = {0}; g_assert_true(radios_playout_ui_get_snapshot(window->ctx, &snapshot));
    if (!run_matches(&condition)) g_error("Playout expected state%d index%u cursor>=%" G_GUINT64_FORMAT "; actual state%d index%u/%u cursor%" G_GUINT64_FORMAT ": %s",
        state, index, position, snapshot.state, snapshot.index, snapshot.total, snapshot.position_ms, snapshot.error);
    return snapshot;
}
/* Named rotations, assigned schemas and a workspace player's card AutoDJ
 * (Başlat with an ordinary playlist) execute in the source player's
 * controller. The general programme remains independent throughout. */
static RadioSPlayerScopeExecutionSnapshot rotation_execution_snapshot(Window *window)
{
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0};
    RadioSPlayerScopeExecutionSnapshot snapshot = {0};
    g_assert_true(radios_player_scope_execution_snapshot(radios_player_scope_execution_get(window->ctx), &scope, &snapshot));
    return snapshot;
}
static guint64 rotation_intent(Window *window)
{
    RadioSViewContext *ctx = radios_player_workspace_source_context(window->ctx->players, 0);
    return ctx ? radios_playout_ui_intent(ctx) : 0;
}
static RadioSPlayoutSnapshot rotation_run_wait(Window *window, RadioSPlayoutState state, guint index, guint64 position)
{
    RadioSPlayoutSnapshot snapshot = {0}; gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    do {
        if (radios_player_workspace_snapshot(window->ctx->players, 0, &snapshot) && snapshot.state == state &&
            snapshot.index == index && snapshot.position_ms >= position) return snapshot;
        pump();
    } while (g_get_monotonic_time() < deadline);
    RadioSPlayerScopeExecutionSnapshot execution = rotation_execution_snapshot(window);
    g_error("Scoped rotation expected state%d index%u cursor>=%" G_GUINT64_FORMAT "; actual state%d index%u/%u cursor%" G_GUINT64_FORMAT ": %s; execution%d: %s",
        state, index, position, snapshot.state, snapshot.index, snapshot.total, snapshot.position_ms, snapshot.error, execution.state, execution.error);
    return snapshot;
}
static void rotation_stop_run(Window *window)
{
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0}; GError *error = NULL;
    require_call(radios_player_scope_execution_action(radios_player_scope_execution_get(window->ctx), &scope, RADIOS_PLAYOUT_STOP, &error), &error);
    run_for(250);
}
static gboolean scoped_owns_deck(Window *window, guint deck)
{ return radios_player_workspace_running_context(window->ctx->players, deck) != NULL; }
static void stop_run(Window *window)
{
    GError *error = NULL; require_call(radios_playout_ui_action(window->ctx, RADIOS_PLAYOUT_STOP, &error), &error);
    run_for(250);
}
/* The flow lists events; the header counts contents ("N event · M içerik"). */
static gint queue_contents(Window *window)
{
    /* Parsed byte-wise: sscanf stops at "·" in the tr_TR locale. */
    GtkWidget *count = find(window->root, "programme-queue-count"); if (!count) return -1;
    const gchar *separator = strstr(gtk_label_get_text(GTK_LABEL(count)), " · "); gchar *end = NULL;
    guint64 contents = separator ? g_ascii_strtoull(separator + strlen(" · "), &end, 10) : 0;
    return separator && end != separator + strlen(" · ") ? (gint)contents : -1;
}
static gboolean label_present(GtkWidget *root, const gchar *text)
{
    if (GTK_IS_LABEL(root) && !g_strcmp0(gtk_label_get_text(GTK_LABEL(root)), text)) return TRUE;
    if (!GTK_IS_CONTAINER(root)) return FALSE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); gboolean found = FALSE;
    for (GList *item = children; item && !found; item = item->next) found = label_present(item->data, text);
    g_list_free(children); return found;
}
/* Opens each event in turn (its contents replace the overview) and looks for title. */
static gboolean queue_shows(Window *window, const gchar *title)
{
    GtkWidget *table = find(window->root, "programme-queue-table"); if (!table) return FALSE;
    if (label_present(table, title)) return TRUE;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(g_object_get_data(G_OBJECT(table), "list-box")));
    GPtrArray *ids = g_ptr_array_new_with_free_func(g_free); gboolean found = FALSE;
    for (GList *item = rows; item; item = item->next) if (radios_programme_queue_row_id(item->data)) g_ptr_array_add(ids, g_strdup(radios_programme_queue_row_id(item->data)));
    g_list_free(rows);
    for (guint i = 0; !found && i < ids->len; ++i) {
        radios_programme_queue_set_expanded(table, g_ptr_array_index(ids, i), TRUE);
        found = label_present(table, title);
        radios_programme_queue_set_expanded(table, g_ptr_array_index(ids, i), FALSE);
    }
    g_ptr_array_unref(ids); return found;
}
static gboolean queue_two(gpointer data)
{
    Window *window = data;
    return queue_contents(window) == 2 && radios_data_document_revision(radios_broadcast_programme_document(window->ctx)) > 0;
}
typedef struct { Window *window; guint count; const gchar *title; } QueueCondition;
static gboolean queue_ready(gpointer data)
{
    QueueCondition *condition = data;
    if (queue_contents(condition->window) != (gint)condition->count) return FALSE;
    return !condition->count || queue_shows(condition->window, condition->title);
}
typedef struct { QueueCondition queue; const gchar *id; gint64 revision; } SeedCondition;
static gboolean seeded_programme_ready(gpointer value)
{
    SeedCondition *condition = value;
    RadioSDocument *document = radios_broadcast_programme_document(condition->queue.window->ctx);
    return radios_data_connected(condition->queue.window->ctx) &&
        !g_strcmp0(radios_data_document_id(document), condition->id) &&
        radios_data_document_revision(document) == condition->revision && queue_ready(&condition->queue);
}
static void seed_programme(Window *window, JsonNode *first, JsonNode *second)
{
    /* These retained finite/manual queue cases explicitly select that mode.
     * The general schedule has separate default-continuous acceptance. */
    GtkWidget *continuous = find(window->root, "programme-general-continuous");
    if (continuous) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(continuous), FALSE);
    JsonNode *cfg = programme_config(first, second), *saved = document_save("programme", "Mevcut program", json_node_get_object(cfg));
    JsonObject *record = json_node_get_object(saved);
    const gchar *id = radios_json_string(record, "_id", ""); gint64 revision = radios_json_int(record, "_rev", 0);
    g_assert_true(g_uuid_string_is_valid(id)); g_assert_cmpint(revision, >, 0);
    json_node_free(cfg); click_label(window->root, "Verileri yenile");
    JsonObject *media = first ? json_object_get_object_member(json_node_get_object(first), "media") : NULL;
    SeedCondition condition = {{window, first ? (second ? 2u : 1u) : 0u, radios_json_string(media, "title", "")}, id, revision};
    g_test_message("Seeded programme prerequisite: queue contents ready=%d, acknowledged identity/revision ready=%d",
        queue_ready(&condition.queue), seeded_programme_ready(&condition));
    wait_until(seeded_programme_ready, &condition, "seeded programme UUID/revision and contents are acknowledged in native queue");
    json_node_free(saved);
}
static gdouble tone_energy(const gchar *bus, goffset offset, gdouble frequency)
{
    gchar *key = g_strdup_printf("RADIOS_TEST_%s_CAPTURE", bus); const gchar *path = g_getenv(key); g_assert_nonnull(path); g_free(key);
    gint fd = open(path, O_RDONLY); g_assert_cmpint(fd, >=, 0); struct stat info; g_assert_cmpint(fstat(fd, &info), ==, 0);
    gfloat buffer[4096]; long double sine = 0, cosine = 0; guint64 frames = 0;
    while (offset + 8 <= info.st_size) {
        ssize_t bytes = pread(fd, buffer, (size_t)MIN((goffset)sizeof buffer, info.st_size - offset), offset);
        g_assert_cmpint(bytes, >, 0); g_assert_cmpint(bytes % 8, ==, 0);
        for (guint i = 0; i < (guint)bytes / 8; ++i, ++frames) {
            gdouble phase = 2 * G_PI * frequency * (gdouble)frames / 48000;
            sine += buffer[i * 2] * sin(phase); cosine += buffer[i * 2] * cos(phase);
        }
        offset += bytes;
    }
    close(fd); g_assert_cmpuint(frames, >, 4800); return (gdouble)(2 * sqrtl(sine * sine + cosine * cosine) / frames);
}

static void test_queue_pcm(void)
{
    Window window = window_open(); assert_gates_off(&window);
    JsonNode *first = media_import(&window, "Native queue A", 600, 0.25, "music");
    JsonNode *second = media_import(&window, "Native queue B", 1800, 0.2, "music");
    /* This case exercises the current visible programme transport and PCM.
     * Seed its real authenticated document rather than an obsolete playlist
     * search and an action deliberately hidden by the current builder UI.
     * Saved playlist drag/manual loading has its own actual native case. */
    seed_programme(&window, first, second);
    wait_until(queue_two, &window, "verified ordinary programme seeded through authenticated CAS");
    navigate(&window, "broadcast"); combo_index(window.root, "programme-transition-mode", 1); spin(window.root, "programme-transition-overlap", 1);
    goffset start = capture_offset("MAIN"); click(window.root, "programme-play");
    RadioSPlayoutSnapshot current = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 200);
    g_assert_cmpstr(current.next_entry_id, !=, "");
    g_assert_cmpuint(current.duration_ms, >, current.cue_in_ms);
    GtkWidget *queue = find(window.root, "programme-queue-table");
    GtkListBox *list = g_object_get_data(G_OBJECT(queue), "list-box");
    GtkListBoxRow *event_row = gtk_list_box_get_row_at_index(list, 0);
    gchar *block_id = g_strdup(radios_programme_queue_row_id(event_row));
    /* The seeded draft declares no blocks, so its two consecutive music
     * contents form one implicit music event (programme_model.h). Open it:
     * the upcoming media identity is its second content row. */
    g_assert_null(gtk_list_box_get_row_at_index(list, 1));
    radios_programme_queue_set_expanded(queue, block_id, TRUE); run_for(120);
    GtkListBoxRow *upcoming = gtk_list_box_get_row_at_index(list, 1); g_assert_nonnull(upcoming);
    g_assert_true(gtk_style_context_has_class(gtk_widget_get_style_context(GTK_WIDGET(upcoming)), "is-upcoming"));
    g_assert_cmpstr(radios_programme_queue_row_id(upcoming), ==, current.next_entry_id);
    radios_programme_queue_set_expanded(queue, block_id, FALSE);
    card_metadata_wait(&window, (guint)current.deck, "Native queue A", "Medya arşivi", "Sıradaki  Native queue B");
    g_assert_null(find(window.root, "automation-live-next"));
    goffset a = capture_offset("MAIN"); run_for(300);
    g_assert_cmpfloat(tone_energy("MAIN", a, 600), >, 0.1); g_assert_cmpfloat(tone_energy("MAIN", a, 1800), <, 0.01);
    RadioSPlayoutSnapshot transition = run_wait(&window, RADIOS_PLAYOUT_OVERLAP, 0, 0);
    card_metadata_wait(&window, (guint)transition.deck, "Native queue A", "Medya arşivi", "Sıradaki  Native queue B");
    card_metadata_wait(&window, (guint)transition.next_deck, "Native queue B", "Medya arşivi", "Sıradaki  —");
    goffset overlap = capture_offset("MAIN"); run_for(400);
    g_assert_cmpfloat(tone_energy("MAIN", overlap, 600), >, 0.02); g_assert_cmpfloat(tone_energy("MAIN", overlap, 1800), >, 0.02);
    RadioSPlayoutSnapshot next = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 1200); g_assert_cmpstr(next.title, ==, "Native queue B");
    card_metadata_wait(&window, (guint)next.deck, "Native queue B", "Medya arşivi", "Sıradaki  —");
    card_metadata_wait(&window, (guint)current.deck, "—", "", "Sıradaki  —");
    goffset b = capture_offset("MAIN"); run_for(300); g_assert_cmpfloat(tone_energy("MAIN", b, 1800), >, 0.1);
    g_assert_cmpfloat(tone_energy("MAIN", b, 600), <, 0.01); g_assert_cmpfloat(measure("CUE").peak[0], <, 0.0001);
    g_assert_cmpuint(capture_signal_span("MAIN", start, NULL), >, 48000 * 5);
    run_wait(&window, RADIOS_PLAYOUT_FINISHED, 1, 0); run_for(150);
    /* Kılavuz 18.10 s.11: a finished event leaves the active broadcast list;
     * the programme document keeps it (checked below). */
    for (gint index = 0; (event_row = gtk_list_box_get_row_at_index(list, index)); ++index)
        g_assert_cmpstr(radios_programme_queue_row_id(event_row), !=, block_id);
    JsonArray *completed = radios_playout_ui_take_completed(window.ctx);
    g_assert_cmpuint(json_array_get_length(completed), ==, 0); json_array_unref(completed);
    g_free(block_id);
    g_assert_false(gtk_widget_is_sensitive(find(window.root, "programme-stop"))); run_for(300);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001);
    JsonNode *programme = document_read("programme", "Mevcut program"); g_assert_nonnull(programme);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_object_get_object_member(json_node_get_object(programme), "config"), "events")), ==, 2);
    json_node_free(programme); json_node_free(first); json_node_free(second); window_close(&window);
}

typedef struct {
    RadioSAudioEngine *engine;
    guint deck, index;
    RadioSAudioRunState state;
    guint64 position;
} PlayerRunCondition;
static gboolean player_run_matches(gpointer value)
{
    PlayerRunCondition *condition = value; RadioSAudioRunSnapshot snapshot;
    radios_audio_engine_player_runner_snapshot(condition->engine, condition->deck, &snapshot);
    return snapshot.state == condition->state && snapshot.index == condition->index && snapshot.actual_current_frame >= condition->position;
}
static RadioSAudioRunSnapshot player_run_wait(RadioSAudioEngine *engine, guint deck, RadioSAudioRunState state, guint index, guint64 position)
{
    PlayerRunCondition condition = {engine, deck, index, state, position};
    wait_until(player_run_matches, &condition, "selected player actual PCM runner state");
    RadioSAudioRunSnapshot snapshot; radios_audio_engine_player_runner_snapshot(engine, deck, &snapshot); return snapshot;
}
static void player_queue_select(Window *window, const gchar *id, const gchar *title)
{
    combo(window->root, "programme-player-selector", id);
    wait_until(window_ready, window, "selected player queue and separate history documents loaded");
    QueueCondition condition = {window, 2, title}; wait_until(queue_ready, &condition, "selected queue contents rendered");
}

static void saved_player_capture(GtkWidget *widget, const gchar *key);
static void (*saved_player_previous_notice)(gpointer, const gchar *);
static gpointer saved_player_previous_notice_data;
static void saved_player_notice(gpointer unused, const gchar *message)
{
    (void)unused; g_test_message("Saved player native notice: %s", message);
    if (saved_player_previous_notice) saved_player_previous_notice(saved_player_previous_notice_data, message);
}
static void saved_player_notices_attach(Window *window)
{
    saved_player_previous_notice = window->ctx->notify;
    saved_player_previous_notice_data = window->ctx->notify_data;
    window->ctx->notify = saved_player_notice;
}
typedef struct { Window *window; const gchar *id; GtkWidget *source; } SavedDragSource;
static gboolean saved_drag_source_ready(gpointer value)
{
    SavedDragSource *condition = value;
    GtkWidget *list = find(condition->window->root, "broadcast-library-playlists");
    if (!GTK_IS_LIST_BOX(list)) return FALSE;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(list));
    condition->source = NULL;
    for (GList *row = rows; row && !condition->source; row = row->next)
        if (!g_strcmp0(g_object_get_data(G_OBJECT(row->data), "library-id"), condition->id))
            condition->source = gtk_bin_get_child(GTK_BIN(row->data));
    g_list_free(rows); return condition->source != NULL;
}
static void saved_drag_clipboard_get(GtkClipboard *clipboard, GtkSelectionData *selection, guint info, gpointer value)
{
    (void)clipboard;
    /* GTK allocates the selection; the production saved-library source emits
     * its real identifier payload. No prepared playlist or callback is faked. */
    g_signal_emit_by_name(value, "drag-data-get", NULL, selection, info, GDK_CURRENT_TIME);
}
static GtkWidget *saved_player_find_class(GtkWidget *root, const gchar *class_name)
{
    if (gtk_style_context_has_class(gtk_widget_get_style_context(root), class_name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *child = children; child && !found; child = child->next)
        found = saved_player_find_class(child->data, class_name);
    g_list_free(children); return found;
}
typedef struct { GtkWidget *card; guint count; } CardInputCount;
static gboolean card_input_count(gpointer value)
{
    CardInputCount *condition = value; GtkWidget *list = saved_player_find_class(condition->card, "lp-input-list");
    if (!list) return FALSE;
    GList *rows = gtk_container_get_children(GTK_CONTAINER(list)); guint count = 0;
    for (GList *row = rows; row; row = row->next)
        if (gtk_style_context_has_class(gtk_widget_get_style_context(row->data), "lp-input")) ++count;
    g_list_free(rows); return count == condition->count;
}
static void assert_card_input_titles(GtkWidget *card, const gchar *const *titles, guint count)
{
    CardInputCount condition = {card, count}; wait_until(card_input_count, &condition, "saved playlist inputs arrive atomically on actual card");
    GtkWidget *list = saved_player_find_class(card, "lp-input-list"); GList *rows = gtk_container_get_children(GTK_CONTAINER(list));
    g_assert_cmpuint(g_list_length(rows), ==, count);
    for (guint i = 0; i < count; ++i) {
        GtkWidget *label = saved_player_find_class(g_list_nth_data(rows, i), "lp-input-title"); g_assert_true(GTK_IS_LABEL(label));
        gchar *expected = g_strdup_printf("%u. %s", i + 1, titles[i]);
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, expected); g_free(expected);
    }
    g_list_free(rows);
}
static gboolean saved_player_widget_mapped(gpointer value) { return gtk_widget_get_mapped(value); }
static void saved_player_hover_live(Window *window, GtkWidget *card)
{
    GtkWidget *button = find_labeled_button(window->root, "Canlı oynatıcılar"); g_assert_nonnull(button);
    GtkWidget *switcher = gtk_widget_get_ancestor(button, GTK_TYPE_STACK_SWITCHER); g_assert_nonnull(switcher);
    g_assert_true(gtk_widget_get_mapped(button)); g_assert_false(gtk_widget_get_mapped(card));
    gint x = 0, y = 0; g_assert_true(gtk_widget_translate_coordinates(button, switcher, 0, 0, &x, &y));
    gboolean handled = FALSE;
    /* Exercise GTK's actual stack-switcher hover timeout, with its real button
     * allocation. No direct stack selection substitutes for hover routing. */
    g_signal_emit_by_name(switcher, "drag-motion", NULL,
        x + gtk_widget_get_allocated_width(button) / 2, y + gtk_widget_get_allocated_height(button) / 2,
        GDK_CURRENT_TIME, &handled);
    wait_until(saved_player_widget_mapped, card, "native GTK drag hover exposes actual live player card");
    GtkWidget *stack = gtk_widget_get_ancestor(card, GTK_TYPE_STACK); g_assert_true(GTK_IS_STACK(stack));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(stack)), ==, "players");
}
static void saved_playlist_card_drop(Window *window, GtkWidget *card, const gchar *id, guint count)
{
    click_label(window->root, "Parça · Albüm · Program"); pump();
    toggle(window->root, "broadcast-kind-playlist", TRUE);
    SavedDragSource source = {window, id, NULL}; wait_until(saved_drag_source_ready, &source, "genuine saved playlist drag source is rendered");
    g_assert_true(gtk_widget_get_mapped(source.source));
    g_object_ref(source.source);
    GtkTargetList *from = gtk_drag_source_get_target_list(source.source), *to = gtk_drag_dest_get_target_list(card);
    guint from_info = 0, to_info = 0; GdkAtom atom = gdk_atom_intern_static_string(RADIOS_DRAG_PLAYLIST);
    g_assert_nonnull(from); g_assert_nonnull(to);
    g_assert_true(gtk_target_list_find(from, atom, &from_info));
    g_assert_true(gtk_target_list_find(to, atom, &to_info));
    g_assert_cmpuint(from_info, ==, RADIOS_DRAG_INFO_PLAYLIST); g_assert_cmpuint(to_info, ==, RADIOS_DRAG_INFO_PLAYLIST);
    g_signal_emit_by_name(source.source, "drag-begin", NULL);
    const GtkTargetEntry *target = &radios_drag_targets(NULL)[RADIOS_DRAG_INFO_PLAYLIST - 1];
    gchar *selection_name = g_uuid_string_random(); GtkClipboard *clipboard = gtk_clipboard_get(gdk_atom_intern(selection_name, FALSE)); g_free(selection_name);
    g_assert_true(gtk_clipboard_set_with_data(clipboard, target, 1, saved_drag_clipboard_get, NULL, source.source));
    GtkSelectionData *selection = gtk_clipboard_wait_for_contents(clipboard, atom); g_assert_nonnull(selection);
    gchar *payload = radios_drag_payload(selection); g_assert_nonnull(payload); JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, payload, -1, NULL));
    g_assert_cmpstr(radios_json_string(json_node_get_object(json_parser_get_root(parser)), "playlist_id", ""), ==, id);
    g_object_unref(parser); g_free(payload);
    saved_player_hover_live(window, card);
    g_assert_false(gtk_widget_get_mapped(source.source));
    /* Public GTK data-transfer signals invoke the unchanged source/destination
     * handlers. This checks MIME registration and authenticated lookup, without
     * claiming a physical compositor mouse gesture. */
    g_signal_emit_by_name(card, "drag-data-received", NULL, 0, 0, selection, RADIOS_DRAG_INFO_PLAYLIST, GDK_CURRENT_TIME);
    gtk_selection_data_free(selection); gtk_clipboard_clear(clipboard);
    g_signal_emit_by_name(source.source, "drag-end", NULL); g_object_unref(source.source);
    CardInputCount condition = {card, count};
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!card_input_count(&condition) && g_get_monotonic_time() < deadline) pump();
    if (!card_input_count(&condition)) saved_player_capture(window->root, "RADIOS_PLAYLIST_PLAYER_CAPTURE");
    g_assert_true(card_input_count(&condition));
}
static void assert_saved_playlist_unchanged(JsonNode *expected)
{
    JsonNode *body = body_new(), *result = request("/v1/workflows/desktop-playlists/list", body); json_node_free(body);
    JsonObject *saved = json_node_get_object(expected), *found = NULL;
    JsonArray *lists = json_object_get_array_member(json_node_get_object(result), "playlists");
    for (guint i = 0; i < json_array_get_length(lists); ++i) {
        JsonObject *list = json_array_get_object_element(lists, i);
        if (!g_strcmp0(radios_json_string(list, "_id", ""), radios_json_string(saved, "_id", ""))) found = list;
    }
    g_assert_nonnull(found); g_assert_cmpint(radios_json_int(found, "_rev", 0), ==, radios_json_int(saved, "_rev", 0));
    JsonArray *before = json_object_get_array_member(saved, "tracks"), *after = json_object_get_array_member(found, "tracks");
    g_assert_cmpuint(json_array_get_length(after), ==, json_array_get_length(before));
    const gchar *keys[] = {"_id", "media_id", "version_id", "media_file_id", "media_sha256", "title"};
    for (guint i = 0; i < json_array_get_length(before); ++i)
        for (guint k = 0; k < G_N_ELEMENTS(keys); ++k)
            g_assert_cmpstr(radios_json_string(json_array_get_object_element(after, i), keys[k], ""), ==,
                            radios_json_string(json_array_get_object_element(before, i), keys[k], ""));
    json_node_free(result);
}
typedef struct { const gchar *name; GtkWidget *dialog, *tree; GtkTreeIter iter; } SavedLoaderList;
static gboolean saved_loader_list_ready(gpointer value)
{
    SavedLoaderList *condition = value; condition->dialog = dialog_find("player-loader-dialog");
    if (!condition->dialog) return FALSE;
    GtkWidget *table = find(condition->dialog, "player-loader-library");
    condition->tree = table ? g_object_get_data(G_OBJECT(table), "tree-view") : NULL;
    if (!GTK_IS_TREE_VIEW(condition->tree)) return FALSE;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(condition->tree));
    gboolean valid = gtk_tree_model_get_iter_first(model, &condition->iter);
    while (valid) {
        gchar *name = NULL; gtk_tree_model_get(model, &condition->iter, 0, &name, -1);
        gboolean matches = !g_strcmp0(name, condition->name); g_free(name); if (matches) return TRUE;
        valid = gtk_tree_model_iter_next(model, &condition->iter);
    }
    return FALSE;
}
static void saved_player_capture(GtkWidget *widget, const gchar *key)
{
    const gchar *path = g_getenv(key); if (!path || !*path) return;
    run_for(75); GdkWindow *window = gtk_widget_get_window(widget);
    g_assert_nonnull(window); g_assert_true(gdk_window_is_viewable(window));
    GdkPixbuf *pixbuf = gdk_pixbuf_get_from_window(window, 0, 0, gdk_window_get_width(window), gdk_window_get_height(window));
    g_assert_nonnull(pixbuf); GError *error = NULL;
    g_assert_true(gdk_pixbuf_save(pixbuf, path, "png", &error, NULL)); g_assert_no_error(error); g_object_unref(pixbuf);
}
static void test_saved_playlist_card_drag_and_loader_pcm(void)
{
    Window window = window_open(); saved_player_notices_attach(&window); assert_gates_off(&window);
    JsonNode *first = media_import(&window, "Saved player source A", 600, 0.25, "music");
    JsonNode *second = media_import(&window, "Saved player source B", 1700, 0.2, "music");
    JsonNode *saved = playlist_create("Saved card drag playlist", first, second);
    JsonNode *append = playlist_create("Append preserved saved playlist", second, NULL);
    seed_programme(&window, NULL, NULL); navigate(&window, "broadcast");
    GtkWidget *card = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0"); g_assert_nonnull(card);
    const gchar *titles[] = {"Saved player source A", "Saved player source B", "Saved player source B"};
    saved_playlist_card_drop(&window, card, radios_json_string(json_node_get_object(saved), "_id", ""), 2);
    assert_card_input_titles(card, titles, 2);
    saved_player_capture(window.root, "RADIOS_PLAYLIST_PLAYER_CAPTURE");
    g_assert_cmpstr(g_object_get_data(G_OBJECT(card), "radios-player-playlist-name"), ==, "Saved card drag playlist");
    click(window.root, "deck-0-play"); RadioSAudioUiSnapshot playing = deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
    g_assert_cmpstr(playing.media_id, ==, radios_json_string(json_object_get_object_member(json_node_get_object(first), "media"), "_id", ""));
    goffset offset = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", offset, 600), >, 0.1);
    click(window.root, "deck-0-next"); playing = deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
    g_assert_cmpstr(playing.media_id, ==, radios_json_string(json_object_get_object_member(json_node_get_object(second), "media"), "_id", ""));
    offset = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", offset, 1700), >, 0.08);
    click(window.root, "deck-0-stop"); deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    saved_playlist_card_drop(&window, card, radios_json_string(json_node_get_object(append), "_id", ""), 3);
    assert_card_input_titles(card, titles, 3); assert_saved_playlist_unchanged(saved); assert_saved_playlist_unchanged(append);
    window_close(&window);
    /* Persistence is proven by reopening the actual full application and loading
     * the still-saved list through its unwrapped card picker. */
    window = window_open(); saved_player_notices_attach(&window); navigate(&window, "broadcast");
    click_label(window.root, "Canlı oynatıcılar"); pump();
    g_assert_true(gtk_widget_get_mapped(find(window.root, "deck-0-playlist"))); click(window.root, "deck-0-playlist");
    SavedLoaderList loader = {"Saved card drag playlist", NULL, NULL, {0}};
    wait_until(saved_loader_list_ready, &loader, "saved playlist survives reopen in actual card loader");
    gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(loader.tree)), &loader.iter);
    GtkWidget *accept = gtk_dialog_get_widget_for_response(GTK_DIALOG(loader.dialog), GTK_RESPONSE_ACCEPT);
    wait_until(widget_enabled, accept, "actual saved playlist picker enables load after selection");
    GtkWidget *items = find(loader.dialog, "player-loader-library-items");
    GtkTreeView *tree = g_object_get_data(G_OBJECT(items), "tree-view"); g_assert_true(GTK_IS_TREE_VIEW(tree));
    g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_tree_view_get_model(tree), NULL), ==, 2);
    saved_player_capture(loader.dialog, "RADIOS_PLAYLIST_PLAYER_LOADER_CAPTURE");
    gtk_button_clicked(GTK_BUTTON(accept)); pump(); g_assert_null(dialog_find("player-loader-dialog"));
    card = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0"); assert_card_input_titles(card, titles, 2);
    click(window.root, "deck-0-play"); playing = deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
    g_assert_cmpstr(playing.media_id, ==, radios_json_string(json_object_get_object_member(json_node_get_object(first), "media"), "_id", ""));
    offset = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", offset, 600), >, 0.1);
    click(window.root, "deck-0-stop"); deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    assert_saved_playlist_unchanged(saved); window_close(&window);
    json_node_free(saved); json_node_free(append); json_node_free(first); json_node_free(second);
}

static void test_independent_player_native_pcm(void)
{
    JsonNode *previous_policy = document_read("playout_policy", "main");
    JsonObject *policy = json_object_new();
    json_object_set_int_member(policy, "schema", 1); json_object_set_int_member(policy, "mode", RADIOS_AUDIO_RUN_CROSSFADE);
    json_object_set_int_member(policy, "overlap_ms", 3000); json_object_set_boolean_member(policy, "beat_sync", TRUE);
    JsonNode *saved_policy = document_save("playout_policy", "main", policy); json_object_unref(policy);
    Window window = window_open(); assert_gates_off(&window);
    JsonNode *first = media_import(&window, "Independent player A", 600, 0.25, "music");
    JsonNode *second = media_import(&window, "Independent player B", 1700, 0.2, "music");
    first = media_metadata_save(first, "Sanatçı A", "Albüm A"); second = media_metadata_save(second, "Sanatçı B", "Albüm B");
    seed_programme(&window, NULL, NULL);
    for (guint player = 1; player <= 2; ++player) {
        JsonNode *configuration = programme_config(player == 1 ? first : second, player == 1 ? second : first);
        JsonObject *settings = json_node_get_object(configuration); json_object_set_int_member(settings, "queue_player", player);
        json_object_set_int_member(settings, "queue_transition", 0); json_object_set_double_member(settings, "queue_overlap", 0);
        gchar *name = g_strdup_printf("Oynatıcı %u yayın sırası", player);
        JsonNode *saved = document_save("programme", name, settings); json_node_free(saved); json_node_free(configuration); g_free(name);
    }
    navigate(&window, "broadcast"); RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    player_queue_select(&window, "1", "Independent player A"); click(window.root, "programme-play");
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    player_queue_select(&window, "2", "Independent player B"); click(window.root, "programme-play");
    RadioSAudioRunSnapshot other = player_run_wait(engine, 1, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    card_metadata_wait(&window, 0, "Independent player A", "Sanatçı A · Albüm A · Medya arşivi", "Sıradaki  Sanatçı B — Independent player B");
    card_metadata_wait(&window, 1, "Independent player B", "Sanatçı B · Albüm B · Medya arşivi", "Sıradaki  Sanatçı A — Independent player A");
    goffset captured = capture_offset("MAIN"); run_for(200);
    g_assert_cmpfloat(tone_energy("MAIN", captured, 600), >, 0.1); g_assert_cmpfloat(tone_energy("MAIN", captured, 1700), >, 0.1);
    player_queue_select(&window, "1", "Independent player A"); click(window.root, "programme-pause");
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PAUSED, 0, 0); run_for(100);
    card_metadata_wait(&window, 0, "Independent player A", "Albüm A", "Sıradaki  Sanatçı B — Independent player B");
    captured = capture_offset("MAIN"); run_for(200);
    g_assert_cmpfloat(tone_energy("MAIN", captured, 600), <, 0.01); g_assert_cmpfloat(tone_energy("MAIN", captured, 1700), >, 0.1);
    RadioSAudioRunSnapshot still = player_run_wait(engine, 1, RADIOS_AUDIO_RUN_PLAYING, 0, other.actual_current_frame);
    g_assert_cmpuint(still.epoch, ==, other.epoch);
    click(window.root, "programme-play"); player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    click(window.root, "programme-next"); player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 1, 4800);
    card_metadata_wait(&window, 0, "Independent player B", "Sanatçı B · Albüm B", "Sıradaki  —");
    card_metadata_wait(&window, 1, "Independent player B", "Sanatçı B · Albüm B", "Sıradaki  Sanatçı A — Independent player A");
    still = player_run_wait(engine, 1, RADIOS_AUDIO_RUN_PLAYING, 0, other.actual_current_frame); g_assert_cmpuint(still.epoch, ==, other.epoch);
    click(window.root, "programme-stop"); run_for(100); captured = capture_offset("MAIN"); run_for(200);
    card_metadata_wait(&window, 0, "—", "", "Sıradaki  —");
    card_metadata_wait(&window, 1, "Independent player B", "Albüm B", "Sıradaki  Sanatçı A — Independent player A");
    g_assert_cmpfloat(tone_energy("MAIN", captured, 1700), >, 0.1);
    still = player_run_wait(engine, 1, RADIOS_AUDIO_RUN_PLAYING, 0, other.actual_current_frame); g_assert_cmpuint(still.epoch, ==, other.epoch);
    player_queue_select(&window, "2", "Independent player B"); click(window.root, "programme-stop"); run_for(200);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001);
    RadioSAudioRunSnapshot global; radios_audio_engine_runner_snapshot(engine, &global); g_assert_cmpint(global.state, ==, RADIOS_AUDIO_RUN_IDLE);
    JsonNode *first_history = document_read("playout_history", "Oynatıcı 1 oynatma geçmişi");
    JsonNode *second_history = document_read("playout_history", "Oynatıcı 2 oynatma geçmişi");
    g_assert_nonnull(first_history); g_assert_nonnull(second_history);
    JsonArray *entries = json_object_get_array_member(json_object_get_object_member(json_node_get_object(second_history), "config"), "entries");
    g_assert_cmpuint(json_array_get_length(entries), ==, 1);
    const gchar *second_id = radios_json_string(json_object_get_object_member(json_node_get_object(second), "media"), "_id", "");
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(entries, 0), "media_id", ""), ==, second_id);
    json_node_free(first_history); json_node_free(second_history); radios_audio_engine_unref(engine);
    json_node_free(first); json_node_free(second); window_close(&window);
    if (previous_policy) {
        JsonNode *restored = document_save("playout_policy", "main", json_object_get_object_member(json_node_get_object(previous_policy), "config"));
        json_node_free(restored); json_node_free(previous_policy);
    } else {
        JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body), *record = json_node_get_object(saved_policy);
        json_object_set_string_member(object, "kind", "playout_policy");
        json_object_set_string_member(object, "document_id", radios_json_string(record, "_id", ""));
        json_object_set_int_member(object, "expected_revision", radios_json_int(record, "_rev", 0));
        gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
        JsonNode *removed = request("/v1/workflows/desktop/delete", body); json_node_free(removed); json_node_free(body);
    }
    json_node_free(saved_policy);
}

static guint history_count(JsonNode *record, const gchar *media_id, gint64 *first_utc, gint64 *last_utc)
{
    JsonObject *configuration = record ? json_object_get_object_member(json_node_get_object(record), "config") : NULL;
    JsonArray *entries = configuration ? json_object_get_array_member(configuration, "entries") : NULL;
    guint count = 0; gint64 first = G_MAXINT64, last = 0;
    for (guint i = 0; entries && i < json_array_get_length(entries); ++i) {
        JsonObject *entry = json_array_get_object_element(entries, i);
        if (g_strcmp0(radios_json_string(entry, "media_id", ""), media_id)) continue;
        g_assert_cmpint(radios_json_int(entry, "position_frames", 0), >, 0);
        GDateTime *stamp = g_date_time_new_from_iso8601(radios_json_string(entry, "started_utc", ""), NULL);
        g_assert_nonnull(stamp); gint64 utc = g_date_time_to_unix(stamp) * G_TIME_SPAN_SECOND + g_date_time_get_microsecond(stamp);
        g_date_time_unref(stamp); first = MIN(first, utc); last = MAX(last, utc); ++count;
    }
    if (first_utc) *first_utc = count ? first : 0;
    if (last_utc) *last_utc = last;
    return count;
}
static void test_player_manual_metadata_identity(void)
{
    Window window = window_open(); seed_programme(&window, NULL, NULL); navigate(&window, "broadcast");
    JsonNode *first = media_import(&window, "Doğrulanmış parça", 600, 0.2, "music");
    JsonNode *second = media_import(&window, "Yeni oynatıcı içeriği", 1700, 0.2, "music");
    first = media_metadata_save(first, "Gerçek sanatçı", "Gerçek albüm");
    GString *long_artist = g_string_new(NULL);
    for (guint index = 0; index < 200; ++index) g_string_append(long_artist, "🎧");
    second = media_metadata_save(second, long_artist->str, "Uzun albüm");
    GtkWidget *card = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0"); g_assert_nonnull(card);
    GError *error = NULL;
    require_call(radios_audio_ui_load_media(window.ctx, card, json_node_get_object(first), 0, FALSE, &error), &error);
    RadioSAudioUiSnapshot loaded = deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    g_assert_cmpstr(loaded.title, ==, "Doğrulanmış parça"); g_assert_cmpstr(loaded.artist, ==, "Gerçek sanatçı");
    card_metadata_wait(&window, 0, "Doğrulanmış parça", "Gerçek sanatçı · Gerçek albüm · Medya arşivi", "Sıradaki  —");
    card_metadata_wait(&window, 1, "—", "", "Sıradaki  —");
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    JsonObject *detail = json_node_get_object(second), *version = json_object_get_object_member(detail, "version");
    JsonObject *media = json_object_get_object_member(detail, "media");
    RadioSAudioLoadSpec replacement = {.path = radios_json_string(version, "managed_path", ""), .sha256 = radios_json_string(version, "sha256", ""),
        .media_id = radios_json_string(media, "_id", ""), .version_id = radios_json_string(version, "_id", ""),
        .cue_out_frame = (guint64)radios_json_int(version, "duration_ms", 0) * 48};
    require_call(radios_audio_engine_load(engine, 0, &replacement, &error), &error);
    loaded = deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0); g_assert_cmpuint(loaded.generation, >, 0); g_assert_cmpstr(loaded.title, ==, "");
    card_metadata_wait(&window, 0, "—", "", "Sıradaki  —");
    require_call(radios_audio_ui_seek(window.ctx, 0, 0, &error), &error); deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    require_call(radios_audio_ui_sync(window.ctx, 0, -1, FALSE, &error), &error); deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    card_metadata_wait(&window, 0, "—", "", "Sıradaki  —");
    require_call(radios_audio_ui_load_media(window.ctx, card, detail, 0, FALSE, &error), &error);
    loaded = deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    g_assert_cmpstr(loaded.artist, ==, long_artist->str); g_assert_true(g_utf8_validate(loaded.artist, -1, NULL));
    card_metadata_wait(&window, 0, "Yeni oynatıcı içeriği", "Medya arşivi", "Sıradaki  —");
    GtkWidget *information = find(window.root, "deck-0-metadata");
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(information)), long_artist->str));
    g_assert_cmpint(gtk_label_get_ellipsize(GTK_LABEL(information)), ==, PANGO_ELLIPSIZE_END);
    gchar *tooltip = gtk_widget_get_tooltip_text(information); g_assert_nonnull(strstr(tooltip, long_artist->str)); g_free(tooltip);
    JsonNode *invalid = deep_copy(first);
    json_object_set_string_member(json_object_get_object_member(json_node_get_object(invalid), "media"), "station_id", "wrong-station");
    g_assert_false(radios_audio_ui_load_media(window.ctx, card, json_node_get_object(invalid), 0, FALSE, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED); g_clear_error(&error); json_node_free(invalid);
    card_metadata_wait(&window, 0, "Yeni oynatıcı içeriği", "Medya arşivi", "Sıradaki  —");
    require_call(radios_audio_engine_unload(engine, 0, &error), &error); deck_wait(&window, 0, RADIOS_AUDIO_UI_EMPTY, 0);
    card_metadata_wait(&window, 0, "—", "", "Sıradaki  —");
    radios_audio_engine_unref(engine); json_node_free(first); json_node_free(second); g_string_free(long_artist, TRUE); window_close(&window);
}
typedef struct { GtkWidget *table; guint minimum; } HistoryRows;
static gboolean history_rows_visible(gpointer data)
{
    HistoryRows *rows = data;
    GtkTreeModel *model = g_object_get_data(G_OBJECT(rows->table), "list-store");
    return gtk_tree_model_iter_n_children(model, NULL) >= (gint)rows->minimum;
}

static void test_transport_busy(void)
{
    Window window = window_open(); JsonNode *first = media_import(&window, "Transport A", 500, 0.25, "music");
    JsonNode *second = media_import(&window, "Transport B", 1500, 0.2, "music"); seed_programme(&window, first, second);
    wait_until(queue_two, &window, "transport queue refreshed"); navigate(&window, "broadcast");
    combo_index(window.root, "programme-transition-mode", 3); click(window.root, "programme-play");
    RadioSPlayoutSnapshot before = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 200);
    click(window.root, "header-playback-history");
    GtkWidget *history_dialog = dialog_find("programme-history-dialog"); g_assert_nonnull(history_dialog);
    HistoryRows visible_history = {find(history_dialog, "programme-history-table"), 1};
    g_assert_nonnull(visible_history.table);
    wait_until(history_rows_visible, &visible_history, "live local playback appears in the history dialog");
    click(window.root, "programme-pause"); RadioSPlayoutSnapshot paused = run_wait(&window, RADIOS_PLAYOUT_PAUSED, 0, 0);
    run_for(250); RadioSPlayoutSnapshot still = {0}; g_assert_true(radios_playout_ui_get_snapshot(window.ctx, &still));
    g_assert_cmpuint(still.position_ms, <=, paused.position_ms + 40); g_assert_cmpuint(still.position_ms, >=, before.position_ms);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001); click(window.root, "programme-play");
    run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, paused.position_ms + 50);
    click(window.root, "programme-next"); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 100);
    click(window.root, "programme-previous"); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 100);
    click(window.root, "programme-stop"); run_for(250); g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001);
    const gchar *first_id = radios_json_string(json_object_get_object_member(json_node_get_object(first), "media"), "_id", "");
    const gchar *second_id = radios_json_string(json_object_get_object_member(json_node_get_object(second), "media"), "_id", "");
    JsonNode *history = NULL; gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline) {
        g_clear_pointer(&history, json_node_free); history = document_read("playout_history", "Yerel oynatma geçmişi");
        if (history_count(history, first_id, NULL, NULL) >= 2 && history_count(history, second_id, NULL, NULL) >= 1) break;
        run_for(250);
    }
    gint64 first_utc = 0, replay_utc = 0;
    g_assert_cmpuint(history_count(history, first_id, &first_utc, &replay_utc), ==, 2);
    g_assert_cmpuint(history_count(history, second_id, NULL, NULL), ==, 1); g_assert_cmpint(replay_utc, >, first_utc);
    visible_history.minimum = 3;
    wait_until(history_rows_visible, &visible_history, "open history dialog refreshes as the queue advances");
    g_clear_pointer(&history, json_node_free);
    GError *error = NULL; require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(first), 0, FALSE, &error), &error);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    /* With other native card outputs disabled, the only MAIN-mapped deck is
     * busy with a manual source. Starting the queue must preserve that load. */
    for (guint deck = 1; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) {
        gchar *key = g_strdup_printf("radios-deck-owner-%u", deck);
        GtkWidget *card = g_object_get_data(G_OBJECT(window.ctx->parent), key); g_free(key);
        if (card) {
            GtkWidget *main = g_object_get_data(G_OBJECT(card), "radios-deck-main-widget"); g_assert_true(GTK_IS_TOGGLE_BUTTON(main));
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(main), FALSE);
        }
    }
    JsonNode *cfg = programme_config(first, second);
    require_call(radios_playout_ui_start_programme(window.ctx, window.root, json_node_get_object(cfg), 0, 1, 1, &error), &error);
    run_wait(&window, RADIOS_PLAYOUT_ERROR, 0, 0);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001); g_assert_cmpfloat(measure("CUE").peak[0], <, 0.0001);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0); toggle(window.root, "header-headphone-cue", FALSE);
    json_node_free(cfg); json_node_free(first); json_node_free(second); window_close(&window);
    window = window_open(); assert_gates_off(&window);
    click(window.root, "header-playback-history");
    history_dialog = dialog_find("programme-history-dialog"); g_assert_nonnull(history_dialog);
    visible_history.table = find(history_dialog, "programme-history-table"); g_assert_nonnull(visible_history.table);
    wait_until(history_rows_visible, &visible_history, "persisted history is visible after reopening the application");
    window_close(&window);
}

static JsonNode *automation_config(JsonNode *first, JsonNode *second, JsonNode *jingle)
{
    const gchar *text = "{\"name\":\"Saved automation\",\"seed\":\"RADIOS-NATIVE-10\",\"planDate\":\"2026-10-06\",\"startTime\":\"00:00\",\"windowStart\":\"00:00\",\"windowEnd\":\"00:00\",\"sourceMode\":1,\"trackMode\":1,\"transition\":0,\"boundaryTransition\":0,\"deckMode\":0,\"fallbackMode\":2,\"trackGap\":0,\"artistGap\":0,\"albumGap\":0,\"previewCount\":2,\"jingleEvery\":1,\"overlap\":0,\"boundaryOverlap\":0,\"includeProgramme\":false,\"jingleEnabled\":true,\"jingleDry\":true,\"requireMetadata\":false,\"jinglePlaylist\":null,\"fallbackPlaylist\":null,\"jingleDeck\":null,\"weekdays\":[1,2,3,4,5,6,7],\"decks\":[\"1\",\"2\"],\"sources\":[]}";
    JsonParser *parser = json_parser_new(); g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));
    JsonNode *node = deep_copy(json_parser_get_root(parser)); g_object_unref(parser); JsonObject *object = json_node_get_object(node);
    GDateTime *now = g_date_time_new_now_utc(); gchar *date = g_date_time_format(now, "%Y-%m-%d"), *clock = g_date_time_format(now, "%H:%M"); g_date_time_unref(now);
    json_object_set_string_member(object, "planDate", date); json_object_set_string_member(object, "startTime", clock); g_free(date); g_free(clock);
    json_object_set_string_member(object, "jinglePlaylist", radios_json_string(json_node_get_object(jingle), "_id", ""));
    json_object_set_string_member(object, "fallbackPlaylist", radios_json_string(json_node_get_object(second), "_id", ""));
    JsonArray *sources = json_object_get_array_member(object, "sources"); JsonNode *playlists[] = {first, second};
    for (guint i = 0; i < G_N_ELEMENTS(playlists); ++i) {
        JsonObject *row = json_object_new(); gchar *uuid = g_uuid_string_random(); json_object_set_string_member(row, "source_id", uuid); g_free(uuid);
        json_object_set_string_member(row, "playlist_id", radios_json_string(json_node_get_object(playlists[i]), "_id", ""));
        json_object_set_int_member(row, "weight", i ? 3 : 1); json_object_set_int_member(row, "count", 1);
        json_object_set_string_member(row, "start", "00:00"); json_object_set_string_member(row, "end", "00:00"); json_object_set_int_member(row, "days", 0);
        json_array_add_object_element(sources, row);
    }
    return node;
}
/* The legacy tab was intentionally removed. These two retained compatibility
 * cases exercise its public authenticated planner/runtime API, while current
 * rotation controls are tested separately through genuine mapped widgets. */
static JsonNode *automation_preview(Window *window, JsonObject *configuration)
{
    Import output = {0}; GError *error = NULL;
    require_call(radios_playout_ui_preview_automation(window->ctx, window->root, configuration,
        import_complete, &output, import_destroy, &error), &error);
    wait_until(import_done, &output, "public authenticated automation preview completes");
    g_assert_no_error(output.error); g_assert_cmpuint(output.callbacks, ==, 1); g_assert_cmpuint(output.destroyed, ==, 1);
    g_assert_nonnull(output.result); JsonObject *plan = json_node_get_object(output.result);
    g_assert_cmpstr(radios_json_string(plan, "kind", ""), ==, "automation");
    g_assert_cmpstr(radios_json_string(plan, "proof", ""), ==, "planning_only");
    return output.result;
}
static gboolean history_contains(JsonNode *record, const gchar *media_id)
{
    JsonObject *object = record ? json_object_get_object_member(json_node_get_object(record), "config") : NULL;
    JsonArray *entries = object ? json_object_get_array_member(object, "entries") : NULL;
    for (guint i = 0; entries && i < json_array_get_length(entries); ++i) {
        JsonObject *entry = json_array_get_object_element(entries, i);
        if (!g_strcmp0(radios_json_string(entry, "media_id", ""), media_id) && radios_json_int(entry, "position_frames", 0) > 0) return TRUE;
    }
    return FALSE;
}
static void test_automation_preview_history(void)
{
    Window window = window_open(); JsonNode *a = media_import(&window, "Automation A", 700, 0.2, "music");
    JsonNode *b = media_import(&window, "Automation B", 1700, 0.2, "music"), *j = media_import(&window, "Automation jingle", 2500, 0.15, "jingle");
    JsonNode *pa = playlist_create("Automation source A", a, NULL), *pb = playlist_create("Automation source B", b, NULL);
    JsonNode *pj = playlist_create("Automation jingle source", j, NULL), *cfg = automation_config(pa, pb, pj);
    JsonNode *saved = document_save("automation", "main", json_node_get_object(cfg));
    gchar *saved_id = g_strdup(radios_json_string(json_node_get_object(saved), "_id", ""));
    gint64 saved_revision = radios_json_int(json_node_get_object(saved), "_rev", 0);
    g_assert_true(g_uuid_string_is_valid(saved_id)); g_assert_cmpint(saved_revision, >, 0); json_node_free(saved);
    saved = document_read("automation", "main"); g_assert_nonnull(saved);
    g_assert_cmpstr(radios_json_string(json_node_get_object(saved), "_id", ""), ==, saved_id);
    g_assert_cmpint(radios_json_int(json_node_get_object(saved), "_rev", 0), ==, saved_revision);
    json_node_free(cfg); cfg = deep_copy(json_object_get_member(json_node_get_object(saved), "config")); json_node_free(saved);
    JsonObject *settings = json_node_get_object(cfg);
    g_assert_cmpstr(radios_json_string(settings, "jinglePlaylist", ""), ==, radios_json_string(json_node_get_object(pj), "_id", ""));
    g_assert_null(find(window.root, "automation-name"));
    json_object_set_string_member(settings, "name", "Current unsaved automation");
    JsonNode *before_preview = document_read("playout_history", "Yerel oynatma geçmişi");
    JsonNode *preview = automation_preview(&window, settings);
    JsonArray *preview_entries = json_object_get_array_member(json_node_get_object(preview), "entries");
    g_assert_cmpuint(json_array_get_length(preview_entries), >=, 3);
    guint preview_music = 0, preview_jingles = 0;
    for (guint index = 0; index < json_array_get_length(preview_entries); ++index) {
        JsonObject *entry = json_array_get_object_element(preview_entries, index);
        g_assert_true(g_uuid_string_is_valid(radios_json_string(entry, "media_id", "")));
        if (g_str_equal(radios_json_string(entry, "kind", ""), "jingle")) ++preview_jingles; else ++preview_music;
    }
    g_assert_cmpuint(preview_music, ==, 2); g_assert_cmpuint(preview_jingles, >, 0); json_node_free(preview);

    g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001); g_assert_cmpfloat(measure("CUE").peak[0], <, 0.0001);
    RadioSPlayoutSnapshot snapshot = {0}; g_assert_true(radios_playout_ui_get_snapshot(window.ctx, &snapshot));
    g_assert_cmpint(snapshot.state, ==, RADIOS_PLAYOUT_IDLE);
    JsonNode *after_preview = document_read("playout_history", "Yerel oynatma geçmişi");
    if (before_preview) { g_assert_nonnull(after_preview); g_assert_true(json_node_equal(before_preview, after_preview)); }
    else g_assert_null(after_preview);
    g_clear_pointer(&before_preview, json_node_free); g_clear_pointer(&after_preview, json_node_free);
    saved = document_read("automation", "main"); g_assert_cmpstr(radios_json_string(json_object_get_object_member(json_node_get_object(saved), "config"), "name", ""), ==, "Saved automation"); json_node_free(saved);
    json_object_set_int_member(settings, "sourceMode", 0); json_object_set_int_member(settings, "previewCount", 1);
    GError *error = NULL; require_call(radios_playout_ui_start_automation(window.ctx, window.root, settings, &error), &error);
    RadioSPlayoutSnapshot started = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 200);
    g_assert_cmpint(started.deck, ==, 0); RadioSAudioUiSnapshot played = deck_wait(&window, (guint)started.deck, RADIOS_AUDIO_UI_PLAYING, 200);
    gchar *played_id = g_strdup(played.media_id); g_assert_true(g_uuid_string_is_valid(played_id));
    g_assert_cmpstr(played_id, ==, radios_json_string(json_object_get_object_member(json_node_get_object(a), "media"), "_id", ""));
    goffset continuity_start = capture_offset("MAIN"), tone = continuity_start;
    run_for(300); g_assert_cmpfloat(tone_energy("MAIN", tone, 700), >, 0.1);
    run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 200); tone = capture_offset("MAIN"); run_for(300);
    g_assert_cmpfloat(tone_energy("MAIN", tone, 2500), >, 0.08);
    guint64 jingle_gap = 0; capture_signal_span("MAIN", continuity_start, &jingle_gap);
    g_test_message("Music to jingle: longest captured silent gap %" G_GUINT64_FORMAT " frames", jingle_gap);
    /* One music item per preparation means index2 belongs to an appended
     * batch. It must advance to source B instead of repeating source A. */
    RadioSPlayoutSnapshot appended = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 2, 200);
    RadioSAudioUiSnapshot second = deck_wait(&window, (guint)appended.deck, RADIOS_AUDIO_UI_PLAYING, 200);
    gchar *second_id = g_strdup(second.media_id);
    g_assert_cmpstr(second_id, ==, radios_json_string(json_object_get_object_member(json_node_get_object(b), "media"), "_id", ""));
    g_assert_cmpstr(second_id, !=, played_id); tone = capture_offset("MAIN"); run_for(300);
    g_assert_cmpfloat(tone_energy("MAIN", tone, 1700), >, 0.1); g_assert_cmpfloat(tone_energy("MAIN", tone, 700), <, 0.01);
    guint64 gap = 0; g_assert_cmpuint(capture_signal_span("MAIN", continuity_start, &gap), >, 48000 * 11);
    g_test_message("One-music batch append: longest captured silent gap %" G_GUINT64_FORMAT " frames", gap);
    g_assert_cmpuint(gap, <, 4800);
    g_assert_cmpfloat(measure("CUE").peak[0], <, 0.0001); stop_run(&window); run_for(50);
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND; JsonNode *history = NULL;
    while (g_get_monotonic_time() < deadline) {
        g_clear_pointer(&history, json_node_free); history = document_read("playout_history", "Yerel oynatma geçmişi");
        JsonObject *configuration = history ? json_object_get_object_member(json_node_get_object(history), "config") : NULL;
        if (history_contains(history, played_id) && history_contains(history, second_id) && json_object_has_member(configuration, "rotation")) break;
        run_for(250);
    }
    g_assert_nonnull(history); g_assert_true(history_contains(history, played_id)); g_assert_true(history_contains(history, second_id));
    g_assert_true(history_contains(history, radios_json_string(json_object_get_object_member(json_node_get_object(j), "media"), "_id", "")));
    JsonObject *hc = json_object_get_object_member(json_node_get_object(history), "config");
    g_assert_cmpstr(radios_json_string(hc, "kind", ""), ==, "local_playback");
    JsonObject *rotation = json_object_get_object_member(hc, "rotation"); g_assert_nonnull(rotation);
    g_assert_cmpint(radios_json_int(rotation, "schema", 0), ==, 1);
    g_assert_cmpuint(strlen(radios_json_string(rotation, "config_key", "")), ==, 64);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(rotation, "cursors")), ==, 2);
    g_assert_cmpint(radios_json_int(rotation, "source_index", -1), ==, 0);
    JsonArray *entries = json_object_get_array_member(hc, "entries"); g_assert_cmpuint(json_array_get_length(entries), >, 0);
    g_assert_cmpint(radios_json_int(json_array_get_object_element(entries, json_array_get_length(entries) - 1), "position_frames", 0), >, 0);
    window_close(&window); window = window_open(); assert_gates_off(&window);
    g_assert_true(radios_playout_ui_get_snapshot(window.ctx, &snapshot)); g_assert_cmpint(snapshot.state, ==, RADIOS_PLAYOUT_IDLE);
    JsonNode *reopened = document_read("playout_history", "Yerel oynatma geçmişi"); g_assert_nonnull(reopened);
    g_assert_true(history_contains(reopened, played_id)); g_assert_true(history_contains(reopened, second_id));
    g_free(played_id); g_free(second_id);
    JsonObject *reopened_config = json_object_get_object_member(json_node_get_object(reopened), "config");
    g_assert_true(json_object_equal(rotation, json_object_get_object_member(reopened_config, "rotation")));
    g_assert_cmpint(radios_json_int(json_node_get_object(reopened), "_rev", 0), >=, radios_json_int(json_node_get_object(history), "_rev", 0));
    saved = document_read("automation", "main"); g_assert_nonnull(saved);
    g_assert_cmpstr(radios_json_string(json_node_get_object(saved), "_id", ""), ==, saved_id);
    g_assert_cmpint(radios_json_int(json_node_get_object(saved), "_rev", 0), ==, saved_revision);
    g_assert_cmpstr(radios_json_string(json_object_get_object_member(json_node_get_object(saved), "config"), "name", ""), ==, "Saved automation");
    json_node_free(saved); g_free(saved_id);
    json_node_free(reopened); json_node_free(history); json_node_free(cfg); json_node_free(pa); json_node_free(pb); json_node_free(pj);
    json_node_free(a); json_node_free(b); json_node_free(j); window_close(&window);
}

static void test_schedule_cancel(void)
{
    Window window = window_open(); JsonNode *detail = media_import(&window, "Scheduled tone", 1100, 0.2, "music");
    JsonNode *cfg = programme_config(detail, NULL); GError *error = NULL;
    require_call(radios_playout_ui_schedule_programme(window.ctx, window.root, json_node_get_object(cfg),
        g_get_real_time() / 1000 + 1200, 0, 0, &error), &error);
    run_wait(&window, RADIOS_PLAYOUT_SCHEDULED, 0, 0); g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001);
    navigate(&window, "broadcast"); click(window.root, "programme-stop"); run_for(1500);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001);
    gint64 target = g_get_real_time() / 1000 + 1200;
    require_call(radios_playout_ui_schedule_programme(window.ctx, window.root, json_node_get_object(cfg), target, 0, 0, &error), &error);
    RadioSPlayoutSnapshot scheduled = run_wait(&window, RADIOS_PLAYOUT_SCHEDULED, 0, 0);
    g_assert_cmpint(scheduled.start_utc_us, ==, target * 1000); goffset before = capture_offset("MAIN");
    while (g_get_real_time() / 1000 < target - 100) pump();
    Capture early = capture("MAIN", before);
    g_test_message("Scheduled PCM check at target%+" G_GINT64_FORMAT " ms: peak %.6f",
        g_get_real_time() / 1000 - target, early.peak[0]);
    g_assert_cmpfloat(early.peak[0], <, 0.0001);
    run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 150); g_assert_cmpfloat(measure("MAIN").peak[0], >, 0.01);
    click(window.root, "programme-stop"); run_for(300);
    json_node_free(cfg); json_node_free(detail); window_close(&window);
}

/* Exercise the calendar's real authenticated two-phase preview and native
 * Schedule command. The future UTC fixture cannot emit playback PCM. */
static JsonNode *calendar_schedule_markers(JsonNode *detail)
{
    JsonObject *media = json_object_get_object_member(json_node_get_object(detail), "media");
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body), *markers = json_object_new();
    json_object_set_string_member(object, "media_id", radios_json_string(media, "_id", ""));
    json_object_set_int_member(object, "expected_revision", radios_json_int(media, "_rev", 0));
    json_object_set_null_member(object, "expected_marker_revision");
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    const gchar *names[] = {"cue_in_ms", "intro_ms", "out_ms", "fade_start_ms", "fade_end_ms", "auto_next_ms", "overlap_ms"};
    const gint64 values[] = {0, 0, 6001, 4500, 6001, 4500, 350};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) json_object_set_int_member(markers, names[i], values[i]);
    json_object_set_boolean_member(markers, "automatic", TRUE); json_object_set_object_member(object, "markers", markers);
    JsonNode *saved = request("/v1/workflows/desktop-media/markers-save", body); json_node_free(body);
    JsonObject *verified = json_object_get_object_member(json_node_get_object(saved), "markers");
    g_assert_nonnull(verified); g_assert_true(json_object_get_boolean_member(verified, "automatic"));
    g_assert_cmpint(radios_json_int(verified, "overlap_ms", 0), ==, 350);
    return saved;
}
static JsonNode *calendar_schedule_config(const gchar *date, const gchar *playlist, guint mode)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *config = json_object_new(); json_node_take_object(node, config);
    JsonObject *definition = json_object_new(), *settings = json_object_new(), *source = json_object_new(), *event = json_object_new(), *assignment = json_object_new();
    JsonArray *definitions = json_array_new(), *sources = json_array_new(), *transitions = json_array_new(), *events = json_array_new(), *assignments = json_array_new();
    gchar *definition_id = g_uuid_string_random(), *source_id = g_uuid_string_random(), *event_id = g_uuid_string_random(), *assignment_id = g_uuid_string_random();
    json_object_set_int_member(config, "schema", 1);
    json_object_set_string_member(definition, "id", definition_id); json_object_set_string_member(definition, "name", "Native scheduled reusable rotation");
    json_object_set_string_member(settings, "selection_mode", "linear");
    const gchar *gaps[] = {"track_gap_minutes", "artist_gap_minutes", "album_gap_minutes"};
    for (guint i = 0; i < G_N_ELEMENTS(gaps); ++i) json_object_set_int_member(settings, gaps[i], 0);
    for (guint i = 0; i < 3; ++i) {
        JsonObject *transition = json_object_new(); json_object_set_int_member(transition, "mode", i);
        json_object_set_int_member(transition, "weight", i == mode ? 100 : 0);
        json_object_set_int_member(transition, "overlap_ms", i ? 500 : 0); json_array_add_object_element(transitions, transition);
    }
    json_object_set_array_member(settings, "transitions", transitions); json_object_set_object_member(definition, "settings", settings);
    json_object_set_string_member(source, "id", source_id); json_object_set_string_member(source, "playlist_id", playlist);
    json_object_set_int_member(source, "frequency", 1); json_object_set_int_member(source, "count", 2); json_object_set_int_member(source, "weight", 1);
    json_object_set_int_member(source, "start_minute", 0); json_object_set_int_member(source, "end_minute", 1440);
    json_object_set_int_member(source, "interval_minutes", 60); json_object_set_string_member(source, "media_kind", "music");
    const gchar *flags[] = {"random", "track_random", "advert_random", "sweeper_random"};
    for (guint i = 0; i < G_N_ELEMENTS(flags); ++i) json_object_set_boolean_member(source, flags[i], FALSE);
    json_array_add_object_element(sources, source); json_object_set_array_member(definition, "sources", sources);
    json_array_add_object_element(definitions, definition); json_object_set_array_member(config, "rotation_library", definitions);
    json_object_set_string_member(event, "id", event_id); json_object_set_string_member(event, "title", "Native future UTC calendar programme");
    json_object_set_string_member(event, "date", date); json_object_set_string_member(event, "color", "#abd6e4");
    json_object_set_int_member(event, "start_minute", 600); json_object_set_int_member(event, "end_minute", 601);
    json_object_set_string_member(assignment, "id", assignment_id); json_object_set_string_member(assignment, "rotation_id", definition_id);
    json_array_add_object_element(assignments, assignment); json_object_set_array_member(event, "rotations", assignments);
    json_array_add_object_element(events, event); json_object_set_array_member(config, "events", events);
    g_free(definition_id); g_free(source_id); g_free(event_id); g_free(assignment_id); return node;
}
static GtkWidget *calendar_preview_tree(GtkWidget *widget)
{
    if (GTK_IS_TREE_VIEW(widget)) return widget;
    if (!GTK_IS_CONTAINER(widget)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = calendar_preview_tree(item->data);
    g_list_free(children); return found;
}
static gboolean calendar_schedule_menu_visible(gpointer data)
{
    GtkWidget *item = data, *menu = gtk_widget_get_parent(item); GdkWindow *window = gtk_widget_get_window(menu);
    return gtk_widget_get_mapped(item) && window && gdk_window_is_viewable(window);
}
static void calendar_schedule_action(GtkWidget *page, const gchar *name)
{
    GtkWidget *tree = find(page, "automation-calendar-rotations"); g_assert_true(GTK_IS_TREE_VIEW(tree));
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree)); g_assert_cmpint(gtk_tree_model_iter_n_children(model, NULL), ==, 1);
    GtkTreePath *path = gtk_tree_path_new_from_indices(0, -1);
    gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(tree), path, NULL, FALSE, 0, 0); run_for(75);
    GdkRectangle rectangle; gtk_tree_view_get_cell_area(GTK_TREE_VIEW(tree), path, NULL, &rectangle); gtk_tree_path_free(path);
    GdkWindow *window = gtk_tree_view_get_bin_window(GTK_TREE_VIEW(tree)); g_assert_nonnull(window); g_assert_true(gdk_window_is_viewable(window));
    GdkEvent *event = gdk_event_new(GDK_BUTTON_PRESS); event->button.window = g_object_ref(window); event->button.send_event = TRUE;
    event->button.button = 3; event->button.x = MAX(12, rectangle.x + 4); event->button.y = rectangle.y + rectangle.height / 2.0;
    gint root_x, root_y; gdk_window_get_origin(window, &root_x, &root_y);
    event->button.x_root = root_x + event->button.x; event->button.y_root = root_y + event->button.y;
    GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(tree)); gdk_event_set_device(event, gdk_seat_get_pointer(seat));
    g_assert_true(gtk_widget_event(tree, event)); gdk_event_free(event); pump();
    GtkWidget *item = NULL;
    for (GList *menu = gtk_menu_get_for_attach_widget(tree); menu && !item; menu = menu->next) item = find(menu->data, name);
    g_assert_true(GTK_IS_MENU_ITEM(item)); g_assert_true(gtk_widget_is_sensitive(item));
    wait_until(calendar_schedule_menu_visible, item, "real calendar row context menu is viewable");
    gtk_menu_shell_activate_item(GTK_MENU_SHELL(gtk_widget_get_parent(item)), item, TRUE); pump();
}
static gboolean calendar_schedule_preview_open(gpointer data)
{ (void)data; return dialog_find("automation-calendar-preview-dialog") != NULL; }
static void calendar_schedule_unchanged(JsonNode *saved)
{
    JsonNode *actual = document_read("automation_calendar", "main"); g_assert_nonnull(actual);
    JsonObject *expected = json_node_get_object(saved), *record = json_node_get_object(actual);
    g_assert_cmpstr(radios_json_string(record, "_id", ""), ==, radios_json_string(expected, "_id", ""));
    g_assert_cmpint(radios_json_int(record, "_rev", 0), ==, radios_json_int(expected, "_rev", 0));
    g_assert_true(json_node_equal(json_object_get_member(record, "config"), json_object_get_member(expected, "config")));
    json_node_free(actual);
}
static void test_named_calendar_preview_schedule_transition_boundaries(void)
{
    Window window = window_open(); assert_gates_off(&window);
    JsonNode *first = media_import(&window, "Scheduled rotation canonical A", 600, 0.2, "music"),
             *second = media_import(&window, "Scheduled rotation canonical B", 1700, 0.2, "music");
    JsonNode *updated = calendar_schedule_markers(first); json_node_free(first); first = updated;
    updated = calendar_schedule_markers(second); json_node_free(second); second = updated;
    JsonNode *playlist = playlist_create("Scheduled reusable native PCM sources", first, second); window_close(&window);
    GDateTime *now = g_date_time_new_now_utc(), *tomorrow = g_date_time_add_days(now, 1);
    GDateTime *target = g_date_time_new_utc(g_date_time_get_year(tomorrow), g_date_time_get_month(tomorrow), g_date_time_get_day_of_month(tomorrow), 10, 0, 0);
    gchar *date = g_date_time_format(target, "%Y-%m-%d"), *shown = g_date_time_format(target, "%d.%m.%Y"); gint64 target_us = g_date_time_to_unix(target) * G_TIME_SPAN_SECOND;
    guint weekday = (guint)g_date_time_get_day_of_week(target) - 1; g_assert_cmpint(target_us, >, g_get_real_time());
    g_date_time_unref(now); g_date_time_unref(tomorrow); g_date_time_unref(target);
    const guint64 expected_overlaps[] = {0, 500, 350};
    for (guint mode = 0; mode < 3; ++mode) {
        JsonNode *configuration = calendar_schedule_config(date, radios_json_string(json_node_get_object(playlist), "_id", ""), mode);
        JsonNode *saved = document_save("automation_calendar", "main", json_node_get_object(configuration));
        window = window_open(); assert_gates_off(&window); navigate(&window, "automation");
        GtkWidget *page = find(window.root, "automation-calendar-main"); g_assert_nonnull(page); g_assert_true(gtk_widget_get_mapped(page));
        JsonNode *loaded = radios_automation_calendar_snapshot(page); g_assert_nonnull(loaded); g_assert_true(json_node_equal(loaded, configuration)); json_node_free(loaded);
        GtkWidget *date_entry = find(page, "automation-calendar-date"); g_assert_true(GTK_IS_ENTRY(date_entry));
        gtk_entry_set_text(GTK_ENTRY(date_entry), date); g_signal_emit_by_name(date_entry, "activate"); pump();
        radios_automation_calendar_select(page, weekday, 600, 601, FALSE); run_for(75);
        calendar_schedule_action(page, "automation-calendar-preview");
        wait_until(calendar_schedule_preview_open, NULL, "authenticated two-phase named calendar preview opens");
        GtkWidget *dialog = dialog_find("automation-calendar-preview-dialog"), *tree = calendar_preview_tree(dialog); g_assert_true(GTK_IS_TREE_VIEW(tree));
        GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree)); g_assert_cmpint(gtk_tree_model_iter_n_children(model, NULL), ==, 2);
        GtkTreeIter iter; g_assert_true(gtk_tree_model_get_iter_first(model, &iter));
        const gchar *titles[] = {"Scheduled rotation canonical A", "Scheduled rotation canonical B"};
        for (guint index = 0; index < 2; ++index) {
            gchar *title = NULL, *order = NULL, *kind = NULL; gtk_tree_model_get(model, &iter, 0, &order, 1, &title, 3, &kind, -1);
            g_assert_cmpstr(title, ==, titles[index]); g_assert_cmpstr(kind, ==, "music");
            /* The preview shows the calendar-local time (UTC here) as "dd.mm.yyyy HH:MM:SS". */
            gchar *prefix = g_strdup_printf("%u · %s 10:00:", index + 1, shown); g_assert_true(g_str_has_prefix(order, prefix)); g_free(prefix);
            if (!index) g_assert_nonnull(strstr(order, " 10:00:00"));
            g_free(title); g_free(order); g_free(kind); if (!index) g_assert_true(gtk_tree_model_iter_next(model, &iter));
        }
        if (mode == 2) saved_player_capture(dialog, "RADIOS_TEST_CALENDAR_PREVIEW_CAPTURE");
        g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001); calendar_schedule_unchanged(saved); assert_saved_playlist_unchanged(playlist);
        gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump();
        calendar_schedule_action(page, "automation-calendar-schedule");
        RadioSPlayoutSnapshot scheduled = run_wait(&window, RADIOS_PLAYOUT_SCHEDULED, 0, 0);
        g_assert_cmpuint(scheduled.total, ==, 2); g_assert_cmpint(scheduled.start_utc_us, ==, target_us);
        g_assert_cmpstr(scheduled.title, ==, titles[0]); g_assert_true(g_uuid_string_is_valid(scheduled.entry_id));
        g_assert_true(g_uuid_string_is_valid(scheduled.next_entry_id)); g_assert_cmpstr(scheduled.entry_id, !=, scheduled.next_entry_id);
        g_assert_cmpuint(scheduled.transition, ==, mode); g_assert_cmpuint(scheduled.overlap_ms, ==, expected_overlaps[mode]);
        RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
        RadioSAudioRunSnapshot actual; radios_audio_engine_runner_snapshot(engine, &actual);
        g_assert_cmpint(actual.state, ==, RADIOS_AUDIO_RUN_SCHEDULED); g_assert_cmpuint(actual.count, ==, 2); g_assert_cmpuint(actual.index, ==, 0);
        g_assert_cmpstr(actual.entry_id, ==, scheduled.entry_id); g_assert_cmpstr(actual.error, ==, ""); radios_audio_engine_unref(engine);
        g_test_message("Real calendar Schedule mode%u: count%u entry%s UTC%" G_GINT64_FORMAT " overlap%" G_GUINT64_FORMAT "ms; verified CUE marker350ms", mode, scheduled.total, scheduled.entry_id, scheduled.start_utc_us, scheduled.overlap_ms);
        g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001); g_assert_cmpfloat(measure("CUE").peak[0], <, 0.0001);
        if (mode == 2) { navigate(&window, "broadcast"); saved_player_capture(window.root, "RADIOS_TEST_CALENDAR_SCHEDULE_CAPTURE"); }
        stop_run(&window); g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001);
        calendar_schedule_unchanged(saved); assert_saved_playlist_unchanged(playlist); window_close(&window);
        json_node_free(saved); json_node_free(configuration);
    }
    g_free(date); g_free(shown); json_node_free(playlist); json_node_free(first); json_node_free(second);
}

static gboolean file_exists(gpointer path) { return g_file_test(path, G_FILE_TEST_EXISTS); }
static gchar *fixture_flag(const gchar *name)
{ gchar *directory = g_path_get_dirname(config.database_file), *path = g_build_filename(directory, name, NULL); g_free(directory); return path; }
static gboolean dialog_exists(gpointer name) { return dialog_find(name) != NULL; }
static void test_validation_lifecycle_cas(void)
{
    Window window = window_open(); JsonNode *detail = media_import(&window, "Lifecycle real tone", 900, 0.2, "music");
    JsonNode *cfg = programme_config(detail, NULL); GError *error = NULL;
    gchar *path = g_strdup(radios_json_string(json_object_get_object_member(json_node_get_object(detail), "version"), "managed_path", ""));
    gchar *bytes = NULL; gsize length = 0; g_assert_true(g_file_get_contents(path, &bytes, &length, NULL));
    bytes[length - 1] ^= 1; g_assert_true(g_file_set_contents(path, bytes, (gssize)length, NULL));
    require_call(radios_playout_ui_start_programme(window.ctx, window.root, json_node_get_object(cfg), 0, 0, 0, &error), &error);
    run_wait(&window, RADIOS_PLAYOUT_ERROR, 0, 0); g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001);
    bytes[length - 1] ^= 1; g_assert_true(g_file_set_contents(path, bytes, (gssize)length, NULL)); g_free(bytes); g_free(path);
    stop_run(&window); JsonNode *unbound = deep_copy(cfg);
    JsonObject *event = json_array_get_object_element(json_object_get_array_member(json_node_get_object(unbound), "events"), 0);
    const gchar *binding_keys[] = {"media_id", "version_id", "media_file_id", "media_sha256"};
    for (guint i = 0; i < G_N_ELEMENTS(binding_keys); ++i) json_object_remove_member(event, binding_keys[i]);
    json_object_set_string_member(event, "primary_file", "/external/unverified.wav");
    require_call(radios_playout_ui_start_programme(window.ctx, window.root, json_node_get_object(unbound), 0, 0, 0, &error), &error);
    run_wait(&window, RADIOS_PLAYOUT_ERROR, 0, 0); g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001); json_node_free(unbound);
    stop_run(&window); gchar *fail = fixture_flag(".acceptance-playout-prepare-fail"); g_assert_true(g_file_set_contents(fail, "fail", -1, NULL));
    require_call(radios_playout_ui_start_programme(window.ctx, window.root, json_node_get_object(cfg), 0, 0, 0, &error), &error);
    run_wait(&window, RADIOS_PLAYOUT_ERROR, 0, 0); g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001);
    g_assert_cmpint(g_unlink(fail), ==, 0); g_free(fail); stop_run(&window);
    gchar *hold = fixture_flag(".acceptance-playout-prepare-hold"), *ready = fixture_flag(".acceptance-playout-prepare-ready");
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL)); GtkWidget *owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(owner);
    require_call(radios_playout_ui_start_programme(window.ctx, owner, json_node_get_object(cfg), 0, 0, 0, &error), &error);
    wait_until(file_exists, ready, "actual computed prepare response pending after owner start");
    gtk_widget_destroy(owner); g_object_unref(owner); g_assert_cmpint(g_unlink(hold), ==, 0); g_assert_cmpint(g_unlink(ready), ==, 0);
    run_for(600); g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001);
    /* The queue shows the saved programme in the finite mode, as seed_programme selects. */
    GtkWidget *continuous = find(window.root, "programme-general-continuous");
    if (continuous) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(continuous), FALSE);
    JsonNode *saved = document_save("programme", "Mevcut program", json_node_get_object(cfg)); json_node_free(saved);
    click_label(window.root, "Verileri yenile");
    QueueCondition loaded = {&window, 1, "Lifecycle real tone"};
    wait_until(queue_ready, &loaded, "current programme arrives before local form edits");
    navigate(&window, "broadcast");
    /* Preserve a real user's field while the genuine native document receives
     * a CAS rejection, without relying on the removed toolbar editor. */
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Native document CAS input", window.ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT, "Kapat", GTK_RESPONSE_CANCEL, NULL);
    GtkWidget *name = gtk_entry_new(); gtk_widget_set_name(name, "programme-name");
    gtk_container_add(GTK_CONTAINER(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), name);
    gtk_widget_show_all(dialog); set(dialog, "programme-name", "Korunan kullanıcı girdisi");
    JsonNode *local = deep_copy(cfg);
    json_object_set_string_member(json_node_get_object(local), "title", gtk_entry_get_text(GTK_ENTRY(name)));
    JsonNode *remote = deep_copy(cfg); json_object_set_string_member(json_node_get_object(remote), "title", "Başka istemcinin kaydı");
    saved = document_save("programme", "Mevcut program", json_node_get_object(remote)); json_node_free(saved); json_node_free(remote);
    RadioSDocument *source = radios_broadcast_programme_document(window.ctx); g_assert_nonnull(source);
    g_assert_true(radios_data_document_save(source, local)); json_node_free(local);
    wait_until(dialog_exists, "database-reload-dialog", "real programme stale revision conflict");
    gtk_dialog_response(GTK_DIALOG(dialog_find("database-reload-dialog")), GTK_RESPONSE_CANCEL);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(dialog, "programme-name"))), ==, "Korunan kullanıcı girdisi");
    saved = document_read("programme", "Mevcut program");
    g_assert_cmpstr(radios_json_string(json_object_get_object_member(json_node_get_object(saved), "config"), "title", ""), ==, "Başka istemcinin kaydı");
    gtk_widget_destroy(dialog); json_node_free(saved);
    g_free(hold); g_free(ready); json_node_free(cfg); json_node_free(detail); window_close(&window);
}

static gboolean autodj_policy_saved(gpointer data)
{
    (void)data; JsonNode *record = document_read("automation", "main");
    JsonObject *configuration = record ? json_object_get_object_member(json_node_get_object(record), "config") : NULL;
    JsonArray *policies = configuration && json_object_has_member(configuration, "deckTransitions") ?
        json_object_get_array_member(configuration, "deckTransitions") : NULL;
    gboolean saved = policies && json_array_get_length(policies) == 2 &&
        fabs(json_object_get_double_member(json_array_get_object_element(policies, 0), "overlap") - 1.0) < 0.001;
    g_clear_pointer(&record, json_node_free);
    if (!saved) run_for(50);
    return saved;
}
static void test_autodj_deck_transition_save_playback(void)
{
    Window window = window_open();
    JsonNode *first = media_import(&window, "AutoDJ Deck A", 700, 0.2, "music");
    JsonNode *second = media_import(&window, "AutoDJ Deck B", 1700, 0.2, "music");
    JsonNode *first_playlist = playlist_create("AutoDJ A source", first, NULL);
    JsonNode *second_playlist = playlist_create("AutoDJ B source", second, NULL);
    JsonNode *configuration = automation_config(first_playlist, second_playlist, first_playlist);
    JsonObject *settings = json_node_get_object(configuration);
    json_object_set_boolean_member(settings, "jingleEnabled", FALSE);
    json_object_set_int_member(settings, "sourceMode", 0);
    json_object_set_int_member(settings, "trackMode", 1);
    json_object_set_int_member(settings, "previewCount", 1);
    JsonArray *policies = json_array_new();
    for (guint index = 0; index < 2; ++index) {
        JsonObject *policy = json_object_new();
        json_object_set_string_member(policy, "deck_id", index ? "2" : "1");
        json_object_set_int_member(policy, "transition", index ? 0 : 1);
        json_object_set_double_member(policy, "overlap", index ? 0 : 0.5);
        json_array_add_object_element(policies, policy);
    }
    json_object_set_array_member(settings, "deckTransitions", policies);
    JsonNode *saved = document_save("automation", "main", settings);
    gchar *saved_id = g_strdup(radios_json_string(json_node_get_object(saved), "_id", ""));
    gint64 initial_revision = radios_json_int(json_node_get_object(saved), "_rev", 0);
    g_assert_true(g_uuid_string_is_valid(saved_id)); g_assert_cmpint(initial_revision, >, 0); json_node_free(saved);
    saved = document_read("automation", "main"); g_assert_nonnull(saved);
    json_node_free(configuration); configuration = deep_copy(json_object_get_member(json_node_get_object(saved), "config"));
    json_node_free(saved); settings = json_node_get_object(configuration); policies = json_object_get_array_member(settings, "deckTransitions");
    g_assert_cmpuint(json_array_get_length(policies), ==, 2);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(policies, 0), "deck_id", ""), ==, "1");
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(policies, 1), "deck_id", ""), ==, "2");
    g_assert_cmpint(radios_json_int(json_array_get_object_element(policies, 0), "transition", -1), ==, 1);
    g_assert_cmpint(radios_json_int(json_array_get_object_element(policies, 1), "transition", -1), ==, 0);
    g_assert_cmpfloat(json_object_get_double_member(json_array_get_object_element(policies, 1), "overlap"), ==, 0);
    g_assert_null(find(window.root, "automation-deck-transition-1"));
    json_object_set_double_member(json_array_get_object_element(policies, 0), "overlap", 1.0);
    saved = document_save("automation", "main", settings);
    gint64 saved_revision = radios_json_int(json_node_get_object(saved), "_rev", 0);
    g_assert_cmpstr(radios_json_string(json_node_get_object(saved), "_id", ""), ==, saved_id);
    g_assert_cmpint(saved_revision, >, initial_revision); json_node_free(saved);
    wait_until(autodj_policy_saved, NULL, "public compatibility AutoDJ deck policy authenticated CAS is committed");
    window_close(&window); window = window_open();
    saved = document_read("automation", "main"); g_assert_nonnull(saved);
    g_assert_cmpstr(radios_json_string(json_node_get_object(saved), "_id", ""), ==, saved_id);
    g_assert_cmpint(radios_json_int(json_node_get_object(saved), "_rev", 0), ==, saved_revision);
    json_node_free(configuration); configuration = deep_copy(json_object_get_member(json_node_get_object(saved), "config"));
    json_node_free(saved); settings = json_node_get_object(configuration); policies = json_object_get_array_member(settings, "deckTransitions");
    g_assert_cmpfloat(json_object_get_double_member(json_array_get_object_element(policies, 0), "overlap"), ==, 1.0);
    JsonNode *preview = automation_preview(&window, settings);
    JsonArray *preview_entries = json_object_get_array_member(json_node_get_object(preview), "entries");
    g_assert_cmpuint(json_array_get_length(preview_entries), ==, 1);
    JsonObject *preview_first = json_array_get_object_element(preview_entries, 0);
    g_assert_cmpint(radios_json_int(preview_first, "transition", -1), ==, 1);
    g_assert_cmpint(radios_json_int(preview_first, "deck_index", -1), ==, 0);
    g_assert_cmpint(radios_json_int(preview_first, "overlap_ms", -1), ==, 1000); json_node_free(preview);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001); g_assert_cmpfloat(measure("CUE").peak[0], <, 0.0001);
    RadioSPlayoutSnapshot idle; g_assert_true(radios_playout_ui_get_snapshot(window.ctx, &idle)); g_assert_cmpint(idle.state, ==, RADIOS_PLAYOUT_IDLE);
    GError *error = NULL; require_call(radios_playout_ui_start_automation(window.ctx, window.root, settings, &error), &error);
    RadioSPlayoutSnapshot playing = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 200);
    card_metadata_wait(&window, 0, "AutoDJ Deck A", "Medya arşivi", "Sıradaki  AutoDJ Deck B");
    g_assert_cmpint(playing.deck, ==, 0); g_assert_cmpint(playing.transition, ==, 1); g_assert_cmpuint(playing.overlap_ms, ==, 1000);
    g_assert_true(playing.automation);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    RadioSAudioRunSnapshot leases; radios_audio_engine_runner_snapshot(engine, &leases); radios_audio_engine_unref(engine);
    g_assert_true(leases.owned[0]); g_assert_true(leases.owned[1]);
    for (guint deck = 2; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) g_assert_false(leases.owned[deck]);
    RadioSPlayoutSnapshot overlap = run_wait(&window, RADIOS_PLAYOUT_OVERLAP, 0, 0);
    g_assert_cmpint(overlap.deck, ==, 0); g_assert_cmpint(overlap.next_deck, ==, 1);
    card_metadata_wait(&window, 1, "AutoDJ Deck B", "Medya arşivi", "Sıradaki  AutoDJ Deck A");
    goffset captured = capture_offset("MAIN"); run_for(300);
    g_assert_cmpfloat(tone_energy("MAIN", captured, 700), >, 0.03);
    g_assert_cmpfloat(tone_energy("MAIN", captured, 1700), >, 0.03);
    playing = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 1500);
    g_assert_cmpint(playing.deck, ==, 1); g_assert_cmpint(playing.transition, ==, 0); g_assert_cmpuint(playing.overlap_ms, ==, 0);
    playing = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 2, 200);
    g_assert_cmpint(playing.deck, ==, 0); g_assert_cmpint(playing.transition, ==, 1); g_assert_cmpuint(playing.overlap_ms, ==, 1000);
    g_assert_true(playing.automation); g_assert_cmpint(playing.deck, ==, 0);
    g_assert_cmpfloat(measure("CUE").peak[0], <, 0.0001); stop_run(&window);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001);
    json_object_set_int_member(json_array_get_object_element(policies, 1), "transition", 3);
    require_call(radios_playout_ui_start_automation(window.ctx, window.root, settings, &error), &error);
    run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 200);
    playing = run_wait(&window, RADIOS_PLAYOUT_MANUAL_WAIT, 1, 0);
    g_assert_cmpint(playing.deck, ==, 1); g_assert_cmpint(playing.transition, ==, 3);
    run_for(300);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001);
    require_call(radios_playout_ui_action(window.ctx, RADIOS_PLAYOUT_NEXT, &error), &error);
    playing = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 2, 200);
    g_assert_cmpint(playing.deck, ==, 0); g_assert_cmpint(playing.transition, ==, 1);
    stop_run(&window);
    g_free(saved_id); json_node_free(configuration); json_node_free(first_playlist); json_node_free(second_playlist);
    json_node_free(first); json_node_free(second); window_close(&window);
}
static JsonNode *media_read(JsonNode *detail)
{
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "media_id",
        radios_json_string(json_object_get_object_member(json_node_get_object(detail), "media"), "_id", ""));
    JsonNode *current = request("/v1/workflows/desktop-media/get", body); json_node_free(body); return current;
}
static gboolean rhythm_saved(gpointer data)
{
    JsonNode *detail = data, *current = media_read(detail);
    gboolean saved = radios_json_int(json_node_get_object(current), "analysis_revision", 0) > radios_json_int(json_node_get_object(detail), "analysis_revision", 0);
    json_node_free(current); return saved;
}
static void rhythm_shot(Window *window, const gchar *basename)
{
    const gchar *directory = g_getenv("RADIOS_SHOT_DIR"); if (!directory || !*directory) return;
    GtkWidget *stack = gtk_widget_get_ancestor(find(window->root, "deck-0-title"), GTK_TYPE_STACK);
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_NONE); gtk_stack_set_visible_child_name(GTK_STACK(stack), "players");
    run_for(200); g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
    GdkPixbuf *picture = gdk_pixbuf_get_from_window(gtk_widget_get_window(window->root), 0, 0,
        gtk_widget_get_allocated_width(window->root), gtk_widget_get_allocated_height(window->root)); g_assert_nonnull(picture);
    gchar *path = g_build_filename(directory, basename, NULL);
    g_assert_true(gdk_pixbuf_save(picture, path, "png", NULL, NULL)); g_free(path); g_object_unref(picture);
}
static void rhythm_notice(gpointer unused, const gchar *message)
{ (void)unused; g_test_message("Rhythm UI: %s", message); }
static void players_show(Window *window)
{
    GtkWidget *stack = gtk_widget_get_ancestor(find(window->root, "deck-0-title"), GTK_TYPE_STACK);
    g_assert_true(GTK_IS_STACK(stack));
    gtk_stack_set_transition_type(GTK_STACK(stack), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_set_visible_child_name(GTK_STACK(stack), "players"); run_for(200);
}
static cairo_surface_t *widget_picture(GtkWidget *widget)
{
    g_assert_true(gtk_widget_is_drawable(widget));
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
        gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget));
    g_assert_cmpint(cairo_surface_status(surface), ==, CAIRO_STATUS_SUCCESS);
    cairo_t *context = cairo_create(surface); gtk_widget_draw(widget, context); cairo_destroy(context);
    cairo_surface_flush(surface); return surface;
}
static guint picture_changed_rows(cairo_surface_t *before, cairo_surface_t *after, gint x)
{
    gint height = cairo_image_surface_get_height(after), stride = cairo_image_surface_get_stride(after);
    const guchar *old_data = cairo_image_surface_get_data(before), *new_data = cairo_image_surface_get_data(after);
    guint changed = 0;
    for (gint y = 3; y < height - 3; ++y) {
        guint32 old_pixel, new_pixel;
        memcpy(&old_pixel, old_data + y * stride + x * 4, sizeof old_pixel);
        memcpy(&new_pixel, new_data + y * stride + x * 4, sizeof new_pixel);
        if (abs((gint)(old_pixel & 255) - (gint)(new_pixel & 255)) > 10 ||
            abs((gint)((old_pixel >> 8) & 255) - (gint)((new_pixel >> 8) & 255)) > 10 ||
            abs((gint)((old_pixel >> 16) & 255) - (gint)((new_pixel >> 16) & 255)) > 10) ++changed;
    }
    return changed;
}
static void waveform_marker_visible(cairo_surface_t *plain, cairo_surface_t *marked, gint64 time_ms, gint64 duration_ms)
{
    gint width = cairo_image_surface_get_width(marked), height = cairo_image_surface_get_height(marked);
    gint expected = (gint)llround((gdouble)time_ms * width / duration_ms); guint changed = 0;
    /* Allow the drawing's inner padding and subpixel antialiasing. A cue must
     * change a substantial vertical span near its actual media time. */
    for (gint x = MAX(0, expected - 16); x <= MIN(width - 1, expected + 16); ++x)
        changed = MAX(changed, picture_changed_rows(plain, marked, x));
    g_assert_cmpuint(changed, >=, (guint)height / 3);
}
static JsonNode *media_markers_for_waveform(JsonNode *detail)
{
    JsonObject *media = json_object_get_object_member(json_node_get_object(detail), "media");
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body), *markers = json_object_new();
    json_object_set_string_member(object, "media_id", radios_json_string(media, "_id", ""));
    json_object_set_int_member(object, "expected_revision", radios_json_int(media, "_rev", 0));
    json_object_set_null_member(object, "expected_marker_revision");
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    const gchar *names[] = {"cue_in_ms", "intro_ms", "out_ms", "fade_start_ms", "fade_end_ms", "auto_next_ms", "overlap_ms"};
    const gint64 values[] = {1000, 2000, 5500, 4500, 5000, 5000, 0};
    for (guint index = 0; index < G_N_ELEMENTS(names); ++index) json_object_set_int_member(markers, names[index], values[index]);
    json_object_set_boolean_member(markers, "automatic", FALSE); json_object_set_object_member(object, "markers", markers);
    JsonNode *saved = request("/v1/workflows/desktop-media/markers-save", body); json_node_free(body); return saved;
}
static void player_square_and_controls(Window *window)
{
    GtkWidget *card = g_object_get_data(G_OBJECT(window->ctx->parent), "radios-deck-owner-0");
    gint width = gtk_widget_get_allocated_width(card), height = gtk_widget_get_allocated_height(card);
    g_assert_cmpint(width, >, 250); g_assert_cmpint(height, >, 250);
    g_assert_cmpfloat(fabs((gdouble)width - height) / MAX(width, height), <=, 0.05);
    /* Square allocation is insufficient if the box overflows its visible
     * controls. Every mapped row must fit inside the actual card bounds. */
    GList *children = gtk_container_get_children(GTK_CONTAINER(card));
    for (GList *item = children; item; item = item->next) {
        GtkWidget *child = item->data; if (!gtk_widget_get_mapped(child)) continue;
        gint x = 0, y = 0, child_width = gtk_widget_get_allocated_width(child), child_height = gtk_widget_get_allocated_height(child);
        g_assert_true(gtk_widget_translate_coordinates(child, card, 0, 0, &x, &y));
        if (x < -2 || y < -2 || x + child_width > width + 2 || y + child_height > height + 2)
            g_test_message("Square card child '%s': %d,%d %dx%d inside %dx%d", gtk_widget_get_name(child), x, y, child_width, child_height, width, height);
        g_assert_cmpint(x, >=, -2); g_assert_cmpint(y, >=, -2);
        g_assert_cmpint(x + child_width, <=, width + 2); g_assert_cmpint(y + child_height, <=, height + 2);
    }
    g_list_free(children);
    g_assert_null(find(card, "deck-0-analyze")); g_assert_null(find(card, "deck-0-sync-master"));
    GtkWidget *sync = find(card, "deck-0-sync"), *bpm = find(card, "deck-0-bpm");
    GtkWidget *row = gtk_widget_get_parent(sync);
    g_assert_true(GTK_IS_BOX(row)); g_assert_true(gtk_widget_get_parent(bpm) == row);
    gint bpm_x = 0, bpm_y = 0, sync_x = 0, sync_y = 0, text_x = 0, text_y = 0;
    GtkAllocation allocation; PangoRectangle ink, logical;
    g_assert_true(gtk_widget_translate_coordinates(bpm, card, 0, 0, &bpm_x, &bpm_y));
    g_assert_true(gtk_widget_translate_coordinates(sync, card, 0, 0, &sync_x, &sync_y));
    gtk_widget_get_allocation(bpm, &allocation);
    gtk_label_get_layout_offsets(GTK_LABEL(bpm), &text_x, &text_y);
    pango_layout_get_pixel_extents(gtk_label_get_layout(GTK_LABEL(bpm)), &ink, &logical);
    /* GtkLabel's layout offsets include its parent-window allocation origin.
     * Convert to label-relative coordinates before adding the card origin.
     * Measure text bounds so an expanded label cannot hide an empty gap. */
    gint gap = sync_x - (bpm_x + text_x - allocation.x + logical.x + logical.width);
    if (gap < 0 || gap > 8) {
        g_test_message("BPM '%s' text='%s' allocation=%d,%d %dx%d card=%d,%d layout=%d,%d width=%d ink=%d,%d %dx%d logical=%d,%d %dx%d SYNC card=%d,%d %dx%d gap=%d",
            gtk_widget_get_name(bpm), gtk_label_get_text(GTK_LABEL(bpm)), allocation.x, allocation.y, allocation.width, allocation.height,
            bpm_x, bpm_y, text_x, text_y, logical.width, ink.x, ink.y, ink.width, ink.height, logical.x, logical.y, logical.width, logical.height,
            sync_x, sync_y, gtk_widget_get_allocated_width(sync), gtk_widget_get_allocated_height(sync), gap);
        rhythm_shot(window, "player-bpm-gap-debug.png");
    }
    g_assert_cmpint(gap, >=, 0); g_assert_cmpint(gap, <=, 8);
    GList *controls = gtk_container_get_children(GTK_CONTAINER(row));
    g_assert_cmpint(abs(g_list_index(controls, sync) - g_list_index(controls, bpm)), ==, 1); g_list_free(controls);
    GtkWidget *waveform = find(card, "deck-0-waveform");
    g_assert_true(GTK_IS_DRAWING_AREA(waveform)); g_assert_cmpint(gtk_widget_get_allocated_height(waveform), >=, 60);
    g_assert_cmpint(gtk_widget_get_allocated_height(waveform), <=, 100);
}
static gboolean player_descendants_contained(GtkWidget *widget, GtkWidget *card)
{
    gboolean contained = TRUE;
    if (gtk_widget_get_mapped(widget)) {
        gint x = 0, y = 0;
        if (gtk_widget_translate_coordinates(widget, card, 0, 0, &x, &y)) {
            gint width = gtk_widget_get_allocated_width(widget), height = gtk_widget_get_allocated_height(widget);
            if (x < -2 || y < -2 || x + width > gtk_widget_get_allocated_width(card) + 2 ||
                y + height > gtk_widget_get_allocated_height(card) + 2) {
                g_test_message("Player descendant '%s' allocation %d,%d %dx%d exceeds card %dx%d",
                    gtk_widget_get_name(widget), x, y, width, height,
                    gtk_widget_get_allocated_width(card), gtk_widget_get_allocated_height(card));
                contained = FALSE;
            }
        }
    }
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *item = children; item; item = item->next)
            contained = player_descendants_contained(item->data, card) && contained;
        g_list_free(children);
    }
    return contained;
}
static void player_geometry_report(GtkWidget *card, const gchar *phase)
{
    gint minimum = 0, natural = 0;
    gtk_widget_get_preferred_width(card, &minimum, &natural);
    g_test_message("Player geometry %s: card %dx%d preferred width %d/%d", phase,
        gtk_widget_get_allocated_width(card), gtk_widget_get_allocated_height(card), minimum, natural);
    const gchar *names[] = {"deck-0-title", "deck-0-metadata", "deck-0-next-title", "deck-0-waveform"};
    for (guint index = 0; index < G_N_ELEMENTS(names); ++index) {
        GtkWidget *widget = find(card, names[index]); g_assert_nonnull(widget);
        gtk_widget_get_preferred_width(widget, &minimum, &natural);
        g_test_message("Player geometry %s/%s: %dx%d preferred width %d/%d lines %d", phase, names[index],
            gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget), minimum, natural,
            GTK_IS_LABEL(widget) ? pango_layout_get_line_count(gtk_label_get_layout(GTK_LABEL(widget))) : 0);
    }
}
static void test_player_waveform_cues_square_automatic_analysis(void)
{
    Window window = window_open(); seed_programme(&window, NULL, NULL); navigate(&window, "broadcast");
    window.ctx->notify = rhythm_notice; players_show(&window); player_square_and_controls(&window);
    GtkWidget *card = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0");
    player_geometry_report(card, "empty");
    GtkWidget *waveform = find(card, "deck-0-waveform"); cairo_surface_t *empty = widget_picture(waveform);
    JsonNode *legacy = media_import_rhythm_format(&window, "Waveform without beatgrid", 700, 0.25, "music", 0, TRUE);
    g_assert_false(json_object_has_member(json_object_get_object_member(json_node_get_object(legacy), "analysis"), "rhythm"));
    GError *error = NULL;
    require_call(radios_audio_ui_load_media(window.ctx, card, json_node_get_object(legacy), 0, FALSE, &error), &error);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    wait_until(rhythm_saved, legacy, "unknown-BPM legacy source is analyzed once without a button"); run_for(200);
    JsonNode *analyzed = media_read(legacy);
    JsonObject *rhythm = json_object_get_object_member(json_object_get_object_member(json_node_get_object(analyzed), "analysis"), "rhythm");
    g_assert_cmpfloat(json_object_get_double_member(rhythm, "bpm"), ==, 0);
    g_assert_false(gtk_widget_is_sensitive(find(card, "deck-0-sync")));
    cairo_surface_t *plain = widget_picture(waveform);
    g_assert_cmpint(cairo_image_surface_get_width(empty), ==, cairo_image_surface_get_width(plain));
    g_assert_cmpint(cairo_image_surface_get_height(empty), ==, cairo_image_surface_get_height(plain));
    guint populated = 0; gint width = cairo_image_surface_get_width(plain);
    for (gint x = 0; x < width; ++x) if (picture_changed_rows(empty, plain, x) >= 5) ++populated;
    g_assert_cmpuint(populated, >, (guint)width / 2); cairo_surface_destroy(empty);
    JsonNode *marked = media_markers_for_waveform(analyzed);
    require_call(radios_audio_ui_load_media(window.ctx, card, json_node_get_object(marked), 0, FALSE, &error), &error);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 1000); run_for(200);
    gchar *tooltip = gtk_widget_get_tooltip_text(waveform);
    gchar *cue_text = g_strdup_printf("Cue giriş: %.3f s", 1.0), *out_text = g_strdup_printf("Çıkış: %.3f s", 5.5);
    g_assert_nonnull(tooltip); g_assert_nonnull(strstr(tooltip, cue_text));
    g_assert_nonnull(strstr(tooltip, out_text)); g_free(tooltip);
    const gchar *description = atk_object_get_description(gtk_widget_get_accessible(waveform));
    g_assert_nonnull(description); g_assert_nonnull(strstr(description, cue_text));
    g_free(cue_text); g_free(out_text);
    player_square_and_controls(&window);
    cairo_surface_t *picture = widget_picture(waveform);
    gint64 duration = radios_json_int(json_object_get_object_member(json_node_get_object(marked), "version"), "duration_ms", 0);
    waveform_marker_visible(plain, picture, 1000, duration); waveform_marker_visible(plain, picture, 5500, duration);
    cairo_surface_destroy(plain); cairo_surface_destroy(picture);
    run_for(750); JsonNode *stable = media_read(marked);
    g_assert_cmpint(radios_json_int(json_node_get_object(stable), "analysis_revision", 0), ==,
        radios_json_int(json_node_get_object(analyzed), "analysis_revision", 0));
    json_node_free(stable); rhythm_shot(&window, "player-waveform-cues.png");
    gtk_window_resize(GTK_WINDOW(window.root), 1440, 1000); run_for(200); player_square_and_controls(&window);
    gtk_window_resize(GTK_WINDOW(window.root), 1100, 800); run_for(200); player_square_and_controls(&window);
    player_geometry_report(card, "ordinary-loaded");
    gint ordinary_width = gtk_widget_get_allocated_width(card), ordinary_height = gtk_widget_get_allocated_height(card);
    rhythm_shot(&window, "player-layout-ordinary.png");
    const gchar *multiline_title = "Uzun Türkçe şarkı başlığı — İstanbul İzmir Ankara\n"
        "İkinci satır — Çeşme Şişli Üsküdar\nÜçüncü satır\nDördüncü satır\nBeşinci satır\nAltıncı satır";
    JsonNode *multiline = media_import(&window, multiline_title, 1700, 0.25, "music");
    multiline = media_metadata_save(multiline,
        "Uzun sanatçı adı — İstanbul\nİkinci sanatçı satırı\nÜçüncü sanatçı satırı",
        "Uzun albüm adı — İzmir\nİkinci albüm satırı\nÜçüncü albüm satırı");
    require_call(radios_audio_ui_load_media(window.ctx, card, json_node_get_object(multiline), 0, FALSE, &error), &error);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0); run_for(300);
    player_geometry_report(card, "multiline-loaded");
    rhythm_shot(&window, "player-layout-multiline.png");
    gchar *full_title = gtk_widget_get_tooltip_text(find(card, "deck-0-title"));
    g_assert_cmpstr(full_title, ==, multiline_title); g_free(full_title);
    g_assert_cmpint(gtk_widget_get_allocated_width(card), ==, ordinary_width);
    g_assert_cmpint(gtk_widget_get_allocated_height(card), ==, ordinary_height);
    g_assert_true(player_descendants_contained(card, card)); player_square_and_controls(&window);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, 0.0001);
    json_node_free(multiline);
    json_node_free(legacy); json_node_free(analyzed); json_node_free(marked); window_close(&window);
}
static void waveform_native_motion(GtkWidget *widget, gdouble ratio)
{
    GdkEvent *event = gdk_event_new(GDK_MOTION_NOTIFY); GdkWindow *window = gtk_widget_get_window(widget);
    g_assert_nonnull(window); event->motion.window = g_object_ref(window); event->motion.send_event = TRUE;
    event->motion.x = 8 + MAX(1, gtk_widget_get_allocated_width(widget) - 16) * ratio;
    event->motion.y = gtk_widget_get_allocated_height(widget) / 2.0;
    gint x, y; gdk_window_get_origin(window, &x, &y); event->motion.x_root = x + event->motion.x; event->motion.y_root = y + event->motion.y;
    GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(widget)); gdk_event_set_device(event, gdk_seat_get_pointer(seat));
    g_assert_true(gtk_widget_event(widget, event)); gdk_event_free(event); pump();
}
static void test_player_waveform_zoom_pan_inspection_no_transport_or_db_write(void)
{
    Window window = window_open(); seed_programme(&window, NULL, NULL); navigate(&window, "broadcast"); players_show(&window);
    GtkWidget *card = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0"); g_assert_nonnull(card);
    GtkWidget *waveform = find(card, "deck-0-waveform"), *pan = find(card, "deck-0-waveform-pan");
    g_assert_true(GTK_IS_DRAWING_AREA(waveform)); g_assert_true(GTK_IS_SCROLLBAR(pan));
    g_assert_false(gtk_widget_is_sensitive(find(card, "deck-0-waveform-zoom-in")));
    g_assert_false(gtk_widget_is_sensitive(find(card, "deck-0-waveform-inspect"))); g_assert_false(gtk_widget_get_visible(pan));
    JsonNode *detail = media_import(&window, "Native zoom measured waveform", 700, 0.25, "music");
    JsonNode *marked = media_markers_for_waveform(detail); json_node_free(detail); GError *error = NULL;
    require_call(radios_audio_ui_load_media(window.ctx, card, json_node_get_object(marked), 0, FALSE, &error), &error);
    RadioSAudioUiSnapshot before = deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 1000); run_for(200);
    JsonNode *baseline = media_read(marked); JsonObject *known = json_node_get_object(baseline);
    gint64 duration = radios_json_int(json_object_get_object_member(known, "version"), "duration_ms", 0);
    GtkAdjustment *range = gtk_range_get_adjustment(GTK_RANGE(pan));
    g_assert_cmpfloat(fabs(gtk_adjustment_get_page_size(range) - duration), <, .001);
    g_assert_cmpfloat(gtk_adjustment_get_value(range), ==, 0); cairo_surface_t *full = widget_picture(waveform);
    click(card, "deck-0-waveform-zoom-in"); run_for(75);
    g_assert_true(gtk_widget_get_visible(pan)); g_assert_cmpfloat(fabs(gtk_adjustment_get_page_size(range) - duration / 2.0), <, .001);
    gtk_adjustment_set_value(range, 250); run_for(75); g_assert_cmpfloat(gtk_adjustment_get_value(range), ==, 250);
    cairo_surface_t *zoomed = widget_picture(waveform);
    gint width = cairo_image_surface_get_width(zoomed), height = cairo_image_surface_get_height(zoomed);
    gint cue_x = (gint)llround(8 + (width - 16) * (1000 - 250) / (duration / 2.0)); guint vertical = 0;
    for (gint x = MAX(0, cue_x - 2); x <= MIN(width - 1, cue_x + 2); ++x) vertical = MAX(vertical, picture_changed_rows(full, zoomed, x));
    /* Cue lines span the drawn band below the two marker-label lanes, as
     * waveform_marker_visible() expects for the unzoomed view. */
    g_assert_cmpuint(vertical, >=, (guint)height / 3); cairo_surface_destroy(full); cairo_surface_destroy(zoomed);
    waveform_native_motion(waveform, .5);
    gchar *pointer = g_strdup_printf("İmleç: %.3f s", (250 + duration / 4.0) / 1000);
    GtkWidget *time = find(card, "deck-0-waveform-time"); g_assert_true(GTK_IS_LABEL(time));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(time)), ==, pointer); g_free(pointer);
    saved_player_capture(window.root, "RADIOS_TEST_WAVEFORM_ZOOM_CAPTURE");
    click(card, "deck-0-waveform-inspect");
    wait_until(dialog_exists, "deck-0-waveform-dialog", "actual large measured waveform inspection dialog");
    GtkWidget *dialog = dialog_find("deck-0-waveform-dialog"), *inspection = find(dialog, "qwf-timeline");
    g_assert_true(GTK_IS_DRAWING_AREA(inspection)); wait_until(saved_player_widget_mapped, inspection, "actual inspection drawing is mapped"); run_for(75);
    g_assert_cmpint(gtk_widget_get_allocated_width(inspection), >, gtk_widget_get_allocated_width(waveform));
    g_assert_cmpint(gtk_widget_get_allocated_height(inspection), >=, 180);
    /* The inspector hosts the shared marker editor beside the deck's beat
     * grid; the editor's own zoom leaves the card's view untouched. */
    g_assert_nonnull(find(dialog, "qwf-set-8")); g_assert_nonnull(find(dialog, "qwf-markers-save")); g_assert_nonnull(find(dialog, "deck-0-grid-bpm"));
    click(dialog, "qwf-zoom-in"); run_for(75);
    g_assert_cmpfloat(fabs(gtk_adjustment_get_page_size(range) - duration / 2.0), <, .001);
    click(dialog, "qwf-fit"); run_for(75);
    g_assert_cmpfloat(gtk_adjustment_get_value(range), ==, 250); g_assert_true(gtk_widget_get_visible(pan));
    saved_player_capture(dialog, "RADIOS_TEST_WAVEFORM_INSPECTION_CAPTURE");
    RadioSAudioUiSnapshot after; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 0, &after));
    g_assert_cmpint(after.state, ==, RADIOS_AUDIO_UI_READY); g_assert_cmpuint(after.generation, ==, before.generation);
    g_assert_cmpuint(after.position_ms, ==, before.position_ms); g_assert_cmpstr(after.media_id, ==, before.media_id);
    g_assert_cmpstr(after.version_id, ==, before.version_id); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    JsonNode *actual = media_read(marked); JsonObject *record = json_node_get_object(actual);
    g_assert_cmpint(radios_json_int(json_object_get_object_member(record, "media"), "_rev", 0), ==, radios_json_int(json_object_get_object_member(known, "media"), "_rev", 0));
    g_assert_cmpint(radios_json_int(record, "analysis_revision", 0), ==, radios_json_int(known, "analysis_revision", 0));
    g_assert_true(json_object_equal(json_object_get_object_member(record, "markers"), json_object_get_object_member(known, "markers")));
    g_assert_true(json_object_equal(json_object_get_object_member(record, "version"), json_object_get_object_member(known, "version")));
    json_node_free(actual); click(card, "deck-0-waveform-zoom-in"); run_for(75);
    JsonNode *replacement = media_import(&window, "Native zoom identity replacement", 1700, 0.25, "music");
    require_call(radios_audio_ui_load_media(window.ctx, card, json_node_get_object(replacement), 0, FALSE, &error), &error);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0); run_for(200);
    g_assert_null(dialog_find("deck-0-waveform-dialog")); g_assert_cmpfloat(gtk_adjustment_get_value(range), ==, 0);
    g_assert_cmpfloat(fabs(gtk_adjustment_get_page_size(range) - radios_json_int(json_object_get_object_member(json_node_get_object(replacement), "version"), "duration_ms", 0)), <, .001);
    g_assert_false(gtk_widget_get_visible(pan)); player_square_and_controls(&window);
    g_test_message("Measured native waveform zoom/pan/pointer/inspection preserve transport generation and actual media/marker DB revisions; replacement resets view.");
    json_node_free(replacement); json_node_free(baseline); json_node_free(marked); window_close(&window);
}

/* SYNC follows the peer chosen in the card's SYNC selector, which lists only
 * analyzed peers in Play. */
typedef struct { GtkWidget *combo; const gchar *id; } SyncTargetChoice;
static gboolean sync_target_chosen(gpointer value)
{
    SyncTargetChoice *choice = value;
    return gtk_widget_is_sensitive(choice->combo) && gtk_combo_box_set_active_id(GTK_COMBO_BOX(choice->combo), choice->id);
}
typedef struct { RadioSViewContext *ctx; guint deck; gint master; gdouble bpm; } SyncedDeck;
static gboolean deck_synced(gpointer value)
{
    SyncedDeck *expected = value; RadioSAudioUiSnapshot snapshot = {0};
    return radios_audio_ui_get_snapshot(expected->ctx, expected->deck, &snapshot) && snapshot.sync_master == expected->master &&
        fabs(snapshot.effective_bpm - expected->bpm) < .01;
}
/* Workspace players keep their AutoDJ mode/target in the saved player
 * definitions (live_players). Poll the real DB at a modest rate. */
static void live_players_target_wait(const gchar *player, gint64 target)
{
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND; gboolean saved = FALSE;
    while (!saved && g_get_monotonic_time() < deadline) {
        JsonNode *record = document_read("live_players", "main");
        JsonObject *config = record ? json_object_get_object_member(json_node_get_object(record), "config") : NULL;
        JsonObject *players = config && json_object_has_member(config, "players") ? json_object_get_object_member(config, "players") : NULL;
        JsonObject *settings = players && json_object_has_member(players, player) ? json_object_get_object_member(players, player) : NULL;
        saved = settings && radios_json_int(settings, "autodj_target", 0) == target &&
            radios_json_int(settings, "autodj_mode", 0) == RADIOS_AUTODJ_TRANSITIONS;
        if (record) json_node_free(record);
        if (!saved) run_for(100);
    }
    g_assert_true(saved);
}
/* Keep the existing generic pair DSP regression separate from the new
 * rotation-only card control. A raw-media click must be inert at that UI gate. */
static void player_generic_pair_start(Window *window, GtkWidget *owner, JsonNode *first, JsonNode *second, guint source, guint target)
{
    gchar *name = g_strdup_printf("deck-%u-autodj-start", source); GtkWidget *button = find(window->root, name); g_free(name);
    g_assert_true(GTK_IS_BUTTON(button)); g_assert_false(gtk_widget_is_sensitive(button));
    guint64 intent = radios_playout_ui_intent(window->ctx); RadioSAudioUiSnapshot before = {0}, after = {0};
    g_assert_true(radios_audio_ui_get_snapshot(window->ctx, source, &before));
    gtk_button_clicked(GTK_BUTTON(button)); run_for(75);
    g_assert_cmpuint(radios_playout_ui_intent(window->ctx), ==, intent);
    g_assert_true(radios_audio_ui_get_snapshot(window->ctx, source, &after));
    g_assert_cmpuint(after.generation, ==, before.generation); g_assert_cmpint(after.state, ==, before.state);
    JsonArray *inputs = json_array_new(); JsonNode *details[] = {first, second};
    for (guint i = 0; i < G_N_ELEMENTS(details); ++i) {
        JsonObject *detail = json_node_get_object(details[i]), *media = json_object_get_object_member(detail, "media");
        JsonObject *input = json_object_new(); bind(input, detail);
        const gchar *keys[] = {"title", "artist", "album"};
        for (guint j = 0; j < G_N_ELEMENTS(keys); ++j) json_object_set_string_member(input, keys[j], radios_json_string(media, keys[j], ""));
        json_object_set_int_member(input, "duration_ms", radios_json_int(json_object_get_object_member(detail, "version"), "duration_ms", 0));
        json_array_add_object_element(inputs, input);
    }
    GError *error = NULL; require_call(radios_playout_ui_start_player_autodj(window->ctx, owner, inputs, source, target, &error), &error); json_array_unref(inputs);
}

static void test_player_sync_and_pair_autodj(void)
{
    Window window = window_open(); seed_programme(&window, NULL, NULL); navigate(&window, "broadcast");
    window.ctx->notify = rhythm_notice;
    JsonNode *first = media_import_rhythm_format(&window, "Beatgrid A", 700, 0.25, "music", 120, TRUE);
    JsonNode *second = media_import_rhythm(&window, "Beatgrid B", 1700, 0.25, "music", 128);
    JsonNode *other = media_import(&window, "Untouched deck", 2600, 0.15, "music");
    RadioSRhythmAnalysis grids[2];
    g_assert_false(json_object_has_member(json_object_get_object_member(json_node_get_object(first), "analysis"), "rhythm"));
    g_assert_true(radios_rhythm_analysis_read(json_object_get_object_member(json_object_get_object_member(json_node_get_object(second), "analysis"), "rhythm"), &grids[1]));
    g_assert_cmpfloat(fabs(grids[1].bpm - 128), <, 0.5);
    GError *error = NULL;
    GtkWidget *source = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0");
    GtkWidget *target = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-2");
    g_assert_nonnull(source); g_assert_nonnull(target);
    g_assert_false(gtk_widget_get_visible(find(window.root, "deck-0-inputs")));
    g_assert_false(gtk_widget_get_visible(g_object_get_data(G_OBJECT(source), "radios-deck-main-widget")));
    require_call(radios_audio_ui_load_media(window.ctx, source, json_node_get_object(first), 0, FALSE, &error), &error);
    require_call(radios_audio_ui_load_media(window.ctx, target, json_node_get_object(second), 2, FALSE, &error), &error);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0); deck_wait(&window, 2, RADIOS_AUDIO_UI_READY, 0); run_for(200);
    wait_until(rhythm_saved, first, "loading legacy media automatically persists actual rhythm through HTTP CAS");
    JsonNode *analyzed = media_read(first);
    g_assert_true(radios_rhythm_analysis_read(json_object_get_object_member(json_object_get_object_member(json_node_get_object(analyzed), "analysis"), "rhythm"), &grids[0]));
    g_assert_cmpfloat(fabs(grids[0].bpm - 120), <, 0.5);
    json_node_free(first); first = analyzed; run_for(200);
    g_assert_null(find(window.root, "deck-2-sync-master")); g_assert_null(find(window.root, "deck-0-analyze"));
    combo(window.root, "deck-2-autodj-target", "0");
    /* SYNC is refused until both players are in Play; then it follows the
     * peer chosen in the SYNC selector. */
    toggle(window.root, "deck-2-sync", TRUE); run_for(150);
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(window.root, "deck-2-sync"))));
    click(window.root, "deck-0-play"); click(window.root, "deck-2-play");
    deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100); deck_wait(&window, 2, RADIOS_AUDIO_UI_PLAYING, 100);
    SyncTargetChoice peer = {find(window.root, "deck-2-sync-target"), "0"}; g_assert_true(GTK_IS_COMBO_BOX(peer.combo));
    wait_until(sync_target_chosen, &peer, "playing analyzed peer is offered as the SYNC target");
    toggle(window.root, "deck-2-sync", TRUE);
    SyncedDeck follower = {window.ctx, 2, 0, grids[0].bpm}; wait_until(deck_synced, &follower, "SYNC matches the chosen playing peer");
    RadioSAudioUiSnapshot synced = {0}; radios_audio_ui_get_snapshot(window.ctx, 2, &synced);
    g_assert_cmpint(synced.sync_master, ==, 0); g_assert_cmpfloat(fabs(synced.effective_bpm - grids[0].bpm), <, 0.01);
    g_assert_cmpfloat(synced.tempo_ratio, <, 1); run_for(500);
    goffset pcm = capture_offset("MAIN"); run_for(400);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, 0.07);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700 * synced.tempo_ratio), <, 0.03);
    click(window.root, "deck-0-stop"); click(window.root, "deck-2-stop"); run_for(200);
    toggle(window.root, "deck-2-sync", FALSE); deck_wait(&window, 2, RADIOS_AUDIO_UI_READY, 0);
    card_metadata_wait(&window, 0, "Beatgrid A", "Medya arşivi", "Sıradaki  —");
    card_metadata_wait(&window, 2, "Beatgrid B", "Medya arşivi", "Sıradaki  —");
    JsonNode *stable = media_read(first);
    g_assert_cmpint(radios_json_int(json_node_get_object(stable), "analysis_revision", 0), ==,
        radios_json_int(json_node_get_object(first), "analysis_revision", 0));
    json_node_free(stable);
    stable = media_read(second);
    g_assert_cmpint(radios_json_int(json_node_get_object(stable), "analysis_revision", 0), ==,
        radios_json_int(json_node_get_object(second), "analysis_revision", 0));
    json_node_free(stable);
    combo(window.root, "deck-0-autodj-target", "2");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(window.root, "deck-2-autodj-target"))), ==, "0");
    wait_until(window_ready, &window, "AutoDJ choices saved before reopen");
    live_players_target_wait("1", 3); live_players_target_wait("3", 1);
    rhythm_shot(&window, "player-sync-autodj.png");
    window_close(&window); window = window_open(); navigate(&window, "broadcast"); run_for(200);
    window.ctx->notify = rhythm_notice;
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(window.root, "deck-0-autodj-target"))), ==, "2");
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(window.root, "deck-2-sync"))));
    source = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0"); target = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-2");
    require_call(radios_audio_ui_load_media(window.ctx, source, json_node_get_object(first), 0, FALSE, &error), &error);
    require_call(radios_audio_ui_load_media(window.ctx, target, json_node_get_object(second), 2, FALSE, &error), &error);
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(other), 1, FALSE, &error), &error);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0); deck_wait(&window, 2, RADIOS_AUDIO_UI_READY, 0); deck_wait(&window, 1, RADIOS_AUDIO_UI_READY, 0);
    click(window.root, "deck-1-play"); click(window.root, "deck-0-play"); run_for(200);
    RadioSAudioUiSnapshot before = {0}; radios_audio_ui_get_snapshot(window.ctx, 0, &before);
    player_generic_pair_start(&window, source, first, second, 0, 2); RadioSPlayoutSnapshot run = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 400);
    radios_audio_ui_get_snapshot(window.ctx, 0, &synced); g_assert_cmpuint(synced.generation, ==, before.generation);
    g_assert_cmpint(run.deck, ==, 0); g_assert_cmpint(run.next_deck, ==, 2);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); RadioSAudioRunSnapshot lease;
    radios_audio_engine_runner_snapshot(engine, &lease); g_assert_true(lease.owned[0]); g_assert_true(lease.owned[2]); g_assert_false(lease.owned[1]);
    pcm = capture_offset("MAIN"); run_for(350); g_assert_cmpfloat(tone_energy("MAIN", pcm, 2600), >, 0.1);
    click(window.root, "deck-0-pause"); run_wait(&window, RADIOS_PLAYOUT_PAUSED, 0, 0);
    click(window.root, "deck-2-play"); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 400);
    run = run_wait(&window, RADIOS_PLAYOUT_OVERLAP, 0, 0); g_assert_cmpint(run.next_deck, ==, 2);
    card_metadata_wait(&window, 2, "Beatgrid B", "Medya arşivi", "Sıradaki  —");
    radios_audio_ui_get_snapshot(window.ctx, 2, &synced); g_assert_cmpfloat(fabs(synced.effective_bpm - grids[0].bpm), <, 0.01);
    pcm = capture_offset("MAIN"); run_for(400);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, 0.02); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, 0.01);
    run = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 2500); g_assert_cmpint(run.deck, ==, 2);
    pcm = capture_offset("MAIN"); run_for(400); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, 0.07);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700 * grids[0].bpm / grids[1].bpm), <, 0.03);
    click(window.root, "deck-2-stop"); run_for(300);
    radios_audio_engine_unref(engine); json_node_free(first); json_node_free(second); json_node_free(other); window_close(&window);
}
typedef struct {
    RadioSAudioEngine *engine;
    RadioSAudioDeckSnapshot source;
    RadioSAudioChannelDsp channel;
    guint64 minimum_cursor;
} SyncPhysicalOutput;
static gboolean sync_physical_output_ready(gpointer value)
{
    SyncPhysicalOutput *physical = value; RadioSAudioSnapshot actual = {0};
    radios_audio_engine_snapshot(physical->engine, &actual);
    RadioSAudioDeckSnapshot *source = &actual.decks[0];
    g_assert_cmpuint(source->generation, ==, physical->source.generation);
    g_assert_cmpint(source->state, ==, RADIOS_AUDIO_PLAYING);
    g_assert_cmpstr(source->media_id, ==, physical->source.media_id);
    g_assert_cmpstr(source->version_id, ==, physical->source.version_id);
    g_assert_cmpfloat(source->gain_db, ==, physical->source.gain_db);
    g_assert_cmpfloat(source->tempo_ratio, ==, physical->source.tempo_ratio);
    g_assert_true(source->main_routed); g_assert_cmpint(source->output_bus, ==, RADIOS_AUDIO_BUS_MAIN);
    g_assert_cmpint(actual.mixer.channels[0].source_deck, ==, physical->channel.source_deck);
    g_assert_cmpfloat(actual.mixer.channels[0].gain_db, ==, physical->channel.gain_db);
    g_assert_cmpint(actual.mixer.channels[0].mute, ==, physical->channel.mute);
    g_assert_cmpint(actual.mixer.channels[0].crossfade_group, ==, physical->channel.crossfade_group);
    return actual.buses[RADIOS_AUDIO_BUS_MAIN].ready &&
        actual.buses[RADIOS_AUDIO_BUS_MAIN].output_buffer_frames > 0 &&
        actual.buses[RADIOS_AUDIO_BUS_MAIN].consumed_frames >= 12000 &&
        source->cursor_frames >= physical->minimum_cursor;
}

static void test_player_sync_rejects_unknown_and_stale_target(void)
{
    Window window = window_open(); seed_programme(&window, NULL, NULL); navigate(&window, "broadcast"); window.ctx->notify = rhythm_notice;
    JsonNode *first = media_import_rhythm(&window, "Preserved source", 700, 0.25, "music", 120);
    JsonNode *second = media_import_rhythm(&window, "Prepared target", 1700, 0.25, "music", 128);
    JsonNode *unknown = media_import(&window, "No beatgrid", 2600, 0.15, "music"); GError *error = NULL;
    GtkWidget *source = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0");
    GtkWidget *target = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-2");
    require_call(radios_audio_ui_load_media(window.ctx, source, json_node_get_object(first), 0, FALSE, &error), &error);
    require_call(radios_audio_ui_load_media(window.ctx, target, json_node_get_object(unknown), 2, FALSE, &error), &error);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0); deck_wait(&window, 2, RADIOS_AUDIO_UI_READY, 0); run_for(200);
    combo(window.root, "deck-0-autodj-target", "2"); click(window.root, "deck-0-play"); run_for(150);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    RadioSAudioSnapshot initial = {0}; radios_audio_engine_snapshot(engine, &initial);
    SyncPhysicalOutput physical = {engine, initial.decks[0], initial.mixer.channels[0], 0};
    g_assert_cmpint(physical.source.state, ==, RADIOS_AUDIO_PLAYING);
    g_assert_cmpfloat(physical.source.gain_db, ==, 0); g_assert_cmpfloat(physical.source.tempo_ratio, ==, 1);
    RadioSAudioUiSnapshot before, after; radios_audio_ui_get_snapshot(window.ctx, 0, &before);
    require_call(radios_audio_ui_load_media(window.ctx, target, json_node_get_object(second), 2, FALSE, &error), &error);
    deck_wait(&window, 2, RADIOS_AUDIO_UI_READY, 0);
    gchar *hold = fixture_flag(".acceptance-playout-prepare-hold"), *ready = fixture_flag(".acceptance-playout-prepare-ready");
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL)); player_generic_pair_start(&window, source, first, second, 0, 2);
    wait_until(file_exists, ready, "pair preparation is held before its authenticated reply");
    require_call(radios_audio_ui_load_media(window.ctx, target, json_node_get_object(unknown), 2, FALSE, &error), &error);
    deck_wait(&window, 2, RADIOS_AUDIO_UI_READY, 0); RadioSAudioUiSnapshot replacement; radios_audio_ui_get_snapshot(window.ctx, 2, &replacement);
    g_assert_cmpint(g_unlink(hold), ==, 0); run_wait(&window, RADIOS_PLAYOUT_ERROR, 0, 0);
    radios_audio_ui_get_snapshot(window.ctx, 2, &after); g_assert_cmpuint(after.generation, ==, replacement.generation);
    g_assert_cmpstr(after.media_id, ==, replacement.media_id);
    radios_audio_ui_get_snapshot(window.ctx, 0, &after); g_assert_cmpuint(after.generation, ==, before.generation);
    g_assert_cmpint(after.state, ==, RADIOS_AUDIO_UI_PLAYING);
    /* Replacing the target legitimately flushes/reopens physical outputs.
     * Decode-ready does not imply MAIN has resumed. Require actual ready
     * output and 250ms of real preserved-source clock advancement before
     * measuring the unchanged 300ms spectral window. */
    RadioSAudioSnapshot pending_output = {0}; radios_audio_engine_snapshot(engine, &pending_output);
    physical.minimum_cursor = pending_output.decks[0].cursor_frames + 12000;
    wait_until(sync_physical_output_ready, &physical, "actual MAIN resumes and preserved source consumes250ms after target replacement");
    goffset captured = capture_offset("MAIN"); run_for(300); g_assert_cmpfloat(tone_energy("MAIN", captured, 700), >, 0.07);
    g_assert_cmpfloat(tone_energy("MAIN", captured, 1700), <, 0.01);
    g_assert_cmpfloat(tone_energy("MAIN", captured, 2600), <, 0.01);
    g_assert_true(sync_physical_output_ready(&physical));
    g_assert_cmpint(g_unlink(ready), ==, 0); g_free(hold); g_free(ready);
    /* An unknown target BPM cannot be beatmatched. The pair still starts,
     * joins the playing source and crossfades into the target at its native
     * tempo instead of rejecting the whole AutoDJ. */
    radios_audio_ui_get_snapshot(window.ctx, 0, &before);
    player_generic_pair_start(&window, source, first, unknown, 0, 2);
    RadioSPlayoutSnapshot run = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 0);
    g_assert_cmpint(run.deck, ==, 0); g_assert_cmpint(run.next_deck, ==, 2); g_assert_cmpuint(run.transition, ==, RADIOS_AUDIO_RUN_CROSSFADE);
    radios_audio_ui_get_snapshot(window.ctx, 0, &after); g_assert_cmpuint(after.generation, ==, before.generation);
    run_wait(&window, RADIOS_PLAYOUT_OVERLAP, 0, 0); deck_wait(&window, 2, RADIOS_AUDIO_UI_PLAYING, 100);
    radios_audio_ui_get_snapshot(window.ctx, 2, &after); g_assert_cmpfloat(after.tempo_ratio, ==, 1);
    captured = capture_offset("MAIN"); run_for(300); g_assert_cmpfloat(tone_energy("MAIN", captured, 2600), >, 0.01);
    stop_run(&window); run_for(200); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    radios_audio_engine_unref(engine);
    json_node_free(first); json_node_free(second); json_node_free(unknown); window_close(&window);
}
typedef struct {
    const gchar *id;
    GtkWidget *dialog, *list;
    GtkTreeIter iter;
} NativeRotationPicker;
static gboolean native_rotation_list_ready(gpointer data)
{
    NativeRotationPicker *picker = data; picker->dialog = dialog_find("player-rotation-dialog");
    if (!picker->dialog) return FALSE;
    picker->list = find(picker->dialog, "player-rotation-list"); if (!GTK_IS_TREE_VIEW(picker->list)) return FALSE;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(picker->list)); GtkTreeIter iter;
    for (gboolean next = gtk_tree_model_get_iter_first(model, &iter); next; next = gtk_tree_model_iter_next(model, &iter)) {
        gchar *id = NULL; gtk_tree_model_get(model, &iter, 2, &id, -1);
        gboolean same = !g_strcmp0(id, picker->id); g_free(id);
        if (same) { picker->iter = iter; return TRUE; }
    }
    return FALSE;
}
typedef struct { GtkWidget *card; const gchar *id, *document; guint count; } NativeRotationCard;
static gboolean native_rotation_card_ready(gpointer data)
{
    NativeRotationCard *card = data; CardInputCount count = {card->card, card->count};
    return !dialog_find("player-rotation-dialog") &&
        !g_strcmp0(g_object_get_data(G_OBJECT(card->card), "radios-player-rotation-id"), card->id) &&
        !g_strcmp0(g_object_get_data(G_OBJECT(card->card), "radios-player-rotation-document"), card->document) && card_input_count(&count);
}
static gboolean native_rotation_picker_failed(gpointer data)
{
    GtkWidget *dialog = data, *status = find(dialog, "player-rotation-status"), *load = find(dialog, "player-rotation-load");
    return GTK_IS_LABEL(status) && GTK_IS_BUTTON(load) && !gtk_widget_is_sensitive(load) &&
        strstr(gtk_label_get_text(GTK_LABEL(status)), "unavailable");
}
static NativeRotationPicker native_rotation_picker(Window *window, const gchar *id)
{
    click(window->root, "deck-0-rotation"); NativeRotationPicker picker = {id, NULL, NULL, {0}};
    wait_until(native_rotation_list_ready, &picker, "real saved reusable definition appears in actual rotation picker");
    gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(picker.list)), &picker.iter); pump(); return picker;
}
static void native_rotation_load(Window *window, const gchar *id, const gchar *document, gboolean capture_dialog)
{
    NativeRotationPicker picker = native_rotation_picker(window, id);
    GtkWidget *load = find(picker.dialog, "player-rotation-load"); g_assert_true(GTK_IS_BUTTON(load));
    wait_until(widget_enabled, load, "actual named rotation two-phase verified preview enables Load");
    GtkWidget *preview = find(picker.dialog, "player-rotation-preview"); g_assert_true(GTK_IS_TREE_VIEW(preview));
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(preview)); g_assert_cmpint(gtk_tree_model_iter_n_children(model, NULL), ==, 2);
    GtkTreeIter iter; g_assert_true(gtk_tree_model_get_iter_first(model, &iter)); const gchar *titles[] = {"DJ rotation canonical A", "DJ rotation canonical B"};
    for (guint index = 0; index < 2; ++index) {
        gchar *title = NULL; gtk_tree_model_get(model, &iter, 1, &title, -1); g_assert_cmpstr(title, ==, titles[index]); g_free(title);
        if (!index) g_assert_true(gtk_tree_model_iter_next(model, &iter));
    }
    if (capture_dialog) saved_player_capture(picker.dialog, "RADIOS_TEST_ROTATION_PICKER_CAPTURE");
    gtk_button_clicked(GTK_BUTTON(load));
    GtkWidget *card = g_object_get_data(G_OBJECT(window->ctx->parent), "radios-deck-owner-0");
    NativeRotationCard ready = {card, id, document, 2}; wait_until(native_rotation_card_ready, &ready, "real final authenticated accept callback assigns named rotation and closes dialog");
    assert_card_input_titles(card, titles, 2); run_for(150);
}
static gboolean native_rotation_cleared(gpointer value)
{
    GtkWidget *card = value, *start = find(card, "deck-0-autodj-start");
    return !g_object_get_data(G_OBJECT(card), "radios-player-rotation-id") && start;
}
static void test_named_rotation_picker_autodj_gate_pair_pcm(void)
{
    Window window = window_open(); seed_programme(&window, NULL, NULL); navigate(&window, "broadcast"); players_show(&window);
    GtkWidget *card = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0"); g_assert_nonnull(card);
    JsonNode *first = media_import(&window, "DJ rotation canonical A", 700, .25, "music"),
             *second = media_import(&window, "DJ rotation canonical B", 1700, .25, "music"),
             *other = media_import_rhythm(&window, "DJ independent third deck", 2600, .25, "music", 120);
    JsonNode *playlist = playlist_create("DJ reusable verified playlist", first, second);
    GDateTime *now = g_date_time_new_now_utc(); gchar *date = g_date_time_format(now, "%Y-%m-%d"); g_date_time_unref(now);
    JsonNode *configuration = calendar_schedule_config(date, radios_json_string(json_node_get_object(playlist), "_id", ""), 1); g_free(date);
    JsonObject *definition = json_array_get_object_element(json_object_get_array_member(json_node_get_object(configuration), "rotation_library"), 0);
    json_object_set_string_member(definition, "name", "DJ native reusable rotation");
    JsonObject *source = json_array_get_object_element(json_object_get_array_member(definition, "sources"), 0);
    json_object_set_int_member(source, "interval_minutes", 10080);
    gchar *id = g_strdup(radios_json_string(definition, "id", "")); JsonNode *saved = document_save("automation_calendar", "main", json_node_get_object(configuration));
    gchar *document = g_strdup(radios_json_string(json_node_get_object(saved), "_id", ""));
    GError *error = NULL; require_call(radios_audio_ui_load_media(window.ctx, card, json_node_get_object(first), 0, FALSE, &error), &error);
    RadioSAudioUiSnapshot raw = deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0); run_for(150);
    GtkWidget *start = find(card, "deck-0-autodj-start"); g_assert_true(GTK_IS_BUTTON(start)); g_assert_false(gtk_widget_is_sensitive(start));
    guint64 intent = rotation_intent(&window); gtk_button_clicked(GTK_BUTTON(start)); run_for(100);
    g_assert_cmpuint(rotation_intent(&window), ==, intent); RadioSAudioUiSnapshot unchanged;
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 0, &unchanged)); g_assert_cmpuint(unchanged.generation, ==, raw.generation);
    g_assert_cmpuint(unchanged.position_ms, ==, raw.position_ms); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    native_rotation_load(&window, id, document, TRUE); g_assert_true(gtk_widget_is_sensitive(start));
    g_assert_cmpstr(g_object_get_data(G_OBJECT(card), "radios-player-rotation-name"), ==, "DJ native reusable rotation");
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(card, "deck-0-rotation-status"))), "2 parça hazır"));
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001); calendar_schedule_unchanged(saved); assert_saved_playlist_unchanged(playlist);
    /* A genuine HTTP preparation failure must preserve the already loaded
     * queue and rotation. This fault gate exists only in the private server. */
    gchar *fail = fixture_flag(".acceptance-playout-prepare-fail"); g_assert_true(g_file_set_contents(fail, "fail", -1, NULL));
    NativeRotationPicker failed = native_rotation_picker(&window, id);
    wait_until(native_rotation_picker_failed, failed.dialog, "real private HTTP failure disables new rotation accept");
    gtk_dialog_response(GTK_DIALOG(failed.dialog), GTK_RESPONSE_CANCEL); pump(); g_assert_cmpint(g_unlink(fail), ==, 0); g_free(fail); run_for(150);
    const gchar *titles[] = {"DJ rotation canonical A", "DJ rotation canonical B"}; assert_card_input_titles(card, titles, 2);
    g_assert_cmpstr(g_object_get_data(G_OBJECT(card), "radios-player-rotation-id"), ==, id); g_assert_true(gtk_widget_is_sensitive(start));
    combo(window.root, "deck-0-autodj-target", "2");
    gchar *hold = fixture_flag(".acceptance-playout-prepare-hold"), *ready = fixture_flag(".acceptance-playout-prepare-ready");
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL)); intent = rotation_intent(&window); click(card, "deck-0-autodj-start");
    wait_until(file_exists, ready, "actual named start HTTP prepare response is pending before delivery");
    combo(window.root, "deck-0-autodj-target", "3"); g_assert_cmpint(g_unlink(hold), ==, 0); g_assert_cmpint(g_unlink(ready), ==, 0);
    wait_until(widget_enabled, start, "changed chosen target cancels old prepared start and unlocks actual card");
    g_assert_cmpuint(rotation_intent(&window), ==, intent); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine); RadioSAudioRunSnapshot lease;
    radios_audio_engine_player_runner_snapshot(engine, 0, &lease); g_assert_cmpint(lease.state, ==, RADIOS_AUDIO_RUN_IDLE);
    for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) g_assert_false(lease.owned[deck]);
    combo(window.root, "deck-0-autodj-target", "2");
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(other), 1, FALSE, &error), &error);
    deck_wait(&window, 1, RADIOS_AUDIO_UI_READY, 0); click(window.root, "deck-1-play");
    RadioSAudioUiSnapshot third = deck_wait(&window, 1, RADIOS_AUDIO_UI_PLAYING, 100);
    saved_player_capture(window.root, "RADIOS_TEST_ROTATION_CARD_CAPTURE"); click(card, "deck-0-autodj-start");
    RadioSPlayoutSnapshot playing = rotation_run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 250);
    g_assert_cmpuint(playing.total, ==, 2); g_assert_cmpint(playing.deck, ==, 0); g_assert_cmpint(playing.next_deck, ==, 2);
    g_assert_cmpuint(playing.transition, ==, RADIOS_AUDIO_RUN_CROSSFADE); g_assert_cmpuint(playing.overlap_ms, ==, 500);
    g_assert_cmpstr(playing.title, ==, titles[0]); radios_audio_engine_player_runner_snapshot(engine, 0, &lease);
    g_assert_true(lease.owned[0]); g_assert_true(lease.owned[2]); g_assert_false(lease.owned[1]);
    goffset pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .08); g_assert_cmpfloat(tone_energy("MAIN", pcm, 2600), >, .08);
    playing = rotation_run_wait(&window, RADIOS_PLAYOUT_OVERLAP, 0, 0); g_assert_cmpuint(playing.overlap_ms, ==, 500);
    pcm = capture_offset("MAIN"); run_for(200); g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .01); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .01);
    playing = rotation_run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 200); g_assert_cmpint(playing.deck, ==, 2); g_assert_cmpstr(playing.title, ==, titles[1]);
    pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .08); g_assert_cmpfloat(tone_energy("MAIN", pcm, 2600), >, .08);
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 1, &unchanged)); g_assert_cmpuint(unchanged.generation, ==, third.generation);
    g_assert_cmpint(unchanged.state, ==, RADIOS_AUDIO_UI_PLAYING); g_assert_cmpstr(unchanged.media_id, ==, third.media_id);
    rotation_stop_run(&window); click(window.root, "deck-1-stop"); run_for(200); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001); radios_audio_engine_unref(engine);
    calendar_schedule_unchanged(saved); assert_saved_playlist_unchanged(playlist);
    click(card, "deck-0-clear"); wait_until(native_rotation_cleared, card, "actual queue clear invalidates rotation provenance and AutoDJ gate");
    native_rotation_load(&window, id, document, FALSE);
    click(card, "deck-0-playlist"); SavedLoaderList ordinary = {"DJ reusable verified playlist", NULL, NULL, {0}};
    wait_until(saved_loader_list_ready, &ordinary, "ordinary real saved playlist remains independently loadable");
    gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(ordinary.tree)), &ordinary.iter);
    GtkWidget *accept = gtk_dialog_get_widget_for_response(GTK_DIALOG(ordinary.dialog), GTK_RESPONSE_ACCEPT); wait_until(widget_enabled, accept, "ordinary playlist accept ready");
    gtk_button_clicked(GTK_BUTTON(accept)); pump(); wait_until(native_rotation_cleared, card, "ordinary playlist replacement cannot retain named AutoDJ authorization"); assert_card_input_titles(card, titles, 2);
    native_rotation_load(&window, id, document, FALSE);
    radios_data_connect(window.ctx); wait_until(window_ready, &window, "real public reconnect obtains fresh authenticated identity");
    wait_until(native_rotation_cleared, card, "reconnected session clears old rotation client provenance"); assert_card_input_titles(card, titles, 2);
    calendar_schedule_unchanged(saved); assert_saved_playlist_unchanged(playlist); window_close(&window);
    window = window_open(); navigate(&window, "broadcast"); players_show(&window); native_rotation_load(&window, id, document, FALSE);
    card = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0"); g_assert_true(gtk_widget_is_sensitive(find(card, "deck-0-autodj-start")));
    calendar_schedule_unchanged(saved); assert_saved_playlist_unchanged(playlist); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    g_test_message("Real named picker/load/reopen, raw single-media AutoDJ guard, failed-load preservation, asynchronous target fence, prepared pair PCM/third-deck isolation, clear/playlist replacement/reconnect provenance passed.");
    window_close(&window); g_free(hold); g_free(ready); g_free(id); g_free(document); json_node_free(saved); json_node_free(configuration); json_node_free(playlist); json_node_free(first); json_node_free(second); json_node_free(other);
}

/* Native source context and durable routing use the full application because
 * the small calendar harness intentionally has no physical player registry. */
static JsonObject *routed_definition(JsonNode *record)
{
    JsonObject *config = json_object_get_object_member(json_node_get_object(record), "config");
    g_assert_nonnull(config);
    JsonArray *library = json_object_get_array_member(config, "rotation_library");
    g_assert_cmpuint(json_array_get_length(library), ==, 1);
    return json_array_get_object_element(library, 0);
}
static JsonObject *routed_source(JsonNode *record)
{
    return json_array_get_object_element(json_object_get_array_member(routed_definition(record), "sources"), 0);
}
static void assert_routed_pools(JsonObject *source)
{
    JsonObject *routing = json_object_get_object_member(source, "autodj"); g_assert_nonnull(routing);
    const gchar *kinds[] = {"music", "advertisement", "jingle", "sweeper"};
    const gint expected[][2] = {{1, 2}, {3, 0}, {4, 0}, {4, 0}};
    g_assert_cmpuint(json_object_get_size(routing), ==, 4);
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) {
        JsonArray *pool = json_object_get_array_member(routing, kinds[i]); g_assert_nonnull(pool);
        g_assert_cmpuint(json_array_get_length(pool), ==, i ? 1 : 2);
        for (guint j = 0; j < json_array_get_length(pool); ++j)
            g_assert_cmpint(json_array_get_int_element(pool, j), ==, expected[i][j]);
    }
}
static JsonNode *playlist_append_verified(JsonNode *playlist, JsonNode *detail)
{
    JsonObject *row = json_node_get_object(playlist), *verified = json_node_get_object(detail);
    JsonObject *media = json_object_get_object_member(verified, "media"), *version = json_object_get_object_member(verified, "version");
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body), *metadata = json_object_new();
    json_object_set_string_member(object, "playlist_id", radios_json_string(row, "_id", ""));
    json_object_set_int_member(object, "expected_revision", radios_json_int(row, "_rev", 0));
    const gchar *names[] = {"title", "artist", "album", "genre"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) json_object_set_string_member(metadata, names[i], radios_json_string(media, names[i], ""));
    json_object_set_string_member(metadata, "key", ""); json_object_set_string_member(metadata, "location", "/untrusted-native-routing.wav");
    json_object_set_string_member(metadata, "comment", "Private verified mixed-kind routing fixture");
    json_object_set_int_member(metadata, "duration_ms", radios_json_int(version, "duration_ms", 0)); json_object_set_double_member(metadata, "bpm", 0);
    bind(metadata, verified); json_object_set_object_member(object, "metadata", metadata);
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    JsonNode *next = request("/v1/workflows/desktop-playlists/entry-save", body); json_node_free(body); json_node_free(playlist); return next;
}
static JsonNode *routing_short_markers(JsonNode *detail, guint duration)
{
    JsonObject *media = json_object_get_object_member(json_node_get_object(detail), "media");
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body), *markers = json_object_new();
    json_object_set_string_member(object, "media_id", radios_json_string(media, "_id", ""));
    json_object_set_int_member(object, "expected_revision", radios_json_int(media, "_rev", 0)); json_object_set_null_member(object, "expected_marker_revision");
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    const gchar *names[] = {"cue_in_ms", "intro_ms", "out_ms", "fade_start_ms", "fade_end_ms", "auto_next_ms", "overlap_ms"};
    const guint values[] = {0, 0, duration, duration, duration, duration, 0};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) json_object_set_int_member(markers, names[i], values[i]);
    json_object_set_boolean_member(markers, "automatic", FALSE); json_object_set_object_member(object, "markers", markers);
    JsonNode *saved = request("/v1/workflows/desktop-media/markers-save", body); json_node_free(body); json_node_free(detail);
    g_assert_cmpint(radios_json_int(json_object_get_object_member(json_node_get_object(saved), "markers"), "out_ms", 0), ==, duration); return saved;
}
typedef struct { GtkWidget *combo; const gchar *id; } RoutingChoice;
static gboolean routing_choice_ready(gpointer value)
{
    RoutingChoice *choice = value;
    return gtk_combo_box_set_active_id(GTK_COMBO_BOX(choice->combo), choice->id);
}
static GtkWidget *routing_editor_open(Window *window, const gchar *id)
{
    navigate(window, "automation"); GtkWidget *page = find(window->root, "automation-calendar-main"); g_assert_nonnull(page);
    g_assert_true(gtk_widget_get_mapped(page));
    RoutingChoice choice = {find(page, "automation-calendar-existing-rotation"), id}; g_assert_true(GTK_IS_COMBO_BOX(choice.combo));
    wait_until(routing_choice_ready, &choice, "actual saved reusable rotation reaches native calendar selector");
    click(page, "automation-calendar-manage-rotation"); wait_until(dialog_exists, "automation-calendar-library-dialog", "actual rotation editor opens");
    GtkWidget *dialog = dialog_find("automation-calendar-library-dialog"); run_for(100);
    GtkWidget *tree = find(dialog, "rotation-editor-sources"); g_assert_true(GTK_IS_TREE_VIEW(tree));
    g_assert_true(gtk_widget_get_mapped(tree)); g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(tree)), NULL), ==, 1); return dialog;
}
static gboolean routing_page_ready(gpointer value)
{
    GtkWidget *page = find(value, "rotation-editor-autodj-dialog");
    return page && gtk_widget_get_mapped(page);
}
static GtkWidget *routing_source_context(GtkWidget *editor)
{
    GtkWidget *tree = find(editor, "rotation-editor-sources"); g_assert_true(GTK_IS_TREE_VIEW(tree));
    GtkTreePath *path = gtk_tree_path_new_from_indices(0, -1); gtk_tree_view_scroll_to_cell(GTK_TREE_VIEW(tree), path, NULL, FALSE, 0, 0); run_for(50);
    GdkRectangle rectangle; gtk_tree_view_get_cell_area(GTK_TREE_VIEW(tree), path, NULL, &rectangle); gtk_tree_path_free(path);
    GdkWindow *bin = gtk_tree_view_get_bin_window(GTK_TREE_VIEW(tree)); g_assert_nonnull(bin); g_assert_true(gdk_window_is_viewable(bin));
    GdkEvent *event = gdk_event_new(GDK_BUTTON_PRESS); event->button.window = g_object_ref(bin); event->button.send_event = TRUE;
    event->button.button = 3; event->button.x = MAX(12, rectangle.x + 4); event->button.y = rectangle.y + rectangle.height / 2.0;
    gint x, y; gdk_window_get_origin(bin, &x, &y); event->button.x_root = x + event->button.x; event->button.y_root = y + event->button.y;
    GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(tree)); gdk_event_set_device(event, gdk_seat_get_pointer(seat));
    g_assert_true(gtk_widget_event(tree, event)); gdk_event_free(event); pump();
    GtkWidget *item = NULL;
    for (GList *menu = gtk_menu_get_for_attach_widget(tree); menu && !item; menu = menu->next) item = find(menu->data, "rotation-editor-autodj");
    g_assert_true(GTK_IS_MENU_ITEM(item)); wait_until(calendar_schedule_menu_visible, item, "real playlist right-click AutoDJ context menu visible");
    gtk_menu_shell_activate_item(GTK_MENU_SHELL(gtk_widget_get_parent(item)), item, TRUE); pump();
    wait_until(routing_page_ready, editor, "actual source AutoDJ settings page is visible inside the editor");
    GtkWidget *page = find(editor, "rotation-editor-autodj-dialog");
    g_assert_false(GTK_IS_WINDOW(page));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(find(editor, "rotation-editor-pages"))), ==, "routing");
    return page;
}
static void routing_checks(GtkWidget *dialog, gboolean render_only)
{
    const gchar *kinds[] = {"music", "advertisement", "jingle", "sweeper"};
    for (guint kind = 0; kind < G_N_ELEMENTS(kinds); ++kind) {
        combo(dialog, "rotation-editor-autodj-content-kind", kinds[kind]);
        if (!render_only) combo(dialog, "rotation-editor-autodj-working-mode", kind ? "single" : "multi");
        run_for(40);
        for (guint deck = 1; deck <= 6; ++deck) {
        gchar *name = g_strdup_printf("rotation-editor-autodj-%s-player-%u", kinds[kind], deck);
        GtkWidget *check = find(dialog, name); g_free(name); g_assert_true(GTK_IS_TOGGLE_BUTTON(check)); g_assert_true(gtk_widget_get_mapped(check));
        gboolean expected = kind == 0 ? deck <= 2 : deck == (kind == 1 ? 3u : 4u);
        if (render_only) g_assert_cmpint(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check)), ==, expected);
        else gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), expected);
        }
    }
    combo(dialog, "rotation-editor-autodj-content-kind", "music"); run_for(40);
    g_assert_null(find(dialog, "rotation-editor-autodj-music-player-7"));
    g_assert_null(find(dialog, "rotation-editor-autodj-music-player-32"));
    g_assert_true(GTK_IS_LABEL(find(dialog, "rotation-editor-autodj-summary"))); pump();
}
typedef struct { const gchar *id; JsonNode *saved; } RoutingSaved;
static gboolean routing_saved(gpointer value)
{
    RoutingSaved *condition = value; JsonNode *saved = document_read("automation_calendar", "main");
    if (!saved) return FALSE;
    JsonObject *source = routed_source(saved), *routing = json_object_has_member(source, "autodj") ? json_object_get_object_member(source, "autodj") : NULL;
    gboolean ready = routing && !g_strcmp0(radios_json_string(routed_definition(saved), "id", ""), condition->id);
    if (ready) { g_clear_pointer(&condition->saved, json_node_free); condition->saved = saved; } else json_node_free(saved);
    return ready;
}
typedef struct { gboolean enabled; JsonNode *saved; } RoutingOverrideSaved;
static gboolean routing_override_saved(gpointer value)
{
    RoutingOverrideSaved *condition = value; JsonNode *saved = document_read("automation_calendar", "main");
    if (!saved) return FALSE;
    JsonObject *source = routed_source(saved);
    gboolean ready = json_object_has_member(source, "settings") == condition->enabled;
    if (ready) { assert_routed_pools(source); g_clear_pointer(&condition->saved, json_node_free); condition->saved = saved; }
    else json_node_free(saved);
    return ready;
}
static void routing_named_load(Window *window, const gchar *id, const gchar *document, const gchar **titles, guint count)
{
    NativeRotationPicker picker = native_rotation_picker(window, id); GtkWidget *load = find(picker.dialog, "player-rotation-load");
    wait_until(widget_enabled, load, "genuine routed rotation preview permits native Load");
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(find(picker.dialog, "player-rotation-preview")));
    g_assert_cmpint(gtk_tree_model_iter_n_children(model, NULL), ==, count); GtkTreeIter iter; g_assert_true(gtk_tree_model_get_iter_first(model, &iter));
    for (guint index = 0; index < count; ++index) {
        gchar *title = NULL; gtk_tree_model_get(model, &iter, 1, &title, -1); g_assert_cmpstr(title, ==, titles[index]); g_free(title);
        if (index + 1 < count) g_assert_true(gtk_tree_model_iter_next(model, &iter));
    }
    gtk_button_clicked(GTK_BUTTON(load)); GtkWidget *card = g_object_get_data(G_OBJECT(window->ctx->parent), "radios-deck-owner-0");
    NativeRotationCard ready = {card, id, document, count}; wait_until(native_rotation_card_ready, &ready, "native routed rotation assignment accepts final authenticated preparation");
    assert_card_input_titles(card, titles, count); run_for(100);
}
static JsonNode *routing_configuration(JsonNode *playlist, guint count, gboolean beatmatch)
{
    GDateTime *now = g_date_time_new_now_utc(); gchar *date = g_date_time_format(now, "%Y-%m-%d"); g_date_time_unref(now);
    JsonNode *configuration = calendar_schedule_config(date, radios_json_string(json_node_get_object(playlist), "_id", ""), 0); g_free(date);
    JsonObject *definition = json_array_get_object_element(json_object_get_array_member(json_node_get_object(configuration), "rotation_library"), 0);
    json_object_set_string_member(definition, "name", beatmatch ? "Native local rotation beatmatch" : "Native per-playlist mixed routing");
    JsonObject *source = json_array_get_object_element(json_object_get_array_member(definition, "sources"), 0);
    json_object_set_int_member(source, "count", count); json_object_set_int_member(source, "interval_minutes", 10080); json_object_set_string_member(source, "media_kind", "all");
    JsonObject *settings = json_object_get_object_member(definition, "settings"); json_object_set_string_member(settings, "transition_selection_mode", "linear");
    if (beatmatch) {
        JsonArray *rules = json_object_get_array_member(settings, "transitions");
        for (guint i = 0; i < json_array_get_length(rules); ++i) json_object_set_int_member(json_array_get_object_element(rules, i), "weight", 0);
        JsonObject *rule = json_object_new(); json_object_set_int_member(rule, "mode", 3); json_object_set_int_member(rule, "weight", 100);
        json_object_set_int_member(rule, "overlap_ms", 500); json_object_set_string_member(rule, "effect", "lowpass"); json_array_add_object_element(rules, rule);
    }
    return configuration;
}
static void routing_ui_apply(Window *window, const gchar *id)
{
    GtkWidget *editor = routing_editor_open(window, id), *dialog = routing_source_context(editor); routing_checks(dialog, FALSE);
    click(dialog, "rotation-editor-autodj-apply"); pump();
    RoutingSaved saved = {id, NULL}; wait_until(routing_saved, &saved, "actual playlist role routing autosaves through authenticated database CAS");
    assert_routed_pools(routed_source(saved.saved)); json_node_free(saved.saved);
    click(dialog, "rotation-editor-autodj-cancel"); pump();
    gtk_dialog_response(GTK_DIALOG(editor), GTK_RESPONSE_CLOSE); pump();
}
typedef struct {
    gboolean rejected;
    void (*previous)(gpointer, const gchar *);
    gpointer previous_data;
} RoutingStartNotice;
static void routing_start_notice(gpointer value, const gchar *message)
{
    RoutingStartNotice *notice = value;
    if (strstr(message, "Hazırlık sırasında oynatıcı veya yayın değişti")) notice->rejected = TRUE;
    if (notice->previous) notice->previous(notice->previous_data, message);
}
static gboolean routing_start_rejected(gpointer value)
{ return ((RoutingStartNotice *)value)->rejected; }
static void test_playlist_context_autodj_routes_persistence_multi_pcm(void)
{
    Window window = window_open(); seed_programme(&window, NULL, NULL); navigate(&window, "broadcast"); players_show(&window);
    /* Keep short station IDs separated, as required by the saved rotation's
     * continuity policy, while exercising every independent routing role. */
    const gchar *titles[] = {"Route music A", "Route jingle", "Route music B", "Route sweeper", "Route advertisement"};
    const gchar *kinds[] = {"music", "jingle", "music", "sweeper", "advertisement"};
    const gdouble frequencies[] = {700, 3300, 1700, 3900, 2700}; JsonNode *media[5];
    for (guint i = 0; i < G_N_ELEMENTS(media); ++i) media[i] = routing_short_markers(media_import(&window, titles[i], frequencies[i], .22, kinds[i]), 1800);
    JsonNode *manual = media_import_rhythm(&window, "Manual fifth deck preserved", 1100, .2, "music", 120);
    JsonNode *playlist = playlist_create("Mixed actual kinds AutoDJ playlist", media[0], media[1]);
    for (guint i = 2; i < G_N_ELEMENTS(media); ++i) playlist = playlist_append_verified(playlist, media[i]);
    JsonNode *configuration = routing_configuration(playlist, 5, FALSE);
    JsonObject *definition = json_array_get_object_element(json_object_get_array_member(json_node_get_object(configuration), "rotation_library"), 0);
    gchar *id = g_strdup(radios_json_string(definition, "id", ""));
    JsonNode *saved = document_save("automation_calendar", "main", json_node_get_object(configuration));
    gchar *document = g_strdup(radios_json_string(json_node_get_object(saved), "_id", "")); window_close(&window); window = window_open();
    GtkWidget *editor = routing_editor_open(&window, id), *dialog = routing_source_context(editor); routing_checks(dialog, FALSE);
    /* An empty required role is rejected without changing the durable document. */
    combo(dialog, "rotation-editor-autodj-content-kind", "jingle");
    toggle(dialog, "rotation-editor-autodj-jingle-player-4", FALSE); click(dialog, "rotation-editor-autodj-apply"); pump();
    g_assert_nonnull(find(editor, "rotation-editor-autodj-dialog"));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(dialog, "rotation-editor-autodj-status"))), "en az")); calendar_schedule_unchanged(saved);
    toggle(dialog, "rotation-editor-autodj-jingle-player-4", TRUE); click(dialog, "rotation-editor-autodj-cancel"); pump(); calendar_schedule_unchanged(saved);
    dialog = routing_source_context(editor); routing_checks(dialog, FALSE); click(dialog, "rotation-editor-autodj-apply"); pump();
    RoutingSaved committed = {id, NULL}; wait_until(routing_saved, &committed, "valid native source role mapping durably saved");
    assert_routed_pools(routed_source(committed.saved)); json_node_free(saved); saved = committed.saved;
    click(dialog, "rotation-editor-autodj-cancel"); pump();
    /* Repeat overrides have their own lifetime; disabling them must leave
     * the source's independent AutoDJ routing intact. */
    GtkWidget *source_tree = find(editor, "rotation-editor-sources");
    GtkTreePath *source_path = gtk_tree_path_new_from_indices(0, -1);
    gtk_tree_view_row_activated(GTK_TREE_VIEW(source_tree), source_path, gtk_tree_view_get_column(GTK_TREE_VIEW(source_tree), 0));
    gtk_tree_path_free(source_path); pump();
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(find(editor, "rotation-editor-pages"))), ==, "songs");
    gtk_expander_set_expanded(GTK_EXPANDER(find(editor, "rotation-editor-source-settings")), TRUE); run_for(50);
    toggle(editor, "rotation-editor-source-override", TRUE);
    RoutingOverrideSaved override = {TRUE, NULL}; wait_until(routing_override_saved, &override, "source repeat override saved while retaining independent AutoDJ map"); json_node_free(override.saved); override.saved = NULL;
    toggle(editor, "rotation-editor-source-override", FALSE); override.enabled = FALSE;
    wait_until(routing_override_saved, &override, "disabling repeat override removes only repeat settings and retains role routing");
    json_node_free(saved); saved = override.saved; click(editor, "rotation-editor-back");
    dialog = routing_source_context(editor); routing_checks(dialog, TRUE); toggle(dialog, "rotation-editor-autodj-music-player-2", FALSE);
    click(dialog, "rotation-editor-autodj-cancel"); pump(); calendar_schedule_unchanged(saved);
    /* Reauthentication to the same immutable identity remains usable. A
     * retained routing page must reject a genuinely closed data session. */
    dialog = routing_source_context(editor); toggle(dialog, "rotation-editor-autodj-music-player-2", FALSE);
    RadioSDataClient *prior_client = radios_data_client_acquire(window.ctx); g_assert_nonnull(prior_client);
    radios_data_connect(window.ctx); wait_until(window_ready, &window, "replacement native authenticated client ready");
    RadioSDataClient *replacement = radios_data_client_acquire(window.ctx); g_assert_nonnull(replacement);
    g_assert_true(prior_client != replacement); g_assert_true(radios_data_client_same_identity(prior_client, replacement));
    radios_data_client_unref(prior_client); radios_data_client_unref(replacement); calendar_schedule_unchanged(saved);
    combo(dialog, "rotation-editor-autodj-working-mode", "single");
    radios_data_close(window.ctx->data); g_assert_false(radios_data_connected(window.ctx));
    click(dialog, "rotation-editor-autodj-apply"); pump(); g_assert_nonnull(find(editor, "rotation-editor-autodj-dialog"));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(dialog, "rotation-editor-autodj-status"))), "yeniden")); calendar_schedule_unchanged(saved);
    click(dialog, "rotation-editor-autodj-cancel"); gtk_dialog_response(GTK_DIALOG(editor), GTK_RESPONSE_CLOSE); pump();
    window_close(&window); window = window_open(); editor = routing_editor_open(&window, id); dialog = routing_source_context(editor); routing_checks(dialog, TRUE);
    saved_player_capture(dialog, "RADIOS_TEST_ROUTING_DIALOG_CAPTURE"); click(dialog, "rotation-editor-autodj-cancel"); gtk_dialog_response(GTK_DIALOG(editor), GTK_RESPONSE_CLOSE); pump();
    navigate(&window, "broadcast"); players_show(&window); combo(window.root, "deck-0-autodj-target", "-1"); routing_named_load(&window, id, document, titles, 5);
    GtkWidget *card = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0");
    combo(card, "deck-0-autodj-mode", "2");
    GtkWidget *start = find(card, "deck-0-autodj-start"); wait_until(widget_enabled, start, "fully routed playlist enables named AutoDJ with legacy target disabled");
    GError *error = NULL; require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(manual), 4, FALSE, &error), &error);
    deck_wait(&window, 4, RADIOS_AUDIO_UI_READY, 0); click(window.root, "deck-4-play"); RadioSAudioUiSnapshot untouched = deck_wait(&window, 4, RADIOS_AUDIO_UI_PLAYING, 100);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    /* Capture the whole saved four-player pool before the asynchronous HTTP
     * prepare. Changing a member beyond the old source/peer pair must reject
     * that stale response without replacing its new media or unrelated PCM. */
    gchar *hold = fixture_flag(".acceptance-playout-prepare-hold"), *ready = fixture_flag(".acceptance-playout-prepare-ready");
    RoutingStartNotice notice = {FALSE, window.ctx->notify, window.ctx->notify_data};
    window.ctx->notify = routing_start_notice; window.ctx->notify_data = &notice;
    guint64 general_intent = radios_playout_ui_intent(window.ctx);
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL)); click(card, "deck-0-autodj-start");
    wait_until(file_exists, ready, "routed prepare held after capturing every saved player generation");
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(manual), 2, FALSE, &error), &error);
    RadioSAudioUiSnapshot changed = deck_wait(&window, 2, RADIOS_AUDIO_UI_READY, 0);
    g_assert_cmpint(g_unlink(hold), ==, 0);
    wait_until(routing_start_rejected, &notice, "changed added-role player rejects the pre-HTTP routed pool snapshot");
    window.ctx->notify = notice.previous; window.ctx->notify_data = notice.previous_data;
    RadioSAudioUiSnapshot preserved; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 2, &preserved));
    g_assert_cmpuint(preserved.generation, ==, changed.generation); g_assert_cmpstr(preserved.media_id, ==, changed.media_id);
    g_assert_cmpint(preserved.state, ==, RADIOS_AUDIO_UI_READY);
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 4, &preserved));
    g_assert_cmpuint(preserved.generation, ==, untouched.generation); g_assert_cmpstr(preserved.media_id, ==, untouched.media_id);
    g_assert_cmpint(preserved.state, ==, RADIOS_AUDIO_UI_PLAYING);
    RadioSPlayoutSnapshot rejected = {0}; g_assert_true(radios_playout_ui_get_snapshot(window.ctx, &rejected));
    g_assert_cmpint(rejected.state, ==, RADIOS_PLAYOUT_IDLE); g_assert_cmpuint(radios_playout_ui_intent(window.ctx), ==, general_intent);
    if (radios_player_workspace_snapshot(window.ctx->players, 0, &rejected)) g_assert_cmpint(rejected.state, ==, RADIOS_PLAYOUT_IDLE);
    RadioSAudioRunSnapshot rejected_run; radios_audio_engine_player_runner_snapshot(engine, 0, &rejected_run);
    g_assert_cmpint(rejected_run.state, ==, RADIOS_AUDIO_RUN_IDLE);
    for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) g_assert_false(rejected_run.owned[deck]);
    NativeRotationCard retained = {card, id, document, 5}; g_assert_true(native_rotation_card_ready(&retained));
    assert_card_input_titles(card, titles, 5); calendar_schedule_unchanged(saved); assert_saved_playlist_unchanged(playlist);
    g_assert_cmpint(g_unlink(ready), ==, 0); g_free(hold); g_free(ready);
    require_call(radios_audio_engine_unload(engine, 2, &error), &error); deck_wait(&window, 2, RADIOS_AUDIO_UI_EMPTY, 0);
    click(card, "deck-0-autodj-start");
    const guint decks[] = {0, 3, 1, 3, 2};
    for (guint index = 0; index < 5; ++index) {
        RadioSPlayoutSnapshot playing = rotation_run_wait(&window, RADIOS_PLAYOUT_PLAYING, index, 300);
        g_assert_cmpuint(playing.total, ==, 5); g_assert_cmpint(playing.deck, ==, decks[index]); g_assert_cmpstr(playing.title, ==, titles[index]);
        RadioSAudioRunSnapshot lease; radios_audio_engine_player_runner_snapshot(engine, 0, &lease);
        for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) g_assert_cmpint(lease.owned[deck], ==, deck < 4);
        goffset pcm = capture_offset("MAIN"); run_for(250);
        g_assert_cmpfloat(tone_energy("MAIN", pcm, frequencies[index]), >, .06); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1100), >, .03);
        RadioSAudioUiSnapshot after; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 4, &after));
        g_assert_cmpuint(after.generation, ==, untouched.generation); g_assert_cmpint(after.state, ==, RADIOS_AUDIO_UI_PLAYING); g_assert_cmpstr(after.media_id, ==, untouched.media_id);
    }
    rotation_run_wait(&window, RADIOS_PLAYOUT_FINISHED, 4, 0); RadioSAudioUiSnapshot after; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 4, &after));
    g_assert_cmpuint(after.generation, ==, untouched.generation); g_assert_cmpint(after.state, ==, RADIOS_AUDIO_UI_PLAYING);
    g_assert_cmpfloat(measure("CUE").peak[0], <, .0001); click(window.root, "deck-4-stop"); run_for(200);
    calendar_schedule_unchanged(saved); assert_saved_playlist_unchanged(playlist); radios_audio_engine_unref(engine);
    for (guint i = 0; i < G_N_ELEMENTS(media); ++i) json_node_free(media[i]);
    json_node_free(manual); json_node_free(playlist); json_node_free(configuration); json_node_free(saved);
    g_free(id); g_free(document); window_close(&window);
    g_test_message("Real source right-click, required-kind validation, cancel/reconnect/reopen persistence and 0/3/1/3/2 routed MAIN PCM preserve independent manual player and CUE.");
}
static void test_named_rotation_local_beatmatch_effect_pcm(void)
{
    Window window = window_open(); seed_programme(&window, NULL, NULL); navigate(&window, "broadcast"); players_show(&window);
    JsonNode *first = routing_short_markers(media_import_rhythm(&window, "Rotation measured beat A", 700, .25, "music", 120), 4000);
    JsonNode *second = routing_short_markers(media_import_rhythm(&window, "Rotation measured beat B", 1700, .25, "music", 128), 4000);
    RadioSRhythmAnalysis grids[2]; JsonNode *details[] = {first, second};
    for (guint i = 0; i < G_N_ELEMENTS(details); ++i) {
        JsonObject *analysis = json_object_get_object_member(json_node_get_object(details[i]), "analysis");
        g_assert_true(radios_rhythm_analysis_read(json_object_get_object_member(analysis, "rhythm"), &grids[i]));
        g_assert_cmpfloat(grids[i].bpm, >, 100); g_assert_cmpfloat(grids[i].bpm, <, 140);
    }
    JsonNode *playlist = playlist_create("Verified local beatmatch rotation playlist", first, second);
    JsonNode *configuration = routing_configuration(playlist, 2, TRUE);
    JsonObject *definition = json_array_get_object_element(json_object_get_array_member(json_node_get_object(configuration), "rotation_library"), 0);
    gchar *id = g_strdup(radios_json_string(definition, "id", "")); JsonNode *saved = document_save("automation_calendar", "main", json_node_get_object(configuration));
    gchar *document = g_strdup(radios_json_string(json_node_get_object(saved), "_id", "")); window_close(&window); window = window_open(); routing_ui_apply(&window, id);
    json_node_free(saved); saved = document_read("automation_calendar", "main"); assert_routed_pools(routed_source(saved));
    navigate(&window, "broadcast"); players_show(&window); combo(window.root, "deck-0-autodj-target", "-1");
    const gchar *titles[] = {"Rotation measured beat A", "Rotation measured beat B"}; routing_named_load(&window, id, document, titles, 2);
    GtkWidget *card = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0");
    combo(card, "deck-0-autodj-mode", "2"); click(card, "deck-0-autodj-start");
    RadioSPlayoutSnapshot playing = rotation_run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 300);
    g_assert_cmpint(playing.deck, ==, 0); g_assert_cmpint(playing.next_deck, ==, 1);
    g_assert_cmpint(playing.transition, ==, RADIOS_AUDIO_RUN_CROSSFADE); g_assert_cmpuint(playing.overlap_ms, ==, 500);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); RadioSAudioRunSnapshot lease; radios_audio_engine_player_runner_snapshot(engine, 0, &lease);
    for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) g_assert_cmpint(lease.owned[deck], ==, deck < 2);
    playing = rotation_run_wait(&window, RADIOS_PLAYOUT_OVERLAP, 0, 0); g_assert_cmpint(playing.next_deck, ==, 1);
    RadioSAudioUiSnapshot synced; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 1, &synced));
    g_assert_cmpfloat(fabs(synced.tempo_ratio - grids[0].bpm / grids[1].bpm), <, .0001);
    g_assert_cmpfloat(fabs(synced.effective_bpm - grids[0].bpm), <, .01);
    goffset pcm = capture_offset("MAIN"); run_for(200); g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .01); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .005);
    playing = rotation_run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 800); g_assert_cmpint(playing.deck, ==, 1);
    pcm = capture_offset("MAIN"); run_for(350); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .05);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700 * grids[0].bpm / grids[1].bpm), <, .025);
    g_assert_cmpfloat(measure("CUE").peak[0], <, .0001); rotation_stop_run(&window); run_for(200);
    calendar_schedule_unchanged(saved); assert_saved_playlist_unchanged(playlist); radios_audio_engine_unref(engine);
    json_node_free(first); json_node_free(second); json_node_free(playlist); json_node_free(configuration); json_node_free(saved); g_free(id); g_free(document); window_close(&window);
    g_test_message("Genuine measured local rotation beatmatch/effect rule selects routed music cards, real fractional atempo, audible overlap and original pitch without using draft BPM or unrelated deck leases.");
}


/* Per-song probability is exercised through real verified media, the actual
 * saved-rotation picker, native AutoDJ cards and private MAIN PCM. Caps make
 * both positive candidates obligatory without assuming any random draw order. */
static void test_named_rotation_weighted_atomic_disabled_pcm(void)
{
    Window window = window_open(); seed_programme(&window, NULL, NULL); navigate(&window, "broadcast"); players_show(&window);
    const gchar *titles[] = {"Weighted disabled atomic A", "Weighted eligible atomic B", "Weighted eligible atomic C"};
    const gdouble frequencies[] = {700, 1700, 2700}; JsonNode *media[3];
    for (guint i = 0; i < G_N_ELEMENTS(media); ++i)
        media[i] = routing_short_markers(media_import(&window, titles[i], frequencies[i], .22, "music"), 1800);
    JsonNode *playlist = playlist_create("Weighted real atomic capped playlist", media[0], media[1]);
    playlist = playlist_append_verified(playlist, media[2]);
    JsonArray *tracks = json_object_get_array_member(json_node_get_object(playlist), "tracks"); g_assert_cmpuint(json_array_get_length(tracks), ==, 3);
    JsonNode *configuration = routing_configuration(playlist, 2, FALSE);
    JsonObject *definition = json_array_get_object_element(json_object_get_array_member(json_node_get_object(configuration), "rotation_library"), 0);
    json_object_set_string_member(definition, "name", "Actual weighted atomic disabled-tone rotation");
    JsonObject *settings = json_object_get_object_member(definition, "settings"); json_object_set_string_member(settings, "selection_mode", "weighted");
    JsonObject *source = json_array_get_object_element(json_object_get_array_member(definition, "sources"), 0);
    json_object_set_int_member(source, "weight", 100); json_object_set_string_member(source, "track_selection_mode", "weighted");
    json_object_set_boolean_member(source, "track_random", TRUE); JsonArray *rules = json_array_new();
    for (guint i = 0; i < G_N_ELEMENTS(media); ++i) {
        JsonObject *rule = json_object_new(); const gchar *entry_id = radios_json_string(json_array_get_object_element(tracks, i), "_id", "");
        g_assert_true(g_uuid_string_is_valid(entry_id)); json_object_set_string_member(rule, "entry_id", entry_id);
        json_object_set_int_member(rule, "weight", i == 0 ? 0 : (i == 1 ? 60 : 40));
        json_object_set_int_member(rule, "order", i); json_object_set_int_member(rule, "frequency", 1);
        json_object_set_int_member(rule, "max_plays", 1); json_object_set_int_member(rule, "start_minute", 0);
        json_object_set_int_member(rule, "end_minute", 1440); json_object_set_int_member(rule, "interval_minutes", 0);
        json_array_add_object_element(rules, rule);
    }
    json_object_set_array_member(source, "track_rules", rules);
    gchar *id = g_strdup(radios_json_string(definition, "id", ""));
    JsonNode *saved = document_save("automation_calendar", "main", json_node_get_object(configuration));
    gchar *document = g_strdup(radios_json_string(json_node_get_object(saved), "_id", ""));
    window_close(&window); window = window_open(); routing_ui_apply(&window, id);
    json_node_free(saved); saved = document_read("automation_calendar", "main"); source = routed_source(saved); assert_routed_pools(source);
    g_assert_cmpstr(radios_json_string(source, "track_selection_mode", ""), ==, "weighted");
    rules = json_object_get_array_member(source, "track_rules"); g_assert_cmpuint(json_array_get_length(rules), ==, 3);
    for (guint i = 0; i < 3; ++i) {
        JsonObject *rule = json_array_get_object_element(rules, i); g_assert_cmpuint(json_object_get_size(rule), ==, 8);
        g_assert_cmpstr(radios_json_string(rule, "entry_id", ""), ==, radios_json_string(json_array_get_object_element(tracks, i), "_id", ""));
        g_assert_cmpint(radios_json_int(rule, "weight", -1), ==, i == 0 ? 0 : (i == 1 ? 60 : 40));
        g_assert_cmpint(radios_json_int(rule, "max_plays", -1), ==, 1);
    }
    navigate(&window, "broadcast"); players_show(&window); combo(window.root, "deck-0-autodj-target", "-1");
    NativeRotationPicker picker = native_rotation_picker(&window, id); GtkWidget *load = find(picker.dialog, "player-rotation-load");
    wait_until(widget_enabled, load, "actual weighted atomic preparation enables native Load");
    GtkTreeModel *preview = gtk_tree_view_get_model(GTK_TREE_VIEW(find(picker.dialog, "player-rotation-preview")));
    g_assert_cmpint(gtk_tree_model_iter_n_children(preview, NULL), ==, 2); GtkTreeIter iter; g_assert_true(gtk_tree_model_get_iter_first(preview, &iter));
    gboolean preview_seen[3] = {FALSE, FALSE, FALSE};
    for (guint index = 0; index < 2; ++index) {
        gchar *title = NULL; gtk_tree_model_get(preview, &iter, 1, &title, -1);
        guint selected = !g_strcmp0(title, titles[1]) ? 1 : (!g_strcmp0(title, titles[2]) ? 2 : 0);
        g_assert_cmpuint(selected, >, 0); g_assert_false(preview_seen[selected]); preview_seen[selected] = TRUE; g_free(title);
        if (!index) g_assert_true(gtk_tree_model_iter_next(preview, &iter));
    }
    g_assert_true(preview_seen[1]); g_assert_true(preview_seen[2]); gtk_button_clicked(GTK_BUTTON(load));
    GtkWidget *card = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0"); NativeRotationCard ready = {card, id, document, 2};
    wait_until(native_rotation_card_ready, &ready, "real weighted final acceptance atomically assigns the rotation card");
    GtkWidget *inputs = saved_player_find_class(card, "lp-input-list"); GList *rows = gtk_container_get_children(GTK_CONTAINER(inputs));
    g_assert_cmpuint(g_list_length(rows), ==, 2); gboolean accepted_seen[3] = {FALSE, FALSE, FALSE};
    for (guint index = 0; index < 2; ++index) {
        GtkWidget *label = saved_player_find_class(g_list_nth_data(rows, index), "lp-input-title"); g_assert_true(GTK_IS_LABEL(label));
        gchar *b = g_strdup_printf("%u. %s", index + 1, titles[1]), *c = g_strdup_printf("%u. %s", index + 1, titles[2]);
        const gchar *text = gtk_label_get_text(GTK_LABEL(label)); guint selected = !g_strcmp0(text, b) ? 1 : (!g_strcmp0(text, c) ? 2 : 0);
        g_assert_cmpuint(selected, >, 0); g_assert_false(accepted_seen[selected]); accepted_seen[selected] = TRUE; g_free(b); g_free(c);
    }
    g_list_free(rows); g_assert_true(accepted_seen[1]); g_assert_true(accepted_seen[2]);
    /* Loading a rotation selects Yarı AutoDJ (one player); the routed pair is
     * the between-players mode, chosen as in the sibling routed cases. */
    combo(card, "deck-0-autodj-mode", "2");
    GtkWidget *start = find(card, "deck-0-autodj-start"); wait_until(widget_enabled, start, "saved weighted rotation authorizes the actual routed AutoDJ card");
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine); click(card, "deck-0-autodj-start");
    gboolean played[3] = {FALSE, FALSE, FALSE};
    for (guint index = 0; index < 2; ++index) {
        RadioSPlayoutSnapshot playing = rotation_run_wait(&window, RADIOS_PLAYOUT_PLAYING, index, 300);
        g_assert_cmpuint(playing.total, ==, 2); g_assert_cmpint(playing.deck, ==, index); g_assert_cmpuint(playing.overlap_ms, ==, 0);
        guint selected = !g_strcmp0(playing.title, titles[1]) ? 1 : (!g_strcmp0(playing.title, titles[2]) ? 2 : 0);
        g_assert_cmpuint(selected, >, 0); g_assert_false(played[selected]); played[selected] = TRUE;
        RadioSAudioUiSnapshot audio; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, playing.deck, &audio));
        g_assert_cmpstr(audio.media_id, ==, radios_json_string(json_object_get_object_member(json_node_get_object(media[selected]), "media"), "_id", ""));
        RadioSAudioRunSnapshot lease; radios_audio_engine_player_runner_snapshot(engine, 0, &lease);
        for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) g_assert_cmpint(lease.owned[deck], ==, deck < 2);
        goffset pcm = capture_offset("MAIN"); run_for(250);
        g_assert_cmpfloat(tone_energy("MAIN", pcm, frequencies[selected]), >, .06);
        g_assert_cmpfloat(tone_energy("MAIN", pcm, frequencies[0]), <, .01);
        g_assert_cmpfloat(tone_energy("MAIN", pcm, frequencies[selected == 1 ? 2 : 1]), <, .01);
    }
    g_assert_false(played[0]); g_assert_true(played[1]); g_assert_true(played[2]); rotation_run_wait(&window, RADIOS_PLAYOUT_FINISHED, 1, 0);
    g_assert_cmpfloat(measure("CUE").peak[0], <, .0001); calendar_schedule_unchanged(saved); assert_saved_playlist_unchanged(playlist);
    radios_audio_engine_unref(engine); for (guint i = 0; i < G_N_ELEMENTS(media); ++i) json_node_free(media[i]);
    json_node_free(playlist); json_node_free(configuration); json_node_free(saved); g_free(id); g_free(document); window_close(&window);
    g_test_message("Real persisted weighted atomic rules exclude the disabled 700Hz song from preview, accepted card and MAIN PCM; capped positive 1700/2700Hz songs each play once on the actual routed pair, with no CUE or saved-document mutation.");
}

typedef struct { const gchar *name; GtkWidget *dialog, *list; GtkTreeIter iter; } WeeklyPickerSelection;
static gboolean weekly_picker_selection_ready(gpointer value)
{
    WeeklyPickerSelection *choice = value; choice->dialog = dialog_find("player-weekly-dialog");
    if (!choice->dialog) return FALSE;
    choice->list = find(choice->dialog, "player-weekly-list"); if (!GTK_IS_TREE_VIEW(choice->list)) return FALSE;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(choice->list)); GtkTreeIter iter;
    for (gboolean valid = gtk_tree_model_get_iter_first(model, &iter); valid; valid = gtk_tree_model_iter_next(model, &iter)) {
        gchar *name = NULL; gtk_tree_model_get(model, &iter, 0, &name, -1);
        gboolean matches = !g_strcmp0(name, choice->name); g_free(name);
        if (matches) { choice->iter = iter; return TRUE; }
    }
    return FALSE;
}
typedef struct { GtkWidget *card; const gchar *name; } WeeklyCardLoaded;
static gboolean weekly_card_loaded(gpointer value)
{
    WeeklyCardLoaded *condition = value; RadioSPlayerWeekly *weekly = g_object_get_data(G_OBJECT(condition->card), "radios-player-weekly");
    RadioSPlayerWeeklySnapshot snapshot = {0};
    return !dialog_find("player-weekly-dialog") && weekly && radios_player_weekly_get_snapshot(weekly, &snapshot) &&
        !g_strcmp0(snapshot.programme_name, condition->name) && snapshot.state == RADIOS_PLAYER_WEEKLY_LOADED && !snapshot.armed;
}
static RadioSPlayerWeekly *weekly_native_load(Window *window, guint deck, const gchar *name)
{
    gchar *key = g_strdup_printf("radios-deck-owner-%u", deck); GtkWidget *card = g_object_get_data(G_OBJECT(window->ctx->parent), key); g_free(key);
    g_assert_nonnull(card); key = g_strdup_printf("deck-%u-weekly-programme", deck); click(card, key); g_free(key);
    WeeklyPickerSelection choice = {.name = name}; wait_until(weekly_picker_selection_ready, &choice, "actual typed saved-programme picker reads private database");
    gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(choice.list)), &choice.iter);
    GtkWidget *preview = find(choice.dialog, "player-weekly-preview"); g_assert_true(GTK_IS_TREE_VIEW(preview));
    g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(preview)), NULL), ==, 1);
    GtkWidget *load = find(choice.dialog, "player-weekly-load"); wait_until(widget_enabled, load, "actual saved-programme picker load enabled");
    gtk_button_clicked(GTK_BUTTON(load)); WeeklyCardLoaded loaded = {card, name};
    wait_until(weekly_card_loaded, &loaded, "saved weekly document accepted without starting audio");
    return g_object_get_data(G_OBJECT(card), "radios-player-weekly");
}

static JsonNode *weekly_pcm_configuration(JsonNode *playlist, JsonNode *disabled, gint64 start_us, guint minutes, const gchar *title)
{
    GDateTime *start = g_date_time_new_from_unix_utc(start_us / G_USEC_PER_SEC);
    gchar *date = g_date_time_format(start, "%Y-%m-%d");
    guint minute = (guint)(g_date_time_get_hour(start) * 60 + g_date_time_get_minute(start));
    g_assert_cmpuint(minute + minutes, <=, 1440);
    JsonNode *calendar = calendar_schedule_config(date, radios_json_string(json_node_get_object(playlist), "_id", ""), 0);
    JsonObject *config = json_node_get_object(calendar);
    JsonObject *event = json_array_get_object_element(json_object_get_array_member(config, "events"), 0);
    json_object_set_int_member(event, "start_minute", minute); json_object_set_int_member(event, "end_minute", minute + minutes);
    json_object_set_string_member(event, "title", title); json_object_set_string_member(event, "rotation_selection_mode", "weighted");
    JsonArray *library = json_object_get_array_member(config, "rotation_library");
    JsonObject *definition = json_array_get_object_element(library, 0);
    json_object_set_string_member(definition, "name", "Weekly selected verified tone");
    JsonObject *source = json_array_get_object_element(json_object_get_array_member(definition, "sources"), 0);
    json_object_set_int_member(source, "count", 1); json_object_set_int_member(source, "interval_minutes", 10080);
    JsonArray *assignments = json_object_get_array_member(event, "rotations");
    json_object_set_int_member(json_array_get_object_element(assignments, 0), "weight", 100);
    if (disabled) {
        JsonNode *copy = deep_copy(json_array_get_element(library, 0)); JsonObject *other = json_node_get_object(copy);
        gchar *id = g_uuid_string_random(), *source_id = g_uuid_string_random(), *assignment_id = g_uuid_string_random();
        json_object_set_string_member(other, "id", id); json_object_set_string_member(other, "name", "Disabled zero-weight weekly rotation");
        JsonObject *other_source = json_array_get_object_element(json_object_get_array_member(other, "sources"), 0);
        json_object_set_string_member(other_source, "id", source_id);
        json_object_set_string_member(other_source, "playlist_id", radios_json_string(json_node_get_object(disabled), "_id", ""));
        json_array_add_element(library, copy);
        JsonObject *assignment = json_object_new(); json_object_set_string_member(assignment, "id", assignment_id);
        json_object_set_string_member(assignment, "rotation_id", id); json_object_set_int_member(assignment, "weight", 0);
        json_array_add_object_element(assignments, assignment); g_free(id); g_free(source_id); g_free(assignment_id);
    }
    GDateTime *monday = g_date_time_add_days(start, 1 - g_date_time_get_day_of_week(start));
    gchar *anchor = g_date_time_format(monday, "%Y-%m-%d");
    GError *error = NULL; JsonNode *programme = radios_weekly_programme_snapshot(config, anchor, FALSE, &error);
    g_free(anchor); g_date_time_unref(monday);
    g_assert_no_error(error); g_assert_nonnull(programme);
    json_node_free(calendar); g_free(date); g_date_time_unref(start); return programme;
}
static RadioSPlayerWeeklySnapshot weekly_snapshot(RadioSPlayerWeekly *weekly)
{
    RadioSPlayerWeeklySnapshot snapshot = {0}; g_assert_true(radios_player_weekly_get_snapshot(weekly, &snapshot)); return snapshot;
}
static void weekly_record_unchanged(JsonNode *expected)
{
    JsonObject *record = json_node_get_object(expected);
    JsonNode *actual = document_read("weekly_programme", radios_json_string(record, "name", ""));
    g_assert_nonnull(actual); g_assert_true(json_node_equal(expected, actual)); json_node_free(actual);
}
static void weekly_wait_real_time(gint64 deadline_us)
{
    while (g_get_real_time() < deadline_us) pump();
}

static void test_weekly_programme_future_due_repeat_weighted_pcm(void)
{
    Window window = window_open(); seed_programme(&window, NULL, NULL); navigate(&window, "broadcast"); players_show(&window);
    JsonNode *positive = routing_short_markers(media_import(&window, "Weekly actual positive tone", 2350, .22, "music"), 1800);
    JsonNode *negative = routing_short_markers(media_import(&window, "Weekly disabled rotation tone", 750, .22, "music"), 1800);
    JsonNode *playlist = playlist_create("Weekly positive verified playlist", positive, NULL);
    JsonNode *disabled = playlist_create("Weekly zero-weight verified playlist", negative, NULL);
    gint64 now = g_get_real_time(), minute = 60 * G_USEC_PER_SEC;
    gint64 start = (now / minute + 1) * minute; if (start - now < 15 * G_USEC_PER_SEC) start += minute;
    /* Do not change the host clock. The production minute-granularity block
     * is tested at its actual UTC boundary, including the next-cycle wait. */
    JsonNode *configuration = weekly_pcm_configuration(playlist, disabled, start, 1, "Gerçek UTC haftalık yayın bloğu");
    JsonNode *saved = document_save("weekly_programme", "Actual future weekly programme", json_node_get_object(configuration));
    combo(window.root, "deck-0-autodj-target", "-1");
    RadioSPlayerWeekly *weekly = weekly_native_load(&window, 0, "Actual future weekly programme");
    RadioSPlayerWeeklySnapshot loaded = weekly_snapshot(weekly);
    g_assert_false(loaded.repeat); g_assert_cmpint(loaded.next_start_utc_us, ==, start); g_assert_cmpint(loaded.remaining_us, >, 0);
    g_assert_cmpstr(loaded.next_event_name, ==, "Gerçek UTC haftalık yayın bloğu");
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    RadioSPlayoutSnapshot quiet = {0}; g_assert_true(radios_playout_ui_get_snapshot(window.ctx, &quiet)); g_assert_cmpint(quiet.state, ==, RADIOS_PLAYOUT_IDLE);
    GtkWidget *card = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0");
    GtkWidget *source_label = find(card, "deck-0-rotation-status"); g_assert_true(GTK_IS_LABEL(source_label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(source_label)), ==, "Actual future weekly programme · program hazır");
    toggle(card, "deck-0-weekly-repeat", TRUE); click(card, "deck-0-play"); run_for(350);
    RadioSPlayerScopeExecutionSnapshot waiting = rotation_execution_snapshot(&window);
    g_assert_true(waiting.schema_active); g_assert_true(waiting.repeat); g_assert_cmpint(waiting.state, ==, RADIOS_SCOPE_EXECUTION_WAITING);
    g_assert_cmpint(waiting.starts_at_utc_us, ==, start); g_assert_cmpstr(waiting.event_name, ==, "Gerçek UTC haftalık yayın bloğu");
    GtkWidget *countdown = find(card, "deck-0-weekly-next"); g_assert_true(GTK_IS_LABEL(countdown));
    gchar *before_label = g_strdup(gtk_label_get_text(GTK_LABEL(countdown))); run_for(1200);
    RadioSPlayerScopeExecutionSnapshot later = rotation_execution_snapshot(&window);
    g_assert_true(later.schema_active); g_assert_cmpint(later.starts_at_utc_us, ==, waiting.starts_at_utc_us);
    g_assert_cmpstr(before_label, !=, gtk_label_get_text(GTK_LABEL(countdown))); g_free(before_label);
    saved_player_capture(window.root, "RADIOS_WEEKLY_COUNTDOWN_CAPTURE");
    g_test_message("Waiting for genuine UTC programme due boundary in %.3f seconds", (start - g_get_real_time()) / 1000000.0);
    gint64 deadline = g_get_monotonic_time() + 90 * G_TIME_SPAN_SECOND; RadioSPlayoutSnapshot playing = {0};
    while (g_get_monotonic_time() < deadline) {
        pump(); radios_player_workspace_snapshot(window.ctx->players, 0, &playing);
        if (playing.state == RADIOS_PLAYOUT_PLAYING && playing.position_ms >= 200) break;
    }
    g_assert_cmpint(playing.state, ==, RADIOS_PLAYOUT_PLAYING); g_assert_cmpstr(playing.title, ==, "Weekly actual positive tone");
    g_assert_cmpuint(playing.total, ==, 1); g_assert_cmpint(playing.deck, ==, 0);
    RadioSAudioUiSnapshot audio = deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 200);
    g_assert_cmpstr(audio.media_id, ==, radios_json_string(json_object_get_object_member(json_node_get_object(positive), "media"), "_id", ""));
    goffset pcm = capture_offset("MAIN"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 2350), >, .07); g_assert_cmpfloat(tone_energy("MAIN", pcm, 750), <, .01);
    g_assert_cmpfloat(measure("CUE").peak[0], <, .0001);
    /* The finite first batch ends before this real block does. The saved
     * source interval must survive EOF/refill, rather than replaying it. */
    run_for(2500);
    RadioSPlayerScopeExecutionSnapshot after_eof = rotation_execution_snapshot(&window);
    g_assert_true(after_eof.schema_active); g_assert_cmpint(after_eof.state, ==, RADIOS_SCOPE_EXECUTION_WAITING);
    g_assert_cmpstr(after_eof.event_name, ==, "Gerçek UTC haftalık yayın bloğu");
    g_assert_cmpint(after_eof.starts_at_utc_us, ==, start);
    goffset quiet_after_eof = capture_offset("MAIN"); run_for(500);
    Capture quiet_pcm = capture("MAIN", quiet_after_eof);
    g_assert_cmpfloat(quiet_pcm.peak[0], <, .0001);
    g_assert_cmpfloat(tone_energy("MAIN", quiet_after_eof, 2350), <, .01);
    g_assert_cmpfloat(tone_energy("MAIN", quiet_after_eof, 750), <, .01);
    g_test_message("Actual2350Hz played once; real EOF/refill retained source interval and MAIN silence before block end.");
    weekly_wait_real_time(start + minute + 600000);
    RadioSPlayerScopeExecutionSnapshot repeated = rotation_execution_snapshot(&window);
    g_assert_true(repeated.schema_active); g_assert_true(repeated.repeat); g_assert_cmpint(repeated.state, ==, RADIOS_SCOPE_EXECUTION_WAITING);
    g_assert_cmpstr(repeated.event_name, ==, "Gerçek UTC haftalık yayın bloğu");
    g_assert_cmpint(repeated.starts_at_utc_us, ==, start + 7 * G_TIME_SPAN_DAY);
    g_assert_cmpint(repeated.starts_at_utc_us - g_get_real_time(), >, 6 * G_TIME_SPAN_DAY);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    const gchar *positive_id = radios_json_string(json_object_get_object_member(json_node_get_object(positive), "media"), "_id", "");
    const gchar *negative_id = radios_json_string(json_object_get_object_member(json_node_get_object(negative), "media"), "_id", "");
    JsonNode *history = NULL; gint64 history_deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < history_deadline) {
        g_clear_pointer(&history, json_node_free); history = document_read("playout_history", "Oynatıcı 1 AutoDJ geçmişi");
        if (history_count(history, positive_id, NULL, NULL) > 0) break;
        run_for(250);
    }
    g_assert_cmpuint(history_count(history, positive_id, NULL, NULL), ==, 1);
    g_assert_cmpuint(history_count(history, negative_id, NULL, NULL), ==, 0); json_node_free(history);
    toggle(card, "deck-0-weekly-repeat", FALSE); run_for(500);
    RadioSPlayerScopeExecutionSnapshot finished = rotation_execution_snapshot(&window);
    g_assert_false(finished.repeat); g_assert_false(finished.schema_active); g_assert_cmpint(finished.state, ==, RADIOS_SCOPE_EXECUTION_IDLE);
    g_assert_cmpint(finished.starts_at_utc_us, ==, 0); weekly_record_unchanged(saved);
    assert_saved_playlist_unchanged(playlist); assert_saved_playlist_unchanged(disabled);
    json_node_free(positive); json_node_free(negative); json_node_free(playlist); json_node_free(disabled); json_node_free(configuration); json_node_free(saved); window_close(&window);
}

static RadioSPlayerWeekly *general_driver(Window *window)
{
    GtkWidget *continuous = find(window->root, "programme-general-continuous"); g_assert_nonnull(continuous);
    GtkWidget *page = continuous; RadioSPlayerWeekly *driver = NULL;
    while (page && !driver) { driver = g_object_get_data(G_OBJECT(page), "radios-general-programme-driver"); page = gtk_widget_get_parent(page); }
    g_assert_nonnull(driver); return driver;
}
static void general_single_batch(JsonObject *calendar, JsonNode *playlist)
{
    JsonObject *definition = json_array_get_object_element(json_object_get_array_member(calendar, "rotation_library"), 0);
    JsonObject *source = json_array_get_object_element(json_object_get_array_member(definition, "sources"), 0);
    json_object_set_int_member(source, "interval_minutes", 0);
    JsonArray *tracks = json_object_get_array_member(json_node_get_object(playlist), "tracks"), *rules = json_array_new();
    JsonObject *rule = json_object_new();
    json_object_set_string_member(rule, "entry_id", radios_json_string(json_array_get_object_element(tracks, 0), "_id", ""));
    json_object_set_int_member(rule, "weight", 100); json_object_set_int_member(rule, "order", 0);
    json_object_set_int_member(rule, "frequency", 1); json_object_set_int_member(rule, "max_plays", 1);
    json_object_set_int_member(rule, "start_minute", 0); json_object_set_int_member(rule, "end_minute", 1440);
    json_object_set_int_member(rule, "interval_minutes", 0); json_array_add_object_element(rules, rule);
    json_object_set_array_member(source, "track_rules", rules);
}
typedef struct { Window *window; RadioSPlayerWeekly *driver; const gchar *title; gint64 start; } GeneralReady;
static gboolean general_future_ready(gpointer value)
{
    GeneralReady *ready = value; RadioSPlayerWeeklySnapshot snapshot = weekly_snapshot(ready->driver);
    return !snapshot.armed && snapshot.state == RADIOS_PLAYER_WEEKLY_LOADED && snapshot.next_start_utc_us == ready->start &&
        g_str_equal(snapshot.next_event_name, ready->title);
}
static gboolean general_retry_visible(gpointer value)
{
    RadioSPlayerWeeklySnapshot snapshot = weekly_snapshot(value);
    return snapshot.armed && snapshot.state == RADIOS_PLAYER_WEEKLY_WAITING && snapshot.error[0];
}
static RadioSPlayoutSnapshot general_pcm_wait(Window *window, const gchar *title, guint64 position, guint seconds)
{
    gint64 until = g_get_monotonic_time() + (gint64)seconds * G_USEC_PER_SEC; RadioSPlayoutSnapshot snapshot = {0};
    do {
        pump(); radios_playout_ui_get_snapshot(window->ctx, &snapshot);
        if (snapshot.state == RADIOS_PLAYOUT_PLAYING && snapshot.position_ms >= position && g_str_equal(snapshot.title, title)) return snapshot;
    } while (g_get_monotonic_time() < until);
    g_error("General PCM expected %s, got state%d title%s error%s", title, snapshot.state, snapshot.title, snapshot.error); return snapshot;
}
static gboolean general_current_mixed_ready(gpointer value)
{
    GeneralReady *ready = value; RadioSPlayerWeeklySnapshot snapshot = weekly_snapshot(ready->driver);
    return !snapshot.armed && snapshot.state == RADIOS_PLAYER_WEEKLY_LOADED && !snapshot.error[0] &&
        snapshot.start_utc_us == ready->start && g_str_equal(snapshot.current_event_name, ready->title) && queue_contents(ready->window) > 0;
}
static void test_general_header_play_local_pcm(void)
{
    Window window = window_open(); navigate(&window, "broadcast"); RadioSPlayerWeekly *driver = general_driver(&window);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(window.root, "programme-general-continuous"))));
    JsonNode *tone = routing_short_markers(media_import(&window, "General local tone", 950, .22, "music"), 1800);
    JsonNode *local = playlist_create("Gece local verified source", tone, NULL);
    /* Cover a block active right now without changing the host clock or
     * waiting for a minute boundary. This fixture's repeat gaps are zero. */
    gint64 start = g_get_real_time() / G_TIME_SPAN_DAY * G_TIME_SPAN_DAY;
    JsonNode *configuration = weekly_pcm_configuration(local, NULL, start, 1440, "Gece Kulübü");
    JsonObject *calendar = json_object_get_object_member(json_node_get_object(configuration), "calendar");
    general_single_batch(calendar, local);
    JsonObject *definition = json_array_get_object_element(json_object_get_array_member(calendar, "rotation_library"), 0);
    json_object_set_string_member(definition, "name", "Gece Kulübü");
    JsonNode *saved = document_save("automation_calendar", "main", calendar); radios_player_weekly_general_refresh(driver);
    GeneralReady ready = {&window, driver, "Gece Kulübü", start};
    wait_until(general_current_mixed_ready, &ready, "active Gece block is projected before main header Play");
    g_assert_cmpint(queue_contents(&window), ==, 1); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    click(window.root, "header-play");
    RadioSPlayoutSnapshot playing = general_pcm_wait(&window, "General local tone", 200, 15);
    g_assert_cmpuint(playing.total, ==, 1);
    RadioSAudioUiSnapshot audio; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, (guint)playing.deck, &audio));
    g_assert_cmpint(audio.state, ==, RADIOS_AUDIO_UI_PLAYING);
    g_assert_cmpstr(audio.media_id, ==, radios_json_string(json_object_get_object_member(json_node_get_object(tone), "media"), "_id", ""));
    RadioSAudioRunSnapshot runner; radios_audio_engine_runner_snapshot(engine, &runner); g_assert_cmpint(runner.state, ==, RADIOS_AUDIO_RUN_PLAYING);
    RadioSPlayerWeeklySnapshot current = weekly_snapshot(driver); g_assert_true(current.armed); g_assert_cmpstr(current.current_event_name, ==, "Gece Kulübü");
    goffset pcm = capture_offset("MAIN"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 950), >, .07);
    g_assert_cmpfloat(measure("CUE").peak[0], <, .0001); click(window.root, "header-stop"); run_for(300);
    g_assert_false(weekly_snapshot(driver).armed); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    calendar_schedule_unchanged(saved); assert_saved_playlist_unchanged(local);
    radios_audio_engine_unref(engine); window_close(&window);
    json_node_free(tone); json_node_free(local); json_node_free(configuration); json_node_free(saved);
}
static void test_general_future_refill_adjacent_reconnect_PCM(void)
{
    Window window = window_open(); navigate(&window, "broadcast");
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(window.root, "programme-general-continuous"))));
    RadioSPlayerWeekly *driver = general_driver(&window);
    JsonNode *a = routing_short_markers(media_import(&window, "General future A", 950, .22, "music"), 1800);
    JsonNode *b = routing_short_markers(media_import(&window, "General future B", 2350, .22, "music"), 1800);
    JsonNode *pa = playlist_create("General future A source", a, NULL), *pb = playlist_create("General future B source", b, NULL);
    gint64 minute = 60 * G_USEC_PER_SEC, now = g_get_real_time(), start = (now / minute + 1) * minute;
    if (start - now < 12 * G_USEC_PER_SEC) start += minute;
    JsonNode *first = weekly_pcm_configuration(pa, NULL, start, 1, "General nearest future main");
    JsonObject *main_calendar = json_object_get_object_member(json_node_get_object(first), "calendar"); general_single_batch(main_calendar, pa);
    /* A slot covering now plays at once (general_tick plans an active
     * occurrence from now); the skipped slot has already ended. */
    JsonNode *past = weekly_pcm_configuration(pa, NULL, now / minute * minute - minute, 1, "General past slot must stay skipped");
    JsonObject *past_calendar = json_object_get_object_member(json_node_get_object(past), "calendar");
    json_array_add_element(json_object_get_array_member(main_calendar, "events"), deep_copy(json_array_get_element(json_object_get_array_member(past_calendar, "events"), 0)));
    json_array_add_element(json_object_get_array_member(main_calendar, "rotation_library"), deep_copy(json_array_get_element(json_object_get_array_member(past_calendar, "rotation_library"), 0)));
    JsonNode *saved_main = document_save("automation_calendar", "main", main_calendar);
    JsonNode *second = weekly_pcm_configuration(pb, NULL, start + minute, 1, "General adjacent saved weekly");
    general_single_batch(json_object_get_object_member(json_node_get_object(second), "calendar"), pb);
    JsonNode *saved_weekly = document_save("weekly_programme", "General later saved schedule", json_node_get_object(second));
    radios_player_weekly_general_refresh(driver);
    GeneralReady ready = {&window, driver, "General nearest future main", start};
    wait_until(general_future_ready, &ready, "general default projects the nearest future main programme without PCM");
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); RadioSAudioSnapshot before, after;
    radios_audio_engine_snapshot(engine, &before); click(window.root, "header-play"); run_for(250);
    g_assert_true(weekly_snapshot(driver).armed); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    click(window.root, "header-pause"); g_assert_true(weekly_snapshot(driver).paused);
    click(window.root, "header-play"); run_for(250); g_assert_false(weekly_snapshot(driver).paused);
    radios_audio_engine_snapshot(engine, &after);
    for (guint deck = 0; deck < 32; ++deck) g_assert_cmpuint(before.decks[deck].generation, ==, after.decks[deck].generation);
    g_test_message("General schedule waits %.3fs for actual minute boundary", (start - g_get_real_time()) / 1000000.0);
    general_pcm_wait(&window, "General future A", 200, 80);
    RadioSPlayerWeeklySnapshot current_schedule = weekly_snapshot(driver);
    g_assert_cmpstr(current_schedule.next_event_name, ==, "General adjacent saved weekly");
    g_assert_cmpint(current_schedule.next_start_utc_us, ==, start + minute);
    RadioSAudioRunSnapshot first_run; radios_audio_engine_runner_snapshot(engine, &first_run);
    goffset pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 950), >, .07);
    gint64 refill_deadline = g_get_monotonic_time() + 8 * G_USEC_PER_SEC; RadioSAudioRunSnapshot refilled = {0};
    do { pump(); radios_audio_engine_runner_snapshot(engine, &refilled); }
    while (refilled.epoch == first_run.epoch && g_get_monotonic_time() < refill_deadline);
    g_assert_cmpuint(refilled.epoch, >, first_run.epoch); g_assert_true(weekly_snapshot(driver).armed);
    general_pcm_wait(&window, "General future A", 200, 10);
    gchar *fail = fixture_flag(".acceptance-playout-prepare-unavailable"); g_assert_true(g_file_set_contents(fail, "fail", -1, NULL));
    wait_until(general_retry_visible, driver, "temporary HTTP failure retains armed general automation");
    g_assert_cmpint(g_unlink(fail), ==, 0); g_free(fail);
    general_pcm_wait(&window, "General future A", 200, 15);
    radios_audio_engine_runner_snapshot(engine, &refilled); pcm = capture_offset("MAIN");
    radios_data_connect(window.ctx); run_for(250);
    RadioSPlayerWeeklySnapshot reconnecting = weekly_snapshot(driver); g_assert_true(reconnecting.armed);
    RadioSAudioRunSnapshot retained; radios_audio_engine_runner_snapshot(engine, &retained);
    g_assert_cmpuint(retained.epoch, ==, refilled.epoch); g_assert_cmpfloat(tone_energy("MAIN", pcm, 950), >, .07);
    wait_until(window_ready, &window, "same-identity reconnect authenticates while the accepted MAIN queue continues");
    general_pcm_wait(&window, "General future A", 200, 10); g_assert_true(weekly_snapshot(driver).armed);
    gchar *hold = fixture_flag(".acceptance-playout-prepare-hold"), *held = fixture_flag(".acceptance-playout-prepare-ready");
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL));
    wait_until(file_exists, held, "next finite general batch is genuinely waiting for HTTP preparation");
    click(window.root, "header-pause"); g_assert_true(weekly_snapshot(driver).paused);
    g_assert_cmpint(g_unlink(hold), ==, 0); g_assert_cmpint(g_unlink(held), ==, 0); g_free(hold); g_free(held);
    run_for(700); g_assert_true(weekly_snapshot(driver).armed); g_assert_true(weekly_snapshot(driver).paused);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    radios_audio_engine_runner_snapshot(engine, &retained); g_assert_cmpint(retained.state, ==, RADIOS_AUDIO_RUN_IDLE);
    click(window.root, "header-play"); general_pcm_wait(&window, "General future A", 200, 10);
    general_pcm_wait(&window, "General future B", 200, 80);
    RadioSPlayerWeeklySnapshot following = weekly_snapshot(driver);
    g_assert_true(following.armed); g_assert_cmpstr(following.current_event_name, ==, "General adjacent saved weekly");
    g_assert_cmpint(following.start_utc_us, ==, start + minute); g_assert_false(following.repeat);
    pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 2350), >, .07);
    click(window.root, "header-stop"); run_for(3000); g_assert_false(weekly_snapshot(driver).armed);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    radios_audio_engine_runner_snapshot(engine, &retained); g_assert_cmpint(retained.state, ==, RADIOS_AUDIO_RUN_IDLE);
    weekly_record_unchanged(saved_weekly); calendar_schedule_unchanged(saved_main);
    radios_audio_engine_unref(engine); window_close(&window);
    json_node_free(a); json_node_free(b); json_node_free(pa); json_node_free(pb);
    json_node_free(first); json_node_free(past); json_node_free(second); json_node_free(saved_main); json_node_free(saved_weekly);
}

static void test_general_unavailable_and_revision_fences(void)
{
    Window window = window_open(); navigate(&window, "broadcast"); RadioSPlayerWeekly *driver = general_driver(&window);
    JsonNode *tone = routing_short_markers(media_import(&window, "General availability restored tone", 1750, .22, "music"), 1800);
    JsonNode *empty = playlist_create("General empty unavailable source", NULL, NULL), *playlist = playlist_create("General available source", tone, NULL);
    GDateTime *now = g_date_time_new_now_utc(), *tomorrow = g_date_time_add_days(now, 1);
    GDateTime *morning = g_date_time_new_utc(g_date_time_get_year(tomorrow), g_date_time_get_month(tomorrow), g_date_time_get_day_of_month(tomorrow), 6, 0, 0);
    gint64 start = g_date_time_to_unix(morning) * G_USEC_PER_SEC;
    g_date_time_unref(now); g_date_time_unref(tomorrow); g_date_time_unref(morning);
    JsonNode *bad = weekly_pcm_configuration(empty, NULL, start, 60, "General unavailable repeating slot");
    json_object_set_boolean_member(json_node_get_object(bad), "repeat", TRUE);
    JsonNode *saved_bad = document_save("weekly_programme", "General unavailable recurring source", json_node_get_object(bad));
    JsonNode *good = weekly_pcm_configuration(playlist, NULL, start + 2 * 60 * G_USEC_PER_SEC, 1, "General valid overlapping slot");
    JsonObject *calendar = json_object_get_object_member(json_node_get_object(good), "calendar");
    JsonNode *saved_good = document_save("automation_calendar", "main", calendar);
    radios_player_weekly_general_refresh(driver);
    GeneralReady ready = {&window, driver, "General valid overlapping slot", start + 2 * 60 * G_USEC_PER_SEC};
    wait_until(general_future_ready, &ready, "unavailable repeating programme does not skip the overlapping valid main slot");
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001); click(window.root, "header-play");
    gint64 until = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
    do { pump(); } while ((weekly_snapshot(driver).state != RADIOS_PLAYER_WEEKLY_WAITING || weekly_snapshot(driver).next_start_utc_us != ready.start) && g_get_monotonic_time() < until);
    g_assert_true(weekly_snapshot(driver).armed); g_assert_cmpint(weekly_snapshot(driver).next_start_utc_us, ==, ready.start);
    /* Re-read the same source while its media response is held, then move the
     * future slot earlier. The old preview geometry must not become a floor. */
    gchar *hold = fixture_flag(".acceptance-playout-prepare-hold"), *held = fixture_flag(".acceptance-playout-prepare-ready");
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL));
    json_object_set_string_member(json_array_get_object_element(json_object_get_array_member(calendar, "events"), 0), "title", "General revision pending");
    JsonNode *revision = document_save("automation_calendar", "main", calendar); json_node_free(revision);
    radios_player_weekly_general_refresh(driver); wait_until(file_exists, held, "future prepare holds a genuine source revision");
    JsonObject *event = json_array_get_object_element(json_object_get_array_member(calendar, "events"), 0);
    json_object_set_string_member(event, "title", "General revision earlier fresh slot");
    json_object_set_int_member(event, "start_minute", 361); json_object_set_int_member(event, "end_minute", 362);
    revision = document_save("automation_calendar", "main", calendar);
    g_assert_cmpint(g_unlink(hold), ==, 0); g_assert_cmpint(g_unlink(held), ==, 0); g_free(hold); g_free(held);
    until = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
    do { pump(); } while ((!g_str_equal(weekly_snapshot(driver).next_event_name, "General revision earlier fresh slot") ||
        weekly_snapshot(driver).state != RADIOS_PLAYER_WEEKLY_WAITING) && g_get_monotonic_time() < until);
    RadioSPlayerWeeklySnapshot fresh = weekly_snapshot(driver);
    g_assert_true(fresh.armed); g_assert_cmpstr(fresh.next_event_name, ==, "General revision earlier fresh slot");
    g_assert_cmpint(fresh.next_start_utc_us, ==, start + 60 * G_USEC_PER_SEC);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    click(window.root, "header-stop"); g_assert_false(weekly_snapshot(driver).armed);
    weekly_record_unchanged(saved_bad); calendar_schedule_unchanged(revision);
    window_close(&window); json_node_free(tone); json_node_free(empty); json_node_free(playlist); json_node_free(bad);
    json_node_free(good); json_node_free(saved_bad); json_node_free(saved_good); json_node_free(revision);
}

static void test_weekly_programme_stop_pending_prepare_preserves_foreign_pcm(void)
{
    Window window = window_open(); seed_programme(&window, NULL, NULL); navigate(&window, "broadcast"); players_show(&window);
    JsonNode *scheduled = routing_short_markers(media_import(&window, "Cancelled actual weekly tone", 2350, .22, "music"), 1800);
    JsonNode *foreign = media_import(&window, "Preserved independent player tone", 1150, .22, "music");
    JsonNode *playlist = playlist_create("Cancel actual weekly source", scheduled, NULL);
    gint64 start = g_get_real_time() / G_TIME_SPAN_DAY * G_TIME_SPAN_DAY;
    JsonNode *configuration = weekly_pcm_configuration(playlist, NULL, start, 1440, "Active weekly cancellation");
    JsonNode *saved = document_save("weekly_programme", "Pending weekly programme cancellation", json_node_get_object(configuration));
    combo(window.root, "deck-0-autodj-target", "-1"); RadioSPlayerWeekly *weekly = weekly_native_load(&window, 0, "Pending weekly programme cancellation");
    GError *error = NULL; require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(foreign), 1, FALSE, &error), &error);
    deck_wait(&window, 1, RADIOS_AUDIO_UI_READY, 0);
    gchar *hold = fixture_flag(".acceptance-playout-prepare-hold"), *ready = fixture_flag(".acceptance-playout-prepare-ready");
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL)); click(window.root, "deck-0-play");
    wait_until(file_exists, ready, "real phase2 preparation held after authenticated weekly source selection");
    /* A workspace player runs its loaded weekly programme through its own
     * player scope (the card assigns the record to that scope on load). */
    RadioSPlayerScopeExecutionSnapshot pending = rotation_execution_snapshot(&window);
    g_assert_true(pending.schema_active); g_assert_cmpint(pending.state, ==, RADIOS_SCOPE_EXECUTION_PREPARING);
    click(window.root, "deck-0-stop");
    RadioSPlayerScopeExecutionSnapshot stopped = rotation_execution_snapshot(&window);
    g_assert_false(stopped.schema_active); g_assert_cmpint(stopped.state, ==, RADIOS_SCOPE_EXECUTION_IDLE); g_assert_false(weekly_snapshot(weekly).armed);
    click(window.root, "deck-1-play"); RadioSAudioUiSnapshot live = deck_wait(&window, 1, RADIOS_AUDIO_UI_PLAYING, 100);
    g_assert_cmpint(g_unlink(hold), ==, 0); g_assert_cmpint(g_unlink(ready), ==, 0); run_for(900);
    stopped = rotation_execution_snapshot(&window);
    g_assert_false(stopped.schema_active); g_assert_cmpint(stopped.state, ==, RADIOS_SCOPE_EXECUTION_IDLE); g_assert_false(weekly_snapshot(weekly).armed);
    RadioSAudioUiSnapshot after; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 1, &after));
    g_assert_cmpuint(after.generation, ==, live.generation); g_assert_cmpstr(after.media_id, ==, live.media_id); g_assert_cmpint(after.state, ==, RADIOS_AUDIO_UI_PLAYING);
    RadioSPlayoutSnapshot run = {0}; radios_playout_ui_get_snapshot(window.ctx, &run); g_assert_cmpint(run.state, ==, RADIOS_PLAYOUT_IDLE);
    goffset pcm = capture_offset("MAIN"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 1150), >, .07); g_assert_cmpfloat(tone_energy("MAIN", pcm, 2350), <, .01);
    weekly_record_unchanged(saved); assert_saved_playlist_unchanged(playlist);
    click(window.root, "deck-1-stop"); g_free(hold); g_free(ready);
    json_node_free(scheduled); json_node_free(foreign); json_node_free(playlist); json_node_free(configuration); json_node_free(saved); window_close(&window);
}

static JsonNode *weekly_mixer_wait_saved(guint channel, gint deck)
{
    JsonNode *record = NULL; gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (g_get_monotonic_time() < deadline) {
        pump(); if (record) json_node_free(record); record = document_read("dj_mixer", "DJ mixer");
        JsonObject *config = record ? json_object_get_object_member(json_node_get_object(record), "config") : NULL;
        JsonArray *channels = config ? json_object_get_array_member(config, "channels") : NULL;
        if (channels && radios_json_int(json_array_get_object_element(channels, channel), "source_deck", -99) == deck) break;
    }
    g_assert_nonnull(record); JsonArray *channels = json_object_get_array_member(json_object_get_object_member(json_node_get_object(record), "config"), "channels");
    g_assert_cmpint(radios_json_int(json_array_get_object_element(channels, channel), "source_deck", -99), ==, deck); return record;
}
static void test_player_output_unconnected_constructor_custom_channel_reopen_pcm(void)
{
    Window window = window_open_raw(); seed_programme(&window, NULL, NULL); navigate(&window, "broadcast"); players_show(&window);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine); RadioSAudioSnapshot before = {0};
    radios_audio_engine_snapshot(engine, &before); RadioSAudioMixerConfig mixer = before.mixer;
    for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel)
        if (mixer.channels[channel].source_deck == 6) mixer.channels[channel].source_deck = -1;
    /* The custom strip's own -12 dB must reach MAIN: with OTO SES (loudness
     * normalization, the default) the engine takes the fader over towards
     * the LUFS target by design, so this channel opts out of it. */
    mixer.channels[8].gain_db = -12; mixer.channels[8].loudness_bypass = TRUE;
    GError *error = NULL; require_call(radios_audio_ui_set_mixer(window.ctx, &mixer, &error), &error);
    require_call(radios_audio_ui_set_deck_main(window.ctx, 6, FALSE, &error), &error);
    JsonNode *unconnected = weekly_mixer_wait_saved(6, -1); json_node_free(unconnected);
    radios_audio_engine_snapshot(engine, &before); add_independent_player(window.root);
    DeckCard added = {GTK_WIDGET(window.ctx->parent), 6}; wait_until(deck_card_present, &added, "the player tree adds the seventh player's card"); run_for(200);
    GtkWidget *card = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-6"); g_assert_nonnull(card);
    g_assert_nonnull(find_label(card, "Oynatıcı 7")); GtkWidget *output = find(card, "deck-6-output-channel"); g_assert_true(GTK_IS_COMBO_BOX(output));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(output)), ==, "-1");
    RadioSAudioSnapshot after = {0}; radios_audio_engine_snapshot(engine, &after); g_assert_false(after.decks[6].main_routed);
    for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel)
        g_assert_cmpint(after.mixer.channels[channel].source_deck, ==, before.mixer.channels[channel].source_deck);
    JsonNode *tone = media_import(&window, "Custom channel actual1500Hz", 1500, .22, "music"), *playlist = playlist_create("Custom output saved playlist", tone, NULL);
    gint64 tomorrow = (g_get_real_time() / G_TIME_SPAN_DAY + 1) * G_TIME_SPAN_DAY;
    JsonNode *configuration = weekly_pcm_configuration(playlist, NULL, tomorrow, 1, "Unconnected loaded programme");
    JsonNode *programme = document_save("weekly_programme", "Silent unconnected weekly programme", json_node_get_object(configuration));
    RadioSPlayerWeekly *weekly = weekly_native_load(&window, 6, "Silent unconnected weekly programme");
    g_assert_false(weekly_snapshot(weekly).armed); radios_audio_engine_snapshot(engine, &after);
    g_assert_false(after.decks[6].main_routed); g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(output)), ==, "-1");
    g_assert_false(gtk_widget_is_sensitive(find(card, "deck-6-play"))); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    combo(card, "deck-6-output-channel", "8"); JsonNode *custom = weekly_mixer_wait_saved(8, 6);
    radios_audio_engine_snapshot(engine, &after); g_assert_true(after.decks[6].main_routed);
    for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel)
        if (channel != 8) g_assert_cmpint(after.mixer.channels[channel].source_deck, !=, 6);
    weekly_record_unchanged(programme); radios_audio_engine_unref(engine); window_close(&window);
    /* A raw reopen and added card must read the acknowledged custom strip,
     * without fixture routing gestures that could manufacture persistence. */
    /* The saved player workspace restores the seventh player on reopen. */
    window = window_open_raw(); navigate(&window, "broadcast"); players_show(&window);
    DeckCard restored = {GTK_WIDGET(window.ctx->parent), 6}; wait_until(deck_card_present, &restored, "the saved workspace restores the seventh player's card"); run_for(200);
    card = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-6"); output = find(card, "deck-6-output-channel");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(output)), ==, "8");
    JsonNode *reopened = document_read("dj_mixer", "DJ mixer"); g_assert_true(json_node_equal(custom, reopened)); json_node_free(reopened);
    saved_playlist_card_drop(&window, card, radios_json_string(json_node_get_object(playlist), "_id", ""), 1);
    click(card, "deck-6-play"); deck_wait(&window, 6, RADIOS_AUDIO_UI_PLAYING, 100); goffset pcm = capture_offset("MAIN"); run_for(250);
    gdouble energy = tone_energy("MAIN", pcm, 1500); g_assert_cmpfloat(energy, >, .025); g_assert_cmpfloat(energy, <, .085);
    engine = radios_audio_ui_engine_acquire(window.ctx); radios_audio_engine_snapshot(engine, &after);
    g_assert_cmpint(after.mixer.channels[8].source_deck, ==, 6); g_assert_cmpfloat(after.dsp_meters.channel_peak[8][0], >, .025);
    g_assert_cmpfloat(after.dsp_meters.channel_peak[6][0], <, .001); click(card, "deck-6-stop");
    JsonNode *foreign = media_import(&window, "Foreign live strip preserved1150Hz", 1150, .22, "music");
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(foreign), 1, FALSE, &error), &error);
    deck_wait(&window, 1, RADIOS_AUDIO_UI_READY, 0); click(window.root, "deck-1-play"); RadioSAudioUiSnapshot live = deck_wait(&window, 1, RADIOS_AUDIO_UI_PLAYING, 100);
    radios_audio_engine_snapshot(engine, &before); combo(card, "deck-6-output-channel", "1"); run_for(300); radios_audio_engine_snapshot(engine, &after);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(output)), ==, "8");
    for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel)
        g_assert_cmpint(after.mixer.channels[channel].source_deck, ==, before.mixer.channels[channel].source_deck);
    RadioSAudioUiSnapshot untouched; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 1, &untouched));
    g_assert_cmpuint(untouched.generation, ==, live.generation); g_assert_cmpstr(untouched.media_id, ==, live.media_id); g_assert_cmpint(untouched.state, ==, RADIOS_AUDIO_UI_PLAYING);
    pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1150), >, .07);
    g_assert_cmpfloat(measure("CUE").peak[0], <, .0001); weekly_record_unchanged(programme); assert_saved_playlist_unchanged(playlist);
    rhythm_shot(&window, "custom-output-channel.png"); click(window.root, "deck-1-stop"); radios_audio_engine_unref(engine);
    json_node_free(custom); json_node_free(tone); json_node_free(foreign); json_node_free(playlist); json_node_free(configuration); json_node_free(programme); window_close(&window);
}

static void autodj_gesture_event(GtkWidget *widget, GdkEventType type, guint key)
{
    GdkEvent *event = gdk_event_new(type); event->any.window = g_object_ref(gtk_widget_get_window(widget)); event->any.send_event = TRUE;
    gdk_event_set_device(event, gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(widget))));
    if (type == GDK_KEY_PRESS) event->key.keyval = key;
    else { event->button.button = 1; event->button.x = 20; event->button.y = 8; }
    gtk_widget_event(widget, event); gdk_event_free(event);
}
static void test_autodj_slider_and_header_seek_preserve_pcm(void)
{
    Window window = window_open(); navigate(&window, "broadcast"); players_show(&window);
    JsonNode *first = media_import(&window, "Seek current AutoDJ", 700, .2, "music"), *second = media_import(&window, "Seek queued AutoDJ", 1700, .2, "music");
    JsonNode *programme = programme_config(first, second); GError *error = NULL;
    require_call(radios_playout_ui_start_programme(window.ctx, window.root, json_node_get_object(programme), 0, 1, .5, &error), &error);
    RadioSPlayoutSnapshot run = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 200); guint deck = (guint)run.deck, next = (guint)run.next_deck;
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); RadioSAudioSnapshot before, after;
    radios_audio_engine_snapshot(engine, &before); gchar *name = g_strdup_printf("deck-%u-seek", deck); GtkWidget *scale = find(window.root, name); g_free(name);
    g_assert_true(GTK_IS_RANGE(scale)); g_assert_true(gtk_widget_get_mapped(scale)); g_assert_true(gtk_widget_is_sensitive(scale));
    autodj_gesture_event(scale, GDK_BUTTON_PRESS, 0); gboolean handled = FALSE;
    for (guint step = 0; step < 4; ++step) g_signal_emit_by_name(scale, "change-value", GTK_SCROLL_JUMP, (gdouble)(600 + step * 200), &handled);
    radios_audio_engine_snapshot(engine, &after); g_assert_cmpuint(after.decks[deck].generation, ==, before.decks[deck].generation);
    autodj_gesture_event(scale, GDK_BUTTON_RELEASE, 0); radios_audio_engine_snapshot(engine, &after);
    g_assert_cmpuint(after.decks[deck].generation, ==, before.decks[deck].generation + 1);
    g_assert_cmpuint(after.decks[next].generation, ==, before.decks[next].generation);
    run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 1200); run_for(100);
    GtkWidget *header = find(window.root, "header-song-progress"); g_assert_true(GTK_IS_WIDGET(header)); gboolean seekable = FALSE;
    radios_neon_progress_get_state(header, NULL, NULL, NULL, &seekable); g_assert_true(seekable);
    radios_audio_engine_snapshot(engine, &before); autodj_gesture_event(header, GDK_KEY_PRESS, GDK_KEY_Home);
    radios_audio_engine_snapshot(engine, &after); g_assert_cmpuint(after.decks[deck].generation, ==, before.decks[deck].generation + 1);
    g_assert_cmpuint(after.decks[next].generation, ==, before.decks[next].generation);
    run = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 100); g_assert_cmpstr(run.title, ==, "Seek current AutoDJ");
    goffset pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .08);
    run = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 200); g_assert_cmpstr(run.title, ==, "Seek queued AutoDJ");
    pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .08);
    stop_run(&window); radios_audio_engine_unref(engine); json_node_free(programme); json_node_free(first); json_node_free(second); window_close(&window);
}

static void test_continuous_autodj_transient_retry(void)
{
    Window window = window_open(); JsonNode *a = media_import(&window, "Retry rotation A", 700, .2, "music");
    JsonNode *b = media_import(&window, "Retry rotation B", 1700, .2, "music");
    JsonNode *pa = playlist_create("Retry A source", a, NULL), *pb = playlist_create("Retry B source", b, NULL);
    JsonNode *config_node = automation_config(pa, pb, pa); JsonObject *settings = json_node_get_object(config_node);
    json_object_set_boolean_member(settings, "jingleEnabled", FALSE); json_object_set_int_member(settings, "sourceMode", 0);
    json_object_set_int_member(settings, "trackMode", 1); json_object_set_int_member(settings, "previewCount", 1);
    gchar *once = fixture_flag(".acceptance-playout-prepare-fail-once"), *failed = fixture_flag(".acceptance-playout-prepare-failed-once");
    g_assert_true(g_file_set_contents(once, "2", -1, NULL)); GError *error = NULL;
    require_call(radios_playout_ui_start_automation(window.ctx, window.root, settings, &error), &error);
    run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 100); wait_until(file_exists, failed, "one genuine 503 continuation response");
    RadioSPlayoutSnapshot run = {0}; g_assert_true(radios_playout_ui_get_snapshot(window.ctx, &run)); g_assert_true(run.automation);
    run = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 100); g_assert_true(run.automation); g_assert_cmpstr(run.title, ==, "Retry rotation B");
    goffset pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .08);
    stop_run(&window); g_assert_cmpint(g_unlink(failed), ==, 0); g_assert_false(g_file_test(once, G_FILE_TEST_EXISTS));
    g_free(once); g_free(failed); json_node_free(config_node); json_node_free(pa); json_node_free(pb); json_node_free(a); json_node_free(b); window_close(&window);
}

static JsonNode *autodj_marker_detail(JsonNode *detail, guint in_ms, guint out_ms)
{
    JsonObject *media = json_object_get_object_member(json_node_get_object(detail), "media");
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body), *markers = json_object_new();
    json_object_set_string_member(object, "media_id", radios_json_string(media, "_id", ""));
    json_object_set_int_member(object, "expected_revision", radios_json_int(media, "_rev", 0)); json_object_set_null_member(object, "expected_marker_revision");
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    const gchar *names[] = {"cue_in_ms", "intro_ms", "out_ms", "fade_start_ms", "fade_end_ms", "auto_next_ms", "overlap_ms"};
    const gint64 values[] = {0, 0, 6001, 6001, 6001, 6001, 0};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) json_object_set_int_member(markers, names[i], values[i]);
    json_object_set_boolean_member(markers, "automatic", FALSE);
    json_object_set_int_member(markers, "autodj_in_ms", in_ms); json_object_set_int_member(markers, "autodj_out_ms", out_ms);
    json_object_set_object_member(object, "markers", markers); JsonNode *result = request("/v1/workflows/desktop-media/markers-save", body); json_node_free(body); return result;
}

static void test_autodj_short_span_three_media_MAIN_PCM(void)
{
    Window window = window_open(); navigate(&window, "broadcast"); players_show(&window);
    JsonNode *a = media_import(&window, "Short AutoDJ incoming A", 700, .2, "music");
    JsonNode *b = media_import(&window, "Short AutoDJ incoming B", 1700, .2, "music");
    JsonNode *c = media_import(&window, "Short AutoDJ incoming C", 2700, .2, "music");
    JsonNode *marked = autodj_marker_detail(a, 1000, 2500); json_node_free(a); a = marked;
    marked = autodj_marker_detail(b, 2000, 2500); json_node_free(b); b = marked;
    JsonNode *programme = programme_config(a, b), *third = programme_config(c, NULL);
    JsonArray *events = json_object_get_array_member(json_node_get_object(programme), "events");
    JsonArray *third_events = json_object_get_array_member(json_node_get_object(third), "events");
    json_array_add_element(events, json_node_copy(json_array_get_element(third_events, 0))); json_node_free(third);
    JsonNode *saved_programme = json_node_copy(programme), *saved_a = json_node_copy(a), *saved_b = json_node_copy(b);
    GError *error = NULL;
    require_call(radios_playout_ui_start_programme(window.ctx, window.root, json_node_get_object(programme), 0, 1, 2, &error), &error);
    RadioSPlayoutSnapshot run = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 1100);
    g_assert_cmpuint(run.cue_in_ms, ==, 1000); g_assert_cmpuint(run.duration_ms, >=, 6000);
    goffset pcm = capture_offset("MAIN"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .08); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), <, .01);
    run = run_wait(&window, RADIOS_PLAYOUT_OVERLAP, 0, 2500);
    pcm = capture_offset("MAIN"); run_for(200);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .015); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .015);
    run = run_wait(&window, RADIOS_PLAYOUT_OVERLAP, 1, 2500);
    /* Its 500 ms entry span fits the previous fade. A two-second fade used
     * to promote this item only after its cursor had crossed 4000 ms. */
    g_assert_cmpuint(run.position_ms, <, 3000); g_assert_cmpuint(run.cue_in_ms, ==, 2000);
    g_assert_cmpuint(run.duration_ms, >=, 6000); g_assert_cmpstr(run.title, ==, "Short AutoDJ incoming B");
    run = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 2, 2100);
    g_assert_cmpstr(run.title, ==, "Short AutoDJ incoming C"); pcm = capture_offset("MAIN"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 2700), >, .08);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), <, .01); g_assert_cmpfloat(measure("CUE").peak[0], <, .0001);
    stop_run(&window); g_assert_true(json_node_equal(saved_programme, programme));
    JsonNode *current_a = media_read(a);
    JsonNode *current_b = media_read(b);
    g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(saved_a), "markers"), json_object_get_member(json_node_get_object(current_a), "markers")));
    g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(saved_b), "markers"), json_object_get_member(json_node_get_object(current_b), "markers")));
    json_node_free(current_a); json_node_free(current_b); json_node_free(saved_programme); json_node_free(saved_a); json_node_free(saved_b);
    json_node_free(programme); json_node_free(a); json_node_free(b); json_node_free(c); window_close(&window);
}

static void test_continuous_short_autodj_append_PCM(void)
{
    Window window = window_open(); navigate(&window, "broadcast");
    combo_index(window.root, "programme-transition-mode", 1); spin(window.root, "programme-transition-overlap", 3);
    JsonNode *a = media_import(&window, "Continuous long A", 700, .2, "music");
    JsonNode *b = media_import(&window, "Continuous long B", 1700, .2, "music");
    JsonNode *c = media_import(&window, "Continuous short IN OUT C", 2500, .2, "music");
    JsonNode *marked = autodj_marker_detail(c, 2000, 3000); json_node_free(c); c = marked;
    JsonNode *pa = playlist_create("Continuous long source A", a, NULL), *pb = playlist_create("Continuous long source B", b, NULL);
    JsonNode *pc = playlist_create("Continuous short source C", c, NULL);
    JsonNode *cfg = automation_config(pa, pb, pc); JsonObject *settings = json_node_get_object(cfg);
    JsonArray *sources = json_object_get_array_member(settings, "sources");
    JsonObject *third = radios_programme_clone_object(json_array_get_object_element(sources, 1));
    gchar *key = g_uuid_string_random(); json_object_set_string_member(third, "source_id", key); g_free(key);
    json_object_set_string_member(third, "playlist_id", radios_json_string(json_node_get_object(pc), "_id", ""));
    json_array_add_object_element(sources, third);
    json_object_set_boolean_member(settings, "jingleEnabled", FALSE); json_object_set_int_member(settings, "sourceMode", 0);
    json_object_set_int_member(settings, "trackMode", 1); json_object_set_int_member(settings, "previewCount", 1);
    json_object_set_int_member(settings, "transition", 1); json_object_set_int_member(settings, "boundaryTransition", 2);
    json_object_set_int_member(settings, "overlap", 3); json_object_set_int_member(settings, "boundaryOverlap", 3);
    GError *error = NULL; require_call(radios_playout_ui_start_automation(window.ctx, window.root, settings, &error), &error);
    RadioSPlayoutSnapshot run = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 200);
    g_assert_cmpstr(run.title, ==, "Continuous long A");
    /* The first batch contains A and B. C is only discovered by continuation,
     * which must rebuild its start from B's three-second to one-second fade. */
    run = run_wait(&window, RADIOS_PLAYOUT_OVERLAP, 1, 5200);
    g_assert_cmpstr(run.title, ==, "Continuous long B"); g_assert_cmpuint(run.total, >=, 4);
    goffset pcm = capture_offset("MAIN"); run_for(150);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .02);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 2500), >, .02);
    /* C's one-second entry interval includes the incoming fade. Its own OUT
     * starts the next A immediately and keeps the remaining physical tail. */
    run = run_wait(&window, RADIOS_PLAYOUT_OVERLAP, 2, 3100);
    g_assert_cmpstr(run.title, ==, "Continuous short IN OUT C"); g_assert_cmpuint(run.position_ms, <, 3800);
    pcm = capture_offset("MAIN"); run_for(150);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .02);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 2500), >, .02);
    stop_run(&window); json_node_free(cfg); json_node_free(pa); json_node_free(pb); json_node_free(pc);
    json_node_free(a); json_node_free(b); json_node_free(c); window_close(&window);
}

static void ordinary_autodj_playlist_load(Window *window, const gchar *name)
{
    click(window->root, "deck-0-playlist"); SavedLoaderList loader = {name, NULL, NULL, {0}};
    wait_until(saved_loader_list_ready, &loader, "saved ordinary local playlist is available");
    gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(loader.tree)), &loader.iter);
    GtkWidget *accept = gtk_dialog_get_widget_for_response(GTK_DIALOG(loader.dialog), GTK_RESPONSE_ACCEPT);
    wait_until(widget_enabled, accept, "ordinary playlist is verified before loading"); gtk_button_clicked(GTK_BUTTON(accept)); pump();
    run_for(200); /* Loading assigns inputs; the real Play action loads the selected media. */
}
static void test_ordinary_AutoDJ_pending_target_fence(void)
{
    Window window = window_open(); navigate(&window, "broadcast"); players_show(&window);
    while (!find(window.root, "deck-2-title")) { click_label(window.root, "+ Oynatıcı ekle"); run_for(100); }
    JsonNode *a = media_import(&window, "Pending ordinary AutoDJ A", 700, .2, "music");
    JsonNode *b = media_import(&window, "Pending ordinary AutoDJ B", 1700, .2, "music");
    JsonNode *c = media_import(&window, "Pending ordinary AutoDJ C", 2700, .2, "music");
    JsonNode *playlist = playlist_create("Pending ordinary AutoDJ target", a, b); ordinary_autodj_playlist_load(&window, "Pending ordinary AutoDJ target");
    GError *error = NULL;
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(b), 1, FALSE, &error), &error);
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(c), 2, FALSE, &error), &error);
    deck_wait(&window, 1, RADIOS_AUDIO_UI_READY, 0); deck_wait(&window, 2, RADIOS_AUDIO_UI_READY, 0);
    combo(window.root, "deck-0-autodj-target", "1"); click(window.root, "deck-0-play"); deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); RadioSAudioSnapshot before, after; radios_audio_engine_snapshot(engine, &before);
    gchar *hold = fixture_flag(".acceptance-playout-prepare-hold"), *ready = fixture_flag(".acceptance-playout-prepare-ready");
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL)); click(window.root, "deck-0-autodj-start");
    wait_until(file_exists, ready, "ordinary AutoDJ asynchronous prepare is genuinely held");
    /* The pending card start keeps its captured pair: a new target is refused
     * until Durdur, so the held prepare cannot start on a stale target. */
    combo(window.root, "deck-0-autodj-target", "2"); run_for(100);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(window.root, "deck-0-autodj-target"))), ==, "1");
    g_assert_cmpint(radios_player_workspace_topology(window.ctx->players)->players[0].autodj_target, ==, 1);
    g_assert_cmpint(g_unlink(hold), ==, 0);
    RadioSPlayoutSnapshot run = rotation_run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 0);
    g_assert_cmpint(run.deck, ==, 0); g_assert_cmpint(run.next_deck, ==, 1);
    g_assert_true(scoped_owns_deck(&window, 0)); g_assert_true(scoped_owns_deck(&window, 1)); g_assert_false(scoped_owns_deck(&window, 2));
    g_assert_false(radios_playout_ui_owns_deck(window.ctx, 0)); g_assert_false(radios_playout_ui_owns_deck(window.ctx, 2));
    radios_audio_engine_snapshot(engine, &after);
    /* The playing source is joined without a reload; the refused target is untouched. */
    for (guint deck = 0; deck < 3; deck += 2) {
        g_assert_cmpuint(after.decks[deck].generation, ==, before.decks[deck].generation);
        g_assert_cmpstr(after.decks[deck].media_id, ==, before.decks[deck].media_id);
    }
    g_assert_cmpint(after.decks[0].state, ==, RADIOS_AUDIO_PLAYING);
    goffset pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .08);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 2700), <, .01);
    rotation_stop_run(&window); g_assert_cmpint(g_unlink(ready), ==, 0);
    g_free(hold); g_free(ready); radios_audio_engine_unref(engine); json_node_free(playlist);
    json_node_free(a); json_node_free(b); json_node_free(c); window_close(&window);
}
static void test_ordinary_AutoDJ_live_short_tail_PCM(void)
{
    Window window = window_open(); navigate(&window, "broadcast"); players_show(&window);
    JsonNode *a = media_import(&window, "Live short-tail AutoDJ A", 700, .2, "music");
    JsonNode *b = media_import(&window, "Live short-tail AutoDJ B", 1700, .2, "music");
    JsonNode *marked = autodj_marker_detail(a, 1000, 3500); json_node_free(a); a = marked;
    marked = autodj_marker_detail(b, 2000, 5500); json_node_free(b); b = marked;
    JsonNode *playlist = playlist_create("Live ordinary AutoDJ short tail", a, b); ordinary_autodj_playlist_load(&window, "Live ordinary AutoDJ short tail");
    /* The operator keeps the displayed station rule (Crossfade · 3 sn); with no
     * saved rule that displayed default must be the effective one. */
    combo_index(window.root, "programme-transition-mode", 1); spin(window.root, "programme-transition-overlap", 3);
    combo(window.root, "deck-0-autodj-target", "1"); combo(window.root, "deck-1-autodj-target", "0");
    click(window.root, "deck-0-play"); deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100); GError *error = NULL;
    require_call(radios_audio_ui_seek(window.ctx, 0, 4000, &error), &error); deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 4000);
    click(window.root, "deck-0-autodj-start"); RadioSPlayoutSnapshot run = rotation_run_wait(&window, RADIOS_PLAYOUT_OVERLAP, 0, 4100);
    g_assert_cmpuint(run.cue_in_ms, >=, 4000); g_assert_cmpuint(run.duration_ms, >=, 6000);
    g_assert_cmpuint(run.position_ms, <, 4600); /* The stored exit was already passed; join safely with the bounded remaining fade. */
    goffset pcm = capture_offset("MAIN"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .04); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .015);
    run = rotation_run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 4050); g_assert_cmpuint(run.cue_in_ms, ==, 2000);
    pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .08);
    rotation_stop_run(&window); json_node_free(playlist); json_node_free(a); json_node_free(b); window_close(&window);
}

static void test_stale_solo_playlist_AutoDJ_pair_transport_PCM(void)
{
    Window window = window_open(); navigate(&window, "broadcast"); players_show(&window);
    JsonNode *a = media_import(&window, "Normal playlist AutoDJ A", 700, .2, "music"), *b = media_import(&window, "Normal playlist AutoDJ B", 1700, .2, "music");
    JsonNode *marked = autodj_marker_detail(a, 2000, 4000); json_node_free(a); a = marked;
    JsonNode *playlist = playlist_create("Normal playlist AutoDJ pair", a, b);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); RadioSAudioSnapshot audio; radios_audio_engine_snapshot(engine, &audio);
    RadioSAudioMixerConfig mixer = audio.mixer; mixer.channels[3].mute = TRUE; mixer.channels[3].solo = TRUE; mixer.channels[3].gain_db = -60;
    GError *error = NULL; require_call(radios_audio_ui_set_mixer(window.ctx, &mixer, &error), &error);
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(a), 0, FALSE, &error), &error);
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(b), 1, FALSE, &error), &error);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0); deck_wait(&window, 1, RADIOS_AUDIO_UI_READY, 0);
    click(window.root, "deck-0-play"); click(window.root, "deck-1-play"); deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
    deck_wait(&window, 1, RADIOS_AUDIO_UI_PLAYING, 100); goffset pcm = capture_offset("MAIN"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .08); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .08);
    radios_audio_engine_snapshot(engine, &audio); g_assert_true(audio.mixer.channels[3].solo); g_assert_true(audio.mixer.channels[3].mute);
    g_assert_true(audio.decks[0].main_path_ready); g_assert_true(audio.decks[1].main_path_ready);
    /* CUE selection must not steal either player's MAIN transport clock. */
    RadioSAudioPreferences preferences; radios_audio_preferences_default(&preferences);
    g_strlcpy(preferences.main_sink, main_sink, sizeof preferences.main_sink); g_strlcpy(preferences.cue_sink, cue_sink, sizeof preferences.cue_sink);
    g_strlcpy(preferences.monitor_sink, monitor_sink, sizeof preferences.monitor_sink); preferences.cue_source = 1;
    require_call(radios_audio_engine_apply_preferences(engine, &preferences, &error), &error);
    require_call(radios_audio_engine_set_bus_gate(engine, RADIOS_AUDIO_BUS_CUE, TRUE, &error), &error); run_for(200);
    pcm = capture_offset("MAIN"); goffset cue = capture_offset("CUE"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .08); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .08);
    g_assert_cmpfloat(tone_energy("CUE", cue, 1700), >, .08);
    click(window.root, "deck-0-stop"); click(window.root, "deck-1-stop"); run_for(150);
    click(window.root, "deck-0-playlist"); SavedLoaderList loader = {"Normal playlist AutoDJ pair", NULL, NULL, {0}};
    wait_until(saved_loader_list_ready, &loader, "ordinary playlist is available to the actual player loader");
    gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(loader.tree)), &loader.iter);
    GtkWidget *accept = gtk_dialog_get_widget_for_response(GTK_DIALOG(loader.dialog), GTK_RESPONSE_ACCEPT);
    wait_until(widget_enabled, accept, "ordinary local playlist may be loaded"); gtk_button_clicked(GTK_BUTTON(accept)); pump();
    combo(window.root, "deck-0-autodj-target", "1"); combo(window.root, "deck-1-autodj-target", "0");
    RadioSAudioUiSnapshot source = deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0); g_assert_cmpuint(source.position_ms, ==, 0);
    GtkWidget *card0 = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0");
    g_assert_null(g_object_get_data(G_OBJECT(card0), "radios-player-rotation-id"));
    click(card0, "deck-0-autodj-start"); RadioSPlayoutSnapshot run = rotation_run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 2100);
    g_assert_cmpint(run.deck, ==, 0); g_assert_cmpuint(run.cue_in_ms, ==, 2000);
    radios_audio_engine_snapshot(engine, &audio); g_assert_cmpuint(audio.decks[0].generation, >, source.generation);
    g_assert_true(scoped_owns_deck(&window, 0)); g_assert_true(scoped_owns_deck(&window, 1));
    g_assert_true(gtk_widget_is_sensitive(find(window.root, "deck-0-seek"))); g_assert_false(gtk_widget_is_sensitive(find(window.root, "deck-1-seek")));
    click(window.root, "deck-1-pause"); rotation_run_wait(&window, RADIOS_PLAYOUT_PAUSED, 0, 2000); g_assert_true(scoped_owns_deck(&window, 1));
    click(window.root, "deck-1-play"); rotation_run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 2100);
    /* No station transition rule is saved here: the default shown by the
     * Otomasyon selector and Ayarlar › Auto DJ (Crossfade · 3 sn) applies. */
    run = rotation_run_wait(&window, RADIOS_PLAYOUT_OVERLAP, 0, 4000); g_assert_cmpuint(run.transition, ==, RADIOS_AUDIO_RUN_CROSSFADE);
    run = rotation_run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 200); g_assert_cmpint(run.deck, ==, 1);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_EMPTY, 0); g_assert_true(scoped_owns_deck(&window, 0));
    /* Even a now-empty source card still controls the shared AutoDJ owner. */
    click(window.root, "deck-0-pause"); rotation_run_wait(&window, RADIOS_PLAYOUT_PAUSED, 1, 200);
    g_assert_true(scoped_owns_deck(&window, 0)); g_assert_true(scoped_owns_deck(&window, 1));
    click(window.root, "deck-0-play"); rotation_run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 200);
    pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .08);
    click(window.root, "deck-0-stop"); run_for(200); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    radios_audio_engine_unref(engine); json_node_free(playlist); json_node_free(a); json_node_free(b); window_close(&window);
}


/* These real 12-second sources retain their physical tails. The early AutoDJ
 * boundary starts a fade but must not shorten the incoming tempo-return span. */
static JsonNode *tempo_return_markers(JsonNode *detail, guint out_ms)
{
    JsonObject *media = json_object_get_object_member(json_node_get_object(detail), "media");
    JsonObject *version = json_object_get_object_member(json_node_get_object(detail), "version");
    guint duration = (guint)radios_json_int(version, "duration_ms", 0);
    g_assert_cmpuint(duration, >, out_ms);
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body), *markers = json_object_new();
    json_object_set_string_member(object, "media_id", radios_json_string(media, "_id", ""));
    json_object_set_int_member(object, "expected_revision", radios_json_int(media, "_rev", 0));
    json_object_set_null_member(object, "expected_marker_revision");
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    const gchar *names[] = {"cue_in_ms", "intro_ms", "out_ms", "fade_start_ms", "fade_end_ms", "auto_next_ms", "overlap_ms"};
    const guint values[] = {0, 0, duration, duration, duration, duration, 0};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) json_object_set_int_member(markers, names[i], values[i]);
    json_object_set_boolean_member(markers, "automatic", FALSE);
    json_object_set_int_member(markers, "autodj_in_ms", 0); json_object_set_int_member(markers, "autodj_out_ms", out_ms);
    json_object_set_object_member(object, "markers", markers);
    JsonNode *saved = request("/v1/workflows/desktop-media/markers-save", body);
    json_node_free(body); json_node_free(detail); return saved;
}
static void tempo_return_cursor_continuous(Window *window, const RadioSAudioUiSnapshot *identity,
    RadioSAudioUiSnapshot *previous, RadioSAudioUiSnapshot *current)
{
    g_assert_true(radios_audio_ui_get_snapshot(window->ctx, 1, current));
    g_assert_cmpuint(current->generation, ==, identity->generation);
    g_assert_cmpstr(current->media_id, ==, identity->media_id);
    g_assert_cmpstr(current->version_id, ==, identity->version_id);
    g_assert_cmpuint(current->position_ms, >=, previous->position_ms);
    g_assert_cmpuint(current->position_ms - previous->position_ms, <=, 250);
    g_assert_cmpfloat(current->tempo_ratio + .0001, >=, previous->tempo_ratio);
    *previous = *current;
}
static void tempo_return_label_matches(Window *window, const RadioSAudioUiSnapshot *snapshot)
{
    GtkWidget *label = find(window->root, "deck-1-bpm"); g_assert_true(GTK_IS_LABEL(label));
    /* The card prints in the session locale (tr_TR: "120,0 BPM · -6,3%"). */
    gchar *ascii = g_strdup(gtk_label_get_text(GTK_LABEL(label))); g_strdelimit(ascii, ",", '.');
    const gchar *text = ascii, *percentage = strstr(text, " BPM · ");
    g_assert_nonnull(percentage); percentage += strlen(" BPM · ");
    gchar *end = NULL; gdouble shown_bpm = g_ascii_strtod(text, &end);
    g_assert_true(end != text); gdouble shown_ratio = 1 + g_ascii_strtod(percentage, &end) / 100;
    g_assert_cmpstr(end, ==, "%");
    /* The card polls every 100 ms. Its displayed percentage must follow the
     * consumed audio clock, including a paused or completed return. */
    g_assert_cmpfloat(fabs(shown_ratio - snapshot->tempo_ratio), <, .004);
    g_assert_cmpfloat(fabs(shown_bpm - snapshot->effective_bpm), <, .6); g_free(ascii);
}
static void test_autodj_tempo_return_ramp_pause_live_UI_MAIN_PCM(void)
{
    JsonNode *previous_policy = document_read("playout_policy", "main");
    JsonObject *policy = json_object_new(); json_object_set_int_member(policy, "schema", 1);
    json_object_set_int_member(policy, "mode", RADIOS_AUDIO_RUN_CROSSFADE);
    json_object_set_int_member(policy, "overlap_ms", 2000); json_object_set_boolean_member(policy, "beat_sync", TRUE);
    JsonNode *saved_policy = document_save("playout_policy", "main", policy); json_object_unref(policy);
    Window window = window_open(); seed_programme(&window, NULL, NULL); navigate(&window, "broadcast"); players_show(&window);
    JsonNode *a = tempo_return_markers(media_import_rhythm(&window, "Tempo return outgoing 120", 700, .3, "music", 120), 4000);
    JsonNode *b = tempo_return_markers(media_import_rhythm(&window, "Tempo return incoming 128", 1700, .3, "music", 128), 11000);
    RadioSRhythmAnalysis grid[2]; JsonNode *details[] = {a, b};
    for (guint i = 0; i < G_N_ELEMENTS(details); ++i) {
        JsonObject *analysis = json_object_get_object_member(json_node_get_object(details[i]), "analysis");
        g_assert_true(radios_rhythm_analysis_read(json_object_get_object_member(analysis, "rhythm"), &grid[i]));
    }
    gdouble matched_ratio = grid[0].bpm / grid[1].bpm;
    g_assert_cmpfloat(matched_ratio, >, .92); g_assert_cmpfloat(matched_ratio, <, .96);
    JsonNode *playlist = playlist_create("Real player tempo return playlist", a, b);
    click(window.root, "deck-0-playlist"); SavedLoaderList loader = {"Real player tempo return playlist", NULL, NULL, {0}};
    wait_until(saved_loader_list_ready, &loader, "verified tempo pair reaches actual player playlist picker");
    gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(loader.tree)), &loader.iter);
    GtkWidget *accept = gtk_dialog_get_widget_for_response(GTK_DIALOG(loader.dialog), GTK_RESPONSE_ACCEPT);
    wait_until(widget_enabled, accept, "verified tempo pair selection enables actual load"); gtk_button_clicked(GTK_BUTTON(accept)); pump();
    combo(window.root, "deck-0-autodj-target", "1"); combo(window.root, "deck-1-autodj-target", "0");
    GtkWidget *card = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0"); g_assert_nonnull(card);
    click(card, "deck-0-autodj-start"); rotation_run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 100);
    RadioSPlayoutSnapshot run = rotation_run_wait(&window, RADIOS_PLAYOUT_OVERLAP, 0, 0);
    g_assert_cmpint(run.next_deck, ==, 1);
    RadioSAudioUiSnapshot identity = deck_wait(&window, 1, RADIOS_AUDIO_UI_PLAYING, 1);
    g_assert_cmpfloat(fabs(identity.tempo_ratio - matched_ratio), <, .0001);
    g_assert_cmpfloat(fabs(identity.effective_bpm - grid[0].bpm), <, .01);
    goffset pcm = capture_offset("MAIN"); run_for(300);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .025); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .01);
    RadioSAudioUiSnapshot current; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 1, &current));
    g_assert_cmpuint(current.generation, ==, identity.generation);
    g_assert_cmpfloat(fabs(current.tempo_ratio - matched_ratio), <, .0001);
    tempo_return_label_matches(&window, &current);
    run = rotation_run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 100); g_assert_cmpint(run.deck, ==, 1);
    RadioSAudioUiSnapshot previous; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 1, &previous));
    g_assert_cmpuint(previous.generation, ==, identity.generation);
    /* A return must be gradual: first observe the middle of the four-second
     * ramp, before pausing it through the now-empty source card. */
    gint64 ramp_observation_start = g_get_monotonic_time();
    gdouble ramp_observation_ratio = previous.tempo_ratio;
    gint64 deadline = ramp_observation_start + 2 * G_TIME_SPAN_SECOND;
    do {
        run_for(25); tempo_return_cursor_continuous(&window, &identity, &previous, &current);
    } while (current.tempo_ratio < matched_ratio + .012 && g_get_monotonic_time() < deadline);
    g_assert_cmpfloat(current.tempo_ratio, >, matched_ratio + .01);
    g_assert_cmpfloat(current.tempo_ratio, <, .99); tempo_return_label_matches(&window, &current);
    gdouble ramp_elapsed = (g_get_monotonic_time() - ramp_observation_start) / (gdouble)G_TIME_SPAN_SECOND;
    g_assert_cmpfloat(fabs(current.tempo_ratio - ramp_observation_ratio - (1 - matched_ratio) * ramp_elapsed / 4), <, .003);
    g_test_message("Incoming ratio %.6f, solo return ratio %.6f, source cursor %" G_GUINT64_FORMAT " ms, generation %" G_GUINT64_FORMAT,
        matched_ratio, current.tempo_ratio, current.position_ms, current.generation);
    click(window.root, "deck-0-pause"); rotation_run_wait(&window, RADIOS_PLAYOUT_PAUSED, 1, 100); run_for(200);
    RadioSAudioUiSnapshot paused; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 1, &paused));
    run_for(700); g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 1, &current));
    rotation_run_wait(&window, RADIOS_PLAYOUT_PAUSED, 1, 100);
    g_assert_cmpuint(current.generation, ==, identity.generation); g_assert_cmpuint(current.position_ms, ==, paused.position_ms);
    g_assert_cmpfloat(fabs(current.tempo_ratio - paused.tempo_ratio), <, .000001);
    g_assert_cmpfloat(current.tempo_ratio, <, .99); tempo_return_label_matches(&window, &current);
    click(window.root, "deck-0-play"); rotation_run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 100);
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 1, &current));
    gdouble remaining_return_seconds = 4 * (1 - current.tempo_ratio) / (1 - matched_ratio);
    gint64 return_resumed = g_get_monotonic_time();
    previous = current; pcm = capture_offset("MAIN"); deadline = return_resumed + 4500 * 1000;
    do {
        run_for(25); tempo_return_cursor_continuous(&window, &identity, &previous, &current);
    } while (current.tempo_ratio < .999999 && g_get_monotonic_time() < deadline);
    g_assert_cmpfloat(fabs(current.tempo_ratio - 1), <, .000001);
    gdouble return_elapsed = (g_get_monotonic_time() - return_resumed) / (gdouble)G_TIME_SPAN_SECOND;
    g_assert_cmpfloat(fabs(return_elapsed - remaining_return_seconds), <, .25);
    g_assert_cmpfloat(fabs(current.effective_bpm - grid[1].bpm), <, .01);
    /* Actual MAIN PCM carries the original carrier throughout the changing
     * atempo ratio. A resampling-only implementation shifts it toward 1594 Hz. */
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .05);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700 * matched_ratio), <, .025);
    run_for(120); g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 1, &current));
    g_assert_cmpuint(current.generation, ==, identity.generation); tempo_return_label_matches(&window, &current);
    gchar *shown = g_strdup(gtk_label_get_text(GTK_LABEL(find(window.root, "deck-1-bpm")))); g_strdelimit(shown, ",", '.');
    g_assert_nonnull(strstr(shown, "0.0%")); g_free(shown);
    g_assert_cmpfloat(measure("CUE").peak[0], <, .0001); click(window.root, "deck-0-stop"); run_for(200);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001); assert_saved_playlist_unchanged(playlist);
    json_node_free(playlist); json_node_free(a); json_node_free(b); window_close(&window);
    if (previous_policy) {
        JsonNode *restored = document_save("playout_policy", "main", json_object_get_object_member(json_node_get_object(previous_policy), "config"));
        json_node_free(restored); json_node_free(previous_policy);
    } else {
        JsonNode *latest = document_read("playout_policy", "main"), *body = body_new();
        JsonObject *object = json_node_get_object(body), *record = json_node_get_object(latest);
        json_object_set_string_member(object, "kind", "playout_policy");
        json_object_set_string_member(object, "document_id", radios_json_string(record, "_id", ""));
        json_object_set_int_member(object, "expected_revision", radios_json_int(record, "_rev", 0));
        gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
        JsonNode *removed = request("/v1/workflows/desktop/delete", body); json_node_free(removed); json_node_free(body); json_node_free(latest);
    }
    json_node_free(saved_policy);
    g_test_message("Matched overlap returns gradually to native BPM in four seconds of consumed MAIN audio; real card percentage, pause freeze, generation/source continuity and original pitch are verified.");
}


typedef struct { GtkWidget *card; gint64 in_ms, out_ms; } MarkerRoundtripCommit;
static gboolean marker_roundtrip_committed(gpointer data)
{
    MarkerRoundtripCommit *commit = data;
    JsonNode *detail = g_object_get_data(G_OBJECT(commit->card), "radios-deck-media-detail");
    JsonObject *markers = detail ? json_object_get_object_member(json_node_get_object(detail), "markers") : NULL;
    return markers && radios_json_int(markers, "autodj_in_ms", -1) == commit->in_ms &&
        radios_json_int(markers, "autodj_out_ms", -1) == commit->out_ms;
}
static void marker_roundtrip_wait(GtkWidget *card, gint64 in_ms, gint64 out_ms)
{
    MarkerRoundtripCommit commit = {card, in_ms, out_ms};
    wait_until(marker_roundtrip_committed, &commit, "real marker-save ACK updates card with exact AutoDJ IN/OUT");
}
/* The inspector hosts the shared marker editor: at zoom 1 its timeline maps
 * the whole duration between 16 px margins. */
static void marker_editor_point(GtkWidget *area, gdouble milliseconds, gint64 duration_ms)
{
    GdkEventButton event = {0}; event.type = GDK_BUTTON_PRESS; event.button = 1;
    event.window = gtk_widget_get_window(area); g_assert_nonnull(event.window);
    event.device = gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(area)));
    event.x = 16 + (gtk_widget_get_allocated_width(area) - 32) * milliseconds / MAX(1.0, (gdouble)duration_ms);
    event.y = gtk_widget_get_allocated_height(area) / 2.0;
    gint origin_x, origin_y; gdk_window_get_origin(event.window, &origin_x, &origin_y);
    event.x_root = origin_x + event.x; event.y_root = origin_y + event.y;
    gboolean handled = FALSE; g_signal_emit_by_name(area, "button-press-event", &event, &handled); g_assert_true(handled);
    event.type = GDK_BUTTON_RELEASE; g_signal_emit_by_name(area, "button-release-event", &event, &handled); pump();
}
static void marker_roundtrip_point(GtkWidget *area, GtkAdjustment *range, gdouble milliseconds, guint button)
{
    GdkEventButton event = {0}; event.type = GDK_BUTTON_PRESS; event.button = button;
    event.window = gtk_widget_get_window(area); g_assert_nonnull(event.window);
    event.device = gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(area)));
    event.x = 8 + (gtk_widget_get_allocated_width(area) - 16) *
        (milliseconds - gtk_adjustment_get_value(range)) / gtk_adjustment_get_page_size(range);
    event.y = gtk_widget_get_allocated_height(area) / 2.0;
    gint origin_x, origin_y; gdk_window_get_origin(event.window, &origin_x, &origin_y);
    event.x_root = origin_x + event.x; event.y_root = origin_y + event.y;
    gboolean handled = FALSE; g_signal_emit_by_name(area, "button-press-event", &event, &handled); g_assert_true(handled); pump();
}
static void marker_roundtrip_context(GtkWidget *card, guint deck, gdouble milliseconds, const gchar *action)
{
    gchar *name = g_strdup_printf("deck-%u-waveform", deck); GtkWidget *area = find(card, name); g_free(name);
    name = g_strdup_printf("deck-%u-waveform-pan", deck); GtkWidget *pan = find(card, name); g_free(name);
    g_assert_true(GTK_IS_DRAWING_AREA(area)); g_assert_true(GTK_IS_RANGE(pan));
    marker_roundtrip_point(area, gtk_range_get_adjustment(GTK_RANGE(pan)), milliseconds, 3);
    GList *menus = gtk_menu_get_for_attach_widget(area); g_assert_nonnull(menus);
    GtkWidget *menu = menus->data, *selected = NULL; GList *items = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *item = items; item; item = item->next) {
        if (GTK_IS_CHECK_MENU_ITEM(item->data)) gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(item->data), FALSE);
        if (!g_strcmp0(g_object_get_data(G_OBJECT(item->data), "autodj-marker-action"), action)) selected = item->data;
    }
    g_list_free(items); g_assert_true(GTK_IS_MENU_ITEM(selected)); g_assert_true(gtk_widget_is_sensitive(selected));
    gtk_menu_popdown(GTK_MENU(menu)); gtk_menu_item_activate(GTK_MENU_ITEM(selected)); pump();
}
static void test_interactive_autodj_markers_saved_playlist_pair_roundtrip_MAIN_PCM(void)
{
    Window window = window_open(); seed_programme(&window, NULL, NULL); navigate(&window, "broadcast"); players_show(&window);
    window.ctx->notify = rhythm_notice;
    combo_index(window.root, "programme-transition-mode", 1); spin(window.root, "programme-transition-overlap", 2);
    JsonNode *a = media_import(&window, "Interactive saved IN OUT A", 700, .25, "music");
    JsonNode *b = media_import(&window, "Interactive saved IN OUT B", 1700, .25, "music");
    JsonNode *c = media_import(&window, "Interactive saved IN OUT C", 2700, .25, "music");
    JsonNode *playlist = playlist_create("Playlist before interactive marker edits", a, b);
    playlist = playlist_append_verified(playlist, c);
    click(window.root, "deck-0-playlist"); SavedLoaderList loader = {"Playlist before interactive marker edits", NULL, NULL, {0}};
    wait_until(saved_loader_list_ready, &loader, "existing saved playlist loads before editing its media markers");
    gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(loader.tree)), &loader.iter);
    GtkWidget *accept = gtk_dialog_get_widget_for_response(GTK_DIALOG(loader.dialog), GTK_RESPONSE_ACCEPT);
    wait_until(widget_enabled, accept, "actual player loader accepts saved media bindings"); gtk_button_clicked(GTK_BUTTON(accept)); pump();
    GtkWidget *card0 = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0");
    GtkWidget *card1 = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-1"); g_assert_nonnull(card0); g_assert_nonnull(card1);
    click(card0, "deck-0-play"); deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100); click(card0, "deck-0-stop");
    RadioSAudioUiSnapshot source = deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    GError *error = NULL; require_call(radios_audio_ui_load_media(window.ctx, card1, json_node_get_object(b), 1, FALSE, &error), &error);
    RadioSAudioUiSnapshot target = deck_wait(&window, 1, RADIOS_AUDIO_UI_READY, 0);
    click(card0, "deck-0-waveform-inspect"); wait_until(dialog_exists, "deck-0-waveform-dialog", "real source waveform inspector opens");
    GtkWidget *inspector = dialog_find("deck-0-waveform-dialog"); run_for(150);
    toggle(inspector, "deck-0-waveform-autodj-snap", FALSE);
    GtkWidget *area = find(inspector, "qwf-timeline"); g_assert_true(GTK_IS_DRAWING_AREA(area));
    /* The inspector hosts the shared marker editor: both points are placed on
     * its timeline and written by one explicit save. */
    gint64 source_duration = radios_json_int(json_object_get_object_member(json_node_get_object(a), "version"), "duration_ms", 0);
    marker_editor_point(area, 1500, source_duration); click(inspector, "qwf-set-8");
    marker_editor_point(area, 3500, source_duration); click(inspector, "qwf-set-9");
    click(inspector, "qwf-markers-save"); marker_roundtrip_wait(card0, 1500, 3500);
    const gchar *status = gtk_label_get_text(GTK_LABEL(find(inspector, "editor-status")));
    g_assert_true(strstr(status, "kaydedildi") || strstr(status, "Kayıt tamamlandı"));
    gtk_dialog_response(GTK_DIALOG(inspector), GTK_RESPONSE_CLOSE); pump();
    /* The incoming song is edited from the compact waveform's real context
     * menu, so both available user surfaces cross the genuine HTTP/CAS path. */
    marker_roundtrip_context(card1, 1, 2000, "in"); marker_roundtrip_wait(card1, 2000, -1);
    marker_roundtrip_context(card1, 1, 4500, "out"); marker_roundtrip_wait(card1, 2000, 4500);
    JsonNode *saved_a = media_read(a), *saved_b = media_read(b);
    JsonObject *marks_a = json_object_get_object_member(json_node_get_object(saved_a), "markers");
    JsonObject *marks_b = json_object_get_object_member(json_node_get_object(saved_b), "markers");
    g_assert_cmpint(radios_json_int(marks_a, "autodj_in_ms", -1), ==, 1500); g_assert_cmpint(radios_json_int(marks_a, "autodj_out_ms", -1), ==, 3500);
    g_assert_cmpint(radios_json_int(marks_b, "autodj_in_ms", -1), ==, 2000); g_assert_cmpint(radios_json_int(marks_b, "autodj_out_ms", -1), ==, 4500);
    g_assert_cmpint(radios_json_int(marks_a, "cue_in_ms", -1), ==, 0); g_assert_cmpint(radios_json_int(marks_b, "cue_in_ms", -1), ==, 0);
    g_assert_cmpint(radios_json_int(marks_a, "out_ms", -1), ==, 6001); g_assert_cmpint(radios_json_int(marks_b, "out_ms", -1), ==, 6001);
    RadioSAudioUiSnapshot unchanged; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 0, &unchanged));
    g_assert_cmpuint(unchanged.generation, ==, source.generation); g_assert_cmpuint(unchanged.position_ms, ==, source.position_ms);
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 1, &unchanged));
    g_assert_cmpuint(unchanged.generation, ==, target.generation); g_assert_cmpuint(unchanged.position_ms, ==, target.position_ms);
    assert_saved_playlist_unchanged(playlist);
    /* Start without reloading the playlist or its already loaded cards. Fresh
     * prepare must resolve the updated markers from immutable media bindings. */
    combo(window.root, "deck-0-autodj-target", "1"); combo(window.root, "deck-1-autodj-target", "0");
    click(card0, "deck-0-autodj-start"); RadioSPlayoutSnapshot run = rotation_run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 1600);
    g_assert_cmpuint(run.cue_in_ms, ==, 1500); g_assert_cmpuint(run.duration_ms, >=, 6000);
    goffset pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .1);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), <, .01);
    run = rotation_run_wait(&window, RADIOS_PLAYOUT_OVERLAP, 0, 0);
    g_assert_cmpuint(run.position_ms, >=, 3490); g_assert_cmpuint(run.position_ms, <, 3750);
    RadioSAudioUiSnapshot incoming = deck_wait(&window, 1, RADIOS_AUDIO_UI_PLAYING, 2000);
    g_assert_cmpuint(incoming.position_ms, <, 2350);
    pcm = capture_offset("MAIN"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .02); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .015);
    run = rotation_run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 2000); g_assert_cmpuint(run.cue_in_ms, ==, 2000);
    run = rotation_run_wait(&window, RADIOS_PLAYOUT_OVERLAP, 1, 0);
    g_assert_cmpuint(run.position_ms, >=, 4490); g_assert_cmpuint(run.position_ms, <, 4750);
    pcm = capture_offset("MAIN"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .02); g_assert_cmpfloat(tone_energy("MAIN", pcm, 2700), >, .015);
    run = rotation_run_wait(&window, RADIOS_PLAYOUT_PLAYING, 2, 100); g_assert_cmpstr(run.title, ==, "Interactive saved IN OUT C");
    pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 2700), >, .1);
    rotation_stop_run(&window); g_assert_cmpfloat(measure("CUE").peak[0], <, .0001); assert_saved_playlist_unchanged(playlist);
    JsonNode *after_a = media_read(a), *after_b = media_read(b);
    g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(saved_a), "markers"), json_object_get_member(json_node_get_object(after_a), "markers")));
    g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(saved_b), "markers"), json_object_get_member(json_node_get_object(after_b), "markers")));
    json_node_free(after_a); json_node_free(after_b); json_node_free(saved_a); json_node_free(saved_b);
    json_node_free(playlist); json_node_free(a); json_node_free(b); json_node_free(c); window_close(&window);
    g_test_message("Real inspector/context-menu marker saves reach backend and freshly prepared AutoDJ pair without playlist reload: IN1500/2000 and OUT3500/4500 trigger genuine MAIN crossfades.");
}

static void test_live_player_widget_destroy_keeps_pcm(void)
{
    Window window = window_open();
    JsonNode *detail = media_import(&window, "Kalıcı canlı oynatıcı kaynağı", 997, 0.2, "music");
    GError *error = NULL;
    for (guint pending_play = 0; pending_play < 2; ++pending_play) {
        GtkWidget *owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(owner);
        require_call(radios_audio_ui_load_media(window.ctx, owner, json_node_get_object(detail), 0, pending_play != 0, &error), &error);
        RadioSAudioUiSnapshot before = {0};
        if (!pending_play) {
            deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
            require_call(radios_audio_ui_play(window.ctx, 0, &error), &error);
            before = deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
        } else {
            /* Destroy before the controller poll accepts the explicit load/
             * Play intent. No visible card may be required for its playback. */
            g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 0, &before));
        }
        gtk_widget_destroy(owner); g_object_unref(owner);
        RadioSAudioUiSnapshot playing = deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, before.position_ms + 100);
        g_assert_cmpuint(playing.generation, ==, before.generation);
        g_assert_cmpstr(playing.title, ==, "Kalıcı canlı oynatıcı kaynağı");
        goffset main_at = capture_offset("MAIN"), cue_at = capture_offset("CUE");
        run_for(400);
        Capture main = capture("MAIN", main_at), cue = capture("CUE", cue_at);
        RadioSAudioUiSnapshot after; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 0, &after));
        g_assert_cmpint(after.state, ==, RADIOS_AUDIO_UI_PLAYING);
        g_assert_cmpuint(after.generation, ==, before.generation);
        g_assert_cmpuint(after.position_ms, >, playing.position_ms);
        g_assert_cmpfloat(main.peak[0], >, 0.1); g_assert_cmpfloat(main.rms[0], >, 0.05);
        g_assert_cmpfloat(cue.peak[0], <, 0.0001);
        require_call(radios_audio_ui_stop(window.ctx, 0, &error), &error);
        deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    }
    json_node_free(detail); window_close(&window);
}
static JsonNode *scope_authenticated_programme(JsonNode *first, JsonNode *second)
{
    JsonNode *configuration = programme_config(first, second), *body = body_new();
    JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", "programme");
    json_object_set_member(object, "config", deep_copy(configuration));
    JsonNode *prepared = request("/v1/workflows/desktop-playout/prepare", body);
    json_node_free(body); json_node_free(configuration); return prepared;
}
static void scope_legacy_engine_start(RadioSAudioEngine *engine, guint source, JsonNode *detail)
{
    JsonObject *object = json_node_get_object(detail), *media = json_object_get_object_member(object, "media");
    JsonObject *version = json_object_get_object_member(object, "version");
    guint64 duration = (guint64)radios_json_int(version, "duration_ms", 0) * 48;
    gchar *entry = g_uuid_string_random();
    RadioSAudioRunItem item = {.entry_id = entry, .duration_frames = duration, .preferred_deck = (gint)source,
        .estimated_duration = TRUE, .media = {.path = radios_json_string(version, "managed_path", ""),
        .sha256 = radios_json_string(version, "sha256", ""), .media_id = radios_json_string(media, "_id", ""),
        .version_id = radios_json_string(version, "_id", ""), .cue_out_frame = duration}};
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options); GError *error = NULL;
    require_call(radios_audio_engine_player_runner_configure(engine, source, &item, 1, &options, &error), &error);
    require_call(radios_audio_engine_player_runner_action(engine, source, RADIOS_AUDIO_PLAYER_RUN_START, &error), &error);
    g_free(entry); player_run_wait(engine, source, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
}
static gboolean scope_workspace_ready(gpointer value)
{ return radios_player_workspace_ready(value); }
typedef struct { RadioSPlayerWorkspace *workspace; guint source; } ScopeReady;
static gboolean scope_workspace_execution_ready(gpointer value)
{
    ScopeReady *scope = value; GError *error = NULL;
    gboolean ready = radios_player_workspace_prepare_scope(scope->workspace, scope->source, &error);
    if (!ready) g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PENDING);
    g_clear_error(&error); return ready;
}
static gboolean scope_controller_history_ready(gpointer value)
{ return radios_playout_ui_ready(value); }
static void scope_assert_live_pcm_unchanged(Window *window, RadioSAudioEngine *engine,
    guint source, guint64 generation, guint64 epoch, gdouble frequency)
{
    RadioSAudioRunSnapshot run = player_run_wait(engine, source, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    g_assert_cmpuint(run.epoch, ==, epoch);
    RadioSAudioSnapshot before, after; radios_audio_engine_snapshot(engine, &before);
    g_assert_cmpuint(before.decks[source].generation, ==, generation);
    goffset main_at = capture_offset("MAIN"), cue_at = capture_offset("CUE"); run_for(250);
    radios_audio_engine_snapshot(engine, &after);
    g_assert_cmpuint(after.decks[source].generation, ==, generation);
    g_assert_cmpint(after.decks[source].state, ==, RADIOS_AUDIO_PLAYING);
    g_assert_cmpuint(after.decks[source].cursor_frames, >, before.decks[source].cursor_frames);
    g_assert_cmpfloat(tone_energy("MAIN", main_at, frequency), >, 0.05);
    g_assert_cmpfloat(capture("CUE", cue_at).peak[0], <, 0.0001);
    (void)window;
}
static void test_workspace_rejected_scope_close_preserves_legacy_pcm(void)
{
    Window window = window_open();
    JsonNode *detail = media_import(&window, "Kapsam dışı canlı sıra", 997, .2, "music");
    JsonNode *prepared = scope_authenticated_programme(detail, NULL);
    RadioSViewContext context = *window.ctx; context.playout = NULL;
    GtkWidget *owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(owner);
    RadioSPlayerWorkspace *workspace = radios_player_workspace_new(&context, owner); g_assert_nonnull(workspace);
    context.players = workspace; wait_until(scope_workspace_ready, workspace, "independent workspace authenticated topology baseline");
    ScopeReady ready = {workspace, 0}; wait_until(scope_workspace_execution_ready, &ready, "scope private history baseline loaded");
    RadioSAutomationScope selected = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0};
    RadioSAutomationBinding binding; GError *error = NULL;
    require_call(radios_automation_scope_resolve(radios_player_workspace_topology(workspace), &selected, &binding, &error), &error);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); scope_legacy_engine_start(engine, 0, detail);
    RadioSAudioRunSnapshot run; radios_audio_engine_player_runner_snapshot(engine, 0, &run);
    RadioSAudioSnapshot before; radios_audio_engine_snapshot(engine, &before);
    g_assert_false(radios_player_workspace_start_prepared(workspace, &binding, json_node_get_object(prepared), RADIOS_AUTODJ_SEMI, &error));
    g_assert_nonnull(error); g_assert_false(g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PENDING)); g_clear_error(&error);
    RadioSPlayoutSnapshot snapshot; g_assert_true(radios_player_workspace_snapshot(workspace, 0, &snapshot));
    g_assert_cmpint(snapshot.state, ==, RADIOS_PLAYOUT_IDLE); g_assert_cmpuint(snapshot.total, ==, 0); g_assert_cmpint(snapshot.deck, ==, -1);
    radios_player_workspace_close(workspace); radios_player_workspace_free(workspace); context.players = NULL;
    gtk_widget_destroy(owner); g_object_unref(owner);
    scope_assert_live_pcm_unchanged(&window, engine, 0, before.decks[0].generation, run.epoch, 997);
    require_call(radios_audio_engine_player_runner_action(engine, 0, RADIOS_AUDIO_PLAYER_RUN_STOP, &error), &error);
    radios_audio_engine_unref(engine); json_node_free(prepared); json_node_free(detail); window_close(&window);
}
static void test_stale_scope_controller_cannot_control_replacement_pcm(void)
{
    Window window = window_open();
    JsonNode *first = media_import(&window, "Kapsamın önceki sırası", 600, .2, "music");
    JsonNode *replacement = media_import(&window, "Yeni bağımsız gerçek sıra", 1700, .2, "music");
    JsonNode *prepared = scope_authenticated_programme(first, NULL);
    RadioSViewContext scoped = *window.ctx; scoped.broadcast = NULL; scoped.advertisement = NULL;
    scoped.playout = radios_playout_ui_new_scope(&scoped, 0, window.root); g_assert_nonnull(scoped.playout);
    wait_until(scope_controller_history_ready, &scoped, "scope controller's authenticated private history baseline");
    guint pool[] = {0}; GError *error = NULL;
    require_call(radios_playout_ui_start_prepared_scope(&scoped, window.root, json_node_get_object(prepared), pool, 1, RADIOS_AUTODJ_SEMI, &error), &error);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    RadioSAudioRunSnapshot owned = player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    /* Replace through the legacy engine entry point without notifying the
     * stale controller. Its old private slot is reused under a new epoch. */
    require_call(radios_audio_engine_player_runner_action(engine, 0, RADIOS_AUDIO_PLAYER_RUN_STOP, &error), &error);
    scope_legacy_engine_start(engine, 0, replacement);
    RadioSAudioRunSnapshot run; radios_audio_engine_player_runner_snapshot(engine, 0, &run); g_assert_cmpuint(run.epoch, !=, owned.epoch);
    RadioSAudioSnapshot before; radios_audio_engine_snapshot(engine, &before);
    run_for(100); RadioSPlayoutSnapshot snapshot; g_assert_true(radios_playout_ui_get_snapshot(&scoped, &snapshot));
    g_assert_cmpint(snapshot.state, ==, RADIOS_PLAYOUT_IDLE); g_assert_cmpuint(snapshot.total, ==, 0);
    g_assert_cmpint(snapshot.deck, ==, -1); g_assert_cmpstr(snapshot.entry_id, ==, "");
    const RadioSPlayoutAction commands[] = {RADIOS_PLAYOUT_PAUSE, RADIOS_PLAYOUT_RESUME, RADIOS_PLAYOUT_NEXT,
        RADIOS_PLAYOUT_PREVIOUS, RADIOS_PLAYOUT_STOP};
    for (guint i = 0; i < G_N_ELEMENTS(commands); ++i) {
        g_assert_false(radios_playout_ui_action(&scoped, commands[i], &error)); g_assert_nonnull(error); g_clear_error(&error);
    }
    /* Public Stop rejects lost ownership; internal close remains harmless. */
    radios_playout_ui_close(scoped.playout); radios_playout_ui_unref(scoped.playout); scoped.playout = NULL;
    scope_assert_live_pcm_unchanged(&window, engine, 0, before.decks[0].generation, run.epoch, 1700);
    require_call(radios_audio_engine_player_runner_action(engine, 0, RADIOS_AUDIO_PLAYER_RUN_STOP, &error), &error);
    radios_audio_engine_unref(engine); json_node_free(prepared); json_node_free(first); json_node_free(replacement); window_close(&window);
}
static JsonNode *scope_short_cue(JsonNode *detail)
{
    JsonObject *media = json_object_get_object_member(json_node_get_object(detail), "media");
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body), *markers = json_object_new();
    json_object_set_string_member(object, "media_id", radios_json_string(media, "_id", ""));
    json_object_set_int_member(object, "expected_revision", radios_json_int(media, "_rev", 0));
    json_object_set_null_member(object, "expected_marker_revision");
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    const gchar *names[] = {"cue_in_ms", "intro_ms", "out_ms", "fade_start_ms", "fade_end_ms", "auto_next_ms", "overlap_ms"};
    const gint64 values[] = {0, 0, 2000, 2000, 2000, 2000, 0};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) json_object_set_int_member(markers, names[i], values[i]);
    json_object_set_boolean_member(markers, "automatic", FALSE); json_object_set_object_member(object, "markers", markers);
    JsonNode *saved = request("/v1/workflows/desktop-media/markers-save", body);
    json_node_free(body); json_node_free(detail); return saved;
}
static void test_workspace_two_clusters_transition_pause_independent_pcm(void)
{
    JsonNode *previous_policy = document_read("playout_policy", "main"); JsonObject *policy = json_object_new();
    json_object_set_int_member(policy, "schema", 1); json_object_set_int_member(policy, "mode", RADIOS_AUDIO_RUN_CROSSFADE);
    json_object_set_int_member(policy, "overlap_ms", 500); json_object_set_boolean_member(policy, "beat_sync", FALSE);
    JsonNode *saved_policy = document_save("playout_policy", "main", policy); json_object_unref(policy);
    Window window = window_open(); RadioSViewContext context = *window.ctx; context.playout = NULL;
    GtkWidget *owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(owner);
    RadioSPlayerWorkspace *workspace = radios_player_workspace_new(&context, owner); g_assert_nonnull(workspace);
    context.players = workspace; wait_until(scope_workspace_ready, workspace, "two-cluster workspace definitions ready");
    RadioSPlayerTopology topology = *radios_player_workspace_topology(workspace); GError *error = NULL; gchar groups[2][37];
    require_call(radios_player_topology_add_cluster(&topology, "Birinci gerçek küme", groups[0], &error), &error);
    require_call(radios_player_topology_add_cluster(&topology, "İkinci gerçek küme", groups[1], &error), &error);
    for (guint i = 0; i < 2; ++i) {
        require_call(radios_player_topology_set_cluster(&topology, i, groups[i], &error), &error);
        require_call(radios_player_topology_set_cluster(&topology, i + 2, groups[i], &error), &error);
        require_call(radios_player_topology_set_autodj(&topology, i, RADIOS_AUTODJ_TRANSITIONS, (gint)i + 2, &error), &error);
    }
    require_call(radios_player_workspace_replace(workspace, &topology, &error), &error);
    RadioSAutomationBinding bindings[2];
    for (guint i = 0; i < 2; ++i) {
        RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER}; g_strlcpy(scope.cluster_id, groups[i], sizeof scope.cluster_id);
        require_call(radios_automation_scope_resolve(radios_player_workspace_topology(workspace), &scope, &bindings[i], &error), &error);
        g_assert_cmpuint(bindings[i].source, ==, i); g_assert_cmpuint(bindings[i].count, ==, 2);
        ScopeReady ready = {workspace, i}; wait_until(scope_workspace_execution_ready, &ready, "cluster scope private history baseline ready");
    }
    JsonNode *media[4] = {
        media_import(&window, "Küme A kaynak", 600, .2, "music"), media_import(&window, "Küme B kaynak", 900, .2, "music"),
        media_import(&window, "Küme A hedef", 1700, .2, "music"), media_import(&window, "Küme B hedef", 2500, .2, "music")};
    media[0] = scope_short_cue(media[0]); media[1] = scope_short_cue(media[1]);
    JsonNode *prepared[2] = {scope_authenticated_programme(media[0], media[2]), scope_authenticated_programme(media[1], media[3])};
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    for (guint i = 0; i < 2; ++i) {
        require_call(radios_player_workspace_start_prepared(workspace, &bindings[i], json_node_get_object(prepared[i]), RADIOS_AUTODJ_TRANSITIONS, &error), &error);
        player_run_wait(engine, i, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    }
    goffset main_at = capture_offset("MAIN"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", main_at, 600), >, .05); g_assert_cmpfloat(tone_energy("MAIN", main_at, 900), >, .05);
    RadioSAudioRunSnapshot runs[2];
    for (guint i = 0; i < 2; ++i) {
        runs[i] = player_run_wait(engine, i, RADIOS_AUDIO_RUN_PLAYING, 1, 4800);
        g_assert_cmpint(runs[i].deck_current, ==, (gint)i + 2);
        g_assert_true(runs[i].owned[i]); g_assert_true(runs[i].owned[i + 2]);
        g_assert_false(runs[i].owned[1 - i]); g_assert_false(runs[i].owned[3 - i]);
        RadioSPlayoutSnapshot snapshot; g_assert_true(radios_player_workspace_snapshot(workspace, i, &snapshot));
        g_assert_cmpuint(snapshot.total, ==, 2); g_assert_cmpuint(snapshot.overlap_ms, ==, 500);
    }
    require_call(radios_player_workspace_action(workspace, 0, RADIOS_PLAYOUT_PAUSE, &error), &error);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PAUSED, 1, 0); run_for(100);
    main_at = capture_offset("MAIN"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", main_at, 1700), <, .01); g_assert_cmpfloat(tone_energy("MAIN", main_at, 2500), >, .05);
    RadioSAudioRunSnapshot other = player_run_wait(engine, 1, RADIOS_AUDIO_RUN_PLAYING, 1, runs[1].actual_current_frame);
    g_assert_cmpuint(other.epoch, ==, runs[1].epoch);
    require_call(radios_player_workspace_action(workspace, 0, RADIOS_PLAYOUT_RESUME, &error), &error);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 1, 4800);
    main_at = capture_offset("MAIN"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", main_at, 1700), >, .05); g_assert_cmpfloat(tone_energy("MAIN", main_at, 2500), >, .05);
    radios_player_workspace_free(workspace); context.players = NULL; gtk_widget_destroy(owner); g_object_unref(owner);
    run_for(150); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    for (guint i = 0; i < 2; ++i) json_node_free(prepared[i]);
    for (guint i = 0; i < 4; ++i) json_node_free(media[i]);
    radios_audio_engine_unref(engine); window_close(&window);
    if (previous_policy) {
        JsonNode *restored = document_save("playout_policy", "main", json_object_get_object_member(json_node_get_object(previous_policy), "config"));
        json_node_free(restored); json_node_free(previous_policy);
    } else {
        JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body), *record = json_node_get_object(saved_policy);
        json_object_set_string_member(object, "kind", "playout_policy");
        json_object_set_string_member(object, "document_id", radios_json_string(record, "_id", ""));
        json_object_set_int_member(object, "expected_revision", radios_json_int(record, "_rev", 0));
        gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
        JsonNode *removed = request("/v1/workflows/desktop/delete", body); json_node_free(removed); json_node_free(body);
    }
    json_node_free(saved_policy);
}
typedef struct { guint callbacks, destroyed, source; GError *error; } ScopeExecutionResult;
static void execution_result(RadioSViewContext *ctx, const RadioSAutomationScope *scope, guint source, const GError *error, gpointer value)
{
    (void)ctx; (void)scope; ScopeExecutionResult *result = value; ++result->callbacks; result->source = source;
    if (error) result->error = g_error_copy(error);
}
static void execution_result_destroy(gpointer value) { ++((ScopeExecutionResult *)value)->destroyed; }
static gboolean execution_completed(gpointer value) { return ((ScopeExecutionResult *)value)->destroyed != 0; }
static void execution_wait(ScopeExecutionResult *result)
{
    wait_until(execution_completed, result, "application-owned scope operation completion");
    g_assert_cmpuint(result->callbacks, ==, 1); g_assert_cmpuint(result->destroyed, ==, 1); g_assert_no_error(result->error);
}
static gboolean execution_main_ready(gpointer value)
{ return radios_audio_ui_ready(value, RADIOS_AUDIO_UI_MAIN); }
static void test_execution_cluster_and_aggregate_hidden_pair_preserve_other_pcm(void)
{
    JsonNode *previous_policy = document_read("playout_policy", "main"); JsonObject *policy = json_object_new();
    json_object_set_int_member(policy, "schema", 1); json_object_set_int_member(policy, "mode", RADIOS_AUDIO_RUN_CROSSFADE);
    json_object_set_int_member(policy, "overlap_ms", 500); json_object_set_boolean_member(policy, "beat_sync", FALSE);
    JsonNode *saved_policy = document_save("playout_policy", "main", policy); json_object_unref(policy);
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "application workspace baseline");
    JsonNode *a = routing_short_markers(media_import(&window, "Kapsam playlist A", 600, .2, "music"), 1800);
    JsonNode *b = media_import(&window, "Kapsam playlist B", 1700, .2, "music");
    JsonNode *other = media_import_rhythm(&window, "Bağımsız çalmaya devam", 2500, .35, "music", 120);
    JsonNode *playlist = playlist_create("Ayrı yüklenen kapsam playlisti", a, b);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); scope_legacy_engine_start(engine, 4, other);
    RadioSAudioRunSnapshot independent; radios_audio_engine_player_runner_snapshot(engine, 4, &independent);
    RadioSAudioSnapshot before; radios_audio_engine_snapshot(engine, &before);
    RadioSPlayerTopology topology = *radios_player_workspace_topology(window.ctx->players); gchar cluster[37]; GError *error = NULL;
    require_call(radios_player_topology_add_cluster(&topology, "Gerçek seçili küme", cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 0, cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 2, cluster, &error), &error);
    require_call(radios_player_topology_set_autodj(&topology, 0, RADIOS_AUTODJ_TRANSITIONS, 2, &error), &error);
    require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(window.ctx); g_assert_nonnull(execution);
    GtkWidget *card = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0"); g_assert_nonnull(card); gtk_widget_hide(card);
    for (guint aggregate = 0; aggregate < 2; ++aggregate) {
        RadioSAutomationScope scope = {.kind = aggregate ? RADIOS_AUTOMATION_SCOPE_UNCLUSTERED : RADIOS_AUTOMATION_SCOPE_CLUSTER};
        if (!aggregate) g_strlcpy(scope.cluster_id, cluster, sizeof scope.cluster_id);
        else {
            topology = *radios_player_workspace_topology(window.ctx->players);
            require_call(radios_player_topology_set_cluster(&topology, 0, "", &error), &error);
            require_call(radios_player_topology_set_cluster(&topology, 2, "", &error), &error);
            require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
        }
        ScopeExecutionResult loaded = {0};
        require_call(radios_player_scope_execution_load_playlist(execution, &scope,
            radios_json_string(json_node_get_object(playlist), "_id", ""), execution_result, &loaded, execution_result_destroy, &error), &error);
        /* Loading binds only the queue recipient: editing the independent
         * AutoDJ policy during its request must not reject the playlist. */
        topology = *radios_player_workspace_topology(window.ctx->players);
        require_call(radios_player_topology_set_autodj(&topology, 0, RADIOS_AUTODJ_SEMI, -1, &error), &error);
        require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
        execution_wait(&loaded); g_assert_cmpuint(loaded.source, ==, 0);
        require_call(radios_player_workspace_programme_saved(window.ctx->players, &scope, &error), &error);
        topology = *radios_player_workspace_topology(window.ctx->players);
        require_call(radios_player_topology_set_autodj(&topology, 0, RADIOS_AUTODJ_TRANSITIONS, 2, &error), &error);
        require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
        RadioSAudioSnapshot unloaded; radios_audio_engine_snapshot(engine, &unloaded);
        g_assert_cmpint(unloaded.decks[0].state, ==, RADIOS_AUDIO_EMPTY);
        JsonObject *programme = NULL; require_call(radios_player_workspace_get_programme(window.ctx->players, &scope, &programme, &error), &error);
        g_assert_cmpuint(json_array_get_length(json_object_get_array_member(programme, "events")), ==, 2);
        g_assert_false(json_object_has_member(programme, "autodj_players")); json_object_unref(programme);
        ScopeExecutionResult played = {0};
        require_call(radios_player_scope_execution_play_programme(execution, &scope, NULL, RADIOS_AUTODJ_TRANSITIONS,
            execution_result, &played, execution_result_destroy, &error), &error);
        execution_wait(&played); player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
        /* Operate the real live-player tree while playback is in flight.
         * Its mirrored UI selection hides the accepted source/target cards. */
        GtkWidget *tree = find(window.root, "live-player-definitions"); g_assert_nonnull(tree);
        radios_player_tree_ui_refresh(tree);
        RadioSAutomationScope other_view = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 4};
        g_assert_true(radios_player_tree_ui_select(tree, &other_view));
        g_assert_false(gtk_widget_get_visible(gtk_widget_get_parent(card)));
        RadioSAudioRunSnapshot owned; radios_audio_engine_player_runner_snapshot(engine, 0, &owned);
        g_assert_true(owned.owned[0]); g_assert_true(owned.owned[2]); g_assert_false(owned.owned[4]);
        RadioSViewContext *managed = radios_player_workspace_running_context(window.ctx->players, 0); g_assert_nonnull(managed);
        g_assert_true(managed == radios_player_workspace_running_context(window.ctx->players, 2));
        RadioSAudioRunSnapshot next = player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 1, 4800);
        g_assert_cmpint(next.deck_current, ==, 2);
        RadioSPlayerScopeExecutionSnapshot observed;
        g_assert_true(radios_player_scope_execution_snapshot(execution, &scope, &observed));
        g_assert_cmpint(observed.state, ==, RADIOS_SCOPE_EXECUTION_PLAYING);
        require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_PAUSE, &error), &error);
        player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PAUSED, 1, 0);
        g_assert_true(radios_player_scope_execution_snapshot(execution, &scope, &observed));
        g_assert_cmpint(observed.state, ==, RADIOS_SCOPE_EXECUTION_PAUSED);
        wait_until(execution_main_ready, window.ctx, "real MAIN device completed the asynchronous pause flush");
        require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_RESUME, &error), &error);
        player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 1, 4800);
        g_assert_true(radios_player_scope_execution_snapshot(execution, &scope, &observed));
        g_assert_cmpint(observed.state, ==, RADIOS_SCOPE_EXECUTION_PLAYING);
        goffset main_at = capture_offset("MAIN"); run_for(250);
        g_assert_cmpfloat(tone_energy("MAIN", main_at, 1700), >, .05); g_assert_cmpfloat(tone_energy("MAIN", main_at, 2500), >, .05);
        require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_STOP, &error), &error);
        run_for(150);
        g_assert_true(radios_player_scope_execution_snapshot(execution, &scope, &observed));
        g_assert_cmpint(observed.state, ==, RADIOS_SCOPE_EXECUTION_IDLE);
        RadioSAudioRunSnapshot still; radios_audio_engine_player_runner_snapshot(engine, 4, &still);
        g_assert_cmpuint(still.epoch, ==, independent.epoch); g_assert_cmpint(still.state, ==, RADIOS_AUDIO_RUN_PLAYING);
        RadioSAudioSnapshot after; radios_audio_engine_snapshot(engine, &after);
        g_assert_cmpuint(after.decks[4].generation, ==, before.decks[4].generation);
        g_assert_cmpfloat(measure("MAIN").peak[0], >, .1);
    }
    require_call(radios_audio_engine_player_runner_action(engine, 4, RADIOS_AUDIO_PLAYER_RUN_STOP, &error), &error);
    radios_audio_engine_unref(engine); json_node_free(playlist); json_node_free(a); json_node_free(b); json_node_free(other); window_close(&window);
    if (previous_policy) {
        JsonNode *restored = document_save("playout_policy", "main", json_object_get_object_member(json_node_get_object(previous_policy), "config"));
        json_node_free(restored); json_node_free(previous_policy);
    } else {
        JsonNode *body = body_new(); JsonObject *o = json_node_get_object(body), *record = json_node_get_object(saved_policy);
        json_object_set_string_member(o, "kind", "playout_policy"); json_object_set_string_member(o, "document_id", radios_json_string(record, "_id", ""));
        json_object_set_int_member(o, "expected_revision", radios_json_int(record, "_rev", 0));
        gchar *key = g_uuid_string_random(); json_object_set_string_member(o, "idempotency_key", key); g_free(key);
        JsonNode *removed = request("/v1/workflows/desktop/delete", body); json_node_free(removed); json_node_free(body);
    }
    json_node_free(saved_policy);
}
/* The broadcast (MAIN) runner: state, index and actual cursor. */
typedef struct { RadioSAudioEngine *engine; RadioSAudioRunState state; guint index; guint64 position; } MainRunCondition;
static gboolean main_run_matches(gpointer value)
{
    MainRunCondition *condition = value; RadioSAudioRunSnapshot snapshot = {0};
    radios_audio_engine_runner_snapshot(condition->engine, &snapshot);
    return snapshot.state == condition->state && snapshot.index == condition->index && snapshot.actual_current_frame >= condition->position;
}
static RadioSAudioRunSnapshot main_run_wait(RadioSAudioEngine *engine, RadioSAudioRunState state, guint index, guint64 position)
{
    MainRunCondition condition = {engine, state, index, position};
    wait_until(main_run_matches, &condition, "broadcast MAIN runner state");
    RadioSAudioRunSnapshot snapshot = {0}; radios_audio_engine_runner_snapshot(engine, &snapshot); return snapshot;
}
static void test_header_selected_weekly_zero_overlap_filter_pcm(void)
{
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "header selected schema workspace baseline");
    JsonNode *first = routing_short_markers(media_import(&window, "Selected weekly cue-end A", 950, .22, "music"), 1800);
    JsonNode *second = routing_short_markers(media_import(&window, "Selected weekly cue-end B", 2350, .22, "music"), 1800);
    JsonNode *playlist = playlist_create("Gece selected weekly cue-end source", first, second);
    gint64 start = g_get_real_time() / G_TIME_SPAN_DAY * G_TIME_SPAN_DAY;
    JsonNode *configuration = weekly_pcm_configuration(playlist, NULL, start, 1440, "Gece Kulübü");
    JsonObject *calendar = json_object_get_object_member(json_node_get_object(configuration), "calendar");
    JsonObject *definition = json_array_get_object_element(json_object_get_array_member(calendar, "rotation_library"), 0);
    json_object_set_string_member(definition, "name", "Gece Kulübü");
    JsonObject *source = json_array_get_object_element(json_object_get_array_member(definition, "sources"), 0);
    json_object_set_int_member(source, "count", 2);
    JsonObject *settings = json_object_get_object_member(definition, "settings");
    JsonArray *transitions = json_object_get_array_member(settings, "transitions");
    for (guint i = 0; i < json_array_get_length(transitions); ++i) {
        JsonObject *rule = json_array_get_object_element(transitions, i);
        gboolean cue = radios_json_int(rule, "mode", -1) == RADIOS_AUDIO_RUN_CUE;
        json_object_set_int_member(rule, "weight", cue ? 100 : 0);
        if (cue) { json_object_set_int_member(rule, "overlap_ms", 500); json_object_set_string_member(rule, "effect", "lowpass"); }
    }
    JsonNode *saved = document_save("weekly_programme", "RadioS 7/24 Yayın Akışı", json_node_get_object(configuration));
    const gchar *programme_id = radios_json_string(json_node_get_object(saved), "_id", "");
    RadioSPlayerTopology topology = *radios_player_workspace_topology(window.ctx->players); gchar cluster[37]; GError *error = NULL;
    require_call(radios_player_topology_add_cluster(&topology, "Header selected weekly cluster", cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 0, cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 1, cluster, &error), &error);
    require_call(radios_player_topology_set_autodj(&topology, 0, RADIOS_AUTODJ_TRANSITIONS, 1, &error), &error);
    require_call(radios_player_topology_set_autodj(&topology, 1, RADIOS_AUTODJ_TRANSITIONS, 0, &error), &error);
    require_call(radios_player_topology_set_cluster_automation(&topology, cluster, programme_id, &error), &error);
    JsonObject *topology_config = radios_player_topology_to_json(&topology); g_assert_nonnull(topology_config);
    JsonNode *saved_topology = document_save("live_players", "main", topology_config); json_object_unref(topology_config);
    window_close(&window); window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "saved header cluster topology reopened");
    RoutingChoice selected = {find(window.root, "header-programme"), programme_id}; g_assert_true(GTK_IS_COMBO_BOX(selected.combo));
    wait_until(routing_choice_ready, &selected, "actual header saved-weekly selector chooses RadioS 7/24 Yayın Akışı");
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER}; g_strlcpy(scope.cluster_id, cluster, sizeof scope.cluster_id);
    RadioSAutomationBinding binding;
    require_call(radios_automation_scope_resolve(radios_player_workspace_topology(window.ctx->players), &scope, &binding, &error), &error);
    g_assert_cmpuint(binding.count, ==, 2); g_assert_cmpint(binding.autodj_mode, ==, RADIOS_AUTODJ_TRANSITIONS);
    g_assert_cmpstr(binding.automation_id, ==, programme_id);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    /* The broadcast page's continuous general queue takes the header's Play:
     * one flow whose event list follows the transport. The general driver
     * plays the assigned programme on its cluster's two players
     * (player_rotation.c general_scope_event); no second scope flow starts. */
    RadioSPlayerWeekly *driver = general_driver(&window);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001); click(window.root, "header-play");
    RadioSAudioRunSnapshot playing = main_run_wait(engine, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    g_assert_true(weekly_snapshot(driver).armed);
    g_assert_cmpuint(playing.count, >=, 2); g_assert_true(playing.owned[0]); g_assert_true(playing.owned[1]);
    RadioSPlayerScopeExecutionSnapshot execution = {0};
    gboolean known = radios_player_scope_execution_snapshot(radios_player_scope_execution_get(window.ctx), &scope, &execution);
    g_assert_true(!known || !execution.schema_active || execution.state == RADIOS_SCOPE_EXECUTION_IDLE);
    JsonNode *plan = radios_playout_ui_plan_snapshot(window.ctx); g_assert_nonnull(plan);
    JsonArray *entries = json_object_get_array_member(json_node_get_object(plan), "entries"); g_assert_cmpuint(json_array_get_length(entries), >=, 2);
    /* The saved cue is at sound-out, so the requested filter has no actual
     * overlap. The engine has accepted this boundary; its filter contract
     * would reject LOWPASS with zero overlap. Read geometry from the public
     * runtime snapshot, since the queue snapshot exposes order metadata. */
    RadioSPlayoutSnapshot accepted = {0}; g_assert_true(radios_playout_ui_get_snapshot(window.ctx, &accepted));
    g_assert_cmpint(accepted.transition, ==, RADIOS_AUDIO_RUN_CUE);
    g_assert_cmpuint(accepted.overlap_ms, ==, 0); json_node_free(plan);
    JsonNode *expected[] = {first, second}; const gdouble frequencies[] = {950, 2350}; gint decks[2] = {-1, -1};
    for (guint i = 0; i < 2; ++i) {
        playing = main_run_wait(engine, RADIOS_AUDIO_RUN_PLAYING, i, 4800); decks[i] = playing.deck_current;
        g_assert_true(decks[i] == 0 || decks[i] == 1); /* the cluster's players only */
        RadioSAudioUiSnapshot audio; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, (guint)playing.deck_current, &audio));
        g_assert_cmpstr(audio.media_id, ==, radios_json_string(json_object_get_object_member(json_node_get_object(expected[i]), "media"), "_id", ""));
        goffset pcm = capture_offset("MAIN"); run_for(250);
        g_assert_cmpfloat(tone_energy("MAIN", pcm, frequencies[i]), >, .07);
        g_assert_cmpfloat(tone_energy("MAIN", pcm, frequencies[1 - i]), <, .01);
    }
    g_assert_cmpint(decks[0], !=, decks[1]); /* AutoDJ alternates both cluster players */
    click(window.root, "header-pause"); main_run_wait(engine, RADIOS_AUDIO_RUN_PAUSED, 1, 0);
    wait_until(execution_main_ready, window.ctx, "selected weekly header Pause flushes actual MAIN output");
    click(window.root, "header-play"); main_run_wait(engine, RADIOS_AUDIO_RUN_PLAYING, 1, 4800);
    click(window.root, "header-stop"); run_for(200);
    g_assert_false(weekly_snapshot(driver).armed);
    known = radios_player_scope_execution_snapshot(radios_player_scope_execution_get(window.ctx), &scope, &execution);
    g_assert_true(!known || !execution.schema_active); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    g_assert_cmpfloat(measure("CUE").peak[0], <, .0001); weekly_record_unchanged(saved); assert_saved_playlist_unchanged(playlist);
    json_node_free(saved_topology); json_node_free(saved); json_node_free(configuration); json_node_free(playlist);
    json_node_free(first); json_node_free(second); radios_audio_engine_unref(engine); window_close(&window);
}
static void test_execution_schema_current_refills_and_future_wait_cancels_silently(void)
{
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "schema workspace baseline");
    JsonNode *tone = routing_short_markers(media_import(&window, "Atanmış gerçek şema", 2350, .22, "music"), 1800);
    JsonNode *playlist = playlist_create("Atanmış şemanın playlisti", tone, NULL);
    gint64 minute = 60 * G_USEC_PER_SEC, start = g_get_real_time() / minute * minute;
    JsonNode *configuration = weekly_pcm_configuration(playlist, NULL, start, 2, "Seçili kümenin gerçek şema eventi");
    JsonObject *calendar = json_object_get_object_member(json_node_get_object(configuration), "calendar");
    general_single_batch(calendar, playlist);
    JsonNode *saved = document_save("weekly_programme", "Execution current schema", json_node_get_object(configuration));
    RadioSPlayerTopology topology = *radios_player_workspace_topology(window.ctx->players); gchar cluster[37]; GError *error = NULL;
    require_call(radios_player_topology_add_cluster(&topology, "Atanmış şema kümesi", cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 0, cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 2, cluster, &error), &error);
    require_call(radios_player_topology_set_cluster_automation(&topology, cluster, radios_json_string(json_node_get_object(saved), "_id", ""), &error), &error);
    require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER}; g_strlcpy(scope.cluster_id, cluster, sizeof scope.cluster_id);
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(window.ctx); ScopeExecutionResult current = {0};
    require_call(radios_player_scope_execution_play_schema(execution, &scope, RADIOS_AUTODJ_SEMI, execution_result, &current, execution_result_destroy, &error), &error);
    execution_wait(&current); RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    RadioSAudioRunSnapshot first = player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    g_assert_false(radios_player_scope_execution_play_schema(execution, &scope, RADIOS_AUTODJ_SEMI, NULL, NULL, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY); g_clear_error(&error);
    gint64 until = g_get_monotonic_time() + 15 * G_USEC_PER_SEC; RadioSAudioRunSnapshot second = {0};
    do {
        pump(); radios_audio_engine_player_runner_snapshot(engine, 0, &second);
    } while ((second.epoch == first.epoch || second.state != RADIOS_AUDIO_RUN_PLAYING || second.actual_current_frame < 4800) && g_get_monotonic_time() < until);
    g_assert_cmpuint(second.epoch, !=, first.epoch); g_assert_cmpint(second.state, ==, RADIOS_AUDIO_RUN_PLAYING);
    g_assert_true(second.owned[0]); g_assert_false(second.owned[2]);
    g_assert_cmpfloat(measure("MAIN").peak[0], >, .1);
    require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_PAUSE, &error), &error);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PAUSED, 0, 0);
    wait_until(execution_main_ready, window.ctx, "schema pause flush finished on actual MAIN device");
    require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_RESUME, &error), &error);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    RadioSPlayerScopeExecutionSnapshot observed;
    g_assert_true(radios_player_scope_execution_snapshot(execution, &scope, &observed));
    g_assert_cmpint(observed.state, ==, RADIOS_SCOPE_EXECUTION_PLAYING);
    /* Play on the player itself (the card, not the scope) also resumes the
     * schema: the cluster leaves "Duraklatıldı" and keeps scheduling. */
    require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_PAUSE, &error), &error);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PAUSED, 0, 0);
    wait_until(execution_main_ready, window.ctx, "second schema pause flush finished on actual MAIN device");
    g_assert_true(radios_player_scope_execution_snapshot(execution, &scope, &observed));
    g_assert_cmpint(observed.state, ==, RADIOS_SCOPE_EXECUTION_PAUSED);
    require_call(radios_player_workspace_action(window.ctx->players, 0, RADIOS_PLAYOUT_RESUME, &error), &error);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    gint64 settle = g_get_monotonic_time() + 4 * G_USEC_PER_SEC;
    do { pump(); g_assert_true(radios_player_scope_execution_snapshot(execution, &scope, &observed)); }
    while (observed.state != RADIOS_SCOPE_EXECUTION_PLAYING && g_get_monotonic_time() < settle);
    g_assert_cmpint(observed.state, ==, RADIOS_SCOPE_EXECUTION_PLAYING);
    require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_STOP, &error), &error); run_for(150);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    gint64 tomorrow = (g_get_real_time() / G_TIME_SPAN_DAY + 1) * G_TIME_SPAN_DAY + 5 * minute;
    JsonNode *future_config = weekly_pcm_configuration(playlist, NULL, tomorrow, 1, "Gelecekte bekleyen şema");
    JsonNode *future_record = document_save("weekly_programme", "Execution future schema", json_node_get_object(future_config));
    require_call(radios_player_workspace_assign(window.ctx->players, &scope, radios_json_string(json_node_get_object(future_record), "_id", ""), &error), &error);
    ScopeExecutionResult future = {0};
    require_call(radios_player_scope_execution_play_schema(execution, &scope, RADIOS_AUTODJ_SEMI, execution_result, &future, execution_result_destroy, &error), &error);
    execution_wait(&future); RadioSPlayerScopeExecutionSnapshot snapshot;
    g_assert_true(radios_player_scope_execution_snapshot(execution, &scope, &snapshot));
    g_assert_cmpint(snapshot.state, ==, RADIOS_SCOPE_EXECUTION_WAITING); g_assert_true(snapshot.schema_active);
    g_assert_cmpint(snapshot.starts_at_utc_us, ==, tomorrow); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    RadioSPlayerTopology incompatible = *radios_player_workspace_topology(window.ctx->players);
    require_call(radios_player_topology_set_cluster(&incompatible, 1, cluster, &error), &error);
    g_assert_false(radios_player_scope_execution_validate_topology(execution, window.ctx->players, &incompatible, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY); g_clear_error(&error);
    require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_STOP, &error), &error);
    run_for(300); g_assert_false(radios_player_scope_execution_busy(execution, &scope));
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    /* A transition peer's separately armed child schema must stay untouched
     * when the selected source is stopped, even before either acquires PCM. */
    topology = *radios_player_workspace_topology(window.ctx->players);
    require_call(radios_player_topology_set_autodj(&topology, 0, RADIOS_AUTODJ_TRANSITIONS, 2, &error), &error);
    require_call(radios_player_topology_set_automation(&topology, 0, radios_json_string(json_node_get_object(future_record), "_id", ""), &error), &error);
    require_call(radios_player_topology_set_automation(&topology, 2, radios_json_string(json_node_get_object(future_record), "_id", ""), &error), &error);
    require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
    RadioSAutomationScope children[2] = {{.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0}, {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 2}};
    for (guint child = 0; child < 2; ++child) {
        ScopeExecutionResult armed = {0};
        require_call(radios_player_scope_execution_play_schema(execution, &children[child], RADIOS_AUTODJ_SEMI,
            execution_result, &armed, execution_result_destroy, &error), &error); execution_wait(&armed);
    }
    require_call(radios_player_scope_execution_action(execution, &children[0], RADIOS_PLAYOUT_STOP, &error), &error);
    g_assert_true(radios_player_scope_execution_snapshot(execution, &children[0], &snapshot));
    g_assert_cmpint(snapshot.state, ==, RADIOS_SCOPE_EXECUTION_IDLE); g_assert_false(snapshot.schema_active);
    g_assert_true(radios_player_scope_execution_snapshot(execution, &children[1], &snapshot));
    g_assert_cmpint(snapshot.state, ==, RADIOS_SCOPE_EXECUTION_WAITING); g_assert_true(snapshot.schema_active);
    require_call(radios_player_scope_execution_set_repeat(execution, &children[1], TRUE, &error), &error);
    g_assert_true(radios_player_scope_execution_snapshot(execution, &children[1], &snapshot)); g_assert_true(snapshot.repeat);
    require_call(radios_player_scope_execution_action(execution, &children[1], RADIOS_PLAYOUT_STOP, &error), &error);
    /* Losing a runner epoch must also release a paused schema driver. Its
     * later Stop and application shutdown cannot touch the replacement PCM. */
    require_call(radios_player_workspace_assign(window.ctx->players, &scope, radios_json_string(json_node_get_object(saved), "_id", ""), &error), &error);
    ScopeExecutionResult paused_owned = {0};
    require_call(radios_player_scope_execution_play_schema(execution, &scope, RADIOS_AUTODJ_SEMI,
        execution_result, &paused_owned, execution_result_destroy, &error), &error); execution_wait(&paused_owned);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_PAUSE, &error), &error);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PAUSED, 0, 0);
    wait_until(execution_main_ready, window.ctx, "paused owner MAIN flush completed before external replacement");
    require_call(radios_audio_engine_player_runner_action(engine, 0, RADIOS_AUDIO_PLAYER_RUN_STOP, &error), &error);
    scope_legacy_engine_start(engine, 0, tone);
    RadioSAudioRunSnapshot replacement; radios_audio_engine_player_runner_snapshot(engine, 0, &replacement);
    RadioSAudioSnapshot replacement_audio; radios_audio_engine_snapshot(engine, &replacement_audio);
    run_for(300); g_assert_true(radios_player_scope_execution_snapshot(execution, &scope, &snapshot));
    g_assert_false(snapshot.schema_active);
    require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_STOP, &error), &error);
    scope_assert_live_pcm_unchanged(&window, engine, 0, replacement_audio.decks[0].generation, replacement.epoch, 2350);
    require_call(radios_audio_engine_player_runner_action(engine, 0, RADIOS_AUDIO_PLAYER_RUN_STOP, &error), &error);
    weekly_record_unchanged(saved); weekly_record_unchanged(future_record); assert_saved_playlist_unchanged(playlist);
    radios_audio_engine_unref(engine); json_node_free(future_config); json_node_free(future_record); json_node_free(saved);
    json_node_free(configuration); json_node_free(playlist); json_node_free(tone); window_close(&window);
}
/* Kullanıcı bildirimi "AutoDJ kanallarını seçtiğimde etki etmiyor" (kılavuz
 * §03 kümeden kümeye): a cluster playing its schema with AutoDJ plays the
 * kinds the rotation routes to a free unclustered player (the jingle on a
 * cart player) there and keeps music on its own players; a busy cart player
 * gives those rows back to the cluster instead of refusing the batch. */
static gint media_deck_wait(RadioSAudioEngine *engine, const gchar *media_id, guint seconds)
{
    gint64 until = g_get_monotonic_time() + (gint64)seconds * G_USEC_PER_SEC;
    do {
        pump(); RadioSAudioSnapshot audio; radios_audio_engine_snapshot(engine, &audio);
        for (guint deck = 0; deck < 32; ++deck)
            if (audio.decks[deck].state == RADIOS_AUDIO_PLAYING && g_str_equal(audio.decks[deck].media_id, media_id)) return (gint)deck;
        g_usleep(5000);
    } while (g_get_monotonic_time() < until);
    return -1;
}
static void test_execution_cluster_schema_routes_kinds_to_unclustered_pcm(void)
{
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "cart routing workspace baseline");
    JsonNode *music = routing_short_markers(media_import(&window, "Kümenin müziği", 700, .22, "music"), 1800);
    JsonNode *jingle = routing_short_markers(media_import(&window, "Kümesiz oynatıcının jingle'ı", 1900, .22, "jingle"), 1800);
    JsonNode *other = media_import(&window, "Kümesiz oynatıcıda ayrı çalan", 2500, .3, "music");
    gchar *music_id = g_strdup(radios_json_string(json_object_get_object_member(json_node_get_object(music), "media"), "_id", ""));
    gchar *jingle_id = g_strdup(radios_json_string(json_object_get_object_member(json_node_get_object(jingle), "media"), "_id", ""));
    JsonNode *playlist = playlist_create("Küme müziği ve kümesiz jingle", music, jingle);
    RadioSPlayerTopology topology = *radios_player_workspace_topology(window.ctx->players); gchar cluster[37]; GError *error = NULL;
    guint cart = 4;
    if (!radios_player_workspace_contains(window.ctx->players, cart)) require_call(radios_player_topology_add_player(&topology, "", &cart, &error), &error);
    g_assert_cmpuint(cart, !=, 0); g_assert_cmpuint(cart, !=, 2); g_assert_cmpstr(topology.players[cart].cluster_id, ==, "");
    gint64 minute = 60 * G_USEC_PER_SEC, start = g_get_real_time() / minute * minute;
    JsonNode *configuration = weekly_pcm_configuration(playlist, NULL, start, 2, "Küme şeması · kümesiz jingle");
    JsonObject *calendar = json_object_get_object_member(json_node_get_object(configuration), "calendar");
    JsonObject *definition = json_array_get_object_element(json_object_get_array_member(calendar, "rotation_library"), 0);
    JsonObject *source = json_array_get_object_element(json_object_get_array_member(definition, "sources"), 0);
    json_object_remove_member(source, "media_kind"); json_object_set_int_member(source, "count", 2); json_object_set_int_member(source, "interval_minutes", 0);
    JsonObject *players = json_object_new(); const gchar *kinds[] = {"music", "advertisement", "jingle", "sweeper"};
    for (guint k = 0; k < G_N_ELEMENTS(kinds); ++k) {
        JsonArray *decks = json_array_new();
        if (k == 2) json_array_add_int_element(decks, cart + 1); else { json_array_add_int_element(decks, 1); json_array_add_int_element(decks, 3); }
        json_object_set_array_member(players, kinds[k], decks);
    }
    json_object_set_object_member(json_object_get_object_member(definition, "settings"), "autodj_players", players);
    JsonNode *saved = document_save("weekly_programme", "Execution cart routing schema", json_node_get_object(configuration));
    require_call(radios_player_topology_add_cluster(&topology, "Kümeli müzik", cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 0, cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 2, cluster, &error), &error);
    require_call(radios_player_topology_set_cluster_automation(&topology, cluster, radios_json_string(json_node_get_object(saved), "_id", ""), &error), &error);
    require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER}; g_strlcpy(scope.cluster_id, cluster, sizeof scope.cluster_id);
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(window.ctx);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    for (guint busy = 0; busy < 2; ++busy) {
        RadioSAudioRunSnapshot independent = {0};
        if (busy) { scope_legacy_engine_start(engine, cart, other); radios_audio_engine_player_runner_snapshot(engine, cart, &independent); }
        ScopeExecutionResult played = {0};
        require_call(radios_player_scope_execution_play_schema(execution, &scope, RADIOS_AUTODJ_TRANSITIONS,
            execution_result, &played, execution_result_destroy, &error), &error); execution_wait(&played);
        gint music_deck = media_deck_wait(engine, music_id, 15);
        g_assert_true(music_deck == 0 || music_deck == 2);
        gint jingle_deck = media_deck_wait(engine, jingle_id, 20);
        g_test_message("busy=%u music on player %d, jingle on player %d", busy, music_deck + 1, jingle_deck + 1);
        if (!busy) {
            g_assert_cmpint(jingle_deck, ==, (gint)cart);
            goffset main_at = capture_offset("MAIN"); run_for(400);
            g_assert_cmpfloat(tone_energy("MAIN", main_at, 1900), >, .05);
        } else {
            g_assert_true(jingle_deck == 0 || jingle_deck == 2);
            RadioSAudioRunSnapshot still; radios_audio_engine_player_runner_snapshot(engine, cart, &still);
            g_assert_cmpuint(still.epoch, ==, independent.epoch); g_assert_cmpint(still.state, ==, RADIOS_AUDIO_RUN_PLAYING);
        }
        require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_STOP, &error), &error); run_for(200);
        if (busy) require_call(radios_audio_engine_player_runner_action(engine, cart, RADIOS_AUDIO_PLAYER_RUN_STOP, &error), &error);
    }
    weekly_record_unchanged(saved);
    radios_audio_engine_unref(engine); json_node_free(saved); json_node_free(configuration); json_node_free(playlist);
    json_node_free(music); json_node_free(jingle); json_node_free(other); g_free(music_id); g_free(jingle_id); window_close(&window);
}
/* User report 18.10 "hâlâ kümesizlerde, ama o program kümelilere ayarlı":
 * the broadcast page's continuous general queue runs the weekly programme a
 * player cluster carries on that cluster's players (3 and 4), never on the
 * general pair 1 and 2: first with no per-kind channels in its rotation (the
 * cluster is the whole pool), then with the jingle pool on an unclustered
 * cart player (5). */
static void test_general_assigned_cluster_programme_on_cluster_pcm(void)
{
    Window window = window_open(); navigate(&window, "broadcast"); RadioSPlayerWeekly *driver = general_driver(&window);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(window.root, "programme-general-continuous"))));
    wait_until(scope_workspace_ready, window.ctx->players, "general cluster workspace baseline");
    JsonNode *music = routing_short_markers(media_import(&window, "Atanmış kümenin müziği", 700, .22, "music"), 1800);
    JsonNode *jingle = routing_short_markers(media_import(&window, "Atanmış programın jingle'ı", 1900, .22, "jingle"), 1800);
    gchar *music_id = g_strdup(radios_json_string(json_object_get_object_member(json_node_get_object(music), "media"), "_id", ""));
    gchar *jingle_id = g_strdup(radios_json_string(json_object_get_object_member(json_node_get_object(jingle), "media"), "_id", ""));
    JsonNode *playlist = playlist_create("Atanmış küme müziği ve jingle", music, jingle);
    RadioSPlayerTopology topology = *radios_player_workspace_topology(window.ctx->players); gchar cluster[37]; GError *error = NULL;
    for (guint deck = 0; deck < 5; ++deck)
        if (!radios_player_workspace_contains(window.ctx->players, deck)) { guint added = deck; require_call(radios_player_topology_add_player(&topology, "", &added, &error), &error); }
    gint64 start = g_get_real_time() / G_TIME_SPAN_DAY * G_TIME_SPAN_DAY;
    JsonNode *configuration = weekly_pcm_configuration(playlist, NULL, start, 1440, "Kümeye atanmış genel program");
    JsonObject *calendar = json_object_get_object_member(json_node_get_object(configuration), "calendar");
    JsonObject *definition = json_array_get_object_element(json_object_get_array_member(calendar, "rotation_library"), 0);
    json_object_set_string_member(definition, "name", "Kümeye atanmış genel program");
    JsonObject *source = json_array_get_object_element(json_object_get_array_member(definition, "sources"), 0);
    json_object_remove_member(source, "media_kind"); json_object_set_int_member(source, "count", 2); json_object_set_int_member(source, "interval_minutes", 0);
    JsonNode *saved = document_save("weekly_programme", "Kümeye atanmış genel program", json_node_get_object(configuration));
    require_call(radios_player_topology_add_cluster(&topology, "Atanmış küme", cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 2, cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 3, cluster, &error), &error);
    /* As on the station: the cart player itself is left on Yarı AutoDJ. */
    require_call(radios_player_topology_set_autodj(&topology, 4, RADIOS_AUTODJ_SEMI, -1, &error), &error);
    require_call(radios_player_topology_set_cluster_automation(&topology, cluster, radios_json_string(json_node_get_object(saved), "_id", ""), &error), &error);
    require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
    radios_player_weekly_general_refresh(driver);
    GeneralReady ready = {&window, driver, "Kümeye atanmış genel program", start};
    wait_until(general_current_mixed_ready, &ready, "the assigned programme's active slot is the general queue's");
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    for (guint carts = 0; carts < 2; ++carts) {
        if (carts) {
            /* The rotation's own per-kind channels: music on the cluster, jingles on cart player 5. */
            JsonObject *players = json_object_new(); const gchar *kinds[] = {"music", "advertisement", "jingle", "sweeper"};
            for (guint k = 0; k < G_N_ELEMENTS(kinds); ++k) {
                JsonArray *decks = json_array_new();
                if (k == 2) json_array_add_int_element(decks, 5); else { json_array_add_int_element(decks, 3); json_array_add_int_element(decks, 4); }
                json_object_set_array_member(players, kinds[k], decks);
            }
            json_object_set_object_member(json_object_get_object_member(definition, "settings"), "autodj_players", players);
            JsonNode *updated = document_save("weekly_programme", "Kümeye atanmış genel program", json_node_get_object(configuration)); json_node_free(saved); saved = updated;
            radios_player_weekly_general_refresh(driver);
            wait_until(general_current_mixed_ready, &ready, "the updated programme is the general queue's again");
        }
        click(window.root, "header-play");
        gint music_deck = media_deck_wait(engine, music_id, 20);
        g_test_message("general queue (carts=%u): music on player %d", carts, music_deck + 1);
        g_assert_true(music_deck == 2 || music_deck == 3);
        g_assert_true(weekly_snapshot(driver).armed);
        gint jingle_deck = media_deck_wait(engine, jingle_id, 25);
        g_test_message("general queue (carts=%u): jingle on player %d", carts, jingle_deck + 1);
        if (carts) g_assert_cmpint(jingle_deck, ==, 4); else g_assert_true(jingle_deck == 2 || jingle_deck == 3);
        goffset main_at = capture_offset("MAIN"); run_for(400);
        g_assert_cmpfloat(tone_energy("MAIN", main_at, 1900), >, .05);
        click(window.root, "header-stop"); run_for(300);
        g_assert_false(weekly_snapshot(driver).armed); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    }
    weekly_record_unchanged(saved);
    radios_audio_engine_unref(engine); json_node_free(saved); json_node_free(configuration); json_node_free(playlist);
    json_node_free(music); json_node_free(jingle); g_free(music_id); g_free(jingle_id); window_close(&window);
}
/* 18.10: the general queue and the cluster's own flow never play one
 * assigned programme at the same time. Whichever starts second is refused
 * with a Turkish reason; the first keeps its players and its PCM. On the
 * general path the cluster's AutoDJ shape holds as in its own flow: two
 * members alternate their songs, a single member plays them in turn. */
static gboolean deck_media_playing(RadioSAudioEngine *engine, guint deck, const gchar *a, const gchar *b)
{
    RadioSAudioSnapshot audio; radios_audio_engine_snapshot(engine, &audio);
    return audio.decks[deck].state == RADIOS_AUDIO_PLAYING && (g_str_equal(audio.decks[deck].media_id, a) || g_str_equal(audio.decks[deck].media_id, b));
}
static guint32 decks_playing(RadioSAudioEngine *engine, const gchar *a, const gchar *b)
{
    guint32 mask = 0;
    for (guint deck = 0; deck < 32; ++deck) if (deck_media_playing(engine, deck, a, b)) mask |= (guint32)1 << deck;
    return mask;
}
static void test_general_and_cluster_flow_never_double_pcm(void)
{
    Window window = window_open(); navigate(&window, "broadcast"); RadioSPlayerWeekly *driver = general_driver(&window);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(window.root, "programme-general-continuous"))));
    wait_until(scope_workspace_ready, window.ctx->players, "double start workspace baseline");
    JsonNode *a = routing_short_markers(media_import(&window, "Çakışmasız küme A", 950, .22, "music"), 1800);
    JsonNode *b = routing_short_markers(media_import(&window, "Çakışmasız küme B", 2350, .22, "music"), 1800);
    gchar *a_id = g_strdup(radios_json_string(json_object_get_object_member(json_node_get_object(a), "media"), "_id", ""));
    gchar *b_id = g_strdup(radios_json_string(json_object_get_object_member(json_node_get_object(b), "media"), "_id", ""));
    JsonNode *playlist = playlist_create("Çakışmasız küme kaynağı", a, b);
    RadioSPlayerTopology topology = *radios_player_workspace_topology(window.ctx->players); gchar cluster[37]; GError *error = NULL;
    for (guint deck = 0; deck < 4; ++deck)
        if (!radios_player_workspace_contains(window.ctx->players, deck)) { guint added = deck; require_call(radios_player_topology_add_player(&topology, "", &added, &error), &error); }
    gint64 start = g_get_real_time() / G_TIME_SPAN_DAY * G_TIME_SPAN_DAY;
    JsonNode *configuration = weekly_pcm_configuration(playlist, NULL, start, 1440, "Çakışmasız küme programı");
    JsonObject *calendar = json_object_get_object_member(json_node_get_object(configuration), "calendar");
    JsonObject *definition = json_array_get_object_element(json_object_get_array_member(calendar, "rotation_library"), 0);
    json_object_set_string_member(definition, "name", "Çakışmasız küme programı");
    JsonObject *source = json_array_get_object_element(json_object_get_array_member(definition, "sources"), 0);
    json_object_remove_member(source, "media_kind"); json_object_set_int_member(source, "count", 2); json_object_set_int_member(source, "interval_minutes", 0);
    JsonNode *saved = document_save("weekly_programme", "Çakışmasız küme programı", json_node_get_object(configuration));
    require_call(radios_player_topology_add_cluster(&topology, "Çakışmasız küme", cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 2, cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 3, cluster, &error), &error);
    require_call(radios_player_topology_set_cluster_automation(&topology, cluster, radios_json_string(json_node_get_object(saved), "_id", ""), &error), &error);
    require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
    radios_player_weekly_general_refresh(driver);
    GeneralReady ready = {&window, driver, "Çakışmasız küme programı", start};
    wait_until(general_current_mixed_ready, &ready, "the assigned programme's slot is the general queue's");
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(window.ctx);
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER}; g_strlcpy(scope.cluster_id, cluster, sizeof scope.cluster_id);
    const guint32 members = (1u << 2) | (1u << 3);
    /* 1. The general queue first: its two songs alternate the cluster's two
     * players. The cluster's own flow is then refused at once, whatever the
     * general runner happens to be loading, and the general PCM goes on. */
    click(window.root, "header-play");
    gint first = media_deck_wait(engine, a_id, 20); g_assert_true(first == 2 || first == 3);
    gint second = media_deck_wait(engine, b_id, 20); g_assert_true(second == 2 || second == 3); g_assert_cmpint(second, !=, first);
    g_test_message("general: A on player %d, B on player %d", first + 1, second + 1);
    g_assert_true(weekly_snapshot(driver).armed);
    ScopeExecutionResult refused = {0};
    if (radios_player_scope_execution_play_schema(execution, &scope, RADIOS_AUTODJ_TRANSITIONS, execution_result, &refused, execution_result_destroy, &error))
        wait_until(execution_completed, &refused, "cluster flow answer while the general queue plays");
    else { refused.error = error; error = NULL; }
    g_assert_nonnull(refused.error); g_test_message("cluster flow refused: %s", refused.error->message);
    g_assert_nonnull(strstr(refused.error->message, "genel yayın sırasında çalıyor")); g_clear_error(&refused.error);
    RadioSPlayerScopeExecutionSnapshot cluster_state = {0};
    g_assert_true(!radios_player_scope_execution_snapshot(execution, &scope, &cluster_state) || !cluster_state.schema_active);
    for (guint i = 0; i < 3; ++i) {
        run_for(1000); guint32 playing = decks_playing(engine, a_id, b_id);
        g_assert_true(weekly_snapshot(driver).armed); g_assert_cmpuint(playing & ~members, ==, 0); g_assert_cmpuint(__builtin_popcount(playing), <=, 2);
    }
    goffset main_at = capture_offset("MAIN"); run_for(300);
    g_assert_cmpfloat(MAX(tone_energy("MAIN", main_at, 950), tone_energy("MAIN", main_at, 2350)), >, .05);
    click(window.root, "header-stop"); run_for(400);
    g_assert_false(weekly_snapshot(driver).armed); g_assert_cmpuint(decks_playing(engine, a_id, b_id), ==, 0);
    /* 2. The cluster's own flow first: starting the general queue then is
     * refused by the players' ownership and the cluster's PCM goes on. */
    ScopeExecutionResult played = {0};
    require_call(radios_player_scope_execution_play_schema(execution, &scope, RADIOS_AUTODJ_TRANSITIONS, execution_result, &played,
        execution_result_destroy, &error), &error); execution_wait(&played);
    gint scoped = media_deck_wait(engine, a_id, 20); g_assert_true(scoped == 2 || scoped == 3);
    RadioSAudioRunSnapshot owner = {0}; radios_audio_engine_player_runner_snapshot(engine, (guint)played.source, &owner);
    if (radios_player_weekly_start_general(driver, &error)) {
        RadioSPlayerWeeklySnapshot refusal = {0}; gint64 until = g_get_monotonic_time() + 15 * G_USEC_PER_SEC;
        do { run_for(100); refusal = weekly_snapshot(driver); } while (!refusal.error[0] && g_get_monotonic_time() < until);
        g_test_message("general queue refused: %s", refusal.error);
        g_assert_nonnull(strstr(refusal.error, "kendi playlist sırasını çalıyor")); g_assert_false(refusal.armed);
    } else { g_assert_nonnull(error); g_test_message("general queue refused: %s", error->message); g_clear_error(&error); }
    RadioSAudioRunSnapshot still = {0}; radios_audio_engine_player_runner_snapshot(engine, (guint)played.source, &still);
    g_assert_cmpuint(still.epoch, ==, owner.epoch); g_assert_cmpuint(decks_playing(engine, a_id, b_id) & ~members, ==, 0);
    main_at = capture_offset("MAIN"); run_for(300);
    g_assert_cmpfloat(MAX(tone_energy("MAIN", main_at, 950), tone_energy("MAIN", main_at, 2350)), >, .05);
    require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_STOP, &error), &error); run_for(400);
    g_assert_cmpuint(decks_playing(engine, a_id, b_id), ==, 0);
    /* 3. A single-member cluster: the general queue plays both songs in turn
     * on that one player (the cluster flow's Yarı AutoDJ shape). */
    topology = *radios_player_workspace_topology(window.ctx->players);
    require_call(radios_player_topology_set_cluster(&topology, 3, "", &error), &error);
    require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
    radios_player_weekly_general_refresh(driver);
    wait_until(general_current_mixed_ready, &ready, "the single-member cluster's programme is the general queue's");
    click(window.root, "header-play");
    g_assert_cmpint(media_deck_wait(engine, a_id, 20), ==, 2); g_assert_cmpint(media_deck_wait(engine, b_id, 20), ==, 2);
    click(window.root, "header-stop"); run_for(400);
    g_assert_false(weekly_snapshot(driver).armed);
    weekly_record_unchanged(saved);
    radios_audio_engine_unref(engine); json_node_free(saved); json_node_free(configuration); json_node_free(playlist);
    json_node_free(a); json_node_free(b); g_free(a_id); g_free(b_id); window_close(&window);
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]); for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1]; --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    main_sink = g_getenv("RADIOS_TEST_MAIN_SINK"); cue_sink = g_getenv("RADIOS_TEST_CUE_SINK"); monitor_sink = g_getenv("RADIOS_TEST_MONITOR_SINK");
    g_assert_nonnull(main_sink); g_assert_nonnull(cue_sink); g_assert_nonnull(monitor_sink);
    g_assert_true(g_str_has_prefix(main_sink, "radios_gui_")); g_assert_true(g_str_has_prefix(cue_sink, "radios_gui_"));
    GError *error = NULL; g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    application = gtk_application_new("org.radios.NativePlayoutAcceptance", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/native-playout/weekly-programme-future-countdown-due-weighted-repeat-PCM", test_weekly_programme_future_due_repeat_weighted_pcm);
    g_test_add_func("/native-playout/weekly-programme-stop-pending-prepare-foreign-PCM", test_weekly_programme_stop_pending_prepare_preserves_foreign_pcm);
    g_test_add_func("/native-playout/player-output-unconnected-constructor-custom-channel-reopen-PCM", test_player_output_unconnected_constructor_custom_channel_reopen_pcm);
    g_test_add_func("/native-playout/verified-queue-auto-next-crossfade-PCM", test_queue_pcm);
    g_test_add_func("/native-playout/transport-busy-deck-CUE-isolation", test_transport_busy);
    g_test_add_func("/native-playout/general-unavailable-revision-fences", test_general_unavailable_and_revision_fences);
    g_test_add_func("/native-playout/general-header-play-local-PCM", test_general_header_play_local_pcm);
    g_test_add_func("/native-playout/header-selected-weekly-zero-overlap-filter-PCM", test_header_selected_weekly_zero_overlap_filter_pcm);
    g_test_add_func("/native-playout/general-future-refill-adjacent-reconnect-PCM", test_general_future_refill_adjacent_reconnect_PCM);
    g_test_add_func("/native-playout/stale-solo-ordinary-playlist-AutoDJ-pair-transport-PCM", test_stale_solo_playlist_AutoDJ_pair_transport_PCM);
    g_test_add_func("/native-playout/ordinary-AutoDJ-pending-target-preserves-source-MAIN", test_ordinary_AutoDJ_pending_target_fence);
    g_test_add_func("/native-playout/ordinary-AutoDJ-live-short-tail-MAIN-PCM", test_ordinary_AutoDJ_live_short_tail_PCM);
    g_test_add_func("/native-playout/AutoDJ-short-IN-OUT-three-media-MAIN-PCM", test_autodj_short_span_three_media_MAIN_PCM);
    g_test_add_func("/native-playout/AutoDJ-interactive-marker-save-playlist-pair-roundtrip-MAIN-PCM", test_interactive_autodj_markers_saved_playlist_pair_roundtrip_MAIN_PCM);
    g_test_add_func("/native-playout/AutoDJ-tempo-return-ramp-pause-live-UI-MAIN-PCM", test_autodj_tempo_return_ramp_pause_live_UI_MAIN_PCM);
    g_test_add_func("/native-playout/AutoDJ-slider-header-seek-preserves-preload-PCM", test_autodj_slider_and_header_seek_preserve_pcm);
    g_test_add_func("/native-playout/continuous-AutoDJ-transient-extension-retry-PCM", test_continuous_autodj_transient_retry);
    g_test_add_func("/native-playout/continuous-AutoDJ-short-IN-OUT-append-PCM", test_continuous_short_autodj_append_PCM);
    g_test_add_func("/native-playout/automation-preview-history-reopen", test_automation_preview_history);
    g_test_add_func("/native-playout/autodj-deck-policy-save-reopen-PCM-append", test_autodj_deck_transition_save_playback);
    g_test_add_func("/native-playout/future-schedule-cancel-native-stop", test_schedule_cancel);
    g_test_add_func("/native-playout/named-calendar-preview-schedule-transition-boundaries", test_named_calendar_preview_schedule_transition_boundaries);
    g_test_add_func("/native-playout/validation-owner-close-CAS-preservation", test_validation_lifecycle_cas);
    g_test_add_func("/native-playout/saved-playlist-card-drag-loader-PCM", test_saved_playlist_card_drag_and_loader_pcm);
    g_test_add_func("/native-playout/independent-player-native-PCM-pause-next-stop-history", test_independent_player_native_pcm);
    g_test_add_func("/native-playout/player-card-manual-metadata-generation-identity", test_player_manual_metadata_identity);
    g_test_add_func("/native-playout/live-player-widget-destroy-keeps-PCM", test_live_player_widget_destroy_keeps_pcm);
    g_test_add_func("/native-playout/workspace-rejected-scope-close-preserves-legacy-PCM", test_workspace_rejected_scope_close_preserves_legacy_pcm);
    g_test_add_func("/native-playout/stale-scope-cannot-control-replacement-PCM", test_stale_scope_controller_cannot_control_replacement_pcm);
    g_test_add_func("/native-playout/workspace-two-clusters-transition-pause-independent-PCM", test_workspace_two_clusters_transition_pause_independent_pcm);
    g_test_add_func("/native-playout/execution-cluster-aggregate-hidden-pair-preserves-other-PCM", test_execution_cluster_and_aggregate_hidden_pair_preserve_other_pcm);
    g_test_add_func("/native-playout/execution-assigned-schema-refill-future-wait-cancel-PCM", test_execution_schema_current_refills_and_future_wait_cancels_silently);
    g_test_add_func("/native-playout/execution-cluster-schema-routes-kinds-to-unclustered-PCM", test_execution_cluster_schema_routes_kinds_to_unclustered_pcm);
    g_test_add_func("/native-playout/general-assigned-cluster-programme-on-cluster-PCM", test_general_assigned_cluster_programme_on_cluster_pcm);
    g_test_add_func("/native-playout/general-and-cluster-flow-never-double-PCM", test_general_and_cluster_flow_never_double_pcm);
    g_test_add_func("/native-playout/player-waveform-cues-square-automatic-analysis", test_player_waveform_cues_square_automatic_analysis);
    g_test_add_func("/native-playout/player-waveform-zoom-pan-inspection-readonly", test_player_waveform_zoom_pan_inspection_no_transport_or_db_write);
    g_test_add_func("/native-playout/named-rotation-picker-AutoDJ-gates-pair-PCM", test_named_rotation_picker_autodj_gate_pair_pcm);
    g_test_add_func("/native-playout/playlist-context-AutoDJ-role-persistence-multi-PCM", test_playlist_context_autodj_routes_persistence_multi_pcm);
    g_test_add_func("/native-playout/named-rotation-local-beatmatch-effect-PCM", test_named_rotation_local_beatmatch_effect_pcm);
    g_test_add_func("/native-playout/player-SYNC-analysis-persistence-chosen-pair-PCM", test_player_sync_and_pair_autodj);
    g_test_add_func("/native-playout/player-SYNC-unknown-and-late-target-preserve-PCM", test_player_sync_rejects_unknown_and_stale_target);
    g_test_add_func("/native-playout/named-rotation-weighted-atomic-disabled-PCM", test_named_rotation_weighted_atomic_disabled_pcm);
    gint result = g_test_run(); radios_data_client_unref(reader); g_object_unref(application); radios_config_clear(&config); return result;
}
