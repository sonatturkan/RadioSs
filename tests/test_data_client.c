#define _XOPEN_SOURCE 700

#include "radios/data_client.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STORE_ID "11111111-1111-4111-8111-111111111111"
#define USER_ID "22222222-2222-4222-8222-222222222222"
#define STATION_ID "33333333-3333-4333-8333-333333333333"
#define ENDPOINT_ID "44444444-4444-4444-8444-444444444444"
#define USER_TOKEN "TestUserPrivateToken_01234567890123456789"
#define ENDPOINT_TOKEN "TestEndpointPrivateToken_0123456789012345"
#define OVERRIDE_TOKEN "EnvPrivateToken_ABCDEFGHIJKLMNOPQRSTUVWXYZ"

typedef struct {
    GSocketListener *listener;
    GCancellable *cancel;
    GThread *thread;
    guint16 port;
    gint requests;
    gint slow_started;
    gint bad_store;
    gint bad_schema;
    gint slow_ms;
    gpointer expected_token;
    gint durable_drop, durable_reject, durable_commits, durable_replays, durable_attempts;
    gint wrong_identity, bad_api;
    gpointer source_digest;
    gpointer transfer_payload;
    gchar *limited_payload;
    gchar *durable_payload;
    gpointer limited_path;
    gint limited_remaining, limited_attempts, limited_delay;
    gint workflow_features;
} TestServer;

typedef struct {
    gchar *root;
    gchar *credential_file;
    RadioSConfig config;
    TestServer server;
    RadioSDataClient *client;
} Fixture;

static void
remove_tree(const gchar *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
        GDir *directory = g_dir_open(path, 0, NULL);
        const gchar *name;
        while ((name = g_dir_read_name(directory)) != NULL) {
            gchar *child = g_build_filename(path, name, NULL);
            remove_tree(child);
            g_free(child);
        }
        g_dir_close(directory);
        g_rmdir(path);
    } else {
        g_unlink(path);
    }
}

static void
serve_connection(TestServer *server, GSocketConnection *connection)
{
    GDataInputStream *input = g_data_input_stream_new(g_io_stream_get_input_stream(G_IO_STREAM(connection)));
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    g_socket_set_timeout(g_socket_connection_get_socket(connection), 3);
    gchar *first = g_data_input_stream_read_line(input, NULL, NULL, NULL);
    if (first == NULL) {
        g_object_unref(input);
        return;
    }
    gchar **words = g_strsplit(first, " ", 3);
    const gchar *path = words[1] != NULL ? words[1] : "/invalid";
    gboolean auth = FALSE;
    gsize length = 0;
    gchar *line;
    while ((line = g_data_input_stream_read_line(input, NULL, NULL, NULL)) != NULL) {
        g_strchomp(line);
        gboolean empty = *line == '\0';
        if (g_str_has_prefix(line, "Authorization: Bearer "))
            auth = g_str_equal(line + strlen("Authorization: Bearer "), g_atomic_pointer_get(&server->expected_token));
        if (g_str_has_prefix(line, "Content-Length: "))
            length = (gsize) g_ascii_strtoull(line + strlen("Content-Length: "), NULL, 10);
        g_free(line);
        if (empty)
            break;
    }
    gchar *request_body = NULL;
    if (length > 0 && length <= 6 * 1024 * 1024) {
        request_body = g_malloc0(length + 1);
        g_input_stream_read_all(G_INPUT_STREAM(input), request_body, length, NULL, NULL, NULL);
    }
    g_atomic_int_inc(&server->requests);
    gint status = 200;
    gchar *body = g_strdup("[]");
    const gchar *extra_header = "";
    gchar *rate_header = NULL;
    const gchar *limited_path = g_atomic_pointer_get(&server->limited_path);
    gboolean limited = limited_path && g_str_equal(path, limited_path);
    if (limited) {
        g_atomic_int_inc(&server->limited_attempts);
        if (request_body) {
            if (!server->limited_payload) server->limited_payload = g_strdup(request_body);
            else g_assert_cmpstr(request_body, ==, server->limited_payload);
        }
    }
    if (!auth) {
        status = 403; g_free(body); body = g_strdup("{\"error\":\"forbidden\"}");
    } else if (limited && g_atomic_int_get(&server->limited_remaining)) {
        status = 429;
        if (g_atomic_int_get(&server->limited_remaining) > 0) g_atomic_int_add(&server->limited_remaining, -1);
        rate_header = g_strdup_printf("Retry-After: %d\r\n", g_atomic_int_get(&server->limited_delay));
        extra_header = rate_header;
    } else if (g_str_equal(path, "/health")) {
        g_free(body);
        body = g_strdup_printf("{\"status\":\"ok\",\"store_id\":\"%s\"}",
            g_atomic_int_get(&server->bad_store) ? USER_ID : STORE_ID);
    } else if (g_str_equal(path, "/v1/capabilities")) {
        g_free(body); body = g_strdup_printf("{\"schema_version\":%d,\"desktop_api_version\":%d,\"source_digest\":\"%s\",%s\"desktop_configuration_kinds\":[\"automation\",\"automation_calendar\",\"weekly_programme\",\"live_players\"]}",
            g_atomic_int_get(&server->bad_schema) ? 3 : 4, g_atomic_int_get(&server->bad_api) ? 0 : 1,
            g_atomic_pointer_get(&server->source_digest) ? (const gchar *)g_atomic_pointer_get(&server->source_digest) : "",
            g_atomic_int_get(&server->workflow_features) ? "\"workflow_features\":[\"kind_transition_distribution\",7]," : "");
    } else if (g_str_equal(path, "/v1/workflows/desktop/context")) {
        g_free(body); body = g_strdup_printf("{\"user\":{\"_id\":\"%s\"},\"station\":{\"_id\":\"%s\"}}",
            g_atomic_int_get(&server->wrong_identity) ? STORE_ID : USER_ID, STATION_ID);
    } else if (g_str_has_prefix(path, "/query?station_id=")) {
        g_free(body); body = g_strdup("{\"query_seen\":true}");
    } else if (g_str_equal(path, "/v1/workflows/desktop/put") || g_str_equal(path, "/v1/workflows/desktop-rekordbox/apply")) {
        g_atomic_int_inc(&server->durable_attempts);
        gint rejection = g_atomic_int_get(&server->durable_reject);
        if (rejection) { status = rejection == 1 ? 403 : rejection; }
        else {
            g_assert_nonnull(request_body);
            if (!server->durable_payload) { server->durable_payload = g_strdup(request_body); g_atomic_int_inc(&server->durable_commits); }
            else { g_assert_cmpstr(request_body, ==, server->durable_payload); g_atomic_int_inc(&server->durable_replays); }
            if (g_atomic_int_compare_and_exchange(&server->durable_drop, 1, 0)) {
                /* The real socket closes after commit, before any HTTP reply. */
                g_free(body); g_free(request_body); g_strfreev(words); g_free(first); g_object_unref(input); return;
            }
            g_free(body); body = g_strdup("{\"record\":{\"_rev\":1},\"replayed\":true}");
        }
    } else if ((g_str_has_prefix(path, "/v1/workflows/transfers/") || g_str_has_prefix(path, "/v1/workflows/desktop-playlists/")) &&
               g_atomic_pointer_get(&server->transfer_payload)) {
        g_free(body); body = g_strdup(g_atomic_pointer_get(&server->transfer_payload));
    } else if (g_str_equal(path, "/echo")) {
        g_free(body); body = request_body != NULL ? g_strdup(request_body) : g_strdup("{}");
    } else if (g_str_equal(path, "/slow")) {
        g_atomic_int_set(&server->slow_started, 1);
        g_usleep((gulong) g_atomic_int_get(&server->slow_ms) * 1000);
    } else if (g_str_equal(path, "/oversized")) {
        g_free(body); body = g_malloc(2 * 1024 * 1024 + 33);
        memset(body, 'x', 2 * 1024 * 1024 + 32); body[2 * 1024 * 1024 + 32] = '\0';
    } else if (g_str_equal(path, "/broken")) {
        g_free(body); body = g_strdup("this is not JSON");
    } else if (g_str_equal(path, "/scalar")) {
        g_free(body); body = g_strdup("42");
    } else if (g_str_equal(path, "/duplicate")) {
        g_free(body); body = g_strdup("{\"same\":1,\"same\":2}");
    } else if (g_str_equal(path, "/redirect")) {
        status = 302; extra_header = "Location: http://127.0.0.1:1/forbidden\r\n";
    } else if (g_str_equal(path, "/committed")) {
        status = 503; g_free(body); body = g_strdup("{\"committed\":true,\"error\":\"archive_acknowledgement_failed\"}");
    } else if (g_str_equal(path, "/limited")) {
        status = 429; extra_header = "Retry-After: 7\r\n";
    } else if (g_str_equal(path, "/missing")) {
        status = 404;
    } else if (g_str_equal(path, "/conflict")) {
        status = 409;
    } else if (g_str_equal(path, "/validation")) {
        status = 422; g_free(body);
        body = g_strdup("{\"detail\":\"invalid value " USER_TOKEN " " ENDPOINT_TOKEN "\\nform field\"}");
    } else if (g_str_equal(path, "/unavailable")) {
        status = 503;
    } else if (g_str_equal(path, "/forbidden")) {
        status = 403;
    }
    gchar *header = g_strdup_printf("HTTP/1.1 %d Mock\r\nContent-Type: application/json\r\nContent-Length: %" G_GSIZE_FORMAT
        "\r\nConnection: close\r\n%s\r\n", status, strlen(body), extra_header);
    GError *write_error = NULL;
    if (g_output_stream_write_all(output, header, strlen(header), NULL, NULL, &write_error))
        g_output_stream_write_all(output, body, strlen(body), NULL, NULL, &write_error);
    g_clear_error(&write_error);
    g_free(rate_header); g_free(header); g_free(body); g_free(request_body); g_strfreev(words); g_free(first);
    g_object_unref(input);
}

