#ifndef RADIOS_AUDIO_RUNNER_H
#define RADIOS_AUDIO_RUNNER_H

#include "radios/audio.h"
#include "radios/audio_tempo.h"
#include <math.h>

G_BEGIN_DECLS
#define RADIOS_AUDIO_RUN_MAX_ITEMS 500u
#define RADIOS_AUDIO_RUN_MAX_RECEIPTS 2000u

typedef enum {
    RADIOS_AUDIO_RUN_EOF, RADIOS_AUDIO_RUN_CROSSFADE,
    RADIOS_AUDIO_RUN_CUE, RADIOS_AUDIO_RUN_MANUAL
} RadioSAudioRunMode;
typedef enum { RADIOS_AUDIO_RUN_EFFECT_NONE, RADIOS_AUDIO_RUN_EFFECT_LOWPASS } RadioSAudioRunEffect;
typedef enum {
    RADIOS_AUDIO_RUN_START_SEQUENTIAL, RADIOS_AUDIO_RUN_START_CUT,
    RADIOS_AUDIO_RUN_START_FINISH
} RadioSAudioRunStartPolicy;
typedef enum { RADIOS_AUDIO_RUN_END_CUT, RADIOS_AUDIO_RUN_END_FINISH } RadioSAudioRunEndPolicy;
typedef enum {
    RADIOS_AUDIO_RUN_IDLE, RADIOS_AUDIO_RUN_PREROLL, RADIOS_AUDIO_RUN_SCHEDULED,
    RADIOS_AUDIO_RUN_PLAYING, RADIOS_AUDIO_RUN_OVERLAP, RADIOS_AUDIO_RUN_PAUSED,
    RADIOS_AUDIO_RUN_WAITING_MEDIA, RADIOS_AUDIO_RUN_MANUAL_WAIT,
    RADIOS_AUDIO_RUN_FINISHED, RADIOS_AUDIO_RUN_ERROR
} RadioSAudioRunState;
typedef enum {
    RADIOS_AUDIO_RUN_TERMINAL_NONE,
    RADIOS_AUDIO_RUN_TERMINAL_COMPLETE,
    RADIOS_AUDIO_RUN_TERMINAL_INTERRUPTED,
    RADIOS_AUDIO_RUN_TERMINAL_FAILED
} RadioSAudioRunTerminal;

typedef struct {
    gboolean restricted, denied;
    gint64 starts_at_utc_us, ends_at_utc_us; /* inclusive start, exclusive end; 0 unbounded */
} RadioSAudioCopyrightWindow;

/* One verified per-song voice, decoded before its output-time trigger. The
 * player is an explicit, distinct, empty MAIN-mapped auxiliary lease; it never
 * substitutes the music source or changes the primary run index. All string
 * pointers are copied by configure, just like the primary media spec. */
typedef struct {
    gboolean enabled;
    guint player; /* zero-based normal deck0..31; never preview/sampler */
    RadioSAudioLoadSpec media;
    const gchar *entry_id, *title, *artist;
    guint64 start_output_frame, fade_frames;
    gdouble gain_db, duck_db;
    RadioSAudioCopyrightWindow copyright;
    RadioSLoudness loudness;
} RadioSAudioRunAdjunct;

typedef struct {
    const gchar *entry_id; /* stable UUID; inputs are copied by configure */
    RadioSAudioLoadSpec media; /* SHA and catalog UUIDs mandatory */
    guint64 duration_frames, auto_next_frame, fade_start_frame, fade_end_frame;
    guint64 autodj_out_frame; /* optional outgoing trigger; physical cue-out remains unchanged */
    guint64 fade_in_frames, overlap_frames;
    gdouble duck_db;
    RadioSAudioRunMode mode;
    gint64 start_utc_us; /* 0 means sequential; wall anchors never drive envelopes */
    gint64 latest_start_utc_us; /* 0 unbounded; expired sources never start */
    RadioSAudioRunStartPolicy start_policy;
    guint64 max_wait_frames;
    gint preferred_deck; /* -1 pool default, otherwise a member of deck_pool */
    RadioSRhythmAnalysis rhythm;
    RadioSLoudness loudness; /* unmeasured: plays at its own level */
    gdouble tempo_ratio;
    guint64 tempo_return_start_frames, tempo_return_frames; /* output-frame hold and native-tempo return */
    gboolean beat_sync;
    gboolean estimated_duration; /* duration_ms fallback: tolerate <=1 ms EOF rounding */
    RadioSAudioRunEffect effect; /* outgoing overlap only; no global mixer FX */
    RadioSAudioTransitionShape shape; /* designed overlap curves/effect; disabled keeps envelope + effect */
    RadioSAudioCopyrightWindow copyright;
    RadioSAudioRunAdjunct adjunct; /* optional midtrack jingle; zeroed means off */
} RadioSAudioRunItem;
static inline RadioSAudioTempoProfile radios_audio_run_tempo_profile(const RadioSAudioRunItem *item)
{
    return (RadioSAudioTempoProfile){item->tempo_ratio > 0 ? item->tempo_ratio : 1,
        item->tempo_return_start_frames, item->tempo_return_frames};
}
/* Planner and renderer use the same source-frame boundary. A valid explicit
 * IN/OUT span must not become empty when its exit is snapped to an earlier beat. */
