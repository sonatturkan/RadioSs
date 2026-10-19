#include "radios/broadcast_header.h"
#include "radios/views.h"
#include "radios/data.h"
#include "radios/youtube_music_client.h"
#include "radios/youtube_music.h"
#include "radios/audio_ui.h"
#include "radios/playout_ui.h"
#include "radios/broadcast_online.h"
#include "radios/programme_queue.h"
#include <string.h>

/* Broadcast catalogue "YouTube Music" source against a private genuine SDBASE.
 * Link wrappers isolate the provider bridge and audio hardware only. */
static RadioSConfig config;
static RadioSDataClient *reader;
static RadioSAudioEngine *engine;
static guint status_calls, library_calls, tracks_calls, search_calls, match_calls, player_starts;
static gchar *last_video;
static RadioSYouTubeMusicSnapshot player_snapshot;
static const gchar *generation = "6f1b8c1e-6c0e-4c8a-9a51-3d5b8f0b9c11";
typedef struct { RadioSViewContext ctx; RadioSConfig config; GtkWidget *window, *page; } Harness;

static JsonNode *parse(const gchar *text)
{
    JsonParser *parser = json_parser_new(); GError *error = NULL;
    g_assert_true(json_parser_load_from_data(parser, text, -1, &error)); g_assert_no_error(error);
    JsonNode *node = json_node_copy(json_parser_get_root(parser)); g_object_unref(parser); return node;
}
static gchar *items_json(const gchar *prefix, guint count)
{
    GString *text = g_string_new("[");
    for (guint i = 0; i < count; ++i)
        g_string_append_printf(text, "%s{\"provider_id\":\"%s%07u\",\"url\":\"https://music.youtube.com/watch?v=%s%07u\","
            "\"title\":\"%s title %u\",\"artist\":\"Online artist\",\"duration_ms\":%u}", i ? "," : "", prefix, i, prefix, i, prefix, i, 180000 + i * 1000);
    g_string_append_c(text, ']'); return g_string_free(text, FALSE);
}
JsonNode *__wrap_radios_youtube_music_request_sync(const RadioSConfig *request_config,
    const gchar *station_id, const gchar *user_id, const gchar *action,
    JsonNode *payload, GCancellable *cancel, GError **error)
{
    (void)cancel; (void)error; g_assert_nonnull(request_config->project_root);
    g_assert_true(g_uuid_string_is_valid(station_id)); g_assert_true(g_uuid_string_is_valid(user_id));
    JsonObject *body = payload ? json_node_get_object(payload) : NULL;
    if (g_str_equal(action, "status")) {
        ++status_calls;
        gchar *text = g_strdup_printf("{\"provider\":\"youtube\",\"configured\":true,\"connected\":true,\"display_name\":\"Broadcast account\","
            "\"channel_handle\":\"@radio\",\"auth_generation\":\"%s\"}", generation);
        JsonNode *node = parse(text); g_free(text); return node;
    }
    g_assert_cmpstr(radios_json_string(body, "expected_auth_generation", ""), ==, generation);
    gchar *text = NULL;
    if (g_str_equal(action, "library_playlists")) {
        ++library_calls;
        text = g_strdup_printf("{\"provider\":\"youtube\",\"playlists\":[{\"id\":\"PLonlineBroadcast1\",\"name\":\"Account list\",\"count\":3,\"owned\":true}],"
            "\"truncated\":false,\"auth_generation\":\"%s\"}", generation);
    } else if (g_str_equal(action, "tracks")) {
        ++tracks_calls; g_assert_cmpstr(radios_json_string(body, "playlist_id", ""), ==, "PLonlineBroadcast1");
        gchar *items = items_json("Acct", 3);
        text = g_strdup_printf("{\"provider\":\"youtube\",\"name\":\"Account list\",\"items\":%s,\"truncated\":false,"
            "\"unavailable_count\":0,\"auth_generation\":\"%s\"}", items, generation); g_free(items);
    } else if (g_str_equal(action, "match")) {
        /* "Spot title N" matches MtchN except N == 1; indexes are batch positions. */
        ++match_calls; JsonArray *items = json_object_get_array_member(body, "items"); GString *matches = g_string_new("[");
        g_assert_cmpuint(json_array_get_length(items), >=, 1); g_assert_cmpuint(json_array_get_length(items), <=, 8);
        for (guint i = 0; i < json_array_get_length(items); ++i) {
            JsonObject *item = json_array_get_object_element(items, i); const gchar *title = radios_json_string(item, "title", "");
            g_assert_cmpint(radios_json_int(item, "index", -1), ==, i); g_assert_true(g_str_has_prefix(title, "Spot title "));
            g_assert_cmpstr(radios_json_string(item, "artist", ""), ==, "Spot artist");
            guint number = (guint)g_ascii_strtoull(title + strlen("Spot title "), NULL, 10);
            if (number == 1) g_string_append_printf(matches, "%s{\"index\":%u,\"matched\":false}", i ? "," : "", i);
            else g_string_append_printf(matches, "%s{\"index\":%u,\"matched\":true,\"item\":{\"provider_id\":\"Mtch%07u\","
                "\"url\":\"https://music.youtube.com/watch?v=Mtch%07u\",\"title\":\"YT %s\",\"artist\":\"Online artist\",\"duration_ms\":%u}}",
                i ? "," : "", i, number, number, title, 200000 + number * 1000);
        }
        g_string_append_c(matches, ']');
        text = g_strdup_printf("{\"provider\":\"youtube\",\"matches\":%s,\"attempted\":%u,\"auth_generation\":\"%s\"}",
            matches->str, json_array_get_length(items), generation);
        g_string_free(matches, TRUE);
    } else {
        g_assert_cmpstr(action, ==, "search"); ++search_calls;
        g_assert_cmpstr(radios_json_string(body, "query", ""), ==, "online query");
        gchar *items = items_json("Find", 2);
        text = g_strdup_printf("{\"provider\":\"youtube\",\"items\":%s,\"auth_generation\":\"%s\"}", items, generation); g_free(items);
    }
    JsonNode *node = parse(text); g_free(text); return node;
}
gboolean __wrap_radios_audio_ui_ready(RadioSViewContext *ctx, RadioSAudioUiBus bus)
{ (void)ctx; return bus == RADIOS_AUDIO_UI_MAIN; }
RadioSAudioEngine *__wrap_radios_audio_ui_engine_acquire(RadioSViewContext *ctx)
{ (void)ctx; return radios_audio_engine_ref(engine); }
gboolean __wrap_radios_youtube_music_player_start(RadioSYouTubeMusicPlayer *player,
    const RadioSConfig *request_config, RadioSAudioEngine *request_engine,
    const gchar *station_id, const gchar *user_id, const gchar *auth_generation,
    const gchar *video_id, gdouble offset, GError **error)
{
    (void)player; (void)error; g_assert_nonnull(request_config); g_assert_true(request_engine == engine);
    g_assert_true(g_uuid_string_is_valid(station_id)); g_assert_true(g_uuid_string_is_valid(user_id));
    g_assert_cmpstr(auth_generation, ==, generation); g_assert_cmpfloat(offset, ==, 0);
    ++player_starts; g_free(last_video); last_video = g_strdup(video_id);
    memset(&player_snapshot, 0, sizeof(player_snapshot)); player_snapshot.state = RADIOS_YOUTUBE_MUSIC_PLAYING;
    g_strlcpy(player_snapshot.video_id, video_id, sizeof(player_snapshot.video_id)); player_snapshot.main_routed = TRUE;
    return TRUE;
}
gboolean __wrap_radios_youtube_music_player_pause(RadioSYouTubeMusicPlayer *player, gboolean paused, GError **error)
{ (void)player; (void)error; player_snapshot.state = paused ? RADIOS_YOUTUBE_MUSIC_PAUSED : RADIOS_YOUTUBE_MUSIC_PLAYING; return TRUE; }
void __wrap_radios_youtube_music_player_stop(RadioSYouTubeMusicPlayer *player)
{ (void)player; player_snapshot.state = RADIOS_YOUTUBE_MUSIC_STOPPED; player_snapshot.main_routed = FALSE; }
void __wrap_radios_youtube_music_player_snapshot(RadioSYouTubeMusicPlayer *player, RadioSYouTubeMusicSnapshot *snapshot)
{ (void)player; *snapshot = player_snapshot; }
static gchar *prepared_video;
static guint prepared_count;
static guint prepared_flow_tracks;
void __wrap_radios_youtube_music_player_prepare_list(RadioSYouTubeMusicPlayer *player, const RadioSConfig *settings,
    const gchar *station_id, const gchar *user_id, const gchar *auth_generation, const gchar *const *videos, guint count)
{
    (void)player; prepared_count = count; prepared_flow_tracks = 0;
    if (!count) return;
    g_assert_nonnull(settings); g_assert_true(g_uuid_string_is_valid(station_id)); g_assert_true(g_uuid_string_is_valid(user_id));
    g_assert_cmpstr(auth_generation, ==, generation);
    for (guint position = 0; position < count; ++position) {
        g_assert_cmpuint(strlen(videos[position]), ==, 11);
        if (g_str_has_prefix(videos[position], "Flow")) ++prepared_flow_tracks;
    }
}
void __wrap_radios_youtube_music_player_preload(RadioSYouTubeMusicPlayer *player, const RadioSConfig *request_config,
    const gchar *station_id, const gchar *user_id, const gchar *auth_generation, const gchar *video_id)
{
    (void)player;
    if (!video_id) { g_clear_pointer(&prepared_video, g_free); return; }
    g_assert_nonnull(request_config); g_assert_true(g_uuid_string_is_valid(station_id));
    g_assert_true(g_uuid_string_is_valid(user_id)); g_assert_cmpstr(auth_generation, ==, generation);
    g_free(prepared_video); prepared_video = g_strdup(video_id);
}

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (GTK_IS_MENU_BUTTON(root)) {
        GtkWidget *popover = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(root)));
        GtkWidget *result = popover ? find(popover,name) : NULL;
        if (result) return result;
    }
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = find(item->data, name);
    g_list_free(children); return found;
}
static void pump(void)
{ while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
typedef gboolean (*Condition)(gpointer);
static void wait_for(Condition condition, gpointer data, const gchar *description)
{
    gint64 deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;
    while (!condition(data) && g_get_monotonic_time() < deadline) pump();
    if (!condition(data)) g_error("Broadcast online catalogue timed out: %s", description);
}
static void click(GtkWidget *root, const gchar *name)
{
    GtkWidget *button = find(root, name); g_assert_true(GTK_IS_BUTTON(button));
    g_assert_true(gtk_widget_is_sensitive(button)); gtk_button_clicked(GTK_BUTTON(button));
}
static GtkTreeView *catalog_tree(Harness *h)
{ GtkWidget *table = find(h->page, "broadcast-media-table"); g_assert_nonnull(table); return g_object_get_data(G_OBJECT(table), "tree-view"); }
static gint rows(Harness *h)
{ return gtk_tree_model_iter_n_children(gtk_tree_view_get_model(catalog_tree(h)), NULL); }
static gchar *cell(Harness *h, gint row, gint column)
{
    GtkTreeModel *model = gtk_tree_view_get_model(catalog_tree(h)); GtkTreeIter iter; gchar *value = NULL;
    g_assert_true(gtk_tree_model_iter_nth_child(model, &iter, NULL, row)); gtk_tree_model_get(model, &iter, column, &value, -1);
    return value;
}
static void select_row(Harness *h, gint row)
{
    GtkTreeSelection *selection = gtk_tree_view_get_selection(catalog_tree(h));
    gtk_tree_selection_unselect_all(selection);
    GtkTreePath *path = gtk_tree_path_new_from_indices(row, -1);
    gtk_tree_selection_select_path(selection, path); gtk_tree_path_free(path);
}
static gboolean connected(gpointer data)
{ Harness *h = data; return radios_data_connected(&h->ctx); }
static Harness *open_harness(const gchar *token_env)
{
    Harness *h = g_new0(Harness, 1); h->config = config;
    h->config.token_environment = g_strdup(token_env);
    h->ctx.config = &h->config; h->ctx.runtime = radios_runtime_new(32, 128);
    h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); h->ctx.parent = GTK_WINDOW(h->window);
    h->ctx.data = radios_data_new(&h->ctx); h->page = radios_broadcast_new(&h->ctx);
    gtk_container_add(GTK_CONTAINER(h->window), h->page); gtk_widget_show_all(h->window);
    radios_data_connect(&h->ctx); wait_for(connected, h, "genuine station identity"); return h;
}
static void close_harness(Harness *h)
{
    gtk_widget_destroy(h->window);
    radios_data_close(h->ctx.data); radios_runtime_free(h->ctx.runtime); radios_data_unref(h->ctx.data);
    for (guint i = 0; i < 100; ++i) pump();
    g_free(h->config.token_environment); g_free(h);
}
static gchar *seed_playlist(const gchar *provider, const gchar *name, gchar *items)
{
    gchar *document = g_uuid_string_random(), *key = g_uuid_string_random();
    gchar *text = g_strdup_printf("{\"kind\":\"external_playlist\",\"name\":\"%s\",\"config\":{\"schema_version\":1,"
        "\"provider\":\"%s\",\"remote_id\":\"\",\"remote_url\":\"\",\"description\":\"\",\"items\":%s},"
        "\"idempotency_key\":\"%s\",\"document_id\":\"%s\",\"expected_revision\":0}", name, provider, items, key, document);
    JsonNode *body = parse(text); g_free(text); g_free(items); g_free(key);
    json_object_set_string_member(json_node_get_object(body), "station_id", radios_data_client_station_id(reader));
    GError *error = NULL; JsonNode *result = radios_data_client_request_sync(reader, "POST", "/v1/workflows/desktop/put", body, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(result); json_node_free(result); json_node_free(body);
    return document;
}
static gchar *seed_saved_playlist(void)
{ return seed_playlist("youtube", "Saved online list", items_json("Save", 2)); }
/* Three Spotify tracks; the bridge fake matches all but "Spot title 1". */
static gchar *seed_spotify_playlist(void)
{
    GString *items = g_string_new("[");
    for (guint i = 0; i < 3; ++i)
        g_string_append_printf(items, "%s{\"provider_id\":\"Spot%018u\",\"url\":\"https://open.spotify.com/track/Spot%018u\","
            "\"title\":\"Spot title %u\",\"artist\":\"Spot artist\",\"duration_ms\":%u}", i ? "," : "", i, i, i, 200000 + i * 1000);
    g_string_append_c(items, ']');
    return seed_playlist("spotify", "Saved spotify list", g_string_free(items, FALSE));
}
typedef struct { Harness *h; const gchar *id; } Choice;
static gboolean choice_ready(gpointer data)
{ Choice *choice = data; return gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(choice->h->page, "broadcast-online-playlists")), choice->id); }
typedef struct { Harness *h; gint count; const gchar *prefix; } Rows;
static gboolean rows_ready(gpointer data)
{
    Rows *expected = data; if (rows(expected->h) != expected->count) return FALSE;
    gchar *title = cell(expected->h, 0, 1); gboolean ok = g_str_has_prefix(title, expected->prefix); g_free(title); return ok;
}
static gboolean player_started(gpointer data)
{ return player_starts >= GPOINTER_TO_UINT(data); }
typedef struct { Harness *harness; guint matched; } PreparedRows;
static gboolean recordings_prepared(gpointer data)
{
    PreparedRows *expected = data;
    RadioSBroadcastOnline *online = g_object_get_data(G_OBJECT(expected->harness->page), "radios-broadcast-online");
    JsonArray *items = radios_broadcast_online_items(online, TRUE); guint matched = 0;
    for (guint index = 0; items && index < json_array_get_length(items); ++index) {
        JsonObject *item = json_array_get_object_element(items, index);
        if (strlen(radios_json_string(item, "youtube_id", "")) == 11) ++matched;
    }
    if (items) json_array_unref(items);
    return matched == expected->matched && prepared_count >= matched;
}
static gboolean idle(gpointer data)
{ Harness *h = data; return gtk_widget_is_sensitive(find(h->page, "broadcast-online-play")); }
static void select_source(Harness *harness, guint source)
{
    const gchar *names[] = {"broadcast-source-archive","broadcast-source-youtube","broadcast-source-spotify","broadcast-source-apple"};
    for (guint index = 0; index < 4; ++index)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(harness->page,names[index])),index == source);
}
static gboolean denied(gpointer data)
{
    Harness *h = data; GtkWidget *status = find(h->page, "broadcast-catalog-status");
    return idle(h) && strstr(gtk_label_get_text(GTK_LABEL(status)), "yetkisi") != NULL;
}

