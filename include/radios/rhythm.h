#ifndef RADIOS_RHYTHM_H
#define RADIOS_RHYTHM_H
#include <glib.h>
#include <json-glib/json-glib.h>

typedef struct RadioSRhythmAnalyzer RadioSRhythmAnalyzer;
typedef struct {
    gdouble bpm, first_beat_ms, confidence;
    guint analyzed_ms;
    /* Optional bar phase: a beat time that starts a four-beat bar. It is valid
     * only when has_downbeat is set; zero-initialised grids keep no bar phase. */
    gdouble downbeat_ms;
    gboolean has_downbeat;
    /* An operator-edited grid. Automatic analysis never replaces it. */
    gboolean manual;
} RadioSRhythmAnalysis;
RadioSRhythmAnalyzer *radios_rhythm_analyzer_new(guint sample_rate);
/* Full-band energy only (mean square across channels) for one sample frame. */
void radios_rhythm_analyzer_push(RadioSRhythmAnalyzer *analyzer, gdouble energy);
/* One interleaved sample frame; also feeds the kick-band envelope. */
void radios_rhythm_analyzer_push_frame(RadioSRhythmAnalyzer *analyzer, const gfloat *samples, guint channels);
RadioSRhythmAnalysis radios_rhythm_analyzer_finish(RadioSRhythmAnalyzer *analyzer);
void radios_rhythm_analyzer_free(RadioSRhythmAnalyzer *analyzer);
JsonObject *radios_rhythm_analysis_json(const RadioSRhythmAnalysis *analysis);
gboolean radios_rhythm_analysis_read(JsonObject *object, RadioSRhythmAnalysis *analysis);
/* Algorithm names a stored analysis may carry. Grids from older analyzers are
 * readable, but a player and the library upkeep re-analyse them automatically.
 * v3 looks at several windows of up to six minutes and also folds the
 * full-band attacks, so sparse hip-hop beats get a dependable tempo. */
#define RADIOS_RHYTHM_ALGORITHM_LEGACY "radios-onset-grid-v1"
#define RADIOS_RHYTHM_ALGORITHM_V2 "radios-onset-grid-v2"
#define RADIOS_RHYTHM_ALGORITHM "radios-onset-grid-v3"
#define RADIOS_RHYTHM_ALGORITHM_MANUAL "radios-manual-grid-v1"
/* TRUE when a stored rhythm object is current: the present analyzer or a
 * manual grid. Unknown-tempo results of the present analyzer are current too. */
gboolean radios_rhythm_analysis_current(JsonObject *object);
gboolean radios_rhythm_tempo_ratio(gdouble source_bpm, gdouble target_bpm, gdouble *ratio);
/* Beat-synced transitions in 48 kHz source frames. An outgoing item leaves on
 * the bar start nearest its marked trigger when the grid knows bar starts,
 * otherwise on the beat at or before it; without a usable grid the trigger is
 * returned unchanged. The result stays inside [cue_in, cue_out). */
guint64 radios_rhythm_transition_frame(const RadioSRhythmAnalysis *rhythm, guint64 trigger, guint64 cue_in, guint64 cue_out);
/* Where an incoming item enters: the first bar start at its cue-in (a grid line
 * up to 20 ms early still counts), or the first beat at or after it when no bar
 * start fits. FALSE when no beat lies inside the cue span. */
gboolean radios_rhythm_entry_frame(const RadioSRhythmAnalysis *rhythm, guint64 cue_in, guint64 cue_out, guint64 *frame);
/* How far a synced deck's grid lies ahead of where its master's grid puts it,
 * in deck beats within [0, unit): 0 means their beat (unit 1) or bar start
 * (unit 4) lines coincide. Frames are 48 kHz source positions; ratios are each
 * deck's playback tempo. A waiting deck starts once this drops below one
 * output frame's worth of beats. */
gdouble radios_rhythm_phase_offset(const RadioSRhythmAnalysis *deck, gdouble deck_ratio, gdouble deck_frame,
    const RadioSRhythmAnalysis *master, gdouble master_ratio, gdouble master_frame, guint unit);
#endif
