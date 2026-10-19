#include "radios/playout.h"
#include "radios/transition_preset.h"
#include "radios/automation_calendar_plan.h"
#include "radios/copyright_policy.h"
#include <string.h>

static JsonNode *song_copy(JsonObject *object);
static gchar *database;
static const gchar *station = "e0000000-0000-4000-8000-000000000001";
static gchar *uuid(guint id) { return g_strdup_printf("00000000-0000-4000-8000-%012u", id); }
static gint64 stamp(const gchar *text)
{ GDateTime *dt = g_date_time_new_from_iso8601(text, NULL); g_assert_nonnull(dt); gint64 time = g_date_time_to_unix(dt) * G_USEC_PER_SEC; g_date_time_unref(dt); return time; }
static JsonObject *row(guint id, const gchar *artist)
{
    gchar *entry = uuid(id), *media_id = uuid(id + 100), *version_id = uuid(id + 200), *file_id = uuid(id + 300), *item_id = uuid(id + 400);
    JsonObject *row = json_object_new(), *detail = json_object_new(), *media = json_object_new(), *version = json_object_new(), *analysis = json_object_new(), *metadata = json_object_new();
    gchar sha[65]; memset(sha, 'a', 64); sha[64] = 0; gchar *path = g_strdup_printf("%s.objects/%s.wav", database, media_id);
    json_object_set_string_member(row, "entry_id", entry); json_object_set_int_member(row, "ordinal", id); json_object_set_boolean_member(row, "eligible", TRUE);
    json_object_set_string_member(media, "_id", media_id); json_object_set_string_member(media, "station_id", station); json_object_set_string_member(media, "version_id", version_id);
    json_object_set_string_member(media, "media_file_id", file_id); json_object_set_string_member(media, "item_id", item_id);
    json_object_set_string_member(version, "_id", version_id); json_object_set_string_member(version, "item_id", item_id); json_object_set_string_member(version, "media_file_id", file_id);
    json_object_set_string_member(version, "sha256", sha); json_object_set_string_member(version, "managed_path", path); json_object_set_int_member(version, "duration_ms", 6001);
    json_object_set_boolean_member(version, "verified", TRUE); json_object_set_boolean_member(version, "object_available", TRUE);
    json_object_set_string_member(analysis, "sha256", sha); json_object_set_int_member(analysis, "sample_rate", 48000); json_object_set_int_member(analysis, "sample_frames", 288031);
    json_object_set_string_member(metadata, "title", entry); json_object_set_string_member(metadata, "artist", artist); json_object_set_string_member(metadata, "album", "Albüm");
    json_object_set_string_member(metadata, "kind", "music"); json_object_set_int_member(metadata, "duration_ms", 999999);
    json_object_set_object_member(detail, "media", media); json_object_set_object_member(detail, "version", version); json_object_set_object_member(detail, "analysis", analysis);
    json_object_set_object_member(row, "detail", detail); json_object_set_object_member(row, "metadata", metadata);
    g_free(entry); g_free(media_id); g_free(version_id); g_free(file_id); g_free(item_id); g_free(path); return row;
}
static JsonObject *prepared(const gchar *kind)
{
    JsonObject *p = json_object_new(); json_object_set_int_member(p, "contract_version", 1); json_object_set_string_member(p, "station_id", station);
    json_object_set_string_member(p, "kind", kind); json_object_set_string_member(p, "timezone", "UTC");
    json_object_set_array_member(p, "tracks", json_array_new()); json_object_set_array_member(p, "sources", json_array_new()); return p;
}
static RadioSPlayoutRequest request(void)
{ return (RadioSPlayoutRequest){.station_id = station, .database_file = database, .now_utc_us = stamp("2026-10-06T00:30:00Z")}; }
static RadioSPlayoutPlan *build(JsonObject *p, RadioSPlayoutRequest *r)
{ GError *error = NULL; RadioSPlayoutPlan *plan = radios_playout_plan_build(p, r, &error); g_assert_no_error(error); g_assert_nonnull(plan); return plan; }
static void rejected(JsonObject *p, RadioSPlayoutRequest *r)
{ GError *error = NULL; g_assert_null(radios_playout_plan_build(p, r, &error)); g_assert_nonnull(error); g_clear_error(&error); }
static RadioSPlayoutEntry *entry(RadioSPlayoutPlan *p, guint i) { return g_ptr_array_index(p->entries, i); }
static JsonObject *configuration(JsonObject *p, guint count)
{
    JsonObject *c = json_object_new(); json_object_set_string_member(c, "name", "Test"); json_object_set_string_member(c, "seed", "repeatable-1");
    json_object_set_string_member(c, "planDate", "2026-10-06"); json_object_set_string_member(c, "startTime", "00:30");
    json_object_set_string_member(c, "windowStart", "00:00"); json_object_set_string_member(c, "windowEnd", "00:00");
    json_object_set_int_member(c, "sourceMode", 0); json_object_set_int_member(c, "trackMode", 1); json_object_set_int_member(c, "transition", 0);
    json_object_set_int_member(c, "boundaryTransition", 0); json_object_set_int_member(c, "fallbackMode", 0); json_object_set_int_member(c, "deckMode", 0);
    json_object_set_int_member(c, "previewCount", count); json_object_set_int_member(c, "trackGap", 0); json_object_set_int_member(c, "artistGap", 0); json_object_set_int_member(c, "albumGap", 0);
    json_object_set_int_member(c, "jingleEvery", 1); json_object_set_boolean_member(c, "jingleEnabled", FALSE); json_object_set_boolean_member(c, "jingleDry", TRUE);
    json_object_set_double_member(c, "overlap", 0.5); json_object_set_double_member(c, "boundaryOverlap", 0.25);
    JsonArray *days = json_array_new(), *decks = json_array_new(); for (guint i = 1; i <= 7; ++i) json_array_add_int_element(days, i);
    json_array_add_string_element(decks, "1"); json_array_add_string_element(decks, "2");
    json_object_set_array_member(c, "weekdays", days); json_object_set_array_member(c, "decks", decks); json_object_set_array_member(c, "sources", json_array_new());
    json_object_set_object_member(p, "config", c); return c;
}
static JsonObject *add_source(JsonObject *p, JsonObject *c, guint id, guint weight, guint count, const gchar *role)
{
    gchar *sid = uuid(id + 1000), *pid = uuid(id + 2000); JsonObject *source = json_object_new();
    json_object_set_string_member(source, "source_id", sid); json_object_set_string_member(source, "playlist_id", pid); json_object_set_string_member(source, "source_name", sid);
    json_object_set_string_member(source, "role", role); json_object_set_array_member(source, "tracks", json_array_new());
    json_array_add_object_element(json_object_get_array_member(p, "sources"), source);
    if (g_str_equal(role, "music")) {
        JsonObject *rule = json_object_new(); json_object_set_string_member(rule, "source_id", sid); json_object_set_string_member(rule, "playlist_id", pid);
        json_object_set_int_member(rule, "weight", weight); json_object_set_int_member(rule, "count", count); json_object_set_int_member(rule, "days", 0);
        json_object_set_string_member(rule, "start", "00:00"); json_object_set_string_member(rule, "end", "00:00");
        json_array_add_object_element(json_object_get_array_member(c, "sources"), rule);
    }
    g_free(sid); g_free(pid); return source;
}
static void test_identity_frames(void)
{
    JsonObject *p = prepared("playlist"), *r = row(1, "İlk"); json_array_add_object_element(json_object_get_array_member(p, "tracks"), r);
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpuint(entry(plan, 0)->run.duration_frames, ==, 288031); g_assert_cmpuint(entry(plan, 0)->run.media.cue_out_frame, ==, 288031);
    g_assert_cmpuint(json_object_get_int_member(json_array_get_object_element(json_object_get_array_member(json_node_get_object(plan->display), "entries"), 0), "duration_ms"), ==, 6000);
    radios_playout_plan_free(plan);
    JsonObject *d = json_object_get_object_member(r, "detail"), *m = json_object_get_object_member(d, "media"), *v = json_object_get_object_member(d, "version");
    json_object_set_string_member(m, "station_id", "e0000000-0000-4000-8000-000000000002"); rejected(p, &req); json_object_set_string_member(m, "station_id", station);
    json_object_set_boolean_member(v, "verified", FALSE); rejected(p, &req); json_object_set_boolean_member(v, "verified", TRUE);
    json_object_set_string_member(v, "managed_path", "/tmp/operator-file.wav"); rejected(p, &req);
    json_object_unref(p);
}
static void test_fractional_whole_file(void)
{
    JsonObject *p = prepared("playlist"), *r = row(1, "Kesirli spot");
    json_array_add_object_element(json_object_get_array_member(p, "tracks"), r);
    JsonObject *detail = json_object_get_object_member(r, "detail"), *version = json_object_get_object_member(detail, "version");
    JsonObject *analysis = json_object_get_object_member(detail, "analysis");
    json_object_set_int_member(version, "duration_ms", 6000);
    json_object_set_int_member(analysis, "sample_rate", 44100); json_object_set_int_member(analysis, "sample_frames", 264601);
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpuint(entry(plan, 0)->run.duration_frames, ==, 288002);
    g_assert_cmpuint(entry(plan, 0)->run.media.cue_out_frame, ==, 288002);
    radios_playout_plan_free(plan);
    JsonObject *marks = json_object_new();
    json_object_set_string_member(marks, "station_id", station);
    json_object_set_string_member(marks, "version_id", json_object_get_string_member(version, "_id"));
    json_object_set_string_member(marks, "media_file_id", json_object_get_string_member(version, "media_file_id"));
    json_object_set_string_member(marks, "media_sha256", json_object_get_string_member(version, "sha256"));
    json_object_set_int_member(marks, "media_duration_ms", 6000); json_object_set_int_member(marks, "cue_in_ms", 0);
    json_object_set_int_member(marks, "out_ms", 5999); json_object_set_object_member(detail, "markers", marks);
    plan = build(p, &req); g_assert_cmpuint(entry(plan, 0)->run.media.cue_out_frame, ==, 287952);
    radios_playout_plan_free(plan); json_object_unref(p);
}
static void test_markers_calendar(void)
{
    JsonObject *p = prepared("programme"), *a = row(1, "A"), *b = row(2, "B"), *config = json_object_new(), *timing = json_object_new();
    JsonArray *tracks = json_object_get_array_member(p, "tracks"); json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b);
    json_object_set_string_member(config, "title", "Takvim"); json_object_set_string_member(config, "date", "2026-10-06");
    json_object_set_string_member(timing, "event_id", json_object_get_string_member(b, "entry_id")); json_object_set_string_member(timing, "start_time", "08:00");
    json_object_set_string_member(timing, "target_time", "08:30"); json_object_set_string_member(timing, "policy", "finish"); json_object_set_int_member(timing, "max_wait_ms", 1250);
    json_object_set_object_member(config, "timing", timing); json_object_set_object_member(p, "config", config);
    JsonObject *d = json_object_get_object_member(a, "detail"), *m = json_object_get_object_member(d, "media"), *v = json_object_get_object_member(d, "version"), *marks = json_object_new();
    json_object_set_string_member(marks, "station_id", station); json_object_set_string_member(marks, "version_id", json_object_get_string_member(v, "_id"));
    json_object_set_string_member(marks, "media_file_id", json_object_get_string_member(m, "media_file_id")); json_object_set_string_member(marks, "media_sha256", json_object_get_string_member(v, "sha256"));
    json_object_set_int_member(marks, "media_duration_ms", 6001); json_object_set_int_member(marks, "cue_in_ms", 100); json_object_set_int_member(marks, "out_ms", 6001);
    json_object_set_int_member(marks, "auto_next_ms", 5600); json_object_set_int_member(marks, "overlap_ms", 500); json_object_set_int_member(marks, "fade_start_ms", 5500);
    json_object_set_int_member(marks, "fade_end_ms", 6001); json_object_set_boolean_member(marks, "automatic", TRUE); json_object_set_object_member(d, "markers", marks);
    RadioSPlayoutRequest req = request(); req.mode = RADIOS_AUDIO_RUN_CUE; req.overlap_seconds = 0.25; RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(plan->display), "programme_title"), ==, "Takvim");
    json_object_set_string_member(config, "title", "Sonradan değişen taslak");
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(plan->display), "programme_title"), ==, "Takvim");
    g_assert_cmpuint(entry(plan, 0)->run.media.cue_in_frame, ==, 4800); g_assert_cmpuint(entry(plan, 0)->run.auto_next_frame, ==, 268800);
    g_assert_cmpint(plan->options.start_utc_us, ==, stamp("2026-10-06T08:00:00Z"));
    g_assert_cmpint(entry(plan, 1)->run.start_utc_us, ==, stamp("2026-10-06T08:30:00Z"));
    g_assert_cmpint(entry(plan, 1)->run.start_policy, ==, RADIOS_AUDIO_RUN_START_FINISH); g_assert_cmpuint(entry(plan, 1)->run.max_wait_frames, ==, 60000);
    g_assert_cmpuint(entry(plan, 0)->run.overlap_frames, ==, 0); radios_playout_plan_free(plan);
    json_object_set_string_member(marks, "media_sha256", "mismatch"); rejected(p, &req); json_object_unref(p);
}
static void test_cycle_complete(void)
{
    JsonObject *p = prepared("automation"), *c = configuration(p, 6), *a = add_source(p, c, 1, 1, 2, "music"), *b = add_source(p, c, 2, 1, 1, "music");
    json_array_add_object_element(json_object_get_array_member(a, "tracks"), row(1, "A")); json_array_add_object_element(json_object_get_array_member(a, "tracks"), row(2, "B"));
    json_array_add_object_element(json_object_get_array_member(b, "tracks"), row(3, "C"));
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    const guint ids[] = {1, 2, 3, 1, 2, 3}; g_assert_cmpuint(plan->entries->len, ==, G_N_ELEMENTS(ids));
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) { gchar *id = uuid(ids[i]); g_assert_cmpstr(entry(plan, i)->run.entry_id, ==, id); g_free(id); g_assert_cmpint(entry(plan, i)->run.preferred_deck, ==, (gint)(i % 2)); }
    radios_playout_plan_free(plan); json_object_set_int_member(c, "sourceMode", 2); plan = build(p, &req);
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) { gchar *id = uuid(ids[i]); g_assert_cmpstr(entry(plan, i)->run.entry_id, ==, id); g_free(id); }
    radios_playout_plan_free(plan); json_object_unref(p);
}
static void test_weighted_deterministic(void)
{
    JsonObject *p = prepared("automation"), *c = configuration(p, 200), *a = add_source(p, c, 1, 9, 1, "music"), *b = add_source(p, c, 2, 1, 1, "music");
    json_array_add_object_element(json_object_get_array_member(a, "tracks"), row(1, "A")); json_array_add_object_element(json_object_get_array_member(b, "tracks"), row(2, "B"));
    json_object_set_int_member(c, "sourceMode", 1); RadioSPlayoutRequest req = request(); RadioSPlayoutHistory *history = radios_playout_history_new(); req.history = history;
    RadioSPlayoutPlan *first = build(p, &req), *second = build(p, &req); guint frequent = 0;
    for (guint i = 0; i < first->entries->len; ++i) { g_assert_cmpstr(entry(first, i)->run.entry_id, ==, entry(second, i)->run.entry_id); if (g_str_equal(entry(first, i)->source_id, json_object_get_string_member(a, "source_id"))) ++frequent; }
    g_assert_cmpuint(frequent, >, 150); g_assert_cmpuint(frequent, <, 198);
    JsonNode *unchanged = radios_playout_history_json(history); g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(unchanged), "entries")), ==, 0); json_node_free(unchanged);
    radios_playout_plan_free(first); radios_playout_plan_free(second); radios_playout_history_free(history); json_object_unref(p);
}
static void test_repeat_history(void)
{
    JsonObject *p = prepared("automation"), *c = configuration(p, 3), *a = add_source(p, c, 1, 1, 1, "music");
    json_array_add_object_element(json_object_get_array_member(a, "tracks"), row(1, "IŞIK")); json_array_add_object_element(json_object_get_array_member(a, "tracks"), row(2, "ışık"));
    json_object_set_int_member(c, "artistGap", 60); RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpuint(plan->entries->len, ==, 1); g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(plan->display), "diagnostics")), >, 0);
    radios_playout_plan_free(plan); json_object_set_int_member(c, "artistGap", 0); json_object_set_int_member(c, "trackGap", 60);
    RadioSPlayoutHistory *history = radios_playout_history_new(); gchar *mid = uuid(101), *vid = uuid(201);
    radios_playout_history_observe(history, mid, vid, "IŞIK", "Albüm", req.now_utc_us - 1000000, 480);
    JsonNode *persisted = radios_playout_history_json(history); RadioSPlayoutHistory *reopened = radios_playout_history_new();
    radios_playout_history_import(reopened, json_object_get_array_member(json_node_get_object(persisted), "entries")); req.history = reopened;
    plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 1); gchar *expected = uuid(2); g_assert_cmpstr(entry(plan, 0)->run.entry_id, ==, expected);
    g_free(expected); g_free(mid); g_free(vid); radios_playout_plan_free(plan); json_node_free(persisted);
    radios_playout_history_free(history); radios_playout_history_free(reopened); json_object_unref(p);
}
static void test_jingle_fallback(void)
{
    JsonObject *p = prepared("automation"), *c = configuration(p, 3), *a = add_source(p, c, 1, 1, 1, "music"), *j = add_source(p, c, 2, 1, 1, "jingle");
    json_array_add_object_element(json_object_get_array_member(a, "tracks"), row(1, "A")); json_array_add_object_element(json_object_get_array_member(j, "tracks"), row(2, "İstasyon"));
    json_object_set_boolean_member(c, "jingleEnabled", TRUE); json_object_set_string_member(c, "jingleDeck", "3");
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 5);
    g_assert_true(entry(plan, 1)->jingle); g_assert_cmpint(entry(plan, 1)->run.preferred_deck, ==, 2); g_assert_cmpuint(plan->options.pool_count, ==, 3);
    g_assert_cmpuint(entry(plan, 0)->run.overlap_frames, ==, 0); radios_playout_plan_free(plan);
    json_object_set_boolean_member(c, "jingleEnabled", FALSE); json_object_set_int_member(c, "fallbackMode", 2);
    json_object_set_boolean_member(json_array_get_object_element(json_object_get_array_member(a, "tracks"), 0), "eligible", FALSE);
    JsonObject *fallback = add_source(p, c, 3, 1, 1, "fallback"); json_array_add_object_element(json_object_get_array_member(fallback, "tracks"), row(3, "Yedek"));
    plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 3); g_assert_cmpstr(entry(plan, 0)->source_id, ==, json_object_get_string_member(fallback, "source_id"));
    radios_playout_plan_free(plan); json_object_unref(p);
}
static void test_midnight_weekdays_single(void)
{
    JsonObject *p = prepared("automation"), *c = configuration(p, 2), *a = add_source(p, c, 1, 1, 1, "music");
    json_array_add_object_element(json_object_get_array_member(a, "tracks"), row(1, "A"));
    json_object_set_string_member(c, "windowStart", "22:00"); json_object_set_string_member(c, "windowEnd", "06:00");
    JsonArray *days = json_array_new(); json_array_add_int_element(days, 1); json_object_set_array_member(c, "weekdays", days);
    JsonObject *rule = json_array_get_object_element(json_object_get_array_member(c, "sources"), 0); json_object_set_string_member(rule, "start", "23:00"); json_object_set_string_member(rule, "end", "05:00"); json_object_set_int_member(rule, "days", 1);
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpint(entry(plan, 0)->planned_utc_us, ==, req.now_utc_us); g_assert_cmpint(entry(plan, 0)->run.latest_start_utc_us, ==, stamp("2026-10-06T05:00:00Z")); radios_playout_plan_free(plan);
    json_object_set_string_member(c, "startTime", "12:00"); plan = build(p, &req);
    g_assert_cmpint(plan->options.start_utc_us, ==, stamp("2026-10-12T23:00:00Z")); radios_playout_plan_free(plan);
    json_object_set_int_member(c, "deckMode", 1); json_object_set_int_member(c, "transition", RADIOS_AUDIO_RUN_CROSSFADE); rejected(p, &req);
    json_object_set_int_member(c, "transition", RADIOS_AUDIO_RUN_EOF); plan = build(p, &req); g_assert_cmpuint(plan->options.pool_count, ==, 1); radios_playout_plan_free(plan); json_object_unref(p);
}
static void set_duration(JsonObject *r, guint milliseconds)
{
    JsonObject *detail = json_object_get_object_member(r, "detail");
    json_object_set_int_member(json_object_get_object_member(detail, "version"), "duration_ms", milliseconds);
    json_object_set_int_member(json_object_get_object_member(detail, "analysis"), "sample_frames", (gint64)milliseconds * 48);
}
static void set_markers(JsonObject *r, guint out, guint next, guint overlap)
{
    JsonObject *d = json_object_get_object_member(r, "detail"), *m = json_object_get_object_member(d, "media"), *v = json_object_get_object_member(d, "version"), *marks = json_object_new();
    json_object_set_string_member(marks, "station_id", station); json_object_set_string_member(marks, "version_id", json_object_get_string_member(v, "_id"));
    json_object_set_string_member(marks, "media_file_id", json_object_get_string_member(m, "media_file_id")); json_object_set_string_member(marks, "media_sha256", json_object_get_string_member(v, "sha256"));
    json_object_set_int_member(marks, "media_duration_ms", out); json_object_set_int_member(marks, "cue_in_ms", 0); json_object_set_int_member(marks, "out_ms", out);
    json_object_set_int_member(marks, "auto_next_ms", next); json_object_set_int_member(marks, "overlap_ms", overlap);
    json_object_set_int_member(marks, "fade_start_ms", next); json_object_set_int_member(marks, "fade_end_ms", out);
    json_object_set_boolean_member(marks, "automatic", TRUE); json_object_set_object_member(d, "markers", marks);
}
static void assert_entry_id(RadioSPlayoutPlan *p, guint index, guint id)
{ gchar *expected = uuid(id); g_assert_cmpstr(entry(p, index)->run.entry_id, ==, expected); g_free(expected); }
static RadioSPlayoutHistory *checkpoint(RadioSPlayoutEntry *e)
{
    RadioSPlayoutHistory *original = radios_playout_history_new();
    radios_playout_history_set_rotation(original, e->rotation_after);
    RadioSPlayoutHistory *copy = radios_playout_history_copy(original); JsonNode *saved = radios_playout_history_json(copy);
    RadioSPlayoutHistory *reopened = radios_playout_history_new(); JsonObject *config = json_node_get_object(saved);
    radios_playout_history_import(reopened, json_object_get_array_member(config, "entries"));
    radios_playout_history_set_rotation(reopened, json_object_get_object_member(config, "rotation"));
    json_node_free(saved); radios_playout_history_free(copy); radios_playout_history_free(original); return reopened;
}
static void test_cycle_durable_checkpoint(void)
{
    JsonObject *p = prepared("automation"), *c = configuration(p, 3), *a = add_source(p, c, 1, 1, 2, "music"), *b = add_source(p, c, 2, 1, 2, "music");
    json_array_add_object_element(json_object_get_array_member(a, "tracks"), row(1, "A")); json_array_add_object_element(json_object_get_array_member(a, "tracks"), row(2, "B"));
    json_array_add_object_element(json_object_get_array_member(b, "tracks"), row(3, "C")); json_array_add_object_element(json_object_get_array_member(b, "tracks"), row(4, "D"));
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *first = build(p, &req);
    assert_entry_id(first, 0, 1); assert_entry_id(first, 1, 2); assert_entry_id(first, 2, 3);
    req.history = checkpoint(entry(first, 2));
    JsonNode *before = radios_playout_history_json(req.history);
    json_object_set_string_member(c, "planDate", "2026-10-07"); json_object_set_string_member(c, "startTime", "01:00");
    json_object_set_string_member(c, "name", "Cosmetic rename"); json_object_set_int_member(c, "previewCount", 4);
    RadioSPlayoutPlan *second = build(p, &req); const guint expected[] = {4, 1, 2, 3};
    for (guint i = 0; i < G_N_ELEMENTS(expected); ++i) assert_entry_id(second, i, expected[i]);
    JsonNode *after = radios_playout_history_json(req.history); g_assert_true(json_node_equal(before, after));
    json_node_free(before); json_node_free(after); radios_playout_plan_free(second);
    json_object_set_int_member(a, "playlist_revision", 2); second = build(p, &req); assert_entry_id(second, 0, 1);
    radios_playout_plan_free(second); radios_playout_history_free((RadioSPlayoutHistory *)req.history); radios_playout_plan_free(first); json_object_unref(p);
}
static void test_weighted_checkpoint_rng(void)
{
    JsonObject *p = prepared("automation"), *c = configuration(p, 20), *a = add_source(p, c, 1, 3, 1, "music"), *b = add_source(p, c, 2, 1, 1, "music");
    json_array_add_object_element(json_object_get_array_member(a, "tracks"), row(1, "A")); json_array_add_object_element(json_object_get_array_member(a, "tracks"), row(2, "B"));
    json_array_add_object_element(json_object_get_array_member(b, "tracks"), row(3, "C")); json_object_set_int_member(c, "sourceMode", 1); json_object_set_int_member(c, "trackMode", 0);
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *whole = build(p, &req); json_object_set_int_member(c, "previewCount", 7);
    RadioSPlayoutPlan *first = build(p, &req); req.history = checkpoint(entry(first, 6)); json_object_set_int_member(c, "previewCount", 13);
    RadioSPlayoutPlan *rest = build(p, &req);
    for (guint i = 0; i < 7; ++i) g_assert_cmpstr(entry(first, i)->run.entry_id, ==, entry(whole, i)->run.entry_id);
    for (guint i = 0; i < 13; ++i) g_assert_cmpstr(entry(rest, i)->run.entry_id, ==, entry(whole, i + 7)->run.entry_id);
    radios_playout_plan_free(rest); radios_playout_plan_free(first); radios_playout_plan_free(whole); radios_playout_history_free((RadioSPlayoutHistory *)req.history); json_object_unref(p);
}
static void test_complete_consumed_unique(void)
{
    JsonObject *p = prepared("automation"), *c = configuration(p, 4), *a = add_source(p, c, 1, 1, 1, "music"), *b = add_source(p, c, 2, 1, 1, "music");
    JsonArray *tracks = json_object_get_array_member(a, "tracks"); for (guint i = 1; i <= 3; ++i) json_array_add_object_element(tracks, row(i, "Artist"));
    json_array_add_object_element(json_object_get_array_member(b, "tracks"), row(4, "Other")); json_object_set_int_member(c, "sourceMode", 2); json_object_set_int_member(c, "trackMode", 0);
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    for (guint i = 0; i < 3; ++i) for (guint j = i + 1; j < 3; ++j) g_assert_cmpstr(entry(plan, i)->run.entry_id, !=, entry(plan, j)->run.entry_id);
    assert_entry_id(plan, 3, 4); radios_playout_plan_free(plan);
    json_object_set_int_member(c, "previewCount", 1); GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < 3; ++i) {
        plan = build(p, &req); g_assert_false(g_hash_table_contains(seen, entry(plan, 0)->run.entry_id)); g_hash_table_add(seen, g_strdup(entry(plan, 0)->run.entry_id));
        RadioSPlayoutHistory *next = checkpoint(entry(plan, 0)); radios_playout_history_free((RadioSPlayoutHistory *)req.history); req.history = next; radios_playout_plan_free(plan);
    }
    plan = build(p, &req); assert_entry_id(plan, 0, 4); radios_playout_plan_free(plan); g_hash_table_unref(seen); radios_playout_history_free((RadioSPlayoutHistory *)req.history); req.history = NULL;
    json_object_set_boolean_member(json_array_get_object_element(tracks, 1), "eligible", FALSE); json_object_set_boolean_member(json_array_get_object_element(tracks, 2), "eligible", FALSE);
    json_object_set_int_member(c, "trackMode", 1); json_object_set_int_member(c, "previewCount", 3); plan = build(p, &req);
    assert_entry_id(plan, 0, 1); assert_entry_id(plan, 1, 4); assert_entry_id(plan, 2, 1); radios_playout_plan_free(plan); json_object_unref(p);
}
static void test_repeat_overlap_actual_start(void)
{
    JsonObject *p = prepared("automation"), *c = configuration(p, 2), *a = add_source(p, c, 1, 1, 1, "music"), *r = row(1, "A");
    set_duration(r, 60000); json_array_add_object_element(json_object_get_array_member(a, "tracks"), r);
    json_object_set_int_member(c, "trackGap", 1); json_object_set_int_member(c, "transition", RADIOS_AUDIO_RUN_CROSSFADE); json_object_set_double_member(c, "overlap", 30);
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 1);
    radios_playout_plan_free(plan); json_object_set_int_member(c, "trackGap", 0); json_object_set_int_member(c, "artistGap", 1);
    JsonObject *second = row(2, "a"); set_duration(second, 60000); json_array_add_object_element(json_object_get_array_member(a, "tracks"), second);
    plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 1); radios_playout_plan_free(plan); json_object_unref(p);
}
static void test_boundary_cue_real_timeline(void)
{
    JsonObject *p = prepared("automation"), *c = configuration(p, 2), *a = add_source(p, c, 1, 1, 1, "music"), *b = add_source(p, c, 2, 1, 1, "music"), *r = row(1, "A");
    set_duration(r, 10000); set_markers(r, 10000, 5000, 1000); json_array_add_object_element(json_object_get_array_member(a, "tracks"), r);
    json_array_add_object_element(json_object_get_array_member(b, "tracks"), row(2, "B")); json_object_set_int_member(c, "boundaryTransition", 3);
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpint(entry(plan, 0)->run.mode, ==, RADIOS_AUDIO_RUN_CUE); g_assert_cmpuint(entry(plan, 0)->run.auto_next_frame, ==, 240000);
    g_assert_cmpuint(entry(plan, 0)->run.fade_start_frame, ==, 240000); g_assert_cmpuint(entry(plan, 0)->run.overlap_frames, ==, 48000);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us - entry(plan, 0)->planned_utc_us, ==, 5 * G_USEC_PER_SEC);
    radios_playout_plan_free(plan); json_object_set_int_member(c, "previewCount", 1); plan = build(p, &req);
    g_assert_cmpuint(plan->entries->len, ==, 1); g_assert_cmpint(entry(plan, 0)->run.mode, ==, RADIOS_AUDIO_RUN_CUE);
    g_assert_cmpuint(entry(plan, 0)->run.auto_next_frame, ==, 240000); g_assert_cmpuint(json_object_get_int_member(entry(plan, 0)->rotation_after, "source_index"), ==, 1);
    radios_playout_plan_free(plan); json_object_unref(p);
}
static void test_sequential_policy_preserved(void)
{
    JsonObject *p = prepared("programme"), *c = json_object_new(), *a = row(1, "A"), *b = row(2, "B");
    json_object_set_string_member(c, "date", "2026-10-06"); json_object_set_object_member(p, "config", c);
    JsonObject *m = json_object_get_object_member(b, "metadata"); json_object_set_string_member(m, "start_time", "00:31"); json_object_set_string_member(m, "start_policy", "sequential");
    json_array_add_object_element(json_object_get_array_member(p, "tracks"), a); json_array_add_object_element(json_object_get_array_member(p, "tracks"), b);
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpint(entry(plan, 1)->run.start_policy, ==, RADIOS_AUDIO_RUN_START_SEQUENTIAL); radios_playout_plan_free(plan); json_object_unref(p);
}
static void test_jingle_metadata_and_checkpoint_phase(void)
{
    JsonObject *p = prepared("automation"), *c = configuration(p, 1), *a = add_source(p, c, 1, 1, 1, "music"), *j = add_source(p, c, 2, 1, 1, "jingle");
    json_array_add_object_element(json_object_get_array_member(a, "tracks"), row(1, "A")); JsonObject *jr = row(2, "");
    json_object_set_string_member(json_object_get_object_member(jr, "metadata"), "kind", "jingle"); json_array_add_object_element(json_object_get_array_member(j, "tracks"), jr);
    json_object_set_boolean_member(c, "requireMetadata", TRUE); json_object_set_boolean_member(c, "jingleEnabled", TRUE); json_object_set_int_member(c, "jingleEvery", 2);
    json_object_set_string_member(c, "jingleDeck", "3"); RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *first = build(p, &req);
    g_assert_cmpuint(first->entries->len, ==, 1);
    req.history = checkpoint(entry(first, 0)); RadioSPlayoutPlan *second = build(p, &req);
    g_assert_true(json_object_get_boolean_member(entry(second, 0)->rotation_after, "jingle_due"));
    radios_playout_history_free((RadioSPlayoutHistory *)req.history); req.history = checkpoint(entry(second, 0)); RadioSPlayoutPlan *third = build(p, &req);
    g_assert_cmpuint(third->entries->len, ==, 2); g_assert_true(entry(third, 0)->jingle); assert_entry_id(third, 0, 2); assert_entry_id(third, 1, 1);
    radios_playout_plan_free(third); radios_playout_plan_free(second); radios_playout_plan_free(first); radios_playout_history_free((RadioSPlayoutHistory *)req.history); json_object_unref(p);
}
static void test_calendar_gap_after_existing_entry(void)
{
    JsonObject *p = prepared("automation"), *c = configuration(p, 2), *source = add_source(p, c, 1, 1, 1, "music"), *r = row(1, "A");
    set_duration(r, 120000); json_array_add_object_element(json_object_get_array_member(source, "tracks"), r);
    json_object_set_string_member(c, "startTime", "00:00"); json_object_set_string_member(c, "windowStart", "00:00"); json_object_set_string_member(c, "windowEnd", "00:01");
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpuint(plan->entries->len, ==, 2);
    g_assert_cmpint(entry(plan, 0)->planned_utc_us, ==, stamp("2026-10-06T00:00:00Z"));
    g_assert_cmpint(entry(plan, 1)->planned_utc_us, ==, stamp("2026-10-07T00:00:00Z"));
    g_assert_cmpint(entry(plan, 1)->run.start_utc_us, ==, stamp("2026-10-07T00:00:00Z"));
    g_assert_cmpint(entry(plan, 1)->run.latest_start_utc_us, ==, stamp("2026-10-07T00:01:00Z"));
    radios_playout_plan_free(plan); json_object_unref(p);
}
static void test_repeat_history_subsecond_roundtrip(void)
{
    JsonObject *p = prepared("automation"), *c = configuration(p, 1), *source = add_source(p, c, 1, 1, 1, "music");
    json_array_add_object_element(json_object_get_array_member(source, "tracks"), row(1, "A")); json_object_set_int_member(c, "trackGap", 1);
    json_object_set_string_member(c, "startTime", "00:31"); RadioSPlayoutRequest req = request();
    RadioSPlayoutHistory *h = radios_playout_history_new(); gchar *mid = uuid(101), *vid = uuid(201);
    radios_playout_history_observe(h, mid, vid, "A", "Albüm", stamp("2026-10-06T00:30:00Z") + 500000, 480);
    JsonNode *saved = radios_playout_history_json(h); RadioSPlayoutHistory *reopened = radios_playout_history_new();
    radios_playout_history_import(reopened, json_object_get_array_member(json_node_get_object(saved), "entries")); req.history = reopened;
    rejected(p, &req);
    g_free(mid); g_free(vid); json_node_free(saved); radios_playout_history_free(reopened); radios_playout_history_free(h); json_object_unref(p);
}
static void test_default_jingle_adjacent_decks(void)
{
    JsonObject *p = prepared("automation"), *c = configuration(p, 3), *music = add_source(p, c, 1, 1, 1, "music"), *jingle = add_source(p, c, 2, 1, 1, "jingle");
    json_array_add_object_element(json_object_get_array_member(music, "tracks"), row(1, "A"));
    json_array_add_object_element(json_object_get_array_member(music, "tracks"), row(2, "B"));
    json_array_add_object_element(json_object_get_array_member(jingle, "tracks"), row(3, "Station"));
    json_object_set_boolean_member(c, "jingleEnabled", TRUE); json_object_set_null_member(c, "jingleDeck");
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpuint(plan->entries->len, ==, 5); const gint decks[] = {0, 1, 0, 1, 0};
    for (guint i = 0; i < G_N_ELEMENTS(decks); ++i) g_assert_cmpint(entry(plan, i)->run.preferred_deck, ==, decks[i]);
    RadioSPlayoutHistory *history = checkpoint(entry(plan, 0)); radios_playout_plan_free(plan);
    req.history = history; json_object_set_int_member(c, "previewCount", 1); plan = build(p, &req);
    g_assert_cmpuint(plan->entries->len, ==, 2); g_assert_true(entry(plan, 0)->jingle);
    g_assert_cmpint(entry(plan, 0)->run.preferred_deck, ==, 0); g_assert_cmpint(entry(plan, 1)->run.preferred_deck, ==, 1);
    radios_playout_plan_free(plan); radios_playout_history_free(history); req.history = NULL;
    plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 1);
    g_assert_cmpint(entry(plan, 0)->run.preferred_deck, ==, 0); g_assert_true(json_object_get_boolean_member(entry(plan, 0)->rotation_after, "jingle_due"));
    radios_playout_plan_free(plan);
    json_object_set_int_member(c, "previewCount", 3); json_object_set_string_member(c, "jingleDeck", "3"); plan = build(p, &req);
    const gint dedicated[] = {0, 2, 1, 2, 0};
    for (guint i = 0; i < G_N_ELEMENTS(dedicated); ++i) g_assert_cmpint(entry(plan, i)->run.preferred_deck, ==, dedicated[i]);
    radios_playout_plan_free(plan); json_object_set_string_member(c, "jingleDeck", "1"); plan = build(p, &req);
    const gint shared[] = {1, 0, 1, 0, 1};
    for (guint i = 0; i < G_N_ELEMENTS(shared); ++i) g_assert_cmpint(entry(plan, i)->run.preferred_deck, ==, shared[i]);
    radios_playout_plan_free(plan); json_object_unref(p);
}
static JsonObject *add_deck_policy(JsonObject *config, const gchar *deck, guint mode, gdouble overlap)
{
    if (!json_object_has_member(config, "deckTransitions")) json_object_set_array_member(config, "deckTransitions", json_array_new());
    JsonObject *policy = json_object_new();
    json_object_set_string_member(policy, "deck_id", deck);
    json_object_set_int_member(policy, "transition", mode);
    json_object_set_double_member(policy, "overlap", overlap);
    json_array_add_object_element(json_object_get_array_member(config, "deckTransitions"), policy);
    return policy;
}
static void test_autodj_deck_transitions(void)
{
    JsonObject *prepared_input = prepared("automation"), *config = configuration(prepared_input, 4);
    JsonObject *first_source = add_source(prepared_input, config, 1, 1, 1, "music");
    JsonObject *second_source = add_source(prepared_input, config, 2, 1, 1, "music");
    JsonObject *first_row = row(1, "A"), *second_row = row(2, "B");
    json_array_add_object_element(json_object_get_array_member(first_source, "tracks"), first_row);
    json_array_add_object_element(json_object_get_array_member(second_source, "tracks"), second_row);
    json_object_set_int_member(config, "transition", RADIOS_AUDIO_RUN_MANUAL);
    json_object_set_int_member(config, "boundaryTransition", RADIOS_AUDIO_RUN_CROSSFADE + 1);
    JsonObject *first_policy = add_deck_policy(config, "1", RADIOS_AUDIO_RUN_CROSSFADE, 1.25);
    JsonObject *second_policy = add_deck_policy(config, "2", RADIOS_AUDIO_RUN_EOF, 0);
    RadioSPlayoutRequest planning = request(); RadioSPlayoutPlan *plan = build(prepared_input, &planning);
    for (guint index = 0; index < plan->entries->len; ++index) {
        RadioSPlayoutEntry *planned = entry(plan, index);
        g_assert_cmpint(planned->run.preferred_deck, ==, (gint)(index % 2));
        g_assert_cmpint(planned->run.mode, ==, index % 2 ? RADIOS_AUDIO_RUN_EOF : RADIOS_AUDIO_RUN_CROSSFADE);
        g_assert_cmpuint(planned->run.overlap_frames, ==, index % 2 ? 0 : 60000);
        if (index) {
            RadioSPlayoutEntry *previous = entry(plan, index - 1);
            guint64 offset = previous->run.duration_frames - previous->run.overlap_frames;
            g_assert_cmpint(planned->planned_utc_us - previous->planned_utc_us, ==, (gint64)(offset * 1000000 / 48000));
            g_assert_cmpuint(planned->run.fade_in_frames, ==, previous->run.overlap_frames);
        }
    }
    JsonArray *display = json_object_get_array_member(json_node_get_object(plan->display), "entries");
    g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(display, 0), "transition"), ==, RADIOS_AUDIO_RUN_CROSSFADE);
    radios_playout_plan_free(plan);
    planning.deck_phase = 1; plan = build(prepared_input, &planning);
    g_assert_cmpint(entry(plan, 0)->run.preferred_deck, ==, 1);
    g_assert_cmpint(entry(plan, 0)->run.mode, ==, RADIOS_AUDIO_RUN_EOF);
    g_assert_cmpint(entry(plan, 1)->run.preferred_deck, ==, 0);
    g_assert_cmpint(entry(plan, 1)->run.mode, ==, RADIOS_AUDIO_RUN_CROSSFADE);
    radios_playout_plan_free(plan);
    planning.deck_phase = 0;
    set_duration(first_row, 6000);
    set_markers(first_row, 6000, 5000, 1000);
    json_object_set_int_member(first_policy, "transition", RADIOS_AUDIO_RUN_CUE);
    json_object_set_double_member(first_policy, "overlap", 0.3);
    json_object_set_int_member(second_policy, "transition", RADIOS_AUDIO_RUN_MANUAL);
    plan = build(prepared_input, &planning);
    g_assert_cmpuint(entry(plan, 0)->run.auto_next_frame, ==, 240000);
    g_assert_cmpuint(entry(plan, 0)->run.overlap_frames, ==, 48000);
    g_assert_cmpint(entry(plan, 1)->run.mode, ==, RADIOS_AUDIO_RUN_MANUAL);
    radios_playout_plan_free(plan); json_object_unref(prepared_input);
}
static void test_autodj_deck_policy_validation(void)
{
    JsonObject *prepared_input = prepared("automation"), *config = configuration(prepared_input, 2);
    JsonObject *source = add_source(prepared_input, config, 1, 1, 1, "music");
    json_array_add_object_element(json_object_get_array_member(source, "tracks"), row(1, "A"));
    JsonObject *policy = add_deck_policy(config, "1", RADIOS_AUDIO_RUN_CROSSFADE, 0.5);
    RadioSPlayoutRequest planning = request();
    const gchar *invalid_ids[] = { "0", "01", "33", "3", "preview", "1junk" };
    for (guint index = 0; index < G_N_ELEMENTS(invalid_ids); ++index) {
        json_object_set_string_member(policy, "deck_id", invalid_ids[index]); rejected(prepared_input, &planning);
    }
    json_object_set_string_member(policy, "deck_id", "1");
    json_object_set_boolean_member(policy, "transition", TRUE); rejected(prepared_input, &planning);
    json_object_set_int_member(policy, "transition", RADIOS_AUDIO_RUN_EOF); rejected(prepared_input, &planning);
    json_object_set_int_member(policy, "transition", RADIOS_AUDIO_RUN_CROSSFADE);
    json_object_set_double_member(policy, "overlap", 31); rejected(prepared_input, &planning);
    json_object_set_double_member(policy, "overlap", 0.5);
    json_object_set_int_member(config, "deckMode", 1); rejected(prepared_input, &planning);
    json_object_set_int_member(config, "deckMode", 0);
    add_deck_policy(config, "1", RADIOS_AUDIO_RUN_EOF, 0); rejected(prepared_input, &planning);
    json_object_set_array_member(config, "deckTransitions", json_array_new());
    json_array_add_null_element(json_object_get_array_member(config, "deckTransitions")); rejected(prepared_input, &planning);
    json_object_set_null_member(config, "deckTransitions"); rejected(prepared_input, &planning);
    json_object_unref(prepared_input);
}
static void test_autodj_deck_jingle_dry(void)
{
    JsonObject *prepared_input = prepared("automation"), *config = configuration(prepared_input, 2);
    JsonObject *source = add_source(prepared_input, config, 1, 1, 1, "music");
    JsonObject *jingle_source = add_source(prepared_input, config, 2, 1, 1, "jingle");
    json_array_add_object_element(json_object_get_array_member(source, "tracks"), row(1, "A"));
    json_array_add_object_element(json_object_get_array_member(jingle_source, "tracks"), row(2, "Jingle"));
    json_object_set_boolean_member(config, "jingleEnabled", TRUE);
    json_object_set_string_member(config, "jingleDeck", "2");
    json_object_set_int_member(config, "boundaryTransition", RADIOS_AUDIO_RUN_CROSSFADE + 1);
    add_deck_policy(config, "1", RADIOS_AUDIO_RUN_CROSSFADE, 1);
    add_deck_policy(config, "2", RADIOS_AUDIO_RUN_CROSSFADE, 1);
    RadioSPlayoutRequest planning = request(); RadioSPlayoutPlan *plan = build(prepared_input, &planning);
    g_assert_cmpuint(plan->entries->len, ==, 3);
    g_assert_cmpuint(entry(plan, 0)->run.overlap_frames, ==, 0);
    g_assert_cmpuint(entry(plan, 1)->run.overlap_frames, ==, 0);
    radios_playout_plan_free(plan); json_object_unref(prepared_input);
    prepared_input = prepared("playlist"); json_array_add_object_element(json_object_get_array_member(prepared_input, "tracks"), row(1, "Manual source"));
    plan = build(prepared_input, &planning);
    g_assert_cmpint(entry(plan, 0)->run.mode, ==, RADIOS_AUDIO_RUN_EOF);
    radios_playout_plan_free(plan); json_object_unref(prepared_input);
}
static void test_player_pair_tempo_and_grid(void)
{
    JsonObject *response = prepared("playlist"), *config = json_object_new();
    JsonArray *players = json_array_new(); json_array_add_int_element(players, 1); json_array_add_int_element(players, 3);
    json_object_set_array_member(config, "autodj_players", players); json_object_set_boolean_member(config, "beat_sync", TRUE);
    json_object_set_object_member(response, "config", config);
    for (guint index = 0; index < 3; ++index) {
        JsonObject *track = row(index + 1, "Rhythm source");
        JsonObject *analysis = json_object_get_object_member(json_object_get_object_member(track, "detail"), "analysis");
        RadioSRhythmAnalysis grid = {.bpm = index % 2 ? 128 : 120, .first_beat_ms = 125, .confidence = 0.9, .analyzed_ms = 6000};
        json_object_set_object_member(analysis, "rhythm", radios_rhythm_analysis_json(&grid));
        json_array_add_object_element(json_object_get_array_member(response, "tracks"), track);
    }
    RadioSPlayoutRequest planning = request(); planning.mode = RADIOS_AUDIO_RUN_CROSSFADE; planning.overlap_seconds = 1;
    RadioSPlayoutPlan *plan = build(response, &planning); g_assert_true(plan->player_autodj);
    g_assert_cmpuint(plan->options.deck_pool[0], ==, 0); g_assert_cmpuint(plan->options.deck_pool[1], ==, 2);
    g_assert_cmpint(entry(plan, 0)->run.preferred_deck, ==, 0); g_assert_cmpint(entry(plan, 1)->run.preferred_deck, ==, 2);
    g_assert_cmpint(entry(plan, 2)->run.preferred_deck, ==, 0); g_assert_true(entry(plan, 1)->run.beat_sync);
    g_assert_cmpfloat(entry(plan, 1)->run.tempo_ratio, ==, 0.9375); g_assert_cmpuint(entry(plan, 1)->run.media.cue_in_frame, ==, 6000);
    JsonObject *track = json_array_get_object_element(json_object_get_array_member(response, "tracks"), 1);
    JsonObject *rhythm = json_object_get_object_member(json_object_get_object_member(json_object_get_object_member(track, "detail"), "analysis"), "rhythm");
    json_object_set_double_member(rhythm, "confidence", 0.1);
    RadioSPlayoutPlan *fallback = build(response, &planning); g_assert_false(entry(fallback, 0)->run.beat_sync); g_assert_cmpfloat(entry(fallback, 1)->run.tempo_ratio, ==, 1); radios_playout_plan_free(fallback);
    g_assert_cmpfloat(entry(plan, 1)->run.rhythm.confidence, ==, 0.9);
    json_object_set_double_member(rhythm, "confidence", 0.9); json_object_set_double_member(rhythm, "bpm", 160);
    fallback = build(response, &planning); g_assert_false(entry(fallback, 0)->run.beat_sync); radios_playout_plan_free(fallback);
    json_object_set_double_member(rhythm, "bpm", 128); json_array_remove_element(players, 1); rejected(response, &planning);
    radios_playout_plan_free(plan); json_object_unref(response);
}
static void test_player_pair_named_transition_timing(void)
{
    const guint64 expected_overlap[] = {0, 24000, 16800};
    const gint64 expected_next_us[] = {6000645, 5500645, 4500000};
    for (guint mode = 0; mode < 3; ++mode) {
        JsonObject *response = prepared("programme"), *config = json_object_new();
        JsonArray *players = json_array_new(); json_array_add_int_element(players, 1); json_array_add_int_element(players, 3);
        json_object_set_array_member(config, "autodj_players", players); json_object_set_boolean_member(config, "beat_sync", FALSE);
        json_object_set_string_member(config, "title", "Named rotation pair"); json_object_set_object_member(response, "config", config);
        for (guint index = 0; index < 2; ++index) {
            JsonObject *track = row(index + 1, "Named canonical source"), *hint = json_object_new();
            JsonObject *detail = json_object_get_object_member(track, "detail"), *version = json_object_get_object_member(detail, "version"), *marks = json_object_new();
            json_object_set_string_member(marks, "station_id", station);
            json_object_set_string_member(marks, "version_id", json_object_get_string_member(version, "_id"));
            json_object_set_string_member(marks, "media_file_id", json_object_get_string_member(version, "media_file_id"));
            json_object_set_string_member(marks, "media_sha256", json_object_get_string_member(version, "sha256"));
            json_object_set_int_member(marks, "media_duration_ms", 6001); json_object_set_int_member(marks, "cue_in_ms", 0);
            json_object_set_int_member(marks, "out_ms", 6001); json_object_set_int_member(marks, "auto_next_ms", 4500);
            json_object_set_int_member(marks, "fade_start_ms", 4500); json_object_set_int_member(marks, "fade_end_ms", 6001);
            json_object_set_int_member(marks, "overlap_ms", 350); json_object_set_boolean_member(marks, "automatic", TRUE);
            json_object_set_object_member(detail, "markers", marks);
            json_object_set_int_member(hint, "mode", mode); json_object_set_int_member(hint, "overlap_ms", mode ? 500 : 0);
            json_object_set_object_member(track, "calendar_transition", hint);
            json_array_add_object_element(json_object_get_array_member(response, "tracks"), track);
        }
        JsonNode *input = json_node_new(JSON_NODE_OBJECT); json_node_set_object(input, response);
        gchar *before = json_to_string(input, FALSE); RadioSPlayoutRequest planning = request(); RadioSPlayoutPlan *plan = build(response, &planning);
        g_assert_true(plan->player_autodj); g_assert_cmpuint(plan->entries->len, ==, 2);
        g_assert_cmpuint(plan->options.pool_count, ==, 2); g_assert_cmpuint(plan->options.deck_pool[0], ==, 0); g_assert_cmpuint(plan->options.deck_pool[1], ==, 2);
        g_assert_cmpint(entry(plan, 0)->run.preferred_deck, ==, 0); g_assert_cmpint(entry(plan, 1)->run.preferred_deck, ==, 2);
        g_assert_cmpuint(entry(plan, 0)->run.mode, ==, mode); g_assert_cmpuint(entry(plan, 1)->run.mode, ==, mode);
        g_assert_cmpuint(entry(plan, 0)->run.overlap_frames, ==, expected_overlap[mode]);
        g_assert_cmpuint(entry(plan, 1)->run.fade_in_frames, ==, expected_overlap[mode]);
        g_assert_cmpint(entry(plan, 1)->planned_utc_us - entry(plan, 0)->planned_utc_us, ==, expected_next_us[mode]);
        g_assert_cmpuint(entry(plan, 0)->run.auto_next_frame, ==, mode == 2 ? 216000 : 288031);
        g_assert_false(entry(plan, 0)->run.beat_sync); g_assert_false(entry(plan, 1)->run.beat_sync);
        g_assert_cmpfloat(entry(plan, 0)->run.tempo_ratio, ==, 1); g_assert_cmpfloat(entry(plan, 1)->run.tempo_ratio, ==, 1);
        RadioSAudioRunItem *items = radios_playout_plan_items(plan);
        g_assert_cmpuint(items[0].mode, ==, mode); g_assert_cmpuint(items[0].overlap_frames, ==, expected_overlap[mode]);
        g_assert_cmpuint(items[0].auto_next_frame, ==, mode == 2 ? 216000 : 288031); g_free(items);
        gchar *after = json_to_string(input, FALSE); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after); json_node_free(input);
        radios_playout_plan_free(plan); json_object_unref(response);
    }
}
static JsonObject *routed_row(guint id, const guint *decks, guint count, guint mode, gboolean sync)
{
    JsonObject *track = row(id, "Different artist"), *route = json_object_new(), *hint = json_object_new(); JsonArray *pool = json_array_new();
    for (guint i = 0; i < count; ++i) json_array_add_int_element(pool, decks[i]);
    json_object_set_array_member(route, "decks", pool); json_object_set_object_member(track, "calendar_routing", route);
    json_object_set_int_member(hint, "mode", mode); json_object_set_int_member(hint, "overlap_ms", mode ? 500 : 0);
    if (sync) { json_object_set_boolean_member(hint, "beat_sync", TRUE); json_object_set_string_member(hint, "effect", "lowpass"); }
    json_object_set_object_member(track, "calendar_transition", hint); return track;
}
static void rhythm_row(JsonObject *track, gdouble bpm)
{
    RadioSRhythmAnalysis grid = {.bpm = bpm, .confidence = .9, .first_beat_ms = 125, .analyzed_ms = 5000};
    JsonObject *analysis = json_object_get_object_member(json_object_get_object_member(track, "detail"), "analysis");
    json_object_set_object_member(analysis, "rhythm", radios_rhythm_analysis_json(&grid));
}
static JsonNode *song_copy(JsonObject *object);
static void test_scope_single_routed_batch(void)
{
    JsonObject *p = prepared("programme"); const guint pool[] = {3, 8};
    JsonObject *track = routed_row(1, pool, 2, 0, FALSE);
    json_array_add_object_element(json_object_get_array_member(p, "tracks"), track);
    RadioSPlayoutRequest req = request(); rejected(p, &req);
    JsonNode *before = song_copy(p); req.allow_single_routed = TRUE;
    RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpuint(plan->entries->len, ==, 1); g_assert_true(plan->routed_rotation);
    g_assert_cmpint(entry(plan, 0)->run.preferred_deck, ==, 2);
    g_assert_true(json_object_equal(json_node_get_object(before), p));
    radios_playout_plan_free(plan); json_node_free(before);
    JsonObject *route = json_object_get_object_member(track, "calendar_routing");
    json_object_set_array_member(route, "decks", json_array_new()); rejected(p, &req);
    json_object_unref(p);
}

