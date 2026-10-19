#include "radios/schema_settings_ui.h"
#include "radios/transition_preset.h"
#include "radios/automation_calendar_plan.h"
#include "radios/widgets.h"
#include <string.h>

#define DEFINITION "11111111-1111-4111-8111-111111111111"
#define SOURCE_A "22222222-2222-4222-8222-222222222222"
#define SOURCE_B "33333333-3333-4333-8333-333333333333"
#define PLAYLIST_A "44444444-4444-4444-8444-444444444444"
#define PLAYLIST_B "55555555-5555-4555-8555-555555555555"
#define PRESET "a1e5c6d0-5f3b-4b8e-9c1a-0d2f4e6a8b10"
typedef struct { GtkWidget *window, *editor; JsonNode *saved; guint changes; } Fixture;
static JsonNode *parse(const gchar *text)
{
    JsonParser *parser = json_parser_new(); GError *error = NULL;
    g_assert_true(json_parser_load_from_data(parser, text, -1, &error)); g_assert_no_error(error);
    JsonNode *result = json_parser_steal_root(parser); g_object_unref(parser); return result;
}
static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (!g_strcmp0(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *field(Fixture *fixture, const gchar *suffix)
{
    gchar *name = g_strconcat("schema-settings-", suffix, NULL); GtkWidget *widget = find(fixture->editor, name); g_free(name); g_assert_nonnull(widget); return widget;
}
static void choose(Fixture *fixture, const gchar *suffix, const gchar *id)
{ g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(field(fixture, suffix)), id)); }
static void click(Fixture *fixture, const gchar *suffix)
{ gtk_button_clicked(GTK_BUTTON(field(fixture, suffix))); }
static void value(Fixture *fixture, const gchar *suffix, gint number)
{ gtk_spin_button_set_value(GTK_SPIN_BUTTON(field(fixture, suffix)), number); }
static void selected(Fixture *fixture, const gchar *suffix, gint row)
{
    GtkTreePath *path = gtk_tree_path_new_from_indices(row, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(field(fixture, suffix))), path); gtk_tree_path_free(path);
}
static void changed(GtkWidget *editor, JsonNode *definition, gpointer data)
{
    (void)editor; Fixture *fixture = data; g_clear_pointer(&fixture->saved, json_node_free);
    gchar *text = json_to_string(definition, FALSE); fixture->saved = parse(text); g_free(text); ++fixture->changes;
    /* The parent can immediately acknowledge/reapply the exact snapshot. */
    radios_schema_settings_set_config(fixture->editor, json_node_get_object(definition));
}
static JsonObject *settings(Fixture *fixture)
{ return json_object_get_object_member(json_node_get_object(fixture->saved), "settings"); }
static JsonObject *row(JsonObject *owner, const gchar *key, guint index)
{ return json_array_get_object_element(json_object_get_array_member(owner, key), index); }
static void assert_runtime_contract(Fixture *fixture)
{
    JsonNode *calendar = parse("{\"schema\":1,\"timezone\":\"UTC\",\"rotation_library\":[],\"events\":[{"
        "\"id\":\"66666666-6666-4666-8666-666666666666\",\"title\":\"Yayın\",\"date\":\"2026-10-18\",\"color\":\"#123456\","
        "\"start_minute\":600,\"end_minute\":660,\"rotations\":[{\"id\":\"77777777-7777-4777-8777-777777777777\",\"rotation_id\":\"" DEFINITION "\"}]}]}");
    JsonObject *root = json_node_get_object(calendar);
    json_array_add_element(json_object_get_array_member(root, "rotation_library"), radios_schema_settings_snapshot(fixture->editor));
    GError *error = NULL; JsonNode *resolved = radios_automation_calendar_resolve_event(root, row(root, "events", 0), &error);
    g_assert_no_error(error); g_assert_nonnull(resolved); json_node_free(resolved); json_node_free(calendar);
}
static void setup(Fixture *fixture)
{
    memset(fixture, 0, sizeof *fixture);
    JsonNode *input = parse("{\"id\":\"" DEFINITION "\",\"name\":\"Deneme şeması\",\"sources\":["
        "{\"id\":\"" SOURCE_A "\",\"playlist_id\":\"" PLAYLIST_A "\",\"frequency\":1,\"count\":1,\"weight\":100,"
        "\"start_minute\":0,\"end_minute\":1440,\"interval_minutes\":0,\"random\":false,\"track_random\":false,\"advert_random\":false,\"sweeper_random\":false,"
        "\"settings\":{\"track_gap_minutes\":17}},"
        "{\"id\":\"" SOURCE_B "\",\"playlist_id\":\"" PLAYLIST_B "\",\"frequency\":1,\"count\":1,\"weight\":100,"
        "\"start_minute\":0,\"end_minute\":1440,\"interval_minutes\":0,\"random\":false,\"track_random\":false,\"advert_random\":false,\"sweeper_random\":false}],"
        "\"settings\":{\"automation_schema\":true,\"selection_mode\":\"linear\",\"track_gap_minutes\":5,\"artist_gap_minutes\":10,\"album_gap_minutes\":0,"
        "\"transitions\":[{\"mode\":0,\"weight\":100,\"overlap_ms\":0}],\"autodj_players\":{\"music\":[12,31],\"advertisement\":[12,31],\"jingle\":[12,31],\"sweeper\":[12,31]},\"excluded_kinds\":[\"advertisement\"]}}");
    JsonObject *global = json_object_get_object_member(json_node_get_object(input), "settings");
    json_object_set_array_member(global, "autodj_presets", radios_transition_preset_templates());
    fixture->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(fixture->window);
    fixture->editor = radios_schema_settings_new(json_node_get_object(input), changed, fixture); gtk_container_add(GTK_CONTAINER(fixture->window), fixture->editor);
    JsonNode *catalogue = parse("[{\"_id\":\"" PLAYLIST_A "\",\"name\":\"A playlist\"},{\"_id\":\"" PLAYLIST_B "\",\"name\":\"B playlist\"}]");
    radios_schema_settings_set_playlists(fixture->editor, json_node_get_array(catalogue)); json_node_free(catalogue);
    g_assert_cmpuint(fixture->changes, ==, 0);
    JsonNode *copy = radios_schema_settings_snapshot(fixture->editor); g_assert_true(json_node_equal(input, copy)); json_node_free(copy);
    json_object_set_string_member(json_node_get_object(input), "name", "external mutation"); json_node_free(input);
}
static void teardown(Fixture *fixture)
{
    gtk_widget_destroy(fixture->window); g_object_unref(fixture->window); g_clear_pointer(&fixture->saved, json_node_free);
}
static void add_inner(Fixture *fixture, const gchar *target, gint order, const gchar *kind, const gchar *automation)
{
    choose(fixture, "step-target", target); click(fixture, "step-new"); value(fixture, "step-order", order);
    choose(fixture, "step-kind", kind); choose(fixture, "step-automation", automation); click(fixture, "step-save");
}
static void typed_schema_and_playlist_records_are_separate(void)
{
    Fixture fixture; setup(&fixture);
    add_inner(&fixture, "schema", 2, "music", PRESET);
    add_inner(&fixture, SOURCE_A, 2, "music", "inherit");
    add_inner(&fixture, SOURCE_B, 2, "music", "inherit");
    g_assert_cmpuint(fixture.changes, ==, 3);
    JsonObject *schema = row(settings(&fixture), "automation_steps", 0);
    g_assert_cmpint(json_object_get_int_member(schema, "order"), ==, 2);
    g_assert_cmpstr(json_object_get_string_member(schema, "preset_id"), ==, PRESET);
    JsonArray *sources = json_object_get_array_member(json_node_get_object(fixture.saved), "sources");
    for (guint i = 0; i < 2; ++i) {
        JsonObject *source_settings = json_object_get_object_member(json_array_get_object_element(sources, i), "settings");
        JsonObject *step = row(source_settings, "automation_steps", 0);
        g_assert_cmpint(json_object_get_int_member(step, "order"), ==, 2);
        g_assert_cmpstr(json_object_get_string_member(step, "kind"), ==, "music");
        g_assert_false(json_object_has_member(step, "preset_id"));
    }
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(json_array_get_object_element(sources, 0), "settings"), "track_gap_minutes"), ==, 17);
    g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(json_object_get_object_member(settings(&fixture), "autodj_players"), "music"), 1), ==, 31);
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(fixture.saved), "name"), ==, "Deneme şeması");
    assert_runtime_contract(&fixture);
    choose(&fixture, "step-target", "schema"); selected(&fixture, "steps", 0);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "step-automation"))), ==, PRESET);
    click(&fixture, "step-new"); value(&fixture, "step-order", 2); click(&fixture, "step-save");
    g_assert_cmpuint(fixture.changes, ==, 3); g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(field(&fixture, "status"))), "zaten"));
    selected(&fixture, "steps", 0); click(&fixture, "step-remove");
    g_assert_false(json_object_has_member(settings(&fixture), "automation_steps"));
    g_assert_true(json_object_has_member(json_object_get_object_member(row(json_node_get_object(fixture.saved), "sources", 0), "settings"), "automation_steps"));
    teardown(&fixture);
}
static void links_assign_and_preserve_existing_automations(void)
{
    Fixture fixture; setup(&fixture); choose(&fixture, "link-from", SOURCE_A); choose(&fixture, "link-to", SOURCE_B);
    choose(&fixture, "link-automation", PRESET); click(&fixture, "link-save"); g_assert_cmpuint(fixture.changes, ==, 1);
    JsonObject *link = row(settings(&fixture), "link_transitions", 0);
    g_assert_cmpstr(json_object_get_string_member(link, "from_source_id"), ==, SOURCE_A);
    g_assert_cmpstr(json_object_get_string_member(link, "to_source_id"), ==, SOURCE_B);
    g_assert_cmpstr(json_object_get_string_member(link, "preset_id"), ==, PRESET);
    assert_runtime_contract(&fixture);
    click(&fixture, "link-new"); choose(&fixture, "link-from", SOURCE_A); choose(&fixture, "link-to", SOURCE_B); click(&fixture, "link-save");
    g_assert_cmpuint(fixture.changes, ==, 1);
    selected(&fixture, "links", 0); choose(&fixture, "link-automation", "simple");
    gtk_combo_box_set_active(GTK_COMBO_BOX(field(&fixture, "link-mode")), 1); value(&fixture, "link-overlap", 3500); click(&fixture, "link-save");
    g_assert_cmpuint(fixture.changes, ==, 2); link = row(settings(&fixture), "link_transitions", 0);
    g_assert_false(json_object_has_member(link, "preset_id"));
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(link, "transition"), "overlap_ms"), ==, 3500);
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "link-overlap")), "3."); click(&fixture, "link-save"); g_assert_cmpuint(fixture.changes, ==, 2);
    value(&fixture, "link-overlap", 2000); gtk_combo_box_set_active(GTK_COMBO_BOX(field(&fixture, "link-mode")), 0); click(&fixture, "link-save");
    link = row(settings(&fixture), "link_transitions", 0);
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(link, "transition"), "overlap_ms"), ==, 0);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(settings(&fixture), "autodj_presets")), ==, 8);
    assert_runtime_contract(&fixture);
    click(&fixture, "link-remove"); g_assert_false(json_object_has_member(settings(&fixture), "link_transitions")); teardown(&fixture);
}
static void programme_entry_alternatives_keep_default_remainder(void)
{
    Fixture fixture; setup(&fixture); choose(&fixture, "alternative-source", SOURCE_B); value(&fixture, "alternative-weight", 35);
    click(&fixture, "alternative-save"); g_assert_cmpuint(fixture.changes, ==, 0);
    choose(&fixture, "entry-source", SOURCE_A); choose(&fixture, "entry-automation", PRESET); click(&fixture, "entry-save");
    choose(&fixture, "alternative-source", SOURCE_B); value(&fixture, "alternative-weight", 35); choose(&fixture, "alternative-automation", "simple");
    gtk_combo_box_set_active(GTK_COMBO_BOX(field(&fixture, "alternative-mode")), 1); value(&fixture, "alternative-overlap", 1800); click(&fixture, "alternative-save");
    g_assert_cmpuint(fixture.changes, ==, 2);
    JsonObject *entry = json_object_get_object_member(settings(&fixture), "programme_entry"), *alternative = row(entry, "alternatives", 0);
    g_assert_cmpint(json_object_get_int_member(entry, "source_order"), ==, 1);
    g_assert_cmpstr(json_object_get_string_member(entry, "preset_id"), ==, PRESET);
    g_assert_cmpint(json_object_get_int_member(alternative, "source_order"), ==, 2); g_assert_cmpint(json_object_get_int_member(alternative, "weight"), ==, 35);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(field(&fixture, "entry-summary"))), "%65"));
    assert_runtime_contract(&fixture);
    click(&fixture, "alternative-new"); choose(&fixture, "alternative-source", SOURCE_A); value(&fixture, "alternative-weight", 80); click(&fixture, "alternative-save");
    g_assert_cmpuint(fixture.changes, ==, 2);
    choose(&fixture, "entry-source", SOURCE_B); click(&fixture, "entry-save");
    entry = json_object_get_object_member(settings(&fixture), "programme_entry");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(entry, "alternatives")), ==, 1);
    selected(&fixture, "alternatives", 0); click(&fixture, "alternative-remove");
    entry = json_object_get_object_member(settings(&fixture), "programme_entry"); g_assert_false(json_object_has_member(entry, "alternatives"));
    g_assert_cmpint(json_object_get_int_member(entry, "source_order"), ==, 2);
    click(&fixture, "entry-remove"); g_assert_false(json_object_has_member(settings(&fixture), "programme_entry")); teardown(&fixture);
}
static void snapshot_ack_isolated_and_missing_preset_rejected(void)
{
    Fixture fixture; setup(&fixture); add_inner(&fixture, "schema", 2, "talk", PRESET);
    JsonNode *snapshot = radios_schema_settings_snapshot(fixture.editor);
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "step-order")), "");
    radios_schema_settings_set_config(fixture.editor, json_node_get_object(snapshot));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "step-order"))), ==, "");
    JsonObject *global = json_object_get_object_member(json_node_get_object(snapshot), "settings");
    json_array_remove_element(json_object_get_array_member(global, "autodj_presets"), 0);
    radios_schema_settings_set_config(fixture.editor, json_node_get_object(snapshot)); json_node_free(snapshot);
    selected(&fixture, "steps", 0); click(&fixture, "step-save"); g_assert_cmpuint(fixture.changes, ==, 1);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(field(&fixture, "status"))), "bulunamıyor"));
    choose(&fixture, "step-automation", "simple"); click(&fixture, "step-save"); g_assert_cmpuint(fixture.changes, ==, 2);
    snapshot = radios_schema_settings_snapshot(fixture.editor); json_object_set_string_member(json_node_get_object(snapshot), "name", "mutated snapshot"); json_node_free(snapshot);
    snapshot = radios_schema_settings_snapshot(fixture.editor);
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(snapshot), "name"), ==, "Deneme şeması"); json_node_free(snapshot);
    radios_schema_settings_set_playlists(fixture.editor, NULL); teardown(&fixture);
}
static void route_selected(GtkWidget *editor, const gchar *source_id, gpointer user)
{
    (void)editor; gchar **captured = user; g_free(*captured); *captured = g_strdup(source_id);
}
static void routing_handler_targets_playlist_step(void)
{
    Fixture fixture; setup(&fixture); gchar *routed = NULL;
    g_assert_false(gtk_widget_get_sensitive(field(&fixture, "step-routing")));
    radios_schema_settings_set_routing_handler(fixture.editor, route_selected, &routed);
    g_assert_false(gtk_widget_get_sensitive(field(&fixture, "step-routing")));
    choose(&fixture, "step-target", SOURCE_B); g_assert_true(gtk_widget_get_sensitive(field(&fixture, "step-routing")));
    click(&fixture, "step-routing"); g_assert_cmpstr(routed, ==, SOURCE_B); g_assert_cmpuint(fixture.changes, ==, 0);
    JsonNode *snapshot = radios_schema_settings_snapshot(fixture.editor);
    JsonObject *source = row(json_node_get_object(snapshot), "sources", 1); json_object_set_string_member(source, "source_type", "live");
    radios_schema_settings_set_config(fixture.editor, json_node_get_object(snapshot)); json_node_free(snapshot);
    g_assert_false(gtk_widget_get_sensitive(field(&fixture, "step-routing")));
    choose(&fixture, "step-target", SOURCE_A); click(&fixture, "step-routing"); g_assert_cmpstr(routed, ==, SOURCE_A);
    radios_schema_settings_set_routing_handler(fixture.editor, NULL, NULL); g_assert_false(gtk_widget_get_sensitive(field(&fixture, "step-routing")));
    g_free(routed); teardown(&fixture);
}
static void catalogue_refresh_preserves_uncommitted_form(void)
{
    Fixture fixture; setup(&fixture);
    choose(&fixture, "step-target", SOURCE_A); choose(&fixture, "step-kind", "talk");
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "step-order")), "");
    choose(&fixture, "link-from", SOURCE_A); choose(&fixture, "link-to", SOURCE_B); choose(&fixture, "link-automation", "simple");
    gtk_combo_box_set_active(GTK_COMBO_BOX(field(&fixture, "link-mode")), 1);
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "link-overlap")), "");
    choose(&fixture, "entry-source", SOURCE_B); choose(&fixture, "alternative-source", SOURCE_A);
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "alternative-weight")), "");
    JsonNode *catalogue = parse("[{\"_id\":\"" PLAYLIST_A "\",\"name\":\"A playlist\"},{\"_id\":\"" PLAYLIST_B "\",\"name\":\"B playlist\"}]");
    for (guint i = 0; i < 2; ++i) {
        if (i) json_object_set_string_member(json_array_get_object_element(json_node_get_array(catalogue), 0), "name", "A playlist yeniden adlandırıldı");
        radios_schema_settings_set_playlists(fixture.editor, json_node_get_array(catalogue));
        g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "step-order"))), ==, "");
        g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "step-target"))), ==, SOURCE_A);
        g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "step-kind"))), ==, "talk");
        g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "link-to"))), ==, SOURCE_B);
        g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "link-automation"))), ==, "simple");
        g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "link-overlap"))), ==, "");
        g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "entry-source"))), ==, SOURCE_B);
        g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "alternative-weight"))), ==, "");
    }
    gchar *caption = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(field(&fixture, "step-target")));
    g_assert_nonnull(strstr(caption, "yeniden adlandırıldı")); g_free(caption);
    g_assert_cmpuint(fixture.changes, ==, 0); json_node_free(catalogue); teardown(&fixture);
}
static gboolean channel_chosen(Fixture *fixture, gint64 number)
{
    JsonArray *chosen = radios_ui_channel_picker_selected(field(fixture, "step-channels")); gboolean found = FALSE;
    for (guint i = 0; i < json_array_get_length(chosen); ++i) found |= json_array_get_int_element(chosen, i) == number;
    json_array_unref(chosen); return found;
}
static gchar *picker_rows(Fixture *fixture)
{
    /* The tree lives in the picker's popover, outside the editor's widget tree. */
    GtkWidget *popover = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(field(fixture, "step-channels"))));
    GtkWidget *tree = find(popover, "schema-settings-step-channels-tree"); g_assert_true(GTK_IS_TREE_VIEW(tree));
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree));
    GString *text = g_string_new(NULL); GtkTreeIter group, child;
    for (gboolean more = gtk_tree_model_get_iter_first(model, &group); more; more = gtk_tree_model_iter_next(model, &group)) {
        gchar *title = NULL; gtk_tree_model_get(model, &group, 2, &title, -1); g_string_append_printf(text, "[%s]", title); g_free(title);
        for (gboolean kids = gtk_tree_model_iter_children(model, &child, &group); kids; kids = gtk_tree_model_iter_next(model, &child)) {
            gchar *row_text = NULL; gtk_tree_model_get(model, &child, 2, &row_text, -1); g_string_append_printf(text, " %s;", row_text); g_free(row_text);
        }
    }
    return g_string_free(text, FALSE);
}
static const gchar *picker_caption(Fixture *fixture)
{ return gtk_label_get_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(field(fixture, "step-channels"))))); }
/* PDF s.4 adım 5 + kullanıcı isteği U4: the channels are chosen, not typed,
 * in a folding tree of only the defined strips grouped by their label. */
