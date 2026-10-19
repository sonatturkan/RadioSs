/* Reuse the genuine GTK application, measured WAV import and isolated virtual
 * MAIN capture harness. Only the tests below are registered by this binary. */
#define main radios_copyright_borrowed_playout_main
#include "test_playout_ui.c"
#undef main
#include "radios/player_binding.h"

static gchar *copyright_timestamp(gint64 utc_us)
{
    GDateTime *date = g_date_time_new_from_unix_utc(utc_us / G_TIME_SPAN_SECOND);
    gchar *stamp = g_date_time_format_iso8601(date); g_date_time_unref(date); return stamp;
}

static void copyright_add_rule(JsonArray *rules, const gchar *title, gint64 end_us)
{
    JsonObject *rule = json_object_new(); gchar *identifier = g_uuid_string_random();
    gchar *start = copyright_timestamp(g_get_real_time() - 3600 * G_TIME_SPAN_SECOND);
    gchar *end = copyright_timestamp(end_us);
    json_object_set_string_member(rule, "id", identifier); json_object_set_string_member(rule, "artist", "");
    json_object_set_string_member(rule, "album", ""); json_object_set_string_member(rule, "title", title);
    json_object_set_string_member(rule, "starts_at", start); json_object_set_string_member(rule, "ends_at", end);
    json_object_set_string_member(rule, "note", "Private native copyright acceptance");
    json_object_set_boolean_member(rule, "enabled", TRUE); json_array_add_object_element(rules, rule);
    g_free(identifier); g_free(start); g_free(end);
}

static JsonNode *copyright_save(gboolean restriction, const gchar *first_title, gint64 first_end,
                                const gchar *expired_title)
{
    JsonObject *policy = json_object_new(); JsonArray *rules = json_array_new();
    json_object_set_int_member(policy, "schema", 1); json_object_set_boolean_member(policy, "restrict", restriction);
    json_object_set_string_member(policy, "unregistered", "allow");
    if (first_title) copyright_add_rule(rules, first_title, first_end);
    if (expired_title) copyright_add_rule(rules, expired_title, g_get_real_time() - 60 * G_TIME_SPAN_SECOND);
    json_object_set_array_member(policy, "rules", rules);
    JsonNode *saved = document_save("copyright_policy", "main", policy); json_object_unref(policy); return saved;
}

static JsonNode *copyright_append_media(JsonNode *playlist, JsonNode *detail)
{
    JsonObject *record = json_node_get_object(playlist), *media = json_object_get_object_member(json_node_get_object(detail), "media");
    JsonObject *version = json_object_get_object_member(json_node_get_object(detail), "version"), *metadata = json_object_new();
    const gchar *fields[] = {"title", "artist", "album", "genre"};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i)
        json_object_set_string_member(metadata, fields[i], radios_json_string(media, fields[i], ""));
    json_object_set_string_member(metadata, "key", ""); json_object_set_string_member(metadata, "location", "/untrusted.wav");
    json_object_set_string_member(metadata, "comment", "Private actual media");
    json_object_set_int_member(metadata, "duration_ms", radios_json_int(version, "duration_ms", 0));
    json_object_set_double_member(metadata, "bpm", 0); bind(metadata, json_node_get_object(detail));
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "playlist_id", radios_json_string(record, "_id", ""));
    json_object_set_int_member(object, "expected_revision", radios_json_int(record, "_rev", 0));
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    json_object_set_object_member(object, "metadata", metadata);
    JsonNode *updated = request("/v1/workflows/desktop-playlists/entry-save", body);
    json_node_free(body); json_node_free(playlist); return updated;
}

static void copyright_playlist_exact(JsonNode *expected)
{
    assert_saved_playlist_unchanged(expected);
    JsonNode *body = body_new(), *response = request("/v1/workflows/desktop-playlists/list", body); json_node_free(body);
    JsonObject *saved = json_node_get_object(expected); JsonArray *lists = json_object_get_array_member(json_node_get_object(response), "playlists");
    JsonObject *found = NULL;
    for (guint i = 0; i < json_array_get_length(lists); ++i) {
        JsonObject *item = json_array_get_object_element(lists, i);
        if (!g_strcmp0(radios_json_string(item, "_id", ""), radios_json_string(saved, "_id", ""))) found = item;
    }
    g_assert_nonnull(found);
    g_assert_true(json_node_equal(json_object_get_member(saved, "tracks"), json_object_get_member(found, "tracks")));
    json_node_free(response);
}

static void copyright_card_play(Window *window, JsonNode *playlist)
{
    JsonObject *record = json_node_get_object(playlist); GError *error = NULL; gchar *player = NULL;
    require_call(radios_broadcast_play_on_player(window->ctx, json_object_get_array_member(record, "tracks"),
        radios_json_string(record, "name", ""), &player, &error), &error);
    g_assert_nonnull(player); g_free(player);
}

