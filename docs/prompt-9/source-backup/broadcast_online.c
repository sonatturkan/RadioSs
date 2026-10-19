#include "radios/broadcast_online.h"
#include "radios/youtube_music.h"
#include "radios/official_player.h"
#include "radios/youtube_music_client.h"
#include "radios/online_services_ui.h"
#include "radios/audio_ui.h"
#include "radios/data.h"
#include "radios/widgets.h"
#include "radios/programme_model.h"
#include <math.h>
#include <string.h>

typedef enum { USER_NONE, USER_CATALOG, USER_FLOW, USER_CARD } PlayerUser;
typedef struct {
    JsonNode *programme;          /* owned copy of the started programme */
    JsonArray *events;
    guint total, index, segment_start, segment_end, skipped, failures, status_serial;
    guint mode;
    gdouble overlap;
    gboolean local, waiting, requested;
    guint64 playout_intent, play_intent;
    gint64 start_at;
} Flow;

struct RadioSBroadcastOnline {
    gint references;
    RadioSViewContext *ctx;
    GtkWidget *owner, *count, *status, *source_row, *player_row, *playlists, *refresh, *connect, *now;
    GtkWidget *play, *pause, *stop, *next;
    GtkListStore *store;
    GtkTreeView *tree;
    RadioSYouTubeMusicPlayer *player;
    /* Spotify / Apple Music in their official player; official_active while
     * it, not the YouTube Music player, owns the current playback. */
    RadioSOfficialPlayer *official;
    RadioSDataClient *client;
    gchar *generation, *rows_label, *provider;
    /* Spotify / Apple Music item → its YouTube Music recording (JsonNode,
     * NULL value when no match was found). */
    GHashTable *matches;
    JsonNode *library, *saved, *rows, *queue;
    JsonArray *matching_rows;
    GPtrArray *prepared_catalog, *prepared_programme;
    GCancellable *cancel;
    /* Catalogue requests and playback starts are fenced separately: browsing
     * while the next song is being authorized must not stop the list. */
    guint64 intent, play_intent;
    guint64 flow_intent;
    guint queue_index, timer, status_serial;
    PlayerUser user;
    Flow *flow;
    GQueue completed;
    gchar flow_text[320];
    /* Card playback: the owning card, a card waiting for its start and the
     * last start failure of a card; all weak pointers. */
    GtkWidget *card, *card_pending, *card_failed;
    gchar *card_video, card_error[256];
    guint card_status_serial;
    gboolean card_resume;
    gboolean active, closed, busy, connected, applying, external_rows, official_active;
};
typedef struct {
    RadioSBroadcastOnline *online;
    RadioSConfig config;
    RadioSDataClient *client;
    gchar *station_id, *user_id, *action, *generation, *label;
    JsonNode *payload;
    GCancellable *cancel;
    guint64 intent;
} Request;
typedef struct {
    RadioSBroadcastOnline *online;
    RadioSConfig config;
    RadioSDataClient *client;
    gchar *station_id, *user_id, *generation, *video;
    guint index;
    guint64 intent;
    gboolean resume;
    PlayerUser user;
    GWeakRef card;
} Play;

static void request_start(RadioSBroadcastOnline *online, const gchar *action, JsonNode *payload, const gchar *label);
static void playlist_changed(GtkComboBox *combo, gpointer data);
static void catalog_match(RadioSBroadcastOnline *online, guint index, gboolean prepare);
static const gchar *item_video(JsonObject *item);
static void online_prepare_all(RadioSBroadcastOnline *online);
static void prepare_catalogue(RadioSBroadcastOnline *online);
static gboolean video_valid(const gchar *video);

static RadioSBroadcastOnline *online_ref(RadioSBroadcastOnline *online)
{ g_atomic_int_inc(&online->references); return online; }
static void online_unref(gpointer data)
{
    RadioSBroadcastOnline *online = data;
    if (!g_atomic_int_dec_and_test(&online->references)) return;
    if (online->client) radios_data_client_unref(online->client);
    g_clear_object(&online->cancel); g_clear_object(&online->store);
    g_clear_pointer(&online->library, json_node_free); g_clear_pointer(&online->saved, json_node_free);
    g_clear_pointer(&online->rows, json_node_free); g_clear_pointer(&online->queue, json_node_free);
    g_queue_clear_full(&online->completed, g_free);
    if (online->player) radios_youtube_music_player_unref(online->player);
    radios_official_player_free(online->official);
    if (online->flow) { json_node_free(online->flow->programme); g_free(online->flow); }
    if (online->matches) g_hash_table_unref(online->matches);
    g_clear_pointer(&online->prepared_catalog, g_ptr_array_unref); g_clear_pointer(&online->prepared_programme, g_ptr_array_unref);
    g_free(online->provider); g_free(online->card_video); g_free(online->generation); g_free(online->rows_label); g_free(online);
}
static void pack(GtkWidget *box, GtkWidget *widget, gboolean expand)
{ gtk_box_pack_start(GTK_BOX(box), widget, expand, expand, 0); }
static GtkWidget *button_new(GtkWidget *row, const gchar *title, const gchar *name, GCallback callback, gpointer data)
{
    GtkWidget *button = radios_ui_button(title); gtk_widget_set_name(button, name);
    g_signal_connect(button, "clicked", callback, data); pack(row, button, FALSE); return button;
}
static JsonNode *body_new(void)
{ JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, json_object_new()); return node; }
static JsonObject *object_get(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonArray *array_get(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static JsonObject *array_object(JsonArray *array, guint index)
{
    JsonNode *node = array && index < json_array_get_length(array) ? json_array_get_element(array, index) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static gboolean bool_get(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN && json_node_get_boolean(node);
}
static void status_set(RadioSBroadcastOnline *online, const gchar *text)
{ if (!online->closed && online->active) gtk_label_set_text(GTK_LABEL(online->status), text); }
static void busy_set(RadioSBroadcastOnline *online, gboolean busy)
{
    online->busy = busy;
    gtk_widget_set_sensitive(online->playlists, !busy); gtk_widget_set_sensitive(online->refresh, !busy);
    gtk_widget_set_sensitive(online->play, !busy); gtk_widget_set_sensitive(online->next, !busy);
}
static void config_copy(RadioSConfig *target, const RadioSConfig *source)
{
    *target = *source;
#define COPY(field) target->field = g_strdup(source->field)
    COPY(project_root); COPY(config_file); COPY(name); COPY(initial_profile); COPY(assets_root);
    COPY(database_file); COPY(endpoint_root); COPY(import_root); COPY(cache_root); COPY(log_root);
    COPY(recording_root); COPY(backup_root); COPY(database_source_dir); COPY(service_url);
    COPY(python_command); COPY(token_environment);
#undef COPY
}
/* ---- Sounding player ------------------------------------------------------ */
/* YouTube Music plays in RadioS. A Spotify / Apple Music id ("spotify:track:…",
 * "apple_music:…") plays in its official player, whose sound RadioS routes to
 * MAIN. MAIN takes one external source, so one of the two sounds at a time. */
static gboolean official_video(const gchar *video) { return radios_official_player_id(video); }
static RadioSOfficialPlayer *official_player(RadioSBroadcastOnline *online)
{
    if (!online->official && online->ctx->config)
        online->official = radios_official_player_new(online->ctx->config->project_root, online->ctx->config->python_command);
    return online->official;
}
static void player_stop(RadioSBroadcastOnline *online)
{
    radios_youtube_music_player_stop(online->player);
    radios_official_player_stop(online->official); online->official_active = FALSE;
}
static void player_snapshot(RadioSBroadcastOnline *online, RadioSYouTubeMusicSnapshot *snapshot)
{
    if (online->official_active) radios_official_player_snapshot(online->official, snapshot);
    else radios_youtube_music_player_snapshot(online->player, snapshot);
}
static gboolean player_pause(RadioSBroadcastOnline *online, gboolean paused, GError **error)
{
    return online->official_active ? radios_official_player_pause(online->official, paused, error) :
        radios_youtube_music_player_pause(online->player, paused, error);
}
/* The id a paused playback resumes with. */
static const gchar *player_video(RadioSBroadcastOnline *online, const RadioSYouTubeMusicSnapshot *snapshot)
{ return online->official_active ? radios_official_player_current(online->official) : snapshot->video_id; }
static void account_clear(RadioSBroadcastOnline *online)
{
    radios_youtube_music_player_prepare_list(online->player, NULL, NULL, NULL, NULL, NULL, 0);
    radios_youtube_music_player_preload(online->player, NULL, NULL, NULL, NULL, NULL);
    /* A Spotify / Apple Music playback does not depend on the YouTube Music account. */
    if (!online->official_active) {
        radios_youtube_music_player_stop(online->player); g_clear_pointer(&online->queue, json_node_free); online->user = USER_NONE;
    }
    online->connected = FALSE; g_clear_pointer(&online->generation, g_free); g_clear_pointer(&online->library, json_node_free);
}
static void listing_clear(RadioSBroadcastOnline *online)
{
    g_clear_pointer(&online->rows, json_node_free); g_clear_pointer(&online->rows_label, g_free);
    if (online->active && !online->external_rows) { gtk_list_store_clear(online->store); gtk_label_set_text(GTK_LABEL(online->count), "— parça"); }
}
static gboolean scope_sync(RadioSBroadcastOnline *online)
{
    RadioSDataClient *client = online->ctx->data ? radios_data_client_acquire(online->ctx) : NULL;
    if (client != online->client) {
        if (online->cancel) g_cancellable_cancel(online->cancel);
        ++online->intent; ++online->play_intent;
        if (online->client) radios_data_client_unref(online->client);
        online->client = client; client = NULL;
        g_clear_pointer(&online->prepared_catalog, g_ptr_array_unref);
        g_clear_pointer(&online->prepared_programme, g_ptr_array_unref);
        account_clear(online); g_clear_pointer(&online->saved, json_node_free); listing_clear(online);
        if (online->official_active) { player_stop(online); g_clear_pointer(&online->queue, json_node_free); online->user = USER_NONE; }
    }
    if (client) radios_data_client_unref(client);
    return online->client != NULL;
}

/* ---- Catalogue rows -------------------------------------------------- */
static gboolean youtube_source(RadioSBroadcastOnline *online) { return !g_strcmp0(online->provider, "youtube"); }
static const gchar *provider_title(const gchar *provider)
{ return !g_strcmp0(provider, "spotify") ? "Spotify" : !g_strcmp0(provider, "apple_music") ? "Apple Music" : "YouTube Music"; }
static void rows_render(RadioSBroadcastOnline *online, JsonArray *items, const gchar *label)
{
    if (online->external_rows) return;
    JsonArray *rows = json_array_new(); guint count = 0;
    /* Spotify / Apple Music rows play in the official player when it is installed. */
    gboolean official = !youtube_source(online) && radios_official_player_available(online->provider);
    gtk_list_store_clear(online->store);
    for (guint i = 0; items && i < json_array_get_length(items); ++i) {
        JsonObject *item = array_object(items, i);
        if (!item || (youtube_source(online) ? strlen(radios_json_string(item, "provider_id", "")) != 11 : !*radios_json_string(item, "title", ""))) continue;
        /* Spotify / Apple Music rows play through their YouTube Music match. */
        JsonObject *row = radios_programme_clone_object(item);
        if (!youtube_source(online)) {
            json_object_set_string_member(row, "source_provider", online->provider);
            gchar *id = official ? radios_official_player_item_id(online->provider, radios_json_string(item, "provider_id", "")) : NULL;
            if (id) json_object_set_string_member(row, "official_id", id);
            g_free(id);
        }
        json_array_add_object_element(rows, row);
        gint64 seconds = radios_json_int(item, "duration_ms", 0) / 1000;
        gchar *duration = seconds > 0 ? g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, seconds / 60, seconds % 60) : g_strdup("—");
        GtkTreeIter iter; gtk_list_store_append(online->store, &iter);
        gtk_list_store_set(online->store, &iter, 0, radios_json_string(item, "artist", ""), 1, radios_json_string(item, "title", ""),
            2, "", 3, provider_title(online->provider), 4, duration, 5, label ? label : provider_title(online->provider), -1);
        g_free(duration); ++count;
    }
    g_clear_pointer(&online->rows, json_node_free);
    online->rows = json_node_new(JSON_NODE_ARRAY); json_node_take_array(online->rows, rows);
    if (label != online->rows_label) { g_free(online->rows_label); online->rows_label = g_strdup(label); }
    gchar *text = g_strdup_printf("%u parça", count); gtk_label_set_text(GTK_LABEL(online->count), text); g_free(text);
    radios_broadcast_online_prepare_items(online, rows, FALSE);
    if (!youtube_source(online) && !official) prepare_catalogue(online);
}
static JsonObject *saved_find(RadioSBroadcastOnline *online, const gchar *id)
{
    JsonArray *records = online->saved ? array_get(json_node_get_object(online->saved), "records") : NULL;
    for (guint i = 0; id && *id && records && i < json_array_get_length(records); ++i) {
        JsonObject *record = array_object(records, i);
        if (record && g_str_equal(id, radios_json_string(record, "_id", ""))) return record;
    }
    return NULL;
}
static void playlists_render(RadioSBroadcastOnline *online)
{
    GtkComboBoxText *combo = GTK_COMBO_BOX_TEXT(online->playlists);
    gchar *previous = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo)));
    online->applying = TRUE; gtk_combo_box_text_remove_all(combo);
    gtk_combo_box_text_append(combo, "", "Playlist seçin");
    JsonArray *records = online->saved ? array_get(json_node_get_object(online->saved), "records") : NULL;
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        JsonObject *record = array_object(records, i), *config = object_get(record, "config");
        if (g_strcmp0(radios_json_string(config, "provider", ""), online->provider)) continue;
        gchar *key = g_strdup_printf("db:%s", radios_json_string(record, "_id", ""));
        gchar *label = g_strdup_printf("%s · %s", youtube_source(online) ? "RadioS" : provider_title(online->provider), radios_json_string(record, "name", ""));
        gtk_combo_box_text_append(combo, key, label); g_free(key); g_free(label);
    }
    JsonArray *library = online->library && youtube_source(online) ? array_get(json_node_get_object(online->library), "playlists") : NULL;
    for (guint i = 0; library && i < json_array_get_length(library); ++i) {
        JsonObject *playlist = array_object(library, i); const gchar *id = radios_json_string(playlist, "id", "");
        if (!*id) continue;
        gchar *key = g_strdup_printf("acct:%s", id);
        gchar *label = g_strdup_printf("YouTube · %s", radios_json_string(playlist, "name", radios_json_string(playlist, "title", id)));
        gtk_combo_box_text_append(combo, key, label); g_free(key); g_free(label);
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    if (previous && *previous) gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), previous);
    online->applying = FALSE; g_free(previous);
}
static void saved_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    (void)ctx; (void)owner; RadioSBroadcastOnline *online = data;
    if (online->closed || error || !result || !JSON_NODE_HOLDS_OBJECT(result)) return;
    g_clear_pointer(&online->saved, json_node_free); online->saved = json_node_copy(result);
    playlists_render(online);
}
static void saved_refresh(RadioSBroadcastOnline *online)
{
    if (online->closed || !radios_data_connected(online->ctx)) return;
    JsonNode *body = radios_data_body(online->ctx);
    json_object_set_string_member(json_node_get_object(body), "kind", "external_playlist");
    radios_data_request(online->ctx, online->owner, "POST", "/v1/workflows/desktop/list", body, saved_loaded, online_ref(online), online_unref);
    json_node_free(body);
}

