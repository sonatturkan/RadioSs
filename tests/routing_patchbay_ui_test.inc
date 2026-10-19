/* Shared acceptance of the dynamic patchbay, with a private HTTP store and
 * synthetic Pulse sources supplied by the runners. No desktop audio is used. */
#include "radios/audio_patchbay.h"
#include "radios/audio_ui.h"
#include "radios/data.h"
#include "radios/routing_graph_ui.h"
#include "radios/views.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#if RADIOS_ROUTING_NATIVE_INPUT
#include <gdk/gdkx.h>
#include <X11/extensions/XTest.h>
#endif
#define TEST_LOCAL static G_GNUC_UNUSED
static RadioSConfig config;
static RadioSDataClient *reader;
static gchar *last_notice;
typedef struct { RadioSViewContext ctx; GtkWidget *window, *page, *canvas; RadioSAudioPatchbay *patchbay; RadioSRoutingGraphUI *editor; } Harness;
typedef gboolean (*Condition)(gpointer);
TEST_LOCAL GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (!g_strcmp0(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
TEST_LOCAL void pump(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
TEST_LOCAL void run_for(guint ms)
{ gint64 until = g_get_monotonic_time() + ms * 1000; while (g_get_monotonic_time() < until) pump(); }
TEST_LOCAL void wait_for(Condition condition, gpointer value, const gchar *description)
{
    gint64 deadline = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!condition(value) && g_get_monotonic_time() < deadline) pump();
    if (!condition(value)) g_error("Patchbay acceptance timed out: %s (%s)", description, last_notice ? last_notice : "no notice");
}
TEST_LOCAL void notice(gpointer unused, const gchar *message)
{ (void)unused; g_free(last_notice); last_notice = g_strdup(message); }
TEST_LOCAL RadioSDocument *document(Harness *h)
{
    GPtrArray *docs = g_object_get_data(G_OBJECT(h->page), "radios-data-documents");
    return docs && docs->len ? g_ptr_array_index(docs, 0) : NULL;
}
TEST_LOCAL gboolean ready(gpointer value)
{
    Harness *h = value; RadioSAudioPatchbaySnapshot *snapshot = g_new0(RadioSAudioPatchbaySnapshot, 1);
    radios_audio_patchbay_snapshot(h->patchbay, snapshot);
    gboolean result = radios_data_connected(&h->ctx) && snapshot->connected && snapshot->input_count >= 3 &&
        document(h) && radios_data_document_ready(document(h)); g_free(snapshot); return result;
}
TEST_LOCAL Harness *open_with_config(RadioSConfig *session)
{
    Harness *h = g_new0(Harness, 1); h->ctx.config = session; h->ctx.notify = notice;
    h->ctx.runtime = radios_runtime_new(64, 512); h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    g_object_ref_sink(h->window); h->ctx.parent = GTK_WINDOW(h->window);
    gtk_window_set_default_size(GTK_WINDOW(h->window), 1800, 1000);
    gtk_window_set_title(GTK_WINDOW(h->window), "RadioS private dynamic patchbay acceptance");
    h->ctx.data = radios_data_new(&h->ctx); h->page = radios_routing_new(&h->ctx);
    h->patchbay = g_object_get_data(G_OBJECT(h->page), "routing-patchbay"); g_assert_nonnull(h->patchbay);
    h->editor = radios_routing_graph_ui_from_widget(find(h->page, "routing-graph-editor")); g_assert_nonnull(h->editor);
    h->canvas = find(h->page, "routing-graph-canvas");
    gtk_container_add(GTK_CONTAINER(h->window), h->page); gtk_widget_show_all(h->window);
    gtk_window_present(GTK_WINDOW(h->window)); radios_data_connect(&h->ctx); wait_for(ready, h, "private HTTP/Pulse and document");
    run_for(180); return h;
}
TEST_LOCAL Harness *open_harness(void) { return open_with_config(&config); }
TEST_LOCAL void close_harness(Harness *h)
{
    gtk_widget_destroy(h->window); radios_data_close(h->ctx.data);
    if (h->ctx.audio) { radios_audio_ui_close(h->ctx.audio); radios_audio_ui_unref(h->ctx.audio); }
    radios_runtime_free(h->ctx.runtime); while (g_main_context_iteration(NULL, FALSE)) {}
    radios_data_unref(h->ctx.data); g_object_unref(h->window); g_free(h); run_for(50);
}
TEST_LOCAL guint kind_id(Harness *h, RadioSRoutingGraphKind kind)
{
    const RadioSRoutingGraph *graph = radios_routing_graph_ui_graph(h->editor);
    for (guint i = 0; i < graph->node_count; ++i) if (graph->nodes[i].kind == kind) return graph->nodes[i].id;
    return 0;
}
TEST_LOCAL void select_node(Harness *h, guint id)
{ radios_routing_graph_ui_select(h->editor, id); run_for(120); }
TEST_LOCAL gboolean sensitive(gpointer value) { return gtk_widget_is_sensitive(value); }
TEST_LOCAL void click(Harness *h, const gchar *name)
{
    GtkWidget *button = find(h->page, name); g_assert_true(GTK_IS_BUTTON(button));
    wait_for(sensitive, button, name); gtk_button_clicked(GTK_BUTTON(button)); run_for(40);
}
TEST_LOCAL guint palette_add(Harness *h, RadioSRoutingGraphKind kind)
{
    gchar *name = g_strdup_printf("routing-graph-palette-%s", radios_routing_graph_kind_name(kind));
    click(h, name); g_free(name); guint id = radios_routing_graph_ui_selected(h->editor); g_assert_cmpuint(id, >, 0); return id;
}
TEST_LOCAL void connect_nodes(Harness *h, guint from, guint to)
{
#if RADIOS_ROUTING_NATIVE_INPUT
    /* Native XTest events exercise the displayed ports; confine input to our
     * window and restore the original pointer in main. */
    gdouble fx, fy, tx, ty; gint ox, oy, wx, wy;
    g_assert_true(radios_routing_graph_ui_node_geometry(h->editor, from, NULL, NULL, NULL, &fx, &fy));
    g_assert_true(radios_routing_graph_ui_node_geometry(h->editor, to, NULL, &tx, &ty, NULL, NULL));
    GdkWindow *window = gtk_widget_get_window(h->window); gtk_window_present(GTK_WINDOW(h->window)); gdk_window_raise(window); run_for(180);
    gdk_window_get_origin(window, &ox, &oy);
    g_assert_true(gtk_widget_translate_coordinates(h->canvas, h->window, 0, 0, &wx, &wy));
    Display *display = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    gint x1 = ox + wx + (gint)llround(fx), y1 = oy + wy + (gint)llround(fy);
    gint x2 = ox + wx + (gint)llround(tx), y2 = oy + wy + (gint)llround(ty);
    XTestFakeMotionEvent(display, -1, x1, y1, CurrentTime); XFlush(display); run_for(70);
    GdkDevice *pointer = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
    GdkWindow *under = gdk_device_get_window_at_position(pointer, NULL, NULL);
    g_assert_nonnull(under); g_assert_true(gdk_window_get_toplevel(under) == gdk_window_get_toplevel(window));
    XTestFakeButtonEvent(display, 1, TRUE, CurrentTime); XFlush(display); run_for(50);
    for (guint step = 1; step <= 20; ++step) {
        XTestFakeMotionEvent(display, -1, x1 + (x2 - x1) * (gint)step / 20, y1 + (y2 - y1) * (gint)step / 20, CurrentTime);
        XFlush(display); run_for(15);
    }
    XTestFakeButtonEvent(display, 1, FALSE, CurrentTime); XFlush(display); run_for(160);
#else
    RadioSRoutingGraph *graph = (RadioSRoutingGraph *)radios_routing_graph_ui_graph(h->editor);
    g_assert_true(radios_routing_graph_connect(graph, from, to, NULL));
    /* Settings commit applies the resulting graph using production callbacks. */
    select_node(h, from); GtkWidget *gain = find(h->page, "routing-block-gain");
    g_assert_true(GTK_IS_RANGE(gain)); gtk_range_set_value(GTK_RANGE(gain), -.5); gtk_range_set_value(GTK_RANGE(gain), 0);
#endif
    g_assert_true(radios_routing_graph_has_edge(radios_routing_graph_ui_graph(h->editor), from, to));
}
TEST_LOCAL void entry(Harness *h, const gchar *name, const gchar *text)
{ GtkWidget *widget = find(h->page, name); g_assert_true(GTK_IS_ENTRY(widget)); gtk_entry_set_text(GTK_ENTRY(widget), text); run_for(30); }
TEST_LOCAL guint build_test_graph(Harness *h, gboolean microphone)
{
    /* Selected application's dedicated synthetic sink and virtual microphone
     * are captured explicitly as inputs; the unrelated sink never joins. */
    guint input = kind_id(h, RG_INPUT); if (!input) input = palette_add(h, RG_INPUT);
    select_node(h, input); g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(h->page, "routing-block-device")), "routing_original.monitor"));
    guint recorder = kind_id(h, RG_RECORDER);
    /* Default MAIN->meter->recorder is removed so the test isolates its inputs. */
    RadioSRoutingGraph *graph = (RadioSRoutingGraph *)radios_routing_graph_ui_graph(h->editor);
    guint meter = kind_id(h, RG_METER); radios_routing_graph_disconnect(graph, meter, recorder, NULL);
    if (!radios_routing_graph_has_edge(graph, input, recorder)) connect_nodes(h, input, recorder);
    if (microphone) {
        guint mic = kind_id(h, RG_MICROPHONE); if (!mic) mic = palette_add(h, RG_MICROPHONE);
        select_node(h, mic); GtkWidget *ptt = find(h->page, "routing-microphone-ptt");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ptt), FALSE);
        g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(find(h->page, "routing-block-device")), "routing_microphone.monitor"));
        if (!radios_routing_graph_has_edge(graph, mic, recorder)) connect_nodes(h, mic, recorder);
    }
    select_node(h, recorder); return recorder;
}
TEST_LOCAL RadioSPatchNodeSnapshot recorder_snapshot(Harness *h, guint id)
{
    RadioSAudioPatchbaySnapshot *snapshot = g_new0(RadioSAudioPatchbaySnapshot, 1); radios_audio_patchbay_snapshot(h->patchbay, snapshot);
    RadioSPatchNodeSnapshot node = {0};
    for (guint i = 0; i < snapshot->node_count; ++i) if (snapshot->nodes[i].id == id) node = snapshot->nodes[i];
    g_free(snapshot); return node;
}
typedef struct { Harness *h; guint id; guint64 frames; gboolean recording, paused; guint segment; } Expected;
TEST_LOCAL gboolean matches(gpointer value)
{
    Expected *expected = value; RadioSPatchNodeSnapshot node = recorder_snapshot(expected->h, expected->id);
    if (node.state == RADIOS_PATCH_FAILED) g_error("Patchbay recorder failed: %s", node.error);
    return node.recording == expected->recording && node.paused == expected->paused && node.frames >= expected->frames && node.segment >= expected->segment;
}
TEST_LOCAL RadioSPatchNodeSnapshot wait_recorder(Harness *h, guint id, guint64 frames, gboolean recording, gboolean paused, guint segment)
{
    Expected expected = {h, id, frames, recording, paused, segment}; wait_for(matches, &expected, "recorder state/frames"); return recorder_snapshot(h, id);
}
TEST_LOCAL guint64 finalized_frames(const gchar *path)
{
    gchar *bytes = NULL; gsize size = 0; g_assert_true(g_file_get_contents(path, &bytes, &size, NULL));
    g_assert_cmpuint(size, >=, 44); g_assert_cmpmem(bytes, 4, "RIFF", 4); g_assert_cmpmem(bytes + 8, 4, "WAVE", 4);
    guint32 riff, data; memcpy(&riff, bytes + 4, 4); memcpy(&data, bytes + 40, 4);
    g_assert_cmpuint(GUINT32_FROM_LE(riff), ==, size - 8); g_assert_cmpuint(GUINT32_FROM_LE(data), ==, size - 44);
    struct stat info; g_assert_cmpint(g_stat(path, &info), ==, 0); g_assert_cmpuint(info.st_mode & 0777, ==, 0600);
    g_free(bytes); return (size - 44) / 4;
}
TEST_LOCAL gboolean stopped(gpointer value)
{
    Harness *h = value; RadioSAudioPatchbaySnapshot *snapshot = g_new0(RadioSAudioPatchbaySnapshot, 1); radios_audio_patchbay_snapshot(h->patchbay, snapshot);
    gboolean result = !snapshot->running && !snapshot->busy;
    for (guint i = 0; i < snapshot->node_count; ++i) result &= !snapshot->nodes[i].recording && !snapshot->nodes[i].streaming;
    g_free(snapshot); return result;
}
TEST_LOCAL gboolean saved(gpointer value)
{ return document(value) && radios_data_document_revision(document(value)) > 0; }
TEST_LOCAL void test_save_reopen_pcm(void)
{
    Harness *h = open_harness(); wait_for(stopped, h, "saved graph never starts capture");
    guint recorder = build_test_graph(h, !RADIOS_ROUTING_NATIVE_INPUT);
    entry(h, "routing-record-prefix", RADIOS_ROUTING_NATIVE_INPUT ? "graph-ui-app" : "routing-ui");
    click(h, "routing-settings-save"); wait_for(saved, h, "schema2 save acknowledged");
    JsonObject *object = radios_routing_graph_to_json(radios_routing_graph_ui_graph(h->editor));
    JsonNode *expected = json_node_new(JSON_NODE_OBJECT); json_node_take_object(expected, object);
    gchar *serialized = json_to_string(expected, FALSE); json_node_free(expected); close_harness(h);
    h = open_harness(); wait_for(stopped, h, "reopen has no capture or recording");
    expected = json_node_new(JSON_NODE_OBJECT); json_node_take_object(expected, radios_routing_graph_to_json(radios_routing_graph_ui_graph(h->editor)));
    gchar *actual = json_to_string(expected, FALSE); json_node_free(expected); g_assert_cmpstr(actual, ==, serialized); g_free(actual); g_free(serialized);
    select_node(h, recorder); click(h, "routing-record-start");
    RadioSPatchNodeSnapshot first = wait_recorder(h, recorder, 96000, TRUE, FALSE, 1); gchar *first_path = g_strdup(first.path);
#if RADIOS_ROUTING_NATIVE_INPUT
    click(h, "routing-record-stop"); wait_recorder(h, recorder, 0, FALSE, FALSE, 1);
    click(h, "routing-run"); wait_for(stopped, h, "explicit graph stop closes capture");
    recorder = build_test_graph(h, TRUE); entry(h, "routing-record-prefix", "graph-ui-mix");
    click(h, "routing-record-start"); RadioSPatchNodeSnapshot second = wait_recorder(h, recorder, 96000, TRUE, FALSE, 1);
    gchar *second_path = g_strdup(second.path);
#else
    click(h, "routing-record-pause"); first = wait_recorder(h, recorder, 96000, TRUE, TRUE, 1);
    run_for(180); g_assert_cmpuint(recorder_snapshot(h, recorder).frames, ==, first.frames);
    click(h, "routing-record-split"); RadioSPatchNodeSnapshot second = wait_recorder(h, recorder, 0, TRUE, TRUE, 2);
    gchar *second_path = g_strdup(second.path); g_assert_cmpstr(first_path, !=, second_path);
    click(h, "routing-record-pause"); wait_recorder(h, recorder, 96000, TRUE, FALSE, 2);
#endif
    click(h, "routing-record-stop"); run_for(200); click(h, "routing-run"); wait_for(stopped, h, "explicit stop closes all capture");
    g_assert_cmpuint(finalized_frames(first_path), >=, 96000); g_assert_cmpuint(finalized_frames(second_path), >=, 96000);
    g_free(first_path); g_free(second_path); close_harness(h);
}
TEST_LOCAL void test_viewer_denied(void)
{
    RadioSConfig viewer = config; viewer.token_environment = "RADIOS_ROUTING_VIEWER_TOKEN";
    Harness *h = open_with_config(&viewer); guint recorder = build_test_graph(h, FALSE); select_node(h, recorder);
    GtkWidget *start = find(h->page, "routing-record-start"); g_assert_true(GTK_IS_BUTTON(start));
    gtk_button_clicked(GTK_BUTTON(start)); run_for(700); wait_for(stopped, h, "viewer authorization rejects capture");
    g_assert_nonnull(last_notice); g_assert_true(strstr(last_notice, "yetki") || strstr(last_notice, "operate")); close_harness(h);
}
TEST_LOCAL gboolean file_exists(gpointer value) { return g_file_test(value, G_FILE_TEST_IS_REGULAR); }
TEST_LOCAL void test_close_pending_start(void)
{
    Harness *h = open_harness(); guint recorder = build_test_graph(h, FALSE); select_node(h, recorder);
    gchar *root = g_path_get_dirname(config.database_file), *hold = g_build_filename(root, ".routing-context-hold", NULL);
    gchar *ack = g_build_filename(root, ".routing-context-ready", NULL); g_free(root); g_unlink(ack);
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL)); click(h, "routing-record-start");
    wait_for(file_exists, ack, "genuine authorization reply held before capture");
    gtk_widget_destroy(h->window); g_assert_cmpint(g_unlink(hold), ==, 0); close_harness(h);
    g_free(hold); g_free(ack);
}
typedef struct { Harness *h; RadioSDataClient *before; gboolean viewer; } Reconnect;
TEST_LOCAL gboolean reconnected(gpointer value)
{
    Reconnect *expected = value; RadioSDataClient *current = radios_data_client_acquire(&expected->h->ctx);
    if (!current) return FALSE;
    gboolean changed = current != expected->before &&
        (expected->viewer ? g_strcmp0(radios_data_client_user_id(current), radios_data_client_user_id(reader)) != 0 :
                           g_strcmp0(radios_data_client_user_id(current), radios_data_client_user_id(reader)) == 0);
    radios_data_client_unref(current); return changed;
}
TEST_LOCAL void reject_old_authorization(gboolean viewer)
{
    RadioSConfig session = config; Harness *h = open_with_config(&session);
    guint recorder = build_test_graph(h, FALSE); select_node(h, recorder);
    gchar *root = g_path_get_dirname(config.database_file), *hold = g_build_filename(root, ".routing-context-hold", NULL);
    gchar *ack = g_build_filename(root, ".routing-context-ready", NULL), *only = g_build_filename(root, ".routing-context-only-user", NULL);
    g_free(root); g_unlink(ack); RadioSDataClient *before = radios_data_client_acquire(&h->ctx); g_assert_nonnull(before);
    g_assert_true(g_file_set_contents(only, radios_data_client_user_id(before), -1, NULL));
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL)); click(h, "routing-record-start");
    wait_for(file_exists, ack, "old authorization held while the new authenticated client connects");
    RadioSRuntime *old_runtime = h->ctx.runtime; h->ctx.runtime = radios_runtime_new(64, 512);
    if (viewer) session.token_environment = "RADIOS_ROUTING_VIEWER_TOKEN";
    radios_data_connect(&h->ctx); Reconnect expected = {h, before, viewer};
    wait_for(reconnected, &expected, "replacement client verified before old start reply");
    g_assert_cmpint(g_unlink(hold), ==, 0); run_for(350);
    wait_for(stopped, h, "stale start authorization cannot activate the replacement session");
    RadioSPatchNodeSnapshot node = recorder_snapshot(h, recorder); g_assert_false(node.recording);
    radios_runtime_free(old_runtime); run_for(50); radios_data_client_unref(before);
    g_unlink(ack); g_unlink(only); g_free(hold); g_free(ack); g_free(only); close_harness(h);
}
TEST_LOCAL void test_reconnect_viewer(void) { reject_old_authorization(TRUE); }
TEST_LOCAL void test_reconnect_same_user(void) { reject_old_authorization(FALSE); }

