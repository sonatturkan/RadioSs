#include "radios/audio.h"
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#ifndef RADIOS_TEST_PROJECT_ROOT
#define RADIOS_TEST_PROJECT_ROOT "."
#endif
typedef struct { gchar *directory, *path, *sha, *media, *version; } Fixture;
static void append16(GByteArray *bytes, guint16 value)
{ guint8 data[] = {(guint8)value, (guint8)(value >> 8)}; g_byte_array_append(bytes, data, 2); }
static void append32(GByteArray *bytes, guint32 value)
{ guint8 data[] = {(guint8)value, (guint8)(value >> 8), (guint8)(value >> 16), (guint8)(value >> 24)}; g_byte_array_append(bytes, data, 4); }
static void wave(Fixture *fixture, guint frames)
{
    GByteArray *bytes = g_byte_array_new();
    g_byte_array_append(bytes, (guint8 *)"RIFF", 4); append32(bytes, frames * 4 + 36);
    g_byte_array_append(bytes, (guint8 *)"WAVEfmt ", 8); append32(bytes, 16); append16(bytes, 1); append16(bytes, 2);
    append32(bytes, 48000); append32(bytes, 192000); append16(bytes, 4); append16(bytes, 16);
    g_byte_array_append(bytes, (guint8 *)"data", 4); append32(bytes, frames * 4);
    for (guint i = 0; i < frames; ++i) { append16(bytes, 8192); append16(bytes, (guint16)-4096); }
    g_assert_true(g_file_set_contents(fixture->path, (gchar *)bytes->data, bytes->len, NULL));
    g_free(fixture->sha); fixture->sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes->data, bytes->len);
    g_byte_array_unref(bytes);
}
static void setup(Fixture *fixture, gconstpointer unused)
{
    (void)unused; gchar *root = g_canonicalize_filename(RADIOS_TEST_PROJECT_ROOT, NULL);
    gchar *build = g_build_filename(root, "build", NULL); g_assert_cmpint(g_mkdir_with_parents(build, 0700), ==, 0);
    fixture->directory = g_build_filename(build, "audio-engine-XXXXXX", NULL); g_assert_nonnull(g_mkdtemp(fixture->directory));
    fixture->path = g_build_filename(fixture->directory, "Measured.wav", NULL);
    fixture->media = g_uuid_string_random(); fixture->version = g_uuid_string_random(); wave(fixture, 12000);
    g_free(root); g_free(build);
}
static void teardown(Fixture *fixture, gconstpointer unused)
{
    (void)unused; g_assert_cmpint(g_unlink(fixture->path), ==, 0); g_assert_cmpint(g_rmdir(fixture->directory), ==, 0);
    g_free(fixture->directory); g_free(fixture->path); g_free(fixture->sha); g_free(fixture->media); g_free(fixture->version);
}
static void load(RadioSAudioEngine *engine, guint deck, Fixture *fixture, guint64 in, guint64 out)
{
    RadioSAudioLoadSpec spec = {fixture->path, fixture->sha, fixture->media, fixture->version, in, out};
    GError *error = NULL; g_assert_true(radios_audio_engine_load(engine, deck, &spec, &error)); g_assert_no_error(error);
}
static void ready(RadioSAudioEngine *engine, guint deck, guint buffered)
{
    RadioSAudioSnapshot snapshot; gint64 until = g_get_monotonic_time() + 10000000;
    do {
        radios_audio_engine_snapshot(engine, &snapshot);
        if (snapshot.decks[deck].state == RADIOS_AUDIO_ERROR) g_error("Decoder: %s", snapshot.decks[deck].error);
        if (snapshot.decks[deck].state != RADIOS_AUDIO_LOADING && snapshot.decks[deck].buffered_frames >= buffered) return;
        g_usleep(1000);
    } while (g_get_monotonic_time() < until);
    g_error("Audio deck %u did not become ready (state %d, buffered %" G_GUINT64_FORMAT ")", deck,
        snapshot.decks[deck].state, snapshot.decks[deck].buffered_frames);
}
static void play(RadioSAudioEngine *engine, guint deck)
{ GError *error = NULL; g_assert_true(radios_audio_engine_play(engine, deck, &error)); g_assert_no_error(error); }
static void render(RadioSAudioEngine *engine, guint frames, gfloat output[3][8192])
{ GError *error = NULL; g_assert_true(radios_audio_engine_render(engine, frames, output[0], output[1], output[2], &error)); g_assert_no_error(error); }
static void close_to(gfloat actual, gfloat expected)
{ g_assert_cmpfloat(fabsf(actual - expected), <, 0.00001f); }
static void mix(Fixture *fixture, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); RadioSAudioPreferences prefs;
    radios_audio_preferences_default(&prefs); prefs.cue_source = 0; prefs.monitor_source = -1;
    g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL));
    load(engine, 0, fixture, 0, 0); load(engine, 1, fixture, 0, 0); ready(engine, 0, 4096); ready(engine, 1, 4096);
    play(engine, 0); play(engine, 1); gfloat output[3][8192]; render(engine, 1000, output);
    for (guint i = 0; i < 1000; ++i) { close_to(output[0][i * 2], .5f); close_to(output[1][i * 2], .25f); close_to(output[2][i * 2 + 1], -.25f); }
    g_assert_true(radios_audio_engine_set_deck_gain(engine, 1, -6.020599913279624, NULL));
    prefs.main_gain_db = -6.020599913279624; g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL));
    render(engine, 500, output);
    g_assert_cmpfloat(output[0][0], >, .1875f); close_to(output[0][998], .1875f);
    render(engine, 64, output); close_to(output[0][0], .1875f); close_to(output[1][0], .25f); close_to(output[2][0], .375f);
    g_assert_true(radios_audio_engine_set_bus_gate(engine, RADIOS_AUDIO_BUS_CUE, FALSE, NULL));
    render(engine, 64, output); close_to(output[1][0], 0); close_to(output[0][0], .1875f);
    RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_cmpuint(snapshot.decks[0].cursor_frames, ==, 1628); g_assert_cmpuint(snapshot.decks[1].cursor_frames, ==, 1628);
    close_to(snapshot.buses[0].peak[0], .1875f); radios_audio_engine_unref(engine);
}
static void transport(Fixture *fixture, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); gfloat output[3][8192]; RadioSAudioSnapshot snapshot;
    load(engine, 0, fixture, 100, 900); ready(engine, 0, 800); play(engine, 0); render(engine, 100, output);
    g_assert_true(radios_audio_engine_pause(engine, 0, NULL)); render(engine, 50, output); close_to(output[0][0], 0);
    radios_audio_engine_snapshot(engine, &snapshot); g_assert_cmpuint(snapshot.decks[0].cursor_frames, ==, 200);
    play(engine, 0); render(engine, 100, output);
    g_assert_true(radios_audio_engine_seek(engine, 0, 500, NULL)); ready(engine, 0, 400); render(engine, 400, output);
    radios_audio_engine_snapshot(engine, &snapshot); g_assert_cmpuint(snapshot.decks[0].cursor_frames, ==, 900);
    g_assert_cmpint(snapshot.decks[0].state, ==, RADIOS_AUDIO_EOF);
    render(engine, 64, output); close_to(output[0][0], 0);
    play(engine, 0); ready(engine, 0, 800); render(engine, 20, output);
    radios_audio_engine_snapshot(engine, &snapshot); g_assert_cmpuint(snapshot.decks[0].cursor_frames, ==, 120);
    g_assert_true(radios_audio_engine_stop(engine, 0, NULL)); ready(engine, 0, 800);
    radios_audio_engine_snapshot(engine, &snapshot); g_assert_cmpuint(snapshot.decks[0].cursor_frames, ==, 100);
    g_assert_cmpint(snapshot.decks[0].state, ==, RADIOS_AUDIO_READY); radios_audio_engine_unref(engine);
}
static void preview(Fixture *fixture, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); GError *error = NULL;
    g_assert_false(radios_audio_engine_set_deck_main(engine, 32, TRUE, &error)); g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID); g_clear_error(&error);
    load(engine, 32, fixture, 0, 500); ready(engine, 32, 500); play(engine, 32); gfloat output[3][8192]; render(engine, 500, output);
    close_to(output[0][0], 0); close_to(output[1][0], .25f); close_to(output[2][0], 0);
    radios_audio_engine_unref(engine);
}
static void validation(Fixture *fixture, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); RadioSAudioPreferences prefs; GError *error = NULL;
    radios_audio_preferences_default(&prefs); memset(prefs.main_sink, 'x', sizeof prefs.main_sink);
    g_assert_false(radios_audio_engine_apply_preferences(engine, &prefs, &error)); g_clear_error(&error);
    radios_audio_preferences_default(&prefs); prefs.main_gain_db = NAN;
    g_assert_false(radios_audio_engine_apply_preferences(engine, &prefs, &error)); g_clear_error(&error);
    g_assert_false(radios_audio_engine_set_deck_gain(engine, 0, INFINITY, &error)); g_clear_error(&error);
    g_assert_false(radios_audio_engine_play(engine, RADIOS_AUDIO_DECKS, &error)); g_clear_error(&error);
    RadioSAudioLoadSpec spec = {fixture->path, "invalid", fixture->media, fixture->version, 0, 0};
    g_assert_false(radios_audio_engine_load(engine, 0, &spec, &error)); g_clear_error(&error);
    radios_audio_engine_unref(engine);
}
static void replacement(Fixture *fixture, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioLoadSpec spec = {fixture->path, "0000000000000000000000000000000000000000000000000000000000000000", fixture->media, fixture->version, 0, 0};
    g_assert_true(radios_audio_engine_load(engine, 0, &spec, NULL)); RadioSAudioSnapshot snapshot;
    gint64 until = g_get_monotonic_time() + 5000000;
    do { radios_audio_engine_snapshot(engine, &snapshot); if (snapshot.decks[0].state == RADIOS_AUDIO_ERROR) break; g_usleep(1000); } while (g_get_monotonic_time() < until);
    g_assert_cmpint(snapshot.decks[0].state, ==, RADIOS_AUDIO_ERROR); g_assert_cmpuint(snapshot.decks[0].cursor_frames, ==, 0);
    load(engine, 0, fixture, 0, 0); ready(engine, 0, 500); g_assert_cmpuint(snapshot.decks[0].generation, ==, 1);
    for (guint i = 0; i < 100; ++i) load(engine, 0, fixture, i, 1000);
    ready(engine, 0, 901); radios_audio_engine_snapshot(engine, &snapshot); g_assert_cmpuint(snapshot.decks[0].cursor_frames, ==, 99);
    radios_audio_engine_unref(engine);
}
static void all_decks(Fixture *fixture, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    for (guint i = 0; i < 33; ++i) { load(engine, i, fixture, i, 200); ready(engine, i, 100); play(engine, i); }
    gfloat output[3][8192]; render(engine, 100, output); close_to(output[0][0], 1); close_to(output[1][0], .25f);
    RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
    for (guint i = 0; i < 33; ++i) g_assert_cmpuint(snapshot.decks[i].cursor_frames, ==, i + 100);
    RadioSAudioEngine *reference = radios_audio_engine_ref(engine); radios_audio_engine_unref(engine); radios_audio_engine_unref(reference);
}
static void sampler_voices(Fixture *fixture, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    const guint voices[] = {0, RADIOS_AUDIO_SAMPLER_BASE, RADIOS_AUDIO_SAMPLER_BASE + 1, RADIOS_AUDIO_MASTER_SAMPLER};
    for (guint i = 0; i < G_N_ELEMENTS(voices); ++i) { load(engine, voices[i], fixture, 0, 4000); ready(engine, voices[i], 2000); play(engine, voices[i]); }
    gfloat output[3][8192]; render(engine, 100, output); close_to(output[0][0], 1);
    RadioSAudioMixerConfig config; radios_audio_engine_get_mixer(engine, &config); config.channels[0].mute = TRUE;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &config, NULL)); render(engine, 100, output); close_to(output[0][0], .5f);
    config.channels[0].mute = FALSE; g_assert_true(radios_audio_engine_apply_mixer(engine, &config, NULL));
    load(engine, RADIOS_AUDIO_SAMPLER_BASE, fixture, 500, 4000); ready(engine, RADIOS_AUDIO_SAMPLER_BASE, 2000); play(engine, RADIOS_AUDIO_SAMPLER_BASE);
    render(engine, 100, output); RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_cmpuint(snapshot.decks[0].cursor_frames, ==, 300);
    g_assert_cmpuint(snapshot.decks[RADIOS_AUDIO_SAMPLER_BASE].cursor_frames, ==, 600);
    g_assert_cmpuint(snapshot.decks[RADIOS_AUDIO_SAMPLER_BASE + 1].cursor_frames, ==, 300);
    g_assert_cmpuint(snapshot.decks[RADIOS_AUDIO_MASTER_SAMPLER].cursor_frames, ==, 300);
    g_assert_cmpuint(snapshot.decks[RADIOS_AUDIO_SAMPLER_BASE].generation, ==, 2);
    radios_audio_engine_unref(engine);
}
static void transient_unload(Fixture *fixture, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); GError *error = NULL;
    RadioSAudioLoadSpec spec = {fixture->path, fixture->sha, NULL, NULL, 0, 1000};
    g_assert_false(radios_audio_engine_load(engine, 0, &spec, &error)); g_clear_error(&error);
    g_assert_true(radios_audio_engine_load(engine, 32, &spec, NULL)); ready(engine, 32, 1000); play(engine, 32);
    gfloat output[3][8192]; render(engine, 100, output); close_to(output[0][0], 0); close_to(output[1][0], .25f);
    g_assert_true(radios_audio_engine_unload(engine, 32, NULL)); g_assert_cmpint(g_unlink(fixture->path), ==, 0);
    g_usleep(10000); RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_cmpint(snapshot.decks[32].state, ==, RADIOS_AUDIO_EMPTY); g_assert_cmpuint(snapshot.decks[32].cursor_frames, ==, 0);
    g_assert_cmpuint(snapshot.decks[32].buffered_frames, ==, 0); g_assert_cmpstr(snapshot.decks[32].media_id, ==, "");
    wave(fixture, 12000); load(engine, 32, fixture, 0, 1000); ready(engine, 32, 1000);
    for (guint i = 0; i < 30; ++i) { load(engine, 32, fixture, i, 1000); g_assert_true(radios_audio_engine_unload(engine, 32, NULL)); }
    g_usleep(10000); radios_audio_engine_snapshot(engine, &snapshot); g_assert_cmpint(snapshot.decks[32].state, ==, RADIOS_AUDIO_EMPTY);
    radios_audio_engine_unref(engine);
}
static void mixer_graph(Fixture *fixture, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); RadioSAudioMixerConfig config;
    radios_audio_engine_get_mixer(engine, &config); config.channels[0].source_deck = -1;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &config, NULL)); load(engine, 0, fixture, 0, 0); ready(engine, 0, 1000);
    GError *error = NULL; g_assert_false(radios_audio_engine_play(engine, 0, &error)); g_clear_error(&error);
    config.channels[0].source_deck = 0; config.channels[0].pfl = TRUE; config.channels[1].source_deck = 0;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &config, NULL)); RadioSAudioPreferences prefs;
    radios_audio_preferences_default(&prefs); prefs.cue_source = -1; g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL));
    play(engine, 0); gfloat output[3][8192]; render(engine, 100, output); close_to(output[0][0], .5f); close_to(output[1][0], .25f);
    RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot); g_assert_cmpuint(snapshot.decks[0].cursor_frames, ==, 100);
    g_assert_true(radios_audio_engine_set_deck_main(engine, 0, FALSE, NULL)); render(engine, 100, output);
    close_to(output[0][0], 0); close_to(output[1][0], .25f); radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_cmpint(snapshot.decks[0].output_bus, ==, RADIOS_AUDIO_BUS_CUE); g_assert_cmpuint(snapshot.decks[0].cursor_frames, ==, 200);
    load(engine, 32, fixture, 0, 500); ready(engine, 32, 500); prefs.cue_source = 32;
    g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL)); play(engine, 32); render(engine, 100, output);
    close_to(output[1][0], .25f); close_to(output[0][0], 0); radios_audio_engine_unref(engine);
}
static void pulse_ready(RadioSAudioEngine *engine, gboolean microphone)
{
    RadioSAudioSnapshot snapshot; gint64 until = g_get_monotonic_time() + 10000000;
    do {
        radios_audio_engine_snapshot(engine, &snapshot);
        if (snapshot.buses[0].ready && snapshot.buses[1].ready && snapshot.buses[2].ready && (!microphone || snapshot.microphone_ready)) return;
        g_usleep(10000);
    } while (g_get_monotonic_time() < until);
    g_error("Pulse streams unavailable: %s MAIN %s CUE %s MON %s mic %s", snapshot.error,
        snapshot.buses[0].error, snapshot.buses[1].error, snapshot.buses[2].error, snapshot.microphone_error);
}
static void pulse(Fixture *fixture, gconstpointer unused)
{
    (void)unused; const gchar *main = g_getenv("RADIOS_TEST_MAIN_SINK");
    if (!main) { g_test_skip("Owned virtual Pulse sinks require tests/run_audio_ui.py --engine-only"); return; }
    wave(fixture, 480000); RadioSAudioEngine *engine = radios_audio_engine_new(TRUE, NULL); RadioSAudioPreferences prefs;
    g_assert_true(radios_audio_engine_set_ducking(engine, FALSE, -18, NULL));
    radios_audio_preferences_default(&prefs);
    g_strlcpy(prefs.main_sink, main, sizeof prefs.main_sink);
    g_strlcpy(prefs.cue_sink, g_getenv("RADIOS_TEST_CUE_SINK"), sizeof prefs.cue_sink);
    g_strlcpy(prefs.monitor_sink, g_getenv("RADIOS_TEST_MONITOR_SINK"), sizeof prefs.monitor_sink);
    g_strlcpy(prefs.microphone_source, g_getenv("RADIOS_TEST_MIC_SOURCE"), sizeof prefs.microphone_source);
    prefs.cue_source = 32; prefs.monitor_source = 1;
    g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL)); pulse_ready(engine, TRUE);
    load(engine, 0, fixture, 0, 0); load(engine, 1, fixture, 0, 0); load(engine, 32, fixture, 0, 0);
    g_assert_true(radios_audio_engine_set_deck_main(engine, 1, FALSE, NULL));
    g_assert_true(radios_audio_engine_set_deck_gain(engine, 1, -6.020599913279624, NULL));
    ready(engine, 0, 1000); ready(engine, 1, 1000); ready(engine, 32, 1000); pulse_ready(engine, TRUE);
    play(engine, 0); play(engine, 1); play(engine, 32);
    RadioSAudioSnapshot snapshot; gint64 until = g_get_monotonic_time() + 10000000;
    do {
        radios_audio_engine_snapshot(engine, &snapshot);
        if (snapshot.decks[0].cursor_frames > 4800 && snapshot.microphone_peak[0] > .1f &&
            snapshot.buses[0].peak[0] > .2f && snapshot.buses[1].peak[0] > .2f && snapshot.buses[2].peak[0] > .1f) break;
        g_usleep(10000);
    } while (g_get_monotonic_time() < until);
    g_assert_cmpuint(snapshot.decks[0].cursor_frames, >, 4800); g_assert_false(snapshot.microphone_gate);
    close_to(snapshot.buses[0].peak[0], .25f); close_to(snapshot.buses[1].peak[0], .25f); close_to(snapshot.microphone_peak[0], .125f);
    g_assert_true(radios_audio_engine_pause(engine, 0, NULL)); radios_audio_engine_snapshot(engine, &snapshot);
    guint64 paused = snapshot.decks[0].cursor_frames; g_usleep(150000); radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_cmpuint(snapshot.decks[0].cursor_frames, ==, paused); g_assert_cmpint(snapshot.decks[0].state, ==, RADIOS_AUDIO_PAUSED);
    pulse_ready(engine, TRUE); play(engine, 0);
    until = g_get_monotonic_time() + 5000000;
    do { radios_audio_engine_snapshot(engine, &snapshot); if (snapshot.decks[0].cursor_frames > paused) break; g_usleep(10000); } while (g_get_monotonic_time() < until);
    g_assert_cmpuint(snapshot.decks[0].cursor_frames, >, paused);
    g_assert_true(radios_audio_engine_set_microphone_gate(engine, TRUE, NULL)); pulse_ready(engine, TRUE);
    until = g_get_monotonic_time() + 5000000;
    do { radios_audio_engine_snapshot(engine, &snapshot); if (snapshot.buses[0].peak[0] > .35f) break; g_usleep(10000); } while (g_get_monotonic_time() < until);
    g_assert_true(snapshot.microphone_gate); close_to(snapshot.buses[0].peak[0], .375f);
    g_assert_true(radios_audio_engine_set_microphone_gate(engine, FALSE, NULL)); pulse_ready(engine, TRUE);
    g_assert_true(radios_audio_engine_seek(engine, 0, 96000, NULL)); pulse_ready(engine, TRUE);
    ready(engine, 0, 1000); g_usleep(150000); radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_cmpuint(snapshot.decks[0].cursor_frames, >=, 96000); g_assert_cmpuint(snapshot.decks[0].cursor_frames, <, 144000);
    g_assert_true(radios_audio_engine_stop(engine, 0, NULL)); ready(engine, 0, 1000); radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_cmpuint(snapshot.decks[0].cursor_frames, ==, 0);
    RadioSAudioMixerConfig mixer; radios_audio_engine_get_mixer(engine, &mixer); mixer.channels[0].pfl = TRUE;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, NULL)); prefs.main_sink[0] = '\0'; prefs.cue_source = -1;
    g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL)); until = g_get_monotonic_time() + 5000000;
    do { radios_audio_engine_snapshot(engine, &snapshot); if (snapshot.buses[1].ready && !snapshot.buses[0].ready) break; g_usleep(10000); } while (g_get_monotonic_time() < until);
    g_assert_true(snapshot.buses[1].ready); g_assert_false(snapshot.buses[0].ready); play(engine, 0);
    until = g_get_monotonic_time() + 5000000;
    do { radios_audio_engine_snapshot(engine, &snapshot); if (snapshot.decks[0].cursor_frames > 1000) break; g_usleep(10000); } while (g_get_monotonic_time() < until);
    g_assert_cmpuint(snapshot.decks[0].cursor_frames, >, 1000); g_assert_cmpint(snapshot.decks[0].output_bus, ==, RADIOS_AUDIO_BUS_CUE);
    g_strlcpy(prefs.main_sink, "radios-nonexistent-device", sizeof prefs.main_sink);
    g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL)); radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_false(snapshot.buses[0].ready); g_usleep(150000); radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_false(snapshot.buses[0].ready); g_assert_false(snapshot.microphone_gate);
    radios_audio_engine_unref(engine);
}
static RadioSAudioPlayerBinding external_binding(RadioSAudioEngine *engine)
{
    RadioSAudioPlayerBinding value = {0};
    g_strlcpy(value.playlist_id, "11111111-1111-4111-8111-111111111111", sizeof value.playlist_id);
    g_strlcpy(value.entry_id, "22222222-2222-4222-8222-222222222222", sizeof value.entry_id);
    g_strlcpy(value.provider, "application", sizeof value.provider);
    g_strlcpy(value.source_id, "pulse:1:1", sizeof value.source_id);
    g_strlcpy(value.title, "External PCM fixture", sizeof value.title);
    g_assert_true(radios_audio_engine_bind_external_player(engine, &value, NULL)); return value;
}
static void external_main(void)
{
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); GError *error = NULL;
    gfloat input[8192], pcm[3][8192];
    for (guint i = 0; i < G_N_ELEMENTS(input); ++i) input[i] = i % 2 ? -.125f : .25f;
    RadioSAudioPlayerBinding binding = external_binding(engine);
    guint64 first = radios_audio_engine_external_begin_player(engine, &binding, &error); g_assert_cmpuint(first, >, 0); g_assert_no_error(error);
    g_assert_cmpuint(radios_audio_engine_external_begin_player(engine, &binding, &error), ==, 0);
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_STATE); g_clear_error(&error);
    g_assert_true(radios_audio_engine_external_push(engine, first, input, 128, &error)); g_assert_no_error(error);
    render(engine, 128, pcm); close_to(pcm[0][0], .25f); close_to(pcm[0][1], -.125f);
    RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_true(snapshot.external_main_active); g_assert_cmpuint(snapshot.external_main_received_frames, ==, 128);
    g_assert_cmpuint(snapshot.external_main_queued_frames, ==, 0); g_assert_false(snapshot.microphone_gate);
    g_assert_cmpint(snapshot.decks[0].state, ==, RADIOS_AUDIO_PLAYING); g_assert_true(snapshot.decks[0].external_bound);
    g_assert_cmpuint(snapshot.decks[0].cursor_frames, ==, 128);
    for (guint i = 1; i < RADIOS_AUDIO_DECKS; ++i) g_assert_cmpint(snapshot.decks[i].state, ==, RADIOS_AUDIO_EMPTY);
    RadioSAudioMixerConfig mixer; radios_audio_engine_get_mixer(engine, &mixer); mixer.master.mute = TRUE;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, NULL));
    g_assert_true(radios_audio_engine_external_push(engine, first, input, 64, NULL)); render(engine, 64, pcm); close_to(pcm[0][0], 0);
    mixer.master.mute = FALSE; g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, NULL));
    RadioSAudioPreferences prefs; radios_audio_preferences_default(&prefs); prefs.main_gain_db = -6.020599913279624;
    g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL));
    g_assert_true(radios_audio_engine_external_push(engine, first, input, 512, NULL)); render(engine, 512, pcm);
    close_to(pcm[0][1022], .125f); close_to(pcm[0][1023], -.0625f);
    input[0] = NAN; g_assert_false(radios_audio_engine_external_push(engine, first, input, 1, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID); g_clear_error(&error); input[0] = .25f;
    g_assert_true(radios_audio_engine_external_push(engine, first, input, 32, NULL));
    radios_audio_engine_external_end(engine, first); render(engine, 32, pcm); close_to(pcm[0][0], 0);
    guint64 second = radios_audio_engine_external_begin_player(engine, &binding, NULL); g_assert_cmpuint(second, >, first);
    radios_audio_engine_external_end(engine, first);
    g_assert_false(radios_audio_engine_external_push(engine, first, input, 1, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_STATE); g_clear_error(&error);
    g_assert_true(radios_audio_engine_external_push(engine, second, input, 4096, NULL));
    g_assert_true(radios_audio_engine_external_push(engine, second, input, 4096, NULL));
    g_assert_false(radios_audio_engine_external_push(engine, second, input, 1, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_UNAVAILABLE); g_clear_error(&error);
    radios_audio_engine_snapshot(engine, &snapshot); g_assert_false(snapshot.external_main_active);
    g_assert_cmpuint(snapshot.external_main_queued_frames, ==, 0); g_assert_nonnull(strstr(snapshot.external_main_error, "doldu"));
    guint64 third = radios_audio_engine_external_begin_player(engine, &binding, NULL); g_assert_cmpuint(third, >, second);
    g_assert_true(radios_audio_engine_set_bus_gate(engine, RADIOS_AUDIO_BUS_MAIN, FALSE, NULL));
    g_assert_false(radios_audio_engine_external_push(engine, third, input, 1, &error)); g_clear_error(&error);
    g_assert_cmpuint(radios_audio_engine_external_begin_player(engine, &binding, &error), ==, 0);
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_UNAVAILABLE); g_clear_error(&error);
    g_assert_true(radios_audio_engine_set_bus_gate(engine, RADIOS_AUDIO_BUS_MAIN, TRUE, NULL));
    render(engine, 32, pcm); close_to(pcm[0][0], 0); radios_audio_engine_unref(engine);
}
static void external_tail(void)
{
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); RadioSAudioMixerConfig mixer;
    radios_audio_engine_get_mixer(engine, &mixer); mixer.master_fx.enabled = TRUE;
    mixer.master_fx.type = RADIOS_FX_ECHO; mixer.master_fx.depth = 1;
    mixer.master_fx.return_db = mixer.master.fx_return_db = 0;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, NULL));
    RadioSAudioPlayerBinding binding = external_binding(engine);
    guint64 token = radios_audio_engine_external_begin_player(engine, &binding, NULL); g_assert_cmpuint(token, >, 0);
    gfloat impulse[2] = {.25f, .25f}, pcm[3][8192];
    g_assert_true(radios_audio_engine_external_push(engine, token, impulse, 1, NULL)); render(engine, 1, pcm);
    gdouble tail = 0;
    for (guint block = 0; block < 8; ++block) {
        render(engine, 2048, pcm);
        for (guint i = 0; i < 4096; ++i) tail += fabsf(pcm[0][i]);
    }
    g_assert_cmpfloat(tail, >, .01); radios_audio_engine_external_end(engine, token);
    for (guint block = 0; block < 12; ++block) {
        render(engine, 4096, pcm);
        for (guint i = 0; i < 8192; ++i) close_to(pcm[0][i], 0);
    }
    radios_audio_engine_unref(engine);
}
static void loudness_player_policy_pcm(Fixture *fixture, gconstpointer unused)
{
    (void)unused; wave(fixture, 40000); RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioPreferences prefs; radios_audio_preferences_default(&prefs); prefs.cue_source=0;
    prefs.loudness_policy.players=1; prefs.loudness_policy.contexts=RADIOS_AUDIO_LOUDNESS_MANUAL;
    g_assert_true(radios_audio_engine_apply_preferences(engine,&prefs,NULL));
    load(engine,0,fixture,0,0);load(engine,1,fixture,0,0);ready(engine,0,4096);ready(engine,1,4096);
    RadioSAudioSnapshot before,after;radios_audio_engine_snapshot(engine,&before);
    RadioSLoudness measured={.integrated_lufs=-8,.peak=.25,.measured=TRUE};
    for(guint deck=0;deck<2;++deck)g_assert_true(radios_audio_engine_set_deck_loudness(engine,deck,before.decks[deck].generation,&measured,NULL));
    g_assert_true(radios_audio_engine_set_deck_gain(engine,0,-6.020599913279624,NULL));play(engine,0);play(engine,1);
    gfloat output[3][8192];render(engine,512,output);gdouble factor=pow(10,-6.0/20);
    close_to(output[0][1022],.25+.125*factor);close_to(output[1][1022],.125*factor);
    radios_audio_engine_snapshot(engine,&after);close_to(after.decks[0].loudness_gain_db,-6);close_to(after.decks[1].loudness_gain_db,0);
    close_to(after.decks[0].gain_db,-6.020599913279624);g_assert_cmpuint(after.decks[0].generation,==,before.decks[0].generation);
    prefs.loudness_policy.schedule_enabled=TRUE; /* Empty schedule matches no time. */
    g_assert_true(radios_audio_engine_apply_preferences(engine,&prefs,NULL));render(engine,512,output);
    g_assert_cmpfloat(output[1][0],>,.125*factor);g_assert_cmpfloat(output[1][0],<,.125);close_to(output[1][1022],.125);
    radios_audio_engine_snapshot(engine,&after);close_to(after.decks[0].loudness_gain_db,0);close_to(after.decks[0].loudness_lufs,-8);
    g_assert_true(after.decks[0].loudness_measured);close_to(after.decks[0].gain_db,-6.020599913279624);
    RadioSAudioPreferences invalid=prefs;g_strlcpy(invalid.loudness_policy.timezone,"Not/AZone",128);GError *error=NULL;
    g_assert_false(radios_audio_engine_apply_preferences(engine,&invalid,&error));g_assert_error(error,RADIOS_AUDIO_ERROR_DOMAIN,RADIOS_AUDIO_ERROR_INVALID);g_clear_error(&error);
    render(engine,256,output);close_to(output[1][510],.125);
    prefs.loudness_policy.schedule_enabled=FALSE;g_assert_true(radios_audio_engine_apply_preferences(engine,&prefs,NULL));render(engine,512,output);close_to(output[1][1022],.125*factor);
    prefs.loudness_normalize=FALSE;g_assert_true(radios_audio_engine_apply_preferences(engine,&prefs,NULL));render(engine,512,output);close_to(output[1][1022],.125);
    radios_audio_engine_unref(engine);
}
static void loudness_preview_legacy_pcm(Fixture *fixture,gconstpointer unused)
{
    (void)unused;RadioSAudioEngine *engine=radios_audio_engine_new(FALSE,NULL);load(engine,32,fixture,0,0);ready(engine,32,4096);
    RadioSAudioSnapshot snapshot;radios_audio_engine_snapshot(engine,&snapshot);RadioSLoudness measured={.integrated_lufs=-8,.peak=.25,.measured=TRUE};
    g_assert_true(radios_audio_engine_set_deck_loudness(engine,32,snapshot.decks[32].generation,&measured,NULL));play(engine,32);
    gfloat output[3][8192];render(engine,512,output);gdouble factor=pow(10,-6.0/20);close_to(output[1][1022],.25*factor);
    RadioSAudioPreferences prefs;radios_audio_preferences_default(&prefs);prefs.loudness_policy.players=1;
    g_assert_true(radios_audio_engine_apply_preferences(engine,&prefs,NULL));render(engine,512,output);close_to(output[1][1022],.25);
    prefs.loudness_policy.players=G_MAXUINT32;g_assert_true(radios_audio_engine_apply_preferences(engine,&prefs,NULL));render(engine,512,output);close_to(output[1][1022],.25*factor);
    close_to(output[0][1022],0);radios_audio_engine_unref(engine);
}
static void loudness_mixer_channel_pcm(Fixture *fixture, gconstpointer unused)
{
    (void)unused; wave(fixture, 40000); RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioPreferences prefs; radios_audio_preferences_default(&prefs); prefs.loudness_normalize = FALSE;
    g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL));
    load(engine, 0, fixture, 0, 0); ready(engine, 0, 4096);
    RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot); guint64 generation = snapshot.decks[0].generation;
    RadioSLoudness measured = {.integrated_lufs = -8, .peak = .25, .measured = TRUE};
    g_assert_true(radios_audio_engine_set_deck_loudness(engine, 0, generation, &measured, NULL));
    RadioSAudioMixerConfig mixer; radios_audio_engine_get_mixer(engine, &mixer);
    mixer.channels[0].source_deck = -1; mixer.channels[7].source_deck = 0; mixer.channels[7].gain_db = -18;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, NULL)); play(engine, 0);
    gfloat output[3][8192]; render(engine, 512, output); gdouble factor = pow(10, -6.0 / 20);
    close_to(output[0][1022], .25 * pow(10, -18.0 / 20));
    prefs.loudness_normalize = TRUE; g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL));
    render(engine, 512, output); close_to(output[0][1022], .25 * factor);
    radios_audio_engine_snapshot(engine, &snapshot);
    close_to(snapshot.dsp_meters.channel_loudness_gain_db[7], -6);
    g_assert_true(snapshot.dsp_meters.channel_loudness_active[7]); g_assert_false(snapshot.dsp_meters.channel_loudness_active[0]);
    close_to(snapshot.mixer.channels[7].gain_db, -6); close_to(snapshot.decks[0].gain_db, 0);
    const gdouble manual_attempts[] = {6, -12, -60};
    for (guint i = 0; i < G_N_ELEMENTS(manual_attempts); ++i) {
        radios_audio_engine_get_mixer(engine, &mixer); mixer.channels[7].gain_db = manual_attempts[i];
        g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, NULL)); render(engine, 512, output);
        close_to(output[0][1022], .25 * factor);
        radios_audio_engine_snapshot(engine, &snapshot); close_to(snapshot.mixer.channels[7].gain_db, -6);
        g_assert_cmpuint(snapshot.decks[0].generation, ==, generation);
    }
    radios_audio_engine_get_mixer(engine, &mixer); mixer.channels[7].loudness_bypass = TRUE;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, NULL)); render(engine, 512, output);
    close_to(output[0][1022], .25 * pow(10, -60.0 / 20));
    radios_audio_engine_snapshot(engine, &snapshot); close_to(snapshot.mixer.channels[7].gain_db, -60);
    g_assert_false(snapshot.dsp_meters.channel_loudness_active[7]);
    radios_audio_engine_get_mixer(engine, &mixer); mixer.channels[7].gain_db = -12;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, NULL)); render(engine, 512, output);
    close_to(output[0][1022], .25 * pow(10, -12.0 / 20));
    radios_audio_engine_get_mixer(engine, &mixer); mixer.channels[7].loudness_bypass = FALSE;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, NULL)); render(engine, 512, output); close_to(output[0][1022], .25 * factor);
    prefs.loudness_normalize = FALSE; g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL));
    render(engine, 512, output); close_to(output[0][1022], .25 * pow(10, -12.0 / 20));
    radios_audio_engine_snapshot(engine, &snapshot); close_to(snapshot.mixer.channels[7].gain_db, -12);
    prefs.loudness_normalize = TRUE; prefs.loudness_policy.players = 2;
    g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL)); render(engine, 512, output);
    close_to(output[0][1022], .25 * pow(10, -12.0 / 20));
    radios_audio_engine_snapshot(engine, &snapshot); close_to(snapshot.mixer.channels[7].gain_db, -12);
    g_assert_false(snapshot.dsp_meters.channel_loudness_active[7]);
    prefs.loudness_policy.players = 1; g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL));
    render(engine, 512, output); close_to(output[0][1022], .25 * factor);
    radios_audio_engine_get_mixer(engine, &mixer); mixer.channels[7].mute = TRUE;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, NULL)); render(engine, 512, output); close_to(output[0][1022], 0);
    radios_audio_engine_snapshot(engine, &snapshot); g_assert_true(snapshot.mixer.channels[7].mute); close_to(snapshot.mixer.channels[7].gain_db, -6);
    radios_audio_engine_get_mixer(engine, &mixer); mixer.channels[7].mute = FALSE;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, NULL)); render(engine, 512, output); close_to(output[0][1022], .25 * factor);
    measured.measured = FALSE; g_assert_true(radios_audio_engine_set_deck_loudness(engine, 0, generation, &measured, NULL));
    render(engine, 512, output); close_to(output[0][1022], .25 * pow(10, -12.0 / 20));
    radios_audio_engine_snapshot(engine, &snapshot); close_to(snapshot.mixer.channels[7].gain_db, -12);
    g_assert_false(snapshot.dsp_meters.channel_loudness_active[7]); radios_audio_engine_unref(engine);
}
static void loudness_auto_positive_residual_and_player_gain_pcm(Fixture *fixture, gconstpointer unused)
{
    (void)unused; wave(fixture, 40000); RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioPreferences prefs; radios_audio_preferences_default(&prefs); prefs.loudness_normalize = FALSE;
    g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL));
    load(engine, 0, fixture, 0, 0); ready(engine, 0, 4096);
    RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
    RadioSLoudness measured = {.integrated_lufs = -40, .peak = .25, .measured = TRUE};
    gdouble correction = radios_loudness_gain_db(&measured, -14), amplitude = .25 * pow(10, correction / 20);
    g_assert_cmpfloat(correction, >, 6); g_assert_cmpfloat(correction, <=, 12); g_assert_cmpfloat(amplitude, <, 1);
    g_assert_true(radios_audio_engine_set_deck_loudness(engine, 0, snapshot.decks[0].generation, &measured, NULL));
    RadioSAudioMixerConfig mixer; radios_audio_engine_get_mixer(engine, &mixer); mixer.channels[0].gain_db = 6;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, NULL));
    prefs.loudness_normalize = TRUE; g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL)); play(engine, 0);
    gfloat output[3][8192]; render(engine, 512, output); close_to(output[0][1022], amplitude);
    radios_audio_engine_snapshot(engine, &snapshot); close_to(snapshot.mixer.channels[0].gain_db, 6);
    close_to(snapshot.dsp_meters.channel_loudness_gain_db[0], correction); g_assert_true(snapshot.dsp_meters.channel_loudness_active[0]);
    g_assert_true(radios_audio_engine_set_deck_gain(engine, 0, -6.020599913279624, NULL));
    render(engine, 512, output); close_to(output[0][1022], amplitude * .5);
    radios_audio_engine_snapshot(engine, &snapshot); close_to(snapshot.mixer.channels[0].gain_db, 6);
    close_to(snapshot.decks[0].gain_db, -6.020599913279624);
    mixer = snapshot.mixer; mixer.channels[0].loudness_bypass = TRUE;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, NULL)); render(engine, 512, output);
    close_to(output[0][1022], .125 * pow(10, 6.0 / 20)); radios_audio_engine_unref(engine);
}

