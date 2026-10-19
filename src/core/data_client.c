#include "radios/data_client.h"
#include "radios/platform.h"
#include "radios/recovery.h"

#include <curl/curl.h>
#include <errno.h>
#include <string.h>

#define CREDENTIAL_LIMIT (64 * 1024)
#define RESPONSE_LIMIT (2 * 1024 * 1024)
/* Playout preparation returns every queued track with its media, analysis
 * and marker detail (about 3.7 KB each); a whole-archive playlist exceeds the
 * ordinary cap. Matches desktop_playout.MAX_RESPONSE_BYTES on the server. */
#define PLAYOUT_LIMIT (40u * 1024 * 1024)
static gboolean playout_preparation_path(const gchar *path)
{
    return path && (g_str_equal(path, "/v1/workflows/desktop-playout/prepare") ||
        g_str_equal(path, "/v1/workflows/desktop-programme/prepare"));
}
/* The playlist catalogue lists every playlist with its tracks (about 0.9 KB
 * each). A library imported from a whole archive passes the ordinary cap
 * (1374 songs in 37 playlists: 2.23 MiB), and every page that reads the
 * catalogue then showed its playlists as unusable. It shares the preparation
 * bound; the request body stays under the ordinary cap. */
static gboolean playlist_catalogue_path(const gchar *path)
{
    return path && g_str_equal(path, "/v1/workflows/desktop-playlists/list");
}

static gsize request_limit(const gchar *path, JsonNode *body)
{
    if (path && body && JSON_NODE_HOLDS_OBJECT(body) && json_object_has_member(json_node_get_object(body), "pdb_zlib_base64") &&
        (g_str_equal(path, "/v1/workflows/desktop-rekordbox/preview") || g_str_equal(path, "/v1/workflows/desktop-rekordbox/apply")))
        return 6u * 1024 * 1024;
    /* A programme/automation preparation posts the complete queue configuration. */
    if (playout_preparation_path(path)) return PLAYOUT_LIMIT;
    return RESPONSE_LIMIT;
}
static gsize response_limit(const gchar *path)
{
    static const gchar *const transfers[] = {
        "/v1/workflows/transfers/mairlist-preview", "/v1/workflows/transfers/mairlist-apply",
        "/v1/workflows/transfers/mairlist-export", "/v1/workflows/transfers/radioboss-preview",
        "/v1/workflows/transfers/radioboss-apply", "/v1/workflows/transfers/radioboss-export"
    };
    /* Exact-byte adapter reviews and loss reports have a matching bounded
     * server contract. Every other response retains the ordinary 2 MiB cap. */
    for (guint i = 0; path && i < G_N_ELEMENTS(transfers); ++i)
        if (g_str_equal(path, transfers[i])) return 4u * 1024 * 1024;
    if (playout_preparation_path(path) || playlist_catalogue_path(path)) return PLAYOUT_LIMIT;
    return RESPONSE_LIMIT;
}
#define READ_RATE_ATTEMPTS 5
#define READ_RATE_WINDOW_US (8 * G_TIME_SPAN_SECOND)
#define READ_RATE_DELAY_SECONDS 3

struct RadioSDataClient {
    gatomicrefcount references;
    gchar *service_url;
    gchar *store_id;
    gchar *station_id;
    const gchar *station_timezone;
    gchar *endpoint_id;
    gchar *user_id;
    gchar *installation_user_id;
    gchar *token;
    gchar *original_token;
    gchar *endpoint_token;
    gint connect_timeout;
    gint request_timeout;
    RadioSRecovery *recovery;
    gint identity_verified, handshake_verified;
    gboolean allow_identity_override;
    gchar *project_root;
    gchar *source_dir;
    GMutex identity_lock;
    gint64 retry_not_before;
    gchar **workflow_features; /* last verified /v1/capabilities list; identity_lock */
};

typedef struct {
    GHashTable *members;
    gboolean duplicate;
} ParseCheck;

typedef struct {
    GByteArray *body;
    gsize response_limit;
    const RadioSCancelToken *cancel;
    gboolean oversized;
    guint retry_after;
    GCancellable *cancellable;
} Transfer;

GQuark
radios_data_client_error_quark(void)
{
    return g_quark_from_static_string("radios-data-client-error");
}

static gboolean
fail(GError **error, RadioSDataClientError code, const gchar *message)
{
    g_set_error_literal(error, RADIOS_DATA_CLIENT_ERROR, code, message);
    return FALSE;
}

static void
object_member(JsonParser *parser, JsonObject *object, const gchar *member, gpointer data)
{
    (void) parser;
    ParseCheck *check = data;
    gchar *key = g_strdup_printf("%p:%s", (void *) object, member);
    if (g_hash_table_contains(check->members, key)) {
        check->duplicate = TRUE;
        g_free(key);
    } else {
        g_hash_table_add(check->members, key);
    }
}

static JsonNode *
parse_json(const gchar *body, gsize length)
{
    if (length == 0 || memchr(body, '\0', length) != NULL ||
            !g_utf8_validate(body, (gssize) length, NULL) ||
            g_strstr_len(body, (gssize) length, "\\u0000") != NULL)
        return NULL;
    JsonParser *parser = json_parser_new();
    json_parser_set_strict(parser, TRUE);
    ParseCheck check = {g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL), FALSE};
    g_signal_connect(parser, "object-member", G_CALLBACK(object_member), &check);
    GError *parse_error = NULL;
    gboolean parsed = json_parser_load_from_data(parser, body, (gssize) length, &parse_error);
    JsonNode *node = NULL;
    if (parsed && !check.duplicate) {
        JsonNode *root = json_parser_get_root(parser);
        if (root != NULL && (JSON_NODE_HOLDS_OBJECT(root) || JSON_NODE_HOLDS_ARRAY(root)))
            node = json_node_copy(root);
    }
    g_clear_error(&parse_error); /* Parser details can contain untrusted secrets. */
    g_hash_table_unref(check.members);
    g_object_unref(parser);
    return node;
}

static const gchar *
string_member(JsonObject *object, const gchar *name)
{
    JsonNode *node = json_object_get_member(object, name);
    return node != NULL && JSON_NODE_HOLDS_VALUE(node) &&
            json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
}

static gboolean
integer_member(JsonObject *object, const gchar *name, gint64 expected)
{
    JsonNode *node = json_object_get_member(object, name);
    return node != NULL && JSON_NODE_HOLDS_VALUE(node) &&
            json_node_get_value_type(node) == G_TYPE_INT64 && json_node_get_int(node) == expected;
}

static gboolean
uuid_valid(const gchar *value)
{
    if (value == NULL || strlen(value) != 36 || !g_uuid_string_is_valid(value))
        return FALSE;
    gchar *lower = g_ascii_strdown(value, -1);
    gboolean valid = g_str_equal(lower, value);
    g_free(lower);
    return valid;
}

static gboolean
token_valid(const gchar *value)
{
    if (value == NULL || strlen(value) < 20 || strlen(value) > 256)
        return FALSE;
    for (const gchar *p = value; *p != '\0'; p++) {
        if (!g_ascii_isalnum(*p) && *p != '_' && *p != '-')
            return FALSE;
    }
    return TRUE;
}

