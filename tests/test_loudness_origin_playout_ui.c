#define _POSIX_C_SOURCE 200809L
#include <glib.h>
static int loudness_origin_run(void);
#define main radios_loudness_origin_fixture_main
#define g_test_run loudness_origin_run
#define window_open radios_legacy_pcm_window_open
#include "test_playout_ui.c"
#undef g_test_run
#undef main
#undef window_open
#include "radios/player_scope_execution.h"

static gboolean loudness_fixture_cards(gpointer data)
{
    Window *window = data;
    return g_object_get_data(G_OBJECT(window->ctx->parent), "radios-deck-owner-0") &&
        g_object_get_data(G_OBJECT(window->ctx->parent), "radios-deck-owner-1");
}
static Window window_open(void)
{
    Window window = window_open_raw(); GError *error = NULL;
    wait_until(scope_workspace_ready, window.ctx->players, "actual application player topology is ready");
    wait_until(loudness_fixture_cards, &window, "topology reconciliation creates the real source/partner cards");
    /* The player tree creates cards after the definition baseline. Explicit
     * fixture routing uses the same public persistent mixer/MAIN APIs as the
     * channel controls, without depending on hidden or lazy combo widgets. */
    RadioSAudioMixerConfig mixer; g_assert_true(radios_audio_ui_get_mixer(window.ctx, &mixer, NULL, NULL));
    for (guint channel = 0; channel < RADIOS_AUDIO_MIXER_CHANNELS; ++channel)
        if (mixer.channels[channel].source_deck == 0 || mixer.channels[channel].source_deck == 1)
            mixer.channels[channel].source_deck = -1;
    mixer.channels[0].source_deck = 0; mixer.channels[1].source_deck = 1;
    require_call(radios_audio_ui_set_mixer(window.ctx, &mixer, &error), &error);
    require_call(radios_audio_ui_set_deck_main(window.ctx, 0, TRUE, &error), &error);
    require_call(radios_audio_ui_set_deck_main(window.ctx, 1, TRUE, &error), &error);
    run_for(300); return window;
}
static JsonNode *loudness_origin_weekly(void)
{
    JsonObject *cfg = json_object_new(), *calendar = json_object_new();
    json_object_set_int_member(cfg, "schema", 1); json_object_set_string_member(cfg, "week_anchor", "2026-10-05");
    json_object_set_int_member(cfg, "week_count", 1); json_object_set_boolean_member(cfg, "repeat", FALSE);
    json_object_set_int_member(calendar, "schema", 1); json_object_set_array_member(calendar, "events", json_array_new());
    json_object_set_object_member(cfg, "calendar", calendar);
    JsonNode *record = document_save("weekly_programme", "Genuine assigned LUFS origin", cfg); json_object_unref(cfg); return record;
}
static RadioSAudioDeckSnapshot loudness_origin_deck(RadioSAudioEngine *engine, guint64 generation, gdouble gain)
{
    RadioSAudioSnapshot actual; radios_audio_engine_snapshot(engine, &actual);
    g_assert_cmpuint(actual.decks[0].generation, ==, generation); g_assert_true(actual.decks[0].loudness_measured);
    g_assert_cmpfloat(fabs(actual.decks[0].loudness_gain_db - gain), <, .001);
    g_assert_cmpfloat(actual.decks[0].gain_db, ==, 0); return actual.decks[0];
}
static void accepted_assignment_origin_filter_manual_takeover(void)
{
    Window window = window_open(); assert_gates_off(&window); 
    JsonNode *media = media_import_rhythm(&window, "Immutable LUFS assignment source", 700, .25, "music", 120);
    JsonNode *weekly = loudness_origin_weekly(); const gchar *weekly_id = radios_json_string(json_node_get_object(weekly), "_id", "");
    RadioSViewContext context = *window.ctx; context.playout = NULL;
    GtkWidget *owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(owner);
    RadioSPlayerWorkspace *workspace = radios_player_workspace_new(&context, owner); g_assert_nonnull(workspace);
    context.players = workspace; wait_until(scope_workspace_ready, workspace, "real workspace definitions ready for immutable assignment");
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0}; GError *error = NULL;
    RadioSPlayerTopology topology = *radios_player_workspace_topology(workspace);
    require_call(radios_player_topology_set_autodj(&topology, 0, RADIOS_AUTODJ_SEMI, -1, &error), &error);
    require_call(radios_player_workspace_replace(workspace, &topology, &error), &error);
    require_call(radios_player_workspace_assign(workspace, &scope, weekly_id, &error), &error);
    ScopeReady ready = {workspace, 0}; wait_until(scope_workspace_execution_ready, &ready, "assigned topology acknowledged and scoped history ready");
    RadioSAutomationBinding captured;
    require_call(radios_automation_scope_resolve(radios_player_workspace_topology(workspace), &scope, &captured, &error), &error);
    g_assert_cmpstr(captured.automation_id, ==, weekly_id);
    RadioSAudioLoudnessPolicy policy; radios_audio_loudness_policy_default(&policy);
    policy.players = 1; policy.contexts = RADIOS_AUDIO_LOUDNESS_AUTODJ;
    policy.automation_mode = RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_SELECTED; policy.automation_count = 1;
    g_strlcpy(policy.automation_ids[0], weekly_id, sizeof policy.automation_ids[0]);
    require_call(radios_audio_ui_set_loudness(window.ctx, TRUE, -24, &error), &error);
    require_call(radios_audio_ui_set_loudness_policy(window.ctx, &policy, &error), &error);
    RadioSLoudness measured; g_assert_true(radios_loudness_read(json_object_get_object_member(json_node_get_object(media), "analysis"), &measured));
    gdouble expected = radios_loudness_gain_db(&measured, -24); g_assert_cmpfloat(expected, <, -1);
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(media), 0, FALSE, &error), &error);
    RadioSAudioUiSnapshot manual = deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    require_call(radios_audio_ui_play(window.ctx, 0, &error), &error); deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 200);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    loudness_origin_deck(engine, manual.generation, 0); /* Manual stays excluded. */
    require_call(radios_audio_ui_stop(window.ctx, 0, &error), &error);
    JsonNode *prepared = scope_authenticated_programme(media, NULL);
    require_call(radios_player_workspace_start_prepared(workspace, &captured, json_node_get_object(prepared), RADIOS_AUTODJ_SEMI, &error), &error);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    RadioSAudioSnapshot actual; radios_audio_engine_snapshot(engine, &actual); guint64 generation = actual.decks[0].generation;
    RadioSAudioDeckSnapshot accepted = loudness_origin_deck(engine, generation, expected);
    policy.automation_mode = RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_ASSIGNED;
    require_call(radios_audio_ui_set_loudness_policy(window.ctx, &policy, &error), &error);
    loudness_origin_deck(engine, generation, expected); /* Actual accepted assignment. */
    /* A new future assignment cannot rewrite the origin of the owned run. */
    require_call(radios_player_workspace_assign(workspace, &scope, "", &error), &error);
    run_for(100); loudness_origin_deck(engine, generation, expected);
    policy.automation_mode = RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_SELECTED;
    g_strlcpy(policy.automation_ids[0], "12345678-abcd-4ef0-8123-456789abcdef", sizeof policy.automation_ids[0]);
    require_call(radios_audio_ui_set_loudness_policy(window.ctx, &policy, &error), &error);
    loudness_origin_deck(engine, generation, 0); /* Wrong UUID excludes gain only. */
    g_strlcpy(policy.automation_ids[0], weekly_id, sizeof policy.automation_ids[0]);
    require_call(radios_audio_ui_set_loudness_policy(window.ctx, &policy, &error), &error);
    RadioSAudioDeckSnapshot still = loudness_origin_deck(engine, generation, expected);
    g_assert_cmpuint(still.cursor_frames, >=, accepted.cursor_frames); g_assert_cmpint(still.state, ==, RADIOS_AUDIO_PLAYING);
    goffset pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 700), >, .015);
    require_call(radios_player_workspace_action(workspace, 0, RADIOS_PLAYOUT_STOP, &error), &error);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_IDLE, 0, 0);
    /* A new manual generation must not inherit any former runner origin. */
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(media), 0, TRUE, &error), &error);
    manual = deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
    loudness_origin_deck(engine, manual.generation, 0);
    policy.contexts = RADIOS_AUDIO_LOUDNESS_MANUAL; policy.automation_mode = RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_ALL;
    require_call(radios_audio_ui_set_loudness_policy(window.ctx, &policy, &error), &error);
    loudness_origin_deck(engine, manual.generation, expected);
    require_call(radios_audio_ui_stop(window.ctx, 0, &error), &error);
    radios_audio_engine_unref(engine); radios_player_workspace_free(workspace); context.players = NULL;
    gtk_widget_destroy(owner); g_object_unref(owner); json_node_free(prepared); json_node_free(weekly); json_node_free(media); window_close(&window);
    g_test_message("Genuine saved weekly assignment flows through accepted scoped AutoDJ; current assignment edits and selected UUID changes preserve its generation/cursor, and manual takeover clears prior origin.");
}
static void scheduled_prepared_origin_and_manual_start(void)
{
    Window window = window_open(); assert_gates_off(&window); 
    JsonNode *media = media_import_rhythm(&window, "Scheduled LUFS context source", 700, .25, "music", 120);
    JsonNode *prepared = scope_authenticated_programme(media, NULL); GError *error = NULL;
    RadioSAudioLoudnessPolicy policy; radios_audio_loudness_policy_default(&policy);
    policy.contexts = RADIOS_AUDIO_LOUDNESS_AUTOMATION;
    require_call(radios_audio_ui_set_loudness(window.ctx, TRUE, -24, &error), &error);
    require_call(radios_audio_ui_set_loudness_policy(window.ctx, &policy, &error), &error);
    RadioSLoudness measured; g_assert_true(radios_loudness_read(json_object_get_object_member(json_node_get_object(media), "analysis"), &measured));
    gdouble expected = radios_loudness_gain_db(&measured, -24); g_assert_cmpfloat(expected, <, -1);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    require_call(radios_playout_ui_start_prepared(window.ctx, window.root, json_node_get_object(prepared),
        g_get_real_time() + 500000, &error), &error);
    RadioSPlayoutSnapshot scheduled = run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 100);
    g_assert_cmpint(scheduled.deck, ==, 0); RadioSAudioSnapshot actual; radios_audio_engine_snapshot(engine, &actual);
    loudness_origin_deck(engine, actual.decks[0].generation, expected);
    stop_run(&window); run_wait(&window, RADIOS_PLAYOUT_IDLE, 0, 0);
    require_call(radios_playout_ui_start_prepared(window.ctx, window.root, json_node_get_object(prepared), 0, &error), &error);
    run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 100); radios_audio_engine_snapshot(engine, &actual);
    loudness_origin_deck(engine, actual.decks[0].generation, 0);
    stop_run(&window); run_wait(&window, RADIOS_PLAYOUT_IDLE, 0, 0);
    JsonNode *weekly = loudness_origin_weekly(); policy.automation_mode = RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_SELECTED;
    policy.automation_count = 1; const gchar *origin = radios_json_string(json_node_get_object(weekly), "_id", "");
    g_strlcpy(policy.automation_ids[0], origin, sizeof policy.automation_ids[0]);
    require_call(radios_audio_ui_set_loudness_policy(window.ctx, &policy, &error), &error);
    require_call(radios_playout_ui_start_prepared_with_loudness_origin(window.ctx, window.root,
        json_node_get_object(prepared), 0, origin, FALSE, &error), &error);
    run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 100); radios_audio_engine_snapshot(engine, &actual);
    loudness_origin_deck(engine, actual.decks[0].generation, expected);
    stop_run(&window); radios_audio_engine_unref(engine); json_node_free(weekly);
    json_node_free(prepared); json_node_free(media); window_close(&window);
    g_test_message("Genuine generic prepared media uses automation context for a real scheduled deadline, manual for immediate untagged start, and immutable saved origin for an explicit authenticated caller.");
}

