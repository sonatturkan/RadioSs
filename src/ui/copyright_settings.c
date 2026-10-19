#include "radios/copyright_settings.h"
#include "radios/data.h"
#include "radios/playout_ui.h"
#include <string.h>

enum { COL_MATCH, COL_START, COL_END, COL_STATE, COL_ID, N_COLUMNS };
typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *page, *restrict_on, *restrict_off, *list, *status, *duration;
    GtkWidget *artist, *album, *title, *starts, *ends, *note, *enabled;
    GtkWidget *save, *remove, *fresh;
    GtkListStore *rows;
    RadioSDocument *document;
    JsonNode *config;
    gchar *selected;
    guint timer;
    gboolean updating, form_dirty, ready_announced, disposed;
} CopyrightPage;

static void pack(GtkWidget *box, GtkWidget *child, gboolean expand)
{ gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0); }
static GtkWidget *named(GtkWidget *widget, const gchar *name)
{ gtk_widget_set_name(widget, name); return widget; }
static GtkWidget *label(const gchar *value)
{
    GtkWidget *widget = gtk_label_new(value); gtk_label_set_xalign(GTK_LABEL(widget), 0);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(widget), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(widget), 76); return widget;
}
static GtkWidget *section(GtkWidget *body, const gchar *title)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8), *heading = label(title);
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "preferences-section");
    gtk_style_context_add_class(gtk_widget_get_style_context(heading), "preferences-heading");
    pack(box, heading, FALSE); pack(body, box, FALSE); return box;
}
static void status(CopyrightPage *page, const gchar *message)
{ gtk_label_set_text(GTK_LABEL(page->status), message); }
static JsonObject *object(CopyrightPage *page) { return json_node_get_object(page->config); }
static JsonArray *rules(CopyrightPage *page) { return json_object_get_array_member(object(page), "rules"); }
static JsonNode *copy_json(JsonNode *source)
{
    if (JSON_NODE_HOLDS_OBJECT(source)) {
        JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *value = json_object_new(), *original = json_node_get_object(source);
        json_node_take_object(node, value); GList *keys = json_object_get_members(original);
        for (GList *key = keys; key; key = key->next) json_object_set_member(value, key->data, copy_json(json_object_get_member(original, key->data)));
        g_list_free(keys); return node;
    }
    if (JSON_NODE_HOLDS_ARRAY(source)) {
        JsonNode *node = json_node_new(JSON_NODE_ARRAY); JsonArray *value = json_array_new(), *original = json_node_get_array(source);
        json_node_take_array(node, value);
        for (guint i = 0; i < json_array_get_length(original); ++i) json_array_add_element(value, copy_json(json_array_get_element(original, i)));
        return node;
    }
    return json_node_copy(source);
}
static gboolean boolean(JsonObject *value, const gchar *key, gboolean fallback)
{
    JsonNode *node = value ? json_object_get_member(value, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN ?
        json_node_get_boolean(node) : fallback;
}
static JsonNode *default_config(void)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *value = json_object_new(); json_node_take_object(node, value);
    json_object_set_int_member(value, "schema", 1); json_object_set_boolean_member(value, "restrict", FALSE);
    json_object_set_string_member(value, "unregistered", "allow"); json_object_set_array_member(value, "rules", json_array_new()); return node;
}
static GDateTime *parse_date(const gchar *value)
{
    gchar *copy = g_strdup(value); g_strstrip(copy);
    if (strlen(copy) > 10 && copy[10] == ' ') copy[10] = 'T';
    if (strlen(copy) >= 16 && copy[13] == ':' && (!copy[16] || copy[16] == 'Z' || copy[16] == '+' || copy[16] == '-')) {
        gchar *seconds = g_strdup_printf("%.16s:00%s", copy, copy + 16); g_free(copy); copy = seconds;
    }
    GTimeZone *zone = g_time_zone_new_local(); GDateTime *date = g_date_time_new_from_iso8601(copy, zone);
    g_time_zone_unref(zone); g_free(copy); return date;
}
static gchar *utc_date(GDateTime *date)
{
    GDateTime *utc = g_date_time_to_utc(date); gchar *value = g_date_time_format(utc, "%Y-%m-%dT%H:%M:%SZ");
    g_date_time_unref(utc); return value;
}
static gchar *local_date(const gchar *value, gboolean brief)
{
    GDateTime *date = g_date_time_new_from_iso8601(value, NULL); if (!date) return g_strdup(value);
    GDateTime *local = g_date_time_to_local(date); gchar *text = g_date_time_format(local, brief ? "%d.%m.%Y %H:%M" : "%Y-%m-%d %H:%M:%S");
    g_date_time_unref(local); g_date_time_unref(date); return text;
}
static const gchar *rule_state(JsonObject *rule)
{
    if (!boolean(rule, "enabled", TRUE)) return "Devre dışı";
    GDateTime *start = g_date_time_new_from_iso8601(radios_json_string(rule, "starts_at", ""), NULL);
    GDateTime *end = g_date_time_new_from_iso8601(radios_json_string(rule, "ends_at", ""), NULL), *now = g_date_time_new_now_utc();
    const gchar *state = !start || !end ? "Tarih hatalı" : g_date_time_compare(now, start) < 0 ? "Başlamadı" :
        g_date_time_compare(now, end) >= 0 ? "Süresi doldu" : "Geçerli";
    if (start) g_date_time_unref(start);
    if (end) g_date_time_unref(end);
    g_date_time_unref(now); return state;
}
static gchar *rule_match(JsonObject *rule)
{
    const gchar *names[] = {"artist", "album", "title"}, *labels[] = {"Sanatçı", "Albüm", "Parça"};
    GString *value = g_string_new(NULL);
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        const gchar *field = radios_json_string(rule, names[i], ""); if (!*field) continue;
        if (value->len) g_string_append(value, " · ");
        g_string_append_printf(value, "%s: %s", labels[i], field);
    }
    return g_string_free(value, FALSE);
}
static JsonObject *selected_rule(CopyrightPage *page, guint *index)
{
    JsonArray *list = rules(page);
    for (guint i = 0; i < json_array_get_length(list); ++i) {
        JsonObject *rule = json_array_get_object_element(list, i);
        if (!g_strcmp0(radios_json_string(rule, "id", ""), page->selected)) { if (index) *index = i; return rule; }
    }
    return NULL;
}
static void duration_update(CopyrightPage *page)
{
    GDateTime *start = parse_date(gtk_entry_get_text(GTK_ENTRY(page->starts))), *end = parse_date(gtk_entry_get_text(GTK_ENTRY(page->ends)));
    gchar *value = NULL;
    if (!start || !end || g_date_time_compare(end, start) <= 0) value = g_strdup("Bitiş tarihi başlangıçtan sonra olmalı.");
    else {
        gint64 seconds = g_date_time_difference(end, start) / G_TIME_SPAN_SECOND;
        value = g_strdup_printf("Geçerlilik süresi: %" G_GINT64_FORMAT " gün %" G_GINT64_FORMAT " saat %" G_GINT64_FORMAT " dakika",
            seconds / 86400, (seconds / 3600) % 24, (seconds / 60) % 60);
    }
    gtk_label_set_text(GTK_LABEL(page->duration), value); g_free(value);
    if (start) g_date_time_unref(start);
    if (end) g_date_time_unref(end);
}
static void sensitivity(CopyrightPage *page)
{
    gboolean ready = radios_data_document_ready(page->document);
    gtk_widget_set_sensitive(page->restrict_on, ready); gtk_widget_set_sensitive(page->restrict_off, ready);
    gtk_widget_set_sensitive(page->save, ready); gtk_widget_set_sensitive(page->fresh, ready);
    gtk_widget_set_sensitive(page->remove, ready && selected_rule(page, NULL));
}
static void form_set(CopyrightPage *page, JsonObject *rule)
{
    page->updating = TRUE;
    gtk_entry_set_text(GTK_ENTRY(page->artist), radios_json_string(rule, "artist", ""));
    gtk_entry_set_text(GTK_ENTRY(page->album), radios_json_string(rule, "album", ""));
    gtk_entry_set_text(GTK_ENTRY(page->title), radios_json_string(rule, "title", ""));
    gtk_entry_set_text(GTK_ENTRY(page->note), radios_json_string(rule, "note", ""));
    gchar *start = NULL, *end = NULL;
    if (rule) { start = local_date(radios_json_string(rule, "starts_at", ""), FALSE); end = local_date(radios_json_string(rule, "ends_at", ""), FALSE); }
    else {
        GDateTime *now = g_date_time_new_now_local(), *later = g_date_time_add_years(now, 1);
        start = g_date_time_format(now, "%Y-%m-%d %H:%M:%S"); end = g_date_time_format(later, "%Y-%m-%d %H:%M:%S");
        g_date_time_unref(now); g_date_time_unref(later);
    }
    gtk_entry_set_text(GTK_ENTRY(page->starts), start); gtk_entry_set_text(GTK_ENTRY(page->ends), end); g_free(start); g_free(end);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->enabled), boolean(rule, "enabled", TRUE));
    gtk_button_set_label(GTK_BUTTON(page->save), rule ? "Değişiklikleri kaydet" : "Telif kaydı ekle");
    page->form_dirty = FALSE; page->updating = FALSE; duration_update(page); sensitivity(page);
}
static void rows_refresh(CopyrightPage *page)
{
    page->updating = TRUE; gtk_list_store_clear(page->rows); JsonArray *list = rules(page);
    for (guint i = 0; i < json_array_get_length(list); ++i) {
        JsonObject *rule = json_array_get_object_element(list, i); GtkTreeIter iter;
        gchar *match = rule_match(rule), *start = local_date(radios_json_string(rule, "starts_at", ""), TRUE), *end = local_date(radios_json_string(rule, "ends_at", ""), TRUE);
        const gchar *id = radios_json_string(rule, "id", ""); gtk_list_store_append(page->rows, &iter);
        gtk_list_store_set(page->rows, &iter, COL_MATCH, match, COL_START, start, COL_END, end, COL_STATE, rule_state(rule), COL_ID, id, -1);
        if (!g_strcmp0(id, page->selected)) gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(page->list)), &iter);
        g_free(match); g_free(start); g_free(end);
    }
    page->updating = FALSE;
}
static void loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data)
{
    (void)ctx; (void)owner; CopyrightPage *page = data; if (page->disposed) return;
    page->ready_announced = TRUE;
    JsonNode *rule_node = config ? json_object_get_member(config, "rules") : NULL;
    if (!rule_node || !JSON_NODE_HOLDS_ARRAY(rule_node)) { status(page, "Telif kayıtları okunamadı."); return; }
    JsonNode *next = json_node_new(JSON_NODE_OBJECT); json_node_set_object(next, config);
    g_clear_pointer(&page->config, json_node_free); page->config = copy_json(next); json_node_free(next);
    radios_playout_ui_copyright_policy_record(page->ctx, config,
        radios_data_document_id(page->document), radios_data_document_revision(page->document));
    page->updating = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(boolean(config, "restrict", FALSE) ? page->restrict_on : page->restrict_off), TRUE);
    page->updating = FALSE; rows_refresh(page);
    if (!page->form_dirty) form_set(page, selected_rule(page, NULL));
    sensitivity(page); status(page, "Telif bilgileri veritabanından yüklendi.");
}
static void document_status(RadioSDocument *document, const gchar *message, gpointer data)
{
    (void)document; CopyrightPage *page = data; if (!page->disposed) { status(page, message); sensitivity(page); }
}
static void persist(CopyrightPage *page)
{
    json_object_set_string_member(object(page), "unregistered", "allow");
    radios_data_document_dirty(page->document, TRUE);
    if (!radios_data_document_autosave(page->document, page->config))
        status(page, "Değişiklik korundu; veritabanına kaydedilmesi için bağlantı bekleniyor.");
}
static void restriction_changed(GtkToggleButton *button, gpointer data)
{
    CopyrightPage *page = data; if (page->updating || !gtk_toggle_button_get_active(button) || !radios_data_document_ready(page->document)) return;
    json_object_set_boolean_member(object(page), "restrict", GTK_WIDGET(button) == page->restrict_on); persist(page);
}
static void form_changed(GtkWidget *widget, gpointer data)
{ (void)widget; CopyrightPage *page = data; if (!page->updating) page->form_dirty = TRUE; duration_update(page); }
static void row_selected(GtkTreeSelection *selection, gpointer data)
{
    CopyrightPage *page = data; if (page->updating) return; GtkTreeModel *model = NULL; GtkTreeIter iter;
    g_clear_pointer(&page->selected, g_free);
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) gtk_tree_model_get(model, &iter, COL_ID, &page->selected, -1);
    form_set(page, selected_rule(page, NULL));
}
static void fresh_clicked(GtkButton *button, gpointer data)
{
    (void)button; CopyrightPage *page = data; page->updating = TRUE;
    gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(page->list)));
    page->updating = FALSE; g_clear_pointer(&page->selected, g_free); form_set(page, NULL); gtk_widget_grab_focus(page->artist);
}
static void save_clicked(GtkButton *button, gpointer data)
{
    (void)button; CopyrightPage *page = data; if (!radios_data_document_ready(page->document)) return;
    GtkWidget *fields[] = {page->artist, page->album, page->title}; const gchar *keys[] = {"artist", "album", "title"};
    gchar *selectors[3]; gboolean any = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) { selectors[i] = g_strdup(gtk_entry_get_text(GTK_ENTRY(fields[i]))); g_strstrip(selectors[i]); any |= *selectors[i] != 0; }
    if (!any) { status(page, "En az bir sanatçı, albüm veya parça adı gir."); goto done; }
    GDateTime *start = parse_date(gtk_entry_get_text(GTK_ENTRY(page->starts))), *end = parse_date(gtk_entry_get_text(GTK_ENTRY(page->ends)));
    if (!start || !end || g_date_time_compare(end, start) <= 0) {
        status(page, "Tarihleri YYYY-AA-GG SS:DD biçiminde gir; bitiş başlangıçtan sonra olmalı.");
        if (start) g_date_time_unref(start);
        if (end) g_date_time_unref(end);
        goto done;
    }
    guint index = 0; JsonObject *previous = selected_rule(page, &index); JsonArray *list = rules(page);
    if (!previous && json_array_get_length(list) >= 1000) {
        status(page, "En fazla 1000 telif kaydı saklanabilir."); g_date_time_unref(start); g_date_time_unref(end); goto done;
    }
    JsonObject *rule = json_object_new(); gchar *identifier = previous ? g_strdup(radios_json_string(previous, "id", "")) : g_uuid_string_random();
    json_object_set_string_member(rule, "id", identifier);
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) json_object_set_string_member(rule, keys[i], selectors[i]);
    gchar *start_text = utc_date(start), *end_text = utc_date(end);
    json_object_set_string_member(rule, "starts_at", start_text); json_object_set_string_member(rule, "ends_at", end_text);
    json_object_set_string_member(rule, "note", gtk_entry_get_text(GTK_ENTRY(page->note)));
    json_object_set_boolean_member(rule, "enabled", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(page->enabled)));
    /* Rebuild rather than mutate the selected object: the acknowledged form
     * and autosave's immutable uncertain first intent retain their own JSON. */
    JsonArray *next = json_array_new();
    for (guint i = 0; i < json_array_get_length(list); ++i) {
        if (previous && i == index) json_array_add_object_element(next, json_object_ref(rule));
        else json_array_add_element(next, json_node_copy(json_array_get_element(list, i)));
    }
    if (!previous) json_array_add_object_element(next, json_object_ref(rule));
    json_object_set_array_member(object(page), "rules", next);
    g_free(page->selected); page->selected = identifier; page->form_dirty = FALSE;
    rows_refresh(page); form_set(page, rule); persist(page);
    json_object_unref(rule); g_free(start_text); g_free(end_text); g_date_time_unref(start); g_date_time_unref(end);
