#include "radios/programme_prerequisites.h"
#include "radios/widgets.h"
#include "radios/media_ui.h"
#include <string.h>

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *dialog, *choice, *summary, *status, *output, *territory, *starts, *ends, *source, *decision;
    GtkWidget *buttons[6], *confirm;
    JsonNode *records;
    gchar document_id[37], selected_id[37];
    gboolean busy, closed, uncertain, applying;
} Prerequisites;
static void reload(Prerequisites *p);
static JsonObject *member(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonArray *array(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static JsonObject *selected(Prerequisites *p)
{
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->choice));
    JsonArray *rows = p->records ? array(json_node_get_object(p->records), "records") : NULL;
    for (guint i = 0; id && rows && i < json_array_get_length(rows); ++i) {
        JsonObject *record = json_array_get_object_element(rows, i);
        if (g_str_equal(id, radios_json_string(member(record, "version"), "_id", ""))) return record;
    }
    return NULL;
}
static void render(Prerequisites *p)
{
    JsonObject *record = selected(p), *version = member(record, "version"), *item = member(record, "item"), *association = member(record, "association");
    gboolean authorized = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->confirm));
    gboolean enabled = !p->busy && !p->uncertain && record && authorized;
    const gchar *state = radios_json_string(version, "status", ""), *life = radios_json_string(item, "lifecycle", "");
    gtk_widget_set_sensitive(p->buttons[0], enabled && (g_str_equal(state, "draft") || g_str_equal(state, "review")));
    gtk_widget_set_sensitive(p->buttons[1], enabled && (g_str_equal(life, "draft") || g_str_equal(life, "archived") || g_str_equal(life, "withdrawn")));
    /* json_node_get_boolean() would read an object or array node as a value. */
    JsonNode *eligible = association ? json_object_get_member(association, "eligible") : NULL;
    gboolean eligible_known = eligible && (JSON_NODE_HOLDS_VALUE(eligible) || JSON_NODE_HOLDS_NULL(eligible));
    gboolean is_eligible = eligible && JSON_NODE_HOLDS_VALUE(eligible) && json_node_get_boolean(eligible);
    gtk_widget_set_sensitive(p->buttons[2], enabled && eligible_known && !is_eligible);
    gtk_widget_set_sensitive(p->buttons[3], enabled);
    gtk_widget_set_sensitive(p->buttons[4], !p->busy);
    JsonArray *outputs = p->records ? array(json_node_get_object(p->records), "outputs") : NULL;
    gtk_widget_set_sensitive(p->buttons[5], enabled && outputs && !json_array_get_length(outputs));
    gtk_widget_set_sensitive(p->choice, !p->busy);
    if (!record) { gtk_label_set_text(GTK_LABEL(p->summary), "Kaydedilmiş programda doğrulanmış medya bağı bulunamadı."); return; }
    GString *summary = g_string_new(NULL);
    g_string_append_printf(summary, "%s\nSürüm: %s · İçerik: %s · İstasyon uygunluğu: %s", radios_json_string(item, "title", ""), state, life,
                           is_eligible ? "Uygun" : "Uygun değil");
    JsonArray *grants = array(record, "rights");
    for (guint i = 0; grants && i < MIN(json_array_get_length(grants), 12u); ++i) {
        JsonObject *grant = json_array_get_object_element(grants, i);
        g_string_append_printf(summary, "\nHak kaydı: %s · %s/%s · %s → %s · %s", radios_json_string(grant, "decision", ""),
            radios_json_string(grant, "territory", ""), radios_json_string(grant, "output_id", ""), radios_json_string(grant, "starts_at", ""),
            radios_json_string(grant, "ends_at", ""), radios_json_string(grant, "source_ref", ""));
    }
    gtk_label_set_text(GTK_LABEL(p->summary), summary->str); g_string_free(summary, TRUE);
}
static void selection_changed(GtkComboBox *combo, gpointer value)
{
    (void)combo; Prerequisites *p = value; if (p->applying) return;
    JsonObject *record = selected(p), *version = member(record, "version");
    const gchar *id = radios_json_string(version, "_id", "");
    if (g_strcmp0(p->selected_id, id)) {
        g_strlcpy(p->selected_id, id, sizeof p->selected_id);
        gtk_entry_set_text(GTK_ENTRY(p->starts), radios_json_string(record, "starts_at", ""));
        gtk_entry_set_text(GTK_ENTRY(p->ends), radios_json_string(record, "ends_at", ""));
        gtk_entry_set_text(GTK_ENTRY(p->source), "");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->confirm), FALSE);
    }
    render(p);
}
static void confirmed(GtkToggleButton *button, gpointer value)
{ (void)button; render(value); }
static void read_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer unused)
{
    (void)ctx; (void)unused; Prerequisites *p = g_object_get_data(G_OBJECT(owner), "programme-prerequisites");
    p->busy = FALSE;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        gtk_label_set_text(GTK_LABEL(p->status), error ? error->message : "İçerik koşulları okunamadı."); render(p); return;
    }
    g_clear_pointer(&p->records, json_node_free); p->records = json_node_copy(result);
    p->applying = TRUE; gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(p->choice));
    JsonObject *root = json_node_get_object(result); JsonArray *rows = array(root, "records"), *outputs = array(root, "outputs");
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        JsonObject *record = json_array_get_object_element(rows, i), *version = member(record, "version"), *item = member(record, "item");
        gchar *name = g_strdup_printf("%s · %s", radios_json_string(item, "title", ""), radios_json_string(version, "label", ""));
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->choice), radios_json_string(version, "_id", ""), name); g_free(name);
    }
    if (!*p->selected_id || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->choice), p->selected_id)) gtk_combo_box_set_active(GTK_COMBO_BOX(p->choice), 0);
    gchar *selected_output = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->output)));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(p->output));
    for (guint i = 0; outputs && i < json_array_get_length(outputs); ++i) {
        JsonObject *output = json_array_get_object_element(outputs, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->output), radios_json_string(output, "output_id", ""), radios_json_string(output, "output_id", ""));
    }
    if (!selected_output || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->output), selected_output)) gtk_combo_box_set_active(GTK_COMBO_BOX(p->output), 0);
    g_free(selected_output); p->applying = FALSE; p->uncertain = FALSE; selection_changed(NULL, p);
    gtk_label_set_text(GTK_LABEL(p->status), "Gerçek içerik ve hak kayıtları okundu. Her onay ve hak kararı ayrı uygulanır."); render(p);
}
static void reload(Prerequisites *p)
{
    if (p->busy || p->closed || !radios_data_connected(p->ctx)) return;
    JsonNode *body = radios_data_body(p->ctx); json_object_set_string_member(json_node_get_object(body), "document_id", p->document_id);
    p->busy = radios_data_request(p->ctx, p->dialog, "POST", "/v1/workflows/desktop-programme/prerequisites", body, read_done, NULL, NULL);
    json_node_free(body); render(p);
}
static void written(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer unused)
{
    (void)ctx; (void)unused; Prerequisites *p = g_object_get_data(G_OBJECT(owner), "programme-prerequisites"); p->busy = FALSE;
    if (error || !result) {
        p->uncertain = TRUE; gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->confirm), FALSE);
        gtk_label_set_text(GTK_LABEL(p->status), error ? error->message : "Yazının sonucu doğrulanamadı. Yeniden denemeden önce kayıtları yenileyin."); render(p); return;
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->confirm), FALSE); reload(p);
}
static gboolean instant_valid(const gchar *value)
{
    if (!value || !strchr(value, 'T')) return FALSE;
    /* UTC offset must be explicit; GLib otherwise accepts a local fallback. */
    const gchar *time = strchr(value, 'T'); if (!strchr(time, 'Z') && !strchr(time, '+') && !strchr(time, '-')) return FALSE;
    GDateTime *dt = g_date_time_new_from_iso8601(value, NULL); if (!dt) return FALSE; g_date_time_unref(dt); return TRUE;
}
static void apply(GtkButton *button, gpointer value)
{
    Prerequisites *p = value; guint operation = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "programme-prerequisite-operation"));
    if (operation == 4) { reload(p); return; }
    if (p->busy || p->uncertain || !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->confirm))) return;
    JsonObject *record = selected(p), *version = member(record, "version"), *item = member(record, "item"), *association = member(record, "association"); if (!record) return;
    JsonNode *body = radios_data_body(p->ctx); JsonObject *o = json_node_get_object(body); gchar *path = NULL;
    if (operation == 5) {
        path = g_strdup("/v1/workflows/desktop-advert/output-create");
        json_object_set_string_member(o, "output_id", "LOCAL_MAIN");
        gchar *key = g_uuid_string_random(); json_object_set_string_member(o, "idempotency_key", key); g_free(key);
    } else if (operation == 0) {
        path = g_strdup_printf("/v1/versions/%s/approve", radios_json_string(version, "_id", ""));
        json_object_set_int_member(o, "expected_revision", radios_json_int(version, "_rev", 0));
    } else if (operation == 1) {
        path = g_strdup_printf("/v1/items/%s/lifecycle", radios_json_string(item, "_id", ""));
        json_object_set_string_member(o, "target", "active"); json_object_set_int_member(o, "expected_revision", radios_json_int(item, "_rev", 0));
    } else if (operation == 2) {
        path = g_strdup_printf("/v1/items/%s/eligibility", radios_json_string(item, "_id", ""));
        json_object_set_boolean_member(o, "eligible", TRUE); json_object_set_int_member(o, "expected_revision", radios_json_int(association, "_rev", 0));
    } else {
        const gchar *start = gtk_entry_get_text(GTK_ENTRY(p->starts)), *end = gtk_entry_get_text(GTK_ENTRY(p->ends));
        const gchar *source = gtk_entry_get_text(GTK_ENTRY(p->source)), *territory = gtk_entry_get_text(GTK_ENTRY(p->territory));
        const gchar *output = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->output));
        if (!instant_valid(start) || !instant_valid(end) || !*source || !*territory || !output) {
            gtk_label_set_text(GTK_LABEL(p->status), "Hak kararında açık UTC ofsetli başlangıç/bitiş, bölge, gerçek çıkış ve izin kaynağı gerekir."); json_node_free(body); return;
        }
        GDateTime *a = g_date_time_new_from_iso8601(start, NULL), *b = g_date_time_new_from_iso8601(end, NULL);
        gboolean ordered = g_date_time_compare(a, b) < 0; g_date_time_unref(a); g_date_time_unref(b);
        if (!ordered) { json_node_free(body); gtk_label_set_text(GTK_LABEL(p->status), "Hak bitişi başlangıçtan sonra olmalı."); return; }
        path = g_strdup("/v1/rights"); json_object_set_string_member(o, "version_id", radios_json_string(version, "_id", ""));
        json_object_set_string_member(o, "territory", territory); json_object_set_string_member(o, "output_id", output);
        json_object_set_string_member(o, "starts_at", start); json_object_set_string_member(o, "ends_at", end); json_object_set_string_member(o, "source_ref", source);
        json_object_set_string_member(o, "decision", gtk_combo_box_get_active(GTK_COMBO_BOX(p->decision)) == 0 ? "allow" : "deny");
    }
    p->busy = radios_data_request(p->ctx, p->dialog, "POST", path, body, written, NULL, NULL);
    g_free(path); json_node_free(body); render(p);
}
static void destroyed(GtkWidget *widget, gpointer value)
{ (void)widget; ((Prerequisites *)value)->closed = TRUE; }
static void freed(gpointer value)
{ Prerequisites *p = value; g_clear_pointer(&p->records, json_node_free); g_free(p); }
static void field(GtkWidget *box, const gchar *label, GtkWidget *input)
{
    GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "ld-actions");
    gtk_box_pack_start(GTK_BOX(row), radios_ui_label(label, NULL), FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(row), input, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
}
void radios_programme_prerequisites_open(RadioSViewContext *ctx, GtkWidget *owner, const gchar *document_id)
{
    if (!g_uuid_string_is_valid(document_id) || !radios_data_connected(ctx)) return;
    Prerequisites *p = g_new0(Prerequisites, 1); p->ctx = ctx; g_strlcpy(p->document_id, document_id, sizeof p->document_id);
    p->dialog = gtk_dialog_new_with_buttons("Program içerik onayları ve kullanım hakları", ctx->parent, GTK_DIALOG_DESTROY_WITH_PARENT, "Kapat", GTK_RESPONSE_CLOSE, NULL);
    gtk_widget_set_name(p->dialog, "programme-prerequisites-dialog"); gtk_window_set_default_size(GTK_WINDOW(p->dialog), 760, 600);
    g_object_set_data_full(G_OBJECT(p->dialog), "programme-prerequisites", p, freed); g_signal_connect(p->dialog, "destroy", G_CALLBACK(destroyed), p);
    g_signal_connect_object(owner, "destroy", G_CALLBACK(gtk_widget_destroy), p->dialog, G_CONNECT_SWAPPED);
    g_signal_connect_swapped(p->dialog, "response", G_CALLBACK(gtk_widget_destroy), p->dialog);
    GtkWidget *box = gtk_dialog_get_content_area(GTK_DIALOG(p->dialog)); gtk_box_set_spacing(GTK_BOX(box), 8); gtk_container_set_border_width(GTK_CONTAINER(box), 14);
    p->choice = gtk_combo_box_text_new(); gtk_widget_set_name(p->choice, "programme-prerequisite-version"); field(box, "Medya sürümü", p->choice);
    p->summary = radios_ui_label("İçerik kayıtları okunuyor…", "ld-head-meta"); gtk_label_set_line_wrap(GTK_LABEL(p->summary), TRUE); gtk_label_set_selectable(GTK_LABEL(p->summary), TRUE);
    gtk_box_pack_start(GTK_BOX(box), p->summary, FALSE, FALSE, 0);
    p->territory = radios_ui_entry("Bölge kodu veya *"); gtk_widget_set_name(p->territory, "programme-rights-territory"); gtk_entry_set_max_length(GTK_ENTRY(p->territory), 100); field(box, "Bölge", p->territory);
    p->output = gtk_combo_box_text_new(); gtk_widget_set_name(p->output, "programme-rights-output"); field(box, "Kayıtlı çıkış", p->output);
    p->starts = radios_ui_entry("2026-10-06T00:00:00Z"); gtk_widget_set_name(p->starts, "programme-rights-starts"); field(box, "Hak başlangıcı", p->starts);
    p->ends = radios_ui_entry("2026-10-07T00:00:00Z"); gtk_widget_set_name(p->ends, "programme-rights-ends"); field(box, "Hak bitişi", p->ends);
    p->source = radios_ui_entry("Sözleşme / izin kaynağı"); gtk_widget_set_name(p->source, "programme-rights-source"); gtk_entry_set_max_length(GTK_ENTRY(p->source), 500); field(box, "İzin dayanağı", p->source);
    const gchar *choices[] = {"İzin ver", "Yasakla", NULL}; p->decision = radios_ui_combo(choices, 0); gtk_widget_set_name(p->decision, "programme-rights-decision"); field(box, "Hak kararı", p->decision);
    p->confirm = gtk_check_button_new_with_label("Seçilen içerik için uygulayacağım kararı ve izin dayanağını kontrol ettim."); gtk_widget_set_name(p->confirm, "programme-prerequisite-confirm"); gtk_box_pack_start(GTK_BOX(box), p->confirm, FALSE, FALSE, 0);
    const gchar *names[] = {"Sürümü onayla", "İçeriği etkinleştir", "İstasyona uygun yap", "Hak kararını kaydet", "Kayıtları yenile", "Yerel MAIN çıkışını kaydet"};
    const gchar *ids[] = {"programme-version-approve", "programme-item-activate", "programme-item-eligible", "programme-rights-save", "programme-prerequisites-refresh", "programme-output-create"};
    GtkWidget *actions = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(p->buttons); ++i) {
        if (!(i % 3)) {
            actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 5, "ld-actions");
            gtk_box_pack_start(GTK_BOX(box), actions, FALSE, FALSE, 0);
        }
        p->buttons[i] = radios_ui_button(names[i]); gtk_widget_set_name(p->buttons[i], ids[i]); g_object_set_data(G_OBJECT(p->buttons[i]), "programme-prerequisite-operation", GUINT_TO_POINTER(i));
        g_signal_connect(p->buttons[i], "clicked", G_CALLBACK(apply), p); gtk_box_pack_start(GTK_BOX(actions), p->buttons[i], FALSE, FALSE, 0);
    }
    p->status = radios_ui_label("", "ld-head-meta");
    gtk_widget_set_name(p->status, "programme-prerequisites-status"); gtk_label_set_line_wrap(GTK_LABEL(p->status), TRUE); gtk_box_pack_start(GTK_BOX(box), p->status, FALSE, FALSE, 0);
    g_signal_connect(p->choice, "changed", G_CALLBACK(selection_changed), p); g_signal_connect(p->confirm, "toggled", G_CALLBACK(confirmed), p);
    gtk_widget_show_all(p->dialog); reload(p);
}