static void test_youtube_source_lists_and_plays_account_saved_and_search(void)
{
    gchar *saved = seed_saved_playlist();
    Harness *h = open_harness("RADIOS_TEST_ONLINE_OPERATE_TOKEN");
    GtkWidget *source = find(h->page, "broadcast-catalog-source"), *row = find(h->page, "broadcast-online-source");
    g_assert_false(gtk_widget_get_visible(row));
    g_assert_true(GTK_IS_MENU_BUTTON(source)); select_source(h,1);
    g_assert_true(gtk_widget_get_visible(row)); g_assert_true(gtk_widget_get_visible(find(h->page, "broadcast-online-player")));
    Choice account = {h, "acct:PLonlineBroadcast1"};
    wait_for(choice_ready, &account, "verified account playlists listed in catalogue");
    Rows expected = {h, 3, "Acct"}; wait_for(rows_ready, &expected, "account playlist tracks fill catalogue table");
    g_assert_cmpuint(prepared_count, ==, 3);
    gchar *kind = cell(h, 0, 3), *origin = cell(h, 0, 5);
    g_assert_cmpstr(kind, ==, "YouTube Music"); g_assert_nonnull(strstr(origin, "Account list")); g_free(kind); g_free(origin);
    g_assert_false(gtk_widget_get_sensitive(find(h->page, "broadcast-catalog-insert")));
    select_row(h, 1); guint before = player_starts; click(h->page, "broadcast-online-play");
    wait_for(player_started, GUINT_TO_POINTER(before + 1), "selected track plays after fresh operate authorization");
    g_assert_cmpstr(last_video, ==, "Acct0000001");
    wait_for(idle, h, "controls released"); click(h->page, "broadcast-online-next");
    wait_for(player_started, GUINT_TO_POINTER(before + 2), "next track"); g_assert_cmpstr(last_video, ==, "Acct0000002");
    wait_for(idle, h, "controls released after next");
    player_snapshot.state = RADIOS_YOUTUBE_MUSIC_FINISHED;
    for (guint i = 0; i < 1500; ++i) pump();
    g_assert_cmpuint(player_starts, ==, before + 2);
    player_snapshot.state = RADIOS_YOUTUBE_MUSIC_PLAYING; click(h->page, "broadcast-online-stop");
    g_assert_cmpint(player_snapshot.state, ==, RADIOS_YOUTUBE_MUSIC_STOPPED);
    gchar *saved_id = g_strdup_printf("db:%s", saved); Choice stored = {h, saved_id};
    wait_for(choice_ready, &stored, "RadioS saved YouTube Music list offered");
    Rows saved_rows = {h, 2, "Save"}; wait_for(rows_ready, &saved_rows, "saved list renders without provider request");
    select_row(h, 0); before = player_starts; click(h->page, "broadcast-online-play");
    wait_for(player_started, GUINT_TO_POINTER(before + 1), "saved list plays"); g_assert_cmpstr(last_video, ==, "Save0000000");
    wait_for(idle, h, "controls released for search"); click(h->page, "broadcast-online-stop");
    gtk_entry_set_text(GTK_ENTRY(find(h->page, "broadcast-catalog-query")), "online query");
    g_signal_emit_by_name(find(h->page, "broadcast-catalog-query"), "activate");
    Rows found = {h, 2, "Find"}; wait_for(rows_ready, &found, "YouTube Music search fills catalogue");
    g_assert_cmpuint(search_calls, ==, 1);
    gtk_entry_set_text(GTK_ENTRY(find(h->page, "broadcast-catalog-query")), "");
    select_source(h,0);
    g_assert_false(gtk_widget_get_visible(row)); g_assert_false(gtk_widget_get_visible(find(h->page, "broadcast-online-player")));
    for (guint i = 0; i < 300; ++i) pump();
    g_assert_cmpint(rows(h), ==, 0);
    g_free(saved_id); g_free(saved); close_harness(h);
}
/* Current "Mevcut program" draft as stored by SDBASE. */
static JsonNode *programme_config(void)
{
    JsonNode *body = parse("{\"kind\":\"programme\"}"); GError *error = NULL;
    json_object_set_string_member(json_node_get_object(body), "station_id", radios_data_client_station_id(reader));
    JsonNode *result = radios_data_client_request_sync(reader, "POST", "/v1/workflows/desktop/list", body, NULL, &error);
    g_assert_no_error(error); json_node_free(body); JsonNode *config = NULL;
    JsonArray *records = json_object_get_array_member(json_node_get_object(result), "records");
    for (guint i = 0; i < json_array_get_length(records); ++i) {
        JsonObject *record = json_array_get_object_element(records, i);
        if (g_str_equal(radios_json_string(record, "name", ""), "Mevcut program")) config = json_node_copy(json_object_get_member(record, "config"));
    }
    json_node_free(result); return config;
}
typedef struct { guint blocks, events; } Shape;
static gboolean programme_shape(gpointer data)
{
    Shape *want = data; JsonNode *node = programme_config(); if (!node) return FALSE;
    JsonObject *config = json_node_get_object(node);
    JsonArray *blocks = json_object_has_member(config, "blocks") ? json_object_get_array_member(config, "blocks") : NULL;
    gboolean match = blocks && json_array_get_length(blocks) == want->blocks &&
        json_array_get_length(json_object_get_array_member(config, "events")) == want->events;
    json_node_free(node); return match;
}
static gboolean programme_ready(gpointer data)
{ Harness *h = data; RadioSDocument *document = radios_broadcast_programme_document(&h->ctx); return document && radios_data_document_ready(document); }
static gboolean button_sensitive(gpointer data)
{ return gtk_widget_is_sensitive(GTK_WIDGET(data)); }
extern const gchar *const radios_reference_css_parts[];
/* RADIOS_SHOT_DIR=… saves the styled page as PNG for visual review. */
static void shoot(Harness *h, const gchar *name)
{
    const gchar *directory = g_getenv("RADIOS_SHOT_DIR"); if (!directory) return;
    static GtkCssProvider *provider;
    if (!provider) {
        GString *css = g_string_new(NULL); gchar *adaptation = NULL;
        for (guint i = 0; radios_reference_css_parts[i]; ++i) g_string_append(css, radios_reference_css_parts[i]);
        gchar *path = g_build_filename(config.assets_root, "themes", "radios.css", NULL);
        g_assert_true(g_file_get_contents(path, &adaptation, NULL, NULL)); g_string_append(css, adaptation); g_free(adaptation); g_free(path);
        provider = gtk_css_provider_new(); g_assert_true(gtk_css_provider_load_from_data(provider, css->str, -1, NULL));
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_string_free(css, TRUE);
    }
    gtk_style_context_add_class(gtk_widget_get_style_context(h->window), "radios");
    gtk_window_resize(GTK_WINDOW(h->window), 1800, 1060);
    for (guint i = 0; i < 400; ++i) pump();
    GtkAllocation allocation; gtk_widget_get_allocation(h->window, &allocation);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
    cairo_t *cr = cairo_create(surface); gtk_widget_draw(h->window, cr); cairo_destroy(cr);
    gchar *file = g_build_filename(directory, name, NULL); g_assert_cmpint(cairo_surface_write_to_png(surface, file), ==, CAIRO_STATUS_SUCCESS);
    g_free(file); cairo_surface_destroy(surface);
}

