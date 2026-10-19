#include "radios/audio.h"
#include "radios/audio_tempo.h"
#include "radios/audio_runner.h"
#include <glib/gstdio.h>
#include <math.h>
#include <locale.h>
#include "radios_test_support.h"

typedef struct { gchar *directory, *path, *sha, *media, *version; } TempoFixture;
static void word(GByteArray *bytes, guint value, guint width)
{
    for (guint index = 0; index < width; ++index) { guchar byte = (guchar)(value >> (index * 8)); g_byte_array_append(bytes, &byte, 1); }
}
static void setup(TempoFixture *fixture, gconstpointer unused)
{
    (void)unused;
    fixture->directory = g_build_filename(RADIOS_TEST_PROJECT_ROOT, "build", "tempo-XXXXXX", NULL);
    g_assert_nonnull(g_mkdtemp(fixture->directory)); fixture->path = g_build_filename(fixture->directory, "tone.wav", NULL);
    GByteArray *bytes = g_byte_array_new(); guint size = 48000 * 4 * 4;
    g_byte_array_append(bytes, (const guchar *)"RIFF", 4); word(bytes, size + 36, 4);
    g_byte_array_append(bytes, (const guchar *)"WAVEfmt ", 8); word(bytes, 16, 4); word(bytes, 1, 2); word(bytes, 2, 2);
    word(bytes, 48000, 4); word(bytes, 192000, 4); word(bytes, 4, 2); word(bytes, 16, 2);
    g_byte_array_append(bytes, (const guchar *)"data", 4); word(bytes, size, 4);
    for (guint frame = 0; frame < 48000 * 4; ++frame) {
        gint16 sample = (gint16)lrint(14000 * sin(2 * G_PI * 997 * frame / 48000));
        word(bytes, (guint16)sample, 2); word(bytes, (guint16)sample, 2);
    }
    g_assert_true(g_file_set_contents(fixture->path, (gchar *)bytes->data, bytes->len, NULL));
    fixture->sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes->data, bytes->len); g_byte_array_unref(bytes);
    fixture->media = g_uuid_string_random(); fixture->version = g_uuid_string_random();
}
static void teardown(TempoFixture *fixture, gconstpointer unused)
{
    (void)unused; g_assert_cmpint(g_unlink(fixture->path), ==, 0); g_assert_cmpint(g_rmdir(fixture->directory), ==, 0);
    g_free(fixture->directory); g_free(fixture->path); g_free(fixture->sha); g_free(fixture->media); g_free(fixture->version);
}
static void assert_pitch_and_duration(TempoFixture *fixture, gdouble ratio)
{
    GError *error = NULL; RadioSAudioDecoder *decoder = radios_audio_decoder_open(fixture->path, fixture->sha, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(decoder);
    RadioSAudioTempo *tempo = radios_audio_tempo_new(ratio, 48000 * 3, &error); g_assert_no_error(error);
    gfloat pcm[4096 * 2], previous = 0; guint64 total = 0, crossings = 0, measured = 0;
    gssize frames;
    while ((frames = radios_audio_tempo_read(tempo, decoder, pcm, 4096, NULL, &error)) > 0) {
        for (guint frame = 0; frame < (guint)frames; ++frame) {
            gfloat sample = pcm[frame * 2]; g_assert_true(isfinite(sample));
            g_assert_cmpfloat(fabsf(sample - pcm[frame * 2 + 1]), <, 0.00001);
            if (total + frame >= 24000 && total + frame < 96000) {
                if (previous < 0 && sample >= 0) ++crossings;
                ++measured;
            }
            previous = sample;
        }
        total += (guint)frames;
    }
    g_assert_no_error(error); g_assert_cmpint(frames, ==, 0);
    g_assert_cmpuint(total, ==, (guint64)ceil(48000 * 3 / ratio));
    gdouble frequency = crossings * 48000.0 / measured;
    g_test_message("rate %.3f, frames %" G_GUINT64_FORMAT ", pitch %.3f Hz", ratio, total, frequency);
    g_assert_cmpfloat(fabs(frequency - 997), <, 2);
    GCancellable *cancel = g_cancellable_new(); g_cancellable_cancel(cancel);
    radios_audio_tempo_free(tempo); tempo = radios_audio_tempo_new(ratio, 48000, NULL);
    g_assert_cmpint(radios_audio_tempo_read(tempo, decoder, pcm, 100, cancel, &error), ==, -1);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED); g_clear_error(&error); g_object_unref(cancel);
    radios_audio_tempo_free(tempo); radios_audio_decoder_close(decoder);
}
static void pitch_preserved(TempoFixture *fixture, gconstpointer unused)
{
    (void)unused; const gdouble ratios[] = {0.84, 0.9, 1.1, 1.16};
    for (guint index = 0; index < G_N_ELEMENTS(ratios); ++index)
        assert_pitch_and_duration(fixture, ratios[index]);
    GError *error = NULL; g_assert_null(radios_audio_tempo_new(NAN, 0, &error)); g_clear_error(&error);
}
static void fractional_numeric_locale(TempoFixture *fixture, gconstpointer setting)
{
    const gchar *name = setting;
    /* Thread-local LC_NUMERIC (UCRT per-thread locale). The Turkish case must
     * exercise a real comma locale, never silently skip. */
    RtNumericLocale *previous_locale = rt_numeric_locale_push(name);
    g_assert_nonnull(previous_locale);
    g_assert_cmpstr(localeconv()->decimal_point, ==, g_str_has_prefix(name, "tr_") ? "," : ".");
    assert_pitch_and_duration(fixture, 0.9375);
    assert_pitch_and_duration(fixture, 1.125);
    rt_numeric_locale_pop(previous_locale);
}
static RadioSAudioDeckSnapshot wait_deck(RadioSAudioEngine *engine, guint deck, guint frames)
{
    RadioSAudioSnapshot snapshot; gint64 until = g_get_monotonic_time() + 5000000;
    do {
        radios_audio_engine_snapshot(engine, &snapshot);
        g_assert_cmpint(snapshot.decks[deck].state, !=, RADIOS_AUDIO_ERROR);
        if (snapshot.decks[deck].state != RADIOS_AUDIO_LOADING && snapshot.decks[deck].buffered_frames >= frames) return snapshot.decks[deck];
        g_usleep(1000);
    } while (g_get_monotonic_time() < until);
    g_error("tempo deck did not buffer"); return snapshot.decks[deck];
}
static void sync_and_clock(TempoFixture *fixture, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioLoadSpec spec = {fixture->path, fixture->sha, fixture->media, fixture->version, 0, 192000};
    for (guint deck = 0; deck < 2; ++deck) {
        g_assert_true(radios_audio_engine_load(engine, deck, &spec, NULL));
        RadioSAudioDeckSnapshot snapshot = wait_deck(engine, deck, 16000);
        RadioSRhythmAnalysis grid = {.bpm = deck ? 128 : 120, .first_beat_ms = deck ? 75 : 125, .confidence = 0.9, .analyzed_ms = 4000};
        g_assert_true(radios_audio_engine_set_deck_rhythm(engine, deck, snapshot.generation, &grid, NULL));
    }
    g_assert_true(radios_audio_engine_play(engine, 0, NULL));
    gfloat pcm[3][8192]; for (guint block = 0; block < 3; ++block) g_assert_true(radios_audio_engine_render(engine, 4000, pcm[0], pcm[1], pcm[2], NULL));
    g_assert_true(radios_audio_engine_play(engine, 1, NULL));
    g_assert_true(radios_audio_engine_sync(engine, 1, 0, TRUE, NULL));
    RadioSAudioDeckSnapshot synced = wait_deck(engine, 1, 16000);
    g_assert_cmpfloat(synced.tempo_ratio, ==, 0.9375); g_assert_cmpfloat(synced.effective_bpm, ==, 120);
    g_assert_cmpint(synced.sync_master, ==, 0);
    gdouble phase = remainder((synced.cursor_frames - 75 * 48.0) / 22500 - (12000 - 125 * 48.0) / 24000, 1);
    g_assert_cmpfloat(fabs(phase), <, 0.0001);
    GError *error = NULL;
    g_assert_false(radios_audio_engine_sync(engine, 0, 1, TRUE, &error)); g_assert_nonnull(error); g_clear_error(&error);
    g_assert_true(radios_audio_engine_play(engine, 1, NULL));
    for (guint block = 0; block < 3; ++block) g_assert_true(radios_audio_engine_render(engine, 4000, pcm[0], pcm[1], pcm[2], NULL));
    RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_cmpuint(snapshot.decks[1].cursor_frames - synced.cursor_frames, ==, 11250);
    g_assert_true(radios_audio_engine_pause(engine, 1, NULL));
    /* Pause itself releases operator SYNC and rebuilds native-tempo PCM. */
    synced = wait_deck(engine, 1, 4096); g_assert_cmpfloat(synced.tempo_ratio, ==, 1);
    g_assert_cmpint(synced.sync_master, ==, -1); g_assert_cmpint(synced.state, !=, RADIOS_AUDIO_PLAYING);
    g_assert_true(radios_audio_engine_unload(engine, 1, NULL)); radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_cmpfloat(snapshot.decks[1].bpm, ==, 0); g_assert_cmpint(snapshot.decks[1].sync_master, ==, -1);
    radios_audio_engine_unref(engine);
}
/* Applying SYNC to a playing source reloads tempo-converted PCM while its
 * master moves on. The render gate skips missed audio and meets the real grid. */