typedef struct {
    GSocketListener *listener;
    GCancellable *cancel;
    GThread *thread;
    gint rejected, accepted;
    gchar accepted_login[64];
} StreamLoginServer;
TEST_LOCAL gpointer serve_stream_login(gpointer value)
{
    StreamLoginServer *server = value;
    while (!g_cancellable_is_cancelled(server->cancel)) {
        GSocket *client = g_socket_listener_accept_socket(server->listener, NULL, server->cancel, NULL);
        if (!client) break;
        gchar login[64] = "", c; gsize length = 0;
        while (g_socket_receive(client, &c, 1, server->cancel, NULL) == 1 && c != '\n')
            if (c != '\r' && length < sizeof login - 1) login[length++] = c;
        gboolean accepted = !g_strcmp0(login, "corrected:#1");
        const gchar *reply = accepted ? "OK2\r\n\r\n" : "invalid password\r\n";
        g_socket_send(client, reply, strlen(reply), server->cancel, NULL);
        if (accepted) {
            g_strlcpy(server->accepted_login, login, sizeof server->accepted_login);
            g_atomic_int_inc(&server->accepted);
            gchar buffer[4096];
            while (g_socket_receive(client, buffer, sizeof buffer, server->cancel, NULL) > 0) {}
        } else g_atomic_int_inc(&server->rejected);
        g_socket_close(client, NULL); g_object_unref(client);
    }
    return NULL;
}
TEST_LOCAL guint16 start_stream_login_server(StreamLoginServer *server)
{
    server->listener = g_socket_listener_new(); server->cancel = g_cancellable_new();
    GInetAddress *loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    GSocketAddress *address = g_inet_socket_address_new(loopback, 0), *bound = NULL;
    GError *error = NULL;
    g_assert_true(g_socket_listener_add_address(server->listener, address, G_SOCKET_TYPE_STREAM,
        G_SOCKET_PROTOCOL_TCP, NULL, &bound, &error)); g_assert_no_error(error);
    guint16 port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
    g_object_unref(bound); g_object_unref(address); g_object_unref(loopback);
    server->thread = g_thread_new("routing-local-shoutcast", serve_stream_login, server); return port;
}
typedef struct { Harness *h; guint id; gboolean streaming; RadioSAudioOutputState state; } StreamExpected;
TEST_LOCAL gboolean stream_matches(gpointer value)
{
    StreamExpected *expected = value; RadioSPatchNodeSnapshot node = recorder_snapshot(expected->h, expected->id);
    return node.streaming == expected->streaming && (!expected->streaming || node.output_state == expected->state);
}
TEST_LOCAL void wait_stream(Harness *h, guint id, gboolean streaming, RadioSAudioOutputState state)
{
    StreamExpected expected = {h, id, streaming, state}; wait_for(stream_matches, &expected, "stream intent and source connection");
}
TEST_LOCAL void assert_stream_fields(Harness *h)
{
    const gchar *fields[] = {"routing-stream-protocol", "routing-stream-host", "routing-stream-port", "routing-stream-id",
        "routing-stream-user", "routing-stream-password", "routing-stream-bitrate", "routing-stream-station", "routing-stream-genre",
        "routing-stream-description", "routing-stream-website", "routing-stream-public"};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) {
        GtkWidget *widget = find(h->page, fields[i]); g_assert_nonnull(widget); g_assert_true(gtk_widget_is_sensitive(widget));
    }
}
typedef struct { Harness *h; gint64 revision; } SavedRevision;
TEST_LOCAL gboolean newer_revision(gpointer value)
{
    SavedRevision *expected = value;
    return document(expected->h) && radios_data_document_revision(document(expected->h)) > expected->revision;
}
typedef struct { gboolean done; gchar *json; } SavedConfiguration;
TEST_LOCAL void configuration_read(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer value)
{
    (void)ctx; (void)owner; SavedConfiguration *saved = value;
    g_assert_no_error(error); g_assert_nonnull(result); saved->json = json_to_string(result, FALSE); saved->done = TRUE;
}
TEST_LOCAL gboolean configuration_read_done(gpointer value) { return ((SavedConfiguration *)value)->done; }
TEST_LOCAL void test_stream_error_edit_restart(void)
{
    StreamLoginServer server = {0}; guint16 source_port = start_stream_login_server(&server);
    Harness *h = open_harness(); guint stream = kind_id(h, RG_STREAM);
    if (!stream) stream = palette_add(h, RG_STREAM);
    select_node(h, stream);
    GtkWidget *protocol = find(h->page, "routing-stream-protocol"), *port = find(h->page, "routing-stream-port");
    gtk_combo_box_set_active(GTK_COMBO_BOX(protocol), RG_STREAM_SHOUTCAST2);
    entry(h, "routing-stream-host", "127.0.0.1");
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port), source_port - 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(find(h->page, "routing-stream-id")), 1);
    entry(h, "routing-stream-password", "pending");

    /* A credential edit also invalidates an authorization reply still in flight. */
    gchar *root = g_path_get_dirname(config.database_file), *hold = g_build_filename(root, ".routing-context-hold", NULL);
    gchar *ack = g_build_filename(root, ".routing-context-ready", NULL); g_free(root); g_unlink(ack);
    g_assert_true(g_file_set_contents(hold, "hold", -1, NULL)); click(h, "routing-stream-start");
    wait_for(file_exists, ack, "stream authorization held before its source connection");
    entry(h, "routing-stream-password", "wrong");
    g_assert_cmpint(g_unlink(hold), ==, 0); run_for(350);
    wait_for(stopped, h, "credential edit cancels pending stream authorization");
    g_assert_cmpint(g_atomic_int_get(&server.rejected), ==, 0);
    g_unlink(ack); g_free(hold); g_free(ack);

    click(h, "routing-stream-start"); wait_stream(h, stream, TRUE, RADIOS_AUDIO_OUTPUT_FAILED); run_for(120);
    RadioSPatchNodeSnapshot failed = recorder_snapshot(h, stream);
    g_assert_nonnull(strstr(failed.error, "invalid password")); assert_stream_fields(h);
    GtkWidget *gain = find(h->page, "routing-block-gain"); gtk_range_set_value(GTK_RANGE(gain), -2); run_for(100);
    g_assert_true(recorder_snapshot(h, stream).streaming);
    entry(h, "routing-stream-host", "localhost"); wait_stream(h, stream, FALSE, RADIOS_AUDIO_OUTPUT_IDLE);
    wait_for(sensitive, find(h->page, "routing-stream-start"), "edited source settings allow another start");

    entry(h, "routing-stream-host", "127.0.0.1");
    click(h, "routing-stream-start"); wait_stream(h, stream, TRUE, RADIOS_AUDIO_OUTPUT_FAILED);
    entry(h, "routing-stream-user", "edited-shoutcast-user"); wait_stream(h, stream, FALSE, RADIOS_AUDIO_OUTPUT_IDLE);
    const RadioSRoutingGraphNode *configured = radios_routing_graph_find_const(radios_routing_graph_ui_graph(h->editor), stream);
    g_assert_cmpstr(configured->username, ==, "edited-shoutcast-user");
    click(h, "routing-stream-start"); wait_stream(h, stream, TRUE, RADIOS_AUDIO_OUTPUT_FAILED);
    entry(h, "routing-stream-password", "corrected"); wait_stream(h, stream, FALSE, RADIOS_AUDIO_OUTPUT_IDLE);
    gtk_combo_box_set_active(GTK_COMBO_BOX(protocol), RG_STREAM_ICECAST); run_for(120);
    g_assert_true(gtk_widget_is_sensitive(find(h->page, "routing-stream-mount")));
    g_assert_true(gtk_widget_is_sensitive(find(h->page, "routing-stream-user")));
    g_assert_false(gtk_widget_is_sensitive(find(h->page, "routing-stream-id")));
    gtk_combo_box_set_active(GTK_COMBO_BOX(protocol), RG_STREAM_SHOUTCAST2);

    /* Start must commit text that has not yet lost focus in the port spinner. */
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(port), source_port - 2);
    gchar *typed_port = g_strdup_printf("%u", source_port - 1);
    gtk_entry_set_text(GTK_ENTRY(port), typed_port); g_free(typed_port);
    click(h, "routing-stream-start"); wait_stream(h, stream, TRUE, RADIOS_AUDIO_OUTPUT_ACTIVE); run_for(150);
    configured = radios_routing_graph_find_const(radios_routing_graph_ui_graph(h->editor), stream);
    g_assert_cmpuint(configured->port, ==, source_port - 1);
    g_assert_cmpint(g_atomic_int_get(&server.accepted), ==, 1); g_assert_cmpstr(server.accepted_login, ==, "corrected:#1");
    assert_stream_fields(h); RadioSPatchNodeSnapshot active = recorder_snapshot(h, stream);
    gtk_range_set_value(GTK_RANGE(gain), -1); run_for(180);
    RadioSPatchNodeSnapshot adjusted = recorder_snapshot(h, stream);
    g_assert_true(adjusted.streaming); g_assert_cmpint(adjusted.output_state, ==, RADIOS_AUDIO_OUTPUT_ACTIVE);
    g_assert_cmpint(adjusted.connected_us, ==, active.connected_us); g_assert_cmpuint(adjusted.sent_bytes, >, active.sent_bytes);
    g_assert_cmpint(g_atomic_int_get(&server.rejected), ==, 3);
    /* The gain edit's genuine autosave ACK must preserve the currently open
     * source editor and its selection while the accepted stream keeps sending.
     * Recreating the panel here interrupts normal connection setup typing. */
    GtkWidget *editing = g_object_ref(find(h->page, "routing-stream-user"));
    gtk_widget_grab_focus(editing); gtk_editable_select_region(GTK_EDITABLE(editing), 1, 8);
    SavedRevision editing_revision = {h, radios_data_document_revision(document(h))};
    wait_for(newer_revision, &editing_revision, "autosave acknowledges without rebuilding the source editor");
    g_assert_true(find(h->page, "routing-stream-user") == editing);
    g_assert_true(gtk_window_get_focus(GTK_WINDOW(h->window)) == editing);
    gint selection_start = 0, selection_end = 0;
    g_assert_true(gtk_editable_get_selection_bounds(GTK_EDITABLE(editing), &selection_start, &selection_end));
    g_assert_cmpint(selection_start, ==, 1); g_assert_cmpint(selection_end, ==, 8);
    g_object_unref(editing);
    RadioSPatchNodeSnapshot acknowledged = recorder_snapshot(h, stream);
    g_assert_true(acknowledged.streaming); g_assert_cmpint(acknowledged.output_state, ==, RADIOS_AUDIO_OUTPUT_ACTIVE);
    g_assert_cmpint(acknowledged.connected_us, ==, active.connected_us);
    g_assert_cmpuint(acknowledged.sent_bytes, >, adjusted.sent_bytes);
    g_assert_cmpint(g_atomic_int_get(&server.accepted), ==, 1);
    click(h, "routing-stream-stop"); wait_stream(h, stream, FALSE, RADIOS_AUDIO_OUTPUT_IDLE);
    SavedRevision saved_settings = {h, radios_data_document_revision(document(h))};
    click(h, "routing-settings-save"); wait_for(newer_revision, &saved_settings, "edited SHOUTcast username saved");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(h->page, "routing-stream-password"))), ==, "corrected");
    select_node(h, kind_id(h, RG_MAIN)); select_node(h, stream);
    GtkWidget *password = find(h->page, "routing-stream-password"), *show = find(h->page, "routing-stream-password-show");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(password)), ==, "corrected");
    g_assert_true(GTK_IS_TOGGLE_BUTTON(show)); g_assert_false(gtk_entry_get_visibility(GTK_ENTRY(password)));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(show), TRUE); g_assert_true(gtk_entry_get_visibility(GTK_ENTRY(password)));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(show), FALSE); g_assert_false(gtk_entry_get_visibility(GTK_ENTRY(password)));
    SavedConfiguration stored = {0}; JsonNode *query = radios_data_body(&h->ctx);
    json_object_set_string_member(json_node_get_object(query), "kind", "audio_routing");
    g_assert_true(radios_data_request(&h->ctx, h->page, "POST", "/v1/workflows/desktop/list", query, configuration_read, &stored, NULL));
    json_node_free(query); wait_for(configuration_read_done, &stored, "saved routing JSON inspected for session passwords");
    g_assert_nonnull(strstr(stored.json, "edited-shoutcast-user"));
    g_assert_null(strstr(stored.json, "corrected")); g_assert_null(strstr(stored.json, "\"password\"")); g_free(stored.json);
    close_harness(h);
    g_cancellable_cancel(server.cancel); g_thread_join(server.thread);
    g_object_unref(server.listener); g_object_unref(server.cancel);
    h = open_harness(); wait_for(stopped, h, "reopening SHOUTcast settings does not start a stream"); select_node(h, stream);
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(h->page, "routing-stream-user"))), ==, "edited-shoutcast-user");
    g_assert_true(gtk_widget_is_sensitive(find(h->page, "routing-stream-user")));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(h->page, "routing-stream-password"))), ==, "");
    /* Opting into password persistence must survive the real durable write
     * path and reopen; a rejected recovery record must not look like a save. */
    entry(h, "routing-stream-password", "saved-private-routing-fixture");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(find(h->page, "routing-stream-password-save")), TRUE);
    SavedRevision password_revision = {h, radios_data_document_revision(document(h))};
    click(h, "routing-settings-save");
    wait_for(newer_revision, &password_revision, "saved source password receives a real database acknowledgement");
    close_harness(h);
    h = open_harness(); wait_for(stopped, h, "saved password reopen never starts a stream"); select_node(h, stream);
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(find(h->page, "routing-stream-password-save"))));
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(h->page, "routing-stream-password"))), ==, "saved-private-routing-fixture");
    close_harness(h);
}


