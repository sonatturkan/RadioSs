#include "radios/routing_graph.h"
#include <string.h>

/* Graph rules: every block kind is reusable, cables cannot loop back
 * through the mixer or a monitored device, JSON and legacy documents load. */
static guint add(RadioSRoutingGraph *graph, RadioSRoutingGraphKind kind)
{
    guint id = 0; GError *error = NULL;
    g_assert_true(radios_routing_graph_add(graph, kind, 10 + graph->node_count * 20, 20, &id, &error)); g_assert_no_error(error);
    return id;
}
static void reject(RadioSRoutingGraph *graph, guint from, guint to)
{
    RadioSRoutingGraph before = *graph; GError *error = NULL;
    g_assert_false(radios_routing_graph_connect(graph, from, to, &error)); g_assert_nonnull(error);
    g_test_message("rejected %u -> %u: %s", from, to, error->message); g_clear_error(&error);
    g_assert_cmpmem(&before, sizeof before, graph, sizeof *graph);
}
static void same_node(const RadioSRoutingGraphNode *a, const RadioSRoutingGraphNode *b)
{
    g_assert_cmpuint(a->id, ==, b->id); g_assert_cmpint(a->kind, ==, b->kind);
    g_assert_cmpfloat(a->x, ==, b->x); g_assert_cmpfloat(a->y, ==, b->y); g_assert_cmpfloat(a->gain_db, ==, b->gain_db);
    g_assert_cmpstr(a->name, ==, b->name); g_assert_cmpstr(a->device, ==, b->device);
    g_assert_cmpuint(a->channel, ==, b->channel); g_assert_cmpint(a->push_to_talk, ==, b->push_to_talk);
    g_assert_cmpint(a->as_microphone, ==, b->as_microphone);
    g_assert_cmpstr(a->folder, ==, b->folder); g_assert_cmpstr(a->prefix, ==, b->prefix); g_assert_cmpuint(a->split_minutes, ==, b->split_minutes);
    g_assert_cmpint(a->protocol, ==, b->protocol); g_assert_cmpstr(a->host, ==, b->host); g_assert_cmpstr(a->mount, ==, b->mount);
    g_assert_cmpstr(a->username, ==, b->username); g_assert_cmpuint(a->port, ==, b->port); g_assert_cmpuint(a->stream_id, ==, b->stream_id);
    g_assert_cmpuint(a->bitrate_kbps, ==, b->bitrate_kbps); g_assert_cmpstr(a->station, ==, b->station);
    g_assert_cmpstr(a->genre, ==, b->genre); g_assert_cmpstr(a->description, ==, b->description);
    g_assert_cmpstr(a->website, ==, b->website); g_assert_cmpint(a->public_listing, ==, b->public_listing);
    g_assert_cmpint(a->password_saved, ==, b->password_saved); g_assert_cmpstr(a->password, ==, b->password);
}
/* Stream passwords: stored and reloaded only with "Şifreyi kaydet"; a bare
 * password, in the block or in a document, is rejected. */
