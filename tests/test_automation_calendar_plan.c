#include "radios/automation_calendar_plan.h"
#include "radios/transition_preset.h"
#include "radios/programme_model.h"
#include "radios/playout.h"
#include <string.h>

static const gchar *station = "e0000000-0000-4000-8000-000000000001";
static const gchar *database = "/tmp/radios-calendar-plan.sdb";
static gchar *uuid(guint value) { return g_strdup_printf("00000000-0000-4000-8000-%012u", value); }
static JsonObject *at(JsonObject *object, const gchar *member, guint index)
{ return json_array_get_object_element(json_object_get_array_member(object, member), index); }
static gchar *serialized(JsonObject *object)
{ JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, object); gchar *result = json_to_string(node, FALSE); json_node_free(node); return result; }
static JsonObject *rule(guint number, guint frequency)
{
    JsonObject *object = json_object_new(); gchar *id = uuid(number + 1000), *playlist = uuid(number + 2000);
    json_object_set_string_member(object, "id", id); json_object_set_string_member(object, "playlist_id", playlist);
    json_object_set_int_member(object, "frequency", frequency);
    const gchar *flags[] = {"random", "track_random", "advert_random", "sweeper_random"};
    for (guint i = 0; i < G_N_ELEMENTS(flags); ++i) json_object_set_boolean_member(object, flags[i], FALSE);
    g_free(id); g_free(playlist); return object;
}
static JsonObject *event_new(void)
{
    JsonObject *object = json_object_new(); gchar *id = uuid(999);
    json_object_set_string_member(object, "id", id); g_free(id);
    json_object_set_string_member(object, "title", "Sabah programı"); json_object_set_string_member(object, "date", "2026-10-09");
    json_object_set_int_member(object, "start_minute", 480); json_object_set_int_member(object, "end_minute", 600);
    json_object_set_string_member(object, "color", "#90C8EB"); json_object_set_array_member(object, "rotations", json_array_new()); return object;
}
static JsonObject *row(guint id, const gchar *kind)
{
    JsonObject *object = json_object_new(), *detail = json_object_new(), *media = json_object_new(), *version = json_object_new(), *metadata = json_object_new();
    gchar *entry_id = uuid(id), *media_id = uuid(id + 10000), *version_id = uuid(id + 20000), *file_id = uuid(id + 30000), *item_id = uuid(id + 40000);
    gchar sha[65]; memset(sha, 'a', 64); sha[64] = 0; gchar *path = g_strdup_printf("%s.objects/%s.wav", database, media_id);
    json_object_set_string_member(object, "entry_id", entry_id); json_object_set_int_member(object, "ordinal", id);
    json_object_set_boolean_member(object, "eligible", TRUE); json_object_set_array_member(object, "reasons", json_array_new());
    json_object_set_string_member(media, "_id", media_id); json_object_set_string_member(media, "station_id", station);
    json_object_set_string_member(media, "version_id", version_id); json_object_set_string_member(media, "media_file_id", file_id);
    json_object_set_string_member(media, "item_id", item_id); json_object_set_string_member(media, "title", entry_id);
    json_object_set_string_member(media, "artist", "Sanatçı"); json_object_set_string_member(media, "album", "Albüm");
    json_object_set_string_member(version, "_id", version_id); json_object_set_string_member(version, "item_id", item_id);
    json_object_set_string_member(version, "media_file_id", file_id); json_object_set_string_member(version, "sha256", sha);
    json_object_set_string_member(version, "managed_path", path); json_object_set_int_member(version, "duration_ms", 1000);
    json_object_set_boolean_member(version, "verified", TRUE); json_object_set_boolean_member(version, "object_available", TRUE);
    json_object_set_string_member(metadata, "title", entry_id); json_object_set_string_member(metadata, "artist", "Sanatçı");
    json_object_set_string_member(metadata, "album", "Albüm"); json_object_set_string_member(metadata, "kind", kind);
    json_object_set_object_member(detail, "media", media); json_object_set_object_member(detail, "version", version);
    json_object_set_object_member(object, "detail", detail); json_object_set_object_member(object, "metadata", metadata);
    g_free(entry_id); g_free(media_id); g_free(version_id); g_free(file_id); g_free(item_id); g_free(path); return object;
}
static JsonObject *prepared_new(void)
{
    JsonObject *prepared = json_object_new(); json_object_set_int_member(prepared, "contract_version", 1);
    json_object_set_string_member(prepared, "kind", "automation"); json_object_set_string_member(prepared, "station_id", station);
    json_object_set_string_member(prepared, "timezone", "UTC"); json_object_set_array_member(prepared, "sources", json_array_new()); return prepared;
}
static JsonObject *add_source(JsonObject *event, JsonObject *prepared, guint id, guint frequency)
{
    JsonObject *rotation = rule(id, frequency), *source = json_object_new();
    json_array_add_object_element(json_object_get_array_member(event, "rotations"), rotation);
    json_object_set_string_member(source, "source_id", json_object_get_string_member(rotation, "id"));
    json_object_set_string_member(source, "playlist_id", json_object_get_string_member(rotation, "playlist_id"));
    json_object_set_string_member(source, "source_name", "Kaynak"); json_object_set_array_member(source, "reasons", json_array_new());
    json_object_set_array_member(source, "tracks", json_array_new());
    json_array_add_object_element(json_object_get_array_member(prepared, "sources"), source); return source;
}
static JsonNode *generate(JsonObject *event, JsonObject *prepared, const gchar *seed, guint count)
{
    GError *error = NULL; JsonNode *result = radios_automation_calendar_programme(event, prepared, station, seed, count, &error);
    g_assert_no_error(error); g_assert_nonnull(result); return result;
}
static const gchar *identity(JsonNode *programme, guint index)
{ return json_object_get_string_member(at(json_node_get_object(programme), "events", index), "media_id"); }
static void assert_identity(JsonNode *programme, guint index, guint id)
{ gchar *expected = uuid(id + 10000); g_assert_cmpstr(identity(programme, index), ==, expected); g_free(expected); }

static void test_prepare_bridge_deep_copy(void)
{
    JsonObject *event = event_new(), *prepared = prepared_new(); add_source(event, prepared, 1, 9);
    json_object_set_int_member(event, "end_minute", 1440); GError *error = NULL;
    JsonNode *defaults = radios_automation_calendar_prepare_config(event, NULL, &error); g_assert_no_error(error); g_assert_nonnull(defaults);
    JsonObject *legacy = json_node_get_object(defaults); json_object_set_string_member(legacy, "seed", "legacy-operator-seed");
    gchar *before = serialized(legacy); json_object_set_int_member(event, "start_minute", 720);
    JsonNode *result = radios_automation_calendar_prepare_config(event, legacy, &error); g_assert_no_error(error);
    JsonObject *config = json_node_get_object(result); g_assert_cmpstr(json_object_get_string_member(config, "startTime"), ==, "12:00");
    g_assert_cmpstr(json_object_get_string_member(config, "windowEnd"), ==, "00:00");
    g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(config, "weekdays"), 0), ==, 5);
    g_assert_cmpint(json_object_get_int_member(at(config, "sources", 0), "count"), ==, 9);
    g_assert_cmpstr(json_object_get_string_member(config, "seed"), ==, "legacy-operator-seed");
    json_object_set_string_member(at(config, "sources", 0), "playlist_id", "changed");
    json_array_add_string_element(json_object_get_array_member(config, "decks"), "3");
    gchar *after = serialized(legacy); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
    json_node_free(result); json_node_free(defaults); json_object_unref(event); json_object_unref(prepared);
}
static void test_frequency_order_and_reorder(void)
{
    JsonObject *event = event_new(), *prepared = prepared_new();
    JsonObject *a = add_source(event, prepared, 1, 2), *b = add_source(event, prepared, 2, 1);
    json_array_add_object_element(json_object_get_array_member(a, "tracks"), row(1, "music"));
    json_array_add_object_element(json_object_get_array_member(a, "tracks"), row(2, "music"));
    json_array_add_object_element(json_object_get_array_member(b, "tracks"), row(3, "music"));
    JsonNode *programme = generate(event, prepared, "fixed", 9); const guint expected[] = {1,2,3,1,2,3,1,2,3};
    for (guint i = 0; i < G_N_ELEMENTS(expected); ++i) assert_identity(programme, i, expected[i]);
    JsonArray *rules = json_object_get_array_member(event, "rotations"); JsonObject *first = json_object_ref(at(event, "rotations", 0));
    json_array_remove_element(rules, 0); json_array_add_object_element(rules, first);
    JsonNode *reordered = generate(event, prepared, "fixed", 6); const guint next[] = {3,1,2,3,1,2};
    for (guint i = 0; i < G_N_ELEMENTS(next); ++i) assert_identity(reordered, i, next[i]);
    json_node_free(programme); json_node_free(reordered); json_object_unref(event); json_object_unref(prepared);
}
static void test_music_jingle_blocks_preserve_playable_order(void)
{
    JsonObject *event = event_new(), *prepared = prepared_new();
    JsonObject *music = add_source(event, prepared, 1, 2), *jingles = add_source(event, prepared, 2, 1);
    json_array_add_object_element(json_object_get_array_member(music, "tracks"), row(1, "music"));
    json_array_add_object_element(json_object_get_array_member(music, "tracks"), row(2, "music"));
    json_array_add_object_element(json_object_get_array_member(jingles, "tracks"), row(3, "jingle"));
    gchar *before = serialized(prepared), *event_before = serialized(event);
    JsonNode *programme = generate(event, prepared, "music-jingle-blocks", 6);
    JsonObject *config = json_node_get_object(programme);
    JsonArray *blocks = json_object_get_array_member(config, "blocks");
    g_assert_cmpuint(json_array_get_length(blocks), ==, 4);
    const guint expected[] = {1, 2, 3, 1, 2, 3}, owner[] = {0, 0, 1, 2, 2, 3};
    for (guint i = 0; i < G_N_ELEMENTS(expected); ++i) {
        assert_identity(programme, i, expected[i]);
        JsonObject *entry = at(config, "events", i), *block = json_array_get_object_element(blocks, owner[i]);
        g_assert_true(g_uuid_string_is_valid(json_object_get_string_member(block, "id")));
        g_assert_cmpstr(json_object_get_string_member(entry, "block_id"), ==, json_object_get_string_member(block, "id"));
        g_assert_cmpstr(json_object_get_string_member(block, "kind"), ==, i % 3 == 2 ? "jingle" : "music");
    }
    g_assert_cmpstr(json_object_get_string_member(at(config, "blocks", 0), "id"), !=,
        json_object_get_string_member(at(config, "blocks", 2), "id"));
    gchar *after = serialized(prepared), *event_after = serialized(event);
    g_assert_cmpstr(before, ==, after); g_assert_cmpstr(event_before, ==, event_after);
    g_free(before); g_free(after); g_free(event_before); g_free(event_after);
    json_node_free(programme); json_object_unref(event); json_object_unref(prepared);
}

static void test_music_sources_keep_separate_event_blocks(void)
{
    JsonObject *event = event_new(), *prepared = prepared_new();
    const gchar *kinds[] = {"music", "music", "advertisement", "music", "music", "sweeper", "music"};
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) {
        JsonObject *source = add_source(event, prepared, i + 1, 1);
        json_array_add_object_element(json_object_get_array_member(source, "tracks"), row(i + 1, kinds[i]));
    }
    gchar *before = serialized(prepared), *event_before = serialized(event);
    JsonNode *programme = generate(event, prepared, "music-source-event-boundaries", 7);
    JsonObject *config = json_node_get_object(programme);
    const guint owners[] = {0, 1, 2, 3, 4, 5, 6};
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(config, "blocks")), ==, 7);
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) {
        assert_identity(programme, i, i + 1);
        g_assert_cmpstr(json_object_get_string_member(at(config, "events", i), "block_id"), ==,
            json_object_get_string_member(at(config, "blocks", owners[i]), "id"));
        g_assert_cmpstr(json_object_get_string_member(at(config, "blocks", owners[i]), "kind"), ==, kinds[i]);
    }
    g_assert_cmpstr(json_object_get_string_member(at(config, "blocks", 0), "title"), ==, "Müzik · Kaynak");
    gchar *after = serialized(prepared), *event_after = serialized(event);
    g_assert_cmpstr(before, ==, after); g_assert_cmpstr(event_before, ==, event_after);
    g_free(before); g_free(after); g_free(event_before); g_free(event_after);
    json_node_free(programme); json_object_unref(event); json_object_unref(prepared);
}

static void test_random_source_exact_quotas_and_fixed_rows(void)
{
    JsonObject *event = event_new(), *prepared = prepared_new();
    for (guint i = 1; i <= 3; ++i) { JsonObject *source = add_source(event, prepared, i, 2); json_array_add_object_element(json_object_get_array_member(source, "tracks"), row(i, "music")); }
    JsonNode *ordered = generate(event, prepared, "source-randomness", 30);
    json_object_set_boolean_member(at(event, "rotations", 0), "random", TRUE);
    JsonNode *shuffled = generate(event, prepared, "source-randomness", 30), *again = generate(event, prepared, "source-randomness", 30);
    gboolean different = FALSE;
    for (guint round = 0; round < 5; ++round) {
        guint counts[3] = {0}; GArray *fixed = g_array_new(FALSE, FALSE, sizeof(guint));
        for (guint j = 0; j < 6; ++j) {
            guint index = round * 6 + j; const gchar *id = identity(shuffled, index);
            g_assert_cmpstr(id, ==, identity(again, index)); different |= g_strcmp0(id, identity(ordered, index)) != 0;
            for (guint kind = 1; kind <= 3; ++kind) { gchar *candidate = uuid(kind + 10000);
                if (g_str_equal(id, candidate)) { ++counts[kind - 1]; if (kind > 1) g_array_append_val(fixed, kind); } g_free(candidate); }
        }
        for (guint i = 0; i < 3; ++i) g_assert_cmpuint(counts[i], ==, 2);
        const guint expected[] = {2,2,3,3}; g_assert_cmpuint(fixed->len, ==, 4);
        for (guint i = 0; i < 4; ++i) g_assert_cmpuint(g_array_index(fixed, guint, i), ==, expected[i]);
        g_array_unref(fixed);
    }
    g_assert_true(different); json_node_free(ordered); json_node_free(shuffled); json_node_free(again); json_object_unref(event); json_object_unref(prepared);
}
static void test_independent_music_advert_sweeper_flags(void)
{
    JsonObject *event = event_new(), *prepared = prepared_new(); JsonObject *source = add_source(event, prepared, 1, 1);
    const gchar *kinds[] = {"music", "advertisement", "sweeper"}, *flags[] = {"track_random", "advert_random", "sweeper_random"};
    for (guint i = 0; i < 15; ++i) json_array_add_object_element(json_object_get_array_member(source, "tracks"), row(i + 1, kinds[i % 3]));
    JsonNode *fixed = generate(event, prepared, "independent-randomness", 90);
    for (guint pool = 0; pool < 3; ++pool) {
        JsonObject *rotation = at(event, "rotations", 0); json_object_set_boolean_member(rotation, flags[pool], TRUE);
        JsonNode *random = generate(event, prepared, "independent-randomness", 90), *again = generate(event, prepared, "independent-randomness", 90);
        gboolean changed = FALSE;
        for (guint i = 0; i < 90; ++i) {
            g_assert_cmpstr(identity(random, i), ==, identity(again, i));
            g_assert_cmpstr(json_object_get_string_member(at(json_node_get_object(random), "events", i), "kind"), ==, kinds[i % 3]);
            if (i % 3 != pool) g_assert_cmpstr(identity(random, i), ==, identity(fixed, i));
            else changed |= g_strcmp0(identity(random, i), identity(fixed, i)) != 0;
        }
        for (guint round = 0; round < 6; ++round) {
            GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
            for (guint i = 0; i < 5; ++i) g_hash_table_add(seen, (gpointer)identity(random, round * 15 + i * 3 + pool));
            g_assert_cmpuint(g_hash_table_size(seen), ==, 5); g_hash_table_unref(seen);
        }
        g_assert_true(changed); json_node_free(random); json_node_free(again); json_object_set_boolean_member(rotation, flags[pool], FALSE);
    }
    json_node_free(fixed); json_object_unref(event); json_object_unref(prepared);
}
static void test_media_kind_filters_and_legacy_default(void)
{
    JsonObject *event = event_new(), *prepared = prepared_new(), *source = add_source(event, prepared, 1, 1);
    const gchar *kinds[] = {"music", "announcement", "advertisement", "jingle", "program", "sweeper", "news", "effect", "voice_track", "other"};
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i)
        json_array_add_object_element(json_object_get_array_member(source, "tracks"), row(i + 1, kinds[i]));
    JsonObject *rotation = at(event, "rotations", 0); gchar *before = serialized(prepared);
    JsonNode *legacy = generate(event, prepared, "kind-filter", 30);
    json_object_set_string_member(rotation, "media_kind", "all");
    JsonNode *all = generate(event, prepared, "kind-filter", 30);
    for (guint i = 0; i < 30; ++i) {
        g_assert_cmpstr(identity(legacy, i), ==, identity(all, i));
        assert_identity(all, i, i % G_N_ELEMENTS(kinds) + 1);
    }
    const gchar *filters[] = {"music", "jingle", "advertisement", "sweeper"};
    const guint expected[] = {1, 4, 3, 6};
    for (guint filter = 0; filter < G_N_ELEMENTS(filters); ++filter) {
        json_object_set_string_member(rotation, "media_kind", filters[filter]);
        JsonNode *selected = generate(event, prepared, "kind-filter", 12);
        for (guint i = 0; i < 12; ++i) {
            assert_identity(selected, i, expected[filter]);
            g_assert_cmpstr(json_object_get_string_member(at(json_node_get_object(selected), "events", i), "kind"), ==, filters[filter]);
        }
        GError *error = NULL; JsonNode *request = radios_automation_calendar_prepare_config(event, NULL, &error);
        g_assert_no_error(error); g_assert_nonnull(request);
        JsonObject *request_source = at(json_node_get_object(request), "sources", 0);
        g_assert_false(json_object_has_member(request_source, "media_kind"));
        g_assert_cmpstr(json_object_get_string_member(request_source, "playlist_id"), ==, json_object_get_string_member(rotation, "playlist_id"));
        g_assert_cmpstr(json_object_get_string_member(rotation, "media_kind"), ==, filters[filter]);
        json_node_free(request); json_node_free(selected);
    }
    gchar *after = serialized(prepared); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
    json_node_free(legacy); json_node_free(all); json_object_unref(event); json_object_unref(prepared);
}
static void test_media_kind_before_frequency_and_round_robin(void)
{
    JsonObject *event = event_new(), *prepared = prepared_new();
    JsonObject *a = add_source(event, prepared, 1, 2), *b = add_source(event, prepared, 2, 1);
    const gchar *kinds[] = {"music", "advertisement", "jingle", "sweeper", "jingle"};
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i)
        json_array_add_object_element(json_object_get_array_member(a, "tracks"), row(i + 1, kinds[i]));
    json_array_add_object_element(json_object_get_array_member(b, "tracks"), row(6, "jingle"));
    json_array_add_object_element(json_object_get_array_member(b, "tracks"), row(7, "music"));
    json_object_set_string_member(at(event, "rotations", 0), "media_kind", "jingle");
    json_object_set_string_member(at(event, "rotations", 1), "media_kind", "music");
    JsonNode *selected = generate(event, prepared, "frequency-kind-filter", 9);
    const guint expected[] = {3, 5, 7, 3, 5, 7, 3, 5, 7};
    for (guint i = 0; i < G_N_ELEMENTS(expected); ++i) assert_identity(selected, i, expected[i]);
    json_node_free(selected); json_object_unref(event); json_object_unref(prepared);
}
static void test_media_kind_invalid_empty_and_security(void)
{
    JsonObject *event = event_new(), *prepared = prepared_new(), *source = add_source(event, prepared, 1, 1);
    json_array_add_object_element(json_object_get_array_member(source, "tracks"), row(1, "music"));
    JsonObject *rotation = at(event, "rotations", 0); GError *error = NULL;
    const gchar *invalid[] = {"\"unknown\"", "\"track\"", "\"Music\"", "\"\"", "null", "true", "1", "[]", "{}"};
    for (guint i = 0; i < G_N_ELEMENTS(invalid); ++i) {
        JsonParser *parser = json_parser_new(); g_assert_true(json_parser_load_from_data(parser, invalid[i], -1, &error)); g_assert_no_error(error);
        json_object_set_member(rotation, "media_kind", json_node_copy(json_parser_get_root(parser))); g_object_unref(parser);
        g_assert_null(radios_automation_calendar_prepare_config(event, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error);
        g_assert_null(radios_automation_calendar_programme(event, prepared, station, "invalid-kind", 3, &error)); g_assert_nonnull(error); g_clear_error(&error);
    }
    json_object_set_string_member(rotation, "media_kind", "jingle");
    g_assert_null(radios_automation_calendar_programme(event, prepared, station, "missing-kind", 3, &error)); g_assert_nonnull(error); g_clear_error(&error);
    json_array_add_object_element(json_object_get_array_member(source, "tracks"), row(2, "jingle"));
    JsonObject *foreign = json_object_get_object_member(json_object_get_object_member(at(source, "tracks", 0), "detail"), "media");
    json_object_set_string_member(foreign, "station_id", "f0000000-0000-4000-8000-000000000001");
    g_assert_null(radios_automation_calendar_programme(event, prepared, station, "filtered-security", 3, &error)); g_assert_nonnull(error); g_clear_error(&error);
    json_object_unref(event); json_object_unref(prepared);
}
static void test_duration_cap_unavailable_diagnostic_and_security(void)
{
    JsonObject *event = event_new(), *prepared = prepared_new(), *source = add_source(event, prepared, 1, 1);
    JsonObject *missing = row(1, "music"), *valid = row(2, "music"); json_object_set_boolean_member(missing, "eligible", FALSE);
    json_array_add_object_element(json_object_get_array_member(source, "tracks"), missing); json_array_add_object_element(json_object_get_array_member(source, "tracks"), valid);
    json_object_set_int_member(json_object_get_object_member(json_object_get_object_member(valid, "detail"), "version"), "duration_ms", 25000);
    json_object_set_int_member(event, "end_minute", 481); gchar *before = serialized(prepared); GError *error = NULL;
    JsonNode *response = radios_automation_calendar_prepared_programme(event, prepared, station, "budget", 200, &error); g_assert_no_error(error);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(response), "tracks")), ==, 2);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(response), "diagnostics")), ==, 2);
    gchar *after = serialized(prepared); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after); json_node_free(response);
    JsonObject *media = json_object_get_object_member(json_object_get_object_member(valid, "detail"), "media");
    json_object_set_string_member(media, "station_id", "f0000000-0000-4000-8000-000000000001");
    g_assert_null(radios_automation_calendar_programme(event, prepared, station, "budget", 10, &error)); g_assert_nonnull(error); g_clear_error(&error);
    json_object_set_string_member(media, "station_id", station); json_object_set_int_member(event, "start_minute", 481);
    g_assert_null(radios_automation_calendar_programme(event, prepared, station, "budget", 10, &error)); g_assert_nonnull(error); g_clear_error(&error);
    json_object_set_int_member(event, "start_minute", 480);
    g_assert_null(radios_automation_calendar_programme(event, prepared, station, "budget", 201, &error)); g_assert_nonnull(error); g_clear_error(&error);
    json_object_set_string_member(at(event, "rotations", 0), "track_random", "true");
    g_assert_null(radios_automation_calendar_prepare_config(event, NULL, &error)); g_assert_nonnull(error); g_clear_error(&error);
    json_object_unref(event); json_object_unref(prepared);
}
static void test_frozen_bindings_deep_copy_enter_existing_planner(void)
{
    JsonObject *event = event_new(), *prepared = prepared_new(), *source = add_source(event, prepared, 1, 1);
    JsonObject *original = row(1, "music"); json_array_add_object_element(json_object_get_array_member(source, "tracks"), original);
    gchar *before = serialized(prepared); GError *error = NULL;
    JsonNode *response = radios_automation_calendar_prepared_programme(event, prepared, station, "actual-plan", 3, &error); g_assert_no_error(error);
    JsonObject *output = json_node_get_object(response), *config = json_object_get_object_member(output, "config");
    GHashTable *ids = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < 3; ++i) {
        JsonObject *entry = at(config, "events", i), *track = at(output, "tracks", i);
        const gchar *id = json_object_get_string_member(entry, "id"); g_assert_true(g_uuid_string_is_valid(id)); g_hash_table_add(ids, (gpointer)id);
        g_assert_cmpstr(id, ==, json_object_get_string_member(track, "entry_id"));
        g_assert_cmpstr(json_object_get_string_member(track, "calendar_source_name"), ==, "Kaynak");
        g_assert_cmpstr(json_object_get_string_member(track, "calendar_playlist_id"), ==, json_object_get_string_member(source, "playlist_id"));
        g_assert_cmpstr(json_object_get_string_member(entry, "primary_file"), ==, "");
        g_assert_cmpstr(json_object_get_string_member(entry, "media_id"), ==, json_object_get_string_member(json_object_get_object_member(json_object_get_object_member(track, "detail"), "media"), "_id"));
    }
    g_assert_cmpuint(g_hash_table_size(ids), ==, 3); g_hash_table_unref(ids);
    GDateTime *now = g_date_time_new_from_iso8601("2026-10-09T07:00:00Z", NULL);
    RadioSPlayoutRequest request = {.station_id = station, .database_file = database, .now_utc_us = g_date_time_to_unix(now) * G_USEC_PER_SEC}; g_date_time_unref(now);
    RadioSPlayoutPlan *plan = radios_playout_plan_build(output, &request, &error); g_assert_no_error(error); g_assert_nonnull(plan);
    g_assert_cmpuint(plan->entries->len, ==, 3);
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(plan->display), "programme_title"), ==, "Sabah programı");
    g_assert_cmpint(plan->options.start_utc_us, ==, request.now_utc_us + G_GINT64_CONSTANT(3600000000));
    RadioSPlayoutEntry *first = g_ptr_array_index(plan->entries, 0); g_assert_cmpuint(first->run.media.cue_out_frame, ==, 48000);
    g_assert_cmpstr(first->source_name, ==, "Kaynak");
    JsonObject *display_first = at(json_node_get_object(plan->display), "entries", 0);
    g_assert_cmpstr(json_object_get_string_member(display_first, "source_name"), ==, "Kaynak");
    g_assert_cmpstr(json_object_get_string_member(display_first, "playlist_id"), ==, json_object_get_string_member(source, "playlist_id"));
    json_object_set_string_member(json_object_get_object_member(json_object_get_object_member(at(output, "tracks", 0), "detail"), "media"), "title", "mutated-copy");
    gchar *after = serialized(prepared); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
    g_assert_cmpstr(first->title, !=, "mutated-copy"); radios_playout_plan_free(plan); json_node_free(response); json_object_unref(event); json_object_unref(prepared);
}