/* ---- YouTube Music bridge requests ------------------------------------ */
static void request_free(gpointer data)
{
    Request *request = data;
    g_clear_pointer(&request->payload, json_node_free); g_clear_object(&request->cancel);
    if (request->client) radios_data_client_unref(request->client);
    radios_config_clear(&request->config);
    g_free(request->station_id); g_free(request->user_id); g_free(request->action);
    g_free(request->generation); g_free(request->label);
    online_unref(request->online); g_free(request);
}
static gboolean identity_current(RadioSBroadcastOnline *online, RadioSDataClient *client, const gchar *station, const gchar *user)
{
    RadioSDataClient *current = radios_data_client_acquire(online->ctx);
    gboolean same = current && current == client && !g_strcmp0(radios_data_client_station_id(current), station) &&
        !g_strcmp0(radios_data_client_user_id(current), user);
    if (current) radios_data_client_unref(current);
    return same;
}
static void request_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; Request *request = data; GError *error = NULL;
    JsonNode *result = radios_youtube_music_request_sync(&request->config, request->station_id,
        request->user_id, request->action, request->payload, cancel, &error);
    if (error) g_task_return_error(task, error);
    else if (result) g_task_return_pointer(task, result, (GDestroyNotify)json_node_free);
    else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "YouTube Music yanıtı alınamadı.");
}
/* Applies a verified account status; a refresh of the same verified account
 * keeps current playback. FALSE when no account is connected. */
static gboolean status_apply(RadioSBroadcastOnline *online, JsonObject *response)
{
    const gchar *generation = radios_json_string(response, "auth_generation", "");
    gboolean connected = bool_get(response, "connected") && *generation; ++online->status_serial;
    if (!connected || g_strcmp0(generation, online->generation)) account_clear(online);
    online->connected = connected; gtk_widget_set_visible(online->connect, !connected);
    if (connected && !online->generation) online->generation = g_strdup(generation);
    if (connected) online_prepare_all(online);
    return connected;
}
static void request_done(GObject *source, GAsyncResult *result, gpointer unused)
{
    (void)source; (void)unused; GTask *task = G_TASK(result); Request *request = g_task_get_task_data(task);
    RadioSBroadcastOnline *online = request->online; GError *error = NULL;
    JsonNode *node = g_task_propagate_pointer(task, &error);
    if (!online->closed && online->cancel == request->cancel) {
        g_clear_object(&online->cancel); busy_set(online, FALSE);
        if (request->intent != online->intent ||
            !identity_current(online, request->client, request->station_id, request->user_id)) {
            scope_sync(online); status_set(online, "RadioS oturumu değişti; yanıt kullanılmadı.");
        } else if (error || !node || !JSON_NODE_HOLDS_OBJECT(node)) {
            if (error && error->domain == RADIOS_YOUTUBE_MUSIC_ERROR && error->code == RADIOS_YOUTUBE_MUSIC_AUTH) {
                account_clear(online); gtk_widget_show(online->connect);
            }
            status_set(online, error ? error->message : "YouTube Music yanıtı doğrulanamadı.");
        } else {
            JsonObject *response = json_node_get_object(node);
            const gchar *generation = radios_json_string(response, "auth_generation", "");
            if (g_str_equal(request->action, "status")) {
                if (status_apply(online, response)) {
                    /* A flow or a card reads the account state without the playlist selector. */
                    status_set(online, ""); if (online->active && !online->external_rows) request_start(online, "library_playlists", NULL, NULL);
                } else status_set(online, "YouTube Music hesabı bağlı değil.");
            } else if (!request->generation || g_strcmp0(generation, request->generation)) {
                account_clear(online); gtk_widget_show(online->connect);
                status_set(online, "YouTube Music hesabı değişti; Yenile ile listeleri yeniden alın.");
            } else if (g_str_equal(request->action, "library_playlists")) {
                g_clear_pointer(&online->library, json_node_free); online->library = json_node_copy(node);
                playlists_render(online); status_set(online, "");
            } else if (g_str_equal(request->action, "tracks") || g_str_equal(request->action, "search")) {
                rows_render(online, array_get(response, "items"), request->label);
                gint64 unavailable = radios_json_int(response, "unavailable_count", 0);
                gchar *text = bool_get(response, "truncated") ? g_strdup("İlk 1000 parça listelendi.") :
                    unavailable > 0 ? g_strdup_printf("Çalınamayan %" G_GINT64_FORMAT " parça listelenmedi.", unavailable) : g_strdup("");
                status_set(online, text); g_free(text);
            }
        }
    }
    g_clear_error(&error); g_clear_pointer(&node, json_node_free);
}
static void request_start(RadioSBroadcastOnline *online, const gchar *action, JsonNode *payload, const gchar *label)
{
    if (online->closed || online->busy) return;
    if (!online->ctx->config || !scope_sync(online)) { status_set(online, "Önce RadioS veritabanına bağlanın."); return; }
    if (!g_str_equal(action, "status") && !online->connected) {
        gtk_widget_show(online->connect); status_set(online, "YouTube Music hesabı bağlı değil."); return;
    }
    Request *request = g_new0(Request, 1); request->online = online_ref(online);
    config_copy(&request->config, online->ctx->config);
    request->client = radios_data_client_acquire(online->ctx);
    request->station_id = g_strdup(radios_data_client_station_id(request->client));
    request->user_id = g_strdup(radios_data_client_user_id(request->client));
    request->action = g_strdup(action); request->generation = g_strdup(online->generation); request->label = g_strdup(label);
    request->payload = payload ? json_node_copy(payload) : body_new();
    if (request->generation && !g_str_equal(action, "status"))
        json_object_set_string_member(json_node_get_object(request->payload), "expected_auth_generation", request->generation);
    request->cancel = g_cancellable_new(); request->intent = ++online->intent;
    g_clear_object(&online->cancel); online->cancel = g_object_ref(request->cancel);
    busy_set(online, TRUE); status_set(online, "YouTube Music…");
    GTask *task = g_task_new(NULL, request->cancel, request_done, NULL);
    g_task_set_task_data(task, request, request_free); g_task_run_in_thread(task, request_worker); g_object_unref(task);
}

/* ---- Shared player ----------------------------------------------------- */
/* One YouTube Music player serves the catalogue, the broadcast flow and live
 * player cards, because MAIN takes one external source; user is its owner. */
