#ifndef RADIOS_PLAYOUT_H
#define RADIOS_PLAYOUT_H
#include "radios/audio_runner.h"
#include <json-glib/json-glib.h>

typedef struct RadioSPlayoutHistory RadioSPlayoutHistory;
typedef struct {
    RadioSAudioRunItem run;
    gchar *title, *artist, *album, *kind, *source_id, *source_name;
    gint64 planned_utc_us;
    gboolean jingle, explicit_transition;
    gboolean verified_analysis; /* hash and measured geometry bound to this version */
    guint routing_decks[32], routing_count; /* validated per-entry, zero-based */
    JsonObject *binding;
    JsonObject *rotation_after; /* owned checkpoint; commit only after actual MAIN consumption */
    JsonObject *calendar_constraints; /* owned authenticated timing/repeat bounds */
    gint64 hold_us; /* planned wait after the previous entry ends (live step with a duration rule);
                     * the transport waits in MANUAL_WAIT and the controller continues */
    JsonObject *verified_markers; /* owned, retained for a later source-boundary cue transition */
    JsonObject *copyright_metadata; /* bound catalogue identity, separate from programme display edits */
} RadioSPlayoutEntry;
typedef struct {
    GPtrArray *entries; /* owns RadioSPlayoutEntry, including all run strings */
    RadioSAudioRunOptions options;
    JsonNode *display; /* immutable preview, never a playback receipt */
    JsonObject *programme_config; /* owned accepted programme identity/flow metadata */
    guint music_count;
    gboolean automation;
    gboolean player_autodj;
    gboolean routed_rotation;
    JsonObject *copyright_policy;
} RadioSPlayoutPlan;
typedef struct {
    const gchar *station_id, *database_file;
    guint mode, start_index;
    guint deck_phase;
    gdouble overlap_seconds;
    gint64 now_utc_us, start_utc_us;
    gboolean live;
    /* A loaded player rotation may start after its selection was prepared.
     * Drop trailing entries that no longer end inside their calendar window
     * instead of rejecting the plan; the first entry must still fit. */
    gboolean trim_calendar_end;
    /* Native-tempo single-player transport. Preserve verified cue IN/OUT and
     * enabled automatic exits, while disabling overlap, beatmatch and transition
     * effects before timing/calendar checks. A window violation rejects the
     * complete plan even when trim_calendar_end was requested. Deck assignment
     * remains the caller's separately validated scope decision. */
    gboolean single_player;
    /* An authenticated scope driver may refill a calendar with one remaining
     * routed entry; the ordinary named rotation still requires two entries. */
    gboolean allow_single_routed;
    gboolean has_default_policy, default_beat_sync;
    guint default_mode;
    gdouble default_overlap_seconds;
    const RadioSPlayoutHistory *history;
    JsonObject *copyright_policy; /* borrowed validated snapshot, NULL uses prepare response */
} RadioSPlayoutRequest;

RadioSPlayoutHistory *radios_playout_history_new(void);
RadioSPlayoutHistory *radios_playout_history_copy(const RadioSPlayoutHistory *history);
void radios_playout_history_free(RadioSPlayoutHistory *history);
void radios_playout_history_import(RadioSPlayoutHistory *history, JsonArray *records);
/* Copies the optional validated rotation checkpoint; NULL clears it. */
void radios_playout_history_set_rotation(RadioSPlayoutHistory *history, JsonObject *rotation);
void radios_playout_history_observe(RadioSPlayoutHistory *history,
    const gchar *media_id, const gchar *version_id, const gchar *artist,
    const gchar *album, gint64 utc_us, guint64 position_frames);
JsonNode *radios_playout_history_json(const RadioSPlayoutHistory *history);

/* Strict C planning against a bounded, authenticated prepare response.
 * Rotation, UTC windows, history, deterministic selection and timing live in
 * C. Verification never follows an operator-entered file path. */
RadioSPlayoutPlan *radios_playout_plan_build(JsonObject *prepared,
    const RadioSPlayoutRequest *request, GError **error);
void radios_playout_plan_free(RadioSPlayoutPlan *plan);
/* Move an accepted prefix into a separate owned plan; no media re-read and
 * no change to entry identities, routing, timing or cue/transition geometry. */
RadioSPlayoutPlan *radios_playout_plan_take_prefix(RadioSPlayoutPlan *plan, guint count);
/* Rebuild accepted jingle annotations and contiguous automatic music events
 * after an appended batch or source rebase, retaining accepted event identity.
 * Never selects a rule, inserts audio, or changes playback timing. */
void radios_playout_plan_refresh_jingle_metadata(RadioSPlayoutPlan *plan);
/* Rebase a playing source atomically, then clip/retime its successor fades
 * and revalidate the authenticated calendar/history bounds. Failure preserves
 * the complete prepared plan; no deck or playback state is changed here. */
gboolean radios_playout_plan_adopt_source(RadioSPlayoutPlan *plan, guint64 source_frame,
    const RadioSPlayoutRequest *request, GError **error);
/* Returns separately allocated flat array; strings remain owned by plan. */
RadioSAudioRunItem *radios_playout_plan_items(const RadioSPlayoutPlan *plan);
void radios_playout_plan_copyright(RadioSPlayoutPlan *plan, JsonObject *policy);

#endif
