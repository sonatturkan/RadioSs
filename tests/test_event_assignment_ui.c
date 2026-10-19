#define _POSIX_C_SOURCE 200809L
#include <glib.h>
static int event_assignment_test_run(void);
static void assignment_fixture_ignore(const char *path, GTestFunc test) { (void)path; (void)test; }
#define main radios_event_assignment_fixture_main
#define g_test_run event_assignment_test_run
#define g_test_add_func assignment_fixture_ignore
#include "test_playout_ui.c"
#undef g_test_add_func
#undef g_test_run
#undef main
#include "radios/event_playout.h"
#include "radios/local_event_ui.h"

static JsonObject *assignment_block(const gchar *id, const gchar *title)
{
    JsonObject *block = json_object_new();
    json_object_set_string_member(block, "id", id); json_object_set_string_member(block, "kind", "music");
    json_object_set_string_member(block, "title", title); json_object_set_string_member(block, "start_time", "");
    json_object_set_string_member(block, "note", "Ayrı event korunur"); return block;
}
static JsonNode *assignment_seed(Window *window, JsonNode *first, JsonNode *second, gchar **target_id, gchar **other_id)
{
    seed_programme(window, first, second);
    JsonNode *initial = document_read("programme", "Mevcut program");
    JsonObject *config_object = radios_programme_clone_object(json_object_get_object_member(json_node_get_object(initial), "config"));
    JsonArray *events = json_object_get_array_member(config_object, "events"), *blocks = json_array_new();
    *target_id = g_uuid_string_random(); *other_id = g_uuid_string_random();
    json_array_add_object_element(blocks, assignment_block(*target_id, "Atanacak event"));
    json_array_add_object_element(blocks, assignment_block(*other_id, "Dokunulmayacak event"));
    json_object_set_array_member(config_object, "blocks", blocks);
    json_object_set_string_member(json_array_get_object_element(events, 0), "block_id", *target_id);
    json_object_set_string_member(json_array_get_object_element(events, 1), "block_id", *other_id);
    JsonNode *saved = document_save("programme", "Mevcut program", config_object);
    SeedCondition seeded = {{window, 2, radios_json_string(json_array_get_object_element(events, 0), "title", "")},
        radios_json_string(json_node_get_object(saved), "_id", ""), radios_json_int(json_node_get_object(saved), "_rev", 0)};
    click_label(window->root, "Verileri yenile"); wait_until(seeded_programme_ready, &seeded, "two explicit events loaded with original UUIDs");
    json_object_unref(config_object); json_node_free(initial); return saved;
}
static GtkWidget *assignment_row(Window *window, const gchar *id)
{
    GtkWidget *table = find(window->root, "programme-queue-table");
    GtkWidget *list = g_object_get_data(G_OBJECT(table), "list-box");
    GList *children = gtk_container_get_children(GTK_CONTAINER(list)); GtkWidget *found = NULL;
    for (GList *item = children; item; item = item->next)
        if (!g_strcmp0(radios_programme_queue_row_id(GTK_LIST_BOX_ROW(item->data)), id)) { found = item->data; break; }
    g_list_free(children); return found;
}
/* Kılavuz 18.10 §07: the Ö button is gone from the event row; Özellikler is
 * the first item of the row's context menu (right click on the row body). */
