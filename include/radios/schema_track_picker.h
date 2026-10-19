#ifndef RADIOS_SCHEMA_TRACK_PICKER_H
#define RADIOS_SCHEMA_TRACK_PICKER_H

#include "radios/views.h"
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Kullanıcı isteği U2 (18.10.2026): the player card's "Otomasyon ▾ ›
 * Şemadan yükle…". A window lists the songs the schema selects for the
 * station's current slot: the player's own or its cluster's assigned
 * weekly_programme, otherwise the schema chosen in the window. Read only —
 * the schema list, one desktop-playout/prepare(automation) and the local
 * calendar selection; nothing is saved and a running automation is not
 * touched. The chosen song is re-read (desktop-media/get) and loads into the
 * deck exactly like "Kütüphaneden yükle" (radios_audio_ui_load_media, then
 * radios_broadcast_player_library_loaded and the card's MAIN choice). owner
 * is the player card; the window closes with it. */
void radios_schema_track_picker_open(RadioSViewContext *ctx, GtkWidget *owner, guint deck);
/* Opens the picker for owner/deck whenever button is clicked (the card menu
 * item); owner is held weakly. */
void radios_schema_track_picker_bind(GtkWidget *button, RadioSViewContext *ctx, GtkWidget *owner, guint deck);

/* Rows of a calendar selection (radios_automation_calendar_prepared_programme*
 * result) in play order: {number, title, artist, kind, duration_ms,
 * start_utc_us, media_id, version_id, entry_id}. Durations and starts follow
 * the selection's own handover geometry (AutoDJ IN/OUT, or cue IN/OUT for a
 * single player) from start_utc_us. Owned; empty for a selection without
 * tracks. */
JsonArray *radios_schema_track_picker_rows(JsonObject *selection, gint64 start_utc_us, gboolean single_player);

G_END_DECLS
#endif
