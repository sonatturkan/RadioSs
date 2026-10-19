#include "radios/transfer_ui.h"
#include "radios/data.h"
#include "radios/preferences.h"
#include <glib/gstdio.h>
#include <string.h>

/* Ayarlar › Export / Import pages against the private fake transfer service
 * that tests/run_transfer_ui.py starts (docs/kilavuz-2026-10-18-D.md). */
static RadioSConfig config;
static gchar *last_notice;
static guint refreshes;
typedef struct { RadioSViewContext ctx; GtkWidget *window, *page; } Harness;
typedef gboolean (*Condition)(gpointer);
typedef struct { GtkWidget *label; const gchar *needle; } Needle;

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *required(GtkWidget *root, const gchar *name)
{ GtkWidget *widget = find(root, name); if (!widget) g_error("Widget bulunamadı: %s", name); return widget; }
static void pump(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void run_for(guint milliseconds)
{ gint64 end = g_get_monotonic_time() + (gint64)milliseconds * 1000; while (g_get_monotonic_time() < end) pump(); }
static void wait_for(Condition condition, gpointer value, const gchar *description)
{
    gint64 end = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;
    while (!condition(value) && g_get_monotonic_time() < end) pump();
    if (!condition(value)) g_error("Aktarım arayüzü zaman aşımı: %s (%s)", description, last_notice ? last_notice : "bildirim yok");
}
static void notice(gpointer unused, const gchar *message)
{ (void)unused; g_free(last_notice); last_notice = g_strdup(message); }
static void counted_refresh(RadioSViewContext *ctx, GtkWidget *owner) { (void)ctx; (void)owner; ++refreshes; }
static gboolean connected(gpointer value) { return radios_data_connected(&((Harness *)value)->ctx); }
static gboolean sensitive(gpointer value) { return gtk_widget_is_sensitive(value); }
static gboolean shown(gpointer value) { return gtk_widget_is_visible(value) && gtk_widget_get_mapped(value); }
static gboolean hidden(gpointer value) { return !gtk_widget_get_visible(value); }
static gboolean label_has(gpointer value)
{ Needle *needle = value; return strstr(gtk_label_get_text(GTK_LABEL(needle->label)), needle->needle) != NULL; }
static gboolean refreshed(gpointer unused) { (void)unused; return refreshes > 0; }
static Harness *harness_open(gboolean connect, GtkWidget *(*builder)(RadioSViewContext *))
{
    Harness *h = g_new0(Harness, 1); g_clear_pointer(&last_notice, g_free);
    h->ctx.config = &config; h->ctx.notify = notice; h->ctx.runtime = radios_runtime_new(32, 128);
    h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); h->ctx.parent = GTK_WINDOW(h->window);
    gtk_window_set_default_size(GTK_WINDOW(h->window), 900, 760);
    if (connect) h->ctx.data = radios_data_new(&h->ctx);
    h->page = builder(&h->ctx); g_assert_nonnull(h->page);
    gtk_container_add(GTK_CONTAINER(h->window), h->page); gtk_widget_show_all(h->window);
    if (connect) { radios_data_connect(&h->ctx); wait_for(connected, h, "sahte aktarım servisi bağlantısı"); }
    return h;
}
static void harness_close(Harness *h)
{
    gtk_widget_destroy(h->window);
    if (h->ctx.data) radios_data_close(h->ctx.data);
    radios_runtime_free(h->ctx.runtime);
    if (h->ctx.data) radios_data_unref(h->ctx.data);
    run_for(40); g_free(h);
}
static void click(GtkWidget *root, const gchar *name)
{
    GtkWidget *button = required(root, name); g_assert_true(GTK_IS_BUTTON(button));
    g_assert_true(gtk_widget_is_sensitive(button)); gtk_button_clicked(GTK_BUTTON(button));
}
static void toggle(GtkWidget *root, const gchar *name, gboolean state)
{ GtkWidget *button = required(root, name); g_assert_true(GTK_IS_TOGGLE_BUTTON(button)); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), state); }
static JsonObject *recorded(const gchar *operation)
{
    gchar *name = g_strdup_printf("fake-%s.json", operation), *path = g_build_filename(config.project_root, name, NULL);
    JsonParser *parser = json_parser_new(); GError *error = NULL;
    g_assert_true(json_parser_load_from_file(parser, path, &error)); g_assert_no_error(error);
    JsonObject *body = json_object_ref(json_node_get_object(json_parser_get_root(parser)));
    g_object_unref(parser); g_free(path); g_free(name); return body;
}
static void write_file(const gchar *name)
{
    gchar *path = g_build_filename(config.project_root, name, NULL);
    g_assert_true(g_file_set_contents(path, "PK\x03\x04 sahte paket", -1, NULL)); g_free(path);
}
static gboolean export_loaded(gpointer value)
{ Harness *h = value; return find(h->page, "transfer-export-group-programme") != NULL && gtk_widget_is_sensitive(required(h->page, "transfer-export-run")); }
static gboolean import_inspected(gpointer value)
{ Harness *h = value; return shown(required(h->page, "transfer-import-summary")) && find(h->page, "transfer-import-group-programme") != NULL; }