static JsonNode *
read_json_file(const gchar *path, gboolean private, GError **error)
{
    gint fd = radios_fs_open(path, RADIOS_O_RDONLY | RADIOS_O_NOFOLLOW | RADIOS_O_NONBLOCK | RADIOS_O_CLOEXEC, 0);
    if (fd < 0) {
        fail(error, RADIOS_DATA_CLIENT_ERROR_CREDENTIALS,
             "Yerel veritabanı kimliği okunamadı; önce veritabanı kurulumu tamamlanmalı.");
        return NULL;
    }
    /* Private = owned by the current Windows user and readable by nobody else
     * (owner-only ACL, the Windows form of 0600). */
    RadioSFileInfo info;
    if (radios_fd_stat(fd, &info) != 0 || info.type != RADIOS_FILE_TYPE_REGULAR ||
            info.size < 0 || info.size > CREDENTIAL_LIMIT ||
            (private && !radios_fd_is_private(fd, RADIOS_PRIVATE_OWNER | RADIOS_PRIVATE_NO_OTHERS |
                                                  RADIOS_PRIVATE_EXACT_FILE_MODE))) {
        close(fd);
        fail(error, RADIOS_DATA_CLIENT_ERROR_CREDENTIALS,
             "Kimlik dosyası bu Windows kullanıcısına ait, yalnızca onun erişebildiği (özel izinli) ve en fazla 64 KiB olmalı.");
        return NULL;
    }
    gsize capacity = CREDENTIAL_LIMIT + 1;
    gchar *buffer = g_malloc(capacity);
    gsize used = 0;
    while (used < capacity) {
        gint amount = read(fd, buffer + used, (guint) (capacity - used));
        if (amount < 0 && errno == EINTR)
            continue;
        if (amount < 0) {
            close(fd);
            g_free(buffer);
            fail(error, RADIOS_DATA_CLIENT_ERROR_CREDENTIALS, "Yerel kimlik dosyası okunamadı.");
            return NULL;
        }
        if (amount == 0)
            break;
        used += (gsize) amount;
    }
    close(fd);
    JsonNode *node = used <= CREDENTIAL_LIMIT ? parse_json(buffer, used) : NULL;
    /* credentials.json holds the access tokens: wipe the copy before freeing. */
    volatile gchar *wipe = buffer;
    for (gsize i = 0; i < used; ++i)
        wipe[i] = '\0';
    g_free(buffer);
    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node)) {
        g_clear_pointer(&node, json_node_unref);
        fail(error, RADIOS_DATA_CLIENT_ERROR_CREDENTIALS,
             "Yerel kimlik veya veritabanı bilgisi geçerli JSON değil.");
    }
    return node;
}

static gboolean
url_valid(const gchar *url, gchar **canonical)
{
    if (url == NULL || strlen(url) > 2048)
        return FALSE;
    GError *error = NULL;
    GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, &error);
    g_clear_error(&error);
    if (uri == NULL)
        return FALSE;
    const gchar *scheme = g_uri_get_scheme(uri);
    const gchar *host = g_uri_get_host(uri);
    const gchar *path = g_uri_get_path(uri);
    gint port = g_uri_get_port(uri);
    gboolean loopback = host != NULL && (g_str_equal(host, "127.0.0.1") ||
            g_ascii_strcasecmp(host, "localhost") == 0 || g_str_equal(host, "::1"));
    gboolean valid = scheme != NULL && host != NULL && *host != '\0' &&
            (g_ascii_strcasecmp(scheme, "https") == 0 ||
             (loopback && g_ascii_strcasecmp(scheme, "http") == 0)) &&
            g_uri_get_userinfo(uri) == NULL && g_uri_get_fragment(uri) == NULL &&
            g_uri_get_query(uri) == NULL && (port == -1 || (port > 0 && port <= 65535)) &&
            (path == NULL || *path == '\0' || g_str_equal(path, "/"));
    if (valid) {
        *canonical = g_strdup(url);
        gsize length = strlen(*canonical);
        if (length > 0 && (*canonical)[length - 1] == '/')
            (*canonical)[length - 1] = '\0';
    }
    g_uri_unref(uri);
    return valid;
}

