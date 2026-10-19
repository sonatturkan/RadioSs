#include "radios/output_resampler.h"

#include <gio/gio.h>
#include <math.h>
#include <string.h>

#define TAU 6.2831853071795864769

static void copy_path(void)
{
    GError *error = NULL;
    RadioSOutputResampler *resampler = radios_output_resampler_new(48000, &error);
    g_assert_no_error(error); g_assert_nonnull(resampler);
    const gfloat input[] = {.25f, -.25f, .5f, -.5f, .75f, -.75f};
    const guint64 source_tags[] = {17, 0, 193};
    gfloat output[6] = {0}; guint64 tags[3] = {0}; guint consumed, produced;
    g_assert_true(radios_output_resampler_convert(resampler, input, source_tags, 3,
        output, tags, 2, &consumed, &produced, &error));
    g_assert_no_error(error); g_assert_cmpuint(consumed, ==, 2); g_assert_cmpuint(produced, ==, 2);
    g_assert_cmpint(memcmp(input, output, sizeof(gfloat) * 4), ==, 0);
    g_assert_cmpuint(tags[0], ==, 17); g_assert_cmpuint(tags[1], ==, 0);
    g_assert_true(radios_output_resampler_convert(resampler, input + 4, source_tags + 2, 1,
        output, tags, 3, &consumed, &produced, &error));
    g_assert_cmpuint(consumed, ==, 1); g_assert_cmpuint(produced, ==, 1); g_assert_cmpuint(tags[0], ==, 193);
    g_assert_cmpfloat(output[0], ==, .75f); g_assert_cmpfloat(output[1], ==, -.75f);
    g_assert_true(radios_output_resampler_flush(resampler, output, tags, 3, &produced, &error));
    g_assert_cmpuint(produced, ==, 0);
    g_assert_false(radios_output_resampler_convert(resampler, input, source_tags, 1,
        output, tags, 3, &consumed, &produced, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
    radios_output_resampler_free(resampler);
}

static void append_output(GArray *pcm, GArray *tags, const gfloat *block,
                          const guint64 *source_tags, guint produced)
{
    g_array_append_vals(pcm, block, produced * 2);
    g_array_append_vals(tags, source_tags, produced);
}

static void stream(const gfloat *input, const guint64 *source_tags, guint frames,
                   guint capacity, GArray *pcm, GArray *tags)
{
    GError *error = NULL;
    RadioSOutputResampler *resampler = radios_output_resampler_new(44100, &error);
    g_assert_no_error(error); g_assert_nonnull(resampler);
    gfloat output[8192]; guint64 block_tags[4096];
    guint position = 0, step = 0;
    const guint chunks[] = {17, 257, 4096, 1000, 63};
    while (position < frames) {
        guint chunk = chunks[step++ % G_N_ELEMENTS(chunks)];
        guint supplied = MIN(chunk, frames - position);
        guint consumed, produced;
        gboolean success = radios_output_resampler_convert(resampler, input + position * 2,
            source_tags ? source_tags + position : NULL, supplied,
            output, block_tags, capacity, &consumed, &produced, &error);
        g_assert_no_error(error); g_assert_true(success); g_assert_cmpuint(consumed, <=, supplied);
        g_assert_cmpuint(produced, <=, capacity); g_assert_cmpuint(consumed + produced, >, 0);
        position += consumed;
        append_output(pcm, tags, output, block_tags, produced);
    }
    guint produced;
    do {
        g_assert_true(radios_output_resampler_flush(resampler, output, block_tags, capacity, &produced, &error));
        g_assert_no_error(error);
        append_output(pcm, tags, output, block_tags, produced);
    } while (produced);
    radios_output_resampler_free(resampler);
}

static gdouble tone_amplitude(const gfloat *pcm, guint frames, guint channel, gdouble frequency)
{
    gdouble real = 0, imaginary = 0;
    guint first = 100, last = frames - 100;
    for (guint i = first; i < last; ++i) {
        gdouble phase = TAU * frequency * i / 44100;
        real += pcm[i * 2 + channel] * cos(phase);
        imaginary += pcm[i * 2 + channel] * sin(phase);
    }
    return 2 * hypot(real, imaginary) / (last - first);
}

static void quality_duration_tags(void)
{
    gfloat *input = g_new(gfloat, 48000 * 2);
    guint64 *source_tags = g_new(guint64, 48000);
    for (guint i = 0; i < 48000; ++i) {
        input[i * 2] = .4f * (gfloat)sin(TAU * 1000 * i / 48000);
        input[i * 2 + 1] = .2f * (gfloat)sin(TAU * 1500 * i / 48000);
        source_tags[i] = i + 1001;
    }
    const guint capacities[] = {1, 64, 511, 4096};
    for (guint option = 0; option < G_N_ELEMENTS(capacities); ++option) {
        GArray *pcm = g_array_new(FALSE, FALSE, sizeof(gfloat));
        GArray *tags = g_array_new(FALSE, FALSE, sizeof(guint64));
        stream(input, source_tags, 48000, capacities[option], pcm, tags);
        g_assert_cmpuint(tags->len, ==, 44100); g_assert_cmpuint(pcm->len, ==, 88200);
        const gfloat *output = (gfloat *)pcm->data;
        g_assert_cmpfloat(fabs(tone_amplitude(output, tags->len, 0, 1000) - .4), <, .002);
        g_assert_cmpfloat(fabs(tone_amplitude(output, tags->len, 1, 1500) - .2), <, .002);
        g_assert_cmpfloat(tone_amplitude(output, tags->len, 0, 1500), <, .002);
        g_assert_cmpfloat(tone_amplitude(output, tags->len, 1, 1000), <, .002);
        for (guint i = 0; i < tags->len; ++i) {
            guint64 position = (guint64)(i + 1) * 48000 / 44100;
            g_assert_cmpuint(g_array_index(tags, guint64, i), ==, 1000 + position);
            if (i) g_assert_cmpuint(g_array_index(tags, guint64, i), >, g_array_index(tags, guint64, i - 1));
        }
        g_array_unref(pcm); g_array_unref(tags);
    }
    g_free(input); g_free(source_tags);
}

static void gaps_and_filter_delay(void)
{
    gfloat input[2048] = {0}; guint64 source_tags[1024];
    for (guint i = 0; i < 1024; ++i)
        source_tags[i] = i < 300 ? 100 + i : (i < 700 ? 0 : 9000 + i);
    GArray *pcm = g_array_new(FALSE, FALSE, sizeof(gfloat));
    GArray *tags = g_array_new(FALSE, FALSE, sizeof(guint64));
    stream(input, source_tags, 1024, 127, pcm, tags);
    g_assert_cmpuint(tags->len, ==, 941);
    guint64 previous = 0;
    for (guint i = 0; i < tags->len; ++i) {
        guint64 position = MIN((guint64)(i + 1) * 48000 / 44100, 1024u);
        guint64 value = g_array_index(tags, guint64, i);
        g_assert_cmpuint(value, ==, source_tags[position - 1]);
        if (value) { g_assert_cmpuint(value, >=, previous); previous = value; }
    }
    g_array_unref(pcm); g_array_unref(tags);
    GError *error = NULL;
    RadioSOutputResampler *resampler = radios_output_resampler_new(44100, &error);
    g_assert_no_error(error);
    gfloat output[256]; guint64 output_tag[128]; guint consumed, produced;
    g_assert_true(radios_output_resampler_convert(resampler, input, source_tags, 8,
        output, output_tag, 128, &consumed, &produced, &error));
    g_assert_cmpuint(consumed, ==, 8); g_assert_cmpuint(produced, ==, 0);
    /* Buffered filter lookahead must not pretend physical playback occurred. */
    g_assert_true(radios_output_resampler_convert(resampler, input + 16, source_tags + 8, 120,
        output, output_tag, 128, &consumed, &produced, &error));
    g_assert_cmpuint(consumed, ==, 120); g_assert_cmpuint(produced, >, 0);
    g_assert_cmpuint(output_tag[0], ==, 100);
    g_assert_cmpuint(output_tag[produced - 1], <, 228);
    radios_output_resampler_free(resampler);
}

static void validation_and_restart(void)
{
    GError *error = NULL;
    g_assert_null(radios_output_resampler_new(96000, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
    RadioSOutputResampler *resampler = radios_output_resampler_new(44100, &error);
    gfloat input[64] = {0}, output[64]; guint64 tags[32]; guint consumed, produced;
    input[0] = NAN;
    g_assert_false(radios_output_resampler_convert(resampler, input, NULL, 32, output, tags, 32,
        &consumed, &produced, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
    g_assert_cmpuint(consumed, ==, 0); g_assert_cmpuint(produced, ==, 0);
    input[0] = 0;
    g_assert_false(radios_output_resampler_convert(resampler, input, NULL, 32, output, tags, 0,
        &consumed, &produced, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_clear_error(&error);
    g_assert_true(radios_output_resampler_convert(resampler, input, NULL, 32, output, tags, 32,
        &consumed, &produced, &error));
    g_assert_no_error(error);
    for (guint i = 0; i < produced; ++i) g_assert_cmpuint(tags[i], ==, 0);
    radios_output_resampler_free(resampler);
    /* A fresh output generation has neither the old filter tail nor old tags. */
    resampler = radios_output_resampler_new(44100, &error);
    g_assert_true(radios_output_resampler_convert(resampler, input, NULL, 0, output, tags, 32,
        &consumed, &produced, &error));
    g_assert_cmpuint(consumed, ==, 0); g_assert_cmpuint(produced, ==, 0);
    radios_output_resampler_free(resampler);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/output-resampler/48000-copy", copy_path);
    g_test_add_func("/output-resampler/44100-quality-duration-tags", quality_duration_tags);
    g_test_add_func("/output-resampler/gaps-filter-delay", gaps_and_filter_delay);
    g_test_add_func("/output-resampler/validation-generation", validation_and_restart);
    return g_test_run();
}
