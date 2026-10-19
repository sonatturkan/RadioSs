/* Reuse the verified-media/calendar fixture builders, not their test run. */
#define main radios_legacy_playout_fixture_main
#include "test_playout.c"
#undef main
#include "radios/automation_policy.h"
static JsonObject *new_policy(void)
{ JsonObject *p=json_object_new();json_object_set_int_member(p,"schema",1);json_object_set_string_member(p,"seed","immutable-policy-seed");return p; }
static JsonObject *rule(guint id,guint mode,guint overlap,gdouble early)
{
    JsonObject *r=json_object_new();gchar *uid=uuid(7000+id);json_object_set_string_member(r,"id",uid);g_free(uid);
    json_object_set_int_member(r,"mode",mode);json_object_set_int_member(r,"overlap_ms",overlap);json_object_set_double_member(r,"early_seconds",early);return r;
}
static JsonObject *policy_programme(guint count,JsonObject *policy)
{
    JsonObject *p=prepared("programme");JsonArray *tracks=json_object_get_array_member(p,"tracks");
    for (guint i=0;i<count;++i) {
        JsonObject *song=row(i+1,"Sanatçı");JsonNode *n=song_copy(policy);json_object_set_member(song,"probability_policy",n);json_array_add_object_element(tracks,song);
    }
    return p;
}
static void approved_voice_cue(JsonObject *song,const gchar *kind,guint position)
{
    JsonObject *detail=json_object_get_object_member(song,"detail"),*version=json_object_get_object_member(detail,"version");
    JsonArray *cues=json_object_has_member(detail,"radio_cues")?json_object_get_array_member(detail,"radio_cues"):NULL;
    if (!cues) { cues=json_array_new();json_object_set_array_member(detail,"radio_cues",cues); }
    JsonObject *cue=json_object_new();gchar *id=uuid(9500+json_array_get_length(cues));
    json_object_set_string_member(cue,"_id",id);g_free(id);json_object_set_int_member(cue,"_rev",1);
    json_object_set_string_member(cue,"station_id",station);json_object_set_string_member(cue,"version_id",json_object_get_string_member(version,"_id"));
    json_object_set_string_member(cue,"media_file_id",json_object_get_string_member(version,"media_file_id"));
    json_object_set_string_member(cue,"media_sha256",json_object_get_string_member(version,"sha256"));
    json_object_set_int_member(cue,"media_duration_ms",json_object_get_int_member(version,"duration_ms"));
    json_object_set_string_member(cue,"status","approved");json_object_set_string_member(cue,"cue_kind",kind);
    json_object_set_int_member(cue,"position_ms",position);json_array_add_object_element(cues,cue);
}
static void early_effect_real_timeline(void)
{
    JsonObject *policy=new_policy();JsonArray *rules=json_array_new();JsonObject *r=rule(1,1,1000,2);
    json_object_set_double_member(r,"probability",100);json_object_set_string_member(r,"effect","lowpass");json_array_add_object_element(rules,r);json_object_set_array_member(policy,"transitions",rules);
    JsonObject *p=policy_programme(2,policy);RadioSPlayoutRequest req=request();RadioSPlayoutPlan *plan=build(p,&req);
    g_assert_cmpuint(entry(plan,0)->run.autodj_out_frame,==,192031);g_assert_cmpuint(entry(plan,0)->run.media.cue_out_frame,==,288031);
    g_assert_cmpuint(entry(plan,0)->run.overlap_frames,==,48000);g_assert_cmpint(entry(plan,0)->run.effect,==,RADIOS_AUDIO_RUN_EFFECT_LOWPASS);
    g_assert_cmpint(entry(plan,1)->planned_utc_us-entry(plan,0)->planned_utc_us,==,(gint64)192031*1000000/48000);
    req.single_player=TRUE;RadioSPlayoutPlan *single=build(p,&req);g_assert_cmpint(entry(single,0)->run.mode,==,RADIOS_AUDIO_RUN_EOF);
    g_assert_cmpuint(entry(single,0)->run.autodj_out_frame,==,192031);g_assert_cmpuint(entry(single,0)->run.overlap_frames,==,0);
    radios_playout_plan_free(single);radios_playout_plan_free(plan);json_object_unref(p);json_object_unref(policy);
}
static void jingle_is_verified_adjunct_checkpoint_owned(void)
{
    JsonObject *policy=new_policy();JsonArray *rules=json_array_new();JsonObject *r=json_object_new();gchar *playlist=uuid(8800),*rid=uuid(8801);
    json_object_set_string_member(r,"id",rid);json_object_set_string_member(r,"playlist_id",playlist);json_object_set_int_member(r,"player",2);
    json_object_set_string_member(r,"anchor","before_end");json_object_set_double_member(r,"seconds",2);json_object_set_double_member(r,"probability",100);json_array_add_object_element(rules,r);json_object_set_array_member(policy,"jingles",rules);
    JsonObject *p=policy_programme(2,policy),*voice=row(99,"Jingle sanatçı");
    json_object_set_int_member(json_object_get_object_member(json_object_get_object_member(voice,"detail"),"version"),"duration_ms",1000);
    json_object_set_int_member(json_object_get_object_member(json_object_get_object_member(voice,"detail"),"analysis"),"sample_frames",48000);
    JsonArray *catalog=json_array_new(),*tracks=json_array_new();JsonObject *source=json_object_new();json_array_add_object_element(tracks,voice);
    json_object_set_string_member(source,"playlist_id",playlist);json_object_set_array_member(source,"tracks",tracks);json_array_add_object_element(catalog,source);json_object_set_array_member(p,"jingle_sources",catalog);
    for (guint i=0;i<2;++i) approved_voice_cue(json_array_get_object_element(json_object_get_array_member(p,"tracks"),i),"outro_start",3000);
    JsonObject *selection=new_policy();json_object_set_string_member(selection,"rng","1234567890abcdef");
    json_object_set_object_member(json_array_get_object_element(json_object_get_array_member(p,"tracks"),0),"policy_selection_checkpoint",selection);
    RadioSPlayoutRequest req=request();RadioSPlayoutPlan *plan=build(p,&req);g_assert_cmpuint(plan->entries->len,==,2);
    RadioSPlayoutEntry *first=entry(plan,0);g_assert_true(first->run.adjunct.enabled);g_assert_cmpuint(first->run.adjunct.player,==,2);
    g_assert_cmpuint(first->run.adjunct.start_output_frame,==,192031);g_assert_cmpuint(first->run.adjunct.media.cue_out_frame,==,48000);
    g_assert_cmpstr(first->run.adjunct.entry_id,==,json_object_get_string_member(voice,"entry_id"));
    JsonObject *shown=json_array_get_object_element(json_object_get_array_member(json_node_get_object(plan->display),"entries"),0);
    g_assert_true(json_object_has_member(shown,"probability_checkpoint"));g_assert_true(json_object_has_member(shown,"policy_selection_checkpoint"));
    JsonArray *hints=json_object_get_array_member(shown,"planned_jingles");g_assert_cmpuint(json_array_get_length(hints),==,1);
    JsonObject *hint=json_array_get_object_element(hints,0);
    g_assert_cmpstr(json_object_get_string_member(hint,"title"),==,first->run.adjunct.title);
    g_assert_cmpstr(json_object_get_string_member(hint,"placement"),==,"overlay");
    g_assert_cmpstr(json_object_get_string_member(hint,"anchor"),==,"before_end");
    g_assert_cmpint(json_object_get_int_member(hint,"player"),==,3);
    g_assert_cmpint(json_object_get_int_member(hint,"start_ms"),==,4000);
    g_assert_cmpint(json_object_get_int_member(hint,"duration_ms"),==,1000);
    /* Adopting after the planned overlay removes its old display annotation. */
    req.now_utc_us+=5*G_USEC_PER_SEC;GError *error=NULL;
    g_assert_true(radios_playout_plan_adopt_source(plan,240000,&req,&error));g_assert_no_error(error);
    g_assert_false(first->run.adjunct.enabled);g_assert_false(json_object_has_member(shown,"planned_jingles"));
    json_object_unref(p);json_object_unref(policy);g_free(playlist);g_free(rid);
    g_assert_true(g_path_is_absolute(first->run.adjunct.media.path));g_assert_cmpstr(first->run.adjunct.artist,==,"Jingle sanatçı");radios_playout_plan_free(plan);
}
static void transition_state_continuation_not_selection_rewind(void)
{
    JsonObject *policy=new_policy();JsonArray *rules=json_array_new();json_array_add_object_element(rules,rule(2,0,0,0));json_array_add_object_element(rules,rule(3,2,0,0));json_object_set_array_member(policy,"transitions",rules);
    JsonObject *p=policy_programme(3,policy);RadioSPlayoutRequest req=request();RadioSPlayoutPlan *plan=build(p,&req);
    g_assert_cmpint(entry(plan,0)->run.mode,==,RADIOS_AUDIO_RUN_EOF);g_assert_cmpint(entry(plan,1)->run.mode,==,RADIOS_AUDIO_RUN_CUE);g_assert_cmpint(entry(plan,2)->run.mode,==,RADIOS_AUDIO_RUN_EOF);
    JsonObject *shown=json_array_get_object_element(json_object_get_array_member(json_node_get_object(plan->display),"entries"),0);
    JsonObject *checkpoint=json_object_get_object_member(shown,"probability_checkpoint");
    JsonObject *refill=policy_programme(2,policy);json_object_set_member(refill,"probability_checkpoint",song_copy(checkpoint));
    /* Selection state belongs to another domain and cannot rewind transitions. */
    for (guint i=0;i<2;++i) json_object_set_member(json_array_get_object_element(json_object_get_array_member(refill,"tracks"),i),"policy_selection_checkpoint",song_copy(checkpoint));
    RadioSPlayoutPlan *next=build(refill,&req);g_assert_cmpint(entry(next,0)->run.mode,==,RADIOS_AUDIO_RUN_CUE);g_assert_cmpint(entry(next,1)->run.mode,==,RADIOS_AUDIO_RUN_EOF);
    radios_playout_plan_free(next);radios_playout_plan_free(plan);json_object_unref(refill);json_object_unref(p);json_object_unref(policy);
}
static void invalid_policy_cannot_bypass_prepare(void)
{
    JsonObject *policy=new_policy();JsonArray *rules=json_array_new();JsonObject *a=rule(4,0,0,0),*b=rule(5,0,0,0);
    json_object_set_double_member(a,"probability",60);json_object_set_double_member(b,"probability",60);json_array_add_object_element(rules,a);json_array_add_object_element(rules,b);json_object_set_array_member(policy,"transitions",rules);
    JsonObject *p=policy_programme(2,policy);RadioSPlayoutRequest req=request();rejected(p,&req);json_object_unref(p);json_object_unref(policy);
}
static JsonObject *target_group(guint id,const gchar *scope,const gchar *const *ids,guint n,gdouble percent,JsonObject *child)
{
    JsonObject *g=json_object_new();gchar *gid=uuid(id);json_object_set_string_member(g,"id",gid);g_free(gid);
    json_object_set_string_member(g,"scope",scope);JsonArray *targets=json_array_new();for (guint i=0;i<n;++i) json_array_add_string_element(targets,ids[i]);
    json_object_set_array_member(g,"ids",targets);if (percent>=0) json_object_set_double_member(g,"probability",percent);if (child) json_object_set_object_member(g,"policy",child);return g;
}
static void raw_group_percent_not_per_member(void)
{
    JsonObject *p=prepared("automation"),*c=configuration(p,200),*a=add_source(p,c,1,100,100,"music"),*b=add_source(p,c,2,1,1,"music");
    JsonArray *songs=json_object_get_array_member(a,"tracks");for (guint i=0;i<3;++i) json_array_add_object_element(songs,row(i+1,"Farklı"));
    json_array_add_object_element(json_object_get_array_member(b,"tracks"),row(4,"Diğer"));json_object_set_int_member(c,"sourceMode",1);
    JsonObject *policy=new_policy(),*child=new_policy();JsonArray *tracks=json_array_new(),*groups=json_array_new();
    const gchar *track_ids[]={json_object_get_string_member(json_array_get_object_element(songs,0),"entry_id"),json_object_get_string_member(json_array_get_object_element(songs,1),"entry_id")};
    json_array_add_object_element(tracks,target_group(9001,"track",track_ids,2,100,NULL));json_array_add_object_element(tracks,target_group(9002,"track",NULL,0,0,NULL));json_object_set_array_member(child,"children",tracks);
    const gchar *playlist[]={json_object_get_string_member(a,"playlist_id")};json_array_add_object_element(groups,target_group(9003,"playlist",playlist,1,30,child));
    json_array_add_object_element(groups,target_group(9004,"playlist",NULL,0,-1,NULL));json_object_set_array_member(policy,"children",groups);json_object_set_object_member(c,"probability_policy",policy);
    RadioSPlayoutRequest req=request();RadioSPlayoutPlan *plan=build(p,&req),*same=build(p,&req);guint selected=0,one=0,two=0;
    g_assert_cmpuint(plan->entries->len,==,200);
    for (guint i=0;i<200;++i) {
        RadioSPlayoutEntry *e=entry(plan,i);g_assert_cmpstr(e->run.entry_id,==,entry(same,i)->run.entry_id);
        if (!strcmp(e->source_id,json_object_get_string_member(a,"source_id"))) {
            ++selected;if (!strcmp(e->run.entry_id,track_ids[0])) ++one;else if (!strcmp(e->run.entry_id,track_ids[1])) ++two;else g_assert_not_reached();
        }
    }
    g_assert_cmpuint(selected,>,44);g_assert_cmpuint(selected,<,76);g_assert_cmpint(ABS((gint)one-(gint)two),<=,1);
    radios_playout_plan_free(same);radios_playout_plan_free(plan);json_object_unref(p);
}
static void raw_no_repeat_protection_bypass(void)
{
    JsonObject *p=prepared("automation"),*c=configuration(p,4),*source=add_source(p,c,1,1,1,"music");
    json_array_add_object_element(json_object_get_array_member(source,"tracks"),row(1,"Sanatçı"));json_object_set_int_member(c,"trackGap",1);
    json_object_set_object_member(c,"probability_policy",new_policy());RadioSPlayoutRequest req=request();RadioSPlayoutPlan *plan=build(p,&req);
    g_assert_cmpuint(plan->entries->len,==,1);g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(plan->display),"diagnostics")),>,0);
    radios_playout_plan_free(plan);json_object_unref(p);
}
static void raw_selection_checkpoint_linear_refill(void)
{
    JsonObject *p=prepared("automation"),*c=configuration(p,3);for (guint i=0;i<2;++i) {
        JsonObject *source=add_source(p,c,i+1,1,1,"music");json_array_add_object_element(json_object_get_array_member(source,"tracks"),row(i+1,"Sanatçı"));
    }
    json_object_set_object_member(c,"probability_policy",new_policy());RadioSPlayoutRequest req=request();RadioSPlayoutPlan *plan=build(p,&req);
    g_assert_cmpstr(entry(plan,0)->run.entry_id,==,entry(plan,2)->run.entry_id);g_assert_cmpstr(entry(plan,0)->run.entry_id,!=,entry(plan,1)->run.entry_id);
    JsonObject *shown=json_array_get_object_element(json_object_get_array_member(json_node_get_object(plan->display),"entries"),0);
    json_object_set_member(p,"policy_selection_checkpoint",song_copy(json_object_get_object_member(shown,"policy_selection_checkpoint")));json_object_set_int_member(c,"previewCount",2);
    RadioSPlayoutPlan *next=build(p,&req);g_assert_cmpstr(entry(next,0)->run.entry_id,==,entry(plan,1)->run.entry_id);g_assert_cmpstr(entry(next,1)->run.entry_id,==,entry(plan,0)->run.entry_id);
    radios_playout_plan_free(next);radios_playout_plan_free(plan);json_object_unref(p);
}
static void raw_posttransition_repeats_checked(void)
{
    JsonObject *p=prepared("automation"),*c=configuration(p,13),*source=add_source(p,c,1,1,1,"music");
    for (guint i=0;i<12;++i) { gchar *artist=g_strdup_printf("Sanatçı%u",i);json_array_add_object_element(json_object_get_array_member(source,"tracks"),row(i+1,artist));g_free(artist); }
    json_object_set_int_member(c,"artistGap",1);JsonObject *policy=new_policy();JsonArray *rules=json_array_new();json_array_add_object_element(rules,rule(31,0,0,2));json_object_set_array_member(policy,"transitions",rules);json_object_set_object_member(c,"probability_policy",policy);
    RadioSPlayoutRequest req=request();RadioSPlayoutPlan *plan=build(p,&req);
    g_assert_cmpuint(plan->entries->len,==,13);g_assert_cmpstr(entry(plan,0)->artist,==,entry(plan,12)->artist);
    g_assert_cmpint(entry(plan,12)->planned_utc_us-entry(plan,0)->planned_utc_us,>=,60*G_USEC_PER_SEC);
    radios_playout_plan_free(plan);json_object_unref(p);
}
static void early_uses_immutable_exit_markers(void)
{
    JsonObject *policy=new_policy();JsonArray *rules=json_array_new();json_array_add_object_element(rules,rule(40,0,0,2));json_object_set_array_member(policy,"transitions",rules);
    JsonObject *p=policy_programme(2,policy);JsonArray *tracks=json_object_get_array_member(p,"tracks");
    for (guint i=0;i<2;++i) short_autodj_markers(json_array_get_object_element(tracks,i),1000,5000);
    RadioSPlayoutRequest req=request();RadioSPlayoutPlan *plan=build(p,&req);
    g_assert_cmpuint(entry(plan,0)->run.autodj_out_frame,==,3000*48);
    g_assert_cmpuint(entry(plan,0)->run.media.cue_out_frame,==,288031);
    g_assert_cmpint(entry(plan,1)->planned_utc_us-entry(plan,0)->planned_utc_us,==,2*G_USEC_PER_SEC);
    radios_playout_plan_free(plan);
    for (guint i=0;i<2;++i) {
        JsonObject *marks=json_object_get_object_member(json_object_get_object_member(json_array_get_object_element(tracks,i),"detail"),"markers");
        json_object_set_null_member(marks,"autodj_out_ms");json_object_set_boolean_member(marks,"automatic",TRUE);
        json_object_set_int_member(marks,"auto_next_ms",4500);
    }
    plan=build(p,&req);g_assert_cmpuint(entry(plan,0)->run.autodj_out_frame,==,2500*48);
    radios_playout_plan_free(plan);json_object_unref(p);json_object_unref(policy);
}
static void short_or_spoken_pair_excludes_overlap_before_draw(void)
{
    JsonObject *policy=new_policy();JsonArray *rules=json_array_new();json_array_add_object_element(rules,rule(41,1,2000,0));
    json_array_add_object_element(rules,rule(42,0,0,0));json_object_set_array_member(policy,"transitions",rules);
    JsonObject *p=policy_programme(2,policy);JsonArray *tracks=json_object_get_array_member(p,"tracks");
    short_autodj_markers(json_array_get_object_element(tracks,1),1000,1250);
    RadioSPlayoutRequest req=request();RadioSPlayoutPlan *plan=build(p,&req);
    g_assert_cmpint(entry(plan,0)->run.mode,==,RADIOS_AUDIO_RUN_EOF);g_assert_cmpuint(entry(plan,0)->run.overlap_frames,==,0);radios_playout_plan_free(plan);
    JsonObject *next=json_array_get_object_element(tracks,1),*detail=json_object_get_object_member(next,"detail");json_object_remove_member(detail,"markers");
    json_object_set_int_member(json_object_get_object_member(detail,"version"),"duration_ms",100);
    json_object_set_int_member(json_object_get_object_member(detail,"analysis"),"sample_frames",4800);
    plan=build(p,&req);g_assert_cmpint(entry(plan,0)->run.mode,==,RADIOS_AUDIO_RUN_EOF);radios_playout_plan_free(plan);
    json_object_set_int_member(json_object_get_object_member(detail,"version"),"duration_ms",6001);
    json_object_set_int_member(json_object_get_object_member(detail,"analysis"),"sample_frames",288031);
    json_object_set_string_member(json_object_get_object_member(next,"metadata"),"kind","advert");
    plan=build(p,&req);g_assert_cmpint(entry(plan,0)->run.mode,==,RADIOS_AUDIO_RUN_EOF);radios_playout_plan_free(plan);
    /* An impossible early offset cannot replace a valid saved exit marker. */
    json_array_remove_element(rules,0);json_array_add_object_element(rules,rule(43,0,0,3600));
    plan=build(p,&req);g_assert_cmpint(entry(plan,0)->run.mode,==,RADIOS_AUDIO_RUN_EOF);g_assert_cmpuint(entry(plan,0)->run.autodj_out_frame,==,0);
    radios_playout_plan_free(plan);json_object_unref(p);json_object_unref(policy);
}
static void exclusions_follow_actual_early_transition_time(void)
{
    JsonObject *first=new_policy();JsonArray *rules=json_array_new();json_array_add_object_element(rules,rule(44,0,0,2));json_object_set_array_member(first,"transitions",rules);
    JsonObject *p=policy_programme(2,first),*second=new_policy(),*r=rule(45,2,0,0),*window=json_object_new();
    JsonArray *second_rules=json_array_new(),*excluded=json_array_new();json_object_set_int_member(window,"start_minute",31);json_object_set_int_member(window,"end_minute",32);
    json_array_add_object_element(excluded,window);json_object_set_array_member(r,"excluded_times",excluded);json_array_add_object_element(second_rules,r);json_object_set_array_member(second,"transitions",second_rules);
    JsonObject *song=json_array_get_object_element(json_object_get_array_member(p,"tracks"),1);json_object_set_object_member(song,"probability_policy",second);
    RadioSPlayoutRequest req=request();req.now_utc_us=stamp("2026-10-06T00:30:55Z");RadioSPlayoutPlan *plan=build(p,&req);
    g_assert_cmpint(entry(plan,1)->planned_utc_us,<,stamp("2026-10-06T00:31:00Z"));
    g_assert_cmpint(entry(plan,1)->run.mode,==,RADIOS_AUDIO_RUN_CUE);
    radios_playout_plan_free(plan);json_object_unref(p);json_object_unref(first);
}
static JsonObject *jingle_window_fixture(JsonObject **song_out,JsonObject **voice_out)
{
    JsonObject *policy=new_policy(),*r=json_object_new();JsonArray *rules=json_array_new();gchar *pid=uuid(9600),*rid=uuid(9601);
    json_object_set_string_member(r,"id",rid);json_object_set_string_member(r,"playlist_id",pid);json_object_set_int_member(r,"player",2);
    json_object_set_string_member(r,"anchor","after_start");json_object_set_double_member(r,"seconds",1);json_array_add_object_element(rules,r);json_object_set_array_member(policy,"jingles",rules);
    JsonObject *p=policy_programme(1,policy),*song=json_array_get_object_element(json_object_get_array_member(p,"tracks"),0),*voice=row(90,"Ses");
    JsonObject *detail=json_object_get_object_member(voice,"detail");json_object_set_int_member(json_object_get_object_member(detail,"version"),"duration_ms",1000);
    json_object_set_int_member(json_object_get_object_member(detail,"analysis"),"sample_frames",48000);
    JsonObject *source=json_object_new();JsonArray *catalog=json_array_new(),*tracks=json_array_new();json_array_add_object_element(tracks,voice);
    json_object_set_string_member(source,"playlist_id",pid);json_object_set_array_member(source,"tracks",tracks);json_array_add_object_element(catalog,source);json_object_set_array_member(p,"jingle_sources",catalog);
    *song_out=song;*voice_out=voice;json_object_unref(policy);g_free(pid);g_free(rid);return p;
}
static void jingle_requires_complete_verified_vocal_window(void)
{
    JsonObject *song,*voice,*p=jingle_window_fixture(&song,&voice);RadioSPlayoutRequest req=request();RadioSPlayoutPlan *plan=build(p,&req);
    g_assert_false(entry(plan,0)->run.adjunct.enabled);
    g_assert_false(json_object_has_member(json_array_get_object_element(json_object_get_array_member(json_node_get_object(plan->display),"entries"),0),"planned_jingles"));
    radios_playout_plan_free(plan);
    approved_voice_cue(song,"vocal_start",2500);plan=build(p,&req);
    g_assert_true(entry(plan,0)->run.adjunct.enabled);g_assert_cmpuint(entry(plan,0)->run.adjunct.start_output_frame,==,48000);radios_playout_plan_free(plan);
    JsonObject *cue=json_array_get_object_element(json_object_get_array_member(json_object_get_object_member(song,"detail"),"radio_cues"),0);
    json_object_set_int_member(cue,"position_ms",1500);plan=build(p,&req);g_assert_false(entry(plan,0)->run.adjunct.enabled);radios_playout_plan_free(plan);
    json_object_set_int_member(cue,"position_ms",2500);
    JsonObject *r=json_array_get_object_element(json_object_get_array_member(json_object_get_object_member(song,"probability_policy"),"jingles"),0);
    json_object_set_string_member(r,"anchor","before_end");json_object_set_double_member(r,"seconds",2);
    plan=build(p,&req);g_assert_false(entry(plan,0)->run.adjunct.enabled);radios_playout_plan_free(plan);
    approved_voice_cue(song,"outro_start",3000);plan=build(p,&req);g_assert_true(entry(plan,0)->run.adjunct.enabled);radios_playout_plan_free(plan);
    short_autodj_markers(song,0,4500);plan=build(p,&req);g_assert_false(entry(plan,0)->run.adjunct.enabled);radios_playout_plan_free(plan);
    const gchar *fields[]={"station_id","version_id","media_file_id","media_sha256","status"};
    for (guint i=0;i<G_N_ELEMENTS(fields);++i) {
        gchar *previous=g_strdup(json_object_get_string_member(cue,fields[i]));json_object_set_string_member(cue,fields[i],"invalid-or-foreign");rejected(p,&req);
        json_object_set_string_member(cue,fields[i],previous);g_free(previous);
    }
    json_object_set_int_member(cue,"_rev",0);rejected(p,&req);json_object_set_int_member(cue,"_rev",1);
    json_object_set_int_member(cue,"media_duration_ms",1);rejected(p,&req);json_object_set_int_member(cue,"media_duration_ms",6001);
    JsonArray *cues=json_object_get_array_member(json_object_get_object_member(song,"detail"),"radio_cues");
    json_array_remove_element(cues,0);json_array_add_null_element(cues);rejected(p,&req);
    json_object_unref(p);
}
int main(int argc,char **argv)
{
    g_test_init(&argc,&argv,NULL);database=g_build_filename(RADIOS_TEST_PROJECT_ROOT,"build","automation-policy-plan-fixture.sdb",NULL);
    g_test_add_func("/automation-probability-playout/early-effect-real-timeline",early_effect_real_timeline);
    g_test_add_func("/automation-probability-playout/jingle-verified-adjunct-owned-checkpoint",jingle_is_verified_adjunct_checkpoint_owned);
    g_test_add_func("/automation-probability-playout/independent-consumed-state-continuation",transition_state_continuation_not_selection_rewind);
    g_test_add_func("/automation-probability-playout/invalid-policy-rejected",invalid_policy_cannot_bypass_prepare);
    g_test_add_func("/automation-probability-playout/raw-group-percent-remaining-linear",raw_group_percent_not_per_member);
    g_test_add_func("/automation-probability-playout/raw-repeat-eligibility-preserved",raw_no_repeat_protection_bypass);
    g_test_add_func("/automation-probability-playout/raw-consumed-selection-linear-refill",raw_selection_checkpoint_linear_refill);
    g_test_add_func("/automation-probability-playout/raw-posttransition-repeat-constraints",raw_posttransition_repeats_checked);
    g_test_add_func("/automation-probability-playout/early-immutable-AutoDJ-automatic-exit",early_uses_immutable_exit_markers);
    g_test_add_func("/automation-probability-playout/short-spoken-transition-preselection",short_or_spoken_pair_excludes_overlap_before_draw);
    g_test_add_func("/automation-probability-playout/jingle-approved-vocal-windows",jingle_requires_complete_verified_vocal_window);
    g_test_add_func("/automation-probability-playout/exclusion-actual-early-boundary-time",exclusions_follow_actual_early_transition_time);
    gint result=g_test_run();g_free(database);return result;
}
