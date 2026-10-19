#include "radios/audio_channel_input.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

/* R5.1 live channel inputs: specification rules, a real FFmpeg network input
 * from a private loopback HTTP server through a mixer strip to MAIN, its
 * reconnect after the stream ends, a prompt close while a connect is pending,
 * and (with owned virtual Pulse devices) a sound-card capture input. */

#define CHANNEL 4u

static gint16 sample_at(guint64 frame)
{
    return (gint16)lrint(8192.0 * sin(2.0 * G_PI * 997.0 * (double)frame / 48000.0));
}

static void append_le(GByteArray *bytes, guint32 value, guint length)
{
    guint8 data[4] = {value, value >> 8, value >> 16, value >> 24};
    g_byte_array_append(bytes, data, length);
}

static GBytes *sine_wav(guint frames)
{
    GByteArray *bytes = g_byte_array_new();
    g_byte_array_append(bytes, (const guint8 *)"RIFF", 4);
    append_le(bytes, frames * 4 + 36, 4);
    g_byte_array_append(bytes, (const guint8 *)"WAVEfmt ", 8);
    append_le(bytes, 16, 4);
    append_le(bytes, 1, 2);
    append_le(bytes, 2, 2);
    append_le(bytes, 48000, 4);
    append_le(bytes, 192000, 4);
    append_le(bytes, 4, 2);
    append_le(bytes, 16, 2);
    g_byte_array_append(bytes, (const guint8 *)"data", 4);
    append_le(bytes, frames * 4, 4);
    for (guint i = 0; i < frames; ++i) {
        const gint16 sample = sample_at(i);
        append_le(bytes, (guint16)sample, 2);
        append_le(bytes, (guint16)(-sample / 2), 2);
    }
    return g_byte_array_free_to_bytes(bytes);
}

static RadioSAudioChannelInputSpec input_spec(RadioSAudioChannelInputKind kind, const gchar *device, const gchar *name)
{
    RadioSAudioChannelInputSpec spec = {kind, {0}, {0}};
    g_strlcpy(spec.device, device, sizeof spec.device);
    g_strlcpy(spec.name, name, sizeof spec.name);
    return spec;
}