static void microphone_state(RadioSAudioEngine *engine, gboolean gate, gboolean latch, gboolean ptt)
{
    RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_cmpint(snapshot.microphone_gate, ==, gate); g_assert_cmpint(snapshot.microphone_latch, ==, latch);
    g_assert_cmpint(snapshot.microphone_ptt, ==, ptt); g_assert_cmpint(radios_audio_engine_microphone_open(engine), ==, gate);
}
/* Each talk-key step keeps the PLAYING deck's transport: same generation and
 * state, its cursor advances by exactly the rendered frames and MAIN carries
 * the uninterrupted programme PCM. */
static void transport_untouched(RadioSAudioEngine *engine, guint64 generation, guint64 *cursor)
{
    gfloat output[3][8192]; render(engine, 128, output); RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_cmpint(snapshot.decks[0].state, ==, RADIOS_AUDIO_PLAYING); g_assert_cmpuint(snapshot.decks[0].generation, ==, generation);
    g_assert_cmpuint(snapshot.decks[0].cursor_frames, ==, *cursor + 128); *cursor += 128;
    for (guint frame = 0; frame < 128; ++frame) { close_to(output[0][frame * 2], .25f); close_to(output[0][frame * 2 + 1], -.125f); }
}
/* R7.3: the on-air gate is latch (L) || push-to-talk (R). Releasing R ends the
 * momentary talk only: it never closes a latched microphone, a latch closed
 * while R is held leaves the gate open until R is released, and no step ever
 * touches a deck's transport. The offline engine has no capture device and
 * accepts the gate so the truth table is testable without a sound server. */
