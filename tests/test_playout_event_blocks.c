/* Reuse authenticated media fixtures without running their suite. */
#define main radios_event_block_fixture_main
#include "test_playout.c"
#undef main

static JsonObject *display_row(RadioSPlayoutPlan *plan, guint index)
{ return json_array_get_object_element(json_object_get_array_member(json_node_get_object(plan->display), "entries"), index); }

static void automation_blocks_follow_source_kind_occurrences(void)
{
    JsonObject *p = prepared("automation"), *config = configuration(p, 7);
    JsonObject *music = add_source(p, config, 1, 1, 3, "music");
    JsonObject *voice = add_source(p, config, 2, 1, 1, "jingle");
    for (guint i = 1; i <= 3; ++i) json_array_add_object_element(json_object_get_array_member(music, "tracks"), row(i, "Sanatçı"));
    JsonObject *jingle = row(4, "İstasyon");
    json_object_set_string_member(json_object_get_object_member(jingle, "metadata"), "kind", "jingle");
    json_object_set_string_member(json_object_get_object_member(jingle, "metadata"), "title", "RadioS kimliği");
    json_array_add_object_element(json_object_get_array_member(voice, "tracks"), jingle);
    json_object_set_boolean_member(config, "jingleEnabled", TRUE);
    json_object_set_int_member(config, "jingleEvery", 3); json_object_set_string_member(config, "jingleDeck", "3");
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req), *again = build(p, &req);
    g_assert_cmpuint(plan->entries->len, ==, 9);
    const guint block_first[] = {0, 0, 0, 3, 4, 4, 4, 7, 8};
    for (guint i = 0; i < plan->entries->len; ++i) {
        JsonObject *shown = display_row(plan, i); const gchar *id = json_object_get_string_member(shown, "block_id");
        g_assert_true(g_uuid_string_is_valid(id));
        g_assert_cmpstr(id, ==, json_object_get_string_member(display_row(plan, block_first[i]), "block_id"));
        g_assert_cmpstr(id, ==, json_object_get_string_member(display_row(again, i), "block_id"));
        if (i && block_first[i] == i) g_assert_cmpstr(id, !=, json_object_get_string_member(display_row(plan, i - 1), "block_id"));
        g_assert_cmpstr(json_object_get_string_member(shown, "block_kind"), ==, i == 3 || i == 7 ? "jingle" : "music");
        g_assert_cmpstr(json_object_get_string_member(shown, "playlist_id"), ==,
            json_object_get_string_member(i == 3 || i == 7 ? voice : music, "playlist_id"));
    }
    /* One row per actual playable occurrence; hints do not add audio items. */
    JsonArray *hints = json_object_get_array_member(display_row(plan, 2), "planned_jingles");
    g_assert_cmpuint(json_array_get_length(hints), ==, 1);
    JsonObject *hint = json_array_get_object_element(hints, 0);
    g_assert_cmpstr(json_object_get_string_member(hint, "title"), ==, "RadioS kimliği");
    g_assert_cmpstr(json_object_get_string_member(hint, "placement"), ==, "after");
    g_assert_cmpint(json_object_get_int_member(hint, "player"), ==, 3);
    g_assert_cmpint(json_object_get_int_member(hint, "start_ms"), ==, 6000);
    g_assert_false(json_object_has_member(display_row(plan, 0), "planned_jingles"));
    g_assert_false(json_object_has_member(display_row(plan, 3), "planned_jingles"));
    radios_playout_plan_free(again); radios_playout_plan_free(plan); json_object_unref(p);
}
static void explicit_blocks_keep_separate_accepted_identity(void)
{
    JsonObject *p = prepared("programme"), *config = json_object_new();
    JsonArray *events = json_array_new(), *blocks = json_array_new();
    json_object_set_object_member(p, "config", config); json_object_set_array_member(config, "events", events);
    json_object_set_array_member(config, "blocks", blocks); json_object_set_string_member(config, "title", "Gerçek program");
    gchar *first_id = uuid(6001), *second_id = uuid(6002);
    for (guint i = 0; i < 2; ++i) {
        JsonObject *block = json_object_new(); json_object_set_string_member(block, "id", i ? second_id : first_id);
        json_object_set_string_member(block, "kind", "music"); json_object_set_string_member(block, "title", i ? "İkinci blok" : "İlk blok");
        gchar *playlist_id = uuid(7001); json_object_set_string_member(block, "playlist_id", playlist_id); g_free(playlist_id);
        json_array_add_object_element(blocks, block);
    }
    for (guint i = 1; i <= 3; ++i) {
        JsonObject *song = row(i, "Aynı sanatçı"), *event = json_object_new();
        json_object_set_string_member(event, "id", json_object_get_string_member(song, "entry_id"));
        json_object_set_string_member(event, "block_id", i < 3 ? first_id : second_id);
        json_object_set_string_member(event, "kind", "music"); json_array_add_object_element(events, event);
        json_array_add_object_element(json_object_get_array_member(p, "tracks"), song);
    }
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpstr(json_object_get_string_member(display_row(plan, 0), "block_id"), ==, first_id);
    g_assert_cmpstr(json_object_get_string_member(display_row(plan, 1), "block_id"), ==, first_id);
    g_assert_cmpstr(json_object_get_string_member(display_row(plan, 2), "block_id"), ==, second_id);
    gchar *playlist_id = uuid(7001);
    for (guint i = 0; i < 3; ++i) {
        g_assert_cmpstr(json_object_get_string_member(display_row(plan, i), "playlist_id"), ==, playlist_id);
        g_assert_cmpstr(json_object_get_string_member(entry(plan, i)->binding, "playlist_id"), ==, playlist_id);
    }
    g_free(playlist_id);
    json_object_set_string_member(json_array_get_object_element(blocks, 0), "title", "Sonraki taslak adı");
    g_assert_cmpstr(json_object_get_string_member(display_row(plan, 0), "block_title"), ==, "İlk blok");
    g_assert_cmpuint(plan->entries->len, ==, 3);
    radios_playout_plan_free(plan); json_object_unref(p); g_free(first_id); g_free(second_id);
}
static void appended_first_jingle_refreshes_previous_music_hint(void)
{
    JsonObject *p = prepared("automation"), *config = configuration(p, 1);
    JsonObject *music = add_source(p, config, 1, 1, 1, "music");
    JsonObject *voice = add_source(p, config, 2, 1, 1, "jingle");
    json_array_add_object_element(json_object_get_array_member(music, "tracks"), row(1, "Müzik"));
    JsonObject *jingle = row(2, "İstasyon");
    json_object_set_string_member(json_object_get_object_member(jingle, "metadata"), "kind", "jingle");
    json_object_set_string_member(json_object_get_object_member(jingle, "metadata"), "title", "Sonraki partinin jingle'ı");
    json_array_add_object_element(json_object_get_array_member(voice, "tracks"), jingle);
    json_object_set_boolean_member(config, "jingleEnabled", TRUE); json_object_set_string_member(config, "jingleDeck", "3");
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpuint(plan->entries->len, ==, 1);
    g_assert_false(json_object_has_member(display_row(plan, 0), "planned_jingles"));
    RadioSPlayoutHistory *history = checkpoint(entry(plan, 0)); req.history = history;
    req.start_utc_us = entry(plan, 0)->planned_utc_us + (gint64)(entry(plan, 0)->run.media.cue_out_frame * G_USEC_PER_SEC / 48000);
    RadioSPlayoutPlan *refill = build(p, &req); g_assert_true(entry(refill, 0)->jingle);
    JsonArray *accepted = json_object_get_array_member(json_node_get_object(plan->display), "entries");
    JsonArray *rows = json_object_get_array_member(json_node_get_object(refill->display), "entries");
    for (guint i = 0; i < json_array_get_length(rows); ++i) json_array_add_element(accepted, song_copy(json_array_get_object_element(rows, i)));
    while (refill->entries->len) g_ptr_array_add(plan->entries, g_ptr_array_steal_index(refill->entries, 0));
    radios_playout_plan_free(refill);
    radios_playout_plan_refresh_jingle_metadata(plan);
    JsonArray *hints = json_object_get_array_member(display_row(plan, 0), "planned_jingles");
    g_assert_cmpuint(json_array_get_length(hints), ==, 1);
    JsonObject *hint = json_array_get_object_element(hints, 0);
    g_assert_cmpstr(json_object_get_string_member(hint, "placement"), ==, "after");
    g_assert_cmpstr(json_object_get_string_member(hint, "title"), ==, "Sonraki partinin jingle'ı");
    g_assert_cmpuint(plan->entries->len, ==, 3);
    radios_playout_plan_free(plan); radios_playout_history_free(history); json_object_unref(p);
}
static void automation_music_stays_one_event_across_source_playlists(void)
{
    JsonObject *p = prepared("automation"), *config = configuration(p, 6);
    JsonObject *first = add_source(p, config, 1, 1, 1, "music"), *second = add_source(p, config, 2, 1, 1, "music");
    json_array_add_object_element(json_object_get_array_member(first, "tracks"), row(1, "Birinci kaynak"));
    json_array_add_object_element(json_object_get_array_member(second, "tracks"), row(2, "İkinci kaynak"));
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    g_assert_cmpuint(plan->entries->len, ==, 6);
    const gchar *id = json_object_get_string_member(display_row(plan, 0), "block_id");
    gboolean source_changed = FALSE;
    for (guint i = 0; i < plan->entries->len; ++i) {
        JsonObject *shown = display_row(plan, i);
        g_assert_cmpstr(json_object_get_string_member(shown, "block_id"), ==, id);
        g_assert_cmpstr(json_object_get_string_member(shown, "block_title"), ==, "Müzik eventi");
        g_assert_cmpstr(json_object_get_string_member(shown, "source_id"), ==, entry(plan, i)->source_id);
        g_assert_cmpstr(json_object_get_string_member(shown, "entry_id"), ==, entry(plan, i)->run.entry_id);
        if (i && g_strcmp0(entry(plan, i - 1)->source_id, entry(plan, i)->source_id)) source_changed = TRUE;
        JsonObject *source = g_str_equal(entry(plan, i)->source_id, json_object_get_string_member(first, "source_id")) ? first : second;
        g_assert_cmpstr(json_object_get_string_member(shown, "playlist_id"), ==, json_object_get_string_member(source, "playlist_id"));
    }
    g_assert_true(source_changed);
    radios_playout_plan_free(plan); json_object_unref(p);
}
static void implicit_programme_music_uses_contiguous_event_identity(void)
{
    JsonObject *p = prepared("programme"), *config = json_object_new(); JsonArray *events = json_array_new();
    json_object_set_object_member(p, "config", config); json_object_set_array_member(config, "events", events);
    json_object_set_string_member(config, "title", "Eski düz sıra");
    const gchar *kinds[] = {"music", "music", "advertisement", "music", "music", "sweeper", "music"};
    const guint first[] = {0, 0, 2, 3, 3, 5, 6};
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) {
        JsonObject *song = row(i + 1, "Korunan sanatçı"), *event = json_object_new();
        json_object_set_string_member(json_object_get_object_member(song, "metadata"), "kind", kinds[i]);
        json_object_set_string_member(event, "id", json_object_get_string_member(song, "entry_id"));
        json_object_set_string_member(event, "kind", kinds[i]); json_array_add_object_element(events, event);
        json_array_add_object_element(json_object_get_array_member(p, "tracks"), song);
    }
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req), *again = build(p, &req);
    g_assert_cmpuint(plan->entries->len, ==, G_N_ELEMENTS(kinds));
    for (guint i = 0; i < plan->entries->len; ++i) {
        JsonObject *shown = display_row(plan, i); const gchar *id = json_object_get_string_member(shown, "block_id");
        g_assert_cmpstr(id, ==, json_object_get_string_member(display_row(plan, first[i]), "block_id"));
        g_assert_cmpstr(id, ==, json_object_get_string_member(display_row(again, i), "block_id"));
        if (i && first[i] == i) g_assert_cmpstr(id, !=, json_object_get_string_member(display_row(plan, i - 1), "block_id"));
        g_assert_false(json_object_get_boolean_member(shown, "block_explicit"));
        g_assert_cmpstr(json_object_get_string_member(shown, "entry_id"), ==, entry(plan, i)->run.entry_id);
    }
    g_assert_false(json_object_has_member(json_array_get_object_element(events, 0), "block_id"));
    radios_playout_plan_free(again); radios_playout_plan_free(plan); json_object_unref(p);
}
static void automatic_music_refill_continues_first_accepted_event(void)
{
    JsonObject *p = prepared("automation"), *config = configuration(p, 2);
    JsonObject *music = add_source(p, config, 1, 1, 1, "music");
    json_array_add_object_element(json_object_get_array_member(music, "tracks"), row(1, "Bir"));
    json_array_add_object_element(json_object_get_array_member(music, "tracks"), row(2, "İki"));
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req);
    gchar *accepted_id = g_strdup(json_object_get_string_member(display_row(plan, 0), "block_id"));
    RadioSPlayoutHistory *history = checkpoint(entry(plan, 1)); req.history = history;
    req.start_utc_us = entry(plan, 1)->planned_utc_us + (gint64)(entry(plan, 1)->run.media.cue_out_frame / 48) * 1000;
    RadioSPlayoutPlan *refill = build(p, &req);
    g_assert_cmpstr(json_object_get_string_member(display_row(refill, 0), "block_id"), !=, accepted_id);
    JsonArray *accepted = json_object_get_array_member(json_node_get_object(plan->display), "entries");
    JsonArray *rows = json_object_get_array_member(json_node_get_object(refill->display), "entries");
    for (guint i = 0; i < json_array_get_length(rows); ++i) json_array_add_element(accepted, song_copy(json_array_get_object_element(rows, i)));
    while (refill->entries->len) g_ptr_array_add(plan->entries, g_ptr_array_steal_index(refill->entries, 0));
    radios_playout_plan_free(refill); radios_playout_plan_refresh_jingle_metadata(plan);
    g_assert_cmpuint(plan->entries->len, ==, 4);
    for (guint i = 0; i < plan->entries->len; ++i)
        g_assert_cmpstr(json_object_get_string_member(display_row(plan, i), "block_id"), ==, accepted_id);
    /* A later source rebase keeps the accepted event identity as well. */
    req.now_utc_us += G_USEC_PER_SEC; GError *error = NULL;
    g_assert_true(radios_playout_plan_adopt_source(plan, 48000, &req, &error)); g_assert_no_error(error);
    for (guint i = 0; i < plan->entries->len; ++i)
        g_assert_cmpstr(json_object_get_string_member(display_row(plan, i), "block_id"), ==, accepted_id);
    g_free(accepted_id); radios_playout_plan_free(plan); radios_playout_history_free(history); json_object_unref(p);
}

