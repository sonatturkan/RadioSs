#ifndef RADIOS_TRANSITION_PRESET_H
#define RADIOS_TRANSITION_PRESET_H

#include "radios/audio_dsp.h"
#include <json-glib/json-glib.h>

G_BEGIN_DECLS
/* Designed AutoDJ transition presets, stored in a rotation's settings as
 * "autodj_presets" (array) and assigned by "autodj_automation":
 *   preset: {id, name, duration_ms 500..30000, beat_sync?,
 *            out: {volume, bass, tone, tone_kind}, in: {...},
 *            effect: {type, depth 0..100, side out|in, mix, bpm? 30..300}}
 *   curve: [[t, v], ...] with 2..8 points, t non-decreasing from 0 to 1, v 0..1
 *   automation: {enabled, kinds: {music|advertisement|jingle|sweeper:
 *                {presets: [ids], rotation: sequential|random}},
 *                schedule?: [{id, name, days: [1..7], start_minute, end_minute,
 *                             kind: all|music|advertisement|jingle|sweeper,
 *                             presets: [ids], rotation}]}
 * A schedule window with start >= end crosses midnight. The same contract is
 * enforced by the data service (desktop_automation_calendar.py). */
#define RADIOS_TRANSITION_PRESET_LIMIT 32u
#define RADIOS_TRANSITION_SCHEDULE_LIMIT 32u
#define RADIOS_TRANSITION_KIND_COUNT 4u
extern const gchar *const radios_transition_kind_ids[RADIOS_TRANSITION_KIND_COUNT];
extern const gchar *const radios_transition_kind_labels[RADIOS_TRANSITION_KIND_COUNT];

gboolean radios_transition_preset_valid(JsonObject *preset, gboolean require_id);
gboolean radios_transition_presets_valid(JsonArray *presets);
gboolean radios_transition_automation_valid(JsonObject *automation, JsonArray *presets);
/* FALSE leaves shape disabled. Succeeds exactly when the preset is valid. */
gboolean radios_transition_preset_to_shape(JsonObject *preset, RadioSAudioTransitionShape *shape);
/* Owned deep copies of the built-in templates (fixed ids, Turkish names). */
JsonArray *radios_transition_preset_templates(void);
JsonObject *radios_transition_preset_template(const gchar *name);
/* Effect ids are lowercase ("echo", "slip_roll", ...); "none" is -1. */
const gchar *radios_transition_effect_id(gint type);
gint radios_transition_effect_type(const gchar *id);
gint radios_transition_kind_index(const gchar *kind);
/* Borrowed preset for one boundary, or NULL when the automation is off or
 * leaves this kind without presets. weekday is ISO 1..7 (0 unknown) and
 * minute the local calendar minute (-1 unknown); unknown times skip the
 * schedule. Sequential rotation advances *counter; random rotation uses draw. */
JsonObject *radios_transition_preset_choose(JsonObject *automation, JsonArray *presets,
    const gchar *kind, gint weekday, gint minute, guint *counter, guint32 draw);

/* The transition type of a preset is its structure, never a stored member:
 * which processing stages the design uses on top of the volume and bass
 * curves every preset has. Every preset has exactly one type (effect and
 * filter presence), so the editor shows a type's fields and nothing else, and
 * the wire contract above stays the one the data service and older binaries
 * accept. "Crossfade" names the engine family every designed overlap renders
 * on (planner mode 1); SYNC beatmatch is the preset's beat_sync flag. */
typedef enum {
    RADIOS_TRANSITION_TYPE_CROSSFADE, RADIOS_TRANSITION_TYPE_EFFECT,
    RADIOS_TRANSITION_TYPE_FILTER, RADIOS_TRANSITION_TYPE_EFFECT_FILTER, RADIOS_TRANSITION_TYPE_COUNT
} RadioSTransitionType;
extern const gchar *const radios_transition_type_ids[RADIOS_TRANSITION_TYPE_COUNT];
extern const gchar *const radios_transition_type_labels[RADIOS_TRANSITION_TYPE_COUNT];
RadioSTransitionType radios_transition_preset_type(JsonObject *preset);
gint radios_transition_type_index(const gchar *id);
/* Normalises a preset to the type. Losing the effect sets its type to "none"
 * (depth, side, tempo and mix stay stored; the engine never reads them while
 * the type is "none"); losing the filter sets both tone kinds to "none".
 * Gaining an effect starts from an audible echo at depth 50; gaining a filter
 * closes a low-pass over the outgoing side. TRUE when a member changed. */
gboolean radios_transition_preset_set_type(JsonObject *preset, RadioSTransitionType type);
G_END_DECLS
#endif
