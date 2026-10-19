#define _POSIX_C_SOURCE 200809L
#include <glib.h>

/* Reuse the real private station/GTK/Pulse fixture. Policy persistence and
 * startup merges are observed through public APIs and authenticated readback. */
static int loudness_policy_run(void);
#define main radios_loudness_fixture_main
#define g_test_run loudness_policy_run
#include "test_audio_ui.c"
#undef g_test_run
#undef main
#include "radios/preferences.h"

typedef struct {
    const RadioSAudioLoudnessPolicy *policy;
    gboolean enabled;
    gdouble target;
} PolicySaved;
static gboolean policy_saved(gpointer value)
{
    PolicySaved *expected = value; JsonNode *record = preferences_read();
    JsonObject *cfg = json_object_get_object_member(json_node_get_object(record), "config");
    JsonNode *wire = json_object_get_member(cfg, "loudness_policy"); RadioSAudioLoudnessPolicy actual;
    gboolean result = wire && JSON_NODE_HOLDS_OBJECT(wire) &&
        radios_audio_loudness_policy_from_json(&actual, json_node_get_object(wire), NULL) &&
        radios_audio_loudness_policy_equal(&actual, expected->policy) &&
        json_object_get_boolean_member(cfg, "loudness_normalize") == expected->enabled &&
        fabs(json_object_get_double_member(cfg, "loudness_target_lufs") - expected->target) < .00001;
    json_node_free(record); return result;
}
static RadioSAudioLoudnessPolicy selective_policy(void)
{
    RadioSAudioLoudnessPolicy policy; radios_audio_loudness_policy_default(&policy);
    policy.players = 1u | ((guint32)1 << 31); policy.contexts = RADIOS_AUDIO_LOUDNESS_MANUAL | RADIOS_AUDIO_LOUDNESS_AUTODJ;
    policy.phase = RADIOS_AUDIO_LOUDNESS_PHASE_TRANSITION; policy.automation_mode = RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_SELECTED;
    policy.automation_count = 2;
    g_strlcpy(policy.automation_ids[0], "23456789-abcd-4ef0-8123-456789abcdef", sizeof policy.automation_ids[0]);
    g_strlcpy(policy.automation_ids[1], "12345678-abcd-4ef0-8123-456789abcdef", sizeof policy.automation_ids[1]);
    policy.pairs_mode = RADIOS_AUDIO_LOUDNESS_PAIRS_SELECTED;
    policy.pairs[0] = (guint32)1 << 31; policy.pairs[31] = 1;
    policy.schedule_enabled = TRUE; g_strlcpy(policy.timezone, "Europe/Istanbul", sizeof policy.timezone);
    policy.window_count = 2;
    policy.windows[0] = (RadioSAudioLoudnessWindow){1u | (1u << 6), 1410, 90};
    policy.windows[1] = (RadioSAudioLoudnessWindow){1u << 1, 600, 600};
    g_assert_true(radios_audio_loudness_policy_validate(&policy, NULL)); return policy;
}
static void assert_policy(Window *window, const RadioSAudioLoudnessPolicy *expected, gboolean enabled, gdouble target)
{
    RadioSAudioLoudnessPolicy actual; gboolean current_enabled; gdouble current_target;
    g_assert_true(radios_audio_ui_loudness_policy_ready(window->ctx));
    g_assert_true(radios_audio_ui_loudness_policy(window->ctx, &actual));
    g_assert_true(radios_audio_loudness_policy_equal(&actual, expected));
    g_assert_true(radios_audio_ui_loudness(window->ctx, &current_enabled, &current_target));
    g_assert_cmpint(current_enabled, ==, enabled); g_assert_cmpfloat(current_target, ==, target);
}
static void save_remote_policy(const RadioSAudioLoudnessPolicy *policy, gboolean enabled, gdouble target)
{
    JsonNode *before = preferences_read(); JsonObject *cfg = json_object_get_object_member(json_node_get_object(before), "config");
    json_object_set_object_member(cfg, "loudness_policy", radios_audio_loudness_policy_to_json(policy));
    json_object_set_boolean_member(cfg, "loudness_normalize", enabled); json_object_set_double_member(cfg, "loudness_target_lufs", target);
    JsonNode *saved = preferences_remote_save(before, cfg); json_node_free(saved); json_node_free(before);
}
static Window policy_window_raw(void)
{
    Window window = {0}; GError *error = NULL; window.runtime = radios_runtime_new(64, 128);
    window.window = radios_window_new(application, &config, window.runtime, FALSE, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(window.window);
    window.ctx = radios_window_view_context(window.window); window.root = GTK_WIDGET(window.ctx->parent);
    g_assert_false(radios_audio_ui_loudness_policy_ready(window.ctx)); return window;
}
static void policy_window_ready(Window *window)
{
    radios_window_present(window->window);
    wait_until(window_ready, window, "first authenticated baseline and private audio outputs become ready");
}
static void policy_roundtrip_and_closing_recovery(void)
{
    Window window = window_open(); assert_gates_off(&window); RadioSAudioLoudnessPolicy original;
    g_assert_true(radios_audio_ui_loudness_policy(window.ctx, &original));
    /* Missing legacy scope is all players/contexts, with the verified station
     * zone; reading the legacy record itself must not save a migration. */
    g_assert_true(radios_audio_loudness_policy_unrestricted(&original));
    g_assert_cmpstr(original.timezone, ==, radios_data_client_station_timezone(reader));
    JsonNode *before = preferences_read();
    g_assert_false(json_object_has_member(json_object_get_object_member(json_node_get_object(before), "config"), "loudness_policy"));
    gboolean enabled; gdouble target; g_assert_true(radios_audio_ui_loudness(window.ctx, &enabled, &target));
    RadioSAudioLoudnessPolicy policy = selective_policy(); GError *error = NULL;
    g_assert_true(radios_audio_ui_set_loudness_policy(window.ctx, &policy, &error)); g_assert_no_error(error);
    PolicySaved expected = {&policy, enabled, target}; wait_until(policy_saved, &expected, "all bounded LUFS filters persist through the real CAS");
    JsonNode *saved = preferences_read(); JsonObject *old_cfg = json_object_get_object_member(json_node_get_object(before), "config");
    JsonObject *new_cfg = json_object_get_object_member(json_node_get_object(saved), "config");
    const gchar *untouched[] = {"main_sink", "cue_sink", "monitor_sink", "microphone_source", "main_gain_db", "microphone_gain_db"};
    for (guint i = 0; i < G_N_ELEMENTS(untouched); ++i)
        g_assert_true(json_node_equal(json_object_get_member(old_cfg, untouched[i]), json_object_get_member(new_cfg, untouched[i])));
    json_node_free(before); json_node_free(saved);
    g_assert_true(radios_audio_ui_set_loudness(window.ctx, FALSE, -18.5, &error)); g_assert_no_error(error);
    expected.enabled = FALSE; expected.target = -18.5; wait_until(policy_saved, &expected, "global target edit preserves all scope filters");
    window_close(&window); window = window_open(); assert_policy(&window, &policy, FALSE, -18.5);
    /* Close before debounce: the newest intent must survive owner teardown,
     * including explicit empty selection and an enabled empty schedule. */
    policy.players = 0; policy.contexts = 0; policy.automation_count = 0;
    memset(policy.automation_ids, 0, sizeof policy.automation_ids); memset(policy.pairs, 0, sizeof policy.pairs);
    policy.window_count = 0; memset(policy.windows, 0, sizeof policy.windows);
    g_assert_true(radios_audio_ui_set_loudness_policy(window.ctx, &policy, &error)); g_assert_no_error(error);
    window_close(&window); window = window_open();
    wait_until(policy_saved, &expected, "latest empty LUFS policy is acknowledged after private close/reopen recovery");
    assert_policy(&window, &policy, FALSE, -18.5); window_close(&window);
}
static void policy_first_read_merges_independent_fields(void)
{
    Window initialized = window_open(); window_close(&initialized);
    RadioSAudioLoudnessPolicy saved_scope = selective_policy(); save_remote_policy(&saved_scope, TRUE, -12.5);
    Window window = policy_window_raw(); GError *error = NULL;
    g_assert_true(radios_audio_ui_set_loudness(window.ctx, FALSE, -18, &error)); g_assert_no_error(error);
    policy_window_ready(&window); assert_policy(&window, &saved_scope, FALSE, -18);
    PolicySaved expected = {&saved_scope, FALSE, -18}; wait_until(policy_saved, &expected, "early enable/target edits retain saved policy");
    window_close(&window);
    RadioSAudioLoudnessPolicy remote_scope; radios_audio_loudness_policy_default(&remote_scope);
    save_remote_policy(&remote_scope, TRUE, -11.5);
    saved_scope.players = (guint32)1 << 7; saved_scope.contexts = RADIOS_AUDIO_LOUDNESS_AUTOMATION;
    saved_scope.automation_mode = RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_ASSIGNED;
    window = policy_window_raw();
    g_assert_true(radios_audio_ui_set_loudness_policy(window.ctx, &saved_scope, &error)); g_assert_no_error(error);
    policy_window_ready(&window); assert_policy(&window, &saved_scope, TRUE, -11.5);
    expected = (PolicySaved){&saved_scope, TRUE, -11.5}; wait_until(policy_saved, &expected, "early scope edit retains saved enable/target baseline");
    window_close(&window);
}
static void policy_invalid_edit_keeps_state_and_revision(void)
{
    Window window = window_open(); RadioSAudioLoudnessPolicy good;
    g_assert_true(radios_audio_ui_loudness_policy(window.ctx, &good));
    JsonNode *before = preferences_read(); gint64 revision = radios_json_int(json_node_get_object(before), "_rev", 0);
    gboolean enabled; gdouble target; radios_audio_ui_loudness(window.ctx, &enabled, &target);
    RadioSAudioLoudnessPolicy invalid = good; invalid.contexts = 8; GError *error = NULL;
    g_assert_false(radios_audio_ui_set_loudness_policy(window.ctx, &invalid, &error)); g_assert_nonnull(error); g_clear_error(&error);
    invalid = good; invalid.pairs[0] = 1;
    g_assert_false(radios_audio_ui_set_loudness_policy(window.ctx, &invalid, &error)); g_assert_nonnull(error); g_clear_error(&error);
    invalid = good; g_strlcpy(invalid.timezone, "Invalid/Zone", sizeof invalid.timezone);
    g_assert_false(radios_audio_ui_set_loudness_policy(window.ctx, &invalid, &error)); g_assert_nonnull(error); g_clear_error(&error);
    assert_policy(&window, &good, enabled, target); run_for(400);
    JsonNode *after = preferences_read();
    g_assert_cmpint(radios_json_int(json_node_get_object(after), "_rev", 0), ==, revision);
    g_assert_true(json_node_equal(json_object_get_member(json_node_get_object(before), "config"), json_object_get_member(json_node_get_object(after), "config")));
    json_node_free(after); json_node_free(before); window_close(&window);
}

static JsonNode *policy_catalog_document(const gchar *kind, const gchar *name, JsonObject *configuration)
{
    JsonNode *body = body_new(); JsonObject *object = json_node_get_object(body);
    json_object_set_string_member(object, "kind", kind); json_object_set_string_member(object, "name", name);
    json_object_set_object_member(object, "config", json_object_ref(configuration));
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    JsonNode *saved = request("/v1/workflows/desktop/put", body); json_node_free(body); return saved;
}
static JsonNode *policy_catalog_automation(const gchar *name)
{
    JsonObject *cfg = json_object_new();
    json_object_set_string_member(cfg, "name", "Gerçek AutoDJ şablon ayarları");
    json_object_set_string_member(cfg, "seed", "native-LUFS-catalogue");
    json_object_set_string_member(cfg, "planDate", "2026-10-13");
    const gchar *times[] = {"startTime", "windowStart", "windowEnd"};
    for (guint i = 0; i < G_N_ELEMENTS(times); ++i) json_object_set_string_member(cfg, times[i], i == 2 ? "23:59" : "00:00");
    const gchar *zeros[] = {"sourceMode", "trackMode", "transition", "boundaryTransition", "deckMode", "fallbackMode", "trackGap", "artistGap", "albumGap"};
    for (guint i = 0; i < G_N_ELEMENTS(zeros); ++i) json_object_set_int_member(cfg, zeros[i], 0);
    json_object_set_int_member(cfg, "previewCount", 1); json_object_set_int_member(cfg, "jingleEvery", 1);
    json_object_set_double_member(cfg, "overlap", 0); json_object_set_double_member(cfg, "boundaryOverlap", 0);
    const gchar *flags[] = {"includeProgramme", "jingleEnabled", "jingleDry", "requireMetadata"};
    for (guint i = 0; i < G_N_ELEMENTS(flags); ++i) json_object_set_boolean_member(cfg, flags[i], FALSE);
    const gchar *nullable[] = {"jinglePlaylist", "fallbackPlaylist", "jingleDeck"};
    for (guint i = 0; i < G_N_ELEMENTS(nullable); ++i) json_object_set_null_member(cfg, nullable[i]);
    JsonArray *days = json_array_new(), *decks = json_array_new();
    for (guint i = 1; i <= 7; ++i) json_array_add_int_element(days, i);
    json_array_add_string_element(decks, "1"); json_array_add_string_element(decks, "2");
    json_object_set_array_member(cfg, "weekdays", days); json_object_set_array_member(cfg, "decks", decks);
    json_object_set_array_member(cfg, "sources", json_array_new());
    JsonNode *record = policy_catalog_document("automation", name, cfg); json_object_unref(cfg); return record;
}
typedef struct {
    GtkWidget *preferences;
    const gchar *ids[3], *names[3];
} PolicyCatalog;
static gboolean policy_catalog_ready(gpointer value)
{
    PolicyCatalog *expected = value; GtkWidget *tree = find(expected->preferences, "preferences-loudness-automation-list");
    g_assert_true(GTK_IS_TREE_VIEW(tree)); GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree));
    gboolean seen[3] = {FALSE}; GtkTreeIter row;
    for (gboolean valid = gtk_tree_model_get_iter_first(model, &row); valid; valid = gtk_tree_model_iter_next(model, &row)) {
        gchar *id = NULL, *name = NULL; gtk_tree_model_get(model, &row, 1, &id, 2, &name, -1);
        for (guint i = 0; i < 3; ++i) if (!g_strcmp0(id, expected->ids[i])) seen[i] = !g_strcmp0(name, expected->names[i]);
        g_free(id); g_free(name);
    }
    return seen[0] && seen[1] && seen[2];
}
static void policy_catalog_toggle(GtkWidget *preferences, const gchar *wanted)
{
    GtkWidget *tree = find(preferences, "preferences-loudness-automation-list"); g_assert_true(GTK_IS_TREE_VIEW(tree));
    g_assert_true(gtk_widget_is_sensitive(tree)); GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree)); GtkTreeIter row;
    gboolean found = FALSE;
    for (gboolean valid = gtk_tree_model_get_iter_first(model, &row); valid; valid = gtk_tree_model_iter_next(model, &row)) {
        gchar *id = NULL; gtk_tree_model_get(model, &row, 1, &id, -1); gboolean matches = !g_strcmp0(id, wanted); g_free(id);
        if (!matches) continue;
        GtkTreePath *path = gtk_tree_model_get_path(model, &row); gchar *index = gtk_tree_path_to_string(path);
        GtkTreeViewColumn *column = gtk_tree_view_get_column(GTK_TREE_VIEW(tree), 0);
        GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(column)); g_assert_nonnull(cells);
        g_assert_true(GTK_IS_CELL_RENDERER_TOGGLE(cells->data));
        /* Emit the actual GTK renderer's interaction signal so the production
         * callback edits the UUID selection, including its live model lifetime. */
        g_signal_emit_by_name(cells->data, "toggled", index); g_list_free(cells); g_free(index); gtk_tree_path_free(path);
        found = TRUE; break;
    }
    g_assert_true(found);
}
static void production_catalogue_names_selection_persistence(void)
{
    const gchar *document_names[] = {"LUFS gerçek otomasyon şablonu", "LUFS gerçek haftalık program", "LUFS gerçek takvim ve rotasyon"};
    JsonNode *documents[3] = {policy_catalog_automation(document_names[0]), NULL, NULL};
    JsonObject *calendar = json_object_new(); json_object_set_int_member(calendar, "schema", 1);
    json_object_set_array_member(calendar, "events", json_array_new()); json_object_set_array_member(calendar, "rotation_library", json_array_new());
    json_object_set_string_member(calendar, "timezone", "Europe/Istanbul");
    JsonObject *weekly = json_object_new(); json_object_set_int_member(weekly, "schema", 1);
    json_object_set_string_member(weekly, "week_anchor", "2026-10-05"); json_object_set_int_member(weekly, "week_count", 1);
    json_object_set_boolean_member(weekly, "repeat", FALSE); json_object_set_object_member(weekly, "calendar", json_object_ref(calendar));
    documents[1] = policy_catalog_document("weekly_programme", document_names[1], weekly);
    documents[2] = policy_catalog_document("automation_calendar", document_names[2], calendar);
    json_object_unref(weekly); json_object_unref(calendar);
    Window window = window_open(); assert_gates_off(&window);
    JsonNode *baseline = preferences_read(); gint64 previous_revision = radios_json_int(json_node_get_object(baseline), "_rev", 0); json_node_free(baseline);
    GtkWidget *preferences = radios_preferences_open(window.ctx); g_assert_true(GTK_IS_WINDOW(preferences));
    GtkWidget *categories = find(preferences, "preferences-categories"), *audio = find(preferences, "preferences-category-audio");
    g_assert_true(GTK_IS_LIST_BOX(categories)); g_assert_true(GTK_IS_LIST_BOX_ROW(audio));
    gtk_list_box_select_row(GTK_LIST_BOX(categories), GTK_LIST_BOX_ROW(audio));
    gtk_notebook_set_current_page(GTK_NOTEBOOK(find(preferences, "preferences-notebook")), 0);
    GtkWidget *scope = find(preferences, "preferences-loudness-scope"); g_assert_true(GTK_IS_EXPANDER(scope)); gtk_expander_set_expanded(GTK_EXPANDER(scope), TRUE);
    PolicyCatalog catalog = {preferences, {NULL, NULL, NULL}, {"Otomasyon · LUFS gerçek otomasyon şablonu", "Program · LUFS gerçek haftalık program", "Takvim / rotasyon · LUFS gerçek takvim ve rotasyon"}};
    for (guint i = 0; i < 3; ++i) catalog.ids[i] = radios_json_string(json_node_get_object(documents[i]), "_id", "");
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!policy_catalog_ready(&catalog) && g_get_monotonic_time() < deadline) pump();
    if (!policy_catalog_ready(&catalog)) {
        GtkWidget *status = find(preferences, "preferences-loudness-scope-status");
        g_error("Production LUFS catalogue did not show three genuine saved names/UUIDs: %s", gtk_label_get_text(GTK_LABEL(status)));
    }
    combo(preferences, "preferences-loudness-automations-mode", "selected");
    RadioSAudioLoudnessPolicy expected; g_assert_true(radios_audio_ui_loudness_policy(window.ctx, &expected));
    g_assert_cmpint(expected.automation_mode, ==, RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_SELECTED); g_assert_cmpuint(expected.automation_count, ==, 0);
    for (guint i = 0; i < 3; ++i) policy_catalog_toggle(preferences, catalog.ids[i]);
    g_assert_true(radios_audio_ui_loudness_policy(window.ctx, &expected)); g_assert_cmpuint(expected.automation_count, ==, 3);
    for (guint i = 0; i < 3; ++i) g_assert_cmpstr(expected.automation_ids[i], ==, catalog.ids[i]);
    gboolean enabled; gdouble target; g_assert_true(radios_audio_ui_loudness(window.ctx, &enabled, &target));
    PolicySaved saved = {&expected, enabled, target}; wait_until(policy_saved, &saved, "production UUID checkboxes persist all three real document kinds");
    JsonNode *record = preferences_read(); g_assert_cmpint(radios_json_int(json_node_get_object(record), "_rev", 0), >, previous_revision); json_node_free(record);
    g_assert_true(policy_catalog_ready(&catalog));
    /* Close only the owned private dialog after another genuine refresh. Late
     * response completion must respect its GTK owner and release JSON safely. */
    click(preferences, "preferences-loudness-automation-refresh"); gtk_widget_destroy(preferences); run_for(200);
    window_close(&window);
    for (guint i = 0; i < 3; ++i) json_node_free(documents[i]);
    g_test_message("Preferences→Ses production HTTP catalogue shows saved automation/program/calendar names and kinds; real checkbox signals persist the exact UUIDs with a newer CAS revision.");
}

