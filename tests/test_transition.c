#include "radios/audio_transition.h"
#include "radios/transition_preset.h"
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>

#ifndef RADIOS_TEST_PROJECT_ROOT
#define RADIOS_TEST_PROJECT_ROOT "."
#endif
typedef struct { gchar *directory, *a, *b, *sha_a, *sha_b; } Fixture;
static void append16(GByteArray *bytes, guint16 n)
{ guint8 p[] = {(guint8)n, (guint8)(n >> 8)}; g_byte_array_append(bytes, p, 2); }
static void append32(GByteArray *bytes, guint32 n)
{ guint8 p[] = {(guint8)n, (guint8)(n >> 8), (guint8)(n >> 16), (guint8)(n >> 24)}; g_byte_array_append(bytes, p, 4); }
static gchar *wave(const gchar *path, gboolean source_a)
{
    guint frames = 4 * 48000; GByteArray *bytes = g_byte_array_new();
    g_byte_array_append(bytes, (const guint8 *)"RIFF", 4); append32(bytes, 36 + frames * 4);
    g_byte_array_append(bytes, (const guint8 *)"WAVEfmt ", 8); append32(bytes, 16); append16(bytes, 1); append16(bytes, 2);
    append32(bytes, 48000); append32(bytes, 192000); append16(bytes, 4); append16(bytes, 16);
    g_byte_array_append(bytes, (const guint8 *)"data", 4); append32(bytes, frames * 4);
    for (guint i = 0; i < frames; ++i) { append16(bytes, source_a ? 8192 : 0); append16(bytes, source_a ? 0 : 4096); }
    g_assert_true(g_file_set_contents(path, (const gchar *)bytes->data, bytes->len, NULL)); g_assert_cmpint(g_chmod(path, 0600), ==, 0);
    gchar *sha = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes->data, bytes->len); g_byte_array_unref(bytes); return sha;
}
static void setup(Fixture *f, gconstpointer unused)
{
    (void)unused; f->directory = g_build_filename(RADIOS_TEST_PROJECT_ROOT, "build", "transition-PCM-XXXXXX", NULL);
    g_assert_nonnull(g_mkdtemp(f->directory)); f->a = g_build_filename(f->directory, "Çıkış A.wav", NULL); f->b = g_build_filename(f->directory, "Giriş B.wav", NULL);
    f->sha_a = wave(f->a, TRUE); f->sha_b = wave(f->b, FALSE);
}
static void teardown(Fixture *f, gconstpointer unused)
{
    (void)unused; g_assert_cmpint(g_unlink(f->a), ==, 0); g_assert_cmpint(g_unlink(f->b), ==, 0); g_assert_cmpint(g_rmdir(f->directory), ==, 0);
    g_free(f->directory); g_free(f->a); g_free(f->b); g_free(f->sha_a); g_free(f->sha_b);
}
static RadioSAudioTransitionMedia settings(Fixture *f)
{
    return (RadioSAudioTransitionMedia){ .path_a = f->a, .path_b = f->b, .sha_a = f->sha_a, .sha_b = f->sha_b,
        .duration_a = 192000, .duration_b = 192000, .cue_a = 48000, .out_a = 144000, .cue_b = 24000,
        .overlap = 48000, .fade_in = 48000, .duck_db = -6.020599913279624, .level_db = 0 };
}
static void close_to(gdouble actual, gdouble expected)
{ g_assert_cmpfloat(fabs(actual - expected), <, .00001); }
static void pcm(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioTransitionMedia s = settings(f); RadioSAudioTransitionPlan plan;
    g_assert_true(radios_audio_transition_plan(&s, &plan, NULL)); g_assert_cmpuint(plan.frames, ==, 216000);
    gchar *path = NULL, *sha = NULL; guint64 frames = 0; GError *error = NULL;
    g_assert_true(radios_audio_transition_prepare(&s, f->directory, NULL, &path, &sha, &frames, &error)); g_assert_no_error(error);
    struct stat st; g_assert_cmpint(g_stat(path, &st), ==, 0); g_assert_cmpint(st.st_mode & 0777, ==, 0600); g_assert_cmpuint(st.st_size, ==, frames * 8 + 44);
    RadioSAudioDecoder *d = radios_audio_decoder_open(path, sha, NULL, &error); g_assert_no_error(error); g_assert_nonnull(d); gfloat sample[2];
    const guint64 positions[] = {24000, 72000, 120000};
    for (guint i = 0; i < G_N_ELEMENTS(positions); ++i) {
        g_assert_true(radios_audio_decoder_seek(d, positions[i], NULL, &error)); g_assert_no_error(error);
        g_assert_cmpint(radios_audio_decoder_read(d, sample, 1, NULL, &error), ==, 1); g_assert_no_error(error);
        gdouble a, b; radios_audio_transition_envelope(&plan, positions[i], &a, &b); close_to(sample[0], .25 * a); close_to(sample[1], .125 * b);
    }
    /* Independent expected values establish the physical duck/fade behavior. */
    g_assert_true(radios_audio_decoder_seek(d, 72000, NULL, &error)); g_assert_cmpint(radios_audio_decoder_read(d, sample, 1, NULL, &error), ==, 1);
    close_to(sample[0], .25 * sqrt(.5) * .5); close_to(sample[1], .125 * sqrt(.5));
    radios_audio_decoder_close(d); g_assert_cmpint(g_unlink(path), ==, 0); g_free(path); g_free(sha);
}
static guint files(Fixture *f)
{ GDir *dir = g_dir_open(f->directory, 0, NULL); guint count = 0; while (g_dir_read_name(dir)) ++count; g_dir_close(dir); return count; }
static void rejection(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioTransitionMedia s = settings(f); gchar *path = NULL, *sha = NULL; guint64 frames = 0; GError *error = NULL;
    GCancellable *cancel = g_cancellable_new(); g_cancellable_cancel(cancel);
    g_assert_false(radios_audio_transition_prepare(&s, f->directory, cancel, &path, &sha, &frames, &error)); g_assert_nonnull(error); g_clear_error(&error);
    g_assert_null(path); g_assert_null(sha); g_assert_cmpuint(files(f), ==, 2); g_object_unref(cancel);
    s.sha_b = "0000000000000000000000000000000000000000000000000000000000000000";
    g_assert_false(radios_audio_transition_prepare(&s, f->directory, NULL, &path, &sha, &frames, &error)); g_assert_nonnull(error); g_clear_error(&error); g_assert_cmpuint(files(f), ==, 2);
    s = settings(f); s.duration_b = 2 * 48000; s.cue_b = 48000; s.overlap = 48001;
    RadioSAudioTransitionPlan plan; g_assert_false(radios_audio_transition_plan(&s, &plan, &error)); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
    s = settings(f); s.level_db = NAN; g_assert_false(radios_audio_transition_plan(&s, &plan, &error)); g_clear_error(&error);
}
static void level_and_end(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioTransitionMedia s = settings(f); s.level_db = -6.020599913279624; s.overlap = 0; s.fade_in = 0;
    RadioSAudioTransitionPlan plan; g_assert_true(radios_audio_transition_plan(&s, &plan, NULL)); gdouble a, b;
    radios_audio_transition_envelope(&plan, 24000, &a, &b); close_to(a, .5); close_to(b, 0);
    radios_audio_transition_envelope(&plan, plan.end_a, &a, &b); close_to(a, 0); close_to(b, .5);
    radios_audio_transition_envelope(&plan, plan.frames, &a, &b); close_to(a, 0); close_to(b, 0);
    s.duration_a = 86400ULL * 48000; s.out_a = s.duration_a; s.cue_a = 0; s.overlap = 48000;
    g_assert_true(radios_audio_transition_plan(&s, &plan, NULL)); g_assert_cmpuint(plan.frames, <=, 90u * 48000);
}
static void lowpass_isolated_response(void)
{
    RadioSAudioTransitionFilter state = {0}, separate = {0}; gdouble high = 0, low = 0, dry_high = 0, dry_low = 0;
    const guint frames = 48000;
    for (guint i = 0; i < frames; ++i) {
        gfloat sample[2] = {(gfloat)sin(2 * G_PI * 10000 * i / 48000), (gfloat)sin(2 * G_PI * 100 * i / 48000)};
        gfloat original[2] = {sample[0], sample[1]}; radios_audio_transition_lowpass(&state, i, frames, sample);
        if (!i) { close_to(sample[0], original[0]); close_to(sample[1], original[1]); }
        if (i > frames * .95) { high += sample[0] * sample[0]; low += sample[1] * sample[1]; dry_high += original[0] * original[0]; dry_low += original[1] * original[1]; }
    }
    g_assert_cmpfloat(high / dry_high, <, .01); g_assert_cmpfloat(low / dry_low, >, .85);
    gfloat dry[2] = {.3f, -.2f}; radios_audio_transition_lowpass(&separate, frames, 0, dry);
    g_assert_cmpfloat(dry[0], ==, .3f); g_assert_cmpfloat(dry[1], ==, -.2f); g_assert_cmpfloat(separate.stereo[0], ==, 0); g_assert_cmpfloat(separate.stereo[1], ==, 0);
    for (guint i = 0; i < 1000; ++i) { gfloat mono[2] = {.2f, 0}; radios_audio_transition_lowpass(&separate, 1000, 1000, mono); g_assert_cmpfloat(mono[1], ==, 0); }
    g_assert_cmpfloat(separate.stereo[0], >, .19); g_assert_cmpfloat(separate.stereo[1], ==, 0);
}

