#include "radios/data.h"
#include "radios/media_jobs.h"
#include <glib/gstdio.h>
#include <string.h>

/* The runner owns a fresh private installation for each selected case. In the
 * SDK cases its service runs on the host, in a different PID namespace. */
static RadioSConfig config;
static gchar *last_notice;
typedef struct { RadioSViewContext ctx; GtkWidget *window; } Harness;
typedef struct { gboolean done; JsonNode *result; GError *error; } Result;
typedef gboolean (*Condition)(gpointer);

static void notice(gpointer unused, const gchar *message)
{ (void)unused; g_free(last_notice); last_notice = g_strdup(message); }
static void pump(void)
{ while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void wait_for(Condition condition, gpointer value, const gchar *description)
{
    gint64 limit = g_get_monotonic_time() + 20 * G_TIME_SPAN_SECOND;
    while (!condition(value) && g_get_monotonic_time() < limit) pump();
    if (!condition(value)) g_error("Native connection timed out: %s (%s)", description,
                                  last_notice ? last_notice : "no notice");
}
static gboolean connected(gpointer value)
{ return radios_data_connected(&((Harness *)value)->ctx); }
static gboolean finished(gpointer value) { return ((Result *)value)->done; }
static gboolean notified(gpointer unused) { (void)unused; return last_notice != NULL; }
static Harness *harness_open(void)
{
    Harness *h = g_new0(Harness, 1); g_clear_pointer(&last_notice, g_free);
    h->ctx.config = &config; h->ctx.notify = notice; h->ctx.runtime = radios_runtime_new(32, 128);
    h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(h->window);
    h->ctx.parent = GTK_WINDOW(h->window); h->ctx.data = radios_data_new(&h->ctx);
    gtk_widget_show_all(h->window); radios_data_connect(&h->ctx); return h;
}
static void harness_close(Harness *h)
{
    gtk_widget_destroy(h->window); radios_data_close(h->ctx.data); radios_runtime_free(h->ctx.runtime);
    while (g_main_context_iteration(NULL, FALSE)) {}
    radios_data_unref(h->ctx.data); g_object_unref(h->window); g_free(h);
}
static void result_done(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result,
    const GError *error, gpointer value)
{
    (void)ctx; (void)owner; Result *output = value;
    output->result = result ? json_node_copy(result) : NULL;
    output->error = error ? g_error_copy(error) : NULL; output->done = TRUE;
}
static void result_clear(Result *result)
{ g_clear_pointer(&result->result, json_node_free); g_clear_error(&result->error); }
static JsonNode *request(Harness *h, const gchar *path, JsonNode *body)
{
    Result result = {0};
    g_assert_true(radios_data_request(&h->ctx, h->window, "POST", path, body,
                                     result_done, &result, NULL));
    wait_for(finished, &result, "authenticated native read"); g_assert_no_error(result.error);
    g_assert_nonnull(result.result); return result.result;
}
static void write_u16(guchar *bytes, guint16 value)
{ value = GUINT16_TO_LE(value); memcpy(bytes, &value, sizeof value); }
static void write_u32(guchar *bytes, guint32 value)
{ value = GUINT32_TO_LE(value); memcpy(bytes, &value, sizeof value); }
static gchar *write_audio(void)
{
    const guint frames = 8000, bytes = frames * 2; guchar *wav = g_malloc0(44 + bytes);
    memcpy(wav, "RIFF", 4); write_u32(wav + 4, 36 + bytes); memcpy(wav + 8, "WAVEfmt ", 8);
    write_u32(wav + 16, 16); write_u16(wav + 20, 1); write_u16(wav + 22, 1);
    write_u32(wav + 24, 8000); write_u32(wav + 28, 16000); write_u16(wav + 32, 2);
    write_u16(wav + 34, 16); memcpy(wav + 36, "data", 4); write_u32(wav + 40, bytes);
    for (guint i = 0; i < frames; ++i) write_u16(wav + 44 + i * 2,
                                              (guint16)(i % 8 < 4 ? 16384 : -16384));
    gchar *path = g_build_filename(config.project_root, "bağlantı-kabul-müziği.wav", NULL);
    g_assert_true(g_file_set_contents(path, (const gchar *)wav, 44 + bytes, NULL));
    g_free(wav); return path;
}
static void existing_service_without_helper(void)
{
    gchar *python = config.python_command;
    config.python_command = g_strdup("/RadioS-no-lifecycle-helper");
    Harness *h = harness_open(); wait_for(connected, h, "reuse host service without lifecycle helper");
    g_assert_nonnull(radios_data_context(&h->ctx));
    g_assert_true(g_uuid_string_is_valid(radios_data_station_id(&h->ctx)));
    gchar *path = write_audio(); Result imported = {0};
    GCancellable *cancel = radios_media_import_async(&h->ctx, h->window, path, "music",
        "Host servis bağlantı kabulü", result_done, &imported, NULL);
    g_assert_nonnull(cancel); wait_for(finished, &imported, "real WAV imported through native UI worker");
    g_assert_no_error(imported.error); g_assert_nonnull(imported.result);
    JsonObject *root = json_node_get_object(imported.result);
    JsonObject *analysis = json_object_get_object_member(root, "analysis");
    JsonObject *version = json_object_get_object_member(root, "version");
    g_assert_cmpint(radios_json_int(analysis, "duration_ms", 0), ==, 1000);
    g_assert_true(json_object_get_boolean_member(version, "verified"));
    g_assert_cmpstr(radios_json_string(version, "status", ""), ==, "draft");
    gchar *id = g_strdup(radios_json_string(json_object_get_object_member(root, "media"), "_id", ""));
    g_assert_true(g_uuid_string_is_valid(id));
    gchar *managed = g_strdup(radios_json_string(version, "managed_path", ""));
    g_assert_true(g_file_test(managed, G_FILE_TEST_IS_REGULAR));
    g_assert_cmpint(g_unlink(path), ==, 0); harness_close(h);
    h = harness_open(); wait_for(connected, h, "reopen native connection to preserved host service");
    JsonNode *body = radios_data_body(&h->ctx);
    json_object_set_string_member(json_node_get_object(body), "media_id", id);
    JsonNode *record = request(h, "/v1/workflows/desktop-media/get", body);
    JsonObject *media = json_object_get_object_member(json_node_get_object(record), "media");
    g_assert_cmpstr(radios_json_string(media, "_id", ""), ==, id);
    g_assert_cmpstr(radios_json_string(media, "title", ""), ==, "Host servis bağlantı kabulü");
    g_assert_true(g_file_test(managed, G_FILE_TEST_IS_REGULAR));
    json_node_free(record); json_node_free(body); harness_close(h);
    result_clear(&imported); g_object_unref(cancel); g_free(path); g_free(id); g_free(managed);
    g_free(config.python_command); config.python_command = python;
}
static void stopped_service_starts_on_host(void)
{
    Harness *h = harness_open(); wait_for(connected, h, "stopped service started through owned lifecycle helper");
    g_assert_nonnull(radios_data_context(&h->ctx));
    g_assert_true(g_uuid_string_is_valid(radios_data_station_id(&h->ctx)));
    g_assert_nonnull(last_notice); g_assert_nonnull(strstr(last_notice, "Veritabanı bağlı"));
    harness_close(h);
}
static void auth_error_does_not_start_service(void)
{
    gchar *python = config.python_command, *token_environment = config.token_environment;
    config.python_command = g_strdup("/RadioS-no-lifecycle-helper");
    config.token_environment = g_strdup("RADIOS_TEST_CONNECTION_BAD_TOKEN");
    g_assert_nonnull(g_getenv(config.token_environment));
    Harness *h = harness_open(); wait_for(notified, NULL, "authentication failure without lifecycle invocation");
    g_assert_false(connected(h)); g_assert_nonnull(strstr(last_notice, "erişim yetkisi"));
    g_assert_null(strstr(last_notice, "çalıştırıcısı")); harness_close(h);
    g_free(config.python_command); config.python_command = python;
    g_free(config.token_environment); config.token_environment = token_environment;
}
typedef struct { guint failed, saved; gchar *last; } SaveStatus;
static void save_status(RadioSDocument *document, const gchar *message, gpointer value)
{
    (void)document; SaveStatus *status = value;
    if (g_str_has_prefix(message, "Kaydedilemedi:")) status->failed++;
    if (g_str_equal(message, "Kaydedildi")) status->saved++;
    g_free(status->last); status->last = g_strdup(message);
}
static gboolean document_saved(gpointer value)
{ return radios_data_document_revision(value) > 0; }
static gboolean save_rejected(gpointer value)
{ return ((SaveStatus *)value)->failed > 0; }
static JsonNode *channel_config(gint decks)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, json_object_new());
    json_object_set_int_member(json_node_get_object(node), "deck_count", decks); return node;
}
static void validation_preserves_latest_pending(void)
{
    Harness *h = harness_open(); wait_for(connected, h, "authenticated document save baseline");
    gchar *id = g_uuid_string_random();
    RadioSDocument *doc = radios_data_document_new_record(&h->ctx, h->window, "channel", "Pending doğrulama kabulü", id, NULL, NULL);
    g_assert_nonnull(doc); SaveStatus status = {0}; radios_data_document_set_status_callback(doc, save_status, &status);
    JsonNode *bad = channel_config(100), *latest = channel_config(7);
    radios_data_document_dirty(doc, TRUE); g_assert_true(radios_data_document_autosave(doc, bad));
    /* Queue a corrected edit before servicing the first HTTP completion. */
    radios_data_document_dirty(doc, TRUE); g_assert_true(radios_data_document_autosave(doc, latest));
    wait_for(document_saved, doc, "latest pending correction saved after rejected first body");
    g_assert_cmpuint(status.failed, ==, 1); g_assert_cmpuint(status.saved, ==, 1);
    g_assert_cmpstr(status.last, ==, "Kaydedildi"); g_assert_cmpint(radios_data_document_revision(doc), ==, 1);
    JsonNode *body = radios_data_body(&h->ctx);
    json_object_set_string_member(json_node_get_object(body), "kind", "channel");
    json_object_set_string_member(json_node_get_object(body), "document_id", id);
    JsonNode *record = request(h, "/v1/workflows/desktop/get", body);
    JsonObject *saved = json_node_get_object(record);
    g_assert_cmpint(radios_json_int(json_object_get_object_member(saved, "config"), "deck_count", 0), ==, 7);
    /* One rejected body cannot remain forever as a saving status or accumulate
     * durable blocked receipts on each automatic timer tick. */
    status.failed = 0; radios_data_document_dirty(doc, TRUE);
    g_assert_true(radios_data_document_autosave(doc, bad)); wait_for(save_rejected, &status, "visible asynchronous validation error");
    g_assert_true(g_str_has_prefix(status.last, "Kaydedilemedi:"));
    JsonNode *before = radios_data_recovery_status(&h->ctx);
    gint64 blocked = radios_json_int(json_node_get_object(before), "blocked", -1); json_node_free(before);
    for (guint i = 0; i < 5; ++i) g_assert_false(radios_data_document_autosave(doc, bad));
    JsonNode *after = radios_data_recovery_status(&h->ctx);
    g_assert_cmpint(radios_json_int(json_node_get_object(after), "blocked", -2), ==, blocked);
    json_node_free(after); json_node_free(record); json_node_free(body); json_node_free(bad); json_node_free(latest);
    harness_close(h); g_free(status.last); g_free(id);
}

