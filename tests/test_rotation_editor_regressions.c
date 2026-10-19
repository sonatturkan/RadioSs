#include "radios/rotation_editor.h"
#include "radios/audio_ui.h"
#include "radios/data.h"
#include "radios/widgets.h"
#include "radios/transition_distribution.h"
#include "radios/transition_preset.h"
#include "radios/autodj_automation_ui.h"
#include <glib/gstdio.h>
#include <string.h>

/* Use the real immutable client identity and editor, with a small main-thread
 * data adapter. No HTTP requests or user installation are used by this test. */
typedef struct {
    RadioSViewContext ctx;
    RadioSConfig config;
    RadioSDataClient *client;
    GtkWidget *window, *editor;
    JsonNode *saved;
    gchar *root;
    guint changes, destroyed;
} Fixture;

RadioSDataClient *radios_data_client_acquire(RadioSViewContext *ctx)
{
    Fixture *fixture = ctx->notify_data;
    return fixture && fixture->client ? radios_data_client_ref(fixture->client) : NULL;
}

/* Mixer strip labels (dj_mixer channels[n].label) the live step dialog reads
 * from the audio layer; this binary has no audio controller. */
static const gchar *strip_labels[RADIOS_AUDIO_MIXER_CHANNELS + 1];
const gchar *radios_audio_ui_get_channel_label(RadioSViewContext *ctx, guint channel)
{ (void)ctx; return channel >= 1 && channel <= RADIOS_AUDIO_MIXER_CHANNELS ? strip_labels[channel] : NULL; }

/* These legacy draft tests do not exercise playlist creation. Production
 * data calls are deliberately refused; real HTTP/store acceptance lives in
 * test_automation_probability_ui.c. */
gboolean radios_data_connected(RadioSViewContext *ctx)
{ Fixture *fixture = ctx->notify_data; return fixture && fixture->client; }
JsonNode *radios_data_body(RadioSViewContext *ctx)
{
    (void)ctx; JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, json_object_new()); return node;
}
gboolean radios_data_request(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *method, const gchar *path, JsonNode *body,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{
    (void)ctx; (void)owner; (void)method; (void)path; (void)body; (void)callback;
    if (destroy) destroy(user);
    return FALSE;
}
void radios_data_refresh(RadioSViewContext *ctx) { (void)ctx; }

const gchar *radios_json_string(JsonObject *object, const gchar *key, const gchar *fallback)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING
        ? json_node_get_string(node) : fallback;
}

gint64 radios_json_int(JsonObject *object, const gchar *key, gint64 fallback)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64
        ? json_node_get_int(node) : fallback;
}

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (!g_strcmp0(gtk_widget_get_name(root), name)) return root;
    GList *menus = gtk_menu_get_for_attach_widget(root);
    for (GList *item = menus; item; item = item->next) {
        GtkWidget *found = find(item->data, name); if (found) return found;
    }
    if (GTK_IS_MENU_BUTTON(root)) {
        GtkWidget *popover = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(root)));
        GtkWidget *found = popover ? find(popover, name) : NULL; if (found) return found;
    }
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = find(item->data, name);
    g_list_free(children); return found;
}

static GtkWidget *field(Fixture *fixture, const gchar *suffix)
{
    gchar *name = g_strconcat("rotation-editor-", suffix, NULL);
    GtkWidget *widget = find(fixture->editor, name); g_free(name); g_assert_nonnull(widget); return widget;
}

static JsonNode *parse(const gchar *text)
{
    JsonParser *parser = json_parser_new(); GError *error = NULL;
    g_assert_true(json_parser_load_from_data(parser, text, -1, &error)); g_assert_no_error(error);
    JsonNode *node = json_parser_steal_root(parser); g_object_unref(parser); return node;
}

static GtkWidget *dialog_find(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *found = NULL;
    for (GList *item = windows; item && !found; item = item->next)
        if (!g_strcmp0(gtk_widget_get_name(item->data), name)) found = item->data;
    g_list_free(windows); return found;
}

static void capture(GtkWidget *editor, JsonNode *snapshot, gpointer user)
{
    (void)editor; Fixture *fixture = user;
    g_clear_pointer(&fixture->saved, json_node_free);
    gchar *text = json_to_string(snapshot, FALSE); fixture->saved = parse(text); g_free(text); ++fixture->changes;
}

static void owner_freed(gpointer user) { ++((Fixture *)user)->destroyed; }

static void remove_tree(const gchar *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        GDir *directory = g_dir_open(path, 0, NULL); const gchar *name;
        while (directory && (name = g_dir_read_name(directory))) {
            gchar *child = g_build_filename(path, name, NULL); remove_tree(child); g_free(child);
        }
        if (directory) g_dir_close(directory);
        g_rmdir(path);
    } else g_unlink(path);
}

static void fixture_start(Fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture)); GError *error = NULL;
    fixture->root = g_dir_make_tmp("radios-rotation-regressions-XXXXXX", &error); g_assert_no_error(error);
    gchar *directory = g_build_filename(fixture->root, "config", NULL); g_assert_cmpint(g_mkdir(directory, 0700), ==, 0);
    gchar *credentials = g_build_filename(directory, "credentials.json", NULL); g_free(directory);
    fixture->config.project_root = g_strdup(fixture->root);
    fixture->config.database_file = g_build_filename(fixture->root, "RadioS.sdb", NULL);
    fixture->config.service_url = g_strdup("http://127.0.0.1:1");
    fixture->config.connect_timeout_seconds = 1; fixture->config.request_timeout_seconds = 1;
    g_assert_true(g_file_set_contents(fixture->config.database_file, "private test journal", -1, &error)); g_assert_no_error(error);
    gchar *metadata = g_strconcat(fixture->config.database_file, ".meta", NULL);
    g_assert_true(g_file_set_contents(metadata, "{\"schema_version\":4,\"store_id\":\"11111111-1111-4111-8111-111111111111\"}", -1, &error));
    g_assert_no_error(error); g_free(metadata);
    g_assert_true(g_file_set_contents(credentials,
        "{\"schema_version\":1,\"store_id\":\"11111111-1111-4111-8111-111111111111\","
        "\"user_id\":\"22222222-2222-4222-8222-222222222222\",\"station_id\":\"33333333-3333-4333-8333-333333333333\","
        "\"endpoint_id\":\"44444444-4444-4444-8444-444444444444\",\"database_file\":\"RadioS.sdb\","
        "\"api_token\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"endpoint_token\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}", -1, &error));
    g_assert_no_error(error); g_assert_cmpint(g_chmod(credentials, 0600), ==, 0); g_free(credentials);
    fixture->client = radios_data_client_new(&fixture->config, &error); g_assert_no_error(error); g_assert_nonnull(fixture->client);
    fixture->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(fixture->window);
    fixture->ctx.parent = GTK_WINDOW(fixture->window); fixture->ctx.notify_data = fixture;
    fixture->ctx.data = (RadioSData *)fixture;
    RadioSRotationEditorHooks hooks = {capture, fixture, owner_freed};
    fixture->editor = radios_rotation_editor_new(&fixture->ctx, &hooks); g_object_ref_sink(fixture->editor);
    gtk_container_add(GTK_CONTAINER(fixture->window), fixture->editor);
    JsonNode *config = parse("{\"id\":\"55555555-5555-4555-8555-555555555555\",\"name\":\"Test rotation\",\"sources\":[{"
        "\"id\":\"66666666-6666-4666-8666-666666666666\",\"playlist_id\":\"77777777-7777-4777-8777-777777777777\","
        "\"frequency\":1,\"count\":1,\"weight\":100,\"start_minute\":0,\"end_minute\":1440,\"interval_minutes\":0,"
        "\"random\":false,\"track_random\":false,\"advert_random\":false,\"sweeper_random\":false}]}");
    radios_rotation_editor_set_config(fixture->editor, json_node_get_object(config)); json_node_free(config);
    JsonNode *catalog = parse("[{\"_id\":\"77777777-7777-4777-8777-777777777777\",\"name\":\"Test playlist\",\"purpose\":\"music\","
        "\"tracks\":[{\"_id\":\"88888888-8888-4888-8888-888888888888\",\"metadata\":{\"title\":\"Test song\",\"duration_ms\":1000}}]}]");
    radios_rotation_editor_set_playlists(fixture->editor, json_node_get_array(catalog)); json_node_free(catalog);
    GtkTreePath *path = gtk_tree_path_new_from_indices(0, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(field(fixture, "sources"))), path);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(field(fixture, "songs"))), path);
    gtk_tree_path_free(path);
    g_assert_true(radios_rotation_editor_ready(fixture->editor)); g_assert_cmpuint(fixture->changes, ==, 0);
}

static void fixture_clear(Fixture *fixture)
{
    gtk_widget_destroy(fixture->window); g_object_unref(fixture->window); g_object_unref(fixture->editor);
    g_assert_cmpuint(fixture->destroyed, ==, 1);
    g_clear_pointer(&fixture->saved, json_node_free); radios_data_client_unref(fixture->client);
    radios_config_clear(&fixture->config); remove_tree(fixture->root); g_free(fixture->root);
}

static JsonObject *saved_source(Fixture *fixture)
{
    g_assert_nonnull(fixture->saved);
    return json_array_get_object_element(json_object_get_array_member(json_node_get_object(fixture->saved), "sources"), 0);
}

static void source_draft_does_not_block_weight(void)
{
    Fixture fixture; fixture_start(&fixture);
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "source-start")), "09:");
    g_assert_cmpuint(fixture.changes, ==, 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(field(&fixture, "source-weight")), 37);
    g_assert_cmpuint(fixture.changes, >, 0);
    g_assert_cmpint(json_object_get_int_member(saved_source(&fixture), "weight"), ==, 37);
    g_assert_cmpint(json_object_get_int_member(saved_source(&fixture), "start_minute"), ==, 0);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "source-start"))), ==, "09:");
    fixture_clear(&fixture);
}

static void song_draft_preserves_complete_rule(void)
{
    Fixture fixture; fixture_start(&fixture);
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "song-start")), "10:");
    g_assert_cmpuint(fixture.changes, ==, 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(field(&fixture, "song-weight")), 41);
    JsonArray *rules = json_object_get_array_member(saved_source(&fixture), "track_rules");
    g_assert_cmpuint(json_array_get_length(rules), ==, 1);
    JsonObject *rule = json_array_get_object_element(rules, 0);
    g_assert_cmpuint(json_object_get_size(rule), ==, 8);
    g_assert_cmpint(json_object_get_int_member(rule, "weight"), ==, 41);
    g_assert_cmpint(json_object_get_int_member(rule, "frequency"), ==, 1);
    g_assert_cmpint(json_object_get_int_member(rule, "end_minute"), ==, 1440);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "song-start"))), ==, "10:");
    fixture_clear(&fixture);
}

static void reconnect_same_identity_keeps_editor_writable(void)
{
    Fixture fixture; fixture_start(&fixture); GError *error = NULL;
    RadioSDataClient *replacement = radios_data_client_new(&fixture.config, &error); g_assert_no_error(error);
    g_assert_true(replacement != fixture.client);
    radios_data_client_unref(fixture.client); fixture.client = replacement;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(field(&fixture, "source-weight")), 29);
    g_assert_cmpint(json_object_get_int_member(saved_source(&fixture), "weight"), ==, 29);
    g_assert_true(radios_rotation_editor_ready(fixture.editor)); fixture_clear(&fixture);
}