static void sync_playing_reload_matches_grid(TempoFixture *fixture, gconstpointer setting)
{
    gboolean bars = GPOINTER_TO_INT(setting);
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioLoadSpec spec = {fixture->path, fixture->sha, fixture->media, fixture->version, 0, 192000};
    RadioSRhythmAnalysis grids[2] = {
        {.bpm = 120, .first_beat_ms = 125, .confidence = 0.9, .analyzed_ms = 4000, .downbeat_ms = 1125, .has_downbeat = bars},
        {.bpm = 128, .first_beat_ms = 75, .confidence = 0.9, .analyzed_ms = 4000, .downbeat_ms = 1481.25, .has_downbeat = bars}};
    for (guint deck = 0; deck < 2; ++deck) {
        g_assert_true(radios_audio_engine_load(engine, deck, &spec, NULL));
        RadioSAudioDeckSnapshot snapshot = wait_deck(engine, deck, 16000);
        g_assert_true(radios_audio_engine_set_deck_rhythm(engine, deck, snapshot.generation, &grids[deck], NULL));
        g_assert_true(radios_audio_engine_play(engine, deck, NULL));
    }
    gfloat pcm[3][8192];
    for (guint block = 0; block < 3; ++block) g_assert_true(radios_audio_engine_render(engine, 4000, pcm[0], pcm[1], pcm[2], NULL));
    g_assert_true(radios_audio_engine_sync(engine, 1, 0, TRUE, NULL));
    /* Source is decoding; the master keeps advancing in the real renderer. */
    wait_deck(engine, 0, 3000);
    g_assert_true(radios_audio_engine_render(engine, 3000, pcm[0], pcm[1], pcm[2], NULL));
    gdouble peak = 0;
    for (guint block = 0; block < 16; ++block) {
        wait_deck(engine, 0, 4000); wait_deck(engine, 1, 4000);
        g_assert_true(radios_audio_engine_render(engine, 500, pcm[0], pcm[1], pcm[2], NULL));
        for (guint frame = 0; frame < 500; ++frame) peak = MAX(peak, fabsf(pcm[0][frame * 2]));
    }
    RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_cmpint(snapshot.decks[1].state, ==, RADIOS_AUDIO_PLAYING);
    g_assert_cmpfloat(snapshot.decks[1].effective_bpm, ==, 120);
    g_assert_cmpfloat(peak, >, 0.1);
    gdouble master = (snapshot.decks[0].cursor_frames - (bars ? 1125 : 125) * 48.0) * 120 / 2880000;
    gdouble follower = (snapshot.decks[1].cursor_frames - (bars ? 1481.25 : 75) * 48.0) * 128 / 2880000;
    g_test_message("bars=%d playing PCM peak=%.3f phase=%.6f", bars, peak, remainder(follower - master, 1));
    g_assert_cmpfloat(fabs(remainder(follower - master, 1)), <, 0.0003);
    radios_audio_engine_unref(engine);
}
static void sync_inactive_rejected_and_stop_releases(TempoFixture *fixture, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioLoadSpec spec = {fixture->path, fixture->sha, fixture->media, fixture->version, 0, 192000};
    for (guint deck = 0; deck < 2; ++deck) {
        g_assert_true(radios_audio_engine_load(engine, deck, &spec, NULL));
        RadioSAudioDeckSnapshot loaded = wait_deck(engine, deck, 16000);
        RadioSRhythmAnalysis grid = {.bpm = deck ? 128 : 120, .confidence = 0.9, .analyzed_ms = 4000};
        g_assert_true(radios_audio_engine_set_deck_rhythm(engine, deck, loaded.generation, &grid, NULL));
    }
    GError *error = NULL; RadioSAudioSnapshot before, after;
    radios_audio_engine_snapshot(engine, &before);
    g_assert_false(radios_audio_engine_sync(engine, 1, 0, TRUE, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_STATE); g_clear_error(&error);
    radios_audio_engine_snapshot(engine, &after);
    g_assert_cmpuint(after.decks[1].generation, ==, before.decks[1].generation);
    g_assert_cmpint(after.decks[1].state, ==, RADIOS_AUDIO_READY);
    g_assert_cmpint(after.decks[1].sync_master, ==, -1);
    g_assert_true(radios_audio_engine_play(engine, 1, NULL));
    g_assert_true(radios_audio_engine_pause(engine, 1, NULL));
    g_assert_false(radios_audio_engine_sync(engine, 1, 0, TRUE, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_STATE); g_clear_error(&error);
    g_assert_true(radios_audio_engine_play(engine, 1, NULL));
    g_assert_true(radios_audio_engine_sync(engine, 1, 0, TRUE, NULL));
    RadioSAudioDeckSnapshot synced = wait_deck(engine, 1, 16000);
    g_assert_cmpfloat(synced.tempo_ratio, ==, 0.9375);
    g_assert_true(radios_audio_engine_stop(engine, 1, NULL));
    RadioSAudioDeckSnapshot stopped = wait_deck(engine, 1, 16000);
    g_assert_cmpuint(stopped.cursor_frames, ==, 0); g_assert_cmpint(stopped.state, ==, RADIOS_AUDIO_READY);
    g_assert_cmpint(stopped.sync_master, ==, -1); g_assert_cmpfloat(stopped.tempo_ratio, ==, 1);
    g_assert_true(radios_audio_engine_play(engine, 1, NULL));
    gfloat pcm[3][8192]; gdouble peak = 0;
    g_assert_true(radios_audio_engine_render(engine, 4000, pcm[0], pcm[1], pcm[2], NULL));
    for (guint frame = 0; frame < 4000; ++frame) peak = MAX(peak, fabsf(pcm[0][frame * 2]));
    radios_audio_engine_snapshot(engine, &after);
    g_assert_cmpuint(after.decks[1].cursor_frames, ==, 4000); g_assert_cmpfloat(peak, >, 0.1);
    radios_audio_engine_unref(engine);
}
static void sync_owned_source_pause_releases_manual_clock(TempoFixture *fixture, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioLoadSpec media = {fixture->path, fixture->sha, fixture->media, fixture->version, 0, 192000};
    g_assert_true(radios_audio_engine_load(engine, 1, &media, NULL));
    RadioSAudioDeckSnapshot reference = wait_deck(engine, 1, 16000);
    RadioSRhythmAnalysis master = {.bpm = 120, .confidence = .9, .analyzed_ms = 4000};
    g_assert_true(radios_audio_engine_set_deck_rhythm(engine, 1, reference.generation, &master, NULL));
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options); options.pool_count = 1;
    gchar *entry = g_uuid_string_random();
    RadioSAudioRunItem item = {.entry_id = entry, .media = media, .duration_frames = 192000, .preferred_deck = 0,
        .rhythm = {.bpm = 128, .confidence = .9, .analyzed_ms = 4000}};
    g_assert_true(radios_audio_engine_scope_runner_configure(engine, 0, &item, 1, &options, NULL));
    g_assert_true(radios_audio_engine_scope_runner_start_replacing(engine, 0, -1, 0, NULL));
    wait_deck(engine, 0, 16000);
    gfloat pcm[3][8192]; g_assert_true(radios_audio_engine_render(engine, 1000, pcm[0], pcm[1], pcm[2], NULL));
    g_assert_true(radios_audio_engine_sync(engine, 0, 1, TRUE, NULL));
    wait_deck(engine, 0, 16000);
    g_assert_true(radios_audio_engine_render(engine, 1000, pcm[0], pcm[1], pcm[2], NULL));
    g_assert_true(radios_audio_engine_player_runner_action(engine, 0, RADIOS_AUDIO_PLAYER_RUN_PAUSE, NULL));
    RadioSAudioDeckSnapshot paused = wait_deck(engine, 0, 16000);
    g_assert_cmpint(paused.state, ==, RADIOS_AUDIO_PAUSED); g_assert_cmpint(paused.sync_master, ==, -1);
    g_assert_cmpfloat(paused.tempo_ratio, ==, 1);
    GError *error = NULL; g_assert_false(radios_audio_engine_sync(engine, 0, 1, TRUE, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_STATE); g_clear_error(&error);
    g_assert_true(radios_audio_engine_player_runner_action(engine, 0, RADIOS_AUDIO_PLAYER_RUN_RESUME, NULL));
    g_assert_true(radios_audio_engine_render(engine, 1000, pcm[0], pcm[1], pcm[2], NULL));
    RadioSAudioSnapshot after; radios_audio_engine_snapshot(engine, &after);
    g_assert_cmpuint(after.decks[0].cursor_frames, ==, paused.cursor_frames + 1000);
    g_assert_cmpint(after.decks[0].state, ==, RADIOS_AUDIO_PLAYING); g_assert_cmpfloat(after.decks[0].tempo_ratio, ==, 1);
    radios_audio_engine_unref(engine); g_free(entry);
}
static gfloat render_peak(RadioSAudioEngine *engine, guint frames)
{
    gfloat pcm[3][8192], peak = 0;
    wait_deck(engine, 0, frames);
    g_assert_true(radios_audio_engine_render(engine, frames, pcm[0], pcm[1], pcm[2], NULL));
    for (guint index = frames; index < frames * 2; ++index) peak = MAX(peak, fabsf(pcm[0][index]));
    return peak;
}
/* A measured deck plays at the target loudness: cuts fully, boosts only up to
 * a -1 dBFS sample peak; switching normalization off restores the file level. */
static void loudness_normalizes_deck(TempoFixture *fixture, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioLoadSpec spec = {fixture->path, fixture->sha, fixture->media, fixture->version, 0, 192000};
    g_assert_true(radios_audio_engine_load(engine, 0, &spec, NULL));
    RadioSAudioDeckSnapshot loaded = wait_deck(engine, 0, 16000);
    RadioSAudioPreferences prefs; radios_audio_preferences_default(&prefs);
    g_assert_true(prefs.loudness_normalize); g_assert_cmpfloat(prefs.loudness_target_lufs, ==, -14);
    prefs.loudness_normalize = FALSE; g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL));
    RadioSLoudness loud = {.integrated_lufs = -10, .peak = 0.43, .measured = TRUE}, quiet = loud;
    quiet.integrated_lufs = -20;
    GError *error = NULL;
    g_assert_false(radios_audio_engine_set_deck_loudness(engine, 0, loaded.generation + 1, &loud, &error)); g_clear_error(&error);
    g_assert_true(radios_audio_engine_set_deck_loudness(engine, 0, loaded.generation, &loud, NULL));
    g_assert_true(radios_audio_engine_play(engine, 0, NULL));
    gfloat plain = render_peak(engine, 2000);
    g_assert_cmpfloat(plain, >, 0.3);
    prefs.loudness_normalize = TRUE; g_assert_true(radios_audio_engine_apply_preferences(engine, &prefs, NULL));
    gfloat cut = render_peak(engine, 2000);
    RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(engine, &snapshot);
    g_assert_true(snapshot.decks[0].loudness_measured); g_assert_cmpfloat(fabs(snapshot.decks[0].loudness_gain_db + 4), <, 1e-9);
    g_assert_cmpfloat(fabs(cut / plain - pow(10, -4.0 / 20)), <, 0.01);
    g_assert_true(radios_audio_engine_set_deck_loudness(engine, 0, loaded.generation, &quiet, NULL));
    gfloat boost = render_peak(engine, 2000);
    g_test_message("plain=%.4f cut=%.4f boost=%.4f", plain, cut, boost);
    g_assert_cmpfloat(fabs(boost / plain - pow(10, 6.0 / 20)), <, 0.02);
    prefs.loudness_target_lufs = -40;
    g_assert_false(radios_audio_engine_apply_preferences(engine, &prefs, &error)); g_clear_error(&error);
    radios_audio_engine_unref(engine);
}
/* SYNC changes output-frame counts; EOF follows the source clock and must
 * consume the complete final PCM buffer for either tempo direction. */