/* The flow keeps events (blocks); YouTube Music rows join an event or become one. */
static void test_youtube_items_become_flow_events(void)
{
    Harness *h = open_harness("RADIOS_TEST_ONLINE_OPERATE_TOKEN");
    wait_for(programme_ready, h, "programme draft loaded");
    click(h->page, "programme-add-music");
    Shape empty = {1, 0}; wait_for(programme_shape, &empty, "empty music event saved");
    select_source(h,1);
    Choice account = {h, "acct:PLonlineBroadcast1"}; wait_for(choice_ready, &account, "account playlists");
    Rows expected = {h, 3, "Acct"}; wait_for(rows_ready, &expected, "account tracks listed");
    select_row(h, 0);
    wait_for(button_sensitive, find(h->page, "broadcast-catalog-insert"), "insert offered for a YouTube row");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(h->page, "broadcast-catalog-title"))), ==, "Acct title 0");
    click(h->page, "broadcast-catalog-insert");
    Shape one = {1, 1}; wait_for(programme_shape, &one, "YouTube row appended to the selected event");
    JsonNode *node = programme_config(); JsonObject *config = json_node_get_object(node);
    JsonObject *event = json_array_get_object_element(json_object_get_array_member(config, "events"), 0);
    JsonObject *block = json_array_get_object_element(json_object_get_array_member(config, "blocks"), 0);
    g_assert_cmpstr(radios_json_string(event, "online_id", ""), ==, "Acct0000000");
    g_assert_cmpstr(radios_json_string(event, "online_url", ""), ==, "https://music.youtube.com/watch?v=Acct0000000");
    g_assert_cmpstr(radios_json_string(event, "block_id", ""), ==, radios_json_string(block, "id", ""));
    g_assert_cmpstr(radios_json_string(block, "title", ""), ==, "Müzik eventi");
    json_node_free(node);
    wait_for(button_sensitive, find(h->page, "broadcast-catalog-all"), "whole list offered");
    click(h->page, "broadcast-catalog-all");
    Shape all = {2, 4}; wait_for(programme_shape, &all, "whole YouTube list became one event");
    node = programme_config(); config = json_node_get_object(node);
    block = json_array_get_object_element(json_object_get_array_member(config, "blocks"), 1);
    g_assert_cmpstr(radios_json_string(block, "title", ""), ==, "YouTube · Account list");
    GtkListBox *list = g_object_get_data(G_OBJECT(find(h->page, "programme-queue-table")), "list-box");
    g_assert_null(gtk_list_box_get_row_at_index(list, 3));
    JsonArray *events = json_object_get_array_member(config, "events");
    for (guint index = 0; index < 3; ++index) {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(list, (gint)index);
        g_assert_nonnull(row);
        g_assert_cmpstr(radios_programme_queue_row_id(row), ==,
            radios_json_string(json_array_get_object_element(events, index + 1), "id", ""));
    }
    json_node_free(node);
    shoot(h, "flow-contents.png");
    click(h->page, "programme-queue-back"); pump();
    g_assert_nonnull(gtk_list_box_get_row_at_index(list, 1));
    g_assert_null(gtk_list_box_get_row_at_index(list, 2));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(h->page, "programme-queue-count"))), ==, "2 event · 4 içerik");
    shoot(h, "flow-events.png");
    close_harness(h);
}
static gboolean flow_stopped(gpointer data)
{ return !radios_broadcast_online_flow_running(data); }
static gboolean flow_prepared(gpointer data)
{ return prepared_flow_tracks == GPOINTER_TO_UINT(data); }
/* ▶ of a flow with YouTube Music events: each plays in turn on the shared
 * player, contents without audio are skipped, Ⅱ/■ act on the sounding part. */