static void stream_password(void)
{
    RadioSRoutingGraph *graph = g_new0(RadioSRoutingGraph, 1); GError *error = NULL;
    guint main_block = add(graph, RG_MAIN), stream = add(graph, RG_STREAM);
    g_assert_true(radios_routing_graph_connect(graph, main_block, stream, NULL));
    RadioSRoutingGraphNode copy = *radios_routing_graph_find(graph, stream);
    g_strlcpy(copy.host, "yayin.example", sizeof copy.host);
    g_assert_true(radios_routing_graph_update(graph, &copy, NULL));
    JsonObject *json = radios_routing_graph_to_json(graph); g_assert_nonnull(json);
    JsonObject *settings = json_object_get_object_member(json_array_get_object_element(json_object_get_array_member(json, "nodes"), 1), "settings");
    g_assert_false(json_object_has_member(settings, "password")); g_assert_false(json_object_has_member(settings, "password_saved"));
    /* An unsaved password never enters the block. */
    g_strlcpy(copy.password, "gizli", sizeof copy.password);
    g_assert_false(radios_routing_graph_update(graph, &copy, &error)); g_assert_nonnull(error); g_clear_error(&error);
    g_assert_cmpstr(radios_routing_graph_find(graph, stream)->password, ==, "");
    copy.password_saved = TRUE;
    g_assert_true(radios_routing_graph_update(graph, &copy, &error)); g_assert_no_error(error);
    json_object_unref(json); json = radios_routing_graph_to_json(graph); g_assert_nonnull(json);
    settings = json_object_get_object_member(json_array_get_object_element(json_object_get_array_member(json, "nodes"), 1), "settings");
    g_assert_true(json_object_get_boolean_member(settings, "password_saved"));
    g_assert_cmpstr(json_object_get_string_member(settings, "password"), ==, "gizli");
    RadioSRoutingGraph *loaded = g_new0(RadioSRoutingGraph, 1);
    g_assert_true(radios_routing_graph_from_json(loaded, json, &error)); g_assert_no_error(error);
    same_node(radios_routing_graph_find(graph, stream), radios_routing_graph_find(loaded, stream));
    g_assert_cmpstr(radios_routing_graph_find(loaded, stream)->password, ==, "gizli");
    /* Documents: a password without the flag, or a flag without a password, is invalid. */
    RadioSRoutingGraph before = *loaded;
    json_object_remove_member(settings, "password_saved");
    g_assert_false(radios_routing_graph_from_json(loaded, json, &error)); g_clear_error(&error);
    g_assert_cmpmem(&before, sizeof before, loaded, sizeof *loaded);
    json_object_set_boolean_member(settings, "password_saved", FALSE);
    g_assert_false(radios_routing_graph_from_json(loaded, json, &error)); g_clear_error(&error);
    json_object_set_boolean_member(settings, "password_saved", TRUE); json_object_remove_member(settings, "password");
    g_assert_false(radios_routing_graph_from_json(loaded, json, &error)); g_clear_error(&error);
    json_object_set_string_member(settings, "password", "a\tb");
    g_assert_false(radios_routing_graph_from_json(loaded, json, &error)); g_clear_error(&error);
    g_assert_cmpmem(&before, sizeof before, loaded, sizeof *loaded);
    /* Unticking "Şifreyi kaydet" drops the stored password and the JSON fields. */
    copy = *radios_routing_graph_find(graph, stream); copy.password_saved = FALSE; copy.password[0] = 0;
    g_assert_true(radios_routing_graph_update(graph, &copy, NULL));
    json_object_unref(json); json = radios_routing_graph_to_json(graph);
    settings = json_object_get_object_member(json_array_get_object_element(json_object_get_array_member(json, "nodes"), 1), "settings");
    g_assert_false(json_object_has_member(settings, "password"));
    json_object_unref(json); g_free(graph); g_free(loaded);
}
static void graph_rules(void)
{
    RadioSRoutingGraph *graph = g_new0(RadioSRoutingGraph, 1);
    guint main_block = add(graph, RG_MAIN), meter = add(graph, RG_METER), recorder = add(graph, RG_RECORDER);
    guint second_recorder = add(graph, RG_RECORDER), mixer = add(graph, RG_MIXER), same_channel = add(graph, RG_MIXER);
    guint input = add(graph, RG_INPUT), output = add(graph, RG_OUTPUT), virtual = add(graph, RG_VIRTUAL), monitor = add(graph, RG_INPUT);
    g_assert_cmpuint(graph->node_count, ==, 10); g_assert_cmpuint(second_recorder, !=, recorder);
    g_assert_true(radios_routing_graph_connect(graph, main_block, meter, NULL));
    g_assert_true(radios_routing_graph_connect(graph, meter, recorder, NULL));
    g_assert_true(radios_routing_graph_connect(graph, meter, second_recorder, NULL));
    reject(graph, recorder, meter);            /* recorders have no output */
    reject(graph, input, input);               /* self */
    reject(graph, main_block, mixer);          /* MAIN back into the mixer */
    reject(graph, meter, mixer);               /* ...also through a meter */
    reject(graph, main_block, add(graph, RG_MAIN)); /* MAIN into MAIN */
    /* Two blocks of mixer channel 1: its output may not feed its own input. */
    g_assert_true(radios_routing_graph_connect(graph, mixer, output, NULL));
    reject(graph, same_channel, mixer);
    RadioSRoutingGraphNode copy = *radios_routing_graph_find(graph, same_channel); copy.channel = 2;
    g_assert_true(radios_routing_graph_update(graph, &copy, NULL));
    g_assert_true(radios_routing_graph_connect(graph, same_channel, mixer, NULL)); /* channel 2 -> channel 1 */
    /* A virtual interface or output cannot reach a capture of its own monitor. */
    copy = *radios_routing_graph_find(graph, monitor);
    g_snprintf(copy.device, sizeof copy.device, "radios_virtual_%u.monitor", virtual);
    g_assert_true(radios_routing_graph_update(graph, &copy, NULL));
    g_assert_true(radios_routing_graph_connect(graph, input, virtual, NULL));
    reject(graph, monitor, virtual);
    copy = *radios_routing_graph_find(graph, output); g_strlcpy(copy.device, "alsa_output.card", sizeof copy.device);
    g_assert_true(radios_routing_graph_update(graph, &copy, NULL));
    copy = *radios_routing_graph_find(graph, input); g_strlcpy(copy.device, "alsa_output.card.monitor", sizeof copy.device);
    g_assert_true(radios_routing_graph_update(graph, &copy, NULL));
    reject(graph, virtual, output); /* input (monitor of the card) -> virtual -> card */
    /* Names, kinds and settings round-trip through JSON. */
    g_assert_true(radios_routing_graph_rename(graph, recorder, "  Gece kaydı  ", NULL));
    g_assert_cmpstr(radios_routing_graph_find(graph, recorder)->name, ==, "Gece kaydı");
    copy = *radios_routing_graph_find(graph, second_recorder); copy.split_minutes = 60; g_strlcpy(copy.prefix, "yedek", sizeof copy.prefix);
    g_assert_true(radios_routing_graph_update(graph, &copy, NULL));
    guint stream = add(graph, RG_STREAM); copy = *radios_routing_graph_find(graph, stream);
    copy.protocol = RG_STREAM_SHOUTCAST2; g_strlcpy(copy.host, "yayin.example", sizeof copy.host); copy.stream_id = 3;
    g_strlcpy(copy.station, "RadioS Test", sizeof copy.station); copy.public_listing = TRUE;
    g_assert_true(radios_routing_graph_update(graph, &copy, NULL));
    g_assert_true(radios_routing_graph_connect(graph, meter, stream, NULL));
    GError *error = NULL; copy.port = 0;
    g_assert_false(radios_routing_graph_update(graph, &copy, &error)); g_clear_error(&error);
    JsonObject *json = radios_routing_graph_to_json(graph); g_assert_nonnull(json);
    RadioSRoutingGraph *loaded = g_new0(RadioSRoutingGraph, 1);
    g_assert_true(radios_routing_graph_from_json(loaded, json, &error)); g_assert_no_error(error);
    g_assert_cmpuint(loaded->node_count, ==, graph->node_count); g_assert_cmpuint(loaded->edge_count, ==, graph->edge_count);
    for (guint i = 0; i < graph->node_count; ++i) same_node(&graph->nodes[i], &loaded->nodes[i]);
    json_object_set_string_member(json_array_get_object_element(json_object_get_array_member(json, "nodes"), 0), "kind", "compressor");
    RadioSRoutingGraph before = *loaded;
    g_assert_false(radios_routing_graph_from_json(loaded, json, &error)); g_clear_error(&error);
    g_assert_cmpmem(&before, sizeof before, loaded, sizeof *loaded);
    json_object_unref(json);
    guint order[RADIOS_ROUTING_GRAPH_MAX_NODES]; g_assert_true(radios_routing_graph_order(graph, order));
    for (guint e = 0; e < graph->edge_count; ++e) {
        guint from = 0, to = 0;
        for (guint i = 0; i < graph->node_count; ++i) { if (order[i] == graph->edges[e].from) from = i; if (order[i] == graph->edges[e].to) to = i; }
        g_assert_cmpuint(from, <, to);
    }
    /* Removing a block removes its cables. */
    g_assert_true(radios_routing_graph_remove(graph, meter, NULL));
    for (guint e = 0; e < graph->edge_count; ++e) { g_assert_cmpuint(graph->edges[e].from, !=, meter); g_assert_cmpuint(graph->edges[e].to, !=, meter); }
    g_free(graph); g_free(loaded);
}
static void legacy_document(void)
{
    JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, "{\"schema_version\":1,\"application_name\":\"\",\"application_binary\":\"\","
        "\"output_sink\":\"alsa_output.x\",\"microphone_source\":\"alsa_input.mic\",\"application_enabled\":true,"
        "\"microphone_enabled\":true,\"application_gain_db\":0,\"microphone_gain_db\":4.5,\"compressor\":\"voice\","
        "\"graph\":{\"nodes\":[{\"id\":1,\"kind\":\"application\",\"x\":40,\"y\":40},{\"id\":2,\"kind\":\"microphone\",\"x\":40,\"y\":240},"
        "{\"id\":3,\"kind\":\"compressor\",\"x\":300,\"y\":240},{\"id\":4,\"kind\":\"output\",\"x\":300,\"y\":40},"
        "{\"id\":6,\"kind\":\"recorder\",\"x\":600,\"y\":130}],\"edges\":[{\"from\":1,\"to\":4},{\"from\":2,\"to\":3},{\"from\":3,\"to\":6}]}}", -1, NULL));
    RadioSRoutingGraph *graph = g_new0(RadioSRoutingGraph, 1);
    radios_routing_graph_from_legacy(graph, json_node_get_object(json_parser_get_root(parser)));
    g_assert_cmpuint(graph->node_count, ==, 3);
    const RadioSRoutingGraphNode *mic = radios_routing_graph_find(graph, 2);
    g_assert_nonnull(mic); g_assert_cmpint(mic->kind, ==, RG_MICROPHONE); g_assert_cmpstr(mic->device, ==, "alsa_input.mic");
    g_assert_cmpfloat(mic->gain_db, ==, 4.5);
    g_assert_cmpstr(radios_routing_graph_find(graph, 4)->device, ==, "alsa_output.x");
    g_assert_true(radios_routing_graph_has_edge(graph, 2, 6)); g_assert_cmpuint(graph->edge_count, ==, 1);
    g_object_unref(parser); g_free(graph);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/routing-graph/rules-loops-json", graph_rules);
    g_test_add_func("/routing-graph/legacy-document", legacy_document);
    g_test_add_func("/routing-graph/stream-password-saved-only", stream_password);
    return g_test_run();
}
