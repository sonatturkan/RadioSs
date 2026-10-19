#include "radios/weekly_programme_plan.h"
#include <string.h>

#define DAY_US G_GINT64_CONSTANT(86400000000)
static gchar *id(guint n) { return g_strdup_printf("20000000-0000-4000-8000-%012u", n); }
static void set_id(JsonObject *o, const gchar *key, guint n) { gchar *s=id(n); json_object_set_string_member(o,key,s);g_free(s); }
static JsonObject *at(JsonObject *o, const gchar *key, guint n) { return json_array_get_object_element(json_object_get_array_member(o,key),n); }
static gint64 epoch(const gchar *value) { GDateTime *d=g_date_time_new_from_iso8601(value,NULL);g_assert_nonnull(d);gint64 result=g_date_time_to_unix(d)*G_USEC_PER_SEC+g_date_time_get_microsecond(d);g_date_time_unref(d);return result; }
static gchar *encode(JsonObject *o) { JsonNode *n=json_node_new(JSON_NODE_OBJECT);json_node_set_object(n,o);gchar *result=json_to_string(n,FALSE);json_node_free(n);return result; }
static JsonObject *source(guint n)
{
    JsonObject *o=json_object_new();set_id(o,"id",n);set_id(o,"playlist_id",n+10000);json_object_set_int_member(o,"frequency",1);
    const gchar *flags[]={"random","track_random","advert_random","sweeper_random"};for(guint i=0;i<G_N_ELEMENTS(flags);++i)json_object_set_boolean_member(o,flags[i],FALSE);return o;
}
static JsonObject *event(guint n,const gchar *date,guint begin,guint end,gboolean playable)
{
    JsonObject *o=json_object_new();set_id(o,"id",n);json_object_set_string_member(o,"title","Haftalık yayın");json_object_set_string_member(o,"date",date);json_object_set_string_member(o,"color","#00ABCD");json_object_set_int_member(o,"start_minute",begin);json_object_set_int_member(o,"end_minute",end);
    JsonArray *rows=json_array_new();if(playable)json_array_add_object_element(rows,source(n+1000));json_object_set_array_member(o,"rotations",rows);return o;
}
static JsonObject *calendar(void) { JsonObject *o=json_object_new();json_object_set_int_member(o,"schema",1);json_object_set_array_member(o,"events",json_array_new());return o; }
static void add(JsonObject *o,JsonObject *e) { json_array_add_object_element(json_object_get_array_member(o,"events"),e); }
static JsonNode *snapshot(JsonObject *c,const gchar *anchor,gboolean repeat) { GError *error=NULL;JsonNode *result=radios_weekly_programme_snapshot(c,anchor,repeat,&error);g_assert_no_error(error);g_assert_nonnull(result);return result; }
static RadioSWeeklyProgrammeOccurrence *next(JsonNode *config,const gchar *now) { GError *error=NULL;RadioSWeeklyProgrammeOccurrence *o=radios_weekly_programme_next(json_node_get_object(config),epoch(now),&error);g_assert_no_error(error);return o; }
static void invalid_snapshot(JsonObject *c,const gchar *anchor) { GError *error=NULL;g_assert_null(radios_weekly_programme_snapshot(c,anchor,FALSE,&error));g_assert_error(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA);g_clear_error(&error); }
static void invalid_config(JsonObject *o) { GError *error=NULL;g_assert_null(radios_weekly_programme_next(o,epoch("2026-10-11T00:00:00Z"),&error));g_assert_error(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA);g_clear_error(&error); }
static JsonObject *settings(void)
{
    JsonObject *o=json_object_new();json_object_set_string_member(o,"selection_mode","weighted");const gchar *gaps[]={"track_gap_minutes","artist_gap_minutes","album_gap_minutes"};for(guint i=0;i<G_N_ELEMENTS(gaps);++i)json_object_set_int_member(o,gaps[i],0);
    JsonObject *choice=json_object_new();json_object_set_int_member(choice,"mode",0);json_object_set_int_member(choice,"weight",100);json_object_set_int_member(choice,"overlap_ms",0);JsonArray *choices=json_array_new();json_array_add_object_element(choices,choice);json_object_set_array_member(o,"transitions",choices);return o;
}
static JsonObject *definition(JsonObject *c,guint n,gboolean playable)
{
    if(!json_object_has_member(c,"rotation_library"))json_object_set_array_member(c,"rotation_library",json_array_new());
    JsonObject *d=json_object_new();set_id(d,"id",n);json_object_set_string_member(d,"name","Bağımsız rotasyon");json_object_set_object_member(d,"settings",settings());JsonArray *rows=json_array_new();if(playable)json_array_add_object_element(rows,source(n+1000));json_object_set_array_member(d,"sources",rows);json_array_add_object_element(json_object_get_array_member(c,"rotation_library"),d);return d;
}
static void assign(JsonObject *e,guint n,JsonObject *d,guint weight)
{ JsonObject *a=json_object_new();set_id(a,"id",n);json_object_set_string_member(a,"rotation_id",json_object_get_string_member(d,"id"));json_object_set_int_member(a,"weight",weight);json_array_add_object_element(json_object_get_array_member(e,"rotations"),a); }

