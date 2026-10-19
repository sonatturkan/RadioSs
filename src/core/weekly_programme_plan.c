#include "radios/weekly_programme_plan.h"
#include "radios/automation_calendar_plan.h"
#include "radios/programme_model.h"
#include "radios/platform.h"
#include <stdio.h>
#include <string.h>

#define DAY_US G_GINT64_CONSTANT(86400000000)
#define FIRST_US G_GINT64_CONSTANT(-62135596800000000)
#define FINAL_US G_GINT64_CONSTANT(253402300800000000)

typedef struct { guint index; gint64 start, end; gboolean playable; } WeeklyWindow;
static const gchar *string(JsonObject *o, const gchar *key)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : "";
}
static gint64 number(JsonObject *o, const gchar *key, gint64 fallback)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_INT64 ? json_node_get_int(n) : fallback;
}
static JsonArray *array(JsonObject *o, const gchar *key)
{ JsonNode *n = o ? json_object_get_member(o, key) : NULL; return n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : NULL; }
static JsonObject *object(JsonObject *o, const gchar *key)
{ JsonNode *n = o ? json_object_get_member(o, key) : NULL; return n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL; }
static JsonObject *at(JsonArray *a, guint i)
{ JsonNode *n = a && i < json_array_get_length(a) ? json_array_get_element(a, i) : NULL; return n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL; }
static gboolean fail(GError **error, const gchar *message)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message); return FALSE; }
static gboolean keys(JsonObject *o, const gchar *const *allowed, guint count, guint minimum)
{
    if (!o || json_object_get_size(o) < minimum || json_object_get_size(o) > count) return FALSE;
    GList *fields = json_object_get_members(o); gboolean valid = TRUE;
    for (GList *it = fields; valid && it; it = it->next) {
        gboolean known = FALSE;
        for (guint i = 0; i < count; ++i) known |= g_str_equal(it->data, allowed[i]);
        valid = known;
    }
    g_list_free(fields); return valid;
}
static GDateTime *day(const gchar *date)
{
    guint year = 0, month = 0, d = 0;
    if (!date || strlen(date) != 10 || !g_regex_match_simple("^[0-9]{4}-[0-9]{2}-[0-9]{2}$", date, 0, 0) ||
        sscanf(date, "%u-%u-%u", &year, &month, &d) != 3 || !g_date_valid_dmy((GDateDay)d, (GDateMonth)month, (GDateYear)year) || year > 9999) return NULL;
    return g_date_time_new_utc((gint)year, (gint)month, (gint)d, 0, 0, 0);
}
static gchar *date_text(GDateTime *date)
{
    /* GLib's %Y is not zero-padded for years below 1000. Persisted dates
     * always require the exact four-digit ISO service-date contract. */
    return g_strdup_printf("%04d-%02d-%02d", g_date_time_get_year(date),
        g_date_time_get_month(date), g_date_time_get_day_of_month(date));
}
static JsonNode *clone(JsonObject *o)
{
    JsonNode *n = json_node_new(JSON_NODE_OBJECT); json_node_set_object(n, o);
    gchar *encoded = json_to_string(n, FALSE); JsonParser *parser = json_parser_new();
    gboolean loaded = json_parser_load_from_data(parser, encoded, -1, NULL);
    JsonNode *result = loaded ? json_node_copy(json_parser_get_root(parser)) : NULL;
    g_free(encoded); json_node_free(n); g_object_unref(parser); return result;
}
static gboolean identity(GHashTable *ids, const gchar *id)
{
    if (!g_uuid_string_is_valid(id)) return FALSE;
    gchar *canonical = g_ascii_strdown(id, -1);
    if (g_hash_table_contains(ids, canonical)) { g_free(canonical); return FALSE; }
    g_hash_table_add(ids, canonical); return TRUE;
}
static gchar *unused_identity(GHashTable *ids)
{
    for (guint i = 0; i <= g_hash_table_size(ids); ++i) {
        gchar *candidate = g_strdup_printf("ffffffff-ffff-4fff-8fff-%012u", i);
        if (identity(ids, candidate)) return candidate;
        g_free(candidate);
    }
    return NULL;
}
/* Repeat civil dates in the calendar timezone. Adding a fixed UTC week
 * would move a local programme by an hour at a daylight-saving boundary. */
static gint64 zoned_event_time(JsonObject *calendar, JsonObject *event, gint64 minute, gint64 shift_days)
{
    GDateTime *source = day(string(event, "date"));
    GDateTime *date = source && shift_days <= G_MAXINT ? g_date_time_add_days(source, (gint)shift_days) : NULL;
    if (source) g_date_time_unref(source);
    if (!date) return G_MININT64;
    JsonObject *probe = json_object_new(); gchar *stamp = date_text(date); g_date_time_unref(date);
    json_object_set_string_member(probe, "date", stamp); g_free(stamp);
    json_object_set_string_member(probe, "timezone", json_object_has_member(calendar, "timezone") ? string(calendar, "timezone") : "UTC");
    gint64 result = radios_automation_calendar_event_time(probe, minute); json_object_unref(probe); return result;
}
/* Kılavuz §04 live event objects (contract §1b and §2): every present member
 * is strictly typed and bounded; a live source row needs the object and never
 * a playlist. Absent members keep the backend defaults (manual end). */
