#include "radios/window.h"
#include "radios/data.h"
#include "radios/audio_ui.h"
#include "radios/header_navigation.h"
#include "radios/media_jobs.h"
#include "radios/playout_ui.h"
#include "radios/programme_queue.h"
#include "radios/weekly_programme_plan.h"
#include "radios/player_rotation.h"
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

static RadioSConfig config;
static GtkApplication *application;
static RadioSDataClient *reader;
typedef struct { RadioSWindow *ui; RadioSRuntime *runtime; RadioSViewContext *ctx; GtkWidget *root; } Window;
typedef gboolean (*Condition)(gpointer);

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (!g_strcmp0(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = find(item->data, name);
    g_list_free(children); return found;
}
static gboolean contains_label(GtkWidget *root, const gchar *text)
{
    if (GTK_IS_LABEL(root) && !g_strcmp0(gtk_label_get_text(GTK_LABEL(root)), text)) return TRUE;
    if (!GTK_IS_CONTAINER(root)) return FALSE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); gboolean found = FALSE;
    for (GList *item = children; item && !found; item = item->next) found = contains_label(item->data, text);
    g_list_free(children); return found;
}
static void pump(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void wait_until(Condition condition, gpointer value, const gchar *description)
{
    gint64 end = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!condition(value) && g_get_monotonic_time() < end) pump();
    if (!condition(value)) g_error("General queue GUI timed out: %s", description);
}
static gboolean documents_ready(GtkWidget *root)
{
    GPtrArray *docs = g_object_get_data(G_OBJECT(root), "radios-data-documents");
    for (guint i = 0; docs && i < docs->len; ++i) if (!radios_data_document_ready(g_ptr_array_index(docs, i))) return FALSE;
    if (!GTK_IS_CONTAINER(root)) return TRUE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); gboolean ready = TRUE;
    for (GList *item = children; item && ready; item = item->next) ready = documents_ready(item->data);
    g_list_free(children); return ready;
}
static gboolean window_ready(gpointer value)
{
    Window *w = value; GtkWidget *output = find(w->root, "task-main-output");
    return radios_data_connected(w->ctx) && documents_ready(w->root) && output &&
        !g_strcmp0(gtk_combo_box_get_active_id(GTK_COMBO_BOX(output)), g_getenv("RADIOS_TEST_MAIN_SINK")) &&
        radios_audio_ui_ready(w->ctx, RADIOS_AUDIO_UI_MAIN);
}
static Window window_open(void)
{
    Window w = {0}; GError *error = NULL; w.runtime = radios_runtime_new(64, 128);
    w.ui = radios_window_new(application, &config, w.runtime, FALSE, NULL, &error); g_assert_no_error(error); g_assert_nonnull(w.ui);
    w.ctx = radios_window_view_context(w.ui); w.root = GTK_WIDGET(w.ctx->parent);
    g_assert_nonnull(w.ctx->audio); g_assert_nonnull(w.ctx->playout); radios_window_present(w.ui);
    g_assert_true(radios_window_select_profile(w.ui, "broadcast"));
    wait_until(window_ready, &w, "real DB, saved private MAIN output and document baselines"); return w;
}
static JsonNode *copy(JsonNode *node)
{
    gchar *text = json_to_string(node, FALSE); JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));
    JsonNode *result = json_node_copy(json_parser_get_root(parser)); g_object_unref(parser); g_free(text); return result;
}
typedef struct { const gchar *path; JsonNode *body, *result; GError *error; gint done; } Request;
static gpointer request_work(gpointer value)
{
    Request *r = value; r->result = radios_data_client_request_sync(reader, "POST", r->path, r->body, NULL, &r->error);
    g_atomic_int_set(&r->done, 1); return NULL;
}
static gboolean request_done(gpointer value) { return g_atomic_int_get(&((Request *)value)->done); }
static JsonNode *request(const gchar *path, JsonNode *body)
{
    Request r = {.path = path, .body = body}; GThread *thread = g_thread_new("general-queue-fixture", request_work, &r);
    wait_until(request_done, &r, "authenticated private backend operation"); g_thread_join(thread);
    g_assert_no_error(r.error); g_assert_nonnull(r.result); return r.result;
}
static JsonNode *body_new(void)
{
    JsonNode *body = json_node_new(JSON_NODE_OBJECT); json_node_take_object(body, json_object_new());
    json_object_set_string_member(json_node_get_object(body), "station_id", radios_data_client_station_id(reader)); return body;
}
static void unique_key(JsonObject *object)
{ gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key); }
static void unique_id(JsonObject *object, const gchar *member)
{ gchar *key = g_uuid_string_random(); json_object_set_string_member(object, member, key); g_free(key); }
static JsonNode *document_read(const gchar *kind, const gchar *name)
{
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "kind", kind);
    JsonNode *response = request("/v1/workflows/desktop/list", body); json_node_free(body);
    JsonArray *records = json_object_get_array_member(json_node_get_object(response), "records"); JsonNode *found = NULL;
    for (guint i = 0; i < json_array_get_length(records); ++i)
        if (!g_strcmp0(radios_json_string(json_array_get_object_element(records, i), "name", ""), name))
            found = copy(json_array_get_element(records, i));
    json_node_free(response); return found;
}
static JsonNode *document_save(const gchar *kind, const gchar *name, JsonNode *configuration)
{
    JsonNode *previous = document_read(kind, name), *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", kind); json_object_set_string_member(object, "name", name);
    json_object_set_member(object, "config", copy(configuration)); unique_key(object);
    if (previous) {
        JsonObject *record = json_node_get_object(previous);
        json_object_set_string_member(object, "document_id", radios_json_string(record, "_id", ""));
        json_object_set_int_member(object, "expected_revision", radios_json_int(record, "_rev", 0));
    }
    JsonNode *saved = request("/v1/workflows/desktop/put", body);
    json_node_free(body); g_clear_pointer(&previous, json_node_free); return saved;
}
static void document_unchanged(JsonNode *saved)
{
    JsonObject *record = json_node_get_object(saved);
    JsonNode *actual = document_read(radios_json_string(record, "kind", ""), radios_json_string(record, "name", ""));
    g_assert_nonnull(actual); g_assert_true(json_node_equal(saved, actual)); json_node_free(actual);
}
static void bind(JsonObject *object, JsonNode *detail)
{
    JsonObject *root = json_node_get_object(detail), *media = json_object_get_object_member(root, "media"), *version = json_object_get_object_member(root, "version");
    json_object_set_string_member(object, "media_id", radios_json_string(media, "_id", ""));
    json_object_set_string_member(object, "version_id", radios_json_string(version, "_id", ""));
    json_object_set_string_member(object, "media_file_id", radios_json_string(version, "media_file_id", ""));
    json_object_set_string_member(object, "media_sha256", radios_json_string(version, "sha256", ""));
}
static void u16(guchar *bytes, guint16 value) { value = GUINT16_TO_LE(value); memcpy(bytes, &value, sizeof value); }
static void u32(guchar *bytes, guint32 value) { value = GUINT32_TO_LE(value); memcpy(bytes, &value, sizeof value); }
typedef struct { JsonNode *result; GError *error; gint done; } Import;
static void imported(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; (void)owner; Import *i = value;
    if (result) i->result = copy(result);
    if (error) i->error = g_error_copy(error);
}
static void import_finished(gpointer value) { ((Import *)value)->done = TRUE; }
static gboolean import_done(gpointer value) { return ((Import *)value)->done; }
static JsonNode *media_import(Window *w)
{
    guint frames = 48000 * 6 + 31, bytes = frames * 4; guchar *wav = g_malloc(44 + bytes);
    memcpy(wav, "RIFF", 4); u32(wav + 4, 36 + bytes); memcpy(wav + 8, "WAVEfmt ", 8); u32(wav + 16, 16);
    u16(wav + 20, 1); u16(wav + 22, 2); u32(wav + 24, 48000); u32(wav + 28, 192000); u16(wav + 32, 4); u16(wav + 34, 16);
    memcpy(wav + 36, "data", 4); u32(wav + 40, bytes);
    for (guint i = 0; i < frames; ++i) {
        gint16 sample = (gint16)lrint(sin(2 * G_PI * 1730 * i / 48000) * .25 * 32767);
        u16(wav + 44 + i * 4, (guint16)sample); u16(wav + 46 + i * 4, (guint16)sample);
    }
    gchar *id = g_uuid_string_random(), *file = g_strconcat(id, ".wav", NULL), *path = g_build_filename(config.project_root, file, NULL);
    g_assert_true(g_file_set_contents(path, (const gchar *)wav, 44 + bytes, NULL)); g_free(wav); g_free(id); g_free(file);
    Import output = {0}; GCancellable *handle = radios_media_import_async(w->ctx, w->root, path, "music", "General verified tone", imported, &output, import_finished);
    g_assert_nonnull(handle); wait_until(import_done, &output, "real staged PCM analysis and SHA-bound import");
    g_assert_no_error(output.error); g_assert_nonnull(output.result);
    JsonNode *body = body_new(); JsonObject *media = json_object_get_object_member(json_node_get_object(output.result), "media");
    json_object_set_string_member(json_node_get_object(body), "media_id", radios_json_string(media, "_id", ""));
    JsonNode *detail = request("/v1/workflows/desktop-media/get", body); JsonObject *version = json_object_get_object_member(json_node_get_object(detail), "version");
    g_assert_true(json_object_get_boolean_member(version, "verified")); g_assert_true(json_object_get_boolean_member(version, "object_available"));
    json_node_free(body); json_node_free(output.result); g_object_unref(handle); g_assert_cmpint(g_unlink(path), ==, 0); g_free(path); return detail;
}
static JsonNode *playlist_create_named(JsonNode *detail, const gchar *name)
{
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "name", name); unique_key(object);
    JsonNode *playlist = request("/v1/workflows/desktop-playlists/create", body); json_node_free(body);
    body = body_new(); object = json_node_get_object(body); JsonObject *record = json_node_get_object(playlist), *metadata = json_object_new();
    json_object_set_string_member(object, "playlist_id", radios_json_string(record, "_id", ""));
    json_object_set_int_member(object, "expected_revision", radios_json_int(record, "_rev", 0)); unique_key(object);
    json_object_set_string_member(metadata, "title", "General verified tone"); json_object_set_string_member(metadata, "artist", "Private fixture");
    json_object_set_string_member(metadata, "album", ""); json_object_set_string_member(metadata, "genre", ""); json_object_set_string_member(metadata, "key", "");
    json_object_set_string_member(metadata, "comment", "Private verified general queue source"); json_object_set_string_member(metadata, "location", "/untrusted.wav");
    json_object_set_int_member(metadata, "duration_ms", 6001); json_object_set_double_member(metadata, "bpm", 120); bind(metadata, detail);
    json_object_set_object_member(object, "metadata", metadata);
    JsonNode *saved = request("/v1/workflows/desktop-playlists/entry-save", body); json_node_free(body); json_node_free(playlist); return saved;
}
static JsonNode *playlist_create(JsonNode *detail) { return playlist_create_named(detail, "General queue source playlist"); }
static JsonNode *calendar(JsonNode *playlist, gint64 start_us, const gchar *title)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *config_object = json_object_new(); json_node_take_object(node, config_object);
    json_object_set_int_member(config_object, "schema", 1); json_object_set_string_member(config_object, "timezone", "UTC");
    JsonObject *definition = json_object_new(), *settings = json_object_new(), *source = json_object_new(); unique_id(definition, "id"); unique_id(source, "id");
    json_object_set_string_member(definition, "name", "General queue verified rotation"); json_object_set_string_member(settings, "selection_mode", "linear");
    const gchar *gaps[] = {"track_gap_minutes", "artist_gap_minutes", "album_gap_minutes"};
    for (guint i = 0; i < G_N_ELEMENTS(gaps); ++i) json_object_set_int_member(settings, gaps[i], 0);
    JsonArray *transitions = json_array_new();
    for (guint i = 0; i < 3; ++i) {
        JsonObject *transition = json_object_new(); json_object_set_int_member(transition, "mode", i);
        json_object_set_int_member(transition, "weight", i ? 0 : 100); json_object_set_int_member(transition, "overlap_ms", 0); json_array_add_object_element(transitions, transition);
    }
    json_object_set_array_member(settings, "transitions", transitions); json_object_set_object_member(definition, "settings", settings);
    json_object_set_string_member(source, "playlist_id", radios_json_string(json_node_get_object(playlist), "_id", ""));
    json_object_set_int_member(source, "frequency", 1); json_object_set_int_member(source, "count", 1); json_object_set_int_member(source, "weight", 100);
    json_object_set_int_member(source, "start_minute", 0); json_object_set_int_member(source, "end_minute", 1440); json_object_set_int_member(source, "interval_minutes", 60);
    json_object_set_string_member(source, "media_kind", "music");
    const gchar *flags[] = {"random", "track_random", "advert_random", "sweeper_random"};
    for (guint i = 0; i < G_N_ELEMENTS(flags); ++i) json_object_set_boolean_member(source, flags[i], FALSE);
    JsonArray *sources = json_array_new(); json_array_add_object_element(sources, source); json_object_set_array_member(definition, "sources", sources);
    JsonArray *library = json_array_new(); json_array_add_object_element(library, definition); json_object_set_array_member(config_object, "rotation_library", library);
    GDateTime *start = g_date_time_new_from_unix_utc(start_us / G_USEC_PER_SEC); gchar *date = g_date_time_format(start, "%Y-%m-%d");
    guint minute = g_date_time_get_hour(start) * 60 + g_date_time_get_minute(start);
    JsonObject *event = json_object_new(), *assignment = json_object_new(); unique_id(event, "id"); unique_id(assignment, "id");
    json_object_set_string_member(event, "title", title); json_object_set_string_member(event, "date", date); json_object_set_string_member(event, "color", "#abd6e4");
    json_object_set_int_member(event, "start_minute", minute); json_object_set_int_member(event, "end_minute", minute + 1);
    json_object_set_string_member(assignment, "rotation_id", radios_json_string(definition, "id", "")); json_object_set_int_member(assignment, "weight", 100);
    JsonArray *assignments = json_array_new(); json_array_add_object_element(assignments, assignment); json_object_set_array_member(event, "rotations", assignments);
    JsonArray *events = json_array_new(); json_array_add_object_element(events, event); json_object_set_array_member(config_object, "events", events);
    g_free(date); g_date_time_unref(start); return node;
}
static JsonNode *manual_programme(JsonNode *detail, const gchar *title, guint slot)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new(), *leaf = json_object_new(); json_node_take_object(node, object);
    GDateTime *now = g_date_time_new_now_utc(); gchar *date = g_date_time_format(now, "%Y-%m-%d"); g_date_time_unref(now);
    json_object_set_string_member(object, "title", title); json_object_set_string_member(object, "date", date); g_free(date); json_object_set_string_member(object, "note", "Preserve this editor draft");
    unique_id(leaf, "id"); json_object_set_string_member(leaf, "title", title); json_object_set_string_member(leaf, "artist", "Private fixture");
    json_object_set_string_member(leaf, "album", ""); json_object_set_string_member(leaf, "kind", "music"); json_object_set_string_member(leaf, "start_time", "");
    json_object_set_int_member(leaf, "duration_ms", 6001); json_object_set_string_member(leaf, "version_label", "Private verified media");
    json_object_set_string_member(leaf, "primary_file", ""); json_object_set_string_member(leaf, "backup_file", ""); json_object_set_string_member(leaf, "note", ""); bind(leaf, detail);
    JsonArray *events = json_array_new(); json_array_add_object_element(events, leaf); json_object_set_array_member(object, "events", events);
    if (slot) {
        json_object_set_int_member(object, "queue_player", slot); json_object_set_int_member(object, "queue_transition", 0); json_object_set_double_member(object, "queue_overlap", 0);
    }
    return node;
}
typedef struct { Window *w; const gchar *event, *status, *row; gboolean general; } Preview;
static gboolean preview_ready(gpointer value)
{
    Preview *p = value; GtkWidget *status = find(p->w->root, "programme-general-schedule"), *table = find(p->w->root, "programme-queue-table");
    const gchar *text = status ? gtk_label_get_text(GTK_LABEL(status)) : "";
    /* The normal queue initially shows event blocks. Open the real V action
     * to inspect the prepared media rows, as an operator would. */
    if (table && !contains_label(table, p->row)) {
        GtkWidget *list = g_object_get_data(G_OBJECT(table), "list-box");
        GList *rows = list ? gtk_container_get_children(GTK_CONTAINER(list)) : NULL;
        for (GList *item = rows; item; item = item->next)
            if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item->data), "programme-item-block"))) {
                GtkWidget *button = find(item->data, "programme-row-waveform");
                if (button && gtk_widget_is_sensitive(button)) { g_object_ref(button); gtk_button_clicked(GTK_BUTTON(button)); g_object_unref(button); }
                break;
            }
        g_list_free(rows);
    }
    return status && table && strstr(text, p->event) && strstr(text, p->status) &&
        gtk_widget_get_visible(status) == p->general && contains_label(table, p->row);
}
static void print_labels(GtkWidget *root)
{
    if (GTK_IS_LABEL(root)) g_test_message("Queue label: %s", gtk_label_get_text(GTK_LABEL(root)));
    if (!GTK_IS_CONTAINER(root)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    for (GList *item = children; item; item = item->next) print_labels(item->data);
    g_list_free(children);
}
static void preview_wait(Window *w, const gchar *event, const gchar *status, const gchar *row, gboolean general)
{
    Preview expected = {w, event, status, row, general}; gint64 end = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!preview_ready(&expected) && g_get_monotonic_time() < end) pump();
    if (!preview_ready(&expected)) {
        GtkWidget *label = find(w->root, "programme-general-schedule"); print_labels(find(w->root, "programme-queue-table"));
        g_error("Expected general label '%s' / '%s', row '%s', visible %d; actual '%s', visible %d",
            event, status, row, general, gtk_label_get_text(GTK_LABEL(label)), gtk_widget_get_visible(label));
    }
}
static gboolean enabled(gpointer value) { return gtk_widget_is_sensitive(value); }
static gboolean disabled(gpointer value) { return !gtk_widget_is_sensitive(value); }
typedef struct { GtkWidget *label; const gchar *text; } Label;
static gboolean label_contains(gpointer value)
{ Label *l = value; return strstr(gtk_label_get_text(GTK_LABEL(l->label)), l->text) != NULL; }
static void click(Window *w, const gchar *name)
{ GtkWidget *button = find(w->root, name); g_assert_true(GTK_IS_BUTTON(button)); wait_until(enabled, button, name); gtk_button_clicked(GTK_BUTTON(button)); }
static void select_queue(Window *w, const gchar *id)
{ g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(w->root, "programme-player-selector")), id)); }
static void assert_runner_idle(Window *w)
{
    RadioSPlayoutSnapshot run = {0}; g_assert_true(radios_playout_ui_get_snapshot(w->ctx, &run)); g_assert_cmpint(run.state, ==, RADIOS_PLAYOUT_IDLE);
}
static void playlist_unchanged(JsonNode *saved)
{
    JsonNode *body = body_new(), *result = request("/v1/workflows/desktop-playlists/list", body); json_node_free(body);
    JsonArray *lists = json_object_get_array_member(json_node_get_object(result), "playlists"); JsonObject *expected = json_node_get_object(saved), *actual = NULL;
    for (guint i = 0; i < json_array_get_length(lists); ++i) {
        JsonObject *row = json_array_get_object_element(lists, i);
        if (!g_strcmp0(radios_json_string(row, "_id", ""), radios_json_string(expected, "_id", ""))) actual = row;
    }
    g_assert_nonnull(actual); g_assert_cmpint(radios_json_int(actual, "_rev", 0), ==, radios_json_int(expected, "_rev", 0));
    JsonNode *a = json_object_get_member(actual, "tracks"), *b = json_object_get_member(expected, "tracks"); g_assert_true(json_node_equal(a, b)); json_node_free(result);
}
static void test_general_queue(void)
{
    Window w = window_open();
    GtkWidget *continuous = find(w.root, "programme-general-continuous"); g_assert_true(GTK_IS_TOGGLE_BUTTON(continuous));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(continuous)));
    click(&w, "header-play");
    Label empty_waiting = {find(w.root, "programme-general-schedule"), "Gelecek program bekleniyor"};
    wait_until(label_contains, &empty_waiting, "Play arms continuous automation even when no future source exists");
    wait_until(enabled, find(w.root, "header-pause"), "empty future waiting scheduler is armed"); assert_runner_idle(&w);
    click(&w, "header-pause"); wait_until(disabled, find(w.root, "header-pause"), "empty future waiting scheduler pauses");
    g_assert_true(gtk_widget_is_sensitive(find(w.root, "header-stop")));
    click(&w, "header-play"); wait_until(enabled, find(w.root, "header-pause"), "empty future waiting scheduler resumes");
    click(&w, "header-stop"); wait_until(disabled, find(w.root, "header-stop"), "empty future waiting Stop disarms"); assert_runner_idle(&w);
    JsonNode *media = media_import(&w), *playlist = playlist_create(media);
    gint64 base = (g_get_real_time() / (60 * G_USEC_PER_SEC) + 1) * (60 * G_USEC_PER_SEC);
    JsonNode *main = calendar(playlist, base + 20 * 60 * G_USEC_PER_SEC, "Later main event");
    JsonNode *main_saved = document_save("automation_calendar", "main", main); json_node_free(main);
    JsonNode *weekly_calendar = calendar(playlist, base + 10 * 60 * G_USEC_PER_SEC, "Earlier saved weekly event");
    GDateTime *utc = g_date_time_new_from_unix_utc(base / G_USEC_PER_SEC), *monday = g_date_time_add_days(utc, 1 - g_date_time_get_day_of_week(utc));
    gchar *anchor = g_date_time_format(monday, "%Y-%m-%d"); GError *error = NULL;
    JsonNode *weekly = radios_weekly_programme_snapshot(json_node_get_object(weekly_calendar), anchor, FALSE, &error);
    g_assert_no_error(error); g_assert_nonnull(weekly); g_free(anchor); g_date_time_unref(monday); g_date_time_unref(utc); json_node_free(weekly_calendar);
    JsonNode *weekly_saved = document_save("weekly_programme", "General UI saved weekly", weekly); json_node_free(weekly);
    JsonNode *manual = manual_programme(media, "Manual general draft preserved", 0), *private_queue = manual_programme(media, "Private queue preserved", 1);
    JsonNode *manual_saved = document_save("programme", "Mevcut program", manual), *private_saved = document_save("programme", "Oynatıcı 1 yayın sırası", private_queue);
    json_node_free(manual); json_node_free(private_queue); radios_data_refresh(w.ctx);
    preview_wait(&w, "Earlier saved weekly event", "Sıradaki program", "General verified tone", TRUE);
    assert_runner_idle(&w); wait_until(disabled, find(w.root, "header-stop"), "idle scheduler is disarmed");
    GtkWidget *table = find(w.root, "programme-queue-table"), *list = g_object_get_data(G_OBJECT(table), "list-box");
    GList *rows = gtk_container_get_children(GTK_CONTAINER(list)); guint playable = 0;
    for (GList *item = rows; item; item = item->next)
        if (!GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item->data), "programme-item-block"))) {
            g_assert_true(g_uuid_string_is_valid(radios_programme_queue_row_id(item->data))); ++playable;
        }
    g_list_free(rows); g_assert_cmpuint(playable, >=, 1);
    select_queue(&w, "1"); preview_wait(&w, "Earlier saved weekly event", "Sıradaki program", "Private queue preserved", FALSE);
    main = calendar(playlist, base + 5 * 60 * G_USEC_PER_SEC, "Nearest main event");
    json_node_free(main_saved); main_saved = document_save("automation_calendar", "main", main); json_node_free(main); radios_data_refresh(w.ctx);
    preview_wait(&w, "Nearest main event", "Sıradaki program", "Private queue preserved", FALSE);
    select_queue(&w, "0"); preview_wait(&w, "Nearest main event", "Sıradaki program", "General verified tone", TRUE);
    click(&w, "header-play"); preview_wait(&w, "Nearest main event", "Sürekli otomasyon", "General verified tone", TRUE);
    wait_until(enabled, find(w.root, "header-pause"), "armed future scheduler can pause before owning a PCM runner"); assert_runner_idle(&w);
    click(&w, "header-pause"); preview_wait(&w, "Nearest main event", "Otomasyon duraklatıldı", "General verified tone", TRUE);
    wait_until(disabled, find(w.root, "header-pause"), "paused general waiting state rejects another pause"); assert_runner_idle(&w);
    click(&w, "header-play"); preview_wait(&w, "Nearest main event", "Sürekli otomasyon", "General verified tone", TRUE); assert_runner_idle(&w);
    click(&w, "header-stop"); wait_until(disabled, find(w.root, "header-stop"), "Stop disarms the future scheduler");
    wait_until(disabled, find(w.root, "header-pause"), "stopped future scheduler cannot pause"); assert_runner_idle(&w);
    preview_wait(&w, "Nearest main event", "Sıradaki program", "General verified tone", TRUE);
    select_queue(&w, "1"); preview_wait(&w, "Nearest main event", "Sıradaki program", "Private queue preserved", FALSE);
    document_unchanged(main_saved); document_unchanged(weekly_saved); document_unchanged(manual_saved); document_unchanged(private_saved); playlist_unchanged(playlist);
    radios_window_free(w.ui); radios_runtime_free(w.runtime); while (g_main_context_iteration(NULL, FALSE)) {}
    document_unchanged(main_saved); document_unchanged(weekly_saved); document_unchanged(manual_saved); document_unchanged(private_saved); playlist_unchanged(playlist);
    json_node_free(media); json_node_free(playlist); json_node_free(main_saved); json_node_free(weekly_saved); json_node_free(manual_saved); json_node_free(private_saved);
}
typedef struct { guint count; gint64 previous, longest; } Heartbeat;
static gboolean heartbeat_tick(gpointer value)
{
    Heartbeat *heartbeat = value; gint64 now = g_get_monotonic_time();
    if (heartbeat->previous) heartbeat->longest = MAX(heartbeat->longest, now - heartbeat->previous);
    heartbeat->previous = now; ++heartbeat->count; return G_SOURCE_CONTINUE;
}
static void responsive_pump(gint64 until)
{
    while (g_get_monotonic_time() < until) {
        g_main_context_iteration(NULL, FALSE); g_usleep(1000);
    }
}
static void test_many_saved_schedules_remain_responsive(void)
{
    Window w = window_open();
    JsonNode *media = media_import(&w), *playlist = playlist_create(media);
    gint64 today = (g_get_real_time() / G_TIME_SPAN_DAY) * G_TIME_SPAN_DAY;
    JsonNode *calendar_node = calendar(playlist, today + G_TIME_SPAN_DAY + 10 * G_TIME_SPAN_MINUTE, "Many schedules next");
    JsonObject *calendar_config = json_node_get_object(calendar_node);
    JsonArray *events = json_object_get_array_member(calendar_config, "events");
    /* 174 genuine saved documents, each with 20 valid event windows. These
     * exist only in run_audio_ui.py's private fixture database. */
    for (guint i = 1; i < 20; ++i) {
        JsonNode *event = copy(json_array_get_element(events, 0));
        JsonObject *object = json_node_get_object(event); unique_id(object, "id");
        unique_id(json_array_get_object_element(json_object_get_array_member(object, "rotations"), 0), "id");
        json_object_set_int_member(object, "start_minute", 10 + i * 2);
        json_object_set_int_member(object, "end_minute", 11 + i * 2);
        json_array_add_element(events, event);
    }
    GDateTime *tomorrow = g_date_time_new_from_unix_utc((today + G_TIME_SPAN_DAY) / G_USEC_PER_SEC);
    GDateTime *monday = g_date_time_add_days(tomorrow, 1 - g_date_time_get_day_of_week(tomorrow));
    gchar *anchor = g_date_time_format(monday, "%Y-%m-%d"); GError *error = NULL;
    JsonNode *weekly = radios_weekly_programme_snapshot(calendar_config, anchor, FALSE, &error);
    g_assert_no_error(error); g_assert_nonnull(weekly);
    g_date_time_unref(monday); g_date_time_unref(tomorrow); g_free(anchor); json_node_free(calendar_node);
    gchar *first_id = NULL, *next_title = NULL;
    for (guint i = 0; i < 174; ++i) {
        gchar *title = g_strdup_printf("Many schedules candidate %u", i);
        JsonObject *first = json_array_get_object_element(json_object_get_array_member(
            json_object_get_object_member(json_node_get_object(weekly), "calendar"), "events"), 0);
        json_object_set_string_member(first, "title", title);
        JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body);
        json_object_set_string_member(object, "kind", "weekly_programme");
        json_object_set_string_member(object, "name", title); json_object_set_member(object, "config", copy(weekly)); unique_key(object);
        JsonNode *saved = request("/v1/workflows/desktop/put", body);
        const gchar *saved_id = radios_json_string(json_node_get_object(saved), "_id", "");
        if (!first_id || g_ascii_strcasecmp(saved_id, first_id) < 0) {
            g_free(first_id); first_id = g_strdup(saved_id); g_free(next_title); next_title = g_strdup(title);
        }
        json_node_free(saved); json_node_free(body); g_free(title);
    }
    json_node_free(weekly);
    Heartbeat heartbeat = {0}; guint timer = g_timeout_add(10, heartbeat_tick, &heartbeat);
    JsonNode *main = calendar(playlist, today, "Many schedules current main");
    JsonObject *current = json_array_get_object_element(json_object_get_array_member(json_node_get_object(main), "events"), 0);
    json_object_set_int_member(current, "end_minute", 1440);
    JsonNode *saved_main = document_save("automation_calendar", "main", main); json_node_free(main);
    radios_data_refresh(w.ctx);
    GtkWidget *page = find(w.root, "broadcast-page");
    RadioSPlayerWeekly *driver = page ? g_object_get_data(G_OBJECT(page), "radios-general-programme-driver") : NULL;
    /* The driver is attached to the broadcast root, whose widget name can
     * differ between themes. Locate it through the normal widget tree. */
    if (!driver) {
        GQueue pending = G_QUEUE_INIT; g_queue_push_tail(&pending, w.root);
        while (!g_queue_is_empty(&pending) && !driver) {
            GtkWidget *widget = g_queue_pop_head(&pending);
            driver = g_object_get_data(G_OBJECT(widget), "radios-general-programme-driver");
            if (GTK_IS_CONTAINER(widget)) {
                GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
                for (GList *it = children; it; it = it->next) g_queue_push_tail(&pending, it->data);
                g_list_free(children);
            }
        }
        g_queue_clear(&pending);
    }
    g_assert_nonnull(driver); radios_player_weekly_general_refresh(driver);
    gboolean ready = FALSE; gint64 deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;
    while (!ready && g_get_monotonic_time() < deadline) {
        RadioSPlayerWeeklySnapshot snapshot = {0}; radios_player_weekly_get_snapshot(driver, &snapshot);
        ready = !g_strcmp0(snapshot.current_event_name, "Many schedules current main") && !g_strcmp0(snapshot.next_event_name, next_title);
        responsive_pump(g_get_monotonic_time() + 20 * G_TIME_SPAN_MILLISECOND);
    }
    g_assert_true(ready); assert_runner_idle(&w);
    guint before = heartbeat.count; responsive_pump(g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND);
    g_assert_cmpuint(heartbeat.count - before, >=, 200);
    g_test_message("174 saved schedules / 3480 windows: %u GTK heartbeats, longest gap %.1f ms; main wins now and UUID tie selects next", heartbeat.count, heartbeat.longest / 1000.0);
    g_assert_cmpint(heartbeat.longest, <, 250 * G_TIME_SPAN_MILLISECOND);
    document_unchanged(saved_main); playlist_unchanged(playlist); assert_runner_idle(&w);
    /* Close during a fresh discovery. Its worker/API completion must never
     * access the destroyed owner/context or restart the accepted intent. */
    radios_player_weekly_general_refresh(driver);
    responsive_pump(g_get_monotonic_time() + 280 * G_TIME_SPAN_MILLISECOND);
    radios_window_free(w.ui); radios_runtime_free(w.runtime);
    responsive_pump(g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND); g_source_remove(timer);
    json_node_free(media); json_node_free(playlist); json_node_free(saved_main); g_free(first_id); g_free(next_title);
}
typedef struct { JsonNode *projection; guint count; } SchemaEntryPreview;
static void schema_entry_preview(JsonObject *programme, gboolean running, gpointer value)
{
    SchemaEntryPreview *capture = value; g_assert_false(running);
    g_clear_pointer(&capture->projection, json_node_free);
    if (!programme) return;
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, programme);
    capture->projection = copy(node); json_node_free(node); ++capture->count;
}
static gboolean schema_entry_ready(gpointer value)
{
    SchemaEntryPreview *capture = value;
    if (!capture->projection) return FALSE;
    JsonArray *events = json_object_get_array_member(json_node_get_object(capture->projection), "events");
    return events && json_array_get_length(events) &&
        json_object_has_member(json_array_get_object_element(events, 0), "programme_entry");
}
/* Use the authenticated general driver and its native prepared-plan preview:
 * a fixed base-order override used to silently disable every alternative. */
