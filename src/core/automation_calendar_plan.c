#include "radios/automation_calendar_plan.h"
#include "radios/transition_preset.h"
#include "radios/media_geometry.h"
#include "radios/copyright_policy.h"
#include "radios/automation_policy.h"
#include "radios/programme_model.h"
#include "radios/platform.h"
#include <stdio.h>
#include <math.h>
#include <string.h>

typedef struct {
    JsonObject *rule, *copyright_policy;
    const gchar *source_name; /* borrowed authenticated playlist metadata */
    GPtrArray *slots;
    GArray *pools[3];
    guint slot_cursor, cursors[3], transition_cursor;
    guint64 rng[3], transition_rng, track_rng;
    GPtrArray *policies;
    GArray *plays, *track_order;
    GHashTable *indices;
    guint track_cursor, current_visit, excluded_rows;
    /* Schema flow (kılavuz §04): a live event step plays no media; a source
     * whose date window excludes the programme date is never selected;
     * outer_order is the source's 1-based position in its rotation. */
    gboolean live, date_blocked;
    guint outer_order;
} RotationSource;

static const gchar *text(JsonObject *object, const gchar *name)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ?
        json_node_get_string(node) : "";
}
static gint64 integer(JsonObject *object, const gchar *name, gint64 fallback)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64 ?
        json_node_get_int(node) : fallback;
}
static gboolean boolean(JsonObject *object, const gchar *name, gboolean *value)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN) return FALSE;
    *value = json_node_get_boolean(node); return TRUE;
}
static gboolean flag(JsonObject *object, const gchar *name)
{ gboolean value = FALSE; boolean(object, name, &value); return value; }
static gboolean media_eligible(JsonObject *row)
{ return row && flag(row, json_object_has_member(row, "media_eligible") ? "media_eligible" : "eligible"); }
static JsonObject *member_object(JsonObject *object, const gchar *name)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonArray *member_array(JsonObject *object, const gchar *name)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static JsonObject *array_object(JsonArray *array, guint index)
{
    JsonNode *node = array && index < json_array_get_length(array) ? json_array_get_element(array, index) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonNode *clone(JsonObject *object)
{
    JsonNode *input = json_node_new(JSON_NODE_OBJECT); json_node_set_object(input, object);
    gchar *serialized = json_to_string(input, FALSE); JsonParser *parser = json_parser_new();
    gboolean loaded = json_parser_load_from_data(parser, serialized, -1, NULL);
    JsonNode *result = loaded ? json_node_copy(json_parser_get_root(parser)) : NULL;
    g_object_unref(parser); g_free(serialized); json_node_free(input); return result;
}
static gboolean fail(GError **error, const gchar *message)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message); return FALSE; }
/* Diagnostics inspect only a bounded prefix, including malformed UTF-8.
 * Untrusted labels cannot produce multiline or unbounded error messages. */
static gchar *diagnostic_text(const gchar *value, guint limit)
{
    GString *out = g_string_new(NULL);
    for (guint i = 0; value && *value && i < limit; ++i) {
        gunichar c = g_utf8_get_char_validated(value, -1);
        if (c == (gunichar)-1 || c == (gunichar)-2) { g_string_append_c(out, '?'); ++value; }
        else {
            g_string_append_unichar(out, g_unichar_iscntrl(c) || g_unichar_isspace(c) ? ' ' : c);
            value = g_utf8_next_char(value);
        }
    }
    g_strstrip(out->str); return g_string_free(out, FALSE);
}
static void event_error_context(GError **error, JsonObject *event)
{
    if (!error || !*error) return;
    gchar *name = diagnostic_text(text(event, "title"), 80), *date = diagnostic_text(text(event, "date"), 16);
    gint64 start = integer(event, "start_minute", -1), end = integer(event, "end_minute", -1);
    gchar begin[6] = "--:--", finish[6] = "--:--";
    if (start >= 0 && start <= 1439) g_snprintf(begin, sizeof begin, "%02d:%02d", (gint)(start / 60), (gint)(start % 60));
    if (end >= 0 && end <= 1440) g_snprintf(finish, sizeof finish, "%02d:%02d", (gint)(end / 60), (gint)(end % 60));
    g_prefix_error(error, "%s · %s %s–%s: ", *name ? name : "Adsız blok", *date ? date : "Tarih yok", begin, finish);
    g_free(name); g_free(date);
}
static gboolean identifier_add(GHashTable *identities, const gchar *identifier)
{
    if (!g_uuid_string_is_valid(identifier)) return FALSE;
    gchar *canonical = g_ascii_strdown(identifier, -1);
    if (g_hash_table_contains(identities, canonical)) { g_free(canonical); return FALSE; }
    g_hash_table_add(identities, canonical); return TRUE;
}
static const gchar *rotation_media_kind(JsonObject *rule)
{
    return json_object_has_member(rule, "media_kind") ? text(rule, "media_kind") : "all";
}
static gboolean rotation_media_kind_valid(JsonObject *rule)
{
    const gchar *kind = rotation_media_kind(rule);
    const gchar *allowed[] = {"all", "music", "jingle", "advertisement", "sweeper"};
    for (guint i = 0; i < G_N_ELEMENTS(allowed); ++i)
        if (g_str_equal(kind, allowed[i])) return TRUE;
    return FALSE;
}
static gboolean deck_ids_valid(JsonArray *decks)
{
    guint32 seen = 0;
    if (!decks || !json_array_get_length(decks) || json_array_get_length(decks) > 32) return FALSE;
    for (guint i = 0; i < json_array_get_length(decks); ++i) {
        JsonNode *node = json_array_get_element(decks, i);
        if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64) return FALSE;
        gint64 id = json_node_get_int(node);
        if (id < 1 || id > 32 || (seen & ((guint32)1 << (id - 1)))) return FALSE;
        seen |= (guint32)1 << (id - 1);
    }
    return TRUE;
}
static gboolean autodj_valid(JsonObject *source)
{
    if (!json_object_has_member(source, "autodj")) return TRUE;
    JsonObject *routing = member_object(source, "autodj");
    const gchar *kinds[] = {"music", "advertisement", "jingle", "sweeper"};
    if (!routing || json_object_get_size(routing) != G_N_ELEMENTS(kinds)) return FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i)
        if (!deck_ids_valid(member_array(routing, kinds[i]))) return FALSE;
    return TRUE;
}
static gboolean track_rules_valid(JsonObject *source);
static gboolean source_options_valid(JsonObject *source, JsonArray *presets);
static gboolean settings_valid(JsonObject *settings, gboolean override, JsonArray *parent_presets);
static gboolean live_valid(JsonObject *live, gboolean allow_title, gboolean allow_schema_rule);
static gboolean entry_settings_valid(JsonObject *settings);
static gboolean source_is_live(JsonObject *source);
static gboolean iso_date_valid(const gchar *date);
static gboolean range(JsonObject *object, const gchar *name, gint64 low, gint64 high);
static gboolean kinds_contain(JsonArray *kinds, const gchar *kind);
static void policy_strip_jingles(JsonObject *policy);
static JsonObject *policy_merge(JsonObject *parent, JsonObject *override);
static gboolean valid_event(JsonObject *event, GError **error)
{
    if (event && json_object_has_member(event, "timezone")) {
        const gchar *name = text(event, "timezone"); GTimeZone *zone = *name ? radios_time_zone_new(name) : NULL;
        if (!zone) return fail(error, "Program saat dilimi geçersiz.");
        g_time_zone_unref(zone);
    }
    if (event && json_object_has_member(event, "probability_policy") && (!member_object(event, "probability_policy") || !radios_automation_policy_validate(member_object(event, "probability_policy"), error))) return FALSE;
    /* "Live event" flag of the programme (kılavuz §04) and the runtime entry
     * order the scope driver passes for programme_entry (§03); never saved. */
    if (event && json_object_has_member(event, "live") && !live_valid(member_object(event, "live"), FALSE, FALSE))
        return fail(error, "Programın Live event ayarı geçersiz.");
    if (event && json_object_has_member(event, "schema_entry_source_order") && !range(event, "schema_entry_source_order", 1, 50))
        return fail(error, "Programa giriş sırası 1..50 olmalı.");
    if (event && json_object_has_member(event, "schema_entry_settings") && !entry_settings_valid(member_object(event, "schema_entry_settings")))
        return fail(error, "Programlar arası şema giriş ayarı geçersiz.");
    const gchar *date = text(event, "date"), *title = text(event, "title");
    guint year = 0, month = 0, day = 0;
    gint64 start = integer(event, "start_minute", -1), end = integer(event, "end_minute", -1);
    JsonArray *rules = member_array(event, "rotations");
    const gchar *event_mode = event && json_object_has_member(event, "rotation_selection_mode") ? text(event, "rotation_selection_mode") : "linear";
    if (!event || (!g_str_equal(event_mode, "linear") && !g_str_equal(event_mode, "weighted")) || !g_uuid_string_is_valid(text(event, "id")) || !g_utf8_validate(title, -1, NULL) ||
        !*title || g_utf8_strlen(title, -1) > 160 || strlen(date) != 10 ||
        !g_regex_match_simple("^[0-9]{4}-[0-9]{2}-[0-9]{2}$", date, 0, 0) ||
        sscanf(date, "%u-%u-%u", &year, &month, &day) != 3 || year > 9999 ||
        !g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year) ||
        start < 0 || start > 1439 || end <= start || end > 1440 || !rules ||
        !json_array_get_length(rules) || json_array_get_length(rules) > 50)
    {
        const gchar *reason = "Takvim programı, saat aralığı veya rotasyon satırları geçersiz.";
        if (!*title || !g_utf8_validate(title, -1, NULL) || g_utf8_strlen(title, -1) > 160)
            reason = "Program adı boş, geçersiz UTF-8 veya 160 karakterden uzun.";
        else if (strlen(date) != 10 || sscanf(date, "%u-%u-%u", &year, &month, &day) != 3 || year > 9999 ||
                 !g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year))
            reason = "Program tarihi geçersiz.";
        else if (start < 0 || start > 1439 || end <= start || end > 1440)
            reason = "Program saat aralığı geçersiz; bitiş başlangıçtan sonra olmalı.";
        else if (!rules || !json_array_get_length(rules)) reason = "Programa rotasyon atanmamış.";
        else if (json_array_get_length(rules) > 50) reason = "Programın rotasyon kaynağı sayısı 50 sınırını aşıyor.";
        return fail(error, reason);
    }
    GHashTable *identities = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    identifier_add(identities, text(event, "id")); gboolean valid = TRUE;
    for (guint i = 0; valid && i < json_array_get_length(rules); ++i) {
        JsonObject *rule = array_object(rules, i); gboolean value;
        gint64 frequency = integer(rule, "frequency", 0);
        JsonArray *presets = member_array(member_object(rule, "calendar_settings"), "autodj_presets");
        /* A live step belongs to a named rotation and has no playlist. */
        valid = rule && identifier_add(identities, text(rule, "id")) &&
            (source_is_live(rule) ? json_object_has_member(rule, "calendar_group_id") : g_uuid_string_is_valid(text(rule, "playlist_id"))) &&
            frequency >= 1 && frequency <= 100 &&
            rotation_media_kind_valid(rule) && autodj_valid(rule) &&
            (!json_object_has_member(rule, "calendar_group_id") ||
             (g_uuid_string_is_valid(text(rule, "calendar_group_id")) && g_uuid_string_is_valid(text(rule, "calendar_rotation_id")) &&
              source_options_valid(rule, presets) && settings_valid(member_object(rule, "calendar_settings"), FALSE, NULL) &&
              (g_str_equal(text(rule, "calendar_selection_mode"), "linear") || g_str_equal(text(rule, "calendar_selection_mode"), "weighted"))));
        const gchar *flags[] = {"random", "track_random", "advert_random", "sweeper_random"};
        for (guint field = 0; valid && field < G_N_ELEMENTS(flags); ++field)
            valid = boolean(rule, flags[field], &value);
    }
    g_hash_table_unref(identities);
    return valid ? TRUE : fail(error, "Rotasyon kimliği, sıklığı, medya türü veya rastgelelik alanı geçersiz.");
}
static gboolean range(JsonObject *object, const gchar *name, gint64 low, gint64 high)
{ gint64 value = integer(object, name, low - 1); return value >= low && value <= high; }
/* settings.transitions (sözleşme 18.10, C8 ile): entries without "kind" are
 * the rotation's own distribution (1..4, distinct modes, positive total; a
 * kind's remainder resolves into it). Entries with "kind" give a content
 * kind its percentage per transition type or per designed preset
 * ({kind, preset_id, mode 1, overlap_ms 0, weight}); Σ ≤ 100 per kind. */
static gboolean is_string(JsonObject *object, const gchar *name);
static gboolean transition_shape_valid(JsonObject *choice, guint extra)
{
    gint64 mode = integer(choice, "mode", -1);
    gboolean has_effect = json_object_has_member(choice, "effect");
    const gchar *effect = has_effect ? text(choice, "effect") : "none";
    return json_object_get_size(choice) == (has_effect ? 4u : 3u) + extra && mode >= 0 && mode <= 3 &&
        range(choice, "overlap_ms", 0, 30000) && (g_str_equal(effect, "none") || g_str_equal(effect, "lowpass")) &&
        !(mode == 0 && (integer(choice, "overlap_ms", 0) || !g_str_equal(effect, "none"))) &&
        !(mode == 3 && !integer(choice, "overlap_ms", 0)) && !(g_str_equal(effect, "lowpass") && !integer(choice, "overlap_ms", 0));
}
static JsonObject *settings_preset(JsonObject *settings, const gchar *id)
{
    JsonArray *presets = member_array(settings, "autodj_presets");
    for (guint i = 0; id && presets && i < json_array_get_length(presets); ++i)
        if (!g_ascii_strcasecmp(text(array_object(presets, i), "id"), id)) return array_object(presets, i);
    return NULL;
}
static gboolean transition_choices_valid(JsonArray *choices, JsonObject *settings, gboolean override)
{
    guint seen = 0, total = 0, base = 0, kind_total[RADIOS_TRANSITION_KIND_COUNT] = {0}, kind_modes[RADIOS_TRANSITION_KIND_COUNT] = {0};
    if (!choices || !json_array_get_length(choices) || json_array_get_length(choices) > 64) return FALSE;
    GHashTable *presets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL); gboolean valid = TRUE;
    for (guint i = 0; valid && i < json_array_get_length(choices); ++i) {
        JsonObject *choice = array_object(choices, i); gint64 mode = integer(choice, "mode", -1);
        if (!choice) { valid = FALSE; break; }
        if (!json_object_has_member(choice, "kind")) {
            valid = transition_shape_valid(choice, 0) && range(choice, "weight", 0, 100) && !(seen & (1u << mode)) && ++base <= 4;
            if (valid) { seen |= 1u << mode; total += (guint)integer(choice, "weight", 0); }
            continue;
        }
        gint index = is_string(choice, "kind") ? radios_transition_kind_index(text(choice, "kind")) : -1;
        if (index < 0 || !range(choice, "weight", 1, 100)) { valid = FALSE; break; }
        kind_total[index] += (guint)integer(choice, "weight", 0);
        if (json_object_has_member(choice, "preset_id")) {
            const gchar *id = is_string(choice, "preset_id") ? text(choice, "preset_id") : "";
            gchar *key = g_ascii_strdown(id, -1), *scoped = g_strdup_printf("%d/%s", index, key); g_free(key);
            valid = !override && json_object_get_size(choice) == 5 && mode == 1 && range(choice, "overlap_ms", 0, 0) &&
                settings_preset(settings, id) && radios_automation_preset_kind_usable(settings, id, text(choice, "kind")) &&
                !g_hash_table_contains(presets, scoped);
            g_hash_table_add(presets, scoped);
        } else {
            valid = transition_shape_valid(choice, 1) && !(kind_modes[index] & (1u << mode));
            if (valid) kind_modes[index] |= 1u << mode;
        }
    }
    for (guint k = 0; valid && k < RADIOS_TRANSITION_KIND_COUNT; ++k) valid = kind_total[k] <= 100;
    g_hash_table_unref(presets);
    return valid && base && total > 0;
}
static const gchar *const autodj_kinds[] = {"music", "advertisement", "jingle", "sweeper"};
/* AutoDJ policy by content kind: {kind: {"mode": 0..3, "overlap_ms": n}}.
 * SYNC beatmatch (mode 3) may add "sync_percent" 0..100: the share of that
 * kind's transitions made with SYNC beatmatch; the rest crossfade. */
static gboolean kind_transitions_valid(JsonObject *policy)
{
    if (!policy || json_object_get_size(policy) != G_N_ELEMENTS(autodj_kinds)) return FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(autodj_kinds); ++i) {
        JsonObject *choice = member_object(policy, autodj_kinds[i]);
        gint64 mode = integer(choice, "mode", -1), overlap = integer(choice, "overlap_ms", -1);
        gboolean percent = choice && json_object_has_member(choice, "sync_percent");
        if (!choice || json_object_get_size(choice) != (percent ? 3u : 2u) || mode < 0 || mode > 3 || !range(choice, "overlap_ms", 0, 30000) ||
            (mode == 0 && overlap) || (mode == 3 && !overlap) || (percent && (mode != 3 || !range(choice, "sync_percent", 0, 100)))) return FALSE;
    }
    return TRUE;
}
static gboolean kind_players_valid(JsonObject *players)
{
    if (!players || json_object_get_size(players) != G_N_ELEMENTS(autodj_kinds)) return FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(autodj_kinds); ++i) if (!deck_ids_valid(member_array(players, autodj_kinds[i]))) return FALSE;
    return TRUE;
}
/* Rotation level only: distinct AutoDJ content kinds the rotation never plays. */
static gboolean excluded_kinds_valid(JsonArray *kinds)
{
    if (!kinds || json_array_get_length(kinds) > G_N_ELEMENTS(autodj_kinds)) return FALSE;
    for (guint i = 0; i < json_array_get_length(kinds); ++i) {
        JsonNode *node = json_array_get_element(kinds, i); gboolean known = FALSE;
        if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING) return FALSE;
        for (guint k = 0; k < G_N_ELEMENTS(autodj_kinds); ++k) known |= g_str_equal(json_node_get_string(node), autodj_kinds[k]);
        if (!known) return FALSE;
        for (guint j = 0; j < i; ++j)
            if (g_str_equal(json_node_get_string(json_array_get_element(kinds, j)), json_node_get_string(node))) return FALSE;
    }
    return TRUE;
}
/* Schema flow records (kılavuz §02-§04); same contract as desktop_automation_calendar.py. */
static const gchar *const step_kinds[] = {"music", "talk", "dj", "advertisement", "jingle", "sweeper"};
static const gchar *const live_labels[] = {"dj", "talk", "music"};
static gboolean is_string(JsonObject *object, const gchar *name)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING;
}
static gboolean one_of(const gchar *value, const gchar *const *choices, guint count)
{
    for (guint i = 0; i < count; ++i) if (g_str_equal(value, choices[i])) return TRUE;
    return FALSE;
}
static gboolean iso_date_valid(const gchar *date)
{
    guint year = 0, month = 0, day = 0;
    return strlen(date) == 10 && g_regex_match_simple("^[0-9]{4}-[0-9]{2}-[0-9]{2}$", date, 0, 0) &&
        sscanf(date, "%u-%u-%u", &year, &month, &day) == 3 && year > 0 && year <= 9999 &&
        g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year);
}
static gboolean schema_transition_valid(JsonObject *transition)
{ return transition && json_object_get_size(transition) == 2 && range(transition, "mode", 0, 3) && range(transition, "overlap_ms", 0, 30000); }
static gboolean preset_known(JsonArray *presets, const gchar *id)
{
    for (guint i = 0; presets && i < json_array_get_length(presets); ++i)
        if (!g_ascii_strcasecmp(text(array_object(presets, i), "id"), id)) return TRUE;
    return FALSE;
}
/* A record carries preset_id (of the rotation's own autodj_presets) or a
 * transition, never both; with neither it only takes part in the order match. */
