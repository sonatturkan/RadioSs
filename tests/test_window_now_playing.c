#define _POSIX_C_SOURCE 200809L
/* Exercise the actual accepted plan, PCM ownership and OS title property;
 * selecting another page/card must never substitute draft metadata. */
#include <glib.h>
static int window_now_playing_run(void);
#define main radios_window_now_playing_fixture_main
#define g_test_run window_now_playing_run
#include "test_playout_ui.c"
#undef g_test_run
#undef main
#include "radios/broadcast_header.h"

typedef struct { Window *window; const gchar *title; } WindowTitleExpected;
static gboolean window_title_matches(gpointer value)
{
    WindowTitleExpected *expected = value;
    return !g_strcmp0(gtk_window_get_title(GTK_WINDOW(expected->window->root)), expected->title);
}
static void window_title_wait(Window *window, const gchar *title)
{
    WindowTitleExpected expected = {window, title};
    gint64 deadline = g_get_monotonic_time() + 4 * G_USEC_PER_SEC;
    while (!window_title_matches(&expected) && g_get_monotonic_time() < deadline) pump();
    if (!window_title_matches(&expected)) g_error("Window title expected '%s', actual '%s'", title,
        gtk_window_get_title(GTK_WINDOW(window->root)));
}
static void window_title_assert(Window *window, const gchar *suffix)
{
    gchar *expected = g_strconcat(config.name, suffix, NULL);
    window_title_wait(window, expected); g_free(expected);
}
static JsonNode *window_title_programme(JsonNode *first, JsonNode *second, JsonNode *third, const gchar *title)
{
    JsonNode *programme = programme_config(first, second);
    json_object_set_string_member(json_node_get_object(programme), "title", title);
    if (third) {
        JsonNode *extra = programme_config(third, NULL);
        JsonArray *events = json_object_get_array_member(json_node_get_object(programme), "events");
        JsonArray *append = json_object_get_array_member(json_node_get_object(extra), "events");
        json_array_add_element(events, json_node_copy(json_array_get_element(append, 0))); json_node_free(extra);
    }
    return programme;
}
static void test_window_now_playing_current_hidden_pause_stop_future(void)
{
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "window title workspace");
    seed_programme(&window, NULL, NULL); window_title_assert(&window, "");
    JsonNode *music = media_metadata_save(media_import_rhythm(&window, "Gece Şarkısı", 810, .24, "music", 120), "Şarkıcı Özgür", "Albüm");
    JsonNode *advert = media_import_rhythm(&window, "Reklam kuşağı", 1650, .24, "advertisement", 120);
    JsonNode *jingle = media_import_rhythm(&window, "İstasyon jingle", 2300, .24, "jingle", 120);
    JsonNode *programme = window_title_programme(music, advert, jingle, "Akşam Programı");
    GError *error = NULL; RadioSPlayerTopology topology = *radios_player_workspace_topology(window.ctx->players); gchar cluster[37];
    require_call(radios_player_topology_add_cluster(&topology, "Başlık yayın kümesi", cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 0, cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 2, cluster, &error), &error);
    require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER}; g_strlcpy(scope.cluster_id, cluster, sizeof scope.cluster_id);
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(window.ctx); ScopeExecutionResult played = {0};
    require_call(radios_player_scope_execution_play_programme(execution, &scope, json_node_get_object(programme), RADIOS_AUTODJ_SEMI,
        execution_result, &played, execution_result_destroy, &error), &error); execution_wait(&played);
    window_title_assert(&window, " · Akşam Programı · Şarkıcı Özgür – Gece Şarkısı");
    goffset music_at = capture_offset("MAIN"); run_for(160); g_assert_cmpfloat(tone_energy("MAIN", music_at, 810), >, .04);
    GtkWidget *tree = find(window.root, "live-player-definitions"); radios_player_tree_ui_refresh(tree);
    RadioSAutomationScope hidden = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 4};
    g_assert_true(radios_player_tree_ui_select(tree, &hidden)); g_assert_true(radios_window_select_profile(window.window, "technical"));
    window_title_assert(&window, " · Akşam Programı · Şarkıcı Özgür – Gece Şarkısı");
    require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_PAUSE, &error), &error);
    window_title_assert(&window, " · Akşam Programı · Şarkıcı Özgür – Gece Şarkısı · Duraklatıldı");
    wait_until(execution_main_ready, window.ctx, "paused title PCM flush");
    require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_RESUME, &error), &error);
    window_title_assert(&window, " · Akşam Programı · Şarkıcı Özgür – Gece Şarkısı");
    require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_NEXT, &error), &error);
    window_title_assert(&window, " · Akşam Programı · Reklam kuşağı");
    goffset advert_at = capture_offset("MAIN"); run_for(160); g_assert_cmpfloat(tone_energy("MAIN", advert_at, 1650), >, .04);
    require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_NEXT, &error), &error);
    window_title_assert(&window, " · Akşam Programı · İstasyon jingle");
    require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_STOP, &error), &error); window_title_assert(&window, "");
    JsonNode *playlist = playlist_create("Beklenen gerçek medya", music, NULL);
    gint64 tomorrow = (g_get_real_time() / G_TIME_SPAN_DAY + 1) * G_TIME_SPAN_DAY + 5 * G_TIME_SPAN_MINUTE;
    JsonNode *future = weekly_pcm_configuration(playlist, NULL, tomorrow, 1, "Yarınki Program");
    JsonNode *record = document_save("weekly_programme", "Pencere başlığının gelecek programı", json_node_get_object(future));
    require_call(radios_player_workspace_assign(window.ctx->players, &scope, radios_json_string(json_node_get_object(record), "_id", ""), &error), &error);
    ScopeExecutionResult armed = {0};
    require_call(radios_player_scope_execution_play_schema(execution, &scope, RADIOS_AUTODJ_SEMI,
        execution_result, &armed, execution_result_destroy, &error), &error); execution_wait(&armed);
    window_title_assert(&window, " · Yarınki Program · Bekliyor");
    require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_STOP, &error), &error); window_title_assert(&window, "");
    weekly_record_unchanged(record); assert_saved_playlist_unchanged(playlist);
    json_node_free(record); json_node_free(future); json_node_free(playlist); json_node_free(programme);
    json_node_free(music); json_node_free(advert); json_node_free(jingle); window_close(&window);
    g_test_message("Native OS title follows accepted hidden cluster programme, actual music/advert/jingle, Pause, Next, Stop and future wait.");
}
static gchar *window_title_repeat(const gchar *text, guint times)
{
    GString *value = g_string_new(NULL); for (guint i = 0; i < times; ++i) g_string_append(value, text);
    return g_string_free(value, FALSE);
}
static gboolean window_long_title_ready(gpointer value)
{
    Window *window = value; const gchar *title = gtk_window_get_title(GTK_WINDOW(window->root));
    return title && strstr(title, "İİİİ") && strstr(title, "ÇÇÇÇ") && strstr(title, "ğğğğ");
}
static void test_window_now_playing_long_utf8_bounded(void)
{
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "bounded title workspace");
    seed_programme(&window, NULL, NULL);
    gchar *track = window_title_repeat("ğ", 160), *artist = window_title_repeat("Ç", 120), *name = window_title_repeat("İ", 160);
    JsonNode *media = media_metadata_save(media_import_rhythm(&window, track, 880, .24, "music", 120), artist, "");
    JsonNode *programme = window_title_programme(media, NULL, NULL, name);
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0};
    ScopeExecutionResult started = {0}; GError *error = NULL; RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(window.ctx);
    require_call(radios_player_scope_execution_play_programme(execution, &scope, json_node_get_object(programme), RADIOS_AUTODJ_SEMI,
        execution_result, &started, execution_result_destroy, &error), &error); execution_wait(&started);
    wait_until(window_long_title_ready, &window, "bounded UTF-8 includes programme, artist and current song");
    const gchar *shown = gtk_window_get_title(GTK_WINDOW(window.root));
    g_assert_true(g_utf8_validate(shown, -1, NULL)); g_assert_cmpint(g_utf8_strlen(shown, -1), <=, 256);
    g_assert_cmpuint(strlen(shown), <, 1024); g_assert_nonnull(strstr(shown, "…"));
    g_assert_null(strchr(shown, '\n')); g_assert_null(strchr(shown, '\r')); g_assert_true(g_str_has_prefix(shown, config.name));
    require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_STOP, &error), &error); window_title_assert(&window, "");
    json_node_free(programme); json_node_free(media); g_free(track); g_free(artist); g_free(name); window_close(&window);
}
static int window_now_playing_run(void)
{
    g_test_add_func("/native-window-title/current-hidden-programme-song-pause-stop-future-PCM", test_window_now_playing_current_hidden_pause_stop_future);
    g_test_add_func("/native-window-title/long-UTF8-bounded", test_window_now_playing_long_utf8_bounded);
    return g_test_run();
}
int main(int argc, char **argv) { return radios_window_now_playing_fixture_main(argc, argv); }
