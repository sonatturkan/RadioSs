#ifndef RADIOS_LOCAL_EVENT_PREPARE_H
#define RADIOS_LOCAL_EVENT_PREPARE_H
#include "radios/views.h"
#include <json-glib/json-glib.h>

/* Manual, read-only materialization of one local Event recipe. The result is
 * a borrowed formal programme configuration, containing one new block and new
 * occurrence UUIDs. The caller must freshly prepare it before any playback.
 * No source document, live queue, automation, history or audio state is changed.
 * The callback runs on the originating main context while owner remains alive.
 * Destroying owner cancels the operation; destroy runs exactly once in all cases.
 * The returned cancellable is owned by the caller; NULL means invalid arguments. */
typedef void (*RadioSLocalEventPrepared)(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *programme, const GError *error, gpointer user);
GCancellable *radios_local_event_prepare_async(RadioSViewContext *ctx,
    GtkWidget *owner, JsonObject *local_config, RadioSLocalEventPrepared callback,
    gpointer user, GDestroyNotify destroy);
/* Kılavuz §06/§07: L on a live event starts the automation schema set for
 * that event. Reads the automation_schema definition schema_id from the main
 * calendar or an active saved weekly programme (the players' schema list),
 * places it on a synthetic event that starts now in the calendar's timezone
 * and lasts duration_ms (<= 0: one hour), cut at local midnight, and runs the
 * authenticated selection from now. The callback receives the same borrowed
 * formal programme configuration as radios_local_event_prepare_async: title
 * (schema name), date, note, events with new UUIDs and block_id, and the
 * planner's blocks (a live step stays a content-less kind "live" block).
 * Nothing is saved; a schema changed meanwhile fails. Same ownership,
 * cancellation and destroy contract; NULL means invalid arguments. */
GCancellable *radios_local_event_prepare_schema_async(RadioSViewContext *ctx,
    GtkWidget *owner, const gchar *schema_id, gint64 duration_ms, RadioSLocalEventPrepared callback,
    gpointer user, GDestroyNotify destroy);
#endif