static RadioSAudioCurve curve_of(guint count, const gdouble *points)
{
    RadioSAudioCurve curve = {count, {0}, {0}};
    for (guint i = 0; i < count; ++i) { curve.t[i] = points[i * 2]; curve.v[i] = points[i * 2 + 1]; }
    return curve;
}
/* Designed curves: linear between points, a vertical step at equal t, 1 for
 * an empty curve; validation needs 2..8 ordered points from t=0 to t=1. Every
 * built-in template parses into a valid shape; a bad effect or length does not. */
static void shape_curves(void)
{
    const gdouble linear[] = {0, 1, 1, 0}, rising[] = {0, 0, 1, 1}, step[] = {0, 1, .5, 1, .5, 0, 1, 0}, back[] = {0, 1, .6, 1, .4, 0, 1, 0};
    RadioSAudioCurve curve = curve_of(2, linear);
    close_to(radios_audio_curve_value(&curve, 0), 1); close_to(radios_audio_curve_value(&curve, .25), .75); close_to(radios_audio_curve_value(&curve, 1), 0);
    close_to(radios_audio_curve_value(&curve, -1), 1); close_to(radios_audio_curve_value(&curve, 2), 0);
    curve = curve_of(4, step); close_to(radios_audio_curve_value(&curve, .49), 1); close_to(radios_audio_curve_value(&curve, .51), 0);
    g_assert_true(radios_audio_curve_valid(&curve));
    curve = curve_of(4, back); g_assert_false(radios_audio_curve_valid(&curve));
    RadioSAudioCurve empty = {0, {0}, {0}}; close_to(radios_audio_curve_value(&empty, .3), 1); g_assert_false(radios_audio_curve_valid(&empty));
    curve = curve_of(2, linear); curve.t[1] = .9; g_assert_false(radios_audio_curve_valid(&curve));
    JsonArray *templates = radios_transition_preset_templates(); g_assert_cmpuint(json_array_get_length(templates), ==, 8);
    for (guint i = 0; i < json_array_get_length(templates); ++i) {
        JsonObject *preset = json_array_get_object_element(templates, i); RadioSAudioTransitionShape shape;
        g_assert_true(radios_transition_preset_valid(preset, TRUE)); g_assert_true(radios_transition_preset_to_shape(preset, &shape));
        g_assert_true(shape.enabled); g_assert_true(radios_audio_transition_shape_validate(&shape, NULL));
    }
    JsonObject *echo = radios_transition_preset_template("Yankı ile çık"); RadioSAudioTransitionShape shape;
    g_assert_nonnull(echo); g_assert_true(radios_transition_preset_to_shape(echo, &shape));
    g_assert_cmpint(shape.effect, ==, RADIOS_FX_ECHO); close_to(shape.effect_depth, .7); g_assert_cmpuint(shape.effect_side, ==, 0);
    g_assert_cmpint(radios_transition_effect_type("slip_roll"), ==, RADIOS_FX_SLIP_ROLL); g_assert_cmpstr(radios_transition_effect_id(RADIOS_FX_MIDI_LFO), ==, "midi_lfo");
    json_object_set_string_member(json_object_get_object_member(echo, "effect"), "type", "chorus");
    g_assert_false(radios_transition_preset_valid(echo, TRUE)); g_assert_false(radios_transition_preset_to_shape(echo, &shape)); g_assert_false(shape.enabled);
    json_object_set_string_member(json_object_get_object_member(echo, "effect"), "type", "echo"); json_object_set_int_member(echo, "duration_ms", 100);
    g_assert_false(radios_transition_preset_valid(echo, TRUE));
    json_object_unref(echo); json_array_unref(templates);
    gdouble a, b; memset(&shape, 0, sizeof shape); shape.enabled = TRUE; shape.volume[0] = curve_of(4, step); shape.volume[1] = curve_of(2, rising);
    radios_audio_transition_shape_gains(&shape, .25, &a, &b); close_to(a, 1); close_to(b, .25);
    radios_audio_transition_shape_gains(&shape, .75, &a, &b); close_to(a, 0); close_to(b, .75);
}
static gdouble rms(const gfloat *samples, guint frames)
{ gdouble sum = 0; for (guint i = 0; i < frames; ++i) sum += (gdouble)samples[i] * samples[i]; return sqrt(sum / frames); }
static void tone_through(RadioSAudioShapeVoice *voice, const RadioSAudioTransitionShape *shape, guint side, gdouble frequency, gfloat *out)
{
    radios_audio_shape_voice_reset(voice);
    for (guint i = 0; i < 48000; ++i) {
        gfloat stereo[2] = {(gfloat)sin(2 * G_PI * frequency * i / 48000.0), 0};
        radios_audio_shape_voice_process(voice, shape, side, 1, stereo); out[i] = stereo[0];
    }
}
/* Real frequency response of one shaped side: a bass curve at 0 cuts 60 Hz by
 * about 30 dB and leaves 6 kHz; a closed low-pass keeps 60 Hz and removes
 * 6 kHz, a closed high-pass the opposite; an echo on the outgoing side adds a
 * tail after the input stops while the incoming side stays dry. */
