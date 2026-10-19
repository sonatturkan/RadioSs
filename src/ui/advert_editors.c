#include "radios/advert_views.h"
#include "radios/media_ui.h"
#include "radios/playout_ui.h"
#include "radios/advert_ui.h"
#include "radios/widgets.h"

#include <string.h>

struct RadioSAdvertViews {
    RadioSViewContext *ctx;
    GtkWidget *page, *status, *campaign_title, *spot_title;
    GtkWidget *notifications, *unread_count, *unread;
    GtkWidget *day, *output, *breaks, *break_summary, *preview_view, *playout_status;
    GtkWidget *report_from, *report_to, *report_campaign, *report_output;
    GtkWidget *report_previous, *report_next;
    GtkWidget *versions, *counters[6], *report_view, *download, *plan_download;
    GtkWidget *file_status, *plan_status;
    GtkWidget *version_table;
    GtkWidget *spot_bind, *spot_approve, *campaign_suspend, *campaign_resume, *campaign_archive;
    GtkWidget *break_preview, *break_reserve, *break_cancel, *break_play, *break_schedule;
    GPtrArray *buttons;
    JsonObject *campaign, *detail, *overview, *preview, *reservation, *prepared, *report;
    GPtrArray *break_records, *output_records;
    guint campaign_epoch, bind_epoch, prepare_epoch, timer;
    gulong break_handler;
    gboolean busy, prepare_pending, syncing;
};

static void workflow_action(GtkButton *button, gpointer data);
static void report_load(GtkButton *button, gpointer data);
static void break_editor(GtkButton *button, gpointer data);
static void break_changed(GtkComboBox *combo, gpointer data);
static void break_filters_changed(GtkWidget *widget, gpointer data);
static void notification_changed(GtkToggleButton *button, gpointer data);
static void overview_apply(RadioSAdvertViews *v);
static void render_campaign(RadioSAdvertViews *v);
static void render_notifications(RadioSAdvertViews *v);
static void operation_done(RadioSViewContext *ctx, GtkWidget *owner,
    JsonNode *result, const GError *error, gpointer data);
static gboolean playout_tick(gpointer data);
static JsonObject *selected_break(RadioSAdvertViews *v);
static JsonArray *array_member(JsonObject *object,const gchar *key);
static gboolean preview_reservable(RadioSAdvertViews *v)
{
    if(!v->preview)return FALSE;
    JsonArray *selected=array_member(v->preview,"selected");
    const gchar *empty=radios_json_string(v->preview,"empty_action","");
    return (selected&&json_array_get_length(selected)>0)||g_str_equal(empty,"fixed_only")||g_str_equal(empty,"overhead")||g_str_equal(empty,"skip");
}

static void pack(GtkWidget *parent, GtkWidget *child, gboolean expand)
{ gtk_box_pack_start(GTK_BOX(parent), child, expand, expand, 0); }
static GtkWidget *box(GtkOrientation direction, gint spacing, const gchar *style)
{ return radios_ui_box(direction, spacing, style); }
static GtkWidget *named(GtkWidget *widget, const gchar *name)
{ gtk_widget_set_name(widget, name); return widget; }
static void replace_object(JsonObject **target, JsonObject *source)
{ JsonObject *copy = source ? json_object_ref(source) : NULL; if (*target) json_object_unref(*target); *target = copy; }
static JsonObject *object_member(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonArray *array_member(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static gchar *today(void)
{ GDateTime *now = g_date_time_new_now_utc(); gchar *date = g_date_time_format(now, "%Y-%m-%d"); g_date_time_unref(now); return date; }
static void status(RadioSAdvertViews *v, const gchar *text)
{ if (v->status) gtk_label_set_text(GTK_LABEL(v->status), text); }
static void busy(RadioSAdvertViews *v, gboolean value)
{
    v->busy = value;
    for (guint i = 0; i < v->buttons->len; ++i)
        gtk_widget_set_sensitive(g_ptr_array_index(v->buttons, i), !value ||
            g_str_equal(gtk_widget_get_name(g_ptr_array_index(v->buttons, i)), "advert-break-stop"));
    if (v->breaks) gtk_widget_set_sensitive(v->breaks, !value);
    if (v->day) gtk_widget_set_sensitive(v->day, !value);
    if (v->output) gtk_widget_set_sensitive(v->output, !value);
    if(v->break_preview){JsonObject *record=selected_break(v);gboolean reserved=g_str_equal(radios_json_string(record,"reservation_state",""),"reserved");
        gtk_widget_set_sensitive(v->break_preview,!value&&record!=NULL);
        gtk_widget_set_sensitive(v->break_reserve,!value&&preview_reservable(v));
        gtk_widget_set_sensitive(v->break_cancel,!value&&reserved);gtk_widget_set_sensitive(v->break_play,!value&&reserved);gtk_widget_set_sensitive(v->break_schedule,!value&&reserved);
    }
    if(v->report_previous){JsonNode *next=v->report?json_object_get_member(v->report,"next_offset"):NULL;
        gtk_widget_set_sensitive(v->report_previous,!value&&v->report&&radios_json_int(v->report,"offset",0)>0);
        gtk_widget_set_sensitive(v->report_next,!value&&next&&!JSON_NODE_HOLDS_NULL(next));
    }
}
static GtkWidget *button(RadioSAdvertViews *v, GtkWidget *parent,
    const gchar *title, const gchar *id, const gchar *operation)
{
    GtkWidget *widget = named(radios_ui_button(title), id);
    if (operation) {
        g_object_set_data(G_OBJECT(widget), "advert-operation", (gpointer)operation);
        g_signal_connect(widget, "clicked", G_CALLBACK(workflow_action), v);
    }
    g_ptr_array_add(v->buttons, widget); pack(parent, widget, FALSE); return widget;
}
static GtkWidget *table(GtkWidget *parent, const gchar *id,
    const gchar **headers, guint count)
{
    GtkWidget *scroll = radios_ui_table(headers, count);
    GtkWidget *view = g_object_get_data(G_OBJECT(scroll), "tree-view");
    named(view, id); gtk_widget_set_size_request(scroll, -1, 130);
    gtk_style_context_add_class(gtk_widget_get_style_context(scroll), "advert-table-scroll");
    gtk_style_context_add_class(gtk_widget_get_style_context(view), "advert-table");
    pack(parent, scroll, FALSE); return view;
}
static GtkListStore *strings_model(GtkWidget *view, guint count)
{
    GType types[10]; g_return_val_if_fail(count <= G_N_ELEMENTS(types), NULL);
    for (guint i = 0; i < count; ++i) types[i] = G_TYPE_STRING;
    GtkListStore *store = gtk_list_store_newv((gint)count, types);
    gtk_tree_view_set_model(GTK_TREE_VIEW(view), GTK_TREE_MODEL(store)); g_object_unref(store); return store;
}
static void row(GtkListStore *store, const gchar **values, guint count)
{
    GtkTreeIter iter; gtk_list_store_append(store, &iter);
    for (guint i = 0; i < count; ++i) gtk_list_store_set(store, &iter, (gint)i, values[i], -1);
}
static void free_views(gpointer data)
{
    RadioSAdvertViews *v = data;
    if (v->timer) g_source_remove(v->timer);
    replace_object(&v->campaign, NULL); replace_object(&v->detail, NULL); replace_object(&v->overview, NULL);
    replace_object(&v->preview, NULL); replace_object(&v->reservation, NULL);
    replace_object(&v->prepared, NULL); replace_object(&v->report, NULL);
    g_ptr_array_unref(v->buttons); g_ptr_array_unref(v->break_records);
    g_ptr_array_unref(v->output_records); g_free(v);
}
static void views_destroyed(GtkWidget *page, gpointer data)
{
    (void)page; RadioSAdvertViews *v = data;
    if(v->timer){g_source_remove(v->timer);v->timer=0;}
    ++v->prepare_epoch;
}
RadioSAdvertViews *radios_advert_views_new(RadioSViewContext *ctx, GtkWidget *page)
{
    RadioSAdvertViews *v = g_new0(RadioSAdvertViews, 1); v->ctx = ctx; v->page = page;
    v->buttons = g_ptr_array_new();
    v->break_records = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
    v->output_records = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
    g_object_set_data_full(G_OBJECT(page), "advert-views", v, free_views);
    g_signal_connect(page,"destroy",G_CALLBACK(views_destroyed),v);return v;
}

GtkWidget *radios_advert_views_notifications(RadioSAdvertViews *v)
{
    GtkWidget *card = box(GTK_ORIENTATION_VERTICAL, 12, "advert-card");
    gtk_style_context_add_class(gtk_widget_get_style_context(card), "advert-notifications");
    GtkWidget *heading = box(GTK_ORIENTATION_HORIZONTAL, 8, "advert-toolbar");
    pack(heading, radios_ui_label("Bildirimler", "heading"), TRUE);
    v->unread_count = named(radios_ui_label("— okunmamış", "advert-sub"), "advert-unread-count");
    pack(heading, v->unread_count, FALSE);
    GtkWidget *all = named(gtk_radio_button_new_with_label(NULL, "Tümü"), "advert-notifications-all");
    v->unread = named(gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(all), "Okunmamış"), "advert-notifications-unread");
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(all), FALSE);
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(v->unread), FALSE);
    g_signal_connect(v->unread, "toggled", G_CALLBACK(notification_changed), v);
    pack(heading, all, FALSE); pack(heading, v->unread, FALSE);
    button(v, heading, "Okundu", "advert-notifications-read", "notifications-read");
    pack(card, heading, FALSE);
    v->notifications = named(gtk_list_box_new(), "advert-notification-list");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(v->notifications), GTK_SELECTION_NONE);
    pack(card, v->notifications, FALSE); return card;
}

