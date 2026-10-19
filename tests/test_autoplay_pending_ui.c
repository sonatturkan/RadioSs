#define main radios_pending_legacy_fixture_main
#include "test_playout_ui.c"
#undef main
#include <stdatomic.h>
static _Atomic gint64 pending_clock_offset;
gint64 radios_test_execution_clock_now(void)
{ return g_get_monotonic_time() + atomic_load(&pending_clock_offset); }
static void test_pending_HTTP_timeout_releases_scope_and_fences_late_reply(void)
{
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "scope definitions ready");
    ScopeReady ready = {window.ctx->players, 0};
    wait_until(scope_workspace_execution_ready, &ready, "private history ready before timed request");
    JsonNode *music = media_import(&window, "Pending real primary", 700, .2, "music");
    JsonNode *foreign = media_import(&window, "Pending preserved foreign", 1700, .2, "music");
    JsonNode *programme = programme_config(music, NULL);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); GError *error = NULL;
    scope_legacy_engine_start(engine, 4, foreign);
    RadioSAudioRunSnapshot other; radios_audio_engine_player_runner_snapshot(engine, 4, &other);
    RadioSAudioSnapshot before; radios_audio_engine_snapshot(engine, &before);
    gchar *hold = fixture_flag(".acceptance-playout-prepare-hold");
    gchar *held = fixture_flag(".acceptance-playout-prepare-ready");
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL));
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(window.ctx);
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0};
    ScopeExecutionResult pending = {0};
    require_call(radios_player_scope_execution_play_programme(execution, &scope,
        json_node_get_object(programme), RADIOS_AUTODJ_SEMI,
        execution_result, &pending, execution_result_destroy, &error), &error);
    wait_until(file_exists, held, "HTTP response genuinely pending before execution deadline");
    RadioSPlayerScopeExecutionSnapshot state;
    g_assert_true(radios_player_scope_execution_snapshot(execution, &scope, &state));
    g_assert_true(state.preparing);
    /* Advance only the production facade's deadline clock. Audio, GTK and
     * the real HTTP server keep their genuine clocks and captured PCM. */
    atomic_store(&pending_clock_offset, 31 * G_USEC_PER_SEC);
    wait_until(execution_completed, &pending, "pending HTTP cannot bypass the startup deadline");
    g_assert_cmpuint(pending.callbacks, ==, 1); g_assert_cmpuint(pending.destroyed, ==, 1);
    g_assert_error(pending.error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_assert_true(radios_player_scope_execution_snapshot(execution, &scope, &state));
    g_assert_cmpint(state.state, ==, RADIOS_SCOPE_EXECUTION_ERROR); g_assert_false(state.preparing);
    g_assert_false(radios_player_scope_execution_busy(execution, &scope));
    atomic_store(&pending_clock_offset, 0);
    g_assert_cmpint(g_unlink(hold), ==, 0); g_assert_cmpint(g_unlink(held), ==, 0);
    run_for(400); g_assert_cmpuint(pending.callbacks, ==, 1);
    RadioSAudioSnapshot after; radios_audio_engine_snapshot(engine, &after);
    g_assert_cmpuint(after.decks[0].generation, ==, before.decks[0].generation);
    scope_assert_live_pcm_unchanged(&window, engine, 4, before.decks[4].generation, other.epoch, 1700);
    ScopeExecutionResult retry = {0};
    require_call(radios_player_scope_execution_play_programme(execution, &scope,
        json_node_get_object(programme), RADIOS_AUTODJ_SEMI,
        execution_result, &retry, execution_result_destroy, &error), &error);
    execution_wait(&retry); player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_STOP, &error), &error);
    require_call(radios_audio_engine_player_runner_action(engine, 4, RADIOS_AUDIO_PLAYER_RUN_STOP, &error), &error);
    g_clear_error(&pending.error); g_free(held); g_free(hold); radios_audio_engine_unref(engine);
    json_node_free(programme); json_node_free(foreign); json_node_free(music); window_close(&window);
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
    application = gtk_application_new("org.radios.NativePendingAcceptance", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/native-playout/pending-deadline-late-reply-retry-foreign-PCM", test_pending_HTTP_timeout_releases_scope_and_fences_late_reply);
    gint result = g_test_run(); radios_data_client_unref(reader); g_object_unref(application); radios_config_clear(&config); return result;
}
