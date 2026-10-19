#ifndef RADIOS_AUDIO_TEMPO_H
#define RADIOS_AUDIO_TEMPO_H
#include "radios/audio_decoder.h"
#include <math.h>
/* Output-frame clock for one frozen source occurrence. The matched ratio is
 * held through its incoming fade, then returns linearly to native tempo. */
typedef struct {
    gdouble ratio;
    guint64 hold_frames, return_frames;
} RadioSAudioTempoProfile;
static inline gdouble radios_audio_tempo_profile_ratio(const RadioSAudioTempoProfile *profile, gdouble output_frames)
{
    gdouble ratio = profile->ratio > 0 ? profile->ratio : 1;
    if (!profile->return_frames || output_frames <= profile->hold_frames) return ratio;
    gdouble progress = MIN(1.0, (output_frames - profile->hold_frames) / profile->return_frames);
    return ratio + (1 - ratio) * progress;
}
static inline gdouble radios_audio_tempo_profile_source(const RadioSAudioTempoProfile *profile, gdouble output_frames)
{
    gdouble ratio = profile->ratio > 0 ? profile->ratio : 1;
    if (!profile->return_frames || output_frames <= profile->hold_frames) return MAX(0.0, output_frames) * ratio;
    gdouble elapsed = output_frames - profile->hold_frames, ramp = MIN(elapsed, (gdouble)profile->return_frames);
    return profile->hold_frames * ratio + ramp * ratio + (1 - ratio) * ramp * ramp / (2 * profile->return_frames) + MAX(0.0, elapsed - ramp);
}
static inline gdouble radios_audio_tempo_profile_output(const RadioSAudioTempoProfile *profile, gdouble source_frames)
{
    gdouble ratio = profile->ratio > 0 ? profile->ratio : 1, held = profile->hold_frames * ratio;
    if (!profile->return_frames || source_frames <= held) return MAX(0.0, source_frames) / ratio;
    gdouble source = source_frames - held, ramp_source = profile->return_frames * (ratio + 1) / 2;
    if (source >= ramp_source) return profile->hold_frames + profile->return_frames + source - ramp_source;
    /* Stable quadratic inverse, including ratios approaching one. */
    gdouble slope = (1 - ratio) / profile->return_frames;
    return profile->hold_frames + 2 * source / (ratio + sqrt(ratio * ratio + 2 * slope * source));
}
typedef struct RadioSAudioTempo RadioSAudioTempo;
RadioSAudioTempo *radios_audio_tempo_new(gdouble ratio, guint64 source_frames, GError **error);
RadioSAudioTempo *radios_audio_tempo_new_profile(const RadioSAudioTempoProfile *profile,
    gdouble output_offset, guint64 source_frames, GError **error);
gssize radios_audio_tempo_read(RadioSAudioTempo *tempo, RadioSAudioDecoder *decoder,
    gfloat *stereo, guint frames, GCancellable *cancel, GError **error);
void radios_audio_tempo_free(RadioSAudioTempo *tempo);
#endif
