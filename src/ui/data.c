#include "radios/data.h"
#include <string.h>

typedef struct { GWeakRef owner; RadioSDataRefresh refresh; } Refresh;
struct RadioSData {
    gint references;
    RadioSViewContext *ctx;
    RadioSDataClient *client;
    GPtrArray *refreshes;
    GHashTable *list_requests;
    guint refresh_timer;
    JsonNode *context;
    gboolean connected, connecting, closed;
    GSubprocess *launcher;
    GCancellable *launch_cancel;
    guint launch_timeout;
    guint recovery_timer;
    gboolean recovering, updating_service;
};
typedef struct {
    RadioSData *data;
    RadioSDataClient *client;
    const RadioSConfig *config;
    GWeakRef owner;
    gchar *method, *path;
    JsonNode *body;
    RadioSDataCallback callback;
    gpointer user;
    GDestroyNotify destroy;
    gboolean completed;
    gboolean report_error;
    gchar *list_key;
    GPtrArray *followers;
} Request;
struct RadioSDocument {
    RadioSViewContext *ctx;
    GtkWidget *owner;
    gchar *kind, *name, *id, *lookup_id;
    gint64 revision;
    RadioSDocumentApply apply;
    gpointer user;
    gboolean ready, busy, dirty, loading;
    JsonNode *pending;
    JsonNode *retry_body;
    RadioSDataClient *read_client, *baseline_client;
    gboolean automatic, autosave_conflict;
    guint64 changes, submitted_changes, read_changes;
    gint64 read_revision;
    gboolean explicit_reload, refresh_again;
    JsonNode *rejected;
    guint retry_timer, retry_attempt;
    RadioSDocumentStatus status;
    gpointer status_user;
};

