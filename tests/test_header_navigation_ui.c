#define _POSIX_C_SOURCE 200809L
#include "radios/window.h"
#include "radios/audio_ui.h"
#include "radios/audio_runner.h"
#include "radios/header_navigation.h"
#include "radios/media_jobs.h"
#include "radios/playout_ui.h"
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

/* run_audio_ui.py supplies the entire private project, authenticated DB,
 * SHA-bound synthetic media and owned Pulse outputs. No production state. */
static RadioSConfig config;
static GtkApplication *application;
static RadioSDataClient *reader;
static const gchar *main_sink, *cue_sink;
typedef struct { RadioSWindow *ui; RadioSRuntime *runtime; RadioSViewContext *ctx; GtkWidget *root; } Window;
typedef gboolean (*Condition)(gpointer);

static void pump(void)
{ while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void wait_until(Condition condition, gpointer data, const gchar *description)
{
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!condition(data) && g_get_monotonic_time() < deadline) pump();
    if (!condition(data)) g_error("Header navigation acceptance timed out: %s", description);
}
static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (!g_strcmp0(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *with_class(GtkWidget *root, const gchar *name)
{
    if (gtk_style_context_has_class(gtk_widget_get_style_context(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = with_class(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *dialog_find(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *result = NULL;
    for (GList *item = windows; item && !result; item = item->next)
        if (!g_strcmp0(gtk_widget_get_name(item->data), name)) result = item->data;
    g_list_free(windows); return result;
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
        radios_audio_ui_ready(window->ctx, RADIOS_AUDIO_UI_MAIN);
}
static Window window_open(void)
{
    Window window = {0}; GError *error = NULL; window.runtime = radios_runtime_new(64, 128);
    window.ui = radios_window_new(application, &config, window.runtime, FALSE, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(window.ui);
    window.ctx = radios_window_view_context(window.ui); window.root = GTK_WIDGET(window.ctx->parent);
    radios_window_present(window.ui); wait_until(window_ready, &window, "private DB, saved outputs and documents ready");
    return window;
}
static void window_close(Window *window)
{
    radios_window_free(window->ui); radios_runtime_free(window->runtime);
    memset(window, 0, sizeof *window); while (g_main_context_iteration(NULL, FALSE)) {}
}
static gboolean widget_enabled(gpointer widget) { return gtk_widget_is_sensitive(widget); }
static gboolean widget_disabled(gpointer widget) { return !gtk_widget_is_sensitive(widget); }
static void click(Window *window, const gchar *name)
{
    GtkWidget *widget = find(window->root, name); g_assert_true(GTK_IS_BUTTON(widget));
    wait_until(widget_enabled, widget, "header action enabled by real selected flow status");
    gtk_button_clicked(GTK_BUTTON(widget));
}
static gboolean profile_emergency(gpointer root)
{ GtkWidget *page = with_class(root, "emergency-view"); return page && gtk_widget_get_mapped(page); }
static gboolean database_not_connected(gpointer widget)
{ return strstr(gtk_label_get_text(GTK_LABEL(widget)), "Bağlı değil") != NULL; }
static void capture_private_window(Window *window, gint requested_width, gint requested_height)
{
    const gchar *directory = g_getenv("RADIOS_PROMPT5_CAPTURE_DIR"); if (!directory || !*directory) return;
    gtk_window_unmaximize(GTK_WINDOW(window->root)); gtk_window_resize(GTK_WINDOW(window->root), requested_width, requested_height);
    gint64 deadline = g_get_monotonic_time() + 300 * 1000;
    while (g_get_monotonic_time() < deadline) pump();
    gint width = gtk_widget_get_allocated_width(window->root), height = gtk_widget_get_allocated_height(window->root);
    g_test_message("Private capture requested %dx%d; actual GTK allocation %dx%d", requested_width, requested_height, width, height);
    g_assert_cmpint(width, >, 0); g_assert_cmpint(height, >, 0); g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
    const gchar *visible[] = {"header-navigation", "header-behavior-text", "header-database-status", "header-backup-details", "header-diagnostic-details", "footer-warning-status"};
    for (guint i = 0; i < G_N_ELEMENTS(visible); ++i) {
        GtkWidget *widget = find(window->root, visible[i]); gint x = 0, y = 0;
        g_assert_nonnull(widget); g_assert_true(gtk_widget_get_mapped(widget));
        g_assert_true(gtk_widget_translate_coordinates(widget, window->root, 0, 0, &x, &y));
        g_assert_cmpint(x, >=, 0); g_assert_cmpint(y, >=, 0);
        g_assert_cmpint(x + gtk_widget_get_allocated_width(widget), <=, width);
        g_assert_cmpint(y + gtk_widget_get_allocated_height(widget), <=, height);
    }
    GdkPixbuf *picture = gdk_pixbuf_get_from_window(gtk_widget_get_window(window->root), 0, 0, width, height);
    g_assert_nonnull(picture);
    gchar *filename = g_strdup_printf("prompt5-footer-%d.png", requested_width), *path = g_build_filename(directory, filename, NULL);
    GError *error = NULL; g_assert_true(gdk_pixbuf_save(picture, path, "png", &error, NULL)); g_assert_no_error(error);
    g_object_unref(picture); g_free(path); g_free(filename);
}

static void test_footer_relocation(void)
{
    Window window = window_open();
    GtkWidget *header = with_class(window.root, "page-header"), *footer = with_class(window.root, "status-footer");
    g_assert_nonnull(header); g_assert_nonnull(footer);
    g_assert_null(with_class(header, "header-behavior"));
    GtkWidget *navigation = find(header, "header-navigation"); g_assert_nonnull(navigation);
    const gchar *controls[] = {"header-previous", "header-play", "header-pause", "header-stop", "header-next-track", "header-next-playlist", "header-schema-load"};
    for (guint i = 0; i < G_N_ELEMENTS(controls); ++i) g_assert_true(GTK_IS_BUTTON(find(navigation, controls[i])));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(find(navigation, "header-schema-load"))), ==, "Şema yükle…");
    const gchar *status[] = {"header-behavior-text", "header-database-status", "header-backup-details", "header-diagnostic-details", "footer-warning-status"};
    for (guint i = 0; i < G_N_ELEMENTS(status); ++i) {
        GtkWidget *widget = find(footer, status[i]); g_assert_nonnull(widget);
        g_assert_null(find(header, status[i])); g_assert_true(gtk_widget_get_mapped(widget));
    }
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(footer, "header-database-status"))), "Bağlı"));
    g_assert_nonnull(find(footer, "footer-station")); g_assert_nonnull(find(footer, "footer-updated"));
    wait_until(widget_disabled, find(navigation, "header-next-track"), "idle next track disabled");
    wait_until(widget_disabled, find(navigation, "header-next-playlist"), "idle next playlist disabled");
    capture_private_window(&window, 1800, 1000); capture_private_window(&window, 1000, 680);
    click(&window, "header-diagnostic-details"); GtkWidget *dialog = dialog_find("diagnostic-details-dialog");
    g_assert_true(GTK_IS_DIALOG(dialog)); g_assert_nonnull(find(dialog, "diagnostic-open-support"));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
    g_assert_null(dialog_find("diagnostic-details-dialog"));
    click(&window, "header-backup-details"); wait_until(profile_emergency, window.root, "relocated backup retains emergency profile navigation");
    radios_data_close(window.ctx->data);
    wait_until(database_not_connected, find(footer, "header-database-status"), "footer reflects a disconnected DB");
    window_close(&window);
}

typedef struct { const gchar *path; JsonNode *body, *result; GError *error; gint done; } Request;
static gpointer request_work(gpointer value)
{
    Request *job = value; job->result = radios_data_client_request_sync(reader, "POST", job->path, job->body, NULL, &job->error);
    g_atomic_int_set(&job->done, 1); return NULL;
}
static gboolean request_done(gpointer value) { return g_atomic_int_get(&((Request *)value)->done); }
static JsonNode *request(const gchar *path, JsonNode *body)
{
    Request job = {.path = path, .body = body}; GThread *thread = g_thread_new("prompt5-private-HTTP", request_work, &job);
    wait_until(request_done, &job, "private authenticated HTTP response"); g_thread_join(thread);
    if (job.error) g_test_message("Private fixture request %s failed: %s", path, job.error->message);
    g_assert_no_error(job.error); g_assert_nonnull(job.result); return job.result;
}
static JsonNode *body_new(void)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, json_object_new());
    json_object_set_string_member(json_node_get_object(node), "station_id", radios_data_client_station_id(reader)); return node;
}
static JsonNode *clone(JsonNode *node)
{ gchar *text = json_to_string(node, FALSE); JsonNode *result = json_from_string(text, NULL); g_free(text); g_assert_nonnull(result); return result; }
static JsonNode *programme_record(const gchar *name)
{
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "kind", "programme");
    JsonNode *result = request("/v1/workflows/desktop/list", body); json_node_free(body); JsonNode *found = NULL;
    JsonArray *records = json_object_get_array_member(json_node_get_object(result), "records");
    for (guint i = 0; i < json_array_get_length(records); ++i) {
        JsonObject *row = json_array_get_object_element(records, i);
        if (!g_strcmp0(radios_json_string(row, "name", ""), name)) found = clone(json_array_get_element(records, i));
    }
    json_node_free(result); return found;
}
static void save_programme(const gchar *name, JsonNode *configuration)
{
    JsonNode *existing = programme_record(name), *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", "programme"); json_object_set_string_member(object, "name", name);
    json_object_set_member(object, "config", clone(configuration));
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    if (existing) {
        json_object_set_string_member(object, "document_id", radios_json_string(json_node_get_object(existing), "_id", ""));
        json_object_set_int_member(object, "expected_revision", radios_json_int(json_node_get_object(existing), "_rev", 0));
    }
    JsonNode *saved = request("/v1/workflows/desktop/put", body);
    json_node_free(saved); json_node_free(body); g_clear_pointer(&existing, json_node_free);
}
static void write_u16(guchar *bytes, guint16 value) { value = GUINT16_TO_LE(value); memcpy(bytes, &value, sizeof value); }
static void write_u32(guchar *bytes, guint32 value) { value = GUINT32_TO_LE(value); memcpy(bytes, &value, sizeof value); }
typedef struct { JsonNode *result; GError *error; guint callbacks; gboolean done; } Import;
static void import_complete(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; (void)owner; Import *output = value; ++output->callbacks;
    if (result) output->result = clone(result);
    if (error) output->error = g_error_copy(error);
}
static void import_destroy(gpointer value) { ((Import *)value)->done = TRUE; }
static gboolean import_done(gpointer value) { return ((Import *)value)->done; }
static JsonNode *import_media(Window *window, const gchar *title, gdouble frequency)
{
    const guint frames = 48000 * 8 + 31, bytes = frames * 4; guchar *wav = g_malloc(44 + bytes);
    memcpy(wav, "RIFF", 4); write_u32(wav + 4, 36 + bytes); memcpy(wav + 8, "WAVEfmt ", 8); write_u32(wav + 16, 16);
    write_u16(wav + 20, 1); write_u16(wav + 22, 2); write_u32(wav + 24, 48000); write_u32(wav + 28, 48000 * 4);
    write_u16(wav + 32, 4); write_u16(wav + 34, 16); memcpy(wav + 36, "data", 4); write_u32(wav + 40, bytes);
    for (guint i = 0; i < frames; ++i) {
        gint16 sample = (gint16)lrint(sin(2 * G_PI * frequency * i / 48000) * 0.2 * 32767);
        write_u16(wav + 44 + i * 4, (guint16)sample); write_u16(wav + 46 + i * 4, (guint16)sample);
    }
    gchar *id = g_uuid_string_random(), *filename = g_strconcat(id, ".wav", NULL), *path = g_build_filename(config.project_root, filename, NULL);
    g_assert_true(g_file_set_contents(path, (const gchar *)wav, 44 + bytes, NULL)); g_free(wav); g_free(id); g_free(filename);
    Import output = {0}; GCancellable *handle = radios_media_import_async(window->ctx, window->root, path,
        "music", title, import_complete, &output, import_destroy);
    g_assert_nonnull(handle); wait_until(import_done, &output, "real SHA-bound synthetic WAV import");
    g_assert_no_error(output.error); g_assert_cmpuint(output.callbacks, ==, 1); g_assert_nonnull(output.result);
    JsonObject *media = json_object_get_object_member(json_node_get_object(output.result), "media");
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "media_id", radios_json_string(media, "_id", ""));
    JsonNode *detail = request("/v1/workflows/desktop-media/get", body);
    JsonObject *version = json_object_get_object_member(json_node_get_object(detail), "version");
    g_assert_true(json_object_get_boolean_member(version, "verified")); g_assert_true(json_object_get_boolean_member(version, "object_available"));
    json_node_free(body); json_node_free(output.result); g_object_unref(handle); g_assert_cmpint(g_unlink(path), ==, 0); g_free(path); return detail;
}
static JsonNode *programme_config(JsonNode *first, JsonNode *second, guint player)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new(); json_node_take_object(node, object);
    GDateTime *now = g_date_time_new_now_utc(); gchar *date = g_date_time_format(now, "%Y-%m-%d"); g_date_time_unref(now);
    json_object_set_string_member(object, "title", "Prompt5 iki playlist"); json_object_set_string_member(object, "date", date); g_free(date);
    json_object_set_string_member(object, "note", "Private immutable block-boundary navigation acceptance");
    JsonArray *events = json_array_new(), *blocks = json_array_new(); gchar *ids[3];
    const gchar *titles[] = {"Playlist A", "Boş playlist", "Playlist B"};
    for (guint i = 0; i < 3; ++i) {
        ids[i] = g_uuid_string_random(); JsonObject *block = json_object_new();
        json_object_set_string_member(block, "id", ids[i]); json_object_set_string_member(block, "kind", "music");
        json_object_set_string_member(block, "title", titles[i]); json_object_set_string_member(block, "start_time", "");
        json_object_set_string_member(block, "note", ""); json_array_add_object_element(blocks, block);
    }
    JsonNode *details[] = {first, first, second, second};
    for (guint i = 0; i < G_N_ELEMENTS(details); ++i) {
        JsonObject *detail = json_node_get_object(details[i]), *media = json_object_get_object_member(detail, "media");
        JsonObject *version = json_object_get_object_member(detail, "version"), *event = json_object_new(); gchar *id = g_uuid_string_random();
        json_object_set_string_member(event, "id", id); g_free(id); json_object_set_string_member(event, "block_id", ids[i < 2 ? 0 : 2]);
        const gchar *keys[] = {"title", "artist", "album", "kind"};
        for (guint j = 0; j < G_N_ELEMENTS(keys); ++j) json_object_set_string_member(event, keys[j], radios_json_string(media, keys[j], ""));
        json_object_set_int_member(event, "duration_ms", radios_json_int(version, "duration_ms", 0));
        const gchar *empty[] = {"start_time", "version_label", "primary_file", "backup_file", "note"};
        for (guint j = 0; j < G_N_ELEMENTS(empty); ++j) json_object_set_string_member(event, empty[j], "");
        json_object_set_string_member(event, "media_id", radios_json_string(media, "_id", ""));
        json_object_set_string_member(event, "version_id", radios_json_string(version, "_id", ""));
        json_object_set_string_member(event, "media_file_id", radios_json_string(version, "media_file_id", ""));
        json_object_set_string_member(event, "media_sha256", radios_json_string(version, "sha256", ""));
        json_array_add_object_element(events, event);
    }
    for (guint i = 0; i < 3; ++i) g_free(ids[i]);
    json_object_set_array_member(object, "events", events); json_object_set_array_member(object, "blocks", blocks);
    if (player) {
        json_object_set_int_member(object, "queue_player", player); json_object_set_int_member(object, "queue_transition", 0);
        json_object_set_double_member(object, "queue_overlap", 0);
    }
    return node;
}

