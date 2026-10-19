/* Real authenticated media, accepted runner leases and isolated MAIN PCM. */
#define main radios_player_content_legacy_fixture_main
#include "test_playout_ui.c"
#undef main
#include "radios/stream_metadata.h"

static GtkWidget *content_label(Window *window, guint deck)
{
    gchar *name = g_strdup_printf("deck-%u-content-status", deck);
    GtkWidget *label = find(window->root, name); g_free(name);
    g_assert_true(GTK_IS_LABEL(label)); return label;
}
typedef struct { GtkWidget *label; const gchar *text; } ContentLabel;
static gboolean content_label_matches(gpointer value)
{
    ContentLabel *condition = value;
    return g_str_equal(gtk_label_get_text(GTK_LABEL(condition->label)), condition->text);
}
static void content_label_wait(Window *window, guint deck, const gchar *text)
{
    ContentLabel condition = {content_label(window, deck), text};
    gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    while (!content_label_matches(&condition) && g_get_monotonic_time() < deadline) pump();
    if (!content_label_matches(&condition))
        g_error("Player %u status expected '%s', actual '%s'", deck + 1, text,
            gtk_label_get_text(GTK_LABEL(condition.label)));
}
static void test_manual_content_kinds_pcm(void)
{
    Window window = window_open(); GError *error = NULL;
    require_call(radios_audio_ui_set_deck_main(window.ctx, 4, TRUE, &error), &error);
    const gchar *kinds[] = {"sweeper", "announcement", "advertisement", "effect", "voice_track", "jingle"};
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) {
        gchar *title = g_strdup_printf("Actual player classification %s", kinds[i]);
        JsonNode *media = media_import(&window, title, 1700, .2, kinds[i]);
        require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(media), 4, FALSE, &error), &error);
        deck_wait(&window, 4, RADIOS_AUDIO_UI_READY, 0);
        gchar *expected = g_strdup_printf("Hazır · %s", radios_programme_kind_label(kinds[i]));
        content_label_wait(&window, 4, expected); g_free(expected);
        require_call(radios_audio_ui_play(window.ctx, 4, &error), &error);
        deck_wait(&window, 4, RADIOS_AUDIO_UI_PLAYING, 100);
        expected = g_strdup_printf("Çalıyor · %s", radios_programme_kind_label(kinds[i]));
        content_label_wait(&window, 4, expected); g_free(expected);
        goffset at = capture_offset("MAIN"); run_for(120);
        g_assert_cmpfloat(tone_energy("MAIN", at, 1700), >, .06);
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(window.root, "deck-4-title"))), ==, title);
        require_call(radios_audio_ui_pause(window.ctx, 4, &error), &error);
        deck_wait(&window, 4, RADIOS_AUDIO_UI_PAUSED, 0);
        expected = g_strdup_printf("Duraklatıldı · %s", radios_programme_kind_label(kinds[i]));
        content_label_wait(&window, 4, expected); g_free(expected);
        RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
        require_call(radios_audio_engine_unload(engine, 4, &error), &error);
        radios_audio_engine_unref(engine); deck_wait(&window, 4, RADIOS_AUDIO_UI_EMPTY, 0);
        content_label_wait(&window, 4, "");
        json_node_free(media); g_free(title);
    }
    window_close(&window);
    g_test_message("Real sweeper, announcement, advertisement, effect, voice-track and jingle cards show ready/playing/paused classification without replacing their actual title, and clear after unload.");
}
static JsonNode *content_intro(JsonNode *detail)
{
    JsonObject *media = json_object_get_object_member(json_node_get_object(detail), "media");
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body), *markers = json_object_new();
    json_object_set_string_member(object, "media_id", radios_json_string(media, "_id", ""));
    json_object_set_int_member(object, "expected_revision", radios_json_int(media, "_rev", 0));
    json_object_set_null_member(object, "expected_marker_revision");
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    const gchar *names[] = {"cue_in_ms", "intro_ms", "out_ms", "fade_start_ms", "fade_end_ms", "auto_next_ms", "overlap_ms"};
    const guint values[] = {0, 3500, 6001, 6001, 6001, 6001, 0};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) json_object_set_int_member(markers, names[i], values[i]);
    json_object_set_boolean_member(markers, "automatic", FALSE); json_object_set_object_member(object, "markers", markers);
    JsonNode *saved = request("/v1/workflows/desktop-media/markers-save", body);
    json_node_free(body); json_node_free(detail); return saved;
}
static JsonNode *content_prepared(JsonNode *music, JsonNode *playlist)
{
    JsonNode *programme = programme_config(music, NULL);
    JsonObject *event = json_array_get_object_element(json_object_get_array_member(json_node_get_object(programme), "events"), 0);
    JsonObject *policy = json_object_new(), *rule = json_object_new(); JsonArray *rules = json_array_new();
    json_object_set_int_member(policy, "schema", 1); json_object_set_string_member(policy, "seed", "actual-sweeper-content-status");
    gchar *id = g_uuid_string_random(); json_object_set_string_member(rule, "id", id); g_free(id);
    json_object_set_string_member(rule, "playlist_id", radios_json_string(json_node_get_object(playlist), "_id", ""));
    json_object_set_int_member(rule, "player", 4); json_object_set_double_member(rule, "probability", 100);
    json_object_set_string_member(rule, "anchor", "after_start"); json_object_set_double_member(rule, "seconds", 1.2);
    json_object_set_double_member(rule, "duck_db", 0); json_object_set_int_member(rule, "fade_ms", 0);
    json_array_add_object_element(rules, rule); json_object_set_array_member(policy, "jingles", rules);
    json_object_set_object_member(event, "probability_policy", policy);
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", "programme"); json_object_set_member(object, "config", deep_copy(programme));
    JsonNode *prepared = request("/v1/workflows/desktop-playout/prepare", body);
    json_node_free(body); json_node_free(programme); return prepared;
}
static void content_snapshot_expect(RadioSViewContext *ctx, guint deck, const gchar *kind,
    RadioSPlayoutContentState state, gboolean auxiliary, RadioSPlayoutContentState overlay)
{
    RadioSPlayoutDeckContent content;
    g_assert_true(radios_playout_ui_deck_content(ctx, deck, &content));
    g_test_message("Accepted player %u kind '%s', state %d, auxiliary %d, overlay state %d title '%s'",
        deck + 1, content.kind, content.state, content.auxiliary, content.overlay_state, content.overlay_title);
    g_assert_cmpstr(content.kind, ==, kind); g_assert_cmpint(content.state, ==, state);
    g_assert_cmpint(content.auxiliary, ==, auxiliary); g_assert_cmpint(content.overlay_state, ==, overlay);
    if (overlay) {
        g_assert_cmpstr(content.overlay_kind, ==, "sweeper");
        g_assert_cmpstr(content.overlay_title, ==, "Actual accepted sweeper"); g_assert_cmpuint(content.overlay_player, ==, 5);
    }
}
static void test_scoped_overlay_content_hidden_pause_stop_pcm(void)
{
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "content scope definitions");
    ScopeReady ready = {window.ctx->players, 0}; wait_until(scope_workspace_execution_ready, &ready, "content scope history");
    RadioSViewContext *scope = radios_player_workspace_source_context(window.ctx->players, 0); g_assert_nonnull(scope);
    JsonNode *music = content_intro(media_import(&window, "Primary song keeps its title", 700, .2, "music"));
    JsonNode *voice = routing_short_markers(media_import(&window, "Actual accepted sweeper", 2500, .2, "sweeper"), 1500);
    JsonNode *playlist = playlist_create("Accepted sweeper catalogue", voice, NULL), *prepared = content_prepared(music, playlist);
    const guint decks[] = {0, 1}; GError *error = NULL;
    require_call(radios_playout_ui_start_prepared_scope(scope, window.root, json_node_get_object(prepared), decks,
        G_N_ELEMENTS(decks), RADIOS_AUTODJ_TRANSITIONS, &error), &error);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    content_snapshot_expect(scope, 0, "music", RADIOS_PLAYOUT_CONTENT_PLAYING, FALSE, RADIOS_PLAYOUT_CONTENT_READY);
    content_label_wait(&window, 0, "Çalıyor · Müzik · Planlanan üzerine sweeper: Actual accepted sweeper (Oynatıcı 5)");
    /* The visible task and selected card do not own or advance the sequence. */
    navigate(&window, "automation");
    deck_wait(&window, 4, RADIOS_AUDIO_UI_PLAYING, 100);
    RadioSAudioSnapshot actual; radios_audio_engine_snapshot(engine, &actual);
    g_assert_true(g_uuid_string_is_valid(actual.decks[4].binding.entry_id));
    g_assert_true(g_uuid_string_is_valid(actual.decks[4].binding.playlist_id));
    g_assert_cmpstr(actual.decks[4].binding.playlist_id, ==, actual.decks[0].binding.playlist_id);
    content_snapshot_expect(scope, 0, "music", RADIOS_PLAYOUT_CONTENT_PLAYING, FALSE, RADIOS_PLAYOUT_CONTENT_PLAYING);
    content_snapshot_expect(scope, 4, "sweeper", RADIOS_PLAYOUT_CONTENT_PLAYING, TRUE, RADIOS_PLAYOUT_CONTENT_NONE);
    RadioSStreamMetadataSnapshot metadata;
    g_assert_false(radios_stream_metadata_resolve(window.ctx, RG_METADATA_PLAYER, 4, "{title}", &metadata));
    g_assert_cmpstr(metadata.text, ==, "");
    g_assert_true(radios_stream_metadata_resolve(window.ctx, RG_METADATA_PLAYER, 0, "{title}", &metadata));
    g_assert_cmpstr(metadata.text, ==, "Primary song keeps its title");
    content_label_wait(&window, 0, "Çalıyor · Müzik · Üzerine sweeper çalıyor: Actual accepted sweeper (Oynatıcı 5)");
    content_label_wait(&window, 4, "Çalıyor · Üzerine Sweeper");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(window.root, "deck-0-title"))), ==, "Primary song keeps its title");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(window.root, "deck-4-title"))), ==, "Actual accepted sweeper");
    goffset at = capture_offset("MAIN"); run_for(150);
    g_assert_cmpfloat(tone_energy("MAIN", at, 700), >, .06); g_assert_cmpfloat(tone_energy("MAIN", at, 2500), >, .06);
    require_call(radios_playout_ui_action(scope, RADIOS_PLAYOUT_PAUSE, &error), &error);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PAUSED, 0, 0);
    content_snapshot_expect(scope, 0, "music", RADIOS_PLAYOUT_CONTENT_PAUSED, FALSE, RADIOS_PLAYOUT_CONTENT_PAUSED);
    content_label_wait(&window, 0, "Duraklatıldı · Müzik · Üzerine sweeper duraklatıldı: Actual accepted sweeper (Oynatıcı 5)");
    content_label_wait(&window, 4, "Duraklatıldı · Üzerine Sweeper");
    require_call(radios_playout_ui_action(scope, RADIOS_PLAYOUT_STOP, &error), &error); run_for(250);
    RadioSPlayoutDeckContent cleared; g_assert_false(radios_playout_ui_deck_content(scope, 0, &cleared));
    g_assert_cmpstr(cleared.kind, ==, ""); g_assert_cmpint(cleared.overlay_state, ==, RADIOS_PLAYOUT_CONTENT_NONE);
    content_label_wait(&window, 0, ""); content_label_wait(&window, 4, "");
    /* An unrelated later owner cannot inherit the accepted sweeper label. */
    JsonNode *replacement = media_import(&window, "Independent later voice track", 1700, .2, "voice_track");
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(replacement), 4, TRUE, &error), &error);
    deck_wait(&window, 4, RADIOS_AUDIO_UI_PLAYING, 100);
    g_assert_false(radios_playout_ui_deck_content(scope, 4, &cleared));
    content_label_wait(&window, 4, "Çalıyor · Ses kaydı");
    assert_saved_playlist_unchanged(playlist); radios_audio_engine_unref(engine);
    json_node_free(replacement); json_node_free(prepared); json_node_free(playlist); json_node_free(voice); json_node_free(music);
    window_close(&window);
    g_test_message("Accepted sweeper retains kind on primary planned/playing/paused status and its own auxiliary card, keeps primary song title, survives hidden task, clears at Stop and rejects the later foreign generation; real dual-tone MAIN PCM verified.");
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    main_sink = g_getenv("RADIOS_TEST_MAIN_SINK"); cue_sink = g_getenv("RADIOS_TEST_CUE_SINK"); monitor_sink = g_getenv("RADIOS_TEST_MONITOR_SINK");
    g_assert_nonnull(main_sink); g_assert_nonnull(cue_sink); g_assert_nonnull(monitor_sink);
    GError *error = NULL; g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    application = gtk_application_new("org.radios.NativePlayerContentStatus", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/native-playout/player-content-manual-kinds-MAIN-PCM", test_manual_content_kinds_pcm);
    g_test_add_func("/native-playout/player-content-scoped-sweeper-hidden-pause-stop-MAIN-PCM", test_scoped_overlay_content_hidden_pause_stop_pcm);
    gint result = g_test_run(); radios_data_client_unref(reader); g_object_unref(application); radios_config_clear(&config); return result;
}
