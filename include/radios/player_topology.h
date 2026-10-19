#ifndef RADIOS_PLAYER_TOPOLOGY_H
#define RADIOS_PLAYER_TOPOLOGY_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS
#define RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS 32u
#define RADIOS_PLAYER_TOPOLOGY_MAX_CLUSTERS 32u

typedef enum {
    RADIOS_AUTODJ_OFF = 0,
    RADIOS_AUTODJ_SEMI = 1,
    RADIOS_AUTODJ_TRANSITIONS = 2
} RadioSAutoDJMode;

typedef struct {
    gchar id[37], name[321], automation_id[37];
} RadioSPlayerCluster;
typedef struct {
    gboolean enabled;
    gchar name[257], cluster_id[37], automation_id[37];
    RadioSAutoDJMode autodj_mode;
    gint autodj_target; /* Engine deck 0..31, -1 chooses the next group member. */
} RadioSPlayerDefinition;
typedef struct {
    RadioSPlayerDefinition players[RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS];
    RadioSPlayerCluster clusters[RADIOS_PLAYER_TOPOLOGY_MAX_CLUSTERS];
    guint cluster_count;
    gchar master_name[257];
} RadioSPlayerTopology;

/* Deck arguments always identify stable engine slots 0..31. Persistence uses
 * slots 0..31 and visible names / player keys 1..32. Loading never starts audio.
 * Existing count-only channel documents become independent players via
 * from_legacy. from_json requires the dedicated live_players schema_version=1.
 * Both parsers reject invalid input without changing the destination.
 * JSON: {schema_version,deck_count,slots,names,clusters:[{id,name,automation_id}],
 * players:{"1":{cluster_id,automation_id,autodj_mode,autodj_target},...}}.
 * JSON autodj_target 0 means automatic; 1..32 names a visible player number. */
void radios_player_topology_default(RadioSPlayerTopology *topology, guint count);
gboolean radios_player_topology_from_json(RadioSPlayerTopology *topology,
    JsonObject *configuration, GError **error);
gboolean radios_player_topology_from_legacy(RadioSPlayerTopology *topology,
    JsonObject *channel_configuration, GError **error);
JsonObject *radios_player_topology_to_json(const RadioSPlayerTopology *topology);
gboolean radios_player_topology_validate(const RadioSPlayerTopology *topology, GError **error);
guint radios_player_topology_count(const RadioSPlayerTopology *topology);
RadioSPlayerDefinition *radios_player_topology_player(RadioSPlayerTopology *topology, guint deck);
const RadioSPlayerDefinition *radios_player_topology_player_const(const RadioSPlayerTopology *topology, guint deck);
RadioSPlayerCluster *radios_player_topology_cluster(RadioSPlayerTopology *topology, const gchar *id);
const RadioSPlayerCluster *radios_player_topology_cluster_const(const RadioSPlayerTopology *topology, const gchar *id);

gboolean radios_player_topology_add_player(RadioSPlayerTopology *topology,
    const gchar *cluster_id, guint *deck, GError **error);
gboolean radios_player_topology_remove_player(RadioSPlayerTopology *topology, guint deck, GError **error);
gboolean radios_player_topology_add_cluster(RadioSPlayerTopology *topology,
    const gchar *name, gchar id[37], GError **error);
/* Removing a cluster returns its members to the independent group. */
gboolean radios_player_topology_remove_cluster(RadioSPlayerTopology *topology, const gchar *id, GError **error);
gboolean radios_player_topology_set_cluster(RadioSPlayerTopology *topology,
    guint deck, const gchar *cluster_id, GError **error);
gboolean radios_player_topology_set_automation(RadioSPlayerTopology *topology,
    guint deck, const gchar *automation_id, GError **error);
gboolean radios_player_topology_set_cluster_automation(RadioSPlayerTopology *topology,
    const gchar *cluster_id, const gchar *automation_id, GError **error);
/* An individual schema overrides the cluster assignment; returned id is
 * borrowed and empty means there is no assignment. */
const gchar *radios_player_topology_automation(const RadioSPlayerTopology *topology, guint deck);
gboolean radios_player_topology_set_autodj(RadioSPlayerTopology *topology,
    guint deck, RadioSAutoDJMode mode, gint target, GError **error);
gboolean radios_player_topology_same_group(const RadioSPlayerTopology *topology, guint first, guint second);
/* Empty/NULL cluster_id names independent players; output can be NULL to count. */
guint radios_player_topology_members(const RadioSPlayerTopology *topology,
    const gchar *cluster_id, guint *decks, guint capacity);
/* -1 when disabled, semi, or no eligible peer exists. Selection never depends
 * on card visibility. Explicit targets stay inside their source group. */
gint radios_player_topology_next_target(const RadioSPlayerTopology *topology, guint source);
G_END_DECLS
#endif