done:
    for (guint i = 0; i < G_N_ELEMENTS(selectors); ++i) g_free(selectors[i]);
}
static void remove_clicked(GtkButton *button, gpointer data)
{
    (void)button; CopyrightPage *page = data; guint index = 0;
    if (!radios_data_document_ready(page->document) || !selected_rule(page, &index)) return;
    json_array_remove_element(rules(page), index); g_clear_pointer(&page->selected, g_free);
    rows_refresh(page); form_set(page, NULL); persist(page);
}
static gboolean tick(gpointer data)
{
    CopyrightPage *page = data; if (page->disposed) return G_SOURCE_REMOVE; sensitivity(page);
    GtkTreeIter iter; GtkTreeModel *model = GTK_TREE_MODEL(page->rows); JsonArray *list = rules(page); guint i = 0;
    for (gboolean valid = gtk_tree_model_get_iter_first(model, &iter); valid && i < json_array_get_length(list); valid = gtk_tree_model_iter_next(model, &iter), ++i)
        gtk_list_store_set(page->rows, &iter, COL_STATE, rule_state(json_array_get_object_element(list, i)), -1);
    if (!radios_data_document_ready(page->document)) status(page, page->ctx->data ? "Telif bilgileri yükleniyor; veritabanı bağlantısı bekleniyor." : "Telif bilgileri için veritabanı bağlantısı gerekiyor.");
    else if (!page->ready_announced) {
        page->ready_announced = TRUE; status(page, "Henüz telif kaydı yok. Yeni kayıt ekleyebilirsin.");
    }
    else if (!radios_data_connected(page->ctx) && radios_data_document_revision(page->document)) status(page, "Veritabanı bağlantısı yok; kayıtlar son yüklenen sürümden gösteriliyor.");
    return G_SOURCE_CONTINUE;
}
static void destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget; CopyrightPage *page = data; page->disposed = TRUE;
    if (page->timer) { g_source_remove(page->timer); page->timer = 0; }
    if (page->document) radios_data_document_preserve_pending(page->document);
}
static void page_free(gpointer data)
{
    CopyrightPage *page = data; if (page->timer) g_source_remove(page->timer);
    g_clear_pointer(&page->config, json_node_free); g_free(page->selected); g_object_unref(page->rows); g_free(page);
}
static GtkWidget *field(GtkGrid *grid, guint column, guint row, const gchar *caption, const gchar *name, const gchar *placeholder, gint length)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4), *entry = named(gtk_entry_new(), name);
    gtk_widget_set_hexpand(box, TRUE); gtk_entry_set_width_chars(GTK_ENTRY(entry), 16);
    gtk_entry_set_max_length(GTK_ENTRY(entry), length); gtk_entry_set_placeholder_text(GTK_ENTRY(entry), placeholder);
    atk_object_set_name(gtk_widget_get_accessible(entry), caption); pack(box, label(caption), FALSE); pack(box, entry, FALSE);
    gtk_grid_attach(grid, box, column, row, 1, 1); return entry;
}
GtkWidget *radios_copyright_settings_new(RadioSViewContext *ctx)
{
    g_return_val_if_fail(ctx, NULL); CopyrightPage *page = g_new0(CopyrightPage, 1); page->ctx = ctx; page->config = default_config();
    page->page = named(gtk_scrolled_window_new(NULL, NULL), "preferences-copyright-policy");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(page->page), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_container_set_border_width(GTK_CONTAINER(body), 12);
    gtk_container_add(GTK_CONTAINER(page->page), body);
    GtkWidget *policy = section(body, "Telif bilgileri");
    pack(policy, label("Otomasyon ve playlistlerde telif tarihlerini uygula"), FALSE);
    GtkWidget *mode = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
    page->restrict_on = named(gtk_radio_button_new_with_label(NULL, "Kısıtla"), "copyright-policy-restrict");
    page->restrict_off = named(gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(page->restrict_on), "Kısıtlama"), "copyright-policy-unrestricted");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->restrict_off), TRUE);
    pack(mode, page->restrict_on, FALSE); pack(mode, page->restrict_off, FALSE); pack(policy, mode, FALSE);
    GtkWidget *records = section(body, "Kayıtlar");
    page->rows = gtk_list_store_new(N_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    page->list = named(gtk_tree_view_new_with_model(GTK_TREE_MODEL(page->rows)), "copyright-policy-rules");
    const gchar *captions[] = {"Sanatçı / albüm / parça", "Başlangıç", "Bitiş", "Durum"};
    for (guint i = 0; i < G_N_ELEMENTS(captions); ++i) {
        GtkCellRenderer *cell = gtk_cell_renderer_text_new(); g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, NULL);
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(captions[i], cell, "text", i, NULL);
        gtk_tree_view_column_set_expand(column, i == COL_MATCH); gtk_tree_view_column_set_min_width(column, i == COL_MATCH ? 130 : 80);
        gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED); gtk_tree_view_column_set_fixed_width(column, i == COL_MATCH ? 220 : i == COL_STATE ? 95 : 135);
        gtk_tree_view_append_column(GTK_TREE_VIEW(page->list), column);
    }
    GtkWidget *table = gtk_scrolled_window_new(NULL, NULL); gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(table), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(table, -1, 145); gtk_container_add(GTK_CONTAINER(table), page->list); pack(records, table, FALSE);
    GtkWidget *buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    page->fresh = named(gtk_button_new_with_label("Yeni kayıt"), "copyright-policy-new"); page->remove = named(gtk_button_new_with_label("Seçili kaydı sil"), "copyright-policy-remove");
    pack(buttons, page->fresh, FALSE); pack(buttons, page->remove, FALSE); pack(records, buttons, FALSE);
    GtkWidget *editor = section(body, "Telif kaydı");
    GtkWidget *metadata = gtk_grid_new(); gtk_grid_set_column_spacing(GTK_GRID(metadata), 10); gtk_grid_set_row_spacing(GTK_GRID(metadata), 8);
    page->artist = field(GTK_GRID(metadata), 0, 0, "Sanatçı", "copyright-policy-artist", "Sanatçı adı", 200);
    page->album = field(GTK_GRID(metadata), 1, 0, "Albüm", "copyright-policy-album", "Albüm adı", 200);
    page->title = field(GTK_GRID(metadata), 2, 0, "Parça", "copyright-policy-title", "Parça adı", 200); pack(editor, metadata, FALSE);
    GtkWidget *dates = gtk_grid_new(); gtk_grid_set_column_spacing(GTK_GRID(dates), 10); gtk_grid_set_row_spacing(GTK_GRID(dates), 8);
    page->starts = field(GTK_GRID(dates), 0, 0, "Geçerlilik başlangıcı", "copyright-policy-starts", "2026-10-13 09:00", 40);
    page->ends = field(GTK_GRID(dates), 1, 0, "Geçerlilik bitişi", "copyright-policy-ends", "2027-10-13 09:00", 40); pack(editor, dates, FALSE);
    page->duration = named(label(""), "copyright-policy-duration"); pack(editor, page->duration, FALSE);
    page->note = named(gtk_entry_new(), "copyright-policy-note"); gtk_entry_set_max_length(GTK_ENTRY(page->note), 2000); gtk_entry_set_placeholder_text(GTK_ENTRY(page->note), "Not / izin dayanağı (isteğe bağlı)");
    atk_object_set_name(gtk_widget_get_accessible(page->note), "Not / izin dayanağı"); pack(editor, page->note, FALSE);
    page->enabled = named(gtk_check_button_new_with_label("Bu telif kaydı etkin"), "copyright-policy-enabled"); pack(editor, page->enabled, FALSE);
    page->save = named(gtk_button_new_with_label("Telif kaydı ekle"), "copyright-policy-save"); gtk_widget_set_halign(page->save, GTK_ALIGN_START); pack(editor, page->save, FALSE);
    page->status = named(label("Telif bilgileri yükleniyor…"), "copyright-policy-status"); pack(body, page->status, FALSE);
    g_object_set_data_full(G_OBJECT(page->page), "radios-copyright-policy", page, page_free); g_signal_connect(page->page, "destroy", G_CALLBACK(destroyed), page);
    g_signal_connect(page->restrict_on, "toggled", G_CALLBACK(restriction_changed), page); g_signal_connect(page->restrict_off, "toggled", G_CALLBACK(restriction_changed), page);
    GtkWidget *inputs[] = {page->artist, page->album, page->title, page->starts, page->ends, page->note};
    for (guint i = 0; i < G_N_ELEMENTS(inputs); ++i) g_signal_connect(inputs[i], "changed", G_CALLBACK(form_changed), page);
    g_signal_connect(page->enabled, "toggled", G_CALLBACK(form_changed), page);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(page->list)), "changed", G_CALLBACK(row_selected), page);
    g_signal_connect(page->fresh, "clicked", G_CALLBACK(fresh_clicked), page); g_signal_connect(page->save, "clicked", G_CALLBACK(save_clicked), page);
    g_signal_connect(page->remove, "clicked", G_CALLBACK(remove_clicked), page);
    page->document = radios_data_document_new(ctx, page->page, "copyright_policy", "main", loaded, page);
    radios_data_document_set_status_callback(page->document, document_status, page); form_set(page, NULL);
    page->timer = g_timeout_add_seconds(1, tick, page); sensitivity(page); tick(page); return page->page;
}