static gpointer
server_main(gpointer data)
{
    TestServer *server = data;
    while (!g_cancellable_is_cancelled(server->cancel)) {
        GSocketConnection *connection = g_socket_listener_accept(server->listener, NULL, server->cancel, NULL);
        if (connection == NULL)
            break;
        serve_connection(server, connection);
        g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
        g_object_unref(connection);
    }
    return NULL;
}

static void
fixture_start(Fixture *fixture)
{
    *fixture = (Fixture) {0};
    gchar *template = g_build_filename(RADIOS_TEST_PROJECT_ROOT, "build/data-client-XXXXXX", NULL);
    fixture->root = g_mkdtemp(template);
    g_assert_nonnull(fixture->root);
    gchar *directory = g_build_filename(fixture->root, "config", NULL);
    g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
    fixture->credential_file = g_build_filename(directory, "credentials.json", NULL);
    g_free(directory);
    fixture->config.project_root = g_strdup(fixture->root);
    fixture->config.database_file = g_build_filename(fixture->root, "RadioS.sdb", NULL);
    g_assert_true(g_file_set_contents(fixture->config.database_file, "mock journal", -1, NULL));
    gchar *meta = g_strconcat(fixture->config.database_file, ".meta", NULL);
    g_assert_true(g_file_set_contents(meta, "{\"schema_version\":4,\"store_id\":\"" STORE_ID "\"}", -1, NULL));
    g_free(meta);
    const gchar *credentials = "{\"schema_version\":1,\"store_id\":\"" STORE_ID "\","
        "\"user_id\":\"" USER_ID "\",\"station_id\":\"" STATION_ID "\","
        "\"endpoint_id\":\"" ENDPOINT_ID "\",\"database_file\":\"RadioS.sdb\","
        "\"api_token\":\"" USER_TOKEN "\",\"endpoint_token\":\"" ENDPOINT_TOKEN "\"}";
    g_assert_true(g_file_set_contents(fixture->credential_file, credentials, -1, NULL));
    g_assert_cmpint(g_chmod(fixture->credential_file, 0600), ==, 0);
    TestServer *server = &fixture->server;
    server->listener = g_socket_listener_new(); server->cancel = g_cancellable_new();
    server->slow_ms = 1800;
    server->expected_token = (gpointer) USER_TOKEN;
    GError *error = NULL;
    server->port = g_socket_listener_add_any_inet_port(server->listener, NULL, &error);
    g_assert_no_error(error); g_assert_cmpuint(server->port, >, 0);
    fixture->config.service_url = g_strdup_printf("http://127.0.0.1:%u", server->port);
    fixture->config.token_environment = g_strdup("RADIOS_DATA_CLIENT_TEST_TOKEN_UNSET");
    fixture->config.connect_timeout_seconds = 1; fixture->config.request_timeout_seconds = 3;
    fixture->client = radios_data_client_new(&fixture->config, &error);
    g_assert_no_error(error); g_assert_nonnull(fixture->client);
    server->thread = g_thread_new("data-client-mock", server_main, server);
}

static void
fixture_clear(Fixture *fixture)
{
    g_cancellable_cancel(fixture->server.cancel);
    g_thread_join(fixture->server.thread);
    g_socket_listener_close(fixture->server.listener);
    g_object_unref(fixture->server.listener); g_object_unref(fixture->server.cancel);
    g_free(fixture->server.durable_payload); g_free(fixture->server.limited_payload);
    g_free(g_atomic_pointer_get(&fixture->server.transfer_payload));
    radios_data_client_unref(fixture->client); radios_config_clear(&fixture->config);
    remove_tree(fixture->root); g_free(fixture->root); g_free(fixture->credential_file);
}

static void
expect_error(Fixture *fixture, const gchar *method, const gchar *path, RadioSDataClientError code)
{
    GError *error = NULL;
    JsonNode *node = radios_data_client_request_sync(fixture->client, method, path, NULL, NULL, &error);
    g_assert_null(node); g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, (gint) code);
    g_assert_null(strstr(error->message, USER_TOKEN)); g_assert_null(strstr(error->message, ENDPOINT_TOKEN));
    g_error_free(error);
}

static void
test_handshake_and_json(void)
{
    Fixture fixture; fixture_start(&fixture);
    GError *error = NULL;
    g_assert_true(radios_data_client_handshake_sync(fixture.client, NULL, &error)); g_assert_no_error(error);
    g_assert_cmpstr(radios_data_client_store_id(fixture.client), ==, STORE_ID);
    g_assert_cmpstr(radios_data_client_station_id(fixture.client), ==, STATION_ID);
    g_assert_cmpstr(radios_data_client_endpoint_id(fixture.client), ==, ENDPOINT_ID);
    g_assert_cmpstr(radios_data_client_user_id(fixture.client), ==, USER_ID);
    JsonNode *node = radios_data_client_request_sync(fixture.client, "GET", "/array", NULL, NULL, &error);
    g_assert_no_error(error); g_assert_true(JSON_NODE_HOLDS_ARRAY(node)); json_node_unref(node);
    node = radios_data_client_request_sync(fixture.client, "GET", "/query?station_id=" STATION_ID "&name=T%C3%BCrk%C3%A7e", NULL, NULL, &error);
    g_assert_no_error(error); g_assert_true(json_object_get_boolean_member(json_node_get_object(node), "query_seen")); json_node_unref(node);
    JsonNode *body = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new();
    json_object_set_string_member(object, "name", "Türkçe form"); json_node_take_object(body, object);
    node = radios_data_client_request_sync(fixture.client, "POST", "/echo", body, NULL, &error);
    g_assert_no_error(error); g_assert_cmpstr(json_object_get_string_member(json_node_get_object(node), "name"), ==, "Türkçe form");
    json_node_unref(node); json_node_unref(body);
    fixture_clear(&fixture);
}