RadioSDataClient *
radios_data_client_new(const RadioSConfig *config, GError **error)
{
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    if (config == NULL || config->project_root == NULL || config->database_file == NULL ||
            config->connect_timeout_seconds < 1 || config->connect_timeout_seconds > 300 ||
            config->request_timeout_seconds < config->connect_timeout_seconds ||
            config->request_timeout_seconds > 3600) {
        fail(error, RADIOS_DATA_CLIENT_ERROR_INVALID_CONFIG, "Veri istemcisi yapılandırması geçersiz.");
        return NULL;
    }
    gchar *service_url = NULL;
    if (!url_valid(config->service_url, &service_url)) {
        fail(error, RADIOS_DATA_CLIENT_ERROR_INVALID_CONFIG,
             "Veri servisi yerel HTTP veya doğrulanan HTTPS adresi olmalı.");
        return NULL;
    }
    /* Physical paths (links, junctions, 8.3 names and subst drives resolved);
     * comparisons follow the Windows rules (case-insensitive, both separators). */
    gchar *root = radios_fs_realpath(config->project_root, NULL);
    gchar *config_requested = root != NULL ? g_build_filename(root, "config", NULL) : NULL;
    gchar *config_root = config_requested != NULL ? radios_fs_realpath(config_requested, NULL) : NULL;
    gchar *database = radios_fs_realpath(config->database_file, NULL);
    RadioSFileInfo db_info;
    gboolean safe = root != NULL && config_root != NULL && database != NULL &&
            radios_path_is_inside(root, config_root) &&
            (config->database_external || (radios_path_is_inside(root, database) && !radios_path_equal(root, database))) &&
            radios_fs_stat(config->database_file, FALSE, &db_info) == 0 && db_info.type == RADIOS_FILE_TYPE_REGULAR;
    g_free(config_requested);
    if (!safe) {
        g_free(root); g_free(config_root); g_free(database); g_free(service_url);
        fail(error, RADIOS_DATA_CLIENT_ERROR_CREDENTIALS,
             "Kimlik ve veritabanı yolları RadioS projesindeki normal dosyalara ait olmalı.");
        return NULL;
    }
    gchar *credential_file = g_build_filename(config_root, "credentials.json", NULL);
    gchar *metadata_file = g_strconcat(database, ".meta", NULL);
    JsonNode *identity = read_json_file(credential_file, TRUE, error);
    JsonNode *metadata = identity != NULL ? read_json_file(metadata_file, FALSE, error) : NULL;
    g_free(credential_file); g_free(metadata_file); g_free(config_root);
    if (identity == NULL || metadata == NULL) {
        g_clear_pointer(&identity, json_node_unref); g_clear_pointer(&metadata, json_node_unref);
        g_free(root); g_free(database); g_free(service_url);
        return NULL;
    }
    JsonObject *object = json_node_get_object(identity);
    JsonObject *meta = json_node_get_object(metadata);
    const gchar *store = string_member(object, "store_id");
    const gchar *user = string_member(object, "user_id");
    const gchar *station = string_member(object, "station_id");
    const gchar *endpoint = string_member(object, "endpoint_id");
    const gchar *token = string_member(object, "api_token");
    const gchar *endpoint_token = string_member(object, "endpoint_token");
    const gchar *stored_path = string_member(object, "database_file");
    /* Project-relative path of the database ("data\database\RadioS.sdb"); both
     * come from realpath, so the root is a byte prefix of the database path.
     * A drive or share root already ends with its separator. */
    const gchar *relative = NULL;
    if (!config->database_external) {
        relative = database + strlen(root);
        while (G_IS_DIR_SEPARATOR(*relative))
            ++relative;
    }
    /* A database moved by storage.ini lives outside the project: the identity
     * file keeps its logical project path, so only the file name and the
     * store UUID (compared with the .meta file below) bind them together.
     * The stored path is portable ("data/database/RadioS.sdb"); files written
     * by older builds may use '\'. Windows names compare case-insensitively. */
    gchar *stored_name = config->database_external && stored_path != NULL ? g_path_get_basename(stored_path) : NULL;
    gchar *actual_name = config->database_external ? g_path_get_basename(database) : NULL;
    gboolean same_database = FALSE;
    if (config->database_external) {
        same_database = stored_name != NULL && radios_path_equal(stored_name, actual_name);
    } else if (stored_path != NULL && *stored_path != '\0' && !radios_path_is_absolute(stored_path)) {
        gchar *native = radios_path_from_portable(stored_path);
        same_database = radios_path_equal(native, relative);
        g_free(native);
    }
    g_free(stored_name); g_free(actual_name);
    gboolean valid = integer_member(object, "schema_version", 1) &&
            integer_member(meta, "schema_version", 4) && uuid_valid(store) && uuid_valid(user) &&
            uuid_valid(station) && uuid_valid(endpoint) && token_valid(token) &&
            token_valid(endpoint_token) && !g_str_equal(token, endpoint_token) &&
            g_strcmp0(store, string_member(meta, "store_id")) == 0 &&
            same_database;
    const gchar *override = NULL;
    if (config->token_environment != NULL && *config->token_environment != '\0') {
        for (const gchar *p = config->token_environment; *p != '\0'; p++) {
            if (!(g_ascii_isalnum(*p) || *p == '_') || (p == config->token_environment && g_ascii_isdigit(*p)))
                valid = FALSE;
        }
        override = g_getenv(config->token_environment);
        if (override != NULL && !token_valid(override))
            valid = FALSE;
    }
    RadioSDataClient *client = NULL;
    if (valid) {
        client = g_new0(RadioSDataClient, 1);
        g_atomic_ref_count_init(&client->references);
        client->service_url = service_url;
        service_url = NULL;
        client->store_id = g_strdup(store); client->user_id = g_strdup(user);
        client->installation_user_id = client->user_id;
        client->station_id = g_strdup(station); client->endpoint_id = g_strdup(endpoint);
        client->station_timezone = g_intern_static_string("UTC");
        client->token = g_strdup(override != NULL ? override : token);
        client->original_token = g_strdup(token); client->endpoint_token = g_strdup(endpoint_token);
        client->connect_timeout = config->connect_timeout_seconds;
        client->request_timeout = config->request_timeout_seconds;
        client->project_root = g_strdup(root);
        client->source_dir = config->database_source_dir ? g_strdup(config->database_source_dir) :
            g_build_filename(root, "database", "SDBASE", NULL);
        client->allow_identity_override = override != NULL && g_strcmp0(override, token) != 0;
        g_mutex_init(&client->identity_lock);
        client->recovery = radios_recovery_new(config, store, station, user, endpoint);
    } else {
        fail(error, RADIOS_DATA_CLIENT_ERROR_CREDENTIALS,
             "Yerel kimliğin UUID, erişim anahtarı, veritabanı yolu veya şema bilgisi eşleşmiyor.");
    }
    json_node_unref(identity); json_node_unref(metadata);
    g_free(root); g_free(database); g_free(service_url);
    return client;
}

RadioSDataClient *
radios_data_client_ref(RadioSDataClient *client)
{
    g_return_val_if_fail(client != NULL, NULL);
    g_atomic_ref_count_inc(&client->references);
    return client;
}

static void
secret_free(gchar *secret)
{
    if (secret != NULL) {
        volatile gchar *p = secret;
        gsize length = strlen(secret);
        while (length-- > 0)
            *p++ = '\0';
        g_free(secret);
    }
}

void
radios_data_client_unref(RadioSDataClient *client)
{
    if (client != NULL && g_atomic_ref_count_dec(&client->references)) {
        g_free(client->service_url); g_free(client->store_id);
        if (client->user_id != client->installation_user_id) g_free(client->user_id);
        g_free(client->installation_user_id);
        g_free(client->station_id); g_free(client->endpoint_id);
        secret_free(client->token); secret_free(client->original_token); secret_free(client->endpoint_token);
        radios_recovery_free(client->recovery);
        g_free(client->project_root); g_free(client->source_dir); g_mutex_clear(&client->identity_lock);
        g_strfreev(client->workflow_features);
        g_free(client);
    }
}

const gchar *radios_data_client_station_id(const RadioSDataClient *client) { return client->station_id; }
/* The atomic load needs a non-const location (GLib's typeof-based macro would
 * otherwise load into a const temporary); the client itself is not modified. */
const gchar *radios_data_client_station_timezone(const RadioSDataClient *client) { return client ? g_atomic_pointer_get((const gchar **) &client->station_timezone) : "UTC"; }
const gchar *radios_data_client_endpoint_id(const RadioSDataClient *client) { return client->endpoint_id; }
const gchar *radios_data_client_store_id(const RadioSDataClient *client) { return client->store_id; }
const gchar *radios_data_client_user_id(const RadioSDataClient *client) { return g_atomic_pointer_get((gchar **) &client->user_id); }
const gchar *radios_data_client_service_url(const RadioSDataClient *client) { return client->service_url; }

gboolean radios_data_client_same_identity(const RadioSDataClient *a, const RadioSDataClient *b)
{
    /* User strings remain alive until client destruction. Atomic publication
     * keeps borrowed getters and comparisons safe during one-time adoption. */
    return a && b && !g_strcmp0(a->store_id, b->store_id) &&
        !g_strcmp0(radios_data_client_user_id(a), radios_data_client_user_id(b)) &&
        !g_strcmp0(a->station_id, b->station_id) && !g_strcmp0(a->endpoint_id, b->endpoint_id) &&
        !g_strcmp0(a->service_url, b->service_url);
}
guint radios_data_client_retry_delay(RadioSDataClient *client)
{
    if (!client) return 0;
    g_mutex_lock(&client->identity_lock);
    gint64 remaining = client->retry_not_before - g_get_monotonic_time();
    g_mutex_unlock(&client->identity_lock);
    return remaining > 0 ? (guint)((remaining + G_TIME_SPAN_SECOND - 1) / G_TIME_SPAN_SECOND) : 0;
}


static size_t
write_body(char *data, size_t size, size_t count, void *context)
{
    Transfer *transfer = context;
    if (size != 0 && count > G_MAXSIZE / size)
        return 0;
    gsize amount = size * count;
    if (transfer->body->len > transfer->response_limit || amount > transfer->response_limit - transfer->body->len) {
        transfer->oversized = TRUE;
        return 0;
    }
    if (radios_cancelled(transfer->cancel) || (transfer->cancellable && g_cancellable_is_cancelled(transfer->cancellable)))
        return 0;
    g_byte_array_append(transfer->body, (const guint8 *) data, (guint) amount);
    return amount;
}

