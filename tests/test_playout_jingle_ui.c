/* Genuine authenticated private-store/native-window/MAIN PCM fixtures. Keep
 * the shared broad acceptance source unchanged while adding focused coverage. */
#define main radios_jingle_legacy_fixture_main
#include "test_playout_ui.c"
#undef main

/* These generated sine sources contain no vocals. Persist their verified
 * two-second intro through the real authenticated API so the .7s + .8s
 * overlay fits the same known intro window production planning requires. */
static JsonNode *jingle_music_intro(JsonNode *detail, guint duration)
{
    JsonObject *media = json_object_get_object_member(json_node_get_object(detail), "media");
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body), *markers = json_object_new();
    json_object_set_string_member(object, "media_id", radios_json_string(media, "_id", ""));
    json_object_set_int_member(object, "expected_revision", radios_json_int(media, "_rev", 0));
    json_object_set_null_member(object, "expected_marker_revision");
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    const gchar *names[] = {"cue_in_ms", "intro_ms", "out_ms", "fade_start_ms", "fade_end_ms", "auto_next_ms", "overlap_ms"};
    const guint values[] = {0, 2000, duration, duration, duration, duration, 0};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) json_object_set_int_member(markers, names[i], values[i]);
    json_object_set_boolean_member(markers, "automatic", FALSE); json_object_set_object_member(object, "markers", markers);
    JsonNode *saved = request("/v1/workflows/desktop-media/markers-save", body); json_node_free(body); json_node_free(detail);
    g_assert_cmpint(radios_json_int(json_object_get_object_member(json_node_get_object(saved), "markers"), "intro_ms", 0), ==, 2000);
    return saved;
}
static JsonNode *jingle_prepared(JsonNode *music, JsonNode *playlist)
{
    JsonNode *programme = programme_config(music, NULL);
    JsonObject *event = json_array_get_object_element(json_object_get_array_member(json_node_get_object(programme), "events"), 0);
    JsonObject *policy = json_object_new(), *rule = json_object_new(); JsonArray *rules = json_array_new();
    json_object_set_int_member(policy, "schema", 1); json_object_set_string_member(policy, "seed", "actual-jingle-route");
    gchar *id = g_uuid_string_random(); json_object_set_string_member(rule, "id", id); g_free(id);
    json_object_set_string_member(rule, "playlist_id", radios_json_string(json_node_get_object(playlist), "_id", ""));
    json_object_set_int_member(rule, "player", 4); json_object_set_double_member(rule, "probability", 100);
    json_object_set_string_member(rule, "anchor", "after_start"); json_object_set_double_member(rule, "seconds", .7);
    json_object_set_double_member(rule, "duck_db", 0); json_object_set_int_member(rule, "fade_ms", 0);
    json_array_add_object_element(rules, rule); json_object_set_array_member(policy, "jingles", rules);
    json_object_set_object_member(event, "probability_policy", policy);
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", "programme"); json_object_set_member(object, "config", deep_copy(programme));
    JsonNode *prepared = request("/v1/workflows/desktop-playout/prepare", body);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(prepared), "jingle_sources")), ==, 1);
    json_node_free(body); json_node_free(programme); return prepared;
}
static RadioSViewContext *jingle_scope(Window *window)
{
    wait_until(scope_workspace_ready, window->ctx->players, "authenticated cluster/player definitions");
    ScopeReady ready = {window->ctx->players, 0}; wait_until(scope_workspace_execution_ready, &ready, "scope history baseline");
    RadioSViewContext *scope = radios_player_workspace_source_context(window->ctx->players, 0); g_assert_nonnull(scope); return scope;
}
static void jingle_start(RadioSViewContext *scope, GtkWidget *owner, JsonNode *prepared)
{
    const guint decks[] = {0, 1}; GError *error = NULL;
    require_call(radios_playout_ui_start_prepared_scope(scope, owner, json_node_get_object(prepared), decks, G_N_ELEMENTS(decks), RADIOS_AUTODJ_TRANSITIONS, &error), &error);
}
static void jingle_stop(RadioSViewContext *scope)
{
    GError *error = NULL; require_call(radios_playout_ui_action(scope, RADIOS_PLAYOUT_STOP, &error), &error); run_for(250);
}
static void assert_accepted_jingle_hint(RadioSViewContext *ctx, guint index, const gchar *title, gboolean continued)
{
    JsonNode *accepted = radios_playout_ui_plan_snapshot(ctx); g_assert_nonnull(accepted);
    JsonArray *entries = json_object_get_array_member(json_node_get_object(accepted), "entries");
    g_assert_cmpuint(json_array_get_length(entries), >, index);
    JsonObject *row = json_array_get_object_element(entries, index);
    if (!json_object_has_member(row, "planned_jingles")) {
        gchar *diagnostic = json_to_string(accepted, FALSE);
        g_test_message("Accepted jingle hint absent at index %u: %s", index, diagnostic); g_free(diagnostic);
    }
    g_assert_true(json_object_has_member(row, "planned_jingles"));
    JsonArray *hints = json_object_get_array_member(row, "planned_jingles");
    g_assert_cmpuint(json_array_get_length(hints), ==, 1);
    JsonObject *hint = json_array_get_object_element(hints, 0);
    g_assert_cmpstr(radios_json_string(hint, "title", ""), ==, title);
    g_assert_cmpstr(radios_json_string(hint, "placement", ""), ==, "overlay");
    g_assert_cmpint(radios_json_int(hint, "player", 0), ==, 5);
    g_assert_cmpint(radios_json_int(hint, "start_ms", -1), ==, 700);
    if (continued) {
        g_assert_cmpuint(index, >, 0);
        g_assert_cmpstr(radios_json_string(row, "block_id", ""), ==,
            radios_json_string(json_array_get_object_element(entries, index - 1), "block_id", ""));
        g_assert_cmpstr(radios_json_string(row, "block_id", ""), !=, "");
    }
    json_object_set_string_member(hint, "title", "Later view must not mutate accepted plan");
    json_node_free(accepted);
    accepted = radios_playout_ui_plan_snapshot(ctx); g_assert_nonnull(accepted);
    entries = json_object_get_array_member(json_node_get_object(accepted), "entries");
    row = json_array_get_object_element(entries, index);
    hint = json_array_get_object_element(json_object_get_array_member(row, "planned_jingles"), 0);
    g_assert_cmpstr(radios_json_string(hint, "title", ""), ==, title); json_node_free(accepted);
}
static void test_empty_unrouted_jingle_connects_MAIN_pcm(void)
{
    Window window = window_open(); RadioSViewContext *scope = jingle_scope(&window);
    JsonNode *music = jingle_music_intro(media_import(&window, "Automatic playlist music", 700, .2, "music"), 6001);
    JsonNode *voice = routing_short_markers(media_import(&window, "Automatic playlist jingle", 2500, .2, "jingle"), 800);
    JsonNode *playlist = playlist_create("Actual optional jingle source", voice, NULL), *prepared = jingle_prepared(music, playlist);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); RadioSAudioSnapshot before;
    radios_audio_engine_snapshot(engine, &before); RadioSAudioMixerConfig mixer = before.mixer; GError *error = NULL;
    for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel)
        if (mixer.channels[channel].source_deck == 4) mixer.channels[channel].source_deck = -1;
    require_call(radios_audio_ui_set_mixer(window.ctx, &mixer, &error), &error);
    require_call(radios_audio_ui_set_deck_main(window.ctx, 4, FALSE, &error), &error);
    radios_audio_engine_snapshot(engine, &before); g_assert_cmpint(before.decks[4].state, ==, RADIOS_AUDIO_EMPTY); g_assert_false(before.decks[4].main_routed);
    jingle_start(scope, window.root, prepared); player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    deck_wait(&window, 4, RADIOS_AUDIO_UI_PLAYING, 100);
    assert_accepted_jingle_hint(scope, 0, "Automatic playlist jingle", FALSE);
    RadioSAudioSnapshot after; radios_audio_engine_snapshot(engine, &after); g_assert_true(after.decks[4].main_routed);
    guint changed = 0;
    for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel) {
        if (before.mixer.channels[channel].source_deck != after.mixer.channels[channel].source_deck) {
            g_assert_cmpint(before.mixer.channels[channel].source_deck, ==, -1);
            g_assert_cmpint(after.mixer.channels[channel].source_deck, ==, 4); ++changed;
        }
        g_assert_cmpint(before.mixer.channels[channel].pfl, ==, after.mixer.channels[channel].pfl);
    }
    g_assert_cmpuint(changed, ==, 1);
    gint owner = -2; g_assert_true(radios_audio_engine_deck_runner_owner(engine, 4, &owner)); g_assert_cmpint(owner, ==, 0);
    goffset main_at = capture_offset("MAIN"), cue_at = capture_offset("CUE"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", main_at, 700), >, .06); g_assert_cmpfloat(tone_energy("MAIN", main_at, 2500), >, .06);
    g_assert_cmpfloat(capture("CUE", cue_at).peak[0], <, .0001);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 96000); jingle_stop(scope);
    assert_saved_playlist_unchanged(playlist); radios_audio_engine_unref(engine); json_node_free(prepared); json_node_free(playlist); json_node_free(voice); json_node_free(music); window_close(&window);
    g_test_message("Actual scoped AutoDJ maps its explicit empty jingle player to a free MAIN strip; verified 700Hz music and 2500Hz jingle both reach MAIN without CUE.");
}
static void test_unavailable_jingle_preserves_main_and_explicit_player(void)
{
    Window window = window_open(); RadioSViewContext *scope = jingle_scope(&window);
    JsonNode *music = jingle_music_intro(media_import(&window, "Continuing automatic playlist", 700, .2, "music"), 6001);
    JsonNode *voice = routing_short_markers(media_import(&window, "Skipped optional jingle", 2500, .2, "jingle"), 800);
    JsonNode *manual = media_import(&window, "Preserved manual auxiliary", 1700, .2, "music");
    JsonNode *playlist = playlist_create("Unavailable explicit auxiliary source", voice, NULL), *prepared = jingle_prepared(music, playlist);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); GError *error = NULL;
    for (guint condition = 0; condition < 3; ++condition) {
        if (condition == 0) {
            require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(manual), 4, FALSE, &error), &error);
            deck_wait(&window, 4, RADIOS_AUDIO_UI_READY, 0);
        } else if (condition == 1) {
            require_call(radios_audio_engine_unload(engine, 4, &error), &error);
            RadioSAudioMixerConfig mixer; radios_audio_engine_get_mixer(engine, &mixer);
            for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel)
                if (mixer.channels[channel].source_deck == 4) mixer.channels[channel].pfl = TRUE;
            require_call(radios_audio_ui_set_mixer(window.ctx, &mixer, &error), &error);
        } else {
            RadioSAudioMixerConfig mixer; radios_audio_engine_get_mixer(engine, &mixer);
            for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel)
                if (mixer.channels[channel].source_deck == 4) mixer.channels[channel].pfl = FALSE;
            require_call(radios_audio_ui_set_mixer(window.ctx, &mixer, &error), &error); scope_legacy_engine_start(engine, 4, manual);
        }
        RadioSAudioSnapshot before; radios_audio_engine_snapshot(engine, &before);
        RadioSAudioRunSnapshot foreign = {0}; if (condition == 2) radios_audio_engine_player_runner_snapshot(engine, 4, &foreign);
        jingle_start(scope, window.root, prepared); player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 48000);
        JsonNode *accepted = radios_playout_ui_plan_snapshot(scope); g_assert_nonnull(accepted);
        JsonObject *shown = json_array_get_object_element(json_object_get_array_member(json_node_get_object(accepted), "entries"), 0);
        g_assert_false(json_object_has_member(shown, "planned_jingles")); json_node_free(accepted);
        goffset main_at = capture_offset("MAIN"); run_for(250);
        g_assert_cmpfloat(tone_energy("MAIN", main_at, 700), >, .06); g_assert_cmpfloat(tone_energy("MAIN", main_at, 2500), <, .01);
        RadioSAudioSnapshot after; radios_audio_engine_snapshot(engine, &after);
        g_assert_cmpuint(after.decks[4].generation, ==, before.decks[4].generation);
        g_assert_cmpstr(after.decks[4].media_id, ==, before.decks[4].media_id);
        g_assert_cmpint(after.decks[4].state, ==, before.decks[4].state);
        for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel) {
            g_assert_cmpint(after.mixer.channels[channel].source_deck, ==, before.mixer.channels[channel].source_deck);
            g_assert_cmpint(after.mixer.channels[channel].pfl, ==, before.mixer.channels[channel].pfl);
        }
        if (condition == 2) {
            RadioSAudioRunSnapshot current; radios_audio_engine_player_runner_snapshot(engine, 4, &current);
            g_assert_cmpuint(current.epoch, ==, foreign.epoch); g_assert_cmpint(current.state, ==, RADIOS_AUDIO_RUN_PLAYING);
            g_assert_cmpuint(after.decks[4].cursor_frames, >, before.decks[4].cursor_frames);
            g_assert_cmpfloat(tone_energy("MAIN", main_at, 1700), >, .06);
        }
        jingle_stop(scope);
        if (condition == 2) {
            gint owner = -2; g_assert_true(radios_audio_engine_deck_runner_owner(engine, 4, &owner)); g_assert_cmpint(owner, ==, 4);
            require_call(radios_audio_engine_player_runner_action(engine, 4, RADIOS_AUDIO_PLAYER_RUN_STOP, &error), &error);
        }
    }
    assert_saved_playlist_unchanged(playlist); radios_audio_engine_unref(engine); json_node_free(prepared); json_node_free(playlist); json_node_free(manual); json_node_free(voice); json_node_free(music); window_close(&window);
    g_test_message("Occupied READY, empty PFL/CUE and foreign running auxiliary are preserved exactly; optional jingle omission leaves actual automatic playlist MAIN PCM playing.");
}
static void test_late_append_jingle_connects_MAIN_pcm(void)
{
    Window window = window_open();
    JsonNode *music = jingle_music_intro(media_import(&window, "Continuous automatic playlist", 700, .2, "music"), 2200);
    JsonNode *voice = routing_short_markers(media_import(&window, "Later appended jingle", 2500, .2, "jingle"), 800);
    JsonNode *music_list = playlist_create("Continuous actual music source", music, NULL);
    JsonNode *voice_list = playlist_create("Later actual optional jingle source", voice, NULL), *sample = jingle_prepared(music, voice_list);
    JsonNode *configuration = automation_config(music_list, music_list, voice_list); JsonObject *settings = json_node_get_object(configuration);
    json_object_set_boolean_member(settings, "jingleEnabled", FALSE); json_object_set_null_member(settings, "jinglePlaylist");
    json_object_set_int_member(settings, "previewCount", 1);
    json_array_remove_element(json_object_get_array_member(settings, "sources"), 1);
    JsonObject *row = json_array_get_object_element(json_object_get_array_member(json_node_get_object(sample), "tracks"), 0);
    g_assert_true(json_object_has_member(row, "probability_policy"));
    JsonNode *policy = deep_copy(json_object_get_member(row, "probability_policy"));
    g_assert_nonnull(policy); g_assert_true(JSON_NODE_HOLDS_OBJECT(policy));
    g_assert_true(json_object_has_member(json_node_get_object(policy), "jingles"));
    JsonObject *rule = json_array_get_object_element(json_object_get_array_member(json_node_get_object(policy), "jingles"), 0);
    g_test_message("Later-append fixture adopted authenticated jingle policy; frequency two next batch will carry voice.");
    json_object_set_int_member(rule, "frequency", 2); json_object_set_member(settings, "probability_policy", policy);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); RadioSAudioMixerConfig mixer;
    radios_audio_engine_get_mixer(engine, &mixer); GError *error = NULL;
    for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel)
        if (mixer.channels[channel].source_deck == 4) mixer.channels[channel].source_deck = -1;
    require_call(radios_audio_ui_set_mixer(window.ctx, &mixer, &error), &error);
    require_call(radios_audio_ui_set_deck_main(window.ctx, 4, FALSE, &error), &error);
    require_call(radios_playout_ui_start_automation(window.ctx, window.root, settings, &error), &error);
    run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 100);
    /* Frequency two omits the voice from the first one-song batch. The
     * asynchronous append may already own its next voice during prefetch. */
    gint owner = -2;
    run_wait(&window, RADIOS_PLAYOUT_PLAYING, 1, 0); deck_wait(&window, 4, RADIOS_AUDIO_UI_PLAYING, 100);
    g_test_message("Later-append primary and auxiliary are playing; checking immutable accepted hint.");
    assert_accepted_jingle_hint(window.ctx, 1, "Later appended jingle", TRUE);
    goffset main_at = capture_offset("MAIN"), cue_at = capture_offset("CUE"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", main_at, 700), >, .06); g_assert_cmpfloat(tone_energy("MAIN", main_at, 2500), >, .06);
    g_assert_cmpfloat(capture("CUE", cue_at).peak[0], <, .0001);
    g_assert_true(radios_audio_engine_deck_runner_owner(engine, 4, &owner)); g_assert_cmpint(owner, ==, -1);
    stop_run(&window); radios_audio_engine_unref(engine); json_node_free(configuration); json_node_free(sample); json_node_free(voice_list); json_node_free(music_list); json_node_free(voice); json_node_free(music); window_close(&window);
    g_test_message("A later probability-selected jingle first appears in the direct automatic append path and reaches MAIN on its explicit newly connected auxiliary.");
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]); for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1]; --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    main_sink = g_getenv("RADIOS_TEST_MAIN_SINK"); cue_sink = g_getenv("RADIOS_TEST_CUE_SINK"); monitor_sink = g_getenv("RADIOS_TEST_MONITOR_SINK");
    g_assert_nonnull(main_sink); g_assert_nonnull(cue_sink); g_assert_nonnull(monitor_sink);
    GError *error = NULL; g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    application = gtk_application_new("org.radios.NativeJingleAcceptance", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/native-playout/jingle-empty-unrouted-MAIN-PCM", test_empty_unrouted_jingle_connects_MAIN_pcm);
    g_test_add_func("/native-playout/jingle-later-append-unrouted-MAIN-PCM", test_late_append_jingle_connects_MAIN_pcm);
    g_test_add_func("/native-playout/jingle-unavailable-preserves-primary-and-explicit-player-PCM", test_unavailable_jingle_preserves_main_and_explicit_player);
    gint result = g_test_run(); radios_data_client_unref(reader); g_object_unref(application); radios_config_clear(&config); return result;
}
