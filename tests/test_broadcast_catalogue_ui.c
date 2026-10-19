#include "radios/data.h"
#include "radios/playout_ui.h"
#include "radios/programme_queue.h"
#include <glib/gstdio.h>
#include <string.h>

static RadioSConfig config;
static RadioSDataClient *reader;
static gchar *last_notice;
static gboolean drop_accepted;
typedef struct { RadioSViewContext ctx; GtkWidget *window, *page; } Harness;
typedef gboolean (*Condition)(gpointer);
void __wrap_gtk_drag_finish(GdkDragContext *context, gboolean success, gboolean delete_source, guint time)
{ (void)context; (void)delete_source; (void)time; drop_accepted = success; }
static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (GTK_IS_MENU_BUTTON(root)) {
        GtkWidget *popover = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(root)));
        GtkWidget *result = popover ? find(popover, name) : NULL;
        if (result) return result;
    }
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *dialog(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *result = NULL;
    for (GList *item = windows; item && !result; item = item->next)
        if (g_str_equal(gtk_widget_get_name(item->data), name)) result = item->data;
    g_list_free(windows); return result;
}
static void pump(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void run_for(guint milliseconds)
{ gint64 end = g_get_monotonic_time() + milliseconds * 1000; while (g_get_monotonic_time() < end) pump(); }
static void wait_for(Condition condition, gpointer value, const gchar *description)
{
    gint64 end = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!condition(value) && g_get_monotonic_time() < end) pump();
    if (!condition(value)) g_error("Native catalogue timed out: %s (%s)", description, last_notice ? last_notice : "no notice");
}
static void notify(gpointer unused, const gchar *message)
{ (void)unused; g_free(last_notice); last_notice = g_strdup(message); }
static gboolean ready(gpointer value)
{ Harness *h = value; return radios_data_connected(&h->ctx) && radios_data_document_revision(radios_broadcast_programme_document(&h->ctx)) > 0; }
static Harness *open_harness(void)
{
    Harness *h = g_new0(Harness, 1); h->ctx.config = &config; h->ctx.notify = notify; h->ctx.runtime = radios_runtime_new(64, 128);
    h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); h->ctx.parent = GTK_WINDOW(h->window);
    h->ctx.data = radios_data_new(&h->ctx); h->page = radios_broadcast_new(&h->ctx);
    gtk_container_add(GTK_CONTAINER(h->window), h->page); gtk_widget_show_all(h->window);
    radios_data_connect(&h->ctx); wait_for(ready, h, "acknowledged current programme"); return h;
}
static void close_harness(Harness *h)
{ gtk_widget_destroy(h->window); radios_data_close(h->ctx.data); radios_runtime_free(h->ctx.runtime); radios_data_unref(h->ctx.data); run_for(40); g_free(h); }
typedef struct { const gchar *path; JsonNode *body, *result; GError *error; gint done; } Request;
static gpointer request_worker(gpointer value)
{
    Request *request = value; request->result = radios_data_client_request_sync(reader, "POST", request->path, request->body, NULL, &request->error);
    g_atomic_int_set(&request->done, 1); return NULL;
}
static gboolean request_done(gpointer value) { return g_atomic_int_get(&((Request *)value)->done); }
static JsonNode *request(const gchar *path, JsonNode *body)
{
    Request work = {.path = path, .body = body}; GThread *thread = g_thread_new("catalogue-proof-read", request_worker, &work);
    wait_for(request_done, &work, "authenticated HTTP proof read"); g_thread_join(thread); g_assert_no_error(work.error); g_assert_nonnull(work.result); return work.result;
}
static JsonNode *body(void)
{ JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, json_object_new()); json_object_set_string_member(json_node_get_object(node), "station_id", radios_data_client_station_id(reader)); return node; }
static JsonNode *copy(JsonNode *node)
{
    gchar *text = json_to_string(node, FALSE); JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, text, -1, NULL)); JsonNode *result = json_node_copy(json_parser_get_root(parser)); g_object_unref(parser); g_free(text); return result;
}
static JsonNode *documents(void)
{ JsonNode *payload = body(); json_object_set_string_member(json_node_get_object(payload), "kind", "programme"); JsonNode *result = request("/v1/workflows/desktop/list", payload); json_node_free(payload); return result; }
static JsonNode *document(const gchar *name)
{
    JsonNode *rows = documents(); JsonArray *records = json_object_get_array_member(json_node_get_object(rows), "records"); JsonNode *result = NULL;
    for (guint i = 0; i < json_array_get_length(records); ++i)
        if (g_str_equal(radios_json_string(json_array_get_object_element(records, i), "name", ""), name)) { result = copy(json_array_get_element(records, i)); break; }
    json_node_free(rows); g_assert_nonnull(result); return result;
}
/* A player queue is read on selection and its record is created by the
 * first edit, so an untouched queue has no record yet: absence is a state
 * that must stay unchanged too. */
static JsonNode *document_optional(const gchar *name)
{
    JsonNode *rows = documents(); JsonArray *records = json_object_get_array_member(json_node_get_object(rows), "records"); JsonNode *result = NULL;
    for (guint i = 0; i < json_array_get_length(records); ++i)
        if (g_str_equal(radios_json_string(json_array_get_object_element(records, i), "name", ""), name)) { result = copy(json_array_get_element(records, i)); break; }
    json_node_free(rows); return result;
}
static gboolean same_document(JsonNode *a, JsonNode *b) { return (!a && !b) || (a && b && json_node_equal(a, b)); }
static void put(JsonNode *row)
{
    JsonObject *record = json_node_get_object(row); JsonNode *payload = body(); JsonObject *o = json_node_get_object(payload);
    json_object_set_string_member(o, "kind", "programme"); json_object_set_string_member(o, "name", radios_json_string(record, "name", ""));
    json_object_set_string_member(o, "document_id", radios_json_string(record, "_id", "")); json_object_set_int_member(o, "expected_revision", radios_json_int(record, "_rev", 0));
    json_object_set_member(o, "config", copy(json_object_get_member(record, "config"))); gchar *key = g_uuid_string_random(); json_object_set_string_member(o, "idempotency_key", key); g_free(key);
    JsonNode *result = request("/v1/workflows/desktop/put", payload); json_node_free(result); json_node_free(payload);
}
static JsonArray *events(JsonNode *row)
{ return json_object_get_array_member(json_object_get_object_member(json_node_get_object(row), "config"), "events"); }
static gboolean enabled(gpointer value) { return gtk_widget_is_sensitive(value); }
static void click(Harness *h, const gchar *name)
{ GtkWidget *button = find(h->page, name); g_assert_true(GTK_IS_BUTTON(button)); wait_for(enabled, button, name); gtk_button_clicked(GTK_BUTTON(button)); }
static void select_row(Harness *h, const gchar *table, guint index)
{
    GtkTreeView *tree = g_object_get_data(G_OBJECT(find(h->page, table)), "tree-view"); g_assert_true(GTK_IS_TREE_VIEW(tree));
    gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(tree));
    GtkTreePath *path = gtk_tree_path_new_from_indices((gint)index, -1); gtk_tree_selection_select_path(gtk_tree_view_get_selection(tree), path); gtk_tree_path_free(path);
}
static GtkListBox *queue_list(Harness *h)
{ GtkListBox *list = g_object_get_data(G_OBJECT(find(h->page, "programme-queue-table")), "list-box"); g_assert_true(GTK_IS_LIST_BOX(list)); return list; }
static gboolean select_item_once(Harness *h, const gchar *id)
{
    GList *rows = gtk_container_get_children(GTK_CONTAINER(queue_list(h))); gboolean found = FALSE;
    for (GList *item = rows; item && !found; item = item->next)
        if (!g_strcmp0(radios_programme_queue_row_id(item->data), id)) { gtk_list_box_select_row(queue_list(h), item->data); found = TRUE; }
    g_list_free(rows); return found;
}
/* Selects a flow event or content by id in the appropriate flat list. */
static void select_item(Harness *h, const gchar *id)
{
    if (select_item_once(h, id)) return;
    JsonNode *record = document("Mevcut program");
    RadioSProgrammeTree *tree = radios_programme_tree_new(json_object_get_object_member(json_node_get_object(record), "config"));
    guint block = 0; gint leaf = -1;
    g_assert_true(radios_programme_tree_find(tree, id, &block, &leaf));
    if (leaf >= 0) {
        RadioSProgrammeBlock *owner = g_ptr_array_index(tree->blocks, block);
        radios_programme_queue_set_expanded(find(h->page, "programme-queue-table"), json_object_get_string_member(owner->block, "id"), TRUE);
    } else gtk_button_clicked(GTK_BUTTON(find(h->page, "programme-queue-back")));
    radios_programme_tree_free(tree); json_node_free(record);
    g_assert_true(select_item_once(h, id));
}
/* Events gain the block_id of their flow event when the draft is saved. */
static gboolean same_event(JsonNode *a, JsonNode *b)
{
    JsonNode *x = copy(a), *y = copy(b); json_object_remove_member(json_node_get_object(x), "block_id"); json_object_remove_member(json_node_get_object(y), "block_id");
    gboolean equal = json_node_equal(x, y); json_node_free(x); json_node_free(y); return equal;
}
typedef struct { Harness *h; const gchar *title; } Catalog;
static gboolean catalog_found(gpointer value)
{
    Catalog *test = value; GtkListStore *store = g_object_get_data(G_OBJECT(find(test->h->page, "broadcast-media-table")), "list-store"); GtkTreeIter iter;
    if (!gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &iter) ||
        g_strcmp0(gtk_label_get_text(GTK_LABEL(find(test->h->page, "broadcast-catalog-count"))), "1 / 1 kayıt")) return FALSE;
    gchar *title = NULL; gtk_tree_model_get(GTK_TREE_MODEL(store), &iter, 1, &title, -1); gboolean same = !g_strcmp0(title, test->title); g_free(title); return same;
}
static void select_media(Harness *h, const gchar *title)
{
    GtkWidget *query = find(h->page, "broadcast-catalog-query"); gtk_entry_set_text(GTK_ENTRY(query), title); g_signal_emit_by_name(query, "activate");
    Catalog desired = {h, title}; wait_for(catalog_found, &desired, "fresh requested media in catalogue"); select_row(h, "broadcast-media-table", 0);
    wait_for(enabled, find(h->page, "broadcast-catalog-insert"), "catalogue source and target ready");
}
static void flag(const gchar *name, gboolean active)
{
    gchar *directory = g_path_get_dirname(config.database_file), *path = g_build_filename(directory, name, NULL); g_free(directory);
    if (active) g_assert_true(g_file_set_contents(path, "hold", -1, NULL)); else if (g_file_test(path, G_FILE_TEST_EXISTS)) g_assert_cmpint(g_unlink(path), ==, 0);
    g_free(path);
}
static gboolean held(gpointer unused)
{ (void)unused; gchar *directory = g_path_get_dirname(config.database_file), *path = g_build_filename(directory, ".catalogue-ready", NULL); gboolean exists = g_file_test(path, G_FILE_TEST_EXISTS); g_free(path); g_free(directory); return exists; }
static gboolean catalogue_idle(gpointer value)
{ return gtk_widget_is_sensitive(find(((Harness *)value)->page, "broadcast-catalog-insert")); }
static gint programme_row(Catalog *test)
{
    GtkTreeModel *model = GTK_TREE_MODEL(g_object_get_data(G_OBJECT(find(test->h->page, "broadcast-media-table")), "list-store")); GtkTreeIter iter; gint index = 0;
    for (gboolean more = gtk_tree_model_get_iter_first(model, &iter); more; more = gtk_tree_model_iter_next(model, &iter), ++index) {
        gchar *title = NULL, *kind = NULL; gtk_tree_model_get(model, &iter, 1, &title, 3, &kind, -1);
        gboolean match = !g_strcmp0(title, test->title) && !g_strcmp0(kind, "Program"); g_free(title); g_free(kind);
        if (match) return index;
    }
    return -1;
}
static gboolean programme_listed(gpointer value) { return programme_row(value) >= 0; }
/* Programlar lists saved programme drafts in the catalogue table. */
static void select_programme(Harness *h, const gchar *name)
{
    gtk_entry_set_text(GTK_ENTRY(find(h->page, "broadcast-catalog-query")), "");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(h->page, "broadcast-kind-programme")), TRUE);
    Catalog desired = {h, name}; wait_for(programme_listed, &desired, "saved programme listed under Programlar");
    select_row(h, "broadcast-media-table", (guint)programme_row(&desired));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(find(h->page, "broadcast-catalog-replace"))), ==, "Mevcut taslağı değiştir");
}
static gboolean library_selected(gpointer value)
{
    GtkWidget *panel = find(((Harness *)value)->page, "broadcast-playlist-tools");
    return panel && find(panel, "broadcast-library-playlists") &&
        gtk_list_box_get_selected_row(GTK_LIST_BOX(find(panel, "broadcast-library-playlists"))) != NULL;
}
typedef struct { Harness *harness; const gchar *id; } LibraryRecord;
static GtkListBoxRow *library_record_row(LibraryRecord *record)
{
    GtkWidget *list = find(record->harness->page, "broadcast-library-playlists");
    GList *rows = gtk_container_get_children(GTK_CONTAINER(list)); GtkListBoxRow *found = NULL;
    for (GList *item = rows; item && !found; item = item->next)
        if (!g_strcmp0(g_object_get_data(G_OBJECT(item->data), "library-id"), record->id)) found = item->data;
    g_list_free(rows); return found;
}
static gboolean library_record_loaded(gpointer value) { return library_record_row(value) != NULL; }
static guint count_documents(void)
{ JsonNode *rows = documents(); guint count = json_array_get_length(json_object_get_array_member(json_node_get_object(rows), "records")); json_node_free(rows); return count; }