static void play_free(gpointer data)
{
    Play *play = data;
    if (play->client) radios_data_client_unref(play->client);
    radios_config_clear(&play->config);
    g_free(play->station_id); g_free(play->user_id); g_free(play->generation); g_free(play->video);
    g_weak_ref_clear(&play->card); online_unref(play->online); g_free(play);
}
static gboolean operate_allowed(Play *play, JsonObject *result)
{
    JsonObject *station = object_get(result, "station"), *user = object_get(result, "user");
    if (g_strcmp0(radios_json_string(station, "_id", ""), play->station_id) ||
        g_strcmp0(radios_json_string(user, "_id", ""), play->user_id) ||
        g_strcmp0(radios_json_string(user, "status", ""), "active")) return FALSE;
    JsonArray *permissions = array_get(result, "permissions");
    for (guint i = 0; permissions && i < json_array_get_length(permissions); ++i) {
        JsonObject *grant = array_object(permissions, i);
        JsonNode *scope = grant ? json_object_get_member(grant, "station_id") : NULL;
        gboolean applies = scope && (JSON_NODE_HOLDS_NULL(scope) ||
            !g_strcmp0(radios_json_string(grant, "station_id", ""), play->station_id));
        if (applies && !g_strcmp0(radios_json_string(grant, "user_id", ""), play->user_id) &&
            !g_strcmp0(radios_json_string(grant, "permission", ""), "operate")) return TRUE;
    }
    return FALSE;
}
static void flow_youtube_failed(RadioSBroadcastOnline *online, const gchar *message, gboolean fatal);
/* Weak card pointers in the online object. */
static void card_set(RadioSBroadcastOnline *online, GtkWidget **slot, GtkWidget *card)
{
    (void)online;
    if (*slot == card) return;
    if (*slot) g_object_remove_weak_pointer(G_OBJECT(*slot), (gpointer *)slot);
    *slot = card;
    if (card) g_object_add_weak_pointer(G_OBJECT(card), (gpointer *)slot);
}
/* Prepares a song to start at once (radios_youtube_music_player_preload). */
static void online_prepare(RadioSBroadcastOnline *online, const gchar *video)
{
    if (!online || online->closed || !video || !*video || official_video(video) || !online->connected || !online->generation ||
        !online->client || !online->ctx->config) return;
    radios_youtube_music_player_preload(online->player, online->ctx->config, radios_data_client_station_id(online->client),
        radios_data_client_user_id(online->client), online->generation, video);
}
static void flow_prepare_next(RadioSBroadcastOnline *online);
static void play_authorized(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    (void)ctx; (void)owner; Play *play = data; RadioSBroadcastOnline *online = play->online;
    if (online->closed) return;
    busy_set(online, FALSE);
    /* A newer start or a stop superseded this authorization. */
    if (play->intent != online->play_intent) return;
    const gchar *message = NULL; GError *failure = NULL; gboolean started = FALSE, fatal = FALSE;
    gboolean official = official_video(play->video);
    if (!identity_current(online, play->client, play->station_id, play->user_id) ||
        (!official && g_strcmp0(play->generation, online->generation))) {
        message = "RadioS veya YouTube Music oturumu değişti; oynatma başlatılmadı."; fatal = TRUE;
    } else if (error || !result || !JSON_NODE_HOLDS_OBJECT(result) || !operate_allowed(play, json_node_get_object(result))) {
        message = error ? error->message : "Çalmak için bu istasyonda yayın kontrolü yetkisi gerekli."; fatal = TRUE;
    } else if (play->resume) started = player_pause(online, FALSE, &failure);
    else {
        RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(online->ctx);
        /* MAIN takes one external source: the other player stops first. */
        if (official) {
            radios_youtube_music_player_stop(online->player);
            started = engine && radios_official_player_start(official_player(online), engine, play->video, &failure);
            online->official_active = started;
        } else {
            radios_official_player_stop(online->official); online->official_active = FALSE;
            started = engine && radios_youtube_music_player_start(online->player, &play->config, engine,
                play->station_id, play->user_id, play->generation, play->video, 0, &failure);
        }
        if (engine) radios_audio_engine_unref(engine);
    }
    GtkWidget *card = g_weak_ref_get(&play->card);
    if (started) {
        online->user = play->user;
        if (play->user == USER_CATALOG) {
            online->queue_index = play->index; status_set(online, "");
            JsonArray *queue = online->queue ? json_node_get_array(online->queue) : NULL;
            JsonObject *next = array_object(queue, play->index + 1);
            if (next && !online->flow) { if (item_video(next)) online_prepare(online, item_video(next)); else catalog_match(online, play->index + 1, TRUE); }
        }
        if (play->user == USER_CARD) card_set(online, &online->card, card);
        if (play->user == USER_FLOW) flow_prepare_next(online);
    } else {
        gchar *text = g_strdup(message ? message : failure ? failure->message : "MAIN ses çıkışı hazır değil.");
        player_stop(online); online->user = USER_NONE;
        if (play->user == USER_CATALOG) { g_clear_pointer(&online->queue, json_node_free); status_set(online, text); }
        else if (play->user == USER_FLOW) flow_youtube_failed(online, text, fatal);
        else if (play->user == USER_CARD) { card_set(online, &online->card_failed, card); g_strlcpy(online->card_error, text, sizeof online->card_error); }
        g_free(text);
    }
    if (card) g_object_unref(card);
    g_clear_error(&failure);
}
/* Starts or resumes video for user after a fresh operate authorization; FALSE
 * when the account, MAIN or a running request does not allow it now. */
static gboolean play_request_card(RadioSBroadcastOnline *online, PlayerUser user, const gchar *video, guint index,
    gboolean resume, GtkWidget *card);
static gboolean play_request(RadioSBroadcastOnline *online, PlayerUser user, const gchar *video, guint index, gboolean resume)
{ return play_request_card(online, user, video, index, resume, NULL); }
static gboolean play_request_card(RadioSBroadcastOnline *online, PlayerUser user, const gchar *video, guint index,
    gboolean resume, GtkWidget *card)
{
    if (online->closed || online->busy) return FALSE;
    /* Spotify / Apple Music use their own app or window, not the YouTube Music account. */
    gboolean official = official_video(video);
    if (!online->ctx->config || !scope_sync(online) || (!official && (!online->connected || !online->generation))) {
        if (user == USER_CATALOG && !official) { gtk_widget_show(online->connect); status_set(online, "YouTube Music hesabı bağlı değil."); }
        else if (user == USER_CATALOG) status_set(online, "RadioS veritabanı bağlantısı hazır değil.");
        return FALSE;
    }
    if (!radios_audio_ui_ready(online->ctx, RADIOS_AUDIO_UI_MAIN)) {
        if (user == USER_CATALOG) status_set(online, "MAIN ses çıkışı hazır değil.");
        return FALSE;
    }
    Play *play = g_new0(Play, 1); play->online = online_ref(online); config_copy(&play->config, online->ctx->config);
    play->client = radios_data_client_acquire(online->ctx);
    play->station_id = g_strdup(radios_data_client_station_id(play->client));
    play->user_id = g_strdup(radios_data_client_user_id(play->client));
    play->generation = g_strdup(online->generation); play->video = g_strdup(video); play->index = index;
    play->resume = resume; play->user = user; play->intent = ++online->play_intent; g_weak_ref_init(&play->card, card);
    busy_set(online, TRUE);
    if (user == USER_CATALOG) status_set(online, "Oynatma yetkisi doğrulanıyor…");
    JsonNode *context = radios_data_body(online->ctx);
    gboolean queued = radios_data_request(online->ctx, online->owner, "POST", "/v1/workflows/desktop/context", context, play_authorized, play, play_free);
    if (!queued) { busy_set(online, FALSE); if (user == USER_CATALOG) status_set(online, "Oynatma yetkisi alınamadı."); }
    json_node_free(context);
    return queued;
}
static gint selected_index(RadioSBroadcastOnline *online)
{
    GtkTreeModel *model = NULL; GtkTreeIter iter;
    if (!gtk_tree_selection_get_selected(gtk_tree_view_get_selection(online->tree), &model, &iter)) return -1;
    GtkTreePath *path = gtk_tree_model_get_path(model, &iter); gint *indices = gtk_tree_path_get_indices(path);
    gint index = indices ? indices[0] : -1; gtk_tree_path_free(path); return index;
}
/* While the flow plays a YouTube Music event, the catalogue controls wait. */
static gboolean flow_holds_player(RadioSBroadcastOnline *online)
{
    if (!online->flow || online->flow->local || online->flow->waiting || online->flow->start_at) return FALSE;
    status_set(online, "Yayın akışı YouTube Music çalıyor."); return TRUE;
}
/* The YouTube Music id a queue item plays with, NULL until a Spotify /
 * Apple Music item is matched. */
static const gchar *item_video(JsonObject *item)
{
    if (item && json_object_has_member(item, "official_id")) return radios_json_string(item, "official_id", "");
    if (item && json_object_has_member(item, "youtube_id")) return radios_json_string(item, "youtube_id", "");
    return item && !json_object_has_member(item, "source_provider") && strlen(radios_json_string(item, "provider_id", "")) == 11 ?
        radios_json_string(item, "provider_id", "") : NULL;
}
typedef struct { RadioSBroadcastOnline *online; JsonNode *queue; guint index; guint64 intent; gboolean prepare; } CatalogMatch;
static void catalog_match_free(gpointer data)
{ CatalogMatch *match = data; json_node_free(match->queue); online_unref(match->online); g_free(match); }
static void catalog_play(RadioSBroadcastOnline *online, guint index);
static void catalog_matched(JsonObject *result, const gchar *error, gpointer data)
{
    CatalogMatch *match = data; RadioSBroadcastOnline *online = match->online;
    JsonArray *items = array_get(result, "items"); JsonObject *found = array_object(items, 0);
    JsonObject *item = array_object(json_node_get_array(match->queue), match->index);
    if (found && item) json_object_set_string_member(item, "youtube_id", radios_json_string(found, "provider_id", ""));
    /* The queue changed or playback stopped while matching. */
    if (online->closed || !online->queue || json_node_get_array(online->queue) != json_node_get_array(match->queue)) return;
    if (match->prepare) { if (found && !online->flow) online_prepare(online, radios_json_string(found, "provider_id", "")); return; }
    if (match->intent != online->play_intent) return;
    if (!found) {
        gchar *text = g_strdup_printf("%s: %s", radios_json_string(item, "title", "—"), error ? error : "YouTube Music'te bulunamadı.");
        status_set(online, text); g_free(text);
        if (match->index + 1 < json_array_get_length(json_node_get_array(online->queue))) catalog_play(online, match->index + 1);
        return;
    }
    play_request(online, USER_CATALOG, radios_json_string(found, "provider_id", ""), match->index, FALSE);
}
static void catalog_match(RadioSBroadcastOnline *online, guint index, gboolean prepare)
{
    JsonArray *queue = online->queue ? json_node_get_array(online->queue) : NULL; JsonObject *item = array_object(queue, index);
    if (!item) return;
    CatalogMatch *match = g_new0(CatalogMatch, 1); match->online = online_ref(online); match->index = index; match->prepare = prepare;
    match->queue = json_node_new(JSON_NODE_ARRAY); json_node_set_array(match->queue, queue); match->intent = online->play_intent;
    JsonArray *one = json_array_new(); json_array_add_object_element(one, json_object_ref(item));
    if (!prepare) status_set(online, "YouTube Music karşılığı aranıyor…");
    radios_broadcast_online_match(online, one, catalog_matched, match, catalog_match_free); json_array_unref(one);
}
/* Plays queue item index; a Spotify / Apple Music item is matched first. */
static void catalog_play(RadioSBroadcastOnline *online, guint index)
{
    JsonObject *item = array_object(online->queue ? json_node_get_array(online->queue) : NULL, index);
    const gchar *video = item_video(item);
    if (!item) return;
    if (video) play_request(online, USER_CATALOG, video, index, FALSE);
    else catalog_match(online, index, FALSE);
}
static void play_clicked(GtkButton *button, gpointer data)
{
    (void)button; RadioSBroadcastOnline *online = data;
    if (flow_holds_player(online)) return;
    JsonArray *rows = online->rows ? json_node_get_array(online->rows) : NULL;
    if (!rows || !json_array_get_length(rows)) { status_set(online, "Çalınacak parça yok."); return; }
    gint index = selected_index(online);
    if (index < 0 || (guint)index >= json_array_get_length(rows)) index = 0;
    g_clear_pointer(&online->queue, json_node_free); online->queue = json_node_copy(online->rows);
    catalog_play(online, (guint)index);
}
static void pause_clicked(GtkButton *button, gpointer data)
{
    (void)button; RadioSBroadcastOnline *online = data; RadioSYouTubeMusicSnapshot snapshot;
    if (flow_holds_player(online)) return;
    player_snapshot(online, &snapshot);
    if (online->user != USER_CATALOG) { status_set(online, "Çalan parça yok."); return; }
    if (snapshot.state == RADIOS_YOUTUBE_MUSIC_PAUSED) { play_request(online, USER_CATALOG, player_video(online, &snapshot), online->queue_index, TRUE); return; }
    if (snapshot.state != RADIOS_YOUTUBE_MUSIC_PLAYING) { status_set(online, "Çalan parça yok."); return; }
    GError *error = NULL;
    if (!player_pause(online, TRUE, &error)) status_set(online, error ? error->message : "Duraklatılamadı.");
    g_clear_error(&error);
}
static void stop_clicked(GtkButton *button, gpointer data)
{
    (void)button; RadioSBroadcastOnline *online = data;
    if (flow_holds_player(online)) return;
    if (online->user == USER_CATALOG || online->user == USER_NONE) {
        ++online->play_intent; player_stop(online); online->user = USER_NONE;
    }
    g_clear_pointer(&online->queue, json_node_free);
    status_set(online, "");
}
static void next_clicked(GtkButton *button, gpointer data)
{
    (void)button; RadioSBroadcastOnline *online = data;
    if (flow_holds_player(online)) return;
    JsonArray *queue = online->queue ? json_node_get_array(online->queue) : NULL;
    guint index = online->queue_index + 1;
    if (!queue || index >= json_array_get_length(queue)) { status_set(online, "Listenin sonuna ulaşıldı."); return; }
    catalog_play(online, index);
    if (online->rows && json_node_get_array(online->rows) == queue && online->active) {
        GtkTreePath *path = gtk_tree_path_new_from_indices((gint)index, -1);
        gtk_tree_selection_select_path(gtk_tree_view_get_selection(online->tree), path); gtk_tree_path_free(path);
    }
}
static gboolean sounding(const RadioSYouTubeMusicSnapshot *snapshot)
{
    return snapshot->state == RADIOS_YOUTUBE_MUSIC_STARTING || snapshot->state == RADIOS_YOUTUBE_MUSIC_PLAYING ||
        snapshot->state == RADIOS_YOUTUBE_MUSIC_PAUSED;
}