static void test_routed_mixed_pool(void)
{
    JsonObject *p = prepared("programme"), *config = json_object_new(); JsonArray *tracks = json_object_get_array_member(p, "tracks"), *legacy = json_array_new();
    json_array_add_int_element(legacy, 1); json_array_add_int_element(legacy, 3); json_object_set_array_member(config, "autodj_players", legacy);
    json_object_set_object_member(p, "config", config); const guint music[] = {1, 2}, ad[] = {3}, short_pool[] = {4};
    for (guint i = 0; i < 5; ++i) json_array_add_object_element(tracks, routed_row(i + 1, i < 2 ? music : i == 2 ? ad : short_pool, i < 2 ? 2 : 1, 0, FALSE));
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, p); gchar *before = json_to_string(node, FALSE);
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *a = build(p, &req), *b = build(p, &req);
    const gint expected[] = {0, 1, 2, 3, 3}; g_assert_true(a->routed_rotation); g_assert_true(a->player_autodj); g_assert_cmpuint(a->options.pool_count, ==, 4);
    for (guint i = 0; i < 5; ++i) { g_assert_cmpint(entry(a, i)->run.preferred_deck, ==, expected[i]); g_assert_cmpint(entry(b, i)->run.preferred_deck, ==, expected[i]); }
    for (guint i = 0; i < 4; ++i) g_assert_cmpuint(a->options.deck_pool[i], ==, i);
    gchar *after = json_to_string(node, FALSE); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after); json_node_free(node);
    radios_playout_plan_free(a); radios_playout_plan_free(b); json_object_unref(p);
}
static void test_routed_lookahead_validation(void)
{
    JsonObject *p = prepared("programme"); JsonArray *tracks = json_object_get_array_member(p, "tracks"); const guint pair[] = {1, 2}, fixed[] = {1};
    json_array_add_object_element(tracks, routed_row(1, pair, 2, 1, FALSE)); json_array_add_object_element(tracks, routed_row(2, pair, 2, 1, FALSE));
    JsonObject *last = routed_row(3, fixed, 1, 0, FALSE); json_array_add_object_element(tracks, last);
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpint(entry(plan, 0)->run.preferred_deck, ==, 0); g_assert_cmpint(entry(plan, 1)->run.preferred_deck, ==, 1); g_assert_cmpint(entry(plan, 2)->run.preferred_deck, ==, 0);
    radios_playout_plan_free(plan); json_object_remove_member(last, "calendar_routing"); rejected(p, &req);
    JsonObject *route = json_object_new(); JsonArray *decks = json_array_new(); json_array_add_int_element(decks, 1); json_array_add_int_element(decks, 1);
    json_object_set_array_member(route, "decks", decks); json_object_set_object_member(last, "calendar_routing", route); rejected(p, &req);
    json_array_remove_element(decks, 1); json_array_remove_element(decks, 0); json_array_add_string_element(decks, "1"); rejected(p, &req);
    json_array_remove_element(decks, 0); json_array_add_int_element(decks, 33); rejected(p, &req); json_array_remove_element(decks, 0); rejected(p, &req);
    json_array_add_int_element(decks, 1);
    JsonObject *middle = json_array_get_object_element(tracks, 1); json_object_set_object_member(middle, "calendar_routing", json_object_ref(route));
    /* Two consecutive items bound to one single player cannot overlap: only
     * that transition degrades to a cut and the block keeps playing. */
    plan = build(p, &req);
    g_assert_cmpuint(entry(plan, 1)->run.overlap_frames, ==, 0); g_assert_cmpint(entry(plan, 1)->run.mode, ==, RADIOS_AUDIO_RUN_EOF);
    g_assert_cmpint(entry(plan, 0)->run.preferred_deck, ==, 1); g_assert_cmpint(entry(plan, 1)->run.preferred_deck, ==, 0);
    g_assert_cmpint(entry(plan, 2)->run.preferred_deck, ==, 0);
    g_assert_cmpint(entry(plan, 2)->planned_utc_us, >=, entry(plan, 1)->planned_utc_us);
    radios_playout_plan_free(plan); json_object_unref(p);
}
static void unsynced(JsonObject *p, RadioSPlayoutRequest *req)
{
    RadioSPlayoutPlan *plan = build(p, req);
    g_assert_false(entry(plan, 0)->run.beat_sync); g_assert_cmpfloat(entry(plan, 1)->run.tempo_ratio, ==, 1);
    radios_playout_plan_free(plan);
}
static void test_routed_beatmatch_filter(void)
{
    JsonObject *p = prepared("programme"); JsonArray *tracks = json_object_get_array_member(p, "tracks"); const guint pool[] = {1, 2};
    JsonObject *a = routed_row(1, pool, 2, 1, TRUE), *b = routed_row(2, pool, 2, 0, FALSE); rhythm_row(a, 120); rhythm_row(b, 128);
    json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b); RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_true(entry(plan, 0)->run.beat_sync); g_assert_false(entry(plan, 1)->run.beat_sync);
    g_assert_cmpint(entry(plan, 0)->run.effect, ==, RADIOS_AUDIO_RUN_EFFECT_LOWPASS); g_assert_cmpint(entry(plan, 1)->run.effect, ==, RADIOS_AUDIO_RUN_EFFECT_NONE);
    g_assert_cmpfloat(entry(plan, 1)->run.tempo_ratio, ==, .9375); g_assert_cmpuint(entry(plan, 1)->run.media.cue_in_frame, ==, 6000);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us - entry(plan, 0)->planned_utc_us, ==, 5125000);
    g_assert_cmpuint(entry(plan, 1)->run.fade_in_frames, ==, 42031); radios_playout_plan_free(plan);
    /* An unknown or too distant tempo keeps the boundary a plain crossfade
     * instead of refusing the whole rotation. */
    JsonObject *grid = json_object_get_object_member(json_object_get_object_member(json_object_get_object_member(b, "detail"), "analysis"), "rhythm");
    for (guint variant = 0; variant < 2; ++variant) {
        json_object_set_double_member(grid, variant ? "bpm" : "confidence", variant ? 160 : .1);
        plan = build(p, &req);
        g_assert_false(entry(plan, 0)->run.beat_sync); g_assert_cmpfloat(entry(plan, 1)->run.tempo_ratio, ==, 1);
        g_assert_cmpint(entry(plan, 0)->run.mode, ==, RADIOS_AUDIO_RUN_CROSSFADE); g_assert_cmpuint(entry(plan, 1)->run.media.cue_in_frame, ==, 0);
        radios_playout_plan_free(plan);
        json_object_set_double_member(grid, "confidence", .9); json_object_set_double_member(grid, "bpm", 128);
    }
    JsonObject *analysis = json_object_get_object_member(json_object_get_object_member(b, "detail"), "analysis");
    gchar *original_sha = g_strdup(json_object_get_string_member(analysis, "sha256"));
    /* An analysis that does not match the verified media cannot drive tempo:
     * that boundary also stays a plain crossfade. */
    json_object_set_string_member(analysis, "sha256", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"); unsynced(p, &req);
    json_object_set_string_member(analysis, "sha256", original_sha); g_free(original_sha);
    json_object_set_int_member(analysis, "sample_rate", 0); unsynced(p, &req); json_object_set_int_member(analysis, "sample_rate", 48000);
    json_object_set_int_member(analysis, "sample_frames", 100); unsynced(p, &req); json_object_set_int_member(analysis, "sample_frames", 288031);
    JsonObject *hint = json_object_get_object_member(a, "calendar_transition"); json_object_set_string_member(hint, "effect", "unimplemented"); rejected(p, &req);
    json_object_set_string_member(hint, "effect", "lowpass"); json_object_set_int_member(hint, "beat_sync", 1); rejected(p, &req);
    json_object_set_boolean_member(hint, "beat_sync", TRUE); json_object_set_int_member(hint, "extra", 1); rejected(p, &req); json_object_unref(p);
}
static guint filter_diagnostic_count(RadioSPlayoutPlan *plan, const gchar *entry_id)
{
    JsonArray *diagnostics = json_object_get_array_member(json_node_get_object(plan->display), "diagnostics");
    guint count = 0;
    for (guint i = 0; i < json_array_get_length(diagnostics); ++i) {
        JsonObject *diagnostic = json_array_get_object_element(diagnostics, i);
        if (g_str_equal(json_object_get_string_member(diagnostic, "reason"), "transition_filter_without_overlap")) {
            g_assert_cmpstr(json_object_get_string_member(diagnostic, "entry_id"), ==, entry_id); ++count;
        }
    }
    return count;
}
static void assert_cue_filter_boundary(RadioSPlayoutPlan *plan, guint overlap_ms)
{
    RadioSPlayoutEntry *first = entry(plan, 0), *next = entry(plan, 1);
    g_assert_cmpuint(plan->entries->len, ==, 2);
    g_assert_cmpuint(plan->options.pool_count, ==, 2);
    g_assert_cmpint(first->run.mode, ==, RADIOS_AUDIO_RUN_CUE);
    g_assert_cmpuint(first->run.media.cue_in_frame, ==, 500 * 48);
    g_assert_cmpuint(first->run.media.cue_out_frame, ==, 6000 * 48);
    g_assert_cmpuint(first->run.auto_next_frame, ==, 4500 * 48);
    g_assert_cmpuint(first->run.fade_start_frame, ==, 4500 * 48);
    g_assert_cmpuint(first->run.fade_end_frame, ==, 6000 * 48);
    g_assert_cmpuint(first->run.overlap_frames, ==, overlap_ms * 48);
    g_assert_cmpuint(next->run.fade_in_frames, ==, overlap_ms * 48);
    g_assert_cmpint(next->planned_utc_us - first->planned_utc_us, ==, 4000000);
    g_assert_false(first->run.beat_sync);
    g_assert_cmpint(first->run.effect, ==, overlap_ms ? RADIOS_AUDIO_RUN_EFFECT_LOWPASS : RADIOS_AUDIO_RUN_EFFECT_NONE);
    g_assert_cmpuint(filter_diagnostic_count(plan, first->run.entry_id), ==, overlap_ms ? 0 : 1);
    JsonObject *display = json_array_get_object_element(json_object_get_array_member(json_node_get_object(plan->display), "entries"), 0);
    g_assert_cmpstr(json_object_get_string_member(display, "effect"), ==, overlap_ms ? "lowpass" : "none");
    g_assert_cmpint(json_object_get_int_member(display, "overlap_ms"), ==, overlap_ms);
}
static void test_filter_cue_zero_actual_overlap(void)
{
    for (guint variant = 0; variant < 2; ++variant) {
        const guint pool[] = {1, 2}; guint overlap_ms = variant ? 350 : 0;
        JsonObject *p = prepared("programme"), *a = routed_row(1, pool, 2, RADIOS_AUDIO_RUN_CUE, FALSE);
        JsonObject *b = routed_row(2, pool, 2, RADIOS_AUDIO_RUN_EOF, FALSE);
        JsonArray *tracks = json_object_get_array_member(p, "tracks");
        set_duration(a, 6000); set_markers(a, 6000, 4500, overlap_ms);
        JsonObject *markers = json_object_get_object_member(json_object_get_object_member(a, "detail"), "markers");
        json_object_set_int_member(markers, "cue_in_ms", 500);
        JsonObject *hint = json_object_get_object_member(a, "calendar_transition");
        json_object_set_string_member(hint, "effect", "lowpass");
        json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b);
        JsonNode *before = song_copy(p); RadioSPlayoutRequest req = request();
        RadioSPlayoutPlan *plan = build(p, &req);
        assert_cue_filter_boundary(plan, overlap_ms);
        g_assert_true(json_object_equal(json_node_get_object(before), p));
        json_node_free(before); radios_playout_plan_free(plan);
        /* A positive authenticated request may lose its runtime overlap;
         * an explicitly invalid zero-overlap filter request still fails. */
        json_object_set_int_member(hint, "overlap_ms", 0); before = song_copy(p);
        rejected(p, &req); g_assert_true(json_object_equal(json_node_get_object(before), p));
        json_node_free(before); json_object_unref(p);
    }
}
static void test_filter_scheduled_start_removes_overlap(void)
{
    const guint pool[] = {1, 2}; JsonObject *p = prepared("programme"), *config = json_object_new();
    JsonObject *a = routed_row(1, pool, 2, RADIOS_AUDIO_RUN_CROSSFADE, TRUE);
    JsonObject *b = routed_row(2, pool, 2, RADIOS_AUDIO_RUN_EOF, FALSE);
    set_duration(a, 6000); rhythm_row(a, 120); rhythm_row(b, 120);
    json_object_set_string_member(config, "date", "2026-10-06"); json_object_set_object_member(p, "config", config);
    JsonObject *metadata = json_object_get_object_member(b, "metadata");
    json_object_set_string_member(metadata, "start_time", "00:31"); json_object_set_string_member(metadata, "start_policy", "finish");
    json_object_set_int_member(metadata, "max_wait_ms", 250);
    JsonArray *tracks = json_object_get_array_member(p, "tracks");
    json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b);
    JsonNode *before = song_copy(p); RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpuint(plan->entries->len, ==, 2); g_assert_cmpuint(plan->options.pool_count, ==, 2);
    g_assert_cmpint(entry(plan, 0)->run.mode, ==, RADIOS_AUDIO_RUN_CROSSFADE);
    g_assert_cmpuint(entry(plan, 0)->run.media.cue_out_frame, ==, 6000 * 48);
    g_assert_cmpuint(entry(plan, 0)->run.overlap_frames, ==, 0);
    g_assert_false(entry(plan, 0)->run.beat_sync);
    g_assert_cmpint(entry(plan, 0)->run.effect, ==, RADIOS_AUDIO_RUN_EFFECT_NONE);
    g_assert_cmpuint(entry(plan, 1)->run.fade_in_frames, ==, 0);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us, ==, stamp("2026-10-06T00:31:00Z"));
    g_assert_cmpint(entry(plan, 1)->run.start_utc_us, ==, stamp("2026-10-06T00:31:00Z"));
    g_assert_cmpint(entry(plan, 1)->run.start_policy, ==, RADIOS_AUDIO_RUN_START_FINISH);
    g_assert_cmpuint(entry(plan, 1)->run.max_wait_frames, ==, 250 * 48);
    g_assert_cmpuint(filter_diagnostic_count(plan, entry(plan, 0)->run.entry_id), ==, 1);
    g_assert_true(json_object_equal(json_node_get_object(before), p));
    json_node_free(before); radios_playout_plan_free(plan); json_object_unref(p);
}
static void test_filter_probability_cue_zero_actual_overlap(void)
{
    const guint pool[] = {1, 2}; JsonObject *p = prepared("programme");
    JsonObject *a = routed_row(1, pool, 2, RADIOS_AUDIO_RUN_EOF, FALSE);
    JsonObject *b = routed_row(2, pool, 2, RADIOS_AUDIO_RUN_EOF, FALSE);
    set_duration(a, 6000); set_markers(a, 6000, 4500, 0);
    JsonObject *markers = json_object_get_object_member(json_object_get_object_member(a, "detail"), "markers");
    json_object_set_int_member(markers, "cue_in_ms", 500);
    JsonObject *policy = json_object_new(), *choice = json_object_new(); JsonArray *choices = json_array_new();
    gchar *choice_id = uuid(7001); json_object_set_string_member(choice, "id", choice_id); g_free(choice_id);
    json_object_set_int_member(choice, "mode", RADIOS_AUDIO_RUN_CUE); json_object_set_int_member(choice, "overlap_ms", 500);
    json_object_set_double_member(choice, "early_seconds", 0); json_object_set_double_member(choice, "probability", 100);
    json_object_set_string_member(choice, "effect", "lowpass"); json_array_add_object_element(choices, choice);
    json_object_set_int_member(policy, "schema", 1); json_object_set_string_member(policy, "seed", "zero-actual-overlap");
    json_object_set_array_member(policy, "transitions", choices); json_object_set_object_member(a, "probability_policy", policy);
    JsonArray *tracks = json_object_get_array_member(p, "tracks"); json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b);
    JsonNode *before = song_copy(p); RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_true(json_object_get_boolean_member(json_node_get_object(plan->display), "probability_applied"));
    assert_cue_filter_boundary(plan, 0);
    g_assert_true(json_object_equal(json_node_get_object(before), p));
    json_node_free(before); radios_playout_plan_free(plan);
    json_object_set_int_member(choice, "overlap_ms", 0); before = song_copy(p);
    rejected(p, &req); g_assert_true(json_object_equal(json_node_get_object(before), p));
    json_node_free(before); json_object_unref(p);
}
static JsonObject *constraints_row(JsonObject *track, gint64 begin, gint64 end, gboolean turn)
{
    JsonObject *c = json_object_new(); gchar *source = uuid(1000); json_object_set_string_member(track, "calendar_source_id", source); g_free(source);
    json_object_set_int_member(c, "earliest_utc_us", begin); json_object_set_int_member(c, "latest_end_utc_us", end);
    const gchar *keys[] = {"track_gap_ms", "artist_gap_ms", "album_gap_ms", "source_interval_ms"};
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) json_object_set_int_member(c, keys[i], 0);
    json_object_set_boolean_member(c, "source_turn_start", turn); json_object_set_object_member(track, "calendar_constraints", c); return c;
}
static void test_routed_actual_time_constraints(void)
{
    JsonObject *p = prepared("programme"); JsonArray *tracks = json_object_get_array_member(p, "tracks"); const guint pool[] = {1, 2};
    JsonObject *a = routed_row(1, pool, 2, 1, TRUE), *b = routed_row(2, pool, 2, 0, FALSE); rhythm_row(a, 120); rhythm_row(b, 128);
    json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b); RadioSPlayoutRequest req = request();
    JsonObject *ca = constraints_row(a, req.now_utc_us, req.now_utc_us + 12000000, TRUE);
    JsonObject *cb = constraints_row(b, req.now_utc_us, req.now_utc_us + 12000000, TRUE);
    RadioSPlayoutPlan *plan = build(p, &req); g_assert_cmpint(entry(plan, 1)->run.latest_start_utc_us, ==, req.now_utc_us + 5944625); radios_playout_plan_free(plan);
    json_object_set_int_member(cb, "latest_end_utc_us", req.now_utc_us + 11000000); rejected(p, &req); json_object_set_int_member(cb, "latest_end_utc_us", req.now_utc_us + 12000000);
    json_object_set_int_member(cb, "artist_gap_ms", 5300);
    plan = build(p, &req); g_assert_false(entry(plan, 0)->run.beat_sync); radios_playout_plan_free(plan); json_object_set_int_member(cb, "artist_gap_ms", 0);
    json_object_set_int_member(cb, "source_interval_ms", 5300);
    plan = build(p, &req); g_assert_false(entry(plan, 0)->run.beat_sync); radios_playout_plan_free(plan); json_object_set_int_member(cb, "source_interval_ms", 0);
    json_object_set_int_member(ca, "extra", 1); rejected(p, &req); json_object_unref(p);
}
static gchar *trim_input_text(JsonObject *prepared)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, prepared);
    gchar *text = json_to_string(node, FALSE); json_node_free(node); return text;
}
static void trim_rejected_immutable(JsonObject *prepared, RadioSPlayoutRequest *request)
{
    gchar *before = trim_input_text(prepared);
    JsonNode *history = request->history ? radios_playout_history_json(request->history) : NULL;
    rejected(prepared, request); gchar *after = trim_input_text(prepared);
    g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
    if (history) {
        JsonNode *current = radios_playout_history_json(request->history);
        g_assert_true(json_node_equal(history, current)); json_node_free(current); json_node_free(history);
    }
}
static void test_loaded_rotation_trims_calendar_tail(void)
{
    JsonObject *p = prepared("programme"); JsonArray *tracks = json_object_get_array_member(p, "tracks");
    JsonObject *a = row(1, "A"), *b = row(2, "B"); json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b);
    RadioSPlayoutRequest req = request();
    JsonObject *ca = constraints_row(a, req.now_utc_us, req.now_utc_us + 20000000, TRUE);
    JsonObject *cb = constraints_row(b, req.now_utc_us, req.now_utc_us + 10000000, FALSE);
    trim_rejected_immutable(p, &req); req.trim_calendar_end = TRUE;
    gchar *before = trim_input_text(p);
    RadioSPlayoutPlan *plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 1); radios_playout_plan_free(plan);
    gchar *after = trim_input_text(p); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
    /* A not-yet-open tail is not an expired tail and must remain invalid. */
    json_object_set_int_member(cb, "earliest_utc_us", req.now_utc_us + 7000000); trim_rejected_immutable(p, &req);
    json_object_set_int_member(cb, "earliest_utc_us", req.now_utc_us);
    /* The complete prepared set is verified before any suffix is removed. */
    JsonObject *detail = json_object_get_object_member(b, "detail");
    JsonObject *media = json_object_get_object_member(detail, "media"), *version = json_object_get_object_member(detail, "version");
    gchar *media_id = g_strdup(json_object_get_string_member(media, "_id"));
    json_object_set_string_member(media, "_id", "not-a-media-UUID"); trim_rejected_immutable(p, &req);
    json_object_set_string_member(media, "_id", media_id); g_free(media_id);
    gchar *version_id = g_strdup(json_object_get_string_member(version, "_id")), *foreign = uuid(9999);
    json_object_set_string_member(version, "_id", foreign); trim_rejected_immutable(p, &req);
    json_object_set_string_member(version, "_id", version_id); g_free(version_id); g_free(foreign);
    json_object_set_int_member(cb, "extra", 1); trim_rejected_immutable(p, &req); json_object_remove_member(cb, "extra");
    json_object_set_int_member(cb, "source_turn_start", 1); trim_rejected_immutable(p, &req);
    json_object_set_boolean_member(cb, "source_turn_start", FALSE);
    gchar *source_id = g_strdup(json_object_get_string_member(b, "calendar_source_id"));
    json_object_set_string_member(b, "calendar_source_id", "not-a-source-UUID"); trim_rejected_immutable(p, &req);
    json_object_set_string_member(b, "calendar_source_id", source_id); g_free(source_id);
    /* A fitting retained first item cannot bypass real consumed history. */
    JsonObject *first_detail = json_object_get_object_member(a, "detail");
    JsonObject *first_media = json_object_get_object_member(first_detail, "media"), *first_version = json_object_get_object_member(first_detail, "version");
    RadioSPlayoutHistory *history = radios_playout_history_new();
    radios_playout_history_observe(history, json_object_get_string_member(first_media, "_id"),
        json_object_get_string_member(first_version, "_id"), "A", "Albüm", req.now_utc_us - 500000, 1);
    req.history = history; const gchar *gaps[] = {"track_gap_ms", "artist_gap_ms", "album_gap_ms"};
    for (guint i = 0; i < G_N_ELEMENTS(gaps); ++i) {
        json_object_set_int_member(ca, gaps[i], 1000); trim_rejected_immutable(p, &req); json_object_set_int_member(ca, gaps[i], 0);
    }
    req.history = NULL; radios_playout_history_free(history);
    json_object_set_int_member(cb, "latest_end_utc_us", req.now_utc_us + 20000000);
    plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 2); radios_playout_plan_free(plan);
    json_object_set_int_member(ca, "latest_end_utc_us", req.now_utc_us + 5000000); trim_rejected_immutable(p, &req); json_object_unref(p);
}
static void rejected_with(JsonObject *p, RadioSPlayoutRequest *r, const gchar *message)
{
    GError *error = NULL; g_assert_null(radios_playout_plan_build(p, r, &error)); g_assert_nonnull(error);
    g_assert_cmpstr(error->message, ==, message); g_clear_error(&error);
}
static gboolean has_plan_diagnostic(RadioSPlayoutPlan *plan, const gchar *reason, guint entry_id)
{
    JsonArray *items = json_object_get_array_member(json_node_get_object(plan->display), "diagnostics"); gchar *expected = uuid(entry_id); gboolean found = FALSE;
    for (guint i = 0; !found && i < json_array_get_length(items); ++i) {
        JsonObject *item = json_array_get_object_element(items, i);
        found = g_str_equal(json_object_get_string_member(item, "reason"), reason) && g_str_equal(json_object_get_string_member(item, "entry_id"), expected);
    }
    g_free(expected); return found;
}
/* The calendar selection estimated its timeline before transitions, cue exits
 * and beatmatch set the real one: twenty 6 s songs with 1 s crossfades end
 * 19 s earlier than played back to back. A later song whose repeat gap only
 * held on the estimate is dropped with its tail under the schedule's trim
 * request (refilled later, like a tail past the block end); the same
 * violation on the first item, or without that request, still fails. */