static void assignment_row_properties(GtkWidget *row)
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
static GtkWidget *assignment_properties(Window *window, const gchar *id)
{
    GtkWidget *row = assignment_row(window, id); g_assert_nonnull(row); assignment_row_properties(row);
    GtkWidget *dialog = dialog_find("programme-block-editor"); g_assert_nonnull(dialog); g_object_ref(dialog); return dialog;
}
typedef struct { GtkWidget *dialog; const gchar *id; } AssignmentSource;
static gboolean assignment_source_available(gpointer value)
{
    AssignmentSource *source = value;
    return gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(source->dialog, "local-event-source")), source->id);
}
static GtkWidget *assignment_builder(GtkWidget *properties, JsonNode *playlist)
{
    click(properties, "programme-block-assign");
    GtkWidget *builder = dialog_find("local-event-dialog"); g_assert_nonnull(builder); g_object_ref(builder);
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(find(builder, "local-event-insert"))), ==, "Seçili evente ata");
    if (playlist) {
        combo(builder, "local-event-source-type", "playlist");
        AssignmentSource source = {builder, radios_json_string(json_node_get_object(playlist), "_id", "")};
        wait_until(assignment_source_available, &source, "event assignment picker lists genuine new playlist");
        click(builder, "local-event-add");
    }
    return builder;
}
typedef struct { Window *window; const gchar *target, *playlist; JsonNode *saved; } AssignmentAck;
static gboolean assignment_acknowledged(gpointer value)
{
    AssignmentAck *ack = value;
    JsonNode *saved = document_read("programme", "Mevcut program");
    if (!saved) return FALSE;
    JsonObject *config_object = json_object_get_object_member(json_node_get_object(saved), "config");
    RadioSProgrammeTree *tree = radios_programme_tree_new(config_object); guint index; gint leaf;
    gboolean accepted = radios_programme_tree_find(tree, ack->target, &index, &leaf) && leaf < 0;
    JsonObject *block = accepted ? ((RadioSProgrammeBlock *)g_ptr_array_index(tree->blocks, index))->block : NULL;
    JsonObject *recipe = block && json_object_has_member(block, "local_event") ? json_object_get_object_member(block, "local_event") : NULL;
    JsonArray *sources = recipe ? json_object_get_array_member(recipe, "sources") : NULL;
    accepted = sources && json_array_get_length(sources) == 1 &&
        !g_strcmp0(radios_json_string(json_array_get_object_element(sources, 0), "source_id", ""), ack->playlist) &&
        radios_data_document_revision(radios_broadcast_programme_document(ack->window->ctx)) == radios_json_int(json_node_get_object(saved), "_rev", 0);
    radios_programme_tree_free(tree);
    if (accepted) { g_clear_pointer(&ack->saved, json_node_free); ack->saved = saved; }
    else json_node_free(saved);
    return accepted;
}
typedef struct { Window *window; RadioSAutomationScope scope; JsonObject *programme; } AssignmentQueue;
static gboolean assignment_queue_set(gpointer value)
{
    AssignmentQueue *queue = value; GError *error = NULL;
    gboolean saved = radios_player_workspace_set_programme(queue->window->ctx->players, &queue->scope, queue->programme, &error);
    if (!saved) g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PENDING);
    g_clear_error(&error); return saved;
}
static gboolean assignment_queue_saved(gpointer value)
{
    AssignmentQueue *queue = value; GError *error = NULL;
    gboolean saved = radios_player_workspace_programme_saved(queue->window->ctx->players, &queue->scope, &error);
    if (!saved) g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PENDING);
    g_clear_error(&error); return saved;
}
static JsonObject *assignment_target(Window *window, JsonObject *programme, gchar cluster[37], RadioSAutomationScope *scope)
{
    GError *error = NULL; RadioSPlayerTopology topology = *radios_player_workspace_topology(window->ctx->players);
    require_call(radios_player_topology_add_cluster(&topology, "Atama sonrası küme", cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 0, cluster, &error), &error);
    require_call(radios_player_topology_set_cluster(&topology, 1, cluster, &error), &error);
    require_call(radios_player_workspace_replace(window->ctx->players, &topology, &error), &error);
    *scope = (RadioSAutomationScope){.kind = RADIOS_AUTOMATION_SCOPE_CLUSTER};
    g_strlcpy(scope->cluster_id, cluster, sizeof scope->cluster_id);
    AssignmentQueue queue = {window, *scope, programme};
    wait_until(assignment_queue_set, &queue, "saved continuation cluster accepts isolated programme");
    wait_until(assignment_queue_saved, &queue, "continuation cluster programme receives durable CAS ACK");
    JsonObject *stored = NULL;
    require_call(radios_player_workspace_get_programme(window->ctx->players, scope, &stored, &error), &error); return stored;
}
typedef struct { Window *window; const gchar *id, *contains; gboolean completed; } AssignmentRuntime;
static gboolean assignment_runtime_matches(gpointer value)
{
    AssignmentRuntime *expected = value; GtkWidget *row = assignment_row(expected->window, expected->id);
    GtkWidget *label = row ? find(row, "programme-row-runtime") : NULL;
    return GTK_IS_LABEL(label) && gtk_widget_get_visible(label) &&
        strstr(gtk_label_get_text(GTK_LABEL(label)), "Yarı AutoDJ") &&
        strstr(gtk_label_get_text(GTK_LABEL(label)), expected->contains) &&
        gtk_style_context_has_class(gtk_widget_get_style_context(row), expected->completed ? "is-event-run-completed" : "is-event-running");
}
static void assignment_assert_document_unchanged(JsonNode *expected)
{
    JsonNode *actual = document_read("programme", "Mevcut program"); g_assert_nonnull(actual);
    g_assert_true(json_node_equal(actual, expected)); json_node_free(actual);
}
static void test_selected_assignment_saved_reopened_semi_row_pcm(void)
{
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "event assignment workspace baseline");
    JsonNode *original = media_import(&window, "Atanmadan önceki eski içerik", 810, .22, "music");
    JsonNode *unrelated = media_import(&window, "Diğer event kesinlikle korunur", 950, .22, "music");
    JsonNode *a = routing_short_markers(media_import(&window, "Atanmış playlist ilk müzik", 1700, .22, "music"), 1800);
    JsonNode *b = routing_short_markers(media_import(&window, "Atanmış playlist son müzik", 2350, .22, "music"), 1800);
    JsonNode *target = routing_short_markers(media_import(&window, "Event bitince küme müziği", 3100, .22, "music"), 2200);
    JsonNode *playlist = playlist_create("Gerçek event atama kaynağı", a, b), *target_config = programme_config(target, target);
    gchar cluster[37], *target_id, *other_id; RadioSAutomationScope target_scope;
    JsonObject *stored_target = assignment_target(&window, json_node_get_object(target_config), cluster, &target_scope);
    JsonNode *before = assignment_seed(&window, original, unrelated, &target_id, &other_id);
    JsonObject *old_config = json_object_get_object_member(json_node_get_object(before), "config");
    JsonNode *untouched_leaf = deep_copy(json_array_get_element(json_object_get_array_member(old_config, "events"), 1));
    JsonNode *untouched_block = deep_copy(json_array_get_element(json_object_get_array_member(old_config, "blocks"), 1));
    GtkWidget *properties = assignment_properties(&window, target_id);
    combo(properties, "event-playback-player", "3"); combo(properties, "event-playback-after", cluster);
    GtkWidget *builder = assignment_builder(properties, playlist); click(builder, "local-event-insert");
    AssignmentAck ack = {&window, target_id, radios_json_string(json_node_get_object(playlist), "_id", ""), NULL};
    wait_until(assignment_acknowledged, &ack, "new playlist is durably assigned to the same event UUID");
    JsonObject *saved_config = json_object_get_object_member(json_node_get_object(ack.saved), "config");
    JsonArray *blocks = json_object_get_array_member(saved_config, "blocks"), *events = json_object_get_array_member(saved_config, "events");
    g_assert_cmpuint(json_array_get_length(blocks), ==, 2); g_assert_cmpuint(json_array_get_length(events), ==, 3);
    JsonObject *assigned = json_array_get_object_element(blocks, 0), *route = json_object_get_object_member(assigned, "playback");
    g_assert_cmpstr(radios_json_string(assigned, "id", ""), ==, target_id);
    g_assert_cmpint(radios_json_int(route, "player", -1), ==, 3);
    g_assert_cmpstr(radios_json_string(route, "next_cluster_id", ""), ==, cluster);
    g_assert_true(json_node_equal(json_array_get_element(blocks, 1), untouched_block));
    g_assert_true(json_node_equal(json_array_get_element(events, 2), untouched_leaf));
    JsonNode *details[] = {a, b};
    for (guint i = 0; i < 2; ++i) {
        JsonObject *leaf = json_array_get_object_element(events, i);
        g_assert_cmpstr(radios_json_string(leaf, "block_id", ""), ==, target_id);
        g_assert_cmpstr(radios_json_string(leaf, "media_id", ""), ==,
            radios_json_string(json_object_get_object_member(json_node_get_object(details[i]), "media"), "_id", ""));
    }
    gtk_widget_destroy(builder); g_object_unref(builder); gtk_widget_destroy(properties); g_object_unref(properties);
    properties = assignment_properties(&window, target_id);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(properties, "event-playback-player"))), ==, "3");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(properties, "event-playback-after"))), ==, cluster);
    builder = assignment_builder(properties, NULL);
    JsonNode *reopened = radios_local_event_ui_snapshot(builder);
    g_assert_true(json_node_equal(reopened, json_object_get_member(assigned, "local_event"))); json_node_free(reopened);
    gtk_widget_destroy(builder); g_object_unref(builder); gtk_widget_destroy(properties); g_object_unref(properties);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    GtkWidget *row = assignment_row(&window, target_id); g_assert_nonnull(row); click(row, "programme-row-play");
    AssignmentRuntime playing = {&window, target_id, "Çalıyor", FALSE};
    for (guint index = 0; index < 2; ++index) {
        RadioSAudioRunSnapshot run = player_run_wait(engine, 2, RADIOS_AUDIO_RUN_PLAYING, index, 4800);
        g_assert_cmpuint(run.count, ==, 2); g_assert_cmpint(run.deck_current, ==, 2);
        for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) g_assert_cmpint(run.owned[deck], ==, deck == 2);
        wait_until(assignment_runtime_matches, &playing, "the original event row shows its independent Yarı AutoDJ runtime");
        goffset pcm = capture_offset("MAIN"); run_for(250);
        g_assert_cmpfloat(tone_energy("MAIN", pcm, index ? 2350 : 1700), >, .05);
        g_assert_cmpfloat(tone_energy("MAIN", pcm, 810), <, .01);
        g_assert_cmpfloat(tone_energy("MAIN", pcm, 950), <, .01);
        RadioSAudioRunSnapshot idle; radios_audio_engine_player_runner_snapshot(engine, 0, &idle);
        g_assert_cmpint(idle.state, ==, RADIOS_AUDIO_RUN_IDLE);
    }
    RadioSAudioRunSnapshot continued = player_run_wait(engine, 0, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    g_assert_true(continued.owned[0]); g_assert_true(continued.owned[1]); g_assert_false(continued.owned[2]);
    RadioSAudioRunSnapshot finished = player_run_wait(engine, 2, RADIOS_AUDIO_RUN_FINISHED, 1, 0);
    g_assert_cmpuint(finished.count, ==, 2);
    for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) g_assert_false(finished.owned[deck]);
    RadioSAudioRunReceipt receipts[8]; guint count = radios_audio_engine_player_runner_receipts(engine, 2, receipts, G_N_ELEMENTS(receipts)), completed = 0;
    for (guint i = 0; i < count; ++i) if (receipts[i].epoch == finished.epoch) {
        g_assert_cmpint(receipts[i].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_COMPLETE); ++completed;
    }
    g_assert_cmpuint(completed, ==, 2);
    AssignmentRuntime done = {&window, target_id, "Tamamlandı", TRUE};
    wait_until(assignment_runtime_matches, &done, "genuine final COMPLETE leaves a stable finished event row");
    goffset pcm = capture_offset("MAIN"); run_for(500); g_assert_true(assignment_runtime_matches(&done));
    g_assert_cmpfloat(tone_energy("MAIN", pcm, 3100), >, .05);
    assignment_assert_document_unchanged(ack.saved); assert_saved_playlist_unchanged(playlist);
    JsonObject *actual_target = NULL; GError *error = NULL;
    require_call(radios_player_workspace_get_programme(window.ctx->players, &target_scope, &actual_target, &error), &error);
    JsonNode *actual_node = json_node_new(JSON_NODE_OBJECT), *stored_node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(actual_node, actual_target); json_node_set_object(stored_node, stored_target);
    g_assert_true(json_node_equal(actual_node, stored_node)); json_node_free(actual_node); json_node_free(stored_node); json_object_unref(actual_target);
    RadioSAudioRunSnapshot global; radios_audio_engine_runner_snapshot(engine, &global); g_assert_cmpint(global.state, ==, RADIOS_AUDIO_RUN_IDLE);
    require_call(radios_player_scope_execution_action(radios_player_scope_execution_get(window.ctx), &target_scope, RADIOS_PLAYOUT_STOP, &error), &error);
    radios_audio_engine_unref(engine); json_object_unref(stored_target);
    json_node_free(untouched_leaf); json_node_free(untouched_block); json_node_free(ack.saved); json_node_free(before);
    json_node_free(target_config); json_node_free(playlist); json_node_free(original); json_node_free(unrelated);
    json_node_free(a); json_node_free(b); json_node_free(target); g_free(target_id); g_free(other_id); window_close(&window);
    g_test_message("Actual Event Ö assigns a real playlist to the same UUID, preserves the unrelated event, reopens saved sources/routing, and Event S plays both songs on only the independent SEMI player before genuine completion, stable row status and cluster PCM.");
}
typedef struct { GtkWidget *dialog; const gchar *contains; } AssignmentMessage;
static gboolean assignment_message_matches(gpointer value)
{
    AssignmentMessage *expected = value; GtkWidget *label = find(expected->dialog, "local-event-status");
    return GTK_IS_LABEL(label) && strstr(gtk_label_get_text(GTK_LABEL(label)), expected->contains);
}
typedef struct { Window *window; gint64 previous; } AssignmentRevision;
static gboolean assignment_revision_advanced(gpointer value)
{
    AssignmentRevision *expected = value; RadioSDocument *document = radios_broadcast_programme_document(expected->window->ctx);
    return radios_data_document_ready(document) && radios_data_document_revision(document) > expected->previous;
}
static void test_selected_assignment_stale_target_fence(void)
{
    Window window = window_open(); wait_until(scope_workspace_ready, window.ctx->players, "stale assignment workspace baseline");
    JsonNode *old = media_import(&window, "Korunacak eski event içeriği", 810, .2, "music");
    JsonNode *other = media_import(&window, "Diğer eski event içeriği", 950, .2, "music");
    JsonNode *fresh = media_import(&window, "Geç gelen atanamaz içerik", 1700, .2, "music");
    JsonNode *playlist = playlist_create("Geç atanacak playlist", fresh, fresh);
    gchar *target_id, *other_id; JsonNode *before = assignment_seed(&window, old, other, &target_id, &other_id);
    GtkWidget *properties = assignment_properties(&window, target_id), *builder = assignment_builder(properties, playlist);
    gchar *hold = fixture_flag(".acceptance-playout-prepare-hold"), *ready = fixture_flag(".acceptance-playout-prepare-ready");
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL)); click(builder, "local-event-insert");
    wait_until(file_exists, ready, "genuine playlist materialization response is delayed after capturing target");
    AssignmentRevision changed = {&window, radios_json_int(json_node_get_object(before), "_rev", 0)};
    click(window.root, "programme-add-announcement");
    wait_until(assignment_revision_advanced, &changed, "new operator event edit receives its own durable revision");
    JsonNode *newer = document_read("programme", "Mevcut program");
    g_assert_cmpint(g_unlink(hold), ==, 0); g_assert_cmpint(g_unlink(ready), ==, 0);
    AssignmentMessage rejected = {builder, "değişti"};
    wait_until(assignment_message_matches, &rejected, "late event assignment rejects a changed queue baseline");
    run_for(200); assignment_assert_document_unchanged(newer); assert_saved_playlist_unchanged(playlist);
    JsonObject *new_config = json_object_get_object_member(json_node_get_object(newer), "config");
    JsonArray *new_blocks = json_object_get_array_member(new_config, "blocks");
    g_assert_cmpuint(json_array_get_length(new_blocks), ==, 3);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(new_blocks, 0), "id", ""), ==, target_id);
    g_assert_false(json_object_has_member(json_array_get_object_element(new_blocks, 0), "local_event"));
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx); RadioSAudioRunSnapshot run;
    radios_audio_engine_player_runner_snapshot(engine, 2, &run); g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_IDLE);
    radios_audio_engine_runner_snapshot(engine, &run); g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_IDLE);
    g_assert_cmpfloat(measure("MAIN").peak[0], <, .0001); radios_audio_engine_unref(engine);
    gtk_widget_destroy(builder); g_object_unref(builder); gtk_widget_destroy(properties); g_object_unref(properties);
    json_node_free(newer); json_node_free(before); json_node_free(playlist); json_node_free(old); json_node_free(other); json_node_free(fresh);
    g_free(target_id); g_free(other_id); g_free(hold); g_free(ready); window_close(&window);
}
static gchar *assignment_first_block_id(Window *window)
{
    GtkWidget *table = find(window->root, "programme-queue-table");
    GtkWidget *list = g_object_get_data(G_OBJECT(table), "list-box");
    GList *rows = gtk_container_get_children(GTK_CONTAINER(list)); gchar *id = NULL;
    for (GList *item = rows; item; item = item->next)
        if (GPOINTER_TO_INT(g_object_get_data(G_OBJECT(item->data), "programme-item-block"))) {
            id = g_strdup(radios_programme_queue_row_id(GTK_LIST_BOX_ROW(item->data))); break;
        }
    g_list_free(rows); g_assert_true(g_uuid_string_is_valid(id)); return id;
}
static void assignment_assert_saved_record(JsonNode *expected)
{
    JsonObject *record = json_node_get_object(expected);
    JsonNode *actual = document_read(radios_json_string(record, "kind", ""), radios_json_string(record, "name", ""));
    g_assert_nonnull(actual); g_assert_true(json_node_equal(actual, expected)); json_node_free(actual);
}
static void test_general_assignment_isolated_semi_row_pcm(void)
{
    Window window = window_open(); navigate(&window, "broadcast");
    wait_until(scope_workspace_ready, window.ctx->players, "general event assignment workspace baseline");
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(window.root, "programme-general-continuous"))));
    RadioSPlayerWeekly *driver = general_driver(&window);
    JsonNode *schedule_song = routing_short_markers(media_import(&window, "Genel takvim müziği korunur", 810, .22, "music"), 1800);
    JsonNode *assigned_a = routing_short_markers(media_import(&window, "Genel evente atanmış ilk müzik", 1700, .22, "music"), 1800);
    JsonNode *assigned_b = routing_short_markers(media_import(&window, "Genel evente atanmış son müzik", 2350, .22, "music"), 1800);
    JsonNode *schedule_playlist = playlist_create("Genel takvim kaynak listesi korunur", schedule_song, NULL);
    JsonNode *assigned_playlist = playlist_create("Genel eventin bağımsız atama kaynağı", assigned_a, assigned_b);
    JsonNode *target_config = programme_config(schedule_song, NULL);
    gchar cluster[37]; RadioSAutomationScope target_scope;
    JsonObject *stored_target = assignment_target(&window, json_node_get_object(target_config), cluster, &target_scope);
    JsonNode *manual_template = programme_config(schedule_song, NULL);
    JsonNode *saved_template = document_save("programme", "Mevcut program", json_node_get_object(manual_template));
    gint64 start = g_get_real_time() / G_TIME_SPAN_DAY * G_TIME_SPAN_DAY;
    JsonNode *configuration = weekly_pcm_configuration(schedule_playlist, NULL, start, 1440, "Genel akış korunur");
    JsonObject *calendar = json_object_get_object_member(json_node_get_object(configuration), "calendar");
    general_single_batch(calendar, schedule_playlist);
    JsonNode *saved_weekly = document_save("weekly_programme", "Genel atama haftalık kaynak", json_node_get_object(configuration));
    JsonNode *saved_calendar = document_save("automation_calendar", "main", calendar);
    radios_player_weekly_general_refresh(driver);
    GeneralReady ready = {&window, driver, "Genel akış korunur", start};
    wait_until(general_current_mixed_ready, &ready, "actual current general calendar event is projected without starting global audio");
    gchar *origin = assignment_first_block_id(&window);
    GtkWidget *properties = assignment_properties(&window, origin);
    combo(properties, "event-playback-player", "3"); combo(properties, "event-playback-after", "stop");
    click(properties, "programme-block-assign");
    GtkWidget *builder = dialog_find("local-event-dialog"); g_assert_nonnull(builder); g_object_ref(builder);
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(find(builder, "local-event-insert"))), ==, "Evente ata ve kümesizde çal");
    combo(builder, "local-event-source-type", "playlist");
    AssignmentSource source = {builder, radios_json_string(json_node_get_object(assigned_playlist), "_id", "")};
    wait_until(assignment_source_available, &source, "general event assignment exposes an independent playlist");
    click(builder, "local-event-add"); click(builder, "local-event-insert");
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(window.ctx);
    AssignmentRuntime playing = {&window, origin, "Çalıyor", FALSE};
    for (guint index = 0; index < 2; ++index) {
        RadioSAudioRunSnapshot run = player_run_wait(engine, 2, RADIOS_AUDIO_RUN_PLAYING, index, 4800);
        g_assert_cmpuint(run.count, ==, 2); g_assert_cmpint(run.deck_current, ==, 2);
        for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) g_assert_cmpint(run.owned[deck], ==, deck == 2);
        wait_until(assignment_runtime_matches, &playing, "general projected origin row reflects its independent SEMI assignment");
        goffset pcm = capture_offset("MAIN"); run_for(250);
        g_assert_cmpfloat(tone_energy("MAIN", pcm, index ? 2350 : 1700), >, .05);
        g_assert_cmpfloat(tone_energy("MAIN", pcm, 810), <, .01);
        RadioSAudioRunSnapshot global; radios_audio_engine_runner_snapshot(engine, &global);
        g_assert_cmpint(global.state, ==, RADIOS_AUDIO_RUN_IDLE);
    }
    player_run_wait(engine, 2, RADIOS_AUDIO_RUN_FINISHED, 1, 0);
    AssignmentRuntime done = {&window, origin, "Tamamlandı", TRUE};
    wait_until(assignment_runtime_matches, &done, "general event assignment finishes with a stable original-row badge");
    gtk_widget_destroy(builder); g_object_unref(builder); gtk_widget_destroy(properties); g_object_unref(properties);
    properties = assignment_properties(&window, origin);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(properties, "event-playback-player"))), ==, "3");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(properties, "event-playback-after"))), ==, "stop");
    click(properties, "programme-block-assign"); builder = dialog_find("local-event-dialog"); g_assert_nonnull(builder); g_object_ref(builder);
    JsonNode *reopened = radios_local_event_ui_snapshot(builder);
    JsonArray *sources = json_object_get_array_member(json_node_get_object(reopened), "sources");
    g_assert_cmpuint(json_array_get_length(sources), ==, 1);
    g_assert_cmpstr(radios_json_string(json_array_get_object_element(sources, 0), "source_id", ""), ==, source.id);
    json_node_free(reopened);
    gtk_widget_destroy(builder); g_object_unref(builder); gtk_widget_destroy(properties); g_object_unref(properties);
    GtkWidget *row = assignment_row(&window, origin); g_assert_nonnull(row); click(row, "programme-row-play");
    RadioSAudioRunSnapshot replay = player_run_wait(engine, 2, RADIOS_AUDIO_RUN_PLAYING, 0, 4800);
    g_assert_cmpuint(replay.count, ==, 2); wait_until(assignment_runtime_matches, &playing, "general Event S reuses the cached source and clears its previous finished state");
    goffset pcm = capture_offset("MAIN"); run_for(250); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1700), >, .05);
    radios_event_playout_cancel(window.ctx, 2);
    RadioSAutomationScope source_scope = {.kind = RADIOS_AUTOMATION_SCOPE_PLAYER, .deck = 2}; GError *error = NULL;
    require_call(radios_player_scope_execution_action(radios_player_scope_execution_get(window.ctx), &source_scope, RADIOS_PLAYOUT_STOP, &error), &error);
    assignment_assert_saved_record(saved_template); assignment_assert_saved_record(saved_calendar); assignment_assert_saved_record(saved_weekly);
    assert_saved_playlist_unchanged(schedule_playlist); assert_saved_playlist_unchanged(assigned_playlist);
    JsonObject *actual_target = NULL;
    require_call(radios_player_workspace_get_programme(window.ctx->players, &target_scope, &actual_target, &error), &error);
    JsonNode *actual_node = json_node_new(JSON_NODE_OBJECT), *stored_node = json_node_new(JSON_NODE_OBJECT);
    json_node_set_object(actual_node, actual_target); json_node_set_object(stored_node, stored_target);
    g_assert_true(json_node_equal(actual_node, stored_node)); json_node_free(actual_node); json_node_free(stored_node); json_object_unref(actual_target);
    RadioSAudioRunSnapshot global; radios_audio_engine_runner_snapshot(engine, &global); g_assert_cmpint(global.state, ==, RADIOS_AUDIO_RUN_IDLE);
    g_assert_false(weekly_snapshot(driver).armed);
    radios_audio_engine_unref(engine); json_object_unref(stored_target); g_free(origin);
    json_node_free(saved_template); json_node_free(saved_calendar); json_node_free(saved_weekly); json_node_free(configuration); json_node_free(manual_template);
    json_node_free(target_config); json_node_free(schedule_playlist); json_node_free(assigned_playlist);
    json_node_free(schedule_song); json_node_free(assigned_a); json_node_free(assigned_b); window_close(&window);
    g_test_message("Actual current general calendar event accepts explicit independent assignment, displays SEMI PCM and completion, reopens runtime sources/routing and replays from Event S while every saved programme, weekly/calendar, playlist and cluster queue remains unchanged.");
}

static int event_assignment_test_run(void)
{
    g_test_add_func("/native-playout/event-selected-assignment-save-reopen-SEMI-row-PCM", test_selected_assignment_saved_reopened_semi_row_pcm);
    g_test_add_func("/native-playout/event-selected-assignment-stale-target-fence", test_selected_assignment_stale_target_fence);
    g_test_add_func("/native-playout/event-general-assignment-isolated-SEMI-row-PCM", test_general_assignment_isolated_semi_row_pcm);
    return g_test_run();
}
int main(int argc, char **argv) { return radios_event_assignment_fixture_main(argc, argv); }