static void test_flow_plays_youtube_events(void)
{
    Harness *h = open_harness("RADIOS_TEST_ONLINE_OPERATE_TOKEN");
    wait_for(programme_ready, h, "programme draft loaded before preparation");
    wait_for(idle, h, "background account status finished");
    RadioSBroadcastOnline *online = g_object_get_data(G_OBJECT(h->page), "radios-broadcast-online"); g_assert_nonnull(online);
    gchar *text = g_strdup_printf("{\"title\":\"Akış\",\"date\":\"2026-10-07\",\"note\":\"\",\"events\":["
        "{\"id\":\"11111111-1111-4111-8111-111111111111\",\"title\":\"Birinci\",\"online_id\":\"FlowAAAAAAA\"},"
        "{\"id\":\"22222222-2222-4222-8222-222222222222\",\"title\":\"Taslak\"},"
        "{\"id\":\"33333333-3333-4333-8333-333333333333\",\"title\":\"İkinci\",\"online_id\":\"FlowBBBBBBB\"}]}");
    JsonNode *programme = parse(text); g_free(text); JsonObject *config = json_node_get_object(programme);
    JsonArray *header_events = json_object_get_array_member(config, "events");
    json_object_set_int_member(json_array_get_object_element(header_events, 0), "duration_ms", 180000);
    json_object_set_int_member(json_array_get_object_element(header_events, 2), "duration_ms", 200000);
    g_assert_true(radios_broadcast_online_flow_needed(config));
    guint before = player_starts; GError *error = NULL;
    radios_broadcast_online_prepare_items(online, json_object_get_array_member(config, "events"), TRUE);
    wait_for(flow_prepared, GUINT_TO_POINTER(2), "whole programme prepared before play with the catalogue hidden");
    g_assert_cmpuint(player_starts, ==, before);
    g_assert_true(radios_broadcast_online_flow_start(online, config, 0, 0, 0, &error)); g_assert_no_error(error);
    RadioSBroadcastHeaderSnapshot header = {0};
    g_assert_true(radios_broadcast_online_header_snapshot(online, &header));
    g_assert_false(header.programme_active); /* Authorization/preparation is not current playback. */
    wait_for(player_started, GUINT_TO_POINTER(before + 1), "first YouTube Music event starts"); g_assert_cmpstr(last_video, ==, "FlowAAAAAAA");
    g_assert_cmpuint(prepared_flow_tracks, ==, 2);
    g_assert_cmpstr(prepared_video, ==, "FlowBBBBBBB");
    player_snapshot.position_seconds = 42.5;
    json_object_set_string_member(config, "title", "Sonradan düzenlenen taslak");
    g_assert_true(radios_broadcast_online_header_snapshot(online, &header));
    g_assert_true(header.current); g_assert_true(header.programme_active);
    g_assert_cmpstr(header.programme_title, ==, "Akış");
    g_assert_cmpstr(header.title, ==, "Birinci");
    g_assert_cmpuint(header.position_ms, ==, 42500); g_assert_cmpuint(header.duration_ms, ==, 180000);
    g_strlcpy(player_snapshot.video_id, "OtherAAAAAA", sizeof player_snapshot.video_id);
    g_assert_true(radios_broadcast_online_header_snapshot(online, &header));
    g_assert_false(header.current); g_assert_false(header.programme_active);
    g_strlcpy(player_snapshot.video_id, "FlowAAAAAAA", sizeof player_snapshot.video_id);
    for (guint i = 0; i < 700; ++i) pump();
    RadioSBroadcastFlowStatus status; radios_broadcast_online_flow_status(online, &status);
    g_assert_true(status.running); g_assert_cmpstr(status.playing, ==, "11111111-1111-4111-8111-111111111111"); g_assert_true(status.can_pause);
    /* Prompt3: the true next playable item skips the silent draft. */
    g_assert_cmpstr(status.next, ==, "33333333-3333-4333-8333-333333333333");
    g_assert_cmpuint(status.position_ms, ==, 42500);
    g_assert_cmpuint(status.duration_ms, ==, 180000);
    g_assert_false(status.paused);
    /* The catalogue cannot take the player from the flow. */
    select_source(h,1);
    Choice account = {h, "acct:PLonlineBroadcast1"}; wait_for(choice_ready, &account, "account playlists");
    Rows expected = {h, 3, "Acct"}; wait_for(rows_ready, &expected, "account tracks"); select_row(h, 0);
    g_assert_cmpuint(prepared_flow_tracks, ==, 2);
    wait_for(idle, h, "catalogue idle"); click(h->page, "broadcast-online-play");
    for (guint i = 0; i < 300; ++i) pump();
    g_assert_cmpuint(player_starts, ==, before + 1);
    g_assert_true(radios_broadcast_online_flow_action(online, RADIOS_PLAYOUT_PAUSE, &error)); g_assert_no_error(error);
    g_assert_cmpint(player_snapshot.state, ==, RADIOS_YOUTUBE_MUSIC_PAUSED);
    g_assert_true(radios_broadcast_online_header_snapshot(online, &header));
    g_assert_true(header.current); g_assert_true(header.paused); g_assert_true(header.programme_active);
    g_assert_cmpuint(header.active_sources, ==, 0);
    radios_broadcast_online_flow_status(online, &status);
    g_assert_true(status.paused);
    g_assert_cmpstr(status.next, ==, "33333333-3333-4333-8333-333333333333");
    g_assert_cmpuint(status.position_ms, ==, 42500);
    g_assert_cmpuint(status.duration_ms, ==, 180000);
    g_assert_true(radios_broadcast_online_flow_action(online, RADIOS_PLAYOUT_RESUME, &error)); g_assert_no_error(error);
    for (guint i = 0; i < 300 && player_snapshot.state != RADIOS_YOUTUBE_MUSIC_PLAYING; ++i) pump();
    g_assert_cmpint(player_snapshot.state, ==, RADIOS_YOUTUBE_MUSIC_PLAYING);
    player_snapshot.state = RADIOS_YOUTUBE_MUSIC_FINISHED;
    wait_for(player_started, GUINT_TO_POINTER(before + 2), "draft without audio skipped, next event starts");
    g_assert_cmpstr(last_video, ==, "FlowBBBBBBB");
    g_assert_true(radios_broadcast_online_header_snapshot(online, &header));
    g_assert_true(header.current); g_assert_cmpstr(header.title, ==, "İkinci");
    g_assert_cmpuint(header.duration_ms, ==, 200000);
    player_snapshot.position_seconds = 25.25;
    radios_broadcast_online_flow_status(online, &status);
    g_assert_cmpstr(status.playing, ==, "33333333-3333-4333-8333-333333333333");
    g_assert_cmpstr(status.next, ==, "");
    g_assert_cmpuint(status.position_ms, ==, 25250);
    g_assert_cmpuint(status.duration_ms, ==, 200000);
    for (guint i = 0; i < 700; ++i) pump();
    player_snapshot.state = RADIOS_YOUTUBE_MUSIC_FINISHED;
    wait_for(flow_stopped, online, "flow ends after the last event");
    g_assert_true(radios_broadcast_online_header_snapshot(online, &header));
    g_assert_false(header.current); g_assert_false(header.programme_active);
    radios_broadcast_online_flow_status(online, &status);
    g_assert_cmpstr(status.text, ==, "Bitti · ses dosyası olmayan 1 içerik atlandı");
    g_assert_false(status.running);
    g_assert_cmpstr(status.playing, ==, "");
    g_assert_cmpstr(status.next, ==, "");
    g_assert_cmpuint(status.position_ms, ==, 0);
    g_assert_cmpuint(status.duration_ms, ==, 0);
    g_assert_true(radios_broadcast_online_flow_start(online, config, 2, 0, 0, &error));
    wait_for(player_started, GUINT_TO_POINTER(before + 3), "start index honoured");
    g_assert_true(radios_broadcast_online_flow_action(online, RADIOS_PLAYOUT_STOP, &error));
    g_assert_false(radios_broadcast_online_flow_running(online)); g_assert_cmpint(player_snapshot.state, ==, RADIOS_YOUTUBE_MUSIC_STOPPED);
    radios_broadcast_online_flow_status(online, &status);
    g_assert_false(status.running);
    g_assert_cmpstr(status.playing, ==, "");
    g_assert_cmpstr(status.next, ==, "");
    g_assert_cmpuint(status.position_ms, ==, 0);
    g_assert_cmpuint(status.duration_ms, ==, 0);
    JsonArray *completed = radios_broadcast_online_flow_take_completed(online);
    g_assert_cmpuint(json_array_get_length(completed), ==, 0);
    json_array_unref(completed);
    json_node_free(programme); close_harness(h);
}
static GtkWidget *dialog_find(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *result = NULL;
    for (GList *item = windows; item && !result; item = item->next)
        if (g_str_equal(gtk_widget_get_name(item->data), name)) result = item->data;
    g_list_free(windows); return result;
}
typedef struct { GtkWidget *table; const gchar *title; gint row; } TableRow;
static gboolean table_row(gpointer data)
{
    TableRow *want = data; GtkTreeModel *model = GTK_TREE_MODEL(g_object_get_data(G_OBJECT(want->table), "list-store")); GtkTreeIter iter; gint index = 0;
    for (gboolean more = gtk_tree_model_get_iter_first(model, &iter); more; more = gtk_tree_model_iter_next(model, &iter), ++index) {
        gchar *title = NULL; gtk_tree_model_get(model, &iter, 0, &title, -1); gboolean same = !g_strcmp0(title, want->title); g_free(title);
        if (same) { want->row = index; return TRUE; }
    }
    return FALSE;
}
static gboolean table_count(gpointer data)
{ TableRow *want = data; return gtk_tree_model_iter_n_children(GTK_TREE_MODEL(g_object_get_data(G_OBJECT(want->table), "list-store")), NULL) == want->row; }
/* A live player card loads a provider list from "Playlist yükle" and plays it
 * in order; the next YouTube Music input is prepared while one plays. */