static gboolean schema_choice_valid(JsonObject *row, JsonArray *presets, guint fixed)
{
    gboolean has_preset = json_object_has_member(row, "preset_id"), has_transition = json_object_has_member(row, "transition");
    if (has_preset && has_transition) return FALSE;
    if (json_object_get_size(row) != fixed + has_preset + has_transition) return FALSE;
    if (has_preset && (!g_uuid_string_is_valid(text(row, "preset_id")) || !preset_known(presets, text(row, "preset_id")))) return FALSE;
    return !has_transition || schema_transition_valid(member_object(row, "transition"));
}
static gboolean mixer_channels_valid(JsonArray *channels)
{
    if (!channels || json_array_get_length(channels) > 32) return FALSE;
    guint32 seen = 0;
    for (guint i = 0; i < json_array_get_length(channels); ++i) {
        JsonNode *node = json_array_get_element(channels, i);
        if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64) return FALSE;
        gint64 channel = json_node_get_int(node);
        if (channel < 1 || channel > 32 || (seen & ((guint32)1 << (channel - 1)))) return FALSE;
        seen |= (guint32)1 << (channel - 1);
    }
    return TRUE;
}
static gboolean automation_steps_valid(JsonArray *steps, JsonArray *presets)
{
    if (!steps || json_array_get_length(steps) > 32) return FALSE;
    guint64 seen[2] = {0, 0};
    for (guint i = 0; i < json_array_get_length(steps); ++i) {
        JsonObject *step = array_object(steps, i); gint64 order = integer(step, "order", 0);
        gboolean channels = step && json_object_has_member(step, "channels");
        if (!step || order < 1 || order > 99 || (seen[order / 64] & (G_GUINT64_CONSTANT(1) << (order % 64))) ||
            !is_string(step, "kind") || !one_of(text(step, "kind"), step_kinds, G_N_ELEMENTS(step_kinds)) ||
            (channels && (!one_of(text(step, "kind"), live_labels, G_N_ELEMENTS(live_labels)) || !mixer_channels_valid(member_array(step, "channels")))) ||
            !schema_choice_valid(step, presets, 2 + channels)) return FALSE;
        seen[order / 64] |= G_GUINT64_CONSTANT(1) << (order % 64);
    }
    return TRUE;
}
static gboolean link_transitions_valid(JsonArray *links, JsonArray *presets)
{
    if (!links || json_array_get_length(links) > 50) return FALSE;
    for (guint i = 0; i < json_array_get_length(links); ++i) {
        JsonObject *link = array_object(links, i);
        if (!link || !g_uuid_string_is_valid(text(link, "from_source_id")) || !g_uuid_string_is_valid(text(link, "to_source_id")) ||
            !schema_choice_valid(link, presets, 2)) return FALSE;
        for (guint j = 0; j < i; ++j) {
            JsonObject *other = array_object(links, j);
            if (!g_ascii_strcasecmp(text(other, "from_source_id"), text(link, "from_source_id")) &&
                !g_ascii_strcasecmp(text(other, "to_source_id"), text(link, "to_source_id"))) return FALSE;
        }
    }
    return TRUE;
}
static gboolean programme_entry_valid(JsonObject *entry, JsonArray *presets)
{
    if (!entry || !range(entry, "source_order", 1, 50)) return FALSE;
    gboolean has_alternatives = json_object_has_member(entry, "alternatives");
    if (!schema_choice_valid(entry, presets, 1u + has_alternatives)) return FALSE;
    if (!has_alternatives) return TRUE;
    JsonArray *alternatives = member_array(entry, "alternatives"); gint64 total = 0;
    if (!alternatives || json_array_get_length(alternatives) > 8) return FALSE;
    for (guint i = 0; i < json_array_get_length(alternatives); ++i) {
        JsonObject *row = array_object(alternatives, i);
        if (!row || !range(row, "weight", 0, 100) || !range(row, "source_order", 1, 50) || !schema_choice_valid(row, presets, 2)) return FALSE;
        total += integer(row, "weight", 0);
    }
    return total <= 100;
}
/* Runtime-only handoff from the preceding programme. Carry its actual
 * entry/alternatives and preset definitions rather than only a fixed order;
 * the next programme's independently named presets must not replace them. */
static gboolean entry_settings_valid(JsonObject *settings)
{
    if (!settings) return FALSE;
    gboolean has_presets = json_object_has_member(settings, "autodj_presets");
    JsonArray *presets = member_array(settings, "autodj_presets");
    return json_object_get_size(settings) == 1u + has_presets &&
        (!has_presets || radios_transition_presets_valid(presets)) &&
        programme_entry_valid(member_object(settings, "programme_entry"), presets);
}
/* Live event step/flag: label dj|talk|music, distinct mixer channels 1..32
 * (may be empty), automation_id uuid|"" (a live event schema, resolved by the
 * calendar), end_rule manual|duration (programme blocks also know "schema"). */
static gboolean live_valid(JsonObject *live, gboolean allow_title, gboolean allow_schema_rule)
{
    if (!live) return FALSE;
    gboolean has_title = json_object_has_member(live, "title");
    if ((has_title && !allow_title) || json_object_get_size(live) != 5u + has_title) return FALSE;
    if (has_title && (!is_string(live, "title") || !g_utf8_validate(text(live, "title"), -1, NULL) || g_utf8_strlen(text(live, "title"), -1) > 160)) return FALSE;
    if (!is_string(live, "label") || !one_of(text(live, "label"), live_labels, G_N_ELEMENTS(live_labels))) return FALSE;
    JsonArray *channels = member_array(live, "channels"); guint32 seen = 0;
    if (!channels || json_array_get_length(channels) > 32) return FALSE;
    for (guint i = 0; i < json_array_get_length(channels); ++i) {
        JsonNode *node = json_array_get_element(channels, i);
        if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64) return FALSE;
        gint64 channel = json_node_get_int(node);
        if (channel < 1 || channel > 32 || (seen & ((guint32)1 << (channel - 1)))) return FALSE;
        seen |= (guint32)1 << (channel - 1);
    }
    if (!is_string(live, "automation_id") || (*text(live, "automation_id") && !g_uuid_string_is_valid(text(live, "automation_id")))) return FALSE;
    const gchar *rule = text(live, "end_rule");
    if (!is_string(live, "end_rule") || !(g_str_equal(rule, "manual") || g_str_equal(rule, "duration") || (allow_schema_rule && g_str_equal(rule, "schema")))) return FALSE;
    if (!range(live, "duration_ms", 0, 86400000)) return FALSE;
    return !g_str_equal(rule, "duration") || integer(live, "duration_ms", 0) > 0;
}
static gboolean source_is_live(JsonObject *source)
{ return source && g_str_equal(text(source, "source_type"), "live"); }
static gboolean settings_valid(JsonObject *settings, gboolean override, JsonArray *parent_presets)
{
    if (!settings) return FALSE;
    JsonArray *presets = json_object_has_member(settings, "autodj_presets") ? member_array(settings, "autodj_presets") : parent_presets;
    GList *members = json_object_get_members(settings); gboolean valid = TRUE;
    for (GList *it = members; valid && it; it = it->next) {
        const gchar *field = it->data;
        if (g_str_equal(field, "selection_mode"))
            valid = !override && (g_str_equal(text(settings, field), "linear") || g_str_equal(text(settings, field), "weighted"));
        else if (g_str_equal(field, "transition_selection_mode"))
            valid = g_str_equal(text(settings, field), "linear") || g_str_equal(text(settings, field), "weighted");
        else if (g_str_equal(field, "transitions")) valid = transition_choices_valid(member_array(settings, field), settings, override);
        else if (g_str_equal(field, "kind_transitions")) valid = kind_transitions_valid(member_object(settings, field));
        else if (g_str_equal(field, "autodj_players")) valid = kind_players_valid(member_object(settings, field));
        else if (g_str_equal(field, "autodj_presets")) valid = !override && radios_transition_presets_valid(member_array(settings, field));
        else if (g_str_equal(field, "autodj_automation"))
            valid = !override && radios_transition_automation_valid(member_object(settings, field), member_array(settings, "autodj_presets"));
        else if (g_str_equal(field, "probability_policy")) valid = member_object(settings, field) && radios_automation_policy_validate(member_object(settings, field), NULL);
        else if (g_str_equal(field, "automation_schema")) { gboolean schema; valid = !override && boolean(settings, field, &schema); }
        else if (g_str_equal(field, "excluded_kinds")) valid = !override && excluded_kinds_valid(member_array(settings, field));
        else if (g_str_equal(field, "automation_steps")) valid = automation_steps_valid(member_array(settings, field), presets);
        else if (g_str_equal(field, "link_transitions")) valid = !override && link_transitions_valid(member_array(settings, field), presets);
        else if (g_str_equal(field, "programme_entry")) valid = !override && programme_entry_valid(member_object(settings, field), presets);
        else if (g_str_equal(field, "soft_end_seconds")) valid = range(settings, field, 0, 600);
        else if (g_str_equal(field, "relax_gaps")) { gboolean relax; valid = boolean(settings, field, &relax); }
        else if (g_str_equal(field, "track_gap_minutes") || g_str_equal(field, "artist_gap_minutes") || g_str_equal(field, "album_gap_minutes"))
            valid = range(settings, field, 0, 10080);
        else valid = FALSE;
    }
    g_list_free(members);
    const gchar *required[] = {"selection_mode", "track_gap_minutes", "artist_gap_minutes", "album_gap_minutes", "transitions"};
    for (guint i = 0; valid && !override && i < G_N_ELEMENTS(required); ++i)
        valid = json_object_has_member(settings, required[i]);
    return valid;
}
static gboolean track_rules_valid(JsonObject *source)
{
    if (json_object_has_member(source, "track_selection_mode") &&
        !g_str_equal(text(source, "track_selection_mode"), "linear") &&
        !g_str_equal(text(source, "track_selection_mode"), "weighted")) return FALSE;
    if (!json_object_has_member(source, "track_rules")) return TRUE;
    JsonArray *rules = member_array(source, "track_rules");
    if (!rules || json_array_get_length(rules) > 500) return FALSE;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gboolean valid = TRUE;
    for (guint i = 0; valid && i < json_array_get_length(rules); ++i) {
        JsonObject *rule = array_object(rules, i);
        valid = rule && json_object_get_size(rule) == 8 && identifier_add(seen, text(rule, "entry_id")) &&
            range(rule, "weight", 0, 100) && range(rule, "order", 0, 4999) &&
            range(rule, "frequency", 1, 100) && range(rule, "max_plays", 0, 200) &&
            range(rule, "start_minute", 0, 1439) && range(rule, "end_minute", 1, 1440) &&
            integer(rule, "start_minute", 0) < integer(rule, "end_minute", 1440) &&
            range(rule, "interval_minutes", 0, 10080);
    }
    g_hash_table_unref(seen); return valid;
}
static gboolean source_options_valid(JsonObject *source, JsonArray *presets)
{
    const gchar *bounded[] = {"weight", "count", "start_minute", "end_minute", "interval_minutes"};
    const gint64 low[] = {0, 1, 0, 1, 0}, high[] = {100, 100, 1439, 1440, 10080};
    for (guint i = 0; i < G_N_ELEMENTS(bounded); ++i)
        if (json_object_has_member(source, bounded[i]) && !range(source, bounded[i], low[i], high[i])) return FALSE;
    /* source_type "live" (kılavuz §04 live event): no playlist or song rules, a
     * live object instead. The date window (§04 step 3) is an inclusive pair. */
    if (json_object_has_member(source, "source_type") && (!is_string(source, "source_type") ||
        (!g_str_equal(text(source, "source_type"), "playlist") && !g_str_equal(text(source, "source_type"), "live")))) return FALSE;
    if (source_is_live(source) ? (json_object_has_member(source, "playlist_id") || json_object_has_member(source, "track_rules") ||
        json_object_has_member(source, "track_selection_mode") || !live_valid(member_object(source, "live"), TRUE, FALSE)) :
        json_object_has_member(source, "live")) return FALSE;
    const gchar *dates[] = {"start_date", "end_date"};
    for (guint i = 0; i < G_N_ELEMENTS(dates); ++i)
        if (json_object_has_member(source, dates[i]) && (!is_string(source, dates[i]) || !iso_date_valid(text(source, dates[i])))) return FALSE;
    if (json_object_has_member(source, "start_date") && json_object_has_member(source, "end_date") &&
        strcmp(text(source, "start_date"), text(source, "end_date")) > 0) return FALSE;
    return (!json_object_has_member(source, "calendar_group_weight") || range(source, "calendar_group_weight", 0, 100)) &&
        autodj_valid(source) && track_rules_valid(source) && integer(source, "start_minute", 0) < integer(source, "end_minute", 1440) &&
        (!json_object_has_member(source, "settings") || settings_valid(member_object(source, "settings"), TRUE, presets));
}
static JsonObject *live_schema_lookup(GHashTable *definitions, JsonObject *live)
{
    if (!live || !*text(live, "automation_id")) return NULL;
    gchar *key = g_ascii_strdown(text(live, "automation_id"), -1);
    JsonObject *definition = g_hash_table_lookup(definitions, key); g_free(key);
    return definition;
}
static gchar *expanded_identifier(const gchar *assignment, const gchar *source)
{
    gchar *a = g_ascii_strdown(assignment, -1), *b = g_ascii_strdown(source, -1);
    gchar *input = g_strdup_printf("RadioS calendar source:%s:%s", a, b);
    gchar *hash = g_compute_checksum_for_string(G_CHECKSUM_SHA256, input, -1);
    hash[12] = '5'; hash[16] = "89ab"[(g_ascii_xdigit_value(hash[16]) & 3)];
    gchar *uuid = g_strdup_printf("%.8s-%.4s-%.4s-%.4s-%.12s", hash, hash + 8, hash + 12, hash + 16, hash + 20);
    g_free(a); g_free(b); g_free(input); g_free(hash); return uuid;
}
JsonNode *radios_automation_calendar_resolve_event(JsonObject *calendar, JsonObject *event, GError **error)
{
    JsonArray *library = member_array(calendar, "rotation_library"), *assignments = member_array(event, "rotations");
    if (!calendar || integer(calendar, "schema", 0) != 1 || !event || !assignments || json_array_get_length(assignments) > 50 ||
        (json_object_has_member(calendar, "rotation_library") && !library) || (library && json_array_get_length(library) > 200)) {
        fail(error, "Rotasyon kitaplığı veya program atamaları geçersiz."); event_error_context(error, event); return NULL;
    }
    if (!json_array_get_length(assignments)) {
        fail(error, "Programa rotasyon atanmamış."); event_error_context(error, event); return NULL;
    }
    const gchar *failure_reason = "Rotasyon tanımı, atama kimliği, kaynak ayarı veya genişletilmiş kaynak sayısı geçersiz.";
    gboolean sourceless_rotation = FALSE;
    GHashTable *definitions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *identities = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gboolean valid = identifier_add(identities, text(event, "id"));
    for (guint i = 0; valid && library && i < json_array_get_length(library); ++i) {
        JsonObject *definition = array_object(library, i); JsonArray *sources = member_array(definition, "sources");
        const gchar *name = text(definition, "name");
        JsonArray *presets = member_array(member_object(definition, "settings"), "autodj_presets");
        valid = definition && identifier_add(identities, text(definition, "id")) && *name && g_utf8_validate(name, -1, NULL) &&
            g_utf8_strlen(name, -1) <= 160 && sources && json_array_get_length(sources) <= 50 && settings_valid(member_object(definition, "settings"), FALSE, NULL);
        for (guint j = 0; valid && j < json_array_get_length(sources); ++j) {
            JsonObject *source = array_object(sources, j);
            valid = source && identifier_add(identities, text(source, "id")) && source_options_valid(source, presets) &&
                (source_is_live(source) || g_uuid_string_is_valid(text(source, "playlist_id"))) && range(source, "frequency", 1, 100) && rotation_media_kind_valid(source);
            const gchar *flags[] = {"random", "track_random", "advert_random", "sweeper_random"}; gboolean value;
            for (guint field = 0; valid && field < G_N_ELEMENTS(flags); ++field) valid = boolean(source, flags[field], &value);
        }
        /* Link defaults name this definition's own sources (both ends). */
        JsonArray *links = valid ? member_array(member_object(definition, "settings"), "link_transitions") : NULL;
        for (guint j = 0; valid && links && j < json_array_get_length(links); ++j) {
            JsonObject *link = array_object(links, j); gboolean from = FALSE, to = FALSE;
            for (guint k = 0; k < json_array_get_length(sources); ++k) {
                const gchar *id = text(array_object(sources, k), "id");
                from |= !g_ascii_strcasecmp(id, text(link, "from_source_id")); to |= !g_ascii_strcasecmp(id, text(link, "to_source_id"));
            }
            if (!from || !to) { valid = FALSE; failure_reason = "Bağlantı geçişi bu rotasyonun kaynaklarını adlandırmalı."; }
        }
        if (valid) g_hash_table_insert(definitions, g_ascii_strdown(text(definition, "id"), -1), definition);
    }
    /* A live step or the programme's Live event flag may name the special live
     * event schema: a library definition with automation_schema. Its inner
     * records and presets travel with the expanded source for the planner. */

    JsonNode *result = valid ? clone(event) : NULL; JsonArray *expanded = json_array_new();
    for (guint i = 0; valid && i < json_array_get_length(assignments); ++i) {
        JsonObject *assignment = array_object(assignments, i);
        valid = assignment && identifier_add(identities, text(assignment, "id")); if (!valid) break;
        if (!json_object_has_member(assignment, "rotation_id")) {
            JsonNode *copy = clone(assignment); json_array_add_element(expanded, copy); continue;
        }
        gchar *key = g_ascii_strdown(text(assignment, "rotation_id"), -1); JsonObject *definition = g_hash_table_lookup(definitions, key); g_free(key);
        gboolean has_weight = json_object_has_member(assignment, "weight");
        if (!definition) {
            failure_reason = !library || !json_array_get_length(library) ? "Atanan rotasyon için rotasyon kitaplığı boş." : "Atanan rotasyon kitaplıkta bulunamadı.";
            valid = FALSE; break;
        }
        if (json_object_get_size(assignment) != 2u + has_weight ||
            (has_weight && !range(assignment, "weight", 0, 100))) { valid = FALSE; break; }
        JsonArray *sources = member_array(definition, "sources");
        sourceless_rotation |= !json_array_get_length(sources);
        for (guint j = 0; j < json_array_get_length(sources); ++j) {
            JsonObject *original = array_object(sources, j); JsonNode *copy = clone(original); JsonObject *source = json_node_get_object(copy);
            JsonNode *settings_copy = clone(member_object(definition, "settings")); JsonObject *settings = json_node_get_object(settings_copy);
            JsonObject *overrides = member_object(original, "settings"); GList *fields = overrides ? json_object_get_members(overrides) : NULL;
            for (GList *field = fields; field; field = field->next) {
                /* The source's inner records are one side of the three-way
                 * schema match, not an override of the active schema. Keep
                 * them in source.settings while calendar_settings retains
                 * the schema's assigned automation for the common order. */
                if (!strcmp(field->data, "automation_steps")) continue;
                if (!strcmp(field->data, "probability_policy"))
                    json_object_set_object_member(settings, field->data, policy_merge(member_object(settings, field->data), member_object(overrides, field->data)));
                else json_object_set_member(settings, field->data, json_node_copy(json_object_get_member(overrides, field->data)));
            }
            g_list_free(fields);
            if (kinds_contain(member_array(settings, "excluded_kinds"), "jingle")) policy_strip_jingles(member_object(settings, "probability_policy"));
            if (source_is_live(original) && *text(member_object(original, "live"), "automation_id")) {
                JsonObject *schema = live_schema_lookup(definitions, member_object(original, "live"));
                if (!schema || !flag(member_object(schema, "settings"), "automation_schema")) {
                    valid = FALSE; failure_reason = "Live event şeması kitaplıkta bulunamadı ya da otomasyon şeması değil."; json_node_free(copy); json_node_free(settings_copy); break;
                }
                JsonObject *carried = json_object_new(); JsonObject *schema_settings = member_object(schema, "settings");
                const gchar *members[] = {"automation_steps", "autodj_presets"};
                for (guint m = 0; m < G_N_ELEMENTS(members); ++m)
                    if (json_object_has_member(schema_settings, members[m])) json_object_set_member(carried, members[m], json_node_copy(json_object_get_member(schema_settings, members[m])));
                json_object_set_object_member(source, "calendar_live_schema", carried);
            }
            gchar *id = expanded_identifier(text(assignment, "id"), text(original, "id"));
            json_object_set_string_member(source, "id", id); g_free(id);
            json_object_set_string_member(source, "calendar_group_id", text(assignment, "id"));
            json_object_set_int_member(source, "calendar_group_weight", integer(assignment, "weight", 100));
            json_object_set_string_member(source, "calendar_rotation_id", text(definition, "id"));
            json_object_set_string_member(source, "calendar_source_id", text(original, "id"));
            json_object_set_string_member(source, "calendar_selection_mode", text(settings, "selection_mode"));
            json_object_set_member(source, "calendar_settings", settings_copy);
            json_array_add_element(expanded, copy);
        }
        valid = valid && json_array_get_length(expanded) <= 50;
    }
    if (valid && *text(member_object(event, "live"), "automation_id")) {
        JsonObject *schema = live_schema_lookup(definitions, member_object(event, "live"));
        if (!schema || !flag(member_object(schema, "settings"), "automation_schema")) { valid = FALSE; failure_reason = "Programın Live event şeması kitaplıkta bulunamadı ya da otomasyon şeması değil."; }
    }
    if (valid && result) {
        if (json_object_has_member(calendar, "timezone"))
            json_object_set_member(json_node_get_object(result), "timezone", json_node_copy(json_object_get_member(calendar, "timezone")));
        guint source_count = json_array_get_length(expanded);
        json_object_set_array_member(json_node_get_object(result), "rotations", expanded); expanded = NULL;
        if (!source_count && sourceless_rotation) { valid = FALSE; failure_reason = "Atanan rotasyonda playlist kaynağı yok."; }
        else valid = valid_event(json_node_get_object(result), error);
    }
    g_hash_table_unref(definitions); g_hash_table_unref(identities); if (expanded) json_array_unref(expanded);
    if (!valid) {
        if (!error || !*error) fail(error, failure_reason);
        event_error_context(error, event); g_clear_pointer(&result, json_node_free);
    }
    return result;
}

static gchar *bounded_text(const gchar *value, guint characters)
{
    if (!g_utf8_validate(value, -1, NULL)) return g_strdup("");
    return g_utf8_substring(value, 0, MIN(g_utf8_strlen(value, -1), (glong)characters));
}