/* A catalog audition (Ön dinleme on CUE) hears the stored LUFS correction
 * like a player does; switching normalization off restores the file level. */
static void preview_applies_stored_measurement(void)
{
    Window window = window_open(); JsonNode *detail = media_import(&window); GError *error = NULL;
    RadioSLoudness measured;
    g_assert_true(radios_loudness_read(json_object_get_object_member(json_node_get_object(detail), "analysis"), &measured));
    g_assert_true(measured.measured);
    gdouble expected = radios_loudness_gain_db(&measured, -14); g_assert_cmpfloat(expected, <, -3);
    GtkWidget *owner = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(owner);
    require_call(radios_audio_ui_preview(window.ctx, owner, json_node_get_object(detail), 0, 0, &error), &error);
    deck_wait(&window, RADIOS_AUDIO_PREVIEW_DECK, RADIOS_AUDIO_UI_PLAYING, 200);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); RadioSAudioSnapshot snapshot;
    radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_true(snapshot.decks[RADIOS_AUDIO_PREVIEW_DECK].loudness_measured);
    g_assert_cmpfloat(fabs(snapshot.decks[RADIOS_AUDIO_PREVIEW_DECK].loudness_lufs - measured.integrated_lufs), <, 1e-9);
    g_assert_cmpfloat(fabs(snapshot.decks[RADIOS_AUDIO_PREVIEW_DECK].loudness_gain_db - expected), <, 1e-6);
    run_for(150); goffset cue = capture_offset("CUE"); run_for(300); gdouble peak = 0, normalized = capture_rms("CUE", cue, &peak);
    require_call(radios_audio_ui_set_loudness(window.ctx, FALSE, -14, &error), &error);
    run_for(150); cue = capture_offset("CUE"); run_for(300); gdouble raw = capture_rms("CUE", cue, &peak);
    g_test_message("CUE preview RMS %.4f normalized, %.4f at file level, %.2f dB (expected %.2f dB)", normalized, raw, 20 * log10(normalized / raw), expected);
    g_assert_cmpfloat(fabs(20 * log10(normalized / raw) - expected), <, .5);
    require_call(radios_audio_ui_set_loudness(window.ctx, TRUE, -14, &error), &error);
    radios_audio_ui_preview_stop(window.ctx, owner); gtk_widget_destroy(owner); g_object_unref(owner);
    radios_audio_engine_unref(engine); json_node_free(detail); window_close(&window);
}
static int loudness_policy_run(void)
{
    g_test_add_func("/native-loudness-policy/preview-applies-stored-measurement", preview_applies_stored_measurement);
    g_test_add_func("/native-loudness-policy/roundtrip-close-recovery", policy_roundtrip_and_closing_recovery);
    g_test_add_func("/native-loudness-policy/first-read-independent-merge", policy_first_read_merges_independent_fields);
    g_test_add_func("/native-loudness-policy/invalid-edit-preserves-revision", policy_invalid_edit_keeps_state_and_revision);
    g_test_add_func("/native-loudness-policy/production-catalogue-names-selection-persistence", production_catalogue_names_selection_persistence);
    return g_test_run();
}
int main(int argc, char **argv) { return radios_loudness_fixture_main(argc, argv); }