static void test_spec_rules(void)
{
    static const gchar *const accepted[] = {
        "http://radyo.example/yayin", "https://akis.example:8443/canli.aac", "rtp://239.0.0.1:5004",
        "udp://@:1234", "rtsp://kamera.local/ses", "HTTP://BUYUK.EXAMPLE/a" /* srt:// depends on the FFmpeg build: /channel-input/srt-needs-ffmpeg-protocol */
    };
    static const gchar *const rejected[] = {
        "file:///etc/passwd", "data:audio/wav;base64,AAAA", "ftp://x.example/y", "http://", "http:// bosluk/",
        "tcp://10.0.0.1:5000", "concat:a.wav|b.wav", "/home/kullanici/a.mp3", "http://x.example/\tsekme", "pipe:0", ""
    };
    GError *error = NULL;
    for (guint i = 0; i < G_N_ELEMENTS(accepted); ++i) {
        RadioSAudioChannelInputSpec spec = input_spec(RADIOS_CHANNEL_INPUT_NETWORK, accepted[i], "Ağ yayını");
        g_assert_true(radios_audio_channel_input_spec_validate(&spec, &error));
        g_assert_no_error(error);
    }
    for (guint i = 0; i < G_N_ELEMENTS(rejected); ++i) {
        RadioSAudioChannelInputSpec spec = input_spec(RADIOS_CHANNEL_INPUT_NETWORK, rejected[i], "Ağ yayını");
        g_assert_false(radios_audio_channel_input_spec_validate(&spec, &error));
        g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID);
        g_clear_error(&error);
    }
    RadioSAudioChannelInputSpec capture = input_spec(RADIOS_CHANNEL_INPUT_CAPTURE, "alsa_input.usb-Stüdyo", "Giriş 1");
    g_assert_true(radios_audio_channel_input_spec_validate(&capture, NULL));
    RadioSAudioChannelInputSpec empty = input_spec(RADIOS_CHANNEL_INPUT_CAPTURE, "", "Giriş 1");
    g_assert_false(radios_audio_channel_input_spec_validate(&empty, NULL));
    empty = input_spec(RADIOS_CHANNEL_INPUT_CAPTURE, "alsa_input.0", "");
    g_assert_false(radios_audio_channel_input_spec_validate(&empty, NULL));
    RadioSAudioChannelInputSpec invalid = input_spec(RADIOS_CHANNEL_INPUT_CAPTURE, "alsa_input.0", "a");
    invalid.name[0] = (gchar)0xff;
    g_assert_false(radios_audio_channel_input_spec_validate(&invalid, NULL));
    RadioSAudioChannelInputSpec none = input_spec(RADIOS_CHANNEL_INPUT_NONE, "alsa_input.0", "Giriş");
    g_assert_false(radios_audio_channel_input_spec_validate(&none, NULL));
    g_assert_false(radios_audio_channel_input_spec_validate(NULL, NULL));
    gchar longest[RADIOS_CHANNEL_INPUT_NAME_SIZE];
    memset(longest, 'a', sizeof longest - 1); longest[sizeof longest - 1] = '\0';
    RadioSAudioChannelInputSpec named = input_spec(RADIOS_CHANNEL_INPUT_CAPTURE, "alsa_input.0", longest);
    g_assert_cmpuint(strlen(named.name), ==, 128);
    g_assert_true(radios_audio_channel_input_spec_validate(&named, NULL));

    RadioSAudioChannelInputKind kind = RADIOS_CHANNEL_INPUT_NONE;
    g_assert_cmpstr(radios_audio_channel_input_kind_name(RADIOS_CHANNEL_INPUT_CAPTURE), ==, "capture");
    g_assert_cmpstr(radios_audio_channel_input_kind_name(RADIOS_CHANNEL_INPUT_NETWORK), ==, "network");
    g_assert_true(radios_audio_channel_input_kind_parse("capture", &kind));
    g_assert_cmpint(kind, ==, RADIOS_CHANNEL_INPUT_CAPTURE);
    g_assert_true(radios_audio_channel_input_kind_parse("network", &kind));
    g_assert_cmpint(kind, ==, RADIOS_CHANNEL_INPUT_NETWORK);
    g_assert_false(radios_audio_channel_input_kind_parse("none", &kind));
    g_assert_false(radios_audio_channel_input_kind_parse(NULL, &kind));

    RadioSAudioChannelInputSpec a = input_spec(RADIOS_CHANNEL_INPUT_NONE, "x", "y");
    RadioSAudioChannelInputSpec b = input_spec(RADIOS_CHANNEL_INPUT_NONE, "z", "w");
    g_assert_true(radios_audio_channel_input_spec_equal(&a, &b));
    RadioSAudioChannelInputSpec c = capture, d = capture;
    g_assert_true(radios_audio_channel_input_spec_equal(&c, &d));
    g_strlcpy(d.name, "Giriş 2", sizeof d.name);
    g_assert_false(radios_audio_channel_input_spec_equal(&c, &d));
    g_assert_false(radios_audio_channel_input_spec_equal(&c, NULL));

    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    g_assert_null(radios_audio_channel_input_new(engine, 32, &capture, NULL, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID);
    g_clear_error(&error);
    g_assert_null(radios_audio_channel_input_new(engine, 0, &none, NULL, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    radios_audio_engine_unref(engine);
}

/* A private loopback HTTP server: every accepted request receives the same
 * WAV body, or nothing at all when `silent` (a connect that never completes). */
typedef struct {
    GSocketListener *listener;
    GCancellable *cancel;
    GThread *thread;
    GBytes *body;
    guint16 port;
    gboolean silent;
    gint connections;
} HttpServer;

static gpointer http_serve(gpointer data)
{
    HttpServer *http = data;
    for (;;) {
        GSocketConnection *connection = g_socket_listener_accept(http->listener, NULL, http->cancel, NULL);
        if (!connection) break;
        g_atomic_int_inc(&http->connections);
        GInputStream *in = g_io_stream_get_input_stream(G_IO_STREAM(connection));
        GOutputStream *out = g_io_stream_get_output_stream(G_IO_STREAM(connection));
        gchar request[8192]; gsize length = 0;
        while (length < sizeof request - 1) {
            gssize got = g_input_stream_read(in, request + length, sizeof request - 1 - length, http->cancel, NULL);
            if (got <= 0) break;
            length += (gsize)got; request[length] = '\0';
            if (strstr(request, "\r\n\r\n")) break;
        }
        if (http->silent) {
            /* Hold the connection open without a response until teardown. */
            while (!g_cancellable_is_cancelled(http->cancel) &&
                   g_input_stream_read(in, request, sizeof request, http->cancel, NULL) > 0) {}
        } else {
            gsize size = 0; const guint8 *body = g_bytes_get_data(http->body, &size);
            gchar *header = g_strdup_printf("HTTP/1.0 200 OK\r\nContent-Type: audio/wav\r\nContent-Length: %" G_GSIZE_FORMAT
                                            "\r\nConnection: close\r\n\r\n", size);
            if (g_output_stream_write_all(out, header, strlen(header), NULL, http->cancel, NULL))
                (void)g_output_stream_write_all(out, body, size, NULL, http->cancel, NULL);
            g_free(header);
        }
        (void)g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
        g_object_unref(connection);
    }
    return NULL;
}

static void http_start(HttpServer *http, gboolean silent, guint frames)
{
    memset(http, 0, sizeof *http);
    http->listener = g_socket_listener_new(); http->cancel = g_cancellable_new(); http->silent = silent;
    http->body = sine_wav(frames);
    GInetAddress *loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    GSocketAddress *address = g_inet_socket_address_new(loopback, 0), *bound = NULL;
    GError *error = NULL;
    g_assert_true(g_socket_listener_add_address(http->listener, address, G_SOCKET_TYPE_STREAM,
                                                G_SOCKET_PROTOCOL_TCP, NULL, &bound, &error));
    g_assert_no_error(error);
    http->port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
    g_object_unref(bound); g_object_unref(address); g_object_unref(loopback);
    http->thread = g_thread_new("channel-input-http", http_serve, http);
}

static void http_stop(HttpServer *http)
{
    g_cancellable_cancel(http->cancel);
    g_socket_listener_close(http->listener);
    g_thread_join(http->thread);
    g_object_unref(http->listener); g_object_unref(http->cancel); g_bytes_unref(http->body);
}

static RadioSAudioEngine *line_engine(guint channel)
{
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioMixerConfig mixer; radios_audio_engine_get_mixer(engine, &mixer);
    mixer.channels[channel].source_deck = -1; /* a live input replaces the player */
    g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, NULL));
    return engine;
}