static JsonNode *copy_node(JsonObject *object)
{ gchar *value = serialized(object); JsonParser *parser = json_parser_new(); g_assert_true(json_parser_load_from_data(parser, value, -1, NULL)); JsonNode *node = json_node_copy(json_parser_get_root(parser)); g_object_unref(parser); g_free(value); return node; }
static JsonObject *settings_new(const gchar *mode)
{
    JsonObject *settings = json_object_new(); json_object_set_string_member(settings, "selection_mode", mode);
    const gchar *gaps[] = {"track_gap_minutes", "artist_gap_minutes", "album_gap_minutes"};
    for (guint i = 0; i < G_N_ELEMENTS(gaps); ++i) json_object_set_int_member(settings, gaps[i], 0);
    JsonArray *choices = json_array_new(); JsonObject *choice = json_object_new();
    json_object_set_int_member(choice, "mode", 0); json_object_set_int_member(choice, "weight", 100); json_object_set_int_member(choice, "overlap_ms", 0);
    json_array_add_object_element(choices, choice); json_object_set_array_member(settings, "transitions", choices); return settings;
}
static void set_transition(JsonObject *settings, guint mode, guint overlap)
{
    JsonArray *choices = json_array_new(); JsonObject *choice = json_object_new();
    json_object_set_int_member(choice, "mode", mode); json_object_set_int_member(choice, "weight", 100); json_object_set_int_member(choice, "overlap_ms", overlap);
    json_array_add_object_element(choices, choice); json_object_set_array_member(settings, "transitions", choices);
}
static JsonObject *calendar_new(JsonObject **event, JsonObject **definition)
{
    JsonObject *calendar = json_object_new(); *event = event_new(); *definition = json_object_new();
    gchar *id = uuid(700), *assignment_id = uuid(800);
    json_object_set_string_member(*definition, "id", id); json_object_set_string_member(*definition, "name", "Sabah rotasyonu");
    json_object_set_object_member(*definition, "settings", settings_new("linear"));
    JsonArray *sources = json_array_new(); json_array_add_object_element(sources, rule(1, 1)); json_array_add_object_element(sources, rule(2, 1));
    json_object_set_array_member(*definition, "sources", sources);
    JsonObject *assignment = json_object_new(); json_object_set_string_member(assignment, "id", assignment_id); json_object_set_string_member(assignment, "rotation_id", id);
    json_array_add_object_element(json_object_get_array_member(*event, "rotations"), assignment);
    JsonArray *library = json_array_new(); json_array_add_object_element(library, *definition); json_object_set_array_member(calendar, "rotation_library", library);
    JsonArray *events = json_array_new(); json_array_add_object_element(events, *event); json_object_set_array_member(calendar, "events", events);
    json_object_set_int_member(calendar, "schema", 1); g_free(id); g_free(assignment_id); return calendar;
}
static JsonNode *resolve(JsonObject *calendar, JsonObject *event)
{ GError *error = NULL; JsonNode *result = radios_automation_calendar_resolve_event(calendar, event, &error); g_assert_no_error(error); g_assert_nonnull(result); return result; }
static JsonObject *prepared_for(JsonObject *resolved)
{
    JsonObject *prepared = prepared_new(); JsonArray *rules = json_object_get_array_member(resolved, "rotations");
    for (guint i = 0; i < json_array_get_length(rules); ++i) {
        JsonObject *rotation = json_array_get_object_element(rules, i), *source = json_object_new();
        json_object_set_string_member(source, "source_id", json_object_get_string_member(rotation, "id"));
        json_object_set_string_member(source, "playlist_id", json_object_get_string_member(rotation, "playlist_id"));
        json_object_set_string_member(source, "source_name", "Gerçek seçki"); json_object_set_array_member(source, "reasons", json_array_new());
        JsonArray *tracks = json_array_new(); for (guint j = 0; j < 2; ++j) json_array_add_object_element(tracks, row((i % 2) * 2 + j + 1, "music"));
        json_object_set_array_member(source, "tracks", tracks); json_array_add_object_element(json_object_get_array_member(prepared, "sources"), source);
    }
    return prepared;
}
static JsonNode *compiled(JsonObject *event, JsonObject *prepared, guint count)
{ GError *error = NULL; JsonNode *node = radios_automation_calendar_prepared_programme(event, prepared, station, "named-fixed", count, &error); g_assert_no_error(error); g_assert_nonnull(node); return node; }
static void expect_track(JsonNode *node, guint index, guint id)
{
    JsonObject *media = json_object_get_object_member(json_object_get_object_member(at(json_node_get_object(node), "tracks", index), "detail"), "media");
    gchar *expected = uuid(id + 10000); g_assert_cmpstr(json_object_get_string_member(media, "_id"), ==, expected); g_free(expected);
}
static void test_named_resolver_identity_overrides_and_invalidation(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    JsonObject *override = json_object_new(); json_object_set_int_member(override, "artist_gap_minutes", 3);
    json_object_set_object_member(at(definition, "sources", 0), "settings", override);
    JsonObject *second = json_object_new(); gchar *id = uuid(801); json_object_set_string_member(second, "id", id); g_free(id);
    json_object_set_string_member(second, "rotation_id", json_object_get_string_member(definition, "id")); json_array_add_object_element(json_object_get_array_member(event, "rotations"), second);
    gchar *before = serialized(calendar); JsonNode *a = resolve(calendar, event), *b = resolve(calendar, event); JsonObject *ao = json_node_get_object(a);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(ao, "rotations")), ==, 4);
    gchar *as = serialized(ao), *bs = serialized(json_node_get_object(b)); g_assert_cmpstr(as, ==, bs); g_free(as); g_free(bs);
    GHashTable *ids = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; i < 4; ++i) g_hash_table_add(ids, (gpointer)json_object_get_string_member(at(ao, "rotations", i), "id"));
    g_assert_cmpuint(g_hash_table_size(ids), ==, 4); g_hash_table_unref(ids);
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(at(ao, "rotations", 0), "calendar_settings"), "artist_gap_minutes"), ==, 3);
    gchar *after = serialized(calendar); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
    json_object_set_int_member(json_object_get_object_member(definition, "settings"), "track_gap_minutes", 9);
    JsonNode *changed = resolve(calendar, event); g_assert_false(json_node_equal(a, changed));
    g_assert_cmpstr(json_object_get_string_member(at(ao, "rotations", 0), "id"), ==, json_object_get_string_member(at(json_node_get_object(changed), "rotations", 0), "id"));
    GError *error = NULL; json_object_set_string_member(at(event, "rotations", 0), "rotation_id", "00000000-0000-4000-8000-000000008888");
    g_assert_null(radios_automation_calendar_resolve_event(calendar, event, &error)); g_assert_nonnull(error); g_clear_error(&error);
    json_node_free(a); json_node_free(b); json_node_free(changed); json_object_unref(calendar);
}
static void test_resolver_errors_identify_block_and_exact_prerequisite(void)
{
    const gchar *reasons[] = {"rotasyon atanmamış", "kitaplığı", "bulunamadı", "playlist kaynağı yok", "adı", "rotasyon atanmamış", "rotasyon atanmamış"};
    for (guint failure = 0; failure < G_N_ELEMENTS(reasons); ++failure) {
        JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
        json_object_set_string_member(event, "title", "Sabah\nprogramı");
        if (failure == 0 || failure >= 5) json_object_set_array_member(event, "rotations", json_array_new());
        else if (failure == 1) json_object_remove_member(calendar, "rotation_library");
        else if (failure == 2) json_object_set_string_member(at(event, "rotations", 0), "rotation_id", "00000000-0000-4000-8000-000000008888");
        else if (failure == 3) json_object_set_array_member(definition, "sources", json_array_new());
        else json_object_set_string_member(event, "title", "");
        if (failure == 5) json_object_set_string_member(event, "title", "Bozuk \xff ad");
        if (failure == 6) {
            gchar *huge = g_strnfill(1024 * 1024, 'A'); json_object_set_string_member(event, "title", huge); g_free(huge);
        }
        GError *error = NULL;
        g_assert_null(radios_automation_calendar_resolve_event(calendar, event, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_assert_nonnull(strstr(error->message, reasons[failure]));
        g_assert_nonnull(strstr(error->message, "2026-10-09"));
        g_assert_nonnull(strstr(error->message, "08:00–10:00"));
        if (failure < 4) g_assert_nonnull(strstr(error->message, "Sabah programı"));
        g_assert_true(g_utf8_validate(error->message, -1, NULL));
        g_assert_null(strchr(error->message, '\n')); g_assert_cmpuint(strlen(error->message), <, 1024);
        g_clear_error(&error); json_object_unref(calendar);
    }
}
static void test_named_linear_count_frequency_and_empty_logical_cycles(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition); json_object_set_int_member(at(definition, "sources", 0), "count", 2);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    JsonNode *output = compiled(json_node_get_object(resolved), prepared, 9); const guint sequence[] = {1,2,3,1,2,4,1,2,3};
    for (guint i = 0; i < G_N_ELEMENTS(sequence); ++i) expect_track(output, i, sequence[i]);
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared);
    json_object_set_int_member(at(definition, "sources", 1), "frequency", 2);
    resolved = resolve(calendar, event); prepared = prepared_for(json_node_get_object(resolved)); output = compiled(json_node_get_object(resolved), prepared, 7);
    const guint nth[] = {1,2,1,2,3,1,2}; for (guint i = 0; i < G_N_ELEMENTS(nth); ++i) expect_track(output, i, nth[i]);
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared);
    json_object_set_int_member(at(definition, "sources", 0), "frequency", 2);
    json_object_set_int_member(at(definition, "sources", 0), "count", 1);
    resolved = resolve(calendar, event); prepared = prepared_for(json_node_get_object(resolved)); output = compiled(json_node_get_object(resolved), prepared, 8);
    const guint empty_cycles[] = {1,3,2,4,1,3,2,4}; for (guint i = 0; i < G_N_ELEMENTS(empty_cycles); ++i) expect_track(output, i, empty_cycles[i]);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(output), "tracks")), ==, 8);
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
static void test_named_weighted_selection_reproducible_relative_weights(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition); JsonObject *settings = json_object_get_object_member(definition, "settings");
    json_object_set_string_member(settings, "selection_mode", "weighted"); json_object_set_int_member(at(definition, "sources", 0), "weight", 70); json_object_set_int_member(at(definition, "sources", 1), "weight", 30);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    JsonNode *a = compiled(json_node_get_object(resolved), prepared, 200), *b = compiled(json_node_get_object(resolved), prepared, 200);
    guint first = 0; gchar *one = uuid(10001), *two = uuid(10002);
    for (guint i = 0; i < 200; ++i) {
        JsonObject *am = json_object_get_object_member(json_object_get_object_member(at(json_node_get_object(a), "tracks", i), "detail"), "media");
        JsonObject *bm = json_object_get_object_member(json_object_get_object_member(at(json_node_get_object(b), "tracks", i), "detail"), "media");
        const gchar *id = json_object_get_string_member(am, "_id"); g_assert_cmpstr(id, ==, json_object_get_string_member(bm, "_id"));
        first += g_str_equal(id, one) || g_str_equal(id, two);
    }
    g_assert_cmpuint(first, >, 110); g_assert_cmpuint(first, <, 170); g_free(one); g_free(two);
    json_node_free(a); json_node_free(b); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
static void test_named_interval_turn_start_and_window_limits(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition); JsonArray *sources = json_object_get_array_member(definition, "sources");
    json_array_remove_element(sources, 1); JsonObject *rule_a = at(definition, "sources", 0);
    json_object_set_int_member(rule_a, "count", 2); json_object_set_int_member(rule_a, "interval_minutes", 1);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    JsonNode *output = compiled(json_node_get_object(resolved), prepared, 20); JsonObject *o = json_node_get_object(output);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(o, "tracks")), ==, 2);
    g_assert_cmpstr(json_object_get_string_member(at(o, "diagnostics", 0), "reason"), ==, "calendar_no_legal_candidate");
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared);
    json_object_set_int_member(rule_a, "interval_minutes", 0); json_object_set_int_member(rule_a, "start_minute", 481);
    resolved = resolve(calendar, event); prepared = prepared_for(json_node_get_object(resolved)); GError *error = NULL;
    g_assert_null(radios_automation_calendar_prepared_programme(json_node_get_object(resolved), prepared, station, "named-fixed", 20, &error));
    g_assert_nonnull(error); g_clear_error(&error); json_node_free(resolved); json_object_unref(prepared);
    json_object_set_int_member(rule_a, "start_minute", 480); json_object_set_int_member(rule_a, "end_minute", 481);
    resolved = resolve(calendar, event); prepared = prepared_for(json_node_get_object(resolved));
    for (guint i = 0; i < 2; ++i) json_object_set_int_member(json_object_get_object_member(json_object_get_object_member(at(at(prepared, "sources", 0), "tracks", i), "detail"), "version"), "duration_ms", 40000);
    output = compiled(json_node_get_object(resolved), prepared, 20);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(output), "tracks")), ==, 1);
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
static void test_named_gap_history_and_short_burst_yields_other_group(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition); JsonObject *settings = json_object_get_object_member(definition, "settings");
    json_object_set_int_member(settings, "artist_gap_minutes", 30); json_object_set_int_member(at(definition, "sources", 0), "count", 3);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    for (guint i = 0; i < 2; ++i) {
        JsonObject *track = at(at(prepared, "sources", 1), "tracks", i), *metadata = json_object_get_object_member(track, "metadata");
        json_object_set_string_member(metadata, "artist", "Başka sanatçı");
        json_object_set_string_member(json_object_get_object_member(json_object_get_object_member(track, "detail"), "media"), "artist", "Başka sanatçı");
    }
    JsonNode *output = compiled(json_node_get_object(resolved), prepared, 20); JsonObject *o = json_node_get_object(output);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(o, "tracks")), ==, 2); expect_track(output, 0, 1); expect_track(output, 1, 3);
    g_assert_cmpstr(json_object_get_string_member(at(o, "diagnostics", 0), "reason"), ==, "calendar_no_legal_candidate");
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared);
    json_object_set_int_member(settings, "artist_gap_minutes", 0); json_object_set_int_member(settings, "track_gap_minutes", 1);
    resolved = resolve(calendar, event); prepared = prepared_for(json_node_get_object(resolved)); output = compiled(json_node_get_object(resolved), prepared, 20);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(output), "tracks")), ==, 4); json_node_free(output);
    JsonObject *history = json_object_new(), *history_config = json_object_new(), *record = json_object_new(); JsonArray *records = json_array_new();
    gchar *mid = uuid(10001), *vid = uuid(20001); json_object_set_string_member(record, "media_id", mid); json_object_set_string_member(record, "version_id", vid); g_free(mid); g_free(vid);
    json_object_set_string_member(record, "artist", "Sanatçı"); json_object_set_string_member(record, "album", "Albüm");
    json_object_set_string_member(record, "started_utc", "2026-10-09T07:59:59Z"); json_object_set_int_member(record, "position_frames", 1);
    json_array_add_object_element(records, record); json_object_set_array_member(history_config, "entries", records); json_object_set_object_member(history, "config", history_config); json_object_set_object_member(prepared, "history", history);
    output = compiled(json_node_get_object(resolved), prepared, 20); expect_track(output, 0, 2);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(output), "tracks")), ==, 3);
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared);
    json_object_set_int_member(settings, "track_gap_minutes", 0); json_object_set_int_member(settings, "album_gap_minutes", 1);
    resolved = resolve(calendar, event); prepared = prepared_for(json_node_get_object(resolved)); output = compiled(json_node_get_object(resolved), prepared, 20);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(output), "tracks")), ==, 1);
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
static RadioSPlayoutRequest request_new(void)
{
    GDateTime *now = g_date_time_new_from_iso8601("2026-10-09T07:00:00Z", NULL);
    RadioSPlayoutRequest request = {.station_id = station, .database_file = database, .now_utc_us = g_date_time_to_unix(now) * G_USEC_PER_SEC}; g_date_time_unref(now); return request;
}
static void test_named_phase_two_merge_and_actual_transition_plan(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition); set_transition(json_object_get_object_member(definition, "settings"), 1, 250);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved)); JsonNode *selection = compiled(json_node_get_object(resolved), prepared, 4);
    JsonObject *selected = json_node_get_object(selection); JsonNode *fresh = copy_node(selected); JsonObject *verified = json_node_get_object(fresh);
    for (guint i = 0; i < 4; ++i) {
        JsonObject *track = at(verified, "tracks", i);
        json_object_remove_member(track, "calendar_transition");
        json_object_remove_member(track, "calendar_source_name");
        json_object_remove_member(track, "calendar_playlist_id");
    }
    json_object_set_string_member(json_object_get_object_member(at(verified, "tracks", 0), "metadata"), "title", "Yeni doğrulanmış başlık");
    gchar *before = serialized(verified); GError *error = NULL; JsonNode *merged = radios_automation_calendar_merge_selection(verified, selected, &error); g_assert_no_error(error); g_assert_nonnull(merged);
    gchar *after = serialized(verified); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
    RadioSPlayoutRequest request = request_new(); RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(merged), &request, &error); g_assert_no_error(error); g_assert_nonnull(plan);
    g_assert_cmpuint(plan->options.pool_count, ==, 2); g_assert_cmpuint(plan->options.deck_pool[0], ==, 0); g_assert_cmpuint(plan->options.deck_pool[1], ==, 1);
    RadioSPlayoutEntry *a = g_ptr_array_index(plan->entries, 0), *b = g_ptr_array_index(plan->entries, 1);
    g_assert_cmpint(a->run.mode, ==, RADIOS_AUDIO_RUN_CROSSFADE); g_assert_cmpuint(a->run.overlap_frames, ==, 12000); g_assert_cmpuint(b->run.fade_in_frames, ==, 12000);
    g_assert_cmpint(b->planned_utc_us - a->planned_utc_us, ==, 750000); g_assert_cmpstr(a->title, ==, "Yeni doğrulanmış başlık");
    g_assert_cmpstr(a->source_name, ==, "Gerçek seçki");
    g_assert_cmpstr(json_object_get_string_member(a->binding, "playlist_id"), ==,
        json_object_get_string_member(at(selected, "tracks", 0), "calendar_playlist_id"));
    radios_playout_plan_free(plan);
    for (guint corruption = 0; corruption < 2; ++corruption) {
        JsonNode *bad = copy_node(selected); JsonObject *track = at(json_node_get_object(bad), "tracks", 0);
        if (corruption == 0) json_object_set_int_member(track, "calendar_source_name", 7);
        else json_object_set_string_member(track, "calendar_playlist_id", "invalid");
        g_assert_null(radios_automation_calendar_merge_selection(verified, json_node_get_object(bad), &error));
        g_assert_nonnull(error); g_clear_error(&error); json_node_free(bad);
    }
    for (guint corruption = 0; corruption < 5; ++corruption) {
        JsonNode *bad = copy_node(verified); JsonObject *track = at(json_node_get_object(bad), "tracks", 0), *detail = json_object_get_object_member(track, "detail");
        JsonObject *media = json_object_get_object_member(detail, "media"), *version = json_object_get_object_member(detail, "version"); gchar *id = uuid(98765);
        if (corruption == 0) json_object_set_string_member(track, "entry_id", id);
        else if (corruption == 1) json_object_set_string_member(media, "_id", id);
        else if (corruption == 2) { json_object_set_string_member(version, "_id", id); json_object_set_string_member(media, "version_id", id); }
        else if (corruption == 3) { json_object_set_string_member(version, "media_file_id", id); json_object_set_string_member(media, "media_file_id", id); }
        else json_object_set_string_member(version, "sha256", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
        g_free(id); g_assert_null(radios_automation_calendar_merge_selection(json_node_get_object(bad), selected, &error)); g_assert_nonnull(error); g_clear_error(&error); json_node_free(bad);
    }
    json_node_free(merged); json_node_free(fresh); json_node_free(selection); json_node_free(resolved); json_object_unref(prepared);
    set_transition(json_object_get_object_member(definition, "settings"), 2, 100); resolved = resolve(calendar, event); prepared = prepared_for(json_node_get_object(resolved)); selection = compiled(json_node_get_object(resolved), prepared, 2);
    plan = radios_playout_plan_build(json_node_get_object(selection), &request, &error); g_assert_no_error(error); g_assert_nonnull(plan);
    a = g_ptr_array_index(plan->entries, 0); b = g_ptr_array_index(plan->entries, 1); g_assert_cmpint(a->run.mode, ==, RADIOS_AUDIO_RUN_CUE);
    g_assert_cmpuint(a->run.auto_next_frame, ==, 48000); g_assert_cmpint(b->planned_utc_us - a->planned_utc_us, ==, 1000000);
    radios_playout_plan_free(plan); json_node_free(selection); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}

static void test_named_transition_probabilities_independent_track_shuffle(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition); JsonObject *settings = json_object_get_object_member(definition, "settings");
    for (guint i = 0; i < 2; ++i) json_object_set_boolean_member(at(definition, "sources", i), "track_random", TRUE);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved)); JsonNode *baseline = compiled(json_node_get_object(resolved), prepared, 200);
    json_node_free(resolved); json_object_unref(prepared);
    JsonArray *choices = json_array_new();
    for (guint mode = 0; mode < 2; ++mode) {
        JsonObject *choice = json_object_new(); json_object_set_int_member(choice, "mode", mode);
        json_object_set_int_member(choice, "weight", mode ? 80 : 20); json_object_set_int_member(choice, "overlap_ms", mode ? 250 : 0); json_array_add_object_element(choices, choice);
    }
    json_object_set_array_member(settings, "transitions", choices); resolved = resolve(calendar, event); prepared = prepared_for(json_node_get_object(resolved));
    JsonNode *output = compiled(json_node_get_object(resolved), prepared, 200), *again = compiled(json_node_get_object(resolved), prepared, 200); guint crossfades = 0;
    for (guint i = 0; i < 200; ++i) {
        JsonObject *track = at(json_node_get_object(output), "tracks", i), *hint = json_object_get_object_member(track, "calendar_transition");
        JsonObject *same = json_object_get_object_member(at(json_node_get_object(again), "tracks", i), "calendar_transition");
        JsonObject *media = json_object_get_object_member(json_object_get_object_member(track, "detail"), "media");
        JsonObject *old = json_object_get_object_member(json_object_get_object_member(at(json_node_get_object(baseline), "tracks", i), "detail"), "media");
        g_assert_cmpstr(json_object_get_string_member(media, "_id"), ==, json_object_get_string_member(old, "_id"));
        g_assert_cmpint(json_object_get_int_member(hint, "mode"), ==, json_object_get_int_member(same, "mode"));
        crossfades += json_object_get_int_member(hint, "mode") == 1;
    }
    g_assert_cmpuint(crossfades, >, 130); g_assert_cmpuint(crossfades, <, 190);
    json_node_free(output); json_node_free(again); json_node_free(baseline); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
static void test_named_verified_artist_keys_and_cue_timing(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition); JsonObject *settings = json_object_get_object_member(definition, "settings");
    json_object_set_int_member(settings, "artist_gap_minutes", 1); JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    for (guint source = 0; source < 2; ++source) for (guint i = 0; i < 2; ++i) {
        JsonObject *track = at(at(prepared, "sources", source), "tracks", i);
        json_object_set_string_member(json_object_get_object_member(track, "metadata"), "artist", source ? "Yanlış metadata B" : "Yanlış metadata A");
        json_object_set_string_member(json_object_get_object_member(json_object_get_object_member(track, "detail"), "media"), "artist", source ? "ışık" : "IŞIK");
    }
    JsonNode *output = compiled(json_node_get_object(resolved), prepared, 20);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(output), "tracks")), ==, 1);
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(at(json_node_get_object(output), "tracks", 0), "metadata"), "artist"), ==, "IŞIK");
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_set_int_member(settings, "artist_gap_minutes", 0); set_transition(settings, 2, 100);
    resolved = resolve(calendar, event); prepared = prepared_for(json_node_get_object(resolved));
    for (guint source = 0; source < 2; ++source) for (guint i = 0; i < 2; ++i) {
        JsonObject *track = at(at(prepared, "sources", source), "tracks", i), *detail = json_object_get_object_member(track, "detail");
        JsonObject *media = json_object_get_object_member(detail, "media"), *version = json_object_get_object_member(detail, "version"), *markers = json_object_new();
        json_object_set_string_member(markers, "station_id", station); json_object_set_string_member(markers, "version_id", json_object_get_string_member(version, "_id"));
        json_object_set_string_member(markers, "media_file_id", json_object_get_string_member(media, "media_file_id")); json_object_set_string_member(markers, "media_sha256", json_object_get_string_member(version, "sha256"));
        json_object_set_int_member(markers, "media_duration_ms", 1000); json_object_set_int_member(markers, "cue_in_ms", 100); json_object_set_int_member(markers, "out_ms", 1000);
        json_object_set_int_member(markers, "auto_next_ms", 700); json_object_set_int_member(markers, "overlap_ms", 300); json_object_set_int_member(markers, "fade_start_ms", 700); json_object_set_int_member(markers, "fade_end_ms", 1000);
        json_object_set_boolean_member(markers, "automatic", TRUE); json_object_set_object_member(detail, "markers", markers);
    }
    output = compiled(json_node_get_object(resolved), prepared, 3); GError *error = NULL; RadioSPlayoutRequest request = request_new();
    RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(output), &request, &error); g_assert_no_error(error); g_assert_nonnull(plan);
    RadioSPlayoutEntry *a = g_ptr_array_index(plan->entries, 0), *b = g_ptr_array_index(plan->entries, 1);
    g_assert_cmpuint(plan->options.pool_count, ==, 2); g_assert_cmpint(a->run.mode, ==, RADIOS_AUDIO_RUN_CUE); g_assert_cmpuint(a->run.auto_next_frame, ==, 33600);
    g_assert_cmpuint(a->run.overlap_frames, ==, 14400); g_assert_cmpuint(b->run.fade_in_frames, ==, 14400); g_assert_cmpint(b->planned_utc_us - a->planned_utc_us, ==, 600000);
    JsonNode *fresh = copy_node(json_node_get_object(output)); JsonObject *changed_marks = json_object_get_object_member(json_object_get_object_member(at(json_node_get_object(fresh), "tracks", 0), "detail"), "markers");
    json_object_set_int_member(changed_marks, "auto_next_ms", 701); g_assert_null(radios_automation_calendar_merge_selection(json_node_get_object(fresh), json_node_get_object(output), &error)); g_assert_nonnull(error); g_clear_error(&error);
    radios_playout_plan_free(plan); json_node_free(fresh); json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}

static void test_named_global_end_chooses_fitting_source(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    json_object_set_int_member(event, "end_minute", 481);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    for (guint i = 0; i < 2; ++i)
        json_object_set_int_member(json_object_get_object_member(json_object_get_object_member(at(at(prepared, "sources", 0), "tracks", i), "detail"), "version"), "duration_ms", 90000);
    JsonNode *output = compiled(json_node_get_object(resolved), prepared, 20);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(output), "tracks")), ==, 20);
    for (guint i = 0; i < 20; ++i) expect_track(output, i, 3 + i % 2);
    GError *error = NULL; RadioSPlayoutRequest request = request_new();
    RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(output), &request, &error); g_assert_no_error(error); g_assert_nonnull(plan);
    RadioSPlayoutEntry *first = g_ptr_array_index(plan->entries, 0), *last = g_ptr_array_index(plan->entries, plan->entries->len - 1);
    g_assert_cmpint(last->planned_utc_us + 1000000 - first->planned_utc_us, ==, 20000000);
    radios_playout_plan_free(plan); json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}