static void
test_error_mapping_no_retries(void)
{
    Fixture fixture; fixture_start(&fixture);
    const struct {const gchar *path; RadioSDataClientError code;} cases[] = {
        {"/forbidden", RADIOS_DATA_CLIENT_ERROR_FORBIDDEN}, {"/missing", RADIOS_DATA_CLIENT_ERROR_NOT_FOUND},
        {"/conflict", RADIOS_DATA_CLIENT_ERROR_CONFLICT}, {"/validation", RADIOS_DATA_CLIENT_ERROR_VALIDATION},
        {"/limited", RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED}, {"/unavailable", RADIOS_DATA_CLIENT_ERROR_UNAVAILABLE},
        {"/committed", RADIOS_DATA_CLIENT_ERROR_COMMITTED}
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); i++) {
        gint before = g_atomic_int_get(&fixture.server.requests);
        expect_error(&fixture, "POST", cases[i].path, cases[i].code);
        g_assert_cmpint(g_atomic_int_get(&fixture.server.requests), ==, before + 1);
    }
    GError *error = NULL;
    g_assert_null(radios_data_client_request_sync(fixture.client, "GET", "/limited", NULL, NULL, &error));
    g_assert_nonnull(strstr(error->message, "7 saniye")); g_clear_error(&error);
    fixture_clear(&fixture);
}

static void
test_response_bounds_and_redirect(void)
{
    Fixture fixture; fixture_start(&fixture);
    expect_error(&fixture, "GET", "/broken", RADIOS_DATA_CLIENT_ERROR_INVALID_RESPONSE);
    expect_error(&fixture, "GET", "/scalar", RADIOS_DATA_CLIENT_ERROR_INVALID_RESPONSE);
    expect_error(&fixture, "GET", "/duplicate", RADIOS_DATA_CLIENT_ERROR_INVALID_RESPONSE);
    expect_error(&fixture, "GET", "/oversized", RADIOS_DATA_CLIENT_ERROR_RESPONSE_TOO_LARGE);
    gint before = g_atomic_int_get(&fixture.server.requests);
    expect_error(&fixture, "GET", "/redirect", RADIOS_DATA_CLIENT_ERROR_HTTP);
    g_assert_cmpint(g_atomic_int_get(&fixture.server.requests), ==, before + 1);
    fixture_clear(&fixture);
}

static void
test_identity_guard(void)
{
    Fixture fixture; fixture_start(&fixture);
    GError *error = NULL;
    g_atomic_int_set(&fixture.server.bad_store, 1);
    g_assert_false(radios_data_client_handshake_sync(fixture.client, NULL, &error));
    g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_STORE_MISMATCH); g_clear_error(&error);
    g_atomic_int_set(&fixture.server.bad_store, 0); g_atomic_int_set(&fixture.server.bad_schema, 1);
    g_assert_false(radios_data_client_handshake_sync(fixture.client, NULL, &error));
    g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_SCHEMA_MISMATCH); g_clear_error(&error);
    fixture_clear(&fixture);
}

static void
test_credentials_permissions_types_and_paths(void)
{
    Fixture fixture; fixture_start(&fixture);
    gchar *original = NULL; g_assert_true(g_file_get_contents(fixture.credential_file, &original, NULL, NULL));
    GError *error = NULL;
    g_assert_cmpint(g_chmod(fixture.credential_file, 0644), ==, 0);
    g_assert_null(radios_data_client_new(&fixture.config, &error));
    g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_CREDENTIALS); g_clear_error(&error);
    g_assert_cmpint(g_chmod(fixture.credential_file, 0600), ==, 0);
    g_assert_true(g_file_set_contents(fixture.credential_file, "{\"schema_version\":1,\"schema_version\":1}", -1, NULL));
    g_assert_null(radios_data_client_new(&fixture.config, &error)); g_assert_nonnull(error); g_clear_error(&error);
    g_assert_true(g_file_set_contents(fixture.credential_file, original, -1, NULL)); g_chmod(fixture.credential_file, 0600);
    const struct {const gchar *key; const gchar *value;} invalid[] = {
        {"store_id", USER_ID}, {"station_id", "ABCDEFAB-1111-4111-8111-111111111111"},
        {"api_token", "invalid\r\nheader"}, {"database_file", "../RadioS.sdb"}
    };
    for (guint i = 0; i < G_N_ELEMENTS(invalid); i++) {
        JsonParser *parser = json_parser_new();
        g_assert_true(json_parser_load_from_data(parser, original, -1, NULL));
        JsonNode *root = json_parser_get_root(parser);
        json_object_set_string_member(json_node_get_object(root), invalid[i].key, invalid[i].value);
        gchar *changed = json_to_string(root, FALSE);
        g_assert_true(g_file_set_contents(fixture.credential_file, changed, -1, NULL));
        g_chmod(fixture.credential_file, 0600);
        g_assert_null(radios_data_client_new(&fixture.config, &error));
        g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_CREDENTIALS); g_clear_error(&error);
        g_free(changed); g_object_unref(parser);
    }
    g_assert_true(g_file_set_contents(fixture.credential_file, original, -1, NULL)); g_chmod(fixture.credential_file, 0600);
    gchar *renamed = g_strconcat(fixture.credential_file, ".private", NULL);
    g_assert_cmpint(g_rename(fixture.credential_file, renamed), ==, 0);
    g_assert_cmpint(symlink(renamed, fixture.credential_file), ==, 0);
    g_assert_null(radios_data_client_new(&fixture.config, &error)); g_assert_nonnull(error); g_clear_error(&error);
    g_assert_cmpint(g_unlink(fixture.credential_file), ==, 0); g_assert_cmpint(g_rename(renamed, fixture.credential_file), ==, 0);
    g_free(renamed); g_free(original);
    gchar *saved_url = fixture.config.service_url;
    fixture.config.service_url = g_strdup("http://example.invalid:8000");
    g_assert_null(radios_data_client_new(&fixture.config, &error));
    g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_INVALID_CONFIG); g_clear_error(&error);
    g_free(fixture.config.service_url); fixture.config.service_url = saved_url;
    fixture_clear(&fixture);
}

static void
test_environment_override(void)
{
    Fixture fixture; fixture_start(&fixture);
    g_free(fixture.config.token_environment);
    fixture.config.token_environment = g_strdup("RADIOS_DATA_CLIENT_TEST_BAD_OVERRIDE");
    GError *error = NULL;
    g_assert_null(radios_data_client_new(&fixture.config, &error));
    g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_CREDENTIALS); g_clear_error(&error);
    g_free(fixture.config.token_environment);
    fixture.config.token_environment = g_strdup("RADIOS_DATA_CLIENT_TEST_VALID_OVERRIDE");
    radios_data_client_unref(fixture.client);
    fixture.client = radios_data_client_new(&fixture.config, &error);
    g_assert_no_error(error); g_assert_nonnull(fixture.client);
    g_atomic_pointer_set(&fixture.server.expected_token, (gpointer) OVERRIDE_TOKEN);
    g_assert_true(radios_data_client_handshake_sync(fixture.client, NULL, &error)); g_assert_no_error(error);
    g_assert_null(radios_data_client_request_sync(fixture.client, "POST", "/validation", NULL, NULL, &error));
    g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_VALIDATION);
    g_assert_null(strstr(error->message, USER_TOKEN)); g_assert_null(strstr(error->message, ENDPOINT_TOKEN));
    g_assert_null(strstr(error->message, OVERRIDE_TOKEN)); g_clear_error(&error);
    fixture_clear(&fixture);
}