static void microphone_latch_ptt(Fixture *fixture, gconstpointer unused)
{
    (void)unused; wave(fixture, 48000); RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); GError *error = NULL;
    load(engine, 0, fixture, 0, 0); ready(engine, 0, 4096); play(engine, 0);
    gfloat output[3][8192]; render(engine, 64, output); RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
    guint64 generation = snapshot.decks[0].generation, cursor = snapshot.decks[0].cursor_frames; g_assert_cmpuint(cursor, ==, 64);
    microphone_state(engine, FALSE, FALSE, FALSE);
    const struct { gboolean latch_step, value, gate, latch, ptt; } steps[] = {
        {FALSE, TRUE, TRUE, FALSE, TRUE},   /* R pressed alone */
        {FALSE, FALSE, FALSE, FALSE, FALSE}, /* R released: talk ends */
        {TRUE, TRUE, TRUE, TRUE, FALSE},    /* L latches */
        {FALSE, TRUE, TRUE, TRUE, TRUE},    /* R pressed while latched */
        {FALSE, FALSE, TRUE, TRUE, FALSE},  /* R released: the latch stays on air */
        {FALSE, TRUE, TRUE, TRUE, TRUE},    /* R pressed again */
        {TRUE, FALSE, TRUE, FALSE, TRUE},   /* L released while R is held: still on air */
        {FALSE, FALSE, FALSE, FALSE, FALSE}, /* R released: off air */
        {FALSE, FALSE, FALSE, FALSE, FALSE}, /* a repeated release is harmless */
        {TRUE, FALSE, FALSE, FALSE, FALSE},  /* so is a repeated latch release */
    };
    for (guint i = 0; i < G_N_ELEMENTS(steps); ++i) {
        gboolean accepted = steps[i].latch_step ? radios_audio_engine_set_microphone_gate(engine, steps[i].value, &error) :
            radios_audio_engine_set_microphone_ptt(engine, steps[i].value, &error);
        g_assert_no_error(error); g_assert_true(accepted);
        microphone_state(engine, steps[i].gate, steps[i].latch, steps[i].ptt);
        transport_untouched(engine, generation, &cursor);
    }
    g_assert_false(radios_audio_engine_set_microphone_ptt(NULL, TRUE, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID); g_clear_error(&error);
    radios_audio_engine_unref(engine);
}