static void test_event_probability_policy_accepted(void)
{
    /* The backend accepts an event-level probability_policy (block "Olasılık"),
     * so the native weekly validator must accept and keep it too. */
    JsonObject *c=calendar();JsonObject *e=event(1,"2026-10-09",0,60,TRUE);JsonObject *policy=json_object_new();
    json_object_set_int_member(policy,"schema",1);json_object_set_string_member(policy,"seed","blok");json_object_set_object_member(e,"probability_policy",policy);add(c,e);
    JsonNode *s=snapshot(c,"2026-10-05",TRUE);RadioSWeeklyProgrammeOccurrence *o=next(s,"2026-10-09T00:30:00Z");g_assert_nonnull(o);g_assert_true(o->active);
    g_assert_true(json_object_has_member(json_node_get_object(o->event),"probability_policy"));radios_weekly_programme_occurrence_free(o);json_node_free(s);
    json_object_set_int_member(policy,"schema",2);invalid_snapshot(c,"2026-10-05");json_object_unref(c);
}
static void test_complete_snapshot_deep_copy_and_anchor(void)
{
    JsonObject *c=calendar();add(c,event(3,"2026-10-19",60,120,TRUE));add(c,event(1,"2026-09-30",0,1,FALSE));add(c,event(2,"2026-10-11",1439,1440,FALSE));definition(c,500,FALSE);
    gchar *before=encode(c);JsonNode *s=snapshot(c,"2026-10-05",TRUE);JsonObject *o=json_node_get_object(s);
    g_assert_cmpstr(json_object_get_string_member(o,"week_anchor"),==,"2026-09-28");g_assert_cmpint(json_object_get_int_member(o,"week_count"),==,4);g_assert_true(json_object_get_boolean_member(o,"repeat"));
    JsonObject *copy=json_object_get_object_member(o,"calendar");gchar *same=encode(copy);g_assert_cmpstr(before,==,same);g_free(same);json_object_set_string_member(at(copy,"events",0),"title","Kopya");same=encode(c);g_assert_cmpstr(before,==,same);g_free(same);g_free(before);json_node_free(s);json_object_unref(c);
}
static void test_current_next_exact_time_and_half_open_boundaries(void)
{
    JsonObject *c=calendar();add(c,event(2,"2026-10-09",60,120,TRUE));add(c,event(1,"2026-10-09",0,60,TRUE));JsonNode *s=snapshot(c,"2026-10-05",FALSE);
    RadioSWeeklyProgrammeOccurrence *o=next(s,"2026-10-09T00:59:59.125Z");g_assert_nonnull(o);g_assert_true(o->active);g_assert_cmpint(o->effective_utc_us,==,epoch("2026-10-09T00:59:59.125Z"));g_assert_cmpint(o->next_start_utc_us,==,epoch("2026-10-09T01:00:00Z"));gchar *key=g_strdup(o->occurrence_key);radios_weekly_programme_occurrence_free(o);
    o=next(s,"2026-10-09T00:30:00Z");g_assert_cmpstr(o->occurrence_key,==,key);g_free(key);radios_weekly_programme_occurrence_free(o);
    o=next(s,"2026-10-09T01:00:00Z");g_assert_true(o->active);g_assert_cmpint(o->start_utc_us,==,epoch("2026-10-09T01:00:00Z"));g_assert_cmpint(o->next_start_utc_us,==,0);radios_weekly_programme_occurrence_free(o);
    o=next(s,"2026-10-08T23:59:59.999Z");g_assert_false(o->active);g_assert_cmpint(o->effective_utc_us,==,o->start_utc_us);radios_weekly_programme_occurrence_free(o);g_assert_null(next(s,"2026-10-09T02:00:00Z"));json_node_free(s);json_object_unref(c);
}
static RadioSWeeklyProgrammeOccurrence *merged(JsonNode *main_config, JsonNode *override_config, const gchar *now, gboolean *use_override, gint64 *preempt)
{
    GError *error=NULL;*use_override=TRUE;*preempt=-1;
    RadioSWeeklyProgrammeOccurrence *o=radios_weekly_programme_override_next(main_config?json_node_get_object(main_config):NULL,
        override_config?json_node_get_object(override_config):NULL,epoch(now),use_override,preempt,&error);
    g_assert_no_error(error);return o;
}
/* "Şema yükle": the temporary schema takes the flow only inside its own slots. */
static void test_override_schema_joins_main_flow_only_inside_its_slots(void)
{
    JsonObject *m=calendar();add(m,event(1,"2026-10-09",0,120,TRUE));add(m,event(2,"2026-10-09",120,240,TRUE));add(m,event(5,"2026-10-09",360,420,TRUE));
    JsonNode *ms=snapshot(m,"2026-10-05",FALSE);
    JsonObject *v=calendar();add(v,event(3,"2026-10-09",60,90,TRUE));add(v,event(4,"2026-10-09",300,330,TRUE));JsonNode *vs=snapshot(v,"2026-10-05",FALSE);
    gboolean use=FALSE;gint64 preempt=0;
    /* main slot plays, the temporary slot begins inside it: main now, preempt at 01:00 */
    RadioSWeeklyProgrammeOccurrence *o=merged(ms,vs,"2026-10-09T00:30:00Z",&use,&preempt);
    g_assert_nonnull(o);g_assert_false(use);g_assert_true(o->active);g_assert_cmpint(o->start_utc_us,==,epoch("2026-10-09T00:00:00Z"));
    g_assert_cmpint(preempt,==,epoch("2026-10-09T01:00:00Z"));radios_weekly_programme_occurrence_free(o);
    /* the temporary slot is active: it plays, nothing to preempt */
    o=merged(ms,vs,"2026-10-09T01:10:00Z",&use,&preempt);
    g_assert_true(use);g_assert_true(o->active);g_assert_cmpint(o->start_utc_us,==,epoch("2026-10-09T01:00:00Z"));
    g_assert_cmpint(o->end_utc_us,==,epoch("2026-10-09T01:30:00Z"));g_assert_cmpint(preempt,==,0);radios_weekly_programme_occurrence_free(o);
    /* the temporary slot ended: the main slot resumes mid-way */
    o=merged(ms,vs,"2026-10-09T01:30:00Z",&use,&preempt);
    g_assert_false(use);g_assert_true(o->active);g_assert_cmpint(o->start_utc_us,==,epoch("2026-10-09T00:00:00Z"));
    g_assert_cmpint(o->effective_utc_us,==,epoch("2026-10-09T01:30:00Z"));g_assert_cmpint(preempt,==,0);radios_weekly_programme_occurrence_free(o);
    /* before both: the earlier main slot waits, preemption inside it is kept */
    o=merged(ms,vs,"2026-10-08T23:00:00Z",&use,&preempt);
    g_assert_false(use);g_assert_false(o->active);g_assert_cmpint(o->start_utc_us,==,epoch("2026-10-09T00:00:00Z"));
    g_assert_cmpint(preempt,==,epoch("2026-10-09T01:00:00Z"));radios_weekly_programme_occurrence_free(o);
    /* a gap in the main schema: the temporary slot that starts first wins */
    o=merged(ms,vs,"2026-10-09T04:30:00Z",&use,&preempt);
    g_assert_true(use);g_assert_false(o->active);g_assert_cmpint(o->start_utc_us,==,epoch("2026-10-09T05:00:00Z"));g_assert_cmpint(preempt,==,0);
    radios_weekly_programme_occurrence_free(o);
    /* after the temporary slots: plain main, no preemption */
    o=merged(ms,vs,"2026-10-09T05:45:00Z",&use,&preempt);
    g_assert_false(use);g_assert_cmpint(o->start_utc_us,==,epoch("2026-10-09T06:00:00Z"));g_assert_cmpint(preempt,==,0);radios_weekly_programme_occurrence_free(o);
    /* without a temporary schema the main schema is unchanged */
    o=merged(ms,NULL,"2026-10-09T00:30:00Z",&use,&preempt);g_assert_false(use);g_assert_true(o->active);g_assert_cmpint(preempt,==,0);radios_weekly_programme_occurrence_free(o);
    /* without a main schema the temporary schema plays alone */
    o=merged(NULL,vs,"2026-10-09T00:30:00Z",&use,&preempt);g_assert_true(use);g_assert_false(o->active);g_assert_cmpint(preempt,==,0);radios_weekly_programme_occurrence_free(o);
    /* both exhausted */
    g_assert_null(merged(ms,vs,"2026-10-10T00:00:00Z",&use,&preempt));g_assert_false(use);g_assert_cmpint(preempt,==,0);
    json_node_free(ms);json_node_free(vs);json_object_unref(m);json_object_unref(v);
}
static void test_repeat_entire_multiweek_pattern_and_all_dates(void)
{
    JsonObject *c=calendar();add(c,event(1,"2026-10-05",0,1,TRUE));add(c,event(2,"2026-10-19",60,61,TRUE));add(c,event(3,"2026-10-25",0,1,FALSE));JsonNode *s=snapshot(c,"2026-10-05",TRUE);g_assert_cmpint(json_object_get_int_member(json_node_get_object(s),"week_count"),==,3);gchar *before=encode(json_node_get_object(s));
    RadioSWeeklyProgrammeOccurrence *o=next(s,"2026-10-26T00:00:00.750Z");g_assert_true(o->active);g_assert_cmpuint(o->cycle_index,==,1);g_assert_cmpint(o->effective_utc_us,==,epoch("2026-10-26T00:00:00.750Z"));g_assert_cmpstr(json_object_get_string_member(at(json_node_get_object(o->calendar),"events",2),"date"),==,"2026-11-15");radios_weekly_programme_occurrence_free(o);
    o=next(s,"2026-11-16T00:01:00Z");g_assert_false(o->active);g_assert_cmpuint(o->cycle_index,==,2);g_assert_cmpint(o->start_utc_us,==,epoch("2026-11-30T01:00:00Z"));radios_weekly_programme_occurrence_free(o);
    o=next(s,"2026-10-20T00:00:00Z");g_assert_cmpuint(o->cycle_index,==,1);g_assert_cmpint(o->start_utc_us,==,epoch("2026-10-26T00:00:00Z"));g_assert_cmpint(o->next_start_utc_us,==,o->start_utc_us);radios_weekly_programme_occurrence_free(o);gchar *after=encode(json_node_get_object(s));g_assert_cmpstr(before,==,after);g_free(before);g_free(after);json_node_free(s);json_object_unref(c);
}
static void test_before_anchor_leap_year_and_negative_epoch(void)
{
    JsonObject *c=calendar();add(c,event(1,"2028-02-29",720,721,TRUE));JsonNode *s=snapshot(c,"2028-02-28",TRUE);RadioSWeeklyProgrammeOccurrence *o=next(s,"2028-02-20T00:00:00Z");g_assert_cmpuint(o->cycle_index,==,0);g_assert_cmpint(o->start_utc_us,==,epoch("2028-02-29T12:00:00Z"));radios_weekly_programme_occurrence_free(o);
    o=next(s,"2028-03-07T12:00:00Z");g_assert_true(o->active);g_assert_cmpuint(o->cycle_index,==,1);radios_weekly_programme_occurrence_free(o);json_node_free(s);json_object_unref(c);
    c=calendar();add(c,event(1,"1969-12-29",0,1,TRUE));s=snapshot(c,"1969-12-29",TRUE);o=next(s,"1969-12-20T00:00:00Z");g_assert_cmpint(o->start_utc_us,<,0);g_assert_cmpuint(o->cycle_index,==,0);radios_weekly_programme_occurrence_free(o);json_node_free(s);json_object_unref(c);
}
static void test_empty_events_and_empty_library_assignments_preserved_skipped(void)
{
    JsonObject *c=calendar();add(c,event(1,"2026-10-05",0,60,FALSE));JsonObject *d=definition(c,500,FALSE),*e=event(2,"2026-10-06",0,60,FALSE);assign(e,600,d,100);add(c,e);add(c,event(3,"2026-10-07",0,60,TRUE));JsonNode *s=snapshot(c,"2026-10-05",FALSE);RadioSWeeklyProgrammeOccurrence *o=next(s,"2026-10-05T00:01:00Z");g_assert_nonnull(o);g_assert_cmpint(o->start_utc_us,==,epoch("2026-10-07T00:00:00Z"));g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(o->calendar),"events")),==,3);radios_weekly_programme_occurrence_free(o);json_node_free(s);
    json_array_remove_element(json_object_get_array_member(c,"events"),2);s=snapshot(c,"2026-10-05",TRUE);g_assert_null(next(s,"2026-10-05T00:01:00Z"));json_node_free(s);json_object_unref(c);
    c=calendar();s=snapshot(c,"2026-10-05",TRUE);g_assert_null(next(s,"2026-10-11T00:00:00Z"));json_node_free(s);json_object_unref(c);
}
static void test_assignment_rules_routing_and_original_identity(void)
{
    JsonObject *c=calendar(),*d=definition(c,500,TRUE),*e=event(1,"2026-10-05",0,60,FALSE);assign(e,600,d,65);json_object_set_string_member(e,"rotation_selection_mode","weighted");add(c,e);
    JsonObject *r=at(d,"sources",0);json_object_set_int_member(r,"weight",55);json_object_set_int_member(r,"count",3);json_object_set_string_member(r,"track_selection_mode","weighted");JsonObject *policy=json_object_new();set_id(policy,"entry_id",9999);const gchar *keys[]={"weight","order","frequency","max_plays","start_minute","end_minute","interval_minutes"};const gint64 values[]={20,4,2,3,0,1440,7};for(guint i=0;i<G_N_ELEMENTS(keys);++i)json_object_set_int_member(policy,keys[i],values[i]);JsonArray *policies=json_array_new();json_array_add_object_element(policies,policy);json_object_set_array_member(r,"track_rules",policies);
    JsonObject *routing=json_object_new();const gchar *kinds[]={"music","advertisement","jingle","sweeper"};for(guint i=0;i<4;++i){JsonArray *pool=json_array_new();json_array_add_int_element(pool,i+2);json_object_set_array_member(routing,kinds[i],pool);}json_object_set_object_member(r,"autodj",routing);JsonNode *s=snapshot(c,"2026-10-05",TRUE);RadioSWeeklyProgrammeOccurrence *o=next(s,"2026-10-12T00:00:00Z");g_assert_nonnull(o);gchar *original=id(1);g_assert_cmpstr(o->original_event_id,==,original);g_free(original);
    JsonObject *expanded=at(json_node_get_object(o->event),"rotations",0);g_assert_cmpint(json_object_get_int_member(expanded,"calendar_group_weight"),==,65);g_assert_cmpint(json_object_get_int_member(expanded,"weight"),==,55);g_assert_cmpint(json_object_get_int_member(at(expanded,"track_rules",0),"interval_minutes"),==,7);g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(json_object_get_object_member(expanded,"autodj"),"jingle"),0),==,4);radios_weekly_programme_occurrence_free(o);json_node_free(s);json_object_unref(c);
}
static void test_snapshot_over_52_weeks_explicit_error_no_mutation(void)
{
    JsonObject *c=calendar();add(c,event(1,"2026-10-05",0,1,TRUE));add(c,event(2,"2027-10-03",0,1,FALSE));JsonNode *s=snapshot(c,"2026-10-05",FALSE);g_assert_cmpint(json_object_get_int_member(json_node_get_object(s),"week_count"),==,52);json_node_free(s);json_object_set_string_member(at(c,"events",1),"date","2027-10-04");gchar *before=encode(c);invalid_snapshot(c,"2026-10-05");gchar *after=encode(c);g_assert_cmpstr(before,==,after);g_free(before);g_free(after);invalid_snapshot(c,"2026-10-06");json_object_unref(c);
}
static void test_strict_metadata_empty_slots_and_config_type_guards(void)
{
    JsonObject *c=calendar();add(c,event(1,"2026-10-05",0,1,FALSE));JsonNode *s=snapshot(c,"2026-10-05",FALSE);JsonObject *config=json_node_get_object(s);json_object_set_int_member(config,"repeat",1);invalid_config(config);json_object_set_boolean_member(config,"repeat",FALSE);json_object_set_int_member(config,"week_count",53);invalid_config(config);json_object_set_int_member(config,"week_count",1);json_object_set_string_member(at(c,"events",0),"date","2026-02-30");invalid_snapshot(c,"2026-10-05");json_object_set_string_member(at(c,"events",0),"date","2026-10-05");json_object_set_string_member(at(c,"events",0),"rotation_selection_mode","random");invalid_snapshot(c,"2026-10-05");json_object_remove_member(at(c,"events",0),"rotation_selection_mode");json_array_add_int_element(json_object_get_array_member(c,"events"),3);invalid_snapshot(c,"2026-10-05");json_node_free(s);json_object_unref(c);
    c=calendar();JsonObject *d=definition(c,500,FALSE);json_object_set_string_member(json_object_get_object_member(d,"settings"),"selection_mode","bad");invalid_snapshot(c,"2026-10-05");json_object_unref(c);
}
static void test_overlap_duplicate_reference_and_unknown_rejected(void)
{
    JsonObject *c=calendar();add(c,event(1,"2026-10-05",0,60,FALSE));add(c,event(2,"2026-10-05",59,61,FALSE));invalid_snapshot(c,"2026-10-05");json_object_set_int_member(at(c,"events",1),"start_minute",60);JsonNode *s=snapshot(c,"2026-10-05",FALSE);json_node_free(s);set_id(at(c,"events",1),"id",1);invalid_snapshot(c,"2026-10-05");json_object_unref(c);
    c=calendar();JsonObject *d=definition(c,500,FALSE),*e=event(1,"2026-10-05",0,1,FALSE);assign(e,600,d,1);set_id(at(e,"rotations",0),"rotation_id",999);add(c,e);invalid_snapshot(c,"2026-10-05");json_object_unref(c);
    c=calendar();d=definition(c,500,TRUE);json_object_set_boolean_member(d,"verified",TRUE);invalid_snapshot(c,"2026-10-05");json_object_remove_member(d,"verified");json_object_set_boolean_member(at(d,"sources",0),"approved",TRUE);invalid_snapshot(c,"2026-10-05");json_object_unref(c);
}
static void test_supported_date_limits_and_repeated_clone_range(void)
{
    JsonObject *c=calendar();invalid_snapshot(c,"9999-12-27");add(c,event(1,"9999-12-20",0,1,TRUE));JsonNode *s=snapshot(c,"9999-12-20",TRUE);GError *error=NULL;g_assert_null(radios_weekly_programme_next(json_node_get_object(s),epoch("9999-12-31T23:59:59Z"),&error));g_assert_error(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA);g_clear_error(&error);json_node_free(s);json_object_unref(c);
    c=calendar();add(c,event(1,"0001-01-01",0,1,TRUE));s=snapshot(c,"0001-01-01",FALSE);RadioSWeeklyProgrammeOccurrence *o=next(s,"0001-01-01T00:00:00.001Z");g_assert_nonnull(o);g_assert_true(o->active);radios_weekly_programme_occurrence_free(o);json_node_free(s);json_object_unref(c);
}
static void test_station_timezone_midnight_and_dst_repeat(void)
{
    JsonObject *c = calendar(); json_object_set_string_member(c, "timezone", "Europe/Istanbul");
    add(c, event(1, "2026-10-05", 0, 60, TRUE));
    JsonNode *saved = snapshot(c, "2026-10-05", TRUE);
    RadioSWeeklyProgrammeOccurrence *o = next(saved, "2026-10-04T21:00:00Z");
    g_assert_nonnull(o); g_assert_true(o->active);
    g_assert_cmpint(o->start_utc_us, ==, epoch("2026-10-04T21:00:00Z"));
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(o->event), "timezone"), ==, "Europe/Istanbul");
    radios_weekly_programme_occurrence_free(o);
    o = next(saved, "2026-10-11T21:00:00Z");
    g_assert_true(o->active); g_assert_cmpuint(o->cycle_index, ==, 1);
    g_assert_cmpint(o->start_utc_us, ==, epoch("2026-10-11T21:00:00Z"));
    radios_weekly_programme_occurrence_free(o); json_node_free(saved); json_object_unref(c);

    c = calendar(); json_object_set_string_member(c, "timezone", "Europe/Berlin");
    add(c, event(1, "2026-10-19", 540, 600, TRUE)); saved = snapshot(c, "2026-10-19", TRUE);
    o = next(saved, "2026-10-19T07:00:00Z"); g_assert_true(o->active);
    g_assert_cmpint(o->start_utc_us, ==, epoch("2026-10-19T07:00:00Z"));
    g_assert_cmpint(o->next_start_utc_us, ==, epoch("2026-10-26T08:00:00Z"));
    radios_weekly_programme_occurrence_free(o);
    o = next(saved, "2026-10-26T08:00:00Z"); g_assert_true(o->active); g_assert_cmpuint(o->cycle_index, ==, 1);
    g_assert_cmpint(o->start_utc_us, ==, epoch("2026-10-26T08:00:00Z"));
    radios_weekly_programme_occurrence_free(o); json_node_free(saved);
    json_object_set_string_member(c, "timezone", "Not/AZone"); invalid_snapshot(c, "2026-10-19"); json_object_unref(c);
}
static RadioSWeeklyProgrammeOccurrence *future(JsonNode *config, const gchar *now)
{
    GError *error = NULL;
    RadioSWeeklyProgrammeOccurrence *result = radios_weekly_programme_next_future(json_node_get_object(config), epoch(now), &error);
    g_assert_no_error(error); return result;
}
static JsonObject *schedule_record(const gchar *kind, guint identifier, JsonObject *config)
{
    JsonObject *record = json_object_new(); set_id(record, "_id", identifier); set_id(record, "station_id", 90000);
    json_object_set_int_member(record, "_rev", 7); json_object_set_string_member(record, "kind", kind);
    json_object_set_string_member(record, "name", g_str_equal(kind, "automation_calendar") ? "main" : "Kayıtlı program");
    json_object_set_string_member(record, "status", "active");
    json_object_set_object_member(record, "config", json_object_ref(config)); return record;
}
static RadioSWeeklyProgrammeSelection *select_schedule(JsonArray *records, const gchar *now)
{
    GError *error = NULL;
    RadioSWeeklyProgrammeSelection *result = radios_weekly_programme_select_next(records, epoch(now), &error);
    g_assert_no_error(error); return result;
}
static void test_strict_future_skips_active_and_preserves_repeat(void)
{
    JsonObject *c = calendar(); add(c, event(1, "2026-10-05", 0, 60, TRUE)); add(c, event(2, "2026-10-05", 60, 120, TRUE));
    JsonNode *saved = snapshot(c, "2026-10-05", FALSE); gchar *before = encode(json_node_get_object(saved));
    RadioSWeeklyProgrammeOccurrence *o = future(saved, "2026-10-05T00:30:00.125Z");
    g_assert_nonnull(o); g_assert_false(o->active); g_assert_cmpint(o->start_utc_us, ==, epoch("2026-10-05T01:00:00Z"));
    g_assert_cmpint(o->effective_utc_us, ==, o->start_utc_us); radios_weekly_programme_occurrence_free(o);
    g_assert_null(future(saved, "2026-10-05T01:00:00Z"));
    o = next(saved, "2026-10-05T01:00:00Z"); g_assert_true(o->active); radios_weekly_programme_occurrence_free(o);
    gchar *after = encode(json_node_get_object(saved)); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
    json_object_set_boolean_member(json_node_get_object(saved), "repeat", TRUE);
    o = future(saved, "2026-10-05T01:00:00Z"); g_assert_nonnull(o); g_assert_cmpuint(o->cycle_index, ==, 1);
    g_assert_cmpint(o->start_utc_us, ==, epoch("2026-10-12T00:00:00Z")); radios_weekly_programme_occurrence_free(o);
    json_node_free(saved); json_object_unref(c);
}
static void test_zero_source_and_assignment_weights_are_skipped(void)
{
    JsonObject *c = calendar(), *group = definition(c, 500, TRUE), *muted = definition(c, 501, TRUE);
    json_object_set_int_member(at(muted, "sources", 0), "weight", 0);
    JsonObject *e = event(1, "2026-10-05", 0, 60, FALSE); assign(e, 600, group, 0); add(c, e);
    e = event(2, "2026-10-05", 60, 120, FALSE); assign(e, 601, muted, 100); add(c, e);
    e = event(3, "2026-10-05", 120, 180, FALSE); assign(e, 602, group, 100); add(c, e);
    JsonNode *saved = snapshot(c, "2026-10-05", TRUE);
    RadioSWeeklyProgrammeOccurrence *o = next(saved, "2026-10-05T00:30:00Z");
    g_assert_nonnull(o); g_assert_false(o->active); g_assert_cmpint(o->start_utc_us, ==, epoch("2026-10-05T02:00:00Z"));
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(o->calendar), "events")), ==, 3);
    radios_weekly_programme_occurrence_free(o); json_node_free(saved);
    json_array_remove_element(json_object_get_array_member(c, "events"), 2); saved = snapshot(c, "2026-10-05", TRUE);
    g_assert_null(next(saved, "2026-10-05T00:30:00Z")); g_assert_null(future(saved, "2026-10-05T00:30:00Z"));
    json_node_free(saved); json_object_unref(c);
}
static void test_mixed_sources_nearest_local_week_boundary_and_main_tie(void)
{
    JsonObject *main = calendar(), *weekly = calendar(); json_object_set_string_member(main, "timezone", "Europe/Istanbul");
    add(main, event(1, "2026-10-05", 0, 60, TRUE)); add(weekly, event(2, "2026-10-05", 0, 60, TRUE));
    JsonNode *saved = snapshot(weekly, "2026-10-05", FALSE);
    JsonArray *records = json_array_new(); JsonObject *weekly_record = schedule_record("weekly_programme", 800, json_node_get_object(saved));
    JsonObject *main_record = schedule_record("automation_calendar", 900, main);
    json_array_add_object_element(records, weekly_record); json_array_add_object_element(records, main_record);
    gchar *before = encode(main_record);
    RadioSWeeklyProgrammeSelection *s = select_schedule(records, "2026-10-04T20:00:00Z");
    g_assert_nonnull(s); g_assert_cmpstr(json_object_get_string_member(json_node_get_object(s->record), "kind"), ==, "automation_calendar");
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(s->config), "week_anchor"), ==, "2026-10-05");
    g_assert_false(json_object_get_boolean_member(json_node_get_object(s->config), "repeat"));
    g_assert_cmpint(s->occurrence->start_utc_us, ==, epoch("2026-10-04T21:00:00Z"));
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(s->occurrence->event), "timezone"), ==, "Europe/Istanbul");
    radios_weekly_programme_selection_free(s);
    /* Main is active now and therefore excluded from the initial future selection. */
    s = select_schedule(records, "2026-10-04T21:30:00Z");
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(s->record), "kind"), ==, "weekly_programme");
    radios_weekly_programme_selection_free(s);
    json_object_set_int_member(at(main, "events", 0), "start_minute", 180); json_object_set_int_member(at(main, "events", 0), "end_minute", 240);
    s = select_schedule(records, "2026-10-04T23:59:59.999999Z");
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(s->record), "kind"), ==, "automation_calendar");
    g_assert_cmpint(s->occurrence->start_utc_us, ==, epoch("2026-10-05T00:00:00Z"));
    json_object_set_string_member(json_node_get_object(s->record), "name", "Owned clone");
    g_assert_cmpstr(json_object_get_string_member(main_record, "name"), ==, "main");
    radios_weekly_programme_selection_free(s);
    json_object_set_int_member(at(main, "events", 0), "start_minute", 0); json_object_set_int_member(at(main, "events", 0), "end_minute", 60);
    gchar *after = encode(main_record); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
    json_array_unref(records); json_node_free(saved); json_object_unref(main); json_object_unref(weekly);
}
static void test_mixed_empty_disabled_finite_and_explicit_repeat(void)
{
    JsonObject *main = calendar(), *weekly = calendar(); add(weekly, event(1, "2026-10-05", 0, 60, TRUE));
    JsonNode *saved = snapshot(weekly, "2026-10-05", FALSE); JsonArray *records = json_array_new();
    json_array_add_object_element(records, schedule_record("automation_calendar", 900, main));
    JsonObject *weekly_record = schedule_record("weekly_programme", 800, json_node_get_object(saved)); json_array_add_object_element(records, weekly_record);
    g_assert_null(select_schedule(records, "2026-10-12T00:00:00Z"));
    json_object_set_boolean_member(json_node_get_object(saved), "repeat", TRUE);
    RadioSWeeklyProgrammeSelection *s = select_schedule(records, "2026-10-12T00:00:00Z");
    g_assert_nonnull(s); g_assert_cmpint(s->occurrence->start_utc_us, ==, epoch("2026-10-19T00:00:00Z"));
    g_assert_true(json_object_get_boolean_member(json_node_get_object(s->config), "repeat")); radios_weekly_programme_selection_free(s);
    json_object_set_string_member(weekly_record, "status", "disabled"); g_assert_null(select_schedule(records, "2026-10-12T00:00:00Z"));
    json_array_unref(records); json_node_free(saved); json_object_unref(main); json_object_unref(weekly);
}
static void test_mixed_record_identity_fences_and_uuid_tie(void)
{
    JsonObject *c = calendar(); add(c, event(1, "2026-10-05", 60, 120, TRUE)); JsonNode *saved = snapshot(c, "2026-10-05", FALSE);
    JsonArray *records = json_array_new(); JsonObject *later = schedule_record("weekly_programme", 900, json_node_get_object(saved));
    JsonObject *first = schedule_record("weekly_programme", 800, json_node_get_object(saved));
    json_array_add_object_element(records, later); json_array_add_object_element(records, first);
    RadioSWeeklyProgrammeSelection *s = select_schedule(records, "2026-10-05T00:00:00Z");
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(s->record), "_id"), ==, json_object_get_string_member(first, "_id"));
    g_assert_true(g_str_has_prefix(s->source_key, "weekly_programme:")); radios_weekly_programme_selection_free(s);
    set_id(first, "station_id", 90001); GError *error = NULL;
    g_assert_null(radios_weekly_programme_select_next(records, epoch("2026-10-05T00:00:00Z"), &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error); json_array_unref(records);
    records = json_array_new(); json_array_add_object_element(records, schedule_record("automation_calendar", 800, c));
    json_array_add_object_element(records, schedule_record("automation_calendar", 900, c));
    g_assert_null(radios_weekly_programme_select_next(records, epoch("2026-10-05T00:00:00Z"), &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    json_array_unref(records); json_node_free(saved); json_object_unref(c);
}
static void test_running_late_boundary_floor_and_finished_slots(void)
{
    JsonObject *main = calendar(), *weekly = calendar();
    add(main, event(1, "2026-10-05", 0, 60, TRUE)); add(main, event(2, "2026-10-05", 60, 120, TRUE));
    add(main, event(3, "2026-10-05", 120, 180, TRUE));
    add(weekly, event(4, "2026-10-05", 30, 150, TRUE)); add(weekly, event(5, "2026-10-05", 150, 180, TRUE));
    JsonNode *saved = snapshot(weekly, "2026-10-05", FALSE); JsonArray *records = json_array_new();
    json_array_add_object_element(records, schedule_record("weekly_programme", 800, json_node_get_object(saved)));
    json_array_add_object_element(records, schedule_record("automation_calendar", 900, main)); GError *error = NULL;
    RadioSWeeklyProgrammeSelection *s = radios_weekly_programme_select_running(records,
        epoch("2026-10-05T01:00:00.005Z"), epoch("2026-10-05T01:00:00Z"), &error);
    g_assert_no_error(error); g_assert_nonnull(s); g_assert_true(s->occurrence->active);
    g_assert_cmpint(s->occurrence->start_utc_us, ==, epoch("2026-10-05T01:00:00Z"));
    g_assert_cmpint(s->occurrence->effective_utc_us, ==, epoch("2026-10-05T01:00:00.005Z"));
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(s->record), "kind"), ==, "automation_calendar");
    radios_weekly_programme_selection_free(s);
    /* A delayed tick skips fully elapsed slots; the older weekly overlap is
     * below the floor and cannot reclaim the general queue. */
    s = radios_weekly_programme_select_running(records, epoch("2026-10-05T02:10:00Z"), epoch("2026-10-05T01:00:00Z"), &error);
    g_assert_no_error(error); g_assert_true(s->occurrence->active);
    g_assert_cmpint(s->occurrence->start_utc_us, ==, epoch("2026-10-05T02:00:00Z")); radios_weekly_programme_selection_free(s);
    /* A future floor filters the earlier candidate within the SAME saved
     * document instead of discarding that document's later event. */
    json_array_remove_element(records, 1);
    s = radios_weekly_programme_select_running(records, epoch("2026-10-05T01:00:00Z"), epoch("2026-10-05T02:00:00Z"), &error);
    g_assert_no_error(error); g_assert_false(s->occurrence->active);
    g_assert_cmpint(s->occurrence->start_utc_us, ==, epoch("2026-10-05T02:30:00Z")); radios_weekly_programme_selection_free(s);
    g_assert_null(radios_weekly_programme_select_running(records, epoch("2026-10-05T03:00:00Z"), epoch("2026-10-05T02:00:00Z"), &error));
    g_assert_no_error(error); json_array_unref(records); json_node_free(saved); json_object_unref(main); json_object_unref(weekly);
}
static void test_multiyear_main_preserves_record_and_selects_one_runtime_slot(void)
{
    JsonObject *main = calendar(); json_object_set_string_member(main, "timezone", "Europe/Istanbul");
    definition(main, 500, FALSE);
    add(main, event(1, "2020-01-06", 0, 60, TRUE));
    add(main, event(2, "2026-10-05", 0, 60, TRUE));
    add(main, event(3, "2030-01-07", 0, 60, TRUE));
    JsonArray *records = json_array_new(); JsonObject *record = schedule_record("automation_calendar", 900, main);
    json_array_add_object_element(records, record); gchar *before = encode(record);
    RadioSWeeklyProgrammeSelection *s = select_schedule(records, "2026-10-04T20:59:59.999999Z");
    g_assert_nonnull(s); g_assert_cmpint(s->occurrence->start_utc_us, ==, epoch("2026-10-04T21:00:00Z"));
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_object_get_object_member(json_node_get_object(s->record), "config"), "events")), ==, 3);
    JsonObject *runtime = json_object_get_object_member(json_node_get_object(s->config), "calendar");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(runtime, "events")), ==, 1);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(runtime, "rotation_library")), ==, 1);
    g_assert_cmpint(json_object_get_int_member(json_node_get_object(s->config), "week_count"), ==, 1);
    g_assert_false(json_object_get_boolean_member(json_node_get_object(s->config), "repeat"));
    g_assert_cmpstr(json_object_get_string_member(at(runtime, "events", 0), "date"), ==, "2026-10-05");
    radios_weekly_programme_selection_free(s); GError *error = NULL;
    s = radios_weekly_programme_select_running(records, epoch("2026-10-04T21:00:00.005Z"), epoch("2026-10-04T21:00:00Z"), &error);
    g_assert_no_error(error); g_assert_true(s->occurrence->active); radios_weekly_programme_selection_free(s);
    s = select_schedule(records, "2026-10-04T22:00:00Z");
    g_assert_nonnull(s); g_assert_cmpint(s->occurrence->start_utc_us, ==, epoch("2030-01-06T21:00:00Z"));
    radios_weekly_programme_selection_free(s);
    g_assert_null(select_schedule(records, "2031-01-01T00:00:00Z"));
    gchar *after = encode(record); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
    json_array_unref(records); json_object_unref(main);
}
static JsonObject *parse(const gchar *text)
{
    JsonParser *parser = json_parser_new(); g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));
    JsonObject *object = json_object_ref(json_node_get_object(json_parser_get_root(parser))); g_object_unref(parser); return object;
}
static JsonObject *member(JsonObject *o, const gchar *key) { return json_object_get_object_member(o, key); }
/* Kılavuz §04/§06 (akım 4c): the broadcast order projection carries the
 * planner's live blocks and the live/live_step/programme_entry hints. */
