#include "radios/audio_runner.h"
#include "radios/audio_decoder.h"

#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#ifndef RADIOS_TEST_PROJECT_ROOT
#define RADIOS_TEST_PROJECT_ROOT "."
#endif

typedef struct {
    gchar *directory;
    gchar *path;
    gchar *sha;
    gchar *media;
    gchar *version;
    RadioSAudioEngine *engine;
} Fixture;

static void append_le(GByteArray *bytes, guint32 value, guint length)
{
    guint8 data[4] = {value, value >> 8, value >> 16, value >> 24};
    g_byte_array_append(bytes, data, length);
}

static gint16 sample_at(guint64 frame)
{
    return (gint16)lrint(8192.0 * sin(2.0 * G_PI * 997.0 * (double)frame / 48000.0));
}

/* A real 997Hz sine wave makes the target frame's PCM differ from the old
 * buffer: changing only a reported cursor cannot satisfy the seek test. */
static void write_media(Fixture *fixture, guint frames)
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
    GError *error = NULL;
    g_assert_true(g_file_set_contents(fixture->path, (const gchar *)bytes->data,
                                     bytes->len, &error));
    g_assert_no_error(error);
    g_free(fixture->sha);
    fixture->sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes->data,
                                              bytes->len);
    g_byte_array_unref(bytes);
}

static void setup_frames(Fixture *fixture, guint frames)
{
    gchar *root = g_canonicalize_filename(RADIOS_TEST_PROJECT_ROOT, NULL);
    gchar *build = g_build_filename(root, "build", "pdf4", NULL);
    g_assert_cmpint(g_mkdir_with_parents(build, 0700), ==, 0);
    fixture->directory = g_build_filename(build, "song-seek-XXXXXX", NULL);
    g_assert_nonnull(g_mkdtemp(fixture->directory));
    fixture->path = g_build_filename(fixture->directory, "seek.wav", NULL);
    g_free(root);
    g_free(build);
    write_media(fixture, frames);
    fixture->media = g_uuid_string_random();
    fixture->version = g_uuid_string_random();
    fixture->engine = radios_audio_engine_new(FALSE, NULL);
    g_assert_nonnull(fixture->engine);
}

static void setup(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    setup_frames(fixture, 48000);
}

/* Six seconds: far beyond the one second a deck decodes ahead, so a seek
 * to the end is outside every decoded window. */
#define LONG_FRAMES 288000u
static void setup_long(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    setup_frames(fixture, LONG_FRAMES);
}

static void teardown(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    radios_audio_engine_unref(fixture->engine);
    g_assert_cmpint(g_unlink(fixture->path), ==, 0);
    g_assert_cmpint(g_rmdir(fixture->directory), ==, 0);
    g_free(fixture->directory);
    g_free(fixture->path);
    g_free(fixture->sha);
    g_free(fixture->media);
    g_free(fixture->version);
}

static RadioSAudioLoadSpec spec(Fixture *fixture, guint64 in, guint64 out)
{
    return (RadioSAudioLoadSpec){ .path = fixture->path, .sha256 = fixture->sha,
        .media_id = fixture->media, .version_id = fixture->version,
        .cue_in_frame = in, .cue_out_frame = out };
}

static RadioSAudioDeckSnapshot snapshot(Fixture *fixture, guint deck)
{
    RadioSAudioSnapshot state;
    radios_audio_engine_snapshot(fixture->engine, &state);
    return state.decks[deck];
}

static RadioSAudioDeckSnapshot ready(Fixture *fixture, guint deck)
{
    const gint64 until = g_get_monotonic_time() + 5000000;
    do {
        RadioSAudioDeckSnapshot state = snapshot(fixture, deck);
        if (state.state == RADIOS_AUDIO_ERROR)
            g_error("Decoder failed: %s", state.error);
        if (state.state != RADIOS_AUDIO_LOADING && state.buffered_frames >= 512)
            return state;
        g_usleep(1000);
    } while (g_get_monotonic_time() < until);
    g_error("Deck %u did not decode the WAV fixture", deck);
    return (RadioSAudioDeckSnapshot){0};
}

static void load(Fixture *fixture, guint64 in, guint64 out)
{
    RadioSAudioLoadSpec source = spec(fixture, in, out);
    GError *error = NULL;
    g_assert_true(radios_audio_engine_load(fixture->engine, 0, &source, &error));
    g_assert_no_error(error);
    (void)ready(fixture, 0);
}

