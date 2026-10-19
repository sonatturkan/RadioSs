#include "radios/automation_scope.h"
#include "radios/weekly_programme_plan.h"
#include "radios/automation_calendar_plan.h"
#include "radios/programme_model.h"
#include <string.h>

static const gchar *const station_id = "10000000-0000-4000-8000-000000000001";
static const gchar *const schema_id = "20000000-0000-4000-8000-000000000001";
static const gchar *const override_id = "20000000-0000-4000-8000-000000000002";
static const gchar *const cluster_id = "30000000-0000-4000-8000-000000000001";
static const gchar *const kinds[] = {"music", "advertisement", "jingle", "sweeper"};

static RadioSPlayerTopology topology(void)
{
    RadioSPlayerTopology result; radios_player_topology_default(&result, 4);
    result.cluster_count = 1;
    g_strlcpy(result.clusters[0].id, cluster_id, sizeof result.clusters[0].id);
    g_strlcpy(result.clusters[0].name, "Müzik", sizeof result.clusters[0].name);
    g_strlcpy(result.clusters[0].automation_id, schema_id, sizeof result.clusters[0].automation_id);
    g_strlcpy(result.players[0].cluster_id, cluster_id, sizeof result.players[0].cluster_id);
    g_strlcpy(result.players[2].cluster_id, cluster_id, sizeof result.players[2].cluster_id);
    g_assert_true(radios_player_topology_validate(&result, NULL)); return result;
}
static RadioSAutomationBinding binding(RadioSPlayerTopology *players, RadioSAutomationScopeKind kind, guint deck)
{
    RadioSAutomationScope scope = {.kind = kind, .deck = deck};
    if (kind == RADIOS_AUTOMATION_SCOPE_CLUSTER) g_strlcpy(scope.cluster_id, cluster_id, sizeof scope.cluster_id);
    RadioSAutomationBinding result; GError *error = NULL;
    g_assert_true(radios_automation_scope_resolve(players, &scope, &result, &error)); g_assert_no_error(error); return result;
}
static void test_membership_and_inheritance(void)
{
    RadioSPlayerTopology players = topology();
    RadioSAutomationBinding cluster = binding(&players, RADIOS_AUTOMATION_SCOPE_CLUSTER, 0);
    g_assert_cmpuint(cluster.count, ==, 2); g_assert_cmpuint(cluster.source, ==, 0);
    g_assert_cmpuint(cluster.decks[0], ==, 0); g_assert_cmpuint(cluster.decks[1], ==, 2);
    g_assert_cmpstr(cluster.automation_id, ==, schema_id); g_assert_false(cluster.inherited);
    RadioSAutomationBinding independent = binding(&players, RADIOS_AUTOMATION_SCOPE_UNCLUSTERED, 0);
    g_assert_cmpuint(independent.count, ==, 2); g_assert_cmpuint(independent.decks[0], ==, 1);
    g_assert_cmpuint(independent.decks[1], ==, 3); g_assert_cmpstr(independent.automation_id, ==, "");
    RadioSAutomationBinding player = binding(&players, RADIOS_AUTOMATION_SCOPE_PLAYER, 2);
    g_assert_cmpuint(player.count, ==, 1); g_assert_cmpuint(player.source, ==, 2);
    g_assert_true(player.inherited); g_assert_cmpstr(player.automation_id, ==, schema_id);
    g_assert_true(radios_player_topology_set_automation(&players, 2, override_id, NULL));
    player = binding(&players, RADIOS_AUTOMATION_SCOPE_PLAYER, 2);
    g_assert_false(player.inherited); g_assert_cmpstr(player.automation_id, ==, override_id);
    g_assert_cmpstr(players.clusters[0].automation_id, ==, schema_id);
}
static void test_semi_and_transition_scopes(void)
{
    RadioSPlayerTopology players = topology();
    g_assert_true(radios_player_topology_set_autodj(&players, 1, RADIOS_AUTODJ_SEMI, -1, NULL));
    RadioSAutomationBinding player = binding(&players, RADIOS_AUTOMATION_SCOPE_PLAYER, 1);
    g_assert_cmpuint(player.count, ==, 1); g_assert_cmpint(player.autodj_target, ==, -1);
    g_assert_true(radios_player_topology_set_autodj(&players, 1, RADIOS_AUTODJ_TRANSITIONS, 3, NULL));
    player = binding(&players, RADIOS_AUTOMATION_SCOPE_PLAYER, 1);
    g_assert_cmpuint(player.count, ==, 2); g_assert_cmpuint(player.decks[0], ==, 1);
    g_assert_cmpuint(player.decks[1], ==, 3); g_assert_cmpint(player.autodj_target, ==, 3);
    g_assert_true(radios_player_topology_set_autodj(&players, 0, RADIOS_AUTODJ_TRANSITIONS, -1, NULL));
    player = binding(&players, RADIOS_AUTOMATION_SCOPE_PLAYER, 0);
    g_assert_cmpint(player.autodj_target, ==, 2);
    g_assert_cmpuint(player.decks[1], ==, 2);
}
static void stale(RadioSPlayerTopology *players, RadioSAutomationBinding *captured)
{
    GError *error = NULL;
    g_assert_false(radios_automation_scope_binding_current(players, captured, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
}
static void test_stale_binding_fences(void)
{
    RadioSPlayerTopology players = topology();
    RadioSAutomationBinding captured = binding(&players, RADIOS_AUTOMATION_SCOPE_CLUSTER, 0);
    g_assert_true(radios_automation_scope_binding_current(&players, &captured, NULL));
    g_strlcpy(players.players[0].name, "Yeni görünen ad", sizeof players.players[0].name);
    g_assert_true(radios_automation_scope_binding_current(&players, &captured, NULL));
    g_assert_true(radios_player_topology_set_automation(&players, 2, override_id, NULL)); stale(&players, &captured);
    players = topology(); captured = binding(&players, RADIOS_AUTOMATION_SCOPE_PLAYER, 0);
    g_assert_true(radios_player_topology_set_cluster_automation(&players, cluster_id, override_id, NULL)); stale(&players, &captured);
    players = topology(); captured = binding(&players, RADIOS_AUTOMATION_SCOPE_CLUSTER, 0);
    g_assert_true(radios_player_topology_set_cluster(&players, 2, "", NULL)); stale(&players, &captured);
    players = topology(); captured = binding(&players, RADIOS_AUTOMATION_SCOPE_PLAYER, 1);
    g_assert_true(radios_player_topology_set_autodj(&players, 1, RADIOS_AUTODJ_SEMI, -1, NULL)); stale(&players, &captured);
}
static void test_saved_rotation_exact_pool(void)
{
    RadioSPlayerTopology players; radios_player_topology_default(&players, 6);
    players.cluster_count = 1;
    g_strlcpy(players.clusters[0].id, cluster_id, sizeof players.clusters[0].id);
    g_strlcpy(players.clusters[0].name, "Başka küme", sizeof players.clusters[0].name);
    g_strlcpy(players.players[5].cluster_id, cluster_id, sizeof players.players[5].cluster_id);
    g_assert_true(radios_player_topology_set_autodj(&players, 3, RADIOS_AUTODJ_TRANSITIONS, 1, NULL));
    RadioSAutomationScope scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 3};
    const guint saved[] = {0, 3, 1, 2}, expected[] = {3, 0, 1, 2};
    RadioSAutomationBinding captured, ordinary; GError *error = NULL;
    g_assert_true(radios_automation_scope_resolve_routed(&players, &scope, saved,
        G_N_ELEMENTS(saved), &captured, &error)); g_assert_no_error(error);
    g_assert_true(captured.routed_pool); g_assert_cmpuint(captured.source, ==, 3);
    g_assert_cmpuint(captured.count, ==, G_N_ELEMENTS(expected));
    for (guint i = 0; i < captured.count; ++i) g_assert_cmpuint(captured.decks[i], ==, expected[i]);
    g_assert_true(radios_automation_scope_binding_current(&players, &captured, NULL));
    g_assert_true(radios_automation_scope_resolve(&players, &scope, &ordinary, NULL));
    g_assert_false(ordinary.routed_pool); g_assert_cmpuint(ordinary.count, ==, 2);
    g_assert_cmpuint(ordinary.decks[0], ==, 3); g_assert_cmpuint(ordinary.decks[1], ==, 1);
    /* Unused players are not included or leased merely because they share a
     * group. Their assignments can change while this exact pool stays valid. */
    g_assert_true(radios_player_topology_set_automation(&players, 4, schema_id, NULL));
    g_assert_true(radios_automation_scope_binding_current(&players, &captured, NULL));
    RadioSPlayerTopology baseline = players;
    g_assert_true(radios_player_topology_set_automation(&players, 2, schema_id, NULL)); stale(&players, &captured);
    players = baseline;
    g_assert_true(radios_player_topology_set_automation(&players, 1, schema_id, NULL)); stale(&players, &captured);
    players = baseline;
    g_assert_true(radios_player_topology_set_automation(&players, 3, override_id, NULL)); stale(&players, &captured);
    players = baseline;
    g_assert_true(radios_player_topology_set_autodj(&players, 3, RADIOS_AUTODJ_TRANSITIONS, 0, NULL)); stale(&players, &captured);
    players = baseline;
    g_assert_true(radios_player_topology_set_autodj(&players, 3, RADIOS_AUTODJ_SEMI, -1, NULL)); stale(&players, &captured);
    players = baseline;
    g_assert_true(radios_player_topology_set_cluster(&players, 2, cluster_id, NULL)); stale(&players, &captured);
    players = baseline;
    g_assert_true(radios_player_topology_remove_player(&players, 2, NULL)); stale(&players, &captured);
    players = baseline;
    const guint rejected[][4] = {{3, 0, 1, 5}, {3, 0, 1, 1}, {0, 1, 2, 4}, {3, 0, 1, 32}};
    for (guint i = 0; i < G_N_ELEMENTS(rejected); ++i) {
        g_assert_false(radios_automation_scope_resolve_routed(&players, &scope, rejected[i], 4, &ordinary, &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    }
    g_assert_false(radios_automation_scope_resolve_routed(&players, &scope, saved, 0, &ordinary, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    /* The same exact-pool contract applies within a real cluster, preserving
     * the clicked (nonlowest) source and its inherited assignment. */
    players = topology();
    g_assert_true(radios_player_topology_set_autodj(&players, 2, RADIOS_AUTODJ_TRANSITIONS, 0, NULL));
    scope.deck = 2; const guint cluster_pool[] = {0, 2};
    g_assert_true(radios_automation_scope_resolve_routed(&players, &scope, cluster_pool, 2, &captured, &error));
    g_assert_no_error(error); g_assert_cmpuint(captured.decks[0], ==, 2);
    g_assert_true(captured.inherited); g_assert_cmpstr(captured.automation_id, ==, schema_id);
    g_assert_true(radios_player_topology_set_cluster_automation(&players, cluster_id, override_id, NULL)); stale(&players, &captured);
}
static JsonObject *event(void)
{
    JsonObject *result = json_object_new(), *source = json_object_new(); JsonArray *sources = json_array_new();
    json_object_set_string_member(result, "id", "40000000-0000-4000-8000-000000000001");
    json_object_set_string_member(result, "title", "Yayın"); json_object_set_string_member(result, "date", "2026-10-12");
    json_object_set_string_member(result, "color", "#00ABCD");
    json_object_set_int_member(result, "start_minute", 0); json_object_set_int_member(result, "end_minute", 60);
    json_object_set_string_member(source, "id", "40000000-0000-4000-8000-000000000002");
    json_object_set_string_member(source, "playlist_id", "40000000-0000-4000-8000-000000000003");
    json_object_set_int_member(source, "frequency", 1);
    const gchar *flags[] = {"random", "track_random", "advert_random", "sweeper_random"};
    for (guint i = 0; i < G_N_ELEMENTS(flags); ++i) json_object_set_boolean_member(source, flags[i], FALSE);
    json_array_add_object_element(sources, source); json_object_set_array_member(result, "rotations", sources); return result;
}
static JsonObject *source(JsonObject *value)
{ return json_array_get_object_element(json_object_get_array_member(value, "rotations"), 0); }
static gchar *encode(JsonObject *value)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, value);
    gchar *result = json_to_string(node, FALSE); json_node_free(node); return result;
}
static JsonObject *distribution(gint64 first, gint64 second)
{
    JsonObject *result = json_object_new();
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) {
        JsonArray *pool = json_array_new(); json_array_add_int_element(pool, first);
        if (second > 0) json_array_add_int_element(pool, second);
        json_object_set_array_member(result, kinds[i], pool);
    }
    return result;
}
static void test_ephemeral_routing_and_saved_pools(void)
{
    RadioSPlayerTopology players = topology(); RadioSAutomationBinding captured = binding(&players, RADIOS_AUTOMATION_SCOPE_CLUSTER, 0);
    JsonObject *original = event(); gchar *before = encode(original); GError *error = NULL;
    JsonNode *routed = radios_automation_scope_route_event(&captured, original, &error); g_assert_no_error(error); g_assert_nonnull(routed);
    JsonObject *copy = source(json_node_get_object(routed)); JsonObject *routing = json_object_get_object_member(copy, "autodj");
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) {
        JsonArray *pool = json_object_get_array_member(routing, kinds[i]);
        g_assert_cmpuint(json_array_get_length(pool), ==, 2);
        g_assert_cmpint(json_array_get_int_element(pool, 0), ==, 1); g_assert_cmpint(json_array_get_int_element(pool, 1), ==, 3);
    }
    gchar *after = encode(original); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
    json_node_free(routed);
    json_object_set_object_member(source(original), "autodj", distribution(3, 1));
    routed = radios_automation_scope_route_event(&captured, original, &error); g_assert_no_error(error); g_assert_nonnull(routed);
    routing = json_object_get_object_member(source(json_node_get_object(routed)), "autodj");
    g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(routing, "music"), 0), ==, 3);
    json_node_free(routed); json_object_unref(original);
}
static void test_group_explicit_target_order(void)
{
    RadioSPlayerTopology players = topology();
    /* Four cluster members and three independent players make a chosen last
     * member visibly different from the automatic next player. */
    guint deck = 32;
    g_assert_true(radios_player_topology_add_player(&players, cluster_id, &deck, NULL)); g_assert_cmpuint(deck, ==, 4);
    g_assert_true(radios_player_topology_add_player(&players, "", &deck, NULL)); g_assert_cmpuint(deck, ==, 5);
    g_assert_true(radios_player_topology_add_player(&players, cluster_id, &deck, NULL)); g_assert_cmpuint(deck, ==, 6);
    g_assert_true(radios_player_topology_set_autodj(&players, 0, RADIOS_AUTODJ_TRANSITIONS, 6, NULL));
    g_assert_true(radios_player_topology_set_autodj(&players, 1, RADIOS_AUTODJ_TRANSITIONS, 5, NULL));
    RadioSAutomationBinding cluster = binding(&players, RADIOS_AUTOMATION_SCOPE_CLUSTER, 0);
    RadioSAutomationBinding independent = binding(&players, RADIOS_AUTOMATION_SCOPE_UNCLUSTERED, 0);
    const guint cluster_order[] = {0, 6, 2, 4}, independent_order[] = {1, 5, 3};
    g_assert_cmpuint(cluster.source, ==, 0); g_assert_cmpuint(cluster.count, ==, G_N_ELEMENTS(cluster_order));
    g_assert_cmpint(cluster.autodj_target, ==, 6); g_assert_cmpint(cluster.autodj_mode, ==, RADIOS_AUTODJ_TRANSITIONS);
    g_assert_cmpuint(independent.source, ==, 1); g_assert_cmpuint(independent.count, ==, G_N_ELEMENTS(independent_order));
    g_assert_cmpint(independent.autodj_target, ==, 5);
    for (guint i = 0; i < cluster.count; ++i) g_assert_cmpuint(cluster.decks[i], ==, cluster_order[i]);
    for (guint i = 0; i < independent.count; ++i) g_assert_cmpuint(independent.decks[i], ==, independent_order[i]);
    g_assert_true(radios_automation_scope_binding_current(&players, &cluster, NULL));
    g_assert_true(radios_automation_scope_binding_current(&players, &independent, NULL));

    JsonObject *original = event(); GError *error = NULL;
    JsonNode *routed = radios_automation_scope_route_event(&cluster, original, &error); g_assert_no_error(error); g_assert_nonnull(routed);
    JsonObject *routing = json_object_get_object_member(source(json_node_get_object(routed)), "autodj");
    for (guint kind = 0; kind < G_N_ELEMENTS(kinds); ++kind) {
        JsonArray *pool = json_object_get_array_member(routing, kinds[kind]);
        g_assert_cmpuint(json_array_get_length(pool), ==, G_N_ELEMENTS(cluster_order));
        for (guint i = 0; i < G_N_ELEMENTS(cluster_order); ++i)
            g_assert_cmpint(json_array_get_int_element(pool, i), ==, cluster_order[i] + 1);
    }
    json_node_free(routed); json_object_unref(original);
    g_assert_true(radios_player_topology_set_autodj(&players, 0, RADIOS_AUTODJ_TRANSITIONS, 4, NULL)); stale(&players, &cluster);
    g_assert_true(radios_automation_scope_binding_current(&players, &independent, NULL));
    cluster = binding(&players, RADIOS_AUTOMATION_SCOPE_CLUSTER, 0);
    const guint changed_order[] = {0, 4, 2, 6};
    for (guint i = 0; i < cluster.count; ++i) g_assert_cmpuint(cluster.decks[i], ==, changed_order[i]);
    g_assert_true(radios_automation_scope_binding_current(&players, &cluster, NULL));
    g_assert_true(radios_player_topology_set_autodj(&players, 1, RADIOS_AUTODJ_TRANSITIONS, 3, NULL)); stale(&players, &independent);
    g_assert_true(radios_automation_scope_binding_current(&players, &cluster, NULL));
    independent = binding(&players, RADIOS_AUTOMATION_SCOPE_UNCLUSTERED, 0);
    g_assert_cmpuint(independent.count, ==, 3); g_assert_cmpuint(independent.decks[0], ==, 1);
    g_assert_cmpuint(independent.decks[1], ==, 3); g_assert_cmpuint(independent.decks[2], ==, 5);
}
static void invalid_route(RadioSAutomationBinding *captured, JsonObject *original)
{
    GError *error = NULL; gchar *before = encode(original);
    g_assert_null(radios_automation_scope_route_event(captured, original, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    gchar *after = encode(original); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
}
static void test_routing_bounds_and_empty_scope(void)
{
    RadioSPlayerTopology players = topology(); RadioSAutomationBinding captured = binding(&players, RADIOS_AUTOMATION_SCOPE_CLUSTER, 0);
    JsonObject *original = event();
    json_object_set_object_member(source(original), "autodj", distribution(1, 1)); invalid_route(&captured, original);
    json_object_set_object_member(source(original), "autodj", distribution(33, -1)); invalid_route(&captured, original);
    JsonObject *routing = distribution(1, -1); JsonArray *bad = json_array_new(); json_array_add_double_element(bad, 1.0);
    json_object_set_array_member(routing, "music", bad); json_object_set_object_member(source(original), "autodj", routing); invalid_route(&captured, original);
    json_object_remove_member(source(original), "autodj"); captured.count = 0; invalid_route(&captured, original);
    captured = binding(&players, RADIOS_AUTOMATION_SCOPE_CLUSTER, 0); captured.decks[1] = 0; invalid_route(&captured, original);
    json_object_unref(original);
}
static JsonArray *pool_of(JsonObject *routed_event, guint index, const gchar *kind)
{
    JsonObject *row = json_array_get_object_element(json_object_get_array_member(routed_event, "rotations"), index);
    return json_object_get_array_member(json_object_get_object_member(row, "autodj"), kind);
}
static void assert_pool(JsonArray *pool, const gint64 *expected, guint count)
{
    g_assert_cmpuint(json_array_get_length(pool), ==, count);
    for (guint i = 0; i < count; ++i) g_assert_cmpint(json_array_get_int_element(pool, i), ==, expected[i]);
}
/* PDF §03/§08: a cluster is the group of players used together. A member's
 * stale individual flag (a card left on Yarı AutoDJ before it was moved in)
 * must not make the whole cluster play on one player. */
static void test_cluster_mode_follows_membership(void)
{
    RadioSPlayerTopology players = topology();
    g_assert_true(radios_player_topology_set_autodj(&players, 0, RADIOS_AUTODJ_SEMI, -1, NULL));
    RadioSAutomationBinding cluster = binding(&players, RADIOS_AUTOMATION_SCOPE_CLUSTER, 0);
    g_assert_cmpuint(cluster.count, ==, 2); g_assert_cmpint(cluster.autodj_mode, ==, RADIOS_AUTODJ_TRANSITIONS);
    g_assert_cmpuint(cluster.decks[0], ==, 0); g_assert_cmpuint(cluster.decks[1], ==, 2);
    g_assert_true(radios_automation_scope_binding_current(&players, &cluster, NULL));
    /* The clicked member itself still follows its own definition. */
    RadioSAutomationBinding member = binding(&players, RADIOS_AUTOMATION_SCOPE_PLAYER, 0);
    g_assert_cmpint(member.autodj_mode, ==, RADIOS_AUTODJ_SEMI); g_assert_cmpuint(member.count, ==, 1);
    /* An explicit transition target still orders the first handover. */
    g_assert_true(radios_player_topology_set_autodj(&players, 0, RADIOS_AUTODJ_TRANSITIONS, 2, NULL)); stale(&players, &cluster);
    cluster = binding(&players, RADIOS_AUTOMATION_SCOPE_CLUSTER, 0);
    g_assert_cmpint(cluster.autodj_mode, ==, RADIOS_AUTODJ_TRANSITIONS); g_assert_cmpint(cluster.autodj_target, ==, 2);
    /* A lone member plays alone, even with a leftover transition flag. */
    g_assert_true(radios_player_topology_set_cluster(&players, 2, "", NULL)); stale(&players, &cluster);
    cluster = binding(&players, RADIOS_AUTOMATION_SCOPE_CLUSTER, 0);
    g_assert_cmpuint(cluster.count, ==, 1); g_assert_cmpint(cluster.autodj_mode, ==, RADIOS_AUTODJ_SEMI);
    g_assert_cmpint(players.players[0].autodj_mode, ==, RADIOS_AUTODJ_TRANSITIONS);
    /* The independent aggregate keeps following its first member's own choice. */
    g_assert_true(radios_player_topology_set_autodj(&players, 1, RADIOS_AUTODJ_SEMI, -1, NULL));
    RadioSAutomationBinding independent = binding(&players, RADIOS_AUTOMATION_SCOPE_UNCLUSTERED, 0);
    g_assert_cmpuint(independent.count, ==, 3); g_assert_cmpint(independent.autodj_mode, ==, RADIOS_AUTODJ_SEMI);
}
/* PDF §03 "Seçimler, mevcut oynatıcı sayısı ve track yerleşimiyle uyumlu
 * olmalıdır": a saved pool naming players outside the executing group is
 * clamped to the group's own players and reported in Turkish instead of
 * ending the schedule. */
static void test_out_of_scope_pools_fall_back_with_notice(void)
{
    RadioSPlayerTopology players = topology(); RadioSAutomationBinding captured = binding(&players, RADIOS_AUTOMATION_SCOPE_CLUSTER, 0);
    JsonObject *original = event(); GError *error = NULL; gchar *notice = NULL;
    JsonObject *routing = distribution(1, 3);
    JsonArray *wide = json_array_new(); /* a four-player format run on a two-player cluster */
    for (gint64 slot = 1; slot <= 4; ++slot) json_array_add_int_element(wide, slot);
    json_object_set_array_member(routing, "music", wide);
    JsonArray *foreign = json_array_new(); json_array_add_int_element(foreign, 4); json_array_add_int_element(foreign, 2);
    json_object_set_array_member(routing, "jingle", foreign);
    json_object_set_object_member(source(original), "autodj", routing);
    gchar *before = encode(original);
    JsonNode *routed = radios_automation_scope_route_event_notice(&captured, original, &notice, &error);
    g_assert_no_error(error); g_assert_nonnull(routed); g_assert_nonnull(notice);
    g_assert_nonnull(strstr(notice, "Şemadaki Oynatıcı 2, Oynatıcı 4"));
    g_assert_nonnull(strstr(notice, "bu kümede yok")); g_assert_null(strstr(notice, "Oynatıcı 1")); g_assert_null(strstr(notice, "Oynatıcı 3"));
    const gint64 clamped[] = {1, 3}, saved[] = {1, 3}, carts[] = {4, 2};
    assert_pool(pool_of(json_node_get_object(routed), 0, "music"), clamped, 2);
    /* Jingles may name the unclustered players 4 and 2 beside the cluster. */
    assert_pool(pool_of(json_node_get_object(routed), 0, "jingle"), carts, 2);
    assert_pool(pool_of(json_node_get_object(routed), 0, "advertisement"), saved, 2);
    gchar *after = encode(original); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
    json_node_free(routed); g_free(notice); notice = NULL;
    /* Saved order inside the scope is kept and nothing is reported. */
    json_object_set_object_member(source(original), "autodj", distribution(3, 1));
    routed = radios_automation_scope_route_event_notice(&captured, original, &notice, &error);
    g_assert_no_error(error); g_assert_nonnull(routed); g_assert_null(notice);
    const gint64 reversed[] = {3, 1}; assert_pool(pool_of(json_node_get_object(routed), 0, "music"), reversed, 2);
    json_node_free(routed);
    /* The independent aggregate names its own group. */
    RadioSAutomationBinding independent = binding(&players, RADIOS_AUTOMATION_SCOPE_UNCLUSTERED, 0);
    json_object_set_object_member(source(original), "autodj", distribution(1, 2));
    routed = radios_automation_scope_route_event_notice(&independent, original, &notice, &error);
    g_assert_no_error(error); g_assert_nonnull(routed); g_assert_nonnull(notice);
    g_assert_nonnull(strstr(notice, "Oynatıcı 1 Kümesizler içinde yok"));
    const gint64 own[] = {2}; assert_pool(pool_of(json_node_get_object(routed), 0, "music"), own, 1);
    json_node_free(routed); g_free(notice); notice = NULL;
    /* Malformed pools are still rejected, with the notice left empty. */
    json_object_set_object_member(source(original), "autodj", distribution(1, 1));
    g_assert_null(radios_automation_scope_route_event_notice(&captured, original, &notice, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error); g_assert_null(notice);
    json_object_unref(original);
}
/* "AutoDJ kanallarını seçtiğimde etki etmiyor" (18.10): music on a cluster,
 * advertisements, jingles and sweepers on unclustered cart players. The
 * cluster keeps the music pool; the other kinds keep their chosen players. A
 * player of another cluster is still dropped and reported. */
static void test_cluster_music_with_unclustered_cart_players(void)
{
    RadioSPlayerTopology players; radios_player_topology_default(&players, 6);
    players.cluster_count = 2;
    g_strlcpy(players.clusters[0].id, cluster_id, sizeof players.clusters[0].id);
    g_strlcpy(players.clusters[0].name, "a", sizeof players.clusters[0].name);
    g_strlcpy(players.clusters[1].id, "6a1d2c3b-0000-4000-8000-000000000002", sizeof players.clusters[1].id);
    g_strlcpy(players.clusters[1].name, "b", sizeof players.clusters[1].name);
    for (guint deck = 2; deck < 5; ++deck) g_strlcpy(players.players[deck].cluster_id, cluster_id, sizeof players.players[deck].cluster_id);
    g_strlcpy(players.players[5].cluster_id, players.clusters[1].id, sizeof players.players[5].cluster_id);
    g_assert_true(radios_player_topology_validate(&players, NULL));
    RadioSAutomationBinding captured = binding(&players, RADIOS_AUTOMATION_SCOPE_CLUSTER, 0);
    g_assert_cmpuint(captured.count, ==, 3); g_assert_cmphex(captured.auxiliary, ==, 0x3);
    JsonObject *original = event(), *routing = json_object_new(); gchar *notice = NULL; GError *error = NULL;
    const gint64 music[] = {3, 4, 5}, carts[] = {1, 2}, mixed[] = {6, 1};
    JsonArray *pool = json_array_new(); for (guint i = 0; i < 3; ++i) json_array_add_int_element(pool, music[i]);
    json_object_set_array_member(routing, "music", pool);
    const gchar *spots[] = {"advertisement", "jingle"};
    for (guint k = 0; k < 2; ++k) {
        pool = json_array_new(); for (guint i = 0; i < 2; ++i) json_array_add_int_element(pool, carts[i]);
        json_object_set_array_member(routing, spots[k], pool);
    }
    pool = json_array_new(); for (guint i = 0; i < 2; ++i) json_array_add_int_element(pool, mixed[i]);
    json_object_set_array_member(routing, "sweeper", pool);
    json_object_set_object_member(source(original), "autodj", routing);
    JsonNode *routed = radios_automation_scope_route_event_notice(&captured, original, &notice, &error);
    g_assert_no_error(error); g_assert_nonnull(routed);
    assert_pool(pool_of(json_node_get_object(routed), 0, "music"), music, 3);
    assert_pool(pool_of(json_node_get_object(routed), 0, "advertisement"), carts, 2);
    assert_pool(pool_of(json_node_get_object(routed), 0, "jingle"), carts, 2);
    const gint64 cart_only[] = {1}; assert_pool(pool_of(json_node_get_object(routed), 0, "sweeper"), cart_only, 1);
    g_assert_nonnull(notice); g_assert_nonnull(strstr(notice, "Şemadaki Oynatıcı 6 bu kümede yok"));
    json_node_free(routed); g_free(notice); notice = NULL;
    /* Music naming the cart players is still kept inside the cluster. */
    json_object_set_object_member(source(original), "autodj", distribution(1, 2));
    routed = radios_automation_scope_route_event_notice(&captured, original, &notice, &error);
    g_assert_no_error(error); g_assert_nonnull(routed);
    const gint64 whole[] = {3, 4, 5}; assert_pool(pool_of(json_node_get_object(routed), 0, "music"), whole, 3);
    assert_pool(pool_of(json_node_get_object(routed), 0, "jingle"), carts, 2);
    json_node_free(routed); g_free(notice); json_object_unref(original);
}
/* PDF §03 "Şema, hangi kümenin hangi çalma satırında kullanılacağını ve
 * kümeler arasındaki geçişi belirler": consecutive rows may change their
 * player pool inside the group, and a row routed to another cluster keeps
 * the run alive on this group's players. */
static void test_pool_change_between_rows(void)
{
    RadioSPlayerTopology players = topology(); RadioSAutomationBinding captured = binding(&players, RADIOS_AUTOMATION_SCOPE_CLUSTER, 0);
    JsonObject *original = event(); JsonArray *rotations = json_object_get_array_member(original, "rotations");
    JsonObject *second = radios_programme_clone_object(source(original)), *third = radios_programme_clone_object(source(original));
    json_object_set_string_member(second, "id", "40000000-0000-4000-8000-000000000012");
    json_object_set_string_member(second, "playlist_id", "40000000-0000-4000-8000-000000000013");
    json_object_set_string_member(third, "id", "40000000-0000-4000-8000-000000000022");
    json_object_set_string_member(third, "playlist_id", "40000000-0000-4000-8000-000000000023");
    json_array_add_object_element(rotations, second); json_array_add_object_element(rotations, third);
    json_object_set_object_member(source(original), "autodj", distribution(1, 3));
    json_object_set_object_member(second, "autodj", distribution(3, -1));   /* one row narrows to player 3 */
    json_object_set_object_member(third, "autodj", distribution(2, 4));    /* another cluster's pool */
    GError *error = NULL; gchar *notice = NULL;
    JsonNode *routed = radios_automation_scope_route_event_notice(&captured, original, &notice, &error);
    g_assert_no_error(error); g_assert_nonnull(routed);
    const gint64 pair[] = {1, 3}, single[] = {3};
    assert_pool(pool_of(json_node_get_object(routed), 0, "music"), pair, 2);
    assert_pool(pool_of(json_node_get_object(routed), 1, "music"), single, 1);
    assert_pool(pool_of(json_node_get_object(routed), 2, "music"), pair, 2);
    g_assert_nonnull(notice); g_assert_nonnull(strstr(notice, "Oynatıcı 2, Oynatıcı 4 bu kümede yok"));
    /* The routed event is accepted by the planner's configuration contract. */
    JsonNode *config = radios_automation_calendar_prepare_config(json_node_get_object(routed), NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(config); json_node_free(config);
    json_node_free(routed); g_free(notice); json_object_unref(original);
}
/* PDF §08 "kümesiz çalışma": an independent player on Yarı AutoDJ is a
 * one-player scope; a saved pool naming its independent neighbour cannot
 * route the schema onto that neighbour, and the neighbour's own binding
 * never contains it either. Only an explicit transition choice adds a peer. */
static void test_independent_players_stay_separate(void)
{
    RadioSPlayerTopology players = topology(); /* players 2 and 4 (slots 1, 3) are independent */
    g_assert_true(radios_player_topology_set_autodj(&players, 1, RADIOS_AUTODJ_SEMI, -1, NULL));
    g_assert_true(radios_player_topology_set_autodj(&players, 3, RADIOS_AUTODJ_SEMI, -1, NULL));
    RadioSAutomationBinding first = binding(&players, RADIOS_AUTOMATION_SCOPE_PLAYER, 1);
    RadioSAutomationBinding second = binding(&players, RADIOS_AUTOMATION_SCOPE_PLAYER, 3);
    g_assert_cmpuint(first.count, ==, 1); g_assert_cmpuint(first.decks[0], ==, 1); g_assert_cmpint(first.autodj_target, ==, -1);
    g_assert_cmpuint(second.count, ==, 1); g_assert_cmpuint(second.decks[0], ==, 3); g_assert_cmpint(second.autodj_target, ==, -1);
    JsonObject *original = event(); GError *error = NULL; gchar *notice = NULL;
    json_object_set_object_member(source(original), "autodj", distribution(2, 4));
    JsonNode *routed = radios_automation_scope_route_event_notice(&first, original, &notice, &error);
    g_assert_no_error(error); g_assert_nonnull(routed); g_assert_nonnull(notice);
    g_assert_nonnull(strstr(notice, "Oynatıcı 4 bu oynatıcının kapsamında yok"));
    const gint64 own[] = {2}; assert_pool(pool_of(json_node_get_object(routed), 0, "music"), own, 1);
    json_node_free(routed); g_free(notice); notice = NULL;
    /* The neighbour's own plain (independent) choice is unaffected. */
    g_assert_true(radios_automation_scope_binding_current(&players, &second, NULL));
    /* Only an explicit transition choice brings the next independent player in. */
    g_assert_true(radios_player_topology_set_autodj(&players, 1, RADIOS_AUTODJ_TRANSITIONS, -1, NULL)); stale(&players, &first);
    first = binding(&players, RADIOS_AUTOMATION_SCOPE_PLAYER, 1);
    g_assert_cmpuint(first.count, ==, 2); g_assert_cmpuint(first.decks[1], ==, 3);
    g_assert_true(radios_automation_scope_binding_current(&players, &second, NULL));
    json_object_unref(original);
}
static JsonObject *record(void)
{
    JsonObject *calendar = json_object_new(); json_object_set_int_member(calendar, "schema", 1);
    JsonArray *events = json_array_new(); json_array_add_object_element(events, event()); json_object_set_array_member(calendar, "events", events);
    GError *error = NULL; JsonNode *config = radios_weekly_programme_snapshot(calendar, "2026-10-12", TRUE, &error);
    g_assert_no_error(error); g_assert_nonnull(config); json_object_unref(calendar);
    JsonObject *result = json_object_new();
    json_object_set_string_member(result, "_id", schema_id); json_object_set_int_member(result, "_rev", 4);
    json_object_set_string_member(result, "station_id", station_id); json_object_set_string_member(result, "kind", "weekly_programme");
    json_object_set_string_member(result, "status", "active"); json_object_set_string_member(result, "name", "Sabah şeması");
    json_object_set_member(result, "config", config); return result;
}
static void invalid_record(JsonObject *saved, const gchar *station, const gchar *schema, gint64 revision)
{
    GError *error = NULL;
    g_assert_false(radios_automation_scope_record_valid(saved, station, schema, revision, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
}
static void test_exact_schema_identity_and_revision(void)
{
    JsonObject *saved = record(); GError *error = NULL; gchar *before = encode(saved);
    g_assert_true(radios_automation_scope_record_valid(saved, station_id, schema_id, 0, &error)); g_assert_no_error(error);
    g_assert_true(radios_automation_scope_record_valid(saved, station_id, schema_id, 4, &error)); g_assert_no_error(error);
    invalid_record(saved, station_id, schema_id, 3); invalid_record(saved, override_id, schema_id, 0);
    invalid_record(saved, station_id, override_id, 0); invalid_record(saved, station_id, schema_id, -1);
    gchar *after = encode(saved); g_assert_cmpstr(before, ==, after); g_free(before); g_free(after);
    json_object_set_string_member(saved, "status", "disabled"); invalid_record(saved, station_id, schema_id, 0);
    json_object_set_string_member(saved, "status", "active"); json_object_set_string_member(saved, "kind", "playlist"); invalid_record(saved, station_id, schema_id, 0);
    json_object_set_string_member(saved, "kind", "weekly_programme");
    json_object_set_int_member(json_object_get_object_member(saved, "config"), "week_count", 0); invalid_record(saved, station_id, schema_id, 0);
    json_object_unref(saved);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/automation-scope/membership-inheritance", test_membership_and_inheritance);
    g_test_add_func("/automation-scope/semi-transition-pools", test_semi_and_transition_scopes);
    g_test_add_func("/automation-scope/stale-binding-fences", test_stale_binding_fences);
    g_test_add_func("/automation-scope/saved-rotation-exact-pool", test_saved_rotation_exact_pool);
    g_test_add_func("/automation-scope/ephemeral-routing", test_ephemeral_routing_and_saved_pools);
    g_test_add_func("/automation-scope/group-explicit-target-order", test_group_explicit_target_order);
    g_test_add_func("/automation-scope/routing-bounds", test_routing_bounds_and_empty_scope);
    g_test_add_func("/automation-scope/cluster-mode-follows-membership", test_cluster_mode_follows_membership);
    g_test_add_func("/automation-scope/out-of-scope-pools-fall-back", test_out_of_scope_pools_fall_back_with_notice);
    g_test_add_func("/automation-scope/cluster-music-unclustered-cart-players", test_cluster_music_with_unclustered_cart_players);
    g_test_add_func("/automation-scope/pool-change-between-rows", test_pool_change_between_rows);
    g_test_add_func("/automation-scope/independent-players-stay-separate", test_independent_players_stay_separate);
    g_test_add_func("/automation-scope/schema-identity-revision", test_exact_schema_identity_and_revision);
    return g_test_run();
}