static gboolean all_documents_ready(gpointer value)
{
    GPtrArray *documents = value;
    for (guint i = 0; i < documents->len; ++i)
        if (!radios_data_document_ready(g_ptr_array_index(documents, i))) return FALSE;
    return TRUE;
}
static void counted_refresh(RadioSViewContext *ctx, GtkWidget *owner)
{
    (void)ctx; guint *count = g_object_get_data(G_OBJECT(owner), "acceptance-refresh-count"); ++*count;
}
static gboolean refresh_counted(gpointer value) { return *(guint *)value > 0; }
static guint drain_started(RadioSRuntime *runtime)
{
    guint started = 0; RadioSEvent *event;
    while ((event = radios_runtime_pop_event(runtime))) {
        if (event->kind == RADIO_S_EVENT_STARTED) ++started;
        radios_event_free(event);
    }
    return started;
}
static void refresh_burst_shares_kind_snapshot(void)
{
    Harness *h = harness_open(); wait_for(connected, h, "authenticated refresh baseline");
    /* Let the connect refresh settle before measuring the next explicit burst. */
    gint64 until = g_get_monotonic_time() + 450 * 1000;
    while (g_get_monotonic_time() < until) pump();
    drain_started(h->ctx.runtime);
    GPtrArray *documents = g_ptr_array_new();
    for (guint i = 0; i < 12; ++i) {
        gchar *name = g_strdup_printf("Aynı tür görünüm %u", i);
        g_ptr_array_add(documents, radios_data_document_new(&h->ctx, h->window, "channel", name, NULL, NULL));
        g_free(name);
    }
    wait_for(all_documents_ready, documents, "all same-kind consumers loaded from one worker read");
    g_assert_cmpuint(drain_started(h->ctx.runtime), ==, 1);
    guint refreshes = 0; g_object_set_data(G_OBJECT(h->window), "acceptance-refresh-count", &refreshes);
    radios_data_register_refresh(&h->ctx, h->window, counted_refresh);
    for (guint i = 0; i < 20; ++i) radios_data_refresh(&h->ctx);
    g_assert_cmpuint(refreshes, ==, 0);
    wait_for(refresh_counted, &refreshes, "refresh burst debounced to one dispatch");
    g_assert_cmpuint(refreshes, ==, 1);
    /* The twelve clean documents join one fresh request on that dispatch. */
    until = g_get_monotonic_time() + 150 * 1000;
    while (g_get_monotonic_time() < until) pump();
    g_assert_cmpuint(drain_started(h->ctx.runtime), ==, 1);
    g_ptr_array_unref(documents); harness_close(h);
}