static void render(Fixture *fixture, guint frames, gfloat output[3][8192])
{
    GError *error = NULL;
    g_assert_true(radios_audio_engine_render(fixture->engine, frames, output[0],
                                             output[1], output[2], &error));
    g_assert_no_error(error);
}

static void test_valid_seek(Fixture *fixture, gconstpointer data)
{
    const gboolean paused = GPOINTER_TO_UINT(data) != 0;
    load(fixture, 100, 40000);
    g_assert_true(radios_audio_engine_play(fixture->engine, 0, NULL));
    gfloat output[3][8192];
    render(fixture, 128, output);
    if (paused)
        g_assert_true(radios_audio_engine_pause(fixture->engine, 0, NULL));
    const RadioSAudioDeckSnapshot before = snapshot(fixture, 0);
    GError *error = NULL;
    g_assert_true(radios_audio_engine_seek_if_generation(fixture->engine, 0,
        before.generation, 3500, &error));
    g_assert_no_error(error);
    const RadioSAudioDeckSnapshot after = ready(fixture, 0);
    g_assert_cmpuint(after.cursor_frames, ==, 3500);
    g_assert_cmpstr(after.media_id, ==, before.media_id);
    g_assert_cmpstr(after.version_id, ==, before.version_id);
    if (paused)
        g_assert_true(after.state == RADIOS_AUDIO_PAUSED ||
                       after.state == RADIOS_AUDIO_READY);
    else
        g_assert_cmpint(after.state, ==, RADIOS_AUDIO_PLAYING);
    render(fixture, 64, output);
    if (paused) {
        g_assert_cmpfloat(output[0][0], ==, 0.0f);
        g_assert_cmpuint(snapshot(fixture, 0).cursor_frames, ==, 3500);
    } else {
        const gint16 sample = sample_at(3500);
        g_assert_cmpfloat(fabsf(output[0][0] - (gfloat)sample / 32768.0f), <,
                          0.00001f);
        g_assert_cmpfloat(fabsf(output[0][1] - (gfloat)(-sample / 2) / 32768.0f), <,
                          0.00001f);
        g_assert_cmpuint(snapshot(fixture, 0).cursor_frames, ==, 3564);
    }
}

static void assert_unchanged(RadioSAudioDeckSnapshot before,
                              RadioSAudioDeckSnapshot after)
{
    g_assert_cmpuint(after.generation, ==, before.generation);
    g_assert_cmpuint(after.cursor_frames, ==, before.cursor_frames);
    g_assert_cmpint(after.state, ==, before.state);
    g_assert_cmpstr(after.media_id, ==, before.media_id);
    g_assert_cmpstr(after.version_id, ==, before.version_id);
}

