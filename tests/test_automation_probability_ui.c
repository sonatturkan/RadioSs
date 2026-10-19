#include <glib.h>
/* Reuse only the genuine private HTTP/config/theme setup and request helpers.
 * New cases create a focused rotation/data harness without old calendar UI
 * assumptions. No player transport or user installation is touched. */
static int probability_ui_run(void);
#define main radios_probability_fixture_main
#define g_test_run probability_ui_run
#include "test_automation_calendar.c"
#undef g_test_run
#undef main
#include "radios/probability_editor.h"
#include "radios/automation_policy.h"

static JsonNode *probability_copy(JsonNode *node)
{
    gchar *text = json_to_string(node, FALSE); JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, text, -1, NULL)); g_free(text);
    JsonNode *copy = json_parser_steal_root(parser); g_object_unref(parser); return copy;
}
typedef struct {
    Harness host;
    RadioSDocument *document;
    JsonNode *saved, *calendar;
    gchar *id, *document_id;
    guint changes;
} ProbabilityHarness;
static gboolean probability_connected(gpointer value)
{ return radios_data_connected(&((ProbabilityHarness *)value)->host.ctx); }
static gboolean probability_saved(gpointer value)
{
    ProbabilityHarness *h = value;
    return radios_data_document_ready(h->document) && radios_data_document_revision(h->document) > 0;
}
static JsonObject *probability_find_definition(JsonObject *calendar, const gchar *id)
{
    JsonNode *node = calendar ? json_object_get_member(calendar, "rotation_library") : NULL;
    JsonArray *library = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
    for (guint i = 0; library && i < json_array_get_length(library); ++i) {
        JsonObject *row = json_array_get_object_element(library, i);
        if (!id || !g_strcmp0(radios_json_string(row, "id", ""), id)) return row;
    }
    return NULL;
}
static JsonObject *probability_definition(JsonNode *record)
{
    JsonObject *definition = probability_find_definition(json_object_get_object_member(json_node_get_object(record), "config"), NULL);
    g_assert_nonnull(definition); return definition;
}
static void probability_apply(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *configuration, gpointer data)
{
    (void)ctx; ProbabilityHarness *h = data; JsonObject *definition = probability_find_definition(configuration, h->id);
    g_assert_nonnull(definition); JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, configuration);
    g_clear_pointer(&h->calendar, json_node_free); h->calendar = probability_copy(node); json_node_free(node);
    radios_rotation_editor_set_config(owner, definition);
}
static void probability_capture(GtkWidget *editor, JsonNode *snapshot, gpointer data)
{
    (void)editor; ProbabilityHarness *h = data; g_clear_pointer(&h->saved, json_node_free);
    h->saved = probability_copy(snapshot); ++h->changes;
    g_assert_nonnull(h->document); radios_data_document_dirty(h->document, TRUE);
    JsonArray *library = json_array_new(); json_array_add_element(library, probability_copy(snapshot));
    json_object_set_array_member(json_node_get_object(h->calendar), "rotation_library", library);
    g_assert_true(radios_data_document_autosave(h->document, h->calendar));
}
static JsonNode *probability_record(const gchar *id)
{
    JsonNode *body = body_new(); json_object_set_string_member(json_node_get_object(body), "kind", "automation_calendar");
    JsonNode *result = request("/v1/workflows/desktop/list", body); json_node_free(body);
    JsonArray *rows = json_object_get_array_member(json_node_get_object(result), "records"); JsonNode *copy = NULL;
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        JsonNode *row = json_array_get_element(rows, i);
        if (probability_find_definition(json_object_get_object_member(json_node_get_object(row), "config"), id)) { copy = probability_copy(row); break; }
    }
    json_node_free(result); return copy;
}
static JsonNode *probability_catalogue(void)
{ JsonNode *body = body_new(), *result = request("/v1/workflows/desktop-playlists/list", body); json_node_free(body); return result; }
static void probability_catalogue_install(ProbabilityHarness *h)
{
    JsonNode *catalogue = probability_catalogue();
    radios_rotation_editor_set_playlists(h->host.page, json_object_get_array_member(json_node_get_object(catalogue), "playlists")); json_node_free(catalogue);
}
static ProbabilityHarness *probability_harness_open(JsonObject *record)
{
    ProbabilityHarness *h = g_new0(ProbabilityHarness, 1); Harness *host = &h->host;
    host->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(host->window);
    gtk_window_set_default_size(GTK_WINDOW(host->window), 1150, 820); gtk_style_context_add_class(gtk_widget_get_style_context(host->window), "radios");
    host->ctx.config = &config; host->ctx.parent = GTK_WINDOW(host->window); host->ctx.notify = harness_notice; host->ctx.notify_data = host;
    host->ctx.runtime = radios_runtime_new(16, 32); active_runtime = host->ctx.runtime; host->ctx.data = radios_data_new(&host->ctx);
    radios_data_connect(&host->ctx); wait_until(probability_connected, h, "private genuine data connection");
    RadioSRotationEditorHooks hooks = {probability_capture, h, NULL}; host->page = radios_rotation_editor_new(&host->ctx, &hooks);
    gtk_container_add(GTK_CONTAINER(host->window), host->page); gtk_widget_show_all(host->window);
    if (record) {
        JsonObject *configuration = json_object_get_object_member(record, "config"), *definition = probability_find_definition(configuration, NULL);
        g_assert_nonnull(definition); radios_rotation_editor_set_config(host->page, definition);
        h->id = g_strdup(radios_json_string(definition, "id", "")); h->document_id = g_strdup(radios_json_string(record, "_id", ""));
        JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, configuration); h->calendar = probability_copy(node); json_node_free(node);
        h->document = radios_data_document_new_for_id(&host->ctx, host->page, "automation_calendar", "Probability native fixture", h->document_id, probability_apply, h);
    } else {
        JsonNode *snapshot = radios_rotation_editor_snapshot(host->page); g_assert_nonnull(snapshot);
        JsonObject *definition = json_node_get_object(snapshot); h->id = g_strdup(radios_json_string(definition, "id", "")); h->document_id = g_uuid_string_random();
        JsonObject *configuration = json_object_new(); json_object_set_int_member(configuration, "schema", 1);
        json_object_set_string_member(configuration, "timezone", "UTC"); json_object_set_array_member(configuration, "events", json_array_new());
        JsonArray *library = json_array_new(); json_array_add_element(library, probability_copy(snapshot)); json_object_set_array_member(configuration, "rotation_library", library);
        h->calendar = json_node_new(JSON_NODE_OBJECT); json_node_take_object(h->calendar, configuration);
        h->document = radios_data_document_new_record(&host->ctx, host->page, "automation_calendar", "Probability native fixture", h->document_id, probability_apply, h);
        g_assert_true(radios_data_document_autosave(h->document, h->calendar)); json_node_free(snapshot);
    }
    wait_until(probability_saved, h, "genuine acknowledged calendar-owned rotation definition"); probability_catalogue_install(h); return h;
}
static void probability_harness_close(ProbabilityHarness *h)
{
    radios_data_document_preserve_pending(h->document); gtk_widget_destroy(h->host.window); radios_data_close(h->host.ctx.data);
    active_runtime = NULL; radios_runtime_free(h->host.ctx.runtime); while (g_main_context_iteration(NULL, FALSE)) {}
    radios_data_unref(h->host.ctx.data); g_object_unref(h->host.window); g_clear_pointer(&h->saved, json_node_free);
    g_free(h->host.last_notice); g_free(h->id); g_free(h->document_id); g_clear_pointer(&h->calendar, json_node_free); g_free(h);
}
static JsonObject *probability_saved_config(ProbabilityHarness *h, JsonNode **owned)
{
    wait_until(probability_saved, h, "latest genuine policy ACK"); *owned = probability_record(h->id); g_assert_nonnull(*owned);
    return probability_definition(*owned);
}
static gboolean probability_create_closed(gpointer unused)
{ (void)unused; return dialog_find("rotation-editor-create-dialog") == NULL; }
static void probability_create_ui(ProbabilityHarness *h, const gchar *name, const gchar *purpose)
{
    click(h->host.page, "rotation-editor-create-playlist"); GtkWidget *dialog = dialog_find("rotation-editor-create-dialog"); g_assert_nonnull(dialog);
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-create-name")), name);
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(control(dialog, "rotation-editor-create-purpose")), purpose));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    wait_until(probability_create_closed, NULL, "actual create workflow receipt closes creation surface"); wait_until(probability_saved, h, "created source rotation ACK");
}
static gchar *probability_playlist_id(const gchar *name, const gchar *purpose)
{
    JsonNode *catalogue = probability_catalogue(); JsonArray *rows = json_object_get_array_member(json_node_get_object(catalogue), "playlists"); gchar *id = NULL;
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        if (!g_strcmp0(radios_json_string(row, "name", ""), name)) { g_assert_cmpstr(radios_json_string(row, "purpose", ""), ==, purpose); id = g_strdup(radios_json_string(row, "_id", "")); break; }
    }
    json_node_free(catalogue); g_assert_nonnull(id); return id;
}
static gchar *probability_entry_create(const gchar *playlist, const gchar *title)
{
    JsonNode *catalogue = probability_catalogue(); JsonArray *rows = json_object_get_array_member(json_node_get_object(catalogue), "playlists"); gint64 revision = 0;
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        if (!g_strcmp0(radios_json_string(row, "_id", ""), playlist)) revision = radios_json_int(row, "_rev", 0);
    }
    json_node_free(catalogue); g_assert_cmpint(revision, >, 0);
    JsonNode *body = body_new(); JsonObject *payload = json_node_get_object(body), *metadata = json_object_new();
    json_object_set_string_member(payload, "playlist_id", playlist); json_object_set_int_member(payload, "expected_revision", revision);
    gchar *key = g_uuid_string_random(); json_object_set_string_member(payload, "idempotency_key", key); g_free(key);
    const gchar *fields[] = {"title", "artist", "album", "genre", "key", "location", "comment"};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) json_object_set_string_member(metadata, fields[i], i == 0 ? title : "");
    json_object_set_int_member(metadata, "duration_ms", 1000); json_object_set_double_member(metadata, "bpm", 120);
    json_object_set_object_member(payload, "metadata", metadata); JsonNode *result = request("/v1/workflows/desktop-playlists/entry-save", body); json_node_free(body);
    JsonArray *tracks = json_object_get_array_member(json_node_get_object(result), "tracks"); gchar *entry = NULL;
    for (guint i = 0; tracks && i < json_array_get_length(tracks); ++i) {
        JsonObject *track = json_array_get_object_element(tracks, i);
        if (!g_strcmp0(radios_json_string(track, "title", ""), title)) entry = g_strdup(radios_json_string(track, "_id", ""));
    }
    json_node_free(result); g_assert_nonnull(entry); g_assert_true(g_uuid_string_is_valid(entry)); return entry;
}
static GtkWidget *probability_open(ProbabilityHarness *h)
{
    click(h->host.page, "rotation-editor-probability");
    GtkWidget *dialog = find(h->host.page, "rotation-editor-probability-dialog");
    if (!dialog) dialog = dialog_find("rotation-editor-probability-dialog");
    g_assert_nonnull(dialog);
    GtkWidget *editor = control(dialog, "probability-editor-root"); g_assert_true(radios_probability_editor_ready(editor));
    gboolean has_choices = gtk_widget_get_visible(control(editor, "probability-editor-choice-section"));
    g_assert_cmpint(gtk_notebook_get_current_page(GTK_NOTEBOOK(control(editor, "probability-editor-tabs"))), ==, has_choices ? 0 : 1);
    g_assert_true(gtk_widget_get_visible(control(editor, "probability-editor-advanced")));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control(editor, "probability-editor-advanced"))));
    return editor;
}
/* Drive GTK's actual in-place GtkEntry, rather than bypassing the cell
 * renderer or modifying JSON directly. A malformed completed edit is rejected
 * and the last acknowledged cell value is restored. */
