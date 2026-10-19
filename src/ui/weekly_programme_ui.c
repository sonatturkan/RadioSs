#include "radios/weekly_programme_ui.h"
#include "radios/weekly_programme_plan.h"
#include "radios/automation_calendar.h"
#include "radios/data.h"
#include "radios/station_time.h"
#include "radios/widgets.h"
#include <string.h>

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *root, *main, *choice, *name, *name_label, *anchor, *weeks, *repeat, *status;
    GtkWidget *holder, *calendar, *save, *meta;
    RadioSDocument *document;
    RadioSDataClient *list_client;
    JsonNode *records, *pending;
    gchar *identifier;
    guint timer, generation;
    gboolean applying, disposed, closing, dirty, listing, fresh, invalid, embedded, programme_mode, loading;
} Workspace;
typedef struct { RadioSDataClient *client; guint generation; } ListJob;
typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *dialog, *name, *status, *apply;
    RadioSDocument *document;
    JsonNode *snapshot;
    guint timer;
    gboolean started, disposed;
} SaveDialog;

static JsonNode *copy_json(JsonNode *node)
{
    gchar *text = json_to_string(node, FALSE); JsonParser *parser = json_parser_new();
    gboolean ok = json_parser_load_from_data(parser, text, -1, NULL); g_free(text);
    JsonNode *copy = ok ? json_parser_steal_root(parser) : NULL; g_object_unref(parser); return copy;
}
static GtkWidget *named(GtkWidget *widget, const gchar *name)
{ gtk_widget_set_name(widget, name); return widget; }
static void message(Workspace *view, const gchar *text)
{ gtk_label_set_text(GTK_LABEL(view->status), text); }
static gchar *monday_today(RadioSViewContext *ctx)
{
    GDateTime *now = radios_station_now(ctx);
    GDateTime *monday = g_date_time_add_days(now, 1 - g_date_time_get_day_of_week(now));
    gchar *text = g_date_time_format(monday, "%Y-%m-%d");
    g_date_time_unref(monday); g_date_time_unref(now); return text;
}
static GtkWidget *find_named(GtkWidget *widget, const gchar *name)
{
    if (!g_strcmp0(gtk_widget_get_name(widget), name)) return widget;
    if (!GTK_IS_CONTAINER(widget)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = find_named(item->data, name);
    g_list_free(children); return found;
}
/* The saved programme document owns its CAS baseline, while the visible
 * calendar is shared. Gate only editing surfaces during that initial read;
 * the programme picker remains available to cancel or choose Main. */
static void publish_document(Workspace *view, RadioSDocument *document)
{
    if (view->embedded && view->main)
        g_object_set_data(G_OBJECT(view->main), "radios-weekly-programme-document", document);
}
static void shared_edit_sensitive(Workspace *view, gboolean enabled)
{
    if (!view->embedded || !view->main) return;
    const gchar *names[] = {"automation-calendar-workspace", "automation-calendar-programme-editor",
        "automation-calendar-available-rotations-panel"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkWidget *widget = find_named(view->main, names[i]);
        if (widget) gtk_widget_set_sensitive(widget, enabled);
    }
}
static JsonNode *workspace_snapshot(Workspace *view, GError **error)
{
    if (!view->calendar) return NULL;
    if (!radios_automation_calendar_snapshot_ready(view->calendar)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Program bloğunun adını, tarihini ve saatlerini düzelt; geçersiz girdiler korunuyor."); return NULL;
    }
    JsonNode *calendar = radios_automation_calendar_snapshot(view->calendar);
    const gchar *anchor = gtk_entry_get_text(GTK_ENTRY(view->anchor));
    JsonNode *config = radios_weekly_programme_snapshot(json_node_get_object(calendar), anchor,
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(view->repeat)), error);
    json_node_free(calendar); if (!config) return NULL;
    JsonObject *object = json_node_get_object(config);
    json_object_set_string_member(object, "week_anchor", anchor);
    json_object_set_int_member(object, "week_count", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(view->weeks)));
    RadioSWeeklyProgrammeOccurrence *next = radios_weekly_programme_next(object, g_get_real_time(), error);
    radios_weekly_programme_occurrence_free(next);
    if (error && *error) { json_node_free(config); return NULL; }
    return config;
}
static void list_refresh(RadioSViewContext *ctx, GtkWidget *owner);
static void staged(Workspace *view)
{
    if (view->applying || view->disposed || view->closing || view->loading || (view->embedded && !view->programme_mode)) return;
    view->dirty = TRUE;
    if (view->document) radios_data_document_dirty(view->document, TRUE);
    GError *error = NULL; JsonNode *config = workspace_snapshot(view, &error);
    if (!config) {
        view->invalid = TRUE;
        message(view, error ? error->message : "Program taslağı henüz hazır değil."); g_clear_error(&error); return;
    }
    view->invalid = FALSE;
    g_clear_pointer(&view->pending, json_node_free); view->pending = config;
    message(view, view->document ? "Değişiklikler otomatik kaydediliyor…" : "Taslak hazır. Programı kaydet ile ayrı kayıt oluştur.");
}
static void calendar_changed(GtkWidget *page, JsonNode *snapshot, gpointer data)
{ (void)page; (void)snapshot; staged(data); }
static void field_changed(GtkWidget *widget, gpointer data)
{ (void)widget; staged(data); }
static void applied(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data)
{
    (void)ctx; (void)owner; Workspace *view = data;
    if (view->disposed || !view->calendar) return;
    gboolean explicit_reload = g_object_get_data(G_OBJECT(owner), "radios-data-explicit-reload") != NULL;
    if (!explicit_reload && (view->invalid || !radios_automation_calendar_snapshot_ready(view->calendar))) {
        view->invalid = TRUE; view->dirty = TRUE;
        if (view->document) radios_data_document_dirty(view->document, TRUE);
        message(view, "Geçersiz program girdisi korunuyor. Düzeltince otomatik kaydedilecek."); return;
    }
    view->invalid = FALSE; view->applying = TRUE;
    gtk_entry_set_text(GTK_ENTRY(view->anchor), radios_json_string(config, "week_anchor", ""));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->weeks), (gdouble)radios_json_int(config, "week_count", 1));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->repeat), json_object_get_boolean_member(config, "repeat"));
    if (explicit_reload) g_object_set_data(G_OBJECT(view->calendar), "radios-data-explicit-reload", GINT_TO_POINTER(TRUE));
    radios_automation_calendar_set_snapshot(view->calendar, json_object_get_object_member(config, "calendar"));
    if (explicit_reload) g_object_set_data(G_OBJECT(view->calendar), "radios-data-explicit-reload", NULL);
    view->applying = FALSE; view->dirty = FALSE;
    g_clear_pointer(&view->pending, json_node_free);
    message(view, "Program veritabanına kaydedildi. Sonraki düzenlemeler otomatik kaydedilir.");
    if (view->fresh) { view->fresh = FALSE; list_refresh(view->ctx, view->root); }
}
static gboolean tick(gpointer data)
{
    Workspace *view = data;
    if (view->disposed || view->closing) { view->timer = 0; return G_SOURCE_REMOVE; }
    if (view->embedded && !view->programme_mode) return G_SOURCE_CONTINUE;
    if (view->loading) {
        if (!view->document || !radios_data_document_ready(view->document) ||
            radios_data_document_revision(view->document) <= 0) return G_SOURCE_CONTINUE;
        view->loading = FALSE; shared_edit_sensitive(view, TRUE);
    }
    if (view->calendar && !radios_automation_calendar_snapshot_ready(view->calendar)) {
        if (!view->invalid && view->document) radios_data_document_dirty(view->document, TRUE);
        view->invalid = TRUE; view->dirty = TRUE;
        message(view, "Program bloğunun adını, tarihini ve saatlerini düzelt; geçersiz girdiler korunuyor.");
        return G_SOURCE_CONTINUE;
    }
    if (!view->document && !view->dirty && view->calendar) {
        JsonNode *main = radios_automation_calendar_main_snapshot(view->main);
        JsonNode *draft = radios_automation_calendar_snapshot(view->calendar);
        JsonNode *library = main ? json_object_get_member(json_node_get_object(main), "rotation_library") : NULL;
        JsonNode *previous = draft ? json_object_get_member(json_node_get_object(draft), "rotation_library") : NULL;
        if (library && (!previous || !json_node_equal(library, previous))) {
            json_object_set_member(json_node_get_object(draft), "rotation_library", copy_json(library));
            radios_automation_calendar_set_snapshot(view->calendar, json_node_get_object(draft));
        }
        g_clear_pointer(&main, json_node_free); g_clear_pointer(&draft, json_node_free);
    }
    if (view->document && radios_data_document_ready(view->document)) {
        gtk_widget_set_sensitive(view->holder, TRUE);
        gtk_widget_set_sensitive(view->meta, TRUE); gtk_widget_set_sensitive(view->save, TRUE);
        if (view->pending && !view->invalid) radios_data_document_autosave(view->document, view->pending);
    }
    return G_SOURCE_CONTINUE;
}
static gboolean can_switch(Workspace *view)
{
    if (view->disposed || view->closing) return FALSE;
    if (view->loading) return TRUE;
    if (view->calendar && !radios_automation_calendar_snapshot_ready(view->calendar)) {
        message(view, "Program değiştirmeden önce seçili bloğun geçersiz girdilerini düzelt."); return FALSE;
    }
    if (!view->dirty) return TRUE;
    if (view->document && !view->pending && radios_data_document_revision(view->document) > 0) return TRUE;
    message(view, "Önce mevcut programı kaydet; kayıt tamamlanınca program değiştirebilirsin."); return FALSE;
}
static void install_editor(Workspace *view, JsonObject *config, const gchar *id, const gchar *name)
{
    view->applying = TRUE;
    publish_document(view, NULL);
    if (view->holder) { gtk_widget_destroy(view->holder); view->holder = NULL; view->document = NULL; }
    g_clear_pointer(&view->identifier, g_free); view->identifier = g_strdup(id);
    g_clear_pointer(&view->pending, json_node_free); view->dirty = FALSE; view->fresh = FALSE; view->invalid = FALSE;
    gtk_entry_set_text(GTK_ENTRY(view->name), name); gtk_editable_set_editable(GTK_EDITABLE(view->name), id == NULL);
    if (view->embedded) { gtk_widget_set_visible(view->name, id == NULL); gtk_widget_set_visible(view->name_label, id == NULL); }
    gtk_entry_set_text(GTK_ENTRY(view->anchor), radios_json_string(config, "week_anchor", ""));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(view->weeks), (gdouble)radios_json_int(config, "week_count", 1));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(view->repeat), json_object_get_boolean_member(config, "repeat"));
    view->holder = named(radios_ui_box(GTK_ORIENTATION_VERTICAL, 0, NULL), "weekly-programme-editor-holder");
    view->programme_mode = TRUE; view->loading = id != NULL;
    gtk_widget_show(view->meta);
    shared_edit_sensitive(view, !view->loading);
    if (view->embedded) {
        view->calendar = view->main;
        radios_automation_calendar_bind_editor(view->main, calendar_changed, view, name);
        radios_automation_calendar_set_snapshot(view->main, json_object_get_object_member(config, "calendar"));
    } else {
        view->calendar = radios_automation_calendar_editor_new(view->ctx,
            json_object_get_object_member(config, "calendar"), calendar_changed, view);
        gtk_widget_set_name(view->calendar, "weekly-programme-calendar");
    }
    GtkWidget *date = find_named(view->calendar, "automation-calendar-date");
    if (date) { gtk_entry_set_text(GTK_ENTRY(date), radios_json_string(config, "week_anchor", "")); g_signal_emit_by_name(date, "activate"); }
    if (!view->embedded) gtk_box_pack_start(GTK_BOX(view->holder), view->calendar, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(view->root), view->holder, TRUE, TRUE, 0);
    if (id) {
        view->document = radios_data_document_new_for_id(view->ctx, view->holder,
            "weekly_programme", name, id, applied, view);
        publish_document(view, view->document);
        gtk_widget_set_sensitive(view->holder, FALSE);
        message(view, "Programın doğrulanmış kaydı yükleniyor…");
    } else message(view, "Yeni haftalık program: blokları ve olasılıkları düzenleyip Programı kaydet'e bas.");
    gtk_widget_set_sensitive(view->meta, !id); gtk_widget_set_sensitive(view->save, !id);
    view->applying = FALSE;
    if (view->embedded) { gtk_widget_set_no_show_all(view->holder, TRUE); gtk_widget_hide(view->holder); }
    else gtk_widget_show_all(view->holder);
}
static void new_clicked(GtkButton *button, gpointer data)
{
    (void)button; Workspace *view = data; if (!can_switch(view)) return;
    JsonNode *calendar = radios_automation_calendar_main_snapshot(view->main);
    json_object_set_array_member(json_node_get_object(calendar), "events", json_array_new());
    json_object_set_string_member(json_node_get_object(calendar), "timezone", radios_station_timezone(view->ctx));
    gchar *anchor = monday_today(view->ctx); GError *error = NULL;
    JsonNode *config = radios_weekly_programme_snapshot(json_node_get_object(calendar), anchor, FALSE, &error);
    json_node_free(calendar); g_free(anchor);
    if (!config) { message(view, error ? error->message : "Program oluşturulamadı."); g_clear_error(&error); return; }
    view->applying = TRUE; gtk_combo_box_set_active(GTK_COMBO_BOX(view->choice), -1); view->applying = FALSE;
    install_editor(view, json_node_get_object(config), NULL, "Yeni program"); json_node_free(config);
}
static void choice_changed(GtkComboBox *combo, gpointer data)
{
    Workspace *view = data; if (view->applying || view->disposed) return;
    const gchar *id = gtk_combo_box_get_active_id(combo); if (!id) return;
    if (!can_switch(view)) {
        view->applying = TRUE; gtk_combo_box_set_active_id(combo, view->identifier ? view->identifier : (view->programme_mode ? NULL : "main")); view->applying = FALSE; return;
    }
    if (view->embedded && !g_strcmp0(id, "main")) {
        view->applying = TRUE;
        publish_document(view, NULL);
        if (view->holder) { gtk_widget_destroy(view->holder); view->holder = NULL; view->document = NULL; }
        g_clear_pointer(&view->identifier, g_free); g_clear_pointer(&view->pending, json_node_free);
        view->programme_mode = FALSE; view->dirty = FALSE; view->invalid = FALSE; view->fresh = FALSE; view->loading = FALSE;
        shared_edit_sensitive(view, TRUE);
        radios_automation_calendar_bind_editor(view->main, NULL, NULL, "Ana yayın takvimi");
        gtk_entry_set_text(GTK_ENTRY(view->name), "Ana yayın takvimi");
        gtk_widget_set_sensitive(view->meta, FALSE); gtk_widget_set_sensitive(view->save, FALSE);
        gtk_widget_hide(view->meta);
        message(view, "Ana yayın takvimi. Düzenlemeler otomatik kaydedilir.");
        view->applying = FALSE; return;
    }
    JsonArray *records = view->records ? json_object_get_array_member(json_node_get_object(view->records), "records") : NULL;
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        JsonObject *record = json_array_get_object_element(records, i);
        if (!g_strcmp0(radios_json_string(record, "_id", ""), id)) {
            install_editor(view, json_object_get_object_member(record, "config"), id, radios_json_string(record, "name", "Program")); return;
        }
    }
}
static void list_job_free(gpointer data)
{ ListJob *job = data; radios_data_client_unref(job->client); g_free(job); }
static void listed(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    Workspace *view = g_object_get_data(G_OBJECT(owner), "weekly-programme-view"); ListJob *job = data;
    if (!view || view->disposed || job->generation != view->generation) return;
    view->listing = FALSE; RadioSDataClient *current = radios_data_client_acquire(ctx);
    gboolean same = current && current == job->client;
    if (current) radios_data_client_unref(current);
    if (!same || error || !result || !JSON_NODE_HOLDS_OBJECT(result)) return;
    g_clear_pointer(&view->records, json_node_free); view->records = copy_json(result);
    view->applying = TRUE; gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(view->choice));
    if (view->embedded) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->choice), "main", "Ana yayın takvimi");
    JsonArray *records = json_object_get_array_member(json_node_get_object(result), "records");
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        JsonObject *record = json_array_get_object_element(records, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->choice), radios_json_string(record, "_id", ""), radios_json_string(record, "name", "Program"));
    }
    if (view->identifier) gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->choice), view->identifier);
    else if (view->embedded && !view->programme_mode) gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->choice), "main");
    view->applying = FALSE;
}
static void list_refresh(RadioSViewContext *ctx, GtkWidget *owner)
{
    Workspace *view = g_object_get_data(G_OBJECT(owner), "weekly-programme-view");
    if (!view || view->disposed || view->listing || !radios_data_connected(ctx)) return;
    ListJob *job = g_new0(ListJob, 1); job->client = radios_data_client_acquire(ctx);
    if (!job->client) { g_free(job); return; }
    job->generation = ++view->generation;
    JsonNode *body = radios_data_body(ctx); json_object_set_string_member(json_node_get_object(body), "kind", "weekly_programme");
    view->listing = radios_data_request(ctx, owner, "POST", "/v1/workflows/desktop/list", body, listed, job, list_job_free);
    json_node_free(body);
}
static void save_clicked(GtkButton *button, gpointer data)
{
    (void)button; Workspace *view = data;
    if (view->embedded && !view->programme_mode) return;
    if (!view->document) {
        gchar *name = g_strdup(gtk_entry_get_text(GTK_ENTRY(view->name))); g_strstrip(name);
        if (!*name || g_utf8_strlen(name, -1) > 160) { message(view, "Program adı 1–160 karakter olmalı."); g_free(name); return; }
        GError *error = NULL; JsonNode *config = workspace_snapshot(view, &error);
        if (!config) { message(view, error ? error->message : "Program hazır değil."); g_clear_error(&error); g_free(name); return; }
        view->identifier = g_uuid_string_random(); view->fresh = TRUE;
        view->document = radios_data_document_new_record(view->ctx, view->holder,
            "weekly_programme", name, view->identifier, applied, view);
        publish_document(view, view->document);
        if (!view->document) { message(view, "Veri bağlantısı kurulmadan program kaydedilemez.");
            g_clear_pointer(&view->identifier, g_free); json_node_free(config); g_free(name); return; }
        gtk_editable_set_editable(GTK_EDITABLE(view->name), FALSE); view->dirty = TRUE;
        if (view->embedded) { gtk_widget_hide(view->name); gtk_widget_hide(view->name_label); }
        radios_data_document_dirty(view->document, TRUE);
        g_clear_pointer(&view->pending, json_node_free); view->pending = config;
        radios_data_document_autosave(view->document, view->pending);
        g_free(name);
    } else staged(view);
    message(view, "Program kaydı doğrulanıyor…");
}
void radios_weekly_programme_prepare_close(GtkWidget *workspace)
{
    Workspace *view = workspace ? g_object_get_data(G_OBJECT(workspace), "weekly-programme-view") : NULL;
    if (!view || view->disposed || view->closing) return;
    if (view->calendar && !view->embedded) radios_automation_calendar_prepare_close(view->calendar);
    if (view->document && view->pending && radios_data_document_ready(view->document))
        radios_data_document_autosave(view->document, view->pending);
    if (view->document) radios_data_document_preserve_pending(view->document);
    view->closing = TRUE; if (view->timer) { g_source_remove(view->timer); view->timer = 0; }
}
static void destroyed(GtkWidget *widget, gpointer data)
{
    Workspace *view = data; radios_weekly_programme_prepare_close(widget);
    publish_document(view, NULL);
    /* If this controls widget is removed independently, its shared grid must
     * not retain a callback into freed Workspace memory. A closing Main grid
     * rejects the detach and cannot emit further edits. */
    if (view->embedded && view->programme_mode && view->main) {
        radios_automation_calendar_bind_editor(view->main, NULL, NULL, "Ana yayın takvimi");
        shared_edit_sensitive(view, TRUE);
    }
    view->disposed = TRUE;
}
static void workspace_free(gpointer data)
{
    Workspace *view = data; if (view->timer) g_source_remove(view->timer);
    g_clear_pointer(&view->records, json_node_free); g_clear_pointer(&view->pending, json_node_free);
    g_clear_pointer(&view->list_client, radios_data_client_unref); g_free(view->identifier); g_free(view);
}
static GtkWidget *workspace_new(RadioSViewContext *ctx, GtkWidget *main_calendar, gboolean embedded)
{
    Workspace *view = g_new0(Workspace, 1); view->ctx = ctx; view->main = main_calendar;
    view->embedded = embedded; view->calendar = embedded ? main_calendar : NULL;
    view->root = named(radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, NULL), "weekly-programme-workspace");
    g_object_set_data_full(G_OBJECT(view->root), "weekly-programme-view", view, workspace_free);
    g_signal_connect(view->root, "destroy", G_CALLBACK(destroyed), view);
    GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    view->choice = named(gtk_combo_box_text_new(), "weekly-programme-choice");
    gtk_box_pack_start(GTK_BOX(row), radios_ui_label("Program adı", "small"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), view->choice, TRUE, TRUE, 0);
    GtkWidget *create = named(radios_ui_button("Yeni program"), "weekly-programme-new");
    GtkWidget *actions = embedded ? radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL) : row;
    gtk_box_pack_start(GTK_BOX(actions), create, FALSE, FALSE, 0);
    view->save = named(radios_ui_button("Programı kaydet"), "weekly-programme-save");
    gtk_box_pack_start(GTK_BOX(actions), view->save, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(view->root), row, FALSE, FALSE, 0);
    if (embedded) gtk_box_pack_start(GTK_BOX(view->root), actions, FALSE, FALSE, 0);
    GtkWidget *meta = gtk_grid_new(); view->meta = meta; gtk_grid_set_column_spacing(GTK_GRID(meta), 8); gtk_grid_set_row_spacing(GTK_GRID(meta), 4);
    view->name = named(radios_ui_entry("Program adı"), "weekly-programme-name");
    gtk_entry_set_max_length(GTK_ENTRY(view->name), 160);
    view->anchor = named(radios_ui_entry("YYYY-MM-DD · Pazartesi"), "weekly-programme-anchor");
    view->weeks = named(gtk_spin_button_new_with_range(1, 52, 1), "weekly-programme-weeks");
    view->repeat = named(gtk_check_button_new_with_label("Tüm haftaları sürekli tekrarla"), "weekly-programme-repeat");
    gtk_widget_set_hexpand(view->name, TRUE);
    if (embedded) {
        view->name_label = radios_ui_label("Program adı", "small");
        gtk_grid_attach(GTK_GRID(meta), view->name_label, 0, 0, 2, 1);
        gtk_grid_attach(GTK_GRID(meta), view->name, 0, 1, 2, 1);
        gtk_grid_attach(GTK_GRID(meta), radios_ui_label("İlk hafta · Pazartesi", "small"), 0, 2, 1, 1);
        gtk_grid_attach(GTK_GRID(meta), radios_ui_label("Hafta sayısı", "small"), 1, 2, 1, 1);
        gtk_grid_attach(GTK_GRID(meta), view->anchor, 0, 3, 1, 1);
        gtk_grid_attach(GTK_GRID(meta), view->weeks, 1, 3, 1, 1);
        gtk_grid_attach(GTK_GRID(meta), view->repeat, 0, 4, 2, 1);
    } else {
        gtk_grid_attach(GTK_GRID(meta), radios_ui_label("Program adı", "small"), 0, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(meta), radios_ui_label("İlk hafta · takvim saati", "small"), 1, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(meta), radios_ui_label("Hafta sayısı", "small"), 2, 0, 1, 1);
        gtk_grid_attach(GTK_GRID(meta), view->name, 0, 1, 1, 1); gtk_grid_attach(GTK_GRID(meta), view->anchor, 1, 1, 1, 1);
        gtk_grid_attach(GTK_GRID(meta), view->weeks, 2, 1, 1, 1); gtk_grid_attach(GTK_GRID(meta), view->repeat, 3, 1, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(view->root), meta, FALSE, FALSE, 0);
    view->status = named(radios_ui_label("", "small"), "weekly-programme-status");
    gtk_label_set_line_wrap(GTK_LABEL(view->status), TRUE); gtk_box_pack_start(GTK_BOX(view->root), view->status, FALSE, FALSE, 0);
    g_signal_connect(create, "clicked", G_CALLBACK(new_clicked), view); g_signal_connect(view->save, "clicked", G_CALLBACK(save_clicked), view);
    g_signal_connect(view->choice, "changed", G_CALLBACK(choice_changed), view);
    g_signal_connect(view->anchor, "changed", G_CALLBACK(field_changed), view);
    g_signal_connect(view->weeks, "value-changed", G_CALLBACK(field_changed), view);
    g_signal_connect(view->repeat, "toggled", G_CALLBACK(field_changed), view);
    if (embedded) {
        g_object_set_data(G_OBJECT(view->root), "weekly-programme-calendar", main_calendar);
        view->applying = TRUE;
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(view->choice), "main", "Ana yayın takvimi");
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(view->choice), "main");
        gtk_entry_set_text(GTK_ENTRY(view->name), "Ana yayın takvimi");
        gtk_widget_set_sensitive(view->meta, FALSE); gtk_widget_set_sensitive(view->save, FALSE);
        gtk_widget_hide(view->meta);
        message(view, "Bir program seç veya Yeni program ile bu takvimde oluştur."); view->applying = FALSE;
    } else new_clicked(NULL, view);
    gtk_widget_show_all(view->meta); gtk_widget_set_no_show_all(view->meta, TRUE);
    if (embedded) {
        gtk_widget_set_no_show_all(view->name, TRUE); gtk_widget_set_no_show_all(view->name_label, TRUE);
        gtk_widget_set_visible(view->name, view->programme_mode && !view->identifier);
        gtk_widget_set_visible(view->name_label, view->programme_mode && !view->identifier);
    }
    gtk_widget_set_visible(view->meta, !embedded || view->programme_mode);
    view->timer = g_timeout_add(250, tick, view);
    if (ctx->data) radios_data_register_refresh(ctx, view->root, list_refresh);
    if (radios_data_connected(ctx)) list_refresh(ctx, view->root);
    return view->root;
}