static GtkWidget *combo(const gchar *id, const gchar *first)
{
    GtkWidget *widget = named(gtk_combo_box_text_new(), id);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(widget), "", first);
    gtk_combo_box_set_active(GTK_COMBO_BOX(widget), 0); return widget;
}

GtkWidget *radios_advert_views_tools(RadioSAdvertViews *v)
{
    GtkWidget *expander = named(gtk_expander_new("Kuşak, sürüm ve yayın sonucu araçları"), "advert-tools");
    gtk_style_context_add_class(gtk_widget_get_style_context(expander), "advert-advanced-details");
    GtkWidget *body = box(GTK_ORIENTATION_VERTICAL, 14, "advert-card");
    v->status = named(radios_ui_label("Veri servisi bağlantısı bekleniyor.", "advert-sub"), "advert-workflow-status");
    gtk_label_set_line_wrap(GTK_LABEL(v->status), TRUE); pack(body, v->status, FALSE);
    v->campaign_title = named(radios_ui_label("Kampanya seçilmedi", "heading"), "advert-selected-campaign");
    pack(body, v->campaign_title, FALSE);
    v->spot_title = named(radios_ui_label("Doğrulanmış spot bağlanmadı", "advert-sub"), "advert-bound-spot");
    pack(body, v->spot_title, FALSE);
    GtkWidget *actions = box(GTK_ORIENTATION_HORIZONTAL, 7, "advert-actions");
    v->spot_bind = button(v, actions, "Kütüphaneden spot seç", "advert-spot-bind", "spot-bind");
    v->spot_approve = button(v, actions, "Spotu onayla", "advert-spot-approve", "spot-approve");
    v->campaign_suspend = button(v, actions, "Askıya al", "advert-campaign-suspend", "campaign-suspend");
    v->campaign_resume = button(v, actions, "Devam ettir", "advert-campaign-resume", "campaign-resume");
    v->campaign_archive = button(v, actions, "Arşivle", "advert-campaign-archive", "campaign-archive");
    pack(body, actions, FALSE);
    const gchar *version_headers[] = {"Sürüm", "Ölçülen süre", "Onay", "SHA / bağ"};
    v->version_table = table(body, "advert-version-table", version_headers, G_N_ELEMENTS(version_headers));
    strings_model(v->version_table, 5);
    pack(body, radios_ui_label("Kuşak ve saat planı", "heading"), FALSE);
    GtkWidget *filters = box(GTK_ORIENTATION_HORIZONTAL, 8, "advert-toolbar");
    pack(filters, radios_ui_label("Gün", NULL), FALSE);
    gchar *date = today(); v->day = named(radios_ui_entry("YYYY-AA-GG"), "advert-day");
    gtk_entry_set_text(GTK_ENTRY(v->day), date); g_free(date);
    gtk_entry_set_width_chars(GTK_ENTRY(v->day), 10); gtk_entry_set_max_length(GTK_ENTRY(v->day), 10);
    g_signal_connect(v->day,"changed",G_CALLBACK(break_filters_changed),v);
    pack(filters, v->day, FALSE);
    v->output = combo("advert-output", "Çıkış seç"); g_signal_connect(v->output,"changed",G_CALLBACK(break_filters_changed),v);pack(filters, v->output, TRUE);
    button(v, filters, "Yerel MAIN çıkışı kur", "advert-output-create", "output-create");
    button(v, filters, "Yenile", "advert-refresh", "refresh"); pack(body, filters, FALSE);
    actions = box(GTK_ORIENTATION_HORIZONTAL, 7, "advert-actions");
    v->breaks = combo("advert-break-selector", "Kaydedilmiş kuşak seç");
    v->break_handler=g_signal_connect(v->breaks, "changed", G_CALLBACK(break_changed), v); pack(actions, v->breaks, TRUE);
    GtkWidget *add = button(v, actions, "+ Kuşak", "advert-break-add", NULL);
    g_signal_connect(add, "clicked", G_CALLBACK(break_editor), v);
    GtkWidget *edit = button(v, actions, "Düzenle", "advert-break-edit", NULL);
    g_object_set_data(G_OBJECT(edit), "advert-edit", GINT_TO_POINTER(1));
    g_signal_connect(edit, "clicked", G_CALLBACK(break_editor), v); pack(body, actions, FALSE);
    v->break_summary = named(radios_ui_label("Kuşak seçilmedi", "advert-sub"), "advert-break-summary");
    pack(body, v->break_summary, FALSE);
    actions = box(GTK_ORIENTATION_HORIZONTAL, 7, "advert-actions");
    v->break_preview = button(v, actions, "Yerleşimi önizle", "advert-break-preview", "break-preview");
    v->break_reserve = button(v, actions, "Rezerve et", "advert-break-reserve", "break-reserve");
    v->break_cancel = button(v, actions, "Rezervasyonu iptal et", "advert-break-cancel", "break-cancel");
    button(v, actions, "Yeniden planla", "advert-break-replan", "break-preview"); pack(body, actions, FALSE);
    const gchar *preview_headers[] = {"Spot / kampanya", "Süre", "Yerleşim", "Neden"};
    v->preview_view = table(body, "advert-break-preview-table", preview_headers, G_N_ELEMENTS(preview_headers));
    strings_model(v->preview_view, 5);
    actions = box(GTK_ORIENTATION_HORIZONTAL, 7, "advert-actions");
    v->break_play = button(v, actions, "Kuşağı çal", "advert-break-play", "break-play");
    v->break_schedule = button(v, actions, "Saatinde başlat", "advert-break-schedule", "break-schedule");
    button(v, actions, "Duraklat", "advert-break-pause", "pause");
    button(v, actions, "Devam", "advert-break-resume", "resume");
    button(v, actions, "Durdur", "advert-break-stop", "stop"); pack(body, actions, FALSE);
    v->playout_status = named(radios_ui_label("Yerel kuşak motoru duruyor", "advert-sub"), "advert-playout-status");
    gtk_label_set_line_wrap(GTK_LABEL(v->playout_status), TRUE); pack(body, v->playout_status, FALSE);
    pack(body, radios_ui_label("Mutabakat raporu", "heading"), FALSE);
    filters = box(GTK_ORIENTATION_HORIZONTAL, 8, "advert-toolbar");
    date = today(); v->report_from = named(radios_ui_entry("YYYY-AA-GG"), "advert-asrun-from");
    v->report_to = named(radios_ui_entry("YYYY-AA-GG"), "advert-asrun-to");
    gtk_entry_set_text(GTK_ENTRY(v->report_from), date); gtk_entry_set_text(GTK_ENTRY(v->report_to), date); g_free(date);
    gtk_entry_set_width_chars(GTK_ENTRY(v->report_from), 10); gtk_entry_set_width_chars(GTK_ENTRY(v->report_to), 10);
    gtk_entry_set_max_length(GTK_ENTRY(v->report_from), 10); gtk_entry_set_max_length(GTK_ENTRY(v->report_to), 10);
    pack(filters, v->report_from, FALSE); pack(filters, v->report_to, FALSE);
    v->report_campaign = combo("advert-asrun-campaign", "Tüm kampanyalar"); pack(filters, v->report_campaign, TRUE);
    v->report_output = combo("advert-asrun-output", "Tüm çıkışlar"); pack(filters, v->report_output, TRUE);
    GtkWidget *load = button(v, filters, "Rapor oluştur", "advert-asrun-load", NULL);
    g_signal_connect(load, "clicked", G_CALLBACK(report_load), v); pack(body, filters, FALSE);
    actions=box(GTK_ORIENTATION_HORIZONTAL,7,"advert-actions");
    v->report_previous=button(v,actions,"Önceki rapor sayfası","advert-asrun-previous","report-previous");
    v->report_next=button(v,actions,"Sonraki rapor sayfası","advert-asrun-next","report-next");pack(body,actions,FALSE);
    gtk_container_add(GTK_CONTAINER(expander), body);
    v->timer = g_timeout_add(200, playout_tick, v);busy(v,FALSE);render_campaign(v);return expander;
}

void radios_advert_views_bind_report(RadioSAdvertViews *v, GtkWidget *versions,
    GtkWidget **counters, GtkWidget *view, GtkWidget *download, GtkWidget *plan_download,
    GtkWidget *file_status, GtkWidget *plan_status)
{
    v->versions = versions; v->report_view = view; v->download = download; v->plan_download = plan_download;
    v->file_status=file_status;v->plan_status=plan_status;
    named(file_status,"advert-file-state");named(plan_status,"advert-plan-state");
    for (guint i = 0; i < 6; ++i) v->counters[i] = counters[i];
    named(view, "advert-asrun-table"); strings_model(view, 7);
    named(download, "advert-asrun-export"); named(plan_download, "advert-plan-export");
    g_object_set_data(G_OBJECT(download), "advert-operation", "report-export");
    g_object_set_data(G_OBJECT(plan_download), "advert-operation", "plan-export");
    g_signal_connect(download, "clicked", G_CALLBACK(workflow_action), v);
    g_signal_connect(plan_download, "clicked", G_CALLBACK(workflow_action), v);
}

