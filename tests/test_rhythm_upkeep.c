/* Library upkeep end to end: the real library_scan of src/ui/rhythm_jobs.c
 * (BPM grids, then automatic AutoDJ IN/OUT points) against a private data
 * service. tests/run_rhythm_upkeep.py starts the service and checks the store.
 *
 *   analyze FILE              prints the native analysis JSON of FILE
 *   scan ROOT DATABASE URL [WORKERS]
 *                             runs two upkeep passes (WORKERS tracks at a time, default 4)
 *                             and prints their summaries
 */
#include "ui/rhythm_jobs.c"
#include "radios/data_client.h"
#include <stdio.h>

/* The widgets and the live data layer are not part of the scan; only the
 * helpers it reaches are provided here. */
const gchar *radios_json_string(JsonObject *object, const gchar *key, const gchar *fallback)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : fallback;
}
gint64 radios_json_int(JsonObject *object, const gchar *key, gint64 fallback)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64 ? json_node_get_int(node) : fallback;
}
gboolean radios_data_connected(RadioSViewContext *ctx) { (void)ctx; return TRUE; }
RadioSDataClient *radios_data_client_acquire(RadioSViewContext *ctx) { (void)ctx; return NULL; }
void radios_data_refresh(RadioSViewContext *ctx) { (void)ctx; }
guint radios_media_catalogue_generation(RadioSViewContext *ctx) { (void)ctx; return 0; }

static int analyze(const char *path)
{
    GError *error = NULL;
    RadioSMediaAnalysis *analysis = radios_media_analyze(path, NULL, NULL, &error);
    if (!analysis) { fprintf(stderr, "analiz: %s\n", error ? error->message : "?"); return 1; }
    JsonNode *node = radios_media_analysis_to_json(analysis);
    gchar *text = json_to_string(node, FALSE); puts(text);
    g_free(text); json_node_free(node); radios_media_analysis_free(analysis);
    return 0;
}
static int scan(char **argv)
{
    RadioSConfig config = {0};
    config.project_root = argv[0]; config.database_file = argv[1]; config.service_url = argv[2];
    config.token_environment = g_strdup("RADIOS_UPKEEP_TEST_TOKEN_UNSET");
    config.connect_timeout_seconds = 5; config.request_timeout_seconds = 60;
    GError *error = NULL;
    RadioSDataClient *client = radios_data_client_new(&config, &error);
    if (!client) { fprintf(stderr, "istemci: %s\n", error ? error->message : "?"); return 1; }
    guint workers = argv[3] ? (guint)g_ascii_strtoull(argv[3], NULL, 10) : 4;
    GHashTable *settled = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint pass = 1; pass <= 2; ++pass) {
        JsonNode *result = library_scan(client, settled, workers, NULL, &error);
        if (!result) { fprintf(stderr, "tarama %u: %s\n", pass, error ? error->message : "?"); return 1; }
        gchar *text = json_to_string(result, FALSE); printf("pass %u: %s\n", pass, text); fflush(stdout); g_free(text);
        /* Like the application, remember tracks that cannot be marked. */
        JsonArray *added = json_object_get_array_member(json_node_get_object(result), "settled");
        for (guint i = 0; added && i < json_array_get_length(added); ++i)
            g_hash_table_add(settled, g_strdup(json_array_get_string_element(added, i)));
        json_node_free(result);
    }
    g_hash_table_unref(settled); radios_data_client_unref(client); g_free(config.token_environment);
    return 0;
}
int main(int argc, char **argv)
{
    if (argc == 3 && g_str_equal(argv[1], "analyze")) return analyze(argv[2]);
    if ((argc == 5 || argc == 6) && g_str_equal(argv[1], "scan")) return scan(argv + 2); /* argv[argc] is NULL: no worker count */
    fprintf(stderr, "kullanım: analyze DOSYA | scan KÖK VERİTABANI URL [İŞÇİ]\n");
    return 2;
}