const gchar *radios_json_string(JsonObject *object, const gchar *key, const gchar *fallback)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ?
        json_node_get_string(node) : fallback;
}
gint64 radios_json_int(JsonObject *object, const gchar *key, gint64 fallback)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64 ?
        json_node_get_int(node) : fallback;
}
static RadioSData *data_ref(RadioSData *data) { g_atomic_int_inc(&data->references); return data; }
static JsonNode *copy_json(JsonNode *node)
{
    if (JSON_NODE_HOLDS_OBJECT(node)) {
        JsonNode *copy = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new();
        JsonObject *source = json_node_get_object(node); GList *keys = json_object_get_members(source);
        for (GList *key = keys; key; key = key->next) json_object_set_member(object, key->data, copy_json(json_object_get_member(source, key->data)));
        g_list_free(keys); json_node_take_object(copy, object); return copy;
    }
    if (JSON_NODE_HOLDS_ARRAY(node)) {
        JsonNode *copy = json_node_new(JSON_NODE_ARRAY); JsonArray *array = json_array_new(); JsonArray *source = json_node_get_array(node);
        for (guint i = 0; i < json_array_get_length(source); ++i) json_array_add_element(array, copy_json(json_array_get_element(source, i)));
        json_node_take_array(copy, array); return copy;
    }
    return json_node_copy(node);
}
static void refresh_free(gpointer value)
{
    Refresh *refresh = value; g_weak_ref_clear(&refresh->owner); g_free(refresh);
}
void radios_data_unref(RadioSData *data)
{
    if (!data || !g_atomic_int_dec_and_test(&data->references)) return;
    if (data->client) radios_data_client_unref(data->client);
    g_ptr_array_unref(data->refreshes); g_hash_table_unref(data->list_requests);
    g_clear_pointer(&data->context, json_node_free);
    g_clear_object(&data->launcher); g_clear_object(&data->launch_cancel);
    g_free(data);
}
RadioSData *radios_data_new(RadioSViewContext *ctx)
{
    RadioSData *data = g_new0(RadioSData, 1);
    data->references = 1; data->ctx = ctx;
    data->refreshes = g_ptr_array_new_with_free_func(refresh_free);
    data->list_requests = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    return data;
}
void radios_data_close(RadioSData *data)
{
    if (!data || data->closed) return;
    data->closed = TRUE; data->connected = FALSE; data->ctx = NULL;
    if (data->launch_timeout) { g_source_remove(data->launch_timeout); data->launch_timeout = 0; }
    if (data->refresh_timer) { g_source_remove(data->refresh_timer); data->refresh_timer = 0; }
    if (data->recovery_timer) { g_source_remove(data->recovery_timer); data->recovery_timer = 0; }
    if (data->launch_cancel) g_cancellable_cancel(data->launch_cancel);
    if (data->launcher) g_subprocess_force_exit(data->launcher);
}
gboolean radios_data_connected(RadioSViewContext *ctx)
{ return ctx && ctx->data && ctx->data->connected && !ctx->data->closed; }
const gchar *radios_data_station_id(RadioSViewContext *ctx)
{ return radios_data_connected(ctx) ? radios_data_client_station_id(ctx->data->client) : ""; }
const gchar *radios_data_endpoint_id(RadioSViewContext *ctx)
{ return radios_data_connected(ctx) ? radios_data_client_endpoint_id(ctx->data->client) : ""; }
JsonObject *radios_data_context(RadioSViewContext *ctx)
{ return radios_data_connected(ctx) && ctx->data->context ? json_node_get_object(ctx->data->context) : NULL; }
RadioSDataClient *radios_data_client_acquire(RadioSViewContext *ctx)
{ return radios_data_connected(ctx) ? radios_data_client_ref(ctx->data->client) : NULL; }
static void notify_data(RadioSData *data, const gchar *message)
{ if (data->ctx && data->ctx->notify) data->ctx->notify(data->ctx->notify_data, message); }
static void owner_destroyed(GtkWidget *owner, gpointer unused)
{ (void)unused; g_object_set_data(G_OBJECT(owner), "radios-data-destroyed", GINT_TO_POINTER(1)); }
static void guard_owner(GtkWidget *owner)
{
    if (!g_object_get_data(G_OBJECT(owner), "radios-data-guard")) {
        g_object_set_data(G_OBJECT(owner), "radios-data-guard", GINT_TO_POINTER(1));
        g_signal_connect(owner, "destroy", G_CALLBACK(owner_destroyed), NULL);
    }
}
static void request_free(gpointer value)
{
    Request *request = value;
    g_weak_ref_clear(&request->owner);
    g_free(request->method); g_free(request->path);
    g_clear_pointer(&request->body, json_node_free);
    if (request->client) radios_data_client_unref(request->client);
    if (request->followers) g_ptr_array_unref(request->followers);
    if (request->destroy) request->destroy(request->user);
    g_free(request->list_key); radios_data_unref(request->data); g_free(request);
}
static void request_complete(Request *request, JsonNode *node, const GError *error)
{
    GtkWidget *owner = g_weak_ref_get(&request->owner);
    if (!request->data->closed && owner && !g_object_get_data(G_OBJECT(owner), "radios-data-destroyed")) {
        if (!error && node && g_str_equal(request->method, "CONNECT")) {
            if (request->data->client) radios_data_client_unref(request->data->client);
            request->data->client = radios_data_client_ref(request->client);
        }
        if (error && request->report_error) notify_data(request->data, error->message);
        if (request->callback) {
            JsonNode *response = node ? copy_json(node) : NULL;
            request->callback(request->data->ctx, owner, response, error, request->user);
            g_clear_pointer(&response, json_node_free);
        }
    }
    if (request->destroy) { request->destroy(request->user); request->destroy = NULL; }
    g_clear_object(&owner);
}
static void request_done(GObject *source, GAsyncResult *result, gpointer unused)
{
    (void)source; (void)unused;
    GTask *task = G_TASK(result); Request *request = g_task_get_task_data(task);
    GError *error = NULL; JsonNode *node = g_task_propagate_pointer(task, &error);
    /* Remove before invoking callbacks: a refresh requested by a completion
     * needs a fresh snapshot rather than joining this completed request. */
    if (request->list_key) g_hash_table_remove(request->data->list_requests, request->list_key);
    request_complete(request, node, error);
    for (guint i = 0; request->followers && i < request->followers->len; ++i)
        request_complete(g_ptr_array_index(request->followers, i), node, error);
    g_clear_error(&error); g_clear_pointer(&node, json_node_free);
}
static gchar *request_work(gpointer value, const RadioSCancelToken *cancel, GError **unused)
{
    (void)unused;
    GTask *task = value; Request *request = g_task_get_task_data(task);
    GError *error = NULL; JsonNode *result = NULL;
    if (g_str_equal(request->method, "CONNECT")) {
        request->client = radios_data_client_new(request->config, &error);
        if (request->client && radios_data_client_handshake_sync(request->client, cancel, &error)) {
            GError *recovery_error = NULL;
            radios_data_client_recover_sync(request->client, cancel, 8, &recovery_error);
            g_clear_error(&recovery_error); /* connect/context remain readable; status exposes pending/blocked */
            JsonNode *body = json_node_new(JSON_NODE_OBJECT); json_node_take_object(body, json_object_new());
            json_object_set_string_member(json_node_get_object(body), "station_id", radios_data_client_station_id(request->client));
            result = radios_data_client_request_sync(request->client, "POST", "/v1/workflows/desktop/context", body, cancel, &error);
            json_node_free(body);
        }
    } else if (g_str_equal(request->method, "RECOVER")) {
        guint recovered = radios_data_client_recover_sync(request->client, cancel, 8, &error);
        if (!error) {
            result = radios_data_client_recovery_status(request->client, &error);
            if (result) json_object_set_int_member(json_node_get_object(result), "recovered", recovered);
        }
    } else result = radios_data_client_request_sync(request->client, request->method, request->path, request->body, cancel, &error);
    request->completed = TRUE;
    if (error) g_task_return_error(task, error);
    else if (result) g_task_return_pointer(task, result, (GDestroyNotify)json_node_free);
    else g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_FAILED, "Veri servisi sonucu alınamadı.");
    return NULL;
}
static void task_destroy(gpointer value)
{
    GTask *task = value; Request *request = g_task_get_task_data(task);
    if (!request->completed) {
        request->completed = TRUE;
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Veri işi iptal edildi.");
    }
    g_object_unref(task);
}
/* Read-only playback preparation must not sit behind scans and view
 * refreshes on the single I/O worker. Writes retain their original order. */
