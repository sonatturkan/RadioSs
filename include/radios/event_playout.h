#ifndef RADIOS_EVENT_PLAYOUT_H
#define RADIOS_EVENT_PLAYOUT_H
#include "radios/views.h"
#include <json-glib/json-glib.h>

/* Explicit, transient event playback. A normal programme or an exact local
 * recipe is prepared without saving/replacing any queue, recipe or assignment.
 * The source is always one independent MAIN player in SEMI mode. G_MAXUINT
 * chooses an eligible idle independent player. owner is a weak lifetime fence;
 * pass the application owner when selection/dialog changes must not cancel it.
 * An optional saved cluster UUID continues only after the same finite event
 * has actually completed and drained. Async failures use ctx->notify. */
gboolean radios_event_playout_programme(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *programme, guint deck, const gchar *next_cluster_id, GError **error);
gboolean radios_event_playout_local(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *local_config, guint deck, const gchar *next_cluster_id, GError **error);
/* Same transient playback, explicitly mapped to an existing GUI event UUID.
 * Materialized recipes may have fresh leaf/block UUIDs; origin remains the
 * operator's selected event and never changes a saved programme or recipe. */
gboolean radios_event_playout_programme_for_event(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *programme, guint deck, const gchar *next_cluster_id,
    const gchar *origin_block_id, GError **error);
gboolean radios_event_playout_local_for_event(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *local_config, guint deck, const gchar *next_cluster_id,
    const gchar *origin_block_id, GError **error);
/* Owned display-only snapshot {runs:[{origin_block_id,source,state,entry_id,
 * next_entry_id,position_ms,duration_ms,cue_in_ms,programme,completed}]}; source
 * is zero-based and state uses RadioSPlayoutState. Captured session, topology,
 * intent and actual runner epoch fence every visible row. Materialized formal
 * programme is an immutable clone, never an editor assignment or queue write. */
JsonNode *radios_event_playout_snapshot(RadioSViewContext *ctx);
/* Same fenced scalar rows and saved source/target names, omitting programme.
 * Use for frequent GUI polling without copying the complete event contents. */
JsonNode *radios_event_playout_display_snapshot(RadioSViewContext *ctx);
/* Owned, consumed terminal notifications [{origin_block_id,source}]. Only a
 * final COMPLETE receipt with released leases and an empty source emits one;
 * Stop, replacement, failure and cancellation never mark an event completed.
 * At most 64 unseen notifications are retained per application window. */
JsonArray *radios_event_playout_take_completed(RadioSViewContext *ctx);
/* Cancels this service's pending preparation or future continuation only.
 * Existing sounding audio and unrelated scopes remain untouched. Call before
 * the corresponding ordinary Stop action; G_MAXUINT selects all event jobs. */
void radios_event_playout_cancel(RadioSViewContext *ctx, guint deck);
gboolean radios_event_playout_busy(RadioSViewContext *ctx, guint deck);
#endif
