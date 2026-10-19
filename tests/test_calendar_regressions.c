#include "radios/automation_calendar.h"
#include "radios/data.h"
#include "radios/widgets.h"
#include <string.h>

typedef struct {
    RadioSViewContext ctx;
    GtkWidget *window, *page, *rows;
    guint changes;
} Fixture;

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (!g_strcmp0(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *it = children; it && !found; it = it->next) found = find(it->data, name);
    g_list_free(children); return found;
}
static void pump(void)
{ while (g_main_context_iteration(NULL, FALSE)) {} }
/* A newly shown widget receives its size on a later frame-clock tick:
 * keep pumping until its allocation is real (bounded, 3 s). */
static void pump_until_allocated(GtkWidget *widget)
{
    gint64 end = g_get_monotonic_time() + 3 * G_TIME_SPAN_SECOND; GtkAllocation box;
    do { pump(); gtk_widget_get_allocation(widget, &box); if (box.x >= 0 && box.width > 1) return; g_usleep(5000); }
    while (g_get_monotonic_time() < end);
}
static void set_id(JsonObject *object, const gchar *key, guint n)
{
    gchar *id = g_strdup_printf("10000000-0000-4000-8000-%012u", n);
    json_object_set_string_member(object, key, id); g_free(id);
}
static JsonObject *source(guint n)
{
    JsonObject *row = json_object_new(); set_id(row, "id", n); set_id(row, "playlist_id", n + 1000);
    json_object_set_int_member(row, "frequency", 1);
    const gchar *flags[] = {"random", "track_random", "advert_random", "sweeper_random"};
    for (guint i = 0; i < G_N_ELEMENTS(flags); ++i) json_object_set_boolean_member(row, flags[i], FALSE);
    json_object_set_string_member(row, "media_kind", "all"); return row;
}
static JsonObject *calendar(guint library_sources)
{
    JsonObject *config = json_object_new(), *event = json_object_new();
    json_object_set_int_member(config, "schema", 1); json_object_set_string_member(config, "timezone", "Europe/Istanbul");
    JsonArray *events = json_array_new(), *rows = json_array_new();
    set_id(event, "id", 1); json_object_set_string_member(event, "title", "Test program");
    json_object_set_string_member(event, "date", "2026-10-12");
    json_object_set_string_member(event, "color", "#abd6e4");
    json_object_set_int_member(event, "start_minute", 600); json_object_set_int_member(event, "end_minute", 660);
    if (library_sources) {
        JsonObject *definition = json_object_new(), *assignment = json_object_new(), *settings = json_object_new();
        JsonArray *library = json_array_new(), *sources = json_array_new();
        set_id(definition, "id", 10); json_object_set_string_member(definition, "name", "Test rotation");
        json_object_set_string_member(settings, "selection_mode", "linear");
        json_object_set_object_member(definition, "settings", settings);
        for (guint i = 0; i < library_sources; ++i) json_array_add_object_element(sources, source(100 + i));
        json_object_set_array_member(definition, "sources", sources); json_array_add_object_element(library, definition);
        json_object_set_array_member(config, "rotation_library", library);
        set_id(assignment, "id", 2); set_id(assignment, "rotation_id", 10); json_array_add_object_element(rows, assignment);
    } else {
        json_array_add_object_element(rows, source(2)); json_array_add_object_element(rows, source(3));
    }
    json_object_set_array_member(event, "rotations", rows); json_array_add_object_element(events, event);
    json_object_set_array_member(config, "events", events); return config;
}
static void changed(GtkWidget *page, JsonNode *snapshot, gpointer data)
{ (void)page; (void)snapshot; Fixture *f = data; ++f->changes; }
static void setup(Fixture *f, guint count)
{
    memset(f, 0, sizeof *f); f->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(f->window);
    f->ctx.parent = GTK_WINDOW(f->window); JsonObject *config = calendar(count);
    f->page = radios_automation_calendar_editor_new(&f->ctx, config, changed, f); json_object_unref(config);
    gtk_container_add(GTK_CONTAINER(f->window), f->page);
    GtkWidget *week = find(f->page, "automation-calendar-week");
    if (!week) week = find(f->page, "automation-calendar-date");
    g_assert_true(GTK_IS_ENTRY(week)); gtk_entry_set_text(GTK_ENTRY(week), "2026-10-12");
    g_signal_emit_by_name(week, "activate");
    radios_automation_calendar_select(f->page, 0, 600, 601, FALSE);
    f->rows = find(f->page, "automation-calendar-rotations"); g_assert_true(GTK_IS_TREE_VIEW(f->rows));
    pump(); f->changes = 0;
}
static void teardown(Fixture *f)
{ gtk_widget_destroy(f->window); g_object_unref(f->window); pump(); }
static JsonArray *rotations(JsonNode *snapshot)
{
    JsonArray *events = json_object_get_array_member(json_node_get_object(snapshot), "events");
    return json_object_get_array_member(json_array_get_object_element(events, 0), "rotations");
}
static void drag_first_to_end(Fixture *f)
{
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(f->rows)));
    GtkTreeIter first, copy; g_assert_true(gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &first));
    GValue values[8] = {G_VALUE_INIT};
    for (gint i = 0; i < 8; ++i) gtk_tree_model_get_value(GTK_TREE_MODEL(store), &first, i, &values[i]);
    /* GtkListStore DND inserts a copy and deletes the original; it does not
     * emit rows-reordered, unlike gtk_list_store_reorder. */
    gtk_list_store_append(store, &copy);
    for (gint i = 0; i < 8; ++i) { gtk_list_store_set_value(store, &copy, i, &values[i]); g_value_unset(&values[i]); }
    gtk_list_store_remove(store, &first);
}
static void test_dnd_order_survives_refresh(void)
{
    Fixture f; setup(&f, 0); drag_first_to_end(&f); pump(); g_assert_cmpuint(f.changes, ==, 1);
    JsonNode *snapshot = radios_automation_calendar_snapshot(f.page); JsonArray *rows = rotations(snapshot);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(rows, 0), "id", ""), ==, "10000000-0000-4000-8000-000000000003");
    radios_automation_calendar_set_snapshot(f.page, json_node_get_object(snapshot)); json_node_free(snapshot);
    GtkTreeIter first; GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(f.rows));
    g_assert_true(gtk_tree_model_get_iter_first(model, &first)); gchar *id = NULL; gtk_tree_model_get(model, &first, 0, &id, -1);
    g_assert_cmpstr(id, ==, "10000000-0000-4000-8000-000000000003"); g_free(id); teardown(&f);
}
static void test_dnd_order_before_selection_refresh(void)
{
    Fixture f; setup(&f, 0); drag_first_to_end(&f);
    radios_automation_calendar_select(f.page, 0, 700, 701, FALSE);
    JsonNode *snapshot = radios_automation_calendar_snapshot(f.page);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(rotations(snapshot), 0), "id", ""), ==,
        "10000000-0000-4000-8000-000000000003");
    json_node_free(snapshot); pump(); g_assert_cmpuint(f.changes, ==, 1); teardown(&f);
}
static void test_pending_dnd_close(void)
{ Fixture f; setup(&f, 0); drag_first_to_end(&f); teardown(&f); g_assert_cmpuint(f.changes, ==, 1); }
static void row_action(Fixture *f, const gchar *name)
{
    GList *menus = gtk_menu_get_for_attach_widget(f->rows); GtkWidget *item = NULL;
    for (GList *it = menus; it && !item; it = it->next) item = find(it->data, name);
    g_assert_true(GTK_IS_MENU_ITEM(item)); gtk_menu_item_activate(GTK_MENU_ITEM(item)); pump();
}
static void test_paste_expanded_playlist_limit(void)
{
    for (guint count = 25; count <= 26; ++count) {
        Fixture f; setup(&f, count);
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(f.rows));
        GtkTreePath *path = gtk_tree_path_new_from_indices(0, -1); gtk_tree_selection_select_path(selection, path); gtk_tree_path_free(path);
        row_action(&f, "automation-calendar-row-copy"); row_action(&f, "automation-calendar-row-paste");
        JsonNode *snapshot = radios_automation_calendar_snapshot(f.page); JsonArray *rows = rotations(snapshot);
        g_assert_cmpuint(json_array_get_length(rows), ==, count == 25 ? 2 : 1);
        g_assert_cmpuint(f.changes, ==, count == 25 ? 1 : 0);
        if (count == 25) g_assert_cmpstr(radios_json_string(json_array_get_object_element(rows, 0), "id", ""), !=,
            radios_json_string(json_array_get_object_element(rows, 1), "id", ""));
        else g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(f.page, "automation-calendar-status"))), "50 playlist"));
        json_node_free(snapshot); teardown(&f);
    }
}
static void test_simple_programme_and_inline_probability(void)
{
    Fixture f; setup(&f, 0);
    gtk_widget_show_all(f.window); pump();
    GtkWidget *programme = find(f.page, "automation-calendar-programme-section");
    GtkWidget *rotation = find(f.page, "automation-calendar-rotation-section");
    GtkWidget *rules = find(f.page, "automation-calendar-rules-section");
    g_assert_nonnull(programme); g_assert_nonnull(rotation); g_assert_nonnull(rules);
    g_assert_true(gtk_widget_is_ancestor(find(f.page, "automation-calendar-programme-name"), programme));
    g_assert_true(gtk_widget_is_ancestor(find(f.page, "automation-calendar-programme-start"), programme));
    g_assert_true(gtk_widget_is_ancestor(f.rows, rotation));
    GtkWidget *probability = find(f.page, "automation-calendar-programme-probability");
    g_assert_true(gtk_widget_is_ancestor(probability, rules));
    GtkWidget *details = find(f.page, "automation-calendar-details");
    GtkWidget *library = find(f.page, "automation-calendar-library-picker");
    g_assert_false(gtk_expander_get_expanded(GTK_EXPANDER(details)));
    g_assert_false(gtk_expander_get_expanded(GTK_EXPANDER(library)));
    GtkWidget *toggle = find(f.page, "automation-calendar-detail-columns");
    for (guint pass = 0; pass < 3; ++pass) {
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), pass == 1);
        g_assert_cmpint(gtk_tree_view_get_n_columns(GTK_TREE_VIEW(f.rows)), ==, 3);
        for (gint i = 0; i < 3; ++i)
            g_assert_cmpint(gtk_tree_view_column_get_visible(gtk_tree_view_get_column(GTK_TREE_VIEW(f.rows), i)), ==, pass == 1 || i < 2);
    }
    JsonNode *before = radios_automation_calendar_snapshot(f.page);
    gtk_button_clicked(GTK_BUTTON(probability)); pump();
    GtkWidget *stack = find(f.page, "automation-calendar-pages");
    GtkWidget *page = find(f.page, "automation-calendar-probability-page");
    g_assert_true(GTK_IS_STACK(stack)); g_assert_nonnull(page); g_assert_false(GTK_IS_WINDOW(page));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(stack)), ==, "probability");
    g_assert_true(gtk_widget_get_toplevel(page) == f.window);
    gtk_button_clicked(GTK_BUTTON(find(page, "automation-calendar-probability-back"))); pump();
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(stack)), ==, "calendar");
    g_assert_null(find(f.page, "automation-calendar-probability-page"));
    JsonNode *after = radios_automation_calendar_snapshot(f.page);
    g_assert_true(json_node_equal(before, after)); g_assert_cmpuint(f.changes, ==, 0);
    json_node_free(before); json_node_free(after); teardown(&f);
}
/* Kılavuz §04 adım 1-2 (akım 4c): the "Live event" box of "Program oluştur"
 * opens the live settings beside the programme fields; a live programme
 * persists events[].live (contract §1b), offers rotation schemas only and
 * reopens the same panel from "Program özellikleri". */