static void export_widgets_offline(void)
{
    Harness *h = harness_open(FALSE, radios_transfer_export_page_new);
    GtkWidget *box = required(h->page, "preferences-export-page"); g_assert_true(GTK_IS_BOX(box));
    GList *children = gtk_container_get_children(GTK_CONTAINER(box));
    g_assert_true(GTK_IS_LABEL(children->data));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(children->data)), ==, "Export");
    g_assert_true(gtk_style_context_has_class(gtk_widget_get_style_context(children->data), "preferences-heading"));
    g_list_free(children);
    g_assert_true(GTK_IS_RADIO_BUTTON(required(h->page, "transfer-export-scope-all")));
    g_assert_true(GTK_IS_RADIO_BUTTON(required(h->page, "transfer-export-scope-selected")));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(required(h->page, "transfer-export-scope-all"))));
    g_assert_true(GTK_IS_BOX(required(h->page, "transfer-export-groups")));
    g_assert_true(GTK_IS_CHECK_BUTTON(required(h->page, "transfer-export-media")));
    GtkWidget *destination = required(h->page, "transfer-export-destination");
    g_assert_true(GTK_IS_FILE_CHOOSER_BUTTON(destination));
    g_assert_cmpint(gtk_file_chooser_get_action(GTK_FILE_CHOOSER(destination)), ==, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    gchar *folder = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(destination));
    g_assert_cmpstr(folder, ==, config.backup_root); g_free(folder);
    GtkWidget *name = required(h->page, "transfer-export-name"); g_assert_true(GTK_IS_ENTRY(name));
    g_assert_true(g_str_has_prefix(gtk_entry_get_text(GTK_ENTRY(name)), "RadioS-"));
    g_assert_true(g_str_has_suffix(gtk_entry_get_text(GTK_ENTRY(name)), ".st"));
    g_assert_true(GTK_IS_BUTTON(required(h->page, "transfer-export-run")));
    g_assert_true(GTK_IS_BUTTON(required(h->page, "transfer-export-cancel")));
    g_assert_true(GTK_IS_PROGRESS_BAR(required(h->page, "transfer-export-progress")));
    GtkWidget *status = required(h->page, "transfer-export-status"); g_assert_true(GTK_IS_LABEL(status));
    g_assert_true(gtk_style_context_has_class(gtk_widget_get_style_context(status), "preferences-status"));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(status)), ==, "Veri servisi bağlı değil.");
    const gchar *passive[] = {"transfer-export-scope-all", "transfer-export-scope-selected", "transfer-export-media",
        "transfer-export-destination", "transfer-export-name", "transfer-export-run"};
    for (guint i = 0; i < G_N_ELEMENTS(passive); ++i) g_assert_false(gtk_widget_get_sensitive(required(h->page, passive[i])));
    g_assert_false(gtk_widget_get_visible(required(h->page, "transfer-export-groups")));
    g_assert_false(gtk_widget_get_visible(required(h->page, "transfer-export-cancel")));
    g_assert_false(gtk_widget_get_visible(required(h->page, "transfer-export-progress")));
    harness_close(h);
}
static void import_widgets_offline(void)
{
    Harness *h = harness_open(FALSE, radios_transfer_import_page_new);
    GtkWidget *box = required(h->page, "preferences-import-page"); g_assert_true(GTK_IS_BOX(box));
    GList *children = gtk_container_get_children(GTK_CONTAINER(box));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(children->data)), ==, "Import"); g_list_free(children);
    GtkWidget *package = required(h->page, "transfer-import-package");
    g_assert_true(GTK_IS_FILE_CHOOSER_BUTTON(package));
    g_assert_cmpint(gtk_file_chooser_get_action(GTK_FILE_CHOOSER(package)), ==, GTK_FILE_CHOOSER_ACTION_OPEN);
    GSList *filters = gtk_file_chooser_list_filters(GTK_FILE_CHOOSER(package));
    g_assert_cmpuint(g_slist_length(filters), ==, 1);
    g_assert_nonnull(strstr(gtk_file_filter_get_name(filters->data), "*.st")); g_slist_free(filters);
    g_assert_true(GTK_IS_BOX(required(h->page, "transfer-import-summary")));
    g_assert_true(GTK_IS_CHECK_BUTTON(required(h->page, "transfer-import-settings")));
    g_assert_true(GTK_IS_CHECK_BUTTON(required(h->page, "transfer-import-media")));
    GtkWidget *root = required(h->page, "transfer-import-media-root");
    g_assert_true(GTK_IS_FILE_CHOOSER_BUTTON(root));
    g_assert_cmpint(gtk_file_chooser_get_action(GTK_FILE_CHOOSER(root)), ==, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER);
    g_assert_true(GTK_IS_BUTTON(required(h->page, "transfer-import-run")));
    g_assert_true(GTK_IS_BUTTON(required(h->page, "transfer-import-cancel")));
    g_assert_true(GTK_IS_PROGRESS_BAR(required(h->page, "transfer-import-progress")));
    GtkWidget *status = required(h->page, "transfer-import-status");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(status)), ==, "Veri servisi bağlı değil.");
    g_assert_false(gtk_widget_get_sensitive(package));
    g_assert_false(gtk_widget_get_sensitive(required(h->page, "transfer-import-run")));
    const gchar *concealed[] = {"transfer-import-summary", "transfer-import-settings", "transfer-import-media",
        "transfer-import-media-root-row", "transfer-import-cancel", "transfer-import-progress"};
    for (guint i = 0; i < G_N_ELEMENTS(concealed); ++i) g_assert_false(gtk_widget_is_visible(required(h->page, concealed[i])));
    harness_close(h);
}
static void export_flow(void)
{
    Harness *h = harness_open(TRUE, radios_transfer_export_page_new);
    wait_for(export_loaded, h, "ayar grupları ve medya sayıları");
    GtkWidget *audio = required(h->page, "transfer-export-group-audio_preferences"), *programme = required(h->page, "transfer-export-group-programme");
    g_assert_true(GTK_IS_CHECK_BUTTON(audio)); g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(audio)));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(audio)), ==, "Ses aygıtı ve ses yüksekliği ayarları (3)");
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(programme)), ==, "Programlar (12)");
    GtkWidget *media = required(h->page, "transfer-export-media");
    g_assert_true(g_str_has_prefix(gtk_button_get_label(GTK_BUTTON(media)), "Medya (7 şarkı, "));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(media)));
    GtkWidget *name = required(h->page, "transfer-export-name");
    g_assert_true(g_str_has_prefix(gtk_entry_get_text(GTK_ENTRY(name)), "RadioS-Test-Radyo-"));
    g_assert_true(g_str_has_suffix(gtk_entry_get_text(GTK_ENTRY(name)), ".st"));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(required(h->page, "transfer-export-status"))), ==, "");
    /* Normal form: the group list appears only for the selected scope. */
    GtkWidget *groups = required(h->page, "transfer-export-groups");
    g_assert_false(gtk_widget_get_visible(groups));
    toggle(h->page, "transfer-export-scope-selected", TRUE); wait_for(shown, groups, "seçili kapsamda grup listesi");
    toggle(h->page, "transfer-export-scope-all", TRUE); wait_for(hidden, groups, "tüm kapsamda grup listesi gizli");
    toggle(h->page, "transfer-export-scope-selected", TRUE); wait_for(shown, groups, "grup listesi yeniden");
    GtkWidget *run = required(h->page, "transfer-export-run");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(audio), FALSE); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(programme), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(media), FALSE);
    g_assert_false(gtk_widget_is_sensitive(run));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(programme), TRUE); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(media), TRUE);
    g_assert_true(gtk_widget_is_sensitive(run));
    gtk_entry_set_text(GTK_ENTRY(name), "deneme");
    g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(required(h->page, "transfer-export-destination")), config.project_root));
    click(h->page, "transfer-export-run");
    GtkWidget *progress = required(h->page, "transfer-export-progress"), *cancel = required(h->page, "transfer-export-cancel");
    wait_for(shown, progress, "ilerleme çubuğu");
    Needle done = {required(h->page, "transfer-export-status"), "Paket yazıldı: "};
    wait_for(label_has, &done, "paket yolu durum satırında");
    gchar *expected = g_build_filename(config.project_root, "deneme.st", NULL);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(done.label)), expected));
    g_assert_nonnull(last_notice); g_assert_nonnull(strstr(last_notice, expected)); g_free(expected);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(name)), ==, "deneme.st");
    g_assert_cmpfloat(gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(progress)), ==, 1.0);
    g_assert_true(gtk_widget_get_visible(progress)); g_assert_false(gtk_widget_get_visible(cancel));
    g_assert_true(gtk_widget_is_sensitive(run));
    JsonObject *body = recorded("export-start");
    g_assert_cmpstr(radios_json_string(body, "settings", ""), ==, "selected");
    g_assert_cmpstr(radios_json_string(body, "destination_dir", ""), ==, config.project_root);
    g_assert_cmpstr(radios_json_string(body, "file_name", ""), ==, "deneme.st");
    g_assert_cmpstr(radios_json_string(body, "station_id", ""), ==, radios_data_station_id(&h->ctx));
    g_assert_true(json_object_get_boolean_member(body, "media"));
    JsonArray *kinds = json_object_get_array_member(body, "kinds");
    g_assert_cmpuint(json_array_get_length(kinds), ==, 1);
    g_assert_cmpstr(json_array_get_string_element(kinds, 0), ==, "programme");
    /* A whole group goes out whole: no per-document choice is sent. */
    g_assert_false(json_object_has_member(body, "documents"));
    json_object_unref(body);
    JsonObject *job = recorded("job"); g_assert_cmpstr(radios_json_string(job, "job_id", ""), !=, ""); json_object_unref(job);
    harness_close(h);
}