JsonNode *radios_automation_calendar_prepare_config(JsonObject *event,
    JsonObject *legacy_defaults, GError **error)
{
    if (!valid_event(event, error)) return NULL;
    JsonNode *result = legacy_defaults ? clone(legacy_defaults) : json_node_new(JSON_NODE_OBJECT);
    if (!legacy_defaults) json_node_take_object(result, json_object_new());
    if (!result) { fail(error, "Rotasyon ayarları kopyalanamadı."); return NULL; }
    JsonObject *config = json_node_get_object(result);
    if (!legacy_defaults) {
        const gchar *integers[] = {"sourceMode", "trackMode", "transition", "boundaryTransition", "deckMode", "fallbackMode", "trackGap", "artistGap", "albumGap"};
        for (guint i = 0; i < G_N_ELEMENTS(integers); ++i) json_object_set_int_member(config, integers[i], 0);
        json_object_set_int_member(config, "jingleEvery", 3);
        json_object_set_double_member(config, "overlap", 0); json_object_set_double_member(config, "boundaryOverlap", 0);
        json_object_set_boolean_member(config, "jingleDry", TRUE); json_object_set_boolean_member(config, "requireMetadata", FALSE);
        json_object_set_null_member(config, "jingleDeck");
        JsonArray *decks = json_array_new(); json_array_add_string_element(decks, "1"); json_array_add_string_element(decks, "2");
        json_object_set_array_member(config, "decks", decks); json_object_set_string_member(config, "seed", text(event, "id"));
    }
    GDateTime *utc_start = g_date_time_new_from_unix_utc(radios_automation_calendar_event_time(event, integer(event, "start_minute", 0)) / G_USEC_PER_SEC);
    GDateTime *utc_end = g_date_time_new_from_unix_utc(radios_automation_calendar_event_time(event, integer(event, "end_minute", 0)) / G_USEC_PER_SEC);
    gchar *name = bounded_text(text(event, "title"), 80), *start = g_date_time_format(utc_start, "%H:%M");
    gchar *end = g_date_time_format(utc_end, "%H:%M"), *utc_day = g_date_time_format(utc_start, "%Y-%m-%d");
    json_object_set_string_member(config, "name", name); json_object_set_string_member(config, "planDate", utc_day);
    g_free(utc_day); g_date_time_unref(utc_end); g_date_time_unref(utc_start);
    json_object_set_string_member(config, "startTime", start); json_object_set_string_member(config, "windowStart", start);
    json_object_set_string_member(config, "windowEnd", end); json_object_set_int_member(config, "previewCount", 200);
    json_object_set_int_member(config, "sourceMode", 0); json_object_set_int_member(config, "trackMode", 0);
    json_object_set_int_member(config, "fallbackMode", 0); json_object_set_boolean_member(config, "jingleEnabled", FALSE);
    json_object_set_boolean_member(config, "includeProgramme", FALSE);
    json_object_set_null_member(config, "jinglePlaylist"); json_object_set_null_member(config, "fallbackPlaylist");
    JsonArray *rules = member_array(event, "rotations"), *sources = json_array_new(), *weekdays = json_array_new();
    gchar *date_iso = g_strconcat(text(event, "date"), "T00:00:00Z", NULL);
    GDateTime *date = g_date_time_new_from_iso8601(date_iso, NULL); g_free(date_iso);
    json_array_add_int_element(weekdays, g_date_time_get_day_of_week(date)); g_date_time_unref(date);
    json_object_set_array_member(config, "weekdays", weekdays);
    for (guint i = 0; i < json_array_get_length(rules); ++i) {
        JsonObject *rule = array_object(rules, i);
        if (source_is_live(rule)) continue; /* a live step has no playlist to prepare */
        JsonObject *source = json_object_new();
        json_object_set_string_member(source, "source_id", text(rule, "id"));
        json_object_set_string_member(source, "playlist_id", text(rule, "playlist_id"));
        gboolean named = *text(rule, "calendar_group_id") != 0;
        json_object_set_int_member(source, "weight", MAX(1, integer(rule, named ? "weight" : "frequency", 1)));
        json_object_set_int_member(source, "count", integer(rule, named ? "count" : "frequency", 1));
        json_object_set_string_member(source, "start", start); json_object_set_string_member(source, "end", end);
        json_object_set_int_member(source, "days", 0); json_array_add_object_element(sources, source);
    }
    JsonObject *policy = member_object(event, "probability_policy");
    if (policy) json_object_set_member(config, "probability_policy", clone(policy));
    /* Every named source retains its effective policy as a catalogue hint. */
    JsonArray *hints = json_array_new();
    for (guint i = 0; i < json_array_get_length(rules); ++i) {
        JsonObject *rule = array_object(rules, i), *source_policy = member_object(member_object(rule, "calendar_settings"), "probability_policy");
        if (source_policy) json_array_add_element(hints, clone(source_policy));
    }
    if (json_array_get_length(hints)) json_object_set_array_member(config, "probability_source_policies", hints); else json_array_unref(hints);
    json_object_set_array_member(config, "sources", sources); g_free(name); g_free(start); g_free(end); return result;
}

static guint64 seed_value(const gchar *seed, const gchar *identity, guint stream)
{
    guint64 value = G_GUINT64_CONSTANT(1469598103934665603);
    const gchar *parts[] = {seed, identity};
    for (guint i = 0; i < G_N_ELEMENTS(parts); ++i) {
        for (const guchar *p = (const guchar *)(parts[i] ? parts[i] : ""); *p; ++p)
            value = (value ^ *p) * G_GUINT64_CONSTANT(1099511628211);
        value = (value ^ 31) * G_GUINT64_CONSTANT(1099511628211);
    }
    value ^= (guint64)stream * G_GUINT64_CONSTANT(0x9e3779b97f4a7c15); return value ? value : 1;
}
static guint random_below(guint64 *state, guint bound)
{
    *state ^= *state >> 12; *state ^= *state << 25; *state ^= *state >> 27;
    guint64 value = *state * G_GUINT64_CONSTANT(2685821657736338717); return (guint)(value % bound);
}
static void shuffle(GArray *values, guint64 *rng)
{
    for (guint i = values->len; i > 1; --i) {
        guint j = random_below(rng, i), temporary = g_array_index(values, guint, i - 1);
        g_array_index(values, guint, i - 1) = g_array_index(values, guint, j);
        g_array_index(values, guint, j) = temporary;
    }
}
static const gchar *row_kind(JsonObject *row)
{
    JsonObject *media = member_object(member_object(row, "detail"), "media");
    return media && json_object_has_member(media, "kind") ? text(media, "kind") : text(member_object(row, "metadata"), "kind");
}
static guint category(JsonObject *row)
{
    const gchar *kind = row_kind(row);
    return g_str_equal(kind, "advertisement") ? 1 : g_str_equal(kind, "sweeper") ? 2 : 0;
}
/* "excluded_kinds" names the AutoDJ content kinds a rotation (schema) never
 * plays. Other catalogue kinds count as music, as in the AutoDJ kind policy. */
static gboolean kinds_contain(JsonArray *kinds, const gchar *kind)
{
    for (guint i = 0; kinds && i < json_array_get_length(kinds); ++i) {
        JsonNode *node = json_array_get_element(kinds, i);
        if (JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING && g_str_equal(json_node_get_string(node), kind)) return TRUE;
    }
    return FALSE;
}
static gboolean row_kind_excluded(JsonObject *rule, JsonObject *row)
{
    JsonArray *kinds = member_array(member_object(rule, "calendar_settings"), "excluded_kinds");
    if (!kinds) return FALSE;
    const gchar *kind = row_kind(row);
    if (!g_str_equal(kind, "advertisement") && !g_str_equal(kind, "jingle") && !g_str_equal(kind, "sweeper")) kind = "music";
    return kinds_contain(kinds, kind);
}
/* Without the jingle kind a rotation layers no in-song jingles either. */
static void policy_strip_jingles(JsonObject *policy)
{
    if (!policy) return;
    if (json_object_has_member(policy, "jingles")) json_object_remove_member(policy, "jingles");
    JsonArray *children = member_array(policy, "children");
    for (guint i = 0; children && i < json_array_get_length(children); ++i)
        policy_strip_jingles(member_object(array_object(children, i), "policy"));
}
gchar *radios_automation_calendar_excluded_kinds_text(JsonObject *settings)
{
    JsonArray *kinds = settings ? member_array(settings, "excluded_kinds") : NULL; GString *caption = g_string_new(NULL);
    const gchar *labels[] = {"Müzik", "Reklam", "Jingle", "Sweeper"};
    for (guint k = 0; kinds && k < G_N_ELEMENTS(autodj_kinds); ++k) {
        if (!kinds_contain(kinds, autodj_kinds[k])) continue;
        if (caption->len) g_string_append(caption, ", ");
        g_string_append(caption, labels[k]);
    }
    return g_string_free(caption, FALSE);
}
static void row_geometry(JsonObject *row, guint64 *in, guint64 *out);
static gboolean row_valid(JsonObject *row, const gchar *station_id)
{
    JsonObject *detail = member_object(row, "detail"), *media = member_object(detail, "media"), *version = member_object(detail, "version");
    const gchar *sha = text(version, "sha256"), *kind = row_kind(row);
    const gchar *kinds[] = {"music", "announcement", "advertisement", "jingle", "program", "sweeper", "news", "effect", "voice_track", "other"};
    gboolean supported = FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(kinds); ++i) supported |= g_str_equal(kind, kinds[i]);
    if (!supported || !g_str_equal(kind, text(member_object(row, "metadata"), "kind")) ||
        !g_uuid_string_is_valid(text(row, "entry_id")) || !media || !version ||
        !g_uuid_string_is_valid(text(media, "_id")) || !g_uuid_string_is_valid(text(version, "_id")) ||
        !g_uuid_string_is_valid(text(media, "media_file_id")) || !g_uuid_string_is_valid(text(media, "item_id")) ||
        !g_str_equal(text(media, "station_id"), station_id) ||
        !g_str_equal(text(media, "version_id"), text(version, "_id")) ||
        !g_str_equal(text(media, "media_file_id"), text(version, "media_file_id")) ||
        !g_str_equal(text(media, "item_id"), text(version, "item_id")) ||
        !flag(version, "verified") || !flag(version, "object_available") ||
        integer(version, "duration_ms", 0) < 1 || integer(version, "duration_ms", 0) > 86400000 || strlen(sha) != 64)
        return FALSE;
    JsonObject *markers = member_object(detail, "markers");
    if (markers && (!g_str_equal(text(markers, "station_id"), station_id) || !g_str_equal(text(markers, "version_id"), text(version, "_id")) ||
        !g_str_equal(text(markers, "media_file_id"), text(version, "media_file_id")) || !g_str_equal(text(markers, "media_sha256"), sha) ||
        integer(markers, "media_duration_ms", 0) != integer(version, "duration_ms", 0) || integer(markers, "cue_in_ms", 0) < 0 ||
        integer(markers, "out_ms", integer(version, "duration_ms", 0)) <= integer(markers, "cue_in_ms", 0) ||
        integer(markers, "out_ms", integer(version, "duration_ms", 0)) > integer(version, "duration_ms", 0))) return FALSE;
    guint64 cue_in, cue_out; row_geometry(row, &cue_in, &cue_out);
    RadioSMediaGeometry geometry = radios_media_geometry(detail, integer(version, "duration_ms", 0), TRUE);
    if (cue_in >= cue_out || !geometry.autodj_valid) return FALSE;
    for (guint i = 0; i < 64; ++i) if (!g_ascii_isxdigit(sha[i])) return FALSE;
    return TRUE;
}
static void source_clear(RotationSource *source)
{
    g_clear_pointer(&source->slots, g_ptr_array_unref);
    for (guint i = 0; i < 3; ++i) g_clear_pointer(&source->pools[i], g_array_unref);
    g_clear_pointer(&source->policies, g_ptr_array_unref);
    g_clear_pointer(&source->plays, g_array_unref);
    g_clear_pointer(&source->track_order, g_array_unref);
    g_clear_pointer(&source->indices, g_hash_table_unref);
}
static void diagnostic(JsonArray *diagnostics, const gchar *reason, JsonObject *rule, JsonObject *row)
{
    JsonObject *message = json_object_new(); json_object_set_string_member(message, "reason", reason);
    json_object_set_string_member(message, "source_id", text(rule, "id"));
    json_object_set_string_member(message, "entry_id", text(row, "entry_id")); json_array_add_object_element(diagnostics, message);
}
static JsonObject *source_policy(RotationSource *source, JsonObject *row)
{
    guint index = GPOINTER_TO_UINT(g_hash_table_lookup(source->indices, row));
    return index ? g_ptr_array_index(source->policies, index - 1) : NULL;
}
static guint source_plays(RotationSource *source, JsonObject *row)
{
    guint index = GPOINTER_TO_UINT(g_hash_table_lookup(source->indices, row));
    return index ? g_array_index(source->plays, guint, index - 1) : 0;
}
static void source_observe(RotationSource *source, JsonObject *row)
{
    guint index = GPOINTER_TO_UINT(g_hash_table_lookup(source->indices, row));
    if (index) ++g_array_index(source->plays, guint, index - 1);
}
static gint track_order_compare(gconstpointer left, gconstpointer right, gpointer data)
{
    RotationSource *source = data; guint a = *(const guint *)left, b = *(const guint *)right;
    JsonObject *ar = g_ptr_array_index(source->slots, a), *br = g_ptr_array_index(source->slots, b);
    gint64 ao = integer(ar, "ordinal", a), bo = integer(br, "ordinal", b);
    gint64 ap = integer(g_ptr_array_index(source->policies, a), "order", ao);
    gint64 bp = integer(g_ptr_array_index(source->policies, b), "order", bo);
    return ap != bp ? (ap < bp ? -1 : 1) : ao != bo ? (ao < bo ? -1 : 1) : a < b ? -1 : a > b;
}
static gboolean source_init(RotationSource *result, JsonObject *rule, JsonObject *prepared,
    const gchar *station_id, const gchar *seed, const gchar *event_date, JsonArray *diagnostics, GError **error)
{
    JsonArray *sources = member_array(prepared, "sources"); JsonObject *matched = NULL;
    gboolean live = source_is_live(rule);
    for (guint i = 0; !live && i < json_array_get_length(sources); ++i) {
        JsonObject *source = array_object(sources, i);
        if (g_str_equal(text(source, "source_id"), text(rule, "id"))) {
            if (matched) return fail(error, "Hazırlık yanıtında aynı rotasyon kaynağı birden fazla kez var.");
            matched = source;
        }
    }
    JsonArray *tracks = member_array(matched, "tracks"), *reasons = member_array(matched, "reasons");
    if (!live && (!matched || !g_str_equal(text(matched, "playlist_id"), text(rule, "playlist_id")) ||
        !tracks || json_array_get_length(tracks) > 500 || !reasons))
        return fail(error, "Rotasyon playlisti değişti veya çalınabilir kaynak hazırlığı yok.");
    for (guint i = 0; reasons && i < json_array_get_length(reasons); ++i) {
        JsonNode *reason = json_array_get_element(reasons, i);
        if (!reason || !JSON_NODE_HOLDS_VALUE(reason) || json_node_get_value_type(reason) != G_TYPE_STRING)
            return fail(error, "Rotasyon kaynak hazırlığı nedenleri geçersiz.");
    }
    gboolean disabled = integer(rule, "weight", 1) == 0 || integer(rule, "calendar_group_weight", 100) == 0;
    /* One changed, disabled or unplayable playlist must not silence the whole
     * block: it stays empty (never selected) and the caller fails only when no
     * source of the event can play. */
    gboolean unplayable = !live && !disabled && json_array_get_length(reasons);
    /* The programme's civil date must lie in the source's inclusive date
     * window (kılavuz §04 step 3); otherwise the source skips this programme. */
    gboolean outside = (json_object_has_member(rule, "start_date") && strcmp(event_date, text(rule, "start_date")) < 0) ||
        (json_object_has_member(rule, "end_date") && strcmp(event_date, text(rule, "end_date")) > 0);
    result->source_name = live ? text(member_object(rule, "live"), "title") : text(matched, "source_name");
    result->live = live; result->date_blocked = !disabled && outside;
    result->copyright_policy = member_object(prepared, "copyright_policy");
    result->rule = rule; result->slots = g_ptr_array_new(); result->excluded_rows = 0; result->transition_rng = seed_value(seed, text(rule, "id"), 91);
    result->track_rng = seed_value(seed, text(rule, "id"), 81);
    result->policies = g_ptr_array_new(); result->plays = g_array_new(FALSE, TRUE, sizeof(guint));
    result->track_order = g_array_new(FALSE, FALSE, sizeof(guint));
    result->indices = g_hash_table_new(g_direct_hash, g_direct_equal);
    for (guint i = 0; i < 3; ++i) { result->pools[i] = g_array_new(FALSE, FALSE, sizeof(guint)); result->rng[i] = seed_value(seed, text(rule, "id"), i + 1); }
    if (disabled) {
        diagnostic(diagnostics, "calendar_source_disabled", rule, NULL); return TRUE;
    }
    if (result->date_blocked) {
        diagnostic(diagnostics, "calendar_source_date_window", rule, NULL); return TRUE;
    }
    if (live) return TRUE;
    if (unplayable) {
        diagnostic(diagnostics, "calendar_source_unplayable", rule, NULL); return TRUE;
    }
    JsonArray *policies = member_array(rule, "track_rules");
    for (guint i = 0; policies && i < json_array_get_length(policies); ++i) {
        JsonObject *policy = array_object(policies, i); gboolean found = FALSE;
        for (guint j = 0; j < json_array_get_length(tracks); ++j)
            found |= !g_ascii_strcasecmp(text(policy, "entry_id"), text(array_object(tracks, j), "entry_id"));
        if (!found) {
            JsonObject *missing = json_object_new(); json_object_set_string_member(missing, "entry_id", text(policy, "entry_id"));
            diagnostic(diagnostics, "calendar_track_rule_missing", rule, missing); json_object_unref(missing);
        }
    }
    for (guint i = 0; i < json_array_get_length(tracks); ++i) {
        JsonObject *row = array_object(tracks, i);
        if (!row) return fail(error, "Hazırlanan playlist satırı geçersiz.");
        for (guint j = 0; j < i; ++j)
            if (!g_ascii_strcasecmp(text(row, "entry_id"), text(array_object(tracks, j), "entry_id")))
                return fail(error, "Hazırlanan playlistte aynı parça kimliği birden fazla kez var.");
        if (!media_eligible(row)) { diagnostic(diagnostics, "calendar_media_unavailable", rule, row); continue; }
        if (!row_valid(row, station_id)) return fail(error, "Takvim kaynağında doğrulanmamış veya başka istasyona ait medya var.");
        const gchar *kind = rotation_media_kind(rule);
        if (!g_str_equal(kind, "all") && !g_str_equal(kind, text(member_object(row, "metadata"), "kind"))) continue;
        if (row_kind_excluded(rule, row)) { ++result->excluded_rows; continue; }
        guint index = result->slots->len, pool = category(row);
        JsonObject *policy = NULL;
        for (guint j = 0; policies && j < json_array_get_length(policies); ++j)
            if (!g_ascii_strcasecmp(text(array_object(policies, j), "entry_id"), text(row, "entry_id"))) policy = array_object(policies, j);
        guint plays = 0;
        g_ptr_array_add(result->slots, row); g_ptr_array_add(result->policies, policy);
        g_hash_table_insert(result->indices, row, GUINT_TO_POINTER(index + 1));
        g_array_append_val(result->plays, plays); g_array_append_val(result->track_order, index);
        g_array_append_val(result->pools[pool], index);
    }
    g_array_sort_with_data(result->track_order, track_order_compare, result);
    if (!result->slots->len && result->excluded_rows) diagnostic(diagnostics, "calendar_source_kind_excluded", rule, NULL);
    if (!result->slots->len) diagnostic(diagnostics, "calendar_source_unplayable", rule, NULL);
    return TRUE;
}
/* After every source is initialised: at least one enabled source must hold
 * playable media, otherwise keep the previous precise rejection. */
