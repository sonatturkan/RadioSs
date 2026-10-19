#define _POSIX_C_SOURCE 200809L
#include <glib.h>

/* Genuine private SDBASE/PCM/window fixture. Production ownership, prepared
 * media, schema selection, header projection and transport are not wrapped. */
static int autoplay_header_run(void);
#define main radios_autoplay_header_fixture_main
#define g_test_run autoplay_header_run
#include "test_playout_ui.c"
#undef g_test_run
#undef main

#include "radios/broadcast_header.h"
#include "radios/header_status.h"
#include "radios/header_navigation.h"
#include "radios/platform.h"

static GtkWidget *autoplay_header(Window *window)
{
    GtkWidget *header = g_object_get_data(G_OBJECT(window->ctx->parent), "radios-header-status-root");
    g_assert_true(GTK_IS_WIDGET(header)); return header;
}
static const gchar *autoplay_value(Window *window, const gchar *field)
{
    gchar *name = g_strdup_printf("header-status-%s-value", field);
    GtkWidget *label = find(window->root, name); g_free(name);
    g_assert_true(GTK_IS_LABEL(label)); return gtk_label_get_text(GTK_LABEL(label));
}
static gboolean autoplay_clock(Window *window, const gchar *field, const gchar *caption, gint64 end)
{
    const gchar *shown = autoplay_value(window, field);
    gchar *prefix = g_strdup_printf("%s · ", caption); gboolean matches = FALSE;
    if (!end) {
        gchar *unknown = g_strconcat(prefix, "—", NULL); matches = !g_strcmp0(shown, unknown); g_free(unknown);
    } else {
        guint hours = 0, minutes = 0, seconds = 0;
        if (g_str_has_prefix(shown, prefix) && sscanf(shown + strlen(prefix), "%u:%u:%u", &hours, &minutes, &seconds) == 3) {
            GTimeZone *zone = radios_time_zone_new(radios_data_client_station_timezone(reader)); g_assert_nonnull(zone);
            GDateTime *utc = g_date_time_new_from_unix_utc(end / G_TIME_SPAN_SECOND), *local = g_date_time_to_timezone(utc, zone);
            gint expected = g_date_time_get_hour(local) * 3600 + g_date_time_get_minute(local) * 60 + g_date_time_get_second(local);
            gint difference = abs((gint)(hours * 3600 + minutes * 60 + seconds) - expected);
            matches = MIN(difference, 86400 - difference) <= 1;
            g_date_time_unref(local); g_date_time_unref(utc); g_time_zone_unref(zone);
        }
    }
    g_free(prefix); return matches;
}
typedef struct {
    Window *window; const gchar *playlist; gboolean current, paused, advert;
    RadioSBroadcastHeaderSnapshot seen;
} AutoplayHeaderExpected;
static gboolean autoplay_header_matches(gpointer value)
{
    AutoplayHeaderExpected *expected = value; Window *window = expected->window;
    radios_header_status_refresh(autoplay_header(window));
    g_assert_true(radios_broadcast_header_snapshot(window->ctx, &expected->seen));
    RadioSBroadcastHeaderSnapshot *actual = &expected->seen;
    if (actual->current != expected->current || actual->paused != expected->paused ||
        actual->programme_active != expected->current || actual->advert_timing_known != expected->advert ||
        g_strcmp0(actual->playlist_title, expected->playlist)) return FALSE;
    GtkWidget *advert_label = find(window->root, "header-next-advert-time"); g_assert_true(GTK_IS_LABEL(advert_label));
    const gchar *advert = gtk_label_get_text(GTK_LABEL(advert_label));
    if (!expected->advert) {
        if (g_strcmp0(advert, "Reklama kalan  —:—")) return FALSE;
    } else {
        guint64 minutes = 0, seconds = 0;
        if (sscanf(advert, "Reklama kalan  %" G_GUINT64_FORMAT ":%" G_GUINT64_FORMAT, &minutes, &seconds) != 2 || seconds >= 60 ||
            (strstr(advert, "Duraklatıldı") != NULL) != expected->paused) return FALSE;
        gint64 shown = (gint64)(minutes * 60 + seconds), wanted = (actual->advert_remaining_us + G_TIME_SPAN_SECOND - 1) / G_TIME_SPAN_SECOND;
        if (llabs(shown - wanted) > 1) return FALSE;
    }
    gchar *playlist = g_strdup_printf("Playlist · %s", *expected->playlist ? expected->playlist : "—");
    gboolean matches = !g_strcmp0(autoplay_value(window, "playlist"), playlist); g_free(playlist);
    if (!matches || !autoplay_clock(window, "programme-end", "Program bitişi", actual->programme_end_utc_us) ||
        !autoplay_clock(window, "playlist-end", "Playlist bitişi", actual->playlist_end_utc_us)) return FALSE;
    if (!expected->current)
        return !actual->programme_end_utc_us && !actual->playlist_end_utc_us && !actual->advert_remaining_us &&
            !g_strcmp0(autoplay_value(window, "remaining"), "—") && !g_strcmp0(autoplay_value(window, "programme"), "—");
    if (g_strcmp0(actual->programme_title, expected->playlist) || actual->track_remaining_us <= 0 ||
        actual->playlist_remaining_us < actual->track_remaining_us ||
        actual->programme_remaining_us < actual->track_remaining_us) return FALSE;
    if (expected->paused)
        return !actual->programme_end_utc_us && !actual->playlist_end_utc_us &&
            !g_strcmp0(autoplay_value(window, "track-end"), "Parça bitişi · Duraklatıldı");
    return actual->programme_end_utc_us > g_get_real_time() && actual->playlist_end_utc_us > g_get_real_time();
}
static RadioSBroadcastHeaderSnapshot autoplay_header_wait(Window *window, const gchar *playlist,
    gboolean current, gboolean paused, gboolean advert)
{
    AutoplayHeaderExpected expected = {window, playlist, current, paused, advert, {0}};
    gint64 deadline = g_get_monotonic_time() + 12 * G_TIME_SPAN_SECOND;
    while (!autoplay_header_matches(&expected) && g_get_monotonic_time() < deadline) pump();
    if (!autoplay_header_matches(&expected)) g_error("Auto playlist header expected '%s' current%d paused%d advert%d; actual '%s' / '%s' current%d paused%d advert%d ends%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT " remaining%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT,
        playlist, current, paused, advert, expected.seen.playlist_title, expected.seen.programme_title,
        expected.seen.current, expected.seen.paused, expected.seen.advert_timing_known,
        expected.seen.programme_end_utc_us, expected.seen.playlist_end_utc_us,
        expected.seen.programme_remaining_us, expected.seen.playlist_remaining_us, expected.seen.track_remaining_us);
    return expected.seen;
}
static RadioSAudioRunSnapshot autoplay_run_wait(RadioSAudioEngine *engine, guint source, RadioSAudioRunState state, guint index, guint64 position)
{
    RadioSAudioRunSnapshot actual = {0}; gint64 deadline = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
    g_test_message("Header scope native source%u expects state%d index%u position%" G_GUINT64_FORMAT, source, state, index, position);
    do {
        pump(); radios_audio_engine_player_runner_snapshot(engine, source, &actual);
        if (actual.state == state && (state == RADIOS_AUDIO_RUN_IDLE || actual.index == index) && actual.actual_current_frame >= position) return actual;
    } while (g_get_monotonic_time() < deadline);
    g_error("Header native source%u expected state%d index%u position%" G_GUINT64_FORMAT "; actual state%d index%u position%" G_GUINT64_FORMAT " error%s",
        source, state, index, position, actual.state, actual.index, actual.actual_current_frame, actual.error);
    return actual;
}
static void autoplay_playlist_scopes_name_end_advert_pause_stop(void)
{
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "scoped header workspace ready");
    seed_programme(&window, NULL, NULL);
    JsonNode *music = media_import_rhythm(&window, "Otomatik başlık gerçek müzik", 820, .24, "music", 120);
    JsonNode *advert = media_import_rhythm(&window, "Otomatik başlık gerçek reklam", 1910, .24, "advertisement", 120);
    JsonNode *playlist = playlist_create("Başlıkta görünen gerçek playlist", music, advert);
    const gchar *playlist_name = radios_json_string(json_node_get_object(playlist), "name", ""); g_assert_true(*playlist_name != '\0');
    RadioSPlayerTopology topology = *radios_player_workspace_topology(window.ctx->players); gchar cluster[37]; GError *error = NULL;
    require_call(radios_player_topology_add_cluster(&topology, "Başlık test kümesi", cluster, &error), &error);
    require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(window.ctx);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    for (guint kind = 0; kind < 3; ++kind) {
        topology = *radios_player_workspace_topology(window.ctx->players);
        require_call(radios_player_topology_set_cluster(&topology, 0, kind == 1 ? cluster : "", &error), &error);
        require_call(radios_player_topology_set_cluster(&topology, 2, kind == 1 ? cluster : "", &error), &error);
        require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
        RadioSAutomationScope scope = {.kind = kind == 0 ? RADIOS_AUTOMATION_SCOPE_PLAYER :
            kind == 1 ? RADIOS_AUTOMATION_SCOPE_CLUSTER : RADIOS_AUTOMATION_SCOPE_UNCLUSTERED, .deck = 0};
        if (kind == 1) g_strlcpy(scope.cluster_id, cluster, sizeof scope.cluster_id);
        g_test_message("Auto header scope kind%u: load/play/paused/resume/advert/stop", kind);
        ScopeExecutionResult loaded = {0};
        require_call(radios_player_scope_execution_load_playlist(execution, &scope,
            radios_json_string(json_node_get_object(playlist), "_id", ""), execution_result, &loaded, execution_result_destroy, &error), &error);
        execution_wait(&loaded); require_call(radios_player_workspace_programme_saved(window.ctx->players, &scope, &error), &error);
        JsonObject *programme = NULL; require_call(radios_player_workspace_get_programme(window.ctx->players, &scope, &programme, &error), &error);
        JsonArray *blocks = json_object_get_array_member(programme, "blocks");
        g_assert_cmpuint(json_array_get_length(blocks), ==, 2);
        JsonArray *events = json_object_get_array_member(programme, "events");
        g_assert_cmpuint(json_array_get_length(events), ==, 2);
        const gchar *block_kinds[] = {"music", "advertisement"};
        for (guint i = 0; i < G_N_ELEMENTS(block_kinds); ++i) {
            JsonObject *block = json_array_get_object_element(blocks, i);
            g_assert_cmpstr(radios_json_string(block, "title", ""), ==, playlist_name);
            g_assert_cmpstr(radios_json_string(block, "kind", ""), ==, block_kinds[i]);
            g_assert_cmpstr(radios_json_string(json_array_get_object_element(events, i), "block_id", ""), ==, radios_json_string(block, "id", ""));
        }
        g_assert_cmpstr(radios_json_string(json_array_get_object_element(blocks, 0), "id", ""), !=,
            radios_json_string(json_array_get_object_element(blocks, 1), "id", ""));
        json_object_unref(programme);
        ScopeExecutionResult played = {0};
        require_call(radios_player_scope_execution_play_programme(execution, &scope, NULL, RADIOS_AUTODJ_SEMI,
            execution_result, &played, execution_result_destroy, &error), &error);
        execution_wait(&played); autoplay_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
        RadioSBroadcastHeaderSnapshot initial = autoplay_header_wait(&window, playlist_name, TRUE, FALSE, TRUE);
        /* Typed Events do not change the identity/end of the original
         * loaded playlist: both music and advert belong to that playlist. */
        g_assert_cmpint(initial.playlist_remaining_us, ==, initial.programme_remaining_us);
        g_assert_cmpint(initial.advert_remaining_us, >, 0); g_assert_cmpint(initial.advert_remaining_us, <=, initial.track_remaining_us);
        GtkWidget *tree = find(window.root, "live-player-definitions"); radios_player_tree_ui_refresh(tree);
        RadioSAutomationScope hidden = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 4};
        g_assert_true(radios_player_tree_ui_select(tree, &hidden));
        run_for(300); RadioSBroadcastHeaderSnapshot advanced = autoplay_header_wait(&window, playlist_name, TRUE, FALSE, TRUE);
        g_assert_cmpint(advanced.advert_remaining_us, <, initial.advert_remaining_us);
        require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_PAUSE, &error), &error);
        autoplay_run_wait(engine, 0, RADIOS_AUDIO_RUN_PAUSED, 0, 0);
        RadioSBroadcastHeaderSnapshot paused = autoplay_header_wait(&window, playlist_name, TRUE, TRUE, TRUE);
        run_for(250); RadioSBroadcastHeaderSnapshot still = autoplay_header_wait(&window, playlist_name, TRUE, TRUE, TRUE);
        g_assert_cmpint(still.advert_remaining_us, ==, paused.advert_remaining_us);
        wait_until(execution_main_ready, window.ctx, "actual MAIN pause flush complete");
        require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_RESUME, &error), &error);
        autoplay_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
        autoplay_header_wait(&window, playlist_name, TRUE, FALSE, TRUE);
        require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_NEXT, &error), &error);
        autoplay_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 1, 4800);
        RadioSBroadcastHeaderSnapshot on_advert = autoplay_header_wait(&window, playlist_name, TRUE, FALSE, TRUE);
        g_assert_cmpint(on_advert.advert_remaining_us, ==, 0);
        goffset main_at = capture_offset("MAIN"); run_for(150); g_assert_cmpfloat(tone_energy("MAIN", main_at, 1910), >, .04);
        require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_STOP, &error), &error);
        autoplay_run_wait(engine, 0, RADIOS_AUDIO_RUN_IDLE, 0, 0); autoplay_header_wait(&window, "", FALSE, FALSE, FALSE);
    }
    assert_saved_playlist_unchanged(playlist); radios_audio_engine_unref(engine);
    json_node_free(playlist); json_node_free(music); json_node_free(advert); window_close(&window);
    g_test_message("Real source0, cluster and Kümesizler playlist identity/end/ad countdown follow hidden scoped PCM; pause freezes and Stop clears.");
}