static RadioSAudioChannelInputStatus wait_status(RadioSAudioChannelInput *input, RadioSAudioChannelInputState state,
                                                 guint64 frames, guint attempts, gint64 timeout_us)
{
    const gint64 until = g_get_monotonic_time() + timeout_us;
    RadioSAudioChannelInputStatus status;
    for (;;) {
        radios_audio_channel_input_status(input, &status);
        if (status.state == state && status.frames >= frames && status.attempts >= attempts) return status;
        if (g_get_monotonic_time() > until)
            g_error("Channel input stayed in state %d (frames %" G_GUINT64_FORMAT ", attempts %u, error '%s')",
                    status.state, status.frames, status.attempts, status.error);
        g_usleep(2000);
    }
}

static void render(RadioSAudioEngine *engine, guint frames, gfloat output[3][8192])
{
    GError *error = NULL;
    g_assert_true(radios_audio_engine_render(engine, frames, output[0], output[1], output[2], &error));
    g_assert_no_error(error);
}

/* The decoded network PCM arrives sample-exact on MAIN through the strip:
 * left is the 997 Hz sine and right its negated half, at some offset. */
static guint64 assert_sine(const gfloat *stereo, guint frames)
{
    guint64 offset = G_MAXUINT64;
    for (guint64 k = 0; k < 48000 && offset == G_MAXUINT64; ++k) {
        gboolean match = TRUE;
        for (guint i = 0; i < 8 && match; ++i)
            match = fabsf(stereo[i * 2] - (gfloat)sample_at(k + i) / 32768.0f) < 0.00001f;
        if (match) offset = k;
    }
    g_assert_cmpuint(offset, !=, G_MAXUINT64);
    for (guint i = 0; i < frames; ++i) {
        const gint16 sample = sample_at(offset + i);
        g_assert_cmpfloat(fabsf(stereo[i * 2] - (gfloat)sample / 32768.0f), <, 0.00001f);
        g_assert_cmpfloat(fabsf(stereo[i * 2 + 1] - (gfloat)(-sample / 2) / 32768.0f), <, 0.00001f);
    }
    return offset;
}