static JsonObject *routing_new(void)
{
    JsonObject *routing = json_object_new(); const gchar *kinds[] = {"music", "advertisement", "jingle", "sweeper"};
    const gint64 ids[] = {1, 3, 4, 5};
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) {
        JsonArray *decks = json_array_new(); json_array_add_int_element(decks, ids[i]);
        if (!i) json_array_add_int_element(decks, 32);
        json_object_set_array_member(routing, kinds[i], decks);
    }
    return routing;
}
static JsonObject *actual_row(guint id, const gchar *kind)
{
    JsonObject *track = row(id, kind);
    json_object_set_string_member(json_object_get_object_member(json_object_get_object_member(track, "detail"), "media"), "kind", kind);
    return track;
}
static void assert_pool(JsonNode *node, guint index, gint64 id, guint size)
{
    JsonObject *track = at(json_node_get_object(node), "tracks", index);
    JsonObject *routing = json_object_get_object_member(track, "calendar_routing");
    g_assert_cmpuint(json_object_get_size(routing), ==, 1);
    JsonArray *decks = json_object_get_array_member(routing, "decks");
    g_assert_cmpuint(json_array_get_length(decks), ==, size); g_assert_cmpint(json_array_get_int_element(decks, 0), ==, id);
    if (size > 1) g_assert_cmpint(json_array_get_int_element(decks, 1), ==, 32);
}
static void test_autodj_legacy_actual_kind_and_music_other_pools(void)
{
    JsonObject *event = event_new(), *prepared = prepared_new(), *source = add_source(event, prepared, 1, 5);
    json_object_set_object_member(at(event, "rotations", 0), "autodj", routing_new());
    const gchar *kinds[] = {"music", "advertisement", "jingle", "sweeper", "announcement"};
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) json_array_add_object_element(json_object_get_array_member(source, "tracks"), actual_row(i + 1, kinds[i]));
    gchar *before = serialized(prepared); JsonNode *output = compiled(event, prepared, 5);
    const gint64 expected[] = {1, 3, 4, 5, 1};
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) { assert_pool(output, i, expected[i], expected[i] == 1 ? 2 : 1); expect_track(output, i, i + 1); }
    gchar *after = serialized(prepared); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
    json_object_set_string_member(json_object_get_object_member(at(source, "tracks", 1), "metadata"), "kind", "music");
    GError *error = NULL; g_assert_null(radios_automation_calendar_prepared_programme(event, prepared, station, "named-fixed", 5, &error));
    g_assert_nonnull(error); g_clear_error(&error); json_node_free(output); json_object_unref(event); json_object_unref(prepared);
}
static void test_autodj_named_override_disable_and_phase_two_kind_fence(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    json_array_remove_element(json_object_get_array_member(definition, "sources"), 1);
    JsonObject *source = at(definition, "sources", 0); json_object_set_object_member(source, "autodj", routing_new());
    JsonObject *override = json_object_new(); json_object_set_int_member(override, "artist_gap_minutes", 0); json_object_set_object_member(source, "settings", override);
    JsonNode *resolved = resolve(calendar, event); json_object_remove_member(source, "settings");
    JsonNode *again = resolve(calendar, event); g_assert_true(json_node_equal(json_object_get_member(at(json_node_get_object(resolved), "rotations", 0), "autodj"), json_object_get_member(at(json_node_get_object(again), "rotations", 0), "autodj")));
    JsonObject *prepared = prepared_for(json_node_get_object(again)), *prepared_source = at(prepared, "sources", 0);
    JsonArray *rows = json_array_new(); json_array_add_object_element(rows, actual_row(1, "music")); json_array_add_object_element(rows, actual_row(2, "jingle")); json_object_set_array_member(prepared_source, "tracks", rows);
    JsonNode *selection = compiled(json_node_get_object(again), prepared, 4), *fresh = copy_node(json_node_get_object(selection));
    JsonObject *verified = json_node_get_object(fresh), *chosen = json_node_get_object(selection);
    for (guint i = 0; i < 4; ++i) json_object_remove_member(at(verified, "tracks", i), "calendar_routing");
    gchar *before = serialized(verified); GError *error = NULL;
    JsonNode *merged = radios_automation_calendar_merge_selection(verified, chosen, &error); g_assert_no_error(error); g_assert_nonnull(merged);
    for (guint i = 0; i < 4; ++i) {
        assert_pool(merged, i, i % 2 ? 4 : 1, i % 2 ? 1 : 2);
        const gchar *provenance[] = {"calendar_source_id", "calendar_group_id", "calendar_rotation_id"};
        for (guint j = 0; j < G_N_ELEMENTS(provenance); ++j)
            g_assert_cmpstr(json_object_get_string_member(at(json_node_get_object(merged), "tracks", i), provenance[j]), ==, json_object_get_string_member(at(chosen, "tracks", i), provenance[j]));
    }
    gchar *after = serialized(verified); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
    for (guint corrupt = 0; corrupt < 6; ++corrupt) {
        JsonNode *bad = copy_node(chosen); JsonObject *track = at(json_node_get_object(bad), "tracks", 0);
        JsonObject *hint = json_object_get_object_member(track, "calendar_routing"); JsonArray *decks = json_array_new();
        if (corrupt == 0) { json_array_add_int_element(decks, 0); json_object_set_array_member(hint, "decks", decks); }
        else if (corrupt == 1) { json_array_add_int_element(decks, 33); json_object_set_array_member(hint, "decks", decks); }
        else if (corrupt == 2) { json_array_add_int_element(decks, 1); json_array_add_int_element(decks, 1); json_object_set_array_member(hint, "decks", decks); }
        else if (corrupt == 3) { json_array_add_boolean_element(decks, TRUE); json_object_set_array_member(hint, "decks", decks); }
        else {
            json_array_unref(decks);
            if (corrupt == 4) json_object_set_int_member(hint, "extra", 1);
            else {
                json_object_set_string_member(json_object_get_object_member(json_object_get_object_member(track, "detail"), "media"), "kind", "jingle");
                json_object_set_string_member(json_object_get_object_member(track, "metadata"), "kind", "jingle");
            }
        }
        g_assert_null(radios_automation_calendar_merge_selection(verified, json_node_get_object(bad), &error)); g_assert_nonnull(error); g_clear_error(&error); json_node_free(bad);
    }
    json_node_free(merged); json_node_free(fresh); json_node_free(selection); json_object_unref(prepared); json_node_free(again); json_node_free(resolved); json_object_unref(calendar);
}
static void test_autodj_source_pool_bounds_and_types(void)
{
    for (guint corrupt = 0; corrupt < 9; ++corrupt) {
        JsonObject *event = event_new(), *prepared = prepared_new(), *source = add_source(event, prepared, 1, 1);
        json_array_add_object_element(json_object_get_array_member(source, "tracks"), actual_row(1, "music"));
        JsonObject *routing = routing_new(); json_object_set_object_member(at(event, "rotations", 0), "autodj", routing);
        JsonArray *decks = json_array_new();
        if (corrupt <= 4) {
            if (corrupt == 0) json_array_add_int_element(decks, 0);
            if (corrupt == 1) json_array_add_int_element(decks, 33);
            if (corrupt == 2) { json_array_add_int_element(decks, 1); json_array_add_int_element(decks, 1); }
            if (corrupt == 3) json_array_add_double_element(decks, 1.0);
            json_object_set_array_member(routing, "music", decks);
        } else {
            json_array_unref(decks);
            if (corrupt == 5) json_object_remove_member(routing, "jingle");
            if (corrupt == 6) json_object_set_int_member(routing, "extra", 1);
            if (corrupt == 7) json_object_set_boolean_member(routing, "music", TRUE);
            if (corrupt == 8) json_object_set_null_member(at(event, "rotations", 0), "autodj");
        }
        GError *error = NULL; g_assert_null(radios_automation_calendar_prepared_programme(event, prepared, station, "pool-boundaries", 1, &error));
        g_assert_nonnull(error); g_clear_error(&error); json_object_unref(event); json_object_unref(prepared);
    }
}
static JsonArray *linear_choices(void)
{
    JsonArray *choices = json_array_new();
    for (guint mode = 0; mode < 4; ++mode) {
        JsonObject *choice = json_object_new(); json_object_set_int_member(choice, "mode", mode);
        json_object_set_int_member(choice, "weight", mode == 2 ? 0 : mode == 1 ? 95 : 1);
        json_object_set_int_member(choice, "overlap_ms", mode ? 100 : 0);
        if (mode == 1) json_object_set_string_member(choice, "effect", "lowpass");
        json_array_add_object_element(choices, choice);
    }
    return choices;
}
static void test_transition_linear_enabled_cycle_effect_and_local_beatmatch_hint(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition), *settings = json_object_get_object_member(definition, "settings");
    json_array_remove_element(json_object_get_array_member(definition, "sources"), 1);
    json_object_set_string_member(settings, "transition_selection_mode", "linear"); json_object_set_array_member(settings, "transitions", linear_choices());
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    JsonNode *output = compiled(json_node_get_object(resolved), prepared, 12), *again = compiled(json_node_get_object(resolved), prepared, 12);
    for (guint i = 0; i < 12; ++i) {
        JsonObject *hint = json_object_get_object_member(at(json_node_get_object(output), "tracks", i), "calendar_transition");
        JsonObject *same = json_object_get_object_member(at(json_node_get_object(again), "tracks", i), "calendar_transition");
        g_assert_true(json_node_equal(json_object_get_member(at(json_node_get_object(output), "tracks", i), "calendar_transition"), json_object_get_member(at(json_node_get_object(again), "tracks", i), "calendar_transition")));
        g_assert_cmpint(json_object_get_int_member(hint, "mode"), ==, i % 3 ? 1 : 0);
        g_assert_cmpint(json_object_get_int_member(hint, "mode"), ==, json_object_get_int_member(same, "mode"));
        if (i % 3 == 1) { g_assert_cmpstr(json_object_get_string_member(hint, "effect"), ==, "lowpass"); g_assert_false(json_object_has_member(hint, "beat_sync")); }
        else if (i % 3 == 2) { g_assert_true(json_object_get_boolean_member(hint, "beat_sync")); g_assert_false(json_object_has_member(hint, "effect")); }
        else g_assert_cmpuint(json_object_get_size(hint), ==, 2);
    }
    JsonNode *fresh = copy_node(json_node_get_object(output)); GError *error = NULL;
    JsonNode *merged = radios_automation_calendar_merge_selection(json_node_get_object(fresh), json_node_get_object(output), &error); g_assert_no_error(error); g_assert_nonnull(merged);
    for (guint corrupt = 0; corrupt < 5; ++corrupt) {
        JsonNode *bad = copy_node(json_node_get_object(output)); JsonObject *hint = json_object_get_object_member(at(json_node_get_object(bad), "tracks", 2), "calendar_transition");
        if (!corrupt) json_object_set_int_member(hint, "mode", 3);
        else if (corrupt == 1) json_object_set_string_member(hint, "beat_sync", "true");
        else if (corrupt == 2) json_object_set_int_member(hint, "overlap_ms", 0);
        else if (corrupt == 3) json_object_set_string_member(hint, "effect", "reverb");
        else json_object_set_int_member(hint, "extra", 1);
        g_assert_null(radios_automation_calendar_merge_selection(json_node_get_object(fresh), json_node_get_object(bad), &error)); g_assert_nonnull(error); g_clear_error(&error); json_node_free(bad);
    }
    json_node_free(merged); json_node_free(fresh); json_node_free(output); json_node_free(again); json_object_unref(prepared); json_node_free(resolved); json_object_unref(calendar);
}
static void test_transition_sparse_linear_override_does_not_change_track_shuffle(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition), *source = at(definition, "sources", 0);
    json_array_remove_element(json_object_get_array_member(definition, "sources"), 1);
    json_object_set_boolean_member(source, "track_random", TRUE);
    JsonObject *settings = json_object_get_object_member(definition, "settings"); json_object_set_array_member(settings, "transitions", linear_choices());
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved)); JsonNode *weighted = compiled(json_node_get_object(resolved), prepared, 40);
    JsonObject *override = json_object_new(); json_object_set_string_member(override, "transition_selection_mode", "linear"); json_object_set_object_member(source, "settings", override);
    JsonNode *linear_event = resolve(calendar, event); JsonNode *linear = compiled(json_node_get_object(linear_event), prepared, 40);
    for (guint i = 0; i < 40; ++i) {
        JsonObject *a = json_object_get_object_member(json_object_get_object_member(at(json_node_get_object(linear), "tracks", i), "detail"), "media");
        JsonObject *b = json_object_get_object_member(json_object_get_object_member(at(json_node_get_object(weighted), "tracks", i), "detail"), "media");
        g_assert_cmpstr(json_object_get_string_member(a, "_id"), ==, json_object_get_string_member(b, "_id"));
        JsonObject *hint = json_object_get_object_member(at(json_node_get_object(linear), "tracks", i), "calendar_transition");
        g_assert_cmpint(json_object_get_int_member(hint, "mode"), ==, i % 3 ? 1 : 0);
    }
    json_node_free(linear); json_node_free(linear_event); json_node_free(weighted); json_object_unref(prepared); json_node_free(resolved); json_object_unref(calendar);
}
static void test_transition_optional_rule_validation_and_legacy_default(void)
{
    for (guint corrupt = 0; corrupt < 8; ++corrupt) {
        JsonObject *event, *definition, *calendar = calendar_new(&event, &definition), *settings = json_object_get_object_member(definition, "settings");
        set_transition(settings, 3, 100); JsonObject *choice = at(settings, "transitions", 0);
        if (corrupt == 0) json_object_set_int_member(choice, "overlap_ms", 0);
        else if (corrupt == 1) json_object_set_int_member(choice, "mode", 4);
        else if (corrupt == 2) json_object_set_string_member(choice, "effect", "reverb");
        else if (corrupt == 3) json_object_set_boolean_member(choice, "effect", TRUE);
        else if (corrupt == 4) { json_object_set_int_member(choice, "mode", 0); json_object_set_int_member(choice, "overlap_ms", 0); json_object_set_string_member(choice, "effect", "lowpass"); }
        else if (corrupt == 5) json_object_set_boolean_member(choice, "beat_sync", TRUE);
        else if (corrupt == 6) json_object_set_boolean_member(settings, "transition_selection_mode", TRUE);
        else { json_object_set_string_member(settings, "transition_selection_mode", "linear"); json_object_remove_member(settings, "album_gap_minutes"); }
        GError *error = NULL; g_assert_null(radios_automation_calendar_resolve_event(calendar, event, &error)); g_assert_nonnull(error); g_clear_error(&error); json_object_unref(calendar);
    }
}

static gint64 utc_text(const gchar *text)
{ GDateTime *date = g_date_time_new_from_iso8601(text, NULL); g_assert_nonnull(date); gint64 utc = g_date_time_to_unix(date) * G_USEC_PER_SEC; g_date_time_unref(date); return utc; }
static void test_constraints_authenticated_bounds_and_turns(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition), *source = at(definition, "sources", 0);
    json_array_remove_element(json_object_get_array_member(definition, "sources"), 1); json_object_set_int_member(source, "count", 2);
    json_object_set_int_member(source, "start_minute", 470); json_object_set_int_member(source, "end_minute", 590);
    JsonObject *settings = json_object_get_object_member(definition, "settings");
    json_object_set_int_member(settings, "artist_gap_minutes", 3); json_object_set_int_member(settings, "album_gap_minutes", 4); json_object_set_int_member(settings, "track_gap_minutes", 2);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    JsonObject *first = at(at(prepared, "sources", 0), "tracks", 0), *second = at(at(prepared, "sources", 0), "tracks", 1);
    json_object_set_string_member(json_object_get_object_member(json_object_get_object_member(first, "detail"), "media"), "artist", "Birinci sanatçı");
    json_object_set_string_member(json_object_get_object_member(json_object_get_object_member(second, "detail"), "media"), "artist", "İkinci sanatçı");
    json_object_set_string_member(json_object_get_object_member(json_object_get_object_member(first, "detail"), "media"), "album", "Birinci albüm");
    json_object_set_string_member(json_object_get_object_member(json_object_get_object_member(second, "detail"), "media"), "album", "İkinci albüm");
    JsonNode *output = compiled(json_node_get_object(resolved), prepared, 2);
    for (guint i = 0; i < 2; ++i) {
        JsonObject *hint = json_object_get_object_member(at(json_node_get_object(output), "tracks", i), "calendar_constraints");
        g_assert_cmpuint(json_object_get_size(hint), ==, 7);
        g_assert_cmpint(json_object_get_int_member(hint, "earliest_utc_us"), ==, utc_text("2026-10-09T08:00:00Z"));
        g_assert_cmpint(json_object_get_int_member(hint, "latest_end_utc_us"), ==, utc_text("2026-10-09T09:50:00Z"));
        g_assert_cmpint(json_object_get_int_member(hint, "track_gap_ms"), ==, 120000); g_assert_cmpint(json_object_get_int_member(hint, "artist_gap_ms"), ==, 180000); g_assert_cmpint(json_object_get_int_member(hint, "album_gap_ms"), ==, 240000);
        g_assert_cmpint(json_object_get_int_member(hint, "source_interval_ms"), ==, 0); g_assert_cmpint(json_object_get_boolean_member(hint, "source_turn_start"), ==, i == 0);
    }
    json_node_free(output); json_object_unref(prepared); json_node_free(resolved);
    json_object_set_int_member(settings, "track_gap_minutes", 0); json_object_set_int_member(settings, "artist_gap_minutes", 0); json_object_set_int_member(settings, "album_gap_minutes", 0);
    resolved = resolve(calendar, event); prepared = prepared_for(json_node_get_object(resolved)); output = compiled(json_node_get_object(resolved), prepared, 6);
    for (guint i = 0; i < 6; ++i) g_assert_cmpint(json_object_get_boolean_member(json_object_get_object_member(at(json_node_get_object(output), "tracks", i), "calendar_constraints"), "source_turn_start"), ==, i % 2 == 0);
    json_node_free(output); json_object_unref(prepared); json_node_free(resolved); json_object_unref(calendar);
    JsonObject *legacy = event_new(); prepared = prepared_new(); source = add_source(legacy, prepared, 1, 1); json_array_add_object_element(json_object_get_array_member(source, "tracks"), actual_row(1, "music"));
    output = compiled(legacy, prepared, 1); g_assert_false(json_object_has_member(at(json_node_get_object(output), "tracks", 0), "calendar_constraints")); json_node_free(output);
    json_object_set_object_member(at(legacy, "rotations", 0), "autodj", routing_new()); output = compiled(legacy, prepared, 1);
    JsonObject *hint = json_object_get_object_member(at(json_node_get_object(output), "tracks", 0), "calendar_constraints");
    g_assert_cmpint(json_object_get_int_member(hint, "latest_end_utc_us"), ==, utc_text("2026-10-09T10:00:00Z")); g_assert_cmpint(json_object_get_int_member(hint, "track_gap_ms"), ==, 0);
    json_node_free(output); json_object_unref(prepared); json_object_unref(legacy);
}
static void test_constraints_phase_two_typed_preservation(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition); JsonObject *source = at(definition, "sources", 0);
    json_object_set_int_member(source, "interval_minutes", 2);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved)); JsonNode *output = compiled(json_node_get_object(resolved), prepared, 2), *fresh = copy_node(json_node_get_object(output));
    for (guint i = 0; i < 2; ++i) json_object_remove_member(at(json_node_get_object(fresh), "tracks", i), "calendar_constraints");
    GError *error = NULL; JsonNode *merged = radios_automation_calendar_merge_selection(json_node_get_object(fresh), json_node_get_object(output), &error); g_assert_no_error(error); g_assert_nonnull(merged);
    g_assert_true(json_node_equal(json_object_get_member(at(json_node_get_object(merged), "tracks", 0), "calendar_constraints"), json_object_get_member(at(json_node_get_object(output), "tracks", 0), "calendar_constraints")));
    JsonObject *original = json_object_get_object_member(at(json_node_get_object(merged), "tracks", 0), "calendar_constraints"); g_assert_cmpint(json_object_get_int_member(original, "source_interval_ms"), ==, 120000);
    for (guint corrupt = 0; corrupt < 8; ++corrupt) {
        JsonNode *bad = copy_node(json_node_get_object(output)); JsonObject *track = at(json_node_get_object(bad), "tracks", 0), *hint = json_object_get_object_member(track, "calendar_constraints");
        if (corrupt == 0) json_object_set_boolean_member(hint, "earliest_utc_us", TRUE);
        else if (corrupt == 1) json_object_set_int_member(hint, "latest_end_utc_us", json_object_get_int_member(hint, "earliest_utc_us"));
        else if (corrupt == 2) json_object_set_int_member(hint, "track_gap_ms", -1);
        else if (corrupt == 3) json_object_set_int_member(hint, "source_interval_ms", 604800001);
        else if (corrupt == 4) json_object_set_int_member(hint, "source_turn_start", 1);
        else if (corrupt == 5) json_object_set_int_member(hint, "extra", 0);
        else if (corrupt == 6) json_object_remove_member(hint, "album_gap_ms");
        else json_object_remove_member(track, "calendar_source_id");
        g_assert_null(radios_automation_calendar_merge_selection(json_node_get_object(fresh), json_node_get_object(bad), &error)); g_assert_nonnull(error); g_clear_error(&error); json_node_free(bad);
    }
    json_node_free(merged); json_node_free(fresh); json_node_free(output); json_object_unref(prepared); json_node_free(resolved); json_object_unref(calendar);
}

static void test_transition_lowpass_zero_overlap_all_weights_rejected(void)
{
    for (guint mode = 1; mode <= 2; ++mode) for (guint weight = 0; weight <= 100; weight += 100) {
        JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
        JsonObject *settings = json_object_get_object_member(definition, "settings"); JsonArray *choices = json_array_new();
        JsonObject *eof = json_object_new(); json_object_set_int_member(eof, "mode", 0); json_object_set_int_member(eof, "weight", 100); json_object_set_int_member(eof, "overlap_ms", 0); json_array_add_object_element(choices, eof);
        JsonObject *lowpass = json_object_new(); json_object_set_int_member(lowpass, "mode", mode); json_object_set_int_member(lowpass, "weight", weight); json_object_set_int_member(lowpass, "overlap_ms", 0); json_object_set_string_member(lowpass, "effect", "lowpass"); json_array_add_object_element(choices, lowpass); json_object_set_array_member(settings, "transitions", choices);
        GError *error = NULL; g_assert_null(radios_automation_calendar_resolve_event(calendar, event, &error)); g_assert_nonnull(error); g_clear_error(&error);
        json_object_set_int_member(lowpass, "overlap_ms", 100); JsonNode *resolved = resolve(calendar, event);
        JsonObject *prepared = prepared_for(json_node_get_object(resolved)); JsonNode *output = compiled(json_node_get_object(resolved), prepared, 2), *fresh = copy_node(json_node_get_object(output));
        JsonObject *hint = json_object_new(); json_object_set_int_member(hint, "mode", mode); json_object_set_int_member(hint, "overlap_ms", 0); json_object_set_string_member(hint, "effect", "lowpass"); json_object_set_object_member(at(json_node_get_object(output), "tracks", 0), "calendar_transition", hint);
        g_assert_null(radios_automation_calendar_merge_selection(json_node_get_object(fresh), json_node_get_object(output), &error)); g_assert_nonnull(error); g_clear_error(&error);
        json_object_set_int_member(hint, "overlap_ms", 100); JsonNode *merged = radios_automation_calendar_merge_selection(json_node_get_object(fresh), json_node_get_object(output), &error); g_assert_no_error(error); g_assert_nonnull(merged);
        json_node_free(merged); json_node_free(fresh); json_node_free(output); json_object_unref(prepared); json_node_free(resolved); json_object_unref(calendar);
    }
}

static JsonObject *song_rule(guint entry, guint weight, guint order, guint frequency, guint cap)
{
    JsonObject *object = json_object_new(); gchar *id = uuid(entry);
    json_object_set_string_member(object, "entry_id", id); g_free(id);
    json_object_set_int_member(object, "weight", weight); json_object_set_int_member(object, "order", order);
    json_object_set_int_member(object, "frequency", frequency); json_object_set_int_member(object, "max_plays", cap);
    json_object_set_int_member(object, "start_minute", 0); json_object_set_int_member(object, "end_minute", 1440);
    json_object_set_int_member(object, "interval_minutes", 0); return object;
}
static JsonArray *song_rules(JsonObject *source, const gchar *mode)
{
    JsonArray *rules = json_array_new(); json_object_set_array_member(source, "track_rules", rules);
    if (mode) json_object_set_string_member(source, "track_selection_mode", mode);
    return rules;
}
static void test_song_linear_order_zero_caps_and_input_copy(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    json_array_remove_element(json_object_get_array_member(definition, "sources"), 1);
    JsonObject *source = at(definition, "sources", 0); JsonArray *rules = song_rules(source, "linear");
    json_object_set_boolean_member(source, "track_random", TRUE);
    json_array_add_object_element(rules, song_rule(1, 100, 20, 1, 2));
    json_array_add_object_element(rules, song_rule(2, 100, 10, 1, 1));
    json_array_add_object_element(rules, song_rule(5, 0, 0, 1, 0));
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    json_array_add_object_element(json_object_get_array_member(at(prepared, "sources", 0), "tracks"), row(5, "music"));
    gchar *before = serialized(prepared); JsonNode *output = compiled(json_node_get_object(resolved), prepared, 20);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(output), "tracks")), ==, 3);
    const guint expected[] = {2,1,1}; for (guint i = 0; i < 3; ++i) expect_track(output, i, expected[i]);
    g_assert_cmpstr(json_object_get_string_member(at(json_node_get_object(output), "diagnostics", 0), "reason"), ==, "calendar_no_legal_candidate");
    gchar *after = serialized(prepared); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
static JsonObject *kind_choice(gint mode, gint overlap)
{ JsonObject *choice = json_object_new(); json_object_set_int_member(choice, "mode", mode); json_object_set_int_member(choice, "overlap_ms", overlap); return choice; }
static JsonArray *players(guint first, guint second)
{ JsonArray *array = json_array_new(); json_array_add_int_element(array, first); if (second) json_array_add_int_element(array, second); return array; }
/* The rotation's AutoDJ policy picks each boundary by content kind and routes
 * every source to its kind's channels; a shared playlist never repeats media
 * back to back. */
static void test_autodj_kind_policy_channels_and_no_repeat(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    JsonObject *settings = json_object_get_object_member(definition, "settings"), *policy = json_object_new(), *channels = json_object_new();
    json_object_set_object_member(policy, "music", kind_choice(3, 8000)); json_object_set_object_member(policy, "advertisement", kind_choice(0, 0));
    json_object_set_object_member(policy, "jingle", kind_choice(1, 1000)); json_object_set_object_member(policy, "sweeper", kind_choice(2, 500));
    json_object_set_array_member(channels, "music", players(1, 2)); json_object_set_array_member(channels, "advertisement", players(3, 0));
    json_object_set_array_member(channels, "jingle", players(3, 0)); json_object_set_array_member(channels, "sweeper", players(3, 0));
    json_object_set_object_member(settings, "kind_transitions", policy); json_object_set_object_member(settings, "autodj_players", channels);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    /* The second source shares no music: it supplies jingles. */
    JsonArray *jingles = json_object_get_array_member(at(prepared, "sources", 1), "tracks");
    for (guint j = 0; j < json_array_get_length(jingles); ++j)
        json_object_set_string_member(json_object_get_object_member(json_array_get_object_element(jingles, j), "metadata"), "kind", "jingle");
    JsonNode *output = compiled(json_node_get_object(resolved), prepared, 12);
    JsonArray *tracks = json_object_get_array_member(json_node_get_object(output), "tracks");
    g_assert_cmpuint(json_array_get_length(tracks), >=, 4);
    for (guint i = 0; i + 1 < json_array_get_length(tracks); ++i) {
        JsonObject *track = json_array_get_object_element(tracks, i), *next = json_array_get_object_element(tracks, i + 1);
        const gchar *kind = json_object_get_string_member(json_object_get_object_member(track, "metadata"), "kind");
        const gchar *next_kind = json_object_get_string_member(json_object_get_object_member(next, "metadata"), "kind");
        JsonObject *hint = json_object_get_object_member(track, "calendar_transition");
        JsonArray *decks = json_object_get_array_member(json_object_get_object_member(track, "calendar_routing"), "decks");
        if (g_str_equal(kind, "music")) { g_assert_cmpuint(json_array_get_length(decks), ==, 2); g_assert_cmpint(json_array_get_int_element(decks, 1), ==, 2); }
        else { g_assert_cmpuint(json_array_get_length(decks), ==, 1); g_assert_cmpint(json_array_get_int_element(decks, 0), ==, 3); }
        if (g_str_equal(kind, "music") && g_str_equal(next_kind, "music")) {
            g_assert_cmpint(json_object_get_int_member(hint, "mode"), ==, 1); g_assert_true(json_object_get_boolean_member(hint, "beat_sync"));
            g_assert_cmpint(json_object_get_int_member(hint, "overlap_ms"), ==, 8000);
        } else {
            g_assert_cmpint(json_object_get_int_member(hint, "mode"), ==, 1); g_assert_false(json_object_has_member(hint, "beat_sync"));
            g_assert_cmpint(json_object_get_int_member(hint, "overlap_ms"), ==, 1000);
        }
        const gchar *media = json_object_get_string_member(json_object_get_object_member(json_object_get_object_member(track, "detail"), "media"), "_id");
        const gchar *following = json_object_get_string_member(json_object_get_object_member(json_object_get_object_member(next, "detail"), "media"), "_id");
        g_assert_cmpstr(media, !=, following);
    }
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
/* SYNC beatmatch with sync_percent: 0 never beat-syncs, 100 always does, and
 * a share draws per song pair, reproducibly across preparations. */
static void test_autodj_kind_sync_percent_share(void)
{
    const gint percents[] = {0, 100, 50};
    for (guint p = 0; p < G_N_ELEMENTS(percents); ++p) {
        JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
        JsonObject *settings = json_object_get_object_member(definition, "settings"), *policy = json_object_new(), *channels = json_object_new();
        JsonObject *music = kind_choice(3, 8000); json_object_set_int_member(music, "sync_percent", percents[p]);
        json_object_set_object_member(policy, "music", music); json_object_set_object_member(policy, "advertisement", kind_choice(0, 0));
        json_object_set_object_member(policy, "jingle", kind_choice(1, 1000)); json_object_set_object_member(policy, "sweeper", kind_choice(2, 500));
        json_object_set_array_member(channels, "music", players(1, 2)); json_object_set_array_member(channels, "advertisement", players(3, 0));
        json_object_set_array_member(channels, "jingle", players(3, 0)); json_object_set_array_member(channels, "sweeper", players(3, 0));
        json_object_set_object_member(settings, "kind_transitions", policy); json_object_set_object_member(settings, "autodj_players", channels);
        JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
        JsonNode *output = compiled(json_node_get_object(resolved), prepared, 12), *again = compiled(json_node_get_object(resolved), prepared, 12);
        JsonArray *tracks = json_object_get_array_member(json_node_get_object(output), "tracks"), *repeated = json_object_get_array_member(json_node_get_object(again), "tracks");
        guint music_pairs = 0, synced = 0;
        g_assert_cmpuint(json_array_get_length(tracks), >=, 4); g_assert_cmpuint(json_array_get_length(tracks), ==, json_array_get_length(repeated));
        for (guint i = 0; i + 1 < json_array_get_length(tracks); ++i) {
            JsonObject *track = json_array_get_object_element(tracks, i), *next = json_array_get_object_element(tracks, i + 1);
            const gchar *kind = json_object_get_string_member(json_object_get_object_member(track, "metadata"), "kind");
            const gchar *next_kind = json_object_get_string_member(json_object_get_object_member(next, "metadata"), "kind");
            JsonObject *hint = json_object_get_object_member(track, "calendar_transition");
            JsonObject *repeat = json_object_get_object_member(json_array_get_object_element(repeated, i), "calendar_transition");
            g_assert_cmpint(json_object_get_int_member(hint, "mode"), ==, 1);
            g_assert_cmpint(json_object_has_member(hint, "beat_sync"), ==, json_object_has_member(repeat, "beat_sync"));
            if (!g_str_equal(kind, "music") || !g_str_equal(next_kind, "music")) continue;
            ++music_pairs; synced += json_object_has_member(hint, "beat_sync");
            g_assert_cmpint(json_object_get_int_member(hint, "overlap_ms"), ==, 8000);
        }
        g_assert_cmpuint(music_pairs, >, 0);
        if (percents[p] == 0) g_assert_cmpuint(synced, ==, 0);
        if (percents[p] == 100) g_assert_cmpuint(synced, ==, music_pairs);
        json_node_free(output); json_node_free(again); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
    }
}
/* Sözleşme 18.10 (C8): settings.transitions entries with "kind" give that
 * content kind its own share per transition type or designed preset; the
 * rest (the remainder) is the rotation's kind-less distribution. */
static JsonArray *presets_enable(JsonObject *definition, const gchar *rotation, gint weekday);
static const gchar *hint_preset(JsonArray *tracks, guint index);
static JsonObject *kind_entry(const gchar *kind, gint mode, gint weight, gint overlap)
{
    JsonObject *entry = json_object_new(); json_object_set_string_member(entry, "kind", kind);
    json_object_set_int_member(entry, "mode", mode); json_object_set_int_member(entry, "weight", weight);
    json_object_set_int_member(entry, "overlap_ms", overlap); return entry;
}
static JsonObject *kind_preset_entry(const gchar *kind, const gchar *preset_id, gint weight)
{
    JsonObject *entry = kind_entry(kind, 1, weight, 0); json_object_set_string_member(entry, "preset_id", preset_id); return entry;
}
static void transitions_add(JsonObject *definition, JsonObject *entry)
{ json_array_add_object_element(json_object_get_array_member(json_object_get_object_member(definition, "settings"), "transitions"), entry); }
static void jingle_second_source(JsonObject *prepared)
{
    JsonArray *jingles = json_object_get_array_member(at(prepared, "sources", 1), "tracks");
    for (guint j = 0; j < json_array_get_length(jingles); ++j)
        json_object_set_string_member(json_object_get_object_member(json_array_get_object_element(jingles, j), "metadata"), "kind", "jingle");
}
static const gchar *entry_kind(JsonArray *tracks, guint index)
{ return json_object_get_string_member(json_object_get_object_member(json_array_get_object_element(tracks, index), "metadata"), "kind"); }
static void test_kind_entries_share_remainder_and_legacy(void)
{
    /* Music entries replace the old music kind rule; jingle boundaries keep
     * the old jingle rule while jingle has no entries of its own. */
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    JsonObject *settings = json_object_get_object_member(definition, "settings"), *policy = json_object_new();
    json_object_set_object_member(policy, "music", kind_choice(2, 500)); json_object_set_object_member(policy, "advertisement", kind_choice(0, 0));
    json_object_set_object_member(policy, "jingle", kind_choice(1, 1000)); json_object_set_object_member(policy, "sweeper", kind_choice(2, 500));
    json_object_set_object_member(settings, "kind_transitions", policy);
    transitions_add(definition, kind_entry("music", 1, 100, 4000));
    json_object_set_int_member(at(definition, "sources", 0), "count", 2); /* music into music as well as music ↔ jingle */
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved)); jingle_second_source(prepared);
    JsonNode *output = compiled(json_node_get_object(resolved), prepared, 12);
    JsonArray *tracks = json_object_get_array_member(json_node_get_object(output), "tracks"); guint music_pairs = 0;
    g_assert_cmpuint(json_array_get_length(tracks), >=, 4);
    for (guint i = 0; i + 1 < json_array_get_length(tracks); ++i) {
        JsonObject *hint = json_object_get_object_member(json_array_get_object_element(tracks, i), "calendar_transition");
        gboolean music = g_str_equal(entry_kind(tracks, i), "music") && g_str_equal(entry_kind(tracks, i + 1), "music");
        music_pairs += music;
        g_assert_cmpint(json_object_get_int_member(hint, "mode"), ==, 1);
        g_assert_cmpint(json_object_get_int_member(hint, "overlap_ms"), ==, music ? 4000 : 1000);
    }
    g_assert_cmpuint(music_pairs, >, 0);
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
    /* A 50 % SYNC share: each music boundary is either the share (SYNC
     * beatmatch, 8 s) or the remainder (the rotation's own EOF), the same
     * on every preparation. */
    calendar = calendar_new(&event, &definition); transitions_add(definition, kind_entry("music", 3, 50, 8000));
    resolved = resolve(calendar, event); prepared = prepared_for(json_node_get_object(resolved));
    output = compiled(json_node_get_object(resolved), prepared, 12); JsonNode *again = compiled(json_node_get_object(resolved), prepared, 12);
    tracks = json_object_get_array_member(json_node_get_object(output), "tracks"); JsonArray *repeated = json_object_get_array_member(json_node_get_object(again), "tracks");
    guint shared = 0, rest = 0; g_assert_cmpuint(json_array_get_length(tracks), ==, json_array_get_length(repeated));
    for (guint i = 0; i + 1 < json_array_get_length(tracks); ++i) {
        JsonObject *hint = json_object_get_object_member(json_array_get_object_element(tracks, i), "calendar_transition");
        JsonObject *repeat = json_object_get_object_member(json_array_get_object_element(repeated, i), "calendar_transition");
        g_assert_cmpint(json_object_get_int_member(hint, "mode"), ==, json_object_get_int_member(repeat, "mode"));
        if (json_object_has_member(hint, "beat_sync")) {
            ++shared; g_assert_cmpint(json_object_get_int_member(hint, "mode"), ==, 1); g_assert_cmpint(json_object_get_int_member(hint, "overlap_ms"), ==, 8000);
        } else { ++rest; g_assert_cmpint(json_object_get_int_member(hint, "mode"), ==, 0); g_assert_cmpint(json_object_get_int_member(hint, "overlap_ms"), ==, 0); }
    }
    g_assert_cmpuint(shared, >, 0); g_assert_cmpuint(rest, >, 0);
    json_node_free(output); json_node_free(again); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
