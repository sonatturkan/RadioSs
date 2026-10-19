#ifndef RADIOS_AUDIO_DSP_H
#define RADIOS_AUDIO_DSP_H
#include <gio/gio.h>

G_BEGIN_DECLS
#define RADIOS_AUDIO_MIXER_CHANNELS 32u
#define RADIOS_AUDIO_FX_SLOTS 5u
#define RADIOS_AUDIO_NORMAL_DECKS 32u
#define RADIOS_AUDIO_SAMPLER_BASE 33u
#define RADIOS_AUDIO_MASTER_SAMPLER 65u
#define RADIOS_AUDIO_DECKS 66u

typedef enum {
    RADIOS_FX_DELAY, RADIOS_FX_ECHO, RADIOS_FX_SPIRAL, RADIOS_FX_REVERB,
    RADIOS_FX_TRANS, RADIOS_FX_FILTER, RADIOS_FX_FLANGER, RADIOS_FX_PHASER,
    RADIOS_FX_ROBOT, RADIOS_FX_MELODIC, RADIOS_FX_SLIP_ROLL, RADIOS_FX_ROLL,
    RADIOS_FX_REV_ROLL, RADIOS_FX_MIDI_LFO, RADIOS_FX_COUNT
} RadioSAudioFxType;
typedef struct {
    gint source_deck; /* -2 microphone, -1 empty, 0..31 normal source */
    gdouble eq_low_db, eq_mid_db, eq_high_db, pan, gain_db;
    gboolean mute, solo, pfl; /* PFL is runtime only */
    gint crossfade_group; /* 0 unassigned, 1 A, 2 B */
    gint fx_slot; /* -1 bypass, 0..4 shared parameter slot */
    gdouble fx_send_db, fx_return_db;
    gint return_effect; /* -2 legacy own SEND return, -1 off, 0..4 independent wet bus */
    gint send_channel, return_channel; /* legacy channel routing, separate from effects */
    gdouble send_db, return_db;
    gboolean loudness_bypass; /* saved channel opt-out; FALSE follows the global LUFS policy */
} RadioSAudioChannelDsp;
typedef struct {
    RadioSAudioFxType type;
    gboolean enabled, cue; /* CUE is runtime only */
    gdouble depth, send_db, return_db, tempo_bpm;
    gint monitor_path; /* 0 SEND, 1 RETURN, 2 SEND + RETURN */
} RadioSAudioFx;
typedef struct {
    gdouble eq_low_db, eq_mid_db, eq_high_db;
    gboolean mute;
    gint fx_slot; /* -1 bypass, 0..4 shared auxiliary slot; inline FX is separate */
    gdouble fx_send_db, fx_return_db;
    gint return_effect; /* -2 legacy own SEND return, -1 off, 0..4 independent wet bus */
    gint send_channel, return_channel; /* legacy channel routing, separate from effects */
    gdouble send_db, return_db;
} RadioSAudioMasterDsp;
typedef struct {
    RadioSAudioChannelDsp channels[RADIOS_AUDIO_MIXER_CHANNELS];
    RadioSAudioMasterDsp master;
    RadioSAudioFx effects[RADIOS_AUDIO_FX_SLOTS], master_fx;
    gdouble crossfader; /* -1 A, +1 B; equal-power gains */
    gboolean speech_ducking;
    gdouble speech_bed_db; /* -36..0; actual gated microphone detector */
    gint speech_fx_slot; /* -1 off, 0..4 independent gated microphone SEND */
    gboolean speech_to_master; /* TRUE preserves legacy microphone routing */
    gint speech_channel; /* -1 no dry destination, 0..31 channel ingress when !speech_to_master */
} RadioSAudioMixerConfig;
typedef struct {
    gfloat channel_peak[RADIOS_AUDIO_MIXER_CHANNELS][2];
    gfloat channel_fx_send_peak[RADIOS_AUDIO_MIXER_CHANNELS][2], master_fx_send_peak[2]; /* actual auxiliary SEND inputs */
    gfloat microphone_peak[2], master_peak[2], speech_fx_send_peak[2];
    gfloat fx_send_peak[RADIOS_AUDIO_FX_SLOTS][2], fx_return_peak[RADIOS_AUDIO_FX_SLOTS][2];
    gdouble speech_gain;
    gdouble channel_loudness_gain_db[RADIOS_AUDIO_MIXER_CHANNELS]; /* total automatic correction: actual fader plus residual ingress trim */
    gboolean channel_loudness_active[RADIOS_AUDIO_MIXER_CHANNELS];
} RadioSAudioDspMeters;
typedef struct RadioSAudioDsp RadioSAudioDsp;
/* Per-frame transport activity, independent of waveform zero crossings.
 * NULL restores sample-derived activity for standalone DSP callers. */
