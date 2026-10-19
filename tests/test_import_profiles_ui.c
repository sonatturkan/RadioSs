#include "radios/data.h"
#include "radios/runtime.h"
#include "radios/transfer_ui.h"
#include <glib/gstdio.h>
#include <string.h>

/* Kılavuz 2026-10-18 R10.1: Ayarlar › Import › "Rekordbox içe aktarma
 * profilleri" against the real private data service of tests/run_admin_ui.py.
 * A profile is created (a bad rule line keeps the dialog and its inputs),
 * edited and disabled, enabled again, then a rekordbox XML is previewed and
 * imported with it; every step is read back from the database. */

static RadioSConfig config;
static RadioSDataClient *reader;
typedef struct { RadioSViewContext ctx; GtkWidget *window, *page; } Harness;

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *dialog(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *result = NULL;
    for (GList *item = windows; item && !result; item = item->next)
        if (gtk_widget_get_visible(item->data) && g_str_equal(gtk_widget_get_name(item->data), name)) result = item->data;
    g_list_free(windows); return result;
}
static void pump(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
typedef gboolean (*Condition)(gpointer);
static void wait_for(Condition condition, gpointer value, const gchar *message)
{
    gint64 deadline = g_get_monotonic_time() + 30 * G_TIME_SPAN_SECOND;
    while (!condition(value) && g_get_monotonic_time() < deadline) pump();
    if (!condition(value)) g_error("Import profile acceptance timed out: %s", message);
}
static gboolean ready(gpointer value)
{ Harness *h = value; return radios_data_connected(&h->ctx) && gtk_widget_is_sensitive(find(h->page, "import-profiles-add")); }
static gboolean dialog_open(gpointer name) { return dialog(name) != NULL; }
static gboolean dialog_closed(gpointer name) { return dialog(name) == NULL; }
static gboolean sensitive(gpointer widget) { return gtk_widget_is_sensitive(widget); }
static gboolean insensitive(gpointer widget) { return !gtk_widget_is_sensitive(widget); }
static gint rows(GtkWidget *page)
{ return gtk_tree_model_iter_n_children(gtk_tree_view_get_model(GTK_TREE_VIEW(find(page, "import-profiles-list"))), NULL); }
static gboolean one_row(gpointer page) { return rows(page) == 1; }
static void set_entry(GtkWidget *root, const gchar *name, const gchar *text)
{ GtkWidget *entry = find(root, name); g_assert_true(GTK_IS_ENTRY(entry)); gtk_entry_set_text(GTK_ENTRY(entry), text); }
static void set_lines(GtkWidget *root, const gchar *name, const gchar *text)
{
    GtkWidget *view = find(root, name); g_assert_true(GTK_IS_TEXT_VIEW(view));
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(view)), text, -1);
}
static const gchar *label(GtkWidget *root, const gchar *name) { return gtk_label_get_text(GTK_LABEL(find(root, name))); }
static void click(GtkWidget *root, const gchar *name)
{
    GtkWidget *button = find(root, name); g_assert_true(GTK_IS_BUTTON(button));
    wait_for(sensitive, button, name); gtk_button_clicked(GTK_BUTTON(button));
}
static void select_first(GtkWidget *page)
{
    GtkTreeView *view = GTK_TREE_VIEW(find(page, "import-profiles-list")); GtkTreeIter iter;
    g_assert_true(gtk_tree_model_get_iter_first(gtk_tree_view_get_model(view), &iter));
    gtk_tree_selection_select_iter(gtk_tree_view_get_selection(view), &iter);
}

static JsonNode *profiles(void)
{
    gchar *escaped = g_uri_escape_string(radios_data_client_station_id(reader), NULL, FALSE);
    gchar *path = g_strconcat("/v1/import-profiles?station_id=", escaped, NULL); GError *error = NULL;
    JsonNode *result = radios_data_client_request_sync(reader, "GET", path, NULL, NULL, &error);
    g_assert_no_error(error); g_assert_true(JSON_NODE_HOLDS_ARRAY(result)); g_free(path); g_free(escaped);
    return result;
}
static JsonObject *only(JsonNode *list)
{ g_assert_cmpuint(json_array_get_length(json_node_get_array(list)), ==, 1); return json_array_get_object_element(json_node_get_array(list), 0); }
typedef struct { const gchar *status; gint64 version; } Saved;
static gboolean profile_saved(gpointer value)
{
    Saved *want = value; JsonNode *list = profiles(); gboolean done = FALSE;
    if (json_array_get_length(json_node_get_array(list)) == 1) {
        JsonObject *row = json_array_get_object_element(json_node_get_array(list), 0);
        done = g_str_equal(radios_json_string(row, "status", ""), want->status) && radios_json_int(row, "config_version", 0) == want->version;
    }
    json_node_free(list); return done;
}
static JsonObject *preview_counts(JsonNode **holder, const gchar *profile, const gchar *encoded)
{
    JsonNode *body = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new(); json_node_take_object(body, object);
    json_object_set_string_member(object, "station_id", radios_data_client_station_id(reader));
    json_object_set_string_member(object, "profile_id", profile); json_object_set_string_member(object, "xml_base64", encoded);
    GError *error = NULL; *holder = radios_data_client_request_sync(reader, "POST", "/v1/imports/rekordbox/preview", body, NULL, &error);
    g_assert_no_error(error); json_node_free(body);
    return json_object_get_object_member(json_node_get_object(*holder), "counts");
}
static gboolean summary_has(gpointer text)
{
    GtkWidget *d = dialog("import-profile-xml-dialog");
    return d && strstr(label(d, "import-profile-xml-summary"), text) != NULL;
}
static gboolean chooser_set(gpointer path)
{
    GtkWidget *d = dialog("import-profile-xml-dialog"); if (!d) return FALSE;
    gchar *chosen = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(find(d, "import-profile-xml-file")));
    gboolean same = chosen && g_str_equal(chosen, path); g_free(chosen); return same;
}

