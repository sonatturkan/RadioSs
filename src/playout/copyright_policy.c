#include "radios/copyright_policy.h"
#include <string.h>

static const gchar *text(JsonObject *o, const gchar *key)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : "";
}
static gboolean boolean(JsonObject *o, const gchar *key)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_BOOLEAN && json_node_get_boolean(n);
}
static gint64 stamp(const gchar *value)
{
    GDateTime *date = value && strchr(value, 'T') ? g_date_time_new_from_iso8601(value, NULL) : NULL;
    if (!date) return 0;
    gint64 result = g_date_time_to_unix(date) * G_USEC_PER_SEC + g_date_time_get_microsecond(date);
    g_date_time_unref(date); return result;
}
static gchar *key(const gchar *value)
{
    gchar *normal = g_utf8_normalize(value ? value : "", -1, G_NORMALIZE_DEFAULT_COMPOSE);
    GString *mapped = g_string_new(NULL); gboolean space = FALSE;
    for (const gchar *p = normal ? normal : ""; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        if (g_unichar_isspace(c)) { space = mapped->len > 0; continue; }
        if (space) { g_string_append_c(mapped, ' '); space = FALSE; }
        g_string_append_unichar(mapped, c == 'I' ? 0x131 : c == 0x130 ? 'i' : c);
    }
    gchar *result = g_utf8_casefold(mapped->str, -1); g_string_free(mapped, TRUE); g_free(normal); return result;
}
gboolean radios_copyright_policy_validate(JsonObject *policy, GError **error)
{
    if (!policy) return TRUE;
    JsonNode *schema = json_object_get_member(policy, "schema"), *restriction = json_object_get_member(policy, "restrict");
    JsonNode *rules_node = json_object_get_member(policy, "rules");
    if (!schema || !JSON_NODE_HOLDS_VALUE(schema) || json_node_get_value_type(schema) != G_TYPE_INT64 || json_node_get_int(schema) != 1 ||
        !restriction || !JSON_NODE_HOLDS_VALUE(restriction) || json_node_get_value_type(restriction) != G_TYPE_BOOLEAN ||
        (!g_str_equal(text(policy, "unregistered"), "allow") && !g_str_equal(text(policy, "unregistered"), "deny")) ||
        !rules_node || !JSON_NODE_HOLDS_ARRAY(rules_node) || json_array_get_length(json_node_get_array(rules_node)) > 1000) goto invalid;
    JsonArray *rules = json_node_get_array(rules_node);
    for (guint i = 0; i < json_array_get_length(rules); ++i) {
        JsonNode *node = json_array_get_element(rules, i);
        if (!node || !JSON_NODE_HOLDS_OBJECT(node)) goto invalid;
        JsonObject *rule = json_node_get_object(node); JsonNode *enabled = json_object_get_member(rule, "enabled");
        if (!enabled || !JSON_NODE_HOLDS_VALUE(enabled) || json_node_get_value_type(enabled) != G_TYPE_BOOLEAN ||
            !g_uuid_string_is_valid(text(rule, "id"))) goto invalid;
        const gchar *selectors[] = {"artist", "album", "title"}; gboolean selected = FALSE;
        for (guint j = 0; j < G_N_ELEMENTS(selectors); ++j) {
            JsonNode *value = json_object_get_member(rule, selectors[j]);
            if (!value || !JSON_NODE_HOLDS_VALUE(value) || json_node_get_value_type(value) != G_TYPE_STRING ||
                !g_utf8_validate(json_node_get_string(value), -1, NULL)) goto invalid;
            gchar *normal = key(json_node_get_string(value)); selected |= *normal != 0; g_free(normal);
        }
        gint64 start = stamp(text(rule, "starts_at")), end = stamp(text(rule, "ends_at"));
        if (!selected || start <= 0 || end <= start) goto invalid;
    }
    return TRUE;
invalid:
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Telif bilgileri veya geçerlilik tarihleri geçersiz."); return FALSE;
}
RadioSAudioCopyrightWindow radios_copyright_policy_window(JsonObject *policy,
    const gchar *artist, const gchar *album, const gchar *title)
{
    RadioSAudioCopyrightWindow result = {0};
    if (!policy || !boolean(policy, "restrict")) return result;
    result.restricted = TRUE;
    JsonNode *node = json_object_get_member(policy, "rules");
    JsonArray *rules = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
    gchar *values[] = {key(artist), key(album), key(title)};
    const gchar *names[] = {"artist", "album", "title"};
    gboolean any_match = FALSE;
    for (guint i = 0; rules && i < json_array_get_length(rules); ++i) {
        JsonObject *rule = json_array_get_object_element(rules, i);
        if (!boolean(rule, "enabled")) continue;
        gboolean matched = TRUE, selected = FALSE;
        for (guint j = 0; j < G_N_ELEMENTS(names); ++j) {
            gchar *selector = key(text(rule, names[j]));
            if (*selector) { selected = TRUE; matched &= g_str_equal(selector, values[j]); }
            g_free(selector);
        }
        if (!matched || !selected) continue;
        any_match = TRUE;
        gint64 start = stamp(text(rule, "starts_at")), end = stamp(text(rule, "ends_at"));
        result.starts_at_utc_us = MAX(result.starts_at_utc_us, start);
        result.ends_at_utc_us = result.ends_at_utc_us ? MIN(result.ends_at_utc_us, end) : end;
        result.denied |= start <= 0 || end <= start;
    }
    for (guint j = 0; j < G_N_ELEMENTS(values); ++j) g_free(values[j]);
    result.denied |= !any_match && g_str_equal(text(policy, "unregistered"), "deny");
    result.denied |= result.ends_at_utc_us && result.starts_at_utc_us >= result.ends_at_utc_us;
    return result;
}
gboolean radios_copyright_window_allowed(const RadioSAudioCopyrightWindow *window, gint64 utc_us)
{
    return !window || !window->restricted || (!window->denied &&
        (!window->starts_at_utc_us || utc_us >= window->starts_at_utc_us) &&
        (!window->ends_at_utc_us || utc_us < window->ends_at_utc_us));
}
