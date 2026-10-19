#include "radios/playout.h"
#include "radios/programme_model.h"
#include "radios/media_geometry.h"
#include "radios/copyright_policy.h"
#include "radios/automation_policy.h"
#include "radios/automation_calendar_plan.h"
#include "radios/transition_preset.h"
#include <math.h>
#include <string.h>

#define HISTORY_LIMIT 2000u
#define DAY_US (G_GINT64_CONSTANT(86400) * G_USEC_PER_SEC)
struct RadioSPlayoutHistory {
    GPtrArray *records;
    GHashTable *tracks, *artists, *albums, *counts;
    JsonObject *rotation;
};
typedef struct { gint64 last; guint count; } Seen;
static const gchar *str(JsonObject *o, const gchar *key, const gchar *fallback)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_STRING ? json_node_get_string(n) : fallback;
}
static gint64 integer(JsonObject *o, const gchar *key, gint64 fallback)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_INT64 ? json_node_get_int(n) : fallback;
}
static gdouble number(JsonObject *o, const gchar *key, gdouble fallback)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    if (!n || !JSON_NODE_HOLDS_VALUE(n)) return fallback;
    GType type = json_node_get_value_type(n);
    return type == G_TYPE_INT64 || type == G_TYPE_DOUBLE ? json_node_get_double(n) : fallback;
}
static gboolean flag(JsonObject *o, const gchar *key)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    return n && JSON_NODE_HOLDS_VALUE(n) && json_node_get_value_type(n) == G_TYPE_BOOLEAN && json_node_get_boolean(n);
}
static JsonObject *object(JsonObject *o, const gchar *key)
{ JsonNode *n = o ? json_object_get_member(o, key) : NULL; return n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL; }
static JsonArray *array(JsonObject *o, const gchar *key)
{ JsonNode *n = o ? json_object_get_member(o, key) : NULL; return n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : NULL; }
static gboolean media_eligible(JsonObject *row)
{ return row && flag(row, json_object_has_member(row, "media_eligible") ? "media_eligible" : "eligible"); }
static RadioSAudioCopyrightWindow row_copyright(JsonObject *row, JsonObject *policy)
{
    JsonObject *metadata = object(row, "metadata"), *media = object(object(row, "detail"), "media");
    return radios_copyright_policy_window(policy, *str(media, "artist", "") ? str(media, "artist", "") : str(metadata, "artist", ""),
        *str(media, "album", "") ? str(media, "album", "") : str(metadata, "album", ""),
        *str(media, "title", "") ? str(media, "title", "") : str(metadata, "title", ""));
}
static JsonObject *copy_object(JsonObject *source)
{
    JsonNode *n = json_node_new(JSON_NODE_OBJECT); json_node_set_object(n, source);
    gchar *json = json_to_string(n, FALSE); JsonParser *parser = json_parser_new();
    json_parser_load_from_data(parser, json, -1, NULL);
    JsonObject *result = json_object_ref(json_node_get_object(json_parser_get_root(parser)));
    g_object_unref(parser); g_free(json); json_node_free(n); return result;
}
static gchar *key_normal(const gchar *text)
{
    GString *mapped = g_string_new(NULL);
    for (const gchar *p = text ? text : ""; *p; p = g_utf8_next_char(p)) {
        gunichar c = g_utf8_get_char(p);
        g_string_append_unichar(mapped, c == 'I' ? 0x131 : c == 0x130 ? 'i' : c);
    }
    gchar *folded = g_utf8_casefold(mapped->str, -1); g_string_free(mapped, TRUE);
    gchar *key = g_utf8_normalize(folded, -1, G_NORMALIZE_ALL_COMPOSE);
    g_free(folded); g_strstrip(key); return key;
}
static void remember(GHashTable *table, const gchar *key, gint64 when)
{
    if (!key || !*key) return;
    Seen *seen = g_hash_table_lookup(table, key);
    if (!seen) { seen = g_new0(Seen, 1); g_hash_table_insert(table, g_strdup(key), seen); }
    seen->last = MAX(seen->last, when); ++seen->count;
}
RadioSPlayoutHistory *radios_playout_history_new(void)
{
    RadioSPlayoutHistory *h = g_new0(RadioSPlayoutHistory, 1);
    h->records = g_ptr_array_new_with_free_func((GDestroyNotify)json_object_unref);
    h->tracks = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    h->artists = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    h->albums = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    h->counts = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free); return h;
}
void radios_playout_history_free(RadioSPlayoutHistory *h)
{
    if (!h) return;
    g_ptr_array_unref(h->records); g_hash_table_unref(h->tracks); g_hash_table_unref(h->artists);
    g_hash_table_unref(h->albums); g_hash_table_unref(h->counts);
    if (h->rotation) json_object_unref(h->rotation);
    g_free(h);
}
void radios_playout_history_set_rotation(RadioSPlayoutHistory *h, JsonObject *rotation)
{
    if (!h) return;
    JsonObject *next = rotation ? copy_object(rotation) : NULL;
    if (h->rotation) json_object_unref(h->rotation);
    h->rotation = next;
}
static gint64 parse_stamp(const gchar *text)
{
    if (!text || !strchr(text, 'T')) return 0;
    GDateTime *dt = g_date_time_new_from_iso8601(text, NULL);
    if (!dt) return 0;
    gint64 time = g_date_time_to_unix(dt) * G_USEC_PER_SEC + g_date_time_get_microsecond(dt);
    g_date_time_unref(dt); return time;
}
static gchar *format_stamp(gint64 utc)
{
    GDateTime *dt = g_date_time_new_from_unix_utc(utc / G_USEC_PER_SEC);
    if (!dt) return g_strdup("");
    gint64 remainder = utc % G_USEC_PER_SEC;
    if (remainder) { GDateTime *precise = g_date_time_add(dt, remainder); g_date_time_unref(dt); dt = precise; }
    gchar *text = g_date_time_format(dt, remainder ? "%Y-%m-%dT%H:%M:%S.%fZ" : "%Y-%m-%dT%H:%M:%SZ"); g_date_time_unref(dt); return text;
}
void radios_playout_history_observe(RadioSPlayoutHistory *h, const gchar *media_id,
    const gchar *version_id, const gchar *artist, const gchar *album, gint64 utc, guint64 position)
{
    if (!h || !g_uuid_string_is_valid(media_id) || !g_uuid_string_is_valid(version_id) || utc <= 0 || !position) return;
    gchar *artist_key = key_normal(artist), *album_key = key_normal(album);
    remember(h->tracks, media_id, utc); remember(h->counts, media_id, utc);
    remember(h->artists, artist_key, utc); remember(h->albums, album_key, utc);
    g_free(artist_key); g_free(album_key);
    JsonObject *record = json_object_new(); gchar *stamp = format_stamp(utc);
    json_object_set_string_member(record, "media_id", media_id); json_object_set_string_member(record, "version_id", version_id);
    json_object_set_string_member(record, "artist", artist ? artist : ""); json_object_set_string_member(record, "album", album ? album : "");
    json_object_set_string_member(record, "started_utc", stamp); json_object_set_int_member(record, "position_frames", (gint64)position);
    g_free(stamp); g_ptr_array_add(h->records, record);
    if (h->records->len > HISTORY_LIMIT) g_ptr_array_remove_index(h->records, 0);
}
void radios_playout_history_import(RadioSPlayoutHistory *h, JsonArray *records)
{
    for (guint i = 0; h && records && i < MIN(json_array_get_length(records), HISTORY_LIMIT); ++i) {
        JsonObject *r = json_array_get_object_element(records, i);
        radios_playout_history_observe(h, str(r, "media_id", ""), str(r, "version_id", ""), str(r, "artist", ""),
            str(r, "album", ""), parse_stamp(str(r, "started_utc", "")), (guint64)MAX(integer(r, "position_frames", 0), 0));
    }
}
RadioSPlayoutHistory *radios_playout_history_copy(const RadioSPlayoutHistory *h)
{
    RadioSPlayoutHistory *copy = radios_playout_history_new();
    for (guint i = 0; h && i < h->records->len; ++i) {
        JsonObject *r = g_ptr_array_index(h->records, i);
        radios_playout_history_observe(copy, str(r, "media_id", ""), str(r, "version_id", ""), str(r, "artist", ""),
            str(r, "album", ""), parse_stamp(str(r, "started_utc", "")), (guint64)integer(r, "position_frames", 1));
    }
    if (h) radios_playout_history_set_rotation(copy, h->rotation);
    return copy;
}
JsonNode *radios_playout_history_json(const RadioSPlayoutHistory *h)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *o = json_object_new(); JsonArray *records = json_array_new();
    json_node_take_object(node, o); json_object_set_string_member(o, "kind", "local_playback");
    for (guint i = 0; h && i < h->records->len; ++i) json_array_add_object_element(records, copy_object(g_ptr_array_index(h->records, i)));
    json_object_set_array_member(o, "entries", records);
    if (h && h->rotation) json_object_set_object_member(o, "rotation", copy_object(h->rotation));
    return node;
}
static void entry_free(gpointer data)
{
    RadioSPlayoutEntry *e = data;
    g_free((gchar *)e->run.entry_id); g_free((gchar *)e->run.media.path); g_free((gchar *)e->run.media.sha256);
    g_free((gchar *)e->run.media.media_id); g_free((gchar *)e->run.media.version_id);
    g_free((gchar *)e->run.adjunct.media.path); g_free((gchar *)e->run.adjunct.media.sha256);
    g_free((gchar *)e->run.adjunct.media.media_id); g_free((gchar *)e->run.adjunct.media.version_id);
    g_free((gchar *)e->run.adjunct.entry_id); g_free((gchar *)e->run.adjunct.title); g_free((gchar *)e->run.adjunct.artist);
    g_free(e->title); g_free(e->artist); g_free(e->album); g_free(e->kind); g_free(e->source_id); g_free(e->source_name);
    if (e->binding) json_object_unref(e->binding);
    if (e->rotation_after) json_object_unref(e->rotation_after);
    if (e->verified_markers) json_object_unref(e->verified_markers);
    if (e->calendar_constraints) json_object_unref(e->calendar_constraints);
    if (e->copyright_metadata) json_object_unref(e->copyright_metadata);
    g_free(e);
}
void radios_playout_plan_free(RadioSPlayoutPlan *plan)
{ if (plan) { g_ptr_array_unref(plan->entries); json_node_free(plan->display); if (plan->programme_config) json_object_unref(plan->programme_config); if (plan->copyright_policy) json_object_unref(plan->copyright_policy); g_free(plan); } }
RadioSPlayoutPlan *radios_playout_plan_take_prefix(RadioSPlayoutPlan *plan, guint count)
{
    if (!plan || !count || !plan->entries->len) return NULL;
    count = MIN(count, plan->entries->len);
    RadioSPlayoutPlan *prefix = g_new0(RadioSPlayoutPlan, 1);
    *prefix = *plan; prefix->entries = g_ptr_array_new_with_free_func(entry_free);
    prefix->display = radios_programme_clone(plan->display);
    prefix->programme_config = plan->programme_config ? json_object_ref(plan->programme_config) : NULL;
    prefix->copyright_policy = plan->copyright_policy ? json_object_ref(plan->copyright_policy) : NULL;
    JsonArray *head = json_object_get_array_member(json_node_get_object(prefix->display), "entries");
    JsonArray *tail = json_object_get_array_member(json_node_get_object(plan->display), "entries");
    while (head && json_array_get_length(head) > count) json_array_remove_element(head, count);
    prefix->music_count = 0;
    for (guint i = 0; i < count; ++i) {
        RadioSPlayoutEntry *entry = g_ptr_array_steal_index(plan->entries, 0);
        g_ptr_array_add(prefix->entries, entry); prefix->music_count += !entry->jingle;
        if (tail && json_array_get_length(tail)) json_array_remove_element(tail, 0);
    }
    plan->music_count = 0;
    for (guint i = 0; i < plan->entries->len; ++i) plan->music_count += !((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, i))->jingle;
    return prefix;
}
void radios_playout_plan_copyright(RadioSPlayoutPlan *plan, JsonObject *policy)
{
    if (!plan) return;
    JsonObject *copy = policy ? copy_object(policy) : NULL;
    if (plan->copyright_policy) json_object_unref(plan->copyright_policy);
    plan->copyright_policy = copy;
    for (guint i = 0; i < plan->entries->len; ++i) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, i);
        entry->run.copyright = radios_copyright_policy_window(copy,
            str(entry->copyright_metadata, "artist", entry->artist),
            str(entry->copyright_metadata, "album", entry->album), str(entry->copyright_metadata, "title", entry->title));
        JsonObject *voice=object(entry->binding,"adjunct_copyright_metadata");
        if (voice) entry->run.adjunct.copyright=radios_copyright_policy_window(copy,
            str(voice,"artist",""),str(voice,"album",""),str(voice,"title",""));
    }
}
RadioSAudioRunItem *radios_playout_plan_items(const RadioSPlayoutPlan *plan)
{
    RadioSAudioRunItem *items = g_new0(RadioSAudioRunItem, plan->entries->len);
    for (guint i = 0; i < plan->entries->len; ++i) items[i] = ((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, i))->run;
    return items;
}
static gboolean fail(GError **error, const gchar *message)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message); return FALSE; }
static gint minute(const gchar *text)
{
    if (!text || strlen(text) != 5 || text[2] != ':' || !g_ascii_isdigit(text[0]) || !g_ascii_isdigit(text[1]) ||
        !g_ascii_isdigit(text[3]) || !g_ascii_isdigit(text[4])) return -1;
    gint h = (text[0] - '0') * 10 + text[1] - '0', m = (text[3] - '0') * 10 + text[4] - '0';
    return h < 24 && m < 60 ? h * 60 + m : -1;
}
static gint64 day_start(gint64 utc) { return utc - utc % DAY_US; }
static gint weekday(gint64 utc)
{ GDateTime *dt = g_date_time_new_from_unix_utc(utc / G_USEC_PER_SEC); gint result = dt ? g_date_time_get_day_of_week(dt) : 0; if (dt) g_date_time_unref(dt); return result; }
static gboolean day_allowed(JsonArray *days, gint day)
{ for (guint i = 0; days && i < json_array_get_length(days); ++i) if (json_array_get_int_element(days, i) == day) return TRUE; return FALSE; }
static gboolean window(gint64 utc, gint begin, gint end, gint64 *anchor, gint64 *close)
{
    gint64 today = day_start(utc); gint m = (gint)((utc - today) / (60 * G_USEC_PER_SEC));
    if (begin < 0 || end < 0) return FALSE;
    *anchor = today;
    if (begin == end) { *close = today + DAY_US; return TRUE; }
    if (begin < end) { *close = today + (gint64)end * 60 * G_USEC_PER_SEC; return m >= begin && m < end; }
    if (m < end) *anchor -= DAY_US;
    *close = *anchor + DAY_US + (gint64)end * 60 * G_USEC_PER_SEC;
    return m >= begin || m < end;
}
static gboolean source_open(JsonObject *config, JsonObject *source, gint64 utc, gint64 *close)
{
    gint64 anchor, global_end, source_anchor, source_end;
    if (!window(utc, minute(str(config, "windowStart", "")), minute(str(config, "windowEnd", "")), &anchor, &global_end) ||
        !day_allowed(array(config, "weekdays"), weekday(anchor))) return FALSE;
    if (!window(utc, minute(str(source, "start", "00:00")), minute(str(source, "end", "00:00")), &source_anchor, &source_end)) return FALSE;
    gint days = (gint)integer(source, "days", 0), day = weekday(source_anchor);
    if ((days == 1 && day >= 6) || (days == 2 && day <= 5)) return FALSE;
    *close = MIN(global_end, source_end); return TRUE;
}
static gint64 source_opening(JsonObject *config, JsonObject *source, gint64 utc)
{
    gint64 anchor, close, source_anchor, source_close;
    gint begin = minute(str(config, "windowStart", "")), end = minute(str(config, "windowEnd", ""));
    gint source_begin = minute(str(source, "start", "00:00")), source_end = minute(str(source, "end", "00:00"));
    if (!window(utc, begin, end, &anchor, &close) || !window(utc, source_begin, source_end, &source_anchor, &source_close)) return utc;
    return MAX(anchor + (gint64)(begin == end ? 0 : begin) * 60 * G_USEC_PER_SEC,
        source_anchor + (gint64)(source_begin == source_end ? 0 : source_begin) * 60 * G_USEC_PER_SEC);
}
static gboolean analysis_verified(JsonObject *detail, gint64 duration)
{ return radios_media_geometry(detail, duration, FALSE).verified; }
static void transition(RadioSPlayoutEntry *e, guint mode, gdouble overlap)
{
    JsonObject *marks = e->verified_markers; guint64 in = e->run.media.cue_in_frame, out = e->run.media.cue_out_frame;
    e->run.mode = (RadioSAudioRunMode)mode;
    e->run.auto_next_frame = mode == RADIOS_AUDIO_RUN_CUE && marks && flag(marks, "automatic") ?
        MIN(out, MAX(in, (guint64)MAX(integer(marks, "auto_next_ms", (gint64)(out / 48)), 0) * 48)) : out;
    e->run.fade_start_frame = mode == RADIOS_AUDIO_RUN_CUE && marks ? MAX(in, MIN(out, (guint64)MAX(integer(marks, "fade_start_ms", (gint64)(out / 48)), 0) * 48)) : out;
    e->run.fade_end_frame = mode == RADIOS_AUDIO_RUN_CUE && marks ? MAX(e->run.fade_start_frame, MIN(out, (guint64)MAX(integer(marks, "fade_end_ms", (gint64)(out / 48)), 0) * 48)) : out;
    e->run.overlap_frames = mode == RADIOS_AUDIO_RUN_CROSSFADE || mode == RADIOS_AUDIO_RUN_CUE ?
        MIN(out - in, (guint64)llround((mode == RADIOS_AUDIO_RUN_CUE && marks && flag(marks, "automatic") ? number(marks, "overlap_ms", overlap * 1000) / 1000 : overlap) * 48000)) : 0;
}
static void single_player_transition(RadioSPlayoutEntry *entry)
{
    /* A saved cue exit stays an exit. It does not authorize a second player
     * or the saved overlap/effect/beatmatch policy on this native transport. */
    transition(entry, entry->verified_markers && flag(entry->verified_markers, "automatic") ?
        RADIOS_AUDIO_RUN_CUE : RADIOS_AUDIO_RUN_EOF, 0);
    entry->run.overlap_frames = entry->run.fade_in_frames = 0;
    entry->run.effect = RADIOS_AUDIO_RUN_EFFECT_NONE; entry->run.beat_sync = FALSE; entry->run.shape.enabled = FALSE;
    entry->run.tempo_ratio = 1;
    entry->run.tempo_return_start_frames = entry->run.tempo_return_frames = 0;
}
static RadioSPlayoutEntry *entry_from_row(JsonObject *row, const RadioSPlayoutRequest *request,
    const gchar *source_id, const gchar *source_name, guint mode, gdouble overlap, GError **error)
{
    JsonObject *detail = object(row, "detail"), *media = object(detail, "media"), *version = object(detail, "version"), *marks = object(detail, "markers");
    JsonObject *metadata = object(row, "metadata");
    const gchar *mid = str(media, "_id", ""), *vid = str(version, "_id", ""), *fid = str(media, "media_file_id", ""), *sha = str(version, "sha256", "");
    gint64 duration = integer(version, "duration_ms", 0);
    if (!media_eligible(row) || !g_uuid_string_is_valid(str(row, "entry_id", "")) || !g_uuid_string_is_valid(mid) || !g_uuid_string_is_valid(vid) ||
        !g_uuid_string_is_valid(fid) || !g_uuid_string_is_valid(str(media, "item_id", "")) || !g_str_equal(str(media, "station_id", ""), request->station_id) ||
        !g_str_equal(str(media, "version_id", ""), vid) || !g_str_equal(str(media, "item_id", ""), str(version, "item_id", "!")) ||
        !g_str_equal(fid, str(version, "media_file_id", "!")) || !flag(version, "verified") || !flag(version, "object_available") ||
        duration < 1 || duration > 86400000 || strlen(sha) != 64 || mode > RADIOS_AUDIO_RUN_MANUAL || !isfinite(overlap) || overlap < 0 || overlap > 30) {
        fail(error, "Sırada doğrulanmamış, başka istasyona ait veya kimliği değişmiş medya var."); return NULL;
    }
    for (guint i = 0; i < 64; ++i) if (!g_ascii_isxdigit(sha[i])) { fail(error, "Medya SHA değeri geçersiz."); return NULL; }
    const gchar *managed = str(version, "managed_path", "");
    gchar *path = g_canonicalize_filename(managed, NULL), *root = g_strdup_printf("%s.objects%c", request->database_file, G_DIR_SEPARATOR);
    gboolean contained = g_path_is_absolute(managed) && g_str_has_prefix(path, root); g_free(root);
    if (!contained) { g_free(path); fail(error, "Medya yönetilen istasyon arşivinin dışında."); return NULL; }
    if (marks && (!g_str_equal(str(marks, "station_id", ""), request->station_id) || !g_str_equal(str(marks, "version_id", ""), vid) ||
        !g_str_equal(str(marks, "media_file_id", ""), fid) || !g_str_equal(str(marks, "media_sha256", ""), sha) ||
        integer(marks, "media_duration_ms", 0) != duration)) { g_free(path); fail(error, "Cue işaretleri bu medya sürümüne ait değil."); return NULL; }
    JsonArray *voice_cues = array(detail, "radio_cues");
    if (detail && json_object_has_member(detail, "radio_cues")) {
        if (!voice_cues || json_array_get_length(voice_cues) > 3) {
            g_free(path); fail(error, "Onaylı vokal işaretleri sınırı veya biçimi geçersiz."); return NULL;
        }
        for (guint i = 0; i < json_array_get_length(voice_cues); ++i) {
            JsonNode *node = json_array_get_element(voice_cues, i);
            JsonObject *cue = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
            const gchar *kind = str(cue, "cue_kind", "");
            if (!g_uuid_string_is_valid(str(cue, "_id", "")) || integer(cue, "_rev", 0) < 1 ||
                !g_str_equal(str(cue, "status", ""), "approved") ||
                !g_str_equal(str(cue, "station_id", ""), request->station_id) ||
                !g_str_equal(str(cue, "version_id", ""), vid) ||
                !g_str_equal(str(cue, "media_file_id", ""), fid) ||
                !g_str_equal(str(cue, "media_sha256", ""), sha) ||
                integer(cue, "media_duration_ms", 0) != duration || integer(cue, "position_ms", -1) < 0 ||
                integer(cue, "position_ms", duration + 1) > duration ||
                !(g_str_equal(kind, "intro_end") || g_str_equal(kind, "vocal_start") || g_str_equal(kind, "outro_start"))) {
                g_free(path); fail(error, "Onaylı vokal işareti bu istasyona ve medya sürümüne ait değil."); return NULL;
            }
        }
    }
    RadioSMediaGeometry geometry = radios_media_geometry(detail, duration, TRUE);
    if (!geometry.autodj_valid) { g_free(path); fail(error, "AutoDJ giriş/çıkış noktaları cue aralığının dışında veya sırası geçersiz."); return NULL; }
    guint64 frames = geometry.frames, in = request->single_player || mode == RADIOS_AUDIO_RUN_MANUAL ?
        geometry.cue_in : geometry.autodj_in, out = geometry.cue_out;
    /* AutoDJ IN/OUT are mix points: at OUT the next song starts over this
     * one's beats-only outro. One player cannot overlap, so there, as with
     * IN, the song is not cut at OUT. Automation never airs trailing silence. */
    gboolean mix_out = mode != RADIOS_AUDIO_RUN_MANUAL && !request->single_player && geometry.has_autodj_out;
    if (mode != RADIOS_AUDIO_RUN_MANUAL) out = radios_media_geometry_sound_out(detail, &geometry, in, mix_out ? geometry.autodj_out : 0);
    if (in >= out) { g_free(path); fail(error, "Cue aralığı gerçek medya örneklerinin dışında."); return NULL; }
    RadioSPlayoutEntry *e = g_new0(RadioSPlayoutEntry, 1); e->run.entry_id = g_strdup(str(row, "entry_id", ""));
    e->run.media = (RadioSAudioLoadSpec){path, g_strdup(sha), g_strdup(mid), g_strdup(vid), in, out};
    e->run.duration_frames = frames; e->run.preferred_deck = -1;
    e->run.estimated_duration = !geometry.verified;
    e->verified_analysis = analysis_verified(detail, duration);
    radios_rhythm_analysis_read(object(object(detail, "analysis"), "rhythm"), &e->run.rhythm);
    radios_loudness_read(object(detail, "analysis"), &e->run.loudness);
    e->run.tempo_ratio = 1;
    e->verified_markers = marks ? copy_object(marks) : NULL;
    /* Early transitions count back from the same exit the run uses. */
    if (e->verified_markers && request->single_player) json_object_remove_member(e->verified_markers, "autodj_out_ms");
    transition(e, mode, overlap);
    if (mix_out) e->run.autodj_out_frame = geometry.autodj_out;
    if (request->single_player) single_player_transition(e);
    e->title = g_strdup(str(metadata, "title", str(media, "title", ""))); e->artist = g_strdup(str(metadata, "artist", "")); e->album = g_strdup(str(metadata, "album", ""));
    e->kind = g_strdup(str(metadata, "kind", "music"));
    e->run.copyright = row_copyright(row, request->copyright_policy);
    e->copyright_metadata = json_object_new();
    const gchar *copyright_fields[] = {"artist", "album", "title"};
    for (guint i = 0; i < G_N_ELEMENTS(copyright_fields); ++i) {
        const gchar *field = copyright_fields[i], *value = str(media, field, "");
        json_object_set_string_member(e->copyright_metadata, field, *value ? value : str(metadata, field, ""));
    }
    e->source_id = g_strdup(source_id ? source_id : ""); e->source_name = g_strdup(source_name ? source_name : "Program");
    e->binding = json_object_new(); json_object_set_string_member(e->binding, "media_id", mid); json_object_set_string_member(e->binding, "version_id", vid);
    json_object_set_string_member(e->binding, "media_file_id", fid); json_object_set_string_member(e->binding, "media_sha256", sha);
    if (voice_cues) {
        JsonArray *copied = json_array_new();
        for (guint i = 0; i < json_array_get_length(voice_cues); ++i)
            json_array_add_object_element(copied, copy_object(json_array_get_object_element(voice_cues, i)));
        json_object_set_array_member(e->binding, "radio_cues", copied);
    }
    return e;
}
static void diagnose(RadioSPlayoutPlan *p, const gchar *reason, const gchar *entry)
{
    JsonObject *d = json_object_new(); json_object_set_string_member(d, "reason", reason); json_object_set_string_member(d, "entry_id", entry ? entry : "");
    json_array_add_object_element(array(json_node_get_object(p->display), "diagnostics"), d);
}
static gboolean protected_by(GHashTable *table, const gchar *key, gint64 utc, gint64 minutes)
{ Seen *seen = key && *key ? g_hash_table_lookup(table, key) : NULL; return minutes > 0 && seen && utc - seen->last < minutes * 60 * G_USEC_PER_SEC; }
static gboolean row_allowed(JsonObject *row, JsonObject *config, JsonObject *policy, RadioSPlayoutHistory *history, gint64 utc)
{
    if (!media_eligible(row)) return FALSE;
    RadioSAudioCopyrightWindow copyright = row_copyright(row, policy);
    if (!radios_copyright_window_allowed(&copyright, utc)) return FALSE;
    JsonObject *m = object(row, "metadata"), *media = object(object(row, "detail"), "media");
    const gchar *artist = str(m, "artist", ""), *album = str(m, "album", "");
    gchar *a = key_normal(artist), *b = key_normal(album);
    gboolean allowed = (!flag(config, "requireMetadata") || !g_str_equal(str(m, "kind", "music"), "music") || *a) &&
        !protected_by(history->tracks, str(media, "_id", ""), utc, integer(config, "trackGap", 0)) &&
        !protected_by(history->artists, a, utc, integer(config, "artistGap", 0)) &&
        !protected_by(history->albums, b, utc, integer(config, "albumGap", 0));
    g_free(a); g_free(b); return allowed;
}
static gint64 protected_until(GHashTable *table, const gchar *key, gint64 minutes)
{
    Seen *seen = key && *key ? g_hash_table_lookup(table, key) : NULL;
    return minutes > 0 && seen ? seen->last + minutes * 60 * G_USEC_PER_SEC : 0;
}
static gint64 row_not_before(JsonObject *row, JsonObject *config, RadioSPlayoutHistory *history)
{
    JsonObject *m = object(row, "metadata"), *media = object(object(row, "detail"), "media");
    gchar *a = key_normal(str(m, "artist", "")), *b = key_normal(str(m, "album", ""));
    gint64 utc = MAX(protected_until(history->tracks, str(media, "_id", ""), integer(config, "trackGap", 0)),
        MAX(protected_until(history->artists, a, integer(config, "artistGap", 0)), protected_until(history->albums, b, integer(config, "albumGap", 0))));
    g_free(a); g_free(b); return utc;
}
static guint64 random_next(guint64 *state)
{ *state ^= *state >> 12; *state ^= *state << 25; *state ^= *state >> 27; return *state * G_GUINT64_CONSTANT(2685821657736338717); }
static guint64 seed_hash(const gchar *seed)
{ guint64 value = G_GUINT64_CONSTANT(1469598103934665603); for (const guchar *c = (const guchar *)seed; *c; ++c) value = (value ^ *c) * G_GUINT64_CONSTANT(1099511628211); return value ? value : 1; }
static JsonObject *choose_track(JsonArray *tracks, JsonObject *config, JsonObject *policy, RadioSPlayoutHistory *history,
    gint64 utc, guint mode, guint *cursor, guint64 *rng, GHashTable *exclude)
{
    guint n = tracks ? json_array_get_length(tracks) : 0, chosen = 0, available = 0, lowest = G_MAXUINT; gint64 oldest = G_MAXINT64;
    JsonObject *result = NULL;
    for (guint i = 0; i < n; ++i) {
        guint index = mode == 1 ? (*cursor + i) % n : i;
        JsonObject *row = json_array_get_object_element(tracks, index);
        if (exclude && g_hash_table_contains(exclude, str(row, "entry_id", ""))) continue;
        if (!row_allowed(row, config, policy, history, utc)) continue;
        if (mode == 1) { result = row; chosen = index; break; }
        if (mode == 0) { ++available; if (random_next(rng) % available == 0) { result = row; chosen = index; } }
        else {
            Seen *seen = g_hash_table_lookup(history->counts, str(object(object(row, "detail"), "media"), "_id", ""));
            guint count = seen ? seen->count : 0; gint64 last = seen ? seen->last : 0;
            if (!result || count < lowest || (count == lowest && last < oldest)) { result = row; chosen = index; lowest = count; oldest = last; }
        }
    }
    if (result) *cursor = (chosen + 1) % n;
    return result;
}
static gboolean parse_pool(JsonObject *config, RadioSAudioRunOptions *options, GError **error)
{
    JsonArray *decks = array(config, "decks"); guint n = decks ? json_array_get_length(decks) : 0;
    if (!n || n > 32) return fail(error, "Otomasyon için en az bir gerçek oynatıcı seçin.");
    options->pool_count = integer(config, "deckMode", 0) == 1 ? 1 : n;
    gboolean seen[32] = {FALSE};
    for (guint i = 0; i < n; ++i) {
        const gchar *id = json_array_get_string_element(decks, i); gchar *end = NULL;
        gint64 value = id ? g_ascii_strtoll(id, &end, 10) : 0;
        gchar canonical[4]; g_snprintf(canonical, sizeof canonical, "%" G_GINT64_FORMAT, value);
        if (!id || !end || *end || value < 1 || value > 32 || !g_str_equal(id, canonical) || seen[value - 1]) return fail(error, "Otomasyon oynatıcı kimliği geçersiz veya yineleniyor.");
        seen[value - 1] = TRUE; if (i < options->pool_count) options->deck_pool[i] = (guint)value - 1;
    }
    return TRUE;
}
static JsonObject *deck_policy(JsonObject *config, gint deck)
{
    JsonArray *policies = array(config, "deckTransitions");
    for (guint index = 0; policies && index < json_array_get_length(policies); ++index) {
        JsonObject *policy = json_array_get_object_element(policies, index);
        if (g_ascii_strtoll(str(policy, "deck_id", ""), NULL, 10) - 1 == deck) return policy;
    }
    return NULL;
}
static gboolean deck_policies_valid(JsonObject *config, GError **error)
{
    if (!json_object_has_member(config, "deckTransitions")) return TRUE;
    JsonArray *policies = array(config, "deckTransitions"), *decks = array(config, "decks");
    if (!policies || json_array_get_length(policies) > 32) return fail(error, "AutoDJ deck geçiş listesi geçersiz.");
    gboolean seen[32] = {FALSE};
    for (guint index = 0; index < json_array_get_length(policies); ++index) {
        JsonNode *node = json_array_get_element(policies, index);
        if (!node || !JSON_NODE_HOLDS_OBJECT(node)) return fail(error, "AutoDJ deck geçiş kaydı geçersiz.");
        JsonObject *policy = json_node_get_object(node);
        const gchar *identifier = str(policy, "deck_id", "");
        gint64 deck = g_ascii_strtoll(identifier, NULL, 10), mode = integer(policy, "transition", -1);
        gchar canonical[4]; g_snprintf(canonical, sizeof canonical, "%" G_GINT64_FORMAT, deck);
        gdouble overlap = number(policy, "overlap", -1);
        gboolean selected = FALSE;
        for (guint selected_index = 0; selected_index < json_array_get_length(decks); ++selected_index)
            selected |= g_str_equal(identifier, json_array_get_string_element(decks, selected_index));
        if (!policy || json_object_get_size(policy) != 3 || deck < 1 || deck > 32 || !g_str_equal(identifier, canonical) ||
            !selected || seen[deck - 1] || mode < 0 || mode > 3 || !isfinite(overlap) || overlap < 0 || overlap > 30 ||
            ((mode == RADIOS_AUDIO_RUN_EOF || mode == RADIOS_AUDIO_RUN_MANUAL) && overlap != 0))
            return fail(error, "AutoDJ geçişi seçili ve tekil bir deck, geçerli tür ve 0–30 saniye bindirme gerektirir.");
        seen[deck - 1] = TRUE;
    }
    return TRUE;
}
static void deck_transition(JsonObject *config, RadioSPlayoutEntry *entry)
{
    JsonObject *policy = deck_policy(config, entry->run.preferred_deck);
    if (policy && !(entry->jingle && flag(config, "jingleDry")))
        transition(entry, (guint)integer(policy, "transition", 0), number(policy, "overlap", 0));
}
static gint64 date_time(const gchar *date, const gchar *time)
{
    if (!date || strlen(date) != 10 || minute(time) < 0) return 0;
    gchar *stamp = g_strdup_printf("%sT%s:00Z", date, time); gint64 utc = parse_stamp(stamp); g_free(stamp); return utc;
}
static guint64 trigger_offset(const RadioSPlayoutEntry *e)
{
    guint64 span = e->run.media.cue_out_frame - e->run.media.cue_in_frame;
    if (e->run.autodj_out_frame && e->run.mode != RADIOS_AUDIO_RUN_MANUAL) return e->run.autodj_out_frame - e->run.media.cue_in_frame;
    if (e->run.mode == RADIOS_AUDIO_RUN_CUE) return e->run.auto_next_frame - e->run.media.cue_in_frame;
    if (e->run.mode == RADIOS_AUDIO_RUN_CROSSFADE) return span - MIN(e->run.overlap_frames, span);
    return span;
}
static gint64 next_time(RadioSPlayoutPlan *p, JsonObject *config, const gchar *source_id, guint boundary, gdouble overlap, gint64 fallback)
{
    if (!p->entries->len) return fallback;
    RadioSPlayoutEntry copy = *(RadioSPlayoutEntry *)g_ptr_array_index(p->entries, p->entries->len - 1);
    if (boundary && g_strcmp0(copy.source_id, source_id) && !deck_policy(config, copy.run.preferred_deck) &&
        !(copy.jingle && flag(config, "jingleDry"))) transition(&copy, boundary - 1, overlap);
    return copy.planned_utc_us + (gint64)(trigger_offset(&copy) * 1000000 / 48000);
}
static void add_entry(RadioSPlayoutPlan *p, RadioSPlayoutEntry *e, gint64 *utc)
{
    guint64 incoming = 0; gint64 start = *utc;
    if (p->entries->len) {
        RadioSPlayoutEntry *previous = g_ptr_array_index(p->entries, p->entries->len - 1);
        guint64 span = previous->run.media.cue_out_frame - previous->run.media.cue_in_frame;
        guint64 incoming_cue = e->run.media.cue_out_frame - e->run.media.cue_in_frame;
        previous->run.overlap_frames = MIN(previous->run.overlap_frames,
            MIN(incoming_cue, radios_audio_run_incoming_span_frames(&e->run)));
        /* An overlap covers at most 40 % of the shorter item (the calendar
         * planner uses the same bound), so short items never stack or stall. */
        previous->run.overlap_frames = MIN(previous->run.overlap_frames, MIN(span, incoming_cue) * 2 / 5);
        guint64 offset = trigger_offset(previous);
        start = MAX(previous->planned_utc_us + (gint64)(offset * 1000000 / 48000) + e->hold_us, e->run.start_utc_us);
        guint64 elapsed = start > previous->planned_utc_us ? (guint64)(start - previous->planned_utc_us) * 48000 / 1000000 : 0;
        guint64 remaining = span > elapsed ? span - elapsed : 0;
        incoming = MIN(MIN(previous->run.overlap_frames, remaining), e->run.media.cue_out_frame - e->run.media.cue_in_frame);
        previous->run.overlap_frames = incoming;
    }
    /* The batch's final item has no successor yet: bound its own outgoing
     * overlap the same way, so a short last sweeper never overlaps itself. */
    e->run.overlap_frames = MIN(e->run.overlap_frames, (e->run.media.cue_out_frame - e->run.media.cue_in_frame) * 2 / 5);
    e->run.fade_in_frames = incoming;
    e->planned_utc_us = MAX(start, e->run.start_utc_us);
    *utc = e->planned_utc_us + (gint64)((e->run.media.cue_out_frame - e->run.media.cue_in_frame) * 1000000 / 48000);
    g_ptr_array_add(p->entries, e);
}
static void seed_programme(JsonObject *prepared, RadioSPlayoutHistory *history, gint64 start)
{
    JsonArray *tracks = array(object(prepared, "programme"), "tracks");
    for (guint i = 0; tracks && i < json_array_get_length(tracks); ++i) {
        JsonObject *r = json_array_get_object_element(tracks, i), *media = object(object(r, "detail"), "media"), *version = object(object(r, "detail"), "version"), *m = object(r, "metadata");
        if (media_eligible(r)) radios_playout_history_observe(history, str(media, "_id", ""), str(version, "_id", ""), str(m, "artist", ""), str(m, "album", ""), start, 1);
    }
}
typedef struct {
    guint cursors[52], source_index, in_turn, jingle_phase;
    guint64 rng;
    gboolean jingle_due;
    gchar *key;
    GHashTable *complete_seen;
} Rotation;
static JsonNode *canonical(JsonNode *source)
{
    if (JSON_NODE_HOLDS_OBJECT(source)) {
        JsonObject *o = json_node_get_object(source), *next = json_object_new();
        GList *keys = g_list_sort(json_object_get_members(o), (GCompareFunc)g_strcmp0);
        for (GList *key = keys; key; key = key->next) json_object_set_member(next, key->data, canonical(json_object_get_member(o, key->data)));
        g_list_free(keys); JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, next); return node;
    }
    if (JSON_NODE_HOLDS_ARRAY(source)) {
        JsonArray *a = json_node_get_array(source), *next = json_array_new();
        for (guint i = 0; i < json_array_get_length(a); ++i) json_array_add_element(next, canonical(json_array_get_element(a, i)));
        JsonNode *node = json_node_new(JSON_NODE_ARRAY); json_node_take_array(node, next); return node;
    }
    return json_node_copy(source);
}
static gchar *rotation_key(JsonObject *prepared)
{
    JsonObject *config = copy_object(object(prepared, "config"));
    json_object_remove_member(config, "planDate"); json_object_remove_member(config, "startTime"); json_object_remove_member(config, "previewCount");
    json_object_remove_member(config, "name");
    JsonArray *identities = json_array_new(), *sources = array(prepared, "sources");
    for (guint i = 0; sources && i < json_array_get_length(sources); ++i) {
        JsonObject *source = json_array_get_object_element(sources, i), *identity = json_object_new();
        const gchar *names[] = {"source_id", "playlist_id", "playlist_version_id", "role"};
        for (guint j = 0; j < G_N_ELEMENTS(names); ++j) json_object_set_string_member(identity, names[j], str(source, names[j], ""));
        json_object_set_int_member(identity, "playlist_revision", integer(source, "playlist_revision", 0));
        JsonArray *ids = json_array_new(), *tracks = array(source, "tracks");
        for (guint j = 0; tracks && j < json_array_get_length(tracks); ++j) json_array_add_string_element(ids, str(json_array_get_object_element(tracks, j), "entry_id", ""));
        json_object_set_array_member(identity, "entries", ids); json_array_add_object_element(identities, identity);
    }
    json_object_set_array_member(config, "prepared_sources", identities);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_take_object(node, config); JsonNode *sorted = canonical(node);
    gchar *text = json_to_string(sorted, FALSE), *key = g_compute_checksum_for_string(G_CHECKSUM_SHA256, text, -1);
    g_free(text); json_node_free(sorted); json_node_free(node); return key;
}
static gboolean hex(const gchar *text, guint length)
{
    if (!text || strlen(text) != length) return FALSE;
    for (guint i = 0; i < length; ++i) if (!g_ascii_isdigit(text[i]) && !(text[i] >= 'a' && text[i] <= 'f')) return FALSE;
    return TRUE;
}
static void rotation_restore(Rotation *r, JsonObject *state, guint n)
{
    JsonArray *cursors = array(state, "cursors"), *seen = array(state, "complete_seen");
    gint64 index = integer(state, "source_index", -1), turn = integer(state, "in_turn", -1), phase = integer(state, "jingle_phase", -1);
    const gchar *rng = str(state, "rng_hex", "");
    if (!state || integer(state, "schema", 0) != 1 || !g_str_equal(str(state, "config_key", ""), r->key) ||
        index < 0 || index >= n || turn < 0 || turn > 500 || phase < 0 || phase > 100 || !hex(rng, 16) ||
        !cursors || json_array_get_length(cursors) != n || !seen || json_array_get_length(seen) > 500) return;
    guint next[52] = {0};
    for (guint i = 0; i < n; ++i) {
        JsonNode *value = json_array_get_element(cursors, i);
        if (!JSON_NODE_HOLDS_VALUE(value) || json_node_get_value_type(value) != G_TYPE_INT64 || json_node_get_int(value) < 0 || json_node_get_int(value) > 2000) return;
        next[i] = (guint)json_node_get_int(value);
    }
    gint64 fallback = integer(state, "fallback_cursor", -1), jingle = integer(state, "jingle_cursor", -1);
    if (fallback < 0 || fallback > 2000 || jingle < 0 || jingle > 2000) return;
    for (guint i = 0; i < json_array_get_length(seen); ++i) {
        JsonNode *value = json_array_get_element(seen, i);
        if (!JSON_NODE_HOLDS_VALUE(value) || json_node_get_value_type(value) != G_TYPE_STRING || !g_uuid_string_is_valid(json_node_get_string(value))) return;
    }
    guint64 random = g_ascii_strtoull(rng, NULL, 16); if (!random) return;
    memcpy(r->cursors, next, sizeof next); r->cursors[50] = (guint)fallback; r->cursors[51] = (guint)jingle;
    r->source_index = (guint)index; r->in_turn = (guint)turn; r->rng = random;
    r->jingle_phase = (guint)phase; r->jingle_due = flag(state, "jingle_due");
    for (guint i = 0; i < json_array_get_length(seen); ++i) g_hash_table_add(r->complete_seen, g_strdup(json_array_get_string_element(seen, i)));
}
static JsonObject *rotation_snapshot(Rotation *r, guint n)
{
    JsonObject *state = json_object_new(); JsonArray *cursors = json_array_new(), *seen = json_array_new(); gchar rng[17];
    g_snprintf(rng, sizeof rng, "%016" G_GINT64_MODIFIER "x", r->rng);
    json_object_set_int_member(state, "schema", 1); json_object_set_string_member(state, "config_key", r->key);
    json_object_set_int_member(state, "source_index", r->source_index); json_object_set_int_member(state, "in_turn", r->in_turn);
    for (guint i = 0; i < n; ++i) json_array_add_int_element(cursors, r->cursors[i]);
    json_object_set_array_member(state, "cursors", cursors); json_object_set_string_member(state, "rng_hex", rng);
    json_object_set_int_member(state, "fallback_cursor", r->cursors[50]); json_object_set_int_member(state, "jingle_cursor", r->cursors[51]);
    json_object_set_int_member(state, "jingle_phase", r->jingle_phase); json_object_set_boolean_member(state, "jingle_due", r->jingle_due);
    GList *keys = g_list_sort(g_hash_table_get_keys(r->complete_seen), (GCompareFunc)g_strcmp0);
    for (GList *key = keys; key; key = key->next) json_array_add_string_element(seen, key->data);
    g_list_free(keys); json_object_set_array_member(state, "complete_seen", seen); return state;
}
static gboolean complete_remaining(JsonArray *tracks, JsonObject *config, JsonObject *policy, gint64 utc, GHashTable *seen)
{
    for (guint i = 0; tracks && i < json_array_get_length(tracks); ++i) {
        JsonObject *row = json_array_get_object_element(tracks, i), *metadata = object(row, "metadata");
        gchar *artist = key_normal(str(metadata, "artist", ""));
        RadioSAudioCopyrightWindow copyright = row_copyright(row, policy);
        gboolean eligible = media_eligible(row) && radios_copyright_window_allowed(&copyright, utc) && (!flag(config, "requireMetadata") ||
            !g_str_equal(str(metadata, "kind", "music"), "music") || *artist);
        g_free(artist);
        if (eligible && !g_hash_table_contains(seen, str(row, "entry_id", ""))) return TRUE;
    }
    return FALSE;
}
static gint adjacent_music_deck(RadioSPlayoutPlan *p, guint count, guint *phase, gint following)
{
    gint previous = p->entries->len ? ((RadioSPlayoutEntry *)g_ptr_array_index(p->entries, p->entries->len - 1))->run.preferred_deck : -1;
    if (count > 1 && (gint)p->options.deck_pool[*phase] == previous) *phase = (*phase + 1) % count;
    if (count > 1 && (gint)p->options.deck_pool[*phase] == following) *phase = (*phase + 1) % count;
    gint deck = (gint)p->options.deck_pool[*phase]; *phase = (*phase + 1) % count; return deck;
}
static gboolean calendar_beatmatch_plan(RadioSPlayoutPlan *, const RadioSPlayoutRequest *, GError **);
static void automation_beat_constraints(RadioSPlayoutEntry *entry, JsonObject *config, gint64 close)
{
    JsonObject *constraints = json_object_new();
    guint64 span = entry->run.media.cue_out_frame - entry->run.media.cue_in_frame;
    json_object_set_int_member(constraints, "earliest_utc_us", entry->run.start_utc_us);
    json_object_set_int_member(constraints, "latest_end_utc_us", close + (gint64)(span * G_USEC_PER_SEC / 48000));
    const gchar *input[] = {"trackGap", "artistGap", "albumGap"}, *output[] = {"track_gap_ms", "artist_gap_ms", "album_gap_ms"};
    for (guint i = 0; i < G_N_ELEMENTS(input); ++i) json_object_set_int_member(constraints, output[i], integer(config, input[i], 0) * 60000);
    json_object_set_int_member(constraints, "source_interval_ms", 0); json_object_set_boolean_member(constraints, "source_turn_start", FALSE);
    entry->calendar_constraints = constraints;
}
static gboolean raw_target_enabled(JsonObject *policy,const gchar *scope,const gchar *id)
{
    JsonArray *groups=array(policy,"children");JsonObject *matched=NULL,*rest=NULL;
    for (guint i=0;groups && i<json_array_get_length(groups);++i) {
        JsonObject *group=json_array_get_object_element(groups,i);if (!g_str_equal(str(group,"scope",""),scope)) continue;
        JsonArray *ids=array(group,"ids");if (!json_array_get_length(ids)) rest=group;
        for (guint j=0;j<json_array_get_length(ids);++j) if (!g_ascii_strcasecmp(json_array_get_string_element(ids,j),id)) matched=group;
    }
    JsonObject *group=matched?matched:rest;JsonNode *percent=group?json_object_get_member(group,"probability"):NULL;
    return !percent || JSON_NODE_HOLDS_NULL(percent) || json_node_get_double(percent)>0;
}
static gboolean raw_policy_row_allowed(JsonObject *row,JsonObject *policy,JsonObject *config,
    const RadioSPlayoutRequest *request,RadioSPlayoutHistory *history,gint64 utc)
{
    return row_allowed(row,config,request->copyright_policy,history,utc) && raw_target_enabled(policy,"track",str(row,"entry_id",""));
}
static JsonObject *raw_source(JsonArray *sources,JsonObject *rule)
{
    for (guint i=0;sources && i<json_array_get_length(sources);++i) {
        JsonObject *source=json_array_get_object_element(sources,i);
        if (g_str_equal(str(source,"source_id",""),str(rule,"source_id","!"))) return source;
    }
    return NULL;
}
static JsonObject *raw_policy_choose(JsonObject *policy,JsonObject *config,JsonArray *rules,JsonArray *sources,
    const RadioSPlayoutRequest *request,RadioSPlayoutHistory *history,RadioSPlayoutPlan *plan,gint64 utc,gint64 floor,
    guint boundary,RadioSAutomationPolicyState *state,JsonObject **out_source,JsonObject **out_rule,guint *out_index,gint64 *out_close)
{
    const gchar *ids[50];gboolean eligible[50]={FALSE};guint n=json_array_get_length(rules);gint64 when[50]={0},close[50]={0};
    for (guint i=0;i<n;++i) {
        JsonObject *rule=json_array_get_object_element(rules,i),*source=raw_source(sources,rule);
        ids[i]=str(source,"playlist_id",str(rule,"playlist_id",""));
        when[i]=MAX(next_time(plan,config,str(rule,"source_id",""),boundary,number(config,"boundaryOverlap",0),utc),floor);
        if (!source || !source_open(config,rule,when[i],&close[i]) || !raw_target_enabled(policy,"playlist",ids[i])) continue;
        JsonObject *child=radios_automation_policy_context(policy,"playlist",ids[i]);JsonArray *tracks=array(source,"tracks");
        for (guint j=0;tracks && j<json_array_get_length(tracks);++j)
            eligible[i]|=raw_policy_row_allowed(json_array_get_object_element(tracks,j),child,config,request,history,when[i]);
        json_object_unref(child);
    }
    gint selected=radios_automation_policy_choose_targets(policy,"playlist",ids,eligible,n,"raw-automation/playlists",state,utc);
    if (selected<0) return NULL;
    guint i=(guint)selected;JsonObject *rule=json_array_get_object_element(rules,i),*source=raw_source(sources,rule);
    JsonObject *child=radios_automation_policy_context(policy,"playlist",ids[i]);JsonArray *tracks=array(source,"tracks");
    guint count=tracks?json_array_get_length(tracks):0;const gchar *track_ids[500];gboolean track_eligible[500]={FALSE};
    for (guint j=0;j<count;++j) {
        JsonObject *row=json_array_get_object_element(tracks,j);track_ids[j]=str(row,"entry_id","");
        track_eligible[j]=raw_policy_row_allowed(row,child,config,request,history,when[i]);
    }
    gchar *key=g_strdup_printf("raw-automation/tracks/%s",ids[i]);
    gint track=radios_automation_policy_choose_targets(child,"track",track_ids,track_eligible,count,key,state,when[i]);
    g_free(key);json_object_unref(child);if (track<0) return NULL;
    *out_source=source;*out_rule=rule;*out_index=i;*out_close=close[i];return json_array_get_object_element(tracks,(guint)track);
}
static gboolean automation_plan(RadioSPlayoutPlan *p, JsonObject *prepared, const RadioSPlayoutRequest *request, GError **error)
{
    JsonObject *config = object(prepared, "config"); JsonArray *sources = array(prepared, "sources"), *rules = array(config, "sources");
    guint n = rules ? json_array_get_length(rules) : 0, target = (guint)integer(config, "previewCount", 0);
    if (!config || !n || n > 50 || target < 1 || target > 200 || !parse_pool(config, &p->options, error) ||
        !deck_policies_valid(config, error)) return error && *error ? FALSE : fail(error, "Rotasyon kaynakları veya plan sınırı geçersiz.");
    for (guint i = 0; sources && i < json_array_get_length(sources); ++i) {
        JsonArray *tracks = array(json_array_get_object_element(sources, i), "tracks");
        if (tracks && json_array_get_length(tracks) > 500) return fail(error, "Bir rotasyon kaynağı en fazla 500 kayıt içerebilir.");
    }
    gint64 utc = request->start_utc_us ? request->start_utc_us : date_time(str(config, "planDate", ""), str(config, "startTime", ""));
    if (!utc) return fail(error, "Rotasyonun UTC tarihi/saati geçersiz.");
    if (request->live) utc = MAX(utc, request->now_utc_us);
    guint music_pool_count = p->options.pool_count;
    const gchar *jingle_deck = str(config, "jingleDeck", "");
    if (flag(config, "jingleEnabled") && *jingle_deck) {
        guint value = (guint)g_ascii_strtoll(jingle_deck, NULL, 10);
        if (value < 1 || value > 32) return fail(error, "Jingle oynatıcı kimliği geçersiz.");
        gboolean exists = FALSE;
        for (guint i = 0; i < p->options.pool_count; ++i) if (p->options.deck_pool[i] == value - 1) exists = TRUE;
        if (!exists) p->options.deck_pool[p->options.pool_count++] = value - 1;
    }
    guint completed = 0, music_phase = request->deck_phase % music_pool_count;
    guint source_mode = (guint)integer(config, "sourceMode", 0), track_mode = (guint)integer(config, "trackMode", 0);
    guint mode = request->has_default_policy ? request->mode : (guint)integer(config, "transition", 0), boundary = (guint)integer(config, "boundaryTransition", 0);
    if (source_mode > 2 || track_mode > 2 || mode > 3 || boundary > 4) return fail(error, "Rotasyon seçim/geçiş yöntemi geçersiz.");
    if (request->single_player) boundary = 0;
    Rotation rotation = {.key = rotation_key(prepared), .rng = seed_hash(str(config, "seed", "")),
        .complete_seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL)};
    RadioSPlayoutHistory *history = radios_playout_history_copy(request->history);
    if (!request->history) {
        JsonObject *saved = object(object(prepared, "history"), "config");
        radios_playout_history_import(history, array(saved, "entries"));
        radios_playout_history_set_rotation(history, object(saved, "rotation"));
    }
    rotation_restore(&rotation, history->rotation, n);
    if (flag(config, "includeProgramme")) seed_programme(prepared, history, utc);
    JsonObject *raw_policy=object(config,"probability_policy"),*raw_context=object(config,"policy_context"),*selection_policy=NULL;
    RadioSAutomationPolicyState *selection_state=NULL;
    if (json_object_has_member(config,"probability_policy")) {
        if (!raw_policy || !radios_automation_policy_validate(raw_policy,error)) {
            radios_playout_history_free(history);g_hash_table_unref(rotation.complete_seen);g_free(rotation.key);
            if (!error || !*error) fail(error,"Ham otomasyon olasılık kuralı geçersiz.");
            return FALSE;
        }
        const gchar *aid=str(raw_context,"automation_id","");
        selection_policy=g_uuid_string_is_valid(aid)?radios_automation_policy_context(raw_policy,"automation",aid):copy_object(raw_policy);
        selection_state=radios_automation_policy_state_new(str(selection_policy,"seed",str(config,"seed","")),object(prepared,"policy_selection_checkpoint"),error);
        if (!selection_state) { json_object_unref(selection_policy);radios_playout_history_free(history);g_hash_table_unref(rotation.complete_seen);g_free(rotation.key);return FALSE; }
    }
    gboolean ok = TRUE, lookahead_added = FALSE; gint64 window_floor = 0;
    while (completed <= target && p->entries->len < RADIOS_AUDIO_RUN_MAX_ITEMS) {
        if (rotation.jingle_due) {
            gint64 jingle_anchor, jingle_close;
            gint64 when = MAX(next_time(p, config, "", 0, 0, utc), window_floor);
            if (!request->single_player && flag(config, "jingleDry") && p->entries->len) {
                RadioSPlayoutEntry *last = g_ptr_array_index(p->entries, p->entries->len - 1);
                when = last->planned_utc_us + (gint64)((last->run.media.cue_out_frame - last->run.media.cue_in_frame) * 1000000 / 48000);
            }
            if (!window(when, minute(str(config, "windowStart", "")), minute(str(config, "windowEnd", "")), &jingle_anchor, &jingle_close) ||
                !day_allowed(array(config, "weekdays"), weekday(jingle_anchor))) {
                diagnose(p, "jingle_outside_day_window", ""); rotation.jingle_due = FALSE; continue;
            }
            JsonObject *jingle = NULL, *source = NULL;
            for (guint k = 0; sources && k < json_array_get_length(sources); ++k) {
                JsonObject *s = json_array_get_object_element(sources, k);
                if (g_str_equal(str(s, "role", ""), "jingle")) {
                    source = s; jingle = choose_track(array(s, "tracks"), config, request->copyright_policy, history, when, 1, &rotation.cursors[51], &rotation.rng, NULL); break;
                }
            }
            if (!jingle) { diagnose(p, "jingle_unavailable_rules_preserved", ""); break; }
            RadioSPlayoutEntry *j = entry_from_row(jingle, request, str(source, "source_id", ""), str(source, "source_name", "Jingle"),
                flag(config, "jingleDry") ? 0 : mode, request->has_default_policy ? request->overlap_seconds : number(config, "overlap", 0), error);
            if (!j) { ok = FALSE; break; }
            if (*str(source, "playlist_id", ""))
                json_object_set_string_member(j->binding, "playlist_id", str(source, "playlist_id", ""));
            j->run.latest_start_utc_us = jingle_close;
            j->run.start_utc_us = MAX(jingle_anchor, row_not_before(jingle, config, history));
            if (request->has_default_policy && request->default_beat_sync) automation_beat_constraints(j, config, jingle_close);
            j->jingle = TRUE; const gchar *deck = str(config, "jingleDeck", "");
            if (*deck) j->run.preferred_deck = (gint)g_ascii_strtoll(deck, NULL, 10) - 1;
            else j->run.preferred_deck = adjacent_music_deck(p, music_pool_count, &music_phase, -1);
            if (!request->single_player) deck_transition(config, j);
            if (!request->single_player && flag(config, "jingleDry") && p->entries->len)
                transition(g_ptr_array_index(p->entries, p->entries->len - 1), RADIOS_AUDIO_RUN_EOF, 0);
            add_entry(p, j, &utc); rotation.jingle_due = FALSE;
            j->rotation_after = rotation_snapshot(&rotation, n);
            radios_playout_history_observe(history, j->run.media.media_id, j->run.media.version_id, j->artist, j->album, j->planned_utc_us, 1);
            if (completed == target) { lookahead_added = TRUE; break; }
            continue;
        }
        JsonObject *chosen = NULL, *chosen_source = NULL, *chosen_rule = NULL; guint chosen_index = 0;
        gint64 chosen_close = 0, earliest_candidate = G_MAXINT64; guint candidate_indices[50], weights = 0, open_count = 0;
        for (guint i = 0; i < n; ++i) {
            JsonObject *rule = json_array_get_object_element(rules, i); gint64 close;
            gint64 when = MAX(next_time(p, config, str(rule, "source_id", ""), boundary, number(config, "boundaryOverlap", 0), utc), window_floor);
            earliest_candidate = MIN(earliest_candidate, when);
            if (source_open(config, rule, when, &close)) { candidate_indices[open_count++] = i; weights += (guint)integer(rule, "weight", 1); }
        }
        if (!open_count) {
            gint64 next = (earliest_candidate / (60 * G_USEC_PER_SEC) + 1) * 60 * G_USEC_PER_SEC;
            gboolean found = FALSE;
            for (guint j = 0; j < 7 * 1440 && !found; ++j, next += 60 * G_USEC_PER_SEC)
                for (guint i = 0; i < n && !found; ++i) { gint64 close; if (source_open(config, json_array_get_object_element(rules, i), next, &close)) { window_floor = utc = next; found = TRUE; } }
            if (!found) { diagnose(p, "no_open_source_window", ""); break; }
            continue;
        }
        if (selection_state) {
            chosen=raw_policy_choose(selection_policy,config,rules,sources,request,history,p,utc,window_floor,boundary,
                selection_state,&chosen_source,&chosen_rule,&chosen_index,&chosen_close);
        } else {
        guint preferred = rotation.source_index;
        if (source_mode == 1) {
            guint draw = weights ? (guint)(random_next(&rotation.rng) % weights) : 0;
            for (guint i = 0; i < open_count; ++i) { guint w = (guint)integer(json_array_get_object_element(rules, candidate_indices[i]), "weight", 1); if (draw < w) { preferred = candidate_indices[i]; break; } draw -= w; }
        }
        for (guint attempt = 0; attempt < n; ++attempt) {
            guint i = (preferred + attempt) % n; JsonObject *rule = json_array_get_object_element(rules, i); gint64 close;
            gint64 when = MAX(next_time(p, config, str(rule, "source_id", ""), boundary, number(config, "boundaryOverlap", 0), utc), window_floor);
            if (!source_open(config, rule, when, &close)) continue;
            JsonObject *source = NULL;
            for (guint j = 0; sources && j < json_array_get_length(sources); ++j) {
                JsonObject *s = json_array_get_object_element(sources, j);
                if (g_str_equal(str(s, "source_id", ""), str(rule, "source_id", "!"))) { source = s; break; }
            }
            if (source_mode == 2 && i == rotation.source_index && !complete_remaining(array(source, "tracks"), config, request->copyright_policy, when, rotation.complete_seen)) {
                rotation.source_index = (i + 1) % n; rotation.in_turn = 0; g_hash_table_remove_all(rotation.complete_seen); continue;
            }
            chosen = choose_track(array(source, "tracks"), config, request->copyright_policy, history, when, track_mode, &rotation.cursors[i], &rotation.rng,
                source_mode == 2 && i == rotation.source_index ? rotation.complete_seen : NULL);
            if (chosen) { chosen_source = source; chosen_rule = rule; chosen_index = i; chosen_close = close; break; }
            diagnose(p, "source_empty_or_repeat_protected", str(rule, "source_id", ""));
            if (integer(config, "fallbackMode", 0) != 1) break;
        }
        }
        if (!selection_state && !chosen && integer(config, "fallbackMode", 0) == 2) {
            for (guint j = 0; sources && j < json_array_get_length(sources); ++j) {
                JsonObject *s = json_array_get_object_element(sources, j);
                if (g_str_equal(str(s, "role", ""), "fallback")) {
                    gint64 when = MAX(next_time(p, config, str(s, "source_id", ""), boundary, number(config, "boundaryOverlap", 0), utc), window_floor);
                    chosen = choose_track(array(s, "tracks"), config, request->copyright_policy, history, when, track_mode, &rotation.cursors[50], &rotation.rng, NULL);
                    if (chosen) {
                        gint64 anchor;
                        window(when, minute(str(config, "windowStart", "")), minute(str(config, "windowEnd", "")), &anchor, &chosen_close);
                        chosen_source = s; diagnose(p, "fallback_playlist_selected", str(chosen, "entry_id", ""));
                    } break;
                }
            }
        }
        if (!chosen) { diagnose(p, "no_eligible_track_rules_preserved", ""); break; }
        RadioSPlayoutEntry *e = entry_from_row(chosen, request, str(chosen_source, "source_id", ""), str(chosen_source, "source_name", ""), mode, request->has_default_policy ? request->overlap_seconds : number(config, "overlap", 0), error);
        if (!e) { ok = FALSE; break; }
        if (*str(chosen_source, "playlist_id", ""))
            json_object_set_string_member(e->binding, "playlist_id", str(chosen_source, "playlist_id", ""));
        if (selection_state) {
            json_object_set_object_member(e->binding,"policy_selection_checkpoint",radios_automation_policy_state_snapshot(selection_state));
            JsonObject *context=json_object_new();json_object_set_string_member(context,"automation_id",str(raw_context,"automation_id",""));
            json_object_set_string_member(context,"playlist_id",str(chosen_source,"playlist_id",""));json_object_set_string_member(context,"entry_id",e->run.entry_id);
            json_object_set_object_member(e->binding,"policy_context",context);
        }
        e->run.latest_start_utc_us = chosen_close;
        gint64 when = MAX(next_time(p, config, str(chosen_source, "source_id", ""), boundary, number(config, "boundaryOverlap", 0), utc), window_floor);
        e->run.start_utc_us = MAX(window_floor, MAX(source_opening(config, chosen_rule, when), row_not_before(chosen, config, history)));
        if (selection_state || (request->has_default_policy && request->default_beat_sync)) automation_beat_constraints(e, config, chosen_close);
        gint following_jingle = flag(config, "jingleEnabled") && *jingle_deck &&
            rotation.jingle_phase + 1 >= (guint)MAX(integer(config, "jingleEvery", 1), 1) ? (gint)g_ascii_strtoll(jingle_deck, NULL, 10) - 1 : -1;
        e->run.preferred_deck = adjacent_music_deck(p, music_pool_count, &music_phase, following_jingle);
        if (!request->single_player) deck_transition(config, e);
        e->run.beat_sync = request->has_default_policy && request->default_beat_sync && e->run.mode == RADIOS_AUDIO_RUN_CROSSFADE && g_str_equal(e->kind, "music");
        if (music_pool_count == 1 && e->run.overlap_frames) { entry_free(e); ok = fail(error, "Bindirmeli müzik için en az iki müzik oynatıcısı seçin."); break; }
        if (!request->single_player && p->entries->len) {
            RadioSPlayoutEntry *previous = g_ptr_array_index(p->entries, p->entries->len - 1);
            if (previous->jingle && flag(config, "jingleDry")) transition(previous, RADIOS_AUDIO_RUN_EOF, 0);
            else if (boundary && g_strcmp0(previous->source_id, e->source_id) && !deck_policy(config, previous->run.preferred_deck)) {
                transition(previous, boundary - 1, number(config, "boundaryOverlap", 0));
            }
        }
        add_entry(p, e, &utc); window_floor = 0; ++completed; ++p->music_count;
        radios_playout_history_observe(history, e->run.media.media_id, e->run.media.version_id, e->artist, e->album, e->planned_utc_us, 1);
        if (chosen_rule) {
            if (chosen_index != rotation.source_index) { rotation.source_index = chosen_index; rotation.in_turn = 0; g_hash_table_remove_all(rotation.complete_seen); }
            if (source_mode != 1) ++rotation.in_turn;
            if (source_mode == 2) g_hash_table_add(rotation.complete_seen, g_strdup(e->run.entry_id));
            guint quota = (guint)integer(chosen_rule, "count", 1);
            gboolean finished = source_mode == 2 ? !complete_remaining(array(chosen_source, "tracks"), config, request->copyright_policy, utc, rotation.complete_seen) : rotation.in_turn >= MAX(quota, 1u);
            if (source_mode != 1 && finished) { rotation.source_index = (rotation.source_index + 1) % n; rotation.in_turn = 0; g_hash_table_remove_all(rotation.complete_seen); }
        }
        if (flag(config, "jingleEnabled")) {
            ++rotation.jingle_phase;
            if (rotation.jingle_phase >= (guint)MAX(integer(config, "jingleEvery", 1), 1)) { rotation.jingle_phase = 0; rotation.jingle_due = TRUE; }
        }
        e->rotation_after = rotation_snapshot(&rotation, n);
        if (completed > target) { lookahead_added = TRUE; --p->music_count; break; }
    }
    if (lookahead_added) g_ptr_array_remove_index(p->entries, p->entries->len - 1);
    radios_playout_history_free(history);
    g_hash_table_unref(rotation.complete_seen); g_free(rotation.key);
    if (selection_state) radios_automation_policy_state_free(selection_state);
    if (selection_policy) json_object_unref(selection_policy);
    if (!ok) return FALSE;
    if (!p->entries->len) return fail(error, "Kurallar ve zaman pencereleri içinde doğrulanmış uygun parça bulunamadı.");
    p->options.start_utc_us = ((RadioSPlayoutEntry *)g_ptr_array_index(p->entries, 0))->planned_utc_us;
    if (request->live && p->options.start_utc_us <= request->now_utc_us) p->options.start_utc_us = 0;
    p->automation = TRUE; return ok && calendar_beatmatch_plan(p, request, error);
}
static gdouble run_output_at(const RadioSAudioRunItem *run, guint64 source_frame)
{
    RadioSAudioTempoProfile profile = radios_audio_run_tempo_profile(run);
    return radios_audio_tempo_profile_output(&profile,
        source_frame > run->media.cue_in_frame ? source_frame - run->media.cue_in_frame : 0);
}
static guint64 run_duration_frames(const RadioSAudioRunItem *run)
{ return (guint64)ceil(run_output_at(run, run->media.cue_out_frame)); }