static void programme_drag_get(GtkClipboard *clipboard, GtkSelectionData *selection, guint info, gpointer data)
{
    (void)clipboard;
    g_signal_emit_by_name(data, "drag-data-get", NULL, selection, info, GDK_CURRENT_TIME);
}

static void programme_drag_stop(GtkWidget *tree, GdkDragContext *context, GtkSelectionData *selection, guint info, guint time, gpointer data)
{
    (void)context; (void)selection; (void)info; (void)time; (void)data;
    g_signal_stop_emission_by_name(tree, "drag-data-get");
}

typedef struct { Harness *harness; gint64 revision; } ProgrammeRevision;
static gboolean revision_advanced(gpointer value)
{
    ProgrammeRevision *expected = value;
    RadioSDocument *document = radios_broadcast_programme_document(&expected->harness->ctx);
    return radios_data_document_ready(document) && radios_data_document_revision(document) > expected->revision;
}

static void drop_selected_programme(Harness *h)
{
    gtk_button_clicked(GTK_BUTTON(find(h->page, "programme-queue-back"))); run_for(30);
    ProgrammeRevision expected = {h, radios_data_document_revision(radios_broadcast_programme_document(&h->ctx))};
    GtkWidget *tree = g_object_get_data(G_OBJECT(find(h->page, "broadcast-media-table")), "tree-view");
    guint count = 0; const GtkTargetEntry *targets = radios_drag_targets(&count);
    GtkClipboard *clipboard = gtk_clipboard_get(gdk_atom_intern_static_string("RADIOS_CATALOGUE_DROP_TEST"));
    g_assert_cmpuint(RADIOS_DRAG_INFO_PROGRAMME, <=, count);
    const GtkTargetEntry *target = &targets[RADIOS_DRAG_INFO_PROGRAMME - 1];
    gulong handler = g_signal_connect(tree, "drag-data-get", G_CALLBACK(programme_drag_stop), NULL);
    g_assert_true(gtk_clipboard_set_with_data(clipboard, target, 1, programme_drag_get, NULL, tree));
    GtkSelectionData *selection = gtk_clipboard_wait_for_contents(clipboard, gdk_atom_intern_static_string(target->target));
    g_signal_handler_disconnect(tree, handler);
    g_assert_nonnull(selection); drop_accepted = FALSE;
    g_signal_emit_by_name(queue_list(h), "drag-data-received", NULL, 0, 10000, selection, RADIOS_DRAG_INFO_PROGRAMME, GDK_CURRENT_TIME);
    gtk_selection_data_free(selection); gtk_clipboard_clear(clipboard);
    g_assert_true(drop_accepted);
    wait_for(revision_advanced, &expected, "whole programme drop saved");
}