static size_t
read_header(char *data, size_t size, size_t count, void *context)
{
    if (size != 0 && count > G_MAXSIZE / size)
        return 0;
    gsize amount = size * count;
    Transfer *transfer = context;
    if (amount >= 12 && amount <= 1024 && g_ascii_strncasecmp(data, "Retry-After:", 12) == 0) {
        gchar *line = g_strndup(data + 12, amount - 12);
        gchar *trimmed = g_strstrip(line);
        gchar *end = NULL;
        guint64 seconds = g_ascii_strtoull(trimmed, &end, 10);
        if (*trimmed != '\0' && end != trimmed && *end == '\0' && seconds <= 86400)
            transfer->retry_after = (guint) seconds;
        g_free(line);
    }
    return amount;
}

static int
progress(void *context, curl_off_t total, curl_off_t current, curl_off_t sent_total, curl_off_t sent)
{
    (void) total; (void) current; (void) sent_total; (void) sent;
    Transfer *transfer = context;
    return radios_cancelled(transfer->cancel) || (transfer->cancellable && g_cancellable_is_cancelled(transfer->cancellable)) ? 1 : 0;
}

static gchar *
replace_secret(gchar *text, const gchar *secret)
{
    gchar **parts = g_strsplit(text, secret, -1);
    gchar *result = g_strjoinv("[gizli]", parts);
    g_strfreev(parts); g_free(text);
    return result;
}

static gchar *
safe_detail(RadioSDataClient *client, JsonNode *node)
{
    if (node == NULL || !JSON_NODE_HOLDS_OBJECT(node))
        return NULL;
    const gchar *detail = string_member(json_node_get_object(node), "detail");
    if (detail == NULL || *detail == '\0')
        return NULL;
    gchar *safe = g_strdup(detail);
    safe = replace_secret(safe, client->token);
    safe = replace_secret(safe, client->original_token);
    safe = replace_secret(safe, client->endpoint_token);
    GRegex *tokens = g_regex_new("[A-Za-z0-9_-]{32,}", 0, 0, NULL);
    gchar *replaced = g_regex_replace_literal(tokens, safe, -1, 0, "[gizli]", 0, NULL);
    g_regex_unref(tokens); g_free(safe); safe = replaced;
    for (gchar *p = safe; *p != '\0'; p++) {
        if ((guchar) *p < 32 || *p == 127)
            *p = ' ';
    }
    if (g_utf8_strlen(safe, -1) > 360)
        *g_utf8_offset_to_pointer(safe, 360) = '\0';
    return safe;
}

static void
http_error(RadioSDataClient *client, long status, JsonNode *node, guint retry, GError **error)
{
    RadioSDataClientError code = RADIOS_DATA_CLIENT_ERROR_HTTP;
    const gchar *message = "Veri servisi isteği tamamlayamadı.";
    switch (status) {
    case 401: case 403: code = RADIOS_DATA_CLIENT_ERROR_FORBIDDEN; message = "Bu işlem için erişim yetkisi yok veya anahtarın süresi dolmuş."; break;
    case 404: code = RADIOS_DATA_CLIENT_ERROR_NOT_FOUND; message = "İstenen kayıt veya veri işlemi bulunamadı."; break;
    case 409: code = RADIOS_DATA_CLIENT_ERROR_CONFLICT; message = "Kayıt başka bir işlem tarafından değiştirildi; güncel veriyi yeniden yükle."; break;
    case 400: case 422: code = RADIOS_DATA_CLIENT_ERROR_VALIDATION; message = "İşlem alanları geçersiz; formdaki değerleri kontrol et."; break;
    case 429: code = RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED; message = "Veri servisi istek sınırına ulaştı; bir süre sonra yeniden dene."; break;
    case 502: case 503: case 504: code = RADIOS_DATA_CLIENT_ERROR_UNAVAILABLE; message = "Veri servisi şu anda kullanılamıyor."; break;
    default: break;
    }
    if (status == 503 && node != NULL && JSON_NODE_HOLDS_OBJECT(node)) {
        JsonNode *committed = json_object_get_member(json_node_get_object(node), "committed");
        if (committed != NULL && JSON_NODE_HOLDS_VALUE(committed) &&
                json_node_get_value_type(committed) == G_TYPE_BOOLEAN && json_node_get_boolean(committed)) {
            code = RADIOS_DATA_CLIENT_ERROR_COMMITTED;
            message = "Kayıt veritabanına işlendi, sonraki doğrulama tamamlanamadı; yenileyerek sonucu kontrol et.";
        }
    }
    gchar *detail = safe_detail(client, node);
    gchar *delay = retry > 0 ? g_strdup_printf(" Yeniden deneme süresi: %u saniye.", retry) : g_strdup("");
    g_set_error(error, RADIOS_DATA_CLIENT_ERROR, code, "%s%s%s%s", message, delay,
                detail != NULL ? " " : "", detail != NULL ? detail : "");
    g_free(delay); g_free(detail);
}

static JsonNode *
request_transport_once(RadioSDataClient *client, const gchar *method,
                               const gchar *path, JsonNode *body,
                               const RadioSCancelToken *cancel, GCancellable *cancellable,
                               gint64 deadline, guint *retry_after, GError **error)
{
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    if (client == NULL || method == NULL || path == NULL || path[0] != '/' ||
            path[1] == '/' || strlen(path) > 4096 || strchr(path, '#') != NULL ||
            strchr(path, '\r') != NULL || strchr(path, '\n') != NULL ||
            !(g_str_equal(method, "GET") || g_str_equal(method, "POST") ||
              g_str_equal(method, "PUT") || g_str_equal(method, "PATCH") || g_str_equal(method, "DELETE")) ||
            (g_str_equal(method, "GET") && body != NULL) ||
            (body != NULL && !JSON_NODE_HOLDS_OBJECT(body) && !JSON_NODE_HOLDS_ARRAY(body))) {
        fail(error, RADIOS_DATA_CLIENT_ERROR_INVALID_CONFIG, "Veri isteğinin yöntemi, yolu veya JSON gövdesi geçersiz.");
        return NULL;
    }
    for (const gchar *p = path; *p != '\0'; p++) {
        if ((guchar) *p <= 32 || (guchar) *p == 127 || *p == '\\') {
            fail(error, RADIOS_DATA_CLIENT_ERROR_INVALID_CONFIG, "Veri isteğinin yolu kontrol veya boşluk karakteri içeriyor.");
            return NULL;
        }
    }
    if (radios_cancelled(cancel) || (cancellable && g_cancellable_is_cancelled(cancellable))) {
        fail(error, RADIOS_DATA_CLIENT_ERROR_CANCELLED, "Veri işlemi iptal edildi.");
        return NULL;
    }
    if (deadline && g_get_monotonic_time() >= deadline) {
        fail(error, RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED, "Veri servisi istek sınırına ulaştı; otomatik okuma bekleme süresi doldu.");
        return NULL;
    }
    static gsize curl_initialized = 0;
    if (g_once_init_enter(&curl_initialized)) {
        CURLcode initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
        g_once_init_leave(&curl_initialized, initialized == CURLE_OK ? 1 : 2);
    }
    if (curl_initialized != 1) {
        fail(error, RADIOS_DATA_CLIENT_ERROR_TRANSPORT, "HTTP veri istemcisi başlatılamadı.");
        return NULL;
    }
    CURL *curl = curl_easy_init();
    if (curl == NULL) {
        fail(error, RADIOS_DATA_CLIENT_ERROR_TRANSPORT, "HTTP veri istemcisi başlatılamadı.");
        return NULL;
    }
    gchar *url = g_strconcat(client->service_url, path, NULL);
    gchar *authorization = g_strconcat("Authorization: Bearer ", client->token, NULL);
    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, authorization);
    headers = curl_slist_append(headers, "Accept: application/json");
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Expect:");
    gchar *payload = body != NULL ? json_to_string(body, FALSE) : NULL;
    if (payload != NULL && strlen(payload) > request_limit(path, body)) {
        fail(error, RADIOS_DATA_CLIENT_ERROR_INVALID_CONFIG, "Veri isteğinin JSON gövdesi boyut sınırını aşıyor.");
        curl_slist_free_all(headers); secret_free(authorization); g_free(url); g_free(payload); curl_easy_cleanup(curl);
        return NULL;
    }
    Transfer transfer = {.body = g_byte_array_new(), .response_limit = response_limit(path), .cancel = cancel, .cancellable = cancellable};
    gint64 timeout_ms = (gint64) client->request_timeout * 1000;
    if (deadline) timeout_ms = MIN(timeout_ms, MAX((deadline - g_get_monotonic_time()) / 1000, 1));
    CURLcode option = CURLE_OK;