static gboolean playback_readiness(const gchar *method, const gchar *path, JsonNode *body)
{
    if (!g_str_equal(method, "POST")) return FALSE;
    if (g_str_equal(path, "/v1/workflows/desktop-playout/prepare")) return TRUE;
    JsonObject *object = body && JSON_NODE_HOLDS_OBJECT(body) ? json_node_get_object(body) : NULL;
    const gchar *kind = radios_json_string(object, "kind", "");
    return (g_str_equal(path, "/v1/workflows/desktop/get") && g_str_equal(kind, "weekly_programme")) ||
        (g_str_equal(path, "/v1/workflows/desktop/list") && g_str_equal(kind, "playout_history"));
}
static gboolean request_dispatch(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *method, const gchar *path, JsonNode *body,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy,
    gboolean report_error)
{
    if (!ctx || !ctx->data || ctx->data->closed || (!radios_data_connected(ctx) && !g_str_equal(method, "CONNECT"))) {
        if (ctx && ctx->notify) ctx->notify(ctx->notify_data, "Veritabanı bağlantısı hazır değil; girdiler korunuyor.");
        if (destroy) destroy(user);
        return FALSE;
    }
    guard_owner(owner);
    Request *request = g_new0(Request, 1); request->data = data_ref(ctx->data);
    if (!g_str_equal(method, "CONNECT")) request->client = radios_data_client_ref(ctx->data->client);
    request->config = ctx->config;
    g_weak_ref_init(&request->owner, owner);
    request->method = g_strdup(method); request->path = g_strdup(path);
    request->body = body ? copy_json(body) : NULL;
    request->callback = callback; request->user = user; request->destroy = destroy;
    request->report_error = report_error;
    if (g_str_equal(method, "POST") && g_str_equal(path, "/v1/workflows/desktop/list") && body) {
        gchar *payload = json_to_string(body, FALSE);
        request->list_key = g_strdup_printf("%p:%s", (void *)request->client, payload); g_free(payload);
        Request *primary = g_hash_table_lookup(ctx->data->list_requests, request->list_key);
        if (primary) {
            if (!primary->followers) primary->followers = g_ptr_array_new_with_free_func(request_free);
            g_ptr_array_add(primary->followers, request); return TRUE;
        }
        g_hash_table_insert(ctx->data->list_requests, g_strdup(request->list_key), request);
    }
    GTask *task = g_task_new(NULL, NULL, request_done, NULL);
    g_task_set_task_data(task, request, request_free);
    GError *error = NULL;
    if (!g_str_equal(method, "CONNECT") && !g_str_equal(method, "RECOVER") &&
        !radios_data_client_prepare(ctx->data->client, method, path, body, &error)) {
        request->completed = TRUE; g_task_return_error(task, error); g_object_unref(task); return TRUE;
    }
    guint64 submitted = playback_readiness(method, path, body) ?
        radios_runtime_submit_priority(ctx->runtime, request_work, task, task_destroy, &error) :
        radios_runtime_submit(ctx->runtime, request_work, task, task_destroy, &error);
    if (!submitted) {
        /* A completion is scheduled even when admission fails; keep the caller busy
         * until that completion so it cannot overlap a second request. */
        request->completed = TRUE; g_task_return_error(task, error); g_object_unref(task); return TRUE;
    }
    return TRUE;
}
gboolean radios_data_request(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *method, const gchar *path, JsonNode *body,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{
    return request_dispatch(ctx, owner, method, path, body, callback, user, destroy, TRUE);
}
void radios_data_register_refresh(RadioSViewContext *ctx, GtkWidget *owner, RadioSDataRefresh callback)
{
    if (!ctx->data) return;
    guard_owner(owner);
    Refresh *refresh = g_new0(Refresh, 1); g_weak_ref_init(&refresh->owner, owner); refresh->refresh = callback;
    g_ptr_array_add(ctx->data->refreshes, refresh);
}
static gboolean refresh_dispatch(gpointer value)
{
    RadioSData *data = value; data->refresh_timer = 0;
    if (data->closed || !data->connected) return G_SOURCE_REMOVE;
    for (guint i = 0; i < data->refreshes->len; ++i) {
        Refresh *refresh = g_ptr_array_index(data->refreshes, i);
        GtkWidget *owner = g_weak_ref_get(&refresh->owner);
        if (owner && !g_object_get_data(G_OBJECT(owner), "radios-data-destroyed")) refresh->refresh(data->ctx, owner);
        g_clear_object(&owner);
    }
    return G_SOURCE_REMOVE;
}
void radios_data_refresh(RadioSViewContext *ctx)
{
    if (!radios_data_connected(ctx)) return;
    if (ctx->data->refresh_timer) g_source_remove(ctx->data->refresh_timer);
    ctx->data->refresh_timer = g_timeout_add(350, refresh_dispatch, ctx->data);
}
static void connect_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer unused)
{
    (void)owner; (void)unused;
    RadioSData *data = ctx->data; data->connecting = FALSE; data->updating_service = FALSE;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { data->connected = FALSE; return; }
    g_clear_pointer(&data->context, json_node_free); data->context = json_node_copy(result); data->connected = TRUE;
    notify_data(data, "Veritabanı bağlı; kalıcı kayıtlar yükleniyor.");
    radios_data_refresh(ctx);
}
JsonNode *radios_data_recovery_status(RadioSViewContext *ctx)
{
    if (!ctx || !ctx->data || !ctx->data->client) return NULL;
    GError *error = NULL; JsonNode *node = radios_data_client_recovery_status(ctx->data->client, &error);
    if (error) {
        node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, json_object_new());
        json_object_set_string_member(json_node_get_object(node), "error", error->message); g_clear_error(&error);
    }
    return node;
}
gboolean radios_data_preserve(RadioSViewContext *ctx, const gchar *path, JsonNode *body)
{
    if (!ctx || !ctx->data || !ctx->data->client || ctx->data->closed) return FALSE;
    GError *error = NULL; gboolean ok = radios_data_client_prepare(ctx->data->client, "POST", path, body, &error);
    if (error) { notify_data(ctx->data, error->message); g_clear_error(&error); }
    return ok;
}
static void recovery_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer unused)
{
    (void)owner; (void)unused; ctx->data->recovering = FALSE;
    if (!error && result && radios_json_int(json_node_get_object(result), "recovered", 0) > 0) {
        notify_data(ctx->data, "Kesintiden kalan işlemler aynı kimlikle doğrulandı; güncel veriler yükleniyor.");
        radios_data_refresh(ctx);
    }
}
void radios_data_recovery_retry(RadioSViewContext *ctx)
{
    if (!radios_data_connected(ctx) || ctx->data->recovering) return;
    ctx->data->recovering = TRUE;
    if (!radios_data_request(ctx, GTK_WIDGET(ctx->parent), "RECOVER", "", NULL, recovery_done, NULL, NULL)) ctx->data->recovering = FALSE;
}
static gboolean recovery_tick(gpointer value)
{
    RadioSData *data = value;
    if (data->closed) return G_SOURCE_REMOVE;
    if (!data->connected) { if (!data->connecting) radios_data_connect(data->ctx); return G_SOURCE_CONTINUE; }
    JsonNode *status = radios_data_recovery_status(data->ctx);
    if (status && radios_json_int(json_node_get_object(status), "pending", 0) > 0) radios_data_recovery_retry(data->ctx);
    g_clear_pointer(&status, json_node_free); return G_SOURCE_CONTINUE;
}
static void launch_done(GObject *source, GAsyncResult *result, gpointer value)
{
    RadioSData *data = value; GError *error = NULL; gchar *output = NULL, *diagnostic = NULL;
    gboolean ok = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, &diagnostic, &error);
    g_free(output); g_free(diagnostic);
    if (data->launch_timeout) { g_source_remove(data->launch_timeout); data->launch_timeout = 0; }
    if (!data->closed) {
        if (!ok || !g_subprocess_get_successful(G_SUBPROCESS(source))) {
            data->connecting = FALSE;
            notify_data(data, data->updating_service ?
                "Veri servisi eski sürümle çalışıyor ve güvenli biçimde güncellenemedi; servisi yeniden başlatın veya servis günlüğünü kontrol edin." :
                "Yerel veri servisi açılamadı; veritabanı/kimlik dosyası ve servis günlüğünü kontrol edin.");
        } else radios_data_request(data->ctx, GTK_WIDGET(data->ctx->parent), "CONNECT", "", NULL, connect_done, NULL, NULL);
    }
    g_clear_error(&error); g_clear_object(&data->launcher); g_clear_object(&data->launch_cancel); radios_data_unref(data);
}
static gboolean launch_timeout(gpointer value)
{
    RadioSData *data = value; data->launch_timeout = 0;
    if (data->launch_cancel) g_cancellable_cancel(data->launch_cancel);
    if (data->launcher) g_subprocess_force_exit(data->launcher);
    return G_SOURCE_REMOVE;
}
static void start_local_service(RadioSViewContext *ctx)
{
    RadioSData *data = ctx->data;
    gchar *script = g_build_filename(ctx->config->project_root, "tools", "radios_database.py", NULL);
    gchar *host_spawn = NULL;
    gboolean in_flatpak = g_file_test("/.flatpak-info", G_FILE_TEST_IS_REGULAR);
    if (in_flatpak) host_spawn = g_find_program_in_path("flatpak-spawn");
    if (in_flatpak && !host_spawn) {
        data->connecting = FALSE;
        notify_data(data, "Yerel veri servisini başlatmak için Flatpak ana sistem çalıştırıcısı bulunamadı.");
        g_free(script); return;
    }
    const gchar *args[11]; guint count = 0;
    /* The SDK frontend has its own /proc. Lifecycle ownership must be checked
     * beside the host service, rather than against an unrelated PID there. */
    if (host_spawn) { args[count++] = host_spawn; args[count++] = "--host"; }
    args[count++] = ctx->config->python_command; args[count++] = script;
    args[count++] = "--project-root"; args[count++] = ctx->config->project_root;
    args[count++] = "--config"; args[count++] = ctx->config->config_file;
    args[count++] = "start"; args[count] = NULL;
    GError *error = NULL;
    data->launcher = g_subprocess_newv(args, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error);
    g_free(host_spawn); g_free(script);
    if (!data->launcher) { data->connecting = FALSE; notify_data(data, "Veri servisi çalıştırıcısı açılamadı."); g_clear_error(&error); return; }
    data->launch_cancel = g_cancellable_new(); data->launch_timeout = g_timeout_add_seconds(15, launch_timeout, data);
    g_subprocess_communicate_utf8_async(data->launcher, NULL, data->launch_cancel, launch_done, data_ref(data));
}
static void probe_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result,
    const GError *error, gpointer unused)
{
    (void)unused;
    if (!error && result) { connect_done(ctx, owner, result, NULL, NULL); return; }
    if (error && error->domain == RADIOS_DATA_CLIENT_ERROR &&
            (error->code == RADIOS_DATA_CLIENT_ERROR_TRANSPORT ||
             error->code == RADIOS_DATA_CLIENT_ERROR_TIMEOUT ||
             error->code == RADIOS_DATA_CLIENT_ERROR_UNAVAILABLE ||
             error->code == RADIOS_DATA_CLIENT_ERROR_SCHEMA_MISMATCH)) {
        ctx->data->updating_service = error->code == RADIOS_DATA_CLIENT_ERROR_SCHEMA_MISMATCH;
        start_local_service(ctx); return;
    }
    ctx->data->connecting = FALSE;
    notify_data(ctx->data, error ? error->message : "Veri servisi bağlantısı doğrulanamadı.");
}
void radios_data_connect(RadioSViewContext *ctx)
{
    RadioSData *data = ctx->data;
    if (!data || data->closed || data->connecting) return;
    if (!data->recovery_timer) data->recovery_timer = g_timeout_add_seconds(5, recovery_tick, data);
    data->connecting = TRUE; data->connected = FALSE;
    /* A service started by run-radios.sh may live outside the GUI's PID
     * namespace. Authenticate its actual store/schema/context before asking
     * a lifecycle helper to start anything. Auth/identity failures stay fatal. */
    if (!request_dispatch(ctx, GTK_WIDGET(ctx->parent), "CONNECT", "", NULL,
            probe_done, NULL, NULL, FALSE)) data->connecting = FALSE;
}
JsonNode *radios_data_body(RadioSViewContext *ctx)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, json_object_new());
    json_object_set_string_member(json_node_get_object(node), "station_id", radios_data_station_id(ctx)); return node;
}
static void document_free(gpointer value)
{
    RadioSDocument *doc = value;
    if (doc->retry_timer) g_source_remove(doc->retry_timer);
    g_clear_pointer(&doc->rejected, json_node_free);
    g_free(doc->kind); g_free(doc->name); g_free(doc->id); g_free(doc->lookup_id);
    g_clear_pointer(&doc->pending, json_node_free); g_clear_pointer(&doc->retry_body, json_node_free);
    g_clear_pointer(&doc->read_client, radios_data_client_unref);
    g_clear_pointer(&doc->baseline_client, radios_data_client_unref); g_free(doc);
}
static void document_refresh(RadioSViewContext *ctx, GtkWidget *owner);
static void document_start_read(RadioSDocument *doc);
static void document_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    RadioSDocument *doc = value; (void)owner;
    doc->loading = FALSE;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { doc->explicit_reload = FALSE; goto finished; }
    /* A reconnect can change the store/station while this response is held.
     * Bind the form only to the client that actually read its CAS baseline. */
    RadioSDataClient *current = radios_data_client_acquire(ctx);
    gboolean same_identity = radios_data_client_same_identity(current, doc->read_client);
    g_clear_pointer(&current, radios_data_client_unref);
    if (!same_identity) {
        doc->explicit_reload = FALSE;
        doc->refresh_again = radios_data_connected(ctx);
        goto finished;
    }
    /* A read can finish after a newer acknowledged save. Its old id/revision
     * and form must never replace the saved document or preserved input. */
    if (doc->ready && (doc->dirty || doc->busy || doc->pending || doc->read_changes != doc->changes || doc->read_revision != doc->revision)) {
        doc->explicit_reload = FALSE; goto finished;
    }
    JsonArray *records = json_object_get_array_member(json_node_get_object(result), "records");
    gboolean same_baseline = doc->ready && radios_data_client_same_identity(doc->baseline_client, doc->read_client);
    doc->ready = TRUE;
    g_clear_pointer(&doc->baseline_client, radios_data_client_unref);
    if (doc->read_client) doc->baseline_client = radios_data_client_ref(doc->read_client);
    doc->autosave_conflict = FALSE;
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        JsonObject *record = json_array_get_object_element(records, i);
        if (doc->lookup_id ? g_str_equal(radios_json_string(record, "_id", ""), doc->lookup_id) :
            g_str_equal(radios_json_string(record, "name", ""), doc->name)) {
            const gchar *id = radios_json_string(record, "_id", "");
            gint64 revision = radios_json_int(record, "_rev", 0);
            /* Global refreshes also run after unrelated writes and reconnects.
             * Reapplying the same record rebuilds forms and can cancel live
             * controls even though no settings changed. Explicit reloads and
             * records from a different identity must still be applied. */
            gboolean changed = doc->explicit_reload || !same_baseline ||
                g_strcmp0(doc->id, id) || doc->revision != revision;
            g_free(doc->id); doc->id = g_strdup(id); doc->revision = revision;
            if (!doc->dirty && doc->apply && changed) {
                g_object_set_data(G_OBJECT(doc->owner), "radios-data-explicit-reload", GINT_TO_POINTER(doc->explicit_reload));
                doc->apply(ctx, doc->owner, json_object_get_object_member(record, "config"), doc->user);
                g_object_set_data(G_OBJECT(doc->owner), "radios-data-explicit-reload", NULL);
            }
            doc->explicit_reload = FALSE;
            goto finished;
        }
    }
    g_clear_pointer(&doc->id, g_free); doc->revision = 0;
    if (doc->lookup_id) doc->id = g_strdup(doc->lookup_id);
    doc->explicit_reload = FALSE;
