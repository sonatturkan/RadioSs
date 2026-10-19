#include "radios/data.h"
#include <glib/gstdio.h>
#include <string.h>

static RadioSConfig config;
static RadioSDataClient *reader;
static gchar *last_notice;
typedef struct { RadioSViewContext ctx; GtkWidget *window, *page; } Harness;

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = find(item->data, name);
    g_list_free(children); return found;
}
static GtkWidget *dialog(void)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *found = NULL;
    for (GList *item = windows; item && !found; item = item->next)
        if (g_str_equal(gtk_widget_get_name(item->data), "pw-import-dialog")) found = item->data;
    g_list_free(windows); return found;
}
static GtkWidget *find_data(GtkWidget *root, const gchar *key)
{
    if (g_object_get_data(G_OBJECT(root), key)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *child = children; child && !found; child = child->next) found = find_data(child->data, key);
    g_list_free(children); return found;
}
static gboolean github_card_ready(gpointer page)
{
    GtkWidget *card = find_data(page, "pw-draft-index");
    return card && gtk_widget_is_sensitive(card);
}
static void pump(void)
{ while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
extern const gchar *const radios_reference_css_parts[];
static void save_shot(GtkWidget *window, const gchar *name)
{
    const gchar *directory = g_getenv("RADIOS_SHOT_DIR"); if (!directory) return;
    static gboolean styled;
    if (!styled) {
        GString *css = g_string_new(NULL); gchar *adaptation = NULL;
        for (guint part = 0; radios_reference_css_parts[part]; ++part) g_string_append(css, radios_reference_css_parts[part]);
        gchar *path = g_build_filename(config.assets_root, "themes", "radios.css", NULL);
        g_assert_true(g_file_get_contents(path, &adaptation, NULL, NULL)); g_string_append(css, adaptation);
        g_free(path); g_free(adaptation);
        GtkCssProvider *provider = gtk_css_provider_new();
        g_assert_true(gtk_css_provider_load_from_data(provider, css->str, -1, NULL)); g_string_free(css, TRUE);
        gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        g_object_unref(provider); styled = TRUE;
    }
    gtk_style_context_add_class(gtk_widget_get_style_context(window), "radios");
    gtk_window_resize(GTK_WINDOW(window), GTK_IS_DIALOG(window) ? 900 : 1500, GTK_IS_DIALOG(window) ? 760 : 850);
    for (guint step = 0; step < 400; ++step) pump();
    gtk_container_check_resize(GTK_CONTAINER(window));
    GtkAllocation allocation; gtk_widget_get_allocation(window, &allocation); gtk_widget_size_allocate(window, &allocation);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
    cairo_t *context = cairo_create(surface); gtk_widget_draw(window, context); cairo_destroy(context);
    gchar *path = g_build_filename(directory, name, NULL);
    g_assert_cmpint(cairo_surface_write_to_png(surface, path), ==, CAIRO_STATUS_SUCCESS);
    g_free(path); cairo_surface_destroy(surface);
}
typedef gboolean (*Condition)(gpointer);
static void wait_for(Condition condition, gpointer data, const gchar *description)
{
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!condition(data) && g_get_monotonic_time() < deadline) pump();
    if (!condition(data)) g_error("Rekordbox native acceptance timed out: %s (%s)", description, last_notice ? last_notice : "no notice");
}
static void notice(gpointer unused, const gchar *message)
{ (void)unused; g_free(last_notice); last_notice = g_strdup(message); }
static gboolean ready(gpointer value)
{
    Harness *h = value;
    return radios_data_connected(&h->ctx) && g_object_get_data(G_OBJECT(h->page), "pw-list-loaded");
}
static Harness *open_harness(void)
{
    Harness *h = g_new0(Harness, 1); h->ctx.config = &config; h->ctx.notify = notice;
    g_clear_pointer(&last_notice, g_free); h->ctx.runtime = radios_runtime_new(32, 128);
    h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); h->ctx.parent = GTK_WINDOW(h->window);
    h->ctx.data = radios_data_new(&h->ctx); h->page = radios_playlist_new(&h->ctx);
    gtk_container_add(GTK_CONTAINER(h->window), h->page); gtk_widget_show_all(h->window);
    radios_data_connect(&h->ctx); wait_for(ready, h, "native playlist list loaded"); return h;
}
static void close_harness(Harness *h)
{
    if (h->window) gtk_widget_destroy(h->window);
    radios_data_close(h->ctx.data); radios_runtime_free(h->ctx.runtime); radios_data_unref(h->ctx.data);
    for (guint i = 0; i < 30; ++i) pump();
    g_free(h);
}
static JsonNode *body(void)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, json_object_new());
    json_object_set_string_member(json_node_get_object(node), "station_id", radios_data_client_station_id(reader)); return node;
}
static JsonNode *request(const gchar *path, JsonNode *payload)
{
    GError *error = NULL;
    JsonNode *result = radios_data_client_request_sync(reader, "POST", path, payload, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(result); return result;
}
static JsonNode *listed(void)
{
    JsonNode *payload = body(); JsonNode *result = request("/v1/workflows/desktop-playlists/list", payload);
    json_node_free(payload); return result;
}
static JsonObject *record(JsonNode *node, const gchar *name)
{
    JsonArray *rows = json_object_get_array_member(json_node_get_object(node), "playlists");
    for (guint i = 0; i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        if (g_str_equal(radios_json_string(row, "name", ""), name)) return row;
    }
    return NULL;
}
static guint list_count(void)
{
    JsonNode *node = listed(); guint count = json_array_get_length(json_object_get_array_member(json_node_get_object(node), "playlists"));
    json_node_free(node); return count;
}
static gboolean discovery_ready(gpointer unused)
{
    (void)unused; GtkWidget *d = dialog(); if (!d) return FALSE;
    GtkWidget *choices = find(d, "pw-import-choices");
    GList *children = gtk_container_get_children(GTK_CONTAINER(choices));
    gboolean ready_now = g_list_length(children) == 2 && gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT));
    g_list_free(children); return ready_now;
}
static gboolean preview_ready(gpointer unused)
{
    (void)unused; GtkWidget *d = dialog(); if (!d) return FALSE;
    GtkWidget *accept = gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    return gtk_widget_is_sensitive(accept) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(accept)), "Seçilenleri içeri aktar") == 0;
}
static gboolean closed(gpointer unused)
{ (void)unused; return dialog() == NULL; }
static gboolean retry_ready(gpointer unused)
{
    (void)unused; GtkWidget *d = dialog(); if (!d) return FALSE;
    GtkWidget *accept = gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    return gtk_widget_is_sensitive(accept) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(accept)), "Aynı işlemi yeniden dene") == 0;
}
static gboolean stale_ready(gpointer unused)
{
    (void)unused; GtkWidget *d = dialog(); if (!d) return FALSE;
    GtkWidget *accept = gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT);
    return gtk_widget_is_sensitive(accept) && g_strcmp0(gtk_button_get_label(GTK_BUTTON(accept)), "Önizlemeyi doğrula") == 0 &&
        strstr(gtk_label_get_text(GTK_LABEL(find(d, "pw-import-status"))), "değiştirildi");
}
static GtkWidget *open_import(Harness *h)
{
    GtkWidget *button = find(h->page, "pw-import"); g_assert_true(GTK_IS_BUTTON(button));
    g_assert_true(gtk_widget_is_sensitive(button)); gtk_button_clicked(GTK_BUTTON(button));
    GtkWidget *d = dialog(); g_assert_nonnull(d);
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(d, "pw-import-format")), "xml"));
    gchar *filename = g_build_filename(config.project_root, "test-rekordbox.xml", NULL);
    GtkWidget *chooser = find(d, "pw-import-file");
    g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), filename)); g_free(filename);
    g_signal_emit_by_name(chooser, "file-set"); wait_for(discovery_ready, NULL, "server XML discovery");
    GList *choices = gtk_container_get_children(GTK_CONTAINER(find(d, "pw-import-choices")));
    g_assert_cmpuint(g_list_length(choices), ==, 2);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(choices->next->data), FALSE); g_list_free(choices);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT); wait_for(preview_ready, NULL, "selected-scope CAS preview"); return d;
}
static gchar *gate(const gchar *name)
{
    gchar *directory = g_path_get_dirname(config.database_file);
    gchar *path = g_build_filename(directory, name, NULL); g_free(directory); return path;
}
static gboolean gate_ready(gpointer path)
{ return g_file_test(path, G_FILE_TEST_IS_REGULAR); }