#define SET_OPTION(name, value) do { if (option == CURLE_OK) option = curl_easy_setopt(curl, name, value); } while (0)
    SET_OPTION(CURLOPT_URL, url);
    SET_OPTION(CURLOPT_CUSTOMREQUEST, method);
    SET_OPTION(CURLOPT_HTTPHEADER, headers);
    SET_OPTION(CURLOPT_PROXY, "");
    SET_OPTION(CURLOPT_NOPROXY, "*");
    SET_OPTION(CURLOPT_FOLLOWLOCATION, 0L);
    SET_OPTION(CURLOPT_MAXREDIRS, 0L);
    SET_OPTION(CURLOPT_PROTOCOLS_STR, "http,https");
    SET_OPTION(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    SET_OPTION(CURLOPT_SSL_VERIFYPEER, 1L);
    SET_OPTION(CURLOPT_SSL_VERIFYHOST, 2L);
    /* HTTPS verifies against the Windows certificate store: the bundled
     * libcurl has no CA file of its own. Loopback HTTP is unaffected. */
    if (g_ascii_strncasecmp(client->service_url, "https:", 6) == 0)
        SET_OPTION(CURLOPT_SSL_OPTIONS, (long) CURLSSLOPT_NATIVE_CA);
    SET_OPTION(CURLOPT_CONNECTTIMEOUT, (long) client->connect_timeout);
    SET_OPTION(CURLOPT_TIMEOUT_MS, (long) timeout_ms);
    SET_OPTION(CURLOPT_NOSIGNAL, 1L);
    SET_OPTION(CURLOPT_WRITEFUNCTION, write_body);
    SET_OPTION(CURLOPT_WRITEDATA, &transfer);
    SET_OPTION(CURLOPT_HEADERFUNCTION, read_header);
    SET_OPTION(CURLOPT_HEADERDATA, &transfer);
    SET_OPTION(CURLOPT_NOPROGRESS, 0L);
    SET_OPTION(CURLOPT_XFERINFOFUNCTION, progress);
    SET_OPTION(CURLOPT_XFERINFODATA, &transfer);
    if (payload != NULL) {
        SET_OPTION(CURLOPT_POSTFIELDS, payload);
        SET_OPTION(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t) strlen(payload));
    }
#undef SET_OPTION
    CURLcode result = option == CURLE_OK ? curl_easy_perform(curl) : option;
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    JsonNode *node = NULL;
    if (result != CURLE_OK) {
        if (radios_cancelled(cancel) || (cancellable && g_cancellable_is_cancelled(cancellable)))
            fail(error, RADIOS_DATA_CLIENT_ERROR_CANCELLED, "Veri işlemi iptal edildi.");
        else if (transfer.oversized)
            g_set_error(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_RESPONSE_TOO_LARGE,
                "Veri servisi yanıtı %" G_GSIZE_FORMAT " MiB sınırını aşıyor.", transfer.response_limit / (1024 * 1024));
        else if (result == CURLE_OPERATION_TIMEDOUT)
            fail(error, RADIOS_DATA_CLIENT_ERROR_TIMEOUT, "Veri servisi zamanında yanıt vermedi.");
        else
            fail(error, RADIOS_DATA_CLIENT_ERROR_TRANSPORT, "Veri servisine güvenli bağlantı kurulamadı veya bağlantı kesildi.");
    } else {
        node = parse_json((const gchar *) transfer.body->data, transfer.body->len);
        if (status < 200 || status >= 300) {
            http_error(client, status, node, transfer.retry_after, error);
            g_clear_pointer(&node, json_node_unref);
        } else if (node == NULL) {
            fail(error, RADIOS_DATA_CLIENT_ERROR_INVALID_RESPONSE, "Veri servisi geçerli bir JSON nesnesi veya listesi döndürmedi.");
        }
    }
    g_byte_array_unref(transfer.body); curl_slist_free_all(headers);
    secret_free(authorization); g_free(url); g_free(payload); curl_easy_cleanup(curl);
    if (retry_after) *retry_after = transfer.retry_after;
    return node;
}

static gboolean
rate_retry_read(const gchar *method, const gchar *path)
{
    /* POST does not imply a write in this API. Only these individually audited
     * reads may repeat after 429; never infer safety from a path prefix, an
     * idempotency key, GET alone, or recovery eligibility. */
    if (!method || !path) return FALSE;
    if (g_str_equal(method, "GET"))
        return g_str_equal(path, "/health") || g_str_equal(path, "/v1/capabilities");
    if (!g_str_equal(method, "POST")) return FALSE;
    const gchar *reads[] = {
        "/v1/workflows/desktop/context", "/v1/workflows/desktop/list",
        "/v1/workflows/desktop/get", "/v1/workflows/desktop/campaign-list",
        "/v1/workflows/desktop/campaign-get",
        "/v1/workflows/desktop-media/list", "/v1/workflows/desktop-media/get",
        "/v1/workflows/desktop-playlists/list",
        "/v1/workflows/desktop-rekordbox/preview", "/v1/workflows/desktop-playout/prepare",
        "/v1/workflows/desktop-broadcast/overview", "/v1/workflows/desktop-broadcast/emergency-prepare",
        "/v1/workflows/desktop-advert/overview", "/v1/workflows/desktop-advert/campaign-get",
        "/v1/workflows/desktop-advert/prepare", "/v1/workflows/desktop-admin/overview",
        "/v1/workflows/desktop-programme/overview", "/v1/workflows/desktop-programme/prerequisites",
        "/v1/workflows/desktop-programme/prepare"
    };
    for (guint i = 0; i < G_N_ELEMENTS(reads); ++i)
        if (g_str_equal(path, reads[i])) return TRUE;
    return FALSE;
}