/* R7.3 with real devices: latch and push-to-talk open on the owned virtual
 * outputs while the deck plays; losing the microphone device closes the gate,
 * the latch and the push-to-talk together, and MAIN keeps playing the deck. */
static void pulse_microphone_loss(Fixture *fixture, gconstpointer unused)
{
    (void)unused; const gchar *main = g_getenv("RADIOS_TEST_MAIN_SINK");
    if (!main) { g_test_skip("Owned virtual Pulse sinks require tests/run_audio_ui.py --engine-only"); return; }
    wave(fixture, 480000); RadioSAudioEngine *engine = radios_audio_engine_new(TRUE, NULL); RadioSAudioPreferences prefs; GError *error = NULL;
    radios_audio_preferences_default(&prefs);
    g_strlcpy(prefs.main_sink, main, sizeof prefs.main_sink);
    g_strlcpy(prefs.cue_sink, g_getenv("RADIOS_TEST_CUE_SINK"), sizeof prefs.cue_sink);
    g_strlcpy(prefs.monitor_sink, g_getenv("RADIOS_TEST_MONITOR_SINK"), sizeof prefs.monitor_sink);
    g_strlcpy(prefs.microphone_source, g_getenv("RADIOS_TEST_MIC_SOURCE"), sizeof prefs.microphone_source);
    g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL)); pulse_ready(engine, TRUE);
    load(engine, 0, fixture, 0, 0); ready(engine, 0, 1000); pulse_ready(engine, TRUE); play(engine, 0);
    RadioSAudioSnapshot snapshot; gint64 until = g_get_monotonic_time() + 5000000;
    do { radios_audio_engine_snapshot(engine, &snapshot); if (snapshot.decks[0].cursor_frames > 4800) break; g_usleep(10000); } while (g_get_monotonic_time() < until);
    g_assert_cmpuint(snapshot.decks[0].cursor_frames, >, 4800); const guint64 generation = snapshot.decks[0].generation;
    g_assert_true(radios_audio_engine_set_microphone_gate(engine, TRUE, &error)); g_assert_no_error(error);
    g_assert_true(radios_audio_engine_set_microphone_ptt(engine, TRUE, &error)); g_assert_no_error(error);
    g_assert_true(radios_audio_engine_set_microphone_ptt(engine, FALSE, &error)); g_assert_no_error(error);
    microphone_state(engine, TRUE, TRUE, FALSE);
    g_assert_true(radios_audio_engine_set_microphone_ptt(engine, TRUE, &error)); g_assert_no_error(error);
    microphone_state(engine, TRUE, TRUE, TRUE);
    radios_audio_engine_snapshot(engine, &snapshot); guint64 cursor = snapshot.decks[0].cursor_frames;
    g_assert_cmpint(snapshot.decks[0].state, ==, RADIOS_AUDIO_PLAYING); g_assert_cmpuint(snapshot.decks[0].generation, ==, generation);
    g_strlcpy(prefs.microphone_source, "radios-nonexistent-microphone", sizeof prefs.microphone_source);
    g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL));
    microphone_state(engine, FALSE, FALSE, FALSE);
    until = g_get_monotonic_time() + 5000000;
    do { radios_audio_engine_snapshot(engine, &snapshot); if (snapshot.buses[0].ready && !snapshot.microphone_ready && snapshot.decks[0].cursor_frames > cursor + 4800) break; g_usleep(10000); } while (g_get_monotonic_time() < until);
    g_assert_true(snapshot.buses[0].ready); g_assert_false(snapshot.microphone_ready);
    microphone_state(engine, FALSE, FALSE, FALSE);
    g_assert_cmpint(snapshot.decks[0].state, ==, RADIOS_AUDIO_PLAYING); g_assert_cmpuint(snapshot.decks[0].generation, ==, generation);
    g_assert_cmpuint(snapshot.decks[0].cursor_frames, >, cursor + 4800);
    g_assert_false(radios_audio_engine_set_microphone_ptt(engine, TRUE, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_UNAVAILABLE); g_clear_error(&error);
    microphone_state(engine, FALSE, FALSE, FALSE);
    radios_audio_engine_unref(engine);
}