static void
test_path_guard_and_owned_config(void)
{
    Fixture fixture; fixture_start(&fixture);
    const gchar *invalid[] = {"http://example.invalid/", "//example.invalid/", "/bad#fragment", "/bad\nheader", "/bad path", "/bad\\path"};
    for (guint i = 0; i < G_N_ELEMENTS(invalid); i++)
        expect_error(&fixture, "GET", invalid[i], RADIOS_DATA_CLIENT_ERROR_INVALID_CONFIG);
    g_assert_cmpint(g_atomic_int_get(&fixture.server.requests), ==, 0);
    radios_config_clear(&fixture.config);
    RadioSDataClient *reference = radios_data_client_ref(fixture.client);
    radios_data_client_unref(fixture.client); fixture.client = reference;
    GError *error = NULL;
    g_assert_true(radios_data_client_handshake_sync(fixture.client, NULL, &error)); g_assert_no_error(error);
    fixture_clear(&fixture);
}

static void
test_timeout(void)
{
    Fixture fixture; fixture_start(&fixture);
    radios_data_client_unref(fixture.client); fixture.config.request_timeout_seconds = 1;
    GError *error = NULL; fixture.client = radios_data_client_new(&fixture.config, &error); g_assert_no_error(error);
    gint64 start = g_get_monotonic_time();
    expect_error(&fixture, "GET", "/slow", RADIOS_DATA_CLIENT_ERROR_TIMEOUT);
    g_assert_cmpint(g_get_monotonic_time() - start, <, 2500 * 1000);
    fixture_clear(&fixture);
}

typedef struct {RadioSDataClient *client; gint cancelled;} CancelTask;

static gchar *
cancel_work(gpointer data, const RadioSCancelToken *cancel, GError **error)
{
    CancelTask *task = data;
    JsonNode *node = radios_data_client_request_sync(task->client, "GET", "/slow", NULL, cancel, error);
    if (node != NULL)
        json_node_unref(node);
    if (error != NULL && *error != NULL && (*error)->domain == RADIOS_DATA_CLIENT_ERROR &&
            (*error)->code == RADIOS_DATA_CLIENT_ERROR_CANCELLED)
        g_atomic_int_set(&task->cancelled, 1);
    return NULL;
}

static void
test_runtime_cancellation(void)
{
    Fixture fixture; fixture_start(&fixture);
    RadioSRuntime *runtime = radios_runtime_new(2, 4);
    CancelTask task = {fixture.client, 0};
    GError *error = NULL;
    g_assert_cmpuint(radios_runtime_submit(runtime, cancel_work, &task, NULL, &error), >, 0);
    g_assert_no_error(error);
    gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    while (!g_atomic_int_get(&fixture.server.slow_started) && g_get_monotonic_time() < deadline)
        g_thread_yield();
    g_assert_true(g_atomic_int_get(&fixture.server.slow_started));
    gint64 start = g_get_monotonic_time();
    radios_runtime_shutdown(runtime);
    g_assert_cmpint(g_get_monotonic_time() - start, <, 2500 * 1000);
    g_assert_cmpint(g_atomic_int_get(&task.cancelled), ==, 1);
    radios_runtime_free(runtime); fixture_clear(&fixture);
}

typedef struct {RadioSDataClient *client; GCancellable *cancel; GError *error; JsonNode *result;} IndividualCancel;
static gpointer individual_cancel_work(gpointer value)
{
    IndividualCancel *job = value;
    job->result = radios_data_client_request_cancellable_sync(job->client, "GET", "/slow", NULL, NULL, job->cancel, &job->error);
    return NULL;
}
static void test_individual_cancellation(void)
{
    Fixture fixture; fixture_start(&fixture); GCancellable *cancel = g_cancellable_new();
    g_cancellable_cancel(cancel); GError *error = NULL;
    JsonNode *node = radios_data_client_request_cancellable_sync(fixture.client, "GET", "/echo", NULL, NULL, cancel, &error);
    g_assert_null(node); g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_CANCELLED); g_clear_error(&error);
    g_assert_cmpint(g_atomic_int_get(&fixture.server.requests), ==, 0); g_cancellable_reset(cancel);
    g_atomic_int_set(&fixture.server.slow_ms, 1500);
    IndividualCancel job = {fixture.client, cancel, NULL, NULL}; GThread *thread = g_thread_new("individual-cancel", individual_cancel_work, &job);
    gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    while (!g_atomic_int_get(&fixture.server.slow_started) && g_get_monotonic_time() < deadline) g_thread_yield();
    g_assert_true(g_atomic_int_get(&fixture.server.slow_started));
    g_cancellable_cancel(cancel); g_thread_join(thread);
    g_assert_null(job.result); g_assert_error(job.error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_CANCELLED); g_clear_error(&job.error);
    /* Cancelling one media request must leave the shared client usable. */
    node = radios_data_client_request_sync(fixture.client, "GET", "/echo", NULL, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(node); json_node_free(node);
    g_object_unref(cancel); fixture_clear(&fixture);
}
static JsonNode *durable_body(void)
{
    JsonNode *n = json_node_new(JSON_NODE_OBJECT); JsonObject *o = json_object_new(); json_node_take_object(n, o);
    json_object_set_string_member(o, "station_id", STATION_ID); json_object_set_string_member(o, "idempotency_key", ENDPOINT_ID);
    json_object_set_string_member(o, "kind", "playout_history"); json_object_set_string_member(o, "name", "Kesinti geçmişi");
    json_object_set_object_member(o, "config", json_object_new()); return n;
}
static void test_durable_lost_response(void)
{
    Fixture f; fixture_start(&f); JsonNode *body = durable_body(); GError *error = NULL;
    g_atomic_int_set(&f.server.durable_drop, 1);
    JsonNode *n = radios_data_client_request_sync(f.client, "POST", "/v1/workflows/desktop/put", body, NULL, &error);
    g_assert_null(n); g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_TRANSPORT); g_clear_error(&error);
    g_assert_cmpint(g_atomic_int_get(&f.server.durable_commits), ==, 1);
    radios_data_client_unref(f.client); f.client = radios_data_client_new(&f.config, &error); g_assert_no_error(error);
    n = radios_data_client_recovery_status(f.client, &error); g_assert_no_error(error);
    g_assert_cmpint(json_object_get_int_member(json_node_get_object(n), "pending"), ==, 1); json_node_free(n);
    g_atomic_int_set(&f.server.bad_store, 1);
    g_assert_cmpuint(radios_data_client_recover_sync(f.client, NULL, 8, &error), ==, 0);
    g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_STORE_MISMATCH); g_clear_error(&error);
    g_assert_cmpint(g_atomic_int_get(&f.server.durable_replays), ==, 0); g_atomic_int_set(&f.server.bad_store, 0);
    g_assert_cmpuint(radios_data_client_recover_sync(f.client, NULL, 8, &error), ==, 1); g_assert_no_error(error);
    g_assert_cmpint(g_atomic_int_get(&f.server.durable_commits), ==, 1); g_assert_cmpint(g_atomic_int_get(&f.server.durable_replays), ==, 1);
    n = radios_data_client_recovery_status(f.client, &error); g_assert_no_error(error);
    g_assert_cmpint(json_object_get_int_member(json_node_get_object(n), "pending"), ==, 0); json_node_free(n);
    json_node_free(body); fixture_clear(&f);
}
static void test_exact_transfer_response_bounds(void)
{
    Fixture fixture; fixture_start(&fixture); GError *error = NULL;
    gchar *content = g_strnfill(3u * 1024 * 1024, 'x');
    gchar *payload = g_strdup_printf("{\"content\":\"%s\"}", content); g_free(content);
    g_atomic_pointer_set(&fixture.server.transfer_payload, payload);
    const gchar *const actions[] = {"mairlist-preview", "mairlist-apply", "mairlist-export",
        "radioboss-preview", "radioboss-apply", "radioboss-export"};
    for (guint i = 0; i < G_N_ELEMENTS(actions); ++i) {
        gchar *path = g_strconcat("/v1/workflows/transfers/", actions[i], NULL);
        JsonNode *result = radios_data_client_request_sync(fixture.client, "POST", path, NULL, NULL, &error);
        g_assert_no_error(error); g_assert_nonnull(result);
        g_assert_cmpuint(strlen(json_object_get_string_member(json_node_get_object(result), "content")), ==, 3u * 1024 * 1024);
        json_node_free(result); g_free(path);
    }
    expect_error(&fixture, "POST", "/v1/workflows/transfers/unknown", RADIOS_DATA_CLIENT_ERROR_RESPONSE_TOO_LARGE);
    content = g_strnfill(4u * 1024 * 1024, 'x');
    gchar *oversized = g_strdup_printf("{\"content\":\"%s\"}", content); g_free(content);
    g_atomic_pointer_set(&fixture.server.transfer_payload, oversized); g_free(payload);
    expect_error(&fixture, "POST", "/v1/workflows/transfers/mairlist-export", RADIOS_DATA_CLIENT_ERROR_RESPONSE_TOO_LARGE);
    fixture_clear(&fixture);
}
/* A whole-archive library makes the playlist catalogue larger than the
 * ordinary 2 MiB cap; only that read gets the larger bound. */
