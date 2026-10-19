#define _POSIX_C_SOURCE 200809L
#include <glib.h>

/* Reuse the genuine isolated SDBASE/Pulse/native-window fixture without
 * modifying its shared source. Existing cases remain registered; the runner
 * selects only /prompt2-header. Audio and playout observations are not wrapped. */
static int prompt2_run(void);
#define main radios_prompt2_fixture_main
#define g_test_run prompt2_run
#include "test_playout_ui.c"
#undef g_test_run
#undef main

#include "radios/broadcast_header.h"
#include "radios/header_status.h"
#include "radios/audio_output.h"
#include "radios/platform.h"

typedef struct {
    Window *window;
    const gchar *programme, *track;
    gboolean current, paused;
    guint sources;
    guint64 duration, minimum_position;
    RadioSBroadcastHeaderSnapshot observed;
} Prompt2HeaderCondition;

static GtkWidget *
prompt2_header(Window *window)
{
    GtkWidget *header = g_object_get_data(G_OBJECT(window->ctx->parent), "radios-header-status-root");
    g_assert_true(GTK_IS_WIDGET(header));
    return header;
}

static const gchar *
prompt2_value(Window *window, const gchar *field)
{
    gchar *name = g_strdup_printf("header-status-%s-value", field);
    GtkWidget *label = find(window->root, name);
    g_free(name);
    g_assert_true(GTK_IS_LABEL(label));
    return gtk_label_get_text(GTK_LABEL(label));
}

static gboolean
prompt2_header_matches(gpointer data)
{
    Prompt2HeaderCondition *expected = data;
    Window *window = expected->window;
    radios_header_status_refresh(prompt2_header(window));
    g_assert_true(radios_broadcast_header_snapshot(window->ctx, &expected->observed));
    RadioSBroadcastHeaderSnapshot *actual = &expected->observed;
    gboolean programme_active = expected->programme != NULL;
    if (actual->current != expected->current || actual->paused != expected->paused ||
        actual->active_sources != expected->sources || actual->programme_active != programme_active ||
        g_strcmp0(actual->programme_title, expected->programme ? expected->programme : "") ||
        g_strcmp0(actual->title, expected->track ? expected->track : "") ||
        actual->duration_ms != expected->duration || actual->position_ms < expected->minimum_position ||
        g_strcmp0(prompt2_value(window, "programme"), expected->programme ? expected->programme : "—")) return FALSE;
    if (!expected->current)
        return !g_strcmp0(prompt2_value(window, "music"), "—") &&
            !g_strcmp0(prompt2_value(window, "remaining"), "—");
    gchar *music = g_strdup_printf("%s%s%s", expected->track,
        expected->paused ? " · Duraklatıldı" : "", expected->sources > 1 ? " · Çoklu MAIN" : "");
    gboolean title_matches = !g_strcmp0(prompt2_value(window, "music"), music);
    g_free(music);
    if (!title_matches) return FALSE;
    guint64 minutes = 0, seconds = 0;
    if (sscanf(prompt2_value(window, "remaining"), "%" G_GUINT64_FORMAT " dk %" G_GUINT64_FORMAT " sn", &minutes, &seconds) != 2 ||
        seconds >= 60 || minutes * 60 + seconds > (expected->duration + 999) / 1000) return FALSE;
    if (expected->paused) {
        guint64 remaining = actual->duration_ms > actual->position_ms ? actual->duration_ms - actual->position_ms : 0;
        guint64 rounded = actual->track_remaining_us > 0 ?
            (guint64)ceil(actual->track_remaining_us / (gdouble)G_TIME_SPAN_SECOND) :
            (guint64)ceil(remaining / (actual->tempo_ratio * 1000));
        return minutes * 60 + seconds == rounded;
    }
    return TRUE;
}

static RadioSBroadcastHeaderSnapshot
prompt2_header_wait(Window *window, const gchar *programme, const gchar *track,
    gboolean current, gboolean paused, guint sources, guint64 duration, guint64 minimum_position)
{
    Prompt2HeaderCondition expected = {window, programme, track, current, paused, sources, duration, minimum_position, {0}};
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!prompt2_header_matches(&expected) && g_get_monotonic_time() < deadline) pump();
    if (!prompt2_header_matches(&expected))
        g_error("Prompt2 header expected programme '%s', track '%s', current%d paused%d sources%u duration%" G_GUINT64_FORMAT
            "; actual '%s' / '%s', current%d paused%d sources%u position%" G_GUINT64_FORMAT " duration%" G_GUINT64_FORMAT
            "; widgets '%s' / '%s' / '%s'", programme ? programme : "", track ? track : "", current, paused, sources, duration,
            expected.observed.programme_title, expected.observed.title, expected.observed.current, expected.observed.paused,
            expected.observed.active_sources, expected.observed.position_ms, expected.observed.duration_ms,
            prompt2_value(window, "programme"), prompt2_value(window, "music"), prompt2_value(window, "remaining"));
    return expected.observed;
}