static gboolean sources_playable(RotationSource *sources, guint count, GError **error)
{
    const gchar *kind = NULL; gboolean enabled = FALSE, excluded = FALSE, dated = TRUE;
    for (guint i = 0; i < count; ++i) {
        if (integer(sources[i].rule, "weight", 1) == 0 || integer(sources[i].rule, "calendar_group_weight", 100) == 0 || sources[i].live) continue;
        enabled = TRUE;
        if (sources[i].slots->len) return TRUE;
        if (!kind) kind = rotation_media_kind(sources[i].rule);
        excluded |= sources[i].excluded_rows > 0; dated &= sources[i].date_blocked;
    }
    if (!enabled) return TRUE;
    return fail(error, excluded ? "Rotasyonun çalınabilir medyası yalnız şemada olmayan türlerde; Rotasyon düzenleyici › Olmayan türler'den en az bir türü açın." :
        dated ? "Rotasyonun playlistleri bu program tarihinde devrede değil (başlangıç/bitiş tarihi)." :
        g_str_equal(kind, "all") ? "Rotasyon playlistinde çalınabilir doğrulanmış medya yok." :
        "Rotasyon playlistinde seçilen medya türünde çalınabilir doğrulanmış medya yok.");
}
static JsonObject *source_next(RotationSource *source)
{
    if (!source->slots->len) return NULL;
    if (g_str_equal(text(source->rule, "track_selection_mode"), "linear")) {
        guint index = g_array_index(source->track_order, guint, source->track_cursor++ % source->track_order->len);
        /* Linear order keeps every slot in place; an advert or sweeper slot
         * with its random flag ("Reklam/Sweeper rastgeleliği") is filled from
         * that kind's own shuffled pool instead of always the same item. */
        guint pool = category(g_ptr_array_index(source->slots, index));
        const gchar *random_flags[] = {NULL, "advert_random", "sweeper_random"};
        if (pool && flag(source->rule, random_flags[pool]) && source->pools[pool]->len) {
            GArray *order = source->pools[pool];
            if (source->cursors[pool] % order->len == 0) shuffle(order, &source->rng[pool]);
            index = g_array_index(order, guint, source->cursors[pool]++ % order->len);
        }
        return g_ptr_array_index(source->slots, index);
    }
    JsonObject *slot = g_ptr_array_index(source->slots, source->slot_cursor++ % source->slots->len);
    guint pool = category(slot); GArray *order = source->pools[pool];
    const gchar *flags[] = {"track_random", "advert_random", "sweeper_random"};
    if (source->cursors[pool] % order->len == 0 && flag(source->rule, flags[pool])) shuffle(order, &source->rng[pool]);
    guint index = g_array_index(order, guint, source->cursors[pool]++ % order->len);
    return g_ptr_array_index(source->slots, index);
}
static void cycle_build(GArray *cycle, RotationSource *sources, guint count, guint64 *rng)
{
    g_array_set_size(cycle, 0); GArray *random = g_array_new(FALSE, FALSE, sizeof(guint));
    for (guint i = 0; i < count; ++i) {
        if (integer(sources[i].rule, "weight", 1) == 0 || !sources[i].slots->len) continue;
        guint frequency = (guint)integer(sources[i].rule, "frequency", 1);
        GArray *target = flag(sources[i].rule, "random") ? random : cycle;
        for (guint j = 0; j < frequency; ++j) g_array_append_val(target, i);
    }
    shuffle(random, rng);
    for (guint i = 0; i < random->len; ++i) {
        guint source = g_array_index(random, guint, i), position = random_below(rng, cycle->len + 1);
        g_array_insert_val(cycle, position, source);
    }
    g_array_unref(random);
}
typedef struct {
    GHashTable *tracks, *artists, *albums;
} CalendarHistory;
typedef struct {
    guint indices[50], length, cursor, visits;
    guint64 rng;
    gboolean weighted;
    guint weight;
} CalendarGroup;
typedef struct {
    CalendarGroup groups[50]; guint length, next_group;
    guint active_source, remaining; gboolean new_turn, weighted_groups;
    guint64 group_rng;
    gint64 last_turn[50], end_utc;
    CalendarHistory history;
    JsonObject *event;
    RadioSAutomationPolicyState *probability_state;
    gboolean probability_enabled, single_player;
    /* Last-resort continuity, used only when nothing is legal: relax 1 drops
     * artist/album gaps, relax 2 also the song gap (never an immediate
     * repeat); overrun_us lets one item end past a window end. */
    guint relax; gint64 overrun_us;
    gboolean last_short; gchar last_media[37];
    /* Kind policy of the previous item's source: its boundary into the next
     * item is the transition native playout really applies. */
    JsonObject *previous_policy;
    /* Designed presets of the previous item's source and the per-kind
     * sequential cursors, advanced once per finalized boundary. */
    JsonObject *previous_settings;
    guint preset_counters[RADIOS_TRANSITION_KIND_COUNT];
    /* Schema flow (kılavuz §03/§04): the outgoing source of the boundary being
     * estimated, one pending live step with the floor its duration rule puts
     * under the next item, and the programme_entry source of the first turn. */
    JsonObject *previous_rule;
    guint pending_live, entry_source, entry_index;
    gint64 live_start, floor_utc;
    /* Window wait: nothing fits before a playlist or song window opens later
     * in the block; only rows whose own window opens at this instant are
     * legal (0 = no wait) and floor_utc holds their start there. */
    gint64 window_open_utc;
} CalendarSelection;
static gchar *history_key(const gchar *value)
{
    GString *mapped = g_string_new(NULL);
    for (const gchar *p = value; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p); g_string_append_unichar(mapped, c == 'I' ? 0x131 : c == 0x130 ? 'i' : c);
    }
    gchar *folded = g_utf8_casefold(mapped->str, -1); g_string_free(mapped, TRUE);
    gchar *result = g_utf8_normalize(folded, -1, G_NORMALIZE_ALL_COMPOSE); g_free(folded); g_strstrip(result); return result;
}
static void history_remember(GHashTable *table, const gchar *key, gint64 when)
{
    if (!*key) return;
    gint64 *last = g_hash_table_lookup(table, key);
    if (!last) { last = g_new(gint64, 1); *last = when; g_hash_table_insert(table, g_strdup(key), last); }
    else *last = MAX(*last, when);
}
static void history_observe(CalendarHistory *history, const gchar *media, const gchar *artist, const gchar *album, gint64 when)
{
    gchar *a = history_key(artist), *b = history_key(album);
    history_remember(history->tracks, media, when); history_remember(history->artists, a, when); history_remember(history->albums, b, when);
    g_free(a); g_free(b);
}
static gboolean history_protected(GHashTable *table, const gchar *key, gint64 when, gint64 minutes)
{
    gint64 *last = *key ? g_hash_table_lookup(table, key) : NULL;
    return minutes > 0 && last && when - *last < minutes * 60 * G_USEC_PER_SEC;
}
gint64 radios_automation_calendar_event_time(JsonObject *event, gint64 minute)
{
    guint year = 0, month = 0, day = 0;
    if (!event || minute < 0 || minute > 1440 || strlen(text(event, "date")) != 10 ||
        sscanf(text(event, "date"), "%u-%u-%u", &year, &month, &day) != 3 || year > 9999 ||
        !g_date_valid_dmy((GDateDay)day, (GDateMonth)month, (GDateYear)year)) return 0;
    const gchar *name = json_object_has_member(event, "timezone") ? text(event, "timezone") : "UTC";
    GTimeZone *zone = *name ? radios_time_zone_new(name) : NULL;
    if (!zone) return 0;
    GDateTime *date = g_date_time_new(zone, (gint)year, (gint)month, (gint)day, (gint)(minute % 1440 / 60), (gint)(minute % 60), 0);
    if (date && minute == 1440) {
        GDateTime *next = g_date_time_add_days(date, 1); g_date_time_unref(date); date = next;
    }
    gint64 utc = date ? g_date_time_to_unix(date) * G_USEC_PER_SEC : 0;
    if (date) g_date_time_unref(date);
    g_time_zone_unref(zone); return utc;
}
static gint64 event_day(JsonObject *event)
{ return radios_automation_calendar_event_time(event, 0); }

/* Local receipts are never persisted as programme configuration. Only actual
 * consumed source-turn starts, at or before this planning epoch, are accepted. */
static gboolean observed_turns_valid(JsonObject *prepared, gint64 now, GError **error)
{
    JsonNode *node = json_object_get_member(prepared, "calendar_turns");
    if (!node) return TRUE;
    if (!JSON_NODE_HOLDS_OBJECT(node)) return fail(error, "Çalınan playlist başlangıçları geçersiz.");
    JsonObject *turns = json_node_get_object(node);
    if (json_object_get_size(turns) > 2000) return fail(error, "Çalınan playlist başlangıçları sınırı aşıyor.");
    GHashTable *identities = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GList *members = json_object_get_members(turns); gboolean valid = TRUE;
    for (GList *item = members; item && valid; item = item->next) {
        const gchar *key = item->data; JsonNode *stamp = json_object_get_member(turns, key);
        valid = identifier_add(identities, key) && JSON_NODE_HOLDS_VALUE(stamp) &&
            json_node_get_value_type(stamp) == G_TYPE_INT64 && json_node_get_int(stamp) >= 0 && json_node_get_int(stamp) <= now;
    }
    g_list_free(members); g_hash_table_unref(identities);
    return valid || fail(error, "Çalınan playlist başlangıcının kimliği veya gerçek zamanı geçersiz.");
}
static gint64 observed_turn(JsonObject *prepared, const gchar *source_id)
{
    JsonObject *turns = member_object(prepared, "calendar_turns");
    if (!turns) return -1;
    GList *members = json_object_get_members(turns); gint64 result = -1;
    for (GList *item = members; item; item = item->next)
        if (g_ascii_strcasecmp(item->data, source_id) == 0) { result = integer(turns, item->data, -1); break; }
    g_list_free(members); return result;
}
static void selection_init(CalendarSelection *selection, RotationSource *sources, guint count, JsonObject *prepared, JsonObject *event, const gchar *seed)
{
    selection->event = event;
    selection->probability_enabled = member_object(event, "probability_policy") != NULL;
    for (guint i = 0; i < count; ++i) selection->probability_enabled |= member_object(member_object(sources[i].rule, "calendar_settings"), "probability_policy") != NULL;
    if (selection->probability_enabled) selection->probability_state = radios_automation_policy_state_new(seed, member_object(prepared, "policy_selection_checkpoint"), NULL);
    selection->weighted_groups = g_str_equal(text(event, "rotation_selection_mode"), "weighted");
    selection->group_rng = seed_value(seed, text(event, "id"), 111);
    selection->history.tracks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    selection->history.artists = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    selection->history.albums = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    JsonArray *entries = member_array(member_object(member_object(prepared, "history"), "config"), "entries");
    for (guint i = 0; entries && i < MIN(json_array_get_length(entries), 2000u); ++i) {
        JsonObject *entry = array_object(entries, i);
        GDateTime *started = g_date_time_new_from_iso8601(text(entry, "started_utc"), NULL);
        if (started && g_uuid_string_is_valid(text(entry, "media_id")) && g_uuid_string_is_valid(text(entry, "version_id")) &&
            integer(entry, "position_frames", 0) > 0)
            history_observe(&selection->history, text(entry, "media_id"), text(entry, "artist"), text(entry, "album"), g_date_time_to_unix(started) * G_USEC_PER_SEC + g_date_time_get_microsecond(started));
        if (started) g_date_time_unref(started);
    }
    for (guint i = 0; i < count; ++i) {
        selection->last_turn[i] = observed_turn(prepared, text(sources[i].rule, "id")); const gchar *id = text(sources[i].rule, "calendar_group_id");
        guint group = selection->length;
        for (guint j = 0; *id && j < selection->length; ++j)
            if (g_str_equal(text(sources[selection->groups[j].indices[0]].rule, "calendar_group_id"), id)) { group = j; break; }
        if (group == selection->length) {
            CalendarGroup *new_group = &selection->groups[selection->length++];
            new_group->weighted = g_str_equal(text(sources[i].rule, "calendar_selection_mode"), "weighted");
            new_group->weight = (guint)integer(sources[i].rule, "calendar_group_weight", 100);
            new_group->rng = seed_value(seed, *id ? id : text(sources[i].rule, "id"), 71);
        }
        CalendarGroup *target = &selection->groups[group]; target->indices[target->length++] = i;
    }
}
static void selection_clear(CalendarSelection *selection)
{
    g_hash_table_unref(selection->history.tracks); g_hash_table_unref(selection->history.artists); g_hash_table_unref(selection->history.albums);
    radios_automation_policy_state_free(selection->probability_state);
}
static void row_geometry_mode(JsonObject *row, guint64 *in, guint64 *out, gboolean single_player)
{
    JsonObject *detail = member_object(row, "detail");
    RadioSMediaGeometry geometry = radios_media_geometry(detail, integer(member_object(detail, "version"), "duration_ms", 0), TRUE);
    *in = single_player ? geometry.cue_in : geometry.autodj_in;
    *out = radios_media_geometry_sound_out(detail, &geometry, *in,
        !single_player && geometry.has_autodj_out ? geometry.autodj_out : 0);
}
static void row_geometry(JsonObject *row, guint64 *in, guint64 *out)
{ row_geometry_mode(row, in, out, FALSE); }

static JsonObject *kind_transition(JsonObject *policy, JsonObject *outgoing, JsonObject *incoming);
static JsonObject *preset_transition(JsonObject *settings, JsonObject *event, JsonObject *outgoing, JsonObject *incoming,
    gint64 outgoing_start, gboolean single_player, guint *counters, gboolean advance);
static JsonObject *boundary_kind_transition(JsonObject *settings, JsonObject *event, JsonObject *outgoing, JsonObject *incoming,
    gint64 outgoing_start, gboolean single_player, guint *counters, gboolean advance);
static const gchar *autodj_kind(JsonObject *row);
/* When the previous item hands over at native tempo: its AutoDJ OUT mark or
 * its sound end, the instant a live step after it begins. */
static gint64 boundary_end(JsonObject *previous, gint64 previous_start, const CalendarSelection *selection)
{
    if (!previous) return previous_start;
    guint64 in, out; row_geometry_mode(previous, &in, &out, selection->single_player);
    JsonObject *detail = member_object(previous, "detail");
    RadioSMediaGeometry geometry = radios_media_geometry(detail, integer(member_object(detail, "version"), "duration_ms", 0), TRUE);
    guint64 offset = !selection->single_player && geometry.has_autodj_out && geometry.autodj_out > in ? geometry.autodj_out - in : out - in;
    return previous_start + (gint64)(offset * G_USEC_PER_SEC / 48000);
}
/* A schema record's automation as a calendar_transition hint: its designed
 * preset (crossfade of the preset's length with its shape) or its plain
 * transition; durable mode 3 is local beatmatch (native crossfade + beat_sync). */
/* Kılavuz §02: "Playlist türü, hangi otomasyonun bu bağlantıda
 * kullanılabileceğini belirler." A preset assigned to content kinds
 * (autodj_automation kinds.<kind>.presets, or a schedule row of one kind)
 * serves only those kinds; a preset assigned to no kind (or to "all")
 * serves every boundary. NULL kind skips the check. */
static gboolean preset_listed(JsonArray *ids, const gchar *id)
{
    for (guint i = 0; ids && i < json_array_get_length(ids); ++i) {
        JsonNode *node = json_array_get_element(ids, i);
        if (node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING &&
            !g_ascii_strcasecmp(json_node_get_string(node), id)) return TRUE;
    }
    return FALSE;
}
gboolean radios_automation_preset_kind_usable(JsonObject *settings, const gchar *preset_id, const gchar *kind)
{
    JsonObject *automation = member_object(settings, "autodj_automation"), *kinds = member_object(automation, "kinds");
    if (!kind || !*kind || !preset_id || !*preset_id) return TRUE;
    gboolean typed = FALSE;
    for (guint i = 0; i < RADIOS_TRANSITION_KIND_COUNT; ++i) {
        if (!preset_listed(member_array(member_object(kinds, radios_transition_kind_ids[i]), "presets"), preset_id)) continue;
        if (g_str_equal(radios_transition_kind_ids[i], kind)) return TRUE;
        typed = TRUE;
    }
    JsonArray *schedule = member_array(automation, "schedule");
    for (guint i = 0; schedule && i < json_array_get_length(schedule); ++i) {
        JsonObject *row = array_object(schedule, i);
        if (!preset_listed(member_array(row, "presets"), preset_id)) continue;
        if (g_str_equal(text(row, "kind"), "all") || g_str_equal(text(row, "kind"), kind)) return TRUE;
        typed = TRUE;
    }
    return !typed;
}
static JsonObject *schema_hint_from(JsonObject *record, JsonObject *settings, const gchar *kind)
{
    if (!record) return NULL;
    JsonArray *presets = member_array(settings, "autodj_presets");
    if (json_object_has_member(record, "preset_id")) {
        JsonObject *preset = NULL;
        for (guint i = 0; presets && i < json_array_get_length(presets); ++i)
            if (!g_ascii_strcasecmp(text(array_object(presets, i), "id"), text(record, "preset_id"))) preset = array_object(presets, i);
        if (!preset || !radios_automation_preset_kind_usable(settings, text(record, "preset_id"), kind)) return NULL;
        JsonObject *hint = json_object_new(); JsonNode *shape = clone(preset);
        json_object_set_int_member(hint, "mode", 1); json_object_set_int_member(hint, "overlap_ms", integer(preset, "duration_ms", 6000));
        if (flag(preset, "beat_sync")) json_object_set_boolean_member(hint, "beat_sync", TRUE);
        if (shape) json_object_set_member(hint, "shape", shape);
        return hint;
    }
    JsonObject *transition = member_object(record, "transition");
    if (!transition) return NULL;
    gint64 mode = integer(transition, "mode", 0), overlap = integer(transition, "overlap_ms", 0); JsonObject *hint = json_object_new();
    json_object_set_int_member(hint, "mode", mode == 3 ? 1 : mode); json_object_set_int_member(hint, "overlap_ms", mode == 0 ? 0 : overlap);
    if (mode == 3 && overlap > 0) json_object_set_boolean_member(hint, "beat_sync", TRUE);
    return hint;
}
static JsonObject *step_with_order(JsonArray *steps, gint64 order)
{
    for (guint i = 0; steps && i < json_array_get_length(steps); ++i)
        if (integer(array_object(steps, i), "order", 0) == order) return array_object(steps, i);
    return NULL;
}
/* Kılavuz §04 "2 2 2": the active schema's inner records and those of the
 * outgoing (A) and incoming (B) sources are scanned in order 1, 2, 3…; the
 * first order present on all three sides whose records fit the boundary's
 * kind is chosen and the schema record's automation applies. Without one,
 * the link default of A → B (link_transitions) applies; NULL keeps the
 * existing rule. The schema is the outgoing source's rotation. */
static JsonObject *schema_boundary_hint(JsonObject *from_rule, JsonObject *to_rule, const gchar *expected, gint *matched_order, const gchar **rule_name)
{
    JsonObject *schema = member_object(from_rule, "calendar_settings");
    JsonArray *steps = member_array(schema, "automation_steps");
    JsonArray *from_steps = member_array(member_object(from_rule, "settings"), "automation_steps");
    JsonArray *to_steps = member_array(member_object(to_rule, "settings"), "automation_steps");
    JsonObject *best = NULL; gint64 best_order = G_MAXINT64;
    for (guint i = 0; steps && i < json_array_get_length(steps); ++i) {
        JsonObject *step = array_object(steps, i); gint64 order = integer(step, "order", 0);
        if (order >= best_order || !g_str_equal(text(step, "kind"), expected)) continue;
        JsonObject *a = step_with_order(from_steps, order), *b = step_with_order(to_steps, order);
        if (!a || !b || !g_str_equal(text(a, "kind"), expected) || !g_str_equal(text(b, "kind"), expected)) continue;
        best = step; best_order = order;
    }
    JsonObject *hint = best ? schema_hint_from(best, schema, expected) : NULL;
    if (hint) {
        if (matched_order) *matched_order = (gint)best_order;
        if (rule_name) *rule_name = "steps";
        return hint;
    }
    JsonArray *links = member_array(schema, "link_transitions");
    for (guint i = 0; links && i < json_array_get_length(links); ++i) {
        JsonObject *link = array_object(links, i);
        if (g_ascii_strcasecmp(text(link, "from_source_id"), text(from_rule, "calendar_source_id")) ||
            g_ascii_strcasecmp(text(link, "to_source_id"), text(to_rule, "calendar_source_id"))) continue;
        hint = schema_hint_from(link, schema, expected);
        if (hint) {
            if (matched_order) *matched_order = best ? (gint)best_order : 0;
            if (rule_name) *rule_name = "link";
        }
        return hint;
    }
    return NULL;
}
/* The live object without its title: what a programme block and the
 * live_step hint carry (label, channels, automation_id, end_rule, duration_ms). */
static JsonObject *live_summary(JsonObject *live)
{
    JsonObject *summary = json_object_new();
    json_object_set_string_member(summary, "label", text(live, "label"));
    JsonArray *channels = member_array(live, "channels");
    json_object_set_member(summary, "channels", channels ? json_node_copy(json_object_get_member(live, "channels")) : json_node_init_array(json_node_alloc(), json_array_new()));
    json_object_set_string_member(summary, "automation_id", text(live, "automation_id"));
    json_object_set_string_member(summary, "end_rule", text(live, "end_rule"));
    json_object_set_int_member(summary, "duration_ms", integer(live, "duration_ms", 0));
    return summary;
}
/* The inner record of the live event schema: the first record whose kind is
 * the step's label ("dış sıradaki konuşma adımı, şemanın içindeki konuşma kaydına bağlanır"). */
static JsonObject *inner_live_record(JsonObject *rule, gint *order)
{
    JsonObject *live = member_object(rule, "live"); JsonArray *steps = member_array(member_object(rule, "calendar_live_schema"), "automation_steps");
    JsonObject *best = NULL; gint64 best_order = G_MAXINT64;
    for (guint i = 0; steps && i < json_array_get_length(steps); ++i) {
        JsonObject *step = array_object(steps, i);
        if (g_str_equal(text(step, "kind"), text(live, "label")) && integer(step, "order", 0) < best_order) { best = step; best_order = integer(step, "order", 0); }
    }
    if (order) *order = best ? (gint)best_order : 0;
    return best;
}
/* The estimated start of a candidate after the previous item, with the same
 * source span, exit and bounded overlap arithmetic as the native planner
 * (add_entry/calendar_boundary). The schema flow ("2 2 2", link default)
 * decides a source boundary first; otherwise a rotation AutoDJ kind policy
 * replaces the probabilistic hint here exactly as it does on the compiled
 * boundary. A pending live step puts the candidate at the step's end. */