static void test_playlist_catalogue_response_bound(void)
{
    Fixture fixture; fixture_start(&fixture); GError *error = NULL;
    gchar *content = g_strnfill(3u * 1024 * 1024, 'x');
    gchar *payload = g_strdup_printf("{\"playlists\":[{\"_id\":\"p\",\"name\":\"%s\"}]}", content); g_free(content);
    g_atomic_pointer_set(&fixture.server.transfer_payload, payload);
    JsonNode *result = radios_data_client_request_sync(fixture.client, "POST", "/v1/workflows/desktop-playlists/list", NULL, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(result);
    JsonArray *rows = json_object_get_array_member(json_node_get_object(result), "playlists");
    g_assert_cmpuint(json_array_get_length(rows), ==, 1);
    g_assert_cmpuint(strlen(json_object_get_string_member(json_array_get_object_element(rows, 0), "name")), ==, 3u * 1024 * 1024);
    json_node_free(result);
    expect_error(&fixture, "POST", "/v1/workflows/desktop-playlists/get", RADIOS_DATA_CLIENT_ERROR_RESPONSE_TOO_LARGE);
    fixture_clear(&fixture);
}
static void test_usb_snapshot_size_and_durable_replay(void)
{
    Fixture fixture; fixture_start(&fixture); GError *error = NULL;
    JsonNode *body = durable_body(); JsonObject *object = json_node_get_object(body);
    gchar *snapshot = g_strnfill(3u * 1024 * 1024, 'A');
    json_object_set_string_member(object, "pdb_zlib_base64", snapshot); g_free(snapshot);
    json_object_set_string_member(object, "usb_root", "/media/Test USB");
    const gchar *apply_path = "/v1/workflows/desktop-rekordbox/apply";
    const gchar *preview_path = "/v1/workflows/desktop-rekordbox/preview";
    gint requests = g_atomic_int_get(&fixture.server.requests);
    g_assert_false(radios_data_client_prepare(fixture.client, "POST", "/v1/workflows/desktop/put", body, &error));
    g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_VALIDATION); g_clear_error(&error);
    JsonNode *response = radios_data_client_request_sync(fixture.client, "POST", "/echo", body, NULL, &error);
    g_assert_null(response); g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_INVALID_CONFIG); g_clear_error(&error);
    g_assert_cmpint(g_atomic_int_get(&fixture.server.requests), ==, requests);
    response = radios_data_client_request_sync(fixture.client, "POST", preview_path, body, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(response); json_node_free(response);
    g_atomic_int_set(&fixture.server.durable_drop, 1);
    response = radios_data_client_request_sync(fixture.client, "POST", apply_path, body, NULL, &error);
    g_assert_null(response); g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_TRANSPORT); g_clear_error(&error);
    radios_data_client_unref(fixture.client); fixture.client = radios_data_client_new(&fixture.config, &error); g_assert_no_error(error);
    response = radios_data_client_recovery_status(fixture.client, &error); g_assert_no_error(error);
    g_assert_cmpint(json_object_get_int_member(json_node_get_object(response), "pending"), ==, 1); json_node_free(response);
    g_assert_cmpuint(radios_data_client_recover_sync(fixture.client, NULL, 8, &error), ==, 1); g_assert_no_error(error);
    g_assert_cmpint(g_atomic_int_get(&fixture.server.durable_commits), ==, 1);
    g_assert_cmpint(g_atomic_int_get(&fixture.server.durable_replays), ==, 1);
    snapshot = g_strnfill(6u * 1024 * 1024, 'A');
    json_object_set_string_member(object, "pdb_zlib_base64", snapshot); g_free(snapshot);
    g_assert_false(radios_data_client_prepare(fixture.client, "POST", apply_path, body, &error));
    g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_VALIDATION); g_clear_error(&error);
    response = radios_data_client_request_sync(fixture.client, "POST", preview_path, body, NULL, &error);
    g_assert_null(response); g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_INVALID_CONFIG); g_clear_error(&error);
    json_node_free(body); fixture_clear(&fixture);
}
static void test_durable_rejected_and_secrets(void)
{
    Fixture f; fixture_start(&f); JsonNode *body = durable_body(); GError *error = NULL;
    g_atomic_int_set(&f.server.durable_reject, 1);
    JsonNode *n = radios_data_client_request_sync(f.client, "POST", "/v1/workflows/desktop/put", body, NULL, &error);
    g_assert_null(n); g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_FORBIDDEN); g_clear_error(&error);
    gint requests = g_atomic_int_get(&f.server.requests); g_atomic_int_set(&f.server.durable_reject, 0);
    g_assert_cmpuint(radios_data_client_recover_sync(f.client, NULL, 8, &error), ==, 0); g_assert_no_error(error);
    g_assert_cmpint(g_atomic_int_get(&f.server.requests), ==, requests);
    n = radios_data_client_recovery_status(f.client, &error); g_assert_no_error(error);
    g_assert_cmpint(json_object_get_int_member(json_node_get_object(n), "blocked"), ==, 1); json_node_free(n);
    json_object_set_string_member(json_node_get_object(body), "name", USER_TOKEN);
    g_assert_false(radios_data_client_prepare(f.client, "POST", "/v1/workflows/desktop/put", body, &error));
    g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_VALIDATION); g_assert_null(strstr(error->message, USER_TOKEN)); g_clear_error(&error);
    json_node_free(body); fixture_clear(&f);
}
static void test_durable_foreign_user_binding(void)
{
    Fixture f; fixture_start(&f); JsonNode *body = durable_body(); GError *error = NULL;
    g_atomic_int_set(&f.server.wrong_identity, 1);
    g_assert_true(radios_data_client_prepare(f.client, "POST", "/v1/workflows/desktop/put", body, &error)); g_assert_no_error(error);
    g_assert_cmpuint(radios_data_client_recover_sync(f.client, NULL, 8, &error), ==, 0);
    g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_STORE_MISMATCH); g_clear_error(&error);
    g_assert_cmpint(g_atomic_int_get(&f.server.durable_commits), ==, 0);
    JsonNode *n = radios_data_client_recovery_status(f.client, &error); g_assert_no_error(error);
    g_assert_cmpint(json_object_get_int_member(json_node_get_object(n), "pending"), ==, 1); json_node_free(n);
    g_atomic_int_set(&f.server.wrong_identity, 0);
    g_assert_cmpuint(radios_data_client_recover_sync(f.client, NULL, 8, &error), ==, 1); g_assert_no_error(error);
    g_assert_cmpint(g_atomic_int_get(&f.server.durable_commits), ==, 1);
    json_node_free(body); fixture_clear(&f);
}
static void test_override_owns_separate_outbox(void)
{
    Fixture f; fixture_start(&f); JsonNode *body = durable_body(); GError *error = NULL;
    /* A manager's uncertain command exists before operator B connects. */
    g_assert_true(radios_data_client_prepare(f.client, "POST", "/v1/workflows/desktop/put", body, &error));
    g_assert_no_error(error);
    g_free(f.config.token_environment);
    f.config.token_environment = g_strdup("RADIOS_DATA_CLIENT_TEST_VALID_OVERRIDE");
    RadioSDataClient *operator = radios_data_client_new(&f.config, &error);
    g_assert_no_error(error); g_assert_nonnull(operator);
    g_atomic_pointer_set(&f.server.expected_token, (gpointer) OVERRIDE_TOKEN);
    g_atomic_int_set(&f.server.wrong_identity, 1);
    g_assert_false(radios_data_client_prepare(operator, "POST", "/v1/workflows/desktop/put", body, &error));
    g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_INVALID_CONFIG); g_clear_error(&error);
    /* Resolve the actual operator before scanning any stored envelopes. */
    g_assert_cmpuint(radios_data_client_recover_sync(operator, NULL, 8, &error), ==, 0); g_assert_no_error(error);
    g_assert_cmpstr(radios_data_client_user_id(operator), ==, STORE_ID);
    g_assert_cmpint(g_atomic_int_get(&f.server.durable_attempts), ==, 0);
    json_object_set_string_member(json_node_get_object(body), "idempotency_key", STATION_ID);
    g_assert_true(radios_data_client_prepare(operator, "POST", "/v1/workflows/desktop/put", body, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(radios_data_client_recover_sync(operator, NULL, 8, &error), ==, 1); g_assert_no_error(error);
    g_assert_cmpint(g_atomic_int_get(&f.server.durable_commits), ==, 1);
    JsonNode *n = radios_data_client_recovery_status(operator, &error); g_assert_no_error(error);
    g_assert_cmpint(json_object_get_int_member(json_node_get_object(n), "pending"), ==, 0); json_node_free(n);
    n = radios_data_client_recovery_status(f.client, &error); g_assert_no_error(error);
    g_assert_cmpint(json_object_get_int_member(json_node_get_object(n), "pending"), ==, 1); json_node_free(n);
    radios_data_client_unref(operator); json_node_free(body); fixture_clear(&f);
}
static void test_conflict_fences_successor_transport(void)
{
    Fixture f; fixture_start(&f); JsonNode *head = durable_body(), *tail = durable_body(); GError *error = NULL;
    JsonObject *h = json_node_get_object(head), *t = json_node_get_object(tail);
    json_object_set_string_member(h, "document_id", USER_ID); json_object_set_int_member(h, "expected_revision", 1);
    json_object_set_string_member(t, "document_id", USER_ID); json_object_set_int_member(t, "expected_revision", 2);
    json_object_set_string_member(t, "idempotency_key", STORE_ID);
    g_assert_true(radios_data_client_prepare(f.client, "POST", "/v1/workflows/desktop/put", head, &error)); g_assert_no_error(error);
    g_assert_true(radios_data_client_prepare(f.client, "POST", "/v1/workflows/desktop/put", tail, &error)); g_assert_no_error(error);
    g_atomic_int_set(&f.server.durable_reject, 409);
    g_assert_cmpuint(radios_data_client_recover_sync(f.client, NULL, 8, &error), ==, 0); g_assert_no_error(error);
    g_assert_cmpint(g_atomic_int_get(&f.server.durable_attempts), ==, 1);
    /* An external writer could now own revision 2. Never submit our tail. */
    g_atomic_int_set(&f.server.durable_reject, 0);
    g_assert_cmpuint(radios_data_client_recover_sync(f.client, NULL, 8, &error), ==, 0); g_assert_no_error(error);
    g_assert_cmpint(g_atomic_int_get(&f.server.durable_attempts), ==, 1);
    JsonNode *n = radios_data_client_recovery_status(f.client, &error); g_assert_no_error(error);
    JsonObject *summary = json_node_get_object(n);
    g_assert_cmpint(json_object_get_int_member(summary, "blocked"), ==, 1);
    g_assert_cmpint(json_object_get_int_member(summary, "pending"), ==, 1);
    g_assert_true(json_object_get_boolean_member(json_array_get_object_element(json_object_get_array_member(summary, "operations"), 1), "dependency_blocked"));
    json_node_free(n); json_node_free(head); json_node_free(tail); fixture_clear(&f);
}

static void limit_path(Fixture *fixture, const gchar *path, gint failures, gint delay)
{
    g_clear_pointer(&fixture->server.limited_payload, g_free);
    g_atomic_int_set(&fixture->server.limited_attempts, 0);
    g_atomic_int_set(&fixture->server.limited_remaining, failures);
    g_atomic_int_set(&fixture->server.limited_delay, delay);
    g_atomic_pointer_set(&fixture->server.limited_path, (gpointer) path);
}

static void test_read_rate_recovery(void)
{
    Fixture f; fixture_start(&f); GError *error = NULL;
    limit_path(&f, "/health", 1, 1);
    g_assert_true(radios_data_client_handshake_sync(f.client, NULL, &error)); g_assert_no_error(error);
    g_assert_cmpint(g_atomic_int_get(&f.server.limited_attempts), ==, 2);
    limit_path(&f, "/v1/workflows/desktop/list", 1, 2);
    JsonNode *body = durable_body(); gint64 start = g_get_monotonic_time();
    JsonNode *node = radios_data_client_request_sync(f.client, "POST", "/v1/workflows/desktop/list", body, NULL, &error);
    g_assert_no_error(error); g_assert_true(JSON_NODE_HOLDS_ARRAY(node));
    g_assert_cmpint(g_atomic_int_get(&f.server.limited_attempts), ==, 2);
    g_assert_cmpint(g_get_monotonic_time() - start, >=, 1900 * 1000);
    json_node_free(node); json_node_free(body); fixture_clear(&f);
}

static void test_read_rate_bounds(void)
{
    Fixture f; fixture_start(&f);
    /* Five total sends, even when the server keeps returning genuine 429. */
    limit_path(&f, "/v1/workflows/desktop/list", -1, 1);
    gint64 start = g_get_monotonic_time();
    expect_error(&f, "POST", "/v1/workflows/desktop/list", RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED);
    g_assert_cmpint(g_atomic_int_get(&f.server.limited_attempts), ==, 5);
    g_assert_cmpint(g_get_monotonic_time() - start, <, 6 * G_TIME_SPAN_SECOND);
    fixture_clear(&f); fixture_start(&f);
    /* The elapsed retry window stops before a fourth three-second wait/send. */
    limit_path(&f, "/v1/workflows/desktop/list", -1, 3); start = g_get_monotonic_time();
    expect_error(&f, "POST", "/v1/workflows/desktop/list", RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED);
    g_assert_cmpint(g_atomic_int_get(&f.server.limited_attempts), ==, 3);
    g_assert_cmpint(g_get_monotonic_time() - start, <, 8 * G_TIME_SPAN_SECOND);
    fixture_clear(&f); fixture_start(&f);
    /* A larger Retry-After is surfaced intact, never clamped into an early send. */
    limit_path(&f, "/v1/workflows/desktop/list", -1, 7); start = g_get_monotonic_time();
    GError *error = NULL;
    g_assert_null(radios_data_client_request_sync(f.client, "POST", "/v1/workflows/desktop/list", NULL, NULL, &error));
    g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED);
    g_assert_nonnull(strstr(error->message, "7 saniye")); g_clear_error(&error);
    g_assert_cmpint(g_atomic_int_get(&f.server.limited_attempts), ==, 1);
    g_assert_cmpint(g_get_monotonic_time() - start, <, G_TIME_SPAN_SECOND);
    fixture_clear(&f);
}