static void test_calendar_repeat_gap_trims_compressed_timeline(void)
{
    const gchar *gap_message = "Tempo ayarlı gerçek çalma sırası parça, sanatçı veya albüm tekrar süresini karşılamıyor.";
    const gchar *turn_message = "Tempo ayarlı playlist sırası belirlenen playlist dönüş sıklığını karşılamıyor.";
    JsonObject *p = prepared("programme"); JsonArray *tracks = json_object_get_array_member(p, "tracks"); RadioSPlayoutRequest req = request();
    JsonObject *constraints[20];
    for (guint i = 0; i < 20; ++i) {
        gchar *artist = g_strdup_printf("Sanatçı %u", i); JsonObject *track = row(i + 1, artist), *hint = json_object_new(); g_free(artist);
        json_object_set_int_member(hint, "mode", 1); json_object_set_int_member(hint, "overlap_ms", 1000);
        json_object_set_object_member(track, "calendar_transition", hint); json_array_add_object_element(tracks, track);
        constraints[i] = constraints_row(track, req.now_utc_us, req.now_utc_us + 600 * G_USEC_PER_SEC, FALSE);
        json_object_set_int_member(constraints[i], "track_gap_ms", 2400000);
    }
    RadioSPlayoutPlan *plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 20);
    const gint64 step = 5000645; /* 288031 - 48000 frames */
    for (guint i = 0; i < 20; ++i) g_assert_cmpint(entry(plan, i)->planned_utc_us - req.now_utc_us, ==, (gint64)i * step);
    radios_playout_plan_free(plan);
    /* The last song last aired 2300 s ago: legal at the estimated +114 s,
     * not at the real +95 s. */
    gchar *media_id = uuid(120), *version_id = uuid(220); RadioSPlayoutHistory *history = radios_playout_history_new(); req.history = history;
    radios_playout_history_observe(history, media_id, version_id, "Sanatçı 19", "Albüm", req.now_utc_us - (gint64)2300 * G_USEC_PER_SEC, 1);
    rejected_with(p, &req, gap_message);
    req.trim_calendar_end = TRUE; gchar *before = trim_input_text(p); JsonNode *saved = radios_playout_history_json(history);
    plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 19);
    g_assert_true(has_plan_diagnostic(plan, "calendar_repeat_gap_trimmed", 20));
    for (guint i = 0; i < 19; ++i) g_assert_cmpint(entry(plan, i)->planned_utc_us - req.now_utc_us, ==, (gint64)i * step);
    g_assert_cmpint(entry(plan, 18)->run.latest_start_utc_us, ==, req.now_utc_us + 600 * G_USEC_PER_SEC - 6000645);
    radios_playout_plan_free(plan);
    gchar *after = trim_input_text(p); g_assert_cmpstr(before, ==, after); g_free(after); g_free(before);
    JsonNode *current = radios_playout_history_json(history); g_assert_true(json_node_equal(saved, current)); json_node_free(current); json_node_free(saved);
    /* The twentieth song is the first one again: a repeat 95 s after its own
     * play inside the same batch, where the estimate had 114 s. */
    radios_playout_history_free(history); req.history = NULL; g_free(media_id); g_free(version_id);
    JsonObject *twentieth = json_array_get_object_element(tracks, 19);
    json_object_set_member(twentieth, "detail", song_copy(json_object_get_object_member(json_array_get_object_element(tracks, 0), "detail")));
    json_object_set_int_member(constraints[19], "track_gap_ms", 100000);
    plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 19); g_assert_true(has_plan_diagnostic(plan, "calendar_repeat_gap_trimmed", 20)); radios_playout_plan_free(plan);
    req.trim_calendar_end = FALSE; rejected_with(p, &req, gap_message); req.trim_calendar_end = TRUE;
    json_object_set_int_member(constraints[19], "track_gap_ms", 95000);
    plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 20); g_assert_false(has_plan_diagnostic(plan, "calendar_repeat_gap_trimmed", 20)); radios_playout_plan_free(plan);
    /* A playlist turn that only kept its interval on the estimate. */
    json_object_set_int_member(constraints[19], "track_gap_ms", 0);
    json_object_set_boolean_member(constraints[0], "source_turn_start", TRUE); json_object_set_boolean_member(constraints[19], "source_turn_start", TRUE);
    json_object_set_int_member(constraints[19], "source_interval_ms", 100000);
    plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 19); g_assert_true(has_plan_diagnostic(plan, "calendar_turn_interval_trimmed", 20)); radios_playout_plan_free(plan);
    req.trim_calendar_end = FALSE; rejected_with(p, &req, turn_message); req.trim_calendar_end = TRUE;
    json_object_set_int_member(constraints[19], "source_interval_ms", 0); json_object_set_int_member(constraints[19], "track_gap_ms", 2400000);
    /* An early first item is never trimmed: the whole batch is retried. */
    history = radios_playout_history_new(); req.history = history; media_id = uuid(101); version_id = uuid(201);
    radios_playout_history_observe(history, media_id, version_id, "Sanatçı 0", "Albüm", req.now_utc_us - G_USEC_PER_SEC, 1);
    before = trim_input_text(p); rejected_with(p, &req, gap_message);
    after = trim_input_text(p); g_assert_cmpstr(before, ==, after); g_free(after); g_free(before);
    radios_playout_history_free(history); g_free(media_id); g_free(version_id); json_object_unref(p);
}