static void persistence_policies_and_replay(void)
{
    Harness *h = open_harness(); GtkWidget *d = open_import(h);
    g_assert_cmpuint(list_count(), ==, 0); /* both discovery and explicit preview are read-only */
    gchar *source_file = g_build_filename(config.project_root, "test-rekordbox.xml", NULL);
    gchar *hidden_file = g_build_filename(config.project_root, "previewed-snapshot.xml", NULL);
    g_assert_cmpint(g_rename(source_file, hidden_file), ==, 0);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT); wait_for(closed, NULL, "real atomic import apply");
    g_assert_cmpint(g_rename(hidden_file, source_file), ==, 0); g_free(source_file); g_free(hidden_file);
    JsonNode *node = listed(); JsonObject *row = record(node, "Selected"); g_assert_nonnull(row);
    JsonArray *tracks = json_object_get_array_member(row, "tracks"); g_assert_cmpuint(json_array_get_length(tracks), ==, 2);
    JsonObject *first = json_array_get_object_element(tracks, 0);
    g_assert_cmpint(radios_json_int(first, "duration_ms", 0), ==, 123456);
    g_assert_false(json_object_has_member(first, "media_id")); g_assert_null(record(node, "Excluded")); json_node_free(node);
    close_harness(h); h = open_harness(); d = open_import(h);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(d, "pw-import-status"))), "0 yeni öğe"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(find(d, "pw-import-name-policy")), 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(find(d, "pw-import-duplicates")), 1);
    gtk_combo_box_set_active(GTK_COMBO_BOX(find(d, "pw-import-cues")), 1);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT); wait_for(preview_ready, NULL, "copy/keep/review policy preview");
    gchar *drop = gate(".acceptance-rekordbox-drop"); g_assert_true(g_file_set_contents(drop, "drop", -1, NULL));
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT); wait_for(retry_ready, NULL, "lost response preserves exact idempotent operation");
    g_assert_false(gtk_widget_is_sensitive(find(d, "pw-import-name-policy")));
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT); wait_for(closed, NULL, "same-body replay acknowledges exactly one import");
    node = listed(); row = record(node, "Selected (2)"); g_assert_nonnull(row);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(row, "tracks")), ==, 3);
    g_assert_cmpuint(list_count(), ==, 2); json_node_free(node); g_free(drop); close_harness(h);
}
static void conflict_and_lifetimes(void)
{
    Harness *h = open_harness(); GtkWidget *d = open_import(h); guint before = list_count();
    JsonNode *payload = body(); JsonObject *o = json_node_get_object(payload);
    json_object_set_string_member(o, "name", "Selected"); gchar *key = g_uuid_string_random();
    json_object_set_string_member(o, "idempotency_key", key); g_free(key);
    JsonNode *result = request("/v1/workflows/desktop-playlists/create", payload); json_node_free(result); json_node_free(payload);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT); wait_for(stale_ready, NULL, "stale preview rejected with editable selection retained");
    g_assert_cmpuint(list_count(), ==, before + 1);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL); close_harness(h);
    h = open_harness(); GtkWidget *button = find(h->page, "pw-import"); gtk_button_clicked(GTK_BUTTON(button)); d = dialog();
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(d, "pw-import-format")), "xml"));
    gtk_combo_box_set_active(GTK_COMBO_BOX(find(d, "pw-import-name-policy")), 1);
    gchar *hold = gate(".acceptance-rekordbox-preview-hold"), *ready_path = gate(".acceptance-rekordbox-preview-ready");
    g_unlink(ready_path); g_assert_true(g_file_set_contents(hold, "hold", -1, NULL));
    gchar *filename = g_build_filename(config.project_root, "test-rekordbox.xml", NULL);
    GtkWidget *chooser = find(d, "pw-import-file"); g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), filename));
    g_free(filename); g_signal_emit_by_name(chooser, "file-set"); wait_for(gate_ready, ready_path, "preview held while dialog is destroyed");
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CANCEL); g_unlink(hold); close_harness(h);
    for (guint i = 0; i < 50; ++i) pump();
    g_unlink(ready_path); g_free(hold); g_free(ready_path);
    h = open_harness(); gtk_button_clicked(GTK_BUTTON(find(h->page, "pw-import"))); d = dialog();
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(d, "pw-import-format")), "xml"));
    filename = g_build_filename(config.project_root, "test-rekordbox.xml", NULL); chooser = find(d, "pw-import-file");
    g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(chooser), filename)); g_free(filename);
    g_signal_emit_by_name(chooser, "file-set"); close_harness(h); /* destroy owner during bounded file worker */
}