/* These checks exercise the real read-only snapshot and visible labels under
 * authenticated private DB/PCM playback. One-second tolerance only accounts
 * for the two observation calls straddling a display-clock boundary. */
static gboolean prompt2_end_clock_matches(Window *window, const gchar *field, const gchar *caption, gint64 end)
{
    const gchar *shown = prompt2_value(window, field);
    if (end <= 0) {
        gchar *unknown = g_strdup_printf("%s · —", caption); gboolean matches = !g_strcmp0(shown, unknown);
        g_free(unknown); return matches;
    }
    guint hours, minutes, seconds; gchar *prefix = g_strdup_printf("%s · ", caption);
    gboolean parsed = g_str_has_prefix(shown, prefix) && sscanf(shown + strlen(prefix), "%u:%u:%u", &hours, &minutes, &seconds) == 3;
    g_free(prefix); if (!parsed || hours >= 24 || minutes >= 60 || seconds >= 60) return FALSE;
    RadioSDataClient *client = radios_data_client_acquire(window->ctx); g_assert_nonnull(client);
    GTimeZone *zone = radios_time_zone_new(radios_data_client_station_timezone(client)); radios_data_client_unref(client);
    g_assert_nonnull(zone); GDateTime *utc = g_date_time_new_from_unix_utc(end / G_TIME_SPAN_SECOND);
    GDateTime *local = g_date_time_to_timezone(utc, zone);
    gint expected = g_date_time_get_hour(local) * 3600 + g_date_time_get_minute(local) * 60 + g_date_time_get_second(local);
    gint observed = (gint)(hours * 3600 + minutes * 60 + seconds);
    gint difference = abs(observed - expected); difference = MIN(difference, 86400 - difference);
    g_date_time_unref(local); g_date_time_unref(utc); g_time_zone_unref(zone); return difference <= 1;
}
typedef struct {
    Window *window;
    const gchar *playlist;
    gboolean programme_known, playlist_known, paused;
    RadioSBroadcastHeaderSnapshot observed;
} Prompt2EndCondition;
static gboolean prompt2_end_matches(gpointer data)
{
    Prompt2EndCondition *expected = data; Window *window = expected->window;
    g_assert_true(radios_broadcast_header_snapshot(window->ctx, &expected->observed));
    RadioSBroadcastHeaderSnapshot *actual = &expected->observed;
    if (expected->playlist && g_strcmp0(actual->playlist_title, expected->playlist)) return FALSE;
    if ((actual->programme_end_utc_us > 0) != expected->programme_known ||
        (actual->playlist_end_utc_us > 0) != expected->playlist_known || actual->paused != expected->paused) return FALSE;
    radios_header_status_refresh(prompt2_header(window));
    gchar *playlist = g_strdup_printf("Playlist · %s", actual->playlist_title[0] ? actual->playlist_title : "—");
    gboolean title_matches = !g_strcmp0(prompt2_value(window, "playlist"), playlist); g_free(playlist);
    if (!title_matches || !prompt2_end_clock_matches(window, "programme-end", "Program bitişi", actual->programme_end_utc_us) ||
        !prompt2_end_clock_matches(window, "playlist-end", "Playlist bitişi", actual->playlist_end_utc_us)) return FALSE;
    if (!actual->current || !actual->duration_ms || actual->waiting)
        return !g_strcmp0(prompt2_value(window, "track-end"), "Parça bitişi · —");
    if (expected->paused)
        return !g_strcmp0(prompt2_value(window, "track-end"), "Parça bitişi · Duraklatıldı");
    if (actual->track_remaining_us <= 0) return FALSE;
    return prompt2_end_clock_matches(window, "track-end", "Parça bitişi", g_get_real_time() + (gint64)actual->track_remaining_us);
}
static RadioSBroadcastHeaderSnapshot prompt2_end_wait(Window *window, const gchar *playlist,
    gboolean programme_known, gboolean playlist_known, gboolean paused)
{
    Prompt2EndCondition expected = {window, playlist, programme_known, playlist_known, paused, {0}};
    wait_until(prompt2_end_matches, &expected, "live accepted programme/playlist/track end labels match actual station-clock snapshot");
    return expected.observed;
}

