#include "radios/programme_model.h"
#include <string.h>

static JsonObject *parse(const gchar *text)
{
    JsonParser *parser = json_parser_new(); GError *error = NULL;
    g_assert_true(json_parser_load_from_data(parser, text, -1, &error)); g_assert_no_error(error);
    JsonObject *object = json_object_ref(json_node_get_object(json_parser_get_root(parser)));
    g_object_unref(parser); return object;
}
static const gchar *member(JsonObject *object, const gchar *key) { return json_object_get_string_member(object, key); }
static JsonObject *element(JsonObject *config, const gchar *array, guint index)
{ return json_array_get_object_element(json_object_get_array_member(config, array), index); }

#define A "11111111-1111-4111-8111-111111111111"
#define B "22222222-2222-4222-8222-222222222222"
#define C "33333333-3333-4333-8333-333333333333"
#define X "44444444-4444-4444-8444-444444444444"
#define Y "55555555-5555-4555-8555-555555555555"

static void test_legacy_events_get_stable_blocks(void)
{
    JsonObject *config = parse("{\"title\":\"P\",\"date\":\"2026-10-07\",\"note\":\"\",\"timing\":{\"event_id\":\"" B "\"},"
        "\"events\":[{\"id\":\"" A "\",\"kind\":\"music\",\"duration_ms\":1000},{\"id\":\"" B "\",\"kind\":\"advertisement\",\"duration_ms\":500}]}");
    RadioSProgrammeTree *tree = radios_programme_tree_new(config);
    g_assert_cmpuint(tree->blocks->len, ==, 2);
    RadioSProgrammeBlock *first = g_ptr_array_index(tree->blocks, 0), *second = g_ptr_array_index(tree->blocks, 1);
    gchar *derived = radios_programme_block_id_for(A);
    g_assert_cmpstr(member(first->block, "id"), ==, derived); g_free(derived);
    g_assert_cmpstr(member(first->block, "title"), ==, "Müzik eventi");
    g_assert_cmpstr(member(second->block, "title"), ==, "Reklam eventi");
    g_assert_cmpstr(radios_programme_kind_label("advertisement"), ==, "Reklam");
    JsonNode *node = radios_programme_tree_config(tree, config); JsonObject *next = json_node_get_object(node);
    g_assert_cmpstr(member(element(next, "events", 1), "block_id"), ==, member(second->block, "id"));
    g_assert_true(json_object_has_member(next, "timing"));
    g_assert_false(json_object_has_member(element(config, "events", 0), "block_id"));
    RadioSProgrammeTree *again = radios_programme_tree_new(next);
    g_assert_cmpstr(member(((RadioSProgrammeBlock *)g_ptr_array_index(again->blocks, 0))->block, "id"), ==, member(first->block, "id"));
    radios_programme_tree_free(again); json_node_free(node); radios_programme_tree_free(tree); json_object_unref(config);
}

