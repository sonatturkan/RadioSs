#define _GNU_SOURCE
#include "radios/recovery.h"
#include "radios/routing_graph.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#define STORE "11111111-1111-4111-8111-111111111111"
#define USER "22222222-2222-4222-8222-222222222222"
#define STATION "33333333-3333-4333-8333-333333333333"
#define ENDPOINT "44444444-4444-4444-8444-444444444444"
#define KEY "55555555-5555-4555-8555-555555555555"
#define PATH "/v1/workflows/desktop/put"
typedef struct { gchar *root, *dir; RadioSConfig config; RadioSRecovery *recovery; JsonNode *body; } Fixture;
static void remove_tree(const gchar *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
        GDir *dir = g_dir_open(path, 0, NULL); const gchar *name;
        while ((name = g_dir_read_name(dir))) { gchar *child = g_build_filename(path, name, NULL); remove_tree(child); g_free(child); }
        g_dir_close(dir); g_rmdir(path);
    } else g_unlink(path);
}
static void start(Fixture *f)
{
    f->root = g_dir_make_tmp("radios-recovery-XXXXXX", NULL); g_assert_nonnull(f->root);
    gchar *data = g_build_filename(f->root, "data", NULL); g_assert_cmpint(g_mkdir(data, 0700), ==, 0); g_free(data);
    f->config = (RadioSConfig){.project_root = f->root};
    f->dir = g_build_filename(f->root, "data", "recovery", STORE, USER, NULL);
    f->recovery = radios_recovery_new(&f->config, STORE, STATION, USER, ENDPOINT);
    f->body = json_node_new(JSON_NODE_OBJECT); JsonObject *o = json_object_new(); json_node_take_object(f->body, o);
    json_object_set_string_member(o, "station_id", STATION); json_object_set_string_member(o, "idempotency_key", KEY);
    json_object_set_string_member(o, "kind", "playout_history"); json_object_set_string_member(o, "name", "Kesinti testi");
    json_object_set_object_member(o, "config", json_object_new());
}
static void clear(Fixture *f)
{ radios_recovery_free(f->recovery); json_node_free(f->body); remove_tree(f->root); g_free(f->root); g_free(f->dir); }
static void prepare(Fixture *f)
{ GError *error = NULL; g_assert_true(radios_recovery_prepare(f->recovery, "POST", PATH, f->body, &error)); g_assert_no_error(error); }
static void counts(Fixture *f, guint pending, guint blocked)
{
    GError *error = NULL; JsonNode *n = radios_recovery_status(f->recovery, &error); g_assert_no_error(error); g_assert_nonnull(n);
    JsonObject *o = json_node_get_object(n); g_assert_cmpuint(json_object_get_int_member(o, "pending"), ==, pending);
    g_assert_cmpuint(json_object_get_int_member(o, "blocked"), ==, blocked); json_node_free(n);
}
static void test_process_reopen(void)
{
    Fixture f; start(&f); counts(&f, 0, 0);
    pid_t child = fork(); g_assert_cmpint(child, >=, 0);
    if (!child) { GError *e = NULL; gboolean ok = radios_recovery_prepare(f.recovery, "POST", PATH, f.body, &e); _exit(ok && !e ? 0 : 1); }
    gint status = 0; g_assert_cmpint(waitpid(child, &status, 0), ==, child); g_assert_true(WIFEXITED(status)); g_assert_cmpint(WEXITSTATUS(status), ==, 0);
    radios_recovery_free(f.recovery); f.recovery = radios_recovery_new(&f.config, STORE, STATION, USER, ENDPOINT);
    counts(&f, 1, 0); GError *error = NULL; JsonNode *n = radios_recovery_pending(f.recovery, &error); g_assert_no_error(error);
    JsonObject *envelope = json_array_get_object_element(json_node_get_array(n), 0);
    g_assert_true(json_node_equal(json_object_get_member(envelope, "body"), f.body));
    gchar *file = g_build_filename(f.dir, KEY ".json", NULL); struct stat st; g_assert_cmpint(lstat(file, &st), ==, 0); g_assert_cmpint(st.st_mode & 0777, ==, 0600); g_free(file);
    g_assert_true(radios_recovery_finish(f.recovery, f.body, TRUE, 0, &error)); g_assert_no_error(error); counts(&f, 0, 0);
    json_node_free(n); clear(&f);
}
static void test_exact_body_and_block(void)
{
    Fixture f; start(&f); prepare(&f); prepare(&f); counts(&f, 1, 0);
    json_object_set_string_member(json_node_get_object(f.body), "name", "Farklı gövde");
    GError *error = NULL; g_assert_false(radios_recovery_prepare(f.recovery, "POST", PATH, f.body, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED); g_clear_error(&error);
    json_object_set_string_member(json_node_get_object(f.body), "name", "Kesinti testi");
    g_assert_true(radios_recovery_finish(f.recovery, f.body, FALSE, 9, &error)); g_assert_no_error(error); counts(&f, 0, 1);
    JsonNode *pending = radios_recovery_pending(f.recovery, &error); g_assert_no_error(error); g_assert_cmpuint(json_array_get_length(json_node_get_array(pending)), ==, 0); json_node_free(pending);
    /* Retrying the same save cannot silently turn a rejected CAS into a new intent. */
    prepare(&f); counts(&f, 0, 1); clear(&f);
}
static void test_secret_and_scope(void)
{
    Fixture f; start(&f); GError *error = NULL;
    json_object_set_string_member(json_node_get_object(f.body), "password", "private-stream-password");
    g_assert_false(radios_recovery_prepare(f.recovery, "POST", PATH, f.body, &error)); g_assert_nonnull(error); g_clear_error(&error);
    json_object_remove_member(json_node_get_object(f.body), "password");
    json_object_set_string_member(json_node_get_object(f.body), "station_id", USER);
    g_assert_false(radios_recovery_prepare(f.recovery, "POST", PATH, f.body, &error)); g_assert_nonnull(error); g_clear_error(&error);
    g_assert_false(radios_recovery_eligible("POST", "/v1/workflows/desktop/user-create", f.body));
    counts(&f, 0, 0); clear(&f);
}
static JsonObject *saved_stream(Fixture *f, gboolean routing)
{
    JsonObject *body = json_node_get_object(f->body), *config = json_object_get_object_member(body, "config");
    json_object_set_string_member(body, "kind", routing ? "audio_routing" : "broadcast_stream");
    JsonObject *settings = config;
    if (routing) {
        RadioSRoutingGraph *graph = g_new0(RadioSRoutingGraph, 1); guint id;
        g_assert_true(radios_routing_graph_add(graph, RG_STREAM, 0, 0, &id, NULL));
        JsonObject *wire = radios_routing_graph_to_json(graph); g_free(graph); g_assert_nonnull(wire);
        JsonObject *item = json_object_new(); json_object_set_string_member(item, "name", "Saved stream");
        json_object_set_object_member(item, "graph", wire);
        JsonArray *configs = json_array_new(); json_array_add_object_element(configs, item);
        json_object_set_int_member(config, "schema_version", 2); json_object_set_string_member(config, "active", "Saved stream");
        json_object_set_array_member(config, "configs", configs);
        JsonObject *node = json_array_get_object_element(json_object_get_array_member(wire, "nodes"), 0);
        settings = json_object_get_object_member(node, "settings");
    } else {
        json_object_set_string_member(config, "url", "http://radio.example/live");
        json_object_set_string_member(config, "username", "source"); json_object_set_int_member(config, "bitrate_kbps", 128);
    }
    json_object_set_boolean_member(settings, "password_saved", TRUE);
    json_object_set_string_member(settings, "password", "saved-encoder-value");
    return settings;
}
static void reject_body(Fixture *f, const gchar *path)
{
    GError *error = NULL;
    g_assert_false(radios_recovery_prepare(f->recovery, "POST", path, f->body, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED); g_clear_error(&error); counts(f, 0, 0);
}
static void test_explicit_saved_stream_passwords(void)
{
    for (guint routing = 0; routing < 2; ++routing) {
        Fixture f; start(&f); saved_stream(&f, routing); prepare(&f); prepare(&f);
        radios_recovery_free(f.recovery); f.recovery = radios_recovery_new(&f.config, STORE, STATION, USER, ENDPOINT);
        GError *error = NULL; JsonNode *pending = radios_recovery_pending(f.recovery, &error);
        g_assert_no_error(error); g_assert_cmpuint(json_array_get_length(json_node_get_array(pending)), ==, 1);
        JsonObject *entry = json_array_get_object_element(json_node_get_array(pending), 0);
        g_assert_true(json_node_equal(json_object_get_member(entry, "body"), f.body)); json_node_free(pending);
        gchar *file = g_build_filename(f.dir, KEY ".json", NULL); struct stat info;
        g_assert_cmpint(lstat(file, &info), ==, 0); g_assert_cmpint(info.st_mode & 0777, ==, 0600); g_free(file);
        JsonNode *status = radios_recovery_status(f.recovery, &error); g_assert_no_error(error);
        gchar *public_status = json_to_string(status, FALSE); g_assert_null(strstr(public_status, "saved-encoder-value"));
        g_free(public_status); json_node_free(status);
        g_assert_true(radios_recovery_finish(f.recovery, f.body, TRUE, 0, &error)); g_assert_no_error(error);
        counts(&f, 0, 0); clear(&f);
    }
}
static void test_saved_password_allowlist_is_scoped(void)
{
    for (guint routing = 0; routing < 2; ++routing) {
        Fixture f; start(&f); JsonObject *settings = saved_stream(&f, routing);
        JsonObject *body = json_node_get_object(f.body);
        reject_body(&f, "/v1/workflows/desktop-broadcast/run-start");
        json_object_set_string_member(body, "kind", "playout_history"); reject_body(&f, PATH);
        json_object_set_string_member(body, "kind", routing ? "audio_routing" : "broadcast_stream");
        static const gchar *const secrets[] = {"password", "passwd", "client_secret", "token", "api_token", "Authorization"};
        for (guint i = 0; i < G_N_ELEMENTS(secrets); ++i) {
            json_object_set_string_member(body, secrets[i], "not-a-saved-encoder-value"); reject_body(&f, PATH);
            json_object_remove_member(body, secrets[i]);
        }
        JsonObject *nested = json_object_new(); json_object_set_boolean_member(nested, "password_saved", TRUE);
        json_object_set_string_member(nested, "password", "nested-secret");
        json_object_set_object_member(settings, "extra", nested); reject_body(&f, PATH); json_object_remove_member(settings, "extra");
        json_object_remove_member(settings, "password_saved"); reject_body(&f, PATH);
        json_object_set_boolean_member(settings, "password_saved", FALSE); reject_body(&f, PATH);
        json_object_set_string_member(settings, "password_saved", "true"); reject_body(&f, PATH);
        json_object_set_boolean_member(settings, "password_saved", TRUE);
        json_object_set_string_member(settings, "password", "line\nbreak"); reject_body(&f, PATH);
        gchar *oversize = g_strnfill(routing ? 1025 : 257, 'x');
        json_object_set_string_member(settings, "password", oversize); g_free(oversize); reject_body(&f, PATH);
        json_object_remove_member(settings, "password"); reject_body(&f, PATH);
        json_object_set_boolean_member(settings, "password_saved", FALSE); prepare(&f);
        GError *error = NULL; g_assert_true(radios_recovery_finish(f.recovery, f.body, TRUE, 0, &error)); g_assert_no_error(error);
        clear(&f);
    }
    Fixture f; start(&f); saved_stream(&f, TRUE);
    JsonObject *config = json_object_get_object_member(json_node_get_object(f.body), "config");
    JsonObject *item = json_array_get_object_element(json_object_get_array_member(config, "configs"), 0);
    JsonObject *graph = json_object_get_object_member(item, "graph");
    JsonObject *node = json_array_get_object_element(json_object_get_array_member(graph, "nodes"), 0);
    json_object_set_string_member(node, "kind", "output"); reject_body(&f, PATH);
    json_object_set_string_member(node, "kind", "stream"); json_object_set_int_member(config, "schema_version", 1);
    reject_body(&f, PATH); clear(&f);
}
static void test_tree_symlink(void)
{
    Fixture f; start(&f); gchar *path = g_build_filename(f.root, "data", "recovery", NULL);
    g_assert_cmpint(symlink("/tmp", path), ==, 0); GError *error = NULL;
    g_assert_false(radios_recovery_prepare(f.recovery, "POST", PATH, f.body, &error)); g_assert_nonnull(error); g_clear_error(&error);
    g_free(path); clear(&f);
}
static void test_entry_symlink_and_corruption(void)
{
    Fixture f; start(&f); prepare(&f); gchar *path = g_build_filename(f.dir, KEY ".json", NULL);
    g_assert_cmpint(g_unlink(path), ==, 0); g_assert_cmpint(symlink("/etc/passwd", path), ==, 0); GError *error = NULL;
    JsonNode *n = radios_recovery_pending(f.recovery, &error); g_assert_null(n); g_assert_nonnull(error); g_clear_error(&error);
    g_assert_cmpint(g_unlink(path), ==, 0); g_assert_true(g_file_set_contents(path, "{broken", -1, NULL)); g_chmod(path, 0600);
    g_assert_false(radios_recovery_prepare(f.recovery, "POST", PATH, f.body, &error)); g_assert_nonnull(error); g_clear_error(&error);
    gchar *bytes = NULL; g_assert_true(g_file_get_contents(path, &bytes, NULL, NULL)); g_assert_cmpstr(bytes, ==, "{broken"); g_free(bytes);
    g_assert_false(radios_recovery_prepare(f.recovery, "POST", PATH, f.body, NULL));
    g_free(path); clear(&f);
}
static void test_identity_and_order(void)
{
    Fixture f; start(&f); prepare(&f); json_object_set_string_member(json_node_get_object(f.body), "idempotency_key", ENDPOINT); prepare(&f);
    GError *error = NULL; JsonNode *n = radios_recovery_pending(f.recovery, &error); g_assert_no_error(error);
    g_assert_cmpuint(json_array_get_length(json_node_get_array(n)), ==, 2);
    JsonObject *first = json_array_get_object_element(json_node_get_array(n), 0);
    g_assert_cmpstr(json_object_get_string_member(json_node_get_object(json_object_get_member(first, "body")), "idempotency_key"), ==, KEY); json_node_free(n);
    radios_recovery_free(f.recovery); f.recovery = radios_recovery_new(&f.config, STORE, STATION, USER, KEY);
    n = radios_recovery_pending(f.recovery, &error); g_assert_null(n); g_assert_nonnull(error); g_clear_error(&error); clear(&f);
}
typedef struct { Fixture *f; gboolean ok; GError *error; } PrepareThread;
static gpointer do_prepare(gpointer value)
{ PrepareThread *p = value; p->ok = radios_recovery_prepare(p->f->recovery, "POST", PATH, p->f->body, &p->error); return NULL; }
static void test_concurrent_prepare(void)
{
    Fixture f; start(&f); PrepareThread a = {.f = &f}, b = {.f = &f};
    GThread *left = g_thread_new("journal-left", do_prepare, &a), *right = g_thread_new("journal-right", do_prepare, &b);
    g_thread_join(left); g_thread_join(right); g_assert_true(a.ok); g_assert_true(b.ok); g_assert_no_error(a.error); g_assert_no_error(b.error);
    counts(&f, 1, 0); clear(&f);
}
static void test_gui_restore_lifecycle_lock(void)
{
    Fixture f; start(&f); gchar *runtime = g_build_filename(f.root, "data", "database-service", NULL);
    g_assert_cmpint(g_mkdir(runtime, 0700), ==, 0); GError *error = NULL;
    gint first = radios_recovery_gui_lock(&f.config, &error), second = radios_recovery_gui_lock(&f.config, &error);
    g_assert_no_error(error); g_assert_cmpint(first, >=, 0); g_assert_cmpint(second, >=, 0);
    gchar *lock_path = g_build_filename(runtime, "gui.lock", NULL); gint restore = open(lock_path, O_RDWR);
    g_assert_cmpint(restore, >=, 0); g_assert_cmpint(flock(restore, LOCK_EX | LOCK_NB), ==, -1);
    close(first); close(second); g_assert_cmpint(flock(restore, LOCK_EX | LOCK_NB), ==, 0);
    g_assert_cmpint(radios_recovery_gui_lock(&f.config, &error), ==, -1); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY); g_clear_error(&error); close(restore);
    gchar *journal = g_build_filename(runtime, "restore-pending.json", NULL);
    g_assert_true(g_file_set_contents(journal, "{}", -1, NULL));
    g_assert_cmpint(radios_recovery_gui_lock(&f.config, &error), ==, -1); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_BUSY); g_clear_error(&error);
    g_unlink(journal); g_free(journal);
    pid_t child = fork(); g_assert_cmpint(child, >=, 0);
    if (!child) { gint fd = radios_recovery_gui_lock(&f.config, NULL); _exit(fd >= 0 ? 0 : 1); }
    gint status = 0; g_assert_cmpint(waitpid(child, &status, 0), ==, child); g_assert_true(WIFEXITED(status)); g_assert_cmpint(WEXITSTATUS(status), ==, 0);
    restore = open(lock_path, O_RDWR); g_assert_cmpint(flock(restore, LOCK_EX | LOCK_NB), ==, 0); close(restore);
    g_free(lock_path); g_free(runtime); clear(&f);
}
static void test_rejected_ancestor_blocks_predicted_successor(void)
{
    Fixture f; start(&f); JsonObject *o = json_node_get_object(f.body);
    json_object_set_string_member(o, "document_id", ENDPOINT); json_object_set_int_member(o, "expected_revision", 1); prepare(&f);
    gchar *encoded = json_to_string(f.body, FALSE); JsonParser *parser = json_parser_new();
    g_assert_true(json_parser_load_from_data(parser, encoded, -1, NULL)); g_free(encoded);
    JsonNode *head = json_node_copy(json_parser_get_root(parser)); g_object_unref(parser);
    /* The optimistic successor was persisted during shutdown. An external
     * writer may also have produced revision 2 while our head was unaccepted. */
    json_object_set_string_member(o, "idempotency_key", STATION); json_object_set_int_member(o, "expected_revision", 2); prepare(&f);
    GError *error = NULL; g_assert_true(radios_recovery_finish(f.recovery, head, FALSE, 9, &error)); g_assert_no_error(error);
    counts(&f, 1, 1); JsonNode *n = radios_recovery_pending(f.recovery, &error); g_assert_no_error(error);
    g_assert_cmpuint(json_array_get_length(json_node_get_array(n)), ==, 0); json_node_free(n);
    n = radios_recovery_status(f.recovery, &error); g_assert_no_error(error);
    JsonArray *rows = json_object_get_array_member(json_node_get_object(n), "operations");
    g_assert_true(json_object_get_boolean_member(json_array_get_object_element(rows, 1), "dependency_blocked")); json_node_free(n);
    /* A different document can still recover on the next pass. */
    json_object_set_string_member(o, "idempotency_key", USER); json_object_set_string_member(o, "document_id", STORE); prepare(&f);
    n = radios_recovery_pending(f.recovery, &error); g_assert_no_error(error);
    g_assert_cmpuint(json_array_get_length(json_node_get_array(n)), ==, 1); json_node_free(n);
    json_node_free(head); clear(&f);
}
static void test_valid_json_damage_checksum(void)
{
    Fixture f; start(&f); prepare(&f); gchar *path = g_build_filename(f.dir, KEY ".json", NULL);
    gchar *bytes = NULL; g_assert_true(g_file_get_contents(path, &bytes, NULL, NULL));
    JsonParser *parser = json_parser_new(); g_assert_true(json_parser_load_from_data(parser, bytes, -1, NULL)); g_free(bytes);
    JsonObject *envelope = json_node_get_object(json_parser_get_root(parser));
    json_object_set_string_member(json_node_get_object(json_object_get_member(envelope, "body")), "name", "Valid JSON but changed intent");
    bytes = json_to_string(json_parser_get_root(parser), FALSE); g_assert_true(g_file_set_contents(path, bytes, -1, NULL)); g_chmod(path, 0600); g_free(bytes);
    GError *error = NULL; JsonNode *pending = radios_recovery_pending(f.recovery, &error);
    g_assert_null(pending); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED); g_clear_error(&error);
    g_assert_false(radios_recovery_prepare(f.recovery, "POST", PATH, f.body, &error)); g_assert_nonnull(error); g_clear_error(&error);
    g_object_unref(parser); g_free(path); clear(&f);
}
static void test_rekordbox_snapshot_receipt_reopen(void)
{
    Fixture f; start(&f); const gchar *path = "/v1/workflows/desktop-rekordbox/apply";
    JsonObject *o = json_node_get_object(f.body);
    json_object_set_string_member(o, "xml_base64", "PERKX1BMQVlMSVNUUy8+");
    json_object_set_string_member(o, "expected_preview_hash", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    json_object_set_string_member(o, "library_key", "native-source");
    json_object_set_string_member(o, "duplicate_policy", "skip");
    JsonArray *paths = json_array_new(), *selection = json_array_new();
    json_array_add_int_element(selection, 0); json_array_add_int_element(selection, 1);
    JsonNode *selected_path = json_node_new(JSON_NODE_ARRAY); json_node_take_array(selected_path, selection);
    json_array_add_element(paths, selected_path); json_object_set_array_member(o, "selected_node_paths", paths);
    g_assert_true(radios_recovery_eligible("POST", path, f.body));
    g_assert_false(radios_recovery_eligible("POST", "/v1/workflows/desktop-rekordbox/preview", f.body));
    g_assert_false(radios_recovery_eligible("POST", "/v1/workflows/desktop-rekordbox/apply?rebase=1", f.body));
    GError *error = NULL;
    g_assert_true(radios_recovery_prepare(f.recovery, "POST", path, f.body, &error)); g_assert_no_error(error);
    radios_recovery_free(f.recovery); f.recovery = radios_recovery_new(&f.config, STORE, STATION, USER, ENDPOINT);
    JsonNode *pending = radios_recovery_pending(f.recovery, &error); g_assert_no_error(error);
    g_assert_cmpuint(json_array_get_length(json_node_get_array(pending)), ==, 1);
    JsonObject *row = json_array_get_object_element(json_node_get_array(pending), 0);
    g_assert_cmpstr(json_object_get_string_member(row, "path"), ==, path);
    g_assert_true(json_node_equal(json_object_get_member(row, "body"), f.body)); json_node_free(pending);
    json_object_set_string_member(o, "duplicate_policy", "keep");
    g_assert_false(radios_recovery_prepare(f.recovery, "POST", path, f.body, &error)); g_assert_nonnull(error); g_clear_error(&error);
    json_object_set_string_member(o, "duplicate_policy", "skip");
    g_assert_true(radios_recovery_finish(f.recovery, f.body, TRUE, 0, &error)); g_assert_no_error(error);
    counts(&f, 0, 0); clear(&f);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/recovery/process-reopen-exact-body", test_process_reopen);
    g_test_add_func("/recovery/exact-body-and-rejected-CAS", test_exact_body_and_block);
    g_test_add_func("/recovery/no-secrets-station-bound", test_secret_and_scope);
    g_test_add_func("/recovery/explicit-saved-stream-passwords", test_explicit_saved_stream_passwords);
    g_test_add_func("/recovery/saved-password-allowlist-scoped", test_saved_password_allowlist_is_scoped);
    g_test_add_func("/recovery/tree-symlink", test_tree_symlink);
    g_test_add_func("/recovery/entry-symlink-corruption", test_entry_symlink_and_corruption);
    g_test_add_func("/recovery/endpoint-identity-order", test_identity_and_order);
    g_test_add_func("/recovery/concurrent-prepare", test_concurrent_prepare);
    g_test_add_func("/recovery/gui-restore-lifecycle-crash-lock", test_gui_restore_lifecycle_lock);
    g_test_add_func("/recovery/rejected-ancestor-no-external-overwrite", test_rejected_ancestor_blocks_predicted_successor);
    g_test_add_func("/recovery/valid-json-damage-sha256", test_valid_json_damage_checksum);
    g_test_add_func("/recovery/rekordbox-exact-snapshot-receipt-reopen", test_rekordbox_snapshot_receipt_reopen);
    return g_test_run();
}