static JsonNode *
request_transport(RadioSDataClient *client, const gchar *method, const gchar *path,
                  JsonNode *body, const RadioSCancelToken *cancel,
                  GCancellable *cancellable, GError **error)
{
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    gboolean retry_read = rate_retry_read(method, path) || radios_recovery_eligible(method, path, body);
    /* A second UI/recovery submission must not bypass a previous long server
     * delay merely because the bounded retry window returned to its caller. */
    guint deferred = retry_read ? radios_data_client_retry_delay(client) : 0;
    if (deferred > READ_RATE_DELAY_SECONDS) { http_error(client, 429, NULL, deferred, error); return NULL; }
    if (deferred) {
        gint64 until = g_get_monotonic_time() + (gint64)deferred * G_TIME_SPAN_SECOND;
        while (g_get_monotonic_time() < until) {
            if (radios_cancelled(cancel) || (cancellable && g_cancellable_is_cancelled(cancellable))) {
                fail(error, RADIOS_DATA_CLIENT_ERROR_CANCELLED, "Veri işlemi iptal edildi."); return NULL;
            }
            g_usleep(20 * 1000);
        }
    }
    gint64 deadline = 0;
    for (guint attempt = 0; ; ++attempt) {
        GError *failure = NULL; guint retry_after = 0;
        JsonNode *node = request_transport_once(client, method, path, body, cancel,
                                               cancellable, deadline, &retry_after, &failure);
        if (node) return node;
        if (g_error_matches(failure, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED)) {
            g_mutex_lock(&client->identity_lock);
            client->retry_not_before = MAX(client->retry_not_before,
                g_get_monotonic_time() + (gint64)MAX(retry_after, 1) * G_TIME_SPAN_SECOND);
            g_mutex_unlock(&client->identity_lock);
        }
        if (!retry_read || !g_error_matches(failure, RADIOS_DATA_CLIENT_ERROR,
                RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED) || attempt + 1 >= READ_RATE_ATTEMPTS) {
            g_propagate_error(error, failure); return NULL;
        }
        gint64 now = g_get_monotonic_time();
        if (!deadline) deadline = now + READ_RATE_WINDOW_US;
        /* A longer server delay is surfaced to the user, never shortened into
         * a resend before Retry-After permits it. */
        if (retry_after > READ_RATE_DELAY_SECONDS) { g_propagate_error(error, failure); return NULL; }
        guint delay = MAX(retry_after, 1);
        gint64 until = now + (gint64) delay * G_TIME_SPAN_SECOND;
        if (until >= deadline) { g_propagate_error(error, failure); return NULL; }
        g_clear_error(&failure);
        /* Worker-only, with the same captured client, identity and JSON body.
         * Short waits let runtime/individual cancellation stop before resend. */
        do {
            if (radios_cancelled(cancel) || (cancellable && g_cancellable_is_cancelled(cancellable))) {
                fail(error, RADIOS_DATA_CLIENT_ERROR_CANCELLED, "Veri işlemi iptal edildi."); return NULL;
            }
            g_usleep((gulong) MIN(until - now, 20 * 1000));
            now = g_get_monotonic_time();
        } while (now < until);
        if (now >= deadline) {
            fail(error, RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED, "Veri servisi istek sınırına ulaştı; otomatik okuma bekleme süresi doldu."); return NULL;
        }
    }
}