static void live_inner_record_channels_validate_and_round_trip(void)
{
    Fixture fixture; setup(&fixture);
    JsonNode *strips = parse("[{\"number\":12,\"name\":\"Stüdyo mik\",\"label\":\"talk\",\"source\":\"Stüdyo mikrofonu\"},"
        "{\"number\":32,\"name\":\"Kanal 32\",\"label\":\"dj\",\"source\":\"USB giriş\"},{\"number\":5,\"name\":\"Kanal 5\",\"label\":\"\",\"source\":\"Oynatıcı 5\"}]");
    radios_schema_settings_set_channels(fixture.editor, json_node_get_array(strips)); json_node_free(strips);
    gchar *rows = picker_rows(&fixture);
    g_assert_cmpstr(rows, ==, "[DJ] Kanal 32 · USB giriş;[Konuşma] Kanal 12 · Stüdyo mik · Stüdyo mikrofonu;[Etiketsiz] Kanal 5 · Oynatıcı 5;"); g_free(rows);
    g_assert_cmpstr(picker_caption(&fixture), ==, "Kanal seçilmedi ▾");
    choose(&fixture, "step-kind", "talk"); value(&fixture, "step-order", 2);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "step-channels-enabled")), TRUE);
    radios_ui_channel_picker_toggle(field(&fixture, "step-channels"), 32); radios_ui_channel_picker_toggle(field(&fixture, "step-channels"), 12);
    g_assert_cmpstr(picker_caption(&fixture), ==, "2 kanal seçili ▾");
    click(&fixture, "step-save");
    g_assert_cmpuint(fixture.changes, ==, 1); assert_runtime_contract(&fixture);
    JsonObject *record = row(settings(&fixture), "automation_steps", 0);
    g_assert_cmpstr(json_object_get_string_member(record, "kind"), ==, "talk");
    g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(record, "channels"), 0), ==, 12);
    g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(record, "channels"), 1), ==, 32);
    radios_ui_channel_picker_set_selected(field(&fixture, "step-channels"), NULL);
    gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(field(&fixture, "steps"))));
    selected(&fixture, "steps", 0);
    g_assert_true(channel_chosen(&fixture, 12)); g_assert_true(channel_chosen(&fixture, 32)); g_assert_false(channel_chosen(&fixture, 5));
    /* A saved channel the mixer no longer defines stays listed, not lost. */
    JsonNode *fewer = parse("[{\"number\":12,\"name\":\"Stüdyo mik\",\"label\":\"talk\",\"source\":\"Stüdyo mikrofonu\"}]");
    radios_schema_settings_set_channels(fixture.editor, json_node_get_array(fewer)); json_node_free(fewer);
    rows = picker_rows(&fixture); g_assert_nonnull(strstr(rows, "[Kayıtlı · tanımsız] Kanal 32")); g_free(rows);
    radios_ui_channel_picker_set_selected(field(&fixture, "step-channels"), NULL); click(&fixture, "step-save");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(row(settings(&fixture), "automation_steps", 0), "channels")), ==, 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "step-channels-enabled")), FALSE); click(&fixture, "step-save");
    g_assert_false(json_object_has_member(row(settings(&fixture), "automation_steps", 0), "channels")); assert_runtime_contract(&fixture);
    teardown(&fixture);
}
/* Kılavuz §02: the incoming step's playlist type decides which automation a
 * link may use. A preset assigned to jingles is offered only where a jingle
 * playlist enters; an unassigned preset stays available everywhere. A kept
 * choice that no longer fits is flagged and cannot be saved. */