/* These integration fixtures exercise verified selection/phase-two/native
 * planning without acquiring a decoder, player lease or production database. */
static JsonObject *song_at(JsonObject *object, const gchar *member, guint index)
{ return json_array_get_object_element(json_object_get_array_member(object, member), index); }
static JsonNode *song_copy(JsonObject *object)
{
    JsonNode *input = json_node_new(JSON_NODE_OBJECT); json_node_set_object(input, object);
    gchar *value = json_to_string(input, FALSE); JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, value, -1, NULL));
    JsonNode *copy = json_node_copy(json_parser_get_root(parser));
    g_object_unref(parser); g_free(value); json_node_free(input); return copy;
}
static void song_transition(JsonObject *settings, guint mode)
{
    JsonArray *choices = json_array_new(); JsonObject *choice = json_object_new();
    json_object_set_int_member(choice, "mode", mode); json_object_set_int_member(choice, "weight", 100);
    json_object_set_int_member(choice, "overlap_ms", mode ? 500 : 0);
    json_array_add_object_element(choices, choice); json_object_set_array_member(settings, "transitions", choices);
}
static JsonObject *song_source(guint id)
{
    JsonObject *source = json_object_new(); gchar *sid = uuid(id + 1000), *pid = uuid(id + 2000);
    json_object_set_string_member(source, "id", sid); json_object_set_string_member(source, "playlist_id", pid);
    json_object_set_int_member(source, "weight", 100); json_object_set_int_member(source, "frequency", 1);
    json_object_set_int_member(source, "count", 1); json_object_set_int_member(source, "start_minute", 0);
    json_object_set_int_member(source, "end_minute", 1440); json_object_set_int_member(source, "interval_minutes", 0);
    const gchar *flags[] = {"random", "track_random", "advert_random", "sweeper_random"};
    for (guint i = 0; i < G_N_ELEMENTS(flags); ++i) json_object_set_boolean_member(source, flags[i], FALSE);
    json_object_set_string_member(source, "track_selection_mode", "linear");
    json_object_set_array_member(source, "track_rules", json_array_new()); g_free(sid); g_free(pid); return source;
}
static JsonObject *song_calendar(JsonObject **event, JsonObject **definition, JsonObject **source)
{
    JsonObject *calendar = json_object_new(), *settings = json_object_new(), *assignment = json_object_new();
    *event = json_object_new(); *definition = json_object_new(); *source = song_source(1);
    gchar *eid = uuid(900), *rid = uuid(901), *aid = uuid(902);
    json_object_set_string_member(*event, "id", eid); json_object_set_string_member(*event, "title", "Song rule integration");
    json_object_set_string_member(*event, "date", "2026-10-06"); json_object_set_int_member(*event, "start_minute", 30);
    json_object_set_int_member(*event, "end_minute", 120); json_object_set_array_member(*event, "rotations", json_array_new());
    json_object_set_string_member(*definition, "id", rid); json_object_set_string_member(*definition, "name", "Verified song policy");
    json_object_set_string_member(settings, "selection_mode", "linear");
    const gchar *gaps[] = {"track_gap_minutes", "artist_gap_minutes", "album_gap_minutes"};
    for (guint i = 0; i < G_N_ELEMENTS(gaps); ++i) json_object_set_int_member(settings, gaps[i], 0);
    song_transition(settings, 0); json_object_set_object_member(*definition, "settings", settings);
    JsonArray *sources = json_array_new(); json_array_add_object_element(sources, *source);
    json_object_set_array_member(*definition, "sources", sources);
    json_object_set_string_member(assignment, "id", aid); json_object_set_string_member(assignment, "rotation_id", rid);
    json_array_add_object_element(json_object_get_array_member(*event, "rotations"), assignment);
    JsonArray *library = json_array_new(), *events = json_array_new();
    json_array_add_object_element(library, *definition); json_array_add_object_element(events, *event);
    json_object_set_array_member(calendar, "rotation_library", library); json_object_set_array_member(calendar, "events", events);
    json_object_set_int_member(calendar, "schema", 1); g_free(eid); g_free(rid); g_free(aid); return calendar;
}
static JsonObject *song_policy(JsonObject *source, guint id, guint weight, guint order)
{
    JsonObject *policy = json_object_new(); gchar *eid = uuid(id);
    json_object_set_string_member(policy, "entry_id", eid); g_free(eid);
    json_object_set_int_member(policy, "weight", weight); json_object_set_int_member(policy, "order", order);
    json_object_set_int_member(policy, "frequency", 1); json_object_set_int_member(policy, "max_plays", 0);
    json_object_set_int_member(policy, "start_minute", 0); json_object_set_int_member(policy, "end_minute", 1440);
    json_object_set_int_member(policy, "interval_minutes", 0);
    json_array_add_object_element(json_object_get_array_member(source, "track_rules"), policy); return policy;
}
static JsonNode *song_resolve(JsonObject *calendar, JsonObject *event)
{
    GError *error = NULL; JsonNode *resolved = radios_automation_calendar_resolve_event(calendar, event, &error);
    g_assert_no_error(error); g_assert_nonnull(resolved); return resolved;
}
static JsonObject *song_prepared(JsonObject *resolved, guint songs)
{
    JsonObject *response = prepared("automation"); JsonArray *rules = json_object_get_array_member(resolved, "rotations");
    for (guint i = 0; i < json_array_get_length(rules); ++i) {
        JsonObject *rule = json_array_get_object_element(rules, i), *source = json_object_new();
        json_object_set_string_member(source, "source_id", json_object_get_string_member(rule, "id"));
        json_object_set_string_member(source, "playlist_id", json_object_get_string_member(rule, "playlist_id"));
        json_object_set_string_member(source, "source_name", "Authenticated saved playlist");
        json_object_set_array_member(source, "reasons", json_array_new()); JsonArray *tracks = json_array_new();
        for (guint j = 0; j < songs; ++j) json_array_add_object_element(tracks, row(i * 10 + j + 1, "Selected artist"));
        json_object_set_array_member(source, "tracks", tracks); json_array_add_object_element(json_object_get_array_member(response, "sources"), source);
    }
    return response;
}
static JsonNode *song_select(JsonObject *resolved, JsonObject *response, guint limit)
{
    GError *error = NULL; JsonNode *selected = radios_automation_calendar_prepared_programme(resolved, response, station, "song-native-fixed", limit, &error);
    g_assert_no_error(error); g_assert_nonnull(selected); return selected;
}
static JsonNode *song_fresh_merge(JsonNode *selection)
{
    JsonObject *selected = json_node_get_object(selection); JsonNode *fresh = song_copy(selected), *selection_before = song_copy(selected);
    JsonObject *verified = json_node_get_object(fresh); JsonArray *tracks = json_object_get_array_member(verified, "tracks");
    for (guint i = 0; i < json_array_get_length(tracks); ++i) {
        JsonObject *track = json_array_get_object_element(tracks, i);
        const gchar *hints[] = {"calendar_transition", "calendar_routing", "calendar_constraints", "calendar_source_id", "calendar_group_id", "calendar_rotation_id"};
        for (guint j = 0; j < G_N_ELEMENTS(hints); ++j) json_object_remove_member(track, hints[j]);
        JsonObject *media = json_object_get_object_member(json_object_get_object_member(track, "detail"), "media");
        gchar *title = g_strdup_printf("Fresh %s", json_object_get_string_member(media, "_id"));
        json_object_set_string_member(media, "title", title);
        json_object_set_string_member(json_object_get_object_member(track, "metadata"), "title", title); g_free(title);
    }
    JsonNode *before = song_copy(verified); GError *error = NULL;
    JsonNode *merged = radios_automation_calendar_merge_selection(verified, selected, &error);
    g_assert_no_error(error); g_assert_nonnull(merged); g_assert_true(json_node_equal(before, fresh));
    g_assert_true(json_node_equal(selection_before, selection));
    json_node_free(before); json_node_free(selection_before); json_node_free(fresh); return merged;
}
static const gchar *song_media(JsonNode *selection, guint index)
{
    JsonObject *track = song_at(json_node_get_object(selection), "tracks", index);
    return json_object_get_string_member(json_object_get_object_member(json_object_get_object_member(track, "detail"), "media"), "_id");
}
static void song_native_identity(RadioSPlayoutPlan *plan, JsonNode *selection)
{
    JsonArray *tracks = json_object_get_array_member(json_node_get_object(selection), "tracks");
    g_assert_cmpuint(plan->entries->len, ==, json_array_get_length(tracks));
    for (guint i = 0; i < plan->entries->len; ++i) {
        const gchar *mid = song_media(selection, i); gchar *title = g_strdup_printf("Fresh %s", mid);
        g_assert_cmpstr(entry(plan, i)->run.media.media_id, ==, mid); g_assert_cmpstr(entry(plan, i)->title, ==, title); g_free(title);
    }
}
static void test_song_linear_disabled_order_count(void)
{
    JsonObject *event, *definition, *source, *calendar = song_calendar(&event, &definition, &source);
    song_policy(source, 1, 0, 0); JsonObject *second = song_policy(source, 2, 100, 20), *third = song_policy(source, 3, 100, 10);
    json_object_set_int_member(second, "max_plays", 1); json_object_set_int_member(third, "max_plays", 1);
    JsonObject *disabled = song_source(2); json_object_set_int_member(disabled, "weight", 0);
    json_array_add_object_element(json_object_get_array_member(definition, "sources"), disabled);
    JsonNode *resolved = song_resolve(calendar, event); JsonObject *response = song_prepared(json_node_get_object(resolved), 3);
    JsonNode *selected = song_select(json_node_get_object(resolved), response, 20);
    gchar *three = uuid(103), *two = uuid(102); g_assert_cmpstr(song_media(selected, 0), ==, three); g_assert_cmpstr(song_media(selected, 1), ==, two);
    JsonNode *merged = song_fresh_merge(selected); RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(json_node_get_object(merged), &req);
    g_assert_cmpuint(plan->entries->len, ==, 2); song_native_identity(plan, selected);
    JsonNode *again = song_select(json_node_get_object(resolved), response, 20);
    g_assert_cmpstr(song_media(again, 0), ==, three); g_assert_cmpstr(song_media(again, 1), ==, two);
    radios_playout_plan_free(plan); json_node_free(again); json_node_free(merged); json_node_free(selected); json_node_free(resolved);
    json_object_unref(response); json_object_unref(calendar); g_free(three); g_free(two);
}
static void test_song_weighted_fresh_native_metadata(void)
{
    JsonObject *event, *definition, *source, *calendar = song_calendar(&event, &definition, &source);
    json_object_set_string_member(json_object_get_object_member(definition, "settings"), "selection_mode", "weighted");
    json_object_set_string_member(source, "track_selection_mode", "weighted");
    song_policy(source, 1, 0, 0); song_policy(source, 2, 80, 1); song_policy(source, 3, 20, 2);
    JsonObject *disabled = song_source(2); json_object_set_int_member(disabled, "weight", 0);
    json_array_add_object_element(json_object_get_array_member(definition, "sources"), disabled);
    JsonNode *resolved = song_resolve(calendar, event); JsonObject *response = song_prepared(json_node_get_object(resolved), 3);
    JsonNode *selected = song_select(json_node_get_object(resolved), response, 100), *again = song_select(json_node_get_object(resolved), response, 100);
    gchar *two = uuid(102), *three = uuid(103); guint high = 0, low = 0;
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(selected), "tracks")), ==, 100);
    for (guint i = 0; i < 100; ++i) {
        const gchar *mid = song_media(selected, i); g_assert_cmpstr(mid, ==, song_media(again, i));
        g_assert_true(g_str_equal(mid, two) || g_str_equal(mid, three)); high += g_str_equal(mid, two); low += g_str_equal(mid, three);
    }
    g_assert_cmpuint(high, >, low); g_assert_cmpuint(low, >, 0);
    JsonNode *merged = song_fresh_merge(selected); RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(json_node_get_object(merged), &req);
    song_native_identity(plan, selected); radios_playout_plan_free(plan); json_node_free(merged); json_node_free(selected); json_node_free(again);
    json_node_free(resolved); json_object_unref(response); json_object_unref(calendar); g_free(two); g_free(three);
}
static void test_song_UTC_intersection_actual_media_history(void)
{
    JsonObject *event, *definition, *source, *calendar = song_calendar(&event, &definition, &source);
    json_object_set_int_member(event, "end_minute", 40); json_object_set_int_member(source, "start_minute", 29); json_object_set_int_member(source, "end_minute", 35);
    json_object_set_int_member(json_object_get_object_member(definition, "settings"), "track_gap_minutes", 1);
    JsonObject *policy = song_policy(source, 1, 100, 0); json_object_set_int_member(policy, "start_minute", 30);
    json_object_set_int_member(policy, "end_minute", 31); json_object_set_int_member(policy, "interval_minutes", 2);
    JsonNode *resolved = song_resolve(calendar, event); JsonObject *response = song_prepared(json_node_get_object(resolved), 1);
    JsonNode *selected = song_select(json_node_get_object(resolved), response, 1), *merged = song_fresh_merge(selected);
    JsonObject *hint = json_object_get_object_member(song_at(json_node_get_object(merged), "tracks", 0), "calendar_constraints");
    RadioSPlayoutRequest req = request(); g_assert_cmpuint(json_object_get_size(hint), ==, 7);
    g_assert_cmpint(json_object_get_int_member(hint, "earliest_utc_us"), ==, req.now_utc_us);
    g_assert_cmpint(json_object_get_int_member(hint, "latest_end_utc_us"), ==, stamp("2026-10-06T00:31:00Z"));
    g_assert_cmpint(json_object_get_int_member(hint, "track_gap_ms"), ==, 120000);
    gchar *mid = uuid(101), *vid = uuid(201); RadioSPlayoutHistory *history = radios_playout_history_new(); req.history = history;
    radios_playout_history_observe(history, mid, vid, "Different cached artist", "Different cached album", req.now_utc_us - 120000000, 1);
    RadioSPlayoutPlan *plan = build(json_node_get_object(merged), &req); song_native_identity(plan, selected); radios_playout_plan_free(plan);
    radios_playout_history_observe(history, mid, vid, "Unrelated cached metadata", "Another album", req.now_utc_us - 119999000, 1);
    JsonNode *before = radios_playout_history_json(history); rejected(json_node_get_object(merged), &req);
    JsonNode *after = radios_playout_history_json(history); g_assert_true(json_node_equal(before, after));
    JsonObject *history_record = json_object_new(); json_object_set_member(history_record, "config", json_node_copy(after));
    json_object_set_object_member(response, "history", history_record); GError *error = NULL;
    g_assert_null(radios_automation_calendar_prepared_programme(json_node_get_object(resolved), response, station, "song-native-fixed", 1, &error));
    g_assert_nonnull(error); g_clear_error(&error); json_node_free(before); json_node_free(after); radios_playout_history_free(history);
    json_node_free(merged); json_node_free(selected); json_node_free(resolved); json_object_unref(response);
    json_object_unref(calendar); g_free(mid); g_free(vid);
}
static JsonObject *song_tempo_fixture(JsonObject *resolved, guint incoming_duration)
{
    JsonObject *response = song_prepared(resolved, 2), *source = song_at(response, "sources", 0);
    JsonObject *a = song_at(source, "tracks", 0), *b = song_at(source, "tracks", 1);
    set_duration(a, 4000); set_duration(b, incoming_duration); rhythm_row(a, 120); rhythm_row(b, 128);
    JsonObject *analysis = json_object_get_object_member(json_object_get_object_member(a, "detail"), "analysis");
    json_object_set_int_member(json_object_get_object_member(analysis, "rhythm"), "analyzed_ms", 4000); return response;
}
static void test_song_posttempo_minute_window_rejection(void)
{
    JsonObject *event, *definition, *source, *calendar = song_calendar(&event, &definition, &source);
    JsonObject *settings = json_object_get_object_member(definition, "settings"); song_transition(settings, 1);
    JsonObject *a = song_policy(source, 1, 100, 0), *b = song_policy(source, 2, 100, 1);
    json_object_set_int_member(a, "max_plays", 1); json_object_set_int_member(b, "max_plays", 1);
    json_object_set_int_member(b, "start_minute", 30); json_object_set_int_member(b, "end_minute", 31);
    RadioSPlayoutRequest req = request(); JsonNode *resolved = song_resolve(calendar, event);
    JsonObject *response = song_tempo_fixture(json_node_get_object(resolved), 56000);
    JsonNode *selected = song_select(json_node_get_object(resolved), response, 2), *merged = song_fresh_merge(selected);
    RadioSPlayoutPlan *plan = build(json_node_get_object(merged), &req); g_assert_cmpuint(plan->entries->len, ==, 2);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us - req.now_utc_us, ==, 3500000); g_assert_cmpfloat(entry(plan, 1)->run.tempo_ratio, ==, 1);
    radios_playout_plan_free(plan); json_node_free(merged); json_node_free(selected); json_node_free(resolved); json_object_unref(response);
    song_transition(settings, 3); resolved = song_resolve(calendar, event); response = song_tempo_fixture(json_node_get_object(resolved), 56000);
    selected = song_select(json_node_get_object(resolved), response, 2); merged = song_fresh_merge(selected);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(merged), "tracks")), ==, 2);
    JsonNode *before = song_copy(json_node_get_object(merged)); GError *error = NULL;
    plan = radios_playout_plan_build(json_node_get_object(merged), &req, &error); g_assert_nonnull(plan); g_assert_no_error(error);
    g_assert_true(entry(plan, 0)->run.beat_sync); g_assert_cmpuint(entry(plan, 1)->run.tempo_return_frames, ==, 192000); radios_playout_plan_free(plan); g_assert_true(json_node_equal(before, merged));
    json_node_free(before); json_node_free(merged); json_node_free(selected); json_node_free(resolved); json_object_unref(response); json_object_unref(calendar);
}
static void test_song_posttempo_artist_album_repeat_rejection(void)
{
    const gchar *gaps[] = {"artist_gap_minutes", "album_gap_minutes"};
    for (guint which = 0; which < G_N_ELEMENTS(gaps); ++which) {
        JsonObject *event, *definition, *source, *calendar = song_calendar(&event, &definition, &source);
        JsonObject *settings = json_object_get_object_member(definition, "settings"); song_transition(settings, 3);
        json_object_set_int_member(settings, gaps[which], 1);
        JsonObject *a = song_policy(source, 1, 100, 0), *b = song_policy(source, 2, 100, 1);
        json_object_set_int_member(a, "max_plays", 1); json_object_set_int_member(b, "max_plays", 1);
        JsonNode *resolved = song_resolve(calendar, event); JsonObject *response = song_tempo_fixture(json_node_get_object(resolved), 6000);
        JsonObject *source_rows = song_at(response, "sources", 0), *first = song_at(source_rows, "tracks", 0), *second = song_at(source_rows, "tracks", 1);
        JsonObject *ma = json_object_get_object_member(first, "metadata"), *mb = json_object_get_object_member(second, "metadata");
        json_object_set_string_member(ma, "artist", "Opening artist"); json_object_set_string_member(ma, "album", "Opening album");
        json_object_set_string_member(mb, "artist", "IŞIK"); json_object_set_string_member(mb, "album", "Repeat album");
        RadioSPlayoutRequest req = request(); RadioSPlayoutHistory *history = radios_playout_history_new(); req.history = history;
        gchar *mid = uuid(999), *vid = uuid(998);
        radios_playout_history_observe(history, mid, vid, "ışık", "repeat album", req.now_utc_us - 56500000, 1);
        JsonNode *saved = radios_playout_history_json(history); JsonObject *record = json_object_new();
        json_object_set_member(record, "config", json_node_copy(saved)); json_object_set_object_member(response, "history", record);
        JsonNode *selected = song_select(json_node_get_object(resolved), response, 2), *merged = song_fresh_merge(selected);
        g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(merged), "tracks")), ==, 2);
        /* Nominal crossfade starts at +3.5 s; measured beat snapping moves it
         * to +3.125 s, below the required sixty seconds since durable history. */
        JsonNode *nominal = song_copy(json_node_get_object(merged));
        for (guint i = 0; i < 2; ++i) json_object_remove_member(json_object_get_object_member(song_at(json_node_get_object(nominal), "tracks", i), "calendar_transition"), "beat_sync");
        RadioSPlayoutPlan *plan = build(json_node_get_object(nominal), &req); g_assert_cmpint(entry(plan, 1)->planned_utc_us - req.now_utc_us, ==, 3500000);
        radios_playout_plan_free(plan); json_node_free(nominal); GError *error = NULL;
        plan = radios_playout_plan_build(json_node_get_object(merged), &req, &error); g_assert_nonnull(plan); g_assert_no_error(error);
        g_assert_false(entry(plan, 0)->run.beat_sync); g_assert_cmpint(entry(plan, 1)->planned_utc_us - req.now_utc_us, ==, 3500000); radios_playout_plan_free(plan);
        JsonNode *after = radios_playout_history_json(history); g_assert_true(json_node_equal(saved, after));
        json_node_free(after); json_node_free(saved); radios_playout_history_free(history); g_free(mid); g_free(vid);
        json_node_free(merged); json_node_free(selected); json_node_free(resolved); json_object_unref(response); json_object_unref(calendar);
    }
}

