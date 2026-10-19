#ifndef RADIOS_OUTPUT_RESAMPLER_H
#define RADIOS_OUTPUT_RESAMPLER_H

#include <glib.h>

G_BEGIN_DECLS

typedef struct RadioSOutputResampler RadioSOutputResampler;

#define RADIOS_OUTPUT_RESAMPLER_MAX_FRAMES 4096u

/* A bounded playback-only bridge from internal 48 kHz stereo float PCM to
 * 44100/48000 Hz. A new instance begins a new output/tag generation. Calls are
 * confined to its owning audio thread. No GTK, devices or files are accessed. */
RadioSOutputResampler *radios_output_resampler_new(guint sample_rate, GError **error);
void radios_output_resampler_free(RadioSOutputResampler *resampler);

/* Input and output counts are at most MAX_FRAMES; output capacity is positive.
 * A partially accepted input is reported through consumed and must be retried.
 * The internal queue never exceeds 8192 source frames. Input tags are one per
 * 48 kHz frame; NULL assigns zero (silence/no source proof). Output tags identify
 * the source position reached by each output frame, including tag gaps; filter
 * delay is retained in the queue, never counted as consumed source playback.
 * A 48000 Hz bridge copies PCM/tags exactly without buffering. */
gboolean radios_output_resampler_convert(RadioSOutputResampler *resampler,
    const gfloat *input, const guint64 *input_tags, guint input_frames,
    gfloat *output, guint64 *output_tags, guint output_capacity,
    guint *consumed, guint *produced, GError **error);

/* Explicit end of generation: drain the filter tail. Repeat until produced=0.
 * Further input is rejected. Output tags never extend past the last input. */
gboolean radios_output_resampler_flush(RadioSOutputResampler *resampler,
    gfloat *output, guint64 *output_tags, guint output_capacity,
    guint *produced, GError **error);

G_END_DECLS

#endif
