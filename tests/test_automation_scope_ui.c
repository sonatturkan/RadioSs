#include "radios/automation_scope_ui.h"
#include "radios/weekly_programme_plan.h"

static const gchar *const station_id = "10000000-0000-4000-8000-000000000001";
static const gchar *const schema_id = "20000000-0000-4000-8000-000000000001";
static guint client_token[2], current_client;
static gboolean connected = TRUE, reject_request;
static guint requests;
typedef struct { guint callbacks, destroyed; gboolean success; gint error_code; } Result;
typedef struct {
    RadioSViewContext *ctx;
    GWeakRef owner;
    RadioSDataCallback callback;
    gpointer user;
    GDestroyNotify destroy;
} Pending;
static GQueue pending;

gboolean __wrap_radios_data_connected(RadioSViewContext *ctx)
{ (void)ctx; return connected; }
RadioSDataClient *__wrap_radios_data_client_acquire(RadioSViewContext *ctx)
{ (void)ctx; return connected ? (RadioSDataClient *)&client_token[current_client] : NULL; }
void __wrap_radios_data_client_unref(RadioSDataClient *client)
{ g_assert_true(client == (RadioSDataClient *)&client_token[0] || client == (RadioSDataClient *)&client_token[1]); }
const gchar *__wrap_radios_data_client_station_id(const RadioSDataClient *client)
{ (void)client; return station_id; }
gint64 __wrap_radios_json_int(JsonObject *object, const gchar *key, gint64 fallback)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64 ? json_node_get_int(node) : fallback;
}
gboolean __wrap_radios_data_request(RadioSViewContext *ctx, GtkWidget *owner, const gchar *method,
    const gchar *path, JsonNode *body, RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{
    ++requests; g_assert_cmpstr(method, ==, "POST"); g_assert_cmpstr(path, ==, "/v1/workflows/desktop/get");
    JsonObject *request = json_node_get_object(body);
    g_assert_cmpstr(json_object_get_string_member(request, "station_id"), ==, station_id);
    g_assert_cmpstr(json_object_get_string_member(request, "kind"), ==, "weekly_programme");
    g_assert_cmpstr(json_object_get_string_member(request, "document_id"), ==, schema_id);
    if (reject_request) { if (destroy) destroy(user); return FALSE; }
    Pending *job = g_new0(Pending, 1); job->ctx = ctx; g_weak_ref_init(&job->owner, owner);
    job->callback = callback; job->user = user; job->destroy = destroy; g_queue_push_tail(&pending, job); return TRUE;
}
static void deliver(JsonNode *record)
{
    Pending *job = g_queue_pop_head(&pending); g_assert_nonnull(job);
    GtkWidget *owner = g_weak_ref_get(&job->owner);
    if (owner && !g_object_get_data(G_OBJECT(owner), "test-destroyed")) job->callback(job->ctx, owner, record, NULL, job->user);
    if (job->destroy) job->destroy(job->user);
    g_clear_object(&owner); g_weak_ref_clear(&job->owner); g_free(job);
}
static void received(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *record, const GError *error, gpointer user)
{
    (void)ctx; g_assert_nonnull(owner); Result *result = user; ++result->callbacks;
    result->success = record != NULL && error == NULL; result->error_code = error ? error->code : -1;
    if (error) g_assert_null(record);
}
static void destroyed(gpointer user) { ++((Result *)user)->destroyed; }
static void owner_destroyed(GtkWidget *owner, gpointer user)
{ (void)user; g_object_set_data(G_OBJECT(owner), "test-destroyed", GINT_TO_POINTER(1)); }
static GtkWidget *owner_new(void)
{
    GtkWidget *owner = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(owner);
    g_signal_connect(owner, "destroy", G_CALLBACK(owner_destroyed), NULL);
    connected = TRUE; reject_request = FALSE; current_client = 0; requests = 0; g_assert_true(g_queue_is_empty(&pending)); return owner;
}
static JsonNode *record(void)
{
    JsonObject *calendar = json_object_new(); json_object_set_int_member(calendar, "schema", 1);
    json_object_set_array_member(calendar, "events", json_array_new()); GError *error = NULL;
    JsonNode *config = radios_weekly_programme_snapshot(calendar, "2026-10-12", TRUE, &error);
    g_assert_no_error(error); g_assert_nonnull(config); json_object_unref(calendar);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *result = json_object_new(); json_node_take_object(node, result);
    json_object_set_string_member(result, "_id", schema_id); json_object_set_int_member(result, "_rev", 4);
    json_object_set_string_member(result, "station_id", station_id); json_object_set_string_member(result, "kind", "weekly_programme");
    json_object_set_string_member(result, "status", "active"); json_object_set_string_member(result, "name", "Şema");
    json_object_set_member(result, "config", config); return node;
}
static void test_exact_double_read(void)
{
    RadioSViewContext ctx = {0}; GtkWidget *owner = owner_new(); Result result = {0}; JsonNode *saved = record();
    g_assert_true(radios_automation_scope_fetch(&ctx, owner, schema_id, 0, received, &result, destroyed));
    g_assert_cmpuint(requests, ==, 1); deliver(saved); g_assert_cmpuint(requests, ==, 2);
    g_assert_cmpuint(result.callbacks, ==, 0); g_assert_cmpuint(result.destroyed, ==, 0);
    deliver(saved); g_assert_cmpuint(result.callbacks, ==, 1); g_assert_true(result.success);
    g_assert_cmpuint(result.destroyed, ==, 1); g_assert_true(g_queue_is_empty(&pending));
    json_node_free(saved); gtk_widget_destroy(owner); g_object_unref(owner);
}
static void test_revision_and_config_changes(void)
{
    RadioSViewContext ctx = {0};
    for (guint variant = 0; variant < 3; ++variant) {
        GtkWidget *owner = owner_new(); Result result = {0}; JsonNode *saved = record();
        g_assert_true(radios_automation_scope_fetch(&ctx, owner, schema_id, variant == 2 ? 3 : 0, received, &result, destroyed));
        deliver(saved);
        if (variant == 0) { json_object_set_int_member(json_node_get_object(saved), "_rev", 5); deliver(saved); }
        if (variant == 1) {
            json_object_set_boolean_member(json_object_get_object_member(json_node_get_object(saved), "config"), "repeat", FALSE);
            deliver(saved);
        }
        g_assert_cmpuint(result.callbacks, ==, 1); g_assert_false(result.success);
        g_assert_cmpint(result.error_code, ==, G_IO_ERROR_INVALID_DATA); g_assert_cmpuint(result.destroyed, ==, 1);
        g_assert_cmpuint(requests, ==, variant == 2 ? 1 : 2);
        json_node_free(saved); gtk_widget_destroy(owner); g_object_unref(owner);
    }
}
static void test_session_and_owner_fences(void)
{
    RadioSViewContext ctx = {0}; GtkWidget *owner = owner_new(); Result result = {0}; JsonNode *saved = record();
    g_assert_true(radios_automation_scope_fetch(&ctx, owner, schema_id, 0, received, &result, destroyed));
    current_client = 1; deliver(saved); g_assert_cmpuint(requests, ==, 1);
    g_assert_cmpuint(result.callbacks, ==, 1); g_assert_false(result.success); g_assert_cmpint(result.error_code, ==, G_IO_ERROR_CANCELLED);
    g_assert_cmpuint(result.destroyed, ==, 1); gtk_widget_destroy(owner); g_object_unref(owner);
    owner = owner_new(); result = (Result){0};
    g_assert_true(radios_automation_scope_fetch(&ctx, owner, schema_id, 0, received, &result, destroyed));
    deliver(saved); gtk_widget_destroy(owner); deliver(saved);
    g_assert_cmpuint(result.callbacks, ==, 0); g_assert_cmpuint(result.destroyed, ==, 1);
    g_object_unref(owner); json_node_free(saved);
}
static void test_rejections_destroy_once(void)
{
    RadioSViewContext ctx = {0}; GtkWidget *owner = owner_new(); Result result = {0};
    g_assert_false(radios_automation_scope_fetch(&ctx, owner, "invalid-id", 0, received, &result, destroyed));
    g_assert_cmpuint(requests, ==, 0); g_assert_cmpuint(result.destroyed, ==, 1);
    result = (Result){0}; reject_request = TRUE;
    g_assert_false(radios_automation_scope_fetch(&ctx, owner, schema_id, 0, received, &result, destroyed));
    g_assert_cmpuint(result.callbacks, ==, 0); g_assert_cmpuint(result.destroyed, ==, 1);
    result = (Result){0}; reject_request = FALSE; JsonNode *saved = record();
    g_assert_true(radios_automation_scope_fetch(&ctx, owner, schema_id, 0, received, &result, destroyed));
    reject_request = TRUE; deliver(saved); g_assert_cmpuint(result.callbacks, ==, 1);
    g_assert_false(result.success); g_assert_cmpuint(result.destroyed, ==, 1); g_assert_true(g_queue_is_empty(&pending));
    json_node_free(saved); gtk_widget_destroy(owner); g_object_unref(owner);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); if (!gtk_init_check(&argc, &argv)) return 77;
    g_test_add_func("/automation-scope-fetch/exact-double-read", test_exact_double_read);
    g_test_add_func("/automation-scope-fetch/revision-config-fences", test_revision_and_config_changes);
    g_test_add_func("/automation-scope-fetch/session-owner-fences", test_session_and_owner_fences);
    g_test_add_func("/automation-scope-fetch/destroy-once", test_rejections_destroy_once);
    return g_test_run();
}