static gboolean calendar_constraints_plan(RadioSPlayoutPlan *plan, const RadioSPlayoutRequest *request, GError **error)
{
    RadioSPlayoutHistory *history = radios_playout_history_copy(request->history);
    GHashTable *turns = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    gboolean valid = TRUE;
    for (guint i = 0; i < plan->entries->len && valid; ++i) {
        RadioSPlayoutEntry *e = g_ptr_array_index(plan->entries, i); JsonObject *c = e->calendar_constraints;
        if (c) {
            gint64 earliest = integer(c, "earliest_utc_us", -1), end = integer(c, "latest_end_utc_us", -1);
            guint64 duration = run_duration_frames(&e->run);
            gint64 length = (gint64)(duration * G_USEC_PER_SEC / 48000);
            if (e->planned_utc_us < earliest || end < length || e->planned_utc_us > end - length) {
                if (request->trim_calendar_end && i && e->planned_utc_us >= earliest) { g_ptr_array_set_size(plan->entries, i); break; }
                valid = fail(error, "Tempo ayarlı rotasyon parçası playlist veya program zaman aralığını aşıyor."); break;
            }
            gchar *artist = key_normal(e->artist), *album = key_normal(e->album);
            Seen *seen[] = {g_hash_table_lookup(history->tracks, e->run.media.media_id),
                g_hash_table_lookup(history->artists, artist), g_hash_table_lookup(history->albums, album)};
            const gchar *gaps[] = {"track_gap_ms", "artist_gap_ms", "album_gap_ms"}; gboolean repeat = FALSE;
            for (guint j = 0; j < 3 && !repeat; ++j) {
                gint64 gap = integer(c, gaps[j], 0);
                repeat = gap && seen[j] && (e->planned_utc_us < seen[j]->last || (e->planned_utc_us - seen[j]->last) / 1000 < gap);
            }
            g_free(artist); g_free(album);
            /* The selection estimated this timeline before transitions, cue
             * exits and beatmatch set the real one. A later item that now
             * repeats (or opens its playlist turn) too early is dropped with
             * its tail and refilled later, like an item pushed past the block
             * end; a first item or a plain programme start still fails. */
            if (repeat) {
                if (request->trim_calendar_end && i) { diagnose(plan, "calendar_repeat_gap_trimmed", e->run.entry_id); g_ptr_array_set_size(plan->entries, i); break; }
                valid = fail(error, "Tempo ayarlı gerçek çalma sırası parça, sanatçı veya albüm tekrar süresini karşılamıyor."); break;
            }
            if (flag(c, "source_turn_start")) {
                Seen *last = g_hash_table_lookup(turns, e->source_id); gint64 interval = integer(c, "source_interval_ms", 0);
                if (interval && last && (e->planned_utc_us < last->last || (e->planned_utc_us - last->last) / 1000 < interval)) {
                    if (request->trim_calendar_end && i) { diagnose(plan, "calendar_turn_interval_trimmed", e->run.entry_id); g_ptr_array_set_size(plan->entries, i); break; }
                    valid = fail(error, "Tempo ayarlı playlist sırası belirlenen playlist dönüş sıklığını karşılamıyor."); break;
                }
                remember(turns, e->source_id, e->planned_utc_us);
            }
            e->run.start_utc_us = MAX(e->run.start_utc_us, earliest);
            e->run.latest_start_utc_us = end - length;
        }
        radios_playout_history_observe(history, e->run.media.media_id, e->run.media.version_id,
            e->artist, e->album, e->planned_utc_us, 1);
    }
    g_hash_table_unref(turns); radios_playout_history_free(history); return valid;
}
static gboolean calendar_constraints_valid(JsonObject *row, RadioSPlayoutEntry *entry, GError **error)
{
    if (!json_object_has_member(row, "calendar_constraints")) return TRUE;
    JsonObject *c = object(row, "calendar_constraints");
    const gchar *keys[] = {"earliest_utc_us", "latest_end_utc_us", "track_gap_ms", "artist_gap_ms", "album_gap_ms", "source_interval_ms"};
    if (!c || json_object_get_size(c) != 7 || !g_uuid_string_is_valid(entry->source_id))
        return fail(error, "Doğrulanmış rotasyon zaman/tekrar sözleşmesi eksik.");
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) {
        gint64 value = integer(c, keys[i], -1);
        if (value < 0 || (i >= 2 && value > G_MAXINT64 / 1000)) return fail(error, "Rotasyon zaman/tekrar sınırı geçersiz.");
    }
    JsonNode *turn = json_object_get_member(c, "source_turn_start");
    if (!turn || !JSON_NODE_HOLDS_VALUE(turn) || json_node_get_value_type(turn) != G_TYPE_BOOLEAN ||
        integer(c, "earliest_utc_us", 0) <= 0 || integer(c, "latest_end_utc_us", 0) <= integer(c, "earliest_utc_us", 0))
        return fail(error, "Rotasyon playlist zaman aralığı geçersiz.");
    entry->calendar_constraints = copy_object(c); return TRUE;
}

