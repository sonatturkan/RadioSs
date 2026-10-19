#include "radios/automation_scope_ui.h"
#include "radios/programme_model.h"

typedef struct {
    RadioSViewContext *ctx;
    RadioSDataClient *client;
    gchar document_id[37];
    gint64 revision;
    JsonNode *record;
    RadioSDataCallback callback;
    gpointer user;
    GDestroyNotify destroy;
    guint references;
} ScopeFetch;

static ScopeFetch *fetch_ref(ScopeFetch *job) { ++job->references; return job; }
static void fetch_free(gpointer value)
{
    ScopeFetch *job = value;
    if (--job->references) return;
    if (job->destroy) job->destroy(job->user);
    radios_data_client_unref(job->client);
    g_clear_pointer(&job->record, json_node_free); g_free(job);
}
static gboolean fetch_client_current(ScopeFetch *job)
{
    RadioSDataClient *current = radios_data_client_acquire(job->ctx);
    gboolean same = current == job->client && radios_data_connected(job->ctx);
    if (current) radios_data_client_unref(current);
    return same;
}
static JsonNode *fetch_body(ScopeFetch *job)
{
    JsonNode *body = json_node_new(JSON_NODE_OBJECT); json_node_take_object(body, json_object_new());
    JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "station_id", radios_data_client_station_id(job->client));
    json_object_set_string_member(object, "kind", "weekly_programme");
    json_object_set_string_member(object, "document_id", job->document_id);
    return body;
}
static void fetch_received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    ScopeFetch *job = value; GError *failure = error ? g_error_copy(error) : NULL;
    JsonObject *record = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    if (!failure && !fetch_client_current(job))
        failure = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "Otomasyon hazırlanırken veri oturumu değişti.");
    if (!failure) radios_automation_scope_record_valid(record,
        radios_data_client_station_id(job->client), job->document_id, job->revision, &failure);
    if (!failure && job->record && !json_node_equal(json_object_get_member(record, "config"),
            json_object_get_member(json_node_get_object(job->record), "config")))
        failure = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Otomasyon şeması hazırlanırken değişti.");
    if (!failure && !job->record) {
        job->revision = radios_json_int(record, "_rev", 0);
        job->record = json_node_new(JSON_NODE_OBJECT);
        json_node_take_object(job->record, radios_programme_clone_object(record));
        JsonNode *body = fetch_body(job);
        gboolean accepted = radios_data_request(ctx, owner, "POST", "/v1/workflows/desktop/get", body,
            fetch_received, fetch_ref(job), fetch_free);
        json_node_free(body);
        if (accepted) return;
        failure = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "Otomasyon şeması yeniden doğrulanamadı.");
    }
    if (job->callback) job->callback(ctx, owner, failure ? NULL : result, failure, job->user);
    g_clear_error(&failure);
}
gboolean radios_automation_scope_fetch(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *document_id, gint64 expected_revision,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{
    if (!ctx || !owner || gtk_widget_in_destruction(owner) || !document_id ||
        !g_uuid_string_is_valid(document_id) || expected_revision < 0 || !radios_data_connected(ctx)) {
        if (destroy) destroy(user);
        return FALSE;
    }
    RadioSDataClient *client = radios_data_client_acquire(ctx);
    if (!client) { if (destroy) destroy(user); return FALSE; }
    ScopeFetch *job = g_new0(ScopeFetch, 1); job->references = 1; job->ctx = ctx; job->client = client;
    job->revision = expected_revision; job->callback = callback; job->user = user; job->destroy = destroy;
    g_strlcpy(job->document_id, document_id, sizeof job->document_id);
    JsonNode *body = fetch_body(job);
    gboolean accepted = radios_data_request(ctx, owner, "POST", "/v1/workflows/desktop/get", body,
        fetch_received, fetch_ref(job), fetch_free);
    json_node_free(body); fetch_free(job); return accepted;
}
