#ifndef RADIOS_ADVERT_UI_H
#define RADIOS_ADVERT_UI_H
#include "radios/playout_ui.h"

typedef struct RadioSAdvertController RadioSAdvertController;
RadioSAdvertController *radios_advert_ui_new(RadioSViewContext *ctx);
void radios_advert_ui_close(RadioSAdvertController *controller);
void radios_advert_ui_unref(RadioSAdvertController *controller);
/* Immutable backend prepare result. A durable run manifest is created before
 * the shared C runner starts. Schedule uses the saved break's UTC instant. */
gboolean radios_advert_ui_start(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *prepared, gboolean scheduled, GError **error);
gboolean radios_advert_ui_action(RadioSViewContext *ctx,
    RadioSPlayoutAction action, GError **error);
gboolean radios_advert_ui_pending(RadioSViewContext *ctx);
/* Capture terminal receipts before another queue replaces the engine's
 * immutable prefix. Local source consumption remains a separate proof class. */
void radios_advert_ui_checkpoint(RadioSViewContext *ctx);
#endif
