#ifndef RADIOS_WEEKLY_PROGRAMME_PLAN_H
#define RADIOS_WEEKLY_PROGRAMME_PLAN_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

/* One immutable UTC occurrence of a saved weekly_programme document.
 * calendar rebases ALL original event dates by the same complete-cycle offset;
 * event is the selected event resolved against that cloned rotation library.
 * Original saved event/source/song identities and settings are preserved.
 * All pointers are owned. No media authorization or playback gate is created. */
typedef struct {
    JsonNode *calendar;
    JsonNode *event;
    gchar *original_event_id;
    gchar *occurrence_key;
    guint64 cycle_index;
    gint64 start_utc_us;
    gint64 end_utc_us;
    gint64 effective_utc_us;
    gint64 next_start_utc_us;
    gboolean active;
} RadioSWeeklyProgrammeOccurrence;

/* Snapshot the complete calendar without dropping any events or definitions.
 * visible_week is a civil Monday YYYY-MM-DD fallback (and lower anchor bound).
 * The minimum Monday and final event determine a complete 1..52-week period.
 * Both arguments are borrowed. The returned typed configuration is owned. */
JsonNode *radios_weekly_programme_snapshot(JsonObject *calendar,
    const gchar *visible_week, gboolean repeat, GError **error);

/* Return the active half-open event, otherwise the closest future event.
 * Genuine now_utc_us is used exactly: effective_utc_us is MAX(now,start).
 * next_start_utc_us is the next unstarted event (0 if none), including the
 * selected future event. repeat cycles the entire week_count-week pattern;
 * times before the original anchor never create negative/backdated cycles.
 * NULL without an error means an empty or exhausted nonrepeating programme.
 * A caller must pin/authenticate document revision and freshly prepare media;
 * use prepared_programme_at(...,effective_utc_us,...) for an honest late join. */
RadioSWeeklyProgrammeOccurrence *radios_weekly_programme_next(JsonObject *config,
    gint64 now_utc_us, GError **error);

/* A temporary schema merged into the main flow ("Şema yükle" in the header):
 * the override's occurrence is returned while it is active, or when it starts
 * before the main schema's next slot; otherwise the main occurrence is
 * returned and preempt_utc_us carries the override's next start when that
 * falls inside the returned main slot (0 otherwise). Either configuration may
 * be NULL. NULL without an error means both schedules are exhausted. */
RadioSWeeklyProgrammeOccurrence *radios_weekly_programme_override_next(JsonObject *main_config,
    JsonObject *override_config, gint64 now_utc_us, gboolean *use_override,
    gint64 *preempt_utc_us, GError **error);

/* Initial general-queue selection: only starts strictly after now are eligible.
 * Already active slots are skipped. A repeating document may supply its next
 * complete cycle; a finite calendar never wraps old dates into the future. */
RadioSWeeklyProgrammeOccurrence *radios_weekly_programme_next_future(JsonObject *config,
    gint64 now_utc_us, GError **error);

typedef struct {
    JsonNode *record; /* original active server document, including kind/id/rev */
    JsonNode *config; /* weekly configuration; main is one finite selected slot */
    RadioSWeeklyProgrammeOccurrence *occurrence;
    gchar *source_key; /* kind:document-id:revision, for immutable source pins */
} RadioSWeeklyProgrammeSelection;

/* Compare authenticated, same-station automation_calendar/main and active
 * weekly_programme records. Main wins equal start times; other ties use UUID.
 * The original main record keeps every historical/future event. Only its
 * runtime config is narrowed to the selected event and complete library.
 * Empty/disabled/exhausted schedules yield NULL without an error. This pure
 * selection grants no media rights: callers must re-read the chosen source
 * and freshly prepare its media before starting. All result fields are owned. */
RadioSWeeklyProgrammeSelection *radios_weekly_programme_select_next(JsonArray *records,
    gint64 now_utc_us, GError **error);
/* Continuous reselection accepts an active or future slot whose start is at
 * or after the pinned initial start / previous slot's end. A late GTK tick
 * may join that slot, but cannot re-enter an older overlapping programme. */
RadioSWeeklyProgrammeSelection *radios_weekly_programme_select_running(JsonArray *records,
    gint64 now_utc_us, gint64 minimum_start_utc_us, GError **error);
void radios_weekly_programme_selection_free(RadioSWeeklyProgrammeSelection *selection);

void radios_weekly_programme_occurrence_free(RadioSWeeklyProgrammeOccurrence *occurrence);

/* Kılavuz §04/§06 broadcast order projection: copy the accepted planner rows
 * (block identity/kind/title, planned jingles, player, live and live_step
 * hints) onto a deep copy of the prepared programme configuration. A live
 * block without its own live object inherits programme_live; when the plan
 * has no live block at all and programme_live is given, every block receives
 * its label/channels so the event row can drive L/R. Inputs are borrowed and
 * unchanged; the owned result is a runtime view, never a saved edit. */
JsonNode *radios_weekly_programme_queue_projection(JsonObject *configuration,
    JsonObject *display, JsonObject *programme_live);

/* Kılavuz §03 "programdan programa devam": the schema definition governing
 * event. schema_id (an applied schema) wins when it exists in calendar's
 * rotation_library; otherwise event.live.automation_id, then the event's first
 * assignment whose definition is an automation_schema, then its first
 * assignment carrying settings.programme_entry. Resolved events (planner
 * rows with calendar_rotation_id) are accepted too. Borrowed; NULL if none. */
JsonObject *radios_weekly_programme_schema_definition(JsonObject *calendar,
    JsonObject *event, const gchar *schema_id);
/* programme_entry.source_order (1..50) of that definition; 0 without one. */
guint radios_weekly_programme_entry_order(JsonObject *calendar, JsonObject *event,
    const gchar *schema_id);

#endif
