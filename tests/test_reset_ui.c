/* Ayarlar › Sıfırlama against an isolated genuine SDBASE store (run_data_ui.py):
 * the preview dialog names every affected record kind in Turkish and lists the
 * kinds every reset keeps (desktop_reset.KEPT_KINDS), the factory reset only
 * unlocks after SIFIRLA is typed, a settings reset retires the station's
 * documents through desktop-reset/apply and reports the kept kinds again, and
 * "Şimdi yedek al" writes a verified backup (desktop-reset/backup) whose real
 * path lands in the status line without resetting anything. */
#include "radios/data.h"
#include "radios/reset_ui.h"
#include <glib/gstdio.h>
#include <string.h>

static RadioSConfig config;
static RadioSDataClient *reader;
typedef struct { RadioSViewContext ctx; GtkWidget *window; } Harness;
typedef gboolean (*Condition)(gpointer);
static void pump(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void wait_for(Condition condition, gpointer value)
{
    gint64 end = g_get_monotonic_time() + 60 * G_TIME_SPAN_SECOND;
    while (!condition(value) && g_get_monotonic_time() < end) pump();
    g_assert_true(condition(value));
}
static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *required(GtkWidget *root, const gchar *name)
{ GtkWidget *widget = find(root, name); g_assert_nonnull(widget); return widget; }
static GtkWidget *toplevel(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *result = NULL;
    for (GList *item = windows; item && !result; item = item->next)
        if (g_str_equal(gtk_widget_get_name(item->data), name) && gtk_widget_get_visible(item->data)) result = item->data;
    g_list_free(windows); return result;
}
static gboolean toplevel_present(gpointer name) { return toplevel(name) != NULL; }
static gboolean toplevel_absent(gpointer name) { return toplevel(name) == NULL; }
static gboolean connected(gpointer value) { return radios_data_connected(&((Harness *)value)->ctx); }
static Harness *open_harness(void)
{
    Harness *h = g_new0(Harness, 1); h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(h->window);
    gtk_widget_set_name(h->window, "reset-ui-test-window");
    h->ctx = (RadioSViewContext){ .config = &config, .runtime = radios_runtime_new(32, 64), .parent = GTK_WINDOW(h->window) };
    gtk_widget_show_all(h->window);
    h->ctx.data = radios_data_new(&h->ctx); radios_data_connect(&h->ctx); wait_for(connected, h);
    return h;
}
static void close_harness(Harness *h)
{
    gtk_widget_destroy(h->window);
    if (h->ctx.data) radios_data_close(h->ctx.data);
    radios_runtime_free(h->ctx.runtime);
    while (g_main_context_iteration(NULL, FALSE)) {}
    if (h->ctx.data) radios_data_unref(h->ctx.data);
    g_object_unref(h->window); g_free(h);
}
typedef struct { const gchar *path; JsonNode *body, *result; GError *error; gint done; } Request;
static gpointer request_worker(gpointer value)
{
    Request *r = value; r->result = radios_data_client_request_sync(reader, "POST", r->path, r->body, NULL, &r->error);
    g_atomic_int_set(&r->done, 1); return NULL;
}
static gboolean request_done(gpointer value) { return g_atomic_int_get(&((Request *)value)->done); }
static JsonNode *request(const gchar *path, JsonNode *body)
{
    Request work = {.path = path, .body = body}; GThread *thread = g_thread_new("reset-read", request_worker, &work);
    wait_for(request_done, &work); g_thread_join(thread); g_assert_no_error(work.error); g_assert_nonnull(work.result); return work.result;
}
static JsonNode *body(void)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, json_object_new());
    json_object_set_string_member(json_node_get_object(node), "station_id", radios_data_client_station_id(reader)); return node;
}
/* Active documents of one kind, as desktop/list reports them. */
static guint active_documents(const gchar *kind)
{
    JsonNode *input = body(); json_object_set_string_member(json_node_get_object(input), "kind", kind);
    JsonNode *result = request("/v1/workflows/desktop/list", input); json_node_free(input);
    JsonArray *records = json_object_get_array_member(json_node_get_object(result), "records");
    guint count = json_array_get_length(records); json_node_free(result); return count;
}
static void put_document(const gchar *kind, const gchar *name, JsonObject *configuration)
{
    JsonNode *input = body(); JsonObject *object = json_node_get_object(input);
    json_object_set_string_member(object, "kind", kind); json_object_set_string_member(object, "name", name);
    json_object_set_object_member(object, "config", configuration);
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    JsonNode *result = request("/v1/workflows/desktop/put", input); json_node_free(input);
    g_assert_true(g_uuid_string_is_valid(radios_json_string(json_node_get_object(result), "_id", "")));
    json_node_free(result);
}
/* One MIDI map (a labelled kind) and one live player layout (a kind whose label
 * this page supplies itself); both belong to the "settings" reset scope. */