static gboolean offered(Fixture *fixture, const gchar *suffix, const gchar *id)
{ return gtk_combo_box_set_active_id(GTK_COMBO_BOX(field(fixture, suffix)), id); }
static void link_presets_follow_playlist_kind(void)
{
    Fixture fixture; setup(&fixture);
    JsonNode *catalogue = parse("[{\"_id\":\"" PLAYLIST_A "\",\"name\":\"A playlist\",\"purpose\":\"music\"},"
        "{\"_id\":\"" PLAYLIST_B "\",\"name\":\"B playlist\",\"purpose\":\"jingle\"}]");
    radios_schema_settings_set_playlists(fixture.editor, json_node_get_array(catalogue)); json_node_free(catalogue);
    JsonNode *snapshot = radios_schema_settings_snapshot(fixture.editor);
    JsonObject *global = json_object_get_object_member(json_node_get_object(snapshot), "settings");
    gchar *typed = g_strdup(json_object_get_string_member(json_array_get_object_element(json_object_get_array_member(global, "autodj_presets"), 1), "id"));
    JsonObject *automation = json_object_new(), *kinds = json_object_new(), *jingle = json_object_new(); JsonArray *ids = json_array_new();
    json_array_add_string_element(ids, typed); json_object_set_array_member(jingle, "presets", ids); json_object_set_string_member(jingle, "rotation", "sequential");
    json_object_set_object_member(kinds, "jingle", jingle); json_object_set_boolean_member(automation, "enabled", TRUE);
    json_object_set_object_member(automation, "kinds", kinds); json_object_set_object_member(global, "autodj_automation", automation);
    radios_schema_settings_set_config(fixture.editor, json_node_get_object(snapshot)); json_node_free(snapshot);
    g_assert_cmpuint(fixture.changes, ==, 0);
    /* A → B: a jingle playlist enters; the jingle preset and the unassigned one are offered. */
    choose(&fixture, "link-from", SOURCE_A); choose(&fixture, "link-to", SOURCE_B);
    g_assert_true(offered(&fixture, "link-automation", PRESET)); g_assert_true(offered(&fixture, "link-automation", typed));
    click(&fixture, "link-save"); g_assert_cmpuint(fixture.changes, ==, 1);
    g_assert_cmpstr(json_object_get_string_member(row(settings(&fixture), "link_transitions", 0), "preset_id"), ==, typed);
    assert_runtime_contract(&fixture);
    /* B → A: music enters; the jingle preset is not offered. */
    click(&fixture, "link-new"); choose(&fixture, "link-from", SOURCE_B); choose(&fixture, "link-to", SOURCE_A);
    g_assert_false(offered(&fixture, "link-automation", typed)); g_assert_true(offered(&fixture, "link-automation", PRESET));
    /* A fitting choice made for B stays visible, flagged, when A becomes the incoming step; it cannot be saved. */
    choose(&fixture, "link-to", SOURCE_B); choose(&fixture, "link-automation", typed); choose(&fixture, "link-to", SOURCE_A);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "link-automation"))), ==, typed);
    gchar *caption = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(field(&fixture, "link-automation")));
    g_assert_nonnull(strstr(caption, "Müzik türüne atanmamış")); g_free(caption);
    click(&fixture, "link-save"); g_assert_cmpuint(fixture.changes, ==, 1);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(field(&fixture, "status"))), "atanmamış"));
    choose(&fixture, "link-automation", PRESET); click(&fixture, "link-save"); g_assert_cmpuint(fixture.changes, ==, 2);
    g_assert_cmpstr(json_object_get_string_member(row(settings(&fixture), "link_transitions", 1), "preset_id"), ==, PRESET);
    /* An inner record's kind filters the same way. */
    choose(&fixture, "step-target", "schema"); click(&fixture, "step-new"); choose(&fixture, "step-kind", "music");
    g_assert_false(offered(&fixture, "step-automation", typed));
    choose(&fixture, "step-kind", "jingle"); g_assert_true(offered(&fixture, "step-automation", typed));
    value(&fixture, "step-order", 3); click(&fixture, "step-save"); g_assert_cmpuint(fixture.changes, ==, 3);
    JsonObject *record = row(settings(&fixture), "automation_steps", 0);
    g_assert_cmpstr(json_object_get_string_member(record, "kind"), ==, "jingle"); g_assert_cmpstr(json_object_get_string_member(record, "preset_id"), ==, typed);
    assert_runtime_contract(&fixture);
    g_free(typed); teardown(&fixture);
}
/* Kılavuz §02–§04 "Yayın sırası ve geçişler": the schema's outer order as one
 * table; per step the cluster (or one unclustered player) and the existing
 * automation into the next step, saved to sources[].autodj and
 * link_transitions; another rotation's preset is copied into the schema. */