static gint64 candidate_start(JsonObject *previous, JsonObject *hint, gint64 previous_start, JsonObject *row, JsonObject *rule, const CalendarSelection *selection)
{
    if (!previous) return MAX(previous_start, selection->floor_utc);
    gboolean single_player = selection->single_player;
    if (selection->pending_live != G_MAXUINT) return MAX(boundary_end(previous, previous_start, selection), selection->floor_utc);
    JsonObject *pair = selection->previous_rule && rule && selection->previous_rule != rule ?
        schema_boundary_hint(selection->previous_rule, rule, autodj_kind(row), NULL, NULL) : NULL;
    if (!pair) pair = boundary_kind_transition(selection->previous_settings, selection->event, previous, row, previous_start, single_player,
        (guint *)selection->preset_counters, FALSE);
    gint64 mode = integer(pair ? pair : hint, "mode", 0), overlap_ms = integer(pair ? pair : hint, "overlap_ms", 0);
    if (pair) json_object_unref(pair);
    guint64 in, out, incoming_in, incoming_out; row_geometry_mode(previous, &in, &out, single_player); row_geometry_mode(row, &incoming_in, &incoming_out, single_player);
    guint64 span = out - in, offset = span;
    JsonObject *detail = member_object(previous, "detail");
    RadioSMediaGeometry geometry = radios_media_geometry(detail, integer(member_object(detail, "version"), "duration_ms", 0), TRUE);
    JsonObject *markers = member_object(member_object(previous, "detail"), "markers");
    if (single_player) {
        /* Match the native one-player planner before choosing a fitting
         * batch. Saved overlap/beatmatch policy stays valid but does not
         * shorten this transport's source span. Authenticated cue exits do. */
        if (flag(markers, "automatic"))
            offset = MIN(out, MAX(in, (guint64)MAX(integer(markers, "auto_next_ms", (gint64)(out / 48)), 0) * 48)) - in;
        /* A beat-only AutoDJ OUT is a mix trigger, not a one-player cut. */
        return MAX(previous_start + (gint64)(offset * G_USEC_PER_SEC / 48000), selection->floor_utc);
    }
    guint64 overlap = (guint64)MAX(overlap_ms, 0) * 48;
    if (mode == 2 && flag(markers, "automatic"))
        overlap = (guint64)MAX(integer(markers, "overlap_ms", (gint64)(overlap / 48)), 0) * 48;
    JsonObject *incoming_detail = member_object(row, "detail");
    RadioSMediaGeometry incoming = radios_media_geometry(incoming_detail,
        integer(member_object(incoming_detail, "version"), "duration_ms", 0), TRUE);
    guint64 incoming_span = incoming_out - incoming_in;
    /* Match the nominal planner: an inbound fade ends before the incoming
     * AutoDJ trigger, while its physical tail remains the source end budget. */
    if (incoming.has_autodj_out) incoming_span = MIN(incoming_span, incoming.autodj_out - incoming_in);
    /* Same bound as the native planner: an overlap covers at most 40 % of the
     * shorter side, so a short sweeper/jingle always advances the clock and
     * never stacks under its neighbours. */
    overlap = MIN(MIN(MIN(overlap, span), incoming_span), MIN(span, incoming_out - incoming_in) * 2 / 5);
    if (mode == 1) offset = span - overlap;
    else if (mode == 2 && flag(markers, "automatic"))
        offset = MIN(out, MAX(in, (guint64)MAX(integer(markers, "auto_next_ms", (gint64)(out / 48)), 0) * 48)) - in;
    if (geometry.has_autodj_out) offset = geometry.autodj_out - in;
    return MAX(previous_start + (gint64)(offset * G_USEC_PER_SEC / 48000), selection->floor_utc);
}
static const gchar *selected_metadata(JsonObject *row, const gchar *field)
{
    const gchar *value = text(member_object(member_object(row, "detail"), "media"), field);
    return *value ? value : text(member_object(row, "metadata"), field);
}
static gboolean copyright_allowed(RotationSource *source, JsonObject *row, gint64 utc)
{
    RadioSAudioCopyrightWindow window = radios_copyright_policy_window(source->copyright_policy,
        selected_metadata(row, "artist"), selected_metadata(row, "album"), selected_metadata(row, "title"));
    return radios_copyright_window_allowed(&window, utc);
}
/* Continuity settings per rotation (both opt-in): soft_end_seconds (0..600, default 0 = hard end)
 * caps the block-end overrun; relax_gaps (default off) opts into relaxing
 * repeat gaps when nothing else is legal. */
static guint effective_relax(const CalendarSelection *selection, JsonObject *rule)
{ return selection->relax && flag(member_object(rule, "calendar_settings"), "relax_gaps") ? selection->relax : 0; }
static gint64 effective_overrun(const CalendarSelection *selection, JsonObject *rule)
{
    JsonObject *settings = member_object(rule, "calendar_settings");
    gint64 cap = integer(settings, "soft_end_seconds", 0);
    return MIN(selection->overrun_us, cap * G_USEC_PER_SEC);
}
static gboolean short_kind(JsonObject *row)
{ const gchar *kind = row_kind(row); return g_str_equal(kind, "sweeper") || g_str_equal(kind, "jingle"); }
static gboolean candidate_allowed(CalendarSelection *selection, RotationSource *source, JsonObject *row, gint64 utc, gint64 day, gboolean frequency)
{
    JsonObject *rule = source->rule, *settings = member_object(rule, "calendar_settings"), *policy = source_policy(source, row);
    if (!copyright_allowed(source, row, utc)) return FALSE;
    gint64 cap = integer(policy, "max_plays", 0);
    if (integer(rule, "weight", 1) == 0 || integer(policy, "weight", 100) == 0 ||
        (cap && source_plays(source, row) >= (guint)cap) ||
        (frequency && source->current_visit % (guint)integer(policy, "frequency", 1))) return FALSE;
    (void)day;
    guint64 cue_in, cue_out; row_geometry_mode(row, &cue_in, &cue_out, selection->single_player);
    gint64 begin = MAX(integer(rule, "start_minute", 0), integer(policy, "start_minute", 0));
    gint64 end = MIN(integer(rule, "end_minute", 1440), integer(policy, "end_minute", 1440));
    gint64 begin_utc = radios_automation_calendar_event_time(selection->event, begin);
    gint64 end_utc = radios_automation_calendar_event_time(selection->event, end);
    /* A soft end stretches only the block end; explicit playlist and song
     * windows stay strict. 24:00 is the day end, not a window end: a block
     * ending at midnight keeps its soft end past it. */
    gint64 limit = selection->end_utc + effective_overrun(selection, rule);
    if (end < 1440) limit = MIN(limit, end_utc);
    guint relax = effective_relax(selection, rule);
    if (utc < begin_utc || utc >= end_utc || utc >= selection->end_utc ||
        (selection->window_open_utc && begin_utc != selection->window_open_utc) ||
        utc + (gint64)((cue_out - cue_in) * G_USEC_PER_SEC / 48000) > limit) return FALSE;
    /* A sweeper or jingle never follows another one: a block end must not
     * be filled with a chain of station IDs. */
    if (selection->last_short && short_kind(row)) return FALSE;
    JsonObject *media = member_object(member_object(row, "detail"), "media");
    /* Relaxed gaps still never allow the same media twice in a row. */
    if (relax >= 2 && !g_strcmp0(selection->last_media, text(media, "_id"))) return FALSE;
    gchar *a = bounded_text(selected_metadata(row, "artist"), 120), *b = bounded_text(selected_metadata(row, "album"), 120);
    gchar *artist = history_key(a), *album = history_key(b); g_free(a); g_free(b);
    /* Relaxing never touches an explicit per-song interval (song rule). */
    gint64 track_gap = relax >= 2 ? integer(policy, "interval_minutes", 0) : MAX(integer(settings, "track_gap_minutes", 0), integer(policy, "interval_minutes", 0));
    gint64 artist_gap = relax >= 1 ? 0 : integer(settings, "artist_gap_minutes", 0);
    gint64 album_gap = relax >= 1 ? 0 : integer(settings, "album_gap_minutes", 0);
    gboolean allowed = !history_protected(selection->history.tracks, text(media, "_id"), utc, track_gap) &&
        !history_protected(selection->history.artists, artist, utc, artist_gap) &&
        !history_protected(selection->history.albums, album, utc, album_gap);
    g_free(artist); g_free(album); return allowed;
}
static guint next_frequency_visit(guint visit, guint source_frequency, guint track_frequency)
{
    guint a = source_frequency, b = track_frequency;
    while (b) { guint rest = a % b; a = b; b = rest; }
    guint period = source_frequency / a * track_frequency;
    return (visit / period + 1) * period;
}
static const gchar *row_media_id(JsonObject *row)
{ return row ? text(member_object(member_object(row, "detail"), "media"), "_id") : ""; }
/* In ordered and shuffled selection the same media never airs twice in a row
 * while another candidate exists: several sources may share one playlist,
 * and a reshuffled cycle may begin with the item that ended the last one.
 * Explicit per-track weights keep their proportions. */
static gboolean immediate_repeat(JsonObject *previous, JsonObject *row)
{ const gchar *before = row_media_id(previous); return *before && g_str_equal(before, row_media_id(row)); }
/* A probability belongs to a selected set, rather than each member. Draw the
 * set once, then use the canonical linear cursor among its legal members. */
static gboolean policy_has_id(JsonArray *ids, const gchar *id)
{
    for (guint i = 0; ids && i < json_array_get_length(ids); ++i)
        if (!g_ascii_strcasecmp(json_array_get_string_element(ids, i), id)) return TRUE;
    return FALSE;
}
static JsonObject *policy_merge(JsonObject *parent, JsonObject *override)
{
    JsonNode *copy = parent ? clone(parent) : json_node_new(JSON_NODE_OBJECT);
    if (!parent) json_node_take_object(copy, json_object_new());
    JsonObject *result = json_object_ref(json_node_get_object(copy)); json_node_free(copy);
    json_object_set_int_member(result, "schema", 1);
    GList *fields = override ? json_object_get_members(override) : NULL;
    for (GList *it = fields; it; it = it->next) json_object_set_member(result, it->data, json_node_copy(json_object_get_member(override, it->data)));
    g_list_free(fields); return result;
}
static JsonObject *policy_context_node(JsonObject *policy, const gchar *scope, const gchar *id)
{ return radios_automation_policy_context(policy, scope, id); }
static gboolean policy_scope_has_groups(JsonObject *policy, const gchar *scope)
{
    JsonArray *groups = member_array(policy, "children");
    for (guint i = 0; groups && i < json_array_get_length(groups); ++i)
        if (!strcmp(text(array_object(groups, i), "scope"), scope)) return TRUE;
    return FALSE;
}
static gboolean policy_scope_allows(JsonObject *policy, const gchar *scope, const gchar *id)
{
    JsonArray *groups = member_array(policy, "children"); JsonObject *matched = NULL, *rest = NULL;
    for (guint i = 0; groups && i < json_array_get_length(groups); ++i) {
        JsonObject *group = array_object(groups, i); if (strcmp(text(group, "scope"), scope)) continue;
        JsonArray *ids = member_array(group, "ids");
        if (!json_array_get_length(ids)) rest = group;
        else if (policy_has_id(ids, id)) matched = group;
    }
    JsonObject *group = matched ? matched : rest; JsonNode *probability = group ? json_object_get_member(group, "probability") : NULL;
    return !probability || JSON_NODE_HOLDS_NULL(probability) || json_node_get_double(probability) > 0;
}
static JsonObject *source_probability(CalendarSelection *selection, RotationSource *source)
{
    JsonObject *root = member_object(selection->event, "probability_policy");
    JsonObject *automation = policy_context_node(root, "automation", text(source->rule, "calendar_rotation_id"));
    JsonObject *specific = member_object(member_object(source->rule, "calendar_settings"), "probability_policy");
    JsonObject *result = policy_merge(automation, specific); json_object_unref(automation); return result;
}
static gboolean probability_row_enabled(CalendarSelection *selection, RotationSource *source, JsonObject *row)
{
    if (!selection->probability_enabled) return TRUE;
    JsonObject *root = source_probability(selection, source);
    gboolean enabled = policy_scope_allows(root, "playlist", text(source->rule, "playlist_id"));
    JsonObject *playlist = policy_context_node(root, "playlist", text(source->rule, "playlist_id"));
    enabled &= policy_scope_allows(playlist, "track", text(row, "entry_id"));
    json_object_unref(playlist); json_object_unref(root); return enabled;
}
/* Priority: probability percentage > weight > linear order. Only an explicit
 * probability group in that scope replaces the saved weights, song order and
 * shuffle flags; transition or mid-song jingle rules alone keep them. With no
 * weight either (linear selection), the order stays linear (spec 5.3). */
static gboolean source_probability_selects(CalendarSelection *selection, RotationSource *source, const gchar *scope)
{
    if (!selection->probability_enabled) return FALSE;
    JsonObject *root = source_probability(selection, source);
    gboolean selects = policy_scope_has_groups(root, scope);
    if (!selects && strcmp(scope, "playlist")) {
        JsonObject *policy = policy_context_node(root, "playlist", text(source->rule, "playlist_id"));
        selects = policy_scope_has_groups(policy, scope); if (policy) json_object_unref(policy);
    }
    json_object_unref(root); return selects;
}
static gint probability_candidate(JsonObject *policy, const gchar *scope, const gchar *const *ids,
    const gboolean *eligible, guint count, const gchar *key, CalendarSelection *selection, gint64 utc)
{ return radios_automation_policy_choose_targets(policy, scope, ids, eligible, count, key, selection->probability_state, utc); }
static JsonObject *probability_track(CalendarSelection *selection, RotationSource *source, JsonObject *previous,
    JsonObject *hint, gint64 previous_start, gint64 day)
{
    const gchar *ids[500]; gboolean eligible[500] = {FALSE};
    for (guint i = 0; i < source->slots->len; ++i) {
        JsonObject *row = g_ptr_array_index(source->slots, i); gint64 utc = candidate_start(previous, hint, previous_start, row, source->rule, selection);
        ids[i] = text(row, "entry_id");
        eligible[i] = candidate_allowed(selection, source, row, utc, day, TRUE) && probability_row_enabled(selection, source, row) &&
            (!selection->new_turn || selection->last_turn[selection->active_source] < 0 || utc - selection->last_turn[selection->active_source] >= integer(source->rule, "interval_minutes", 0) * 60 * G_USEC_PER_SEC);
    }
    JsonObject *root = source_probability(selection, source), *policy = policy_context_node(root, "playlist", text(source->rule, "playlist_id"));
    gchar *key = g_strdup_printf("calendar/%s/track/%s", text(selection->event, "id"), text(source->rule, "id"));
    gint chosen = probability_candidate(policy, "track", ids, eligible, source->slots->len, key, selection, previous_start);
    g_free(key); json_object_unref(policy); json_object_unref(root);
    if (chosen >= 0) return g_ptr_array_index(source->slots, (guint)chosen);
    /* An undrawn remainder must not starve the source: undefined probability
     * is linear (specification 5.3), over the currently eligible songs. */
    for (guint k = 0; k < source->slots->len; ++k) {
        guint index = (source->track_cursor + k) % source->slots->len;
        if (eligible[index]) { source->track_cursor = index + 1; return g_ptr_array_index(source->slots, index); }
    }
    return NULL;
}
static JsonObject *weighted_track(CalendarSelection *selection, RotationSource *source, JsonObject *previous,
    JsonObject *hint, gint64 previous_start, gint64 day)
{
    guint indices[500], length = 0, total = 0;
    for (guint i = 0; i < source->slots->len; ++i) {
        JsonObject *row = g_ptr_array_index(source->slots, i);
        gint64 utc = candidate_start(previous, hint, previous_start, row, source->rule, selection);
        if (!candidate_allowed(selection, source, row, utc, day, TRUE) ||
            (selection->new_turn && selection->last_turn[selection->active_source] >= 0 &&
             utc - selection->last_turn[selection->active_source] < integer(source->rule, "interval_minutes", 0) * 60 * G_USEC_PER_SEC)) continue;
        indices[length++] = i; total += (guint)integer(g_ptr_array_index(source->policies, i), "weight", 100);
    }
    if (!length) return NULL;
    guint draw = random_below(&source->track_rng, total);
    for (guint i = 0; i < length; ++i) {
        guint index = indices[i], weight = (guint)integer(g_ptr_array_index(source->policies, index), "weight", 100);
        if (draw < weight) return g_ptr_array_index(source->slots, index);
        draw -= weight;
    }
    return NULL;
}
/* programme_entry (kılavuz §03): the first turn starts from the entry source
 * when it is legal now; otherwise the ordinary selection decides. */
static gboolean entry_force(CalendarSelection *selection, RotationSource *sources, JsonObject *previous, JsonObject *hint, gint64 previous_start, gint64 day)
{
    guint index = selection->entry_source;
    if (index == G_MAXUINT) return FALSE;
    selection->entry_source = G_MAXUINT; RotationSource *source = &sources[index];
    if (integer(source->rule, "weight", 1) == 0) return FALSE;
    gboolean legal = source->live && boundary_end(previous, previous_start, selection) < selection->end_utc;
    for (guint k = 0; !legal && k < source->slots->len; ++k) {
        JsonObject *row = g_ptr_array_index(source->slots, k); gint64 utc = candidate_start(previous, hint, previous_start, row, source->rule, selection);
        legal = candidate_allowed(selection, source, row, utc, day, FALSE) && probability_row_enabled(selection, source, row);
    }
    if (!legal) return FALSE;
    for (guint g = 0; g < selection->length; ++g) {
        CalendarGroup *group = &selection->groups[g];
        for (guint j = 0; j < group->length; ++j)
            if (group->indices[j] == index) { group->cursor = (j + 1) % group->length; selection->next_group = (g + 1) % selection->length; }
    }
    selection->active_source = index; selection->new_turn = TRUE;
    selection->remaining = (guint)integer(source->rule, *text(source->rule, "calendar_group_id") ? "count" : "frequency", 1);
    return TRUE;
}
static JsonObject *named_next(CalendarSelection *selection, RotationSource *sources, JsonObject *previous, JsonObject *hint,
    gint64 previous_start, gint64 day, guint *source_index, gint64 *start, gboolean *turn_start)
{
    /* Frequency skips logical visits, never elapsed playback time. A blocked
     * unfinished burst yields to another legal group without weakening gaps. */
    for (guint retry = 0; retry <= 100; ++retry) {
        if (!selection->remaining && !entry_force(selection, sources, previous, hint, previous_start, day)) {
            gboolean found = FALSE, frequency_only = FALSE;
            guint group_candidates[50][50], lengths[50] = {0}, totals[50] = {0};
            guint eligible_groups[50], eligible_length = 0, group_total = 0;
            guint next_visits[50]; for (guint i = 0; i < 50; ++i) next_visits[i] = G_MAXUINT;
            for (guint offset = 0; offset < selection->length; ++offset) {
                guint group_index = (selection->next_group + offset) % selection->length; CalendarGroup *group = &selection->groups[group_index];
                if (!group->weight) continue;
                guint *candidates = group_candidates[group_index];
                guint length = 0, total = 0, visit = group->visits++, next_visit = G_MAXUINT;
                for (guint j = 0; j < group->length; ++j) {
                    guint index = group->indices[(group->cursor + j) % group->length]; RotationSource *source = &sources[index];
                    source->current_visit = visit;
                    if (integer(source->rule, "weight", 1) == 0) continue;
                    guint source_frequency = *text(source->rule, "calendar_group_id") ? (guint)integer(source->rule, "frequency", 1) : 1;
                    gboolean legal = FALSE;
                    /* A live step (kılavuz §04) is legal while the programme is open
                     * and no other step is pending; frequency visits apply as for playlists. */
                    if (source->live) {
                        if (selection->pending_live != G_MAXUINT || boundary_end(previous, previous_start, selection) >= selection->end_utc) continue;
                        if (visit % source_frequency) { frequency_only = TRUE; next_visit = MIN(next_visit, next_frequency_visit(visit, source_frequency, 1)); continue; }
                        candidates[length++] = index; total += (guint)integer(source->rule, "weight", 1); continue;
                    }
                    for (guint k = 0; k < source->slots->len; ++k) {
                        JsonObject *row = g_ptr_array_index(source->slots, k); gint64 utc = candidate_start(previous, hint, previous_start, row, source->rule, selection);
                        gboolean available = (selection->last_turn[index] < 0 || utc - selection->last_turn[index] >= integer(source->rule, "interval_minutes", 0) * 60 * G_USEC_PER_SEC) &&
                            candidate_allowed(selection, source, row, utc, day, FALSE) && probability_row_enabled(selection, source, row);
                        if (!available) continue;
                        guint track_frequency = (guint)integer(source_policy(source, row), "frequency", 1);
                        if (visit % source_frequency || visit % track_frequency) {
                            frequency_only = TRUE; next_visit = MIN(next_visit, next_frequency_visit(visit, source_frequency, track_frequency));
                        } else legal = TRUE;
                    }
                    if (!legal) continue;
                    candidates[length++] = index; total += (guint)integer(source->rule, "weight", 1);
                }
                if (!length) { next_visits[group_index] = next_visit; continue; }
                lengths[group_index] = length; totals[group_index] = total;
                eligible_groups[eligible_length++] = group_index; group_total += group->weight;
                if (!selection->weighted_groups && !selection->probability_enabled) break;
            }
            if (eligible_length) {
                guint group_index = eligible_groups[0], forced_source = G_MAXUINT;
                JsonObject *event_policy = member_object(selection->event, "probability_policy");
                if (selection->probability_enabled && !policy_scope_has_groups(event_policy, "automation") && policy_scope_has_groups(event_policy, "playlist")) {
                    const gchar *ids[50]; gboolean available[50]; guint indices[50], group_indices[50], candidates_count = 0;
                    for (guint i = 0; i < eligible_length; ++i) {
                        guint group = eligible_groups[i];
                        for (guint j = 0; j < lengths[group]; ++j) {
                            guint index = group_candidates[group][j]; ids[candidates_count] = text(sources[index].rule, "playlist_id");
                            available[candidates_count] = TRUE; indices[candidates_count] = index; group_indices[candidates_count++] = group;
                        }
                    }
                    gchar *key = g_strdup_printf("calendar/%s/playlist", text(selection->event, "id"));
                    gint selected = probability_candidate(event_policy, "playlist", ids, available, candidates_count, key, selection, previous_start);
                    g_free(key); if (selected < 0) return NULL;
                    group_index = group_indices[selected]; forced_source = indices[selected];
                } else if (selection->probability_enabled && policy_scope_has_groups(event_policy, "automation")) {
                    const gchar *ids[50]; gboolean available[50] = {FALSE};
                    for (guint i = 0; i < selection->length; ++i) {
                        ids[i] = text(sources[selection->groups[i].indices[0]].rule, "calendar_rotation_id");
                        if (!*ids[i]) ids[i] = text(sources[selection->groups[i].indices[0]].rule, "id");
                        available[i] = lengths[i] > 0;
                    }
                    gchar *key = g_strdup_printf("calendar/%s/automation", text(selection->event, "id"));
                    gint selected = probability_candidate(member_object(selection->event, "probability_policy"), "automation", ids, available, selection->length, key, selection, previous_start);
                    g_free(key); if (selected < 0) return NULL; group_index = (guint)selected;
                } else if (selection->weighted_groups) {
                    guint draw = random_below(&selection->group_rng, group_total);
                    for (guint i = 0; i < eligible_length; ++i) {
                        guint index = eligible_groups[i], weight = selection->groups[index].weight;
                        if (draw < weight) { group_index = index; break; } draw -= weight;
                    }
                }
                CalendarGroup *group = &selection->groups[group_index]; guint *candidates = group_candidates[group_index];
                guint length = lengths[group_index], chosen = candidates[0];
                if (forced_source != G_MAXUINT) chosen = forced_source;
                else if (source_probability_selects(selection, &sources[chosen], "playlist")) {
                    const gchar *ids[50]; gboolean available[50];
                    for (guint j = 0; j < length; ++j) { ids[j] = text(sources[candidates[j]].rule, "playlist_id"); available[j] = TRUE; }
                    JsonObject *policy = source_probability(selection, &sources[chosen]);
                    gchar *key = g_strdup_printf("calendar/%s/playlist/%s", text(selection->event, "id"), text(sources[chosen].rule, "calendar_group_id"));
                    gint selected = probability_candidate(policy, "playlist", ids, available, length, key, selection, previous_start);
                    g_free(key); json_object_unref(policy); if (selected < 0) return NULL; chosen = candidates[selected];
                } else if (group->weighted) {
                    guint draw = random_below(&group->rng, totals[group_index]);
                    for (guint j = 0; j < length; ++j) {
                        guint weight = (guint)integer(sources[candidates[j]].rule, "weight", 1);
                        if (draw < weight) { chosen = candidates[j]; break; } draw -= weight;
                    }
                }
                for (guint j = 0; j < group->length; ++j) if (group->indices[j] == chosen) group->cursor = (j + 1) % group->length;
                selection->active_source = chosen;
                selection->remaining = (guint)integer(sources[chosen].rule, *text(sources[chosen].rule, "calendar_group_id") ? "count" : "frequency", 1);
                selection->next_group = (group_index + 1) % selection->length; found = TRUE; selection->new_turn = TRUE;
            }
            if (!found) {
                if (!frequency_only) return NULL;
                /* Skip only empty logical cycles. Other eligible groups must
                 * consume their normal visit, preserving source frequencies. */
                for (guint i = 0; i < selection->length; ++i)
                    if (next_visits[i] != G_MAXUINT) selection->groups[i].visits = next_visits[i];
                continue;
            }
        }
        RotationSource *source = &sources[selection->active_source];
        if (source->live) {
            /* The step plays nothing: it begins where the previous item hands
             * over, and the following turn picks the media that comes after it. */
            JsonObject *live = member_object(source->rule, "live");
            selection->pending_live = selection->active_source; selection->live_start = boundary_end(previous, previous_start, selection);
            selection->floor_utc = g_str_equal(text(live, "end_rule"), "duration") ? selection->live_start + integer(live, "duration_ms", 0) * 1000 : 0;
            selection->last_turn[selection->active_source] = selection->live_start; selection->remaining = 0; selection->new_turn = FALSE;
            continue;
        }
        JsonObject *repeat = NULL; gint64 repeat_utc = 0;
        gboolean weighted = g_str_equal(text(source->rule, "track_selection_mode"), "weighted");
        for (guint attempt = 0; attempt < source->slots->len; ++attempt) {
            JsonObject *row = source_probability_selects(selection, source, "track") ? probability_track(selection, source, previous, hint, previous_start, day) : weighted ? weighted_track(selection, source, previous, hint, previous_start, day) : source_next(source);
            if (!row) break;
            gint64 utc = candidate_start(previous, hint, previous_start, row, source->rule, selection);
            if (!candidate_allowed(selection, source, row, utc, day, TRUE) || (selection->new_turn && selection->last_turn[selection->active_source] >= 0 &&
                utc - selection->last_turn[selection->active_source] < integer(source->rule, "interval_minutes", 0) * 60 * G_USEC_PER_SEC)) continue;
            if (!weighted && !selection->probability_enabled && immediate_repeat(previous, row) && attempt + 1 < source->slots->len) {
                if (!repeat) { repeat = row; repeat_utc = utc; }
                continue;
            }
            *source_index = selection->active_source; *start = utc; *turn_start = selection->new_turn; --selection->remaining;
            source_observe(source, row);
            if (selection->new_turn) { selection->last_turn[*source_index] = utc; selection->new_turn = FALSE; }
            return row;
        }
        if (repeat) {
            /* Only the same media was legal: a repeat beats an empty turn. */
            *source_index = selection->active_source; *start = repeat_utc; *turn_start = selection->new_turn; --selection->remaining;
            source_observe(source, repeat);
            if (selection->new_turn) { selection->last_turn[*source_index] = repeat_utc; selection->new_turn = FALSE; }
            return repeat;
        }
        selection->remaining = 0;
    }
    return NULL;
}
/* Window wait: the first instant inside the block at which a playlist or
 * song window opens after the estimated start of its row; 0 when none. */
