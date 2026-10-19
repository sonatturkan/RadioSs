#include "radios/admin_ui.h"
#include "radios/widgets.h"
#include "radios/audio_ui.h"
#include "radios/broadcast_ui.h"
#include <stdarg.h>
#include <string.h>

typedef struct {
    gint references;
    RadioSViewContext *ctx;
    GWeakRef page;
    GtkWidget *expander, *health, *audio, *recovery, *runs, *audit;
    GtkWidget *backups, *backup_status, *backup_actions;
    GtkWidget *db_refresh, *db_full, *db_state, *db_metrics, *db_integrity, *db_issues;
    JsonNode *overview;
    GSubprocess *child;
    GCancellable *cancel;
    guint poll, timeout, db_pending, db_job_poll, db_job_failures;
    gint64 db_job_started;
    gboolean closed, reading, again, authorizing, db_denied, db_job_starting, db_job_reading;
    gchar *edit_user, *db_error, *db_job;
} Admin;
/* The same scrub limits the administration overview uses: an on-demand check
 * must not replay a large live journal under the shared database lock. */
#define DATABASE_INTEGRITY_PATH "/v1/operations/integrity?max_bytes=67108864&max_records=50000"
/* The whole-store check runs as a data-service job polled once a second. */
#define DATABASE_FULL_START "/v1/workflows/desktop-admin/integrity-start"
#define DATABASE_FULL_JOB "/v1/workflows/desktop-admin/integrity-job"
#define DATABASE_FULL_FAILURES 5

static const gchar *permissions[] = {
    "view", "edit", "bulk_edit", "import", "approve", "activate", "operate", "repair", "manage"
};
static const gchar *permission_labels[] = {
    "İzleme", "Düzenleme", "Toplu düzenleme", "Medya içe aktarma", "Onay", "Etkinleştirme",
    "Yayın kontrolü", "Onarım", "Yönetim"
};
static void refresh(Admin *admin);
static gboolean poll(gpointer value);
static void user_editor(Admin *admin, JsonObject *user);
static void tokens_loaded(RadioSViewContext *ctx, GtkWidget *dialog, JsonNode *result,
    const GError *error, gpointer unused);