static JsonObject *selected_break(RadioSAdvertViews *v)
{
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(v->breaks));
    for (guint i = 0; id && i < v->break_records->len; ++i) {
        JsonObject *record = g_ptr_array_index(v->break_records, i);
        if (g_str_equal(id, radios_json_string(record, "_id", ""))) return record;
    }
    return NULL;
}
static void keyed_body(GtkWidget *owner, JsonObject *body)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, body);
    gchar *serialized = json_to_string(node, FALSE); json_node_free(node);
    gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, serialized, -1); g_free(serialized);
    const gchar *previous_hash = g_object_get_data(G_OBJECT(owner), "advert-idempotency-body");
    if (g_strcmp0(hash, previous_hash) != 0) g_object_set_data(G_OBJECT(owner), "advert-idempotency-key", NULL);
    g_object_set_data_full(G_OBJECT(owner), "advert-idempotency-body", hash, g_free);
    const gchar *key = g_object_get_data(G_OBJECT(owner), "advert-idempotency-key");
    if (!key) {
        gchar *created = g_uuid_string_random();
        g_object_set_data_full(G_OBJECT(owner), "advert-idempotency-key", created, g_free); key = created;
    }
    json_object_set_string_member(body, "idempotency_key", key);
}
static gboolean request(RadioSAdvertViews *v, GtkWidget *owner,
    const gchar *operation, JsonNode *body, RadioSDataCallback callback, gpointer user)
{
    gchar *path = g_strdup_printf("/v1/workflows/desktop-advert/%s", operation);
    gboolean accepted = radios_data_request(v->ctx, owner, "POST", path, body, callback, user, NULL);
    g_free(path); json_node_free(body);
    if (!accepted) status(v, "İşlem başlatılamadı; veri servisi bağlantısını kontrol edin.");
    return accepted;
}
static void notification_changed(GtkToggleButton *toggle, gpointer data)
{ (void)toggle; render_notifications(data); }
static void render_notifications(RadioSAdvertViews *v)
{
    if (!v->notifications) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(v->notifications));
    for (GList *item = children; item; item = item->next) gtk_widget_destroy(item->data);
    g_list_free(children);
    JsonArray *records = array_member(v->overview, "notifications"); guint unread = 0, shown = 0;
    gboolean only_unread = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(v->unread));
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        JsonObject *record = json_array_get_object_element(records, i);
        gboolean read = json_object_has_member(record, "read") && json_object_get_boolean_member(record, "read");
        if (!read) ++unread;
        if (only_unread && read) continue;
        const gchar *reason=radios_json_string(record,"reason","");
        const gchar *caption=g_str_equal(reason,"spot_unbound")?"Doğrulanmış spot bekleniyor":g_str_equal(reason,"spot_unapproved")?"Spot onayı bekleniyor":g_str_equal(reason,"break_unready")?"Kuşak sesleri hazır değil":radios_json_string(record,"message","Bildirim");
        GtkWidget *label = radios_ui_label(caption, "advert-sub");
        gtk_widget_set_tooltip_text(label,radios_json_string(record,"message",caption));
        gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
        gtk_container_add(GTK_CONTAINER(v->notifications), label); ++shown;
    }
    if (!shown) gtk_container_add(GTK_CONTAINER(v->notifications), radios_ui_label("Bildirim yok", "advert-empty"));
    gchar *count = g_strdup_printf("%u okunmamış", unread);
    gtk_label_set_text(GTK_LABEL(v->unread_count), count); g_free(count); gtk_widget_show_all(v->notifications);
}
static void render_campaign(RadioSAdvertViews *v)
{
    if (!v->campaign_title) return;
    gtk_label_set_text(GTK_LABEL(v->campaign_title), radios_json_string(v->campaign, "name", "Kampanya seçilmedi"));
    JsonObject *binding = object_member(v->campaign, "binding");
    const gchar *state = radios_json_string(v->campaign, "status", "");
    gtk_widget_set_sensitive(v->spot_bind, !v->busy && v->campaign && !g_str_equal(state, "cancelled"));
    gtk_widget_set_sensitive(v->spot_approve, !v->busy && binding != NULL);
    gtk_widget_set_sensitive(v->campaign_suspend, !v->busy && g_str_equal(state, "approved"));
    gtk_widget_set_sensitive(v->campaign_resume, !v->busy && (g_str_equal(state, "paused") || g_str_equal(state, "suspended")));
    gtk_widget_set_sensitive(v->campaign_archive, !v->busy && v->campaign && !g_str_equal(state, "cancelled"));
    gchar *text = binding ? g_strdup_printf("Doğrulanmış medya %s · SHA %.12s", radios_json_string(binding, "media_id", ""), radios_json_string(binding, "media_sha256", "")) : g_strdup("Doğrulanmış spot bağlanmadı");
    gtk_label_set_text(GTK_LABEL(v->spot_title), text); g_free(text);
    if(v->file_status)gtk_label_set_text(GTK_LABEL(v->file_status),binding?"İçe alınmış sürüm bağı · yedek kopya kanıtı yok":"Doğrulanmış medya bağı yok");
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(v->version_table))); gtk_list_store_clear(store);
    JsonArray *versions = array_member(v->campaign, "versions");
    for (guint i = 0; versions && i < json_array_get_length(versions); ++i) {
        JsonObject *version = json_array_get_object_element(versions, i);
        gchar *duration = radios_json_int(version, "duration_ms", 0) > 0 ?
            g_strdup_printf("%.3f sn", (gdouble)radios_json_int(version, "duration_ms", 0) / 1000.0) : g_strdup("Ölçüm yok");
        const gchar *values[] = {radios_json_string(version, "name", radios_json_string(version, "label", "Sürüm")), duration,
            radios_json_string(version, "status", "draft"), radios_json_string(version, "sha256", ""), radios_json_string(version, "_id", "")};
        row(store, values, G_N_ELEMENTS(values)); g_free(duration);
    }
    if (v->versions) {
        text = g_strdup_printf("%u gerçek sürüm · %u onaylı spot", versions ? json_array_get_length(versions) : 0,
            array_member(v->campaign, "spots") ? json_array_get_length(array_member(v->campaign, "spots")) : 0);
        gtk_label_set_text(GTK_LABEL(v->versions), text); g_free(text);
    }
}
void radios_advert_views_select(RadioSAdvertViews *v, JsonObject *campaign)
{
    const gchar *id = radios_json_string(campaign, "_id", "");
    gboolean changed = !g_str_equal(id, radios_json_string(v->campaign, "_id", ""));
    if (changed) ++v->campaign_epoch;
    JsonArray *records = array_member(v->overview, "campaigns"); JsonObject *chosen = campaign;
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        JsonObject *record = json_array_get_object_element(records, i);
        if (g_str_equal(id, radios_json_string(record, "_id", ""))) { if(radios_json_int(record,"_rev",0)>=radios_json_int(chosen,"_rev",0))chosen=record;break; }
    }
    if(v->campaign&&g_str_equal(id,radios_json_string(v->campaign,"_id",""))&&radios_json_int(v->campaign,"_rev",0)>radios_json_int(chosen,"_rev",0))chosen=v->campaign;
    replace_object(&v->campaign, chosen); render_campaign(v);
    if (changed && v->report_campaign) gtk_combo_box_set_active_id(GTK_COMBO_BOX(v->report_campaign), id);
    if (v->plan_download) gtk_widget_set_sensitive(v->plan_download, selected_break(v) && v->campaign != NULL);
}
static void overview_apply(RadioSAdvertViews *v)
{
    if (!v->breaks) return;
    v->syncing=TRUE;
    gchar *output_id = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(v->output)));
    gchar *report_id = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(v->report_output)));
    gchar *campaign_id = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(v->report_campaign)));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(v->output)); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(v->output), "", "Çıkış seç");
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(v->report_output)); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(v->report_output), "", "Tüm çıkışlar");
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(v->report_campaign)); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(v->report_campaign), "", "Tüm kampanyalar");
    g_ptr_array_set_size(v->break_records, 0); g_ptr_array_set_size(v->output_records, 0);
    JsonArray *records = array_member(v->overview, "breaks");
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        JsonObject *record = json_array_get_object_element(records, i); g_ptr_array_add(v->break_records, json_object_ref(record));
    }
    records = array_member(v->overview, "outputs");
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        JsonObject *record = json_array_get_object_element(records, i); g_ptr_array_add(v->output_records, json_object_ref(record));
        const gchar *id = radios_json_string(record, "output_id", "");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(v->output), id, id);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(v->report_output), id, id);
    }
    if (!output_id || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(v->output), output_id)) gtk_combo_box_set_active(GTK_COMBO_BOX(v->output), 0);
    if (!report_id || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(v->report_output), report_id)) gtk_combo_box_set_active(GTK_COMBO_BOX(v->report_output), 0);
    records = array_member(v->overview, "campaigns");
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        JsonObject *record = json_array_get_object_element(records, i);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(v->report_campaign), radios_json_string(record, "_id", ""), radios_json_string(record, "name", ""));
    }
    if (!campaign_id || !gtk_combo_box_set_active_id(GTK_COMBO_BOX(v->report_campaign), campaign_id)) gtk_combo_box_set_active(GTK_COMBO_BOX(v->report_campaign), 0);
    g_free(output_id); g_free(report_id); g_free(campaign_id);
    v->syncing=FALSE;break_filters_changed(NULL,v);
    radios_advert_views_select(v, v->campaign); render_notifications(v);
}
static void break_filters_changed(GtkWidget *widget,gpointer data)
{
    (void)widget;RadioSAdvertViews *v=data;if(v->syncing||!v->breaks||!v->output)return;
    gchar *selected=g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(v->breaks)));
    const gchar *date=gtk_entry_get_text(GTK_ENTRY(v->day)),*output=gtk_combo_box_get_active_id(GTK_COMBO_BOX(v->output));
    g_signal_handler_block(v->breaks,v->break_handler);
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(v->breaks));gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(v->breaks),"","Kaydedilmiş kuşak seç");
    for(guint i=0;i<v->break_records->len;++i){JsonObject *record=g_ptr_array_index(v->break_records,i),*config=object_member(record,"config");
        if((*date&&!g_str_equal(date,radios_json_string(config,"date","")))||(output&&*output&&!g_str_equal(output,radios_json_string(config,"output_id",""))))continue;
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(v->breaks),radios_json_string(record,"_id",""),radios_json_string(record,"name","Kuşak"));
    }
    if(!selected||!gtk_combo_box_set_active_id(GTK_COMBO_BOX(v->breaks),selected))gtk_combo_box_set_active(GTK_COMBO_BOX(v->breaks),0);
    g_signal_handler_unblock(v->breaks,v->break_handler);g_free(selected);break_changed(GTK_COMBO_BOX(v->breaks),v);
}
static void overview_loaded(RadioSViewContext *ctx, GtkWidget *page, JsonNode *result, const GError *error, gpointer data)
{
    (void)ctx; (void)page; RadioSAdvertViews *v = data;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { status(v, error ? error->message : "Reklam çalışma alanı okunamadı."); return; }
    replace_object(&v->overview, json_node_get_object(result)); overview_apply(v); status(v, "Reklam kayıtları yüklendi.");
}
void radios_advert_views_refresh(RadioSAdvertViews *v)
{
    if (!radios_data_connected(v->ctx)) return;
    request(v, v->page, "overview", radios_data_body(v->ctx), overview_loaded, v);
}
static void break_changed(GtkComboBox *combo_widget, gpointer data)
{
    (void)combo_widget; RadioSAdvertViews *v = data; JsonObject *record = selected_break(v);
    replace_object(&v->preview, NULL); replace_object(&v->prepared, NULL);
    if (!v->break_summary) return;
    JsonObject *config = object_member(record, "config");
    gchar *text = config ? g_strdup_printf("%s %s · %s · kapasite %" G_GINT64_FORMAT " sn · plan revizyonu %" G_GINT64_FORMAT,
        radios_json_string(config, "date", ""), radios_json_string(config, "time", ""), radios_json_string(config, "output_id", ""),
        radios_json_int(config, "capacity_seconds", 0), radios_json_int(record, "plan_revision", 0)) : g_strdup("Kuşak seçilmedi");
    gtk_label_set_text(GTK_LABEL(v->break_summary), text); g_free(text);
    if(v->plan_status)gtk_label_set_text(GTK_LABEL(v->plan_status),radios_json_string(record,"reservation_state","Kuşak planı seçilmedi"));
    if (v->plan_download) gtk_widget_set_sensitive(v->plan_download, record && v->campaign != NULL);
    busy(v,v->busy);render_campaign(v);
}