static JsonNode *
prompt2_save_private(guint slot, const gchar *title, JsonNode *first, JsonNode *second, guint transition_mode)
{
    JsonNode *configuration = programme_config(first, second);
    JsonObject *settings = json_node_get_object(configuration);
    json_object_set_string_member(settings, "title", title);
    json_object_set_int_member(settings, "queue_player", slot);
    json_object_set_int_member(settings, "queue_transition", transition_mode);
    json_object_set_double_member(settings, "queue_overlap", 0);
    gchar *name = g_strdup_printf("Oynatıcı %u yayın sırası", slot);
    JsonNode *saved = document_save("programme", name, settings);
    json_node_free(saved); g_free(name);
    return configuration;
}

static guint64
prompt2_actual_duration(RadioSAudioEngine *engine, guint deck)
{
    RadioSAudioSnapshot actual;
    radios_audio_engine_snapshot(engine, &actual);
    g_assert_true(actual.buses[RADIOS_AUDIO_BUS_MAIN].ready);
    g_assert_true(actual.buses[RADIOS_AUDIO_BUS_MAIN].gate);
    g_assert_true(actual.decks[deck].main_routed);
    g_assert_cmpint(actual.decks[deck].output_bus, ==, RADIOS_AUDIO_BUS_MAIN);
    g_assert_cmpuint(actual.decks[deck].cue_out_frame, >, 0);
    return actual.decks[deck].cue_out_frame / 48;
}

static void
prompt2_private_programme_pcm_pause_priority_stop(void)
{
    Window window = window_open(); assert_gates_off(&window);
    JsonNode *first = media_import_rhythm(&window, "P2 gerçek parça A", 600, 0.25, "music", 120);
    JsonNode *second = media_import_rhythm(&window, "P2 gerçek parça B", 1700, 0.2, "music", 120);
    seed_programme(&window, NULL, NULL);
    JsonNode *first_configuration = prompt2_save_private(1, "Kabul edilmiş özel program A", first, second, 0);
    JsonNode *second_configuration = prompt2_save_private(2, "Kabul edilmiş özel program B", second, first, 0);
    navigate(&window, "broadcast");
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    player_queue_select(&window, "1", "P2 gerçek parça A"); click(window.root, "programme-play");
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 9600);
    guint64 first_duration = prompt2_actual_duration(engine, 0);
    g_assert_cmpuint(first_duration, ==, 12000);
    prompt2_header_wait(&window, "Kabul edilmiş özel program A", "P2 gerçek parça A", TRUE, FALSE, 1, first_duration, 100);
    RadioSBroadcastHeaderSnapshot first_ends = prompt2_end_wait(&window, "", TRUE, FALSE, FALSE);
    g_assert_cmpuint(first_ends.track_remaining_us, >, 8 * G_TIME_SPAN_SECOND);
    g_assert_cmpint(first_ends.programme_remaining_us - first_ends.track_remaining_us, >, 10 * G_TIME_SPAN_SECOND);
    g_assert_cmpint(first_ends.programme_remaining_us - first_ends.track_remaining_us, <, 13 * G_TIME_SPAN_SECOND);
    click(window.root, "programme-pause");
    RadioSAudioRunSnapshot paused_run = player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PAUSED, 0, 0);
    /* Runner pause freezes owned PCM but intentionally leaves the underlying
     * decoder deck PLAYING. The header must observe the owned runner state. */
    RadioSBroadcastHeaderSnapshot paused = prompt2_header_wait(&window, "Kabul edilmiş özel program A", "P2 gerçek parça A", TRUE, TRUE, 0, first_duration, paused_run.actual_current_frame / 48);
    g_assert_cmpuint(paused.position_ms, ==, paused_run.actual_current_frame / 48);
    prompt2_end_wait(&window, NULL, FALSE, FALSE, TRUE);
    goffset paused_offset = capture_offset("MAIN"); run_for(200);
    g_assert_cmpfloat(tone_energy("MAIN", paused_offset, 600), <, 0.01);
    RadioSAudioRunSnapshot still_run = player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PAUSED, 0, paused_run.actual_current_frame);
    g_assert_cmpuint(still_run.actual_current_frame, ==, paused_run.actual_current_frame);
    RadioSBroadcastHeaderSnapshot still = prompt2_header_wait(&window, "Kabul edilmiş özel program A", "P2 gerçek parça A", TRUE, TRUE, 0, first_duration, paused_run.actual_current_frame / 48);
    g_assert_cmpuint(still.position_ms, ==, paused.position_ms);
    player_queue_select(&window, "2", "P2 gerçek parça B"); click(window.root, "programme-play");
    player_run_wait(engine, 1, RADIOS_AUDIO_RUN_PLAYING, 0, 9600);
    guint64 second_duration = prompt2_actual_duration(engine, 1);
    prompt2_header_wait(&window, "Kabul edilmiş özel program B", "P2 gerçek parça B", TRUE, FALSE, 1, second_duration, 100);
    goffset offset = capture_offset("MAIN"); run_for(200);
    g_assert_cmpfloat(tone_energy("MAIN", offset, 1700), >, 0.05);
    g_assert_cmpfloat(tone_energy("MAIN", offset, 600), <, 0.01);
    click(window.root, "programme-pause"); player_run_wait(engine, 1, RADIOS_AUDIO_RUN_PAUSED, 0, 0);
    prompt2_header_wait(&window, "Kabul edilmiş özel program A", "P2 gerçek parça A", TRUE, TRUE, 0, first_duration, 100);
    player_queue_select(&window, "1", "P2 gerçek parça A"); click(window.root, "programme-stop");
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_IDLE, 0, 0);
    prompt2_header_wait(&window, "Kabul edilmiş özel program B", "P2 gerçek parça B", TRUE, TRUE, 0, second_duration, 100);
    player_queue_select(&window, "2", "P2 gerçek parça B"); click(window.root, "programme-stop");
    player_run_wait(engine, 1, RADIOS_AUDIO_RUN_IDLE, 0, 0);
    prompt2_header_wait(&window, NULL, NULL, FALSE, FALSE, 0, 0, 0);
    prompt2_end_wait(&window, "", FALSE, FALSE, FALSE);
    radios_audio_engine_unref(engine); json_node_free(first_configuration); json_node_free(second_configuration);
    json_node_free(first); json_node_free(second); window_close(&window);
}


