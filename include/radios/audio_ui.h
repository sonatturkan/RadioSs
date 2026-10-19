#ifndef RADIOS_AUDIO_UI_H
#define RADIOS_AUDIO_UI_H
#include "radios/data.h"
#include "radios/audio_dsp.h"
#include "radios/audio_channel_input.h"
#include "radios/loudness_policy.h"

typedef struct RadioSAudioController RadioSAudioController;
typedef struct RadioSAudioEngine RadioSAudioEngine;
typedef enum {
    RADIOS_AUDIO_UI_MAIN, RADIOS_AUDIO_UI_CUE, RADIOS_AUDIO_UI_MONITOR,
    RADIOS_AUDIO_UI_MICROPHONE
} RadioSAudioUiBus;
typedef enum {
    RADIOS_AUDIO_UI_EMPTY, RADIOS_AUDIO_UI_LOADING, RADIOS_AUDIO_UI_READY,
    RADIOS_AUDIO_UI_PLAYING, RADIOS_AUDIO_UI_PAUSED, RADIOS_AUDIO_UI_EOF,
    RADIOS_AUDIO_UI_ERROR
} RadioSAudioUiState;
typedef struct {
    RadioSAudioUiState state;
    guint64 generation, position_ms, duration_ms;
    guint64 cue_in_ms, cue_out_ms;
    gdouble peak[2];
    gboolean main_enabled, output_ready, runner_paused;
    gboolean main_path_ready;
    gchar main_block_reason[256];
    gchar media_id[37], version_id[37], error[256];
    gchar title[801], artist[801], album[801];
    gdouble bpm, effective_bpm, tempo_ratio;
    gint sync_master;
    gdouble loudness_lufs, loudness_gain_db;
    gboolean loudness_measured;
    gchar kind[41]; /* loaded catalogue classification, fenced by generation */
} RadioSAudioUiSnapshot;
typedef struct { gint sync_master, autodj_target; } RadioSAudioPlayerOptions;

/* Controller is created before views and started after data initialization.
 * GTK entry points publish bounded commands; decoding/device I/O run off GTK. */
RadioSAudioController *radios_audio_ui_new(RadioSViewContext *ctx);
RadioSAudioController *radios_audio_ui_ref(RadioSAudioController *audio);
/* Acquired engine reference for the C playout controller. */
RadioSAudioEngine *radios_audio_ui_engine_acquire(RadioSViewContext *ctx);
void radios_audio_ui_runner_duration(RadioSViewContext *ctx, guint deck, guint64 duration_ms);
void radios_audio_ui_start(RadioSViewContext *ctx);
/* Flush the latest debounced mixer/preferences edits into recovery before
 * radios_data_close or destruction of the owning window. */
void radios_audio_ui_prepare_close(RadioSAudioController *audio);
void radios_audio_ui_close(RadioSAudioController *audio);
void radios_audio_ui_unref(RadioSAudioController *audio);
gboolean radios_audio_ui_get_snapshot(RadioSViewContext *ctx, guint deck, RadioSAudioUiSnapshot *snapshot);
/* Decks 0..count-1 from a single engine snapshot; periodic scans over many
 * decks use this instead of one engine lock per deck. */
gboolean radios_audio_ui_get_snapshots(RadioSViewContext *ctx, RadioSAudioUiSnapshot *snapshots, guint count);
gboolean radios_audio_ui_ready(RadioSViewContext *ctx, RadioSAudioUiBus bus);
/* Capture before an async media/preview lookup and compare before execution. */
guint64 radios_audio_ui_loss_epoch(RadioSViewContext *ctx);
gboolean radios_audio_ui_load_media(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *detail, guint deck, gboolean autoplay, GError **error);
gboolean radios_audio_ui_play(RadioSViewContext *ctx, guint deck, GError **error);
gboolean radios_audio_ui_pause(RadioSViewContext *ctx, guint deck, GError **error);
gboolean radios_audio_ui_stop(RadioSViewContext *ctx, guint deck, GError **error);
gboolean radios_audio_ui_seek(RadioSViewContext *ctx, guint deck, guint64 milliseconds, GError **error);
gboolean radios_audio_ui_seek_if_generation(RadioSViewContext *ctx, guint deck,
    guint64 expected_generation, guint64 milliseconds, GError **error);