static gboolean calendar_transition_valid(JsonObject *hint, guint *mode, gdouble *overlap,
    gboolean *sync, RadioSAudioRunEffect *effect, RadioSAudioTransitionShape *shape, GError **error)
{
    gint64 value = integer(hint, "mode", -1), ms = integer(hint, "overlap_ms", -1);
    /* Mode 3 (native MANUAL) holds the transport before a live step; it never overlaps. */
    if (!hint || value < 0 || value > 3 || ms < 0 || ms > 30000 || ((!value || value == 3) && ms))
        return fail(error, "Takvim parçasının doğrulanmış geçiş türü veya bindirme süresi geçersiz.");
    GList *keys = json_object_get_members(hint); gboolean valid = TRUE;
    for (GList *item = keys; item; item = item->next) {
        const gchar *key = item->data;
        valid &= g_str_equal(key, "mode") || g_str_equal(key, "overlap_ms") ||
            g_str_equal(key, "beat_sync") || g_str_equal(key, "effect") || g_str_equal(key, "shape");
    }
    g_list_free(keys);
    /* A designed preset shape rides only on a real crossfade boundary. */
    memset(shape, 0, sizeof *shape); shape->effect = -1;
    if (json_object_has_member(hint, "shape") &&
        (value != 1 || !ms || !radios_transition_preset_to_shape(object(hint, "shape"), shape)))
        return fail(error, "Rotasyonun tasarlanmış geçiş ön ayarı geçersiz veya bindirmesiz sınırda kullanılmış.");
    JsonNode *node = json_object_get_member(hint, "beat_sync");
    if (node && (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN)) valid = FALSE;
    *sync = flag(hint, "beat_sync");
    const gchar *fx = str(hint, "effect", "none");
    if (json_object_has_member(hint, "effect") && !g_str_equal(fx, "none") && !g_str_equal(fx, "lowpass")) valid = FALSE;
    node = json_object_get_member(hint, "effect");
    if (node && (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_STRING)) valid = FALSE;
    if ((*sync && (value != 1 || !ms)) || (g_str_equal(fx, "lowpass") && (!value || !ms))) valid = FALSE;
    if (!valid) return fail(error, "Rotasyon beat eşleme veya geçiş filtresi sözleşmesi geçersiz.");
    *mode = (guint)value; *overlap = (gdouble)ms / 1000;
    *effect = g_str_equal(fx, "lowpass") ? RADIOS_AUDIO_RUN_EFFECT_LOWPASS : RADIOS_AUDIO_RUN_EFFECT_NONE;
    return TRUE;
}
static gboolean calendar_routing_valid(JsonObject *row, JsonObject *prepared, RadioSPlayoutEntry *entry, GError **error)
{
    if (!json_object_has_member(row, "calendar_routing")) return TRUE;
    JsonObject *routing = object(row, "calendar_routing"); JsonArray *decks = array(routing, "decks");
    guint count = decks ? json_array_get_length(decks) : 0; gboolean seen[32] = {FALSE};
    if (!g_str_equal(str(prepared, "kind", ""), "programme") || !routing || json_object_get_size(routing) != 1 || !count || count > 32)
        return fail(error, "Rotasyonun içerik/player dağılımı geçersiz.");
    for (guint i = 0; i < count; ++i) {
        JsonNode *node = json_array_get_element(decks, i);
        gint64 deck = node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64 ? json_node_get_int(node) : 0;
        if (deck < 1 || deck > 32 || seen[deck - 1]) return fail(error, "Rotasyon player listesi 1–32 arasında benzersiz player numaraları içermeli.");
        seen[deck - 1] = TRUE; entry->routing_decks[i] = (guint)deck - 1;
    }
    entry->routing_count = count; return TRUE;
}
static gboolean calendar_rhythm_valid(const RadioSPlayoutEntry *entry)
{
    const RadioSRhythmAnalysis *r = &entry->run.rhythm;
    return entry->verified_analysis && isfinite(r->bpm) && r->bpm >= 60 && r->bpm <= 200 && isfinite(r->first_beat_ms) && r->first_beat_ms >= 0 &&
        isfinite(r->confidence) && r->confidence >= .45 && r->confidence <= 1 && r->analyzed_ms >= 4000 &&
        r->analyzed_ms <= 120000 && r->first_beat_ms < r->analyzed_ms &&
        (guint64)r->analyzed_ms * 48 <= entry->run.duration_frames + 47;
}
/* Matches the render engine's source-frame trigger, including bounded tempo
 * and beat-grid alignment. Input overlap remains unchanged so it is snapped
 * exactly once by the renderer. */