typedef struct { guint applies; gint64 decks; } AppliedDocument;
static void apply_document(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *record, gpointer value)
{
    (void)ctx; (void)owner; AppliedDocument *state = value;
    ++state->applies; state->decks = radios_json_int(record, "deck_count", 0);
}
static gboolean document_ready(gpointer value) { return radios_data_document_ready(value); }
static gboolean document_reapplied(gpointer value) { return ((AppliedDocument *)value)->applies > 1; }
static void refresh_and_wait(Harness *h, guint *refreshes)
{
    *refreshes = 0; radios_data_refresh(&h->ctx);
    wait_for(refresh_counted, refreshes, "global refresh dispatched");
    /* Join the queued channel read so the assertion runs after its callbacks. */
    JsonNode *body = radios_data_body(&h->ctx);
    json_object_set_string_member(json_node_get_object(body), "kind", "channel");
    JsonNode *result = request(h, "/v1/workflows/desktop/list", body);
    json_node_free(result); json_node_free(body);
}
static void unchanged_refresh_preserves_forms(void)
{
    Harness *h = harness_open(); wait_for(connected, h, "authenticated form refresh baseline");
    gchar *id = g_uuid_string_random(); JsonNode *saved = channel_config(7);
    RadioSDocument *writer = radios_data_document_new_record(&h->ctx, h->window,
        "channel", "Yenilemede korunan form", id, NULL, NULL);
    g_assert_true(radios_data_document_save(writer, saved));
    wait_for(document_saved, writer, "initial form settings saved");
    AppliedDocument state = {0};
    RadioSDocument *doc = radios_data_document_new_for_id(&h->ctx, h->window,
        "channel", "Yenilemede korunan form", id, apply_document, &state);
    wait_for(document_ready, doc, "initial record applied to the form");
    g_assert_cmpuint(state.applies, ==, 1); g_assert_cmpint(state.decks, ==, 7);
    guint refreshes = 0; g_object_set_data(G_OBJECT(h->window), "acceptance-refresh-count", &refreshes);
    radios_data_register_refresh(&h->ctx, h->window, counted_refresh);
    for (guint i = 0; i < 3; ++i) refresh_and_wait(h, &refreshes);
    g_assert_cmpuint(state.applies, ==, 1);
    radios_data_document_refresh(doc);
    JsonNode *body = radios_data_body(&h->ctx);
    json_object_set_string_member(json_node_get_object(body), "kind", "channel");
    JsonNode *snapshot = request(h, "/v1/workflows/desktop/list", body);
    json_node_free(snapshot); json_node_free(body);
    g_assert_cmpuint(state.applies, ==, 1);

    radios_data_connect(&h->ctx);
    wait_for(connected, h, "same-identity reconnection");
    refresh_and_wait(h, &refreshes); g_assert_cmpuint(state.applies, ==, 1);

    /* Explicit reload still restores the current record, and a genuine
     * external revision remains visible on the next passive refresh. */
    radios_data_document_reload(doc);
    wait_for(document_reapplied, &state, "explicit reload reapplies unchanged settings");
    g_assert_cmpuint(state.applies, ==, 2);
    JsonNode *updated = channel_config(9);
    g_assert_true(radios_data_document_save(writer, updated));
    wait_for(document_saved, writer, "external edit saved");
    refresh_and_wait(h, &refreshes);
    g_assert_cmpuint(state.applies, ==, 3); g_assert_cmpint(state.decks, ==, 9);
    refresh_and_wait(h, &refreshes); g_assert_cmpuint(state.applies, ==, 3);
    json_node_free(updated); json_node_free(saved); g_free(id); harness_close(h);
}