gboolean radios_audio_ui_set_deck_gain(RadioSViewContext *ctx, guint deck, gdouble db, GError **error);
gboolean radios_audio_ui_set_deck_main(RadioSViewContext *ctx, guint deck, gboolean enabled, GError **error);
gboolean radios_audio_ui_sync(RadioSViewContext *ctx, guint deck, gint master, gboolean enabled, GError **error);
gboolean radios_audio_ui_player_options(RadioSViewContext *ctx, guint deck, RadioSAudioPlayerOptions *options);
gboolean radios_audio_ui_set_player_options(RadioSViewContext *ctx, guint deck, const RadioSAudioPlayerOptions *options, GError **error);
gboolean radios_audio_ui_preview(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *detail, guint64 start_ms, guint64 end_ms, GError **error);
void radios_audio_ui_preview_stop(RadioSViewContext *ctx, GtkWidget *owner);
gboolean radios_audio_ui_preview_owned(RadioSViewContext *ctx, GtkWidget *owner);
/* Audition controls belong to their editor owner, survive Stop/reload and are
 * released when that widget is destroyed. Only CUE or MONITOR is accepted;
 * gain applies to preview PCM without changing the station bus faders. */
gboolean radios_audio_ui_preview_set_output(RadioSViewContext *ctx, GtkWidget *owner,
    RadioSAudioUiBus bus, gdouble gain_db, GError **error);
gboolean radios_audio_ui_preview_get_output(RadioSViewContext *ctx, GtkWidget *owner,
    RadioSAudioUiBus *bus, gdouble *gain_db);
/* Owner-scoped editor transport. Pause preserves position; Cue returns to its
 * marker paused; waveform seek preserves playback and never affects another owner. */
gboolean radios_audio_ui_preview_play(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *detail, guint64 position_ms, GError **error);
/* Play/resume an editable, nondestructive cue interval. A changed interval
 * loads a new preview generation; Pause/seek retain the accepted interval. */
gboolean radios_audio_ui_preview_play_range(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *detail, guint64 position_ms, guint64 start_ms, guint64 end_ms, GError **error);
gboolean radios_audio_ui_preview_pause(RadioSViewContext *ctx, GtkWidget *owner, GError **error);
gboolean radios_audio_ui_preview_cue(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *detail, guint64 cue_ms, GError **error);
gboolean radios_audio_ui_preview_seek(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *detail, guint64 position_ms, GError **error);

gboolean radios_audio_ui_get_mixer(RadioSViewContext *ctx, RadioSAudioMixerConfig *config,
    RadioSAudioDspMeters *meters, gdouble *master_gain_db);
/* Persistent controls wait for the first database baseline. Once ready,
 * temporary connection loss still permits edits and durable offline staging. */
gboolean radios_audio_ui_mixer_ready(RadioSViewContext *ctx);
gboolean radios_audio_ui_set_mixer(RadioSViewContext *ctx, const RadioSAudioMixerConfig *config, GError **error);
gboolean radios_audio_ui_set_master_gain(RadioSViewContext *ctx, gdouble db, GError **error);
/* Programme loudness normalization of every measured deck (saved with the
 * station's audio preferences). FALSE without an audio controller. */
gboolean radios_audio_ui_loudness(RadioSViewContext *ctx, gboolean *enabled, gdouble *target_lufs);
gboolean radios_audio_ui_set_loudness(RadioSViewContext *ctx, gboolean enabled, gdouble target_lufs, GError **error);
/* Scope is independent from the global enable/target controls. Reading never
 * changes transport; valid edits use the engine's smooth gain update and CAS. */
gboolean radios_audio_ui_loudness_policy(RadioSViewContext *ctx, RadioSAudioLoudnessPolicy *policy);
gboolean radios_audio_ui_loudness_policy_ready(RadioSViewContext *ctx);
gboolean radios_audio_ui_set_loudness_policy(RadioSViewContext *ctx,
    const RadioSAudioLoudnessPolicy *policy, GError **error);
gboolean radios_audio_ui_mixer_save(RadioSViewContext *ctx);
void radios_audio_ui_mixer_reload(RadioSViewContext *ctx);
gboolean radios_audio_ui_microphone(RadioSViewContext *ctx, gboolean enabled, GError **error);
gboolean radios_audio_ui_microphone_active(RadioSViewContext *ctx);
/* Live event controls. radios_audio_ui_microphone is the latch (L on a DJ or
 * talk step); the push-to-talk (R) is momentary and the on-air gate is
 * latch || ptt. Releasing never fails and never touches any deck's transport.
 * A mixer channel 1..32 goes on air by clearing its mute (fader and routing
 * untouched); the change is applied to the engine and saved like any other
 * mixer edit. The read-back reports the engine's current mute state. */
