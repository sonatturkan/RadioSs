#include "radios/window.h"
#include "radios/data.h"
#include "radios/broadcast_header.h"
#include "radios/playout_ui.h"
#include <math.h>
#include <string.h>

static RadioSConfig config;
static GtkApplication *application;
static RadioSDataClient *reader;
typedef struct { RadioSWindow *window; RadioSRuntime *runtime; GtkWidget *root; } Window;

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (!g_strcmp0(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *it = children; it && !found; it = it->next) found = find(it->data, name);
    g_list_free(children); return found;
}
static void pump(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static gboolean documents_ready(GtkWidget *root)
{
    GPtrArray *docs = g_object_get_data(G_OBJECT(root), "radios-data-documents");
    for (guint i = 0; docs && i < docs->len; ++i) if (!radios_data_document_ready(g_ptr_array_index(docs, i))) return FALSE;
    if (!GTK_IS_CONTAINER(root)) return TRUE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); gboolean ready = TRUE;
    for (GList *it = children; it && ready; it = it->next) ready = documents_ready(it->data);
    g_list_free(children); return ready;
}
static Window open_window(void)
{
    Window w = {0}; GError *error = NULL; w.runtime = radios_runtime_new(64, 128);
    w.window = radios_window_new(application, &config, w.runtime, FALSE, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(w.window);
    w.root = gtk_application_get_windows(application)->data; radios_window_present(w.window);
    RadioSViewContext *ctx = radios_window_view_context(w.window);
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while ((!radios_data_connected(ctx) || !documents_ready(w.root)) && g_get_monotonic_time() < deadline) pump();
    g_assert_true(radios_data_connected(ctx)); g_assert_true(documents_ready(w.root)); return w;
}
static void close_window(Window *w)
{ radios_window_free(w->window); radios_runtime_free(w->runtime); pump(); }
static void wait_saved(Window *w)
{
    GtkWidget *status = find(w->root, "programme-automation-status"); g_assert_true(GTK_IS_LABEL(status));
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    while (g_strcmp0(gtk_label_get_text(GTK_LABEL(status)), "Kaydedildi") && g_get_monotonic_time() < deadline) pump();
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(status)), ==, "Kaydedildi");
}
static void assert_policy(Window *w)
{
    guint mode = 99; gdouble overlap = -1; gboolean beat = FALSE;
    g_assert_true(radios_broadcast_transition_policy(radios_window_view_context(w->window), &mode, &overlap, &beat));
    g_assert_cmpuint(mode, ==, 1); g_assert_cmpfloat(fabs(overlap - 1.6), <, .00001); g_assert_true(beat);
}
static void test_general_defaults_save_restore_and_player_queue(void)
{
    Window w = open_window(); GtkWidget *mode = find(w.root, "programme-transition-mode");
    GtkWidget *overlap = find(w.root, "programme-transition-overlap");
    g_assert_true(GTK_IS_COMBO_BOX(mode)); g_assert_true(GTK_IS_SPIN_BUTTON(overlap));
    gtk_combo_box_set_active(GTK_COMBO_BOX(mode), 4); gtk_spin_button_set_value(GTK_SPIN_BUTTON(overlap), 1.6);
    assert_policy(&w); wait_saved(&w);
    JsonNode *body = json_node_new(JSON_NODE_OBJECT); json_node_take_object(body, json_object_new());
    json_object_set_string_member(json_node_get_object(body), "station_id", radios_data_client_station_id(reader));
    json_object_set_string_member(json_node_get_object(body), "kind", "playout_policy");
    GError *error = NULL; JsonNode *result = radios_data_client_request_sync(reader, "POST", "/v1/workflows/desktop/list", body, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(result); JsonArray *records = json_object_get_array_member(json_node_get_object(result), "records");
    g_assert_cmpuint(json_array_get_length(records), ==, 1);
    JsonObject *policy = json_object_get_object_member(json_array_get_object_element(records, 0), "config");
    g_assert_cmpint(radios_json_int(policy, "overlap_ms", 0), ==, 1600); g_assert_true(json_object_get_boolean_member(policy, "beat_sync"));
    json_node_free(result); json_node_free(body);
    GtkWidget *selector = find(w.root, "programme-player-selector");
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(selector), "1")); assert_policy(&w);
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(mode)), ==, 0);
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(selector), "0"));
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(mode)), ==, 4); assert_policy(&w);
    close_window(&w); w = open_window(); assert_policy(&w);
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(find(w.root, "programme-transition-mode"))), ==, 4);
    GtkWidget *gear = find(w.root, "programme-automation-settings"); g_assert_true(GTK_IS_BUTTON(gear));
    gtk_button_clicked(GTK_BUTTON(gear)); pump();
    GtkWidget *dialog = NULL;
    GList *windows = gtk_application_get_windows(application);
    for (GList *it = windows; it; it = it->next)
        if (!g_strcmp0(gtk_widget_get_name(it->data), "programme-automation-dialog")) dialog = it->data;
    /* Transient GTK dialogs need not belong to GtkApplication. */
    if (!dialog) {
        GList *toplevels = gtk_window_list_toplevels();
        for (GList *it = toplevels; it; it = it->next)
            if (!g_strcmp0(gtk_widget_get_name(it->data), "programme-automation-dialog")) dialog = it->data;
        g_list_free(toplevels);
    }
    g_assert_true(GTK_IS_DIALOG(dialog));
    gtk_combo_box_set_active(GTK_COMBO_BOX(find(dialog, "programme-automation-dialog-mode")), 0);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); pump(); assert_policy(&w);
    gtk_button_clicked(GTK_BUTTON(gear)); pump();
    GList *toplevels = gtk_window_list_toplevels(); dialog = NULL;
    for (GList *it = toplevels; it; it = it->next)
        if (!g_strcmp0(gtk_widget_get_name(it->data), "programme-automation-dialog")) dialog = it->data;
    g_list_free(toplevels); g_assert_true(GTK_IS_DIALOG(dialog));
    RadioSViewContext *ctx = radios_window_view_context(w.window); radios_data_connect(ctx);
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!radios_data_connected(ctx) && g_get_monotonic_time() < deadline) pump();
    g_assert_true(radios_data_connected(ctx));
    gtk_combo_box_set_active(GTK_COMBO_BOX(find(dialog, "programme-automation-dialog-mode")), 0);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); pump(); wait_saved(&w);
    guint saved_mode = 99; gdouble saved_overlap = -1; gboolean saved_beat = TRUE;
    g_assert_true(radios_broadcast_transition_policy(ctx, &saved_mode, &saved_overlap, &saved_beat));
    g_assert_cmpuint(saved_mode, ==, 0); g_assert_cmpfloat(saved_overlap, ==, 0); g_assert_false(saved_beat);
    gtk_button_clicked(GTK_BUTTON(gear)); pump(); toplevels = gtk_window_list_toplevels(); dialog = NULL;
    for (GList *it = toplevels; it; it = it->next)
        if (!g_strcmp0(gtk_widget_get_name(it->data), "programme-automation-dialog")) dialog = it->data;
    g_list_free(toplevels); g_assert_true(GTK_IS_DIALOG(dialog));
    GWeakRef popup; g_weak_ref_init(&popup, dialog); close_window(&w);
    GObject *remaining = g_weak_ref_get(&popup); g_assert_null(remaining); g_weak_ref_clear(&popup);
    /* A reconnect disables transport synchronously. Close before its callback
     * can return and require both edits to survive in the durable recovery log. */
    w = open_window(); ctx = radios_window_view_context(w.window); radios_data_connect(ctx);
    g_assert_false(radios_data_connected(ctx));
    gtk_combo_box_set_active(GTK_COMBO_BOX(find(w.root, "programme-transition-mode")), 4);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(find(w.root, "programme-transition-overlap")), 1.6);
    close_window(&w); w = open_window(); assert_policy(&w); close_window(&w);
}
static void test_copyright_cache_revision_and_window_isolation(void)
{
    GtkWidget *first = gtk_window_new(GTK_WINDOW_TOPLEVEL), *second = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    RadioSViewContext ctx = {.parent = GTK_WINDOW(first)}, other = {.parent = GTK_WINDOW(second)};
    JsonObject *policy = json_object_new(); json_object_set_int_member(policy, "schema", 1);
    json_object_set_boolean_member(policy, "restrict", TRUE); json_object_set_string_member(policy, "unregistered", "allow");
    json_object_set_array_member(policy, "rules", json_array_new());
    const gchar *id = "90000000-0000-4000-8000-000000000001";
    g_assert_true(radios_playout_ui_copyright_policy_record(&ctx, policy, id, 5));
    json_object_set_boolean_member(policy, "restrict", FALSE);
    g_assert_true(json_object_get_boolean_member(radios_playout_ui_copyright_policy_get(&ctx), "restrict"));
    g_assert_false(radios_playout_ui_copyright_policy_record(&ctx, policy, id, 4));
    g_assert_false(radios_playout_ui_copyright_policy_record(&ctx, policy, NULL, 0));
    g_assert_true(json_object_get_boolean_member(radios_playout_ui_copyright_policy_get(&ctx), "restrict"));
    g_assert_null(radios_playout_ui_copyright_policy_get(&other));
    g_assert_true(radios_playout_ui_copyright_policy_record(&ctx, policy, id, 6));
    g_assert_false(json_object_get_boolean_member(radios_playout_ui_copyright_policy_get(&ctx), "restrict"));
    json_object_set_boolean_member(policy, "restrict", TRUE);
    g_assert_true(radios_playout_ui_copyright_policy_record(&other, policy, id, 1));
    g_assert_true(json_object_get_boolean_member(radios_playout_ui_copyright_policy_get(&other), "restrict"));
    g_assert_false(json_object_get_boolean_member(radios_playout_ui_copyright_policy_get(&ctx), "restrict"));
    json_object_unref(policy); gtk_widget_destroy(first); gtk_widget_destroy(second);
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc; g_test_init(&argc, &argv, NULL); if (!gtk_init_check(&argc, &argv)) return 77;
    GError *error = NULL; g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    application = gtk_application_new("org.radios.PlayoutPolicyAcceptance", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/playout-policy/general-save-restore-player-queue", test_general_defaults_save_restore_and_player_queue);
    g_test_add_func("/playout-policy/copyright-cache-revision-isolation", test_copyright_cache_revision_and_window_isolation);
    gint result = g_test_run(); radios_data_client_unref(reader); g_object_unref(application); radios_config_clear(&config); return result;
}