static void test_kind_entries_presets_schedule_and_automation_off(void)
{
    /* A music preset entry takes every music boundary over the former
     * kinds.music presets; with the AutoDJ automation off its share falls to
     * the rotation's own distribution; a matching time slot still wins. */
    for (guint step = 0; step < 3; ++step) {
        JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
        JsonArray *presets = presets_enable(definition, "sequential", step == 2 ? 5 : 0);
        const gchar *chosen = json_object_get_string_member(json_array_get_object_element(presets, 3), "id");
        const gchar *name = json_object_get_string_member(json_array_get_object_element(presets, 3), "name");
        transitions_add(definition, kind_preset_entry("music", chosen, 100));
        if (step == 1) json_object_set_boolean_member(json_object_get_object_member(json_object_get_object_member(definition, "settings"), "autodj_automation"), "enabled", FALSE);
        JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
        JsonNode *output = compiled(json_node_get_object(resolved), prepared, 12);
        JsonArray *tracks = json_object_get_array_member(json_node_get_object(output), "tracks");
        g_assert_cmpuint(json_array_get_length(tracks), >=, 4);
        for (guint i = 0; i + 1 < json_array_get_length(tracks); ++i) {
            JsonObject *hint = json_object_get_object_member(json_array_get_object_element(tracks, i), "calendar_transition");
            if (step == 1) { g_assert_false(json_object_has_member(hint, "shape")); g_assert_cmpint(json_object_get_int_member(hint, "mode"), ==, 0); }
            else g_assert_cmpstr(hint_preset(tracks, i), ==, step == 2 ? "Ton erit" : name);
        }
        json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
    }
}
static void test_kind_entries_validation(void)
{
    struct { const gchar *entries; gboolean valid; } cases[] = {
        {"[{\"kind\":\"music\",\"mode\":1,\"weight\":60,\"overlap_ms\":3000},{\"kind\":\"music\",\"mode\":3,\"weight\":40,\"overlap_ms\":8000,\"effect\":\"lowpass\"}]", TRUE},
        {"[{\"kind\":\"music\",\"mode\":1,\"weight\":60,\"overlap_ms\":3000},{\"kind\":\"music\",\"mode\":3,\"weight\":41,\"overlap_ms\":8000}]", FALSE},
        {"[{\"kind\":\"jingle\",\"mode\":1,\"weight\":10,\"overlap_ms\":3000},{\"kind\":\"jingle\",\"mode\":1,\"weight\":10,\"overlap_ms\":2000}]", FALSE},
        {"[{\"kind\":\"sweeper\",\"mode\":0,\"weight\":10,\"overlap_ms\":500}]", FALSE},
        {"[{\"kind\":\"sweeper\",\"mode\":3,\"weight\":10,\"overlap_ms\":0}]", FALSE},
        {"[{\"kind\":\"music\",\"mode\":1,\"weight\":0,\"overlap_ms\":3000}]", FALSE},
        {"[{\"kind\":\"talk\",\"mode\":1,\"weight\":10,\"overlap_ms\":3000}]", FALSE},
        {"[{\"kind\":\"music\",\"preset_id\":\"00000000-0000-4000-8000-000000000999\",\"mode\":1,\"weight\":10,\"overlap_ms\":0}]", FALSE},
        {"[{\"kind\":\"music\",\"preset_id\":\"PRESET\",\"mode\":1,\"weight\":10,\"overlap_ms\":0}]", TRUE},
        {"[{\"kind\":\"music\",\"preset_id\":\"PRESET\",\"mode\":1,\"weight\":10,\"overlap_ms\":500}]", FALSE},
        {"[{\"kind\":\"music\",\"preset_id\":\"PRESET\",\"mode\":1,\"weight\":10,\"overlap_ms\":0},{\"kind\":\"music\",\"preset_id\":\"PRESET\",\"mode\":1,\"weight\":10,\"overlap_ms\":0}]", FALSE},
        {"[{\"preset_id\":\"PRESET\",\"mode\":1,\"weight\":10,\"overlap_ms\":0}]", FALSE},
    };
    for (guint c = 0; c < G_N_ELEMENTS(cases); ++c) {
        JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
        JsonArray *presets = presets_enable(definition, "sequential", 0);
        gchar *text = g_strdup(cases[c].entries); gchar **parts = g_strsplit(text, "PRESET", -1);
        gchar *json = g_strjoinv(json_object_get_string_member(json_array_get_object_element(presets, 3), "id"), parts);
        JsonParser *parser = json_parser_new(); g_assert_true(json_parser_load_from_data(parser, json, -1, NULL));
        JsonArray *entries = json_node_get_array(json_parser_get_root(parser));
        for (guint i = 0; i < json_array_get_length(entries); ++i) transitions_add(definition, json_object_ref(json_array_get_object_element(entries, i)));
        GError *error = NULL; JsonNode *resolved = radios_automation_calendar_resolve_event(calendar, event, &error);
        if (cases[c].valid) { g_assert_no_error(error); g_assert_nonnull(resolved); }
        else { g_assert_null(resolved); g_assert_nonnull(error); }
        g_clear_error(&error); if (resolved) json_node_free(resolved);
        g_object_unref(parser); g_free(json); g_strfreev(parts); g_free(text); json_object_unref(calendar);
    }
    /* The rotation's own distribution stays required: kind entries alone are refused. */
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    JsonArray *only = json_array_new(); json_array_add_object_element(only, kind_entry("music", 1, 100, 3000));
    json_object_set_array_member(json_object_get_object_member(definition, "settings"), "transitions", only);
    GError *error = NULL; g_assert_null(radios_automation_calendar_resolve_event(calendar, event, &error)); g_assert_nonnull(error);
    g_clear_error(&error); json_object_unref(calendar);
}
static void test_song_weighted_reproducible_relative_and_transition_independence(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    json_array_remove_element(json_object_get_array_member(definition, "sources"), 1);
    JsonArray *rules = song_rules(at(definition, "sources", 0), "weighted");
    json_array_add_object_element(rules, song_rule(1, 80, 0, 1, 0)); json_array_add_object_element(rules, song_rule(2, 20, 1, 1, 0));
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    JsonNode *a = compiled(json_node_get_object(resolved), prepared, 200), *b = compiled(json_node_get_object(resolved), prepared, 200);
    set_transition(json_object_get_object_member(at(json_node_get_object(resolved), "rotations", 0), "calendar_settings"), 1, 250);
    JsonNode *different_transition = compiled(json_node_get_object(resolved), prepared, 200);
    guint first = 0; gchar *id = uuid(10001);
    for (guint i = 0; i < 200; ++i) {
        JsonObject *am = json_object_get_object_member(json_object_get_object_member(at(json_node_get_object(a), "tracks", i), "detail"), "media");
        const gchar *actual = json_object_get_string_member(am, "_id"); first += g_str_equal(actual, id);
        for (guint j = 0; j < 2; ++j) {
            JsonNode *other = j ? different_transition : b;
            JsonObject *om = json_object_get_object_member(json_object_get_object_member(at(json_node_get_object(other), "tracks", i), "detail"), "media");
            g_assert_cmpstr(actual, ==, json_object_get_string_member(om, "_id"));
        }
    }
    g_assert_cmpuint(first, >, 120); g_assert_cmpuint(first, <, 190); g_free(id);
    json_node_free(a); json_node_free(b); json_node_free(different_transition); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
static void test_song_frequency_pinned_quota_and_empty_cycles(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    json_array_remove_element(json_object_get_array_member(definition, "sources"), 1);
    JsonObject *source = at(definition, "sources", 0); json_object_set_int_member(source, "count", 2);
    JsonArray *rules = song_rules(source, "linear");
    json_array_add_object_element(rules, song_rule(1, 100, 0, 2, 0)); json_array_add_object_element(rules, song_rule(2, 100, 1, 1, 0));
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    JsonNode *output = compiled(json_node_get_object(resolved), prepared, 6); const guint expected[] = {1,2,2,2,1,2};
    for (guint i = 0; i < 6; ++i) expect_track(output, i, expected[i]);
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared);
    json_object_set_int_member(source, "count", 1); json_object_set_int_member(source, "frequency", 100);
    json_object_set_int_member(json_array_get_object_element(rules, 0), "frequency", 99);
    json_object_set_int_member(json_array_get_object_element(rules, 1), "weight", 0);
    resolved = resolve(calendar, event); prepared = prepared_for(json_node_get_object(resolved)); output = compiled(json_node_get_object(resolved), prepared, 4);
    for (guint i = 0; i < 4; ++i) expect_track(output, i, 1);
    RadioSPlayoutRequest request = request_new(); GError *error = NULL;
    RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(output), &request, &error); g_assert_no_error(error); g_assert_nonnull(plan);
    RadioSPlayoutEntry *first = g_ptr_array_index(plan->entries, 0), *last = g_ptr_array_index(plan->entries, 3);
    g_assert_cmpint(last->planned_utc_us - first->planned_utc_us, ==, 3000000);
    radios_playout_plan_free(plan); json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
static void test_song_windows_actual_media_interval_and_phase_two(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    json_array_remove_element(json_object_get_array_member(definition, "sources"), 1);
    JsonObject *source = at(definition, "sources", 0); json_object_set_int_member(source, "start_minute", 480); json_object_set_int_member(source, "end_minute", 482);
    json_object_set_int_member(json_object_get_object_member(definition, "settings"), "track_gap_minutes", 1);
    JsonArray *rules = song_rules(source, "linear");
    for (guint i = 1; i <= 2; ++i) {
        JsonObject *policy = song_rule(i, 100, i - 1, 1, 0);
        json_object_set_int_member(policy, "start_minute", 479); json_object_set_int_member(policy, "end_minute", 481);
        json_object_set_int_member(policy, "interval_minutes", 2); json_array_add_object_element(rules, policy);
    }
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    JsonObject *one = at(at(prepared, "sources", 0), "tracks", 0), *two = at(at(prepared, "sources", 0), "tracks", 1);
    json_object_set_member(two, "detail", json_node_copy(json_object_get_member(one, "detail")));
    JsonNode *output = compiled(json_node_get_object(resolved), prepared, 10);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(output), "tracks")), ==, 1);
    JsonObject *hint = json_object_get_object_member(at(json_node_get_object(output), "tracks", 0), "calendar_constraints");
    g_assert_cmpuint(json_object_get_size(hint), ==, 7); g_assert_cmpint(json_object_get_int_member(hint, "track_gap_ms"), ==, 120000);
    g_assert_cmpint(json_object_get_int_member(hint, "earliest_utc_us"), ==, utc_text("2026-10-09T08:00:00Z"));
    g_assert_cmpint(json_object_get_int_member(hint, "latest_end_utc_us"), ==, utc_text("2026-10-09T08:01:00Z"));
    JsonNode *fresh = copy_node(json_node_get_object(output)); json_object_remove_member(at(json_node_get_object(fresh), "tracks", 0), "calendar_constraints");
    GError *error = NULL; JsonNode *merged = radios_automation_calendar_merge_selection(json_node_get_object(fresh), json_node_get_object(output), &error);
    g_assert_no_error(error); g_assert_nonnull(merged); g_assert_true(json_node_equal(json_object_get_member(at(json_node_get_object(merged), "tracks", 0), "calendar_constraints"), json_object_get_member(at(json_node_get_object(output), "tracks", 0), "calendar_constraints")));
    json_node_free(merged); json_node_free(fresh); json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
static void test_song_rule_validation_missing_and_legacy_modes(void)
{
    const gchar *fields[] = {"weight", "order", "frequency", "max_plays", "start_minute", "end_minute", "interval_minutes"};
    const gint64 invalid[] = {101, 5000, 0, 201, 1440, 0, 10081};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) for (guint boolean = 0; boolean < 2; ++boolean) {
        JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
        JsonArray *rules = song_rules(at(definition, "sources", 0), "linear"); JsonObject *policy = song_rule(1, 100, 0, 1, 0);
        if (boolean) json_object_set_boolean_member(policy, fields[i], TRUE); else json_object_set_int_member(policy, fields[i], invalid[i]);
        json_array_add_object_element(rules, policy); GError *error = NULL;
        g_assert_null(radios_automation_calendar_resolve_event(calendar, event, &error)); g_assert_nonnull(error); g_clear_error(&error); json_object_unref(calendar);
    }
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    JsonArray *rules = song_rules(at(definition, "sources", 0), NULL); json_array_add_object_element(rules, song_rule(999, 100, 0, 1, 0));
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved)); JsonNode *output = compiled(json_node_get_object(resolved), prepared, 4);
    g_assert_cmpstr(json_object_get_string_member(at(json_node_get_object(output), "diagnostics", 0), "reason"), ==, "calendar_track_rule_missing");
    const guint expected[] = {1,3,2,4}; for (guint i = 0; i < 4; ++i) expect_track(output, i, expected[i]);
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
static void test_zero_source_disabled_empty_and_transport_positive(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    json_object_set_int_member(at(definition, "sources", 0), "weight", 0);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    json_object_set_array_member(at(prepared, "sources", 0), "tracks", json_array_new());
    JsonNode *output = compiled(json_node_get_object(resolved), prepared, 4); for (guint i = 0; i < 4; ++i) expect_track(output, i, 3 + i % 2);
    GError *error = NULL; JsonNode *bridge = radios_automation_calendar_prepare_config(json_node_get_object(resolved), NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(bridge); g_assert_cmpint(json_object_get_int_member(at(json_node_get_object(bridge), "sources", 0), "weight"), ==, 1);
    g_assert_cmpint(json_object_get_int_member(at(json_node_get_object(resolved), "rotations", 0), "weight"), ==, 0);
    json_node_free(bridge); json_node_free(output); json_node_free(resolved); json_object_unref(prepared);
    json_object_set_int_member(at(definition, "sources", 1), "weight", 0);
    resolved = resolve(calendar, event); prepared = prepared_for(json_node_get_object(resolved));
    g_assert_null(radios_automation_calendar_prepared_programme(json_node_get_object(resolved), prepared, station, "named-fixed", 4, &error)); g_assert_nonnull(error); g_clear_error(&error);
    json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}

static void test_zero_source_unavailable_reasons_skip_but_preserve_fences(void)
{
    const gchar *reasons[] = {"playlist_disabled", "playlist_draft_unavailable"};
    for (guint reason = 0; reason < G_N_ELEMENTS(reasons); ++reason) {
        JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
        json_object_set_int_member(at(definition, "sources", 0), "weight", 0);
        JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
        JsonObject *unavailable = at(prepared, "sources", 0); JsonArray *why = json_array_new();
        json_array_add_string_element(why, reasons[reason]); json_object_set_array_member(unavailable, "reasons", why);
        json_object_set_array_member(unavailable, "tracks", json_array_new());
        JsonNode *output = compiled(json_node_get_object(resolved), prepared, 4);
        for (guint i = 0; i < 4; ++i) expect_track(output, i, 3 + i % 2);
        g_assert_cmpstr(json_object_get_string_member(at(json_node_get_object(output), "diagnostics", 0), "reason"), ==, "calendar_source_disabled");
        json_node_free(output); GError *error = NULL;
        json_object_set_int_member(at(json_node_get_object(resolved), "rotations", 0), "weight", 1);
        /* An enabled but unavailable playlist is skipped with a diagnostic;
         * the remaining sources keep the block playing. */
        output = compiled(json_node_get_object(resolved), prepared, 4);
        for (guint i = 0; i < 4; ++i) expect_track(output, i, 3 + i % 2);
        g_assert_cmpstr(json_object_get_string_member(at(json_node_get_object(output), "diagnostics", 0), "reason"), ==, "calendar_source_unplayable");
        json_node_free(output);
        json_object_set_int_member(at(json_node_get_object(resolved), "rotations", 0), "weight", 0);
        json_array_add_int_element(why, 1);
        g_assert_null(radios_automation_calendar_prepared_programme(json_node_get_object(resolved), prepared, station, "named-fixed", 4, &error));
        g_assert_nonnull(error); g_clear_error(&error); json_array_remove_element(why, 1);
        gchar *wrong = uuid(999); json_object_set_string_member(unavailable, "playlist_id", wrong); g_free(wrong);
        g_assert_null(radios_automation_calendar_prepared_programme(json_node_get_object(resolved), prepared, station, "named-fixed", 4, &error));
        g_assert_nonnull(error); g_clear_error(&error);
        json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
    }
    /* Direct legacy planner inputs cannot schedule an empty weight-zero source. */
    JsonObject *legacy = event_new(), *prepared = prepared_new();
    JsonObject *zero = add_source(legacy, prepared, 1, 1), *active = add_source(legacy, prepared, 2, 1);
    json_object_set_int_member(at(legacy, "rotations", 0), "weight", 0);
    JsonArray *why = json_array_new(); json_array_add_string_element(why, "playlist_disabled");
    json_object_set_array_member(zero, "reasons", why);
    json_array_add_object_element(json_object_get_array_member(active, "tracks"), row(3, "music"));
    json_array_add_object_element(json_object_get_array_member(active, "tracks"), row(4, "music"));
    JsonNode *output = compiled(legacy, prepared, 4);
    for (guint i = 0; i < 4; ++i) expect_track(output, i, 3 + i % 2);
    json_node_free(output); json_object_set_int_member(at(legacy, "rotations", 1), "weight", 0); GError *error = NULL;
    g_assert_null(radios_automation_calendar_prepared_programme(legacy, prepared, station, "named-fixed", 4, &error));
    g_assert_nonnull(error); g_clear_error(&error); json_object_unref(prepared); json_object_unref(legacy);
    /* A prepared identity cannot be duplicated to divide the per-entry cap. */
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    JsonNode *resolved = resolve(calendar, event); prepared = prepared_for(json_node_get_object(resolved));
    JsonArray *tracks = json_object_get_array_member(at(prepared, "sources", 0), "tracks");
    json_array_add_object_element(tracks, json_object_ref(json_array_get_object_element(tracks, 0)));
    g_assert_null(radios_automation_calendar_prepared_programme(json_node_get_object(resolved), prepared, station, "named-fixed", 4, &error));
    g_assert_nonnull(error); g_clear_error(&error); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}

static gint64 actual_time(const gchar *stamp)
{ GDateTime *d=g_date_time_new_from_iso8601(stamp,NULL);g_assert_nonnull(d);gint64 result=g_date_time_to_unix(d)*G_USEC_PER_SEC+g_date_time_get_microsecond(d);g_date_time_unref(d);return result; }
static void test_late_join_remaining_window_exact_epoch_and_native_plan(void)
{
    JsonObject *event,*definition,*calendar=calendar_new(&event,&definition);json_object_set_int_member(event,"end_minute",481);JsonNode *resolved=resolve(calendar,event);JsonObject *prepared=prepared_for(json_node_get_object(resolved));gint64 now=actual_time("2026-10-09T08:00:58.500123Z");GError *error=NULL;
    JsonNode *result=radios_automation_calendar_prepared_programme_at(json_node_get_object(resolved),prepared,station,"late",100,now,&error);g_assert_no_error(error);g_assert_nonnull(result);g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(result),"tracks")),==,1);JsonObject *hint=json_object_get_object_member(at(json_node_get_object(result),"tracks",0),"calendar_constraints");g_assert_cmpuint(json_object_get_size(hint),==,7);g_assert_cmpint(json_object_get_int_member(hint,"latest_end_utc_us"),==,actual_time("2026-10-09T08:01:00Z"));
    RadioSPlayoutRequest request=request_new();request.now_utc_us=now;RadioSPlayoutPlan *plan=radios_playout_plan_build(json_node_get_object(result),&request,&error);g_assert_no_error(error);g_assert_nonnull(plan);RadioSPlayoutEntry *first=g_ptr_array_index(plan->entries,0);g_assert_cmpint(first->planned_utc_us,==,now);radios_playout_plan_free(plan);json_node_free(result);
    result=compiled(json_node_get_object(resolved),prepared,100);g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(result),"tracks")),==,60);json_node_free(result);
    g_assert_null(radios_automation_calendar_prepared_programme_at(json_node_get_object(resolved),prepared,station,"late",100,actual_time("2026-10-09T08:00:59.500123Z"),&error));g_assert_error(error,G_IO_ERROR,G_IO_ERROR_WOULD_BLOCK);g_clear_error(&error);
    g_assert_null(radios_automation_calendar_prepared_programme_at(json_node_get_object(resolved),prepared,station,"late",100,actual_time("2026-10-09T08:01:00Z"),&error));g_assert_error(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA);g_clear_error(&error);
    g_assert_null(radios_automation_calendar_prepared_programme_at(NULL,prepared,station,"late",100,now,&error));g_assert_error(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA);g_clear_error(&error);
    json_object_set_int_member(prepared,"contract_version",2);g_assert_null(radios_automation_calendar_prepared_programme_at(json_node_get_object(resolved),prepared,station,"late",100,now,&error));g_assert_error(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA);g_clear_error(&error);json_node_free(resolved);json_object_unref(prepared);json_object_unref(calendar);
}
static void test_late_join_actual_history_expiry_and_legacy_hints(void)
{
    JsonObject *event,*definition,*calendar=calendar_new(&event,&definition);json_array_remove_element(json_object_get_array_member(definition,"sources"),1);json_object_set_int_member(json_object_get_object_member(definition,"settings"),"track_gap_minutes",1);JsonNode *resolved=resolve(calendar,event);JsonObject *prepared=prepared_for(json_node_get_object(resolved));json_array_remove_element(json_object_get_array_member(at(prepared,"sources",0),"tracks"),1);
    JsonObject *history=json_object_new(),*config=json_object_new(),*record=json_object_new();JsonArray *records=json_array_new();gchar *mid=uuid(10001),*vid=uuid(20001);json_object_set_string_member(record,"media_id",mid);json_object_set_string_member(record,"version_id",vid);g_free(mid);g_free(vid);json_object_set_string_member(record,"artist","Sanatçı");json_object_set_string_member(record,"album","Albüm");json_object_set_string_member(record,"started_utc","2026-10-09T08:00:00Z");json_object_set_int_member(record,"position_frames",1);json_array_add_object_element(records,record);json_object_set_array_member(config,"entries",records);json_object_set_object_member(history,"config",config);json_object_set_object_member(prepared,"history",history);GError *error=NULL;
    g_assert_null(radios_automation_calendar_prepared_programme(json_node_get_object(resolved),prepared,station,"late",2,&error));g_assert_error(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA);g_clear_error(&error);
    JsonNode *result=radios_automation_calendar_prepared_programme_at(json_node_get_object(resolved),prepared,station,"late",2,actual_time("2026-10-09T08:01:00.125Z"),&error);g_assert_no_error(error);g_assert_nonnull(result);g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(result),"tracks")),==,1);expect_track(result,0,1);json_node_free(result);json_node_free(resolved);json_object_unref(prepared);json_object_unref(calendar);
    JsonObject *legacy=event_new();prepared=prepared_new();JsonObject *p=add_source(legacy,prepared,1,1);json_array_add_object_element(json_object_get_array_member(p,"tracks"),row(1,"music"));result=radios_automation_calendar_prepared_programme_at(legacy,prepared,station,"late",1,actual_time("2026-10-09T08:01:00.125Z"),&error);g_assert_no_error(error);g_assert_nonnull(result);g_assert_cmpuint(json_object_get_size(json_object_get_object_member(at(json_node_get_object(result),"tracks",0),"calendar_constraints")),==,7);json_node_free(result);json_object_unref(legacy);json_object_unref(prepared);
}
static JsonObject *two_groups(JsonObject **event,JsonObject **first,JsonObject **second)
{
    JsonObject *c=calendar_new(event,first);json_array_remove_element(json_object_get_array_member(*first,"sources"),1);json_object_set_int_member(at(*first,"sources",0),"count",2);
    *second=json_object_new();gchar *rid=uuid(701),*aid=uuid(801);json_object_set_string_member(*second,"id",rid);json_object_set_string_member(*second,"name","İkinci rotasyon");json_object_set_object_member(*second,"settings",settings_new("linear"));JsonArray *sources=json_array_new();JsonObject *r=rule(2,1);json_object_set_int_member(r,"count",2);json_array_add_object_element(sources,r);json_object_set_array_member(*second,"sources",sources);json_array_add_object_element(json_object_get_array_member(c,"rotation_library"),*second);
    JsonObject *a=json_object_new();json_object_set_string_member(a,"id",aid);json_object_set_string_member(a,"rotation_id",rid);json_array_add_object_element(json_object_get_array_member(*event,"rotations"),a);g_free(rid);g_free(aid);return c;
}
static const gchar *track_group(JsonNode *n,guint i) { return json_object_get_string_member(at(json_node_get_object(n),"tracks",i),"calendar_group_id"); }
static void test_assignment_weighted_bursts_determinism_and_rng_independence(void)
{
    JsonObject *event,*a,*b,*calendar=two_groups(&event,&a,&b);json_object_set_string_member(event,"rotation_selection_mode","weighted");json_object_set_int_member(at(event,"rotations",0),"weight",80);json_object_set_int_member(at(event,"rotations",1),"weight",20);JsonNode *resolved=resolve(calendar,event);JsonObject *prepared=prepared_for(json_node_get_object(resolved));JsonNode *out=compiled(json_node_get_object(resolved),prepared,200),*again=compiled(json_node_get_object(resolved),prepared,200);const gchar *first=json_object_get_string_member(at(event,"rotations",0),"id");guint count=0;
    for(guint i=0;i<200;++i){g_assert_cmpstr(track_group(out,i),==,track_group(again,i));count+=g_str_equal(track_group(out,i),first);if(i%2==0)g_assert_cmpstr(track_group(out,i),==,track_group(out,i+1));}g_assert_cmpuint(count,>,130);g_assert_cmpuint(count,<,195);json_node_free(again);json_node_free(resolved);json_object_unref(prepared);
    json_object_set_boolean_member(at(a,"sources",0),"track_random",TRUE);json_object_set_boolean_member(at(b,"sources",0),"track_random",TRUE);set_transition(json_object_get_object_member(a,"settings"),1,250);set_transition(json_object_get_object_member(b,"settings"),1,250);resolved=resolve(calendar,event);prepared=prepared_for(json_node_get_object(resolved));again=compiled(json_node_get_object(resolved),prepared,200);for(guint i=0;i<200;++i)g_assert_cmpstr(track_group(out,i),==,track_group(again,i));json_node_free(out);json_node_free(again);json_node_free(resolved);json_object_unref(prepared);json_object_unref(calendar);
}
static void test_assignment_zero_disable_linear_and_typed_empty_parking(void)
{
    JsonObject *event,*a,*b,*calendar=two_groups(&event,&a,&b);json_object_set_int_member(at(event,"rotations",0),"weight",0);JsonNode *resolved=resolve(calendar,event);JsonObject *prepared=prepared_for(json_node_get_object(resolved));JsonObject *disabled=at(prepared,"sources",0);json_array_add_string_element(json_object_get_array_member(disabled,"reasons"),"playlist_disabled");json_object_set_array_member(disabled,"tracks",json_array_new());JsonNode *out=compiled(json_node_get_object(resolved),prepared,8);const gchar *second=json_object_get_string_member(at(event,"rotations",1),"id");for(guint i=0;i<8;++i)g_assert_cmpstr(track_group(out,i),==,second);json_node_free(out);json_node_free(resolved);json_object_unref(prepared);
    json_object_set_int_member(at(event,"rotations",1),"weight",0);resolved=resolve(calendar,event);prepared=prepared_for(json_node_get_object(resolved));GError *error=NULL;g_assert_null(radios_automation_calendar_prepared_programme_at(json_node_get_object(resolved),prepared,station,"park",2,actual_time("2026-10-09T08:01:00Z"),&error));g_assert_error(error,G_IO_ERROR,G_IO_ERROR_WOULD_BLOCK);g_clear_error(&error);g_assert_null(radios_automation_calendar_prepared_programme(json_node_get_object(resolved),prepared,station,"park",2,&error));g_assert_error(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA);g_clear_error(&error);json_node_free(resolved);json_object_unref(prepared);
    json_object_set_int_member(at(event,"rotations",0),"weight",100);resolved=resolve(calendar,event);prepared=prepared_for(json_node_get_object(resolved));json_array_add_string_element(json_object_get_array_member(at(prepared,"sources",0),"reasons"),"playlist_disabled");g_assert_null(radios_automation_calendar_prepared_programme_at(json_node_get_object(resolved),prepared,station,"park",2,actual_time("2026-10-09T08:01:00Z"),&error));g_assert_error(error,G_IO_ERROR,G_IO_ERROR_INVALID_DATA);g_clear_error(&error);json_node_free(resolved);json_object_unref(prepared);json_object_unref(calendar);
}
static void test_assignment_weighted_chooses_only_currently_legal_groups(void)
{
    JsonObject *event,*a,*b,*calendar=two_groups(&event,&a,&b);json_object_set_string_member(event,"rotation_selection_mode","weighted");json_object_set_int_member(at(event,"rotations",0),"weight",100);json_object_set_int_member(at(event,"rotations",1),"weight",1);json_object_set_int_member(at(a,"sources",0),"start_minute",601);json_object_set_int_member(at(a,"sources",0),"end_minute",700);JsonNode *resolved=resolve(calendar,event);JsonObject *prepared=prepared_for(json_node_get_object(resolved));JsonNode *out=compiled(json_node_get_object(resolved),prepared,20);const gchar *second=json_object_get_string_member(at(event,"rotations",1),"id");for(guint i=0;i<20;++i)g_assert_cmpstr(track_group(out,i),==,second);json_node_free(out);json_node_free(resolved);json_object_unref(prepared);json_object_unref(calendar);
}