#define SCHEMA_ID "10000000-0000-4000-8000-000000000020"
#define ROTATION_ID "10000000-0000-4000-8000-000000000010"
static GtkWidget *toplevel_named(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *found = NULL;
    for (GList *it = windows; it && !found; it = it->next) if (!g_strcmp0(gtk_widget_get_name(it->data), name)) found = it->data;
    g_list_free(windows); return found;
}
static GtkWidget *open_library_editor(Fixture *f)
{
    gtk_button_clicked(GTK_BUTTON(find(f->page, "automation-calendar-manage-rotation"))); pump();
    GtkWidget *dialog = toplevel_named("automation-calendar-library-dialog");
    g_assert_true(GTK_IS_DIALOG(dialog)); return dialog;
}
static void test_detached_rotation_edit_survives_reopen(void)
{
    Fixture f; setup(&f, 1); GtkWidget *dialog = open_library_editor(&f);
    GtkWidget *name = find(dialog, "rotation-editor-name");
    gtk_entry_set_text(GTK_ENTRY(name), "Yeni yayın akışı"); pump();
    g_assert_true(gtk_widget_is_sensitive(name)); g_assert_cmpuint(f.changes, ==, 1);
    JsonNode *snapshot = radios_automation_calendar_snapshot(f.page);
    JsonObject *definition = json_array_get_object_element(json_object_get_array_member(json_node_get_object(snapshot), "rotation_library"), 0);
    g_assert_cmpstr(radios_json_string(definition, "name", ""), ==, "Yeni yayın akışı");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump();
    radios_automation_calendar_set_snapshot(f.page, json_node_get_object(snapshot)); json_node_free(snapshot);
    dialog = open_library_editor(&f);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(dialog, "rotation-editor-name"))), ==, "Yeni yayın akışı");
    teardown(&f);
}
static void test_snapshot_reconciles_open_rotation_editor(void)
{
    Fixture f; setup(&f, 1); GtkWidget *dialog = open_library_editor(&f);
    JsonNode *snapshot = radios_automation_calendar_snapshot(f.page);
    /* An unchanged acknowledgement must leave the editor usable. */
    radios_automation_calendar_set_snapshot(f.page, json_node_get_object(snapshot)); pump();
    g_assert_true(toplevel_named("automation-calendar-library-dialog") == dialog);
    JsonObject *definition = json_array_get_object_element(json_object_get_array_member(json_node_get_object(snapshot), "rotation_library"), 0);
    json_object_set_string_member(definition, "name", "Yeniden yüklenen şema");
    radios_automation_calendar_set_snapshot(f.page, json_node_get_object(snapshot)); json_node_free(snapshot); pump();
    g_assert_null(toplevel_named("automation-calendar-library-dialog"));
    dialog = open_library_editor(&f);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(dialog, "rotation-editor-name"))), ==, "Yeniden yüklenen şema");
    teardown(&f);
}
static void test_programme_switch_closes_rotation_editor(void)
{
    Fixture f; setup(&f, 1); open_library_editor(&f);
    /* Copied programmes can legitimately share identical definition IDs and
     * values. The old editor must still not write to the new programme. */
    g_assert_true(radios_automation_calendar_bind_editor(f.page, changed, &f, "Diğer program")); pump();
    g_assert_null(toplevel_named("automation-calendar-library-dialog"));
    g_assert_cmpuint(f.changes, ==, 0); teardown(&f);
}
static JsonObject *schema_definition(guint n, const gchar *name)
{
    JsonObject *definition = json_object_new(), *settings = json_object_new(); JsonArray *sources = json_array_new();
    set_id(definition, "id", n); json_object_set_string_member(definition, "name", name);
    json_object_set_string_member(settings, "selection_mode", "linear"); json_object_set_boolean_member(settings, "automation_schema", TRUE);
    json_object_set_object_member(definition, "settings", settings);
    json_array_add_object_element(sources, source(n + 100)); json_object_set_array_member(definition, "sources", sources); return definition;
}
static void test_schema_settings_cancel_does_not_create(void)
{
    Fixture f; setup(&f, 1);
    GtkWidget *button = find(f.page, "automation-calendar-create-rotation-schema");
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(button)), ==, "Şema Ayarları");
    JsonNode *before = radios_automation_calendar_snapshot(f.page);
    gtk_button_clicked(GTK_BUTTON(button)); pump();
    GtkWidget *dialog = toplevel_named("automation-calendar-schema-settings-dialog");
    g_assert_true(GTK_IS_DIALOG(dialog)); g_assert_null(toplevel_named("automation-calendar-library-dialog"));
    g_assert_false(gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT)));
    radios_automation_calendar_set_snapshot(f.page, json_node_get_object(before)); pump();
    g_assert_true(toplevel_named("automation-calendar-schema-settings-dialog") == dialog);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump();
    JsonNode *after = radios_automation_calendar_snapshot(f.page);
    g_assert_true(json_node_equal(before, after)); g_assert_cmpuint(f.changes, ==, 0);
    json_node_free(before); json_node_free(after); teardown(&f);
}
static void test_schema_settings_reopens_existing_without_duplicates(void)
{
    Fixture f; setup(&f, 1);
    JsonNode *snapshot = radios_automation_calendar_snapshot(f.page);
    json_array_add_object_element(json_object_get_array_member(json_node_get_object(snapshot), "rotation_library"), schema_definition(20, "Sabah şeması"));
    radios_automation_calendar_set_snapshot(f.page, json_node_get_object(snapshot));
    for (guint i = 0; i < 2; ++i) {
        gtk_button_clicked(GTK_BUTTON(find(f.page, "automation-calendar-create-rotation-schema"))); pump();
        GtkWidget *dialog = toplevel_named("automation-calendar-library-dialog"); g_assert_true(GTK_IS_DIALOG(dialog));
        g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(dialog)), ==, "Şema Ayarları");
        g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(find(dialog, "rotation-editor-pages"))), ==, "schema-settings");
        g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(dialog, "rotation-editor-name"))), ==, "Sabah şeması");
        gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump();
    }
    JsonNode *after = radios_automation_calendar_snapshot(f.page);
    g_assert_true(json_node_equal(snapshot, after)); g_assert_cmpuint(f.changes, ==, 0);
    json_node_free(snapshot); json_node_free(after); teardown(&f);
}
static void test_schema_settings_explicit_create_once(void)
{
    Fixture f; setup(&f, 1);
    gtk_button_clicked(GTK_BUTTON(find(f.page, "automation-calendar-create-rotation-schema"))); pump();
    GtkWidget *chooser = toplevel_named("automation-calendar-schema-settings-dialog"); g_assert_true(GTK_IS_DIALOG(chooser));
    gtk_dialog_response(GTK_DIALOG(chooser), GTK_RESPONSE_APPLY); pump();
    GtkWidget *dialog = toplevel_named("automation-calendar-library-dialog"); g_assert_true(GTK_IS_DIALOG(dialog));
    g_assert_null(toplevel_named("automation-calendar-schema-settings-dialog"));
    g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(dialog)), ==, "Şema Ayarları");
    JsonNode *snapshot = radios_automation_calendar_snapshot(f.page);
    JsonArray *library = json_object_get_array_member(json_node_get_object(snapshot), "rotation_library");
    g_assert_cmpuint(json_array_get_length(library), ==, 2);
    JsonObject *definition = json_array_get_object_element(library, 1);
    g_assert_true(json_object_get_boolean_member(json_object_get_object_member(definition, "settings"), "automation_schema"));
    g_assert_cmpuint(f.changes, ==, 1); json_node_free(snapshot); teardown(&f);
}
static void test_schema_settings_can_create_second_schema(void)
{
    Fixture f; setup(&f, 1);
    JsonNode *snapshot = radios_automation_calendar_snapshot(f.page);
    json_array_add_object_element(json_object_get_array_member(json_node_get_object(snapshot), "rotation_library"), schema_definition(20, "İlk şema"));
    radios_automation_calendar_set_snapshot(f.page, json_node_get_object(snapshot)); json_node_free(snapshot);
    gtk_button_clicked(GTK_BUTTON(find(f.page, "automation-calendar-create-rotation-schema"))); pump();
    GtkWidget *dialog = toplevel_named("automation-calendar-library-dialog"); g_assert_true(GTK_IS_DIALOG(dialog));
    GtkWidget *create = find(dialog, "automation-calendar-new-schema"); g_assert_true(GTK_IS_BUTTON(create));
    gtk_button_clicked(GTK_BUTTON(create)); pump();
    dialog = toplevel_named("automation-calendar-library-dialog"); g_assert_true(GTK_IS_DIALOG(dialog));
    g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(dialog)), ==, "Şema Ayarları");
    snapshot = radios_automation_calendar_snapshot(f.page);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(snapshot), "rotation_library")), ==, 3);
    g_assert_cmpuint(f.changes, ==, 1); json_node_free(snapshot); teardown(&f);
}
/* The chooser lists schemas with their step count and deletes an unused one;
 * a schema a programme names is kept and the reason shown (empty leftover
 * schemas of an old build were otherwise permanent). */