static gboolean typed(JsonObject *o, const gchar *key, GType type)
{
    JsonNode *n = json_object_get_member(o, key);
    return !n || (JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == type);
}
static gboolean live_valid(JsonObject *live)
{
    const gchar *allowed[] = {"title", "label", "channels", "automation_id", "end_rule", "duration_ms"};
    if (!live || !keys(live, allowed, G_N_ELEMENTS(allowed), 0) || !typed(live, "title", G_TYPE_STRING) ||
        !typed(live, "label", G_TYPE_STRING) || !typed(live, "automation_id", G_TYPE_STRING) ||
        !typed(live, "end_rule", G_TYPE_STRING) || !typed(live, "duration_ms", G_TYPE_INT64)) return FALSE;
    const gchar *title = string(live, "title"), *label = string(live, "label"), *automation = string(live, "automation_id");
    if (!g_utf8_validate(title, -1, NULL) || g_utf8_strlen(title, -1) > 160) return FALSE;
    if (json_object_has_member(live, "label") && !g_str_equal(label, "dj") && !g_str_equal(label, "talk") && !g_str_equal(label, "music")) return FALSE;
    if (*automation && !g_uuid_string_is_valid(automation)) return FALSE;
    if (json_object_has_member(live, "channels")) {
        JsonArray *channels = array(live, "channels"); guint32 seen = 0;
        if (!channels || json_array_get_length(channels) > 32) return FALSE;
        for (guint i = 0; i < json_array_get_length(channels); ++i) {
            JsonNode *n = json_array_get_element(channels, i);
            if (!JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_INT64) return FALSE;
            gint64 channel = json_node_get_int(n);
            if (channel < 1 || channel > 32 || (seen & (1u << (channel - 1)))) return FALSE;
            seen |= 1u << (channel - 1);
        }
    }
    const gchar *rule = json_object_has_member(live, "end_rule") ? string(live, "end_rule") : "manual";
    gint64 duration = number(live, "duration_ms", 0);
    if (!g_str_equal(rule, "manual") && !g_str_equal(rule, "duration")) return FALSE;
    if (duration < 0 || duration > 86400000 || (g_str_equal(rule, "duration") && duration <= 0)) return FALSE;
    return TRUE;
}
static gboolean source_live_valid(JsonObject *source)
{
    if (!typed(source, "source_type", G_TYPE_STRING) || !typed(source, "start_date", G_TYPE_STRING) || !typed(source, "end_date", G_TYPE_STRING)) return FALSE;
    const gchar *type = json_object_has_member(source, "source_type") ? string(source, "source_type") : "playlist";
    if (!g_str_equal(type, "playlist") && !g_str_equal(type, "live")) return FALSE;
    if (g_str_equal(type, "live") && json_object_has_member(source, "playlist_id")) return FALSE;
    if ((g_str_equal(type, "live") || json_object_has_member(source, "live")) && !live_valid(object(source, "live"))) return FALSE;
    GDateTime *from = json_object_has_member(source, "start_date") ? day(string(source, "start_date")) : NULL;
    GDateTime *to = json_object_has_member(source, "end_date") ? day(string(source, "end_date")) : NULL;
    gboolean valid = (!json_object_has_member(source, "start_date") || from) && (!json_object_has_member(source, "end_date") || to) &&
        (!from || !to || g_date_time_compare(from, to) <= 0);
    if (from) g_date_time_unref(from);
    if (to) g_date_time_unref(to);
    return valid;
}
static gint64 civil_now(JsonObject *calendar, gint64 utc)
{
    const gchar *identifier = json_object_has_member(calendar, "timezone") ? string(calendar, "timezone") : "UTC";
    GTimeZone *zone = radios_time_zone_new(identifier);
    if (!zone) return G_MININT64;
    GDateTime *stamp = g_date_time_new_from_unix_utc(utc / G_USEC_PER_SEC);
    GDateTime *local = stamp ? g_date_time_to_timezone(stamp, zone) : NULL;
    GDateTime *civil = local ? g_date_time_new_utc(g_date_time_get_year(local), g_date_time_get_month(local),
        g_date_time_get_day_of_month(local), g_date_time_get_hour(local), g_date_time_get_minute(local), g_date_time_get_second(local)) : NULL;
    gint64 result = civil ? g_date_time_to_unix(civil) * G_USEC_PER_SEC : G_MININT64;
    if (stamp) g_date_time_unref(stamp);
    if (local) g_date_time_unref(local);
    if (civil) g_date_time_unref(civil);
    g_time_zone_unref(zone); return result;
}
static void occurrence_times(JsonObject *calendar, WeeklyWindow window, gint64 shift, gint64 *start, gint64 *end)
{
    JsonObject *event = at(array(calendar, "events"), window.index);
    *start = zoned_event_time(calendar, event, number(event, "start_minute", 0), shift / DAY_US);
    *end = zoned_event_time(calendar, event, number(event, "end_minute", 0), shift / DAY_US);
}
static gint window_compare(gconstpointer left, gconstpointer right)
{
    const WeeklyWindow *a = left, *b = right;
    return a->start != b->start ? (a->start < b->start ? -1 : 1) : a->index < b->index ? -1 : a->index > b->index;
}
static gboolean calendar_windows(JsonObject *calendar, gint64 anchor, gint64 period, GArray *windows, GError **error)
{
    const gchar *calendar_fields[] = {"schema", "events", "rotation_library", "timezone"};
    JsonArray *events = array(calendar, "events"), *library = array(calendar, "rotation_library");
    if (!keys(calendar, calendar_fields, G_N_ELEMENTS(calendar_fields), 2) || number(calendar, "schema", 0) != 1 ||
        !events || json_array_get_length(events) > 500 || (json_object_has_member(calendar, "rotation_library") && !library) ||
        (library && json_array_get_length(library) > 200)) return fail(error, "Haftalık programın takvim biçimi geçersiz.");
    if (json_object_has_member(calendar, "timezone")) {
        GTimeZone *zone = radios_time_zone_new(string(calendar, "timezone"));
        if (!zone) return fail(error, "Haftalık programın saat dilimi geçersiz.");
        g_time_zone_unref(zone);
    }
    GHashTable *ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *definitions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL); gboolean valid = TRUE;
    for (guint i = 0; valid && library && i < json_array_get_length(library); ++i) {
        JsonObject *definition = at(library, i); JsonArray *sources = array(definition, "sources");
        const gchar *definition_fields[] = {"id", "name", "sources", "settings"};
        valid = keys(definition, definition_fields, G_N_ELEMENTS(definition_fields), 4) && identity(ids, string(definition, "id")) && sources && json_array_get_length(sources) <= 50;
        for (guint j = 0; valid && j < json_array_get_length(sources); ++j) {
            const gchar *source_fields[] = {"id", "playlist_id", "frequency", "random", "track_random", "advert_random", "sweeper_random", "media_kind", "autodj", "weight", "count", "start_minute", "end_minute", "interval_minutes", "settings", "track_selection_mode", "track_rules",
                "source_type", "live", "start_date", "end_date"};
            JsonObject *source = at(sources, j);
            valid = keys(source, source_fields, G_N_ELEMENTS(source_fields), 7) && identity(ids, string(source, "id")) && source_live_valid(source);
        }
        if (valid) g_hash_table_insert(definitions, g_ascii_strdown(string(definition, "id"), -1), definition);
    }
    /* Validate unused named library settings too, including an empty draft.
     * This synthetic metadata-only probe is never persisted or prepared. */
    if (valid) {
        JsonObject *probe = json_object_new(), *source = json_object_new(); JsonArray *rotations = json_array_new();
        gchar *event_id = unused_identity(ids), *source_id = unused_identity(ids);
        GDateTime *stamp = g_date_time_new_from_unix_utc(anchor / G_USEC_PER_SEC); gchar *date = date_text(stamp); g_date_time_unref(stamp);
        json_object_set_string_member(probe, "id", event_id); json_object_set_string_member(probe, "title", "Haftalık doğrulama");
        json_object_set_string_member(probe, "date", date); json_object_set_string_member(probe, "color", "#abd6e4");
        json_object_set_int_member(probe, "start_minute", 0); json_object_set_int_member(probe, "end_minute", 1);
        json_object_set_string_member(source, "id", source_id); json_object_set_string_member(source, "playlist_id", event_id); json_object_set_int_member(source, "frequency", 1);
        const gchar *flags[] = {"random", "track_random", "advert_random", "sweeper_random"};
        for (guint i = 0; i < G_N_ELEMENTS(flags); ++i) json_object_set_boolean_member(source, flags[i], FALSE);
        json_array_add_object_element(rotations, source); json_object_set_array_member(probe, "rotations", rotations);
        JsonNode *checked = radios_automation_calendar_resolve_event(calendar, probe, error); valid = checked != NULL;
        if (checked) json_node_free(checked);
        g_hash_table_remove(ids, event_id); g_hash_table_remove(ids, source_id); g_free(event_id); g_free(source_id); g_free(date); json_object_unref(probe);
    }
    for (guint i = 0; valid && i < json_array_get_length(events); ++i) {
        JsonObject *event = at(events, i); const gchar *fields[] = {"id", "title", "date", "start_minute", "end_minute", "color", "rotations", "rotation_selection_mode", "probability_policy", "live"};
        JsonArray *rotations = array(event, "rotations"); GDateTime *date = day(string(event, "date"));
        gint64 begin = number(event, "start_minute", -1), end = number(event, "end_minute", -1), base = date ? g_date_time_to_unix(date) * G_USEC_PER_SEC : 0;
        const gchar *mode = event && json_object_has_member(event, "rotation_selection_mode") ? string(event, "rotation_selection_mode") : "linear";
        const gchar *title = string(event, "title");
        valid = keys(event, fields, G_N_ELEMENTS(fields), 7) && identity(ids, string(event, "id")) && *title && g_utf8_validate(title, -1, NULL) && g_utf8_strlen(title, -1) <= 160 &&
            (!json_object_has_member(event, "live") || live_valid(object(event, "live"))) &&
            date && base >= anchor && base < anchor + period && begin >= 0 && begin <= 1439 && end > begin && end <= 1440 &&
            g_regex_match_simple("^#[0-9a-fA-F]{6}$", string(event, "color"), 0, 0) && rotations && json_array_get_length(rotations) <= 50 &&
            (g_str_equal(mode, "linear") || g_str_equal(mode, "weighted"));
        if (date) g_date_time_unref(date);
        guint expanded = 0; gboolean playable = FALSE;
        for (guint j = 0; valid && j < json_array_get_length(rotations); ++j) {
            JsonObject *rotation = at(rotations, j); valid = rotation && identity(ids, string(rotation, "id"));
            if (valid && json_object_has_member(rotation, "rotation_id")) {
                const gchar *assignment_fields[] = {"id", "rotation_id", "weight"};
                gchar *key = g_ascii_strdown(string(rotation, "rotation_id"), -1); JsonObject *definition = g_hash_table_lookup(definitions, key); g_free(key);
                gint64 weight = json_object_has_member(rotation, "weight") ? number(rotation, "weight", -1) : 100;
                valid = keys(rotation, assignment_fields, G_N_ELEMENTS(assignment_fields), 2) && definition && weight >= 0 && weight <= 100;
                if (valid) expanded += json_array_get_length(array(definition, "sources"));
            } else if (valid) {
                const gchar *legacy_fields[] = {"id", "playlist_id", "frequency", "random", "track_random", "advert_random", "sweeper_random", "media_kind", "autodj"};
                valid = keys(rotation, legacy_fields, G_N_ELEMENTS(legacy_fields), 7);
                if (valid) expanded++;
            }
        }
        valid = valid && expanded <= 50;
        if (valid && expanded) {
            JsonNode *checked = radios_automation_calendar_resolve_event(calendar, event, error); valid = checked != NULL;
            if (checked) {
                JsonArray *sources = array(json_node_get_object(checked), "rotations");
                for (guint j = 0; sources && j < json_array_get_length(sources); ++j) {
                    JsonObject *source = at(sources, j);
                    playable |= number(source, "weight", 100) > 0 && number(source, "calendar_group_weight", 100) > 0;
                }
                json_node_free(checked);
            }
        }
        if (valid) { WeeklyWindow window = {i, zoned_event_time(calendar, event, begin, 0), zoned_event_time(calendar, event, end, 0), playable};
            valid = window.start != G_MININT64 && window.end > window.start;
            if (valid) g_array_append_val(windows, window); }
    }
    g_hash_table_unref(ids); g_hash_table_unref(definitions);
    if (valid) {
        g_array_sort(windows, window_compare);
        for (guint i = 1; valid && i < windows->len; ++i) valid = g_array_index(windows, WeeklyWindow, i).start >= g_array_index(windows, WeeklyWindow, i - 1).end;
    }
    if (!valid && (!error || !*error)) fail(error, "Haftalık takvim kimliği, ataması, UTC aralığı veya çakışması geçersiz.");
    return valid;
}
static gboolean configuration(JsonObject *config, gint64 *anchor_us, gint64 *period_us, GArray *windows, GError **error)
{
    const gchar *fields[] = {"schema", "week_anchor", "week_count", "repeat", "calendar"};
    GDateTime *anchor = day(string(config, "week_anchor")); gint64 count = number(config, "week_count", 0);
    JsonNode *repeat = config ? json_object_get_member(config, "repeat") : NULL;
    gboolean valid = keys(config, fields, G_N_ELEMENTS(fields), 5) && number(config, "schema", 0) == 1 && anchor && g_date_time_get_day_of_week(anchor) == 1 &&
        count >= 1 && count <= 52 && repeat && JSON_NODE_HOLDS_VALUE(repeat) && json_node_get_value_type(repeat) == G_TYPE_BOOLEAN && object(config, "calendar");
    GDateTime *end = valid ? g_date_time_add_days(anchor, (gint)(count * 7)) : NULL; valid = valid && end && g_date_time_get_year(end) <= 9999;
    if (valid) { *anchor_us = g_date_time_to_unix(anchor) * G_USEC_PER_SEC; *period_us = count * 7 * DAY_US; }
    if (anchor) g_date_time_unref(anchor);
    if (end) g_date_time_unref(end);
    if (!valid) return fail(error, "Haftalık programın Pazartesi tarihi, hafta sayısı veya tekrar alanı geçersiz.");
    return calendar_windows(object(config, "calendar"), *anchor_us, *period_us, windows, error);
}
JsonNode *radios_weekly_programme_snapshot(JsonObject *calendar, const gchar *visible_week, gboolean repeat, GError **error)
{
    GDateTime *visible = day(visible_week); JsonArray *events = array(calendar, "events");
    if (!visible || g_date_time_get_day_of_week(visible) != 1 || !events || json_array_get_length(events) > 500) {
        if (visible) g_date_time_unref(visible);
        fail(error, "Programı kaydetmek için geçerli Pazartesi haftası ve tam takvim gerekli."); return NULL;
    }
    gint64 anchor = g_date_time_to_unix(visible) * G_USEC_PER_SEC, last = anchor; g_date_time_unref(visible);
    for (guint i = 0; i < json_array_get_length(events); ++i) {
        GDateTime *date = day(string(at(events, i), "date"));
        if (!date) { fail(error, "Takvim programının tarihi geçersiz; hiçbir program düşürülmedi."); return NULL; }
        gint64 stamp = g_date_time_to_unix(date) * G_USEC_PER_SEC;
        anchor = MIN(anchor, stamp - (g_date_time_get_day_of_week(date) - 1) * DAY_US); last = MAX(last, stamp); g_date_time_unref(date);
    }
    guint64 weeks = (guint64)(last - anchor) / (7 * DAY_US) + 1;
    if (weeks > 52) { fail(error, "Tam takvim 52 haftayı aşıyor; hiçbir program düşürülmedi."); return NULL; }
    JsonObject *config = json_object_new(); json_object_set_int_member(config, "schema", 1);
    GDateTime *date = g_date_time_new_from_unix_utc(anchor / G_USEC_PER_SEC); gchar *formatted = date_text(date); g_date_time_unref(date);
    json_object_set_string_member(config, "week_anchor", formatted); g_free(formatted); json_object_set_int_member(config, "week_count", (gint64)weeks); json_object_set_boolean_member(config, "repeat", repeat);
    JsonNode *copy = clone(calendar); if (!copy) { json_object_unref(config); fail(error, "Tam takvim kopyalanamadı."); return NULL; }
    json_object_set_member(config, "calendar", copy); GArray *windows = g_array_new(FALSE, FALSE, sizeof(WeeklyWindow)); gint64 checked_anchor = 0, period = 0;
    gboolean valid = configuration(config, &checked_anchor, &period, windows, error); g_array_unref(windows);
    JsonNode *result = NULL; if (valid) { result = json_node_new(JSON_NODE_OBJECT); json_node_take_object(result, config); } else json_object_unref(config);
    return result;
}
static RadioSWeeklyProgrammeOccurrence *programme_next(JsonObject *config, gint64 now_utc_us,
    gboolean future_only, gint64 minimum_start_utc_us, GError **error)
{
    GArray *windows = g_array_new(FALSE, FALSE, sizeof(WeeklyWindow)); gint64 anchor = 0, period = 0;
    if (!configuration(config, &anchor, &period, windows, error) || now_utc_us < FIRST_US || now_utc_us >= FINAL_US) {
        if (!error || !*error) fail(error, "Gerçek UTC zaman desteklenen tarih aralığı dışında.");
        g_array_unref(windows); return NULL;
    }
    gboolean repeat = json_object_get_boolean_member(config, "repeat"), active = FALSE; gint selected = -1;
    JsonObject *source_calendar = object(config, "calendar");
    gint64 civil = civil_now(source_calendar, MAX(now_utc_us, minimum_start_utc_us));
    if (civil == G_MININT64) { g_array_unref(windows); fail(error, "İstasyon zamanı desteklenen tarih aralığı dışında."); return NULL; }
    guint64 cycle = repeat && civil >= anchor ? (guint64)(civil - anchor) / (guint64)period : 0;
    gint64 shift = (gint64)cycle * period, start = 0, end = 0, next_start = 0;
    for (guint i = 0; i < windows->len; ++i) {
        WeeklyWindow window = g_array_index(windows, WeeklyWindow, i); if (!window.playable) continue;
        gint64 a = 0, b = 0; occurrence_times(source_calendar, window, shift, &a, &b);
        if (a == G_MININT64 || b <= a) { g_array_unref(windows); fail(error, "Tekrar edilen programın saat aralığı geçersiz."); return NULL; }
        if (a < minimum_start_utc_us) continue;
        if (a > now_utc_us && (!next_start || a < next_start)) next_start = a;
        if (!future_only && a <= now_utc_us && now_utc_us < b) { selected = (gint)i; active = TRUE; start = a; end = b; }
        else if (!active && a > now_utc_us && (selected < 0 || a < start)) { selected = (gint)i; start = a; end = b; }
    }
    if (repeat && !next_start) {
        for (guint i = 0; i < windows->len; ++i) {
            WeeklyWindow window = g_array_index(windows, WeeklyWindow, i); if (!window.playable) continue;
            gint64 following_end = 0, following_start = 0; occurrence_times(source_calendar, window, shift + period, &following_start, &following_end);
            if (following_start < minimum_start_utc_us) continue;
            next_start = following_start;
            if (selected < 0) { selected = (gint)i; ++cycle; shift += period; start = next_start; end = following_end; }
            break;
        }
    }
    if (selected < 0) { g_array_unref(windows); return NULL; }
    JsonNode *calendar = clone(object(config, "calendar")); JsonArray *events = calendar ? array(json_node_get_object(calendar), "events") : NULL; gboolean valid = calendar != NULL;
    for (guint i = 0; valid && i < json_array_get_length(events); ++i) {
        JsonObject *event = at(events, i); GDateTime *source = day(string(event, "date")), *date = source ? g_date_time_add_days(source, (gint)(shift / DAY_US)) : NULL;
        valid = date && g_date_time_get_year(date) <= 9999;
        if (valid) { gchar *formatted = date_text(date); json_object_set_string_member(event, "date", formatted); g_free(formatted); }
        if (source) g_date_time_unref(source);
        if (date) g_date_time_unref(date);
    }
    if (!valid || start < FIRST_US || end > FINAL_US || next_start >= FINAL_US) {
        if (calendar) json_node_free(calendar);
        g_array_unref(windows); fail(error, "Tekrar edilen tam program desteklenen UTC tarih aralığını aşıyor."); return NULL;
    }
    WeeklyWindow chosen = g_array_index(windows, WeeklyWindow, (guint)selected); JsonObject *event = at(events, chosen.index);
    JsonNode *resolved = radios_automation_calendar_resolve_event(json_node_get_object(calendar), event, error); g_array_unref(windows);
    if (!resolved) { json_node_free(calendar); return NULL; }
    RadioSWeeklyProgrammeOccurrence *result = g_new0(RadioSWeeklyProgrammeOccurrence, 1); result->calendar = calendar; result->event = resolved;
    result->original_event_id = g_strdup(string(event, "id")); result->occurrence_key = g_strdup_printf("%s-%" G_GINT64_FORMAT, result->original_event_id, start);
    result->cycle_index = cycle; result->start_utc_us = start; result->end_utc_us = end; result->effective_utc_us = MAX(now_utc_us, start); result->next_start_utc_us = next_start; result->active = active; return result;
}
RadioSWeeklyProgrammeOccurrence *radios_weekly_programme_next(JsonObject *config, gint64 now_utc_us, GError **error)
{ return programme_next(config, now_utc_us, FALSE, G_MININT64, error); }
RadioSWeeklyProgrammeOccurrence *radios_weekly_programme_override_next(JsonObject *main_config,
    JsonObject *override_config, gint64 now_utc_us, gboolean *use_override, gint64 *preempt_utc_us, GError **error)
{
    if (use_override) *use_override = FALSE;
    if (preempt_utc_us) *preempt_utc_us = 0;
    GError *failure = NULL;
    RadioSWeeklyProgrammeOccurrence *main = main_config ? radios_weekly_programme_next(main_config, now_utc_us, &failure) : NULL;
    if (failure) { g_propagate_error(error, failure); return NULL; }
    RadioSWeeklyProgrammeOccurrence *over = override_config ? radios_weekly_programme_next(override_config, now_utc_us, &failure) : NULL;
    if (failure) { radios_weekly_programme_occurrence_free(main); g_propagate_error(error, failure); return NULL; }
    /* The temporary schema wins only inside its own defined slots: while one
     * is active, or when its next slot begins before the main's next slot. */
    if (over && (over->active || !main || over->start_utc_us < main->start_utc_us)) {
        radios_weekly_programme_occurrence_free(main);
        if (use_override) *use_override = TRUE;
        return over;
    }
    if (main && over && preempt_utc_us && over->start_utc_us < main->end_utc_us) *preempt_utc_us = over->start_utc_us;
    radios_weekly_programme_occurrence_free(over);
    return main;
}
RadioSWeeklyProgrammeOccurrence *radios_weekly_programme_next_future(JsonObject *config, gint64 now_utc_us, GError **error)
{ return programme_next(config, now_utc_us, TRUE, G_MININT64, error); }