static void probability_edit_started(GtkCellRenderer *cell, GtkCellEditable *entry, const gchar *path, gpointer data)
{
    (void)cell; (void)path; g_assert_true(GTK_IS_ENTRY(entry));
    *(GtkWidget **)data = GTK_WIDGET(g_object_ref(entry));
}
static GtkCellRenderer *probability_cell(GtkWidget *editor, const gchar *table, gint column)
{
    GtkTreeView *tree = GTK_TREE_VIEW(control(editor, table));
    GtkTreeViewColumn *view = gtk_tree_view_get_column(tree, column); g_assert_nonnull(view);
    GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(view)); g_assert_nonnull(cells);
    GtkCellRenderer *cell = cells->data; g_list_free(cells); return cell;
}
static void probability_show_table(GtkWidget *editor, const gchar *table, gint column)
{
    gint page = !g_strcmp0(table, "probability-editor-transitions") ? 1 :
        !g_strcmp0(table, "probability-editor-jingles") ? 2 : 0;
    gtk_notebook_set_current_page(GTK_NOTEBOOK(control(editor, "probability-editor-tabs")), page);
    (void)column;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(editor, "probability-editor-advanced")), TRUE);
    settle_layout();
}
static void probability_cell_edit(GtkWidget *editor, const gchar *table, gint row, gint column, const gchar *value)
{
    probability_show_table(editor, table, column);
    GtkTreeView *tree = GTK_TREE_VIEW(control(editor, table));
    GtkTreeViewColumn *view = gtk_tree_view_get_column(tree, column);
    GtkCellRenderer *cell = probability_cell(editor, table, column); GtkWidget *entry = NULL;
    gulong handler = g_signal_connect(cell, "editing-started", G_CALLBACK(probability_edit_started), &entry);
    GtkTreePath *path = gtk_tree_path_new_from_indices(row, -1);
    gtk_tree_view_scroll_to_cell(tree, path, view, TRUE, .5, .5); settle_layout();
    g_assert_true(gtk_widget_get_mapped(GTK_WIDGET(tree)));
    g_test_message("Editing visible advanced cell %s row %d column %d (%dx%d)", table, row, column,
        gtk_widget_get_allocated_width(GTK_WIDGET(tree)), gtk_widget_get_allocated_height(GTK_WIDGET(tree)));
    gtk_tree_view_set_cursor_on_cell(tree, path, view, cell, TRUE); pump();
    g_signal_handler_disconnect(cell, handler); g_assert_true(GTK_IS_ENTRY(entry));
    gtk_entry_set_text(GTK_ENTRY(entry), value);
    gtk_cell_editable_editing_done(GTK_CELL_EDITABLE(entry));
    gtk_cell_editable_remove_widget(GTK_CELL_EDITABLE(entry));
    g_object_unref(entry); gtk_tree_path_free(path); pump();
}
static void probability_cell_option(GtkWidget *editor, const gchar *table, gint row, gint column, const gchar *id)
{
    probability_show_table(editor, table, column);
    GtkCellRenderer *cell = probability_cell(editor, table, column); g_assert_true(GTK_IS_CELL_RENDERER_COMBO(cell));
    GtkTreeModel *model = NULL; g_object_get(cell, "model", &model, NULL); g_assert_nonnull(model);
    GtkTreeIter iter; gboolean found = FALSE;
    for (gboolean more = gtk_tree_model_get_iter_first(model, &iter); more; more = gtk_tree_model_iter_next(model, &iter)) {
        gchar *candidate = NULL; gtk_tree_model_get(model, &iter, 0, &candidate, -1);
        found = !g_strcmp0(candidate, id); g_free(candidate); if (found) break;
    }
    g_assert_true(found); gchar *path = g_strdup_printf("%d", row);
    g_signal_emit_by_name(cell, "changed", path, &iter); g_free(path); g_object_unref(model); pump();
}
static gint probability_choice_row(GtkWidget *editor, const gchar *id)
{
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(control(editor, "probability-editor-choices"))); GtkTreeIter iter; gint row = 0;
    for (gboolean more = gtk_tree_model_get_iter_first(model, &iter); more; more = gtk_tree_model_iter_next(model, &iter), ++row) {
        gchar *candidate = NULL; gtk_tree_model_get(model, &iter, 0, &candidate, -1);
        gboolean match = !g_strcmp0(candidate, id); g_free(candidate); if (match) return row;
    }
    g_error("Expected actual policy target %s", id); return -1;
}
static void probability_choice_edit(GtkWidget *editor, const gchar *scope, const gchar *id, const gchar *value)
{
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(control(editor, "probability-editor-level")), scope)); pump();
    probability_cell_edit(editor, "probability-editor-choices", probability_choice_row(editor, id), 1, value);
}
static JsonObject *probability_policy(JsonObject *configuration)
{ return json_object_get_object_member(json_object_get_object_member(configuration, "settings"), "probability_policy"); }
static JsonObject *probability_group(JsonObject *policy, const gchar *scope, const gchar *id)
{
    JsonArray *children = json_object_get_array_member(policy, "children");
    for (guint i = 0; children && i < json_array_get_length(children); ++i) {
        JsonObject *group = json_array_get_object_element(children, i); if (g_strcmp0(radios_json_string(group, "scope", ""), scope)) continue;
        JsonArray *ids = json_object_get_array_member(group, "ids");
        if ((!id || !*id) && !json_array_get_length(ids)) return group;
        for (guint j = 0; id && j < json_array_get_length(ids); ++j) if (!g_strcmp0(json_array_get_string_element(ids, j), id)) return group;
    }
    g_error("Expected saved group %s/%s", scope, id ? id : "remaining"); return NULL;
}
static void probability_add_crossfade(GtkWidget *editor)
{
    click(editor, "probability-editor-transition-add");
    probability_cell_option(editor, "probability-editor-transitions", 0, 0, "1");
    probability_cell_edit(editor, "probability-editor-transitions", 0, 3, "3");
    g_signal_emit_by_name(probability_cell(editor, "probability-editor-transitions", 2), "toggled", "0"); pump();
    probability_cell_edit(editor, "probability-editor-transitions", 0, 4, "10");
    probability_cell_edit(editor, "probability-editor-transitions", 0, 5, "2");
    probability_cell_edit(editor, "probability-editor-transitions", 0, 6, "7");
    probability_cell_edit(editor, "probability-editor-transitions", 0, 7, "23:00-01:00, 12:00-13:00");
    g_assert_true(radios_probability_editor_ready(editor));
}
static void test_probability_create_ack_reopen(void)
{
    ProbabilityHarness *h = probability_harness_open(NULL);
    g_assert_false(gtk_widget_get_visible(control(h->host.page, "rotation-editor-selection-mode")));
    g_assert_false(gtk_widget_get_visible(control(h->host.page, "rotation-editor-source-properties")));
    probability_create_ui(h, "PDF Sweeper native creation", "sweeper"); gchar *id = probability_playlist_id("PDF Sweeper native creation", "sweeper");
    JsonNode *record = NULL; JsonObject *configuration = probability_saved_config(h, &record); JsonArray *sources = json_object_get_array_member(configuration, "sources");
    g_assert_cmpuint(json_array_get_length(sources), ==, 1); g_assert_cmpstr(radios_json_string(json_array_get_object_element(sources, 0), "playlist_id", ""), ==, id);
    GtkTreeView *tree = GTK_TREE_VIEW(control(h->host.page, "rotation-editor-sources")); GtkTreeViewColumn *kind = gtk_tree_view_get_column(tree, 1); g_assert_cmpstr(gtk_tree_view_column_get_title(kind), ==, "Tür");
    GtkTreeIter row; g_assert_true(gtk_tree_model_get_iter_first(gtk_tree_view_get_model(tree), &row)); gchar *title = NULL;
    gtk_tree_model_get(gtk_tree_view_get_model(tree), &row, 9, &title, -1); g_assert_cmpstr(title, ==, "Sweeper"); g_free(title);
    probability_harness_close(h); h = probability_harness_open(json_node_get_object(record)); JsonNode *restored = radios_rotation_editor_snapshot(h->host.page);
    JsonNode *definition = json_node_new(JSON_NODE_OBJECT); json_node_set_object(definition, probability_definition(record));
    g_assert_true(json_node_equal(definition, restored)); json_node_free(definition); json_node_free(restored);
    probability_harness_close(h); json_node_free(record); g_free(id);
}
static void test_probability_scopes_ack_reopen(void)
{
    ProbabilityHarness *h = probability_harness_open(NULL); probability_create_ui(h, "Selected music", "music"); probability_create_ui(h, "Remaining music", "music");
    gchar *selected = probability_playlist_id("Selected music", "music"), *remaining = probability_playlist_id("Remaining music", "music");
    gchar *entry_one = probability_entry_create(selected, "Selected playlist entry A"), *entry_two = probability_entry_create(remaining, "Other playlist entry B");
    probability_catalogue_install(h); GtkWidget *editor = probability_open(h);
    probability_choice_edit(editor, "playlist", selected, "65"); probability_choice_edit(editor, "playlist", "", "35");
    probability_choice_edit(editor, "track", entry_one, "60"); probability_choice_edit(editor, "track", entry_two, "0");
    probability_choice_edit(editor, "track", "", "40");
    probability_add_crossfade(editor); probability_cell_edit(editor, "probability-editor-transitions", 0, 1, "80");
    JsonNode *record = NULL; JsonObject *policy = probability_policy(probability_saved_config(h, &record));
    GError *error = NULL; g_assert_true(radios_automation_policy_validate(policy, &error)); g_assert_no_error(error);
    g_assert_cmpfloat(json_object_get_double_member(probability_group(policy, "playlist", selected), "probability"), ==, 65);
    g_assert_cmpfloat(json_object_get_double_member(probability_group(policy, "playlist", ""), "probability"), ==, 35);
    g_assert_cmpfloat(json_object_get_double_member(probability_group(policy, "track", entry_one), "probability"), ==, 60);
    g_assert_cmpfloat(json_object_get_double_member(probability_group(policy, "track", entry_two), "probability"), ==, 0);
    g_assert_cmpfloat(json_object_get_double_member(probability_group(policy, "track", ""), "probability"), ==, 40);
    JsonObject *transition = json_array_get_object_element(json_object_get_array_member(policy, "transitions"), 0);
    g_assert_cmpint(json_object_get_int_member(transition, "mode"), ==, 1); g_assert_cmpint(json_object_get_int_member(transition, "overlap_ms"), ==, 3000);
    g_assert_cmpfloat(json_object_get_double_member(transition, "early_seconds"), ==, 10);
    g_assert_cmpstr(radios_json_string(transition, "effect", ""), ==, "lowpass"); g_assert_cmpuint(json_array_get_length(json_object_get_array_member(transition, "excluded_times")), ==, 2);
    Harness capture = h->host; capture.window = gtk_widget_get_toplevel(editor); settle_layout();
    capture_to(&capture, g_getenv("RADIOS_PROBABILITY_CAPTURE"));
    probability_harness_close(h); h = probability_harness_open(json_node_get_object(record)); editor = probability_open(h);
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control(editor, "probability-editor-advanced"))));
    JsonNode *restored = radios_probability_editor_snapshot(editor); g_assert_nonnull(restored);
    g_assert_true(json_node_equal(restored, json_object_get_member(json_object_get_object_member(probability_definition(record), "settings"), "probability_policy")));
    guint before = h->changes;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(editor, "probability-editor-advanced")), FALSE);
    g_assert_false(gtk_tree_view_column_get_visible(gtk_tree_view_get_column(GTK_TREE_VIEW(control(editor, "probability-editor-transitions")), 5)));
    JsonNode *hidden = radios_probability_editor_snapshot(editor);
    g_assert_true(json_node_equal(restored, hidden)); g_assert_cmpuint(h->changes, ==, before); json_node_free(hidden);
    /* Clearing one row restores its inherited share without deleting the
     * other scope or its zero/explicit remaining shares. */
    probability_choice_edit(editor, "playlist", selected, "");
    json_node_free(restored); probability_harness_close(h); json_node_free(record);
    g_free(selected); g_free(remaining); g_free(entry_one); g_free(entry_two);
}
static void test_probability_invalid_draft_no_write(void)
{
    ProbabilityHarness *h = probability_harness_open(NULL); probability_create_ui(h, "Invalid draft guard", "music"); GtkWidget *editor = probability_open(h);
    probability_add_crossfade(editor); probability_cell_edit(editor, "probability-editor-transitions", 0, 1, "40");
    JsonNode *baseline = NULL; probability_saved_config(h, &baseline); guint changes = h->changes;
    probability_cell_edit(editor, "probability-editor-transitions", 0, 1, "101");
    g_assert_true(radios_probability_editor_ready(editor)); g_assert_cmpuint(h->changes, ==, changes);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(control(editor, "probability-editor-status"))), "0 ile 100"));
    JsonNode *unchanged = probability_record(h->id); g_assert_true(json_node_equal(baseline, unchanged)); json_node_free(unchanged);
    probability_cell_edit(editor, "probability-editor-transitions", 0, 7, "25:00-01:00");
    g_assert_cmpuint(h->changes, ==, changes);
    unchanged = probability_record(h->id); g_assert_true(json_node_equal(baseline, unchanged)); json_node_free(unchanged);
    probability_cell_edit(editor, "probability-editor-transitions", 0, 7, "23:00-01:00");
    JsonNode *record = NULL; JsonObject *policy = probability_policy(probability_saved_config(h, &record));
    JsonObject *row = json_array_get_object_element(json_object_get_array_member(policy, "transitions"), 0), *window = json_array_get_object_element(json_object_get_array_member(row, "excluded_times"), 0);
    g_assert_cmpint(json_object_get_int_member(window, "start_minute"), ==, 1380); g_assert_cmpint(json_object_get_int_member(window, "end_minute"), ==, 60);
    /* A second valid row cannot push the table above 100 percent. */
    click(editor, "probability-editor-transition-add"); wait_until(probability_saved, h, "second transition ACK"); changes = h->changes;
    probability_cell_edit(editor, "probability-editor-transitions", 1, 1, "70"); g_assert_cmpuint(h->changes, ==, changes);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(control(editor, "probability-editor-status"))), "%100"));
    probability_harness_close(h); json_node_free(baseline); json_node_free(record);
}
static void test_probability_jingle_explicit_timing(void)
{
    ProbabilityHarness *h = probability_harness_open(NULL); probability_create_ui(h, "Explicit jingle playlist", "jingle"); gchar *playlist = probability_playlist_id("Explicit jingle playlist", "jingle");
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(card); gtk_widget_show(card); g_object_set_data(G_OBJECT(h->host.window), "radios-deck-owner-4", card);
    GtkWidget *editor = probability_open(h);
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(control(editor, "probability-editor-jingle-player"))), ==, -1);
    g_assert_false(gtk_widget_is_sensitive(control(editor, "probability-editor-jingle-add")));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(control(editor, "probability-editor-jingle-playlist")), playlist));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(control(editor, "probability-editor-jingle-player")), "4"));
    click(editor, "probability-editor-jingle-add");
    probability_cell_option(editor, "probability-editor-jingles", 0, 2, "before_end");
    probability_cell_edit(editor, "probability-editor-jingles", 0, 3, "12");
    probability_cell_edit(editor, "probability-editor-jingles", 0, 4, "25");
    probability_cell_edit(editor, "probability-editor-jingles", 0, 5, "-15");
    probability_cell_edit(editor, "probability-editor-jingles", 0, 6, "3");
    JsonNode *record = NULL; JsonObject *policy = probability_policy(probability_saved_config(h, &record)); JsonObject *jingle = json_array_get_object_element(json_object_get_array_member(policy, "jingles"), 0);
    g_assert_cmpstr(radios_json_string(jingle, "playlist_id", ""), ==, playlist); g_assert_cmpint(json_object_get_int_member(jingle, "player"), ==, 4); g_assert_cmpstr(radios_json_string(jingle, "anchor", ""), ==, "before_end");
    g_assert_cmpfloat(json_object_get_double_member(jingle, "seconds"), ==, 12); g_assert_cmpfloat(json_object_get_double_member(jingle, "probability"), ==, 25);
    g_assert_cmpfloat(json_object_get_double_member(jingle, "duck_db"), ==, -15); g_assert_cmpint(json_object_get_int_member(jingle, "frequency"), ==, 3);
    g_object_set_data(G_OBJECT(h->host.window), "radios-deck-owner-4", NULL); g_object_unref(card); probability_harness_close(h);
    h = probability_harness_open(json_node_get_object(record)); editor = probability_open(h); JsonNode *snapshot = radios_probability_editor_snapshot(editor); g_assert_nonnull(snapshot);
    g_assert_true(json_node_equal(snapshot, json_object_get_member(json_object_get_object_member(probability_definition(record), "settings"), "probability_policy")));
    json_node_free(snapshot); probability_harness_close(h); json_node_free(record); g_free(playlist);
}
static void probability_simple_select(GtkWidget *editor, const gchar *list_name, const gchar *id)
{
    GtkWidget *list = control(editor, list_name); g_assert_true(GTK_IS_LIST_BOX(list));
    for (gint i = 0; ; ++i) {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(list), i); g_assert_nonnull(row);
        if (g_strcmp0(g_object_get_data(G_OBJECT(row), "rule-id"), id)) continue;
        gtk_list_box_select_row(GTK_LIST_BOX(list), row); pump(); return;
    }
}
static void probability_form_percent(GtkWidget *editor, const gchar *prefix, gdouble value)
{
    gchar *automatic = g_strdup_printf("probability-editor-%s-auto", prefix), *spin = g_strdup_printf("probability-editor-%s-percent", prefix);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(editor, automatic)), FALSE);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(editor, spin)), value); g_free(automatic); g_free(spin);
}
static void probability_simple_capture(ProbabilityHarness *h, GtkWidget *editor, const gchar *environment)
{
    Harness capture = h->host; capture.window = gtk_widget_get_toplevel(editor); settle_layout();
    capture_to(&capture, g_getenv(environment));
}
/* The ordinary path never enters a table cell: select, fill, add/update/delete.
 * Advanced rule details must survive a later edit through the simple form. */