static void test_queue_projection_carries_live_hints(void)
{
    JsonObject *config = parse("{\"title\":\"Sabah\",\"blocks\":[{\"id\":\"b-music\",\"kind\":\"music\"},"
        "{\"id\":\"b-live\",\"kind\":\"live\",\"live\":{\"label\":\"talk\",\"channels\":[3]}},{\"id\":\"b-live-2\",\"kind\":\"live\"}],"
        "\"events\":[{\"id\":\"e1\",\"kind\":\"music\",\"live_step\":{\"block_id\":\"b-live\",\"label\":\"talk\",\"outer_order\":3,\"inner_order\":2}},"
        "{\"id\":\"e2\",\"kind\":\"music\"},{\"id\":\"e3\",\"kind\":\"music\"}]}");
    JsonObject *display = parse("{\"entries\":[{\"entry_id\":\"e1\",\"block_id\":\"b-music\",\"block_kind\":\"music\",\"player\":2,\"planned_jingles\":[\"j\"]},"
        "{\"entry_id\":\"e2\",\"block_id\":\"b-new\",\"block_kind\":\"live\",\"block_title\":\"Live event\",\"live\":{\"label\":\"dj\",\"channels\":[1,2]},"
        "\"live_step\":{\"block_id\":\"b-new\",\"inner_order\":1},\"programme_entry\":{\"source_order\":2}}]}");
    JsonObject *programme_live = parse("{\"label\":\"music\",\"channels\":[7],\"automation_id\":\"\",\"end_rule\":\"manual\",\"duration_ms\":0}");
    gchar *config_before = encode(config), *display_before = encode(display);
    JsonNode *result = radios_weekly_programme_queue_projection(config, display, programme_live); g_assert_nonnull(result);
    JsonObject *projection = json_node_get_object(result);
    JsonObject *e1 = at(projection, "events", 0), *e2 = at(projection, "events", 1), *e3 = at(projection, "events", 2);
    g_assert_cmpint(json_object_get_int_member(member(e1, "live_step"), "outer_order"), ==, 3);
    g_assert_cmpint(json_object_get_int_member(e1, "player"), ==, 2);
    g_assert_cmpstr(json_object_get_string_member(e1, "block_id"), ==, "b-music");
    g_assert_true(json_object_has_member(e1, "planned_jingles"));
    g_assert_cmpint(json_object_get_int_member(member(e2, "live_step"), "inner_order"), ==, 1);
    g_assert_cmpstr(json_object_get_string_member(member(e2, "live"), "label"), ==, "dj");
    g_assert_cmpint(json_object_get_int_member(member(e2, "programme_entry"), "source_order"), ==, 2);
    g_assert_cmpstr(json_object_get_string_member(e2, "block_id"), ==, "b-new");
    g_assert_false(json_object_has_member(e3, "live_step")); g_assert_false(json_object_has_member(e3, "player"));
    JsonArray *blocks = json_object_get_array_member(projection, "blocks"); g_assert_cmpuint(json_array_get_length(blocks), ==, 4);
    JsonObject *b_music = json_array_get_object_element(blocks, 0), *b_live = json_array_get_object_element(blocks, 1);
    JsonObject *b_live_2 = json_array_get_object_element(blocks, 2), *b_new = json_array_get_object_element(blocks, 3);
    /* A live block keeps its own live object; one without it inherits the programme's. */
    g_assert_cmpstr(json_object_get_string_member(member(b_live, "live"), "label"), ==, "talk");
    g_assert_cmpstr(json_object_get_string_member(member(b_live_2, "live"), "label"), ==, "music");
    g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(member(b_live_2, "live"), "channels"), 0), ==, 7);
    g_assert_cmpstr(json_object_get_string_member(b_new, "kind"), ==, "live");
    g_assert_cmpstr(json_object_get_string_member(b_new, "title"), ==, "Live event");
    g_assert_cmpstr(json_object_get_string_member(member(b_new, "live"), "label"), ==, "dj");
    g_assert_false(json_object_has_member(b_music, "live"));
    gchar *config_after = encode(config), *display_after = encode(display);
    g_assert_cmpstr(config_before, ==, config_after); g_assert_cmpstr(display_before, ==, display_after);
    json_node_free(result);
    /* Without live steps every block of a live programme carries label/channels only. */
    JsonObject *plain = parse("{\"blocks\":[{\"id\":\"b1\",\"kind\":\"music\"}],\"events\":[]}");
    JsonNode *stamped = radios_weekly_programme_queue_projection(plain, NULL, programme_live);
    JsonObject *b1 = at(json_node_get_object(stamped), "blocks", 0);
    g_assert_cmpstr(json_object_get_string_member(member(b1, "live"), "label"), ==, "music");
    g_assert_false(json_object_has_member(member(b1, "live"), "end_rule")); json_node_free(stamped);
    JsonNode *bare = radios_weekly_programme_queue_projection(plain, NULL, NULL);
    g_assert_false(json_object_has_member(at(json_node_get_object(bare), "blocks", 0), "live")); json_node_free(bare);
    g_assert_null(radios_weekly_programme_queue_projection(NULL, display, programme_live));
    g_free(config_before); g_free(display_before); g_free(config_after); g_free(display_after);
    json_object_unref(config); json_object_unref(display); json_object_unref(programme_live); json_object_unref(plain);
}
#define SCHEMA_A "20000000-0000-4000-8000-00000000000a"
#define SCHEMA_B "20000000-0000-4000-8000-00000000000b"
#define SCHEMA_C "20000000-0000-4000-8000-00000000000c"
/* Kılavuz §03 "programdan programa devam": the governing schema and its
 * programme_entry source order handed to the planner by the general driver. */