/* radios_audio_ui_set_channel_on_air reads the manual mixer, changes only the
 * strip's mute and applies it. Even while automatic levelling drives the
 * effective fader, that round trip must keep the operator's manual fader:
 * once levelling is bypassed the manual -12 dB is back. */
static void on_air_mute_keeps_manual_fader(Fixture *fixture, gconstpointer unused)
{
    (void)unused; wave(fixture, 40000); RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioPreferences prefs; radios_audio_preferences_default(&prefs); prefs.loudness_normalize = TRUE;
    g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL));
    load(engine, 0, fixture, 0, 0); ready(engine, 0, 4096);
    RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
    RadioSLoudness measured = {.integrated_lufs = -8, .peak = .25, .measured = TRUE};
    g_assert_true(radios_audio_engine_set_deck_loudness(engine, 0, snapshot.decks[0].generation, &measured, NULL));
    RadioSAudioMixerConfig mixer; radios_audio_engine_get_mixer(engine, &mixer);
    mixer.channels[0].source_deck = -1; mixer.channels[7].source_deck = 0;
    mixer.channels[7].gain_db = -12; mixer.channels[7].loudness_bypass = TRUE;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, NULL));
    radios_audio_engine_get_mixer(engine, &mixer); mixer.channels[7].loudness_bypass = FALSE;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, NULL)); play(engine, 0);
    gfloat output[3][8192]; render(engine, 512, output);
    radios_audio_engine_snapshot(engine, &snapshot); g_assert_true(snapshot.dsp_meters.channel_loudness_active[7]);
    for (guint step = 0; step < 4; ++step) {
        const gboolean on_air = step % 2 == 1;
        RadioSAudioMixerConfig manual; radios_audio_engine_get_manual_mixer(engine, &manual);
        close_to(manual.channels[7].gain_db, -12); manual.channels[7].mute = !on_air;
        g_assert_true(radios_audio_engine_apply_mixer(engine, &manual, NULL)); render(engine, 512, output);
        radios_audio_engine_get_manual_mixer(engine, &manual);
        close_to(manual.channels[7].gain_db, -12); g_assert_cmpint(manual.channels[7].mute, ==, !on_air);
        radios_audio_engine_snapshot(engine, &snapshot); g_assert_cmpint(snapshot.mixer.channels[7].mute, ==, !on_air);
        g_assert_cmpint(snapshot.decks[0].state, ==, RADIOS_AUDIO_PLAYING);
        if (!on_air) close_to(output[0][1022], 0); else g_assert_cmpfloat(fabsf(output[0][1022]), >, .01f);
    }
    radios_audio_engine_get_mixer(engine, &mixer); mixer.channels[7].loudness_bypass = TRUE;
    g_assert_true(radios_audio_engine_apply_mixer(engine, &mixer, NULL)); render(engine, 512, output);
    radios_audio_engine_snapshot(engine, &snapshot); close_to(snapshot.mixer.channels[7].gain_db, -12);
    close_to(output[0][1022], .25 * pow(10, -12.0 / 20)); radios_audio_engine_unref(engine);
}