static void test_actual_source_turns_survive_refill_and_native_fence(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    json_object_set_int_member(at(definition, "sources", 0), "interval_minutes", 5);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    JsonObject *turns = json_object_new();
    gchar *upper = g_ascii_strup(json_object_get_string_member(at(json_node_get_object(resolved), "rotations", 0), "id"), -1);
    gint64 first = actual_time("2026-10-09T08:00:00Z");
    json_object_set_int_member(turns, upper, first); g_free(upper);
    gchar *foreign = uuid(99999); json_object_set_int_member(turns, foreign, first); g_free(foreign);
    json_object_set_object_member(prepared, "calendar_turns", turns); GError *error = NULL;
    JsonNode *output = radios_automation_calendar_prepared_programme_at(json_node_get_object(resolved), prepared, station, "turn", 1, first + 120 * G_USEC_PER_SEC, &error);
    g_assert_no_error(error); g_assert_nonnull(output); expect_track(output, 0, 3); json_node_free(output);
    /* Remove the unrelated source. Its absence cannot reset the consumed turn. */
    json_array_remove_element(json_object_get_array_member(json_node_get_object(resolved), "rotations"), 1);
    g_assert_null(radios_automation_calendar_prepared_programme_at(json_node_get_object(resolved), prepared, station, "turn", 1, first + 120 * G_USEC_PER_SEC, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK); g_clear_error(&error);
    output = radios_automation_calendar_prepared_programme_at(json_node_get_object(resolved), prepared, station, "turn", 1, first + 300 * G_USEC_PER_SEC, &error);
    g_assert_no_error(error); g_assert_nonnull(output); expect_track(output, 0, 1);
    JsonObject *hint = json_object_get_object_member(at(json_node_get_object(output), "tracks", 0), "calendar_constraints");
    g_assert_cmpuint(json_object_get_size(hint), ==, 7);
    g_assert_cmpint(json_object_get_int_member(hint, "earliest_utc_us"), ==, first + 300 * G_USEC_PER_SEC);
    g_assert_false(json_object_has_member(json_node_get_object(output), "calendar_turns"));
    RadioSPlayoutRequest request = request_new(); request.now_utc_us = first + 300 * G_USEC_PER_SEC;
    RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(output), &request, &error);
    g_assert_no_error(error); g_assert_nonnull(plan); radios_playout_plan_free(plan);
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
static void test_actual_source_turns_reject_fabricated_identity_epoch_and_bounds(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    gint64 now = actual_time("2026-10-09T08:00:00Z"); GError *error = NULL;
    const gchar *id = json_object_get_string_member(at(json_node_get_object(resolved), "rotations", 0), "id");
    for (guint variant = 0; variant < 8; ++variant) {
        JsonObject *turns = json_object_new(); json_object_set_int_member(turns, id, now);
        if (variant == 0) { json_object_unref(turns); json_object_set_int_member(prepared, "calendar_turns", now); }
        else {
            if (variant == 1) json_object_set_int_member(turns, "bad-uuid", now);
            if (variant == 2) json_object_set_string_member(turns, id, "not-an-epoch");
            if (variant == 3) json_object_set_int_member(turns, id, -1);
            if (variant == 4) json_object_set_int_member(turns, id, now + 1);
            if (variant == 5) json_object_set_double_member(turns, id, (gdouble)now);
            if (variant == 6) { gchar *upper = g_ascii_strup(id, -1); json_object_set_int_member(turns, upper, now); g_free(upper); }
            if (variant == 7) for (guint i = 0; i < 2001; ++i) { gchar *other = uuid(50000 + i); json_object_set_int_member(turns, other, now); g_free(other); }
            json_object_set_object_member(prepared, "calendar_turns", turns);
        }
        g_assert_null(radios_automation_calendar_prepared_programme_at(json_node_get_object(resolved), prepared, station, "turn", 1, now, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    }
    json_object_remove_member(prepared, "calendar_turns");
    JsonNode *output = radios_automation_calendar_prepared_programme_at(json_node_get_object(resolved), prepared, station, "turn", 1, now, &error);
    g_assert_no_error(error); g_assert_nonnull(output); json_node_free(output);
    json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}

static void test_timezone_and_shared_audible_geometry(void)
{
    JsonObject *event = event_new(), *prepared = prepared_new(), *source = add_source(event, prepared, 1, 1);
    JsonArray *tracks = json_object_get_array_member(source, "tracks");
    for (guint id = 1; id <= 2; ++id) {
        JsonObject *track = row(id, "music"), *detail = json_object_get_object_member(track, "detail");
        JsonObject *version = json_object_get_object_member(detail, "version"), *analysis = json_object_new(), *audible = json_object_new();
        json_object_set_int_member(version, "duration_ms", 5000);
        json_object_set_string_member(analysis, "sha256", json_object_get_string_member(version, "sha256"));
        json_object_set_int_member(analysis, "sample_rate", 48000); json_object_set_int_member(analysis, "sample_frames", 240000);
        json_object_set_int_member(audible, "start_ms", 1000); json_object_set_int_member(audible, "end_ms", 4000);
        json_object_set_object_member(analysis, "audible", audible); json_object_set_object_member(detail, "analysis", analysis);
        json_array_add_object_element(tracks, track);
    }
    JsonObject *calendar = json_object_new(); json_object_set_int_member(calendar, "schema", 1);
    json_object_set_string_member(calendar, "timezone", "Europe/Istanbul"); GError *error = NULL;
    JsonNode *resolved = radios_automation_calendar_resolve_event(calendar, event, &error); g_assert_no_error(error); g_assert_nonnull(resolved);
    JsonObject *local = json_node_get_object(resolved); g_assert_cmpstr(json_object_get_string_member(local, "timezone"), ==, "Europe/Istanbul");
    gint64 utc_start = radios_automation_calendar_event_time(event, 480), start = radios_automation_calendar_event_time(local, 480);
    g_assert_cmpint(utc_start - start, ==, G_GINT64_CONSTANT(3) * 60 * 60 * G_USEC_PER_SEC);
    JsonNode *selected = radios_automation_calendar_prepared_programme_at(local, prepared, station, "shared-geometry", 2, start, &error);
    g_assert_no_error(error); g_assert_nonnull(selected);
    RadioSPlayoutRequest request = {.station_id = station, .database_file = database, .now_utc_us = start, .start_utc_us = start};
    RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(selected), &request, &error); g_assert_no_error(error); g_assert_nonnull(plan);
    g_assert_cmpuint(plan->entries->len, ==, 2);
    RadioSPlayoutEntry *first = g_ptr_array_index(plan->entries, 0), *second = g_ptr_array_index(plan->entries, 1);
    g_assert_cmpuint(first->run.media.cue_in_frame, ==, 48000); g_assert_cmpuint(first->run.media.cue_out_frame, ==, 192000);
    g_assert_cmpint(second->planned_utc_us - first->planned_utc_us, ==, 3 * G_USEC_PER_SEC);
    g_assert_cmpint(first->run.latest_start_utc_us, ==, radios_automation_calendar_event_time(local, 600) - 3 * G_USEC_PER_SEC);
    radios_playout_plan_free(plan); json_node_free(selected); json_node_free(resolved);
    json_object_set_string_member(event, "timezone", "Europe/Berlin"); json_object_set_string_member(event, "date", "2026-03-29");
    g_assert_cmpint(radios_automation_calendar_event_time(event, 1440) - radios_automation_calendar_event_time(event, 0), ==, G_GINT64_CONSTANT(23) * 60 * 60 * G_USEC_PER_SEC);
    json_object_set_string_member(calendar, "timezone", "Unknown/Invalid");
    resolved = radios_automation_calendar_resolve_event(calendar, event, &error); g_assert_null(resolved); g_assert_nonnull(error); g_clear_error(&error);
    json_object_unref(calendar); json_object_unref(event); json_object_unref(prepared);
}

/* Calendar selection must budget the same audible source tail as playback,
 * including marked recordings. Two fades fit this eight-second window; two
 * full files do not, and one-player rhythm points cannot shorten the songs. */
static void test_marked_sound_end_window_matches_native_playout(void)
{
    for (guint explicit_points = 0; explicit_points < 2; ++explicit_points) {
        JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
        json_object_set_int_member(event, "end_minute", 481);
        set_transition(json_object_get_object_member(definition, "settings"), 1, 1000);
        json_object_set_int_member(at(definition, "sources", 0), "count", 2);
        json_object_set_int_member(at(definition, "sources", 1), "weight", 0);
        JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
        for (guint i = 0; i < 2; ++i) {
            JsonObject *detail = json_object_get_object_member(at(at(prepared, "sources", 0), "tracks", i), "detail");
            JsonObject *media = json_object_get_object_member(detail, "media"), *version = json_object_get_object_member(detail, "version");
            JsonObject *analysis = json_object_new(), *audible = json_object_new(), *marks = json_object_new();
            json_object_set_int_member(version, "duration_ms", 6000);
            json_object_set_string_member(analysis, "sha256", json_object_get_string_member(version, "sha256"));
            json_object_set_int_member(analysis, "sample_rate", 48000); json_object_set_int_member(analysis, "sample_frames", 288000);
            json_object_set_int_member(audible, "start_ms", 0); json_object_set_int_member(audible, "end_ms", 4500);
            json_object_set_object_member(analysis, "audible", audible); json_object_set_object_member(detail, "analysis", analysis);
            const gchar *bindings[] = {"station_id", "item_id", "version_id", "media_file_id"};
            for (guint k = 0; k < G_N_ELEMENTS(bindings); ++k)
                json_object_set_string_member(marks, bindings[k], json_object_get_string_member(media, bindings[k]));
            json_object_set_string_member(marks, "media_sha256", json_object_get_string_member(version, "sha256"));
            json_object_set_int_member(marks, "media_duration_ms", 6000); json_object_set_int_member(marks, "cue_in_ms", 0);
            json_object_set_int_member(marks, "out_ms", 6000); json_object_set_boolean_member(marks, "automatic", FALSE);
            if (explicit_points) { json_object_set_int_member(marks, "autodj_in_ms", 1000); json_object_set_int_member(marks, "autodj_out_ms", 3000); }
            json_object_set_object_member(detail, "markers", marks);
        }
        gchar *before = serialized(prepared); gint64 start = actual_time("2026-10-09T08:00:52Z");
        for (guint single = 0; single < 2; ++single) {
            GError *error = NULL;
            JsonNode *selected = radios_automation_calendar_prepared_programme_at_mode(json_node_get_object(resolved), prepared,
                station, "marked-sound-end", 2, start, single, &error);
            g_assert_no_error(error); g_assert_nonnull(selected);
            g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(selected), "tracks")), ==, single ? 1 : 2);
            RadioSPlayoutRequest request = {.station_id = station, .database_file = database, .now_utc_us = start,
                .start_utc_us = start, .single_player = single, .allow_single_routed = TRUE};
            RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(selected), &request, &error);
            g_assert_no_error(error); g_assert_nonnull(plan);
            RadioSPlayoutEntry *first = g_ptr_array_index(plan->entries, 0);
            g_assert_cmpuint(first->run.media.cue_out_frame, ==, 216000);
            g_assert_cmpuint(first->run.autodj_out_frame, ==, explicit_points && !single ? 144000 : 0);
            if (!single) {
                RadioSPlayoutEntry *second = g_ptr_array_index(plan->entries, 1);
                g_assert_cmpint(second->planned_utc_us - first->planned_utc_us, ==, explicit_points ? 2000000 : 3500000);
            }
            radios_playout_plan_free(plan); json_node_free(selected);
        }
        gchar *after = serialized(prepared); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
        json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
    }
}

static void test_short_autodj_incoming_span_keeps_source_window_eligible(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    set_transition(json_object_get_object_member(definition, "settings"), 1, 3000);
    json_object_set_int_member(event, "end_minute", 482);
    JsonObject *first_rule = at(definition, "sources", 0), *second_rule = at(definition, "sources", 1);
    json_object_set_int_member(first_rule, "count", 1); json_object_set_int_member(second_rule, "count", 1);
    json_object_set_int_member(second_rule, "start_minute", 481);
    json_array_add_object_element(song_rules(first_rule, "linear"), song_rule(1, 100, 1, 1, 1));
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    for (guint i = 0; i < 2; ++i) json_array_remove_element(json_object_get_array_member(at(prepared, "sources", i), "tracks"), 1);
    JsonObject *a = at(at(prepared, "sources", 0), "tracks", 0), *b = at(at(prepared, "sources", 1), "tracks", 0);
    JsonObject *adetail = json_object_get_object_member(a, "detail"), *bdetail = json_object_get_object_member(b, "detail");
    json_object_set_int_member(json_object_get_object_member(adetail, "version"), "duration_ms", 6000);
    JsonObject *version = json_object_get_object_member(bdetail, "version"), *media = json_object_get_object_member(bdetail, "media");
    json_object_set_int_member(version, "duration_ms", 10000);
    JsonObject *marks = json_object_new(); const gchar *bindings[] = {"station_id", "item_id", "version_id", "media_file_id"};
    for (guint i = 0; i < G_N_ELEMENTS(bindings); ++i)
        json_object_set_string_member(marks, bindings[i], json_object_get_string_member(media, bindings[i]));
    json_object_set_string_member(marks, "media_sha256", json_object_get_string_member(version, "sha256"));
    json_object_set_string_member(marks, "status", "draft"); json_object_set_int_member(marks, "media_duration_ms", 10000);
    json_object_set_int_member(marks, "cue_in_ms", 0); json_object_set_int_member(marks, "out_ms", 10000);
    json_object_set_int_member(marks, "intro_ms", 0); json_object_set_int_member(marks, "fade_start_ms", 10000);
    json_object_set_int_member(marks, "fade_end_ms", 10000); json_object_set_int_member(marks, "auto_next_ms", 10000);
    json_object_set_int_member(marks, "overlap_ms", 0); json_object_set_boolean_member(marks, "automatic", FALSE);
    json_object_set_int_member(marks, "autodj_in_ms", 2000); json_object_set_int_member(marks, "autodj_out_ms", 3000);
    json_object_set_object_member(bdetail, "markers", marks);
    gchar *before = serialized(prepared); gint64 start = actual_time("2026-10-09T08:00:55Z"); GError *error = NULL;
    JsonNode *selected = radios_automation_calendar_prepared_programme_at(json_node_get_object(resolved), prepared, station,
        "short-autodj-window", 2, start, &error);
    g_assert_no_error(error); g_assert_nonnull(selected);
    /* A's physical end is 08:01:01. Its incoming fade must be one second,
     * so B starts exactly at its source window 08:01:00, not 08:00:58. */
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(selected), "tracks")), ==, 2);
    expect_track(selected, 0, 1); expect_track(selected, 1, 3);
    RadioSPlayoutRequest request = {.station_id = station, .database_file = database, .now_utc_us = start, .start_utc_us = start};
    RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(selected), &request, &error);
    g_assert_no_error(error); g_assert_nonnull(plan); g_assert_cmpuint(plan->entries->len, ==, 2);
    RadioSPlayoutEntry *first = g_ptr_array_index(plan->entries, 0), *second = g_ptr_array_index(plan->entries, 1);
    g_assert_cmpint(second->planned_utc_us, ==, actual_time("2026-10-09T08:01:00Z"));
    g_assert_cmpuint(first->run.overlap_frames, ==, 48000); g_assert_cmpuint(second->run.fade_in_frames, ==, 48000);
    g_assert_cmpuint(second->run.media.cue_in_frame, ==, 96000); g_assert_cmpuint(second->run.autodj_out_frame, ==, 144000);
    g_assert_cmpuint(second->run.media.cue_out_frame, ==, 480000);
    gchar *after = serialized(prepared); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
    radios_playout_plan_free(plan); json_node_free(selected); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}

static void test_semi_tight_slot_selects_complete_native_prefix(void)
{
    for (guint exit = 0; exit < 3; ++exit) {
        JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
        json_object_set_int_member(event, "end_minute", 481);
        set_transition(json_object_get_object_member(definition, "settings"), 1, 3000);
        json_object_set_int_member(at(definition, "sources", 0), "count", 2);
        json_object_set_int_member(at(definition, "sources", 1), "weight", 0);
        JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
        for (guint track = 0; track < 2; ++track) {
            JsonObject *detail = json_object_get_object_member(at(at(prepared, "sources", 0), "tracks", track), "detail");
            JsonObject *media = json_object_get_object_member(detail, "media"), *version = json_object_get_object_member(detail, "version"), *marks = json_object_new();
            json_object_set_int_member(version, "duration_ms", 10000);
            const gchar *bindings[] = {"station_id", "item_id", "version_id", "media_file_id"};
            for (guint i = 0; i < G_N_ELEMENTS(bindings); ++i) json_object_set_string_member(marks, bindings[i], json_object_get_string_member(media, bindings[i]));
            json_object_set_string_member(marks, "media_sha256", json_object_get_string_member(version, "sha256"));
            json_object_set_int_member(marks, "media_duration_ms", 10000);
            json_object_set_int_member(marks, "cue_in_ms", 1000); json_object_set_int_member(marks, "out_ms", 9000);
            json_object_set_int_member(marks, "intro_ms", 1000); json_object_set_int_member(marks, "fade_start_ms", 8000);
            json_object_set_int_member(marks, "fade_end_ms", 9000); json_object_set_int_member(marks, "auto_next_ms", 7000);
            json_object_set_int_member(marks, "overlap_ms", 2000); json_object_set_boolean_member(marks, "automatic", exit == 2);
            json_object_set_int_member(marks, "autodj_in_ms", 4000);
            if (exit == 1) json_object_set_int_member(marks, "autodj_out_ms", 7000);
            json_object_set_object_member(detail, "markers", marks);
        }
        gchar *before = serialized(prepared), *event_before = serialized(json_node_get_object(resolved));
        gint64 start = actual_time("2026-10-09T08:00:48Z"); GError *error = NULL;
        JsonNode *ordinary = radios_automation_calendar_prepared_programme_at(json_node_get_object(resolved), prepared,
            station, "semi-tight", 2, start, &error); g_assert_no_error(error); g_assert_nonnull(ordinary);
        g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(ordinary), "tracks")), ==, 2);
        JsonNode *native = radios_automation_calendar_prepared_programme_at_mode(json_node_get_object(resolved), prepared,
            station, "semi-tight", 2, start, TRUE, &error); g_assert_no_error(error); g_assert_nonnull(native);
        g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(native), "tracks")), ==, 1);
        RadioSPlayoutRequest request = {.station_id = station, .database_file = database, .now_utc_us = start,
            .start_utc_us = start, .single_player = TRUE, .allow_single_routed = TRUE};
        RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(native), &request, &error);
        g_assert_no_error(error); g_assert_nonnull(plan); g_assert_cmpuint(plan->entries->len, ==, 1);
        RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, 0);
        g_assert_cmpuint(entry->run.media.cue_in_frame, ==, 48000); g_assert_cmpuint(entry->run.media.cue_out_frame, ==, 432000);
        g_assert_cmpuint(entry->run.overlap_frames, ==, 0); g_assert_false(entry->run.beat_sync);
        g_assert_cmpint(entry->run.mode, ==, exit == 2 ? RADIOS_AUDIO_RUN_CUE : RADIOS_AUDIO_RUN_EOF);
        g_assert_cmpuint(entry->run.autodj_out_frame, ==, 0);
        if (exit == 2) g_assert_cmpuint(entry->run.auto_next_frame, ==, 336000);
        gchar *after = serialized(prepared), *event_after = serialized(json_node_get_object(resolved));
        g_assert_cmpstr(before, ==, after); g_assert_cmpstr(event_before, ==, event_after);
        g_free(before); g_free(after); g_free(event_before); g_free(event_after);
        JsonObject *settings = json_object_get_object_member(at(json_node_get_object(resolved), "rotations", 0), "calendar_settings");
        json_object_set_int_member(at(settings, "transitions", 0), "mode", 5);
        g_assert_null(radios_automation_calendar_prepared_programme_at_mode(json_node_get_object(resolved), prepared,
            station, "semi-tight", 2, start, TRUE, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
        radios_playout_plan_free(plan); json_node_free(native); json_node_free(ordinary); json_node_free(resolved);
        json_object_unref(prepared); json_object_unref(calendar);
    }
}

static void test_copyright_planned_dates_and_mixed_source(void)
{
    JsonObject *event = event_new(), *prepared = prepared_new(), *source = add_source(event, prepared, 1, 1);
    JsonArray *tracks = json_object_get_array_member(source, "tracks");
    JsonObject *denied = row(1, "music"), *future = row(2, "music"), *unmatched = row(3, "music");
    json_array_add_object_element(tracks, denied); json_array_add_object_element(tracks, future); json_array_add_object_element(tracks, unmatched);
    /* Backend prepare at an earlier clock had denied this row; bound media
     * integrity still holds and it becomes eligible at the scheduled start. */
    json_object_set_boolean_member(future, "media_eligible", TRUE); json_object_set_boolean_member(future, "eligible", FALSE);
    JsonObject *policy = json_object_new(); json_object_set_int_member(policy, "schema", 1);
    json_object_set_boolean_member(policy, "restrict", TRUE); json_object_set_string_member(policy, "unregistered", "allow");
    JsonArray *rules = json_array_new(); json_object_set_array_member(policy, "rules", rules);
    json_object_set_object_member(prepared, "copyright_policy", policy);
    for (guint i = 0; i < 2; ++i) {
        JsonObject *rule = json_object_new(); gchar *id = uuid(900000 + i), *title = uuid(i + 1);
        json_object_set_string_member(rule, "id", id); json_object_set_string_member(rule, "artist", "");
        json_object_set_string_member(rule, "album", ""); json_object_set_string_member(rule, "title", title);
        json_object_set_string_member(rule, "starts_at", i ? "2026-10-09T08:00:00Z" : "2026-10-01T00:00:00Z");
        json_object_set_string_member(rule, "ends_at", i ? "2026-10-09T09:00:00Z" : "2026-10-09T08:00:00Z");
        json_object_set_string_member(rule, "note", ""); json_object_set_boolean_member(rule, "enabled", TRUE);
        json_array_add_object_element(rules, rule); g_free(id); g_free(title);
    }
    gchar *original = serialized(prepared); GError *error = NULL;
    /* Legacy ordered calendar selection and named late join both select each
     * individual item using its planned UTC, without dropping the source. */
    JsonNode *selected = radios_automation_calendar_prepared_programme(event, prepared, station, "copyright", 4, &error);
    g_assert_no_error(error); g_assert_nonnull(selected); JsonArray *out = json_object_get_array_member(json_node_get_object(selected), "tracks");
    g_assert_cmpuint(json_array_get_length(out), ==, 4);
    for (guint i = 0; i < 4; ++i) {
        JsonObject *media = json_object_get_object_member(json_object_get_object_member(at(json_node_get_object(selected), "tracks", i), "detail"), "media");
        gchar *expected = uuid(10000 + (i % 2 ? 3 : 2)); g_assert_cmpstr(json_object_get_string_member(media, "_id"), ==, expected); g_free(expected);
    }
    json_node_free(selected);
    selected = radios_automation_calendar_prepared_programme_at(event, prepared, station, "copyright", 2,
        actual_time("2026-10-09T08:01:00Z"), &error);
    g_assert_no_error(error); g_assert_nonnull(selected); json_node_free(selected);
    gchar *after = serialized(prepared); g_assert_cmpstr(after, ==, original); g_free(after); g_free(original);
    json_object_set_boolean_member(policy, "restrict", FALSE);
    selected = radios_automation_calendar_prepared_programme(event, prepared, station, "copyright", 3, &error);
    g_assert_no_error(error); g_assert_nonnull(selected);
    JsonObject *first = json_object_get_object_member(json_object_get_object_member(at(json_node_get_object(selected), "tracks", 0), "detail"), "media");
    gchar *expired_identity = uuid(10001); g_assert_cmpstr(json_object_get_string_member(first, "_id"), ==, expired_identity); g_free(expired_identity);
    json_node_free(selected);
    /* An entirely expired source must return promptly, preserving the source
     * document, instead of repeatedly searching identical blocked visits. */
    json_object_set_boolean_member(policy, "restrict", TRUE);
    JsonObject *global = json_object_new(); gchar *rule_id = uuid(999999);
    json_object_set_string_member(global, "id", rule_id); g_free(rule_id);
    json_object_set_string_member(global, "artist", "Sanatçı"); json_object_set_string_member(global, "album", "");
    json_object_set_string_member(global, "title", ""); json_object_set_string_member(global, "note", "");
    json_object_set_string_member(global, "starts_at", "2026-10-01T00:00:00Z");
    json_object_set_string_member(global, "ends_at", "2026-10-09T08:00:00Z"); json_object_set_boolean_member(global, "enabled", TRUE);
    json_array_add_object_element(rules, global);
    selected = radios_automation_calendar_prepared_programme(event, prepared, station, "copyright", 3, &error);
    g_assert_null(selected); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    g_assert_cmpuint(json_array_get_length(tracks), ==, 3);
    json_object_unref(prepared); json_object_unref(event);
}
/* Continuity rules (named path): a sweeper/jingle never follows another one,
 * an opted-in soft block end lets one last item overrun, and opted-in gap
 * relaxing keeps playing instead of silence without immediate repeats. */
static const gchar *track_kind(JsonNode *node, guint index)
{ return json_object_get_string_member(json_object_get_object_member(at(json_node_get_object(node), "tracks", index), "metadata"), "kind"); }
static const gchar *track_media(JsonNode *node, guint index)
{ return json_object_get_string_member(json_object_get_object_member(json_object_get_object_member(at(json_node_get_object(node), "tracks", index), "detail"), "media"), "_id"); }
static gboolean has_diagnostic(JsonNode *node, const gchar *reason)
{
    JsonArray *items = json_object_get_array_member(json_node_get_object(node), "diagnostics");
    for (guint i = 0; i < json_array_get_length(items); ++i)
        if (!g_strcmp0(json_object_get_string_member(json_array_get_object_element(items, i), "reason"), reason)) return TRUE;
    return FALSE;
}
static void test_named_no_consecutive_short_items(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    json_object_set_int_member(at(definition, "sources", 0), "count", 2);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    for (guint i = 0; i < 2; ++i)
        json_object_set_string_member(json_object_get_object_member(at(at(prepared, "sources", 0), "tracks", i), "metadata"), "kind", "sweeper");
    JsonNode *output = compiled(json_node_get_object(resolved), prepared, 20);
    guint n = json_array_get_length(json_object_get_array_member(json_node_get_object(output), "tracks")), sweepers = 0;
    g_assert_cmpuint(n, ==, 20);
    for (guint i = 0; i < n; ++i) {
        gboolean sweeper = !g_strcmp0(track_kind(output, i), "sweeper"); sweepers += sweeper;
        if (i && sweeper) g_assert_cmpstr(track_kind(output, i - 1), !=, "sweeper");
    }
    g_assert_cmpuint(sweepers, >, 0);
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
static void test_named_soft_end_and_relaxed_gaps_opt_in(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    JsonObject *settings = json_object_get_object_member(definition, "settings");
    json_object_set_int_member(event, "end_minute", 481);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    for (guint s = 0; s < 2; ++s) for (guint i = 0; i < 2; ++i)
        json_object_set_int_member(json_object_get_object_member(json_object_get_object_member(at(at(prepared, "sources", s), "tracks", i), "detail"), "version"), "duration_ms", 40000);
    JsonNode *output = compiled(json_node_get_object(resolved), prepared, 20);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(output), "tracks")), ==, 1);
    json_node_free(output); json_node_free(resolved);
    json_object_set_int_member(settings, "soft_end_seconds", 60);
    resolved = resolve(calendar, event);
    output = compiled(json_node_get_object(resolved), prepared, 20);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(output), "tracks")), ==, 2);
    g_assert_true(has_diagnostic(output, "calendar_soft_end_overrun"));
    JsonObject *hint = json_object_get_object_member(at(json_node_get_object(output), "tracks", 1), "calendar_constraints");
    g_assert_cmpint(json_object_get_int_member(hint, "latest_end_utc_us"), >, actual_time("2026-10-09T08:01:00Z"));
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared);
    json_object_remove_member(settings, "soft_end_seconds"); json_object_set_int_member(event, "end_minute", 600);
    json_object_set_int_member(settings, "track_gap_minutes", 10);
    resolved = resolve(calendar, event); prepared = prepared_for(json_node_get_object(resolved));
    output = compiled(json_node_get_object(resolved), prepared, 20);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(output), "tracks")), ==, 4);
    json_node_free(output); json_node_free(resolved);
    json_object_set_boolean_member(settings, "relax_gaps", TRUE);
    resolved = resolve(calendar, event);
    output = compiled(json_node_get_object(resolved), prepared, 20);
    guint n = json_array_get_length(json_object_get_array_member(json_node_get_object(output), "tracks"));
    g_assert_cmpuint(n, ==, 20); g_assert_true(has_diagnostic(output, "calendar_rules_relaxed"));
    for (guint i = 1; i < n; ++i) g_assert_cmpstr(track_media(output, i), !=, track_media(output, i - 1));
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
/* "excluded_kinds" on a rotation drops those content kinds from every source
 * of the rotation (news counts as music), strips the rotation's in-song jingle
 * layers when jingles are excluded, rejects a rotation whose playable media
 * is entirely excluded, and is validated as a distinct list of AutoDJ kinds
 * at rotation level only. */