/* Per-document choice inside a group (guide §12 "belirli ayarlar veya ayar
 * grupları"); the fake lists three programme documents. */
static const gchar *const programme_ids[] = {"6f1c2a3b-4d5e-4f60-8a7b-9c0d1e2f3a01", "6f1c2a3b-4d5e-4f60-8a7b-9c0d1e2f3a02",
    "6f1c2a3b-4d5e-4f60-8a7b-9c0d1e2f3a03"};
static GtkWidget *programme_item(Harness *h, guint index)
{
    gchar *name = g_strconcat("transfer-export-item-programme-", programme_ids[index], NULL);
    GtkWidget *item = required(h->page, name); g_free(name); g_assert_true(GTK_IS_CHECK_BUTTON(item)); return item;
}
static gboolean item_active(Harness *h, guint index) { return gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(programme_item(h, index))); }
static void item_set(Harness *h, guint index, gboolean state)
{ gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(programme_item(h, index)), state); }
static gboolean partial(GtkWidget *group) { return gtk_toggle_button_get_inconsistent(GTK_TOGGLE_BUTTON(group)); }
static gboolean laid_out(gpointer value)
{ GtkAllocation allocation; gtk_widget_get_allocation(value, &allocation); return allocation.x >= 0 && allocation.width > 1; }
static gboolean reloaded(gpointer value)
{
    GtkWidget *group = find(((Harness *)value)->page, "transfer-export-group-programme");
    return group && !g_object_get_data(G_OBJECT(group), "transfer-test-before-reload");
}
static void export_run(Harness *h, const gchar *file_name)
{
    gtk_entry_set_text(GTK_ENTRY(required(h->page, "transfer-export-name")), file_name);
    g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(required(h->page, "transfer-export-destination")), config.project_root));
    click(h->page, "transfer-export-run");
    gchar *expected = g_build_filename(config.project_root, file_name, NULL);
    Needle done = {required(h->page, "transfer-export-status"), expected};
    wait_for(label_has, &done, "paket yolu durum satırında");
    g_assert_true(g_str_has_prefix(gtk_label_get_text(GTK_LABEL(done.label)), "Paket yazıldı: ")); g_free(expected);
    wait_for(sensitive, required(h->page, "transfer-export-run"), "iş bitince düğme yeniden etkin");
}
static void export_selected_items(void)
{
    Harness *h = harness_open(TRUE, radios_transfer_export_page_new);
    wait_for(export_loaded, h, "ayar grupları ve program kayıtları");
    run_for(900); /* the connection's refresh may reload the groups once more */
    GtkWidget *programme = required(h->page, "transfer-export-group-programme");
    GtkWidget *expand = required(h->page, "transfer-export-group-programme-expand");
    GtkWidget *items = required(h->page, "transfer-export-group-programme-items");
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(programme)), ==, "Programlar (12)");
    g_assert_true(GTK_IS_TOGGLE_BUTTON(expand)); g_assert_false(GTK_IS_CHECK_BUTTON(expand));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(expand)), ==, "▸");
    gchar *tip = gtk_widget_get_tooltip_text(expand); g_assert_cmpstr(tip, ==, "Kayıtları göster"); g_free(tip);
    g_assert_true(GTK_IS_BOX(items)); g_assert_false(gtk_widget_get_visible(items));
    /* A group the service lists no documents for (an older service) keeps a plain check button. */
    g_assert_null(find(h->page, "transfer-export-group-audio_preferences-expand"));
    g_assert_null(find(h->page, "transfer-export-group-audio_preferences-items"));
    /* Normal form: the list stays hidden until opened, and opens indented below its group. */
    toggle(h->page, "transfer-export-scope-selected", TRUE); wait_for(shown, programme, "seçili kapsamda programlar grubu");
    g_assert_false(gtk_widget_get_visible(items));
    gtk_button_clicked(GTK_BUTTON(expand)); wait_for(shown, items, "program kayıtları açıldı");
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(expand)), ==, "▾");
    const gchar *titles[] = {"Akşam kuşağı", "Öğle haberleri", "Sabah programı"};
    for (guint i = 0; i < G_N_ELEMENTS(titles); ++i) {
        GtkWidget *item = programme_item(h, i); gint x = 0, y = 0;
        g_assert_true(shown(item)); g_assert_true(item_active(h, i)); wait_for(laid_out, item, "kayıt satırı yerleşti");
        g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(item)), ==, titles[i]);
        g_assert_true(gtk_widget_translate_coordinates(item, programme, 0, 0, &x, &y)); g_assert_cmpint(x, >, 0); g_assert_cmpint(y, >, 0);
    }
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(programme))); g_assert_false(partial(programme));

    /* One document unchecked: the group turns partial and goes out with the other two only. */
    GtkWidget *run = required(h->page, "transfer-export-run");
    item_set(h, 1, FALSE);
    g_assert_true(partial(programme)); g_assert_true(gtk_widget_is_sensitive(run));
    export_run(h, "parcali.st");
    JsonObject *body = recorded("export-start");
    g_assert_cmpstr(radios_json_string(body, "settings", ""), ==, "selected");
    JsonArray *kinds = json_object_get_array_member(body, "kinds");
    g_assert_cmpuint(json_array_get_length(kinds), ==, 2);
    g_assert_cmpstr(json_array_get_string_element(kinds, 0), ==, "audio_preferences");
    g_assert_cmpstr(json_array_get_string_element(kinds, 1), ==, "programme");
    g_assert_true(json_object_has_member(body, "documents"));
    JsonObject *documents = json_object_get_object_member(body, "documents");
    g_assert_cmpuint(json_object_get_size(documents), ==, 1); g_assert_true(json_object_has_member(documents, "programme"));
    JsonArray *ids = json_object_get_array_member(documents, "programme");
    g_assert_cmpuint(json_array_get_length(ids), ==, 2);
    g_assert_cmpstr(json_array_get_string_element(ids, 0), ==, programme_ids[0]);
    g_assert_cmpstr(json_array_get_string_element(ids, 1), ==, programme_ids[2]);
    json_object_unref(body);

    /* Every document checked again: the group is whole and names no documents. */
    item_set(h, 1, TRUE);
    g_assert_false(partial(programme)); g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(programme)));
    export_run(h, "tam.st");
    body = recorded("export-start");
    kinds = json_object_get_array_member(body, "kinds");
    g_assert_cmpuint(json_array_get_length(kinds), ==, 2); g_assert_cmpstr(json_array_get_string_element(kinds, 1), ==, "programme");
    g_assert_false(json_object_has_member(body, "documents"));
    json_object_unref(body);

    /* No document checked: the group is out; with nothing else chosen there is nothing to export. */
    for (guint i = 0; i < G_N_ELEMENTS(programme_ids); ++i) item_set(h, i, FALSE);
    g_assert_false(partial(programme)); g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(programme)));
    toggle(h->page, "transfer-export-group-audio_preferences", FALSE); toggle(h->page, "transfer-export-media", FALSE);
    g_assert_false(gtk_widget_is_sensitive(run));
    item_set(h, 2, TRUE);
    g_assert_true(partial(programme)); g_assert_true(gtk_widget_is_sensitive(run));
    /* The group button drives the list: a partial group checks all, a full one clears all. */
    click(h->page, "transfer-export-group-programme");
    for (guint i = 0; i < G_N_ELEMENTS(programme_ids); ++i) g_assert_true(item_active(h, i));
    g_assert_false(partial(programme)); g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(programme)));
    click(h->page, "transfer-export-group-programme");
    for (guint i = 0; i < G_N_ELEMENTS(programme_ids); ++i) g_assert_false(item_active(h, i));
    g_assert_false(partial(programme)); g_assert_false(gtk_widget_is_sensitive(run));

    /* A reload (every map and data refresh) keeps the choice and the open list. */
    item_set(h, 0, TRUE);
    g_object_set_data(G_OBJECT(programme), "transfer-test-before-reload", GINT_TO_POINTER(1));
    radios_data_refresh(&h->ctx); wait_for(reloaded, h, "gruplar yeniden yüklendi");
    programme = required(h->page, "transfer-export-group-programme"); expand = required(h->page, "transfer-export-group-programme-expand");
    items = required(h->page, "transfer-export-group-programme-items");
    g_assert_true(partial(programme)); g_assert_true(item_active(h, 0)); g_assert_false(item_active(h, 1)); g_assert_false(item_active(h, 2));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(required(h->page, "transfer-export-group-audio_preferences"))));
    wait_for(shown, items, "açık liste yeniden yüklemeden sonra açık"); g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(expand)), ==, "▾");
    wait_for(sensitive, run, "kısmi grup dışa aktarılabilir");

    /* "Tüm radyo ayarları" never names documents, whatever the list holds. */
    toggle(h->page, "transfer-export-scope-all", TRUE); wait_for(hidden, required(h->page, "transfer-export-groups"), "tüm kapsamda liste gizli");
    export_run(h, "hepsi.st");
    body = recorded("export-start");
    g_assert_cmpstr(radios_json_string(body, "settings", ""), ==, "all");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(body, "kinds")), ==, 0);
    g_assert_false(json_object_has_member(body, "documents"));
    json_object_unref(body);

    /* The toggle closes the list again. */
    toggle(h->page, "transfer-export-scope-selected", TRUE); wait_for(shown, items, "liste yeniden görünür");
    gtk_button_clicked(GTK_BUTTON(expand)); wait_for(hidden, items, "program kayıtları kapandı");
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(expand)), ==, "▸");
    harness_close(h);
}
static void export_error(void)
{
    Harness *h = harness_open(TRUE, radios_transfer_export_page_new);
    wait_for(export_loaded, h, "ayar grupları");
    gtk_entry_set_text(GTK_ENTRY(required(h->page, "transfer-export-name")), "hata.st");
    g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(required(h->page, "transfer-export-destination")), config.project_root));
    click(h->page, "transfer-export-run");
    Needle failed = {required(h->page, "transfer-export-status"), "Hedef klasör yazılabilir değil."};
    wait_for(label_has, &failed, "servis hatası durum satırında");
    g_assert_true(g_str_has_prefix(gtk_label_get_text(GTK_LABEL(failed.label)), "Dışa aktarma başlatılamadı: "));
    wait_for(sensitive, required(h->page, "transfer-export-run"), "hata sonrası düğme yeniden etkin");
    g_assert_false(gtk_widget_get_visible(required(h->page, "transfer-export-cancel")));
    g_assert_false(gtk_widget_get_visible(required(h->page, "transfer-export-progress")));
    harness_close(h);
}
static void export_cancel(void)
{
    Harness *h = harness_open(TRUE, radios_transfer_export_page_new);
    wait_for(export_loaded, h, "ayar grupları");
    gtk_entry_set_text(GTK_ENTRY(required(h->page, "transfer-export-name")), "yavas.st");
    g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(required(h->page, "transfer-export-destination")), config.project_root));
    click(h->page, "transfer-export-run");
    GtkWidget *cancel = required(h->page, "transfer-export-cancel");
    wait_for(shown, cancel, "vazgeç düğmesi iş sırasında"); wait_for(sensitive, cancel, "iş kimliği alındı");
    g_assert_false(gtk_widget_is_sensitive(required(h->page, "transfer-export-run")));
    g_assert_false(gtk_widget_is_sensitive(required(h->page, "transfer-export-name")));
    gtk_button_clicked(GTK_BUTTON(cancel));
    Needle cancelled = {required(h->page, "transfer-export-status"), "Dışa aktarma iptal edildi."};
    wait_for(label_has, &cancelled, "iptal sonucu");
    wait_for(hidden, cancel, "vazgeç gizlendi"); g_assert_true(gtk_widget_is_sensitive(required(h->page, "transfer-export-run")));
    JsonObject *body = recorded("job-cancel"); g_assert_cmpstr(radios_json_string(body, "job_id", ""), !=, ""); json_object_unref(body);
    harness_close(h);
}
static void choose_package(Harness *h, const gchar *name)
{
    write_file(name);
    gchar *path = g_build_filename(config.project_root, name, NULL);
    GtkWidget *chooser = required(h->page, "transfer-import-package");
    g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), path)); g_free(path);
    g_signal_emit_by_name(chooser, "file-set");
}
static void import_flow(void)
{
    refreshes = 0;
    Harness *h = harness_open(TRUE, radios_transfer_import_page_new);
    radios_data_register_refresh(&h->ctx, h->window, counted_refresh);
    wait_for(sensitive, required(h->page, "transfer-import-package"), "bağlantı sonrası paket seçici etkin");
    g_assert_false(gtk_widget_is_sensitive(required(h->page, "transfer-import-run")));
    choose_package(h, "paket.st");
    wait_for(import_inspected, h, "paket özeti");
    const gchar *head = gtk_label_get_text(GTK_LABEL(required(h->page, "transfer-import-created")));
    g_assert_nonnull(strstr(head, "18.10.2026")); g_assert_nonnull(strstr(head, "İstasyon: Test Radyo")); g_assert_nonnull(strstr(head, "RadioS 0.19.0"));
    GtkWidget *audio = required(h->page, "transfer-import-group-audio_preferences"), *programme = required(h->page, "transfer-import-group-programme");
    g_assert_true(GTK_IS_CHECK_BUTTON(audio)); g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(programme)));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(programme)), ==, "Programlar (12)");
    g_assert_true(g_str_has_prefix(gtk_label_get_text(GTK_LABEL(required(h->page, "transfer-import-media-summary"))), "Medya: 7 şarkı ("));
    GtkWidget *warnings = required(h->page, "transfer-import-warnings");
    g_assert_true(shown(warnings)); g_assert_true(g_str_has_prefix(gtk_label_get_text(GTK_LABEL(warnings)), "Uyarı: "));
    GtkWidget *settings = required(h->page, "transfer-import-settings"), *media = required(h->page, "transfer-import-media");
    GtkWidget *groups = required(h->page, "transfer-import-groups"), *root_row = required(h->page, "transfer-import-media-root-row");
    g_assert_true(shown(settings)); g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(settings)));
    g_assert_true(shown(media)); g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(media)));
    g_assert_true(shown(groups)); g_assert_true(shown(root_row));
    /* Normal form: media root follows the media choice, group rows follow the settings choice. */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(media), FALSE); wait_for(hidden, root_row, "medya kökü gizli");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(settings), FALSE); wait_for(hidden, groups, "grup satırları gizli");
    g_assert_false(gtk_widget_is_sensitive(required(h->page, "transfer-import-run")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(settings), TRUE); wait_for(shown, groups, "grup satırları yeniden");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(media), TRUE); wait_for(shown, root_row, "medya kökü yeniden");
    g_assert_true(gtk_widget_is_sensitive(required(h->page, "transfer-import-run")));
    g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(required(h->page, "transfer-import-media-root")), config.project_root));
    click(h->page, "transfer-import-run");
    wait_for(shown, required(h->page, "transfer-import-cancel"), "vazgeç düğmesi içe aktarma sırasında");
    Needle done = {required(h->page, "transfer-import-status"), "İçe aktarma tamamlandı."};
    wait_for(label_has, &done, "içe aktarma sonucu");
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(done.label)), "Bir paket kaydı atlandı: test kaydı"));
    g_assert_nonnull(last_notice); g_assert_nonnull(strstr(last_notice, "İçe aktarma tamamlandı."));
    wait_for(refreshed, NULL, "diğer görünümler yeniden yüklendi");
    g_assert_cmpfloat(gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(required(h->page, "transfer-import-progress"))), ==, 1.0);
    g_assert_false(gtk_widget_get_visible(required(h->page, "transfer-import-cancel")));
    g_assert_true(gtk_widget_is_sensitive(required(h->page, "transfer-import-run")));
    JsonObject *body = recorded("import-start");
    gchar *package = g_build_filename(config.project_root, "paket.st", NULL);
    g_assert_cmpstr(radios_json_string(body, "package_path", ""), ==, package); g_free(package);
    g_assert_true(json_object_get_boolean_member(body, "settings")); g_assert_true(json_object_get_boolean_member(body, "media"));
    g_assert_cmpstr(radios_json_string(body, "media_root", ""), ==, config.project_root);
    JsonArray *kinds = json_object_get_array_member(body, "kinds"); g_assert_cmpuint(json_array_get_length(kinds), ==, 2);
    json_object_unref(body);
    harness_close(h);
}
static void import_error(void)
{
    Harness *h = harness_open(TRUE, radios_transfer_import_page_new);
    choose_package(h, "bozuk.st");
    Needle failed = {required(h->page, "transfer-import-status"), "Paket bütünlüğü doğrulanamadı."};
    wait_for(label_has, &failed, "paket hatası durum satırında");
    g_assert_true(g_str_has_prefix(gtk_label_get_text(GTK_LABEL(failed.label)), "Paket incelenemedi: "));
    g_assert_false(gtk_widget_get_visible(required(h->page, "transfer-import-summary")));
    g_assert_false(gtk_widget_is_sensitive(required(h->page, "transfer-import-run")));
    g_assert_true(gtk_widget_is_sensitive(required(h->page, "transfer-import-package")));
    harness_close(h);
}
static void capture(GtkWidget *window, const gchar *name)
{
    /* The idle pointer sits mid-screen under Xvfb; keep it off the page so no tooltip covers the capture. */
    GdkDisplay *display = gdk_display_get_default();
    gdk_device_warp(gdk_seat_get_pointer(gdk_display_get_default_seat(display)), gdk_display_get_default_screen(display), 1590, 990);
    gtk_window_present(GTK_WINDOW(window)); run_for(500);
    gtk_widget_queue_draw(window); run_for(500); gdk_display_sync(display); run_for(200);
    GdkWindow *surface = gtk_widget_get_window(window); g_assert_nonnull(surface);
    GdkPixbuf *shot = gdk_pixbuf_get_from_window(surface, 0, 0, gdk_window_get_width(surface), gdk_window_get_height(surface));
    g_assert_nonnull(shot);
    gchar *path = g_build_filename(g_getenv("RADIOS_TRANSFER_SHOTS"), name, NULL); GError *error = NULL;
    g_assert_true(gdk_pixbuf_save(shot, path, "png", &error, NULL)); g_assert_no_error(error);
    g_test_message("Ekran görüntüsü: %s", path); g_object_unref(shot); g_free(path);
}
static void dress(Harness *h)
{
    gtk_widget_set_name(h->window, "preferences-window");
    gtk_style_context_add_class(gtk_widget_get_style_context(h->window), "radios");
    gtk_window_resize(GTK_WINDOW(h->window), 980, 820);
}
static void screenshots(void)
{
    if (!g_getenv("RADIOS_TRANSFER_SHOTS")) { g_test_skip("RADIOS_TRANSFER_SHOTS tanımlı değil"); return; }
    /* The application's Preferences stylesheet is installed by its window; keep that window hidden. */
    GtkWidget *parent = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(parent);
    RadioSViewContext bare = {.config = &config, .parent = GTK_WINDOW(parent)};
    GtkWidget *preferences = radios_preferences_open(&bare); g_assert_nonnull(preferences);
    run_for(100); gtk_widget_hide(preferences);
    Harness *h = harness_open(TRUE, radios_transfer_export_page_new); dress(h);
    wait_for(export_loaded, h, "ayar grupları"); toggle(h->page, "transfer-export-scope-selected", TRUE);
    capture(h->window, "d3-export.png");
    /* Per-document choice: the programme list open, one document unchecked, the group partial. */
    gtk_button_clicked(GTK_BUTTON(required(h->page, "transfer-export-group-programme-expand")));
    wait_for(shown, required(h->page, "transfer-export-group-programme-items"), "program kayıtları açık");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(required(h->page, "transfer-export-item-programme-6f1c2a3b-4d5e-4f60-8a7b-9c0d1e2f3a02")), FALSE);
    g_assert_true(gtk_toggle_button_get_inconsistent(GTK_TOGGLE_BUTTON(required(h->page, "transfer-export-group-programme"))));
    capture(h->window, "d3-export-kismi.png"); harness_close(h);
    h = harness_open(TRUE, radios_transfer_import_page_new); dress(h);
    choose_package(h, "paket.st"); wait_for(import_inspected, h, "paket özeti");
    capture(h->window, "d3-import.png"); harness_close(h);
    gtk_widget_destroy(parent); g_object_unref(parent); run_for(40);
}

