#include "radios/player_waveform.h"
#include "radios/media_ui.h"
#include "radios/programme_model.h"
#include "radios/audio.h"
#include "radios/data.h"
#include <glib/gstdio.h>
#include <string.h>

#define SID "33333333-3333-4333-8333-333333333333"
#define MID "55555555-5555-4555-8555-555555555555"
#define VID "66666666-6666-4666-8666-666666666666"
#define ITEM "77777777-7777-4777-8777-777777777777"
#define FILE_ID "88888888-8888-4888-8888-888888888888"
#define SHA "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"

typedef struct {
    RadioSViewContext ctx;
    RadioSConfig config;
    RadioSDataClient *client;
    GtkWidget *window, *card, *wave;
    RadioSAudioUiSnapshot snapshot;
    JsonNode *body;
    RadioSDataCallback callback;
    gpointer user;
    GDestroyNotify destroy;
    GWeakRef owner;
    gchar *root;
    guint requests;
} Fixture;

/* The executable links the real UI library (the inspector embeds the shared
 * marker editor); these wrappers stand in for the data service and the audio
 * controller of the deck under test. */
static Fixture *fixture_of(RadioSViewContext *ctx) { return ctx->notify_data; }
RadioSDataClient *__wrap_radios_data_client_acquire(RadioSViewContext *ctx)
{ return radios_data_client_ref(fixture_of(ctx)->client); }
gboolean __wrap_radios_data_connected(RadioSViewContext *ctx) { return fixture_of(ctx)->client != NULL; }
const gchar *__wrap_radios_data_station_id(RadioSViewContext *ctx) { return radios_data_client_station_id(fixture_of(ctx)->client); }
void __wrap_radios_data_register_refresh(RadioSViewContext *ctx, GtkWidget *owner, RadioSDataRefresh refresh)
{ (void)ctx; (void)owner; (void)refresh; }
JsonNode *__wrap_radios_data_body(RadioSViewContext *ctx)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new();
    json_object_set_string_member(object, "station_id", radios_data_client_station_id(fixture_of(ctx)->client)); json_node_take_object(node, object); return node;
}
gboolean __wrap_radios_audio_ui_get_snapshot(RadioSViewContext *ctx, guint deck, RadioSAudioUiSnapshot *snapshot)
{ if (deck != 0) return FALSE; *snapshot = fixture_of(ctx)->snapshot; return TRUE; }
RadioSAudioEngine *__wrap_radios_audio_ui_engine_acquire(RadioSViewContext *ctx) { (void)ctx; return NULL; }
void __wrap_radios_audio_engine_unref(RadioSAudioEngine *engine) { (void)engine; g_assert_not_reached(); }
gboolean __wrap_radios_audio_engine_set_deck_rhythm(RadioSAudioEngine *engine, guint deck, guint64 generation, const RadioSRhythmAnalysis *rhythm, GError **error)
{ (void)engine; (void)deck; (void)generation; (void)rhythm; (void)error; g_assert_not_reached(); return FALSE; }
GCancellable *__wrap_radios_media_rhythm_save_async(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail,
    const RadioSRhythmAnalysis *grid, RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{
    (void)detail; (void)grid; Fixture *fixture = fixture_of(ctx); g_assert_null(fixture->callback);
    fixture->callback = callback; fixture->user = user; fixture->destroy = destroy;
    g_weak_ref_set(&fixture->owner, owner); return g_cancellable_new();
}
GCancellable *__wrap_radios_media_rhythm_analyze_async(RadioSViewContext *ctx, GtkWidget *owner, const gchar *media_id,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{ (void)ctx; (void)owner; (void)media_id; (void)callback; (void)user; (void)destroy; return NULL; }
gboolean __wrap_radios_data_request(RadioSViewContext *ctx, GtkWidget *owner, const gchar *method, const gchar *path,
    JsonNode *body, RadioSDataCallback callback, gpointer user, GDestroyNotify destroy)
{
    Fixture *fixture = fixture_of(ctx); g_assert_null(fixture->body);
    g_assert_cmpstr(method, ==, "POST"); g_assert_cmpstr(path, ==, "/v1/workflows/desktop-media/markers-save");
    fixture->body = json_node_new(JSON_NODE_OBJECT); json_node_take_object(fixture->body, radios_programme_clone_object(json_node_get_object(body)));
    fixture->callback = callback; fixture->user = user; fixture->destroy = destroy; g_weak_ref_set(&fixture->owner, owner); ++fixture->requests; return TRUE;
}
static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (!g_strcmp0(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = find(item->data, name);
    g_list_free(children); return found;
}
static GtkWidget *dialog_find(void)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *dialog = NULL;
    for (GList *item = windows; item && !dialog; item = item->next)
        if (!g_strcmp0(gtk_widget_get_name(item->data), "deck-0-waveform-dialog")) dialog = item->data;
    g_list_free(windows); return dialog;
}
static GtkWidget *control(const gchar *name)
{
    GtkWidget *dialog = dialog_find(); g_assert_nonnull(dialog);
    GtkWidget *widget = find(dialog, name); g_assert_nonnull(widget); return widget;
}
static JsonNode *parse(const gchar *text)
{
    JsonParser *parser = json_parser_new(); GError *error = NULL;
    g_assert_true(json_parser_load_from_data(parser, text, -1, &error)); g_assert_no_error(error);
    JsonNode *node = json_parser_steal_root(parser); g_object_unref(parser); return node;
}
static void pump(void) { while (g_main_context_iteration(NULL, FALSE)) {} }
static void remove_tree(const gchar *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_DIR)) {
        GDir *directory = g_dir_open(path, 0, NULL); const gchar *name;
        while (directory && (name = g_dir_read_name(directory))) { gchar *child = g_build_filename(path, name, NULL); remove_tree(child); g_free(child); }
        if (directory) g_dir_close(directory);
        g_rmdir(path);
    } else g_unlink(path);
}
static void fixture_start(Fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture)); GError *error = NULL; g_weak_ref_init(&fixture->owner, NULL);
    fixture->root = g_dir_make_tmp("radios-waveform-markers-XXXXXX", &error); g_assert_no_error(error);
    gchar *directory = g_build_filename(fixture->root, "config", NULL); g_assert_cmpint(g_mkdir(directory, 0700), ==, 0);
    gchar *credentials = g_build_filename(directory, "credentials.json", NULL); g_free(directory);
    fixture->config.project_root = g_strdup(fixture->root); fixture->config.database_file = g_build_filename(fixture->root, "RadioS.sdb", NULL);
    fixture->config.service_url = g_strdup("http://127.0.0.1:1"); fixture->config.connect_timeout_seconds = fixture->config.request_timeout_seconds = 1;
    g_assert_true(g_file_set_contents(fixture->config.database_file, "private journal", -1, &error)); g_assert_no_error(error);
    gchar *metadata = g_strconcat(fixture->config.database_file, ".meta", NULL);
    g_assert_true(g_file_set_contents(metadata, "{\"schema_version\":4,\"store_id\":\"11111111-1111-4111-8111-111111111111\"}", -1, &error));
    g_assert_no_error(error); g_free(metadata);
    g_assert_true(g_file_set_contents(credentials, "{\"schema_version\":1,\"store_id\":\"11111111-1111-4111-8111-111111111111\","
        "\"user_id\":\"22222222-2222-4222-8222-222222222222\",\"station_id\":\"" SID "\","
        "\"endpoint_id\":\"44444444-4444-4444-8444-444444444444\",\"database_file\":\"RadioS.sdb\","
        "\"api_token\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"endpoint_token\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"}", -1, &error));
    g_assert_no_error(error); g_assert_cmpint(g_chmod(credentials, 0600), ==, 0); g_free(credentials);
    fixture->client = radios_data_client_new(&fixture->config, &error); g_assert_no_error(error); g_assert_nonnull(fixture->client);
    fixture->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(fixture->window);
    gtk_window_set_default_size(GTK_WINDOW(fixture->window), 430, 110);
    fixture->card = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); g_object_ref_sink(fixture->card); gtk_container_add(GTK_CONTAINER(fixture->window), fixture->card);
    fixture->ctx.parent = GTK_WINDOW(fixture->window); fixture->ctx.notify_data = fixture; fixture->ctx.data = (RadioSData *)fixture;
    fixture->snapshot.generation = 4; fixture->snapshot.duration_ms = 12000;
    g_strlcpy(fixture->snapshot.media_id, MID, sizeof fixture->snapshot.media_id); g_strlcpy(fixture->snapshot.version_id, VID, sizeof fixture->snapshot.version_id);
    JsonNode *detail = parse("{\"media\":{\"_id\":\"" MID "\",\"_rev\":1,\"station_id\":\"" SID "\",\"version_id\":\"" VID "\",\"item_id\":\"" ITEM "\",\"media_file_id\":\"" FILE_ID "\"},"
        "\"version\":{\"_id\":\"" VID "\",\"duration_ms\":12000,\"verified\":true,\"object_available\":true,\"sha256\":\"" SHA "\"},"
        "\"analysis\":{\"sha256\":\"" SHA "\",\"waveform_frames_per_bin\":4000,\"sample_rate\":8000,"
        "\"waveform\":[-0.4,0.4,-0.5,0.5,-0.6,0.6,-0.5,0.5,-0.4,0.4,-0.5,0.5,-0.6,0.6,-0.5,0.5,"
        "-0.4,0.4,-0.5,0.5,-0.6,0.6,-0.5,0.5,-0.4,0.4,-0.5,0.5,-0.6,0.6,-0.5,0.5,"
        "-0.4,0.4,-0.5,0.5,-0.6,0.6,-0.5,0.5,-0.4,0.4,-0.5,0.5,-0.6,0.6,-0.5,0.5],"
        "\"rhythm\":{\"schema\":1,\"algorithm\":\"radios-manual-grid-v1\",\"bpm\":120,\"first_beat_ms\":0,\"downbeat_ms\":0,\"confidence\":1,\"analyzed_ms\":12000}},\"markers\":null}");
    g_object_set_data_full(G_OBJECT(fixture->card), "radios-deck-media-detail", detail, (GDestroyNotify)json_node_free);
    fixture->wave = radios_player_waveform_new(&fixture->ctx, fixture->card, 0); gtk_box_pack_start(GTK_BOX(fixture->card), fixture->wave, TRUE, TRUE, 0);
    radios_player_waveform_update(fixture->wave, &fixture->snapshot, FALSE); gtk_widget_show_all(fixture->window); pump();
    g_assert_true(radios_player_waveform_edit_grid(fixture->wave)); pump(); g_assert_nonnull(dialog_find());
}
static void fixture_clear(Fixture *fixture)
{
    gtk_widget_destroy(fixture->window); g_object_unref(fixture->window); g_object_unref(fixture->card); pump();
    if (fixture->destroy) fixture->destroy(fixture->user);
    g_clear_pointer(&fixture->body, json_node_free); g_weak_ref_clear(&fixture->owner);
    radios_data_client_unref(fixture->client); radios_config_clear(&fixture->config); remove_tree(fixture->root); g_free(fixture->root);
}
/* The inspector hosts the shared marker editor: a click on its timeline moves
 * the cursor; at zoom 1 the whole track spans the 16 px margins. */