static void test_named_excluded_kinds_filter_jingle_layers_and_validation(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    JsonObject *settings = json_object_get_object_member(definition, "settings");
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_new();
    JsonArray *rules = json_object_get_array_member(json_node_get_object(resolved), "rotations");
    const gchar *kinds[] = {"music", "advertisement", "jingle", "sweeper", "news"};
    for (guint i = 0; i < json_array_get_length(rules); ++i) {
        JsonObject *rotation = json_array_get_object_element(rules, i), *source = json_object_new();
        json_object_set_string_member(source, "source_id", json_object_get_string_member(rotation, "id"));
        json_object_set_string_member(source, "playlist_id", json_object_get_string_member(rotation, "playlist_id"));
        json_object_set_string_member(source, "source_name", "Türlü seçki"); json_object_set_array_member(source, "reasons", json_array_new());
        JsonArray *tracks = json_array_new();
        for (guint j = 0; j < G_N_ELEMENTS(kinds); ++j) json_array_add_object_element(tracks, row(i * 10 + j + 1, kinds[j]));
        json_object_set_array_member(source, "tracks", tracks); json_array_add_object_element(json_object_get_array_member(prepared, "sources"), source);
    }
    JsonNode *output = compiled(json_node_get_object(resolved), prepared, 20); gboolean seen_advert = FALSE;
    for (guint i = 0; i < 20; ++i) seen_advert |= g_str_equal(track_kind(output, i), "advertisement");
    g_assert_true(seen_advert); json_node_free(output); json_node_free(resolved);
    JsonArray *excluded = json_array_new(); json_array_add_string_element(excluded, "advertisement"); json_array_add_string_element(excluded, "sweeper");
    json_object_set_array_member(settings, "excluded_kinds", excluded);
    resolved = resolve(calendar, event); output = compiled(json_node_get_object(resolved), prepared, 20);
    for (guint i = 0; i < 20; ++i) {
        g_assert_cmpstr(track_kind(output, i), !=, "advertisement"); g_assert_cmpstr(track_kind(output, i), !=, "sweeper");
    }
    g_assert_false(has_diagnostic(output, "calendar_source_kind_excluded")); json_node_free(output); json_node_free(resolved);
    /* Music excluded too: news is music for AutoDJ purposes, so only jingles remain. */
    json_array_add_string_element(json_object_get_array_member(settings, "excluded_kinds"), "music");
    resolved = resolve(calendar, event); output = compiled(json_node_get_object(resolved), prepared, 6);
    guint produced = json_array_get_length(json_object_get_array_member(json_node_get_object(output), "tracks"));
    g_assert_cmpuint(produced, >=, 1);
    for (guint i = 0; i < produced; ++i) g_assert_cmpstr(track_kind(output, i), ==, "jingle");
    json_node_free(output); json_node_free(resolved);
    /* Everything excluded: a precise rejection naming the setting, not silence. */
    json_array_add_string_element(json_object_get_array_member(settings, "excluded_kinds"), "jingle");
    resolved = resolve(calendar, event); GError *error = NULL;
    g_assert_null(radios_automation_calendar_prepared_programme(json_node_get_object(resolved), prepared, station, "named-fixed", 6, &error));
    g_assert_nonnull(error); g_assert_nonnull(strstr(error->message, "olmayan türler")); g_clear_error(&error); json_node_free(resolved);
    /* Jingle excluded strips the rotation's in-song jingle layers, root and children, in the resolved copy only. */
    excluded = json_array_new(); json_array_add_string_element(excluded, "jingle"); json_object_set_array_member(settings, "excluded_kinds", excluded);
    const gchar *policy_text = "{\"schema\":1,\"seed\":\"tür\",\"transitions\":[{\"id\":\"00000000-0000-4000-8000-000000000601\",\"mode\":1,\"overlap_ms\":2000,\"probability\":40}],"
        "\"jingles\":[{\"id\":\"00000000-0000-4000-8000-000000000602\",\"playlist_id\":\"00000000-0000-4000-8000-000000002001\",\"player\":31,\"anchor\":\"before_end\",\"seconds\":1,\"probability\":50}],"
        "\"children\":[{\"id\":\"00000000-0000-4000-8000-000000000603\",\"scope\":\"playlist\",\"ids\":[\"00000000-0000-4000-8000-000000002001\"],\"probability\":50,"
        "\"policy\":{\"schema\":1,\"jingles\":[{\"id\":\"00000000-0000-4000-8000-000000000604\",\"playlist_id\":\"00000000-0000-4000-8000-000000002001\",\"player\":31,\"anchor\":\"before_end\",\"seconds\":1,\"probability\":50}]}}]}";
    JsonParser *parser = json_parser_new(); g_assert_true(json_parser_load_from_data(parser, policy_text, -1, &error)); g_assert_no_error(error);
    json_object_set_member(settings, "probability_policy", json_node_copy(json_parser_get_root(parser))); g_object_unref(parser);
    resolved = resolve(calendar, event);
    JsonObject *policy = json_object_get_object_member(json_object_get_object_member(at(json_node_get_object(resolved), "rotations", 0), "calendar_settings"), "probability_policy");
    g_assert_nonnull(policy); g_assert_false(json_object_has_member(policy, "jingles")); g_assert_true(json_object_has_member(policy, "transitions"));
    g_assert_false(json_object_has_member(json_object_get_object_member(at(policy, "children", 0), "policy"), "jingles"));
    g_assert_true(json_object_has_member(json_object_get_object_member(settings, "probability_policy"), "jingles"));
    json_node_free(resolved); json_object_remove_member(settings, "probability_policy");
    /* Validation: rotation level only, known kinds, distinct, a string list of at most four. */
    const gchar *invalid[] = {"[\"Reklam\"]", "[\"music\",\"music\"]", "\"advertisement\"", "[1]", "{}", "[\"music\",\"advertisement\",\"jingle\",\"sweeper\",\"news\"]"};
    for (guint i = 0; i < G_N_ELEMENTS(invalid); ++i) {
        parser = json_parser_new(); g_assert_true(json_parser_load_from_data(parser, invalid[i], -1, NULL));
        json_object_set_member(settings, "excluded_kinds", json_node_copy(json_parser_get_root(parser))); g_object_unref(parser);
        g_assert_null(radios_automation_calendar_resolve_event(calendar, event, &error)); g_assert_nonnull(error); g_clear_error(&error);
    }
    json_object_remove_member(settings, "excluded_kinds");
    JsonObject *override = json_object_new(); JsonArray *list = json_array_new(); json_array_add_string_element(list, "jingle");
    json_object_set_array_member(override, "excluded_kinds", list);
    json_object_set_object_member(at(definition, "sources", 0), "settings", override);
    g_assert_null(radios_automation_calendar_resolve_event(calendar, event, &error)); g_assert_nonnull(error); g_clear_error(&error);
    json_object_remove_member(at(definition, "sources", 0), "settings");
    resolved = resolve(calendar, event); json_node_free(resolved);
    json_object_unref(prepared); json_object_unref(calendar);
}
/* The selection estimates every start with the transition the native planner
 * really applies: under a rotation AutoDJ kind policy that is the kind
 * boundary, not the probabilistic transitions tab. Four 30 s songs with a
 * 12 s music crossfade reach the fifth slot at 72 s, so a two-minute song gap
 * blocks a repeat there (no legal candidate) and a one-minute gap allows it
 * at exactly the time native playout then plans. */
static void test_kind_transition_estimate_matches_native_repeat_gap(void)
{
    const gint64 gap_minutes[] = {2, 1}; const guint expected[] = {4, 5};
    for (guint which = 0; which < G_N_ELEMENTS(gap_minutes); ++which) {
        JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
        JsonObject *settings = json_object_get_object_member(definition, "settings"), *policy = json_object_new();
        json_object_set_object_member(policy, "music", kind_choice(1, 12000)); json_object_set_object_member(policy, "advertisement", kind_choice(0, 0));
        json_object_set_object_member(policy, "jingle", kind_choice(1, 1000)); json_object_set_object_member(policy, "sweeper", kind_choice(2, 500));
        json_object_set_object_member(settings, "kind_transitions", policy); json_object_set_int_member(settings, "track_gap_minutes", gap_minutes[which]);
        JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
        for (guint s = 0; s < 2; ++s) for (guint i = 0; i < 2; ++i)
            json_object_set_int_member(json_object_get_object_member(json_object_get_object_member(at(at(prepared, "sources", s), "tracks", i), "detail"), "version"), "duration_ms", 30000);
        JsonNode *output = compiled(json_node_get_object(resolved), prepared, 5);
        JsonArray *tracks = json_object_get_array_member(json_node_get_object(output), "tracks");
        g_assert_cmpuint(json_array_get_length(tracks), ==, expected[which]);
        g_assert_cmpint(has_diagnostic(output, "calendar_no_legal_candidate"), ==, which == 0);
        for (guint i = 0; i < expected[which]; ++i) {
            JsonObject *hint = json_object_get_object_member(json_array_get_object_element(tracks, i), "calendar_transition");
            g_assert_cmpint(json_object_get_int_member(hint, "mode"), ==, 1); g_assert_cmpint(json_object_get_int_member(hint, "overlap_ms"), ==, 12000);
            JsonObject *constraints = json_object_get_object_member(json_array_get_object_element(tracks, i), "calendar_constraints");
            g_assert_cmpint(json_object_get_int_member(constraints, "track_gap_ms"), ==, gap_minutes[which] * 60000);
        }
        if (which) g_assert_cmpstr(track_media(output, 4), ==, track_media(output, 0));
        RadioSPlayoutRequest request = request_new(); GError *error = NULL;
        RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(output), &request, &error);
        g_assert_no_error(error); g_assert_nonnull(plan); g_assert_cmpuint(plan->entries->len, ==, expected[which]);
        RadioSPlayoutEntry *first = g_ptr_array_index(plan->entries, 0);
        g_assert_cmpint(first->planned_utc_us, ==, actual_time("2026-10-09T08:00:00Z"));
        for (guint i = 1; i < plan->entries->len; ++i) {
            RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, i);
            g_assert_cmpint(entry->planned_utc_us - first->planned_utc_us, ==, (gint64)i * 18 * G_USEC_PER_SEC);
            g_assert_cmpuint(entry->run.fade_in_frames, ==, 576000);
        }
        radios_playout_plan_free(plan); json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
    }
}
static JsonArray *preset_ids(JsonArray *presets, guint first, guint count)
{
    JsonArray *ids = json_array_new();
    for (guint i = 0; i < count; ++i) json_array_add_string_element(ids, json_object_get_string_member(json_array_get_object_element(presets, first + i), "id"));
    return ids;
}
static JsonObject *preset_assignment(JsonArray *ids, const gchar *rotation)
{
    JsonObject *entry = json_object_new(); json_object_set_array_member(entry, "presets", ids);
    json_object_set_string_member(entry, "rotation", rotation); return entry;
}
/* Enables designed presets on the fixture rotation: the templates plus a
 * music assignment; an optional schedule row for the given weekday. */
static JsonArray *presets_enable(JsonObject *definition, const gchar *rotation, gint weekday)
{
    JsonObject *settings = json_object_get_object_member(definition, "settings");
    JsonArray *presets = radios_transition_preset_templates(); json_object_set_array_member(settings, "autodj_presets", presets);
    JsonObject *automation = json_object_new(), *kinds = json_object_new();
    json_object_set_boolean_member(automation, "enabled", TRUE);
    json_object_set_object_member(kinds, "music", preset_assignment(preset_ids(presets, 0, 2), rotation));
    json_object_set_object_member(automation, "kinds", kinds);
    if (weekday) {
        JsonArray *schedule = json_array_new(), *days = json_array_new(); JsonObject *row = json_object_new(); gchar *id = uuid(950);
        json_object_set_string_member(row, "id", id); g_free(id); json_object_set_string_member(row, "name", "Sabah");
        json_array_add_int_element(days, weekday); json_object_set_array_member(row, "days", days);
        json_object_set_int_member(row, "start_minute", 480); json_object_set_int_member(row, "end_minute", 600);
        json_object_set_string_member(row, "kind", "music"); json_object_set_array_member(row, "presets", preset_ids(presets, 2, 1));
        json_object_set_string_member(row, "rotation", "sequential"); json_array_add_object_element(schedule, row);
        json_object_set_array_member(automation, "schedule", schedule);
    }
    json_object_set_object_member(settings, "autodj_automation", automation); return presets;
}
static const gchar *hint_preset(JsonArray *tracks, guint index)
{
    JsonObject *hint = json_object_get_object_member(json_array_get_object_element(tracks, index), "calendar_transition");
    return json_object_get_string_member(json_object_get_object_member(hint, "shape"), "name");
}
/* Designed presets ("AutoDJ otomasyonu") replace the kind policy for their
 * kinds: each boundary carries the preset's length, beat sync and shape;
 * sequential rotation alternates, random rotation is reproducible per song
 * pair, and a schedule row matching the block's weekday and clock wins. */
static void test_autodj_presets_rotation_and_schedule(void)
{
    const gchar *rotations[] = {"sequential", "random"};
    for (guint r = 0; r < G_N_ELEMENTS(rotations); ++r) {
        JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
        presets_enable(definition, rotations[r], 0);
        JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
        JsonNode *output = compiled(json_node_get_object(resolved), prepared, 12), *again = compiled(json_node_get_object(resolved), prepared, 12);
        JsonArray *tracks = json_object_get_array_member(json_node_get_object(output), "tracks"), *repeated = json_object_get_array_member(json_node_get_object(again), "tracks");
        g_assert_cmpuint(json_array_get_length(tracks), >=, 4); g_assert_cmpuint(json_array_get_length(repeated), ==, json_array_get_length(tracks));
        for (guint i = 0; i + 1 < json_array_get_length(tracks); ++i) {
            JsonObject *hint = json_object_get_object_member(json_array_get_object_element(tracks, i), "calendar_transition");
            JsonObject *shape = json_object_get_object_member(hint, "shape"); const gchar *name = json_object_get_string_member(shape, "name");
            g_assert_true(radios_transition_preset_valid(shape, TRUE));
            g_assert_cmpint(json_object_get_int_member(hint, "mode"), ==, 1);
            g_assert_cmpint(json_object_get_int_member(hint, "overlap_ms"), ==, json_object_get_int_member(shape, "duration_ms"));
            g_assert_cmpint(json_object_has_member(hint, "beat_sync"), ==, json_object_get_boolean_member(shape, "beat_sync"));
            if (r == 0) g_assert_cmpstr(name, ==, i % 2 ? "Bas dalgalı geçiş" : "Düz geçiş");
            else g_assert_true(g_str_equal(name, "Düz geçiş") || g_str_equal(name, "Bas dalgalı geçiş"));
            g_assert_cmpstr(hint_preset(repeated, i), ==, name);
        }
        RadioSPlayoutRequest request = request_new(); GError *error = NULL;
        RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(output), &request, &error);
        g_assert_no_error(error); g_assert_nonnull(plan); g_assert_cmpuint(plan->entries->len, >=, 2);
        RadioSPlayoutEntry *first = g_ptr_array_index(plan->entries, 0);
        g_assert_true(first->run.shape.enabled); g_assert_cmpuint(first->run.mode, ==, RADIOS_AUDIO_RUN_CROSSFADE); g_assert_cmpuint(first->run.overlap_frames, >, 0);
        radios_playout_plan_free(plan);
        json_node_free(output); json_node_free(again); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
    }
    /* 2026-10-09 is a Friday (ISO 5): a Friday 08:00-10:00 row wins, a Saturday row is ignored. */
    const gint weekdays[] = {5, 6};
    for (guint d = 0; d < G_N_ELEMENTS(weekdays); ++d) {
        JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
        presets_enable(definition, "sequential", weekdays[d]);
        JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for(json_node_get_object(resolved));
        JsonNode *output = compiled(json_node_get_object(resolved), prepared, 12);
        JsonArray *tracks = json_object_get_array_member(json_node_get_object(output), "tracks");
        g_assert_cmpuint(json_array_get_length(tracks), >=, 4);
        for (guint i = 0; i + 1 < json_array_get_length(tracks); ++i)
            g_assert_cmpstr(hint_preset(tracks, i), ==, d == 0 ? "Ton erit" : (i % 2 ? "Bas dalgalı geçiş" : "Düz geçiş"));
        json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
    }
    /* An unknown preset reference or an invalid preset rejects the calendar. */
    for (guint which = 0; which < 2; ++which) {
        JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
        JsonArray *presets = presets_enable(definition, "sequential", 0);
        if (which == 0) {
            JsonObject *music = json_object_get_object_member(json_object_get_object_member(json_object_get_object_member(
                json_object_get_object_member(definition, "settings"), "autodj_automation"), "kinds"), "music");
            JsonArray *ids = json_array_new(); gchar *id = uuid(960); json_array_add_string_element(ids, id); g_free(id);
            json_object_set_array_member(music, "presets", ids);
        } else json_object_set_int_member(json_array_get_object_element(presets, 0), "duration_ms", 100);
        GError *error = NULL; JsonNode *bad = radios_automation_calendar_resolve_event(calendar, event, &error);
        g_assert_null(bad); g_assert_nonnull(error); g_clear_error(&error); json_object_unref(calendar);
    }
}


/* --- Kılavuz 4c: schema flow contract (automation_steps, link_transitions, programme_entry, live, date window) --- */
static JsonObject *schema_transition(gint mode, gint overlap)
{ JsonObject *t = json_object_new(); json_object_set_int_member(t, "mode", mode); json_object_set_int_member(t, "overlap_ms", overlap); return t; }
static JsonObject *schema_step(gint order, const gchar *kind)
{ JsonObject *s = json_object_new(); json_object_set_int_member(s, "order", order); json_object_set_string_member(s, "kind", kind); return s; }
static JsonArray *schema_steps(guint count, const gint *orders, const gchar *const *kinds)
{ JsonArray *steps = json_array_new(); for (guint i = 0; i < count; ++i) json_array_add_object_element(steps, schema_step(orders[i], kinds[i])); return steps; }
static JsonObject *live_object(const gchar *label, const gchar *end_rule, gint duration, guint schema)
{
    JsonObject *live = json_object_new(); JsonArray *channels = json_array_new(); json_array_add_int_element(channels, 3); json_array_add_int_element(channels, 4);
    json_object_set_string_member(live, "title", "Sabah sohbeti"); json_object_set_string_member(live, "label", label);
    json_object_set_array_member(live, "channels", channels);
    gchar *id = schema ? uuid(schema) : g_strdup(""); json_object_set_string_member(live, "automation_id", id); g_free(id);
    json_object_set_string_member(live, "end_rule", end_rule); json_object_set_int_member(live, "duration_ms", duration); return live;
}
/* Live step row of a definition: no playlist, the live object instead. */
static JsonObject *live_rule(guint number, const gchar *label, const gchar *end_rule, gint duration, guint schema)
{
    JsonObject *object = rule(number, 1); json_object_remove_member(object, "playlist_id");
    json_object_set_string_member(object, "source_type", "live"); json_object_set_object_member(object, "live", live_object(label, end_rule, duration, schema));
    return object;
}
static JsonObject *library_definition(JsonObject *calendar, guint id, const gchar *name, gboolean schema)
{
    JsonObject *definition = json_object_new(); gchar *identifier = uuid(id);
    json_object_set_string_member(definition, "id", identifier); g_free(identifier); json_object_set_string_member(definition, "name", name);
    JsonObject *settings = settings_new("linear"); if (schema) json_object_set_boolean_member(settings, "automation_schema", TRUE);
    json_object_set_object_member(definition, "settings", settings); json_object_set_array_member(definition, "sources", json_array_new());
    json_array_add_object_element(json_object_get_array_member(calendar, "rotation_library"), definition); return definition;
}
/* Active schema: A (steps 1,2) → live talk step → C (step 2); live event schema 701 holds the inner talk record. */
static JsonObject *schema_calendar(JsonObject **event, JsonObject **definition)
{
    JsonObject *calendar = calendar_new(event, definition); JsonObject *settings = json_object_get_object_member(*definition, "settings");
    json_object_set_boolean_member(settings, "automation_schema", TRUE);
    JsonArray *sources = json_object_get_array_member(*definition, "sources");
    JsonObject *a = json_array_get_object_element(sources, 0), *c = json_array_get_object_element(sources, 1);
    json_object_set_string_member(a, "start_date", "2026-10-01"); json_object_set_string_member(a, "end_date", "2026-10-31");
    const gint a_orders[] = {1, 2}; const gchar *const a_kinds[] = {"music", "music"};
    JsonObject *a_settings = json_object_new(); json_object_set_array_member(a_settings, "automation_steps", schema_steps(2, a_orders, a_kinds));
    json_object_set_object_member(a, "settings", a_settings);
    const gint c_orders[] = {2}; const gchar *const c_kinds[] = {"music"};
    JsonObject *c_settings = json_object_new(); json_object_set_array_member(c_settings, "automation_steps", schema_steps(1, c_orders, c_kinds));
    json_object_set_object_member(c, "settings", c_settings);
    json_array_add_object_element(sources, live_rule(3, "talk", "duration", 180000, 701));
    const gint orders[] = {1, 2, 3}; const gchar *const kinds[] = {"music", "music", "talk"};
    JsonArray *steps = schema_steps(3, orders, kinds);
    json_object_set_object_member(json_array_get_object_element(steps, 1), "transition", schema_transition(1, 2500));
    json_object_set_array_member(settings, "automation_steps", steps);
    JsonArray *links = json_array_new(); JsonObject *link = json_object_new(); gchar *from = uuid(1003), *to = uuid(1002);
    json_object_set_string_member(link, "from_source_id", from); json_object_set_string_member(link, "to_source_id", to); g_free(from); g_free(to);
    json_object_set_object_member(link, "transition", schema_transition(3, 4000)); json_array_add_object_element(links, link);
    json_object_set_array_member(settings, "link_transitions", links);
    JsonObject *entry = json_object_new(); json_object_set_int_member(entry, "source_order", 1); json_object_set_object_member(entry, "transition", schema_transition(1, 3000));
    JsonArray *alternatives = json_array_new(); JsonObject *alternative = json_object_new();
    json_object_set_int_member(alternative, "weight", 30); json_object_set_int_member(alternative, "source_order", 2); json_array_add_object_element(alternatives, alternative);
    json_object_set_array_member(entry, "alternatives", alternatives); json_object_set_object_member(settings, "programme_entry", entry);
    JsonObject *live_schema = library_definition(calendar, 701, "Konuşma şeması", TRUE);
    const gint inner_orders[] = {1, 2}; const gchar *const inner_kinds[] = {"music", "talk"};
    JsonArray *inner = schema_steps(2, inner_orders, inner_kinds);
    json_object_set_object_member(json_array_get_object_element(inner, 1), "transition", schema_transition(0, 0));
    json_object_set_array_member(json_object_get_object_member(live_schema, "settings"), "automation_steps", inner);
    JsonObject *flag_live = live_object("dj", "manual", 0, 701); json_object_remove_member(flag_live, "title");
    json_object_set_object_member(*event, "live", flag_live); json_object_set_int_member(*event, "schema_entry_source_order", 1);
    return calendar;
}
static void resolve_rejected(JsonObject *calendar, JsonObject *event, const gchar *fragment)
{
    GError *error = NULL; JsonNode *result = radios_automation_calendar_resolve_event(calendar, event, &error);
    g_assert_null(result); g_assert_nonnull(error);
    if (fragment) g_assert_nonnull(strstr(error->message, fragment));
    g_clear_error(&error);
}
static void test_schema_contract_validation_and_live_kind(void)
{
    g_assert_true(radios_programme_kind_valid("live"));
    g_assert_cmpstr(radios_programme_kind_label("live"), ==, "Canlı");
    g_assert_cmpstr(radios_programme_event_title("live"), ==, "Live event");
    JsonObject *event, *definition, *calendar = schema_calendar(&event, &definition);
    gchar *before = serialized(calendar); JsonNode *resolved = resolve(calendar, event); JsonObject *out = json_node_get_object(resolved);
    gchar *after = serialized(calendar); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
    JsonArray *rules = json_object_get_array_member(out, "rotations"); g_assert_cmpuint(json_array_get_length(rules), ==, 3);
    JsonObject *live = json_array_get_object_element(rules, 2);
    g_assert_cmpstr(json_object_get_string_member(live, "source_type"), ==, "live");
    g_assert_false(json_object_has_member(live, "playlist_id"));
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(live, "live"), "duration_ms"), ==, 180000);
    JsonObject *carried = json_object_get_object_member(live, "calendar_live_schema");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(carried, "automation_steps")), ==, 2);
    g_assert_cmpstr(json_object_get_string_member(at(carried, "automation_steps", 1), "kind"), ==, "talk");
    JsonObject *merged = json_object_get_object_member(json_array_get_object_element(rules, 0), "calendar_settings");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(merged, "automation_steps")), ==, 3);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_object_get_object_member(json_array_get_object_element(rules, 0), "settings"), "automation_steps")), ==, 2);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(merged, "link_transitions")), ==, 1);
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(merged, "programme_entry"), "source_order"), ==, 1);
    g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(rules, 0), "start_date"), ==, "2026-10-01");
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(out, "live"), "label"), ==, "dj");
    g_assert_cmpint(json_object_get_int_member(out, "schema_entry_source_order"), ==, 1);
    json_node_free(resolved);
    JsonObject *settings = json_object_get_object_member(definition, "settings");
    JsonArray *steps = json_object_get_array_member(settings, "automation_steps"), *sources = json_object_get_array_member(definition, "sources");
    JsonObject *live_rule_row = json_array_get_object_element(sources, 2), *live_value = json_object_get_object_member(live_rule_row, "live");
    /* Rejections mirror desktop_automation_calendar.py: unknown preset, duplicate
     * order, both choices, unknown link source, overweight alternatives, a live
     * row with a playlist, a non-schema live reference, a reversed date window,
     * a titled event flag, an unknown label, a duration rule without duration. */
    gchar *unknown = uuid(9999);
    json_object_set_string_member(json_array_get_object_element(steps, 0), "preset_id", unknown); resolve_rejected(calendar, event, NULL);
    json_object_remove_member(json_array_get_object_element(steps, 0), "preset_id");
    json_object_set_int_member(json_array_get_object_element(steps, 0), "order", 2); resolve_rejected(calendar, event, NULL);
    json_object_set_int_member(json_array_get_object_element(steps, 0), "order", 1);
    json_object_set_string_member(json_array_get_object_element(steps, 1), "preset_id", unknown); resolve_rejected(calendar, event, NULL);
    json_object_remove_member(json_array_get_object_element(steps, 1), "preset_id");
    JsonObject *link = at(settings, "link_transitions", 0); gchar *saved_to = g_strdup(json_object_get_string_member(link, "to_source_id"));
    json_object_set_string_member(link, "to_source_id", unknown); resolve_rejected(calendar, event, "Bağlantı geçişi");
    json_object_set_string_member(link, "to_source_id", saved_to); g_free(saved_to);
    json_object_set_int_member(at(json_object_get_object_member(settings, "programme_entry"), "alternatives", 0), "weight", 101); resolve_rejected(calendar, event, NULL);
    json_object_set_int_member(at(json_object_get_object_member(settings, "programme_entry"), "alternatives", 0), "weight", 30);
    gchar *playlist = uuid(2003); json_object_set_string_member(live_rule_row, "playlist_id", playlist); g_free(playlist); resolve_rejected(calendar, event, NULL);
    json_object_remove_member(live_rule_row, "playlist_id");
    gchar *plain = uuid(700); json_object_set_string_member(live_value, "automation_id", plain); g_free(plain);
    json_object_remove_member(settings, "automation_schema"); resolve_rejected(calendar, event, "Live event şeması");
    json_object_set_boolean_member(settings, "automation_schema", TRUE); gchar *schema_id = uuid(701); json_object_set_string_member(live_value, "automation_id", schema_id);
    json_object_set_string_member(json_array_get_object_element(sources, 0), "end_date", "2026-09-30"); resolve_rejected(calendar, event, NULL);
    json_object_set_string_member(json_array_get_object_element(sources, 0), "end_date", "2026-10-31");
    json_object_set_string_member(json_object_get_object_member(event, "live"), "title", "x"); resolve_rejected(calendar, event, "Live event ayarı");
    json_object_remove_member(json_object_get_object_member(event, "live"), "title");
    json_object_set_string_member(live_value, "label", "host"); resolve_rejected(calendar, event, NULL); json_object_set_string_member(live_value, "label", "talk");
    json_object_set_int_member(live_value, "duration_ms", 0); resolve_rejected(calendar, event, NULL); json_object_set_int_member(live_value, "duration_ms", 180000);
    json_object_set_int_member(event, "schema_entry_source_order", 51); resolve_rejected(calendar, event, "giriş sırası"); json_object_set_int_member(event, "schema_entry_source_order", 1);
    JsonNode *again = resolve(calendar, event); json_node_free(again);
    g_free(unknown); g_free(schema_id); json_object_unref(calendar);
}

/* --- Kılavuz 4c: the schema flow in the planner --- */
static JsonObject *prepared_for_media(JsonObject *resolved)
{
    JsonObject *prepared = prepared_new(); JsonArray *rules = json_object_get_array_member(resolved, "rotations"); guint media = 0;
    for (guint i = 0; i < json_array_get_length(rules); ++i) {
        JsonObject *rotation = json_array_get_object_element(rules, i), *source = json_object_new();
        if (!json_object_has_member(rotation, "playlist_id")) { json_object_unref(source); continue; }
        json_object_set_string_member(source, "source_id", json_object_get_string_member(rotation, "id"));
        json_object_set_string_member(source, "playlist_id", json_object_get_string_member(rotation, "playlist_id"));
        json_object_set_string_member(source, "source_name", "Gerçek seçki"); json_object_set_array_member(source, "reasons", json_array_new());
        JsonArray *tracks = json_array_new(); for (guint j = 0; j < 2; ++j) json_array_add_object_element(tracks, row(media * 2 + j + 1, "music"));
        json_object_set_array_member(source, "tracks", tracks); json_array_add_object_element(json_object_get_array_member(prepared, "sources"), source); ++media;
    }
    return prepared;
}
static JsonObject *track_at(JsonNode *node, guint index) { return at(json_node_get_object(node), "tracks", index); }
static JsonObject *hint_at(JsonNode *node, guint index) { return json_object_get_object_member(track_at(node, index), "calendar_transition"); }
static void expect_hint(JsonNode *node, guint index, gint mode, gint overlap)
{
    JsonObject *hint = hint_at(node, index); g_assert_nonnull(hint);
    g_assert_cmpint(json_object_get_int_member(hint, "mode"), ==, mode); g_assert_cmpint(json_object_get_int_member(hint, "overlap_ms"), ==, overlap);
}
static void expect_source(JsonNode *node, guint index, guint source)
{
    /* Runtime source IDs are scoped to an assignment; the fixture's playlist
     * identity verifies which saved source actually supplied the media. */
    gchar *id = uuid(source + 1000);
    g_assert_cmpstr(json_object_get_string_member(track_at(node, index), "calendar_playlist_id"), ==, id); g_free(id);
}
static void set_steps(JsonObject *holder, guint count, const gint *orders, const gchar *const *kinds)
{
    JsonObject *settings = json_object_has_member(holder, "settings") ? json_object_get_object_member(holder, "settings") : NULL;
    if (!settings) { settings = json_object_new(); json_object_set_object_member(holder, "settings", settings); }
    json_object_set_array_member(settings, "automation_steps", schema_steps(count, orders, kinds));
}
/* "2 2 2": A (1, 2) → B (2, 3) with the schema (1, 2, 3): order 2 is common
 * and music on all sides, so its schema transition rules the A→B and B→A
 * boundaries. A kind mismatch skips the order; without any common record the
 * link default A→B applies (here a designed preset), B→A keeps the rotation
 * rule. Deck pools differing per source change nothing in that chain. */
