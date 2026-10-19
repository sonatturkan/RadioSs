/* Kılavuz 18.10.2026 s.4/s.6/s.15 (akım B, H4): the end rule of a live step
 * moves the flow on. The general queue plays A → live talk step → C from the
 * main calendar. With a duration rule the step lasts its duration from A's
 * actual end: pausing A or a late end never shortens it, and the controller
 * continues with C exactly like the operator's Next (playout_ui.c
 * live_wait_poll); an earlier Next ends the step early. A manual step keeps
 * waiting for the operator. Real PCM, private DB and owned null outputs only
 * (run_playout_ui.py). */
#define _POSIX_C_SOURCE 200809L
#include <glib.h>
static int live_step_wait_run(void);
#define main radios_live_step_wait_fixture_main
#define g_test_run live_step_wait_run
#include "test_playout_ui.c"
#undef g_test_run
#undef main

#define WAIT_PROGRAMME "Süre adımı programı"
#define WAIT_A "Süre adımı A"
#define WAIT_C "Süre adımı C"
#define WAIT_A_HZ 700.0
#define WAIT_C_HZ 1700.0

static JsonObject *wait_source(void)
{
    JsonObject *source = json_object_new(); gchar *id = g_uuid_string_random();
    json_object_set_string_member(source, "id", id); g_free(id);
    json_object_set_int_member(source, "frequency", 1); json_object_set_int_member(source, "count", 1); json_object_set_int_member(source, "weight", 100);
    json_object_set_int_member(source, "start_minute", 0); json_object_set_int_member(source, "end_minute", 1440);
    json_object_set_int_member(source, "interval_minutes", 0);
    const gchar *flags[] = {"random", "track_random", "advert_random", "sweeper_random"};
    for (guint i = 0; i < G_N_ELEMENTS(flags); ++i) json_object_set_boolean_member(source, flags[i], FALSE);
    return source;
}
static JsonObject *wait_playlist_source(JsonNode *playlist)
{
    JsonObject *source = wait_source();
    json_object_set_string_member(source, "playlist_id", radios_json_string(json_node_get_object(playlist), "_id", ""));
    json_object_set_string_member(source, "media_kind", "music"); return source;
}
/* docs/kilavuz-2026-10-18-4c/sozlesme.md §2: no playlist_id, a live object. */
static JsonObject *wait_step_source(const gchar *end_rule, gint64 duration_ms)
{
    JsonObject *source = wait_source(), *live = json_object_new(); JsonArray *channels = json_array_new();
    json_object_set_string_member(source, "source_type", "live");
    json_object_set_string_member(live, "title", "Süre adımı konuşması"); json_object_set_string_member(live, "label", "talk");
    json_array_add_int_element(channels, 3); json_object_set_array_member(live, "channels", channels);
    json_object_set_string_member(live, "automation_id", ""); json_object_set_string_member(live, "end_rule", end_rule);
    json_object_set_int_member(live, "duration_ms", duration_ms); json_object_set_object_member(source, "live", live); return source;
}
/* Main calendar: one programme covering today (UTC), active now, whose
 * linear schema plays A (1) → live step (2) → C (3). */