/* Against a real private SDBASE service (scratchpad runner): export the
 * seeded station into RADIOS_TRANSFER_REAL_DIR, then import that package
 * back with RADIOS_TRANSFER_REAL_MEDIA_ROOT as the new media root. */
static gboolean real_export_loaded(gpointer value)
{ Harness *h = value; return find(h->page, "transfer-export-group-midi_mapping") != NULL && gtk_widget_is_sensitive(required(h->page, "transfer-export-run")); }
static gboolean real_import_inspected(gpointer value)
{ Harness *h = value; return shown(required(h->page, "transfer-import-summary")) && find(h->page, "transfer-import-group-midi_mapping") != NULL; }
static void real_service(void)
{
    const gchar *dir = g_getenv("RADIOS_TRANSFER_REAL_DIR"), *media_root = g_getenv("RADIOS_TRANSFER_REAL_MEDIA_ROOT");
    if (!dir || !media_root) { g_test_skip("RADIOS_TRANSFER_REAL_DIR / RADIOS_TRANSFER_REAL_MEDIA_ROOT tanımlı değil"); return; }
    Harness *h = harness_open(TRUE, radios_transfer_export_page_new);
    wait_for(real_export_loaded, h, "gerçek servisten ayar grupları");
    GtkWidget *midi = required(h->page, "transfer-export-group-midi_mapping"), *media = required(h->page, "transfer-export-media");
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(midi)), ==, "MIDI eşlemeleri (1)");
    g_assert_true(g_str_has_prefix(gtk_button_get_label(GTK_BUTTON(media)), "Medya (1 şarkı, "));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(media)));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(required(h->page, "transfer-export-scope-all"))));
    gtk_entry_set_text(GTK_ENTRY(required(h->page, "transfer-export-name")), "gercek");
    g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(required(h->page, "transfer-export-destination")), dir));
    click(h->page, "transfer-export-run");
    Needle done = {required(h->page, "transfer-export-status"), "Paket yazıldı: "};
    wait_for(label_has, &done, "gerçek paket yolu durum satırında");
    gchar *package = g_build_filename(dir, "gercek.st", NULL);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(done.label)), package));
    g_assert_true(g_file_test(package, G_FILE_TEST_IS_REGULAR));
    harness_close(h);

    refreshes = 0;
    h = harness_open(TRUE, radios_transfer_import_page_new);
    radios_data_register_refresh(&h->ctx, h->window, counted_refresh);
    wait_for(sensitive, required(h->page, "transfer-import-package"), "paket seçici etkin");
    GtkWidget *chooser = required(h->page, "transfer-import-package");
    g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), package));
    g_signal_emit_by_name(chooser, "file-set");
    wait_for(real_import_inspected, h, "gerçek paket özeti");
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(required(h->page, "transfer-import-group-midi_mapping"))), ==, "MIDI eşlemeleri (1)");
    g_assert_true(g_str_has_prefix(gtk_label_get_text(GTK_LABEL(required(h->page, "transfer-import-media-summary"))), "Medya: 1 şarkı ("));
    GtkWidget *place = required(h->page, "transfer-import-media"), *root_row = required(h->page, "transfer-import-media-root-row");
    g_assert_true(shown(place)); g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(place))); g_assert_true(shown(root_row));
    g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(required(h->page, "transfer-import-media-root")), media_root));
    click(h->page, "transfer-import-run");
    Needle finished = {required(h->page, "transfer-import-status"), "İçe aktarma tamamlandı."};
    wait_for(label_has, &finished, "gerçek içe aktarma sonucu");
    wait_for(refreshed, NULL, "diğer görünümler yeniden yüklendi");
    /* The runner names the relative path it expects below the new media root
     * (the song's original archive subfolders, or Album/file without them). */
    const gchar *relative = g_getenv("RADIOS_TRANSFER_REAL_PLACED");
    gchar *placed = g_build_filename(media_root, relative && *relative ? relative : "E2E Albümü/e2e-sarki.wav", NULL);
    g_assert_true(g_file_test(placed, G_FILE_TEST_IS_REGULAR));
    g_free(placed); g_free(package);
    harness_close(h);
}