static GtkWidget *find(GtkWidget *widget, const gchar *id)
{
    if (g_str_equal(gtk_widget_get_name(widget), id)) return widget;
    if (!GTK_IS_CONTAINER(widget)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, id);
    g_list_free(children); return result;
}
static const gchar *text(GtkWidget *form, const gchar *id)
{ GtkWidget *field = find(form, id); return GTK_IS_ENTRY(field) ? gtk_entry_get_text(GTK_ENTRY(field)) : ""; }
static GtkWidget *spin(gdouble minimum, gdouble maximum, gdouble value)
{
    GtkWidget *field = gtk_spin_button_new_with_range(minimum, maximum, 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(field), value); return field;
}
static void field(GtkWidget *grid, guint index, const gchar *title, const gchar *id, GtkWidget *widget)
{
    GtkWidget *group = box(GTK_ORIENTATION_VERTICAL, 6, "advert-field");
    pack(group, radios_ui_label(title, "form-label"), FALSE); pack(group, named(widget, id), FALSE);
    gtk_widget_set_hexpand(group, TRUE); gtk_grid_attach(GTK_GRID(grid), group, (gint)(index % 2), (gint)(index / 2), 1, 1);
}
static GtkWidget *dialog_form(RadioSAdvertViews *v, const gchar *title, const gchar *id, GtkWidget **form)
{
    GtkWidget *dialog = gtk_dialog_new_with_buttons(title, v->ctx->parent,
        GTK_DIALOG_MODAL | GTK_DIALOG_DESTROY_WITH_PARENT, "Vazgeç", GTK_RESPONSE_CANCEL, "Kaydet", GTK_RESPONSE_ACCEPT, NULL);
    g_signal_connect_object(v->page,"destroy",G_CALLBACK(gtk_widget_destroy),dialog,G_CONNECT_SWAPPED);
    named(dialog, id); gtk_window_set_default_size(GTK_WINDOW(dialog), 680, 650);
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "radios");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "advert-dialog");
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    *form = box(GTK_ORIENTATION_VERTICAL, 14, "advert-form"); gtk_container_set_border_width(GTK_CONTAINER(*form), 24);
    gtk_container_add(GTK_CONTAINER(scroll), *form); pack(gtk_dialog_get_content_area(GTK_DIALOG(dialog)), scroll, TRUE);
    GtkWidget *label = named(radios_ui_label("", "advert-sub"), "advert-form-status"); gtk_label_set_line_wrap(GTK_LABEL(label), TRUE); pack(*form, label, FALSE);
    g_object_set_data(G_OBJECT(dialog), "advert-form", *form); g_object_set_data(G_OBJECT(dialog), "advert-form-status", label);
    return dialog;
}
static void form_status(GtkWidget *dialog, const gchar *message)
{ GtkWidget *label = g_object_get_data(G_OBJECT(dialog), "advert-form-status"); if (label) gtk_label_set_text(GTK_LABEL(label), message); }
static gboolean valid_date(const gchar *value)
{
    if (!g_regex_match_simple("^[0-9]{4}-[0-9]{2}-[0-9]{2}$", value, 0, 0)) return FALSE;
    gchar *iso = g_strdup_printf("%sT00:00:00Z", value); GDateTime *date = g_date_time_new_from_iso8601(iso, NULL); g_free(iso);
    if (date) g_date_time_unref(date);
    return date != NULL;
}
static JsonObject *binding_from_detail(JsonObject *detail)
{
    JsonObject *media = object_member(detail, "media"), *version = object_member(detail, "version");
    if (!media || !version) return NULL;
    JsonObject *binding = json_object_new();
    json_object_set_string_member(binding, "media_id", radios_json_string(media, "_id", ""));
    json_object_set_string_member(binding, "version_id", radios_json_string(version, "_id", ""));
    json_object_set_string_member(binding, "media_file_id", radios_json_string(version, "media_file_id", ""));
    json_object_set_string_member(binding, "media_sha256", radios_json_string(version, "sha256", "")); return binding;
}
static void overhead_picked(RadioSViewContext *ctx, GtkWidget *dialog, JsonObject *detail, gpointer user)
{
    (void)ctx; const gchar *kind = user; GtkWidget *form = g_object_get_data(G_OBJECT(dialog), "advert-form");
    gchar *id = g_strdup_printf("advert-k-%s", kind); GtkWidget *field_widget = find(form, id); g_free(id);
    JsonObject *analysis = object_member(detail, "analysis"); gint64 duration = radios_json_int(analysis, "duration_ms", 0);
    if (!field_widget || duration <= 0) { form_status(dialog, "Seçilen sesin ölçülmüş süresi yok."); return; }
    if(duration>(g_str_equal(kind,"fixed")?300000:120000)){form_status(dialog,"Seçilen ses bu kuşak alanı için fazla uzun; açılış/kapanış en fazla 120, sabit anons 300 saniye olabilir.");return;}
    g_object_set_data_full(G_OBJECT(field_widget), "advert-media-binding", binding_from_detail(detail), (GDestroyNotify)json_object_unref);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(field_widget), (gdouble)((duration + 999) / 1000));
    gchar *message = g_strdup_printf("%s: %s · ölçülen %.3f sn", kind,
        radios_json_string(object_member(detail, "media"), "title", "Ses"), (gdouble)duration / 1000.0);
    form_status(dialog, message); g_free(message);
    g_object_set_data(G_OBJECT(dialog), "advert-idempotency-key", NULL);
}
static void overhead_choose(GtkButton *widget, gpointer data)
{
    RadioSAdvertViews *v = data; GtkWidget *dialog = gtk_widget_get_toplevel(GTK_WIDGET(widget));
    const gchar *kind = g_object_get_data(G_OBJECT(widget), "advert-overhead");
    radios_media_library_pick(v->ctx, dialog, overhead_picked, (gpointer)kind);
}
static void overhead_clear(GtkButton *widget, gpointer data)
{
    (void)data; GtkWidget *dialog = gtk_widget_get_toplevel(GTK_WIDGET(widget));
    GtkWidget *form = g_object_get_data(G_OBJECT(dialog), "advert-form"); const gchar *kind = g_object_get_data(G_OBJECT(widget), "advert-overhead");
    gchar *id = g_strdup_printf("advert-k-%s", kind); GtkWidget *field_widget = find(form, id); g_free(id);
    g_object_set_data(G_OBJECT(field_widget), "advert-media-binding", NULL); gtk_spin_button_set_value(GTK_SPIN_BUTTON(field_widget), 0);
    g_object_set_data(G_OBJECT(dialog), "advert-idempotency-key", NULL);
}
static void editor_saved(RadioSViewContext *ctx, GtkWidget *dialog, JsonNode *result, const GError *error, gpointer data)
{
    RadioSAdvertViews *v = data; gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT, TRUE);
    GtkWidget *form=g_object_get_data(G_OBJECT(dialog),"advert-form");gtk_widget_set_sensitive(form,TRUE);
    if (error || !result) { form_status(dialog, error ? error->message : "Kayıt döndürülmedi; alanlarınız korunuyor."); return; }
    const gchar *saved=g_object_get_data(G_OBJECT(dialog),"advert-success-message");
    gchar *message=g_strdup(saved?saved:"Kuşak gerçek planıyla kaydedildi.");
    gtk_widget_destroy(dialog);radios_advert_page_refresh(ctx,v->page);status(v,message);g_free(message);
}
static void break_response(GtkDialog *dialog, gint response, gpointer data)
{
    RadioSAdvertViews *v = data;
    if (response != GTK_RESPONSE_ACCEPT) { gtk_widget_destroy(GTK_WIDGET(dialog)); return; }
    GtkWidget *form = g_object_get_data(G_OBJECT(dialog), "advert-form");
    const gchar *date = text(form, "advert-k-date"), *time = text(form, "advert-k-time");
    const gchar *output = gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(form, "advert-k-output")));
    if (!*text(form, "advert-k-name") || !valid_date(date) || !g_regex_match_simple("^(?:[01][0-9]|2[0-3]):[0-5][0-9]$", time, 0, 0) || !output || !*output) {
        form_status(GTK_WIDGET(dialog), "Kuşak adı, geçerli tarih/saat ve gerçek bir çıkış seçin."); return;
    }
    JsonNode *body = radios_data_body(v->ctx); JsonObject *object = json_node_get_object(body), *config = json_object_new();
    json_object_set_string_member(object, "name", text(form, "advert-k-name"));
    json_object_set_string_member(config, "date", date); json_object_set_string_member(config, "time", time);
    json_object_set_string_member(config, "output_id", output); json_object_set_string_member(config, "region_id", text(form, "advert-k-region"));
    const gchar *names[] = {"capacity", "tolerance", "fixed", "opener", "closer", "wait"};
    const gchar *keys[] = {"capacity_seconds", "tolerance_seconds", "fixed_seconds", "opener_seconds", "closer_seconds", "wait_seconds"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        gchar *id = g_strdup_printf("advert-k-%s", names[i]); GtkWidget *input = find(form, id); g_free(id);
        json_object_set_int_member(config, keys[i], gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(input)));
        if (i >= 2 && i <= 4) {
            JsonObject *binding = g_object_get_data(G_OBJECT(input), "advert-media-binding");
            if (binding) json_object_set_object_member(config, names[i], json_object_ref(binding));
        }
    }
    json_object_set_int_member(config, "empty_mode", gtk_combo_box_get_active(GTK_COMBO_BOX(find(form, "advert-k-empty"))));
    json_object_set_int_member(config, "start_mode", gtk_combo_box_get_active(GTK_COMBO_BOX(find(form, "advert-k-start"))));
    json_object_set_object_member(object, "config", config); keyed_body(GTK_WIDGET(dialog), object);
    JsonObject *record = g_object_get_data(G_OBJECT(dialog), "advert-break-record");
    if (record) { json_object_set_string_member(object, "document_id", radios_json_string(record, "_id", "")); json_object_set_int_member(object, "expected_revision", radios_json_int(record, "_rev", 0)); }
    if (request(v, GTK_WIDGET(dialog), "break-save", body, editor_saved, v)) {
        gtk_dialog_set_response_sensitive(dialog, GTK_RESPONSE_ACCEPT, FALSE);gtk_widget_set_sensitive(form,FALSE);form_status(GTK_WIDGET(dialog), "Kuşak ve gerçek plan kaydediliyor…");
    }
}
static void start_mode_changed(GtkComboBox *widget, gpointer data)
{ gtk_widget_set_sensitive(GTK_WIDGET(data), gtk_combo_box_get_active(widget) == 2); }
static void break_editor(GtkButton *widget, gpointer data)
{
    RadioSAdvertViews *v = data; JsonObject *record = g_object_get_data(G_OBJECT(widget), "advert-edit") ? selected_break(v) : NULL;
    if (g_object_get_data(G_OBJECT(widget), "advert-edit") && !record) { status(v, "Düzenlemek için kaydedilmiş kuşak seçin."); return; }
    GtkWidget *form; GtkWidget *dialog = dialog_form(v, record ? "Kuşağı düzenle" : "Kuşak ekle", "advert-break-dialog", &form);
    if (record) g_object_set_data_full(G_OBJECT(dialog), "advert-break-record", json_object_ref(record), (GDestroyNotify)json_object_unref);
    JsonObject *config = object_member(record, "config"); GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_homogeneous(GTK_GRID(grid), TRUE); gtk_grid_set_column_spacing(GTK_GRID(grid), 16); gtk_grid_set_row_spacing(GTK_GRID(grid), 14);
    GtkWidget *date = radios_ui_entry("YYYY-AA-GG"), *time = radios_ui_entry("SS:DD"), *name = radios_ui_entry(NULL);
    gtk_entry_set_text(GTK_ENTRY(date), radios_json_string(config, "date", gtk_entry_get_text(GTK_ENTRY(v->day))));
    gtk_entry_set_text(GTK_ENTRY(time), radios_json_string(config, "time", "12:00"));
    gtk_entry_set_text(GTK_ENTRY(name), record ? radios_json_string(record, "name", "") : "");
    gtk_entry_set_max_length(GTK_ENTRY(date), 10); gtk_entry_set_max_length(GTK_ENTRY(time), 5); gtk_entry_set_max_length(GTK_ENTRY(name), 60);
    field(grid, 0, "Saat", "advert-k-time", time); field(grid, 1, "Ad", "advert-k-name", name);
    field(grid, 2, "Kapasite (sn)", "advert-k-capacity", spin(10, 1800, radios_json_int(config, "capacity_seconds", 180)));
    field(grid, 3, "İzin verilen aşım (sn)", "advert-k-tolerance", spin(0, 120, radios_json_int(config, "tolerance_seconds", 0)));
    const gchar *names[] = {"fixed", "opener", "closer"}, *keys[] = {"fixed_seconds", "opener_seconds", "closer_seconds"};
    const gchar *captions[] = {"Sabit anons · ölçülen sn", "Açılış · ölçülen sn", "Kapanış · ölçülen sn"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkWidget *input = spin(0, i == 0 ? 300 : 120, radios_json_int(config, keys[i], 0));
        gtk_editable_set_editable(GTK_EDITABLE(input), FALSE); gtk_widget_set_sensitive(input, FALSE);
        gchar *id = g_strdup_printf("advert-k-%s", names[i]); field(grid, i + 4, captions[i], id, input); g_free(id);
        JsonObject *binding = object_member(config, names[i]);
        if (binding) g_object_set_data_full(G_OBJECT(input), "advert-media-binding", json_object_ref(binding), (GDestroyNotify)json_object_unref);
    }
    const gchar *const empties[] = {"Açılış/kapanış çalmaz", "Kuşak atlanır", "Açılış/kapanış çalar", NULL};
    const gchar *const starts[] = {"Çalan parça bitince", "Kesin saatte, ses kısılır", "En fazla bekle, sonra kıs", NULL};
    field(grid, 7, "Boş kuşakta", "advert-k-empty", radios_ui_combo(empties, (gint)radios_json_int(config, "empty_mode", 0)));
    GtkWidget *mode = radios_ui_combo(starts, (gint)radios_json_int(config, "start_mode", 0));
    GtkWidget *wait = spin(5, 600, radios_json_int(config, "wait_seconds", 30));
    field(grid, 8, "Başlama", "advert-k-start", mode); field(grid, 9, "En fazla bekleme (sn)", "advert-k-wait", wait);
    g_signal_connect(mode, "changed", G_CALLBACK(start_mode_changed), wait); start_mode_changed(GTK_COMBO_BOX(mode), wait);
    field(grid, 10, "Gün", "advert-k-date", date);
    GtkWidget *output = combo("advert-k-output", "Çıkış seç");
    for (guint i = 0; i < v->output_records->len; ++i) {
        const gchar *id = radios_json_string(g_ptr_array_index(v->output_records, i), "output_id", "");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(output), id, id);
    }
    const gchar *output_id = radios_json_string(config, "output_id", gtk_combo_box_get_active_id(GTK_COMBO_BOX(v->output)));
    if (output_id) gtk_combo_box_set_active_id(GTK_COMBO_BOX(output), output_id);
    field(grid, 11, "Gerçek istasyon çıkışı", "advert-k-output", output);
    GtkWidget *region = radios_ui_entry(NULL); gtk_entry_set_text(GTK_ENTRY(region), radios_json_string(config, "region_id", "LOCAL"));
    gtk_entry_set_max_length(GTK_ENTRY(region), 100); field(grid, 12, "Bölge kodu", "advert-k-region", region);
    pack(form, grid, FALSE);
    GtkWidget *actions = box(GTK_ORIENTATION_HORIZONTAL, 7, "advert-actions");
    const gchar *pick_labels[] = {"Sabit anons seç", "Açılış seç", "Kapanış seç"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkWidget *choose = radios_ui_button(pick_labels[i]); gchar *id = g_strdup_printf("advert-k-%s-select", names[i]); named(choose, id); g_free(id);
        g_object_set_data(G_OBJECT(choose), "advert-overhead", (gpointer)names[i]); g_signal_connect(choose, "clicked", G_CALLBACK(overhead_choose), v); pack(actions, choose, FALSE);
        GtkWidget *clear = radios_ui_button("Kaldır"); id = g_strdup_printf("advert-k-%s-clear", names[i]); named(clear, id); g_free(id);
        g_object_set_data(G_OBJECT(clear), "advert-overhead", (gpointer)names[i]); g_signal_connect(clear, "clicked", G_CALLBACK(overhead_clear), v); pack(actions, clear, FALSE);
    }
    pack(form, actions, FALSE); g_signal_connect(dialog, "response", G_CALLBACK(break_response), v); gtk_widget_show_all(dialog);
}