/* Routing applies from the next render block: a window applying every deck's
 * route causes no output flush at all, and the MAIN deck keeps playing. Seeks
 * of the playing deck do need a flush; a burst of them settles into one. */
static void pulse_routing_burst_single_flush(Fixture *fixture, gconstpointer unused)
{
    (void)unused; const gchar *main = g_getenv("RADIOS_TEST_MAIN_SINK");
    if (!main) { g_test_skip("Owned virtual Pulse sinks require tests/run_audio_ui.py --engine-only"); return; }
    wave(fixture, 480000); RadioSAudioEngine *engine = radios_audio_engine_new(TRUE, NULL); RadioSAudioPreferences prefs;
    radios_audio_preferences_default(&prefs);
    g_strlcpy(prefs.main_sink, main, sizeof prefs.main_sink);
    g_strlcpy(prefs.cue_sink, g_getenv("RADIOS_TEST_CUE_SINK"), sizeof prefs.cue_sink);
    g_strlcpy(prefs.monitor_sink, g_getenv("RADIOS_TEST_MONITOR_SINK"), sizeof prefs.monitor_sink);
    g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL)); pulse_ready(engine, FALSE);
    load(engine, 0, fixture, 0, 0); ready(engine, 0, 1000); pulse_ready(engine, FALSE); play(engine, 0);
    RadioSAudioSnapshot snapshot; gint64 until = g_get_monotonic_time() + 5000000;
    do { radios_audio_engine_snapshot(engine, &snapshot); if (snapshot.decks[0].cursor_frames > 4800) break; g_usleep(10000); } while (g_get_monotonic_time() < until);
    g_assert_cmpuint(snapshot.decks[0].cursor_frames, >, 4800);
    for (guint round = 0; round < 2; ++round) {
        guint64 before = radios_audio_engine_output_flushes(engine);
        for (guint deck = 1; deck <= 12; ++deck) {
            g_assert_true(radios_audio_engine_set_deck_main(engine, deck, round == 1, NULL)); g_usleep(3300);
        }
        g_usleep(300000);
        g_assert_cmpuint(radios_audio_engine_output_flushes(engine) - before, ==, 0);
    }
    /* Loading and stopping an idle deck do not disturb the outputs either. */
    guint64 before = radios_audio_engine_output_flushes(engine);
    load(engine, 5, fixture, 0, 0); ready(engine, 5, 1000); g_assert_true(radios_audio_engine_stop(engine, 5, NULL)); g_usleep(300000);
    g_assert_cmpuint(radios_audio_engine_output_flushes(engine) - before, ==, 0);
    radios_audio_engine_snapshot(engine, &snapshot); guint64 generation = snapshot.decks[0].generation;
    before = radios_audio_engine_output_flushes(engine);
    for (guint i = 0; i < 12; ++i) {
        g_assert_true(radios_audio_engine_seek_if_generation(engine, 0, generation, 48000 + i * 4800, NULL)); ++generation; g_usleep(3300);
    }
    g_usleep(300000); pulse_ready(engine, FALSE);
    guint64 flushes = radios_audio_engine_output_flushes(engine) - before;
    g_test_message("12 seeks of the playing deck 3.3 ms apart: %" G_GUINT64_FORMAT " output flush(es)", flushes);
    g_assert_cmpuint(flushes, >=, 1); g_assert_cmpuint(flushes, <=, 2);
    before = radios_audio_engine_output_flushes(engine);
    g_assert_true(radios_audio_engine_seek(engine, 0, 240000, NULL)); g_usleep(300000); pulse_ready(engine, FALSE);
    g_assert_cmpuint(radios_audio_engine_output_flushes(engine) - before, ==, 1);
    radios_audio_engine_snapshot(engine, &snapshot); guint64 cursor = snapshot.decks[0].cursor_frames; g_usleep(200000);
    radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_cmpint(snapshot.decks[0].state, ==, RADIOS_AUDIO_PLAYING); g_assert_cmpuint(snapshot.decks[0].cursor_frames, >, cursor + 4800);
    g_assert_cmpuint(snapshot.decks[0].cursor_frames, >=, 240000);
    radios_audio_engine_unref(engine);
    RadioSAudioEngine *offline = radios_audio_engine_new(FALSE, NULL);
    g_assert_true(radios_audio_engine_set_deck_main(offline, 1, FALSE, NULL));
    g_assert_cmpuint(radios_audio_engine_output_flushes(offline), ==, 0); radios_audio_engine_unref(offline);
}
static goffset capture_size(const gchar *path)
{ GStatBuf info; return path && g_stat(path, &info) == 0 ? (goffset)info.st_size : 0; }
/* Longest run of MAIN capture frames (float32 stereo) carrying no programme:
 * a flush or a stalled output shows as consecutive silent frames. */