static JsonObject *central_policy(guint mode, guint overlap, gboolean beat)
{
    JsonObject *policy = json_object_new(); json_object_set_int_member(policy, "schema", 1); json_object_set_int_member(policy, "mode", mode);
    json_object_set_int_member(policy, "overlap_ms", overlap); json_object_set_boolean_member(policy, "beat_sync", beat); return policy;
}
static void test_central_policy_and_autodj_marks(void)
{
    JsonObject *p = prepared("programme"); JsonArray *tracks = json_object_get_array_member(p, "tracks");
    JsonObject *a = row(1, "A"), *b = row(2, "B"); json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b);
    JsonObject *detail = json_object_get_object_member(a, "detail"), *media = json_object_get_object_member(detail, "media"), *version = json_object_get_object_member(detail, "version");
    JsonObject *marks = json_object_new(); json_object_set_string_member(marks, "station_id", station);
    json_object_set_string_member(marks, "version_id", json_object_get_string_member(version, "_id"));
    json_object_set_string_member(marks, "media_file_id", json_object_get_string_member(media, "media_file_id"));
    json_object_set_string_member(marks, "media_sha256", json_object_get_string_member(version, "sha256"));
    json_object_set_int_member(marks, "media_duration_ms", 6001); json_object_set_int_member(marks, "cue_in_ms", 0); json_object_set_int_member(marks, "out_ms", 6001);
    json_object_set_int_member(marks, "autodj_in_ms", 1000); json_object_set_int_member(marks, "autodj_out_ms", 4000); json_object_set_object_member(detail, "markers", marks);
    json_object_set_object_member(p, "playout_policy", central_policy(1, 500, TRUE)); RadioSPlayoutRequest req = request();
    RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpuint(entry(plan, 0)->run.media.cue_in_frame, ==, 48000); g_assert_cmpuint(entry(plan, 0)->run.media.cue_out_frame, ==, 288031);
    g_assert_cmpuint(entry(plan, 0)->run.autodj_out_frame, ==, 192000); g_assert_cmpuint(entry(plan, 0)->run.overlap_frames, ==, 24000);
    g_assert_false(entry(plan, 0)->run.beat_sync); g_assert_cmpint(entry(plan, 1)->planned_utc_us - entry(plan, 0)->planned_utc_us, ==, 3000000); radios_playout_plan_free(plan);
    rhythm_row(a, 120); rhythm_row(b, 120);
    plan = build(p, &req); g_assert_true(entry(plan, 0)->run.beat_sync);
    g_assert_cmpuint(entry(plan, 1)->run.fade_in_frames, ==, 24000);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us - entry(plan, 0)->planned_utc_us, ==, 2625000);
    radios_playout_plan_free(plan);
    req.has_default_policy = TRUE; req.default_mode = 0; req.default_overlap_seconds = 0;
    plan = build(p, &req); g_assert_cmpint(entry(plan, 0)->run.mode, ==, RADIOS_AUDIO_RUN_EOF); radios_playout_plan_free(plan);
    JsonObject *hint = json_object_new(); json_object_set_int_member(hint, "mode", 1); json_object_set_int_member(hint, "overlap_ms", 750);
    json_object_set_object_member(a, "calendar_transition", hint); plan = build(p, &req);
    g_assert_cmpuint(entry(plan, 0)->run.overlap_frames, ==, 36000); radios_playout_plan_free(plan);
    json_object_set_int_member(marks, "autodj_out_ms", 1000); rejected(p, &req);
    json_object_set_null_member(marks, "autodj_out_ms"); json_object_set_null_member(marks, "autodj_in_ms");
    plan = build(p, &req); g_assert_cmpuint(entry(plan, 0)->run.media.cue_in_frame, ==, 0); g_assert_cmpuint(entry(plan, 0)->run.autodj_out_frame, ==, 0);
    radios_playout_plan_free(plan); json_object_unref(p);
}
/* Marked media without an AutoDJ OUT leaves where its sound ends: the
 * trailing silence is not aired, a short tail and manual play are kept. */
