#include "radios/recovery.h"
#include "radios/platform.h"
#include <errno.h>
#include <string.h>
#include <gio/gio.h>

#define FILE_LIMIT (6u * 1024 * 1024 + 16384u)
#define ENTRY_LIMIT 512u
/* Owner-only object (owner = current user, no access for others): the Windows
 * form of "st_uid == getuid() && !(st_mode & 077)". */
#define PRIVATE_CHECKS (RADIOS_PRIVATE_OWNER | RADIOS_PRIVATE_NO_OTHERS)
struct RadioSRecovery { gchar *directory, *store, *station, *user, *endpoint; };

static const gchar *str(JsonObject *o, const gchar *key)
{
    JsonNode *n = json_object_get_member(o, key);
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : "";
}
static const gchar *key_of(JsonNode *body)
{
    if (!body || !JSON_NODE_HOLDS_OBJECT(body)) return "";
    JsonObject *o = json_node_get_object(body);
    const gchar *key = str(o, "idempotency_key");
    return *key ? key : str(o, "operation_key");
}
typedef enum {
    BODY_GENERIC, BODY_STREAM_PUT, BODY_STREAM_CONFIG, BODY_ROUTING_PUT,
    BODY_ROUTING_CONFIG, BODY_CONFIGS, BODY_CONFIG, BODY_GRAPH, BODY_NODES,
    BODY_NODE, BODY_STREAM_SETTINGS
} BodyScope;
static gboolean saved_password_valid(JsonObject *object, guint limit)
{
    JsonNode *flag = json_object_get_member(object, "password_saved");
    JsonNode *password = json_object_get_member(object, "password");
    if (!flag || !JSON_NODE_HOLDS_VALUE(flag) || json_node_get_value_type(flag) != G_TYPE_BOOLEAN) return FALSE;
    if (!json_node_get_boolean(flag)) return password == NULL;
    if (!password || !JSON_NODE_HOLDS_VALUE(password) || json_node_get_value_type(password) != G_TYPE_STRING) return FALSE;
    const gchar *text = json_node_get_string(password);
    if (!text || strlen(text) > limit || !g_utf8_validate(text, -1, NULL)) return FALSE;
    for (const guchar *p = (const guchar *)text; *p; ++p) if (*p < 32 || *p == 127) return FALSE;
    return TRUE;
}
static BodyScope child_scope(JsonObject *object, const gchar *key, BodyScope scope)
{
    if (scope == BODY_STREAM_PUT && g_str_equal(key, "config")) return BODY_STREAM_CONFIG;
    if (scope == BODY_ROUTING_PUT && g_str_equal(key, "config")) return BODY_ROUTING_CONFIG;
    if (scope == BODY_ROUTING_CONFIG && g_str_equal(key, "configs")) {
        JsonNode *version = json_object_get_member(object, "schema_version");
        if (version && JSON_NODE_HOLDS_VALUE(version) && json_node_get_value_type(version) == G_TYPE_INT64 &&
            json_node_get_int(version) == 2) return BODY_CONFIGS;
    }
    if (scope == BODY_CONFIG && g_str_equal(key, "graph")) return BODY_GRAPH;
    if (scope == BODY_GRAPH && g_str_equal(key, "nodes")) return BODY_NODES;
    if (scope == BODY_NODE && g_str_equal(str(object, "kind"), "stream") && g_str_equal(key, "settings")) return BODY_STREAM_SETTINGS;
    return BODY_GENERIC;
}
static gboolean safe_body(JsonNode *node, guint depth, BodyScope scope)
{
    if (!node || depth > 32) return FALSE;
    if (JSON_NODE_HOLDS_OBJECT(node)) {
        JsonObject *o = json_node_get_object(node); GList *keys = json_object_get_members(o);
        gboolean safe = TRUE;
        for (GList *p = keys; safe && p; p = p->next) {
            gchar *lower = g_ascii_strdown(p->data, -1);
            gboolean saved_field = (scope == BODY_STREAM_CONFIG || scope == BODY_STREAM_SETTINGS) &&
                (g_str_equal(p->data, "password") || g_str_equal(p->data, "password_saved")) &&
                saved_password_valid(o, scope == BODY_STREAM_CONFIG ? 256 : 1024);
            safe = saved_field || (!strstr(lower, "password") && !strstr(lower, "passwd") && !strstr(lower, "secret") &&
                !g_str_equal(lower, "token") && !g_str_has_suffix(lower, "_token") && !g_str_equal(lower, "authorization"));
            g_free(lower);
            if (safe) safe = safe_body(json_object_get_member(o, p->data), depth + 1, child_scope(o, p->data, scope));
        }
        g_list_free(keys); return safe;
    }
    if (JSON_NODE_HOLDS_ARRAY(node)) {
        JsonArray *a = json_node_get_array(node);
        BodyScope item = scope == BODY_CONFIGS ? BODY_CONFIG : scope == BODY_NODES ? BODY_NODE : BODY_GENERIC;
        for (guint i = 0; i < json_array_get_length(a); ++i) if (!safe_body(json_array_get_element(a, i), depth + 1, item)) return FALSE;
    }
    return TRUE;
}
static gboolean safe_request_body(const gchar *path, JsonNode *body)
{
    BodyScope scope = BODY_GENERIC;
    /* Only the two persisted stream settings schemas may carry an explicitly
     * saved encoder password. Headers, tokens, receipts and other documents
     * keep the recursive secret exclusion, including nested extra fields. */
    if (g_str_equal(path, "/v1/workflows/desktop/put") && body && JSON_NODE_HOLDS_OBJECT(body)) {
        const gchar *kind = str(json_node_get_object(body), "kind");
        if (g_str_equal(kind, "broadcast_stream")) scope = BODY_STREAM_PUT;
        else if (g_str_equal(kind, "audio_routing")) scope = BODY_ROUTING_PUT;
    }
    return safe_body(body, 0, scope);
}
gboolean radios_recovery_eligible(const gchar *method, const gchar *path, JsonNode *body)
{
    if (!method || !path || !g_str_equal(method, "POST") || !g_uuid_string_is_valid(key_of(body))) return FALSE;
    static const gchar *const exact[] = {
        "/v1/workflows/desktop/put", "/v1/workflows/desktop-media/import", "/v1/workflows/desktop-media/metadata-save",
        "/v1/workflows/desktop-broadcast/profile-save", "/v1/workflows/desktop-broadcast/run-start", "/v1/workflows/desktop-broadcast/run-event",
        "/v1/workflows/desktop-advert/run-start", "/v1/workflows/desktop-advert/run-event",
        "/v1/workflows/desktop-admin/user-save", "/v1/workflows/desktop-admin/token-revoke", "/v1/workflows/desktop-admin/recovery-close",
        "/v1/workflows/desktop-programme/submit", "/v1/workflows/desktop-rekordbox/apply"
    };
    for (guint i = 0; i < G_N_ELEMENTS(exact); ++i) if (g_str_equal(path, exact[i])) return TRUE;
    /* These workflow families use immutable full-body receipts. One-time user
     * key creation and administrative key delivery are deliberately excluded. */
    if (g_str_has_prefix(path, "/v1/workflows/desktop-playlists/") || g_str_has_prefix(path, "/v1/workflows/desktop-advert/"))
        return !strchr(path, '?') && !strchr(path, '#');
    return FALSE;
}
static gboolean error_io(GError **error)
{
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
        "Dayanıklı işlem kuyruğu okunamadı/yazılamadı; veri isteği gönderilmedi. Özel kurtarma dosyalarını kontrol edin.");
    return FALSE;
}
RadioSRecovery *radios_recovery_new(const RadioSConfig *config, const gchar *store,
    const gchar *station, const gchar *user, const gchar *endpoint)
{
    RadioSRecovery *r = g_new0(RadioSRecovery, 1);
    r->directory = g_build_filename(config->project_root, "data", "recovery", store, user, NULL);
    r->store = g_strdup(store); r->station = g_strdup(station); r->user = g_strdup(user); r->endpoint = g_strdup(endpoint);
    return r;
}
void radios_recovery_free(RadioSRecovery *r)
{
    if (!r) return;
    g_free(r->directory); g_free(r->store); g_free(r->station); g_free(r->user); g_free(r->endpoint); g_free(r);
}
/* Walk every component from the volume root (C:\, \\server\share\) with
 * no-follow opens. Parent project directories can be public; the recovery
 * tree must be owned and private. No path-based reopen.
 * The tree is <root>\data\recovery\<store>\<user>: the last four components
 * (data is created when missing, recovery/<store>/<user> are created private
 * and verified private), decided by position so a project that itself lives
 * below a folder named "recovery" is not mistaken for the outbox tree. */