static void prompt2_saved_playlist_name_end_pause_resume_stop_MAIN_PCM(void)
{
    Window window = window_open(); assert_gates_off(&window); seed_programme(&window, NULL, NULL);
    JsonNode *first = media_import_rhythm(&window, "Header ordinary playlist A", 700, .25, "music", 120);
    JsonNode *second = media_import_rhythm(&window, "Header ordinary playlist B", 1700, .25, "music", 120);
    JsonNode *playlist = playlist_create("Gerçek kaydedilmiş header playlisti", first, second);
    JsonNode *append = playlist_create("Sonradan eklenen diğer header playlisti", second, NULL);
    navigate(&window, "broadcast"); players_show(&window); click(window.root, "deck-0-playlist");
    SavedLoaderList loader = {"Gerçek kaydedilmiş header playlisti", NULL, NULL, {0}};
    wait_until(saved_loader_list_ready, &loader, "real saved playlist reaches native card loader");
    gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(loader.tree)), &loader.iter);
    GtkWidget *accept = gtk_dialog_get_widget_for_response(GTK_DIALOG(loader.dialog), GTK_RESPONSE_ACCEPT);
    wait_until(widget_enabled, accept, "selected saved playlist can load on actual card"); gtk_button_clicked(GTK_BUTTON(accept)); pump();
    click(window.root, "deck-0-play"); RadioSAudioUiSnapshot playing = deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 200);
    RadioSBroadcastHeaderSnapshot observed = prompt2_end_wait(&window, "Gerçek kaydedilmiş header playlisti", FALSE, TRUE, FALSE);
    g_assert_true(observed.current); g_assert_false(observed.programme_active); g_assert_cmpstr(prompt2_value(&window, "programme"), ==, "—");
    g_assert_cmpuint(observed.track_remaining_us, >, 8 * G_TIME_SPAN_SECOND);
    g_assert_cmpint(observed.playlist_end_utc_us - g_get_real_time(), >, 18 * G_TIME_SPAN_SECOND);
    g_assert_cmpuint(observed.playlist_remaining_us, >, observed.track_remaining_us + 10 * G_TIME_SPAN_SECOND);
    goffset pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .05);
    GtkWidget *card = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0"); g_assert_nonnull(card);
    saved_playlist_card_drop(&window, card, radios_json_string(json_node_get_object(append), "_id", ""), 3);
    RadioSBroadcastHeaderSnapshot appended = prompt2_end_wait(&window, "Gerçek kaydedilmiş header playlisti", FALSE, TRUE, FALSE);
    RadioSAudioUiSnapshot after_append = deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, playing.position_ms);
    g_assert_cmpuint(after_append.generation, ==, playing.generation);
    g_assert_cmpstr(appended.title, ==, "Header ordinary playlist A");
    g_assert_cmpint(appended.playlist_remaining_us, <=, observed.playlist_remaining_us + 200000);
    g_assert_cmpint(appended.playlist_remaining_us - appended.track_remaining_us, >, 10 * G_TIME_SPAN_SECOND);
    click(window.root, "deck-0-pause"); deck_wait(&window, 0, RADIOS_AUDIO_UI_PAUSED, 200); run_for(200);
    RadioSBroadcastHeaderSnapshot paused = prompt2_end_wait(&window, "Gerçek kaydedilmiş header playlisti", FALSE, FALSE, TRUE);
    g_assert_cmpuint(paused.track_remaining_us, >, 0); g_assert_cmpuint(paused.playlist_remaining_us, >, 0);
    run_for(400); observed = prompt2_end_wait(&window, "Gerçek kaydedilmiş header playlisti", FALSE, FALSE, TRUE);
    g_assert_cmpuint(observed.position_ms, ==, paused.position_ms);
    g_assert_cmpuint(observed.track_remaining_us, ==, paused.track_remaining_us);
    click(window.root, "deck-0-play"); RadioSAudioUiSnapshot resumed = deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, paused.position_ms + 100);
    g_assert_cmpuint(resumed.generation, ==, playing.generation);
    prompt2_end_wait(&window, "Gerçek kaydedilmiş header playlisti", FALSE, TRUE, FALSE);
    click(window.root, "deck-0-stop"); deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    prompt2_header_wait(&window, NULL, NULL, FALSE, FALSE, 0, 0, 0);
    prompt2_end_wait(&window, "", FALSE, FALSE, FALSE);
    assert_saved_playlist_unchanged(playlist); assert_saved_playlist_unchanged(append);
    g_assert_cmpfloat(measure("CUE").peak[0], <, .0001);
    json_node_free(append); json_node_free(playlist); json_node_free(first); json_node_free(second); window_close(&window);
    g_test_message("Ordinary saved playlist MAIN shows genuine name and end clocks; appending a different playlist preserves the playing list's name, contiguous end and generation; pause/resume/Stop preserve saved documents.");
}