static void test_marked_trailing_silence_autodj_exit(void)
{
    JsonObject *p = prepared("programme"); JsonArray *tracks = json_object_get_array_member(p, "tracks");
    JsonObject *a = row(1, "A"), *b = row(2, "B"); json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b);
    JsonObject *detail = json_object_get_object_member(a, "detail"), *media = json_object_get_object_member(detail, "media");
    JsonObject *version = json_object_get_object_member(detail, "version"), *marks = json_object_new(), *audible = json_object_new();
    json_object_set_string_member(marks, "station_id", station);
    json_object_set_string_member(marks, "version_id", json_object_get_string_member(version, "_id"));
    json_object_set_string_member(marks, "media_file_id", json_object_get_string_member(media, "media_file_id"));
    json_object_set_string_member(marks, "media_sha256", json_object_get_string_member(version, "sha256"));
    json_object_set_int_member(marks, "media_duration_ms", 6001); json_object_set_int_member(marks, "cue_in_ms", 0); json_object_set_int_member(marks, "out_ms", 6001);
    json_object_set_object_member(detail, "markers", marks);
    json_object_set_int_member(audible, "start_ms", 0); json_object_set_int_member(audible, "end_ms", 4500);
    json_object_set_object_member(json_object_get_object_member(detail, "analysis"), "audible", audible);
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpuint(entry(plan, 0)->run.media.cue_out_frame, ==, 4500 * 48);
    g_assert_cmpuint(entry(plan, 0)->run.autodj_out_frame, ==, 0);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us - entry(plan, 0)->planned_utc_us, ==, 4500000); radios_playout_plan_free(plan);
    /* The fade starts before the measured sound end, retaining its audible
     * tail rather than crossfading after it into the file's silent padding. */
    req.mode = RADIOS_AUDIO_RUN_CROSSFADE; req.overlap_seconds = 1;
    plan = build(p, &req);
    g_assert_cmpuint(entry(plan, 0)->run.media.cue_out_frame, ==, 4500 * 48);
    g_assert_cmpuint(entry(plan, 0)->run.overlap_frames, ==, 48000);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us - entry(plan, 0)->planned_utc_us, ==, 3500000);
    radios_playout_plan_free(plan);
    /* An explicit rhythm OUT starts that fade sooner; it does not truncate
     * the decoded source span to the mix point. */
    json_object_set_int_member(marks, "autodj_in_ms", 1000); json_object_set_int_member(marks, "autodj_out_ms", 3000);
    JsonNode *original = song_copy(p); plan = build(p, &req);
    g_assert_cmpuint(entry(plan, 0)->run.media.cue_in_frame, ==, 48000);
    g_assert_cmpuint(entry(plan, 0)->run.media.cue_out_frame, ==, 216000);
    g_assert_cmpuint(entry(plan, 0)->run.autodj_out_frame, ==, 144000);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us - entry(plan, 0)->planned_utc_us, ==, 2000000);
    radios_playout_plan_free(plan);
    req.single_player = TRUE; plan = build(p, &req);
    g_assert_cmpuint(entry(plan, 0)->run.media.cue_in_frame, ==, 0);
    g_assert_cmpuint(entry(plan, 0)->run.media.cue_out_frame, ==, 216000);
    g_assert_cmpuint(entry(plan, 0)->run.autodj_out_frame, ==, 0);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us - entry(plan, 0)->planned_utc_us, ==, 4500000);
    JsonNode *after = song_copy(p); g_assert_true(json_node_equal(original, after));
    json_node_free(original); json_node_free(after); radios_playout_plan_free(plan);
    req.single_player = FALSE; req.mode = RADIOS_AUDIO_RUN_EOF;
    json_object_remove_member(marks, "autodj_in_ms"); json_object_remove_member(marks, "autodj_out_ms");
    json_object_set_int_member(audible, "end_ms", 5900); plan = build(p, &req);
    g_assert_cmpuint(entry(plan, 0)->run.autodj_out_frame, ==, 0); radios_playout_plan_free(plan);
    json_object_set_int_member(audible, "end_ms", 4500); req.mode = RADIOS_AUDIO_RUN_MANUAL; plan = build(p, &req);
    g_assert_cmpuint(entry(plan, 0)->run.autodj_out_frame, ==, 0);
    g_assert_cmpuint(entry(plan, 0)->run.media.cue_out_frame, ==, 288031);
    radios_playout_plan_free(plan);
    /* A mismatched analysis cannot shorten a manually established cue span. */
    req.mode = RADIOS_AUDIO_RUN_EOF;
    json_object_set_string_member(json_object_get_object_member(detail, "analysis"), "sha256", "unverified");
    plan = build(p, &req); g_assert_cmpuint(entry(plan, 0)->run.media.cue_out_frame, ==, 288048);
    radios_playout_plan_free(plan); json_object_unref(p);
}


static void short_autodj_markers(JsonObject *row, guint in_ms, guint out_ms)
{

    JsonObject *detail = json_object_get_object_member(row, "detail"), *media = json_object_get_object_member(detail, "media");
    JsonObject *version = json_object_get_object_member(detail, "version"), *marks = json_object_new();
    json_object_set_string_member(marks, "station_id", station);
    json_object_set_string_member(marks, "version_id", json_object_get_string_member(version, "_id"));
    json_object_set_string_member(marks, "media_file_id", json_object_get_string_member(media, "media_file_id"));
    json_object_set_string_member(marks, "media_sha256", json_object_get_string_member(version, "sha256"));
    json_object_set_int_member(marks, "media_duration_ms", 6001);
    json_object_set_int_member(marks, "cue_in_ms", 0); json_object_set_int_member(marks, "out_ms", 6001);
    json_object_set_int_member(marks, "autodj_in_ms", in_ms); json_object_set_int_member(marks, "autodj_out_ms", out_ms);
    json_object_set_object_member(detail, "markers", marks);
}
static void test_short_autodj_span(void)
{
    JsonObject *p = prepared("programme"); JsonArray *tracks = json_object_get_array_member(p, "tracks");
    JsonObject *a = row(1, "A"), *b = row(2, "B"), *c = row(3, "C");
    json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b); json_array_add_object_element(tracks, c);
    short_autodj_markers(a, 1000, 2000); short_autodj_markers(b, 1000, 1250); short_autodj_markers(c, 1000, 5000);
    RadioSPlayoutRequest req = request(); req.mode = RADIOS_AUDIO_RUN_CROSSFADE; req.overlap_seconds = 2;
    JsonNode *original = song_copy(p); RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpuint(entry(plan, 0)->run.overlap_frames, ==, 12000);
    g_assert_cmpuint(entry(plan, 1)->run.fade_in_frames, ==, 12000);
    g_assert_cmpuint(entry(plan, 1)->run.media.cue_in_frame, ==, 48000);
    g_assert_cmpuint(entry(plan, 1)->run.autodj_out_frame, ==, 60000);
    g_assert_cmpuint(entry(plan, 1)->run.media.cue_out_frame, ==, 288031);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us - req.now_utc_us, ==, 1000000);
    g_assert_cmpint(entry(plan, 2)->planned_utc_us - req.now_utc_us, ==, 1250000);
    JsonNode *after = song_copy(p); g_assert_true(json_node_equal(original, after)); json_node_free(after);
    radios_playout_plan_free(plan);
    JsonObject *marks = json_object_get_object_member(json_object_get_object_member(a, "detail"), "markers");
    json_object_set_null_member(marks, "autodj_out_ms"); plan = build(p, &req);
    g_assert_cmpuint(entry(plan, 0)->run.overlap_frames, ==, 12000);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us - req.now_utc_us, ==, 4750645);
    g_assert_cmpint(entry(plan, 2)->planned_utc_us - entry(plan, 1)->planned_utc_us, ==, 250000);
    radios_playout_plan_free(plan); json_node_free(original); json_object_unref(p);
}


static void test_autodj_beat_short_span(void)
{
    JsonObject *p = prepared("programme"); JsonArray *tracks = json_object_get_array_member(p, "tracks");
    JsonObject *a = row(1, "A"), *b = row(2, "B"), *c = row(3, "C");
    json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b); json_array_add_object_element(tracks, c);
    short_autodj_markers(a, 1000, 2000); short_autodj_markers(b, 1000, 1250); short_autodj_markers(c, 1000, 5000);
    rhythm_row(a, 120); rhythm_row(b, 120); rhythm_row(c, 120);
    json_object_set_object_member(p, "playout_policy", central_policy(1, 2000, TRUE)); RadioSPlayoutRequest req = request();
    RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_true(entry(plan, 0)->run.beat_sync); g_assert_true(entry(plan, 1)->run.beat_sync);
    /* The matched entry is 1125 ms. Its 1250 ms exit has no intervening beat,
     * so keep that exit and fit the incoming fade in the remaining 125 ms. */
    g_assert_cmpuint(entry(plan, 1)->run.media.cue_in_frame, ==, 54000);
    g_assert_cmpuint(entry(plan, 1)->run.autodj_out_frame, ==, 60000);
    g_assert_cmpuint(entry(plan, 0)->run.overlap_frames, ==, 6000);
    g_assert_cmpuint(entry(plan, 1)->run.fade_in_frames, ==, 6000);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us - req.now_utc_us, ==, 625000);
    g_assert_cmpint(entry(plan, 2)->planned_utc_us - req.now_utc_us, ==, 750000);
    radios_playout_plan_free(plan);
    short_autodj_markers(a, 1030, 1100);
    plan = build(p, &req); g_assert_true(entry(plan, 0)->run.beat_sync);
    g_assert_cmpuint(entry(plan, 0)->run.media.cue_in_frame, ==, 49440);
    g_assert_cmpuint(entry(plan, 0)->run.autodj_out_frame, ==, 52800);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us - req.now_utc_us, ==, 70000);
    g_assert_cmpuint(entry(plan, 0)->run.media.cue_out_frame, ==, 288031);
    radios_playout_plan_free(plan); json_object_unref(p);
}