static gint64 next_window_open(CalendarSelection *selection, RotationSource *sources, guint count, JsonObject *previous, JsonObject *hint, gint64 previous_start)
{
    gint64 next = 0;
    for (guint i = 0; i < count; ++i)
        for (guint k = 0; k < sources[i].slots->len; ++k) {
            JsonObject *row = g_ptr_array_index(sources[i].slots, k), *policy = source_policy(&sources[i], row);
            gint64 open = radios_automation_calendar_event_time(selection->event,
                MAX(integer(sources[i].rule, "start_minute", 0), integer(policy, "start_minute", 0)));
            if (open < selection->end_utc && (!next || open < next) &&
                open > candidate_start(previous, hint, previous_start, row, sources[i].rule, selection)) next = open;
        }
    return next;
}
static JsonObject *choose_transition(RotationSource *source)
{
    JsonObject *settings = member_object(source->rule, "calendar_settings");
    JsonArray *all = member_array(settings, "transitions");
    if (!all) return NULL;
    /* The rotation's own (kind-less) distribution; kind entries are drawn
     * per boundary in the compile pass and leave their remainder to this. */
    GPtrArray *choices = g_ptr_array_new();
    for (guint i = 0; i < json_array_get_length(all); ++i)
        if (array_object(all, i) && !json_object_has_member(array_object(all, i), "kind")) g_ptr_array_add(choices, array_object(all, i));
    JsonObject *chosen = NULL;
    if (!choices->len) { g_ptr_array_unref(choices); return NULL; }
    if (g_str_equal(text(settings, "transition_selection_mode"), "linear")) {
        for (guint attempt = 0; attempt < choices->len; ++attempt) {
            JsonObject *choice = g_ptr_array_index(choices, source->transition_cursor++ % choices->len);
            if (integer(choice, "weight", 0) > 0) { chosen = choice; break; }
        }
    } else {
        guint total = 0; for (guint i = 0; i < choices->len; ++i) total += (guint)integer(g_ptr_array_index(choices, i), "weight", 0);
        guint draw = random_below(&source->transition_rng, total);
        for (guint i = 0; i < choices->len; ++i) {
            JsonObject *choice = g_ptr_array_index(choices, i); guint weight = (guint)integer(choice, "weight", 0);
            if (draw < weight) { chosen = choice; break; } draw -= weight;
        }
    }
    g_ptr_array_unref(choices);
    gint64 mode = integer(chosen, "mode", 0); JsonObject *hint = json_object_new();
    json_object_set_int_member(hint, "mode", mode == 3 ? 1 : mode);
    json_object_set_int_member(hint, "overlap_ms", integer(chosen, "overlap_ms", 0));
    if (mode == 3) json_object_set_boolean_member(hint, "beat_sync", TRUE);
    if (json_object_has_member(chosen, "effect") && !g_str_equal(text(chosen, "effect"), "none"))
        json_object_set_string_member(hint, "effect", text(chosen, "effect"));
    return hint;
}
static const gchar *autodj_kind(JsonObject *row)
{
    const gchar *kind = row_kind(row);
    return g_str_equal(kind, "advertisement") || g_str_equal(kind, "jingle") || g_str_equal(kind, "sweeper") ? kind : "music";
}
/* Music into music uses the music rule; any other pair uses the rule of its
 * non-music side, preferring the incoming item (a jingle or advert entry). */
static JsonObject *kind_transition(JsonObject *policy, JsonObject *outgoing, JsonObject *incoming)
{
    const gchar *from = autodj_kind(outgoing), *to = incoming ? autodj_kind(incoming) : from;
    const gchar *kind = !g_str_equal(to, "music") ? to : from;
    JsonObject *choice = member_object(policy, kind); gint64 mode = integer(choice, "mode", 0);
    JsonObject *hint = json_object_new();
    gboolean sync = mode == 3;
    if (sync && json_object_has_member(choice, "sync_percent")) {
        /* Each boundary draws independently but reproducibly from the pair of
         * recordings (entry ids are fresh per preparation), so a repeated
         * preparation keeps its choice. */
        const gchar *from_media = text(member_object(member_object(outgoing, "detail"), "media"), "_id");
        const gchar *to_media = incoming ? text(member_object(member_object(incoming, "detail"), "media"), "_id") : "";
        guint32 draw = g_str_hash(from_media) * 2654435761u ^ g_str_hash(to_media);
        sync = (gint64)((draw ^ (draw >> 15)) % 100u) < integer(choice, "sync_percent", 100);
    }
    json_object_set_int_member(hint, "mode", mode == 3 ? 1 : mode);
    json_object_set_int_member(hint, "overlap_ms", mode ? integer(choice, "overlap_ms", 0) : 0);
    if (sync) json_object_set_boolean_member(hint, "beat_sync", TRUE);
    return hint;
}
/* Local calendar weekday (ISO 1..7) and minute of a UTC instant in the
 * event's timezone; both unknown (0, -1) without a usable time or zone. */
static void local_clock(JsonObject *event, gint64 utc_us, gint *weekday, gint *minute)
{
    *weekday = 0; *minute = -1;
    const gchar *name = event && json_object_has_member(event, "timezone") ? text(event, "timezone") : "UTC";
    GTimeZone *zone = utc_us > 0 && *name ? radios_time_zone_new(name) : NULL;
    if (!zone) return;
    GDateTime *when = g_date_time_new_from_unix_utc(utc_us / G_USEC_PER_SEC);
    GDateTime *local = when ? g_date_time_to_timezone(when, zone) : NULL;
    if (local) { *weekday = g_date_time_get_day_of_week(local); *minute = g_date_time_get_hour(local) * 60 + g_date_time_get_minute(local); }
    if (local) g_date_time_unref(local);
    if (when) g_date_time_unref(when);
    g_time_zone_unref(zone);
}
/* A designed preset ("AutoDJ otomasyonu") replaces the kind policy for the
 * boundary into incoming: its own length, beat sync and curve/effect shape.
 * The boundary clock is the outgoing item's exit at native tempo, so the
 * selection estimate and the compiled hint consult the same schedule row. */
static JsonObject *preset_transition(JsonObject *settings, JsonObject *event, JsonObject *outgoing, JsonObject *incoming,
    gint64 outgoing_start, gboolean single_player, guint *counters, gboolean advance)
{
    JsonObject *automation = member_object(settings, "autodj_automation"); JsonArray *presets = member_array(settings, "autodj_presets");
    if (!automation || !presets || !outgoing || !incoming) return NULL;
    const gchar *from = autodj_kind(outgoing), *to = autodj_kind(incoming);
    const gchar *kind = !g_str_equal(to, "music") ? to : from; gint index = radios_transition_kind_index(kind);
    if (index < 0) return NULL;
    guint64 in, out; row_geometry_mode(outgoing, &in, &out, single_player);
    gint weekday, minute; local_clock(event, outgoing_start + (gint64)((out - in) * G_USEC_PER_SEC / 48000), &weekday, &minute);
    const gchar *from_media = text(member_object(member_object(outgoing, "detail"), "media"), "_id");
    const gchar *to_media = text(member_object(member_object(incoming, "detail"), "media"), "_id");
    guint32 draw = g_str_hash(from_media) * 2654435761u ^ g_str_hash(to_media); draw ^= draw >> 15;
    guint counter = counters ? counters[index] : 0;
    JsonObject *preset = radios_transition_preset_choose(automation, presets, kind, weekday, minute, &counter, draw);
    if (!preset) return NULL;
    if (advance && counters) counters[index] = counter;
    JsonObject *hint = json_object_new(); JsonNode *shape = clone(preset);
    json_object_set_int_member(hint, "mode", 1);
    json_object_set_int_member(hint, "overlap_ms", integer(preset, "duration_ms", 6000));
    if (flag(preset, "beat_sync")) json_object_set_boolean_member(hint, "beat_sync", TRUE);
    if (shape) json_object_set_member(hint, "shape", shape);
    return hint;
}
/* The boundary's content kind: music into music is music; any other pair is
 * its non-music side, preferring the incoming item (a jingle or advert). */
static const gchar *boundary_kind(JsonObject *outgoing, JsonObject *incoming)
{
    const gchar *from = autodj_kind(outgoing), *to = incoming ? autodj_kind(incoming) : from;
    return !g_str_equal(to, "music") ? to : from;
}
static guint32 boundary_draw(JsonObject *outgoing, JsonObject *incoming)
{
    const gchar *from_media = text(member_object(member_object(outgoing, "detail"), "media"), "_id");
    const gchar *to_media = incoming ? text(member_object(member_object(incoming, "detail"), "media"), "_id") : "";
    guint32 draw = g_str_hash(from_media) * 2654435761u ^ g_str_hash(to_media); return draw ^ (draw >> 15);
}
/* Sözleşme 18.10 (C8): a kind's own share of transitions. The draw is
 * reproducible from the pair of recordings, as sync_percent's; a share that
 * falls on no entry (the remainder, or a designed preset while the AutoDJ
 * automation is off) leaves the boundary to the rotation's distribution. */
static JsonObject *kind_entry_transition(JsonObject *settings, JsonObject *outgoing, JsonObject *incoming, gboolean *scoped)
{
    *scoped = FALSE;
    JsonArray *choices = member_array(settings, "transitions"); if (!choices || !outgoing) return NULL;
    const gchar *kind = boundary_kind(outgoing, incoming);
    gboolean automation = flag(member_object(settings, "autodj_automation"), "enabled");
    guint percent = boundary_draw(outgoing, incoming) % 100u, position = 0;
    for (guint i = 0; i < json_array_get_length(choices); ++i) {
        JsonObject *choice = array_object(choices, i);
        if (!choice || !g_str_equal(text(choice, "kind"), kind)) continue;
        *scoped = TRUE; position += (guint)integer(choice, "weight", 0);
        if (percent >= position) continue;
        if (json_object_has_member(choice, "preset_id")) {
            JsonObject *preset = automation ? settings_preset(settings, text(choice, "preset_id")) : NULL;
            if (!preset) return NULL;
            JsonObject *hint = json_object_new(); JsonNode *shape = clone(preset);
            json_object_set_int_member(hint, "mode", 1); json_object_set_int_member(hint, "overlap_ms", integer(preset, "duration_ms", 6000));
            if (flag(preset, "beat_sync")) json_object_set_boolean_member(hint, "beat_sync", TRUE);
            if (shape) json_object_set_member(hint, "shape", shape);
            return hint;
        }
        gint64 mode = integer(choice, "mode", 0); JsonObject *hint = json_object_new();
        json_object_set_int_member(hint, "mode", mode == 3 ? 1 : mode);
        json_object_set_int_member(hint, "overlap_ms", mode ? integer(choice, "overlap_ms", 0) : 0);
        if (mode == 3) json_object_set_boolean_member(hint, "beat_sync", TRUE);
        if (g_str_equal(json_object_has_member(choice, "effect") ? text(choice, "effect") : "none", "lowpass"))
            json_object_set_string_member(hint, "effect", "lowpass");
        return hint;
    }
    return NULL;
}
/* AutoDJ otomasyonu › Saat dilimleri alone: a kind with entries of its own no
 * longer takes the presets of autodj_automation.kinds (they were carried
 * into its entries); a matching time slot still decides first. */
static JsonObject *schedule_transition(JsonObject *settings, JsonObject *event, JsonObject *outgoing, JsonObject *incoming,
    gint64 outgoing_start, gboolean single_player, guint *counters, gboolean advance)
{
    JsonObject *automation = member_object(settings, "autodj_automation");
    if (!automation || !json_object_has_member(automation, "schedule")) return NULL;
    JsonObject *slots = json_object_new(), *copy = json_object_new();
    json_object_set_boolean_member(slots, "enabled", flag(automation, "enabled"));
    json_object_set_member(slots, "schedule", json_node_copy(json_object_get_member(automation, "schedule")));
    json_object_set_object_member(slots, "kinds", json_object_new());
    GList *members = json_object_get_members(settings);
    for (GList *it = members; it; it = it->next)
        if (!g_str_equal(it->data, "autodj_automation")) json_object_set_member(copy, it->data, json_node_copy(json_object_get_member(settings, it->data)));
    g_list_free(members); json_object_set_object_member(copy, "autodj_automation", slots);
    JsonObject *hint = preset_transition(copy, event, outgoing, incoming, outgoing_start, single_player, counters, advance);
    json_object_unref(copy); return hint;
}
/* One rule for a boundary's AutoDJ transition, shared by the selection
 * estimate and the compiled hint: a time slot, then the kind's own entries
 * (their remainder: NULL, the rotation's distribution); without entries the
 * former kind presets and kind_transitions of autodj_automation. */
static JsonObject *boundary_kind_transition(JsonObject *settings, JsonObject *event, JsonObject *outgoing, JsonObject *incoming,
    gint64 outgoing_start, gboolean single_player, guint *counters, gboolean advance)
{
    if (!outgoing) return NULL;
    gboolean scoped = FALSE; JsonObject *entry = kind_entry_transition(settings, outgoing, incoming, &scoped);
    if (scoped) {
        JsonObject *slot = schedule_transition(settings, event, outgoing, incoming, outgoing_start, single_player, counters, advance);
        if (slot) { if (entry) json_object_unref(entry); return slot; }
        return entry;
    }
    JsonObject *hint = preset_transition(settings, event, outgoing, incoming, outgoing_start, single_player, counters, advance);
    JsonObject *policy = member_object(settings, "kind_transitions");
    if (!hint && policy) hint = kind_transition(policy, outgoing, incoming);
    return hint;
}
static JsonObject *choose_routing(JsonObject *source, JsonObject *row)
{
    /* A source's own player map wins over the rotation's AutoDJ channels. */
    JsonObject *routing = member_object(source, "autodj");
    if (!routing) routing = member_object(member_object(source, "calendar_settings"), "autodj_players");
    if (!routing) return NULL;
    const gchar *kind = row_kind(row);
    if (!g_str_equal(kind, "advertisement") && !g_str_equal(kind, "jingle") && !g_str_equal(kind, "sweeper")) kind = "music";
    JsonObject *hint = json_object_new();
    json_object_set_member(hint, "decks", json_node_copy(json_object_get_member(routing, kind)));
    return hint;
}

static JsonObject *calendar_constraints(JsonObject *event, JsonObject *source, JsonObject *policy, gint64 day, gboolean turn_start, gint64 last_observed)
{
    JsonObject *settings = member_object(source, "calendar_settings"), *hint = json_object_new();
    gint64 begin = MAX(MAX(integer(event, "start_minute", 0), integer(source, "start_minute", 0)), integer(policy, "start_minute", 0));
    gint64 end = MIN(MIN(integer(event, "end_minute", 1440), integer(source, "end_minute", 1440)), integer(policy, "end_minute", 1440));
    (void)day;
    gint64 earliest = radios_automation_calendar_event_time(event, begin);
    if (last_observed >= 0) earliest = MAX(earliest, last_observed + integer(source, "interval_minutes", 0) * 60 * G_USEC_PER_SEC);
    json_object_set_int_member(hint, "earliest_utc_us", earliest);
    json_object_set_int_member(hint, "latest_end_utc_us", radios_automation_calendar_event_time(event, end));
    const gchar *keys[] = {"track_gap_minutes", "artist_gap_minutes", "album_gap_minutes"};
    const gchar *output[] = {"track_gap_ms", "artist_gap_ms", "album_gap_ms"};
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i)
        json_object_set_int_member(hint, output[i], (i == 0 ? MAX(integer(settings, keys[i], 0), integer(policy, "interval_minutes", 0)) : integer(settings, keys[i], 0)) * 60000);
    json_object_set_int_member(hint, "source_interval_ms", integer(source, "interval_minutes", 0) * 60000);
    json_object_set_boolean_member(hint, "source_turn_start", turn_start); return hint;
}

static JsonObject *programme_entry(JsonObject *row, JsonObject *rule, const gchar *id)
{
    JsonObject *detail = member_object(row, "detail"), *media = member_object(detail, "media"), *version = member_object(detail, "version"), *metadata = member_object(row, "metadata");
    JsonObject *event = json_object_new(); json_object_set_string_member(event, "id", id);
    const gchar *fields[] = {"title", "artist", "album"}; const guint limits[] = {160, 120, 120};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) {
        const gchar *value = text(media, fields[i]); if (!*value) value = text(metadata, fields[i]);
        gchar *bounded = bounded_text(value, limits[i]);
        json_object_set_string_member(event, fields[i], *bounded || i != 0 ? bounded : "Takvim parçası"); g_free(bounded);
    }
    json_object_set_string_member(event, "kind", row_kind(row)); json_object_set_string_member(event, "start_time", "");
    json_object_set_int_member(event, "duration_ms", integer(version, "duration_ms", 0));
    gchar *label = bounded_text(text(version, "label"), 80); json_object_set_string_member(event, "version_label", label); g_free(label);
    json_object_set_string_member(event, "primary_file", ""); json_object_set_string_member(event, "backup_file", "");
    gchar *note = g_strdup_printf("Takvim rotasyonu: %s", text(rule, "id")); json_object_set_string_member(event, "note", note); g_free(note);
    json_object_set_string_member(event, "media_id", text(media, "_id")); json_object_set_string_member(event, "version_id", text(version, "_id"));
    json_object_set_string_member(event, "media_file_id", text(media, "media_file_id")); json_object_set_string_member(event, "media_sha256", text(version, "sha256"));
    return event;
}