static void test_schema_entry_alternative_survives_general_driver(void)
{
    Window w = window_open(); JsonNode *media = media_import(&w), *playlist = playlist_create(media);
    gint64 next_day = (g_get_real_time() / G_TIME_SPAN_DAY + 1) * G_TIME_SPAN_DAY;
    JsonNode *calendar_node = calendar(playlist, next_day + 10 * G_TIME_SPAN_MINUTE, "Schema weighted entry preview");
    JsonObject *definition = json_array_get_object_element(json_object_get_array_member(json_node_get_object(calendar_node), "rotation_library"), 0);
    JsonArray *sources = json_object_get_array_member(definition, "sources");
    JsonNode *second = copy(json_array_get_element(sources, 0)); unique_id(json_node_get_object(second), "id"); json_array_add_element(sources, second);
    JsonObject *settings = json_object_get_object_member(definition, "settings"), *entry = json_object_new(), *transition = json_object_new(), *alternative = json_object_new();
    json_object_set_boolean_member(settings, "automation_schema", TRUE);
    json_object_set_int_member(transition, "mode", 1); json_object_set_int_member(transition, "overlap_ms", 1300);
    json_object_set_int_member(entry, "source_order", 1); json_object_set_object_member(entry, "transition", transition);
    json_object_set_int_member(alternative, "source_order", 2); json_object_set_int_member(alternative, "weight", 100);
    JsonArray *alternatives = json_array_new(); json_array_add_object_element(alternatives, alternative);
    json_object_set_array_member(entry, "alternatives", alternatives); json_object_set_object_member(settings, "programme_entry", entry);
    JsonNode *saved = document_save("automation_calendar", "main", calendar_node); json_node_free(calendar_node);
    SchemaEntryPreview capture = {0}; RadioSPlayerWeekly *driver = radios_player_weekly_new_general(w.ctx, w.root);
    g_assert_nonnull(driver); radios_player_weekly_general_set_queue(driver, schema_entry_preview, &capture);
    radios_player_weekly_general_refresh(driver); wait_until(schema_entry_ready, &capture, "weighted schema entry in real general driver preview");
    JsonObject *first = json_array_get_object_element(json_object_get_array_member(json_node_get_object(capture.projection), "events"), 0);
    JsonObject *hint = json_object_get_object_member(first, "programme_entry");
    g_assert_cmpint(radios_json_int(hint, "source_order", 0), ==, 2);
    g_assert_cmpint(radios_json_int(hint, "alternative_index", -1), ==, 0);
    JsonObject *chosen = json_object_get_object_member(hint, "transition");
    g_assert_cmpint(radios_json_int(chosen, "mode", -1), ==, 1); g_assert_cmpint(radios_json_int(chosen, "overlap_ms", -1), ==, 1300);
    RadioSPlayerWeeklySnapshot snapshot = {0}; radios_player_weekly_get_snapshot(driver, &snapshot);
    g_assert_cmpuint(snapshot.entry_source_order, ==, 2); g_assert_cmpstr(snapshot.entry_text, ==, "Giriş: 2. sıra");
    document_unchanged(saved); playlist_unchanged(playlist); assert_runner_idle(&w);
    radios_player_weekly_general_set_queue(driver, NULL, NULL); radios_player_weekly_free(driver);
    g_clear_pointer(&capture.projection, json_node_free); json_node_free(saved); json_node_free(media); json_node_free(playlist);
    radios_window_free(w.ui); radios_runtime_free(w.runtime); while (g_main_context_iteration(NULL, FALSE)) {}
}
/* Kılavuz §02 "Mevcut şemayı uygula" (akım B): a schema applied to the running
 * general programme replaces its remaining queue without cutting the item on
 * air; the following items come from the schema's own source, and the saved
 * calendar is not edited. Uses the broadcast page's own general driver, the
 * one the calendar's "Mevcut şemayı uygula" button finds. */