static void test_replacement_generation(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    load(fixture, 100, 40000);
    const guint64 old_generation = snapshot(fixture, 0).generation;
    g_free(fixture->media);
    g_free(fixture->version);
    fixture->media = g_uuid_string_random();
    fixture->version = g_uuid_string_random();
    load(fixture, 200, 42000);
    const RadioSAudioDeckSnapshot before = snapshot(fixture, 0);
    g_assert_cmpuint(before.generation, >, old_generation);
    GError *error = NULL;
    g_assert_false(radios_audio_engine_seek_if_generation(fixture->engine, 0,
        old_generation, 3500, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    assert_unchanged(before, snapshot(fixture, 0));
}

static void test_zero_generation(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    load(fixture, 100, 40000);
    const RadioSAudioDeckSnapshot before = snapshot(fixture, 0);
    GError *error = NULL;
    g_assert_false(radios_audio_engine_seek_if_generation(fixture->engine, 0,
        0, 3500, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    assert_unchanged(before, snapshot(fixture, 0));
}

static void test_real_cue_bounds(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    load(fixture, 1234, 34567);
    const RadioSAudioDeckSnapshot before = snapshot(fixture, 0);
    g_assert_cmpuint(before.cue_in_frame, ==, 1234);
    g_assert_cmpuint(before.cue_out_frame, ==, 34567);
    g_assert_cmpuint(before.cursor_frames, ==, 1234);
}

/* latest_start_utc_us bounds the first item's start like a calendar or rule
 * window does; 0 leaves it unbounded. */
static void start_queue_window(Fixture *fixture, gint64 latest_start_utc_us)
{
    RadioSAudioRunOptions options;
    radios_audio_run_options_default(&options);
    gchar *entry0 = g_uuid_string_random();
    gchar *entry1 = g_uuid_string_random();
    RadioSAudioRunItem items[2] = {
        { .entry_id = entry0, .media = spec(fixture, 100, 12000),
          .duration_frames = 48000, .auto_next_frame = 12000,
          .fade_start_frame = 12000, .fade_end_frame = 12000,
          .latest_start_utc_us = latest_start_utc_us, .preferred_deck = -1 },
        { .entry_id = entry1, .media = spec(fixture, 200, 14000),
          .duration_frames = 48000, .auto_next_frame = 14000,
          .fade_start_frame = 14000, .fade_end_frame = 14000,
          .preferred_deck = -1 }
    };
    GError *error = NULL;
    g_assert_true(radios_audio_engine_runner_configure(fixture->engine, items, 2,
                                                       &options, &error));
    g_assert_no_error(error);
    g_assert_true(radios_audio_engine_runner_start(fixture->engine, &error));
    g_assert_no_error(error);
    (void)ready(fixture, 0);
    (void)ready(fixture, 1);
    gfloat output[3][8192];
    render(fixture, 128, output);
    g_free(entry0);
    g_free(entry1);
}

static void start_queue(Fixture *fixture)
{
    start_queue_window(fixture, 0);
}

static void test_valid_seek_keeps_runner(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    start_queue(fixture);
    const RadioSAudioDeckSnapshot before = snapshot(fixture, 0);
    g_assert_cmpint(before.state, ==, RADIOS_AUDIO_PLAYING);
    GError *error = NULL;
    g_assert_true(radios_audio_engine_seek_if_generation(fixture->engine, 0,
        before.generation, 3500, &error));
    g_assert_no_error(error);
    const RadioSAudioDeckSnapshot after = ready(fixture, 0);
    g_assert_true(after.state == RADIOS_AUDIO_READY || after.state == RADIOS_AUDIO_PLAYING);
    g_assert_cmpuint(after.cursor_frames, ==, 3500);
    RadioSAudioRunSnapshot run;
    radios_audio_engine_runner_snapshot(fixture->engine, &run);
    g_assert_cmpint(run.state, !=, RADIOS_AUDIO_RUN_IDLE);
    g_assert_true(run.owned[0]);
    g_assert_true(run.owned[1]);
    g_assert_cmpint(snapshot(fixture, 1).state, !=, RADIOS_AUDIO_EMPTY);
    gfloat output[3][8192];
    render(fixture, 64, output);
    g_assert_cmpfloat(fabsf(output[0][0] - (gfloat)sample_at(3500) / 32768.0f), <,
                      0.00001f);
    g_assert_cmpuint(snapshot(fixture, 0).cursor_frames, ==, 3564);
}

static void test_paused_runner_seek_remains_silent(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    start_queue(fixture);
    g_assert_true(radios_audio_engine_runner_pause(fixture->engine, NULL));
    RadioSAudioRunSnapshot run;
    radios_audio_engine_runner_snapshot(fixture->engine, &run);
    g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_PAUSED);
    g_assert_true(run.owned[0]);
    const RadioSAudioDeckSnapshot before = snapshot(fixture, 0);
    /* Runner pause intentionally preserves the decoder's PLAYING state. */
    g_assert_cmpint(before.state, ==, RADIOS_AUDIO_PLAYING);
    g_assert_true(before.runner_paused);
    gfloat output[3][8192];
    render(fixture, 64, output);
    for (guint sample = 0; sample < 128; ++sample)
        g_assert_cmpfloat(output[0][sample], ==, 0.0f);
    g_assert_cmpuint(snapshot(fixture, 0).cursor_frames, ==, before.cursor_frames);
    GError *error = NULL;
    g_assert_true(radios_audio_engine_seek_if_generation(fixture->engine, 0,
        before.generation, 3500, &error));
    g_assert_no_error(error);
    const RadioSAudioDeckSnapshot after = ready(fixture, 0);
    g_assert_cmpuint(after.cursor_frames, ==, 3500);
    g_assert_true(after.state == RADIOS_AUDIO_READY || after.state == RADIOS_AUDIO_PAUSED);
    g_assert_true(after.runner_paused);
    g_assert_cmpstr(after.media_id, ==, before.media_id);
    g_assert_cmpstr(after.version_id, ==, before.version_id);
    radios_audio_engine_runner_snapshot(fixture->engine, &run);
    g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_PAUSED);
    g_assert_true(run.owned[0]);
    g_assert_true(run.owned[1]);
    g_assert_cmpint(snapshot(fixture, 1).state, !=, RADIOS_AUDIO_EMPTY);
    for (guint block = 0; block < 8; ++block) {
        render(fixture, 64, output);
        for (guint sample = 0; sample < 128; ++sample)
            g_assert_cmpfloat(output[0][sample], ==, 0.0f);
        g_assert_cmpuint(snapshot(fixture, 0).cursor_frames, ==, 3500);
        g_usleep(2000);
    }
}

static void test_preview_public_guarded_seek(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    const guint deck = RADIOS_AUDIO_PREVIEW_DECK;
    RadioSAudioLoadSpec source = spec(fixture, 100, 40000);
    GError *error = NULL;
    g_assert_true(radios_audio_engine_load(fixture->engine, deck, &source, &error));
    g_assert_no_error(error);
    (void)ready(fixture, deck);
    g_assert_true(radios_audio_engine_play(fixture->engine, deck, &error));
    g_assert_no_error(error);
    gfloat output[3][8192];
    render(fixture, 64, output);
    const RadioSAudioDeckSnapshot before = snapshot(fixture, deck);
    g_assert_cmpint(before.state, ==, RADIOS_AUDIO_PLAYING);
    g_assert_false(before.main_routed);
    g_assert_true(radios_audio_engine_seek_if_generation(fixture->engine, deck,
        before.generation, 3500, &error));
    g_assert_no_error(error);
    const RadioSAudioDeckSnapshot after = ready(fixture, deck);
    g_assert_cmpuint(after.cursor_frames, ==, 3500);
    g_assert_cmpint(after.state, ==, RADIOS_AUDIO_PLAYING);
    g_assert_false(after.main_routed);
    render(fixture, 64, output);
    for (guint frame = 0; frame < 64; ++frame) {
        const gint16 expected = sample_at(3500 + frame);
        g_assert_cmpfloat(output[0][frame * 2], ==, 0.0f);
        g_assert_cmpfloat(output[0][frame * 2 + 1], ==, 0.0f);
        g_assert_cmpfloat(fabsf(output[1][frame * 2] - (gfloat)expected / 32768.0f), <,
                          0.00001f);
        g_assert_cmpfloat(fabsf(output[1][frame * 2 + 1] - (gfloat)(-expected / 2) / 32768.0f), <,
                          0.00001f);
    }
    g_assert_cmpuint(snapshot(fixture, deck).cursor_frames, ==, 3564);
}

static void test_invalid_cue_preserves_runner(Fixture *fixture, gconstpointer data)
{
    const gboolean guarded = GPOINTER_TO_UINT(data) != 0;
    start_queue(fixture);
    RadioSAudioRunSnapshot before, after;
    radios_audio_engine_runner_snapshot(fixture->engine, &before);
    g_assert_cmpint(before.state, ==, RADIOS_AUDIO_RUN_PLAYING);
    g_assert_true(before.owned[0]);
    g_assert_true(before.owned[1]);
    const RadioSAudioDeckSnapshot deck_before = snapshot(fixture, 0);
    GError *error = NULL;
    const gboolean accepted = guarded ?
        radios_audio_engine_seek_if_generation(fixture->engine, 0,
            deck_before.generation, 12001, &error) :
        radios_audio_engine_seek(fixture->engine, 0, 12001, &error);
    g_assert_false(accepted);
    g_assert_nonnull(error);
    g_clear_error(&error);
    radios_audio_engine_runner_snapshot(fixture->engine, &after);
    g_assert_cmpuint(after.epoch, ==, before.epoch);
    g_assert_cmpint(after.state, ==, before.state);
    g_assert_cmpuint(after.index, ==, before.index);
    g_assert_cmpuint(after.count, ==, before.count);
    g_assert_cmpint(after.deck_current, ==, before.deck_current);
    g_assert_true(after.owned[0]);
    g_assert_true(after.owned[1]);
    assert_unchanged(deck_before, snapshot(fixture, 0));
    gfloat output[3][8192];
    render(fixture, 64, output);
    g_assert_cmpfloat(fabsf(output[0][0] -
                          (gfloat)sample_at(before.actual_current_frame) / 32768.0f),
                      <, 0.00001f);
    radios_audio_engine_runner_snapshot(fixture->engine, &after);
    g_assert_cmpuint(after.actual_current_frame, ==, before.actual_current_frame + 64);
}

static void assert_pcm_at(const gfloat *stereo, guint frames, guint64 first)
{
    for (guint i = 0; i < frames; ++i) {
        const gint16 sample = sample_at(first + i);
        g_assert_cmpfloat(fabsf(stereo[i * 2] - (gfloat)sample / 32768.0f), <, 0.00001f);
        g_assert_cmpfloat(fabsf(stereo[i * 2 + 1] - (gfloat)(-sample / 2) / 32768.0f), <,
                          0.00001f);
    }
}

/* Waits for decoded PCM at a seek target while asserting that the deck never
 * passes through LOADING and keeps reporting the chosen point (R8.3). */
static RadioSAudioDeckSnapshot settle_at(Fixture *fixture, guint deck,
                                         RadioSAudioState expected, guint64 target)
{
    const gint64 until = g_get_monotonic_time() + 5000000;
    RadioSAudioDeckSnapshot state = snapshot(fixture, deck);
    for (;;) {
        g_assert_cmpint(state.state, ==, expected);
        g_assert_cmpuint(state.cursor_frames, ==, target);
        if (state.buffered_frames >= 512)
            return state;
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        g_usleep(1000);
        state = snapshot(fixture, deck);
    }
}

/* R8.3: moving the play bar forward past the decoded window is a move to the
 * chosen point, not a reload. The deck keeps PLAYING (or PAUSED), reports the
 * target at once, keeps its media and resumes PCM exactly there. The
 * generation still advances, so a command issued before the move is fenced. */
static void test_forward_beyond_window(Fixture *fixture, gconstpointer data)
{
    const gboolean paused = GPOINTER_TO_UINT(data) != 0;
    const guint64 target = 240000;
    load(fixture, 0, 0);
    g_assert_true(radios_audio_engine_play(fixture->engine, 0, NULL));
    gfloat output[3][8192];
    render(fixture, 128, output);
    if (paused)
        g_assert_true(radios_audio_engine_pause(fixture->engine, 0, NULL));
    const RadioSAudioDeckSnapshot before = snapshot(fixture, 0);
    g_assert_cmpuint(before.cursor_frames, ==, 128);
    g_assert_cmpuint(before.cursor_frames + before.buffered_frames, <, target);
    GError *error = NULL;
    g_assert_true(radios_audio_engine_seek_if_generation(fixture->engine, 0,
        before.generation, target, &error));
    g_assert_no_error(error);
    const RadioSAudioState expected = paused ? RADIOS_AUDIO_PAUSED : RADIOS_AUDIO_PLAYING;
    const RadioSAudioDeckSnapshot moved = snapshot(fixture, 0);
    g_assert_cmpint(moved.state, ==, expected);
    g_assert_cmpuint(moved.cursor_frames, ==, target);
    g_assert_cmpuint(moved.generation, >, before.generation);
    g_assert_cmpstr(moved.media_id, ==, before.media_id);
    g_assert_cmpstr(moved.version_id, ==, before.version_id);
    (void)settle_at(fixture, 0, expected, target);
    g_assert_false(radios_audio_engine_seek_if_generation(fixture->engine, 0,
        before.generation, 1000, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_STATE);
    g_clear_error(&error);
    render(fixture, 64, output);
    if (paused) {
        for (guint sample = 0; sample < 128; ++sample)
            g_assert_cmpfloat(output[0][sample], ==, 0.0f);
        g_assert_cmpuint(snapshot(fixture, 0).cursor_frames, ==, target);
        g_assert_true(radios_audio_engine_play(fixture->engine, 0, NULL));
        render(fixture, 64, output);
    }
    assert_pcm_at(output[0], 64, target);
    g_assert_cmpuint(snapshot(fixture, 0).cursor_frames, ==, target + 64);
    g_assert_cmpint(snapshot(fixture, 0).state, ==, RADIOS_AUDIO_PLAYING);
}

static void decoded_through(Fixture *fixture, guint deck, guint64 frame)
{
    const gint64 until = g_get_monotonic_time() + 5000000;
    for (;;) {
        const RadioSAudioDeckSnapshot state = snapshot(fixture, deck);
        if (state.cursor_frames + state.buffered_frames > frame)
            return;
        g_assert_cmpint(g_get_monotonic_time(), <, until);
        g_usleep(1000);
    }
}

/* A backward move reopens the verified source and is just as exact; a move
 * within the decoded window needs no decode at all. */
static void test_backward_and_window_seek(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    load(fixture, 0, 0);
    g_assert_true(radios_audio_engine_play(fixture->engine, 0, NULL));
    gfloat output[3][8192];
    render(fixture, 128, output);
    GError *error = NULL;
    const guint64 steps[] = {200000, 24000, 30000};
    for (guint i = 0; i < G_N_ELEMENTS(steps); ++i) {
        /* 30000 lies inside the PCM decoded after 24000. */
        if (steps[i] == 30000)
            decoded_through(fixture, 0, steps[i] + 512);
        const RadioSAudioDeckSnapshot before = snapshot(fixture, 0);
        g_assert_true(radios_audio_engine_seek_if_generation(fixture->engine, 0,
            before.generation, steps[i], &error));
        g_assert_no_error(error);
        const RadioSAudioDeckSnapshot moved = steps[i] == 30000 ? snapshot(fixture, 0) :
            settle_at(fixture, 0, RADIOS_AUDIO_PLAYING, steps[i]);
        if (steps[i] == 30000)
            g_assert_cmpuint(moved.buffered_frames, >=, 512);
        g_assert_cmpint(moved.state, ==, RADIOS_AUDIO_PLAYING);
        g_assert_cmpuint(moved.cursor_frames, ==, steps[i]);
        render(fixture, 64, output);
        assert_pcm_at(output[0], 64, steps[i]);
        g_assert_cmpuint(snapshot(fixture, 0).cursor_frames, ==, steps[i] + 64);
    }
}

/* The decoder continues a forward seek from its current position and reopens
 * for a backward one; both give exactly the frames a fresh decode would. */
static void test_decoder_seek_continuation(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    GError *error = NULL;
    RadioSAudioDecoder *decoder = radios_audio_decoder_open(fixture->path, fixture->sha,
                                                            NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(decoder);
    gfloat pcm[512 * 2];
    g_assert_cmpint(radios_audio_decoder_read(decoder, pcm, 512, NULL, &error), ==, 512);
    assert_pcm_at(pcm, 512, 0);
    const guint64 targets[] = {200000, 200000, 250000, 1000, LONG_FRAMES - 512};
    for (guint i = 0; i < G_N_ELEMENTS(targets); ++i) {
        g_assert_true(radios_audio_decoder_seek(decoder, targets[i], NULL, &error));
        g_assert_no_error(error);
        g_assert_cmpuint(radios_audio_decoder_position_frames(decoder), ==, targets[i]);
        g_assert_cmpint(radios_audio_decoder_read(decoder, pcm, 512, NULL, &error), ==, 512);
        assert_pcm_at(pcm, 512, targets[i]);
    }
    g_assert_cmpint(radios_audio_decoder_read(decoder, pcm, 512, NULL, &error), ==, 0);
    g_assert_false(radios_audio_decoder_seek(decoder, LONG_FRAMES + 1, NULL, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_true(radios_audio_decoder_seek(decoder, 7, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpint(radios_audio_decoder_read(decoder, pcm, 512, NULL, &error), ==, 512);
    assert_pcm_at(pcm, 512, 7);
    radios_audio_decoder_close(decoder);
}

/* Calendar and rule automation bound each source's latest start. A seek
 * re-prerolls the current item, which already went on air: the deadline of
 * its first start must not turn the move into "start window has expired"
 * and stop the whole automation, while playing or after a paused seek. */
static void test_runner_seek_after_latest_start(Fixture *fixture, gconstpointer data)
{
    const gboolean paused = GPOINTER_TO_UINT(data) != 0;
    const gint64 latest = g_get_real_time() + 300000;
    start_queue_window(fixture, latest);
    RadioSAudioRunSnapshot run;
    radios_audio_engine_runner_snapshot(fixture->engine, &run);
    g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_PLAYING);
    GError *error = NULL;
    if (paused)
        g_assert_true(radios_audio_engine_runner_pause(fixture->engine, &error));
    g_assert_no_error(error);
    while (g_get_real_time() <= latest + 20000)
        g_usleep(5000);
    const RadioSAudioDeckSnapshot before = snapshot(fixture, 0);
    g_assert_true(radios_audio_engine_seek_if_generation(fixture->engine, 0,
        before.generation, 3500, &error));
    g_assert_no_error(error);
    (void)ready(fixture, 0);
    gfloat output[3][8192];
    if (paused) {
        render(fixture, 64, output);
        for (guint sample = 0; sample < 128; ++sample)
            g_assert_cmpfloat(output[0][sample], ==, 0.0f);
        g_assert_true(radios_audio_engine_runner_resume(fixture->engine, &error));
        g_assert_no_error(error);
    }
    render(fixture, 64, output);
    radios_audio_engine_runner_snapshot(fixture->engine, &run);
    g_assert_cmpstr(run.error, ==, "");
    g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_PLAYING);
    g_assert_cmpint(run.deck_current, ==, 0);
    g_assert_true(run.owned[0]);
    g_assert_true(run.owned[1]);
    assert_pcm_at(output[0], 64, 3500);
    g_assert_cmpuint(snapshot(fixture, 0).cursor_frames, ==, 3564);
    g_assert_cmpint(snapshot(fixture, 0).state, ==, RADIOS_AUDIO_PLAYING);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/song-seek/playing-cursor-pcm", Fixture, GUINT_TO_POINTER(0),
               setup, test_valid_seek, teardown);
    g_test_add("/song-seek/paused-cursor-silence", Fixture, GUINT_TO_POINTER(1),
               setup, test_valid_seek, teardown);
    g_test_add("/song-seek/replacement-generation", Fixture, NULL, setup,
               test_replacement_generation, teardown);
    g_test_add("/song-seek/zero-generation", Fixture, NULL, setup,
               test_zero_generation, teardown);
    g_test_add("/song-seek/real-cue-bounds", Fixture, NULL, setup,
               test_real_cue_bounds, teardown);
    g_test_add("/song-seek/valid-seek-keeps-runner-playing", Fixture, NULL, setup,
               test_valid_seek_keeps_runner, teardown);
    g_test_add("/song-seek/guarded-invalid-cue-keeps-runner", Fixture,
               GUINT_TO_POINTER(1), setup, test_invalid_cue_preserves_runner, teardown);
    g_test_add("/song-seek/legacy-invalid-cue-keeps-runner", Fixture,
               GUINT_TO_POINTER(0), setup, test_invalid_cue_preserves_runner, teardown);
    g_test_add("/song-seek/paused-runner-seek-stays-silent", Fixture, NULL, setup,
               test_paused_runner_seek_remains_silent, teardown);
    g_test_add("/song-seek/preview-public-guard-cue-isolation", Fixture, NULL, setup,
               test_preview_public_guarded_seek, teardown);
    g_test_add("/song-seek/forward-beyond-window-keeps-playing", Fixture,
               GUINT_TO_POINTER(0), setup_long, test_forward_beyond_window, teardown);
    g_test_add("/song-seek/forward-beyond-window-stays-paused", Fixture,
               GUINT_TO_POINTER(1), setup_long, test_forward_beyond_window, teardown);
    g_test_add("/song-seek/backward-and-window-moves-exact", Fixture, NULL, setup_long,
               test_backward_and_window_seek, teardown);
    g_test_add("/song-seek/decoder-forward-continuation-exact", Fixture, NULL, setup_long,
               test_decoder_seek_continuation, teardown);
    g_test_add("/song-seek/autodj-seek-after-latest-start-keeps-runner", Fixture,
               GUINT_TO_POINTER(0), setup, test_runner_seek_after_latest_start, teardown);
    g_test_add("/song-seek/paused-autodj-seek-resume-after-latest-start", Fixture,
               GUINT_TO_POINTER(1), setup, test_runner_seek_after_latest_start, teardown);
    return g_test_run();
}