static gboolean file_exists(gpointer value) { return g_file_test(value, G_FILE_TEST_EXISTS); }
static void manual_save_ack_preserves_reconnected_identity(void)
{
    Harness *h = harness_open(); wait_for(connected, h, "manual save identity baseline");
    RadioSDataClient *original = radios_data_client_acquire(&h->ctx); g_assert_nonnull(original);
    AppliedDocument state = {0}; gchar *id = g_uuid_string_random();
    RadioSDocument *doc = radios_data_document_new_record(&h->ctx, h->window,
        "channel", "Önceki oturum kaydı", id, apply_document, &state);
    gchar *root = g_path_get_dirname(config.database_file);
    gchar *hold = g_build_filename(root, ".data-put-hold", NULL);
    gchar *ready = g_build_filename(root, ".data-put-ready", NULL); g_free(root);
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL));
    JsonNode *saved = channel_config(7); g_assert_true(radios_data_document_save(doc, saved));
    wait_for(file_exists, ready, "committed manual save response held by private service");

    /* A separate worker lets the replacement session authenticate while the
     * original worker is still waiting for its committed write response. */
    RadioSRuntime *old_runtime = h->ctx.runtime; h->ctx.runtime = radios_runtime_new(32, 128);
    gchar *token_environment = config.token_environment;
    config.token_environment = g_strdup("RADIOS_TEST_CONNECTION_VIEWER_TOKEN");
    g_assert_nonnull(g_getenv(config.token_environment));
    radios_data_connect(&h->ctx); wait_for(connected, h, "replacement user authenticated before manual save ACK");
    RadioSDataClient *replacement = radios_data_client_acquire(&h->ctx); g_assert_nonnull(replacement);
    g_assert_false(radios_data_client_same_identity(original, replacement));
    g_assert_cmpint(g_unlink(hold), ==, 0);
    wait_for(document_saved, doc, "original manual write acknowledged without applying old settings");
    g_assert_cmpuint(state.applies, ==, 0);
    g_assert_cmpint(radios_data_document_revision(doc), ==, 1);

    JsonNode *body = json_node_new(JSON_NODE_OBJECT); json_node_take_object(body, json_object_new());
    JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "station_id", radios_data_client_station_id(original));
    json_object_set_string_member(object, "kind", "channel"); json_object_set_string_member(object, "document_id", id);
    GError *error = NULL;
    JsonNode *record = radios_data_client_request_sync(original, "POST", "/v1/workflows/desktop/get", body, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(record);
    g_assert_cmpint(radios_json_int(json_object_get_object_member(json_node_get_object(record), "config"), "deck_count", 0), ==, 7);
    json_node_free(record); json_node_free(body); json_node_free(saved);
    radios_runtime_free(old_runtime); radios_data_client_unref(original); radios_data_client_unref(replacement);
    harness_close(h); g_free(config.token_environment); config.token_environment = token_environment;
    g_unlink(ready); g_free(ready); g_free(hold); g_free(id);
}