static gchar *calendar_anchor(JsonObject *calendar, gint64 now_utc_us, GError **error)
{
    JsonArray *events = array(calendar, "events"); GDateTime *first = NULL;
    if (!events) { fail(error, "Ana takvimin program listesi geçersiz."); return NULL; }
    for (guint i = 0; i < json_array_get_length(events); ++i) {
        GDateTime *date = day(string(at(events, i), "date"));
        if (!date) { if (first) g_date_time_unref(first); fail(error, "Ana takvim tarihi geçersiz."); return NULL; }
        if (!first || g_date_time_compare(date, first) < 0) { if (first) g_date_time_unref(first); first = date; }
        else g_date_time_unref(date);
    }
    if (!first) {
        GTimeZone *zone = radios_time_zone_new(json_object_has_member(calendar, "timezone") ? string(calendar, "timezone") : "UTC");
        GDateTime *utc = g_date_time_new_from_unix_utc(now_utc_us / G_USEC_PER_SEC);
        first = zone && utc ? g_date_time_to_timezone(utc, zone) : NULL;
        if (utc) g_date_time_unref(utc);
        if (zone) g_time_zone_unref(zone);
    }
    GDateTime *monday = first ? g_date_time_add_days(first, 1 - g_date_time_get_day_of_week(first)) : NULL;
    if (first) g_date_time_unref(first);
    gchar *result = monday ? date_text(monday) : NULL;
    if (monday) g_date_time_unref(monday);
    if (!result) fail(error, "Ana takvimin yerel haftası geçersiz.");
    return result;
}
static JsonNode *main_configuration(JsonObject *calendar, gint64 now_utc_us,
    gboolean future_only, gint64 minimum_start_utc_us, GError **error)
{
    /* Main calendars may retain many years of archived weeks. Validate and
     * compare their original scalar dates; the bounded weekly runtime receives
     * just the chosen slot. The full server record remains the auth baseline. */
    GArray *windows = g_array_new(FALSE, FALSE, sizeof(WeeklyWindow));
    if (!calendar_windows(calendar, FIRST_US, FINAL_US - FIRST_US, windows, error)) {
        g_array_unref(windows); return NULL;
    }
    gint chosen = -1; gboolean active = FALSE;
    for (guint i = 0; i < windows->len; ++i) {
        WeeklyWindow window = g_array_index(windows, WeeklyWindow, i);
        if (!window.playable || window.start < minimum_start_utc_us) continue;
        if (!future_only && window.start <= now_utc_us && now_utc_us < window.end) {
            chosen = (gint)window.index; active = TRUE;
        } else if (!active && chosen < 0 && window.start > now_utc_us) chosen = (gint)window.index;
    }
    g_array_unref(windows);
    if (chosen < 0) return NULL;
    JsonNode *candidate = clone(calendar); JsonArray *events = json_array_new();
    json_array_add_element(events, clone(at(array(calendar, "events"), (guint)chosen)));
    json_object_set_array_member(json_node_get_object(candidate), "events", events);
    gchar *anchor = calendar_anchor(json_node_get_object(candidate), now_utc_us, error);
    JsonNode *result = anchor ? radios_weekly_programme_snapshot(json_node_get_object(candidate), anchor, FALSE, error) : NULL;
    g_free(anchor); json_node_free(candidate); return result;
}
static RadioSWeeklyProgrammeSelection *select_records(JsonArray *records, gint64 now_utc_us,
    gboolean future_only, gint64 minimum_start_utc_us, GError **error)
{
    if (now_utc_us < FIRST_US || now_utc_us >= FINAL_US) {
        fail(error, "Gerçek UTC zaman desteklenen tarih aralığı dışında."); return NULL;
    }
    RadioSWeeklyProgrammeSelection *best = NULL; const gchar *station = NULL; gboolean saw_main = FALSE;
    for (guint i = 0; records && i < json_array_get_length(records); ++i) {
        JsonObject *record = at(records, i); const gchar *kind = string(record, "kind");
        gboolean main = g_str_equal(kind, "automation_calendar") && g_str_equal(string(record, "name"), "main");
        if (!main && !g_str_equal(kind, "weekly_programme")) continue;
        if (g_str_equal(string(record, "status"), "disabled")) continue;
        JsonObject *config = object(record, "config"); const gchar *scope = string(record, "station_id");
        if (!g_str_equal(string(record, "status"), "active") || !g_uuid_string_is_valid(string(record, "_id")) ||
            number(record, "_rev", 0) < 1 || !g_uuid_string_is_valid(scope) || !config ||
            (station && g_ascii_strcasecmp(station, scope)) || (main && saw_main)) {
            radios_weekly_programme_selection_free(best);
            fail(error, "Genel sıra kaynak kimliği, istasyonu veya tek ana takvim koşulu geçersiz."); return NULL;
        }
        station = scope; saw_main |= main;
        JsonNode *normalized = NULL; GError *invalid = NULL;
        if (main) {
            normalized = main_configuration(config, now_utc_us, future_only, minimum_start_utc_us, &invalid);
        } else normalized = clone(config);
        if (invalid) {
            radios_weekly_programme_selection_free(best); g_propagate_error(error, invalid); return NULL;
        }
        if (!normalized) continue;
        RadioSWeeklyProgrammeOccurrence *occurrence = programme_next(json_node_get_object(normalized), now_utc_us,
            future_only, minimum_start_utc_us, &invalid);
        if (invalid) {
            json_node_free(normalized); radios_weekly_programme_selection_free(best);
            g_propagate_error(error, invalid); return NULL;
        }
        if (!occurrence) { json_node_free(normalized); continue; }
        JsonObject *previous = best ? json_node_get_object(best->record) : NULL;
        gboolean previous_main = previous && g_str_equal(string(previous, "kind"), "automation_calendar");
        gboolean better = !best || occurrence->start_utc_us < best->occurrence->start_utc_us ||
            (occurrence->start_utc_us == best->occurrence->start_utc_us &&
             ((main && !previous_main) || (main == previous_main && g_ascii_strcasecmp(string(record, "_id"), string(previous, "_id")) < 0)));
        if (!better) { json_node_free(normalized); radios_weekly_programme_occurrence_free(occurrence); continue; }
        radios_weekly_programme_selection_free(best); best = g_new0(RadioSWeeklyProgrammeSelection, 1);
        best->record = clone(record); best->config = normalized; best->occurrence = occurrence;
        best->source_key = g_strdup_printf("%s:%s:%" G_GINT64_FORMAT, kind, string(record, "_id"), number(record, "_rev", 0));
    }
    return best;
}
RadioSWeeklyProgrammeSelection *radios_weekly_programme_select_next(JsonArray *records, gint64 now_utc_us, GError **error)
{ return select_records(records, now_utc_us, TRUE, G_MININT64, error); }
RadioSWeeklyProgrammeSelection *radios_weekly_programme_select_running(JsonArray *records, gint64 now_utc_us,
    gint64 minimum_start_utc_us, GError **error)
{
    if (minimum_start_utc_us < FIRST_US || minimum_start_utc_us >= FINAL_US) {
        fail(error, "Genel sıranın başlangıç sınırı geçersiz."); return NULL;
    }
    return select_records(records, now_utc_us, FALSE, minimum_start_utc_us, error);
}
void radios_weekly_programme_selection_free(RadioSWeeklyProgrammeSelection *selection)
{
    if (!selection) return;
    g_clear_pointer(&selection->record, json_node_free); g_clear_pointer(&selection->config, json_node_free);
    radios_weekly_programme_occurrence_free(selection->occurrence); g_free(selection->source_key); g_free(selection);
}
void radios_weekly_programme_occurrence_free(RadioSWeeklyProgrammeOccurrence *o)
{
    if (!o) return;
    g_clear_pointer(&o->calendar, json_node_free); g_clear_pointer(&o->event, json_node_free); g_free(o->original_event_id); g_free(o->occurrence_key); g_free(o);
}