static gboolean prompt2_waiting_without_track(gpointer value)
{
    Window *window = value; RadioSBroadcastHeaderSnapshot snapshot = {0};
    return radios_broadcast_header_snapshot(window->ctx, &snapshot) && snapshot.waiting && !snapshot.current;
}

static void
prompt2_private_manual_wait_preserves_programme(void)
{
    Window window = window_open(); assert_gates_off(&window);
    JsonNode *media = media_import(&window, "P2 manuel bekleme parçası", 900, 0.2, "music");
    JsonNode *markers = media_markers_for_waveform(media); json_node_free(markers);
    seed_programme(&window, NULL, NULL);
    JsonNode *configuration = prompt2_save_private(1, "Bekleyen kabul edilmiş özel program", media, media, 3);
    navigate(&window, "broadcast");
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    player_queue_select(&window, "1", "P2 manuel bekleme parçası");
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(find(window.root, "programme-transition-mode"))), ==, 3);
    click(window.root, "programme-play"); player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 48000);
    guint64 duration = prompt2_actual_duration(engine, 0);
    g_assert_cmpuint(duration, ==, 5500);
    prompt2_header_wait(&window, "Bekleyen kabul edilmiş özel program", "P2 manuel bekleme parçası", TRUE, FALSE, 1, duration, 1000);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_MANUAL_WAIT, 0, 0);
    prompt2_header_wait(&window, "Bekleyen kabul edilmiş özel program", NULL, FALSE, FALSE, 0, 0, 0);
    /* The controller's public status follows its polling timer, while the
     * raw PCM wait above can be observed before that timer has run. */
    wait_until(prompt2_waiting_without_track, &window, "accepted manual wait reaches actual header waiting flag");
    RadioSBroadcastHeaderSnapshot waiting = prompt2_end_wait(&window, NULL, FALSE, FALSE, FALSE);
    g_assert_true(waiting.waiting);
    combo(window.root, "programme-player-selector", "0");
    wait_until(window_ready, &window, "global queue selected while private accepted programme waits");
    prompt2_header_wait(&window, "Bekleyen kabul edilmiş özel program", NULL, FALSE, FALSE, 0, 0, 0);
    prompt2_end_wait(&window, NULL, FALSE, FALSE, FALSE);
    player_queue_select(&window, "1", "P2 manuel bekleme parçası"); click(window.root, "programme-stop");
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_IDLE, 0, 0);
    prompt2_header_wait(&window, NULL, NULL, FALSE, FALSE, 0, 0, 0);
    prompt2_end_wait(&window, "", FALSE, FALSE, FALSE);
    radios_audio_engine_unref(engine); json_node_free(configuration); json_node_free(media); window_close(&window);
}