static void plain_playlist_music_runs_end_at_short_content(void)
{
    JsonObject *p = prepared("playlist"); json_object_set_string_member(p, "source_name", "Korunan kaynak playlist");
    const gchar *kinds[] = {"music", "music", "advertisement", "music", "music", "sweeper", "music"};
    const guint first[] = {0, 0, 2, 3, 3, 5, 6};
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) {
        JsonObject *song = row(i + 1, "Kaynak sanatçı");
        json_object_set_string_member(json_object_get_object_member(song, "metadata"), "kind", kinds[i]);
        json_array_add_object_element(json_object_get_array_member(p, "tracks"), song);
    }
    RadioSPlayoutRequest req = request(); RadioSPlayoutPlan *plan = build(p, &req), *again = build(p, &req);
    g_assert_cmpuint(plan->entries->len, ==, G_N_ELEMENTS(kinds));
    for (guint i = 0; i < plan->entries->len; ++i) {
        JsonObject *shown = display_row(plan, i); const gchar *id = json_object_get_string_member(shown, "block_id");
        g_assert_cmpstr(id, ==, json_object_get_string_member(display_row(plan, first[i]), "block_id"));
        g_assert_cmpstr(id, ==, json_object_get_string_member(display_row(again, i), "block_id"));
        if (i && first[i] == i) g_assert_cmpstr(id, !=, json_object_get_string_member(display_row(plan, i - 1), "block_id"));
        g_assert_false(json_object_get_boolean_member(shown, "block_explicit"));
        g_assert_cmpstr(entry(plan, i)->source_name, ==, "Korunan kaynak playlist");
        g_assert_cmpstr(json_object_get_string_member(shown, "entry_id"), ==,
            json_object_get_string_member(json_array_get_object_element(json_object_get_array_member(p, "tracks"), i), "entry_id"));
    }
    radios_playout_plan_free(again); radios_playout_plan_free(plan); json_object_unref(p);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    database = g_build_filename(RADIOS_TEST_PROJECT_ROOT, "build", "event-blocks-plan-fixture.sdb", NULL);
    g_test_add_func("/playout-event-blocks/automation-source-kind-occurrences-jingle-hint", automation_blocks_follow_source_kind_occurrences);
    g_test_add_func("/playout-event-blocks/explicit-accepted-blocks-preserved", explicit_blocks_keep_separate_accepted_identity);
    g_test_add_func("/playout-event-blocks/music-across-source-playlists", automation_music_stays_one_event_across_source_playlists);
    g_test_add_func("/playout-event-blocks/plain-playlist-contiguous-music", plain_playlist_music_runs_end_at_short_content);
    g_test_add_func("/playout-event-blocks/implicit-programme-contiguous-music", implicit_programme_music_uses_contiguous_event_identity);
    g_test_add_func("/playout-event-blocks/music-refill-keeps-event", automatic_music_refill_continues_first_accepted_event);
    g_test_add_func("/playout-event-blocks/appended-first-jingle-refreshes-previous-music", appended_first_jingle_refreshes_previous_music_hint);
    gint result = g_test_run(); g_free(database); return result;
}