/* Metadata changes are independent of transport/server credentials. This
 * deliberately uses the current real panel and acknowledged routing document;
 * it neither opens an output nor edits another operator's configuration. */
TEST_LOCAL void metadata_source(Harness *h, const gchar *source)
{
    GtkWidget *combo = find(h->page, "routing-stream-metadata-source");
    g_assert_true(GTK_IS_COMBO_BOX(combo)); g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), source)); run_for(150);
}
TEST_LOCAL void metadata_control_geometry(Harness *h)
{
    GtkWidget *body = radios_routing_graph_ui_settings(h->editor);
    const gchar *names[] = {"routing-stream-metadata-source", "routing-stream-metadata-player", "routing-stream-metadata-format",
        "routing-stream-metadata", "routing-stream-metadata-send", "routing-stream-metadata-source-state"};
    GtkAllocation bounds; gtk_widget_get_allocation(body, &bounds);
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkWidget *control = find(h->page, names[i]); g_assert_nonnull(control);
        if (!gtk_widget_get_visible(control)) continue;
        GtkAllocation allocation; gtk_widget_get_allocation(control, &allocation); gint x = 0, y = 0;
        g_assert_true(gtk_widget_translate_coordinates(control, body, 0, 0, &x, &y));
        g_assert_cmpint(allocation.width, >, 0); g_assert_cmpint(x, >=, 0);
        g_assert_cmpint(x + allocation.width, <=, bounds.width + 2);
    }
}
TEST_LOCAL void test_stream_metadata_settings(void)
{
    Harness *h = open_harness(); guint stream = kind_id(h, RG_STREAM);
    if (!stream) stream = palette_add(h, RG_STREAM);
    select_node(h, stream);
    const RadioSRoutingGraphNode *configured = radios_routing_graph_find_const(radios_routing_graph_ui_graph(h->editor), stream);
    RadioSRoutingGraphNode server_before = *configured;
    GtkWidget *metadata = find(h->page, "routing-stream-metadata"); g_assert_true(GTK_IS_ENTRY(metadata));
    metadata_source(h, "manual"); g_assert_true(gtk_editable_get_editable(GTK_EDITABLE(metadata)));
    entry(h, "routing-stream-metadata", "Manuel yayın bilgisi 🎧 / korunmalı");
    metadata_source(h, "player");
    entry(h, "routing-stream-metadata-format", "{title} • {artist}");
    GtkWidget *player = find(h->page, "routing-stream-metadata-player"); g_assert_true(GTK_IS_COMBO_BOX(player));
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(player), "3")); run_for(150);
    g_assert_false(gtk_editable_get_editable(GTK_EDITABLE(metadata)));
    g_assert_false(gtk_widget_is_sensitive(find(h->page, "routing-stream-metadata-send")));
    GtkWidget *status = find(h->page, "routing-stream-metadata-source-state"); g_assert_true(GTK_IS_LABEL(status));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(status)), !=, "");
    metadata_control_geometry(h);
    configured = radios_routing_graph_find_const(radios_routing_graph_ui_graph(h->editor), stream);
    g_assert_cmpstr(configured->host, ==, server_before.host); g_assert_cmpstr(configured->mount, ==, server_before.mount);
    g_assert_cmpstr(configured->username, ==, server_before.username); g_assert_cmpuint(configured->port, ==, server_before.port);
    g_assert_cmpuint(configured->protocol, ==, server_before.protocol); g_assert_cmpuint(configured->bitrate_kbps, ==, server_before.bitrate_kbps);
    g_assert_cmpstr(configured->metadata_manual, ==, "Manuel yayın bilgisi 🎧 / korunmalı");
    g_assert_cmpstr(configured->metadata_format, ==, "{title} • {artist}"); g_assert_cmpuint(configured->metadata_player, ==, 3);
    SavedRevision revision = {h, radios_data_document_revision(document(h))};
    click(h, "routing-settings-save"); wait_for(newer_revision, &revision, "metadata source/template/manual draft save acknowledged");
    /* Let the preexisting graph-edit autosave finish before attributing any
     * later write to read-only metadata polling. */
    run_for(2200); gint64 acknowledged = radios_data_document_revision(document(h)); run_for(1200);
    g_assert_cmpint(radios_data_document_revision(document(h)), ==, acknowledged);
    /* Independent backend readback guards against a local-only save claim. */
    SavedConfiguration stored = {0}; JsonNode *query = radios_data_body(&h->ctx);
    json_object_set_string_member(json_node_get_object(query), "kind", "audio_routing");
    g_assert_true(radios_data_request(&h->ctx, h->page, "POST", "/v1/workflows/desktop/list", query, configuration_read, &stored, NULL));
    json_node_free(query); wait_for(configuration_read_done, &stored, "metadata acknowledged JSON readback");
    g_assert_nonnull(strstr(stored.json, "\"metadata_source\":\"player\""));
    g_assert_nonnull(strstr(stored.json, "\"metadata_player\":3"));
    g_assert_nonnull(strstr(stored.json, "Manuel yayın bilgisi 🎧 / korunmalı"));
    g_assert_nonnull(strstr(stored.json, "{title} • {artist}")); g_free(stored.json);
    select_node(h, kind_id(h, RG_MAIN)); select_node(h, stream);
    metadata_source(h, "manual");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(h->page, "routing-stream-metadata"))), ==, "Manuel yayın bilgisi 🎧 / korunmalı");
    metadata_source(h, "automation"); metadata_source(h, "player");
    revision = (SavedRevision){h, radios_data_document_revision(document(h))};
    click(h, "routing-settings-save"); wait_for(newer_revision, &revision, "selected player metadata mode persisted before close");
    close_harness(h); h = open_harness(); wait_for(stopped, h, "metadata reopen never starts transport"); select_node(h, stream);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(h->page, "routing-stream-metadata-source"))), ==, "player");
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(find(h->page, "routing-stream-metadata-player"))), ==, "3");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(h->page, "routing-stream-metadata-format"))), ==, "{title} • {artist}");
    metadata_source(h, "manual");
    g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(h->page, "routing-stream-metadata"))), ==, "Manuel yayın bilgisi 🎧 / korunmalı");
    gtk_window_resize(GTK_WINDOW(h->window), 900, 720); run_for(200); metadata_control_geometry(h);
    close_harness(h); run_for(250); /* No metadata timer/callback may survive its owner. */
}

