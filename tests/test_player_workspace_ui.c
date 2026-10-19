#include "radios/window.h"
#include "radios/player_workspace.h"
#include "radios/automation_scope_ui.h"
#include "radios/audio_ui.h"
#include "radios/audio.h"
#include "radios/programme_model.h"
#include "radios/player_tree_ui.h"
#include <glib/gstdio.h>
#include <string.h>

static RadioSConfig config;
static GtkApplication *application;
static RadioSDataClient *reader;
typedef struct { RadioSWindow *window; RadioSRuntime *runtime; RadioSViewContext *ctx; } Window;
typedef gboolean (*Condition)(gpointer);

static void pump(void)
{ while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void run_for(guint milliseconds)
{
    gint64 deadline = g_get_monotonic_time() + milliseconds * 1000;
    while (g_get_monotonic_time() < deadline) pump();
}
static void wait_until(Condition condition, gpointer data, const gchar *description)
{
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!condition(data) && g_get_monotonic_time() < deadline) pump();
    if (!condition(data)) g_error("Player workspace acceptance timed out: %s", description);
}
typedef struct { const gchar *path; JsonNode *body, *result; GError *error; gint done; } Request;
static gpointer request_work(gpointer value)
{
    Request *task = value;
    task->result = radios_data_client_request_sync(reader, "POST", task->path, task->body, NULL, &task->error);
    g_atomic_int_set(&task->done, 1); return NULL;
}
static gboolean request_done(gpointer data)
{ return g_atomic_int_get(&((Request *)data)->done) != 0; }
static JsonNode *request(const gchar *path, JsonNode *body, GError **error)
{
    for (guint attempt = 0; ; ++attempt) {
        Request task = {.path = path, .body = body};
        GThread *worker = g_thread_new("workspace-private-DB", request_work, &task);
        wait_until(request_done, &task, "authenticated private DB response"); g_thread_join(worker);
        /* Many acceptance cases share this private service. An explicit 429
         * rejects the fixture request; preserve its idempotency key on retry. */
        if (attempt < 3 && g_error_matches(task.error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED)) {
            g_clear_error(&task.error); g_clear_pointer(&task.result, json_node_free);
            run_for(MAX(1u, radios_data_client_retry_delay(reader)) * 1000); continue;
        }
        if (task.error) g_propagate_error(error, task.error);
        return task.result;
    }
}
static JsonNode *body_new(void)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, json_object_new());
    json_object_set_string_member(json_node_get_object(node), "station_id", radios_data_client_station_id(reader));
    return node;
}
static JsonNode *list(const gchar *kind)
{
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "kind", kind);
    GError *error = NULL; JsonNode *result = request("/v1/workflows/desktop/list", body, &error);
    g_assert_no_error(error); g_assert_nonnull(result); json_node_free(body); return result;
}
static JsonObject *record_named(JsonNode *listed, const gchar *name)
{
    JsonArray *rows = json_object_get_array_member(json_node_get_object(listed), "records");
    for (guint i = 0; i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        if (g_str_equal(radios_json_string(row, "name", ""), name)) return row;
    }
    return NULL;
}
static JsonNode *put(const gchar *kind, const gchar *name, JsonObject *configuration, JsonObject *previous, GError **error)
{
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body);
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    json_object_set_string_member(object, "kind", kind); json_object_set_string_member(object, "name", name);
    json_object_set_object_member(object, "config", radios_programme_clone_object(configuration));
    if (previous) {
        json_object_set_string_member(object, "document_id", radios_json_string(previous, "_id", ""));
        json_object_set_int_member(object, "expected_revision", radios_json_int(previous, "_rev", 0));
    }
    JsonNode *result = request("/v1/workflows/desktop/put", body, error); json_node_free(body); return result;
}
static void reset_private_definitions(void)
{
    JsonNode *existing = list("live_players");
    JsonArray *rows = json_object_get_array_member(json_node_get_object(existing), "records");
    for (guint i = 0; i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body); gchar *key = g_uuid_string_random();
        json_object_set_string_member(object, "kind", "live_players");
        json_object_set_string_member(object, "document_id", radios_json_string(row, "_id", ""));
        json_object_set_int_member(object, "expected_revision", radios_json_int(row, "_rev", 0));
        json_object_set_string_member(object, "idempotency_key", key); g_free(key);
        GError *error = NULL; JsonNode *result = request("/v1/workflows/desktop/delete", body, &error);
        g_assert_no_error(error); g_assert_nonnull(result); json_node_free(result); json_node_free(body);
    }
    json_node_free(existing);
    JsonObject *legacy = json_object_new(); JsonArray *slots = json_array_new(); JsonObject *names = json_object_new();
    json_object_set_int_member(legacy, "deck_count", 3);
    json_array_add_int_element(slots, 0); json_array_add_int_element(slots, 2); json_array_add_int_element(slots, 4);
    json_object_set_array_member(legacy, "slots", slots);
    json_object_set_string_member(names, "1", "Eski A"); json_object_set_string_member(names, "3", "Eski B");
    json_object_set_string_member(names, "5", "Eski bağımsız"); json_object_set_object_member(legacy, "names", names);
    JsonNode *channels = list("channel"); GError *error = NULL;
    JsonNode *saved = put("channel", "Yayın oynatıcıları", legacy, record_named(channels, "Yayın oynatıcıları"), &error);
    g_assert_no_error(error); g_assert_nonnull(saved); json_node_free(saved); json_node_free(channels); json_object_unref(legacy);
}
static gboolean window_ready(gpointer data)
{
    Window *window = data;
    return radios_data_connected(window->ctx) && radios_player_workspace_ready(window->ctx->players) &&
        radios_audio_ui_ready(window->ctx, RADIOS_AUDIO_UI_MAIN) &&
        radios_audio_ui_ready(window->ctx, RADIOS_AUDIO_UI_CUE);
}
static Window window_open(void)
{
    Window window = {0}; GError *error = NULL; window.runtime = radios_runtime_new(64, 128);
    window.window = radios_window_new(application, &config, window.runtime, FALSE, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(window.window);
    window.ctx = radios_window_view_context(window.window); g_assert_nonnull(window.ctx->players);
    radios_window_present(window.window); wait_until(window_ready, &window, "real DB, persisted definitions and owned null outputs");
    return window;
}
static void window_close(Window *window)
{
    radios_window_free(window->window); radios_runtime_free(window->runtime);
    *window = (Window){0}; run_for(50);
}
static void assert_silent(Window *window)
{
    for (guint deck = 0; deck < 32; ++deck) {
        RadioSAudioUiSnapshot audio = {0}; g_assert_true(radios_audio_ui_get_snapshot(window->ctx, deck, &audio));
        g_assert_cmpint(audio.state, !=, RADIOS_AUDIO_UI_PLAYING); g_assert_cmpint(audio.state, !=, RADIOS_AUDIO_UI_PAUSED);
        RadioSPlayoutSnapshot run = {0}; g_assert_false(radios_player_workspace_snapshot(window->ctx->players, deck, &run));
    }
}
static JsonNode *wait_saved(JsonObject *configuration)
{
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    do {
        JsonNode *result = list("live_players"); JsonObject *row = record_named(result, "main");
        if (row && json_object_equal(json_object_get_object_member(row, "config"), configuration)) return result;
        json_node_free(result); run_for(100);
    } while (g_get_monotonic_time() < deadline);
    g_error("Player workspace exact topology autosave was not acknowledged"); return NULL;
}
static void test_legacy_load(void)
{
    reset_private_definitions(); Window window = window_open();
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(window.ctx->players);
    g_assert_cmpuint(radios_player_topology_count(topology), ==, 3);
    g_assert_true(radios_player_workspace_contains(window.ctx->players, 4));
    g_assert_false(radios_player_workspace_contains(window.ctx->players, 1));
    g_assert_cmpstr(topology->players[2].name, ==, "Eski B"); g_assert_cmpuint(topology->cluster_count, ==, 0);
    JsonNode *definitions = list("live_players");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(definitions), "records")), ==, 0);
    json_node_free(definitions); assert_silent(&window); window_close(&window);
}
static JsonNode *weekly(const gchar *name, const gchar *event_title)
{
    JsonObject *configuration = json_object_new(), *calendar = json_object_new(), *event = json_object_new();
    JsonArray *events = json_array_new(); gchar *id = g_uuid_string_random();
    json_object_set_int_member(configuration, "schema", 1); json_object_set_string_member(configuration, "week_anchor", "2026-10-05");
    json_object_set_int_member(configuration, "week_count", 1); json_object_set_boolean_member(configuration, "repeat", FALSE);
    json_object_set_int_member(calendar, "schema", 1); json_object_set_array_member(calendar, "events", events);
    json_object_set_string_member(event, "id", id); g_free(id);
    json_object_set_string_member(event, "title", event_title); json_object_set_string_member(event, "date", "2026-10-06");
    json_object_set_int_member(event, "start_minute", 540); json_object_set_int_member(event, "end_minute", 600);
    json_object_set_string_member(event, "color", "#123456"); json_object_set_array_member(event, "rotations", json_array_new());
    json_array_add_object_element(events, event); json_object_set_object_member(configuration, "calendar", calendar);
    GError *error = NULL; JsonNode *record = put("weekly_programme", name, configuration, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(record); json_object_unref(configuration); return record;
}
static JsonObject *programme(const gchar *title, const gchar *event_title)
{
    JsonObject *configuration = json_object_new(), *event = json_object_new(); JsonArray *events = json_array_new();
    gchar *id = g_uuid_string_random();
    json_object_set_string_member(configuration, "title", title); json_object_set_string_member(configuration, "date", "2026-10-06");
    json_object_set_string_member(configuration, "note", ""); json_object_set_array_member(configuration, "events", events);
    json_object_set_string_member(event, "id", id); g_free(id);
    json_object_set_string_member(event, "title", event_title); json_object_set_string_member(event, "kind", "music");
    const gchar *keys[] = {"artist", "album", "version_label", "primary_file", "backup_file", "note", "start_time"};
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) json_object_set_string_member(event, keys[i], "");
    json_object_set_int_member(event, "duration_ms", 1000); json_array_add_object_element(events, event); return configuration;
}
static void set_programme(Window *window, const RadioSAutomationScope *scope, JsonObject *configuration)
{
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    do {
        GError *error = NULL;
        if (radios_player_workspace_set_programme(window->ctx->players, scope, configuration, &error)) { g_assert_no_error(error); return; }
        g_clear_error(&error); run_for(100);
    } while (g_get_monotonic_time() < deadline);
    g_error("Scoped programme document never became ready");
}
static JsonNode *project(Window *window, const RadioSAutomationScope *scope)
{
    GError *error = NULL; JsonNode *projection = radios_player_workspace_projection(window->ctx->players, scope, &error);
    g_assert_no_error(error); g_assert_nonnull(projection); return projection;
}
typedef struct { Window *window; const RadioSAutomationScope *scope; } ProjectionReady;
static gboolean projection_ready(gpointer data)
{
    ProjectionReady *condition = data; JsonNode *node = project(condition->window, condition->scope);
    JsonArray *players = json_object_get_array_member(json_node_get_object(node), "players");
    gboolean ready = json_array_get_length(players) > 0;
    for (guint i = 0; ready && i < json_array_get_length(players); ++i) {
        JsonObject *player = json_array_get_object_element(players, i);
        ready = json_object_get_boolean_member(player, "queue_ready") && json_object_has_member(player, "programme");
    }
    json_node_free(node); return ready;
}
static void assert_event(JsonObject *configuration, const gchar *title)
{
    g_assert_nonnull(configuration);
    JsonArray *events = json_object_get_array_member(configuration, "events"); g_assert_cmpuint(json_array_get_length(events), ==, 1);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(events, 0), "title", ""), ==, title);
}
typedef struct { JsonNode *result; GError *error; guint callbacks, destroys; } Fetch;
static void fetched(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    (void)ctx; (void)owner; Fetch *fetch = data; ++fetch->callbacks;
    if (result) fetch->result = json_node_copy(result);
    if (error) fetch->error = g_error_copy(error);
}
static void fetch_destroyed(gpointer data) { ++((Fetch *)data)->destroys; }
static gboolean fetch_done(gpointer data) { return ((Fetch *)data)->destroys > 0; }
static void test_scope_persistence(void)
{
    reset_private_definitions();
    /* Fixture mutations finish before the window can submit asynchronous
     * topology autosaves; desktop writes compare the global store sequence. */
    JsonNode *shared = weekly("Küme şeması", "Küme event"), *individual = weekly("Tek şema", "Tek event");
    Window window = window_open(); GError *error = NULL;
    RadioSPlayerTopology topology = *radios_player_workspace_topology(window.ctx->players); gchar cluster[37];
    g_assert_true(radios_player_topology_add_cluster(&topology, "Müzik kümesi", cluster, &error));
    g_assert_true(radios_player_topology_set_cluster(&topology, 0, cluster, &error));
    g_assert_true(radios_player_topology_set_cluster(&topology, 2, cluster, &error));
    g_strlcpy(topology.players[0].name, "Küme A", sizeof topology.players[0].name);
    g_strlcpy(topology.players[2].name, "Küme B", sizeof topology.players[2].name);
    g_strlcpy(topology.players[4].name, "Tek kanal", sizeof topology.players[4].name);
    g_assert_true(radios_player_topology_set_autodj(&topology, 0, RADIOS_AUTODJ_TRANSITIONS, 2, &error));
    g_assert_true(radios_player_workspace_replace(window.ctx->players, &topology, &error)); g_assert_no_error(error);
    RadioSAutomationScope cluster_scope = {.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER};
    g_strlcpy(cluster_scope.cluster_id, cluster, sizeof cluster_scope.cluster_id);
    RadioSAutomationScope player = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0};
    RadioSAutomationScope independent = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 4};
    RadioSAutomationScope aggregate = {.kind = RADIOS_AUTOMATION_SCOPE_UNCLUSTERED};
    g_assert_true(radios_player_workspace_assign(window.ctx->players, &cluster_scope, radios_json_string(json_node_get_object(shared), "_id", ""), &error));
    g_assert_true(radios_player_workspace_assign(window.ctx->players, &player, radios_json_string(json_node_get_object(individual), "_id", ""), &error));
    g_assert_no_error(error);
    topology = *radios_player_workspace_topology(window.ctx->players); JsonObject *expected = radios_player_topology_to_json(&topology);
    JsonNode *saved = wait_saved(expected); JsonObject *row = record_named(saved, "main");
    g_assert_true(json_object_equal(json_object_get_object_member(row, "config"), expected));
    g_assert_cmpstr(radios_player_topology_automation(&topology, 0), ==, radios_json_string(json_node_get_object(individual), "_id", ""));
    g_assert_cmpstr(radios_player_topology_automation(&topology, 2), ==, radios_json_string(json_node_get_object(shared), "_id", ""));
    RadioSAutomationBinding captured;
    g_assert_true(radios_automation_scope_resolve(&topology, &player, &captured, &error));
    Fetch fetch = {0};
    g_assert_true(radios_automation_scope_fetch(window.ctx, GTK_WIDGET(window.ctx->parent), captured.automation_id,
        radios_json_int(json_node_get_object(individual), "_rev", 0), fetched, &fetch, fetch_destroyed));
    wait_until(fetch_done, &fetch, "exact saved schema read and revision reread");
    g_assert_cmpuint(fetch.callbacks, ==, 1); g_assert_cmpuint(fetch.destroys, ==, 1); g_assert_no_error(fetch.error);
    JsonObject *loaded_config = json_object_get_object_member(json_node_get_object(fetch.result), "config");
    JsonObject *calendar = json_object_get_object_member(loaded_config, "calendar");
    assert_event(calendar, "Tek event"); json_node_free(fetch.result);
    JsonObject *a = programme("A sırası", "A event"), *b = programme("Tek sırası", "Tek event"), *c = programme("Küme sırası", "Ortak event");
    JsonArray *old_pair = json_array_new(); json_array_add_int_element(old_pair, 1); json_array_add_int_element(old_pair, 5);
    json_object_set_array_member(a, "autodj_players", old_pair); json_object_set_boolean_member(a, "beat_sync", TRUE);
    set_programme(&window, &player, a); set_programme(&window, &independent, b); set_programme(&window, &cluster_scope, c);
    JsonObject *unchanged = radios_player_topology_to_json(radios_player_workspace_topology(window.ctx->players));
    g_assert_true(json_object_equal(expected, unchanged)); json_object_unref(unchanged);
    JsonNode *projection = project(&window, &player); JsonObject *object = json_node_get_object(projection);
    JsonArray *players = json_object_get_array_member(object, "players"); g_assert_cmpuint(json_array_get_length(players), ==, 1);
    JsonObject *first = json_array_get_object_element(players, 0); g_assert_cmpstr(radios_json_string(first, "name", ""), ==, "Küme A");
    JsonObject *queue = json_object_get_object_member(first, "programme"); assert_event(queue, "A event");
    g_assert_false(json_object_has_member(queue, "autodj_players")); g_assert_false(json_object_has_member(queue, "beat_sync"));
    g_assert_cmpint(radios_json_int(queue, "queue_player", 0), ==, 1); json_node_free(projection);
    projection = project(&window, &aggregate); object = json_node_get_object(projection);
    g_assert_cmpstr(radios_json_string(object, "name", ""), ==, "Kümesizler"); players = json_object_get_array_member(object, "players");
    g_assert_cmpuint(json_array_get_length(players), ==, 1); first = json_array_get_object_element(players, 0);
    g_assert_cmpint(radios_json_int(first, "player", 0), ==, 5); assert_event(json_object_get_object_member(first, "programme"), "Tek event");
    json_node_free(projection);
    projection = project(&window, &cluster_scope); object = json_node_get_object(projection);
    g_assert_cmpstr(radios_json_string(object, "name", ""), ==, "Müzik kümesi");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(object, "players")), ==, 2);
    assert_event(json_object_get_object_member(object, "programme"), "Ortak event"); json_node_free(projection);
    assert_silent(&window);
    g_assert_true(radios_player_topology_set_autodj(&topology, 0, RADIOS_AUTODJ_SEMI, -1, &error));
    g_assert_true(radios_player_workspace_replace(window.ctx->players, &topology, &error)); g_assert_no_error(error);
    JsonObject *empty = json_object_new();
    g_assert_false(radios_player_workspace_start_prepared(window.ctx->players, &captured, empty, RADIOS_AUTODJ_TRANSITIONS, &error));
    g_assert_nonnull(error); g_clear_error(&error); json_object_unref(empty); assert_silent(&window);
    JsonObject *updated = radios_player_topology_to_json(&topology); JsonNode *latest = wait_saved(updated); json_node_free(latest);
    window_close(&window); window = window_open();
    g_assert_cmpstr(radios_player_workspace_topology(window.ctx->players)->clusters[0].name, ==, "Müzik kümesi");
    g_assert_cmpstr(radios_player_topology_automation(radios_player_workspace_topology(window.ctx->players), 0), ==,
        radios_json_string(json_node_get_object(individual), "_id", ""));
    ProjectionReady ready = {&window, &player}; wait_until(projection_ready, &ready, "individual programme reread after window reopen");
    projection = project(&window, &player); first = json_array_get_object_element(json_object_get_array_member(json_node_get_object(projection), "players"), 0);
    assert_event(json_object_get_object_member(first, "programme"), "A event"); json_node_free(projection);
    assert_silent(&window); window_close(&window);
    json_node_free(shared); json_node_free(individual); json_node_free(saved);
    json_object_unref(expected); json_object_unref(updated); json_object_unref(a); json_object_unref(b); json_object_unref(c);
}
static GtkWidget *reload_dialog(void)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *result = NULL;
    for (GList *item = windows; item && !result; item = item->next)
        if (g_str_equal(gtk_widget_get_name(item->data), "database-reload-dialog")) result = item->data;
    g_list_free(windows); return result;
}
static gboolean conflict_dialog_ready(gpointer unused) { (void)unused; return reload_dialog() != NULL; }
typedef struct { Window *window; const gchar *name; } LoadedName;
static gboolean name_loaded(gpointer data)
{
    LoadedName *condition = data; const RadioSPlayerTopology *topology = radios_player_workspace_topology(condition->window->ctx->players);
    return topology && g_str_equal(topology->players[0].name, condition->name);
}
static void test_cas_conflict(void)
{
    reset_private_definitions(); Window window = window_open(); GError *error = NULL;
    RadioSPlayerTopology topology = *radios_player_workspace_topology(window.ctx->players);
    g_assert_true(radios_player_workspace_replace(window.ctx->players, &topology, &error)); g_assert_no_error(error);
    JsonObject *configuration = radios_player_topology_to_json(&topology); JsonNode *saved = wait_saved(configuration);
    JsonObject *original = record_named(saved, "main"); run_for(100);
    g_strlcpy(topology.players[0].name, "Sunucudaki yeni ad", sizeof topology.players[0].name);
    JsonObject *server_configuration = radios_player_topology_to_json(&topology);
    JsonNode *server = put("live_players", "main", server_configuration, original, &error); g_assert_no_error(error); g_assert_nonnull(server);
    JsonNode *rejected = put("live_players", "main", configuration, original, &error);
    g_assert_null(rejected); g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_CONFLICT); g_clear_error(&error);
    RadioSPlayerTopology local = *radios_player_workspace_topology(window.ctx->players);
    g_strlcpy(local.players[0].name, "Eski istemci denemesi", sizeof local.players[0].name);
    g_assert_true(radios_player_workspace_replace(window.ctx->players, &local, &error)); g_assert_no_error(error);
    wait_until(conflict_dialog_ready, NULL, "stale service autosave raises the existing CAS reload prompt");
    JsonNode *current = list("live_players"); JsonObject *row = record_named(current, "main");
    g_assert_cmpint(radios_json_int(row, "_rev", 0), ==, radios_json_int(json_node_get_object(server), "_rev", 0));
    g_assert_true(json_object_equal(json_object_get_object_member(row, "config"), server_configuration)); json_node_free(current);
    RadioSAutomationScope source_scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0};
    RadioSAutomationBinding blocked_binding;
    g_assert_true(radios_automation_scope_resolve(radios_player_workspace_topology(window.ctx->players),
        &source_scope, &blocked_binding, &error)); g_assert_no_error(error);
    g_assert_false(radios_player_workspace_start_prepared(window.ctx->players, &blocked_binding,
        NULL, RADIOS_AUTODJ_SEMI, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PENDING); g_clear_error(&error);
    /* An explicit reload request can offer a second discard prompt while the
     * first CAS conflict is unresolved. The clean legacy read must never
     * replace this draft simply because its acknowledged revision is zero. */
    radios_player_workspace_reload(window.ctx->players); run_for(450);
    LoadedName preserved = {&window, "Eski istemci denemesi"}; g_assert_true(name_loaded(&preserved));
    guint cancelled = 0;
    while (reload_dialog()) {
        gtk_dialog_response(GTK_DIALOG(reload_dialog()), GTK_RESPONSE_CANCEL); ++cancelled; pump();
    }
    g_assert_cmpuint(cancelled, >=, 1); run_for(150); g_assert_true(name_loaded(&preserved));
    current = list("live_players"); row = record_named(current, "main");
    g_assert_true(json_object_equal(json_object_get_object_member(row, "config"), server_configuration)); json_node_free(current);
    radios_player_workspace_reload(window.ctx->players);
    wait_until(conflict_dialog_ready, NULL, "fresh explicit reload still offers draft discard");
    gtk_dialog_response(GTK_DIALOG(reload_dialog()), GTK_RESPONSE_ACCEPT);
    LoadedName loaded = {&window, "Sunucudaki yeni ad"}; wait_until(name_loaded, &loaded, "operator reload adopts exact current server baseline");
    assert_silent(&window); window_close(&window);
    window = window_open(); g_assert_true(name_loaded(&(LoadedName){&window, "Sunucudaki yeni ad"})); assert_silent(&window); window_close(&window);
    json_node_free(saved); json_node_free(server); json_object_unref(configuration); json_object_unref(server_configuration);
}
static GtkWidget *widget_find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (GTK_IS_MENU_BUTTON(root)) {
        GtkWidget *popover = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(root)));
        GtkWidget *result = popover ? widget_find(popover, name) : NULL;
        if (result) return result;
    }
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = widget_find(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *card_owner(Window *window, guint deck)
{
    gchar *key = g_strdup_printf("radios-deck-owner-%u", deck);
    GtkWidget *card = g_object_get_data(G_OBJECT(window->ctx->parent), key); g_free(key); return card;
}
static GtkWidget *card_wrapper(GtkWidget *card)
{
    GtkWidget *wrapper = gtk_widget_get_parent(card);
    while (wrapper && !GTK_IS_FLOW_BOX_CHILD(wrapper)) wrapper = gtk_widget_get_parent(wrapper);
    return wrapper;
}
typedef struct { Window *window; guint32 shown; } CardsVisible;
static gboolean cards_visible(gpointer data)
{
    CardsVisible *condition = data;
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(condition->window->ctx->players);
    if (!topology) return FALSE;
    for (guint deck = 0; deck < 32; ++deck) {
        GtkWidget *card = card_owner(condition->window, deck);
        if (!topology->players[deck].enabled) { if (card) return FALSE; continue; }
        if (!card) return FALSE;
        GtkWidget *wrapper = card_wrapper(card); if (!wrapper) return FALSE;
        if (gtk_widget_get_visible(wrapper) != ((condition->shown & ((guint32)1 << deck)) != 0)) return FALSE;
    }
    return TRUE;
}
static void selected_dump(GtkWidget *selector, const gchar *label)
{
    RadioSAutomationScope scope = {0}; gboolean general = FALSE;
    if (!selector || !radios_player_tree_ui_selection(selector, &scope, &general)) {
        g_test_message("%s tree has no selected row", label); return;
    }
    g_test_message("%s selection kind=%d player=%u cluster=%s general=%d", label, scope.kind, scope.deck + 1, scope.cluster_id, general);
}
typedef struct { GtkWidget *selector; const RadioSAutomationScope *scope; } SelectedScope;
static gboolean scope_selected(gpointer data)
{
    SelectedScope *expected = data; RadioSAutomationScope selected = {0}; gboolean general = TRUE;
    if (!radios_player_tree_ui_selection(expected->selector, &selected, &general) || general ||
        selected.kind != expected->scope->kind) return FALSE;
    if (selected.kind == RADIOS_AUTOMATION_SCOPE_CLUSTER) return g_str_equal(selected.cluster_id, expected->scope->cluster_id);
    if (selected.kind == RADIOS_AUTOMATION_SCOPE_PLAYER) return selected.deck == expected->scope->deck;
    return TRUE;
}
static void selection_wait(GtkWidget *selector, const RadioSAutomationScope *scope)
{
    SelectedScope expected = {selector, scope};
    wait_until(scope_selected, &expected, "player hierarchy mirrors the selected folder or player");
}
static void cards_wait(Window *window, guint32 shown)
{
    CardsVisible expected = {window, shown};
    GtkWidget *root = GTK_WIDGET(window->ctx->parent), *definitions = widget_find(root, "live-player-definitions");
    g_test_message("Waiting for visible card mask 0x%08x", shown); selected_dump(definitions, "live manager");
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!cards_visible(&expected) && g_get_monotonic_time() < deadline) pump();
    if (cards_visible(&expected)) return;
    selected_dump(definitions, "live manager after timeout");
    GtkWidget *panel = widget_find(root, "automation-workspace");
    selected_dump(panel ? widget_find(panel, "player-tree-selector") : NULL, "left automation after timeout");
    GtkWidget *heading = widget_find(root, "live-player-selection-title");
    if (GTK_IS_LABEL(heading)) g_test_message("visible scope heading=%s", gtk_label_get_text(GTK_LABEL(heading)));
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(window->ctx->players);
    for (guint deck = 0; topology && deck < 32; ++deck) {
        GtkWidget *card = card_owner(window, deck); if (!card && !topology->players[deck].enabled) continue;
        GtkWidget *wrapper = card ? card_wrapper(card) : NULL;
        g_test_message("player=%u enabled=%d group=%s name=%s expected_visible=%d card=%p wrapper=%s visible=%d mapped=%d",
            deck + 1, topology->players[deck].enabled, topology->players[deck].cluster_id, topology->players[deck].name,
            (shown & ((guint32)1 << deck)) != 0, (void *)card, wrapper ? G_OBJECT_TYPE_NAME(wrapper) : "missing",
            wrapper ? gtk_widget_get_visible(wrapper) : -1, wrapper ? gtk_widget_get_mapped(wrapper) : -1);
    }
    g_error("Selected player/group mask 0x%08x did not match persistent card visibility", shown);
}
static gboolean ui_enabled(gpointer widget) { return gtk_widget_get_sensitive(widget); }
static void ui_click(GtkWidget *root, const gchar *name)
{
    GtkWidget *button = widget_find(root, name); g_assert_true(GTK_IS_BUTTON(button));
    wait_until(ui_enabled, button, "actual native scope management control becomes ready"); gtk_button_clicked(GTK_BUTTON(button)); pump();
}
static GtkWidget *ui_dialog(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *dialog = NULL;
    for (GList *item = windows; item && !dialog; item = item->next)
        if (g_str_equal(gtk_widget_get_name(item->data), name)) dialog = item->data;
    g_list_free(windows); return dialog;
}
static void ui_name(const gchar *name)
{
    GtkWidget *dialog = ui_dialog("player-tree-edit-dialog"); g_assert_true(GTK_IS_DIALOG(dialog));
    GtkWidget *entry = widget_find(dialog, "player-tree-name-entry"); g_assert_true(GTK_IS_ENTRY(entry));
    gtk_entry_set_text(GTK_ENTRY(entry), name); gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); pump();
    g_assert_null(ui_dialog("player-tree-edit-dialog"));
}
static void ui_move(GtkWidget *definitions, const gchar *cluster_id)
{
    ui_click(definitions, "player-tree-move"); GtkWidget *dialog = ui_dialog("player-tree-edit-dialog"); g_assert_nonnull(dialog);
    GtkWidget *target = widget_find(dialog, "player-tree-move-target"); g_assert_true(GTK_IS_COMBO_BOX(target));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(target), cluster_id));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); pump(); g_assert_null(ui_dialog("player-tree-edit-dialog"));
}
static gboolean tree_roots(GtkWidget *selector, guint count, const gchar *cluster_name)
{
    GtkWidget *tree = widget_find(selector, "player-tree"); if (!GTK_IS_TREE_VIEW(tree)) return FALSE;
    GtkTreeModel *store = gtk_tree_view_get_model(GTK_TREE_VIEW(tree));
    if (gtk_tree_model_iter_n_children(store, NULL) != (gint)count) return FALSE;
    GtkTreeIter iter; gboolean valid = gtk_tree_model_get_iter_first(store, &iter), cluster = cluster_name == NULL;
    gboolean grouped = FALSE, independent = FALSE;
    while (valid) {
        gchar *name = NULL; gtk_tree_model_get(store, &iter, 0, &name, -1);
        if (g_str_has_prefix(name, "Kümeli")) {
            grouped = TRUE;
            GtkTreeIter child;
            for (gboolean have = gtk_tree_model_iter_children(store, &child, &iter); have; have = gtk_tree_model_iter_next(store, &child)) {
                gchar *child_name = NULL; gtk_tree_model_get(store, &child, 0, &child_name, -1);
                cluster |= cluster_name && g_str_has_prefix(child_name, cluster_name); g_free(child_name);
            }
        }
        independent |= g_str_has_prefix(name, "Kümesizler"); g_free(name);
        valid = gtk_tree_model_iter_next(store, &iter);
    }
    return grouped && cluster && independent;
}
typedef struct { GtkWidget *selector; guint roots; const gchar *cluster_name; } TreeRoots;
static gboolean tree_ready(gpointer data)
{ TreeRoots *condition = data; return tree_roots(condition->selector, condition->roots, condition->cluster_name); }
static void destroyed_card(GtkWidget *widget, gpointer data)
{ (void)widget; ++*((guint *)data); }
static void assert_stable(Window *window, GtkWidget *const cards[5], GtkWidget *const wrappers[5], const guint destroyed[5])
{
    for (guint deck = 0; deck < 5; ++deck) if (cards[deck]) {
        g_assert_true(card_owner(window, deck) == cards[deck]); g_assert_cmpuint(destroyed[deck], ==, 0);
        g_assert_true(card_wrapper(cards[deck]) == wrappers[deck]);
    }
}
typedef struct { GtkWidget *label; const gchar *text; } LabelText;
static gboolean label_matches(gpointer data)
{
    LabelText *condition = data;
    return g_str_equal(gtk_label_get_text(GTK_LABEL(condition->label)), condition->text);
}
static void capture_ui(Window *window, guint width, guint height)
{
    const gchar *directory = g_getenv("RADIOS_SHOT_DIR"); if (!directory || !*directory) return;
    GtkWidget *root = GTK_WIDGET(window->ctx->parent);
    for (guint i = 0; i < 3; ++i) { gtk_window_resize(window->ctx->parent, width, height); run_for(250); }
    gint actual_width, actual_height; gtk_window_get_size(window->ctx->parent, &actual_width, &actual_height);
    g_test_message("Cluster UI requested %ux%u, allocated %dx%d", width, height, actual_width, actual_height);
    g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
    GdkPixbuf *capture = gdk_pixbuf_get_from_window(gtk_widget_get_window(root), 0, 0,
        gtk_widget_get_allocated_width(root), gtk_widget_get_allocated_height(root));
    g_assert_nonnull(capture);
    gchar *filename = g_strdup_printf("cluster-workspace-%ux%u.png", width, height), *path = g_build_filename(directory, filename, NULL);
    GError *error = NULL; g_assert_true(gdk_pixbuf_save(capture, path, "png", &error, NULL));
    g_assert_no_error(error); g_object_unref(capture);
    g_free(path); g_free(filename);
    GtkWidget *definitions = widget_find(root, "live-player-definitions");
    const gchar *controls[] = {"player-tree-add-cluster", "player-tree-add-player"};
    for (guint i = 0; i < G_N_ELEMENTS(controls); ++i) {
        GtkWidget *control = widget_find(definitions, controls[i]); g_assert_nonnull(control);
        g_assert_cmpint(gtk_widget_get_allocated_width(control), >=, 25); g_assert_cmpint(gtk_widget_get_allocated_height(control), >=, 18);
        gint x, y; g_assert_true(gtk_widget_translate_coordinates(control, definitions, 0, 0, &x, &y));
        g_assert_cmpint(x, >=, 0); g_assert_cmpint(x + gtk_widget_get_allocated_width(control), <=, gtk_widget_get_allocated_width(definitions));
    }
    GtkWidget *scroll = widget_find(definitions, "player-tree-scroll");
    g_assert_nonnull(scroll); g_assert_true(gtk_widget_is_drawable(scroll));
    const gchar *hidden[] = {"player-tree-rename", "player-tree-move", "player-tree-remove", "player-tree-reload", "player-tree-status"};
    for (guint i = 0; i < G_N_ELEMENTS(hidden); ++i) {
        GtkWidget *widget = widget_find(definitions, hidden[i]); g_assert_nonnull(widget); g_assert_false(gtk_widget_is_drawable(widget));
    }
}
static void test_visible_cluster_management(void)
{
    reset_private_definitions(); Window window = window_open();
    g_assert_true(radios_window_select_profile(window.window, "broadcast"));
    GtkWidget *root = GTK_WIDGET(window.ctx->parent), *rack = widget_find(root, "broadcast-players-pane");
    g_assert_nonnull(rack); GtkWidget *right_stack = gtk_widget_get_parent(rack); g_assert_true(GTK_IS_STACK(right_stack));
    gtk_stack_set_visible_child_name(GTK_STACK(right_stack), "players");
    GtkWidget *management = widget_find(rack, "live-player-management"); g_assert_true(GTK_IS_EXPANDER(management));
    g_assert_false(gtk_expander_get_expanded(GTK_EXPANDER(management)));
    gtk_expander_set_expanded(GTK_EXPANDER(management), TRUE); run_for(100);
    GtkWidget *definitions = widget_find(rack, "live-player-definitions"); g_assert_nonnull(definitions);
    g_assert_true(gtk_widget_get_mapped(definitions));
    TreeRoots initial = {definitions, 2, NULL}; wait_until(tree_ready, &initial, "live manager shows Kümeli and Kümesizler roots");
    GtkWidget *scroll = widget_find(definitions, "player-tree-scroll");
    g_assert_nonnull(scroll); g_assert_true(gtk_widget_is_drawable(scroll));
    const guint32 independent_start = ((guint32)1 << 0) | ((guint32)1 << 2) | ((guint32)1 << 4);
    cards_wait(&window, independent_start);
    GtkWidget *cards[5] = {0}, *wrappers[5] = {0}; guint destroyed[5] = {0};
    const guint initial_decks[] = {0, 2, 4};
    for (guint i = 0; i < G_N_ELEMENTS(initial_decks); ++i) {
        guint deck = initial_decks[i]; cards[deck] = card_owner(&window, deck); wrappers[deck] = card_wrapper(cards[deck]);
        g_signal_connect(cards[deck], "destroy", G_CALLBACK(destroyed_card), &destroyed[deck]);
    }
    ui_click(definitions, "player-tree-add-cluster"); ui_name("Gündüz yayın");
    RadioSAutomationScope cluster; gboolean general = TRUE;
    g_assert_true(radios_player_tree_ui_selection(definitions, &cluster, &general));
    g_assert_cmpint(cluster.kind, ==, RADIOS_AUTOMATION_SCOPE_CLUSTER); g_assert_false(general); cards_wait(&window, 0);
    ui_click(definitions, "player-tree-add-player"); selection_wait(definitions, &cluster);
    g_assert_true(radios_player_workspace_topology(window.ctx->players)->players[1].enabled);
    g_assert_cmpstr(radios_player_workspace_topology(window.ctx->players)->players[1].cluster_id, ==, cluster.cluster_id);
    cards_wait(&window, (guint32)1 << 1);
    cards[1] = card_owner(&window, 1); wrappers[1] = card_wrapper(cards[1]);
    g_signal_connect(cards[1], "destroy", G_CALLBACK(destroyed_card), &destroyed[1]);
    RadioSAutomationScope first = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0};
    g_assert_true(radios_player_tree_ui_select(definitions, &first)); cards_wait(&window, 1);
    ui_move(definitions, cluster.cluster_id); ui_click(definitions, "player-tree-rename"); ui_name("Ana müzik");
    g_assert_true(radios_player_tree_ui_select(definitions, &cluster)); cards_wait(&window, ((guint32)1 << 0) | ((guint32)1 << 1));
    TreeRoots clustered = {definitions, 2, "Gündüz yayın"}; wait_until(tree_ready, &clustered, "named cluster is nested under Kümeli beside Kümesizler");
    RadioSAutomationScope independent = {.kind = RADIOS_AUTOMATION_SCOPE_UNCLUSTERED};
    for (guint repeat = 0; repeat < 3; ++repeat) {
        g_assert_true(radios_player_tree_ui_select(definitions, &independent)); cards_wait(&window, ((guint32)1 << 2) | ((guint32)1 << 4));
        assert_stable(&window, cards, wrappers, destroyed);
        g_assert_true(radios_player_tree_ui_select(definitions, &cluster)); cards_wait(&window, 3);
        assert_stable(&window, cards, wrappers, destroyed);
        g_assert_true(radios_player_tree_ui_select(definitions, &first)); cards_wait(&window, 1);
        assert_stable(&window, cards, wrappers, destroyed);
    }
    GtkWidget *caption = g_object_get_data(G_OBJECT(cards[0]), "radios-player-caption-widget");
    g_assert_true(GTK_IS_LABEL(caption)); LabelText renamed = {caption, "Ana müzik"};
    wait_until(label_matches, &renamed, "saved player name updates the existing card caption");
    GtkWidget *panel = widget_find(root, "automation-workspace"); g_assert_nonnull(panel);
    GtkWidget *left_selector = widget_find(panel, "automation-workspace-selector");
    if (!left_selector) left_selector = widget_find(panel, "player-tree-selector");
    g_assert_nonnull(left_selector); TreeRoots automation = {left_selector, 2, "Gündüz yayın"};
    wait_until(tree_ready, &automation, "left automation hierarchy shows the named group under Kümeli");
    g_assert_true(radios_player_tree_ui_select(left_selector, &cluster)); selection_wait(definitions, &cluster);
    ui_click(definitions, "player-tree-add-player");
    selection_wait(definitions, &cluster); selection_wait(left_selector, &cluster);
    guint32 clustered_mask = ((guint32)1 << 0) | ((guint32)1 << 1) | ((guint32)1 << 3);
    cards_wait(&window, clustered_mask); assert_stable(&window, cards, wrappers, destroyed);
    g_assert_true(radios_player_tree_ui_select(left_selector, &first)); selection_wait(definitions, &first);
    ui_click(definitions, "player-tree-add-player");
    selection_wait(definitions, &cluster); selection_wait(left_selector, &cluster);
    clustered_mask |= (guint32)1 << 5;
    cards_wait(&window, clustered_mask); assert_stable(&window, cards, wrappers, destroyed);
    g_assert_true(radios_player_tree_ui_select(left_selector, &independent)); selection_wait(definitions, &independent);
    ui_click(definitions, "player-tree-add-player");
    selection_wait(definitions, &independent); selection_wait(left_selector, &independent);
    guint32 independent_mask = ((guint32)1 << 2) | ((guint32)1 << 4) | ((guint32)1 << 6);
    cards_wait(&window, independent_mask); assert_stable(&window, cards, wrappers, destroyed);
    RadioSAutomationScope individual = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 4};
    g_assert_true(radios_player_tree_ui_select(left_selector, &individual)); selection_wait(definitions, &individual);
    ui_click(definitions, "player-tree-add-player");
    selection_wait(definitions, &independent); selection_wait(left_selector, &independent);
    independent_mask |= (guint32)1 << 7;
    cards_wait(&window, independent_mask); assert_stable(&window, cards, wrappers, destroyed);
    g_assert_true(radios_player_tree_ui_select(left_selector, &individual)); cards_wait(&window, (guint32)1 << 4);
    ui_click(panel, "automation-workspace-edit");
    GtkWidget *queue = widget_find(root, "programme-player-selector"), *sequence = widget_find(root, "broadcast-sequence-stack");
    g_assert_true(GTK_IS_COMBO_BOX(queue)); g_assert_false(gtk_widget_get_visible(queue));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(queue)), ==, "5");
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(sequence)), ==, "editor");
    assert_stable(&window, cards, wrappers, destroyed); assert_silent(&window);
    gtk_stack_set_visible_child_name(GTK_STACK(sequence), "scopes");
    g_assert_true(radios_player_tree_ui_select(definitions, &cluster)); cards_wait(&window, clustered_mask);
    capture_ui(&window, 1920, 1080); capture_ui(&window, 1000, 740);
    const RadioSPlayerTopology *topology = radios_player_workspace_topology(window.ctx->players);
    g_assert_cmpuint(radios_player_topology_count(topology), ==, 8);
    const guint clustered_decks[] = {0, 1, 3, 5}, independent_decks[] = {2, 4, 6, 7};
    for (guint i = 0; i < G_N_ELEMENTS(clustered_decks); ++i)
        g_assert_cmpstr(topology->players[clustered_decks[i]].cluster_id, ==, cluster.cluster_id);
    for (guint i = 0; i < G_N_ELEMENTS(independent_decks); ++i)
        g_assert_cmpstr(topology->players[independent_decks[i]].cluster_id, ==, "");
    JsonObject *expected = radios_player_topology_to_json(topology); JsonNode *saved = wait_saved(expected); json_node_free(saved);
    window_close(&window); window = window_open(); root = GTK_WIDGET(window.ctx->parent);
    GtkWidget *profile = widget_find(root, "profile-broadcast-tab"); g_assert_true(GTK_IS_TOGGLE_BUTTON(profile));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(profile)));
    management = widget_find(root, "live-player-management"); g_assert_true(GTK_IS_EXPANDER(management));
    g_assert_false(gtk_expander_get_expanded(GTK_EXPANDER(management)));
    gtk_expander_set_expanded(GTK_EXPANDER(management), TRUE); run_for(100);
    definitions = widget_find(root, "live-player-definitions"); g_assert_nonnull(definitions);
    TreeRoots reopened = {definitions, 2, "Gündüz yayın"}; wait_until(tree_ready, &reopened, "persisted grouped live definitions reappear after reopen");
    saved = list("live_players"); JsonObject *reopened_record = record_named(saved, "main"); g_assert_nonnull(reopened_record);
    g_assert_true(json_object_equal(expected, json_object_get_object_member(reopened_record, "config")));
    JsonObject *reopened_topology = radios_player_topology_to_json(radios_player_workspace_topology(window.ctx->players));
    g_assert_true(json_object_equal(expected, reopened_topology)); json_object_unref(reopened_topology);
    g_assert_true(radios_player_tree_ui_select(definitions, &independent)); cards_wait(&window, independent_mask);
    g_assert_true(radios_player_tree_ui_select(definitions, &cluster)); cards_wait(&window, clustered_mask);
    g_assert_cmpstr(radios_player_workspace_topology(window.ctx->players)->players[0].name, ==, "Ana müzik");
    assert_silent(&window); window_close(&window); json_node_free(saved); json_object_unref(expected);
}
static gboolean combo_has_id(GtkWidget *combo, const gchar *id)
{
    GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(combo)); GtkTreeIter iter;
    gint column = gtk_combo_box_get_id_column(GTK_COMBO_BOX(combo));
    for (gboolean valid = gtk_tree_model_get_iter_first(model, &iter); valid; valid = gtk_tree_model_iter_next(model, &iter)) {
        gchar *value = NULL; gtk_tree_model_get(model, &iter, column, &value, -1);
        gboolean found = !g_strcmp0(value, id); g_free(value); if (found) return TRUE;
    }
    return FALSE;
}
typedef struct { Window *window; guint deck; RadioSAudioUiState state; gint sync_master; } SyncAudio;
static gboolean sync_audio_matches(gpointer value)
{
    SyncAudio *expected = value; RadioSAudioUiSnapshot snapshot = {0};
    return radios_audio_ui_get_snapshot(expected->window->ctx, expected->deck, &snapshot) &&
        snapshot.state == expected->state && snapshot.sync_master == expected->sync_master && snapshot.output_ready;
}
static gchar *sync_wave(const gchar *path)
{
    /* Thirty seconds of silent PCM keeps this test independent of the media
     * catalogue and of audible hardware; run_audio_ui supplies owned null sinks. */
    const guint frames = 30 * 48000, size = 44 + frames * 4;
    guint8 *bytes = g_malloc0(size); memcpy(bytes, "RIFF", 4); memcpy(bytes + 8, "WAVEfmt ", 8); memcpy(bytes + 36, "data", 4);
    const guint offsets32[] = {4, 16, 24, 28, 40}, values32[] = {size - 8, 16, 48000, 192000, frames * 4};
    const guint offsets16[] = {20, 22, 32, 34}, values16[] = {1, 2, 4, 16};
    for (guint i = 0; i < G_N_ELEMENTS(offsets32); ++i) { guint32 number = GUINT32_TO_LE(values32[i]); memcpy(bytes + offsets32[i], &number, 4); }
    for (guint i = 0; i < G_N_ELEMENTS(offsets16); ++i) { guint16 number = GUINT16_TO_LE(values16[i]); memcpy(bytes + offsets16[i], &number, 2); }
    GError *error = NULL; g_assert_true(g_file_set_contents(path, (const gchar *)bytes, size, &error)); g_assert_no_error(error);
    gchar *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes, size); g_free(bytes); return sha;
}
static void capture_sync_controls(Window *window, guint deck, const gchar *variable)
{
    const gchar *path = g_getenv(variable); if (!path || !*path) return;
    g_assert_true(radios_window_select_profile(window->window, "broadcast"));
    GtkWidget *rack = widget_find(GTK_WIDGET(window->ctx->parent), "broadcast-players-pane");
    gtk_stack_set_visible_child_name(GTK_STACK(gtk_widget_get_parent(rack)), "players");
    gtk_window_resize(window->ctx->parent, 1600, 1000); run_for(400);
    GtkWidget *card = card_owner(window, deck), *scroll = gtk_widget_get_ancestor(card, GTK_TYPE_SCROLLED_WINDOW);
    GtkWidget *controls = widget_find(card, "lp-independent-controls");
    if (GTK_IS_SCROLLED_WINDOW(scroll) && gtk_widget_get_visible(controls)) {
        gint x, y; g_assert_true(gtk_widget_translate_coordinates(controls, gtk_bin_get_child(GTK_BIN(scroll)), 0, 0, &x, &y));
        GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
        gtk_adjustment_set_value(adjustment, y); run_for(200);
    }
    GdkWindow *surface = gtk_widget_get_window(GTK_WIDGET(window->ctx->parent));
    GdkPixbuf *capture = gdk_pixbuf_get_from_window(surface, 0, 0, gdk_window_get_width(surface), gdk_window_get_height(surface));
    g_assert_nonnull(capture); GError *error = NULL;
    g_assert_true(gdk_pixbuf_save(capture, path, "png", &error, NULL)); g_assert_no_error(error); g_object_unref(capture);
}
static void test_sync_explicit_group_source(void)
{
    reset_private_definitions(); Window window = window_open(); GError *error = NULL;
    GtkWidget *management = widget_find(GTK_WIDGET(window.ctx->parent), "live-player-management");
    g_assert_true(GTK_IS_EXPANDER(management)); g_assert_false(gtk_expander_get_expanded(GTK_EXPANDER(management)));
    RadioSPlayerTopology topology = *radios_player_workspace_topology(window.ctx->players); gchar cluster_id[37]; guint independent_deck;
    g_assert_true(radios_player_topology_add_cluster(&topology, "SYNC kümesi", cluster_id, &error));
    g_assert_true(radios_player_topology_set_cluster(&topology, 4, cluster_id, &error));
    g_assert_true(radios_player_topology_add_player(&topology, "", &independent_deck, &error));
    g_assert_cmpuint(independent_deck, ==, 1);
    g_assert_true(radios_player_topology_set_cluster(&topology, independent_deck, cluster_id, &error));
    g_assert_true(radios_player_workspace_replace(window.ctx->players, &topology, &error)); g_assert_no_error(error);
    JsonObject *configuration = radios_player_topology_to_json(&topology); JsonNode *saved = wait_saved(configuration);
    json_node_free(saved); json_object_unref(configuration); run_for(200);
    gchar *directory = g_dir_make_tmp("radios-player-sync-XXXXXX", &error); g_assert_no_error(error);
    gchar *path = g_build_filename(directory, "silence.wav", NULL), *sha = sync_wave(path);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    const guint decks[] = {0, 1, 2, 4};
    RadioSAudioMixerConfig mixer; radios_audio_engine_get_mixer(engine, &mixer);
    for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel) mixer.channels[channel].source_deck = -1;
    for (guint i = 0; i < G_N_ELEMENTS(decks); ++i) mixer.channels[i].source_deck = (gint)decks[i];
    g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, &error)); g_assert_no_error(error);
    RadioSRhythmAnalysis rhythm = {.bpm = 120, .confidence = 1, .analyzed_ms = 10000, .manual = TRUE};
    for (guint i = 0; i < G_N_ELEMENTS(decks); ++i) {
        guint deck = decks[i]; gchar *media = g_uuid_string_random(), *version = g_uuid_string_random();
        RadioSAudioLoadSpec spec = {path, sha, media, version, 0, 0};
        g_assert_true(radios_audio_engine_load(engine, deck, &spec, &error)); g_assert_no_error(error); g_free(media); g_free(version);
        g_assert_true(radios_audio_engine_set_deck_main(engine, deck, TRUE, &error)); g_assert_no_error(error);
        SyncAudio ready = {&window, deck, RADIOS_AUDIO_UI_READY, -1}; wait_until(sync_audio_matches, &ready, "local SYNC fixture is decoded");
        RadioSAudioUiSnapshot snapshot; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, deck, &snapshot));
        g_assert_true(radios_audio_engine_set_deck_rhythm(engine, deck, snapshot.generation, &rhythm, &error)); g_assert_no_error(error);
        if (deck) { g_assert_true(radios_audio_engine_play(engine, deck, &error)); g_assert_no_error(error); }
    }
    run_for(200);
    GtkWidget *source = card_owner(&window, 0), *target = widget_find(source, "deck-0-sync-target"), *sync = widget_find(source, "deck-0-sync");
    g_assert_true(GTK_IS_COMBO_BOX(target)); g_assert_true(GTK_IS_TOGGLE_BUTTON(sync));
    g_assert_true(gtk_widget_get_visible(widget_find(source, "lp-independent-controls")));
    g_assert_true(combo_has_id(target, "2")); g_assert_false(combo_has_id(target, "1")); g_assert_false(combo_has_id(target, "4"));
    g_assert_false(gtk_widget_is_sensitive(target)); g_assert_false(gtk_widget_is_sensitive(sync));
    g_assert_true(radios_audio_engine_play(engine, 0, &error)); g_assert_no_error(error); run_for(200);
    g_assert_true(gtk_widget_is_sensitive(target)); g_assert_false(gtk_widget_is_sensitive(sync));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(target), "2")); run_for(150);
    g_assert_true(gtk_widget_is_sensitive(sync)); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sync), TRUE);
    SyncAudio linked = {&window, 0, RADIOS_AUDIO_UI_PLAYING, 2}; wait_until(sync_audio_matches, &linked, "SYNC uses the explicitly selected group peer");
    capture_sync_controls(&window, 0, "RADIOS_INDEPENDENT_CONTROLS_CAPTURE");
    GtkWidget *independent = card_owner(&window, 4), *independent_target = widget_find(independent, "deck-4-sync-target");
    g_assert_false(gtk_widget_get_visible(widget_find(independent, "lp-independent-controls")));
    g_assert_true(combo_has_id(independent_target, "1")); g_assert_false(combo_has_id(independent_target, "0")); g_assert_false(combo_has_id(independent_target, "2"));
    GtkWidget *definitions = widget_find(GTK_WIDGET(window.ctx->parent), "live-player-definitions");
    RadioSAutomationScope other = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 1};
    g_assert_true(radios_player_tree_ui_select(definitions, &other)); run_for(200);
    capture_sync_controls(&window, 1, "RADIOS_CLUSTER_CONTROLS_CAPTURE");
    g_assert_false(gtk_widget_get_visible(card_wrapper(source))); g_assert_true(sync_audio_matches(&linked));
    g_assert_true(radios_audio_engine_pause(engine, 0, &error)); g_assert_no_error(error); run_for(150);
    g_assert_false(gtk_widget_is_sensitive(sync));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sync), FALSE); run_for(150);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(sync), TRUE); run_for(150);
    RadioSAudioUiSnapshot paused; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 0, &paused));
    g_assert_cmpint(paused.state, ==, RADIOS_AUDIO_UI_PAUSED); g_assert_cmpint(paused.sync_master, ==, -1);
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(sync)));
    radios_audio_engine_unref(engine); window_close(&window);
    g_assert_cmpint(g_unlink(path), ==, 0); g_assert_cmpint(g_rmdir(directory), ==, 0); g_free(sha); g_free(path); g_free(directory);
}
static void start_property_changed(GObject *object, GParamSpec *property, gpointer data)
{ (void)object; (void)property; ++*(guint *)data; }
static void assert_start_stable(GtkWidget *card, const gchar *text, gboolean sensitive)
{
    GtkWidget *button = widget_find(card, "deck-0-autodj-start");
    g_assert_true(GTK_IS_BUTTON(button)); g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(button)), ==, text);
    g_assert_cmpint(gtk_widget_get_sensitive(button), ==, sensitive);
    GtkWidget *child = g_object_ref(gtk_bin_get_child(GTK_BIN(button))); guint labels = 0, sensitivities = 0;
    gulong label_changed = g_signal_connect(button, "notify::label", G_CALLBACK(start_property_changed), &labels);
    gulong sensitive_changed = g_signal_connect(button, "notify::sensitive", G_CALLBACK(start_property_changed), &sensitivities);
    const gchar *names[] = {"deck-0-play", "deck-0-pause", "deck-0-stop", "deck-0-autodj-target", "deck-0-autodj-mode"};
    GtkWidget *controls[G_N_ELEMENTS(names)]; gboolean states[G_N_ELEMENTS(names)];
    guint changes[G_N_ELEMENTS(names)] = {0}; gulong handlers[G_N_ELEMENTS(names)];
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        controls[i] = widget_find(card, names[i]); g_assert_nonnull(controls[i]); states[i] = gtk_widget_get_sensitive(controls[i]);
        handlers[i] = g_signal_connect(controls[i], "notify::sensitive", G_CALLBACK(start_property_changed), &changes[i]);
    }
    /* A FALSE→TRUE pair in one 100 ms callback is invisible to final-state
     * assertions. Observe notifications throughout several complete polls. */
    for (guint poll = 0; poll < 4; ++poll) {
        run_for(110); g_assert_true(gtk_bin_get_child(GTK_BIN(button)) == child);
        g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(button)), ==, text);
        g_assert_cmpint(gtk_widget_get_sensitive(button), ==, sensitive);
        for (guint i = 0; i < G_N_ELEMENTS(names); ++i) g_assert_cmpint(gtk_widget_get_sensitive(controls[i]), ==, states[i]);
    }
    g_assert_cmpuint(labels, ==, 0); g_assert_cmpuint(sensitivities, ==, 0);
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        g_test_message("%s unchanged sensitivity notifications: %u", names[i], changes[i]);
        g_assert_cmpuint(changes[i], ==, 0); g_signal_handler_disconnect(controls[i], handlers[i]);
    }
    g_signal_handler_disconnect(button, label_changed); g_signal_handler_disconnect(button, sensitive_changed); g_object_unref(child);
}
static gboolean idle_programme_saved(gpointer data)
{
    Window *window = data; RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0};
    GError *error = NULL; gboolean saved = radios_player_workspace_programme_saved(window->ctx->players, &scope, &error);
    g_clear_error(&error); return saved;
}
static gboolean start_card_ready(gpointer data)
{
    GtkWidget *card = card_owner(data, 0);
    return card && GTK_IS_BUTTON(widget_find(card, "deck-0-autodj-start"));
}
static gboolean ui_disabled(gpointer widget) { return !gtk_widget_get_sensitive(widget); }
static void test_start_stable_for_saved_semi_queue(void)
{
    reset_private_definitions(); Window window = window_open();
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0};
    gchar *path = g_build_filename(config.project_root, "idle-start-regression.wav", NULL), *sha = sync_wave(path);
    JsonObject *queue = programme("Sabit Başlat sırası", "Yerel WAV");
    JsonObject *event = json_array_get_object_element(json_object_get_array_member(queue, "events"), 0);
    json_object_set_string_member(event, "primary_file", path); json_object_set_int_member(event, "duration_ms", 30000);
    set_programme(&window, &scope, queue); wait_until(idle_programme_saved, &window, "real WAV programme queue CAS save");
    wait_until(start_card_ready, &window, "player card created by the UI refresh");
    GtkWidget *card = card_owner(&window, 0); g_assert_nonnull(card);
    GtkWidget *start = widget_find(card, "deck-0-autodj-start"), *mode = widget_find(card, "deck-0-autodj-mode");
    g_assert_true(GTK_IS_COMBO_BOX(mode)); g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(mode)), ==, "0");
    run_for(350); assert_start_stable(card, "Başlat", FALSE);
    guint changes = 0; gulong changed = g_signal_connect(start, "notify::sensitive", G_CALLBACK(start_property_changed), &changes);
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(mode), "1")); wait_until(ui_enabled, start, "SEMI enables Start for the saved queue");
    g_assert_cmpint(radios_player_workspace_topology(window.ctx->players)->players[0].autodj_mode, ==, RADIOS_AUTODJ_SEMI);
    g_assert_cmpuint(changes, ==, 1); g_assert_true(widget_find(card_owner(&window, 0), "deck-0-autodj-start") == start);
    assert_start_stable(card, "Başlat", TRUE); g_assert_cmpuint(changes, ==, 1);
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(mode), "0")); wait_until(ui_disabled, start, "manual mode disables Start again");
    g_assert_cmpuint(changes, ==, 2); assert_start_stable(card, "Başlat", FALSE); g_assert_cmpuint(changes, ==, 2);
    g_signal_handler_disconnect(start, changed); assert_silent(&window); window_close(&window);
    json_object_unref(queue); g_assert_cmpint(g_unlink(path), ==, 0); g_free(path); g_free(sha);
}
typedef struct { GtkWidget *dialog; const gchar *name; GtkTreeIter iter; } StartWeeklyPick;
static gboolean start_weekly_row_ready(gpointer data)
{
    StartWeeklyPick *pick = data; GtkWidget *tree = widget_find(pick->dialog, "player-weekly-list");
    if (!GTK_IS_TREE_VIEW(tree)) return FALSE;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree)); GtkTreeIter iter;
    for (gboolean valid = gtk_tree_model_get_iter_first(model, &iter); valid; valid = gtk_tree_model_iter_next(model, &iter)) {
        gchar *name = NULL; gtk_tree_model_get(model, &iter, 0, &name, -1); gboolean same = !g_strcmp0(name, pick->name); g_free(name);
        if (same) { pick->iter = iter; return TRUE; }
    }
    return FALSE;
}
static gboolean start_weekly_label_ready(gpointer data)
{ return !g_strcmp0(gtk_button_get_label(GTK_BUTTON(data)), "Programı başlat"); }
static void test_start_stable_for_loaded_weekly(void)
{
    reset_private_definitions(); JsonNode *record = weekly("Sabit Başlat haftalık", "Sabit program bloğu");
    Window window = window_open(); wait_until(start_card_ready, &window, "player card created by the UI refresh");
    GtkWidget *card = card_owner(&window, 0); g_assert_nonnull(card);
    GtkWidget *start = widget_find(card, "deck-0-autodj-start");
    /* Start is disabled at construction; the target becomes disabled only
     * when the first audio poll has applied the loaded manual mode. */
    wait_until(ui_disabled, widget_find(card, "deck-0-autodj-target"), "first audio poll applies manual mode");
    assert_start_stable(card, "Başlat", FALSE);
    guint labels = 0; gulong changed = g_signal_connect(start, "notify::label", G_CALLBACK(start_property_changed), &labels);
    ui_click(card, "deck-0-weekly-programme"); GtkWidget *picker = ui_dialog("player-weekly-dialog"); g_assert_nonnull(picker);
    StartWeeklyPick pick = {picker, "Sabit Başlat haftalık", {0}}; wait_until(start_weekly_row_ready, &pick, "saved weekly fixture appears in the real picker");
    GtkWidget *tree = widget_find(picker, "player-weekly-list");
    gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), &pick.iter);
    gtk_dialog_response(GTK_DIALOG(picker), GTK_RESPONSE_ACCEPT); wait_until(start_weekly_label_ready, start, "loaded weekly changes Start to Programı başlat");
    g_assert_cmpuint(labels, ==, 1); g_assert_null(ui_dialog("player-weekly-dialog"));
    run_for(350); assert_start_stable(card, "Programı başlat", TRUE); g_assert_cmpuint(labels, ==, 1);
    g_signal_handler_disconnect(start, changed); assert_silent(&window); window_close(&window); json_node_free(record);
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc; g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    const gchar *main_sink = g_getenv("RADIOS_TEST_MAIN_SINK"), *cue_sink = g_getenv("RADIOS_TEST_CUE_SINK");
    g_assert_nonnull(main_sink); g_assert_nonnull(cue_sink); g_assert_true(g_str_has_prefix(main_sink, "radios_gui_"));
    g_assert_true(g_str_has_prefix(cue_sink, "radios_gui_"));
    GError *error = NULL; g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    application = gtk_application_new("org.radios.PlayerWorkspaceAcceptance", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/player-workspace/legacy-readonly-migration", test_legacy_load);
    g_test_add_func("/player-workspace/scopes-persist-projection-reopen", test_scope_persistence);
    g_test_add_func("/player-workspace/CAS-reject-reload-reopen", test_cas_conflict);
    g_test_add_func("/player-workspace/visible-cluster-management", test_visible_cluster_management);
    g_test_add_func("/player-workspace/sync-explicit-group-source", test_sync_explicit_group_source);
    g_test_add_func("/player-workspace/start-stable-for-saved-semi-queue", test_start_stable_for_saved_semi_queue);
    g_test_add_func("/player-workspace/start-stable-for-loaded-weekly", test_start_stable_for_loaded_weekly);
    gint result = g_test_run(); radios_data_client_unref(reader); g_object_unref(application); radios_config_clear(&config); return result;
}
