#ifndef RADIOS_AUDIO_H
#define RADIOS_AUDIO_H

#include <gio/gio.h>
#include "radios/audio_decoder.h"
#include "radios/audio_dsp.h"
#include "radios/rhythm.h"
#include "radios/loudness.h"
#include "radios/loudness_policy.h"

G_BEGIN_DECLS

#define RADIOS_AUDIO_PREVIEW_DECK 32u
#define RADIOS_AUDIO_MAX_DEVICES 64u
#define RADIOS_AUDIO_DEVICE_NAME_SIZE 513u

typedef struct RadioSAudioEngine RadioSAudioEngine;
typedef enum { RADIOS_AUDIO_BUS_MAIN, RADIOS_AUDIO_BUS_CUE, RADIOS_AUDIO_BUS_MONITOR,
               RADIOS_AUDIO_BUS_COUNT } RadioSAudioBus;
typedef enum { RADIOS_AUDIO_EMPTY, RADIOS_AUDIO_LOADING, RADIOS_AUDIO_READY,
               RADIOS_AUDIO_PLAYING, RADIOS_AUDIO_PAUSED, RADIOS_AUDIO_EOF,
               RADIOS_AUDIO_ERROR } RadioSAudioState;
typedef enum { RADIOS_AUDIO_ERROR_INVALID, RADIOS_AUDIO_ERROR_UNAVAILABLE,
               RADIOS_AUDIO_ERROR_STATE, RADIOS_AUDIO_ERROR_CLOSED } RadioSAudioError;
#define RADIOS_AUDIO_ERROR_DOMAIN (radios_audio_error_quark())
GQuark radios_audio_error_quark(void);

typedef struct {
    const gchar *path, *sha256, *media_id, *version_id;
    guint64 cue_in_frame, cue_out_frame; /* actual 48 kHz frames; out=0 means EOF */
} RadioSAudioLoadSpec;

typedef struct {
    gchar main_sink[RADIOS_AUDIO_DEVICE_NAME_SIZE];
    gchar cue_sink[RADIOS_AUDIO_DEVICE_NAME_SIZE];
    gchar monitor_sink[RADIOS_AUDIO_DEVICE_NAME_SIZE];
    gchar microphone_source[RADIOS_AUDIO_DEVICE_NAME_SIZE];
    gdouble main_gain_db, cue_gain_db, monitor_gain_db, microphone_gain_db;
    gint cue_source, monitor_source; /* -1 is MAIN mix, 0..32 is one deck */
    /* Measured players are normalized through their mixer channel fader, unless
     * that channel bypasses normalization (-30..-5 LUFS). Dedicated preview
     * and direct audition outputs retain their own trim. See the peak limit
     * in radios_loudness_gain_db. */
    gboolean loudness_normalize;
    gdouble loudness_target_lufs;
    RadioSAudioLoudnessPolicy loudness_policy;
} RadioSAudioPreferences;

typedef struct {
    gchar name[RADIOS_AUDIO_DEVICE_NAME_SIZE], description[RADIOS_AUDIO_DEVICE_NAME_SIZE];
    guint channels;
    gboolean monitor;
} RadioSAudioDevice;

/* A validated playlist entry is registered silently before its provider opens.
 * generation is filled by bind_external_player and fences every later stream. */
typedef struct {
    guint deck;
    guint64 generation;
    gchar playlist_id[37], entry_id[37], provider[32], source_id[513];
    gchar title[256], artist[256];
    guint64 duration_frames;
} RadioSAudioPlayerBinding;

typedef struct {
    RadioSAudioState state;
    guint64 generation, cursor_frames, buffered_frames;
    guint64 cue_in_frame, cue_out_frame; /* actual loaded source span */
    gchar media_id[37], version_id[37], error[256];
    gdouble gain_db;
    gboolean main_routed, runner_paused; /* actual automation owner pause */
    gboolean main_path_ready;
    gchar main_block_reason[256]; /* physical output / fader / live solo blockage */
    gint output_bus; /* actual transport clock driver; -1 means unrouted */
    gdouble bpm, effective_bpm, tempo_ratio;
    gint sync_master;
    gdouble loudness_lufs, loudness_gain_db; /* measured file; policy/direct audition trim (actual player trim is in mixer meters) */
    gboolean loudness_measured;
    gfloat peak[2];
    gboolean external_bound;
    RadioSAudioPlayerBinding binding;
    guint64 external_received_frames;
} RadioSAudioDeckSnapshot;

