#define _POSIX_C_SOURCE 200809L
#include <glib.h>
static int event_playout_test_run(void);
static void event_fixture_ignore(const char *path, GTestFunc test) { (void)path; (void)test; }
#define main radios_event_playout_fixture_main
#define g_test_run event_playout_test_run
#define g_test_add_func event_fixture_ignore
#include "test_playout_ui.c"
#undef g_test_add_func
#undef g_test_run
#undef main
#include "radios/event_playout.h"
#include "radios/local_event_ui.h"

static void event_document_unchanged(const gchar *kind, const gchar *name, JsonNode *expected)
{
    JsonNode *actual = document_read(kind, name); g_assert_nonnull(actual);
    g_assert_true(json_node_equal(expected, actual)); json_node_free(actual);
}
static JsonObject *event_recipe(const gchar *title, JsonNode *playlist, JsonNode *weekly)
{
    JsonObject *recipe = json_object_new(); JsonArray *sources = json_array_new();
    json_object_set_int_member(recipe, "schema", 1); json_object_set_string_member(recipe, "title", title);
    json_object_set_string_member(recipe, "kind", "music"); json_object_set_array_member(recipe, "sources", sources);
    JsonNode *records[] = {playlist, weekly}; const gchar *types[] = {"playlist", "weekly_programme"};
    for (guint i = 0; i < G_N_ELEMENTS(records); ++i) {
        if (!records[i]) continue;
        JsonObject *source = json_object_new(); gchar *id = g_uuid_string_random();
        json_object_set_string_member(source, "id", id); g_free(id);
        json_object_set_string_member(source, "type", types[i]);
        json_object_set_string_member(source, "source_id", radios_json_string(json_node_get_object(records[i]), "_id", ""));
        json_array_add_object_element(sources, source);
    }
    return recipe;
}
typedef struct { Window *window; RadioSAutomationScope scope; JsonObject *programme; } EventQueue;
static gboolean event_queue_set(gpointer value)
{
    EventQueue *queue = value; GError *error = NULL;
    gboolean accepted = radios_player_workspace_set_programme(queue->window->ctx->players, &queue->scope, queue->programme, &error);
    if (!accepted) g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PENDING);
    g_clear_error(&error); return accepted;
}
static gboolean event_queue_saved(gpointer value)
{
    EventQueue *queue = value; GError *error = NULL;
    gboolean saved = radios_player_workspace_programme_saved(queue->window->ctx->players, &queue->scope, &error);
    if (!saved) g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PENDING);
    g_clear_error(&error); return saved;
}
static JsonObject *event_store_queue(Window *window, const RadioSAutomationScope *scope, JsonObject *programme)
{
    EventQueue queue = {window, *scope, programme};
    wait_until(event_queue_set, &queue, "isolated saved scope queue baseline ready");
    wait_until(event_queue_saved, &queue, "isolated scope queue has durable CAS acknowledgement");
    JsonObject *stored = NULL; GError *error = NULL;
    require_call(radios_player_workspace_get_programme(window->ctx->players, scope, &stored, &error), &error);
    return stored;
}
static void event_queue_unchanged(Window *window, const RadioSAutomationScope *scope, JsonObject *expected)
{
    JsonObject *actual = NULL; GError *error = NULL;
    require_call(radios_player_workspace_get_programme(window->ctx->players, scope, &actual, &error), &error);
    JsonNode *a = json_node_new(JSON_NODE_OBJECT), *b = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(a, actual); json_node_set_object(b, expected);
    g_assert_true(json_node_equal(a, b)); json_node_free(a); json_node_free(b); json_object_unref(actual);
}
static RadioSAudioRunSnapshot event_runner(Window *window, guint source)
{
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window->ctx); RadioSAudioRunSnapshot run;
    radios_audio_engine_player_runner_snapshot(engine, source, &run); radios_audio_engine_unref(engine); return run;
}
static void event_assert_target_idle(Window *window, guint source)
{
    RadioSAudioRunSnapshot run = event_runner(window, source); g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_IDLE);
    for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) g_assert_false(run.owned[deck]);
}
static void event_assert_complete(RadioSAudioEngine *engine, guint source, guint count)
{
    RadioSAudioRunSnapshot finished = player_run_wait(engine, source, RADIOS_AUDIO_RUN_FINISHED, count - 1, 0);
    g_assert_cmpuint(finished.count, ==, count);
    for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) g_assert_false(finished.owned[deck]);
    RadioSAudioRunReceipt receipts[32]; guint n = radios_audio_engine_player_runner_receipts(engine, source, receipts, G_N_ELEMENTS(receipts));
    guint complete = 0;
    for (guint i = 0; i < n; ++i) if (receipts[i].epoch == finished.epoch) {
        g_assert_cmpint(receipts[i].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_COMPLETE); ++complete;
    }
    g_assert_cmpuint(complete, ==, count);
}
static void event_stop_scope(Window *window, const RadioSAutomationScope *scope)
{
    GError *error = NULL;
    require_call(radios_player_scope_execution_action(radios_player_scope_execution_get(window->ctx), scope, RADIOS_PLAYOUT_STOP, &error), &error);
    run_for(200);
}
typedef struct { GtkWidget *dialog; const gchar *id, *title; } EventRecipeChoice;
static gboolean event_recipe_available(gpointer value)
{
    EventRecipeChoice *choice = value;
    return gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(choice->dialog, "local-event-saved")), choice->id);
}
static gboolean event_recipe_loaded(gpointer value)
{
    EventRecipeChoice *choice = value;
    return !g_strcmp0(gtk_entry_get_text(GTK_ENTRY(find(choice->dialog, "local-event-name"))), choice->title) &&
        gtk_widget_is_sensitive(find(choice->dialog, "local-event-play"));
}
static gboolean event_semi_label_ready(gpointer value)
{
    Window *window = value; GtkWidget *label = find(window->root, "deck-5-autodj-state");
    return GTK_IS_LABEL(label) && strstr(gtk_label_get_text(GTK_LABEL(label)), "Yarı AutoDJ") &&
        strstr(gtk_label_get_text(GTK_LABEL(label)), "Event");
}
static gboolean event_service_idle(gpointer ctx)
{ return !radios_event_playout_busy(ctx, G_MAXUINT); }
static void test_local_event_automatic_unclustered_semi_preserves_all_queues(void)
{
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "event topology baseline");
    JsonNode *background = media_import_rhythm(&window, "Event dışında canlı yayın", 700, .2, "music", 120);
    JsonNode *a = routing_short_markers(media_import(&window, "Yerel event A", 1700, .22, "music"), 1500);
    JsonNode *b = routing_short_markers(media_import(&window, "Yerel event B", 2350, .22, "music"), 1500);
    JsonNode *playlist = playlist_create("Event kaynak playlisti değişmez", a, b);
    JsonNode *schema_list = playlist_create("Event şema playlisti değişmez", a, NULL);
    gint64 tomorrow = (g_get_real_time() / G_TIME_SPAN_DAY + 1) * G_TIME_SPAN_DAY + 5 * G_TIME_SPAN_MINUTE;
    JsonNode *weekly_config = weekly_pcm_configuration(schema_list, NULL, tomorrow, 1, "Event için bağımsız şema");
    JsonNode *weekly = document_save("weekly_programme", "Event için bağımsız şema", json_node_get_object(weekly_config));
    JsonObject *recipe = event_recipe("Program dışında doğrudan event", playlist, weekly);
    JsonObject *playback = json_object_new();
    json_object_set_int_member(playback, "player", 0); json_object_set_string_member(playback, "next_cluster_id", "");
    json_object_set_object_member(recipe, "playback", playback);
    JsonNode *saved_recipe = document_save("local_event", "Program dışında doğrudan event", recipe);
    RadioSAutomationScope source_scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 5};
    /* The explicit local-event override must stay single-player even when
     * this source's saved ordinary mode would transition to busy player 4. */
    GError *definition_error = NULL;
    RadioSPlayerTopology definitions = *radios_player_workspace_topology(window.ctx->players);
    require_call(radios_player_topology_set_autodj(&definitions, 5, RADIOS_AUTODJ_TRANSITIONS, 4, &definition_error), &definition_error);
    require_call(radios_player_workspace_replace(window.ctx->players, &definitions, &definition_error), &definition_error);
    RadioSAutoDJMode saved_mode = radios_player_workspace_topology(window.ctx->players)->players[5].autodj_mode;
    g_assert_cmpint(saved_mode, ==, RADIOS_AUTODJ_TRANSITIONS);
    JsonNode *source_draft = programme_config(background, NULL);
    JsonObject *stored_source = event_store_queue(&window, &source_scope, json_node_get_object(source_draft));
    seed_programme(&window, background, NULL); JsonNode *global_draft = document_read("programme", "Mevcut program");
    click(window.root, "programme-local-event");
    GtkWidget *dialog = dialog_find("local-event-dialog"); g_assert_nonnull(dialog); g_object_ref(dialog);
    EventRecipeChoice choice = {dialog, radios_json_string(json_node_get_object(saved_recipe), "_id", ""), "Program dışında doğrudan event"};
    wait_until(event_recipe_available, &choice, "actual root builder lists saved independent event recipe");
    wait_until(event_recipe_loaded, &choice, "actual root builder loads recipe and enables standalone playback");
    combo(dialog, "local-event-player", "auto"); combo(dialog, "local-event-after", "stop");
    JsonNode *ui_recipe = radios_local_event_ui_snapshot(dialog);
    g_assert_true(json_node_equal(ui_recipe, json_object_get_member(json_node_get_object(saved_recipe), "config"))); json_node_free(ui_recipe);
    click(window.root, "programme-play"); run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 100);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); RadioSAudioRunSnapshot global_before;
    radios_audio_engine_runner_snapshot(engine, &global_before); g_assert_true(global_before.owned[0]); g_assert_true(global_before.owned[1]);
    GError *error = NULL;
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(background), 2, FALSE, &error), &error);
    RadioSAudioUiSnapshot manual = deck_wait(&window, 2, RADIOS_AUDIO_UI_READY, 0);
    require_call(radios_audio_ui_set_deck_main(window.ctx, 3, FALSE, &error), &error);
    RadioSAudioUiSnapshot cue_only; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 3, &cue_only)); g_assert_false(cue_only.main_enabled);
    scope_legacy_engine_start(engine, 4, background); RadioSAudioRunSnapshot outsider = event_runner(&window, 4);
    RadioSAudioSnapshot audio_before; radios_audio_engine_snapshot(engine, &audio_before);
    click(dialog, "local-event-play");
    for (guint index = 0; index < 3; ++index) {
        RadioSAudioRunSnapshot playing = player_run_wait(engine, 5, RADIOS_AUDIO_RUN_PLAYING, index, 4800);
        g_assert_cmpuint(playing.count, ==, 3); g_assert_cmpint(playing.deck_current, ==, 5);
        if (!index) wait_until(event_semi_label_ready, &window, "actual player card shows transient Yarı AutoDJ event mode");
        for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) g_assert_cmpint(playing.owned[deck], ==, deck == 5);
        goffset pcm = capture_offset("MAIN"); run_for(250);
        g_assert_cmpfloat(tone_energy("MAIN", pcm, index == 1 ? 2350 : 1700), >, .05);
    }
    event_assert_complete(engine, 5, 3);
    wait_until(event_service_idle, window.ctx, "complete finite event clears its transient runtime mode");
    g_assert_cmpint(radios_player_workspace_topology(window.ctx->players)->players[5].autodj_mode, ==, saved_mode);
    RadioSAudioRunSnapshot global_after; radios_audio_engine_runner_snapshot(engine, &global_after);
    g_assert_cmpuint(global_after.epoch, ==, global_before.epoch); g_assert_cmpuint(global_after.count, ==, global_before.count);
    g_assert_cmpint(global_after.state, ==, RADIOS_AUDIO_RUN_PLAYING);
    scope_assert_live_pcm_unchanged(&window, engine, 4, audio_before.decks[4].generation, outsider.epoch, 700);
    RadioSAudioUiSnapshot unchanged; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 2, &unchanged));
    g_assert_cmpuint(unchanged.generation, ==, manual.generation); g_assert_cmpint(unchanged.state, ==, RADIOS_AUDIO_UI_READY);
    g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 3, &unchanged));
    g_assert_cmpuint(unchanged.generation, ==, cue_only.generation); g_assert_false(unchanged.main_enabled);
    event_document_unchanged("programme", "Mevcut program", global_draft);
    event_document_unchanged("weekly_programme", "Event için bağımsız şema", weekly);
    event_document_unchanged("local_event", "Program dışında doğrudan event", saved_recipe);
    event_queue_unchanged(&window, &source_scope, stored_source);
    assert_saved_playlist_unchanged(playlist); assert_saved_playlist_unchanged(schema_list);
    stop_run(&window); require_call(radios_audio_engine_player_runner_action(engine, 4, RADIOS_AUDIO_PLAYER_RUN_STOP, &error), &error);
    gtk_widget_destroy(dialog); g_object_unref(dialog);
    radios_audio_engine_unref(engine); json_object_unref(stored_source); json_node_free(source_draft); json_node_free(global_draft);
    json_node_free(saved_recipe); json_object_unref(recipe); json_node_free(weekly); json_node_free(weekly_config);
    json_node_free(schema_list); json_node_free(playlist); json_node_free(a); json_node_free(b); json_node_free(background); window_close(&window);
}
static void event_make_target(Window *window, gchar cluster[37], RadioSAutomationScope *scope, JsonObject *programme, JsonObject **stored)
{
    GError *error = NULL; RadioSPlayerTopology topology = *radios_player_workspace_topology(window->ctx->players);
    require_call(radios_player_topology_add_cluster(&topology, "Event sonrası küme", cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 0, cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 1, cluster, &error), &error);
    require_call(radios_player_workspace_replace(window->ctx->players, &topology, &error), &error);
    *scope = (RadioSAutomationScope){.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER}; g_strlcpy(scope->cluster_id, cluster, sizeof scope->cluster_id);
    *stored = event_store_queue(window, scope, programme);
}
/* Kılavuz 18.10 §07: the Ö button is gone from the event row; Özellikler is
 * the first item of the row's context menu (right click on the row body). */