static void sync_ends_at_source_boundary(TempoFixture *fixture, gconstpointer setting)
{
    gboolean fast = GPOINTER_TO_INT(setting);
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioLoadSpec media = {fixture->path, fixture->sha, fixture->media,
        fixture->version, 0, 12000};
    for (guint deck = 0; deck < 2; ++deck) {
        g_assert_true(radios_audio_engine_load(engine, deck, &media, NULL));
        RadioSAudioDeckSnapshot initial = wait_deck(engine, deck, 12000);
        gdouble bpm = deck == 0 ? (fast ? 128 : 120) : (fast ? 120 : 128);
        RadioSRhythmAnalysis grid = {.bpm = bpm, .confidence = .9, .analyzed_ms = 4000};
        g_assert_true(radios_audio_engine_set_deck_rhythm(engine, deck,
            initial.generation, &grid, NULL));
    }
    g_assert_true(radios_audio_engine_play(engine, 1, NULL));
    g_assert_true(radios_audio_engine_sync(engine, 1, 0, TRUE, NULL));
    guint expected = (guint)ceil(12000 / (fast ? (128.0 / 120) : (120.0 / 128)));
    RadioSAudioDeckSnapshot prepared = wait_deck(engine, 1, expected);
    g_test_message("prepared ratio=%.9f source-cue=%u output-queued=%" G_GUINT64_FORMAT,
        prepared.tempo_ratio, 12000, prepared.buffered_frames);
    g_assert_true(radios_audio_engine_play(engine, 1, NULL));
    RadioSAudioSnapshot snapshot = {0};
    gfloat pcm[3][8192];
    for (guint block = 0; block < 40; ++block) {
        g_assert_true(radios_audio_engine_render(engine, 500, pcm[0], pcm[1], pcm[2], NULL));
        radios_audio_engine_snapshot(engine, &snapshot);
        if (snapshot.decks[1].state == RADIOS_AUDIO_EOF) break;
    }
    RadioSAudioDeckSnapshot result = snapshot.decks[1];
    GError *error = NULL; g_assert_false(radios_audio_engine_sync(engine, 1, 0, TRUE, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_STATE); g_clear_error(&error);
    radios_audio_engine_unref(engine);
    g_test_message("RESULT speed=%s state=%u cursor=%" G_GUINT64_FORMAT
        " queued=%" G_GUINT64_FORMAT " cue-out=%" G_GUINT64_FORMAT,
        fast ? "fast" : "slow", result.state, result.cursor_frames,
        result.buffered_frames, result.cue_out_frame);
    g_assert_cmpint(result.state, ==, RADIOS_AUDIO_EOF);
    g_assert_cmpuint(result.buffered_frames, ==, 0);
    g_assert_cmpuint(result.cursor_frames, ==, 12000);
    g_assert_cmpint(result.sync_master, ==, -1);
}

static void runner_grid_and_adopt(TempoFixture *fixture, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options); options.deck_pool[0] = 2; options.deck_pool[1] = 3;
    gchar *ids[3]; RadioSAudioRunItem items[3] = {0};
    for (guint index = 0; index < 3; ++index) {
        ids[index] = g_uuid_string_random(); items[index].entry_id = ids[index];
        items[index].media = (RadioSAudioLoadSpec){fixture->path, fixture->sha, fixture->media, fixture->version, index ? 6000 : 1000, 192000};
        items[index].duration_frames = items[index].auto_next_frame = items[index].fade_start_frame = items[index].fade_end_frame = 192000;
        items[index].preferred_deck = index % 2 ? 3 : 2; items[index].mode = RADIOS_AUDIO_RUN_CROSSFADE; items[index].overlap_frames = 48000;
        items[index].rhythm = (RadioSRhythmAnalysis){.bpm = index % 2 ? 128 : 120, .first_beat_ms = 125, .confidence = 0.9, .analyzed_ms = 4000};
        items[index].tempo_ratio = index % 2 ? 0.9375 : 1; items[index].beat_sync = TRUE;
    }
    RadioSAudioLoadSpec manual = items[0].media; manual.cue_in_frame = 0;
    g_assert_true(radios_audio_engine_load(engine, 2, &manual, NULL)); RadioSAudioDeckSnapshot before = wait_deck(engine, 2, 16000);
    g_assert_true(radios_audio_engine_play(engine, 2, NULL)); gfloat pcm[3][8192];
    g_assert_true(radios_audio_engine_render(engine, 1000, pcm[0], pcm[1], pcm[2], NULL));
    g_assert_true(radios_audio_engine_runner_configure(engine, items, 3, &options, NULL)); GError *error = NULL;
    g_assert_false(radios_audio_engine_runner_start_adopting(engine, 2, before.generation + 1, &error)); g_assert_nonnull(error); g_clear_error(&error);
    g_assert_true(radios_audio_engine_runner_start_adopting(engine, 2, before.generation, NULL));
    RadioSRhythmAnalysis replacement = {.bpm = 130, .first_beat_ms = 125, .confidence = 0.9, .analyzed_ms = 4000};
    g_assert_false(radios_audio_engine_set_deck_rhythm(engine, 2, before.generation, &replacement, &error));
    g_assert_nonnull(error); g_clear_error(&error);
    wait_deck(engine, 3, 16000); RadioSAudioSnapshot audio; radios_audio_engine_snapshot(engine, &audio);
    g_assert_cmpuint(audio.decks[2].generation, ==, before.generation); g_assert_cmpuint(audio.decks[2].cursor_frames, ==, 1000);
    for (guint remaining = 125000; remaining;) {
        guint count = MIN(4096, remaining); g_assert_true(radios_audio_engine_render(engine, count, pcm[0], pcm[1], pcm[2], NULL)); remaining -= count;
        g_usleep(1000);
    }
    g_assert_true(radios_audio_engine_render(engine, 1, pcm[0], pcm[1], pcm[2], NULL));
    RadioSAudioRunSnapshot run; radios_audio_engine_runner_snapshot(engine, &run);
    g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_OVERLAP); g_assert_cmpint(run.deck_current, ==, 2); g_assert_cmpint(run.deck_next, ==, 3);
    g_assert_cmpuint(run.transition_frames, ==, 1); g_assert_cmpuint(run.actual_current_frame, ==, 126001); g_assert_cmpuint(run.actual_next_frame, ==, 6000);
    gboolean recycled = FALSE;
    for (guint block = 0; block < 250 && run.state != RADIOS_AUDIO_RUN_FINISHED; ++block) {
        g_assert_true(radios_audio_engine_render(engine, 4096, pcm[0], pcm[1], pcm[2], NULL)); g_usleep(1500);
        radios_audio_engine_runner_snapshot(engine, &run);
        g_assert_cmpint(run.state, !=, RADIOS_AUDIO_RUN_ERROR);
        if (run.index == 2 && run.deck_current == 2) {
            recycled = TRUE; radios_audio_engine_snapshot(engine, &audio); g_assert_cmpint(audio.decks[2].sync_master, ==, -1);
        }
    }
    g_assert_true(recycled); g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_FINISHED);
    RadioSAudioRunReceipt receipts[8]; guint count = radios_audio_engine_runner_receipts(engine, receipts, 8);
    guint completed = 0;
    for (guint index = 0; index < count; ++index) if (receipts[index].terminal == RADIOS_AUDIO_RUN_TERMINAL_COMPLETE) {
        g_assert_cmpuint(receipts[index].position_frames, ==, 192000); ++completed;
    }
    g_assert_cmpuint(completed, ==, 3);
    for (guint index = 0; index < 3; ++index) g_free(ids[index]);
    radios_audio_engine_unref(engine);
}

