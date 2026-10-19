#include "radios/catalogue_playlist.h"
#include <glib/gstdio.h>
#include <string.h>

static RadioSConfig config;
static RadioSDataClient *reader;
static gchar *last_notice;
static JsonNode *catalogue_records;
static JsonObject *catalogue[3];
typedef gboolean (*Condition)(gpointer value);
typedef struct {
    RadioSViewContext ctx;
    RadioSConfig session_config;
    GtkWidget *window, *page, *tree, *button;
    GtkListStore *rows;
    gboolean invalid_selection;
    gboolean destroy_on_created;
    JsonNode *created;
    guint created_count;
} Harness;

static void pump(void)
{ while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void run_for(guint milliseconds)
{ gint64 end = g_get_monotonic_time() + milliseconds * 1000; while (g_get_monotonic_time() < end) pump(); }
static void wait_for(Condition condition, gpointer value, const gchar *description)
{
    gint64 end = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!condition(value) && g_get_monotonic_time() < end) pump();
    if (!condition(value)) g_error("Playlist selection timeout: %s (%s)", description, last_notice ? last_notice : "no notice");
}
static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *dialog(void)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *result = NULL;
    for (GList *item = windows; item && !result; item = item->next)
        if (g_str_equal(gtk_widget_get_name(item->data), "broadcast-selection-playlist-dialog")) result = item->data;
    g_list_free(windows); return result;
}
static gboolean no_dialog(gpointer unused) { (void)unused; return dialog() == NULL; }
static void notify(gpointer unused, const gchar *message)
{ (void)unused; g_free(last_notice); last_notice = g_strdup(message); }
static JsonNode *copy(JsonNode *node)
{
    gchar *text = json_to_string(node, FALSE); JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));
    JsonNode *result = json_node_copy(json_parser_get_root(parser));
    g_object_unref(parser); g_free(text); return result;
}
static JsonArray *selected(gpointer data)
{
    Harness *h = data;
    if (h->invalid_selection) return NULL;
    GtkTreeModel *model = NULL;
    GList *paths = gtk_tree_selection_get_selected_rows(gtk_tree_view_get_selection(GTK_TREE_VIEW(h->tree)), &model);
    JsonArray *entries = json_array_new();
    for (GList *row = paths; row; row = row->next) {
        GtkTreeIter iter; g_assert_true(gtk_tree_model_get_iter(model, &iter, row->data));
        gint index = 0; gtk_tree_model_get(model, &iter, 0, &index, -1);
        g_assert_cmpint(index, >=, 0); g_assert_cmpint(index, <, 3);
        JsonObject *entry = radios_catalogue_playlist_track(catalogue[index]); g_assert_nonnull(entry);
        json_array_add_object_element(entries, entry);
    }
    g_list_free_full(paths, (GDestroyNotify)gtk_tree_path_free); return entries;
}
static void created(JsonObject *playlist, gpointer data)
{
    Harness *h = data; ++h->created_count;
    g_clear_pointer(&h->created, json_node_free);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, playlist);
    h->created = copy(node); json_node_free(node);
    if (h->destroy_on_created) gtk_widget_destroy(h->window);
}
static gboolean connected(gpointer value) { return radios_data_connected(&((Harness *)value)->ctx); }
static Harness *open_harness(void)
{
    Harness *h = g_new0(Harness, 1); h->session_config = config; h->ctx.config = &h->session_config;
    h->ctx.runtime = radios_runtime_new(64, 128); h->ctx.notify = notify;
    h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(h->window); h->ctx.parent = GTK_WINDOW(h->window);
    h->page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_container_add(GTK_CONTAINER(h->window), h->page);
    h->rows = gtk_list_store_new(1, G_TYPE_INT);
    for (gint index = 0; index < 3; ++index) { GtkTreeIter iter; gtk_list_store_append(h->rows, &iter); gtk_list_store_set(h->rows, &iter, 0, index, -1); }
    h->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(h->rows));
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    gtk_tree_view_append_column(GTK_TREE_VIEW(h->tree), gtk_tree_view_column_new_with_attributes("Fixture row", renderer, "text", 0, NULL));
    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(h->tree));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
    gtk_box_pack_start(GTK_BOX(h->page), h->tree, TRUE, TRUE, 0);
    h->ctx.data = radios_data_new(&h->ctx);
    h->button = radios_catalogue_playlist_button_new(&h->ctx, h->page, selection, selected, created, h);
    gtk_box_pack_start(GTK_BOX(h->page), h->button, FALSE, FALSE, 0);
    gtk_widget_show_all(h->window); radios_data_connect(&h->ctx); wait_for(connected, h, "real private station context");
    return h;
}
static void close_harness(Harness *h)
{
    gtk_widget_destroy(h->window); radios_data_close(h->ctx.data);
    radios_runtime_free(h->ctx.runtime); radios_data_unref(h->ctx.data); run_for(40);
    g_object_unref(h->window);
    g_clear_pointer(&h->created, json_node_free); g_object_unref(h->rows); g_free(h);
}
static void select_row(Harness *h, guint index)
{
    GtkTreePath *path = gtk_tree_path_new_from_indices((gint)index, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(h->tree)), path); gtk_tree_path_free(path);
}
static void select_two(Harness *h)
{ select_row(h, 2); select_row(h, 0); g_assert_true(gtk_widget_is_sensitive(h->button)); }
static void click(Harness *h)
{ g_assert_true(gtk_widget_is_sensitive(h->button)); gtk_button_clicked(GTK_BUTTON(h->button)); g_assert_nonnull(dialog()); }
static void set_name(const gchar *name)
{ gtk_entry_set_text(GTK_ENTRY(find(dialog(), "broadcast-selection-playlist-name")), name); }
static gboolean completed(gpointer data) { return ((Harness *)data)->created_count > 0; }
static gboolean retry_ready(gpointer unused)
{
    (void)unused; GtkWidget *d = dialog();
    return d && gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT)) &&
        !gtk_widget_is_sensitive(find(d, "broadcast-selection-playlist-name"));
}
static void flag(const gchar *name, gboolean active)
{
    gchar *root = g_path_get_dirname(config.database_file), *path = g_build_filename(root, name, NULL); g_free(root);
    if (active) g_assert_true(g_file_set_contents(path, "hold", -1, NULL));
    else if (g_file_test(path, G_FILE_TEST_EXISTS)) g_assert_cmpint(g_unlink(path), ==, 0);
    g_free(path);
}
static gboolean held(gpointer unused)
{
    (void)unused; gchar *root = g_path_get_dirname(config.database_file), *path = g_build_filename(root, ".selection-ready", NULL);
    gboolean exists = g_file_test(path, G_FILE_TEST_EXISTS); g_free(path); g_free(root); return exists;
}
typedef struct { const gchar *path; JsonNode *result; GError *error; gint done; } Read;
static gpointer read_worker(gpointer value)
{
    Read *read = value; JsonNode *body = json_node_new(JSON_NODE_OBJECT); json_node_take_object(body, json_object_new());
    json_object_set_string_member(json_node_get_object(body), "station_id", radios_data_client_station_id(reader));
    read->result = radios_data_client_request_sync(reader, "POST", read->path, body, NULL, &read->error);
    json_node_free(body); g_atomic_int_set(&read->done, 1); return NULL;
}
static gboolean read_done(gpointer value) { return g_atomic_int_get(&((Read *)value)->done); }
static JsonNode *read_path(const gchar *path)
{
    Read read = {.path = path}; GThread *thread = g_thread_new("selection-proof-read", read_worker, &read);
    wait_for(read_done, &read, "real HTTP playlist proof"); g_thread_join(thread);
    g_assert_no_error(read.error); g_assert_nonnull(read.result); return read.result;
}
static JsonNode *playlists(void) { return read_path("/v1/workflows/desktop-playlists/list"); }
static guint playlist_count(void)
{ JsonNode *all = playlists(); guint n = json_array_get_length(json_object_get_array_member(json_node_get_object(all), "playlists")); json_node_free(all); return n; }
static void assert_tracks(JsonNode *playlist)
{
    JsonArray *tracks = json_object_get_array_member(json_node_get_object(playlist), "tracks"); g_assert_cmpuint(json_array_get_length(tracks), ==, 2);
    for (guint index = 0; index < 2; ++index) {
        JsonObject *row = json_array_get_object_element(tracks, index);
        g_assert_cmpstr(radios_json_string(row, "title", ""), ==, index == 0 ? "Selected A" : "Selected C");
        g_assert_cmpstr(radios_json_string(row, "artist", ""), ==, "Selected artist");
        g_assert_cmpstr(radios_json_string(row, "album", ""), ==, "Selected album");
        g_assert_cmpint(radios_json_int(row, "duration_ms", 0), ==, 200);
        g_assert_cmpint(radios_json_int(row, "ordinal", -1), ==, index);
        g_assert_cmpstr(radios_json_string(row, "media_state", ""), ==, "unverified");
        JsonObject *source = json_object_get_object_member(catalogue[index == 0 ? 0 : 2], "media");
        g_assert_cmpstr(radios_json_string(row, "media_id", ""), ==, radios_json_string(source, "_id", ""));
    }
}
static void test_empty_blank_invalid_cancel(void)
{
    Harness *h = open_harness(); guint before = playlist_count();
    g_assert_false(gtk_widget_is_sensitive(h->button)); select_two(h);
    h->invalid_selection = TRUE; radios_catalogue_playlist_button_refresh(h->button); g_assert_false(gtk_widget_is_sensitive(h->button));
    h->invalid_selection = FALSE; radios_catalogue_playlist_button_refresh(h->button); click(h);
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(h->button)), ==, "Seçilenlerle playlist oluştur");
    set_name("   "); gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(dialog(), "broadcast-selection-playlist-status"))), "adını"));
    g_assert_cmpuint(playlist_count(), ==, before); g_assert_cmpuint(h->created_count, ==, 0);
    gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_CANCEL); wait_for(no_dialog, NULL, "cancel closes unsent create");
    g_assert_cmpuint(playlist_count(), ==, before); close_harness(h);
}
static void test_selection_snapshot_order_reopen(void)
{
    Harness *h = open_harness(); guint before = playlist_count(); select_two(h); click(h);
    gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(h->tree))); select_row(h, 1);
    set_name("  Snapshot selected playlist  "); gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT);
    wait_for(completed, h, "selected playlist receipt"); wait_for(no_dialog, NULL, "success closes create");
    g_assert_cmpuint(h->created_count, ==, 1); assert_tracks(h->created);
    g_assert_cmpstr(radios_json_string(json_node_get_object(h->created), "name", ""), ==, "Snapshot selected playlist");
    JsonNode *expected = copy(h->created); close_harness(h); h = open_harness();
    JsonNode *all = playlists(); JsonArray *rows = json_object_get_array_member(json_node_get_object(all), "playlists"); gboolean found = FALSE;
    for (guint index = 0; index < json_array_get_length(rows); ++index)
        if (json_node_equal(json_array_get_element(rows, index), expected)) found = TRUE;
    g_assert_true(found); g_assert_cmpuint(json_array_get_length(rows), ==, before + 1);
    json_node_free(all); json_node_free(expected); close_harness(h);
}
static void test_lost_reply_retry_pins_payload(void)
{
    Harness *h = open_harness(); guint before = playlist_count(); select_two(h); click(h); set_name("Lost reply selection");
    flag(".selection-drop", TRUE); gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT);
    wait_for(retry_ready, NULL, "committed response lost; exact request available for retry");
    g_assert_cmpuint(h->created_count, ==, 0); g_assert_cmpuint(playlist_count(), ==, before + 1);
    gtk_tree_selection_unselect_all(gtk_tree_view_get_selection(GTK_TREE_VIEW(h->tree))); select_row(h, 1);
    set_name("Changed after lost reply"); gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT);
    wait_for(completed, h, "idempotent lost reply receipt"); assert_tracks(h->created);
    g_assert_cmpstr(radios_json_string(json_node_get_object(h->created), "name", ""), ==, "Lost reply selection");
    g_assert_cmpuint(playlist_count(), ==, before + 1); g_assert_cmpuint(h->created_count, ==, 1); close_harness(h);
}
static void test_forged_same_count_receipt_rejected(void)
{
    Harness *h = open_harness(); guint before = playlist_count(); select_two(h); click(h); set_name("Forged receipt selection");
    flag(".selection-forge", TRUE); gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT);
    wait_for(retry_ready, NULL, "same-sized wrong metadata receipt rejected");
    g_assert_cmpuint(h->created_count, ==, 0);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(dialog(), "broadcast-selection-playlist-status"))), "eşleşmedi"));
    gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT); wait_for(completed, h, "genuine receipt after forged response");
    assert_tracks(h->created); g_assert_cmpuint(playlist_count(), ==, before + 1); close_harness(h);
}
static gboolean viewer_connected(gpointer value)
{
    Harness *h = value; RadioSDataClient *current = radios_data_client_acquire(&h->ctx);
    gboolean match = current && g_strcmp0(radios_data_client_user_id(current), radios_data_client_user_id(reader));
    if (current) radios_data_client_unref(current);
    return match;
}
static void test_identity_switch_before_send(void)
{
    Harness *h = open_harness(); guint before = playlist_count(); select_two(h); click(h); set_name("Wrong identity rejected");
    h->session_config.token_environment = "RADIOS_TEST_VIEW_TOKEN"; radios_data_connect(&h->ctx);
    wait_for(viewer_connected, h, "same live data service switched to genuine viewer");
    gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(dialog(), "broadcast-selection-playlist-status"))), "değişti"));
    g_assert_cmpuint(playlist_count(), ==, before); g_assert_cmpuint(h->created_count, ==, 0); close_harness(h);
}
static void test_button_and_owner_destroy_lifetime(void)
{
    Harness *h = open_harness(); guint before = playlist_count(); select_two(h); click(h);
    g_object_ref(h->button); gtk_widget_destroy(h->button); g_object_unref(h->button); h->button = NULL;
    wait_for(no_dialog, NULL, "destroyed button closes dependent create before freeing state");
    g_assert_cmpuint(playlist_count(), ==, before); close_harness(h);
    h = open_harness(); select_two(h); click(h); set_name("Closed pending selection");
    flag(".selection-ready", FALSE); flag(".selection-hold", TRUE);
    gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT); wait_for(held, NULL, "actual committed HTTP response held");
    gtk_widget_destroy(h->window); wait_for(no_dialog, NULL, "destroyed owner closes in-flight dialog");
    flag(".selection-hold", FALSE); run_for(200); g_assert_cmpuint(h->created_count, ==, 0);
    g_assert_cmpuint(playlist_count(), ==, before + 1); close_harness(h); flag(".selection-ready", FALSE);
    h = open_harness(); select_two(h); g_object_ref(h->button);
    gtk_widget_destroy(h->window); radios_catalogue_playlist_button_refresh(h->button);
    gtk_button_clicked(GTK_BUTTON(h->button)); g_assert_null(dialog());
    g_object_unref(h->button); h->button = NULL; close_harness(h);
}
static void test_local_unknown_duration_and_unsupported_mapping(void)
{
    JsonObject *wrapper = json_object_new(), *media = json_object_new(), *version = json_object_new();
    json_object_set_object_member(wrapper, "media", media); json_object_set_object_member(wrapper, "version", version);
    json_object_set_string_member(media, "title", "  Local track  ");
    json_object_set_string_member(media, "_id", "11111111-1111-4111-8111-111111111111");
    json_object_set_string_member(version, "_id", "22222222-2222-4222-8222-222222222222");
    json_object_set_string_member(version, "media_file_id", "33333333-3333-4333-8333-333333333333");
    json_object_set_string_member(version, "sha256", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    JsonObject *track = radios_catalogue_playlist_track(wrapper); g_assert_nonnull(track);
    g_assert_cmpstr(radios_json_string(track, "title", ""), ==, "Local track");
    g_assert_cmpstr(radios_json_string(track, "media_id", ""), ==, "11111111-1111-4111-8111-111111111111");
    g_assert_cmpstr(radios_json_string(track, "version_id", ""), ==, "22222222-2222-4222-8222-222222222222");
    g_assert_cmpstr(radios_json_string(track, "media_file_id", ""), ==, "33333333-3333-4333-8333-333333333333");
    g_assert_cmpstr(radios_json_string(track, "media_sha256", ""), ==, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(track, "duration_ms")));
    g_assert_false(json_object_has_member(track, "provider")); json_object_unref(track);
    json_object_set_string_member(media, "title", "   ");
    g_assert_null(radios_catalogue_playlist_track(wrapper));
    json_object_set_string_member(media, "title", "Local track"); json_object_remove_member(wrapper, "version");
    g_assert_null(radios_catalogue_playlist_track(wrapper)); json_object_unref(wrapper);
    wrapper = json_object_new(); json_object_set_object_member(wrapper, "programme", json_object_new());
    g_assert_null(radios_catalogue_playlist_track(wrapper)); json_object_unref(wrapper);
    /* Retired remote catalogue rows are no longer track sources. */
    wrapper = json_object_new(); json_object_set_object_member(wrapper, "online", json_object_new());
    json_object_set_string_member(json_object_get_object_member(wrapper, "online"), "title", "Remote row");
    g_assert_null(radios_catalogue_playlist_track(wrapper)); json_object_unref(wrapper);
}
static void test_created_callback_destroys_owner(void)
{
    Harness *h = open_harness(); guint before = playlist_count(); select_two(h); click(h);
    set_name("Owner destroyed from callback"); h->destroy_on_created = TRUE;
    gtk_dialog_response(GTK_DIALOG(dialog()), GTK_RESPONSE_ACCEPT);
    wait_for(completed, h, "created callback can destroy its owner safely");
    g_assert_null(dialog()); assert_tracks(h->created); g_assert_cmpuint(playlist_count(), ==, before + 1);
    close_harness(h);
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]);
    for (gint index = 1; index < argc; ++index) argv[index] = argv[index + 1];
    --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv); GError *error = NULL;
    g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error);
    g_assert_true(radios_data_client_handshake_sync(reader, NULL, &error)); g_assert_no_error(error);
    catalogue_records = read_path("/v1/workflows/desktop-media/list");
    JsonArray *media = json_object_get_array_member(json_node_get_object(catalogue_records), "records");
    for (guint row = 0; row < json_array_get_length(media); ++row) {
        JsonObject *record = json_array_get_object_element(media, row), *source = json_object_get_object_member(record, "media");
        const gchar *title = radios_json_string(source, "title", "");
        for (guint index = 0; index < 3; ++index) {
            gchar *expected = g_strdup_printf("Selected %c", 'A' + index);
            if (g_str_equal(title, expected)) catalogue[index] = record;
            g_free(expected);
        }
    }
    for (guint index = 0; index < 3; ++index) g_assert_nonnull(catalogue[index]);
    g_test_add_func("/native-catalogue-playlist/local-unknown-duration-unsupported-mapping", test_local_unknown_duration_and_unsupported_mapping);
    g_test_add_func("/native-catalogue-playlist/empty-blank-invalid-cancel", test_empty_blank_invalid_cancel);
    g_test_add_func("/native-catalogue-playlist/selection-snapshot-order-reopen", test_selection_snapshot_order_reopen);
    g_test_add_func("/native-catalogue-playlist/lost-reply-exact-retry", test_lost_reply_retry_pins_payload);
    g_test_add_func("/native-catalogue-playlist/forged-same-count-receipt", test_forged_same_count_receipt_rejected);
    g_test_add_func("/native-catalogue-playlist/identity-switch-before-send", test_identity_switch_before_send);
    g_test_add_func("/native-catalogue-playlist/button-owner-destroy-lifetime", test_button_and_owner_destroy_lifetime);
    g_test_add_func("/native-catalogue-playlist/created-callback-destroys-owner", test_created_callback_destroys_owner);
    gint result = g_test_run(); json_node_free(catalogue_records); radios_data_client_unref(reader); radios_config_clear(&config); g_free(last_notice); return result;
}