static inline guint64 radios_audio_run_trigger_frame(const RadioSAudioRunItem *item,
    const RadioSRhythmAnalysis *rhythm, gdouble ratio, guint64 out)
{
    guint64 trigger = out;
    if (item->mode == RADIOS_AUDIO_RUN_CROSSFADE) {
        if (item->tempo_return_frames) {
            RadioSAudioTempoProfile profile = radios_audio_run_tempo_profile(item);
            gdouble end = radios_audio_tempo_profile_output(&profile, out - item->media.cue_in_frame);
            trigger = item->media.cue_in_frame + (guint64)ceil(radios_audio_tempo_profile_source(&profile, MAX(0.0, end - item->overlap_frames)));
        } else trigger -= MIN((guint64)ceil(item->overlap_frames * ratio), out - item->media.cue_in_frame);
    }
    else if (item->mode == RADIOS_AUDIO_RUN_CUE) trigger = item->auto_next_frame;
    if (item->autodj_out_frame && item->mode != RADIOS_AUDIO_RUN_MANUAL) trigger = MIN(out, item->autodj_out_frame);
    if (item->beat_sync) {
        guint64 aligned = radios_rhythm_transition_frame(rhythm, trigger, item->media.cue_in_frame, out);
        if (!item->autodj_out_frame || aligned > item->media.cue_in_frame) trigger = aligned;
    }
    return trigger;
}
/* A two-source fade must finish before the incoming source reaches its own
 * explicit AutoDJ exit; otherwise that exit is crossed while it is still next. */
static inline guint64 radios_audio_run_incoming_span_frames(const RadioSAudioRunItem *item)
{
    if (!item->autodj_out_frame || item->mode == RADIOS_AUDIO_RUN_MANUAL) return G_MAXUINT64;
    gdouble ratio = item->tempo_ratio > 0 ? item->tempo_ratio : 1;
    guint64 out = item->media.cue_out_frame ? item->media.cue_out_frame : item->duration_frames;
    guint64 trigger = radios_audio_run_trigger_frame(item, &item->rhythm, ratio, out);
    RadioSAudioTempoProfile profile = radios_audio_run_tempo_profile(item);
    return (guint64)ceil(radios_audio_tempo_profile_output(&profile, trigger - item->media.cue_in_frame));
}
/* A stored AutoDJ exit starts the selected policy envelope. Beat alignment
 * may move that start; the physical tail must not lengthen an explicit fade. */
static inline guint64 radios_audio_run_overlap_frames(const RadioSAudioRunItem *item, guint64 remaining)
{
    return item->mode == RADIOS_AUDIO_RUN_EOF ? 0 :
        item->beat_sync && !item->autodj_out_frame ? remaining : MIN(item->overlap_frames, remaining);
}
typedef struct {
    guint deck_pool[RADIOS_AUDIO_NORMAL_DECKS], pool_count;
    gint64 start_utc_us, end_utc_us; /* 0 means immediate / no global deadline */
    RadioSAudioRunEndPolicy end_policy;
    guint64 finish_grace_frames;
    /* Immutable accepted execution provenance, never inferred from UI choices.
     * Zero context from legacy aggregate initializers means manual. */
    guint loudness_context;
    gchar loudness_automation_id[37];
    gboolean loudness_assigned;
    /* Optional voices may be omitted when their explicit auxiliary player is
     * unavailable. The primary queue and all occupied/CUE/foreign decks stay
     * intact. FALSE preserves strict all-player acquisition for API callers. */
    gboolean skip_unavailable_adjuncts;
} RadioSAudioRunOptions;
typedef struct {
    RadioSAudioRunState state;
    guint64 epoch;
    guint count, index, next_index;
    gint deck_current, deck_next; /* -1 when absent */
    guint64 actual_current_frame, actual_next_frame, transition_frames;
    guint64 transition_total_frames; /* actual output envelope, including late-start caps */
    gchar entry_id[37], next_entry_id[37], error[256];
    gboolean owned[RADIOS_AUDIO_NORMAL_DECKS];
    gboolean handover_pending; /* armed, including physical drain; manual navigation cancels */
    gboolean handover_completed; /* successful armed drain; cancellation never sets this */
} RadioSAudioRunSnapshot;
typedef struct {
    guint64 epoch;
    guint64 occurrence; /* each actually consumed load/replay is independent */
    guint index;
    gint64 first_utc_us; /* first confirmed MAIN consumption observation */
    guint64 position_frames; /* latest actually consumed absolute source frame */
    RadioSAudioRunTerminal terminal;
    gint64 last_observed_utc_us; /* observation time, never acoustic/on-air proof */
} RadioSAudioRunReceipt;

