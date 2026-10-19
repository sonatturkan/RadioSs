#include "radios/rhythm.h"
#include <math.h>

static void test_click_grids(void)
{
    const gdouble tempos[] = {80, 100, 120, 128, 150, 180};
    for (guint index = 0; index < G_N_ELEMENTS(tempos); ++index) {
        RadioSRhythmAnalyzer *analyzer = radios_rhythm_analyzer_new(48000);
        for (guint frame = 0; frame < 48000 * 24; ++frame) {
            gdouble position = (gdouble)frame / 48000 - 0.125;
            gdouble phase = position >= 0 ? fmod(position, 60 / tempos[index]) : 1;
            gdouble sample = phase < 0.015 ? 0.8 * exp(-phase * 200) * sin(2 * G_PI * 600 * position) : 0;
            radios_rhythm_analyzer_push(analyzer, sample * sample);
        }
        RadioSRhythmAnalysis analysis = radios_rhythm_analyzer_finish(analyzer);
        g_test_message("%.1f BPM -> %.3f confidence %.3f first %.3f ms", tempos[index], analysis.bpm, analysis.confidence, analysis.first_beat_ms);
        g_assert_cmpfloat(analysis.confidence, >=, 0.45);
        gdouble ratio = analysis.bpm / tempos[index];
        g_assert_true(fabs(ratio - 1) < 0.01 || fabs(ratio - 0.5) < 0.01 || fabs(ratio - 2) < 0.01);
        /* Every grid line, including the bar start, must fall on a click. A
         * half-tempo grid may start on either of two clicks. */
        gdouble click = 60000 / tempos[index], first = fmod(analysis.first_beat_ms - 125 + click * 4, click);
        g_assert_cmpfloat(MIN(first, click - first), <, 10);
        g_assert_true(analysis.has_downbeat); g_assert_false(analysis.manual);
        gdouble bar = fmod(analysis.downbeat_ms - 125 + click * 4, click);
        g_assert_cmpfloat(MIN(bar, click - bar), <, 10); g_assert_cmpfloat(analysis.downbeat_ms, <, 4 * 60000 / analysis.bpm);
        JsonObject *json = radios_rhythm_analysis_json(&analysis); RadioSRhythmAnalysis read;
        g_assert_cmpstr(json_object_get_string_member(json, "algorithm"), ==, RADIOS_RHYTHM_ALGORITHM);
        g_assert_true(radios_rhythm_analysis_current(json));
        g_assert_true(radios_rhythm_analysis_read(json, &read)); g_assert_cmpfloat(read.bpm, ==, analysis.bpm);
        g_assert_true(read.has_downbeat); g_assert_cmpfloat(read.downbeat_ms, ==, analysis.downbeat_ms);
        json_object_unref(json); radios_rhythm_analyzer_free(analyzer);
    }
}
/* Kick drums carry the beat; the hi-hats between them and a bass note that
 * starts each bar must neither halve the tempo nor move the grid. */