static void settings_drafts_do_not_block_independent_fields(void)
{
    Fixture fixture; fixture_start(&fixture);
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "track-gap")), "");
    g_assert_cmpuint(fixture.changes, ==, 0);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(field(&fixture, "artist-gap")), 17);
    JsonObject *settings = json_object_get_object_member(json_node_get_object(fixture.saved), "settings");
    g_assert_cmpint(json_object_get_int_member(settings, "artist_gap_minutes"), ==, 17);
    g_assert_cmpint(json_object_get_int_member(settings, "track_gap_minutes"), ==, 0);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "track-gap"))), ==, "");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(field(&fixture, "album-gap")), 25);
    settings = json_object_get_object_member(json_node_get_object(fixture.saved), "settings");
    g_assert_cmpint(json_object_get_int_member(settings, "album_gap_minutes"), ==, 25);
    /* Transition choices are no longer edited here; the stored array stays as saved. */
    JsonArray *transitions = json_object_get_array_member(settings, "transitions");
    g_assert_cmpuint(json_array_get_length(transitions), ==, 3);
    g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(transitions, 0), "mode"), ==, 0);
    g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(transitions, 0), "weight"), ==, 100);
    g_assert_null(find(fixture.editor, "rotation-editor-transition-crossfade-weight"));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "track-gap"))), ==, "");
    fixture_clear(&fixture);
}

static void source_settings_drafts_do_not_block_independent_fields(void)
{
    Fixture fixture; fixture_start(&fixture);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "source-override")), TRUE);
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "source-track-gap")), "");
    guint previous = fixture.changes;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(field(&fixture, "source-album-gap")), 23);
    g_assert_cmpuint(fixture.changes, >, previous);
    JsonObject *settings = json_object_get_object_member(saved_source(&fixture), "settings");
    g_assert_cmpint(json_object_get_int_member(settings, "album_gap_minutes"), ==, 23);
    g_assert_cmpint(json_object_get_int_member(settings, "track_gap_minutes"), ==, 0);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "source-track-gap"))), ==, "");
    fixture_clear(&fixture);
}

static void adding_and_reordering_preserves_unfinished_drafts(void)
{
    Fixture fixture; fixture_start(&fixture);
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "source-start")), "09:");
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "track-gap")), "");
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "add")));
    JsonArray *sources = json_object_get_array_member(json_node_get_object(fixture.saved), "sources");
    g_assert_cmpuint(json_array_get_length(sources), ==, 2);
    g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(sources, 0), "start_minute"), ==, 0);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "track-gap"))), ==, "");
    GtkTreePath *path = gtk_tree_path_new_from_indices(0, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(field(&fixture, "sources"))), path);
    gtk_tree_path_free(path);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "source-start"))), ==, "09:");
    guint previous = fixture.changes; gtk_button_clicked(GTK_BUTTON(field(&fixture, "down")));
    g_assert_cmpuint(fixture.changes, >, previous);
    sources = json_object_get_array_member(json_node_get_object(fixture.saved), "sources");
    g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(sources, 1), "id"), ==, "66666666-6666-4666-8666-666666666666");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "source-start"))), ==, "09:");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "track-gap"))), ==, "");
    fixture_clear(&fixture);
}

static void routing_assignment_ignores_unfinished_clock(void)
{
    Fixture fixture; fixture_start(&fixture);
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(card);
    g_object_set_data(G_OBJECT(fixture.window), "radios-deck-owner-0", card);
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "source-start")), "09:");
    gtk_menu_item_activate(GTK_MENU_ITEM(field(&fixture, "autodj")));
    GtkWidget *dialog = field(&fixture, "autodj-dialog"); g_assert_false(GTK_IS_WINDOW(dialog));
    gtk_button_clicked(GTK_BUTTON(find(dialog, "rotation-editor-autodj-apply")));
    JsonObject *routing = json_object_get_object_member(saved_source(&fixture), "autodj");
    const gchar *kinds[] = {"music", "advertisement", "jingle", "sweeper"};
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i)
        g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(routing, kinds[i]), 0), ==, 1);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "source-start"))), ==, "09:");
    g_object_set_data(G_OBJECT(fixture.window), "radios-deck-owner-0", NULL); g_object_unref(card);
    fixture_clear(&fixture);
}

static void reconnect_different_identity_rejects_edits(void)
{
    Fixture fixture; fixture_start(&fixture); GError *error = NULL;
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "probability")));
    JsonNode *opened_config = radios_rotation_editor_snapshot(fixture.editor); g_assert_nonnull(opened_config);
    gchar *path = g_build_filename(fixture.root, "config", "credentials.json", NULL), *text = NULL;
    g_assert_true(g_file_get_contents(path, &text, NULL, &error)); g_assert_no_error(error);
    JsonNode *credentials = parse(text); g_free(text);
    json_object_set_string_member(json_node_get_object(credentials), "user_id", "99999999-9999-4999-8999-999999999999");
    text = json_to_string(credentials, FALSE); json_node_free(credentials);
    g_assert_true(g_file_set_contents(path, text, -1, &error)); g_assert_no_error(error);
    g_assert_cmpint(g_chmod(path, 0600), ==, 0); g_free(path); g_free(text);
    RadioSDataClient *replacement = radios_data_client_new(&fixture.config, &error); g_assert_no_error(error);
    radios_data_client_unref(fixture.client); fixture.client = replacement;
    radios_rotation_editor_set_config(fixture.editor, json_node_get_object(opened_config)); json_node_free(opened_config);
    g_assert_null(find(fixture.editor, "rotation-editor-probability-dialog"));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(field(&fixture, "pages"))), ==, "library");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(field(&fixture, "source-weight")), 19);
    g_assert_cmpuint(fixture.changes, ==, 0); g_assert_false(radios_rotation_editor_ready(fixture.editor));
    fixture_clear(&fixture);
}

/* "Olmayan türler": the four toggles write settings.excluded_kinds in kind
 * order, reopen with the same state, and an empty selection drops the field. */
static void excluded_kinds_round_trip_and_empty_drop(void)
{
    Fixture fixture; fixture_start(&fixture);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "excluded-kind-advertisement")), TRUE);
    JsonObject *settings = json_object_get_object_member(json_node_get_object(fixture.saved), "settings");
    JsonArray *excluded = json_object_get_array_member(settings, "excluded_kinds");
    g_assert_cmpuint(json_array_get_length(excluded), ==, 1); g_assert_cmpstr(json_array_get_string_element(excluded, 0), ==, "advertisement");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "excluded-kind-sweeper")), TRUE);
    settings = json_object_get_object_member(json_node_get_object(fixture.saved), "settings");
    excluded = json_object_get_array_member(settings, "excluded_kinds");
    g_assert_cmpuint(json_array_get_length(excluded), ==, 2); g_assert_cmpstr(json_array_get_string_element(excluded, 1), ==, "sweeper");
    JsonNode *config = radios_rotation_editor_snapshot(fixture.editor); g_assert_nonnull(config);
    radios_rotation_editor_set_config(fixture.editor, json_node_get_object(config)); json_node_free(config);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field(&fixture, "excluded-kind-advertisement"))));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field(&fixture, "excluded-kind-sweeper"))));
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field(&fixture, "excluded-kind-jingle"))));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "excluded-kind-advertisement")), FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "excluded-kind-sweeper")), FALSE);
    settings = json_object_get_object_member(json_node_get_object(fixture.saved), "settings");
    g_assert_false(json_object_has_member(settings, "excluded_kinds"));
    fixture_clear(&fixture);
}
static void sparse_override_keeps_repeat_limits_only(void)
{
    Fixture fixture; fixture_start(&fixture);
    JsonNode *config = radios_rotation_editor_snapshot(fixture.editor); g_assert_nonnull(config);
    JsonObject *source = json_array_get_object_element(json_object_get_array_member(json_node_get_object(config), "sources"), 0);
    JsonObject *override = json_object_new(); json_object_set_int_member(override, "track_gap_minutes", 7);
    json_object_set_object_member(source, "settings", override);
    radios_rotation_editor_set_config(fixture.editor, json_node_get_object(config)); json_node_free(config);
    GtkTreePath *path = gtk_tree_path_new_from_indices(0, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(field(&fixture, "sources"))), path);
    gtk_tree_path_free(path);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(field(&fixture, "source-artist-gap")), 33);
    JsonObject *settings = json_object_get_object_member(saved_source(&fixture), "settings");
    /* A playlist override holds repeat limits only; transitions stay rotation-wide. */
    g_assert_false(json_object_has_member(settings, "transitions"));
    g_assert_false(json_object_has_member(settings, "transition_selection_mode"));
    g_assert_cmpint(json_object_get_int_member(settings, "artist_gap_minutes"), ==, 33);
    g_assert_cmpint(json_object_get_int_member(settings, "track_gap_minutes"), ==, 7);
    g_assert_null(find(fixture.editor, "rotation-editor-source-transition-crossfade-weight"));
    g_assert_null(find(fixture.editor, "rotation-editor-source-routing"));
    fixture_clear(&fixture);
}

static gboolean destroy_during_chooser(gpointer user)
{
    Fixture *fixture = user; GList *windows = gtk_window_list_toplevels(); gboolean found = FALSE;
    for (GList *item = windows; item; item = item->next)
        if (!g_strcmp0(gtk_widget_get_name(item->data), "rotation-editor-add-dialog")) found = TRUE;
    g_list_free(windows); g_assert_true(found); gtk_widget_destroy(fixture->editor); return G_SOURCE_REMOVE;
}

static void chooser_survives_owner_destroy(void)
{
    Fixture fixture; fixture_start(&fixture);
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "source-start")), "11:");
    g_idle_add(destroy_during_chooser, &fixture);
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "add-choose")));
    g_assert_false(radios_rotation_editor_ready(fixture.editor));
    g_assert_cmpuint(fixture.changes, ==, 0); fixture_clear(&fixture);
}

static gboolean accept_during_chooser(gpointer user)
{
    (void)user; GtkWidget *dialog = dialog_find("rotation-editor-add-dialog"); g_assert_nonnull(dialog);
    GtkWidget *tree = find(dialog, "rotation-editor-add-list"); g_assert_nonnull(tree);
    GtkTreePath *path = gtk_tree_path_new_from_indices(0, -1);
    gtk_tree_view_row_activated(GTK_TREE_VIEW(tree), path, gtk_tree_view_get_column(GTK_TREE_VIEW(tree), 0));
    gtk_tree_path_free(path); gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); return G_SOURCE_REMOVE;
}

static void chooser_adds_while_clock_is_unfinished(void)
{
    Fixture fixture; fixture_start(&fixture);
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "source-start")), "11:");
    g_idle_add(accept_during_chooser, &fixture);
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "add-choose")));
    JsonArray *sources = json_object_get_array_member(json_node_get_object(fixture.saved), "sources");
    g_assert_cmpuint(json_array_get_length(sources), ==, 2);
    g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(sources, 0), "start_minute"), ==, 0);
    GtkTreePath *path = gtk_tree_path_new_from_indices(0, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(field(&fixture, "sources"))), path);
    gtk_tree_path_free(path);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "source-start"))), ==, "11:");
    fixture_clear(&fixture);
}

static void capture_layout(GtkWidget *window, const gchar *variable)
{
    const gchar *path = g_getenv(variable); if (!path || !*path) return;
    gtk_window_set_default_size(GTK_WINDOW(window), 760, 650); gtk_widget_show_all(window);
    while (gtk_events_pending()) gtk_main_iteration();
    GdkWindow *surface = gtk_widget_get_window(window); g_assert_nonnull(surface);
    GdkPixbuf *capture = gdk_pixbuf_get_from_window(surface, 0, 0,
        gdk_window_get_width(surface), gdk_window_get_height(surface)); g_assert_nonnull(capture);
    GError *error = NULL; g_assert_true(gdk_pixbuf_save(capture, path, "png", &error, NULL));
    g_assert_no_error(error); g_object_unref(capture);
}

static guint visible_application_windows(void)
{
    GList *windows = gtk_window_list_toplevels(); guint count = 0;
    for (GList *item = windows; item; item = item->next)
        if (gtk_window_get_window_type(GTK_WINDOW(item->data)) == GTK_WINDOW_TOPLEVEL &&
            gtk_widget_get_visible(GTK_WIDGET(item->data))) ++count;
    g_list_free(windows); return count;
}

