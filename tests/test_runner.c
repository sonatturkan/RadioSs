#include "radios/audio_runner.h"
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#ifndef RADIOS_TEST_PROJECT_ROOT
#define RADIOS_TEST_PROJECT_ROOT "."
#endif
typedef struct { gchar *directory, *path[2], *sha[2], *media[2], *version[2], *entry[3]; } Fixture;
static void append16(GByteArray *bytes, guint16 n)
{ guint8 b[] = {(guint8)n, (guint8)(n >> 8)}; g_byte_array_append(bytes, b, 2); }
static void append32(GByteArray *bytes, guint32 n)
{ guint8 b[] = {(guint8)n, (guint8)(n >> 8), (guint8)(n >> 16), (guint8)(n >> 24)}; g_byte_array_append(bytes, b, 4); }
static void setup(Fixture *f, gconstpointer unused)
{
    (void)unused; gchar *root = g_canonicalize_filename(RADIOS_TEST_PROJECT_ROOT, NULL), *build = g_build_filename(root, "build", NULL);
    g_assert_cmpint(g_mkdir_with_parents(build, 0700), ==, 0); f->directory = g_build_filename(build, "audio-runner-XXXXXX", NULL); g_assert_nonnull(g_mkdtemp(f->directory));
    for (guint file = 0; file < 2; ++file) {
        gchar *name = g_strdup_printf("source-%u.wav", file); f->path[file] = g_build_filename(f->directory, name, NULL); g_free(name);
        GByteArray *b = g_byte_array_new(); g_byte_array_append(b, (guint8 *)"RIFF", 4); append32(b, 96000 * 4 + 36);
        g_byte_array_append(b, (guint8 *)"WAVEfmt ", 8); append32(b, 16); append16(b, 1); append16(b, 2); append32(b, 48000); append32(b, 192000); append16(b, 4); append16(b, 16);
        g_byte_array_append(b, (guint8 *)"data", 4); append32(b, 96000 * 4);
        for (guint i = 0; i < 96000; ++i) { append16(b, file ? 4096 : 8192); append16(b, file ? (guint16)-4096 : (guint16)-8192); }
        g_assert_true(g_file_set_contents(f->path[file], (gchar *)b->data, b->len, NULL)); f->sha[file] = g_compute_checksum_for_data(G_CHECKSUM_SHA256, b->data, b->len); g_byte_array_unref(b);
        f->media[file] = g_uuid_string_random(); f->version[file] = g_uuid_string_random();
    }
    for (guint i = 0; i < 3; ++i) f->entry[i] = g_uuid_string_random();
    g_free(root); g_free(build);
}
static void teardown(Fixture *f, gconstpointer unused)
{
    (void)unused; for (guint i = 0; i < 2; ++i) { g_assert_cmpint(g_unlink(f->path[i]), ==, 0); g_free(f->path[i]); g_free(f->sha[i]); g_free(f->media[i]); g_free(f->version[i]); }
    for (guint i = 0; i < 3; ++i) g_free(f->entry[i]);
    g_assert_cmpint(g_rmdir(f->directory), ==, 0); g_free(f->directory);
}
static RadioSAudioRunItem item(Fixture *f, guint file, guint entry, guint64 in, guint64 out)
{
    RadioSAudioRunItem result = {0}; result.entry_id = f->entry[entry]; result.media = (RadioSAudioLoadSpec){f->path[file], f->sha[file], f->media[file], f->version[file], in, out};
    result.duration_frames = 96000; result.auto_next_frame = result.fade_start_frame = result.fade_end_frame = out; result.preferred_deck = -1; return result;
}
static void near(gdouble actual, gdouble expected)
{ if (fabs(actual - expected) >= .00001) g_test_message("Actual PCM %.9f, expected %.9f", actual, expected); g_assert_cmpfloat(fabs(actual - expected), <, .00001); }
static void render(RadioSAudioEngine *engine, guint frames, gfloat output[3][8192])
{ GError *error = NULL; g_assert_true(radios_audio_engine_render(engine, frames, output[0], output[1], output[2], &error)); g_assert_no_error(error); }
static void ready_after(RadioSAudioEngine *engine, guint deck, const gchar *media, guint buffered, guint64 old_generation)
{
    gint64 until = g_get_monotonic_time() + 10000000; RadioSAudioSnapshot s;
    do {
        radios_audio_engine_snapshot(engine, &s); RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(engine, &r);
        if (r.state == RADIOS_AUDIO_RUN_ERROR) g_error("Runner: %s", r.error);
        if (g_str_equal(s.decks[deck].media_id, media) && s.decks[deck].generation > old_generation &&
            s.decks[deck].state != RADIOS_AUDIO_LOADING && s.decks[deck].buffered_frames >= buffered) return;
        g_usleep(1000);
    } while (g_get_monotonic_time() < until);
    g_error("Runner deck%u did not become ready (state%d, buffers%" G_GUINT64_FORMAT ")", deck, s.decks[deck].state, s.decks[deck].buffered_frames);
}
static void ready(RadioSAudioEngine *engine, guint deck, const gchar *media, guint buffered)
{ ready_after(engine, deck, media, buffered, 0); }
static void start(RadioSAudioEngine *engine, RadioSAudioRunItem *items, guint count, RadioSAudioRunOptions *options)
{
    GError *error = NULL; g_assert_true(radios_audio_engine_runner_configure(engine, items, count, options, &error)); g_assert_no_error(error);
    g_assert_true(radios_audio_engine_runner_start(engine, &error)); g_assert_no_error(error);
}
static void validation(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[] = {item(f, 0, 0, 100, 1100)}; GError *error = NULL;
    g_assert_true(radios_audio_engine_runner_configure(e, q, 1, &o, NULL)); o.deck_pool[1] = 32;
    g_assert_false(radios_audio_engine_runner_configure(e, q, 1, &o, &error)); g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID); g_clear_error(&error);
    RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(e, &r); g_assert_cmpuint(r.count, ==, 1);
    radios_audio_run_options_default(&o); q[0].preferred_deck = 31; g_assert_false(radios_audio_engine_runner_configure(e, q, 1, &o, &error)); g_clear_error(&error); q[0].preferred_deck = -1;
    g_assert_true(radios_audio_engine_load(e, 0, &q[0].media, NULL)); ready(e, 0, f->media[0], 1000);
    g_assert_false(radios_audio_engine_runner_start(e, &error)); g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_STATE); g_clear_error(&error);
    RadioSAudioSnapshot s; radios_audio_engine_snapshot(e, &s); g_assert_cmpstr(s.decks[0].media_id, ==, f->media[0]); g_assert_cmpuint(s.decks[0].cursor_frames, ==, 100);
    radios_audio_engine_unref(e);
}
static void eof_midblock(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[] = {item(f, 0, 0, 100, 1100), item(f, 1, 1, 200, 1600)}; start(e, q, 2, &o);
    ready(e, 0, f->media[0], 1000); ready(e, 1, f->media[1], 1400); gfloat output[3][8192]; render(e, 997, output);
    for (guint i = 0; i < 997; ++i) near(output[0][i * 2], .25);
    render(e, 20, output);
    for (guint i = 0; i < 20; ++i) { near(output[0][i * 2], i < 3 ? .25 : .125); near(output[1][i * 2], 0); }
    RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(e, &r); g_assert_cmpuint(r.index, ==, 1); g_assert_cmpuint(r.actual_current_frame, ==, 217);
    render(e, 1400, output); radios_audio_engine_runner_snapshot(e, &r); g_assert_cmpint(r.state, ==, RADIOS_AUDIO_RUN_FINISHED); radios_audio_engine_unref(e);
}
static void overlap(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 2000), item(f, 1, 1, 0, 3000)};
    q[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; q[0].overlap_frames = q[1].fade_in_frames = 1000; q[0].duck_db = -6.020599913279624;
    start(e, q, 2, &o); ready(e, 0, f->media[0], 2000); ready(e, 1, f->media[1], 3000); gfloat output[3][8192]; render(e, 1000, output); render(e, 1000, output);
    RadioSAudioTransition t = {1000, 1000, 1000, q[0].duck_db};
    for (guint i = 0; i < 1000; ++i) { gdouble a, b; radios_audio_transition_gains(&t, i, &a, &b); near(output[0][i * 2], .25 * a + .125 * b); }
    render(e, 1, output); near(output[0][0], .125); RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(e, &r);
    g_assert_cmpuint(r.index, ==, 1); g_assert_cmpuint(r.actual_current_frame, ==, 1001); radios_audio_engine_unref(e);
}
static void pause_overlap(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 2000), item(f, 1, 1, 0, 3000)};
    q[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; q[0].overlap_frames = q[1].fade_in_frames = 1000; start(e, q, 2, &o);
    ready(e, 0, f->media[0], 2000); ready(e, 1, f->media[1], 3000); gfloat output[3][8192]; render(e, 1400, output);
    g_assert_true(radios_audio_engine_runner_pause(e, NULL)); render(e, 700, output); near(output[0][0], 0);
    RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(e, &r); g_assert_cmpint(r.state, ==, RADIOS_AUDIO_RUN_PAUSED);
    g_assert_cmpuint(r.actual_current_frame, ==, 1400); g_assert_cmpuint(r.actual_next_frame, ==, 400);
    g_assert_true(radios_audio_engine_runner_resume(e, NULL)); render(e, 1, output); near(output[0][0], .25 * cos(G_PI / 2 * .4) + .125 * sin(G_PI / 2 * .4));
    radios_audio_engine_runner_snapshot(e, &r); g_assert_cmpuint(r.actual_current_frame, ==, 1401); g_assert_cmpuint(r.actual_next_frame, ==, 401); radios_audio_engine_unref(e);
}
static void single_deck(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; radios_audio_run_options_default(&o); o.pool_count = 1;
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 1000), item(f, 1, 1, 50, 1500)}; q[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; GError *error = NULL;
    g_assert_false(radios_audio_engine_runner_configure(e, q, 2, &o, &error)); g_clear_error(&error); q[0].mode = RADIOS_AUDIO_RUN_EOF;
    start(e, q, 2, &o); ready(e, 0, f->media[0], 1000); gfloat output[3][8192]; render(e, 1001, output); near(output[0][2000], 0);
    RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(e, &r); g_assert_cmpint(r.state, ==, RADIOS_AUDIO_RUN_WAITING_MEDIA);
    ready(e, 0, f->media[1], 1450); render(e, 100, output); near(output[0][0], .125); radios_audio_engine_runner_snapshot(e, &r);
    g_assert_cmpuint(r.index, ==, 1); g_assert_cmpuint(r.actual_current_frame, ==, 150); g_assert_false(r.owned[1]); radios_audio_engine_unref(e);
}
static void takeover(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 12000), item(f, 1, 1, 0, 12000)}; start(e, q, 2, &o);
    ready(e, 0, f->media[0], 4800); ready(e, 1, f->media[1], 4800); gfloat output[3][8192]; render(e, 100, output);
    RadioSAudioRunSnapshot before, after; radios_audio_engine_runner_snapshot(e, &before);
    RadioSAudioMixerConfig c; radios_audio_engine_get_mixer(e, &c); c.channels[0].gain_db = -6; g_assert_true(radios_audio_engine_apply_mixer(e, &c, NULL));
    radios_audio_engine_runner_snapshot(e, &after); g_assert_cmpuint(after.epoch, ==, before.epoch); g_assert_true(after.owned[0]);
    g_assert_true(radios_audio_engine_load(e, 0, &q[1].media, NULL)); ready(e, 0, f->media[1], 4800); radios_audio_engine_runner_snapshot(e, &after);
    g_assert_cmpint(after.state, ==, RADIOS_AUDIO_RUN_IDLE); g_assert_cmpuint(after.epoch, >, before.epoch); g_assert_false(after.owned[1]);
    RadioSAudioSnapshot s; radios_audio_engine_snapshot(e, &s); g_assert_cmpint(s.decks[1].state, ==, RADIOS_AUDIO_EMPTY);
    radios_audio_engine_unref(e);
}
static void cue_navigation(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 2000), item(f, 1, 1, 100, 2000), item(f, 0, 2, 0, 2000)};
    q[0].mode = RADIOS_AUDIO_RUN_CUE; q[0].auto_next_frame = 500; q[0].preferred_deck = q[2].preferred_deck = 0; q[1].preferred_deck = 1;
    start(e, q, 3, &o); ready(e, 0, f->media[0], 2000); ready(e, 1, f->media[1], 1900); gfloat output[3][8192]; render(e, 501, output);
    near(output[0][998], .25); near(output[0][1000], .125); RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(e, &r);
    g_assert_cmpuint(r.index, ==, 1); g_assert_cmpuint(r.actual_current_frame, ==, 101);
    RadioSAudioSnapshot before; radios_audio_engine_snapshot(e, &before);
    g_assert_true(radios_audio_engine_runner_previous(e, NULL)); render(e, 241, output); ready_after(e, 0, f->media[0], 2000, before.decks[0].generation);
    render(e, 1, output); near(output[0][0], .25); radios_audio_engine_runner_snapshot(e, &r); g_assert_cmpuint(r.index, ==, 0);
    radios_audio_engine_snapshot(e, &before);
    g_assert_true(radios_audio_engine_runner_next(e, NULL)); render(e, 241, output); ready_after(e, 1, f->media[1], 1900, before.decks[1].generation);
    render(e, 1, output); near(output[0][0], .125); radios_audio_engine_runner_snapshot(e, &r); g_assert_cmpuint(r.index, ==, 1);
    radios_audio_engine_unref(e);
}
static void manual_wait(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 1000), item(f, 1, 1, 0, 1500)}; q[0].mode = RADIOS_AUDIO_RUN_MANUAL; q[0].preferred_deck = 0; q[1].preferred_deck = 1;
    start(e, q, 2, &o); ready(e, 0, f->media[0], 1000); ready(e, 1, f->media[1], 1500); gfloat output[3][8192]; render(e, 1100, output);
    near(output[0][2000], 0); RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(e, &r); g_assert_cmpint(r.state, ==, RADIOS_AUDIO_RUN_MANUAL_WAIT);
    g_assert_cmpuint(r.actual_next_frame, ==, 0); RadioSAudioSnapshot s; radios_audio_engine_snapshot(e, &s); guint64 old = s.decks[1].generation;
    g_assert_true(radios_audio_engine_runner_next(e, NULL)); render(e, 241, output);
    ready_after(e, 1, f->media[1], 1500, old); render(e, 1, output); near(output[0][0], .125); radios_audio_engine_unref(e);
}
static void preferred_recycle(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 1000), item(f, 1, 1, 0, 1500)}; q[0].preferred_deck = q[1].preferred_deck = 0;
    start(e, q, 2, &o); ready(e, 0, f->media[0], 1000); gfloat output[3][8192]; render(e, 1001, output); ready(e, 0, f->media[1], 1500);
    render(e, 1, output); near(output[0][0], .125); RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(e, &r); g_assert_cmpint(r.deck_current, ==, 0);
    g_assert_true(radios_audio_engine_runner_stop(e, NULL)); q[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; q[0].overlap_frames = 100; GError *error = NULL;
    g_assert_false(radios_audio_engine_runner_configure(e, q, 2, &o, &error)); g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID); g_clear_error(&error);
    radios_audio_engine_unref(e);
}
static void windows_and_deadlines(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 2000), item(f, 1, 1, 0, 3000)}; q[0].start_utc_us = g_get_real_time() + 500000;
    q[0].latest_start_utc_us = q[0].start_utc_us + 2000000; start(e, q, 2, &o); ready(e, 0, f->media[0], 2000); ready(e, 1, f->media[1], 3000);
    g_test_message("Scheduled window, remaining microseconds %" G_GINT64_FORMAT, q[0].start_utc_us - g_get_real_time());
    gfloat output[3][8192]; render(e, 1, output); near(output[0][0], 0); RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(e, &r);
    g_assert_cmpint(r.state, ==, RADIOS_AUDIO_RUN_SCHEDULED); g_assert_cmpuint(r.actual_current_frame, ==, 0); g_usleep(550000); render(e, 1, output); near(output[0][0], .25);
    g_assert_true(radios_audio_engine_runner_stop(e, NULL)); q[0] = item(f, 0, 0, 0, 2000); q[0].latest_start_utc_us = g_get_real_time() - 1;
    start(e, q, 2, &o); ready(e, 0, f->media[0], 2000); render(e, 1, output); radios_audio_engine_runner_snapshot(e, &r);
    g_test_message("Expired start window state %d", r.state);
    g_assert_cmpint(r.state, ==, RADIOS_AUDIO_RUN_ERROR); g_assert_nonnull(strstr(r.error, "window")); near(output[0][0], 0);
    g_assert_true(radios_audio_engine_runner_stop(e, NULL)); q[0] = item(f, 0, 0, 0, 2000); q[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; q[0].overlap_frames = 500; q[1].fade_in_frames = 500;
    o.end_utc_us = g_get_real_time() + 500000; o.end_policy = RADIOS_AUDIO_RUN_END_FINISH; o.finish_grace_frames = 48000;
    start(e, q, 2, &o); ready(e, 0, f->media[0], 2000); ready(e, 1, f->media[1], 3000); render(e, 1000, output); g_usleep(550000); render(e, 1500, output);
    g_test_message("FINISH deadline expired microseconds %" G_GINT64_FORMAT, g_get_real_time() - o.end_utc_us);
    for (guint i = 0; i < 1500; ++i) near(output[0][i * 2], i < 1000 ? .25 : 0);
    radios_audio_engine_runner_snapshot(e, &r); g_assert_cmpint(r.state, ==, RADIOS_AUDIO_RUN_FINISHED); g_assert_cmpuint(r.index, ==, 0);
    radios_audio_engine_unref(e);
}
static void anchors(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 12000), item(f, 1, 1, 0, 12000)}; q[1].start_utc_us = g_get_real_time() + 500000; q[1].start_policy = RADIOS_AUDIO_RUN_START_CUT;
    start(e, q, 2, &o); ready(e, 0, f->media[0], 4800); ready(e, 1, f->media[1], 4800); gfloat output[3][8192]; render(e, 100, output); near(output[0][0], .25);
    g_usleep(550000); render(e, 241, output); RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(e, &r);
    g_assert_cmpuint(r.index, ==, 1); g_assert_cmpuint(r.actual_current_frame, ==, 241); near(output[0][480], .125);
    radios_audio_engine_unref(e);
}
static void hash_error(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 2000), item(f, 1, 1, 0, 2000)};
    q[1].media.sha256 = "0000000000000000000000000000000000000000000000000000000000000000"; start(e, q, 2, &o);
    RadioSAudioRunSnapshot r; gint64 until = g_get_monotonic_time() + 5000000;
    do { radios_audio_engine_runner_snapshot(e, &r); if (r.state == RADIOS_AUDIO_RUN_ERROR) break; g_usleep(1000); } while (g_get_monotonic_time() < until);
    g_assert_cmpint(r.state, ==, RADIOS_AUDIO_RUN_ERROR); g_assert_true(r.error[0] != '\0'); gfloat output[3][8192]; render(e, 100, output); near(output[0][0], 0);
    g_assert_true(radios_audio_engine_runner_stop(e, NULL)); radios_audio_engine_unref(e);
}
static void preroll_cancel(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    o.start_utc_us = g_get_real_time() + 10000000;
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 2000), item(f, 1, 1, 0, 2000)};
    for (guint i = 0; i < 40; ++i) { start(e, q, 2, &o); g_assert_true(radios_audio_engine_runner_stop(e, NULL)); }
    RadioSAudioSnapshot s; radios_audio_engine_snapshot(e, &s); g_assert_cmpint(s.decks[0].state, ==, RADIOS_AUDIO_EMPTY); g_assert_cmpint(s.decks[1].state, ==, RADIOS_AUDIO_EMPTY);
    RadioSAudioRunReceipt receipts[3]; g_assert_cmpuint(radios_audio_engine_runner_receipts(e, receipts, 3), ==, 0); radios_audio_engine_unref(e);
}
static void append_receipts(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; radios_audio_run_options_default(&o); o.pool_count = 3; o.deck_pool[2] = 2;
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 480), item(f, 1, 1, 0, 480), item(f, 0, 2, 0, 480)};
    q[0].preferred_deck = 1; q[1].preferred_deck = 0; q[2].preferred_deck = 2;
    start(e, q, 1, &o); ready(e, 1, f->media[0], 480); RadioSAudioRunReceipt receipts[3]; g_assert_cmpuint(radios_audio_engine_runner_receipts(e, receipts, 3), ==, 0);
    RadioSAudioRunSnapshot before, after; radios_audio_engine_runner_snapshot(e, &before); gfloat output[3][8192]; render(e, 10, output);
    g_assert_true(radios_audio_engine_runner_append(e, q + 1, 2, NULL)); radios_audio_engine_runner_snapshot(e, &after);
    g_assert_cmpuint(after.epoch, ==, before.epoch); g_assert_cmpuint(after.count, ==, 3); g_assert_true(after.owned[0]); ready(e, 0, f->media[1], 480);
    RadioSAudioRunItem invalid = q[0]; invalid.preferred_deck = 31; GError *error = NULL;
    g_assert_false(radios_audio_engine_runner_append(e, &invalid, 1, &error)); g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID); g_clear_error(&error);
    radios_audio_engine_runner_snapshot(e, &after); g_assert_cmpuint(after.count, ==, 3);
    render(e, 470, output); render(e, 481, output); ready(e, 2, f->media[0], 480); render(e, 481, output);
    g_assert_cmpuint(radios_audio_engine_runner_receipts(e, receipts, 3), ==, 3);
    for (guint i = 0; i < 3; ++i) { g_assert_cmpuint(receipts[i].epoch, ==, before.epoch); g_assert_cmpuint(receipts[i].index, ==, i); g_assert_cmpint(receipts[i].first_utc_us, >, 0); g_assert_cmpuint(receipts[i].position_frames, ==, 480); }
    g_assert_true(radios_audio_engine_runner_stop(e, NULL)); g_assert_cmpuint(radios_audio_engine_runner_receipts(e, receipts, 3), ==, 3); radios_audio_engine_unref(e);
    /* Both first-play receipts become observable in one MAIN render block;
     * deck1 precedes deck0 and must still be recorded first. */
    e = radios_audio_engine_new(FALSE, NULL); start(e, q, 2, &o);
    ready(e, 1, f->media[0], 480); ready(e, 0, f->media[1], 480); render(e, 961, output);
    g_assert_cmpuint(radios_audio_engine_runner_receipts(e, receipts, 3), ==, 2);
    for (guint i = 0; i < 2; ++i) { g_assert_cmpuint(receipts[i].index, ==, i); g_assert_cmpuint(receipts[i].occurrence, ==, i + 1); }
    radios_audio_engine_unref(e);
}
static void previous_receipts(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 12000), item(f, 1, 1, 0, 12000)};
    start(e, q, 2, &o); ready(e, 0, f->media[0], 4800); ready(e, 1, f->media[1], 4800);
    gfloat output[3][8192]; render(e, 480, output); near(output[0][0], .25);
    RadioSAudioRunReceipt receipts[3]; g_assert_cmpuint(radios_audio_engine_runner_receipts(e, receipts, 3), ==, 1);
    RadioSAudioRunReceipt first = receipts[0]; g_assert_cmpuint(first.position_frames, ==, 480);
    RadioSAudioSnapshot before; radios_audio_engine_snapshot(e, &before);
    g_assert_true(radios_audio_engine_runner_previous(e, NULL)); render(e, 241, output);
    ready_after(e, 0, f->media[0], 4800, before.decks[0].generation); render(e, 480, output); near(output[0][0], .25);
    g_assert_cmpuint(radios_audio_engine_runner_receipts(e, receipts, 3), ==, 2);
    g_assert_cmpuint(receipts[0].epoch, ==, first.epoch); g_assert_cmpuint(receipts[1].epoch, ==, first.epoch);
    g_assert_cmpuint(receipts[0].index, ==, 0); g_assert_cmpuint(receipts[1].index, ==, 0);
    g_assert_cmpuint(receipts[0].occurrence, ==, first.occurrence); g_assert_cmpuint(receipts[1].occurrence, >, first.occurrence);
    g_assert_cmpint(receipts[1].first_utc_us, >=, first.first_utc_us); g_assert_cmpuint(receipts[1].position_frames, ==, 480);
    g_assert_cmpuint(receipts[0].position_frames, ==, first.position_frames);
    g_assert_cmpint(receipts[0].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED);
    g_assert_cmpint(receipts[1].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_NONE);
    RadioSAudioRunReceipt latest; g_assert_cmpuint(radios_audio_engine_runner_receipts(e, &latest, 1), ==, 1);
    g_assert_cmpuint(latest.occurrence, ==, receipts[1].occurrence);
    g_assert_true(radios_audio_engine_runner_stop(e, NULL)); g_assert_cmpuint(radios_audio_engine_runner_receipts(e, receipts, 3), ==, 2);
    radios_audio_engine_unref(e);
}
static void terminals(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 1000), item(f, 1, 1, 0, 1500)};
    start(e, q, 2, &o); ready(e, 0, f->media[0], 1000); ready(e, 1, f->media[1], 1500);
    gfloat output[3][8192]; render(e, 400, output); RadioSAudioRunReceipt receipts[2];
    g_assert_cmpuint(radios_audio_engine_runner_receipts(e, receipts, 2), ==, 1);
    g_assert_cmpint(receipts[0].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_NONE);
    g_assert_cmpint(receipts[0].last_observed_utc_us, >=, receipts[0].first_utc_us);
    g_assert_true(radios_audio_engine_runner_pause(e, NULL)); render(e, 700, output);
    g_assert_cmpuint(radios_audio_engine_runner_receipts(e, receipts, 2), ==, 1);
    g_assert_cmpint(receipts[0].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_NONE); g_assert_cmpuint(receipts[0].position_frames, ==, 400);
    g_assert_true(radios_audio_engine_runner_resume(e, NULL)); render(e, 600, output);
    g_assert_cmpuint(radios_audio_engine_runner_receipts(e, receipts, 2), ==, 1);
    g_assert_cmpuint(receipts[0].position_frames, ==, 1000); g_assert_cmpint(receipts[0].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_NONE);
    render(e, 1, output); near(output[0][0], .125);
    g_assert_cmpuint(radios_audio_engine_runner_receipts(e, receipts, 2), ==, 2);
    g_assert_cmpint(receipts[0].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_COMPLETE); g_assert_cmpuint(receipts[0].position_frames, ==, 1000);
    g_assert_cmpint(receipts[1].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_NONE); g_assert_cmpuint(receipts[1].position_frames, ==, 1);
    g_assert_true(radios_audio_engine_runner_stop(e, NULL));
    g_assert_cmpuint(radios_audio_engine_runner_receipts(e, receipts, 2), ==, 2);
    g_assert_cmpint(receipts[0].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_COMPLETE);
    g_assert_cmpint(receipts[1].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED); g_assert_cmpuint(receipts[1].position_frames, ==, 1);
    gint64 end = receipts[1].last_observed_utc_us; render(e, 700, output);
    g_assert_cmpuint(radios_audio_engine_runner_receipts(e, receipts, 2), ==, 2);
    g_assert_cmpint(receipts[1].last_observed_utc_us, ==, end); g_assert_cmpuint(receipts[1].position_frames, ==, 1);
    radios_audio_engine_unref(e);
}
static void failed_terminal(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunItem q = item(f, 0, 0, 0, 97000); q.duration_frames = 97000;
    start(e, &q, 1, &o); ready(e, 0, f->media[0], 4800); gfloat output[3][8192]; RadioSAudioRunSnapshot run;
    for (guint i = 0; i < 200; ++i) { render(e, 4096, output); radios_audio_engine_runner_snapshot(e, &run); if (run.state == RADIOS_AUDIO_RUN_ERROR) break; g_usleep(2000); }
    g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_ERROR);
    RadioSAudioRunReceipt receipt; g_assert_cmpuint(radios_audio_engine_runner_receipts(e, &receipt, 1), ==, 1);
    g_assert_cmpint(receipt.terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_FAILED);
    g_assert_cmpuint(receipt.position_frames, ==, 96000); g_assert_cmpint(receipt.last_observed_utc_us, >=, receipt.first_utc_us);
    g_assert_true(radios_audio_engine_runner_stop(e, NULL));
    g_assert_cmpuint(radios_audio_engine_runner_receipts(e, &receipt, 1), ==, 1); g_assert_cmpint(receipt.terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_FAILED);
    radios_audio_engine_unref(e);
}
static void handover_released(RadioSAudioEngine *engine)
{
    gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    RadioSAudioRunSnapshot run; gboolean owned;
    do {
        radios_audio_engine_runner_snapshot(engine, &run); owned = FALSE;
        for (guint i = 0; i < 32; ++i) owned |= run.owned[i];
        if (run.state == RADIOS_AUDIO_RUN_FINISHED && !owned) break;
        g_usleep(1000);
    } while (g_get_monotonic_time() < deadline);
    g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_FINISHED); g_assert_false(owned); g_assert_false(run.handover_pending); g_assert_true(run.handover_completed);
}
static void handover_finish(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 1000), item(f, 1, 1, 0, 2000)};
    q[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; q[0].overlap_frames = 400;
    start(e, q, 2, &o); ready(e, 0, f->media[0], 1000); ready(e, 1, f->media[1], 2000);
    gfloat output[3][8192]; render(e, 400, output); RadioSAudioRunSnapshot run; radios_audio_engine_runner_snapshot(e, &run);
    guint64 epoch = run.epoch;
    g_assert_true(radios_audio_engine_runner_handover(e, epoch, g_get_real_time() - 1, RADIOS_AUDIO_RUN_START_FINISH, 0, 240, NULL));
    render(e, 601, output);
    for (guint i = 0; i < 601; ++i) near(output[0][i * 2], i < 600 ? .25 : 0);
    handover_released(e); radios_audio_engine_runner_snapshot(e, &run); g_assert_cmpuint(run.epoch, ==, epoch);
    RadioSAudioRunReceipt receipts[2]; g_assert_cmpuint(radios_audio_engine_runner_receipts(e, receipts, 2), ==, 1);
    g_assert_cmpuint(receipts[0].position_frames, ==, 1000); g_assert_cmpint(receipts[0].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_COMPLETE);
    /* An already audible overlap cannot be erased. Finish A, record the
     * already consumed B prefix as interrupted, and suppress further tracks. */
    start(e, q, 2, &o); ready(e, 0, f->media[0], 1000); ready(e, 1, f->media[1], 2000); render(e, 700, output);
    radios_audio_engine_runner_snapshot(e, &run); g_assert_true(radios_audio_engine_runner_handover(e, run.epoch,
        g_get_real_time() - 1, RADIOS_AUDIO_RUN_START_FINISH, 0, 240, NULL));
    render(e, 301, output); near(output[0][600], 0); handover_released(e);
    g_assert_cmpuint(radios_audio_engine_runner_receipts(e, receipts, 2), ==, 2);
    g_assert_cmpint(receipts[0].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_COMPLETE);
    g_assert_cmpint(receipts[1].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED); g_assert_cmpuint(receipts[1].position_frames, ==, 400);
    radios_audio_engine_unref(e);
}
static void handover_cut(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 10000), item(f, 1, 1, 0, 10000)};
    start(e, q, 2, &o); ready(e, 0, f->media[0], 4800); ready(e, 1, f->media[1], 4800);
    gfloat output[3][8192]; render(e, 100, output); RadioSAudioRunSnapshot run; radios_audio_engine_runner_snapshot(e, &run);
    gint64 target = g_get_real_time() + 200000;
    g_assert_true(radios_audio_engine_runner_handover(e, run.epoch, target, RADIOS_AUDIO_RUN_START_CUT, 0, 240, NULL));
    render(e, 100, output); for (guint i = 0; i < 100; ++i) near(output[0][i * 2], .25);
    radios_audio_engine_runner_snapshot(e, &run); g_assert_true(run.handover_pending); g_assert_cmpuint(run.actual_current_frame, ==, 200);
    g_usleep(220000); render(e, 241, output);
    for (guint i = 0; i < 241; ++i) near(output[0][i * 2], i < 240 ? .25 * (240 - i) / 240 : 0);
    handover_released(e); RadioSAudioRunReceipt receipt;
    g_assert_cmpuint(radios_audio_engine_runner_receipts(e, &receipt, 1), ==, 1);
    g_assert_cmpuint(receipt.position_frames, ==, 440); g_assert_cmpint(receipt.terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED);
    /* A FINISH wait expires after a bounded wall deadline, then uses the same
     * source-sample fade instead of activating the preloaded next item. */
    start(e, q, 2, &o); ready(e, 0, f->media[0], 4800); ready(e, 1, f->media[1], 4800); render(e, 100, output);
    radios_audio_engine_runner_snapshot(e, &run);
    g_assert_true(radios_audio_engine_runner_handover(e, run.epoch, g_get_real_time() - 20000, RADIOS_AUDIO_RUN_START_FINISH, 480, 240, NULL));
    render(e, 241, output); near(output[0][0], .25); near(output[0][478], .25 / 240); near(output[0][480], 0); handover_released(e);
    g_assert_cmpuint(radios_audio_engine_runner_receipts(e, &receipt, 1), ==, 1); g_assert_cmpuint(receipt.position_frames, ==, 340);
    g_assert_cmpint(receipt.terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED);
    radios_audio_engine_unref(e);
}
static void handover_cancel(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 1000), item(f, 1, 1, 0, 2000)};
    start(e, q, 2, &o); ready(e, 0, f->media[0], 1000); ready(e, 1, f->media[1], 2000);
    gfloat output[3][8192]; render(e, 100, output); RadioSAudioRunSnapshot run; radios_audio_engine_runner_snapshot(e, &run);
    GError *error = NULL;
    g_assert_false(radios_audio_engine_runner_handover(e, run.epoch + 1, g_get_real_time() + 500000,
        RADIOS_AUDIO_RUN_START_CUT, 0, 240, &error)); g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_STATE); g_clear_error(&error);
    g_assert_true(radios_audio_engine_runner_handover(e, run.epoch, g_get_real_time() + 500000,
        RADIOS_AUDIO_RUN_START_CUT, 0, 240, NULL));
    radios_audio_engine_runner_handover_cancel(e, run.epoch + 1); radios_audio_engine_runner_snapshot(e, &run); g_assert_true(run.handover_pending);
    radios_audio_engine_runner_handover_cancel(e, run.epoch); render(e, 901, output); near(output[0][1798], .25); near(output[0][1800], .125);
    radios_audio_engine_runner_snapshot(e, &run); g_assert_false(run.handover_pending); g_assert_false(run.handover_completed); g_assert_cmpuint(run.index, ==, 1);
    g_assert_true(radios_audio_engine_runner_handover(e, run.epoch, g_get_real_time() + 500000,
        RADIOS_AUDIO_RUN_START_FINISH, 0, 240, NULL));
    g_assert_true(radios_audio_engine_runner_previous(e, NULL)); radios_audio_engine_runner_snapshot(e, &run); g_assert_false(run.handover_pending); g_assert_false(run.handover_completed);
    g_assert_true(radios_audio_engine_runner_stop(e, NULL));
    /* Manual NEXT past the last entry also yields FINISHED; it must not be
     * confused with the successfully drained scheduled handover. */
    start(e, q, 1, &o); ready(e, 0, f->media[0], 1000); render(e, 100, output);
    radios_audio_engine_runner_snapshot(e, &run);
    g_assert_true(radios_audio_engine_runner_handover(e, run.epoch, g_get_real_time() + 500000,
        RADIOS_AUDIO_RUN_START_FINISH, 0, 240, NULL));
    g_assert_true(radios_audio_engine_runner_next(e, NULL)); render(e, 241, output);
    gint64 deadline = g_get_monotonic_time() + 2 * G_TIME_SPAN_SECOND;
    do { radios_audio_engine_runner_snapshot(e, &run); if (!run.owned[0] && !run.owned[1]) break; g_usleep(1000); } while (g_get_monotonic_time() < deadline);
    g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_FINISHED); g_assert_false(run.handover_pending); g_assert_false(run.handover_completed);
    g_assert_false(run.owned[0]); g_assert_false(run.owned[1]);
    /* Manual load takes ownership and invalidates the arm without disturbing
     * the new source. Its IDLE state is not a successful handover either. */
    start(e, q, 2, &o); ready(e, 0, f->media[0], 1000); ready(e, 1, f->media[1], 2000); render(e, 100, output);
    radios_audio_engine_runner_snapshot(e, &run); guint64 old_epoch = run.epoch;
    g_assert_true(radios_audio_engine_runner_handover(e, run.epoch, g_get_real_time() + 500000,
        RADIOS_AUDIO_RUN_START_CUT, 0, 240, NULL));
    g_assert_true(radios_audio_engine_load(e, 0, &q[1].media, NULL)); ready(e, 0, f->media[1], 2000);
    radios_audio_engine_runner_snapshot(e, &run); g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_IDLE);
    g_assert_cmpuint(run.epoch, >, old_epoch); g_assert_false(run.handover_pending); g_assert_false(run.handover_completed);
    g_assert_true(radios_audio_engine_unload(e, 0, NULL));
    /* Paused CUT releases its own sources with last confirmed actual cursor;
     * it neither resumes them nor creates a completion for queued media. */
    start(e, q, 2, &o); ready(e, 0, f->media[0], 1000); ready(e, 1, f->media[1], 2000); render(e, 100, output);
    g_assert_true(radios_audio_engine_runner_pause(e, NULL)); radios_audio_engine_runner_snapshot(e, &run);
    g_assert_true(radios_audio_engine_runner_handover(e, run.epoch, g_get_real_time() - 1,
        RADIOS_AUDIO_RUN_START_CUT, 0, 240, NULL)); handover_released(e);
    RadioSAudioRunReceipt receipt; g_assert_cmpuint(radios_audio_engine_runner_receipts(e, &receipt, 1), ==, 1);
    g_assert_cmpuint(receipt.position_frames, ==, 100); g_assert_cmpint(receipt.terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED);
    radios_audio_engine_unref(e);
}
static void handover_old_deadline(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    o.end_utc_us = g_get_real_time() + 200000; o.end_policy = RADIOS_AUDIO_RUN_END_CUT;
    /* Exercise the worker deadline branch without rendering more source PCM;
     * the normal 30-minute grace intentionally waits for render/drain. */
    o.finish_grace_frames = 0;
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 12000), item(f, 1, 1, 0, 12000)};
    start(e, q, 2, &o); ready(e, 0, f->media[0], 4800); ready(e, 1, f->media[1], 4800);
    gfloat output[3][8192]; render(e, 100, output); RadioSAudioRunSnapshot run; radios_audio_engine_runner_snapshot(e, &run);
    guint64 epoch = run.epoch; gint64 future_ad = g_get_real_time() + 5000000;
    g_assert_true(radios_audio_engine_runner_handover(e, epoch, future_ad, RADIOS_AUDIO_RUN_START_FINISH, 0, 240, NULL));
    g_usleep(220000); handover_released(e); radios_audio_engine_runner_snapshot(e, &run);
    g_assert_cmpuint(run.epoch, ==, epoch); g_assert_cmpuint(run.actual_current_frame, ==, 100);
    RadioSAudioRunReceipt receipt; g_assert_cmpuint(radios_audio_engine_runner_receipts(e, &receipt, 1), ==, 1);
    g_assert_cmpint(receipt.terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED); g_assert_cmpuint(receipt.position_frames, ==, 100);
    /* Installing the accepted next queue after early natural programme end
     * preserves its future UTC gate and produces no premature source PCM. */
    radios_audio_run_options_default(&o); o.start_utc_us = future_ad;
    start(e, &q[1], 1, &o); ready(e, 0, f->media[1], 4800); render(e, 1, output); near(output[0][0], 0);
    radios_audio_engine_runner_snapshot(e, &run); g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_SCHEDULED);
    g_assert_cmpuint(run.actual_current_frame, ==, 0); radios_audio_engine_unref(e);
}
static gdouble capture_peak(const gchar *path)
{
    gchar *bytes = NULL; gsize length = 0; g_assert_true(g_file_get_contents(path, &bytes, &length, NULL)); gdouble peak = 0;
    for (gsize offset = 0; offset + sizeof(gfloat) <= length; offset += sizeof(gfloat)) { gfloat value; memcpy(&value, bytes + offset, sizeof value); g_assert_true(isfinite(value)); peak = MAX(peak, fabs(value)); }
    g_free(bytes); return peak;
}
static void pulse(Fixture *f, gconstpointer unused)
{
    (void)unused; const gchar *main_sink = g_getenv("RADIOS_TEST_MAIN_SINK"), *cue_sink = g_getenv("RADIOS_TEST_CUE_SINK"), *monitor_sink = g_getenv("RADIOS_TEST_MONITOR_SINK");
    if (!main_sink || !cue_sink || !monitor_sink) { g_test_skip("Requires owned isolated Pulse null sinks"); return; }
    RadioSAudioEngine *e = radios_audio_engine_new(TRUE, NULL); RadioSAudioPreferences p; radios_audio_preferences_default(&p);
    g_strlcpy(p.main_sink, main_sink, sizeof p.main_sink); g_strlcpy(p.cue_sink, cue_sink, sizeof p.cue_sink); g_strlcpy(p.monitor_sink, monitor_sink, sizeof p.monitor_sink);
    g_assert_true(radios_audio_engine_apply_preferences(e, &p, NULL)); RadioSAudioSnapshot s; gint64 until = g_get_monotonic_time() + 5000000;
    do { radios_audio_engine_snapshot(e, &s); if (s.buses[0].ready && s.buses[1].ready) break; g_usleep(5000); } while (g_get_monotonic_time() < until);
    g_assert_true(s.buses[0].ready); g_assert_true(s.buses[1].ready);
    RadioSAudioRunOptions o; radios_audio_run_options_default(&o); RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 48000), item(f, 1, 1, 0, 96000)};
    q[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; q[0].overlap_frames = q[1].fade_in_frames = 24000; start(e, q, 2, &o); RadioSAudioRunSnapshot r;
    until = g_get_monotonic_time() + 5000000;
    gint last_state = -1, last_bucket = -1; gint64 began = g_get_monotonic_time();
    do { radios_audio_engine_runner_snapshot(e, &r); gint bucket = (gint)(r.actual_current_frame / 5000);
        if (last_state != (gint)r.state || bucket != last_bucket) { g_test_message("Pulse +%.3fs state%d index%u actual%" G_GUINT64_FORMAT "/%" G_GUINT64_FORMAT " phase%" G_GUINT64_FORMAT, (g_get_monotonic_time() - began) / 1000000.0, r.state, r.index, r.actual_current_frame, r.actual_next_frame, r.transition_frames); last_state = r.state; last_bucket = bucket; }
        if (r.state == RADIOS_AUDIO_RUN_OVERLAP && r.actual_current_frame > 26000) break;
        if (r.state == RADIOS_AUDIO_RUN_ERROR) g_error("Pulse runner: %s", r.error);
        g_usleep(5000);
    } while (g_get_monotonic_time() < until);
    if (r.state != RADIOS_AUDIO_RUN_OVERLAP) g_test_message("Captured MAIN peak%.6f", capture_peak(g_getenv("RADIOS_TEST_MAIN_CAPTURE")));
    g_assert_cmpint(r.state, ==, RADIOS_AUDIO_RUN_OVERLAP); g_assert_cmpuint(r.actual_next_frame, >, 0);
    g_assert_true(radios_audio_engine_runner_pause(e, NULL)); g_usleep(150000); RadioSAudioRunSnapshot paused; radios_audio_engine_runner_snapshot(e, &paused);
    g_assert_cmpint(paused.state, ==, RADIOS_AUDIO_RUN_PAUSED); g_usleep(100000); radios_audio_engine_runner_snapshot(e, &r);
    g_assert_cmpuint(r.actual_current_frame, ==, paused.actual_current_frame); g_assert_cmpuint(r.actual_next_frame, ==, paused.actual_next_frame);
    g_assert_true(radios_audio_engine_runner_resume(e, NULL)); until = g_get_monotonic_time() + 5000000;
    do { radios_audio_engine_runner_snapshot(e, &r); if (r.index == 1 && r.actual_current_frame > 30000) break; if (r.state == RADIOS_AUDIO_RUN_ERROR) g_error("Resume: %s", r.error); g_usleep(5000); } while (g_get_monotonic_time() < until);
    g_assert_cmpuint(r.index, ==, 1); g_assert_cmpuint(r.actual_current_frame, >, 30000);
    g_assert_true(radios_audio_engine_runner_stop(e, NULL)); g_usleep(100000);
    RadioSAudioRunReceipt receipts[2]; g_assert_cmpuint(radios_audio_engine_runner_receipts(e, receipts, 2), ==, 2);
    g_assert_cmpint(receipts[0].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_COMPLETE);
    g_assert_cmpint(receipts[1].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED);
    g_assert_cmpuint(receipts[1].position_frames, <, q[1].media.cue_out_frame);
    RadioSAudioRunItem short_item = item(f, 0, 2, 0, 4800); start(e, &short_item, 1, &o);
    gboolean queued_eof = FALSE, complete = FALSE; until = g_get_monotonic_time() + 5000000;
    do {
        radios_audio_engine_snapshot(e, &s); guint count = radios_audio_engine_runner_receipts(e, receipts, 2);
        /* All source PCM was rendered, while physical consumption is still
         * behind. Public deck EOF already waits for actual device progress. */
        if (s.decks[0].state == RADIOS_AUDIO_PLAYING && s.decks[0].buffered_frames == 0 &&
            s.decks[0].cursor_frames > 0 && s.decks[0].cursor_frames < 4800) {
            queued_eof = TRUE;
            if (count) g_assert_cmpint(receipts[0].terminal, ==, RADIOS_AUDIO_RUN_TERMINAL_NONE);
        }
        if (count && receipts[0].terminal == RADIOS_AUDIO_RUN_TERMINAL_COMPLETE) { complete = TRUE; break; }
        g_usleep(1000);
    } while (g_get_monotonic_time() < until);
    g_assert_true(queued_eof); g_assert_true(complete); g_assert_cmpuint(receipts[0].position_frames, ==, 4800);
    g_assert_cmpint(receipts[0].last_observed_utc_us, >=, receipts[0].first_utc_us);
    radios_audio_engine_unref(e);
    g_assert_cmpfloat(capture_peak(g_getenv("RADIOS_TEST_MAIN_CAPTURE")), >, .27); near(capture_peak(g_getenv("RADIOS_TEST_CUE_CAPTURE")), 0);
}
static void independent_players(Fixture *fixture, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE,NULL); GError *error = NULL;
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    RadioSAudioRunItem first[] = {item(fixture,0,0,0,30000),item(fixture,1,1,0,30000)};
    RadioSAudioRunItem second[] = {item(fixture,1,2,0,60000)};
    g_assert_true(radios_audio_engine_player_runner_configure(engine,0,first,2,&options,&error)); g_assert_no_error(error);
    g_assert_true(radios_audio_engine_player_runner_configure(engine,1,second,1,&options,&error)); g_assert_no_error(error);
    g_assert_true(radios_audio_engine_player_runner_action(engine,0,RADIOS_AUDIO_PLAYER_RUN_START,&error)); g_assert_no_error(error);
    g_assert_true(radios_audio_engine_player_runner_action(engine,1,RADIOS_AUDIO_PLAYER_RUN_START,&error)); g_assert_no_error(error);
    ready(engine,0,fixture->media[0],4800); ready(engine,1,fixture->media[1],4800);
    gfloat output[3][8192]; render(engine,1024,output); near(output[0][200],.375);
    RadioSAudioRunSnapshot before, after;
    radios_audio_engine_player_runner_snapshot(engine,1,&before);
    g_assert_true(radios_audio_engine_player_runner_action(engine,0,RADIOS_AUDIO_PLAYER_RUN_PAUSE,&error)); g_assert_no_error(error);
    render(engine,512,output); near(output[0][200],.125);
    radios_audio_engine_player_runner_snapshot(engine,1,&after);
    g_assert_cmpuint(after.epoch,==,before.epoch); g_assert_cmpint(after.state,==,RADIOS_AUDIO_RUN_PLAYING);
    g_assert_cmpuint(after.actual_current_frame,>,before.actual_current_frame);
    g_assert_true(radios_audio_engine_player_runner_action(engine,0,RADIOS_AUDIO_PLAYER_RUN_RESUME,&error)); g_assert_no_error(error);
    render(engine,512,output); near(output[0][200],.375);
    RadioSAudioSnapshot audio; radios_audio_engine_snapshot(engine,&audio); guint64 generation = audio.decks[0].generation;
    g_assert_true(radios_audio_engine_player_runner_action(engine,0,RADIOS_AUDIO_PLAYER_RUN_NEXT,&error)); g_assert_no_error(error);
    for (guint index = 0; index < 50; ++index) { render(engine,128,output); g_usleep(1000); }
    ready_after(engine,0,fixture->media[1],4800,generation); render(engine,512,output); near(output[0][200],.25);
    radios_audio_engine_player_runner_snapshot(engine,1,&after); g_assert_cmpuint(after.index,==,0); g_assert_cmpuint(after.epoch,==,before.epoch);
    g_assert_true(radios_audio_engine_player_runner_action(engine,0,RADIOS_AUDIO_PLAYER_RUN_STOP,&error)); g_assert_no_error(error);
    for (guint index = 0; index < 20; ++index) { render(engine,128,output); g_usleep(1000); }
    near(output[0][200],.125);
    options.pool_count = 1; options.deck_pool[0] = 1;
    g_assert_true(radios_audio_engine_runner_configure(engine,first,1,&options,&error)); g_assert_no_error(error);
    g_assert_false(radios_audio_engine_runner_start(engine,&error)); g_assert_error(error,RADIOS_AUDIO_ERROR_DOMAIN,RADIOS_AUDIO_ERROR_STATE); g_clear_error(&error);
    radios_audio_engine_player_runner_snapshot(engine,1,&after); g_assert_cmpint(after.state,==,RADIOS_AUDIO_RUN_PLAYING);
    RadioSAudioRunReceipt receipts[8];
    g_assert_cmpuint(radios_audio_engine_player_runner_receipts(engine,0,receipts,8),>=,2);
    g_assert_cmpuint(radios_audio_engine_player_runner_receipts(engine,1,receipts,8),==,1);
    g_assert_cmpuint(radios_audio_engine_runner_receipts(engine,receipts,8),==,0);
    radios_audio_engine_unref(engine);
}
static void independent_eof(Fixture *fixture, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE,NULL); GError *error = NULL;
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    RadioSAudioRunItem first[] = {item(fixture,0,0,0,1000),item(fixture,1,1,0,1000)};
    RadioSAudioRunItem second[] = {item(fixture,1,2,0,48000)};
    g_assert_true(radios_audio_engine_player_runner_configure(engine,0,first,2,&options,&error)); g_assert_no_error(error);
    g_assert_true(radios_audio_engine_player_runner_configure(engine,1,second,1,&options,&error)); g_assert_no_error(error);
    g_assert_true(radios_audio_engine_player_runner_action(engine,0,RADIOS_AUDIO_PLAYER_RUN_START,&error)); g_assert_no_error(error);
    g_assert_true(radios_audio_engine_player_runner_action(engine,1,RADIOS_AUDIO_PLAYER_RUN_START,&error)); g_assert_no_error(error);
    ready(engine,0,fixture->media[0],1000); ready(engine,1,fixture->media[1],1000);
    gfloat output[3][8192]; RadioSAudioRunSnapshot ended = {0}, other = {0};
    radios_audio_engine_player_runner_snapshot(engine,1,&other); guint64 epoch = other.epoch;
    for (guint block = 0; block < 200; ++block) {
        render(engine,128,output); radios_audio_engine_player_runner_snapshot(engine,0,&ended);
        if (ended.state == RADIOS_AUDIO_RUN_FINISHED) break;
        g_usleep(1000);
    }
    g_assert_cmpint(ended.state,==,RADIOS_AUDIO_RUN_FINISHED); g_assert_cmpuint(ended.index,==,1);
    radios_audio_engine_player_runner_snapshot(engine,1,&other);
    g_assert_cmpint(other.state,==,RADIOS_AUDIO_RUN_PLAYING); g_assert_cmpuint(other.epoch,==,epoch);
    render(engine,512,output); near(output[0][200],.125);
    RadioSAudioRunReceipt receipts[4];
    g_assert_cmpuint(radios_audio_engine_player_runner_receipts(engine,0,receipts,4),==,2);
    g_assert_cmpint(receipts[0].terminal,==,RADIOS_AUDIO_RUN_TERMINAL_COMPLETE);
    g_assert_cmpint(receipts[1].terminal,==,RADIOS_AUDIO_RUN_TERMINAL_COMPLETE);
    g_assert_cmpuint(radios_audio_engine_player_runner_receipts(engine,1,receipts,4),==,1);
    g_assert_cmpint(receipts[0].terminal,==,RADIOS_AUDIO_RUN_TERMINAL_NONE);
    radios_audio_engine_unref(engine);
}
static void seek_keeps_autodj(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    RadioSAudioRunItem queue[] = {item(f, 0, 0, 0, 3000), item(f, 1, 1, 0, 3000)};
    queue[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; queue[0].overlap_frames = queue[1].fade_in_frames = 500;
    start(engine, queue, 2, &options); ready(engine, 0, f->media[0], 3000); ready(engine, 1, f->media[1], 3000);
    gfloat output[3][8192]; render(engine, 100, output);
    RadioSAudioSnapshot before, after; radios_audio_engine_snapshot(engine, &before);
    g_assert_true(radios_audio_engine_seek_if_generation(engine, 0, before.decks[0].generation, 500, NULL));
    ready_after(engine, 0, f->media[0], 2500, before.decks[0].generation);
    radios_audio_engine_snapshot(engine, &after);
    g_assert_cmpuint(after.decks[1].generation, ==, before.decks[1].generation);
    RadioSAudioRunSnapshot run; radios_audio_engine_runner_snapshot(engine, &run);
    g_assert_true(run.owned[0]); g_assert_true(run.owned[1]); g_assert_cmpuint(run.index, ==, 0);
    render(engine, 10, output); near(output[0][0], .25);
    g_assert_true(radios_audio_engine_runner_pause(engine, NULL));
    g_assert_true(radios_audio_engine_seek(engine, 0, 1000, NULL)); ready(engine, 0, f->media[0], 2000);
    render(engine, 100, output); near(output[0][0], 0);
    radios_audio_engine_runner_snapshot(engine, &run); g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_PAUSED);
    g_assert_true(radios_audio_engine_runner_resume(engine, NULL)); render(engine, 1600, output);
    radios_audio_engine_runner_snapshot(engine, &run); g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_OVERLAP);
    GError *error = NULL;
    g_assert_false(radios_audio_engine_seek(engine, 0, 1500, &error)); g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_STATE); g_clear_error(&error);
    g_assert_false(radios_audio_engine_seek(engine, 1, 1000, &error)); g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_STATE); g_clear_error(&error);
    render(engine, 1000, output); radios_audio_engine_runner_snapshot(engine, &run); g_assert_cmpuint(run.index, ==, 1);
    radios_audio_engine_unref(engine);
}
static void adoption_is_atomic(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    RadioSAudioRunItem queue[] = {item(f, 0, 0, 2000, 5000), item(f, 1, 1, 0, 3000)};
    queue[0].preferred_deck = 0;
    RadioSAudioLoadSpec manual = queue[0].media; manual.cue_in_frame = 0; manual.cue_out_frame = 96000;
    g_assert_true(radios_audio_engine_load(engine, 0, &manual, NULL)); ready(engine, 0, f->media[0], 5000);
    g_assert_true(radios_audio_engine_load(engine, 1, &queue[1].media, NULL)); ready(engine, 1, f->media[1], 3000);
    g_assert_true(radios_audio_engine_runner_configure(engine, queue, 2, &options, NULL));
    RadioSAudioSnapshot before, after; radios_audio_engine_snapshot(engine, &before); GError *error = NULL;
    g_assert_false(radios_audio_engine_runner_start_replacing(engine, 0, before.decks[0].generation + 1, &error)); g_clear_error(&error);
    radios_audio_engine_snapshot(engine, &after);
    g_assert_cmpuint(after.decks[1].generation, ==, before.decks[1].generation); g_assert_cmpstr(after.decks[1].media_id, ==, f->media[1]);
    g_assert_true(radios_audio_engine_runner_start_replacing(engine, 0, before.decks[0].generation, NULL));
    ready_after(engine, 0, f->media[0], 3000, before.decks[0].generation); ready(engine, 1, f->media[1], 3000);
    radios_audio_engine_snapshot(engine, &after); g_assert_cmpuint(after.decks[0].cue_in_frame, ==, 2000); g_assert_cmpuint(after.decks[0].cue_out_frame, ==, 5000);
    gfloat output[3][8192]; render(engine, 3100, output); RadioSAudioRunSnapshot run; radios_audio_engine_runner_snapshot(engine, &run);
    g_assert_cmpuint(run.index, ==, 1); near(output[0][3000 * 2], .125); radios_audio_engine_unref(engine);
}
static void estimated_eof_rounding(Fixture *f, gconstpointer unused)
{
    (void)unused;
    for (guint estimated = 0; estimated < 2; ++estimated) {
        RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
        RadioSAudioRunItem queue[] = {item(f, 0, 0, 90000, 96001), item(f, 1, 1, 0, 3000)};
        queue[0].duration_frames = 96001; queue[0].estimated_duration = estimated;
        queue[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; queue[0].overlap_frames = queue[1].fade_in_frames = 1000;
        start(engine, queue, 2, &options); ready(engine, 0, f->media[0], 6000); ready(engine, 1, f->media[1], 3000);
        gfloat output[3][8192]; render(engine, 4096, output); render(engine, 4096, output);
        RadioSAudioRunSnapshot run; radios_audio_engine_runner_snapshot(engine, &run);
        g_assert_cmpint(run.state, ==, estimated ? RADIOS_AUDIO_RUN_FINISHED : RADIOS_AUDIO_RUN_ERROR);
        radios_audio_engine_unref(engine);
    }
}

static void autodj_exit_preserves_physical_tail(Fixture *f, gconstpointer unused)
{
    (void)unused;
    /* Give the measured grid a genuine four-second file while the physical
     * cue span still ends at two seconds. */
    gchar *wav = NULL; gsize size = 0; g_assert_true(g_file_get_contents(f->path[0], &wav, &size, NULL));
    g_assert_cmpuint(size, ==, 44 + 96000 * 4); wav = g_realloc(wav, 44 + 192000 * 4);
    memcpy(wav + 44 + 96000 * 4, wav + 44, 96000 * 4);
    guint32 riff_size = GUINT32_TO_LE(192000 * 4 + 36), data_size = GUINT32_TO_LE(192000 * 4);
    memcpy(wav + 4, &riff_size, 4); memcpy(wav + 40, &data_size, 4);
    g_assert_true(g_file_set_contents(f->path[0], wav, 44 + 192000 * 4, NULL));
    g_free(f->sha[0]); f->sha[0] = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (guchar *)wav, 44 + 192000 * 4); g_free(wav);
    for (guint synced = 0; synced < 2; ++synced) {
        RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
        RadioSAudioRunItem queue[] = {item(f, 0, 0, 48000, 96000), item(f, 1, 1, 24000, 90000)};
        queue[0].duration_frames = 192000;
        queue[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; queue[0].autodj_out_frame = 72000; queue[0].overlap_frames = queue[1].fade_in_frames = 12000;
        queue[0].beat_sync = synced; queue[0].rhythm = (RadioSRhythmAnalysis){.bpm = 120, .first_beat_ms = 0, .confidence = .9, .analyzed_ms = 4000};
        start(engine, queue, 2, &options); ready(engine, 0, f->media[0], 48000); ready(engine, 1, f->media[1], 48000);
        gfloat output[3][8192]; for (guint n = 0; n < 6; ++n) render(engine, 4096, output);
        RadioSAudioRunSnapshot run; radios_audio_engine_runner_snapshot(engine, &run); g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_OVERLAP);
        g_assert_cmpuint(run.actual_next_frame, ==, 24576);
        RadioSAudioSnapshot audio; radios_audio_engine_snapshot(engine, &audio); g_assert_cmpuint(audio.decks[0].cue_out_frame, ==, 96000);
        for (guint n = 0; n < 3; ++n) render(engine, 4096, output);
        radios_audio_engine_runner_snapshot(engine, &run); g_assert_cmpuint(run.index, ==, 1);
        near(output[0][(4096 - 1) * 2], .125); radios_audio_engine_unref(engine);
    }
}