static guint64 calendar_trigger(const RadioSPlayoutEntry *entry)
{
    const RadioSAudioRunItem *run = &entry->run;
    return radios_audio_run_trigger_frame(run, &run->rhythm, run->tempo_ratio, run->media.cue_out_frame);
}
static void calendar_tempo_return(RadioSPlayoutEntry *entry)
{
    RadioSAudioRunItem *run = &entry->run;
    run->tempo_return_start_frames = run->tempo_return_frames = 0;
    if (fabs(run->tempo_ratio - 1) < .000001 || run->mode == RADIOS_AUDIO_RUN_MANUAL) return;
    /* The native-tempo boundary is the earliest possible outgoing trigger.
     * A later incoming-span cap can only move it later. Complete the return
     * before this boundary so the next fade holds one stable matched tempo. */
    RadioSAudioRunItem native = *run; native.tempo_ratio = 1;
    guint64 trigger = radios_audio_run_trigger_frame(&native, &native.rhythm, 1, native.media.cue_out_frame);
    gdouble solo_source = (trigger - run->media.cue_in_frame) - run->fade_in_frames * run->tempo_ratio;
    gdouble available = floor(2 * MAX(0.0, solo_source) / (run->tempo_ratio + 1));
    if (available < 24000) return; /* Short songs retain the match, without an abrupt return. */
    run->tempo_return_start_frames = run->fade_in_frames;
    run->tempo_return_frames = (guint64)MIN(192000.0, available);
    if (run_output_at(run, calendar_trigger(entry)) + .000001 <
            run->tempo_return_start_frames + run->tempo_return_frames)
        run->tempo_return_start_frames = run->tempo_return_frames = 0;
}
/* hold_us: the incoming entry's planned wait after a duration live step. */
static guint64 calendar_boundary(RadioSPlayoutEntry *outgoing, const RadioSAudioRunItem *incoming, gint64 hold_us, gint64 *start)
{
    RadioSAudioRunItem *run = &outgoing->run;
    run->overlap_frames = MIN(run->overlap_frames, radios_audio_run_incoming_span_frames(incoming));
    guint64 trigger = calendar_trigger(outgoing);
    gdouble elapsed = run_output_at(run, trigger), end = run_output_at(run, run->media.cue_out_frame);
    gint64 boundary = outgoing->planned_utc_us + (gint64)(ceil(elapsed) * G_USEC_PER_SEC / 48000);
    *start = MAX(boundary + hold_us, incoming->start_utc_us);
    gdouble delay = (*start - boundary) * 48000.0 / G_USEC_PER_SEC;
    guint64 remaining = (guint64)ceil(MAX(0.0, end - elapsed - delay));
    return MIN(radios_audio_run_overlap_frames(run, remaining),
        MIN(run_duration_frames(incoming), radios_audio_run_incoming_span_frames(incoming)));
}
static gboolean calendar_beatmatch_plan(RadioSPlayoutPlan *plan, const RadioSPlayoutRequest *request, GError **error)
{
    gboolean changed = FALSE;
    RadioSAudioRunItem original[RADIOS_AUDIO_RUN_MAX_ITEMS];
    gint64 times[RADIOS_AUDIO_RUN_MAX_ITEMS];
    for (guint i = 0; i < plan->entries->len; ++i) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, i);
        original[i] = entry->run; times[i] = entry->planned_utc_us;
    }
    for (guint i = 0; i + 1 < plan->entries->len; ++i) {
        RadioSPlayoutEntry *outgoing = g_ptr_array_index(plan->entries, i), *incoming = g_ptr_array_index(plan->entries, i + 1);
        RadioSAudioRunItem candidate = incoming->run; gboolean matched = FALSE;
        if (outgoing->run.beat_sync) {
            guint64 trigger = calendar_trigger(outgoing), beat = 0; gdouble ratio = 1;
            RadioSAudioTempoProfile profile = radios_audio_run_tempo_profile(&outgoing->run);
            gdouble bpm = outgoing->run.rhythm.bpm * radios_audio_tempo_profile_ratio(&profile, run_output_at(&outgoing->run, trigger));
            if (calendar_rhythm_valid(outgoing) && calendar_rhythm_valid(incoming) &&
                radios_rhythm_tempo_ratio(incoming->run.rhythm.bpm, bpm, &ratio) &&
                radios_rhythm_entry_frame(&incoming->run.rhythm, incoming->run.media.cue_in_frame,
                    incoming->run.autodj_out_frame ? incoming->run.autodj_out_frame : incoming->run.media.cue_out_frame, &beat)) {
                candidate.tempo_ratio = ratio; candidate.media.cue_in_frame = beat;
                candidate.tempo_return_start_frames = candidate.tempo_return_frames = 0;
                candidate.auto_next_frame = MAX(candidate.auto_next_frame, beat);
                if (candidate.fade_start_frame || candidate.fade_end_frame) {
                    candidate.fade_start_frame = MAX(candidate.fade_start_frame, beat);
                    candidate.fade_end_frame = MAX(candidate.fade_end_frame, candidate.fade_start_frame);
                }
                matched = candidate.overlap_frames < run_duration_frames(&candidate);
            }
            if (!matched) outgoing->run.beat_sync = FALSE;
        }
        if (!matched) candidate = incoming->run;
        gint64 start = 0; guint64 fade = calendar_boundary(outgoing, &candidate, incoming->hold_us, &start);
        /* A tempo-stretched fade past the 30 s envelope falls back to a plain
         * crossfade for this pair, then to a 30 s overlap, instead of
         * rejecting the whole block. */
        if (fade > 1440000 && matched) {
            outgoing->run.beat_sync = FALSE; matched = FALSE; candidate = incoming->run;
            fade = calendar_boundary(outgoing, &candidate, incoming->hold_us, &start);
        }
        if (fade > 1440000) {
            outgoing->run.overlap_frames = MIN(outgoing->run.overlap_frames, 1440000);
            fade = calendar_boundary(outgoing, &candidate, incoming->hold_us, &start);
        }
        if (fade > 1440000) return fail(error, "Beatmatch geçişi 30 saniye sınırını aşıyor.");
        incoming->run = candidate; incoming->run.fade_in_frames = fade; incoming->planned_utc_us = start;
        if (matched) { calendar_tempo_return(incoming); changed = TRUE; }
    }
    if (plan->entries->len) ((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, plan->entries->len - 1))->run.beat_sync = FALSE;
    if (!changed) return TRUE;
    for (guint i = 0; i < plan->entries->len; ++i) {
        RadioSPlayoutEntry *e = g_ptr_array_index(plan->entries, i);
        if (e->run.overlap_frames >= run_duration_frames(&e->run))
            return fail(error, "Beatmatch bindirmesi tempo ayarlı cue süresinden kısa olmalı.");
    }
    /* Actual profile durations and boundary tempos must still fit every
     * authenticated calendar/repeat window. Fall back to the nominal plan
     * before any engine lease if a return would violate those constraints. */
    RadioSPlayoutRequest check = *request; check.trim_calendar_end = FALSE;
    GError *constraint_error = NULL;
    if (!calendar_constraints_plan(plan, &check, &constraint_error)) {
        for (guint i = 0; i < plan->entries->len; ++i) {
            RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, i);
            entry->run = original[i]; entry->run.beat_sync = FALSE; entry->planned_utc_us = times[i];
        }
    }
    g_clear_error(&constraint_error);
    return TRUE;
}
static gboolean calendar_route_plan(RadioSPlayoutPlan *plan, gboolean allow_single, GError **error)
{
    guint n = plan->entries->len; guint32 viable[RADIOS_AUDIO_RUN_MAX_ITEMS] = {0}; gboolean retime = FALSE;
    if (!n || (n < 2 && !allow_single)) return fail(error, "AutoDJ rotasyonu en az iki doğrulanmış parça içermeli.");
    for (guint reverse = n; reverse; --reverse) {
        guint i = reverse - 1; RadioSPlayoutEntry *e = g_ptr_array_index(plan->entries, i);
        if (!e->routing_count) return fail(error, "Player dağılımı kullanıldığında rotasyondaki bütün playlistlerin AutoDJ dağılımını ayarlayın.");
        for (gboolean again = TRUE; again; ) {
            again = FALSE;
            for (guint j = 0; j < e->routing_count; ++j) {
                guint deck = e->routing_decks[j]; guint32 bit = ((guint32)1) << deck;
                if (i + 1 == n || (viable[i + 1] && (!e->run.overlap_frames || (viable[i + 1] & ~bit)))) viable[i] |= bit;
            }
            /* Two consecutive items that can only share one physical player
             * cannot overlap: that single transition becomes a plain cut. */
            if (!viable[i] && e->run.overlap_frames && i + 1 < n && viable[i + 1]) {
                e->run.overlap_frames = 0; e->run.beat_sync = FALSE; e->run.effect = RADIOS_AUDIO_RUN_EFFECT_NONE; e->run.shape.enabled = FALSE;
                if (e->run.mode == RADIOS_AUDIO_RUN_CROSSFADE) e->run.mode = RADIOS_AUDIO_RUN_EOF;
                diagnose(plan, "routing_overlap_dropped", e->run.entry_id); retime = again = TRUE;
            }
        }
        if (!viable[i]) return fail(error, "Bindirmeli geçiş için ardışık içeriklerin player havuzlarında farklı fiziksel player seçilebilmeli.");
    }
    for (guint i = 1; retime && i < n; ++i) {
        RadioSPlayoutEntry *previous = g_ptr_array_index(plan->entries, i - 1), *entry = g_ptr_array_index(plan->entries, i);
        entry->run.fade_in_frames = calendar_boundary(previous, &entry->run, entry->hold_us, &entry->planned_utc_us);
    }
    GHashTable *cursors = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    gboolean used[32] = {FALSE}; plan->options.pool_count = 0;
    for (guint i = 0; i < n; ++i) {
        RadioSPlayoutEntry *e = g_ptr_array_index(plan->entries, i); GString *key = g_string_new(NULL);
        for (guint j = 0; j < e->routing_count; ++j) g_string_append_printf(key, "%u,", e->routing_decks[j]);
        guint cursor = GPOINTER_TO_UINT(g_hash_table_lookup(cursors, key->str)), chosen = G_MAXUINT;
        RadioSPlayoutEntry *previous = i ? g_ptr_array_index(plan->entries, i - 1) : NULL;
        for (guint j = 0; j < e->routing_count; ++j) {
            guint position = (cursor + j) % e->routing_count, deck = e->routing_decks[position];
            if (!(viable[i] & (((guint32)1) << deck)) || (previous && previous->run.overlap_frames && previous->run.preferred_deck == (gint)deck)) continue;
            chosen = deck; g_hash_table_replace(cursors, g_strdup(key->str), GUINT_TO_POINTER((position + 1) % e->routing_count)); break;
        }
        g_string_free(key, TRUE);
        if (chosen == G_MAXUINT) { g_hash_table_unref(cursors); return fail(error, "Rotasyon için çakışmasız player dağılımı bulunamadı."); }
        e->run.preferred_deck = (gint)chosen;
        if (!used[chosen]) { used[chosen] = TRUE; plan->options.deck_pool[plan->options.pool_count++] = chosen; }
    }
    g_hash_table_unref(cursors); plan->player_autodj = plan->routed_rotation = TRUE; return TRUE;
}