static void tempo_return_track(JsonObject *track, guint duration, guint out_ms, gdouble bpm)
{
    set_duration(track, duration); set_markers(track, duration, duration, 0); rhythm_row(track, bpm);
    JsonObject *detail = json_object_get_object_member(track, "detail");
    json_object_set_double_member(json_object_get_object_member(json_object_get_object_member(detail, "analysis"), "rhythm"), "first_beat_ms", 0);
    if (out_ms) json_object_set_int_member(json_object_get_object_member(detail, "markers"), "autodj_out_ms", out_ms);
}
static void test_tempo_return_exact_clock_and_next_native_target(void)
{
    const gdouble outgoing_bpms[] = {120, 144}, expected_ratios[] = {.9375, 1.125};
    for (guint scenario = 0; scenario < 2; ++scenario) {
        JsonObject *p = prepared("programme"); JsonArray *tracks = json_object_get_array_member(p, "tracks");
        JsonObject *a = row(1, "A"), *b = row(2, "B"), *c = row(3, "C");
        tempo_return_track(a, 6000, 4000, outgoing_bpms[scenario]); tempo_return_track(b, 10000, 0, 128); tempo_return_track(c, 10000, 0, 140);
        json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b); json_array_add_object_element(tracks, c);
        json_object_set_object_member(p, "playout_policy", central_policy(1, 1000, TRUE));
        JsonNode *before = song_copy(p); RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
        RadioSAudioRunItem *incoming = &entry(plan, 1)->run;
        g_assert_cmpfloat(incoming->tempo_ratio, ==, expected_ratios[scenario]);
        g_assert_cmpuint(incoming->tempo_return_start_frames, ==, 48000); g_assert_cmpuint(incoming->tempo_return_frames, ==, 192000);
        RadioSAudioTempoProfile profile = radios_audio_run_tempo_profile(incoming);
        g_assert_cmpfloat(radios_audio_tempo_profile_ratio(&profile, 48000), ==, expected_ratios[scenario]);
        g_assert_cmpfloat(radios_audio_tempo_profile_ratio(&profile, 240000), ==, 1);
        /* Independent integral: 1 s matched hold + 4 s average-ratio return,
         * followed by native tempo gives 10.1875 s or 9.625 s for this source. */
        gdouble duration = radios_audio_tempo_profile_output(&profile, 480000);
        g_assert_cmpfloat_with_epsilon(duration / 48000, scenario ? 9.625 : 10.1875, .000001);
        g_assert_cmpfloat_with_epsilon(entry(plan, 2)->run.tempo_ratio, 128.0 / 140, .000001);
        g_assert_false(entry(plan, 2)->run.beat_sync); g_assert_cmpuint(entry(plan, 2)->run.tempo_return_frames, ==, 192000);
        g_assert_cmpint(entry(plan, 2)->planned_utc_us - entry(plan, 1)->planned_utc_us, ==, scenario ? 8531250 : 9093750);
        g_assert_cmpuint(incoming->media.cue_in_frame, ==, 0); g_assert_cmpuint(incoming->media.cue_out_frame, ==, 480000);
        JsonNode *after = song_copy(p); g_assert_true(json_node_equal(before, after)); json_node_free(after); json_node_free(before);
        radios_playout_plan_free(plan); json_object_unref(p);
    }
}
static void test_tempo_return_calendar_end_uses_actual_profile(void)
{
    for (guint scenario = 0; scenario < 2; ++scenario) {
        JsonObject *p = prepared("programme"); JsonArray *tracks = json_object_get_array_member(p, "tracks");
        JsonObject *a = row(1, "A"), *b = row(2, "B");
        tempo_return_track(a, 6000, 4000, scenario ? 144 : 120); tempo_return_track(b, 10000, 0, 128);
        json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b);
        json_object_set_object_member(p, "playout_policy", central_policy(1, 1000, TRUE)); RadioSPlayoutRequest req = request();
        constraints_row(b, req.now_utc_us, req.now_utc_us + (scenario ? 13000000 : 14500000), TRUE);
        if (scenario) {
            /* The sped-up static duration would fit; returning to native tempo
             * extends this source past its authenticated 13-second deadline. */
            rejected(p, &req);
        } else {
            RadioSPlayoutPlan *plan = build(p, &req);
            g_assert_true(entry(plan, 0)->run.beat_sync); g_assert_cmpuint(entry(plan, 1)->run.tempo_return_frames, ==, 192000);
            g_assert_cmpint(entry(plan, 1)->run.latest_start_utc_us, ==, req.now_utc_us + 4312500);
            radios_playout_plan_free(plan);
        }
        json_object_unref(p);
    }
}
static void test_tempo_return_short_slot_retains_match_or_clips_smoothly(void)
{
    const guint exits[] = {1200, 3000};
    for (guint scenario = 0; scenario < 2; ++scenario) {
        JsonObject *p = prepared("programme"); JsonArray *tracks = json_object_get_array_member(p, "tracks");
        JsonObject *a = row(1, "A"), *b = row(2, "B"), *c = row(3, "C");
        tempo_return_track(a, 6000, 4000, 120); tempo_return_track(b, 4000, exits[scenario], 128); tempo_return_track(c, 10000, 0, 140);
        JsonObject *grid = json_object_get_object_member(json_object_get_object_member(json_object_get_object_member(b, "detail"), "analysis"), "rhythm");
        json_object_set_int_member(grid, "analyzed_ms", 4000);
        json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b); json_array_add_object_element(tracks, c);
        json_object_set_object_member(p, "playout_policy", central_policy(1, 1000, TRUE)); RadioSPlayoutRequest req = request();
        RadioSPlayoutPlan *plan = build(p, &req); RadioSAudioRunItem *incoming = &entry(plan, 1)->run;
        g_assert_cmpfloat(incoming->tempo_ratio, ==, .9375);
        if (!scenario) {
            g_assert_cmpuint(incoming->tempo_return_frames, ==, 0);
            g_assert_cmpfloat_with_epsilon(entry(plan, 2)->run.tempo_ratio, 120.0 / 140, .000001);
        } else {
            g_assert_cmpuint(incoming->tempo_return_frames, ==, 92903);
            RadioSAudioTempoProfile profile = radios_audio_run_tempo_profile(incoming);
            guint64 trigger = radios_audio_run_trigger_frame(incoming, &incoming->rhythm, incoming->tempo_ratio, incoming->media.cue_out_frame);
            gdouble at = radios_audio_tempo_profile_output(&profile, trigger - incoming->media.cue_in_frame);
            g_assert_cmpfloat(radios_audio_tempo_profile_ratio(&profile, at), ==, 1);
            g_assert_cmpfloat_with_epsilon(entry(plan, 2)->run.tempo_ratio, 128.0 / 140, .000001);
        }
        g_assert_cmpuint(incoming->media.cue_out_frame, ==, 192000);
        radios_playout_plan_free(plan); json_object_unref(p);
    }
}


static JsonObject *adopt_constraints(gint64 earliest, gint64 end, gint64 track_gap)
{
    JsonObject *constraints = json_object_new();
    json_object_set_int_member(constraints, "earliest_utc_us", earliest); json_object_set_int_member(constraints, "latest_end_utc_us", end);
    json_object_set_int_member(constraints, "track_gap_ms", track_gap); json_object_set_int_member(constraints, "artist_gap_ms", 0);
    json_object_set_int_member(constraints, "album_gap_ms", 0); json_object_set_int_member(constraints, "source_interval_ms", 0);
    json_object_set_boolean_member(constraints, "source_turn_start", FALSE); return constraints;
}
static void test_live_adopt_retime_short_tail_constraints(void)
{
    JsonObject *p = prepared("programme"), *a = row(1, "A"), *b = row(2, "B"), *config = json_object_new();
    JsonArray *tracks = json_object_get_array_member(p, "tracks"), *players = json_array_new();
    json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b);
    short_autodj_markers(a, 1000, 5000); short_autodj_markers(b, 2000, 5500);
    json_array_add_int_element(players, 1); json_array_add_int_element(players, 2);
    json_object_set_array_member(config, "autodj_players", players); json_object_set_boolean_member(config, "beat_sync", FALSE);
    json_object_set_object_member(p, "config", config); RadioSPlayoutRequest req = request();
    req.mode = RADIOS_AUDIO_RUN_CROSSFADE; req.overlap_seconds = 3;
    RadioSPlayoutPlan *plan = build(p, &req); g_assert_true(plan->player_autodj);
    JsonArray *display = json_object_get_array_member(json_node_get_object(plan->display), "entries");
    json_object_set_string_member(json_array_get_object_element(display, 1), "block_id", "accepted-event-metadata");
    g_assert_true(radios_playout_plan_adopt_source(plan, 192000, &req, NULL));
    g_assert_cmpuint(entry(plan, 0)->run.media.cue_in_frame, ==, 192000);
    g_assert_cmpuint(entry(plan, 0)->run.media.cue_out_frame, ==, 288031);
    g_assert_cmpuint(entry(plan, 0)->run.autodj_out_frame, ==, 240000);
    g_assert_cmpuint(entry(plan, 0)->run.overlap_frames, ==, 48031);
    g_assert_cmpuint(entry(plan, 1)->run.fade_in_frames, ==, 48031);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us - req.now_utc_us, ==, 1000000);
    g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(display, 1), "block_id"), ==, "accepted-event-metadata");
    RadioSAudioRunItem saved = entry(plan, 0)->run; JsonNode *saved_display = json_node_copy(plan->display); GError *error = NULL;
    g_assert_false(radios_playout_plan_adopt_source(plan, 288031, &req, &error)); g_clear_error(&error);
    g_assert_cmpmem(&saved, sizeof saved, &entry(plan, 0)->run, sizeof saved); g_assert_true(json_node_equal(saved_display, plan->display));
    json_node_free(saved_display); radios_playout_plan_free(plan);
    plan = build(p, &req); g_assert_true(radios_playout_plan_adopt_source(plan, 264000, &req, NULL));
    g_assert_cmpuint(entry(plan, 0)->run.autodj_out_frame, ==, 0);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us - req.now_utc_us, ==, 20); radios_playout_plan_free(plan);
    plan = build(p, &req); entry(plan, 0)->calendar_constraints = adopt_constraints(req.now_utc_us - G_USEC_PER_SEC, req.now_utc_us + 6000000, 0);
    saved = entry(plan, 0)->run; saved_display = json_node_copy(plan->display); gint64 first_time = entry(plan, 0)->planned_utc_us;
    RadioSPlayoutRequest late = req; late.now_utc_us += 5000000;
    g_assert_false(radios_playout_plan_adopt_source(plan, 192000, &late, &error)); g_clear_error(&error);
    g_assert_cmpmem(&saved, sizeof saved, &entry(plan, 0)->run, sizeof saved); g_assert_cmpint(entry(plan, 0)->planned_utc_us, ==, first_time);
    g_assert_true(json_node_equal(saved_display, plan->display)); json_node_free(saved_display); radios_playout_plan_free(plan);
    plan = build(p, &req); entry(plan, 1)->calendar_constraints = adopt_constraints(req.now_utc_us - 60000000, req.now_utc_us + 60000000, 60000);
    RadioSPlayoutHistory *history = radios_playout_history_new();
    radios_playout_history_observe(history, entry(plan, 1)->run.media.media_id, entry(plan, 1)->run.media.version_id, "B", "Albüm", req.now_utc_us - 57000000, 1);
    JsonNode *saved_history = radios_playout_history_json(history); saved_display = json_node_copy(plan->display); saved = entry(plan, 1)->run; req.history = history;
    /* The original +4 s successor respected the minute repeat gap. Joining
     * the playing source would move it to +1 s; reject atomically instead. */
    g_assert_false(radios_playout_plan_adopt_source(plan, 192000, &req, &error)); g_clear_error(&error);
    g_assert_cmpmem(&saved, sizeof saved, &entry(plan, 1)->run, sizeof saved); g_assert_true(json_node_equal(saved_display, plan->display));
    JsonNode *after_history = radios_playout_history_json(history); g_assert_true(json_node_equal(saved_history, after_history));
    json_node_free(after_history); json_node_free(saved_history); json_node_free(saved_display); radios_playout_history_free(history);
    radios_playout_plan_free(plan); json_object_unref(p);
}

static void single_player_native_entry(RadioSPlayoutEntry *value)
{
    g_assert_cmpfloat(value->run.tempo_ratio, ==, 1);
    g_assert_false(value->run.beat_sync);
    g_assert_cmpuint(value->run.overlap_frames, ==, 0);
    g_assert_cmpuint(value->run.fade_in_frames, ==, 0);
    g_assert_cmpuint(value->run.tempo_return_start_frames, ==, 0);
    g_assert_cmpuint(value->run.tempo_return_frames, ==, 0);
    g_assert_cmpint(value->run.effect, ==, RADIOS_AUDIO_RUN_EFFECT_NONE);
    g_assert_cmpint(value->run.mode, !=, RADIOS_AUDIO_RUN_CROSSFADE);
}
static void test_single_player_native_timing_and_exits(void)
{
    JsonObject *p = prepared("playlist"); JsonArray *tracks = json_object_get_array_member(p, "tracks");
    JsonObject *a = row(1, "A"), *b = row(2, "B");
    set_duration(a, 6000); set_markers(a, 6000, 4500, 1000); rhythm_row(a, 120);
    set_duration(b, 6000); set_markers(b, 6000, 4000, 1000); rhythm_row(b, 128);
    JsonObject *ma = json_object_get_object_member(json_object_get_object_member(a, "detail"), "markers");
    JsonObject *mb = json_object_get_object_member(json_object_get_object_member(b, "detail"), "markers");
    json_object_set_int_member(ma, "cue_in_ms", 500); json_object_set_int_member(ma, "autodj_in_ms", 1000);
    json_object_set_int_member(mb, "cue_in_ms", 1000); json_object_set_boolean_member(mb, "automatic", FALSE);
    json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b);
    json_object_set_object_member(p, "playout_policy", central_policy(1, 1000, TRUE));
    JsonNode *input = json_node_new(JSON_NODE_OBJECT); json_node_set_object(input, p); gchar *before = json_to_string(input, FALSE);
    RadioSPlayoutRequest req = request(); req.single_player = TRUE;
    RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpuint(plan->entries->len, ==, 2);
    for (guint i = 0; i < plan->entries->len; ++i) single_player_native_entry(entry(plan, i));
    g_assert_cmpuint(entry(plan, 0)->run.media.cue_in_frame, ==, 500 * 48);
    g_assert_cmpuint(entry(plan, 0)->run.media.cue_out_frame, ==, 6000 * 48);
    g_assert_cmpuint(entry(plan, 0)->run.auto_next_frame, ==, 4500 * 48);
    g_assert_cmpint(entry(plan, 0)->run.mode, ==, RADIOS_AUDIO_RUN_CUE);
    g_assert_cmpuint(entry(plan, 1)->run.media.cue_in_frame, ==, 1000 * 48);
    g_assert_cmpuint(entry(plan, 1)->run.media.cue_out_frame, ==, 6000 * 48);
    g_assert_cmpuint(entry(plan, 1)->run.auto_next_frame, ==, 6000 * 48);
    g_assert_cmpint(entry(plan, 1)->run.mode, ==, RADIOS_AUDIO_RUN_EOF);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us - entry(plan, 0)->planned_utc_us, ==, 4000000);
    JsonObject *display = json_array_get_object_element(json_object_get_array_member(json_node_get_object(plan->display), "entries"), 0);
    g_assert_false(json_object_get_boolean_member(display, "beat_sync"));
    g_assert_cmpint(json_object_get_int_member(display, "overlap_ms"), ==, 0);
    gchar *after = json_to_string(input, FALSE); g_assert_cmpstr(before, ==, after);
    g_free(before); g_free(after); json_node_free(input); radios_playout_plan_free(plan);
    json_object_set_int_member(ma, "autodj_out_ms", 5000);
    plan = build(p, &req);
    g_assert_cmpuint(entry(plan, 0)->run.autodj_out_frame, ==, 0);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us - entry(plan, 0)->planned_utc_us, ==, 4000000);
    radios_playout_plan_free(plan); json_object_unref(p);
}
static void test_single_player_calendar_effect_and_window(void)
{
    JsonObject *p = prepared("programme"); JsonArray *tracks = json_object_get_array_member(p, "tracks");
    const guint pool[] = {1, 2};
    JsonObject *a = routed_row(1, pool, 2, 1, TRUE), *b = routed_row(2, pool, 2, 0, FALSE);
    rhythm_row(a, 120); rhythm_row(b, 128);
    json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b);
    RadioSPlayoutRequest req = request();
    constraints_row(a, req.now_utc_us, req.now_utc_us + 12500000, TRUE);
    JsonObject *cb = constraints_row(b, req.now_utc_us, req.now_utc_us + 11500000, TRUE);
    RadioSPlayoutPlan *matched = build(p, &req);
    g_assert_true(entry(matched, 0)->run.beat_sync); g_assert_cmpfloat(entry(matched, 1)->run.tempo_ratio, ==, .9375);
    g_assert_cmpint(entry(matched, 0)->run.effect, ==, RADIOS_AUDIO_RUN_EFFECT_LOWPASS);
    radios_playout_plan_free(matched);
    req.single_player = TRUE; req.trim_calendar_end = TRUE;
    /* The same complete sequence is longer at native tempo without overlap.
     * It must fail, including when the legacy caller requested tail trimming. */
    rejected(p, &req);
    json_object_set_int_member(cb, "latest_end_utc_us", req.now_utc_us + 12500000);
    RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpuint(plan->entries->len, ==, 2);
    single_player_native_entry(entry(plan, 0)); single_player_native_entry(entry(plan, 1));
    g_assert_cmpuint(entry(plan, 1)->run.media.cue_in_frame, ==, 0);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us - entry(plan, 0)->planned_utc_us, ==, 6000645);
    g_assert_cmpint(entry(plan, 1)->run.latest_start_utc_us, ==, req.now_utc_us + 6499355);
    JsonObject *hint = json_object_get_object_member(a, "calendar_transition");
    radios_playout_plan_free(plan);
    /* Single-player mode suppresses valid cross-player policies, while their
     * original authenticated shape still has to pass strict validation. */
    json_object_set_string_member(hint, "effect", "unknown-effect"); rejected(p, &req); json_object_unref(p);
}
static void test_single_player_legacy_deck_boundary_policy(void)
{
    JsonObject *p = prepared("automation"), *config = configuration(p, 2);
    JsonObject *s = add_source(p, config, 1, 1, 2, "music");
    JsonObject *a = row(1, "A"), *b = row(2, "B");
    set_duration(a, 6000); set_markers(a, 6000, 4000, 1000);
    json_array_add_object_element(json_object_get_array_member(s, "tracks"), a);
    json_array_add_object_element(json_object_get_array_member(s, "tracks"), b);
    json_object_set_int_member(config, "transition", RADIOS_AUDIO_RUN_CROSSFADE);
    json_object_set_int_member(config, "boundaryTransition", RADIOS_AUDIO_RUN_CROSSFADE + 1);
    add_deck_policy(config, "1", RADIOS_AUDIO_RUN_CROSSFADE, 1);
    add_deck_policy(config, "2", RADIOS_AUDIO_RUN_CROSSFADE, 1);
    RadioSPlayoutRequest req = request(); req.single_player = TRUE;
    RadioSPlayoutPlan *plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 2);
    single_player_native_entry(entry(plan, 0)); single_player_native_entry(entry(plan, 1));
    g_assert_cmpuint(entry(plan, 0)->run.auto_next_frame, ==, 4000 * 48);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us - entry(plan, 0)->planned_utc_us, ==, 4000000);
    radios_playout_plan_free(plan); json_object_unref(p);
}

