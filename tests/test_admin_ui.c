#include "radios/admin_ui.h"
#include "radios/recovery.h"
#include <glib/gstdio.h>
#include <string.h>

static RadioSConfig config;
static RadioSDataClient *reader;
static gint64 recovery_blocked;
static gchar *last_notice;
typedef struct { RadioSViewContext ctx; GtkWidget *window, *page; } Harness;

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *dialog(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *result = NULL;
    for (GList *item = windows; item && !result; item = item->next)
        if (g_str_equal(gtk_widget_get_name(item->data), name)) result = item->data;
    g_list_free(windows); return result;
}
static void pump(void)
{ while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
typedef gboolean (*Condition)(gpointer);
static void wait_for(Condition condition, gpointer value, const gchar *message)
{
    gint64 deadline = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;
    while (!condition(value) && g_get_monotonic_time() < deadline) pump();
    if (!condition(value)) g_error("Native admin acceptance timed out: %s (%s)", message, last_notice ? last_notice : "no notice");
}
static gboolean connected(gpointer value)
{ return radios_data_connected(&((Harness *)value)->ctx) && find(((Harness *)value)->page, "admin-user-edit"); }
static void notice(gpointer unused, const gchar *message)
{ (void)unused; g_free(last_notice); last_notice = g_strdup(message); }
static Harness *open_harness(void)
{
    Harness *h = g_new0(Harness, 1); h->ctx.config = &config;
    h->ctx.notify = notice;
    h->ctx.runtime = radios_runtime_new(64, 256);
    h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); h->ctx.parent = GTK_WINDOW(h->window);
    h->ctx.data = radios_data_new(&h->ctx); h->page = radios_technical_new(&h->ctx);
    gtk_container_add(GTK_CONTAINER(h->window), h->page); gtk_widget_show_all(h->window);
    radios_data_connect(&h->ctx); wait_for(connected, h, "station context and actual users");
    return h;
}
static void close_harness(Harness *h)
{
    gtk_widget_destroy(h->window); radios_data_close(h->ctx.data);
    radios_runtime_free(h->ctx.runtime); radios_data_unref(h->ctx.data);
    for (guint i = 0; i < 20; ++i) pump();
    g_free(h);
}
static void click(GtkWidget *root, const gchar *name)
{
    GtkWidget *widget = find(root, name); g_assert_true(GTK_IS_BUTTON(widget));
    g_assert_true(gtk_widget_is_sensitive(widget)); gtk_button_clicked(GTK_BUTTON(widget));
}
static JsonNode *body(void)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, json_object_new());
    json_object_set_string_member(json_node_get_object(node), "station_id", radios_data_client_station_id(reader)); return node;
}
static void key(JsonNode *node)
{
    gchar *value = g_uuid_string_random(); json_object_set_string_member(json_node_get_object(node), "idempotency_key", value); g_free(value);
}
static JsonNode *request(const gchar *action, JsonNode *payload)
{
    gchar *path = g_strdup_printf("/v1/workflows/%s", action); GError *error = NULL;
    JsonNode *result = radios_data_client_request_sync(reader, "POST", path, payload, NULL, &error);
    g_free(path); g_assert_no_error(error); g_assert_nonnull(result); return result;
}
static JsonNode *overview(void)
{ JsonNode *payload = body(), *result = request("desktop-admin/overview", payload); json_node_free(payload); return result; }
static JsonObject *user_find(JsonNode *node, const gchar *username)
{
    JsonArray *users = json_object_get_array_member(json_node_get_object(node), "users");
    for (guint i = 0; i < json_array_get_length(users); ++i) {
        JsonObject *user = json_array_get_object_element(users, i);
        if (g_str_equal(radios_json_string(user, "username", ""), username)) return user;
    }
    return NULL;
}
static gboolean dialog_open(gpointer name) { return dialog(name) != NULL; }
static gboolean dialog_closed(gpointer name) { return dialog(name) == NULL; }
static gboolean health_ready(gpointer root)
{
    GtkWidget *status = find(root, "admin-health-status");
    return status && strstr(gtk_label_get_text(GTK_LABEL(status)), "Depo:") != NULL;
}
static void monitor(Harness *h)
{
    GtkWidget *expander = find(h->page, "admin-monitor-expander");
    gtk_expander_set_expanded(GTK_EXPANDER(expander), TRUE); wait_for(health_ready, h->page, "real service integrity overview");
}
static GtkWidget *user_button(GtkWidget *root, const gchar *user_id)
{
    if (g_strcmp0(g_object_get_data(G_OBJECT(root), "admin-user-id"), user_id) == 0) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = user_button(item->data, user_id);
    g_list_free(children); return result;
}
static void open_user(Harness *h, const gchar *id)
{
    GtkWidget *people = find(h->page, "technical-people-pane");
    gtk_stack_set_visible_child_name(GTK_STACK(gtk_widget_get_parent(people)), "people");
    GtkWidget *widget = user_button(h->page, id); g_assert_nonnull(widget); gtk_button_clicked(GTK_BUTTON(widget));
    wait_for(dialog_open, "admin-user-dialog", "fresh station permission editor");
}
static JsonNode *create_user(const gchar *username)
{
    JsonNode *payload = body(); JsonObject *record = json_node_get_object(payload);
    json_object_set_string_member(record, "username", username);
    json_object_set_string_member(record, "display_name", "Native Yönetim Kişisi");
    json_object_set_string_member(record, "role", "operator");
    json_object_set_array_member(record, "additional_permissions", json_array_new()); key(payload);
    JsonNode *result = request("desktop-admin/user-create", payload); json_node_free(payload); return result;
}
static gboolean saved_name(gpointer unused)
{
    (void)unused; JsonNode *view = overview(); JsonObject *user = user_find(view, "native_admin_operator");
    gboolean result = user && g_str_equal(radios_json_string(user, "display_name", ""), "Native İzinleri Güncellendi");
    json_node_free(view); return result;
}
static gboolean error_visible(gpointer unused)
{
    (void)unused; GtkWidget *d = dialog("admin-user-dialog"); if (!d) return FALSE;
    GtkWidget *status = find(d, "admin-user-status");
    return gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT)) &&
        *gtk_label_get_text(GTK_LABEL(status)) && !strstr(gtk_label_get_text(GTK_LABEL(status)), "kaydediliyor");
}
static void test_users(void)
{
    JsonNode *created = create_user("native_admin_operator");
    gchar *id = g_strdup(radios_json_string(json_node_get_object(created), "user_id", ""));
    g_assert_false(json_object_has_member(json_node_get_object(created), "token")); json_node_free(created);
    Harness *h = open_harness();
    GtkWidget *health = find(h->page, "admin-health-status");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(health)), ==, "");
    JsonNode *before = overview(); gint64 sequence = radios_json_int(json_node_get_object(before), "sequence", 0); json_node_free(before);
    monitor(h); JsonNode *after = overview();
    g_assert_cmpint(radios_json_int(json_node_get_object(after), "sequence", 0), ==, sequence); json_node_free(after);
    open_user(h, id); GtkWidget *d = dialog("admin-user-dialog");
    gtk_entry_set_text(GTK_ENTRY(find(d, "admin-user-name")), "Native İzinleri Güncellendi");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(d, "admin-permission-edit")), TRUE);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT); wait_for(dialog_closed, "admin-user-dialog", "actual CAS permission save");
    wait_for(saved_name, NULL, "user name stored in genuine DB");
    after = overview(); JsonObject *updated = user_find(after, "native_admin_operator");
    JsonArray *rights = json_object_get_array_member(updated, "local_permissions"); gboolean edit = FALSE;
    for (guint i = 0; i < json_array_get_length(rights); ++i) edit |= g_str_equal(json_array_get_string_element(rights, i), "edit");
    g_assert_true(edit); json_node_free(after);
    open_user(h, id); d = dialog("admin-user-dialog");
    after = overview(); updated = user_find(after, "native_admin_operator");
    JsonNode *payload = body(); JsonObject *record = json_node_get_object(payload);
    json_object_set_string_member(record, "user_id", id);
    json_object_set_int_member(record, "expected_revision", radios_json_int(updated, "revision", 0));
    json_object_set_string_member(record, "display_name", "Dışarıdan Güncellendi"); json_object_set_string_member(record, "status", "active");
    json_object_set_array_member(record, "permissions", json_array_ref(json_object_get_array_member(updated, "local_permissions"))); key(payload);
    JsonNode *external = request("desktop-admin/user-save", payload); json_node_free(external); json_node_free(payload); json_node_free(after);
    gtk_entry_set_text(GTK_ENTRY(find(d, "admin-user-name")), "Çakışan Girdi Korunmalı");
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT); wait_for(error_visible, NULL, "stale CAS edit is rejected visibly");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(d, "admin-user-name"))), ==, "Çakışan Girdi Korunmalı");
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    open_user(h, id); d = dialog("admin-user-dialog"); click(d, "admin-token-revoke");
    GtkWidget *token_dialog = dialog("admin-token-dialog"); g_assert_nonnull(token_dialog);
    gtk_dialog_response(GTK_DIALOG(token_dialog), GTK_RESPONSE_ACCEPT); wait_for(dialog_closed, "admin-token-dialog", "actual token CAS revoke");
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    after = overview(); updated = user_find(after, "native_admin_operator");
    JsonArray *tokens = json_object_get_array_member(updated, "tokens"); g_assert_cmpuint(json_array_get_length(tokens), ==, 1);
    g_assert_true(*radios_json_string(json_array_get_object_element(tokens, 0), "revoked_at", "")); json_node_free(after);
    close_harness(h); g_free(id);
}
static gboolean run_button_ready(gpointer root) { return find(root, "admin-run-close") != NULL; }
static void test_interrupted_run(void)
{
    JsonNode *payload = body(); JsonObject *record = json_node_get_object(payload);
    json_object_set_string_member(record, "kind", "record"); json_object_set_string_member(record, "name", "Native Kesintili Kayıt");
    JsonObject *configuration = json_object_new(); json_object_set_string_member(configuration, "filename", "native-kesintili.wav");
    json_object_set_object_member(record, "config", configuration); key(payload);
    JsonNode *profile = request("desktop-broadcast/profile-save", payload); json_node_free(payload);
    payload = body(); record = json_node_get_object(payload); JsonObject *saved = json_node_get_object(profile);
    if (json_object_has_member(saved, "record")) saved = json_object_get_object_member(saved, "record");
    json_object_set_string_member(record, "kind", "record");
    json_object_set_string_member(record, "profile_id", radios_json_string(saved, "_id", ""));
    json_object_set_int_member(record, "expected_revision", radios_json_int(saved, "_rev", 0)); key(payload);
    JsonNode *started = request("desktop-broadcast/run-start", payload); json_node_free(payload); json_node_free(profile);
    gchar *run_id = g_strdup(radios_json_string(json_node_get_object(started), "run_id", "")); json_node_free(started);
    Harness *h = open_harness(); monitor(h); wait_for(run_button_ready, h->page, "nonterminal run shown as possibly live");
    click(h->page, "admin-run-close"); GtkWidget *d = dialog("admin-run-dialog"); g_assert_nonnull(d);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT); pump(); g_assert_nonnull(dialog("admin-run-dialog"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(d, "admin-run-confirm")), TRUE);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT); wait_for(dialog_closed, "admin-run-dialog", "explicit interrupted unknown-tail closure");
    JsonNode *view = overview(); JsonArray *open = json_object_get_array_member(json_node_get_object(view), "recoverable_runs");
    for (guint i = 0; i < json_array_get_length(open); ++i)
        g_assert_cmpstr(radios_json_string(json_array_get_object_element(open, i), "run_id", ""), !=, run_id);
    JsonArray *recovered = json_object_get_array_member(json_node_get_object(view), "recovered_runs"); gboolean found = FALSE;
    for (guint i = 0; i < json_array_get_length(recovered); ++i) {
        JsonObject *row = json_array_get_object_element(recovered, i);
        if (g_str_equal(radios_json_string(row, "run_id", ""), run_id)) {
            found = TRUE; JsonObject *recovery = json_object_get_object_member(row, "recovery");
            g_assert_true(json_object_get_boolean_member(recovery, "unknown_tail"));
            g_assert_false(json_object_get_boolean_member(row, "delivery_proven"));
        }
    }
    g_assert_true(found); json_node_free(view); close_harness(h); g_free(run_id);
}
typedef struct { GtkWidget *root; const gchar *text; } Text;
static gboolean tool_status(gpointer value)
{
    Text *wait = value; GtkWidget *status = find(wait->root, "admin-backup-status");
    return status && strstr(gtk_label_get_text(GTK_LABEL(status)), wait->text) != NULL;
}
static void test_backup_view_denied(void)
{
    g_assert_nonnull(g_getenv("RADIOS_TEST_VIEW_TOKEN"));
    gchar *original_environment = config.token_environment;
    config.token_environment = g_strdup("RADIOS_TEST_VIEW_TOKEN");
    Harness *h = open_harness();
    RadioSDataClient *viewer = radios_data_client_acquire(&h->ctx); g_assert_nonnull(viewer);
    JsonObject *actual_user = json_object_get_object_member(radios_data_context(&h->ctx), "user");
    g_assert_cmpstr(radios_data_client_user_id(viewer), ==, radios_json_string(actual_user, "_id", ""));
    g_assert_cmpstr(radios_data_client_user_id(viewer), !=, radios_data_client_user_id(reader));
    JsonNode *viewer_recovery = radios_data_recovery_status(&h->ctx);
    g_assert_cmpint(radios_json_int(json_node_get_object(viewer_recovery), "blocked", -1), ==, 0);
    json_node_free(viewer_recovery); radios_data_client_unref(viewer);
    GtkWidget *expander = find(h->page, "admin-monitor-expander"); gtk_expander_set_expanded(GTK_EXPANDER(expander), TRUE);
    gchar *hold = g_build_filename(config.project_root, ".acceptance-tool-hold", NULL);
    gchar *ready = g_build_filename(config.project_root, ".acceptance-tool-ready", NULL);
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL)); g_chmod(hold, 0600);
    click(h->page, "admin-backup-create");
    gint64 deadline = g_get_monotonic_time() + 10000000;
    while (g_get_monotonic_time() < deadline) {
        const gchar *status = gtk_label_get_text(GTK_LABEL(find(h->page, "admin-backup-status")));
        if (!strstr(status, "doğrulanıyor")) break;
        pump();
    }
    const gchar *status = gtk_label_get_text(GTK_LABEL(find(h->page, "admin-backup-status")));
    g_assert_null(strstr(status, "doğrulanıyor")); g_assert_false(g_file_test(ready, G_FILE_TEST_EXISTS));
    close_harness(h); g_unlink(hold); g_unlink(ready); g_free(hold); g_free(ready);
    g_free(config.token_environment); config.token_environment = original_environment;

}
static gboolean backups_ready(gpointer root)
{ return gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(root, "admin-backup-selector"))) != NULL; }
static void test_backup(void)
{
    Harness *h = open_harness(); monitor(h);
    JsonNode *before = overview(); gint64 sequence = radios_json_int(json_node_get_object(before), "sequence", 0); json_node_free(before);
    click(h->page, "admin-backup-create"); Text wait = {h->page, "created"}; wait_for(tool_status, &wait, "real private backup creation");
    click(h->page, "admin-backup-list"); wait_for(backups_ready, h->page, "verified backup selector");
    click(h->page, "admin-backup-verify"); wait.text = "verified"; wait_for(tool_status, &wait, "backup manifest/hash identity verification");
    g_assert_cmpstr(g_object_get_data(G_OBJECT(h->ctx.parent), "radios-backup-summary"), ==, "Doğrulandı");
    g_assert_nonnull(strstr(g_object_get_data(G_OBJECT(h->ctx.parent), "radios-backup-summary-detail"), "Son kontrol:"));
    click(h->page, "admin-backup-restore-stage"); wait.text = "Etkinleştirilmedi"; wait_for(tool_status, &wait, "restored copy integrity preview");
    click(h->page, "admin-backup-restore-plan"); wait.text = "--confirm-store-id"; wait_for(tool_status, &wait, "reviewable explicit offline restore command");
    JsonNode *after = overview(); g_assert_cmpint(radios_json_int(json_node_get_object(after), "sequence", 0), ==, sequence); json_node_free(after);
    close_harness(h);
}
static void test_owner_close(void)
{
    Harness *h = open_harness(); monitor(h);
    gchar *hold = g_build_filename(config.project_root, ".acceptance-tool-hold", NULL);
    gchar *ready = g_build_filename(config.project_root, ".acceptance-tool-ready", NULL);
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL)); g_chmod(hold, 0600);
    click(h->page, "admin-backup-list");
    gint64 deadline = g_get_monotonic_time() + 10000000;
    while (!g_file_test(ready, G_FILE_TEST_IS_REGULAR) && g_get_monotonic_time() < deadline) pump();
    g_assert_true(g_file_test(ready, G_FILE_TEST_IS_REGULAR)); close_harness(h);
    g_unlink(hold); g_unlink(ready); g_free(hold); g_free(ready);
    for (guint i = 0; i < 200; ++i) pump();
    h = open_harness(); monitor(h);
    g_assert_nonnull(find(h->page, "admin-recovery-retry"));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(h->page, "admin-recovery-status"))), "Dayanıklı işlem kuyruğu:"));
    close_harness(h);
}
static guint children(GtkWidget *container)
{
    GList *list = gtk_container_get_children(GTK_CONTAINER(container)); guint count = g_list_length(list);
    g_list_free(list); return count;
}
static gboolean token_shown(gpointer rows)
{
    GtkWidget *d = dialog("admin-user-dialog"); if (!d) return FALSE;
    return *gtk_label_get_text(GTK_LABEL(find(d, "admin-token-secret"))) &&
        children(find(d, "admin-token-list")) == GPOINTER_TO_UINT(rows) && gtk_widget_is_sensitive(find(d, "admin-token-issue"));
}
/* Authenticates with the shown key over a plain socket, so the native client's
 * installation identity is never swapped inside this threaded process. */