static gboolean sequential_plan(RadioSPlayoutPlan *p, JsonObject *prepared, const RadioSPlayoutRequest *request, GError **error)
{
    JsonArray *tracks = array(prepared, "tracks"); JsonObject *config = object(prepared, "config"), *timing = object(config, "timing");
    guint n = tracks ? json_array_get_length(tracks) : 0;
    if (!n || n > RADIOS_AUDIO_RUN_MAX_ITEMS || request->start_index >= n) return fail(error, "Çalınabilir sıra boş veya başlangıç seçimi geçersiz.");
    gint64 utc = request->start_utc_us ? request->start_utc_us : request->now_utc_us;
    if (timing && !request->start_utc_us) {
        gint64 begin = date_time(str(config, "date", ""), str(timing, "start_time", ""));
        if (!begin) return fail(error, "Program başlangıç takvimi geçersiz.");
        utc = MAX(utc, begin);
    }
    p->options.start_utc_us = utc > request->now_utc_us ? utc : 0;
    for (guint i = request->start_index; i < n; ++i) {
        JsonObject *row = json_array_get_object_element(tracks, i), *m = object(row, "metadata");
        RadioSAudioCopyrightWindow copyright = row_copyright(row, request->copyright_policy);
        const gchar *scheduled_time = str(m, "start_time", "");
        if (timing && g_str_equal(str(timing, "event_id", ""), str(row, "entry_id", ""))) scheduled_time = str(timing, "target_time", "");
        gint64 check_at = *scheduled_time ? MAX(utc, date_time(str(config, "date", ""), scheduled_time)) : utc;
        if (!radios_copyright_window_allowed(&copyright, check_at)) {
            diagnose(p, "copyright_outside_validity", str(row, "entry_id", "")); continue;
        }
        guint mode = request->mode; gdouble overlap = request->overlap_seconds;
        JsonObject *hint = NULL; gboolean sync = request->has_default_policy && request->default_beat_sync && mode == RADIOS_AUDIO_RUN_CROSSFADE;  RadioSAudioRunEffect effect = RADIOS_AUDIO_RUN_EFFECT_NONE;
        RadioSAudioTransitionShape shape = {0}; shape.effect = -1;
        if (row && json_object_has_member(row, "calendar_transition")) {
            hint = object(row, "calendar_transition");
            if (!g_str_equal(str(prepared, "kind", ""), "programme"))
                return fail(error, "Rotasyon geçiş ayarları yalnız doğrulanmış program sırasında kullanılabilir.");
            if (!calendar_transition_valid(hint, &mode, &overlap, &sync, &effect, &shape, error)) return FALSE;
            if (!request->single_player && (mode == RADIOS_AUDIO_RUN_CROSSFADE || mode == RADIOS_AUDIO_RUN_CUE)) {
                p->options.pool_count = 2; p->options.deck_pool[0] = 0; p->options.deck_pool[1] = 1;
            }
        }
        if (request->single_player) {
            mode = RADIOS_AUDIO_RUN_EOF; overlap = 0; sync = FALSE; effect = RADIOS_AUDIO_RUN_EFFECT_NONE; shape.enabled = FALSE;
        }
        RadioSPlayoutEntry *e = entry_from_row(row, request, str(row, "calendar_source_id", ""),
            str(row, "calendar_source_name", str(config, "title", str(prepared, "source_name", "Playlist"))), mode, overlap, error);
        if (!e) return FALSE;
        if (*str(row, "calendar_playlist_id", ""))
            json_object_set_string_member(e->binding, "playlist_id", str(row, "calendar_playlist_id", ""));
        /* Phase two reattaches the verified schema flow to these tracks.
         * Keep it in the accepted entry/display so queue projection can
         * identify the live step and the programme's actual entry choice. */
        const gchar *flow[] = {"live_step", "programme_entry", "schema_boundary"};
        for (guint j = 0; j < G_N_ELEMENTS(flow); ++j) {
            if (!json_object_has_member(row, flow[j])) continue;
            JsonObject *value = object(row, flow[j]);
            if (!value) { entry_free(e); return fail(error, "Programın şema akış bilgisi geçersiz."); }
            json_object_set_object_member(e->binding, flow[j], copy_object(value));
        }
        e->explicit_transition = hint != NULL;
        e->run.beat_sync = sync && (hint || g_str_equal(e->kind, "music") || g_str_equal(e->kind, "other")); e->run.effect = effect;
        e->run.shape = shape;
        if (shape.enabled) json_object_set_string_member(e->binding, "transition_preset", str(object(hint, "shape"), "name", ""));
        if (!calendar_routing_valid(row, prepared, e, error) || !calendar_constraints_valid(row, e, error)) { entry_free(e); return FALSE; }
        const gchar *time = str(m, "start_time", ""), *policy = str(m, "start_policy", "sequential");
        gint64 wait_ms = integer(m, "max_wait_ms", 0);
        if (timing && g_str_equal(str(timing, "event_id", ""), e->run.entry_id)) {
            time = str(timing, "target_time", ""); policy = str(timing, "policy", "cut"); wait_ms = integer(timing, "max_wait_ms", 0);
        }
        if (*time) {
            e->run.start_utc_us = date_time(str(config, "date", ""), time);
            if (!e->run.start_utc_us) { entry_free(e); return fail(error, "Olayın UTC takvim tarihi/saati geçersiz."); }
            e->run.start_policy = g_str_equal(policy, "finish") ? RADIOS_AUDIO_RUN_START_FINISH :
                g_str_equal(policy, "cut") ? RADIOS_AUDIO_RUN_START_CUT : RADIOS_AUDIO_RUN_START_SEQUENTIAL;
            e->run.max_wait_frames = (guint64)MAX(wait_ms, 0) * 48;
        }
        /* Kılavuz §04/§07 (PDF s.15): a live step follows the row that carries
         * its live_step hint. That row ends plainly into the step (native
         * MANUAL), and the step's end rule moves on: manual waits for the
         * operator, duration counts from the row's actual end (the controller
         * continues; playout_ui.c live_wait_poll), so pausing or a late end
         * never shortens the step. The wait is part of the planned timeline,
         * not an absolute anchor. */
        JsonObject *own = json_object_has_member(row, "live_step") ? object(row, "live_step") : NULL;
        if (own && (g_str_equal(str(own, "end_rule", ""), "manual") || g_str_equal(str(own, "end_rule", ""), "duration"))) {
            e->run.mode = RADIOS_AUDIO_RUN_MANUAL; e->run.overlap_frames = 0; e->run.beat_sync = FALSE;
            e->run.effect = RADIOS_AUDIO_RUN_EFFECT_NONE; e->run.shape.enabled = FALSE;
        }
        JsonObject *before = i ? json_array_get_object_element(tracks, i - 1) : NULL;
        JsonObject *step = before && json_object_has_member(before, "live_step") ? object(before, "live_step") : NULL;
        if (step && g_str_equal(str(step, "end_rule", ""), "duration"))
            e->hold_us = MAX(integer(step, "duration_ms", 0), G_GINT64_CONSTANT(0)) * 1000;
        /* The calendar selection waited for this row's playlist or song window
         * (calendar_window_wait): it starts at that window, never at the previous
         * row's end. Any other row before its window still fails the bounds. */
        if (json_object_has_member(row, "calendar_window_wait")) {
            if (!flag(row, "calendar_window_wait") || !e->calendar_constraints) { entry_free(e); return fail(error, "Rotasyon pencere bekleme işareti geçersiz."); }
            e->run.start_utc_us = MAX(e->run.start_utc_us, integer(e->calendar_constraints, "earliest_utc_us", 0));
        }
        add_entry(p, e, &utc);
    }
    return calendar_beatmatch_plan(p, request, error);
}
static gboolean player_autodj_plan(RadioSPlayoutPlan *plan, JsonObject *prepared, const RadioSPlayoutRequest *request, GError **error)
{
    JsonObject *config = object(prepared, "config"); JsonArray *players = array(config, "autodj_players");
    gboolean routed = FALSE;
    for (guint i = 0; i < plan->entries->len; ++i)
        routed |= ((RadioSPlayoutEntry *)g_ptr_array_index(plan->entries, i))->routing_count != 0;
    if (routed) return calendar_route_plan(plan, request->allow_single_routed, error);
    if (!players) return TRUE;
    if (json_array_get_length(players) != 2) return fail(error, "AutoDJ için iki farklı deck gerekli.");
    gint64 source = json_array_get_int_element(players, 0), target = json_array_get_int_element(players, 1);
    if (source < 1 || source > 32 || target < 1 || target > 32 || source == target) return fail(error, "AutoDJ oynatıcı çifti geçersiz.");
    if (plan->entries->len < 2 && (!plan->copyright_policy || !flag(plan->copyright_policy, "restrict")))
        return fail(error, "AutoDJ için en az iki parça gerekli.");
    plan->player_autodj = TRUE; plan->options.pool_count = 2;
    plan->options.deck_pool[0] = (guint)source - 1; plan->options.deck_pool[1] = (guint)target - 1;
    gboolean sync = request->has_default_policy ? request->default_beat_sync : json_object_has_member(config, "beat_sync") ? flag(config, "beat_sync") : TRUE;
    for (guint index = 0; index < plan->entries->len; ++index) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, index);
        entry->run.preferred_deck = (gint)plan->options.deck_pool[index % 2];
        if (sync && entry->run.mode == RADIOS_AUDIO_RUN_CROSSFADE && !entry->explicit_transition) entry->run.beat_sync = TRUE;
    }
    return !sync || calendar_beatmatch_plan(plan, request, error);
}