typedef struct { void (*forward)(gpointer, const gchar *); gpointer forward_data; gchar *programme; } AutoplayNotices;
static void autoplay_notice(gpointer value, const gchar *message)
{
    AutoplayNotices *notices = value;
    g_test_message("Observed header notification: %s", message);
    if (g_str_has_prefix(message, "Program akışı")) { g_free(notices->programme); notices->programme = g_strdup(message); }
    if (notices->forward) notices->forward(notices->forward_data, message);
}
typedef struct { AutoplayNotices *notices; const gchar *text; } AutoplayNoticeExpected;
static gboolean autoplay_notice_matches(gpointer value)
{
    AutoplayNoticeExpected *expected = value; return expected->notices->programme && strstr(expected->notices->programme, expected->text);
}
static void autoplay_notice_wait(AutoplayNotices *notices, const gchar *text)
{
    AutoplayNoticeExpected expected = {notices, text};
    gint64 deadline = g_get_monotonic_time() + 12 * G_TIME_SPAN_SECOND;
    while (!autoplay_notice_matches(&expected) && g_get_monotonic_time() < deadline) pump();
    if (!autoplay_notice_matches(&expected)) g_error("Header notification expected '%s', last programme message '%s'", text, notices->programme ? notices->programme : "<none>");
    g_assert_null(strstr(notices->programme, "başlatılıyor")); g_assert_null(strstr(notices->programme, "hazırlanıyor"));
}
typedef struct { GtkWidget *combo; const gchar *id; } AutoplaySchemaChoice;
static gboolean autoplay_schema_choice(gpointer value)
{
    AutoplaySchemaChoice *choice = value;
    return gtk_combo_box_set_active_id(GTK_COMBO_BOX(choice->combo), choice->id);
}
static void autoplay_header_schema_status_current_future_stop(void)
{
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "header schema workspace ready");
    seed_programme(&window, NULL, NULL);
    JsonNode *tone = media_import_rhythm(&window, "Başlık program akışı gerçek sesi", 1760, .24, "music", 120);
    JsonNode *playlist = playlist_create("Program akışının gerçek playlisti", tone, NULL);
    gint64 minute = G_TIME_SPAN_MINUTE, day = g_get_real_time() / G_TIME_SPAN_DAY * G_TIME_SPAN_DAY;
    gint64 start = MAX(day, g_get_real_time() / minute * minute - minute);
    guint length = (guint)MIN((gint64)3, (day + G_TIME_SPAN_DAY - start) / minute);
    JsonNode *current = weekly_pcm_configuration(playlist, NULL, start, length, "Yayındaki gerçek event");
    JsonNode *record = document_save("weekly_programme", "Başlık şimdiki program", json_node_get_object(current));
    RadioSPlayerTopology topology = *radios_player_workspace_topology(window.ctx->players); gchar cluster[37]; GError *error = NULL;
    require_call(radios_player_topology_add_cluster(&topology, "Program akışı başlık kümesi", cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 0, cluster, &error), &error);
    require_call(radios_player_topology_set_cluster_automation(&topology, cluster, radios_json_string(json_node_get_object(record), "_id", ""), &error), &error);
    require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER}; g_strlcpy(scope.cluster_id, cluster, sizeof scope.cluster_id);
    AutoplayNotices notices = {window.ctx->notify, window.ctx->notify_data, NULL}; window.ctx->notify = autoplay_notice; window.ctx->notify_data = &notices;
    g_assert_true(radios_broadcast_navigation_available(window.ctx, 1));
    g_assert_false(radios_broadcast_navigation_queue_playable(window.ctx));
    click(window.root, "header-play"); autoplay_notice_wait(&notices, "yayında: Yayındaki gerçek event");
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    autoplay_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    RadioSBroadcastHeaderSnapshot seen = {0}; g_assert_true(radios_broadcast_header_snapshot(window.ctx, &seen));
    g_assert_true(seen.programme_active); g_assert_cmpstr(seen.programme_title, ==, "Yayındaki gerçek event");
    g_assert_cmpint(seen.programme_end_utc_us, ==, start + length * minute);
    g_assert_cmpstr(seen.playlist_title, ==, "Program akışının gerçek playlisti");
    g_assert_cmpint(seen.playlist_end_utc_us, >, g_get_real_time());
    click(window.root, "header-pause"); autoplay_notice_wait(&notices, "duraklatıldı: Yayındaki gerçek event");
    click(window.root, "header-stop"); autoplay_notice_wait(&notices, "durduruldu");
    gint64 tomorrow = day + G_TIME_SPAN_DAY + 5 * minute;
    JsonNode *future = weekly_pcm_configuration(playlist, NULL, tomorrow, 1, "Gelecekteki gerçek event");
    JsonNode *future_record = document_save("weekly_programme", "Başlık gelecekteki program", json_node_get_object(future));
    require_call(radios_player_workspace_assign(window.ctx->players, &scope,
        radios_json_string(json_node_get_object(future_record), "_id", ""), &error), &error);
    /* Explicit selection avoids waiting for the background catalogue refresh.
     * The assigned record still drives the genuine header Play path. */
    GtkWidget *selector = find(window.root, "header-programme");
    g_assert_true(GTK_IS_COMBO_BOX(selector)); gtk_combo_box_set_active(GTK_COMBO_BOX(selector), 0);
    click(window.root, "header-play"); autoplay_notice_wait(&notices, "hazır: Gelecekteki gerçek event · Başlangıç");
    g_assert_true(radios_broadcast_header_snapshot(window.ctx, &seen)); g_assert_false(seen.current); g_assert_true(seen.waiting);
    g_assert_cmpstr(seen.programme_title, ==, "Gelecekteki gerçek event"); g_assert_cmpint(seen.programme_end_utc_us, ==, tomorrow + minute);
    radios_header_status_refresh(autoplay_header(&window));
    g_assert_cmpstr(autoplay_value(&window, "programme"), ==, "Gelecekteki gerçek event");
    g_assert_true(autoplay_clock(&window, "programme-end", "Program bitişi", tomorrow + minute));
    click(window.root, "header-stop"); autoplay_notice_wait(&notices, "durduruldu");
    autoplay_header_wait(&window, "", FALSE, FALSE, FALSE);
    JsonNode *past = weekly_pcm_configuration(playlist, NULL, day - 14 * G_TIME_SPAN_DAY + 5 * minute, 1, "Bitmiş eski event");
    JsonNode *past_record = document_save("weekly_programme", "Başlık bitmiş program", json_node_get_object(past));
    require_call(radios_player_workspace_assign(window.ctx->players, &scope,
        radios_json_string(json_node_get_object(past_record), "_id", ""), &error), &error);
    gtk_combo_box_set_active(GTK_COMBO_BOX(selector), 0); g_clear_pointer(&notices.programme, g_free);
    click(window.root, "header-play"); autoplay_notice_wait(&notices, "durduruldu");
    autoplay_header_wait(&window, "", FALSE, FALSE, FALSE);
    /* An explicitly selected finite queue must still play its own content
     * even though an idle weekly schema is assigned to a player cluster. */
    seed_programme(&window, tone, NULL); gtk_combo_box_set_active(GTK_COMBO_BOX(selector), 0);
    g_assert_true(radios_broadcast_navigation_queue_playable(window.ctx));
    click(window.root, "header-play"); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 100);
    goffset manual_at = capture_offset("MAIN"); run_for(150); g_assert_cmpfloat(tone_energy("MAIN", manual_at, 1760), >, .04);
    RadioSAudioRunSnapshot manual_before = {0}; g_assert_true(radios_playout_ui_navigation_snapshot(window.ctx, &manual_before));
    g_assert_true(radios_broadcast_navigation_queue_active(window.ctx));
    click(window.root, "header-pause"); run_wait(&window, RADIOS_PLAYOUT_PAUSED, 0, 100);
    AutoplaySchemaChoice choice = {selector, radios_json_string(json_node_get_object(record), "_id", "")};
    wait_until(autoplay_schema_choice, &choice, "authenticated header catalogue has a nonempty saved schema selected while actual manual PCM is paused");
    click(window.root, "header-play"); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 100);
    RadioSAudioRunSnapshot manual_resumed = {0}; g_assert_true(radios_playout_ui_navigation_snapshot(window.ctx, &manual_resumed));
    g_assert_cmpuint(manual_resumed.epoch, ==, manual_before.epoch);
    manual_at = capture_offset("MAIN"); run_for(150); g_assert_cmpfloat(tone_energy("MAIN", manual_at, 1760), >, .04);
    click(window.root, "header-stop"); run_for(150); autoplay_header_wait(&window, "", FALSE, FALSE, FALSE);
    window.ctx->notify = notices.forward; window.ctx->notify_data = notices.forward_data; g_free(notices.programme);
    weekly_record_unchanged(record); weekly_record_unchanged(future_record); weekly_record_unchanged(past_record); assert_saved_playlist_unchanged(playlist);
    radios_audio_engine_unref(engine); json_node_free(record); json_node_free(current); json_node_free(future_record); json_node_free(future);
    json_node_free(past_record); json_node_free(past);
    json_node_free(playlist); json_node_free(tone); window_close(&window);
    g_test_message("Header Play tracks actual current/future schema state and clears preparing after Play, Pause and Stop; future programme end is visible.");
}
static int autoplay_header_run(void)
{
    g_test_add_func("/native-autoplay-header/scoped-playlist-name-end-advert-pause-stop-PCM", autoplay_playlist_scopes_name_end_advert_pause_stop);
    g_test_add_func("/native-autoplay-header/schema-current-future-status-clears-preparing-PCM", autoplay_header_schema_status_current_future_stop);
    return g_test_run();
}
int main(int argc, char **argv) { return radios_autoplay_header_fixture_main(argc, argv); }