typedef struct {
    gboolean ready, gate;
    guint64 consumed_frames;
    guint output_sample_rate, output_buffer_frames; /* actual ready device stream; 0 if unavailable */
    gfloat peak[2], rms[2];
    gchar device[RADIOS_AUDIO_DEVICE_NAME_SIZE], error[256];
} RadioSAudioBusSnapshot;

typedef struct {
    gboolean connected, realtime, microphone_ready, microphone_gate;
    gboolean microphone_latch, microphone_ptt; /* gate = latch (L) || push-to-talk (R) */
    guint64 loss_epoch; /* advances on physical server loss; fences old autoplay */
    gchar error[256], microphone_error[256];
    gfloat microphone_peak[2];
    RadioSAudioDeckSnapshot decks[RADIOS_AUDIO_DECKS];
    RadioSAudioBusSnapshot buses[RADIOS_AUDIO_BUS_COUNT];
    guint output_count, input_count;
    RadioSAudioDevice outputs[RADIOS_AUDIO_MAX_DEVICES], inputs[RADIOS_AUDIO_MAX_DEVICES];
    RadioSAudioMixerConfig mixer;
    RadioSAudioDspMeters dsp_meters;
    gboolean external_main_active;
    guint external_main_queued_frames;
    guint64 external_main_received_frames;
    gfloat external_main_peak[2];
    gchar external_main_error[256];
} RadioSAudioSnapshot;

/* Constructor starts background device connection only in realtime mode.
 * Default MAIN targets @default; CUE/MON/capture are unassigned, mic gate off.
 * No GTK callback: query bounded snapshots on the main/UI thread.
 * server: NULL uses $RADIOS_AUDIO_SERVER, else the Windows audio service
 * ("wasapi"); an unsupported locator leaves the engine disconnected with a
 * Turkish error. Device names are Windows endpoint ids, "<output id>.monitor"
 * (loopback of an output), "@default" or "" (unassigned). */
RadioSAudioEngine *radios_audio_engine_new(gboolean realtime, const gchar *server);
/* Operator name of the sound system ("Windows ses (WASAPI)"). */
const gchar *radios_audio_backend_display_name(void);
/* Highest sample rate a connected output accepts (exclusive-mode capability
 * or its Windows mix format); 0 when unknown. Never touches a device on the
 * calling thread: the realtime engine measures it after each device scan;
 * without one, the first call waits at most 1 s for a one-off measurement. */
guint radios_audio_backend_max_sample_rate(void);
RadioSAudioEngine *radios_audio_engine_ref(RadioSAudioEngine *engine);
void radios_audio_engine_unref(RadioSAudioEngine *engine);
void radios_audio_preferences_default(RadioSAudioPreferences *preferences);
gboolean radios_audio_engine_apply_preferences(RadioSAudioEngine *engine,
    const RadioSAudioPreferences *preferences, GError **error);
void radios_audio_engine_refresh_devices(RadioSAudioEngine *engine);
/* Playback-only format: internal PCM, microphone and recording stay 48 kHz.
 * 44100/48000 Hz and power-of-two 64..4096 frame targets are accepted. Applying
 * a changed format pauses current sources, closes the mic gate and fences old
 * autoplay. A control worker rebuilds streams; snapshots prove actual format.
 * Initial legacy output is 48000 Hz / 2400 frames until explicitly configured. */
gboolean radios_audio_engine_configure_output(RadioSAudioEngine *engine,
    guint sample_rate, guint buffer_frames, GError **error);
void radios_audio_engine_output_configuration(RadioSAudioEngine *engine,
    guint *sample_rate, guint *buffer_frames);
void radios_audio_engine_snapshot(RadioSAudioEngine *engine, RadioSAudioSnapshot *snapshot);

/* Nonblocking commands copy input. Decoder/seek work belongs to a bounded
 * per-deck worker. Replacement cancels the previous generation. Deck 32 can
 * only reach CUE/monitor and can never be routed to MAIN. */
gboolean radios_audio_engine_load(RadioSAudioEngine *engine, guint deck,
    const RadioSAudioLoadSpec *spec, GError **error);
/* Same verified local decoding/transport, with its accepted playlist entry.
 * NULL metadata creates a runtime single-entry playlist before any PCM plays.
 * All 66 local decks are supported; external_bound remains FALSE. */
gboolean radios_audio_engine_load_player(RadioSAudioEngine *engine, guint deck,
    const RadioSAudioLoadSpec *spec, const RadioSAudioPlayerBinding *metadata, GError **error);
