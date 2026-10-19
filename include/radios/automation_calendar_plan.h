#ifndef RADIOS_AUTOMATION_CALENDAR_PLAN_H
#define RADIOS_AUTOMATION_CALENDAR_PLAN_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

/* Convert a civil event minute (0..1440) in its optional IANA timezone
 * (default UTC) to UTC microseconds. Invalid date/timezone returns zero. */
gint64 radios_automation_calendar_event_time(JsonObject *event, gint64 minute);

/* Owned deep event snapshot. Named assignments resolve against the same
 * calendar document and expand at most 50 playlist sources with deterministic
 * per-assignment UUIDs. Both inputs remain unchanged; unknown references fail. */
JsonNode *radios_automation_calendar_resolve_event(JsonObject *calendar,
    JsonObject *event, GError **error);

/* Reattach trusted internal selection hints to freshly authenticated phase-2
 * programme preparation only after exact entry order/UUID and all four media
 * binding identities, authoritative kind, cues and analysis match. Routing and
 * transition hints are strictly typed. Neither input is mutated. This adds no fields to
 * the durable formal programme schema and never authorizes playback. */
JsonNode *radios_automation_calendar_merge_selection(JsonObject *verified,
    JsonObject *selection, GError **error);

/* Owned legacy automation config for the existing authenticated prepare API.
 * Both inputs are borrowed and remain unchanged. NULL defaults use an idle
 * two-deck configuration; this function never opens a playback gate. */
JsonNode *radios_automation_calendar_prepare_config(JsonObject *event,
    JsonObject *legacy_defaults, GError **error);

/* Owned standard programme preparation selected from authenticated sources.
 * Legacy frequency is the row quota per rotation round. Named assignments
 * choose one eligible source per group turn, emit count (default 1), and
 * apply frequency to logical group visits. Named gaps, source time windows,
 * minimum turn-start intervals and independently seeded weighted or linear
 * enabled-rule transition selection are enforced without
 * inventing elapsed time. An unfinished protected quota yields to another
 * eligible group; selection stops diagnostically if no legal media remains.
 * Trusted calendar_transition hints are revalidated by merge_selection after
 * fresh programme preparation and applied before native plan timing. Optional
 * per-source autodj maps music/advertisement/jingle/sweeper to nonempty unique
 * 1-based deck pools (1..32), emitted as calendar_routing.decks for each actual
 * verified media kind. Other supported music/other kinds use the music pool.
 * Durable local beatmatch mode 3 becomes native crossfade mode 1 plus the
 * beat_sync hint; native preparation must verify rhythm before playback.
 * Each candidate's estimated start uses the native planner's span, exit and
 * bounded overlap arithmetic with the boundary that will really be compiled:
 * a rotation kind_transitions policy replaces the probabilistic hint there.
 * Beat alignment, tempo stretch and probability exits remain native-only;
 * the native planner trims a later entry whose repeat gap or playlist turn
 * no longer holds on its real timeline when trailing trimming is requested.
 * Optional lowpass effects require positive overlap and apply only to the selected transition.
 * Named and explicitly routed legacy rows retain authenticated
 * calendar_constraints with UTC source/event bounds, repeat gaps, minimum
 * source-turn interval and an exact first-item-of-turn flag. Native timing
 * must enforce these again after any beatmatch tempo adjustment. Ordinary
 * legacy rows retain their previous absence of internal constraint hints.
 * Legacy random disperses that row's
 * slots within the round while fixed rows retain their relative order.
 * Optional rotation media_kind is all (also the omitted-field default),
 * music, jingle, advertisement or sweeper. Authenticated verified candidates
 * are filtered before frequency quotas and kind pools are built; a filtered
 * source with no playable match is rejected. No playlist identities change.
 * Optional event rotation_selection_mode weights legal assignment groups
 * independently from each group's source/song/transition RNG streams. Named
 * assignment weight zero disables its entire group in both modes; omission
 * retains weight100 and the previous round-robin group order.
 * Named source weight zero disables that source in all selection modes.
 * Optional track_selection_mode linear/weighted overrides category shuffling;
 * omission preserves legacy category order/random flags. Sparse track_rules
 * bind original saved entry UUIDs, weight (zero disables), stable explicit
 * order, logical source-candidate visit frequency, per-plan/per-source maximum
 * play counts (zero unlimited), UTC windows and actual-media repeat intervals.
 * Missing saved entries are reported diagnostically. Song/source/event window
 * intersections and max(track gap, song interval) retain the existing seven
 * authenticated constraint fields for native tempo-adjusted revalidation.
 * Playlist kind slots remain in order; each row independently shuffles its
 * music/other, advertisement and sweeper pools using the three corresponding
 * flags. A pool is exhausted before reuse. limit is 1..200, and full source
 * durations never exceed the selected calendar interval.
 * All JSON is deep-copied before mutation; generated programme entry UUIDs
 * are new. Runtime schema_entry_settings carries the preceding programme's
 * programme_entry and optional autodj_presets, preserving weighted entry
 * alternatives and their own assigned transitions. An explicit runtime
 * schema_entry_source_order still overrides the weighted choice. Neither
 * runtime member belongs to the durable calendar contract. The existing
 * programme prepare/planner path must revalidate media
 * before playback. No history or broadcast evidence is created here. */
JsonNode *radios_automation_calendar_prepared_programme(JsonObject *event,
    JsonObject *prepared, const gchar *station_id, const gchar *seed,
    guint limit, GError **error);

/* Genuine UTC late join or future occurrence start. start_utc_us must lie in
 * the selected event's half-open UTC window. Selection evaluates eligibility,
 * source/song/repeat gaps and remaining cue geometry from this exact epoch;
 * even legacy rows gain the existing seven native constraint hints. Ordinary
 * prepared_programme callers retain their original start/order behavior.
 * After fully valid preparation, zero currently eligible/fitting rows return
 * G_IO_ERROR_WOULD_BLOCK; malformed preparation remains INVALID_DATA. */
JsonNode *radios_automation_calendar_prepared_programme_at(JsonObject *event,
    JsonObject *prepared, const gchar *station_id, const gchar *seed,
    guint limit, gint64 start_utc_us, GError **error);

/* Runtime SEMI selects a whole fitting batch using native cue IN, CUE/
 * auto-next and explicit AutoDJ OUT geometry, without saved overlaps. This
 * changes selection timing only: saved JSON, authenticated markers and strict
 * transition validation remain intact. FALSE preserves ordinary selection. */
JsonNode *radios_automation_calendar_prepared_programme_at_mode(JsonObject *event,
    JsonObject *prepared, const gchar *station_id, const gchar *seed,
    guint limit, gint64 start_utc_us, gboolean single_player, GError **error);

/* Same selection, returning only the owned standard programme config for
 * the existing programme prepare, append, save and transport APIs. */
JsonNode *radios_automation_calendar_programme(JsonObject *event,
    JsonObject *prepared, const gchar *station_id, const gchar *seed,
    guint limit, GError **error);

/* Kılavuz §02: whether a designed preset may serve a boundary into content
 * of kind (music, advertisement, jingle, sweeper, or a live label). Presets
 * assigned to kinds in settings.autodj_automation serve only those kinds;
 * unassigned presets serve every kind. NULL/empty kind or id returns TRUE. */
gboolean radios_automation_preset_kind_usable(JsonObject *settings,
    const gchar *preset_id, const gchar *kind);

/* Turkish names of the content kinds a rotation's "excluded_kinds" setting
 * leaves out ("Reklam, Sweeper"), or an owned empty string. */
gchar *radios_automation_calendar_excluded_kinds_text(JsonObject *settings);

#endif