static void test_probability_simple_forms(void)
{
    ProbabilityHarness *h = probability_harness_open(NULL);
    probability_create_ui(h, "Simple form music", "music"); probability_create_ui(h, "Simple form jingle", "jingle");
    gchar *music = probability_playlist_id("Simple form music", "music"), *jingle_id = probability_playlist_id("Simple form jingle", "jingle");
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(card); gtk_widget_show(card);
    g_object_set_data(G_OBJECT(h->host.window), "radios-deck-owner-4", card);
    GtkWidget *editor = probability_open(h), *tabs = control(editor, "probability-editor-tabs"), *advanced = control(editor, "probability-editor-advanced");
    g_assert_false(gtk_widget_get_mapped(control(editor, "probability-editor-choices")));
    probability_simple_select(editor, "probability-editor-choice-list", music); probability_form_percent(editor, "choice", 65);
    click(editor, "probability-editor-choice-save"); wait_until(probability_saved, h, "simple choice ACK");
    g_assert_true(gtk_widget_get_mapped(editor));
    probability_simple_capture(h, editor, "RADIOS_SIMPLE_PROBABILITY_CHOICE_CAPTURE");

    gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs), 1); pump();
    g_assert_false(gtk_widget_get_mapped(control(editor, "probability-editor-transitions")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(editor, "probability-editor-transition-mode-3")), TRUE);
    probability_form_percent(editor, "transition", 40);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(editor, "probability-editor-transition-overlap")), 2.5);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(editor, "probability-editor-transition-early")), 4);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(control(editor, "probability-editor-transition-effect")), TRUE);
    click(editor, "probability-editor-transition-add"); wait_until(probability_saved, h, "simple transition ACK");
    g_assert_true(gtk_widget_get_mapped(editor));
    g_assert_nonnull(gtk_list_box_get_selected_row(GTK_LIST_BOX(control(editor, "probability-editor-transition-list"))));
    probability_cell_edit(editor, "probability-editor-transitions", 0, 5, "3");
    probability_cell_edit(editor, "probability-editor-transitions", 0, 6, "7");
    probability_cell_edit(editor, "probability-editor-transitions", 0, 7, "23:00-01:00");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(advanced), FALSE); pump();
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(editor, "probability-editor-transition-overlap")), 4.5);
    click(editor, "probability-editor-transition-save"); wait_until(probability_saved, h, "simple update preserves advanced rule ACK");
    g_assert_true(gtk_widget_get_mapped(editor));
    probability_simple_capture(h, editor, "RADIOS_SIMPLE_PROBABILITY_CAPTURE");

    gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs), 2); pump();
    g_assert_false(gtk_widget_get_mapped(control(editor, "probability-editor-jingles")));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(control(editor, "probability-editor-jingle-playlist")), jingle_id));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(control(editor, "probability-editor-jingle-player")), "4"));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(control(editor, "probability-editor-jingle-anchor")), "before_end"));
    probability_form_percent(editor, "jingle", 25);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(editor, "probability-editor-jingle-seconds")), 12);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(editor, "probability-editor-jingle-duck")), -15);
    click(editor, "probability-editor-jingle-add"); wait_until(probability_saved, h, "simple jingle ACK");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(control(editor, "probability-editor-jingle-seconds")), 18);
    click(editor, "probability-editor-jingle-save"); wait_until(probability_saved, h, "simple jingle update ACK");
    g_assert_true(gtk_widget_get_mapped(editor));
    probability_simple_capture(h, editor, "RADIOS_SIMPLE_PROBABILITY_JINGLE_CAPTURE");

    JsonNode *record = NULL; JsonObject *policy = probability_policy(probability_saved_config(h, &record));
    g_assert_cmpfloat(json_object_get_double_member(probability_group(policy, "playlist", music), "probability"), ==, 65);
    JsonArray *transitions = json_object_get_array_member(policy, "transitions"), *jingles = json_object_get_array_member(policy, "jingles");
    g_assert_cmpuint(json_array_get_length(transitions), ==, 1); g_assert_cmpuint(json_array_get_length(jingles), ==, 1);
    JsonObject *transition = json_array_get_object_element(transitions, 0), *jingle = json_array_get_object_element(jingles, 0);
    g_assert_cmpint(radios_json_int(transition, "mode", -1), ==, 3); g_assert_cmpint(radios_json_int(transition, "overlap_ms", -1), ==, 4500);
    g_assert_cmpint(radios_json_int(transition, "frequency", -1), ==, 3); g_assert_cmpint(radios_json_int(transition, "interval_minutes", -1), ==, 7);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(transition, "excluded_times")), ==, 1);
    g_assert_cmpstr(radios_json_string(jingle, "anchor", ""), ==, "before_end"); g_assert_cmpfloat(json_object_get_double_member(jingle, "seconds"), ==, 18);
    g_assert_cmpfloat(json_object_get_double_member(jingle, "probability"), ==, 25); g_assert_cmpfloat(json_object_get_double_member(jingle, "duck_db"), ==, -15);
    g_object_set_data(G_OBJECT(h->host.window), "radios-deck-owner-4", NULL); g_object_unref(card); probability_harness_close(h);

    h = probability_harness_open(json_node_get_object(record)); editor = probability_open(h);
    JsonNode *snapshot = radios_probability_editor_snapshot(editor); g_assert_nonnull(snapshot);
    g_assert_true(json_node_equal(snapshot, json_object_get_member(json_object_get_object_member(probability_definition(record), "settings"), "probability_policy")));
    json_node_free(snapshot); gtk_notebook_set_current_page(GTK_NOTEBOOK(control(editor, "probability-editor-tabs")), 1);
    probability_simple_select(editor, "probability-editor-transition-list", radios_json_string(transition, "id", ""));
    click(editor, "probability-editor-transition-remove"); wait_until(probability_saved, h, "simple delete ACK");
    JsonNode *removed = NULL; JsonObject *remaining = probability_policy(probability_saved_config(h, &removed));
    g_assert_false(json_object_has_member(remaining, "transitions")); g_assert_true(json_object_has_member(remaining, "jingles"));
    json_node_free(removed); probability_harness_close(h); json_node_free(record); g_free(music); g_free(jingle_id);
}
static void test_probability_soft_end_relax_ack_reopen(void)
{
    ProbabilityHarness *h = probability_harness_open(NULL); probability_create_ui(h, "Soft ending controls", "music");
    GtkWidget *sources = control(h->host.page, "rotation-editor-sources"); GtkTreePath *path = gtk_tree_path_new_from_indices(0, -1);
    g_signal_emit_by_name(sources, "row-activated", path, NULL); gtk_tree_path_free(path); pump();
    GtkWidget *settings_panel = control(h->host.page, "rotation-editor-settings"); g_assert_true(gtk_widget_get_visible(settings_panel));
    gtk_expander_set_expanded(GTK_EXPANDER(settings_panel), TRUE); settle_layout();
    GtkWidget *soft = control(h->host.page, "rotation-editor-soft-end-seconds"), *relax = control(h->host.page, "rotation-editor-relax-gaps");
    reusable_reveal(soft); g_assert_true(gtk_widget_get_mapped(soft));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(soft), 47); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(relax), TRUE); pump();
    JsonNode *record = NULL; JsonObject *settings = json_object_get_object_member(probability_saved_config(h, &record), "settings");
    g_assert_cmpint(json_object_get_int_member(settings, "soft_end_seconds"), ==, 47); g_assert_true(json_object_get_boolean_member(settings, "relax_gaps"));
    guint changes = h->changes; gtk_entry_set_text(GTK_ENTRY(soft), "601"); pump(); g_assert_cmpuint(h->changes, ==, changes);
    JsonNode *unchanged = probability_record(h->id); g_assert_true(json_node_equal(record, unchanged)); json_node_free(unchanged);
    gtk_entry_set_text(GTK_ENTRY(soft), "47"); pump(); probability_harness_close(h);
    h = probability_harness_open(json_node_get_object(record));
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control(h->host.page, "rotation-editor-soft-end-seconds"))), ==, 47);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control(h->host.page, "rotation-editor-relax-gaps"))));
    probability_harness_close(h); json_node_free(record);
}
static gchar *probability_barrier(gpointer value, const RadioSCancelToken *cancel, GError **error)
{
    (void)cancel; (void)error; g_atomic_int_set((gint *)value, 1); return NULL;
}
static gboolean probability_barrier_done(gpointer value)
{ return g_atomic_int_get((gint *)value) != 0; }
static void test_probability_late_create_rotation_fence(void)
{
    ProbabilityHarness *h = probability_harness_open(NULL); RuntimeGate gate = {0}; gate_start(&h->host, &gate);
    click(h->host.page, "rotation-editor-create-playlist"); GtkWidget *dialog = dialog_find("rotation-editor-create-dialog"); g_assert_nonnull(dialog);
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-create-name")), "PDF late creation owner fence"); gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_false(gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT)));
    JsonNode *replacement = radios_rotation_editor_snapshot(h->host.page); g_assert_nonnull(replacement); gchar *new_id = g_uuid_string_random();
    json_object_set_string_member(json_node_get_object(replacement), "id", new_id); radios_rotation_editor_set_config(h->host.page, json_node_get_object(replacement)); json_node_free(replacement);
    g_assert_null(dialog_find("rotation-editor-create-dialog")); guint changes = h->changes; g_atomic_int_set(&gate.release, 1); gint completed = 0; GError *error = NULL;
    g_assert_cmpuint(radios_runtime_submit(h->host.ctx.runtime, probability_barrier, &completed, NULL, &error), >, 0); g_assert_no_error(error);
    wait_until(probability_barrier_done, &completed, "late real HTTP work completes after rotation replacement");
    pump(); JsonNode *after = radios_rotation_editor_snapshot(h->host.page); g_assert_nonnull(after); g_assert_cmpstr(radios_json_string(json_node_get_object(after), "id", ""), ==, new_id);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(after), "sources")), ==, 0); g_assert_cmpuint(h->changes, ==, changes);
    json_node_free(after); probability_harness_close(h); g_free(new_id);
}
static guint probability_named_count(GtkWidget *widget, const gchar *name)
{
    guint count = !g_strcmp0(gtk_widget_get_name(widget), name);
    if (GTK_IS_CONTAINER(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *item = children; item; item = item->next) count += probability_named_count(item->data, name);
        g_list_free(children);
    }
    return count;
}
static gchar *probability_main_definition(Harness *harness, const gchar *playlist, const gchar *name)
{
    click(harness->page, "automation-calendar-independent-rotation"); GtkWidget *dialog = reusable_dialog();
    gtk_entry_set_text(GTK_ENTRY(control(dialog, "rotation-editor-name")), name);
    PlaylistChoice choice = {control(dialog, "rotation-editor-playlist-combo"), playlist};
    wait_until(playlist_available, &choice, "actual playlist choice for unified calendar rotation");
    click(dialog, "rotation-editor-add"); wait_saved(harness); JsonNode *calendar = snapshot_of(harness->page);
    JsonArray *library = json_object_get_array_member(json_node_get_object(calendar), "rotation_library");
    gchar *id = g_strdup(radios_json_string(json_array_get_object_element(library, json_array_get_length(library)-1), "id", ""));
    json_node_free(calendar); gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump(); wait_saved(harness); return id;
}
static void probability_reveal_calendar_control(Harness *harness, GtkWidget *widget)
{
    GtkWidget *scroll = control(harness->page, "automation-calendar-properties-scroll");
    if (!gtk_widget_is_ancestor(widget, scroll)) return;
    GtkWidget *content = gtk_bin_get_child(GTK_BIN(scroll));
    if (GTK_IS_VIEWPORT(content)) content = gtk_bin_get_child(GTK_BIN(content));
    gint x = 0, y = 0;
    g_assert_true(gtk_widget_translate_coordinates(widget, content, 0, 0, &x, &y));
    GtkAdjustment *vertical = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    gtk_adjustment_clamp_page(vertical, y, y + gtk_widget_get_allocated_height(widget)); settle_layout();
}
static void test_probability_unified_geometry(void)
{
    Harness *harness = harness_open(); set_date(harness, "2026-11-02");
    gchar *playlist = create_playlist("PDF tek takvim gerçek playlisti"); radios_data_refresh(&harness->ctx);
    gchar *first = probability_main_definition(harness, playlist, "PDF rotasyon A"), *second = probability_main_definition(harness, playlist, "PDF rotasyon B");
    new_event(harness, 0, 600, 660, "PDF takvim program bloğu", TRUE);
    GtkWidget *picker = control(harness->page, "automation-calendar-library-picker");
    gtk_expander_set_expanded(GTK_EXPANDER(picker), TRUE); settle_layout();
    GtkTreeView *available = GTK_TREE_VIEW(control(harness->page, "automation-calendar-available-rotations"));
    g_assert_cmpint(gtk_tree_selection_get_mode(gtk_tree_view_get_selection(available)), ==, GTK_SELECTION_MULTIPLE);
    gtk_tree_selection_select_all(gtk_tree_view_get_selection(available)); click(harness->page, "automation-calendar-assign-rotation"); wait_saved(harness);
    JsonNode *calendar = snapshot_of(harness->page); JsonObject *event = event_at(calendar, "2026-11-02", 600);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(event, "rotations")), ==, 2); json_node_free(calendar);
    gtk_expander_set_expanded(GTK_EXPANDER(picker), FALSE); settle_layout();
    JsonNode *main_record = calendar_record(); JsonNode *programme = weekly_save_as(harness, "PDF tek takvim kayıtlı program");
    radios_data_refresh(&harness->ctx); GtkWidget *workspace = weekly_workspace(harness, radios_json_string(json_node_get_object(programme), "_id", ""));
    GtkWidget *main = g_object_get_data(G_OBJECT(workspace), "weekly-programme-calendar"); g_assert_true(main == harness->page);
    g_assert_cmpuint(probability_named_count(harness->page, "automation-calendar-grid"), ==, 1); g_assert_null(find(harness->page, "automation-calendar-tabs"));
    set_date(harness, "2026-11-02"); radios_automation_calendar_select(harness->page, 0, 600, 660, FALSE); settle_layout();
    /* Placeholder detail columns are gone: the rotation table keeps its weight
     * column behind the toggle; the library lists name and content only. */
    const gchar *assigned_names[] = {"Rotasyon / Playlist", "İçerik / Sıklık", "Ağırlık"};
    const gchar *available_names[] = {"Rotasyon", "İçerik"};
    const guint column_counts[] = {3, 2};
    const gchar *tables[] = {"automation-calendar-rotations", "automation-calendar-available-rotations"};
    for (guint t = 0; t < G_N_ELEMENTS(tables); ++t) {
        GtkTreeView *tree = GTK_TREE_VIEW(control(harness->page, tables[t]));
        g_assert_cmpint(gtk_tree_view_get_n_columns(tree), ==, (gint)column_counts[t]);
        g_assert_cmpint(gtk_widget_get_mapped(GTK_WIDGET(tree)), ==, t == 0);
        for (guint c = 0; c < column_counts[t]; ++c) {
            GtkTreeViewColumn *column = gtk_tree_view_get_column(tree, (gint)c);
            g_assert_cmpstr(gtk_tree_view_column_get_title(column), ==, t ? available_names[c] : assigned_names[c]);
            g_assert_cmpint(gtk_tree_view_column_get_visible(column), ==, c < 2);
        }
    }
    capture_to(harness, g_getenv("RADIOS_UNIFIED_CALENDAR_CAPTURE"));
    const gchar *visible[] = {"weekly-programme-choice", "automation-calendar-days", "automation-calendar-date", "automation-calendar-programme-name", "automation-calendar-rotations", "automation-calendar-programme-probability", "automation-calendar-library-picker"};
    for (guint i = 0; i < G_N_ELEMENTS(visible); ++i) {
        GtkWidget *widget = control(harness->page, visible[i]);
        probability_reveal_calendar_control(harness, widget);
        GdkRectangle rectangle = window_rect(harness, widget);
        g_assert_true(gtk_widget_get_mapped(widget)); g_assert_cmpint(rectangle.width, >, 0); g_assert_cmpint(rectangle.height, >, 0);
        g_assert_cmpint(rectangle.x, >=, 0); g_assert_cmpint(rectangle.y, >=, 0);
        g_assert_cmpint(rectangle.x + rectangle.width, <=, gtk_widget_get_allocated_width(harness->window));
        g_assert_cmpint(rectangle.y + rectangle.height, <=, gtk_widget_get_allocated_height(harness->window));
        for (GtkWidget *parent = gtk_widget_get_parent(widget); parent && parent != harness->window; parent = gtk_widget_get_parent(parent)) {
            if (!GTK_IS_SCROLLED_WINDOW(parent) && !GTK_IS_VIEWPORT(parent)) continue;
            GdkRectangle viewport = window_rect(harness, parent), intersection;
            g_assert_true(gdk_rectangle_intersect(&rectangle, &viewport, &intersection));
            g_assert_cmpint(intersection.height, ==, rectangle.height);
        }
        g_test_message("Unified real widget %s: %dx%d at%d,%d in window%dx%d", visible[i], rectangle.width, rectangle.height, rectangle.x, rectangle.y,
            gtk_widget_get_allocated_width(harness->window), gtk_widget_get_allocated_height(harness->window));
    }
    GtkWidget *properties_scroll = control(harness->page, "automation-calendar-properties-scroll");
    GtkAdjustment *vertical = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(properties_scroll));
    gtk_adjustment_set_value(vertical, 0); settle_layout();
    capture_to(harness, g_getenv("RADIOS_UNIFIED_CALENDAR_CAPTURE"));
    /* Details are opt-in. Showing either table's remaining columns changes
     * presentation only, while the ordinary view keeps two clear columns. */
    JsonNode *before_details = snapshot_of(harness->page);
    GtkWidget *details = control(harness->page, "automation-calendar-details");
    GtkWidget *detail_columns = control(harness->page, "automation-calendar-detail-columns");
    g_assert_false(gtk_expander_get_expanded(GTK_EXPANDER(details)));
    gtk_expander_set_expanded(GTK_EXPANDER(details), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(detail_columns), TRUE); settle_layout();
    for (guint t = 0; t < G_N_ELEMENTS(tables); ++t)
        for (gint c = 0; c < (gint)column_counts[t]; ++c)
            g_assert_true(gtk_tree_view_column_get_visible(gtk_tree_view_get_column(GTK_TREE_VIEW(control(harness->page, tables[t])), c)));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(detail_columns), FALSE);
    gtk_expander_set_expanded(GTK_EXPANDER(details), FALSE);
    picker = control(harness->page, "automation-calendar-library-picker");
    gtk_expander_set_expanded(GTK_EXPANDER(picker), TRUE); settle_layout();
    gtk_adjustment_set_value(vertical, gtk_adjustment_get_upper(vertical) - gtk_adjustment_get_page_size(vertical)); settle_layout();
    GtkWidget *available_widget = control(harness->page, "automation-calendar-available-rotations");
    GtkWidget *assign_widget = control(harness->page, "automation-calendar-assign-rotation");
    g_assert_true(gtk_widget_get_mapped(available_widget)); g_assert_true(gtk_widget_get_mapped(assign_widget));
    GdkRectangle available_rect = window_rect(harness, available_widget), assign_rect = window_rect(harness, assign_widget);
    GdkRectangle viewport = window_rect(harness, properties_scroll), intersection;
    g_assert_true(gdk_rectangle_intersect(&available_rect, &viewport, &intersection));
    g_assert_cmpint(intersection.height, ==, available_rect.height);
    g_assert_true(gdk_rectangle_intersect(&assign_rect, &viewport, &intersection));
    g_assert_cmpint(intersection.height, ==, assign_rect.height);
    g_assert_cmpint(assign_rect.y + assign_rect.height, <=, available_rect.y);
    gtk_expander_set_expanded(GTK_EXPANDER(picker), FALSE); gtk_adjustment_set_value(vertical, 0); settle_layout();
    JsonNode *after_details = snapshot_of(harness->page); g_assert_true(json_node_equal(before_details, after_details));
    json_node_free(before_details); json_node_free(after_details); weekly_main_unchanged(harness, main_record);
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(control(workspace, "weekly-programme-choice")), "main")); pump();
    g_assert_cmpuint(probability_named_count(harness->page, "automation-calendar-grid"), ==, 1);
    JsonNode *restored = snapshot_of(harness->page); g_assert_true(json_node_equal(restored, json_object_get_member(json_node_get_object(main_record), "config"))); json_node_free(restored);
    weekly_main_unchanged(harness, main_record); harness_close(harness); json_node_free(programme); json_node_free(main_record); g_free(first); g_free(second); g_free(playlist);
}
typedef struct { Harness *host; const gchar *text; } WarningWait;
static gboolean probability_warning_matches(gpointer value)
{
    WarningWait *wait = value; GtkWidget *warning = control(wait->host->page, "automation-calendar-content-warning");
    return wait->text ? gtk_widget_get_visible(warning) && strstr(gtk_label_get_text(GTK_LABEL(warning)), wait->text) != NULL : !gtk_widget_get_visible(warning);
}
static void probability_warning_wait(Harness *host, const gchar *text)
{ WarningWait wait = {host, text}; wait_until(probability_warning_matches, &wait, text ? text : "verified source clears warning"); }
static void probability_assert_warning_border(Harness *host)
{
    GtkWidget *grid = control(host->page, "automation-calendar-grid"); settle_layout();
    gint width = gtk_widget_get_allocated_width(grid), height = gtk_widget_get_allocated_height(grid);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height); cairo_t *cr = cairo_create(surface);
    gtk_widget_draw(grid, cr); cairo_destroy(cr); cairo_surface_flush(surface);
    const guchar *pixels = cairo_image_surface_get_data(surface); gint stride = cairo_image_surface_get_stride(surface); guint red = 0;
    for (gint y = 0; y < height; ++y) for (gint x = 0; x < width; ++x) {
        guint32 pixel; memcpy(&pixel, pixels + y * stride + x * 4, sizeof(pixel));
        guint r = (pixel >> 16) & 255, g = (pixel >> 8) & 255, b = pixel & 255;
        if (r > 150 && g < 80 && b < 90) ++red;
    }
    g_assert_cmpuint(red, >, 30); cairo_surface_destroy(surface);
}
static void test_probability_calendar_content_warnings(void)
{
    Harness *h = harness_open(); set_date(h, "2026-12-07"); new_event(h, 0, 600, 660, "Eksik içerik denetimi", TRUE); wait_saved(h);
    probability_warning_wait(h, "rotasyon atanmamış");
    gchar *warning = gtk_widget_get_tooltip_text(control(h->page, "automation-calendar-content-warning"));
    g_assert_nonnull(warning); g_assert_nonnull(strstr(warning, "Eksik içerik denetimi"));
    g_assert_nonnull(strstr(warning, "2026-12-07 10:00–11:00")); g_free(warning);
    probability_assert_warning_border(h);
    click(h->page, "automation-calendar-define-rotation"); GtkWidget *dialog = reusable_dialog();
    g_assert_true(gtk_widget_get_visible(control(dialog, "rotation-editor-content-warning")));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump(); wait_saved(h);
    probability_warning_wait(h, "rotasyonunda playlist yok");
    GtkWidget *rows = control(h->page, "automation-calendar-rotations"); GtkTreePath *path = gtk_tree_path_new_from_indices(0, -1);
    g_signal_emit_by_name(rows, "row-activated", path, NULL); gtk_tree_path_free(path); pump();
    /* The calendar table opens edits through its button/keyboard handler. */
    dialog = dialog_find("automation-calendar-library-dialog");
    if (!dialog) {
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(rows)); gtk_tree_selection_select_all(selection);
        GdkEvent *event = gdk_event_new(GDK_BUTTON_PRESS); event->type = GDK_2BUTTON_PRESS;
        event->button.window = g_object_ref(gtk_tree_view_get_bin_window(GTK_TREE_VIEW(rows))); event->button.button = 1;
        event->button.x = 10; event->button.y = 8;
        GdkSeat *seat = gdk_display_get_default_seat(gtk_widget_get_display(rows)); gdk_event_set_device(event, gdk_seat_get_pointer(seat));
        gtk_widget_event(rows, event); gdk_event_free(event); pump(); dialog = reusable_dialog();
    }
    g_assert_nonnull(dialog);
    gchar *playlist = create_playlist("Warning preflight playlist"); radios_data_refresh(&h->ctx);
    PlaylistChoice choice = {control(dialog, "rotation-editor-playlist-combo"), playlist}; wait_until(playlist_available, &choice, "new playlist for warning preflight");
    click(dialog, "rotation-editor-add"); g_assert_false(gtk_widget_get_visible(control(dialog, "rotation-editor-content-warning")));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CLOSE); pump(); wait_saved(h); probability_warning_wait(h, "playlisti boş");
    gchar *entry = probability_entry_create(playlist, "Unverified metadata draft"); g_free(entry); radios_data_refresh(&h->ctx);
    probability_warning_wait(h, "doğrulanmış yerel ses kaydı yok");
    JsonNode *media = verified_media(h); bind_playlist(playlist, media); json_node_free(media); radios_data_refresh(&h->ctx); probability_warning_wait(h, NULL);
    JsonNode *catalogue = probability_catalogue(); JsonArray *playlists = json_object_get_array_member(json_node_get_object(catalogue), "playlists"); gint64 revision = 0;
    for (guint i = 0; i < json_array_get_length(playlists); ++i) { JsonObject *row = json_array_get_object_element(playlists, i); if (!g_strcmp0(radios_json_string(row, "_id", ""), playlist)) revision = radios_json_int(row, "_rev", 0); }
    json_node_free(catalogue); g_assert_cmpint(revision, >, 0);
    JsonNode *body = body_new(); JsonObject *payload = json_node_get_object(body); json_object_set_string_member(payload, "playlist_id", playlist);
    json_object_set_int_member(payload, "expected_revision", revision); gchar *key = g_uuid_string_random(); json_object_set_string_member(payload, "idempotency_key", key); g_free(key);
    JsonNode *result = request("/v1/workflows/desktop-playlists/delete", body); json_node_free(result); json_node_free(body); radios_data_refresh(&h->ctx);
    probability_warning_wait(h, "kaldırılmış veya devre dışı"); wait_saved(h); capture_to(h, g_getenv("RADIOS_CALENDAR_WARNING_CAPTURE"));
    harness_close(h); h = harness_open(); set_date(h, "2026-12-07"); radios_automation_calendar_select(h->page, 0, 600, 660, FALSE);
    probability_warning_wait(h, "kaldırılmış veya devre dışı"); harness_close(h); g_free(playlist);
}