static void return_profile_pitch_clock(TempoFixture *fixture, gconstpointer unused)
{
    (void)unused; const gdouble ratios[] = {.84, 1.16};
    for (guint variant = 0; variant < G_N_ELEMENTS(ratios); ++variant) {
        RadioSAudioTempoProfile profile = {ratios[variant], 24000, 72000};
        for (guint output = 0; output <= 240000; output += 127) {
            gdouble source = radios_audio_tempo_profile_source(&profile, output);
            g_assert_cmpfloat(fabs(radios_audio_tempo_profile_output(&profile, source) - output), <, 1e-7);
        }
        g_assert_cmpfloat(radios_audio_tempo_profile_ratio(&profile, 24000), ==, ratios[variant]);
        g_assert_cmpfloat(radios_audio_tempo_profile_ratio(&profile, 96000), ==, 1);
        RadioSAudioDecoder *decoder = radios_audio_decoder_open(fixture->path, fixture->sha, NULL, NULL);
        GError *error = NULL; RadioSAudioTempo *tempo = radios_audio_tempo_new_profile(&profile, 0, 144000, &error);
        g_assert_no_error(error); g_assert_nonnull(tempo);
        gfloat pcm[8192], previous = 0; guint64 total = 0, crossings[3] = {0}, measured[3] = {0};
        gdouble square[3] = {0}, peak_jump = 0; gssize got;
        while ((got = radios_audio_tempo_read(tempo, decoder, pcm, 4096, NULL, &error)) > 0) {
            for (guint frame = 0; frame < (guint)got; ++frame) {
                gfloat sample = pcm[frame * 2]; g_assert_true(isfinite(sample));
                g_assert_cmpfloat(fabsf(sample - pcm[frame * 2 + 1]), <, .00001);
                if (total + frame > 4800 && total + frame < 120000) peak_jump = MAX(peak_jump, fabs(sample - previous));
                gint stage = total + frame < 24000 ? 0 : total + frame < 96000 ? 1 : 2;
                if (total + frame >= 4800 && total + frame < 120000) {
                    if (previous < 0 && sample >= 0) ++crossings[stage];
                    ++measured[stage]; square[stage] += sample * sample;
                }
                previous = sample;
            }
            total += (guint)got;
        }
        g_assert_no_error(error); g_assert_cmpint(got, ==, 0);
        g_assert_cmpuint(total, ==, (guint64)ceil(radios_audio_tempo_profile_output(&profile, 144000)));
        for (guint stage = 0; stage < 3; ++stage) {
            gdouble pitch = crossings[stage] * 48000.0 / measured[stage], rms = sqrt(square[stage] / measured[stage]);
            g_test_message("return ratio%.2f stage%u pitch%.2f rms%.4f jump%.4f", ratios[variant], stage, pitch, rms, peak_jump);
            g_assert_cmpfloat(fabs(pitch - 997), <, 4); g_assert_cmpfloat(rms, >, .28);
        }
        g_assert_cmpfloat(peak_jump, <, .08);
        radios_audio_tempo_free(tempo); radios_audio_decoder_close(decoder);
    }
}

