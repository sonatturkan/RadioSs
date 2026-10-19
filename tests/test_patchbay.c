#define _POSIX_C_SOURCE 200809L
#include "radios/audio_patchbay.h"
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <signal.h>

static guint add(RadioSRoutingGraph *graph, RadioSRoutingGraphKind kind)
{
    guint id = 0; GError *error = NULL;
    g_assert_true(radios_routing_graph_add(graph, kind, 10 + graph->node_count * 20, 20, &id, &error)); g_assert_no_error(error);
    return id;
}
/* Live run: offline engine plays a tone on deck 1 (mixer channel 1). */
typedef struct {
    GSocket *socket;
    gboolean shoutcast;
    GThread *thread;
    gint stop;
    GString *head;
    gsize body;
    gchar password[64];
} Server;
static gssize receive(GSocket *socket, gchar *buffer, gsize size, Server *server)
{
    for (;;) {
        GError *error = NULL; gssize got = g_socket_receive(socket, buffer, size, NULL, &error);
        if (got >= 0) return got;
        gboolean timeout = g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT); g_clear_error(&error);
        if (!timeout || g_atomic_int_get(&server->stop)) return -1;
    }
}
static gpointer serve(gpointer data)
{
    Server *server = data; GError *error = NULL;
    GSocket *client = g_socket_accept(server->socket, NULL, &error);
    if (!client) { g_clear_error(&error); return NULL; }
    g_socket_set_timeout(client, 1); gchar buffer[8192];
    if (server->shoutcast) {
        gsize length = 0; gchar c;
        while (receive(client, &c, 1, server) == 1 && c != '\n') if (c != '\r' && length < sizeof server->password - 1) server->password[length++] = c;
        const gchar *ok = "OK2\r\nicy-caps:11\r\n\r\n"; g_socket_send(client, ok, strlen(ok), NULL, NULL);
    }
    while (!strstr(server->head->str, "\r\n\r\n")) {
        gssize got = receive(client, buffer, 1, server); if (got <= 0) break;
        g_string_append_len(server->head, buffer, got);
    }
    if (!server->shoutcast) { const gchar *ok = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n"; g_socket_send(client, ok, strlen(ok), NULL, NULL); }
    gssize got;
    while ((got = receive(client, buffer, sizeof buffer, server)) > 0) server->body += (gsize)got;
    g_socket_close(client, NULL); g_object_unref(client); return NULL;
}
static guint16 server_start(Server *server, gboolean shoutcast)
{
    server->shoutcast = shoutcast; server->head = g_string_new(NULL);
    server->socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, NULL);
    GInetAddress *loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    GSocketAddress *address = g_inet_socket_address_new(loopback, 0);
    g_assert_true(g_socket_bind(server->socket, address, TRUE, NULL)); g_assert_true(g_socket_listen(server->socket, NULL));
    g_socket_set_timeout(server->socket, 20);
    GSocketAddress *bound = g_socket_get_local_address(server->socket, NULL);
    guint16 port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
    g_object_unref(bound); g_object_unref(address); g_object_unref(loopback);
    server->thread = g_thread_new("fake-stream-server", serve, server); return port;
}
static void server_stop(Server *server)
{
    g_atomic_int_set(&server->stop, 1); g_thread_join(server->thread);
    g_socket_close(server->socket, NULL); g_object_unref(server->socket); g_string_free(server->head, TRUE);
}
typedef struct { RadioSAudioEngine *engine; gint stop; } Renderer;
static gpointer render(gpointer data)
{
    Renderer *renderer = data; gfloat main[480 * 2], cue[480 * 2], monitor[480 * 2]; gint64 next = g_get_monotonic_time();
    while (!g_atomic_int_get(&renderer->stop)) {
        g_assert_true(radios_audio_engine_render(renderer->engine, 480, main, cue, monitor, NULL));
        next += 10000; gint64 wait = next - g_get_monotonic_time(); if (wait > 0) g_usleep((gulong)wait);
    }
    return NULL;
}
static void word(GByteArray *bytes, guint value, guint width)
{ for (guint index = 0; index < width; ++index) { guchar byte = (guchar)(value >> (index * 8)); g_byte_array_append(bytes, &byte, 1); } }
static gchar *tone(const gchar *directory, gchar **sha)
{
    GByteArray *bytes = g_byte_array_new(); guint frames = 48000 * 10, size = frames * 4;
    g_byte_array_append(bytes, (const guchar *)"RIFF", 4); word(bytes, size + 36, 4);
    g_byte_array_append(bytes, (const guchar *)"WAVEfmt ", 8); word(bytes, 16, 4); word(bytes, 1, 2); word(bytes, 2, 2);
    word(bytes, 48000, 4); word(bytes, 192000, 4); word(bytes, 4, 2); word(bytes, 16, 2);
    g_byte_array_append(bytes, (const guchar *)"data", 4); word(bytes, size, 4);
    for (guint frame = 0; frame < frames; ++frame) {
        gint16 sample = (gint16)lrint(14000 * sin(2 * G_PI * 997 * frame / 48000)); word(bytes, (guint16)sample, 2); word(bytes, (guint16)sample, 2);
    }
    gchar *path = g_build_filename(directory, "tone.wav", NULL);
    g_assert_true(g_file_set_contents(path, (gchar *)bytes->data, bytes->len, NULL));
    *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes->data, bytes->len); g_byte_array_unref(bytes); return path;
}
static gdouble wav_rms(const gchar *path, guint64 *frames)
{
    gchar *data = NULL; gsize length = 0; g_assert_true(g_file_get_contents(path, &data, &length, NULL));
    g_assert_cmpuint(length, >=, 44); g_assert_true(!memcmp(data, "RIFF", 4) && !memcmp(data + 36, "data", 4));
    guint32 bytes = (guint8)data[40] | (guint8)data[41] << 8 | (guint8)data[42] << 16 | (guint32)(guint8)data[43] << 24;
    g_assert_cmpuint(bytes + 44, ==, length);
    *frames = bytes / 4; gdouble squares = 0; const gint16 *pcm = (const gint16 *)(data + 44);
    for (guint64 i = *frames / 4; i < *frames; ++i) squares += (pcm[i * 2] / 32768.0) * (pcm[i * 2] / 32768.0);
    gdouble rms = *frames ? sqrt(squares / (*frames - *frames / 4)) : 0; g_free(data); return rms;
}
static const RadioSPatchNodeSnapshot *node(const RadioSAudioPatchbaySnapshot *snapshot, guint id)
{
    for (guint i = 0; i < snapshot->node_count; ++i) if (snapshot->nodes[i].id == id) return &snapshot->nodes[i];
    return NULL;
}
static void set_stream(RadioSRoutingGraph *graph, guint id, RadioSRoutingStreamProtocol protocol, guint16 port)
{
    RadioSRoutingGraphNode copy = *radios_routing_graph_find(graph, id);
    copy.protocol = protocol; g_strlcpy(copy.host, "127.0.0.1", sizeof copy.host); copy.port = port;
    g_strlcpy(copy.mount, "/canli", sizeof copy.mount); g_strlcpy(copy.station, "RadioS Test", sizeof copy.station);
    g_strlcpy(copy.genre, "Test", sizeof copy.genre); copy.bitrate_kbps = 128;
    g_assert_true(radios_routing_graph_update(graph, &copy, NULL));
}
static void live_run(void)
{
    gchar *directory = g_build_filename(RADIOS_TEST_PROJECT_ROOT, "build", "patchbay-XXXXXX", NULL);
    g_assert_nonnull(g_mkdtemp(directory));
    gchar *sha = NULL, *path = tone(directory, &sha), *media = g_uuid_string_random(), *version = g_uuid_string_random();
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioLoadSpec spec = {path, sha, media, version, 0, 0};
    g_assert_true(radios_audio_engine_load(engine, 0, &spec, NULL));
    g_assert_true(radios_audio_engine_play(engine, 0, NULL));
    Renderer renderer = {engine, 0}; GThread *thread = g_thread_new("render", render, &renderer);
    Server icecast = {0}, shoutcast = {0};
    guint16 icecast_port = server_start(&icecast, FALSE), shoutcast_port = server_start(&shoutcast, TRUE);
    RadioSRoutingGraph *graph = g_new0(RadioSRoutingGraph, 1);
    guint channel = add(graph, RG_MIXER), meter = add(graph, RG_METER), recorder = add(graph, RG_RECORDER);
    guint send = add(graph, RG_MIXER), tap = add(graph, RG_MIXER), line_recorder = add(graph, RG_RECORDER);
    guint main_block = add(graph, RG_MAIN), ice = add(graph, RG_STREAM), shout = add(graph, RG_STREAM);
    RadioSRoutingGraphNode copy = *radios_routing_graph_find(graph, send); copy.channel = 6; g_assert_true(radios_routing_graph_update(graph, &copy, NULL));
    copy = *radios_routing_graph_find(graph, tap); copy.channel = 6; g_assert_true(radios_routing_graph_update(graph, &copy, NULL));
    copy = *radios_routing_graph_find(graph, line_recorder); g_strlcpy(copy.prefix, "hat", sizeof copy.prefix);
    g_assert_true(radios_routing_graph_update(graph, &copy, NULL));
    set_stream(graph, ice, RG_STREAM_ICECAST, icecast_port); set_stream(graph, shout, RG_STREAM_SHOUTCAST1, (guint16)(shoutcast_port - 1));
    g_assert_true(radios_routing_graph_connect(graph, channel, meter, NULL));
    g_assert_true(radios_routing_graph_connect(graph, meter, recorder, NULL));
    g_assert_true(radios_routing_graph_connect(graph, channel, send, NULL));      /* channel 1 into channel 6 */
    g_assert_true(radios_routing_graph_connect(graph, tap, line_recorder, NULL)); /* channel 6 recorded */
    g_assert_true(radios_routing_graph_connect(graph, main_block, ice, NULL));
    g_assert_true(radios_routing_graph_connect(graph, main_block, shout, NULL));
    /* An unreachable server keeps the test off the host's audio. */
    RadioSAudioPatchbay *patchbay = radios_audio_patchbay_new("unix:/nonexistent/radios-patchbay-test");
    radios_audio_patchbay_set_engine(patchbay, engine); radios_audio_patchbay_set_recording_root(patchbay, directory);
    GError *error = NULL;
    g_assert_true(radios_audio_patchbay_apply(patchbay, graph, TRUE, &error)); g_assert_no_error(error);
    /* Requests made before the plan is in place are kept until it is. */
    g_assert_true(radios_audio_patchbay_record(patchbay, recorder, &error)); g_assert_no_error(error);
    RadioSAudioPatchbaySnapshot *snapshot = g_new0(RadioSAudioPatchbaySnapshot, 1);
    gint64 deadline = g_get_monotonic_time() + 5000000;
    do { g_usleep(20000); radios_audio_patchbay_snapshot(patchbay, snapshot); } while ((snapshot->busy || !snapshot->running) && g_get_monotonic_time() < deadline);
    g_assert_true(snapshot->running); g_assert_false(snapshot->busy);
    g_assert_true(radios_audio_patchbay_record(patchbay, line_recorder, &error)); g_assert_no_error(error);
    g_assert_false(radios_audio_patchbay_stream_start(patchbay, ice, "", &error)); g_clear_error(&error);
    g_assert_true(radios_audio_patchbay_stream_start(patchbay, ice, "kaynak-sifre", &error)); g_assert_no_error(error);
    g_assert_true(radios_audio_patchbay_stream_start(patchbay, shout, "kaynak-sifre", &error)); g_assert_no_error(error);
    gfloat peak = 0; deadline = g_get_monotonic_time() + 4000000;
    while (g_get_monotonic_time() < deadline) {
        g_usleep(100000); radios_audio_patchbay_snapshot(patchbay, snapshot);
        const RadioSPatchNodeSnapshot *m = node(snapshot, meter); g_assert_nonnull(m); peak = MAX(peak, MAX(m->peak[0], m->peak[1]));
    }
    const RadioSPatchNodeSnapshot *r = node(snapshot, recorder), *l = node(snapshot, line_recorder);
    const RadioSPatchNodeSnapshot *i = node(snapshot, ice), *s = node(snapshot, shout);
    g_test_message("meter peak %.3f, recorder %" G_GUINT64_FORMAT " frames, line %" G_GUINT64_FORMAT
        ", icecast %s/%d %" G_GUINT64_FORMAT " B, shoutcast %s/%d %" G_GUINT64_FORMAT " B, underruns %" G_GUINT64_FORMAT,
        peak, r->total_frames, l->total_frames, i->error, i->output_state, i->sent_bytes, s->error, s->output_state, s->sent_bytes, snapshot->underruns);
    g_assert_cmpfloat(peak, >, 0.2);
    g_assert_true(r->recording); g_assert_cmpuint(r->total_frames, >, 48000 * 3);
    g_assert_true(l->recording); g_assert_cmpuint(l->total_frames, >, 48000 * 3);
    g_assert_cmpint(i->output_state, ==, RADIOS_AUDIO_OUTPUT_ACTIVE); g_assert_cmpint(s->output_state, ==, RADIOS_AUDIO_OUTPUT_ACTIVE);
    g_assert_true(radios_audio_patchbay_stream_metadata(patchbay, shout, "Sanatçı - Parça", &error)); g_assert_no_error(error);
    gchar *recorded = g_strdup(r->path), *line_recorded = g_strdup(l->path);
    g_assert_true(radios_audio_patchbay_record_split(patchbay, recorder, &error)); g_assert_no_error(error);
    g_usleep(300000); radios_audio_patchbay_snapshot(patchbay, snapshot);
    gchar *second = g_strdup(node(snapshot, recorder)->path); g_assert_cmpstr(second, !=, recorded);
    g_assert_cmpuint(node(snapshot, recorder)->segment, ==, 2);
    radios_audio_patchbay_record_stop(patchbay, recorder); radios_audio_patchbay_stream_stop(patchbay, ice);
    g_assert_true(radios_audio_patchbay_apply(patchbay, graph, FALSE, NULL)); /* stops the rest */
    deadline = g_get_monotonic_time() + 5000000;
    do { g_usleep(20000); radios_audio_patchbay_snapshot(patchbay, snapshot); } while ((snapshot->busy || snapshot->running) && g_get_monotonic_time() < deadline);
    g_assert_false(snapshot->running); g_assert_false(node(snapshot, line_recorder)->recording); g_assert_false(node(snapshot, shout)->streaming);
    radios_audio_patchbay_unref(patchbay); /* joins writers: every WAV is final */
    g_atomic_int_set(&renderer.stop, 1); g_thread_join(thread);
    server_stop(&icecast); server_stop(&shoutcast);
    g_test_message("icecast head: %s", icecast.head->len ? "received" : "none");
    guint64 frames = 0; gdouble rms = wav_rms(recorded, &frames);
    g_test_message("channel WAV %" G_GUINT64_FORMAT " frames rms %.3f", frames, rms);
    g_assert_cmpuint(frames, >, 48000 * 3); g_assert_cmpfloat(rms, >, 0.15);
    rms = wav_rms(line_recorded, &frames);
    g_test_message("line WAV %" G_GUINT64_FORMAT " frames rms %.3f", frames, rms);
    g_assert_cmpuint(frames, >, 48000 * 3); g_assert_cmpfloat(rms, >, 0.15);
    g_assert_true(g_file_test(second, G_FILE_TEST_IS_REGULAR));
    g_assert_cmpuint(icecast.body, >, 20000); g_assert_cmpuint(shoutcast.body, >, 20000);
    g_assert_cmpstr(shoutcast.password, ==, "kaynak-sifre");
    radios_audio_engine_unref(engine);
    /* Clean up every file the run produced. */
    GDir *dir = g_dir_open(directory, 0, NULL); const gchar *leaf;
    while ((leaf = g_dir_read_name(dir))) { gchar *file = g_build_filename(directory, leaf, NULL); g_unlink(file); g_free(file); }
    g_dir_close(dir); g_rmdir(directory);
    g_free(recorded); g_free(line_recorded); g_free(second); g_free(snapshot); g_free(graph);
    g_free(path); g_free(sha); g_free(media); g_free(version); g_free(directory);
}
static void icecast_headers(void)
{
    /* Separate check of the Icecast request so a failure names the header. */
    Server icecast = {0}; guint16 port = server_start(&icecast, FALSE);
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioPatchbay *patchbay = radios_audio_patchbay_new("unix:/nonexistent/radios-patchbay-test");
    radios_audio_patchbay_set_engine(patchbay, engine);
    RadioSRoutingGraph *graph = g_new0(RadioSRoutingGraph, 1);
    guint main_block = add(graph, RG_MAIN), ice = add(graph, RG_STREAM);
    set_stream(graph, ice, RG_STREAM_ICECAST, port);
    RadioSRoutingGraphNode copy = *radios_routing_graph_find(graph, ice); copy.public_listing = TRUE;
    g_strlcpy(copy.description, "Deneme yayını", sizeof copy.description); g_assert_true(radios_routing_graph_update(graph, &copy, NULL));
    g_assert_true(radios_routing_graph_connect(graph, main_block, ice, NULL));
    g_assert_true(radios_audio_patchbay_apply(patchbay, graph, TRUE, NULL));
    g_assert_true(radios_audio_patchbay_stream_start(patchbay, ice, "gizli", NULL));
    Renderer renderer = {engine, 0}; GThread *thread = g_thread_new("render", render, &renderer);
    gint64 deadline = g_get_monotonic_time() + 5000000;
    while (!strstr(icecast.head->str, "\r\n\r\n") && g_get_monotonic_time() < deadline) g_usleep(20000);
    g_test_message("%s", icecast.head->str);
    g_assert_nonnull(strstr(icecast.head->str, "PUT /canli HTTP/1.1"));
    g_assert_nonnull(strstr(icecast.head->str, "Ice-Name: RadioS Test"));
    g_assert_nonnull(strstr(icecast.head->str, "Ice-Genre: Test"));
    g_assert_nonnull(strstr(icecast.head->str, "Ice-Description: Deneme yay"));
    g_assert_nonnull(strstr(icecast.head->str, "Ice-Public: 1"));
    g_assert_nonnull(strstr(icecast.head->str, "Content-Type: audio/mpeg"));
    gchar *auth = g_base64_encode((const guchar *)"source:gizli", 12), *line = g_strdup_printf("Authorization: Basic %s", auth);
    g_assert_nonnull(strstr(icecast.head->str, line)); g_free(auth); g_free(line);
    radios_audio_patchbay_unref(patchbay);
    g_atomic_int_set(&renderer.stop, 1); g_thread_join(thread);
    server_stop(&icecast); radios_audio_engine_unref(engine); g_free(graph);
}
/* Real Pulse devices; only against a private server (RADIOS_TEST_PULSE_SERVER). */
static gchar *pactl(const gchar *server, const gchar *arguments)
{
    gchar *command = g_strdup_printf("env PULSE_SERVER=%s pactl %s", server, arguments), *out = NULL;
    g_assert_true(g_spawn_command_line_sync(command, &out, NULL, NULL, NULL)); g_free(command); return out;
}
static void set_device(RadioSRoutingGraph *graph, guint id, const gchar *device)
{
    RadioSRoutingGraphNode copy = *radios_routing_graph_find(graph, id);
    g_strlcpy(copy.device, device, sizeof copy.device); copy.push_to_talk = FALSE;
    g_assert_true(radios_routing_graph_update(graph, &copy, NULL));
}
static void pulse_devices(void)
{
    const gchar *server = g_getenv("RADIOS_TEST_PULSE_SERVER");
    if (!server || !*server) { g_test_skip("needs a private Pulse server in RADIOS_TEST_PULSE_SERVER"); return; }
    g_free(pactl(server, "load-module module-null-sink sink_name=fixture_source rate=48000 channels=2"));
    g_free(pactl(server, "load-module module-null-sink sink_name=fixture_output rate=48000 channels=2"));
    gchar *directory = g_build_filename(RADIOS_TEST_PROJECT_ROOT, "build", "patchbay-pulse-XXXXXX", NULL);
    g_assert_nonnull(g_mkdtemp(directory));
    gchar *sha = NULL, *path = tone(directory, &sha); g_free(sha);
    gchar *argv[] = {"pacat", "--playback", "--device=fixture_source", "--client-name=fixture-player", path, NULL};
    gchar **environment = g_environ_setenv(g_get_environ(), "PULSE_SERVER", server, TRUE);
    GPid player = 0;
    g_assert_true(g_spawn_async(NULL, argv, environment, G_SPAWN_SEARCH_PATH, NULL, NULL, &player, NULL));
    RadioSRoutingGraph *graph = g_new0(RadioSRoutingGraph, 1);
    guint input = add(graph, RG_INPUT), output = add(graph, RG_OUTPUT), loopback = add(graph, RG_INPUT), recorder = add(graph, RG_RECORDER);
    guint virtual = add(graph, RG_VIRTUAL), virtual_mic = add(graph, RG_INPUT), virtual_recorder = add(graph, RG_RECORDER);
    guint microphone = add(graph, RG_MICROPHONE), meter = add(graph, RG_METER);
    set_device(graph, input, "fixture_source.monitor"); set_device(graph, output, "fixture_output");
    set_device(graph, loopback, "fixture_output.monitor"); set_device(graph, microphone, "fixture_source.monitor");
    gchar mic_name[64]; g_snprintf(mic_name, sizeof mic_name, "radios_virtual_%u_mic", virtual); set_device(graph, virtual_mic, mic_name);
    RadioSRoutingGraphNode copy = *radios_routing_graph_find(graph, virtual); copy.as_microphone = TRUE;
    g_strlcpy(copy.name, "Sanal test", sizeof copy.name); g_assert_true(radios_routing_graph_update(graph, &copy, NULL));
    g_assert_true(radios_routing_graph_connect(graph, input, output, NULL));
    g_assert_true(radios_routing_graph_connect(graph, loopback, recorder, NULL));
    g_assert_true(radios_routing_graph_connect(graph, input, virtual, NULL));
    g_assert_true(radios_routing_graph_connect(graph, virtual_mic, virtual_recorder, NULL));
    g_assert_true(radios_routing_graph_connect(graph, microphone, meter, NULL));
    RadioSAudioPatchbay *patchbay = radios_audio_patchbay_new(server); radios_audio_patchbay_set_recording_root(patchbay, directory);
    /* Stopped: only the virtual interface exists. */
    g_assert_true(radios_audio_patchbay_apply(patchbay, graph, FALSE, NULL));
    RadioSAudioPatchbaySnapshot *snapshot = g_new0(RadioSAudioPatchbaySnapshot, 1);
    gint64 deadline = g_get_monotonic_time() + 10000000; const RadioSPatchNodeSnapshot *v = NULL;
    do { g_usleep(50000); radios_audio_patchbay_snapshot(patchbay, snapshot); v = node(snapshot, virtual); }
    while ((!v || !v->sink[0]) && g_get_monotonic_time() < deadline);
    g_assert_true(snapshot->connected); g_assert_nonnull(v); g_test_message("virtual sink %s, source %s", v->sink, v->source);
    g_assert_cmpstr(v->source, ==, mic_name);
    gchar *sinks = pactl(server, "list short sinks"), *sources = pactl(server, "list short sources");
    g_assert_nonnull(strstr(sinks, v->sink)); g_assert_nonnull(strstr(sources, mic_name)); g_free(sinks); g_free(sources);
    g_assert_true(radios_audio_patchbay_apply(patchbay, graph, TRUE, NULL));
    g_assert_true(radios_audio_patchbay_record(patchbay, recorder, NULL));
    g_assert_true(radios_audio_patchbay_record(patchbay, virtual_recorder, NULL));
    deadline = g_get_monotonic_time() + 4500000; gfloat peak = 0;
    while (g_get_monotonic_time() < deadline) {
        g_usleep(100000); radios_audio_patchbay_snapshot(patchbay, snapshot);
        peak = MAX(peak, node(snapshot, meter)->peak[0]);
    }
    for (guint i = 0; i < snapshot->node_count; ++i)
        g_test_message("block %u state %d error '%s' peak %.3f", snapshot->nodes[i].id, snapshot->nodes[i].state, snapshot->nodes[i].error, snapshot->nodes[i].peak[0]);
    g_assert_cmpint(node(snapshot, output)->state, ==, RADIOS_PATCH_ACTIVE); g_assert_cmpint(node(snapshot, virtual)->state, ==, RADIOS_PATCH_ACTIVE);
    g_assert_cmpfloat(peak, >, 0.2);
    gchar *played = g_strdup(node(snapshot, recorder)->path), *virtual_played = g_strdup(node(snapshot, virtual_recorder)->path);
    g_assert_cmpstr(played, !=, virtual_played);
    for (guint i = 0; i < snapshot->node_count; ++i) g_assert_cmpstr(snapshot->nodes[i].error, ==, "");
    /* The fixture player's stream moves into the virtual interface. */
    guint32 app = G_MAXUINT32, sink_index = G_MAXUINT32;
    for (guint i = 0; i < snapshot->application_count; ++i) if (strstr(snapshot->applications[i].name, "fixture-player")) app = snapshot->applications[i].index;
    for (guint i = 0; i < snapshot->output_count; ++i) if (g_str_equal(snapshot->outputs[i].name, v->sink)) sink_index = snapshot->outputs[i].index;
    g_assert_cmpuint(app, !=, G_MAXUINT32); g_assert_cmpuint(sink_index, !=, G_MAXUINT32);
    GError *error = NULL;
    g_assert_true(radios_audio_patchbay_move_application(patchbay, app, virtual, &error)); g_assert_no_error(error);
    deadline = g_get_monotonic_time() + 5000000; gboolean moved = FALSE;
    while (!moved && g_get_monotonic_time() < deadline) {
        g_usleep(100000); radios_audio_patchbay_snapshot(patchbay, snapshot);
        for (guint i = 0; i < snapshot->application_count; ++i) moved |= snapshot->applications[i].index == app && snapshot->applications[i].sink == sink_index;
    }
    g_assert_true(moved);
    radios_audio_patchbay_unref(patchbay);
    kill(player, SIGTERM); g_spawn_close_pid(player);
    sinks = pactl(server, "list short sinks"); g_assert_null(strstr(sinks, "radios_virtual_")); g_free(sinks);
    guint64 frames = 0; gdouble rms = wav_rms(played, &frames);
    g_test_message("output loopback WAV %" G_GUINT64_FORMAT " frames rms %.3f", frames, rms);
    g_assert_cmpuint(frames, >, 48000 * 3); g_assert_cmpfloat(rms, >, 0.1);
    rms = wav_rms(virtual_played, &frames);
    g_test_message("virtual microphone WAV %" G_GUINT64_FORMAT " frames rms %.3f", frames, rms);
    g_assert_cmpuint(frames, >, 48000 * 3); g_assert_cmpfloat(rms, >, 0.1);
    GDir *dir = g_dir_open(directory, 0, NULL); const gchar *leaf;
    while ((leaf = g_dir_read_name(dir))) { gchar *file = g_build_filename(directory, leaf, NULL); g_unlink(file); g_free(file); }
    g_dir_close(dir); g_rmdir(directory);
    g_free(played); g_free(virtual_played); g_free(snapshot); g_free(graph); g_free(path); g_free(directory); g_strfreev(environment);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/patchbay/live-mixer-recorder-streams", live_run);
    g_test_add_func("/patchbay/icecast-headers", icecast_headers);
    g_test_add_func("/patchbay/pulse-devices", pulse_devices);
    return g_test_run();
}