static gint directory_open(RadioSRecovery *r, gboolean create, GError **error)
{
    gchar **parts = NULL;
    gint dir = -1;
    if (radios_path_component_valid(r->store) && radios_path_component_valid(r->user))
        dir = radios_fs_open_root(r->directory, &parts);
    else
        errno = EINVAL;
    guint count = parts ? g_strv_length(parts) : 0;
    if (dir >= 0 && count < 4) { close(dir); dir = -1; errno = EINVAL; }
    for (guint i = 0; dir >= 0 && i < count; ++i) {
        gboolean private = i + 3 >= count;
        if (create && i + 4 >= count) {
            gint made = radios_fs_mkdirat(dir, parts[i], 0700);
            if ((made < 0 && errno != EEXIST) || (made == 0 && radios_fd_sync_dir(dir) < 0)) { close(dir); dir = -1; break; }
        }
        gint next = radios_fs_openat(dir, parts[i], RADIOS_O_RDONLY | RADIOS_O_DIRECTORY | RADIOS_O_NOFOLLOW | RADIOS_O_CLOEXEC, 0);
        if (next < 0) { gint saved = errno; close(dir); errno = saved; dir = -1; break; }
        if (private && !radios_fd_is_private(next, PRIVATE_CHECKS)) {
            gint saved = errno; close(next); close(dir); errno = saved; dir = -1; break;
        }
        close(dir); dir = next;
    }
    gint saved = errno; g_strfreev(parts);
    if (dir < 0 && (create || saved != ENOENT)) error_io(error);
    return dir;
}
/* Regular, owner-only file with exactly one name (no hard link elsewhere). */
static gboolean private_file(gint fd, RadioSFileInfo *info)
{
    return radios_fd_stat(fd, info) == 0 && info->type == RADIOS_FILE_TYPE_REGULAR && info->nlink == 1 &&
        radios_fd_is_private(fd, PRIVATE_CHECKS);
}
/* Exclusive per-user lock (LockFileEx region shared with the Python tools);
 * released with radios_fd_close(). */