static gboolean verify_context(RadioSDataClient *client, JsonNode *node, GError **error)
{
    JsonObject *o = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    JsonNode *user = o ? json_object_get_member(o, "user") : NULL, *station = o ? json_object_get_member(o, "station") : NULL;
    const gchar *actual = user && JSON_NODE_HOLDS_OBJECT(user) ? string_member(json_node_get_object(user), "_id") : NULL;
    g_mutex_lock(&client->identity_lock);
    gboolean adopting = client->allow_identity_override && !g_atomic_int_get(&client->identity_verified);
    gboolean ok = user && station && JSON_NODE_HOLDS_OBJECT(user) && JSON_NODE_HOLDS_OBJECT(station) && uuid_valid(actual) &&
        (adopting || g_strcmp0(actual, client->user_id) == 0) &&
        g_strcmp0(string_member(json_node_get_object(station), "_id"), client->station_id) == 0;
    if (!ok) { g_mutex_unlock(&client->identity_lock); return fail(error, RADIOS_DATA_CLIENT_ERROR_STORE_MISMATCH,
        "Erişim anahtarı beklenen kurulum kullanıcısı ve istasyonuyla eşleşmiyor; dayanıklı işlem gönderilmedi.");
    }
    if (adopting && g_strcmp0(actual, client->user_id)) {
        g_atomic_pointer_set(&client->user_id, g_strdup(actual));
        radios_recovery_free(client->recovery);
        RadioSConfig config = {.project_root = client->project_root};
        client->recovery = radios_recovery_new(&config, client->store_id, client->station_id, client->user_id, client->endpoint_id);
    }
    const gchar *zone = string_member(json_node_get_object(station), "timezone");
    if (zone && *zone && strlen(zone) <= 128) g_atomic_pointer_set(&client->station_timezone, g_intern_string(zone));
    g_atomic_int_set(&client->identity_verified, 1); g_mutex_unlock(&client->identity_lock); return TRUE;
}
static gboolean context_fetch_verify(RadioSDataClient *client, const RadioSCancelToken *cancel, GCancellable *cancellable, GError **error)
{
    JsonNode *body = json_node_new(JSON_NODE_OBJECT); json_node_take_object(body, json_object_new());
    json_object_set_string_member(json_node_get_object(body), "station_id", client->station_id);
    JsonNode *context = request_transport(client, "POST", "/v1/workflows/desktop/context", body, cancel, cancellable, error); json_node_free(body);
    if (!context) return FALSE;
    gboolean ok = verify_context(client, context, error); json_node_free(context); return ok;
}
static gboolean ensure_identity(RadioSDataClient *client, const RadioSCancelToken *cancel, GCancellable *cancellable, GError **error)
{
    if (!g_atomic_int_get(&client->handshake_verified) && !radios_data_client_handshake_sync(client, cancel, error)) return FALSE;
    return g_atomic_int_get(&client->identity_verified) || context_fetch_verify(client, cancel, cancellable, error);
}
gboolean radios_data_client_prepare(RadioSDataClient *client, const gchar *method,
    const gchar *path, JsonNode *body, GError **error)
{
    if (!client) return fail(error, RADIOS_DATA_CLIENT_ERROR_INVALID_CONFIG, "Veri istemcisi hazır değil.");
    if (radios_recovery_eligible(method, path, body) && client->allow_identity_override && !g_atomic_int_get(&client->identity_verified))
        return fail(error, RADIOS_DATA_CLIENT_ERROR_INVALID_CONFIG, "Ortam anahtarının kullanıcısı önce bağlantı/kimlik kontrolüyle doğrulanmalı.");
    if (radios_recovery_eligible(method, path, body)) {
        gchar *payload = json_to_string(body, FALSE);
        if (strlen(payload) > request_limit(path, body)) {
            g_free(payload); return fail(error, RADIOS_DATA_CLIENT_ERROR_VALIDATION, "İşlem kuyruğu gövdesi boyut sınırını aşıyor.");
        }
        gboolean safe = !strstr(payload, client->token) && !strstr(payload, client->original_token) && !strstr(payload, client->endpoint_token);
        g_free(payload);
        if (!safe) return fail(error, RADIOS_DATA_CLIENT_ERROR_VALIDATION, "Gizli anahtar içeren içerik işlem kuyruğuna yazılamaz.");
    }
    g_mutex_lock(&client->identity_lock);
    gboolean prepared = radios_recovery_prepare(client->recovery, method, path, body, error);
    g_mutex_unlock(&client->identity_lock);
    return prepared;
}
JsonNode *radios_data_client_request_cancellable_sync(RadioSDataClient *client, const gchar *method,
    const gchar *path, JsonNode *body, const RadioSCancelToken *cancel, GCancellable *cancellable, GError **error)
{
    g_return_val_if_fail(error == NULL || *error == NULL, NULL);
    if (!client) { fail(error, RADIOS_DATA_CLIENT_ERROR_INVALID_CONFIG, "Veri istemcisi hazır değil."); return NULL; }
    if (radios_recovery_eligible(method, path, body) && client->allow_identity_override && !g_atomic_int_get(&client->identity_verified) &&
        !ensure_identity(client, cancel, cancellable, error)) return NULL;
    if (!radios_data_client_prepare(client, method, path, body, error)) return NULL;
    GError *failure = NULL;
    JsonNode *node = NULL;
    if (!radios_recovery_eligible(method, path, body) || ensure_identity(client, cancel, cancellable, &failure))
        node = request_transport(client, method, path, body, cancel, cancellable, &failure);
    if (node && g_str_equal(path, "/v1/workflows/desktop/context") && !verify_context(client, node, &failure)) g_clear_pointer(&node, json_node_free);
    if (radios_recovery_eligible(method, path, body)) {
        gint blocked = 0;
        if (failure && failure->domain == RADIOS_DATA_CLIENT_ERROR &&
            (failure->code == RADIOS_DATA_CLIENT_ERROR_FORBIDDEN || failure->code == RADIOS_DATA_CLIENT_ERROR_CONFLICT ||
             failure->code == RADIOS_DATA_CLIENT_ERROR_NOT_FOUND || failure->code == RADIOS_DATA_CLIENT_ERROR_VALIDATION)) blocked = failure->code;
        GError *disk = NULL;
        g_mutex_lock(&client->identity_lock);
        gboolean finished = radios_recovery_finish(client->recovery, body, node != NULL, blocked, &disk);
        g_mutex_unlock(&client->identity_lock);
        if (!finished) {
            if (!failure) {
                g_clear_pointer(&node, json_node_free);
                g_set_error_literal(&failure, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_COMMITTED,
                    "Veri işlemi kabul edildi; yerel dayanıklı alındı temizliği doğrulanamadı. Aynı işlem kimliği korunuyor.");
            }
            g_clear_error(&disk);
        }
    }
    if (failure) g_propagate_error(error, failure);
    return node;
}
JsonNode *radios_data_client_recovery_status(RadioSDataClient *client, GError **error)
{
    if (client && client->allow_identity_override && !g_atomic_int_get(&client->identity_verified)) {
        fail(error, RADIOS_DATA_CLIENT_ERROR_INVALID_CONFIG, "Kurtarma kullanıcısının kimliği henüz doğrulanmadı."); return NULL;
    }
    if (!client) return NULL;
    g_mutex_lock(&client->identity_lock);
    JsonNode *status = radios_recovery_status(client->recovery, error);
    g_mutex_unlock(&client->identity_lock);
    return status;
}
guint radios_data_client_recover_sync(RadioSDataClient *client, const RadioSCancelToken *cancel, guint limit, GError **error)
{
    /* Resolve an override's real user BEFORE looking at any installation-user
     * outbox. Previous users' envelopes must never be replayed as this user. */
    if (client->allow_identity_override && !g_atomic_int_get(&client->identity_verified) && !ensure_identity(client, cancel, NULL, error)) return 0;
    g_mutex_lock(&client->identity_lock);
    JsonNode *pending = radios_recovery_pending(client->recovery, error);
    g_mutex_unlock(&client->identity_lock);
    if (!pending) return 0;
    JsonArray *rows = json_node_get_array(pending); guint count = 0;
    if (json_array_get_length(rows) && !radios_data_client_handshake_sync(client, cancel, error)) { json_node_free(pending); return 0; }
    for (guint i = 0; i < MIN(limit, json_array_get_length(rows)); ++i) {
        if (radios_cancelled(cancel)) break;
        JsonObject *row = json_array_get_object_element(rows, i);
        JsonNode *body = json_object_get_member(row, "body");
        GError *failure = NULL;
        JsonNode *response = radios_data_client_request_sync(client, string_member(row, "method"), string_member(row, "path"), body, cancel, &failure);
        if (response) { ++count; json_node_free(response); }
        if (failure) {
            gboolean definitive = failure->domain == RADIOS_DATA_CLIENT_ERROR &&
                (failure->code == RADIOS_DATA_CLIENT_ERROR_FORBIDDEN || failure->code == RADIOS_DATA_CLIENT_ERROR_CONFLICT ||
                 failure->code == RADIOS_DATA_CLIENT_ERROR_NOT_FOUND || failure->code == RADIOS_DATA_CLIENT_ERROR_VALIDATION);
            if (!definitive) { g_propagate_error(error, failure); break; }
            g_clear_error(&failure);
            /* A successor CAS may equal somebody else's external revision.
             * Rescan before continuing so a rejected ancestor fences its tail. */
            break;
        }
    }
    json_node_free(pending); return count;
}

JsonNode *
radios_data_client_request_sync(RadioSDataClient *client, const gchar *method,
    const gchar *path, JsonNode *body, const RadioSCancelToken *cancel, GError **error)
{
    return radios_data_client_request_cancellable_sync(client, method, path, body, cancel, NULL, error);
}

static gint source_name_compare(gconstpointer a, gconstpointer b)
{ return strcmp(*(gchar * const *)a, *(gchar * const *)b); }
/* Streams one source file into the digest. Opened with share-delete access,
 * so a service update replacing a module is never blocked by this reader. */
static gboolean source_hash_file(GChecksum *hash, gint dir, const gchar *directory, const gchar *name, guchar *chunk, gsize size, GError **error)
{
    gint fd = radios_fs_openat(dir, name, RADIOS_O_RDONLY | RADIOS_O_NOFOLLOW | RADIOS_O_CLOEXEC, 0);
    if (fd < 0) return radios_platform_set_io_error(error, errno, "%s\\%s", directory, name);
    for (;;) {
        gint amount = read(fd, chunk, (guint) size);
        if (amount < 0 && errno == EINTR) continue;
        if (amount < 0) { gint saved = errno; close(fd); return radios_platform_set_io_error(error, saved, "%s\\%s", directory, name); }
        if (amount == 0) break;
        g_checksum_update(hash, chunk, (gssize) amount);
    }
    close(fd);
    return TRUE;
}
/* Cross-language contract with sdbase/runtime_version.py: regular files (not
 * links) whose name ends with the exact lowercase ".py", ordered by the UTF-8
 * bytes of the name, hashed as name NUL raw-bytes NUL. Windows name matching
 * and ordering are case-insensitive, so neither is used here. */
