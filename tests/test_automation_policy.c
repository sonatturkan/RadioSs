#include "radios/automation_policy.h"
#include <gio/gio.h>
#include <math.h>
#include <string.h>

static guint next_id;
static gchar *uuid(void)
{ return g_strdup_printf("%08x-1111-4111-8111-111111111111", ++next_id); }
static JsonObject *choice(gdouble probability)
{
    JsonObject *o = json_object_new(); gchar *id = uuid(); json_object_set_string_member(o, "id", id); g_free(id);
    json_object_set_int_member(o, "mode", 0); json_object_set_int_member(o, "overlap_ms", 0);
    if (probability >= 0) json_object_set_double_member(o, "probability", probability);
    return o;
}
static JsonObject *policy(void)
{ JsonObject *o = json_object_new(); json_object_set_int_member(o, "schema", 1); return o; }
static JsonObject *group(const gchar *scope, const gchar *target, JsonObject *child)
{
    JsonObject *g = json_object_new(); gchar *id = uuid(); json_object_set_string_member(g, "id", id); g_free(id);
    json_object_set_string_member(g, "scope", scope); JsonArray *ids = json_array_new();
    if (target) json_array_add_string_element(ids, target);
    json_object_set_array_member(g, "ids", ids);
    if (child) json_object_set_object_member(g, "policy", child);
    return g;
}
static void validates_scopes_and_boundaries(void)
{
    JsonObject *p = policy(); JsonArray *rules = json_array_new();
    json_array_add_object_element(rules, choice(60)); json_array_add_object_element(rules, choice(40));
    json_object_set_array_member(p, "transitions", rules); g_assert_true(radios_automation_policy_validate(p, NULL));
    JsonObject *last = json_array_get_object_element(rules, 1); json_object_set_double_member(last, "probability", 40.001);
    g_assert_false(radios_automation_policy_validate(p, NULL)); json_object_set_double_member(last, "probability", 40);
    json_object_set_boolean_member(last, "probability", TRUE); g_assert_false(radios_automation_policy_validate(p, NULL));
    json_object_set_double_member(last, "probability", NAN); g_assert_false(radios_automation_policy_validate(p, NULL));
    json_object_set_double_member(last, "probability", 40); json_object_set_int_member(last, "mode", 3);
    g_assert_false(radios_automation_policy_validate(p, NULL)); json_object_set_int_member(last, "overlap_ms", 1000);
    g_assert_true(radios_automation_policy_validate(p, NULL)); json_object_unref(p);
    p = policy(); JsonArray *children = json_array_new(); gchar *target = uuid();
    JsonObject *a = group("playlist", target, policy()), *b = group("track", target, policy());
    json_object_set_double_member(a, "probability", 90); json_object_set_double_member(b, "probability", 90);
    json_array_add_object_element(children, a); json_array_add_object_element(children, b); json_object_set_array_member(p, "children", children);
    g_assert_true(radios_automation_policy_validate(p, NULL));
    JsonObject *duplicate = group("playlist", target, policy()); json_array_add_object_element(children, duplicate);
    g_assert_false(radios_automation_policy_validate(p, NULL)); json_array_remove_element(children, 2);
    JsonObject *r1 = group("playlist", NULL, policy()), *r2 = group("playlist", NULL, policy());
    json_array_add_object_element(children, r1); g_assert_true(radios_automation_policy_validate(p, NULL));
    json_array_add_object_element(children, r2); g_assert_false(radios_automation_policy_validate(p, NULL));
    g_free(target); json_object_unref(p);
}
static void inheritance_is_owned_and_specific(void)
{
    gchar *automation = uuid(), *playlist = uuid(), *track = uuid();
    JsonObject *p = policy(); JsonArray *root_rules = json_array_new(); json_array_add_object_element(root_rules, choice(100)); json_object_set_array_member(p, "transitions", root_rules);
    JsonObject *playlist_policy = policy(); JsonArray *playlist_rules = json_array_new(); JsonObject *r = choice(100);
    json_object_set_int_member(r, "mode", 1); json_object_set_int_member(r, "overlap_ms", 2000); json_array_add_object_element(playlist_rules, r); json_object_set_array_member(playlist_policy, "transitions", playlist_rules);
    JsonObject *track_policy = policy(); json_object_set_array_member(track_policy, "transitions", json_array_new());
    JsonArray *track_groups = json_array_new(); json_array_add_object_element(track_groups, group("track", track, track_policy)); json_object_set_array_member(playlist_policy, "children", track_groups);
    JsonArray *playlist_groups = json_array_new(); json_array_add_object_element(playlist_groups, group("playlist", playlist, playlist_policy)); json_object_set_array_member(p, "children", playlist_groups);
    g_assert_true(radios_automation_policy_validate(p, NULL));
    JsonObject *inherited = radios_automation_policy_effective(p, automation, playlist, NULL); g_assert_nonnull(inherited);
    JsonObject *copied = json_array_get_object_element(json_object_get_array_member(inherited, "transitions"), 0);
    g_assert_cmpint(json_object_get_int_member(copied, "mode"), ==, 1); json_object_set_int_member(copied, "mode", 0);
    g_assert_cmpint(json_object_get_int_member(r, "mode"), ==, 1); json_object_unref(inherited);
    JsonObject *specific = radios_automation_policy_effective(p, automation, playlist, track);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(specific, "transitions")), ==, 0);
    json_object_unref(specific); json_object_unref(p); g_free(automation); g_free(playlist); g_free(track);
}
static void seeded_percentages_and_linear_remainder(void)
{
    JsonArray *rules = json_array_new(); json_array_add_object_element(rules, choice(0)); json_array_add_object_element(rules, choice(35));
    json_array_add_object_element(rules, choice(-1)); json_array_add_object_element(rules, choice(-1));
    RadioSAutomationPolicyState *s = radios_automation_policy_state_new("native genuine deterministic selector", NULL, NULL); g_assert_nonnull(s);
    guint counts[4] = {0}; gint previous = -1;
    for (guint i = 0; i < 20000; ++i) {
        gint selected = radios_automation_policy_choose(rules, NULL, "group:one", s, G_GINT64_CONSTANT(1700000000000000) + i);
        g_assert_cmpint(selected, >=, 1); ++counts[selected];
        if (selected >= 2) { if (previous >= 2) g_assert_cmpint(selected, !=, previous); previous = selected; }
    }
    g_assert_cmpuint(counts[0], ==, 0); g_assert_cmpfloat(fabs(counts[1] / 20000.0 - .35), <, .015);
    g_assert_cmpint(ABS((gint)counts[2] - (gint)counts[3]), <=, 1);
    gboolean eligible[] = {FALSE, FALSE, TRUE, TRUE};
    for (guint i = 0; i < 100; ++i) g_assert_cmpint(radios_automation_policy_choose(rules, eligible, "group:filtered", s, i), ==, (gint)(i % 2 + 2));
    radios_automation_policy_state_free(s); json_array_unref(rules);
}
static void checkpoint_replays_the_next_decision(void)
{
    JsonArray *rules = json_array_new(); json_array_add_object_element(rules, choice(25)); json_array_add_object_element(rules, choice(-1));
    RadioSAutomationPolicyState *a = radios_automation_policy_state_new("checkpoint seed", NULL, NULL);
    for (guint i = 0; i < 73; ++i) radios_automation_policy_choose(rules, NULL, "persistent:group", a, i);
    JsonObject *snapshot = radios_automation_policy_state_snapshot(a); RadioSAutomationPolicyState *b = radios_automation_policy_state_new("changed seed is overridden by consumed checkpoint", snapshot, NULL); g_assert_nonnull(b);
    for (guint i = 73; i < 1000; ++i) g_assert_cmpint(radios_automation_policy_choose(rules, NULL, "persistent:group", a, i), ==, radios_automation_policy_choose(rules, NULL, "persistent:group", b, i));
    json_object_unref(snapshot); radios_automation_policy_state_free(a); radios_automation_policy_state_free(b); json_array_unref(rules);
}
static void frequency_interval_and_UTC_exclusions(void)
{
    JsonArray *rules = json_array_new(); JsonObject *r = choice(-1); json_object_set_int_member(r, "frequency", 2); json_array_add_object_element(rules, r);
    RadioSAutomationPolicyState *s = radios_automation_policy_state_new("gates", NULL, NULL);
    g_assert_cmpint(radios_automation_policy_choose(rules, NULL, "frequency", s, 0), ==, -1);
    g_assert_cmpint(radios_automation_policy_choose(rules, NULL, "frequency", s, 0), ==, 0);
    json_object_set_int_member(r, "frequency", 1); json_object_set_int_member(r, "interval_minutes", 2);
    g_assert_cmpint(radios_automation_policy_choose(rules, NULL, "interval", s, 0), ==, 0);
    g_assert_cmpint(radios_automation_policy_choose(rules, NULL, "interval", s, G_TIME_SPAN_MINUTE), ==, -1);
    g_assert_cmpint(radios_automation_policy_choose(rules, NULL, "interval", s, 2 * G_TIME_SPAN_MINUTE), ==, 0);
    JsonObject *window = json_object_new(); json_object_set_int_member(window, "start_minute", 22 * 60); json_object_set_int_member(window, "end_minute", 2 * 60);
    JsonArray *excluded = json_array_new(); json_array_add_object_element(excluded, window); json_object_set_array_member(r, "excluded_times", excluded);
    g_assert_cmpint(radios_automation_policy_choose(rules, NULL, "UTC", s, 23 * G_TIME_SPAN_HOUR), ==, -1);
    g_assert_cmpint(radios_automation_policy_choose(rules, NULL, "UTC", s, 12 * G_TIME_SPAN_HOUR), ==, 0);
    json_object_set_int_member(window, "start_minute", 0); json_object_set_int_member(window, "end_minute", 0);
    g_assert_cmpint(radios_automation_policy_choose(rules, NULL, "UTC", s, 24 * G_TIME_SPAN_HOUR), ==, -1);
    /* Station time: 02:00-03:00 Istanbul (UTC+3) is 23:00-24:00 UTC. */
    json_object_set_int_member(window, "start_minute", 2 * 60); json_object_set_int_member(window, "end_minute", 3 * 60);
    json_object_set_string_member(r, "timezone", "Europe/Istanbul");
    JsonObject *root = json_object_new(); json_object_set_int_member(root, "schema", 1); json_object_set_array_member(root, "transitions", json_array_ref(rules));
    g_assert_true(radios_automation_policy_validate(root, NULL));
    gint64 day = G_GINT64_CONSTANT(1792022400) * G_USEC_PER_SEC; /* 2026-10-15 00:00 UTC */
    g_assert_cmpint(radios_automation_policy_choose(rules, NULL, "local", s, day - 30 * G_TIME_SPAN_MINUTE), ==, -1);
    g_assert_cmpint(radios_automation_policy_choose(rules, NULL, "local", s, day + 2 * G_TIME_SPAN_HOUR + 30 * G_TIME_SPAN_MINUTE), ==, 0);
    json_object_set_string_member(r, "timezone", "Mars/Olympus"); g_assert_false(radios_automation_policy_validate(root, NULL));
    json_object_set_int_member(r, "timezone", 3); g_assert_false(radios_automation_policy_validate(root, NULL));
    json_object_remove_member(r, "timezone"); g_assert_true(radios_automation_policy_validate(root, NULL)); json_object_unref(root);
    radios_automation_policy_state_free(s); json_array_unref(rules);
}
static void canonical_targets_large_refresh_and_scope_overlay(void)
{
    RadioSAutomationPolicyState *s=radios_automation_policy_state_new("long-lived",NULL,NULL);
    for (guint batch=0;batch<10;++batch) {
        gchar *ids[500];for (guint j=0;j<500;++j) ids[j]=g_strdup_printf("%08x-1111-4111-8111-111111111111",70000+batch*500+j);
        for (guint j=0;j<500;++j) g_assert_cmpint(radios_automation_policy_choose_targets(NULL,"track",(const gchar *const *)ids,NULL,500,"refresh",s,j),==,(gint)j);
        for (guint j=0;j<500;++j) g_free(ids[j]);
    }
    JsonObject *checkpoint=radios_automation_policy_state_snapshot(s);
    g_assert_cmpuint(json_object_get_size(json_object_get_object_member(checkpoint,"rules")),==,0);
    RadioSAutomationPolicyState *restored=radios_automation_policy_state_new("different",checkpoint,NULL);g_assert_nonnull(restored);
    gchar *target=uuid();JsonObject *p=policy(),*child=policy();JsonArray *rules=json_array_new(),*groups=json_array_new();
    json_array_add_object_element(rules,choice(100));json_object_set_array_member(child,"transitions",rules);
    json_array_add_object_element(groups,group("playlist",target,child));json_object_set_array_member(p,"children",groups);
    g_assert_true(radios_automation_policy_validate(p,NULL));
    JsonObject *context=radios_automation_policy_context(p,"playlist",target);g_assert_nonnull(context);
    g_assert_true(radios_automation_policy_validate(context,NULL));
    JsonObject *track=radios_automation_policy_context(context,"track",target);g_assert_nonnull(track);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(track,"transitions")),==,1);
    json_object_unref(track);json_object_unref(context);json_object_unref(p);g_free(target);
    radios_automation_policy_state_free(restored);radios_automation_policy_state_free(s);json_object_unref(checkpoint);
}
/* Every playlist has a percentage (they sum to 100) and one is not eligible
 * now, e.g. a sweeper right after a sweeper: a draw falling into its share
 * must go to the eligible playlists in proportion, never end the queue. */