/* Immutable authenticated rows are the policy authority. Calendar selection
 * checkpoints are carried separately: importing them here would rewind the
 * transition/jingle RNG each time another preselected occurrence is visited. */
static JsonObject *probability_row(JsonObject *prepared, const RadioSPlayoutEntry *entry)
{
    JsonArray *tracks = array(prepared, "tracks"), *sources = array(prepared,"sources");
    for (guint s=0;s<= (sources ? json_array_get_length(sources):0);++s) {
        if (s) tracks=array(json_array_get_object_element(sources,s-1),"tracks");
        for (guint i=0;tracks && i<json_array_get_length(tracks);++i) {
            JsonObject *row=json_array_get_object_element(tracks,i);
            if (g_str_equal(str(row,"entry_id",""),entry->run.entry_id)) return row;
        }
    }
    return NULL;
}
static JsonObject *probability_effective(JsonObject *prepared, JsonObject *row, const RadioSPlayoutEntry *entry, GError **error)
{
    JsonObject *policy=object(row,"probability_policy"), *context=object(row,"policy_context"), *config=object(prepared,"config");
    if (!context) context=object(entry->binding,"policy_context");
    if ((row && json_object_has_member(row,"probability_policy") && !policy) ||
        (!policy && config && json_object_has_member(config,"probability_policy") && !object(config,"probability_policy"))) {
        fail(error,"Olasılık kuralı nesnesi geçersiz."); return NULL;
    }
    if (!policy) { policy=object(config,"probability_policy"); if (!context) context=object(config,"policy_context"); }
    if (!policy) return NULL;
    if (!radios_automation_policy_validate(policy,error)) return NULL;
    JsonObject *effective=radios_automation_policy_effective(policy,str(context,"automation_id",""),
        str(context,"playlist_id",str(row,"calendar_playlist_id","")),str(context,"entry_id",entry->run.entry_id));
    if (effective && context) json_object_set_object_member(entry->binding,"policy_context",copy_object(context));
    return effective;
}
/* Derive every recalculation from immutable media markers, so a second
 * tempo/finalization pass cannot subtract the offset twice or extend an OUT. */
static guint64 probability_exit_reference(const RadioSPlayoutEntry *entry)
{
    guint64 in = entry->run.media.cue_in_frame, out = entry->run.media.cue_out_frame;
    gint64 marked = integer(entry->verified_markers, "autodj_out_ms", -1);
    if (marked < 0 && flag(entry->verified_markers, "automatic"))
        marked = integer(entry->verified_markers, "auto_next_ms", -1);
    return marked >= 0 ? CLAMP((guint64)marked * 48, in, out) : out;
}
static gboolean probability_early_frame(const RadioSPlayoutEntry *entry, gdouble seconds, guint64 *frame)
{
    RadioSAudioTempoProfile profile = radios_audio_run_tempo_profile(&entry->run);
    guint64 in = entry->run.media.cue_in_frame, reference = probability_exit_reference(entry);
    gdouble end = radios_audio_tempo_profile_output(&profile, reference - in);
    if (seconds * 48000 >= end) return FALSE;
    *frame = in + (guint64)ceil(radios_audio_tempo_profile_source(&profile, end - seconds * 48000));
    return *frame > in && *frame <= reference;
}
static void probability_early(RadioSPlayoutEntry *entry)
{
    gdouble seconds = number(entry->binding, "probability_early_seconds", 0); guint64 frame;
    if (seconds > 0 && entry->run.mode != RADIOS_AUDIO_RUN_MANUAL && probability_early_frame(entry, seconds, &frame))
        entry->run.autodj_out_frame = frame;
}
static gboolean probability_transition_eligible(const RadioSPlayoutEntry *entry,
    const RadioSPlayoutEntry *next, JsonObject *rule, gboolean single_player)
{
    RadioSPlayoutEntry candidate = *entry;
    guint mode = (guint)integer(rule, "mode", 0);
    transition(&candidate, single_player ? RADIOS_AUDIO_RUN_EOF : mode == 3 ? RADIOS_AUDIO_RUN_CROSSFADE : mode,
        single_player ? 0 : number(rule, "overlap_ms", 0) / 1000);
    gdouble early = number(rule, "early_seconds", 0); guint64 frame;
    if (early > 0) {
        if (!probability_early_frame(&candidate, early, &frame)) return FALSE;
        candidate.run.autodj_out_frame = frame;
    }
    if (!candidate.run.overlap_frames) return TRUE;
    if (!next || !(g_str_equal(next->kind, "music") || g_str_equal(next->kind, "other"))) return FALSE;
    guint64 exit = candidate.run.autodj_out_frame ? candidate.run.autodj_out_frame : candidate.run.media.cue_out_frame;
    return candidate.run.overlap_frames < run_output_at(&candidate.run, exit) &&
        candidate.run.overlap_frames < MIN(run_duration_frames(&next->run), radios_audio_run_incoming_span_frames(&next->run));
}
/* A fade marker is not a certified vocal-free outro. Only an approved radio
 * outro cue can authorize the ending window; native intro marks retain their
 * established vocal-start meaning. Unknown windows simply omit the overlay. */