static void simple_workspace_and_integrated_settings(void)
{
    Fixture fixture; fixture_start(&fixture);
    GtkTreeView *sources = GTK_TREE_VIEW(field(&fixture, "sources"));
    g_assert_cmpint(gtk_tree_view_get_n_columns(sources), ==, 3);
    const gchar *titles[] = {"Playlist", "Tür", "Parça"};
    for (guint i = 0; i < G_N_ELEMENTS(titles); ++i)
        g_assert_cmpstr(gtk_tree_view_column_get_title(gtk_tree_view_get_column(sources, i)), ==, titles[i]);
    g_assert_false(gtk_expander_get_expanded(GTK_EXPANDER(field(&fixture, "settings"))));
    g_assert_false(gtk_expander_get_expanded(GTK_EXPANDER(field(&fixture, "source-advanced"))));
    g_assert_false(gtk_expander_get_expanded(GTK_EXPANDER(field(&fixture, "song-advanced"))));
    g_assert_false(gtk_widget_get_visible(field(&fixture, "quick-add")));
    capture_layout(fixture.window, "RADIOS_SIMPLE_ROTATION_CAPTURE");
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(card);
    g_object_set_data(G_OBJECT(card), "radios-player-name", "Ana yayın");
    g_object_set_data(G_OBJECT(fixture.window), "radios-deck-owner-0", card);
    guint count = visible_application_windows();
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "source-probability")));
    GtkWidget *routing = field(&fixture, "autodj-dialog");
    g_assert_false(GTK_IS_WINDOW(routing));
    g_assert_true(gtk_widget_get_toplevel(routing) == fixture.window);
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(field(&fixture, "pages"))), ==, "routing");
    g_assert_cmpint(gtk_notebook_get_current_page(GTK_NOTEBOOK(field(&fixture, "autodj-settings-tabs"))), ==, 1);
    g_assert_nonnull(find(routing, "probability-editor-root"));
    g_assert_cmpuint(visible_application_windows(), ==, count);
    gtk_notebook_set_current_page(GTK_NOTEBOOK(field(&fixture, "autodj-settings-tabs")), 0);
    capture_layout(fixture.window, "RADIOS_SIMPLE_ROUTING_CAPTURE");
    gtk_button_clicked(GTK_BUTTON(find(routing, "rotation-editor-autodj-cancel")));
    g_assert_null(find(fixture.editor, "rotation-editor-autodj-dialog"));
    g_assert_cmpuint(fixture.changes, ==, 0);
    g_object_set_data(G_OBJECT(fixture.window), "radios-deck-owner-0", NULL); g_object_unref(card);
    fixture_clear(&fixture);
}

static void simple_routing_preserves_content_specific_choices(void)
{
    Fixture fixture; fixture_start(&fixture);
    JsonNode *config = radios_rotation_editor_snapshot(fixture.editor); g_assert_nonnull(config);
    JsonObject *source = json_array_get_object_element(json_object_get_array_member(json_node_get_object(config), "sources"), 0);
    JsonNode *mapping = parse("{\"music\":[1,3],\"advertisement\":[2],\"jingle\":[2],\"sweeper\":[4],\"custom_saved_value\":7}");
    json_object_set_member(source, "autodj", mapping);
    radios_rotation_editor_set_config(fixture.editor, json_node_get_object(config)); json_node_free(config);
    GtkTreePath *path = gtk_tree_path_new_from_indices(0, -1);
    gtk_tree_selection_select_path(gtk_tree_view_get_selection(GTK_TREE_VIEW(field(&fixture, "sources"))), path); gtk_tree_path_free(path);
    GtkWidget *cards[4];
    const gchar *names[] = {"Ana yayın", "Reklam", "Yedek yayın", "Jingle"};
    for (guint i = 0; i < G_N_ELEMENTS(cards); ++i) {
        cards[i] = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(cards[i]);
        g_object_set_data(G_OBJECT(cards[i]), "radios-player-name", (gpointer)names[i]);
        if (i >= 2) {
            g_object_set_data(G_OBJECT(cards[i]), "radios-player-cluster-id", "fixture-cluster");
            g_object_set_data(G_OBJECT(cards[i]), "radios-player-cluster-name", "Gece yayını");
        }
        gchar *key = g_strdup_printf("radios-deck-owner-%u", i);
        g_object_set_data(G_OBJECT(fixture.window), key, cards[i]); g_free(key);
    }
    radios_rotation_editor_open_schema_settings(fixture.editor);
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(fixture.editor, "schema-settings-step-target")), "66666666-6666-4666-8666-666666666666"));
    gtk_button_clicked(GTK_BUTTON(find(fixture.editor, "schema-settings-step-routing")));
    GtkWidget *routing = field(&fixture, "autodj-dialog");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(routing, "rotation-editor-autodj-working-mode"))), ==, "multi");
    g_assert_cmpuint(fixture.changes, ==, 0);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(routing, "rotation-editor-autodj-player-scope"))), ==, "saved-mixed");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(routing, "rotation-editor-autodj-content-kind")), "advertisement");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(routing, "rotation-editor-autodj-player-scope"))), ==, "independent");
    g_assert_false(gtk_widget_get_visible(find(routing, "rotation-editor-autodj-advertisement-player-3")));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(routing, "rotation-editor-autodj-working-mode"))), ==, "single");
    gtk_button_clicked(GTK_BUTTON(find(routing, "rotation-editor-autodj-apply")));
    JsonObject *saved = json_object_get_object_member(saved_source(&fixture), "autodj");
    g_assert_cmpint(json_object_get_int_member(saved, "custom_saved_value"), ==, 7);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(saved, "music")), ==, 2);
    g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(saved, "sweeper"), 0), ==, 4);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(routing, "rotation-editor-autodj-advertisement-player-1")), TRUE);
    gtk_button_clicked(GTK_BUTTON(find(routing, "rotation-editor-autodj-apply")));
    saved = json_object_get_object_member(saved_source(&fixture), "autodj");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(saved, "advertisement")), ==, 1);
    g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(saved, "advertisement"), 0), ==, 1);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(saved, "music")), ==, 2);
    guint before_move = fixture.changes;
    g_object_set_data(G_OBJECT(cards[0]), "radios-player-cluster-id", "moved-cluster");
    gtk_button_clicked(GTK_BUTTON(find(routing, "rotation-editor-autodj-apply")));
    g_assert_cmpuint(fixture.changes, ==, before_move);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(routing, "rotation-editor-autodj-status"))), "grubu değişti"));
    for (guint i = 0; i < G_N_ELEMENTS(cards); ++i) {
        gchar *key = g_strdup_printf("radios-deck-owner-%u", i);
        g_object_set_data(G_OBJECT(fixture.window), key, NULL); g_free(key); g_object_unref(cards[i]);
    }
    fixture_clear(&fixture);
}

static void saved_ack_preserves_inline_settings(void)
{
    Fixture fixture; fixture_start(&fixture);
    GtkWidget *card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(card);
    g_object_set_data(G_OBJECT(fixture.window), "radios-deck-owner-0", card);
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "source-probability")));
    GtkWidget *routing = field(&fixture, "autodj-dialog");
    JsonNode *snapshot = radios_rotation_editor_snapshot(fixture.editor); g_assert_nonnull(snapshot);
    radios_rotation_editor_set_config(fixture.editor, json_node_get_object(snapshot)); json_node_free(snapshot);
    g_assert_true(field(&fixture, "autodj-dialog") == routing);
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(field(&fixture, "pages"))), ==, "routing");
    g_assert_cmpint(gtk_notebook_get_current_page(GTK_NOTEBOOK(field(&fixture, "autodj-settings-tabs"))), ==, 1);
    /* A following edit also renders source tables while the inline page is
     * active; the selected source must remain editable after the ACK. */
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(field(&fixture, "source-weight")), 73);
    g_assert_cmpint(json_object_get_int_member(saved_source(&fixture), "weight"), ==, 73);
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(field(&fixture, "pages"))), ==, "routing");
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "probability")));
    GtkWidget *probability = field(&fixture, "probability-dialog");
    snapshot = radios_rotation_editor_snapshot(fixture.editor); g_assert_nonnull(snapshot);
    radios_rotation_editor_set_config(fixture.editor, json_node_get_object(snapshot));
    g_assert_true(field(&fixture, "probability-dialog") == probability);
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(field(&fixture, "pages"))), ==, "probability");
    json_object_set_string_member(json_node_get_object(snapshot), "id", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    radios_rotation_editor_set_config(fixture.editor, json_node_get_object(snapshot)); json_node_free(snapshot);
    g_assert_null(find(fixture.editor, "rotation-editor-probability-dialog"));
    g_assert_null(find(fixture.editor, "rotation-editor-autodj-dialog"));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(field(&fixture, "pages"))), ==, "library");
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "probability")));
    JsonNode *catalog = parse("[{\"_id\":\"77777777-7777-4777-8777-777777777777\",\"name\":\"Refreshed playlist\",\"purpose\":\"music\",\"tracks\":[]}]");
    radios_rotation_editor_set_playlists(fixture.editor, json_node_get_array(catalog)); json_node_free(catalog);
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(field(&fixture, "pages"))), ==, "probability");
    snapshot = radios_rotation_editor_snapshot(fixture.editor); g_assert_nonnull(snapshot);
    g_object_set_data(G_OBJECT(fixture.editor), "radios-data-explicit-reload", GINT_TO_POINTER(1));
    radios_rotation_editor_set_config(fixture.editor, json_node_get_object(snapshot)); json_node_free(snapshot);
    g_object_set_data(G_OBJECT(fixture.editor), "radios-data-explicit-reload", NULL);
    g_assert_null(find(fixture.editor, "rotation-editor-probability-dialog"));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(field(&fixture, "pages"))), ==, "library");
    g_object_set_data(G_OBJECT(fixture.window), "radios-deck-owner-0", NULL); g_object_unref(card);
    fixture_clear(&fixture);
}

static JsonObject *saved_settings(Fixture *fixture)
{
    g_assert_nonnull(fixture->saved);
    return json_object_get_object_member(json_node_get_object(fixture->saved), "settings");
}
/* The "AutoDJ otomasyonu" page: opening it saves nothing; enabling it stores
 * the template presets and the automation; assigning a preset to music is
 * persisted and greys out that kind's basic transition fields on the kind
 * page; renaming and a schedule row persist; an empty name is refused
 * without losing other settings; a saved snapshot renders back unchanged. */