static void unavailable_share_goes_to_available_percentages(void)
{
    gchar *ids[3] = {uuid(), uuid(), uuid()}; const gdouble shares[3] = {60, 30, 10};
    JsonObject *p = policy(); JsonArray *groups = json_array_new();
    for (guint i = 0; i < 3; ++i) {
        JsonObject *g = group("playlist", ids[i], NULL); json_object_set_double_member(g, "probability", shares[i]);
        json_array_add_object_element(groups, g);
    }
    json_object_set_array_member(p, "children", groups); g_assert_true(radios_automation_policy_validate(p, NULL));
    RadioSAutomationPolicyState *s = radios_automation_policy_state_new("continuity", NULL, NULL);
    gboolean eligible[3] = {FALSE, TRUE, TRUE}; guint counts[3] = {0};
    for (guint i = 0; i < 6000; ++i) {
        gint selected = radios_automation_policy_choose_targets(p, "playlist", (const gchar *const *)ids, eligible, 3, "calendar/x/playlist", s, i);
        g_assert_cmpint(selected, >=, 1); ++counts[selected];
    }
    g_assert_cmpfloat(fabs(counts[1] / 6000.0 - .75), <, .03);
    /* Only candidates that are actually offered: the absent one keeps no share. */
    for (guint i = 0; i < 200; ++i)
        g_assert_cmpint(radios_automation_policy_choose_targets(p, "playlist", (const gchar *const *)&ids[2], NULL, 1, "calendar/x/single", s, i), ==, 0);
    radios_automation_policy_state_free(s); json_object_unref(p);
    for (guint i = 0; i < 3; ++i) g_free(ids[i]);
}
/* Jingle and transition percentages may sum below 100: the remainder means
 * "none" and must stay so, even while one rule is unavailable. */