static void test_network_pcm_reconnect_close(void)
{
    HttpServer http; http_start(&http, FALSE, 48000);
    RadioSAudioEngine *engine = line_engine(CHANNEL);
    gchar *url = g_strdup_printf("http://127.0.0.1:%u/kanal.wav", http.port);
    RadioSAudioChannelInputSpec spec = input_spec(RADIOS_CHANNEL_INPUT_NETWORK, url, "Yerel test yayını"), copy;
    GError *error = NULL;
    RadioSAudioChannelInput *input = radios_audio_channel_input_new(engine, CHANNEL, &spec, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(input);
    g_assert_cmpuint(radios_audio_channel_input_channel(input), ==, CHANNEL);
    radios_audio_channel_input_spec(input, &copy);
    g_assert_true(radios_audio_channel_input_spec_equal(&copy, &spec));
    RadioSAudioChannelInputStatus status = wait_status(input, RADIOS_CHANNEL_INPUT_READY, 8192, 1, 10 * G_USEC_PER_SEC);
    g_assert_cmpuint(status.attempts, ==, 1);
    g_assert_cmpfloat(status.peak[0], >, 0.2f);
    gfloat output[3][8192];
    render(engine, 2048, output);
    g_assert_cmpuint(assert_sine(output[0], 2048), ==, 0);
    /* Consume the rest of the one-second stream; its end is a loss, retried. */
    guint64 rendered = 2048;
    const gint64 until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    for (;;) {
        radios_audio_channel_input_status(input, &status);
        if (status.state == RADIOS_CHANNEL_INPUT_ERROR) break;
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        render(engine, 1024, output); rendered += 1024;
        g_usleep(2000);
    }
    g_assert_cmpuint(status.frames, ==, 48000);
    g_assert_cmpuint(status.attempts, ==, 1);
    g_assert_nonnull(strstr(status.error, "Ağ kaynağı"));
    g_assert_cmpuint(rendered, >=, 36000);
    /* Play out what the line still holds of the ended stream. */
    for (guint i = 0; i < 10; ++i) render(engine, 1024, output);
    /* RADIOS_CHANNEL_INPUT_RETRY_SECONDS later the stream is connected again. */
    const gint64 lost = g_get_monotonic_time();
    status = wait_status(input, RADIOS_CHANNEL_INPUT_READY, 48000 + 8192, 2, 12 * G_USEC_PER_SEC);
    g_assert_cmpint(g_get_monotonic_time() - lost, >=, (RADIOS_CHANNEL_INPUT_RETRY_SECONDS - 1) * G_USEC_PER_SEC);
    g_assert_cmpint(g_atomic_int_get(&http.connections), ==, 2);
    render(engine, 2048, output);
    g_assert_cmpuint(assert_sine(output[0], 2048), ==, 0);
    const gint64 before = g_get_monotonic_time();
    radios_audio_channel_input_free(input);
    g_assert_cmpint(g_get_monotonic_time() - before, <, 500000);
    /* The line is closed: the strip carries nothing any more. */
    render(engine, 2048, output); render(engine, 2048, output);
    for (guint i = 0; i < 4096; ++i) g_assert_cmpfloat(output[0][i], ==, 0.0f);
    radios_audio_engine_unref(engine);
    http_stop(&http);
    g_free(url);
}

static void test_close_while_connecting(void)
{
    HttpServer http; http_start(&http, TRUE, 4800);
    RadioSAudioEngine *engine = line_engine(CHANNEL);
    gchar *url = g_strdup_printf("http://127.0.0.1:%u/bekle.wav", http.port);
    RadioSAudioChannelInputSpec spec = input_spec(RADIOS_CHANNEL_INPUT_NETWORK, url, "Yanıtsız kaynak");
    RadioSAudioChannelInput *input = radios_audio_channel_input_new(engine, CHANNEL, &spec, NULL, NULL);
    g_assert_nonnull(input);
    const gint64 until = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
    while (g_atomic_int_get(&http.connections) < 1) { g_assert_cmpint(g_get_monotonic_time(), <, until); g_usleep(2000); }
    g_usleep(100000);
    RadioSAudioChannelInputStatus status; radios_audio_channel_input_status(input, &status);
    g_assert_cmpint(status.state, ==, RADIOS_CHANNEL_INPUT_CONNECTING);
    const gint64 before = g_get_monotonic_time();
    radios_audio_channel_input_free(input);
    g_assert_cmpint(g_get_monotonic_time() - before, <, 600000);
    radios_audio_engine_unref(engine);
    http_stop(&http);
    g_free(url);
}

/* Sound-card capture on the owned virtual devices of tests/run_audio_ui.py
 * --engine-only: a realtime engine plays a sine to the MONITOR sink, whose
 * monitor source is captured into channel 5 of an offline mixer. */
static void test_capture_pcm(void)
{
    const gchar *main = g_getenv("RADIOS_TEST_MAIN_SINK"), *source = g_getenv("RADIOS_TEST_MIC_SOURCE");
    if (!main || !source) { g_test_skip("Owned virtual Pulse devices require tests/run_audio_ui.py --engine-only"); return; }
    gchar *directory = g_dir_make_tmp("channel-input-XXXXXX", NULL); g_assert_nonnull(directory);
    gchar *path = g_build_filename(directory, "kaynak.wav", NULL);
    GBytes *wav = sine_wav(480000); gsize size = 0; const gchar *data = g_bytes_get_data(wav, &size);
    g_assert_true(g_file_set_contents(path, data, (gssize)size, NULL));
    gchar *sha = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, wav);
    gchar *media = g_uuid_string_random(), *version = g_uuid_string_random();
    RadioSAudioEngine *player = radios_audio_engine_new(TRUE, NULL); RadioSAudioPreferences prefs;
    radios_audio_preferences_default(&prefs);
    g_strlcpy(prefs.main_sink, main, sizeof prefs.main_sink);
    g_strlcpy(prefs.cue_sink, g_getenv("RADIOS_TEST_CUE_SINK"), sizeof prefs.cue_sink);
    g_strlcpy(prefs.monitor_sink, g_getenv("RADIOS_TEST_MONITOR_SINK"), sizeof prefs.monitor_sink);
    prefs.monitor_source = 1; prefs.main_gain_db = prefs.cue_gain_db = prefs.monitor_gain_db = 0;
    g_assert_true(radios_audio_engine_apply_preferences(player, &prefs, NULL));
    RadioSAudioSnapshot snapshot; gint64 until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    do {
        radios_audio_engine_snapshot(player, &snapshot);
        if (snapshot.buses[0].ready && snapshot.buses[1].ready && snapshot.buses[2].ready) break;
        g_usleep(10000);
    } while (g_get_monotonic_time() < until);
    g_assert_true(snapshot.buses[0].ready && snapshot.buses[1].ready && snapshot.buses[2].ready);
    RadioSAudioLoadSpec load = {path, sha, media, version, 0, 0}; GError *error = NULL;
    g_assert_true(radios_audio_engine_load(player, 1, &load, &error)); g_assert_no_error(error);
    until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    do { radios_audio_engine_snapshot(player, &snapshot); if (snapshot.decks[1].state == RADIOS_AUDIO_READY && snapshot.decks[1].buffered_frames >= 4800) break; g_usleep(5000); } while (g_get_monotonic_time() < until);
    /* Loading flushes the outputs; their streams report ready again shortly. */
    until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    do {
        radios_audio_engine_snapshot(player, &snapshot);
        if (snapshot.buses[0].ready && snapshot.buses[1].ready && snapshot.buses[2].ready) break;
        g_usleep(10000);
    } while (g_get_monotonic_time() < until);
    gboolean playing = radios_audio_engine_play(player, 1, &error); g_assert_no_error(error); g_assert_true(playing);

    RadioSAudioEngine *mixer = line_engine(CHANNEL);
    RadioSAudioChannelInputSpec spec = input_spec(RADIOS_CHANNEL_INPUT_CAPTURE, source, "Sanal giriş");
    RadioSAudioChannelInput *input = radios_audio_channel_input_new(mixer, CHANNEL, &spec, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(input);
    /* Nothing renders the mixer yet: the input paces itself at ~100 ms queued. */
    RadioSAudioChannelInputStatus status = wait_status(input, RADIOS_CHANNEL_INPUT_READY, 1440, 1, 10 * G_USEC_PER_SEC);
    /* Wait for the played sine itself, not the silence before playback. */
    until = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
    while (status.peak[0] < 0.2f) {
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        gfloat drain[3][8192]; render(mixer, 1024, drain); g_usleep(15000);
        radios_audio_channel_input_status(input, &status);
    }
    /* Play out the silence queued before the sine reached the capture. */
    gfloat output[3][8192];
    for (guint i = 0; i < 12; ++i) { render(mixer, 1024, output); g_usleep(15000); }
    render(mixer, 480, output);
    gdouble squares = 0; guint compared = 0;
    for (guint i = 0; i < 480; ++i) {
        squares += (gdouble)output[0][i * 2] * output[0][i * 2];
        if (fabsf(output[0][i * 2]) > 0.02f) {
            g_assert_cmpfloat(fabsf(output[0][i * 2 + 1] + output[0][i * 2] / 2), <, 0.002f);
            ++compared;
        }
    }
    g_assert_cmpuint(compared, >, 100);
    g_assert_cmpfloat(sqrt(squares / 480), >, 0.1);
    const gint64 before = g_get_monotonic_time();
    radios_audio_channel_input_free(input);
    g_assert_cmpint(g_get_monotonic_time() - before, <, 500000);
    radios_audio_engine_unref(mixer);
    radios_audio_engine_unref(player);
    g_assert_cmpint(g_unlink(path), ==, 0); g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_bytes_unref(wav); g_free(sha); g_free(media); g_free(version); g_free(path); g_free(directory);
}

/* --- R5.1 independent verification (radios-f1) findings ------------------ */

/* libavformat is linked through radios_audio, whose include path is private. */
const char *avio_enum_protocols(void **opaque, int output);
static gboolean ffmpeg_input_protocol(const gchar *name)
{
    void *opaque = NULL;
    for (const char *protocol; (protocol = avio_enum_protocols(&opaque, 0));) if (g_str_equal(protocol, name)) return TRUE;
    return FALSE;
}

/* srt:// on an FFmpeg without the srt protocol (the Freedesktop SDK's): the
 * address is refused with the reason when it is registered or assigned, and
 * an input is never started, so "Protocol not found" is not retried every
 * five seconds. The other schemes are unaffected. */
static void test_srt_needs_ffmpeg_protocol(void)
{
    RadioSAudioChannelInputSpec srt = input_spec(RADIOS_CHANNEL_INPUT_NETWORK, "srt://127.0.0.1:9000?mode=caller", "Saha bağlantısı");
    GError *error = NULL;
    if (ffmpeg_input_protocol("srt")) {
        g_test_message("This FFmpeg has the srt protocol: srt:// is accepted.");
        g_assert_true(radios_audio_channel_input_spec_validate(&srt, &error));
        g_assert_no_error(error);
    } else {
        g_assert_false(radios_audio_channel_input_spec_validate(&srt, &error));
        g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID);
        g_assert_nonnull(strstr(error->message, "Bu sistemin FFmpeg'inde SRT desteği yok"));
        g_clear_error(&error);
        RadioSAudioEngine *engine = line_engine(CHANNEL);
        g_assert_null(radios_audio_channel_input_new(engine, CHANNEL, &srt, NULL, &error));
        g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID);
        g_assert_nonnull(strstr(error->message, "SRT desteği yok"));
        g_clear_error(&error);
        radios_audio_engine_unref(engine);
        /* A wrong address lists only the schemes this build can open. */
        RadioSAudioChannelInputSpec other = input_spec(RADIOS_CHANNEL_INPUT_NETWORK, "ftp://x.example/y", "Ağ yayını");
        g_assert_false(radios_audio_channel_input_spec_validate(&other, &error));
        g_assert_nonnull(strstr(error->message, "http, https"));
        g_assert_null(strstr(error->message, "srt"));
        g_clear_error(&error);
    }
    static const gchar *const kept[] = {
        "http://radyo.example/yayin", "https://akis.example/canli.aac", "rtp://239.0.0.1:5004", "udp://@:1234",
        "rtsp://kamera.local/ses", "rtsp://kamera.local/ses?tcp"
    };
    for (guint i = 0; i < G_N_ELEMENTS(kept); ++i) {
        RadioSAudioChannelInputSpec spec = input_spec(RADIOS_CHANNEL_INPUT_NETWORK, kept[i], "Ağ yayını");
        g_assert_true(radios_audio_channel_input_spec_validate(&spec, &error));
        g_assert_no_error(error);
    }
}