static gboolean probability_jingle_anchor(const RadioSPlayoutEntry *entry, JsonObject *rule,
    guint64 voice_span, guint64 *output_frame)
{
    guint64 in = entry->run.media.cue_in_frame, out = entry->run.media.cue_out_frame;
    guint64 exit = entry->run.autodj_out_frame ? entry->run.autodj_out_frame :
        entry->run.mode == RADIOS_AUDIO_RUN_CUE ? entry->run.auto_next_frame : out;
    gdouble end = run_output_at(&entry->run, exit), begin = 0, limit = end;
    gboolean before_end = g_str_equal(str(rule, "anchor", "after_start"), "before_end"), known = FALSE;
    if (!before_end) {
        gint64 intro = integer(entry->verified_markers, "intro_ms", 0);
        if (intro > 0 && (guint64)intro * 48 > in && (guint64)intro * 48 <= out) {
            limit = MIN(limit, run_output_at(&entry->run, (guint64)intro * 48)); known = TRUE;
        }
    }
    JsonArray *cues = array(entry->binding, "radio_cues");
    for (guint i = 0; cues && i < json_array_get_length(cues); ++i) {
        JsonObject *cue = json_array_get_object_element(cues, i); const gchar *kind = str(cue, "cue_kind", "");
        guint64 frame = (guint64)integer(cue, "position_ms", 0) * 48;
        if (before_end && g_str_equal(kind, "outro_start")) {
            begin = MAX(begin, run_output_at(&entry->run, frame)); known = TRUE;
        } else if (!before_end && (g_str_equal(kind, "intro_end") || g_str_equal(kind, "vocal_start"))) {
            limit = MIN(limit, run_output_at(&entry->run, frame)); known = TRUE;
        }
    }
    gdouble start = before_end ? end - number(rule, "seconds", 0) * 48000 : number(rule, "seconds", 0) * 48000;
    if (!known || start < begin || start < 0 || start >= limit || ceil(start) + voice_span > limit) return FALSE;
    *output_frame = (guint64)ceil(start); return TRUE;
}
static JsonArray *probability_jingle_tracks(JsonObject *prepared, JsonObject *rule)
{
    JsonArray *catalog = array(prepared, "jingle_sources");
    for (guint i = 0; catalog && i < json_array_get_length(catalog); ++i) {
        JsonObject *source = json_array_get_object_element(catalog, i);
        if (g_str_equal(str(source, "playlist_id", ""), str(rule, "playlist_id", ""))) return array(source, "tracks");
    }
    return NULL;
}
static gboolean probability_jingle_candidate(const RadioSPlayoutEntry *entry, JsonObject *rule,
    JsonObject *candidate, const RadioSPlayoutRequest *request)
{
    JsonObject *detail = object(candidate, "detail");
    RadioSMediaGeometry geometry = radios_media_geometry(detail, integer(object(detail, "version"), "duration_ms", 0), TRUE);
    guint64 start;
    if (!media_eligible(candidate) || !g_uuid_string_is_valid(str(candidate, "entry_id", "")) ||
        !geometry.autodj_valid || geometry.autodj_in >= geometry.cue_out ||
        !probability_jingle_anchor(entry, rule, geometry.cue_out - geometry.autodj_in, &start)) return FALSE;
    RadioSAudioCopyrightWindow rights = row_copyright(candidate, request->copyright_policy);
    gint64 when = entry->planned_utc_us + (gint64)(start * 1000000 / 48000);
    return radios_copyright_window_allowed(&rights, when) && radios_copyright_window_allowed(&rights,
        when + (gint64)((geometry.cue_out - geometry.autodj_in) * 1000000 / 48000) - 1);
}
static gboolean probability_plan(RadioSPlayoutPlan *plan, JsonObject *prepared, const RadioSPlayoutRequest *request, GError **error)
{
    RadioSAutomationPolicyState *state=NULL;
    for (guint i=0;i<plan->entries->len;++i) {
        RadioSPlayoutEntry *entry=g_ptr_array_index(plan->entries,i); JsonObject *row=probability_row(prepared,entry);
        /* A preceding probabilistic exit has already changed the real start.
         * Time exclusions, intervals and jingle rights must use that boundary. */
        if (i && flag(json_node_get_object(plan->display),"probability_applied")) {
            RadioSPlayoutEntry *previous=g_ptr_array_index(plan->entries,i-1);
            entry->run.fade_in_frames=calendar_boundary(previous,&entry->run,entry->hold_us,&entry->planned_utc_us);
        }
        JsonObject *effective=probability_effective(prepared,row,entry,error);
        if (error && *error) { if (state) radios_automation_policy_state_free(state); return FALSE; }
        JsonObject *selection=object(row,"policy_selection_checkpoint");
        if (selection) json_object_set_object_member(entry->binding,"policy_selection_checkpoint",copy_object(selection));
        if (!effective) continue;
        if (!g_str_equal(entry->kind,"music") && !g_str_equal(entry->kind,"other")) { json_object_unref(effective); continue; }
        json_object_set_boolean_member(json_node_get_object(plan->display),"probability_applied",TRUE);
        if (!state) {
            state=radios_automation_policy_state_new(str(effective,"seed",""),object(prepared,"probability_checkpoint"),error);
            if (!state) { json_object_unref(effective); return FALSE; }
        }
        JsonObject *context=object(entry->binding,"policy_context");
        const gchar *aid=str(context,"automation_id",entry->source_id);
        JsonArray *rules=array(effective,"transitions");
        if (rules) {
            const gchar *first=json_array_get_length(rules)?str(json_array_get_object_element(rules,0),"id",""):"disabled";
            gchar *key=g_strdup_printf("%s/transitions/%s",aid,first);
            gboolean eligible[RADIOS_AUTOMATION_POLICY_MAX_CHOICES] = {FALSE};
            RadioSPlayoutEntry *next = i + 1 < plan->entries->len ? g_ptr_array_index(plan->entries, i + 1) : NULL;
            for (guint r = 0; r < json_array_get_length(rules); ++r)
                eligible[r] = probability_transition_eligible(entry, next, json_array_get_object_element(rules, r), request->single_player);
            gint selected=radios_automation_policy_choose(rules,eligible,key,state,entry->planned_utc_us); g_free(key);
            /* An empty draw (no rule, or an unrepresented remainder) keeps the
             * planned rotation/kind transition instead of forcing a hard EOF. */
            if (selected>=0) {
                transition(entry,RADIOS_AUDIO_RUN_EOF,0); entry->run.beat_sync=FALSE; entry->run.effect=RADIOS_AUDIO_RUN_EFFECT_NONE; entry->run.shape.enabled=FALSE;
                entry->explicit_transition=TRUE;
                JsonObject *rule=json_array_get_object_element(rules,(guint)selected); guint mode=(guint)integer(rule,"mode",0);
                transition(entry,request->single_player?RADIOS_AUDIO_RUN_EOF:mode==3?RADIOS_AUDIO_RUN_CROSSFADE:mode,request->single_player?0:number(rule,"overlap_ms",0)/1000);
                entry->run.beat_sync=!request->single_player && mode==3; entry->run.effect=!request->single_player && g_str_equal(str(rule,"effect","none"),"lowpass")?RADIOS_AUDIO_RUN_EFFECT_LOWPASS:RADIOS_AUDIO_RUN_EFFECT_NONE;
                gdouble early=number(rule,"early_seconds",0); json_object_set_double_member(entry->binding,"probability_early_seconds",early);
                probability_early(entry);
                if (early*48000>=run_duration_frames(&entry->run) && early>0) diagnose(plan,"probability_early_outside_song",entry->run.entry_id);
            } else if (selected<0 && json_array_get_length(rules)) diagnose(plan,"probability_transition_no_eligible_remainder",entry->run.entry_id);
        }
        rules=array(effective,"jingles");
        if (rules && json_array_get_length(rules)) {
            gchar *key=g_strdup_printf("%s/jingles/%s",aid,str(json_array_get_object_element(rules,0),"id",""));
            gboolean allowed[RADIOS_AUTOMATION_POLICY_MAX_CHOICES] = {FALSE};
            for (guint r = 0; r < json_array_get_length(rules); ++r) {
                JsonObject *rule = json_array_get_object_element(rules, r); JsonArray *tracks = probability_jingle_tracks(prepared, rule);
                guint player = (guint)integer(rule, "player", 32); gboolean distinct = player < 32;
                for (guint d = 0; d < plan->options.pool_count; ++d) if (plan->options.deck_pool[d] == player) distinct = FALSE;
                for (guint t = 0; distinct && tracks && t < json_array_get_length(tracks); ++t)
                    allowed[r] |= probability_jingle_candidate(entry, rule, json_array_get_object_element(tracks, t), request);
            }
            gint selected=radios_automation_policy_choose(rules,allowed,key,state,entry->planned_utc_us); g_free(key);
            if (selected>=0) {
                JsonObject *rule=json_array_get_object_element(rules,(guint)selected); JsonArray *tracks=probability_jingle_tracks(prepared,rule);
                const gchar *playlist=str(rule,"playlist_id","");
                const gchar *choice_ids[500]; gboolean eligible[500]={FALSE}; guint n=tracks?MIN(json_array_get_length(tracks),500u):0;
                for (guint j=0;j<n;++j) {
                    JsonObject *candidate=json_array_get_object_element(tracks,j);
                    choice_ids[j]=str(candidate,"entry_id","");
                    eligible[j]=probability_jingle_candidate(entry,rule,candidate,request);
                }
                key=g_strdup_printf("%s/jingle-catalog/%s",aid,playlist);
                gint pick=radios_automation_policy_choose_targets(NULL,"track",choice_ids,eligible,n,key,state,entry->planned_utc_us); g_free(key);
                RadioSPlayoutEntry *voice=NULL;
                if (pick>=0) voice=entry_from_row(json_array_get_object_element(tracks,(guint)pick),request,playlist,"Jingle",RADIOS_AUDIO_RUN_EOF,0,error);
                if (error && *error) { json_object_unref(effective); radios_automation_policy_state_free(state); return FALSE; }
                if (voice) {
                    guint64 start = 0;
                    guint player=(guint)integer(rule,"player",32); gboolean distinct=player<32;
                    for (guint d=0;d<plan->options.pool_count;++d) if (plan->options.deck_pool[d]==player) distinct=FALSE;
                    guint64 voice_span=voice->run.media.cue_out_frame-voice->run.media.cue_in_frame;
                    if (distinct && probability_jingle_anchor(entry,rule,voice_span,&start)) {
                        entry->run.adjunct=(RadioSAudioRunAdjunct){.enabled=TRUE,.player=player,.media=voice->run.media,
                            .entry_id=voice->run.entry_id,.title=voice->title,.artist=voice->artist,.start_output_frame=(guint64)ceil(start),
                            .fade_frames=(guint64)integer(rule,"fade_ms",100)*48,.gain_db=number(rule,"gain_db",0),.duck_db=number(rule,"duck_db",-12),
                            .copyright=voice->run.copyright,.loudness=voice->run.loudness};
                        voice->run.media=(RadioSAudioLoadSpec){0}; voice->run.entry_id=NULL; voice->title=voice->artist=NULL;
                        json_object_set_object_member(entry->binding,"jingle_rule",copy_object(rule));
                        json_object_set_string_member(entry->binding,"adjunct_kind",voice->kind ? voice->kind : "jingle");
                        json_object_set_object_member(entry->binding,"adjunct_copyright_metadata",copy_object(voice->copyright_metadata));
                    } else diagnose(plan,distinct?"midtrack_jingle_does_not_fit":"midtrack_jingle_player_is_music",entry->run.entry_id);
                    entry_free(voice);
                } else diagnose(plan,"midtrack_jingle_no_verified_source",entry->run.entry_id);
            } else diagnose(plan,"probability_jingle_no_eligible_safe_window",entry->run.entry_id);
        }
        json_object_set_object_member(entry->binding,"probability_policy",effective);
        json_object_set_object_member(entry->binding,"probability_checkpoint",radios_automation_policy_state_snapshot(state));
    }
    if (state) radios_automation_policy_state_free(state);
    return TRUE;
}
/* Beatmatching may move the cue entrance and tempo profile. Recompute output
 * anchors after that immutable plan adjustment, before any decoder is leased. */
static gboolean probability_finalize(RadioSPlayoutPlan *plan, GError **error)
{
    for (guint i=0;i<plan->entries->len;++i) {
        RadioSPlayoutEntry *entry=g_ptr_array_index(plan->entries,i); probability_early(entry);
        JsonObject *rule=object(entry->binding,"jingle_rule");
        if (entry->run.adjunct.enabled && rule) {
            guint64 start;
            if (!probability_jingle_anchor(entry,rule,entry->run.adjunct.media.cue_out_frame-entry->run.adjunct.media.cue_in_frame,&start)) {
                entry->run.adjunct.enabled=FALSE;
                diagnose(plan,"midtrack_jingle_outside_verified_vocal_window",entry->run.entry_id);
            } else entry->run.adjunct.start_output_frame=start;
        }
    }
    for (guint i=0;i<plan->entries->len;++i) {
        RadioSPlayoutEntry *entry=g_ptr_array_index(plan->entries,i); RadioSAudioRunAdjunct *voice=&entry->run.adjunct;
        if (!voice->enabled) continue;
        for (guint d=0;d<plan->options.pool_count;++d) if (plan->options.deck_pool[d]==voice->player)
            return fail(error,"Jingle oynatıcısı müzik oynatıcı havuzundan farklı olmalı.");
        gint64 start=entry->planned_utc_us+(gint64)(voice->start_output_frame*1000000/48000);
        gint64 end=start+(gint64)((voice->media.cue_out_frame-voice->media.cue_in_frame)*1000000/48000);
        if (i+1<plan->entries->len) {
            RadioSPlayoutEntry *next=g_ptr_array_index(plan->entries,i+1);
            gint64 retirement=next->planned_utc_us+(gint64)(entry->run.overlap_frames*1000000/48000);
            if (end>retirement) { voice->enabled=FALSE; diagnose(plan,"midtrack_jingle_crosses_primary_retirement",entry->run.entry_id); continue; }
        }
        for (guint j=0;j<i;++j) {
            RadioSPlayoutEntry *previous=g_ptr_array_index(plan->entries,j); RadioSAudioRunAdjunct *prior=&previous->run.adjunct;
            if (!prior->enabled || prior->player!=voice->player) continue;
            gint64 prior_start=previous->planned_utc_us+(gint64)(prior->start_output_frame*1000000/48000);
            gint64 prior_end=prior_start+(gint64)((prior->media.cue_out_frame-prior->media.cue_in_frame)*1000000/48000);
            if (start<prior_end && end>prior_start) return fail(error,"Bindirmeli iki jingle aynı oynatıcıyı kullanamaz; farklı jingle oynatıcıları seçin.");
        }
    }
    return TRUE;
}
static void plan_automation_block_metadata(RadioSPlayoutPlan *plan);
/* Display the accepted adjunct/next jingle selection without inserting an
 * extra audio item. Disabled or rejected probability rules leave no hint. */