static gchar *rate_cancel_work(gpointer value, const RadioSCancelToken *cancel, GError **error)
{
    CancelTask *task = value;
    JsonNode *node = radios_data_client_request_sync(task->client, "POST", "/v1/workflows/desktop/list", NULL, cancel, error);
    g_clear_pointer(&node, json_node_free);
    if (g_error_matches(error ? *error : NULL, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_CANCELLED))
        g_atomic_int_set(&task->cancelled, 1);
    return NULL;
}
static gpointer rate_individual_work(gpointer value)
{
    IndividualCancel *job = value;
    job->result = radios_data_client_request_cancellable_sync(job->client, "POST", "/v1/workflows/desktop/list", NULL, NULL, job->cancel, &job->error);
    return NULL;
}
static void wait_rate_response(Fixture *fixture)
{
    gint64 deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
    while (!g_atomic_int_get(&fixture->server.limited_attempts) && g_get_monotonic_time() < deadline) g_thread_yield();
    g_assert_cmpint(g_atomic_int_get(&fixture->server.limited_attempts), ==, 1);
    g_usleep(50 * 1000); /* The socket has returned429; worker is in backoff. */
}
static void test_read_rate_cancellation(void)
{
    Fixture f; fixture_start(&f); limit_path(&f, "/v1/workflows/desktop/list", -1, 3);
    RadioSRuntime *runtime = radios_runtime_new(2, 4); CancelTask task = {f.client, 0}; GError *error = NULL;
    g_assert_cmpuint(radios_runtime_submit(runtime, rate_cancel_work, &task, NULL, &error), >, 0); g_assert_no_error(error);
    wait_rate_response(&f); gint64 start = g_get_monotonic_time(); radios_runtime_shutdown(runtime);
    g_assert_cmpint(g_get_monotonic_time() - start, <, 250 * 1000);
    g_assert_cmpint(g_atomic_int_get(&task.cancelled), ==, 1);
    g_assert_cmpint(g_atomic_int_get(&f.server.limited_attempts), ==, 1); radios_runtime_free(runtime);
    fixture_clear(&f); fixture_start(&f);
    limit_path(&f, "/v1/workflows/desktop/list", -1, 3);
    GCancellable *cancel = g_cancellable_new(); IndividualCancel job = {f.client, cancel, NULL, NULL};
    GThread *thread = g_thread_new("rate-individual-cancel", rate_individual_work, &job);
    wait_rate_response(&f); start = g_get_monotonic_time(); g_cancellable_cancel(cancel); g_thread_join(thread);
    g_assert_cmpint(g_get_monotonic_time() - start, <, 250 * 1000);
    g_assert_null(job.result); g_assert_error(job.error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_CANCELLED);
    g_clear_error(&job.error); g_object_unref(cancel);
    g_assert_cmpint(g_atomic_int_get(&f.server.limited_attempts), ==, 1);
    /* Per-request cancellation does not disable future requests on this client. */
    g_atomic_int_set(&f.server.limited_remaining, 0);
    JsonNode *node = radios_data_client_request_sync(f.client, "POST", "/v1/workflows/desktop/list", NULL, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(node); json_node_free(node); fixture_clear(&f);
}

static void test_rate_nonidempotent_never_retry(void)
{
    Fixture f; fixture_start(&f); JsonNode *body = durable_body();
    const gchar *writes[] = { "/v1/workflows/desktop-programme/approve", "/v1/workflows/media/approve-version",
        "/v1/workflows/rights/grant" };
    for (guint i = 0; i < G_N_ELEMENTS(writes); ++i) {
        gchar *key = g_uuid_string_random(); json_object_set_string_member(json_node_get_object(body), "idempotency_key", key); g_free(key);
        limit_path(&f, writes[i], -1, 1); GError *error = NULL;
        g_assert_null(radios_data_client_request_sync(f.client, "POST", writes[i], body, NULL, &error));
        g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED); g_clear_error(&error);
        g_assert_cmpint(g_atomic_int_get(&f.server.limited_attempts), ==, 1);
    }
    /* Neither arbitrary GET nor a whitelisted POST path under another method
     * acquires automatic retry permission. Unknown future routes stay excluded. */
    limit_path(&f, "/unknown-read", -1, 1); expect_error(&f, "GET", "/unknown-read", RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED);
    g_assert_cmpint(g_atomic_int_get(&f.server.limited_attempts), ==, 1);
    limit_path(&f, "/v1/workflows/desktop/list", -1, 1); expect_error(&f, "GET", "/v1/workflows/desktop/list", RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED);
    g_assert_cmpint(g_atomic_int_get(&f.server.limited_attempts), ==, 1);
    JsonNode *status = radios_data_client_recovery_status(f.client, NULL);
    g_assert_cmpint(json_object_get_int_member(json_node_get_object(status), "pending"), ==, 0);
    json_node_free(status); json_node_free(body); fixture_clear(&f);
}