/* A live loopback sender: `first_us` of a 997 Hz sine, then `pause_us` with
 * the connection kept open and nothing sent, then the sine again until
 * stopped. RTP sends payload type 10 (L16, 44.1 kHz stereo) to a UDP port
 * from its own socket; HTTP answers the first request with an endless WAV
 * that starts with HTTP_HEAD_FRAMES at once: FFmpeg reads 512 KB of a live
 * WAV before it reports the stream, so the input then plays that far behind
 * the sender. */
#define HTTP_HEAD_FRAMES 150000u
typedef struct {
    gboolean http;
    GSocket *socket; /* HTTP: the listening socket; RTP: the sending socket */
    GSocketAddress *target; /* RTP destination */
    guint16 port;
    gint64 first_us, pause_us;
    gint stop, paused, resumed;
    GThread *thread;
} LiveSender;

static gboolean live_send_all(GSocket *socket, const guint8 *data, gsize size)
{
    while (size) {
        gssize sent = g_socket_send(socket, (const gchar *)data, size, NULL, NULL);
        if (sent <= 0) return FALSE;
        data += sent; size -= (gsize)sent;
    }
    return TRUE;
}

static gpointer live_send(gpointer data)
{
    LiveSender *sender = data; GSocket *peer = NULL;
    const guint rate = sender->http ? 48000 : 44100, chunk = rate / 100;
    if (sender->http) {
        while (!peer && !g_atomic_int_get(&sender->stop))
            if (g_socket_condition_timed_wait(sender->socket, G_IO_IN, 100000, NULL, NULL)) peer = g_socket_accept(sender->socket, NULL, NULL);
        if (!peer) return NULL;
        gchar request[8192]; gsize length = 0;
        while (length < sizeof request - 1 && !g_atomic_int_get(&sender->stop)) {
            if (!g_socket_condition_timed_wait(peer, G_IO_IN, 100000, NULL, NULL)) continue;
            gssize got = g_socket_receive(peer, request + length, sizeof request - 1 - length, NULL, NULL);
            if (got <= 0) break;
            length += (gsize)got; request[length] = '\0';
            if (strstr(request, "\r\n\r\n")) break;
        }
        GByteArray *head = g_byte_array_new();
        const gchar *status = "HTTP/1.0 200 OK\r\nContent-Type: audio/wav\r\n\r\n";
        g_byte_array_append(head, (const guint8 *)status, (guint)strlen(status));
        g_byte_array_append(head, (const guint8 *)"RIFF", 4); append_le(head, 0xffffffffu, 4);
        g_byte_array_append(head, (const guint8 *)"WAVEfmt ", 8);
        append_le(head, 16, 4); append_le(head, 1, 2); append_le(head, 2, 2); append_le(head, 48000, 4);
        append_le(head, 192000, 4); append_le(head, 4, 2); append_le(head, 16, 2);
        g_byte_array_append(head, (const guint8 *)"data", 4); append_le(head, 0xffffffffu - 36, 4);
        gboolean sent = live_send_all(peer, head->data, head->len);
        g_byte_array_unref(head);
        if (!sent) { g_object_unref(peer); return NULL; }
    }
    const gint64 start = g_get_monotonic_time();
    guint64 frames = 0; guint16 sequence = 0;
    const guint64 head = sender->http ? HTTP_HEAD_FRAMES : 0;
    guint8 packet[12 + 480 * 4]; /* 10 ms: 441 RTP or 480 WAV frames */
    while (!g_atomic_int_get(&sender->stop)) {
        const gint64 elapsed = g_get_monotonic_time() - start;
        if (elapsed >= sender->first_us && elapsed < sender->first_us + sender->pause_us) {
            g_atomic_int_set(&sender->paused, 1); g_usleep(2000); continue;
        }
        const gboolean after = elapsed >= sender->first_us + sender->pause_us;
        if (after) g_atomic_int_set(&sender->resumed, 1);
        const guint64 due = head + (guint64)((after ? elapsed - sender->pause_us : elapsed) * (gint64)rate / G_USEC_PER_SEC);
        if (frames > due) { g_usleep(2000); continue; }
        gsize size = 0;
        if (!sender->http) {
            const guint32 timestamp = (guint32)frames, ssrc = 0x52310001u;
            packet[0] = 0x80; packet[1] = 10; packet[2] = sequence >> 8; packet[3] = sequence & 0xff;
            packet[4] = timestamp >> 24; packet[5] = timestamp >> 16; packet[6] = timestamp >> 8; packet[7] = timestamp;
            packet[8] = ssrc >> 24; packet[9] = ssrc >> 16; packet[10] = ssrc >> 8; packet[11] = ssrc;
            size = 12; ++sequence;
        }
        for (guint i = 0; i < chunk; ++i) {
            const gint16 left = (gint16)lrint(8192.0 * sin(2.0 * G_PI * 997.0 * (double)(frames + i) / rate)), right = (gint16)(-left / 2);
            const guint16 l = (guint16)left, r = (guint16)right;
            if (sender->http) { packet[size++] = l & 0xff; packet[size++] = l >> 8; packet[size++] = r & 0xff; packet[size++] = r >> 8; }
            else { packet[size++] = l >> 8; packet[size++] = l & 0xff; packet[size++] = r >> 8; packet[size++] = r & 0xff; }
        }
        frames += chunk;
        if (sender->http) { if (!live_send_all(peer, packet, size)) break; }
        else (void)g_socket_send_to(sender->socket, sender->target, (const gchar *)packet, size, NULL, NULL);
    }
    if (peer) { (void)g_socket_close(peer, NULL); g_object_unref(peer); }
    return NULL;
}