static void jingle_remainder_still_means_none(void)
{
    JsonArray *rules = json_array_new(); json_array_add_object_element(rules, choice(20)); json_array_add_object_element(rules, choice(25));
    RadioSAutomationPolicyState *s = radios_automation_policy_state_new("jingle remainder", NULL, NULL);
    guint none = 0, picked[2] = {0};
    for (guint i = 0; i < 8000; ++i) {
        gint selected = radios_automation_policy_choose(rules, NULL, "jingles:remainder", s, i);
        if (selected < 0) ++none; else ++picked[selected];
    }
    g_assert_cmpfloat(fabs(none / 8000.0 - .55), <, .03);
    g_assert_cmpfloat(fabs(picked[0] / 8000.0 - .20), <, .03);
    gboolean eligible[] = {FALSE, TRUE}; none = 0;
    for (guint i = 0; i < 8000; ++i) {
        gint selected = radios_automation_policy_choose(rules, eligible, "jingles:one-unavailable", s, i);
        g_assert_cmpint(selected, !=, 0); none += selected < 0;
    }
    g_assert_cmpfloat(fabs(none / 8000.0 - .75), <, .03);
    radios_automation_policy_state_free(s); json_array_unref(rules);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/automation-policy/strict-scope-percent-boundaries", validates_scopes_and_boundaries);
    g_test_add_func("/automation-policy/inheritance-owned-specific-empty", inheritance_is_owned_and_specific);
    g_test_add_func("/automation-policy/seeded-percent-linear-rest", seeded_percentages_and_linear_remainder);
    g_test_add_func("/automation-policy/consumed-checkpoint-replay", checkpoint_replays_the_next_decision);
    g_test_add_func("/automation-policy/frequency-interval-UTC-exclusions", frequency_interval_and_UTC_exclusions);
    g_test_add_func("/automation-policy/large-refresh-canonical-target-scope-overlay",canonical_targets_large_refresh_and_scope_overlay);
    g_test_add_func("/automation-policy/unavailable-share-to-available-percentages", unavailable_share_goes_to_available_percentages);
    g_test_add_func("/automation-policy/jingle-remainder-still-means-none", jingle_remainder_still_means_none);
    return g_test_run();
}