static gchar *credential_user(const gchar *secret)
{
    GError *error = NULL; GSocketClient *client = g_socket_client_new(); g_socket_client_set_enable_proxy(client, FALSE);
    GSocketConnection *connection = g_socket_client_connect_to_uri(client, config.service_url, 80, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(connection);
    gchar *payload = g_strdup_printf("{\"station_id\":\"%s\"}", radios_data_client_station_id(reader));
    gchar *request = g_strdup_printf("POST /v1/workflows/desktop/context HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Authorization: Bearer %s\r\nAccept: application/json\r\nContent-Type: application/json\r\n"
        "Content-Length: %" G_GSIZE_FORMAT "\r\nConnection: close\r\n\r\n%s", secret, strlen(payload), payload);
    g_assert_true(g_output_stream_write_all(g_io_stream_get_output_stream(G_IO_STREAM(connection)),
        request, strlen(request), NULL, NULL, &error)); g_assert_no_error(error);
    GByteArray *response = g_byte_array_new(); guint8 chunk[4096]; gssize got;
    while ((got = g_input_stream_read(g_io_stream_get_input_stream(G_IO_STREAM(connection)), chunk, sizeof chunk, NULL, &error)) > 0)
        g_byte_array_append(response, chunk, (guint)got);
    g_assert_no_error(error); g_byte_array_append(response, (const guint8 *)"", 1);
    const gchar *text = (const gchar *)response->data, *json = strstr(text, "\r\n\r\n");
    g_assert_true(g_str_has_prefix(text, "HTTP/1.1 200 ")); g_assert_nonnull(json);
    JsonParser *parser = json_parser_new(); g_assert_true(json_parser_load_from_data(parser, json + 4, -1, &error)); g_assert_no_error(error);
    JsonObject *user = json_object_get_object_member(json_node_get_object(json_parser_get_root(parser)), "user");
    gchar *result = g_strdup(radios_json_string(user, "_id", ""));
    g_object_unref(parser); g_byte_array_unref(response); g_free(request); g_free(payload);
    g_object_unref(connection); g_object_unref(client); return result;
}
static void test_token_issue(void)
{
    JsonNode *created = create_user("native_admin_anahtar");
    gchar *id = g_strdup(radios_json_string(json_node_get_object(created), "user_id", "")); json_node_free(created);
    Harness *h = open_harness(); open_user(h, id);
    GtkWidget *d = dialog("admin-user-dialog"), *secret = find(d, "admin-token-secret");
    GtkSpinButton *days = GTK_SPIN_BUTTON(find(d, "admin-token-days")); gdouble low = 0, high = 0;
    gtk_spin_button_get_range(days, &low, &high);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(days), ==, 30); g_assert_cmpfloat(low, ==, 1); g_assert_cmpfloat(high, ==, 365);
    g_assert_cmpuint(children(find(d, "admin-token-list")), ==, 1);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(secret)), ==, ""); g_assert_false(gtk_widget_is_visible(secret));
    gtk_spin_button_set_value(days, 7); click(d, "admin-token-issue");
    wait_for(token_shown, GUINT_TO_POINTER(2), "one-time key shown and genuine key list reloaded");
    gchar *token = g_strdup(gtk_label_get_text(GTK_LABEL(secret)));
    g_assert_cmpuint(strlen(token), >=, 32); g_assert_true(gtk_label_get_selectable(GTK_LABEL(secret)));
    g_assert_true(gtk_widget_is_visible(secret)); g_assert_cmpstr(last_notice, ==, "Yeni erişim anahtarı verildi.");
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(d, "admin-user-status"))), "Bitiş: "));
    click(d, "admin-token-copy");
    gchar *copied = gtk_clipboard_wait_for_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD));
    g_assert_cmpstr(copied, ==, token); g_free(copied);
    JsonNode *view = overview(); JsonObject *user = user_find(view, "native_admin_anahtar"); g_assert_nonnull(user);
    JsonArray *tokens = json_object_get_array_member(user, "tokens"); g_assert_cmpuint(json_array_get_length(tokens), ==, 2);
    GDateTime *now = g_date_time_new_now_utc(); guint week = 0;
    for (guint i = 0; i < json_array_get_length(tokens); ++i) {
        JsonObject *row = json_array_get_object_element(tokens, i);
        GDateTime *expiry = g_date_time_new_from_iso8601(radios_json_string(row, "expires_at", ""), NULL);
        g_assert_nonnull(expiry); g_assert_false(*radios_json_string(row, "revoked_at", ""));
        GTimeSpan left = g_date_time_difference(expiry, now);
        if (left > 6 * G_TIME_SPAN_DAY && left <= 7 * G_TIME_SPAN_DAY) ++week;
        g_date_time_unref(expiry);
    }
    g_assert_cmpuint(week, ==, 1); g_date_time_unref(now); json_node_free(view);
    gchar *owner = credential_user(token); g_assert_cmpstr(owner, ==, id); g_free(owner); g_free(token);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL); wait_for(dialog_closed, "admin-user-dialog", "closed after key delivery");
    close_harness(h); g_free(id);
}
static gboolean database_read(gpointer root)
{
    return g_str_has_prefix(gtk_label_get_text(GTK_LABEL(find(root, "admin-db-state"))), "Okundu: ") &&
        gtk_widget_is_sensitive(find(root, "admin-db-refresh"));
}
static void test_database_status(void)
{
    Harness *h = open_harness(); monitor(h);
    GtkWidget *metrics = find(h->page, "admin-db-metrics"), *integrity = find(h->page, "admin-db-integrity");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(integrity)), ==, ""); g_assert_false(gtk_widget_get_visible(integrity));
    JsonNode *before = overview(); gint64 sequence = radios_json_int(json_node_get_object(before), "sequence", 0); json_node_free(before);
    click(h->page, "admin-db-refresh"); g_assert_false(gtk_widget_is_sensitive(find(h->page, "admin-db-refresh")));
    wait_for(database_read, h->page, "engine metrics and bounded integrity scrub");
    const gchar *verdict = gtk_label_get_text(GTK_LABEL(integrity)), *facts = gtk_label_get_text(GTK_LABEL(metrics));
    g_assert_true(g_str_has_prefix(verdict, "Bütünlük: Sağlam")); g_assert_nonnull(strstr(verdict, "Depolama: geçti"));
    g_assert_nonnull(strstr(verdict, "Kayıtlar: geçti")); g_assert_nonnull(strstr(verdict, "Denetlenen: "));
    g_assert_nonnull(strstr(facts, "Kayıtlar: ")); g_assert_nonnull(strstr(facts, "Günlük: "));
    g_assert_nonnull(strstr(facts, "İşlem sırası: ")); g_assert_nonnull(strstr(facts, "Kullanıcı: "));
    g_assert_true(gtk_widget_get_visible(metrics) && gtk_widget_get_visible(integrity));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(h->page, "admin-db-issues"))), ==, "");
    JsonNode *after = overview(); g_assert_cmpint(radios_json_int(json_node_get_object(after), "sequence", 0), ==, sequence); json_node_free(after);
    close_harness(h);
}
static gboolean database_denied(gpointer root)
{
    return strstr(gtk_label_get_text(GTK_LABEL(find(root, "admin-db-state"))), "genel yönetim yetkisi") &&
        gtk_widget_is_sensitive(find(root, "admin-db-refresh"));
}
static void test_database_status_denied(void)
{
    g_assert_nonnull(g_getenv("RADIOS_TEST_VIEW_TOKEN"));
    gchar *original_environment = config.token_environment;
    config.token_environment = g_strdup("RADIOS_TEST_VIEW_TOKEN");
    Harness *h = open_harness();
    gtk_expander_set_expanded(GTK_EXPANDER(find(h->page, "admin-monitor-expander")), TRUE);
    click(h->page, "admin-db-refresh"); wait_for(database_denied, h->page, "engine status needs global manage");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(h->page, "admin-db-metrics"))), ==, "");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(h->page, "admin-db-integrity"))), ==, "");
    close_harness(h); g_free(config.token_environment); config.token_environment = original_environment;
}
static gboolean full_check_running(gpointer root)
{ return g_str_has_prefix(gtk_label_get_text(GTK_LABEL(find(root, "admin-db-state"))), "Tam denetim sürüyor · "); }
static gboolean full_check_done(gpointer root)
{
    return g_str_has_prefix(gtk_label_get_text(GTK_LABEL(find(root, "admin-db-state"))), "Tam denetim bitti: ") &&
        gtk_widget_is_sensitive(find(root, "admin-db-full-check"));
}
static gboolean full_check_denied(gpointer root)
{
    return strstr(gtk_label_get_text(GTK_LABEL(find(root, "admin-db-state"))), "genel yönetim yetkisi") &&
        gtk_widget_is_sensitive(find(root, "admin-db-full-check"));
}
static void test_database_full_check(void)
{
    Harness *h = open_harness(); monitor(h);
    GtkWidget *state = find(h->page, "admin-db-state"), *integrity = find(h->page, "admin-db-integrity");
    GtkWidget *full = find(h->page, "admin-db-full-check"), *refresh = find(h->page, "admin-db-refresh");
    JsonNode *before = overview(); gint64 sequence = radios_json_int(json_node_get_object(before), "sequence", 0); json_node_free(before);
    click(h->page, "admin-db-full-check"); GtkWidget *d = dialog("admin-db-full-dialog"); g_assert_nonnull(d);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(d, "admin-db-full-warning"))), "medya dosyalarını"));
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL);
    /* Starting sets the status line at once; declining leaves the section untouched. */
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(state)), ==, ""); g_assert_false(gtk_widget_get_visible(state));
    wait_for(dialog_closed, "admin-db-full-dialog", "declined full check closes its confirmation");
    for (guint i = 0; i < 100; ++i) pump();
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(state)), ==, ""); g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(integrity)), ==, "");
    g_assert_true(gtk_widget_is_sensitive(full) && gtk_widget_is_sensitive(refresh));
    click(h->page, "admin-db-full-check"); d = dialog("admin-db-full-dialog"); g_assert_nonnull(d);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    g_assert_true(g_str_has_prefix(gtk_label_get_text(GTK_LABEL(state)), "Tam denetim başlatılıyor"));
    g_assert_false(gtk_widget_is_sensitive(full)); g_assert_false(gtk_widget_is_sensitive(refresh));
    wait_for(full_check_running, h->page, "background job accepted with elapsed time");
    g_assert_false(gtk_widget_is_sensitive(full));
    wait_for(full_check_done, h->page, "polled job shows the whole-store scrub");
    const gchar *verdict = gtk_label_get_text(GTK_LABEL(integrity));
    g_assert_true(g_str_has_prefix(verdict, "Tam denetim: Sağlam")); g_assert_null(strstr(verdict, "tamamlanmadı"));
    g_assert_nonnull(strstr(verdict, "Medya dosyaları: geçti")); g_assert_nonnull(strstr(verdict, "Soğuk arşiv: geçti"));
    g_assert_nonnull(strstr(verdict, "Kayıtlar: geçti")); g_assert_nonnull(strstr(verdict, "Denetlenen: "));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(state)), " · Süre: "));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(h->page, "admin-db-issues"))), ==, "");
    g_assert_true(gtk_widget_is_sensitive(refresh) && gtk_widget_get_visible(integrity));
    JsonNode *after = overview(); g_assert_cmpint(radios_json_int(json_node_get_object(after), "sequence", 0), ==, sequence); json_node_free(after);
    close_harness(h);
    gchar *original_environment = config.token_environment;
    config.token_environment = g_strdup("RADIOS_TEST_VIEW_TOKEN");
    h = open_harness(); gtk_expander_set_expanded(GTK_EXPANDER(find(h->page, "admin-monitor-expander")), TRUE);
    click(h->page, "admin-db-full-check"); d = dialog("admin-db-full-dialog"); g_assert_nonnull(d);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT); wait_for(full_check_denied, h->page, "full check needs global manage");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(h->page, "admin-db-integrity"))), ==, "");
    close_harness(h); g_free(config.token_environment); config.token_environment = original_environment;
}
static gboolean label_has(GtkWidget *root, const gchar *text)
{
    if (GTK_IS_LABEL(root) && strstr(gtk_label_get_text(GTK_LABEL(root)), text)) return TRUE;
    if (!GTK_IS_CONTAINER(root)) return FALSE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); gboolean result = FALSE;
    for (GList *item = children; item && !result; item = item->next) result = label_has(item->data, text);
    g_list_free(children); return result;
}
static gboolean token_list_revoked(gpointer unused)
{
    (void)unused; GtkWidget *d = dialog("admin-user-dialog"); if (!d) return FALSE;
    GtkWidget *list = find(d, "admin-token-list"), *revoke = find(list, "admin-token-revoke");
    return revoke && !gtk_widget_is_sensitive(revoke) && label_has(list, "İptal edilmiş");
}
static void test_token_revoke_refresh(void)
{
    JsonNode *created = create_user("native_admin_iptal");
    gchar *id = g_strdup(radios_json_string(json_node_get_object(created), "user_id", "")); json_node_free(created);
    Harness *h = open_harness(); open_user(h, id);
    GtkWidget *d = dialog("admin-user-dialog"), *list = find(d, "admin-token-list");
    g_assert_cmpuint(children(list), ==, 1); g_assert_true(label_has(list, "Etkin"));
    click(d, "admin-token-revoke"); GtkWidget *token_dialog = dialog("admin-token-dialog"); g_assert_nonnull(token_dialog);
    gtk_dialog_response(GTK_DIALOG(token_dialog), GTK_RESPONSE_ACCEPT); wait_for(dialog_closed, "admin-token-dialog", "actual token CAS revoke");
    g_assert_true(dialog("admin-user-dialog") == d);
    wait_for(token_list_revoked, NULL, "open person window reloads its key list after a revoke");
    g_assert_cmpuint(children(list), ==, 1); g_assert_false(label_has(list, "Etkin"));
    JsonNode *view = overview(); JsonObject *user = user_find(view, "native_admin_iptal"); g_assert_nonnull(user);
    JsonArray *tokens = json_object_get_array_member(user, "tokens"); g_assert_cmpuint(json_array_get_length(tokens), ==, 1);
    g_assert_true(*radios_json_string(json_array_get_object_element(tokens, 0), "revoked_at", "")); json_node_free(view);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL); wait_for(dialog_closed, "admin-user-dialog", "closed after revoke");
    close_harness(h); g_free(id);
}
static gboolean outbox_recovered(gpointer value)
{
    Harness *h = value; JsonNode *status = radios_data_recovery_status(&h->ctx);
    gboolean result = status && radios_json_int(json_node_get_object(status), "pending", -1) == 0 &&
        radios_json_int(json_node_get_object(status), "blocked", -1) == recovery_blocked;
    g_clear_pointer(&status, json_node_free); return result;
}
static void test_recovery_summary(void)
{
    RadioSRecovery *outbox = radios_recovery_new(&config, radios_data_client_store_id(reader),
        radios_data_client_station_id(reader), radios_data_client_user_id(reader), radios_data_client_endpoint_id(reader));
    JsonNode *baseline = radios_recovery_status(outbox, NULL);
    recovery_blocked = radios_json_int(json_node_get_object(baseline), "blocked", 0) + 1; json_node_free(baseline);
    JsonNode *blocked = body(); key(blocked); GError *error = NULL;
    g_assert_true(radios_recovery_prepare(outbox, "POST", "/v1/workflows/desktop/put", blocked, &error)); g_assert_no_error(error);
    g_assert_true(radios_recovery_finish(outbox, blocked, FALSE, RADIOS_DATA_CLIENT_ERROR_CONFLICT, &error)); g_assert_no_error(error);
    JsonNode *pending = body(); key(pending); JsonObject *record = json_node_get_object(pending);
    json_object_set_string_member(record, "kind", "server"); json_object_set_string_member(record, "name", "Native Kurtarılan Sunucu");
    JsonObject *settings = json_object_new();
    json_object_set_string_member(settings, "architecture", "local"); json_object_set_string_member(settings, "role", "management");
    json_object_set_string_member(settings, "target_name", "GUI Kabul İstasyonu");
    json_object_set_string_member(settings, "address", "https://recovery.example.invalid");
    json_object_set_object_member(record, "config", settings);
    Harness *h = open_harness();
    g_assert_true(radios_recovery_prepare(outbox, "POST", "/v1/workflows/desktop/put", pending, &error)); g_assert_no_error(error);
    monitor(h);
    const gchar *text = gtk_label_get_text(GTK_LABEL(find(h->page, "admin-recovery-status")));
    gchar *blocked_text = g_strdup_printf("%" G_GINT64_FORMAT " inceleme gereken", recovery_blocked);
    g_assert_nonnull(strstr(text, "1 bekleyen")); g_assert_nonnull(strstr(text, blocked_text));
    g_assert_nonnull(strstr(text, radios_json_string(json_node_get_object(blocked), "idempotency_key", "")));
    click(h->page, "admin-recovery-retry"); wait_for(outbox_recovered, h, "exact pending body acknowledged while rejected body stays blocked");
    click(h->page, "admin-refresh");
    text = gtk_label_get_text(GTK_LABEL(find(h->page, "admin-recovery-status")));
    g_assert_nonnull(strstr(text, "0 bekleyen")); g_assert_nonnull(strstr(text, blocked_text)); g_free(blocked_text);
    g_assert_true(radios_recovery_finish(outbox, blocked, TRUE, 0, &error)); g_assert_no_error(error);
    close_harness(h); radios_recovery_free(outbox); json_node_free(blocked); json_node_free(pending);
}
/* Kılavuz 18.10 R10.1: Teknik › "+ Kişi ata" creates the user through
 * desktop-admin/user-create. Its reply carries no bearer key; the dialog
 * names only the owner-private key file, which is mode 0600. */