static void ensure_documents(void)
{
    if (!active_documents("midi_mapping")) {
        JsonObject *midi = json_object_new(); json_object_set_string_member(midi, "device", "");
        json_object_set_array_member(midi, "maps", json_array_new());
        put_document("midi_mapping", "MIDI", midi);
    }
    if (!active_documents("live_players")) {
        JsonObject *players = json_object_new(); json_object_set_int_member(players, "schema_version", 1);
        json_object_set_int_member(players, "deck_count", 2);
        put_document("live_players", "main", players);
    }
    g_assert_cmpuint(active_documents("midi_mapping"), ==, 1); g_assert_cmpuint(active_documents("live_players"), ==, 1);
}
static gboolean reset_ready(gpointer value)
{ return gtk_widget_is_sensitive(required(((Harness *)value)->window, "preferences-reset-settings")); }
static const gchar *status_text(Harness *h)
{ return gtk_label_get_text(GTK_LABEL(required(h->window, "preferences-reset-status"))); }
static GtkWidget *open_page(Harness *h)
{
    GtkWidget *page = radios_reset_ui_page_new(&h->ctx); gtk_container_add(GTK_CONTAINER(h->window), page);
    gtk_widget_show_all(h->window); wait_for(reset_ready, h);
    for (guint i = 0; i < 2; ++i) /* every reset needs the connection and the manage grant */
        g_assert_true(gtk_widget_is_sensitive(required(h->window, i ? "preferences-reset-factory" : "preferences-reset-media")));
    return page;
}
static GtkWidget *open_confirmation(Harness *h, const gchar *scope)
{
    gchar *name = g_strdup_printf("preferences-reset-%s", scope);
    gtk_button_clicked(GTK_BUTTON(required(h->window, name))); g_free(name);
    wait_for(toplevel_present, "preferences-reset-confirm");
    return toplevel("preferences-reset-confirm");
}
static void assert_summary_names_kinds(GtkWidget *dialog)
{
    const gchar *summary = gtk_label_get_text(GTK_LABEL(required(dialog, "preferences-reset-summary")));
    g_assert_nonnull(strstr(summary, "• MIDI haritaları (1)"));
    g_assert_nonnull(strstr(summary, "• Oynatıcı düzeni ve kümeleri (1)"));
    g_assert_null(strstr(summary, "live_players")); g_assert_null(strstr(summary, "midi_mapping"));
    const gchar *kept = gtk_label_get_text(GTK_LABEL(required(dialog, "preferences-reset-kept")));
    g_assert_true(g_str_has_prefix(kept, "Korunur: "));
    g_assert_nonnull(strstr(kept, "Yayın geçmişi")); g_assert_nonnull(strstr(kept, "Reklam raporları"));
    g_assert_nonnull(strstr(kept, "Yönetim kurtarma kayıtları"));
    g_assert_null(strstr(kept, "playout_history")); g_assert_null(strstr(kept, "_"));
}

static void test_preview_names_kinds_and_kept_records(void)
{
    Harness *h = open_harness(); ensure_documents(); open_page(h);
    GtkWidget *dialog = open_confirmation(h, "settings");
    assert_summary_names_kinds(dialog);
    g_assert_true(gtk_widget_is_sensitive(required(dialog, "preferences-reset-accept")));
    g_assert_null(find(dialog, "preferences-reset-confirm-text"));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    wait_for(toplevel_absent, "preferences-reset-confirm");
    g_assert_nonnull(strstr(status_text(h), "iptal edildi"));
    g_assert_cmpuint(active_documents("midi_mapping"), ==, 1); g_assert_cmpuint(active_documents("live_players"), ==, 1);
    close_harness(h);
}