static void test_insert_replace_saved_programme_reopen_embedded(void)
{
    Harness *h = open_harness(); JsonNode *before = document("Mevcut program"); guint count = json_array_get_length(events(before));
    gchar *event_id = g_strdup(radios_json_string(json_array_get_object_element(events(before), 0), "id", ""));
    gchar *block_id = radios_programme_block_id_for(event_id);
    select_media(h, "Temporary A"); select_item(h, block_id); click(h, "broadcast-catalog-insert"); wait_for(catalogue_idle, h, "verified insert acknowledged");
    JsonNode *after = document("Mevcut program"); g_assert_cmpuint(json_array_get_length(events(after)), ==, count + 1);
    g_assert_true(same_event(json_array_get_element(events(before), 0), json_array_get_element(events(after), 0)));
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(events(after), 1), "block_id", ""), ==, block_id);
    JsonObject *inserted = json_array_get_object_element(events(after), 1); g_assert_cmpstr(radios_json_string(inserted, "title", ""), ==, "Temporary A"); g_assert_cmpint(radios_json_int(inserted, "duration_ms", 0), ==, 200);
    select_media(h, "Temporary B"); select_item(h, event_id); click(h, "broadcast-catalog-replace"); wait_for(catalogue_idle, h, "verified replacement acknowledged");
    json_node_free(after); after = document("Mevcut program"); JsonObject *replaced = json_array_get_object_element(events(after), 0);
    g_assert_cmpuint(json_array_get_length(events(after)), ==, count + 1); g_assert_cmpstr(radios_json_string(replaced, "id", ""), ==, event_id);
    g_assert_cmpstr(radios_json_string(replaced, "title", ""), ==, "Temporary B"); g_assert_cmpstr(radios_json_string(replaced, "note", ""), ==, "Preserved note");
    /* Named programmes remain a catalogue source; seed one through the
     * genuine backend now that its toolbar archive command is removed. */
    guint names = count_documents();
    JsonNode *archive_body = body(); JsonObject *archive = json_node_get_object(archive_body);
    JsonNode *saved_config = copy(json_object_get_member(json_node_get_object(after), "config"));
    JsonObject *saved_settings = json_node_get_object(saved_config);
    json_object_set_string_member(saved_settings, "title", "Saved native programme");
    json_object_remove_member(saved_settings, "queue_player");
    json_object_remove_member(saved_settings, "queue_transition");
    json_object_remove_member(saved_settings, "queue_overlap");
    json_object_set_string_member(archive, "kind", "programme");
    json_object_set_string_member(archive, "name", "Saved native programme");
    json_object_set_member(archive, "config", saved_config);
    gchar *saved_id = g_uuid_string_random(), *archive_key = g_uuid_string_random();
    json_object_set_string_member(archive, "document_id", saved_id);
    json_object_set_int_member(archive, "expected_revision", 0);
    json_object_set_string_member(archive, "idempotency_key", archive_key);
    JsonNode *saved = request("/v1/workflows/desktop/put", archive_body);
    json_node_free(archive_body); g_free(saved_id); g_free(archive_key);
    g_assert_cmpuint(count_documents(), ==, names + 1);
    g_assert_cmpuint(json_array_get_length(events(saved)), ==, count + 1);
    select_programme(h, "Saved native programme");
    ProgrammeRevision expected = {h, radios_data_document_revision(radios_broadcast_programme_document(&h->ctx))};
    click(h, "broadcast-catalog-replace"); GtkWidget *load = dialog("programme-library-load-dialog"); g_assert_nonnull(load); gtk_dialog_response(GTK_DIALOG(load), GTK_RESPONSE_ACCEPT);
    wait_for(revision_advanced, &expected, "saved copy replaces current draft atomically"); JsonNode *loaded = document("Mevcut program");
    g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(loaded), "config"), json_object_get_member(json_node_get_object(saved), "config")));
    select_programme(h, "Saved native programme"); drop_selected_programme(h);
    JsonNode *dropped = document("Mevcut program");
    g_assert_cmpuint(json_array_get_length(events(dropped)), ==, 2 * (count + 1));
    JsonArray *saved_blocks = json_object_get_array_member(json_object_get_object_member(json_node_get_object(saved), "config"), "blocks");
    JsonArray *dropped_blocks = json_object_get_array_member(json_object_get_object_member(json_node_get_object(dropped), "config"), "blocks");
    g_assert_cmpuint(json_array_get_length(dropped_blocks), ==, json_array_get_length(saved_blocks) + 1);
    JsonObject *added_block = json_array_get_object_element(dropped_blocks, json_array_get_length(dropped_blocks) - 1);
    g_assert_cmpstr(radios_json_string(added_block, "kind", ""), ==, "program");
    g_assert_cmpstr(radios_json_string(added_block, "title", ""), ==, "Saved native programme");
    for (guint index = 0; index <= count; ++index) {
        JsonObject *source = json_array_get_object_element(events(saved), index);
        JsonObject *inserted_copy = json_array_get_object_element(events(dropped), count + 1 + index);
        g_assert_cmpstr(radios_json_string(inserted_copy, "block_id", ""), ==, radios_json_string(added_block, "id", ""));
        g_assert_cmpstr(radios_json_string(inserted_copy, "id", ""), !=, radios_json_string(source, "id", ""));
        g_assert_cmpstr(radios_json_string(inserted_copy, "title", ""), ==, radios_json_string(source, "title", ""));
        g_assert_cmpstr(radios_programme_queue_row_id(gtk_list_box_get_row_at_index(queue_list(h), (gint)index)), ==,
            radios_json_string(inserted_copy, "id", ""));
    }
    g_assert_null(gtk_list_box_get_row_at_index(queue_list(h), (gint)count + 1));
    JsonNode *source_unchanged = document("Saved native programme");
    g_assert_true(json_node_equal(source_unchanged, saved)); json_node_free(source_unchanged);
    json_node_free(loaded); loaded = dropped;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(h->page, "broadcast-kind-playlist")), TRUE);
    GtkWidget *create = find(h->page, "broadcast-library-new-button");
    GtkWidget *query = find(h->page, "broadcast-library-query");
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(create)), ==, "Playlist oluştur");
    g_assert_true(gtk_widget_get_mapped(create));
    g_assert_true(gtk_widget_get_parent(create) == gtk_widget_get_parent(query));
    JsonNode *payload = body(), *initial_lists = request("/v1/workflows/desktop-playlists/list", payload); json_node_free(payload);
    click(h, "broadcast-library-new-button"); GtkWidget *created = dialog("broadcast-library-new"); g_assert_nonnull(created);
    g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(created)), ==, "Playlist oluştur");
    gtk_entry_set_text(GTK_ENTRY(find(created, "broadcast-library-new-name")), "Cancelled native playlist");
    gtk_dialog_response(GTK_DIALOG(created), GTK_RESPONSE_CANCEL); g_assert_null(dialog("broadcast-library-new"));
    payload = body(); JsonNode *cancelled_lists = request("/v1/workflows/desktop-playlists/list", payload); json_node_free(payload);
    g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(initial_lists), "playlists"),
        json_object_get_member(json_node_get_object(cancelled_lists), "playlists")));
    json_node_free(initial_lists); json_node_free(cancelled_lists);
    gtk_entry_set_text(GTK_ENTRY(query), "Search excludes new playlist"); g_signal_emit_by_name(query, "activate");
    click(h, "broadcast-library-new-button"); created = dialog("broadcast-library-new"); g_assert_nonnull(created);
    gtk_entry_set_text(GTK_ENTRY(find(created, "broadcast-library-new-name")), "  Embedded native playlist  ");
    gtk_dialog_response(GTK_DIALOG(created), GTK_RESPONSE_ACCEPT);
    wait_for(library_selected, h, "playlist created and selected in the broadcast panel");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(query)), ==, "");
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(find(h->page, "broadcast-catalog-content"))), ==, "playlists");
    payload = body(); JsonNode *lists = request("/v1/workflows/desktop-playlists/list", payload); json_node_free(payload);
    JsonArray *records = json_object_get_array_member(json_node_get_object(lists), "playlists"); gchar *playlist_id = NULL;
    for (guint index = 0; index < json_array_get_length(records); ++index) {
        JsonObject *record = json_array_get_object_element(records, index);
        if (g_str_equal(radios_json_string(record, "name", ""), "Embedded native playlist")) playlist_id = g_strdup(radios_json_string(record, "_id", ""));
    }
    g_assert_nonnull(playlist_id); json_node_free(lists);
    GtkListBoxRow *selected = gtk_list_box_get_selected_row(GTK_LIST_BOX(find(h->page, "broadcast-library-playlists")));
    g_assert_cmpstr(g_object_get_data(G_OBJECT(selected), "library-id"), ==, playlist_id);
    close_harness(h); h = open_harness();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(h->page, "broadcast-kind-playlist")), TRUE);
    LibraryRecord record = {h, playlist_id}; wait_for(library_record_loaded, &record, "created playlist visible after reopening the broadcast Playlist tab");
    gtk_list_box_select_row(GTK_LIST_BOX(find(h->page, "broadcast-library-playlists")), library_record_row(&record));
    g_assert_true(gtk_widget_get_mapped(find(h->page, "broadcast-library-new-button"))); g_free(playlist_id);
    select_programme(h, "Saved native programme");
    JsonNode *reopened = document("Mevcut program"); g_assert_true(json_node_equal(reopened, loaded)); g_assert_null(h->ctx.playout);
    close_harness(h); json_node_free(reopened); json_node_free(saved); json_node_free(loaded); json_node_free(before); json_node_free(after); g_free(event_id); g_free(block_id);
}
static JsonNode *media_detail(const gchar *title)
{
    JsonNode *payload = body(); json_object_set_string_member(json_node_get_object(payload), "query", title); JsonNode *rows = request("/v1/workflows/desktop-media/list", payload); json_node_free(payload);
    JsonArray *records = json_object_get_array_member(json_node_get_object(rows), "records"); g_assert_cmpuint(json_array_get_length(records), ==, 1);
    JsonObject *media = json_object_get_object_member(json_array_get_object_element(records, 0), "media"); payload = body(); json_object_set_string_member(json_node_get_object(payload), "media_id", radios_json_string(media, "_id", ""));
    JsonNode *result = request("/v1/workflows/desktop-media/get", payload); json_node_free(payload); json_node_free(rows); return result;
}
static gboolean playlists_loaded(gpointer value)
{
    GtkWidget *lists = find(((Harness *)value)->page, "broadcast-library-playlists");
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(GTK_LIST_BOX(lists), 0);
    return row && g_object_get_data(G_OBJECT(row), "library-id") != NULL;
}
static JsonNode *playlist_fixture(const gchar *name)
{
    JsonNode *payload = body(); JsonObject *object = json_node_get_object(payload);
    json_object_set_string_member(object, "name", name);
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    JsonNode *playlist = request("/v1/workflows/desktop-playlists/create", payload); json_node_free(payload);
    const gchar *titles[] = {"Temporary A", "Temporary B"};
    for (guint index = 0; index < G_N_ELEMENTS(titles); ++index) {
        JsonNode *detail = media_detail(titles[index]); JsonObject *source = json_node_get_object(detail);
        JsonObject *media = json_object_get_object_member(source, "media"), *version = json_object_get_object_member(source, "version");
        JsonObject *record = json_node_get_object(playlist);
        payload = body(); object = json_node_get_object(payload);
        json_object_set_string_member(object, "playlist_id", radios_json_string(record, "_id", ""));
        json_object_set_int_member(object, "expected_revision", radios_json_int(record, "_rev", 0));
        JsonObject *metadata = json_object_new();
        const gchar *names[] = {"title", "artist", "album", "genre"};
        for (guint field = 0; field < G_N_ELEMENTS(names); ++field)
            json_object_set_string_member(metadata, names[field], radios_json_string(media, names[field], ""));
        json_object_set_string_member(metadata, "key", "");
        json_object_set_string_member(metadata, "location", "");
        json_object_set_string_member(metadata, "comment", "");
        json_object_set_double_member(metadata, "bpm", 120);
        json_object_set_int_member(metadata, "duration_ms", radios_json_int(version, "duration_ms", 0));
        json_object_set_string_member(metadata, "media_id", radios_json_string(media, "_id", ""));
        json_object_set_string_member(metadata, "version_id", radios_json_string(version, "_id", ""));
        json_object_set_string_member(metadata, "media_file_id", radios_json_string(version, "media_file_id", ""));
        json_object_set_string_member(metadata, "media_sha256", radios_json_string(version, "sha256", ""));
        json_object_set_object_member(object, "metadata", metadata);
        key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
        JsonNode *next = request("/v1/workflows/desktop-playlists/entry-save", payload);
        json_node_free(payload); json_node_free(detail); json_node_free(playlist); playlist = next;
    }
    return playlist;
}
static void test_playlist_tab_and_programme_toolbar(void)
{
    JsonNode *playlist = playlist_fixture("Native Playlist tab");
    Harness *h = open_harness();
    GtkWidget *header = find(h->page, "broadcast-catalog-header");
    GtkWidget *toolbar = find(header, "programme-toolbar");
    g_assert_nonnull(toolbar);
    g_assert_null(find(toolbar, "programme-properties"));
    g_assert_null(find(toolbar, "programme-save"));
    JsonNode *before = document("Mevcut program");
    JsonObject *config_object = json_object_get_object_member(json_node_get_object(before), "config");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(toolbar, "programme-title"))), ==, radios_json_string(config_object, "title", ""));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(toolbar, "programme-date"))), ==, radios_json_string(config_object, "date", ""));
    g_assert_null(find(toolbar, "programme-options"));
    g_assert_null(find(h->page, "programme-controls"));
    g_assert_null(find(h->page, "broadcast-catalog-source-summary"));
    g_assert_null(find(h->page, "broadcast-media-library"));
    g_assert_null(find(h->page, "broadcast-programme-archive"));
    GtkWidget *create_selected = find(h->page, "broadcast-catalog-create-playlist");
    g_assert_true(GTK_IS_BUTTON(create_selected));
    GtkWidget *library_toolbar = gtk_widget_get_parent(create_selected); g_assert_true(GTK_IS_BOX(library_toolbar));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(create_selected)), ==, "Seçilenlerle playlist oluştur");
    g_assert_true(gtk_widget_get_mapped(create_selected));
    GList *tools = gtk_container_get_children(GTK_CONTAINER(library_toolbar));
    g_assert_cmpuint(g_list_length(tools), ==, 2);
    g_assert_true(find(library_toolbar, "broadcast-catalog-count") != NULL);
    g_list_free(tools);
    GtkWidget *stack = find(h->page, "broadcast-catalog-content");
    GtkWidget *panel = find(h->page, "broadcast-playlist-tools");
    g_assert_true(GTK_IS_BOX(panel));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(stack)), ==, "media");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(header, "broadcast-kind-playlist")), TRUE);
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(stack)), ==, "playlists");
    wait_for(playlists_loaded, h, "saved playlists loaded directly in Playlist tab");
    g_assert_true(gtk_widget_get_mapped(panel));
    g_assert_false(gtk_widget_get_mapped(find(h->page, "broadcast-media-table")));
    g_assert_true(gtk_widget_get_mapped(toolbar));
    GtkListBox *lists = GTK_LIST_BOX(find(panel, "broadcast-library-playlists"));
    GList *rows = gtk_container_get_children(GTK_CONTAINER(lists));
    for (GList *item = rows; item; item = item->next)
        if (!g_strcmp0(g_object_get_data(G_OBJECT(item->data), "library-id"), radios_json_string(json_node_get_object(playlist), "_id", "")))
            gtk_list_box_select_row(lists, GTK_LIST_BOX_ROW(item->data));
    g_list_free(rows);
    wait_for(library_selected, h, "playlist selectable without expanding a separate panel");
    GtkListBox *tracks = GTK_LIST_BOX(find(panel, "broadcast-library-tracks"));
    JsonArray *entries = json_object_get_array_member(json_node_get_object(playlist), "tracks");
    for (guint index = 0; index < json_array_get_length(entries); ++index) {
        GtkListBoxRow *row = gtk_list_box_get_row_at_index(tracks, (gint)index);
        g_assert_nonnull(row);
        g_assert_cmpstr(g_object_get_data(G_OBJECT(row), "library-id"), ==, radios_json_string(json_array_get_object_element(entries, index), "_id", ""));
    }
    g_assert_null(gtk_list_box_get_row_at_index(tracks, 2));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(header, "broadcast-kind-music")), TRUE); run_for(30);
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(stack)), ==, "media");
    g_assert_false(gtk_widget_get_mapped(panel));
    g_assert_true(gtk_widget_get_mapped(find(h->page, "broadcast-media-table")));
    JsonNode *after = document("Mevcut program");
    g_assert_true(json_node_equal(before, after)); g_assert_null(h->ctx.playout);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(header, "broadcast-kind-playlist")), TRUE);
    wait_for(library_selected, h, "playlist selection retained on return");
    ProgrammeRevision expected = {h, radios_data_document_revision(radios_broadcast_programme_document(&h->ctx))};
    click(h, "broadcast-library-add-all");
    wait_for(revision_advanced, &expected, "playlist tracks persisted as a new flow event");
    JsonNode *added = document("Mevcut program");
    guint previous = json_array_get_length(events(before));
    g_assert_cmpuint(json_array_get_length(events(added)), ==, previous + 2);
    JsonObject *first = json_array_get_object_element(events(added), previous), *second = json_array_get_object_element(events(added), previous + 1);
    g_assert_cmpstr(radios_json_string(first, "title", ""), ==, "Temporary A");
    g_assert_cmpstr(radios_json_string(second, "title", ""), ==, "Temporary B");
    g_assert_cmpstr(radios_json_string(first, "block_id", ""), ==, radios_json_string(second, "block_id", ""));
    g_assert_null(h->ctx.playout);
    json_node_free(playlist); json_node_free(added); json_node_free(before); json_node_free(after); close_harness(h);
}
static void test_stale_selection_sha_cas_and_close(void)
{
    Harness *h = open_harness(); JsonNode *before = document("Mevcut program");
    if (json_array_get_length(events(before)) < 2) {
        select_media(h, "Temporary B");
        ProgrammeRevision expected = {h, radios_data_document_revision(radios_broadcast_programme_document(&h->ctx))};
        click(h, "broadcast-catalog-insert"); wait_for(revision_advanced, &expected, "second target seeded in this test's own fixture");
        json_node_free(before); before = document("Mevcut program");
    }
    select_media(h, "Temporary A");
    gchar *first = g_strdup(radios_json_string(json_array_get_object_element(events(before), 0), "id", ""));
    gchar *second = g_strdup(radios_json_string(json_array_get_object_element(events(before), 1), "id", ""));
    select_item(h, first);
    flag(".catalogue-ready", FALSE); flag(".catalogue-hold", TRUE); click(h, "broadcast-catalog-replace"); wait_for(held, NULL, "verified source response held");
    select_item(h, second); flag(".catalogue-hold", FALSE); wait_for(catalogue_idle, h, "target selection changed response rejected"); g_free(first); g_free(second);
    JsonNode *after = document("Mevcut program"); g_assert_true(json_node_equal(before, after)); json_node_free(after);
    JsonNode *detail = media_detail("Temporary A"); const gchar *path = radios_json_string(json_object_get_object_member(json_node_get_object(detail), "version"), "managed_path", "");
    gchar *bytes = NULL; gsize length = 0; g_assert_true(g_file_get_contents(path, &bytes, &length, NULL)); g_assert_true(g_file_set_contents(path, "altered", -1, NULL));
    click(h, "broadcast-catalog-insert"); wait_for(catalogue_idle, h, "real corrupt source SHA blocks insertion"); after = document("Mevcut program"); g_assert_true(json_node_equal(before, after)); json_node_free(after);
    g_assert_true(g_file_set_contents(path, bytes, length, NULL)); g_free(bytes); json_node_free(detail);
    flag(".catalogue-ready", FALSE); flag(".catalogue-hold", TRUE); click(h, "broadcast-catalog-insert"); wait_for(held, NULL, "source held before external target CAS change");
    JsonNode *external = copy(before); JsonObject *cfg = json_object_get_object_member(json_node_get_object(external), "config"); json_object_set_string_member(cfg, "note", "Independent writer preserved"); put(external); json_node_free(external);
    flag(".catalogue-hold", FALSE); gint64 end = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!dialog("database-reload-dialog") && g_get_monotonic_time() < end) pump();
    g_assert_nonnull(dialog("database-reload-dialog"));
    gtk_dialog_response(GTK_DIALOG(dialog("database-reload-dialog")), GTK_RESPONSE_CANCEL);
    after = document("Mevcut program"); g_assert_cmpuint(json_array_get_length(events(after)), ==, json_array_get_length(events(before)));
    g_assert_cmpstr(radios_json_string(json_object_get_object_member(json_node_get_object(after), "config"), "note", ""), ==, "Independent writer preserved");
    close_harness(h); h = open_harness(); select_media(h, "Temporary B"); flag(".catalogue-ready", FALSE); flag(".catalogue-hold", TRUE);
    click(h, "broadcast-catalog-insert"); wait_for(held, NULL, "source verification held before owner close"); close_harness(h); flag(".catalogue-hold", FALSE); run_for(200);
    JsonNode *closed = document("Mevcut program"); g_assert_true(json_node_equal(closed, after)); json_node_free(closed); json_node_free(after); json_node_free(before); flag(".catalogue-ready", FALSE);
}
static JsonNode *programme_seed(JsonNode *playlist)
{
    JsonNode *record = document("Mevcut program"); JsonObject *config_object = json_object_get_object_member(json_node_get_object(record), "config");
    json_object_set_array_member(config_object, "blocks", json_array_new());
    json_object_set_array_member(config_object, "events", json_array_new());
    json_object_remove_member(config_object, "timing");
    RadioSProgrammeTree *tree = radios_programme_tree_new(config_object);
    radios_programme_tree_add_block(tree, 0, "music", "Otomasyondan gelen playlist");
    radios_programme_tree_add_block(tree, 1, "music", "Sıradaki event");
    JsonArray *tracks = json_object_get_array_member(json_node_get_object(playlist), "tracks");
    const gchar *keys[] = {"title", "artist", "album", "duration_ms", "media_id", "version_id", "media_file_id", "media_sha256"};
    const gchar *blank[] = {"start_time", "version_label", "primary_file", "backup_file", "note"};
    for (guint index = 0; index < 3; ++index) {
        JsonObject *source = json_array_get_object_element(tracks, MIN(index, 1u)), *leaf = json_object_new();
        for (guint field = 0; field < G_N_ELEMENTS(keys); ++field)
            json_object_set_member(leaf, keys[field], json_node_copy(json_object_get_member(source, keys[field])));
        for (guint field = 0; field < G_N_ELEMENTS(blank); ++field) json_object_set_string_member(leaf, blank[field], "");
        gchar *id = g_uuid_string_random(); json_object_set_string_member(leaf, "id", id); g_free(id);
        json_object_set_string_member(leaf, "kind", "music");
        radios_programme_tree_insert(tree, index < 2 ? 0 : 1, index < 2 ? index : 0, leaf);
    }
    JsonNode *config_node = radios_programme_tree_config(tree, config_object); radios_programme_tree_free(tree);
    json_object_set_member(json_node_get_object(record), "config", config_node); put(record); json_node_free(record);
    return document("Mevcut program");
}
static GtkListBoxRow *library_select(Harness *h, const gchar *id)
{
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(h->page, "broadcast-kind-playlist")), TRUE);
    wait_for(playlists_loaded, h, "drag source playlists loaded");
    GtkListBox *list = GTK_LIST_BOX(find(h->page, "broadcast-library-playlists"));
    GList *rows = gtk_container_get_children(GTK_CONTAINER(list)); GtkListBoxRow *selected = NULL;
    for (GList *item = rows; item && !selected; item = item->next)
        if (!g_strcmp0(g_object_get_data(G_OBJECT(item->data), "library-id"), id)) selected = item->data;
    g_list_free(rows); g_assert_nonnull(selected); gtk_list_box_select_row(list, selected); run_for(30); return selected;
}
static void source_drop(Harness *h, GtkWidget *source, RadioSDragInfo info, gint target_y)
{
    g_test_message("Real drag source type %u at queue coordinate %d", info, target_y);
    g_object_ref(source);
    if (GTK_IS_EVENT_BOX(source)) g_signal_emit_by_name(source, "drag-begin", NULL);
    guint count = 0; const GtkTargetEntry *targets = radios_drag_targets(&count); g_assert_cmpuint(info, <=, count);
    GtkTargetList *advertised = gtk_drag_source_get_target_list(source); guint advertised_info = 0;
    g_assert_true(gtk_target_list_find(advertised, gdk_atom_intern_static_string(targets[info - 1].target), &advertised_info));
    g_assert_cmpuint(advertised_info, ==, info);
    GtkClipboard *clipboard = gtk_clipboard_get(gdk_atom_intern_static_string("RADIOS_PLAYLIST_DROP_TEST"));
    gulong stop = GTK_IS_TREE_VIEW(source) ? g_signal_connect(source, "drag-data-get", G_CALLBACK(programme_drag_stop), NULL) : 0;
    g_assert_true(gtk_clipboard_set_with_data(clipboard, &targets[info - 1], 1, programme_drag_get, NULL, source));
    GtkSelectionData *selection = gtk_clipboard_wait_for_contents(clipboard, gdk_atom_intern_static_string(targets[info - 1].target));
    if (stop) g_signal_handler_disconnect(source, stop);
    g_assert_nonnull(selection); drop_accepted = FALSE;
    g_signal_emit_by_name(queue_list(h), "drag-data-received", NULL, 0, target_y, selection, info, GDK_CURRENT_TIME);
    gtk_selection_data_free(selection); gtk_clipboard_clear(clipboard);
    if (GTK_IS_EVENT_BOX(source)) g_signal_emit_by_name(source, "drag-end", NULL);
    g_object_unref(source); g_assert_true(drop_accepted);
}
static void open_flow_event(Harness *h, guint index)
{
    GtkListBoxRow *row = gtk_list_box_get_row_at_index(queue_list(h), (gint)index); g_assert_nonnull(row);
    g_signal_emit_by_name(queue_list(h), "row-activated", row); run_for(30);
    g_assert_true(gtk_widget_get_mapped(find(h->page, "programme-queue-back")));
}
static gboolean row_allocated(gpointer value)
{
    GtkWidget *row = value; GtkAllocation allocation; gtk_widget_get_allocation(row, &allocation);
    return gtk_widget_get_mapped(row) && allocation.height > 1 &&
        gtk_list_box_get_row_at_y(GTK_LIST_BOX(gtk_widget_get_parent(row)), allocation.y + allocation.height / 2) == GTK_LIST_BOX_ROW(row);
}
static void assert_opened_tracks(Harness *h, JsonNode *record, guint count)
{
    for (guint index = 0; index < count; ++index)
        g_assert_cmpstr(radios_programme_queue_row_id(gtk_list_box_get_row_at_index(queue_list(h), (gint)index)), ==,
            radios_json_string(json_array_get_object_element(events(record), index), "id", ""));
    g_assert_null(gtk_list_box_get_row_at_index(queue_list(h), (gint)count));
}
static void test_playlist_drop_after_event_and_into_opened_event(void)
{
    JsonNode *playlist = playlist_fixture("Native drag playlist"), *seed = programme_seed(playlist); Harness *h = open_harness();
    GtkListBoxRow *source_row = library_select(h, radios_json_string(json_node_get_object(playlist), "_id", ""));
    GtkWidget *source = gtk_bin_get_child(GTK_BIN(source_row));
    GtkAllocation target; gtk_widget_get_allocation(GTK_WIDGET(gtk_list_box_get_row_at_index(queue_list(h), 0)), &target);
    ProgrammeRevision expected = {h, radios_data_document_revision(radios_broadcast_programme_document(&h->ctx))};
    source_drop(h, source, RADIOS_DRAG_INFO_PLAYLIST, target.y + 1);
    wait_for(revision_advanced, &expected, "playlist appended after the existing automation event");
    JsonNode *after = document("Mevcut program"); JsonObject *config_object = json_object_get_object_member(json_node_get_object(after), "config");
    JsonArray *blocks = json_object_get_array_member(config_object, "blocks");
    g_assert_cmpuint(json_array_get_length(blocks), ==, 3); g_assert_cmpuint(json_array_get_length(events(after)), ==, 5);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(blocks, 0), "title", ""), ==, "Otomasyondan gelen playlist");
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(blocks, 1), "title", ""), ==, "Native drag playlist");
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(blocks, 2), "title", ""), ==, "Sıradaki event");
    g_assert_true(json_node_equal(json_array_get_element(events(seed), 0), json_array_get_element(events(after), 0)));
    g_assert_true(json_node_equal(json_array_get_element(events(seed), 1), json_array_get_element(events(after), 1)));
    g_assert_true(json_node_equal(json_array_get_element(events(seed), 2), json_array_get_element(events(after), 4)));
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(events(after), 2), "title", ""), ==, "Temporary A");
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(events(after), 3), "title", ""), ==, "Temporary B");
    open_flow_event(h, 0);
    GtkWidget *target_row = GTK_WIDGET(gtk_list_box_get_row_at_index(queue_list(h), 0));
    wait_for(row_allocated, target_row, "opened event row allocated before computing the drop position");
    gtk_widget_get_allocation(target_row, &target);
    g_assert_true(gtk_list_box_get_row_at_y(queue_list(h), target.y + target.height - 1) == GTK_LIST_BOX_ROW(target_row));
    expected.revision = radios_data_document_revision(radios_broadcast_programme_document(&h->ctx));
    source_drop(h, source, RADIOS_DRAG_INFO_PLAYLIST, target.y + target.height - 1);
    wait_for(revision_advanced, &expected, "whole playlist inserted directly into opened event");
    JsonNode *inside = document("Mevcut program"); config_object = json_object_get_object_member(json_node_get_object(inside), "config");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(config_object, "blocks")), ==, 3);
    const gchar *titles[] = {"Temporary A", "Temporary A", "Temporary B", "Temporary B"};
    for (guint index = 0; index < G_N_ELEMENTS(titles); ++index) {
        JsonObject *leaf = json_array_get_object_element(events(inside), index);
        g_assert_cmpstr(radios_json_string(leaf, "title", ""), ==, titles[index]);
        g_assert_cmpstr(radios_json_string(leaf, "block_id", ""), ==, radios_json_string(json_array_get_object_element(events(seed), 0), "block_id", ""));
    }
    assert_opened_tracks(h, inside, 4);
    g_assert_cmpstr(radios_programme_queue_row_id(gtk_list_box_get_selected_row(queue_list(h))), ==,
        radios_json_string(json_array_get_object_element(events(inside), 1), "id", ""));
    for (guint index = 2; index < 5; ++index)
        g_assert_true(json_node_equal(json_array_get_element(events(after), index), json_array_get_element(events(inside), index + 2)));
    GtkListBoxRow *track = gtk_list_box_get_row_at_index(GTK_LIST_BOX(find(h->page, "broadcast-library-tracks")), 0);
    expected.revision = radios_data_document_revision(radios_broadcast_programme_document(&h->ctx));
    source_drop(h, gtk_bin_get_child(GTK_BIN(track)), RADIOS_DRAG_INFO_PLAYLIST, 10000);
    wait_for(revision_advanced, &expected, "single dragged playlist track appended inside the open event");
    JsonNode *single = document("Mevcut program"); assert_opened_tracks(h, single, 5);
    g_assert_cmpuint(json_array_get_length(events(single)), ==, 8);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(events(single), 4), "title", ""), ==, "Temporary A");
    JsonNode *payload = body(); JsonObject *object = json_node_get_object(payload);
    json_object_set_string_member(object, "kind", "programme"); json_object_set_string_member(object, "name", "Native dropped programme");
    json_object_set_member(object, "config", copy(json_object_get_member(json_node_get_object(seed), "config")));
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    JsonNode *saved = request("/v1/workflows/desktop/put", payload); json_node_free(payload);
    select_programme(h, "Native dropped programme");
    expected.revision = radios_data_document_revision(radios_broadcast_programme_document(&h->ctx));
    source_drop(h, g_object_get_data(G_OBJECT(find(h->page, "broadcast-media-table")), "tree-view"), RADIOS_DRAG_INFO_PROGRAMME, 10000);
    wait_for(revision_advanced, &expected, "saved programme contents dropped into opened event rather than another event");
    JsonNode *merged = document("Mevcut program"); assert_opened_tracks(h, merged, 8);
    g_assert_cmpuint(json_array_get_length(events(merged)), ==, 11);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_object_get_object_member(json_node_get_object(merged), "config"), "blocks")), ==, 3);
    g_assert_null(h->ctx.playout); close_harness(h); h = open_harness();
    JsonNode *reopened = document("Mevcut program"); g_assert_true(json_node_equal(reopened, merged)); g_assert_null(h->ctx.playout);
    close_harness(h); json_node_free(reopened); json_node_free(merged); json_node_free(saved); json_node_free(single);
    json_node_free(inside); json_node_free(after); json_node_free(seed); json_node_free(playlist);
}
static gboolean library_idle(gpointer value)
{ return gtk_widget_is_sensitive(find(((Harness *)value)->page, "broadcast-library-add-all")); }
static void test_playlist_drop_delayed_target_and_close(void)
{
    JsonNode *playlist = playlist_fixture("Native delayed playlist"), *before = programme_seed(playlist); Harness *h = open_harness();
    GtkListBoxRow *row = library_select(h, radios_json_string(json_node_get_object(playlist), "_id", ""));
    GtkWidget *source = gtk_bin_get_child(GTK_BIN(row)); open_flow_event(h, 0);
    flag(".catalogue-ready", FALSE); flag(".catalogue-hold", TRUE);
    source_drop(h, source, RADIOS_DRAG_INFO_PLAYLIST, 10000); wait_for(held, NULL, "actual playlist preparation held before drop target changes");
    gtk_button_clicked(GTK_BUTTON(find(h->page, "programme-queue-back"))); run_for(30); open_flow_event(h, 1);
    flag(".catalogue-hold", FALSE); wait_for(library_idle, h, "stale drop target rejected");
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(h->page, "broadcast-library-status"))), "Hedef program"));
    JsonNode *after = document("Mevcut program"); g_assert_true(json_node_equal(before, after)); json_node_free(after);
    flag(".catalogue-ready", FALSE); flag(".catalogue-hold", TRUE);
    source_drop(h, source, RADIOS_DRAG_INFO_PLAYLIST, 10000); wait_for(held, NULL, "actual playlist preparation held before window close");
    close_harness(h); flag(".catalogue-hold", FALSE); run_for(200);
    after = document("Mevcut program"); g_assert_true(json_node_equal(before, after));
    json_node_free(after); json_node_free(before); json_node_free(playlist); flag(".catalogue-ready", FALSE);
}
static gboolean selected_player_queue_loaded(gpointer value)
{
    Harness *harness = value;
    RadioSDocument *document = radios_broadcast_programme_document(&harness->ctx);
    return radios_data_connected(&harness->ctx) && document && radios_data_document_ready(document);
}
static void select_player_queue(Harness *harness, const gchar *player)
{
    GtkWidget *selector = find(harness->page, "programme-player-selector");
    g_assert_true(GTK_IS_COMBO_BOX(selector)); g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(selector), player));
    wait_for(selected_player_queue_loaded, harness, "selected player queue loaded");
}
static void test_player_queues_database_switch_drop_and_reopen(void)
{
    JsonNode *playlist = playlist_fixture("Native independent playlist"); Harness *harness = open_harness();
    JsonNode *general = document("Mevcut program"); select_player_queue(harness, "1");
    click(harness, "programme-add-music");
    wait_for(ready, harness, "first native edit saved before playlist drop");
    GtkListBoxRow *row = library_select(harness, radios_json_string(json_node_get_object(playlist), "_id", ""));
    ProgrammeRevision expected = {harness, radios_data_document_revision(radios_broadcast_programme_document(&harness->ctx))};
    source_drop(harness, gtk_bin_get_child(GTK_BIN(row)), RADIOS_DRAG_INFO_PLAYLIST, 10000);
    wait_for(revision_advanced, &expected, "playlist dropped into player one only");
    expected.revision = radios_data_document_revision(radios_broadcast_programme_document(&harness->ctx));
    gtk_combo_box_set_active(GTK_COMBO_BOX(find(harness->page, "programme-transition-mode")), 3);
    wait_for(revision_advanced, &expected, "player one manual transition persisted");
    JsonNode *first = document("Oynatıcı 1 yayın sırası");
    g_assert_cmpuint(json_array_get_length(events(first)), ==, 2);
    g_assert_cmpint(radios_json_int(json_object_get_object_member(json_node_get_object(first), "config"), "queue_player", 0), ==, 1);
    select_player_queue(harness, "2");
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(find(harness->page, "programme-transition-mode"))), ==, 0);
    g_assert_false(gtk_widget_is_sensitive(find(harness->page, "programme-transition-overlap")));
    expected.revision = radios_data_document_revision(radios_broadcast_programme_document(&harness->ctx));
    click(harness, "programme-add-announcement");
    wait_for(revision_advanced, &expected, "player two event persisted separately");
    JsonNode *second = document("Oynatıcı 2 yayın sırası");
    g_assert_cmpstr(radios_json_string(json_node_get_object(first), "_id", ""), !=, radios_json_string(json_node_get_object(second), "_id", ""));
    select_player_queue(harness, "1");
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(find(harness->page, "programme-transition-mode"))), ==, 3);
    JsonNode *unchanged = document("Oynatıcı 1 yayın sırası"); g_assert_true(json_node_equal(first, unchanged)); json_node_free(unchanged);
    row = library_select(harness, radios_json_string(json_node_get_object(playlist), "_id", ""));
    flag(".catalogue-ready", FALSE); flag(".catalogue-hold", TRUE);
    source_drop(harness, gtk_bin_get_child(GTK_BIN(row)), RADIOS_DRAG_INFO_PLAYLIST, 10000);
    wait_for(held, NULL, "playlist preparation held before selected player changes");
    select_player_queue(harness, "2"); flag(".catalogue-hold", FALSE); wait_for(library_idle, harness, "delayed drop cannot migrate to player two");
    unchanged = document("Oynatıcı 1 yayın sırası"); g_assert_true(json_node_equal(first, unchanged)); json_node_free(unchanged);
    unchanged = document("Oynatıcı 2 yayın sırası"); g_assert_true(json_node_equal(second, unchanged)); json_node_free(unchanged);
    select_player_queue(harness, "0"); unchanged = document("Mevcut program"); g_assert_true(json_node_equal(general, unchanged)); json_node_free(unchanged);
    close_harness(harness); harness = open_harness();
    select_player_queue(harness, "1"); unchanged = document("Oynatıcı 1 yayın sırası"); g_assert_true(json_node_equal(first, unchanged)); json_node_free(unchanged);
    select_player_queue(harness, "2"); unchanged = document("Oynatıcı 2 yayın sırası"); g_assert_true(json_node_equal(second, unchanged)); json_node_free(unchanged);
    close_harness(harness); json_node_free(first); json_node_free(second); json_node_free(general); json_node_free(playlist); flag(".catalogue-ready", FALSE);
}
typedef struct { Harness *harness; guint count; const gchar *first; } CatalogueRows;
static gboolean catalogue_rows(gpointer value)
{
    CatalogueRows *wanted = value;
    GtkTreeModel *model = GTK_TREE_MODEL(g_object_get_data(G_OBJECT(find(wanted->harness->page, "broadcast-media-table")), "list-store"));
    if (gtk_tree_model_iter_n_children(model, NULL) != (gint)wanted->count) return FALSE;
    GtkTreeIter iter; gchar *title = NULL;
    if (!wanted->count) return TRUE;
    if (!gtk_tree_model_get_iter_first(model, &iter)) return FALSE;
    gtk_tree_model_get(model, &iter, 1, &title, -1);
    gboolean matches = !g_strcmp0(title, wanted->first); g_free(title); return matches;
}
static GtkTreeSelection *catalogue_selection(Harness *h)
{
    GtkTreeView *tree = g_object_get_data(G_OBJECT(find(h->page, "broadcast-media-table")), "tree-view");
    return gtk_tree_view_get_selection(tree);
}
static void select_pair(Harness *h, guint first, guint second)
{
    GtkTreeSelection *selection = catalogue_selection(h);
    g_assert_cmpint(gtk_tree_selection_get_mode(selection), ==, GTK_SELECTION_MULTIPLE);
    gtk_tree_selection_unselect_all(selection);
    /* Reversing the click order must still insert the table's visible order. */
    GtkTreePath *path = gtk_tree_path_new_from_indices((gint)second, -1);
    gtk_tree_selection_select_path(selection, path); gtk_tree_path_free(path);
    path = gtk_tree_path_new_from_indices((gint)first, -1);
    gtk_tree_selection_select_path(selection, path); gtk_tree_path_free(path);
    g_assert_cmpint(gtk_tree_selection_count_selected_rows(selection), ==, 2);
}
static void catalogue_query(Harness *h, const gchar *query, guint count, const gchar *first)
{
    GtkWidget *entry = find(h->page, "broadcast-catalog-query");
    gtk_entry_set_text(GTK_ENTRY(entry), query); g_signal_emit_by_name(entry, "activate");
    CatalogueRows wanted = {h, count, first}; wait_for(catalogue_rows, &wanted, "fresh category-filtered rows");
}
static void assert_pair_appended(JsonNode *before, JsonNode *after, const gchar *first, const gchar *second)
{
    guint count = json_array_get_length(events(before));
    g_assert_cmpuint(json_array_get_length(events(after)), ==, count + 2);
    g_assert_cmpint(radios_json_int(json_node_get_object(after), "_rev", 0), ==,
        radios_json_int(json_node_get_object(before), "_rev", 0) + 1);
    for (guint index = 0; index < count; ++index)
        g_assert_true(same_event(json_array_get_element(events(before), index), json_array_get_element(events(after), index)));
    for (guint index = 0; index < 2; ++index) {
        JsonObject *event = json_array_get_object_element(events(after), count + index);
        g_assert_cmpstr(radios_json_string(event, "title", ""), ==, index ? second : first);
        g_assert_cmpint(radios_json_int(event, "duration_ms", 0), ==, 200);
        g_assert_true(g_uuid_string_is_valid(radios_json_string(event, "media_id", "")));
        g_assert_true(g_uuid_string_is_valid(radios_json_string(event, "version_id", "")));
        g_assert_cmpuint(strlen(radios_json_string(event, "media_sha256", "")), ==, 64);
    }
}
static void test_multiple_media_insert_drag_atomic(void)
{
    Harness *h = open_harness(); catalogue_query(h, "Temporary", 2, "Temporary A");
    JsonNode *before = document("Mevcut program");
    select_item(h, radios_json_string(json_array_get_object_element(events(before), 0), "id", ""));
    select_pair(h, 0, 1);
    g_assert_false(gtk_widget_is_sensitive(find(h->page, "broadcast-catalog-replace")));
    g_assert_false(gtk_widget_is_sensitive(find(h->page, "broadcast-catalog-edit")));
    click(h, "broadcast-catalog-insert"); wait_for(catalogue_idle, h, "verified multi insert saved atomically");
    JsonNode *after = document("Mevcut program"); assert_pair_appended(before, after, "Temporary A", "Temporary B");
    json_node_free(before); before = after;
    select_pair(h, 0, 1);
    GtkWidget *source = g_object_get_data(G_OBJECT(find(h->page, "broadcast-media-table")), "tree-view");
    source_drop(h, source, RADIOS_DRAG_INFO_MEDIA, 10000);
    wait_for(catalogue_idle, h, "verified multi drag saved atomically");
    after = document("Mevcut program"); assert_pair_appended(before, after, "Temporary A", "Temporary B");
    json_node_free(before); json_node_free(after); close_harness(h);
}
static void test_multiple_media_stale_selection_and_failed_member(void)
{
    Harness *h = open_harness(); catalogue_query(h, "Temporary", 2, "Temporary A");
    select_pair(h, 0, 1); JsonNode *before = document("Mevcut program");
    flag(".catalogue-ready", FALSE); flag(".catalogue-hold", TRUE);
    click(h, "broadcast-catalog-insert"); wait_for(held, NULL, "first member response held before selection change");
    GtkTreePath *path = gtk_tree_path_new_from_indices(1, -1);
    gtk_tree_selection_unselect_path(catalogue_selection(h), path); gtk_tree_path_free(path);
    flag(".catalogue-hold", FALSE); wait_for(catalogue_idle, h, "changed selection rejects complete batch");
    JsonNode *after = document("Mevcut program"); g_assert_true(json_node_equal(before, after)); json_node_free(after);
    catalogue_query(h, "", 3, "Scope9 Music");
    GtkTreePath *first = gtk_tree_path_new_from_indices(0, -1), *last = gtk_tree_path_new_from_indices(2, -1);
    gtk_tree_selection_select_range(catalogue_selection(h), first, last);
    g_assert_cmpint(gtk_tree_selection_count_selected_rows(catalogue_selection(h)), ==, 3);
    gtk_tree_path_free(first); gtk_tree_path_free(last);
    select_pair(h, 0, 1);
    JsonNode *detail = media_detail("Temporary A");
    const gchar *managed = radios_json_string(json_object_get_object_member(json_node_get_object(detail), "version"), "managed_path", "");
    gchar *bytes = NULL; gsize length = 0;
    g_assert_true(g_file_get_contents(managed, &bytes, &length, NULL));
    g_assert_true(g_file_set_contents(managed, "corrupt second selected file", -1, NULL));
    click(h, "broadcast-catalog-insert"); wait_for(catalogue_idle, h, "failed second SHA prevents partial batch commit");
    after = document("Mevcut program"); g_assert_true(json_node_equal(before, after));
    g_assert_true(g_file_set_contents(managed, bytes, length, NULL));
    g_free(bytes); json_node_free(detail); json_node_free(after); json_node_free(before);
    /* A late batch belongs to its original queue epoch, even after switching
     * back to that same queue. Both queues remain unchanged and can be edited. */
    select_player_queue(h, "1"); JsonNode *player_before = document_optional("Oynatıcı 1 yayın sırası");
    select_player_queue(h, "0"); JsonNode *general_before = document("Mevcut program");
    catalogue_query(h, "Temporary", 2, "Temporary A"); select_pair(h, 0, 1);
    flag(".catalogue-ready", FALSE); flag(".catalogue-hold", TRUE);
    click(h, "broadcast-catalog-insert"); wait_for(held, NULL, "two-media batch held before queue epoch change");
    select_player_queue(h, "1"); select_player_queue(h, "0");
    flag(".catalogue-hold", FALSE); run_for(500);
    wait_for(catalogue_idle, h, "catalogue actions recover after queue switch rejects late batch");
    JsonNode *general_after = document("Mevcut program"), *player_after = document_optional("Oynatıcı 1 yayın sırası");
    g_assert_true(json_node_equal(general_before, general_after));
    g_assert_true(same_document(player_before, player_after));
    json_node_free(general_after); if (player_after) json_node_free(player_after);
    click(h, "broadcast-catalog-insert"); wait_for(catalogue_idle, h, "new batch succeeds after rejecting the previous queue epoch");
    general_after = document("Mevcut program"); assert_pair_appended(general_before, general_after, "Temporary A", "Temporary B");
    json_node_free(general_after); json_node_free(general_before); if (player_before) json_node_free(player_before);
    flag(".catalogue-ready", FALSE); close_harness(h);
}
static void test_all_category_tabs_reset_playlist_scope(void)
{
    JsonNode *playlist = playlist_fixture("Scope9 Playlist"); Harness *h = open_harness();
    JsonNode *before = document("Mevcut program");
    const gchar *tabs[] = {"broadcast-kind-jingle", "broadcast-kind-announcement", "broadcast-kind-advert",
        "broadcast-kind-programme", "broadcast-kind-music"};
    const gchar *titles[] = {"Scope9 Jingle", "Scope9 Anons", "Scope9 Reklam", "Scope9 Program", "Scope9 Music"};
    GtkWidget *query = find(h->page, "broadcast-catalog-query"), *field = find(h->page, "broadcast-catalog-field");
    gtk_entry_set_text(GTK_ENTRY(query), "Scope9");
    for (guint index = 0; index < G_N_ELEMENTS(tabs); ++index) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(field), 1);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(h->page, tabs[index])), TRUE);
        g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(field)), ==, 0);
        g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(find(h->page, "broadcast-catalog-content"))), ==, "media");
        CatalogueRows wanted = {h, 1, titles[index]}; wait_for(catalogue_rows, &wanted, "category replaces Playlist search scope");
        g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(query)), ==, "Scope9");
        g_assert_cmpint(gtk_tree_selection_count_selected_rows(catalogue_selection(h)), ==, 0);
    }
    /* Clicking the already active Parçalar tab must also exit Playlist scope. */
    gtk_combo_box_set_active(GTK_COMBO_BOX(field), 1);
    gtk_button_clicked(GTK_BUTTON(find(h->page, "broadcast-kind-music")));
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(field)), ==, 0);
    CatalogueRows music = {h, 1, "Scope9 Music"}; wait_for(catalogue_rows, &music, "active music tab click resets Playlist scope");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(h->page, "broadcast-kind-playlist")), TRUE);
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(find(h->page, "broadcast-catalog-content"))), ==, "playlists");
    LibraryRecord record = {h, radios_json_string(json_node_get_object(playlist), "_id", "")};
    wait_for(library_record_loaded, &record, "Playlist category displays its own saved lists");
    g_assert_false(gtk_widget_get_mapped(find(h->page, "broadcast-media-table")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(h->page, "broadcast-kind-music")), TRUE);
    CatalogueRows wanted = {h, 1, "Scope9 Music"}; wait_for(catalogue_rows, &wanted, "music content restored after Playlist");
    JsonNode *after = document("Mevcut program"); g_assert_true(json_node_equal(before, after));
    json_node_free(playlist); json_node_free(before); json_node_free(after); close_harness(h);
}
static JsonNode *selection_playlist(const gchar *name)
{
    JsonNode *payload = body(), *result = request("/v1/workflows/desktop-playlists/list", payload);
    json_node_free(payload);
    JsonArray *rows = json_object_get_array_member(json_node_get_object(result), "playlists");
    JsonNode *found = NULL;
    for (guint i = 0; i < json_array_get_length(rows); ++i)
        if (!g_strcmp0(radios_json_string(json_array_get_object_element(rows, i), "name", ""), name))
            found = copy(json_array_get_element(rows, i));
    json_node_free(result); return found;
}
static gboolean selection_playlist_closed(gpointer unused)
{ (void)unused; return dialog("broadcast-selection-playlist-dialog") == NULL; }
extern const gchar *const radios_reference_css_parts[];
static void selected_catalogue_screenshot(Harness *h)
{
    const gchar *path = g_getenv("RADIOS_P10_SCREENSHOT"); if (!path) return;
    GString *css = g_string_new(NULL);
    for (guint i = 0; radios_reference_css_parts[i]; ++i) g_string_append(css, radios_reference_css_parts[i]);
    gchar *adaptation = NULL, *theme = g_build_filename(config.assets_root, "themes", "radios.css", NULL);
    g_assert_true(g_file_get_contents(theme, &adaptation, NULL, NULL));
    g_string_append(css, adaptation); g_free(adaptation); g_free(theme);
    GtkCssProvider *provider = gtk_css_provider_new();
    g_assert_true(gtk_css_provider_load_from_data(provider, css->str, -1, NULL));
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_string_free(css, TRUE);
    gtk_style_context_add_class(gtk_widget_get_style_context(h->window), "radios");
    gtk_window_resize(GTK_WINDOW(h->window), 1800, 1060); run_for(450);
    GdkWindow *window = gtk_widget_get_window(h->window); g_assert_nonnull(window);
    GdkPixbuf *shot = gdk_pixbuf_get_from_window(window, 0, 0, gdk_window_get_width(window), gdk_window_get_height(window));
    g_assert_nonnull(shot); GError *error = NULL;
    g_assert_true(gdk_pixbuf_save(shot, path, "png", &error, NULL)); g_assert_no_error(error);
    g_object_unref(shot);
    gtk_style_context_remove_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider));
    g_object_unref(provider);
}