static void event_row_properties(GtkWidget *row)
{
    GtkWidget *handle = gtk_bin_get_child(GTK_BIN(row)); g_assert_true(GTK_IS_EVENT_BOX(handle));
    GdkEvent *event = gdk_event_new(GDK_BUTTON_PRESS);
    event->button.window = g_object_ref(gtk_widget_get_window(handle)); event->button.button = 3;
    event->button.time = GDK_CURRENT_TIME; event->button.x = event->button.y = 4;
    gdk_event_set_device(event, gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(handle))));
    g_assert_true(gtk_widget_event(handle, event)); gdk_event_free(event);
    GtkWidget *menu = g_object_get_data(G_OBJECT(row), "programme-row-menu"); g_assert_true(GTK_IS_MENU(menu));
    GtkWidget *item = find(menu, "programme-menu-properties"); g_assert_true(GTK_IS_MENU_ITEM(item));
    gtk_menu_shell_deactivate(GTK_MENU_SHELL(menu)); gtk_menu_item_activate(GTK_MENU_ITEM(item));
}
static GtkWidget *event_first_row(Window *window)
{
    GtkWidget *table = find(window->root, "programme-queue-table"); g_assert_nonnull(table);
    GtkListBox *list = g_object_get_data(G_OBJECT(table), "list-box");
    GtkWidget *row = GTK_WIDGET(gtk_list_box_get_row_at_index(list, 0)); g_assert_nonnull(row); return row;
}
static void test_event_full_completion_then_target_cluster(void)
{
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "cluster event baseline");
    JsonNode *a = routing_short_markers(media_import(&window, "Kümesiz event ilk içerik", 1700, .22, "music"), 1800);
    JsonNode *b = routing_short_markers(media_import(&window, "Kümesiz event son içerik", 2350, .22, "music"), 1800);
    JsonNode *target = routing_short_markers(media_import(&window, "Event sonrası küme içeriği", 3100, .22, "music"), 2200);
    JsonNode *background = media_import_rhythm(&window, "Event dışı bağımsız yayın", 700, .2, "music", 120);
    JsonNode *target_config = programme_config(target, target);
    gchar cluster[37]; RadioSAutomationScope target_scope; JsonObject *stored;
    event_make_target(&window, cluster, &target_scope, json_node_get_object(target_config), &stored);
    seed_programme(&window, a, b); JsonNode *global_draft = document_read("programme", "Mevcut program");
    event_row_properties(event_first_row(&window));
    GtkWidget *properties = dialog_find("programme-block-editor"); g_assert_nonnull(properties); g_object_ref(properties);
    combo(properties, "event-playback-player", "3"); combo(properties, "event-playback-after", cluster);
    click(properties, "programme-block-assign");
    GtkWidget *builder = dialog_find("local-event-dialog"); g_assert_nonnull(builder); g_object_ref(builder);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(builder, "local-event-name"))), ==, "Müzik eventi");
    guint selected = G_MAXUINT; gchar selected_cluster[37];
    g_assert_true(radios_local_event_ui_playback(builder, &selected, selected_cluster));
    g_assert_cmpuint(selected, ==, 2); g_assert_cmpstr(selected_cluster, ==, cluster);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(builder, "local-event-kind"))), ==, "music");
    gtk_widget_destroy(builder); g_object_unref(builder);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); scope_legacy_engine_start(engine, 4, background);
    RadioSAudioRunSnapshot outsider = event_runner(&window, 4); RadioSAudioSnapshot before; radios_audio_engine_snapshot(engine, &before);
    GError *error = NULL;
    click(properties, "programme-block-play");
    for (guint index = 0; index < 2; ++index) {
        RadioSAudioRunSnapshot source = player_run_wait(engine, 2, RADIOS_AUDIO_RUN_PLAYING, index, 9600);
        g_assert_cmpuint(source.count, ==, 2); g_assert_true(source.owned[2]); g_assert_false(source.owned[0]); g_assert_false(source.owned[1]);
        event_assert_target_idle(&window, 0);
    }
    RadioSAudioRunSnapshot continued = player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    g_assert_true(continued.owned[0]); g_assert_true(continued.owned[1]); g_assert_false(continued.owned[2]);
    event_assert_complete(engine, 2, 2);
    goffset pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 3100), >, .05);
    scope_assert_live_pcm_unchanged(&window, engine, 4, before.decks[4].generation, outsider.epoch, 700);
    event_queue_unchanged(&window, &target_scope, stored);
    event_document_unchanged("programme", "Mevcut program", global_draft);
    RadioSAudioRunSnapshot global; radios_audio_engine_runner_snapshot(engine, &global); g_assert_cmpint(global.state, ==, RADIOS_AUDIO_RUN_IDLE);
    gtk_widget_destroy(properties); g_object_unref(properties); json_node_free(global_draft);
    event_stop_scope(&window, &target_scope);
    require_call(radios_audio_engine_player_runner_action(engine, 4, RADIOS_AUDIO_PLAYER_RUN_STOP, &error), &error);
    radios_audio_engine_unref(engine); json_object_unref(stored); json_node_free(target_config);
    json_node_free(a); json_node_free(b); json_node_free(target); json_node_free(background); window_close(&window);
}
static void test_event_stop_replacement_and_target_change_cancel_continuation(void)
{
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "event cancellation baseline");
    JsonNode *tone = routing_short_markers(media_import(&window, "İptal edilebilir event", 1700, .22, "music"), 1800);
    JsonNode *replacement = media_import(&window, "Yeni elle yüklenen korunur", 2350, .22, "music");
    JsonNode *event = programme_config(tone, NULL), *target_config = programme_config(replacement, NULL);
    gchar cluster[37]; RadioSAutomationScope target_scope; JsonObject *stored;
    event_make_target(&window, cluster, &target_scope, json_node_get_object(target_config), &stored);
    RadioSAutomationScope source_scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 2};
    ScopeReady scope_ready = {window.ctx->players, 2}; wait_until(scope_workspace_execution_ready, &scope_ready, "source scope history baseline");
    GError *error = NULL; RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    gchar *hold = fixture_flag(".acceptance-playout-prepare-hold"), *ready = fixture_flag(".acceptance-playout-prepare-ready");
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL));
    require_call(radios_event_playout_programme(window.ctx, window.root, json_node_get_object(event), 2, cluster, &error), &error);
    wait_until(file_exists, ready, "actual event prepare held before response delivery");
    event_stop_scope(&window, &source_scope); g_assert_cmpint(g_unlink(hold), ==, 0); g_assert_cmpint(g_unlink(ready), ==, 0);
    run_for(2300); event_assert_target_idle(&window, 0);
    RadioSAudioRunSnapshot source = event_runner(&window, 2); g_assert_cmpint(source.state, ==, RADIOS_AUDIO_RUN_IDLE);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    require_call(radios_event_playout_programme(window.ctx, window.root, json_node_get_object(event), 2, cluster, &error), &error);
    player_run_wait(engine, 2, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(replacement), 2, FALSE, &error), &error);
    RadioSAudioUiSnapshot manual = deck_wait(&window, 2, RADIOS_AUDIO_UI_READY, 0);
    run_for(2300); event_assert_target_idle(&window, 0);
    RadioSAudioUiSnapshot actual; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 2, &actual));
    g_assert_cmpuint(actual.generation, ==, manual.generation); g_assert_cmpint(actual.state, ==, RADIOS_AUDIO_UI_READY);
    g_assert_cmpstr(actual.title, ==, "Yeni elle yüklenen korunur");
    require_call(radios_audio_engine_unload(engine, 2, &error), &error); deck_wait(&window, 2, RADIOS_AUDIO_UI_EMPTY, 0);
    require_call(radios_event_playout_programme(window.ctx, window.root, json_node_get_object(event), 2, cluster, &error), &error);
    player_run_wait(engine, 2, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    RadioSPlayerTopology topology = *radios_player_workspace_topology(window.ctx->players);
    require_call(radios_player_topology_set_autodj(&topology, 0, RADIOS_AUTODJ_SEMI, -1, &error), &error);
    require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
    run_for(2300); event_assert_target_idle(&window, 0);
    event_queue_unchanged(&window, &target_scope, stored);
    RadioSAudioRunSnapshot global; radios_audio_engine_runner_snapshot(engine, &global); g_assert_cmpint(global.state, ==, RADIOS_AUDIO_RUN_IDLE);
    radios_audio_engine_unref(engine); g_free(hold); g_free(ready); json_object_unref(stored);
    json_node_free(target_config); json_node_free(event); json_node_free(replacement); json_node_free(tone); window_close(&window);
}
typedef struct {
    Window *window;
    RadioSAutomationScope scope;
    gint64 start;
} EventSchemaWaiting;
static gboolean event_schema_waiting(gpointer value)
{
    EventSchemaWaiting *waiting = value;
    RadioSPlayerScopeExecutionSnapshot state;
    return radios_player_scope_execution_snapshot(radios_player_scope_execution_get(waiting->window->ctx),
        &waiting->scope, &state) && state.schema_active && state.state == RADIOS_SCOPE_EXECUTION_WAITING &&
        state.starts_at_utc_us == waiting->start;
}
static void test_event_assigned_schema_continuation_and_future_stop(void)
{
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "assigned-schema event baseline");
    JsonNode *source = routing_short_markers(media_import(&window, "Sonlu kümesiz event", 1700, .22, "music"), 1800);
    JsonNode *schema_tone = routing_short_markers(media_import(&window, "Atanmış küme şeması gerçek ses", 3100, .22, "music"), 2500);
    JsonNode *stored_tone = routing_short_markers(media_import(&window, "Saklı sıra şemanın yerine çalınmaz", 2350, .22, "music"), 2500);
    JsonNode *event = programme_config(source, NULL), *stored_config = programme_config(stored_tone, NULL);
    gchar cluster[37]; RadioSAutomationScope target_scope; JsonObject *stored;
    event_make_target(&window, cluster, &target_scope, json_node_get_object(stored_config), &stored);
    JsonNode *playlist = playlist_create("Devam kümesinin atanmış şema kaynakları", schema_tone, NULL);
    const gint64 minute = G_TIME_SPAN_MINUTE;
    gint64 now_minute = g_get_real_time() / minute * minute;
    GDateTime *now = g_date_time_new_from_unix_utc(now_minute / G_USEC_PER_SEC);
    guint remaining_minutes = 1440 - (g_date_time_get_hour(now) * 60 + g_date_time_get_minute(now));
    g_date_time_unref(now);
    JsonNode *active_config = weekly_pcm_configuration(playlist, NULL, now_minute, MIN(remaining_minutes, 2), "Event sonrası atanmış şema");
    general_single_batch(json_object_get_object_member(json_node_get_object(active_config), "calendar"), playlist);
    JsonNode *active = document_save("weekly_programme", "Event sonrası atanmış şema", json_node_get_object(active_config));
    GError *error = NULL;
    require_call(radios_player_workspace_assign(window.ctx->players, &target_scope,
        radios_json_string(json_node_get_object(active), "_id", ""), &error), &error);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    require_call(radios_event_playout_programme(window.ctx, window.root, json_node_get_object(event), 2, cluster, &error), &error);
    player_run_wait(engine, 2, RADIOS_AUDIO_RUN_PLAYING, 0, 4800); event_assert_target_idle(&window, 0);
    RadioSAudioRunSnapshot target = player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    g_assert_true(target.owned[0]); g_assert_true(target.owned[1]); g_assert_false(target.owned[2]);
    event_assert_complete(engine, 2, 1);
    wait_until(event_service_idle, window.ctx, "assigned target starts before the finite event handoff retires");
    goffset pcm = capture_offset("MAIN"); run_for(250);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 3100), >, .05);
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 2350), <, .01);
    RadioSPlayerScopeExecutionSnapshot target_state;
    g_assert_true(radios_player_scope_execution_snapshot(radios_player_scope_execution_get(window.ctx), &target_scope, &target_state));
    g_assert_true(target_state.schema_active); g_assert_cmpstr(target_state.event_name, ==, "Event sonrası atanmış şema");
    event_stop_scope(&window, &target_scope);
    event_queue_unchanged(&window, &target_scope, stored);
    event_document_unchanged("weekly_programme", "Event sonrası atanmış şema", active);

    /* A future assigned schema is an armed handoff, not a completed start.
     * Keep its caller fence alive while waiting, and let target Stop revoke it. */
    gint64 tomorrow = (g_get_real_time() / G_TIME_SPAN_DAY + 1) * G_TIME_SPAN_DAY + 5 * minute;
    JsonNode *future_config = weekly_pcm_configuration(playlist, NULL, tomorrow, 1, "Gelecekteki devam şeması");
    JsonNode *future = document_save("weekly_programme", "Gelecekteki devam şeması", json_node_get_object(future_config));
    require_call(radios_player_workspace_assign(window.ctx->players, &target_scope,
        radios_json_string(json_node_get_object(future), "_id", ""), &error), &error);
    require_call(radios_event_playout_programme(window.ctx, window.root, json_node_get_object(event), 2, cluster, &error), &error);
    player_run_wait(engine, 2, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    event_assert_complete(engine, 2, 1);
    EventSchemaWaiting waiting = {&window, target_scope, tomorrow};
    wait_until(event_schema_waiting, &waiting, "event continuation retains its fence while the assigned schema waits for UTC");
    g_assert_true(radios_event_playout_busy(window.ctx, 2)); event_assert_target_idle(&window, 0);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    event_stop_scope(&window, &target_scope);
    wait_until(event_service_idle, window.ctx, "target Stop revokes finite event's future schema handoff");
    g_assert_false(radios_player_scope_execution_busy(radios_player_scope_execution_get(window.ctx), &target_scope));
    event_assert_target_idle(&window, 0); run_for(300);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001);
    event_queue_unchanged(&window, &target_scope, stored);
    event_document_unchanged("weekly_programme", "Gelecekteki devam şeması", future);
    assert_saved_playlist_unchanged(playlist);
    radios_audio_engine_unref(engine); json_object_unref(stored);
    json_node_free(future); json_node_free(future_config); json_node_free(active); json_node_free(active_config);
    json_node_free(playlist); json_node_free(event); json_node_free(stored_config);
    json_node_free(source); json_node_free(schema_tone); json_node_free(stored_tone); window_close(&window);
}
typedef struct {
    RadioSViewContext *ctx;
    const gchar *origin, *entry;
    guint source;
} EventRunVisible;
static gboolean event_run_visible(gpointer value)
{
    EventRunVisible *visible = value; JsonNode *snapshot = radios_event_playout_snapshot(visible->ctx);
    JsonArray *runs = json_object_get_array_member(json_node_get_object(snapshot), "runs");
    gboolean found = FALSE;
    for (guint i = 0; i < json_array_get_length(runs); ++i) {
        JsonObject *row = json_array_get_object_element(runs, i);
        if (g_strcmp0(radios_json_string(row, "origin_block_id", ""), visible->origin)) continue;
        found = radios_json_int(row, "source", -1) == visible->source &&
            radios_json_int(row, "state", RADIOS_PLAYOUT_IDLE) == RADIOS_PLAYOUT_PLAYING &&
            !g_strcmp0(radios_json_string(row, "entry_id", ""), visible->entry) &&
            radios_json_int(row, "position_ms", 0) > 0 && radios_json_int(row, "duration_ms", 0) > 0 &&
            !json_object_get_boolean_member(row, "completed") &&
            json_object_has_member(row, "programme");
        g_assert_cmpstr(radios_json_string(row, "source_name", ""), !=, "");
        g_assert_cmpstr(radios_json_string(row, "next_cluster_id", "invalid"), ==, "");
    }
    json_node_free(snapshot);
    if (found) {
        JsonNode *display = radios_event_playout_display_snapshot(visible->ctx);
        JsonArray *display_runs = json_object_get_array_member(json_node_get_object(display), "runs");
        gboolean display_found = FALSE;
        for (guint i = 0; i < json_array_get_length(display_runs); ++i) {
            JsonObject *row = json_array_get_object_element(display_runs, i);
            g_assert_false(json_object_has_member(row, "programme"));
            if (!g_strcmp0(radios_json_string(row, "origin_block_id", ""), visible->origin)) {
                g_assert_cmpint(radios_json_int(row, "source", -1), ==, visible->source);
                g_assert_cmpstr(radios_json_string(row, "entry_id", ""), ==, visible->entry);
                display_found = TRUE;
            }
        }
        g_assert_true(display_found); json_node_free(display);
    }
    return found;
}
static void test_event_semi_pending_ignores_ordinary_transition_peer_replacement(void)
{
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "SEMI peer independence baseline");
    JsonNode *tone = routing_short_markers(media_import(&window, "Tek kaynak gerçek Event", 1700, .22, "music"), 2400);
    JsonNode *old_peer = media_import(&window, "Eski bağımsız eş", 700, .2, "music");
    JsonNode *new_peer = media_import(&window, "Event hazırlanırken değişen bağımsız eş", 2350, .2, "music");
    JsonNode *programme = programme_config(tone, NULL);
    /* Keep service terminal notifications separate from the broadcast GUI's
     * ordinary consumer, so this API acceptance can assert exact-once delivery. */
    GtkWidget *service_owner = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(service_owner);
    RadioSViewContext service_ctx = *window.ctx; service_ctx.parent = GTK_WINDOW(service_owner);
    GError *error = NULL; RadioSPlayerTopology topology = *radios_player_workspace_topology(window.ctx->players);
    require_call(radios_player_topology_set_autodj(&topology, 2, RADIOS_AUTODJ_TRANSITIONS, 3, &error), &error);
    require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(old_peer), 3, FALSE, &error), &error);
    RadioSAudioUiSnapshot old = deck_wait(&window, 3, RADIOS_AUDIO_UI_READY, 0);
    gchar *hold = fixture_flag(".acceptance-playout-prepare-hold"), *ready = fixture_flag(".acceptance-playout-prepare-ready");
    gchar *origin = g_uuid_string_random();
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL));
    require_call(radios_event_playout_programme_for_event(&service_ctx, service_owner, json_node_get_object(programme),
        2, NULL, origin, &error), &error);
    wait_until(file_exists, ready, "SEMI exact formal programme preparation held after actual-pool capture");
    require_call(radios_audio_ui_load_media(window.ctx, window.root, json_node_get_object(new_peer), 3, FALSE, &error), &error);
    RadioSAudioUiSnapshot peer = deck_wait(&window, 3, RADIOS_AUDIO_UI_READY, 0);
    g_assert_cmpuint(peer.generation, !=, old.generation);
    g_assert_cmpint(g_unlink(hold), ==, 0); g_assert_cmpint(g_unlink(ready), ==, 0);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    RadioSAudioRunSnapshot run = player_run_wait(engine, 2, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    g_assert_cmpuint(run.count, ==, 1);
    for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) g_assert_cmpint(run.owned[deck], ==, deck == 2);
    JsonObject *leaf = json_array_get_object_element(json_object_get_array_member(json_node_get_object(programme), "events"), 0);
    EventRunVisible visible = {&service_ctx, origin, radios_json_string(leaf, "id", ""), 2};
    wait_until(event_run_visible, &visible, "origin UUID and actual SEMI entry progress are exposed to the existing Event GUI");
    goffset pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .05);
    RadioSAudioUiSnapshot unchanged; g_assert_true(radios_audio_ui_get_snapshot(window.ctx, 3, &unchanged));
    g_assert_cmpuint(unchanged.generation, ==, peer.generation); g_assert_cmpint(unchanged.state, ==, RADIOS_AUDIO_UI_READY);
    g_assert_cmpstr(unchanged.title, ==, "Event hazırlanırken değişen bağımsız eş");
    event_assert_complete(engine, 2, 1); wait_until(event_service_idle, &service_ctx, "independent SEMI Event naturally completes");
    JsonArray *completed = radios_event_playout_take_completed(&service_ctx);
    g_assert_cmpuint(json_array_get_length(completed), ==, 1);
    JsonObject *terminal = json_array_get_object_element(completed, 0);
    g_assert_cmpstr(radios_json_string(terminal, "origin_block_id", ""), ==, origin);
    g_assert_cmpint(radios_json_int(terminal, "source", -1), ==, 2); json_array_unref(completed);
    completed = radios_event_playout_take_completed(&service_ctx);
    g_assert_cmpuint(json_array_get_length(completed), ==, 0); json_array_unref(completed);
    require_call(radios_event_playout_programme_for_event(&service_ctx, service_owner, json_node_get_object(programme),
        2, NULL, origin, &error), &error);
    player_run_wait(engine, 2, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    RadioSAutomationScope source_scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 2};
    require_call(radios_player_scope_execution_action(radios_player_scope_execution_get(&service_ctx),
        &source_scope, RADIOS_PLAYOUT_STOP, &error), &error);
    wait_until(event_service_idle, &service_ctx, "Stop cancels Event service before a genuine final completion");
    run_for(300); completed = radios_event_playout_take_completed(&service_ctx);
    g_assert_cmpuint(json_array_get_length(completed), ==, 0); json_array_unref(completed);
    g_assert_cmpint(radios_player_workspace_topology(window.ctx->players)->players[2].autodj_mode, ==, RADIOS_AUTODJ_TRANSITIONS);
    gtk_widget_destroy(service_owner); g_object_unref(service_owner);
    radios_audio_engine_unref(engine); g_free(origin); g_free(hold); g_free(ready);
    json_node_free(programme); json_node_free(tone); json_node_free(old_peer); json_node_free(new_peer); window_close(&window);
}
static int event_playout_test_run(void)
{
    g_test_add_func("/native-playout/local-event-unclustered-SEMI-preserves-queues-PCM", test_local_event_automatic_unclustered_semi_preserves_all_queues);
    g_test_add_func("/native-playout/event-complete-then-cluster-continuation-PCM", test_event_full_completion_then_target_cluster);
    g_test_add_func("/native-playout/event-stop-newload-target-change-fences-PCM", test_event_stop_replacement_and_target_change_cancel_continuation);
    g_test_add_func("/native-playout/event-assigned-schema-continuation-future-stop-PCM", test_event_assigned_schema_continuation_and_future_stop);
    g_test_add_func("/native-playout/event-SEMI-pending-unrelated-peer-replacement-PCM", test_event_semi_pending_ignores_ordinary_transition_peer_replacement);
    return g_test_run();
}
int main(int argc, char **argv) { return radios_event_playout_fixture_main(argc, argv); }