static void return_profile_actual_click_intervals(TempoFixture *fixture, gconstpointer unused)
{
    (void)unused; gchar *wav = NULL; gsize size = 0;
    g_assert_true(g_file_get_contents(fixture->path, &wav, &size, NULL)); g_assert_cmpuint(size, ==, 44 + 192000 * 4);
    for (guint frame = 0; frame < 192000; ++frame) {
        guint phase = frame >= 6000 ? (frame - 6000) % 24000 : 24000;
        gint16 sample = phase < 4800 ? (gint16)lrint(14000 * sin(2 * G_PI * 997 * frame / 48000)) : 0;
        guint16 little = GUINT16_TO_LE((guint16)sample);
        memcpy(wav + 44 + frame * 4, &little, 2); memcpy(wav + 44 + frame * 4 + 2, &little, 2);
    }
    g_assert_true(g_file_set_contents(fixture->path, wav, size, NULL)); g_free(fixture->sha);
    fixture->sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (guchar *)wav, size); g_free(wav);
    const gdouble ratios[] = {.84, 1.16};
    for (guint variant = 0; variant < G_N_ELEMENTS(ratios) * 2; ++variant) {
        gdouble initial = ratios[variant % G_N_ELEMENTS(ratios)];
        RadioSAudioTempoProfile profile = {initial, 24000, variant < G_N_ELEMENTS(ratios) ? 72000 : 0};
        RadioSAudioDecoder *decoder = radios_audio_decoder_open(fixture->path, fixture->sha, NULL, NULL);
        GError *error = NULL; RadioSAudioTempo *tempo = radios_audio_tempo_new_profile(&profile, 0, 192000, &error);
        g_assert_no_error(error); gfloat pcm[8192]; guint64 total = 0, onsets[8] = {0}; guint count = 0; gssize got;
        while ((got = radios_audio_tempo_read(tempo, decoder, pcm, 4096, NULL, &error)) > 0) {
            for (guint frame = 0; frame < (guint)got; ++frame) {
                guint64 at = total + frame;
                if (fabsf(pcm[frame * 2]) > .2 && (!count || at - onsets[count - 1] > 16000)) {
                    g_assert_cmpuint(count, <, G_N_ELEMENTS(onsets)); onsets[count++] = at;
                }
            }
            total += (guint)got;
        }
        g_assert_no_error(error); g_assert_cmpuint(count, ==, G_N_ELEMENTS(onsets));
        for (guint i = 1; i < count; ++i) {
            gdouble actual = onsets[i] - onsets[i - 1];
            gdouble expected = radios_audio_tempo_profile_output(&profile, 6000 + i * 24000) -
                radios_audio_tempo_profile_output(&profile, 6000 + (i - 1) * 24000);
            g_test_message("return ratio%.2f click%u actual%.1f expected%.1f ms", initial, i, actual / 48, expected / 48);
            g_assert_cmpfloat(fabs(actual - expected), <, 960);
        }
        /* These acoustic intervals must return to native 120 BPM. A graph
         * left at the original matched ratio fails by 69–95 ms per beat. */
        gdouble native_interval = profile.return_frames ? 24000 : 24000 / initial;
        g_assert_cmpfloat(fabs((gdouble)(onsets[7] - onsets[6]) - native_interval), <, 960);
        radios_audio_tempo_free(tempo); radios_audio_decoder_close(decoder);
    }
}