static gint lock_open(gint dir, GError **error)
{
    gint fd = radios_fs_openat(dir, ".lock", RADIOS_O_RDWR | RADIOS_O_CREAT | RADIOS_O_NOFOLLOW | RADIOS_O_NONBLOCK | RADIOS_O_CLOEXEC, 0600);
    RadioSFileInfo info;
    if (fd < 0 || !private_file(fd, &info) || radios_fd_lock(fd, RADIOS_LOCK_EX) < 0) {
        if (fd >= 0) radios_fd_close(fd);
        error_io(error); return -1;
    }
    return fd;
}
static JsonNode *read_entry(gint dir, const gchar *name, GError **error)
{
    gint fd = radios_fs_openat(dir, name, RADIOS_O_RDONLY | RADIOS_O_NOFOLLOW | RADIOS_O_NONBLOCK | RADIOS_O_CLOEXEC, 0);
    RadioSFileInfo info;
    if (fd < 0) { if (errno != ENOENT) error_io(error); return NULL; }
    if (!private_file(fd, &info) || info.size < 2 || info.size > (gint64)FILE_LIMIT) {
        close(fd); error_io(error); return NULL;
    }
    gsize size = (gsize)info.size;
    gchar *bytes = g_malloc(size + 1); gsize used = 0;
    while (used < size) {
        gint n = read(fd, bytes + used, (guint)(size - used));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (gsize)n;
    }
    close(fd); bytes[used] = 0;
    JsonParser *p = json_parser_new(); json_parser_set_strict(p, TRUE);
    gboolean valid = used == size && !memchr(bytes, 0, used) &&
        g_utf8_validate(bytes, used, NULL) && json_parser_load_from_data(p, bytes, used, NULL);
    JsonNode *node = valid ? json_node_copy(json_parser_get_root(p)) : NULL;
    if (!node || !JSON_NODE_HOLDS_OBJECT(node)) { g_clear_pointer(&node, json_node_free); error_io(error); }
    g_free(bytes); g_object_unref(p); return node;
}
static gchar *checksum(JsonNode *node)
{
    JsonObject *object = json_node_get_object(node); gchar *old = g_strdup(str(object, "sha256"));
    json_object_remove_member(object, "sha256"); gchar *bytes = json_to_string(node, FALSE);
    gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, bytes, -1); g_free(bytes);
    if (*old) json_object_set_string_member(object, "sha256", old);
    g_free(old); return digest;
}
static gboolean matches(RadioSRecovery *r, JsonNode *node, const gchar *filename)
{
    JsonObject *o = json_node_get_object(node); JsonNode *body = json_object_get_member(o, "body");
    gchar *actual = checksum(node); gboolean intact = g_str_equal(actual, str(o, "sha256")); g_free(actual);
    const gchar *key = key_of(body); gchar *name = g_strconcat(key, ".json", NULL);
    JsonNode *v = json_object_get_member(o, "version"), *created = json_object_get_member(o, "created_us");
    gboolean ok = intact && v && JSON_NODE_HOLDS_VALUE(v) && json_node_get_value_type(v) == G_TYPE_INT64 && json_node_get_int(v) == 1 &&
        created && JSON_NODE_HOLDS_VALUE(created) && json_node_get_value_type(created) == G_TYPE_INT64 && json_node_get_int(created) > 0 &&
        g_str_equal(str(o, "store_id"), r->store) && g_str_equal(str(o, "station_id"), r->station) &&
        g_str_equal(str(o, "user_id"), r->user) && g_str_equal(str(o, "endpoint_id"), r->endpoint) &&
        g_str_equal(name, filename) && (g_str_equal(str(o, "state"), "pending") || g_str_equal(str(o, "state"), "blocked")) &&
        radios_recovery_eligible(str(o, "method"), str(o, "path"), body) && safe_request_body(str(o, "path"), body) &&
        g_str_equal(str(json_node_get_object(body), "station_id"), r->station);
    g_free(name); return ok;
}
static gboolean atomic_write(gint dir, const gchar *name, JsonNode *node, GError **error)
{
    gchar *digest = checksum(node); json_object_set_string_member(json_node_get_object(node), "sha256", digest); g_free(digest);
    gchar *uuid = g_uuid_string_random(), *temp = g_strconcat(".write-", uuid, NULL); g_free(uuid);
    gchar *bytes = json_to_string(node, FALSE); gsize length = strlen(bytes), used = 0;
    /* 0600: created with the private owner-only descriptor. */
    gint fd = radios_fs_openat(dir, temp, RADIOS_O_WRONLY | RADIOS_O_CREAT | RADIOS_O_EXCL | RADIOS_O_NOFOLLOW | RADIOS_O_CLOEXEC, 0600);
    gboolean ok = fd >= 0 && length <= FILE_LIMIT, renamed = FALSE;
    while (ok && used < length) {
        gint n = write(fd, bytes + used, (guint)(length - used));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = FALSE; break; }
        used += (gsize)n;
    }
    if (ok) ok = radios_fd_sync(fd) == 0;
    if (fd >= 0 && close(fd) < 0) ok = FALSE;
    /* Atomic replacement (POSIX-semantics rename, retried while a scanner
     * holds the target briefly); the temporary name is removed on failure. */
    if (ok) ok = renamed = radios_fs_renameat(dir, temp, dir, name) == 0;
    if (ok) ok = radios_fd_sync_dir(dir) == 0;
    if (fd >= 0 && !renamed) (void)radios_fs_unlinkat(dir, temp, 0);
    g_free(temp); g_free(bytes);
    return ok ? TRUE : error_io(error);
}
static gint entry_count(gint dir)
{
    GError *unused = NULL; GPtrArray *names = radios_fd_list_names(dir, &unused);
    if (!names) { g_clear_error(&unused); return -1; }
    gint count = 0;
    for (guint i = 0; i < names->len; ++i) if (g_str_has_suffix(g_ptr_array_index(names, i), ".json")) ++count;
    g_ptr_array_unref(names); return count;
}
gboolean radios_recovery_prepare(RadioSRecovery *r, const gchar *method, const gchar *path, JsonNode *body, GError **error)
{
    if (!error) { GError *local = NULL; gboolean ok = radios_recovery_prepare(r, method, path, body, &local); g_clear_error(&local); return ok; }
    if (!radios_recovery_eligible(method, path, body)) return TRUE;
    if (!safe_request_body(path, body) || !g_str_equal(str(json_node_get_object(body), "station_id"), r->station)) return error_io(error);
    gint dir = directory_open(r, TRUE, error); if (dir < 0) return FALSE;
    gint lock = lock_open(dir, error); if (lock < 0) { close(dir); return FALSE; }
    gchar *name = g_strconcat(key_of(body), ".json", NULL); JsonNode *old = read_entry(dir, name, error);
    gboolean ok = !error || !*error;
    if (old) {
        JsonObject *o = json_node_get_object(old);
        ok = matches(r, old, name) && g_str_equal(str(o, "method"), method) && g_str_equal(str(o, "path"), path) && json_node_equal(json_object_get_member(o, "body"), body);
        if (!ok) error_io(error);
    } else if (ok) {
        gint count = entry_count(dir);
        if (count < 0 || count >= (gint)ENTRY_LIMIT) ok = error_io(error);
        else {
            JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *o = json_object_new(); json_node_take_object(node, o);
            json_object_set_int_member(o, "version", 1); json_object_set_int_member(o, "created_us", g_get_real_time());
            json_object_set_string_member(o, "store_id", r->store); json_object_set_string_member(o, "station_id", r->station);
            json_object_set_string_member(o, "user_id", r->user); json_object_set_string_member(o, "endpoint_id", r->endpoint);
            json_object_set_string_member(o, "method", method); json_object_set_string_member(o, "path", path);
            json_object_set_string_member(o, "state", "pending"); json_object_set_member(o, "body", json_node_copy(body));
            ok = atomic_write(dir, name, node, error); json_node_free(node);
        }
    }
    g_clear_pointer(&old, json_node_free); g_free(name); radios_fd_close(lock); close(dir); return ok;
}
gboolean radios_recovery_finish(RadioSRecovery *r, JsonNode *body, gboolean acknowledged, gint rejection_code, GError **error)
{
    if (!error) { GError *local = NULL; gboolean ok = radios_recovery_finish(r, body, acknowledged, rejection_code, &local); g_clear_error(&local); return ok; }
    if (!g_uuid_string_is_valid(key_of(body))) return TRUE;
    gint dir = directory_open(r, FALSE, error); if (dir < 0) return !error || !*error;
    gint lock = lock_open(dir, error); if (lock < 0) { close(dir); return FALSE; }
    gchar *name = g_strconcat(key_of(body), ".json", NULL); JsonNode *node = read_entry(dir, name, error);
    gboolean ok = !error || !*error;
    if (node) {
        ok = matches(r, node, name) && json_node_equal(json_object_get_member(json_node_get_object(node), "body"), body);
        if (!ok) error_io(error);
        else if (acknowledged) ok = radios_fs_unlinkat(dir, name, 0) == 0 && radios_fd_sync_dir(dir) == 0;
        else if (rejection_code) {
            json_object_set_string_member(json_node_get_object(node), "state", "blocked");
            json_object_set_int_member(json_node_get_object(node), "rejection_code", rejection_code);
            ok = atomic_write(dir, name, node, error);
        }
        if (!ok && (!error || !*error)) error_io(error);
    }
    g_clear_pointer(&node, json_node_free); g_free(name); radios_fd_close(lock); close(dir); return ok;
}
static gint chronological(gconstpointer left, gconstpointer right)
{
    JsonObject *a = json_node_get_object(*(JsonNode *const *)left), *b = json_node_get_object(*(JsonNode *const *)right);
    gint64 x = json_object_get_int_member(a, "created_us"), y = json_object_get_int_member(b, "created_us");
    return x < y ? -1 : x > y ? 1 : g_strcmp0(key_of(json_object_get_member(a, "body")), key_of(json_object_get_member(b, "body")));
}
static gchar *resource(JsonObject *envelope)
{
    JsonObject *body = json_node_get_object(json_object_get_member(envelope, "body"));
    static const gchar *const ids[] = {"run_id", "document_id", "playlist_id", "campaign_id", "reservation_id", "media_id", "token_id", "user_id"};
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i)
        if (*str(body, ids[i])) return g_strconcat(ids[i], ":", str(body, ids[i]), NULL);
    return g_strdup(key_of(json_object_get_member(envelope, "body")));
}
static JsonNode *scan(RadioSRecovery *r, gboolean status, GError **error)
{
    if (!error) { GError *local = NULL; JsonNode *node = scan(r, status, &local); g_clear_error(&local); return node; }
    JsonNode *result = json_node_new(status ? JSON_NODE_OBJECT : JSON_NODE_ARRAY);
    JsonArray *rows = json_array_new(); guint pending = 0, blocked = 0;
    gint dir = directory_open(r, FALSE, error);
    if (dir >= 0) {
        gint lock = lock_open(dir, error);
        if (lock < 0) { close(dir); json_array_unref(rows); json_node_free(result); return NULL; }
        GPtrArray *names = radios_fd_list_names(dir, error);
        GPtrArray *sorted = g_ptr_array_new_with_free_func((GDestroyNotify)json_node_free);
        for (guint index = 0; names && index < names->len; ++index) {
            const gchar *name = g_ptr_array_index(names, index);
            if (!g_str_has_suffix(name, ".json")) continue;
            JsonNode *node = read_entry(dir, name, error);
            if (!node || !matches(r, node, name) || sorted->len >= ENTRY_LIMIT) {
                g_clear_pointer(&node, json_node_free); if (!error || !*error) error_io(error); break;
            }
            g_ptr_array_add(sorted, node);
        }
        if (names) g_ptr_array_unref(names);
        g_ptr_array_sort(sorted, chronological);
        GHashTable *blocked_resources = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        for (guint i = 0; i < sorted->len; ++i) {
            JsonNode *node = g_ptr_array_index(sorted, i); JsonObject *o = json_node_get_object(node);
            gboolean waiting = g_str_equal(str(o, "state"), "pending"); pending += waiting; blocked += !waiting;
            gchar *identity = resource(o); gboolean dependent = waiting && g_hash_table_contains(blocked_resources, identity);
            if (!waiting) g_hash_table_add(blocked_resources, g_strdup(identity));
            g_free(identity);
            if (status) {
                JsonObject *summary = json_object_new();
                json_object_set_string_member(summary, "operation_id", key_of(json_object_get_member(o, "body")));
                json_object_set_string_member(summary, "path", str(o, "path")); json_object_set_string_member(summary, "state", str(o, "state"));
                json_object_set_int_member(summary, "created_us", json_object_get_int_member(o, "created_us"));
                if (json_object_has_member(o, "rejection_code")) json_object_set_int_member(summary, "rejection_code", json_object_get_int_member(o, "rejection_code"));
                if (dependent) json_object_set_boolean_member(summary, "dependency_blocked", TRUE);
                json_array_add_object_element(rows, summary);
            } else if (waiting && !dependent) json_array_add_element(rows, json_node_copy(node));
        }
        g_hash_table_unref(blocked_resources);
        g_ptr_array_unref(sorted); radios_fd_close(lock); close(dir);
    }
    if (error && *error) { json_array_unref(rows); json_node_free(result); return NULL; }
    if (status) {
        JsonObject *o = json_object_new(); json_node_take_object(result, o);
        json_object_set_int_member(o, "pending", pending); json_object_set_int_member(o, "blocked", blocked);
        json_object_set_array_member(o, "operations", rows);
    } else json_node_take_array(result, rows);
    return result;
}
JsonNode *radios_recovery_pending(RadioSRecovery *r, GError **error) { return scan(r, FALSE, error); }
JsonNode *radios_recovery_status(RadioSRecovery *r, GError **error) { return scan(r, TRUE, error); }
gint radios_recovery_gui_lock(const RadioSConfig *config, GError **error)
{
    const gint directory = RADIOS_O_RDONLY | RADIOS_O_DIRECTORY | RADIOS_O_NOFOLLOW | RADIOS_O_CLOEXEC;
    gint root = config && config->project_root ? radios_fs_open(config->project_root, directory, 0) : -1;
    gint data = root >= 0 ? radios_fs_openat(root, "data", directory, 0) : -1;
    gint dir = data >= 0 ? radios_fs_openat(data, "database-service", directory, 0) : -1;
    if (root >= 0) close(root);
    if (data >= 0) close(data);
    RadioSFileInfo info; gint fd = -1;
    /* The service creates data\database-service owner-only; the GUI holds a
     * shared lock on gui.lock for its lifetime, the offline restore takes it
     * exclusively (same LockFileEx region in C and Python). */
    if (dir >= 0 && radios_fd_is_private(dir, PRIVATE_CHECKS)) {
        fd = radios_fs_openat(dir, "gui.lock", RADIOS_O_RDWR | RADIOS_O_CREAT | RADIOS_O_NOFOLLOW | RADIOS_O_NONBLOCK | RADIOS_O_CLOEXEC, 0600);
        if (fd >= 0 && (!private_file(fd, &info) || radios_fd_lock(fd, RADIOS_LOCK_SH | RADIOS_LOCK_NB) < 0 ||
            radios_fs_statat(dir, "restore-pending.json", FALSE, &info) == 0 || errno != ENOENT)) { radios_fd_close(fd); fd = -1; }
    }
    if (dir >= 0) close(dir);
    if (fd < 0) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_BUSY,
        "Geri yükleme sürüyor veya yarım kaldı; offline kurtarma tamamlanmadan uygulama açılamaz.");
    return fd;
}