/* The media-only rights editor shares the real grant endpoint with programme
 * prerequisites. It never approves content or creates outputs implicitly. */
typedef struct {
    RadioSViewContext *ctx;
    RadioSDataClient *client;
    GtkWidget *dialog, *pick, *grants, *summary, *status, *output, *territory, *starts, *ends, *source, *decision, *confirm, *save, *refresh, *previous, *next;
    JsonNode *records, *pending, *acknowledged, *uncertain_intent;
    gchar media_id[37], version_id[37], item_id[37], file_id[37], sha[65], pending_output[37], selected_grant[37];
    gint64 pending_output_revision;
    guint offset;
    gboolean busy, closed, uncertain, applying;
} MediaRights;
static void media_rights_reload(MediaRights *p);
static gboolean media_rights_current(MediaRights *p)
{
    RadioSDataClient *current = radios_data_client_acquire(p->ctx);
    gboolean same = !p->closed && p->client && current == p->client && radios_data_connected(p->ctx);
    if (current) radios_data_client_unref(current);
    return same;
}
static gboolean rights_boolean(JsonObject *o, const gchar *key, gboolean *value)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_BOOLEAN) return FALSE;
    *value = json_node_get_boolean(n); return TRUE;
}
static JsonObject *rights_record(MediaRights *p)
{
    JsonArray *rows = p->records ? array(json_node_get_object(p->records), "records") : NULL;
    return rows && json_array_get_length(rows) == 1 ? json_array_get_object_element(rows, 0) : NULL;
}
static JsonObject *rights_grant(MediaRights *p, const gchar *id)
{
    JsonArray *rows = array(rights_record(p), "rights");
    for (guint i = 0; id && rows && i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        if (g_str_equal(id, radios_json_string(row, "_id", ""))) return row;
    }
    return NULL;
}
static JsonObject *rights_output(MediaRights *p, const gchar *id)
{
    JsonArray *rows = p->records ? array(json_node_get_object(p->records), "outputs") : NULL;
    for (guint i = 0; id && rows && i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        if (g_str_equal(id, radios_json_string(row, "_id", ""))) return row;
    }
    return NULL;
}
static void media_rights_render(MediaRights *p)
{
    gboolean current = media_rights_current(p), can_approve = FALSE;
    if (p->records) rights_boolean(json_node_get_object(p->records), "can_approve", &can_approve);
    gboolean ready = current && !p->busy, loaded = rights_record(p) != NULL;
    gtk_widget_set_sensitive(p->pick, ready);
    gtk_widget_set_sensitive(p->refresh, ready && *p->media_id);
    gtk_widget_set_sensitive(p->save, ready && loaded && can_approve && !p->uncertain &&
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->confirm)) && rights_output(p, gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->output))));
    GtkWidget *inputs[] = {p->grants,p->output,p->territory,p->starts,p->ends,p->source,p->decision,p->confirm};
    for (guint i = 0; i < G_N_ELEMENTS(inputs); ++i) gtk_widget_set_sensitive(inputs[i], ready && loaded);
    JsonObject *root = p->records ? json_node_get_object(p->records) : NULL;
    gtk_widget_set_sensitive(p->previous, ready && root && p->offset > 0);
    JsonNode *next_offset = root ? json_object_get_member(root, "rights_next_offset") : NULL;
    gtk_widget_set_sensitive(p->next, ready && next_offset && !JSON_NODE_HOLDS_NULL(next_offset));
}
static void rights_changed(GtkWidget *widget, gpointer value)
{ (void)widget; MediaRights *p = value; if (!p->applying) media_rights_render(p); }
static void rights_grant_changed(GtkComboBox *combo, gpointer value)
{
    MediaRights *p = value; if (p->applying) return;
    const gchar *id = gtk_combo_box_get_active_id(combo); JsonObject *grant = rights_grant(p, id);
    g_strlcpy(p->selected_grant, grant ? id : "", sizeof p->selected_grant);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->confirm), FALSE);
    if (grant) {
        gtk_entry_set_text(GTK_ENTRY(p->territory), radios_json_string(grant, "territory", ""));
        gtk_entry_set_text(GTK_ENTRY(p->starts), radios_json_string(grant, "starts_at", ""));
        gtk_entry_set_text(GTK_ENTRY(p->ends), radios_json_string(grant, "ends_at", ""));
        gtk_entry_set_text(GTK_ENTRY(p->source), radios_json_string(grant, "source_ref", ""));
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->decision), radios_json_string(grant, "decision", "allow"));
        JsonArray *outputs = array(json_node_get_object(p->records), "outputs");
        gtk_combo_box_set_active(GTK_COMBO_BOX(p->output), -1);
        for (guint i = 0; outputs && i < json_array_get_length(outputs); ++i) {
            JsonObject *output = json_array_get_object_element(outputs, i);
            if (g_str_equal(radios_json_string(output, "output_id", ""), radios_json_string(grant, "output_id", ""))) {
                gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->output), radios_json_string(output, "_id", "")); break;
            }
        }
    }
    media_rights_render(p);
}
static gboolean rights_response_valid(MediaRights *p, JsonObject *root)
{
    gboolean can_approve; JsonArray *rows = array(root, "records"), *outputs = array(root, "outputs");
    if (!root || radios_json_int(root, "contract_version", 0) != 1 ||
        !g_str_equal(radios_json_string(root, "station_id", ""), radios_data_client_station_id(p->client)) ||
        !g_str_equal(radios_json_string(root, "media_id", ""), p->media_id) || !g_str_equal(radios_json_string(root, "version_id", ""), p->version_id) ||
        !rights_boolean(root, "can_approve", &can_approve) || !rows || json_array_get_length(rows) != 1 || !outputs || json_array_get_length(outputs) > 200 ||
        radios_json_int(root, "rights_offset", -1) < 0 || radios_json_int(root, "rights_total", -1) < 0) return FALSE;
    if (!JSON_NODE_HOLDS_OBJECT(json_array_get_element(rows, 0))) return FALSE;
    JsonObject *record = json_array_get_object_element(rows, 0), *version = member(record, "version"), *item = member(record, "item"), *media = member(record, "media"), *file = member(record, "file"), *association = member(record, "association");
    JsonArray *grants = array(record, "rights");
    if (!version || !item || !media || !file || !association || !grants || json_array_get_length(grants) > 100 ||
        !g_str_equal(radios_json_string(version, "_id", ""), p->version_id) || !g_str_equal(radios_json_string(version, "item_id", ""), p->item_id) ||
        !g_str_equal(radios_json_string(item, "_id", ""), p->item_id) || !g_str_equal(radios_json_string(media, "_id", ""), p->media_id) ||
        !g_str_equal(radios_json_string(media, "version_id", ""), p->version_id) || !g_str_equal(radios_json_string(media, "item_id", ""), p->item_id) ||
        !g_str_equal(radios_json_string(media, "station_id", ""), radios_data_client_station_id(p->client)) ||
        !g_str_equal(radios_json_string(file, "_id", ""), p->file_id) || !g_str_equal(radios_json_string(file, "version_id", ""), p->version_id) ||
        !g_str_equal(radios_json_string(file, "status", ""), "verified") || !g_str_equal(radios_json_string(file, "sha256", ""), p->sha) ||
        !g_str_equal(radios_json_string(association, "station_id", ""), radios_data_client_station_id(p->client)) || !g_str_equal(radios_json_string(association, "item_id", ""), p->item_id)) return FALSE;
    for (guint i = 0; i < json_array_get_length(grants); ++i) {
        JsonNode *node = json_array_get_element(grants, i); if (!JSON_NODE_HOLDS_OBJECT(node)) return FALSE;
        JsonObject *grant = json_node_get_object(node);
        const gchar *decision = radios_json_string(grant, "decision", "");
        if (!g_uuid_string_is_valid(radios_json_string(grant, "_id", "")) || !g_str_equal(radios_json_string(grant, "station_id", ""), radios_data_client_station_id(p->client)) ||
            !g_str_equal(radios_json_string(grant, "version_id", ""), p->version_id) || (!g_str_equal(decision,"allow") && !g_str_equal(decision,"deny")) ||
            !instant_valid(radios_json_string(grant,"starts_at","")) || !instant_valid(radios_json_string(grant,"ends_at",""))) return FALSE;
    }
    for (guint i = 0; i < json_array_get_length(outputs); ++i) {
        JsonNode *node = json_array_get_element(outputs, i); if (!JSON_NODE_HOLDS_OBJECT(node)) return FALSE;
        JsonObject *output = json_node_get_object(node);
        if (!g_uuid_string_is_valid(radios_json_string(output,"_id","")) || !g_uuid_string_is_valid(radios_json_string(output,"endpoint_id","")) ||
            !g_str_equal(radios_json_string(output,"station_id",""),radios_data_client_station_id(p->client)) ||
            !g_str_equal(radios_json_string(output,"status",""),"active") || radios_json_int(output,"_rev",0) < 1 || !*radios_json_string(output,"output_id","")) return FALSE;
    }
    return TRUE;
}
static gboolean rights_intent_matches(MediaRights *p, JsonObject *grant, JsonNode *intent)
{
    JsonObject *body = intent ? json_node_get_object(intent) : NULL;
    if (!grant || !body || !g_uuid_string_is_valid(radios_json_string(grant,"_id","")) ||
        !g_str_equal(radios_json_string(grant,"approved_by",""),radios_data_client_user_id(p->client))) return FALSE;
    const gchar *fields[]={"station_id","version_id","territory","output_id","decision","source_ref"};
    for(guint i=0;i<G_N_ELEMENTS(fields);++i)if(!g_str_equal(radios_json_string(grant,fields[i],""),radios_json_string(body,fields[i],"")))return FALSE;
    const gchar *dates[]={"starts_at","ends_at"};
    for(guint i=0;i<2;++i){GDateTime *a=g_date_time_new_from_iso8601(radios_json_string(grant,dates[i],""),NULL),*b=g_date_time_new_from_iso8601(radios_json_string(body,dates[i],""),NULL);gboolean valid=a&&b&&g_date_time_compare(a,b)==0;if(a)g_date_time_unref(a);if(b)g_date_time_unref(b);if(!valid)return FALSE;}
    return TRUE;
}
static void rights_written(RadioSViewContext *ctx,GtkWidget *owner,JsonNode *result,const GError *error,gpointer unused)
{
    (void)ctx;(void)unused;MediaRights *p=g_object_get_data(G_OBJECT(owner),"media-rights");p->busy=FALSE;
    if(!media_rights_current(p)) {p->uncertain=TRUE;gtk_label_set_text(GTK_LABEL(p->status),"Veri oturumu değişti; hak sonucunu özgün oturumda yeniden okuyun.");media_rights_render(p);return;}
    JsonObject *grant=result&&JSON_NODE_HOLDS_OBJECT(result)?json_node_get_object(result):NULL;
    if(error||!rights_intent_matches(p,grant,p->pending)){p->uncertain=TRUE;g_clear_pointer(&p->uncertain_intent,json_node_free);p->uncertain_intent=p->pending;p->pending=NULL;gtk_label_set_text(GTK_LABEL(p->status),error?error->message:"Hak yazısının sonucu doğrulanamadı; kayıtları yenileyip kontrol edin.");media_rights_render(p);return;}
    g_clear_pointer(&p->acknowledged,json_node_free);p->acknowledged=json_node_copy(result);g_strlcpy(p->selected_grant,radios_json_string(grant,"_id",""),sizeof p->selected_grant);g_clear_pointer(&p->pending,json_node_free);media_rights_reload(p);
}
static void rights_read(RadioSViewContext *ctx,GtkWidget *owner,JsonNode *result,const GError *error,gpointer unused)
{
    (void)ctx;(void)unused;MediaRights *p=g_object_get_data(G_OBJECT(owner),"media-rights");p->busy=FALSE;JsonObject *root=result&&JSON_NODE_HOLDS_OBJECT(result)?json_node_get_object(result):NULL;
    if(!media_rights_current(p)||error||!rights_response_valid(p,root)){p->uncertain=TRUE;g_clear_pointer(&p->pending,json_node_free);gtk_label_set_text(GTK_LABEL(p->status),error?error->message:"Medya, sürüm veya oturumla eşleşen hak kayıtları okunamadı.");media_rights_render(p);return;}
    g_clear_pointer(&p->records,json_node_free);p->records=json_node_copy(result);p->offset=(guint)radios_json_int(root,"rights_offset",0);
    if(p->pending){
        gboolean can_approve=FALSE;rights_boolean(root,"can_approve",&can_approve);JsonObject *output=rights_output(p,p->pending_output),*body=json_node_get_object(p->pending);
        if(!can_approve||!output||radios_json_int(output,"_rev",0)!=p->pending_output_revision||!g_str_equal(radios_json_string(output,"output_id",""),radios_json_string(body,"output_id",""))){g_clear_pointer(&p->pending,json_node_free);gtk_label_set_text(GTK_LABEL(p->status),"Onay yetkisi veya kayıtlı çıkış değişti; kayıtları kontrol edip yeniden seçin.");media_rights_render(p);return;}
        JsonArray *matching = array(root, "matching_grants");
        if (!matching || json_array_get_length(matching) > 100) { g_clear_pointer(&p->pending,json_node_free);p->uncertain=TRUE;gtk_label_set_text(GTK_LABEL(p->status),"Hak kararının mevcut kayıtları doğrulanamadı.");media_rights_render(p);return; }
        if (json_array_get_length(matching)) {
            JsonNode *grant = json_array_get_element(matching, 0);
            if (!JSON_NODE_HOLDS_OBJECT(grant) || !rights_intent_matches(p,json_node_get_object(grant),p->pending)) { g_clear_pointer(&p->pending,json_node_free);p->uncertain=TRUE;media_rights_render(p);return; }
            g_clear_pointer(&p->acknowledged,json_node_free);p->acknowledged=json_node_copy(grant);
            g_strlcpy(p->selected_grant,radios_json_string(json_node_get_object(grant),"_id",""),sizeof p->selected_grant);
            g_clear_pointer(&p->pending,json_node_free);
        } else {
            p->busy=TRUE;gboolean accepted=radios_data_request(p->ctx,p->dialog,"POST","/v1/rights",p->pending,rights_written,NULL,NULL);
            if(!accepted){p->busy=FALSE;g_clear_pointer(&p->pending,json_node_free);gtk_label_set_text(GTK_LABEL(p->status),"Hak yazısı gönderilemedi.");}media_rights_render(p);return;
        }
    }
    if (p->uncertain_intent) {
        JsonArray *persisted = array(root, "matching_grants");
        for (guint i = 0; persisted && i < MIN(json_array_get_length(persisted), 100u); ++i) {
            JsonNode *node = json_array_get_element(persisted, i);
            if (!JSON_NODE_HOLDS_OBJECT(node)) continue;
            JsonObject *grant = json_node_get_object(node);
            if (!rights_intent_matches(p, grant, p->uncertain_intent)) continue;
            g_clear_pointer(&p->acknowledged, json_node_free);
            p->acknowledged = json_node_new(JSON_NODE_OBJECT); json_node_set_object(p->acknowledged, grant);
            g_strlcpy(p->selected_grant, radios_json_string(grant, "_id", ""), sizeof p->selected_grant);
            g_clear_pointer(&p->uncertain_intent, json_node_free); break;
        }
    }
    gchar *output_id=g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->output)));p->applying=TRUE;gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(p->output));JsonArray *outputs=array(root,"outputs");
    for(guint i=0;i<json_array_get_length(outputs);++i){JsonObject *o=json_array_get_object_element(outputs,i);gchar *label=g_strdup_printf("%s · %s",radios_json_string(o,"output_id",""),radios_json_string(o,"endpoint_id",""));gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->output),radios_json_string(o,"_id",""),label);g_free(label);}
    if (!output_id || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->output), output_id)) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(p->output), 0);
    }
    g_free(output_id);
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(p->grants));gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->grants),"new","Yeni hak kararı");JsonArray *grants=array(rights_record(p),"rights");
    for(guint i=0;i<json_array_get_length(grants);++i){JsonObject *g=json_array_get_object_element(grants,i);gchar *label=g_strdup_printf("%s · %s/%s · %s → %s",radios_json_string(g,"decision",""),radios_json_string(g,"territory",""),radios_json_string(g,"output_id",""),radios_json_string(g,"starts_at",""),radios_json_string(g,"ends_at",""));gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->grants),radios_json_string(g,"_id",""),label);g_free(label);}
    if (!*p->selected_grant || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->grants), p->selected_grant)) {
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(p->grants), "new");
    }
    p->applying = FALSE;
    gboolean confirmed=TRUE;if(p->acknowledged){JsonObject *grant=rights_grant(p,radios_json_string(json_node_get_object(p->acknowledged),"_id",""));JsonNode *node=grant?json_node_new(JSON_NODE_OBJECT):NULL;if(node)json_node_set_object(node,grant);confirmed=node&&json_node_equal(node,p->acknowledged);if(node)json_node_free(node);}
    p->uncertain=!confirmed || p->uncertain_intent != NULL;rights_grant_changed(GTK_COMBO_BOX(p->grants),p);JsonObject *record=rights_record(p);gchar *summary=g_strdup_printf("%s · %s\nToplam %"G_GINT64_FORMAT" hak kaydı; gösterilen sayfa %u–%u. Düzenlenen bilgiler ayrı bir yeni hak kararı olarak kaydedilir.",radios_json_string(member(record,"item"),"title",""),radios_json_string(member(record,"version"),"label",""),radios_json_int(root,"rights_total",0),p->offset,p->offset+json_array_get_length(grants));gtk_label_set_text(GTK_LABEL(p->summary),summary);g_free(summary);
    gtk_label_set_text(GTK_LABEL(p->status),p->uncertain_intent?"Önceki yazı henüz doğrulanamadı. Hak sayfalarını okuyup mevcut kararı kontrol edin; otomatik tekrar gönderilmez.":!confirmed?"Kaydedilen karar yeniden okunamadı; tekrar yazmadan önce kayıtları yenileyin.":p->acknowledged?"Kaydedilen hak kararı gerçek kayıttan yeniden okundu.":"Gerçek hak kayıtları okundu. Hak kaydı içerik veya program onayı yerine geçmez.");media_rights_render(p);
}
static void media_rights_reload(MediaRights *p)
{
    if (p->busy || !media_rights_current(p) || !*p->media_id) {
        return;
    }
    JsonNode *body = radios_data_body(p->ctx);JsonObject *o=json_node_get_object(body);json_object_set_string_member(o,"media_id",p->media_id);json_object_set_string_member(o,"version_id",p->version_id);json_object_set_int_member(o,"offset",p->offset);json_object_set_int_member(o,"limit",50);if(*p->selected_grant)json_object_set_string_member(o,"grant_id",p->selected_grant);
    JsonNode *intent = p->pending ? p->pending : p->uncertain_intent;
    if (intent) {
        JsonObject *terms = json_object_new(), *source = json_node_get_object(intent);
        const gchar *fields[] = {"territory","output_id","starts_at","ends_at","decision","source_ref"};
        for (guint i=0;i<G_N_ELEMENTS(fields);++i) json_object_set_member(terms,fields[i],json_node_copy(json_object_get_member(source,fields[i])));
        json_object_set_object_member(o,"intent",terms);
    }
    p->busy=radios_data_request(p->ctx,p->dialog,"POST","/v1/workflows/desktop-media/rights",body,rights_read,NULL,NULL);json_node_free(body);if(!p->busy){g_clear_pointer(&p->pending,json_node_free);gtk_label_set_text(GTK_LABEL(p->status),"Hak kayıtları isteği gönderilemedi.");}media_rights_render(p);
}
static void rights_picked(RadioSViewContext *ctx,GtkWidget *owner,JsonObject *detail,gpointer unused)
{
    (void)ctx;(void)unused;MediaRights *p=g_object_get_data(G_OBJECT(owner),"media-rights");if(!p||p->busy||!media_rights_current(p))return;JsonObject *media=member(detail,"media"),*version=member(detail,"version");gboolean verified=FALSE,available=FALSE;
    const gchar *mid=radios_json_string(media,"_id",""),*vid=radios_json_string(version,"_id",""),*iid=radios_json_string(media,"item_id",""),*fid=radios_json_string(media,"media_file_id",""),*sha=radios_json_string(version,"sha256","");
    if(!g_uuid_string_is_valid(mid)||!g_uuid_string_is_valid(vid)||!g_uuid_string_is_valid(iid)||!g_uuid_string_is_valid(fid)||!g_str_equal(radios_json_string(media,"station_id",""),radios_data_client_station_id(p->client))||!g_str_equal(radios_json_string(version,"item_id",""),iid)||!g_str_equal(radios_json_string(media,"version_id",""),vid)||!rights_boolean(version,"verified",&verified)||!verified||!rights_boolean(version,"object_available",&available)||!available||!g_regex_match_simple("^[0-9a-f]{64}$",sha,0,0)){gtk_label_set_text(GTK_LABEL(p->status),"İstasyona ait doğrulanmış medya sürümü seçilmeli.");return;}
    g_strlcpy(p->media_id,mid,sizeof p->media_id);g_strlcpy(p->version_id,vid,sizeof p->version_id);g_strlcpy(p->item_id,iid,sizeof p->item_id);g_strlcpy(p->file_id,fid,sizeof p->file_id);g_strlcpy(p->sha,sha,sizeof p->sha);p->offset=0;p->selected_grant[0]='\0';p->uncertain=FALSE;g_clear_pointer(&p->records,json_node_free);g_clear_pointer(&p->acknowledged,json_node_free);g_clear_pointer(&p->uncertain_intent,json_node_free);gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->confirm),FALSE);gtk_entry_set_text(GTK_ENTRY(p->source),"");media_rights_reload(p);
}
static void rights_pick_clicked(GtkButton *button,gpointer value)
{(void)button;MediaRights *p=value;if(!p->busy&&media_rights_current(p))radios_media_library_pick(p->ctx,p->dialog,rights_picked,NULL);}
static void rights_action(GtkButton *button,gpointer value)
{
    MediaRights *p=value;if(p->busy||!media_rights_current(p))return;
    if(GTK_WIDGET(button)==p->refresh){media_rights_reload(p);return;}
    if(GTK_WIDGET(button)==p->previous||GTK_WIDGET(button)==p->next){JsonObject *root=p->records?json_node_get_object(p->records):NULL;if(!root)return;p->offset=GTK_WIDGET(button)==p->previous?(p->offset>=50?p->offset-50:0):(guint)radios_json_int(root,"rights_next_offset",p->offset);p->selected_grant[0]='\0';g_clear_pointer(&p->acknowledged,json_node_free);media_rights_reload(p);return;}
    gboolean can_approve=FALSE;if(p->records)rights_boolean(json_node_get_object(p->records),"can_approve",&can_approve);if(p->uncertain||!can_approve||!rights_record(p)||!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(p->confirm)))return;
    const gchar *a=gtk_entry_get_text(GTK_ENTRY(p->starts)),*b=gtk_entry_get_text(GTK_ENTRY(p->ends)),*territory=gtk_entry_get_text(GTK_ENTRY(p->territory)),*source=gtk_entry_get_text(GTK_ENTRY(p->source)),*decision=gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->decision));JsonObject *output=rights_output(p,gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->output)));
    if(!instant_valid(a)||!instant_valid(b)||!*territory||!*source||!output||!decision){gtk_label_set_text(GTK_LABEL(p->status),"Açık UTC ofseti, bölge, kayıtlı çıkış ve izin kaynağı gerekli.");return;}GDateTime *start=g_date_time_new_from_iso8601(a,NULL),*end=g_date_time_new_from_iso8601(b,NULL);gboolean valid=g_date_time_compare(start,end)<0;g_date_time_unref(start);g_date_time_unref(end);if(!valid){gtk_label_set_text(GTK_LABEL(p->status),"Hak bitişi başlangıçtan sonra olmalı.");return;}
    g_clear_pointer(&p->pending,json_node_free);p->pending=radios_data_body(p->ctx);JsonObject *o=json_node_get_object(p->pending);json_object_set_string_member(o,"version_id",p->version_id);json_object_set_string_member(o,"territory",territory);json_object_set_string_member(o,"output_id",radios_json_string(output,"output_id",""));json_object_set_string_member(o,"starts_at",a);json_object_set_string_member(o,"ends_at",b);json_object_set_string_member(o,"decision",decision);json_object_set_string_member(o,"source_ref",source);g_strlcpy(p->pending_output,radios_json_string(output,"_id",""),sizeof p->pending_output);p->pending_output_revision=radios_json_int(output,"_rev",0);gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(p->confirm),FALSE);media_rights_reload(p);
}
static void rights_destroyed(GtkWidget *widget,gpointer value){(void)widget;((MediaRights *)value)->closed=TRUE;}
static void rights_freed(gpointer value){MediaRights *p=value;if(p->client)radios_data_client_unref(p->client);g_clear_pointer(&p->records,json_node_free);g_clear_pointer(&p->pending,json_node_free);g_clear_pointer(&p->acknowledged,json_node_free);g_clear_pointer(&p->uncertain_intent,json_node_free);g_free(p);}
static GtkWidget *rights_input(GtkWidget *box,const gchar *label,const gchar *name,const gchar *placeholder,guint maximum)
{GtkWidget *w=radios_ui_entry(placeholder);gtk_widget_set_name(w,name);gtk_entry_set_max_length(GTK_ENTRY(w),(gint)maximum);field(box,label,w);return w;}
void radios_media_rights_open(RadioSViewContext *ctx,GtkWidget *owner)
{
    if(!ctx||!owner||gtk_widget_in_destruction(owner)||!radios_data_connected(ctx))return;
    MediaRights *p=g_new0(MediaRights,1);p->ctx=ctx;p->client=radios_data_client_acquire(ctx);if(!p->client){g_free(p);return;}
    p->dialog=gtk_dialog_new_with_buttons("Yayın ve medya kullanım hakları",GTK_IS_WINDOW(gtk_widget_get_toplevel(owner))?GTK_WINDOW(gtk_widget_get_toplevel(owner)):ctx->parent,GTK_DIALOG_DESTROY_WITH_PARENT,"Kapat",GTK_RESPONSE_CLOSE,NULL);gtk_widget_set_name(p->dialog,"media-rights-dialog");gtk_window_set_default_size(GTK_WINDOW(p->dialog),920,620);g_object_set_data_full(G_OBJECT(p->dialog),"media-rights",p,rights_freed);g_signal_connect(p->dialog,"destroy",G_CALLBACK(rights_destroyed),p);g_signal_connect_object(owner,"destroy",G_CALLBACK(gtk_widget_destroy),p->dialog,G_CONNECT_SWAPPED);g_signal_connect_swapped(p->dialog,"response",G_CALLBACK(gtk_widget_destroy),p->dialog);
    GtkWidget *scroll=gtk_scrolled_window_new(NULL,NULL);gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll),GTK_POLICY_AUTOMATIC,GTK_POLICY_ALWAYS);gtk_scrolled_window_set_overlay_scrolling(GTK_SCROLLED_WINDOW(scroll),FALSE);gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(p->dialog))),scroll,TRUE,TRUE,0);GtkWidget *box=radios_ui_box(GTK_ORIENTATION_VERTICAL,8,"ld-actions");gtk_container_set_border_width(GTK_CONTAINER(box),14);gtk_container_add(GTK_CONTAINER(scroll),box);
    p->pick=radios_ui_button("Kütüphaneden medya seç");gtk_widget_set_name(p->pick,"media-rights-pick");gtk_box_pack_start(GTK_BOX(box),p->pick,FALSE,FALSE,0);g_signal_connect(p->pick,"clicked",G_CALLBACK(rights_pick_clicked),p);
    p->summary=radios_ui_label("","ld-head-meta");gtk_widget_set_name(p->summary,"media-rights-summary");gtk_label_set_line_wrap(GTK_LABEL(p->summary),TRUE);gtk_label_set_selectable(GTK_LABEL(p->summary),TRUE);gtk_box_pack_start(GTK_BOX(box),p->summary,FALSE,FALSE,0);
    p->grants=gtk_combo_box_text_new();gtk_widget_set_name(p->grants,"media-rights-grant");field(box,"Mevcut karar / yeni karar",p->grants);p->territory=rights_input(box,"Bölge","media-rights-territory","Bölge kodu veya *",100);gtk_entry_set_text(GTK_ENTRY(p->territory),"*");p->output=gtk_combo_box_text_new();gtk_widget_set_name(p->output,"media-rights-output");field(box,"Kayıtlı yayın çıkışı",p->output);
    p->starts=rights_input(box,"Hak başlangıcı · UTC","media-rights-starts","2026-10-11T00:00:00Z",80);p->ends=rights_input(box,"Hak bitişi · UTC","media-rights-ends","2026-10-12T00:00:00Z",80);GDateTime *now=g_date_time_new_now_utc(),*later=g_date_time_add_days(now,1);gchar *start=g_date_time_format_iso8601(now),*end=g_date_time_format_iso8601(later);gtk_entry_set_text(GTK_ENTRY(p->starts),start);gtk_entry_set_text(GTK_ENTRY(p->ends),end);g_free(start);g_free(end);g_date_time_unref(now);g_date_time_unref(later);
    p->source=rights_input(box,"Sözleşme / izin kaynağı","media-rights-source","İzin dayanağı",500);p->decision=gtk_combo_box_text_new();gtk_widget_set_name(p->decision,"media-rights-decision");gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->decision),"allow","İzin ver");gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->decision),"deny","Yasakla");gtk_combo_box_set_active(GTK_COMBO_BOX(p->decision),0);field(box,"Hak kararı",p->decision);
    p->confirm=gtk_check_button_new_with_label("Seçili sürümün bölgesini, çıkışını, zaman aralığını ve izin dayanağını kontrol ettim.");gtk_widget_set_name(p->confirm,"media-rights-confirm");gtk_box_pack_start(GTK_BOX(box),p->confirm,FALSE,FALSE,0);GtkWidget *actions=radios_ui_box(GTK_ORIENTATION_HORIZONTAL,6,"ld-actions");gtk_box_pack_start(GTK_BOX(box),actions,FALSE,FALSE,0);GtkWidget **buttons[]={&p->save,&p->refresh,&p->previous,&p->next};const gchar *labels[]={"Yeni hak kararını kaydet","Kayıtları yenile","Önceki hak sayfası","Sonraki hak sayfası"};const gchar *names[]={"media-rights-save","media-rights-refresh","media-rights-previous","media-rights-next"};for(guint i=0;i<4;++i){*buttons[i]=radios_ui_button(labels[i]);gtk_widget_set_name(*buttons[i],names[i]);g_signal_connect(*buttons[i],"clicked",G_CALLBACK(rights_action),p);gtk_box_pack_start(GTK_BOX(actions),*buttons[i],FALSE,FALSE,0);}
    p->status=radios_ui_label("","ld-head-meta");gtk_widget_set_name(p->status,"media-rights-status");gtk_label_set_line_wrap(GTK_LABEL(p->status),TRUE);gtk_box_pack_start(GTK_BOX(box),p->status,FALSE,FALSE,0);g_signal_connect(p->grants,"changed",G_CALLBACK(rights_grant_changed),p);g_signal_connect(p->output,"changed",G_CALLBACK(rights_changed),p);g_signal_connect(p->confirm,"toggled",G_CALLBACK(rights_changed),p);gtk_widget_show_all(p->dialog);media_rights_render(p);
}