static void test_schema_definition_and_programme_entry_order(void)
{
    JsonObject *calendar = parse("{\"schema\":1,\"events\":[],\"rotation_library\":["
        "{\"id\":\"" SCHEMA_A "\",\"name\":\"Müzik\",\"sources\":[],\"settings\":{\"selection_mode\":\"linear\"}},"
        "{\"id\":\"" SCHEMA_B "\",\"name\":\"Konuşma şeması\",\"sources\":[],\"settings\":{\"selection_mode\":\"linear\",\"automation_schema\":true,\"programme_entry\":{\"source_order\":3}}},"
        "{\"id\":\"" SCHEMA_C "\",\"name\":\"Giriş kuralı\",\"sources\":[],\"settings\":{\"selection_mode\":\"linear\",\"programme_entry\":{\"source_order\":5}}}]}");
    JsonObject *event = parse("{\"id\":\"e\",\"rotations\":[{\"id\":\"r1\",\"rotation_id\":\"" SCHEMA_A "\"},{\"id\":\"r2\",\"rotation_id\":\"" SCHEMA_C "\"},{\"id\":\"r3\",\"rotation_id\":\"" SCHEMA_B "\"}]}");
    JsonObject *b = at(calendar, "rotation_library", 1), *c = at(calendar, "rotation_library", 2);
    /* The assigned schema wins over an ordinary definition that has programme_entry. */
    g_assert_true(radios_weekly_programme_schema_definition(calendar, event, NULL) == b);
    g_assert_cmpuint(radios_weekly_programme_entry_order(calendar, event, NULL), ==, 3);
    /* An applied schema id wins; an unknown one falls back. */
    g_assert_true(radios_weekly_programme_schema_definition(calendar, event, SCHEMA_C) == c);
    g_assert_cmpuint(radios_weekly_programme_entry_order(calendar, event, SCHEMA_C), ==, 5);
    g_assert_cmpuint(radios_weekly_programme_entry_order(calendar, event, "ffffffff-ffff-4fff-8fff-000000000000"), ==, 3);
    JsonObject *live_event = parse("{\"id\":\"e2\",\"live\":{\"label\":\"talk\",\"automation_id\":\"" SCHEMA_C "\"},\"rotations\":[{\"id\":\"r\",\"rotation_id\":\"" SCHEMA_A "\"}]}");
    g_assert_cmpuint(radios_weekly_programme_entry_order(calendar, live_event, NULL), ==, 5);
    JsonObject *resolved = parse("{\"id\":\"e3\",\"rotations\":[{\"id\":\"x\",\"calendar_rotation_id\":\"" SCHEMA_B "\",\"calendar_group_id\":\"g\"}]}");
    g_assert_cmpuint(radios_weekly_programme_entry_order(calendar, resolved, NULL), ==, 3);
    JsonObject *plain = parse("{\"id\":\"e4\",\"rotations\":[{\"id\":\"r\",\"rotation_id\":\"" SCHEMA_A "\"}]}");
    g_assert_null(radios_weekly_programme_schema_definition(calendar, plain, NULL));
    g_assert_cmpuint(radios_weekly_programme_entry_order(calendar, plain, NULL), ==, 0);
    json_object_set_int_member(member(member(c, "settings"), "programme_entry"), "source_order", 51);
    g_assert_cmpuint(radios_weekly_programme_entry_order(calendar, event, SCHEMA_C), ==, 0);
    g_assert_cmpuint(radios_weekly_programme_entry_order(NULL, NULL, NULL), ==, 0);
    json_object_unref(calendar); json_object_unref(event); json_object_unref(live_event); json_object_unref(resolved); json_object_unref(plain);
}
/* Contract §1b/§2: events[].live and live/date-window source fields pass the
 * weekly validator when typed; malformed objects are rejected. */
