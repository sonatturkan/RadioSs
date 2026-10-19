#ifndef RADIOS_MEDIA_ROUTING_H
#define RADIOS_MEDIA_ROUTING_H
#include "radios/data.h"

/* The routing panel retains independent snapshots of the selected real media.
 * Checked playlists receive every selected record, without adding duplicates.
 * Destination choices survive refresh; commits use existing CAS receipts. */
GtkWidget *radios_media_routing_new(RadioSViewContext *ctx);
void radios_media_routing_select(GtkWidget *panel, JsonArray *records);
gboolean radios_media_routing_busy(GtkWidget *panel);
#endif
