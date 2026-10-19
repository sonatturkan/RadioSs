#ifndef RADIOS_PLAYER_WORKSPACE_H
#define RADIOS_PLAYER_WORKSPACE_H

#include "radios/automation_scope.h"
#include "radios/playout_ui.h"

typedef struct RadioSPlayerWorkspace RadioSPlayerWorkspace;

/* Application-owned definitions, programme drafts and execution contexts.
 * All calls run on the GTK thread. Loading definitions never starts audio. */
RadioSPlayerWorkspace *radios_player_workspace_new(RadioSViewContext *ctx, GtkWidget *owner);
void radios_player_workspace_close(RadioSPlayerWorkspace *workspace);
void radios_player_workspace_free(RadioSPlayerWorkspace *workspace);
gboolean radios_player_workspace_ready(const RadioSPlayerWorkspace *workspace);
/* Explicitly discard a conflicted local definition draft and reread its CAS baseline. */
void radios_player_workspace_reload(RadioSPlayerWorkspace *workspace);
const RadioSPlayerTopology *radios_player_workspace_topology(const RadioSPlayerWorkspace *workspace);
gboolean radios_player_workspace_contains(const RadioSPlayerWorkspace *workspace, guint deck);
gboolean radios_player_workspace_replace(RadioSPlayerWorkspace *workspace,
    const RadioSPlayerTopology *topology, GError **error);
gboolean radios_player_workspace_assign(RadioSPlayerWorkspace *workspace,
    const RadioSAutomationScope *scope, const gchar *automation_id, GError **error);
/* Owned selection projection: clusters and one "Kümesizler" aggregate.
 * An individual scope returns only that player's events/queue/runtime. */
JsonNode *radios_player_workspace_projection(RadioSPlayerWorkspace *workspace,
    const RadioSAutomationScope *scope, GError **error);
/* A playlist/programme draft is independent from the AutoDJ mode/target.
 * First access opens its existing CAS document; retry after it becomes ready. */
/* Owned formal programme clone. PENDING opens/awaits the scope's CAS draft. */
gboolean radios_player_workspace_get_programme(RadioSPlayerWorkspace *workspace,
    const RadioSAutomationScope *scope, JsonObject **programme, GError **error);
/* TRUE only after the latest draft has an acknowledged positive CAS
 * revision. PENDING covers its initial read and outstanding save/conflict. */
gboolean radios_player_workspace_programme_saved(RadioSPlayerWorkspace *workspace,
    const RadioSAutomationScope *scope, GError **error);
gboolean radios_player_workspace_set_programme(RadioSPlayerWorkspace *workspace,
    const RadioSAutomationScope *scope, JsonObject *programme, GError **error);
/* Opens the source's isolated history baseline without starting audio.
 * FALSE with G_IO_ERROR_PENDING awaits the definition save or history read. */
gboolean radios_player_workspace_prepare_scope(RadioSPlayerWorkspace *workspace,
    guint source, GError **error);
/* Freshly authenticated prepared media, routed within the captured scope.
 * SEMI uses one player; TRANSITIONS uses the captured group/target pool. */
gboolean radios_player_workspace_start_prepared(RadioSPlayerWorkspace *workspace,
    const RadioSAutomationBinding *binding, JsonObject *prepared,
    RadioSAutoDJMode mode, GError **error);
/* Exact execution subset inside the original immutable binding. Plain
 * independent AutoDJ may lease its chosen pair while other players continue.
 * Saved per-kind schema distributions use the original complete pool.
 * Origin is independent from future scope assignment: a plain programme uses
 * NULL/FALSE, while an authenticated schema supplies its exact document UUID.
 * assigned=TRUE additionally requires that UUID to match the frozen binding. */
gboolean radios_player_workspace_start_prepared_pool(RadioSPlayerWorkspace *workspace,
    const RadioSAutomationBinding *binding, JsonObject *prepared,
    const guint *decks, guint count, RadioSAutoDJMode mode,
    const gchar *automation_id, gboolean assigned, GError **error);
gboolean radios_player_workspace_action(RadioSPlayerWorkspace *workspace,
    guint source, RadioSPlayoutAction action, GError **error);
/* Borrowed app-owned context, valid until workspace close. source_context
 * requires prepare_scope to have initialized the source's private controller.
 * running_context finds only this workspace's current owner of a physical deck. */
RadioSViewContext *radios_player_workspace_source_context(RadioSPlayerWorkspace *workspace, guint source);
RadioSViewContext *radios_player_workspace_running_context(RadioSPlayerWorkspace *workspace, guint deck);
gboolean radios_player_workspace_snapshot(RadioSPlayerWorkspace *workspace,
    guint source, RadioSPlayoutSnapshot *snapshot);

#endif