static void test_factory_reset_unlocks_after_typed_confirmation(void)
{
    Harness *h = open_harness(); ensure_documents(); open_page(h);
    GtkWidget *dialog = open_confirmation(h, "factory");
    assert_summary_names_kinds(dialog);
    GtkWidget *accept = required(dialog, "preferences-reset-accept"), *entry = required(dialog, "preferences-reset-confirm-text");
    g_assert_false(gtk_widget_is_sensitive(accept));
    gtk_entry_set_text(GTK_ENTRY(entry), "sifirla"); pump(); g_assert_false(gtk_widget_is_sensitive(accept));
    gtk_entry_set_text(GTK_ENTRY(entry), "SIFIRLA"); pump(); g_assert_true(gtk_widget_is_sensitive(accept));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    wait_for(toplevel_absent, "preferences-reset-confirm");
    g_assert_cmpuint(active_documents("midi_mapping"), ==, 1); g_assert_cmpuint(active_documents("live_players"), ==, 1);
    close_harness(h);
}

static gboolean reset_finished(gpointer value)
{ return toplevel("preferences-reset-done") != NULL && reset_ready(value); }

static void test_settings_reset_retires_documents_and_reports_kept_kinds(void)
{
    Harness *h = open_harness(); ensure_documents(); open_page(h);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(required(h->window, "preferences-reset-backup")), FALSE);
    GtkWidget *dialog = open_confirmation(h, "settings");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    wait_for(reset_finished, h);
    const gchar *status = status_text(h);
    g_assert_nonnull(strstr(status, "Genel ayarları sıfırla tamamlandı."));
    g_assert_nonnull(strstr(status, "• MIDI haritaları (1)")); g_assert_nonnull(strstr(status, "• Oynatıcı düzeni ve kümeleri (1)"));
    g_assert_nonnull(strstr(status, "Korunur: ")); g_assert_nonnull(strstr(status, "Yayın geçmişi"));
    g_assert_null(strstr(status, "Yedek:"));
    gtk_dialog_response(GTK_DIALOG(toplevel("preferences-reset-done")), GTK_RESPONSE_CANCEL);
    wait_for(toplevel_absent, "preferences-reset-done");
    g_assert_cmpuint(active_documents("midi_mapping"), ==, 0); g_assert_cmpuint(active_documents("live_players"), ==, 0);
    close_harness(h);
}

static gboolean backup_reported(gpointer value)
{ return g_str_has_prefix(status_text(value), "Yedek alındı: ") && reset_ready(value); }

static void test_backup_now_writes_a_verified_backup_without_resetting(void)
{
    Harness *h = open_harness(); ensure_documents(); open_page(h);
    g_assert_true(gtk_widget_is_sensitive(required(h->window, "preferences-reset-backup-now")));
    gtk_button_clicked(GTK_BUTTON(required(h->window, "preferences-reset-backup-now")));
    wait_for(backup_reported, h);
    gchar **lines = g_strsplit(status_text(h), "\n", 2);
    g_assert_nonnull(lines[1]); g_assert_nonnull(strstr(lines[1], "Dosya: ")); g_assert_nonnull(strstr(lines[1], "İşlem sırası: "));
    const gchar *path = lines[0] + strlen("Yedek alındı: ");
    g_assert_true(g_path_is_absolute(path)); g_assert_true(g_file_test(path, G_FILE_TEST_IS_DIR));
    gchar *base = g_path_get_basename(path); g_assert_true(g_str_has_prefix(base, "yedek-")); g_free(base);
    gchar *manifest = g_build_filename(path, "manifest.json", NULL), *contents = NULL;
    g_assert_true(g_file_get_contents(manifest, &contents, NULL, NULL)); g_free(manifest);
    g_assert_nonnull(strstr(contents, radios_data_client_store_id(reader))); g_free(contents);
    g_assert_null(toplevel("preferences-reset-confirm")); g_assert_null(toplevel("preferences-reset-done"));
    g_assert_cmpuint(active_documents("midi_mapping"), ==, 1); g_assert_cmpuint(active_documents("live_players"), ==, 1);
    g_strfreev(lines); close_harness(h);
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *project_root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc; g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    GError *error = NULL; g_assert_true(radios_config_load(project_root, NULL, &config, &error)); g_assert_no_error(error); g_free(project_root);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/reset-ui/preview-names-kinds-and-kept-records", test_preview_names_kinds_and_kept_records);
    g_test_add_func("/reset-ui/factory-reset-unlocks-after-typed-confirmation", test_factory_reset_unlocks_after_typed_confirmation);
    g_test_add_func("/reset-ui/settings-reset-retires-documents-and-reports-kept-kinds", test_settings_reset_retires_documents_and_reports_kept_kinds);
    g_test_add_func("/reset-ui/backup-now-writes-a-verified-backup-without-resetting", test_backup_now_writes_a_verified_backup_without_resetting);
    gint result = g_test_run(); radios_data_client_unref(reader); radios_config_clear(&config); return result;
}
