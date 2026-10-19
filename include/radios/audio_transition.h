#ifndef RADIOS_AUDIO_TRANSITION_H
#define RADIOS_AUDIO_TRANSITION_H
#include "radios/audio_decoder.h"
#include "radios/audio_dsp.h"

typedef struct {
    const gchar *path_a, *sha_a, *path_b, *sha_b;
    guint64 duration_a, duration_b, cue_a, out_a, cue_b;
    guint64 overlap, fade_in;
    gdouble duck_db, level_db;
    const RadioSAudioTransitionShape *shape; /* optional designed overlap; NULL or disabled keeps the envelope */
} RadioSAudioTransitionMedia;
typedef struct {
    guint64 start_a, end_a, start_b, frames, overlap, fade_in;
    gdouble duck_gain, duck_db, level_gain;
} RadioSAudioTransitionPlan;

/* All positions are actual 48 kHz sample frames. The bounded audition contains
 * the last 10 seconds before the overlap and 10 seconds after A ends. */
gboolean radios_audio_transition_plan(const RadioSAudioTransitionMedia *settings,
    RadioSAudioTransitionPlan *plan, GError **error);
void radios_audio_transition_envelope(const RadioSAudioTransitionPlan *plan,
    guint64 frame, gdouble *gain_a, gdouble *gain_b);
/* Worker-only streaming preparation, constant PCM memory, cancellable hash and
 * decoding. Caller owns the private temporary WAV and must delete it. */
gboolean radios_audio_transition_prepare(const RadioSAudioTransitionMedia *settings,
    const gchar *directory, GCancellable *cancellable, gchar **path,
    gchar **sha256, guint64 *frames, GError **error);
/* Render-safe isolated outgoing filter: logarithmic 18 kHz to 300 Hz sweep,
 * smoothly mixed from dry to wet. A zero-length transition is unchanged. */
typedef struct { gdouble stereo[2]; } RadioSAudioTransitionFilter;
void radios_audio_transition_lowpass(RadioSAudioTransitionFilter *state,
    guint64 elapsed, guint64 total, gfloat stereo[2]);
#endif