static void test_write_rate_recovery(void)
{
    Fixture f; fixture_start(&f); JsonNode *body = durable_body(); GError *error = NULL;
    limit_path(&f, "/v1/workflows/desktop/put", 1, 2);
    gint64 start = g_get_monotonic_time();
    JsonNode *node = radios_data_client_request_sync(f.client, "POST", "/v1/workflows/desktop/put", body, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(node); json_node_free(node);
    g_assert_cmpint(g_atomic_int_get(&f.server.limited_attempts), ==, 2);
    g_assert_cmpint(g_get_monotonic_time() - start, >=, 1900 * 1000);
    g_assert_cmpint(g_atomic_int_get(&f.server.durable_commits), ==, 1);
    JsonNode *status = radios_data_client_recovery_status(f.client, &error);
    g_assert_no_error(error); g_assert_cmpint(json_object_get_int_member(json_node_get_object(status), "pending"), ==, 0);
    json_node_free(status);
    /* Replays use the same receipt and respect the same server delay. */
    g_assert_true(radios_data_client_prepare(f.client, "POST", "/v1/workflows/desktop/put", body, &error));
    limit_path(&f, "/v1/workflows/desktop/put", 1, 1);
    g_assert_cmpuint(radios_data_client_recover_sync(f.client, NULL, 8, &error), ==, 1); g_assert_no_error(error);
    g_assert_cmpint(g_atomic_int_get(&f.server.limited_attempts), ==, 2);
    /* A long delay remains pending and is exposed for the UI retry timer. */
    limit_path(&f, "/v1/workflows/desktop/put", 1, 7);
    g_assert_null(radios_data_client_request_sync(f.client, "POST", "/v1/workflows/desktop/put", body, NULL, &error));
    g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED); g_clear_error(&error);
    g_assert_cmpuint(radios_data_client_retry_delay(f.client), >=, 6);
    g_assert_cmpint(g_atomic_int_get(&f.server.limited_attempts), ==, 1);
    json_node_free(body); fixture_clear(&f);
}
static void test_service_source_version(void)
{
    Fixture f; fixture_start(&f); GError *error = NULL;
    g_atomic_int_set(&f.server.bad_api, 1);
    g_assert_false(radios_data_client_handshake_sync(f.client, NULL, &error));
    g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_SCHEMA_MISMATCH); g_clear_error(&error);
    g_atomic_int_set(&f.server.bad_api, 0);
    gchar *directory = g_build_filename(f.root, "database", "SDBASE", "src", "sdbase", NULL);
    g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
    gchar *path = g_build_filename(directory, "sample.py", NULL);
    g_assert_true(g_file_set_contents(path, "VALUE", -1, NULL));
    const gchar bytes[] = "sample.py\0VALUE\0";
    gchar *digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)bytes, sizeof bytes - 1);
    g_atomic_pointer_set(&f.server.source_digest, digest);
    g_assert_true(radios_data_client_handshake_sync(f.client, NULL, &error)); g_assert_no_error(error);
    g_assert_true(g_file_set_contents(path, "VALUE_CHANGED", -1, NULL));
    g_assert_false(radios_data_client_handshake_sync(f.client, NULL, &error));
    g_assert_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_SCHEMA_MISMATCH); g_clear_error(&error);
    g_atomic_pointer_set(&f.server.source_digest, NULL); g_free(digest); g_free(path); g_free(directory); fixture_clear(&f);
}
static void test_same_identity(void)
{
    Fixture f; fixture_start(&f); GError *error = NULL;
    RadioSDataClient *same = radios_data_client_new(&f.config, &error);
    g_assert_no_error(error); g_assert_true(radios_data_client_same_identity(f.client, same));
    g_free(f.config.token_environment); f.config.token_environment = g_strdup("RADIOS_DATA_CLIENT_TEST_VALID_OVERRIDE");
    RadioSDataClient *other = radios_data_client_new(&f.config, &error); g_assert_no_error(error);
    g_atomic_pointer_set(&f.server.expected_token, (gpointer)OVERRIDE_TOKEN);
    g_atomic_int_set(&f.server.wrong_identity, 1);
    /* A caller may retain the borrowed provisional ID across adoption. */
    const gchar *before = radios_data_client_user_id(other);
    g_assert_true(radios_data_client_handshake_sync(other, NULL, &error)); g_assert_no_error(error);
    g_assert_cmpstr(before, ==, USER_ID);
    g_assert_cmpstr(radios_data_client_user_id(other), ==, STORE_ID);
    g_assert_false(radios_data_client_same_identity(f.client, other));
    radios_data_client_unref(other); radios_data_client_unref(same); fixture_clear(&f);
}
/* C8 (kılavuz 18.10): optional workflow features of /v1/capabilities. None
 * before a handshake or when not listed; every handshake replaces the list;
 * non-string members are ignored. */