static void shape_voice_response(void)
{
    const gdouble flat_one[] = {0, 1, 1, 1}, flat_zero[] = {0, 0, 1, 0};
    RadioSAudioShapeVoice *voice = radios_audio_shape_voice_new(); RadioSAudioTransitionShape shape; memset(&shape, 0, sizeof shape);
    shape.enabled = TRUE; shape.effect = -1; shape.effect_bpm = 120; shape.effect_mix = curve_of(2, flat_one);
    for (guint side = 0; side < 2; ++side) { shape.volume[side] = curve_of(2, flat_one); shape.bass[side] = curve_of(2, flat_one); shape.tone[side] = curve_of(2, flat_zero); }
    static gfloat low[48000], high[48000];
    shape.bass[0] = curve_of(2, flat_zero);
    tone_through(voice, &shape, 0, 60, low); tone_through(voice, &shape, 0, 6000, high);
    g_assert_cmpfloat(rms(low + 24000, 24000), <, .07); g_assert_cmpfloat(rms(high + 24000, 24000), >, .6);
    shape.bass[0] = curve_of(2, flat_one); shape.tone[0] = curve_of(2, flat_one); shape.tone_kind[0] = RADIOS_AUDIO_TONE_LOWPASS;
    tone_through(voice, &shape, 0, 60, low); tone_through(voice, &shape, 0, 6000, high);
    g_assert_cmpfloat(rms(low + 24000, 24000), >, .6); g_assert_cmpfloat(rms(high + 24000, 24000), <, .1);
    shape.tone_kind[0] = RADIOS_AUDIO_TONE_HIGHPASS;
    tone_through(voice, &shape, 0, 60, low); tone_through(voice, &shape, 0, 6000, high);
    g_assert_cmpfloat(rms(low + 24000, 24000), <, .1); g_assert_cmpfloat(rms(high + 24000, 24000), >, .2);
    /* The incoming side is untouched by an outgoing-only tone curve. */
    tone_through(voice, &shape, 1, 6000, high); g_assert_cmpfloat(rms(high + 24000, 24000), >, .7);
    shape.tone_kind[0] = RADIOS_AUDIO_TONE_NONE; shape.tone[0] = curve_of(2, flat_zero);
    shape.effect = RADIOS_FX_ECHO; shape.effect_depth = .8; shape.effect_side = 0;
    RadioSAudioShapeVoice *other = radios_audio_shape_voice_new(); gdouble tail = 0, dry_tail = 0;
    radios_audio_shape_voice_reset(voice);
    for (guint i = 0; i < 96000; ++i) {
        gfloat stereo[2] = {i < 24000 ? (gfloat)sin(2 * G_PI * 440 * i / 48000.0) : 0, 0}, dry[2] = {stereo[0], 0};
        radios_audio_shape_voice_process(voice, &shape, 0, .5, stereo); radios_audio_shape_voice_process(other, &shape, 1, .5, dry);
        if (i >= 48000) { tail += fabs(stereo[0]); dry_tail += fabs(dry[0]); }
    }
    g_assert_cmpfloat(tail, >, 100); g_assert_cmpfloat(dry_tail, ==, 0);
    radios_audio_shape_voice_free(other); radios_audio_shape_voice_free(voice);
}
/* A designed shape renders the real preview: the overlap follows the volume
 * steps (A alone, then B alone) and an invalid shape is refused before any
 * file is written. */