static void test_band_limited_kicks_and_bar_start(void)
{
    const gdouble bpm = 124, beat = 60.0 / bpm;
    RadioSRhythmAnalyzer *analyzer = radios_rhythm_analyzer_new(44100);
    guint32 noise = 1;
    for (guint frame = 0; frame < 44100 * 40; ++frame) {
        gdouble t = (gdouble)frame / 44100 - 0.3, sample = 0;
        if (t >= 0) {
            gdouble in_beat = fmod(t, beat), in_half = fmod(t, beat / 2);
            guint beat_index = (guint)(t / beat);
            noise = noise * 1664525u + 1013904223u;
            if (in_beat < 0.12) sample += (beat_index % 4 ? 0.5 : 0.9) * exp(-in_beat * 30) * sin(2 * G_PI * 55 * in_beat);
            if (in_half < 0.03) sample += 0.15 * exp(-in_half * 150) * ((gdouble)(noise >> 8) / (1 << 24) * 2 - 1);
        }
        gfloat frame_samples[2] = {(gfloat)sample, (gfloat)sample};
        radios_rhythm_analyzer_push_frame(analyzer, frame_samples, 2);
    }
    RadioSRhythmAnalysis analysis = radios_rhythm_analyzer_finish(analyzer);
    g_test_message("124 BPM kicks -> %.3f confidence %.3f first %.1f bar %.1f", analysis.bpm, analysis.confidence, analysis.first_beat_ms, analysis.downbeat_ms);
    g_assert_cmpfloat(fabs(analysis.bpm - bpm), <, 0.5); g_assert_cmpfloat(analysis.confidence, >=, 0.45);
    gdouble period = 60000 / bpm, first = fmod(analysis.first_beat_ms - 300 + period * 4, period);
    g_assert_cmpfloat(MIN(first, period - first), <, 15);
    g_assert_true(analysis.has_downbeat);
    gdouble bar = fmod(analysis.downbeat_ms - 300 + period * 8, period * 4);
    g_assert_cmpfloat(MIN(bar, period * 4 - bar), <, 15);
    radios_rhythm_analyzer_free(analyzer);
}
static void test_stored_grid_versions(void)
{
    RadioSRhythmAnalysis manual = {.bpm = 121.5, .first_beat_ms = 210, .confidence = 1, .analyzed_ms = 120000,
        .downbeat_ms = 1198, .has_downbeat = TRUE, .manual = TRUE}, read;
    JsonObject *json = radios_rhythm_analysis_json(&manual);
    g_assert_cmpstr(json_object_get_string_member(json, "algorithm"), ==, RADIOS_RHYTHM_ALGORITHM_MANUAL);
    g_assert_true(radios_rhythm_analysis_current(json)); g_assert_true(radios_rhythm_analysis_read(json, &read));
    g_assert_true(read.manual && read.has_downbeat); g_assert_cmpfloat(read.downbeat_ms, ==, 1198); g_assert_cmpfloat(read.bpm, ==, 121.5);
    /* First-generation grids stay usable but are re-analysed; they never had a bar phase. */
    json_object_set_string_member(json, "algorithm", RADIOS_RHYTHM_ALGORITHM_LEGACY);
    g_assert_false(radios_rhythm_analysis_current(json)); g_assert_false(radios_rhythm_analysis_read(json, &read));
    json_object_remove_member(json, "downbeat_ms");
    g_assert_true(radios_rhythm_analysis_read(json, &read)); g_assert_false(read.has_downbeat || read.manual);
    json_object_set_string_member(json, "algorithm", "radios-onset-grid-v9");
    g_assert_false(radios_rhythm_analysis_current(json)); g_assert_false(radios_rhythm_analysis_read(json, &read));
    json_object_set_string_member(json, "algorithm", RADIOS_RHYTHM_ALGORITHM);
    json_object_set_double_member(json, "downbeat_ms", 120000);
    g_assert_false(radios_rhythm_analysis_read(json, &read));
    json_object_unref(json);
    g_assert_false(radios_rhythm_analysis_current(NULL));
}
/* Beatmatched transitions leave on the bar start nearest the marked point and
 * enter on the next track's bar start; beat-only grids keep beat snapping. */
