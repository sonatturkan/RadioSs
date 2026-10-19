#ifndef RADIOS_CATALOGUE_PLAYLIST_H
#define RADIOS_CATALOGUE_PLAYLIST_H
#include "radios/data.h"

/* The selection callback returns owned desktop playlist metadata in table order.
 * NULL means the selection contains content that cannot become playlist tracks. */
typedef JsonArray *(*RadioSCataloguePlaylistSelection)(gpointer data);
typedef void (*RadioSCataloguePlaylistCreated)(JsonObject *playlist, gpointer data);
/* Convert a media/version catalogue row into owned track metadata.
 * Programme and playlist summary rows do not represent individual tracks. */
JsonObject *radios_catalogue_playlist_track(JsonObject *record);
GtkWidget *radios_catalogue_playlist_button_new(RadioSViewContext *ctx,
    GtkWidget *owner, GtkTreeSelection *selection,
    RadioSCataloguePlaylistSelection selected, RadioSCataloguePlaylistCreated created,
    gpointer data);
void radios_catalogue_playlist_button_refresh(GtkWidget *button);
#endif