static gchar *installed_source_digest(RadioSDataClient *client, GError **error)
{
    gchar *directory = g_build_filename(client->source_dir, "src", "sdbase", NULL);
    RadioSFileInfo info;
    if (radios_fs_stat(directory, TRUE, &info) != 0 || info.type != RADIOS_FILE_TYPE_DIRECTORY) { g_free(directory); return NULL; }
    gint dir = radios_fs_open(directory, RADIOS_O_RDONLY | RADIOS_O_DIRECTORY | RADIOS_O_CLOEXEC, 0);
    if (dir < 0) { radios_platform_set_io_error(error, errno, "%s", directory); g_free(directory); return NULL; }
    GPtrArray *listed = radios_fd_list_names(dir, error);
    if (!listed) { close(dir); g_free(directory); return NULL; }
    GPtrArray *names = g_ptr_array_new();
    for (guint i = 0; i < listed->len; ++i) {
        gchar *name = g_ptr_array_index(listed, i);
        if (g_str_has_suffix(name, ".py") && radios_fs_statat(dir, name, FALSE, &info) == 0 &&
            info.type == RADIOS_FILE_TYPE_REGULAR)
            g_ptr_array_add(names, name);
    }
    g_ptr_array_sort(names, source_name_compare);
    GChecksum *hash = g_checksum_new(G_CHECKSUM_SHA256); gboolean ok = TRUE;
    gsize size = 64 * 1024; guchar *chunk = g_malloc(size);
    for (guint i = 0; ok && i < names->len; ++i) {
        const gchar *name = g_ptr_array_index(names, i);
        g_checksum_update(hash, (const guchar *)name, (gssize) strlen(name) + 1);
        ok = source_hash_file(hash, dir, directory, name, chunk, size, error);
        g_checksum_update(hash, (const guchar *)"", 1);
    }
    gchar *digest = ok ? g_strdup(g_checksum_get_string(hash)) : NULL;
    g_free(chunk); g_checksum_free(hash); g_ptr_array_unref(names); g_ptr_array_unref(listed);
    close(dir); g_free(directory); return digest;
}
static gboolean supports_desktop_kinds(JsonObject *capabilities)
{
    JsonNode *node = json_object_get_member(capabilities, "desktop_configuration_kinds");
    if (!node || !JSON_NODE_HOLDS_ARRAY(node)) return FALSE;
    JsonArray *kinds = json_node_get_array(node);
    const gchar *required[] = {"automation", "automation_calendar", "weekly_programme", "live_players"};
    for (guint i = 0; i < G_N_ELEMENTS(required); ++i) {
        gboolean found = FALSE;
        for (guint j = 0; j < json_array_get_length(kinds); ++j) {
            JsonNode *kind = json_array_get_element(kinds, j);
            if (JSON_NODE_HOLDS_VALUE(kind) && json_node_get_value_type(kind) == G_TYPE_STRING &&
                !g_strcmp0(json_node_get_string(kind), required[i])) { found = TRUE; break; }
        }
        if (!found) return FALSE;
    }
    return TRUE;
}

gboolean
radios_data_client_handshake_sync(RadioSDataClient *client,
                                  const RadioSCancelToken *cancel, GError **error)
{
    JsonNode *health = radios_data_client_request_sync(client, "GET", "/health", NULL, cancel, error);
    if (health == NULL)
        return FALSE;
    gboolean matches = JSON_NODE_HOLDS_OBJECT(health) &&
            g_strcmp0(string_member(json_node_get_object(health), "status"), "ok") == 0 &&
            g_strcmp0(string_member(json_node_get_object(health), "store_id"), client->store_id) == 0;
    json_node_unref(health);
    if (!matches)
        return fail(error, RADIOS_DATA_CLIENT_ERROR_STORE_MISMATCH, "Bağlanan servis beklenen RadioS veritabanına ait değil.");
    JsonNode *capabilities = radios_data_client_request_sync(client, "GET", "/v1/capabilities", NULL, cancel, error);
    if (capabilities == NULL)
        return FALSE;
    JsonObject *features = JSON_NODE_HOLDS_OBJECT(capabilities) ? json_node_get_object(capabilities) : NULL;
    gboolean compatible = features && integer_member(features, "schema_version", 4) &&
        integer_member(features, "desktop_api_version", 1) && supports_desktop_kinds(features);
    /* Optional workflow features the service announces (absent: none). */
    JsonNode *listed = features ? json_object_get_member(features, "workflow_features") : NULL;
    GPtrArray *names = g_ptr_array_new();
    for (guint i = 0; listed && JSON_NODE_HOLDS_ARRAY(listed) && i < json_array_get_length(json_node_get_array(listed)); ++i) {
        JsonNode *name = json_array_get_element(json_node_get_array(listed), i);
        if (JSON_NODE_HOLDS_VALUE(name) && json_node_get_value_type(name) == G_TYPE_STRING) g_ptr_array_add(names, g_strdup(json_node_get_string(name)));
    }
    g_ptr_array_add(names, NULL);
    gchar **workflow_features = (gchar **)g_ptr_array_free(names, FALSE);
    GError *source_error = NULL;
    gchar *expected_digest = compatible ? installed_source_digest(client, &source_error) : NULL;
    if (source_error) {
        g_clear_error(&source_error); g_free(expected_digest); json_node_unref(capabilities); g_strfreev(workflow_features);
        return fail(error, RADIOS_DATA_CLIENT_ERROR_SCHEMA_MISMATCH, "Kurulu veri servisi kaynak sürümü okunamadı; bağlantı doğrulanamadı.");
    }
    if (expected_digest) compatible = !g_strcmp0(expected_digest, string_member(features, "source_digest"));
    g_free(expected_digest); json_node_unref(capabilities);
    if (!compatible) {
        g_strfreev(workflow_features);
        return fail(error, RADIOS_DATA_CLIENT_ERROR_SCHEMA_MISMATCH,
            "Veri servisi sürümü kurulu RadioS kaynaklarıyla uyumlu değil; yerel servis güncellenip yeniden başlatılmalı.");
    }
    g_mutex_lock(&client->identity_lock);
    g_strfreev(client->workflow_features); client->workflow_features = workflow_features;
    g_mutex_unlock(&client->identity_lock);
    g_atomic_int_set(&client->handshake_verified, 1);
    if (client->allow_identity_override && !g_atomic_int_get(&client->identity_verified)) return context_fetch_verify(client, cancel, NULL, error);
    return TRUE;
}

gboolean
radios_data_client_has_feature(RadioSDataClient *client, const gchar *name)
{
    if (client == NULL || name == NULL) return FALSE;
    g_mutex_lock(&client->identity_lock);
    gboolean found = client->workflow_features != NULL && g_strv_contains((const gchar *const *)client->workflow_features, name);
    g_mutex_unlock(&client->identity_lock);
    return found;
}
