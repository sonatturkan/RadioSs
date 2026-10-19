/* Kullanıcı isteği U2 (18.10.2026): "Otomasyon ▾ › Şemadan yükle…" lists the
 * songs the schema selects now; the chosen one loads into the player exactly
 * like "Kütüphaneden yükle". Read only: the schema record stays the same and
 * no automation starts. Real authenticated media and isolated MAIN PCM. */
#define main radios_schema_track_picker_legacy_fixture_main
#include "test_playout_ui.c"
#undef main
#include "radios/schema_track_picker.h"

/* A weekly schema whose block holds the current UTC minute. */
static JsonNode *picker_schedule(JsonNode *playlist, const gchar *title)
{
    GDateTime *start = g_date_time_new_now_utc();
    gchar *date = g_date_time_format(start, "%Y-%m-%d");
    guint minute = (guint)(g_date_time_get_hour(start) * 60 + g_date_time_get_minute(start));
    JsonNode *calendar = calendar_schedule_config(date, radios_json_string(json_node_get_object(playlist), "_id", ""), 0);
    JsonObject *config = json_node_get_object(calendar);
    JsonObject *event = json_array_get_object_element(json_object_get_array_member(config, "events"), 0);
    json_object_set_int_member(event, "start_minute", minute); json_object_set_int_member(event, "end_minute", MIN(minute + 30, 1440u));
    json_object_set_string_member(event, "title", title);
    JsonObject *definition = json_array_get_object_element(json_object_get_array_member(config, "rotation_library"), 0);
    json_object_set_int_member(json_array_get_object_element(json_object_get_array_member(definition, "sources"), 0), "interval_minutes", 0);
    GDateTime *monday = g_date_time_add_days(start, 1 - g_date_time_get_day_of_week(start));
    gchar *anchor = g_date_time_format(monday, "%Y-%m-%d"); GError *error = NULL;
    JsonNode *programme = radios_weekly_programme_snapshot(config, anchor, FALSE, &error);
    g_assert_no_error(error); g_assert_nonnull(programme);
    g_free(anchor); g_date_time_unref(monday); json_node_free(calendar); g_free(date); g_date_time_unref(start);
    return programme;
}
typedef struct { GtkWidget *combo; const gchar *id; } ComboEntry;
static gboolean combo_lists(gpointer value)
{
    ComboEntry *entry = value; GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(entry->combo)); GtkTreeIter iter;
    gint column = gtk_combo_box_get_id_column(GTK_COMBO_BOX(entry->combo)); gboolean found = FALSE;
    for (gboolean more = model && gtk_tree_model_get_iter_first(model, &iter); more && !found; more = gtk_tree_model_iter_next(model, &iter)) {
        gchar *id = NULL; gtk_tree_model_get(model, &iter, column, &id, -1); found = !g_strcmp0(id, entry->id); g_free(id);
    }
    return found;
}
static gint picker_count(GtkWidget *tree)
{ GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree)); return model ? gtk_tree_model_iter_n_children(model, NULL) : 0; }
static gboolean picker_has_rows(gpointer tree) { return picker_count(tree) > 0; }
static gchar *picker_cell(GtkWidget *tree, gint row, gint column)
{
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree)); GtkTreeIter iter; gchar *text = NULL;
    if (model && gtk_tree_model_iter_nth_child(model, &iter, NULL, row)) gtk_tree_model_get(model, &iter, column, &text, -1);
    return text;
}
typedef struct { GtkWidget *tree; const gchar *needle; } SearchCondition;
static gboolean picker_filtered(gpointer value)
{
    SearchCondition *condition = value; gint count = picker_count(condition->tree);
    for (gint i = 0; i < count; ++i) { gchar *title = picker_cell(condition->tree, i, 1); gboolean match = title && strstr(title, condition->needle); g_free(title); if (!match) return FALSE; }
    return count > 0;
}
static gboolean picker_closed(gpointer unused) { (void)unused; return dialog_find("schema-track-picker-dialog") == NULL; }
typedef struct { RadioSPlayerWorkspace *workspace; const gchar *id; } Assigned;
static gboolean schema_assigned(gpointer value)
{
    Assigned *assigned = value;
    return g_str_equal(radios_player_topology_automation(radios_player_workspace_topology(assigned->workspace), 0), assigned->id);
}
/* Optional PNG of the window for visual review (RADIOS_SCHEMA_PICKER_CAPTURE). */
static void picker_capture(GtkWidget *dialog)
{
    const gchar *path = g_getenv("RADIOS_SCHEMA_PICKER_CAPTURE"); if (!path || !*path) return;
    run_for(300); GdkWindow *surface = gtk_widget_get_window(dialog); if (!surface) return;
    GdkPixbuf *pixbuf = gdk_pixbuf_get_from_window(surface, 0, 0, gdk_window_get_width(surface), gdk_window_get_height(surface));
    if (pixbuf) { g_assert_true(gdk_pixbuf_save(pixbuf, path, "png", NULL, NULL)); g_object_unref(pixbuf); }
}
static GtkWidget *picker_open(Window *window)
{
    GtkWidget *card = g_object_get_data(G_OBJECT(window->ctx->parent), "radios-deck-owner-0"); g_assert_nonnull(card);
    GtkWidget *item = find(card, "player-automation-schema-tracks"); g_assert_true(GTK_IS_BUTTON(item));
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(item)), ==, "Şemadan yükle…");
    gtk_button_clicked(GTK_BUTTON(item)); pump();
    GtkWidget *dialog = dialog_find("schema-track-picker-dialog"); g_assert_nonnull(dialog); return dialog;
}
static void test_schema_track_picker_load_pcm(void)
{
    Window window = window_open(); players_show(&window);
    wait_until(scope_workspace_ready, window.ctx->players, "picker player definitions");
    JsonNode *first = media_import(&window, "Şema parçası A", 600, .2, "music");
    JsonNode *second = media_import(&window, "Şema parçası B", 900, .2, "music");
    JsonNode *playlist = playlist_create("Şema parça listesi", first, second);
    JsonNode *configuration = picker_schedule(playlist, "Şema parça kuşağı");
    JsonNode *saved = document_save("weekly_programme", "Şemadan yükle şeması", json_node_get_object(configuration));
    const gchar *schema_id = radios_json_string(json_node_get_object(saved), "_id", "");
    /* Player 1 has no schema: nothing is listed until one is chosen. */
    GtkWidget *dialog = picker_open(&window);
    GtkWidget *schema = find(dialog, "schema-track-picker-schema"), *tree = find(dialog, "schema-track-picker-list");
    GtkWidget *search = find(dialog, "schema-track-picker-search"), *load = find(dialog, "schema-track-picker-load");
    GtkWidget *status = find(dialog, "schema-track-picker-status");
    ComboEntry entry = {schema, schema_id}; wait_until(combo_lists, &entry, "saved schemas listed");
    g_assert_cmpint(picker_count(tree), ==, 0); g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(status)), ==, "Şema seçin.");
    g_assert_false(gtk_widget_is_sensitive(load));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(schema), schema_id));
    wait_until(picker_has_rows, tree, "schema selection listed");
    gchar *number = picker_cell(tree, 0, 0), *title = picker_cell(tree, 0, 1), *next = picker_cell(tree, 1, 1);
    gchar *length = picker_cell(tree, 0, 2), *kind = picker_cell(tree, 0, 3), *start = picker_cell(tree, 0, 4);
    g_test_message("Picker: %d rows, first '%s' %s %s %s", picker_count(tree), title, length, kind, start);
    g_assert_cmpstr(number, ==, "1"); g_assert_cmpstr(kind, ==, "Müzik");
    g_assert_true(g_str_equal(title, "Şema parçası A") || g_str_equal(title, "Şema parçası B"));
    g_assert_true(next && g_strcmp0(title, next)); /* a song never follows itself */
    g_assert_true(g_str_equal(length, "00:06") || g_str_equal(length, "00:05"));
    g_assert_true(g_regex_match_simple("^[0-2][0-9]:[0-5][0-9]:[0-5][0-9]$", start, 0, 0));
    g_assert_true(g_str_has_prefix(gtk_label_get_text(GTK_LABEL(status)), "Şema parça kuşağı · "));
    g_free(number); g_free(title); g_free(next); g_free(length); g_free(kind); g_free(start);
    picker_capture(dialog);
    /* Search narrows the list; the chosen song loads like "Kütüphaneden yükle". */
    gtk_entry_set_text(GTK_ENTRY(search), "parçası B");
    SearchCondition filtered = {tree, "Şema parçası B"}; wait_until(picker_filtered, &filtered, "search shows only song B");
    GtkTreeIter iter; GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(tree));
    g_assert_true(gtk_tree_model_get_iter_first(model, &iter));
    gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(tree)), &iter);
    wait_until(widget_enabled, load, "load enabled for a chosen song"); gtk_button_clicked(GTK_BUTTON(load));
    wait_until(picker_closed, NULL, "picker closes after loading");
    RadioSAudioUiSnapshot loaded = deck_wait(&window, 0, RADIOS_AUDIO_UI_READY, 0);
    g_assert_cmpstr(loaded.media_id, ==, radios_json_string(json_object_get_object_member(json_node_get_object(second), "media"), "_id", ""));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(window.root, "deck-0-title"))), ==, "Şema parçası B");
    /* Read only: same schema record, no automation run on the player. */
    weekly_record_unchanged(saved);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); RadioSAudioRunSnapshot run;
    radios_audio_engine_player_runner_snapshot(engine, 0, &run); g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_IDLE);
    GError *error = NULL; require_call(radios_audio_ui_play(window.ctx, 0, &error), &error);
    deck_wait(&window, 0, RADIOS_AUDIO_UI_PLAYING, 100);
    goffset at = capture_offset("MAIN"); run_for(150);
    g_assert_cmpfloat(tone_energy("MAIN", at, 900), >, .06);
    require_call(radios_audio_ui_pause(window.ctx, 0, &error), &error); deck_wait(&window, 0, RADIOS_AUDIO_UI_PAUSED, 0);
    /* The player's own schema is listed at once; Vazgeç changes nothing. */
    RadioSAutomationScope player = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 0};
    require_call(radios_player_workspace_assign(window.ctx->players, &player, schema_id, &error), &error);
    Assigned assigned = {window.ctx->players, schema_id}; wait_until(schema_assigned, &assigned, "schema assigned to player 1");
    RadioSAudioUiSnapshot before; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 0, &before));
    dialog = picker_open(&window); schema = find(dialog, "schema-track-picker-schema"); tree = find(dialog, "schema-track-picker-list");
    wait_until(picker_has_rows, tree, "assigned schema listed without a choice");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(schema)), ==, schema_id);
    gtk_dialog_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL); wait_until(picker_closed, NULL, "picker closed by Vazgeç");
    RadioSAudioUiSnapshot after; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 0, &after));
    g_assert_cmpuint(after.generation, ==, before.generation); g_assert_cmpstr(after.media_id, ==, before.media_id);
    radios_audio_engine_player_runner_snapshot(engine, 0, &run); g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_IDLE);
    weekly_record_unchanged(saved);
    radios_audio_engine_unref(engine);
    json_node_free(saved); json_node_free(configuration); json_node_free(playlist); json_node_free(second); json_node_free(first);
    window_close(&window);
    g_test_message("Şemadan yükle: unassigned player lists after a choice, assigned player at once; search filters; the chosen song loads and plays on real MAIN PCM; schema record and player automation unchanged.");
}
int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    main_sink = g_getenv("RADIOS_TEST_MAIN_SINK"); cue_sink = g_getenv("RADIOS_TEST_CUE_SINK"); monitor_sink = g_getenv("RADIOS_TEST_MONITOR_SINK");
    g_assert_nonnull(main_sink); g_assert_nonnull(cue_sink); g_assert_nonnull(monitor_sink);
    GError *error = NULL; g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    application = gtk_application_new("org.radios.NativeSchemaTrackPicker", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error); g_assert_nonnull(reader);
    g_test_add_func("/native-playout/schema-track-picker-load-readonly-PCM", test_schema_track_picker_load_pcm);
    gint result = g_test_run(); radios_data_client_unref(reader); g_object_unref(application); radios_config_clear(&config); return result;
}
