#include "radios/playout_ui.h"
#include "radios/programme_model.h"
#include "radios/audio_runner.h"
#include <string.h>

/* Kılavuz §06/§08 çekme adedi: pure window selection of playout_ui.c. */
static gchar *uuid(guint id) { return g_strdup_printf("00000000-0000-4000-8000-%012u", id); }
static JsonObject *event_new(guint id, const gchar *kind, const gchar *block_id)
{
    JsonObject *event = json_object_new(); gchar *text = uuid(id);
    json_object_set_string_member(event, "id", text); json_object_set_string_member(event, "kind", kind);
    json_object_set_string_member(event, "title", text);
    if (block_id) json_object_set_string_member(event, "block_id", block_id);
    g_free(text); return event;
}
/* blocks × per_block contents; content ids are 1-based in playback order. */
static JsonObject *programme(guint blocks, guint per_block)
{
    JsonObject *config = json_object_new(); json_object_set_string_member(config, "title", "Test"); json_object_set_string_member(config, "date", "2026-10-18");
    JsonArray *events = json_array_new(), *list = json_array_new();
    for (guint b = 0; b < blocks; ++b) {
        JsonObject *block = json_object_new(); gchar *bid = uuid(100000 + b);
        json_object_set_string_member(block, "id", bid); json_object_set_string_member(block, "kind", "music");
        json_object_set_string_member(block, "title", "Event"); json_object_set_string_member(block, "start_time", ""); json_object_set_string_member(block, "note", "");
        json_array_add_object_element(list, block);
        for (guint l = 0; l < per_block; ++l) json_array_add_object_element(events, event_new(b * per_block + l + 1, "music", bid));
        g_free(bid);
    }
    json_object_set_array_member(config, "blocks", list); json_object_set_array_member(config, "events", events); return config;
}
static void assert_ids(JsonArray *ids, guint first, guint count)
{
    g_assert_nonnull(ids); g_assert_cmpuint(json_array_get_length(ids), ==, count);
    for (guint i = 0; i < count; ++i) {
        gchar *expected = uuid(first + i);
        g_assert_cmpstr(json_array_get_string_element(ids, i), ==, expected); g_free(expected);
    }
}
static void window(JsonObject *config, guint start, guint events, guint contents, guint first, guint count, guint next)
{
    JsonArray *ids = NULL; guint next_index = G_MAXUINT;
    g_assert_true(radios_playout_programme_window_entries(config, start, events, contents, &ids, &next_index));
    assert_ids(ids, first, count); g_assert_cmpuint(next_index, ==, next); json_array_unref(ids);
}
static void test_three_events_five_contents(void)
{
    JsonObject *config = programme(3, 5);
    /* The first window stays inside event 1: an event longer than
     * content_fetch ends the window there, so playback order is kept. */
    window(config, 0, 2, 3, 1, 3, 3);
    /* The next window resumes at content 4: the rest of event 1 plus the
     * first three of event 2 (two events, at most three contents each). */
    window(config, 3, 2, 3, 4, 5, 8);
    window(config, 8, 2, 3, 9, 5, 13);
    window(config, 13, 2, 3, 14, 2, 15);
    /* Whole events fit when content_fetch is larger than the events. */
    window(config, 0, 2, 5, 1, 10, 10);
    window(config, 0, 2, 0, 1, 10, 10);
    window(config, 0, 3, 0, 1, 15, 15);
    window(config, 0, 9, 9, 1, 15, 15);
    /* Events unbounded, contents bounded. */
    window(config, 0, 0, 2, 1, 2, 2);
    json_object_unref(config);
}
static void test_start_index_in_the_middle(void)
{
    JsonObject *config = programme(3, 5);
    /* Start at event 2 content 3 (index 7): three contents of event 2, then three of event 3. */
    window(config, 7, 2, 3, 8, 6, 13);
    window(config, 4, 1, 5, 5, 1, 5);
    window(config, 14, 2, 3, 15, 1, 15);
    JsonArray *ids = (JsonArray *)config; guint next = 0;
    g_assert_false(radios_playout_programme_window_entries(config, 15, 2, 3, &ids, &next)); g_assert_null(ids); g_assert_cmpuint(next, ==, 15);
    g_assert_false(radios_playout_programme_window_entries(config, 99, 2, 3, &ids, &next)); g_assert_null(ids); g_assert_cmpuint(next, ==, 15);
    json_object_unref(config);
}
static void test_zero_zero_is_whole_programme(void)
{
    JsonObject *config = programme(3, 5); JsonArray *ids = (JsonArray *)config; guint next = 7;
    g_assert_false(radios_playout_programme_window_entries(config, 2, 0, 0, &ids, &next));
    g_assert_null(ids); g_assert_cmpuint(next, ==, 2);
    g_assert_false(radios_playout_programme_window_entries(config, 0, 2, 3, NULL, &next)); g_assert_cmpuint(next, ==, 0);
    g_assert_false(radios_playout_programme_window_entries(NULL, 0, 2, 3, &ids, &next)); g_assert_null(ids); g_assert_cmpuint(next, ==, 0);
    JsonObject *empty = json_object_new();
    g_assert_false(radios_playout_programme_window_entries(empty, 0, 2, 3, &ids, &next)); g_assert_null(ids);
    json_object_unref(empty); json_object_unref(config);
}
static void test_backend_limit_of_500(void)
{
    JsonObject *config = programme(1, 2000);
    gint64 started = g_get_monotonic_time();
    window(config, 0, 1, 0, 1, RADIOS_AUDIO_RUN_MAX_ITEMS, 500);
    g_test_message("2000-content window selection: %.2f ms", (g_get_monotonic_time() - started) / 1000.0);
    window(config, 500, 1, 0, 501, RADIOS_AUDIO_RUN_MAX_ITEMS, 1000);
    window(config, 0, G_MAXUINT, 0, 1, RADIOS_AUDIO_RUN_MAX_ITEMS, 500);
    window(config, 1800, G_MAXUINT, 0, 1801, 200, 2000);
    window(config, 0, 1, 600, 1, RADIOS_AUDIO_RUN_MAX_ITEMS, 500);
    json_object_unref(config);
    config = programme(40, 50);
    window(config, 0, 40, 0, 1, RADIOS_AUDIO_RUN_MAX_ITEMS, 500);
    window(config, 0, 12, 0, 1, RADIOS_AUDIO_RUN_MAX_ITEMS, 500);
    window(config, 0, 5, 50, 1, 250, 250);
    json_object_unref(config);
}
static void test_legacy_flat_events_group_by_kind(void)
{
    /* No blocks: consecutive music is one implicit event until another kind. */
    JsonObject *config = json_object_new(); JsonArray *events = json_array_new();
    const gchar *kinds[] = {"music", "music", "music", "music", "advertisement", "music", "music"};
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) json_array_add_object_element(events, event_new(i + 1, kinds[i], NULL));
    json_object_set_array_member(config, "events", events);
    window(config, 0, 2, 10, 1, 5, 5);
    window(config, 0, 1, 10, 1, 4, 4);
    window(config, 5, 1, 10, 6, 2, 7);
    window(config, 2, 2, 1, 3, 1, 3);
    /* A content without an id is skipped, never requested; the window goes
     * on inside its event. */
    json_object_remove_member(json_array_get_object_element(events, 1), "id");
    JsonArray *ids = NULL; guint next = 0;
    g_assert_true(radios_playout_programme_window_entries(config, 0, 1, 10, &ids, &next));
    g_assert_cmpuint(json_array_get_length(ids), ==, 3); g_assert_cmpuint(next, ==, 4);
    gchar *first = uuid(1), *third = uuid(3), *fourth = uuid(4);
    g_assert_cmpstr(json_array_get_string_element(ids, 0), ==, first);
    g_assert_cmpstr(json_array_get_string_element(ids, 1), ==, third);
    g_assert_cmpstr(json_array_get_string_element(ids, 2), ==, fourth);
    g_free(first); g_free(third); g_free(fourth); json_array_unref(ids);
    json_object_unref(config);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/programme-window/three-events-five-contents", test_three_events_five_contents);
    g_test_add_func("/programme-window/start-index-middle", test_start_index_in_the_middle);
    g_test_add_func("/programme-window/zero-zero-whole-programme", test_zero_zero_is_whole_programme);
    g_test_add_func("/programme-window/backend-limit-500", test_backend_limit_of_500);
    g_test_add_func("/programme-window/legacy-flat-events", test_legacy_flat_events_group_by_kind);
    return g_test_run();
}
