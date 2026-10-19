#include "radios/player_loader.h"
#include "radios/playout_ui.h"
#include "radios/data.h"
#include <string.h>

/* Control only the asynchronous list boundaries. The real dialog, GTK
 * selection signals, item conversion and response handling run unchanged. */
typedef struct {
    RadioSViewContext *ctx;
    GWeakRef owner;
    RadioSDataCallback callback;
    gpointer data;
    GDestroyNotify destroy;
    gchar *kind;
} DataReply;
typedef struct {
    RadioSViewContext ctx;
    GtkWidget *window, *owner, *dialog;
    guint calls, count;
    gboolean append;
    JsonArray *items;
    gchar *name, *title, *playlist_id, *entry_id;
} Harness;

static GPtrArray *data_replies;
static const gchar *rejected_kind;

gboolean __wrap_radios_data_connected(RadioSViewContext *ctx);
gboolean __wrap_radios_data_request(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *method, const gchar *path, JsonNode *body,
    RadioSDataCallback callback, gpointer data, GDestroyNotify destroy);

gboolean __wrap_radios_data_connected(RadioSViewContext *ctx)
{ (void)ctx; return TRUE; }
gboolean __wrap_radios_data_request(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *method, const gchar *path, JsonNode *body,
    RadioSDataCallback callback, gpointer data, GDestroyNotify destroy)
{
    g_assert_cmpstr(method, ==, "POST");
    const gchar *kind = g_str_has_suffix(path, "desktop-playlists/list") ? "library" :
        radios_json_string(json_node_get_object(body), "kind", "");
    if (rejected_kind && !g_strcmp0(rejected_kind, kind)) {
        if (destroy) destroy(data);
        return FALSE;
    }
    DataReply *reply = g_new0(DataReply, 1); reply->ctx = ctx;
    g_weak_ref_init(&reply->owner, owner); reply->callback = callback; reply->data = data; reply->destroy = destroy;
    reply->kind = g_strdup(g_str_has_suffix(path, "desktop-playlists/list") ? "library" :
        radios_json_string(json_node_get_object(body), "kind", ""));
    g_ptr_array_add(data_replies, reply); return TRUE;
}
static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *loader_dialog(void)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *result = NULL;
    for (GList *item = windows; item && !result; item = item->next)
        if (g_str_equal(gtk_widget_get_name(item->data), "player-loader-dialog")) result = item->data;
    g_list_free(windows); return result;
}
static JsonNode *parse(const gchar *text)
{
    JsonParser *parser = json_parser_new(); g_assert_true(json_parser_load_from_data(parser, text, -1, NULL));
    JsonNode *node = json_node_copy(json_parser_get_root(parser)); g_object_unref(parser); return node;
}
static void data_reply_free(gpointer data)
{
    DataReply *reply = data; if (reply->destroy) reply->destroy(reply->data);
    g_weak_ref_clear(&reply->owner); g_free(reply->kind); g_free(reply);
}
static void data_complete(const gchar *kind, const gchar *text)
{
    for (guint i = 0; i < data_replies->len; ++i) {
        DataReply *reply = g_ptr_array_index(data_replies, i); if (g_strcmp0(reply->kind, kind)) continue;
        GtkWidget *owner = g_weak_ref_get(&reply->owner); JsonNode *node = parse(text);
        if (owner && !gtk_widget_in_destruction(owner)) reply->callback(reply->ctx, owner, node, NULL, reply->data);
        if (owner) g_object_unref(owner);
        json_node_free(node); g_ptr_array_remove_index(data_replies, i); return;
    }
    g_error("Expected pending %s list request", kind);
}
static void data_fail(const gchar *kind)
{
    for (guint i = 0; i < data_replies->len; ++i) {
        DataReply *reply = g_ptr_array_index(data_replies, i); if (g_strcmp0(reply->kind, kind)) continue;
        GtkWidget *owner = g_weak_ref_get(&reply->owner);
        GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Temporary list request timeout");
        if (owner && !gtk_widget_in_destruction(owner)) reply->callback(reply->ctx, owner, NULL, error, reply->data);
        if (owner) g_object_unref(owner);
        g_error_free(error); g_ptr_array_remove_index(data_replies, i); return;
    }
    g_error("Expected pending %s list request to fail", kind);
}
static guint pending_count(const gchar *kind)
{
    guint count = 0;
    for (guint i = 0; i < data_replies->len; ++i)
        count += !g_strcmp0(((DataReply *)g_ptr_array_index(data_replies, i))->kind, kind);
    return count;
}
static void loaded(JsonArray *items, const gchar *name, gboolean append, gpointer data)
{
    Harness *h = data; ++h->calls; h->count = json_array_get_length(items); h->append = append;
    g_clear_pointer(&h->items, json_array_unref); h->items = json_array_ref(items);
    g_free(h->name); h->name = g_strdup(name); g_free(h->title);
    JsonObject *item = json_array_get_object_element(items, 0);
    h->title = g_strdup(radios_json_string(item, "title", ""));
    g_free(h->playlist_id); h->playlist_id = g_strdup(radios_json_string(item, "playlist_id", ""));
    g_free(h->entry_id); h->entry_id = g_strdup(radios_json_string(item, "entry_id", ""));
}
static Harness *open_harness(void)
{
    g_assert_null(data_replies); data_replies = g_ptr_array_new_with_free_func(data_reply_free);
    Harness *h = g_new0(Harness, 1); h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    h->ctx.parent = GTK_WINDOW(h->window); h->owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_container_add(GTK_CONTAINER(h->window), h->owner); gtk_widget_show_all(h->window);
    radios_player_loader_open(&h->ctx, h->owner, "Playlist yükle", loaded, h);
    h->dialog = loader_dialog(); g_assert_nonnull(h->dialog); return h;
}
static void close_harness(Harness *h)
{
    gtk_widget_destroy(h->window); rejected_kind = NULL;
    g_clear_pointer(&data_replies, g_ptr_array_unref); g_free(h->name); g_free(h->title);
    g_free(h->playlist_id); g_free(h->entry_id);
    g_clear_pointer(&h->items, json_array_unref); g_free(h);
    while (g_main_context_iteration(NULL, FALSE)) {}
}
static GtkTreeSelection *selection(Harness *h, const gchar *name)
{
    GtkWidget *table = find(h->dialog, name); g_assert_nonnull(table);
    return gtk_tree_view_get_selection(g_object_get_data(G_OBJECT(table), "tree-view"));
}
static void select_row(Harness *h, const gchar *name, guint index)
{
    GtkTreePath *path = gtk_tree_path_new_from_indices((gint)index, -1);
    gtk_tree_selection_select_path(selection(h, name), path); gtk_tree_path_free(path);
}
static void page(Harness *h, const gchar *name)
{ gtk_stack_set_visible_child_name(GTK_STACK(find(h->dialog, "player-loader-stack")), name); }
static gboolean can_load(Harness *h)
{ return gtk_widget_is_sensitive(gtk_dialog_get_widget_for_response(GTK_DIALOG(h->dialog), GTK_RESPONSE_ACCEPT)); }
static void library_complete(void)
{
    data_complete("library", "{\"playlists\":[{\"_id\":\"11111111-1111-4111-8111-111111111111\",\"name\":\"Local list\",\"tracks\":[{"
        "\"id\":\"22222222-2222-4222-8222-222222222222\","
        "\"metadata\":{\"title\":\"Local song\",\"duration_ms\":1000},\"media_id\":\"item\",\"version_id\":\"version\","
        "\"media_file_id\":\"media\",\"media_sha256\":\"sha\"}]}]}");
}
static void programme_complete(void)
{ data_complete("programme", "{\"records\":[]}"); }

