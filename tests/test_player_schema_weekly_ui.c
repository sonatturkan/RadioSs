/* Genuine saved-weekly schema acceptance. Reuse the focused production GTK
 * and authenticated HTTP fixture, not a substitute picker or backend. */
#define main radios_saved_schema_legacy_calendar_main
#include "test_automation_calendar.c"
#undef main
#include "radios/player_rotation.h"
#include "radios/programme_model.h"

typedef struct {
    JsonNode *prepared;
    gchar *document_id, *rotation_id, *name;
    gint64 revision;
    RadioSDataClient *client;
    guint called;
} SchemaCapture;

static void schema_loaded(JsonObject *prepared, const gchar *document_id, gint64 revision,
    const gchar *rotation_id, const gchar *name, RadioSDataClient *client, gpointer value)
{
    SchemaCapture *capture = value; ++capture->called;
    capture->prepared = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(capture->prepared, radios_programme_clone_object(prepared));
    capture->document_id = g_strdup(document_id); capture->rotation_id = g_strdup(rotation_id);
    capture->name = g_strdup(name); capture->revision = revision; capture->client = radios_data_client_ref(client);
}
static void schema_capture_clear(SchemaCapture *capture)
{
    g_clear_pointer(&capture->prepared, json_node_free);
    g_clear_pointer(&capture->client, radios_data_client_unref);
    g_free(capture->document_id); g_free(capture->rotation_id); g_free(capture->name);
}
static gboolean schema_captured(gpointer value) { return ((SchemaCapture *)value)->called > 0; }
static JsonObject *schema_definition(const gchar *playlist, const gchar *name, gboolean schema)
{
    JsonObject *definition = json_object_new(), *settings = json_object_new(), *transition = json_object_new(), *source = json_object_new();
    JsonArray *transitions = json_array_new(), *sources = json_array_new(); gchar *id = g_uuid_string_random();
    json_object_set_string_member(definition, "id", id); g_free(id); json_object_set_string_member(definition, "name", name);
    json_object_set_string_member(settings, "selection_mode", "linear");
    const gchar *gaps[] = {"track_gap_minutes", "artist_gap_minutes", "album_gap_minutes"};
    for (guint i = 0; i < G_N_ELEMENTS(gaps); ++i) json_object_set_int_member(settings, gaps[i], 0);
    json_object_set_boolean_member(settings, "automation_schema", schema);
    json_object_set_int_member(transition, "mode", 0); json_object_set_int_member(transition, "weight", 100);
    json_object_set_int_member(transition, "overlap_ms", 0); json_array_add_object_element(transitions, transition);
    json_object_set_array_member(settings, "transitions", transitions); json_object_set_object_member(definition, "settings", settings);
    id = g_uuid_string_random(); json_object_set_string_member(source, "id", id); g_free(id);
    json_object_set_string_member(source, "playlist_id", playlist); json_object_set_int_member(source, "frequency", 1);
    const gchar *flags[] = {"random", "track_random", "advert_random", "sweeper_random"};
    for (guint i = 0; i < G_N_ELEMENTS(flags); ++i) json_object_set_boolean_member(source, flags[i], FALSE);
    json_object_set_int_member(source, "count", 1); json_object_set_int_member(source, "weight", 100);
    json_object_set_int_member(source, "start_minute", 0); json_object_set_int_member(source, "end_minute", 1440);
    json_object_set_int_member(source, "interval_minutes", 0); json_array_add_object_element(sources, source);
    json_object_set_array_member(definition, "sources", sources); return definition;
}
static JsonNode *schema_calendar(const gchar *playlist, const gchar *zone)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *calendar = json_object_new();
    json_node_take_object(node, calendar); json_object_set_int_member(calendar, "schema", 1);
    json_object_set_array_member(calendar, "events", json_array_new()); json_object_set_string_member(calendar, "timezone", zone);
    JsonArray *library = json_array_new(); json_array_add_object_element(library, schema_definition(playlist, "Aynı adlı şema", TRUE));
    json_array_add_object_element(library, schema_definition(playlist, "Sıradan rotasyon gizlenmeli", FALSE));
    json_object_set_array_member(calendar, "rotation_library", library); return node;
}
static JsonNode *schema_main_put(JsonNode *configuration)
{
    JsonNode *old = calendar_record(), *body = body_new(); JsonObject *o = json_node_get_object(body);
    json_object_set_string_member(o, "kind", "automation_calendar"); json_object_set_string_member(o, "name", "main");
    if (old) {
        json_object_set_string_member(o, "document_id", radios_json_string(json_node_get_object(old), "_id", ""));
        json_object_set_int_member(o, "expected_revision", radios_json_int(json_node_get_object(old), "_rev", 0));
    }
    gchar *key = g_uuid_string_random(); json_object_set_string_member(o, "idempotency_key", key); g_free(key);
    json_object_set_member(o, "config", json_node_copy(configuration)); JsonNode *saved = request("/v1/workflows/desktop/put", body);
    json_node_free(body); g_clear_pointer(&old, json_node_free); return saved;
}
static JsonNode *schema_weekly_create(const gchar *playlist, const gchar *name, const gchar *zone)
{
    JsonNode *calendar = schema_calendar(playlist, zone), *node = json_node_new(JSON_NODE_OBJECT); JsonObject *o = json_object_new();
    json_node_take_object(node, o); json_object_set_int_member(o, "schema", 1);
    json_object_set_string_member(o, "week_anchor", "2026-11-02"); json_object_set_int_member(o, "week_count", 1);
    json_object_set_boolean_member(o, "repeat", FALSE); json_object_set_member(o, "calendar", calendar);
    gchar *id = g_uuid_string_random(); JsonNode *saved = weekly_put(NULL, name, node, id); g_free(id); json_node_free(node); return saved;
}
typedef struct {
    Harness *harness;
    gchar *current, *other;
    JsonNode *main, *weekly_a, *weekly_b;
} SchemaFixture;
static SchemaFixture schema_fixture_new(void)
{
    SchemaFixture fixture = {0}; fixture.harness = harness_open();
    fixture.current = create_playlist("Şema mevcut playlist"); fixture.other = create_playlist("Şema diğer playlist");
    JsonNode *media = verified_media(fixture.harness);
    bind_playlist_with_title(fixture.current, media, "Mevcut playlist ilk şarkı");
    bind_playlist_with_title(fixture.other, media, "Şemadan sonraki şarkı"); json_node_free(media);
    JsonNode *calendar = schema_calendar(fixture.other, "UTC"); fixture.main = schema_main_put(calendar); json_node_free(calendar);
    fixture.weekly_a = schema_weekly_create(fixture.other, "Birinci kayıtlı program", "Asia/Istanbul");
    fixture.weekly_b = schema_weekly_create(fixture.other, "İkinci kayıtlı program", "UTC");
    radios_data_refresh(&fixture.harness->ctx); wait_until(harness_ready, fixture.harness, "real saved main and weekly records ready");
    return fixture;
}
static void schema_fixture_unchanged(SchemaFixture *fixture)
{
    JsonNode *current = calendar_record(); g_assert_true(json_node_equal(current, fixture->main)); json_node_free(current);
    current = weekly_record(radios_json_string(json_node_get_object(fixture->weekly_a), "_id", ""), NULL);
    g_assert_true(json_node_equal(current, fixture->weekly_a)); json_node_free(current);
    current = weekly_record(radios_json_string(json_node_get_object(fixture->weekly_b), "_id", ""), NULL);
    g_assert_true(json_node_equal(current, fixture->weekly_b)); json_node_free(current);
}
static void schema_fixture_close(SchemaFixture *fixture)
{
    harness_close(fixture->harness); g_free(fixture->current); g_free(fixture->other);
    json_node_free(fixture->main); json_node_free(fixture->weekly_a); json_node_free(fixture->weekly_b);
}
typedef struct { GtkWidget *dialog; SchemaFixture *fixture; } SchemaListWait;
static gboolean schema_list_ready(gpointer value)
{
    SchemaListWait *wait = value;
    GtkWidget *list = control(wait->dialog, "player-rotation-list");
    if (!gtk_widget_get_sensitive(list)) return FALSE;
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(list)); GtkTreeIter iter;
    JsonNode *expected[] = {wait->fixture->main, wait->fixture->weekly_a, wait->fixture->weekly_b};
    guint found[G_N_ELEMENTS(expected)] = {0};
    for (gboolean valid = gtk_tree_model_get_iter_first(model, &iter); valid; valid = gtk_tree_model_iter_next(model, &iter)) {
        gchar *kind = NULL, *document_id = NULL, *rotation_id = NULL; gint64 revision = 0;
        gtk_tree_model_get(model, &iter, 2, &rotation_id, 3, &kind, 4, &document_id, 5, &revision, -1);
        for (guint i = 0; i < G_N_ELEMENTS(expected); ++i) {
            JsonObject *record = json_node_get_object(expected[i]);
            if (g_strcmp0(document_id, radios_json_string(record, "_id", ""))) continue;
            JsonObject *config = json_object_get_object_member(record, "config");
            if (!g_strcmp0(radios_json_string(record, "kind", ""), "weekly_programme"))
                config = json_object_get_object_member(config, "calendar");
            JsonObject *definition = json_array_get_object_element(json_object_get_array_member(config, "rotation_library"), 0);
            if (!g_strcmp0(kind, radios_json_string(record, "kind", "")) &&
                !g_strcmp0(rotation_id, radios_json_string(definition, "id", "")) &&
                revision == radios_json_int(record, "_rev", 0)) ++found[i];
        }
        g_free(kind); g_free(document_id); g_free(rotation_id);
    }
    /* Earlier cases deliberately leave valid programmes in this same private
     * store. Readiness requires each current fixture's exact pinned identity,
     * never a brittle total count or an unrelated previously loaded record. */
    for (guint i = 0; i < G_N_ELEMENTS(found); ++i) if (found[i] != 1) return FALSE;
    return TRUE;
}
static gboolean schema_preview_ready(gpointer value)
{
    GtkWidget *dialog = value; const gchar *status = gtk_label_get_text(GTK_LABEL(control(dialog, "player-rotation-status")));
    const gchar *previous = g_object_get_data(G_OBJECT(dialog), "schema-test-last-status");
    if (g_strcmp0(previous, status)) {
        g_test_message("Actual schema preview status: %s", status);
        g_object_set_data_full(G_OBJECT(dialog), "schema-test-last-status", g_strdup(status), g_free);
    }
    return gtk_widget_get_sensitive(control(dialog, "player-rotation-load"));
}
static void schema_select(GtkWidget *dialog, const gchar *document_id, const gchar *kind)
{
    GtkWidget *tree = control(dialog, "player-rotation-list"); GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree)); GtkTreeIter iter;
    g_assert_cmpint(gtk_tree_model_get_n_columns(model), ==, 6); gboolean found = FALSE;
    for (gboolean valid = gtk_tree_model_get_iter_first(model, &iter); valid; valid = gtk_tree_model_iter_next(model, &iter)) {
        gchar *row_kind = NULL, *row_document = NULL; gint64 revision = 0;
        gtk_tree_model_get(model, &iter, 3, &row_kind, 4, &row_document, 5, &revision, -1);
        gboolean selected = !g_strcmp0(row_kind, kind) && !g_strcmp0(row_document, document_id); g_free(row_kind); g_free(row_document);
        if (selected) { g_assert_cmpint(revision, >, 0); gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), &iter); found = TRUE; break; }
    }
    g_assert_true(found); wait_until(schema_preview_ready, dialog, "actual selected schema authenticated and planned");
}
static JsonObject *schema_first(JsonObject *prepared)
{ return json_array_get_object_element(json_object_get_array_member(prepared, "tracks"), 0); }
static void test_saved_weekly_schema_current_first_readonly_revalidate(void)
{
    SchemaFixture fixture = schema_fixture_new(); SchemaCapture capture = {0};
    GtkWidget *dialog = radios_player_rotation_open_schema(&fixture.harness->ctx, fixture.harness->page, fixture.current, schema_loaded, &capture);
    g_assert_true(GTK_IS_DIALOG(dialog)); SchemaListWait listed = {dialog, &fixture};
    wait_until(schema_list_ready, &listed, "all saved programme and Main schemas listed, ordinary rotations excluded");
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(control(dialog, "player-rotation-list"))); GtkTreeIter iter;
    for (gboolean valid = gtk_tree_model_get_iter_first(model, &iter); valid; valid = gtk_tree_model_iter_next(model, &iter)) {
        gchar *caption = NULL; gtk_tree_model_get(model, &iter, 0, &caption, -1);
        g_assert_nonnull(strstr(caption, " · Aynı adlı şema")); g_assert_null(strstr(caption, "Sıradan")); g_free(caption);
    }
    const gchar *weekly_id = radios_json_string(json_node_get_object(fixture.weekly_a), "_id", "");
    schema_select(dialog, weekly_id, "weekly_programme"); click(dialog, "player-rotation-load");
    wait_until(schema_captured, &capture, "real weekly schema callback after final document and media revalidation");
    g_assert_cmpuint(capture.called, ==, 1); g_assert_cmpstr(capture.document_id, ==, weekly_id);
    g_assert_cmpstr(capture.name, ==, "Aynı adlı şema"); JsonObject *prepared = json_node_get_object(capture.prepared);
    g_assert_cmpstr(radios_json_string(prepared, "rotation_document_kind", ""), ==, "weekly_programme");
    JsonArray *tracks = json_object_get_array_member(prepared, "tracks"); g_assert_cmpuint(json_array_get_length(tracks), >, 1);
    g_assert_cmpstr(radios_json_string(schema_first(prepared), "calendar_playlist_id", ""), ==, fixture.current);
    gboolean other_present = FALSE;
    for (guint i = 1; i < json_array_get_length(tracks); ++i)
        other_present |= !g_strcmp0(radios_json_string(json_array_get_object_element(tracks, i), "calendar_playlist_id", ""), fixture.other);
    g_assert_true(other_present);
    JsonObject *constraints = json_object_get_object_member(schema_first(prepared), "calendar_constraints");
    gint64 now = g_get_real_time(), earliest = radios_json_int(constraints, "earliest_utc_us", 0);
    g_assert_cmpint(earliest, >, now - 2 * G_TIME_SPAN_MINUTE); g_assert_cmpint(earliest, <=, now);
    schema_fixture_unchanged(&fixture);
    MediaImport revalidated = {0};
    g_assert_true(radios_player_rotation_prepare_loaded(&fixture.harness->ctx, fixture.harness->page, prepared,
        capture.document_id, capture.revision, capture.rotation_id, capture.client, media_imported, &revalidated, NULL));
    wait_until(media_done, &revalidated, "weekly kind survives loaded-player authentication");
    g_assert_no_error(revalidated.error); g_assert_nonnull(revalidated.result);
    g_assert_cmpstr(radios_json_string(json_node_get_object(revalidated.result), "rotation_document_kind", ""), ==, "weekly_programme");
    g_assert_cmpstr(radios_json_string(schema_first(json_node_get_object(revalidated.result)), "calendar_playlist_id", ""), ==, fixture.current);
    schema_fixture_unchanged(&fixture); json_node_free(revalidated.result); schema_capture_clear(&capture); schema_fixture_close(&fixture);
}
static void test_saved_weekly_schema_pinned_revision_rejects_changed_document(void)
{
    SchemaFixture fixture = schema_fixture_new(); SchemaCapture capture = {0};
    GtkWidget *dialog = radios_player_rotation_open_schema(&fixture.harness->ctx, fixture.harness->page, fixture.current, schema_loaded, &capture);
    SchemaListWait listed = {dialog, &fixture};
    wait_until(schema_list_ready, &listed, "complete schema list before stale revision test");
    const gchar *weekly_id = radios_json_string(json_node_get_object(fixture.weekly_b), "_id", "");
    schema_select(dialog, weekly_id, "weekly_programme");
    JsonNode *config_node = json_object_get_member(json_node_get_object(fixture.weekly_b), "config");
    JsonNode *won = weekly_put(fixture.weekly_b, "Başka istemci değiştirdi", config_node, NULL);
    click(dialog, "player-rotation-load");
    gint64 deadline = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (!strstr(gtk_label_get_text(GTK_LABEL(control(dialog, "player-rotation-status"))), "değişti") && g_get_monotonic_time() < deadline) pump();
    g_assert_cmpuint(capture.called, ==, 0); g_assert_false(gtk_widget_get_sensitive(control(dialog, "player-rotation-load")));
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(control(dialog, "player-rotation-status"))), "değişti"));
    JsonNode *actual = weekly_record(weekly_id, NULL); g_assert_true(json_node_equal(won, actual)); json_node_free(actual);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); pump(); json_node_free(won);
    schema_fixture_close(&fixture);
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *project_root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv); gdk_event_handler_set(fixture_event, NULL, NULL);
    GError *error = NULL; g_assert_true(radios_config_load(project_root, NULL, &config, &error));
    g_assert_no_error(error); g_free(project_root); reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/native-player-schema/saved-weekly-current-first-readonly-revalidate", test_saved_weekly_schema_current_first_readonly_revalidate);
    g_test_add_func("/native-player-schema/saved-weekly-pinned-revision-rejects-changed-document", test_saved_weekly_schema_pinned_revision_rejects_changed_document);
    gint result = g_test_run(); radios_data_client_unref(reader); radios_config_clear(&config); return result;
}