static void test_card_loads_and_plays_provider_list(void)
{
    Harness *h = open_harness("RADIOS_TEST_ONLINE_OPERATE_TOKEN");
    gtk_button_clicked(GTK_BUTTON(find(h->page, "deck-0-playlist")));
    GtkWidget *dialog = dialog_find("player-loader-dialog"); g_assert_nonnull(dialog);
    gtk_stack_set_visible_child_name(GTK_STACK(find(dialog, "player-loader-stack")), "providers");
    TableRow list = {find(dialog, "player-loader-providers"), "YouTube · Account list", -1};
    wait_for(table_row, &list, "account playlist offered by the loader");
    GtkTreePath *path = gtk_tree_path_new_from_indices(list.row, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(g_object_get_data(G_OBJECT(list.table), "tree-view")), path); gtk_tree_path_free(path);
    TableRow items = {find(dialog, "player-loader-providers-items"), NULL, 3}; wait_for(table_count, &items, "playlist tracks listed");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_cmpstr(gtk_expander_get_label(GTK_EXPANDER(find(h->page, "deck-0-inputs"))), ==, "Girdiler (3)");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(h->page, "deck-0-title"))), ==, "Acct title 0");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(h->page, "deck-0-metadata"))), ==, "Online artist · YouTube Music");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(h->page, "deck-0-next-title"))), ==, "Sıradaki  Online artist — Acct title 1");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(h->page, "deck-1-title"))), ==, "—");
    guint before = player_starts; gtk_button_clicked(GTK_BUTTON(find(h->page, "deck-0-play")));
    wait_for(player_started, GUINT_TO_POINTER(before + 1), "card starts its first YouTube Music input");
    g_assert_cmpstr(last_video, ==, "Acct0000000"); g_assert_cmpstr(prepared_video, ==, "Acct0000001");
    GtkWidget *players = gtk_widget_get_ancestor(find(h->page, "deck-0-title"), GTK_TYPE_STACK);
    g_assert_true(GTK_IS_STACK(players)); gtk_stack_set_transition_type(GTK_STACK(players), GTK_STACK_TRANSITION_TYPE_NONE);
    gtk_stack_set_visible_child_name(GTK_STACK(players), "players");
    shoot(h, "player-metadata.png");
    gtk_button_clicked(GTK_BUTTON(find(h->page, "deck-0-next")));
    wait_for(player_started, GUINT_TO_POINTER(before + 2), "card advances its own metadata with the online input");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(h->page, "deck-0-title"))), ==, "Acct title 1");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(h->page, "deck-0-next-title"))), ==, "Sıradaki  Online artist — Acct title 2");
    gtk_button_clicked(GTK_BUTTON(find(h->page, "deck-0-stop")));
    g_assert_cmpint(player_snapshot.state, ==, RADIOS_YOUTUBE_MUSIC_STOPPED);
    close_harness(h);
}
/* Spotify lists saved in RadioS: rows play, enter the flow and load on a card
 * through their YouTube Music matches; the unmatched row is skipped. */