static void test_selected_media_playlist_cancel_order_reopen(void)
{
    Harness *h = open_harness(); JsonNode *before = document("Mevcut program");
    catalogue_query(h, "Temporary", 2, "Temporary A");
    GtkTreeSelection *selection = catalogue_selection(h);
    gtk_tree_selection_unselect_all(selection);
    GtkWidget *button = find(h->page, "broadcast-catalog-create-playlist"); g_assert_true(GTK_IS_BUTTON(button));
    g_assert_false(gtk_widget_is_sensitive(button));
    select_pair(h, 0, 1); selected_catalogue_screenshot(h); click(h, "broadcast-catalog-create-playlist");
    GtkWidget *create = dialog("broadcast-selection-playlist-dialog"); g_assert_nonnull(create);
    g_assert_cmpstr(gtk_window_get_title(GTK_WINDOW(create)), ==, "Seçilenlerle playlist oluştur");
    gtk_entry_set_text(GTK_ENTRY(find(create, "broadcast-selection-playlist-name")), "Cancelled selected media");
    gtk_dialog_response(GTK_DIALOG(create), GTK_RESPONSE_CANCEL);
    g_assert_null(selection_playlist("Cancelled selected media"));
    JsonNode *unchanged = document("Mevcut program"); g_assert_true(json_node_equal(before, unchanged)); json_node_free(unchanged);
    click(h, "broadcast-catalog-create-playlist"); create = dialog("broadcast-selection-playlist-dialog"); g_assert_nonnull(create);
    gtk_dialog_response(GTK_DIALOG(create), GTK_RESPONSE_ACCEPT);
    g_assert_nonnull(dialog("broadcast-selection-playlist-dialog"));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(create, "broadcast-selection-playlist-status"))), "adını"));
    gtk_entry_set_text(GTK_ENTRY(find(create, "broadcast-selection-playlist-name")), "  Selected native media  ");
    gtk_dialog_response(GTK_DIALOG(create), GTK_RESPONSE_ACCEPT);
    wait_for(selection_playlist_closed, NULL, "selected media playlist persisted and its receipt acknowledged");
    wait_for(library_selected, h, "created selected media playlist opened in the Playlist tab");
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(find(h->page, "broadcast-catalog-content"))), ==, "playlists");
    JsonNode *playlist = selection_playlist("Selected native media"); g_assert_nonnull(playlist);
    JsonObject *record = json_node_get_object(playlist);
    JsonArray *tracks = json_object_get_array_member(record, "tracks"); g_assert_cmpuint(json_array_get_length(tracks), ==, 2);
    gchar *id = g_strdup(radios_json_string(record, "_id", ""));
    const gchar *titles[] = {"Temporary A", "Temporary B"};
    const gchar *bindings[] = {"media_id", "version_id", "media_file_id", "media_sha256"};
    for (guint i = 0; i < 2; ++i) {
        JsonObject *track = json_array_get_object_element(tracks, i);
        g_assert_cmpstr(radios_json_string(track, "title", ""), ==, titles[i]);
        g_assert_cmpint(radios_json_int(track, "ordinal", -1), ==, i);
        g_assert_cmpstr(radios_json_string(track, "media_state", ""), ==, "unverified");
        JsonNode *detail = media_detail(titles[i]); JsonObject *source = json_node_get_object(detail);
        JsonObject *media = json_object_get_object_member(source, "media"), *version = json_object_get_object_member(source, "version");
        const gchar *expected[] = {radios_json_string(media, "_id", ""), radios_json_string(version, "_id", ""),
            radios_json_string(version, "media_file_id", ""), radios_json_string(version, "sha256", "")};
        for (guint field = 0; field < G_N_ELEMENTS(bindings); ++field)
            g_assert_cmpstr(radios_json_string(track, bindings[field], ""), ==, expected[field]);
        g_assert_cmpint(radios_json_int(track, "duration_ms", 0), ==, 200);
        json_node_free(detail);
    }
    unchanged = document("Mevcut program"); g_assert_true(json_node_equal(before, unchanged)); json_node_free(unchanged);
    g_assert_null(h->ctx.playout); close_harness(h); h = open_harness();
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(h->page, "broadcast-kind-playlist")), TRUE);
    LibraryRecord wanted = {h, id}; wait_for(library_record_loaded, &wanted, "selected media playlist remains available after owner reopen");
    JsonNode *reopened = selection_playlist("Selected native media"); g_assert_true(json_node_equal(playlist, reopened));
    unchanged = document("Mevcut program"); g_assert_true(json_node_equal(before, unchanged)); json_node_free(unchanged);
    g_assert_null(h->ctx.playout);
    close_harness(h); json_node_free(before); json_node_free(playlist); json_node_free(reopened); g_free(id);
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]); for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1]; --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv); GError *error = NULL;
    g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error);
    g_test_add_func("/native-broadcast-catalogue/insert-replace-saved-programme-reopen-embedded", test_insert_replace_saved_programme_reopen_embedded);
    g_test_add_func("/native-broadcast-catalogue/stale-selection-real-SHA-CAS-owner-close", test_stale_selection_sha_cas_and_close);
    g_test_add_func("/native-broadcast-catalogue/playlist-tab-programme-toolbar", test_playlist_tab_and_programme_toolbar);
    g_test_add_func("/native-broadcast-catalogue/playlist-drop-after-event-opened-event-track-programme-reopen", test_playlist_drop_after_event_and_into_opened_event);
    g_test_add_func("/native-broadcast-catalogue/playlist-drop-delayed-target-owner-close", test_playlist_drop_delayed_target_and_close);
    g_test_add_func("/native-broadcast-catalogue/player-queues-database-switch-stale-drop-reopen", test_player_queues_database_switch_drop_and_reopen);
    g_test_add_func("/native-broadcast-catalogue/multiple-media-ordered-insert-drag-one-CAS", test_multiple_media_insert_drag_atomic);
    g_test_add_func("/native-broadcast-catalogue/multiple-media-stale-selection-failed-member-no-partial", test_multiple_media_stale_selection_and_failed_member);
    g_test_add_func("/native-broadcast-catalogue/all-six-category-tabs-reset-playlist-scope", test_all_category_tabs_reset_playlist_scope);
    g_test_add_func("/native-broadcast-catalogue/selected-media-playlist-cancel-order-bindings-reopen", test_selected_media_playlist_cancel_order_reopen);
    gint result = g_test_run(); radios_data_client_unref(reader); radios_config_clear(&config); g_free(last_notice); return result;
}
