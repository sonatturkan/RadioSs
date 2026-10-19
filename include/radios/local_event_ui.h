#ifndef RADIOS_LOCAL_EVENT_UI_H
#define RADIOS_LOCAL_EVENT_UI_H
#include "radios/views.h"
#include <json-glib/json-glib.h>

/* The borrowed configuration is delivered only for the explicit insert action.
 * Returning FALSE leaves the local event open and displays the supplied error.
 * Saving this dialog never starts playback or writes a programme/schema. */
typedef gboolean (*RadioSLocalEventInsert)(RadioSViewContext *ctx,
    JsonObject *configuration, gpointer user, GError **error);
GtkWidget *radios_local_event_ui_open(RadioSViewContext *ctx, GtkWindow *parent,
    RadioSLocalEventInsert insert, gpointer user);
JsonNode *radios_local_event_ui_snapshot(GtkWidget *dialog); /* owned */
/* Seed a freshly opened, unchanged builder from an event. Existing edited or
 * saved drafts remain intact. This never writes a document or starts audio. */
gboolean radios_local_event_ui_set_event(GtkWidget *dialog, const gchar *title,
    const gchar *kind);
/* Seed an unchanged fresh builder with an existing event assignment. Refuses
 * to overwrite an edited or saved local document. The recipe is copied. */
gboolean radios_local_event_ui_set_configuration(GtkWidget *dialog, JsonObject *recipe);
/* Give the explicit insert action its contextual meaning, e.g. assign. */
void radios_local_event_ui_set_insert_label(GtkWidget *dialog, const gchar *label);
/* Optional explicit playback action. The handler borrows the same exact recipe
 * as Insert. Playback choices are saved with the recipe only by explicit Save;
 * changing them marks the builder dirty and never starts audio.
 * NULL hides the play action and its selectors. Callback data is borrowed. */
void radios_local_event_ui_set_play_handler(GtkWidget *dialog,
    RadioSLocalEventInsert play, gpointer user);
/* Read the current selection without changing or saving it: engine deck
 * 0..31 or G_MAXUINT for automatic,
 * and an empty cluster id means stop after this event. The caller must validate
 * current membership, output, ownership and async fences before playback. */
gboolean radios_local_event_ui_playback(GtkWidget *dialog, guint *deck,
    gchar cluster_id[37]);
/* Change an available selection atomically and mark the recipe dirty;
 * invalid ids leave both selectors unchanged. Empty/NULL cluster means stop. */
gboolean radios_local_event_ui_set_playback(GtkWidget *dialog, guint deck,
    const gchar *cluster_id);
/* Async insert completion updates only the originating, still-open dialog. */
void radios_local_event_ui_insert_result(GtkWidget *dialog, const gchar *message);
#endif
