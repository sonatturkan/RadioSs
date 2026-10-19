#include "radios/output_resampler.h"

#include <gio/gio.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
#include <math.h>
#include <string.h>

#define SOURCE_RATE 48000u
#define TAG_CAPACITY 8192u

struct RadioSOutputResampler {
    SwrContext *converter;
    guint rate;
    guint64 input_frames, output_frames;
    guint64 tags[TAG_CAPACITY];
    gboolean flushing;
};

static gboolean failure(GError **error, const gchar *message)
{
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, message);
    return FALSE;
}

static gboolean converter_failure(GError **error, gint code)
{
    gchar message[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(code, message, sizeof message);
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Çıkış örneklemesi: %s", message);
    return FALSE;
}

RadioSOutputResampler *radios_output_resampler_new(guint sample_rate, GError **error)
{
    if (sample_rate != 44100 && sample_rate != SOURCE_RATE) {
        failure(error, "Çıkış örneklemesi 44100 veya 48000 Hz olmalı.");
        return NULL;
    }
    RadioSOutputResampler *resampler = g_new0(RadioSOutputResampler, 1);
    resampler->rate = sample_rate;
    if (sample_rate == SOURCE_RATE) return resampler;
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
    gint code = swr_alloc_set_opts2(&resampler->converter,
        &stereo, AV_SAMPLE_FMT_FLT, (gint)sample_rate,
        &stereo, AV_SAMPLE_FMT_FLT, SOURCE_RATE, 0, NULL);
    if (code >= 0) code = swr_init(resampler->converter);
    if (code < 0) {
        converter_failure(error, code);
        radios_output_resampler_free(resampler);
        return NULL;
    }
    return resampler;
}

void radios_output_resampler_free(RadioSOutputResampler *resampler)
{
    if (!resampler) return;
    swr_free(&resampler->converter);
    g_free(resampler);
}

static guint64 source_position(const RadioSOutputResampler *resampler, guint64 output_frames)
{
    /* Split multiplication to keep the playback clock valid for long runs. */
    return (output_frames / resampler->rate) * SOURCE_RATE +
        (output_frames % resampler->rate) * SOURCE_RATE / resampler->rate;
}

static void output_tags(RadioSOutputResampler *resampler, guint64 *tags, guint count)
{
    for (guint i = 0; i < count; ++i) {
        guint64 position = MIN(source_position(resampler, resampler->output_frames + i + 1), resampler->input_frames);
        if (tags) tags[i] = position ? resampler->tags[(position - 1) % TAG_CAPACITY] : 0;
    }
    resampler->output_frames += count;
}

static gboolean output_valid(RadioSOutputResampler *resampler, gfloat *output,
                             guint capacity, GError **error)
{
    return (resampler && output && capacity > 0 && capacity <= RADIOS_OUTPUT_RESAMPLER_MAX_FRAMES) ||
        failure(error, "Örnekleyici çıkışı ve 1–4096 kare kapasitesi gerekli.");
}

gboolean radios_output_resampler_convert(RadioSOutputResampler *resampler,
    const gfloat *input, const guint64 *input_tags, guint input_frames,
    gfloat *output, guint64 *tags, guint capacity,
    guint *consumed, guint *produced, GError **error)
{
    if (consumed) *consumed = 0;
    if (produced) *produced = 0;
    if (!consumed || !produced || !output_valid(resampler, output, capacity, error)) return FALSE;
    if (resampler->flushing || input_frames > RADIOS_OUTPUT_RESAMPLER_MAX_FRAMES || (input_frames && !input))
        return failure(error, "Örnekleyici girdisi geçersiz veya bu çıkış nesli bitmiş.");
    for (guint i = 0; i < input_frames * 2; ++i)
        if (!isfinite(input[i])) return failure(error, "Çıkış PCM örnekleri sonlu olmalı.");
    if (resampler->rate == SOURCE_RATE) {
        guint frames = MIN(input_frames, capacity);
        if (frames) memcpy(output, input, frames * 2 * sizeof *output);
        if (tags) {
            if (input_tags) memcpy(tags, input_tags, frames * sizeof *tags);
            else memset(tags, 0, frames * sizeof *tags);
        }
        resampler->input_frames += frames; resampler->output_frames += frames;
        *consumed = *produced = frames;
        return TRUE;
    }
    guint64 reached = MIN(source_position(resampler, resampler->output_frames), resampler->input_frames);
    guint64 queued = resampler->input_frames - reached;
    guint accepted = (guint)MIN((guint64)input_frames, TAG_CAPACITY - MIN(queued, TAG_CAPACITY));
    for (guint i = 0; i < accepted; ++i)
        resampler->tags[(resampler->input_frames + i) % TAG_CAPACITY] = input_tags ? input_tags[i] : 0;
    /* A non-NULL zero-length input drains already buffered output without
     * telling libswresample that this generation has ended. */
    const gfloat empty[2] = {0, 0};
    const guint8 *source[] = {(const guint8 *)(accepted ? input : empty)};
    guint8 *destination[] = {(guint8 *)output};
    gint count = swr_convert(resampler->converter, destination, (gint)capacity, source, (gint)accepted);
    if (count < 0) return converter_failure(error, count);
    resampler->input_frames += accepted;
    output_tags(resampler, tags, (guint)count);
    *consumed = accepted; *produced = (guint)count;
    return TRUE;
}

gboolean radios_output_resampler_flush(RadioSOutputResampler *resampler,
    gfloat *output, guint64 *tags, guint capacity,
    guint *produced, GError **error)
{
    if (produced) *produced = 0;
    if (!produced || !output_valid(resampler, output, capacity, error)) return FALSE;
    resampler->flushing = TRUE;
    if (resampler->rate == SOURCE_RATE) return TRUE;
    guint8 *destination[] = {(guint8 *)output};
    gint count = swr_convert(resampler->converter, destination, (gint)capacity, NULL, 0);
    if (count < 0) return converter_failure(error, count);
    output_tags(resampler, tags, (guint)count);
    *produced = (guint)count;
    return TRUE;
}
