#ifndef RADIOS_ROTATION_EDITOR_H
#define RADIOS_ROTATION_EDITOR_H

#include "radios/views.h"
#include <json-glib/json-glib.h>

/* The parent owns definition persistence. Snapshot is borrowed only during
 * changed(); use json_node_copy/deep copy if retaining it asynchronously. */
typedef struct {
    void (*changed)(GtkWidget *editor, JsonNode *snapshot, gpointer user);
    gpointer user;
    GDestroyNotify destroy;
} RadioSRotationEditorHooks;

GtkWidget *radios_rotation_editor_new(RadioSViewContext *ctx,
    const RadioSRotationEditorHooks *hooks);
/* Setters clone authenticated parent data and never emit changed(). */
void radios_rotation_editor_set_config(GtkWidget *editor, JsonObject *config);
void radios_rotation_editor_set_playlists(GtkWidget *editor, JsonArray *playlists);
/* Existing schema definitions for live-event references; input is cloned. */
void radios_rotation_editor_set_schemas(GtkWidget *editor, JsonArray *definitions);
/* The parent's defined mixer strips [{number 1..32, name, label, source}] for
 * the live step and schema channel trees; cloned. NULL: every strip. */
void radios_rotation_editor_set_channels(GtkWidget *editor, JsonArray *channels);
/* Open the schema's sequence, player-group and assigned transition settings. */
void radios_rotation_editor_open_schema_settings(GtkWidget *editor);
/* Owned deep snapshot, or NULL while the visible draft has invalid fields. */
JsonNode *radios_rotation_editor_snapshot(GtkWidget *editor);
gboolean radios_rotation_editor_ready(GtkWidget *editor);

#endif