static void automation_page_presets_assign_and_persist(void)
{
    Fixture fixture; fixture_start(&fixture);
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "autodj-automation-open")));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(field(&fixture, "pages"))), ==, "autodj-automation");
    g_assert_cmpuint(fixture.changes, ==, 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-enabled")), TRUE);
    g_assert_cmpuint(fixture.changes, ==, 1);
    JsonArray *presets = json_object_get_array_member(saved_settings(&fixture), "autodj_presets");
    g_assert_cmpuint(json_array_get_length(presets), ==, 8);
    gchar *first = g_strdup(json_object_get_string_member(json_array_get_object_element(presets, 0), "id"));
    g_assert_true(json_object_get_boolean_member(json_object_get_object_member(saved_settings(&fixture), "autodj_automation"), "enabled"));
    g_assert_true(gtk_widget_get_sensitive(field(&fixture, "autodj-music-mode")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-music-preset-0")), TRUE);
    JsonObject *music = json_object_get_object_member(json_object_get_object_member(json_object_get_object_member(saved_settings(&fixture), "autodj_automation"), "kinds"), "music");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(music, "presets")), ==, 1);
    g_assert_cmpstr(json_array_get_string_element(json_object_get_array_member(music, "presets"), 0), ==, first);
    g_assert_cmpstr(json_object_get_string_member(music, "rotation"), ==, "sequential");
    g_assert_false(gtk_widget_get_sensitive(field(&fixture, "autodj-music-mode")));
    g_assert_true(gtk_widget_get_sensitive(field(&fixture, "autodj-jingle-mode")));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(field(&fixture, "autodj-music-preset-note"))), ==, "Ön ayar kullanılıyor (AutoDJ otomasyonu)");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(field(&fixture, "automation-music-rotation")), "random");
    music = json_object_get_object_member(json_object_get_object_member(json_object_get_object_member(saved_settings(&fixture), "autodj_automation"), "kinds"), "music");
    g_assert_cmpstr(json_object_get_string_member(music, "rotation"), ==, "random");
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "automation-name")), "Sabah geçişi");
    presets = json_object_get_array_member(saved_settings(&fixture), "autodj_presets");
    g_assert_cmpstr(json_object_get_string_member(json_array_get_object_element(presets, 0), "name"), ==, "Sabah geçişi");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(field(&fixture, "automation-effect")), "echo");
    presets = json_object_get_array_member(saved_settings(&fixture), "autodj_presets");
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_array_get_object_element(presets, 0), "effect"), "type"), ==, "echo");
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "automation-schedule-add")));
    JsonArray *schedule = json_object_get_array_member(json_object_get_object_member(saved_settings(&fixture), "autodj_automation"), "schedule");
    g_assert_cmpuint(json_array_get_length(schedule), ==, 1);
    g_assert_cmpstr(json_array_get_string_element(json_object_get_array_member(json_array_get_object_element(schedule, 0), "presets"), 0), ==, first);
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "automation-schedule-end")), "06:00");
    schedule = json_object_get_array_member(json_object_get_object_member(saved_settings(&fixture), "autodj_automation"), "schedule");
    g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(schedule, 0), "end_minute"), ==, 360);
    guint changes = fixture.changes;
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "automation-name")), "");
    g_assert_cmpuint(fixture.changes, ==, changes); g_assert_false(radios_rotation_editor_ready(fixture.editor));
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "automation-name")), "Sabah geçişi");
    g_assert_true(radios_rotation_editor_ready(fixture.editor)); g_assert_cmpuint(fixture.changes, ==, changes + 1);
    JsonNode *snapshot = radios_rotation_editor_snapshot(fixture.editor); g_assert_nonnull(snapshot);
    radios_rotation_editor_set_config(fixture.editor, json_node_get_object(snapshot)); json_node_free(snapshot);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-enabled"))));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "automation-name"))), ==, "Sabah geçişi");
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-music-preset-0"))));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "automation-effect"))), ==, "echo");
    g_assert_false(gtk_widget_get_sensitive(field(&fixture, "autodj-music-mode")));
    g_free(first); fixture_clear(&fixture);
}

/* Normal form of the "AutoDJ otomasyonu" page: each control appears only
 * once an earlier choice needs it — effect parameters and the effect curve
 * with an effect, the tone curve with a filter, a kind's rotation with a
 * second preset, the schedule table and form with the first row and the
 * row's rotation with a second preset. The page itself never looks empty:
 * the presets stay shown and the switch only decides whether they act
 * (C8, 18.10: an unchecked switch had left the page blank). */
static gboolean nf_quit(gpointer loop) { g_main_loop_quit(loop); return G_SOURCE_REMOVE; }
static void nf_settle(guint ms)
{ GMainLoop *loop = g_main_loop_new(NULL, FALSE); g_timeout_add(ms, nf_quit, loop); g_main_loop_run(loop); g_main_loop_unref(loop); }
/* RADIOS_AUTOMATION_PAGE_CAPTURE=<prefix> saves <prefix>-<tag>.png of the
 * shown fixture window for a visual review; unset, nothing is shown. */
static void page_capture(Fixture *fixture, const gchar *tag)
{
    const gchar *prefix = g_getenv("RADIOS_AUTOMATION_PAGE_CAPTURE");
    if (!prefix || !*prefix) return;
    gtk_window_set_default_size(GTK_WINDOW(fixture->window), 980, 720);
    gtk_widget_show_all(fixture->window); nf_settle(400);
    GdkPixbuf *pixels = gdk_pixbuf_get_from_window(gtk_widget_get_window(fixture->window), 0, 0,
        gtk_widget_get_allocated_width(fixture->window), gtk_widget_get_allocated_height(fixture->window));
    g_assert_nonnull(pixels); GError *error = NULL; gchar *path = g_strdup_printf("%s-%s.png", prefix, tag);
    g_assert_true(gdk_pixbuf_save(pixels, path, "png", &error, NULL)); g_assert_no_error(error);
    g_free(path); g_object_unref(pixels);
}
static void automation_page_normal_form_visibility(void)
{
    Fixture fixture; fixture_start(&fixture);
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "autodj-automation-open")));
    GtkWidget *enabled = field(&fixture, "automation-enabled"), *body = field(&fixture, "automation-body");
    g_assert_true(gtk_widget_get_visible(body)); g_assert_cmpuint(fixture.changes, ==, 0);
    page_capture(&fixture, "off");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enabled), TRUE);
    g_assert_true(gtk_widget_get_visible(body));
    page_capture(&fixture, "on");
    g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-effect-params")));
    g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-curve-mix")));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(field(&fixture, "automation-effect")), "echo");
    g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-effect-params")));
    g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-curve-mix")));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(field(&fixture, "automation-effect")), "none");
    g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-effect-params")));
    g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-curve-mix")));
    g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-curve-tone")));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(field(&fixture, "automation-tone-out")), "lowpass");
    g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-curve-tone")));
    JsonArray *presets = json_object_get_array_member(saved_settings(&fixture), "autodj_presets");
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(json_array_get_object_element(presets, 0), "out"), "tone_kind"), ==, "lowpass");
    g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-music-rotation")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-music-preset-0")), TRUE);
    g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-music-rotation")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-music-preset-1")), TRUE);
    g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-music-rotation")));
    JsonObject *music = json_object_get_object_member(json_object_get_object_member(json_object_get_object_member(saved_settings(&fixture), "autodj_automation"), "kinds"), "music");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(music, "presets")), ==, 2);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-music-preset-1")), FALSE);
    g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-music-rotation")));
    g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-schedule-box")));
    g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-schedule-form")));
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "automation-schedule-add")));
    g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-schedule-box")));
    g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-schedule-form")));
    g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-schedule-rotation-box")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-schedule-preset-1")), TRUE);
    g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-schedule-rotation-box")));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(field(&fixture, "automation-effect")), "echo");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-music-preset-1")), TRUE);
    page_capture(&fixture, "full");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(field(&fixture, "automation-effect")), "none");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-music-preset-1")), FALSE);
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "automation-schedule-remove")));
    g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-schedule-box")));
    JsonArray *schedule = json_object_get_array_member(json_object_get_object_member(saved_settings(&fixture), "autodj_automation"), "schedule");
    g_assert_cmpuint(json_array_get_length(schedule), ==, 0);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(enabled), FALSE);
    g_assert_true(gtk_widget_get_visible(body));
    g_assert_false(json_object_get_boolean_member(json_object_get_object_member(saved_settings(&fixture), "autodj_automation"), "enabled"));
    fixture_clear(&fixture);
}

/* The transition type comes first on the "AutoDJ otomasyonu" page and is the
 * preset's structure: a crossfade shows length, SYNC and the volume/bass
 * curves only; the effect type opens the effect row (an audible default, its
 * parameters and curve); the filter type opens the filter row and the tone
 * curve; both open both. "Efekt yok" and "Filtre yok" leave their type, so
 * nothing stays active while hidden; every template chip arrives with its type. */
static void automation_page_type_drives_fields(void)
{
    Fixture fixture; fixture_start(&fixture);
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "autodj-automation-open")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-enabled")), TRUE);
    GtkWidget *type = field(&fixture, "automation-type"), *effect_row = field(&fixture, "automation-effect-row"), *filter_row = field(&fixture, "automation-filter-row");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(type)), ==, "crossfade");
    g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-duration"))); g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-beat-sync")));
    g_assert_false(gtk_widget_get_visible(effect_row)); g_assert_false(gtk_widget_get_visible(filter_row));
    g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-effect-params")));
    g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-curve-mix"))); g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-curve-tone")));
    g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-curve-volume"))); g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-curve-bass")));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(type), "effect");
    g_assert_true(gtk_widget_get_visible(effect_row)); g_assert_false(gtk_widget_get_visible(filter_row));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "automation-effect"))), ==, "echo");
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(field(&fixture, "automation-effect-depth"))), ==, 50);
    g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-effect-params"))); g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-curve-mix")));
    JsonArray *presets = json_object_get_array_member(saved_settings(&fixture), "autodj_presets");
    JsonObject *preset = json_array_get_object_element(presets, 0);
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(preset, "effect"), "type"), ==, "echo");
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(preset, "effect"), "depth"), ==, 50);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(type), "filter");
    g_assert_false(gtk_widget_get_visible(effect_row)); g_assert_true(gtk_widget_get_visible(filter_row));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "automation-tone-out"))), ==, "lowpass");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "automation-tone-in"))), ==, "none");
    g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-curve-tone"))); g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-curve-mix")));
    g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-effect-params")));
    presets = json_object_get_array_member(saved_settings(&fixture), "autodj_presets"); preset = json_array_get_object_element(presets, 0);
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(preset, "effect"), "type"), ==, "none");
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(preset, "out"), "tone_kind"), ==, "lowpass");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(type), "effect_filter");
    g_assert_true(gtk_widget_get_visible(effect_row)); g_assert_true(gtk_widget_get_visible(filter_row));
    g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-curve-mix"))); g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-curve-tone")));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(field(&fixture, "automation-effect")), "none");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(type)), ==, "filter");
    g_assert_false(gtk_widget_get_visible(effect_row)); g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-effect-params")));
    g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-curve-mix")));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(field(&fixture, "automation-tone-out")), "none");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(type)), ==, "crossfade");
    g_assert_false(gtk_widget_get_visible(filter_row)); g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-curve-tone")));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(type), "effect_filter"); gtk_combo_box_set_active_id(GTK_COMBO_BOX(type), "crossfade");
    presets = json_object_get_array_member(saved_settings(&fixture), "autodj_presets"); preset = json_array_get_object_element(presets, 0);
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(preset, "effect"), "type"), ==, "none");
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(preset, "out"), "tone_kind"), ==, "none");
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(preset, "in"), "tone_kind"), ==, "none");
    g_assert_false(gtk_widget_get_visible(effect_row)); g_assert_false(gtk_widget_get_visible(filter_row));
    g_assert_true(radios_rotation_editor_ready(fixture.editor));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-chip-5")), TRUE);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "automation-name"))), ==, "Yankı ile çık");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(type)), ==, "effect");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "automation-effect"))), ==, "echo");
    g_assert_true(gtk_widget_get_visible(effect_row)); g_assert_false(gtk_widget_get_visible(filter_row));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-beat-sync"))));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-chip-2")), TRUE);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "automation-name"))), ==, "Ton erit");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(type)), ==, "filter");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "automation-tone-out"))), ==, "lowpass");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "automation-tone-in"))), ==, "highpass");
    g_assert_false(gtk_widget_get_visible(effect_row)); g_assert_true(gtk_widget_get_visible(filter_row));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-chip-7")), TRUE);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "automation-name"))), ==, "Kes");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(type)), ==, "crossfade");
    g_assert_false(gtk_widget_get_visible(effect_row)); g_assert_false(gtk_widget_get_visible(filter_row));
    fixture_clear(&fixture);
}

/* A designed preset round-trips through the saved rotation with its
 * structure only (no type member joins the wire contract): both rows, the
 * effect, its depth, the two filters and SYNC come back exactly, and a plain
 * template renders as a crossfade with closed rows. */