finished:
    g_clear_pointer(&doc->read_client, radios_data_client_unref);
    if (doc->refresh_again) {
        doc->refresh_again = FALSE;
        document_start_read(doc);
    }
}
static void document_start_read(RadioSDocument *doc)
{
    if (doc->ready && (doc->busy || doc->pending || doc->dirty)) return;
    if (doc->loading) { doc->refresh_again = TRUE; return; }
    JsonNode *body = radios_data_body(doc->ctx); json_object_set_string_member(json_node_get_object(body), "kind", doc->kind);
    doc->read_changes = doc->changes; doc->read_revision = doc->revision;
    g_clear_pointer(&doc->read_client, radios_data_client_unref);
    if (doc->ctx->data && doc->ctx->data->client)
        doc->read_client = radios_data_client_ref(doc->ctx->data->client);
    doc->loading = radios_data_request(doc->ctx, doc->owner, "POST", "/v1/workflows/desktop/list", body, document_loaded, doc, NULL);
    json_node_free(body);
}
void radios_data_document_refresh(RadioSDocument *doc)
{
    if (doc && radios_data_connected(doc->ctx)) document_start_read(doc);
}
static void document_refresh(RadioSViewContext *ctx, GtkWidget *owner)
{
    (void)ctx;
    GPtrArray *documents = g_object_get_data(G_OBJECT(owner), "radios-data-documents");
    for (guint i = 0; documents && i < documents->len; ++i) {
        RadioSDocument *doc = g_ptr_array_index(documents, i);
        /* An explicit refresh during an older in-flight read is coalesced,
         * so an external commit is read once the old snapshot completes. */
        document_start_read(doc);
    }
}
static RadioSDocument *document_create(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *kind, const gchar *name, const gchar *identifier, gboolean fresh, RadioSDocumentApply apply, gpointer user)
{
    if (!ctx->data || (fresh && !radios_data_connected(ctx))) return NULL;
    RadioSDataClient *baseline = fresh ? radios_data_client_acquire(ctx) : NULL;
    if (fresh && !baseline) return NULL;
    GPtrArray *documents = g_object_get_data(G_OBJECT(owner), "radios-data-documents");
    if (!documents) {
        documents = g_ptr_array_new_with_free_func(document_free);
        g_object_set_data_full(G_OBJECT(owner), "radios-data-documents", documents, (GDestroyNotify)g_ptr_array_unref);
        radios_data_register_refresh(ctx, owner, document_refresh);
    }
    RadioSDocument *doc = g_new0(RadioSDocument, 1);
    doc->ctx = ctx; doc->owner = owner; doc->kind = g_strdup(kind); doc->name = g_strdup(name); doc->lookup_id = g_strdup(identifier); doc->apply = apply; doc->user = user;
    if (fresh) { doc->id = g_strdup(identifier); doc->ready = TRUE; doc->baseline_client = baseline; }
    g_ptr_array_add(documents, doc);
    if (!fresh && radios_data_connected(ctx)) document_start_read(doc);
    return doc;
}
RadioSDocument *radios_data_document_new(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *kind, const gchar *name, RadioSDocumentApply apply, gpointer user)
{ return document_create(ctx, owner, kind, name, NULL, FALSE, apply, user); }
RadioSDocument *radios_data_document_new_for_id(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *kind, const gchar *name, const gchar *identifier, RadioSDocumentApply apply, gpointer user)
{
    if (!identifier || !g_uuid_string_is_valid(identifier)) return NULL;
    return document_create(ctx, owner, kind, name, identifier, FALSE, apply, user);
}
RadioSDocument *radios_data_document_new_record(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *kind, const gchar *name, const gchar *identifier, RadioSDocumentApply apply, gpointer user)
{
    if (!identifier || !g_uuid_string_is_valid(identifier)) return NULL;
    /* A fresh, operator-named UUID is always CAS0. Server validation/collision
     * checks cannot overwrite an existing record, including equal names. */
    return document_create(ctx, owner, kind, name, identifier, TRUE, apply, user);
}
gboolean radios_data_document_ready(RadioSDocument *document) { return document && document->ready; }
void radios_data_document_set_status_callback(RadioSDocument *doc, RadioSDocumentStatus callback, gpointer user)
{ if (doc) { doc->status = callback; doc->status_user = user; } }
static void document_status(RadioSDocument *doc, const gchar *message)
{ if (doc->status) doc->status(doc, message, doc->status_user); }