/* An even UDP port whose odd neighbour (RTCP) is free too. */
static guint16 rtp_port_pair(void)
{
    for (guint attempt = 0; attempt < 200; ++attempt) {
        GSocket *rtp = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
        GInetAddress *loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
        GSocketAddress *any = g_inet_socket_address_new(loopback, 0);
        guint16 port = 0;
        if (g_socket_bind(rtp, any, FALSE, NULL)) {
            GSocketAddress *bound = g_socket_get_local_address(rtp, NULL);
            port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound)); g_object_unref(bound);
        }
        gboolean usable = port && port % 2 == 0 && port < 65534 && port != 8000 && port != 8008;
        if (usable) {
            GSocket *rtcp = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, NULL);
            GSocketAddress *next = g_inet_socket_address_new(loopback, port + 1);
            usable = g_socket_bind(rtcp, next, FALSE, NULL);
            g_object_unref(next); g_socket_close(rtcp, NULL); g_object_unref(rtcp);
        }
        g_object_unref(any); g_object_unref(loopback); g_socket_close(rtp, NULL); g_object_unref(rtp);
        if (usable) return port;
    }
    g_error("No free loopback RTP port pair");
}

static void live_start(LiveSender *sender, gboolean http, gint64 first_us, gint64 pause_us)
{
    memset(sender, 0, sizeof *sender);
    sender->http = http; sender->first_us = first_us; sender->pause_us = pause_us;
    GInetAddress *loopback = g_inet_address_new_loopback(G_SOCKET_FAMILY_IPV4);
    GError *error = NULL;
    if (http) {
        sender->socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_STREAM, G_SOCKET_PROTOCOL_TCP, &error);
        g_assert_no_error(error);
        GSocketAddress *any = g_inet_socket_address_new(loopback, 0);
        g_assert_true(g_socket_bind(sender->socket, any, TRUE, &error)); g_assert_no_error(error);
        g_assert_true(g_socket_listen(sender->socket, &error)); g_assert_no_error(error);
        GSocketAddress *bound = g_socket_get_local_address(sender->socket, NULL);
        sender->port = g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(bound));
        g_object_unref(bound); g_object_unref(any);
    } else {
        sender->port = rtp_port_pair();
        sender->socket = g_socket_new(G_SOCKET_FAMILY_IPV4, G_SOCKET_TYPE_DATAGRAM, G_SOCKET_PROTOCOL_UDP, &error);
        g_assert_no_error(error);
        sender->target = g_inet_socket_address_new(loopback, sender->port);
    }
    g_object_unref(loopback);
    sender->thread = g_thread_new("channel-input-live", live_send, sender);
}