/* ---- Broadcast flow ----------------------------------------------------- */
/* A programme with YouTube Music events plays part by part: consecutive
 * library contents run as one local playout programme, each YouTube Music
 * event through the shared player. Contents without audio are skipped. */
static JsonObject *flow_event(Flow *flow, guint index)
{ return index < flow->total ? array_object(flow->events, index) : NULL; }
static gboolean event_online(JsonObject *event) { return event && json_object_has_member(event, "online_id"); }
static gboolean event_bound(JsonObject *event) { return event && json_object_has_member(event, "media_id"); }
static gboolean event_playable(JsonObject *event) { return event_online(event) || event_bound(event); }
static void flow_free(Flow *flow)
{ if (flow) { json_node_free(flow->programme); g_free(flow); } }

gboolean radios_broadcast_online_flow_needed(JsonObject *programme)
{
    JsonArray *events = array_get(programme, "events");
    for (guint i = 0; events && i < json_array_get_length(events); ++i)
        if (!event_bound(array_object(events, i))) return TRUE;
    return FALSE;
}
gboolean radios_broadcast_online_flow_running(RadioSBroadcastOnline *online)
{ return online && online->flow != NULL; }

/* Stops whatever sounds for the flow: its local programme and its YouTube event. */
static void flow_stop_engines(RadioSBroadcastOnline *online)
{
    Flow *flow = online->flow; if (!flow) return;
    if (flow->local && radios_playout_ui_intent(online->ctx) == flow->playout_intent)
        radios_playout_ui_action(online->ctx, RADIOS_PLAYOUT_STOP, NULL);
    flow->local = FALSE;
    if (online->user == USER_FLOW || flow->requested) {
        ++online->play_intent; player_stop(online);
        if (online->user == USER_FLOW) online->user = USER_NONE;
    }
}
static void flow_finish(RadioSBroadcastOnline *online, const gchar *text)
{
    gchar *copy = g_strdup(text);
    flow_free(online->flow); online->flow = NULL;
    g_strlcpy(online->flow_text, copy ? copy : "", sizeof online->flow_text); g_free(copy);
    online->flow_intent = radios_playout_ui_intent(online->ctx);
}
static JsonObject *segment_config(Flow *flow, guint start, guint end)
{
    JsonObject *programme = json_node_get_object(flow->programme), *segment = json_object_new();
    const gchar *keys[] = {"title", "date", "note"};
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) json_object_set_string_member(segment, keys[i], radios_json_string(programme, keys[i], ""));
    JsonObject *timing = object_get(programme, "timing"); JsonArray *events = json_array_new(); gboolean timed = FALSE;
    for (guint i = start; i < end; ++i) {
        JsonObject *event = radios_programme_clone_object(flow_event(flow, i));
        json_object_remove_member(event, "block_id");
        timed |= timing && g_str_equal(radios_json_string(timing, "event_id", ""), radios_json_string(event, "id", ""));
        json_array_add_object_element(events, event);
    }
    json_object_set_array_member(segment, "events", events);
    if (timed) json_object_set_object_member(segment, "timing", radios_programme_clone_object(timing));
    return segment;
}
static void flow_try_youtube(RadioSBroadcastOnline *online)
{
    Flow *flow = online->flow;
    if (!flow || flow->local || flow->requested || flow->waiting || flow->start_at || online->busy) return;
    const gchar *video = radios_json_string(flow_event(flow, flow->index), "online_id", "");
    if (!online->connected && !official_video(video)) {
        /* The account state is read once; a disconnected account ends the flow. */
        if (flow->status_serial && online->status_serial >= flow->status_serial) {
            flow_finish(online, "YouTube Music hesabı bağlı değil."); return;
        }
        if (!flow->status_serial) { flow->status_serial = online->status_serial + 1; request_start(online, "status", NULL, NULL); }
        return;
    }
    if (play_request(online, USER_FLOW, video, flow->index, FALSE)) { flow->requested = TRUE; flow->play_intent = online->play_intent; }
}
static void flow_begin(RadioSBroadcastOnline *online, guint index)
{
    Flow *flow = online->flow;
    while (index < flow->total && !event_playable(flow_event(flow, index))) { ++flow->skipped; ++index; }
    flow->waiting = FALSE; flow->requested = FALSE; flow->status_serial = 0;
    if (index >= flow->total) {
        guint skipped = flow->skipped; flow_stop_engines(online);
        gchar *text = skipped ? g_strdup_printf("Bitti · ses dosyası olmayan %u içerik atlandı", skipped) : g_strdup("Bitti");
        flow_finish(online, text); g_free(text); return;
    }
    flow->index = index;
    if (event_online(flow_event(flow, index))) {
        if (flow->local && radios_playout_ui_intent(online->ctx) == flow->playout_intent)
            radios_playout_ui_action(online->ctx, RADIOS_PLAYOUT_STOP, NULL);
        flow->local = FALSE;
        /* The catalogue or a card yields the shared player to the flow. */
        if (online->user != USER_NONE) { player_stop(online); online->user = USER_NONE; }
        g_clear_pointer(&online->queue, json_node_free); ++online->play_intent;
        flow_try_youtube(online);
        return;
    }
    if (online->user == USER_FLOW) { player_stop(online); online->user = USER_NONE; }
    ++online->play_intent;
    guint end = index;
    while (end < flow->total && event_bound(flow_event(flow, end))) ++end;
    JsonObject *segment = segment_config(flow, index, end); GError *error = NULL;
    gboolean started = radios_playout_ui_start_programme(online->ctx, online->owner, segment, 0, flow->mode, flow->overlap, &error);
    json_object_unref(segment);
    if (!started) {
        gchar *text = g_strdup(error ? error->message : "Yerel yürütücü başlatılamadı."); g_clear_error(&error);
        flow->local = FALSE; flow_finish(online, text); g_free(text); return;
    }
    flow->local = TRUE; flow->segment_start = index; flow->segment_end = end;
    flow->playout_intent = radios_playout_ui_intent(online->ctx);
    flow_prepare_next(online);
}
/* The next YouTube Music event of the flow is prepared while this part plays. */
static void flow_prepare_next(RadioSBroadcastOnline *online)
{
    Flow *flow = online->flow; if (!flow) return;
    guint next = flow->local ? flow->segment_end : flow->index + 1;
    while (next < flow->total && !event_playable(flow_event(flow, next))) ++next;
    JsonObject *event = flow_event(flow, next);
    if (event_online(event)) online_prepare(online, radios_json_string(event, "online_id", ""));
}
/* "Elle geç" waits for the operator before each next part. */
static void flow_next_part(RadioSBroadcastOnline *online, guint next)
{
    Flow *flow = online->flow;
    if (flow->mode == 3 && next < flow->total) { flow->local = FALSE; flow->index = next; flow->waiting = TRUE; return; }
    flow_begin(online, next);
}
static void flow_youtube_failed(RadioSBroadcastOnline *online, const gchar *message, gboolean fatal)
{
    Flow *flow = online->flow; if (!flow) return;
    if (fatal || ++flow->failures >= 3) { flow_stop_engines(online); flow_finish(online, message); return; }
    flow_next_part(online, flow->index + 1);
}
static void flow_tick(RadioSBroadcastOnline *online)
{
    Flow *flow = online->flow; if (!flow) return;
    if (flow->start_at) {
        if (g_get_real_time() < flow->start_at) return;
        flow->start_at = 0; flow_begin(online, flow->index); return;
    }
    if (flow->waiting) return;
    if (flow->local) {
        RadioSPlayoutSnapshot snapshot = {0};
        /* Another start or Stop took the local runner from this flow. */
        if (radios_playout_ui_intent(online->ctx) != flow->playout_intent) { flow->local = FALSE; flow_finish(online, "Durduruldu"); return; }
        if (!radios_playout_ui_get_snapshot(online->ctx, &snapshot)) return;
        JsonArray *completed = radios_playout_ui_take_completed(online->ctx);
        for (guint i = 0; i < json_array_get_length(completed); ++i)
            g_queue_push_tail(&online->completed, g_strdup(json_array_get_string_element(completed, i)));
        json_array_unref(completed);
        if (snapshot.state == RADIOS_PLAYOUT_ERROR) {
            gchar *text = g_strdup(snapshot.error[0] ? snapshot.error : "Yerel yürütücü hata verdi.");
            flow->local = FALSE; flow_finish(online, text); g_free(text); return;
        }
        if (snapshot.state == RADIOS_PLAYOUT_FINISHED) { flow->local = FALSE; flow_next_part(online, flow->segment_end); return; }
        flow->index = flow->segment_start + MIN(snapshot.index, flow->segment_end - flow->segment_start - 1);
        return;
    }
    if (!flow->requested) { flow_try_youtube(online); return; }
    if (online->user != USER_FLOW) {
        /* Superseded before it sounded (for example a session change): retry. */
        if (flow->play_intent != online->play_intent && !online->busy) flow->requested = FALSE;
        return;
    }
    RadioSYouTubeMusicSnapshot snapshot; player_snapshot(online, &snapshot);
    if (snapshot.state == RADIOS_YOUTUBE_MUSIC_FINISHED) {
        g_queue_push_tail(&online->completed, g_strdup(radios_json_string(flow_event(flow, flow->index), "id", "")));
        flow->failures = 0; online->user = USER_NONE; flow_next_part(online, flow->index + 1);
    }
    else if (snapshot.state == RADIOS_YOUTUBE_MUSIC_FAILED || snapshot.state == RADIOS_YOUTUBE_MUSIC_STOPPED) {
        gchar *text = g_strdup(snapshot.error[0] ? snapshot.error : "YouTube Music parçası çalınamadı.");
        online->user = USER_NONE; flow_youtube_failed(online, text, FALSE); g_free(text);
    }
}
static gboolean flow_create(RadioSBroadcastOnline *online, JsonObject *programme, guint start, guint mode,
    gdouble overlap, gint64 start_at, GError **error)
{
    JsonArray *events = array_get(programme, "events");
    if (!online || online->closed || mode > 3 || !isfinite(overlap) || overlap < 0 || overlap > 30) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Yayın akışı başlatılamadı."); return FALSE;
    }
    if (!events || start >= json_array_get_length(events)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Akışta çalınacak içerik yok."); return FALSE;
    }
    radios_broadcast_online_flow_stop(online);
    Flow *flow = g_new0(Flow, 1);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, programme);
    flow->programme = radios_programme_clone(node); json_node_free(node);
    flow->events = array_get(json_node_get_object(flow->programme), "events"); flow->total = json_array_get_length(flow->events);
    flow->mode = mode; flow->overlap = overlap; flow->index = start; flow->start_at = start_at;
    online->flow = flow; online->flow_text[0] = '\0';
    radios_broadcast_online_prepare_items(online, flow->events, TRUE);
    if (!start_at) flow_begin(online, start);
    else if (event_online(flow_event(flow, start))) online_prepare(online, radios_json_string(flow_event(flow, start), "online_id", ""));
    return TRUE;
}
gboolean radios_broadcast_online_flow_start(RadioSBroadcastOnline *online, JsonObject *programme,
    guint start_index, guint mode, gdouble overlap, GError **error)
{ return flow_create(online, programme, start_index, mode, overlap, 0, error); }
gboolean radios_broadcast_online_flow_schedule(RadioSBroadcastOnline *online, JsonObject *programme,
    gint64 start_utc_us, guint mode, gdouble overlap, GError **error)
{
    if (start_utc_us <= 0) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Takvim başlangıcı geçersiz."); return FALSE; }
    return flow_create(online, programme, 0, mode, overlap, start_utc_us, error);
}
void radios_broadcast_online_flow_stop(RadioSBroadcastOnline *online)
{
    if (!online || online->closed) return;
    if (online->flow) { flow_stop_engines(online); flow_free(online->flow); online->flow = NULL; }
    /* The programme takes the air: catalogue or card playback stops too. */
    if (online->user != USER_NONE) { ++online->play_intent; player_stop(online); online->user = USER_NONE; }
    g_clear_pointer(&online->queue, json_node_free);
    online->flow_text[0] = '\0';
    g_queue_clear_full(&online->completed, g_free);
}
gboolean radios_broadcast_online_flow_action(RadioSBroadcastOnline *online, RadioSPlayoutAction action, GError **error)
{
    Flow *flow = online ? online->flow : NULL;
    if (!flow) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Yayın akışı çalmıyor."); return FALSE; }
    RadioSPlayoutSnapshot local = {0};
    if (flow->local) radios_playout_ui_get_snapshot(online->ctx, &local);
    RadioSYouTubeMusicSnapshot player; player_snapshot(online, &player);
    switch (action) {
    case RADIOS_PLAYOUT_STOP:
        flow_stop_engines(online); flow_finish(online, "Durduruldu"); return TRUE;
    case RADIOS_PLAYOUT_PAUSE:
        if (flow->local) return radios_playout_ui_action(online->ctx, RADIOS_PLAYOUT_PAUSE, error);
        if (online->user == USER_FLOW && player.state == RADIOS_YOUTUBE_MUSIC_PLAYING)
            return player_pause(online, TRUE, error);
        break;
    case RADIOS_PLAYOUT_RESUME:
        if (flow->waiting || flow->start_at) { flow->start_at = 0; flow_begin(online, flow->index); return TRUE; }
        if (flow->local) return radios_playout_ui_action(online->ctx, RADIOS_PLAYOUT_RESUME, error);
        if (online->user == USER_FLOW && player.state == RADIOS_YOUTUBE_MUSIC_PAUSED &&
            play_request(online, USER_FLOW, player_video(online, &player), flow->index, TRUE)) return TRUE;
        break;
    case RADIOS_PLAYOUT_NEXT:
        if (flow->waiting) { flow_begin(online, flow->index); return TRUE; }
        if (flow->local && flow->segment_start + local.index + 1 < flow->segment_end)
            return radios_playout_ui_action(online->ctx, RADIOS_PLAYOUT_NEXT, error);
        flow_begin(online, flow->local ? flow->segment_end : flow->index + 1); return TRUE;
    case RADIOS_PLAYOUT_PREVIOUS:
        if (flow->local && local.index > 0) return radios_playout_ui_action(online->ctx, RADIOS_PLAYOUT_PREVIOUS, error);
        for (gint i = (gint)(flow->local ? flow->segment_start : flow->index) - 1; i >= 0; --i)
            if (event_playable(flow_event(flow, (guint)i))) { flow_begin(online, (guint)i); return TRUE; }
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Önceki içerik yok."); return FALSE;
    }
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Yayın akışı komutu şu an uygulanamaz.");
    return FALSE;
}
void radios_broadcast_online_flow_status(RadioSBroadcastOnline *online, RadioSBroadcastFlowStatus *status)
{
    memset(status, 0, sizeof *status);
    if (!online || online->closed) return;
    Flow *flow = online->flow;
    if (!flow) {
        /* The last flow result stays until the local runner gets another command. */
        if (online->flow_text[0] && radios_playout_ui_intent(online->ctx) == online->flow_intent)
            g_strlcpy(status->text, online->flow_text, sizeof status->text);
        return;
    }
    JsonObject *event = flow_event(flow, flow->index);
    status->running = TRUE; status->total = flow->total; status->index = flow->index; status->waiting = flow->waiting;
    g_strlcpy(status->playing, radios_json_string(event, "id", ""), sizeof status->playing);
    status->can_next = flow->index + 1 < flow->total || flow->waiting; status->can_previous = flow->index > 0;
    const gchar *title = radios_json_string(event, "title", "");
    if (flow->start_at) {
        GDateTime *start = g_date_time_new_from_unix_utc(flow->start_at / G_USEC_PER_SEC);
        gchar *clock = start ? g_date_time_format(start, "%H:%M:%S") : g_strdup("—");
        g_snprintf(status->text, sizeof status->text, "Saatli başlangıç · %s UTC", clock);
        g_free(clock); if (start) g_date_time_unref(start);
        status->playing[0] = '\0'; return;
    }
    if (flow->waiting) {
        g_snprintf(status->text, sizeof status->text, "Elle geçiş bekleniyor · %u/%u · %s", flow->index + 1, flow->total, title);
        status->playing[0] = '\0'; return;
    }
    if (flow->local) {
        RadioSPlayoutSnapshot snapshot = {0}; radios_playout_ui_get_snapshot(online->ctx, &snapshot);
        if (snapshot.entry_id[0]) g_strlcpy(status->playing, snapshot.entry_id, sizeof status->playing);
        status->paused = snapshot.state == RADIOS_PLAYOUT_PAUSED;
        status->can_pause = snapshot.state == RADIOS_PLAYOUT_PLAYING || snapshot.state == RADIOS_PLAYOUT_OVERLAP;
        status->position_ms = snapshot.position_ms - MIN(snapshot.position_ms, snapshot.cue_in_ms);
        status->duration_ms = snapshot.duration_ms - MIN(snapshot.duration_ms, snapshot.cue_in_ms);
        g_strlcpy(status->next, snapshot.next_entry_id, sizeof status->next);
        if (!status->next[0]) {
            guint next = flow->segment_end;
            while (next < flow->total && !event_playable(flow_event(flow, next))) ++next;
            g_strlcpy(status->next, radios_json_string(flow_event(flow, next), "id", ""), sizeof status->next);
        }
        g_snprintf(status->text, sizeof status->text, "%s · %u/%u · %s · %02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT,
            radios_playout_ui_state_name(snapshot.state), flow->index + 1, flow->total, snapshot.title[0] ? snapshot.title : title,
            snapshot.position_ms / 60000, (snapshot.position_ms / 1000) % 60);
        return;
    }
    RadioSYouTubeMusicSnapshot snapshot; player_snapshot(online, &snapshot);
    gboolean mine = online->user == USER_FLOW;
    static const gchar *const states[] = {"Hazırlanıyor", "Hazırlanıyor", "Çalıyor", "Duraklatıldı", "Bitti", "Çalınamadı"};
    guint state = mine && snapshot.state <= RADIOS_YOUTUBE_MUSIC_FAILED ? (guint)snapshot.state : 0;
    status->paused = mine && snapshot.state == RADIOS_YOUTUBE_MUSIC_PAUSED;
    status->can_pause = mine && snapshot.state == RADIOS_YOUTUBE_MUSIC_PLAYING;
    if (mine && (snapshot.state == RADIOS_YOUTUBE_MUSIC_PLAYING || snapshot.state == RADIOS_YOUTUBE_MUSIC_PAUSED)) {
        status->position_ms = (guint64)(MAX(0.0, snapshot.position_seconds) * 1000);
        status->duration_ms = (guint64)MAX((gint64)0, radios_json_int(event, "duration_ms", 0));
        guint next = flow->index + 1;
        while (next < flow->total && !event_playable(flow_event(flow, next))) ++next;
        g_strlcpy(status->next, radios_json_string(flow_event(flow, next), "id", ""), sizeof status->next);
    }
    gint seconds = mine ? (gint)snapshot.position_seconds : 0;
    g_snprintf(status->text, sizeof status->text, "YouTube Music · %s · %u/%u · %s · %02d:%02d",
        states[state], flow->index + 1, flow->total, title, seconds / 60, seconds % 60);
}
JsonArray *radios_broadcast_online_flow_take_completed(RadioSBroadcastOnline *online)
{
    JsonArray *result = json_array_new();
    if (!online || online->closed) return result;
    gchar *id;
    while ((id = g_queue_pop_head(&online->completed))) {
        json_array_add_string_element(result, id); g_free(id);
    }
    return result;
}