static void autodj_short_incoming_span(Fixture *f, gconstpointer explicit_exit)
{
    gboolean has_exit = GPOINTER_TO_UINT(explicit_exit) != 0;
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    options.pool_count = 3; options.deck_pool[2] = 2;
    RadioSAudioRunItem queue[] = {item(f, 0, 0, 1000, 12000), item(f, 1, 1, 2000, 12000), item(f, 0, 2, 4000, 8000)};
    queue[0].mode = queue[1].mode = RADIOS_AUDIO_RUN_CROSSFADE;
    queue[0].autodj_out_frame = has_exit ? 2000 : 0; queue[1].autodj_out_frame = 3000;
    queue[0].overlap_frames = queue[1].fade_in_frames = 6000;
    queue[1].overlap_frames = queue[2].fade_in_frames = 1000;
    for (guint i = 0; i < 3; ++i) queue[i].preferred_deck = (gint)i;
    start(engine, queue, 3, &options);
    ready(engine, 0, f->media[0], 11000); ready(engine, 1, f->media[1], 10000);
    gfloat output[3][8192];
    for (guint left = has_exit ? 1000 : 10000; left;) {
        guint frames = MIN(left, 4096u); render(engine, frames, output);
        for (guint frame = 0; frame < frames; ++frame) near(output[0][frame * 2], .25);
        left -= frames;
    }
    render(engine, 1000, output);
    RadioSAudioTransition envelope = {1000, 1000, 1000, 0};
    for (guint frame = 0; frame < 1000; ++frame) {
        gdouble a, b; radios_audio_transition_gains(&envelope, frame, &a, &b);
        near(output[0][frame * 2], .25 * a + .125 * b);
    }
    render(engine, 1, output); RadioSAudioRunSnapshot run; radios_audio_engine_runner_snapshot(engine, &run);
    g_assert_cmpuint(run.index, ==, 1); g_assert_cmpuint(run.actual_current_frame, ==, 3000);
    ready(engine, 2, f->media[0], 4000); render(engine, 1, output); radios_audio_engine_runner_snapshot(engine, &run);
    g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_OVERLAP);
    g_assert_cmpuint(run.actual_current_frame, ==, 3001); g_assert_cmpuint(run.actual_next_frame, ==, 4001);
    near(output[0][0], .125);
    RadioSAudioSnapshot audio; radios_audio_engine_snapshot(engine, &audio);
    g_assert_cmpuint(audio.decks[1].cue_out_frame, ==, 12000);
    render(engine, 1000, output); radios_audio_engine_runner_snapshot(engine, &run);
    g_assert_cmpuint(run.index, ==, 2); near(output[0][1998], .25);
    radios_audio_engine_unref(engine);
}


