#include "radios/automation_policy.h"
#include "radios/platform.h"
#include <gio/gio.h>
#include <math.h>
#include <string.h>

#define DAY_MINUTES 1440
#define MAX_CHECKPOINT_KEYS 2048u

typedef struct { guint64 cursor, occurrences; } ScopeState;
typedef struct { guint64 applied; gint64 last; } RuleState;
struct RadioSAutomationPolicyState { guint64 rng; GHashTable *scopes, *rules; };
typedef struct { guint groups, rules; GHashTable *ids; } Validation;
static gboolean invalid(GError **error, const gchar *message)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message); return FALSE; }
static JsonNode *member(JsonObject *o, const gchar *key)
{ return o ? json_object_get_member(o, key) : NULL; }
static const gchar *string(JsonObject *o, const gchar *key, const gchar *fallback)
{
    JsonNode *n = member(o, key);
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : fallback;
}
static gboolean numeric(JsonNode *n)
{ return n && JSON_NODE_HOLDS_VALUE(n) && (json_node_get_value_type(n) == G_TYPE_INT64 || json_node_get_value_type(n) == G_TYPE_DOUBLE); }
static gdouble number(JsonObject *o, const gchar *key, gdouble fallback)
{ JsonNode *n = member(o, key); return numeric(n) ? json_node_get_double(n) : fallback; }
static gint64 integer(JsonObject *o, const gchar *key, gint64 fallback)
{ JsonNode *n = member(o, key); return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_INT64 ? json_node_get_int(n) : fallback; }
static gboolean text(const gchar *value, guint limit)
{
    if (!value || strlen(value) > limit || !g_utf8_validate(value, -1, NULL)) return FALSE;
    for (const gchar *p = value; *p; p = g_utf8_next_char(p)) if (g_unichar_iscntrl(g_utf8_get_char(p))) return FALSE;
    return TRUE;
}
static gboolean keys(JsonObject *o, const gchar *const *allowed, guint count)
{
    GList *names = json_object_get_members(o); gboolean okay = TRUE;
    for (GList *p = names; p; p = p->next) {
        gboolean found = FALSE;
        for (guint i = 0; i < count; ++i) found |= g_str_equal(p->data, allowed[i]);
        if (!found) { okay = FALSE; break; }
    }
    g_list_free(names); return okay;
}
static gboolean int_bound(JsonObject *o, const gchar *key, gint64 low, gint64 high, gboolean required)
{
    JsonNode *n = member(o, key); if (!n) return !required;
    return JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_INT64 && json_node_get_int(n) >= low && json_node_get_int(n) <= high;
}
static gboolean num_bound(JsonObject *o, const gchar *key, gdouble low, gdouble high, gboolean required)
{
    JsonNode *n = member(o, key); if (!n) return !required;
    return numeric(n) && isfinite(json_node_get_double(n)) && json_node_get_double(n) >= low && json_node_get_double(n) <= high;
}
static gboolean probability(JsonObject *o, gdouble *out)
{
    JsonNode *n = member(o, "probability"); *out = 0;
    if (!n || JSON_NODE_HOLDS_NULL(n)) return TRUE;
    if (!num_bound(o, "probability", 0, 100, TRUE)) return FALSE;
    *out = json_node_get_double(n); return TRUE;
}
static gboolean excluded_valid(JsonObject *o)
{
    JsonNode *node = member(o, "excluded_times"); if (!node) return TRUE;
    if (!JSON_NODE_HOLDS_ARRAY(node)) return FALSE;
    JsonArray *a = json_node_get_array(node); if (json_array_get_length(a) > 16) return FALSE;
    const gchar *const allowed[] = {"start_minute", "end_minute"};
    for (guint i = 0; i < json_array_get_length(a); ++i) {
        JsonNode *n = json_array_get_element(a, i); if (!JSON_NODE_HOLDS_OBJECT(n)) return FALSE;
        JsonObject *w = json_node_get_object(n);
        if (!keys(w, allowed, G_N_ELEMENTS(allowed)) || !int_bound(w, "start_minute", 0, 1439, TRUE) || !int_bound(w, "end_minute", 0, 1439, TRUE)) return FALSE;
    }
    return TRUE;
}
/* Optional IANA zone of excluded_times; without it the windows are UTC. */
static gboolean zone_valid(JsonObject *o)
{
    JsonNode *n = member(o, "timezone"); if (!n) return TRUE;
    if (!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_STRING) return FALSE;
    const gchar *name = json_node_get_string(n); if (!text(name, 64) || !name[0]) return FALSE;
    GTimeZone *zone = radios_time_zone_new(name); if (!zone) return FALSE;
    g_time_zone_unref(zone); return TRUE;
}
static gboolean common(JsonObject *o)
{
    gdouble p = 0;
    return g_uuid_string_is_valid(string(o, "id", "")) && probability(o, &p) &&
        int_bound(o, "frequency", 1, 1000, FALSE) && int_bound(o, "interval_minutes", 0, 10080, FALSE) && excluded_valid(o) && zone_valid(o);
}
static gboolean rules_valid(JsonObject *o, const gchar *key, gboolean jingle, Validation *v)
{
    JsonNode *node = member(o, key); if (!node) return TRUE;
    if (!JSON_NODE_HOLDS_ARRAY(node)) return FALSE;
    JsonArray *a = json_node_get_array(node); guint n = json_array_get_length(a);
    if (n > RADIOS_AUTOMATION_POLICY_MAX_CHOICES || v->rules + n > RADIOS_AUTOMATION_POLICY_MAX_RULES) return FALSE;
    v->rules += n; gdouble sum = 0; gboolean okay = TRUE;
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    const gchar *const transition_keys[] = {"id", "mode", "probability", "overlap_ms", "early_seconds", "effect", "frequency", "interval_minutes", "excluded_times", "timezone"};
    const gchar *const jingle_keys[] = {"id", "playlist_id", "player", "probability", "anchor", "seconds", "duck_db", "gain_db", "fade_ms", "frequency", "interval_minutes", "excluded_times", "timezone"};
    for (guint i = 0; i < n && okay; ++i) {
        JsonNode *value = json_array_get_element(a, i); if (!JSON_NODE_HOLDS_OBJECT(value)) { okay = FALSE; break; }
        JsonObject *r = json_node_get_object(value); const gchar *id = string(r, "id", ""); gdouble p = 0;
        okay = common(r) && !g_hash_table_contains(seen, id) && !g_hash_table_contains(v->ids, id) && probability(r, &p);
        if (!okay) break;
        g_hash_table_add(seen, (gpointer)id); g_hash_table_add(v->ids, (gpointer)id); sum += p;
        if (jingle) {
            const gchar *anchor = string(r, "anchor", "");
            okay = keys(r, jingle_keys, G_N_ELEMENTS(jingle_keys)) && g_uuid_string_is_valid(string(r, "playlist_id", "")) &&
                int_bound(r, "player", 0, 31, TRUE) && (g_str_equal(anchor, "after_start") || g_str_equal(anchor, "before_end")) &&
                num_bound(r, "seconds", 0, 86400, TRUE) && num_bound(r, "duck_db", -60, 0, FALSE) &&
                num_bound(r, "gain_db", -60, 12, FALSE) && int_bound(r, "fade_ms", 0, 5000, FALSE);
        } else {
            gint64 mode = integer(r, "mode", -1), overlap = integer(r, "overlap_ms", 0); const gchar *effect = string(r, "effect", "none");
            okay = keys(r, transition_keys, G_N_ELEMENTS(transition_keys)) && int_bound(r, "mode", 0, 3, TRUE) &&
                int_bound(r, "overlap_ms", 0, 30000, FALSE) && num_bound(r, "early_seconds", 0, 3600, FALSE) &&
                (g_str_equal(effect, "none") || g_str_equal(effect, "lowpass")) &&
                (mode != 0 || (!overlap && g_str_equal(effect, "none"))) && (mode != 3 || overlap > 0) &&
                (!g_str_equal(effect, "lowpass") || overlap > 0);
            JsonNode *fx = member(r, "effect"); if (fx && (!JSON_NODE_HOLDS_VALUE(fx) || json_node_get_value_type(fx) != G_TYPE_STRING)) okay = FALSE;
        }
    }
    g_hash_table_unref(seen); return okay && sum <= 100.000000001;
}
static gint scope_index(const gchar *scope)
{
    const gchar *const names[] = {"automation", "playlist", "track"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) if (g_str_equal(scope, names[i])) return (gint)i;
    return -1;
}
static gboolean node_valid(JsonObject *o, guint depth, Validation *v)
{
    if (!o || depth > RADIOS_AUTOMATION_POLICY_MAX_DEPTH) return FALSE;
    const gchar *const allowed[] = {"schema", "seed", "transitions", "jingles", "children"};
    if (!keys(o, allowed, G_N_ELEMENTS(allowed)) || !int_bound(o, "schema", 1, 1, TRUE)) return FALSE;
    JsonNode *seed = member(o, "seed");
    if (seed && (!JSON_NODE_HOLDS_VALUE(seed) || json_node_get_value_type(seed) != G_TYPE_STRING || !text(json_node_get_string(seed), 128))) return FALSE;
    if (!rules_valid(o, "transitions", FALSE, v) || !rules_valid(o, "jingles", TRUE, v)) return FALSE;
    JsonNode *children = member(o, "children"); if (!children) return TRUE;
    if (!JSON_NODE_HOLDS_ARRAY(children)) return FALSE;
    JsonArray *a = json_node_get_array(children); guint n = json_array_get_length(a);
    if (v->groups + n > RADIOS_AUTOMATION_POLICY_MAX_GROUPS) return FALSE;
    v->groups += n; gdouble sums[3] = {0}; gboolean remainder[3] = {0}, okay = TRUE;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL), *group_ids = g_hash_table_new(g_str_hash, g_str_equal);
    const gchar *const group_keys[] = {"id", "scope", "ids", "probability", "policy"};
    for (guint i = 0; i < n && okay; ++i) {
        JsonNode *value = json_array_get_element(a, i); if (!JSON_NODE_HOLDS_OBJECT(value)) { okay = FALSE; break; }
        JsonObject *g = json_node_get_object(value); const gchar *id = string(g, "id", ""); gint s = scope_index(string(g, "scope", ""));
        JsonNode *ids_node = member(g, "ids"), *p = member(g, "policy"); gdouble percent = 0;
        okay = keys(g, group_keys, G_N_ELEMENTS(group_keys)) && g_uuid_string_is_valid(id) && !g_hash_table_contains(group_ids, id) && !g_hash_table_contains(v->ids, id) &&
            s >= 0 && ids_node && JSON_NODE_HOLDS_ARRAY(ids_node) && probability(g, &percent) && (!p || JSON_NODE_HOLDS_OBJECT(p));
        if (!okay) break;
        g_hash_table_add(group_ids, (gpointer)id); g_hash_table_add(v->ids, (gpointer)id); sums[s] += percent;
        JsonArray *ids = json_node_get_array(ids_node); guint count = json_array_get_length(ids);
        if (count > 500 || (!count && remainder[s])) { okay = FALSE; break; }
        if (!count) remainder[s] = TRUE;
        for (guint k = 0; k < count && okay; ++k) {
            JsonNode *target = json_array_get_element(ids, k);
            if (!JSON_NODE_HOLDS_VALUE(target) || json_node_get_value_type(target) != G_TYPE_STRING || !g_uuid_string_is_valid(json_node_get_string(target))) { okay = FALSE; break; }
            gchar *key = g_strdup_printf("%d:%s", s, json_node_get_string(target));
            if (g_hash_table_contains(seen, key)) { g_free(key); okay = FALSE; break; }
            g_hash_table_add(seen, key);
        }
        if (okay && p) okay = node_valid(json_node_get_object(p), depth + 1, v);
    }
    g_hash_table_unref(seen); g_hash_table_unref(group_ids);
    return okay && sums[0] <= 100.000000001 && sums[1] <= 100.000000001 && sums[2] <= 100.000000001;
}
gboolean radios_automation_policy_validate(JsonObject *policy, GError **error)
{
    if (!policy) return TRUE;
    Validation v = {.ids = g_hash_table_new(g_str_hash, g_str_equal)};
    gboolean okay = node_valid(policy, 0, &v); g_hash_table_unref(v.ids);
    return okay || invalid(error, "Olasılık politikası, yüzde toplamı veya kalıtım kapsamı geçersiz.");
}
static JsonNode *deep_copy(JsonNode *n)
{
    if (JSON_NODE_HOLDS_OBJECT(n)) {
        JsonObject *from = json_node_get_object(n), *to = json_object_new(); GList *names = json_object_get_members(from);
        for (GList *p = names; p; p = p->next) json_object_set_member(to, p->data, deep_copy(member(from, p->data)));
        g_list_free(names); JsonNode *copy = json_node_new(JSON_NODE_OBJECT); json_node_take_object(copy, to); return copy;
    }
    if (JSON_NODE_HOLDS_ARRAY(n)) {
        JsonArray *from = json_node_get_array(n), *to = json_array_new();
        for (guint i = 0; i < json_array_get_length(from); ++i) json_array_add_element(to, deep_copy(json_array_get_element(from, i)));
        JsonNode *copy = json_node_new(JSON_NODE_ARRAY); json_node_take_array(copy, to); return copy;
    }
    return json_node_copy(n);
}
static void merge_effective(JsonObject *result, JsonObject *node, const gchar *const ids[3])
{
    const gchar *const fields[] = {"seed", "transitions", "jingles"};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) if (member(node, fields[i])) json_object_set_member(result, fields[i], deep_copy(member(node, fields[i])));
    JsonNode *child = member(node, "children"); if (!child) return;
    JsonArray *children = json_node_get_array(child);
    for (guint scope = 0; scope < 3; ++scope) {
        if (!ids[scope] || !ids[scope][0]) continue;
        JsonObject *matched = NULL, *rest = NULL;
        for (guint i = 0; i < json_array_get_length(children); ++i) {
            JsonObject *g = json_array_get_object_element(children, i);
            if (scope_index(string(g, "scope", "")) != (gint)scope) continue;
            JsonArray *targets = json_node_get_array(member(g, "ids"));
            if (!json_array_get_length(targets)) rest = g;
            for (guint k = 0; k < json_array_get_length(targets); ++k) if (!g_ascii_strcasecmp(ids[scope], json_array_get_string_element(targets, k))) matched = g;
        }
        JsonObject *g = matched ? matched : rest; JsonNode *policy = member(g, "policy");
        if (policy) merge_effective(result, json_node_get_object(policy), ids);
    }
}
JsonObject *radios_automation_policy_effective(JsonObject *policy, const gchar *automation_id,
    const gchar *playlist_id, const gchar *catalogue_entry_id)
{
    if (!radios_automation_policy_validate(policy, NULL)) return NULL;
    JsonObject *result = json_object_new(); json_object_set_int_member(result, "schema", 1);
    const gchar *const ids[] = {automation_id, playlist_id, catalogue_entry_id};
    if (policy) merge_effective(result, policy, ids);
    return result;
}
guint64 radios_automation_policy_random_next(guint64 *state)
{
    if (!state) return 0;
    guint64 x = *state ? *state : 1; x ^= x << 13; x ^= x >> 7; x ^= x << 17; *state = x; return x;
}
static guint64 seed_hash(const gchar *seed)
{
    guint64 hash = G_GUINT64_CONSTANT(14695981039346656037);
    for (const guchar *p = (const guchar *)(seed ? seed : ""); *p; ++p) hash = (hash ^ *p) * G_GUINT64_CONSTANT(1099511628211);
    return hash ? hash : 1;
}
static gboolean restore_map(GHashTable *table, JsonNode *node, gboolean scope)
{
    if (!node) return TRUE;
    if (!JSON_NODE_HOLDS_OBJECT(node)) return FALSE;
    JsonObject *o = json_node_get_object(node); if (json_object_get_size(o) > MAX_CHECKPOINT_KEYS) return FALSE;
    GList *names = json_object_get_members(o); gboolean okay = TRUE;
    for (GList *p = names; p && okay; p = p->next) {
        JsonNode *n = member(o, p->data);
        if (!text(p->data, scope ? 512 : 1024) || !((gchar *)p->data)[0] || !JSON_NODE_HOLDS_OBJECT(n)) { okay = FALSE; break; }
        JsonObject *v = json_node_get_object(n);
        const gchar *const scope_fields[] = {"cursor", "occurrences"}, *const rule_fields[] = {"applied", "last_utc_us"};
        okay = keys(v, scope ? scope_fields : rule_fields, 2) &&
            int_bound(v, scope ? "cursor" : "applied", 0, G_MAXINT64, TRUE) &&
            int_bound(v, scope ? "occurrences" : "last_utc_us", 0, G_MAXINT64, TRUE);
        if (!okay) break;
        if (scope) {
            ScopeState *s = g_new0(ScopeState, 1); s->cursor = (guint64)integer(v, "cursor", 0); s->occurrences = (guint64)integer(v, "occurrences", 0);
            g_hash_table_insert(table, g_strdup(p->data), s);
        } else {
            RuleState *r = g_new0(RuleState, 1); r->applied = (guint64)integer(v, "applied", 0); r->last = integer(v, "last_utc_us", 0);
            g_hash_table_insert(table, g_strdup(p->data), r);
        }
    }
    g_list_free(names); return okay;
}
RadioSAutomationPolicyState *radios_automation_policy_state_new(const gchar *seed, JsonObject *checkpoint, GError **error)
{
    if (!text(seed ? seed : "", 128)) { invalid(error, "Olasılık seed UTF8 metni geçersiz."); return NULL; }
    RadioSAutomationPolicyState *s = g_new0(RadioSAutomationPolicyState, 1); s->rng = seed_hash(seed);
    s->scopes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free); s->rules = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (checkpoint) {
        const gchar *const fields[] = {"schema", "rng", "scopes", "rules"}; const gchar *rng = string(checkpoint, "rng", ""); gchar *end = NULL;
        guint64 parsed = g_ascii_strtoull(rng, &end, 16); gboolean hex = strlen(rng) == 16;
        for (const gchar *p = rng; *p; ++p) hex &= g_ascii_isxdigit(*p);
        if (!keys(checkpoint, fields, G_N_ELEMENTS(fields)) || !int_bound(checkpoint, "schema", 1, 1, TRUE) || !hex || !parsed || !end || *end ||
            !restore_map(s->scopes, member(checkpoint, "scopes"), TRUE) || !restore_map(s->rules, member(checkpoint, "rules"), FALSE)) {
            radios_automation_policy_state_free(s); invalid(error, "Olasılık tüketim checkpoint'i geçersiz."); return NULL;
        }
        s->rng = parsed;
    }
    return s;
}
void radios_automation_policy_state_free(RadioSAutomationPolicyState *state)
{ if (state) { g_hash_table_unref(state->scopes); g_hash_table_unref(state->rules); g_free(state); } }
static JsonObject *map_snapshot(GHashTable *table, gboolean scope)
{
    JsonObject *o = json_object_new(); GList *names = g_list_sort(g_hash_table_get_keys(table), (GCompareFunc)g_strcmp0);
    for (GList *p = names; p; p = p->next) {
        JsonObject *value = json_object_new();
        if (scope) {
            ScopeState *s = g_hash_table_lookup(table, p->data); json_object_set_int_member(value, "cursor", (gint64)MIN(s->cursor, (guint64)G_MAXINT64));
            json_object_set_int_member(value, "occurrences", (gint64)MIN(s->occurrences, (guint64)G_MAXINT64));
        } else {
            RuleState *r = g_hash_table_lookup(table, p->data); json_object_set_int_member(value, "applied", (gint64)MIN(r->applied, (guint64)G_MAXINT64)); json_object_set_int_member(value, "last_utc_us", r->last);
        }
        json_object_set_object_member(o, p->data, value);
    }
    g_list_free(names); return o;
}
JsonObject *radios_automation_policy_state_snapshot(const RadioSAutomationPolicyState *state)
{
    if (!state) return NULL;
    JsonObject *o = json_object_new(); gchar rng[17]; g_snprintf(rng, sizeof rng, "%016" G_GINT64_MODIFIER "x", state->rng);
    json_object_set_int_member(o, "schema", 1); json_object_set_string_member(o, "rng", rng);
    json_object_set_object_member(o, "scopes", map_snapshot(state->scopes, TRUE)); json_object_set_object_member(o, "rules", map_snapshot(state->rules, FALSE)); return o;
}
static gboolean excluded(JsonObject *rule, gint64 utc)
{
    JsonNode *n = member(rule, "excluded_times"); if (!n) return FALSE;
    gint64 seconds = utc / G_USEC_PER_SEC; const gchar *name = string(rule, "timezone", NULL);
    GTimeZone *zone = name ? radios_time_zone_new(name) : NULL;
    if (zone) {
        gint interval = g_time_zone_find_interval(zone, G_TIME_TYPE_UNIVERSAL, seconds);
        if (interval >= 0) seconds += g_time_zone_get_offset(zone, interval);
        g_time_zone_unref(zone);
    }
    JsonArray *a = json_node_get_array(n); gint minute = (gint)((seconds / 60 % DAY_MINUTES + DAY_MINUTES) % DAY_MINUTES);
    for (guint i = 0; i < json_array_get_length(a); ++i) {
        JsonObject *w = json_array_get_object_element(a, i); gint begin = (gint)integer(w, "start_minute", 0), end = (gint)integer(w, "end_minute", 0);
        if (begin == end || (begin < end ? minute >= begin && minute < end : minute >= begin || minute < end)) return TRUE;
    }
    return FALSE;
}
static gint policy_choose(JsonArray *rules, const gboolean *eligible, const gchar *scope_key,
    RadioSAutomationPolicyState *state, gint64 utc_us,gboolean remember)
{
    if (!rules || !state || utc_us < 0 || !text(scope_key, 512) || !scope_key[0]) return -1;
    guint n = json_array_get_length(rules); if (!n || n > 500u) return -1;
    ScopeState *scope = g_hash_table_lookup(state->scopes, scope_key);
    if (!scope) {
        if (g_hash_table_size(state->scopes) >= MAX_CHECKPOINT_KEYS) return -1;
        scope = g_new0(ScopeState, 1); g_hash_table_insert(state->scopes, g_strdup(scope_key), scope);
    }
    if (scope->occurrences == G_MAXINT64) return -1;
    ++scope->occurrences;
    gboolean available[500] = {0}, undefined[500] = {0}; gdouble sum = 0;
    for (guint i = 0; i < n; ++i) {
        JsonNode *node = json_array_get_element(rules, i); if (!JSON_NODE_HOLDS_OBJECT(node)) return -1;
        JsonObject *r = json_node_get_object(node); JsonNode *p = member(r, "probability"); const gchar *id = string(r, "id", "");
        gchar *key = g_strdup_printf("%s/%s", scope_key, id); RuleState *prior = g_hash_table_lookup(state->rules, key); g_free(key);
        guint frequency = (guint)MAX(integer(r, "frequency", 1), 1); gint64 interval = integer(r, "interval_minutes", 0);
        available[i] = (!eligible || eligible[i]) && scope->occurrences % frequency == 0 && !excluded(r, utc_us) &&
            (!prior || !interval || utc_us - prior->last >= interval * 60 * G_USEC_PER_SEC);
        undefined[i] = !p || JSON_NODE_HOLDS_NULL(p);
        if (available[i] && !undefined[i]) sum += number(r, "probability", 0);
    }
    if (sum > 100.000000001) return -1;
    gint selected = -1;
    if (sum > 0) {
        gdouble draw = (gdouble)(radios_automation_policy_random_next(&state->rng) >> 11) * (100.0 / 9007199254740992.0);
        for (guint i = 0; i < n; ++i) if (available[i] && !undefined[i]) {
            gdouble p = number(json_array_get_object_element(rules, i), "probability", 0);
            if (p > 0 && draw < p) { selected = (gint)i; break; }
            draw -= p;
        }
    }
    if (selected < 0) for (guint offset = 0; offset < n; ++offset) {
        guint i = (guint)((scope->cursor + offset) % n);
        if (available[i] && undefined[i]) { selected = (gint)i; scope->cursor = (guint64)i + 1; break; }
    }
    if (selected >= 0 && remember) {
        const gchar *id = string(json_array_get_object_element(rules, (guint)selected), "id", ""); gchar *key = g_strdup_printf("%s/%s", scope_key, id);
        RuleState *r = g_hash_table_lookup(state->rules, key);
        if (!r) {
            if (g_hash_table_size(state->rules) >= MAX_CHECKPOINT_KEYS) { g_free(key); return -1; }
            r = g_new0(RuleState, 1); g_hash_table_insert(state->rules, key, r);
        } else g_free(key);
        if (r->applied < G_MAXINT64) ++r->applied;
        r->last = utc_us;
    }
    return selected;
}