static void test_schema_settings_delete_unused_keep_used(void)
{
    Fixture f; setup(&f, 1);
    JsonNode *snapshot = radios_automation_calendar_snapshot(f.page); JsonObject *root = json_node_get_object(snapshot);
    json_array_add_object_element(json_object_get_array_member(root, "rotation_library"), schema_definition(20, "Kullanılan şema"));
    json_array_add_object_element(json_object_get_array_member(root, "rotation_library"), schema_definition(30, "Boş kalmış şema"));
    /* Another day's programme names the first schema (the selected one has its own rotation). */
    gchar *text = json_to_string(json_array_get_element(json_object_get_array_member(root, "events"), 0), FALSE);
    JsonNode *copy = json_from_string(text, NULL); g_free(text); JsonObject *other = json_object_ref(json_node_get_object(copy)); json_node_free(copy);
    set_id(other, "id", 41); json_object_set_string_member(other, "date", "2026-10-13");
    JsonArray *other_rows = json_array_new(); JsonObject *assignment = json_object_new();
    set_id(assignment, "id", 40); set_id(assignment, "rotation_id", 20); json_array_add_object_element(other_rows, assignment);
    json_object_set_array_member(other, "rotations", other_rows);
    json_array_add_object_element(json_object_get_array_member(root, "events"), other);
    radios_automation_calendar_set_snapshot(f.page, root); json_node_free(snapshot); pump(); f.changes = 0;
    gtk_button_clicked(GTK_BUTTON(find(f.page, "automation-calendar-create-rotation-schema"))); pump();
    GtkWidget *dialog = toplevel_named("automation-calendar-schema-settings-dialog"); g_assert_true(GTK_IS_DIALOG(dialog));
    GtkComboBox *choice = GTK_COMBO_BOX(find(dialog, "automation-calendar-schema-settings-choice"));
    gchar *caption = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(choice)); g_assert_cmpstr(caption, ==, "Kullanılan şema · 1 adım"); g_free(caption);
    gtk_combo_box_set_active(choice, 0); gtk_dialog_response(GTK_DIALOG(dialog), 1); pump();
    g_assert_true(toplevel_named("automation-calendar-schema-settings-dialog") == dialog); g_assert_cmpuint(f.changes, ==, 0);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(dialog, "automation-calendar-schema-status"))), "1 yerde kullanılıyor"));
    gtk_combo_box_set_active(choice, 1); gtk_dialog_response(GTK_DIALOG(dialog), 1); pump();
    g_assert_cmpuint(f.changes, ==, 1);
    JsonNode *after = radios_automation_calendar_snapshot(f.page);
    JsonArray *library = json_object_get_array_member(json_node_get_object(after), "rotation_library");
    g_assert_cmpuint(json_array_get_length(library), ==, 2);
    for (guint i = 0; i < json_array_get_length(library); ++i)
        g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(library, i), "name"), !=, "Boş kalmış şema");
    /* With one schema left the button opens it directly. */
    GtkWidget *settings = toplevel_named("automation-calendar-library-dialog"); g_assert_true(GTK_IS_DIALOG(settings));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(settings, "rotation-editor-name"))), ==, "Kullanılan şema");
    json_node_free(after); teardown(&f);
}
static void test_player_schema_creation_reports_busy(void)
{
    Fixture f; setup(&f, 1); gtk_widget_set_name(f.page, "automation-calendar-main");
    GError *error = NULL;
    g_assert_false(radios_automation_calendar_create_player_schema(&f.ctx, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
    GtkWidget *dialog = open_library_editor(&f);
    const gchar *playlist_id = "10000000-0000-4000-8000-000000001100";
    g_assert_false(radios_automation_calendar_create_player_schema(&f.ctx, playlist_id, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY); g_clear_error(&error);
    g_assert_cmpuint(f.changes, ==, 0); gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump();
    g_assert_true(radios_automation_calendar_create_player_schema(&f.ctx, playlist_id, &error)); g_assert_no_error(error);
    JsonNode *snapshot = radios_automation_calendar_snapshot(f.page);
    JsonArray *library = json_object_get_array_member(json_node_get_object(snapshot), "rotation_library");
    g_assert_cmpuint(json_array_get_length(library), ==, 2);
    JsonArray *sources = json_object_get_array_member(json_array_get_object_element(library, 1), "sources");
    g_assert_cmpuint(json_array_get_length(sources), ==, 1);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(sources, 0), "playlist_id", ""), ==, playlist_id);
    g_assert_cmpuint(f.changes, ==, 1); json_node_free(snapshot); teardown(&f);
}
static GtkWidget *grid_menu_item(Fixture *f, const gchar *name)
{
    GList *menus = gtk_menu_get_for_attach_widget(find(f->page, "automation-calendar-grid")); GtkWidget *item = NULL;
    for (GList *it = menus; it && !item; it = it->next) item = find(it->data, name);
    g_assert_true(GTK_IS_MENU_ITEM(item)); return item;
}
/* Kullanıcı isteği U4: the live strips are chosen in a folding tree. */
static GtkWidget *live_channels(GtkWidget *dialog)
{ GtkWidget *picker = find(dialog, "automation-calendar-event-live-channels"); g_assert_true(GTK_IS_MENU_BUTTON(picker)); return picker; }
static gboolean live_channel_chosen(GtkWidget *dialog, guint number)
{
    JsonArray *chosen = radios_ui_channel_picker_selected(live_channels(dialog)); gboolean found = FALSE;
    for (guint i = 0; i < json_array_get_length(chosen); ++i) found |= json_array_get_int_element(chosen, i) == number;
    json_array_unref(chosen); return found;
}
static const gchar *rotation_title(Fixture *f)
{ return gtk_label_get_text(GTK_LABEL(find(f->page, "automation-calendar-rotation-title"))); }
static void test_live_event_dialog_panel_and_schema_list(void)
{
    Fixture f; setup(&f, 1);
    JsonNode *snapshot = radios_automation_calendar_snapshot(f.page);
    json_array_add_object_element(json_object_get_array_member(json_node_get_object(snapshot), "rotation_library"), schema_definition(20, "Konuşma şeması"));
    radios_automation_calendar_set_snapshot(f.page, json_node_get_object(snapshot)); json_node_free(snapshot);
    gtk_widget_show_all(f.window); pump();
    radios_automation_calendar_select(f.page, 2, 720, 780, FALSE); pump();
    GtkWidget *create = find(f.page, "automation-calendar-save-selection"); g_assert_true(gtk_widget_is_sensitive(create));
    gtk_button_clicked(GTK_BUTTON(create)); pump();
    GtkWidget *dialog = toplevel_named("automation-calendar-event-dialog"); g_assert_true(GTK_IS_DIALOG(dialog));
    GtkWidget *live = find(dialog, "automation-calendar-event-live"), *panel = find(dialog, "automation-calendar-event-live-panel");
    g_assert_true(GTK_IS_CHECK_BUTTON(live)); g_assert_nonnull(panel);
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(live))); g_assert_false(gtk_widget_get_visible(panel));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(live), TRUE); pump();
    g_assert_true(gtk_widget_get_visible(panel)); g_assert_true(gtk_widget_get_mapped(panel));
    pump_until_allocated(panel);
    /* The live settings sit to the right of the programme fields. */
    GtkWidget *name = find(dialog, "automation-calendar-event-name"); GtkAllocation name_box; gint name_x = 0, panel_x = 0;
    gtk_widget_get_allocation(name, &name_box);
    g_assert_true(gtk_widget_translate_coordinates(name, dialog, 0, 0, &name_x, NULL));
    g_assert_true(gtk_widget_translate_coordinates(panel, dialog, 0, 0, &panel_x, NULL));
    g_assert_cmpint(panel_x, >=, name_x + name_box.width);
    const gchar *controls[] = {"automation-calendar-event-live-label", "automation-calendar-event-live-schema", "automation-calendar-event-live-end",
        "automation-calendar-event-live-duration", "automation-calendar-event-live-channels"};
    for (guint i = 0; i < G_N_ELEMENTS(controls); ++i) {
        GtkWidget *widget = find(dialog, controls[i]); g_assert_nonnull(widget);
        g_assert_true(gtk_widget_get_visible(widget)); g_assert_true(gtk_widget_is_ancestor(widget, panel));
    }
    /* Only automation_schema definitions are offered as the live event schema. */
    GtkWidget *schema = find(dialog, "automation-calendar-event-live-schema");
    g_assert_cmpint(gtk_tree_model_iter_n_children(gtk_combo_box_get_model(GTK_COMBO_BOX(schema)), NULL), ==, 2);
    g_assert_false(gtk_combo_box_set_active_id(GTK_COMBO_BOX(schema), ROTATION_ID));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(schema), SCHEMA_ID));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(live), FALSE); pump(); g_assert_false(gtk_widget_get_visible(panel));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(live), TRUE); pump(); g_assert_true(gtk_widget_get_visible(panel));
    gtk_entry_set_text(GTK_ENTRY(name), "Sabah sohbeti");
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(dialog, "automation-calendar-event-live-label")), "dj"));
    radios_ui_channel_picker_toggle(live_channels(dialog), 2); radios_ui_channel_picker_toggle(live_channels(dialog), 5); pump();
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(dialog, "automation-calendar-event-live-end")), "duration"));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(find(dialog, "automation-calendar-event-live-duration")), 45);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); pump();
    g_assert_null(toplevel_named("automation-calendar-event-dialog")); g_assert_cmpuint(f.changes, ==, 1);
    snapshot = radios_automation_calendar_snapshot(f.page);
    JsonArray *array = json_object_get_array_member(json_node_get_object(snapshot), "events"); g_assert_cmpuint(json_array_get_length(array), ==, 2);
    JsonObject *event = json_array_get_object_element(array, 1); g_assert_cmpstr(radios_json_string(event, "title", ""), ==, "Sabah sohbeti");
    JsonObject *saved = json_object_get_object_member(event, "live"); g_assert_nonnull(saved);
    g_assert_cmpstr(radios_json_string(saved, "label", ""), ==, "dj");
    JsonArray *channels = json_object_get_array_member(saved, "channels"); g_assert_cmpuint(json_array_get_length(channels), ==, 2);
    g_assert_cmpint(json_array_get_int_element(channels, 0), ==, 2); g_assert_cmpint(json_array_get_int_element(channels, 1), ==, 5);
    g_assert_cmpstr(radios_json_string(saved, "automation_id", ""), ==, SCHEMA_ID);
    g_assert_cmpstr(radios_json_string(saved, "end_rule", ""), ==, "duration");
    g_assert_cmpint(radios_json_int(saved, "duration_ms", 0), ==, 45 * 60000);
    JsonArray *rows = json_object_get_array_member(event, "rotations"); g_assert_cmpuint(json_array_get_length(rows), ==, 1);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(rows, 0), "rotation_id", ""), ==, SCHEMA_ID);
    json_node_free(snapshot);
    /* Kılavuz §04 adım 2: the live programme offers rotation schemas only. */
    g_assert_cmpstr(rotation_title(&f), ==, "2  Rotasyon şemaları");
    GtkTreeModel *library = gtk_tree_view_get_model(GTK_TREE_VIEW(find(f.page, "automation-calendar-available-rotations")));
    g_assert_cmpint(gtk_tree_model_iter_n_children(library, NULL), ==, 1);
    GtkTreeIter first; gchar *id = NULL; g_assert_true(gtk_tree_model_get_iter_first(library, &first)); gtk_tree_model_get(library, &first, 0, &id, -1);
    g_assert_cmpstr(id, ==, SCHEMA_ID); g_free(id);
    radios_automation_calendar_select(f.page, 0, 600, 601, FALSE); pump();
    g_assert_cmpstr(rotation_title(&f), ==, "2  Rotasyonlar"); g_assert_cmpint(gtk_tree_model_iter_n_children(library, NULL), ==, 2);
    /* "Program özellikleri" reopens the panel with the saved values. */
    radios_automation_calendar_select(f.page, 2, 720, 780, FALSE); pump();
    gtk_menu_item_activate(GTK_MENU_ITEM(grid_menu_item(&f, "automation-calendar-programme-properties"))); pump();
    dialog = toplevel_named("automation-calendar-event-dialog"); g_assert_true(GTK_IS_DIALOG(dialog));
    g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(dialog)), ==, "Program özellikleri");
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(dialog, "automation-calendar-event-live"))));
    g_assert_true(gtk_widget_get_visible(find(dialog, "automation-calendar-event-live-panel")));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(dialog, "automation-calendar-event-live-label"))), ==, "dj");
    g_assert_true(live_channel_chosen(dialog, 2)); g_assert_true(live_channel_chosen(dialog, 5)); g_assert_false(live_channel_chosen(dialog, 1));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(dialog, "automation-calendar-event-live-schema"))), ==, SCHEMA_ID);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(dialog, "automation-calendar-event-live-end"))), ==, "duration");
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(find(dialog, "automation-calendar-event-live-duration"))), ==, 45);
    /* Unchecking the box removes the live settings on apply. */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(dialog, "automation-calendar-event-live")), FALSE);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); pump(); g_assert_cmpuint(f.changes, ==, 2);
    snapshot = radios_automation_calendar_snapshot(f.page);
    event = json_array_get_object_element(json_object_get_array_member(json_node_get_object(snapshot), "events"), 1);
    g_assert_false(json_object_has_member(event, "live")); json_node_free(snapshot);
    g_assert_cmpstr(rotation_title(&f), ==, "2  Rotasyonlar");
    teardown(&f);
}
/* Kılavuz §02: "Mevcut şemayı uygula" sits beside "Şema Ayarları"
 * and reports in Turkish when no general programme is playing. */
