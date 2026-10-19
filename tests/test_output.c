#define _POSIX_C_SOURCE 200809L
#include "radios/audio_output.h"
#include "../src/audio/audio_internal.h"
#include <glib/gstdio.h>
#include <arpa/inet.h>
#include <math.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef RADIOS_TEST_PROJECT_ROOT
#define RADIOS_TEST_PROJECT_ROOT "."
#endif
typedef struct { gchar *directory, *wave, *sha; } Fixture;
typedef struct {
    gint socket;
    guint port;
    GThread *thread;
    GPtrArray *connections;
    GMutex mutex;
    gint stopping;
    GByteArray *mp3;
    gchar *metadata, *stream_headers;
    gboolean reject, early;
    gint metadata_status;
} Server;
typedef struct { Server *server; gint socket; } Connection;
static void append16(GByteArray *bytes, guint16 value)
{ guint8 data[] = {(guint8)value, (guint8)(value >> 8)}; g_byte_array_append(bytes, data, 2); }
static void append32(GByteArray *bytes, guint32 value)
{ guint8 data[] = {(guint8)value, (guint8)(value >> 8), (guint8)(value >> 16), (guint8)(value >> 24)}; g_byte_array_append(bytes, data, 4); }
static void setup(Fixture *fixture, gconstpointer data)
{
    (void)data; gchar *build = g_build_filename(RADIOS_TEST_PROJECT_ROOT, "build", NULL);
    fixture->directory = g_build_filename(build, "audio-output-XXXXXX", NULL); g_assert_nonnull(g_mkdtemp(fixture->directory));
    fixture->wave = g_build_filename(fixture->directory, "source.wav", NULL); GByteArray *bytes = g_byte_array_new();
    guint frames = 48000;
    g_byte_array_append(bytes, (const guint8 *)"RIFF", 4); append32(bytes, frames * 4 + 36);
    g_byte_array_append(bytes, (const guint8 *)"WAVEfmt ", 8); append32(bytes, 16); append16(bytes, 1); append16(bytes, 2);
    append32(bytes, 48000); append32(bytes, 192000); append16(bytes, 4); append16(bytes, 16);
    g_byte_array_append(bytes, (const guint8 *)"data", 4); append32(bytes, frames * 4);
    for (guint i = 0; i < frames; ++i) {
        gint16 value = (gint16)lrint(0.35 * sin(2 * G_PI * 440 * i / 48000) * 32767);
        append16(bytes, (guint16)value); append16(bytes, (guint16)(-value / 2));
    }
    g_assert_true(g_file_set_contents(fixture->wave, (const gchar *)bytes->data, bytes->len, NULL));
    fixture->sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes->data, bytes->len);
    g_byte_array_unref(bytes); g_free(build);
}
static void teardown(Fixture *fixture, gconstpointer data)
{
    (void)data; GDir *directory = g_dir_open(fixture->directory, 0, NULL); const gchar *name;
    while ((name = g_dir_read_name(directory))) { gchar *path = g_build_filename(fixture->directory, name, NULL); g_assert_cmpint(g_unlink(path), ==, 0); g_free(path); }
    g_dir_close(directory); g_assert_cmpint(g_rmdir(fixture->directory), ==, 0);
    g_free(fixture->directory); g_free(fixture->wave); g_free(fixture->sha);
}
static RadioSAudioOutputSnapshot wait_state(RadioSAudioOutput *output, RadioSAudioOutputState state)
{
    RadioSAudioOutputSnapshot snapshot; gint64 until = g_get_monotonic_time() + 10000000;
    do {
        radios_audio_output_snapshot(output, &snapshot); if (snapshot.state == state) return snapshot;
        if (snapshot.state == RADIOS_AUDIO_OUTPUT_FAILED && state != RADIOS_AUDIO_OUTPUT_FAILED)
            g_error("Output failed while waiting for %d: %s", state, snapshot.error);
        g_usleep(1000);
    } while (g_get_monotonic_time() < until);
    g_error("Output did not reach state %d (actual %d)", state, snapshot.state); return snapshot;
}
static void load_play(RadioSAudioEngine *engine, guint deck, Fixture *fixture)
{
    gchar *media = g_uuid_string_random(), *version = g_uuid_string_random();
    RadioSAudioLoadSpec spec = {fixture->wave, fixture->sha, media, version, 0, 0};
    g_assert_true(radios_audio_engine_load(engine, deck, &spec, NULL)); g_free(media); g_free(version);
    RadioSAudioSnapshot snapshot; gint64 until = g_get_monotonic_time() + 10000000;
    do {
        radios_audio_engine_snapshot(engine, &snapshot);
        if (snapshot.decks[deck].state == RADIOS_AUDIO_READY && snapshot.decks[deck].buffered_frames >= 48000) break;
        g_assert_cmpint(snapshot.decks[deck].state, !=, RADIOS_AUDIO_ERROR); g_usleep(1000);
    } while (g_get_monotonic_time() < until);
    g_assert_cmpint(snapshot.decks[deck].state, ==, RADIOS_AUDIO_READY);
    g_assert_true(radios_audio_engine_play(engine, deck, NULL));
}
static void render(RadioSAudioEngine *engine, guint frames, gfloat pcm[3][8192])
{ g_assert_true(radios_audio_engine_render(engine, frames, pcm[0], pcm[1], pcm[2], NULL)); }
static guint32 read32(const guint8 *data)
{ return (guint32)data[0] | ((guint32)data[1] << 8) | ((guint32)data[2] << 16) | ((guint32)data[3] << 24); }
static void recording(Fixture *fixture, gconstpointer data)
{
    (void)data; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioOutput *output = radios_audio_engine_output_acquire(engine, RADIOS_AUDIO_OUTPUT_RECORD);
    gchar *path = g_build_filename(fixture->directory, "main.wav", NULL);
    load_play(engine, 0, fixture); load_play(engine, RADIOS_AUDIO_PREVIEW_DECK, fixture);
    RadioSAudioPreferences prefs; radios_audio_preferences_default(&prefs); prefs.main_gain_db = -6;
    g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL));
    g_assert_true(radios_audio_output_start_record(output, path, NULL)); wait_state(output, RADIOS_AUDIO_OUTPUT_ACTIVE);
    gfloat pcm[3][8192]; render(engine, 1000, pcm); GArray *expected = g_array_new(FALSE, FALSE, sizeof(gint16));
    for (guint i = 0; i < 2000; ++i) { gint16 value = (gint16)lrintf(pcm[0][i] * (pcm[0][i] < 0 ? 32768.0f : 32767.0f)); g_array_append_val(expected, value); }
    g_assert_true(radios_audio_engine_set_bus_gate(engine, RADIOS_AUDIO_BUS_MAIN, FALSE, NULL)); render(engine, 600, pcm);
    for (guint i = 0; i < 1200; ++i) { g_assert_cmpfloat(pcm[0][i], ==, 0); gint16 value = 0; g_array_append_val(expected, value); }
    radios_audio_output_stop(output); RadioSAudioOutputSnapshot snapshot = wait_state(output, RADIOS_AUDIO_OUTPUT_STOPPED);
    g_assert_true(snapshot.recording_finalized); g_assert_cmpuint(snapshot.frames_received, ==, 1600);
    g_assert_cmpuint(snapshot.frames_processed, ==, 1600); g_assert_cmpuint(snapshot.bytes_written, ==, 6400);
    gchar *bytes = NULL; gsize length = 0; g_assert_true(g_file_get_contents(path, &bytes, &length, NULL));
    g_assert_cmpuint(length, ==, 6444); g_assert_cmpmem(bytes, 4, "RIFF", 4);
    g_assert_cmpuint(read32((guint8 *)bytes + 40), ==, 6400); g_assert_cmpuint(read32((guint8 *)bytes + 24), ==, 48000);
    for (guint i = 0; i < expected->len; ++i) {
        const guint8 *sample = (guint8 *)bytes + 44 + i * 2;
        g_assert_cmpint((gint16)((guint16)sample[0] | ((guint16)sample[1] << 8)), ==, g_array_index(expected, gint16, i));
    }
    g_assert_true(radios_audio_output_start_record(output, path, NULL)); snapshot = wait_state(output, RADIOS_AUDIO_OUTPUT_FAILED);
    g_assert_nonnull(strstr(snapshot.error, "created")); gchar *after = NULL; gsize after_length = 0;
    g_assert_true(g_file_get_contents(path, &after, &after_length, NULL)); g_assert_cmpmem(bytes, length, after, after_length);
    g_free(after); g_free(bytes); g_array_unref(expected); radios_audio_engine_unref(engine);
    GError *error = NULL; g_assert_false(radios_audio_output_start_record(output, path, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_CLOSED); g_clear_error(&error);
    radios_audio_output_unref(output); g_free(path);
}
static gboolean receive_exact(gint socket, guint8 *bytes, gsize length)
{
    while (length) { ssize_t got = recv(socket, bytes, length, 0); if (got <= 0) return FALSE; bytes += got; length -= (gsize)got; }
    return TRUE;
}
static gchar *receive_line(gint socket)
{
    GString *line = g_string_new(NULL); guint8 value;
    while (line->len < 8192 && receive_exact(socket, &value, 1)) {
        if (value == '\n') return g_string_free(line, FALSE);
        if (value != '\r') g_string_append_c(line, (gchar)value);
    }
    g_string_free(line, TRUE); return NULL;
}
static void respond(gint socket, const gchar *response)
{
    gsize remaining = strlen(response);
    while (remaining) { ssize_t count = send(socket, response, remaining, MSG_NOSIGNAL); if (count <= 0) break; response += count; remaining -= (gsize)count; }
}
static gpointer connection_worker(gpointer data)
{
    Connection *connection = data; Server *server = connection->server; gint socket = connection->socket;
    struct timeval timeout = {10, 0}; setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
    GString *headers = g_string_new(NULL); gchar *line;
    while ((line = receive_line(socket))) {
        gboolean empty = !*line; g_string_append(headers, line); g_string_append_c(headers, '\n'); g_free(line); if (empty) break;
    }
    if (g_str_has_prefix(headers->str, "GET ")) {
        g_mutex_lock(&server->mutex); g_free(server->metadata); server->metadata = g_strdup(headers->str); g_mutex_unlock(&server->mutex);
        respond(socket, g_atomic_int_get(&server->metadata_status) == 403 ? "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n" :
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK");
    } else {
        g_mutex_lock(&server->mutex); g_free(server->stream_headers); server->stream_headers = g_strdup(headers->str); g_mutex_unlock(&server->mutex);
        if (server->reject) {
            respond(socket, "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
            shutdown(socket, SHUT_WR); guint8 discard[4096];
            while (recv(socket, discard, sizeof discard, 0) > 0) { }
        }
        else {
            if (server->early) respond(socket, "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n");
            GByteArray *bytes = g_byte_array_new();
            guint8 received[4096]; ssize_t got;
            while ((got = recv(socket, received, sizeof received, 0)) > 0) {
                if (bytes->len + (guint)got > 1048576) break;
                g_byte_array_append(bytes, received, (guint)got);
            }
            g_mutex_lock(&server->mutex); g_byte_array_append(server->mp3, bytes->data, bytes->len); g_mutex_unlock(&server->mutex);
            g_byte_array_unref(bytes);
            if (!server->early) respond(socket, "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        }
    }
    g_string_free(headers, TRUE); close(socket); g_free(connection); return NULL;
}
static gpointer server_worker(gpointer data)
{
    Server *server = data;
    while (!g_atomic_int_get(&server->stopping)) {
        struct pollfd fd = {server->socket, POLLIN, 0}; if (poll(&fd, 1, 100) <= 0) continue;
        gint socket = accept(server->socket, NULL, NULL); if (socket < 0) continue;
        Connection *connection = g_new(Connection, 1); connection->server = server; connection->socket = socket;
        g_ptr_array_add(server->connections, g_thread_new("output-http-fixture", connection_worker, connection));
    }
    return NULL;
}
static void server_start(Server *server, gboolean reject, gboolean early)
{
    memset(server, 0, sizeof *server); server->reject = reject; server->early = early; g_mutex_init(&server->mutex);
    g_atomic_int_set(&server->metadata_status, 200);
    server->connections = g_ptr_array_new(); server->mp3 = g_byte_array_new(); server->socket = socket(AF_INET, SOCK_STREAM, 0);
    g_assert_cmpint(server->socket, >=, 0); struct sockaddr_in address = {0}; address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    g_assert_cmpint(bind(server->socket, (struct sockaddr *)&address, sizeof address), ==, 0);
    socklen_t length = sizeof address; g_assert_cmpint(getsockname(server->socket, (struct sockaddr *)&address, &length), ==, 0);
    server->port = ntohs(address.sin_port); g_assert_cmpint(listen(server->socket, 8), ==, 0);
    server->thread = g_thread_new("output-http-listener", server_worker, server);
}
static void server_stop(Server *server)
{
    g_atomic_int_set(&server->stopping, TRUE); g_thread_join(server->thread); close(server->socket);
    for (guint i = 0; i < server->connections->len; ++i) g_thread_join(g_ptr_array_index(server->connections, i));
    g_ptr_array_unref(server->connections);
}
static void server_clear(Server *server)
{ g_byte_array_unref(server->mp3); g_free(server->metadata); g_free(server->stream_headers); g_mutex_clear(&server->mutex); }
static void streaming(Fixture *fixture, gconstpointer data)
{
    gboolean early = GPOINTER_TO_INT(data); Server server; server_start(&server, FALSE, early);
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); load_play(engine, 0, fixture);
    RadioSAudioOutput *output = radios_audio_engine_output_acquire(engine, RADIOS_AUDIO_OUTPUT_STREAM);
    gchar *url = g_strdup_printf("http://127.0.0.1:%u/live.mp3", server.port);
    g_assert_true(radios_audio_output_start_stream(output, url, "source", "test-only-secret", 128, NULL));
    wait_state(output, RADIOS_AUDIO_OUTPUT_ACTIVE);
    g_assert_true(radios_audio_output_metadata(output, "Çalan & eser / 12", NULL));
    RadioSAudioOutputSnapshot snapshot; gint64 until = g_get_monotonic_time() + 10000000;
    do { radios_audio_output_snapshot(output, &snapshot); if (snapshot.metadata_state != RADIOS_AUDIO_METADATA_PENDING) break; g_usleep(1000); }
    while (g_get_monotonic_time() < until);
    g_assert_cmpint(snapshot.metadata_state, ==, RADIOS_AUDIO_METADATA_SUCCESS); g_assert_cmpint(snapshot.metadata_http_status, ==, 200);
    gfloat pcm[3][8192];
    for (guint i = 0; i < 12; ++i) { render(engine, 4000, pcm); g_usleep(1000); }
    radios_audio_output_stop(output); snapshot = wait_state(output, RADIOS_AUDIO_OUTPUT_STOPPED);
    g_assert_cmpuint(snapshot.frames_received, ==, 48000); g_assert_cmpuint(snapshot.frames_processed, ==, 48000);
    g_assert_cmpint(snapshot.response_confirmed, ==, early); if (early) g_assert_cmpint(snapshot.http_status, ==, 200);
    g_assert_cmpuint(snapshot.bytes_written, >, 1000);
    server_stop(&server); g_assert_cmpuint(server.mp3->len, >, 1000);
    g_assert_nonnull(strstr(server.stream_headers, "PUT /live.mp3 HTTP/1.1"));
    g_assert_nonnull(strstr(server.stream_headers, "Content-Type: audio/mpeg"));
    g_assert_null(strstr(server.stream_headers, "Transfer-Encoding:"));
    g_assert_null(strstr(server.stream_headers, "Content-Length:"));
    g_assert_nonnull(strstr(server.metadata, "GET /admin/metadata?mode=updinfo&mount=%2Flive.mp3&song=%C3%87alan%20%26%20eser%20%2F%2012 HTTP/1.1"));
    gchar *path = g_build_filename(fixture->directory, early ? "early.mp3" : "actual.mp3", NULL);
    g_assert_true(g_file_set_contents(path, (const gchar *)server.mp3->data, server.mp3->len, NULL));
    gchar *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, server.mp3->data, server.mp3->len);
    GError *error = NULL; RadioSAudioDecoder *decoder = radios_audio_decoder_open(path, sha, NULL, &error); g_assert_no_error(error); g_assert_nonnull(decoder);
    guint64 frames = 0; gdouble energy = 0; gfloat decoded[8192]; gssize got;
    while ((got = radios_audio_decoder_read(decoder, decoded, 4096, NULL, &error)) > 0) {
        frames += (guint64)got; for (guint i = 0; i < (guint)got * 2; ++i) energy += decoded[i] * decoded[i];
    }
    g_assert_no_error(error); g_assert_cmpuint(frames, >=, 48000); g_assert_cmpuint(frames, <, 52000); g_assert_cmpfloat(energy, >, 100);
    radios_audio_decoder_close(decoder); g_free(sha); g_free(path); server_clear(&server);
    radios_audio_engine_unref(engine); radios_audio_output_unref(output); g_free(url);
}
static void rejection(Fixture *fixture, gconstpointer data)
{
    (void)data; Server server; server_start(&server, TRUE, FALSE); RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    load_play(engine, 0, fixture); RadioSAudioOutput *output = radios_audio_engine_output_acquire(engine, RADIOS_AUDIO_OUTPUT_STREAM);
    gchar *url = g_strdup_printf("http://127.0.0.1:%u/denied.mp3", server.port);
    g_assert_true(radios_audio_output_start_stream(output, url, "source", "test-only-secret", 128, NULL));
    gfloat pcm[3][8192]; for (guint i = 0; i < 4; ++i) { render(engine, 2000, pcm); g_usleep(10000); }
    RadioSAudioOutputSnapshot snapshot = wait_state(output, RADIOS_AUDIO_OUTPUT_FAILED);
    g_assert_cmpint(snapshot.http_status, ==, 403); g_assert_false(snapshot.response_confirmed); g_assert_cmpuint(snapshot.generation, ==, 1);
    g_assert_null(strstr(snapshot.error, "test-only-secret")); server_stop(&server); server_clear(&server);
    radios_audio_engine_unref(engine); radios_audio_output_unref(output); g_free(url);
}
static void overrun(Fixture *fixture, gconstpointer data)
{
    (void)data; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); load_play(engine, 0, fixture);
    RadioSAudioOutput *output = radios_audio_engine_output_acquire(engine, RADIOS_AUDIO_OUTPUT_RECORD);
    gchar *path = g_build_filename(fixture->directory, "overrun.wav", NULL);
    g_assert_true(radios_audio_output_start_record(output, path, NULL)); wait_state(output, RADIOS_AUDIO_OUTPUT_ACTIVE);
    gfloat value[2] = {0, 0}; radios_audio_output_push_internal(output, value, 240001);
    RadioSAudioOutputSnapshot snapshot = wait_state(output, RADIOS_AUDIO_OUTPUT_FAILED);
    g_assert_cmpuint(snapshot.overrun_frames, ==, 240001); g_assert_nonnull(strstr(snapshot.error, "overrun"));
    g_assert_true(snapshot.recording_finalized); gfloat pcm[3][8192]; render(engine, 2000, pcm);
    g_assert_cmpfloat(fabsf(pcm[0][100]), >, .01); radios_audio_output_snapshot(output, &snapshot);
    g_assert_cmpuint(snapshot.frames_received, ==, 0); radios_audio_engine_unref(engine); radios_audio_output_unref(output); g_free(path);
}
static void metadata_rejection(Fixture *fixture, gconstpointer data)
{
    (void)data; Server server; server_start(&server, FALSE, TRUE); g_atomic_int_set(&server.metadata_status, 403);
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); load_play(engine, 0, fixture);
    RadioSAudioOutput *output = radios_audio_engine_output_acquire(engine, RADIOS_AUDIO_OUTPUT_STREAM);
    gchar *url = g_strdup_printf("http://127.0.0.1:%u/live.mp3", server.port);
    g_assert_true(radios_audio_output_start_stream(output, url, "source", "test-only-secret", 128, NULL));
    wait_state(output, RADIOS_AUDIO_OUTPUT_ACTIVE); g_assert_true(radios_audio_output_metadata(output, "Denied title", NULL));
    RadioSAudioOutputSnapshot snapshot; gint64 until = g_get_monotonic_time() + 10000000;
    do { radios_audio_output_snapshot(output, &snapshot); if (snapshot.metadata_state != RADIOS_AUDIO_METADATA_PENDING) break; g_usleep(1000); }
    while (g_get_monotonic_time() < until);
    g_assert_cmpint(snapshot.metadata_state, ==, RADIOS_AUDIO_METADATA_FAILED); g_assert_cmpint(snapshot.metadata_http_status, ==, 403);
    g_assert_cmpint(snapshot.state, ==, RADIOS_AUDIO_OUTPUT_ACTIVE); g_assert_null(strstr(snapshot.metadata_error, "test-only-secret"));
    gfloat pcm[3][8192]; render(engine, 4096, pcm); radios_audio_output_stop(output); snapshot = wait_state(output, RADIOS_AUDIO_OUTPUT_STOPPED);
    g_assert_cmpuint(snapshot.frames_processed, ==, 4096); g_assert_true(snapshot.response_confirmed);
    server_stop(&server); server_clear(&server); radios_audio_engine_unref(engine); radios_audio_output_unref(output); g_free(url);
}
static void input_starvation(void)
{
    Server server; server_start(&server, FALSE, FALSE); RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioOutput *output = radios_audio_engine_output_acquire(engine, RADIOS_AUDIO_OUTPUT_STREAM);
    gchar *url = g_strdup_printf("http://127.0.0.1:%u/no-clock.mp3", server.port);
    g_assert_true(radios_audio_output_start_stream(output, url, "source", "test-only-secret", 128, NULL));
    RadioSAudioOutputSnapshot snapshot = wait_state(output, RADIOS_AUDIO_OUTPUT_FAILED);
    g_assert_cmpuint(snapshot.frames_processed, ==, 0); g_assert_cmpuint(snapshot.bytes_written, ==, 0);
    g_assert_nonnull(strstr(snapshot.error, "PCM stopped producing frames"));
    server_stop(&server); server_clear(&server); radios_audio_engine_unref(engine); radios_audio_output_unref(output); g_free(url);
}
static void validation(void)
{
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioOutput *record = radios_audio_engine_output_acquire(engine, RADIOS_AUDIO_OUTPUT_RECORD);
    RadioSAudioOutput *stream = radios_audio_engine_output_acquire(engine, RADIOS_AUDIO_OUTPUT_STREAM); GError *error = NULL;
    g_assert_null(radios_audio_engine_output_acquire(engine, RADIOS_AUDIO_OUTPUT_COUNT));
    g_assert_false(radios_audio_output_start_record(record, "relative.wav", &error)); g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID); g_clear_error(&error);
    const gchar *urls[] = {"file:///tmp/output", "http://user:secret@localhost/live", "http://localhost/", "http://localhost/live?q=secret", "http://localhost/live#secret"};
    for (guint i = 0; i < G_N_ELEMENTS(urls); ++i) {
        g_assert_false(radios_audio_output_start_stream(stream, urls[i], "source", "secret", 128, &error));
        g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID); g_clear_error(&error);
    }
    g_assert_false(radios_audio_output_start_stream(record, "http://localhost/live", "source", "secret", 128, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID); g_clear_error(&error);
    g_assert_false(radios_audio_output_metadata(stream, "title", &error)); g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_STATE); g_clear_error(&error);
    RadioSAudioOutputSnapshot snapshot; radios_audio_output_snapshot(stream, &snapshot); g_assert_cmpint(snapshot.state, ==, RADIOS_AUDIO_OUTPUT_IDLE);
    radios_audio_engine_unref(engine); radios_audio_output_unref(record); radios_audio_output_unref(stream);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); g_test_add_func("/output/validation", validation);
    g_test_add("/output/main-recording", Fixture, NULL, setup, recording, teardown);
    g_test_add("/output/mp3-http-metadata", Fixture, GINT_TO_POINTER(FALSE), setup, streaming, teardown);
    g_test_add("/output/mp3-early-http-acceptance", Fixture, GINT_TO_POINTER(TRUE), setup, streaming, teardown);
    g_test_add("/output/http-rejection", Fixture, NULL, setup, rejection, teardown);
    g_test_add("/output/queue-overrun", Fixture, NULL, setup, overrun, teardown);
    g_test_add("/output/metadata-rejection", Fixture, NULL, setup, metadata_rejection, teardown);
    g_test_add_func("/output/input-starvation", input_starvation);
    return g_test_run();
}