typedef struct { Window *window; const gchar *media_id; RadioSAudioUiState state; } CopyrightDeck;
static gboolean copyright_deck_matches(gpointer value)
{
    CopyrightDeck *want = value; RadioSAudioUiSnapshot snapshot;
    return radios_audio_ui_get_snapshot(want->window->ctx, 0, &snapshot) && snapshot.state == want->state &&
        !g_strcmp0(snapshot.media_id, want->media_id) && (want->state != RADIOS_AUDIO_UI_PLAYING || snapshot.position_ms >= 100);
}
static void copyright_deck_wait(Window *window, JsonNode *detail, RadioSAudioUiState state)
{
    CopyrightDeck want = {window, radios_json_string(json_object_get_object_member(json_node_get_object(detail), "media"), "_id", ""), state};
    wait_until(copyright_deck_matches, &want, "correct direct card media and transport state after copyright filtering");
}

static void copyright_local_expiry(gboolean pause_first)
{
    Window window = window_open(); saved_player_notices_attach(&window); navigate(&window, "broadcast");
    JsonNode *first = media_import(&window, "Telif kısa süreli kaynak", 700, .25, "music");
    JsonNode *expired = media_import(&window, "Telif süresi bitmiş kaynak", 1700, .25, "music");
    JsonNode *unmatched = media_import(&window, "Telif kaydı olmayan kaynak", 2700, .25, "music");
    JsonNode *playlist = copyright_append_media(playlist_create("Telif doğrudan player karma liste", first, expired), unmatched);
    const gchar *first_title = radios_json_string(json_object_get_object_member(json_node_get_object(first), "media"), "title", "");
    const gchar *expired_title = radios_json_string(json_object_get_object_member(json_node_get_object(expired), "media"), "title", "");
    gint64 expires = (g_get_real_time() / G_TIME_SPAN_SECOND + 3) * G_TIME_SPAN_SECOND;
    JsonNode *saved_policy = copyright_save(TRUE, first_title, expires, expired_title);
    copyright_card_play(&window, playlist); copyright_deck_wait(&window, first, RADIOS_AUDIO_UI_PLAYING);
    goffset from = capture_offset("MAIN"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", from, 700), >, .08);
    g_assert_cmpfloat(tone_energy("MAIN", from, 1700), <, .01);
    if (pause_first) {
        click(window.root, "deck-0-pause"); copyright_deck_wait(&window, first, RADIOS_AUDIO_UI_PAUSED);
        while (g_get_real_time() < expires + 300 * G_TIME_SPAN_MILLISECOND) pump();
        copyright_deck_wait(&window, unmatched, RADIOS_AUDIO_UI_READY);
        from = capture_offset("MAIN"); run_for(250);
        g_assert_cmpfloat(capture("MAIN", from).peak[0], <, .0001);
        click(window.root, "deck-0-play"); copyright_deck_wait(&window, unmatched, RADIOS_AUDIO_UI_PLAYING);
    } else {
        copyright_deck_wait(&window, unmatched, RADIOS_AUDIO_UI_PLAYING);
    }
    from = capture_offset("MAIN"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", from, 2700), >, .08);
    g_assert_cmpfloat(tone_energy("MAIN", from, 700), <, .01);
    g_assert_cmpfloat(tone_energy("MAIN", from, 1700), <, .01);
    GtkWidget *card = g_object_get_data(G_OBJECT(window.ctx->parent), "radios-deck-owner-0");
    JsonArray *inputs = NULL; guint index = 0;
    g_assert_true(radios_broadcast_player_playlist(window.ctx, card, &inputs, NULL, &index, NULL));
    g_assert_cmpuint(json_array_get_length(inputs), ==, 3); g_assert_cmpuint(index, ==, 2); json_array_unref(inputs);
    copyright_playlist_exact(playlist);
    click(window.root, "deck-0-stop"); deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    json_node_free(saved_policy); saved_policy = copyright_save(FALSE, first_title, expires, expired_title);
    copyright_card_play(&window, playlist); copyright_deck_wait(&window, first, RADIOS_AUDIO_UI_PLAYING);
    click(window.root, "deck-0-next"); copyright_deck_wait(&window, expired, RADIOS_AUDIO_UI_PLAYING);
    from = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", from, 1700), >, .08);
    copyright_playlist_exact(playlist);
    click(window.root, "deck-0-stop"); deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    window_close(&window); json_node_free(saved_policy); json_node_free(playlist);
    json_node_free(first); json_node_free(expired); json_node_free(unmatched);
}
static void copyright_local_running(void) { copyright_local_expiry(FALSE); }
static void copyright_local_paused(void) { copyright_local_expiry(TRUE); }

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    main_sink = g_getenv("RADIOS_TEST_MAIN_SINK"); cue_sink = g_getenv("RADIOS_TEST_CUE_SINK"); monitor_sink = g_getenv("RADIOS_TEST_MONITOR_SINK");
    g_assert_nonnull(main_sink); g_assert_nonnull(cue_sink); g_assert_nonnull(monitor_sink);
    GError *error = NULL; g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    application = gtk_application_new("org.radios.CopyrightDirectPlaybackAcceptance", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/native-playout/copyright-direct-card-expiry-skip-toggle-PCM", copyright_local_running);
    g_test_add_func("/native-playout/copyright-direct-card-paused-expiry-skip-toggle-PCM", copyright_local_paused);
    gint result = g_test_run(); radios_data_client_unref(reader); g_object_unref(application); radios_config_clear(&config); return result;
}
