#include "radios/local_event_prepare.h"
#include "radios/automation_calendar_plan.h"
#include "radios/data.h"
#include "radios/programme_model.h"
#include "radios/weekly_programme_plan.h"
#include "radios/platform.h"
#include <string.h>

#define LOCAL_EVENT_MAX_SOURCES 100
#define LOCAL_EVENT_MAX_ROWS 5000

typedef struct {
    RadioSViewContext *ctx; /* accessed on the main context only */
    RadioSDataClient *client;
    GWeakRef owner;
    gulong owner_destroy;
    JsonObject *configuration;
    RadioSLocalEventPrepared callback;
    gpointer user;
    GDestroyNotify destroy;
    gint64 now;
    gchar *schema_id;   /* schema preparation only */
    gint64 duration_ms; /* schema preparation only; <= 0 means one hour */
} LocalPreparation;

static JsonObject *object(JsonObject *parent, const gchar *key)
{
    JsonNode *node = parent ? json_object_get_member(parent, key) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonArray *array(JsonObject *parent, const gchar *key)
{
    JsonNode *node = parent ? json_object_get_member(parent, key) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static JsonObject *at(JsonArray *items, guint index)
{
    JsonNode *node = items && index < json_array_get_length(items) ? json_array_get_element(items, index) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static const gchar *text(JsonObject *parent, const gchar *key)
{ return radios_json_string(parent, key, ""); }
static gboolean flag(JsonObject *parent, const gchar *key)
{
    JsonNode *node = parent ? json_object_get_member(parent, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN && json_node_get_boolean(node);
}
static gboolean integer_is(JsonObject *parent, const gchar *key, gint64 expected)
{
    JsonNode *node = parent ? json_object_get_member(parent, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64 && json_node_get_int(node) == expected;
}
static gboolean fail(GError **error, const gchar *message)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message); return FALSE; }
static gboolean uuid(const gchar *value)
{
    if (!g_uuid_string_is_valid(value)) return FALSE;
    for (const gchar *p = value; *p; ++p) if (g_ascii_isupper(*p)) return FALSE;
    return TRUE;
}
static gboolean source_kind(const gchar *kind)
{ return !strcmp(kind, "playlist") || !strcmp(kind, "automation_calendar") || !strcmp(kind, "weekly_programme"); }
static gboolean local_valid(JsonObject *config, GError **error)
{
    JsonArray *sources = array(config, "sources"); const gchar *title = text(config, "title");
    gchar *trimmed = g_strdup(title); g_strstrip(trimmed);
    JsonObject *playback = object(config, "playback");
    gboolean has_playback = config && json_object_has_member(config, "playback");
    JsonNode *player = playback ? json_object_get_member(playback, "player") : NULL;
    gboolean playback_valid = !has_playback || (playback && json_object_get_size(playback) == 2 &&
        player && JSON_NODE_HOLDS_VALUE(player) && json_node_get_value_type(player) == G_TYPE_INT64 &&
        json_node_get_int(player) >= 0 && json_node_get_int(player) <= 32 &&
        json_object_has_member(playback, "next_cluster_id") &&
        JSON_NODE_HOLDS_VALUE(json_object_get_member(playback, "next_cluster_id")) &&
        json_node_get_value_type(json_object_get_member(playback, "next_cluster_id")) == G_TYPE_STRING &&
        (!*text(playback, "next_cluster_id") || uuid(text(playback, "next_cluster_id"))));
    gboolean valid = config && json_object_get_size(config) == (has_playback ? 5 : 4) &&
        playback_valid && integer_is(config, "schema", 1) &&
        *title && g_utf8_validate(title, -1, NULL) && g_utf8_strlen(title, -1) <= 160 && !strcmp(title, trimmed) &&
        radios_programme_kind_valid(text(config, "kind")) && sources &&
        json_array_get_length(sources) > 0 && json_array_get_length(sources) <= LOCAL_EVENT_MAX_SOURCES;
    g_free(trimmed);
    GHashTable *ids = g_hash_table_new(g_str_hash, g_str_equal);
    for (guint i = 0; valid && i < json_array_get_length(sources); ++i) {
        JsonObject *source = at(sources, i); const gchar *id = text(source, "id");
        valid = source && json_object_get_size(source) == 3 && uuid(id) && !g_hash_table_contains(ids, id) &&
            source_kind(text(source, "type")) && uuid(text(source, "source_id"));
        if (valid) g_hash_table_add(ids, (gpointer)id);
    }
    g_hash_table_unref(ids);
    return valid || fail(error, "Yerel Event başlığı, türü veya kaynakları geçersiz; en az bir etkin kaynak seçin.");
}
static JsonNode *request(LocalPreparation *job, GCancellable *cancel, const gchar *path,
    const gchar *kind, const gchar *id_key, const gchar *id, JsonNode *config, GError **error)
{
    if (g_cancellable_set_error_if_cancelled(cancel, error)) return NULL;
    JsonObject *body = json_object_new(); JsonNode *node = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(node, body);
    json_object_set_string_member(body, "station_id", radios_data_client_station_id(job->client));
    json_object_set_string_member(body, "kind", kind);
    if (id_key) json_object_set_string_member(body, id_key, id);
    if (config) json_object_set_member(body, "config", radios_programme_clone(config));
    JsonNode *result = radios_data_client_request_cancellable_sync(job->client, "POST", path, node, NULL, cancel, error);
    json_node_free(node);
    if (result && !JSON_NODE_HOLDS_OBJECT(result)) {
        json_node_free(result); fail(error, "Kaynak yanıtı geçersiz."); return NULL;
    }
    return result;
}
static gboolean response_valid(LocalPreparation *job, JsonObject *result, const gchar *kind, GError **error)
{
    return (result && integer_is(result, "contract_version", 1) &&
        !strcmp(text(result, "kind"), kind) && !strcmp(text(result, "timezone"), "UTC") &&
        !strcmp(text(result, "station_id"), radios_data_client_station_id(job->client))) ||
        fail(error, "Kaynak hazırlama sözleşmesi veya istasyon kimliği değişti.");
}
static gboolean row_valid(LocalPreparation *job, JsonObject *row, GError **error)
{
    JsonObject *detail = object(row, "detail"), *media = object(detail, "media"), *version = object(detail, "version");
    JsonObject *metadata = object(row, "metadata"); const gchar *sha = text(version, "sha256");
    gboolean valid = row && flag(row, "eligible") && uuid(text(row, "entry_id")) && media && version && metadata &&
        uuid(text(media, "_id")) && uuid(text(version, "_id")) && uuid(text(media, "media_file_id")) && uuid(text(media, "item_id")) &&
        !strcmp(text(media, "station_id"), radios_data_client_station_id(job->client)) &&
        !strcmp(text(media, "version_id"), text(version, "_id")) &&
        !strcmp(text(media, "media_file_id"), text(version, "media_file_id")) &&
        !strcmp(text(media, "item_id"), text(version, "item_id")) &&
        flag(version, "verified") && flag(version, "object_available") &&
        radios_json_int(version, "duration_ms", 0) > 0 && radios_json_int(version, "duration_ms", 0) <= 86400000 &&
        radios_programme_kind_valid(text(media, "kind")) && !strcmp(text(media, "kind"), text(metadata, "kind")) && strlen(sha) == 64;
    for (guint i = 0; valid && i < 64; ++i) valid = g_ascii_isxdigit(sha[i]);
    return valid || fail(error, "Kaynakta kullanılamayan veya doğrulanmamış bir parça var. Event bütünüyle eklenmedi.");
}
static gchar *bounded(const gchar *value, guint limit)
{
    if (!g_utf8_validate(value, -1, NULL)) return g_strdup("");
    return g_utf8_substring(value, 0, MIN(g_utf8_strlen(value, -1), (glong)limit));
}
static JsonObject *playlist_entry(JsonObject *row)
{
    JsonObject *detail = object(row, "detail"), *media = object(detail, "media"), *version = object(detail, "version");
    JsonObject *entry = json_object_new(); const gchar *fields[] = {"title", "artist", "album", "version_label"};
    const guint limits[] = {160, 120, 120, 80};
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) {
        const gchar *value = i == 3 ? text(version, "label") : text(media, fields[i]);
        if (!*value && i < 3) value = text(object(row, "metadata"), fields[i]);
        gchar *copy = bounded(value, limits[i]);
        json_object_set_string_member(entry, fields[i], i == 0 && !*copy ? "Parça" : copy); g_free(copy);
    }
    json_object_set_string_member(entry, "kind", text(media, "kind"));
    json_object_set_int_member(entry, "duration_ms", radios_json_int(version, "duration_ms", 0));
    json_object_set_string_member(entry, "primary_file", ""); json_object_set_string_member(entry, "backup_file", "");
    json_object_set_string_member(entry, "note", "Yerel Event playlist kopyası.");
    json_object_set_string_member(entry, "media_id", text(media, "_id"));
    json_object_set_string_member(entry, "version_id", text(version, "_id"));
    json_object_set_string_member(entry, "media_file_id", text(media, "media_file_id"));
    json_object_set_string_member(entry, "media_sha256", text(version, "sha256"));
    const gchar *policies[] = {"probability_policy", "policy_context"};
    for (guint i = 0; i < G_N_ELEMENTS(policies); ++i) {
        JsonNode *policy = json_object_get_member(row, policies[i]);
        if (policy) json_object_set_member(entry, policies[i], radios_programme_clone(policy));
    }
    return entry;
}
static gboolean append_entry(JsonArray *events, JsonObject *entry, const gchar *block_id, GError **error)
{
    if (json_array_get_length(events) >= LOCAL_EVENT_MAX_ROWS) {
        json_object_unref(entry); return fail(error, "Yerel Event 5000 parça sınırını aşıyor.");
    }
    gchar *id = g_uuid_string_random(); json_object_set_string_member(entry, "id", id); g_free(id);
    json_object_set_string_member(entry, "block_id", block_id); json_object_set_string_member(entry, "start_time", "");
    json_object_remove_member(entry, "start_policy"); json_object_remove_member(entry, "max_wait_ms");
    json_array_add_object_element(events, entry); return TRUE;
}
static gboolean append_playlist(LocalPreparation *job, GCancellable *cancel, JsonObject *source,
    JsonArray *events, const gchar *block_id, GError **error)
{
    JsonNode *prepared = request(job, cancel, "/v1/workflows/desktop-playout/prepare", "playlist",
        "playlist_id", text(source, "source_id"), NULL, error);
    if (!prepared) return FALSE;
    JsonObject *response = json_node_get_object(prepared); JsonArray *tracks = array(response, "tracks"), *diagnostics = array(response, "diagnostics");
    gboolean valid = response_valid(job, response, "playlist", error);
    if (valid && (strcmp(text(response, "playlist_id"), text(source, "source_id")) || !tracks ||
            !json_array_get_length(tracks) || (diagnostics && json_array_get_length(diagnostics))))
        valid = fail(error, "Playlist boş, kapalı veya hazırlanamadı; yerel Event eklenmedi.");
    for (guint i = 0; valid && i < json_array_get_length(tracks); ++i) {
        JsonObject *row = at(tracks, i);
        valid = !g_cancellable_set_error_if_cancelled(cancel, error) && row_valid(job, row, error);
        if (valid) valid = append_entry(events, playlist_entry(row), block_id, error);
    }
    json_node_free(prepared); return valid;
}
static gboolean record_valid(LocalPreparation *job, JsonObject *record, JsonObject *source, GError **error)
{
    return (record && !strcmp(text(record, "_id"), text(source, "source_id")) &&
        !strcmp(text(record, "kind"), text(source, "type")) && !strcmp(text(record, "status"), "active") &&
        !strcmp(text(record, "station_id"), radios_data_client_station_id(job->client)) &&
        radios_json_int(record, "_rev", 0) > 0 && object(record, "config")) ||
        fail(error, "Otomasyon kaynağı kapalı, kaldırılmış veya başka bir istasyona ait.");
}
static gchar *current_monday(LocalPreparation *job)
{
    GDateTime *now = g_date_time_new_from_unix_utc(job->now / G_USEC_PER_SEC);
    GDateTime *monday = g_date_time_add_days(now, 1 - g_date_time_get_day_of_week(now));
    gchar *date = g_date_time_format(monday, "%Y-%m-%d");
    g_date_time_unref(monday); g_date_time_unref(now); return date;
}
static JsonObject *formal_entry(JsonObject *source)
{
    const gchar *fields[] = {"title", "artist", "album", "kind", "duration_ms", "version_label", "primary_file", "backup_file", "note",
        "media_id", "version_id", "media_file_id", "media_sha256", "probability_policy", "policy_context"};
    JsonObject *entry = json_object_new();
    for (guint i = 0; i < G_N_ELEMENTS(fields); ++i) {
        JsonNode *node = json_object_get_member(source, fields[i]);
        if (node) json_object_set_member(entry, fields[i], radios_programme_clone(node));
    }
    return entry;
}
static gboolean append_schedule(LocalPreparation *job, GCancellable *cancel, JsonObject *source,
    JsonArray *events, const gchar *block_id, GError **error)
{
    JsonNode *record = request(job, cancel, "/v1/workflows/desktop/get", text(source, "type"),
        "document_id", text(source, "source_id"), NULL, error);
    if (!record) return FALSE;
    gboolean valid = record_valid(job, json_node_get_object(record), source, error);
    JsonNode *weekly = NULL, *automation = NULL, *prepared = NULL, *selection = NULL;
    RadioSWeeklyProgrammeOccurrence *occurrence = NULL;
    JsonObject *config = object(json_node_get_object(record), "config");
    if (valid && !strcmp(text(source, "type"), "automation_calendar")) {
        gchar *monday = current_monday(job);
        weekly = radios_weekly_programme_snapshot(config, monday, FALSE, error); g_free(monday);
        valid = weekly != NULL;
    }
    if (valid) {
        occurrence = radios_weekly_programme_next(weekly ? json_node_get_object(weekly) : config, job->now, error);
        if (!occurrence) {
            if (!error || !*error) fail(error, "Bu otomasyon kaynağında kullanılabilir mevcut veya gelecek Event yok.");
            valid = FALSE;
        }
    }
    if (valid) {
        automation = radios_automation_calendar_prepare_config(json_node_get_object(occurrence->event), NULL, error);
        valid = automation != NULL;
    }
    if (valid) {
        prepared = request(job, cancel, "/v1/workflows/desktop-playout/prepare", "automation", NULL, NULL, automation, error);
        valid = prepared && response_valid(job, json_node_get_object(prepared), "automation", error);
    }
    if (valid) {
        selection = radios_automation_calendar_prepared_programme(json_node_get_object(occurrence->event),
            json_node_get_object(prepared), radios_data_client_station_id(job->client), text(source, "id"), 200, error);
        valid = selection != NULL;
    }
    if (valid) {
        JsonArray *leaves = array(object(json_node_get_object(selection), "config"), "events");
        if (!leaves || !json_array_get_length(leaves)) valid = fail(error, "Otomasyon kaynağında seçilebilir parça yok.");
        for (guint i = 0; valid && i < json_array_get_length(leaves); ++i) {
            JsonObject *leaf = at(leaves, i);
            valid = leaf && !g_cancellable_set_error_if_cancelled(cancel, error);
            if (valid) valid = append_entry(events, formal_entry(leaf), block_id, error);
            else if (!error || !*error) fail(error, "Otomasyon parçası geçersiz.");
        }
    }
    if (valid) {
        JsonNode *fresh = request(job, cancel, "/v1/workflows/desktop/get", text(source, "type"),
            "document_id", text(source, "source_id"), NULL, error);
        valid = fresh && record_valid(job, json_node_get_object(fresh), source, error);
        if (valid && radios_json_int(json_node_get_object(fresh), "_rev", 0) != radios_json_int(json_node_get_object(record), "_rev", 0))
            valid = fail(error, "Otomasyon şeması hazırlanırken değişti. Yeniden ekleyin.");
        g_clear_pointer(&fresh, json_node_free);
    }
    g_clear_pointer(&selection, json_node_free); g_clear_pointer(&prepared, json_node_free);
    g_clear_pointer(&automation, json_node_free); g_clear_pointer(&weekly, json_node_free);
    radios_weekly_programme_occurrence_free(occurrence); json_node_free(record); return valid;
}
/* Source recipes are copies of playable media, not a declared mixed
 * programme container. Music from adjacent sources remains one event until
 * a station break, even when the recipe itself was labelled "program". */
static void materialized_blocks(JsonObject *programme, JsonObject *configuration)
{
    JsonArray *events = array(programme, "events"), *blocks = json_array_new();
    const gchar *first_kind = text(at(events, 0), "kind");
    gboolean mixed = FALSE;
    for (guint i = 1; events && i < json_array_get_length(events); ++i)
        mixed |= g_strcmp0(first_kind, text(at(events, i), "kind")) != 0;
    JsonObject *block = NULL;
    for (guint i = 0; events && i < json_array_get_length(events); ++i) {
        JsonObject *entry = at(events, i);
        const gchar *kind = mixed ? text(entry, "kind") : text(configuration, "kind");
        if (!block || g_strcmp0(text(block, "kind"), kind)) {
            block = json_object_new();
            gchar *id = radios_programme_block_id_for(text(entry, "id"));
            json_object_set_string_member(block, "id", id); g_free(id);
            json_object_set_string_member(block, "kind", kind);
            json_object_set_string_member(block, "title", i == 0 || g_str_equal(kind, "music") ?
                text(configuration, "title") : radios_programme_event_title(kind));
            json_object_set_string_member(block, "start_time", "");
            json_object_set_string_member(block, "note", "Yerel Event");
            json_array_add_object_element(blocks, block);
        }
        json_object_set_string_member(entry, "block_id", text(block, "id"));
    }
    json_object_set_array_member(programme, "blocks", blocks);
}
static void prepare_worker(GTask *task, gpointer source, gpointer value, GCancellable *cancel)
{
    (void)source; LocalPreparation *job = value; GError *error = NULL;
    if (!local_valid(job->configuration, &error)) { g_task_return_error(task, error); return; }
    JsonObject *programme = json_object_new(), *block = json_object_new();
    JsonArray *events = json_array_new(), *blocks = json_array_new(); gchar *id = g_uuid_string_random();
    json_object_set_string_member(programme, "title", text(job->configuration, "title"));
    GDateTime *now = g_date_time_new_from_unix_utc(job->now / G_USEC_PER_SEC);
    gchar *date = g_date_time_format(now, "%Y-%m-%d"); g_date_time_unref(now);
    json_object_set_string_member(programme, "date", date); g_free(date);
    json_object_set_string_member(programme, "note", "Yerel Event: kaynakların bağımsız anlık kopyası.");
    json_object_set_array_member(programme, "events", events); json_object_set_array_member(programme, "blocks", blocks);
    json_object_set_string_member(block, "id", id); json_object_set_string_member(block, "title", text(job->configuration, "title"));
    json_object_set_string_member(block, "kind", text(job->configuration, "kind"));
    json_object_set_string_member(block, "start_time", ""); json_object_set_string_member(block, "note", "Yerel Event");
    json_array_add_object_element(blocks, block);
    JsonArray *sources = array(job->configuration, "sources"); gboolean valid = TRUE;
    for (guint i = 0; valid && i < json_array_get_length(sources); ++i) {
        JsonObject *item = at(sources, i);
        valid = !g_cancellable_set_error_if_cancelled(cancel, &error);
        if (valid) valid = !strcmp(text(item, "type"), "playlist") ?
            append_playlist(job, cancel, item, events, id, &error) : append_schedule(job, cancel, item, events, id, &error);
    }
    g_free(id);
    if (valid) {
        materialized_blocks(programme, job->configuration);
        g_task_return_pointer(task, programme, (GDestroyNotify)json_object_unref);
    }
    else {
        json_object_unref(programme);
        if (!error) fail(&error, "Yerel Event hazırlanamadı.");
        g_task_return_error(task, error);
    }
}
/* ---- Kılavuz §06/§07: an event's automation schema, from now on -------
 * L on a live event row starts the automation schema set for that event.
 * The schema definition is looked up where the players' "Otomasyon şeması
 * yükle…" list reads it (main calendar, active saved weekly programmes);
 * a synthetic event in the calendar's own timezone covers [now, now+length)
 * up to local midnight, and the ordinary authenticated chain selects media
 * from now. Nothing is saved; the source revision is fenced. */
static gboolean schema_record(LocalPreparation *job, JsonObject *record)
{
    return record && uuid(text(record, "_id")) && radios_json_int(record, "_rev", 0) > 0 &&
        !strcmp(text(record, "status"), "active") && !strcmp(text(record, "station_id"), radios_data_client_station_id(job->client)) &&
        (!strcmp(text(record, "kind"), "weekly_programme") ||
         (!strcmp(text(record, "kind"), "automation_calendar") && !strcmp(text(record, "name"), "main")));
}
static JsonObject *record_calendar(JsonObject *record)
{
    JsonObject *config = object(record, "config");
    return !strcmp(text(record, "kind"), "weekly_programme") ? object(config, "calendar") : config;
}
static JsonObject *schema_definition(JsonObject *calendar, const gchar *id)
{
    JsonArray *library = array(calendar, "rotation_library");
    for (guint i = 0; library && i < json_array_get_length(library); ++i) {
        JsonObject *definition = at(library, i), *settings = object(definition, "settings");
        if (definition && !strcmp(text(definition, "id"), id) && flag(settings, "automation_schema")) return definition;
    }
    return NULL;
}
/* Owned calendar copy holding only the synthetic "from now" event. */
static JsonObject *schema_window(LocalPreparation *job, JsonObject *calendar, JsonObject *definition, JsonObject **event)
{
    const gchar *zone_name = json_object_has_member(calendar, "timezone") ? text(calendar, "timezone") : "UTC";
    GTimeZone *zone = radios_time_zone_new(*zone_name ? zone_name : "UTC");
    if (!zone) zone = g_time_zone_new_utc();
    GDateTime *utc = g_date_time_new_from_unix_utc(job->now / G_USEC_PER_SEC), *local = g_date_time_to_timezone(utc, zone);
    gint64 start = g_date_time_get_hour(local) * 60 + g_date_time_get_minute(local);
    gint64 minutes = job->duration_ms > 0 ? (job->duration_ms + 59999) / 60000 : 60;
    gint64 end = MIN((gint64)1440, start + MAX((gint64)1, minutes));
    gchar *date = g_date_time_format(local, "%Y-%m-%d"), *id = g_uuid_string_random(), *assignment_id = g_uuid_string_random();
    g_date_time_unref(local); g_date_time_unref(utc); g_time_zone_unref(zone);
    JsonObject *copy = radios_programme_clone_object(calendar), *window = json_object_new(), *assignment = json_object_new();
    JsonArray *events = json_array_new(), *rotations = json_array_new();
    gchar *title = bounded(*text(definition, "name") ? text(definition, "name") : "Live event şeması", 160);
    json_object_set_string_member(window, "id", id); json_object_set_string_member(window, "title", title);
    json_object_set_string_member(window, "date", date); json_object_set_int_member(window, "start_minute", start);
    json_object_set_int_member(window, "end_minute", end); json_object_set_string_member(window, "color", "#abd6e4");
    json_object_set_string_member(assignment, "id", assignment_id); json_object_set_string_member(assignment, "rotation_id", job->schema_id);
    json_array_add_object_element(rotations, assignment); json_object_set_array_member(window, "rotations", rotations);
    json_array_add_object_element(events, window); json_object_set_array_member(copy, "events", events);
    g_free(title); g_free(date); g_free(id); g_free(assignment_id);
    *event = window; return copy;
}
static void schema_worker(GTask *task, gpointer source, gpointer value, GCancellable *cancel)
{
    (void)source; LocalPreparation *job = value; GError *error = NULL;
    const gchar *kinds[] = {"automation_calendar", "weekly_programme"};
    JsonNode *record = NULL; JsonObject *definition = NULL;
    for (guint k = 0; !record && !error && k < G_N_ELEMENTS(kinds); ++k) {
        JsonNode *list = request(job, cancel, "/v1/workflows/desktop/list", kinds[k], NULL, NULL, NULL, &error);
        JsonArray *records = list ? array(json_node_get_object(list), "records") : NULL;
        for (guint i = 0; records && !record && i < json_array_get_length(records); ++i) {
            JsonObject *candidate = at(records, i);
            if (schema_record(job, candidate) && schema_definition(record_calendar(candidate), job->schema_id))
                record = radios_programme_clone(json_array_get_element(records, i));
        }
        g_clear_pointer(&list, json_node_free);
    }
    JsonObject *calendar = record ? record_calendar(json_node_get_object(record)) : NULL;
    if (record) definition = schema_definition(calendar, job->schema_id);
    if (!error && !definition) fail(&error, "Eventin otomasyon şeması bulunamadı ya da artık şema değil.");
    JsonObject *window = NULL, *event = NULL, *programme = NULL;
    JsonNode *resolved = NULL, *automation = NULL, *prepared = NULL, *selection = NULL;
    gboolean valid = !error;
    if (valid) {
        window = schema_window(job, calendar, definition, &event);
        resolved = radios_automation_calendar_resolve_event(window, event, &error); valid = resolved != NULL;
    }
    if (valid) { automation = radios_automation_calendar_prepare_config(json_node_get_object(resolved), NULL, &error); valid = automation != NULL; }
    if (valid) {
        prepared = request(job, cancel, "/v1/workflows/desktop-playout/prepare", "automation", NULL, NULL, automation, &error);
        valid = prepared && response_valid(job, json_node_get_object(prepared), "automation", &error);
    }
    if (valid) {
        selection = radios_automation_calendar_prepared_programme_at(json_node_get_object(resolved), json_node_get_object(prepared),
            radios_data_client_station_id(job->client), job->schema_id, 200, job->now, &error);
        if (!selection && error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_WOULD_BLOCK)) {
            g_clear_error(&error); fail(&error, "Eventin otomasyon şemasında şu an çalınabilir parça yok.");
        }
        valid = selection != NULL;
    }
    if (valid) {
        JsonObject *config = object(json_node_get_object(selection), "config");
        JsonArray *events = array(config, "events");
        if (!events || !json_array_get_length(events)) valid = fail(&error, "Eventin otomasyon şemasında şu an çalınabilir parça yok.");
        else {
            programme = json_object_new();
            gchar *title = bounded(*text(definition, "name") ? text(definition, "name") : "Live event şeması", 160);
            gchar *note = g_strdup_printf("Live event şeması: %s", title);
            json_object_set_string_member(programme, "title", title); json_object_set_string_member(programme, "date", text(config, "date"));
            json_object_set_string_member(programme, "note", note); g_free(note); g_free(title);
            json_object_set_member(programme, "events", radios_programme_clone(json_object_get_member(config, "events")));
            JsonNode *blocks = json_object_get_member(config, "blocks");
            json_object_set_member(programme, "blocks", blocks ? radios_programme_clone(blocks) : json_node_init_array(json_node_alloc(), json_array_new()));
        }
    }
    if (valid) {
        /* The schema must not have changed while its media was prepared. */
        JsonObject *original = json_node_get_object(record);
        JsonNode *fresh = request(job, cancel, "/v1/workflows/desktop/get", text(original, "kind"), "document_id", text(original, "_id"), NULL, &error);
        valid = fresh && radios_json_int(json_node_get_object(fresh), "_rev", 0) == radios_json_int(original, "_rev", 0) &&
            schema_definition(record_calendar(json_node_get_object(fresh)), job->schema_id);
        if (!valid && !error) fail(&error, "Eventin otomasyon şeması hazırlanırken değişti. Yeniden deneyin.");
        g_clear_pointer(&fresh, json_node_free);
    }
    g_clear_pointer(&selection, json_node_free); g_clear_pointer(&prepared, json_node_free);
    g_clear_pointer(&automation, json_node_free); g_clear_pointer(&resolved, json_node_free);
    if (window) json_object_unref(window);
    g_clear_pointer(&record, json_node_free);
    if (valid) g_task_return_pointer(task, programme, (GDestroyNotify)json_object_unref);
    else {
        if (programme) json_object_unref(programme);
        if (!error) fail(&error, "Eventin otomasyon şeması hazırlanamadı.");
        g_task_return_error(task, error);
    }
}
static void prepare_free(gpointer value)
{
    LocalPreparation *job = value;
    g_free(job->schema_id);
    if (job->destroy) job->destroy(job->user);
    if (job->client) radios_data_client_unref(job->client);
    if (job->configuration) json_object_unref(job->configuration);
    g_weak_ref_clear(&job->owner); g_free(job);
}
static void owner_destroyed(GtkWidget *owner, gpointer value)
{ (void)owner; g_cancellable_cancel(G_CANCELLABLE(value)); }
static void cancel_unref(gpointer value, GClosure *closure)
{ (void)closure; g_object_unref(value); }
static void prepare_complete(GObject *source, GAsyncResult *result, gpointer value)
{
    (void)source; (void)value; GTask *task = G_TASK(result); LocalPreparation *job = g_task_get_task_data(task);
    GError *error = NULL; JsonObject *programme = g_task_propagate_pointer(task, &error);
    GtkWidget *owner = g_weak_ref_get(&job->owner);
    if (owner) {
        if (g_signal_handler_is_connected(owner, job->owner_destroy)) g_signal_handler_disconnect(owner, job->owner_destroy);
        if (!gtk_widget_in_destruction(owner) && !g_cancellable_is_cancelled(g_task_get_cancellable(task))) {
            RadioSDataClient *current = radios_data_client_acquire(job->ctx);
            gboolean same = current && current == job->client && radios_data_connected(job->ctx);
            if (current) radios_data_client_unref(current);
            if (!same && !error) g_set_error_literal(&error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Veri oturumu değişti. Yerel Event'i yeniden ekleyin.");
            if (job->callback) job->callback(job->ctx, owner, error ? NULL : programme, error, job->user);
        }
        g_object_unref(owner);
    }
    g_clear_pointer(&programme, json_object_unref); g_clear_error(&error);
}
GCancellable *radios_local_event_prepare_async(RadioSViewContext *ctx,
    GtkWidget *owner, JsonObject *config, RadioSLocalEventPrepared callback,
    gpointer user, GDestroyNotify destroy)
{
    if (!ctx || !GTK_IS_WIDGET(owner) || !config || gtk_widget_in_destruction(owner)) {
        if (destroy) destroy(user);
        return NULL;
    }
    LocalPreparation *job = g_new0(LocalPreparation, 1); job->ctx = ctx;
    job->client = radios_data_client_acquire(ctx); job->configuration = radios_programme_clone_object(config);
    job->callback = callback; job->user = user; job->destroy = destroy; job->now = g_get_real_time();
    g_weak_ref_init(&job->owner, owner);
    GCancellable *cancel = g_cancellable_new(); GTask *task = g_task_new(NULL, cancel, prepare_complete, NULL);
    g_task_set_return_on_cancel(task, FALSE); g_task_set_task_data(task, job, prepare_free);
    job->owner_destroy = g_signal_connect_data(owner, "destroy", G_CALLBACK(owner_destroyed), g_object_ref(cancel), cancel_unref, 0);
    if (!job->client || !radios_data_connected(ctx))
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "Yerel Event hazırlamak için veritabanına bağlanın.");
    else g_task_run_in_thread(task, prepare_worker);
    g_object_unref(task); return cancel;
}
GCancellable *radios_local_event_prepare_schema_async(RadioSViewContext *ctx,
    GtkWidget *owner, const gchar *schema_id, gint64 duration_ms, RadioSLocalEventPrepared callback,
    gpointer user, GDestroyNotify destroy)
{
    if (!ctx || !GTK_IS_WIDGET(owner) || gtk_widget_in_destruction(owner) || !schema_id || !uuid(schema_id)) {
        if (destroy) destroy(user);
        return NULL;
    }
    LocalPreparation *job = g_new0(LocalPreparation, 1); job->ctx = ctx;
    job->client = radios_data_client_acquire(ctx); job->schema_id = g_strdup(schema_id); job->duration_ms = duration_ms;
    job->callback = callback; job->user = user; job->destroy = destroy; job->now = g_get_real_time();
    g_weak_ref_init(&job->owner, owner);
    GCancellable *cancel = g_cancellable_new(); GTask *task = g_task_new(NULL, cancel, prepare_complete, NULL);
    g_task_set_return_on_cancel(task, FALSE); g_task_set_task_data(task, job, prepare_free);
    job->owner_destroy = g_signal_connect_data(owner, "destroy", G_CALLBACK(owner_destroyed), g_object_ref(cancel), cancel_unref, 0);
    if (!job->client || !radios_data_connected(ctx))
        g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "Eventin şemasını hazırlamak için veritabanına bağlanın.");
    else g_task_run_in_thread(task, schema_worker);
    g_object_unref(task); return cancel;
}