int main(int argc, char **argv)
{
    g_assert_cmpint(argc, >=, 2); gchar *root = g_strdup(argv[1]);
    for (gint i = 1; i < argc; ++i) argv[i] = argv[i + 1];
    --argc;
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
#if RADIOS_ROUTING_NATIVE_INPUT
    if (!GDK_IS_X11_DISPLAY(gdk_display_get_default())) { g_free(root); return 77; }
    Display *display = GDK_DISPLAY_XDISPLAY(gdk_display_get_default()); gint event, xerror, major, minor;
    if (!XTestQueryExtension(display, &event, &xerror, &major, &minor)) { g_free(root); return 77; }
    Window pointer_root, pointer_child; gint original_x, original_y, wx, wy; guint mask;
    g_assert_true(XQueryPointer(display, DefaultRootWindow(display), &pointer_root, &pointer_child, &original_x, &original_y, &wx, &wy, &mask));
#endif
    GError *error = NULL; g_assert_true(radios_config_load(root, NULL, &config, &error)); g_assert_no_error(error); g_free(root);
    reader = radios_data_client_new(&config, &error); g_assert_no_error(error);
#if RADIOS_ROUTING_NATIVE_INPUT
    g_test_add_func("/native-routing-graph/drag-connect-save-reopen-pcm", test_save_reopen_pcm);
#else
    g_test_add_func("/native-routing/controls-save-safe-reopen", test_save_reopen_pcm);
    g_test_add_func("/native-routing/viewer-denied", test_viewer_denied);
    g_test_add_func("/native-routing/close-pending-start", test_close_pending_start);
    g_test_add_func("/native-routing/reconnect-rejects-old-recording-authorization", test_reconnect_viewer);
    g_test_add_func("/native-routing/same-user-new-client-rejects-old-authorization", test_reconnect_same_user);
    g_test_add_func("/native-routing/stream-error-edit-restart", test_stream_error_edit_restart);
    g_test_add_func("/native-routing/stream-metadata-settings-preserve-reopen", test_stream_metadata_settings);
#endif
    gint result = g_test_run();
#if RADIOS_ROUTING_NATIVE_INPUT
    XTestFakeMotionEvent(display, -1, original_x, original_y, CurrentTime); XFlush(display);
#endif
    radios_data_client_unref(reader); radios_config_clear(&config); g_free(last_notice); return result;
}
