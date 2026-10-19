#include "radios/workflow_hub.h"
#include "radios/data.h"
#include <glib/gstdio.h>
#include <string.h>

static RadioSConfig config;
static RadioSDataClient *reader;
static RadioSRuntime *runtime;
typedef struct { GtkWidget *window, *hub; RadioSViewContext ctx; } Fixture;
typedef gboolean (*Condition)(gpointer);
static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (!g_strcmp0(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *control(Fixture *fixture, const gchar *name)
{ GtkWidget *widget = find(fixture->hub, name); g_assert_nonnull(widget); return widget; }
static void pump(void)
{
    RadioSEvent *event;
    while (runtime && (event = radios_runtime_pop_event(runtime)) != NULL) radios_event_free(event);
    while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000);
}
static void wait_until(Condition condition, gpointer data, const gchar *why)
{
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!condition(data) && g_get_monotonic_time() < deadline) pump();
    if (!condition(data)) g_error("Workflow UI acceptance timed out: %s", why);
}
static gboolean ready(gpointer data)
{
    Fixture *fixture = data; GtkWidget *choice = control(fixture, "workflow-family");
    return radios_data_connected(&fixture->ctx) && gtk_tree_model_iter_n_children(gtk_combo_box_get_model(GTK_COMBO_BOX(choice)), NULL) > 0;
}
static Fixture *open_fixture(void)
{
    Fixture *fixture = g_new0(Fixture, 1); fixture->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(fixture->window);
    gtk_window_set_default_size(GTK_WINDOW(fixture->window), 1280, 900);
    fixture->ctx.config = &config; fixture->ctx.parent = GTK_WINDOW(fixture->window);
    fixture->ctx.runtime = runtime = radios_runtime_new(64, 256); fixture->ctx.data = radios_data_new(&fixture->ctx);
    fixture->hub = radios_workflow_hub_new(&fixture->ctx); gtk_container_add(GTK_CONTAINER(fixture->window), fixture->hub);
    gtk_widget_show_all(fixture->window); radios_data_connect(&fixture->ctx);
    wait_until(ready, fixture, "genuine authenticated catalogue renders native family controls"); return fixture;
}
static void close_fixture(Fixture *fixture)
{
    gtk_widget_destroy(fixture->window); pump(); radios_data_close(fixture->ctx.data); radios_data_unref(fixture->ctx.data);
    radios_runtime_free(fixture->ctx.runtime); runtime = NULL;
    g_object_unref(fixture->window); g_free(fixture);
}
static JsonNode *request(const gchar *path, JsonNode *body)
{
    GError *error = NULL; JsonNode *result = radios_data_client_request_sync(reader, "POST", path, body, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(result); return result;
}
static JsonNode *body(void)
{
    JsonObject *object = json_object_new(); json_object_set_string_member(object, "station_id", radios_data_client_station_id(reader));
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, object); return node;
}
static JsonNode *records(const gchar *collection, const gchar *id)
{
    JsonNode *input = body(); JsonObject *object = json_node_get_object(input); json_object_set_string_member(object, "collection", collection);
    if (id) json_object_set_string_member(object, "record_id", id);
    JsonNode *result = request("/v1/workflows/desktop-workflows/records", input); json_node_free(input); return result;
}
static void choose(Fixture *fixture, const gchar *family, const gchar *action)
{
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(control(fixture, "workflow-family")), family));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(control(fixture, "workflow-action")), action)); pump();
}
static void entry(Fixture *fixture, const gchar *name, const gchar *value)
{
    gchar *key = g_strconcat("workflow-input-", name, NULL); GtkWidget *widget = control(fixture, key); g_free(key);
    g_assert_true(GTK_IS_ENTRY(widget)); gtk_entry_set_text(GTK_ENTRY(widget), value); pump();
}
static void click(Fixture *fixture, const gchar *name)
{
    GtkWidget *button = control(fixture, name); g_assert_true(GTK_IS_BUTTON(button)); g_assert_true(gtk_widget_is_sensitive(button));
    gtk_button_clicked(GTK_BUTTON(button)); pump();
}
static gboolean acknowledged(gpointer data)
{ Fixture *fixture = data; return g_object_get_data(G_OBJECT(fixture->hub), "workflow-last-result") != NULL && gtk_widget_is_sensitive(control(fixture, "workflow-submit")); }
static gboolean readback(gpointer data)
{ Fixture *fixture = data; return g_object_get_data(G_OBJECT(fixture->hub), "workflow-readback") != NULL; }
typedef struct { GtkComboBox *choice; const gchar *id; } Selection;
static gboolean available(gpointer data)
{
    Selection *selection = data; GtkTreeModel *model = gtk_combo_box_get_model(selection->choice); GtkTreeIter iter;
    gboolean more = gtk_tree_model_get_iter_first(model, &iter);
    while (more) { gchar *id = NULL; gtk_tree_model_get(model, &iter, 1, &id, -1); gboolean match = !g_strcmp0(id, selection->id); g_free(id); if (match) return TRUE; more = gtk_tree_model_iter_next(model, &iter); }
    return FALSE;
}
static void pick(Fixture *fixture, const gchar *name, const gchar *id)
{
    gchar *key = g_strconcat("workflow-input-", name, NULL); Selection selection = {GTK_COMBO_BOX(control(fixture, key)), id}; g_free(key);
    wait_until(available, &selection, "real immutable identity appears in record picker"); g_assert_true(gtk_combo_box_set_active_id(selection.choice, id)); pump();
}
static gboolean conflict(gpointer data)
{
    Fixture *fixture = data; const gchar *text = gtk_label_get_text(GTK_LABEL(control(fixture, "workflow-status")));
    return strstr(text, "başka bir işlem") != NULL;
}
static void test_category_ack_cas_reopen(void)
{
    Fixture *fixture = open_fixture(); choose(fixture, "catalog", "catalog/category-create");
    entry(fixture, "code", "native-workflow-category"); entry(fixture, "name", "Türkçe özel kategori"); click(fixture, "workflow-submit");
    wait_until(readback, fixture, "native create ACK followed by exact independent record readback");
    JsonNode *result = g_object_get_data(G_OBJECT(fixture->hub), "workflow-last-result");
    gchar *id = g_strdup(radios_json_string(json_node_get_object(result), "_id", "")); gint64 revision = radios_json_int(json_node_get_object(result), "_rev", 0);
    g_assert_true(g_uuid_string_is_valid(id)); g_assert_cmpint(revision, >, 0);
    choose(fixture, "catalog", "catalog/category-update"); pick(fixture, "category_id", id); pick(fixture, "expected_revision", id); entry(fixture, "name", "Operatörün yeni adı");
    JsonNode *input = body(); JsonObject *change = json_node_get_object(input); json_object_set_string_member(change, "category_id", id);
    json_object_set_int_member(change, "expected_revision", revision); json_object_set_string_member(change, "name", "Eşzamanlı gerçek değişiklik");
    JsonNode *other = request("/v1/workflows/catalog/category-update", input); json_node_free(input); json_node_free(other);
    click(fixture, "workflow-submit"); wait_until(conflict, fixture, "actual stale CAS is rejected with the operator's draft intact");
    JsonNode *check = records("categories", id); JsonObject *row = json_array_get_object_element(json_object_get_array_member(json_node_get_object(check), "records"), 0);
    g_assert_cmpstr(radios_json_string(row, "name", ""), ==, "Eşzamanlı gerçek değişiklik"); json_node_free(check);
    click(fixture, "workflow-refresh-category_id"); click(fixture, "workflow-refresh-expected_revision");
    gint64 deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND; while (g_get_monotonic_time() < deadline) pump();
    click(fixture, "workflow-submit"); wait_until(readback, fixture, "reconciled current CAS change is acknowledged and reread");
    check = records("categories", id); row = json_array_get_object_element(json_object_get_array_member(json_node_get_object(check), "records"), 0);
    g_assert_cmpstr(radios_json_string(row, "name", ""), ==, "Operatörün yeni adı"); g_assert_cmpint(radios_json_int(row, "_rev", 0), >, revision); json_node_free(check);
    close_fixture(fixture); fixture = open_fixture(); choose(fixture, "catalog", "catalog/category-update"); pick(fixture, "category_id", id);
    gchar *label = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(control(fixture, "workflow-input-category_id")));
    g_assert_nonnull(strstr(label, "Operatörün yeni adı")); g_free(label); close_fixture(fixture); g_free(id);
}
static void test_smart_block_native_tree(void)
{
    Fixture *fixture = open_fixture(); choose(fixture, "smart-blocks", "smart-blocks/create");
    entry(fixture, "code", "native-smart-rule"); entry(fixture, "name", "Yerli rock seçim kuralları"); entry(fixture, "target_duration_ms", "180000");
    gtk_combo_box_set_active(GTK_COMBO_BOX(control(fixture, "workflow-choice-rule_tree")), 1);
    click(fixture, "workflow-add-all");
    gtk_combo_box_set_active(GTK_COMBO_BOX(control(fixture, "workflow-input-field")), 5);
    gtk_combo_box_set_active(GTK_COMBO_BOX(control(fixture, "workflow-input-op")), 1); entry(fixture, "value", "Rock");
    click(fixture, "workflow-submit"); wait_until(acknowledged, fixture, "native structured rule tree reaches the real smart-block validator");
    wait_until(readback, fixture, "actual smart-block owner identity reread");
    JsonNode *result = g_object_get_data(G_OBJECT(fixture->hub), "workflow-last-result"); gchar *id = g_strdup(radios_json_string(json_node_get_object(result), "block_id", "")); g_assert_true(g_uuid_string_is_valid(id));
    JsonNode *input = body(); json_object_set_string_member(json_node_get_object(input), "block_id", id);
    JsonNode *versions = request("/v1/workflows/smart-blocks/versions", input); json_node_free(input);
    gchar *text = json_to_string(versions, FALSE); g_assert_nonnull(strstr(text, "Rock")); g_assert_nonnull(strstr(text, "contains")); g_assert_nonnull(strstr(text, "\"all\"")); g_free(text); json_node_free(versions);
    close_fixture(fixture); g_free(id);
}
static void test_explicit_null_clears_category(void)
{
    JsonNode *input = body(); json_object_set_string_member(json_node_get_object(input), "kind", "music"); json_object_set_string_member(json_node_get_object(input), "title", "Nullable gerçek katalog parçası");
    JsonNode *created = request("/v1/items", input); json_node_free(input); gchar *item_id = g_strdup(radios_json_string(json_node_get_object(created), "_id", "")); json_node_free(created); g_assert_true(g_uuid_string_is_valid(item_id));
    input = body(); json_object_set_string_member(json_node_get_object(input), "code", "nullable-native-category"); json_object_set_string_member(json_node_get_object(input), "name", "Temizlenecek kategori");
    created = request("/v1/workflows/catalog/category-create", input); json_node_free(input); gchar *category_id = g_strdup(radios_json_string(json_node_get_object(created), "_id", "")); json_node_free(created);
    JsonNode *associations = records("station_items", NULL); JsonArray *rows = json_object_get_array_member(json_node_get_object(associations), "records"); gchar *association_id = NULL;
    for (guint i = 0; i < json_array_get_length(rows); ++i) { JsonObject *row = json_array_get_object_element(rows, i); if (!g_strcmp0(radios_json_string(row, "item_id", ""), item_id)) association_id = g_strdup(radios_json_string(row, "_id", "")); }
    json_node_free(associations); g_assert_nonnull(association_id);
    Fixture *fixture = open_fixture(); choose(fixture, "catalog", "catalog/item-category"); pick(fixture, "item_id", item_id); pick(fixture, "category_id", category_id); pick(fixture, "expected_revision", association_id);
    click(fixture, "workflow-submit"); wait_until(readback, fixture, "actual item association category assignment is acknowledged and reread");
    JsonNode *check = records("station_items", association_id); JsonObject *row = json_array_get_object_element(json_object_get_array_member(json_node_get_object(check), "records"), 0);
    g_assert_cmpstr(radios_json_string(row, "category_id", ""), ==, category_id); json_node_free(check);
    click(fixture, "workflow-refresh-expected_revision"); gint64 deadline = g_get_monotonic_time() + G_TIME_SPAN_SECOND; while (g_get_monotonic_time() < deadline) pump();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(fixture, "workflow-null-category_id")), TRUE); click(fixture, "workflow-submit");
    wait_until(readback, fixture, "explicit null reaches required nullable domain field and clears the category");
    check = records("station_items", association_id); row = json_array_get_object_element(json_object_get_array_member(json_node_get_object(check), "records"), 0);
    g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(row, "category_id"))); json_node_free(check);
    close_fixture(fixture); g_free(item_id); g_free(category_id); g_free(association_id);
}
static gboolean source_ready(gpointer data)
{
    Fixture *fixture = data; return strstr(gtk_label_get_text(GTK_LABEL(control(fixture, "workflow-status"))), "tam içeriği alındı") != NULL;
}
static void test_native_file_preview_apply(void)
{
    gchar *path = g_build_filename(config.project_root, "Özel mAirList kaynağı.csv", NULL);
    GString *csv = g_string_new(",native-source-1,C:\\Radio\\Yerel.wav,Türkçe kaynak,Özgün,,2,");
    for (guint i = 0; i < 12; ++i) g_string_append_c(csv, ',');
    g_string_append(csv, "\r\n"); GError *error = NULL;
    g_assert_true(g_file_set_contents(path, csv->str, (gssize)csv->len, &error)); g_assert_no_error(error);
    gchar *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)csv->str, csv->len); g_string_free(csv, TRUE);
    Fixture *fixture = open_fixture(); choose(fixture, "transfers", "transfers/mairlist-preview");
    GtkWidget *picker = control(fixture, "workflow-input-source_base64"); g_assert_true(GTK_IS_FILE_CHOOSER_BUTTON(picker));
    g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(picker), path)); g_signal_emit_by_name(picker, "file-set");
    wait_until(source_ready, fixture, "bounded real source bytes load on the file worker"); entry(fixture, "source_namespace", "native-transfer-private");
    click(fixture, "workflow-submit"); wait_until(acknowledged, fixture, "actual mAirList parser reviews the selected file bytes");
    JsonNode *preview = g_object_get_data(G_OBJECT(fixture->hub), "workflow-last-result");
    g_assert_cmpstr(radios_json_string(json_node_get_object(preview), "source_sha256", ""), ==, sha);
    g_assert_cmpint(radios_json_int(json_node_get_object(preview), "invalid_rows", -1), ==, 0);
    choose(fixture, "transfers", "transfers/mairlist-apply"); g_assert_null(find(fixture->hub, "workflow-input-source_base64"));
    click(fixture, "workflow-submit"); wait_until(acknowledged, fixture, "apply reuses exact preview bytes and source hash through the real import service");
    JsonNode *applied = g_object_get_data(G_OBJECT(fixture->hub), "workflow-last-result"); JsonObject *receipt = json_node_get_object(applied);
    g_assert_cmpint(radios_json_int(receipt, "created_items", -1), ==, 1); g_assert_cmpint(radios_json_int(receipt, "audio_files_imported", -1), ==, 0);
    g_assert_true(json_object_get_boolean_member(receipt, "all_content_requires_review")); g_assert_cmpstr(radios_json_string(receipt, "source_sha256", ""), ==, sha);
    close_fixture(fixture); g_assert_cmpint(g_unlink(path), ==, 0); g_free(path); g_free(sha);
}
static void test_all_declared_forms_render(void)
{
    Fixture *fixture = open_fixture(); JsonNode *input = body(); JsonNode *catalogue = request("/v1/workflows/desktop-workflows/catalogue", input); json_node_free(input);
    JsonArray *families = json_object_get_array_member(json_node_get_object(catalogue), "families"); guint count = 0;
    for (guint i = 0; i < json_array_get_length(families); ++i) {
        JsonObject *family = json_array_get_object_element(families, i); JsonArray *actions = json_object_get_array_member(family, "actions");
        for (guint j = 0; j < json_array_get_length(actions); ++j) {
            JsonObject *action = json_array_get_object_element(actions, j); choose(fixture, radios_json_string(family, "id", ""), radios_json_string(action, "id", ""));
            g_assert_true(gtk_widget_is_sensitive(control(fixture, "workflow-submit"))); g_assert_true(GTK_IS_BOX(control(fixture, "workflow-form"))); ++count;
        }
    }
    g_assert_cmpuint(count, >=, 150); g_test_message("Rendered %u genuine backend action forms without submitting mutations", count);
    json_node_free(catalogue); close_fixture(fixture);
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]); for (gint i = 1; i < argc; ++i) argv[i] = argv[i+1]; --argc;
    g_setenv("GSETTINGS_BACKEND", "memory", FALSE); g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    GError *error = NULL; g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/workflow-hub/category-create-reread-CAS-conflict-reconcile-reopen", test_category_ack_cas_reopen);
    g_test_add_func("/workflow-hub/native-smart-block-condition-tree", test_smart_block_native_tree);
    g_test_add_func("/workflow-hub/explicit-null-clears-real-item-category", test_explicit_null_clears_category);
    g_test_add_func("/workflow-hub/native-file-exact-byte-preview-apply", test_native_file_preview_apply);
    g_test_add_func("/workflow-hub/all-declared-action-forms-render", test_all_declared_forms_render);
    gint result = g_test_run(); radios_data_client_unref(reader); radios_config_clear(&config); return result;
}