void radios_audio_dsp_set_source_activity(RadioSAudioDsp *dsp, const gboolean *active);
guint32 radios_audio_dsp_contributing_solo_mask(const RadioSAudioDsp *dsp);
/* Runtime trim for the channel's directly assigned player, before EQ/fader.
 * Samplers, microphone and already processed channel returns are excluded.
 * Serialized caller; gain changes ramp over 240 samples. */
void radios_audio_dsp_set_channel_loudness(RadioSAudioDsp *dsp, guint channel,
    gdouble gain_db, gboolean enabled);
/* Automatic mixer volume: control the actual visible channel fader and apply
 * only the correction above its +6 dB range as residual direct-source trim.
 * Disabled/bypassed/unassigned channels restore manual_gain_db. Changes ramp
 * over 240 frames; serialized, allocation-free caller (including renderer). */
void radios_audio_dsp_set_channel_auto_gain(RadioSAudioDsp *dsp, guint channel,
    gdouble manual_gain_db, gdouble correction_db, gboolean enabled);

void radios_audio_mixer_config_default(RadioSAudioMixerConfig *config);
gboolean radios_audio_mixer_config_validate(const RadioSAudioMixerConfig *config, GError **error);
RadioSAudioDsp *radios_audio_dsp_new(void);
void radios_audio_dsp_free(RadioSAudioDsp *dsp);
gboolean radios_audio_dsp_configure(RadioSAudioDsp *dsp, const RadioSAudioMixerConfig *config, GError **error);
void radios_audio_dsp_get_config(const RadioSAudioDsp *dsp, RadioSAudioMixerConfig *config);
void radios_audio_dsp_reset(RadioSAudioDsp *dsp);
/* Microphone gate ramp in frames (0..4800; default 240 = 5 ms at 48 kHz). A
 * gate change resets no filter (strip or MASTER EQ) and no programme effect:
 * the MASTER inline and aux effects (fed by the summed programme, MASTER
 * sampler included) and every music history keep running; ducking keeps its
 * own envelope. The gate owns only the microphone's voice histories: the
 * speech SEND and the strips' voice effects. The on-air ingress ramps
 * linearly (opening reaches full level and closing reaches silence within
 * the ramp); on closing, the voice histories' wet output fades over the same
 * frames and they are cleared while silent, so there is no click and no
 * voice-effect tail; a microphone reopened while still fading continues
 * seamlessly. 0 restores the immediate legacy gate: the on-air microphone
 * switches at the frame and the voice histories restart at each change.
 * The device signal still feeds the microphone meter, the ducking detector
 * and a microphone strip's PFL. Serialized caller. */
void radios_audio_dsp_set_microphone_ramp(RadioSAudioDsp *dsp, guint frames);
void radios_audio_dsp_begin_block(RadioSAudioDsp *dsp);
/* Serialized caller. One real sample frame, no allocation or I/O.
 * source_stereo has 66*2 floats, each source is already advanced exactly once.
 * Sampler 33+n combines only with mixer channel n; sampler65 goes to MASTER.
 * cue_mix is true only for CUE source=-1; explicit preview32 stays exclusive.
 * Channel PFL is post EQ/gain, before mute, solo and crossfader.
 * Independent per-source effect history shares the five slot parameters.
 * Explicit effect returns consume wet-only buses after all sends are processed;
 * they never feed the channel SEND graph or the master auxiliary SEND. */
void radios_audio_dsp_process_frame(RadioSAudioDsp *dsp, const gfloat *source_stereo,
    const gboolean *main_routed, const gfloat microphone[2], gboolean microphone_gate,
    gboolean cue_mix, gfloat main_output[2], gfloat cue_output[2]);