static void test_schema_two_two_two_link_default_and_cross_pool(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    JsonObject *settings = json_object_get_object_member(definition, "settings"); JsonArray *sources = json_object_get_array_member(definition, "sources");
    JsonObject *a = json_array_get_object_element(sources, 0), *b = json_array_get_object_element(sources, 1);
    json_object_set_boolean_member(settings, "automation_schema", TRUE);
    const gint schema_orders[] = {1, 2, 3}; const gchar *const schema_kinds[] = {"music", "music", "jingle"};
    JsonArray *steps = schema_steps(3, schema_orders, schema_kinds);
    json_object_set_object_member(json_array_get_object_element(steps, 0), "transition", schema_transition(0, 0));
    json_object_set_object_member(json_array_get_object_element(steps, 1), "transition", schema_transition(3, 2500));
    json_object_set_object_member(json_array_get_object_element(steps, 2), "transition", schema_transition(1, 500));
    json_object_set_array_member(settings, "automation_steps", steps);
    const gint a_orders[] = {1, 2}, b_orders[] = {2, 3}; const gchar *const a_kinds[] = {"music", "music"}, *const b_kinds[] = {"music", "music"};
    set_steps(a, 2, a_orders, a_kinds); set_steps(b, 2, b_orders, b_kinds);
    /* Source automations are separate definitions. The active schema's
     * 2500 ms beatmatch wins over A's own 700 ms transition at order 2. */
    json_object_set_object_member(at(json_object_get_object_member(a, "settings"), "automation_steps", 1), "transition", schema_transition(1, 700));
    JsonObject *a_routing = routing_new(), *b_routing = routing_new(); JsonArray *b_music = json_array_new();
    json_array_add_int_element(b_music, 3); json_array_add_int_element(b_music, 4); json_object_set_array_member(b_routing, "music", b_music);
    json_object_set_object_member(a, "autodj", a_routing); json_object_set_object_member(b, "autodj", b_routing);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for_media(json_node_get_object(resolved));
    JsonNode *out = compiled(json_node_get_object(resolved), prepared, 4);
    expect_source(out, 0, 1001); expect_source(out, 1, 1002); expect_source(out, 2, 1001); expect_source(out, 3, 1002);
    for (guint i = 0; i < 3; ++i) {
        expect_hint(out, i, 1, 2500); g_assert_true(json_object_get_boolean_member(hint_at(out, i), "beat_sync"));
        JsonObject *info = json_object_get_object_member(track_at(out, i), "schema_boundary");
        g_assert_cmpstr(json_object_get_string_member(info, "rule"), ==, "steps"); g_assert_cmpint(json_object_get_int_member(info, "order"), ==, 2);
    }
    g_assert_false(json_object_has_member(track_at(out, 3), "schema_boundary"));
    g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(json_object_get_object_member(track_at(out, 0), "calendar_routing"), "decks"), 0), ==, 1);
    g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(json_object_get_object_member(track_at(out, 1), "calendar_routing"), "decks"), 0), ==, 3);
    gint64 first = json_object_get_int_member(json_object_get_object_member(track_at(out, 0), "calendar_constraints"), "earliest_utc_us");
    g_assert_cmpint(first, ==, radios_automation_calendar_event_time(event, 480));
    /* These are the pools selected for two different clusters. The actual
     * general-broadcast plan must retain both groups and route each source's
     * next track through its own pool, not merely display JSON hints. */
    GError *plan_error = NULL; RadioSPlayoutRequest request = request_new();
    RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(out), &request, &plan_error);
    g_assert_no_error(plan_error); g_assert_nonnull(plan); g_assert_true(plan->routed_rotation);
    g_assert_cmpuint(plan->options.pool_count, ==, 4);
    const gint expected_decks[] = {0, 2, 31, 3};
    for (guint i = 0; i < G_N_ELEMENTS(expected_decks); ++i) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, i);
        g_assert_cmpint(entry->run.preferred_deck, ==, expected_decks[i]);
        g_assert_cmpuint(entry->routing_count, ==, 2);
        g_assert_cmpuint(entry->routing_decks[0], ==, i % 2 ? 2 : 0);
    }
    RadioSAudioRunItem *items = radios_playout_plan_items(plan);
    for (guint i = 0; i < G_N_ELEMENTS(expected_decks); ++i) g_assert_cmpint(items[i].preferred_deck, ==, expected_decks[i]);
    g_free(items); radios_playout_plan_free(plan);
    json_node_free(out);
    /* Order 2 on A becomes a jingle record: the type no longer fits, order 3 is
     * missing on A, so the boundary falls to the link default (a designed preset). */
    json_object_set_string_member(at(json_object_get_object_member(a, "settings"), "automation_steps", 1), "kind", "jingle");
    JsonArray *presets = radios_transition_preset_templates(); json_object_set_array_member(settings, "autodj_presets", presets);
    JsonArray *links = json_array_new(); JsonObject *link = json_object_new(); gchar *from = uuid(1001), *to = uuid(1002);
    json_object_set_string_member(link, "from_source_id", from); json_object_set_string_member(link, "to_source_id", to); g_free(from); g_free(to);
    json_object_set_string_member(link, "preset_id", json_object_get_string_member(json_array_get_object_element(presets, 0), "id"));
    json_array_add_object_element(links, link); json_object_set_array_member(settings, "link_transitions", links);
    json_node_free(resolved); resolved = resolve(calendar, event); json_object_unref(prepared); prepared = prepared_for_media(json_node_get_object(resolved));
    out = compiled(json_node_get_object(resolved), prepared, 4);
    JsonObject *hint = hint_at(out, 0); g_assert_cmpint(json_object_get_int_member(hint, "mode"), ==, 1);
    g_assert_cmpint(json_object_get_int_member(hint, "overlap_ms"), ==, json_object_get_int_member(json_array_get_object_element(presets, 0), "duration_ms"));
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(hint, "shape"), "name"), ==, json_object_get_string_member(json_array_get_object_element(presets, 0), "name"));
    JsonObject *info = json_object_get_object_member(track_at(out, 0), "schema_boundary");
    g_assert_cmpstr(json_object_get_string_member(info, "rule"), ==, "link"); g_assert_cmpint(json_object_get_int_member(info, "order"), ==, 0);
    expect_hint(out, 1, 0, 0); g_assert_false(json_object_has_member(track_at(out, 1), "schema_boundary"));
    /* Phase two keeps the matched boundary and rejects a forged one. */
    GError *error = NULL; JsonNode *merged = radios_automation_calendar_merge_selection(json_node_get_object(out), json_node_get_object(out), &error);
    g_assert_no_error(error); g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(track_at(merged, 0), "schema_boundary"), "rule"), ==, "link"); json_node_free(merged);
    json_object_set_string_member(info, "rule", "magic");
    merged = radios_automation_calendar_merge_selection(json_node_get_object(out), json_node_get_object(out), &error);
    g_assert_null(merged); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    json_node_free(out); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
/* Kılavuz §02: the playlist type decides which automation a link may use.
 * A preset assigned to jingles cannot rule a music → music link (the link is
 * skipped and the rotation's own rule applies); assigned to music, or to no
 * kind at all, the same link applies. */
static void test_schema_link_preset_follows_playlist_kind(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    JsonObject *settings = json_object_get_object_member(definition, "settings");
    json_object_set_boolean_member(settings, "automation_schema", TRUE);
    JsonArray *presets = radios_transition_preset_templates(); json_object_set_array_member(settings, "autodj_presets", presets);
    gchar *preset = g_strdup(json_object_get_string_member(json_array_get_object_element(presets, 0), "id"));
    gchar *other = g_strdup(json_object_get_string_member(json_array_get_object_element(presets, 1), "id"));
    JsonArray *links = json_array_new(); JsonObject *link = json_object_new(); gchar *from = uuid(1001), *to = uuid(1002);
    json_object_set_string_member(link, "from_source_id", from); json_object_set_string_member(link, "to_source_id", to); g_free(from); g_free(to);
    json_object_set_string_member(link, "preset_id", preset); json_array_add_object_element(links, link);
    json_object_set_array_member(settings, "link_transitions", links);
    JsonObject *automation = json_object_new(), *kinds = json_object_new(); json_object_set_boolean_member(automation, "enabled", TRUE);
    json_object_set_object_member(kinds, "jingle", preset_assignment(preset_ids(presets, 0, 1), "sequential"));
    json_object_set_object_member(automation, "kinds", kinds); json_object_set_object_member(settings, "autodj_automation", automation);
    g_assert_false(radios_automation_preset_kind_usable(settings, preset, "music"));
    g_assert_true(radios_automation_preset_kind_usable(settings, preset, "jingle"));
    g_assert_true(radios_automation_preset_kind_usable(settings, other, "music"));
    g_assert_true(radios_automation_preset_kind_usable(settings, preset, NULL));
    g_assert_false(radios_automation_preset_kind_usable(settings, preset, "talk"));
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for_media(json_node_get_object(resolved));
    JsonNode *out = compiled(json_node_get_object(resolved), prepared, 4);
    expect_source(out, 0, 1001); expect_source(out, 1, 1002);
    g_assert_false(json_object_has_member(track_at(out, 0), "schema_boundary"));
    JsonObject *hint = hint_at(out, 0); g_assert_true(!hint || !json_object_has_member(hint, "shape"));
    json_node_free(out); json_node_free(resolved); json_object_unref(prepared);
    /* Assigned to music, the link rules the music boundary. */
    json_object_set_object_member(kinds, "music", preset_assignment(preset_ids(presets, 0, 1), "sequential"));
    resolved = resolve(calendar, event); prepared = prepared_for_media(json_node_get_object(resolved));
    out = compiled(json_node_get_object(resolved), prepared, 4);
    JsonObject *info = json_object_get_object_member(track_at(out, 0), "schema_boundary"); g_assert_nonnull(info);
    g_assert_cmpstr(json_object_get_string_member(info, "rule"), ==, "link");
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(hint_at(out, 0), "shape"), "id"), ==, preset);
    json_node_free(out); json_node_free(resolved); json_object_unref(prepared);
    /* Assigned to no kind, it serves every boundary. */
    json_object_remove_member(kinds, "jingle"); json_object_remove_member(kinds, "music");
    g_assert_true(radios_automation_preset_kind_usable(settings, preset, "talk"));
    resolved = resolve(calendar, event); prepared = prepared_for_media(json_node_get_object(resolved));
    out = compiled(json_node_get_object(resolved), prepared, 4);
    info = json_object_get_object_member(track_at(out, 0), "schema_boundary"); g_assert_nonnull(info);
    g_assert_cmpstr(json_object_get_string_member(info, "rule"), ==, "link");
    json_node_free(out); json_node_free(resolved); json_object_unref(prepared);
    g_free(preset); g_free(other); json_object_unref(calendar);
}
/* Kılavuz §03 "Kümeler arası geçiş": A plays on cluster {1,2}, B on cluster
 * {3,4}; the A → B link carries a 1.5 s crossfade. The native plan hands the
 * flow from A's players to B's with that crossfade (B starts on the other
 * cluster's player while A fades) and returns by the rotation's own rule. */
static void test_schema_cluster_handover_native_plan(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    JsonObject *settings = json_object_get_object_member(definition, "settings"); JsonArray *sources = json_object_get_array_member(definition, "sources");
    json_object_set_boolean_member(settings, "automation_schema", TRUE);
    const gint64 pools[2][2] = {{1, 2}, {3, 4}}; const gchar *kinds[] = {"music", "advertisement", "jingle", "sweeper"};
    for (guint s = 0; s < 2; ++s) {
        JsonObject *routing = json_object_new();
        for (guint k = 0; k < G_N_ELEMENTS(kinds); ++k) {
            JsonArray *decks = json_array_new(); json_array_add_int_element(decks, pools[s][0]); json_array_add_int_element(decks, pools[s][1]);
            json_object_set_array_member(routing, kinds[k], decks);
        }
        json_object_set_object_member(json_array_get_object_element(sources, s), "autodj", routing);
    }
    JsonArray *links = json_array_new(); JsonObject *link = json_object_new(); gchar *from = uuid(1001), *to = uuid(1002);
    json_object_set_string_member(link, "from_source_id", from); json_object_set_string_member(link, "to_source_id", to); g_free(from); g_free(to);
    json_object_set_object_member(link, "transition", schema_transition(1, 1500)); json_array_add_object_element(links, link);
    json_object_set_array_member(settings, "link_transitions", links);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for_media(json_node_get_object(resolved));
    JsonNode *out = compiled(json_node_get_object(resolved), prepared, 4); JsonObject *root = json_node_get_object(out);
    expect_source(out, 0, 1001); expect_source(out, 1, 1002); expect_source(out, 2, 1001); expect_source(out, 3, 1002);
    expect_hint(out, 0, 1, 1500);
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(track_at(out, 0), "schema_boundary"), "rule"), ==, "link");
    gint64 start = radios_automation_calendar_event_time(event, 480); GError *error = NULL;
    RadioSPlayoutRequest request = {.station_id = station, .database_file = database, .now_utc_us = start, .start_utc_us = start};
    RadioSPlayoutPlan *plan = radios_playout_plan_build(root, &request, &error); g_assert_no_error(error); g_assert_nonnull(plan);
    g_assert_cmpuint(plan->entries->len, ==, 4); g_assert_true(plan->routed_rotation);
    for (guint i = 0; i < 4; ++i) {
        gint deck = ((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, i))->run.preferred_deck;
        /* engine decks are zero-based: cluster {1,2} → {0,1}, cluster {3,4} → {2,3} */
        if (i % 2) g_assert_true(deck == 2 || deck == 3); else g_assert_true(deck == 0 || deck == 1);
    }
    /* The crossfade into B's cluster is kept (clipped to the media's cue
     * geometry, never longer than requested); B → A follows the rotation rule. */
    RadioSPlayoutEntry *first = g_ptr_array_index(plan->entries, 0), *second = g_ptr_array_index(plan->entries, 1);
    g_assert_cmpuint(first->run.mode, ==, RADIOS_AUDIO_RUN_CROSSFADE);
    g_assert_cmpuint(first->run.overlap_frames, >, 0); g_assert_cmpuint(first->run.overlap_frames, <=, 1500 * 48);
    g_assert_cmpuint(second->run.overlap_frames, ==, 0);
    radios_playout_plan_free(plan);
    json_node_free(out); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
/* Live step (kılavuz §04): A → talk → C. The step plays nothing; a live block
 * precedes C's block, the row before carries live_step (outer order 2, inner
 * record 2 of the live event schema) and the inner record's automation as its
 * boundary hint. Natively A ends plainly into the step (MANUAL wait): with a
 * duration rule C is planned at the step's end and the wait counts from A's
 * actual end (no absolute anchor, PDF s.15); a manual rule waits for the operator. */
static void test_live_step_duration_manual_and_native_wait(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    JsonObject *settings = json_object_get_object_member(definition, "settings"); JsonArray *sources = json_object_get_array_member(definition, "sources");
    json_object_set_boolean_member(settings, "automation_schema", TRUE);
    JsonObject *c = json_object_ref(json_array_get_object_element(sources, 1)); json_array_remove_element(sources, 1);
    json_array_add_object_element(sources, live_rule(3, "talk", "duration", 180000, 701)); json_array_add_object_element(sources, c);
    const gint orders[] = {1, 2, 3}; const gchar *const kinds[] = {"music", "music", "talk"};
    json_object_set_array_member(settings, "automation_steps", schema_steps(3, orders, kinds));
    JsonObject *live_schema = library_definition(calendar, 701, "Konuşma şeması", TRUE);
    const gint inner_orders[] = {1, 2}; const gchar *const inner_kinds[] = {"music", "talk"};
    JsonArray *inner = schema_steps(2, inner_orders, inner_kinds);
    json_object_set_object_member(json_array_get_object_element(inner, 1), "transition", schema_transition(1, 1500));
    JsonArray *inner_channels = json_array_new(); json_array_add_int_element(inner_channels, 12); json_array_add_int_element(inner_channels, 32);
    json_object_set_array_member(json_array_get_object_element(inner, 1), "channels", inner_channels);
    json_object_set_array_member(json_object_get_object_member(live_schema, "settings"), "automation_steps", inner);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for_media(json_node_get_object(resolved));
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(prepared, "sources")), ==, 2);
    JsonNode *out = compiled(json_node_get_object(resolved), prepared, 3); JsonObject *root = json_node_get_object(out);
    JsonObject *config = json_object_get_object_member(root, "config"); JsonArray *blocks = json_object_get_array_member(config, "blocks");
    expect_source(out, 0, 1001); expect_source(out, 1, 1002); expect_source(out, 2, 1001);
    /* A → live → C → A has four distinct broadcast blocks: returning to A
     * must not merge its second turn into C's block. */
    g_assert_cmpuint(json_array_get_length(blocks), ==, 4);
    JsonObject *live_block = json_array_get_object_element(blocks, 1);
    g_assert_cmpstr(json_object_get_string_member(live_block, "kind"), ==, "live");
    g_assert_cmpstr(json_object_get_string_member(live_block, "title"), ==, "Sabah sohbeti");
    JsonObject *live = json_object_get_object_member(live_block, "live"); gchar *schema_id = uuid(701);
    g_assert_cmpstr(json_object_get_string_member(live, "label"), ==, "talk"); g_assert_cmpstr(json_object_get_string_member(live, "automation_id"), ==, schema_id);
    g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(live, "channels"), 0), ==, 12);
    g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(live, "channels"), 1), ==, 32);
    g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(json_object_get_object_member(json_array_get_object_element(sources, 1), "live"), "channels"), 1), ==, 4);
    g_assert_cmpint(json_object_get_int_member(live, "duration_ms"), ==, 180000); g_assert_false(json_object_has_member(live, "title"));
    JsonObject *step = json_object_get_object_member(track_at(out, 0), "live_step");
    g_assert_cmpstr(json_object_get_string_member(step, "block_id"), ==, json_object_get_string_member(live_block, "id"));
    g_assert_cmpint(json_object_get_int_member(step, "outer_order"), ==, 2); g_assert_cmpint(json_object_get_int_member(step, "inner_order"), ==, 2);
    g_assert_cmpstr(json_object_get_string_member(step, "end_rule"), ==, "duration");
    g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(step, "channels"), 1), ==, 32);
    expect_hint(out, 0, 1, 1500); /* the inner talk record's automation enters the talk */
    gint64 start = radios_automation_calendar_event_time(event, 480), live_end = start + G_USEC_PER_SEC + 180 * G_USEC_PER_SEC;
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(track_at(out, 1), "calendar_constraints"), "earliest_utc_us"), ==, start);
    g_assert_cmpstr(json_object_get_string_member(at(config, "events", 1), "block_id"), ==, json_object_get_string_member(json_array_get_object_element(blocks, 2), "id"));
    g_assert_cmpstr(json_object_get_string_member(at(config, "events", 2), "block_id"), ==, json_object_get_string_member(json_array_get_object_element(blocks, 3), "id"));
    g_assert_true(has_diagnostic(out, "calendar_live_step"));
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(config, "events")), ==, 3);
    GError *error = NULL; RadioSPlayoutRequest request = {.station_id = station, .database_file = database, .now_utc_us = start, .start_utc_us = start};
    RadioSPlayoutPlan *plan = radios_playout_plan_build(root, &request, &error); g_assert_no_error(error); g_assert_nonnull(plan);
    RadioSPlayoutEntry *second = g_ptr_array_index(plan->entries, 1);
    g_assert_cmpint(second->planned_utc_us, ==, live_end); g_assert_cmpint(second->hold_us, ==, 180 * G_USEC_PER_SEC);
    g_assert_cmpint(second->run.start_utc_us, ==, start);
    g_assert_cmpint(((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, 0))->run.mode, ==, RADIOS_AUDIO_RUN_MANUAL);
    g_assert_cmpuint(((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, 0))->run.overlap_frames, ==, 0);
    RadioSPlayoutEntry *first_entry = g_ptr_array_index(plan->entries, 0);
    JsonObject *display_live = json_object_get_object_member(first_entry->binding, "live_step");
    g_assert_nonnull(display_live);
    g_assert_cmpint(json_object_get_int_member(display_live, "outer_order"), ==, 2);
    g_assert_cmpint(json_object_get_int_member(display_live, "inner_order"), ==, 2);
    g_assert_cmpstr(json_object_get_string_member(display_live, "end_rule"), ==, "duration");
    radios_playout_plan_free(plan);
    JsonNode *merged = radios_automation_calendar_merge_selection(root, root, &error); g_assert_no_error(error);
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(track_at(merged, 0), "live_step"), "outer_order"), ==, 2); json_node_free(merged);
    json_object_set_int_member(step, "outer_order", 0);
    merged = radios_automation_calendar_merge_selection(root, root, &error); g_assert_null(merged); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    json_node_free(out);
    /* Manual end rule: the row before the talk holds (native MANUAL), C follows the operator. */
    JsonObject *live_value = json_object_get_object_member(json_array_get_object_element(sources, 1), "live");
    json_object_set_string_member(live_value, "end_rule", "manual"); json_object_set_int_member(live_value, "duration_ms", 0);
    json_node_free(resolved); resolved = resolve(calendar, event); json_object_unref(prepared); prepared = prepared_for_media(json_node_get_object(resolved));
    out = compiled(json_node_get_object(resolved), prepared, 3); root = json_node_get_object(out);
    expect_hint(out, 0, 3, 0); g_assert_cmpuint(json_object_get_size(hint_at(out, 0)), ==, 2);
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(track_at(out, 1), "calendar_constraints"), "earliest_utc_us"), ==, start);
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(track_at(out, 0), "live_step"), "end_rule"), ==, "manual");
    plan = radios_playout_plan_build(root, &request, &error); g_assert_no_error(error); g_assert_nonnull(plan);
    g_assert_cmpint(((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, 0))->run.mode, ==, RADIOS_AUDIO_RUN_MANUAL);
    g_assert_cmpint(((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, 1))->planned_utc_us, ==, start + G_USEC_PER_SEC);
    radios_playout_plan_free(plan);
    merged = radios_automation_calendar_merge_selection(root, root, &error); g_assert_no_error(error); expect_hint(merged, 0, 3, 0); json_node_free(merged);
    json_node_free(out); json_node_free(resolved); json_object_unref(prepared); g_free(schema_id); json_object_unref(calendar);
}
/* programme_entry (kılavuz §03): the schema names the source order and the
 * transition the next programme enters with; alternatives are its probability
 * path (seeded, reproducible). The scope driver's runtime order wins. */
static void test_programme_entry_runtime_and_seeded_alternatives(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    JsonObject *settings = json_object_get_object_member(definition, "settings"); json_object_set_boolean_member(settings, "automation_schema", TRUE);
    JsonObject *entry = json_object_new(); json_object_set_int_member(entry, "source_order", 2); json_object_set_object_member(entry, "transition", schema_transition(1, 3000));
    JsonArray *alternatives = json_array_new(); JsonObject *alternative = json_object_new();
    json_object_set_int_member(alternative, "weight", 100); json_object_set_int_member(alternative, "source_order", 1);
    json_object_set_object_member(alternative, "transition", schema_transition(2, 0)); json_array_add_object_element(alternatives, alternative);
    json_object_set_array_member(entry, "alternatives", alternatives); json_object_set_object_member(settings, "programme_entry", entry);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for_media(json_node_get_object(resolved));
    JsonNode *out = compiled(json_node_get_object(resolved), prepared, 2), *again = compiled(json_node_get_object(resolved), prepared, 2);
    JsonObject *hint = json_object_get_object_member(track_at(out, 0), "programme_entry");
    g_assert_cmpint(json_object_get_int_member(hint, "source_order"), ==, 1); g_assert_cmpint(json_object_get_int_member(hint, "alternative_index"), ==, 0);
    g_assert_true(json_object_get_boolean_member(hint, "applied")); expect_source(out, 0, 1001); expect_hint(out, 0, 2, 0);
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(hint, "transition"), "mode"), ==, 2);
    gchar *first = serialized(hint), *second = serialized(json_object_get_object_member(track_at(again, 0), "programme_entry"));
    g_assert_cmpstr(first, ==, second); g_free(first); g_free(second); expect_source(again, 0, 1001);
    g_assert_false(json_object_has_member(track_at(out, 1), "programme_entry")); json_node_free(out); json_node_free(again);
    /* Weight zero never draws the alternative: the main entry (order 2, crossfade 3 s). */
    json_object_set_int_member(alternative, "weight", 0);
    json_node_free(resolved); resolved = resolve(calendar, event); out = compiled(json_node_get_object(resolved), prepared, 2);
    hint = json_object_get_object_member(track_at(out, 0), "programme_entry");
    g_assert_cmpint(json_object_get_int_member(hint, "source_order"), ==, 2); g_assert_cmpint(json_object_get_int_member(hint, "alternative_index"), ==, -1);
    g_assert_true(json_object_get_boolean_member(hint, "applied")); expect_source(out, 0, 1002); expect_hint(out, 0, 1, 3000); json_node_free(out);
    /* The runtime entry order of the scope driver: order 1 is the alternative's order. */
    json_object_set_int_member(json_node_get_object(resolved), "schema_entry_source_order", 1);
    out = compiled(json_node_get_object(resolved), prepared, 2); hint = json_object_get_object_member(track_at(out, 0), "programme_entry");
    g_assert_cmpint(json_object_get_int_member(hint, "source_order"), ==, 1); g_assert_cmpint(json_object_get_int_member(hint, "alternative_index"), ==, 0);
    expect_source(out, 0, 1001); expect_hint(out, 0, 2, 0);
    GError *error = NULL; JsonNode *merged = radios_automation_calendar_merge_selection(json_node_get_object(out), json_node_get_object(out), &error);
    g_assert_no_error(error); g_assert_true(json_object_has_member(track_at(merged, 0), "programme_entry")); json_node_free(merged); json_node_free(out);
    /* An alternate path without its own transition still uses the main
     * programme link's assigned automation; it must not drop to cut. */
    json_object_remove_member(alternative, "transition");
    json_object_set_int_member(alternative, "weight", 100);
    json_node_free(resolved); resolved = resolve(calendar, event);
    out = compiled(json_node_get_object(resolved), prepared, 2);
    hint = json_object_get_object_member(track_at(out, 0), "programme_entry");
    g_assert_cmpint(json_object_get_int_member(hint, "alternative_index"), ==, 0);
    expect_source(out, 0, 1001); expect_hint(out, 0, 1, 3000);
    json_node_free(out);
    /* An order no source has: the ordinary selection starts, the hint says so. */
    json_object_set_int_member(json_node_get_object(resolved), "schema_entry_source_order", 7);
    out = compiled(json_node_get_object(resolved), prepared, 2); hint = json_object_get_object_member(track_at(out, 0), "programme_entry");
    g_assert_cmpint(json_object_get_int_member(hint, "source_order"), ==, 7); g_assert_false(json_object_get_boolean_member(hint, "applied"));
    g_assert_cmpint(json_object_get_int_member(hint, "alternative_index"), ==, -1); expect_source(out, 0, 1001);
    json_node_free(out); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
/* Date window (kılavuz §04 step 3) and the programme's Live event flag. */
static void test_date_window_excludes_source_and_event_live_default(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    JsonArray *sources = json_object_get_array_member(definition, "sources");
    JsonObject *a = json_array_get_object_element(sources, 0), *b = json_array_get_object_element(sources, 1);
    json_object_set_string_member(a, "start_date", "2026-10-10"); json_object_set_string_member(a, "end_date", "2026-12-31");
    json_object_set_string_member(b, "start_date", "2026-10-01");
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for_media(json_node_get_object(resolved));
    JsonNode *out = compiled(json_node_get_object(resolved), prepared, 4);
    for (guint i = 0; i < 4; ++i) expect_source(out, i, 1002);
    g_assert_true(has_diagnostic(out, "calendar_source_date_window"));
    JsonArray *diagnostics = json_object_get_array_member(json_node_get_object(out), "diagnostics"); gboolean named_a = FALSE;
    const gchar *a_id = json_object_get_string_member(at(json_node_get_object(resolved), "rotations", 0), "id");
    for (guint i = 0; i < json_array_get_length(diagnostics); ++i) {
        JsonObject *row = json_array_get_object_element(diagnostics, i);
        named_a |= g_str_equal(json_object_get_string_member(row, "reason"), "calendar_source_date_window") && g_str_equal(json_object_get_string_member(row, "source_id"), a_id);
    }
    g_assert_true(named_a); json_node_free(out);
    /* Every playlist outside its window: a precise rejection, not silence. */
    json_object_set_string_member(b, "end_date", "2026-10-08");
    json_node_free(resolved); resolved = resolve(calendar, event); GError *error = NULL;
    out = radios_automation_calendar_prepared_programme(json_node_get_object(resolved), prepared, station, "named-fixed", 4, &error);
    g_assert_null(out); g_assert_nonnull(error); g_assert_nonnull(strstr(error->message, "tarih")); g_clear_error(&error);
    json_object_remove_member(b, "end_date"); json_object_remove_member(a, "start_date"); json_object_remove_member(a, "end_date");
    /* A Live event programme without a live step: every block carries its live binding. */
    JsonObject *flag_live = live_object("dj", "manual", 0, 0); json_object_remove_member(flag_live, "title");
    json_object_set_object_member(event, "live", flag_live);
    json_node_free(resolved); resolved = resolve(calendar, event); out = compiled(json_node_get_object(resolved), prepared, 3);
    JsonArray *blocks = json_object_get_array_member(json_object_get_object_member(json_node_get_object(out), "config"), "blocks");
    g_assert_cmpuint(json_array_get_length(blocks), >=, 1);
    for (guint i = 0; i < json_array_get_length(blocks); ++i) {
        JsonObject *live = json_object_get_object_member(json_array_get_object_element(blocks, i), "live");
        g_assert_cmpstr(json_object_get_string_member(live, "label"), ==, "dj"); g_assert_cmpstr(json_object_get_string_member(live, "end_rule"), ==, "manual");
        g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(blocks, i), "kind"), ==, "music");
    }
    json_node_free(out); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