static gboolean import_choices_ready(gpointer value)
{
    guint expected = GPOINTER_TO_UINT(value); GtkWidget *window = dialog(); if (!window) return FALSE;
    GList *children = gtk_container_get_children(GTK_CONTAINER(find(window, "pw-import-choices")));
    gboolean ready = g_list_length(children) == expected &&
        gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(window), GTK_RESPONSE_ACCEPT));
    g_list_free(children); return ready;
}
static gboolean usb_error_ready(gpointer unused)
{
    (void)unused; GtkWidget *window = dialog(); if (!window) return FALSE;
    const gchar *status = gtk_label_get_text(GTK_LABEL(find(window, "pw-import-status")));
    return strstr(status, "export.pdb") != NULL &&
        !gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(window), GTK_RESPONSE_ACCEPT));
}
static GtkWidget *open_usb(Harness *h, const gchar *directory, guint playlists)
{
    gtk_button_clicked(GTK_BUTTON(find(h->page, "pw-import"))); GtkWidget *window = dialog(); g_assert_nonnull(window);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(window, "pw-import-format"))), ==, "usb");
    GtkWidget *folder = find(window, "pw-import-folder");
    gchar *path = g_build_filename(config.project_root, directory, NULL);
    g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(folder), path)); g_free(path);
    g_signal_emit_by_name(folder, "file-set");
    wait_for(import_choices_ready, GUINT_TO_POINTER(playlists), "real binary USB playlist discovery");
    gtk_combo_box_set_active(GTK_COMBO_BOX(find(window, "pw-import-duplicates")), 1);
    return window;
}
static void usb_tracklist_snapshot_and_unplug(void)
{
    Harness *h = open_harness(); guint before = list_count(); GtkWidget *window = open_usb(h, "USB örnek", 2);
    g_assert_cmpuint(list_count(), ==, before);
    GList *choices = gtk_container_get_children(GTK_CONTAINER(find(window, "pw-import-choices")));
    g_assert_nonnull(strstr(gtk_button_get_label(GTK_BUTTON(choices->data)), "USB Setleri / USB Sıra"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(choices->next->data), FALSE); g_list_free(choices);
    gtk_dialog_response(GTK_DIALOG(window), GTK_RESPONSE_ACCEPT); wait_for(preview_ready, NULL, "selected USB tracklist review");
    gchar *source = g_build_filename(config.project_root, "USB örnek", "PIONEER", "rekordbox", "export.pdb", NULL);
    gchar *hidden = g_strconcat(source, ".unplugged", NULL);
    gchar *original = NULL; gsize size; g_assert_true(g_file_get_contents(source, &original, &size, NULL));
    g_assert_cmpint(g_rename(source, hidden), ==, 0);
    gchar *drop = gate(".acceptance-rekordbox-drop"); g_assert_true(g_file_set_contents(drop, "drop", -1, NULL));
    gtk_dialog_response(GTK_DIALOG(window), GTK_RESPONSE_ACCEPT); wait_for(retry_ready, NULL, "USB lost-response retry retains the immutable PDB snapshot");
    gtk_dialog_response(GTK_DIALOG(window), GTK_RESPONSE_ACCEPT); wait_for(closed, NULL, "USB replay succeeds after removal of its source database");
    g_assert_cmpint(g_rename(hidden, source), ==, 0);
    gchar *after = NULL; gsize after_size; g_assert_true(g_file_get_contents(source, &after, &after_size, NULL));
    g_assert_cmpmem(original, size, after, after_size); g_free(original); g_free(after);
    JsonNode *node = listed(); JsonObject *playlist = record(node, "USB Sıra"); g_assert_nonnull(playlist);
    JsonArray *tracks = json_object_get_array_member(playlist, "tracks"); g_assert_cmpuint(json_array_get_length(tracks), ==, 3);
    const gchar *titles[] = {"İkinci", "Birinci", "Birinci"};
    for (guint position = 0; position < G_N_ELEMENTS(titles); ++position) {
        JsonObject *track = json_array_get_object_element(tracks, position);
        g_assert_cmpstr(radios_json_string(track, "title", ""), ==, titles[position]);
        g_assert_cmpstr(radios_json_string(track, "media_state", ""), ==, "unverified");
        gchar *root = g_build_filename(config.project_root, "USB örnek", "Contents", NULL);
        g_assert_true(g_str_has_prefix(radios_json_string(track, "location", ""), root)); g_free(root);
    }
    json_node_free(node); close_harness(h); h = open_harness(); node = listed(); g_assert_nonnull(record(node, "USB Sıra"));
    json_node_free(node);
    gtk_button_clicked(GTK_BUTTON(find(h->page, "pw-import"))); window = dialog();
    GtkWidget *folder = find(window, "pw-import-folder");
    g_assert_true(gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(folder), config.project_root));
    g_signal_emit_by_name(folder, "file-set"); wait_for(usb_error_ready, NULL, "a directory without a device database is rejected");
    gtk_dialog_response(GTK_DIALOG(window), GTK_RESPONSE_CANCEL);
    g_free(drop); g_free(source); g_free(hidden); close_harness(h);
}
static void github_usb_tracklist(void)
{
    Harness *h = open_harness(); GtkWidget *window = open_usb(h, "GitHub USB", 1);
    gtk_dialog_response(GTK_DIALOG(window), GTK_RESPONSE_ACCEPT); wait_for(preview_ready, NULL, "GitHub CDJ export review");
    save_shot(window, "rekordbox-github-usb-preview.png");
    gtk_dialog_response(GTK_DIALOG(window), GTK_RESPONSE_ACCEPT); wait_for(closed, NULL, "GitHub CDJ export native import");
    JsonNode *node = listed(); JsonObject *playlist = record(node, "test_formats"); g_assert_nonnull(playlist);
    JsonArray *tracks = json_object_get_array_member(playlist, "tracks"); g_assert_cmpuint(json_array_get_length(tracks), ==, 40);
    for (guint position = 0; position < json_array_get_length(tracks); ++position) {
        JsonObject *track = json_array_get_object_element(tracks, position);
        g_assert_true(*radios_json_string(track, "title", ""));
        gchar *expected = g_strdup_printf("%u", 612 + position);
        g_assert_cmpstr(radios_json_string(track, "rekordbox_track_id", ""), ==, expected); g_free(expected);
        g_assert_cmpstr(radios_json_string(track, "media_state", ""), ==, "unverified");
    }
    gtk_entry_set_text(GTK_ENTRY(find(h->page, "pw-list-search")), "test_formats");
    wait_for(github_card_ready, h->page, "GitHub playlist sidebar loaded");
    gtk_button_clicked(GTK_BUTTON(find_data(h->page, "pw-draft-index")));
    GtkWidget *table = find_data(h->page, "tree-view"); g_assert_nonnull(table);
    GtkTreeView *tree = g_object_get_data(G_OBJECT(table), "tree-view");
    GtkTreeModel *model = gtk_tree_view_get_model(tree); g_assert_cmpint(gtk_tree_model_iter_n_children(model, NULL), ==, 40);
    GtkTreeIter row; g_assert_true(gtk_tree_model_get_iter_first(model, &row));
    for (guint position = 0; position < 40; ++position) {
        gint ordinal = 0; gtk_tree_model_get(model, &row, 1, &ordinal, -1);
        g_assert_cmpint(ordinal, ==, (gint)position + 1);
        if (position < 39) g_assert_true(gtk_tree_model_iter_next(model, &row));
    }
    g_test_message("GitHub original DeviceSQL export: test_formats, 40 ordered tracks, native preview/apply passed without XML.");
    save_shot(h->window, "rekordbox-github-native-playlist.png");
    json_node_free(node); close_harness(h);
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]);
    for (gint i = 1; i < argc - 1; ++i) argv[i] = argv[i + 1];
    --argc; g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv); GError *error = NULL;
    g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/native-rekordbox/persistence-policies-lost-response", persistence_policies_and_replay);
    g_test_add_func("/native-rekordbox/stale-preview-dialog-worker-lifetimes", conflict_and_lifetimes);
    g_test_add_func("/native-rekordbox/USB-binary-order-unplug-readonly-replay", usb_tracklist_snapshot_and_unplug);
    if (g_getenv("RADIOS_TEST_USB_EXPORT")) g_test_add_func("/native-rekordbox/GitHub-real-CDJ-export", github_usb_tracklist);
    gint result = g_test_run(); radios_data_client_unref(reader); radios_config_clear(&config); g_free(last_notice); return result;
}
