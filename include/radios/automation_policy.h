#ifndef RADIOS_AUTOMATION_POLICY_H
#define RADIOS_AUTOMATION_POLICY_H

#include <json-glib/json-glib.h>

G_BEGIN_DECLS
#define RADIOS_AUTOMATION_POLICY_MAX_DEPTH 8u
#define RADIOS_AUTOMATION_POLICY_MAX_GROUPS 256u
#define RADIOS_AUTOMATION_POLICY_MAX_RULES 512u
#define RADIOS_AUTOMATION_POLICY_MAX_CHOICES 16u

typedef struct RadioSAutomationPolicyState RadioSAutomationPolicyState;
/* Optional schema1 policy. NULL preserves legacy configuration. Percentages
 * are finite0..100, explicit0 disabled, omitted/null unspecified. Comparator
 * siblings share one scope; their explicit total cannot exceed100. Empty ids
 * is the unique remainder group at that scope. Excluded windows may cross
 * midnight; they are UTC unless the rule names an IANA "timezone". */
gboolean radios_automation_policy_validate(JsonObject *policy, GError **error);
/* Owned deep merged policy: matching automation, playlist and catalogue-entry
 * children override only fields explicitly present. Native occurrence entry
 * UUID is intentionally a different identity from catalogue_entry_id. */
JsonObject *radios_automation_policy_effective(JsonObject *policy,
    const gchar *automation_id, const gchar *playlist_id, const gchar *catalogue_entry_id);
/* State is private to a preparation. Attach snapshots to actual-consumption
 * checkpoints; a preview must never replace the consumed state. */
RadioSAutomationPolicyState *radios_automation_policy_state_new(const gchar *seed,
    JsonObject *checkpoint, GError **error);
void radios_automation_policy_state_free(RadioSAutomationPolicyState *state);
JsonObject *radios_automation_policy_state_snapshot(const RadioSAutomationPolicyState *state);
/* Validated transition/jingle/group choices. Caller eligibility preserves
 * verified media, copyright and inherited repeat/calendar bounds. Remaining
 * mass advances a linear cursor only among unspecified eligible choices;
 * an unrepresented remainder returns-1 rather than renormalizing percentages.
 * Frequency/interval/UTC exclusions are applied before the draw. No I/O. */
gint radios_automation_policy_choose(JsonArray *rules, const gboolean *eligible,
    const gchar *scope_key, RadioSAutomationPolicyState *state, gint64 utc_us);
/* Canonical set draw followed by linear selection within its eligible
 * members. ids are real catalogue UUIDs, count<=500. Explicit empty ids is
 * remainder; when absent an implicit unspecified remainder is added. Caller
 * supplies a validated policy and a scope key<=400 bytes. No I/O. */
gint radios_automation_policy_choose_targets(JsonObject *policy,const gchar *scope,
    const gchar *const *ids,const gboolean *eligible,guint count,const gchar *scope_key,
    RadioSAutomationPolicyState *state,gint64 utc_us);
/* Owned one-level scope overlay, retaining children for the next target
 * decision. Omitted fields inherit; explicit empty arrays disable that field. */
JsonObject *radios_automation_policy_context(JsonObject *policy,const gchar *scope,const gchar *id);
/* Common deterministic primitive for native probabilistic policy decisions. */
guint64 radios_automation_policy_random_next(guint64 *state);
G_END_DECLS
#endif