static JsonNode *wait_calendar(JsonNode *a, JsonNode *c, const gchar *end_rule, gint64 duration_ms, gint64 day_us)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *root = json_object_new(); json_node_take_object(node, root);
    json_object_set_int_member(root, "schema", 1); json_object_set_string_member(root, "timezone", "UTC");
    JsonObject *definition = json_object_new(), *settings = json_object_new(); gchar *definition_id = g_uuid_string_random();
    json_object_set_string_member(definition, "id", definition_id); json_object_set_string_member(definition, "name", "Süre adımı şeması");
    json_object_set_string_member(settings, "selection_mode", "linear");
    const gchar *gaps[] = {"track_gap_minutes", "artist_gap_minutes", "album_gap_minutes"};
    for (guint i = 0; i < G_N_ELEMENTS(gaps); ++i) json_object_set_int_member(settings, gaps[i], 0);
    JsonArray *transitions = json_array_new();
    for (guint i = 0; i < 3; ++i) {
        JsonObject *transition = json_object_new(); json_object_set_int_member(transition, "mode", i);
        json_object_set_int_member(transition, "weight", i ? 0 : 100); json_object_set_int_member(transition, "overlap_ms", 0);
        json_array_add_object_element(transitions, transition);
    }
    json_object_set_array_member(settings, "transitions", transitions); json_object_set_boolean_member(settings, "automation_schema", TRUE);
    json_object_set_object_member(definition, "settings", settings);
    JsonArray *sources = json_array_new();
    json_array_add_object_element(sources, wait_playlist_source(a)); json_array_add_object_element(sources, wait_step_source(end_rule, duration_ms));
    json_array_add_object_element(sources, wait_playlist_source(c)); json_object_set_array_member(definition, "sources", sources);
    JsonArray *library = json_array_new(); json_array_add_object_element(library, definition); json_object_set_array_member(root, "rotation_library", library);
    GDateTime *day = g_date_time_new_from_unix_utc(day_us / G_USEC_PER_SEC); gchar *date = g_date_time_format(day, "%Y-%m-%d");
    JsonObject *event = json_object_new(), *assignment = json_object_new(); gchar *event_id = g_uuid_string_random(), *assignment_id = g_uuid_string_random();
    json_object_set_string_member(event, "id", event_id); json_object_set_string_member(event, "title", WAIT_PROGRAMME);
    json_object_set_string_member(event, "date", date); json_object_set_string_member(event, "color", "#abd6e4");
    json_object_set_int_member(event, "start_minute", 0); json_object_set_int_member(event, "end_minute", 1440);
    json_object_set_string_member(assignment, "id", assignment_id); json_object_set_string_member(assignment, "rotation_id", definition_id);
    json_object_set_int_member(assignment, "weight", 100);
    JsonArray *assignments = json_array_new(); json_array_add_object_element(assignments, assignment); json_object_set_array_member(event, "rotations", assignments);
    JsonArray *events = json_array_new(); json_array_add_object_element(events, event); json_object_set_array_member(root, "events", events);
    g_free(definition_id); g_free(event_id); g_free(assignment_id); g_free(date); g_date_time_unref(day); return node;
}

typedef struct { Window window; RadioSPlayerWeekly *driver; JsonNode *a, *c, *pa, *pc, *saved; } WaitFlow;
static void wait_open(WaitFlow *f, const gchar *end_rule, gint64 duration_ms, guint a_ms)
{
    memset(f, 0, sizeof *f); f->window = window_open(); Window *w = &f->window;
    assert_gates_off(w); navigate(w, "broadcast");
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(w->root, "programme-general-continuous"))));
    f->driver = general_driver(w);
    f->a = routing_short_markers(media_import(w, WAIT_A, WAIT_A_HZ, .22, "music"), a_ms);
    f->c = routing_short_markers(media_import(w, WAIT_C, WAIT_C_HZ, .22, "music"), 3000);
    f->pa = playlist_create("Süre adımı A kaynağı", f->a, NULL); f->pc = playlist_create("Süre adımı C kaynağı", f->c, NULL);
    gint64 day = g_get_real_time() / G_TIME_SPAN_DAY * G_TIME_SPAN_DAY;
    JsonNode *calendar = wait_calendar(f->pa, f->pc, end_rule, duration_ms, day);
    f->saved = document_save("automation_calendar", "main", json_node_get_object(calendar)); json_node_free(calendar);
    radios_player_weekly_general_refresh(f->driver);
    GeneralReady ready = {w, f->driver, WAIT_PROGRAMME, day};
    wait_until(general_current_mixed_ready, &ready, "the active live step programme is projected before header Play");
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    click(w->root, "header-play"); general_pcm_wait(w, WAIT_A, 200, 20);
}
static void wait_close(WaitFlow *f)
{
    Window *w = &f->window;
    click(w->root, "header-stop"); run_for(400); g_assert_false(weekly_snapshot(f->driver).armed);
    calendar_schedule_unchanged(f->saved); assert_saved_playlist_unchanged(f->pa); assert_saved_playlist_unchanged(f->pc);
    json_node_free(f->a); json_node_free(f->c); json_node_free(f->pa); json_node_free(f->pc); json_node_free(f->saved);
    window_close(w);
}
static RadioSPlayoutSnapshot wait_flow(WaitFlow *f)
{ RadioSPlayoutSnapshot snapshot = {0}; g_assert_true(radios_playout_ui_get_snapshot(f->window.ctx, &snapshot)); return snapshot; }
/* Waits while A is on air; returns the wall time A's step wait began. */
static gint64 wait_a_end(WaitFlow *f, guint seconds)
{
    gint64 until = g_get_monotonic_time() + (gint64)seconds * G_USEC_PER_SEC; RadioSPlayoutSnapshot flow = wait_flow(f);
    while (g_str_equal(flow.title, WAIT_A) && flow.state != RADIOS_PLAYOUT_MANUAL_WAIT && g_get_monotonic_time() < until) { pump(); flow = wait_flow(f); }
    if (flow.state != RADIOS_PLAYOUT_MANUAL_WAIT || !g_str_equal(flow.title, WAIT_A))
        g_error("A's end should enter the live step's wait; state %d title '%s' error '%s'", flow.state, flow.title, flow.error);
    return g_get_real_time();
}