static void card_pending_try(RadioSBroadcastOnline *online);
static gboolean playback_tick(gpointer data)
{
    RadioSBroadcastOnline *online = data;
    if (online->closed) { online->timer = 0; return G_SOURCE_REMOVE; }
    flow_tick(online); card_pending_try(online);
    RadioSYouTubeMusicSnapshot snapshot; player_snapshot(online, &snapshot);
    /* Controls of a playing list stay reachable after switching to the media archive. */
    gtk_widget_set_visible(online->player_row, online->active || (online->user == USER_CATALOG && sounding(&snapshot)));
    static const gchar *const states[] = {"Durduruldu", "Hazırlanıyor", "Çalıyor", "Duraklatıldı", "Bitti", "Çalınamadı"};
    guint state = snapshot.state <= RADIOS_YOUTUBE_MUSIC_FAILED ? (guint)snapshot.state : (guint)RADIOS_YOUTUBE_MUSIC_FAILED;
    JsonArray *queue = online->queue ? json_node_get_array(online->queue) : NULL;
    JsonObject *item = online->user == USER_CATALOG && snapshot.state != RADIOS_YOUTUBE_MUSIC_STOPPED ? array_object(queue, online->queue_index) :
        online->user == USER_FLOW && online->flow ? flow_event(online->flow, online->flow->index) : NULL;
    gint seconds = (gint)snapshot.position_seconds;
    const gchar *official = online->official_active ? radios_official_player_current(online->official) : NULL;
    gchar *text = g_strdup_printf("%s%s%s%s%s · %d:%02d%s%s%s", online->user == USER_FLOW ? "Yayın akışı · " : "",
        !official ? "" : g_str_has_prefix(official, "spotify:") ? "Spotify uygulaması · " : "Apple Music penceresi · ", states[state], item ? " · " : "",
        item ? radios_json_string(item, "title", "") : "", seconds / 60, seconds % 60,
        snapshot.main_routed ? " · MAIN" : "", *snapshot.error ? " · " : "", snapshot.error);
    if (snapshot.prepared_total) {
        gchar *prepared = g_strdup_printf("%s · Ses hazır %u/%u%s", text, snapshot.prepared_ready, snapshot.prepared_total,
            snapshot.preparing ? " · paralel hazırlanıyor" : snapshot.prepared_failed ? " · bazı parçalar hazırlanamadı" : "");
        g_free(text); text = prepared;
    }
    gtk_label_set_text(GTK_LABEL(online->now), text); g_free(text);
    if (snapshot.state == RADIOS_YOUTUBE_MUSIC_FINISHED && queue && !online->busy && online->user == USER_CATALOG) {
        if (online->queue_index + 1 < json_array_get_length(queue)) next_clicked(NULL, online);
        else { g_clear_pointer(&online->queue, json_node_free); online->user = USER_NONE; }
    }
    return G_SOURCE_CONTINUE;
}