gboolean radios_audio_ui_microphone_ptt(RadioSViewContext *ctx, gboolean held, GError **error);
gboolean radios_audio_ui_set_channel_on_air(RadioSViewContext *ctx, guint channel, gboolean on_air, GError **error);
gboolean radios_audio_ui_channel_on_air(RadioSViewContext *ctx, guint channel);
/* Channel labels (R5.2) and live inputs (R5.1) are saved beside the DSP mixer
 * in the dj_mixer document: channels[i].label is "dj", "talk" or "music"
 * (absent when none); channels[i].input is {"kind": "capture"|"network",
 * "device", "name"} and then source_deck stays -1; network_sources lists the
 * saved {name, url} pairs offered by every channel's source menu. Channels are
 * 1..32. Edits apply at once and autosave like other mixer edits; a loaded
 * document replaces the model, restarts changed inputs and never re-saves.
 * A live input runs while the controller is open and reports its status. */
const gchar *radios_audio_ui_get_channel_label(RadioSViewContext *ctx, guint channel);
gboolean radios_audio_ui_set_channel_label(RadioSViewContext *ctx, guint channel, const gchar *label, GError **error);
gboolean radios_audio_ui_get_channel_input(RadioSViewContext *ctx, guint channel, RadioSAudioChannelInputSpec *spec);
gboolean radios_audio_ui_set_channel_input(RadioSViewContext *ctx, guint channel, const RadioSAudioChannelInputSpec *spec, GError **error);
gboolean radios_audio_ui_channel_input_status(RadioSViewContext *ctx, guint channel, RadioSAudioChannelInputStatus *status);
/* Sound-card capture sources from the engine's device list; monitors last. */
guint radios_audio_ui_capture_devices(RadioSViewContext *ctx, RadioSAudioDevice *devices, guint capacity);
typedef struct { gchar name[RADIOS_CHANNEL_INPUT_NAME_SIZE]; gchar url[RADIOS_CHANNEL_INPUT_DEVICE_SIZE]; } RadioSAudioUiNetworkSource;
#define RADIOS_AUDIO_UI_NETWORK_SOURCES 32u
guint radios_audio_ui_network_sources(RadioSViewContext *ctx, RadioSAudioUiNetworkSource *sources, guint capacity);
gboolean radios_audio_ui_add_network_source(RadioSViewContext *ctx, const gchar *name, const gchar *url, GError **error);
gboolean radios_audio_ui_remove_network_source(RadioSViewContext *ctx, const gchar *url, GError **error);
/* Changes whenever the capture device list or the saved network sources change. */
guint radios_audio_ui_sources_hash(RadioSViewContext *ctx);
gboolean radios_audio_ui_cue_mix(RadioSViewContext *ctx, gboolean enabled, GError **error);
/* Releasing a removed PFL strip cannot stop another owner's exclusive preview. */
void radios_audio_ui_cue_mix_release(RadioSViewContext *ctx);
/* Start a cancellable worker audition of a real bound media pair. Values are
 * A cue/out, B cue, B fade-in, overlap in ms; ducking applies to A during B. */
gboolean radios_audio_ui_transition_preview(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *detail_a, JsonObject *detail_b, guint64 cue_a_ms, guint64 out_a_ms,
    guint64 cue_b_ms, guint64 fade_in_ms, guint64 overlap_ms,
    gdouble duck_db, gdouble level_db, GError **error);

void radios_audio_ui_bind_meter(RadioSViewContext *ctx, GtkWidget *meter, RadioSAudioUiBus bus);
void radios_audio_ui_bind_gain(RadioSViewContext *ctx, GtkWidget *knob, RadioSAudioUiBus bus);
void radios_audio_ui_bind_monitor(RadioSViewContext *ctx, GtkWidget *module,
    GtkWidget *source, GtkWidget *destination, GtkWidget *toggle, RadioSAudioUiBus bus);
void radios_audio_ui_bind_settings(RadioSViewContext *ctx, GtkWidget *page);
void radios_meter_set_levels(GtkWidget *meter, gdouble left_peak, gdouble right_peak,
    gdouble left_rms, gdouble right_rms, gboolean clipped);

#endif