static GtkWidget *data_owner(GtkWidget *root, const gchar *key)
{
    if (g_object_get_data(G_OBJECT(root), key)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = data_owner(item->data, key);
    g_list_free(children); return found;
}
static void wait_long(Condition condition, gpointer value, gint seconds, const gchar *description)
{
    gint64 end = g_get_monotonic_time() + seconds * G_TIME_SPAN_SECOND;
    while (!condition(value) && g_get_monotonic_time() < end) pump();
    if (!condition(value)) g_error("General queue GUI timed out: %s", description);
}
static gboolean driver_playing(gpointer value)
{ RadioSPlayerWeeklySnapshot s = {0}; radios_player_weekly_get_snapshot(value, &s); return s.armed && s.state == RADIOS_PLAYER_WEEKLY_PLAYING; }
typedef struct { Window *w; RadioSPlayerWeekly *driver; const gchar *schema, *source; gchar *entry; } ApplyWait;
static gboolean runner_on_air(gpointer value)
{
    ApplyWait *a = value; JsonNode *plan = radios_playout_ui_plan_snapshot(a->w->ctx); if (!plan) return FALSE;
    g_free(a->entry); a->entry = g_strdup(radios_json_string(json_node_get_object(plan), "entry_id", "")); json_node_free(plan);
    return *a->entry != '\0';
}
static gboolean schema_follows(gpointer value)
{
    ApplyWait *a = value; RadioSPlayerWeeklySnapshot s = {0}; radios_player_weekly_get_snapshot(a->driver, &s);
    if (g_strcmp0(s.schema_name, a->schema)) return FALSE;
    JsonNode *plan = radios_playout_ui_plan_snapshot(a->w->ctx); if (!plan) return FALSE;
    JsonObject *o = json_node_get_object(plan); JsonArray *entries = json_object_get_array_member(o, "entries");
    gint64 index = radios_json_int(o, "index", 0); gboolean later = FALSE;
    for (guint i = (guint)index + 1; entries && i < json_array_get_length(entries); ++i)
        later |= !g_strcmp0(radios_json_string(json_array_get_object_element(entries, i), "source_name", ""), a->source);
    json_node_free(plan); return later;
}
static gboolean runner_stopped(gpointer value)
{ RadioSPlayoutSnapshot run = {0}; radios_playout_ui_get_snapshot(((Window *)value)->ctx, &run); return run.state == RADIOS_PLAYOUT_IDLE || run.state == RADIOS_PLAYOUT_FINISHED; }
static void test_schema_apply_running_general_programme(void)
{
    Window w = window_open(); JsonNode *media = media_import(&w);
    JsonNode *playlist = playlist_create(media), *schema_playlist = playlist_create_named(media, "Kılavuz şema kaynağı");
    gint64 minute = 60 * G_USEC_PER_SEC, now = g_get_real_time(), start = (now / minute + 1) * minute;
    if (start - now < 8 * G_USEC_PER_SEC) start += minute;
    JsonNode *calendar_node = calendar(playlist, start, "Schema apply running programme");
    JsonObject *root = json_node_get_object(calendar_node), *event = json_array_get_object_element(json_object_get_array_member(root, "events"), 0);
    json_object_set_int_member(event, "end_minute", json_object_get_int_member(event, "start_minute") + 3);
    JsonArray *library = json_object_get_array_member(root, "rotation_library"); JsonObject *base = json_array_get_object_element(library, 0);
    json_object_set_int_member(json_array_get_object_element(json_object_get_array_member(base, "sources"), 0), "interval_minutes", 0);
    JsonNode *schema_node = copy(json_array_get_element(library, 0)); JsonObject *schema = json_node_get_object(schema_node);
    unique_id(schema, "id"); json_object_set_string_member(schema, "name", "Kılavuz uygulanan şema");
    json_object_set_boolean_member(json_object_get_object_member(schema, "settings"), "automation_schema", TRUE);
    JsonObject *schema_source = json_array_get_object_element(json_object_get_array_member(schema, "sources"), 0); unique_id(schema_source, "id");
    json_object_set_string_member(schema_source, "playlist_id", radios_json_string(json_node_get_object(schema_playlist), "_id", ""));
    gchar *schema_id = g_strdup(radios_json_string(schema, "id", "")); json_array_add_element(library, schema_node);
    JsonNode *saved = document_save("automation_calendar", "main", calendar_node); json_node_free(calendar_node); radios_data_refresh(w.ctx);
    GtkWidget *owner = data_owner(w.root, "radios-general-programme-driver"); g_assert_nonnull(owner);
    RadioSPlayerWeekly *driver = g_object_get_data(G_OBJECT(owner), "radios-general-programme-driver"); g_assert_nonnull(driver);
    GError *error = NULL; g_assert_false(radios_player_weekly_general_apply_schema(driver, schema_id, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND); g_clear_error(&error);
    click(&w, "header-play");
    wait_long(driver_playing, driver, 85, "the general programme owns PCM at its slot start");
    ApplyWait apply = {&w, driver, "Kılavuz uygulanan şema", "Kılavuz şema kaynağı", NULL};
    wait_long(runner_on_air, &apply, 10, "an item of the running programme is on air");
    gchar *on_air = g_strdup(apply.entry); gboolean accepted = FALSE; gint64 retry_end = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    do { g_clear_error(&error); accepted = radios_player_weekly_general_apply_schema(driver, schema_id, &error); if (!accepted) pump(); }
    while (!accepted && error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PENDING) && g_get_monotonic_time() < retry_end);
    g_assert_no_error(error); g_assert_true(accepted);
    wait_long(schema_follows, &apply, 20, "the remaining queue follows the applied schema's source");
    /* The item on air was not cut to the schema: it is still part of the accepted plan,
     * or an item of the original source was still sounding when the replacement committed. */
    JsonNode *plan = radios_playout_ui_plan_snapshot(w.ctx); g_assert_nonnull(plan);
    JsonObject *accepted_plan = json_node_get_object(plan); JsonArray *entries = json_object_get_array_member(accepted_plan, "entries"); gboolean kept = FALSE;
    for (guint i = 0; entries && i < json_array_get_length(entries); ++i)
        kept |= !g_strcmp0(radios_json_string(json_array_get_object_element(entries, i), "entry_id", ""), on_air);
    if (!kept) {
        JsonObject *current = json_array_get_object_element(entries, (guint)radios_json_int(accepted_plan, "index", 0));
        g_assert_cmpstr(radios_json_string(current, "source_name", ""), ==, "General queue source playlist");
    }
    json_node_free(plan);
    RadioSPlayoutSnapshot run = {0}; radios_playout_ui_get_snapshot(w.ctx, &run);
    g_assert_true(run.state == RADIOS_PLAYOUT_PLAYING || run.state == RADIOS_PLAYOUT_OVERLAP || run.state == RADIOS_PLAYOUT_WAITING);
    RadioSPlayerWeeklySnapshot snapshot = {0}; radios_player_weekly_get_snapshot(driver, &snapshot);
    g_assert_true(snapshot.armed); g_assert_cmpstr(snapshot.schema_name, ==, "Kılavuz uygulanan şema");
    document_unchanged(saved); playlist_unchanged(playlist); playlist_unchanged(schema_playlist);
    click(&w, "header-stop"); wait_long(runner_stopped, &w, 10, "Stop ends the general programme's PCM run");
    g_free(on_air); g_free(apply.entry); g_free(schema_id);
    json_node_free(saved); json_node_free(media); json_node_free(playlist); json_node_free(schema_playlist);
    radios_window_free(w.ui); radios_runtime_free(w.runtime); while (g_main_context_iteration(NULL, FALSE)) {}
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc; g_test_init(&argc, &argv, NULL); if (!gtk_init_check(&argc, &argv)) return 77;
    GError *error = NULL; g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    application = gtk_application_new("org.radios.GeneralQueueAcceptance", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/general-queue/nearest-saved-preview-private-isolation-header-waiting-controls", test_general_queue);
    g_test_add_func("/general-queue/174-schedules-responsive-current-next-owner-cancel", test_many_saved_schedules_remain_responsive);
    g_test_add_func("/general-queue/schema-entry-weighted-alternative-assigned-transition", test_schema_entry_alternative_survives_general_driver);
    g_test_add_func("/general-queue/schema-apply-running-programme", test_schema_apply_running_general_programme);
    gint result = g_test_run(); radios_data_client_unref(reader); g_object_unref(application); radios_config_clear(&config); return result;
}