const gchar *radios_data_document_id(RadioSDocument *document)
{ return document && document->id ? document->id : ""; }
gint64 radios_data_document_revision(RadioSDocument *document)
{ return document && document->ready && !document->busy && !document->dirty && !document->pending && !document->retry_body ? document->revision : 0; }
void radios_data_document_dirty(RadioSDocument *document, gboolean dirty)
{ if (document) { document->dirty = dirty; if (dirty) document->changes++; } }
static gboolean document_autosave_identity(RadioSDocument *doc)
{
    RadioSData *data = doc->ctx ? doc->ctx->data : NULL;
    RadioSDataClient *current = data && !data->closed ? data->client : NULL;
    RadioSDataClient *baseline = doc->baseline_client;
    return radios_data_client_same_identity(current, baseline);
}
gboolean radios_data_document_preserve_pending(RadioSDocument *doc)
{
    if (!doc || !doc->retry_body) return TRUE;
    if (doc->automatic && !document_autosave_identity(doc)) return FALSE;
    if (!radios_data_preserve(doc->ctx, "/v1/workflows/desktop/put", doc->retry_body)) return FALSE;
    if (!doc->pending) return TRUE;
    JsonObject *previous = json_node_get_object(doc->retry_body);
    const gchar *id = radios_json_string(previous, "document_id", "");
    /* New documents also have a client UUID and expected revision zero. */
    if (!*id) return FALSE;
    JsonNode *next = copy_json(doc->retry_body); JsonObject *body = json_node_get_object(next);
    json_object_set_member(body, "config", copy_json(doc->pending));
    json_object_set_int_member(body, "expected_revision", radios_json_int(previous, "expected_revision", 0) + 1);
    gchar *key = g_uuid_string_random(); json_object_set_string_member(body, "idempotency_key", key); g_free(key);
    gboolean ok = radios_data_preserve(doc->ctx, "/v1/workflows/desktop/put", next); json_node_free(next); return ok;
}
static void document_reload_response(GtkDialog *dialog, gint response, gpointer value)
{
    RadioSDocument *doc = value;
    if (response == GTK_RESPONSE_ACCEPT && !doc->busy) {
        doc->dirty = FALSE; doc->explicit_reload = TRUE; doc->autosave_conflict = FALSE; g_clear_pointer(&doc->pending, json_node_free);
        if (doc->retry_timer) { g_source_remove(doc->retry_timer); doc->retry_timer = 0; }
        g_clear_pointer(&doc->rejected, json_node_free);
        g_clear_pointer(&doc->retry_body, json_node_free);
        document_refresh(doc->ctx, doc->owner);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
static void document_offer_reload(RadioSDocument *doc, gboolean uncertain)
{
    GtkWidget *dialog = gtk_message_dialog_new(doc->ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING,
        GTK_BUTTONS_NONE, "%s", uncertain ? "Kayıt sonucu doğrulanamadı." : "Kayıt başka oturumda değişti.");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog),
        "%s Girdileriniz korunuyor. Veritabanından yeniden yükleme bu formdaki kaydedilmemiş değişiklikleri siler.",
        uncertain ? "Aynı içeriği tekrar kaydetmek aynı işlem kimliğini kullanır." : "Devam etmeden önce güncel kayıt yüklenmeli.");
    gtk_widget_set_name(dialog, "database-reload-dialog");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Girdileri koru", GTK_RESPONSE_CANCEL,
                          "Veritabanından yeniden yükle", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    /* The document is owned by this widget; destroy the prompt before it is freed. */
    g_signal_connect_object(doc->owner, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    g_signal_connect(dialog, "response", G_CALLBACK(document_reload_response), doc);
    gtk_widget_show_all(dialog);
}
void radios_data_document_reload(RadioSDocument *doc)
{
    if (!doc || doc->busy || doc->loading) return;
    if (!doc->dirty && !doc->pending && !doc->retry_body) {
        doc->explicit_reload = TRUE; document_refresh(doc->ctx, doc->owner); return;
    }
    GtkWidget *dialog = gtk_message_dialog_new(doc->ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, GTK_MESSAGE_QUESTION,
        GTK_BUTTONS_NONE, "%s", "Kaydedilmemiş ayarları veritabanından yeniden yükle?");
    gtk_message_dialog_format_secondary_text(GTK_MESSAGE_DIALOG(dialog), "%s",
        "Yeniden yükleme bu formun yerel değişikliklerini siler. Diğer formlar korunur.");
    gtk_widget_set_name(dialog, "database-reload-dialog");
    gtk_dialog_add_buttons(GTK_DIALOG(dialog), "Girdileri koru", GTK_RESPONSE_CANCEL,
        "Veritabanından yeniden yükle", GTK_RESPONSE_ACCEPT, NULL);
    gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    g_signal_connect_object(doc->owner, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    g_signal_connect(dialog, "response", G_CALLBACK(document_reload_response), doc);
    gtk_widget_show_all(dialog);
}
static void document_saved(RadioSViewContext *, GtkWidget *, JsonNode *, const GError *, gpointer);
static void document_schedule_retry(RadioSDocument *doc);
static gboolean document_retry_tick(gpointer value)
{
    RadioSDocument *doc = value; doc->retry_timer = 0;
    if (!doc->retry_body || doc->busy || doc->autosave_conflict) return G_SOURCE_REMOVE;
    if (doc->automatic && !document_autosave_identity(doc)) {
        document_status(doc, "Kaydedilemedi: Veri kimliği değişti; değişiklikler korunuyor"); return G_SOURCE_REMOVE;
    }
    if (!radios_data_connected(doc->ctx)) {
        document_status(doc, "Bağlantı bekleniyor"); document_schedule_retry(doc); return G_SOURCE_REMOVE;
    }
    doc->busy = radios_data_request(doc->ctx, doc->owner, "POST", "/v1/workflows/desktop/put",
                                    doc->retry_body, document_saved, doc, NULL);
    if (doc->busy) document_status(doc, "Kaydediliyor…");
    return G_SOURCE_REMOVE;
}
static void document_schedule_retry(RadioSDocument *doc)
{
    if (doc->retry_timer || !doc->ctx || !doc->ctx->data || doc->ctx->data->closed) return;
    guint delay = MAX(radios_data_client_retry_delay(doc->ctx->data->client), 1u << MIN(doc->retry_attempt++, 5u));
    doc->retry_timer = g_timeout_add_seconds(delay, document_retry_tick, doc);
}
static void document_send_pending(RadioSDocument *doc)
{
    if (!doc->pending) return;
    JsonNode *pending = doc->pending; doc->pending = NULL;
    if (doc->automatic) radios_data_document_autosave(doc, pending);
    else radios_data_document_save(doc, pending);
    json_node_free(pending);
}
static void document_saved(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    RadioSDocument *doc = value; (void)owner; doc->busy = FALSE;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        gchar *message = g_strdup_printf("Kaydedilemedi: %s", error ? error->message : "Geçersiz servis yanıtı");
        document_status(doc, message); g_free(message);
        if (error && error->domain == RADIOS_DATA_CLIENT_ERROR &&
            (error->code == RADIOS_DATA_CLIENT_ERROR_CONFLICT || error->code == RADIOS_DATA_CLIENT_ERROR_NOT_FOUND)) {
            if (doc->automatic) doc->autosave_conflict = TRUE;
            g_clear_pointer(&doc->retry_body, json_node_free); document_offer_reload(doc, FALSE);
        } else if (error && error->domain == RADIOS_DATA_CLIENT_ERROR &&
            (error->code == RADIOS_DATA_CLIENT_ERROR_VALIDATION || error->code == RADIOS_DATA_CLIENT_ERROR_FORBIDDEN)) {
            g_clear_pointer(&doc->rejected, json_node_free);
            if (doc->retry_body) doc->rejected = copy_json(json_object_get_member(json_node_get_object(doc->retry_body), "config"));
            g_clear_pointer(&doc->retry_body, json_node_free);
            /* A newer edit may already correct the rejected field. Its CAS
             * baseline remains unchanged because the first write did not commit. */
            document_send_pending(doc);
        } else if (doc->retry_body && (doc->automatic ||
                   g_error_matches(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED))) {
            document_schedule_retry(doc);
        }
        return;
    }
    if (doc->retry_timer) { g_source_remove(doc->retry_timer); doc->retry_timer = 0; }
    doc->retry_attempt = 0; g_clear_pointer(&doc->rejected, json_node_free);
    JsonObject *record = json_node_get_object(result);
    if (json_object_has_member(record, "record")) record = json_object_get_object_member(record, "record");
    g_free(doc->id); doc->id = g_strdup(radios_json_string(record, "_id", "")); doc->revision = radios_json_int(record, "_rev", 0);
    notify_data(ctx->data, "Değişiklik veritabanına kaydedildi.");
    g_clear_pointer(&doc->retry_body, json_node_free);
    doc->dirty = doc->pending != NULL || doc->changes != doc->submitted_changes;
    /* The original write still committed, but its ACK cannot apply a form
     * under a newly connected identity (or bind it to an offline NULL client).
     * Pending autosaves retain their existing baseline identity check. */
    RadioSDataClient *current = radios_data_client_acquire(ctx);
    gboolean apply_identity = radios_data_client_same_identity(current, doc->baseline_client);
    g_clear_pointer(&current, radios_data_client_unref);
    if (!doc->dirty && apply_identity && doc->apply && json_object_has_member(record, "config"))
        doc->apply(ctx, doc->owner, json_object_get_object_member(record, "config"), doc->user);
    document_status(doc, doc->dirty ? "Kaydediliyor…" : "Kaydedildi");
    document_send_pending(doc);
}
gboolean radios_data_document_save(RadioSDocument *doc, JsonNode *config)
{
    if (!doc || !doc->ready || !radios_data_connected(doc->ctx)) {
        if (doc) notify_data(doc->ctx->data, "Kayıt henüz yüklenmedi; değişiklik korunuyor.");
        return FALSE;
    }
    doc->dirty = TRUE;
    if (doc->rejected && json_node_equal(doc->rejected, config)) return FALSE;
    g_clear_pointer(&doc->rejected, json_node_free);
    if (doc->busy || doc->retry_timer) {
        JsonNode *first = doc->retry_body ? json_object_get_member(json_node_get_object(doc->retry_body), "config") : NULL;
        if (!first || !json_node_equal(first, config)) {
            g_clear_pointer(&doc->pending, json_node_free); doc->pending = copy_json(config);
        }
        return TRUE;
    }
    g_clear_pointer(&doc->pending, json_node_free);
    if (doc->retry_body) {
        JsonNode *previous = json_object_get_member(json_node_get_object(doc->retry_body), "config");
        if (!json_node_equal(previous, config)) {
            document_offer_reload(doc, TRUE); return FALSE;
        }
        doc->submitted_changes = doc->changes;
        doc->busy = radios_data_request(doc->ctx, doc->owner, "POST", "/v1/workflows/desktop/put",
                                        doc->retry_body, document_saved, doc, NULL);
        return doc->busy;
    }
    JsonNode *body = radios_data_body(doc->ctx); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", doc->kind); json_object_set_string_member(object, "name", doc->name);
    json_object_set_member(object, "config", json_node_copy(config)); gchar *key = g_uuid_string_random();
    json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    if (!doc->id || !*doc->id) { g_free(doc->id); doc->id = g_uuid_string_random(); }
    json_object_set_string_member(object, "document_id", doc->id); json_object_set_int_member(object, "expected_revision", doc->revision);
    doc->retry_body = copy_json(body); doc->submitted_changes = doc->changes;
    doc->busy = radios_data_request(doc->ctx, doc->owner, "POST", "/v1/workflows/desktop/put", body, document_saved, doc, NULL);
    json_node_free(body); if (doc->busy) document_status(doc, "Kaydediliyor…"); return doc->busy;
}

/* Automatic forms retain one immutable uncertain intent and one latest local
 * successor. Manual saves keep their existing explicit reconciliation flow. */
static void document_autosave_latest(RadioSDocument *doc, JsonNode *config)
{
    JsonNode *latest = copy_json(config);
    g_clear_pointer(&doc->pending, json_node_free); doc->pending = latest;
}
gboolean radios_data_document_autosave(RadioSDocument *doc, JsonNode *config)
{
    if (!doc || !doc->ready || !config || !JSON_NODE_HOLDS_OBJECT(config)) return FALSE;
    /* A retry timer must not restore the discarded draft while an explicitly
     * accepted reload is still fetching its replacement baseline. */
    if (doc->loading && doc->explicit_reload) return FALSE;
    doc->automatic = TRUE; doc->dirty = TRUE;
    if (doc->rejected && json_node_equal(doc->rejected, config)) return FALSE;
    g_clear_pointer(&doc->rejected, json_node_free);
    if (!document_autosave_identity(doc)) {
        document_autosave_latest(doc, config);
        if (doc->ctx && doc->ctx->data)
            notify_data(doc->ctx->data, "Otomatik kayıt veri kimliği değişti; yerel değişiklik korunuyor.");
        return FALSE;
    }
    if (doc->autosave_conflict) {
        document_autosave_latest(doc, config); return FALSE;
    }
    if (doc->retry_body) {
        JsonNode *previous = json_object_get_member(json_node_get_object(doc->retry_body), "config");
        if (json_node_equal(previous, config)) {
            /* A retry of the first intent must retain an already queued newer
             * successor and the first intent's change counter. */
            if (!doc->pending) doc->submitted_changes = doc->changes;
        } else document_autosave_latest(doc, config);
    } else {
        JsonNode *body = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new();
        json_node_take_object(body, object);
        json_object_set_string_member(object, "station_id", radios_data_client_station_id(doc->baseline_client));
        json_object_set_string_member(object, "kind", doc->kind); json_object_set_string_member(object, "name", doc->name);
        json_object_set_member(object, "config", copy_json(config));
        gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
        if (!doc->id || !*doc->id) { g_free(doc->id); doc->id = g_uuid_string_random(); }
        json_object_set_string_member(object, "document_id", doc->id);
        json_object_set_int_member(object, "expected_revision", doc->revision);
        doc->retry_body = body; doc->submitted_changes = doc->changes;
        g_clear_pointer(&doc->pending, json_node_free);
    }
    /* The exact first body remains durable even while the transport is down.
     * A successor is checkpointed only once by preserve_pending at close. */
    if (!radios_data_preserve(doc->ctx, "/v1/workflows/desktop/put", doc->retry_body)) return FALSE;
    if (!radios_data_connected(doc->ctx)) return FALSE;
    if (doc->busy || doc->retry_timer) return TRUE;
    doc->busy = radios_data_request(doc->ctx, doc->owner, "POST", "/v1/workflows/desktop/put",
                                    doc->retry_body, document_saved, doc, NULL);
    if (doc->busy) document_status(doc, "Kaydediliyor…");
    return doc->busy;
}