static void runner_return_profile_pause_seek(TempoFixture *fixture, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    gchar *ids[] = {g_uuid_string_random(), g_uuid_string_random()}; RadioSAudioRunItem items[2] = {{0}};
    for (guint i = 0; i < 2; ++i) {
        items[i].entry_id = ids[i]; items[i].media = (RadioSAudioLoadSpec){fixture->path, fixture->sha, fixture->media, fixture->version, 0, i ? 192000 : 48000};
        items[i].duration_frames = 192000; items[i].auto_next_frame = items[i].media.cue_out_frame; items[i].preferred_deck = (gint)i;
        items[i].tempo_ratio = i ? .9375 : 1;
        items[i].rhythm = (RadioSRhythmAnalysis){.bpm = i ? 128 : 120, .first_beat_ms = 0, .confidence = .9, .analyzed_ms = 4000};
    }
    items[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; items[0].autodj_out_frame = 24000;
    items[0].overlap_frames = items[1].fade_in_frames = 24000;
    items[1].tempo_return_start_frames = 24000; items[1].tempo_return_frames = 48000;
    g_assert_true(radios_audio_engine_runner_configure(engine, items, 2, &options, NULL));
    g_assert_true(radios_audio_engine_runner_start(engine, NULL)); wait_deck(engine, 0, 48000); wait_deck(engine, 1, 48000);
    gfloat pcm[3][8192];
    for (guint left = 48001; left;) {
        guint frames = MIN(left, 4000u); wait_deck(engine, 1, frames);
        g_assert_true(radios_audio_engine_render(engine, frames, pcm[0], pcm[1], pcm[2], NULL)); left -= frames;
    }
    RadioSAudioRunSnapshot run; radios_audio_engine_runner_snapshot(engine, &run); g_assert_cmpuint(run.index, ==, 1);
    RadioSAudioSnapshot before, after; radios_audio_engine_snapshot(engine, &before);
    guint64 generation = before.decks[1].generation; g_assert_cmpfloat(fabs(before.decks[1].tempo_ratio - .9375), <, .00001);
    for (guint left = 24000; left;) {
        guint frames = MIN(left, 4000u); wait_deck(engine, 1, frames);
        g_assert_true(radios_audio_engine_render(engine, frames, pcm[0], pcm[1], pcm[2], NULL)); left -= frames;
    }
    radios_audio_engine_snapshot(engine, &before); g_assert_cmpfloat(fabs(before.decks[1].tempo_ratio - .96875), <, .00001);
    RadioSAudioTempoProfile profile = radios_audio_run_tempo_profile(&items[1]);
    g_assert_cmpuint(before.decks[1].cursor_frames, ==, (guint64)floor(radios_audio_tempo_profile_source(&profile, 48001)));
    g_assert_true(radios_audio_engine_runner_pause(engine, NULL));
    g_assert_true(radios_audio_engine_render(engine, 4000, pcm[0], pcm[1], pcm[2], NULL)); radios_audio_engine_snapshot(engine, &after);
    g_assert_cmpuint(after.decks[1].cursor_frames, ==, before.decks[1].cursor_frames);
    g_assert_cmpfloat(after.decks[1].tempo_ratio, ==, before.decks[1].tempo_ratio); g_assert_cmpuint(after.decks[1].generation, ==, generation);
    g_assert_true(radios_audio_engine_runner_resume(engine, NULL));
    for (guint left = 24000; left;) {
        guint frames = MIN(left, 4000u); wait_deck(engine, 1, frames);
        g_assert_true(radios_audio_engine_render(engine, frames, pcm[0], pcm[1], pcm[2], NULL)); left -= frames;
    }
    radios_audio_engine_snapshot(engine, &after); g_assert_cmpfloat(after.decks[1].tempo_ratio, ==, 1); g_assert_cmpuint(after.decks[1].generation, ==, generation);
    /* An ordinary current-source seek resumes the original curve phase. */
    g_assert_true(radios_audio_engine_seek(engine, 1, 45000, NULL)); wait_deck(engine, 1, 16000);
    radios_audio_engine_snapshot(engine, &after); g_assert_cmpuint(after.decks[1].generation, ==, generation + 1);
    g_assert_cmpfloat(fabs(after.decks[1].tempo_ratio - radios_audio_tempo_profile_ratio(&profile,
        radios_audio_tempo_profile_output(&profile, 45000))), <, .000001);
    g_assert_cmpfloat(after.decks[1].tempo_ratio, >, .96); g_assert_cmpfloat(after.decks[1].tempo_ratio, <, .98);
    g_assert_true(radios_audio_engine_render(engine, 4000, pcm[0], pcm[1], pcm[2], NULL)); radios_audio_engine_snapshot(engine, &after);
    g_assert_cmpuint(after.decks[1].cursor_frames, >, 45000); g_assert_cmpfloat(after.decks[1].tempo_ratio, >, .96);
    g_assert_true(radios_audio_engine_runner_stop(engine, NULL)); radios_audio_engine_unref(engine); g_free(ids[0]); g_free(ids[1]);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/tempo/pitch-and-duration", TempoFixture, NULL, setup, pitch_preserved, teardown);
    g_test_add("/tempo/native-return-continuous-pitch-and-profile-clock", TempoFixture, NULL, setup, return_profile_pitch_clock, teardown);
    g_test_add("/tempo/native-return-actual-click-intervals", TempoFixture, NULL, setup, return_profile_actual_click_intervals, teardown);
    g_test_add("/tempo/runner-native-return-pause-seek-generation", TempoFixture, NULL, setup, runner_return_profile_pause_seek, teardown);
    g_test_add("/tempo/fractional-numeric-locale-c", TempoFixture, "C", setup, fractional_numeric_locale, teardown);
    g_test_add("/tempo/fractional-numeric-locale-turkish", TempoFixture, "tr_TR.UTF-8", setup, fractional_numeric_locale, teardown);
    g_test_add("/tempo/sync-phase-source-clock", TempoFixture, NULL, setup, sync_and_clock, teardown);
    g_test_add("/tempo/sync-inactive-rejection-stop-native-PCM", TempoFixture, NULL, setup, sync_inactive_rejected_and_stop_releases, teardown);
    g_test_add("/tempo/sync-owned-source-pause-native-clock", TempoFixture, NULL, setup, sync_owned_source_pause_releases_manual_clock, teardown);
    g_test_add("/tempo/loudness-normalizes-deck", TempoFixture, NULL, setup, loudness_normalizes_deck, teardown);
    g_test_add("/tempo/sync-playing-reload-phase", TempoFixture, GINT_TO_POINTER(FALSE), setup, sync_playing_reload_matches_grid, teardown);
    g_test_add("/tempo/sync-playing-reload-phase-with-bars", TempoFixture, GINT_TO_POINTER(TRUE), setup, sync_playing_reload_matches_grid, teardown);
    g_test_add("/tempo/sync-slow-end", TempoFixture, GINT_TO_POINTER(FALSE), setup, sync_ends_at_source_boundary, teardown);
    g_test_add("/tempo/sync-fast-end", TempoFixture, GINT_TO_POINTER(TRUE), setup, sync_ends_at_source_boundary, teardown);
    g_test_add("/tempo/runner-grid-recycle-adopt-receipts", TempoFixture, NULL, setup, runner_grid_and_adopt, teardown);
    return g_test_run();
}