static void shaped_preview(Fixture *f, gconstpointer unused)
{
    (void)unused; RadioSAudioTransitionMedia s = settings(f); RadioSAudioTransitionShape shape; memset(&shape, 0, sizeof shape);
    const gdouble flat_one[] = {0, 1, 1, 1}, flat_zero[] = {0, 0, 1, 0}, out_step[] = {0, 1, .5, 1, .5, 0, 1, 0}, in_step[] = {0, 0, .5, 0, .5, 1, 1, 1};
    shape.enabled = TRUE; shape.effect = -1; shape.effect_bpm = 120; shape.effect_mix = curve_of(2, flat_one);
    for (guint side = 0; side < 2; ++side) { shape.bass[side] = curve_of(2, flat_one); shape.tone[side] = curve_of(2, flat_zero); }
    shape.volume[0] = curve_of(4, out_step); shape.volume[1] = curve_of(4, in_step); s.shape = &shape;
    gchar *path = NULL, *sha = NULL; guint64 frames = 0; GError *error = NULL; RadioSAudioTransitionPlan plan;
    g_assert_true(radios_audio_transition_plan(&s, &plan, NULL));
    g_assert_true(radios_audio_transition_prepare(&s, f->directory, NULL, &path, &sha, &frames, &error)); g_assert_no_error(error);
    gchar *data = NULL; gsize length = 0; g_assert_true(g_file_get_contents(path, &data, &length, NULL));
    g_assert_cmpuint(length, ==, 44 + frames * 8);
    guint64 quarter = plan.start_b + plan.overlap / 4, three = plan.start_b + plan.overlap * 3 / 4; gfloat sample[4];
    memcpy(sample, data + 44 + quarter * 8, 8); memcpy(sample + 2, data + 44 + three * 8, 8);
    close_to(sample[0], .25); close_to(sample[1], 0); close_to(sample[2], 0); close_to(sample[3], .125);
    g_free(data); g_assert_cmpint(g_unlink(path), ==, 0); g_free(path); g_free(sha);
    shape.volume[0].count = 1; path = sha = NULL;
    g_assert_false(radios_audio_transition_plan(&s, &plan, NULL));
    g_assert_false(radios_audio_transition_prepare(&s, f->directory, NULL, &path, &sha, &frames, &error)); g_clear_error(&error);
    g_assert_null(path); g_assert_null(sha);
}