/* ---- Selection, activation, construction ------------------------------ */
static void playlist_changed(GtkComboBox *combo, gpointer data)
{
    RadioSBroadcastOnline *online = data;
    if (online->applying || !online->active || online->closed) return;
    const gchar *id = gtk_combo_box_get_active_id(combo);
    if (!id || !*id) { listing_clear(online); return; }
    gchar *label = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (g_str_has_prefix(id, "db:")) {
        JsonObject *config = object_get(saved_find(online, id + 3), "config");
        rows_render(online, array_get(config, "items"), label); status_set(online, "");
    } else if (g_str_has_prefix(id, "acct:")) {
        JsonNode *payload = body_new();
        json_object_set_string_member(json_node_get_object(payload), "playlist_id", id + 5);
        listing_clear(online); request_start(online, "tracks", payload, label); json_node_free(payload);
    }
    g_free(label);
}
void radios_broadcast_online_search(RadioSBroadcastOnline *online, const gchar *query)
{
    if (!online || !online->active || online->closed) return;
    gchar *text = g_strstrip(g_strdup(query ? query : ""));
    if (*text && !youtube_source(online)) {
        /* Spotify / Apple Music lists are searched in place. */
        const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(online->playlists));
        JsonArray *items = id && g_str_has_prefix(id, "db:") ? array_get(object_get(saved_find(online, id + 3), "config"), "items") : NULL;
        JsonArray *found = json_array_new(); gchar *needle = g_utf8_casefold(text, -1);
        for (guint i = 0; items && i < json_array_get_length(items); ++i) {
            JsonObject *item = array_object(items, i);
            gchar *title = g_utf8_casefold(radios_json_string(item, "title", ""), -1), *artist = g_utf8_casefold(radios_json_string(item, "artist", ""), -1);
            if (strstr(title, needle) || strstr(artist, needle)) json_array_add_element(found, json_node_copy(json_array_get_element(items, i)));
            g_free(title); g_free(artist);
        }
        rows_render(online, found, online->rows_label); json_array_unref(found); g_free(needle);
    } else if (!*text) playlist_changed(GTK_COMBO_BOX(online->playlists), online);
    else if (g_utf8_strlen(text, -1) > 256) status_set(online, "Arama metni en fazla 256 karakter olabilir.");
    else {
        JsonNode *payload = body_new(); json_object_set_string_member(json_node_get_object(payload), "query", text);
        request_start(online, "search", payload, "YouTube Music · arama"); json_node_free(payload);
    }
    g_free(text);
}
static void row_activated(GtkTreeView *tree, GtkTreePath *path, GtkTreeViewColumn *column, gpointer data)
{ (void)tree; (void)path; (void)column; RadioSBroadcastOnline *online = data; if (online->active) play_clicked(NULL, online); }
static void refresh_clicked(GtkButton *button, gpointer data)
{
    (void)button; RadioSBroadcastOnline *online = data;
    saved_refresh(online); request_start(online, "status", NULL, NULL);
}
static void connect_clicked(GtkButton *button, gpointer data)
{ (void)button; RadioSBroadcastOnline *online = data; radios_online_services_ui_open(online->ctx); }
void radios_broadcast_online_set_active(RadioSBroadcastOnline *online, gboolean active)
{
    if (!online || online->closed) return;
    if (!active && online->cancel) {
        g_cancellable_cancel(online->cancel); g_clear_object(&online->cancel); ++online->intent; busy_set(online,FALSE);
    }
    online->external_rows = FALSE;
    online->active = active;
    RadioSYouTubeMusicSnapshot snapshot; player_snapshot(online, &snapshot);
    gtk_widget_set_visible(online->source_row, active); gtk_widget_set_visible(online->player_row, active || sounding(&snapshot));
    if (!active) return;
    gtk_label_set_text(GTK_LABEL(online->status), "");
    if (online->rows) rows_render(online, json_node_get_array(online->rows), online->rows_label);
    else { gtk_list_store_clear(online->store); gtk_label_set_text(GTK_LABEL(online->count), "— parça"); }
    scope_sync(online); gtk_widget_set_visible(online->connect, !online->connected);
    saved_refresh(online);
    if (!online->connected) request_start(online, "status", NULL, NULL);
    else if (!online->library) request_start(online, "library_playlists", NULL, NULL);
}
gboolean radios_broadcast_online_active(RadioSBroadcastOnline *online)
{ return online && online->active && !online->closed; }
void radios_broadcast_online_catalogue_rows(RadioSBroadcastOnline *online, JsonArray *rows, const gchar *label)
{
    if (!online || online->closed) return;
    scope_sync(online); online->external_rows = TRUE; online->active = TRUE;
    g_clear_pointer(&online->rows, json_node_free);
    online->rows = json_node_new(JSON_NODE_ARRAY); json_node_set_array(online->rows, rows);
    g_free(online->rows_label); online->rows_label = g_strdup(label);
    gtk_widget_hide(online->source_row); gtk_widget_show(online->player_row);
    if (!online->connected && !online->cancel) request_start(online, "status", NULL, NULL);
}
GtkWidget *radios_broadcast_online_source_row(RadioSBroadcastOnline *online) { return online->source_row; }
void radios_broadcast_online_set_provider(RadioSBroadcastOnline *online, const gchar *provider)
{
    if (!online || online->closed || !g_strcmp0(online->provider, provider)) return;
    /* Rows of another provider never stay listed. */
    g_free(online->provider); online->provider = g_strdup(provider);
    listing_clear(online); playlists_render(online);
}
const gchar *radios_broadcast_online_provider(RadioSBroadcastOnline *online) { return online ? online->provider : "youtube"; }
GtkWidget *radios_broadcast_online_player_row(RadioSBroadcastOnline *online) { return online->player_row; }
static void owner_destroyed(GtkWidget *owner, gpointer data)
{
    (void)owner; RadioSBroadcastOnline *online = data;
    if (online->closed) return;
    online->closed = TRUE; ++online->intent; ++online->play_intent;
    if (online->flow) { json_node_free(online->flow->programme); g_free(online->flow); online->flow = NULL; }
    if (online->cancel) g_cancellable_cancel(online->cancel);
    if (online->timer) { g_source_remove(online->timer); online->timer = 0; }
    radios_youtube_music_player_prepare_list(online->player, NULL, NULL, NULL, NULL, NULL, 0);
    radios_youtube_music_player_stop(online->player);
    radios_official_player_free(online->official); online->official = NULL; online->official_active = FALSE;
}
static GtkWidget *hidden_row(GtkWidget *row)
{ gtk_widget_show_all(row); gtk_widget_hide(row); gtk_widget_set_no_show_all(row, TRUE); return row; }
RadioSBroadcastOnline *radios_broadcast_online_new(RadioSViewContext *ctx, GtkWidget *owner,
    GtkWidget *table, GtkWidget *count, GtkWidget *status)
{
    g_return_val_if_fail(ctx && owner && table && count && status, NULL);
    RadioSBroadcastOnline *online = g_new0(RadioSBroadcastOnline, 1);
    online->references = 1; online->ctx = ctx; online->owner = owner; online->count = count; online->status = status;
    online->store = g_object_ref(g_object_get_data(G_OBJECT(table), "list-store"));
    online->tree = g_object_get_data(G_OBJECT(table), "tree-view");
    online->player = radios_youtube_music_player_new(); online->provider = g_strdup("youtube");
    online->matches = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, (GDestroyNotify)json_node_free);
    online->source_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 7, "broadcast-online-source-row");
    gtk_widget_set_name(online->source_row, "broadcast-online-source");
    pack(online->source_row, radios_ui_label("Playlist", NULL), FALSE);
    online->playlists = gtk_combo_box_text_new(); gtk_widget_set_name(online->playlists, "broadcast-online-playlists");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(online->playlists), "", "Playlist seçin");
    gtk_combo_box_set_active(GTK_COMBO_BOX(online->playlists), 0); pack(online->source_row, online->playlists, TRUE);
    g_signal_connect(online->playlists, "changed", G_CALLBACK(playlist_changed), online);
    online->refresh = button_new(online->source_row, "Yenile", "broadcast-online-refresh", G_CALLBACK(refresh_clicked), online);
    online->connect = button_new(online->source_row, "Hesabı bağla", "broadcast-online-connect", G_CALLBACK(connect_clicked), online);
    online->player_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "broadcast-online-player");
    gtk_widget_set_name(online->player_row, "broadcast-online-player");
    online->play = button_new(online->player_row, "▶ Çal", "broadcast-online-play", G_CALLBACK(play_clicked), online);
    online->pause = button_new(online->player_row, "Ⅱ Duraklat / Devam", "broadcast-online-pause", G_CALLBACK(pause_clicked), online);
    online->stop = button_new(online->player_row, "■ Durdur", "broadcast-online-stop", G_CALLBACK(stop_clicked), online);
    online->next = button_new(online->player_row, "▸| Sonraki", "broadcast-online-next", G_CALLBACK(next_clicked), online);
    online->now = radios_ui_label("Durduruldu", NULL); gtk_widget_set_name(online->now, "broadcast-online-now");
    gtk_label_set_ellipsize(GTK_LABEL(online->now), PANGO_ELLIPSIZE_END); gtk_label_set_xalign(GTK_LABEL(online->now), 0);
    pack(online->player_row, online->now, TRUE);
    hidden_row(online->source_row); hidden_row(online->player_row);
    if (online->tree) g_signal_connect(online->tree, "row-activated", G_CALLBACK(row_activated), online);
    g_signal_connect(owner, "destroy", G_CALLBACK(owner_destroyed), online);
    g_object_set_data_full(G_OBJECT(owner), "radios-broadcast-online", online, online_unref);
    online->timer = g_timeout_add_full(G_PRIORITY_DEFAULT, 500, playback_tick, online_ref(online), online_unref);
    return online;
}