/* The broadcast order is a projection of the verified configuration and the
 * accepted planner result. Timing/voice choices never become saved edits. */
JsonNode *radios_weekly_programme_queue_projection(JsonObject *configuration, JsonObject *display, JsonObject *programme_live)
{
    if (!configuration) return NULL;
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, radios_programme_clone_object(configuration));
    JsonObject *projection = json_node_get_object(node);
    JsonArray *events = array(projection, "events"), *rows = display ? array(display, "entries") : NULL;
    JsonArray *blocks = array(projection, "blocks");
    if (!blocks) { blocks = json_array_new(); json_object_set_array_member(projection, "blocks", blocks); }
    for (guint i = 0; events && i < json_array_get_length(events); ++i) {
        JsonObject *event = at(events, i), *row = NULL;
        if (!event) continue;
        json_object_remove_member(event, "planned_jingles");
        json_object_remove_member(event, "planned_jingle");
        for (guint j = 0; rows && j < json_array_get_length(rows); ++j) {
            JsonObject *candidate = at(rows, j);
            if (*string(event, "id") && g_str_equal(string(event, "id"), string(candidate, "entry_id"))) { row = candidate; break; }
        }
        if (!row) continue;
        /* Kılavuz §04/§06: live and inner-order hints ride along unchanged. */
        const gchar *fields[] = {"block_id", "block_kind", "block_title", "block_explicit", "planned_jingles", "planned_jingle", "live", "live_step", "programme_entry"};
        for (guint j = 0; j < G_N_ELEMENTS(fields); ++j) {
            JsonNode *member = json_object_get_member(row, fields[j]);
            if (member) json_object_set_member(event, fields[j], radios_programme_clone(member));
        }
        /* An accepted run names the player of each content (1..32). A preview
         * row only has a guess from the default pool, so the flow keeps saying
         * that automation chooses until the run really starts. */
        gint64 player = number(row, "player", 0);
        if (player >= 1 && player <= 32) json_object_set_int_member(event, "player", player);
        const gchar *block_id = string(row, "block_id");
        if (!*block_id) continue;
        JsonObject *block = NULL;
        for (guint j = 0; j < json_array_get_length(blocks); ++j)
            if (g_str_equal(string(at(blocks, j), "id"), block_id)) { block = at(blocks, j); break; }
        if (!block) {
            block = json_object_new(); json_object_set_string_member(block, "id", block_id);
            json_object_set_string_member(block, "start_time", ""); json_object_set_string_member(block, "note", "");
            json_array_add_object_element(blocks, block);
        }
        if (*string(row, "block_kind")) json_object_set_string_member(block, "kind", string(row, "block_kind"));
        if (*string(row, "block_title")) json_object_set_string_member(block, "title", string(row, "block_title"));
        if (object(row, "live") && !object(block, "live")) json_object_set_object_member(block, "live", radios_programme_clone_object(object(row, "live")));
    }
    if (programme_live) {
        /* Kılavuz §04 adım 1 (contract §1b): the live programme's own settings
         * are the default of every live block; a plan without live steps
         * still carries label/channels so the event row can drive L/R. */
        gboolean any_live = FALSE;
        for (guint j = 0; j < json_array_get_length(blocks); ++j) {
            JsonObject *block = at(blocks, j);
            if (!block || !g_str_equal(string(block, "kind"), "live")) continue;
            any_live = TRUE;
            if (!object(block, "live")) json_object_set_object_member(block, "live", radios_programme_clone_object(programme_live));
        }
        for (guint j = 0; !any_live && j < json_array_get_length(blocks); ++j) {
            JsonObject *block = at(blocks, j);
            if (!block || object(block, "live")) continue;
            JsonObject *fallback = json_object_new(); const gchar *copied[] = {"label", "channels"};
            for (guint k = 0; k < G_N_ELEMENTS(copied); ++k)
                if (json_object_has_member(programme_live, copied[k])) json_object_set_member(fallback, copied[k], radios_programme_clone(json_object_get_member(programme_live, copied[k])));
            json_object_set_object_member(block, "live", fallback);
        }
    }
    return node;
}