static void autodj_beat_short_span(Fixture *f, gconstpointer unused)
{
    (void)unused;
    gchar *wav = NULL; gsize size = 0; g_assert_true(g_file_get_contents(f->path[0], &wav, &size, NULL));
    g_assert_cmpuint(size, ==, 44 + 96000 * 4); wav = g_realloc(wav, 44 + 192000 * 4);
    memcpy(wav + 44 + 96000 * 4, wav + 44, 96000 * 4);
    guint32 riff_size = GUINT32_TO_LE(192000 * 4 + 36), data_size = GUINT32_TO_LE(192000 * 4);
    memcpy(wav + 4, &riff_size, 4); memcpy(wav + 40, &data_size, 4);
    g_assert_true(g_file_set_contents(f->path[0], wav, 44 + 192000 * 4, NULL));
    g_free(f->sha[0]); f->sha[0] = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (guchar *)wav, 44 + 192000 * 4); g_free(wav);
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    RadioSAudioRunItem queue[] = {item(f, 0, 0, 48000, 96000), item(f, 1, 1, 24000, 90000)};
    queue[0].duration_frames = 192000; queue[0].mode = RADIOS_AUDIO_RUN_CROSSFADE;
    queue[0].autodj_out_frame = 49200; queue[0].overlap_frames = queue[1].fade_in_frames = 1200;
    queue[0].beat_sync = TRUE;
    queue[0].rhythm = (RadioSRhythmAnalysis){.bpm = 120, .first_beat_ms = 0, .confidence = .9, .analyzed_ms = 4000,
        .has_downbeat = TRUE, .downbeat_ms = 0};
    start(engine, queue, 2, &options); ready(engine, 0, f->media[0], 48000); ready(engine, 1, f->media[1], 48000);
    gfloat output[3][8192]; render(engine, 1200, output);
    for (guint frame = 0; frame < 1200; ++frame) near(output[0][frame * 2], .25);
    RadioSAudioRunSnapshot run; radios_audio_engine_runner_snapshot(engine, &run);
    g_assert_cmpint(run.state, ==, RADIOS_AUDIO_RUN_PLAYING); g_assert_cmpuint(run.actual_current_frame, ==, 49200);
    render(engine, 1200, output); RadioSAudioTransition envelope = {1200, 1200, 1200, 0};
    for (guint frame = 0; frame < 1200; ++frame) {
        gdouble a, b; radios_audio_transition_gains(&envelope, frame, &a, &b);
        near(output[0][frame * 2], .25 * a + .125 * b);
    }
    render(engine, 1, output); radios_audio_engine_runner_snapshot(engine, &run);
    g_assert_cmpuint(run.index, ==, 1); near(output[0][0], .125); radios_audio_engine_unref(engine);
}