JsonArray *radios_broadcast_online_items(RadioSBroadcastOnline *online, gboolean all)
{
    JsonArray *rows = online && online->active && !online->closed && online->rows ? json_node_get_array(online->rows) : NULL;
    if (!rows || !json_array_get_length(rows)) return NULL;
    gint selected = all ? -1 : selected_index(online);
    if (!all && (selected < 0 || (guint)selected >= json_array_get_length(rows))) return NULL;
    JsonArray *items = json_array_new();
    for (guint i = all ? 0 : (guint)selected; i < json_array_get_length(rows) && (all || i == (guint)selected); ++i)
        if (JSON_NODE_HOLDS_OBJECT(json_array_get_element(rows, i))) json_array_add_element(items, json_node_copy(json_array_get_element(rows, i)));
    return items;
}
const gchar *radios_broadcast_online_label(RadioSBroadcastOnline *online)
{ return online && online->rows_label && *online->rows_label ? online->rows_label : "YouTube Music"; }

/* ---- Provider calls for other views ------------------------------------ */
typedef struct {
    RadioSBroadcastOnline *online;
    RadioSConfig config;
    RadioSDataClient *client;
    gchar *station_id, *user_id, *action, *next, *generation;
    JsonNode *request, *payload;
    RadioSOnlineReply reply;
    gpointer data;
    GDestroyNotify destroy;
} Call;
static void call_free(gpointer value)
{
    Call *call = value;
    if (call->destroy) call->destroy(call->data);
    g_clear_pointer(&call->payload, json_node_free); g_clear_pointer(&call->request, json_node_free);
    if (call->client) radios_data_client_unref(call->client);
    radios_config_clear(&call->config);
    g_free(call->station_id); g_free(call->user_id); g_free(call->action); g_free(call->next);
    g_free(call->generation); online_unref(call->online); g_free(call);
}
static void call_worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; Call *call = data; GError *error = NULL;
    JsonNode *result = radios_youtube_music_request_sync(&call->config, call->station_id, call->user_id, call->action,
        call->payload, cancel, &error);
    if (error) g_task_return_error(task, error);
    else if (result) g_task_return_pointer(task, result, (GDestroyNotify)json_node_free);
    else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "YouTube Music yanıtı alınamadı.");
}
static void call_start(RadioSBroadcastOnline *online, const gchar *action, JsonNode *request,
    RadioSOnlineReply reply, gpointer data, GDestroyNotify destroy);
static void call_reply(Call *call, JsonObject *result, const gchar *error)
{
    if (call->reply) call->reply(result, error, call->data);
    call->reply = NULL;
}
static void call_done(GObject *source, GAsyncResult *result, gpointer unused)
{
    (void)source; (void)unused; GTask *task = G_TASK(result); Call *call = g_task_get_task_data(task);
    RadioSBroadcastOnline *online = call->online; GError *error = NULL;
    JsonNode *node = g_task_propagate_pointer(task, &error);
    if (online->closed) {
        call_reply(call, NULL, "YouTube Music oynatıcısı kapandı.");
        g_clear_error(&error); g_clear_pointer(&node, json_node_free); return;
    }
    if (!identity_current(online, call->client, call->station_id, call->user_id)) call_reply(call, NULL, "RadioS oturumu değişti.");
    else if (error || !node || !JSON_NODE_HOLDS_OBJECT(node)) {
        if (error && error->domain == RADIOS_YOUTUBE_MUSIC_ERROR && error->code == RADIOS_YOUTUBE_MUSIC_AUTH) account_clear(online);
        call_reply(call, NULL, error ? error->message : "YouTube Music yanıtı doğrulanamadı.");
    } else if (g_str_equal(call->action, "status")) {
        if (!status_apply(online, json_node_get_object(node))) call_reply(call, NULL, "YouTube Music hesabı bağlı değil.");
        else {
            /* The waiting request continues with the verified account. */
            call_start(online, call->next, call->request, call->reply, call->data, call->destroy);
            call->reply = NULL; call->destroy = NULL;
        }
    } else if (g_strcmp0(radios_json_string(json_node_get_object(node), "auth_generation", ""), call->generation)) {
        account_clear(online); call_reply(call, NULL, "YouTube Music hesabı değişti; listeyi yenileyin.");
    } else {
        if (g_str_equal(call->action, "library_playlists")) {
            g_clear_pointer(&online->library, json_node_free); online->library = json_node_copy(node); playlists_render(online);
        }
        call_reply(call, json_node_get_object(node), NULL);
    }
    g_clear_error(&error); g_clear_pointer(&node, json_node_free);
}
static void call_start(RadioSBroadcastOnline *online, const gchar *action, JsonNode *request,
    RadioSOnlineReply reply, gpointer data, GDestroyNotify destroy)
{
    if (!online || online->closed || !online->ctx->config || !scope_sync(online)) {
        if (reply) reply(NULL, "Önce RadioS veritabanına bağlanın.", data);
        if (destroy) destroy(data);
        return;
    }
    Call *call = g_new0(Call, 1); call->online = online_ref(online); config_copy(&call->config, online->ctx->config);
    call->client = radios_data_client_acquire(online->ctx);
    call->station_id = g_strdup(radios_data_client_station_id(call->client));
    call->user_id = g_strdup(radios_data_client_user_id(call->client));
    call->reply = reply; call->data = data; call->destroy = destroy;
    call->request = request ? radios_programme_clone(request) : body_new();
    if (!online->connected || !online->generation) { call->action = g_strdup("status"); call->next = g_strdup(action); call->payload = body_new(); }
    else { call->action = g_strdup(action); call->payload = radios_programme_clone(call->request); }
    call->generation = g_strdup(online->generation);
    if (call->generation && !g_str_equal(call->action, "status"))
        json_object_set_string_member(json_node_get_object(call->payload), "expected_auth_generation", call->generation);
    GTask *task = g_task_new(NULL, NULL, call_done, NULL);
    g_task_set_task_data(task, call, call_free); g_task_run_in_thread(task, call_worker); g_object_unref(task);
}
void radios_broadcast_online_provider_lists(RadioSBroadcastOnline *online, RadioSOnlineReply reply,
    gpointer data, GDestroyNotify destroy)
{
    if (online && !online->closed && online->connected && online->library) {
        reply(json_node_get_object(online->library), NULL, data);
        if (destroy) destroy(data);
        return;
    }
    call_start(online, "library_playlists", NULL, reply, data, destroy);
}
void radios_broadcast_online_provider_tracks(RadioSBroadcastOnline *online, const gchar *playlist_id,
    RadioSOnlineReply reply, gpointer data, GDestroyNotify destroy)
{
    JsonNode *request = body_new(); json_object_set_string_member(json_node_get_object(request), "playlist_id", playlist_id);
    call_start(online, "tracks", request, reply, data, destroy); json_node_free(request);
}

/* ---- Spotify / Apple Music through YouTube Music ------------------------- */
/* The same recording on YouTube Music (tools/radios_music_match.py), eight
 * items per bridge request; found and missing results are remembered. */
enum { MATCH_BATCH = 8 };
typedef struct {
    RadioSBroadcastOnline *online;
    JsonArray *items;
    JsonNode **found;
    guint *pending, pending_count, done;
    RadioSOnlineReply reply;
    gpointer data;
    GDestroyNotify destroy;
} MatchJob;
static gchar *match_key(JsonObject *item)
{
    gchar *title = g_utf8_casefold(radios_json_string(item, "title", ""), -1), *artist = g_utf8_casefold(radios_json_string(item, "artist", ""), -1);
    gchar *key = g_strdup_printf("%s\x1f%s\x1f%" G_GINT64_FORMAT, title, artist, radios_json_int(item, "duration_ms", 0) / 1000);
    g_free(title); g_free(artist); return key;
}
static void match_job_free(MatchJob *job)
{
    for (guint i = 0; i < json_array_get_length(job->items); ++i) if (job->found[i]) json_node_free(job->found[i]);
    if (job->destroy) job->destroy(job->data);
    json_array_unref(job->items); g_free(job->found); g_free(job->pending); online_unref(job->online); g_free(job);
}
static void match_job_finish(MatchJob *job, const gchar *error)
{
    if (error) { job->reply(NULL, error, job->data); match_job_free(job); return; }
    JsonObject *result = json_object_new(); JsonArray *items = json_array_new(); guint found = 0;
    for (guint i = 0; i < json_array_get_length(job->items); ++i) {
        if (job->found[i]) { json_array_add_element(items, json_node_copy(job->found[i])); ++found; }
        else json_array_add_null_element(items);
    }
    json_object_set_array_member(result, "items", items);
    json_object_set_int_member(result, "found", found);
    json_object_set_int_member(result, "missing", (gint64)json_array_get_length(job->items) - found);
    job->reply(result, NULL, job->data); json_object_unref(result); match_job_free(job);
}
static void match_job_next(MatchJob *job);
static void match_batch_done(JsonObject *result, const gchar *error, gpointer data)
{
    MatchJob *job = data; RadioSBroadcastOnline *online = job->online;
    if (error) { match_job_finish(job, error); return; }
    JsonArray *matches = array_get(result, "matches"); gint64 attempted = radios_json_int(result, "attempted", 0);
    if (!matches || attempted < 1 || (guint)attempted > MIN(MATCH_BATCH, job->pending_count - job->done)) {
        match_job_finish(job, "YouTube Music eşleşme yanıtı geçersiz."); return;
    }
    for (guint i = 0; i < (guint)attempted && i < json_array_get_length(matches); ++i) {
        JsonObject *entry = array_object(matches, i); guint index = job->pending[job->done + i];
        JsonNode *item = entry ? json_object_get_member(entry, "item") : NULL;
        gchar *key = match_key(array_object(job->items, index));
        if (entry && bool_get(entry, "matched") && item && JSON_NODE_HOLDS_OBJECT(item) &&
            radios_json_int(entry, "index", -1) == (gint64)i && strlen(radios_json_string(json_node_get_object(item), "provider_id", "")) == 11) {
            job->found[index] = json_node_copy(item);
            g_hash_table_replace(online->matches, key, json_node_copy(item));
        } else g_hash_table_replace(online->matches, key, NULL);
    }
    job->done += (guint)attempted;
    match_job_next(job);
}
static void match_job_next(MatchJob *job)
{
    if (job->online->closed) { match_job_finish(job, "YouTube Music oynatıcısı kapandı."); return; }
    if (job->done >= job->pending_count) { match_job_finish(job, NULL); return; }
    JsonNode *request = body_new(); JsonArray *batch = json_array_new();
    for (guint i = job->done; i < job->pending_count && i < job->done + MATCH_BATCH; ++i) {
        /* The bridge accepts 512 title and 256 artist characters. */
        JsonObject *item = array_object(job->items, job->pending[i]), *entry = json_object_new();
        json_object_set_int_member(entry, "index", i - job->done);
        const gchar *name = radios_json_string(item, "title", ""), *by = radios_json_string(item, "artist", "");
        gchar *title = g_utf8_substring(name, 0, MIN(g_utf8_strlen(name, -1), 512));
        gchar *artist = g_utf8_substring(by, 0, MIN(g_utf8_strlen(by, -1), 256));
        json_object_set_string_member(entry, "title", title); json_object_set_string_member(entry, "artist", artist);
        g_free(title); g_free(artist);
        json_object_set_int_member(entry, "duration_ms", CLAMP(radios_json_int(item, "duration_ms", 0), 0, 86400000));
        json_array_add_object_element(batch, entry);
    }
    json_object_set_array_member(json_node_get_object(request), "items", batch);
    call_start(job->online, "match", request, match_batch_done, job, NULL); json_node_free(request);
}
void radios_broadcast_online_match(RadioSBroadcastOnline *online, JsonArray *items, RadioSOnlineReply reply,
    gpointer data, GDestroyNotify destroy)
{
    MatchJob *job = g_new0(MatchJob, 1); job->online = online_ref(online); job->items = json_array_ref(items);
    job->reply = reply; job->data = data; job->destroy = destroy;
    guint count = json_array_get_length(items);
    job->found = g_new0(JsonNode *, MAX(count, 1)); job->pending = g_new0(guint, MAX(count, 1));
    for (guint i = 0; i < count; ++i) {
        JsonObject *item = array_object(items, i); gpointer cached = NULL;
        gchar *title = g_strstrip(g_strdup(item ? radios_json_string(item, "title", "") : ""));
        gboolean named = *title && g_utf8_validate(title, -1, NULL); g_free(title);
        if (!item) continue;
        /* A YouTube Music item needs no match. */
        if (!json_object_has_member(item, "source_provider") && strlen(radios_json_string(item, "provider_id", "")) == 11) {
            job->found[i] = json_node_new(JSON_NODE_OBJECT); json_node_set_object(job->found[i], item); continue;
        }
        if (!named) continue;
        gchar *key = match_key(item);
        if (g_hash_table_lookup_extended(online->matches, key, NULL, &cached)) { if (cached) job->found[i] = json_node_copy(cached); }
        else job->pending[job->pending_count++] = i;
        g_free(key);
    }
    match_job_next(job);
}