static void test_tab_restore(void)
{
    Harness *h = open_harness(); library_complete(); select_row(h, "player-loader-library", 0); g_assert_true(can_load(h));
    page(h, "programme"); g_assert_false(can_load(h)); page(h, "library");
    g_assert_true(gtk_tree_selection_get_selected(selection(h, "player-loader-library"), NULL, NULL));
    g_assert_true(can_load(h)); gtk_dialog_response(GTK_DIALOG(h->dialog), 1);
    g_assert_cmpuint(h->calls, ==, 1); g_assert_cmpuint(h->count, ==, 1); g_assert_true(h->append);
    g_assert_cmpstr(h->name, ==, "Local list"); g_assert_cmpstr(h->title, ==, "Local song");
    g_assert_cmpstr(h->playlist_id, ==, "11111111-1111-4111-8111-111111111111");
    g_assert_cmpstr(h->entry_id, ==, "22222222-2222-4222-8222-222222222222"); close_harness(h);
}
static void test_destroyed_owner_discards_replies(void)
{
    Harness *h = open_harness(); page(h, "programme"); gtk_widget_destroy(h->owner);
    g_assert_null(loader_dialog()); programme_complete(); library_complete();
    g_assert_cmpuint(h->calls, ==, 0); close_harness(h);
}