void radios_audio_run_options_default(RadioSAudioRunOptions *options);
/* Fully verified ordered queue, max500. Start leases only EMPTY, MAIN-mapped
 * normal decks. Preview32 and sampler33..65 are never selected. No GTK callback
 * and no file I/O on the render thread; existing workers verify/read sources.
 * Manual transport/load (except seeking its current nonoverlapping source)
 * or removing a leased deck's MAIN mapping cancels the
 * run before the manual command. Live mixer EQ/gains/FX remain independent.
 * A one-deck pool supports EOF/manual/CUE without overlap; recycling requires
 * actual output drain and reports WAITING_MEDIA rather than claiming gapless. */
gboolean radios_audio_engine_runner_configure(RadioSAudioEngine *engine,
    const RadioSAudioRunItem *items, guint count, const RadioSAudioRunOptions *options, GError **error);
/* Append before rendered FINISHED. The existing queue prefix, epoch, source
 * ownership and sample envelopes remain unchanged; total queue stays <=500. */
gboolean radios_audio_engine_runner_append(RadioSAudioEngine *engine,
    const RadioSAudioRunItem *items, guint count, GError **error);
gboolean radios_audio_engine_runner_append_source(RadioSAudioEngine *engine, gint source,
    const RadioSAudioRunItem *items, guint count, GError **error);
/* Atomically replace only the unheard future. Epoch, current media/cursor,
 * PCM generation, receipts and the active adjunct remain intact. BUSY means
 * an overlap/drain must finish first; failed validation changes no queue. */
gboolean radios_audio_engine_runner_replace_future(RadioSAudioEngine *engine, gint source,
    guint64 epoch, guint64 generation, const gchar *entry_id,
    const RadioSAudioRunItem *items, guint count, GError **error);
/* Remove/reorder unheard entries (kullanıcı isteği U7). Same fences as
 * replace_future, but the sounding item keeps its own exit policy (mode,
 * overlap, effect, shape); only an overlap the new successor makes
 * physically impossible is capped or turned into a plain cut. count 0 ends
 * the queue after the sounding item (items may then be NULL). A loaded
 * successor that stays the same entry in the same place stays loaded and no
 * output flush happens; preloaded pool players of other entries are released. */
gboolean radios_audio_engine_runner_remove_future(RadioSAudioEngine *engine, gint source,
    guint64 epoch, guint64 generation, const gchar *entry_id,
    const RadioSAudioRunItem *items, guint count, GError **error);
gboolean radios_audio_engine_runner_start(RadioSAudioEngine *engine, GError **error);
gboolean radios_audio_engine_runner_start_adopting(RadioSAudioEngine *engine, guint deck, guint64 generation, GError **error);
/* Explicit AutoDJ takeover: validate first, then replace nonplaying pool
 * sources atomically. adopt=-1 loads the first item; otherwise retain it. */