static void autodj_tempo_incoming_fade(Fixture *f, gconstpointer unused)
{
    (void)unused;
    RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    RadioSAudioRunItem queue[] = {item(f, 0, 0, 1000, 12000), item(f, 1, 1, 2000, 12000)};
    queue[0].mode = queue[1].mode = RADIOS_AUDIO_RUN_CROSSFADE;
    queue[0].autodj_out_frame = 2000; queue[1].autodj_out_frame = 3000;
    queue[0].overlap_frames = queue[1].fade_in_frames = 6000; queue[1].tempo_ratio = .84;
    start(engine, queue, 2, &options); ready(engine, 0, f->media[0], 11000); ready(engine, 1, f->media[1], 10000);
    gfloat output[3][8192]; render(engine, 1000, output); render(engine, 1191, output);
    RadioSAudioTransition envelope = {1191, 1191, 1191, 0};
    for (guint frame = 0; frame < 1191; ++frame) {
        gdouble a, b; radios_audio_transition_gains(&envelope, frame, &a, &b);
        g_assert_cmpfloat(fabs(output[0][frame * 2] - (.25 * a + .125 * b)), <, .0001);
    }
    render(engine, 1, output); RadioSAudioRunSnapshot run; radios_audio_engine_runner_snapshot(engine, &run);
    g_assert_cmpuint(run.index, ==, 1); g_assert_cmpfloat(fabs(output[0][0] - .125), <, .0001);
    render(engine, 100, output);
    for (guint frame = 0; frame < 100; ++frame) g_assert_cmpfloat(fabs(output[0][frame * 2] - .125), <, .0001);
    radios_audio_engine_unref(engine);
}