static void retry_and_load_library(Harness *h)
{
    g_assert_false(can_load(h));
    page(h, "programme"); page(h, "library");
    g_assert_cmpuint(pending_count("library"), ==, 1);
    library_complete(); select_row(h, "player-loader-library", 0); g_assert_true(can_load(h));
    gtk_dialog_response(GTK_DIALOG(h->dialog), GTK_RESPONSE_ACCEPT);
    g_assert_cmpuint(h->calls, ==, 1); g_assert_cmpuint(h->count, ==, 1); g_assert_false(h->append);
    g_assert_cmpstr(h->name, ==, "Local list"); g_assert_cmpstr(h->title, ==, "Local song");
}
static void test_failed_list_can_retry_without_reopening(void)
{
    Harness *h = open_harness(); data_fail("library");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(h->dialog, "player-loader-status"))), ==,
        "Temporary list request timeout");
    retry_and_load_library(h); close_harness(h);
}
static void test_rejected_list_can_retry_without_reopening(void)
{
    rejected_kind = "library";
    Harness *h = open_harness();
    g_assert_cmpuint(pending_count("library"), ==, 0);
    rejected_kind = NULL;
    retry_and_load_library(h); close_harness(h);
}

static guint list_count(Harness *h, const gchar *name)
{
    GtkWidget *table = find(h->dialog, name); g_assert_nonnull(table);
    GtkTreeView *tree = g_object_get_data(G_OBJECT(table), "tree-view");
    return (guint)gtk_tree_model_iter_n_children(gtk_tree_view_get_model(tree), NULL);
}
static void test_input_identity_fences(void)
{
    /* Only a complete verified media binding becomes a card input. */
    const gchar *invalid[] = {
        "{\"title\":\"Metadata only\"}",
        "{\"title\":\"Partial binding\",\"media_id\":\"media-identity\",\"version_id\":\"version-identity\"}",
        "{\"title\":\"Legacy saved reference\",\"external_id\":\"abcdefghijk\",\"url\":\"https://example.invalid/recording\"}"
    };
    for (guint i = 0; i < G_N_ELEMENTS(invalid); ++i) {
        JsonNode *node = parse(invalid[i]);
        g_assert_null(radios_player_input_new(json_node_get_object(node))); json_node_free(node);
    }
    const gchar *keys[] = {"media_id", "version_id", "media_file_id", "media_sha256"};
    const gchar *values[] = {"media-identity", "version-identity", "file-identity", "digest-identity"};
    JsonObject *row = json_object_new(); json_object_set_string_member(row, "title", "Bound local recording");
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) json_object_set_string_member(row, keys[i], values[i]);
    JsonObject *input = radios_player_input_new(row); g_assert_nonnull(input);
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i)
        g_assert_cmpstr(radios_json_string(input, keys[i], ""), ==, values[i]);
    json_object_unref(input);
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) {
        json_object_remove_member(row, keys[i]); g_assert_null(radios_player_input_new(row));
        json_object_set_string_member(row, keys[i], values[i]);
    }
    json_object_unref(row);
}
/* A library list only offers entries with a complete verified media binding;
 * metadata-only rows are counted in the status text, never loaded. */
static void test_library_picker_skips_unbound_entries(void)
{
    for (guint append = 0; append < 2; ++append) {
        Harness *h = open_harness();
        data_complete("library", "{\"playlists\":[{\"_id\":\"11111111-1111-4111-8111-111111111111\",\"name\":\"Mixed binding list\",\"tracks\":["
            "{\"_id\":\"22222222-2222-4222-8222-222222222221\",\"title\":\"Local recording\",\"duration_ms\":1000,"
            "\"media_id\":\"item\",\"version_id\":\"version\",\"media_file_id\":\"media\",\"media_sha256\":\"sha\"},"
            "{\"_id\":\"22222222-2222-4222-8222-222222222222\",\"title\":\"Unbound metadata\"},"
            "{\"_id\":\"22222222-2222-4222-8222-222222222223\",\"title\":\"Second recording\",\"duration_ms\":2000,"
            "\"media_id\":\"item2\",\"version_id\":\"version2\",\"media_file_id\":\"media2\",\"media_sha256\":\"sha2\"}]}]}");
        select_row(h, "player-loader-library", 0); g_assert_true(can_load(h));
        g_assert_cmpuint(list_count(h, "player-loader-library-items"), ==, 2);
        g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(h->dialog, "player-loader-status"))), ==,
            "2 parça · ses dosyası olmayan 1 parça listelenmedi.");
        gtk_dialog_response(GTK_DIALOG(h->dialog), append ? 1 : GTK_RESPONSE_ACCEPT);
        g_assert_cmpuint(h->calls, ==, 1); g_assert_cmpuint(h->count, ==, 2); g_assert_cmpint(h->append, ==, append);
        g_assert_cmpstr(h->name, ==, "Mixed binding list");
        const gchar *entries[] = {"22222222-2222-4222-8222-222222222221", "22222222-2222-4222-8222-222222222223"};
        const gchar *files[] = {"media", "media2"};
        for (guint i = 0; i < h->count; ++i) {
            JsonObject *input = json_array_get_object_element(h->items, i);
            g_assert_cmpstr(radios_json_string(input, "playlist_id", ""), ==, "11111111-1111-4111-8111-111111111111");
            g_assert_cmpstr(radios_json_string(input, "entry_id", ""), ==, entries[i]);
            g_assert_cmpstr(radios_json_string(input, "media_file_id", ""), ==, files[i]);
        }
        close_harness(h);
    }
}

