#include "radios/programme_lifecycle.h"
#include "radios/playout_ui.h"
#include "radios/programme_prerequisites.h"
#include "radios/widgets.h"
#include <string.h>

/* The service key entry is limited to 100 characters (gtk_entry_set_max_length
 * counts characters, not bytes): a buffer for 100 UTF-8 characters of up to 4
 * bytes each, so a non-ASCII key is never truncated. A truncated copy never
 * compared equal to the entry, so every tick refreshed the review and a
 * prepared programme was always refused as "changed". */
#define SERVICE_KEY_BYTES (100 * 4 + 1)

typedef struct {
    RadioSViewContext *ctx;
    RadioSDocument **document;
    GtkWidget *panel, *owner, *status, *service, *endpoint, *summary;
    GtkWidget *buttons[6], *prerequisites;
    JsonNode *overview, *submit_retry;
    guint timer, generation;
    guint64 intent;
    gint64 acknowledged_revision;
    gboolean busy, closed;
    gchar document_id[37], service_key[SERVICE_KEY_BYTES];
} ProgrammeLifecycle;
static const gchar *operations[] = {"overview", "submit", "approve", "fork", "prepare", "distribute"};
static void action(GtkButton *, gpointer);
static void refresh(ProgrammeLifecycle *);
static void notice(ProgrammeLifecycle *p, const gchar *message)
{ gtk_label_set_text(GTK_LABEL(p->status), message); }
static JsonObject *member(JsonObject *o, const gchar *name)
{
    JsonNode *n = o ? json_object_get_member(o, name) : NULL;
    return n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
}
static JsonNode *clone(JsonNode *n)
{
    gchar *json = json_to_string(n, FALSE); JsonParser *parser = json_parser_new();
    JsonNode *copy = json_parser_load_from_data(parser, json, -1, NULL) ? json_node_copy(json_parser_get_root(parser)) : NULL;
    g_object_unref(parser); g_free(json); return copy;
}
static gboolean clean_document(ProgrammeLifecycle *p)
{
    return !p->closed && p->document && *p->document && radios_data_connected(p->ctx) &&
        radios_data_document_revision(*p->document) > 0 && *radios_data_document_id(*p->document);
}
static void render(ProgrammeLifecycle *p)
{
    JsonObject *overview = p->overview ? json_node_get_object(p->overview) : NULL;
    JsonObject *plan = member(overview, "plan"), *revision = member(overview, "revision");
    const gchar *state = radios_json_string(revision, "status", "Gönderilmedi");
    gboolean draft = g_str_equal(state, "draft"), approved = g_str_equal(state, "approved");
    JsonNode *blockers = overview ? json_object_get_member(overview, "blockers") : NULL;
    gboolean blocked = blockers && JSON_NODE_HOLDS_ARRAY(blockers) && json_array_get_length(json_node_get_array(blockers));
    gboolean clean = clean_document(p), current = clean && overview &&
        radios_json_int(overview, "document_revision", 0) == radios_data_document_revision(*p->document);
    gboolean source_matches = overview && json_object_get_boolean_member_with_default(overview, "source_matches", FALSE);
    gchar *text = g_strdup_printf("Veritabanı taslağı %" G_GINT64_FORMAT " · Plan %s · Sürüm %" G_GINT64_FORMAT " · %s",
        clean ? radios_data_document_revision(*p->document) : 0, radios_json_string(plan, "_id", "—"),
        radios_json_int(revision, "revision_no", 0), state);
    gtk_label_set_text(GTK_LABEL(p->summary), text); g_free(text);
    gboolean enabled[] = {clean, clean && (!plan || draft), current && draft && !blocked, current && approved,
                           current && approved && source_matches && p->ctx->playout, current && approved};
    for (guint i = 0; i < G_N_ELEMENTS(p->buttons); ++i) gtk_widget_set_sensitive(p->buttons[i], !p->busy && enabled[i]);
    gtk_widget_set_sensitive(p->service, !p->busy); gtk_widget_set_sensitive(p->endpoint, !p->busy);
    gtk_widget_set_sensitive(p->prerequisites, !p->busy && clean);
}
typedef struct {
    guint generation, operation;
    guint64 intent;
    gint64 document_revision;
    gchar document_id[37], service_key[SERVICE_KEY_BYTES];
} LifecycleJob;
static void done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    ProgrammeLifecycle *p = g_object_get_data(G_OBJECT(owner), "programme-lifecycle"); LifecycleJob *job = value;
    if (!p || p->closed || p->generation != job->generation) return;
    p->busy = FALSE;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        notice(p, error ? error->message : "Program işlemi doğrulanamadı; yerel taslak korunuyor.");
        if (error && error->domain == RADIOS_DATA_CLIENT_ERROR &&
            (error->code == RADIOS_DATA_CLIENT_ERROR_CONFLICT || error->code == RADIOS_DATA_CLIENT_ERROR_VALIDATION || error->code == RADIOS_DATA_CLIENT_ERROR_FORBIDDEN)) {
            g_clear_pointer(&p->submit_retry, json_node_free); g_clear_pointer(&p->overview, json_node_free);
        }
        render(p); return;
    }
    if (job->operation == 0) {
        g_clear_pointer(&p->overview, json_node_free); p->overview = clone(result);
        JsonObject *o = json_node_get_object(result); JsonNode *b = json_object_get_member(o, "blockers");
        GString *text = g_string_new("İnceleme veritabanından okundu. Formal onaylı cue snapshot'ı native marker taslağından ayrıdır; yerel düğme operatör anında sıralı ses başlatır, uç dağıtımı ayrı izlenir.");
        JsonNode *entries = json_object_get_member(o, "review_entries");
        JsonArray *rows = entries && JSON_NODE_HOLDS_ARRAY(entries) ? json_node_get_array(entries) : NULL;
        for (guint i = 0; rows && i < MIN(json_array_get_length(rows), 16u); ++i) {
            JsonObject *row = json_array_get_object_element(rows, i);
            g_string_append_printf(text, "\n%" G_GINT64_FORMAT " · %s · %s · %.3f sn · %s · %s",
                radios_json_int(row, "ordinal", 0) + 1, radios_json_string(row, "title", ""), radios_json_string(row, "scheduled_at", ""),
                (gdouble)radios_json_int(row, "duration_ms", 0) / 1000.0, radios_json_string(row, "state", ""), radios_json_string(row, "version_status", ""));
        }
        if (rows && json_array_get_length(rows) > 16)
            g_string_append_printf(text, "\n%u öğe; tüm değişmez öğeler veritabanındaki plan revizyonunda korunur.", json_array_get_length(rows));
        JsonArray *issues = b && JSON_NODE_HOLDS_ARRAY(b) ? json_node_get_array(b) : NULL;
        for (guint i = 0; issues && i < MIN(json_array_get_length(issues), 12u); ++i) {
            JsonObject *issue = json_array_get_object_element(issues, i);
            g_string_append_printf(text, "\n%s · %s", radios_json_string(issue, "entry_id", "Program"),
                                   radios_json_string(issue, "detail", radios_json_string(issue, "code", "Engel")));
        }
        notice(p, text->str); g_string_free(text, TRUE);
    } else if (job->operation == 4) {
        ctx = p->ctx;
        if (radios_playout_ui_intent(ctx) != job->intent || !clean_document(p) ||
            radios_data_document_revision(*p->document) != job->document_revision ||
            g_strcmp0(radios_data_document_id(*p->document), job->document_id) ||
            g_strcmp0(gtk_entry_get_text(GTK_ENTRY(p->service)), job->service_key)) {
            notice(p, "Beklerken yayın komutu veya taslak değişti; gecikmiş onaylı sıra başlatılmadı.");
        } else {
            JsonObject *o = json_node_get_object(result), *snapshot = member(o, "formal_snapshot");
            JsonObject *view = p->overview ? json_node_get_object(p->overview) : NULL;
            JsonObject *revision = member(view, "revision"); GError *failure = NULL;
            if (!snapshot || !revision || !g_str_equal(radios_json_string(snapshot, "plan_revision_id", ""), radios_json_string(revision, "_id", "!")))
                notice(p, "Onaylı sıra kimliği değişti; yeniden inceleyin.");
            else if (!radios_playout_ui_start_prepared(ctx, p->owner, o, 0, &failure))
                notice(p, failure ? failure->message : "Onaylı sıra yerel motora yüklenemedi.");
            else notice(p, "Onaylı sürümün değişmez ses sırası yerel MAIN motoruna yüklendi. Uç kabulü ve teslim kanıtı ayrı izlenir.");
            g_clear_error(&failure);
        }
    } else if (job->operation == 5) {
        notice(p, "Onaylı sürüm için uç dağıtımı kuyruğa alındı. Bu sonuç uç kabulü, ses veya teslim kanıtı değildir.");
    } else {
        g_clear_pointer(&p->submit_retry, json_node_free);
        notice(p, job->operation == 1 ? "Program gerçek plan taslağına gönderildi; içerik ve yayın onayı verilmedi." :
            job->operation == 2 ? "Program revizyonu gerçek doğrulamalardan geçerek onaylandı." : "Onaylı sürüm korunarak yeni plan taslağı açıldı.");
        refresh(p); return;
    }
    render(p);
}
static gboolean request(ProgrammeLifecycle *p, guint operation, JsonNode *body)
{
    LifecycleJob *job = g_new0(LifecycleJob, 1); job->operation = operation; job->generation = ++p->generation;
    job->intent = radios_playout_ui_intent(p->ctx);
    job->document_revision = radios_data_document_revision(*p->document);
    g_strlcpy(job->document_id, radios_data_document_id(*p->document), sizeof job->document_id);
    g_strlcpy(job->service_key, gtk_entry_get_text(GTK_ENTRY(p->service)), sizeof job->service_key);
    gchar *path = g_strdup_printf("/v1/workflows/desktop-programme/%s", operations[operation]); p->busy = TRUE; render(p);
    gboolean ok = radios_data_request(p->ctx, p->panel, "POST", path, body, done, job, g_free); g_free(path);
    if (!ok) { p->busy = FALSE; notice(p, "Veri işlemi başlatılamadı; taslak korunuyor."); render(p); }
    return ok;
}
static JsonNode *base(ProgrammeLifecycle *p)
{
    JsonNode *body = radios_data_body(p->ctx); JsonObject *o = json_node_get_object(body);
    json_object_set_string_member(o, "document_id", radios_data_document_id(*p->document));
    json_object_set_string_member(o, "service_key", gtk_entry_get_text(GTK_ENTRY(p->service))); return body;
}
static void refresh(ProgrammeLifecycle *p)
{
    if (!clean_document(p) || p->busy) return;
    JsonNode *body = base(p); request(p, 0, body); json_node_free(body);
}
static void action(GtkButton *button, gpointer value)
{
    ProgrammeLifecycle *p = value; guint operation = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "programme-operation"));
    if (!clean_document(p) || p->busy) return;
    if (!operation) { refresh(p); return; }
    JsonNode *body = base(p); JsonObject *o = json_node_get_object(body);
    JsonObject *overview = p->overview ? json_node_get_object(p->overview) : NULL;
    JsonObject *plan = member(overview, "plan"), *revision = member(overview, "revision");
    if (operation == 1) {
        json_object_set_int_member(o, "expected_document_revision", radios_data_document_revision(*p->document));
        if (plan) json_object_set_int_member(o, "expected_plan_revision", radios_json_int(plan, "_rev", 0));
        if (p->submit_retry) { json_node_free(body); body = clone(p->submit_retry); }
        else { gchar *key = g_uuid_string_random(); json_object_set_string_member(o, "idempotency_key", key); g_free(key); p->submit_retry = clone(body); }
    } else {
        json_object_remove_member(o, "service_key");
        if (!plan || !revision) { json_node_free(body); notice(p, "Önce gerçek program revizyonunu inceleyin."); return; }
        json_object_set_string_member(o, "plan_id", radios_json_string(plan, "_id", ""));
        json_object_set_string_member(o, "revision_id", radios_json_string(revision, "_id", ""));
        json_object_set_int_member(o, "expected_plan_revision", radios_json_int(plan, "_rev", 0));
        if (operation >= 4) {
            const gchar *endpoint = gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->endpoint));
            if (!endpoint || !g_uuid_string_is_valid(endpoint)) { json_node_free(body); notice(p, "Kayıtlı hedef ucu seçin."); return; }
            json_object_set_string_member(o, "endpoint_id", endpoint);
        }
    }
    request(p, operation, body); json_node_free(body);
}
static gboolean tick(gpointer value)
{
    ProgrammeLifecycle *p = value; if (p->closed) return G_SOURCE_REMOVE;
    if (clean_document(p) && !p->busy) {
        const gchar *id = radios_data_document_id(*p->document), *key = gtk_entry_get_text(GTK_ENTRY(p->service));
        gint64 revision = radios_data_document_revision(*p->document);
        if (g_strcmp0(p->document_id, id) || p->acknowledged_revision != revision || g_strcmp0(p->service_key, key)) {
            g_strlcpy(p->document_id, id, sizeof p->document_id); p->acknowledged_revision = revision;
            g_strlcpy(p->service_key, key, sizeof p->service_key); g_clear_pointer(&p->overview, json_node_free);
            g_clear_pointer(&p->submit_retry, json_node_free); refresh(p);
        }
        if (!gtk_combo_box_get_active_id(GTK_COMBO_BOX(p->endpoint))) {
            JsonObject *context = radios_data_context(p->ctx); JsonNode *rows = context ? json_object_get_member(context, "endpoints") : NULL;
            JsonArray *endpoints = rows && JSON_NODE_HOLDS_ARRAY(rows) ? json_node_get_array(rows) : NULL;
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(p->endpoint));
            for (guint i = 0; endpoints && i < json_array_get_length(endpoints); ++i) {
                JsonObject *endpoint = json_array_get_object_element(endpoints, i);
                gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->endpoint), radios_json_string(endpoint, "_id", ""), radios_json_string(endpoint, "name", radios_json_string(endpoint, "_id", "")));
            }
            if (endpoints && json_array_get_length(endpoints)) gtk_combo_box_set_active(GTK_COMBO_BOX(p->endpoint), 0);
        }
    }
    render(p); return G_SOURCE_CONTINUE;
}
static void closed(GtkWidget *widget, gpointer value)
{
    (void)widget; ProgrammeLifecycle *p = value; p->closed = TRUE;
    if (p->timer) { g_source_remove(p->timer); p->timer = 0; }
}
static void freed(gpointer value)
{
    ProgrammeLifecycle *p = value; g_clear_pointer(&p->overview, json_node_free); g_clear_pointer(&p->submit_retry, json_node_free); g_free(p);
}
static void prerequisites_open(GtkButton *button, gpointer value)
{
    (void)button; ProgrammeLifecycle *p = value;
    if (clean_document(p)) radios_programme_prerequisites_open(p->ctx, p->panel, radios_data_document_id(*p->document));
}
GtkWidget *radios_programme_lifecycle_new(RadioSViewContext *ctx, GtkWidget *programme_owner, RadioSDocument **document)
{
    ProgrammeLifecycle *p = g_new0(ProgrammeLifecycle, 1); p->ctx = ctx; p->document = document; p->owner = programme_owner;
    p->panel = radios_ui_box(GTK_ORIENTATION_VERTICAL, 7, "ld-revision"); gtk_widget_set_name(p->panel, "programme-lifecycle");
    g_object_set_data_full(G_OBJECT(p->panel), "programme-lifecycle", p, freed);
    g_signal_connect(p->panel, "destroy", G_CALLBACK(closed), p);
    p->summary = radios_ui_label("", "ld-head-meta");
    gtk_label_set_line_wrap(GTK_LABEL(p->summary), TRUE); gtk_box_pack_start(GTK_BOX(p->panel), p->summary, FALSE, FALSE, 0);
    GtkWidget *targets = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 7, "ld-actions");
    p->service = radios_ui_entry("default"); gtk_entry_set_text(GTK_ENTRY(p->service), "default"); gtk_entry_set_max_length(GTK_ENTRY(p->service), 100);
    gtk_widget_set_name(p->service, "programme-service-key"); p->endpoint = gtk_combo_box_text_new(); gtk_widget_set_name(p->endpoint, "programme-endpoint");
    gtk_box_pack_start(GTK_BOX(targets), radios_ui_label("Yayın servisi", NULL), FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(targets), p->service, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(targets), p->endpoint, TRUE, TRUE, 0); gtk_box_pack_start(GTK_BOX(p->panel), targets, FALSE, FALSE, 0);
    const gchar *names[] = {"Değişiklikleri incele", "Yerel onaya gönder", "Programı onayla", "Yeni taslak aç", "Onaylı sırayı yerel çal", "Uca dağıt"};
    const gchar *ids[] = {"programme-review", "programme-submit", "programme-approve", "programme-fork", "programme-load", "programme-distribute"};
    GtkWidget *actions = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(p->buttons); ++i) {
        if (!(i % 3)) {
            actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 5, "ld-actions");
            gtk_box_pack_start(GTK_BOX(p->panel), actions, FALSE, FALSE, 0);
        }
        p->buttons[i] = radios_ui_button(names[i]); gtk_widget_set_name(p->buttons[i], ids[i]);
        g_object_set_data(G_OBJECT(p->buttons[i]), "programme-operation", GUINT_TO_POINTER(i)); g_signal_connect(p->buttons[i], "clicked", G_CALLBACK(action), p);
        gtk_box_pack_start(GTK_BOX(actions), p->buttons[i], FALSE, FALSE, 0);
    }
    p->prerequisites = radios_ui_button("İçerik onayları ve kullanım hakları");
    gtk_widget_set_name(p->prerequisites, "programme-prerequisites");
    g_signal_connect(p->prerequisites, "clicked", G_CALLBACK(prerequisites_open), p);
    gtk_box_pack_start(GTK_BOX(p->panel), p->prerequisites, FALSE, FALSE, 0);
    p->status = radios_ui_label("", "ld-head-meta");
    gtk_widget_set_name(p->status, "programme-lifecycle-status"); gtk_label_set_line_wrap(GTK_LABEL(p->status), TRUE); gtk_label_set_selectable(GTK_LABEL(p->status), TRUE);
    gtk_box_pack_start(GTK_BOX(p->panel), p->status, FALSE, FALSE, 0);
    p->timer = g_timeout_add(250, tick, p); render(p); return p->panel;
}