/* The transition type is the preset's structure: every template has exactly
 * one, choosing a type normalises the preset to it (an audible default when a
 * stage is gained, "none" when it is lost) and the result stays a valid
 * preset without any new stored member. */
static void designed_type_normal_form(void)
{
    const gchar *expected[] = {"crossfade", "crossfade", "filter", "crossfade", "effect", "effect", "filter", "crossfade"};
    const gchar *allowed[] = {"id", "name", "duration_ms", "beat_sync", "out", "in", "effect"};
    JsonArray *templates = radios_transition_preset_templates(); g_assert_cmpuint(json_array_get_length(templates), ==, G_N_ELEMENTS(expected));
    for (guint i = 0; i < json_array_get_length(templates); ++i)
        g_assert_cmpstr(radios_transition_type_ids[radios_transition_preset_type(json_array_get_object_element(templates, i))], ==, expected[i]);
    json_array_unref(templates);
    g_assert_cmpint(radios_transition_type_index("filter"), ==, RADIOS_TRANSITION_TYPE_FILTER);
    g_assert_cmpint(radios_transition_type_index("cue"), ==, -1); g_assert_cmpint(radios_transition_type_index(NULL), ==, -1);
    g_assert_cmpint(radios_transition_preset_type(NULL), ==, RADIOS_TRANSITION_TYPE_CROSSFADE);
    JsonObject *preset = radios_transition_preset_template("Düz geçiş"); g_assert_nonnull(preset);
    JsonObject *effect = json_object_get_object_member(preset, "effect"), *out = json_object_get_object_member(preset, "out"), *in = json_object_get_object_member(preset, "in");
    g_assert_false(radios_transition_preset_set_type(preset, RADIOS_TRANSITION_TYPE_CROSSFADE));
    g_assert_true(radios_transition_preset_set_type(preset, RADIOS_TRANSITION_TYPE_EFFECT));
    g_assert_cmpstr(json_object_get_string_member(effect, "type"), ==, "echo"); g_assert_cmpint(json_object_get_int_member(effect, "depth"), ==, 50);
    g_assert_cmpint(radios_transition_preset_type(preset), ==, RADIOS_TRANSITION_TYPE_EFFECT); g_assert_true(radios_transition_preset_valid(preset, TRUE));
    g_assert_true(radios_transition_preset_set_type(preset, RADIOS_TRANSITION_TYPE_FILTER));
    g_assert_cmpstr(json_object_get_string_member(effect, "type"), ==, "none"); g_assert_cmpstr(json_object_get_string_member(out, "tone_kind"), ==, "lowpass");
    g_assert_cmpstr(json_object_get_string_member(in, "tone_kind"), ==, "none");
    JsonArray *tone = json_object_get_array_member(out, "tone"); g_assert_cmpuint(json_array_get_length(tone), ==, 2);
    close_to(json_array_get_double_element(json_array_get_array_element(tone, 1), 1), 1);
    g_assert_cmpint(radios_transition_preset_type(preset), ==, RADIOS_TRANSITION_TYPE_FILTER); g_assert_true(radios_transition_preset_valid(preset, TRUE));
    g_assert_true(radios_transition_preset_set_type(preset, RADIOS_TRANSITION_TYPE_EFFECT_FILTER));
    g_assert_cmpstr(json_object_get_string_member(effect, "type"), ==, "echo"); g_assert_cmpstr(json_object_get_string_member(out, "tone_kind"), ==, "lowpass");
    g_assert_cmpint(radios_transition_preset_type(preset), ==, RADIOS_TRANSITION_TYPE_EFFECT_FILTER); g_assert_true(radios_transition_preset_valid(preset, TRUE));
    json_object_set_string_member(in, "tone_kind", "highpass");
    g_assert_true(radios_transition_preset_set_type(preset, RADIOS_TRANSITION_TYPE_CROSSFADE));
    g_assert_cmpstr(json_object_get_string_member(effect, "type"), ==, "none"); g_assert_cmpstr(json_object_get_string_member(out, "tone_kind"), ==, "none");
    g_assert_cmpstr(json_object_get_string_member(in, "tone_kind"), ==, "none");
    g_assert_cmpint(radios_transition_preset_type(preset), ==, RADIOS_TRANSITION_TYPE_CROSSFADE); g_assert_true(radios_transition_preset_valid(preset, TRUE));
    GList *members = json_object_get_members(preset);
    for (GList *it = members; it; it = it->next) {
        gboolean known = FALSE;
        for (guint i = 0; i < G_N_ELEMENTS(allowed); ++i) known |= g_str_equal(allowed[i], it->data);
        g_assert_true(known);
    }
    g_list_free(members); json_object_unref(preset);
}
/* "Efekt yok": depth and tempo stay stored but the engine never reads them.
 * The rendered voice is sample-identical for depth 100 / 300 BPM and depth 0 /
 * 30 BPM, equal to its input, validation ignores both values, and only a real
 * effect changes the signal. */