static void spot_picked(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail, gpointer data)
{
    (void)ctx; RadioSAdvertViews *v = data;
    if (v->bind_epoch != v->campaign_epoch || !v->campaign) { status(v, "Kampanya seçimi değişti; spot bağlanmadı."); return; }
    JsonObject *media = object_member(detail, "media");
    if (!g_str_equal(radios_json_string(media, "kind", ""), "advertisement")) {
        status(v, "Reklam türündeki doğrulanmış bir ses seçin; kütüphanede türü düzenleyebilirsiniz."); return;
    }
    JsonObject *binding = binding_from_detail(detail);
    if (!binding) { status(v, "Seçilen medya bağı okunamadı."); return; }
    JsonNode *body = radios_data_body(v->ctx); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "campaign_id", radios_json_string(v->campaign, "_id", ""));
    json_object_set_int_member(object, "expected_revision", radios_json_int(v->campaign, "_rev", 0));
    const gchar *keys[] = {"media_id", "version_id", "media_file_id", "media_sha256"};
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) json_object_set_string_member(object, keys[i], radios_json_string(binding, keys[i], ""));
    json_object_unref(binding); keyed_body(owner, object);
    g_object_set_data(G_OBJECT(v->page), "advert-current-operation", "bind");
    if (request(v, owner, "bind", body, operation_done, v)) { busy(v, TRUE); status(v, "Doğrulanmış spot kampanyaya bağlanıyor…"); }
}
static gboolean break_body(RadioSAdvertViews *v, JsonObject *object)
{
    JsonObject *record = selected_break(v);
    if (!record) { status(v, "Kaydedilmiş bir kuşak seçin."); return FALSE; }
    json_object_set_string_member(object, "break_document_id", radios_json_string(record, "_id", ""));
    json_object_set_int_member(object, "expected_revision", radios_json_int(record, "_rev", 0)); return TRUE;
}
static const gchar *offer_title(RadioSAdvertViews *v, JsonObject *offer)
{
    const gchar *campaign_id=radios_json_string(offer,"campaign_id","");
    const gchar *spot_id=radios_json_string(offer,"spot_id","");
    const gchar *offer_id=radios_json_string(offer,"offer_id","");
    if(!*spot_id&&strlen(offer_id)==73&&offer_id[36]==':')spot_id=offer_id+37;
    JsonArray *campaigns=array_member(v->overview,"campaigns");
    for(guint i=0;campaigns&&i<json_array_get_length(campaigns);++i){
        JsonObject *campaign=json_array_get_object_element(campaigns,i);
        if(*campaign_id&&g_str_equal(campaign_id,radios_json_string(campaign,"_id","")))return radios_json_string(campaign,"name","Spot");
        JsonArray *spots=array_member(campaign,"spots");
        for(guint j=0;spots&&j<json_array_get_length(spots);++j)
            if(g_str_equal(spot_id,radios_json_string(json_array_get_object_element(spots,j),"_id","")))return radios_json_string(campaign,"name","Spot");
    }
    return radios_json_string(offer,"title",radios_json_string(offer,"campaign_name","Spot"));
}
static gchar *offer_reasons(JsonObject *offer)
{
    JsonArray *reasons=array_member(offer,"reasons");GString *text=g_string_new("");
    for(guint i=0;reasons&&i<json_array_get_length(reasons);++i){if(i)g_string_append(text," · ");g_string_append(text,json_array_get_string_element(reasons,i));}
    if(!text->len)g_string_append(text,radios_json_string(offer,"reason",""));
    return g_string_free(text,FALSE);
}
static void render_preview(RadioSAdvertViews *v)
{
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(v->preview_view))); gtk_list_store_clear(store);
    const gchar *sections[] = {"selected", "rejected"};
    for (guint section = 0; section < G_N_ELEMENTS(sections); ++section) {
        JsonArray *records = array_member(v->preview, sections[section]);
        for (guint i = 0; records && i < json_array_get_length(records); ++i) {
            JsonObject *record = json_array_get_object_element(records, i);
            gchar *duration = radios_json_int(record,"duration_ms",0)>0?g_strdup_printf("%.3f sn", (gdouble)radios_json_int(record, "duration_ms", 0) / 1000.0):g_strdup("—");
            gchar *reasons=offer_reasons(record);
            const gchar *values[] = {offer_title(v,record), duration,
                section == 0 ? "Seçildi" : "Yerleşemedi", reasons, radios_json_string(record, "spot_id", "")};
            row(store, values, G_N_ELEMENTS(values)); g_free(duration);g_free(reasons);
        }
    }
    gchar *message = g_strdup_printf("Önizleme: %u seçildi · %u reddedildi · kapasite %" G_GINT64_FORMAT " ms + izin verilen aşım %" G_GINT64_FORMAT " ms · açılış/anons/kapanış %" G_GINT64_FORMAT " ms · spotlar %" G_GINT64_FORMAT " ms · kalan %" G_GINT64_FORMAT " ms",
        array_member(v->preview, "selected") ? json_array_get_length(array_member(v->preview, "selected")) : 0,
        array_member(v->preview, "rejected") ? json_array_get_length(array_member(v->preview, "rejected")) : 0,
        radios_json_int(v->preview,"total_capacity_ms",0),radios_json_int(v->preview,"tolerance_ms",0),radios_json_int(v->preview,"overhead_ms",0),
        radios_json_int(v->preview, "used_ms", 0),radios_json_int(v->preview,"remaining_capacity_ms",0));
    status(v, message); g_free(message);
    const gchar *empty=radios_json_string(v->preview,"empty_action","");
    if(g_str_equal(empty,"skip")||g_str_equal(empty,"fixed_only")||g_str_equal(empty,"overhead"))status(v,g_str_equal(empty,"skip")?"Boş kuşak ilkesi: kuşak atlanacak.":g_str_equal(empty,"fixed_only")?"Boş kuşak ilkesi: yalnız doğrulanmış sabit anons çalacak.":"Boş kuşak ilkesi: doğrulanmış açılış/anons/kapanış çalacak.");
    gtk_widget_set_sensitive(v->break_reserve,preview_reservable(v));
}
static void render_report(RadioSAdvertViews *v)
{
    JsonObject *config = object_member(v->report, "config"); JsonObject *snapshot = object_member(config, "snapshot");
    if (!snapshot) snapshot = object_member(v->report, "snapshot");
    JsonObject *counts = object_member(snapshot, "counters");
    const gchar *keys[] = {"planned", "local_complete", "local_interrupted", "local_notexecuted", "local_failed", "local_unknown"};
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) if (v->counters[i]) {
        gchar *value = g_strdup_printf("%" G_GINT64_FORMAT, radios_json_int(counts, keys[i], 0));
        gtk_label_set_text(GTK_LABEL(v->counters[i]), value); g_free(value);
        gtk_widget_set_tooltip_text(v->counters[i],i==0?"Gerçek trafik yerleşimleri":"Yerel kaynak tüketimi kayıtları; dış çıkış kanıtı ayrı rapor satırıdır.");
    }
    if (v->report_view) {
        GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(v->report_view))); gtk_list_store_clear(store);
        JsonArray *records = array_member(snapshot, "rows");
        for (guint i = 0; records && i < json_array_get_length(records); ++i) {
            JsonObject *record = json_array_get_object_element(records, i);
            gchar *outcome = g_strdup_printf("%s · %s · %s", radios_json_string(record, "state", radios_json_string(record, "status", "unknown")),
                radios_json_string(record, "proof_class", ""), radios_json_string(record, "reason", ""));
            const gchar *values[] = {radios_json_string(record, "started_at", radios_json_string(record, "scheduled_at",radios_json_string(record, "time", ""))),
                radios_json_string(record, "title", radios_json_string(record, "version_id", "")), radios_json_string(record, "output_id", ""), outcome,
                radios_json_string(record, "placement_id", ""), radios_json_string(record, "attempt_id", ""), radios_json_string(record, "proof_class", "")};
            row(store, values, G_N_ELEMENTS(values)); g_free(outcome);
        }
    }
    gtk_widget_set_sensitive(v->download, v->report != NULL);
    JsonNode *next=json_object_get_member(v->report,"next_offset");
    gboolean more=next&&!JSON_NODE_HOLDS_NULL(next);
    gtk_widget_set_sensitive(v->report_previous,radios_json_int(v->report,"offset",0)>0);
    gtk_widget_set_sensitive(v->report_next,more);
    gchar *message=g_strdup_printf("Değişmez rapor · %" G_GINT64_FORMAT " toplam satır · gösterilen %" G_GINT64_FORMAT "–%" G_GINT64_FORMAT "%s · plan, yerel kaynak ve dış gözlem sınıfları ayrı",
        radios_json_int(v->report,"total",0),radios_json_int(v->report,"offset",0),
        radios_json_int(v->report,"offset",0)+(array_member(snapshot,"rows")?json_array_get_length(array_member(snapshot,"rows")):0),more?" · devamı sonraki sayfada":"");
    status(v,message);g_free(message);
}
static void operation_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    RadioSAdvertViews *v = data; busy(v, FALSE); render_campaign(v);
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        status(v, error ? error->message : "İşlem sonucu alınamadı; aynı işlemi tekrar deneyebilirsiniz."); return;
    }
    JsonObject *object = json_node_get_object(result);
    const gchar *operation = g_object_get_data(G_OBJECT(owner), "advert-current-operation");
    if (g_strcmp0(operation, "preview") == 0) { replace_object(&v->preview, object); render_preview(v); return; }
    if (g_strcmp0(operation, "report-create") == 0 || g_strcmp0(operation,"report-get")==0) { replace_object(&v->report, object);g_object_set_data(G_OBJECT(owner),"advert-idempotency-key",NULL);render_report(v);return; }
    if(g_strcmp0(operation,"campaign-state")==0&&v->campaign&&g_str_equal(radios_json_string(object,"_id",""),radios_json_string(v->campaign,"_id",""))){
        json_object_set_int_member(v->campaign,"_rev",radios_json_int(object,"_rev",0));json_object_set_string_member(v->campaign,"status",radios_json_string(object,"status",""));render_campaign(v);
    }
    if (g_strcmp0(operation, "reserve") == 0) replace_object(&v->reservation, object);
    g_object_set_data(G_OBJECT(owner), "advert-idempotency-key", NULL);
    status(v, "İşlem veritabanına kaydedildi."); radios_advert_page_refresh(ctx,v->page);
}
static void report_load(GtkButton *widget, gpointer data)
{
    (void)widget; RadioSAdvertViews *v = data;
    const gchar *from = gtk_entry_get_text(GTK_ENTRY(v->report_from)), *to = gtk_entry_get_text(GTK_ENTRY(v->report_to));
    if (!valid_date(from) || !valid_date(to) || strcmp(from, to) > 0) { status(v, "Rapor için geçerli başlangıç/bitiş tarihleri girin."); return; }
    JsonNode *body = radios_data_body(v->ctx); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "date_from", from); json_object_set_string_member(object, "date_to", to);
    const gchar *campaign_id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(v->report_campaign));
    if (campaign_id && *campaign_id) json_object_set_string_member(object, "campaign_id", campaign_id);
    const gchar *output = gtk_combo_box_get_active_id(GTK_COMBO_BOX(v->report_output));
    if (output && *output) json_object_set_string_member(object, "output_id", output);
    keyed_body(v->page, object); g_object_set_data(G_OBJECT(v->page), "advert-current-operation", "report-create");
    if (request(v, v->page, "report-create", body, operation_done, v)) { busy(v, TRUE); status(v, "Değişmez rapor hazırlanıyor…"); }
}