#define NEIGHBOUR "abababab-abab-4bab-8bab-abababababab"
#define TYPED_SWEEPER "cdcdcdcd-cdcd-4dcd-8dcd-cdcdcdcdcdcd"
static gchar *flow_cell(Fixture *fixture, gint index, gint column)
{
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(field(fixture, "flow"))); GtkTreeIter iter; gchar *text = NULL;
    g_assert_true(gtk_tree_model_iter_nth_child(model, &iter, NULL, index)); gtk_tree_model_get(model, &iter, column, &text, -1); return text;
}
static void flow_cell_is(Fixture *fixture, gint index, gint column, const gchar *expected)
{ gchar *text = flow_cell(fixture, index, column); g_assert_cmpstr(text, ==, expected); g_free(text); }
static JsonArray *flow_players(void)
{
    JsonNode *node = parse("[{\"deck\":1,\"name\":\"Oynatıcı 1\",\"cluster_id\":\"\",\"cluster_name\":\"\"},"
        "{\"deck\":2,\"name\":\"Stüdyo\",\"cluster_id\":\"\",\"cluster_name\":\"\"},"
        "{\"deck\":3,\"name\":\"Oynatıcı 3\",\"cluster_id\":\"k1\",\"cluster_name\":\"Gece\"},"
        "{\"deck\":4,\"name\":\"Oynatıcı 4\",\"cluster_id\":\"k1\",\"cluster_name\":\"Gece\"}]");
    JsonArray *players = json_array_ref(json_node_get_array(node)); json_node_free(node); return players;
}
static JsonArray *flow_library(void)
{
    JsonArray *templates = radios_transition_preset_templates();
    JsonNode *neighbour = json_node_copy(json_array_get_element(templates, 1)), *typed = json_node_copy(json_array_get_element(templates, 2));
    json_object_set_string_member(json_node_get_object(neighbour), "id", NEIGHBOUR); json_object_set_string_member(json_node_get_object(neighbour), "name", "Komşu geçiş");
    json_object_set_string_member(json_node_get_object(typed), "id", TYPED_SWEEPER); json_object_set_string_member(json_node_get_object(typed), "name", "Sweeper girişi");
    JsonNode *library = parse("[{\"id\":\"" DEFINITION "\",\"name\":\"Bu şema\",\"settings\":{}},"
        "{\"id\":\"99999999-9999-4999-8999-999999999999\",\"name\":\"Komşu rotasyon\",\"settings\":{\"autodj_presets\":[],"
        "\"autodj_automation\":{\"enabled\":true,\"kinds\":{\"sweeper\":{\"presets\":[\"" TYPED_SWEEPER "\"]}}}}}]");
    JsonObject *other = json_object_get_object_member(json_array_get_object_element(json_node_get_array(library), 1), "settings");
    json_array_add_element(json_object_get_array_member(other, "autodj_presets"), neighbour);
    json_array_add_element(json_object_get_array_member(other, "autodj_presets"), typed);
    JsonArray *result = json_array_ref(json_node_get_array(library)); json_node_free(library); json_array_unref(templates); return result;
}
static void flow_table_assigns_players_and_existing_automations(void)
{
    Fixture fixture; setup(&fixture);
    JsonArray *players = flow_players(), *library = flow_library();
    radios_schema_settings_set_players(fixture.editor, players); radios_schema_settings_set_library(fixture.editor, library);
    json_array_unref(players); json_array_unref(library); g_assert_cmpuint(fixture.changes, ==, 0);
    g_assert_cmpint(gtk_notebook_get_current_page(GTK_NOTEBOOK(field(&fixture, "tabs"))), ==, 0);
    flow_cell_is(&fixture, 0, 1, "1"); flow_cell_is(&fixture, 0, 2, "A playlist"); flow_cell_is(&fixture, 0, 3, "Müzik");
    flow_cell_is(&fixture, 0, 4, "Şemanın AutoDJ oynatıcıları"); flow_cell_is(&fixture, 0, 5, "→ 2 · Atanmadı · varsayılan geçiş");
    flow_cell_is(&fixture, 1, 5, "→ 1 · Atanmadı · varsayılan geçiş");
    /* Step 1 to the "Gece" cluster, its link to step 2 with another rotation's automation. */
    selected(&fixture, "flow", 0);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(field(&fixture, "flow-next"))), ==, "2 · B playlist");
    g_assert_false(offered(&fixture, "flow-automation", TYPED_SWEEPER));
    choose(&fixture, "flow-players", "cluster:k1"); choose(&fixture, "flow-automation", NEIGHBOUR); click(&fixture, "flow-save");
    g_assert_cmpuint(fixture.changes, ==, 1);
    JsonObject *source = json_array_get_object_element(json_object_get_array_member(json_node_get_object(fixture.saved), "sources"), 0);
    const gchar *pool_kinds[] = {"music", "advertisement", "jingle", "sweeper"};
    for (guint k = 0; k < G_N_ELEMENTS(pool_kinds); ++k) {
        JsonArray *decks = json_object_get_array_member(json_object_get_object_member(source, "autodj"), pool_kinds[k]);
        g_assert_cmpuint(json_array_get_length(decks), ==, 2);
        g_assert_cmpint(json_array_get_int_element(decks, 0), ==, 3); g_assert_cmpint(json_array_get_int_element(decks, 1), ==, 4);
    }
    JsonObject *link = row(settings(&fixture), "link_transitions", 0);
    g_assert_cmpstr(json_object_get_string_member(link, "from_source_id"), ==, SOURCE_A);
    g_assert_cmpstr(json_object_get_string_member(link, "to_source_id"), ==, SOURCE_B);
    g_assert_cmpstr(json_object_get_string_member(link, "preset_id"), ==, NEIGHBOUR);
    /* The chosen existing automation is now the schema's own (the store and planner resolve only those). */
    JsonArray *own = json_object_get_array_member(settings(&fixture), "autodj_presets"); gboolean imported = FALSE;
    for (guint i = 0; i < json_array_get_length(own); ++i) imported |= !g_strcmp0(json_object_get_string_member(json_array_get_object_element(own, i), "id"), NEIGHBOUR);
    g_assert_true(imported);
    flow_cell_is(&fixture, 0, 4, "Küme Gece · Oynatıcı 3, 4"); flow_cell_is(&fixture, 0, 5, "→ 2 · Komşu geçiş");
    assert_runtime_contract(&fixture);
    /* Step 2: one unclustered player (its own name), and back to "Atanmadı" removes nothing that was not there. */
    selected(&fixture, "flow", 1); choose(&fixture, "flow-players", "player:2"); click(&fixture, "flow-save");
    g_assert_cmpuint(fixture.changes, ==, 2); flow_cell_is(&fixture, 1, 4, "Stüdyo");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(settings(&fixture), "link_transitions")), ==, 1);
    /* Clearing the first link and returning step 1 to the schema's players. */
    selected(&fixture, "flow", 0); choose(&fixture, "flow-players", "default"); choose(&fixture, "flow-automation", "inherit"); click(&fixture, "flow-save");
    g_assert_cmpuint(fixture.changes, ==, 3);
    source = json_array_get_object_element(json_object_get_array_member(json_node_get_object(fixture.saved), "sources"), 0);
    g_assert_false(json_object_has_member(source, "autodj")); g_assert_false(json_object_has_member(settings(&fixture), "link_transitions"));
    flow_cell_is(&fixture, 0, 5, "→ 2 · Atanmadı · varsayılan geçiş");
    assert_runtime_contract(&fixture); teardown(&fixture);
}
/* A live step plays nothing: no players to choose, its link still is. */
static void flow_live_step_has_no_players(void)
{
    Fixture fixture; setup(&fixture);
    JsonNode *snapshot = radios_schema_settings_snapshot(fixture.editor);
    JsonObject *live = json_array_get_object_element(json_object_get_array_member(json_node_get_object(snapshot), "sources"), 1);
    json_object_remove_member(live, "playlist_id"); json_object_set_string_member(live, "source_type", "live");
    json_object_set_object_member(live, "live", json_node_get_object(json_node_copy(parse("{\"title\":\"Sabah sohbeti\",\"label\":\"talk\",\"channels\":[3,4],\"automation_id\":\"\",\"end_rule\":\"manual\",\"duration_ms\":0}"))));
    radios_schema_settings_set_config(fixture.editor, json_node_get_object(snapshot)); json_node_free(snapshot);
    JsonArray *players = flow_players(); radios_schema_settings_set_players(fixture.editor, players); json_array_unref(players);
    flow_cell_is(&fixture, 1, 2, "Sabah sohbeti"); flow_cell_is(&fixture, 1, 3, "Live · Konuşma · elle"); flow_cell_is(&fixture, 1, 4, "—");
    selected(&fixture, "flow", 1);
    g_assert_false(gtk_widget_get_sensitive(field(&fixture, "flow-players"))); g_assert_false(gtk_widget_get_sensitive(field(&fixture, "flow-detail")));
    choose(&fixture, "flow-automation", PRESET); click(&fixture, "flow-save"); g_assert_cmpuint(fixture.changes, ==, 1);
    JsonObject *link = row(settings(&fixture), "link_transitions", 0);
    g_assert_cmpstr(json_object_get_string_member(link, "from_source_id"), ==, SOURCE_B); g_assert_cmpstr(json_object_get_string_member(link, "to_source_id"), ==, SOURCE_A);
    g_assert_false(json_object_has_member(json_array_get_object_element(json_object_get_array_member(json_node_get_object(fixture.saved), "sources"), 1), "autodj"));
    teardown(&fixture);
}
int main(int argc, char **argv)
{
    gtk_test_init(&argc, &argv, NULL);
    g_test_add_func("/schema-settings/flow-table-players-existing-automations", flow_table_assigns_players_and_existing_automations);
    g_test_add_func("/schema-settings/flow-live-step-no-players", flow_live_step_has_no_players);
    g_test_add_func("/schema-settings/link-presets-follow-playlist-kind", link_presets_follow_playlist_kind);
    g_test_add_func("/schema-settings/live-inner-record-channels", live_inner_record_channels_validate_and_round_trip);
    g_test_add_func("/schema-settings/typed-schema-source-orders", typed_schema_and_playlist_records_are_separate);
    g_test_add_func("/schema-settings/links-existing-automations", links_assign_and_preserve_existing_automations);
    g_test_add_func("/schema-settings/programme-entry-alternatives", programme_entry_alternatives_keep_default_remainder);
    g_test_add_func("/schema-settings/ack-isolation-missing-preset", snapshot_ack_isolated_and_missing_preset_rejected);
    g_test_add_func("/schema-settings/routing-target-playlist-step", routing_handler_targets_playlist_step);
    g_test_add_func("/schema-settings/catalogue-refresh-preserves-drafts", catalogue_refresh_preserves_uncommitted_form);
    return g_test_run();
}