static void automation_page_type_round_trip(void)
{
    Fixture fixture; fixture_start(&fixture);
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "autodj-automation-open")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-enabled")), TRUE);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(field(&fixture, "automation-type")), "effect_filter");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(field(&fixture, "automation-effect")), "reverb");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(field(&fixture, "automation-effect-depth")), 65);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(field(&fixture, "automation-tone-in")), "highpass");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-beat-sync")), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-music-preset-0")), TRUE);
    JsonArray *presets = json_object_get_array_member(saved_settings(&fixture), "autodj_presets");
    JsonObject *preset = json_array_get_object_element(presets, 0);
    g_assert_false(json_object_has_member(preset, "transition_type")); g_assert_false(json_object_has_member(preset, "type"));
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(preset, "effect"), "type"), ==, "reverb");
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(preset, "effect"), "depth"), ==, 65);
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(preset, "out"), "tone_kind"), ==, "lowpass");
    g_assert_cmpstr(json_object_get_string_member(json_object_get_object_member(preset, "in"), "tone_kind"), ==, "highpass");
    g_assert_true(json_object_get_boolean_member(preset, "beat_sync"));
    g_assert_true(radios_rotation_editor_ready(fixture.editor));
    JsonNode *snapshot = radios_rotation_editor_snapshot(fixture.editor); g_assert_nonnull(snapshot);
    radios_rotation_editor_set_config(fixture.editor, json_node_get_object(snapshot)); json_node_free(snapshot);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "automation-type"))), ==, "effect_filter");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "automation-effect"))), ==, "reverb");
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(field(&fixture, "automation-effect-depth"))), ==, 65);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "automation-tone-out"))), ==, "lowpass");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "automation-tone-in"))), ==, "highpass");
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-beat-sync"))));
    g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-effect-row"))); g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-filter-row")));
    g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-curve-mix"))); g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-curve-tone")));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-music-preset-0"))));
    g_assert_true(radios_rotation_editor_ready(fixture.editor));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-chip-1")), TRUE);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(field(&fixture, "automation-name"))), ==, "Bas dalgalı geçiş");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(&fixture, "automation-type"))), ==, "crossfade");
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-beat-sync"))));
    g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-effect-row"))); g_assert_false(gtk_widget_get_visible(field(&fixture, "automation-filter-row")));
    fixture_clear(&fixture);
}

/* "Türler" opens the probability window ("Olasılık ve geçişler…") and its
 * back button returns here. A kind's designed presets open from its row;
 * once "Tasarlanmış geçişler" is on a chosen preset replaces the kind's rule
 * fields in the same row (C8, 18.10; replaces the read-only rule text). */
static void automation_page_kind_rule_and_links(void)
{
    Fixture fixture; fixture_start(&fixture);
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "autodj-automation-open")));
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "automation-probability-open")));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(field(&fixture, "pages"))), ==, "probability");
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "probability-back")));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(field(&fixture, "pages"))), ==, "autodj-automation");
    g_assert_true(gtk_widget_get_visible(field(&fixture, "automation-music-presets-open")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-enabled")), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-music-preset-0")), TRUE);
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(field(&fixture, "automation-music-presets-open"))), ==, "1 ön ayar seçili ▾");
    g_assert_false(gtk_widget_get_sensitive(field(&fixture, "autodj-music-mode")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-music-preset-0")), FALSE);
    g_assert_true(gtk_widget_get_sensitive(field(&fixture, "autodj-music-mode")));
    fixture_clear(&fixture);
}

/* Bulgu (C8, gerçek katalogla): with no playlist row selected — the usual
 * state when the editor opens — an edit on "Olasılık ve geçişler", "AutoDJ:
 * geçiş ve kanallar" or "AutoDJ otomasyonu" is saved and the page stays open
 * instead of falling back to the playlist list. */
static void unselected_edits_stay_on_page(void)
{
    Fixture fixture; fixture_start(&fixture);
    /* Opening another definition leaves no playlist row selected, as on open. */
    JsonNode *snapshot = radios_rotation_editor_snapshot(fixture.editor); g_assert_nonnull(snapshot);
    json_object_set_string_member(json_node_get_object(snapshot), "id", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    radios_rotation_editor_set_config(fixture.editor, json_node_get_object(snapshot)); json_node_free(snapshot);
    GtkTreeModel *model = NULL;
    g_assert_false(gtk_tree_selection_get_selected(gtk_tree_view_get_selection(GTK_TREE_VIEW(field(&fixture, "sources"))), &model, NULL));
    GtkStack *pages = GTK_STACK(field(&fixture, "pages"));
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "probability")));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(pages), ==, "probability");
    guint changes = fixture.changes;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(field(&fixture, "transition-weight-1")), 40);
    g_assert_cmpuint(fixture.changes, >, changes);
    g_assert_cmpstr(gtk_stack_get_visible_child_name(pages), ==, "probability");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(field(&fixture, "transition-weight-0")), 60);
    g_assert_cmpstr(gtk_stack_get_visible_child_name(pages), ==, "probability");
    JsonArray *choices = json_object_get_array_member(saved_settings(&fixture), "transitions");
    g_assert_cmpuint(json_array_get_length(choices), ==, 2);
    /* A probability rule edit re-renders the step tables too. */
    GtkWidget *add = find(field(&fixture, "probability-dialog"), "probability-editor-transition-add"); g_assert_true(GTK_IS_BUTTON(add));
    changes = fixture.changes; gtk_button_clicked(GTK_BUTTON(add));
    g_assert_cmpuint(fixture.changes, >, changes);
    g_assert_true(json_object_has_member(saved_settings(&fixture), "probability_policy"));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(pages), ==, "probability");
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "probability-back")));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(pages), ==, "library");
    /* "AutoDJ: geçiş ve kanallar…" opens the merged AutoDJ page (C8, 18.10). */
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "autodj-policy-open")));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(pages), ==, "autodj-automation");
    changes = fixture.changes;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "autodj-policy-enabled")), TRUE);
    g_assert_cmpuint(fixture.changes, >, changes);
    g_assert_cmpstr(gtk_stack_get_visible_child_name(pages), ==, "autodj-automation");
    changes = fixture.changes;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(field(&fixture, "autodj-music-overlap")), 9200);
    g_assert_cmpuint(fixture.changes, >, changes);
    g_assert_cmpstr(gtk_stack_get_visible_child_name(pages), ==, "autodj-automation");
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "autodj-automation-back")));
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "autodj-automation-open")));
    g_assert_cmpstr(gtk_stack_get_visible_child_name(pages), ==, "autodj-automation");
    changes = fixture.changes;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-enabled")), TRUE);
    g_assert_cmpuint(fixture.changes, >, changes);
    g_assert_cmpstr(gtk_stack_get_visible_child_name(pages), ==, "autodj-automation");
    /* The song page still needs a playlist row: it falls back to the list. */
    gtk_stack_set_visible_child_name(pages, "songs");
    JsonNode *catalog = parse("[{\"_id\":\"77777777-7777-4777-8777-777777777777\",\"name\":\"Test playlist\",\"purpose\":\"music\",\"tracks\":[]}]");
    radios_rotation_editor_set_playlists(fixture.editor, json_node_get_array(catalog)); json_node_free(catalog);
    g_assert_cmpstr(gtk_stack_get_visible_child_name(pages), ==, "library");
    fixture_clear(&fixture);
}

static void high_player_mappings_survive_policy_edit(void)
{
    Fixture fixture; fixture_start(&fixture);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "autodj-policy-enabled")), TRUE);
    JsonNode *config = radios_rotation_editor_snapshot(fixture.editor); g_assert_nonnull(config);
    JsonObject *settings = json_object_get_object_member(json_node_get_object(config), "settings");
    JsonObject *mapping = json_object_get_object_member(settings, "autodj_players");
    const gchar *kinds[] = {"music", "advertisement", "jingle", "sweeper"};
    for (guint k = 0; k < G_N_ELEMENTS(kinds); ++k) {
        JsonArray *decks = json_array_new(); json_array_add_int_element(decks, 32); json_array_add_int_element(decks, 12);
        json_object_set_array_member(mapping, kinds[k], decks);
    }
    radios_rotation_editor_set_config(fixture.editor, json_node_get_object(config)); json_node_free(config);
    g_assert_true(radios_rotation_editor_ready(fixture.editor));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(field(&fixture, "autodj-music-overlap")), 9200);
    mapping = json_object_get_object_member(saved_settings(&fixture), "autodj_players");
    for (guint k = 0; k < G_N_ELEMENTS(kinds); ++k) {
        JsonArray *decks = json_object_get_array_member(mapping, kinds[k]);
        g_assert_cmpuint(json_array_get_length(decks), ==, 2);
        g_assert_cmpint(json_array_get_int_element(decks, 0), ==, 32); g_assert_cmpint(json_array_get_int_element(decks, 1), ==, 12);
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "autodj-music-player-12")), FALSE);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_object_get_object_member(saved_settings(&fixture), "autodj_players"), "music")), ==, 1);
    fixture_clear(&fixture);
}
static void live_source_draft_and_dates_round_trip(void)
{
    Fixture fixture; fixture_start(&fixture); guint changes = fixture.changes;
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "add-live")));
    GtkWidget *dialog = dialog_find("rotation-editor-step-dialog"); g_assert_nonnull(dialog);
    gtk_entry_set_text(GTK_ENTRY(find(dialog, "rotation-editor-step-title")), "   ");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    g_assert_cmpuint(fixture.changes, ==, changes); g_assert_nonnull(dialog_find("rotation-editor-step-dialog"));
    gtk_entry_set_text(GTK_ENTRY(find(dialog, "rotation-editor-step-title")), "Konuşma arası");
    GtkWidget *picker = find(dialog, "rotation-editor-step-channels");
    radios_ui_channel_picker_toggle(picker, 3); radios_ui_channel_picker_toggle(picker, 32);
    gtk_entry_set_text(GTK_ENTRY(find(dialog, "rotation-editor-step-start-date")), "2026-10-18");
    gtk_entry_set_text(GTK_ENTRY(find(dialog, "rotation-editor-step-end-date")), "2026-10-17");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); g_assert_cmpuint(fixture.changes, ==, changes);
    gtk_entry_set_text(GTK_ENTRY(find(dialog, "rotation-editor-step-end-date")), "2026-10-25");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(dialog, "rotation-editor-step-end-rule")), "duration");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(find(dialog, "rotation-editor-step-duration")), 45);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); g_assert_null(dialog_find("rotation-editor-step-dialog"));
    JsonArray *sources = json_object_get_array_member(json_node_get_object(fixture.saved), "sources");
    g_assert_cmpuint(json_array_get_length(sources), ==, 2);
    JsonObject *source = json_array_get_object_element(sources, 1), *live = json_object_get_object_member(source, "live");
    g_assert_cmpstr(json_object_get_string_member(source, "source_type"), ==, "live"); g_assert_false(json_object_has_member(source, "playlist_id"));
    g_assert_cmpstr(json_object_get_string_member(live, "title"), ==, "Konuşma arası");
    g_assert_cmpint(json_object_get_int_member(live, "duration_ms"), ==, 45000);
    g_assert_cmpint(json_array_get_int_element(json_object_get_array_member(live, "channels"), 1), ==, 32);
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "step-edit"))); dialog = dialog_find("rotation-editor-step-dialog"); g_assert_nonnull(dialog);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(dialog, "rotation-editor-step-title"))), ==, "Konuşma arası");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    radios_rotation_editor_open_schema_settings(fixture.editor);
    g_assert_cmpstr(gtk_stack_get_visible_child_name(GTK_STACK(field(&fixture, "pages"))), ==, "schema-settings");
    g_assert_nonnull(find(fixture.editor, "schema-settings-step-target"));
    fixture_clear(&fixture);
}
/* Kılavuz s.4 adım 5 / s.7: the label of the strips typed into the live
 * step's channels sets its Tür (what L does); unlabelled or mixed strips
 * leave the operator's choice, and opening a saved step never moves it. */