typedef struct { RadioSBroadcastOnline *online; JsonArray *rows; } CataloguePreparation;
static void catalogue_preparation_free(gpointer data)
{
    CataloguePreparation *preparation = data;
    json_array_unref(preparation->rows); online_unref(preparation->online); g_free(preparation);
}
static void catalogue_prepared(JsonObject *result, const gchar *error, gpointer data)
{
    CataloguePreparation *preparation = data; RadioSBroadcastOnline *online = preparation->online;
    if (online->matching_rows == preparation->rows) online->matching_rows = NULL;
    if (online->closed) return;
    if (!online->rows || json_node_get_array(online->rows) != preparation->rows) { prepare_catalogue(online); return; }
    if (error) { status_set(online, error); return; }
    JsonArray *items = array_get(result, "items"); guint found = 0;
    for (guint index = 0; index < json_array_get_length(preparation->rows); ++index) {
        JsonObject *item = array_object(preparation->rows, index), *match = array_object(items, index);
        const gchar *video = radios_json_string(match, "provider_id", "");
        if (item && video_valid(video)) { json_object_set_string_member(item, "youtube_id", video); ++found; }
    }
    radios_broadcast_online_prepare_items(online, preparation->rows, FALSE);
    if (!online->flow) {
        gint selected = selected_index(online);
        for (guint index = (guint)MAX(selected, 0); index < json_array_get_length(preparation->rows); ++index) {
            const gchar *video = item_video(array_object(preparation->rows, index));
            if (video_valid(video)) { online_prepare(online, video); break; }
        }
    }
    gchar *text = g_strdup_printf("%u/%u parça eşleşti · YouTube Music", found, json_array_get_length(preparation->rows));
    status_set(online, text); g_free(text);
}
static void prepare_catalogue(RadioSBroadcastOnline *online)
{
    if (online->closed || !online->rows || youtube_source(online) || online->matching_rows) return;
    JsonArray *rows = json_node_get_array(online->rows);
    if (!json_array_get_length(rows)) return;
    CataloguePreparation *preparation = g_new0(CataloguePreparation, 1);
    preparation->online = online_ref(online); preparation->rows = json_array_ref(rows); online->matching_rows = rows;
    radios_broadcast_online_match(online, rows, catalogue_prepared, preparation, catalogue_preparation_free);
}

/* ---- Live player cards --------------------------------------------------- */
static gboolean video_valid(const gchar *video)
{
    gboolean valid = video && strlen(video) == 11;
    for (guint i = 0; valid && video[i]; ++i) valid = g_ascii_isalnum(video[i]) || video[i] == '_' || video[i] == '-';
    return valid;
}
/* Starts a card waiting for the account or a running request. */
static void card_pending_try(RadioSBroadcastOnline *online)
{
    if (!online->card_pending || online->busy || online->closed) return;
    if (!online->connected && !official_video(online->card_video)) {
        if (online->card_status_serial && online->status_serial >= online->card_status_serial) {
            /* The account status arrived: no account is connected. */
            card_set(online, &online->card_failed, online->card_pending);
            g_strlcpy(online->card_error, "YouTube Music hesabı bağlı değil.", sizeof online->card_error);
            card_set(online, &online->card_pending, NULL); online->card_status_serial = 0; return;
        }
        if (!online->card_status_serial) { online->card_status_serial = online->status_serial + 1; request_start(online, "status", NULL, NULL); }
        return;
    }
    online->card_status_serial = 0;
    if (play_request_card(online, USER_CARD, online->card_video, 0, online->card_resume, online->card_pending))
        card_set(online, &online->card_pending, NULL);
}
gboolean radios_broadcast_online_card_play(RadioSBroadcastOnline *online, GtkWidget *card,
    const gchar *video, gboolean resume, GError **error)
{
    if (!online || online->closed || !card) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CLOSED, "YouTube Music oynatıcısı hazır değil."); return FALSE; }
    if (online->flow && !online->flow->local && !online->flow->waiting && !online->flow->start_at) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY, "Yayın akışı YouTube Music çalıyor."); return FALSE;
    }
    if (!video_valid(video) && !official_video(video)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Geçerli YouTube Music / Spotify / Apple Music parçası yok."); return FALSE;
    }
    /* The catalogue or another card yields the shared player to this card. */
    if (!resume && online->user != USER_NONE) { ++online->play_intent; player_stop(online); online->user = USER_NONE; }
    g_clear_pointer(&online->queue, json_node_free);
    if (online->card_failed == card) { card_set(online, &online->card_failed, NULL); online->card_error[0] = '\0'; }
    g_free(online->card_video); online->card_video = g_strdup(video); online->card_resume = resume;
    online->card_status_serial = 0; card_set(online, &online->card_pending, card);
    card_pending_try(online);
    return TRUE;
}
gboolean radios_broadcast_online_card_pause(RadioSBroadcastOnline *online, GtkWidget *card, GError **error)
{
    if (!online || online->user != USER_CARD || online->card != card) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Bu oynatıcıda çalan YouTube Music parçası yok."); return FALSE;
    }
    return player_pause(online, TRUE, error);
}
void radios_broadcast_online_card_stop(RadioSBroadcastOnline *online, GtkWidget *card)
{
    if (!online || online->closed) return;
    if (online->card_pending == card) card_set(online, &online->card_pending, NULL);
    if (online->user == USER_CARD && online->card == card) {
        ++online->play_intent; player_stop(online); online->user = USER_NONE;
        card_set(online, &online->card, NULL);
    }
}
void radios_broadcast_online_card_state(RadioSBroadcastOnline *online, GtkWidget *card,
    RadioSYouTubeMusicSnapshot *snapshot, gboolean *starting)
{
    memset(snapshot, 0, sizeof *snapshot); snapshot->state = RADIOS_YOUTUBE_MUSIC_STOPPED; *starting = FALSE;
    if (!online || online->closed || !card) return;
    if (online->user == USER_CARD && online->card == card) player_snapshot(online, snapshot);
    else if (online->card_pending == card) *starting = TRUE;
    else if (online->card_failed == card && online->card_error[0]) {
        snapshot->state = RADIOS_YOUTUBE_MUSIC_FAILED; g_strlcpy(snapshot->error, online->card_error, sizeof snapshot->error);
    }
}

void radios_broadcast_online_prepare(RadioSBroadcastOnline *online, const gchar *video, gboolean flow)
{
    /* A running flow keeps the preparation of its own next event. */
    if (!online || (online->flow && !flow)) return;
    online_prepare(online, video);
}

static void online_prepare_all(RadioSBroadcastOnline *online)
{
    if (!online || online->closed || !online->connected || !online->generation || !online->client || !online->ctx->config) return;
    GPtrArray *videos = g_ptr_array_new();
    GPtrArray *sources[] = {online->flow ? NULL : online->prepared_programme, online->prepared_catalog};
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint index = 0; online->flow && index < online->flow->total && videos->len < 1000; ++index) {
        const gchar *video = radios_json_string(flow_event(online->flow, index), "online_id", "");
        if (strlen(video) == 11 && g_hash_table_add(seen, (gpointer)video)) g_ptr_array_add(videos, (gpointer)video);
    }
    for (guint source = 0; source < G_N_ELEMENTS(sources); ++source) {
        GPtrArray *items = sources[source];
        for (guint index = 0; items && index < items->len && videos->len < 1000; ++index) {
            gchar *video = g_ptr_array_index(items, index);
            if (g_hash_table_add(seen, video)) g_ptr_array_add(videos, video);
        }
    }
    radios_youtube_music_player_prepare_list(online->player, online->ctx->config, radios_data_client_station_id(online->client),
        radios_data_client_user_id(online->client), online->generation, (const gchar *const *)videos->pdata, videos->len);
    g_hash_table_unref(seen); g_ptr_array_unref(videos);
}

void radios_broadcast_online_prepare_items(RadioSBroadcastOnline *online, JsonArray *items, gboolean programme)
{
    if (!online || online->closed) return;
    if (!online->ctx->config || !scope_sync(online)) return;
    GPtrArray *videos = g_ptr_array_new_with_free_func(g_free);
    for (guint index = 0; items && index < json_array_get_length(items) && videos->len < 1000; ++index) {
        JsonObject *item = array_object(items, index);
        const gchar *video = programme ? radios_json_string(item, "online_id", "") : item_video(item);
        if (video && strlen(video) == 11) g_ptr_array_add(videos, g_strdup(video));
    }
    GPtrArray **destination = programme ? &online->prepared_programme : &online->prepared_catalog;
    g_clear_pointer(destination, g_ptr_array_unref); *destination = videos;
    online_prepare_all(online);
    if (videos->len && !online->connected && !online->busy && !online->status_serial)
        request_start(online, "status", NULL, NULL);
}