static void test_blocks_edit_and_index(void)
{
    JsonObject *config = parse("{\"title\":\"P\",\"date\":\"2026-10-07\",\"note\":\"\",\"timing\":{\"event_id\":\"" B "\"},"
        "\"blocks\":[{\"id\":\"" X "\",\"kind\":\"music\",\"title\":\"Sabah\",\"start_time\":\"\",\"note\":\"\"},"
        "{\"id\":\"" Y "\",\"kind\":\"announcement\",\"title\":\"Boş\",\"start_time\":\"\",\"note\":\"\"}],"
        "\"events\":[{\"id\":\"" A "\",\"block_id\":\"" X "\",\"duration_ms\":1000},{\"id\":\"" B "\",\"block_id\":\"" X "\",\"duration_ms\":2000}]}");
    RadioSProgrammeTree *tree = radios_programme_tree_new(config);
    guint block = 9; gint leaf = 9;
    g_assert_cmpuint(tree->blocks->len, ==, 2);
    g_assert_true(radios_programme_tree_find(tree, Y, &block, &leaf)); g_assert_cmpuint(block, ==, 1); g_assert_cmpint(leaf, ==, -1);
    g_assert_cmpint(radios_programme_tree_leaf_index(tree, Y), ==, -1);
    g_assert_cmpint(radios_programme_tree_leaf_index(tree, X), ==, 0);
    g_assert_cmpint(radios_programme_tree_leaf_index(tree, B), ==, 1);
    g_assert_cmpint(radios_programme_block_duration(g_ptr_array_index(tree->blocks, 0)), ==, 3000);
    g_assert_cmpstr(member(radios_programme_tree_leaf_at(tree, 1, &block), "id"), ==, B);
    g_assert_true(radios_programme_tree_move(tree, B, 1, 0));
    g_assert_cmpint(radios_programme_tree_leaf_index(tree, Y), ==, 1);
    g_assert_false(radios_programme_tree_move(tree, A, 0, 1));
    g_assert_true(radios_programme_tree_move(tree, Y, 0, 0));
    g_assert_cmpstr(member(((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks, 0))->block, "id"), ==, Y);
    JsonObject *added = json_object_new(); json_object_set_string_member(added, "id", C);
    RadioSProgrammeBlock *created = radios_programme_tree_add_block(tree, 9, "jingle", NULL);
    g_assert_cmpstr(member(created->block, "title"), ==, "Jingle eventi");
    radios_programme_tree_insert(tree, 2, 0, added);
    g_assert_cmpuint(radios_programme_tree_leaf_count(tree), ==, 3);
    g_assert_true(radios_programme_tree_remove(tree, B));
    JsonNode *node = radios_programme_tree_config(tree, config); JsonObject *next = json_node_get_object(node);
    g_assert_false(json_object_has_member(next, "timing"));
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(next, "blocks")), ==, 3);
    g_assert_cmpstr(member(element(next, "events", 1), "block_id"), ==, member(created->block, "id"));
    radios_programme_tree_clear(tree, 1);
    g_assert_cmpuint(radios_programme_tree_leaf_count(tree), ==, 1);
    json_node_free(node); radios_programme_tree_free(tree); json_object_unref(config);
}

static void test_plan_hints_are_transient_in_saved_programme(void)
{
    JsonObject *config = parse("{\"title\":\"P\",\"events\":[{\"id\":\"" A "\",\"kind\":\"music\","
        "\"media_id\":\"" B "\",\"planned_jingles\":[{\"title\":\"Gerçek jingle\",\"placement\":\"after\"}],"
        "\"planned_jingle\":{\"title\":\"Eski görünüm\"}}]}");
    RadioSProgrammeTree *tree = radios_programme_tree_new(config);
    JsonNode *saved = radios_programme_tree_config(tree, config);
    JsonObject *leaf = element(json_node_get_object(saved), "events", 0);
    g_assert_false(json_object_has_member(leaf, "planned_jingles"));
    g_assert_false(json_object_has_member(leaf, "planned_jingle"));
    g_assert_cmpstr(member(leaf, "id"), ==, A); g_assert_cmpstr(member(leaf, "media_id"), ==, B);
    g_assert_true(json_object_has_member(element(config, "events", 0), "planned_jingles"));
    g_assert_true(json_object_has_member(radios_programme_tree_leaf_at(tree, 0, NULL), "planned_jingles"));
    json_node_free(saved); radios_programme_tree_free(tree); json_object_unref(config);
}