gboolean radios_audio_engine_runner_start_replacing(RadioSAudioEngine *engine, gint adopt, guint64 generation, GError **error);
gboolean radios_audio_engine_runner_pause(RadioSAudioEngine *engine, GError **error);
gboolean radios_audio_engine_runner_resume(RadioSAudioEngine *engine, GError **error);
gboolean radios_audio_engine_runner_stop(RadioSAudioEngine *engine, GError **error);
gboolean radios_audio_engine_runner_next(RadioSAudioEngine *engine, GError **error);
gboolean radios_audio_engine_runner_previous(RadioSAudioEngine *engine, GError **error);
/* Arm a generation-fenced handover of this owned queue. Existing PCM continues
 * until target UTC. FINISH suppresses a successor once the target is reached;
 * zero max_wait is unlimited. CUT, or an expired FINISH wait, fades for exactly
 * fade_frames (0..48000) rendered samples, then drains actual MAIN consumption.
 * UTC gates rendering; device buffering may delay physical output. FINISHED
 * with no owned decks and handover_completed is the safe handover point. Manual NEXT,
 * PREVIOUS, Stop or deck takeover cancels the gate; unrelated manual decks are
 * untouched. Configuration and cancellation do no I/O and allocate no PCM. */
gboolean radios_audio_engine_runner_handover(RadioSAudioEngine *engine,
    guint64 expected_epoch, gint64 target_utc_us, RadioSAudioRunStartPolicy policy,
    guint64 max_wait_frames, guint fade_frames, GError **error);
void radios_audio_engine_runner_handover_cancel(RadioSAudioEngine *engine,
    guint64 expected_epoch);
void radios_audio_engine_runner_snapshot(RadioSAudioEngine *engine, RadioSAudioRunSnapshot *snapshot);
/* Read-only lease lookup, including silent reserved auxiliary players. source
 * is -1 for the global runner or 0..31 for a player/scope runner. FALSE means
 * no runner lease (or invalid arguments); source is then left untouched. */
gboolean radios_audio_engine_deck_runner_owner(RadioSAudioEngine *engine,
    guint deck, gint *source);
/* Cumulative per-queue records survive source retirement. A source is absent
 * until MAIN has actually consumed its first PCM tag, including short items
 * completed between GUI polls. PREVIOUS/reloads have distinct occurrences in
 * the same epoch. Copies newest capacity records, oldest first (max2000). */
guint radios_audio_engine_runner_receipts(RadioSAudioEngine *engine,
    RadioSAudioRunReceipt *out, guint capacity);
typedef enum {
    RADIOS_AUDIO_PLAYER_RUN_START, RADIOS_AUDIO_PLAYER_RUN_PAUSE,
    RADIOS_AUDIO_PLAYER_RUN_RESUME, RADIOS_AUDIO_PLAYER_RUN_STOP,
    RADIOS_AUDIO_PLAYER_RUN_NEXT, RADIOS_AUDIO_PLAYER_RUN_PREVIOUS
} RadioSAudioPlayerRunCommand;
gboolean radios_audio_engine_player_runner_configure(RadioSAudioEngine *engine, guint deck,
    const RadioSAudioRunItem *items, guint count, const RadioSAudioRunOptions *options, GError **error);
/* Private scope queue, keyed by source player. The explicit pool must contain
 * source and distinct normal decks; item preferred_deck and overlap settings
 * remain intact. Disjoint scopes run concurrently without a visible widget.
 * Existing single-player configure continues to enforce a one-deck pool. */
gboolean radios_audio_engine_scope_runner_configure(RadioSAudioEngine *engine, guint source,
    const RadioSAudioRunItem *items, guint count, const RadioSAudioRunOptions *options, GError **error);
gboolean radios_audio_engine_scope_runner_start_replacing(RadioSAudioEngine *engine, guint source,
    gint adopt, guint64 generation, GError **error);
gboolean radios_audio_engine_player_runner_action(RadioSAudioEngine *engine, guint deck,
    RadioSAudioPlayerRunCommand command, GError **error);
void radios_audio_engine_player_runner_snapshot(RadioSAudioEngine *engine, guint deck, RadioSAudioRunSnapshot *snapshot);
guint radios_audio_engine_player_runner_receipts(RadioSAudioEngine *engine, guint deck,
    RadioSAudioRunReceipt *out, guint capacity);
/* Update only copyright bounds of an existing queue, without replacing its
 * order, epoch, history or deck ownership. source=-1 is the station runner. */
gboolean radios_audio_engine_runner_copyright(RadioSAudioEngine *engine, gint source,
    const RadioSAudioCopyrightWindow *windows, guint count, GError **error);
/* Same immutable primary occurrence count; updates only its separate jingle
 * windows. Music order/ownership/epoch and primary copyright stay unchanged. */
gboolean radios_audio_engine_runner_adjunct_copyright(RadioSAudioEngine *engine, gint source,
    const RadioSAudioCopyrightWindow *windows, guint count, GError **error);
G_END_DECLS
#endif