static void test_workflow_features(void)
{
    Fixture f; fixture_start(&f); GError *error = NULL;
    g_assert_false(radios_data_client_has_feature(f.client, "kind_transition_distribution"));
    g_assert_true(radios_data_client_handshake_sync(f.client, NULL, &error)); g_assert_no_error(error);
    g_assert_false(radios_data_client_has_feature(f.client, "kind_transition_distribution"));
    g_atomic_int_set(&f.server.workflow_features, 1);
    g_assert_true(radios_data_client_handshake_sync(f.client, NULL, &error)); g_assert_no_error(error);
    g_assert_true(radios_data_client_has_feature(f.client, "kind_transition_distribution"));
    g_assert_false(radios_data_client_has_feature(f.client, "unknown_feature"));
    g_assert_false(radios_data_client_has_feature(f.client, NULL)); g_assert_false(radios_data_client_has_feature(NULL, "kind_transition_distribution"));
    g_atomic_int_set(&f.server.workflow_features, 0);
    g_assert_true(radios_data_client_handshake_sync(f.client, NULL, &error)); g_assert_no_error(error);
    g_assert_false(radios_data_client_has_feature(f.client, "kind_transition_distribution"));
    fixture_clear(&f);
}

int
main(int argc, char **argv)
{
    /* Set all test environment values before any worker/server is created. */
    g_unsetenv("RADIOS_DATA_CLIENT_TEST_TOKEN_UNSET");
    g_setenv("RADIOS_DATA_CLIENT_TEST_VALID_OVERRIDE", OVERRIDE_TOKEN, TRUE);
    g_setenv("RADIOS_DATA_CLIENT_TEST_BAD_OVERRIDE", "invalid\r\nheader", TRUE);
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/data-client/handshake-json-query-post", test_handshake_and_json);
    g_test_add_func("/data-client/http-errors-no-retry", test_error_mapping_no_retries);
    g_test_add_func("/data-client/response-bounds-redirect", test_response_bounds_and_redirect);
    g_test_add_func("/data-client/exact-transfer-response-bounds", test_exact_transfer_response_bounds);
    g_test_add_func("/data-client/playlist-catalogue-response-bound", test_playlist_catalogue_response_bound);
    g_test_add_func("/data-client/identity-guard", test_identity_guard);
    g_test_add_func("/data-client/private-credentials", test_credentials_permissions_types_and_paths);
    g_test_add_func("/data-client/environment-override", test_environment_override);
    g_test_add_func("/data-client/path-owned-config", test_path_guard_and_owned_config);
    g_test_add_func("/data-client/timeout", test_timeout);
    g_test_add_func("/data-client/runtime-cancellation", test_runtime_cancellation);
    g_test_add_func("/data-client/individual-cancellation", test_individual_cancellation);
    g_test_add_func("/data-client/durable-lost-response-reopen", test_durable_lost_response);
    g_test_add_func("/data-client/USB-snapshot-size-durable-replay", test_usb_snapshot_size_and_durable_replay);
    g_test_add_func("/data-client/durable-rejected-no-secret", test_durable_rejected_and_secrets);
    g_test_add_func("/data-client/durable-foreign-user-fence", test_durable_foreign_user_binding);
    g_test_add_func("/data-client/override-separate-outbox", test_override_owns_separate_outbox);
    g_test_add_func("/data-client/conflict-fences-successor-transport", test_conflict_fences_successor_transport);
    g_test_add_func("/data-client/readonly-rate-recovery", test_read_rate_recovery);
    g_test_add_func("/data-client/readonly-rate-bounds", test_read_rate_bounds);
    g_test_add_func("/data-client/readonly-rate-cancellation", test_read_rate_cancellation);
    g_test_add_func("/data-client/rate-nonidempotent-never-retry", test_rate_nonidempotent_never_retry);
    g_test_add_func("/data-client/idempotent-write-rate-recovery", test_write_rate_recovery);
    g_test_add_func("/data-client/service-source-version", test_service_source_version);
    g_test_add_func("/data-client/reconnect-identity-comparison", test_same_identity);
    g_test_add_func("/data-client/workflow-features", test_workflow_features);
    return g_test_run();
}