static int probability_ui_run(void)
{
    g_test_add_func("/automation-probability/rotation-create-Sweeper-kind-ACK-reopen", test_probability_create_ack_reopen);
    g_test_add_func("/automation-probability/scoped-transitions-selected-rest-ACK-reopen", test_probability_scopes_ack_reopen);
    g_test_add_func("/automation-probability/invalid-percentage-UTC-draft-no-write", test_probability_invalid_draft_no_write);
    g_test_add_func("/automation-probability/jingle-explicit-target-timing-ACK-reopen", test_probability_jingle_explicit_timing);
    g_test_add_func("/automation-probability/simple-forms-choice-transition-jingle-update-delete-ACK-reopen", test_probability_simple_forms);
    g_test_add_func("/automation-probability/calendar-content-warning-draft-verified-deleted-reopen", test_probability_calendar_content_warnings);
    g_test_add_func("/automation-probability/soft-end-relax-ACK-invalid-reopen", test_probability_soft_end_relax_ack_reopen);
    g_test_add_func("/automation-probability/late-create-rotation-fence", test_probability_late_create_rotation_fence);
    g_test_add_func("/automation-probability/unified-calendar-programmes-rotation-tables-geometry", test_probability_unified_geometry);
    return g_test_run();
}
int main(int argc, char **argv) { return radios_probability_fixture_main(argc, argv); }