/* Saved programme events keep their verified bindings and order when they
 * become card inputs; the persisted document itself is never modified. */
static void test_programme_local_identity_roundtrip(void)
{
    const gchar *keys[] = {"media_id", "version_id", "media_file_id", "media_sha256"};
    Harness *h = open_harness(); library_complete(); page(h, "programme");
    JsonObject *document = json_object_new(), *config = json_object_new(); JsonArray *events = json_array_new(), *records = json_array_new();
    json_object_set_string_member(document, "_id", "11111111-1111-4111-8111-111111111111");
    json_object_set_string_member(document, "name", "Persisted local programme");
    json_object_set_string_member(config, "title", "Persisted local programme");
    for (guint i = 0; i < 3; ++i) {
        JsonObject *event = json_object_new(); gchar *id = g_strdup_printf("22222222-2222-4222-8222-22222222222%u", i + 1);
        json_object_set_string_member(event, "id", id); g_free(id);
        gchar *title = g_strdup_printf("Local event %u", i + 1);
        json_object_set_string_member(event, "title", title); g_free(title);
        json_object_set_string_member(event, "artist", "Source artist");
        json_object_set_string_member(event, "album", "Source album"); json_object_set_int_member(event, "duration_ms", (i + 1) * 1000);
        /* The last event is metadata only and must not become an input. */
        for (guint j = 0; i < 2 && j < G_N_ELEMENTS(keys); ++j) {
            gchar *value = g_strdup_printf("%s-%u", keys[j], i + 1);
            json_object_set_string_member(event, keys[j], value); g_free(value);
        }
        json_array_add_object_element(events, event);
    }
    json_object_set_array_member(config, "events", events); json_object_set_object_member(document, "config", config);
    json_array_add_object_element(records, document); JsonNode *payload = parse("{}");
    json_object_set_array_member(json_node_get_object(payload), "records", records);
    gchar *persisted = json_to_string(payload, FALSE); data_complete("programme", persisted);
    select_row(h, "player-loader-programme", 0); g_assert_true(can_load(h));
    g_assert_cmpuint(list_count(h, "player-loader-programme-items"), ==, 2);
    gtk_dialog_response(GTK_DIALOG(h->dialog), GTK_RESPONSE_ACCEPT);
    g_assert_cmpuint(h->calls, ==, 1); g_assert_cmpuint(h->count, ==, 2);
    g_assert_cmpstr(h->name, ==, "Persisted local programme");
    for (guint i = 0; i < 2; ++i) {
        JsonObject *input = json_array_get_object_element(h->items, i);
        gchar *entry = g_strdup_printf("22222222-2222-4222-8222-22222222222%u", i + 1), *title = g_strdup_printf("Local event %u", i + 1);
        g_assert_cmpstr(radios_json_string(input, "entry_id", ""), ==, entry); g_free(entry);
        g_assert_cmpstr(radios_json_string(input, "playlist_id", ""), ==, "11111111-1111-4111-8111-111111111111");
        g_assert_cmpstr(radios_json_string(input, "title", ""), ==, title); g_free(title);
        g_assert_cmpstr(radios_json_string(input, "artist", ""), ==, "Source artist");
        g_assert_cmpstr(radios_json_string(input, "album", ""), ==, "Source album");
        g_assert_cmpint(radios_json_int(input, "duration_ms", 0), ==, (i + 1) * 1000);
        for (guint j = 0; j < G_N_ELEMENTS(keys); ++j) {
            gchar *value = g_strdup_printf("%s-%u", keys[j], i + 1);
            g_assert_cmpstr(radios_json_string(input, keys[j], ""), ==, value); g_free(value);
        }
    }
    gchar *after = json_to_string(payload, FALSE); g_assert_cmpstr(after, ==, persisted); g_free(after); g_free(persisted);
    close_harness(h); json_node_free(payload);
}

int main(int argc, char **argv)
{
    if (!gtk_init_check(&argc, &argv)) return 77;
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/player-loader/tab-restore", test_tab_restore);
    g_test_add_func("/player-loader/destroyed-owner", test_destroyed_owner_discards_replies);
    g_test_add_func("/player-loader/failed-list-retry", test_failed_list_can_retry_without_reopening);
    g_test_add_func("/player-loader/rejected-list-retry", test_rejected_list_can_retry_without_reopening);
    g_test_add_func("/player-loader/input-identity-fences", test_input_identity_fences);
    g_test_add_func("/player-loader/library-picker-skips-unbound", test_library_picker_skips_unbound_entries);
    g_test_add_func("/player-loader/programme-local-identity-roundtrip", test_programme_local_identity_roundtrip);
    return g_test_run();
}