static void select_point(gdouble milliseconds)
{
    GtkWidget *area = control("qwf-timeline");
    GdkEventButton event = {0}; event.type = GDK_BUTTON_PRESS; event.button = 1;
    event.x = 16 + (gtk_widget_get_allocated_width(area) - 32) * milliseconds / 12000; event.y = gtk_widget_get_allocated_height(area) / 2.0;
    gboolean handled = FALSE; g_signal_emit_by_name(area, "button-press-event", &event, &handled); g_assert_true(handled); pump();
}
static void set_marker(guint index)
{
    gchar *name = g_strdup_printf("qwf-set-%u", index); GtkWidget *button = control(name); g_free(name);
    g_assert_true(gtk_widget_get_sensitive(button)); gtk_button_clicked(GTK_BUTTON(button)); pump();
}
static void save(void)
{
    GtkWidget *button = control("qwf-markers-save");
    g_assert_true(gtk_widget_get_sensitive(button)); gtk_button_clicked(GTK_BUTTON(button)); pump();
}
static void clear_points(void)
{
    GtkWidget *button = control("deck-0-waveform-autodj-clear");
    g_assert_true(gtk_widget_get_sensitive(button)); gtk_button_clicked(GTK_BUTTON(button)); pump();
}
static JsonNode *response(Fixture *fixture)
{
    JsonNode *known = g_object_get_data(G_OBJECT(fixture->card), "radios-deck-media-detail");
    JsonObject *detail = radios_programme_clone_object(json_node_get_object(known)), *body = json_node_get_object(fixture->body);
    JsonObject *media = json_object_get_object_member(detail, "media"), *markers = radios_programme_clone_object(json_object_get_object_member(body, "markers"));
    json_object_set_int_member(media, "_rev", json_object_get_int_member(body, "expected_revision") + 1);
    json_object_set_int_member(markers, "_rev", radios_json_int(body, "expected_marker_revision", 0) + 1);
    json_object_set_string_member(markers, "station_id", SID); json_object_set_string_member(markers, "item_id", ITEM);
    json_object_set_string_member(markers, "version_id", VID); json_object_set_string_member(markers, "media_file_id", FILE_ID);
    json_object_set_string_member(markers, "media_sha256", SHA); json_object_set_int_member(markers, "media_duration_ms", 12000);
    json_object_set_object_member(detail, "markers", markers); JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, detail); return node;
}
static void complete(Fixture *fixture, JsonNode *node)
{
    GtkWidget *owner = g_weak_ref_get(&fixture->owner); g_assert_nonnull(owner);
    RadioSDataCallback callback = fixture->callback; gpointer user = fixture->user; GDestroyNotify destroy = fixture->destroy;
    fixture->callback = NULL; fixture->user = NULL; fixture->destroy = NULL;
    callback(&fixture->ctx, owner, node, NULL, user); g_object_unref(owner);
    if (destroy) destroy(user);
    g_clear_pointer(&fixture->body, json_node_free); json_node_free(node); pump();
}
static JsonObject *card_markers(Fixture *fixture)
{
    JsonNode *cache = g_object_get_data(G_OBJECT(fixture->card), "radios-deck-media-detail");
    return json_object_get_object_member(json_node_get_object(cache), "markers");
}
static void capture(Fixture *fixture)
{
    const gchar *directory = g_getenv("RADIOS_TEST_WAVEFORM_CAPTURE_DIR"); if (!directory) return;
    g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
    gint64 deadline = g_get_monotonic_time() + 100000;
    while (g_get_monotonic_time() < deadline) { pump(); g_usleep(1000); }
    GtkWidget *widgets[] = {fixture->wave, dialog_find()}; const gchar *names[] = {"compact.png", "inspection.png"};
    for (guint i = 0; i < G_N_ELEMENTS(widgets); ++i) {
        gint width = gtk_widget_get_allocated_width(widgets[i]), height = gtk_widget_get_allocated_height(widgets[i]);
        cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height); cairo_t *cr = cairo_create(surface);
        gtk_widget_draw(widgets[i], cr); cairo_destroy(cr);
        gchar *path = g_build_filename(directory, names[i], NULL);
        g_assert_cmpint(cairo_surface_write_to_png(surface, path), ==, CAIRO_STATUS_SUCCESS); g_free(path); cairo_surface_destroy(surface);
    }
}
static void inspector_embeds_shared_editor_and_saves_bar_points(void)
{
    Fixture fixture; fixture_start(&fixture); GtkWidget *dialog = dialog_find();
    /* The media library's marker editor, not a private canvas, beside the beat grid. */
    g_assert_nonnull(find(dialog, "media-marker-panel")); g_assert_nonnull(find(dialog, "qwf-preview-play"));
    g_assert_nonnull(find(dialog, "qwf-selection-in")); g_assert_nonnull(find(dialog, "deck-0-grid-bpm"));
    g_assert_null(find(dialog, "deck-0-waveform-inspection")); g_assert_null(find(dialog, "deck-0-waveform-autodj-in"));
    select_point(2500); set_marker(8); g_assert_null(fixture.body); save();
    JsonObject *body = json_node_get_object(fixture.body), *markers = json_object_get_object_member(body, "markers");
    g_assert_cmpint(json_object_get_int_member(markers, "autodj_in_ms"), ==, 2000);
    g_assert_cmpint(json_object_get_int_member(markers, "out_ms"), ==, 12000);
    g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(body, "expected_marker_revision")));
    complete(&fixture, response(&fixture));
    g_assert_cmpint(radios_json_int(card_markers(&fixture), "autodj_in_ms", -1), ==, 2000);
    select_point(9500); set_marker(9); save();
    body = json_node_get_object(fixture.body); markers = json_object_get_object_member(body, "markers");
    g_assert_cmpint(json_object_get_int_member(body, "expected_revision"), ==, 2);
    g_assert_cmpint(json_object_get_int_member(body, "expected_marker_revision"), ==, 1);
    g_assert_cmpint(json_object_get_int_member(markers, "autodj_out_ms"), ==, 10000);
    g_assert_cmpint(json_object_get_int_member(markers, "out_ms"), ==, 12000);
    complete(&fixture, response(&fixture)); capture(&fixture);
    g_assert_cmpint(radios_json_int(card_markers(&fixture), "autodj_out_ms", -1), ==, 10000);
    clear_points(); save();
    markers = json_object_get_object_member(json_node_get_object(fixture.body), "markers");
    g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(markers, "autodj_in_ms")));
    g_assert_true(JSON_NODE_HOLDS_NULL(json_object_get_member(markers, "autodj_out_ms")));
    complete(&fixture, response(&fixture)); fixture_clear(&fixture);
}
static void snapped_points_stay_inside_the_valid_transition_interval(void)
{
    Fixture fixture; fixture_start(&fixture); select_point(11500); set_marker(8); save();
    g_assert_nonnull(fixture.body);
    JsonObject *markers = json_object_get_object_member(json_node_get_object(fixture.body), "markers");
    g_assert_cmpint(json_object_get_int_member(markers, "autodj_in_ms"), ==, 10000);
    complete(&fixture, response(&fixture)); fixture_clear(&fixture);

    fixture_start(&fixture); select_point(2500); set_marker(8); save(); complete(&fixture, response(&fixture));
    select_point(2500); set_marker(9); save(); g_assert_nonnull(fixture.body);
    markers = json_object_get_object_member(json_node_get_object(fixture.body), "markers");
    g_assert_cmpint(json_object_get_int_member(markers, "autodj_out_ms"), ==, 4000);
    complete(&fixture, response(&fixture)); fixture_clear(&fixture);
}
static void exact_points_and_snap_preference_survive_inspector_reopen(void)
{
    Fixture fixture; fixture_start(&fixture);
    GtkWidget *snap = control("deck-0-waveform-autodj-snap");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(snap), FALSE);
    select_point(3456); set_marker(8); save();
    JsonObject *markers = json_object_get_object_member(json_node_get_object(fixture.body), "markers");
    g_assert_cmpint(json_object_get_int_member(markers, "autodj_in_ms"), ==, 3456);
    complete(&fixture, response(&fixture));
    gtk_widget_destroy(dialog_find()); pump();
    g_assert_true(radios_player_waveform_edit_grid(fixture.wave)); pump();
    snap = control("deck-0-waveform-autodj-snap");
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(snap)));
    select_point(6789); set_marker(9); save();
    markers = json_object_get_object_member(json_node_get_object(fixture.body), "markers");
    g_assert_cmpint(json_object_get_int_member(markers, "autodj_in_ms"), ==, 3456);
    g_assert_cmpint(json_object_get_int_member(markers, "autodj_out_ms"), ==, 6789);
    complete(&fixture, response(&fixture)); fixture_clear(&fixture);
}
static void context_menu_and_inspector_share_snap_preference(void)
{
    Fixture fixture; fixture_start(&fixture);
    GtkWidget *area = find(fixture.card, "deck-0-waveform"); g_assert_nonnull(area);
    GdkEventButton event = {0}; event.type = GDK_BUTTON_PRESS; event.button = 3;
    event.window = gtk_widget_get_window(area);
    event.device = gdk_seat_get_pointer(gdk_display_get_default_seat(gtk_widget_get_display(area)));
    event.x = 8 + (gtk_widget_get_allocated_width(area) - 16) * 3456.0 / 12000;
    gboolean handled = FALSE; g_signal_emit_by_name(area, "button-press-event", &event, &handled); g_assert_true(handled);
    GList *menus = gtk_menu_get_for_attach_widget(area); g_assert_nonnull(menus);
    GtkWidget *menu = menus->data, *snap = NULL, *set_in = NULL;
    GList *items = gtk_container_get_children(GTK_CONTAINER(menu));
    for (GList *item = items; item; item = item->next) {
        if (GTK_IS_CHECK_MENU_ITEM(item->data)) snap = item->data;
        if (!g_strcmp0(g_object_get_data(G_OBJECT(item->data), "autodj-marker-action"), "in")) set_in = item->data;
    }
    g_list_free(items); g_assert_nonnull(snap); g_assert_nonnull(set_in);
    gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(snap), FALSE);
    GtkWidget *inspector_snap = control("deck-0-waveform-autodj-snap");
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(inspector_snap)));
    gtk_menu_popdown(GTK_MENU(menu)); gtk_menu_item_activate(GTK_MENU_ITEM(set_in));
    g_assert_nonnull(fixture.body);
    JsonObject *markers = json_object_get_object_member(json_node_get_object(fixture.body), "markers");
    g_assert_cmpint(json_object_get_int_member(markers, "autodj_in_ms"), ==, 3456);
    complete(&fixture, response(&fixture));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(inspector_snap), TRUE);
    g_assert_true(gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(snap)));
    fixture_clear(&fixture);
}
static void invalid_point_message_survives_player_updates(void)
{
    Fixture fixture; fixture_start(&fixture); select_point(2500); set_marker(8); save(); complete(&fixture, response(&fixture));
    select_point(1500); set_marker(9); g_assert_null(fixture.body);
    GtkWidget *status = control("editor-status");
    gchar *message = g_strdup(gtk_label_get_text(GTK_LABEL(status)));
    g_assert_nonnull(strstr(message, "önce"));
    fixture.snapshot.position_ms = 6000; radios_player_waveform_update(fixture.wave, &fixture.snapshot, FALSE);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(status)), ==, message);
    /* An identical refresh keeps the same editor; only changed revisions replace it. */
    JsonNode *known = g_object_get_data(G_OBJECT(fixture.card), "radios-deck-media-detail");
    JsonNode *refresh = json_node_new(JSON_NODE_OBJECT); json_node_take_object(refresh, radios_programme_clone_object(json_node_get_object(known)));
    g_object_set_data_full(G_OBJECT(fixture.card), "radios-deck-media-detail", refresh, (GDestroyNotify)json_node_free);
    radios_player_waveform_update(fixture.wave, &fixture.snapshot, FALSE);
    g_assert_true(control("editor-status") == status);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(status)), ==, message);
    select_point(4500); set_marker(9);
    g_assert_null(strstr(gtk_label_get_text(GTK_LABEL(status)), "önce")); g_assert_null(fixture.body);
    g_free(message); fixture_clear(&fixture);
}
static void changed_generation_still_adopts_the_acknowledged_markers(void)
{
    Fixture fixture; fixture_start(&fixture); select_point(2500); set_marker(8); save(); JsonNode *reply = response(&fixture);
    /* Markers belong to the media record, not to one deck load. */
    ++fixture.snapshot.generation; radios_player_waveform_update(fixture.wave, &fixture.snapshot, FALSE);
    complete(&fixture, reply);
    JsonNode *cache = g_object_get_data(G_OBJECT(fixture.card), "radios-deck-media-detail");
    g_assert_cmpint(json_object_get_int_member(json_object_get_object_member(json_node_get_object(cache), "media"), "_rev"), ==, 2);
    g_assert_cmpint(radios_json_int(card_markers(&fixture), "autodj_in_ms", -1), ==, 2000);
    fixture_clear(&fixture);
}
static void changed_generation_before_save_still_writes_the_media(void)
{
    Fixture fixture; fixture_start(&fixture); select_point(2500); set_marker(8);
    ++fixture.snapshot.generation; radios_player_waveform_update(fixture.wave, &fixture.snapshot, FALSE);
    save(); g_assert_cmpuint(fixture.requests, ==, 1); complete(&fixture, response(&fixture));
    g_assert_cmpint(radios_json_int(card_markers(&fixture), "autodj_in_ms", -1), ==, 2000);
    fixture_clear(&fixture);
}
static void invalid_point_order_places_nothing(void)
{
    Fixture fixture; fixture_start(&fixture); select_point(2500); set_marker(8); save(); complete(&fixture, response(&fixture));
    select_point(1500); set_marker(9); g_assert_cmpuint(fixture.requests, ==, 1); g_assert_null(fixture.body);
    fixture_clear(&fixture);
}
static void changed_version_closes_the_inspector_before_a_save(void)
{
    Fixture fixture; fixture_start(&fixture); select_point(2500); set_marker(8);
    g_strlcpy(fixture.snapshot.version_id, "99999999-9999-4999-8999-999999999999", sizeof fixture.snapshot.version_id);
    radios_player_waveform_update(fixture.wave, &fixture.snapshot, FALSE); pump();
    g_assert_null(dialog_find()); g_assert_cmpuint(fixture.requests, ==, 0); fixture_clear(&fixture);
}
static void owner_destroy_during_save_is_safe(void)
{
    Fixture fixture; fixture_start(&fixture); select_point(2500); set_marker(8); save(); JsonNode *reply = response(&fixture);
    GtkWidget *owner = g_weak_ref_get(&fixture.owner); g_assert_nonnull(owner); g_object_unref(owner);
    /* The data service delivers no reply to a destroyed owner; the inspector
     * and its editor go away with the deck window without a pending write. */
    gtk_widget_destroy(fixture.window); pump();
    g_assert_null(g_weak_ref_get(&fixture.owner)); g_assert_null(dialog_find());
    fixture.callback = NULL; if (fixture.destroy) fixture.destroy(fixture.user); fixture.destroy = NULL; fixture.user = NULL;
    json_node_free(reply); fixture_clear(&fixture);
}
static JsonNode *grid_revision_detail(JsonNode *base, gint64 media_revision, gint64 marker_revision,
    gint64 analysis_revision, gint64 in_ms, gint64 out_ms, gdouble bpm)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *detail = radios_programme_clone_object(json_node_get_object(base));
    json_node_take_object(node, detail);
    json_object_set_int_member(json_object_get_object_member(detail, "media"), "_rev", media_revision);
    JsonObject *markers = json_object_get_object_member(detail, "markers");
    json_object_set_int_member(markers, "_rev", marker_revision);
    json_object_set_int_member(markers, "autodj_in_ms", in_ms); json_object_set_int_member(markers, "autodj_out_ms", out_ms);
    json_object_set_int_member(detail, "analysis_revision", analysis_revision);
    JsonObject *analysis = json_object_get_object_member(detail, "analysis");
    json_object_set_double_member(json_object_get_object_member(analysis, "rhythm"), "bpm", bpm);
    return node;
}
static void grid_ack_revision_merge(gboolean stale_grid)
{
    Fixture fixture; fixture_start(&fixture);
    select_point(2500); set_marker(8); save(); complete(&fixture, response(&fixture));
    select_point(9500); set_marker(9); save(); complete(&fixture, response(&fixture));
    JsonNode *base = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(base, radios_programme_clone_object(json_node_get_object(
        g_object_get_data(G_OBJECT(fixture.card), "radios-deck-media-detail"))));
    GtkWidget *bpm = control("deck-0-grid-bpm"), *grid_save = control("deck-0-grid-save");
    g_assert_true(GTK_IS_SPIN_BUTTON(bpm)); g_assert_true(GTK_IS_BUTTON(grid_save));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(bpm), 140); g_assert_true(gtk_widget_is_sensitive(grid_save));
    gtk_button_clicked(GTK_BUTTON(grid_save)); g_assert_nonnull(fixture.callback);
    /* A displayed GET and a subsequent card-cache GET race the grid ACK.
     * The displayed detail has the latest known analysis; the card cache has
     * newer media/markers but an older analysis snapshot. */
    JsonNode *shown = grid_revision_detail(base, 5, 4, 2, 4000, 10000, 130);
    g_object_set_data_full(G_OBJECT(fixture.card), "radios-deck-media-detail", shown, (GDestroyNotify)json_node_free);
    radios_player_waveform_update(fixture.wave, &fixture.snapshot, FALSE);
    JsonNode *cache = grid_revision_detail(base, 6, 5, 1, 6000, 11000, 120);
    g_object_set_data_full(G_OBJECT(fixture.card), "radios-deck-media-detail", cache, (GDestroyNotify)json_node_free);
    JsonNode *ack = grid_revision_detail(base, 4, 3, stale_grid ? 1 : 3, 2000, 9000, 140);
    GtkWidget *owner = g_weak_ref_get(&fixture.owner); g_assert_nonnull(owner);
    fixture.callback(&fixture.ctx, owner, ack, NULL, fixture.user); g_object_unref(owner);
    if (fixture.destroy) fixture.destroy(fixture.user);
    fixture.callback = NULL; fixture.user = NULL; fixture.destroy = NULL; json_node_free(ack); json_node_free(base); pump();
    JsonObject *merged = json_node_get_object(g_object_get_data(G_OBJECT(fixture.card), "radios-deck-media-detail"));
    JsonObject *marks = json_object_get_object_member(merged, "markers"), *analysis = json_object_get_object_member(merged, "analysis");
    g_assert_cmpint(radios_json_int(json_object_get_object_member(merged, "media"), "_rev", 0), ==, 6);
    g_assert_cmpint(radios_json_int(marks, "_rev", 0), ==, 5);
    g_assert_cmpint(radios_json_int(marks, "autodj_in_ms", 0), ==, 6000);
    g_assert_cmpint(radios_json_int(marks, "autodj_out_ms", 0), ==, 11000);
    g_assert_cmpint(radios_json_int(merged, "analysis_revision", 0), ==, stale_grid ? 2 : 3);
    g_assert_cmpfloat(json_object_get_double_member(json_object_get_object_member(analysis, "rhythm"), "bpm"), ==, stale_grid ? 130 : 140);
    g_assert_cmpfloat(gtk_spin_button_get_value(GTK_SPIN_BUTTON(bpm)), ==, stale_grid ? 130 : 140);
    /* The embedded editor follows the merged card cache, so a later AutoDJ
     * edit keeps the newer IN and sends the correct CAS tokens. */
    select_point(9500); set_marker(9); save(); g_assert_nonnull(fixture.body);
    JsonObject *body = json_node_get_object(fixture.body);
    g_assert_cmpint(radios_json_int(body, "expected_revision", 0), ==, 6);
    g_assert_cmpint(radios_json_int(body, "expected_marker_revision", 0), ==, 5);
    g_assert_cmpint(radios_json_int(json_object_get_object_member(body, "markers"), "autodj_in_ms", 0), ==, 6000);
    complete(&fixture, response(&fixture)); fixture_clear(&fixture);
}
static void new_grid_ack_preserves_newer_autodj_markers(void)
{ grid_ack_revision_merge(FALSE); }
static void stale_grid_ack_preserves_newer_displayed_analysis(void)
{ grid_ack_revision_merge(TRUE); }

