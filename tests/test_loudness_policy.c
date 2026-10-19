#include "radios/loudness_policy.h"
#include <string.h>
static const gchar *id = "11111111-1111-4111-8111-111111111111";
static const gchar *other = "22222222-2222-4222-8222-222222222222";
static RadioSAudioLoudnessExecution execution(void)
{ return (RadioSAudioLoudnessExecution){.deck=0,.context=RADIOS_AUDIO_LOUDNESS_MANUAL,.automation_id="",.source=-1,.target=-1}; }
static void defaults(void)
{
    RadioSAudioLoudnessPolicy p; radios_audio_loudness_policy_default(&p);
    g_assert_true(radios_audio_loudness_policy_validate(&p,NULL)); g_assert_true(radios_audio_loudness_policy_unrestricted(&p));
    RadioSAudioLoudnessExecution e=execution();
    for (guint deck=0;deck<66;++deck) for (guint context=1;context<=4;context*=2) {
        e.deck=deck;e.context=context;g_assert_true(radios_audio_loudness_policy_matches(&p,&e,TRUE));
    }
    e.deck=32;p.players=1;g_assert_false(radios_audio_loudness_policy_matches(&p,&e,TRUE));
    g_assert_false(radios_audio_loudness_policy_matches(&p,&e,FALSE));
}
static void intersections(void)
{
    RadioSAudioLoudnessPolicy p;radios_audio_loudness_policy_default(&p);
    p.players=(guint32)1<<31;p.contexts=RADIOS_AUDIO_LOUDNESS_AUTOMATION|RADIOS_AUDIO_LOUDNESS_AUTODJ;
    p.phase=RADIOS_AUDIO_LOUDNESS_PHASE_TRANSITION;p.automation_mode=RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_SELECTED;
    p.automation_count=1;g_strlcpy(p.automation_ids[0],id,37);p.pairs_mode=RADIOS_AUDIO_LOUDNESS_PAIRS_SELECTED;p.pairs[31]=1;
    for(guint mask=0;mask<64;++mask) {
        RadioSAudioLoudnessExecution e={.deck=mask&1?31:1,.context=mask&2?RADIOS_AUDIO_LOUDNESS_AUTODJ:RADIOS_AUDIO_LOUDNESS_MANUAL,
            .transition=!!(mask&4),.automation_id=mask&8?id:other,.source=mask&16?31:0,.target=mask&16?0:31};
        g_assert_cmpint(radios_audio_loudness_policy_matches(&p,&e,!!(mask&32)),==,mask==63);
    }
    RadioSAudioLoudnessExecution e={.deck=0,.context=RADIOS_AUDIO_LOUDNESS_AUTOMATION,.transition=TRUE,.automation_id=id,.source=31,.target=0};
    g_assert_false(radios_audio_loudness_policy_matches(&p,&e,TRUE)); /* Player membership is independent of pair membership. */
    p.players|=1;g_assert_true(radios_audio_loudness_policy_matches(&p,&e,TRUE));
    e.deck=5;p.players|=(guint32)1<<5;g_assert_false(radios_audio_loudness_policy_matches(&p,&e,TRUE));
    p.automation_mode=RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_ASSIGNED;e.deck=0;e.assigned=FALSE;
    g_assert_false(radios_audio_loudness_policy_matches(&p,&e,TRUE));e.assigned=TRUE;g_assert_true(radios_audio_loudness_policy_matches(&p,&e,TRUE));
    e.automation_id="";g_assert_false(radios_audio_loudness_policy_matches(&p,&e,TRUE));
    p.phase=RADIOS_AUDIO_LOUDNESS_PHASE_STEADY;e.automation_id=id;g_assert_false(radios_audio_loudness_policy_matches(&p,&e,TRUE));
    e.transition=FALSE;g_assert_true(radios_audio_loudness_policy_matches(&p,&e,TRUE));
    e.source=0;e.target=31;g_assert_false(radios_audio_loudness_policy_matches(&p,&e,TRUE));
    p.pairs_mode=RADIOS_AUDIO_LOUDNESS_PAIRS_ALL;e.source=e.target=-1;g_assert_true(radios_audio_loudness_policy_matches(&p,&e,TRUE));
    p.contexts=0;g_assert_false(radios_audio_loudness_policy_matches(&p,&e,TRUE));
}
static void windows(void)
{
    RadioSAudioLoudnessPolicy p;radios_audio_loudness_policy_default(&p);p.schedule_enabled=TRUE;
    g_assert_false(radios_audio_loudness_policy_time_matches(&p,1,0));
    p.window_count=1;p.windows[0]=(RadioSAudioLoudnessWindow){1u<<6,22*60,2*60};
    g_assert_false(radios_audio_loudness_policy_time_matches(&p,7,1319));g_assert_true(radios_audio_loudness_policy_time_matches(&p,7,1320));
    g_assert_true(radios_audio_loudness_policy_time_matches(&p,1,119));g_assert_false(radios_audio_loudness_policy_time_matches(&p,1,120));
    g_assert_false(radios_audio_loudness_policy_time_matches(&p,7,119));
    p.windows[0]=(RadioSAudioLoudnessWindow){1,60,60};g_assert_true(radios_audio_loudness_policy_time_matches(&p,1,0));
    g_assert_true(radios_audio_loudness_policy_time_matches(&p,1,1439));g_assert_false(radios_audio_loudness_policy_time_matches(&p,2,0));
    p.windows[0]=(RadioSAudioLoudnessWindow){1,60,120};g_assert_false(radios_audio_loudness_policy_time_matches(&p,1,59));
    g_assert_true(radios_audio_loudness_policy_time_matches(&p,1,60));g_assert_false(radios_audio_loudness_policy_time_matches(&p,1,120));
    p.windows[0].days=0;g_assert_false(radios_audio_loudness_policy_time_matches(&p,1,90));
}
static void json(void)
{
    RadioSAudioLoudnessPolicy p,out;radios_audio_loudness_policy_default(&p);
    p.players=G_MAXUINT32;p.contexts=5;p.phase=RADIOS_AUDIO_LOUDNESS_PHASE_STEADY;
    p.automation_mode=RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_SELECTED;p.automation_count=1;g_strlcpy(p.automation_ids[0],id,37);
    p.pairs_mode=RADIOS_AUDIO_LOUDNESS_PAIRS_SELECTED;p.pairs[31]=1;p.schedule_enabled=TRUE;g_strlcpy(p.timezone,"Europe/Istanbul",128);
    p.window_count=1;p.windows[0]=(RadioSAudioLoudnessWindow){65,1320,120};
    JsonObject *o=radios_audio_loudness_policy_to_json(&p);g_assert_nonnull(o);g_assert_cmpuint(json_object_get_size(o),==,11);
    g_assert_true(radios_audio_loudness_policy_from_json(&out,o,NULL));g_assert_true(radios_audio_loudness_policy_equal(&p,&out));
    RadioSAudioLoudnessPolicy before=out;JsonArray *players=json_object_get_array_member(o,"players");json_array_add_int_element(players,32);
    GError *error=NULL;g_assert_false(radios_audio_loudness_policy_from_json(&out,o,&error));g_assert_error(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA);g_clear_error(&error);
    g_assert_true(radios_audio_loudness_policy_equal(&out,&before));json_object_unref(o);
    const gchar *zones[]={"Not/AZone","+03:00","Europe/../UTC"};
    for(guint i=0;i<G_N_ELEMENTS(zones);++i){g_strlcpy(p.timezone,zones[i],128);g_assert_false(radios_audio_loudness_policy_validate(&p,NULL));}
    g_strlcpy(p.timezone,"UTC",128);p.automation_ids[0][0]='A';g_assert_false(radios_audio_loudness_policy_validate(&p,NULL));
    g_strlcpy(p.automation_ids[0],id,37);p.pairs[31]|=(guint32)1<<31;g_assert_false(radios_audio_loudness_policy_validate(&p,NULL));
    p.pairs[31]=1;p.players=0;p.contexts=0;p.automation_count=0;p.window_count=0;
    o=radios_audio_loudness_policy_to_json(&p);g_assert_true(radios_audio_loudness_policy_from_json(&out,o,NULL));g_assert_true(radios_audio_loudness_policy_equal(&p,&out));json_object_unref(o);
}
int main(int argc,char **argv)
{
    g_test_init(&argc,&argv,NULL);g_test_add_func("/loudness-policy/legacy-defaults-preview-sampler",defaults);
    g_test_add_func("/loudness-policy/player-context-origin-phase-directed-pair-intersection",intersections);
    g_test_add_func("/loudness-policy/week-boundary-overnight-halfopen-whole-day",windows);
    g_test_add_func("/loudness-policy/strict-atomic-json-canonical-identities",json);return g_test_run();
}