static guint capture_longest_silence(const gchar *path, goffset offset, guint *frames)
{
    gchar *bytes = NULL; gsize length = 0; g_assert_true(g_file_get_contents(path, &bytes, &length, NULL));
    offset -= offset % 8; const gfloat *pcm = (const gfloat *)(bytes + offset); guint count = (guint)((length - (gsize)offset) / 8), run = 0, longest = 0;
    for (guint i = 0; i < count; ++i) {
        if (fabsf(pcm[i * 2]) < .01f && fabsf(pcm[i * 2 + 1]) < .01f) longest = MAX(longest, ++run); else run = 0;
    }
    g_free(bytes); *frames = count; return longest;
}
/* L/R must not cut the playing song (PDF p.8, p.15): opening or closing the
 * microphone gate, latched or momentary, leaves MAIN's programme PCM without
 * holes (at most one 10 ms render block) and the deck's clock running. */
static void pulse_microphone_gate_keeps_programme(Fixture *fixture, gconstpointer unused)
{
    (void)unused; const gchar *main = g_getenv("RADIOS_TEST_MAIN_SINK"), *capture = g_getenv("RADIOS_TEST_MAIN_CAPTURE");
    if (!main || !capture) { g_test_skip("Owned virtual Pulse sinks and captures require tests/run_audio_ui.py --engine-only"); return; }
    wave(fixture, 960000); RadioSAudioEngine *engine = radios_audio_engine_new(TRUE, NULL); RadioSAudioPreferences prefs; GError *error = NULL;
    radios_audio_preferences_default(&prefs); prefs.loudness_normalize = FALSE;
    g_strlcpy(prefs.main_sink, main, sizeof prefs.main_sink);
    g_strlcpy(prefs.cue_sink, g_getenv("RADIOS_TEST_CUE_SINK"), sizeof prefs.cue_sink);
    g_strlcpy(prefs.monitor_sink, g_getenv("RADIOS_TEST_MONITOR_SINK"), sizeof prefs.monitor_sink);
    g_strlcpy(prefs.microphone_source, g_getenv("RADIOS_TEST_MIC_SOURCE"), sizeof prefs.microphone_source);
    g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL)); pulse_ready(engine, TRUE);
    load(engine, 0, fixture, 0, 0); ready(engine, 0, 1000); pulse_ready(engine, TRUE); play(engine, 0);
    RadioSAudioSnapshot snapshot; gint64 until = g_get_monotonic_time() + 5000000;
    do { radios_audio_engine_snapshot(engine, &snapshot); if (snapshot.decks[0].cursor_frames > 24000) break; g_usleep(10000); } while (g_get_monotonic_time() < until);
    g_usleep(300000); radios_audio_engine_snapshot(engine, &snapshot);
    const guint64 generation = snapshot.decks[0].generation, cursor = snapshot.decks[0].cursor_frames;
    const goffset offset = capture_size(capture); const gint64 start = g_get_monotonic_time();
    const guint64 flushes = radios_audio_engine_output_flushes(engine);
    /* latch on/off, push-to-talk alone, push-to-talk over the latch */
    const struct { gboolean latch_step, value; } steps[] = {
        {TRUE, TRUE}, {TRUE, FALSE}, {FALSE, TRUE}, {FALSE, FALSE}, {TRUE, TRUE}, {FALSE, TRUE}, {FALSE, FALSE}, {TRUE, FALSE}
    };
    for (guint i = 0; i < G_N_ELEMENTS(steps); ++i) {
        gboolean accepted = steps[i].latch_step ? radios_audio_engine_set_microphone_gate(engine, steps[i].value, &error) :
            radios_audio_engine_set_microphone_ptt(engine, steps[i].value, &error);
        g_assert_no_error(error); g_assert_true(accepted); g_usleep(250000);
    }
    g_usleep(300000); radios_audio_engine_snapshot(engine, &snapshot);
    const gint64 elapsed = g_get_monotonic_time() - start; guint frames = 0;
    const guint silence = capture_longest_silence(capture, offset, &frames);
    const guint64 advanced = snapshot.decks[0].cursor_frames - cursor;
    g_test_message("MAIN gate sequence: longest silence %u frames (%.1f ms) in %u captured frames; deck advanced %.0f ms in %.0f ms; %" G_GUINT64_FORMAT " output flush(es)",
        silence, silence / 48.0, frames, advanced / 48.0, elapsed / 1000.0, radios_audio_engine_output_flushes(engine) - flushes);
    g_assert_cmpuint(frames, >, 48000);
    g_assert_cmpuint(silence, <=, 480);
    g_assert_cmpint(snapshot.decks[0].state, ==, RADIOS_AUDIO_PLAYING); g_assert_cmpuint(snapshot.decks[0].generation, ==, generation);
    g_assert_cmpuint(advanced, >=, (guint64)((elapsed - 100000) * 48 / 1000));
    microphone_state(engine, FALSE, FALSE, FALSE); radios_audio_engine_unref(engine);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/audio/loudness-selected-player-schedule-fader-PCM", Fixture, NULL, setup, loudness_player_policy_pcm, teardown);
    g_test_add("/audio/loudness-preview-legacy-and-custom-scope-PCM", Fixture, NULL, setup, loudness_preview_legacy_pcm, teardown);
    g_test_add("/audio/loudness-mixer-remapping-bypass-fader-PCM", Fixture, NULL, setup, loudness_mixer_channel_pcm, teardown);
    g_test_add("/audio/loudness-auto-positive-residual-player-trim-no-clip-PCM", Fixture, NULL, setup, loudness_auto_positive_residual_and_player_gain_pcm, teardown);
    g_test_add("/audio/mix", Fixture, NULL, setup, mix, teardown);
    g_test_add("/audio/transport", Fixture, NULL, setup, transport, teardown);
    g_test_add("/audio/microphone-latch-ptt-keeps-transport", Fixture, NULL, setup, microphone_latch_ptt, teardown);
    g_test_add("/audio/on-air-mute-keeps-manual-fader", Fixture, NULL, setup, on_air_mute_keeps_manual_fader, teardown);
    g_test_add("/audio/pulse-microphone-loss-closes-latch-and-ptt", Fixture, NULL, setup, pulse_microphone_loss, teardown);
    g_test_add("/audio/pulse-routing-no-flush-seek-burst-single-flush", Fixture, NULL, setup, pulse_routing_burst_single_flush, teardown);
    g_test_add("/audio/pulse-microphone-gate-keeps-programme", Fixture, NULL, setup, pulse_microphone_gate_keeps_programme, teardown);
    g_test_add("/audio/preview-isolation", Fixture, NULL, setup, preview, teardown);
    g_test_add("/audio/validation", Fixture, NULL, setup, validation, teardown);
    g_test_add("/audio/replacement", Fixture, NULL, setup, replacement, teardown);
    g_test_add("/audio/33-decks", Fixture, NULL, setup, all_decks, teardown);
    g_test_add("/audio/sampler-voice-isolation", Fixture, NULL, setup, sampler_voices, teardown);
    g_test_add("/audio/transient-unload", Fixture, NULL, setup, transient_unload, teardown);
    g_test_add("/audio/mixer-graph-pfl", Fixture, NULL, setup, mixer_graph, teardown);
    g_test_add("/audio/pulse", Fixture, NULL, setup, pulse, teardown);
    g_test_add_func("/audio/external-main-session", external_main);
    g_test_add_func("/audio/external-stop-clears-master-tail", external_tail);
    return g_test_run();
}