static JsonObject *library_definition(JsonArray *library, const gchar *id)
{
    if (!library || !id || !*id) return NULL;
    for (guint i = 0; i < json_array_get_length(library); ++i) {
        JsonObject *definition = at(library, i);
        if (definition && !g_ascii_strcasecmp(string(definition, "id"), id)) return definition;
    }
    return NULL;
}
static gboolean definition_schema(JsonObject *definition)
{
    JsonObject *settings = object(definition, "settings");
    JsonNode *flag = settings ? json_object_get_member(settings, "automation_schema") : NULL;
    return flag && JSON_NODE_HOLDS_VALUE(flag) && json_node_get_value_type(flag) == G_TYPE_BOOLEAN && json_node_get_boolean(flag);
}
JsonObject *radios_weekly_programme_schema_definition(JsonObject *calendar, JsonObject *event, const gchar *schema_id)
{
    JsonArray *library = array(calendar, "rotation_library");
    JsonObject *definition = library_definition(library, schema_id);
    if (definition) return definition;
    definition = library_definition(library, string(object(event, "live"), "automation_id"));
    if (definition) return definition;
    JsonArray *rows = array(event, "rotations"); JsonObject *fallback = NULL;
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        JsonObject *row = at(rows, i);
        const gchar *reference = row && json_object_has_member(row, "rotation_id") ? string(row, "rotation_id") : string(row, "calendar_rotation_id");
        JsonObject *candidate = library_definition(library, reference);
        if (!candidate) continue;
        if (definition_schema(candidate)) return candidate;
        if (!fallback && object(object(candidate, "settings"), "programme_entry")) fallback = candidate;
    }
    return fallback;
}
guint radios_weekly_programme_entry_order(JsonObject *calendar, JsonObject *event, const gchar *schema_id)
{
    JsonObject *definition = radios_weekly_programme_schema_definition(calendar, event, schema_id);
    gint64 order = number(object(object(definition, "settings"), "programme_entry"), "source_order", 0);
    return order >= 1 && order <= 50 ? (guint)order : 0;
}