/* Optional external music is mixed before speech ducking and master DSP. */
void radios_audio_dsp_process_frame_with_external(RadioSAudioDsp *dsp,
    const gfloat *source_stereo, const gboolean *main_routed,
    const gfloat microphone[2], gboolean microphone_gate,
    const gfloat external_stereo[2], gboolean cue_mix,
    gfloat main_output[2], gfloat cue_output[2]);
void radios_audio_dsp_get_meters(const RadioSAudioDsp *dsp, RadioSAudioDspMeters *meters);
/* While enabled, each processed frame keeps every channel's post-fader
 * contribution to MAIN (after mute/solo, crossfader and its own effect
 * return); muted or silent channels read zero. */
void radios_audio_dsp_capture_channels(RadioSAudioDsp *dsp, gboolean enabled);
void radios_audio_dsp_channel_frame(const RadioSAudioDsp *dsp, guint channel, gfloat output[2]);
const gchar *radios_audio_fx_name(RadioSAudioFxType type);

/* Sample positions are relative to the A preview start. B starts at
 * a_frames-overlap_frames. A fades out over overlap and is ducked while B
 * overlaps; B fades in independently. Both sources remain in sample units. */
typedef struct {
    guint64 a_frames, overlap_frames, fade_in_frames;
    gdouble duck_db;
} RadioSAudioTransition;
gboolean radios_audio_transition_validate(const RadioSAudioTransition *transition, GError **error);
void radios_audio_transition_gains(const RadioSAudioTransition *transition,
    guint64 elapsed_frames, gdouble *a_gain, gdouble *b_gain);

/* Designed AutoDJ transition: piecewise-linear curves over the overlap.
 * t runs 0..1 from the first to the last overlap frame; values are 0..1.
 * Side 0 is the outgoing source, side 1 the incoming one. volume replaces the
 * equal-power envelope. bass 1 keeps the 200 Hz shelf flat, 0 cuts it 30 dB.
 * tone 0 leaves the filter open, 1 closes it (low-pass down to 300 Hz or
 * high-pass up to 8 kHz). effect renders one mixer effect type on a private
 * voice of effect_side; effect_mix is its wet share. A disabled shape keeps
 * the legacy envelope and outgoing filter. */
#define RADIOS_AUDIO_CURVE_POINTS 8u
typedef struct { guint count; gdouble t[RADIOS_AUDIO_CURVE_POINTS], v[RADIOS_AUDIO_CURVE_POINTS]; } RadioSAudioCurve;
typedef enum { RADIOS_AUDIO_TONE_NONE, RADIOS_AUDIO_TONE_LOWPASS, RADIOS_AUDIO_TONE_HIGHPASS } RadioSAudioToneKind;
typedef struct {
    gboolean enabled;
    RadioSAudioCurve volume[2], bass[2], tone[2], effect_mix;
    RadioSAudioToneKind tone_kind[2];
    gint effect; /* -1 none, otherwise RadioSAudioFxType */
    gdouble effect_depth; /* 0..1 effect parameter, like the mixer knob */
    guint effect_side; /* 0 outgoing, 1 incoming */
    gdouble effect_bpm; /* 30..300 tempo of tempo-based effects */
} RadioSAudioTransitionShape;
/* An empty curve reads 1. Between points the value is linear; equal t makes a step. */
gdouble radios_audio_curve_value(const RadioSAudioCurve *curve, gdouble t);
gboolean radios_audio_curve_valid(const RadioSAudioCurve *curve);
gboolean radios_audio_transition_shape_validate(const RadioSAudioTransitionShape *shape, GError **error);
void radios_audio_transition_shape_gains(const RadioSAudioTransitionShape *shape,
    gdouble progress, gdouble *outgoing, gdouble *incoming);
/* One side's bass, tone and effect processing. Voices are allocated off the
 * render thread and reset at each overlap start; process never allocates. */
typedef struct RadioSAudioShapeVoice RadioSAudioShapeVoice;
RadioSAudioShapeVoice *radios_audio_shape_voice_new(void);
void radios_audio_shape_voice_free(RadioSAudioShapeVoice *voice);
void radios_audio_shape_voice_reset(RadioSAudioShapeVoice *voice);
void radios_audio_shape_voice_process(RadioSAudioShapeVoice *voice,
    const RadioSAudioTransitionShape *shape, guint side, gdouble progress, gfloat stereo[2]);
G_END_DECLS
#endif