static void test_apply_schema_without_running_programme(void)
{
    Fixture f; setup(&f, 1);
    GtkWidget *button = find(f.page, "automation-calendar-apply-schema"); g_assert_true(GTK_IS_BUTTON(button));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(button)), ==, "Mevcut şemayı uygula");
    g_assert_true(gtk_widget_get_parent(button) == gtk_widget_get_parent(find(f.page, "automation-calendar-create-rotation-schema")));
    JsonNode *before = radios_automation_calendar_snapshot(f.page);
    gtk_button_clicked(GTK_BUTTON(button)); pump();
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(f.page, "automation-calendar-status"))), ==, "Çalan genel program yok.");
    JsonNode *after = radios_automation_calendar_snapshot(f.page); g_assert_true(json_node_equal(before, after));
    g_assert_cmpuint(f.changes, ==, 0); json_node_free(before); json_node_free(after); teardown(&f);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); if (!gtk_init_check(&argc, &argv)) return 77;
    g_test_add_func("/calendar-regressions/detached-rotation-edit-reopen", test_detached_rotation_edit_survives_reopen);
    g_test_add_func("/calendar-regressions/snapshot-reconciles-rotation-editor", test_snapshot_reconciles_open_rotation_editor);
    g_test_add_func("/calendar-regressions/programme-switch-closes-rotation-editor", test_programme_switch_closes_rotation_editor);
    g_test_add_func("/calendar-regressions/schema-settings-cancel-no-create", test_schema_settings_cancel_does_not_create);
    g_test_add_func("/calendar-regressions/schema-settings-reopen-no-duplicates", test_schema_settings_reopens_existing_without_duplicates);
    g_test_add_func("/calendar-regressions/schema-settings-explicit-create", test_schema_settings_explicit_create_once);
    g_test_add_func("/calendar-regressions/schema-settings-create-second", test_schema_settings_can_create_second_schema);
    g_test_add_func("/calendar-regressions/schema-settings-delete-unused-keep-used", test_schema_settings_delete_unused_keep_used);
    g_test_add_func("/calendar-regressions/player-schema-create-busy", test_player_schema_creation_reports_busy);
    g_test_add_func("/calendar-regressions/live-event-dialog-panel-schema-list", test_live_event_dialog_panel_and_schema_list);
    g_test_add_func("/calendar-regressions/apply-schema-without-running-programme", test_apply_schema_without_running_programme);
    g_test_add_func("/calendar-regressions/dnd-order-refresh", test_dnd_order_survives_refresh);
    g_test_add_func("/calendar-regressions/dnd-before-selection-refresh", test_dnd_order_before_selection_refresh);
    g_test_add_func("/calendar-regressions/pending-dnd-close", test_pending_dnd_close);
    g_test_add_func("/calendar-regressions/paste-expanded-playlist-limit", test_paste_expanded_playlist_limit);
    g_test_add_func("/calendar-regressions/simple-programme-inline-probability", test_simple_programme_and_inline_probability);
    return g_test_run();
}
