#ifndef RADIOS_SCHEMA_SETTINGS_UI_H
#define RADIOS_SCHEMA_SETTINGS_UI_H

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

/* The parent owns persistence and session authorization. Inputs are cloned;
 * changed receives a borrowed full definition valid during the callback.
 * Setters do not emit changes; unrelated definition fields are preserved. */
typedef void (*RadioSSchemaSettingsChanged)(GtkWidget *editor,
    JsonNode *definition, gpointer user);

GtkWidget *radios_schema_settings_new(JsonObject *definition,
    RadioSSchemaSettingsChanged changed, gpointer user);
void radios_schema_settings_set_config(GtkWidget *editor, JsonObject *definition);
void radios_schema_settings_set_playlists(GtkWidget *editor, JsonArray *playlists);
/* Opens the parent's dynamic player/cluster routing for a selected playlist
 * step. The source id is borrowed for the duration of the callback. */
void radios_schema_settings_set_routing_handler(GtkWidget *editor,
    void (*handler)(GtkWidget *editor, const gchar *source_id, gpointer user),
    gpointer user);
/* Live player topology for the per-step "Oynatıcı kümesi" choice: objects
 * {deck 1..32, name, cluster_id, cluster_name}; cloned. */
void radios_schema_settings_set_players(GtkWidget *editor, JsonArray *players);
/* The station's saved rotation library: its AutoDJ presets become
 * assignable "existing automations"; a chosen one is copied into the
 * schema's own autodj_presets when a record using it is saved. */
void radios_schema_settings_set_library(GtkWidget *editor, JsonArray *rotation_library);
/* Defined mixer strips for the live record channel tree:
 * [{number 1..32, name, label "dj"|"talk"|"music"|"", source}]; cloned. */
void radios_schema_settings_set_channels(GtkWidget *editor, JsonArray *channels);
JsonNode *radios_schema_settings_snapshot(GtkWidget *editor);

#endif