static void marker_ack_analysis_race(gboolean update_wave)
{
    Fixture fixture; fixture_start(&fixture); select_point(2500); set_marker(8); save();
    g_assert_nonnull(fixture.body);
    /* The marker transaction succeeds first, but its callback arrives after
     * an independent player analysis refresh updated the displayed detail. */
    JsonNode *ack = response(&fixture);
    JsonNode *known = g_object_get_data(G_OBJECT(fixture.card), "radios-deck-media-detail");
    JsonNode *newer = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(newer, radios_programme_clone_object(json_node_get_object(known)));
    json_object_set_int_member(json_node_get_object(newer), "analysis_revision", 2);
    g_object_set_data_full(G_OBJECT(fixture.card), "radios-deck-media-detail", newer, (GDestroyNotify)json_node_free);
    if (update_wave) radios_player_waveform_update(fixture.wave, &fixture.snapshot, FALSE);
    complete(&fixture, ack);
    JsonObject *merged = json_node_get_object(g_object_get_data(G_OBJECT(fixture.card), "radios-deck-media-detail"));
    g_assert_cmpint(radios_json_int(json_object_get_object_member(merged, "media"), "_rev", 0), ==, 2);
    g_assert_cmpint(radios_json_int(merged, "analysis_revision", 0), ==, 2);
    JsonObject *markers = json_object_get_object_member(merged, "markers"); g_assert_nonnull(markers);
    g_assert_cmpint(radios_json_int(markers, "_rev", 0), ==, 1);
    g_assert_cmpint(radios_json_int(markers, "autodj_in_ms", -1), ==, 2000);
    /* Another point must CAS the acknowledged marker rather than retry the
     * already committed request against stale media/marker revisions. */
    select_point(9500); set_marker(9); save(); g_assert_nonnull(fixture.body);
    JsonObject *body = json_node_get_object(fixture.body);
    g_assert_cmpint(radios_json_int(body, "expected_revision", 0), ==, 2);
    g_assert_cmpint(radios_json_int(body, "expected_marker_revision", 0), ==, 1);
    g_assert_cmpint(radios_json_int(json_object_get_object_member(body, "markers"), "autodj_in_ms", -1), ==, 2000);
    complete(&fixture, response(&fixture)); fixture_clear(&fixture);
}
static void marker_ack_preserves_newer_analysis(void)
{ marker_ack_analysis_race(TRUE); }
static void marker_ack_preserves_newer_card_cache_analysis(void)
{ marker_ack_analysis_race(FALSE); }

