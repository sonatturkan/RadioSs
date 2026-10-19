#include "radios/audio.h"

#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#define TAU 6.2831853071795864769

typedef struct {
    gchar *directory, *path, *sha, *media, *version;
    RadioSAudioEngine *engine;
} Fixture;

static void append16(GByteArray *bytes, guint16 value)
{
    guint8 data[] = {(guint8)value, (guint8)(value >> 8)};
    g_byte_array_append(bytes, data, sizeof data);
}
static void append32(GByteArray *bytes, guint32 value)
{
    guint8 data[] = {(guint8)value, (guint8)(value >> 8), (guint8)(value >> 16), (guint8)(value >> 24)};
    g_byte_array_append(bytes, data, sizeof data);
}
static void wave(Fixture *fixture, guint frames)
{
    GByteArray *bytes = g_byte_array_new();
    g_byte_array_append(bytes, (const guint8 *)"RIFF", 4); append32(bytes, frames * 4 + 36);
    g_byte_array_append(bytes, (const guint8 *)"WAVEfmt ", 8); append32(bytes, 16); append16(bytes, 1); append16(bytes, 2);
    append32(bytes, 48000); append32(bytes, 192000); append16(bytes, 4); append16(bytes, 16);
    g_byte_array_append(bytes, (const guint8 *)"data", 4); append32(bytes, frames * 4);
    for (guint i = 0; i < frames; ++i) {
        append16(bytes, (guint16)(gint16)lround(8192 * sin(TAU * 1000 * i / 48000)));
        append16(bytes, (guint16)(gint16)lround(4096 * sin(TAU * 1500 * i / 48000)));
    }
    g_assert_true(g_file_set_contents(fixture->path, (const gchar *)bytes->data, bytes->len, NULL));
    g_free(fixture->sha); fixture->sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes->data, bytes->len);
    g_byte_array_unref(bytes);
}
static void wait_ready(Fixture *fixture, guint rate)
{
    RadioSAudioSnapshot snapshot;
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    do {
        radios_audio_engine_snapshot(fixture->engine, &snapshot);
        if (snapshot.buses[0].ready && snapshot.microphone_ready && snapshot.buses[0].output_sample_rate == rate) {
            g_assert_cmpuint(snapshot.buses[0].output_buffer_frames, >, 0);
            return;
        }
        g_usleep(2000);
    } while (g_get_monotonic_time() < deadline);
    g_error("Actual output unavailable: ready %d rate %u error %s / mic %s", snapshot.buses[0].ready,
        snapshot.buses[0].output_sample_rate, snapshot.buses[0].error, snapshot.microphone_error);
}
static void setup(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GError *error = NULL;
    fixture->directory = g_dir_make_tmp("radios-prefs-output-XXXXXX", &error);
    g_assert_no_error(error);
    fixture->path = g_build_filename(fixture->directory, "source-48k.wav", NULL);
    fixture->media = g_uuid_string_random(); fixture->version = g_uuid_string_random();
    fixture->engine = radios_audio_engine_new(TRUE, NULL);
    RadioSAudioPreferences preferences;
    radios_audio_preferences_default(&preferences);
    const gchar *sink = g_getenv("RADIOS_TEST_OUTPUT_SINK");
    g_assert_nonnull(sink);
    g_strlcpy(preferences.main_sink, sink, sizeof preferences.main_sink);
    gchar *source = g_strconcat(sink, ".monitor", NULL);
    g_strlcpy(preferences.microphone_source, source, sizeof preferences.microphone_source);
    preferences.microphone_gain_db = -60;
    g_free(source);
    g_assert_true(radios_audio_engine_apply_preferences(fixture->engine, &preferences, &error));
    g_assert_no_error(error);
    g_assert_true(radios_audio_engine_configure_output(fixture->engine, 44100, 256, &error));
    g_assert_no_error(error); wait_ready(fixture, 44100);
}
static void teardown(Fixture *fixture, gconstpointer data)
{
    (void)data;
    radios_audio_engine_unref(fixture->engine);
    if (g_file_test(fixture->path, G_FILE_TEST_EXISTS)) g_assert_cmpint(g_remove(fixture->path), ==, 0);
    g_assert_cmpint(g_rmdir(fixture->directory), ==, 0);
    g_free(fixture->directory); g_free(fixture->path); g_free(fixture->sha);
    g_free(fixture->media); g_free(fixture->version);
}
static void load_and_play(Fixture *fixture, guint frames)
{
    wave(fixture, frames);
    RadioSAudioLoadSpec spec = {fixture->path, fixture->sha, fixture->media, fixture->version, 0, frames};
    g_assert_true(radios_audio_engine_load(fixture->engine, 0, &spec, NULL));
    RadioSAudioSnapshot snapshot;
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    do {
        radios_audio_engine_snapshot(fixture->engine, &snapshot);
        if (snapshot.decks[0].state == RADIOS_AUDIO_READY && snapshot.buses[0].ready &&
            snapshot.decks[0].buffered_frames >= MIN(frames, 4096u)) break;
        g_assert_cmpint(snapshot.decks[0].state, !=, RADIOS_AUDIO_ERROR); g_usleep(2000);
    } while (g_get_monotonic_time() < deadline);
    g_assert_cmpint(snapshot.decks[0].state, ==, RADIOS_AUDIO_READY);
    g_assert_true(snapshot.buses[0].ready);
    GError *error = NULL;
    gboolean accepted = radios_audio_engine_play(fixture->engine, 0, &error);
    g_assert_no_error(error); g_assert_true(accepted);
}
static guint64 capture_frames(void)
{
    GStatBuf info;
    g_assert_cmpint(g_stat(g_getenv("RADIOS_TEST_OUTPUT_CAPTURE"), &info), ==, 0);
    return (guint64)info.st_size / (2 * sizeof(gfloat));
}
static void tone_timing(Fixture *fixture, gconstpointer data)
{
    (void)data;
    g_usleep(200000);
    guint64 first_capture = capture_frames();
    load_and_play(fixture, 48000);
    guint64 previous = 0;
    gint64 started = g_get_monotonic_time(), deadline = started + 5 * G_TIME_SPAN_SECOND;
    RadioSAudioSnapshot snapshot;
    do {
        radios_audio_engine_snapshot(fixture->engine, &snapshot);
        g_assert_cmpuint(snapshot.buses[0].output_sample_rate, ==, 44100);
        g_assert_cmpuint(snapshot.decks[0].cursor_frames, >=, previous);
        g_assert_cmpuint(snapshot.decks[0].cursor_frames, <=, 48000);
        previous = snapshot.decks[0].cursor_frames;
        if (snapshot.decks[0].state == RADIOS_AUDIO_EOF) break;
        g_assert_cmpint(snapshot.decks[0].state, !=, RADIOS_AUDIO_ERROR);
        g_usleep(2000);
    } while (g_get_monotonic_time() < deadline);
    g_assert_cmpint(snapshot.decks[0].state, ==, RADIOS_AUDIO_EOF);
    g_assert_cmpuint(snapshot.decks[0].cursor_frames, ==, 48000);
    gdouble seconds = (g_get_monotonic_time() - started) / 1000000.0;
    g_assert_cmpfloat(seconds, >, .90); g_assert_cmpfloat(seconds, <, 2.5);
    g_usleep(250000);
    gchar *proof = g_strdup_printf("{\"first_frame\":%" G_GUINT64_FORMAT ",\"last_frame\":%" G_GUINT64_FORMAT
        ",\"actual_sample_rate\":%u,\"actual_buffer_frames\":%u,\"source_cursor_frames\":%" G_GUINT64_FORMAT
        ",\"eof_seconds\":%.6f}\n", first_capture, capture_frames(), snapshot.buses[0].output_sample_rate,
        snapshot.buses[0].output_buffer_frames, snapshot.decks[0].cursor_frames, seconds);
    g_assert_true(g_file_set_contents(g_getenv("RADIOS_TEST_OUTPUT_PROOF"), proof, -1, NULL));
    g_free(proof);
}
static void change_fence(Fixture *fixture, gconstpointer data)
{
    (void)data;
    load_and_play(fixture, 96000); g_usleep(150000);
    RadioSAudioSnapshot before, after;
    radios_audio_engine_snapshot(fixture->engine, &before);
    g_assert_cmpint(before.decks[0].state, ==, RADIOS_AUDIO_PLAYING);
    g_assert_cmpuint(before.decks[0].cursor_frames, >, 0);
    g_assert_true(radios_audio_engine_set_microphone_gate(fixture->engine, TRUE, NULL));
    g_assert_true(radios_audio_engine_configure_output(fixture->engine, 44100, 256, NULL));
    radios_audio_engine_snapshot(fixture->engine, &after);
    g_assert_cmpuint(after.loss_epoch, ==, before.loss_epoch);
    g_assert_true(after.microphone_gate);
    g_assert_cmpint(after.decks[0].state, ==, RADIOS_AUDIO_PLAYING);
    GError *error = NULL;
    g_assert_false(radios_audio_engine_configure_output(fixture->engine, 96000, 256, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID); g_clear_error(&error);
    g_assert_false(radios_audio_engine_configure_output(fixture->engine, 44100, 255, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID); g_clear_error(&error);
    g_assert_true(radios_audio_engine_configure_output(fixture->engine, 48000, 512, &error));
    g_assert_no_error(error); radios_audio_engine_snapshot(fixture->engine, &after);
    g_assert_cmpuint(after.loss_epoch, ==, before.loss_epoch + 1);
    g_assert_false(after.microphone_gate);
    g_assert_cmpint(after.decks[0].state, ==, RADIOS_AUDIO_PAUSED);
    wait_ready(fixture, 48000);
    radios_audio_engine_snapshot(fixture->engine, &after);
    guint64 paused = after.decks[0].cursor_frames;
    g_usleep(200000); radios_audio_engine_snapshot(fixture->engine, &after);
    g_assert_cmpint(after.decks[0].state, ==, RADIOS_AUDIO_PAUSED);
    g_assert_cmpuint(after.decks[0].cursor_frames, ==, paused);
    guint rate = 0, frames = 0;
    radios_audio_engine_output_configuration(fixture->engine, &rate, &frames);
    g_assert_cmpuint(rate, ==, 48000); g_assert_cmpuint(frames, ==, 512);
    g_assert_true(radios_audio_engine_play(fixture->engine, 0, NULL));
    g_usleep(150000); radios_audio_engine_snapshot(fixture->engine, &after);
    g_assert_cmpint(after.decks[0].state, ==, RADIOS_AUDIO_PLAYING);
    g_assert_cmpuint(after.decks[0].cursor_frames, >, paused);
    g_assert_false(after.microphone_gate);
}

static void wait_buffer_change(Fixture *fixture, guint previous, gboolean larger)
{
    RadioSAudioSnapshot snapshot;
    gint64 deadline = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
    do {
        radios_audio_engine_snapshot(fixture->engine, &snapshot);
        guint actual = snapshot.buses[0].output_buffer_frames;
        if (snapshot.buses[0].ready && snapshot.microphone_ready &&
            snapshot.buses[0].output_sample_rate == 44100 && actual > 0 &&
            (larger ? actual > previous : actual < previous)) return;
        g_usleep(2000);
    } while (g_get_monotonic_time() < deadline);
    g_error("Actual output buffer did not %s from %u: ready %d rate %u buffer %u error %s",
        larger ? "grow" : "shrink", previous, snapshot.buses[0].ready,
        snapshot.buses[0].output_sample_rate, snapshot.buses[0].output_buffer_frames,
        snapshot.buses[0].error);
}

static void buffer_target(Fixture *fixture, gconstpointer data)
{
    (void)data;
    RadioSAudioSnapshot snapshot;
    radios_audio_engine_snapshot(fixture->engine, &snapshot);
    guint first_buffer = snapshot.buses[0].output_buffer_frames;
    g_assert_true(radios_audio_engine_configure_output(fixture->engine, 44100, 4096, NULL));
    wait_buffer_change(fixture, first_buffer, TRUE);
    radios_audio_engine_snapshot(fixture->engine, &snapshot);
    /* PipeWire/Pulse can round small requests up to a device quantum, but the
     * large target must really change the stream's accepted buffer size. */
    g_assert_cmpuint(snapshot.buses[0].output_buffer_frames, >, first_buffer);
    guint rate = 0, frames = 0;
    radios_audio_engine_output_configuration(fixture->engine, &rate, &frames);
    g_assert_cmpuint(rate, ==, 44100); g_assert_cmpuint(frames, ==, 4096);
    g_assert_false(snapshot.microphone_gate);
    guint large_buffer = snapshot.buses[0].output_buffer_frames;
    g_assert_true(radios_audio_engine_configure_output(fixture->engine, 44100, 64, NULL));
    wait_buffer_change(fixture, large_buffer, FALSE);
    radios_audio_engine_output_configuration(fixture->engine, &rate, &frames);
    g_assert_cmpuint(frames, ==, 64);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    if (!g_getenv("RADIOS_TEST_OUTPUT_SINK") || !g_getenv("RADIOS_TEST_OUTPUT_CAPTURE") || !g_getenv("RADIOS_TEST_OUTPUT_PROOF"))
        return 77;
    g_test_add("/preferences-output/44100-actual-pcm-timing", Fixture, NULL, setup, tone_timing, teardown);
    g_test_add("/preferences-output/format-fence-idempotence", Fixture, NULL, setup, change_fence, teardown);
    g_test_add("/preferences-output/buffer-target", Fixture, NULL, setup, buffer_target, teardown);
    return g_test_run();
}