gboolean radios_audio_engine_play(RadioSAudioEngine *engine, guint deck, GError **error);
gboolean radios_audio_engine_pause(RadioSAudioEngine *engine, guint deck, GError **error);
gboolean radios_audio_engine_stop(RadioSAudioEngine *engine, guint deck, GError **error);
/* Cancel and release a source without reseeking it. Safe before deleting a
 * private prepared preview file; no worker can publish the old generation. */
gboolean radios_audio_engine_unload(RadioSAudioEngine *engine, guint deck, GError **error);
/* A seek is a move to the chosen point, not a reload: a loaded deck keeps
 * PLAYING, PAUSED or READY (it never drops to LOADING; only a deck that is
 * still loading stays LOADING), reports the target at once and resumes PCM
 * there as soon as it is decoded. A target inside the decoded window plays
 * immediately; a later one decodes only the frames in between. The generation
 * still advances: it fences stale commands and the cursor's render history.
 * An AutoDJ item that already went on air resumes without re-checking its
 * start window. */
gboolean radios_audio_engine_seek(RadioSAudioEngine *engine, guint deck, guint64 frame, GError **error);
/* Reject stale source generations atomically. Current AutoDJ seeks preserve
 * the queue and preloaded successor; overlap/successor seeks are rejected. */
gboolean radios_audio_engine_seek_if_generation(RadioSAudioEngine *engine, guint deck,
    guint64 expected_generation, guint64 frame, GError **error);
gboolean radios_audio_engine_set_deck_gain(RadioSAudioEngine *engine, guint deck, gdouble db, GError **error);
/* Temporary, exclusive deck-32 audition route. Its local gain never changes
 * station bus gains, preferences or MAIN playback. Disabling restores the
 * station's CUE/monitor sources and gates without flushing MAIN. */
gboolean radios_audio_engine_set_preview_output(RadioSAudioEngine *engine,
    RadioSAudioBus bus, gdouble gain_db, gboolean enabled, GError **error);
gboolean radios_audio_engine_set_deck_main(RadioSAudioEngine *engine, guint deck, gboolean routed, GError **error);
gboolean radios_audio_engine_set_deck_rhythm(RadioSAudioEngine *engine, guint deck,
    guint64 generation, const RadioSRhythmAnalysis *rhythm, GError **error);
/* The loaded file's programme loudness; normalization applies at once. */
gboolean radios_audio_engine_set_deck_loudness(RadioSAudioEngine *engine, guint deck,
    guint64 generation, const RadioSLoudness *loudness, GError **error);
/* Operator SYNC can only be enabled while the source is PLAYING (and its
 * automation owner is running). Pause/Stop release manual SYNC and restore
 * native tempo; prepared AutoDJ beat transitions retain their own profiles. */
gboolean radios_audio_engine_sync(RadioSAudioEngine *engine, guint deck, gint master,
    gboolean enabled, GError **error);
gboolean radios_audio_engine_set_bus_gate(RadioSAudioEngine *engine, RadioSAudioBus bus, gboolean gate, GError **error);
gboolean radios_audio_engine_set_microphone_gate(RadioSAudioEngine *engine, gboolean gate, GError **error);
/* Momentary push-to-talk (the R key): the on-air gate is latch || ptt, and
 * set_microphone_gate is the latch. Pressing needs the microphone and the MAIN
 * device (an offline engine has neither and accepts the gate for tests);
 * releasing always succeeds. Like the latch, neither changes any deck's
 * transport. The snapshot reports gate, latch and ptt separately. */
gboolean radios_audio_engine_set_microphone_ptt(RadioSAudioEngine *engine, gboolean held, GError **error);
/* Live DSP configuration never grants a microphone gate. MASTER gain remains
 * the single existing preferences.main_gain_db used by both header and DJ. */
gboolean radios_audio_engine_apply_mixer(RadioSAudioEngine *engine, const RadioSAudioMixerConfig *config, GError **error);
/* Effective runtime configuration, including automatically controlled faders. */
void radios_audio_engine_get_mixer(RadioSAudioEngine *engine, RadioSAudioMixerConfig *config);
/* Persistence/manual-intent view: restores each channel's last manual gain,
 * while retaining current routing, bypass and other mixer controls. */
