#ifndef RADIOS_AUTOMATION_SCOPE_H
#define RADIOS_AUTOMATION_SCOPE_H

#include "radios/player_topology.h"

G_BEGIN_DECLS

typedef enum {
    RADIOS_AUTOMATION_SCOPE_PLAYER,
    RADIOS_AUTOMATION_SCOPE_CLUSTER,
    RADIOS_AUTOMATION_SCOPE_UNCLUSTERED
} RadioSAutomationScopeKind;

typedef struct {
    RadioSAutomationScopeKind kind;
    guint deck; /* Stable engine slot, used only for PLAYER. */
    gchar cluster_id[37]; /* Used only for CLUSTER. */
} RadioSAutomationScope;

typedef struct {
    RadioSAutomationScope scope;
    gchar automation_id[37]; /* Saved weekly_programme document, or empty. */
    gboolean inherited;
    gboolean routed_pool; /* Exact runtime pool from an authenticated saved rotation. */
    guint source, decks[RADIOS_PLAYER_TOPOLOGY_MAX_PLAYERS], count;
    RadioSAutoDJMode autodj_mode;
    gint autodj_target;
    gchar topology_fingerprint[65];
    /* Enabled unclustered players outside decks (bit = slot). A rotation's
     * advertisement, jingle and sweeper pools may name them (cart-style
     * players beside a music cluster); music stays inside decks. */
    guint32 auxiliary;
} RadioSAutomationBinding;

/* Capture scope membership independently from widget visibility. Individual
 * assignments override cluster assignments. A player includes its transition
 * peer only in full AutoDJ mode; semi/off keep a single player. Cluster and
 * independent aggregate scopes include all enabled members. A cluster plays
 * together: its autodj_mode is TRANSITIONS with two or more members and SEMI
 * with one, independent of stale per-member flags the cluster UI cannot edit;
 * the independent aggregate keeps its first member's own mode. Empty groups
 * are valid selections but cannot execute an event. Nothing starts playback. */
gboolean radios_automation_scope_resolve(const RadioSPlayerTopology *topology,
    const RadioSAutomationScope *scope, RadioSAutomationBinding *binding,
    GError **error);

/* Preserve a saved rotation's exact physical-player pool for a PLAYER start.
 * Every member must be enabled and in the clicked player's own group or an
 * unclustered auxiliary player (RadioSAutomationBinding.auxiliary); the
 * source must occur in the supplied unique pool. Its slot is moved to the
 * front without changing the relative order of the remaining players.
 * This does not claim decks or alter ordinary/semi player scope resolution. */
gboolean radios_automation_scope_resolve_routed(const RadioSPlayerTopology *topology,
    const RadioSAutomationScope *scope, const guint *decks, guint count,
    RadioSAutomationBinding *binding, GError **error);

/* Recheck after asynchronous schema/media preparation. Membership, inherited
 * assignment, AutoDJ mode and target changes invalidate the captured binding. */
gboolean radios_automation_scope_binding_current(const RadioSPlayerTopology *topology,
    const RadioSAutomationBinding *binding, GError **error);

/* Owned ephemeral copy of an already resolved calendar event. Missing player
 * distributions become explicit 1-based pools inside the captured scope.
 * Existing per-kind distributions retain their order; saved players outside
 * the scope are dropped (an emptied kind falls back to the complete scope
 * pool) so an incompatible schema keeps playing on the group's own players.
 * Malformed pools remain errors. Durable weekly record/config and caller
 * input remain unchanged. The ordinary authenticated media preparation path
 * still must run. */
JsonNode *radios_automation_scope_route_event(const RadioSAutomationBinding *binding,
    JsonObject *resolved_event, GError **error);
/* Same routing; *notice receives an owned Turkish sentence naming the saved
 * players that are not in this scope ("Şemadaki Oynatıcı 5 bu kümede yok; …"),
 * or NULL when every saved pool already fits. */
JsonNode *radios_automation_scope_route_event_notice(const RadioSAutomationBinding *binding,
    JsonObject *resolved_event, gchar **notice, GError **error);

/* Validate an exact active weekly document and its complete calendar. Revision
 * zero accepts any positive revision; a positive revision requires equality. */
gboolean radios_automation_scope_record_valid(JsonObject *record,
    const gchar *station_id, const gchar *document_id, gint64 revision,
    GError **error);

G_END_DECLS
#endif