GtkWidget *radios_weekly_programme_new(RadioSViewContext *ctx, GtkWidget *main_calendar)
{ return workspace_new(ctx, main_calendar, FALSE); }
GtkWidget *radios_weekly_programme_controls_new(RadioSViewContext *ctx, GtkWidget *main_calendar)
{ return workspace_new(ctx, main_calendar, TRUE); }

static gboolean destroy_dialog_idle(gpointer data)
{ gtk_widget_destroy(GTK_WIDGET(data)); return G_SOURCE_REMOVE; }
static void save_applied(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data)
{
    (void)config; SaveDialog *save = data; if (save->disposed || !save->started) return;
    gtk_label_set_text(GTK_LABEL(save->status), "Program veritabanına kaydedildi.");
    radios_data_refresh(ctx);
    g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, destroy_dialog_idle, g_object_ref(owner), g_object_unref);
}
static gboolean save_tick(gpointer data)
{
    SaveDialog *save = data; if (save->disposed) { save->timer = 0; return G_SOURCE_REMOVE; }
    if (save->started && radios_data_document_ready(save->document))
        radios_data_document_autosave(save->document, save->snapshot);
    return G_SOURCE_CONTINUE;
}
static void save_response(GtkDialog *dialog, gint response, gpointer data)
{
    SaveDialog *save = data;
    if (response != GTK_RESPONSE_ACCEPT) {
        if (save->document) radios_data_document_preserve_pending(save->document);
        gtk_widget_destroy(GTK_WIDGET(dialog)); return;
    }
    if (save->started) return;
    gchar *name = g_strdup(gtk_entry_get_text(GTK_ENTRY(save->name))); g_strstrip(name);
    if (!*name || g_utf8_strlen(name, -1) > 160) { gtk_label_set_text(GTK_LABEL(save->status), "Program adı 1–160 karakter olmalı."); g_free(name); return; }
    gchar *id = g_uuid_string_random(); save->document = radios_data_document_new_record(save->ctx,
        save->dialog, "weekly_programme", name, id, save_applied, save); g_free(id); g_free(name);
    if (!save->document) { gtk_label_set_text(GTK_LABEL(save->status), "Veri bağlantısı kurulmadan program kaydedilemez."); return; }
    save->started = TRUE; radios_data_document_dirty(save->document, TRUE);
    radios_data_document_autosave(save->document, save->snapshot);
    gtk_widget_set_sensitive(save->apply, FALSE); gtk_editable_set_editable(GTK_EDITABLE(save->name), FALSE);
    gtk_label_set_text(GTK_LABEL(save->status), "Tam otomasyon kopyası veritabanına kaydediliyor…");
}
static void save_destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget; SaveDialog *save = data;
    if (save->document) radios_data_document_preserve_pending(save->document);
    save->disposed = TRUE; if (save->timer) { g_source_remove(save->timer); save->timer = 0; }
}
static void save_free(gpointer data)
{ SaveDialog *save = data; if (save->timer) g_source_remove(save->timer); json_node_free(save->snapshot); g_free(save); }
void radios_weekly_programme_save_snapshot(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *calendar, const gchar *visible_week)
{
    GError *error = NULL; JsonNode *snapshot = radios_weekly_programme_snapshot(calendar, visible_week, FALSE, &error);
    if (!snapshot) { if (ctx->notify) ctx->notify(ctx->notify_data, error ? error->message : "Program oluşturulamadı."); g_clear_error(&error); return; }
    SaveDialog *save = g_new0(SaveDialog, 1); save->ctx = ctx; save->snapshot = snapshot;
    save->dialog = named(gtk_dialog_new_with_buttons("Otomasyonu program olarak kaydet", ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT | GTK_DIALOG_MODAL, NULL, NULL), "automation-programme-save-dialog");
    GtkWidget *cancel = gtk_dialog_add_button(GTK_DIALOG(save->dialog), "Vazgeç", GTK_RESPONSE_CANCEL);
    gtk_widget_set_name(cancel, "automation-programme-save-cancel");
    save->apply = named(gtk_dialog_add_button(GTK_DIALOG(save->dialog), "Program olarak kaydet", GTK_RESPONSE_ACCEPT), "automation-programme-save-apply");
    GtkWidget *content = gtk_dialog_get_content_area(GTK_DIALOG(save->dialog)); gtk_container_set_border_width(GTK_CONTAINER(content), 16);
    save->name = named(radios_ui_entry("Program adı"), "automation-programme-save-name"); gtk_entry_set_text(GTK_ENTRY(save->name), "Yeni program"); gtk_entry_set_max_length(GTK_ENTRY(save->name), 160);
    gtk_box_pack_start(GTK_BOX(content), save->name, FALSE, FALSE, 6);
    save->status = radios_ui_label("", "small"); gtk_label_set_line_wrap(GTK_LABEL(save->status), TRUE);
    gtk_box_pack_start(GTK_BOX(content), save->status, FALSE, FALSE, 6);
    g_object_set_data_full(G_OBJECT(save->dialog), "weekly-programme-save", save, save_free);
    g_signal_connect(save->dialog, "response", G_CALLBACK(save_response), save); g_signal_connect(save->dialog, "destroy", G_CALLBACK(save_destroyed), save);
    g_signal_connect_object(owner, "destroy", G_CALLBACK(gtk_widget_destroy), save->dialog, G_CONNECT_SWAPPED);
    save->timer = g_timeout_add(250, save_tick, save); gtk_widget_show_all(save->dialog);
}
