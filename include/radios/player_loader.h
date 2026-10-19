#ifndef RADIOS_PLAYER_LOADER_H
#define RADIOS_PLAYER_LOADER_H

#include "radios/views.h"
#include <json-glib/json-glib.h>

/* A live player card input: {title, artist, album, duration_ms} with a verified
 * media binding (media_id, version_id, media_file_id, media_sha256). */
typedef void (*RadioSPlayerLoaderDone)(JsonArray *items, const gchar *name, gboolean append, gpointer data);

/* "Playlist yükle" of one card: library playlists, the plan an automation
 * provides and programmes. done runs at most once, only while owner is alive. */
void radios_player_loader_open(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *title, RadioSPlayerLoaderDone done, gpointer data);

/* Card input from a programme content or a playlist entry; NULL when it has
 * no verified media. */
JsonObject *radios_player_input_new(JsonObject *source);

#endif