static void test_runtime_programme_handoff_keeps_alternatives_and_presets(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    JsonObject *own = json_object_new(); json_object_set_int_member(own, "source_order", 1);
    json_object_set_object_member(own, "transition", schema_transition(0, 0));
    json_object_set_object_member(json_object_get_object_member(definition, "settings"), "programme_entry", own);
    JsonNode *resolved = resolve(calendar, event); JsonObject *runtime = json_node_get_object(resolved);
    JsonObject *settings = json_object_new(), *entry = json_object_new(), *alternative = json_object_new();
    JsonArray *presets = radios_transition_preset_templates(), *alternatives = json_array_new();
    JsonObject *preset = json_array_get_object_element(presets, 0);
    json_object_set_int_member(entry, "source_order", 1);
    json_object_set_string_member(entry, "preset_id", json_object_get_string_member(preset, "id"));
    json_object_set_int_member(alternative, "source_order", 2); json_object_set_int_member(alternative, "weight", 100);
    json_array_add_object_element(alternatives, alternative); json_object_set_array_member(entry, "alternatives", alternatives);
    json_object_set_object_member(settings, "programme_entry", entry); json_object_set_array_member(settings, "autodj_presets", presets);
    json_object_set_object_member(runtime, "schema_entry_settings", settings);
    JsonObject *prepared = prepared_for_media(runtime); JsonNode *out = compiled(runtime, prepared, 2);
    expect_source(out, 0, 1002); JsonObject *hint = json_object_get_object_member(track_at(out, 0), "programme_entry");
    g_assert_cmpint(json_object_get_int_member(hint, "alternative_index"), ==, 0);
    g_assert_cmpint(json_object_get_int_member(hint, "source_order"), ==, 2);
    expect_hint(out, 0, 1, (gint)json_object_get_int_member(preset, "duration_ms"));
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(hint_at(out, 0), "shape"), "id"), ==,
        json_object_get_string_member(preset, "id"));
    GError *error = NULL; JsonNode *merged = radios_automation_calendar_merge_selection(json_node_get_object(out), json_node_get_object(out), &error);
    g_assert_no_error(error); g_assert_nonnull(merged); json_node_free(merged); json_node_free(out);
    /* No weighted route remains: the carried link's base order and preset
     * still win over the destination schema's unrelated cut transition. */
    json_object_set_int_member(alternative, "weight", 0);
    out = compiled(runtime, prepared, 2); expect_source(out, 0, 1001);
    expect_hint(out, 0, 1, (gint)json_object_get_int_member(preset, "duration_ms")); json_node_free(out);
    json_object_set_string_member(entry, "preset_id", "00000000-0000-4000-8000-000000009999");
    out = radios_automation_calendar_prepared_programme(runtime, prepared, station, "named-fixed", 2, &error);
    g_assert_null(out); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    g_assert_false(json_object_has_member(event, "schema_entry_settings"));
    json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
/* Continuity across midnight and across back-to-back playlist windows. */
static JsonObject *prepared_minutes(JsonNode *resolved, gint64 duration_ms)
{
    JsonObject *prepared = prepared_for(json_node_get_object(resolved));
    for (guint s = 0; s < json_array_get_length(json_object_get_array_member(prepared, "sources")); ++s)
        for (guint i = 0; i < 2; ++i)
            json_object_set_int_member(json_object_get_object_member(json_object_get_object_member(at(at(prepared, "sources", s), "tracks", i), "detail"), "version"), "duration_ms", duration_ms);
    return prepared;
}
static JsonNode *plan_from(JsonNode *resolved, JsonObject *prepared, const gchar *stamp, GError **error)
{ return radios_automation_calendar_prepared_programme_at(json_node_get_object(resolved), prepared, station, "continuity", 200, actual_time(stamp), error); }
static guint track_count(JsonNode *node) { return json_array_get_length(json_object_get_array_member(json_node_get_object(node), "tracks")); }
/* 24:00 is the day end, not an explicit window end: a block ending at midnight
 * keeps its opted-in soft end. A cold start at 23:59:30 still plans music past
 * midnight; the merge keeps its bound (a whole-day block then spans more than
 * 24 h) and native playout accepts it. Without a soft end, or at explicit
 * playlist windows ending with the block, the end stays strict. */
static void test_named_soft_end_crosses_midnight(void)
{
    for (guint whole_day = 0; whole_day < 2; ++whole_day) {
        JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
        JsonObject *settings = json_object_get_object_member(definition, "settings");
        json_object_set_int_member(settings, "soft_end_seconds", 300);
        json_object_set_int_member(event, "start_minute", whole_day ? 0 : 1380); json_object_set_int_member(event, "end_minute", 1440);
        json_object_set_int_member(at(definition, "sources", 1), "end_minute", 1440); /* an explicit 24:00 is the day end too */
        JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_minutes(resolved, 180000); GError *error = NULL;
        gint64 now = actual_time("2026-10-09T23:59:30Z"), midnight = actual_time("2026-10-10T00:00:00Z");
        JsonNode *output = plan_from(resolved, prepared, "2026-10-09T23:59:30Z", &error); g_assert_no_error(error); g_assert_nonnull(output);
        g_assert_cmpuint(track_count(output), ==, 1); g_assert_cmpstr(track_kind(output, 0), ==, "music");
        g_assert_true(has_diagnostic(output, "calendar_soft_end_overrun"));
        gint64 latest = json_object_get_int_member(json_object_get_object_member(track_at(output, 0), "calendar_constraints"), "latest_end_utc_us");
        g_assert_cmpint(latest, >=, now + 180 * G_USEC_PER_SEC); g_assert_cmpint(latest, >, midnight);
        JsonNode *fresh = copy_node(json_node_get_object(output)); json_object_remove_member(track_at(fresh, 0), "calendar_constraints");
        JsonNode *merged = radios_automation_calendar_merge_selection(json_node_get_object(fresh), json_node_get_object(output), &error);
        g_assert_no_error(error); g_assert_nonnull(merged);
        g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(track_at(merged, 0), "calendar_constraints"), "latest_end_utc_us"), ==, latest);
        RadioSPlayoutRequest request = request_new(); request.now_utc_us = now;
        RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(merged), &request, &error); g_assert_no_error(error); g_assert_nonnull(plan);
        g_assert_cmpuint(plan->entries->len, ==, 1); RadioSPlayoutEntry *first = g_ptr_array_index(plan->entries, 0);
        g_assert_cmpint(first->planned_utc_us, ==, now); g_assert_cmpint(first->run.latest_start_utc_us, >=, now);
        radios_playout_plan_free(plan); json_node_free(merged); json_node_free(fresh); json_node_free(output); json_node_free(resolved);
        json_object_remove_member(settings, "soft_end_seconds"); resolved = resolve(calendar, event);
        g_assert_null(plan_from(resolved, prepared, "2026-10-09T23:59:30Z", &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK); g_clear_error(&error);
        json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
    }
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    json_object_set_int_member(json_object_get_object_member(definition, "settings"), "soft_end_seconds", 300);
    json_object_set_int_member(event, "start_minute", 1320); json_object_set_int_member(event, "end_minute", 1380);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_minutes(resolved, 180000); GError *error = NULL;
    JsonNode *output = plan_from(resolved, prepared, "2026-10-09T22:59:30Z", &error); g_assert_no_error(error); g_assert_nonnull(output);
    g_assert_cmpuint(track_count(output), ==, 1); g_assert_true(has_diagnostic(output, "calendar_soft_end_overrun"));
    json_node_free(output); json_node_free(resolved);
    for (guint i = 0; i < 2; ++i) json_object_set_int_member(at(definition, "sources", i), "end_minute", 1380);
    resolved = resolve(calendar, event);
    g_assert_null(plan_from(resolved, prepared, "2026-10-09T22:59:30Z", &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK); g_clear_error(&error);
    json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
/* Back-to-back playlist windows in one block (A 10–11, then B 11–12): when A
 * has nothing left that fits, the selection waits for B's window instead of
 * ending the plan. B's first row starts at 11:00 and carries
 * calendar_window_wait; the merge keeps it and native playout starts that row
 * at its window (without the mark the whole plan fails). A fitting short item
 * fills the gap first; the programme start itself never waits. */
static void test_named_window_wait_next_source_window(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    json_object_set_int_member(event, "start_minute", 600); json_object_set_int_member(event, "end_minute", 720);
    JsonObject *a = at(definition, "sources", 0), *b = at(definition, "sources", 1);
    json_object_set_int_member(a, "start_minute", 600); json_object_set_int_member(a, "end_minute", 660);
    json_object_set_int_member(b, "start_minute", 660); json_object_set_int_member(b, "end_minute", 720);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_minutes(resolved, 240000); GError *error = NULL;
    JsonNode *output = plan_from(resolved, prepared, "2026-10-09T10:05:00Z", &error); g_assert_no_error(error); g_assert_nonnull(output);
    /* A plays 10:05–10:57 (13 rows), B 11:00–12:00 (15 rows). */
    guint n = track_count(output); g_assert_cmpuint(n, ==, 28);
    gchar *b_one = uuid(10003), *b_two = uuid(10004);
    for (guint i = 0; i < n; ++i) {
        gboolean from_b = g_str_equal(track_media(output, i), b_one) || g_str_equal(track_media(output, i), b_two);
        g_assert_cmpint(from_b, ==, i >= 13);
        g_assert_cmpint(json_object_has_member(track_at(output, i), "calendar_window_wait"), ==, i == 13);
    }
    g_free(b_one); g_free(b_two);
    JsonObject *waited = track_at(output, 13);
    g_assert_true(json_object_get_boolean_member(waited, "calendar_window_wait")); g_assert_true(has_diagnostic(output, "calendar_window_wait"));
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(waited, "calendar_constraints"), "earliest_utc_us"), ==, actual_time("2026-10-09T11:00:00Z"));
    JsonNode *fresh = copy_node(json_node_get_object(output));
    for (guint i = 0; i < n; ++i) { json_object_remove_member(track_at(fresh, i), "calendar_constraints"); json_object_remove_member(track_at(fresh, i), "calendar_window_wait"); }
    JsonNode *merged = radios_automation_calendar_merge_selection(json_node_get_object(fresh), json_node_get_object(output), &error);
    g_assert_no_error(error); g_assert_nonnull(merged); g_assert_true(json_object_get_boolean_member(track_at(merged, 13), "calendar_window_wait"));
    gint64 start = actual_time("2026-10-09T10:05:00Z");
    RadioSPlayoutRequest request = {.station_id = station, .database_file = database, .now_utc_us = start, .start_utc_us = start};
    RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(merged), &request, &error); g_assert_no_error(error); g_assert_nonnull(plan);
    g_assert_cmpuint(plan->entries->len, ==, n);
    RadioSPlayoutEntry *last_a = g_ptr_array_index(plan->entries, 12), *first_b = g_ptr_array_index(plan->entries, 13), *last = g_ptr_array_index(plan->entries, n - 1);
    g_assert_cmpint(last_a->planned_utc_us, ==, actual_time("2026-10-09T10:53:00Z"));
    g_assert_cmpint(first_b->planned_utc_us, ==, actual_time("2026-10-09T11:00:00Z")); g_assert_cmpint(first_b->run.start_utc_us, ==, actual_time("2026-10-09T11:00:00Z"));
    g_assert_cmpint(last->planned_utc_us, ==, actual_time("2026-10-09T11:56:00Z"));
    radios_playout_plan_free(plan);
    json_object_remove_member(track_at(merged, 13), "calendar_window_wait");
    g_assert_null(radios_playout_plan_build(json_node_get_object(merged), &request, &error)); g_assert_nonnull(error); g_clear_error(&error);
    json_node_free(merged);
    json_object_set_int_member(waited, "calendar_window_wait", 1);
    g_assert_null(radios_automation_calendar_merge_selection(json_node_get_object(fresh), json_node_get_object(output), &error)); g_assert_nonnull(error); g_clear_error(&error);
    json_object_set_boolean_member(waited, "calendar_window_wait", TRUE); json_object_remove_member(waited, "calendar_constraints");
    g_assert_null(radios_automation_calendar_merge_selection(json_node_get_object(fresh), json_node_get_object(output), &error)); g_assert_nonnull(error); g_clear_error(&error);
    json_node_free(fresh); json_node_free(output);
    g_assert_null(plan_from(resolved, prepared, "2026-10-09T10:58:00Z", &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK); g_clear_error(&error);
    json_node_free(resolved); json_object_unref(prepared);
    /* A's sweeper (its song window opens 10:56) fits the gap: it plays
     * 10:57–10:57:20 and only then the selection waits for B at 11:00. */
    JsonObject *sweeper = song_rule(5, 100, 9, 1, 0); json_object_set_int_member(sweeper, "start_minute", 656);
    json_array_add_object_element(song_rules(a, NULL), sweeper);
    resolved = resolve(calendar, event); prepared = prepared_minutes(resolved, 240000);
    JsonObject *short_row = row(5, "sweeper");
    json_object_set_int_member(json_object_get_object_member(json_object_get_object_member(short_row, "detail"), "version"), "duration_ms", 20000);
    json_array_add_object_element(json_object_get_array_member(at(prepared, "sources", 0), "tracks"), short_row);
    output = plan_from(resolved, prepared, "2026-10-09T10:05:00Z", &error); g_assert_no_error(error); g_assert_nonnull(output);
    n = track_count(output); guint waited_index = 0;
    for (guint i = 0; i < n; ++i) if (json_object_has_member(track_at(output, i), "calendar_window_wait")) waited_index = i;
    g_assert_cmpuint(n, ==, 29); g_assert_cmpuint(waited_index, ==, 14); g_assert_cmpstr(track_kind(output, 13), ==, "sweeper");
    for (guint i = 1; i < n; ++i) g_assert_false(!g_strcmp0(track_kind(output, i), "sweeper") && !g_strcmp0(track_kind(output, i - 1), "sweeper"));
    plan = radios_playout_plan_build(json_node_get_object(output), &request, &error); g_assert_no_error(error); g_assert_nonnull(plan);
    g_assert_cmpint(((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, 13))->planned_utc_us, ==, actual_time("2026-10-09T10:57:00Z"));
    g_assert_cmpint(((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, 14))->planned_utc_us, ==, actual_time("2026-10-09T11:00:00Z"));
    radios_playout_plan_free(plan); json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
/* Continuity before silence, opted in like the soft block end: with
 * soft_end_seconds the opening window's first row starts at the previous
 * row's end when that is at most soft_end_seconds early (radios-16's 7/24
 * rerun: a strict wait left ~6.6 s of silence at 11:00). No anchor; the row's
 * earliest bound is where it really starts and native playout keeps it. */
static void test_named_window_early_with_soft_end(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    json_object_set_int_member(json_object_get_object_member(definition, "settings"), "soft_end_seconds", 300);
    json_object_set_int_member(event, "start_minute", 600); json_object_set_int_member(event, "end_minute", 720);
    JsonObject *a = at(definition, "sources", 0), *b = at(definition, "sources", 1);
    json_object_set_int_member(a, "start_minute", 600); json_object_set_int_member(a, "end_minute", 660);
    json_object_set_int_member(b, "start_minute", 660); json_object_set_int_member(b, "end_minute", 720);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_minutes(resolved, 240000); GError *error = NULL;
    JsonNode *output = plan_from(resolved, prepared, "2026-10-09T10:05:00Z", &error); g_assert_no_error(error); g_assert_nonnull(output);
    guint n = track_count(output); g_assert_cmpuint(n, >, 13);
    gchar *b_one = uuid(10003), *b_two = uuid(10004);
    for (guint i = 0; i < n; ++i) {
        gboolean from_b = g_str_equal(track_media(output, i), b_one) || g_str_equal(track_media(output, i), b_two);
        g_assert_cmpint(from_b, ==, i >= 13); g_assert_false(json_object_has_member(track_at(output, i), "calendar_window_wait"));
    }
    g_free(b_one); g_free(b_two);
    g_assert_true(has_diagnostic(output, "calendar_window_early")); g_assert_false(has_diagnostic(output, "calendar_window_wait"));
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(track_at(output, 13), "calendar_constraints"), "earliest_utc_us"), ==, actual_time("2026-10-09T10:57:00Z"));
    gint64 start = actual_time("2026-10-09T10:05:00Z");
    RadioSPlayoutRequest request = {.station_id = station, .database_file = database, .now_utc_us = start, .start_utc_us = start};
    RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(output), &request, &error); g_assert_no_error(error); g_assert_nonnull(plan);
    RadioSPlayoutEntry *first_b = g_ptr_array_index(plan->entries, 13);
    g_assert_cmpint(first_b->planned_utc_us, ==, actual_time("2026-10-09T10:57:00Z")); g_assert_cmpint(first_b->run.start_utc_us, <=, actual_time("2026-10-09T10:57:00Z"));
    radios_playout_plan_free(plan); json_node_free(output); json_node_free(resolved); json_object_unref(prepared);
    /* A gap longer than soft_end_seconds still waits for the window. */
    json_object_set_int_member(json_object_get_object_member(definition, "settings"), "soft_end_seconds", 60);
    resolved = resolve(calendar, event); prepared = prepared_minutes(resolved, 240000);
    output = plan_from(resolved, prepared, "2026-10-09T10:05:00Z", &error); g_assert_no_error(error); g_assert_nonnull(output);
    g_assert_true(json_object_get_boolean_member(track_at(output, 13), "calendar_window_wait")); g_assert_false(has_diagnostic(output, "calendar_window_early"));
    json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
/* Only rows whose own window opens at the waited instant take it: C, free
 * again by then (artist gap), stays behind B. The anchored row is B's, so
 * native playout never moves C before its gap ends. */
static void test_named_window_wait_only_opening_rows(void)
{
    JsonObject *event, *definition, *calendar = calendar_new(&event, &definition);
    json_object_set_int_member(event, "start_minute", 600); json_object_set_int_member(event, "end_minute", 720);
    json_object_set_int_member(json_object_get_object_member(definition, "settings"), "artist_gap_minutes", 30);
    JsonObject *late = rule(3, 1); json_object_set_int_member(late, "start_minute", 640);
    json_array_add_object_element(json_object_get_array_member(definition, "sources"), late);
    JsonNode *resolved = resolve(calendar, event); JsonObject *prepared = prepared_for_media(json_node_get_object(resolved)); GError *error = NULL;
    for (guint s = 0; s < 3; ++s) for (guint i = 0; i < 2; ++i) {
        JsonObject *track = at(at(prepared, "sources", s), "tracks", i), *detail = json_object_get_object_member(track, "detail");
        json_object_set_int_member(json_object_get_object_member(detail, "version"), "duration_ms", 240000);
        if (s < 2) continue;
        json_object_set_string_member(json_object_get_object_member(detail, "media"), "artist", "Başka sanatçı");
        json_object_set_string_member(json_object_get_object_member(track, "metadata"), "artist", "Başka sanatçı");
    }
    JsonNode *output = plan_from(resolved, prepared, "2026-10-09T10:05:00Z", &error); g_assert_no_error(error); g_assert_nonnull(output);
    g_assert_cmpuint(track_count(output), >=, 2); expect_track(output, 0, 1); expect_track(output, 1, 5);
    g_assert_true(json_object_get_boolean_member(track_at(output, 1), "calendar_window_wait"));
    gint64 start = actual_time("2026-10-09T10:05:00Z");
    RadioSPlayoutRequest request = {.station_id = station, .database_file = database, .now_utc_us = start, .start_utc_us = start};
    RadioSPlayoutPlan *plan = radios_playout_plan_build(json_node_get_object(output), &request, &error); g_assert_no_error(error); g_assert_nonnull(plan);
    g_assert_cmpint(((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, 1))->planned_utc_us, ==, actual_time("2026-10-09T10:40:00Z"));
    radios_playout_plan_free(plan); json_node_free(output); json_node_free(resolved); json_object_unref(prepared); json_object_unref(calendar);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/automation-calendar-plan/schema-runtime-handoff-alternatives-presets", test_runtime_programme_handoff_keeps_alternatives_and_presets);
    g_test_add_func("/automation-calendar-plan/schema-two-two-two-link-default-cross-pool", test_schema_two_two_two_link_default_and_cross_pool);
    g_test_add_func("/automation-calendar-plan/schema-link-preset-follows-playlist-kind", test_schema_link_preset_follows_playlist_kind);
    g_test_add_func("/automation-calendar-plan/schema-cluster-handover-native-plan", test_schema_cluster_handover_native_plan);
    g_test_add_func("/automation-calendar-plan/schema-live-step-duration-manual-native-wait", test_live_step_duration_manual_and_native_wait);
    g_test_add_func("/automation-calendar-plan/schema-programme-entry-runtime-seeded", test_programme_entry_runtime_and_seeded_alternatives);
    g_test_add_func("/automation-calendar-plan/schema-date-window-event-live-default", test_date_window_excludes_source_and_event_live_default);
    g_test_add_func("/automation-calendar-plan/schema-contract-validation-live-kind", test_schema_contract_validation_and_live_kind);
    g_test_add_func("/automation-calendar-plan/autodj-designed-presets-rotation-schedule", test_autodj_presets_rotation_and_schedule);
    g_test_add_func("/automation-calendar-plan/SEMI-tight-slot-native-complete-prefix", test_semi_tight_slot_selects_complete_native_prefix);
    g_test_add_func("/automation-calendar-plan/copyright-planned-date-mixed-source", test_copyright_planned_dates_and_mixed_source);
    g_test_add_func("/automation-calendar-plan/AutoDJ-short-IN-OUT-source-window", test_short_autodj_incoming_span_keeps_source_window_eligible);
    g_test_add_func("/automation-calendar-plan/timezone-and-shared-audible-geometry", test_timezone_and_shared_audible_geometry);
    g_test_add_func("/automation-calendar-plan/marked-sound-end-window-native-playout", test_marked_sound_end_window_matches_native_playout);
    g_test_add_func("/automation-calendar-plan/song-linear-order-zero-caps-copy", test_song_linear_order_zero_caps_and_input_copy);
    g_test_add_func("/automation-calendar-plan/song-weighted-seeded-relative-transition-independent", test_song_weighted_reproducible_relative_and_transition_independence);
    g_test_add_func("/automation-calendar-plan/song-frequency-pinned-quota-empty-cycles", test_song_frequency_pinned_quota_and_empty_cycles);
    g_test_add_func("/automation-calendar-plan/song-window-actual-media-interval-phase2", test_song_windows_actual_media_interval_and_phase_two);
    g_test_add_func("/automation-calendar-plan/song-rules-invalid-missing-legacy", test_song_rule_validation_missing_and_legacy_modes);
    g_test_add_func("/automation-calendar-plan/source-zero-unavailable-reasons-fences", test_zero_source_unavailable_reasons_skip_but_preserve_fences);
    g_test_add_func("/automation-calendar-plan/continuity-no-consecutive-short-items", test_named_no_consecutive_short_items);
    g_test_add_func("/automation-calendar-plan/continuity-soft-end-relaxed-gaps-opt-in", test_named_soft_end_and_relaxed_gaps_opt_in);
    g_test_add_func("/automation-calendar-plan/continuity-soft-end-crosses-midnight", test_named_soft_end_crosses_midnight);
    g_test_add_func("/automation-calendar-plan/continuity-window-wait-next-source-window", test_named_window_wait_next_source_window);
    g_test_add_func("/automation-calendar-plan/continuity-window-early-with-soft-end", test_named_window_early_with_soft_end);
    g_test_add_func("/automation-calendar-plan/continuity-window-wait-only-opening-rows", test_named_window_wait_only_opening_rows);
    g_test_add_func("/automation-calendar-plan/kind-transition-estimate-matches-native-repeat-gap", test_kind_transition_estimate_matches_native_repeat_gap);
    g_test_add_func("/automation-calendar-plan/source-zero-empty-safe-positive-transport", test_zero_source_disabled_empty_and_transport_positive);
    g_test_add_func("/automation-calendar-plan/lowpass-zero-overlap-rejected-all-weights", test_transition_lowpass_zero_overlap_all_weights_rejected);
    g_test_add_func("/automation-calendar-plan/constraints-authenticated-bounds-turns", test_constraints_authenticated_bounds_and_turns);
    g_test_add_func("/automation-calendar-plan/constraints-phase2-typed-preservation", test_constraints_phase_two_typed_preservation);
    g_test_add_func("/automation-calendar-plan/autodj-legacy-actual-kind", test_autodj_legacy_actual_kind_and_music_other_pools);
    g_test_add_func("/automation-calendar-plan/autodj-named-phase2-kind-fence", test_autodj_named_override_disable_and_phase_two_kind_fence);
    g_test_add_func("/automation-calendar-plan/autodj-source-pool-bounds-types", test_autodj_source_pool_bounds_and_types);
    g_test_add_func("/automation-calendar-plan/transition-linear-effect-local-beatmatch", test_transition_linear_enabled_cycle_effect_and_local_beatmatch_hint);
    g_test_add_func("/automation-calendar-plan/transition-sparse-linear-independent-rng", test_transition_sparse_linear_override_does_not_change_track_shuffle);
    g_test_add_func("/automation-calendar-plan/transition-optional-rule-validation", test_transition_optional_rule_validation_and_legacy_default);
    g_test_add_func("/automation-calendar-plan/prepare-deep-copy", test_prepare_bridge_deep_copy);
    g_test_add_func("/automation-calendar-plan/frequency-order-reorder", test_frequency_order_and_reorder);
    g_test_add_func("/automation-calendar-plan/music-jingle-blocks-preserve-order", test_music_jingle_blocks_preserve_playable_order);
    g_test_add_func("/automation-calendar-plan/music-sources-keep-separate-event-blocks", test_music_sources_keep_separate_event_blocks);
    g_test_add_func("/automation-calendar-plan/random-source-quotas", test_random_source_exact_quotas_and_fixed_rows);
    g_test_add_func("/automation-calendar-plan/independent-kind-randomness", test_independent_music_advert_sweeper_flags);
    g_test_add_func("/automation-calendar-plan/media-kind-legacy-default", test_media_kind_filters_and_legacy_default);
    g_test_add_func("/automation-calendar-plan/media-kind-frequency-order", test_media_kind_before_frequency_and_round_robin);
    g_test_add_func("/automation-calendar-plan/media-kind-invalid-empty-security", test_media_kind_invalid_empty_and_security);
    g_test_add_func("/automation-calendar-plan/duration-diagnostic-security", test_duration_cap_unavailable_diagnostic_and_security);
    g_test_add_func("/automation-calendar-plan/frozen-bindings-real-planner", test_frozen_bindings_deep_copy_enter_existing_planner);
    g_test_add_func("/automation-calendar-plan/named-resolver-identity", test_named_resolver_identity_overrides_and_invalidation);
    g_test_add_func("/automation-calendar-plan/resolver-specific-bounded-block-context", test_resolver_errors_identify_block_and_exact_prerequisite);
    g_test_add_func("/automation-calendar-plan/named-count-frequency", test_named_linear_count_frequency_and_empty_logical_cycles);
    g_test_add_func("/automation-calendar-plan/named-weighted-reproducible", test_named_weighted_selection_reproducible_relative_weights);
    g_test_add_func("/automation-calendar-plan/named-interval-window", test_named_interval_turn_start_and_window_limits);
    g_test_add_func("/automation-calendar-plan/named-gaps-history-yield", test_named_gap_history_and_short_burst_yields_other_group);
    g_test_add_func("/automation-calendar-plan/named-merge-native-transitions", test_named_phase_two_merge_and_actual_transition_plan);
    g_test_add_func("/automation-calendar-plan/named-transition-probabilities", test_named_transition_probabilities_independent_track_shuffle);
    g_test_add_func("/automation-calendar-plan/named-artist-verified-cue", test_named_verified_artist_keys_and_cue_timing);
    g_test_add_func("/automation-calendar-plan/named-global-end-fitting-source", test_named_global_end_chooses_fitting_source);
    g_test_add_func("/automation-calendar-plan/latejoin-exact-remaining-native", test_late_join_remaining_window_exact_epoch_and_native_plan);
    g_test_add_func("/automation-calendar-plan/latejoin-real-history-legacy-hints", test_late_join_actual_history_expiry_and_legacy_hints);
    g_test_add_func("/automation-calendar-plan/assignment-weighted-bursts-independent", test_assignment_weighted_bursts_determinism_and_rng_independence);
    g_test_add_func("/automation-calendar-plan/assignment-zero-disable-typed-parking", test_assignment_zero_disable_linear_and_typed_empty_parking);
    g_test_add_func("/automation-calendar-plan/assignment-only-eligible-groups", test_assignment_weighted_chooses_only_currently_legal_groups);
    g_test_add_func("/automation-calendar-plan/autodj-kind-policy-channels-no-repeat", test_autodj_kind_policy_channels_and_no_repeat);
    g_test_add_func("/automation-calendar-plan/autodj-kind-sync-percent-share", test_autodj_kind_sync_percent_share);
    g_test_add_func("/automation-calendar-plan/kind-entries-share-remainder-and-legacy", test_kind_entries_share_remainder_and_legacy);
    g_test_add_func("/automation-calendar-plan/kind-entries-presets-schedule-automation-off", test_kind_entries_presets_schedule_and_automation_off);
    g_test_add_func("/automation-calendar-plan/kind-entries-validation", test_kind_entries_validation);
    g_test_add_func("/automation-calendar-plan/named-excluded-kinds", test_named_excluded_kinds_filter_jingle_layers_and_validation);
    g_test_add_func("/automation-calendar-plan/actual-source-turns-refill-native", test_actual_source_turns_survive_refill_and_native_fence);
    g_test_add_func("/automation-calendar-plan/actual-source-turns-identity-epoch-bounds", test_actual_source_turns_reject_fabricated_identity_epoch_and_bounds);
    return g_test_run();
}
