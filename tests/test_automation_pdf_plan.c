#define main radios_pdf_legacy_calendar_plan_main
#include "test_automation_calendar_plan.c"
#undef main
#include "radios/automation_policy.h"

static JsonObject *pdf_policy(void)
{
    JsonObject *p = json_object_new(); json_object_set_int_member(p, "schema", 1);
    json_object_set_array_member(p, "children", json_array_new()); return p;
}
static JsonObject *pdf_group(JsonObject *parent, const gchar *scope, guint id, const guint *targets, guint count, gint probability)
{
    JsonObject *g = json_object_new(); gchar *name = uuid(id); json_object_set_string_member(g, "id", name); g_free(name);
    json_object_set_string_member(g, "scope", scope); JsonArray *ids = json_array_new();
    for (guint i = 0; i < count; ++i) { gchar *target = uuid(targets[i]); json_array_add_string_element(ids, target); g_free(target); }
    json_object_set_array_member(g, "ids", ids); if (probability >= 0) json_object_set_int_member(g, "probability", probability);
    json_array_add_object_element(json_object_get_array_member(parent, "children"), g); return g;
}
static JsonObject *pdf_fixture(JsonObject **event)
{
    *event = event_new(); JsonObject *prepared = prepared_new();
    JsonObject *a = add_source(*event, prepared, 1, 1), *b = add_source(*event, prepared, 2, 1);
    json_array_add_object_element(json_object_get_array_member(a, "tracks"), row(1, "music"));
    json_array_add_object_element(json_object_get_array_member(a, "tracks"), row(2, "music"));
    json_array_add_object_element(json_object_get_array_member(b, "tracks"), row(3, "music"));
    json_array_add_object_element(json_object_get_array_member(b, "tracks"), row(4, "music")); return prepared;
}
static JsonNode *pdf_generate(JsonObject *event, JsonObject *prepared, guint count)
{
    GError *error = NULL; JsonNode *out = radios_automation_calendar_prepared_programme(event, prepared, station, "pdf-policy-seed", count, &error);
    g_assert_no_error(error); g_assert_nonnull(out); return out;
}
static void pdf_assert_identity(JsonNode *out, guint index, guint number)
{
    JsonObject *detail = json_object_get_object_member(at(json_node_get_object(out), "tracks", index), "detail");
    gchar *id = uuid(number + 10000); g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(detail, "media"), "_id"), ==, id); g_free(id);
}
static void test_pdf_undefined_linear_and_original_context(void)
{
    JsonObject *event, *prepared = pdf_fixture(&event); json_object_set_object_member(event, "probability_policy", pdf_policy());
    gchar *before = serialized(prepared); JsonNode *out = pdf_generate(event, prepared, 8); const guint sequence[] = {1, 3, 2, 4, 1, 3, 2, 4};
    for (guint i = 0; i < G_N_ELEMENTS(sequence); ++i) {
        pdf_assert_identity(out, i, sequence[i]); JsonObject *track = at(json_node_get_object(out), "tracks", i);
        JsonObject *context = json_object_get_object_member(track, "policy_context"); gchar *origin = uuid(sequence[i]);
        g_assert_cmpstr(json_object_get_string_member(context, "entry_id"), ==, origin);
        g_assert_cmpstr(json_object_get_string_member(track, "entry_id"), !=, origin); g_free(origin);
        g_assert_true(json_object_has_member(track, "policy_selection_checkpoint"));
        g_assert_false(json_object_has_member(at(json_object_get_object_member(json_node_get_object(out), "config"), "events", i), "policy_selection_checkpoint"));
    }
    gchar *after = serialized(prepared); g_assert_cmpstr(before, ==, after);
    g_free(before); g_free(after); json_node_free(out); json_object_unref(event); json_object_unref(prepared);
}
static void test_pdf_selected_cross_playlist_tracks_and_remainder(void)
{
    JsonObject *event, *prepared = pdf_fixture(&event), *policy = pdf_policy();
    const guint selected[] = {1, 3}; pdf_group(policy, "track", 60001, selected, 2, 100); pdf_group(policy, "track", 60002, NULL, 0, 0);
    json_object_set_object_member(event, "probability_policy", policy); JsonNode *out = pdf_generate(event, prepared, 12), *again = pdf_generate(event, prepared, 12);
    for (guint i = 0; i < 12; ++i) { pdf_assert_identity(out, i, i % 2 ? 3 : 1); pdf_assert_identity(again, i, i % 2 ? 3 : 1); }
    json_node_free(out); json_node_free(again); json_object_unref(event); json_object_unref(prepared);
}
static void test_pdf_scoped_playlist_song_inheritance_and_zero(void)
{
    JsonObject *event, *prepared = pdf_fixture(&event), *policy = pdf_policy(); const guint playlist[] = {2001}, song[] = {2};
    JsonObject *group = pdf_group(policy, "playlist", 60003, playlist, 1, 100), *child = pdf_policy();
    pdf_group(child, "track", 60004, song, 1, 100); pdf_group(child, "track", 60005, NULL, 0, 0);
    json_object_set_object_member(group, "policy", child); pdf_group(policy, "playlist", 60006, NULL, 0, 0);
    json_object_set_object_member(event, "probability_policy", policy); JsonNode *out = pdf_generate(event, prepared, 10);
    for (guint i = 0; i < 10; ++i) pdf_assert_identity(out, i, 2);
    json_node_free(out);
    /* Media eligibility cannot be overridden by an explicit 100% rule. */
    json_object_set_boolean_member(at(at(prepared, "sources", 0), "tracks", 1), "eligible", FALSE);
    GError *error = NULL; out = radios_automation_calendar_prepared_programme(event, prepared, station, "pdf-policy-seed", 10, &error);
    g_assert_null(out); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    json_object_unref(event); json_object_unref(prepared);
}
static void test_pdf_current_playlist_first_and_consumption_resume(void)
{
    JsonObject *event, *prepared = pdf_fixture(&event), *policy = pdf_policy(); const guint playlist[] = {2001};
    pdf_group(policy, "playlist", 60007, playlist, 1, 100); pdf_group(policy, "playlist", 60008, NULL, 0, 0);
    json_object_set_object_member(event, "probability_policy", policy); gchar *id = uuid(2002);
    json_object_set_string_member(event, "schema_first_playlist_id", id); g_free(id); JsonNode *out = pdf_generate(event, prepared, 3);
    pdf_assert_identity(out, 0, 3); pdf_assert_identity(out, 1, 1); pdf_assert_identity(out, 2, 2); json_node_free(out);
    /* Undefined probability keeps weights/linear order; explicit remainder
     * groups at every scope select the probability path, whose consumed
     * checkpoint resumes exactly where the previous batch stopped. */
    json_object_remove_member(event, "schema_first_playlist_id");
    JsonObject *root = pdf_policy(), *automation = pdf_group(root, "automation", 60010, NULL, 0, -1), *nested = pdf_policy();
    JsonObject *songs = pdf_policy(), *playlists = pdf_group(nested, "playlist", 60011, NULL, 0, -1);
    pdf_group(songs, "track", 60012, NULL, 0, -1); json_object_set_object_member(playlists, "policy", songs);
    json_object_set_object_member(automation, "policy", nested); json_object_set_object_member(event, "probability_policy", root);
    out = pdf_generate(event, prepared, 4); JsonObject *checkpoint = json_object_get_object_member(at(json_node_get_object(out), "tracks", 0), "policy_selection_checkpoint");
    JsonNode *copy = json_node_new(JSON_NODE_OBJECT); json_node_set_object(copy, checkpoint);
    json_object_set_member(prepared, "policy_selection_checkpoint", json_node_copy(copy)); json_node_free(copy);
    JsonNode *resumed = pdf_generate(event, prepared, 3);
    for (guint i = 0; i < 3; ++i) {
        JsonObject *expected = json_object_get_object_member(json_object_get_object_member(at(json_node_get_object(out), "tracks", i + 1), "detail"), "media");
        JsonObject *actual = json_object_get_object_member(json_object_get_object_member(at(json_node_get_object(resumed), "tracks", i), "detail"), "media");
        g_assert_cmpstr(json_object_get_string_member(actual, "_id"), ==, json_object_get_string_member(expected, "_id"));
    }
    json_node_free(out); json_node_free(resumed); json_object_unref(event); json_object_unref(prepared);
}
static void test_pdf_partial_source_override_keeps_parent_jingle_catalogue(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition), *parent = pdf_policy();
    JsonArray *jingles = json_array_new(); JsonObject *jingle = json_object_new();
    gchar *id = uuid(62000), *playlist = uuid(2003);
    json_object_set_string_member(jingle, "id", id); json_object_set_string_member(jingle, "playlist_id", playlist);
    json_object_set_int_member(jingle, "player", 31); json_object_set_string_member(jingle, "anchor", "after_start");
    json_object_set_double_member(jingle, "seconds", 10); json_array_add_object_element(jingles, jingle);
    json_object_set_array_member(parent, "jingles", jingles);
    json_object_set_object_member(json_object_get_object_member(definition, "settings"), "probability_policy", parent);
    JsonObject *override = json_object_new(), *child = pdf_policy(); json_object_set_array_member(child, "transitions", json_array_new());
    json_object_set_object_member(override, "probability_policy", child); json_object_set_object_member(at(definition, "sources", 0), "settings", override);
    JsonNode *resolved = resolve(calendar, event); GError *error = NULL;
    JsonNode *request = radios_automation_calendar_prepare_config(json_node_get_object(resolved), NULL, &error); g_assert_no_error(error); g_assert_nonnull(request);
    JsonObject *policy = json_object_get_object_member(json_object_get_object_member(at(json_node_get_object(resolved), "rotations", 0), "calendar_settings"), "probability_policy");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(policy, "transitions")), ==, 0);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(policy, "jingles")), ==, 1);
    JsonObject *hint = at(json_node_get_object(request), "probability_source_policies", 0);
    g_assert_cmpstr(json_object_get_string_member(at(hint, "jingles", 0), "playlist_id"), ==, playlist);
    g_assert_cmpstr(json_object_get_string_member(at(parent, "jingles", 0), "id"), ==, id);
    g_free(id); g_free(playlist); json_node_free(request); json_node_free(resolved); json_object_unref(calendar);
}
static void test_pdf_recursive_scope_rules_reach_programme(void)
{
    JsonObject *event, *prepared = pdf_fixture(&event), *root = pdf_policy(); const guint playlist_ids[] = {2001}, song_ids[] = {2};
    JsonObject *playlist_group = pdf_group(root, "playlist", 63000, playlist_ids, 1, 100), *playlist_policy = pdf_policy();
    JsonArray *transitions = json_array_new(); JsonObject *transition = json_object_new(); gchar *transition_id = uuid(63001);
    json_object_set_string_member(transition, "id", transition_id); g_free(transition_id);
    json_object_set_int_member(transition, "mode", 1); json_object_set_int_member(transition, "probability", 100);
    json_object_set_int_member(transition, "overlap_ms", 100); json_array_add_object_element(transitions, transition);
    json_object_set_array_member(playlist_policy, "transitions", transitions);
    JsonObject *track_group = pdf_group(playlist_policy, "track", 63002, song_ids, 1, 100), *track_policy = pdf_policy();
    JsonArray *jingles = json_array_new(); JsonObject *jingle = json_object_new(); gchar *jingle_id = uuid(63003), *voice_playlist = uuid(2003);
    json_object_set_string_member(jingle, "id", jingle_id); json_object_set_string_member(jingle, "playlist_id", voice_playlist);
    json_object_set_int_member(jingle, "player", 31); json_object_set_string_member(jingle, "anchor", "before_end"); json_object_set_double_member(jingle, "seconds", .5);
    json_array_add_object_element(jingles, jingle); json_object_set_array_member(track_policy, "jingles", jingles);
    json_object_set_object_member(track_group, "policy", track_policy); pdf_group(playlist_policy, "track", 63004, NULL, 0, 0);
    json_object_set_object_member(playlist_group, "policy", playlist_policy); pdf_group(root, "playlist", 63005, NULL, 0, 0);
    json_object_set_object_member(event, "probability_policy", root); JsonNode *out = pdf_generate(event, prepared, 4);
    for (guint i = 0; i < 4; ++i) {
        pdf_assert_identity(out, i, 2); JsonObject *row = at(json_object_get_object_member(json_node_get_object(out), "config"), "events", i);
        JsonObject *policy = json_object_get_object_member(row, "probability_policy"); g_assert_true(radios_automation_policy_validate(policy, NULL));
        g_assert_cmpuint(json_array_get_length(json_object_get_array_member(policy, "transitions")), ==, 1);
        g_assert_cmpstr(json_object_get_string_member(at(policy, "jingles", 0), "id"), ==, jingle_id);
        g_assert_false(json_object_has_member(policy, "children"));
    }
    g_free(jingle_id); g_free(voice_playlist); json_node_free(out); json_object_unref(event); json_object_unref(prepared);
}
static void test_probability_remainder_never_reopens_zero_tracks(void)
{
    JsonObject *event, *prepared = pdf_fixture(&event), *policy = pdf_policy();
    const guint disabled[] = {1, 3}, enabled[] = {2, 4};
    pdf_group(policy, "track", 64000, disabled, G_N_ELEMENTS(disabled), 0);
    pdf_group(policy, "track", 64001, enabled, G_N_ELEMENTS(enabled), 1);
    pdf_group(policy, "track", 64002, NULL, 0, 0);
    json_object_set_object_member(event, "probability_policy", policy);
    JsonNode *out = pdf_generate(event, prepared, 20);
    for (guint i = 0; i < 20; ++i) pdf_assert_identity(out, i, i % 2 ? 4 : 2);
    json_node_free(out); json_object_unref(event); json_object_unref(prepared);
}
static void test_probability_consumption_checkpoint_rejects_invalid_state(void)
{
    JsonObject *event, *prepared = pdf_fixture(&event);
    json_object_set_object_member(event, "probability_policy", pdf_policy());
    json_object_set_object_member(prepared, "policy_selection_checkpoint", json_object_new());
    GError *error = NULL;
    JsonNode *out = radios_automation_calendar_prepared_programme(event, prepared, station, "pdf-policy-seed", 4, &error);
    g_assert_null(out); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error); json_object_unref(event); json_object_unref(prepared);
}
static void test_pdf_local_date_utc_anchor_across_midnight(void)
{
    JsonObject *event, *prepared = pdf_fixture(&event);
    json_object_set_string_member(event, "date", "2026-10-14"); json_object_set_string_member(event, "timezone", "Europe/Istanbul");
    json_object_set_int_member(event, "start_minute", 60); json_object_set_int_member(event, "end_minute", 120);
    JsonNode *out = pdf_generate(event, prepared, 3); JsonObject *config = json_object_get_object_member(json_node_get_object(out), "config");
    g_assert_cmpstr(json_object_get_string_member(event, "date"), ==, "2026-10-14");
    g_assert_cmpstr(json_object_get_string_member(config, "date"), ==, "2026-10-13");
    JsonObject *timing = json_object_get_object_member(config, "timing"); g_assert_cmpstr(json_object_get_string_member(timing, "target_time"), ==, "22:00");
    GError *error = NULL; gint64 start = radios_automation_calendar_event_time(event, 60);
    RadioSPlayoutRequest request = {.station_id = station, .database_file = database, .now_utc_us = start, .start_utc_us = start};
    RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(out), &request, &error); g_assert_no_error(error); g_assert_nonnull(plan);
    g_assert_cmpint(((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, 0))->planned_utc_us, ==, start);
    radios_playout_plan_free(plan); json_node_free(out); json_object_unref(event); json_object_unref(prepared);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/automation-pdf/undefined-linear-origin-occurrence", test_pdf_undefined_linear_and_original_context);
    g_test_add_func("/automation-pdf/cross-playlist-track-sets-remainder", test_pdf_selected_cross_playlist_tracks_and_remainder);
    g_test_add_func("/automation-pdf/playlist-song-inheritance-eligibility-zero", test_pdf_scoped_playlist_song_inheritance_and_zero);
    g_test_add_func("/automation-pdf/schema-current-first-consumed-checkpoint", test_pdf_current_playlist_first_and_consumption_resume);
    g_test_add_func("/automation-pdf/source-partial-override-keeps-parent-jingle", test_pdf_partial_source_override_keeps_parent_jingle_catalogue);
    g_test_add_func("/automation-pdf/recursive-scope-rules-reach-authenticated-programme", test_pdf_recursive_scope_rules_reach_programme);
    g_test_add_func("/automation-pdf/local-date-UTC-anchor-crosses-midnight", test_pdf_local_date_utc_anchor_across_midnight);
    g_test_add_func("/automation-pdf/remainder-never-reopens-zero-tracks", test_probability_remainder_never_reopens_zero_tracks);
    g_test_add_func("/automation-pdf/consumption-checkpoint-rejects-invalid-state", test_probability_consumption_checkpoint_rejects_invalid_state);
    return g_test_run();
}
