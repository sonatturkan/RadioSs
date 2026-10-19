#include "radios/player_topology.h"
#include <string.h>

static JsonObject *document(const gchar *json)
{
    JsonParser *parser = json_parser_new(); GError *error = NULL;
    g_assert_true(json_parser_load_from_data(parser, json, -1, &error)); g_assert_no_error(error);
    JsonObject *result = json_object_ref(json_node_get_object(json_parser_get_root(parser)));
    g_object_unref(parser); return result;
}
static void migration(void)
{
    RadioSPlayerTopology topology;
    JsonObject *old = document("{\"deck_count\":3,\"slots\":[2,7,20],\"names\":{\"0\":\"MAIN\",\"3\":\"Bağımsız\"}}");
    GError *error = NULL;
    g_assert_true(radios_player_topology_from_legacy(&topology, old, &error)); g_assert_no_error(error);
    g_assert_cmpuint(radios_player_topology_count(&topology), ==, 3);
    g_assert_false(topology.players[0].enabled); g_assert_true(topology.players[20].enabled);
    g_assert_cmpstr(topology.players[2].name, ==, "Bağımsız"); g_assert_cmpstr(topology.master_name, ==, "MAIN");
    g_assert_cmpuint(radios_player_topology_members(&topology, "", NULL, 0), ==, 3);
    g_assert_cmpuint(topology.cluster_count, ==, 0);
    for (guint i = 0; i < 32; ++i) g_assert_cmpint(topology.players[i].autodj_mode, ==, RADIOS_AUTODJ_OFF);
    JsonObject *new = radios_player_topology_to_json(&topology);
    RadioSPlayerTopology loaded;
    g_assert_true(radios_player_topology_from_json(&loaded, new, &error)); g_assert_no_error(error);
    g_assert_cmpint(memcmp(&topology, &loaded, sizeof topology), ==, 0);
    json_object_unref(old); json_object_unref(new);
}
static void pools(void)
{
    RadioSPlayerTopology topology; radios_player_topology_default(&topology, 6);
    gchar first[37], second[37]; GError *error = NULL;
    g_assert_true(radios_player_topology_add_cluster(&topology, "Müzik", first, &error));
    g_assert_true(radios_player_topology_add_cluster(&topology, "Haber", second, &error));
    g_assert_true(radios_player_topology_set_cluster(&topology, 0, first, &error));
    g_assert_true(radios_player_topology_set_cluster(&topology, 2, first, &error));
    g_assert_true(radios_player_topology_set_cluster(&topology, 1, second, &error));
    g_assert_true(radios_player_topology_set_autodj(&topology, 0, RADIOS_AUTODJ_TRANSITIONS, -1, &error));
    g_assert_cmpint(radios_player_topology_next_target(&topology, 0), ==, 2);
    g_assert_true(radios_player_topology_set_autodj(&topology, 2, RADIOS_AUTODJ_TRANSITIONS, -1, &error));
    g_assert_cmpint(radios_player_topology_next_target(&topology, 2), ==, 0);
    g_assert_false(radios_player_topology_set_autodj(&topology, 0, RADIOS_AUTODJ_TRANSITIONS, 1, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
    g_assert_true(radios_player_topology_set_autodj(&topology, 3, RADIOS_AUTODJ_TRANSITIONS, 5, &error));
    g_assert_cmpint(radios_player_topology_next_target(&topology, 3), ==, 5);
    g_assert_true(radios_player_topology_set_cluster(&topology, 5, second, &error));
    g_assert_cmpint(topology.players[3].autodj_target, ==, -1);
    g_assert_cmpint(radios_player_topology_next_target(&topology, 3), ==, 4);
    g_assert_true(radios_player_topology_set_autodj(&topology, 4, RADIOS_AUTODJ_SEMI, -1, &error));
    g_assert_cmpint(radios_player_topology_next_target(&topology, 4), ==, -1);
    g_assert_false(radios_player_topology_set_autodj(&topology, 4, RADIOS_AUTODJ_SEMI, 3, &error)); g_clear_error(&error);
    guint members[2]; g_assert_cmpuint(radios_player_topology_members(&topology, first, members, 2), ==, 2);
    g_assert_cmpuint(members[0], ==, 0); g_assert_cmpuint(members[1], ==, 2);
    g_assert_true(radios_player_topology_remove_cluster(&topology, first, &error)); g_assert_no_error(error);
    g_assert_cmpstr(topology.players[0].cluster_id, ==, "");
    g_assert_cmpuint(radios_player_topology_members(&topology, "", NULL, 0), ==, 4);
}
static void assignments(void)
{
    RadioSPlayerTopology topology; radios_player_topology_default(&topology, 2);
    gchar cluster[37]; GError *error = NULL;
    const gchar *shared = "ea7fc088-978a-442b-891f-2df2a040c324";
    const gchar *individual = "ef3bcd15-e730-48d6-be70-4c1e6384f64c";
    g_assert_true(radios_player_topology_add_cluster(&topology, "Yayın", cluster, &error));
    g_assert_true(radios_player_topology_set_cluster(&topology, 0, cluster, &error));
    g_assert_true(radios_player_topology_set_cluster(&topology, 1, cluster, &error));
    g_assert_true(radios_player_topology_set_cluster_automation(&topology, cluster, shared, &error));
    g_assert_cmpstr(radios_player_topology_automation(&topology, 0), ==, shared);
    g_assert_true(radios_player_topology_set_automation(&topology, 0, individual, &error));
    g_assert_cmpstr(radios_player_topology_automation(&topology, 0), ==, individual);
    g_assert_cmpstr(radios_player_topology_automation(&topology, 1), ==, shared);
    g_assert_true(radios_player_topology_set_automation(&topology, 0, "", &error));
    g_assert_cmpstr(radios_player_topology_automation(&topology, 0), ==, shared);
    RadioSPlayerTopology before = topology;
    g_assert_false(radios_player_topology_set_automation(&topology, 0, "invalid", &error)); g_clear_error(&error);
    g_assert_cmpint(memcmp(&topology, &before, sizeof topology), ==, 0);
    JsonObject *json = radios_player_topology_to_json(&topology); RadioSPlayerTopology roundtrip;
    g_assert_true(radios_player_topology_from_json(&roundtrip, json, &error)); g_assert_no_error(error);
    g_assert_cmpuint(roundtrip.cluster_count, ==, topology.cluster_count);
    g_assert_cmpstr(roundtrip.clusters[0].id, ==, topology.clusters[0].id);
    g_assert_cmpstr(radios_player_topology_automation(&roundtrip, 0), ==, shared);
    g_assert_cmpstr(radios_player_topology_automation(&roundtrip, 1), ==, shared);
    json_object_unref(json);
}
static void stable_slots(void)
{
    RadioSPlayerTopology topology; radios_player_topology_default(&topology, 3); GError *error = NULL;
    g_assert_true(radios_player_topology_set_autodj(&topology, 0, RADIOS_AUTODJ_TRANSITIONS, 1, &error));
    g_assert_true(radios_player_topology_remove_player(&topology, 1, &error));
    g_assert_true(topology.players[2].enabled); g_assert_cmpint(topology.players[0].autodj_target, ==, -1);
    guint deck = 32;
    g_assert_true(radios_player_topology_add_player(&topology, "", &deck, &error)); g_assert_cmpuint(deck, ==, 1);
    g_assert_true(radios_player_topology_remove_player(&topology, 0, &error));
    g_assert_true(radios_player_topology_remove_player(&topology, 1, &error));
    g_assert_false(radios_player_topology_remove_player(&topology, 2, &error)); g_clear_error(&error);
    g_assert_cmpuint(radios_player_topology_count(&topology), ==, 1);
}
static void invalid_documents(void)
{
    static const gchar *const bad[] = {
        "{}", "{\"schema_version\":2,\"deck_count\":2}", "{\"schema_version\":true,\"deck_count\":2}",
        "{\"schema_version\":1,\"deck_count\":2.0}", "{\"schema_version\":1,\"deck_count\":0}",
        "{\"schema_version\":1,\"deck_count\":2,\"slots\":[0,0]}",
        "{\"schema_version\":1,\"deck_count\":2,\"slots\":[0,true]}",
        "{\"schema_version\":1,\"deck_count\":2,\"names\":{\"01\":\"wrong\"}}",
        "{\"schema_version\":1,\"deck_count\":2,\"names\":{\"1\":null}}",
        "{\"schema_version\":1,\"deck_count\":2,\"clusters\":[null]}",
        "{\"schema_version\":1,\"deck_count\":2,\"clusters\":[{\"id\":\"a\",\"name\":\"B\"}]}",
        "{\"schema_version\":1,\"deck_count\":2,\"players\":{\"3\":{}}}",
        "{\"schema_version\":1,\"deck_count\":2,\"players\":{\"1\":{\"autodj_mode\":1,\"autodj_target\":2}}}",
        "{\"schema_version\":1,\"deck_count\":2,\"players\":{\"1\":{\"autodj_mode\":2,\"autodj_target\":1}}}",
        "{\"schema_version\":1,\"deck_count\":2,\"players\":{\"1\":{\"cluster_id\":\"ed40b810-90c9-4880-803e-c4f59f7e5b85\"}}}",
        "{\"schema_version\":1,\"deck_count\":2,\"running\":true}"
    };
    RadioSPlayerTopology topology; radios_player_topology_default(&topology, 6);
    RadioSPlayerTopology before = topology;
    for (guint i = 0; i < G_N_ELEMENTS(bad); ++i) {
        JsonObject *json = document(bad[i]); GError *error = NULL;
        g_assert_false(radios_player_topology_from_json(&topology, json, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
        g_assert_cmpint(memcmp(&topology, &before, sizeof topology), ==, 0); json_object_unref(json);
    }
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/player-topology/migration", migration);
    g_test_add_func("/player-topology/pools", pools);
    g_test_add_func("/player-topology/assignments", assignments);
    g_test_add_func("/player-topology/stable-slots", stable_slots);
    g_test_add_func("/player-topology/invalid-documents", invalid_documents);
    return g_test_run();
}
