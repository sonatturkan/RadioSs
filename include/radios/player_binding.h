#ifndef RADIOS_PLAYER_BINDING_H
#define RADIOS_PLAYER_BINDING_H

#include "radios/views.h"
#include "radios/audio.h"
#include <json-glib/json-glib.h>

/* Broadcast's existing card queue is the presentation/transport owner. These
 * hooks update its data only and preserve its existing widgets and layout. */
gboolean radios_broadcast_player_register_playlist(RadioSViewContext *ctx,
    JsonArray *items, const gchar *name, guint index, gint preferred_deck,
    guint *deck, GError **error);
gboolean radios_broadcast_player_playlist(RadioSViewContext *ctx, GtkWidget *card,
    JsonArray **items, gchar **name, guint *index, guint *deck);

#endif