void radios_playout_plan_refresh_jingle_metadata(RadioSPlayoutPlan *plan)
{
    /* A refill is a planning batch, not a new event boundary. Reuse accepted
     * music identities while joining consecutive source playlists. */
    if (plan->automation) plan_automation_block_metadata(plan);
    JsonArray *rows = array(json_node_get_object(plan->display), "entries");
    for (guint i = 0; rows && i < MIN(plan->entries->len, json_array_get_length(rows)); ++i) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, i);
        JsonObject *row = json_array_get_object_element(rows, i);
        json_object_remove_member(row, "planned_jingles");
        if (entry->jingle || (!g_str_equal(entry->kind, "music") && !g_str_equal(entry->kind, "other"))) continue;
        JsonArray *hints = json_array_new();
        RadioSAudioRunAdjunct *voice = &entry->run.adjunct;
        if (voice->enabled) {
            JsonObject *hint = json_object_new(), *rule = object(entry->binding, "jingle_rule");
            json_object_set_string_member(hint, "title", voice->title ? voice->title : "Jingle");
            json_object_set_string_member(hint, "artist", voice->artist ? voice->artist : "");
            json_object_set_string_member(hint, "placement", "overlay");
            json_object_set_string_member(hint, "entry_id", voice->entry_id);
            JsonObject *parent = json_object_new(); json_object_set_string_member(parent, "id", entry->run.entry_id);
            gchar *identity = radios_programme_hint_id(parent, hint, 0);
            json_object_set_string_member(hint, "id", identity); g_free(identity); json_object_unref(parent);
            json_object_set_string_member(hint, "kind", str(entry->binding, "adjunct_kind", "jingle"));
            json_object_set_int_member(hint, "player", voice->player + 1);
            json_object_set_string_member(hint, "anchor", str(rule, "anchor", "after_start"));
            json_object_set_double_member(hint, "seconds", number(rule, "seconds", 0));
            json_object_set_int_member(hint, "start_ms", (gint64)(voice->start_output_frame / 48));
            json_object_set_int_member(hint, "duration_ms", (gint64)((voice->media.cue_out_frame - voice->media.cue_in_frame) / 48));
            json_array_add_object_element(hints, hint);
        }
        for (guint j = i + 1; j < plan->entries->len; ++j) {
            RadioSPlayoutEntry *next = g_ptr_array_index(plan->entries, j);
            const gchar *kind = next->kind ? next->kind : "";
            if (!next->jingle && !g_str_equal(kind, "jingle") && !g_str_equal(kind, "sweeper") &&
                !g_str_equal(kind, "announcement") && !g_str_equal(kind, "advertisement") &&
                !g_str_equal(kind, "effect") && !g_str_equal(kind, "voice_track")) break;
            JsonObject *hint = json_object_new();
            json_object_set_string_member(hint, "title", next->title ? next->title : "Jingle");
            json_object_set_string_member(hint, "artist", next->artist ? next->artist : "");
            json_object_set_string_member(hint, "placement", "after");
            json_object_set_string_member(hint, "entry_id", next->run.entry_id);
            json_object_set_string_member(hint, "id", next->run.entry_id);
            json_object_set_string_member(hint, "kind", *kind ? kind : "jingle");
            gint deck = next->run.preferred_deck >= 0 ? next->run.preferred_deck :
                (gint)plan->options.deck_pool[j % plan->options.pool_count];
            json_object_set_int_member(hint, "player", deck + 1);
            json_object_set_string_member(hint, "anchor", "after_end");
            gint64 start_ms = MAX(G_GINT64_CONSTANT(0), (next->planned_utc_us - entry->planned_utc_us) / 1000);
            json_object_set_double_member(hint, "seconds", (gdouble)start_ms / 1000);
            json_object_set_int_member(hint, "start_ms", start_ms);
            json_object_set_int_member(hint, "duration_ms", (gint64)((next->run.media.cue_out_frame - next->run.media.cue_in_frame) / 48));
            json_array_add_object_element(hints, hint);
        }
        if (json_array_get_length(hints)) json_object_set_array_member(row, "planned_jingles", hints);
        else json_array_unref(hints);
    }
}
static void plan_automation_block_metadata(RadioSPlayoutPlan *plan)
{
    JsonArray *rows = array(json_node_get_object(plan->display), "entries");
    gchar *block_id = NULL; const gchar *previous_source = "", *previous_kind = "";
    for (guint i = 0; rows && i < plan->entries->len; ++i) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, i);
        const gchar *kind = entry->jingle ? "jingle" : entry->kind;
        JsonObject *row = json_array_get_object_element(rows, i);
        if (!block_id || g_strcmp0(previous_kind, kind) ||
            (!g_str_equal(kind, "music") && g_strcmp0(previous_source, entry->source_id))) {
            const gchar *accepted_id = str(row, "block_id", "");
            gchar *keep = g_uuid_string_is_valid(accepted_id) && g_strcmp0(accepted_id, block_id) ? g_strdup(accepted_id) : NULL;
            g_free(block_id);
            if (keep) block_id = keep;
            else {
                gchar *occurrence = g_strdup_printf("%s:%s:%" G_GINT64_FORMAT ":%u",
                    entry->source_id, entry->run.entry_id, entry->planned_utc_us, i);
                block_id = radios_programme_block_id_for(occurrence); g_free(occurrence);
            }
            previous_source = entry->source_id; previous_kind = kind;
        }
        json_object_set_string_member(row, "block_id", block_id);
        json_object_set_string_member(row, "block_kind", kind);
        json_object_set_string_member(row, "block_title", !g_str_equal(kind, "music") && entry->source_name && *entry->source_name ?
            entry->source_name : radios_programme_event_title(kind));
        /* Plain library playlists still keep their playlist/header identity;
         * grouping their contents does not invent an explicit saved event. */
        json_object_set_boolean_member(row, "block_explicit", plan->automation);
    }
    g_free(block_id);
}
static void plan_display(RadioSPlayoutPlan *p)
{
    JsonObject *display = json_node_get_object(p->display); JsonArray *rows = json_array_new();
    json_object_set_string_member(display, "kind", p->automation ? "automation" : "sequence");
    json_object_set_string_member(display, "proof", "planning_only");
    for (guint i = 0; i < p->entries->len; ++i) {
        RadioSPlayoutEntry *e = g_ptr_array_index(p->entries, i); JsonObject *row = copy_object(e->binding);
        gchar *stamp = format_stamp(e->planned_utc_us);
        json_object_set_string_member(row, "entry_id", e->run.entry_id); json_object_set_string_member(row, "title", e->title);
        json_object_set_string_member(row, "artist", e->artist); json_object_set_string_member(row, "album", e->album);
        json_object_set_string_member(row, "kind", e->jingle ? "jingle" : e->kind);
        json_object_set_string_member(row, "source_name", e->source_name); json_object_set_string_member(row, "source_id", e->source_id);
        json_object_set_string_member(row, "start_utc", stamp);
        json_object_set_int_member(row, "duration_ms", (gint64)((e->run.media.cue_out_frame - e->run.media.cue_in_frame) / 48));
        json_object_set_int_member(row, "overlap_ms", (gint64)e->run.overlap_frames / 48);
        json_object_set_int_member(row, "transition", e->run.mode);
        json_object_set_boolean_member(row, "beat_sync", e->run.beat_sync);
        json_object_set_string_member(row, "effect", e->run.effect == RADIOS_AUDIO_RUN_EFFECT_LOWPASS ? "lowpass" : "none");
        json_object_set_string_member(row, "preset", e->run.shape.enabled ? str(e->binding, "transition_preset", "") : "");
        json_object_set_int_member(row, "deck_index", e->run.preferred_deck >= 0 ? e->run.preferred_deck : (gint)p->options.deck_pool[i % p->options.pool_count]);
        json_object_set_string_member(row, "reason", e->jingle ? "Jingle" : "Kurallara uygun");
        /* The queue projection reads a live step's bounds from the display. */
        if (e->calendar_constraints) json_object_set_object_member(row, "calendar_constraints", copy_object(e->calendar_constraints));
        json_array_add_object_element(rows, row); g_free(stamp);
    }
    json_object_set_array_member(display, "entries", rows);
}
gboolean radios_playout_plan_adopt_source(RadioSPlayoutPlan *plan, guint64 source_frame,
    const RadioSPlayoutRequest *request, GError **error)
{
    if (!plan || !request || !plan->entries->len || plan->entries->len > RADIOS_AUDIO_RUN_MAX_ITEMS || request->now_utc_us <= 0)
        return fail(error, "Çalan AutoDJ kaynağı veya başlangıç saati geçersiz.");
    RadioSPlayoutEntry *first = g_ptr_array_index(plan->entries, 0);
    if (source_frame >= first->run.media.cue_out_frame)
        return fail(error, "Çalan parçanın kullanılabilir AutoDJ süresi kalmadı.");
    RadioSAudioRunItem original[RADIOS_AUDIO_RUN_MAX_ITEMS]; gint64 times[RADIOS_AUDIO_RUN_MAX_ITEMS];
    for (guint i = 0; i < plan->entries->len; ++i) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, i); original[i] = entry->run; times[i] = entry->planned_utc_us;
    }
    RadioSAudioTempoProfile profile = radios_audio_run_tempo_profile(&first->run);
    gdouble elapsed = radios_audio_tempo_profile_output(&profile,
        source_frame >= first->run.media.cue_in_frame ? source_frame - first->run.media.cue_in_frame : 0);
    first->run.tempo_ratio = radios_audio_tempo_profile_ratio(&profile, elapsed);
    first->run.tempo_return_start_frames = (guint64)ceil(MAX(0.0, profile.hold_frames - elapsed));
    first->run.tempo_return_frames = (guint64)ceil(MAX(0.0,
        profile.return_frames - MIN((gdouble)profile.return_frames, MAX(0.0, elapsed - profile.hold_frames))));
    first->run.media.cue_in_frame = source_frame; first->run.fade_in_frames = 0;
    if (first->run.adjunct.enabled) {
        if (elapsed >= first->run.adjunct.start_output_frame) first->run.adjunct.enabled = FALSE;
        else first->run.adjunct.start_output_frame -= (guint64)ceil(elapsed);
    }
    if (first->run.autodj_out_frame && first->run.autodj_out_frame <= source_frame) first->run.autodj_out_frame = 0;
    first->run.auto_next_frame = MAX(first->run.auto_next_frame, source_frame);
    first->run.fade_start_frame = MAX(first->run.fade_start_frame, source_frame);
    first->run.fade_end_frame = MAX(first->run.fade_end_frame, first->run.fade_start_frame);
    guint64 span = MIN(run_duration_frames(&first->run), first->run.media.cue_out_frame - source_frame);
    first->run.overlap_frames = MIN(first->run.overlap_frames, span ? span - 1 : 0);
    first->planned_utc_us = request->now_utc_us;
    for (guint i = 1; i < plan->entries->len; ++i) {
        RadioSPlayoutEntry *previous = g_ptr_array_index(plan->entries, i - 1), *entry = g_ptr_array_index(plan->entries, i);
        entry->run.fade_in_frames = calendar_boundary(previous, &entry->run, entry->hold_us, &entry->planned_utc_us);
        if (entry->run.tempo_return_frames) calendar_tempo_return(entry);
    }
    RadioSPlayoutRequest check = *request; check.trim_calendar_end = FALSE;
    if (!calendar_beatmatch_plan(plan, &check, error) || !calendar_constraints_plan(plan, &check, error)) {
        for (guint i = 0; i < plan->entries->len; ++i) {
            RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, i); entry->run = original[i]; entry->planned_utc_us = times[i];
        }
        return FALSE;
    }
    /* Update timing without discarding the accepted event/block metadata. */
    JsonArray *rows = array(json_node_get_object(plan->display), "entries");
    for (guint i = 0; rows && i < MIN(plan->entries->len, json_array_get_length(rows)); ++i) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, i); JsonObject *row = json_array_get_object_element(rows, i);
        gchar *stamp = format_stamp(entry->planned_utc_us); json_object_set_string_member(row, "start_utc", stamp); g_free(stamp);
        json_object_set_int_member(row, "duration_ms", (gint64)(entry->run.media.cue_out_frame - entry->run.media.cue_in_frame) / 48);
        json_object_set_int_member(row, "overlap_ms", (gint64)entry->run.overlap_frames / 48);
        json_object_set_boolean_member(row, "beat_sync", entry->run.beat_sync);
    }
    radios_playout_plan_refresh_jingle_metadata(plan);
    return TRUE;
}
/* Preserve declared event boundaries from the accepted programme. Flat
 * legacy queues use the same contiguous-music model as the editor. */
static void plan_block_metadata(RadioSPlayoutPlan *plan, JsonObject *config)
{
    RadioSProgrammeTree *tree = radios_programme_tree_new(config);
    GHashTable *owners = g_hash_table_new(g_str_hash, g_str_equal);
    GHashTable *explicit = g_hash_table_new(g_str_hash, g_str_equal);
    JsonArray *blocks = array(config, "blocks");
    for (guint i = 0; blocks && i < json_array_get_length(blocks); ++i) {
        JsonObject *block = json_array_get_object_element(blocks, i);
        g_hash_table_add(explicit, (gpointer)str(block, "id", ""));
    }
    for (guint i = 0; i < tree->blocks->len; ++i) {
        RadioSProgrammeBlock *block = g_ptr_array_index(tree->blocks, i);
        for (guint j = 0; j < block->leaves->len; ++j)
            g_hash_table_insert(owners, (gpointer)str(g_ptr_array_index(block->leaves, j), "id", ""), block);
    }
    JsonArray *rows = array(json_node_get_object(plan->display), "entries");
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) {
        JsonObject *row = json_array_get_object_element(rows, i);
        RadioSProgrammeBlock *owner = g_hash_table_lookup(owners, str(row, "entry_id", ""));
        if (!owner) continue;
        const gchar *id = str(owner->block, "id", "");
        json_object_set_string_member(row, "block_id", id);
        json_object_set_string_member(row, "block_kind", str(owner->block, "kind", "music"));
        json_object_set_string_member(row, "block_title", str(owner->block, "title", ""));
        json_object_set_boolean_member(row, "block_explicit", g_hash_table_contains(explicit, id));
        /* Several kind-specific events may belong to one loaded playlist.
         * Preserve that parent identity for header timing. */
        const gchar *playlist_id = str(owner->block, "playlist_id", "");
        if (g_uuid_string_is_valid(playlist_id)) {
            json_object_set_string_member(row, "playlist_id", playlist_id);
            RadioSPlayoutEntry *entry = g_ptr_array_index(plan->entries, i);
            json_object_set_string_member(entry->binding, "playlist_id", playlist_id);
        }
    }
    g_hash_table_unref(explicit); g_hash_table_unref(owners); radios_programme_tree_free(tree);
}
static gboolean default_policy(RadioSPlayoutRequest *request, JsonObject *prepared, GError **error)
{
    if (!request->has_default_policy && json_object_has_member(prepared, "playout_policy")) {
        JsonObject *policy = object(prepared, "playout_policy"); JsonNode *beat = policy ? json_object_get_member(policy, "beat_sync") : NULL;
        gint64 mode = integer(policy, "mode", -1), overlap = integer(policy, "overlap_ms", -1);
        if (!policy || json_object_get_size(policy) != 4 || integer(policy, "schema", 0) != 1 || mode < 0 || mode > 3 ||
            overlap < 0 || overlap > 30000 || !beat || !JSON_NODE_HOLDS_VALUE(beat) || json_node_get_value_type(beat) != G_TYPE_BOOLEAN)
            return fail(error, "Genel yayın geçiş ayarları geçersiz.");
        request->has_default_policy = TRUE; request->default_mode = (guint)mode;
        request->default_overlap_seconds = (gdouble)overlap / 1000; request->default_beat_sync = json_node_get_boolean(beat);
    }
    if (!request->has_default_policy) return TRUE;
    if (request->default_mode > 3 || !isfinite(request->default_overlap_seconds) || request->default_overlap_seconds < 0 ||
        request->default_overlap_seconds > 30 || ((request->default_mode == 0 || request->default_mode == 3) &&
        (request->default_overlap_seconds != 0 || request->default_beat_sync)) ||
        (request->default_beat_sync && (request->default_mode != RADIOS_AUDIO_RUN_CROSSFADE || request->default_overlap_seconds <= 0)))
        return fail(error, "Genel yayın geçiş yöntemi veya bindirme süresi geçersiz.");
    request->mode = request->default_mode; request->overlap_seconds = request->default_overlap_seconds; return TRUE;
}
RadioSPlayoutPlan *radios_playout_plan_build(JsonObject *prepared, const RadioSPlayoutRequest *request, GError **error)
{
    if (!prepared || !request || !request->station_id || !request->database_file || !g_uuid_string_is_valid(request->station_id) ||
        integer(prepared, "contract_version", 0) != 1 || !g_str_equal(str(prepared, "station_id", ""), request->station_id) || request->now_utc_us <= 0 ||
        request->mode > 3 || !isfinite(request->overlap_seconds) || request->overlap_seconds < 0 || request->overlap_seconds > 30) {
        fail(error, "Oynatma hazırlığı istasyon/sürüm veya süre sözleşmesini karşılamıyor."); return NULL;
    }
    RadioSPlayoutRequest effective = *request;
    if (!effective.copyright_policy) effective.copyright_policy = object(prepared, "copyright_policy");
    if (!radios_copyright_policy_validate(effective.copyright_policy, error)) return NULL;
    if (!default_policy(&effective, prepared, error)) return NULL;
    if (effective.single_player) {
        effective.has_default_policy = TRUE;
        effective.mode = effective.default_mode = RADIOS_AUDIO_RUN_EOF;
        effective.overlap_seconds = effective.default_overlap_seconds = 0;
        effective.default_beat_sync = FALSE; effective.trim_calendar_end = FALSE;
    }
    request = &effective;
    RadioSPlayoutPlan *p = g_new0(RadioSPlayoutPlan, 1); p->entries = g_ptr_array_new_with_free_func(entry_free);
    p->copyright_policy = effective.copyright_policy ? copy_object(effective.copyright_policy) : NULL;
    radios_audio_run_options_default(&p->options); p->display = json_node_new(JSON_NODE_OBJECT); json_node_take_object(p->display, json_object_new());
    json_object_set_array_member(json_node_get_object(p->display), "diagnostics", json_array_new());
    guint candidates = 0; JsonArray *rows = array(prepared, "tracks"), *sources = array(prepared, "sources");
    for (guint i = 0; rows && i < json_array_get_length(rows); ++i) if (media_eligible(json_array_get_object_element(rows, i))) ++candidates;
    for (guint i = 0; sources && i < json_array_get_length(sources); ++i) {
        rows = array(json_array_get_object_element(sources, i), "tracks");
        for (guint j = 0; rows && j < json_array_get_length(rows); ++j) if (media_eligible(json_array_get_object_element(rows, j))) ++candidates;
    }
    json_object_set_int_member(json_node_get_object(p->display), "candidate_count", candidates);
    gboolean ok = g_str_equal(str(prepared, "kind", ""), "automation") ? automation_plan(p, prepared, request, error) : sequential_plan(p, prepared, request, error);
    if (ok) ok = probability_plan(p, prepared, request, error);
    if (ok) ok = player_autodj_plan(p, prepared, request, error);
    if (ok && flag(json_node_get_object(p->display),"probability_applied")) {
        ok = probability_finalize(p,error);
        if (ok) ok = calendar_beatmatch_plan(p,request,error);
        if (ok) ok = probability_finalize(p,error);
    }
    if (ok) ok = calendar_constraints_plan(p, request, error);
    if (ok && p->options.pool_count == 1 && p->entries->len) {
        /* The batch's last item has no successor in this plan; on a single
         * player it simply ends (the next refill starts after it). */
        RadioSPlayoutEntry *last = g_ptr_array_index(p->entries, p->entries->len - 1);
        last->run.overlap_frames = 0; last->run.beat_sync = FALSE; last->run.effect = RADIOS_AUDIO_RUN_EFFECT_NONE; last->run.shape.enabled = FALSE;
        if (last->run.mode == RADIOS_AUDIO_RUN_CROSSFADE) last->run.mode = RADIOS_AUDIO_RUN_EOF;
    }
    if (ok) for (guint i = 0; i < p->entries->len; ++i) {
        RadioSPlayoutEntry *entry = g_ptr_array_index(p->entries, i);
        if (entry->run.shape.enabled && (!entry->run.overlap_frames ||
            (entry->run.mode != RADIOS_AUDIO_RUN_CROSSFADE && entry->run.mode != RADIOS_AUDIO_RUN_CUE))) {
            /* A designed shape only exists over a real two-source overlap. */
            entry->run.shape.enabled = FALSE;
            diagnose(p, "transition_shape_without_overlap", entry->run.entry_id);
        }
        if (entry->run.effect == RADIOS_AUDIO_RUN_EFFECT_LOWPASS && !entry->run.overlap_frames) {
            /* A valid requested fade can disappear at an authenticated cue
             * exit or a later scheduled start. Keep that boundary and omit
             * its overlap-only effect instead of rejecting the whole block. */
            entry->run.effect = RADIOS_AUDIO_RUN_EFFECT_NONE;
            entry->run.beat_sync = FALSE;
            diagnose(p, "transition_filter_without_overlap", entry->run.entry_id);
        }
    }
    if (ok && p->options.pool_count == 1)
        for (guint i = 0; i < p->entries->len; ++i) {
            RadioSPlayoutEntry *e = g_ptr_array_index(p->entries, i);
            if (e->run.overlap_frames || e->run.mode == RADIOS_AUDIO_RUN_CROSSFADE) { ok = fail(error, "Bindirmeli otomasyon için en az iki oynatıcı seçin; tek oynatıcıda parça sonu veya manuel geçiş kullanın."); break; }
        }
    if (!ok) { radios_playout_plan_free(p); return NULL; }
    /* Keep the started programme identity with its accepted immutable plan;
     * later edits to the broadcast draft must not rename the running source. */
    if (g_str_equal(str(prepared, "kind", ""), "programme"))
        json_object_set_string_member(json_node_get_object(p->display), "programme_title",
            str(object(prepared, "config"), "title", ""));
    plan_display(p);
    if (g_str_equal(str(prepared, "kind", ""), "programme"))
        plan_block_metadata(p, object(prepared, "config"));
    else if (!p->automation) plan_automation_block_metadata(p);
    radios_playout_plan_refresh_jingle_metadata(p);
    /* A programme preparation may omit its config (bare test/legacy rows):
     * the accepted flow metadata is then simply absent. */
    if (g_str_equal(str(prepared, "kind", ""), "programme") && object(prepared, "config"))
        p->programme_config = copy_object(object(prepared, "config"));
    return p;
}