static GtkWidget *labelled_button(GtkWidget *root, const gchar *text)
{
    if (GTK_IS_BUTTON(root) && !g_strcmp0(gtk_button_get_label(GTK_BUTTON(root)), text)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = labelled_button(item->data, text);
    g_list_free(children); return result;
}
static const gchar *person_message(void)
{
    GtkWidget *d = dialog("technical-person-dialog"); GtkWidget *form = d ? g_object_get_data(G_OBJECT(d), "technical-form") : NULL;
    GtkWidget *status = form ? g_object_get_data(G_OBJECT(form), "technical-status-label") : NULL;
    return status ? gtk_label_get_text(GTK_LABEL(status)) : "";
}
static gboolean person_created(gpointer unused)
{ (void)unused; return g_str_has_prefix(person_message(), "Kullanıcı ve istasyon izinleri kaydedildi."); }
static void test_person_create(void)
{
    Harness *h = open_harness();
    GtkWidget *people = find(h->page, "technical-people-pane");
    gtk_stack_set_visible_child_name(GTK_STACK(gtk_widget_get_parent(people)), "people");
    GtkWidget *add = labelled_button(people, "+ Kişi ata"); g_assert_nonnull(add); gtk_button_clicked(GTK_BUTTON(add));
    wait_for(dialog_open, "technical-person-dialog", "person dialog");
    GtkWidget *d = dialog("technical-person-dialog");
    gtk_entry_set_text(GTK_ENTRY(find(d, "technical-person-name")), "Native Kişi Ata");
    gtk_entry_set_text(GTK_ENTRY(find(d, "technical-person-login")), "native_person_create");
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    wait_for(person_created, NULL, "user created through desktop-admin/user-create");
    g_assert_false(gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT)));
    gchar *folder = g_path_get_dirname(config.database_file), *keys = g_build_filename(folder, "private-user-keys", NULL);
    const gchar *file = strstr(person_message(), keys); g_assert_nonnull(file);
    GStatBuf info; g_assert_cmpint(g_stat(file, &info), ==, 0); g_assert_cmpint(info.st_mode & 0777, ==, 0600);
    JsonNode *view = overview(); JsonObject *user = user_find(view, "native_person_create"); g_assert_nonnull(user);
    g_assert_cmpstr(radios_json_string(user, "display_name", ""), ==, "Native Kişi Ata"); json_node_free(view);
    g_free(keys); g_free(folder);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL); close_harness(h);
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]);
    for (gint i = 1; i < argc - 1; ++i) argv[i] = argv[i + 1];
    --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv); GError *error = NULL;
    g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/native-admin/users-cas-revoke", test_users);
    g_test_add_func("/native-admin/interrupted-run", test_interrupted_run);
    g_test_add_func("/native-admin/backup-preview", test_backup);
    g_test_add_func("/native-admin/owner-close", test_owner_close);
    g_test_add_func("/native-admin/recovery-summary", test_recovery_summary);
    g_test_add_func("/native-admin/backup-view-denied", test_backup_view_denied);
    g_test_add_func("/native-admin/token-issue", test_token_issue);
    g_test_add_func("/native-admin/database-status", test_database_status);
    g_test_add_func("/native-admin/database-status-denied", test_database_status_denied);
    g_test_add_func("/native-admin/person-create-private-key", test_person_create);
    g_test_add_func("/native-admin/database-full-check", test_database_full_check);
    g_test_add_func("/native-admin/token-revoke-refreshes-list", test_token_revoke_refresh);
    gint result = g_test_run(); radios_data_client_unref(reader); radios_config_clear(&config); g_free(last_notice); return result;
}