static void test_bar_aligned_transitions(void)
{
    /* 120 BPM: a beat is 24000 frames; bars start at 1125 ms (54000 frames). */
    RadioSRhythmAnalysis grid = {.bpm = 120, .first_beat_ms = 125, .confidence = 0.9, .analyzed_ms = 120000,
        .downbeat_ms = 1125, .has_downbeat = TRUE}, beats = grid;
    beats.has_downbeat = FALSE;
    g_assert_cmpuint(radios_rhythm_transition_frame(&beats, 100000, 0, 480000), ==, 78000);
    g_assert_cmpuint(radios_rhythm_transition_frame(&grid, 100000, 0, 480000), ==, 54000);
    g_assert_cmpuint(radios_rhythm_transition_frame(&grid, 200000, 0, 480000), ==, 54000 + 96000 * 2);
    /* The nearer bar would leave no transition inside the cue span. */
    g_assert_cmpuint(radios_rhythm_transition_frame(&grid, 240000, 0, 245000), ==, 150000);
    /* Before the first beat, or with no grid, the marked trigger stands. */
    g_assert_cmpuint(radios_rhythm_transition_frame(&grid, 4000, 0, 480000), ==, 4000);
    RadioSRhythmAnalysis none = {0};
    g_assert_cmpuint(radios_rhythm_transition_frame(&none, 100000, 0, 480000), ==, 100000);
    guint64 frame = 0;
    g_assert_true(radios_rhythm_entry_frame(&beats, 30000, 480000, &frame)); g_assert_cmpuint(frame, ==, 30000);
    g_assert_true(radios_rhythm_entry_frame(&beats, 30001, 480000, &frame)); g_assert_cmpuint(frame, ==, 54000);
    g_assert_true(radios_rhythm_entry_frame(&grid, 30000, 480000, &frame)); g_assert_cmpuint(frame, ==, 54000);
    g_assert_true(radios_rhythm_entry_frame(&grid, 54500, 480000, &frame)); g_assert_cmpuint(frame, ==, 54000);
    g_assert_true(radios_rhythm_entry_frame(&grid, 55000, 480000, &frame)); g_assert_cmpuint(frame, ==, 150000);
    /* No bar start before cue-out: fall back to the next beat. */
    g_assert_true(radios_rhythm_entry_frame(&grid, 55000, 100000, &frame)); g_assert_cmpuint(frame, ==, 78000);
    g_assert_false(radios_rhythm_entry_frame(&grid, 79000, 100000, &frame));
    g_assert_false(radios_rhythm_entry_frame(&none, 0, 100000, &frame));
    /* Phase offset: identical grids at matching positions coincide; a deck one
     * beat into its bar is one beat ahead of a master on its bar start. */
    g_assert_cmpfloat(radios_rhythm_phase_offset(&grid, 1, 54000, &grid, 1, 54000, 4), ==, 0);
    g_assert_cmpfloat(fabs(radios_rhythm_phase_offset(&grid, 1, 78000, &grid, 1, 54000, 4) - 1), <, 1e-9);
    g_assert_cmpfloat(fabs(radios_rhythm_phase_offset(&grid, 1, 78000, &grid, 1, 54000, 1)), <, 1e-9);
    g_assert_cmpfloat(fabs(radios_rhythm_phase_offset(&grid, 1, 42000, &grid, 1, 54000, 4) - 3.5), <, 1e-9);
}
static void test_unknown_and_limits(void)
{
    RadioSRhythmAnalyzer *analyzer = radios_rhythm_analyzer_new(48000);
    for (guint frame = 0; frame < 48000 * 8; ++frame) radios_rhythm_analyzer_push(analyzer, 0.2);
    RadioSRhythmAnalysis analysis = radios_rhythm_analyzer_finish(analyzer);
    g_assert_cmpfloat(analysis.bpm, ==, 0); g_assert_cmpfloat(analysis.confidence, ==, 0);
    g_assert_false(analysis.has_downbeat);
    JsonObject *json = radios_rhythm_analysis_json(&analysis); g_assert_false(radios_rhythm_analysis_read(json, &analysis));
    /* A finished analysis without a reliable tempo is complete, not pending. */
    g_assert_true(radios_rhythm_analysis_current(json)); g_assert_false(json_object_has_member(json, "downbeat_ms"));
    json_object_unref(json); radios_rhythm_analyzer_free(analyzer);
    gdouble ratio = 0;
    g_assert_true(radios_rhythm_tempo_ratio(128, 120, &ratio)); g_assert_cmpfloat(fabs(ratio - 0.9375), <, 0.000001);
    g_assert_true(radios_rhythm_tempo_ratio(60, 128, &ratio)); g_assert_cmpfloat(fabs(ratio - 128.0 / 120), <, 0.000001);
    g_assert_false(radios_rhythm_tempo_ratio(100, 150, &ratio)); g_assert_false(radios_rhythm_tempo_ratio(NAN, 120, &ratio));
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/rhythm/real-click-grid-tempo-phase", test_click_grids);
    g_test_add_func("/rhythm/band-limited-kicks-bar-start", test_band_limited_kicks_and_bar_start);
    g_test_add_func("/rhythm/stored-grid-versions", test_stored_grid_versions);
    g_test_add_func("/rhythm/bar-aligned-transitions", test_bar_aligned_transitions);
    g_test_add_func("/rhythm/unknown-steady-audio-safe-tempo-limits", test_unknown_and_limits);
    return g_test_run();
}