/* config.blocks[] entry of a live step: kind "live", the step's title (or
 * "Live event") and the live object for the event list's L/R binding. */
static JsonObject *live_block_new(JsonObject *live, const gchar *id)
{
    JsonObject *block = json_object_new(); gchar *title = bounded_text(text(live, "title"), 160);
    json_object_set_string_member(block, "id", id); json_object_set_string_member(block, "kind", "live");
    json_object_set_string_member(block, "title", *title ? title : radios_programme_event_title("live")); g_free(title);
    json_object_set_string_member(block, "start_time", ""); json_object_set_string_member(block, "note", "");
    json_object_set_object_member(block, "live", live_summary(live));
    return block;
}
static JsonObject *live_step_hint(JsonObject *live, const gchar *block_id, guint outer_order, gint inner_order)
{
    JsonObject *hint = live_summary(live);
    json_object_set_string_member(hint, "block_id", block_id);
    json_object_set_int_member(hint, "outer_order", outer_order); json_object_set_int_member(hint, "inner_order", inner_order);
    return hint;
}
/* The outgoing item's boundary into a live step: a manual end rule holds the
 * transport (native MANUAL, operator starts the next item); a duration rule
 * takes the schema chain ("2 2 2" with the step's label, link default), then
 * the inner record's own automation, else the existing rule. */
static JsonObject *live_boundary_hint(JsonObject *previous_rule, JsonObject *step_rule, JsonObject *live)
{
    if (g_str_equal(text(live, "end_rule"), "manual")) {
        JsonObject *hint = json_object_new(); json_object_set_int_member(hint, "mode", 3); json_object_set_int_member(hint, "overlap_ms", 0); return hint;
    }
    JsonObject *hint = previous_rule ? schema_boundary_hint(previous_rule, step_rule, text(live, "label"), NULL, NULL) : NULL;
    if (!hint) hint = schema_hint_from(inner_live_record(step_rule, NULL), member_object(step_rule, "calendar_live_schema"), text(live, "label"));
    return hint;
}
/* The live step enters the programme: a live block, the live_step hint on the
 * media row before it and that row's boundary. Returns TRUE when a row carried the hint. */
static gboolean live_step_emit(RotationSource *step, JsonObject *previous_rule, JsonArray *blocks, JsonArray *tracks, GArray *track_fixed, JsonArray *diagnostics)
{
    JsonObject *live = radios_programme_clone_object(member_object(step->rule, "live")); gchar *block_id = g_uuid_string_random(); gint inner = 0;
    JsonObject *record = inner_live_record(step->rule, &inner);
    /* An explicit channel list belongs to the matched inner live record.
     * Missing channels inherit the outer step; an empty list means none. */
    if (record && json_object_has_member(record, "channels"))
        json_object_set_member(live, "channels", json_node_copy(json_object_get_member(record, "channels")));
    json_array_add_object_element(blocks, live_block_new(live, block_id));
    JsonObject *before = json_array_get_length(tracks) ? array_object(tracks, json_array_get_length(tracks) - 1) : NULL;
    if (before) {
        json_object_set_object_member(before, "live_step", live_step_hint(live, block_id, step->outer_order, inner));
        JsonObject *boundary = live_boundary_hint(previous_rule, step->rule, live);
        if (boundary) { json_object_set_object_member(before, "calendar_transition", boundary); g_array_index(track_fixed, guint8, json_array_get_length(tracks) - 1) = 1; }
    }
    diagnostic(diagnostics, "calendar_live_step", step->rule, NULL); g_free(block_id); json_object_unref(live);
    return before != NULL;
}
/* programme_entry (kılavuz §03): the runtime entry order from the scope
 * driver wins; otherwise the schema's own entry with a seeded draw among its
 * alternatives (weights out of 100, the remainder is the main entry). The
 * hint names the source order, the alternative (-1 = main), whether the first
 * row really came from it, and the transition of that entry. */
static JsonObject *programme_entry_resolve(JsonObject *event, RotationSource *rotations, guint count, const gchar *seed, CalendarSelection *selection)
{
    JsonObject *settings = member_object(event, "schema_entry_settings");
    JsonObject *entry = member_object(settings, "programme_entry");
    const gchar *group = entry && count ? text(rotations[0].rule, "calendar_group_id") : NULL;
    for (guint i = 0; i < count && !entry; ++i) {
        settings = member_object(rotations[i].rule, "calendar_settings"); entry = member_object(settings, "programme_entry");
        if (entry) group = text(rotations[i].rule, "calendar_group_id");
    }
    gboolean runtime = json_object_has_member(event, "schema_entry_source_order");
    if (!entry && !runtime) return NULL;
    if (!group) group = count ? text(rotations[0].rule, "calendar_group_id") : "";
    gint64 order = runtime ? integer(event, "schema_entry_source_order", 0) : integer(entry, "source_order", 0);
    JsonArray *alternatives = member_array(entry, "alternatives"); gint alternative = -1; JsonObject *chosen = entry;
    if (runtime) {
        for (guint i = 0; alternatives && i < json_array_get_length(alternatives); ++i)
            if (integer(array_object(alternatives, i), "source_order", 0) == order) { alternative = (gint)i; chosen = array_object(alternatives, i); break; }
    } else if (alternatives && json_array_get_length(alternatives)) {
        guint64 rng = seed_value(seed, text(event, "id"), 121); guint draw = random_below(&rng, 100);
        for (guint i = 0; i < json_array_get_length(alternatives); ++i) {
            guint weight = (guint)integer(array_object(alternatives, i), "weight", 0);
            if (draw < weight) { alternative = (gint)i; chosen = array_object(alternatives, i); order = integer(chosen, "source_order", 0); break; }
            draw -= weight;
        }
    }
    JsonObject *hint = json_object_new();
    json_object_set_int_member(hint, "source_order", order); json_object_set_int_member(hint, "alternative_index", alternative);
    json_object_set_boolean_member(hint, "applied", FALSE);
    JsonObject *transition = entry ? schema_hint_from(chosen, settings, NULL) : NULL;
    /* An alternate entry changes the destination. If it has no automation
     * of its own, the transition assigned to this programme link still
     * applies (kılavuz §03), instead of falling to unrelated AutoDJ rules. */
    if (!transition && entry && chosen != entry)
        transition = schema_hint_from(entry, settings, NULL);
    if (transition) json_object_set_object_member(hint, "transition", transition);
    for (guint i = 0; i < count; ++i)
        if (g_str_equal(text(rotations[i].rule, "calendar_group_id"), group) && rotations[i].outer_order == (guint)order) {
            selection->entry_source = selection->entry_index = i; break;
        }
    return hint;
}

static JsonNode *prepared_programme_internal(JsonObject *event,
    JsonObject *prepared, const gchar *station_id, const gchar *seed, guint limit,
    gint64 start_utc_us, gboolean explicit_start, gboolean single_player, GError **error)
{
    if (!valid_event(event, error)) return NULL;
    gint64 day = event_day(event), event_start = radios_automation_calendar_event_time(event, integer(event, "start_minute", 0));
    gint64 end_utc = radios_automation_calendar_event_time(event, integer(event, "end_minute", 0));
    if (!explicit_start) start_utc_us = event_start;
    if (start_utc_us < event_start || start_utc_us >= end_utc) {
        fail(error, "Gerçek başlangıç zamanı programın UTC aralığı dışında."); return NULL;
    }
    JsonArray *rules = member_array(event, "rotations"), *sources = member_array(prepared, "sources");
    if (!prepared || !station_id || !g_uuid_string_is_valid(station_id) || !g_str_equal(text(prepared, "station_id"), station_id) ||
        integer(prepared, "contract_version", 0) != 1 || !g_str_equal(text(prepared, "kind"), "automation") ||
        !g_str_equal(text(prepared, "timezone"), "UTC") || !sources || json_array_get_length(sources) > 52 ||
        !seed || !*seed || strlen(seed) > 64 || limit < 1 || limit > 200) {
        fail(error, "Takvim hazırlığının kimliği, rastgelelik anahtarı veya önizleme sınırı geçersiz."); return NULL;
    }
    if (!radios_copyright_policy_validate(member_object(prepared, "copyright_policy"), error) ||
        !observed_turns_valid(prepared, start_utc_us, error)) return NULL;
    JsonNode *checkpoint = json_object_get_member(prepared, "policy_selection_checkpoint");
    if (checkpoint) {
        if (!JSON_NODE_HOLDS_OBJECT(checkpoint)) {
            fail(error, "Olasılık tüketim checkpoint'i geçersiz."); return NULL;
        }
        RadioSAutomationPolicyState *consumed = radios_automation_policy_state_new(seed, json_node_get_object(checkpoint), error);
        if (!consumed) return NULL;
        radios_automation_policy_state_free(consumed);
    }
    guint count = json_array_get_length(rules); RotationSource rotations[50] = {{0}};
    JsonArray *diagnostics = json_array_new(); gboolean ready = TRUE;
    for (guint i = 0; ready && i < count; ++i)
        ready = source_init(&rotations[i], array_object(rules, i), prepared, station_id, seed, text(event, "date"), diagnostics, error);
    if (ready) ready = sources_playable(rotations, count, error);
    JsonNode *output = ready ? clone(prepared) : NULL;
    if (ready && !output) { fail(error, "Hazırlanan medya kopyalanamadı."); ready = FALSE; }
    if (ready) {
        JsonObject *response = json_node_get_object(output), *config = json_object_new();
        json_object_remove_member(response, "calendar_turns");
        JsonArray *events = json_array_new(), *tracks = json_array_new(), *blocks = json_array_new();
        JsonObject *current_block = NULL; const gchar *block_source = "";
        json_object_set_array_member(config, "blocks", blocks);
        json_object_set_string_member(config, "title", text(event, "title"));
        /* The programme timing contract is UTC. The calendar keeps its local
         * date, while the compiled date must accompany its UTC clock anchor. */
        GDateTime *utc_date = g_date_time_new_from_unix_utc(event_start / G_USEC_PER_SEC);
        gchar *programme_date = g_date_time_format(utc_date, "%Y-%m-%d"); g_date_time_unref(utc_date);
        json_object_set_string_member(config, "date", programme_date); g_free(programme_date);
        json_object_set_string_member(config, "note", "Takvim rotasyonlarından üretilen doğrulanmış medya taslağı.");
        guint64 budget = (guint64)(end_utc - start_utc_us) / 1000, duration = 0;
        guint64 rng = seed_value(seed, text(event, "id"), 0); GArray *cycle = g_array_new(FALSE, FALSE, sizeof(guint));
        guint position = 0, previous_source_index = G_MAXUINT;
        gboolean named = explicit_start || g_str_equal(text(event, "rotation_selection_mode"), "weighted"), blocked = FALSE;
        for (guint i = 0; i < count; ++i) named |= *text(rotations[i].rule, "calendar_group_id") != 0 || member_object(member_object(rotations[i].rule, "calendar_settings"), "probability_policy") != NULL;
        named |= member_object(event, "probability_policy") != NULL;
        CalendarSelection selection = {.single_player = single_player}; if (named) selection_init(&selection, rotations, count, prepared, event, seed);
        selection.pending_live = selection.entry_source = selection.entry_index = G_MAXUINT;
        for (guint i = 0; i < count; ++i) {
            rotations[i].outer_order = 1;
            for (guint j = 0; j < i; ++j) if (g_str_equal(text(rotations[j].rule, "calendar_group_id"), text(rotations[i].rule, "calendar_group_id"))) ++rotations[i].outer_order;
        }
        JsonObject *entry_hint = named ? programme_entry_resolve(event, rotations, count, seed, &selection) : NULL;
        gint64 previous_start = start_utc_us;
        if (named) selection.end_utc = end_utc;
        JsonObject *previous = NULL, *previous_hint = NULL, *last_row = NULL; gboolean live_emitted = FALSE;
        GPtrArray *track_sources = g_ptr_array_new(); GArray *track_starts = g_array_new(FALSE, FALSE, sizeof(gint64));
        GArray *track_fixed = g_array_new(FALSE, TRUE, sizeof(guint8));
        while (json_array_get_length(events) < limit) {
            guint source_index = 0; gint64 start_utc = previous_start; JsonObject *row; gboolean turn_start = FALSE, fixed = FALSE, window_wait = FALSE, window_early = FALSE;
            if (named && !previous && *text(event, "schema_first_playlist_id")) {
                row = NULL;
                for (guint i = 0; !row && i < count; ++i) {
                    RotationSource *candidate = &rotations[i];
                    if (g_strcmp0(text(candidate->rule, "playlist_id"), text(event, "schema_first_playlist_id"))) continue;
                    for (guint j = 0; !row && j < candidate->slots->len; ++j) {
                        JsonObject *item = g_ptr_array_index(candidate->slots, j);
                        if (candidate_allowed(&selection, candidate, item, start_utc, day, FALSE)) { row = item; source_index = i; }
                    }
                }
                if (row) {
                    source_observe(&rotations[source_index], row); selection.last_turn[source_index] = start_utc;
                    selection.remaining = 0; turn_start = TRUE;
                }
            } else if (named) {
                row = named_next(&selection, rotations, previous, previous_hint, previous_start, day, &source_index, &start_utc, &turn_start);
                /* Continuity before silence: first let one item end past a
                 * window end (soft block end), then relax separation gaps,
                 * then both. The normal rules return for the next item. */
                static const struct { guint relax; gint64 overrun_s; } fallback[] = {
                    {0, 60}, {0, 150}, {0, 300}, {1, 0}, {2, 0}, {1, 300}, {2, 300}};
                for (guint step = 0; !row && step < G_N_ELEMENTS(fallback); ++step) {
                    /* SEMI (one player) overruns only by the rotation's own
                     * soft_end_seconds; without it slots stay strictly fitting. */
                    selection.relax = fallback[step].relax; selection.overrun_us = fallback[step].overrun_s * G_USEC_PER_SEC;
                    row = named_next(&selection, rotations, previous, previous_hint, previous_start, day, &source_index, &start_utc, &turn_start);
                }
                /* Window wait: still nothing legal after a planned row while a
                 * playlist or song window opens later in the block (10–11, then
                 * 11–12). A fitting short item was already preferred above; the
                 * first window with a legal row starts that row at its opening. */
                gint64 floor_before = selection.floor_utc;
                for (guint waits = 0; !row && previous && waits < 50; ++waits) {
                    gint64 open = next_window_open(&selection, rotations, count, previous, previous_hint, previous_start);
                    if (!open) break;
                    selection.relax = 0; selection.overrun_us = 0; selection.floor_utc = MAX(selection.floor_utc, open); selection.window_open_utc = open;
                    row = named_next(&selection, rotations, previous, previous_hint, previous_start, day, &source_index, &start_utc, &turn_start);
                    window_wait = row != NULL;
                }
                selection.window_open_utc = 0;
                /* Continuity before silence (opt-in, like the soft block end): with
                 * soft_end_seconds the opening window's first row starts at the
                 * previous row's end when that is at most soft_end_seconds early,
                 * instead of leaving the gap silent; without it the window is strict. */
                if (window_wait) {
                    gint64 waited_floor = selection.floor_utc; selection.floor_utc = floor_before;
                    gint64 natural = candidate_start(previous, previous_hint, previous_start, row, rotations[source_index].rule, &selection);
                    selection.floor_utc = waited_floor;
                    gint64 soft = integer(member_object(rotations[source_index].rule, "calendar_settings"), "soft_end_seconds", 0) * G_USEC_PER_SEC;
                    if (soft > 0 && natural < start_utc && start_utc - natural <= soft) { start_utc = natural; window_wait = FALSE; window_early = TRUE; }
                }
            } else {
                row = NULL; start_utc = start_utc_us + (gint64)duration * 1000;
                gboolean tested[50] = {FALSE}; guint considered = 0, active = 0;
                for (guint i = 0; i < count; ++i) active += integer(rotations[i].rule, "weight", 1) != 0;
                for (guint attempt = 0; !row && considered < active && attempt < MAX(1u, count * 200u); ++attempt) {
                    if (position == cycle->len) { cycle_build(cycle, rotations, count, &rng); position = 0; }
                    if (!cycle->len) break;
                    source_index = g_array_index(cycle, guint, position++);
                    if (tested[source_index]) continue;
                    tested[source_index] = TRUE; ++considered;
                    RotationSource *candidate = &rotations[source_index];
                    for (guint track_attempt = 0; !row && track_attempt < candidate->slots->len; ++track_attempt) {
                        JsonObject *next = source_next(candidate);
                        if (next && copyright_allowed(candidate, next, start_utc)) row = next;
                    }
                    /* Shared playlists and reshuffles must not air one media twice in a row. */
                    for (guint retry = 1; row && immediate_repeat(last_row, row) && retry < candidate->slots->len; ++retry) {
                        JsonObject *next = source_next(candidate);
                        if (next && copyright_allowed(candidate, next, start_utc)) row = next;
                    }
                    turn_start = position == 1 || source_index != previous_source_index;
                }
            }
            RotationSource *source = &rotations[source_index];
            if (!row) { diagnostic(diagnostics, "calendar_no_legal_candidate", NULL, NULL); blocked = TRUE; break; }
            guint64 milliseconds = (guint64)integer(member_object(member_object(row, "detail"), "version"), "duration_ms", 0);
            guint64 cue_in, cue_out; row_geometry_mode(row, &cue_in, &cue_out, single_player);
            gint64 item_us = (gint64)((cue_out - cue_in) * G_USEC_PER_SEC / 48000);
            gint64 overrun_used = named ? effective_overrun(&selection, source->rule) : 0;
            guint relax_used = named ? effective_relax(&selection, source->rule) : 0;
            if ((!named && milliseconds > budget - duration) || (named && start_utc + item_us > end_utc + overrun_used)) {
                diagnostic(diagnostics, "calendar_end_boundary", source->rule, row); break;
            }
            gboolean overrun_end = named && start_utc + item_us > end_utc;
            /* A live step chosen before this row: its block precedes this row's
             * block (the outer sequence continues from the next playlist). */
            gboolean after_live = selection.pending_live != G_MAXUINT;
            if (after_live) {
                if (live_step_emit(&rotations[selection.pending_live], selection.previous_rule, blocks, tracks, track_fixed, diagnostics)) previous_hint = NULL;
                live_emitted = TRUE; current_block = NULL; block_source = ""; selection.pending_live = G_MAXUINT;
            }
            gchar *id = g_uuid_string_random(); JsonObject *entry = programme_entry(row, source->rule, id);
            /* A source is a deliberate event boundary, including two music
             * playlists in succession. Retain that boundary for source-level
             * automation, the queue editor and live-event scheduling. */
            const gchar *kind = text(entry, "kind"), *source_id = text(source->rule, "id");
            if (!current_block || !g_str_equal(text(current_block, "kind"), kind) ||
                !g_str_equal(block_source, source_id)) {
                current_block = json_object_new(); gchar *block_id = radios_programme_block_id_for(id);
                json_object_set_string_member(current_block, "id", block_id); g_free(block_id);
                json_object_set_string_member(current_block, "kind", kind);
                const gchar *source_title = source->source_name;
                gchar *title = source_title && *source_title ?
                    g_strdup_printf("%s · %s", radios_programme_kind_label(kind), source_title) :
                    g_strdup(radios_programme_event_title(kind));
                gchar *bounded = bounded_text(title, 160); g_free(title);
                json_object_set_string_member(current_block, "title", bounded); g_free(bounded);
                json_object_set_string_member(current_block, "start_time", "");
                json_object_set_string_member(current_block, "note", "");
                json_array_add_object_element(blocks, current_block); block_source = source_id;
            }
            if (g_str_equal(kind, "music") && !g_str_equal(block_source, source_id))
                json_object_set_string_member(current_block, "title", radios_programme_event_title("music"));
            json_object_set_string_member(entry, "block_id", text(current_block, "id"));
            JsonNode *copy = clone(row); JsonObject *track = json_node_get_object(copy);
            if (selection.probability_enabled) {
                JsonObject *root = source_probability(&selection, source);
                JsonObject *effective = radios_automation_policy_effective(root, text(source->rule, "calendar_rotation_id"), text(source->rule, "playlist_id"), text(row, "entry_id"));
                json_object_unref(root);
                JsonObject *context = json_object_new();
                const gchar *automation_id = text(source->rule, "calendar_rotation_id");
                if (!*automation_id) automation_id = text(source->rule, "id");
                json_object_set_string_member(context, "automation_id", automation_id);
                json_object_set_string_member(context, "playlist_id", text(source->rule, "playlist_id"));
                json_object_set_string_member(context, "entry_id", text(row, "entry_id"));
                json_object_set_string_member(context, "source_id", text(source->rule, "id"));
                JsonNode *context_copy = clone(context);
                json_object_set_object_member(entry, "policy_context", context); json_object_set_member(track, "policy_context", context_copy);
                if (effective) { JsonNode *policy_copy = clone(effective); json_object_set_object_member(entry, "probability_policy", effective); json_object_set_member(track, "probability_policy", policy_copy); }
                JsonObject *checkpoint = radios_automation_policy_state_snapshot(selection.probability_state);
                json_object_set_object_member(track, "policy_selection_checkpoint", checkpoint);
            }
            json_array_add_object_element(events, entry);
            json_object_set_string_member(track, "entry_id", id); json_object_set_int_member(track, "ordinal", json_array_get_length(tracks));
            json_object_set_string_member(track, "calendar_source_name", source->source_name);
            json_object_set_string_member(track, "calendar_playlist_id", text(source->rule, "playlist_id"));
            JsonObject *metadata = member_object(track, "metadata"); json_object_set_string_member(metadata, "start_time", "");
            json_object_set_string_member(metadata, "start_policy", "sequential"); json_object_set_int_member(metadata, "max_wait_ms", 0);
            JsonObject *routing = choose_routing(source->rule, row);
            if (routing) json_object_set_object_member(track, "calendar_routing", routing);
            if (named || routing) {
                JsonObject *constraints = calendar_constraints(event, source->rule, source_policy(source, row), day, turn_start, observed_turn(prepared, text(source->rule, "id")));
                /* An early opening row is bound to where it really starts. */
                if (window_early) json_object_set_int_member(constraints, "earliest_utc_us", MIN(integer(constraints, "earliest_utc_us", 0), start_utc));
                /* A last-resort pick carries its relaxed bounds, so native
                 * timing revalidates exactly what was chosen. */
                if (overrun_used) /* slack: live prepare round trip and beatmatch tempo stretch */
                    json_object_set_int_member(constraints, "latest_end_utc_us", MAX(integer(constraints, "latest_end_utc_us", 0),
                        start_utc + item_us + MAX(30 * G_USEC_PER_SEC, item_us / 5)));
                if (relax_used >= 1) {
                    json_object_set_int_member(constraints, "artist_gap_ms", 0); json_object_set_int_member(constraints, "album_gap_ms", 0);
                }
                if (relax_used >= 2)
                    json_object_set_int_member(constraints, "track_gap_ms", integer(source_policy(source, row), "interval_minutes", 0) * 60000);
                /* After a live step with a duration rule the selection plans this
                 * row at the step's end (selection.floor_utc); playout counts the
                 * wait from the previous row's actual end (planner.c hold_us), so
                 * no absolute bound is placed here: pausing or a late end must
                 * not shorten the step (PDF s.15). */
                json_object_set_object_member(track, "calendar_constraints", constraints);
                json_object_set_string_member(track, "calendar_source_id", text(source->rule, "id"));
            }
            /* A waited row starts at its window (earliest_utc_us) in native
             * playout too, never at the previous row's end. */
            if (window_wait) { json_object_set_boolean_member(track, "calendar_window_wait", TRUE); diagnostic(diagnostics, "calendar_window_wait", source->rule, row); }
            if (window_early) diagnostic(diagnostics, "calendar_window_early", source->rule, row);
            if (relax_used || overrun_end)
                diagnostic(diagnostics, relax_used ? "calendar_rules_relaxed" : "calendar_soft_end_overrun", source->rule, row);
            previous_source_index = source_index;
            if (named) {
                JsonObject *hint = choose_transition(source);
                if (hint) json_object_set_object_member(track, "calendar_transition", hint);
                if (entry_hint && !json_array_get_length(tracks)) {
                    /* The programme's first row: how the previous programme hands
                     * over (programme_entry); the scope driver reads this hint. */
                    json_object_set_boolean_member(entry_hint, "applied", source_index == selection.entry_index);
                    JsonObject *transition = member_object(entry_hint, "transition");
                    if (transition) { json_object_set_member(track, "calendar_transition", clone(transition)); fixed = TRUE; }
                    json_object_set_member(track, "programme_entry", clone(entry_hint));
                }
                json_object_set_string_member(track, "calendar_source_id", text(source->rule, "id"));
                const gchar *provenance[] = {"calendar_group_id", "calendar_rotation_id"};
                for (guint j = 0; j < G_N_ELEMENTS(provenance); ++j)
                    if (*text(source->rule, provenance[j])) json_object_set_string_member(track, provenance[j], text(source->rule, provenance[j]));
                JsonObject *media = member_object(member_object(row, "detail"), "media");
                const gchar *fields[] = {"title", "artist", "album", "kind"};
                for (guint j = 0; j < G_N_ELEMENTS(fields); ++j) json_object_set_string_member(metadata, fields[j], text(entry, fields[j]));
                history_observe(&selection.history, text(media, "_id"), text(entry, "artist"), text(entry, "album"), start_utc);
                /* The boundary before this row is final: advance its kind's preset cursor. */
                if (previous) { JsonObject *used = preset_transition(selection.previous_settings, event, previous, row, previous_start, single_player, selection.preset_counters, TRUE); if (used) json_object_unref(used); }
                previous = row; previous_hint = member_object(track, "calendar_transition"); previous_start = start_utc;
                selection.previous_policy = member_object(member_object(source->rule, "calendar_settings"), "kind_transitions");
                selection.previous_settings = member_object(source->rule, "calendar_settings");
                selection.last_short = short_kind(row); g_strlcpy(selection.last_media, text(media, "_id"), sizeof selection.last_media);
                selection.relax = 0; selection.overrun_us = 0; selection.floor_utc = 0; selection.previous_rule = source->rule;
            }
            guint8 fixed_flag = fixed;
            json_array_add_object_element(tracks, json_object_ref(track)); json_node_free(copy); g_free(id); duration += milliseconds;
            g_ptr_array_add(track_sources, source->rule); g_array_append_val(track_starts, start_utc); g_array_append_val(track_fixed, fixed_flag); last_row = row;
            if (overrun_end) break; /* the soft end item closes this block */
        }
        /* A live step chosen after the last media row still enters the flow. */
        if (selection.pending_live != G_MAXUINT && json_array_get_length(events)) {
            live_step_emit(&rotations[selection.pending_live], selection.previous_rule, blocks, tracks, track_fixed, diagnostics);
            live_emitted = TRUE; selection.pending_live = G_MAXUINT;
        }
        /* A programme flagged Live event without a live step of its own: every
         * block carries the programme's live binding for the event list's L/R. */
        if (!live_emitted && member_object(event, "live"))
            for (guint i = 0; i < json_array_get_length(blocks); ++i)
                json_object_set_object_member(array_object(blocks, i), "live", live_summary(member_object(event, "live")));
        /* At a source boundary the schema flow ("2 2 2", then the link default)
         * decides first; otherwise a rotation-level AutoDJ policy sets each
         * boundary by content kind and designed presets (AutoDJ otomasyonu)
         * take precedence for their kinds. The preset cursor advances on every
         * boundary, as the selection estimate did. Boundaries fixed by a live
         * step or the programme entry keep their hint. */
        guint preset_counters[RADIOS_TRANSITION_KIND_COUNT] = {0};
        for (guint i = 0; i < json_array_get_length(tracks); ++i) {
            JsonObject *from_rule = g_ptr_array_index(track_sources, i), *settings = member_object(from_rule, "calendar_settings");
            JsonObject *track = array_object(tracks, i), *next = i + 1 < json_array_get_length(tracks) ? array_object(tracks, i + 1) : NULL;
            JsonObject *to_rule = next ? g_ptr_array_index(track_sources, i + 1) : NULL;
            JsonObject *hint = boundary_kind_transition(settings, event, track, next, g_array_index(track_starts, gint64, i), single_player, preset_counters, TRUE);
            if (g_array_index(track_fixed, guint8, i)) { if (hint) json_object_unref(hint); continue; }
            gint order = 0; const gchar *rule_name = NULL;
            JsonObject *schema = next && to_rule != from_rule && !json_object_has_member(track, "live_step") ?
                schema_boundary_hint(from_rule, to_rule, autodj_kind(next), &order, &rule_name) : NULL;
            if (schema) {
                if (hint) json_object_unref(hint);
                hint = schema; JsonObject *info = json_object_new();
                json_object_set_string_member(info, "rule", rule_name); json_object_set_int_member(info, "order", order);
                json_object_set_object_member(track, "schema_boundary", info);
            }
            if (hint) json_object_set_object_member(track, "calendar_transition", hint);
        }
        g_ptr_array_unref(track_sources); g_array_unref(track_starts); g_array_unref(track_fixed);
        g_clear_pointer(&entry_hint, json_object_unref);
        g_array_unref(cycle); if (named) selection_clear(&selection);
        if (!json_array_get_length(events)) {
            const gchar *message = blocked ? "Tekrar, saat, sıklık ve çalma aralığı kuralları içinde uygun parça yok." : "Seçilen saat aralığına sığan doğrulanmış medya yok.";
            if (explicit_start) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK, message);
            else fail(error, message);
            ready = FALSE;
        } else {
            JsonObject *timing = json_object_new(); GDateTime *utc_time = g_date_time_new_from_unix_utc(event_start / G_USEC_PER_SEC);
            gchar *start = g_date_time_format(utc_time, "%H:%M"); g_date_time_unref(utc_time);
            json_object_set_string_member(timing, "event_id", text(array_object(events, 0), "id"));
            json_object_set_string_member(timing, "start_time", start); json_object_set_string_member(timing, "target_time", start);
            json_object_set_string_member(timing, "policy", "finish"); json_object_set_int_member(timing, "max_wait_ms", 0);
            json_object_set_object_member(config, "timing", timing); g_free(start);
        }
        json_object_set_array_member(config, "events", events); json_object_set_object_member(response, "config", config);
        json_object_set_string_member(response, "kind", "programme"); json_object_set_array_member(response, "tracks", tracks);
        json_object_set_array_member(response, "sources", json_array_new()); json_object_set_null_member(response, "programme");
        json_object_set_null_member(response, "history"); json_object_set_array_member(response, "diagnostics", diagnostics); diagnostics = NULL;
    }
    for (guint i = 0; i < count; ++i) source_clear(&rotations[i]);
    if (diagnostics) json_array_unref(diagnostics);
    if (!ready) g_clear_pointer(&output, json_node_free);
    return output;
}
JsonNode *radios_automation_calendar_prepared_programme(JsonObject *event,
    JsonObject *prepared, const gchar *station_id, const gchar *seed, guint limit, GError **error)
{ return prepared_programme_internal(event, prepared, station_id, seed, limit, 0, FALSE, FALSE, error); }