/* Duration rule: A paused 3 s still leaves the full 2.5 s step after its
 * actual end (an absolute bound used to shorten it to ~0); C follows by itself. */
static void test_duration_counts_from_actual_end(void)
{
    const gint64 duration_ms = 2500;
    WaitFlow f; wait_open(&f, "duration", duration_ms, 6000); Window *w = &f.window;
    run_for(800); click(w->root, "header-pause"); run_for(150);
    g_assert_cmpint(wait_flow(&f).state, ==, RADIOS_PLAYOUT_PAUSED);
    run_for(3000); click(w->root, "header-play"); general_pcm_wait(w, WAIT_A, 1000, 5);
    goffset span = capture_offset("MAIN");
    gint64 a_end = wait_a_end(&f, 15);
    RadioSPlayoutSnapshot waiting = wait_flow(&f);
    g_test_message("A ended; step wait until %+.3f s from the observed end", (waiting.live_wait_until_utc_us - a_end) / 1e6);
    g_assert_cmpint(waiting.live_wait_until_utc_us, >=, a_end + duration_ms * 1000 - 300000);
    g_assert_cmpint(waiting.live_wait_until_utc_us, <=, a_end + duration_ms * 1000 + 100000);
    /* Nothing sounds during the step; C follows without any operator action. */
    RadioSPlayoutSnapshot following = general_pcm_wait(w, WAIT_C, 0, 8); gint64 c_start = g_get_real_time();
    g_assert_cmpint(following.live_wait_until_utc_us, ==, 0);
    goffset pcm = capture_offset("MAIN"); run_for(400);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, WAIT_C_HZ), >, .07); g_assert_cmpfloat(tone_energy("MAIN", pcm, WAIT_A_HZ), <, .01);
    guint64 gap = 0; g_assert_cmpuint(capture_signal_span("MAIN", span, &gap), >, 0);
    gdouble silence = gap / 48000.0, observed = (c_start - a_end) / 1e6;
    g_test_message("MAIN silence between A and C %.3f s, C observed %.3f s after A's end (rule %.3f s)", silence, observed, duration_ms / 1e3);
    /* The PCM gap is the measure; the GUI observation adds poll latency under load. */
    g_assert_cmpfloat(fabs(silence - duration_ms / 1e3), <, .5);
    g_assert_cmpfloat(observed, >, duration_ms / 1e3 - .5); g_assert_cmpfloat(observed, <, duration_ms / 1e3 + 1.5);
    wait_close(&f);
}
/* Next during the step ends it early. */
static void test_next_ends_duration_step_early(void)
{
    WaitFlow f; wait_open(&f, "duration", 20000, 3000); Window *w = &f.window;
    gint64 a_end = wait_a_end(&f, 10); run_for(600);
    RadioSPlayoutSnapshot waiting = wait_flow(&f);
    g_assert_cmpint(waiting.state, ==, RADIOS_PLAYOUT_MANUAL_WAIT); g_assert_cmpint(waiting.live_wait_until_utc_us, >, a_end + 19 * G_USEC_PER_SEC);
    GError *error = NULL; require_call(radios_playout_ui_action(w->ctx, RADIOS_PLAYOUT_NEXT, &error), &error);
    RadioSPlayoutSnapshot following = general_pcm_wait(w, WAIT_C, 0, 3);
    g_test_message("Next ended the 20 s step %.3f s after A", (g_get_real_time() - a_end) / 1e6);
    g_assert_cmpint(following.live_wait_until_utc_us, ==, 0); g_assert_cmpint(g_get_real_time() - a_end, <, 5 * G_USEC_PER_SEC);
    wait_close(&f);
}
/* A manual step never continues alone; the operator's Next moves on. */
static void test_manual_step_waits_for_next(void)
{
    WaitFlow f; wait_open(&f, "manual", 0, 3000); Window *w = &f.window;
    wait_a_end(&f, 10); run_for(3500);
    RadioSPlayoutSnapshot held = wait_flow(&f);
    g_assert_cmpint(held.state, ==, RADIOS_PLAYOUT_MANUAL_WAIT); g_assert_cmpstr(held.title, ==, WAIT_A);
    g_assert_cmpint(held.live_wait_until_utc_us, ==, 0); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    GError *error = NULL; require_call(radios_playout_ui_action(w->ctx, RADIOS_PLAYOUT_NEXT, &error), &error);
    general_pcm_wait(w, WAIT_C, 0, 3);
    wait_close(&f);
}

