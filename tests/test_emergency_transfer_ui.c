#include "radios/programme_model.h"
#include "radios/data.h"
#include "radios/playout_ui.h"
#include <glib/gstdio.h>
#include <string.h>

static RadioSConfig config;
static RadioSDataClient *reader;
typedef struct { RadioSViewContext ctx; GtkWidget *window, *page; } Harness;
typedef gboolean (*Condition)(gpointer);
static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *dialog(void)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *result = NULL;
    for (GList *item = windows; item && !result; item = item->next)
        if (g_str_equal(gtk_widget_get_name(item->data), "em-programme-transfer-dialog")) result = item->data;
    g_list_free(windows); return result;
}
static void pump(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void run_for(guint milliseconds)
{ gint64 end = g_get_monotonic_time() + milliseconds * 1000; while (g_get_monotonic_time() < end) pump(); }
static void wait_for(Condition condition, gpointer data)
{
    gint64 end = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!condition(data) && g_get_monotonic_time() < end) pump();
    g_assert_true(condition(data));
}
static RadioSDocument *source(Harness *h)
{
    GPtrArray *documents = g_object_get_data(G_OBJECT(h->page), "radios-data-documents");
    return documents && documents->len ? g_ptr_array_index(documents, 0) : NULL;
}
static gboolean ready(gpointer value)
{
    Harness *h = value;
    return radios_data_connected(&h->ctx) && radios_data_document_revision(source(h)) > 0 &&
        radios_data_document_revision(radios_broadcast_programme_document(&h->ctx)) > 0;
}
static Harness *open_harness(void)
{
    Harness *h = g_new0(Harness, 1); h->ctx.config = &config; h->ctx.runtime = radios_runtime_new(64, 128);
    h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); h->ctx.parent = GTK_WINDOW(h->window);
    h->ctx.data = radios_data_new(&h->ctx);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(box), radios_broadcast_new(&h->ctx));
    h->page = radios_emergency_new(&h->ctx); gtk_container_add(GTK_CONTAINER(box), h->page);
    gtk_container_add(GTK_CONTAINER(h->window), box); gtk_widget_show_all(h->window);
    radios_data_connect(&h->ctx); wait_for(ready, h);
    return h;
}
static void close_harness(Harness *h)
{
    gtk_widget_destroy(h->window); radios_data_close(h->ctx.data);
    radios_runtime_free(h->ctx.runtime); radios_data_unref(h->ctx.data);
    run_for(30); g_free(h);
}
typedef struct { const gchar *path; JsonNode *body, *result; GError *error; gint done; } Request;
static gpointer request_worker(gpointer value)
{
    Request *request = value;
    request->result = radios_data_client_request_sync(reader, "POST", request->path, request->body, NULL, &request->error);
    g_atomic_int_set(&request->done, 1); return NULL;
}
static gboolean request_done(gpointer value) { return g_atomic_int_get(&((Request *)value)->done); }
static JsonNode *request(const gchar *path, JsonNode *body)
{
    Request work = {.path = path, .body = body}; GThread *thread = g_thread_new("emergency-transfer-read", request_worker, &work);
    wait_for(request_done, &work); g_thread_join(thread); g_assert_no_error(work.error); g_assert_nonnull(work.result);
    return work.result;
}
static JsonNode *body(void)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, json_object_new());
    json_object_set_string_member(json_node_get_object(node), "station_id", radios_data_client_station_id(reader)); return node;
}
static JsonNode *deep_copy(JsonNode *node)
{
    gchar *encoded = json_to_string(node, FALSE);
    JsonNode *result = json_from_string(encoded, NULL);
    g_free(encoded); g_assert_nonnull(result); return result;
}
static JsonNode *record(const gchar *kind)
{
    JsonNode *payload = body(); json_object_set_string_member(json_node_get_object(payload), "kind", kind);
    JsonNode *result = request("/v1/workflows/desktop/list", payload); json_node_free(payload);
    JsonArray *records = json_object_get_array_member(json_node_get_object(result), "records");
    g_assert_cmpuint(json_array_get_length(records), ==, 1);
    JsonNode *row = json_node_copy(json_array_get_element(records, 0)); json_node_free(result); return row;
}
static void put_record(JsonNode *row)
{
    JsonObject *object = json_node_get_object(row); JsonNode *payload = body(); JsonObject *data = json_node_get_object(payload);
    json_object_set_string_member(data, "kind", radios_json_string(object, "kind", ""));
    json_object_set_string_member(data, "name", radios_json_string(object, "name", ""));
    json_object_set_string_member(data, "document_id", radios_json_string(object, "_id", ""));
    json_object_set_int_member(data, "expected_revision", radios_json_int(object, "_rev", 0));
    gchar *key = g_uuid_string_random(); json_object_set_string_member(data, "idempotency_key", key); g_free(key);
    json_object_set_member(data, "config", json_node_copy(json_object_get_member(object, "config")));
    JsonNode *result = request("/v1/workflows/desktop/put", payload); json_node_free(payload); json_node_free(result);
}
static guint events(JsonNode *row)
{
    JsonObject *configuration = json_object_get_object_member(json_node_get_object(row), "config");
    return json_array_get_length(json_object_get_array_member(configuration, "events"));
}
static GtkWidget *begin(Harness *h)
{
    GtkWidget *button = find(h->page, "em-queue-export"); g_assert_true(GTK_IS_BUTTON(button));
    gtk_button_clicked(GTK_BUTTON(button)); GtkWidget *transfer = dialog(); g_assert_nonnull(transfer);
    gtk_dialog_response(GTK_DIALOG(transfer), GTK_RESPONSE_ACCEPT); return transfer;
}
static gboolean accepted(gpointer value) { return gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(value), GTK_RESPONSE_ACCEPT)); }
static gboolean no_dialog(gpointer unused) { (void)unused; return !dialog(); }
typedef struct { Harness *harness; gint64 revision; } SourceRevision;
static gboolean source_revised(gpointer value)
{
    SourceRevision *expected = value;
    return radios_data_document_revision(source(expected->harness)) == expected->revision;
}
static void flag(const gchar *name, gboolean enabled)
{
    gchar *parent = g_path_get_dirname(config.database_file), *path = g_build_filename(parent, name, NULL); g_free(parent);
    if (enabled) g_assert_true(g_file_set_contents(path, "ready", -1, NULL));
    else if (g_file_test(path, G_FILE_TEST_EXISTS)) g_assert_cmpint(g_unlink(path), ==, 0);
    g_free(path);
}
static void wait_held(void)
{
    gchar *parent = g_path_get_dirname(config.database_file), *path = g_build_filename(parent, ".transfer-ready", NULL); g_free(parent);
    gint64 end = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (!g_file_test(path, G_FILE_TEST_EXISTS) && g_get_monotonic_time() < end) {
        GtkWidget *transfer = dialog();
        if (transfer && accepted(transfer)) {
            g_test_message("Prepare completed before test barrier: %s", gtk_label_get_text(GTK_LABEL(find(transfer, "em-programme-transfer-status"))));
            break;
        }
        pump();
    }
    g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS)); g_free(path);
}
static void test_append_save_reopen(void)
{
    Harness *h = open_harness(); JsonNode *before = record("programme"), *original_source = record("emergency");
    guint count = events(before); begin(h); wait_for(no_dialog, NULL); wait_for(ready, h);
    JsonNode *after = record("programme"); g_assert_cmpuint(events(after), ==, count + 2);
    JsonArray *entries = json_object_get_array_member(json_object_get_object_member(json_node_get_object(after), "config"), "events");
    JsonArray *existing = json_object_get_array_member(json_object_get_object_member(json_node_get_object(before), "config"), "events");
    /* Existing events keep every field; saving adds the block_id of their flow event. */
    for (guint i = 0; i < count; ++i) {
        JsonNode *was = radios_programme_clone(json_array_get_element(existing, i)), *is = radios_programme_clone(json_array_get_element(entries, i));
        json_object_remove_member(json_node_get_object(was), "block_id"); json_object_remove_member(json_node_get_object(is), "block_id");
        g_assert_true(json_node_equal(was, is)); json_node_free(was); json_node_free(is);
    }
    for (guint i = 0; i < 2; ++i) {
        JsonObject *entry = json_array_get_object_element(entries, count + i);
        g_assert_cmpstr(radios_json_string(entry, "title", ""), ==, i ? "Temporary B" : "Temporary A");
        g_assert_cmpint(radios_json_int(entry, "duration_ms", 0), ==, 200);
        g_assert_true(g_uuid_string_is_valid(radios_json_string(entry, "media_file_id", "")));
        g_assert_cmpuint(strlen(radios_json_string(entry, "media_sha256", "")), ==, 64);
    }
    JsonNode *source_after = record("emergency"); g_assert_true(json_node_equal(original_source, source_after));
    g_assert_null(h->ctx.audio); g_assert_null(h->ctx.playout); close_harness(h); h = open_harness();
    RadioSDocument *target = radios_broadcast_programme_document(&h->ctx);
    g_assert_cmpint(radios_data_document_revision(target), ==, radios_json_int(json_node_get_object(after), "_rev", 0));
    JsonNode *reopened = record("programme"); g_assert_true(json_node_equal(after, reopened));
    close_harness(h); json_node_free(before); json_node_free(after); json_node_free(original_source); json_node_free(source_after); json_node_free(reopened);
}
static void test_unbound_and_outdated_source(void)
{
    Harness *h = open_harness(); JsonNode *original = record("emergency"), *before = record("programme");
    JsonNode *bad = deep_copy(original); JsonArray *queues = json_object_get_array_member(json_object_get_object_member(json_node_get_object(bad), "config"), "queues");
    JsonObject *entry = json_array_get_object_element(json_array_get_array_element(queues, 1), 1);
    const gchar *keys[] = {"media_id", "version_id", "media_file_id", "media_sha256"};
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) json_object_remove_member(entry, keys[i]);
    put_record(bad); JsonNode *saved_bad = record("emergency");
    SourceRevision expected = {h, radios_json_int(json_node_get_object(saved_bad), "_rev", 0)};
    json_node_free(saved_bad); radios_data_refresh(&h->ctx); wait_for(source_revised, &expected);
    GtkWidget *transfer = begin(h); wait_for(accepted, transfer);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(transfer, "em-programme-transfer-status"))), "unbound"));
    JsonNode *after = record("programme"); g_assert_true(json_node_equal(before, after)); json_node_free(after);
    gtk_widget_destroy(transfer); close_harness(h);
    JsonNode *current = record("emergency");
    json_object_set_member(json_node_get_object(current), "config", json_node_copy(json_object_get_member(json_node_get_object(original), "config")));
    put_record(current); json_node_free(current); h = open_harness();
    current = record("emergency"); put_record(current); json_node_free(current);
    transfer = begin(h); wait_for(accepted, transfer);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(transfer, "em-programme-transfer-status"))), "broadcast input changed"));
    after = record("programme"); g_assert_true(json_node_equal(before, after));
    gtk_widget_destroy(transfer); close_harness(h);
    json_node_free(original); json_node_free(before); json_node_free(bad); json_node_free(after);
}
static void test_target_change_and_closed_dialog(void)
{
    g_test_message("Opening target-change fixture");
    Harness *h = open_harness(); JsonNode *before = record("programme");
    flag(".transfer-ready", FALSE); flag(".transfer-hold", TRUE);
    g_test_message("Waiting for held prepared response");
    GtkWidget *transfer = begin(h); wait_held();
    radios_data_document_dirty(radios_broadcast_programme_document(&h->ctx), TRUE);
    g_test_message("Releasing response after target dirtied");
    flag(".transfer-hold", FALSE); wait_for(accepted, transfer);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(transfer, "em-programme-transfer-status"))), "değişti"));
    JsonNode *after = record("programme"); g_assert_true(json_node_equal(before, after)); json_node_free(after);
    gtk_widget_destroy(transfer); close_harness(h); h = open_harness();
    g_test_message("Waiting for response before closing dialog");
    flag(".transfer-ready", FALSE); flag(".transfer-hold", TRUE);
    transfer = begin(h); wait_held(); gtk_widget_destroy(transfer); flag(".transfer-hold", FALSE);
    run_for(500); after = record("programme"); g_assert_true(json_node_equal(before, after));
    json_node_free(after); close_harness(h); h = open_harness();
    g_test_message("Closing source page before held response completes");
    flag(".transfer-ready", FALSE); flag(".transfer-hold", TRUE);
    begin(h); wait_held(); close_harness(h); flag(".transfer-hold", FALSE);
    run_for(500); after = record("programme"); g_assert_true(json_node_equal(before, after));
    json_node_free(before); json_node_free(after);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); g_assert_cmpint(argc, ==, 2); gtk_init(&argc, &argv);
    GError *error = NULL; g_assert_true(radios_config_load(argv[1], NULL, &config, &error)); g_assert_no_error(error);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/emergency-transfer/verified-append-save-reopen", test_append_save_reopen);
    g_test_add_func("/emergency-transfer/unbound-and-outdated-source", test_unbound_and_outdated_source);
    g_test_add_func("/emergency-transfer/changed-target-and-closed-dialog", test_target_change_and_closed_dialog);
    int result = g_test_run(); radios_data_client_unref(reader); radios_config_clear(&config); return result;
}