JsonNode *radios_automation_calendar_prepared_programme_at(JsonObject *event,
    JsonObject *prepared, const gchar *station_id, const gchar *seed, guint limit, gint64 start_utc_us, GError **error)
{ return prepared_programme_internal(event, prepared, station_id, seed, limit, start_utc_us, TRUE, FALSE, error); }

JsonNode *radios_automation_calendar_prepared_programme_at_mode(JsonObject *event,
    JsonObject *prepared, const gchar *station_id, const gchar *seed, guint limit, gint64 start_utc_us,
    gboolean single_player, GError **error)
{ return prepared_programme_internal(event, prepared, station_id, seed, limit, start_utc_us, TRUE, single_player, error); }

static gboolean hint_valid(JsonObject *hint)
{
    /* Mode 3 is the native MANUAL hold before a live step: no overlap or extras. */
    if (!hint || !range(hint, "mode", 0, 3) || !range(hint, "overlap_ms", 0, 30000)) return FALSE;
    if (integer(hint, "mode", 0) == 3) return json_object_get_size(hint) == 2 && integer(hint, "overlap_ms", 0) == 0;
    gboolean has_sync = json_object_has_member(hint, "beat_sync"), sync = FALSE;
    gboolean has_effect = json_object_has_member(hint, "effect"), has_shape = json_object_has_member(hint, "shape");
    const gchar *effect = has_effect ? text(hint, "effect") : "none";
    gint64 mode = integer(hint, "mode", 0), overlap = integer(hint, "overlap_ms", 0);
    return json_object_get_size(hint) == 2u + has_sync + has_effect + has_shape &&
        (!has_shape || (mode == 1 && overlap > 0 && radios_transition_preset_valid(member_object(hint, "shape"), FALSE))) &&
        (!has_sync || boolean(hint, "beat_sync", &sync)) &&
        (!sync || (mode == 1 && overlap > 0)) &&
        (g_str_equal(effect, "none") || (g_str_equal(effect, "lowpass") && overlap > 0)) &&
        (mode != 0 || (overlap == 0 && !sync && g_str_equal(effect, "none")));
}
static gboolean routing_hint_valid(JsonObject *hint)
{ return hint && json_object_get_size(hint) == 1 && deck_ids_valid(member_array(hint, "decks")); }
/* live_step: the live summary plus block_id, outer_order 1..50 and inner_order 0..99. */
static gboolean live_step_hint_valid(JsonObject *hint)
{
    if (!hint || json_object_get_size(hint) != 8 || !g_uuid_string_is_valid(text(hint, "block_id")) ||
        !range(hint, "outer_order", 1, 50) || !range(hint, "inner_order", 0, 99)) return FALSE;
    JsonObject *live = json_object_new(); const gchar *members[] = {"label", "channels", "automation_id", "end_rule", "duration_ms"};
    for (guint i = 0; i < G_N_ELEMENTS(members); ++i)
        if (json_object_has_member(hint, members[i])) json_object_set_member(live, members[i], json_node_copy(json_object_get_member(hint, members[i])));
    gboolean valid = live_valid(live, FALSE, FALSE); json_object_unref(live); return valid;
}
static gboolean programme_entry_hint_valid(JsonObject *hint)
{
    gboolean applied, has_transition = hint && json_object_has_member(hint, "transition");
    return hint && json_object_get_size(hint) == 3u + has_transition && range(hint, "source_order", 1, 50) &&
        range(hint, "alternative_index", -1, 7) && boolean(hint, "applied", &applied) &&
        (!has_transition || hint_valid(member_object(hint, "transition")));
}
static gboolean schema_boundary_valid(JsonObject *hint)
{
    return hint && json_object_get_size(hint) == 2 && range(hint, "order", 0, 99) &&
        (g_str_equal(text(hint, "rule"), "steps") || g_str_equal(text(hint, "rule"), "link"));
}

static gboolean constraints_valid(JsonObject *hint)
{
    gboolean turn_start;
    const gint64 first_utc = G_GINT64_CONSTANT(-62135596800000000), final_utc = G_GINT64_CONSTANT(253402300800000000);
    if (!hint || json_object_get_size(hint) != 7 ||
        !range(hint, "earliest_utc_us", first_utc, final_utc) || !range(hint, "latest_end_utc_us", first_utc, final_utc) ||
        !boolean(hint, "source_turn_start", &turn_start)) return FALSE;
    gint64 earliest = integer(hint, "earliest_utc_us", 0), latest = integer(hint, "latest_end_utc_us", 0);
    /* One local day (25 h across a DST change), and a soft end may carry the
     * last item of a midnight block past 24:00: two days bound the span. */
    if (earliest >= latest || latest - earliest > G_GINT64_CONSTANT(172800000000)) return FALSE;
    const gchar *gaps[] = {"track_gap_ms", "artist_gap_ms", "album_gap_ms", "source_interval_ms"};
    for (guint i = 0; i < G_N_ELEMENTS(gaps); ++i) if (!range(hint, gaps[i], 0, 604800000)) return FALSE;
    return TRUE;
}
static gboolean same_optional(JsonObject *left, JsonObject *right, const gchar *field)
{
    JsonNode *a = json_object_get_member(left, field), *b = json_object_get_member(right, field);
    return (!a && !b) || (a && b && json_node_equal(a, b));
}
JsonNode *radios_automation_calendar_merge_selection(JsonObject *verified, JsonObject *selection, GError **error)
{
    JsonArray *fresh = member_array(verified, "tracks"), *chosen = member_array(selection, "tracks");
    const gchar *station = text(selection, "station_id");
    if (!verified || !selection || !g_uuid_string_is_valid(station) || !g_str_equal(station, text(verified, "station_id")) ||
        integer(verified, "contract_version", 0) != 1 || integer(selection, "contract_version", 0) != 1 ||
        !g_str_equal(text(verified, "kind"), "programme") || !g_str_equal(text(selection, "kind"), "programme") ||
        !g_str_equal(text(verified, "timezone"), "UTC") || !g_str_equal(text(selection, "timezone"), "UTC") ||
        !fresh || !chosen || !json_array_get_length(fresh) || json_array_get_length(fresh) > 200 || json_array_get_length(fresh) != json_array_get_length(chosen)) {
        fail(error, "Program hazırlığı, istasyon veya seçilmiş parça sayısı değişti."); return NULL;
    }
    JsonNode *result = clone(verified); if (!result) { fail(error, "Doğrulanan program kopyalanamadı."); return NULL; }
    JsonArray *output = member_array(json_node_get_object(result), "tracks"); gboolean valid = TRUE;
    for (guint i = 0; valid && i < json_array_get_length(fresh); ++i) {
        JsonObject *a = array_object(fresh, i), *b = array_object(chosen, i), *target = array_object(output, i);
        JsonObject *ad = member_object(a, "detail"), *bd = member_object(b, "detail");
        JsonObject *am = member_object(ad, "media"), *bm = member_object(bd, "media"), *av = member_object(ad, "version"), *bv = member_object(bd, "version");
        valid = media_eligible(a) && media_eligible(b) && row_valid(a, station) && row_valid(b, station) &&
            g_str_equal(text(a, "entry_id"), text(b, "entry_id")) && integer(a, "ordinal", -1) == (gint64)i && integer(b, "ordinal", -1) == (gint64)i &&
            g_str_equal(text(am, "_id"), text(bm, "_id")) && g_str_equal(text(av, "_id"), text(bv, "_id")) &&
            g_str_equal(text(am, "media_file_id"), text(bm, "media_file_id")) && g_str_equal(text(av, "sha256"), text(bv, "sha256")) &&
            g_str_equal(text(am, "item_id"), text(bm, "item_id")) && integer(av, "duration_ms", 0) == integer(bv, "duration_ms", 0) &&
            g_str_equal(row_kind(a), row_kind(b)) && same_optional(ad, bd, "markers") && same_optional(ad, bd, "analysis");
        if (valid) valid = same_optional(a, b, "probability_policy") && same_optional(a, b, "policy_context");
        if (!valid) break;
        if (json_object_has_member(b, "policy_selection_checkpoint")) {
            RadioSAutomationPolicyState *state = radios_automation_policy_state_new("selection", member_object(b, "policy_selection_checkpoint"), NULL);
            valid = state != NULL; radios_automation_policy_state_free(state);
            if (!valid) break;
            json_object_set_member(target, "policy_selection_checkpoint", clone(member_object(b, "policy_selection_checkpoint")));
        }
        json_object_remove_member(target, "calendar_transition");
        if (json_object_has_member(b, "calendar_transition")) {
            JsonObject *hint = member_object(b, "calendar_transition"); valid = hint_valid(hint);
            if (valid) json_object_set_member(target, "calendar_transition", clone(hint));
        }
        /* Schema flow hints travel with the row: the live step before the next
         * row, the programme entry of the first row and the matched boundary. */
        const gchar *flow[] = {"live_step", "programme_entry", "schema_boundary"};
        for (guint j = 0; valid && j < G_N_ELEMENTS(flow); ++j) {
            json_object_remove_member(target, flow[j]);
            if (!json_object_has_member(b, flow[j])) continue;
            JsonObject *hint = member_object(b, flow[j]);
            valid = j == 0 ? live_step_hint_valid(hint) : j == 1 ? programme_entry_hint_valid(hint) : schema_boundary_valid(hint);
            if (valid) json_object_set_member(target, flow[j], clone(hint));
        }
        json_object_remove_member(target, "calendar_routing");
        if (valid && json_object_has_member(b, "calendar_routing")) {
            JsonObject *hint = member_object(b, "calendar_routing"); valid = routing_hint_valid(hint);
            if (valid) json_object_set_member(target, "calendar_routing", clone(hint));
        }
        json_object_remove_member(target, "calendar_constraints");
        if (valid && json_object_has_member(b, "calendar_constraints")) {
            JsonObject *hint = member_object(b, "calendar_constraints");
            valid = constraints_valid(hint) && g_uuid_string_is_valid(text(b, "calendar_source_id"));
            if (valid) json_object_set_member(target, "calendar_constraints", clone(hint));
        }
        /* A row the selection waited for keeps that wait; it needs its bounds. */
        json_object_remove_member(target, "calendar_window_wait");
        if (valid && json_object_has_member(b, "calendar_window_wait")) {
            valid = flag(b, "calendar_window_wait") && json_object_has_member(target, "calendar_constraints");
            if (valid) json_object_set_boolean_member(target, "calendar_window_wait", TRUE);
        }
        const gchar *provenance[] = {"calendar_source_id", "calendar_group_id", "calendar_rotation_id", "calendar_playlist_id"};
        for (guint j = 0; valid && j < G_N_ELEMENTS(provenance); ++j) {
            json_object_remove_member(target, provenance[j]);
            if (json_object_has_member(b, provenance[j])) {
                valid = g_uuid_string_is_valid(text(b, provenance[j]));
                if (valid) json_object_set_string_member(target, provenance[j], text(b, provenance[j]));
            }
        }
        json_object_remove_member(target, "calendar_source_name");
        if (valid && json_object_has_member(b, "calendar_source_name")) {
            JsonNode *name_node = json_object_get_member(b, "calendar_source_name");
            const gchar *name = text(b, "calendar_source_name");
            valid = JSON_NODE_HOLDS_VALUE(name_node) && json_node_get_value_type(name_node) == G_TYPE_STRING &&
                g_utf8_validate(name, -1, NULL) && g_utf8_strlen(name, -1) <= 800;
            if (valid) json_object_set_string_member(target, "calendar_source_name", name);
        }
    }
    if (!valid) { json_node_free(result); fail(error, "Program sırası, parça UUID'si, doğrulanmış medya bağları, cue zamanı veya rotasyon geçişi değişti."); return NULL; }
    return result;
}

JsonNode *radios_automation_calendar_programme(JsonObject *event, JsonObject *prepared,
    const gchar *station_id, const gchar *seed, guint limit, GError **error)
{
    JsonNode *response = radios_automation_calendar_prepared_programme(event, prepared, station_id, seed, limit, error);
    if (!response) return NULL;
    JsonNode *result = clone(member_object(json_node_get_object(response), "config")); json_node_free(response); return result;
}