static void effect_none_depth_tempo_inert(void)
{
    JsonObject *preset = radios_transition_preset_template("Düz geçiş"); g_assert_nonnull(preset);
    JsonObject *effect = json_object_get_object_member(preset, "effect"); RadioSAudioTransitionShape loud, quiet;
    json_object_set_int_member(effect, "depth", 100); json_object_set_int_member(effect, "bpm", 300);
    g_assert_true(radios_transition_preset_valid(preset, TRUE)); g_assert_true(radios_transition_preset_to_shape(preset, &loud));
    g_assert_cmpint(loud.effect, ==, -1); close_to(loud.effect_depth, 1); close_to(loud.effect_bpm, 300);
    json_object_set_int_member(effect, "depth", 0); json_object_set_int_member(effect, "bpm", 30);
    g_assert_true(radios_transition_preset_to_shape(preset, &quiet)); g_assert_cmpint(quiet.effect, ==, -1); close_to(quiet.effect_depth, 0);
    RadioSAudioTransitionShape wild = loud; wild.effect_depth = 7; wild.effect_bpm = -1;
    g_assert_true(radios_audio_transition_shape_validate(&wild, NULL));
    wild.effect = RADIOS_FX_ECHO; g_assert_false(radios_audio_transition_shape_validate(&wild, NULL));
    RadioSAudioShapeVoice *voice = radios_audio_shape_voice_new(), *other = radios_audio_shape_voice_new(); gdouble tail = 0;
    for (guint i = 0; i < 96000; ++i) {
        gfloat input = i < 24000 ? (gfloat)sin(2 * G_PI * 440 * i / 48000.0) : 0, a[2] = {input, input}, b[2] = {input, input};
        radios_audio_shape_voice_process(voice, &loud, 0, i / 96000.0, a); radios_audio_shape_voice_process(other, &quiet, 0, i / 96000.0, b);
        g_assert_cmpfloat(a[0], ==, b[0]); g_assert_cmpfloat(a[1], ==, b[1]); g_assert_cmpfloat(a[0], ==, input);
    }
    loud.effect = RADIOS_FX_ECHO; radios_audio_shape_voice_reset(voice);
    for (guint i = 0; i < 96000; ++i) {
        gfloat input = i < 24000 ? (gfloat)sin(2 * G_PI * 440 * i / 48000.0) : 0, a[2] = {input, input};
        radios_audio_shape_voice_process(voice, &loud, 0, i / 96000.0, a);
        if (i >= 48000) tail += fabs(a[0]);
    }
    g_assert_cmpfloat(tail, >, 100);
    radios_audio_shape_voice_free(other); radios_audio_shape_voice_free(voice); json_object_unref(preset);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/transition/verified-pair-real-pcm", Fixture, NULL, setup, pcm, teardown);
    g_test_add("/transition/designed-shape-real-preview", Fixture, NULL, setup, shaped_preview, teardown);
    g_test_add_func("/transition/designed-curves-and-templates", shape_curves);
    g_test_add_func("/transition/designed-voice-real-frequency-response", shape_voice_response);
    g_test_add("/transition/cancel-hash-bounds-no-artifact", Fixture, NULL, setup, rejection, teardown);
    g_test_add("/transition/level-hard-cut-bounded-window", Fixture, NULL, setup, level_and_end, teardown);
    g_test_add_func("/transition/isolated-lowpass-real-frequency-response", lowpass_isolated_response);
    g_test_add_func("/transition/designed-type-normal-form", designed_type_normal_form);
    g_test_add_func("/transition/effect-none-depth-tempo-inert", effect_none_depth_tempo_inert);
    return g_test_run();
}
