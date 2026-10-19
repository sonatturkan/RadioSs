#include "radios/loudness.h"
#include <math.h>

static gdouble measure_sine(guint rate, guint channels, gdouble dbfs, gdouble seconds, gboolean *measured)
{
    RadioSLoudnessMeter *meter = radios_loudness_meter_new(rate, channels);
    gdouble amplitude = pow(10, dbfs / 20), lufs = 0; gfloat frame[2];
    for (guint index = 0; index < (guint)(rate * seconds); ++index) {
        frame[0] = frame[1] = (gfloat)(amplitude * sin(2 * G_PI * 1000 * index / rate));
        radios_loudness_meter_push_frame(meter, frame);
    }
    *measured = radios_loudness_meter_finish(meter, &lufs); radios_loudness_meter_free(meter); return lufs;
}
/* EBU Tech 3341 case 1: a 1 kHz stereo sine peaking at -23 dBFS reads -23.0
 * LUFS at any sample rate. A mono copy plays on both speakers and reads the same. */
static void test_reference_sine(void)
{
    const guint rates[] = {44100, 48000, 96000}; gboolean measured = FALSE;
    for (guint index = 0; index < G_N_ELEMENTS(rates); ++index) {
        gdouble lufs = measure_sine(rates[index], 2, -23, 20, &measured);
        g_test_message("%u Hz stereo -> %.3f LUFS", rates[index], lufs);
        g_assert_true(measured); g_assert_cmpfloat(fabs(lufs + 23), <, 0.1);
    }
    gdouble mono = measure_sine(48000, 1, -23, 20, &measured);
    g_assert_true(measured); g_assert_cmpfloat(fabs(mono + 23), <, 0.1);
    g_assert_cmpfloat(fabs(measure_sine(48000, 2, -33, 20, &measured) + 33), <, 0.1);
}
/* EBU Tech 3341 case 3: the relative gate ignores quiet passages. */
static void test_relative_gate(void)
{
    RadioSLoudnessMeter *meter = radios_loudness_meter_new(48000, 2); gfloat frame[2]; gdouble lufs = 0;
    const gdouble levels[] = {-36, -23, -36}, seconds[] = {10, 60, 10};
    guint offset = 0;
    for (guint part = 0; part < 3; ++part) {
        gdouble amplitude = pow(10, levels[part] / 20);
        for (guint index = 0; index < (guint)(48000 * seconds[part]); ++index, ++offset) {
            frame[0] = frame[1] = (gfloat)(amplitude * sin(2 * G_PI * 1000 * offset / 48000));
            radios_loudness_meter_push_frame(meter, frame);
        }
    }
    g_assert_true(radios_loudness_meter_finish(meter, &lufs)); radios_loudness_meter_free(meter);
    g_test_message("gated -> %.3f LUFS", lufs);
    g_assert_cmpfloat(fabs(lufs + 23), <, 0.1);
}
static void test_silence_and_json(void)
{
    gboolean measured = TRUE;
    (void)measure_sine(48000, 2, -200, 5, &measured); g_assert_false(measured);
    g_assert_null(radios_loudness_meter_new(48000, 0));
    RadioSLoudness loudness = {.integrated_lufs = -9.25, .peak = 0.97, .measured = TRUE}, read;
    JsonObject *analysis = json_object_new();
    json_object_set_object_member(analysis, "loudness", radios_loudness_json(&loudness));
    json_object_set_double_member(analysis, "peak", 0.97);
    g_assert_true(radios_loudness_read(analysis, &read));
    g_assert_true(read.measured); g_assert_cmpfloat(read.integrated_lufs, ==, -9.25); g_assert_cmpfloat(read.peak, ==, 0.97);
    RadioSLoudness silent = {0};
    json_object_set_object_member(analysis, "loudness", radios_loudness_json(&silent));
    g_assert_true(radios_loudness_read(analysis, &read)); g_assert_false(read.measured);
    json_object_set_string_member(json_object_get_object_member(analysis, "loudness"), "algorithm", "other");
    g_assert_false(radios_loudness_read(analysis, &read));
    json_object_remove_member(analysis, "loudness"); g_assert_false(radios_loudness_read(analysis, &read));
    json_object_unref(analysis);
}
/* Cuts always reach the target; boosts stop at a -1 dBFS sample peak. */
static void test_gain(void)
{
    RadioSLoudness loud = {.integrated_lufs = -9, .peak = 1, .measured = TRUE};
    g_assert_cmpfloat(fabs(radios_loudness_gain_db(&loud, -14) + 5), <, 1e-9);
    RadioSLoudness quiet = {.integrated_lufs = -20, .peak = 0.5, .measured = TRUE};
    g_assert_cmpfloat(fabs(radios_loudness_gain_db(&quiet, -14) - (-1 + 20 * log10(2))), <, 1e-9);
    quiet.peak = 0.25; g_assert_cmpfloat(fabs(radios_loudness_gain_db(&quiet, -14) - 6), <, 1e-9);
    quiet.peak = 0.99; g_assert_cmpfloat(radios_loudness_gain_db(&quiet, -14), ==, 0);
    RadioSLoudness unknown = {0}; g_assert_cmpfloat(radios_loudness_gain_db(&unknown, -14), ==, 0);
    RadioSLoudness whisper = {.integrated_lufs = -60, .peak = 0.001, .measured = TRUE};
    g_assert_cmpfloat(radios_loudness_gain_db(&whisper, -14), ==, 12);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/loudness/ebu-reference-sine", test_reference_sine);
    g_test_add_func("/loudness/relative-gate", test_relative_gate);
    g_test_add_func("/loudness/silence-and-json", test_silence_and_json);
    g_test_add_func("/loudness/peak-limited-gain", test_gain);
    return g_test_run();
}
