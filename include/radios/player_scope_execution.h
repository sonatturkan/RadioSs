#ifndef RADIOS_PLAYER_SCOPE_EXECUTION_H
#define RADIOS_PLAYER_SCOPE_EXECUTION_H
#include "radios/player_workspace.h"

typedef struct RadioSPlayerScopeExecution RadioSPlayerScopeExecution;
typedef enum {
    RADIOS_SCOPE_EXECUTION_IDLE, RADIOS_SCOPE_EXECUTION_PREPARING,
    RADIOS_SCOPE_EXECUTION_WAITING, RADIOS_SCOPE_EXECUTION_PLAYING,
    RADIOS_SCOPE_EXECUTION_PAUSED, RADIOS_SCOPE_EXECUTION_ERROR
} RadioSPlayerScopeExecutionState;
typedef struct {
    RadioSPlayerScopeExecutionState state;
    guint source;
    gboolean schema_active, repeat;
    gboolean override_active; /* the current slot comes from the temporary schema */
    gboolean loading_playlist, preparing;
    gint64 starts_at_utc_us, ends_at_utc_us;
    gchar event_name[801], error[256];
} RadioSPlayerScopeExecutionSnapshot;
/* Borrowed callback data. UI consumers must guard their widgets weakly: these
 * application-owned operations deliberately outlive selection/view changes.
 * destroy(user) runs exactly once, including synchronous rejection. */
typedef void (*RadioSPlayerScopeExecutionDone)(RadioSViewContext *ctx,
    const RadioSAutomationScope *scope, guint source, const GError *error, gpointer user);
/* Borrowed application-window service. Get never starts playback. */
RadioSPlayerScopeExecution *radios_player_scope_execution_get(RadioSViewContext *ctx);
void radios_player_scope_execution_close(RadioSPlayerScopeExecution *execution);
/* Load persists an authenticated formal programme, without starting audio;
 * playlist binding and AutoDJ target remain separate. */
gboolean radios_player_scope_execution_load_playlist(RadioSPlayerScopeExecution *execution,
    const RadioSAutomationScope *scope, const gchar *playlist_id,
    RadioSPlayerScopeExecutionDone done, gpointer user, GDestroyNotify destroy, GError **error);
/* NULL programme loads the selected scope's stored queue. OFF follows its
 * source definition (defaulting to SEMI); explicit modes override this start. */
gboolean radios_player_scope_execution_play_programme(RadioSPlayerScopeExecution *execution,
    const RadioSAutomationScope *scope, JsonObject *programme, RadioSAutoDJMode mode,
    RadioSPlayerScopeExecutionDone done, gpointer user, GDestroyNotify destroy, GError **error);
/* Optional caller fence runs immediately before each asynchronous install,
 * while callback user data is still alive. It never runs after done/destroy.
 * FALSE must describe the rejected stale intent in error. Guarded schema
 * callbacks await the first actual installation, retaining their fence across
 * future waits/retries; ordinary unguarded schema notifications are unchanged. */
typedef gboolean (*RadioSPlayerScopeExecutionGuard)(RadioSViewContext *ctx,
    const RadioSAutomationScope *scope, guint source, gpointer user, GError **error);
gboolean radios_player_scope_execution_play_programme_guarded(RadioSPlayerScopeExecution *execution,
    const RadioSAutomationScope *scope, JsonObject *programme, RadioSAutoDJMode mode,
    RadioSPlayerScopeExecutionGuard guard, RadioSPlayerScopeExecutionDone done,
    gpointer user, GDestroyNotify destroy, GError **error);
/* Play the assigned exact saved weekly schema continuously, including future
 * waits, late joins, remaining-event refills and subsequent/repeated events. */
/* Existing authenticated loaders may hand off their just-verified programme
 * response. The caller owns authentication; this captures its current client,
 * definitions and deck generations across the asynchronous readiness wait. */
gboolean radios_player_scope_execution_play_prepared(RadioSPlayerScopeExecution *execution,
    const RadioSAutomationScope *scope, JsonObject *prepared, RadioSAutoDJMode mode,
    RadioSPlayerScopeExecutionDone done, gpointer user, GDestroyNotify destroy, GError **error);
gboolean radios_player_scope_execution_play_schema(RadioSPlayerScopeExecution *execution,
    const RadioSAutomationScope *scope, RadioSAutoDJMode mode,
    RadioSPlayerScopeExecutionDone done, gpointer user, GDestroyNotify destroy, GError **error);
gboolean radios_player_scope_execution_play_schema_guarded(RadioSPlayerScopeExecution *execution,
    const RadioSAutomationScope *scope, RadioSAutoDJMode mode,
    RadioSPlayerScopeExecutionGuard guard, RadioSPlayerScopeExecutionDone done,
    gpointer user, GDestroyNotify destroy, GError **error);
/* Stop also cancels pending/scheduled operations. Only owned scope runs are
 * controlled; selecting a child of a running cluster resolves its real owner. */
gboolean radios_player_scope_execution_action(RadioSPlayerScopeExecution *execution,
    const RadioSAutomationScope *scope, RadioSPlayoutAction action, GError **error);
/* Runtime override only; turning repeat off finishes the current complete
 * cycle. The saved weekly record remains unchanged. */
gboolean radios_player_scope_execution_set_repeat(RadioSPlayerScopeExecution *execution,
    const RadioSAutomationScope *scope, gboolean repeat, GError **error);
/* Read-only definition edit fence. Running/pending schema and programme
 * operations retain captured scopes until Stop. Playlist loads remain free to
 * change AutoDJ mode/target/schema without changing their queue recipient. */
gboolean radios_player_scope_execution_validate_topology(RadioSPlayerScopeExecution *execution,
    RadioSPlayerWorkspace *workspace, const RadioSPlayerTopology *proposed, GError **error);
gboolean radios_player_scope_execution_busy(RadioSPlayerScopeExecution *execution,
    const RadioSAutomationScope *scope);
/* Temporary schema ("Şema yükle"): a saved weekly_programme record that joins
 * the scope's running main schema only inside its own defined slots and hands
 * back to the main schema afterwards. NULL clears it. A running main slot is
 * only interrupted when the temporary schema has a slot right now. */
gboolean radios_player_scope_execution_set_schema_override(RadioSPlayerScopeExecution *execution,
    const RadioSAutomationScope *scope, const gchar *record_id, GError **error);
/* Borrowed id of the scope's temporary schema; NULL without one. */
const gchar *radios_player_scope_execution_schema_override(RadioSPlayerScopeExecution *execution,
    const RadioSAutomationScope *scope);
gboolean radios_player_scope_execution_snapshot(RadioSPlayerScopeExecution *execution,
    const RadioSAutomationScope *scope, RadioSPlayerScopeExecutionSnapshot *snapshot);
#endif