static void live_stop(LiveSender *sender)
{
    g_atomic_int_set(&sender->stop, 1);
    g_thread_join(sender->thread);
    g_socket_close(sender->socket, NULL); g_object_unref(sender->socket);
    g_clear_object(&sender->target);
}

/* A sender that stops while its connection stays open, over RTP/UDP (whose
 * reads never time out in FFmpeg) and HTTP (inside its 10 s read timeout):
 * once the input has played what it holds, its level drops at once, ~5 s
 * later it reports "Ağ kaynağından veri gelmiyor." instead of "Hazır", and
 * when the sender comes back the same connection is heard again, with no
 * reconnect. Both are rendered in real time, 960 frames every 20 ms. */
static void test_no_data_reported_then_recovered(void)
{
    const guint channels[2] = {CHANNEL, CHANNEL + 1};
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioMixerConfig mixer; radios_audio_engine_get_mixer(engine, &mixer);
    for (guint i = 0; i < 2; ++i) mixer.channels[channels[i]].source_deck = -1;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, NULL));
    LiveSender senders[2];
    /* HTTP plays ~3.1 s behind its sender; its pause still ends well inside
     * the 10 s read timeout that starts when that head is used up. */
    live_start(&senders[0], FALSE, 2500000, 6500000);
    live_start(&senders[1], TRUE, 2500000, 10000000);
    RadioSAudioChannelInput *inputs[2];
    for (guint i = 0; i < 2; ++i) {
        gchar *url = senders[i].http ? g_strdup_printf("http://127.0.0.1:%u/canli.wav", senders[i].port)
                                     : g_strdup_printf("rtp://127.0.0.1:%u?localaddr=127.0.0.1", senders[i].port);
        RadioSAudioChannelInputSpec spec = input_spec(RADIOS_CHANNEL_INPUT_NETWORK, url, senders[i].http ? "Yerel HTTP" : "Yerel RTP");
        GError *error = NULL;
        inputs[i] = radios_audio_channel_input_new(engine, channels[i], &spec, NULL, &error);
        g_assert_no_error(error); g_assert_nonnull(inputs[i]);
        g_free(url);
    }
    enum { PLAYING, PAUSED, SILENT, BACK } phase[2] = {PLAYING, PLAYING};
    gint64 grown_at[2] = {0, 0}, silent_at[2] = {0, 0};
    guint64 frames[2] = {0, 0};
    gboolean level_dropped[2] = {FALSE, FALSE};
    gfloat output[3][8192];
    const gint64 start = g_get_monotonic_time(); gint64 next = start;
    while (phase[0] != BACK || phase[1] != BACK) {
        next += 20000;
        const gint64 now = g_get_monotonic_time();
        if (next > now) g_usleep((gulong)(next - now));
        render(engine, 960, output);
        g_assert_cmpint(g_get_monotonic_time() - start, <, 25 * G_USEC_PER_SEC);
        for (guint i = 0; i < 2; ++i) {
            RadioSAudioChannelInputStatus status; radios_audio_channel_input_status(inputs[i], &status);
            const gint64 at = g_get_monotonic_time();
            const gchar *name = senders[i].http ? "HTTP" : "RTP";
            g_assert_cmpuint(status.attempts, <=, 1); /* never reconnected */
            if (status.frames != frames[i]) { frames[i] = status.frames; grown_at[i] = at; }
            if (phase[i] == PLAYING) {
                if (!g_atomic_int_get(&senders[i].paused)) continue;
                if (status.state != RADIOS_CHANNEL_INPUT_READY)
                    g_test_message("%s: state %d, frames %" G_GUINT64_FORMAT ", error '%s' when the sender paused", name, status.state, status.frames, status.error);
                g_assert_cmpint(status.state, ==, RADIOS_CHANNEL_INPUT_READY);
                g_assert_cmpuint(status.attempts, ==, 1);
                g_assert_cmpuint(status.frames, >, 24000);
                phase[i] = PAUSED;
            } else if (phase[i] == PAUSED) {
                /* Measured from the last delivery: what was buffered still plays. */
                const gint64 since = at - grown_at[i];
                if (since > 1200000) {
                    /* The last block's level is not shown for a silent source. */
                    g_assert_cmpfloat(status.peak[0], ==, 0.0f); g_assert_cmpfloat(status.peak[1], ==, 0.0f);
                    level_dropped[i] = TRUE;
                }
                if (status.state == RADIOS_CHANNEL_INPUT_ERROR) {
                    g_test_message("%s: no data reported %.2f s after the last audio", name, since / 1e6);
                    g_assert_cmpstr(status.error, ==, "Ağ kaynağından veri gelmiyor.");
                    g_assert_cmpint(since, >, 4 * G_USEC_PER_SEC);
                    g_assert_cmpint(since, <, 6500000);
                    g_assert_true(level_dropped[i]);
                    g_assert_false(g_atomic_int_get(&senders[i].resumed));
                    phase[i] = SILENT; silent_at[i] = at;
                } else {
                    g_assert_cmpint(status.state, ==, RADIOS_CHANNEL_INPUT_READY);
                    g_assert_cmpint(since, <, 6500000);
                }
            } else if (phase[i] == SILENT) {
                if (status.state == RADIOS_CHANNEL_INPUT_READY) {
                    g_assert_true(g_atomic_int_get(&senders[i].resumed));
                    g_assert_cmpstr(status.error, ==, "");
                    phase[i] = BACK;
                    g_test_message("%s: audio again %.2f s after no data was reported", name, (at - silent_at[i]) / 1e6);
                } else {
                    g_assert_cmpint(status.state, ==, RADIOS_CHANNEL_INPUT_ERROR);
                    g_assert_cmpstr(status.error, ==, "Ağ kaynağından veri gelmiyor.");
                    g_assert_cmpfloat(status.peak[0], ==, 0.0f);
                    g_assert_cmpint(at - silent_at[i], <, 10 * G_USEC_PER_SEC);
                }
            }
        }
    }
    /* Back on the kept connection: the level returns with the audio. */
    const gint64 until = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
    gboolean heard[2] = {FALSE, FALSE};
    while (!heard[0] || !heard[1]) {
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        g_usleep(20000); render(engine, 960, output);
        for (guint i = 0; i < 2; ++i) {
            RadioSAudioChannelInputStatus status; radios_audio_channel_input_status(inputs[i], &status);
            g_assert_cmpint(status.state, ==, RADIOS_CHANNEL_INPUT_READY);
            g_assert_cmpuint(status.attempts, ==, 1);
            if (status.peak[0] > 0.2f) heard[i] = TRUE;
        }
    }
    for (guint i = 0; i < 2; ++i) {
        const gint64 before = g_get_monotonic_time();
        radios_audio_channel_input_free(inputs[i]);
        g_assert_cmpint(g_get_monotonic_time() - before, <, 500000);
        live_stop(&senders[i]);
    }
    radios_audio_engine_unref(engine);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/channel-input/spec-rules", test_spec_rules);
    g_test_add_func("/channel-input/network-PCM-reconnect-close", test_network_pcm_reconnect_close);
    g_test_add_func("/channel-input/close-while-connecting", test_close_while_connecting);
    g_test_add_func("/channel-input/capture-PCM", test_capture_pcm);
    g_test_add_func("/channel-input/srt-needs-ffmpeg-protocol", test_srt_needs_ffmpeg_protocol);
    g_test_add_func("/channel-input/no-data-reported-then-recovered", test_no_data_reported_then_recovered);
    return g_test_run();
}
