/* Genuine HTTP/imported PCM/marker-save/native scoped playback acceptance. */
#define main radios_vocal_jingle_fixture_main
#include "test_playout_ui.c"
#undef main

static JsonNode *vocal_intro_markers(JsonNode *detail)
{
    JsonObject *media=json_object_get_object_member(json_node_get_object(detail),"media");
    JsonNode *body=body_new();JsonObject *object=json_node_get_object(body),*markers=json_object_new();
    json_object_set_string_member(object,"media_id",radios_json_string(media,"_id",""));
    json_object_set_int_member(object,"expected_revision",radios_json_int(media,"_rev",0));json_object_set_null_member(object,"expected_marker_revision");
    gchar *key=g_uuid_string_random();json_object_set_string_member(object,"idempotency_key",key);g_free(key);
    const gchar *names[]={"cue_in_ms","intro_ms","out_ms","fade_start_ms","fade_end_ms","auto_next_ms","overlap_ms"};
    const guint values[]={0,2500,6001,6001,6001,6001,0};
    for (guint i=0;i<G_N_ELEMENTS(names);++i) json_object_set_int_member(markers,names[i],values[i]);
    json_object_set_boolean_member(markers,"automatic",FALSE);json_object_set_object_member(object,"markers",markers);
    JsonNode *saved=request("/v1/workflows/desktop-media/markers-save",body);json_node_free(body);json_node_free(detail);return saved;
}
static void test_vocal_intro_jingle_primary_duck_MAIN_PCM(void)
{
    Window window=window_open();
    wait_until(scope_workspace_ready,window.ctx->players,"actual authenticated player scopes");ScopeReady ready={window.ctx->players,0};
    wait_until(scope_workspace_execution_ready,&ready,"actual private scope history");
    RadioSViewContext *scope=radios_player_workspace_source_context(window.ctx->players,0);g_assert_nonnull(scope);
    JsonNode *music=vocal_intro_markers(media_import(&window,"Verified intro primary",700,.2,"music"));
    JsonNode *voice=routing_short_markers(media_import(&window,"Verified intro voice",2500,.2,"jingle"),800);
    JsonNode *playlist=playlist_create("Private vocal-safe jingle",voice,NULL),*programme=programme_config(music,NULL);
    JsonObject *event=json_array_get_object_element(json_object_get_array_member(json_node_get_object(programme),"events"),0);
    JsonObject *policy=json_object_new(),*rule=json_object_new();JsonArray *rules=json_array_new();
    json_object_set_int_member(policy,"schema",1);gchar *id=g_uuid_string_random();json_object_set_string_member(rule,"id",id);g_free(id);
    json_object_set_string_member(rule,"playlist_id",radios_json_string(json_node_get_object(playlist),"_id",""));
    json_object_set_int_member(rule,"player",4);json_object_set_double_member(rule,"probability",100);
    json_object_set_string_member(rule,"anchor","after_start");json_object_set_double_member(rule,"seconds",.7);
    json_object_set_double_member(rule,"duck_db",-6);json_object_set_int_member(rule,"fade_ms",0);
    json_array_add_object_element(rules,rule);json_object_set_array_member(policy,"jingles",rules);json_object_set_object_member(event,"probability_policy",policy);
    JsonNode *body=body_new();JsonObject *object=json_node_get_object(body);json_object_set_string_member(object,"kind","programme");
    json_object_set_member(object,"config",deep_copy(programme));JsonNode *prepared=request("/v1/workflows/desktop-playout/prepare",body);json_node_free(body);
    RadioSAudioEngine *engine=radios_audio_ui_engine_acquire(window.ctx);const guint decks[]={0,1};GError *error=NULL;
    require_call(radios_playout_ui_start_prepared_scope(scope,window.root,json_node_get_object(prepared),decks,2,RADIOS_AUTODJ_TRANSITIONS,&error),&error);
    player_run_wait(engine,0,RADIOS_AUDIO_RUN_PLAYING,0,4800);
    goffset before=capture_offset("MAIN");run_for(180);gdouble clean=tone_energy("MAIN",before,700);g_assert_cmpfloat(clean,>,.06);
    deck_wait(&window,4,RADIOS_AUDIO_UI_PLAYING,100);
    goffset during=capture_offset("MAIN"),cue_at=capture_offset("CUE");run_for(200);
    gdouble duck=tone_energy("MAIN",during,700);g_assert_cmpfloat(duck/clean,>,.3);g_assert_cmpfloat(duck/clean,<,.75);
    g_assert_cmpfloat(tone_energy("MAIN",during,2500),>,.06);g_assert_cmpfloat(capture("CUE",cue_at).peak[0],<,.0001);
    player_run_wait(engine,0,RADIOS_AUDIO_RUN_PLAYING,0,96000);
    goffset restored=capture_offset("MAIN");run_for(200);g_assert_cmpfloat(tone_energy("MAIN",restored,700)/clean,>,.8);
    RadioSAudioSnapshot snapshot;radios_audio_engine_snapshot(engine,&snapshot);
    g_assert_cmpstr(snapshot.decks[0].media_id,==,radios_json_string(json_object_get_object_member(json_node_get_object(music),"media"),"_id",""));
    require_call(radios_playout_ui_action(scope,RADIOS_PLAYOUT_STOP,&error),&error);run_for(200);
    assert_saved_playlist_unchanged(playlist);radios_audio_engine_unref(engine);json_node_free(prepared);json_node_free(programme);
    json_node_free(playlist);json_node_free(voice);json_node_free(music);window_close(&window);
    g_test_message("Real verified intro admits the whole jingle; MAIN contains primary plus voice with measured -6dB duck, then restores primary; CUE stays silent.");
}
int main(int argc,char **argv)
{
    g_assert_cmpint(argc,>=,2);gchar *root=g_strdup(argv[1]);for (gint i=1;i<argc;++i) argv[i]=argv[i+1];--argc;
    g_test_init(&argc,&argv,NULL);gtk_init(&argc,&argv);main_sink=g_getenv("RADIOS_TEST_MAIN_SINK");cue_sink=g_getenv("RADIOS_TEST_CUE_SINK");monitor_sink=g_getenv("RADIOS_TEST_MONITOR_SINK");
    g_assert_nonnull(main_sink);g_assert_nonnull(cue_sink);g_assert_nonnull(monitor_sink);GError *error=NULL;
    g_assert_true(radios_config_load(root,NULL,&config,&error));g_assert_no_error(error);g_free(root);
    application=gtk_application_new("org.radios.VocalJingleAcceptance",G_APPLICATION_NON_UNIQUE);g_assert_true(g_application_register(G_APPLICATION(application),NULL,&error));g_assert_no_error(error);
    reader=radios_data_client_new(&config,&error);g_assert_no_error(error);g_assert_nonnull(reader);
    g_test_add_func("/native-playout/vocal-intro-whole-jingle-duck-primary-MAIN-PCM",test_vocal_intro_jingle_primary_duck_MAIN_PCM);
    gint result=g_test_run();radios_data_client_unref(reader);g_object_unref(application);radios_config_clear(&config);return result;
}