static void live_step_label_follows_mixer_channels(void)
{
    Fixture fixture; fixture_start(&fixture);
    strip_labels[3] = strip_labels[4] = "dj"; strip_labels[6] = "music";
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "add-live")));
    GtkWidget *dialog = dialog_find("rotation-editor-step-dialog"); g_assert_nonnull(dialog);
    GtkWidget *label = find(dialog, "rotation-editor-step-label"), *channels = find(dialog, "rotation-editor-step-channels");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(label)), ==, "talk");
    radios_ui_channel_picker_toggle(channels, 3); g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(label)), ==, "dj");
    radios_ui_channel_picker_toggle(channels, 4); radios_ui_channel_picker_toggle(channels, 6);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(label)), ==, "dj");
    radios_ui_channel_picker_toggle(channels, 3); radios_ui_channel_picker_toggle(channels, 4);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(label)), ==, "music");
    radios_ui_channel_picker_toggle(channels, 1); g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(label)), ==, "music");
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(label), "talk"));
    gtk_entry_set_text(GTK_ENTRY(find(dialog, "rotation-editor-step-title")), "Etiketli adım");
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); g_assert_null(dialog_find("rotation-editor-step-dialog"));
    JsonArray *sources = json_object_get_array_member(json_node_get_object(fixture.saved), "sources");
    JsonObject *live = json_object_get_object_member(json_array_get_object_element(sources, json_array_get_length(sources) - 1), "live");
    g_assert_cmpstr(json_object_get_string_member(live, "label"), ==, "talk");
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "step-edit"))); dialog = dialog_find("rotation-editor-step-dialog"); g_assert_nonnull(dialog);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(dialog, "rotation-editor-step-label"))), ==, "talk");
    JsonArray *reopened = radios_ui_channel_picker_selected(find(dialog, "rotation-editor-step-channels"));
    g_assert_cmpuint(json_array_get_length(reopened), ==, 2);
    g_assert_cmpint(json_array_get_int_element(reopened, 0), ==, 1); g_assert_cmpint(json_array_get_int_element(reopened, 1), ==, 6); json_array_unref(reopened);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
    strip_labels[3] = strip_labels[4] = strip_labels[6] = NULL;
    fixture_clear(&fixture);
}
static void source_changes_keep_schema_references_consistent(void)
{
    Fixture fixture; fixture_start(&fixture);
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "add")));
    JsonNode *config = radios_rotation_editor_snapshot(fixture.editor); g_assert_nonnull(config);
    JsonObject *definition = json_node_get_object(config), *settings = json_object_get_object_member(definition, "settings");
    JsonArray *sources = json_object_get_array_member(definition, "sources"); g_assert_cmpuint(json_array_get_length(sources), ==, 2);
    const gchar *a = json_object_get_string_member(json_array_get_object_element(sources, 0), "id"), *b = json_object_get_string_member(json_array_get_object_element(sources, 1), "id");
    JsonArray *links = json_array_new(); JsonObject *link = json_object_new();
    json_object_set_string_member(link, "from_source_id", a); json_object_set_string_member(link, "to_source_id", b);
    json_array_add_object_element(links, link); json_object_set_array_member(settings, "link_transitions", links);
    JsonObject *entry = json_object_new(); json_object_set_int_member(entry, "source_order", 2);
    JsonArray *alternatives = json_array_new(); JsonObject *alternative = json_object_new();
    json_object_set_int_member(alternative, "weight", 25); json_object_set_int_member(alternative, "source_order", 1);
    json_array_add_object_element(alternatives, alternative); json_object_set_array_member(entry, "alternatives", alternatives);
    json_object_set_object_member(settings, "programme_entry", entry);
    radios_rotation_editor_set_config(fixture.editor, definition); json_node_free(config);
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "up")));
    entry = json_object_get_object_member(saved_settings(&fixture), "programme_entry");
    g_assert_cmpint(json_object_get_int_member(entry, "source_order"), ==, 1);
    g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(json_object_get_array_member(entry, "alternatives"), 0), "source_order"), ==, 2);
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "remove")));
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(saved_settings(&fixture), "link_transitions")), ==, 0);
    entry = json_object_get_object_member(saved_settings(&fixture), "programme_entry");
    g_assert_cmpint(json_object_get_int_member(entry, "source_order"), ==, 1);
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "remove")));
    g_assert_false(json_object_has_member(saved_settings(&fixture), "programme_entry"));
    fixture_clear(&fixture);
}
static gboolean choose_schema_source(gpointer data)
{
    (void)data; GtkWidget *dialog = dialog_find("rotation-editor-add-dialog"); g_assert_nonnull(dialog);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_YES); return G_SOURCE_REMOVE;
}
static void schema_source_load_preserves_identity_and_isolation(void)
{
    Fixture fixture; fixture_start(&fixture);
    JsonNode *schema = radios_rotation_editor_snapshot(fixture.editor); g_assert_nonnull(schema);
    JsonObject *definition = json_node_get_object(schema), *settings = json_object_get_object_member(definition, "settings");
    json_object_set_string_member(definition, "id", "aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa");
    json_object_set_string_member(definition, "name", "Kayıtlı yayın şeması"); json_object_set_boolean_member(settings, "automation_schema", TRUE);
    JsonObject *source = json_array_get_object_element(json_object_get_array_member(definition, "sources"), 0);
    json_object_set_string_member(source, "start_date", "2026-10-18"); json_object_set_string_member(source, "end_date", "2026-10-25");
    JsonArray *steps = json_array_new(); JsonObject *step = json_object_new();
    json_object_set_int_member(step, "order", 2); json_object_set_string_member(step, "kind", "music"); json_array_add_object_element(steps, step);
    json_object_set_array_member(settings, "automation_steps", steps);
    JsonObject *entry = json_object_new(); json_object_set_int_member(entry, "source_order", 1); json_object_set_object_member(settings, "programme_entry", entry);
    JsonArray *library = json_array_new(); json_array_add_element(library, json_node_copy(schema));
    radios_rotation_editor_set_schemas(fixture.editor, library); json_array_unref(library);
    g_idle_add(choose_schema_source, NULL); gtk_button_clicked(GTK_BUTTON(field(&fixture, "add-choose")));
    GtkWidget *dialog = dialog_find("rotation-editor-schema-load-dialog"); g_assert_nonnull(dialog);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); g_assert_cmpuint(fixture.changes, ==, 0);
    g_assert_nonnull(dialog_find("rotation-editor-schema-load-dialog"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(dialog, "rotation-editor-schema-load-replace")), TRUE);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); g_assert_cmpuint(fixture.changes, ==, 1);
    g_assert_null(dialog_find("rotation-editor-schema-load-dialog"));
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(fixture.saved), "id"), ==, "55555555-5555-4555-8555-555555555555");
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(fixture.saved), "name"), ==, "Test rotation");
    g_assert_cmpstr(json_object_get_string_member(saved_source(&fixture), "start_date"), ==, "2026-10-18");
    g_assert_cmpint(json_object_get_int_member(json_array_get_object_element(json_object_get_array_member(saved_settings(&fixture), "automation_steps"), 0), "order"), ==, 2);
    json_object_set_string_member(source, "start_date", "2030-01-01");
    g_assert_cmpstr(json_object_get_string_member(saved_source(&fixture), "start_date"), ==, "2026-10-18");
    json_node_free(schema);
    /* A late load cannot overwrite a newer draft. */
    g_idle_add(choose_schema_source, NULL); gtk_button_clicked(GTK_BUTTON(field(&fixture, "add-choose")));
    dialog = dialog_find("rotation-editor-schema-load-dialog"); g_assert_nonnull(dialog);
    gtk_entry_set_text(GTK_ENTRY(field(&fixture, "name")), "Yeni taslak"); guint changes = fixture.changes;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(dialog, "rotation-editor-schema-load-replace")), TRUE);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT); g_assert_cmpuint(fixture.changes, ==, changes);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(find(dialog, "rotation-editor-schema-load-status"))), "değişti"));
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); fixture_clear(&fixture);
}
static void designed_presets_hide_redundant_fallback_fields(void)
{
    Fixture fixture; fixture_start(&fixture);
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "autodj-automation-open")));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-enabled")), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-music-preset-0")), TRUE);
    const gchar *ids[] = {"autodj-music-mode", "autodj-music-overlap", "autodj-music-sync-percent"};
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) g_assert_false(gtk_widget_get_visible(field(&fixture, ids[i])));
    gtk_widget_show_all(fixture.editor);
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) g_assert_false(gtk_widget_get_visible(field(&fixture, ids[i])));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-music-preset-0")), FALSE);
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) g_assert_true(gtk_widget_get_visible(field(&fixture, ids[i])));
    fixture_clear(&fixture);
}

static void assigned_automation_cannot_be_deleted(void)
{
    Fixture fixture; fixture_start(&fixture);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-enabled")), TRUE);
    JsonNode *config = radios_rotation_editor_snapshot(fixture.editor);
    JsonObject *settings = json_object_get_object_member(json_node_get_object(config), "settings");
    JsonArray *presets = json_object_get_array_member(settings, "autodj_presets"), *steps = json_array_new();
    JsonObject *step = json_object_new(); json_object_set_int_member(step, "order", 2); json_object_set_string_member(step, "kind", "music");
    json_object_set_string_member(step, "preset_id", json_object_get_string_member(json_array_get_object_element(presets, 0), "id"));
    json_array_add_object_element(steps, step); json_object_set_array_member(settings, "automation_steps", steps);
    radios_rotation_editor_set_config(fixture.editor, json_node_get_object(config)); json_node_free(config);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "automation-chip-0")), TRUE);
    guint changes = fixture.changes;
    gtk_button_clicked(GTK_BUTTON(field(&fixture, "automation-delete")));
    g_assert_cmpuint(fixture.changes, ==, changes);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(field(&fixture, "status"))), "şemada kullanılıyor"));
    config = radios_rotation_editor_snapshot(fixture.editor); g_assert_nonnull(config);
    settings = json_object_get_object_member(json_node_get_object(config), "settings");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(settings, "autodj_presets")), ==, 8);
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(settings, "automation_steps")), ==, 1);
    json_node_free(config); fixture_clear(&fixture);
}
static void new_policy_uses_available_cluster_members(void)
{
    Fixture fixture; fixture_start(&fixture);
    GtkWidget *cards[2]; const guint decks[] = {11, 31};
    for (guint i = 0; i < 2; ++i) {
        cards[i] = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(cards[i]);
        g_object_set_data(G_OBJECT(cards[i]), "radios-player-cluster-id", "test-cluster");
        g_object_set_data(G_OBJECT(cards[i]), "radios-player-cluster-name", "Canlı yayın");
        gchar *key = g_strdup_printf("radios-deck-owner-%u", decks[i]); g_object_set_data(G_OBJECT(fixture.window), key, cards[i]); g_free(key);
    }
    JsonNode *config = radios_rotation_editor_snapshot(fixture.editor);
    radios_rotation_editor_set_config(fixture.editor, json_node_get_object(config)); json_node_free(config);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "autodj-policy-enabled")), TRUE);
    JsonArray *pool = json_object_get_array_member(json_object_get_object_member(saved_settings(&fixture), "autodj_players"), "music");
    g_assert_cmpuint(json_array_get_length(pool), ==, 2);
    g_assert_cmpint(json_array_get_int_element(pool, 0), ==, 12); g_assert_cmpint(json_array_get_int_element(pool, 1), ==, 32);
    for (guint i = 0; i < 2; ++i) {
        gchar *key = g_strdup_printf("radios-deck-owner-%u", decks[i]); g_object_set_data(G_OBJECT(fixture.window), key, NULL); g_free(key); g_object_unref(cards[i]);
    }
    fixture_clear(&fixture);
}

