#ifndef RADIOS_BROADCAST_UI_H
#define RADIOS_BROADCAST_UI_H
#include "radios/data.h"
#include "radios/audio_output.h"
RadioSBroadcastController *radios_broadcast_ui_new(RadioSViewContext *ctx);
void radios_broadcast_ui_close(RadioSBroadcastController *controller);
void radios_broadcast_ui_unref(RadioSBroadcastController *controller);
/* profile is an acknowledged {_id,_rev,config} document. Credentials live
 * only in the output worker; persistence and execution are distinct. */
gboolean radios_broadcast_ui_output_start(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *kind, JsonObject *profile, const gchar *password, GError **error);
gboolean radios_broadcast_ui_output_stop(RadioSViewContext *ctx, const gchar *kind, GError **error);
gboolean radios_broadcast_ui_metadata(RadioSViewContext *ctx, const gchar *title, GError **error);
void radios_broadcast_ui_metadata_automatic(RadioSViewContext *ctx, gboolean enabled);
gchar *radios_broadcast_ui_status(RadioSViewContext *ctx, const gchar *kind);
gboolean radios_broadcast_ui_output_snapshot(RadioSViewContext *ctx, const gchar *kind, RadioSAudioOutputSnapshot *out);
gboolean radios_broadcast_ui_emergency_start(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *document_id, gint64 revision, guint queue_index, GError **error);
gboolean radios_broadcast_ui_emergency_return(RadioSViewContext *ctx, GError **error);
gchar *radios_broadcast_ui_emergency_status(RadioSViewContext *ctx);
gboolean radios_broadcast_ui_emergency_pending(RadioSViewContext *ctx);
/* Capture terminal MAIN receipts before another queue replaces them. */
void radios_broadcast_ui_checkpoint(RadioSViewContext *ctx);
#endif