static void outdated_service_restarts_on_host(void)
{
    Harness *h = harness_open(); wait_for(connected, h, "outdated owned host service restarted through source digest handshake");
    g_assert_nonnull(radios_data_context(&h->ctx));
    g_assert_nonnull(last_notice); g_assert_nonnull(strstr(last_notice, "Veritabanı bağlı"));
    harness_close(h);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); g_assert_cmpint(argc, ==, 2); GError *error = NULL;
    g_assert_true(radios_config_load(argv[1], NULL, &config, &error)); g_assert_no_error(error);
    g_assert_true(gtk_init_check(NULL, NULL));
    g_test_add_func("/data-connection-ui/existing-service-without-helper", existing_service_without_helper);
    g_test_add_func("/data-connection-ui/stopped-service-starts-on-host", stopped_service_starts_on_host);
    g_test_add_func("/data-connection-ui/outdated-service-restarts-on-host", outdated_service_restarts_on_host);
    g_test_add_func("/data-connection-ui/auth-error-does-not-start-service", auth_error_does_not_start_service);
    g_test_add_func("/data-connection-ui/validation-preserves-latest-pending", validation_preserves_latest_pending);
    g_test_add_func("/data-connection-ui/refresh-burst-shares-kind-snapshot", refresh_burst_shares_kind_snapshot);
    g_test_add_func("/data-connection-ui/unchanged-refresh-preserves-forms", unchanged_refresh_preserves_forms);
    g_test_add_func("/data-connection-ui/manual-save-ack-preserves-reconnected-identity", manual_save_ack_preserves_reconnected_identity);
    int result = g_test_run(); radios_config_clear(&config); g_free(last_notice); return result;
}