static void prompt2_autodj_exit_predicts_audible_track_end_MAIN_PCM(void)
{
    Window window = window_open(); assert_gates_off(&window); seed_programme(&window, NULL, NULL);
    JsonNode *first = tempo_return_markers(media_import_rhythm(&window, "Header early AutoDJ OUT A", 700, .25, "music", 120), 4000);
    JsonNode *second = media_import_rhythm(&window, "Header full incoming B", 1700, .25, "music", 120);
    seed_programme(&window, first, second);
    navigate(&window, "broadcast");
    combo_index(window.root, "programme-transition-mode", 1);
    spin(window.root, "programme-transition-overlap", 2);
    click(window.root, "programme-play"); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 1000);
    RadioSBroadcastHeaderSnapshot before = prompt2_end_wait(&window, "", TRUE, FALSE, FALSE);
    /* OUT4s starts a two-second outgoing envelope. Its physical source lasts
     * twelve seconds, but the audible retirement ends at6s from initial play. */
    g_assert_cmpuint(before.position_ms, >=, 1000); g_assert_cmpuint(before.position_ms, <, 2000);
    g_assert_cmpuint(before.duration_ms, >=, 12000); g_assert_cmpint(before.track_remaining_us, >, 4 * G_TIME_SPAN_SECOND);
    g_assert_cmpint(before.track_remaining_us, <, 5 * G_TIME_SPAN_SECOND + 100000);
    g_assert_cmpint(before.programme_remaining_us - before.track_remaining_us, >, 9 * G_TIME_SPAN_SECOND);
    g_assert_cmpint(before.programme_remaining_us - before.track_remaining_us, <, 11 * G_TIME_SPAN_SECOND);
    RadioSPlayoutSnapshot overlap = run_wait(&window, RADIOS_PLAYOUT_OVERLAP, 0, 4200);
    g_assert_cmpuint(overlap.position_ms, >=, 4000);
    RadioSBroadcastHeaderSnapshot during = prompt2_end_wait(&window, "", TRUE, FALSE, FALSE);
    g_assert_cmpint(during.track_remaining_us, >, 0); g_assert_cmpint(during.track_remaining_us, <, 2 * G_TIME_SPAN_SECOND);
    goffset pcm = capture_offset("MAIN"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .015); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .01);
    run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 2100);
    RadioSBroadcastHeaderSnapshot promoted = prompt2_end_wait(&window, "", TRUE, FALSE, FALSE);
    g_assert_cmpstr(promoted.title, ==, "Header full incoming B");
    g_assert_cmpint(promoted.track_remaining_us, >, 8 * G_TIME_SPAN_SECOND);
    g_assert_cmpint(llabs(promoted.programme_remaining_us - promoted.track_remaining_us), <, 100000);
    click(window.root, "programme-stop"); run_wait(&window, RADIOS_PLAYOUT_IDLE, 0, 0);
    prompt2_end_wait(&window, "", FALSE, FALSE, FALSE);
    json_node_free(first); json_node_free(second); window_close(&window);
    g_test_message("Before and during real OUT4s+fade2s MAIN handover, track end predicts audible retirement6s instead of physical12s; finite programme end does not double-count the overlap.");
}