/* Real service, "Seçili radyo ayarları" with a partial group: only the MIDI
 * group stays, and inside it every record except RADIOS_TRANSFER_REAL_SKIP_ID. */
static void uncheck_groups_except(GtkWidget *root, const gchar *keep)
{
    if (GTK_IS_CHECK_BUTTON(root)) {
        const gchar *name = gtk_widget_get_name(root);
        if (g_str_has_prefix(name, "transfer-export-group-") && !g_str_has_suffix(name, keep))
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(root), FALSE);
        return;
    }
    if (!GTK_IS_CONTAINER(root)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    for (GList *item = children; item; item = item->next) uncheck_groups_except(item->data, keep);
    g_list_free(children);
}
static void real_service_selected(void)
{
    const gchar *dir = g_getenv("RADIOS_TRANSFER_REAL_DIR"), *skip = g_getenv("RADIOS_TRANSFER_REAL_SKIP_ID");
    if (!dir || !skip) { g_test_skip("RADIOS_TRANSFER_REAL_DIR / RADIOS_TRANSFER_REAL_SKIP_ID tanımlı değil"); return; }
    Harness *h = harness_open(TRUE, radios_transfer_export_page_new);
    wait_for(real_export_loaded, h, "gerçek servisten ayar grupları");
    toggle(h->page, "transfer-export-scope-selected", TRUE);
    wait_for(shown, required(h->page, "transfer-export-groups"), "seçili kapsamda grup listesi");
    uncheck_groups_except(required(h->page, "transfer-export-groups"), "-midi_mapping");
    GtkWidget *midi = required(h->page, "transfer-export-group-midi_mapping");
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(midi)), ==, "MIDI eşlemeleri (2)");
    click(h->page, "transfer-export-group-midi_mapping-expand");
    wait_for(shown, required(h->page, "transfer-export-group-midi_mapping-items"), "MIDI kayıt listesi");
    gchar *item = g_strconcat("transfer-export-item-midi_mapping-", skip, NULL);
    toggle(h->page, item, FALSE); g_free(item);
    g_assert_true(gtk_toggle_button_get_inconsistent(GTK_TOGGLE_BUTTON(midi)));
    toggle(h->page, "transfer-export-media", FALSE);
    gtk_entry_set_text(GTK_ENTRY(required(h->page, "transfer-export-name")), "kismi");
    g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(required(h->page, "transfer-export-destination")), dir));
    click(h->page, "transfer-export-run");
    Needle done = {required(h->page, "transfer-export-status"), "Paket yazıldı: "};
    wait_for(label_has, &done, "kısmi paket yolu durum satırında");
    gchar *package = g_build_filename(dir, "kismi.st", NULL);
    g_assert_true(g_file_test(package, G_FILE_TEST_IS_REGULAR)); g_free(package);
    harness_close(h);
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]);
    for (gint i = 1; i < argc - 1; ++i) argv[i] = argv[i + 1];
    --argc; g_test_init(&argc, &argv, NULL);
    g_assert_true(gtk_init_check(&argc, &argv));
    GError *error = NULL;
    g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    g_test_add_func("/transfer-ui/export-widgets-offline", export_widgets_offline);
    g_test_add_func("/transfer-ui/import-widgets-offline", import_widgets_offline);
    g_test_add_func("/transfer-ui/export-flow", export_flow);
    g_test_add_func("/transfer-ui/export-error", export_error);
    g_test_add_func("/transfer-ui/export-cancel", export_cancel);
    g_test_add_func("/transfer-ui/export-selected-items", export_selected_items);
    g_test_add_func("/transfer-ui/import-flow", import_flow);
    g_test_add_func("/transfer-ui/import-error", import_error);
    g_test_add_func("/transfer-ui/screenshots", screenshots);
    g_test_add_func("/transfer-ui/real-service", real_service);
    g_test_add_func("/transfer-ui/real-service-selected", real_service_selected);
    int result = g_test_run(); radios_config_clear(&config); g_free(last_notice); return result;
}
