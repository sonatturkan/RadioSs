#ifndef RADIOS_PLAYLIST_BROWSER_H
#define RADIOS_PLAYLIST_BROWSER_H

#include "radios/views.h"
#include <json-glib/json-glib.h>

#define RADIOS_PLAYLIST_ENTRIES_MIME "application/x-radios-playlist-entries"

/* Independent source browser: direct local media with search and paging.
 * It never starts playback. */
GtkWidget *radios_playlist_browser_new(RadioSViewContext *ctx);
/* Owned complete entry-save metadata in visible table order. Returns an
 * empty array when no songs are selected or when the database identity that
 * supplied those rows is no longer current. */
JsonArray *radios_playlist_browser_selected(GtkWidget *browser);
/* Explicitly reloads the current source; target playlist saves need not call
 * this function, so source selection survives append acknowledgements. */
void radios_playlist_browser_refresh(GtkWidget *browser);

#endif