int main(int argc, char **argv)
{
    gtk_test_init(&argc, &argv, NULL);
    g_test_add_func("/player-waveform/grid-ACK-preserves-newer-AutoDJ-markers", new_grid_ack_preserves_newer_autodj_markers);
    g_test_add_func("/player-waveform/marker-ACK-preserves-newer-analysis", marker_ack_preserves_newer_analysis);
    g_test_add_func("/player-waveform/marker-ACK-preserves-newer-card-cache-analysis", marker_ack_preserves_newer_card_cache_analysis);
    g_test_add_func("/player-waveform/stale-grid-ACK-preserves-displayed-analysis", stale_grid_ack_preserves_newer_displayed_analysis);
    g_test_add_func("/player-waveform/inspector-shared-editor-bar-points-CAS-clear", inspector_embeds_shared_editor_and_saves_bar_points);
    g_test_add_func("/player-waveform/autodj-snap-valid-boundaries", snapped_points_stay_inside_the_valid_transition_interval);
    g_test_add_func("/player-waveform/autodj-exact-points-snap-preference", exact_points_and_snap_preference_survive_inspector_reopen);
    g_test_add_func("/player-waveform/autodj-context-menu-shared-snap", context_menu_and_inspector_share_snap_preference);
    g_test_add_func("/player-waveform/autodj-validation-message-persists", invalid_point_message_survives_player_updates);
    g_test_add_func("/player-waveform/autodj-changed-generation-ack-adopted", changed_generation_still_adopts_the_acknowledged_markers);
    g_test_add_func("/player-waveform/autodj-changed-generation-save", changed_generation_before_save_still_writes_the_media);
    g_test_add_func("/player-waveform/autodj-invalid-point-order", invalid_point_order_places_nothing);
    g_test_add_func("/player-waveform/autodj-changed-version-closes-inspector", changed_version_closes_the_inspector_before_a_save);
    g_test_add_func("/player-waveform/autodj-owner-destroy", owner_destroy_during_save_is_safe);
    return g_test_run();
}