gint radios_automation_policy_choose(JsonArray *rules,const gboolean *eligible,const gchar *scope_key,
    RadioSAutomationPolicyState *state,gint64 utc_us)
{ return policy_choose(rules,eligible,scope_key,state,utc_us,TRUE); }
static gboolean target_contains(JsonArray *ids,const gchar *id)
{
    for (guint i=0;ids && i<json_array_get_length(ids);++i)
        if (!g_ascii_strcasecmp(json_array_get_string_element(ids,i),id)) return TRUE;
    return FALSE;
}
JsonObject *radios_automation_policy_context(JsonObject *policy,const gchar *scope,const gchar *id)
{
    if (scope_index(scope)<0 || !radios_automation_policy_validate(policy,NULL)) return NULL;
    JsonObject *result=json_object_new();json_object_set_int_member(result,"schema",1);
    GList *fields=policy?json_object_get_members(policy):NULL;
    for (GList *it=fields;it;it=it->next) json_object_set_member(result,it->data,deep_copy(member(policy,it->data)));
    g_list_free(fields);JsonArray *groups=policy && member(policy,"children")?json_node_get_array(member(policy,"children")):NULL;
    JsonObject *matched=NULL,*rest=NULL;
    for (guint i=0;groups && i<json_array_get_length(groups);++i) {
        JsonObject *group=json_array_get_object_element(groups,i);if (g_strcmp0(string(group,"scope",""),scope)) continue;
        JsonArray *targets=json_node_get_array(member(group,"ids"));
        if (!json_array_get_length(targets)) rest=group;
        else if (id && *id && target_contains(targets,id)) matched=group;
    }
    JsonObject *group=matched?matched:rest;JsonNode *child=member(group,"policy");
    JsonObject *override=child?json_node_get_object(child):NULL;fields=override?json_object_get_members(override):NULL;
    for (GList *it=fields;it;it=it->next) json_object_set_member(result,it->data,deep_copy(member(override,it->data)));
    g_list_free(fields);
    /* The selected child is now represented at the root. Retaining its
     * policy again under inherited children would duplicate rule UUIDs and
     * reapply the same override during the next context descent. */
    JsonNode *retained_node=member(result,"children");
    JsonArray *retained=retained_node?json_node_get_array(retained_node):NULL;
    for (guint i=0;group && retained && i<json_array_get_length(retained);++i) {
        JsonObject *candidate=json_array_get_object_element(retained,i);
        if (!g_strcmp0(string(candidate,"id",""),string(group,"id",""))) json_object_remove_member(candidate,"policy");
    }
    return result;
}
gint radios_automation_policy_choose_targets(JsonObject *policy,const gchar *scope,
    const gchar *const *ids,const gboolean *eligible,guint count,const gchar *key,
    RadioSAutomationPolicyState *state,gint64 utc)
{
    if (!count || count>500 || !ids || !state || scope_index(scope)<0 || !text(key,400) || !*key) return -1;
    for (guint i=0;i<count;++i) if (!ids[i] || !g_uuid_string_is_valid(ids[i])) return -1;
    JsonArray *groups=policy && member(policy,"children")?json_node_get_array(member(policy,"children")):NULL,*rules=json_array_new();
    GPtrArray *sets=g_ptr_array_new();gboolean legal[257]={FALSE},claimed[500]={FALSE};guint rest=G_MAXUINT;
    for (guint i=0;groups && i<json_array_get_length(groups);++i) {
        JsonObject *group=json_array_get_object_element(groups,i);if (g_strcmp0(string(group,"scope",""),scope)) continue;
        JsonArray *targets=json_node_get_array(member(group,"ids"));guint at=json_array_get_length(rules);
        if (at>=256) { g_ptr_array_unref(sets);json_array_unref(rules);return -1; }
        json_array_add_element(rules,deep_copy(json_array_get_element(groups,i)));g_ptr_array_add(sets,targets);
        if (!json_array_get_length(targets)) { rest=at;continue; }
        for (guint j=0;j<count;++j) if (target_contains(targets,ids[j])) { claimed[j]=TRUE;legal[at]|=!eligible||eligible[j]; }
    }
    if (rest==G_MAXUINT) {
        JsonObject *implicit=json_object_new();json_object_set_string_member(implicit,"id","implicit-remainder");rest=json_array_get_length(rules);
        json_array_add_object_element(rules,implicit);g_ptr_array_add(sets,NULL);
    }
    for (guint j=0;j<count;++j) if (!claimed[j]) legal[rest]|=!eligible||eligible[j];
    gint selected=policy_choose(rules,legal,key,state,utc,FALSE),result=-1;
    /* Playlists and songs must keep playing. When the draw falls into the share
     * of a set with no eligible target now (a repeat gap, an interval, a sweeper
     * right after a sweeper...) or into a remainder nobody can take, the
     * eligible percentages share it in proportion; without any, the first
     * eligible unassigned set. Explicit 0 % and excluded times stay respected.
     * Jingle and transition draws keep their remainder: that means "none". */
    if (selected<0) {
        guint n=json_array_get_length(rules);gdouble sum=0;
        for (guint i=0;i<n;++i) {
            JsonObject *r=json_array_get_object_element(rules,i);JsonNode *p=member(r,"probability");
            if (legal[i] && !excluded(r,utc) && p && !JSON_NODE_HOLDS_NULL(p)) sum+=MAX(0,number(r,"probability",0));
        }
        if (sum>0) {
            gdouble draw=(gdouble)(radios_automation_policy_random_next(&state->rng)>>11)*(sum/9007199254740992.0);
            for (guint i=0;i<n && selected<0;++i) {
                JsonObject *r=json_array_get_object_element(rules,i);JsonNode *p=member(r,"probability");
                gdouble share=p && !JSON_NODE_HOLDS_NULL(p)?number(r,"probability",0):0;
                if (!legal[i] || excluded(r,utc) || share<=0) continue;
                if (draw<share) selected=(gint)i; else draw-=share;
            }
            for (guint i=n;selected<0 && i-->0;) {
                JsonObject *r=json_array_get_object_element(rules,i);JsonNode *p=member(r,"probability");
                if (legal[i] && !excluded(r,utc) && p && !JSON_NODE_HOLDS_NULL(p) && number(r,"probability",0)>0) selected=(gint)i;
            }
        }
        for (guint i=0;selected<0 && i<n;++i) {
            JsonObject *r=json_array_get_object_element(rules,i);JsonNode *p=member(r,"probability");
            if (legal[i] && !excluded(r,utc) && (!p || JSON_NODE_HOLDS_NULL(p))) selected=(gint)i;
        }
    }
    if (selected>=0) {
        JsonArray *members=json_array_new(),*targets=g_ptr_array_index(sets,(guint)selected);guint indices[500],length=0;
        for (guint j=0;j<count;++j) if ((!eligible||eligible[j]) && ((guint)selected==rest?!claimed[j]:target_contains(targets,ids[j]))) {
            JsonObject *choice=json_object_new();json_object_set_string_member(choice,"id",ids[j]);json_array_add_object_element(members,choice);indices[length++]=j;
        }
        gchar *member_key=g_strdup_printf("%s/members/%s",key,string(json_array_get_object_element(rules,(guint)selected),"id",""));
        gint chosen=policy_choose(members,NULL,member_key,state,utc,FALSE);
        if (chosen>=0 && (guint)chosen<length) result=(gint)indices[chosen];
        g_free(member_key);json_array_unref(members);
    }
    g_ptr_array_unref(sets);json_array_unref(rules);return result;
}