static void test_live_event_fields_validated(void)
{
    JsonObject *c = calendar(); JsonObject *e = event(1, "2026-10-09", 0, 60, TRUE);
    JsonObject *live = parse("{\"label\":\"talk\",\"channels\":[1,2],\"automation_id\":\"\",\"end_rule\":\"duration\",\"duration_ms\":600000}");
    json_object_set_object_member(e, "live", json_object_ref(live)); add(c, e);
    JsonNode *s = snapshot(c, "2026-10-05", FALSE); RadioSWeeklyProgrammeOccurrence *o = next(s, "2026-10-09T00:10:00Z"); g_assert_nonnull(o);
    g_assert_cmpstr(json_object_get_string_member(member(json_node_get_object(o->event), "live"), "label"), ==, "talk");
    radios_weekly_programme_occurrence_free(o); json_node_free(s);
    json_object_set_string_member(live, "label", "host"); invalid_snapshot(c, "2026-10-05"); json_object_set_string_member(live, "label", "talk");
    JsonArray *duplicate = json_array_new(); json_array_add_int_element(duplicate, 1); json_array_add_int_element(duplicate, 1);
    json_object_set_array_member(live, "channels", duplicate); invalid_snapshot(c, "2026-10-05");
    JsonArray *single = json_array_new(); json_array_add_int_element(single, 4); json_object_set_array_member(live, "channels", single);
    json_object_set_int_member(live, "duration_ms", 0); invalid_snapshot(c, "2026-10-05"); json_object_set_int_member(live, "duration_ms", 600000);
    json_object_set_string_member(live, "mode", "x"); invalid_snapshot(c, "2026-10-05"); json_object_remove_member(live, "mode");
    s = snapshot(c, "2026-10-05", FALSE); json_node_free(s);
    JsonObject *d = definition(c, 500, TRUE); JsonObject *row = at(d, "sources", 0);
    json_object_set_string_member(row, "start_date", "2026-10-01"); json_object_set_string_member(row, "end_date", "2026-10-31");
    s = snapshot(c, "2026-10-05", FALSE); json_node_free(s);
    json_object_set_string_member(row, "end_date", "2026-09-30"); invalid_snapshot(c, "2026-10-05"); json_object_remove_member(row, "end_date");
    /* A live step row never carries a playlist. */
    json_object_set_string_member(row, "source_type", "live"); invalid_snapshot(c, "2026-10-05");
    json_object_unref(live); json_object_unref(c);
}
int main(int argc,char **argv)
{
    g_test_init(&argc,&argv,NULL);
    g_test_add_func("/weekly-programme-plan/queue-projection-live-hints", test_queue_projection_carries_live_hints);
    g_test_add_func("/weekly-programme-plan/schema-definition-programme-entry-order", test_schema_definition_and_programme_entry_order);
    g_test_add_func("/weekly-programme-plan/live-event-fields-validated", test_live_event_fields_validated);
    g_test_add_func("/weekly-programme-plan/snapshot-full-deep-copy",test_complete_snapshot_deep_copy_and_anchor);
    g_test_add_func("/weekly-programme-plan/event-probability-policy-accepted",test_event_probability_policy_accepted);
    g_test_add_func("/weekly-programme-plan/current-next-exact-boundaries",test_current_next_exact_time_and_half_open_boundaries);
    g_test_add_func("/weekly-programme-plan/override-schema-joins-main-flow",test_override_schema_joins_main_flow_only_inside_its_slots);
    g_test_add_func("/weekly-programme-plan/multiweek-repeat-all-dates",test_repeat_entire_multiweek_pattern_and_all_dates);
    g_test_add_func("/weekly-programme-plan/leap-negative-before-anchor",test_before_anchor_leap_year_and_negative_epoch);
    g_test_add_func("/weekly-programme-plan/empty-slots-preserved-skipped",test_empty_events_and_empty_library_assignments_preserved_skipped);
    g_test_add_func("/weekly-programme-plan/rules-routing-original-identity",test_assignment_rules_routing_and_original_identity);
    g_test_add_func("/weekly-programme-plan/full-52-limit-no-drop",test_snapshot_over_52_weeks_explicit_error_no_mutation);
    g_test_add_func("/weekly-programme-plan/strict-empty-metadata-types",test_strict_metadata_empty_slots_and_config_type_guards);
    g_test_add_func("/weekly-programme-plan/overlap-identity-reference-unknown",test_overlap_duplicate_reference_and_unknown_rejected);
    g_test_add_func("/weekly-programme-plan/supported-date-range",test_supported_date_limits_and_repeated_clone_range);
    g_test_add_func("/weekly-programme-plan/station-timezone-midnight-DST-repeat", test_station_timezone_midnight_and_dst_repeat);
    g_test_add_func("/weekly-programme-plan/strict-future-active-repeat", test_strict_future_skips_active_and_preserves_repeat);
    g_test_add_func("/weekly-programme-plan/zero-weights-skipped", test_zero_source_and_assignment_weights_are_skipped);
    g_test_add_func("/weekly-programme-plan/mixed-nearest-local-week-main-tie", test_mixed_sources_nearest_local_week_boundary_and_main_tie);
    g_test_add_func("/weekly-programme-plan/mixed-empty-disabled-finite-repeat", test_mixed_empty_disabled_finite_and_explicit_repeat);
    g_test_add_func("/weekly-programme-plan/mixed-identity-and-uuid-tie", test_mixed_record_identity_fences_and_uuid_tie);
    g_test_add_func("/weekly-programme-plan/running-late-boundary-floor", test_running_late_boundary_floor_and_finished_slots);
    g_test_add_func("/weekly-programme-plan/multiyear-main-runtime-slot", test_multiyear_main_preserves_record_and_selects_one_runtime_slot);
    return g_test_run();
}