static Shape programme_counts(void)
{
    Shape shape = {0, 0}; JsonNode *node = programme_config(); if (!node) return shape;
    JsonObject *config = json_node_get_object(node);
    if (json_object_has_member(config, "blocks")) shape.blocks = json_array_get_length(json_object_get_array_member(config, "blocks"));
    shape.events = json_array_get_length(json_object_get_array_member(config, "events"));
    json_node_free(node); return shape;
}
static gboolean status_is(gpointer data)
{
    gpointer *pair = data;
    return !g_strcmp0(gtk_label_get_text(GTK_LABEL(find(pair[0], "broadcast-catalog-status"))), pair[1]);
}
static void test_spotify_list_plays_through_youtube_matches(void)
{
    gchar *saved = seed_spotify_playlist();
    Harness *h = open_harness("RADIOS_TEST_ONLINE_OPERATE_TOKEN");
    wait_for(programme_ready, h, "programme draft loaded");
    guint searches = search_calls, matches = match_calls;
    select_source(h,2);
    gchar *saved_id = g_strdup_printf("db:%s", saved); Choice stored = {h, saved_id};
    wait_for(choice_ready, &stored, "saved Spotify list offered");
    Rows listed = {h, 3, "Spot"}; wait_for(rows_ready, &listed, "Spotify rows listed");
    PreparedRows prepared = {h, 2}; wait_for(recordings_prepared, &prepared, "whole Spotify list matched and prepared before play");
    g_assert_cmpuint(match_calls, >, matches); guint warmed_calls = match_calls;
    g_assert_cmpstr(prepared_video, ==, "Mtch0000000");
    gchar *kind = cell(h, 0, 3); g_assert_cmpstr(kind, ==, "Spotify"); g_free(kind);
    GtkWidget *query = find(h->page, "broadcast-catalog-query");
    gtk_entry_set_text(GTK_ENTRY(query), "TITLE 2"); g_signal_emit_by_name(query, "activate");
    Rows filtered = {h, 1, "Spot title 2"}; wait_for(rows_ready, &filtered, "Spotify list searched in place");
    gtk_entry_set_text(GTK_ENTRY(query), ""); g_signal_emit_by_name(query, "activate");
    wait_for(rows_ready, &listed, "whole Spotify list again");
    g_assert_cmpuint(search_calls, ==, searches); g_assert_cmpuint(match_calls, ==, warmed_calls);
    select_row(h, 0); guint before = player_starts; click(h->page, "broadcast-online-play");
    wait_for(player_started, GUINT_TO_POINTER(before + 1), "Spotify row plays as its YouTube Music match");
    g_assert_cmpstr(last_video, ==, "Mtch0000000"); g_assert_cmpuint(match_calls, ==, warmed_calls);
    wait_for(idle, h, "controls released"); click(h->page, "broadcast-online-next");
    wait_for(player_started, GUINT_TO_POINTER(before + 2), "unmatched row skipped"); g_assert_cmpstr(last_video, ==, "Mtch0000002");
    wait_for(idle, h, "controls released after next"); click(h->page, "broadcast-online-stop");
    Shape start = programme_counts();
    wait_for(button_sensitive, find(h->page, "broadcast-catalog-all"), "whole list offered");
    click(h->page, "broadcast-catalog-all");
    Shape all = {start.blocks + 1, start.events + 2}; wait_for(programme_shape, &all, "matched rows became one event");
    gpointer status[] = {h->page, "2 parça eklendi · 1 parça YouTube Music'te bulunamadı."};
    wait_for(status_is, status, "missing match reported");
    JsonNode *node = programme_config(); JsonArray *events = json_object_get_array_member(json_node_get_object(node), "events");
    JsonArray *blocks = json_object_get_array_member(json_node_get_object(node), "blocks");
    JsonObject *block = json_array_get_object_element(blocks, start.blocks);
    g_assert_cmpstr(radios_json_string(block, "title", ""), ==, "Spotify · Saved spotify list");
    guint found = 0;
    for (guint i = 0; i < json_array_get_length(events); ++i) {
        JsonObject *event = json_array_get_object_element(events, i);
        if (g_strcmp0(radios_json_string(event, "block_id", ""), radios_json_string(block, "id", ""))) continue;
        g_assert_cmpstr(radios_json_string(event, "online_provider", ""), ==, "youtube");
        g_assert_cmpstr(radios_json_string(event, "artist", ""), ==, "Spot artist");
        g_assert_cmpstr(radios_json_string(event, "online_id", ""), ==, found ? "Mtch0000002" : "Mtch0000000");
        g_assert_cmpstr(radios_json_string(event, "title", ""), ==, found ? "Spot title 2" : "Spot title 0");
        ++found;
    }
    g_assert_cmpuint(found, ==, 2); json_node_free(node);
    gtk_button_clicked(GTK_BUTTON(find(h->page, "deck-0-playlist")));
    GtkWidget *dialog = dialog_find("player-loader-dialog"); g_assert_nonnull(dialog);
    gtk_stack_set_visible_child_name(GTK_STACK(find(dialog, "player-loader-stack")), "providers");
    TableRow list = {find(dialog, "player-loader-providers"), "Spotify · Saved spotify list", -1};
    wait_for(table_row, &list, "Spotify list offered by the loader");
    GtkTreePath *path = gtk_tree_path_new_from_indices(list.row, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(g_object_get_data(G_OBJECT(list.table), "tree-view")), path); gtk_tree_path_free(path);
    TableRow items = {find(dialog, "player-loader-providers-items"), NULL, 3}; wait_for(table_count, &items, "Spotify tracks listed");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_cmpstr(gtk_expander_get_label(GTK_EXPANDER(find(h->page, "deck-0-inputs"))), ==, "Girdiler (3)");
    before = player_starts; gtk_button_clicked(GTK_BUTTON(find(h->page, "deck-0-play")));
    wait_for(player_started, GUINT_TO_POINTER(before + 1), "card plays the Spotify input's match");
    g_assert_cmpstr(last_video, ==, "Mtch0000000");
    gtk_button_clicked(GTK_BUTTON(find(h->page, "deck-0-stop")));
    g_assert_cmpint(player_snapshot.state, ==, RADIOS_YOUTUBE_MUSIC_STOPPED);
    g_free(saved_id); g_free(saved); close_harness(h);
}
static void test_apple_list_prepares_before_play(void)
{
    GString *items = g_string_new("[");
    for (guint index = 0; index < 3; ++index)
        g_string_append_printf(items, "%s{\"provider_id\":\"%u\",\"url\":\"https://music.apple.com/tr/song/fixture/%u\","
            "\"title\":\"Spot title %u\",\"artist\":\"Spot artist\",\"duration_ms\":%u}",
            index ? "," : "", 1230000 + index, 1230000 + index, index, 200000 + index * 1000);
    g_string_append_c(items, ']');
    gchar *saved = seed_playlist("apple_music", "Saved Apple list", g_string_free(items, FALSE));
    Harness *harness = open_harness("RADIOS_TEST_ONLINE_OPERATE_TOKEN");
    wait_for(programme_ready, harness, "programme loaded before Apple preparation");
    guint before = player_starts;
    select_source(harness,3);
    gchar *saved_id = g_strdup_printf("db:%s", saved); Choice stored = {harness, saved_id};
    wait_for(choice_ready, &stored, "saved Apple Music list offered");
    Rows listed = {harness, 3, "Spot"}; wait_for(rows_ready, &listed, "Apple tracks listed");
    gchar *kind = cell(harness, 0, 3); g_assert_cmpstr(kind, ==, "Apple Music"); g_free(kind);
    PreparedRows prepared = {harness, 2}; wait_for(recordings_prepared, &prepared, "whole Apple list prepared before play");
    g_assert_cmpuint(player_starts, ==, before); guint warmed_calls = match_calls;
    select_row(harness, 0); click(harness->page, "broadcast-online-play");
    wait_for(player_started, GUINT_TO_POINTER(before + 1), "prepared Apple recording plays");
    g_assert_cmpstr(last_video, ==, "Mtch0000000"); g_assert_cmpuint(match_calls, ==, warmed_calls);
    wait_for(idle, harness, "Apple controls released"); click(harness->page, "broadcast-online-stop");
    g_free(saved_id); g_free(saved); close_harness(harness);
}
static void test_viewer_cannot_play(void)
{
    Harness *h = open_harness("RADIOS_TEST_ONLINE_VIEW_TOKEN");
    select_source(h,1);
    Choice account = {h, "acct:PLonlineBroadcast1"}; wait_for(choice_ready, &account, "viewer lists playlists");
    Rows expected = {h, 3, "Acct"}; wait_for(rows_ready, &expected, "viewer sees tracks");
    guint before = player_starts; click(h->page, "broadcast-online-play");
    wait_for(denied, h, "view-only identity denied playback"); g_assert_cmpuint(player_starts, ==, before);
    close_harness(h);
}
static void test_multiple_sources_and_music_playlist_scope(void)
{
    gchar *saved = seed_playlist("apple_music","online query Apple list",g_strdup(
        "[{\"provider_id\":\"1564530960\",\"url\":\"https://music.apple.com/tr/song/1564530960\","
        "\"title\":\"online query Apple song\",\"artist\":\"Apple artist\",\"duration_ms\":180000}]"));
    Harness *harness = open_harness("RADIOS_TEST_ONLINE_OPERATE_TOKEN"); guint starts = player_starts;
    GtkWidget *query = find(harness->page,"broadcast-catalog-query");
    gtk_entry_set_text(GTK_ENTRY(query),"online query");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(harness->page,"broadcast-source-apple")),TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(harness->page,"broadcast-source-youtube")),TRUE);
    g_signal_emit_by_name(query,"activate");
    Rows merged = {harness,3,"Find"}; wait_for(rows_ready,&merged,"YouTube live search and saved Apple results merged without replacing archive selection");
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(find(harness->page,"broadcast-catalog-source"))),==,"3 kaynak");
    g_assert_null(find(harness->page, "broadcast-catalog-source-summary"));
    gchar *origin = cell(harness,2,5); g_assert_cmpstr(origin,==,"Apple Music · kayıtlı"); g_free(origin);
    g_assert_cmpuint(player_starts,==,starts);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(harness->page,"broadcast-source-youtube")),FALSE);
    Rows apple = {harness,1,"online query Apple song"}; wait_for(rows_ready,&apple,"deselecting YouTube removes only its results");
    gtk_combo_box_set_active(GTK_COMBO_BOX(find(harness->page,"broadcast-catalog-field")),1);
    Rows playlists = {harness,1,"online query Apple list"}; wait_for(rows_ready,&playlists,"Playlist scope searches list names rather than tracks");
    gchar *kind = cell(harness,0,3); g_assert_cmpstr(kind,==,"Playlist"); g_free(kind);
    select_row(harness,0); click(harness->page,"broadcast-catalog-edit");
    wait_for(rows_ready,&apple,"saved provider playlist opens its real tracks in the same catalogue");
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(find(harness->page,"broadcast-catalog-field"))),==,0);
    g_assert_cmpuint(player_starts,==,starts); g_free(saved); close_harness(harness);
}
static void test_source_menu_and_search_prompts(void)
{
    Harness *harness = open_harness("RADIOS_TEST_ONLINE_OPERATE_TOKEN"); guint starts = player_starts;
    GtkWidget *source = find(harness->page, "broadcast-catalog-source");
    GtkWidget *query = find(harness->page, "broadcast-catalog-query");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(harness->page, "broadcast-catalog-source-label"))), ==, "Arama kaynağı");
    g_assert_cmpstr(atk_object_get_name(gtk_widget_get_accessible(source)), ==, "Arama kaynağı");
    GtkWidget *arrow = gtk_button_get_image(GTK_BUTTON(source)); g_assert_true(GTK_IS_IMAGE(arrow));
    const gchar *icon = NULL; GtkIconSize size; gtk_image_get_icon_name(GTK_IMAGE(arrow), &icon, &size);
    g_assert_cmpstr(icon, ==, "pan-down-symbolic"); g_assert_true(gtk_button_get_always_show_image(GTK_BUTTON(source)));
    GtkWidget *menu = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(source)));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(source), TRUE); g_assert_true(gtk_widget_get_visible(menu));
    const gchar *choices[] = {"broadcast-source-archive", "broadcast-source-youtube", "broadcast-source-spotify", "broadcast-source-apple"};
    const gchar *providers[] = {"Medya arşivi", "YouTube Music", "Spotify", "Apple Music"};
    for (guint index = 0; index < G_N_ELEMENTS(choices); ++index) {
        GtkWidget *choice = find(menu, choices[index]); g_assert_true(GTK_IS_CHECK_BUTTON(choice));
        g_assert_true(gtk_widget_get_visible(choice)); g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(choice)), ==, providers[index]);
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(source), FALSE);
    gtk_entry_set_text(GTK_ENTRY(query), "online query");
    for (guint index = 0; index < G_N_ELEMENTS(providers); ++index) {
        select_source(harness, index);
        g_assert_nonnull(strstr(gtk_entry_get_placeholder_text(GTK_ENTRY(query)), providers[index]));
        g_assert_nonnull(strstr(gtk_entry_get_placeholder_text(GTK_ENTRY(query)), "Parça"));
        g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(query)), ==, "online query");
    }
    gtk_combo_box_set_active(GTK_COMBO_BOX(find(harness->page, "broadcast-catalog-field")), 1);
    g_assert_cmpstr(gtk_entry_get_placeholder_text(GTK_ENTRY(query)), ==, "Apple Music · Playlist ara…");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(harness->page, "broadcast-source-youtube")), TRUE);
    g_assert_cmpstr(gtk_entry_get_placeholder_text(GTK_ENTRY(query)), ==, "Seçili 2 kaynakta playlist ara…");
    for (guint index = 0; index < G_N_ELEMENTS(choices); ++index)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(menu, choices[index])), FALSE);
    g_assert_cmpstr(gtk_entry_get_placeholder_text(GTK_ENTRY(query)), ==, "Önce arama kaynağı seçin");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(query)), ==, "online query");
    g_assert_cmpuint(player_starts, ==, starts); close_harness(harness);
}
static void test_multiple_selected_rows_insert_and_play_order(void)
{
    Harness *h = open_harness("RADIOS_TEST_ONLINE_OPERATE_TOKEN");
    wait_for(programme_ready, h, "programme ready before selected-only online list");
    click(h->page, "programme-add-music");
    wait_for(programme_ready, h, "new target event saved before selected-only online list");
    JsonNode *before = programme_config(); JsonObject *config_object = json_node_get_object(before);
    guint count = json_array_get_length(json_object_get_array_member(config_object, "events"));
    guint blocks = json_array_get_length(json_object_get_array_member(config_object, "blocks"));
    select_source(h, 1);
    Choice account = {h, "acct:PLonlineBroadcast1"}; wait_for(choice_ready, &account, "account lists ready for multi selection");
    Rows expected = {h, 3, "Acct"}; wait_for(rows_ready, &expected, "three online tracks available");
    GtkTreeSelection *selection = gtk_tree_view_get_selection(catalog_tree(h));
    g_assert_cmpint(gtk_tree_selection_get_mode(selection), ==, GTK_SELECTION_MULTIPLE);
    gtk_tree_selection_unselect_all(selection);
    GtkTreePath *path = gtk_tree_path_new_from_indices(2, -1);
    gtk_tree_selection_select_path(selection, path); gtk_tree_path_free(path);
    path = gtk_tree_path_new_from_indices(0, -1);
    gtk_tree_selection_select_path(selection, path); gtk_tree_path_free(path);
    g_assert_cmpint(gtk_tree_selection_count_selected_rows(selection), ==, 2);
    RadioSBroadcastOnline *online = g_object_get_data(G_OBJECT(h->page), "radios-broadcast-online");
    JsonArray *items = radios_broadcast_online_items(online, FALSE);
    g_assert_nonnull(items); g_assert_cmpuint(json_array_get_length(items), ==, 2);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(items, 0), "provider_id", ""), ==, "Acct0000000");
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(items, 1), "provider_id", ""), ==, "Acct0000002");
    json_array_unref(items);
    shoot(h, "scope9-online-multiple.png");
    g_assert_false(gtk_widget_is_sensitive(find(h->page, "broadcast-catalog-replace")));
    guint starts = player_starts; click(h->page, "broadcast-online-play");
    wait_for(player_started, GUINT_TO_POINTER(starts + 1), "multi playback starts at first visible selected row");
    g_assert_cmpstr(last_video, ==, "Acct0000000");
    wait_for(idle, h, "selected-only playback operation acknowledged");
    click(h->page, "broadcast-online-next");
    wait_for(player_started, GUINT_TO_POINTER(starts + 2), "next skips the unselected middle row");
    g_assert_cmpstr(last_video, ==, "Acct0000002");
    wait_for(idle, h, "selected-only next acknowledged"); click(h->page, "broadcast-online-stop");
    wait_for(button_sensitive, find(h->page, "broadcast-catalog-insert"), "multi online insert enabled");
    click(h->page, "broadcast-catalog-insert");
    Shape wanted = {blocks, count + 2}; wait_for(programme_shape, &wanted, "only selected online rows persisted in visible order");
    JsonNode *after = programme_config(); JsonArray *events = json_object_get_array_member(json_node_get_object(after), "events");
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(events, count), "online_id", ""), ==, "Acct0000000");
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(events, count + 1), "online_id", ""), ==, "Acct0000002");
    gtk_tree_selection_unselect_all(selection);
    g_assert_null(radios_broadcast_online_items(online, FALSE));
    json_node_free(before); json_node_free(after); close_harness(h);
}
static JsonNode *selected_online_playlist(const gchar *name)
{
    JsonNode *payload = parse("{}");
    json_object_set_string_member(json_node_get_object(payload), "station_id", radios_data_client_station_id(reader));
    GError *error = NULL;
    JsonNode *result = radios_data_client_request_sync(reader, "POST", "/v1/workflows/desktop-playlists/list", payload, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(result); json_node_free(payload);
    JsonArray *rows = json_object_get_array_member(json_node_get_object(result), "playlists"); JsonNode *found = NULL;
    for (guint i = 0; i < json_array_get_length(rows); ++i)
        if (!g_strcmp0(radios_json_string(json_array_get_object_element(rows, i), "name", ""), name))
            found = json_node_copy(json_array_get_element(rows, i));
    json_node_free(result); return found;
}
static gboolean online_playlist_closed(gpointer unused)
{ (void)unused; return dialog_find("broadcast-selection-playlist-dialog") == NULL; }
static gint catalogue_title_row(Harness *h, const gchar *title)
{
    GtkTreeModel *model = gtk_tree_view_get_model(catalog_tree(h)); GtkTreeIter iter; gint index = 0;
    for (gboolean more = gtk_tree_model_get_iter_first(model, &iter); more; more = gtk_tree_model_iter_next(model, &iter), ++index) {
        gchar *found = NULL; gtk_tree_model_get(model, &iter, 1, &found, -1);
        gboolean match = !g_strcmp0(found, title); g_free(found); if (match) return index;
    }
    return -1;
}
typedef struct { Harness *h; const gchar *first, *second; } OnlineTitles;
static gboolean online_titles_ready(gpointer value)
{
    OnlineTitles *wanted = value;
    return catalogue_title_row(wanted->h, wanted->first) >= 0 && catalogue_title_row(wanted->h, wanted->second) >= 0;
}
static void create_online_selection(Harness *h, const gchar *name, const gchar *first, const gchar *second,
    const gchar *first_provider, const gchar *second_provider, const gchar *first_id, const gchar *second_id)
{
    GtkTreeSelection *selection = gtk_tree_view_get_selection(catalog_tree(h));
    g_assert_cmpint(gtk_tree_selection_get_mode(selection), ==, GTK_SELECTION_MULTIPLE);
    gtk_tree_selection_unselect_all(selection);
    select_row(h, (guint)catalogue_title_row(h, second)); select_row(h, (guint)catalogue_title_row(h, first));
    g_assert_cmpint(gtk_tree_selection_count_selected_rows(selection), ==, 2);
    JsonNode *before = programme_config(); guint starts = player_starts;
    click(h->page, "broadcast-catalog-create-playlist");
    GtkWidget *create = dialog_find("broadcast-selection-playlist-dialog"); g_assert_nonnull(create);
    gtk_entry_set_text(GTK_ENTRY(find(create, "broadcast-selection-playlist-name")), name);
    gtk_dialog_response(GTK_DIALOG(create), GTK_RESPONSE_ACCEPT);
    wait_for(online_playlist_closed, NULL, "selected provider playlist persisted through actual broadcast adapter");
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(find(h->page, "broadcast-catalog-content"))), ==, "playlists");
    JsonNode *playlist = selected_online_playlist(name); g_assert_nonnull(playlist);
    JsonObject *record = json_node_get_object(playlist);
    JsonArray *tracks = json_object_get_array_member(record, "tracks"); g_assert_cmpuint(json_array_get_length(tracks), ==, 2);
    const gchar *titles[] = {first, second}, *providers[] = {first_provider, second_provider}, *ids[] = {first_id, second_id};
    for (guint i = 0; i < 2; ++i) {
        JsonObject *track = json_array_get_object_element(tracks, i);
        g_assert_cmpstr(radios_json_string(track, "title", ""), ==, titles[i]);
        g_assert_cmpstr(radios_json_string(track, "provider", ""), ==, providers[i]);
        g_assert_cmpstr(radios_json_string(track, "external_id", ""), ==, ids[i]);
        g_assert_cmpstr(radios_json_string(track, "location", ""), ==, radios_json_string(track, "url", ""));
        g_assert_true(g_str_has_prefix(radios_json_string(track, "url", ""), "https://"));
        g_assert_cmpstr(radios_json_string(track, "media_state", ""), ==, "unverified");
        g_assert_cmpint(radios_json_int(track, "ordinal", -1), ==, i);
        g_assert_false(json_object_has_member(track, "media_id"));
    }
    JsonNode *after = programme_config();
    if (before) g_assert_true(json_node_equal(before, after)); else g_assert_null(after);
    g_assert_cmpuint(player_starts, ==, starts);
    g_clear_pointer(&before, json_node_free); g_clear_pointer(&after, json_node_free); json_node_free(playlist);
}
static void test_selected_account_and_multiple_provider_playlists(void)
{
    Harness *h = open_harness("RADIOS_TEST_ONLINE_OPERATE_TOKEN"); select_source(h, 1);
    Choice account = {h, "acct:PLonlineBroadcast1"}; wait_for(choice_ready, &account, "account playlist available for selection-create");
    Rows wanted = {h, 3, "Acct"}; wait_for(rows_ready, &wanted, "account tracks available for selection-create");
    create_online_selection(h, "Selected account references", "Acct title 0", "Acct title 2", "youtube", "youtube", "Acct0000000", "Acct0000002");
    close_harness(h);
    gchar *saved = seed_playlist("apple_music", "PDF10 selected Apple list", g_strdup(
        "[{\"provider_id\":\"1564530960\",\"url\":\"https://music.apple.com/tr/song/1564530960\","
        "\"title\":\"online query PDF10 Apple song\",\"artist\":\"Apple artist\",\"duration_ms\":180000}]"));
    h = open_harness("RADIOS_TEST_ONLINE_OPERATE_TOKEN");
    GtkWidget *query = find(h->page, "broadcast-catalog-query"); gtk_entry_set_text(GTK_ENTRY(query), "online query");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(h->page, "broadcast-source-apple")), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(h->page, "broadcast-source-youtube")), TRUE);
    g_signal_emit_by_name(query, "activate");
    OnlineTitles pair = {h, "Find title 0", "online query PDF10 Apple song"};
    wait_for(online_titles_ready, &pair, "live YouTube and saved Apple rows coexist in unified search");
    g_assert_cmpint(catalogue_title_row(h, pair.first), <, catalogue_title_row(h, pair.second));
    create_online_selection(h, "Selected multiple providers", pair.first, pair.second, "youtube", "apple_music", "Find0000000", "1564530960");
    close_harness(h); g_free(saved);
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]);
    for (gint i = 1; i < argc - 1; ++i) argv[i] = argv[i + 1];
    --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv); GError *error = NULL;
    /* Spotify / Apple Music rows use their YouTube Music matches here; the
     * official Spotify app and Chrome window are never started by tests. */
    g_setenv("RADIOS_OFFICIAL_PLAYERS", "0", TRUE);
    g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    engine = radios_audio_engine_new(FALSE, NULL);
    g_test_add_func("/broadcast-online/youtube-source-lists-plays-account-saved-search", test_youtube_source_lists_and_plays_account_saved_and_search);
    g_test_add_func("/broadcast-online/viewer-cannot-play", test_viewer_cannot_play);
    g_test_add_func("/broadcast-online/youtube-items-become-flow-events", test_youtube_items_become_flow_events);
    g_test_add_func("/broadcast-online/flow-plays-youtube-events", test_flow_plays_youtube_events);
    g_test_add_func("/broadcast-online/card-loads-and-plays-provider-list", test_card_loads_and_plays_provider_list);
    g_test_add_func("/broadcast-online/spotify-list-plays-through-youtube-matches", test_spotify_list_plays_through_youtube_matches);
    g_test_add_func("/broadcast-online/apple-list-prepares-before-play", test_apple_list_prepares_before_play);
    g_test_add_func("/broadcast-online/multiple-sources-music-playlist-scope", test_multiple_sources_and_music_playlist_scope);
    g_test_add_func("/broadcast-online/source-menu-search-hints-scope", test_source_menu_and_search_prompts);
    g_test_add_func("/broadcast-online/multiple-selected-rows-insert-play-visible-order", test_multiple_selected_rows_insert_and_play_order);
    g_test_add_func("/broadcast-online/selected-account-multiple-provider-playlists", test_selected_account_and_multiple_provider_playlists);
    gint result = g_test_run(); radios_audio_engine_unref(engine); radios_data_client_unref(reader); radios_config_clear(&config);
    g_free(last_video); g_free(prepared_video); return result;
}