typedef struct { Window *window; const gchar *name, *value; } LiveStripValue;
static gboolean live_strip_value_matches(gpointer data)
{
    LiveStripValue *expected = data; GtkWidget *label = find(expected->window->root, expected->name);
    return GTK_IS_LABEL(label) && !g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), expected->value);
}
static void live_strip_wait(Window *window, const gchar *name, const gchar *value)
{
    LiveStripValue expected = {window, name, value};
    RadioSBroadcastHeaderSnapshot observed = {0}; radios_broadcast_header_snapshot(window->ctx, &observed);
    g_test_message("Live strip check %s='%s' [deck%u local%d paused%d]", name, value, observed.deck, observed.local_deck, observed.paused);
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!live_strip_value_matches(&expected) && g_get_monotonic_time() < deadline) pump();
    GtkWidget *label = find(window->root, name);
    if (!live_strip_value_matches(&expected)) g_error("Live shared strip %s expected '%s', actual '%s'", name, value,
        GTK_IS_LABEL(label) ? gtk_label_get_text(GTK_LABEL(label)) : "<missing>");
}
static gboolean stream_failed(gpointer data)
{ RadioSAudioOutputSnapshot stream = {0}; radios_audio_output_snapshot(data, &stream); return stream.state == RADIOS_AUDIO_OUTPUT_FAILED; }
static void prompt2_live_strip_owned_next_and_diagnostics(void)
{
    Window window = window_open(); assert_gates_off(&window);
    live_strip_wait(&window, "header-onair-title", "—"); live_strip_wait(&window, "header-next-track-title", "—");
    live_strip_wait(&window, "header-live-main", "Açık"); live_strip_wait(&window, "header-live-internet", "Kapalı");
    JsonNode *first = media_import_rhythm(&window, "Same actual song", 600, .2, "music", 120);
    JsonNode *manual_next = media_import_rhythm(&window, "Manual playlist successor", 1400, .2, "music", 120);
    JsonNode *managed_next = media_import_rhythm(&window, "Managed exact successor", 2200, .2, "music", 120);
    seed_programme(&window, NULL, NULL); navigate(&window, "broadcast"); players_show(&window);
    JsonNode *playlist = playlist_create("Live header ordinary queue", first, manual_next);
    GtkWidget *card = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0");
    saved_playlist_card_drop(&window, card, radios_json_string(json_node_get_object(playlist), "_id", ""), 2);
    click(card, "deck-0-play"); deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
    live_strip_wait(&window, "header-onair-title", "Same actual song");
    live_strip_wait(&window, "header-active-source", "Aktif kaynak  Oynatıcı 1");
    live_strip_wait(&window, "header-next-track-title", "Manual playlist successor");
    JsonNode *private = prompt2_save_private(2, "Exact second owner", first, managed_next, 0);
    radios_data_refresh(window.ctx); player_queue_select(&window, "2", "Same actual song"); click(window.root, "programme-play");
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    player_run_wait(engine, 1, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    RadioSBroadcastHeaderSnapshot owner = prompt2_header_wait(&window, "Exact second owner", "Same actual song", TRUE, FALSE, 2, 12000, 100);
    g_assert_true(owner.local_deck); g_assert_cmpuint(owner.deck, ==, 1); g_assert_true(g_uuid_string_is_valid(owner.entry_id));
    live_strip_wait(&window, "header-active-source", "Aktif kaynak  Oynatıcı 2");
    live_strip_wait(&window, "header-next-track-title", "Managed exact successor");
    goffset pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 600), >, .04);
    click(card, "deck-0-stop"); click(window.root, "programme-pause"); player_run_wait(engine, 1, RADIOS_AUDIO_RUN_PAUSED, 0, 0);
    live_strip_wait(&window, "header-onair-title", "Same actual song · Duraklatıldı");
    live_strip_wait(&window, "header-next-track-title", "Managed exact successor");
    const gchar *clock = gtk_label_get_text(GTK_LABEL(find(window.root, "header-track-countdown"))); gchar *frozen = g_strdup(clock);
    run_for(450); g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(window.root, "header-track-countdown"))), ==, frozen); g_free(frozen);
    click(window.root, "programme-stop"); player_run_wait(engine, 1, RADIOS_AUDIO_RUN_IDLE, 0, 0);
    live_strip_wait(&window, "header-onair-title", "—"); live_strip_wait(&window, "header-next-track-title", "—");
    GError *error = NULL; require_call(radios_audio_engine_set_bus_gate(engine, RADIOS_AUDIO_BUS_MAIN, FALSE, &error), &error);
    live_strip_wait(&window, "header-live-main", "Kapalı");
    live_strip_wait(&window, "footer-warning-status", "Uyarı  MAIN çıkışı kapalı");
    click(window.root, "header-diagnostic-details"); GtkWidget *dialog = dialog_find("diagnostic-details-dialog"); g_assert_nonnull(dialog);
    GtkWidget *details = find(dialog, "diagnostic-live-observations"); g_assert_true(GTK_IS_LABEL(details));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(details)), "MAIN çıkışı: Kapalı"));
    g_assert_false(gtk_widget_is_sensitive(find(dialog, "diagnostic-acknowledge")));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
    require_call(radios_audio_engine_set_bus_gate(engine, RADIOS_AUDIO_BUS_MAIN, TRUE, &error), &error);
    live_strip_wait(&window, "header-live-main", "Açık");
    RadioSAudioOutput *stream = radios_audio_engine_output_acquire(engine, RADIOS_AUDIO_OUTPUT_STREAM);
    /* Real connection refusal, with an unprivileged port reserved only for
     * this private engine; no remote endpoint or fixture response is forged. */
    GSocket *socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, &error); g_assert_no_error(error);
    GInetAddress *loopback = g_inet_address_new_from_string("127.0.0.1"); GSocketAddress *address = g_inet_socket_address_new(loopback, 0);
    g_assert_true(g_socket_bind(socket, address, FALSE, &error)); g_assert_no_error(error);
    GSocketAddress *bound = g_socket_get_local_address(socket, &error); g_assert_no_error(error);
    guint port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
    gchar *url = g_strdup_printf("http://127.0.0.1:%u/private-header-stream", port);
    require_call(radios_audio_output_start_stream(stream, url, "source", "private-fixture-only", 128, &error), &error);
    wait_until(stream_failed, stream, "actual private stream TCP refusal reaches terminal output error");
    live_strip_wait(&window, "header-live-internet", "Hata");
    RadioSAudioOutputSnapshot failure = {0}; radios_audio_output_snapshot(stream, &failure); g_assert_true(failure.error[0] != '\0');
    gchar *warning = g_strdup_printf("Uyarı  %s", failure.error); live_strip_wait(&window, "footer-warning-status", warning); g_free(warning);
    click(window.root, "header-diagnostic-details"); dialog = dialog_find("diagnostic-details-dialog");
    details = find(dialog, "diagnostic-live-observations"); g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(details)), failure.error));
    g_assert_false(gtk_widget_is_sensitive(find(dialog, "diagnostic-acknowledge")));
    saved_player_capture(window.root, "RADIOS_LIVE_HEADER_CAPTURE");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE);
    g_object_unref(bound); g_object_unref(address); g_object_unref(loopback); g_object_unref(socket); g_free(url);
    radios_audio_output_unref(stream); radios_audio_engine_unref(engine);
    json_node_free(private); json_node_free(playlist); json_node_free(first); json_node_free(manual_next); json_node_free(managed_next); window_close(&window);
    g_test_message("Real ordinary and managed queues with identical current titles select the exact owner and next track; pause freezes, stop clears; MAIN gate and actual stream failure reach live footer and disabled-ack details.");
}

static int
prompt2_run(void)
{
    g_test_add_func("/prompt2-header/live-strip-exact-owner-next-diagnostics-MAIN-PCM", prompt2_live_strip_owned_next_and_diagnostics);
    g_test_add_func("/prompt2-header/private-programme-PCM-pause-priority-stop", prompt2_private_programme_pcm_pause_priority_stop);
    g_test_add_func("/prompt2-header/private-manual-wait-preserves-programme", prompt2_private_manual_wait_preserves_programme);
    g_test_add_func("/prompt2-header/saved-playlist-name-end-pause-resume-stop-MAIN-PCM", prompt2_saved_playlist_name_end_pause_resume_stop_MAIN_PCM);
    g_test_add_func("/prompt2-header/AutoDJ-OUT-audible-track-end-MAIN-PCM", prompt2_autodj_exit_predicts_audible_track_end_MAIN_PCM);
    return g_test_run();
}

int
main(int argc, char **argv)
{
    return radios_prompt2_fixture_main(argc, argv);
}
