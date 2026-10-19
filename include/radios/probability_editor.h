#ifndef RADIOS_PROBABILITY_EDITOR_H
#define RADIOS_PROBABILITY_EDITOR_H

#include "radios/views.h"
#include <json-glib/json-glib.h>

/* The parent owns persistence. Snapshot is borrowed only during changed().
 * A JSON null snapshot means nothing is left at this level: the parent
 * removes its probability_policy. Setters clone their inputs and never emit
 * changed(). Invalid edits are refused at once and never reach changed(). */
typedef struct {
    void (*changed)(GtkWidget *editor, JsonNode *snapshot, gpointer user);
    gpointer user;
    GDestroyNotify destroy;
} RadioSProbabilityEditorHooks;

GtkWidget *radios_probability_editor_new(RadioSViewContext *ctx,
    const RadioSProbabilityEditorHooks *hooks);
void radios_probability_editor_set_policy(GtkWidget *editor, JsonObject *policy);
/* Authenticated targets: {id: UUID, scope: automation|playlist|track, name: text,
 * playlist_id?: UUID, purpose?: text, listed?: bool}. Tracks identify playlist
 * entries, never media UUIDs. Playlist targets with purpose jingle (or none)
 * are offered as jingle sources; listed=false keeps a target out of the
 * percentage table (jingle source or name only). */
void radios_probability_editor_set_targets(GtkWidget *editor, JsonArray *targets);
/* Opens the percentage table on automation, playlist or track rows. */
void radios_probability_editor_show_scope(GtkWidget *editor, const gchar *scope);
JsonNode *radios_probability_editor_snapshot(GtkWidget *editor);
gboolean radios_probability_editor_ready(GtkWidget *editor);

#endif