typedef struct { guint callbacks, destroyed; GError *error; } LoudnessExecutionDone;
static void loudness_execution_done(RadioSViewContext *ctx, const RadioSAutomationScope *scope,
    guint source, const GError *error, gpointer value)
{
    (void)ctx; LoudnessExecutionDone *done = value; g_assert_cmpint(scope->kind, ==, RADIOS_AUTOMATION_SCOPE_PLAYER);
    g_assert_cmpuint(source, ==, 0); ++done->callbacks; if (error) done->error = g_error_copy(error);
}
static void loudness_execution_destroyed(gpointer value) { ++((LoudnessExecutionDone *)value)->destroyed; }
static gboolean loudness_execution_complete(gpointer value) { return ((LoudnessExecutionDone *)value)->destroyed != 0; }
static void scope_execution_manual_programme_excludes_assigned_schema(void)
{
    Window window = window_open(); assert_gates_off(&window); GError *error = NULL;
    JsonNode *media = media_import(&window, "Scope genuine LUFS schema source", 700, .25, "music");
    JsonNode *playlist = playlist_create("Genuine scoped LUFS schema playlist", media, NULL);
    JsonNode *configuration = weekly_pcm_configuration(playlist, NULL, g_get_real_time(), 3, "Scope actual saved weekly source");
    JsonNode *weekly = document_save("weekly_programme", "Scope LUFS exact schema origin", json_node_get_object(configuration));
    const gchar *origin = radios_json_string(json_node_get_object(weekly), "_id", "");
    RadioSPlayerWorkspace *workspace = window.ctx->players;
    RadioSPlayerTopology topology = *radios_player_workspace_topology(workspace);
    require_call(radios_player_topology_set_autodj(&topology, 0, RADIOS_AUTODJ_SEMI, -1, &error), &error);
    require_call(radios_player_workspace_replace(workspace, &topology, &error), &error);
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0};
    require_call(radios_player_workspace_assign(workspace, &scope, origin, &error), &error);
    ScopeReady ready = {workspace, 0}; wait_until(scope_workspace_execution_ready, &ready, "actual assigned scope baseline is acknowledged");
    RadioSAudioLoudnessPolicy policy; radios_audio_loudness_policy_default(&policy);
    policy.players = 1; policy.contexts = RADIOS_AUDIO_LOUDNESS_AUTOMATION | RADIOS_AUDIO_LOUDNESS_AUTODJ;
    policy.automation_mode = RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_SELECTED; policy.automation_count = 1;
    g_strlcpy(policy.automation_ids[0], origin, sizeof policy.automation_ids[0]);
    require_call(radios_audio_ui_set_loudness(window.ctx, TRUE, -24, &error), &error);
    require_call(radios_audio_ui_set_loudness_policy(window.ctx, &policy, &error), &error);
    RadioSLoudness measured; g_assert_true(radios_loudness_read(json_object_get_object_member(json_node_get_object(media), "analysis"), &measured));
    gdouble expected = radios_loudness_gain_db(&measured, -24); g_assert_cmpfloat(expected, <, -1);
    RadioSPlayerScopeExecution *execution = radios_player_scope_execution_get(window.ctx); g_assert_nonnull(execution);
    JsonNode *manual = programme_config(media, NULL); LoudnessExecutionDone done = {0};
    require_call(radios_player_scope_execution_play_programme(execution, &scope, json_node_get_object(manual),
        RADIOS_AUTODJ_SEMI, loudness_execution_done, &done, loudness_execution_destroyed, &error), &error);
    wait_until(loudness_execution_complete, &done, "ordinary scoped programme has genuine authenticated prepare/accepted run");
    g_assert_no_error(done.error); g_assert_cmpuint(done.callbacks, ==, 1); g_assert_cmpuint(done.destroyed, ==, 1);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); g_assert_nonnull(engine);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    RadioSAudioSnapshot actual; radios_audio_engine_snapshot(engine, &actual);
    loudness_origin_deck(engine, actual.decks[0].generation, 0); /* Assignment is not a claim of this manual queue's source. */
    goffset manual_pcm = capture_offset("MAIN"); run_for(200); gdouble manual_energy = tone_energy("MAIN", manual_pcm, 700);
    g_assert_cmpfloat(manual_energy, >, .08);
    require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_STOP, &error), &error);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_IDLE, 0, 0);
    done = (LoudnessExecutionDone){0};
    require_call(radios_player_scope_execution_play_schema(execution, &scope, RADIOS_AUTODJ_SEMI,
        loudness_execution_done, &done, loudness_execution_destroyed, &error), &error);
    wait_until(loudness_execution_complete, &done, "actual saved weekly schema passes two GET fences and genuine media preparation");
    g_assert_no_error(done.error); g_assert_cmpuint(done.callbacks, ==, 1); g_assert_cmpuint(done.destroyed, ==, 1);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800); radios_audio_engine_snapshot(engine, &actual);
    guint64 generation = actual.decks[0].generation; loudness_origin_deck(engine, generation, expected);
    policy.automation_mode = RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_ASSIGNED;
    require_call(radios_audio_ui_set_loudness_policy(window.ctx, &policy, &error), &error);
    loudness_origin_deck(engine, generation, expected);
    goffset schema_pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", schema_pcm, 700), >, .015);
    g_assert_cmpfloat(tone_energy("MAIN", schema_pcm, 700), <, manual_energy * .6);
    require_call(radios_player_scope_execution_action(execution, &scope, RADIOS_PLAYOUT_STOP, &error), &error);
    player_run_wait(engine, 0, RADIOS_AUDIO_RUN_IDLE, 0, 0); weekly_record_unchanged(weekly); assert_saved_playlist_unchanged(playlist);
    radios_audio_engine_unref(engine); json_node_free(manual); json_node_free(weekly); json_node_free(configuration);
    json_node_free(playlist); json_node_free(media); window_close(&window);
    g_test_message("Actual ScopeExecution ordinary programme in a schema-assigned player has no schema origin; actual freshly fetched weekly schema matches selected UUID and ASSIGNED filters, with measured MAIN gain and saved sources unchanged.");
}

static int loudness_origin_run(void)
{
    g_test_add_func("/native-loudness-origin/assigned-scope-immutable-filter-manual-takeover-MAIN-PCM", accepted_assignment_origin_filter_manual_takeover);
    g_test_add_func("/native-loudness-origin/scheduled-prepared-manual-and-explicit-origin", scheduled_prepared_origin_and_manual_start);
    g_test_add_func("/native-loudness-origin/scope-execution-manual-versus-actual-schema-MAIN-PCM", scope_execution_manual_programme_excludes_assigned_schema);
    return g_test_run();
}
int main(int argc, char **argv) { return radios_loudness_origin_fixture_main(argc, argv); }
