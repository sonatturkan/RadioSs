#define _POSIX_C_SOURCE 200809L
#include <glib.h>
static int local_event_test_run(void);
#define main radios_local_event_fixture_main
#define g_test_run local_event_test_run
#include "test_playout_ui.c"
#undef g_test_run
#undef main
#include "radios/local_event_ui.h"
#include "radios/header_navigation.h"

typedef struct { GtkWidget *dialog; const gchar *id; } LocalChoice;
static gboolean local_choice_ready(gpointer data)
{
    LocalChoice *choice = data;
    return gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(choice->dialog, "local-event-source")), choice->id);
}
static void local_add(GtkWidget *dialog, const gchar *type, JsonNode *record)
{
    combo(dialog, "local-event-source-type", type);
    LocalChoice choice = {dialog, radios_json_string(json_node_get_object(record), "_id", "")};
    wait_until(local_choice_ready, &choice, "real local event catalogue exposes authenticated source");
    click(dialog, "local-event-add");
}
static gboolean local_saved(gpointer data)
{
    GtkWidget *dialog = data;
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(dialog, "local-event-saved")));
    return g_uuid_string_is_valid(id) && gtk_widget_is_sensitive(find(dialog, "local-event-new")) &&
        !gtk_widget_is_sensitive(find(dialog, "local-event-save"));
}
static gboolean local_loaded(gpointer data)
{
    GtkWidget *dialog = data;
    return documents_ready(dialog) &&
        !g_strcmp0(gtk_entry_get_text(GTK_ENTRY(find(dialog, "local-event-name"))), "Sadece yerel event");
}
typedef struct { guint calls; JsonNode *last; } LocalInserted;
static gboolean local_insert(RadioSViewContext *ctx, JsonObject *configuration, gpointer data, GError **error)
{
    (void)ctx; (void)error; LocalInserted *inserted = data; ++inserted->calls;
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, configuration);
    g_clear_pointer(&inserted->last, json_node_free); inserted->last = deep_copy(node); json_node_free(node);
    return TRUE;
}
typedef struct { LocalInserted inserted; GtkWidget *dialog; guint deck; gchar cluster[37]; } LocalPlayed;
static gboolean local_play(RadioSViewContext *ctx, JsonObject *configuration, gpointer data, GError **error)
{
    LocalPlayed *played = data;
    g_assert_true(radios_local_event_ui_playback(played->dialog, &played->deck, played->cluster));
    return local_insert(ctx, configuration, &played->inserted, error);
}
static void local_source_assert(JsonArray *sources, guint index, const gchar *type, JsonNode *record)
{
    JsonObject *source = json_array_get_object_element(sources, index);
    g_assert_cmpstr(radios_json_string(source, "type", ""), ==, type);
    g_assert_cmpstr(radios_json_string(source, "source_id", ""), ==,
        radios_json_string(json_node_get_object(record), "_id", ""));
    g_assert_true(g_uuid_string_is_valid(radios_json_string(source, "id", "")));
}
static void local_document_unchanged(const gchar *kind, const gchar *name, JsonNode *expected)
{
    JsonNode *actual = document_read(kind, name); g_assert_nonnull(actual);
    g_assert_true(json_node_equal(actual, expected)); json_node_free(actual);
}
static void local_event_builder_isolated_pcm(void)
{
    Window window = window_open();
    wait_until(scope_workspace_ready, window.ctx->players, "local event authenticated topology baseline");
    RadioSPlayerTopology topology = *radios_player_workspace_topology(window.ctx->players);
    gchar continuation[37]; guint grouped_deck; GError *error = NULL;
    require_call(radios_player_topology_add_cluster(&topology, "Event sonrası küme", continuation, &error), &error);
    require_call(radios_player_topology_add_player(&topology, continuation, &grouped_deck, &error), &error);
    require_call(radios_player_workspace_replace(window.ctx->players, &topology, &error), &error);
    JsonNode *music = media_import_rhythm(&window, "Yerel event yayını korur", 1270, .3, "music", 120);
    JsonNode *playlist = playlist_create("Yerel event kaynak playlist", music, music);
    GDateTime *now = g_date_time_new_now_utc(); gchar *date = g_date_time_format(now, "%Y-%m-%d"); g_date_time_unref(now);
    JsonNode *calendar_config = calendar_schedule_config(date, radios_json_string(json_node_get_object(playlist), "_id", ""), 0); g_free(date);
    JsonNode *calendar = document_save("automation_calendar", "Yerel event şeması", json_node_get_object(calendar_config));
    gint64 day = g_get_real_time() / G_TIME_SPAN_DAY * G_TIME_SPAN_DAY;
    JsonNode *weekly_config = weekly_pcm_configuration(playlist, NULL, day, 1, "Yerel event haftalık kaynak");
    JsonNode *weekly = document_save("weekly_programme", "Yerel event haftalık", json_node_get_object(weekly_config));
    seed_programme(&window, music, music);
    JsonNode *programme = document_read("programme", "Mevcut program"); g_assert_nonnull(programme);
    LocalInserted seeded = {0};
    GtkWidget *seed_dialog = radios_local_event_ui_open(window.ctx, GTK_WINDOW(window.root), local_insert, &seeded);
    g_object_ref(seed_dialog);
    radios_local_event_ui_set_play_handler(seed_dialog, local_insert, &seeded);
    JsonObject *seed = json_object_new(), *seed_source = json_object_new(), *seed_route = json_object_new();
    JsonArray *seed_sources = json_array_new(); gchar *occurrence = g_uuid_string_random();
    json_object_set_int_member(seed, "schema", 1); json_object_set_string_member(seed, "title", "Seçili eventin kaynakları");
    json_object_set_string_member(seed, "kind", "music"); json_object_set_array_member(seed, "sources", seed_sources);
    json_object_set_string_member(seed_source, "id", occurrence); g_free(occurrence);
    json_object_set_string_member(seed_source, "type", "playlist");
    json_object_set_string_member(seed_source, "source_id", radios_json_string(json_node_get_object(playlist), "_id", ""));
    json_array_add_object_element(seed_sources, seed_source);
    json_object_set_int_member(seed_route, "player", 1); json_object_set_string_member(seed_route, "next_cluster_id", continuation);
    json_object_set_object_member(seed, "playback", seed_route);
    g_assert_true(radios_local_event_ui_set_configuration(seed_dialog, seed));
    radios_local_event_ui_set_insert_label(seed_dialog, "Seçili evente ata");
    g_assert_cmpstr(gtk_button_get_label(GTK_BUTTON(find(seed_dialog, "local-event-insert"))), ==, "Seçili evente ata");
    JsonNode *seed_snapshot = radios_local_event_ui_snapshot(seed_dialog);
    JsonNode *seed_expected = json_node_new(JSON_NODE_OBJECT); json_node_set_object(seed_expected, seed);
    g_assert_true(json_node_equal(seed_snapshot, seed_expected)); json_node_free(seed_snapshot); json_node_free(seed_expected);
    guint seeded_deck; gchar seeded_cluster[37];
    g_assert_true(radios_local_event_ui_playback(seed_dialog, &seeded_deck, seeded_cluster));
    g_assert_cmpuint(seeded_deck, ==, 0); g_assert_cmpstr(seeded_cluster, ==, continuation);
    g_assert_false(gtk_widget_is_sensitive(find(seed_dialog, "local-event-save")));
    gtk_entry_set_text(GTK_ENTRY(find(seed_dialog, "local-event-name")), "Düzenlenmiş atama korunur");
    g_assert_false(radios_local_event_ui_set_configuration(seed_dialog, seed));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(seed_dialog, "local-event-name"))), ==, "Düzenlenmiş atama korunur");
    g_assert_cmpuint(seeded.calls, ==, 0); json_object_unref(seed);
    gtk_widget_destroy(seed_dialog); g_object_unref(seed_dialog);
    LocalInserted inserted = {0};
    GtkWidget *dialog = radios_local_event_ui_open(window.ctx, GTK_WINDOW(window.root), local_insert, &inserted);
    g_object_ref(dialog);
    g_assert_false(gtk_widget_get_visible(find(dialog, "local-event-play")));
    g_assert_false(gtk_widget_get_visible(find(dialog, "local-event-playback-controls")));
    LocalPlayed played = {.dialog = dialog};
    radios_local_event_ui_set_play_handler(dialog, local_play, &played);
    g_assert_true(gtk_widget_get_visible(find(dialog, "local-event-play")));
    g_assert_false(gtk_widget_is_sensitive(find(dialog, "local-event-play")));
    guint selected_deck = 0; gchar selected_cluster[37];
    g_assert_true(radios_local_event_ui_playback(dialog, &selected_deck, selected_cluster));
    g_assert_cmpuint(selected_deck, ==, G_MAXUINT); g_assert_cmpstr(selected_cluster, ==, "");
    gchar *grouped_id = g_strdup_printf("%u", grouped_deck + 1);
    g_assert_false(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(dialog, "local-event-player")), grouped_id)); g_free(grouped_id);
    g_assert_true(radios_local_event_ui_set_event(dialog, "Sadece yerel event", "sweeper"));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(dialog, "local-event-name"))), ==, "Sadece yerel event");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(dialog, "local-event-kind"))), ==, "sweeper");
    g_assert_false(radios_local_event_ui_set_event(dialog, "Düzenlenmiş event korunmalı", "music"));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(dialog, "local-event-status"))), ==,
        "Açık Event taslağı korundu; farklı bir Event için Yeni event seçin.");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(dialog, "local-event-name"))), ==, "Sadece yerel event");
    local_add(dialog, "playlist", playlist);
    local_add(dialog, "automation_calendar", calendar);
    local_add(dialog, "weekly_programme", weekly);
    GtkListBox *sources = GTK_LIST_BOX(find(dialog, "local-event-sources"));
    gtk_list_box_select_row(sources, gtk_list_box_get_row_at_index(sources, 2));
    click(dialog, "local-event-up");
    g_assert_cmpuint(inserted.calls, ==, 0); g_assert_cmpuint(played.inserted.calls, ==, 0);
    navigate(&window, "broadcast"); click(window.root, "programme-play");
    run_wait(&window, RADIOS_PLAYOUT_PLAYING, 0, 200);
    RadioSAudioRunSnapshot before = {0}; g_assert_true(radios_playout_ui_navigation_snapshot(window.ctx, &before));
    goffset pcm = capture_offset("MAIN");
    click(dialog, "local-event-save"); wait_until(local_saved, dialog, "local-only save ACK and stable saved UUID");
    gchar *id = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(dialog, "local-event-saved"))));
    JsonNode *saved = document_read("local_event", "Sadece yerel event"); g_assert_nonnull(saved);
    g_assert_cmpstr(radios_json_string(json_node_get_object(saved), "_id", ""), ==, id);
    JsonNode *snapshot = radios_local_event_ui_snapshot(dialog);
    JsonObject *configuration = json_node_get_object(snapshot);
    g_assert_cmpuint(json_object_get_size(configuration), ==, 5);
    g_assert_true(radios_local_event_ui_set_playback(dialog, 0, continuation));
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(dialog, "local-event-player"))), ==, "1");
    g_assert_true(radios_local_event_ui_playback(dialog, &selected_deck, selected_cluster));
    g_assert_cmpuint(selected_deck, ==, 0); g_assert_cmpstr(selected_cluster, ==, continuation);
    g_assert_false(radios_local_event_ui_set_playback(dialog, grouped_deck, continuation));
    g_assert_false(radios_local_event_ui_set_playback(dialog, G_MAXUINT, "not-a-cluster"));
    g_assert_true(radios_local_event_ui_playback(dialog, &selected_deck, selected_cluster));
    g_assert_cmpuint(selected_deck, ==, 0); g_assert_cmpstr(selected_cluster, ==, continuation);
    JsonNode *routed_snapshot = radios_local_event_ui_snapshot(dialog);
    g_assert_false(json_node_equal(snapshot, routed_snapshot));
    JsonObject *playback = json_object_get_object_member(json_node_get_object(routed_snapshot), "playback");
    g_assert_cmpint(radios_json_int(playback, "player", -1), ==, 1);
    g_assert_cmpstr(radios_json_string(playback, "next_cluster_id", ""), ==, continuation);
    g_assert_true(gtk_widget_is_sensitive(find(dialog, "local-event-save")));
    click(dialog, "local-event-save"); wait_until(local_saved, dialog, "explicit save persists player and continuation with the recipe");
    json_node_free(saved); saved = document_read("local_event", "Sadece yerel event");
    g_assert_true(json_node_equal(routed_snapshot, json_object_get_member(json_node_get_object(saved), "config")));
    json_node_free(snapshot); snapshot = routed_snapshot; configuration = json_node_get_object(snapshot);
    g_assert_false(gtk_widget_is_sensitive(find(dialog, "local-event-save")));
    g_assert_cmpuint(played.inserted.calls, ==, 0);
    g_assert_cmpstr(radios_json_string(configuration, "kind", ""), ==, "sweeper");
    JsonArray *refs = json_object_get_array_member(configuration, "sources");
    g_assert_cmpuint(json_array_get_length(refs), ==, 3);
    local_source_assert(refs, 0, "playlist", playlist);
    local_source_assert(refs, 1, "weekly_programme", weekly);
    local_source_assert(refs, 2, "automation_calendar", calendar);
    gtk_widget_destroy(dialog); g_object_unref(dialog); run_for(50);
    dialog = radios_local_event_ui_open(window.ctx, GTK_WINDOW(window.root), local_insert, &inserted); g_object_ref(dialog);
    played.dialog = dialog; radios_local_event_ui_set_play_handler(dialog, local_play, &played);
    g_assert_true(radios_local_event_ui_playback(dialog, &selected_deck, selected_cluster));
    g_assert_cmpuint(selected_deck, ==, G_MAXUINT); g_assert_cmpstr(selected_cluster, ==, "");
    /* Reopen from the saved UUID rather than a draft retained by the test. */
    LocalChoice choice = {dialog, id};
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(dialog, "local-event-saved")), choice.id) && g_get_monotonic_time() < deadline) pump();
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(dialog, "local-event-saved"))), ==, id);
    wait_until(local_loaded, dialog, "saved local event title and ordered references reloaded from private DB");
    JsonNode *reopened = radios_local_event_ui_snapshot(dialog); g_assert_true(json_node_equal(reopened, snapshot)); json_node_free(reopened);
    g_assert_true(radios_local_event_ui_playback(dialog, &selected_deck, selected_cluster));
    g_assert_cmpuint(selected_deck, ==, 0); g_assert_cmpstr(selected_cluster, ==, continuation);
    g_assert_false(gtk_widget_is_sensitive(find(dialog, "local-event-save")));
    g_assert_false(radios_local_event_ui_set_configuration(dialog, configuration));
    sources = GTK_LIST_BOX(find(dialog, "local-event-sources"));
    gtk_list_box_select_row(sources, gtk_list_box_get_row_at_index(sources, 0)); click(dialog, "local-event-remove");
    g_assert_cmpuint(inserted.calls, ==, 0);
    /* Editing is a private draft. No source document or accepted queue changes. */
    local_document_unchanged("programme", "Mevcut program", programme);
    local_document_unchanged("automation_calendar", "Yerel event şeması", calendar);
    local_document_unchanged("weekly_programme", "Yerel event haftalık", weekly);
    local_document_unchanged("local_event", "Sadece yerel event", saved);
    assert_saved_playlist_unchanged(playlist);
    RadioSAudioRunSnapshot after = {0}; g_assert_true(radios_playout_ui_navigation_snapshot(window.ctx, &after));
    g_assert_cmpuint(after.epoch, ==, before.epoch); g_assert_cmpuint(after.index, ==, before.index);
    g_assert_cmpint(after.state, ==, RADIOS_AUDIO_RUN_PLAYING);
    g_assert_cmpuint(after.actual_current_frame, >, before.actual_current_frame);
    run_for(150); g_assert_cmpfloat(tone_energy("MAIN", pcm, 1270), >, .04);
    g_assert_cmpuint(inserted.calls, ==, 0); g_assert_cmpuint(played.inserted.calls, ==, 0);
    combo(dialog, "local-event-player", "1"); combo(dialog, "local-event-after", continuation);
    click(dialog, "local-event-play");
    g_assert_cmpuint(played.inserted.calls, ==, 1); g_assert_cmpuint(inserted.calls, ==, 0);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(dialog, "local-event-status"))), ==,
        "Event kümesiz oynatıcıya Yarı AutoDJ ile gönderildi.");
    g_assert_cmpuint(played.deck, ==, 0); g_assert_cmpstr(played.cluster, ==, continuation);
    g_assert_cmpuint(json_object_get_size(json_node_get_object(played.inserted.last)), ==, 5);
    JsonNode *current_recipe = radios_local_event_ui_snapshot(dialog);
    g_assert_true(json_node_equal(current_recipe, played.inserted.last)); json_node_free(current_recipe);
    radios_local_event_ui_set_play_handler(dialog, NULL, NULL);
    g_assert_false(gtk_widget_get_visible(find(dialog, "local-event-play")));
    g_assert_false(gtk_widget_is_sensitive(find(dialog, "local-event-play")));
    g_assert_false(gtk_widget_get_visible(find(dialog, "local-event-playback-controls")));
    click(dialog, "local-event-insert"); g_assert_cmpuint(inserted.calls, ==, 1);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(dialog, "local-event-status"))), ==, "Event kaynakları hazırlanıyor…");
    radios_local_event_ui_insert_result(dialog, "Yerel event seçili sıraya eklendi.");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(find(dialog, "local-event-status"))), ==, "Yerel event seçili sıraya eklendi.");
    g_assert_cmpuint(json_array_get_length(json_object_get_array_member(json_node_get_object(inserted.last), "sources")), ==, 2);
    local_document_unchanged("programme", "Mevcut program", programme);
    gtk_widget_destroy(dialog);
    radios_local_event_ui_insert_result(dialog, "Kapanmış penceredeki geç sonuç");
    g_object_unref(dialog); click(window.root, "programme-stop"); run_for(50);
    json_node_free(played.inserted.last); json_node_free(inserted.last); json_node_free(snapshot); json_node_free(saved); g_free(id);
    json_node_free(programme); json_node_free(calendar_config); json_node_free(calendar); json_node_free(weekly_config); json_node_free(weekly);
    json_node_free(playlist); json_node_free(music); window_close(&window);
    g_test_message("Local builder save/reopen preserves exact sources and saved independent-player/cluster choices; getter and draft edits preserve current PCM, and Play/Insert invoke separate explicit handlers.");
}
static int local_event_test_run(void)
{
    g_test_add_func("/native-playout/local-event-builder-save-reopen-isolated-PCM", local_event_builder_isolated_pcm);
    return g_test_run();
}
int main(int argc, char **argv) { return radios_local_event_fixture_main(argc, argv); }