static void test_implicit_music_runs_end_at_every_nonmusic_separator(void)
{
    JsonObject *config = parse("{\"title\":\"P\",\"note\":\"Korunan not\",\"events\":[]}");
    JsonArray *events = json_object_get_array_member(config, "events");
    const gchar *kinds[] = {"music", "music", "advertisement", "music", "music", "sweeper", "music", "jingle", "music", "announcement", "music"};
    const guint expected[] = {0, 0, 1, 2, 2, 3, 4, 5, 6, 7, 8};
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) {
        JsonObject *leaf = json_object_new(); gchar *id = g_strdup_printf("00000000-0000-4000-8000-%012u", i + 1);
        json_object_set_string_member(leaf, "id", id); json_object_set_string_member(leaf, "kind", kinds[i]);
        json_object_set_string_member(leaf, "title", "Korunan parça"); json_object_set_string_member(leaf, "note", "Parça notu");
        json_object_set_int_member(leaf, "duration_ms", 1000 + i); json_array_add_object_element(events, leaf); g_free(id);
    }
    RadioSProgrammeTree *tree = radios_programme_tree_new(config); g_assert_cmpuint(tree->blocks->len, ==, 9);
    JsonNode *saved = radios_programme_tree_config(tree, config); JsonObject *next = json_node_get_object(saved);
    RadioSProgrammeTree *again = radios_programme_tree_new(next);
    g_assert_cmpuint(again->blocks->len, ==, tree->blocks->len);
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) {
        guint block = G_MAXUINT; JsonObject *leaf = radios_programme_tree_leaf_at(tree, i, &block);
        g_assert_cmpuint(block, ==, expected[i]);
        g_assert_cmpstr(member(leaf, "id"), ==, member(element(config, "events", i), "id"));
        JsonObject *copy = element(next, "events", i);
        g_assert_cmpstr(member(copy, "id"), ==, member(leaf, "id"));
        g_assert_cmpstr(member(copy, "title"), ==, "Korunan parça");
        g_assert_cmpstr(member(copy, "note"), ==, "Parça notu");
        g_assert_cmpint(json_object_get_int_member(copy, "duration_ms"), ==, 1000 + i);
        g_assert_cmpstr(member(copy, "block_id"), ==,
            member(((RadioSProgrammeBlock *)g_ptr_array_index(again->blocks, block))->block, "id"));
        g_assert_false(json_object_has_member(element(config, "events", i), "block_id"));
    }
    gchar *derived = radios_programme_block_id_for(member(element(config, "events", 0), "id"));
    g_assert_cmpstr(member(((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks, 0))->block, "id"), ==, derived);
    g_assert_cmpint(radios_programme_block_duration(g_ptr_array_index(tree->blocks, 0)), ==, 2001);
    g_free(derived); radios_programme_tree_free(again); json_node_free(saved);
    radios_programme_tree_free(tree); json_object_unref(config);
}
static void test_adjacent_explicit_music_events_keep_operator_boundaries(void)
{
    JsonObject *config = parse("{\"title\":\"P\",\"blocks\":["
        "{\"id\":\"" X "\",\"kind\":\"music\",\"title\":\"Yerel event\",\"start_time\":\"10:00\",\"note\":\"Elle atama\"},"
        "{\"id\":\"" Y "\",\"kind\":\"music\",\"title\":\"İkinci ayrı event\",\"start_time\":\"11:00\",\"note\":\"Başka şema\"}],"
        "\"events\":[{\"id\":\"" A "\",\"block_id\":\"" X "\",\"kind\":\"music\"},"
        "{\"id\":\"" B "\",\"block_id\":\"" Y "\",\"kind\":\"music\"}]}");
    RadioSProgrammeTree *tree = radios_programme_tree_new(config);
    g_assert_cmpuint(tree->blocks->len, ==, 2);
    JsonNode *saved = radios_programme_tree_config(tree, config); JsonObject *next = json_node_get_object(saved);
    const gchar *ids[] = {X, Y}, *notes[] = {"Elle atama", "Başka şema"}, *times[] = {"10:00", "11:00"};
    for (guint i = 0; i < 2; ++i) {
        JsonObject *block = element(next, "blocks", i);
        g_assert_cmpstr(member(block, "id"), ==, ids[i]); g_assert_cmpstr(member(block, "note"), ==, notes[i]);
        g_assert_cmpstr(member(block, "start_time"), ==, times[i]);
        g_assert_cmpstr(member(element(next, "events", i), "block_id"), ==, ids[i]);
    }
    json_node_free(saved); radios_programme_tree_free(tree); json_object_unref(config);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/programme-model/legacy", test_legacy_events_get_stable_blocks);
    g_test_add_func("/programme-model/edit", test_blocks_edit_and_index);
    g_test_add_func("/programme-model/implicit-contiguous-music", test_implicit_music_runs_end_at_every_nonmusic_separator);
    g_test_add_func("/programme-model/explicit-adjacent-music-boundaries", test_adjacent_explicit_music_events_keep_operator_boundaries);
    g_test_add_func("/programme-model/transient-accepted-jingle-hints", test_plan_hints_are_transient_in_saved_programme);
    return g_test_run();
}