void radios_audio_engine_get_manual_mixer(RadioSAudioEngine *engine, RadioSAudioMixerConfig *config);
gboolean radios_audio_engine_set_channel_dsp(RadioSAudioEngine *engine, guint channel, const RadioSAudioChannelDsp *config, GError **error);
gboolean radios_audio_engine_set_master_dsp(RadioSAudioEngine *engine, const RadioSAudioMasterDsp *config, GError **error);
gboolean radios_audio_engine_set_fx(RadioSAudioEngine *engine, guint slot, const RadioSAudioFx *config, GError **error);
gboolean radios_audio_engine_set_master_fx(RadioSAudioEngine *engine, const RadioSAudioFx *config, GError **error);
gboolean radios_audio_engine_set_crossfader(RadioSAudioEngine *engine, gdouble position, GError **error);
gboolean radios_audio_engine_set_ducking(RadioSAudioEngine *engine, gboolean enabled, gdouble bed_db, GError **error);

/* A registered external playlist entry contributes 48 kHz stereo PCM through
 * its normal player and mixer channel, then MAIN recording/stream delivery.
 * A single owner token fences stale capture callbacks. PRESTART is silent.
 * Pushes are bounded to 4096 frames and never block the renderer. Overflow,
 * device loss or MAIN closure clears the session; a new start is explicit. */
/* PRESTART registers a silent player source and writes its new generation.
 * Neither API accepts the preview deck or an active local/runner-owned deck. */
gboolean radios_audio_engine_bind_external_player(RadioSAudioEngine *engine,
    RadioSAudioPlayerBinding *binding, GError **error);
guint64 radios_audio_engine_external_begin_player(RadioSAudioEngine *engine,
    const RadioSAudioPlayerBinding *binding, GError **error);
guint64 radios_audio_engine_external_begin_player_at(RadioSAudioEngine *engine,
    const RadioSAudioPlayerBinding *binding, guint64 start_frame, GError **error);
/* Legacy unbound entry point is retained only to report a binding error. */
guint64 radios_audio_engine_external_begin(RadioSAudioEngine *engine, GError **error);
gboolean radios_audio_engine_external_push(RadioSAudioEngine *engine, guint64 token,
    const gfloat *stereo, guint frames, GError **error);
void radios_audio_engine_external_end(RadioSAudioEngine *engine, guint64 token);

/* Live signal taps for the routing patchbay. A tap copies MAIN (as sent to
 * the MAIN device), the talk microphone after its gain and before the on-air
 * gate, or mixer channel 0..31 after its fader. The newest second is kept;
 * reads never block. Taps and lines hold an engine reference until closed. */
typedef struct RadioSAudioTap RadioSAudioTap;
typedef struct RadioSAudioLine RadioSAudioLine;
#define RADIOS_AUDIO_TAP_MAIN (-1)
#define RADIOS_AUDIO_TAP_MICROPHONE (-2)
RadioSAudioTap *radios_audio_engine_tap_open(RadioSAudioEngine *engine, gint source, GError **error);
guint radios_audio_tap_read(RadioSAudioTap *tap, gfloat *stereo, guint frames);
guint radios_audio_tap_available(RadioSAudioTap *tap);
void radios_audio_tap_skip(RadioSAudioTap *tap, guint frames);
void radios_audio_tap_close(RadioSAudioTap *tap);
/* Live audio into mixer channel 0..31 (through the channel strip: EQ, fader,
 * mute, effects) or straight into MAIN (-1). Starts after 30 ms of queued
 * audio; a producer that runs ahead is trimmed to 50 ms. */
RadioSAudioLine *radios_audio_engine_line_open(RadioSAudioEngine *engine, gint target, GError **error);
guint radios_audio_line_write(RadioSAudioLine *line, const gfloat *stereo, guint frames);
/* Frames written but not yet rendered; a producer paces itself with this. */
guint radios_audio_line_queued(RadioSAudioLine *line);
void radios_audio_line_close(RadioSAudioLine *line);
/* Lock-free read of the on-air microphone gate as last rendered or set. */
gboolean radios_audio_engine_microphone_open(RadioSAudioEngine *engine);
/* Full output flushes the realtime engine has performed (diagnostics/tests). */
guint64 radios_audio_engine_output_flushes(RadioSAudioEngine *engine);

/* Offline engine only: consume actual PCM deterministically into three caller
 * buffers, each frames*2 floats. frames is 1..4096. Never waits for decoding;
 * insufficient PCM is silence. Snapshot reports those consumed frame counts. */
gboolean radios_audio_engine_render(RadioSAudioEngine *engine, guint frames,
    gfloat *main_output, gfloat *cue_output, gfloat *monitor_output, GError **error);

G_END_DECLS
#endif