static void disjoint_scopes_crossfade_and_pause(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); GError *error = NULL;
    RadioSAudioRunOptions first_options, second_options;
    radios_audio_run_options_default(&first_options); first_options.pool_count = 2;
    first_options.deck_pool[0] = 0; first_options.deck_pool[1] = 2;
    second_options = first_options; second_options.deck_pool[0] = 1; second_options.deck_pool[1] = 3;
    RadioSAudioRunItem first[] = {item(f, 0, 0, 0, 6000), item(f, 1, 1, 0, 12000)};
    RadioSAudioRunItem second[] = {item(f, 1, 2, 0, 20000), item(f, 0, 1, 0, 20000)};
    first[0].preferred_deck = 0; first[1].preferred_deck = 2;
    second[0].preferred_deck = 1; second[1].preferred_deck = 3;
    first[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; first[0].overlap_frames = first[1].fade_in_frames = 2000;
    second[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; second[0].overlap_frames = second[1].fade_in_frames = 1000;
    g_assert_true(radios_audio_engine_scope_runner_configure(engine, 0, first, 2, &first_options, &error)); g_assert_no_error(error);
    g_assert_true(radios_audio_engine_scope_runner_configure(engine, 1, second, 2, &second_options, &error)); g_assert_no_error(error);
    g_assert_true(radios_audio_engine_scope_runner_start_replacing(engine, 0, -1, 0, &error)); g_assert_no_error(error);
    g_assert_true(radios_audio_engine_scope_runner_start_replacing(engine, 1, -1, 0, &error)); g_assert_no_error(error);
    ready(engine, 0, f->media[0], 6000); ready(engine, 2, f->media[1], 12000);
    ready(engine, 1, f->media[1], 20000); ready(engine, 3, f->media[0], 20000);
    gfloat output[3][8192]; render(engine, 4000, output); near(output[0][200], .375);
    render(engine, 500, output);
    RadioSAudioTransition envelope = {2000, 2000, 2000, 0};
    for (guint frame = 0; frame < 500; ++frame) {
        gdouble a, b; radios_audio_transition_gains(&envelope, frame, &a, &b);
        near(output[0][frame * 2], .125 + .25 * a + .125 * b);
    }
    RadioSAudioRunSnapshot first_snap, second_before, second_after;
    radios_audio_engine_player_runner_snapshot(engine, 0, &first_snap);
    radios_audio_engine_player_runner_snapshot(engine, 1, &second_before);
    g_assert_cmpint(first_snap.state, ==, RADIOS_AUDIO_RUN_OVERLAP);
    g_assert_cmpint(first_snap.deck_current, ==, 0); g_assert_cmpint(first_snap.deck_next, ==, 2);
    g_assert_true(first_snap.owned[0]); g_assert_true(first_snap.owned[2]); g_assert_false(first_snap.owned[1]);
    g_assert_true(radios_audio_engine_player_runner_action(engine, 0, RADIOS_AUDIO_PLAYER_RUN_PAUSE, &error)); g_assert_no_error(error);
    render(engine, 1000, output); near(output[0][200], .125);
    radios_audio_engine_player_runner_snapshot(engine, 1, &second_after);
    g_assert_cmpuint(second_after.actual_current_frame, ==, second_before.actual_current_frame + 1000);
    g_assert_cmpuint(second_after.epoch, ==, second_before.epoch); g_assert_cmpuint(second_after.index, ==, 0);
    RadioSAudioRunOptions conflicting = first_options; conflicting.deck_pool[0] = 4;
    RadioSAudioRunItem conflict[] = {first[0], first[1]}; conflict[0].preferred_deck = 4;
    g_assert_false(radios_audio_engine_scope_runner_configure(engine, 4, conflict, 2, &conflicting, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_STATE); g_clear_error(&error);
    /* Invalid pool definitions leave both live queues and their PCM intact. */
    conflicting.deck_pool[1] = 4; conflict[1].preferred_deck = 4;
    g_assert_false(radios_audio_engine_scope_runner_configure(engine, 4, conflict, 2, &conflicting, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID); g_clear_error(&error);
    g_assert_false(radios_audio_engine_scope_runner_configure(engine, 4, second, 2, &second_options, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID); g_clear_error(&error);
    g_assert_true(radios_audio_engine_player_runner_action(engine, 0, RADIOS_AUDIO_PLAYER_RUN_RESUME, &error)); g_assert_no_error(error);
    render(engine, 1500, output);
    for (guint frame = 0; frame < 1500; ++frame) {
        gdouble a, b; radios_audio_transition_gains(&envelope, 500 + frame, &a, &b);
        near(output[0][frame * 2], .125 + .25 * a + .125 * b);
    }
    render(engine, 100, output); near(output[0][100], .25);
    radios_audio_engine_player_runner_snapshot(engine, 0, &first_snap);
    g_assert_cmpint(first_snap.deck_current, ==, 2); g_assert_cmpuint(first_snap.index, ==, 1);
    g_assert_true(radios_audio_engine_player_runner_action(engine, 0, RADIOS_AUDIO_PLAYER_RUN_STOP, &error)); g_assert_no_error(error);
    render(engine, 500, output); near(output[0][200], .125);
    radios_audio_engine_player_runner_snapshot(engine, 1, &second_after);
    g_assert_cmpint(second_after.state, ==, RADIOS_AUDIO_RUN_PLAYING); g_assert_cmpuint(second_after.epoch, ==, second_before.epoch);
    RadioSAudioRunReceipt receipts[8];
    g_assert_cmpuint(radios_audio_engine_player_runner_receipts(engine, 0, receipts, 8), >=, 2);
    g_assert_cmpuint(radios_audio_engine_player_runner_receipts(engine, 1, receipts, 8), ==, 1);
    radios_audio_engine_unref(engine);
}
static void scope_adoption_generation_preserves_unrelated_pcm(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL); GError *error = NULL;
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options); options.deck_pool[1] = 2;
    RadioSAudioRunItem queue[] = {item(f, 0, 0, 0, 6000), item(f, 1, 1, 0, 12000)};
    queue[0].preferred_deck = 0; queue[1].preferred_deck = 2;
    queue[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; queue[0].overlap_frames = queue[1].fade_in_frames = 2000;
    g_assert_true(radios_audio_engine_load(engine, 0, &queue[0].media, &error)); g_assert_no_error(error);
    RadioSAudioRunItem unrelated = item(f, 1, 2, 0, 30000);
    g_assert_true(radios_audio_engine_load(engine, 1, &unrelated.media, &error)); g_assert_no_error(error);
    ready(engine, 0, f->media[0], 6000); ready(engine, 1, f->media[1], 30000);
    g_assert_true(radios_audio_engine_play(engine, 0, &error)); g_assert_no_error(error);
    g_assert_true(radios_audio_engine_play(engine, 1, &error)); g_assert_no_error(error);
    gfloat output[3][8192]; render(engine, 500, output); near(output[0][200], .375);
    RadioSAudioSnapshot before, after; radios_audio_engine_snapshot(engine, &before);
    g_assert_true(radios_audio_engine_scope_runner_configure(engine, 0, queue, 2, &options, &error)); g_assert_no_error(error);
    g_assert_false(radios_audio_engine_scope_runner_start_replacing(engine, 0, 0, before.decks[0].generation + 1, &error));
    g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_STATE); g_clear_error(&error);
    radios_audio_engine_snapshot(engine, &after);
    g_assert_cmpuint(after.decks[0].generation, ==, before.decks[0].generation);
    g_assert_cmpuint(after.decks[0].cursor_frames, ==, before.decks[0].cursor_frames);
    g_assert_cmpuint(after.decks[1].generation, ==, before.decks[1].generation);
    g_assert_true(radios_audio_engine_scope_runner_start_replacing(engine, 0, 0, before.decks[0].generation, &error)); g_assert_no_error(error);
    ready(engine, 2, f->media[1], 12000); render(engine, 500, output); near(output[0][200], .375);
    radios_audio_engine_snapshot(engine, &after);
    g_assert_cmpuint(after.decks[0].generation, ==, before.decks[0].generation);
    g_assert_cmpuint(after.decks[0].cursor_frames, ==, before.decks[0].cursor_frames + 500);
    g_assert_cmpuint(after.decks[1].generation, ==, before.decks[1].generation);
    g_assert_cmpuint(after.decks[1].cursor_frames, ==, before.decks[1].cursor_frames + 500);
    radios_audio_engine_unref(engine);
}
static void copyright_delayed_start_skips_item(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    RadioSAudioRunItem queue[] = {item(f, 0, 0, 0, 5000), item(f, 1, 1, 0, 10000)};
    gint64 now = g_get_real_time();
    queue[0].copyright = (RadioSAudioCopyrightWindow){TRUE, FALSE, now - G_USEC_PER_SEC, now + 100000};
    g_assert_true(radios_audio_engine_runner_configure(engine, queue, 2, &options, NULL));
    g_usleep(150000); /* was valid when queued, expired before the delayed start */
    g_assert_true(radios_audio_engine_runner_start(engine, NULL)); ready(engine, 0, f->media[1], 10000);
    gfloat output[3][8192]; render(engine, 1000, output);
    for (guint i = 0; i < 1000; ++i) near(output[0][i * 2], .125);
    RadioSAudioRunSnapshot state; radios_audio_engine_runner_snapshot(engine, &state);
    g_assert_cmpuint(state.count, ==, 2); g_assert_cmpuint(state.index, ==, 1);
    RadioSAudioRunReceipt receipts[4]; guint count = radios_audio_engine_runner_receipts(engine, receipts, 4);
    g_assert_cmpuint(count, ==, 1); g_assert_cmpuint(receipts[0].index, ==, 1);
    radios_audio_engine_unref(engine);
}
static void copyright_preload_expiry_skips_middle(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    RadioSAudioRunItem queue[] = {item(f, 0, 0, 0, 2000), item(f, 1, 1, 0, 5000), item(f, 0, 2, 0, 10000)};
    queue[1].copyright = (RadioSAudioCopyrightWindow){TRUE, FALSE, g_get_real_time() - G_USEC_PER_SEC, g_get_real_time() + 10 * G_USEC_PER_SEC};
    start(engine, queue, 3, &options); ready(engine, 0, f->media[0], 2000); ready(engine, 1, f->media[1], 5000);
    RadioSAudioSnapshot before; radios_audio_engine_snapshot(engine, &before);
    RadioSAudioCopyrightWindow windows[] = {queue[0].copyright, queue[1].copyright, queue[2].copyright};
    windows[1].ends_at_utc_us = g_get_real_time() - 1;
    g_assert_true(radios_audio_engine_runner_copyright(engine, -1, windows, 3, NULL));
    ready_after(engine, 1, f->media[0], 10000, before.decks[1].generation);
    gfloat output[3][8192]; render(engine, 2001, output); near(output[0][4000], .25);
    RadioSAudioRunSnapshot state; radios_audio_engine_runner_snapshot(engine, &state);
    g_assert_cmpuint(state.count, ==, 3); g_assert_cmpuint(state.index, ==, 2);
    RadioSAudioRunReceipt receipts[4]; guint count = radios_audio_engine_runner_receipts(engine, receipts, 4);
    g_assert_cmpuint(count, ==, 2);
    g_assert_cmpuint(receipts[0].index, ==, 0); g_assert_cmpuint(receipts[1].index, ==, 2);
    radios_audio_engine_unref(engine);
}
static void copyright_overlap_expiry_preserves_other_pcm(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    RadioSAudioRunItem queue[] = {item(f, 0, 0, 0, 2000), item(f, 1, 1, 0, 10000)};
    queue[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; queue[0].overlap_frames = queue[1].fade_in_frames = 1000;
    queue[0].copyright = (RadioSAudioCopyrightWindow){TRUE, FALSE, g_get_real_time() - G_USEC_PER_SEC, g_get_real_time() + 10 * G_USEC_PER_SEC};
    RadioSAudioRunItem unrelated = item(f, 1, 2, 0, 30000);
    g_assert_true(radios_audio_engine_load(engine, 2, &unrelated.media, NULL)); ready(engine, 2, f->media[1], 30000);
    g_assert_true(radios_audio_engine_play(engine, 2, NULL));
    start(engine, queue, 2, &options); ready(engine, 0, f->media[0], 2000); ready(engine, 1, f->media[1], 10000);
    gfloat output[3][8192]; render(engine, 1400, output);
    RadioSAudioCopyrightWindow windows[] = {queue[0].copyright, queue[1].copyright};
    windows[0].ends_at_utc_us = g_get_real_time() - 1;
    g_assert_true(radios_audio_engine_runner_copyright(engine, -1, windows, 2, NULL));
    render(engine, 100, output);
    /* Incoming track retains its already consumed position and fade-in; the
     * unrelated .125 channel stays audible when the expired .25 source retires. */
    near(output[0][0], .125 + .125 * sin(G_PI / 2 * .4));
    RadioSAudioRunSnapshot state; radios_audio_engine_runner_snapshot(engine, &state);
    g_assert_cmpuint(state.index, ==, 1); g_assert_cmpint(state.state, !=, RADIOS_AUDIO_RUN_ERROR);
    RadioSAudioSnapshot after; radios_audio_engine_snapshot(engine, &after);
    g_assert_cmpuint(after.decks[2].cursor_frames, ==, 1500);
    radios_audio_engine_unref(engine);
}
static void copyright_toggle_off_pause_resume(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    RadioSAudioRunItem queue[] = {item(f, 0, 0, 0, 50000), item(f, 1, 1, 0, 10000)};
    queue[0].copyright = (RadioSAudioCopyrightWindow){TRUE, FALSE, g_get_real_time() - G_USEC_PER_SEC, g_get_real_time() + 250000};
    start(engine, queue, 2, &options); ready(engine, 0, f->media[0], 48000); ready(engine, 1, f->media[1], 10000);
    gfloat output[3][8192]; render(engine, 100, output);
    g_assert_true(radios_audio_engine_runner_pause(engine, NULL));
    RadioSAudioCopyrightWindow windows[2] = {{0}, {0}};
    g_assert_true(radios_audio_engine_runner_copyright(engine, -1, windows, 2, NULL));
    g_usleep(300000); g_assert_true(radios_audio_engine_runner_resume(engine, NULL)); render(engine, 100, output); near(output[0][0], .25);
    RadioSAudioRunSnapshot state; radios_audio_engine_runner_snapshot(engine, &state);
    g_assert_cmpuint(state.index, ==, 0); g_assert_cmpuint(state.actual_current_frame, ==, 200);
    /* Restore the elapsed constraint: the current source is skipped on resume,
     * and the otherwise valid successor survives the same queue and epoch. */
    g_assert_true(radios_audio_engine_runner_pause(engine, NULL));
    windows[0] = queue[0].copyright;
    g_assert_true(radios_audio_engine_runner_copyright(engine, -1, windows, 2, NULL));
    g_usleep(30000); g_assert_true(radios_audio_engine_runner_resume(engine, NULL)); render(engine, 100, output); near(output[0][0], .125);
    radios_audio_engine_runner_snapshot(engine, &state); g_assert_cmpuint(state.index, ==, 1);
    radios_audio_engine_unref(engine);
}
static void copyright_preferred_deck_recycle(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *engine = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunOptions options; radios_audio_run_options_default(&options);
    RadioSAudioRunItem queue[] = {item(f, 0, 0, 0, 2000), item(f, 1, 1, 0, 5000), item(f, 0, 2, 0, 10000)};
    queue[0].preferred_deck = queue[2].preferred_deck = 0; queue[1].preferred_deck = 1;
    queue[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; queue[0].overlap_frames = 500;
    queue[1].copyright = (RadioSAudioCopyrightWindow){TRUE, FALSE, g_get_real_time() - G_USEC_PER_SEC, g_get_real_time() + 10 * G_USEC_PER_SEC};
    start(engine, queue, 3, &options); ready(engine, 0, f->media[0], 2000); ready(engine, 1, f->media[1], 5000);
    RadioSAudioSnapshot before; radios_audio_engine_snapshot(engine, &before);
    RadioSAudioCopyrightWindow windows[] = {queue[0].copyright, queue[1].copyright, queue[2].copyright};
    windows[1].ends_at_utc_us = g_get_real_time() - 1;
    g_assert_true(radios_audio_engine_runner_copyright(engine, -1, windows, 3, NULL));
    gfloat output[3][8192]; render(engine, 2001, output); near(output[0][3998], .25);
    ready_after(engine, 0, f->media[0], 10000, before.decks[0].generation);
    render(engine, 100, output); near(output[0][0], .25);
    RadioSAudioRunSnapshot state; radios_audio_engine_runner_snapshot(engine, &state);
    g_assert_cmpuint(state.index, ==, 2); g_assert_cmpint(state.state, !=, RADIOS_AUDIO_RUN_ERROR);
    radios_audio_engine_unref(engine);
}
static void loudness_origin_phase_pair_pcm(Fixture *f,gconstpointer unused)
{
    (void)unused;RadioSAudioEngine *engine=radios_audio_engine_new(FALSE,NULL);RadioSAudioPreferences prefs;radios_audio_preferences_default(&prefs);
    prefs.loudness_policy.players=3;prefs.loudness_policy.contexts=RADIOS_AUDIO_LOUDNESS_AUTODJ;
    prefs.loudness_policy.phase=RADIOS_AUDIO_LOUDNESS_PHASE_TRANSITION;
    prefs.loudness_policy.automation_mode=RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_SELECTED;prefs.loudness_policy.automation_count=1;
    g_strlcpy(prefs.loudness_policy.automation_ids[0],"11111111-1111-4111-8111-111111111111",37);
    prefs.loudness_policy.pairs_mode=RADIOS_AUDIO_LOUDNESS_PAIRS_SELECTED;prefs.loudness_policy.pairs[0]=2;
    g_assert_true(radios_audio_engine_apply_preferences(engine,&prefs,NULL));
    RadioSAudioRunOptions options;radios_audio_run_options_default(&options);options.loudness_context=RADIOS_AUDIO_LOUDNESS_AUTODJ;
    g_strlcpy(options.loudness_automation_id,prefs.loudness_policy.automation_ids[0],37);options.loudness_assigned=TRUE;
    RadioSAudioRunItem q[]={item(f,0,0,0,3000),item(f,1,1,0,6000)};
    for(guint i=0;i<2;++i)q[i].loudness=(RadioSLoudness){.integrated_lufs=-8,.peak=.25,.measured=TRUE};
    q[0].mode=RADIOS_AUDIO_RUN_CROSSFADE;q[0].overlap_frames=q[1].fade_in_frames=1000;
    start(engine,q,2,&options);ready(engine,0,f->media[0],3000);ready(engine,1,f->media[1],6000);
    RadioSAudioSnapshot before,after;radios_audio_engine_snapshot(engine,&before);gfloat output[3][8192];render(engine,1800,output);near(output[0][3598],.25);
    render(engine,600,output);gdouble a,b;RadioSAudioTransition fade={1000,1000,1000,0};radios_audio_transition_gains(&fade,399,&a,&b);
    gdouble factor=pow(10,-6.0/20);near(output[0][1198],factor*(.25*a+.125*b));
    radios_audio_engine_snapshot(engine,&after);near(after.decks[0].loudness_gain_db,-6);near(after.decks[1].loudness_gain_db,-6);
    g_assert_cmpuint(after.decks[0].generation,==,before.decks[0].generation);g_assert_cmpuint(after.decks[1].generation,==,before.decks[1].generation);
    g_assert_true(radios_audio_engine_runner_pause(engine,NULL));render(engine,500,output);near(output[0][998],0);
    prefs.loudness_policy.pairs[0]=0;prefs.loudness_policy.pairs[1]=1;g_assert_true(radios_audio_engine_apply_preferences(engine,&prefs,NULL));
    g_assert_true(radios_audio_engine_runner_resume(engine,NULL));render(engine,300,output);radios_audio_transition_gains(&fade,699,&a,&b);near(output[0][598],.25*a+.125*b);
    radios_audio_engine_snapshot(engine,&after);near(after.decks[0].loudness_gain_db,0);near(after.decks[1].loudness_gain_db,0);
    prefs.loudness_policy.phase=RADIOS_AUDIO_LOUDNESS_PHASE_STEADY;prefs.loudness_policy.pairs_mode=RADIOS_AUDIO_LOUDNESS_PAIRS_ALL;
    g_assert_true(radios_audio_engine_apply_preferences(engine,&prefs,NULL));render(engine,301,output);render(engine,400,output);near(output[0][798],.125*factor);
    RadioSAudioRunSnapshot run;radios_audio_engine_runner_snapshot(engine,&run);g_assert_cmpuint(run.index,==,1);
    /* Manual takeover must not retain the accepted automation document/context. */
    g_assert_true(radios_audio_engine_stop(engine,1,NULL));ready(engine,1,f->media[1],2000);g_assert_true(radios_audio_engine_play(engine,1,NULL));render(engine,512,output);near(output[0][1022],.125);
    radios_audio_engine_snapshot(engine,&after);near(after.decks[1].loudness_gain_db,0);near(after.decks[1].loudness_lufs,-8);near(after.decks[1].gain_db,0);
    radios_audio_engine_unref(engine);
}
static void loudness_assigned_origin_pcm(Fixture *f,gconstpointer unused)
{
    (void)unused;RadioSAudioEngine *engine=radios_audio_engine_new(FALSE,NULL);RadioSAudioPreferences prefs;radios_audio_preferences_default(&prefs);
    prefs.loudness_policy.contexts=RADIOS_AUDIO_LOUDNESS_AUTOMATION;prefs.loudness_policy.automation_mode=RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_ASSIGNED;
    g_assert_true(radios_audio_engine_apply_preferences(engine,&prefs,NULL));
    RadioSAudioRunOptions options;radios_audio_run_options_default(&options);options.loudness_context=RADIOS_AUDIO_LOUDNESS_AUTOMATION;
    g_strlcpy(options.loudness_automation_id,"11111111-1111-4111-8111-111111111111",37);
    RadioSAudioRunItem q[]={item(f,0,0,0,10000)};q[0].loudness=(RadioSLoudness){.integrated_lufs=-8,.peak=.25,.measured=TRUE};
    /* 4800 = the runner's start threshold (engine.c runner_main_ready). */
    start(engine,q,1,&options);ready(engine,0,f->media[0],4800);gfloat output[3][8192];render(engine,512,output);near(output[0][1022],.25);
    g_assert_true(radios_audio_engine_runner_stop(engine,NULL));options.loudness_assigned=TRUE;start(engine,q,1,&options);ready(engine,0,f->media[0],4800);
    render(engine,512,output);near(output[0][1022],.25*pow(10,-6.0/20));radios_audio_engine_unref(engine);
}
static void loudness_steady_selected_pair_pcm(Fixture *f,gconstpointer unused)
{
    (void)unused;RadioSAudioEngine *engine=radios_audio_engine_new(FALSE,NULL);RadioSAudioPreferences prefs;radios_audio_preferences_default(&prefs);
    prefs.loudness_policy.players=3;prefs.loudness_policy.contexts=RADIOS_AUDIO_LOUDNESS_AUTODJ;
    prefs.loudness_policy.phase=RADIOS_AUDIO_LOUDNESS_PHASE_STEADY;
    prefs.loudness_policy.pairs_mode=RADIOS_AUDIO_LOUDNESS_PAIRS_SELECTED;prefs.loudness_policy.pairs[0]=2;
    g_assert_true(radios_audio_engine_apply_preferences(engine,&prefs,NULL));RadioSAudioRunOptions options;radios_audio_run_options_default(&options);
    options.loudness_context=RADIOS_AUDIO_LOUDNESS_AUTODJ;RadioSAudioRunItem q[]={item(f,0,0,0,3000),item(f,1,1,0,7000)};
    for(guint i=0;i<2;++i)q[i].loudness=(RadioSLoudness){.integrated_lufs=-8,.peak=.25,.measured=TRUE};
    q[0].mode=RADIOS_AUDIO_RUN_CROSSFADE;q[0].overlap_frames=q[1].fade_in_frames=1000;
    start(engine,q,2,&options);ready(engine,0,f->media[0],3000);ready(engine,1,f->media[1],7000);
    gfloat output[3][8192];render(engine,1800,output);gdouble factor=pow(10,-6.0/20);near(output[0][3598],.25*factor);
    render(engine,600,output);gdouble a,b;RadioSAudioTransition fade={1000,1000,1000,0};radios_audio_transition_gains(&fade,399,&a,&b);
    near(output[0][1198],.25*a+.125*b);RadioSAudioSnapshot audio;radios_audio_engine_snapshot(engine,&audio);
    near(audio.decks[0].loudness_gain_db,0);near(audio.decks[1].loudness_gain_db,0);
    render(engine,1001,output);near(output[0][2000],.125*factor);RadioSAudioRunSnapshot state;radios_audio_engine_runner_snapshot(engine,&state);
    g_assert_cmpuint(state.index,==,1);g_assert_cmpint(state.deck_next,==,-1);radios_audio_engine_snapshot(engine,&audio);near(audio.decks[1].loudness_gain_db,-6);
    /* Final incoming tail retains the actual successful direction, until a
     * manual takeover clears that association and the accepted origin. */
    g_assert_true(radios_audio_engine_stop(engine,1,NULL));ready(engine,1,f->media[1],2000);g_assert_true(radios_audio_engine_play(engine,1,NULL));
    render(engine,512,output);near(output[0][1022],.125);radios_audio_engine_snapshot(engine,&audio);near(audio.decks[1].loudness_gain_db,0);
    radios_audio_engine_unref(engine);
}
static RadioSAudioCurve shape_curve(guint count, const gdouble *points)
{
    RadioSAudioCurve curve = {count, {0}, {0}};
    for (guint i = 0; i < count; ++i) { curve.t[i] = points[i * 2]; curve.v[i] = points[i * 2 + 1]; }
    return curve;
}
/* A designed shape on the outgoing item replaces the equal-power and duck
 * envelope with its own volume curves over the real overlap; it is refused
 * on a boundary without a two-source overlap. */
static void shaped_overlap(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL); RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 2000), item(f, 1, 1, 0, 3000)};
    q[0].mode = RADIOS_AUDIO_RUN_EOF; q[0].overlap_frames = q[1].fade_in_frames = 1000; q[0].duck_db = -6.020599913279624;
    const gdouble flat_one[] = {0, 1, 1, 1}, flat_zero[] = {0, 0, 1, 0}, out_step[] = {0, 1, .5, 1, .5, 0, 1, 0}, in_step[] = {0, 0, .5, 0, .5, 1, 1, 1};
    RadioSAudioTransitionShape *shape = &q[0].shape; shape->enabled = TRUE; shape->effect = -1; shape->effect_bpm = 120; shape->effect_mix = shape_curve(2, flat_one);
    for (guint side = 0; side < 2; ++side) { shape->bass[side] = shape_curve(2, flat_one); shape->tone[side] = shape_curve(2, flat_zero); }
    shape->volume[0] = shape_curve(4, out_step); shape->volume[1] = shape_curve(4, in_step);
    GError *error = NULL;
    g_assert_false(radios_audio_engine_runner_configure(e, q, 2, &o, &error)); g_assert_error(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_INVALID); g_clear_error(&error);
    q[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; start(e, q, 2, &o); ready(e, 0, f->media[0], 2000); ready(e, 1, f->media[1], 3000);
    gfloat output[3][8192]; render(e, 1000, output); render(e, 1000, output);
    for (guint i = 0; i < 1000; ++i) near(output[0][i * 2], i <= 500 ? .25 : .125);
    render(e, 1, output); near(output[0][0], .125); RadioSAudioRunSnapshot r; radios_audio_engine_runner_snapshot(e, &r);
    g_assert_cmpuint(r.index, ==, 1); g_assert_cmpuint(r.actual_current_frame, ==, 1001); radios_audio_engine_unref(e);
}

/* Kullanıcı isteği U7 (× in the running order): remove_future replaces only
 * unheard entries and the sounding item keeps its own exit policy — a MANUAL
 * wait before a live step, an accepted crossfade length — while
 * replace_future applies the new first entry's policy. */
static void remove_future_keeps_sounding_policy(Fixture *f, gconstpointer unused)
{
    (void)unused; gchar *extra = g_uuid_string_random(); gfloat output[3][8192]; GError *error = NULL;
    RadioSAudioRunOptions o; radios_audio_run_options_default(&o);
    RadioSAudioRunSnapshot r; RadioSAudioSnapshot s;
    /* MANUAL survives removing its successor; replace_future would not keep it. */
    for (guint api = 0; api < 2; ++api) {
        RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL);
        RadioSAudioRunItem q[] = {item(f, 0, 0, 0, 2000), item(f, 1, 1, 0, 2000), item(f, 0, 2, 0, 2000)};
        q[0].mode = RADIOS_AUDIO_RUN_MANUAL; q[0].preferred_deck = q[2].preferred_deck = 0; q[1].preferred_deck = 1;
        start(e, q, 3, &o); ready(e, 0, f->media[0], 2000); ready(e, 1, f->media[1], 2000); render(e, 100, output);
        radios_audio_engine_runner_snapshot(e, &r); radios_audio_engine_snapshot(e, &s);
        gboolean accepted = api ? radios_audio_engine_runner_replace_future(e, -1, r.epoch, s.decks[0].generation, f->entry[0], q + 2, 1, &error) :
            radios_audio_engine_runner_remove_future(e, -1, r.epoch, s.decks[0].generation, f->entry[0], q + 2, 1, &error);
        g_assert_no_error(error); g_assert_true(accepted);
        render(e, 2000, output); radios_audio_engine_runner_snapshot(e, &r); g_assert_cmpuint(r.count, ==, 2);
        if (api) { g_assert_cmpint(r.state, !=, RADIOS_AUDIO_RUN_MANUAL_WAIT); radios_audio_engine_unref(e); continue; }
        g_assert_cmpint(r.state, ==, RADIOS_AUDIO_RUN_MANUAL_WAIT); g_assert_cmpuint(r.index, ==, 0);
        radios_audio_engine_snapshot(e, &s); g_assert_cmpint(s.decks[1].state, ==, RADIOS_AUDIO_EMPTY);
        g_assert_true(radios_audio_engine_runner_next(e, NULL)); render(e, 241, output);
        ready_after(e, 0, f->media[0], 2000, s.decks[0].generation); render(e, 1, output); near(output[0][0], .25);
        radios_audio_engine_runner_snapshot(e, &r); g_assert_cmpuint(r.index, ==, 1); radios_audio_engine_unref(e);
    }
    /* The accepted crossfade length carries into the new successor. */
    RadioSAudioEngine *e = radios_audio_engine_new(FALSE, NULL); o.pool_count = 3; o.deck_pool[2] = 2;
    RadioSAudioRunItem x[] = {item(f, 0, 0, 0, 3000), item(f, 1, 1, 0, 3000), item(f, 1, 2, 0, 3000)};
    x[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; x[0].overlap_frames = 500; x[1].fade_in_frames = x[2].fade_in_frames = 500;
    for (guint i = 0; i < 3; ++i) x[i].preferred_deck = (gint)i;
    start(e, x, 3, &o); ready(e, 0, f->media[0], 3000); ready(e, 1, f->media[1], 3000); ready(e, 2, f->media[1], 3000);
    render(e, 100, output); radios_audio_engine_runner_snapshot(e, &r); radios_audio_engine_snapshot(e, &s);
    g_assert_true(radios_audio_engine_runner_remove_future(e, -1, r.epoch, s.decks[0].generation, f->entry[0], x + 2, 1, &error));
    g_assert_no_error(error); ready_after(e, 2, f->media[1], 3000, s.decks[2].generation);
    render(e, 2400, output); for (guint i = 0; i < 2400; ++i) near(output[0][i * 2], .25);
    render(e, 500, output); RadioSAudioTransition t = {500, 500, 500, 0};
    for (guint i = 0; i < 500; ++i) { gdouble a, b; radios_audio_transition_gains(&t, i, &a, &b); near(output[0][i * 2], .25 * a + .125 * b); }
    radios_audio_engine_snapshot(e, &s); g_assert_cmpint(s.decks[1].state, ==, RADIOS_AUDIO_EMPTY); radios_audio_engine_unref(e);
    /* An overlap onto the same planned player becomes a plain cut, not an error. */
    e = radios_audio_engine_new(FALSE, NULL); radios_audio_run_options_default(&o);
    RadioSAudioRunItem y[] = {item(f, 0, 0, 0, 2000), item(f, 1, 1, 0, 2000), item(f, 1, 2, 0, 2000)};
    y[0].mode = RADIOS_AUDIO_RUN_CROSSFADE; y[0].overlap_frames = y[1].fade_in_frames = 500;
    y[0].preferred_deck = y[2].preferred_deck = 0; y[1].preferred_deck = 1;
    start(e, y, 3, &o); ready(e, 0, f->media[0], 2000); ready(e, 1, f->media[1], 2000); render(e, 100, output);
    radios_audio_engine_runner_snapshot(e, &r); radios_audio_engine_snapshot(e, &s);
    g_assert_true(radios_audio_engine_runner_remove_future(e, -1, r.epoch, s.decks[0].generation, f->entry[0], y + 2, 1, &error));
    g_assert_no_error(error); render(e, 1900, output); near(output[0][1898], .25); render(e, 1, output);
    ready_after(e, 0, f->media[1], 2000, s.decks[0].generation); render(e, 2, output); near(output[0][0], .125);
    radios_audio_engine_runner_snapshot(e, &r); g_assert_cmpuint(r.index, ==, 1); radios_audio_engine_unref(e);
    /* count 0 ends the queue after the sounding item. */
    e = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunItem z[] = {item(f, 0, 0, 0, 1000), item(f, 1, 1, 0, 1000)};
    start(e, z, 2, &o); ready(e, 0, f->media[0], 1000); ready(e, 1, f->media[1], 1000); render(e, 10, output);
    radios_audio_engine_runner_snapshot(e, &r); radios_audio_engine_snapshot(e, &s);
    g_assert_true(radios_audio_engine_runner_remove_future(e, -1, r.epoch, s.decks[0].generation, f->entry[0], NULL, 0, &error));
    g_assert_no_error(error); render(e, 1000, output); radios_audio_engine_runner_snapshot(e, &r);
    g_assert_cmpint(r.state, ==, RADIOS_AUDIO_RUN_FINISHED); g_assert_cmpuint(r.count, ==, 1);
    RadioSAudioRunReceipt receipts[2]; g_assert_cmpuint(radios_audio_engine_runner_receipts(e, receipts, 2), ==, 1);
    radios_audio_engine_unref(e);
    /* Removing a later entry keeps the loaded successor: same generation, no reload. */
    e = radios_audio_engine_new(FALSE, NULL);
    RadioSAudioRunItem k[] = {item(f, 0, 0, 0, 2000), item(f, 1, 1, 0, 2000), item(f, 0, 2, 0, 2000), item(f, 1, 0, 0, 2000)};
    for (guint i = 0; i < 4; ++i) k[i].preferred_deck = (gint)(i % 2);
    k[3].entry_id = extra; k[3].preferred_deck = 0;
    start(e, k, 4, &o); ready(e, 0, f->media[0], 2000); ready(e, 1, f->media[1], 2000); render(e, 10, output);
    radios_audio_engine_runner_snapshot(e, &r); radios_audio_engine_snapshot(e, &s);
    RadioSAudioRunItem kept[] = {k[1], k[3]}; guint64 successor = s.decks[1].generation;
    g_assert_true(radios_audio_engine_runner_remove_future(e, -1, r.epoch, s.decks[0].generation, f->entry[0], kept, 2, &error));
    g_assert_no_error(error); radios_audio_engine_snapshot(e, &s); g_assert_cmpuint(s.decks[1].generation, ==, successor);
    render(e, 1991, output); near(output[0][1990 * 2], .125); radios_audio_engine_runner_snapshot(e, &r);
    g_assert_cmpuint(r.index, ==, 1); g_assert_cmpint(r.deck_current, ==, 1);
    ready_after(e, 0, f->media[1], 2000, s.decks[0].generation); render(e, 2001, output); near(output[0][2000 * 2], .125);
    radios_audio_engine_runner_snapshot(e, &r); g_assert_cmpuint(r.index, ==, 2); g_assert_cmpstr(r.entry_id, ==, extra);
    radios_audio_engine_unref(e); g_free(extra);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/runner/designed-shape-overlap-envelope", Fixture, NULL, setup, shaped_overlap, teardown);
    g_test_add("/runner/loudness-steady-selected-pair-before-fade-final-tail-PCM", Fixture, NULL, setup, loudness_steady_selected_pair_pcm, teardown);
    g_test_add("/runner/loudness-origin-phase-directed-pair-pause-manual-PCM", Fixture, NULL, setup, loudness_origin_phase_pair_pcm, teardown);
    g_test_add("/runner/loudness-assigned-origin-context-PCM", Fixture, NULL, setup, loudness_assigned_origin_pcm, teardown);
    g_test_add("/runner/copyright-delayed-start-skips-item", Fixture, NULL, setup, copyright_delayed_start_skips_item, teardown);
    g_test_add("/runner/copyright-preload-expiry-skips-middle", Fixture, NULL, setup, copyright_preload_expiry_skips_middle, teardown);
    g_test_add("/runner/copyright-overlap-expiry-preserves-other-PCM", Fixture, NULL, setup, copyright_overlap_expiry_preserves_other_pcm, teardown);
    g_test_add("/runner/copyright-toggle-off-pause-resume", Fixture, NULL, setup, copyright_toggle_off_pause_resume, teardown);
    g_test_add("/runner/copyright-preferred-deck-recycle", Fixture, NULL, setup, copyright_preferred_deck_recycle, teardown);
    g_test_add("/runner/seek-preserves-queue-preload-paused-overlap", Fixture, NULL, setup, seek_keeps_autodj, teardown);
    g_test_add("/runner/adoption-span-and-atomic-target", Fixture, NULL, setup, adoption_is_atomic, teardown);
    g_test_add("/runner/estimated-eof-rounding", Fixture, NULL, setup, estimated_eof_rounding, teardown);
    g_test_add("/runner/AutoDJ-exit-trigger-preserves-physical-tail", Fixture, NULL, setup, autodj_exit_preserves_physical_tail, teardown);
    g_test_add("/runner/AutoDJ-short-IN-OUT-caps-incoming-fade-PCM", Fixture, GUINT_TO_POINTER(1), setup, autodj_short_incoming_span, teardown);
    g_test_add("/runner/AutoDJ-short-IN-OUT-caps-default-outgoing-timing-PCM", Fixture, NULL, setup, autodj_short_incoming_span, teardown);
    g_test_add("/runner/AutoDJ-beatmatch-short-IN-OUT-keeps-positive-span-PCM", Fixture, NULL, setup, autodj_beat_short_span, teardown);
    g_test_add("/runner/AutoDJ-tempo-short-IN-OUT-fade-handover-continuity-PCM", Fixture, NULL, setup, autodj_tempo_incoming_fade, teardown);
    g_test_add("/runner/disjoint-scopes-crossfade-pause-conflict-PCM", Fixture, NULL, setup, disjoint_scopes_crossfade_and_pause, teardown);
    g_test_add("/runner/scope-adoption-generation-unrelated-PCM", Fixture, NULL, setup, scope_adoption_generation_preserves_unrelated_pcm, teardown);
    g_test_add("/runner/validation-manual-ownership", Fixture, NULL, setup, validation, teardown);
    g_test_add("/runner/eof-midblock-cue-position", Fixture, NULL, setup, eof_midblock, teardown);
    g_test_add("/runner/sample-crossfade-duck", Fixture, NULL, setup, overlap, teardown);
    g_test_add("/runner/pause-overlap", Fixture, NULL, setup, pause_overlap, teardown);
    g_test_add("/runner/single-deck-drain", Fixture, NULL, setup, single_deck, teardown);
    g_test_add("/runner/manual-takeover", Fixture, NULL, setup, takeover, teardown);
    g_test_add("/runner/cue-navigation", Fixture, NULL, setup, cue_navigation, teardown);
    g_test_add("/runner/manual-wait", Fixture, NULL, setup, manual_wait, teardown);
    g_test_add("/runner/preferred-deck-recycle", Fixture, NULL, setup, preferred_recycle, teardown);
    g_test_add("/runner/windows-deadlines", Fixture, NULL, setup, windows_and_deadlines, teardown);
    g_test_add("/runner/programme-anchor-cut", Fixture, NULL, setup, anchors, teardown);
    g_test_add("/runner/verified-hash-error", Fixture, NULL, setup, hash_error, teardown);
    g_test_add("/runner/preroll-cancel-lifecycle", Fixture, NULL, setup, preroll_cancel, teardown);
    g_test_add("/runner/append-short-consumption-receipts", Fixture, NULL, setup, append_receipts, teardown);
    g_test_add("/runner/previous-distinct-consumption-receipts", Fixture, NULL, setup, previous_receipts, teardown);
    g_test_add("/runner/terminal-actual-consumption", Fixture, NULL, setup, terminals, teardown);
    g_test_add("/runner/failed-terminal", Fixture, NULL, setup, failed_terminal, teardown);
    g_test_add("/runner/handover-finish-successor-suppression", Fixture, NULL, setup, handover_finish, teardown);
    g_test_add("/runner/handover-UTC-cut-bounded-fade", Fixture, NULL, setup, handover_cut, teardown);
    g_test_add("/runner/handover-cancel-epoch-manual-paused", Fixture, NULL, setup, handover_cancel, teardown);
    g_test_add("/runner/handover-old-programme-deadline", Fixture, NULL, setup, handover_old_deadline, teardown);
    g_test_add("/runner/pulse", Fixture, NULL, setup, pulse, teardown);
    g_test_add("/runner/independent-player-PCM-pause-next-stop-ownership-receipts", Fixture, NULL, setup, independent_players, teardown);
    g_test_add("/runner/independent-player-EOF-next-finish-other-keeps-playing", Fixture, NULL, setup, independent_eof, teardown);
    g_test_add("/runner/remove-future-keeps-sounding-exit-policy-PCM", Fixture, NULL, setup, remove_future_keeps_sounding_policy, teardown);
    return g_test_run();
}