static void export_response(GtkDialog *dialog, gint response, gpointer data)
{
    RadioSAdvertViews *v = data;
    if (response == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        const gchar *content = g_object_get_data(G_OBJECT(dialog), "advert-export-content"); GError *error = NULL;
        if (filename && content && g_file_set_contents(filename, content, -1, &error)) status(v, "Değişmez rapor dosyaya kaydedildi.");
        else status(v, error ? error->message : "Dosya kaydedilemedi.");
        g_clear_error(&error); g_free(filename);
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
static void export_open(RadioSAdvertViews *v, const gchar *filename, const gchar *content)
{
    GtkWidget *dialog = gtk_file_chooser_dialog_new("Reklam kaydını indir", v->ctx->parent,
        GTK_FILE_CHOOSER_ACTION_SAVE, "Vazgeç", GTK_RESPONSE_CANCEL, "Kaydet", GTK_RESPONSE_ACCEPT, NULL);
    g_signal_connect_object(v->page,"destroy",G_CALLBACK(gtk_widget_destroy),dialog,G_CONNECT_SWAPPED);
    named(dialog, "advert-export-dialog"); gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(dialog), TRUE);
    radios_ui_file_chooser_default_folder(GTK_FILE_CHOOSER(dialog), v->ctx);
    /* The service names the file; keep only a valid Windows file name of it. */
    gchar *basename = g_path_get_basename(filename), *suggested = radios_ui_file_name_suggestion(basename, "RadioS-reklam.csv");
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), suggested); g_free(suggested); g_free(basename);
    g_object_set_data_full(G_OBJECT(dialog), "advert-export-content", g_strdup(content), g_free);
    g_signal_connect(dialog, "response", G_CALLBACK(export_response), v); gtk_widget_show_all(dialog);
}
static void export_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    (void)ctx; (void)owner; RadioSAdvertViews *v = data;
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { status(v, error ? error->message : "Dışa aktarma sonucu alınamadı."); return; }
    JsonObject *object = json_node_get_object(result); const gchar *content = radios_json_string(object, "content", "");
    if (!*content) { status(v, "Rapor içeriği boş; dosya oluşturulmadı."); return; }
    if(json_object_has_member(object,"partial")&&json_object_get_boolean_member(object,"partial"))status(v,"Dışa aktarma raporun seçili sayfasını içeriyor; diğer sayfaları ayrıca kaydedebilirsiniz.");
    export_open(v, radios_json_string(object, "filename", "RadioS-reklam.csv"), content);
}
typedef struct { RadioSAdvertViews *views; guint epoch; gboolean scheduled; } PrepareRequest;
static void prepared_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    PrepareRequest *job = data; RadioSAdvertViews *v = job->views;
    if (job->epoch != v->prepare_epoch) return;
    v->prepare_pending = FALSE; busy(v, FALSE); render_campaign(v);
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) { status(v, error ? error->message : "Kuşak hazırlığı alınamadı."); return; }
    JsonObject *response=json_node_get_object(result);
    if(json_object_has_member(response,"skipped")&&json_object_get_boolean_member(response,"skipped")){status(v,"Boş kuşak ilkesi uygulandı: kuşak atlandı.");return;}
    replace_object(&v->prepared, json_node_get_object(result)); GError *start_error = NULL;
    if (!radios_advert_ui_start(ctx, owner, v->prepared, job->scheduled, &start_error)) status(v, start_error ? start_error->message : "Kuşak başlatılamadı.");
    else status(v, job->scheduled ? "Kuşak gerçek saatinde başlamak üzere hazırlandı." : "Kuşak gerçek yerel MAIN motoruna hazırlanıyor.");
    g_clear_error(&start_error);
}
static void rights_toggled(GtkToggleButton *toggle,gpointer data)
{
    GtkWidget *form=data;gboolean enabled=gtk_toggle_button_get_active(toggle);
    const gchar *ids[]={"advert-spot-rights-territory","advert-spot-rights-output","advert-spot-rights-reference"};
    for(guint i=0;i<G_N_ELEMENTS(ids);++i)gtk_widget_set_sensitive(find(form,ids[i]),enabled);
}
static void spot_approve_response(GtkDialog *dialog,gint response,gpointer data)
{
    RadioSAdvertViews *v=data;
    if(response!=GTK_RESPONSE_ACCEPT){gtk_widget_destroy(GTK_WIDGET(dialog));return;}
    GtkWidget *form=g_object_get_data(G_OBJECT(dialog),"advert-form");
    JsonObject *campaign=g_object_get_data(G_OBJECT(dialog),"advert-campaign-record");
    JsonNode *body=radios_data_body(v->ctx);JsonObject *object=json_node_get_object(body);
    json_object_set_string_member(object,"campaign_id",radios_json_string(campaign,"_id",""));
    json_object_set_int_member(object,"expected_revision",radios_json_int(campaign,"_rev",0));
    gint position=gtk_combo_box_get_active(GTK_COMBO_BOX(find(form,"advert-spot-position")));
    json_object_set_string_member(object,"position",position==1?"first":position==2?"last":"any");
    JsonArray *categories=json_array_new();const gchar *category=text(form,"advert-spot-category");
    if(*category)json_array_add_string_element(categories,category);
    json_object_set_array_member(object,"competitive_categories",categories);
    if(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(form,"advert-spot-rights-enable")))){
        const gchar *territory=text(form,"advert-spot-rights-territory"),*reference=text(form,"advert-spot-rights-reference");
        const gchar *output=gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(form,"advert-spot-rights-output")));
        gchar *check=g_strdup(reference);gboolean valid=*territory&&output&&*output&&*g_strstrip(check);g_free(check);
        if(!valid){json_node_free(body);form_status(GTK_WIDGET(dialog),"Kullanım yetkisi için bölge, yapılandırılmış çıkış ve açık bir yetki referansı girin.");return;}
        JsonObject *rights=json_object_new();json_object_set_string_member(rights,"territory",territory);
        json_object_set_string_member(rights,"output_id",output);json_object_set_string_member(rights,"source_ref",reference);
        json_object_set_object_member(object,"rights",rights);
    }
    keyed_body(GTK_WIDGET(dialog),object);
    if(request(v,GTK_WIDGET(dialog),"spot-approve",body,editor_saved,v)){
        gtk_dialog_set_response_sensitive(dialog,GTK_RESPONSE_ACCEPT,FALSE);gtk_widget_set_sensitive(form,FALSE);form_status(GTK_WIDGET(dialog),"Spot ve açıkça seçilen kullanım yetkisi kaydediliyor…");
    }
}
static void spot_approve_dialog(RadioSAdvertViews *v)
{
    if(!v->campaign||!object_member(v->campaign,"binding")){status(v,"Önce kampanyaya doğrulanmış bir reklam sesi bağlayın.");return;}
    GtkWidget *form;GtkWidget *dialog=dialog_form(v,"Spot onayı ve kullanım yetkisi","advert-spot-approve-dialog",&form);
    g_object_set_data_full(G_OBJECT(dialog),"advert-campaign-record",json_object_ref(v->campaign),(GDestroyNotify)json_object_unref);
    g_object_set_data(G_OBJECT(dialog),"advert-success-message","Spot onayı ve seçilen kullanım yetkisi kaydedildi.");
    GtkWidget *grid=gtk_grid_new();gtk_grid_set_column_homogeneous(GTK_GRID(grid),TRUE);gtk_grid_set_column_spacing(GTK_GRID(grid),16);gtk_grid_set_row_spacing(GTK_GRID(grid),14);
    JsonObject *fields=object_member(v->campaign,"desktop_fields");
    JsonObject *existing_spot=NULL;JsonArray *spots=array_member(v->campaign,"spots");
    const gchar *version_id=radios_json_string(object_member(v->campaign,"binding"),"version_id","");
    for(guint i=0;spots&&i<json_array_get_length(spots);++i){JsonObject *spot=json_array_get_object_element(spots,i);
        if(g_str_equal(version_id,radios_json_string(spot,"version_id",""))&&g_str_equal(radios_json_string(spot,"status",""),"approved")){existing_spot=spot;break;}}
    gint position=(gint)radios_json_int(fields,"position",0);
    if(existing_spot){const gchar *value=radios_json_string(existing_spot,"position","any");position=g_str_equal(value,"first")?1:g_str_equal(value,"last")?2:0;}
    const gchar *const positions[]={"Herhangi","İlk","Son",NULL};
    field(grid,0,"Kuşaktaki sıra","advert-spot-position",radios_ui_combo(positions,position));
    GtkWidget *category=radios_ui_entry(NULL);const gchar *category_text=radios_json_string(fields,"competition","");
    JsonArray *existing_categories=array_member(existing_spot,"competitive_categories");
    if(existing_spot)category_text=existing_categories&&json_array_get_length(existing_categories)?json_array_get_string_element(existing_categories,0):"";
    gtk_entry_set_text(GTK_ENTRY(category),g_str_equal(category_text,"Belirtilmedi")?"":category_text);gtk_entry_set_max_length(GTK_ENTRY(category),100);
    field(grid,1,"Rakip kategorisi","advert-spot-category",category);pack(form,grid,FALSE);
    GtkWidget *enable=named(gtk_check_button_new_with_label("Bu ses için aşağıdaki çıkışta kullanım yetkisi veriyorum"),"advert-spot-rights-enable");pack(form,enable,FALSE);
    grid=gtk_grid_new();gtk_grid_set_column_homogeneous(GTK_GRID(grid),TRUE);gtk_grid_set_column_spacing(GTK_GRID(grid),16);gtk_grid_set_row_spacing(GTK_GRID(grid),14);
    GtkWidget *territory=radios_ui_entry(NULL);gtk_entry_set_text(GTK_ENTRY(territory),"LOCAL");gtk_entry_set_max_length(GTK_ENTRY(territory),100);
    field(grid,0,"Yetki bölgesi","advert-spot-rights-territory",territory);
    GtkWidget *output=combo("advert-spot-rights-output","Yapılandırılmış çıkış seç");
    for(guint i=0;i<v->output_records->len;++i){const gchar *id=radios_json_string(g_ptr_array_index(v->output_records,i),"output_id","");gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(output),id,id);}
    const gchar *chosen=gtk_combo_box_get_active_id(GTK_COMBO_BOX(v->output));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(output),chosen&&*chosen?chosen:"LOCAL_MAIN");
    field(grid,1,"Yetki verilen çıkış","advert-spot-rights-output",output);
    GtkWidget *reference=radios_ui_entry("Sözleşme, izin veya operatör yetki referansı");gtk_entry_set_max_length(GTK_ENTRY(reference),500);
    field(grid,2,"Yetki kaynağı · zorunlu","advert-spot-rights-reference",reference);pack(form,grid,FALSE);
    g_signal_connect(enable,"toggled",G_CALLBACK(rights_toggled),form);rights_toggled(GTK_TOGGLE_BUTTON(enable),form);
    g_signal_connect(dialog,"response",G_CALLBACK(spot_approve_response),v);gtk_widget_show_all(dialog);
}
static void workflow_action(GtkButton *widget, gpointer data)
{
    RadioSAdvertViews *v = data; const gchar *operation = g_object_get_data(G_OBJECT(widget), "advert-operation");
    if (!operation) return;
    if (g_str_equal(operation, "pause") || g_str_equal(operation, "resume") || g_str_equal(operation, "stop")) {
        RadioSPlayoutAction command = g_str_equal(operation, "pause") ? RADIOS_PLAYOUT_PAUSE : g_str_equal(operation, "resume") ? RADIOS_PLAYOUT_RESUME : RADIOS_PLAYOUT_STOP;
        gboolean was_prepare_pending=v->prepare_pending;
        if (command == RADIOS_PLAYOUT_STOP) { ++v->prepare_epoch; if(v->prepare_pending){v->prepare_pending=FALSE;busy(v,FALSE);render_campaign(v);} }
        if(command==RADIOS_PLAYOUT_STOP&&was_prepare_pending&&!radios_advert_ui_pending(v->ctx)){radios_advert_ui_checkpoint(v->ctx);status(v,"Bekleyen kuşak hazırlığı iptal edildi; mevcut ses devam ediyor.");return;}
        GError *error = NULL;
        if (!radios_advert_ui_action(v->ctx, command, &error)) status(v, error ? error->message : "Motor komutu uygulanamadı.");
        else if (command == RADIOS_PLAYOUT_STOP) status(v, "Kuşak durduruldu; tamamlanma iddiası üretilmedi.");
        g_clear_error(&error); return;
    }
    if (v->busy) return;
    if (g_str_equal(operation, "refresh")) { radios_advert_page_refresh(v->ctx,v->page); return; }
    if (g_str_equal(operation, "spot-bind")) {
        if (!v->campaign) { status(v, "Spot bağlamak için kampanya seçin."); return; }
        v->bind_epoch = v->campaign_epoch; radios_media_library_pick(v->ctx, v->page, spot_picked, v); return;
    }
    if(g_str_equal(operation,"spot-approve")){spot_approve_dialog(v);return;}
    if (g_str_equal(operation, "plan-export")) {
        JsonObject *record = selected_break(v);
        if (!record || !*radios_json_string(record, "plan_id", "")) { status(v, "Dışa aktarmak için gerçek planı olan bir kuşak seçin."); return; }
        JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, record);
        gchar *content = json_to_string(node, TRUE); json_node_free(node); export_open(v, "RadioS-kusak-plani.json", content); g_free(content); return;
    }
    JsonNode *body = radios_data_body(v->ctx); JsonObject *object = json_node_get_object(body); const gchar *endpoint = operation;
    if (g_str_has_prefix(operation, "campaign-")) {
        if (!v->campaign) { json_node_free(body); status(v, "Önce kampanya seçin."); return; }
        json_object_set_string_member(object, "campaign_id", radios_json_string(v->campaign, "_id", ""));
        json_object_set_int_member(object, "expected_revision", radios_json_int(v->campaign, "_rev", 0));
        endpoint = "campaign-state";
        json_object_set_string_member(object, "state", g_str_equal(operation, "campaign-suspend") ? "pause" : g_str_equal(operation, "campaign-resume") ? "resume" : "cancel");
    } else if (g_str_equal(operation, "output-create")) {
        json_object_set_string_member(object, "output_id", "LOCAL_MAIN");
    } else if (g_str_equal(operation, "notifications-read")) {
        JsonArray *ids = json_array_new(), *notifications = array_member(v->overview, "notifications");
        for (guint i = 0; notifications && i < json_array_get_length(notifications); ++i) {
            JsonObject *record = json_array_get_object_element(notifications, i);
            const gchar *id = radios_json_string(record, "id", radios_json_string(record, "_id", ""));
            if (*id) json_array_add_string_element(ids, id);
        }
        json_object_set_array_member(object, "notification_ids", ids);
    } else if (g_str_equal(operation, "report-export")) {
        if (!v->report) { json_node_free(body); status(v, "Önce değişmez bir rapor oluşturun."); return; }
        json_object_set_string_member(object, "report_id", radios_json_string(v->report, "_id", ""));
        json_object_set_string_member(object, "format", "csv");
        json_object_set_int_member(object,"offset",radios_json_int(v->report,"offset",0));
        request(v, v->page, endpoint, body, export_loaded, v); return;
    } else if(g_str_equal(operation,"report-next")||g_str_equal(operation,"report-previous")){
        if(!v->report){json_node_free(body);status(v,"Önce rapor oluşturun.");return;}
        gint64 offset=radios_json_int(v->report,"offset",0);
        if(g_str_equal(operation,"report-next")){
            JsonNode *next=json_object_get_member(v->report,"next_offset");
            if(!next||JSON_NODE_HOLDS_NULL(next)){json_node_free(body);return;}offset=json_node_get_int(next);
        }else offset=MAX((gint64)0,offset-500);
        json_object_set_string_member(object,"report_id",radios_json_string(v->report,"_id",""));
        json_object_set_int_member(object,"offset",offset);endpoint="report-get";
    } else {
        if (!break_body(v, object)) { json_node_free(body); return; }
        if (g_str_equal(operation, "break-play") || g_str_equal(operation, "break-schedule")) {
            PrepareRequest *job = g_new0(PrepareRequest, 1); job->views = v; job->epoch = ++v->prepare_epoch; job->scheduled = g_str_equal(operation, "break-schedule");
            gboolean accepted = radios_data_request(v->ctx, v->page, "POST", "/v1/workflows/desktop-advert/prepare", body, prepared_loaded, job, g_free);
            json_node_free(body);
            if (accepted) { v->prepare_pending=TRUE;busy(v, TRUE); status(v, "Doğrulanmış kuşak medyaları hazırlanıyor…"); } return;
        }
        endpoint = g_str_equal(operation, "break-preview") ? "preview" : g_str_equal(operation, "break-reserve") ? "reserve" : "cancel";
        if (g_str_equal(endpoint, "reserve")) {
            if (!v->preview) { json_node_free(body); status(v, "Rezervasyondan önce yerleşimi önizleyin."); return; }
            json_object_set_string_member(object, "expected_preview_hash", radios_json_string(v->preview, "preview_hash", ""));
            json_object_set_int_member(object, "expected_plan_revision", radios_json_int(v->preview, "plan_revision", 0));
        }
    }
    if (!g_str_equal(endpoint, "preview")) keyed_body(v->page, object);
    g_object_set_data(G_OBJECT(v->page), "advert-current-operation", (gpointer)endpoint);
    if (request(v, v->page, endpoint, body, operation_done, v)) { busy(v, TRUE); status(v, "İşlem gerçek veritabanında uygulanıyor…"); }
}
static gboolean playout_tick(gpointer data)
{
    RadioSAdvertViews *v = data; RadioSPlayoutSnapshot snapshot;
    if (radios_playout_ui_get_snapshot(v->ctx, &snapshot)) {
        gchar *message = g_strdup_printf("Yerel PCM · %s · %u/%u · %s · %.3f / %.3f sn%s%s",
            radios_playout_ui_state_name(snapshot.state), snapshot.total ? snapshot.index + 1 : 0, snapshot.total, snapshot.title,
            (gdouble)snapshot.position_ms / 1000.0, (gdouble)snapshot.duration_ms / 1000.0, *snapshot.error ? " · " : "", snapshot.error);
        gtk_label_set_text(GTK_LABEL(v->playout_status), message); g_free(message);
    }
    return G_SOURCE_CONTINUE;
}