static Admin *admin_ref(Admin *admin) { g_atomic_int_inc(&admin->references); return admin; }
static void admin_unref(gpointer value)
{
    Admin *admin = value;
    if (!g_atomic_int_dec_and_test(&admin->references)) return;
    g_weak_ref_clear(&admin->page);
    g_clear_pointer(&admin->overview, json_node_free);
    g_clear_object(&admin->child); g_clear_object(&admin->cancel);
    g_free(admin->edit_user); g_free(admin->db_error); g_free(admin->db_job); g_free(admin);
}
static void pack(GtkWidget *parent, GtkWidget *child, gboolean expand)
{ gtk_box_pack_start(GTK_BOX(parent), child, expand, expand, 0); }
static GtkWidget *label(const gchar *text, const gchar *name)
{
    GtkWidget *widget = radios_ui_label(text, "technical-status");
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_xalign(GTK_LABEL(widget), 0.0f);
    if (name) gtk_widget_set_name(widget, name);
    return widget;
}
static gboolean boolean(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN && json_node_get_boolean(node);
}
static JsonArray *array(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static JsonObject *object(JsonObject *parent, const gchar *key)
{
    JsonNode *node = parent ? json_object_get_member(parent, key) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static gboolean contains(JsonArray *values, const gchar *text)
{
    for (guint i = 0; values && i < json_array_get_length(values); ++i) {
        JsonNode *node = json_array_get_element(values, i);
        if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING &&
            g_str_equal(json_node_get_string(node), text)) return TRUE;
    }
    return FALSE;
}
static void clear(GtkWidget *container)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(container));
    for (GList *item = children; item; item = item->next) gtk_widget_destroy(item->data);
    g_list_free(children);
}
static gboolean number(JsonObject *object, const gchar *key, gdouble *value)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    if (!node || !JSON_NODE_HOLDS_VALUE(node)) return FALSE;
    if (json_node_get_value_type(node) == G_TYPE_INT64) *value = (gdouble)json_node_get_int(node);
    else if (json_node_get_value_type(node) == G_TYPE_DOUBLE) *value = json_node_get_double(node);
    else return FALSE;
    return TRUE;
}
static void show_text(GtkWidget *widget, const gchar *text)
{ gtk_label_set_text(GTK_LABEL(widget), text); gtk_widget_set_visible(widget, *text != '\0'); }
static gchar *moment(const gchar *iso)
{
    GDateTime *parsed = *iso ? g_date_time_new_from_iso8601(iso, NULL) : NULL;
    if (!parsed) return g_strdup(iso);
    GDateTime *local = g_date_time_to_local(parsed);
    gchar *text = g_date_time_format(local, "%d.%m.%Y %H:%M");
    g_date_time_unref(local); g_date_time_unref(parsed); return text;
}
static gboolean expired(const gchar *iso)
{
    GDateTime *parsed = *iso ? g_date_time_new_from_iso8601(iso, NULL) : NULL;
    if (!parsed) return FALSE;
    GDateTime *now = g_date_time_new_now_utc(); gboolean result = g_date_time_compare(parsed, now) <= 0;
    g_date_time_unref(now); g_date_time_unref(parsed); return result;
}
static GtkWidget *button(GtkWidget *parent, const gchar *text, const gchar *name,
    GCallback callback, gpointer user)
{
    GtkWidget *widget = radios_ui_button(text);
    if (name) gtk_widget_set_name(widget, name);
    if (callback) g_signal_connect(widget, "clicked", callback, user);
    pack(parent, widget, FALSE); return widget;
}
static GtkWidget *dialog_new(Admin *admin, const gchar *title, const gchar *name,
    const gchar *accept, GtkWidget **form)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(title, admin->ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, "Vazgeç", GTK_RESPONSE_CANCEL,
        accept, GTK_RESPONSE_ACCEPT, NULL);
    gtk_widget_set_name(dialog, name);
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "technical-dialog");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "radios");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 600, 580);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    *form = radios_ui_box(GTK_ORIENTATION_VERTICAL, 9, "technical-form");
    gtk_container_set_border_width(GTK_CONTAINER(*form), 18);
    gtk_container_add(GTK_CONTAINER(scroll), *form);
    pack(gtk_dialog_get_content_area(GTK_DIALOG(dialog)), scroll, TRUE);
    g_object_set_data_full(G_OBJECT(dialog), "admin-controller", admin_ref(admin), admin_unref);
    return dialog;
}
static GtkWidget *field(GtkWidget *parent, const gchar *text, const gchar *name, GtkWidget *control)
{
    pack(parent, label(text, NULL), FALSE); gtk_widget_set_name(control, name);
    pack(parent, control, FALSE); return control;
}
static void write_key(GtkWidget *dialog, JsonObject *body)
{
    const gchar *key = g_object_get_data(G_OBJECT(dialog), "admin-write-key");
    if (!key) {
        g_object_set_data_full(G_OBJECT(dialog), "admin-write-key", g_uuid_string_random(), g_free);
        key = g_object_get_data(G_OBJECT(dialog), "admin-write-key");
    }
    json_object_set_string_member(body, "idempotency_key", key);
}
static void saved(RadioSViewContext *ctx, GtkWidget *dialog, JsonNode *result,
    const GError *error, gpointer unused)
{
    (void)ctx; (void)unused;
    Admin *admin = g_object_get_data(G_OBJECT(dialog), "admin-controller");
    GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "admin-status");
    GtkWidget *issue = g_object_get_data(G_OBJECT(dialog), "admin-token-issue");
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, TRUE);
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        if (issue) gtk_widget_set_sensitive(issue, TRUE);
        gtk_label_set_text(GTK_LABEL(status), error ? error->message : "Geçerli kayıt alındısı alınamadı; girdiler korunuyor.");
        return;
    }
    if (!admin->closed) { refresh(admin); radios_data_refresh(admin->ctx); }
    gtk_widget_destroy(dialog);
}
static void user_response(GtkDialog *dialog, gint response, gpointer unused)
{
    (void)unused;
    /* A key is being minted: closing now would lose its only display. */
    if (g_object_get_data(G_OBJECT(dialog), "admin-token-busy")) return;
    if (response != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    Admin *admin = g_object_get_data(G_OBJECT(dialog), "admin-controller");
    if (admin->closed) return;
    GtkWidget *name = g_object_get_data(G_OBJECT(dialog), "admin-user-name");
    GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "admin-status");
    gchar *display = g_strdup(gtk_entry_get_text(GTK_ENTRY(name)));
    if (!*g_strstrip(display)) {
        gtk_label_set_text(GTK_LABEL(status), "Kişi adı boş bırakılamaz."); g_free(display); return;
    }
    JsonObject *record = g_object_get_data(G_OBJECT(dialog), "admin-record");
    JsonNode *body = radios_data_body(admin->ctx); JsonObject *request = json_node_get_object(body);
    json_object_set_string_member(request, "user_id", radios_json_string(record, "user_id", ""));
    json_object_set_int_member(request, "expected_revision", radios_json_int(record, "revision", 0));
    json_object_set_string_member(request, "display_name", display); g_free(display);
    GtkWidget *state = g_object_get_data(G_OBJECT(dialog), "admin-user-state");
    json_object_set_string_member(request, "status", gtk_combo_box_get_active(GTK_COMBO_BOX(state)) == 1 ? "disabled" : "active");
    JsonArray *rights = json_array_new();
    for (guint i = 0; i < G_N_ELEMENTS(permissions); ++i) {
        GtkWidget *check = g_object_get_data(G_OBJECT(dialog), permissions[i]);
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check))) json_array_add_string_element(rights, permissions[i]);
    }
    json_object_set_array_member(request, "permissions", rights); write_key(GTK_WIDGET(dialog), request);
    if (radios_data_request(admin->ctx, GTK_WIDGET(dialog), "POST", "/v1/workflows/desktop-admin/user-save", body, saved, NULL, NULL)) {
        gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_ACCEPT, FALSE);
        gtk_widget_set_sensitive(g_object_get_data(G_OBJECT(dialog), "admin-token-issue"), FALSE);
        gtk_label_set_text(GTK_LABEL(status), "İzin değişikliği kaydediliyor…");
    } else gtk_label_set_text(GTK_LABEL(status), "Veritabanı bağlı değil; girdiler korunuyor.");
    json_node_free(body);
}
static void weak_free(gpointer value) { g_weak_ref_clear(value); g_free(value); }
static void token_revoked(RadioSViewContext *ctx, GtkWidget *dialog, JsonNode *result,
    const GError *error, gpointer unused)
{
    Admin *admin = g_object_get_data(G_OBJECT(dialog), "admin-controller");
    GWeakRef *owner = g_object_get_data(G_OBJECT(dialog), "admin-user-dialog");
    GtkWidget *person = owner ? g_weak_ref_get(owner) : NULL;
    /* The open person window lists the keys too: reload them as after issuing one. */
    if (!error && result && JSON_NODE_HOLDS_OBJECT(result) && person && !admin->closed &&
        !gtk_widget_in_destruction(person) && g_object_get_data(G_OBJECT(person), "admin-token-list")) {
        JsonNode *body = radios_data_body(admin->ctx);
        (void)radios_data_request(admin->ctx, person, "POST", "/v1/workflows/desktop-admin/overview", body, tokens_loaded, NULL, NULL);
        json_node_free(body);
    }
    g_clear_object(&person);
    saved(ctx, dialog, result, error, unused);
}
static void token_response(GtkDialog *dialog, gint response, gpointer unused)
{
    (void)unused;
    if (response != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    Admin *admin = g_object_get_data(G_OBJECT(dialog), "admin-controller");
    if (admin->closed) return;
    JsonObject *record = g_object_get_data(G_OBJECT(dialog), "admin-record");
    JsonNode *body = radios_data_body(admin->ctx); JsonObject *request = json_node_get_object(body);
    json_object_set_string_member(request, "token_id", radios_json_string(record, "token_id", ""));
    json_object_set_int_member(request, "expected_revision", radios_json_int(record, "revision", 0));
    write_key(GTK_WIDGET(dialog), request);
    if (radios_data_request(admin->ctx, GTK_WIDGET(dialog), "POST", "/v1/workflows/desktop-admin/token-revoke", body, token_revoked, NULL, NULL))
        gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_ACCEPT, FALSE);
    json_node_free(body);
}
static void token_revoke(GtkButton *button_widget, gpointer user)
{
    Admin *admin = user; if (admin->closed) return;
    JsonObject *record = g_object_get_data(G_OBJECT(button_widget), "admin-record");
    GtkWidget *form, *dialog = dialog_new(admin, "Erişim anahtarını iptal et", "admin-token-dialog", "Anahtarı iptal et", &form);
    GWeakRef *person = g_new0(GWeakRef, 1);
    g_weak_ref_init(person, gtk_widget_get_toplevel(GTK_WIDGET(button_widget)));
    g_object_set_data_full(G_OBJECT(dialog), "admin-user-dialog", person, weak_free);
    pack(form, label(radios_json_string(record, "token_id", ""), NULL), FALSE);
    GtkWidget *status = label("", "admin-token-status"); pack(form, status, FALSE);
    g_object_set_data(G_OBJECT(dialog), "admin-status", status);
    g_object_set_data_full(G_OBJECT(dialog), "admin-record", json_object_ref(record), (GDestroyNotify)json_object_unref);
    g_signal_connect(dialog, "response", G_CALLBACK(token_response), NULL); gtk_widget_show_all(dialog);
}
static void token_rows(Admin *admin, GtkWidget *list, JsonArray *tokens, gboolean editable)
{
    for (guint i = 0; tokens && i < json_array_get_length(tokens); ++i) {
        JsonObject *token = json_array_get_object_element(tokens, i);
        const gchar *scope = radios_json_string(token, "scope", ""), *expiry = radios_json_string(token, "expires_at", "");
        gboolean revoked = *radios_json_string(token, "revoked_at", "") != '\0';
        GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "technical-actions");
        gchar *until = moment(expiry);
        gchar *text = g_strdup_printf("%s · %s · Bitiş: %s", g_str_equal(scope, "user") ? "Kullanıcı anahtarı" :
            g_str_equal(scope, "endpoint") ? "Uç nokta anahtarı" : g_str_equal(scope, "monitor") ? "İzleme anahtarı" : scope,
            revoked ? "İptal edilmiş" : expired(expiry) ? "Süresi dolmuş" : "Etkin", until);
        pack(row, label(text, NULL), TRUE); g_free(text); g_free(until);
        GtkWidget *revoke = button(row, "İptal et", "admin-token-revoke", G_CALLBACK(token_revoke), admin);
        gtk_widget_set_sensitive(revoke, editable && !revoked);
        g_object_set_data_full(G_OBJECT(revoke), "admin-record", json_object_ref(token), (GDestroyNotify)json_object_unref);
        pack(list, row, FALSE);
    }
}
static void tokens_loaded(RadioSViewContext *ctx, GtkWidget *dialog, JsonNode *result,
    const GError *error, gpointer unused)
{
    (void)ctx; (void)unused;
    Admin *admin = g_object_get_data(G_OBJECT(dialog), "admin-controller");
    JsonObject *record = g_object_get_data(G_OBJECT(dialog), "admin-record");
    GtkWidget *list = g_object_get_data(G_OBJECT(dialog), "admin-token-list");
    if (admin->closed || error || !result || !JSON_NODE_HOLDS_OBJECT(result)) return;
    JsonArray *users = array(json_node_get_object(result), "users");
    for (guint i = 0; users && i < json_array_get_length(users); ++i) {
        JsonObject *user = json_array_get_object_element(users, i);
        if (!g_str_equal(radios_json_string(user, "user_id", ""), radios_json_string(record, "user_id", ""))) continue;
        /* Only the key list is replaced; the form keeps its opening CAS baseline. */
        clear(list); token_rows(admin, list, array(user, "tokens"), boolean(record, "editable"));
        gtk_widget_show_all(list); break;
    }
}
static void token_issued(RadioSViewContext *ctx, GtkWidget *dialog, JsonNode *result,
    const GError *error, gpointer unused)
{
    (void)unused;
    Admin *admin = g_object_get_data(G_OBJECT(dialog), "admin-controller");
    JsonObject *record = g_object_get_data(G_OBJECT(dialog), "admin-record");
    GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "admin-status");
    g_object_set_data(G_OBJECT(dialog), "admin-token-busy", NULL);
    gtk_widget_set_sensitive(g_object_get_data(G_OBJECT(dialog), "admin-token-issue"), TRUE);
    gtk_widget_set_sensitive(g_object_get_data(G_OBJECT(dialog), "admin-token-days"), TRUE);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL, TRUE);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, boolean(record, "editable"));
    JsonObject *issued = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    const gchar *token = radios_json_string(issued, "token", "");
    if (error || !*token) {
        gchar *message = g_strdup_printf("Anahtar verilemedi. %s", error ? error->message : "Veri servisi anahtar döndürmedi.");
        gtk_label_set_text(GTK_LABEL(status), message); g_free(message); return;
    }
    GtkWidget *reveal = g_object_get_data(G_OBJECT(dialog), "admin-token-reveal");
    gtk_label_set_text(GTK_LABEL(g_object_get_data(G_OBJECT(dialog), "admin-token-secret")), token);
    gtk_widget_set_no_show_all(reveal, FALSE); gtk_widget_show_all(reveal);
    gchar *until = moment(radios_json_string(issued, "expires_at", ""));
    gchar *message = g_strdup_printf("Yeni erişim anahtarı verildi · Bitiş: %s", until);
    gtk_label_set_text(GTK_LABEL(status), message); g_free(message); g_free(until);
    if (ctx->notify) ctx->notify(ctx->notify_data, "Yeni erişim anahtarı verildi.");
    if (!admin->closed) {
        JsonNode *body = radios_data_body(admin->ctx);
        (void)radios_data_request(admin->ctx, dialog, "POST", "/v1/workflows/desktop-admin/overview", body, tokens_loaded, NULL, NULL);
        json_node_free(body);
    }
}
static void token_issue(GtkButton *widget, gpointer value)
{
    Admin *admin = value; if (admin->closed) return;
    GtkWidget *dialog = gtk_widget_get_toplevel(GTK_WIDGET(widget));
    if (g_object_get_data(G_OBJECT(dialog), "admin-token-busy")) return;
    JsonObject *record = g_object_get_data(G_OBJECT(dialog), "admin-record");
    GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "admin-status");
    GtkWidget *days = g_object_get_data(G_OBJECT(dialog), "admin-token-days");
    const gchar *id = radios_json_string(record, "user_id", "");
    if (!g_uuid_string_is_valid(id)) { gtk_label_set_text(GTK_LABEL(status), "Kullanıcı kimliği geçersiz; anahtar verilmedi."); return; }
    gtk_spin_button_update(GTK_SPIN_BUTTON(days));
    JsonNode *body = json_node_new(JSON_NODE_OBJECT); json_node_take_object(body, json_object_new());
    json_object_set_int_member(json_node_get_object(body), "days", CLAMP(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(days)), 1, 365));
    gchar *path = g_strdup_printf("/v1/access/users/%s/tokens", id);
    if (radios_data_request(admin->ctx, dialog, "POST", path, body, token_issued, NULL, NULL)) {
        g_object_set_data(G_OBJECT(dialog), "admin-token-busy", GINT_TO_POINTER(1));
        gtk_widget_set_sensitive(GTK_WIDGET(widget), FALSE); gtk_widget_set_sensitive(days, FALSE);
        gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, FALSE);
        gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL, FALSE);
        gtk_label_set_text(GTK_LABEL(status), "Yeni erişim anahtarı veriliyor…");
    } else gtk_label_set_text(GTK_LABEL(status), "Veritabanı bağlı değil; anahtar verilmedi.");
    g_free(path); json_node_free(body);
}
static void token_copy(GtkButton *widget, gpointer secret)
{
    const gchar *token = gtk_label_get_text(GTK_LABEL(secret)); if (!*token) return;
    gtk_clipboard_set_text(gtk_widget_get_clipboard(GTK_WIDGET(widget), GDK_SELECTION_CLIPBOARD), token, -1);
    GtkWidget *status = g_object_get_data(G_OBJECT(gtk_widget_get_toplevel(GTK_WIDGET(widget))), "admin-status");
    if (status) gtk_label_set_text(GTK_LABEL(status), "Erişim anahtarı panoya kopyalandı.");
}
static void user_editor(Admin *admin, JsonObject *user)
{
    GtkWidget *form, *dialog = dialog_new(admin, "Kişi ve istasyon izinleri", "admin-user-dialog", "Kaydet", &form);
    pack(form, label(radios_json_string(user, "username", ""), NULL), FALSE);
    GtkWidget *name = field(form, "Kişinin adı", "admin-user-name", radios_ui_entry(NULL));
    gtk_entry_set_max_length(GTK_ENTRY(name), 120);
    gtk_entry_set_text(GTK_ENTRY(name), radios_json_string(user, "display_name", ""));
    const gchar *states[] = {"Etkin", "Devre dışı", NULL};
    GtkWidget *state = field(form, "Kullanıcı durumu", "admin-user-state", radios_ui_combo(states,
        g_str_equal(radios_json_string(user, "status", ""), "disabled") ? 1 : 0));
    g_object_set_data(G_OBJECT(dialog), "admin-user-name", name);
    g_object_set_data(G_OBJECT(dialog), "admin-user-state", state);
    gboolean editable = boolean(user, "editable");
    for (guint i = 0; i < G_N_ELEMENTS(permissions); ++i) {
        GtkWidget *check = gtk_check_button_new_with_label(permission_labels[i]);
        gchar *id = g_strdup_printf("admin-permission-%s", permissions[i]); gtk_widget_set_name(check, id); g_free(id);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), contains(array(user, "local_permissions"), permissions[i]));
        gtk_widget_set_sensitive(check, editable); pack(form, check, FALSE);
        g_object_set_data(G_OBJECT(dialog), permissions[i], check);
    }
    gtk_widget_set_sensitive(name, editable); gtk_widget_set_sensitive(state, editable);
    if (!editable) pack(form, label("Genel veya birden fazla istasyonu kapsayan izinler bu istasyon formundan değiştirilemez.", NULL), FALSE);
    GtkWidget *tokens = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, NULL);
    gtk_widget_set_name(tokens, "admin-token-list"); pack(form, tokens, FALSE);
    token_rows(admin, tokens, array(user, "tokens"), editable);
    GtkWidget *issue = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "technical-actions");
    GtkWidget *caption = radios_ui_label("Geçerlilik (gün)", "form-label");
    gtk_widget_set_valign(caption, GTK_ALIGN_CENTER); pack(issue, caption, FALSE);
    GtkWidget *days = gtk_spin_button_new_with_range(1, 365, 1);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(days), TRUE); gtk_spin_button_set_value(GTK_SPIN_BUTTON(days), 30);
    gtk_widget_set_name(days, "admin-token-days"); pack(issue, days, FALSE);
    GtkWidget *give = button(issue, "Yeni anahtar ver", "admin-token-issue", G_CALLBACK(token_issue), admin);
    gtk_widget_set_sensitive(issue, editable && !g_str_equal(radios_json_string(user, "status", ""), "disabled"));
    pack(form, issue, FALSE);
    GtkWidget *reveal = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "technical-actions");
    GtkWidget *secret = label("", "admin-token-secret"); gtk_label_set_selectable(GTK_LABEL(secret), TRUE);
    gtk_widget_set_tooltip_text(secret, "Anahtar yalnız bu pencerede bir kez gösterilir.");
    pack(reveal, secret, TRUE); button(reveal, "Panoya kopyala", "admin-token-copy", G_CALLBACK(token_copy), secret);
    gtk_widget_set_no_show_all(reveal, TRUE); pack(form, reveal, FALSE);
    g_object_set_data(G_OBJECT(dialog), "admin-token-list", tokens);
    g_object_set_data(G_OBJECT(dialog), "admin-token-days", days);
    g_object_set_data(G_OBJECT(dialog), "admin-token-issue", give);
    g_object_set_data(G_OBJECT(dialog), "admin-token-secret", secret);
    g_object_set_data(G_OBJECT(dialog), "admin-token-reveal", reveal);
    GtkWidget *status = label("", "admin-user-status"); pack(form, status, FALSE);
    g_object_set_data(G_OBJECT(dialog), "admin-status", status);
    g_object_set_data_full(G_OBJECT(dialog), "admin-record", json_object_ref(user), (GDestroyNotify)json_object_unref);
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, editable);
    g_signal_connect(dialog, "response", G_CALLBACK(user_response), NULL); gtk_widget_show_all(dialog);
}
static void edit_user(GtkButton *widget, gpointer value)
{
    Admin *admin = value; if (admin->closed) return;
    g_free(admin->edit_user);
    admin->edit_user = g_strdup(g_object_get_data(G_OBJECT(widget), "admin-user-id"));
    refresh(admin);
}
void radios_admin_ui_people(RadioSViewContext *ctx, GtkWidget *page, JsonObject *context)
{
    (void)ctx;
    Admin *admin = g_object_get_data(G_OBJECT(page), "admin-controller");
    GtkWidget *list = g_object_get_data(G_OBJECT(page), "technical-user-list");
    GtkWidget *empty = g_object_get_data(G_OBJECT(page), "technical-user-empty");
    if (!admin || !list) return;
    JsonArray *users = array(context, "users"), *grants = array(context, "permissions"); clear(list);
    for (guint i = 0; users && i < json_array_get_length(users); ++i) {
        JsonObject *user = json_array_get_object_element(users, i);
        const gchar *id = radios_json_string(user, "user_id", radios_json_string(user, "_id", ""));
        GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 9, "technical-user-row");
        GtkWidget *text = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, NULL);
        gchar *title = g_strdup_printf("%s · %s", radios_json_string(user, "display_name", ""), radios_json_string(user, "username", ""));
        pack(text, radios_ui_label(title, "heading"), FALSE); g_free(title);
        GString *rights = g_string_new(""); JsonArray *effective = array(user, "permissions");
        if (effective) {
            for (guint k = 0; k < json_array_get_length(effective); ++k) {
                if (rights->len) g_string_append(rights, ", ");
                g_string_append(rights, json_array_get_string_element(effective, k));
            }
        } else for (guint k = 0; grants && k < json_array_get_length(grants); ++k) {
            JsonObject *grant = json_array_get_object_element(grants, k);
            if (g_str_equal(id, radios_json_string(grant, "user_id", ""))) {
                if (rights->len) g_string_append(rights, ", ");
                g_string_append(rights, radios_json_string(grant, "permission", ""));
            }
        }
        pack(text, label(rights->str, NULL), FALSE); g_string_free(rights, TRUE); pack(row, text, TRUE);
        GtkWidget *manage = button(row, "Yetkileri yönet", "admin-user-edit", G_CALLBACK(edit_user), admin);
        g_object_set_data_full(G_OBJECT(manage), "admin-user-id", g_strdup(id), g_free);
        gtk_container_add(GTK_CONTAINER(list), row);
    }
    gtk_widget_set_visible(empty, !users || json_array_get_length(users) == 0); gtk_widget_show_all(list);
}
static void close_response(GtkDialog *dialog, gint response, gpointer unused)
{
    (void)unused;
    if (response != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    Admin *admin = g_object_get_data(G_OBJECT(dialog), "admin-controller"); if (admin->closed) return;
    GtkWidget *check = g_object_get_data(G_OBJECT(dialog), "admin-run-confirm");
    GtkWidget *status = g_object_get_data(G_OBJECT(dialog), "admin-status");
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(check))) {
        gtk_label_set_text(GTK_LABEL(status), "Yürütmenin durduğunu doğrulayın."); return;
    }
    JsonObject *record = g_object_get_data(G_OBJECT(dialog), "admin-record");
    JsonNode *body = radios_data_body(admin->ctx); JsonObject *request = json_node_get_object(body);
    json_object_set_string_member(request, "run_id", radios_json_string(record, "run_id", ""));
    json_object_set_int_member(request, "expected_revision", radios_json_int(record, "revision", 0));
    json_object_set_string_member(request, "reason", "Operatör yürütmenin durduğunu doğruladı; son bölümün sonucu bilinmiyor.");
    write_key(GTK_WIDGET(dialog), request);
    if (radios_data_request(admin->ctx, GTK_WIDGET(dialog), "POST", "/v1/workflows/desktop-admin/recovery-close", body, saved, NULL, NULL))
        gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_ACCEPT, FALSE);
    json_node_free(body);
}
static void close_run(GtkButton *widget, gpointer value)
{
    Admin *admin = value; if (admin->closed) return;
    JsonObject *record = g_object_get_data(G_OBJECT(widget), "admin-record");
    GtkWidget *form, *dialog = dialog_new(admin, "Kesintili yürütme kaydını uzlaştır", "admin-run-dialog", "Kesintili olarak kapat", &form);
    pack(form, label(radios_json_string(record, "name", ""), NULL), FALSE);
    GtkWidget *check = gtk_check_button_new_with_label("Bu yürütmenin artık çalışmadığını doğruladım");
    gtk_widget_set_name(check, "admin-run-confirm"); pack(form, check, FALSE);
    GtkWidget *status = label("", "admin-run-status"); pack(form, status, FALSE);
    g_object_set_data(G_OBJECT(dialog), "admin-run-confirm", check);
    g_object_set_data(G_OBJECT(dialog), "admin-status", status);
    g_object_set_data_full(G_OBJECT(dialog), "admin-record", json_object_ref(record), (GDestroyNotify)json_object_unref);
    g_signal_connect(dialog, "response", G_CALLBACK(close_response), NULL); gtk_widget_show_all(dialog);
}
static const gchar *status_label(const gchar *status)
{
    static const gchar *const labels[][2] = {{"observed", "Sorunsuz"}, {"warning", "Uyarı var"}, {"critical", "Kritik"},
        {"unknown", "Bilinmiyor"}, {"unavailable", "Okunamadı"}, {"ok", "Sağlam"}, {"invalid", "Sorun var"},
        {"incomplete", "Denetim tamamlanmadı"}};
    for (guint i = 0; i < G_N_ELEMENTS(labels); ++i) if (g_str_equal(status, labels[i][0])) return labels[i][1];
    return status;
}
static void overview_loaded(RadioSViewContext *ctx, GtkWidget *page, JsonNode *result,
    const GError *error, gpointer value)
{
    Admin *admin = value; admin->reading = FALSE;
    if (admin->closed) return;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        gtk_label_set_text(GTK_LABEL(admin->health), error ? error->message : "Yönetim durumu alınamadı.");
    } else {
        g_clear_pointer(&admin->overview, json_node_free); admin->overview = json_node_copy(result);
        JsonObject *root = json_node_get_object(result), *health = object(root, "health");
        JsonObject *integrity = object(health, "integrity");
        gchar *summary = g_strdup_printf("Yerel veri servisi: %s · Bütünlük: %s · Şema: %" G_GINT64_FORMAT "\nDepo: %s · İşlem sırası: %" G_GINT64_FORMAT " · DB: %" G_GINT64_FORMAT " bayt · Boş disk: %" G_GINT64_FORMAT " bayt",
            status_label(radios_json_string(health, "status", "unknown")), status_label(radios_json_string(integrity, "status", "unknown")),
            radios_json_int(health, "schema_version", 0), radios_json_string(health, "store_id", ""),
            radios_json_int(health, "sequence", radios_json_int(root, "sequence", 0)),
            radios_json_int(health, "database_bytes", 0), radios_json_int(health, "filesystem_free_bytes", 0));
        gtk_label_set_text(GTK_LABEL(admin->health), summary); g_free(summary);
        GtkWidget *server = g_object_get_data(G_OBJECT(page), "admin-server-status");
        if (server) gtk_label_set_text(GTK_LABEL(server), status_label(radios_json_string(health, "status", "unknown")));
        server = g_object_get_data(G_OBJECT(page), "admin-database-status");
        if (server) gtk_label_set_text(GTK_LABEL(server), status_label(radios_json_string(integrity, "status", "unknown")));
        radios_admin_ui_people(ctx, page, root);
        clear(admin->runs); JsonArray *runs = array(root, "recoverable_runs");
        for (guint i = 0; runs && i < json_array_get_length(runs); ++i) {
            JsonObject *run = json_array_get_object_element(runs, i);
            GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 7, "technical-actions");
            gchar *text = g_strdup_printf("%s · %s · %s", radios_json_string(run, "name", ""),
                radios_json_string(run, "kind", ""), radios_json_string(run, "state", ""));
            pack(row, label(text, NULL), TRUE); g_free(text);
            GtkWidget *close_button = button(row, "Kesintiyi uzlaştır", "admin-run-close", G_CALLBACK(close_run), admin);
            g_object_set_data_full(G_OBJECT(close_button), "admin-record", json_object_ref(run), (GDestroyNotify)json_object_unref);
            pack(admin->runs, row, FALSE);
        }
        if (!runs || !json_array_get_length(runs)) pack(admin->runs, label("Açık yürütme kaydı yok.", NULL), FALSE);
        gtk_widget_show_all(admin->runs);
        GString *audit = g_string_new(""); JsonArray *tail = array(root, "audit_tail");
        for (guint i = 0; tail && i < json_array_get_length(tail); ++i) {
            JsonObject *event = json_array_get_object_element(tail, i);
            g_string_append_printf(audit, "#%" G_GINT64_FORMAT " · %s · %s", radios_json_int(event, "sequence", 0),
                radios_json_string(event, "at", ""), radios_json_string(event, "actor", ""));
            JsonArray *collections = array(event, "collections");
            for (guint k = 0; collections && k < json_array_get_length(collections); ++k)
                g_string_append_printf(audit, "%s%s", k ? ", " : " · ", json_array_get_string_element(collections, k));
            g_string_append_c(audit, '\n');
        }
        gtk_label_set_text(GTK_LABEL(admin->audit), audit->len ? audit->str : "Bu istasyona ait işlem kaydı yok.");
        g_string_free(audit, TRUE);
        if (admin->edit_user) {
            JsonArray *users = array(root, "users");
            for (guint i = 0; users && i < json_array_get_length(users); ++i) {
                JsonObject *user = json_array_get_object_element(users, i);
                if (g_str_equal(admin->edit_user, radios_json_string(user, "user_id", ""))) { user_editor(admin, user); break; }
            }
            g_clear_pointer(&admin->edit_user, g_free);
        }
    }
    if (admin->again) { admin->again = FALSE; refresh(admin); }
}
static void refresh(Admin *admin)
{
    if (admin->closed) return;
    if (admin->reading) { admin->again = TRUE; return; }
    GtkWidget *page = g_weak_ref_get(&admin->page); if (!page) return;
    if (radios_data_connected(admin->ctx)) {
        JsonNode *body = radios_data_body(admin->ctx); admin->reading = TRUE;
        if (!radios_data_request(admin->ctx, page, "POST", "/v1/workflows/desktop-admin/overview", body,
            overview_loaded, admin_ref(admin), admin_unref)) admin->reading = FALSE;
        json_node_free(body);
    } else gtk_label_set_text(GTK_LABEL(admin->health), "Veritabanı bağlantısı hazır değil.");
    g_object_unref(page);
}
void radios_admin_ui_refresh(GtkButton *widget, gpointer page)
{
    (void)widget; Admin *admin = g_object_get_data(G_OBJECT(page), "admin-controller");
    if (admin) {
        if (gtk_expander_get_expanded(GTK_EXPANDER(admin->expander)) && gtk_widget_get_mapped(admin->expander)) (void)poll(admin);
        else refresh(admin);
    }
}
static gboolean poll(gpointer value)
{
    Admin *admin = value;
    if (admin->closed) return G_SOURCE_REMOVE;
    if (!gtk_expander_get_expanded(GTK_EXPANDER(admin->expander)) || !gtk_widget_get_mapped(admin->expander)) return G_SOURCE_CONTINUE;
    refresh(admin);
    gchar *stream = radios_broadcast_ui_status(admin->ctx, "stream");
    gchar *record = radios_broadcast_ui_status(admin->ctx, "record");
    gchar *audio = g_strdup_printf("MAIN ses aygıtı: %s · Mikrofon: %s\n%s\n%s",
        radios_audio_ui_ready(admin->ctx, RADIOS_AUDIO_UI_MAIN) ? "hazır" : "hazır değil",
        radios_audio_ui_microphone_active(admin->ctx) ? "açık" : "kapalı", stream ? stream : "", record ? record : "");
    gtk_label_set_text(GTK_LABEL(admin->audio), audio); g_free(audio); g_free(stream); g_free(record);
    JsonNode *recovery = radios_data_recovery_status(admin->ctx);
    if (recovery && JSON_NODE_HOLDS_OBJECT(recovery)) {
        JsonObject *status = json_node_get_object(recovery);
        GString *summary = g_string_new(NULL);
        if (*radios_json_string(status, "error", "")) g_string_append(summary, radios_json_string(status, "error", ""));
        else g_string_append_printf(summary, "Dayanıklı işlem kuyruğu: %" G_GINT64_FORMAT " bekleyen · %" G_GINT64_FORMAT " inceleme gereken.",
            radios_json_int(status, "pending", 0), radios_json_int(status, "blocked", 0));
        JsonArray *operations = array(status, "operations");
        for (guint i = 0; operations && i < json_array_get_length(operations); ++i) {
            JsonObject *operation = json_array_get_object_element(operations, i);
            g_string_append_printf(summary, "\n%s · %s · %s", radios_json_string(operation, "operation_id", ""),
                radios_json_string(operation, "path", ""), radios_json_string(operation, "state", ""));
            if (json_object_has_member(operation, "rejection_code"))
                g_string_append_printf(summary, " · Hata kodu %" G_GINT64_FORMAT, radios_json_int(operation, "rejection_code", 0));
            if (boolean(operation, "dependency_blocked"))
                g_string_append(summary, "\nÖnceki işlem reddedildi; bu kayıt gönderilmiyor.");
        }
        g_string_append(summary, "\nKesinti sonrasında ses kendiliğinden başlatılmaz. Açık çalışma kaydı canlı olabilir; uzlaştırmadan önce yürütmenin durduğunu doğrulayın.");
        gtk_label_set_text(GTK_LABEL(admin->recovery), summary->str); g_string_free(summary, TRUE);
    }
    g_clear_pointer(&recovery, json_node_free);
    return G_SOURCE_CONTINUE;
}
static void retry(GtkButton *widget, gpointer value)
{
    (void)widget; Admin *admin = value;
    if (!admin->closed) { radios_data_recovery_retry(admin->ctx); (void)poll(admin); }
}
static void expanded(GObject *widget, GParamSpec *property, gpointer value)
{ (void)widget; (void)property; (void)poll(value); }
static void audit_expanded(GObject *widget, GParamSpec *property, gpointer value)
{
    (void)property;
    if (gtk_expander_get_expanded(GTK_EXPANDER(widget))) refresh(value);
}
static void backup_observed(Admin *admin, const gchar *summary, const gchar *detail)
{
    if (!admin->ctx->parent || admin->closed) return;
    GDateTime *now = g_date_time_new_now_utc();
    gchar *time = g_date_time_format(now, "%Y-%m-%d %H:%M:%S UTC");
    gchar *observation = g_strdup_printf("%s\nSon kontrol: %s", detail, time);
    g_object_set_data_full(G_OBJECT(admin->ctx->parent), "radios-backup-summary", g_strdup(summary), g_free);
    g_object_set_data_full(G_OBJECT(admin->ctx->parent), "radios-backup-summary-detail", observation, g_free);
    g_free(time); g_date_time_unref(now);
}
static void tool_completed(GObject *source, GAsyncResult *result, gpointer value)
{
    Admin *admin = value; GError *error = NULL; gchar *output = NULL, *errors = NULL;
    gboolean communicated = g_subprocess_communicate_utf8_finish(G_SUBPROCESS(source), result, &output, &errors, &error);
    if (admin->timeout) { g_source_remove(admin->timeout); admin->timeout = 0; }
    g_clear_object(&admin->child); g_clear_object(&admin->cancel);
    if (!admin->closed) {
        gtk_widget_set_sensitive(admin->backup_actions, TRUE);
        gtk_widget_set_sensitive(admin->backups, TRUE);
        JsonParser *parser = json_parser_new();
        gboolean parsed = communicated && output && strlen(output) <= 1024 * 1024 && json_parser_load_from_data(parser, output, -1, NULL);
        JsonNode *node = parsed ? json_parser_get_root(parser) : NULL;
        if (!node || !JSON_NODE_HOLDS_OBJECT(node)) {
            gtk_label_set_text(GTK_LABEL(admin->backup_status), error ? "Yedek işlemi kesildi veya zaman aşımına uğradı." : "Yedek aracı geçerli JSON alındısı döndürmedi.");
            backup_observed(admin, "İşlem başarısız", gtk_label_get_text(GTK_LABEL(admin->backup_status)));
        } else {
            JsonObject *response = json_node_get_object(node); const gchar *status = radios_json_string(response, "status", "");
            JsonArray *backups = array(response, "backups");
            if (backups) {
                gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(admin->backups));
                for (guint i = 0; i < json_array_get_length(backups); ++i) {
                    JsonObject *backup = json_array_get_object_element(backups, i);
                    const gchar *name = radios_json_string(backup, "name", "");
                    gchar *text = g_strdup_printf("%s · %s · sıra %" G_GINT64_FORMAT, name,
                        radios_json_string(backup, "status", ""), radios_json_int(backup, "sequence", 0));
                    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(admin->backups), name, text); g_free(text);
                }
                if (json_array_get_length(backups)) gtk_combo_box_set_active(GTK_COMBO_BOX(admin->backups), 0);
            }
            gchar *message;
            if (g_str_equal(status, "error")) message = g_strdup(radios_json_string(response, "error", "Yedek işlemi başarısız."));
            else if (g_str_equal(status, "staged")) message = g_strdup_printf("Yedek ayrı bir dizine açıldı ve doğrulandı. Etkinleştirilmedi.\n%s\nDepo: %s · Bütünlük: %s",
                radios_json_string(response, "restored_path", ""), radios_json_string(response, "store_id", ""), radios_json_string(response, "integrity", ""));
            else if (g_str_equal(status, "planned")) {
                GString *command = g_string_new(""); JsonArray *args = array(response, "command");
                for (guint i = 0; args && i < json_array_get_length(args); ++i) {
                    gchar *quoted = g_shell_quote(json_array_get_string_element(args, i));
                    if (command->len) g_string_append_c(command, ' ');
                    g_string_append(command, quoted); g_free(quoted);
                }
                message = g_strdup_printf("Geri yükleme planı hazır. Uygulamayı ve servisi kapatıp aşağıdaki komutu çalıştırın.\n%s\n%s",
                    radios_json_string(response, "note", ""), command->str); g_string_free(command, TRUE);
            } else message = g_strdup_printf("Yedek işlemi: %s%s%s · İşlem sırası: %" G_GINT64_FORMAT,
                status, *radios_json_string(response, "name", "") ? " · " : "", radios_json_string(response, "name", ""), radios_json_int(response, "sequence", 0));
            gtk_label_set_text(GTK_LABEL(admin->backup_status), message);
            if (g_str_equal(status, "error")) backup_observed(admin, "İşlem başarısız", message);
            else if (backups) {
                guint verified = 0, invalid = 0;
                for (guint i = 0; i < json_array_get_length(backups); ++i) {
                    JsonObject *row = json_array_get_object_element(backups, i);
                    if (!g_strcmp0(radios_json_string(row, "status", ""), "verified")) ++verified;
                    else ++invalid;
                }
                gchar *summary = invalid ? g_strdup_printf("%u doğrulandı · %u geçersiz", verified, invalid) :
                    verified ? g_strdup_printf("%u doğrulandı", verified) : g_strdup("Yedek yok");
                backup_observed(admin, summary, "Yerel yedek arşivleri ve yönetilen medya dosyaları denetlendi.");
                g_free(summary);
            } else if (g_str_equal(status, "created")) backup_observed(admin, "Oluşturuldu", message);
            else if (g_str_equal(status, "verified")) backup_observed(admin, "Doğrulandı", message);
            g_free(message);
        }
        g_object_unref(parser);
    }
    g_clear_error(&error); g_free(output); g_free(errors); admin_unref(admin);
}
static gboolean tool_timeout(gpointer value)
{
    Admin *admin = value; admin->timeout = 0;
    if (admin->cancel) g_cancellable_cancel(admin->cancel);
    if (admin->child) g_subprocess_force_exit(admin->child);
    return G_SOURCE_REMOVE;
}
static void tool_launch(Admin *admin, const gchar *operation, const gchar *backup)
{
    gchar *helper = g_build_filename(admin->ctx->config->project_root, "tools", "radios_reliability.py", NULL);
    const gchar *args[12] = {admin->ctx->config->python_command, helper, "--project-root", admin->ctx->config->project_root,
        "--config", admin->ctx->config->config_file, operation, NULL, NULL, NULL, NULL, NULL};
    if (backup && !g_str_equal(operation, "list") && !g_str_equal(operation, "create")) { args[7] = "--backup"; args[8] = backup; }
    GError *error = NULL;
    admin->child = g_subprocess_newv(args, G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE, &error); g_free(helper);
    if (!admin->child) {
        gtk_widget_set_sensitive(admin->backup_actions, TRUE); gtk_widget_set_sensitive(admin->backups, TRUE);
        gtk_label_set_text(GTK_LABEL(admin->backup_status), "Yedek aracı başlatılamadı."); g_clear_error(&error); return;
    }
    gtk_widget_set_sensitive(admin->backup_actions, FALSE);
    gtk_label_set_text(GTK_LABEL(admin->backup_status), "Yedek işlemi sürüyor…");
    admin->cancel = g_cancellable_new(); admin->timeout = g_timeout_add_seconds(60, tool_timeout, admin);
    g_subprocess_communicate_utf8_async(admin->child, NULL, admin->cancel, tool_completed, admin_ref(admin));
}
typedef struct { Admin *admin; gchar *operation, *backup; } ToolIntent;
static void tool_intent_free(gpointer value)
{
    ToolIntent *intent = value; admin_unref(intent->admin); g_free(intent->operation); g_free(intent->backup); g_free(intent);
}
static void tool_authorized(RadioSViewContext *ctx, GtkWidget *page, JsonNode *result,
    const GError *error, gpointer value)
{
    (void)ctx; (void)page; ToolIntent *intent = value; Admin *admin = intent->admin;
    admin->authorizing = FALSE; if (admin->closed) return;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result) ||
        !contains(array(json_node_get_object(result), "permissions"), "manage")) {
        gtk_widget_set_sensitive(admin->backup_actions, TRUE); gtk_widget_set_sensitive(admin->backups, TRUE);
        gtk_label_set_text(GTK_LABEL(admin->backup_status), error ? error->message : "Yedek yönetimi için bu istasyonda güncel yönetim yetkisi gerekli.");
        return;
    }
    tool_launch(admin, intent->operation, intent->backup);
}
static void tool_start(GtkButton *widget, gpointer value)
{
    Admin *admin = value; if (admin->closed || admin->child || admin->authorizing) return;
    const gchar *operation = g_object_get_data(G_OBJECT(widget), "admin-tool-operation");
    const gchar *backup = gtk_combo_box_get_active_id(GTK_COMBO_BOX(admin->backups));
    if (!g_str_equal(operation, "list") && !g_str_equal(operation, "create") && (!backup || !*backup)) {
        gtk_label_set_text(GTK_LABEL(admin->backup_status), "Önce yedekleri listeleyip bir yedek seçin."); return;
    }
    GtkWidget *page = g_weak_ref_get(&admin->page); if (!page) return;
    ToolIntent *intent = g_new0(ToolIntent, 1); intent->admin = admin_ref(admin);
    intent->operation = g_strdup(operation); intent->backup = g_strdup(backup);
    admin->authorizing = TRUE;
    gtk_widget_set_sensitive(admin->backup_actions, FALSE); gtk_widget_set_sensitive(admin->backups, FALSE);
    gtk_label_set_text(GTK_LABEL(admin->backup_status), "İstasyon yönetim yetkisi doğrulanıyor…");
    JsonNode *body = radios_data_body(admin->ctx);
    if (!radios_data_request(admin->ctx, page, "POST", "/v1/workflows/desktop-admin/overview", body,
        tool_authorized, intent, tool_intent_free)) {
        admin->authorizing = FALSE; gtk_widget_set_sensitive(admin->backup_actions, TRUE); gtk_widget_set_sensitive(admin->backups, TRUE);
        gtk_label_set_text(GTK_LABEL(admin->backup_status), "Yönetim yetkisi doğrulanamadı; veri servisini bağlayın.");
    }
    json_node_free(body); g_object_unref(page);
}
static void part(GString *line, const gchar *format, ...) G_GNUC_PRINTF(2, 3);
static void part(GString *line, const gchar *format, ...)
{
    va_list args; va_start(args, format);
    if (line->len) g_string_append(line, " · ");
    g_string_append_vprintf(line, format, args); va_end(args);
}
static void size_part(GString *line, const gchar *name, gdouble bytes)
{ gchar *size = g_format_size((guint64)MAX(bytes, 0)); part(line, "%s: %s", name, size); g_free(size); }
static void sentence(GString *text, GString *line)
{
    if (line->len) g_string_append_printf(text, "%s%s", text->len ? "\n" : "", line->str);
    g_string_truncate(line, 0);
}
static gchar *duration(gdouble seconds)
{
    guint64 total = seconds > 0 ? (guint64)seconds : 0;
    if (total >= 86400) return g_strdup_printf("%" G_GUINT64_FORMAT " gün %" G_GUINT64_FORMAT " sa", total / 86400, total % 86400 / 3600);
    if (total >= 3600) return g_strdup_printf("%" G_GUINT64_FORMAT " sa %" G_GUINT64_FORMAT " dk", total / 3600, total % 3600 / 60);
    if (total >= 60) return g_strdup_printf("%" G_GUINT64_FORMAT " dk", total / 60);
    return g_strdup_printf("%" G_GUINT64_FORMAT " sn", total);
}
static gchar *metrics_text(JsonObject *root)
{
    GString *text = g_string_new(""), *line = g_string_new(""); gdouble value = 0, other = 0;
    JsonObject *collections = object(root, "collections");
    if (collections) {
        gdouble total = 0; guint filled = 0; GList *names = json_object_get_members(collections);
        for (GList *name = names; name; name = name->next)
            if (number(collections, name->data, &value) && value > 0) { total += value; ++filled; }
        g_list_free(names);
        part(line, "Kayıtlar: %.0f kayıt", total); part(line, "%u koleksiyon", filled); sentence(text, line);
        static const gchar *const known[][2] = {{"desktop_media_library", "Medya"}, {"desktop_playlist_entries", "Çalma listesi girdisi"},
            {"users", "Kullanıcı"}, {"tokens", "Erişim anahtarı"}, {"desktop_configurations", "Ayar kaydı"}, {"_audit", "İşlem kaydı"}};
        for (guint i = 0; i < G_N_ELEMENTS(known); ++i)
            if (number(collections, known[i][0], &value)) part(line, "%s: %.0f", known[i][1], value);
        sentence(text, line);
    }
    JsonObject *bytes = object(root, "database_bytes"), *disk = object(root, "disk_bytes");
    if (number(bytes, "journal", &value)) size_part(line, "Günlük", value);
    if (number(bytes, "checkpoint", &value)) size_part(line, "Checkpoint", value);
    if (number(disk, "free", &value) && number(disk, "total", &other)) {
        gchar *free_text = g_format_size((guint64)MAX(value, 0)), *total_text = g_format_size((guint64)MAX(other, 0));
        part(line, "Boş disk: %s / %s", free_text, total_text); g_free(free_text); g_free(total_text);
    }
    sentence(text, line);
    if (number(root, "sequence", &value)) part(line, "İşlem sırası: %.0f", value);
    if (number(root, "schema_version", &value)) part(line, "Şema: %.0f", value);
    static const gchar *const roles[][2] = {{"primary", "birincil"}, {"standby", "bekleme"}, {"fenced", "ayrılmış"}};
    const gchar *role = radios_json_string(object(root, "node"), "node_role", "");
    for (guint i = 0; i < G_N_ELEMENTS(roles); ++i) if (g_str_equal(role, roles[i][0])) part(line, "Rol: %s", roles[i][1]);
    if (boolean(root, "read_only_handle")) part(line, "Salt okunur");
    sentence(text, line);
    JsonObject *wal = object(root, "wal"), *replication = object(root, "replication");
    if (wal && json_object_has_member(wal, "enabled")) {
        if (!boolean(wal, "enabled")) part(line, "WAL arşivi: kapalı");
        else if (number(wal, "unarchived_sequences", &value)) part(line, "WAL arşivi: açık, arşivlenmemiş işlem %.0f", value);
        else part(line, "WAL arşivi: açık");
    }
    if (replication && json_object_has_member(replication, "pending_replay"))
        part(line, "Çoğaltma: %s", boolean(replication, "pending_replay") ? "bekleyen yeniden oynatma var" : "bekleyen iş yok");
    sentence(text, line);
    JsonObject *process = object(root, "process_metrics"), *counters = object(process, "counters");
    JsonObject *http = object(root, "http_server");
    if (number(process, "uptime_seconds", &value)) { gchar *up = duration(value); part(line, "Çalışma süresi: %s", up); g_free(up); }
    if (number(counters, "commits", &value)) part(line, "%.0f yazma", value);
    if (number(counters, "conflicts", &value)) part(line, "%.0f çakışma", value);
    if (number(counters, "lock_timeouts", &value)) part(line, "%.0f kilit zaman aşımı", value);
    if (number(object(object(process, "timings"), "http_request_ms"), "p95", &value)) part(line, "İstek süresi p95: %.0f ms", value);
    if (number(http, "active_connections", &value) && number(http, "max_connections", &other)) part(line, "Bağlantı: %.0f/%.0f", value, other);
    if (number(http, "capacity_rejections", &value) && value > 0) part(line, "%.0f reddedilen bağlantı", value);
    sentence(text, line);
    g_string_free(line, TRUE); return g_string_free(text, FALSE);
}
static const gchar *check_reason(const gchar *reason)
{
    if (g_str_has_prefix(reason, "byte budget exhausted")) return "bayt bütçesi yetmedi";
    if (g_str_has_prefix(reason, "record validation budget exhausted")) return "kayıt bütçesi yetmedi";
    if (g_str_has_prefix(reason, "source lock wait exceeded")) return "kilit beklemesi aşıldı";
    if (g_str_has_prefix(reason, "verification disabled")) return "denetim kapalı";
    return NULL;
}
static gchar *integrity_text(JsonObject *root, const gchar *title, GString *issues)
{
    GString *text = g_string_new(""), *line = g_string_new(""); gdouble value = 0;
    static const gchar *const checks[][2] = {{"storage", "Depolama"}, {"records", "Kayıtlar"}, {"indexes", "Dizinler"},
        {"managed_media", "Medya dosyaları"}, {"cold_history", "Soğuk arşiv"}};
    static const gchar *const states[][2] = {{"passed", "geçti"}, {"failed", "başarısız"}, {"skipped", "atlandı"},
        {"incomplete", "tamamlanmadı"}, {"not_checked", "denetlenmedi"}};
    const gchar *why = NULL, *status = radios_json_string(root, "status", "");
    for (guint i = 0; i < G_N_ELEMENTS(checks); ++i) {
        JsonObject *check = object(object(root, "checks"), checks[i][0]);
        const gchar *state = radios_json_string(check, "status", "");
        for (guint k = 0; k < G_N_ELEMENTS(states); ++k) if (g_str_equal(state, states[k][0])) part(line, "%s: %s", checks[i][1], states[k][1]);
        if (!why && g_str_equal(state, "incomplete")) why = check_reason(radios_json_string(check, "reason", ""));
    }
    GString *verdict = g_string_new("");
    if (g_str_equal(status, "ok")) g_string_append_printf(verdict, "%s: Sağlam", title);
    else if (g_str_equal(status, "invalid")) {
        g_string_append_printf(verdict, "%s: Sorun var", title);
        if (number(root, "issue_count", &value)) g_string_append_printf(verdict, " · %.0f sorun", value);
    } else if (g_str_equal(status, "incomplete"))
        g_string_append_printf(verdict, "%s: Denetim tamamlanmadı%s%s", title, why ? " · " : "", why ? why : "");
    sentence(text, verdict); g_string_free(verdict, TRUE); sentence(text, line);
    JsonObject *work = object(root, "work");
    if (number(work, "records_checked", &value)) part(line, "Denetlenen: %.0f kayıt", value);
    if (number(work, "bytes_read", &value)) size_part(line, "Okunan", value);
    if (number(work, "files_read", &value)) part(line, "%.0f dosya", value);
    sentence(text, line);
    static const gchar *const codes[][2] = {{"record_map_invalid", "Koleksiyon yapısı bozuk"},
        {"persistent_record_invalid", "Geçersiz kayıt"}, {"audit_coverage_invalid", "İşlem kaydı kapsamı bozuk"},
        {"managed_media_invalid", "Medya dosyası doğrulanamadı"}, {"cold_history_invalid", "Soğuk arşiv doğrulanamadı"},
        {"storage_invalid", "Depolama bozuk"}, {"records_invalid", "Kayıt denetimi başarısız"}};
    JsonArray *found = array(root, "issues"); guint shown = 0;
    for (guint i = 0; found && i < json_array_get_length(found) && shown < 20; ++i) {
        JsonNode *node = json_array_get_element(found, i);
        if (!JSON_NODE_HOLDS_OBJECT(node)) continue;
        JsonObject *issue = json_node_get_object(node); const gchar *name = "Sorun";
        for (guint k = 0; k < G_N_ELEMENTS(codes); ++k) if (g_str_equal(radios_json_string(issue, "code", ""), codes[k][0])) name = codes[k][1];
        g_string_append_printf(issues, "%s• %s", issues->len ? "\n" : "", name);
        const gchar *collection = radios_json_string(issue, "collection", ""), *key = radios_json_string(issue, "key", "");
        if (*collection) g_string_append_printf(issues, " · %s%s%s", collection, *key ? "/" : "", key);
        const gchar *detail = radios_json_string(issue, "detail", "");
        if (*detail) {
            gchar *clip = g_utf8_substring(detail, 0, MIN(g_utf8_strlen(detail, -1), 200));
            g_string_append_printf(issues, ": %s", clip); g_free(clip);
        }
        ++shown;
    }
    if (number(root, "issue_count", &value) && value > shown)
        g_string_append_printf(issues, "%s… ve %.0f sorun daha", issues->len ? "\n" : "", value - shown);
    g_string_free(line, TRUE); return g_string_free(text, FALSE);
}
static void database_failed(Admin *admin, const GError *error)
{
    if (g_error_matches(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_FORBIDDEN)) admin->db_denied = TRUE;
    else if (!admin->db_error) admin->db_error = g_strdup(error ? error->message : "Veritabanı durumu alınamadı.");
}
static gboolean database_busy(Admin *admin)
{ return admin->db_pending || admin->db_job_starting || admin->db_job; }
/* The bounded and the full check write the same lines, so one runs at a time. */
static void database_buttons(Admin *admin)
{
    gtk_widget_set_sensitive(admin->db_refresh, !database_busy(admin));
    gtk_widget_set_sensitive(admin->db_full, !database_busy(admin));
}
static void database_done(Admin *admin)
{
    if (admin->db_pending && --admin->db_pending) return;
    database_buttons(admin);
    if (admin->db_denied) show_text(admin->db_state, "Veritabanı durumu için genel yönetim yetkisi gerekli.");
    else if (admin->db_error) show_text(admin->db_state, admin->db_error);
    else {
        GDateTime *now = g_date_time_new_now_local(); gchar *text = g_date_time_format(now, "Okundu: %H:%M:%S");
        show_text(admin->db_state, text); g_free(text); g_date_time_unref(now);
    }
}
static void metrics_loaded(RadioSViewContext *ctx, GtkWidget *page, JsonNode *result,
    const GError *error, gpointer value)
{
    (void)ctx; (void)page; Admin *admin = value; if (admin->closed) return;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { database_failed(admin, error); show_text(admin->db_metrics, ""); }
    else { gchar *text = metrics_text(json_node_get_object(result)); show_text(admin->db_metrics, text); g_free(text); }
    database_done(admin);
}
static void integrity_loaded(RadioSViewContext *ctx, GtkWidget *page, JsonNode *result,
    const GError *error, gpointer value)
{
    (void)ctx; (void)page; Admin *admin = value; if (admin->closed) return;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        database_failed(admin, error); show_text(admin->db_integrity, ""); show_text(admin->db_issues, "");
    } else {
        GString *issues = g_string_new(""); gchar *text = integrity_text(json_node_get_object(result), "Bütünlük", issues);
        show_text(admin->db_integrity, text); show_text(admin->db_issues, issues->str);
        g_free(text); g_string_free(issues, TRUE);
    }
    database_done(admin);
}
static void database_refresh(GtkButton *widget, gpointer value)
{
    (void)widget; Admin *admin = value;
    if (admin->closed || database_busy(admin)) return;
    if (!radios_data_connected(admin->ctx)) { show_text(admin->db_state, "Veritabanı bağlantısı hazır değil."); return; }
    GtkWidget *page = g_weak_ref_get(&admin->page); if (!page) return;
    admin->db_denied = FALSE; g_clear_pointer(&admin->db_error, g_free); admin->db_pending = 2;
    database_buttons(admin); show_text(admin->db_state, "Veritabanı durumu okunuyor…");
    if (!radios_data_request(admin->ctx, page, "GET", "/v1/operations/metrics", NULL, metrics_loaded, admin_ref(admin), admin_unref)) {
        admin->db_pending = 0; database_buttons(admin);
        show_text(admin->db_state, "Veritabanı bağlantısı hazır değil.");
    } else if (!radios_data_request(admin->ctx, page, "GET", DATABASE_INTEGRITY_PATH, NULL, integrity_loaded, admin_ref(admin), admin_unref)) {
        admin->db_error = g_strdup("Veritabanı bağlantısı hazır değil."); database_done(admin);
    }
    g_object_unref(page);
}
static gchar *elapsed_text(gint64 seconds)
{
    seconds = MAX(seconds, 0);
    if (seconds < 60) return g_strdup_printf("%" G_GINT64_FORMAT " sn", seconds);
    if (seconds < 3600) return g_strdup_printf("%" G_GINT64_FORMAT " dk %" G_GINT64_FORMAT " sn", seconds / 60, seconds % 60);
    return g_strdup_printf("%" G_GINT64_FORMAT " sa %" G_GINT64_FORMAT " dk", seconds / 3600, seconds % 3600 / 60);
}
static gchar *full_elapsed(Admin *admin)
{ return elapsed_text((g_get_monotonic_time() - admin->db_job_started) / G_TIME_SPAN_SECOND); }
static void full_stop(Admin *admin)
{
    if (admin->db_job_poll) { g_source_remove(admin->db_job_poll); admin->db_job_poll = 0; }
    g_clear_pointer(&admin->db_job, g_free); admin->db_job_failures = 0;
    database_buttons(admin);
}
static void full_failed(Admin *admin, const gchar *what, const GError *error)
{
    if (g_error_matches(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_FORBIDDEN))
        show_text(admin->db_state, "Veritabanı durumu için genel yönetim yetkisi gerekli.");
    else {
        gchar *text = g_strdup_printf("%s: %s", what, error ? error->message : "veri servisi geçerli yanıt döndürmedi.");
        show_text(admin->db_state, text); g_free(text);
    }
}
static void full_finished(Admin *admin, JsonObject *job)
{
    JsonObject *report = object(job, "result"); const gchar *state = radios_json_string(job, "state", "");
    if (g_str_equal(state, "done") && report) {
        GString *issues = g_string_new(""); gchar *text = integrity_text(report, "Tam denetim", issues);
        show_text(admin->db_integrity, text); show_text(admin->db_issues, issues->str);
        g_free(text); g_string_free(issues, TRUE);
        GDateTime *now = g_date_time_new_now_local(); gchar *clock = g_date_time_format(now, "%H:%M:%S");
        gchar *took = full_elapsed(admin), *line = g_strdup_printf("Tam denetim bitti: %s · Süre: %s", clock, took);
        show_text(admin->db_state, line); g_free(line); g_free(took); g_free(clock); g_date_time_unref(now);
    } else {
        const gchar *message = g_str_equal(state, "failed") ? radios_json_string(job, "message", "") : "";
        show_text(admin->db_integrity, ""); show_text(admin->db_issues, "");
        show_text(admin->db_state, *message ? message : "Tam denetim tamamlanamadı.");
    }
    full_stop(admin);
}
static void full_polled(RadioSViewContext *ctx, GtkWidget *page, JsonNode *result,
    const GError *error, gpointer value)
{
    (void)ctx; (void)page; Admin *admin = value;
    admin->db_job_reading = FALSE;
    if (admin->closed || !admin->db_job) return;
    JsonObject *job = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    if (error || !job) {
        /* A busy or briefly unreachable service is polled again; a refusal or a lost job is final. */
        if (g_error_matches(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_FORBIDDEN) ||
            g_error_matches(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_NOT_FOUND) ||
            g_error_matches(error, RADIOS_DATA_CLIENT_ERROR, RADIOS_DATA_CLIENT_ERROR_VALIDATION) ||
            ++admin->db_job_failures >= DATABASE_FULL_FAILURES) {
            full_failed(admin, "Tam denetim izlenemedi", error); full_stop(admin);
        }
        return;
    }
    admin->db_job_failures = 0;
    if (!g_str_equal(radios_json_string(job, "job_id", ""), admin->db_job)) return;
    if (!g_str_equal(radios_json_string(job, "state", ""), "running")) full_finished(admin, job);
}
static gboolean full_tick(gpointer value)
{
    Admin *admin = value;
    if (admin->closed || !admin->db_job) { admin->db_job_poll = 0; return G_SOURCE_REMOVE; }
    gchar *took = full_elapsed(admin), *line = g_strdup_printf("Tam denetim sürüyor · %s", took);
    show_text(admin->db_state, line); g_free(line); g_free(took);
    if (admin->db_job_reading) return G_SOURCE_CONTINUE;
    GtkWidget *page = g_weak_ref_get(&admin->page);
    if (!page) { admin->db_job_poll = 0; return G_SOURCE_REMOVE; }
    JsonNode *body = radios_data_body(admin->ctx);
    json_object_set_string_member(json_node_get_object(body), "job_id", admin->db_job);
    admin->db_job_reading = radios_data_request(admin->ctx, page, "POST", DATABASE_FULL_JOB, body,
        full_polled, admin_ref(admin), admin_unref);
    json_node_free(body); g_object_unref(page);
    if (admin->db_job_reading || ++admin->db_job_failures < DATABASE_FULL_FAILURES) return G_SOURCE_CONTINUE;
    admin->db_job_poll = 0;
    show_text(admin->db_state, "Tam denetim izlenemedi: veritabanı bağlantısı hazır değil."); full_stop(admin);
    return G_SOURCE_REMOVE;
}
static void full_started(RadioSViewContext *ctx, GtkWidget *page, JsonNode *result,
    const GError *error, gpointer value)
{
    (void)ctx; (void)page; Admin *admin = value;
    admin->db_job_starting = FALSE;
    if (admin->closed) return;
    JsonObject *job = result && JSON_NODE_HOLDS_OBJECT(result) ? json_node_get_object(result) : NULL;
    const gchar *id = radios_json_string(job, "job_id", "");
    if (error || !g_uuid_string_is_valid(id)) {
        full_failed(admin, "Tam denetim başlatılamadı", error); database_buttons(admin); return;
    }
    g_free(admin->db_job); admin->db_job = g_strdup(id);
    admin->db_job_failures = 0; admin->db_job_reading = FALSE; admin->db_job_started = g_get_monotonic_time();
    database_buttons(admin);
    if (!g_str_equal(radios_json_string(job, "state", ""), "running")) { full_finished(admin, job); return; }
    show_text(admin->db_state, "Tam denetim sürüyor · 0 sn");
    admin->db_job_poll = g_timeout_add(1000, full_tick, admin);
}
static void full_start(Admin *admin)
{
    if (admin->closed || database_busy(admin)) return;
    if (!radios_data_connected(admin->ctx)) { show_text(admin->db_state, "Veritabanı bağlantısı hazır değil."); return; }
    GtkWidget *page = g_weak_ref_get(&admin->page); if (!page) return;
    JsonNode *body = radios_data_body(admin->ctx);
    json_object_set_boolean_member(json_node_get_object(body), "verify_media", TRUE);
    admin->db_job_starting = radios_data_request(admin->ctx, page, "POST", DATABASE_FULL_START, body,
        full_started, admin_ref(admin), admin_unref);
    show_text(admin->db_state, admin->db_job_starting ? "Tam denetim başlatılıyor…" : "Veritabanı bağlantısı hazır değil.");
    database_buttons(admin); json_node_free(body); g_object_unref(page);
}
static void full_response(GtkDialog *dialog, gint response, gpointer unused)
{
    (void)unused;
    if (response == GTK_RESPONSE_ACCEPT) full_start(g_object_get_data(G_OBJECT(dialog), "admin-controller"));
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
static void database_full_check(GtkButton *widget, gpointer value)
{
    (void)widget; Admin *admin = value;
    if (admin->closed || database_busy(admin)) return;
    GtkWidget *form, *dialog = dialog_new(admin, "Tam bütünlük denetimi", "admin-db-full-dialog", "Başlat", &form);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 520, 240);
    pack(form, label("Tam denetim bütün veritabanını ve medya dosyalarını okur ve birkaç dakika sürebilir; "
        "başlangıçta kayıtlar kısa süre bekleyebilir.", "admin-db-full-warning"), FALSE);
    g_signal_connect(dialog, "response", G_CALLBACK(full_response), NULL); gtk_widget_show_all(dialog);
}
static void page_destroy(GtkWidget *page, gpointer value)
{
    (void)page; Admin *admin = value; admin->closed = TRUE;
    if (admin->poll) { g_source_remove(admin->poll); admin->poll = 0; }
    if (admin->db_job_poll) { g_source_remove(admin->db_job_poll); admin->db_job_poll = 0; }
    if (admin->timeout) { g_source_remove(admin->timeout); admin->timeout = 0; }
    if (admin->cancel) g_cancellable_cancel(admin->cancel);
    if (admin->child) g_subprocess_force_exit(admin->child);
}
void radios_admin_ui_attach(RadioSViewContext *ctx, GtkWidget *page,
    GtkWidget *server_content, GtkWidget *audit_content)
{
    Admin *admin = g_new0(Admin, 1); admin->references = 1; admin->ctx = ctx;
    g_weak_ref_init(&admin->page, page);
    g_object_set_data_full(G_OBJECT(page), "admin-controller", admin, admin_unref);
    g_signal_connect(page, "destroy", G_CALLBACK(page_destroy), admin);
    admin->expander = gtk_expander_new("Yerel servis, izleme ve kurtarma");
    gtk_widget_set_name(admin->expander, "admin-monitor-expander");
    GtkWidget *content = radios_ui_box(GTK_ORIENTATION_VERTICAL, 9, "technical-facts");
    gtk_container_add(GTK_CONTAINER(admin->expander), content); pack(server_content, admin->expander, FALSE);
    GtkWidget *actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 7, "technical-actions");
    button(actions, "Durumu yenile", "admin-refresh", G_CALLBACK(radios_admin_ui_refresh), page); pack(content, actions, FALSE);
    button(actions, "Bekleyen işlemleri uzlaştır", "admin-recovery-retry", G_CALLBACK(retry), admin);
    admin->health = label("", "admin-health-status"); pack(content, admin->health, FALSE);
    admin->audio = label("", "admin-audio-status"); pack(content, admin->audio, FALSE);
    admin->recovery = label("", "admin-recovery-status");
    pack(content, admin->recovery, FALSE);
    admin->runs = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, NULL); pack(content, admin->runs, FALSE);
    pack(content, radios_ui_label("Veritabanı durumu", "heading"), FALSE);
    GtkWidget *database = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 7, "technical-actions"); pack(content, database, FALSE);
    admin->db_refresh = button(database, "Durumu yenile", "admin-db-refresh", G_CALLBACK(database_refresh), admin);
    admin->db_full = button(database, "Tam denetim", "admin-db-full-check", G_CALLBACK(database_full_check), admin);
    const gchar *database_names[] = {"admin-db-state", "admin-db-metrics", "admin-db-integrity", "admin-db-issues"};
    GtkWidget **database_labels[] = {&admin->db_state, &admin->db_metrics, &admin->db_integrity, &admin->db_issues};
    for (guint i = 0; i < G_N_ELEMENTS(database_names); ++i) {
        *database_labels[i] = label("", database_names[i]);
        gtk_widget_set_no_show_all(*database_labels[i], TRUE); pack(content, *database_labels[i], FALSE);
    }
    gtk_label_set_selectable(GTK_LABEL(admin->db_issues), TRUE);
    pack(content, radios_ui_label("Yedek ve geri yükleme", "heading"), FALSE);
    admin->backups = gtk_combo_box_text_new(); gtk_widget_set_name(admin->backups, "admin-backup-selector"); pack(content, admin->backups, FALSE);
    admin->backup_actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 5, "technical-actions"); pack(content, admin->backup_actions, FALSE);
    const gchar *operations[] = {"list", "create", "verify", "restore-stage", "restore-plan"};
    const gchar *texts[] = {"Yedekleri listele", "Yedek oluştur", "Doğrula", "Ayrı dizinde dene", "Geri yükleme planı"};
    for (guint i = 0; i < G_N_ELEMENTS(operations); ++i) {
        gchar *name = g_strdup_printf("admin-backup-%s", operations[i]);
        GtkWidget *action = button(admin->backup_actions, texts[i], name, G_CALLBACK(tool_start), admin); g_free(name);
        g_object_set_data(G_OBJECT(action), "admin-tool-operation", (gpointer)operations[i]);
    }
    admin->backup_status = label("", "admin-backup-status");
    gtk_label_set_selectable(GTK_LABEL(admin->backup_status), TRUE); pack(content, admin->backup_status, FALSE);
    GtkWidget *audit_expander = gtk_expander_new("İstasyona ait son işlemler");
    gtk_widget_set_name(audit_expander, "admin-audit-expander");
    GtkWidget *audit_box = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, "technical-facts");
    GtkWidget *audit_actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 5, "technical-actions");
    button(audit_actions, "İşlem kaydını yenile", "admin-audit-refresh", G_CALLBACK(radios_admin_ui_refresh), page);
    pack(audit_box, audit_actions, FALSE); admin->audit = label("Henüz okunmadı.", "admin-audit-status"); pack(audit_box, admin->audit, FALSE);
    gtk_container_add(GTK_CONTAINER(audit_expander), audit_box); pack(audit_content, audit_expander, FALSE);
    g_signal_connect(audit_expander, "notify::expanded", G_CALLBACK(audit_expanded), admin);
    g_signal_connect(admin->expander, "notify::expanded", G_CALLBACK(expanded), admin);
    admin->poll = g_timeout_add_seconds(15, poll, admin);
}
