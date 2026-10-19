#include "radios/loudness_policy.h"
#include "radios/platform.h"
#include <string.h>

static gboolean invalid(GError **error)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Ses eşitleme kapsamı veya zaman kuralları geçersiz."); return FALSE; }
static gboolean uuid(const gchar *id)
{
    if (!memchr(id, 0, 37) || !g_uuid_string_is_valid(id)) return FALSE;
    for (guint i = 0; id[i]; ++i) if (g_ascii_isupper(id[i])) return FALSE;
    return TRUE;
}
void radios_audio_loudness_policy_default(RadioSAudioLoudnessPolicy *p)
{
    g_return_if_fail(p); memset(p, 0, sizeof *p);
    p->players = RADIOS_AUDIO_LOUDNESS_ALL_PLAYERS; p->contexts = RADIOS_AUDIO_LOUDNESS_ALL_CONTEXTS;
    g_strlcpy(p->timezone, "UTC", sizeof p->timezone);
}
gboolean radios_audio_loudness_policy_validate(const RadioSAudioLoudnessPolicy *p, GError **error)
{
    if (!p || p->contexts & ~RADIOS_AUDIO_LOUDNESS_ALL_CONTEXTS || (guint)p->phase > RADIOS_AUDIO_LOUDNESS_PHASE_STEADY ||
        (guint)p->automation_mode > RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_SELECTED ||
        (guint)p->pairs_mode > RADIOS_AUDIO_LOUDNESS_PAIRS_SELECTED || p->automation_count > RADIOS_AUDIO_LOUDNESS_MAX_AUTOMATIONS ||
        p->window_count > RADIOS_AUDIO_LOUDNESS_MAX_WINDOWS || !memchr(p->timezone, 0, sizeof p->timezone) ||
        !p->timezone[0] || !g_utf8_validate(p->timezone, -1, NULL)) return invalid(error);
    /* GTimeZone also accepts POSIX/fixed-offset strings. The persisted
     * contract uses named zoneinfo identifiers of the bundled tzdata,
     * matching the service (strict syntax: no '\', ':', "..", ...). */
    GTimeZone *zone = radios_time_zone_identifier_exists(p->timezone) ? radios_time_zone_new(p->timezone) : NULL;
    if (!zone) return invalid(error);
    g_time_zone_unref(zone);
    for (guint i = 0; i < p->automation_count; ++i) {
        if (!uuid(p->automation_ids[i])) return invalid(error);
        for (guint j = 0; j < i; ++j) if (g_str_equal(p->automation_ids[i], p->automation_ids[j])) return invalid(error);
    }
    for (guint i = 0; i < 32; ++i) if (p->pairs[i] & ((guint32)1 << i)) return invalid(error);
    for (guint i = 0; i < p->window_count; ++i)
        if (p->windows[i].days > 127 || p->windows[i].start_minute > 1439 || p->windows[i].end_minute > 1439) return invalid(error);
    return TRUE;
}
gboolean radios_audio_loudness_policy_equal(const RadioSAudioLoudnessPolicy *a, const RadioSAudioLoudnessPolicy *b)
{
    if (!a || !b || a->automation_count > RADIOS_AUDIO_LOUDNESS_MAX_AUTOMATIONS || b->automation_count > RADIOS_AUDIO_LOUDNESS_MAX_AUTOMATIONS ||
        a->window_count > RADIOS_AUDIO_LOUDNESS_MAX_WINDOWS || b->window_count > RADIOS_AUDIO_LOUDNESS_MAX_WINDOWS ||
        !memchr(a->timezone, 0, sizeof a->timezone) || !memchr(b->timezone, 0, sizeof b->timezone) || a->players != b->players || a->contexts != b->contexts || a->phase != b->phase ||
        a->automation_mode != b->automation_mode || a->automation_count != b->automation_count || a->pairs_mode != b->pairs_mode ||
        a->schedule_enabled != b->schedule_enabled || g_strcmp0(a->timezone, b->timezone) || a->window_count != b->window_count) return FALSE;
    for (guint i = 0; i < 32; ++i) if (a->pairs[i] != b->pairs[i]) return FALSE;
    for (guint i = 0; i < a->automation_count; ++i)
        if (!memchr(a->automation_ids[i], 0, 37) || !memchr(b->automation_ids[i], 0, 37) ||
            g_strcmp0(a->automation_ids[i], b->automation_ids[i])) return FALSE;
    for (guint i = 0; i < a->window_count; ++i)
        if (a->windows[i].days != b->windows[i].days || a->windows[i].start_minute != b->windows[i].start_minute ||
            a->windows[i].end_minute != b->windows[i].end_minute) return FALSE;
    return TRUE;
}
static JsonArray *array(JsonObject *o, const gchar *key)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    return n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : NULL;
}
static const gchar *string(JsonNode *n)
{ return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : NULL; }
static gboolean integer(JsonNode *n, gint64 *out)
{
    if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64) return FALSE;
    *out = json_node_get_int(n); return TRUE;
}
static gboolean exact_keys(JsonObject *o, const gchar *const *keys, guint count)
{
    if (!o || json_object_get_size(o) != count) return FALSE;
    for (guint i = 0; i < count; ++i) if (!json_object_has_member(o, keys[i])) return FALSE;
    return TRUE;
}
static gint choice(JsonObject *o, const gchar *key, const gchar *const *choices, guint count)
{
    const gchar *value = string(json_object_get_member(o, key));
    for (guint i = 0; value && i < count; ++i) if (g_str_equal(value, choices[i])) return (gint)i;
    return -1;
}
static gboolean mask(JsonArray *a, guint maximum, guint32 *out)
{
    if (!a || json_array_get_length(a) > maximum) return FALSE;
    *out = 0;
    for (guint i = 0; i < json_array_get_length(a); ++i) {
        gint64 n; if (!integer(json_array_get_element(a, i), &n) || n < 1 || n > maximum) return FALSE;
        guint32 bit = (guint32)1 << (n - 1); if (*out & bit) return FALSE; *out |= bit;
    }
    return TRUE;
}
gboolean radios_audio_loudness_policy_from_json(RadioSAudioLoudnessPolicy *out, JsonObject *o, GError **error)
{
    const gchar *const keys[] = {"schema_version", "players", "contexts", "phase", "automation_mode", "automation_ids",
        "pairs_mode", "pairs", "schedule_enabled", "timezone", "windows"};
    const gchar *const phases[] = {"always", "transition", "steady"};
    const gchar *const automations[] = {"all", "assigned", "selected"};
    const gchar *const pair_modes[] = {"all", "selected"};
    gint64 schema;
    if (!out || !exact_keys(o, keys, G_N_ELEMENTS(keys)) || !integer(json_object_get_member(o, "schema_version"), &schema) || schema != 1) return invalid(error);
    RadioSAudioLoudnessPolicy p; radios_audio_loudness_policy_default(&p);
    if (!mask(array(o, "players"), 32, &p.players)) return invalid(error);
    JsonArray *contexts = array(o, "contexts"); p.contexts = 0;
    if (!contexts || json_array_get_length(contexts) > 3) return invalid(error);
    const gchar *const names[] = {"manual", "automation", "autodj"};
    for (guint i = 0; i < json_array_get_length(contexts); ++i) {
        const gchar *name = string(json_array_get_element(contexts, i)); guint bit = 0;
        for (guint j = 0; name && j < 3; ++j) if (g_str_equal(name, names[j])) bit = 1u << j;
        if (!bit || p.contexts & bit) return invalid(error);
        p.contexts |= bit;
    }
    gint phase = choice(o, "phase", phases, 3), automation = choice(o, "automation_mode", automations, 3), pairs = choice(o, "pairs_mode", pair_modes, 2);
    if (phase < 0 || automation < 0 || pairs < 0) return invalid(error);
    p.phase = (RadioSAudioLoudnessPhase)phase; p.automation_mode = (RadioSAudioLoudnessAutomationMode)automation; p.pairs_mode = (RadioSAudioLoudnessPairsMode)pairs;
    JsonArray *ids = array(o, "automation_ids"), *directions = array(o, "pairs"), *windows = array(o, "windows");
    if (!ids || json_array_get_length(ids) > RADIOS_AUDIO_LOUDNESS_MAX_AUTOMATIONS || !directions || json_array_get_length(directions) > 992 ||
        !windows || json_array_get_length(windows) > RADIOS_AUDIO_LOUDNESS_MAX_WINDOWS) return invalid(error);
    p.automation_count = json_array_get_length(ids);
    for (guint i = 0; i < p.automation_count; ++i) {
        const gchar *id = string(json_array_get_element(ids, i)); if (!id || strlen(id) != 36) return invalid(error);
        g_strlcpy(p.automation_ids[i], id, sizeof p.automation_ids[i]);
    }
    const gchar *const direction_keys[] = {"from", "to"};
    for (guint i = 0; i < json_array_get_length(directions); ++i) {
        JsonNode *node = json_array_get_element(directions, i);
        JsonObject *direction = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL; gint64 from, to;
        if (!exact_keys(direction, direction_keys, 2) || !integer(json_object_get_member(direction, "from"), &from) ||
            !integer(json_object_get_member(direction, "to"), &to) || from < 1 || from > 32 || to < 1 || to > 32 || from == to) return invalid(error);
        guint32 bit = (guint32)1 << (to - 1); if (p.pairs[from - 1] & bit) return invalid(error); p.pairs[from - 1] |= bit;
    }
    JsonNode *enabled = json_object_get_member(o, "schedule_enabled");
    const gchar *zone = string(json_object_get_member(o, "timezone"));
    if (!enabled || !JSON_NODE_HOLDS_VALUE(enabled) || json_node_get_value_type(enabled) != G_TYPE_BOOLEAN || !zone || strlen(zone) > 127) return invalid(error);
    p.schedule_enabled = json_node_get_boolean(enabled); g_strlcpy(p.timezone, zone, sizeof p.timezone); p.window_count = json_array_get_length(windows);
    const gchar *const window_keys[] = {"days", "start_minute", "end_minute"};
    for (guint i = 0; i < p.window_count; ++i) {
        JsonNode *node = json_array_get_element(windows, i); JsonObject *window = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
        gint64 start, end; guint32 days;
        if (!exact_keys(window, window_keys, 3) || !mask(array(window, "days"), 7, &days) ||
            !integer(json_object_get_member(window, "start_minute"), &start) || !integer(json_object_get_member(window, "end_minute"), &end) ||
            start < 0 || start > 1439 || end < 0 || end > 1439) return invalid(error);
        p.windows[i] = (RadioSAudioLoudnessWindow){days, (guint)start, (guint)end};
    }
    if (!radios_audio_loudness_policy_validate(&p, error)) return FALSE;
    *out = p; return TRUE;
}
static JsonArray *bits(guint32 mask_value, guint maximum)
{
    JsonArray *a = json_array_new();
    for (guint i = 0; i < maximum; ++i) if (mask_value & ((guint32)1 << i)) json_array_add_int_element(a, i + 1);
    return a;
}
JsonObject *radios_audio_loudness_policy_to_json(const RadioSAudioLoudnessPolicy *p)
{
    if (!radios_audio_loudness_policy_validate(p, NULL)) return NULL;
    JsonObject *o = json_object_new(); json_object_set_int_member(o, "schema_version", 1);
    json_object_set_array_member(o, "players", bits(p->players, 32));
    const gchar *const contexts[] = {"manual", "automation", "autodj"}; JsonArray *a = json_array_new();
    for (guint i = 0; i < 3; ++i) if (p->contexts & (1u << i)) json_array_add_string_element(a, contexts[i]);
    json_object_set_array_member(o, "contexts", a);
    const gchar *const phases[] = {"always", "transition", "steady"}, *const automations[] = {"all", "assigned", "selected"}, *const pair_modes[] = {"all", "selected"};
    json_object_set_string_member(o, "phase", phases[p->phase]); json_object_set_string_member(o, "automation_mode", automations[p->automation_mode]);
    a = json_array_new(); for (guint i = 0; i < p->automation_count; ++i) json_array_add_string_element(a, p->automation_ids[i]);
    json_object_set_array_member(o, "automation_ids", a); json_object_set_string_member(o, "pairs_mode", pair_modes[p->pairs_mode]);
    a = json_array_new();
    for (guint from = 0; from < 32; ++from) for (guint to = 0; to < 32; ++to) if (p->pairs[from] & ((guint32)1 << to)) {
        JsonObject *pair = json_object_new(); json_object_set_int_member(pair, "from", from + 1); json_object_set_int_member(pair, "to", to + 1); json_array_add_object_element(a, pair);
    }
    json_object_set_array_member(o, "pairs", a); json_object_set_boolean_member(o, "schedule_enabled", p->schedule_enabled);
    json_object_set_string_member(o, "timezone", p->timezone); a = json_array_new();
    for (guint i = 0; i < p->window_count; ++i) {
        JsonObject *window = json_object_new(); json_object_set_array_member(window, "days", bits(p->windows[i].days, 7));
        json_object_set_int_member(window, "start_minute", p->windows[i].start_minute); json_object_set_int_member(window, "end_minute", p->windows[i].end_minute);
        json_array_add_object_element(a, window);
    }
    json_object_set_array_member(o, "windows", a); return o;
}
gboolean radios_audio_loudness_policy_unrestricted(const RadioSAudioLoudnessPolicy *p)
{
    return p && p->players == RADIOS_AUDIO_LOUDNESS_ALL_PLAYERS && p->contexts == RADIOS_AUDIO_LOUDNESS_ALL_CONTEXTS &&
        p->phase == RADIOS_AUDIO_LOUDNESS_PHASE_ALWAYS && p->automation_mode == RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_ALL &&
        p->pairs_mode == RADIOS_AUDIO_LOUDNESS_PAIRS_ALL && !p->schedule_enabled;
}
gboolean radios_audio_loudness_policy_time_matches(const RadioSAudioLoudnessPolicy *p, guint day, guint minute)
{
    if (!p || p->window_count > RADIOS_AUDIO_LOUDNESS_MAX_WINDOWS) return FALSE;
    if (!p->schedule_enabled) return TRUE;
    if (day < 1 || day > 7 || minute > 1439) return FALSE;
    guint current = 1u << (day - 1), previous = 1u << ((day + 5) % 7);
    for (guint i = 0; i < p->window_count; ++i) {
        const RadioSAudioLoudnessWindow *w = &p->windows[i];
        if (w->start_minute == w->end_minute) { if (w->days & current) return TRUE; }
        else if (w->start_minute < w->end_minute) { if ((w->days & current) && minute >= w->start_minute && minute < w->end_minute) return TRUE; }
        else if (((w->days & current) && minute >= w->start_minute) || ((w->days & previous) && minute < w->end_minute)) return TRUE;
    }
    return FALSE;
}
gboolean radios_audio_loudness_policy_matches(const RadioSAudioLoudnessPolicy *p, const RadioSAudioLoudnessExecution *e, gboolean time)
{
    if (!p || !e || !time || p->automation_count > RADIOS_AUDIO_LOUDNESS_MAX_AUTOMATIONS) return FALSE;
    if (e->deck >= 32) return radios_audio_loudness_policy_unrestricted(p);
    if (!(p->players & ((guint32)1 << e->deck)) || !(p->contexts & e->context) ||
        (p->phase == RADIOS_AUDIO_LOUDNESS_PHASE_TRANSITION && !e->transition) ||
        (p->phase == RADIOS_AUDIO_LOUDNESS_PHASE_STEADY && e->transition)) return FALSE;
    if (p->automation_mode == RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_ASSIGNED && (!e->assigned || !e->automation_id || !*e->automation_id)) return FALSE;
    if (p->automation_mode == RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_SELECTED) {
        gboolean found = FALSE;
        for (guint i = 0; e->automation_id && i < p->automation_count; ++i) found |= g_str_equal(e->automation_id, p->automation_ids[i]);
        if (!found) return FALSE;
    }
    if (p->pairs_mode == RADIOS_AUDIO_LOUDNESS_PAIRS_SELECTED) {
        if (e->source < 0 || e->source >= 32 || e->target < 0 || e->target >= 32 || e->source == e->target ||
            (e->deck != (guint)e->source && e->deck != (guint)e->target) || !(p->pairs[e->source] & ((guint32)1 << e->target))) return FALSE;
    }
    return TRUE;
}