static JsonObject *copyright_config(JsonObject *prepared)
{
    JsonObject *policy = json_object_new(); json_object_set_int_member(policy, "schema", 1);
    json_object_set_boolean_member(policy, "restrict", TRUE); json_object_set_string_member(policy, "unregistered", "allow");
    json_object_set_array_member(policy, "rules", json_array_new());
    json_object_set_object_member(prepared, "copyright_policy", policy); return policy;
}
static JsonObject *copyright_rule(JsonObject *policy, guint id, const gchar *artist,
    const gchar *album, const gchar *title, const gchar *from, const gchar *until)
{
    JsonObject *rule = json_object_new(); gchar *identity = uuid(900000 + id);
    json_object_set_string_member(rule, "id", identity); g_free(identity);
    json_object_set_string_member(rule, "artist", artist); json_object_set_string_member(rule, "album", album);
    json_object_set_string_member(rule, "title", title); json_object_set_string_member(rule, "starts_at", from);
    json_object_set_string_member(rule, "ends_at", until); json_object_set_string_member(rule, "note", "");
    json_object_set_boolean_member(rule, "enabled", TRUE);
    json_array_add_object_element(json_object_get_array_member(policy, "rules"), rule); return rule;
}
static void test_copyright_intersection_and_mixed_playlist(void)
{
    JsonObject *p = prepared("playlist"), *policy = copyright_config(p);
    JsonArray *tracks = json_object_get_array_member(p, "tracks");
    JsonObject *first = row(1, " IŞIK  BAND "), *second = row(2, "Başka"), *third = row(3, "IŞIK BAND");
    json_array_add_object_element(tracks, first); json_array_add_object_element(tracks, second); json_array_add_object_element(tracks, third);
    json_object_set_string_member(json_object_get_object_member(third, "metadata"), "album", "Başka albüm");
    copyright_rule(policy, 1, "ışık band", "", "", "2026-10-01T00:00:00Z", "2026-10-10T00:00:00Z");
    copyright_rule(policy, 2, "ışık band", "Albüm", "", "2026-10-01T00:00:00Z", "2026-10-06T00:30:00Z");
    GError *error = NULL; g_assert_true(radios_copyright_policy_validate(policy, &error)); g_assert_no_error(error);
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpuint(plan->entries->len, ==, 2); gchar *second_id = uuid(2), *third_id = uuid(3);
    g_assert_cmpstr(entry(plan, 0)->run.entry_id, ==, second_id); g_assert_cmpstr(entry(plan, 1)->run.entry_id, ==, third_id);
    g_assert_cmpint(entry(plan, 1)->run.copyright.ends_at_utc_us, ==, stamp("2026-10-10T00:00:00Z"));
    g_assert_cmpuint(json_array_get_length(tracks), ==, 3); /* catalogue playlist untouched */
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(plan->display), "diagnostics")), ==, 1);
    radios_playout_plan_free(plan);
    /* Combined backend denial must never survive turning this optional setting off. */
    json_object_set_boolean_member(first, "media_eligible", TRUE); json_object_set_boolean_member(first, "eligible", FALSE);
    json_object_set_boolean_member(policy, "restrict", FALSE); plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 3);
    for (guint i = 0; i < 3; ++i) g_assert_false(entry(plan, i)->run.copyright.restricted);
    radios_playout_plan_free(plan); g_free(second_id); g_free(third_id); json_object_unref(p);
}
static void test_copyright_future_and_bound_catalogue(void)
{
    JsonObject *p = prepared("playlist"), *policy = copyright_config(p), *track = row(1, "Affirmed artist");
    json_array_add_object_element(json_object_get_array_member(p, "tracks"), track);
    JsonObject *media = json_object_get_object_member(json_object_get_object_member(track, "detail"), "media");
    json_object_set_string_member(media, "artist", "Catalogue artist");
    copyright_rule(policy, 1, "Catalogue artist", "", "", "2026-10-06T01:00:00Z", "2026-10-06T02:00:00Z");
    RadioSPlayoutRequest req = request(); req.start_utc_us = stamp("2026-10-06T01:00:00Z");
    RadioSPlayoutPlan *plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 1);
    g_assert_cmpint(entry(plan, 0)->run.copyright.starts_at_utc_us, ==, req.start_utc_us);
    /* An editor display alias must not bypass the stored artist restriction. */
    radios_playout_plan_copyright(plan, policy); g_assert_false(radios_copyright_window_allowed(&entry(plan, 0)->run.copyright, req.now_utc_us));
    g_assert_true(radios_copyright_window_allowed(&entry(plan, 0)->run.copyright, req.start_utc_us));
    g_assert_false(radios_copyright_window_allowed(&entry(plan, 0)->run.copyright, stamp("2026-10-06T02:00:00Z")));
    radios_playout_plan_free(plan); json_object_unref(p);
}
static void test_copyright_unregistered_fallback_contract(void)
{
    JsonObject *p = prepared("playlist"), *policy = copyright_config(p);
    JsonObject *matched = row(1, "Registered"), *unmatched = row(2, "Unregistered");
    JsonArray *tracks = json_object_get_array_member(p, "tracks");
    json_array_add_object_element(tracks, matched); json_array_add_object_element(tracks, unmatched);
    JsonObject *rule = copyright_rule(policy, 1, "Registered", "", "", "2026-10-01T00:00:00Z", "2026-10-10T00:00:00Z");
    RadioSPlayoutRequest req = request(); GError *error = NULL;
    RadioSAudioCopyrightWindow window = radios_copyright_policy_window(policy, "Unregistered", "", "");
    g_assert_true(radios_copyright_window_allowed(&window, req.now_utc_us));
    json_object_set_string_member(policy, "unregistered", "deny");
    g_assert_true(radios_copyright_policy_validate(policy, &error)); g_assert_no_error(error);
    window = radios_copyright_policy_window(policy, "Unregistered", "", "");
    g_assert_true(window.restricted); g_assert_true(window.denied);
    g_assert_false(radios_copyright_window_allowed(&window, req.now_utc_us));
    window = radios_copyright_policy_window(policy, "Registered", "", "");
    g_assert_true(radios_copyright_window_allowed(&window, req.now_utc_us));
    /* A combined backend denial must be re-evaluated against the same saved
     * fallback, rather than accidentally revived by base media eligibility. */
    json_object_set_boolean_member(unmatched, "media_eligible", TRUE);
    json_object_set_boolean_member(unmatched, "eligible", FALSE);
    RadioSPlayoutPlan *plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 1);
    gchar *identity = uuid(1); g_assert_cmpstr(entry(plan, 0)->run.entry_id, ==, identity); g_free(identity);
    radios_playout_plan_free(plan);
    json_object_set_boolean_member(rule, "enabled", FALSE);
    window = radios_copyright_policy_window(policy, "Registered", "", "");
    g_assert_false(radios_copyright_window_allowed(&window, req.now_utc_us));
    json_object_set_boolean_member(policy, "restrict", FALSE);
    window = radios_copyright_policy_window(policy, "Unregistered", "", "");
    g_assert_true(radios_copyright_window_allowed(&window, req.now_utc_us));
    plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 2); radios_playout_plan_free(plan);
    json_object_set_string_member(policy, "unregistered", "invalid");
    g_assert_false(radios_copyright_policy_validate(policy, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    json_object_remove_member(policy, "unregistered");
    g_assert_false(radios_copyright_policy_validate(policy, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    json_object_unref(p);
}
static void test_copyright_automation_individual_expiry(void)
{
    JsonObject *p = prepared("automation"), *config = configuration(p, 3), *source = add_source(p, config, 1, 1, 1, "music");
    JsonArray *tracks = json_object_get_array_member(source, "tracks");
    json_array_add_object_element(tracks, row(1, "Expired")); json_array_add_object_element(tracks, row(2, "Unmatched"));
    JsonObject *policy = copyright_config(p);
    copyright_rule(policy, 1, "Expired", "", "", "2026-10-01T00:00:00Z", "2026-10-06T00:29:59Z");
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req); gchar *allowed = uuid(2);
    g_assert_cmpuint(plan->entries->len, ==, 3);
    for (guint i = 0; i < plan->entries->len; ++i) g_assert_cmpstr(entry(plan, i)->run.entry_id, ==, allowed);
    g_assert_cmpuint(json_array_get_length(tracks), ==, 2);
    radios_playout_plan_free(plan); g_free(allowed); json_object_unref(p);
    /* Two-player AutoDJ keeps its one surviving legal track rather than
     * rejecting the entire list after a matching item expired. */
    p = prepared("playlist"); policy = copyright_config(p); config = json_object_new();
    JsonArray *players = json_array_new(); json_array_add_int_element(players, 1); json_array_add_int_element(players, 2);
    json_object_set_array_member(config, "autodj_players", players); json_object_set_boolean_member(config, "beat_sync", FALSE);
    json_object_set_object_member(p, "config", config); tracks = json_object_get_array_member(p, "tracks");
    json_array_add_object_element(tracks, row(1, "Expired")); json_array_add_object_element(tracks, row(2, "Unmatched"));
    copyright_rule(policy, 1, "Expired", "", "", "2026-10-01T00:00:00Z", "2026-10-06T00:29:59Z");
    plan = build(p, &req); g_assert_cmpuint(plan->entries->len, ==, 1); g_assert_true(plan->player_autodj);
    g_assert_cmpuint(plan->options.pool_count, ==, 2); radios_playout_plan_free(plan); json_object_unref(p);
}
/* A rotation hint may carry a designed preset shape: it rides only on a real
 * crossfade, reaches the runner item, names the preset in the display, is
 * dropped on the native single-player transport and is validated strictly. */
static void test_calendar_transition_shape(void)
{
    JsonObject *response = prepared("programme"), *config = json_object_new();
    JsonArray *players = json_array_new(); json_array_add_int_element(players, 1); json_array_add_int_element(players, 3);
    json_object_set_array_member(config, "autodj_players", players); json_object_set_boolean_member(config, "beat_sync", FALSE);
    json_object_set_string_member(config, "title", "Shaped rotation pair"); json_object_set_object_member(response, "config", config);
    JsonObject *preset = radios_transition_preset_template("Kes"); g_assert_nonnull(preset);
    for (guint index = 0; index < 2; ++index) {
        JsonObject *track = row(index + 1, "Named canonical source"), *hint = json_object_new();
        json_object_set_int_member(hint, "mode", 1); json_object_set_int_member(hint, "overlap_ms", 500);
        JsonNode *shape = json_node_new(JSON_NODE_OBJECT); json_node_set_object(shape, preset); json_object_set_member(hint, "shape", shape);
        json_object_set_object_member(track, "calendar_transition", hint);
        json_array_add_object_element(json_object_get_array_member(response, "tracks"), track);
    }
    RadioSPlayoutRequest planning = request(); RadioSPlayoutPlan *plan = build(response, &planning);
    g_assert_cmpuint(plan->entries->len, ==, 2);
    g_assert_true(entry(plan, 0)->run.shape.enabled); g_assert_cmpuint(entry(plan, 0)->run.mode, ==, RADIOS_AUDIO_RUN_CROSSFADE);
    g_assert_cmpuint(entry(plan, 0)->run.overlap_frames, ==, 24000);
    g_assert_cmpint(entry(plan, 0)->run.shape.effect, ==, -1); g_assert_cmpuint(entry(plan, 0)->run.shape.volume[0].count, ==, 3);
    g_assert_true(radios_audio_transition_shape_validate(&entry(plan, 0)->run.shape, NULL));
    RadioSAudioRunItem *items = radios_playout_plan_items(plan); g_assert_true(items[0].shape.enabled); g_free(items);
    JsonObject *display = song_at(json_node_get_object(plan->display), "entries", 0);
    g_assert_cmpstr(json_object_get_string_member(display, "preset"), ==, "Kes");
    radios_playout_plan_free(plan);
    planning.single_player = TRUE; plan = build(response, &planning);
    g_assert_false(entry(plan, 0)->run.shape.enabled); g_assert_cmpuint(entry(plan, 0)->run.overlap_frames, ==, 0);
    radios_playout_plan_free(plan); planning.single_player = FALSE;
    JsonObject *hint = json_object_get_object_member(song_at(response, "tracks", 0), "calendar_transition");
    json_object_set_int_member(hint, "mode", 0); json_object_set_int_member(hint, "overlap_ms", 0); rejected(response, &planning);
    json_object_set_int_member(hint, "mode", 1); json_object_set_int_member(hint, "overlap_ms", 500);
    json_object_set_string_member(json_object_get_object_member(preset, "effect"), "type", "chorus"); rejected(response, &planning);
    json_object_unref(preset); json_object_unref(response);
}


/* A live step (kılavuz §04) between two rows: with a duration rule the next
 * row's calendar bound is a chosen wait, so the entry is anchored there (the
 * runner holds in SCHEDULED until then); without the live_step hint a not-yet
 * -open bound stays invalid. A manual rule marks the previous row MANUAL
 * (runner MANUAL_WAIT), which never carries an overlap. */
static void test_live_step_duration_wait_and_manual_hold(void)
{
    JsonObject *p = prepared("programme"); JsonArray *tracks = json_object_get_array_member(p, "tracks");
    JsonObject *a = row(1, "A"), *b = row(2, "B"); json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b);
    RadioSPlayoutRequest req = request();
    constraints_row(a, req.now_utc_us, req.now_utc_us + 60 * G_USEC_PER_SEC, TRUE);
    constraints_row(b, req.now_utc_us + 10 * G_USEC_PER_SEC, req.now_utc_us + 60 * G_USEC_PER_SEC, FALSE);
    rejected(p, &req);
    JsonObject *step = json_object_new(); JsonArray *channels = json_array_new(); json_array_add_int_element(channels, 3); gchar *block = uuid(77);
    json_object_set_string_member(step, "block_id", block); g_free(block); json_object_set_string_member(step, "label", "talk");
    json_object_set_array_member(step, "channels", channels); json_object_set_string_member(step, "automation_id", "");
    json_object_set_string_member(step, "end_rule", "duration"); json_object_set_int_member(step, "duration_ms", 4000);
    json_object_set_int_member(step, "outer_order", 2); json_object_set_int_member(step, "inner_order", 0);
    json_object_set_object_member(a, "live_step", step);
    /* Duration rule (PDF s.15): A ends plainly into the step (MANUAL wait),
     * B is planned 4 s after A's end; the wait counts from A's actual end,
     * so B carries no absolute start anchor at the step's planned end. */
    constraints_row(b, req.now_utc_us, req.now_utc_us + 60 * G_USEC_PER_SEC, FALSE);
    RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpint(entry(plan, 0)->run.mode, ==, RADIOS_AUDIO_RUN_MANUAL);
    g_assert_cmpint(entry(plan, 1)->hold_us, ==, 4 * G_USEC_PER_SEC);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us, ==, req.now_utc_us + 6000645 + 4 * G_USEC_PER_SEC);
    g_assert_cmpint(entry(plan, 1)->run.start_utc_us, ==, req.now_utc_us);
    g_assert_cmpuint(entry(plan, 0)->run.overlap_frames, ==, 0); radios_playout_plan_free(plan);
    /* A later window start still bounds B; the wait reaches it here. */
    constraints_row(b, req.now_utc_us + 10 * G_USEC_PER_SEC, req.now_utc_us + 60 * G_USEC_PER_SEC, FALSE);
    plan = build(p, &req);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us, ==, req.now_utc_us + 6000645 + 4 * G_USEC_PER_SEC);
    radios_playout_plan_free(plan);
    JsonObject *hint = json_object_new(); json_object_set_int_member(hint, "mode", 3); json_object_set_int_member(hint, "overlap_ms", 0);
    json_object_set_object_member(a, "calendar_transition", hint); json_object_set_string_member(step, "end_rule", "manual"); json_object_set_int_member(step, "duration_ms", 0);
    constraints_row(b, req.now_utc_us, req.now_utc_us + 60 * G_USEC_PER_SEC, FALSE);
    plan = build(p, &req); g_assert_cmpint(entry(plan, 0)->run.mode, ==, RADIOS_AUDIO_RUN_MANUAL);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us, ==, req.now_utc_us + 6000645); radios_playout_plan_free(plan); /* 288031 frames */
    json_object_set_int_member(hint, "overlap_ms", 100); rejected(p, &req); json_object_set_int_member(hint, "overlap_ms", 0);
    json_object_set_boolean_member(hint, "beat_sync", TRUE); rejected(p, &req);
    json_object_unref(p);
}
/* calendar_window_wait: the selection waited for B's playlist window. B
 * starts there instead of rejecting the plan; without the mark the early row
 * still fails, and a malformed mark or one without bounds is refused. */
static void test_calendar_window_wait_anchor(void)
{
    JsonObject *p = prepared("programme"); JsonArray *tracks = json_object_get_array_member(p, "tracks");
    JsonObject *a = row(1, "A"), *b = row(2, "B"); json_array_add_object_element(tracks, a); json_array_add_object_element(tracks, b);
    RadioSPlayoutRequest req = request();
    constraints_row(a, req.now_utc_us, req.now_utc_us + 60 * G_USEC_PER_SEC, TRUE);
    constraints_row(b, req.now_utc_us + 10 * G_USEC_PER_SEC, req.now_utc_us + 60 * G_USEC_PER_SEC, TRUE);
    rejected(p, &req);
    json_object_set_boolean_member(b, "calendar_window_wait", TRUE);
    RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpint(entry(plan, 1)->planned_utc_us, ==, req.now_utc_us + 10 * G_USEC_PER_SEC);
    g_assert_cmpint(entry(plan, 1)->run.start_utc_us, ==, req.now_utc_us + 10 * G_USEC_PER_SEC);
    g_assert_cmpint(entry(plan, 1)->run.latest_start_utc_us, ==, req.now_utc_us + 60 * G_USEC_PER_SEC - 6000645); /* 288031 frames */
    g_assert_cmpuint(entry(plan, 1)->run.fade_in_frames, ==, 0); radios_playout_plan_free(plan);
    json_object_set_int_member(b, "calendar_window_wait", 1); rejected(p, &req);
    json_object_set_boolean_member(b, "calendar_window_wait", FALSE); rejected(p, &req);
    json_object_set_boolean_member(b, "calendar_window_wait", TRUE); json_object_remove_member(b, "calendar_constraints"); rejected(p, &req);
    json_object_unref(p);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/playout/live-step-duration-wait-manual-hold", test_live_step_duration_wait_and_manual_hold);
    g_test_add_func("/playout/calendar-window-wait-anchors-window-start", test_calendar_window_wait_anchor);
    g_test_add_func("/playout/calendar-transition-designed-shape", test_calendar_transition_shape);
    g_test_add_func("/playout/copyright-selector-intersection-mixed-playlist-toggle", test_copyright_intersection_and_mixed_playlist);
    g_test_add_func("/playout/copyright-future-bound-catalogue", test_copyright_future_and_bound_catalogue);
    g_test_add_func("/playout/copyright-unregistered-fallback-contract", test_copyright_unregistered_fallback_contract);
    g_test_add_func("/playout/copyright-automation-individual-expiry", test_copyright_automation_individual_expiry); database = g_build_filename(RADIOS_TEST_PROJECT_ROOT, "build", "plan-fixture.sdb", NULL);
    g_test_add_func("/playout/single-player-native-timing-cue-exits", test_single_player_native_timing_and_exits);
    g_test_add_func("/playout/single-player-native-timing-effect-calendar-window", test_single_player_calendar_effect_and_window);
    g_test_add_func("/playout/single-player-legacy-deck-boundary-policy", test_single_player_legacy_deck_boundary_policy);
    g_test_add_func("/playout/tempo-return-exact-clock-next-native-target", test_tempo_return_exact_clock_and_next_native_target);
    g_test_add_func("/playout/tempo-return-calendar-end-profile", test_tempo_return_calendar_end_uses_actual_profile);
    g_test_add_func("/playout/tempo-return-short-slot-retain-or-clip", test_tempo_return_short_slot_retains_match_or_clips_smoothly);
    g_test_add_func("/playout/central-policy-explicit-hint-AutoDJ-IN-OUT", test_central_policy_and_autodj_marks);
    g_test_add_func("/playout/marked-trailing-silence-AutoDJ-exit", test_marked_trailing_silence_autodj_exit);
    g_test_add_func("/playout/live-source-adopt-short-tail-retime-constraints-atomic", test_live_adopt_retime_short_tail_constraints);
    g_test_add_func("/playout/AutoDJ-short-IN-OUT-caps-incoming-fade", test_short_autodj_span);
    g_test_add_func("/playout/AutoDJ-beatmatch-short-IN-OUT-keeps-positive-span", test_autodj_beat_short_span);
    g_test_add_func("/playout/verified-identity-exact-duration", test_identity_frames);
    g_test_add_func("/playout/fractional-whole-file-and-explicit-cue", test_fractional_whole_file);
    g_test_add_func("/playout/cue-markers-UTC-calendar", test_markers_calendar);
    g_test_add_func("/playout/cycle-complete-order-decks", test_cycle_complete);
    g_test_add_func("/playout/weighted-determinism-preview-isolation", test_weighted_deterministic);
    g_test_add_func("/playout/repeat-turkish-durable-history", test_repeat_history);
    g_test_add_func("/playout/jingle-dedicated-deck-fallback", test_jingle_fallback);
    g_test_add_func("/playout/midnight-weekday-calendar-single-deck", test_midnight_weekdays_single);
    g_test_add_func("/playout/cycle-durable-checkpoint-clock-and-revision", test_cycle_durable_checkpoint);
    g_test_add_func("/playout/weighted-checkpoint-RNG-continuation", test_weighted_checkpoint_rng);
    g_test_add_func("/playout/complete-consumed-eligible-unique", test_complete_consumed_unique);
    g_test_add_func("/playout/repeat-overlap-actual-start", test_repeat_overlap_actual_start);
    g_test_add_func("/playout/boundary-cue-real-timeline-lookahead", test_boundary_cue_real_timeline);
    g_test_add_func("/playout/sequential-start-policy-preserved", test_sequential_policy_preserved);
    g_test_add_func("/playout/jingle-metadata-checkpoint-phase", test_jingle_metadata_and_checkpoint_phase);
    g_test_add_func("/playout/calendar-gap-after-existing-entry", test_calendar_gap_after_existing_entry);
    g_test_add_func("/playout/repeat-history-subsecond-roundtrip", test_repeat_history_subsecond_roundtrip);
    g_test_add_func("/playout/default-jingle-adjacent-physical-decks", test_default_jingle_adjacent_decks);
    g_test_add_func("/playout/autodj-deck-transitions-timeline-continuation", test_autodj_deck_transitions);
    g_test_add_func("/playout/autodj-deck-policy-validation", test_autodj_deck_policy_validation);
    g_test_add_func("/playout/autodj-deck-jingle-dry-and-scope", test_autodj_deck_jingle_dry);
    g_test_add_func("/playout/player-pair-tempo-grid-immutable-validation", test_player_pair_tempo_and_grid);
    g_test_add_func("/playout/player-pair-named-transition-exact-timing", test_player_pair_named_transition_timing);
    g_test_add_func("/playout/scope-single-routed-batch-preserves-validation", test_scope_single_routed_batch);
    g_test_add_func("/playout/routed-mixed-pool-immutable-deterministic", test_routed_mixed_pool);
    g_test_add_func("/playout/routed-lookahead-strict-validation", test_routed_lookahead_validation);
    g_test_add_func("/playout/routed-local-beatmatch-filter-boundary", test_routed_beatmatch_filter);
    g_test_add_func("/playout/filter-cue-zero-actual-overlap", test_filter_cue_zero_actual_overlap);
    g_test_add_func("/playout/filter-scheduled-start-removes-overlap", test_filter_scheduled_start_removes_overlap);
    g_test_add_func("/playout/filter-probability-cue-zero-actual-overlap", test_filter_probability_cue_zero_actual_overlap);
    g_test_add_func("/playout/routed-effective-time-repeat-window-constraints", test_routed_actual_time_constraints);
    g_test_add_func("/playout/loaded-rotation-trims-calendar-tail", test_loaded_rotation_trims_calendar_tail);
    g_test_add_func("/playout/calendar-repeat-gap-trims-compressed-timeline", test_calendar_repeat_gap_trims_compressed_timeline);
    g_test_add_func("/playout/song-linear-disabled-order-per-plan-cap", test_song_linear_disabled_order_count);
    g_test_add_func("/playout/song-weighted-deterministic-fresh-native-metadata", test_song_weighted_fresh_native_metadata);
    g_test_add_func("/playout/song-UTC-intersection-actual-media-history", test_song_UTC_intersection_actual_media_history);
    g_test_add_func("/playout/song-posttempo-minute-window-before-lease", test_song_posttempo_minute_window_rejection);
    g_test_add_func("/playout/song-posttempo-artist-album-repeat-before-lease", test_song_posttempo_artist_album_repeat_rejection);
    gint result = g_test_run(); g_free(database); return result;
}