typedef struct { Window *window; RadioSPlayoutState state; guint index; } RunCondition;
static gboolean run_matches(gpointer value)
{
    RunCondition *expected = value; RadioSPlayoutSnapshot snapshot = {0};
    if (!radios_playout_ui_get_snapshot(expected->window->ctx, &snapshot) ||
        snapshot.state != expected->state || snapshot.index != expected->index) return FALSE;
    if (expected->state == RADIOS_PLAYOUT_PLAYING || expected->state == RADIOS_PLAYOUT_PAUSED ||
        expected->state == RADIOS_PLAYOUT_SCHEDULED) {
        RadioSAudioRunSnapshot actual = {0};
        RadioSAudioRunState state = expected->state == RADIOS_PLAYOUT_PLAYING ? RADIOS_AUDIO_RUN_PLAYING :
            expected->state == RADIOS_PLAYOUT_PAUSED ? RADIOS_AUDIO_RUN_PAUSED : RADIOS_AUDIO_RUN_SCHEDULED;
        return radios_playout_ui_navigation_snapshot(expected->window->ctx, &actual) &&
            actual.state == state && actual.index == expected->index;
    }
    return TRUE;
}
static gboolean actual_pause_ready(gpointer value)
{
    /* The UI poll must have seen the run as well: before its first poll the
     * controller still reports PREPARING and the facade refuses Play. */
    Window *window = value; RadioSAudioRunSnapshot actual = {0}; RadioSPlayoutSnapshot ui = {0};
    return window_ready(window) && radios_playout_ui_navigation_snapshot(window->ctx, &actual) &&
        (actual.state == RADIOS_AUDIO_RUN_PLAYING || actual.state == RADIOS_AUDIO_RUN_OVERLAP) &&
        radios_playout_ui_get_snapshot(window->ctx, &ui) && ui.state != RADIOS_PLAYOUT_PREPARING &&
        radios_broadcast_navigation_available(window->ctx, 2);
}
static void run_wait(Window *window, RadioSPlayoutState state, guint index)
{
    RunCondition expected = {window, state, index};
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    g_test_message("Waiting for local UI state %d, index %u", state, index);
    while (!run_matches(&expected) && g_get_monotonic_time() < deadline) pump();
    if (!run_matches(&expected)) {
        RadioSPlayoutSnapshot snapshot = {0}; RadioSAudioRunSnapshot actual = {0};
        radios_playout_ui_get_snapshot(window->ctx, &snapshot);
        radios_playout_ui_navigation_snapshot(window->ctx, &actual);
        g_error("Header navigation state timeout: expected %d/%u; UI %d/%u (%s), actual %d/%u epoch %" G_GUINT64_FORMAT " (%s)",
            state, index, snapshot.state, snapshot.index, snapshot.error,
            actual.state, actual.index, actual.epoch, actual.error);
    }
}
static void immediate_resume(Window *window, RadioSAudioEngine *engine, const gchar *owner)
{
    wait_until(widget_enabled, find(window->root, "header-pause"), "pause ready before immediate resume");
    wait_until(actual_pause_ready, window, "authoritative owned PLAYING runner before immediate pause probe");
    RadioSAudioRunSnapshot before, after; radios_audio_engine_runner_snapshot(engine, &before);
    g_test_message("%s before Pause: state %d index %u epoch %" G_GUINT64_FORMAT " frame %" G_GUINT64_FORMAT " entry %s",
        owner, before.state, before.index, before.epoch, before.actual_current_frame, before.entry_id);
    gtk_button_clicked(GTK_BUTTON(find(window->root, "header-pause")));
    radios_audio_engine_runner_snapshot(engine, &after);
    g_test_message("%s after Pause: state %d index %u epoch %" G_GUINT64_FORMAT " frame %" G_GUINT64_FORMAT " entry %s",
        owner, after.state, after.index, after.epoch, after.actual_current_frame, after.entry_id);
    g_assert_cmpint(after.state, ==, RADIOS_AUDIO_RUN_PAUSED);
    /* The facade observes PAUSED before the GTK poll. Pulse deliberately
     * rejects resume until its pending output flush is acknowledged. */
    GError *error = NULL; gboolean accepted = radios_broadcast_navigation_action(window->ctx, 1, &error);
    radios_audio_engine_runner_snapshot(engine, &after);
    g_assert_cmpuint(after.epoch, ==, before.epoch); g_assert_cmpstr(after.entry_id, ==, before.entry_id);
    if (!accepted) {
        /* The rejection comes either from the engine (flush pending) or, as the
         * navigation availability now mirrors that state, from the facade. */
        g_assert_nonnull(error);
        g_assert_true(g_error_matches(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_UNAVAILABLE) ||
            g_error_matches(error, G_IO_ERROR, G_IO_ERROR_FAILED));
        g_assert_cmpint(after.state, ==, RADIOS_AUDIO_RUN_PAUSED);
        g_test_message("%s immediate resume retained epoch/entry while MAIN flush was pending: %s", owner, error->message);
        g_clear_error(&error);
        /* The engine coalesces output flushes, so MAIN may report ready just
         * before the pause's flush begins; a Play in that gap is refused the
         * same way. Each refusal keeps epoch and entry; Play is pressed again
         * once MAIN is ready, as an operator would. */
        gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND; guint presses = 0;
        do {
            wait_until(window_ready, window, "MAIN output flush acknowledged before same-owner header resume");
            radios_audio_engine_runner_snapshot(engine, &after);
            if (after.state == RADIOS_AUDIO_RUN_PLAYING) break; /* an earlier press resumed once the flush ended */
            RadioSPlayoutSnapshot ui = {0}; radios_playout_ui_get_snapshot(window->ctx, &ui);
            g_test_message("%s before header Play #%u: engine state %d epoch %" G_GUINT64_FORMAT "; UI state %d",
                owner, presses + 1, after.state, after.epoch, ui.state);
            click(window, "header-play"); ++presses;
            for (gint64 settle = g_get_monotonic_time() + 300 * G_TIME_SPAN_MILLISECOND; g_get_monotonic_time() < settle;) {
                radios_audio_engine_runner_snapshot(engine, &after); if (after.state == RADIOS_AUDIO_RUN_PLAYING) break; pump();
            }
            g_assert_cmpuint(after.epoch, ==, before.epoch); g_assert_cmpstr(after.entry_id, ==, before.entry_id);
            if (after.state != RADIOS_AUDIO_RUN_PLAYING) g_assert_cmpint(after.state, ==, RADIOS_AUDIO_RUN_PAUSED);
        } while (after.state != RADIOS_AUDIO_RUN_PLAYING && g_get_monotonic_time() < deadline);
        /* A further Play on the resumed run never restarts it. */
        click(window, "header-play"); radios_audio_engine_runner_snapshot(engine, &after);
        g_assert_cmpuint(after.epoch, ==, before.epoch); g_assert_cmpstr(after.entry_id, ==, before.entry_id);
        g_test_message("%s header Play resumed after %u press(es) once the MAIN flush was acknowledged", owner, presses);
    } else {
        /* The controller bridges the short post-flush gap (RESUME retry) and
         * the engine applies an accepted resume on its next control round. */
        g_assert_no_error(error);
        for (gint64 settle = g_get_monotonic_time() + G_TIME_SPAN_SECOND; after.state != RADIOS_AUDIO_RUN_PLAYING && g_get_monotonic_time() < settle;) {
            pump(); radios_audio_engine_runner_snapshot(engine, &after);
        }
    }
    g_assert_cmpint(after.state, ==, RADIOS_AUDIO_RUN_PLAYING); g_assert_cmpuint(after.epoch, ==, before.epoch);
    g_assert_cmpstr(after.entry_id, ==, before.entry_id); g_assert_cmpuint(after.actual_current_frame, >=, before.actual_current_frame);
}
typedef struct { RadioSDocument *document; gint64 revision; } SaveCondition;
static gboolean save_done(gpointer value)
{ SaveCondition *expected = value; return radios_data_document_ready(expected->document) && radios_data_document_revision(expected->document) > expected->revision; }
static void test_local_navigation(void)
{
    Window window = window_open(); JsonNode *first = import_media(&window, "Prompt5 A", 700), *second = import_media(&window, "Prompt5 B", 1700);
    JsonNode *configuration = programme_config(first, second, 0); save_programme("Mevcut program", configuration);
    window_close(&window); window = window_open(); g_assert_true(radios_window_select_profile(window.ui, "broadcast"));
    /* The broadcast page starts in the calendar-driven continuous mode; this
     * finite local queue case explicitly selects the saved programme. */
    GtkWidget *continuous = find(window.root, "programme-general-continuous");
    if (continuous) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(continuous), FALSE);
    gtk_combo_box_set_active(GTK_COMBO_BOX(find(window.root, "programme-transition-mode")), 0);
    click(&window, "header-play"); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); RadioSAudioRunSnapshot run;
    radios_audio_engine_runner_snapshot(engine, &run); GError *error = NULL;
    g_assert_false(radios_audio_engine_runner_jump_to(engine, run.epoch + 1, 2, &error)); g_assert_nonnull(error); g_clear_error(&error);
    g_assert_false(radios_audio_engine_runner_jump_to(engine, run.epoch, G_MAXUINT, &error)); g_assert_nonnull(error); g_clear_error(&error);
    RadioSAudioRunSnapshot retained; radios_audio_engine_runner_snapshot(engine, &retained);
    g_assert_cmpuint(retained.epoch, ==, run.epoch); g_assert_cmpuint(retained.index, ==, 0);
    immediate_resume(&window, engine, "Local queue");
    click(&window, "header-next-track"); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1);
    click(&window, "header-previous"); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0);
    /* Change the editable block mapping while the accepted plan is running.
     * Header navigation must follow the immutable plan, not this new draft. */
    JsonNode *edited = clone(configuration); JsonObject *object = json_node_get_object(edited);
    JsonArray *events = json_object_get_array_member(object, "events"), *blocks = json_object_get_array_member(object, "blocks");
    const gchar *last_id = radios_json_string(json_array_get_object_element(blocks, 2), "id", "");
    json_object_set_string_member(json_array_get_object_element(events, 1), "block_id", last_id);
    RadioSDocument *document = radios_broadcast_programme_document(window.ctx); SaveCondition saved = {document, radios_data_document_revision(document)};
    g_assert_true(radios_data_document_save(document, edited)); json_node_free(edited);
    wait_until(save_done, &saved, "changed draft saved while the original queue remains active");
    g_assert_true(radios_window_select_profile(window.ui, "technical"));
    click(&window, "header-next-playlist"); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 2);
    wait_until(widget_disabled, find(window.root, "header-next-playlist"), "last playlist has no successor");
    g_assert_false(radios_broadcast_navigation_action(window.ctx, 5, &error)); g_assert_nonnull(error); g_clear_error(&error);
    run_wait(&window, RADIOS_PLAYOUT_PLAYING, 2);
    click(&window, "header-pause"); run_wait(&window, RADIOS_PLAYOUT_PAUSED, 2);
    radios_audio_engine_runner_snapshot(engine, &run);
    g_assert_false(radios_audio_engine_runner_jump_to(engine, run.epoch, 3, &error)); g_assert_nonnull(error); g_clear_error(&error);
    radios_audio_engine_runner_snapshot(engine, &retained);
    g_assert_cmpuint(retained.index, ==, 2); g_assert_cmpint(retained.state, ==, RADIOS_AUDIO_RUN_PAUSED);
    wait_until(window_ready, &window, "MAIN flush acknowledged before ordinary paused resume");
    click(&window, "header-play"); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 2);
    click(&window, "header-next-track"); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 3);
    wait_until(widget_disabled, find(window.root, "header-next-track"), "last content has no successor");
    click(&window, "header-stop"); run_wait(&window, RADIOS_PLAYOUT_IDLE, 0);
    g_assert_true(radios_playout_ui_schedule_programme(window.ctx, window.root, json_node_get_object(configuration),
        g_get_real_time() / 1000 + 60000, 0, 0, &error)); g_assert_no_error(error);
    run_wait(&window, RADIOS_PLAYOUT_SCHEDULED, 0); radios_audio_engine_runner_snapshot(engine, &run);
    g_assert_false(radios_audio_engine_runner_jump_to(engine, run.epoch, 2, &error)); g_assert_nonnull(error); g_clear_error(&error);
    radios_audio_engine_runner_snapshot(engine, &retained); g_assert_cmpint(retained.state, ==, RADIOS_AUDIO_RUN_SCHEDULED);
    g_assert_cmpuint(retained.index, ==, 0); click(&window, "header-stop"); run_wait(&window, RADIOS_PLAYOUT_IDLE, 0);
    radios_audio_engine_unref(engine);
    json_node_free(configuration); json_node_free(first); json_node_free(second); window_close(&window);
}
typedef struct { RadioSAudioEngine *engine; guint deck, index; RadioSAudioRunState state; } PlayerCondition;
static gboolean player_matches(gpointer value)
{
    PlayerCondition *expected = value; RadioSAudioRunSnapshot snapshot;
    radios_audio_engine_player_runner_snapshot(expected->engine, expected->deck, &snapshot);
    return snapshot.state == expected->state && snapshot.index == expected->index;
}
static RadioSAudioRunSnapshot player_wait(RadioSAudioEngine *engine, guint deck, RadioSAudioRunState state, guint index)
{
    PlayerCondition expected = {engine, deck, index, state}; wait_until(player_matches, &expected, "selected independent player actually advances");
    RadioSAudioRunSnapshot snapshot; radios_audio_engine_player_runner_snapshot(engine, deck, &snapshot); return snapshot;
}
static void queue_select(Window *window, const gchar *id)
{
    GtkWidget *selector = find(window->root, "programme-player-selector"); g_assert_true(GTK_IS_COMBO_BOX(selector));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(selector), id)); wait_until(window_ready, window, "selected independent queue clean acknowledged document");
}
static void test_selected_queue_isolation(void)
{
    Window window = window_open(); JsonNode *first = import_media(&window, "Queue A", 800), *second = import_media(&window, "Queue B", 1900);
    JsonNode *first_queue = programme_config(first, second, 1), *other_queue = programme_config(second, first, 2);
    save_programme("Oynatıcı 1 yayın sırası", first_queue); save_programme("Oynatıcı 2 yayın sırası", other_queue);
    window_close(&window); window = window_open(); g_assert_true(radios_window_select_profile(window.ui, "broadcast"));
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    queue_select(&window, "2"); click(&window, "header-play"); player_wait(engine, 1, RADIOS_AUDIO_RUN_PLAYING, 0);
    click(&window, "header-pause"); RadioSAudioRunSnapshot other = player_wait(engine, 1, RADIOS_AUDIO_RUN_PAUSED, 0);
    queue_select(&window, "1"); click(&window, "header-play"); player_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0);
    click(&window, "header-next-track"); player_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 1);
    click(&window, "header-next-playlist"); player_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 2);
    RadioSAudioRunSnapshot unchanged = player_wait(engine, 1, RADIOS_AUDIO_RUN_PAUSED, 0);
    g_assert_cmpuint(unchanged.epoch, ==, other.epoch); g_assert_cmpstr(unchanged.entry_id, ==, other.entry_id);
    RadioSAudioRunSnapshot global; radios_audio_engine_runner_snapshot(engine, &global); g_assert_cmpint(global.state, ==, RADIOS_AUDIO_RUN_IDLE);
    click(&window, "header-stop"); queue_select(&window, "2"); player_wait(engine, 1, RADIOS_AUDIO_RUN_PAUSED, 0);
    click(&window, "header-stop"); radios_audio_engine_unref(engine);
    json_node_free(first_queue); json_node_free(other_queue); json_node_free(first); json_node_free(second); window_close(&window);
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]); for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1]; --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    main_sink = g_getenv("RADIOS_TEST_MAIN_SINK"); cue_sink = g_getenv("RADIOS_TEST_CUE_SINK");
    g_assert_nonnull(main_sink); g_assert_nonnull(cue_sink); g_assert_true(g_str_has_prefix(main_sink, "radios_gui_"));
    GError *error = NULL; g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    application = gtk_application_new("org.radios.Prompt5HeaderAcceptance", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/native-header-navigation/footer-relocation-diagnostic-backup-offline", test_footer_relocation);
    g_test_add_func("/native-header-navigation/local-next-immutable-playlist-boundary", test_local_navigation);
    g_test_add_func("/native-header-navigation/selected-player-queue-isolation", test_selected_queue_isolation);
    gint result = g_test_run(); radios_data_client_unref(reader); g_object_unref(application); radios_config_clear(&config); return result;
}