/* Next during the row before the step ends that row into the step's wait;
 * it never skips the step (PDF s.15). Manual: a second Next continues;
 * duration: the step counts from that end. */
static void test_next_during_row_enters_step(void)
{
    WaitFlow f; wait_open(&f, "manual", 0, 6000); Window *w = &f.window;
    run_for(800); GError *error = NULL;
    require_call(radios_playout_ui_action(w->ctx, RADIOS_PLAYOUT_NEXT, &error), &error);
    gint64 until = g_get_monotonic_time() + 3 * G_USEC_PER_SEC; RadioSPlayoutSnapshot flow = wait_flow(&f);
    while (flow.state != RADIOS_PLAYOUT_MANUAL_WAIT && g_get_monotonic_time() < until) { pump(); flow = wait_flow(&f); }
    g_assert_cmpint(flow.state, ==, RADIOS_PLAYOUT_MANUAL_WAIT); g_assert_cmpstr(flow.title, ==, WAIT_A);
    run_for(1200); flow = wait_flow(&f);
    g_assert_cmpint(flow.state, ==, RADIOS_PLAYOUT_MANUAL_WAIT); g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    require_call(radios_playout_ui_action(w->ctx, RADIOS_PLAYOUT_NEXT, &error), &error);
    general_pcm_wait(w, WAIT_C, 0, 3);
    wait_close(&f);
}
static void test_next_during_row_starts_duration(void)
{
    WaitFlow f; wait_open(&f, "duration", 2000, 6000); Window *w = &f.window;
    run_for(800); GError *error = NULL; gint64 pressed = g_get_real_time();
    require_call(radios_playout_ui_action(w->ctx, RADIOS_PLAYOUT_NEXT, &error), &error);
    gint64 until = g_get_monotonic_time() + 3 * G_USEC_PER_SEC; RadioSPlayoutSnapshot flow = wait_flow(&f);
    while (flow.state != RADIOS_PLAYOUT_MANUAL_WAIT && g_get_monotonic_time() < until) { pump(); flow = wait_flow(&f); }
    g_assert_cmpint(flow.state, ==, RADIOS_PLAYOUT_MANUAL_WAIT); g_assert_cmpstr(flow.title, ==, WAIT_A);
    g_assert_cmpint(flow.live_wait_until_utc_us, >=, pressed + 1500000); g_assert_cmpint(flow.live_wait_until_utc_us, <=, pressed + 3000000);
    general_pcm_wait(w, WAIT_C, 0, 6); g_test_message("C started %.3f s after Next (step 2 s)", (g_get_real_time() - pressed) / 1e6);
    g_assert_cmpint(g_get_real_time() - pressed, >=, 1800000);
    wait_close(&f);
}
static int live_step_wait_run(void)
{
    g_test_add_func("/live-step/duration-counts-from-actual-end-PCM", test_duration_counts_from_actual_end);
    g_test_add_func("/live-step/next-ends-duration-step-early-PCM", test_next_ends_duration_step_early);
    g_test_add_func("/live-step/manual-step-waits-for-next-PCM", test_manual_step_waits_for_next);
    g_test_add_func("/live-step/next-during-row-enters-step-PCM", test_next_during_row_enters_step);
    g_test_add_func("/live-step/next-during-row-starts-duration-PCM", test_next_during_row_starts_duration);
    return g_test_run();
}
int main(int argc, char **argv) { return radios_live_step_wait_fixture_main(argc, argv); }
