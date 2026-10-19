#ifndef RADIOS_PLAYER_TRANSPORT_H
#define RADIOS_PLAYER_TRANSPORT_H

#include "radios/views.h"

/* Call after an explicit library load succeeds on a live player's deck. The
 * new PCM remains loaded; queued lookup/match callbacks lose their intent and
 * the card's transport now belongs to the selected library media. */
void radios_broadcast_player_library_loaded(RadioSViewContext *ctx, guint deck);

#endif