static const gchar XML[] = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<DJ_PLAYLISTS Version=\"1.0.0\"><PRODUCT Name=\"rekordbox\" Version=\"7\" Company=\"AlphaTheta\"/>\n"
    "<COLLECTION Entries=\"2\">\n"
    "<TRACK TrackID=\"1\" Name=\"Kulüp Bir\" Artist=\"Sanatçı\" TotalTime=\"123\" Location=\"file://localhost/Volumes/DJ/Muzik/bir.wav\"/>\n"
    "<TRACK TrackID=\"2\" Name=\"Kulüp İki\" TotalTime=\"60\" Location=\"file://localhost/Volumes/DJ/Muzik/iki.wav\"/>\n"
    "</COLLECTION><PLAYLISTS><NODE Type=\"0\" Name=\"ROOT\" Count=\"1\">\n"
    "<NODE Type=\"1\" Name=\"Kulüp seti\" KeyType=\"0\" Entries=\"2\"><TRACK Key=\"1\"/><TRACK Key=\"2\"/></NODE>\n"
    "</NODE></PLAYLISTS></DJ_PLAYLISTS>\n";

static void test_profiles(void)
{
    Harness h = {0}; h.ctx.config = &config; h.ctx.runtime = radios_runtime_new(64, 256);
    h.window = gtk_window_new(GTK_WINDOW_TOPLEVEL); h.ctx.parent = GTK_WINDOW(h.window);
    gtk_window_set_default_size(GTK_WINDOW(h.window), 1100, 900);
    h.ctx.data = radios_data_new(&h.ctx); h.page = radios_transfer_import_page_new(&h.ctx);
    gtk_container_add(GTK_CONTAINER(h.window), h.page); gtk_widget_show_all(h.window);
    radios_data_connect(&h.ctx); wait_for(ready, &h, "connected import profile section");
    g_assert_cmpint(rows(h.page), ==, 0);
    g_assert_false(gtk_widget_is_sensitive(find(h.page, "import-profiles-edit")));
    gchar *folder = g_dir_make_tmp("import-profiles-XXXXXX", NULL), *music = g_build_filename(folder, "muzik", NULL);
    gchar *rule = g_strdup_printf("/Volumes/DJ/Muzik => %s", music), *bad = g_strdup_printf("%s\nyerel kök yok", rule);

    /* Create: a malformed second rule line keeps the dialog and its inputs. */
    click(h.page, "import-profiles-add"); wait_for(dialog_open, "import-profile-dialog", "new profile dialog");
    GtkWidget *d = dialog("import-profile-dialog");
    set_entry(d, "import-profile-name", "Kulüp DJ dizüstü"); set_entry(d, "import-profile-library-key", "kulup-dj-dizustu");
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(d, "import-profile-cue")), "propose_radio_cues"));
    set_lines(d, "import-profile-path-rules", bad); set_lines(d, "import-profile-roots", music);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT); pump();
    g_assert_true(dialog("import-profile-dialog") == d);
    g_assert_nonnull(strstr(label(d, "import-profile-status"), "Yol kuralı 2"));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(d, "import-profile-name"))), ==, "Kulüp DJ dizüstü");
    set_lines(d, "import-profile-path-rules", rule);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT); wait_for(dialog_closed, "import-profile-dialog", "profile saved");
    Saved want = {"active", 1}; wait_for(profile_saved, &want, "profile in the database");
    JsonNode *list = profiles(); JsonObject *row = only(list), *config_object = json_object_get_object_member(row, "config");
    g_assert_cmpstr(radios_json_string(row, "name", ""), ==, "Kulüp DJ dizüstü");
    g_assert_cmpstr(radios_json_string(row, "library_key", ""), ==, "kulup-dj-dizustu");
    g_assert_cmpstr(radios_json_string(config_object, "cue_policy", ""), ==, "propose_radio_cues");
    JsonArray *rules = json_object_get_array_member(config_object, "path_rules"); g_assert_cmpuint(json_array_get_length(rules), ==, 1);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(rules, 0), "source_root", ""), ==, "/Volumes/DJ/Muzik");
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(rules, 0), "target_root", ""), ==, music);
    g_assert_cmpstr(json_array_get_string_element(json_object_get_array_member(config_object, "allowed_roots"), 0), ==, music);
    gchar *profile = g_strdup(radios_json_string(row, "_id", "")); json_node_free(list);
    wait_for(one_row, h.page, "saved profile listed");

    /* Edit and disable: the XML import is offered only for an active profile. */
    select_first(h.page); g_assert_true(gtk_widget_is_sensitive(find(h.page, "import-profiles-xml")));
    click(h.page, "import-profiles-edit"); wait_for(dialog_open, "import-profile-dialog", "edit dialog");
    d = dialog("import-profile-dialog");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(d, "import-profile-library-key"))), ==, "kulup-dj-dizustu");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(d, "import-profile-cue"))), ==, "propose_radio_cues");
    set_entry(d, "import-profile-name", "Kulüp DJ dizüstü · yedek");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(d, "import-profile-active")), FALSE);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT); wait_for(dialog_closed, "import-profile-dialog", "profile edited");
    want = (Saved){"disabled", 2}; wait_for(profile_saved, &want, "disabled profile in the database");
    list = profiles(); row = only(list);
    g_assert_cmpstr(radios_json_string(row, "name", ""), ==, "Kulüp DJ dizüstü · yedek");
    g_assert_cmpstr(radios_json_string(json_object_get_object_member(row, "config"), "cue_policy", ""), ==, "propose_radio_cues");
    json_node_free(list);
    wait_for(insensitive, find(h.page, "import-profiles-xml"), "disabled profile offers no XML import");
    click(h.page, "import-profiles-edit"); wait_for(dialog_open, "import-profile-dialog", "enable dialog");
    d = dialog("import-profile-dialog"); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(d, "import-profile-active")), TRUE);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT); wait_for(dialog_closed, "import-profile-dialog", "profile enabled");
    want = (Saved){"active", 3}; wait_for(profile_saved, &want, "enabled profile in the database");

    /* XML import with the profile: the preview the page shows is the
     * service's own; the apply commits exactly that preview. */
    gchar *xml = g_build_filename(folder, "kulup.xml", NULL); g_assert_true(g_file_set_contents(xml, XML, -1, NULL));
    gchar *encoded = g_base64_encode((const guchar *)XML, strlen(XML)); JsonNode *before = NULL;
    JsonObject *expected = preview_counts(&before, profile, encoded);
    g_assert_cmpint(radios_json_int(expected, "create", 0), ==, 2);
    gchar *paths = g_strdup_printf("eşlendi 2 · eşlenmedi 0 · geçersiz 0 · Playlist %u",
        json_array_get_length(json_object_get_array_member(json_node_get_object(before), "playlist_nodes")));
    click(h.page, "import-profiles-xml"); wait_for(dialog_open, "import-profile-xml-dialog", "XML import dialog");
    d = dialog("import-profile-xml-dialog");
    g_assert_false(gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT)));
    gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(find(d, "import-profile-xml-file")), xml);
    wait_for(chooser_set, xml, "XML chosen");
    gtk_dialog_response(GTK_DIALOG(d), 1); wait_for(summary_has, "Yeni 2 ·", "service preview shown");
    g_assert_nonnull(strstr(label(d, "import-profile-xml-summary"), paths)); g_free(paths);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_ACCEPT); wait_for(summary_has, "İçe aktarıldı: yeni parça 2", "import applied");
    g_assert_true(g_str_has_prefix(label(h.page, "import-profiles-status"), "İçe aktarıldı: yeni parça 2 ·"));
    JsonNode *after = NULL; JsonObject *again = preview_counts(&after, profile, encoded);
    g_assert_cmpint(radios_json_int(again, "create", 0), ==, 0);
    g_assert_cmpint(radios_json_int(again, "create", 0) + radios_json_int(again, "update", 0) + radios_json_int(again, "preserve", 0), ==, 2);
    gtk_dialog_response(GTK_DIALOG(d), GTK_RESPONSE_CLOSE); wait_for(dialog_closed, "import-profile-xml-dialog", "XML dialog closed");
    json_node_free(before); json_node_free(after);
    g_assert_cmpint(g_unlink(xml), ==, 0); g_assert_cmpint(g_rmdir(folder), ==, 0);
    g_free(xml); g_free(encoded); g_free(profile); g_free(rule); g_free(bad); g_free(music); g_free(folder);
    gtk_widget_destroy(h.window); radios_data_close(h.ctx.data); radios_runtime_free(h.ctx.runtime); radios_data_unref(h.ctx.data);
    for (guint i = 0; i < 20; ++i) pump();
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]);
    for (gint i = 1; i < argc - 1; ++i) argv[i] = argv[i + 1];
    --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv); GError *error = NULL;
    g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/native-import-profiles/create-edit-disable-xml-import", test_profiles);
    gint result = g_test_run();
    radios_data_client_unref(reader); radios_config_clear(&config); return result;
}