/* Kılavuz §02 (akım 4c): "Playlist ekle" is now "Playlist ya da live event
 * ekle"; a live event and a schema's definitions are added from the same place. */
static void kilavuz_add_names_playlist_or_live_event(void)
{
    Fixture fixture; fixture_start(&fixture);
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(field(&fixture, "add-choose"))), ==, "Playlist ya da live event ekle…");
    g_assert_true(GTK_IS_BUTTON(field(&fixture, "add-live")));
    fixture_clear(&fixture);
}
/* Kılavuz §03 "Kümeli ve kümesiz seçim" (akım 4c): the AutoDJ player drop-down
 * lists clustered players under their cluster (whose own check selects the
 * whole cluster), independent players one by one under "Kümesizler", and a
 * saved but absent player stays listed while selected. The saved format is
 * unchanged: autodj_players.<kind> = [1..32]. */
static void kilavuz_players_grouped_by_cluster(void)
{
    Fixture fixture; fixture_start(&fixture);
    struct { guint deck; const gchar *cluster, *cluster_name, *name; } players[] = {
        {0, "c-a", "Küme A", "Stüdyo 1"}, {1, "c-a", "Küme A", "Stüdyo 2"}, {2, "c-b", "Küme B", "Reklam 1"}, {3, "", "", "Tek oynatıcı"}};
    GtkWidget *cards[G_N_ELEMENTS(players)];
    for (guint i = 0; i < G_N_ELEMENTS(players); ++i) {
        cards[i] = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(cards[i]);
        g_object_set_data(G_OBJECT(cards[i]), "radios-player-cluster-id", (gpointer)players[i].cluster);
        g_object_set_data(G_OBJECT(cards[i]), "radios-player-cluster-name", (gpointer)players[i].cluster_name);
        g_object_set_data(G_OBJECT(cards[i]), "radios-player-name", (gpointer)players[i].name);
        gchar *key = g_strdup_printf("radios-deck-owner-%u", players[i].deck); g_object_set_data(G_OBJECT(fixture.window), key, cards[i]); g_free(key);
    }
    JsonNode *config = radios_rotation_editor_snapshot(fixture.editor);
    JsonObject *settings = json_object_get_object_member(json_node_get_object(config), "settings"), *policy = json_object_new(), *pools = json_object_new();
    const gchar *kinds[] = {"music", "advertisement", "jingle", "sweeper"};
    for (guint k = 0; k < G_N_ELEMENTS(kinds); ++k) {
        JsonObject *choice = json_object_new(); json_object_set_int_member(choice, "mode", k ? 0 : 1); json_object_set_int_member(choice, "overlap_ms", k ? 0 : 3000);
        json_object_set_object_member(policy, kinds[k], choice);
        JsonArray *decks = json_array_new();
        if (k) json_array_add_int_element(decks, 4); else { json_array_add_int_element(decks, 1); json_array_add_int_element(decks, 2); json_array_add_int_element(decks, 10); }
        json_object_set_array_member(pools, kinds[k], decks);
    }
    json_object_set_object_member(settings, "kind_transitions", policy); json_object_set_object_member(settings, "autodj_players", pools);
    radios_rotation_editor_set_config(fixture.editor, json_node_get_object(config)); json_node_free(config);
    GtkWidget *member = field(&fixture, "autodj-music-player-1"), *list = gtk_widget_get_parent(member);
    GList *children = gtk_container_get_children(GTK_CONTAINER(list)); GPtrArray *order = g_ptr_array_new();
    for (GList *it = children; it; it = it->next) if (gtk_widget_get_visible(it->data)) g_ptr_array_add(order, it->data);
    g_list_free(children);
    const gchar *expected[] = {"Küme A", "Stüdyo 1", "Stüdyo 2", "Küme B", "Reklam 1", "Kümesizler", "Tek oynatıcı", "Kayıtlı · şu an kapalı", "Oynatıcı 10"};
    g_assert_cmpuint(order->len, ==, G_N_ELEMENTS(expected));
    for (guint i = 0; i < order->len; ++i) {
        GtkWidget *row = g_ptr_array_index(order, i);
        const gchar *text = GTK_IS_BUTTON(row) ? gtk_button_get_label(GTK_BUTTON(row)) : gtk_label_get_text(GTK_LABEL(row));
        g_assert_cmpstr(text, ==, expected[i]);
    }
    g_ptr_array_unref(order);
    GtkWidget *cluster_a = field(&fixture, "autodj-music-cluster-0"), *cluster_b = field(&fixture, "autodj-music-cluster-1");
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cluster_a))); g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(cluster_b)));
    g_assert_false(gtk_widget_get_visible(field(&fixture, "autodj-music-player-11")));
    /* The cluster's own check selects all its members in one change. */
    guint changes = fixture.changes; gtk_button_clicked(GTK_BUTTON(cluster_b));
    g_assert_cmpuint(fixture.changes, ==, changes + 1);
    JsonArray *music = json_object_get_array_member(json_object_get_object_member(saved_settings(&fixture), "autodj_players"), "music");
    gboolean three = FALSE, ten = FALSE;
    for (guint i = 0; i < json_array_get_length(music); ++i) { three |= json_array_get_int_element(music, i) == 3; ten |= json_array_get_int_element(music, i) == 10; }
    g_assert_true(three); g_assert_true(ten);
    /* A partly selected cluster shows it; its check then selects the rest. */
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(field(&fixture, "autodj-music-player-2")), FALSE);
    cluster_a = field(&fixture, "autodj-music-cluster-0");
    g_assert_true(gtk_toggle_button_get_inconsistent(GTK_TOGGLE_BUTTON(cluster_a)));
    gtk_button_clicked(GTK_BUTTON(cluster_a));
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(field(&fixture, "autodj-music-player-2"))));
    g_assert_false(gtk_toggle_button_get_inconsistent(GTK_TOGGLE_BUTTON(field(&fixture, "autodj-music-cluster-0"))));
    for (guint i = 0; i < G_N_ELEMENTS(players); ++i) {
        gchar *key = g_strdup_printf("radios-deck-owner-%u", players[i].deck); g_object_set_data(G_OBJECT(fixture.window), key, NULL); g_free(key); g_object_unref(cards[i]);
    }
    fixture_clear(&fixture);
}
/* C8 · "Geçiş dağılımı" bileşeni (kılavuz 18.10 sözleşmesi, settings.transitions). */
static void distribution_changed(GtkWidget *widget, gpointer user) { (void)widget; ++*(guint *)user; }
static JsonObject *entry_at(JsonObject *settings, guint index)
{ return json_array_get_object_element(json_object_get_array_member(settings, "transitions"), index); }
/* The base table keeps the earlier grid's names, reads only entries without
 * "kind", writes them first and leaves kind entries untouched. */
static void transition_distribution_base_round_trip(void)
{
    JsonNode *node = parse("{\"transitions\":[{\"mode\":1,\"weight\":55,\"overlap_ms\":3500},{\"mode\":2,\"weight\":45,\"overlap_ms\":2500},"
        "{\"kind\":\"music\",\"mode\":3,\"weight\":60,\"overlap_ms\":8000}],\"transition_selection_mode\":\"linear\"}");
    JsonObject *settings = json_node_get_object(node); guint changes = 0;
    RadioSTransitionDistributionHooks hooks = {distribution_changed, &changes, NULL};
    GtkWidget *base = radios_transition_distribution_new(NULL, FALSE, &hooks); g_object_ref_sink(base);
    radios_transition_distribution_render(base, settings);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(find(base, "rotation-editor-transition-weight-1"))), ==, 55);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(find(base, "rotation-editor-transition-weight-0"))), ==, 0);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(base, "rotation-editor-transition-share-2"))), ==, "%45");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(base, "rotation-editor-transition-order"))), ==, "linear");
    g_assert_true(gtk_widget_get_visible(find(base, "rotation-editor-transition-overlap-1")));
    g_assert_false(gtk_widget_get_visible(find(base, "rotation-editor-transition-overlap-3")));
    g_assert_cmpuint(changes, ==, 0); g_assert_false(radios_transition_distribution_pending(base));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(find(base, "rotation-editor-transition-weight-3")), 20);
    g_assert_cmpuint(changes, ==, 1); g_assert_true(gtk_widget_get_visible(find(base, "rotation-editor-transition-overlap-3")));
    radios_transition_distribution_read(base, settings);
    JsonArray *entries = json_object_get_array_member(settings, "transitions");
    g_assert_cmpuint(json_array_get_length(entries), ==, 4);
    g_assert_cmpint(json_object_get_int_member(entry_at(settings, 2), "mode"), ==, 3);
    g_assert_cmpint(json_object_get_int_member(entry_at(settings, 2), "weight"), ==, 20);
    g_assert_false(json_object_has_member(entry_at(settings, 2), "kind"));
    g_assert_cmpstr(json_object_get_string_member(entry_at(settings, 3), "kind"), ==, "music");
    g_assert_cmpint(json_object_get_int_member(entry_at(settings, 3), "weight"), ==, 60);
    g_assert_cmpstr(json_object_get_string_member(settings, "transition_selection_mode"), ==, "linear");
    for (guint m = 0; m < 4; ++m) {
        gchar *name = g_strdup_printf("rotation-editor-transition-weight-%u", m);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(find(base, name)), 0); g_free(name);
    }
    gchar *message = NULL; g_assert_false(radios_transition_distribution_valid(base, &message));
    g_assert_nonnull(strstr(message, "en az bir geçiş")); g_free(message);
    g_object_unref(base); json_node_free(node);
}
/* A kind's shares: its earlier SYNC rule arrives converted (SYNC 60, the
 * rest crossfading over the same length); designed presets join with their
 * own shares; more than %100 is refused; "Kalan pay" names the rest and its
 * destination; switched-off presets leave their share to the rest; the base
 * and other kinds stay as saved. Earlier kind presets arrive in equal shares. */
static void transition_distribution_kind_shares_and_rest(void)
{
    JsonArray *templates = radios_transition_preset_templates();
    gchar *ids[3]; for (guint i = 0; i < 3; ++i) ids[i] = g_strdup(json_object_get_string_member(json_array_get_object_element(templates, i), "id"));
    gchar *text = g_strdup_printf("{\"transitions\":[{\"mode\":0,\"weight\":100,\"overlap_ms\":0}],"
        "\"kind_transitions\":{\"music\":{\"mode\":3,\"overlap_ms\":8000,\"sync_percent\":60},\"advertisement\":{\"mode\":0,\"overlap_ms\":0},"
        "\"jingle\":{\"mode\":1,\"overlap_ms\":1000},\"sweeper\":{\"mode\":1,\"overlap_ms\":500}},"
        "\"autodj_automation\":{\"enabled\":true,\"kinds\":{\"jingle\":{\"presets\":[\"%s\",\"%s\",\"%s\"],\"rotation\":\"random\"}}}}", ids[0], ids[1], ids[2]);
    JsonNode *node = parse(text); g_free(text); JsonObject *settings = json_node_get_object(node);
    json_object_set_array_member(settings, "autodj_presets", json_array_ref(templates));
    guint changes = 0; RadioSTransitionDistributionHooks hooks = {distribution_changed, &changes, NULL};
    GtkWidget *music = radios_transition_distribution_new("music", TRUE, &hooks); g_object_ref_sink(music);
    radios_transition_distribution_render(music, settings);
    g_assert_true(radios_transition_distribution_pending(music)); g_assert_cmpuint(changes, ==, 0);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(find(music, "rotation-editor-distribution-music-weight-3"))), ==, 60);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(find(music, "rotation-editor-distribution-music-weight-1"))), ==, 40);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(find(music, "rotation-editor-distribution-music-overlap-1"))), ==, 8000);
    GtkWidget *summary = gtk_bin_get_child(GTK_BIN(music));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(summary)), ==, "Crossfade %40 · SYNC beatmatch %60 ▾");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(music, "rotation-editor-distribution-music-rest"))), ==, "%0 → Geçiş dağılımı");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(find(music, "rotation-editor-distribution-music-preset-0")), 20);
    gchar *message = NULL; g_assert_false(radios_transition_distribution_valid(music, &message));
    g_assert_cmpstr(message, ==, "Müzik geçişlerinin toplamı %120 olur; en çok %100 verilebilir."); g_free(message);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(find(music, "rotation-editor-distribution-music-weight-1")), 10);
    g_assert_true(radios_transition_distribution_valid(music, NULL)); g_assert_true(radios_transition_distribution_uses_presets(music));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(summary)), ==, "Crossfade %10 · SYNC beatmatch %60 · Düz geçiş %20 · kalan %10 ▾");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(music, "rotation-editor-distribution-music-rest"))), ==, "%10 → Geçiş dağılımı");
    radios_transition_distribution_read(music, settings);
    JsonArray *entries = json_object_get_array_member(settings, "transitions");
    g_assert_cmpuint(json_array_get_length(entries), ==, 4);
    g_assert_false(json_object_has_member(entry_at(settings, 0), "kind"));
    g_assert_cmpint(json_object_get_int_member(entry_at(settings, 1), "mode"), ==, 1);
    g_assert_cmpint(json_object_get_int_member(entry_at(settings, 1), "overlap_ms"), ==, 8000);
    JsonObject *preset = entry_at(settings, 3);
    g_assert_cmpstr(json_object_get_string_member(preset, "preset_id"), ==, ids[0]);
    g_assert_cmpuint(json_object_get_size(preset), ==, 5);
    g_assert_cmpint(json_object_get_int_member(preset, "mode"), ==, 1); g_assert_cmpint(json_object_get_int_member(preset, "overlap_ms"), ==, 0);
    radios_transition_distribution_set_presets(music, templates, FALSE);
    g_assert_false(radios_transition_distribution_uses_presets(music));
    g_assert_false(gtk_widget_get_visible(find(music, "rotation-editor-distribution-music-preset-0")));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(music, "rotation-editor-distribution-music-rest"))), ==, "%30 → Geçiş dağılımı");
    radios_transition_distribution_render(music, settings);
    g_assert_false(radios_transition_distribution_pending(music));
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(find(music, "rotation-editor-distribution-music-preset-0"))), ==, 20);
    GtkWidget *jingle = radios_transition_distribution_new("jingle", TRUE, &hooks); g_object_ref_sink(jingle);
    radios_transition_distribution_render(jingle, settings);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(find(jingle, "rotation-editor-distribution-jingle-preset-0"))), ==, 34);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(find(jingle, "rotation-editor-distribution-jingle-preset-2"))), ==, 33);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(find(jingle, "rotation-editor-distribution-jingle-weight-1"))), ==, 0);
    g_assert_cmpuint(radios_transition_distribution_count(jingle), ==, 3);
    g_assert_cmpuint(changes, ==, 2);
    g_object_unref(jingle); g_object_unref(music); json_node_free(node); json_array_unref(templates);
    for (guint i = 0; i < 3; ++i) g_free(ids[i]);
}
/* With kind distributions on, the "Türler" rows edit each kind's
 * "Geçişler": the saved rule and kind presets arrive converted, the first
 * save writes every kind into settings.transitions and drops kind_transitions
 * and the kinds' preset lists in the same record; presets switched off stop
 * counting as the kind's presets. */
static void automation_page_kind_distributions_migrate(void)
{
    JsonArray *templates = radios_transition_preset_templates();
    const gchar *first = json_object_get_string_member(json_array_get_object_element(templates, 0), "id");
    gchar *text = g_strdup_printf("{\"transitions\":[{\"mode\":1,\"weight\":100,\"overlap_ms\":3000}],"
        "\"kind_transitions\":{\"music\":{\"mode\":3,\"overlap_ms\":8000,\"sync_percent\":70},\"advertisement\":{\"mode\":0,\"overlap_ms\":0},"
        "\"jingle\":{\"mode\":1,\"overlap_ms\":1000},\"sweeper\":{\"mode\":1,\"overlap_ms\":500}},"
        "\"autodj_automation\":{\"enabled\":true,\"kinds\":{\"sweeper\":{\"presets\":[\"%s\"],\"rotation\":\"sequential\"}}}}", first);
    JsonNode *node = parse(text); g_free(text); JsonObject *settings = json_node_get_object(node);
    json_object_set_array_member(settings, "autodj_presets", json_array_ref(templates));
    RadioSViewContext ctx; memset(&ctx, 0, sizeof ctx); guint changes = 0;
    RadioSAutodjAutomationHooks hooks = {.changed = distribution_changed, .user = &changes, .kind_distributions = TRUE};
    GtkWidget *page = radios_autodj_automation_new(&ctx, &hooks); g_object_ref_sink(page);
    radios_autodj_automation_render(page, settings);
    GtkWidget *music = find(page, "rotation-editor-distribution-music"), *sweeper = find(page, "rotation-editor-distribution-sweeper");
    g_assert_true(GTK_IS_MENU_BUTTON(music)); g_assert_null(find(page, "rotation-editor-automation-music-presets-open"));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(music)))), ==, "Crossfade %30 · SYNC beatmatch %70 ▾");
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(find(sweeper, "rotation-editor-distribution-sweeper-preset-0"))), ==, 100);
    g_assert_true(radios_autodj_automation_kind_uses_presets(page, "sweeper"));
    g_assert_false(radios_autodj_automation_kind_uses_presets(page, "music"));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(find(music, "rotation-editor-distribution-music-weight-1")), 20);
    g_assert_cmpuint(changes, ==, 1);
    gchar *message = NULL; g_assert_true(radios_autodj_automation_valid(page, &message)); g_assert_null(message);
    radios_autodj_automation_read(page, settings);
    g_assert_false(json_object_has_member(settings, "kind_transitions"));
    g_assert_cmpuint(json_object_get_size(json_object_get_object_member(json_object_get_object_member(settings, "autodj_automation"), "kinds")), ==, 0);
    JsonArray *entries = json_object_get_array_member(settings, "transitions"); guint music_entries = 0, sweeper_presets = 0, base = 0;
    for (guint i = 0; i < json_array_get_length(entries); ++i) {
        JsonObject *entry = json_array_get_object_element(entries, i);
        if (!json_object_has_member(entry, "kind")) { ++base; continue; }
        const gchar *kind = json_object_get_string_member(entry, "kind");
        music_entries += !g_strcmp0(kind, "music"); sweeper_presets += !g_strcmp0(kind, "sweeper") && json_object_has_member(entry, "preset_id");
        g_assert_cmpint(json_object_get_int_member(entry, "weight"), >, 0);
    }
    g_assert_cmpuint(base, ==, 1); g_assert_cmpuint(music_entries, ==, 2); g_assert_cmpuint(sweeper_presets, ==, 1);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(page, "rotation-editor-automation-enabled")), FALSE);
    g_assert_false(radios_autodj_automation_kind_uses_presets(page, "sweeper"));
    radios_autodj_automation_render(page, settings);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(gtk_bin_get_child(GTK_BIN(find(page, "rotation-editor-distribution-music"))))), ==,
        "Crossfade %20 · SYNC beatmatch %70 · kalan %10 ▾");
    g_object_unref(page); json_node_free(node); json_array_unref(templates);
}
int main(int argc, char **argv)
{
    gtk_test_init(&argc, &argv, NULL);
    g_test_add_func("/rotation-editor/schema-used-preset-delete-protected", assigned_automation_cannot_be_deleted);
    g_test_add_func("/rotation-editor/new-policy-current-cluster", new_policy_uses_available_cluster_members);
    g_test_add_func("/rotation-editor/kilavuz-4c-players-grouped-by-cluster", kilavuz_players_grouped_by_cluster);
    g_test_add_func("/rotation-editor/kilavuz-4c-add-playlist-or-live-event", kilavuz_add_names_playlist_or_live_event);
    g_test_add_func("/rotation-editor/schema-source-load-isolation", schema_source_load_preserves_identity_and_isolation);
    g_test_add_func("/rotation-editor/presets-hide-fallback-fields", designed_presets_hide_redundant_fallback_fields);
    g_test_add_func("/rotation-editor/high-player-mapping-preserved", high_player_mappings_survive_policy_edit);
    g_test_add_func("/rotation-editor/unselected-edits-stay-on-page", unselected_edits_stay_on_page);
    g_test_add_func("/rotation-editor/live-draft-dates-round-trip", live_source_draft_and_dates_round_trip);
    g_test_add_func("/rotation-editor/kilavuz-live-step-label-follows-mixer-channels", live_step_label_follows_mixer_channels);
    g_test_add_func("/rotation-editor/schema-source-reference-maintenance", source_changes_keep_schema_references_consistent);
    g_test_add_func("/rotation-editor/automation-page-presets-assign-persist", automation_page_presets_assign_and_persist);
    g_test_add_func("/rotation-editor/automation-page-normal-form", automation_page_normal_form_visibility);
    g_test_add_func("/rotation-editor/saved-ack-preserves-inline-settings", saved_ack_preserves_inline_settings);
    g_test_add_func("/rotation-editor/simple-workspace-integrated-settings", simple_workspace_and_integrated_settings);
    g_test_add_func("/rotation-editor/simple-routing-preserves-content-choices", simple_routing_preserves_content_specific_choices);
    g_test_add_func("/rotation-editor/source-draft-independent-weight", source_draft_does_not_block_weight);
    g_test_add_func("/rotation-editor/song-draft-complete-rule", song_draft_preserves_complete_rule);
    g_test_add_func("/rotation-editor/reconnect-same-identity", reconnect_same_identity_keeps_editor_writable);
    g_test_add_func("/rotation-editor/settings-independent-drafts", settings_drafts_do_not_block_independent_fields);
    g_test_add_func("/rotation-editor/source-settings-independent-drafts", source_settings_drafts_do_not_block_independent_fields);
    g_test_add_func("/rotation-editor/add-reorder-preserves-drafts", adding_and_reordering_preserves_unfinished_drafts);
    g_test_add_func("/rotation-editor/routing-independent-clock-draft", routing_assignment_ignores_unfinished_clock);
    g_test_add_func("/rotation-editor/reconnect-different-identity", reconnect_different_identity_rejects_edits);
    g_test_add_func("/rotation-editor/sparse-override-repeat-limits-only", sparse_override_keeps_repeat_limits_only);
    g_test_add_func("/rotation-editor/excluded-kinds-round-trip", excluded_kinds_round_trip_and_empty_drop);
    g_test_add_func("/rotation-editor/chooser-owner-destroy", chooser_survives_owner_destroy);
    g_test_add_func("/rotation-editor/chooser-add-independent-clock-draft", chooser_adds_while_clock_is_unfinished);
    g_test_add_func("/rotation-editor/automation-page-type-drives-fields", automation_page_type_drives_fields);
    g_test_add_func("/rotation-editor/automation-page-type-round-trip", automation_page_type_round_trip);
    g_test_add_func("/rotation-editor/automation-page-kind-rule-links", automation_page_kind_rule_and_links);
    g_test_add_func("/rotation-editor/transition-distribution-base-round-trip", transition_distribution_base_round_trip);
    g_test_add_func("/rotation-editor/transition-distribution-kind-shares-rest", transition_distribution_kind_shares_and_rest);
    g_test_add_func("/rotation-editor/automation-page-kind-distributions-migrate", automation_page_kind_distributions_migrate);
    return g_test_run();
}
