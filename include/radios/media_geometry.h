#ifndef RADIOS_MEDIA_GEOMETRY_H
#define RADIOS_MEDIA_GEOMETRY_H

#include <json-glib/json-glib.h>
#include <math.h>

/* The same verified source span is used by selection, playback and loading.
 * Millisecond durations are estimates; their rounding error is handled by
 * the runner when the decoder establishes the real EOF. */
typedef struct {
    guint64 frames, cue_in, cue_out;
    gboolean verified;
    guint64 autodj_in, autodj_out;
    gboolean has_autodj_in, has_autodj_out, autodj_valid;
} RadioSMediaGeometry;

static inline JsonObject *radios_geometry_object(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static inline gint64 radios_geometry_integer(JsonObject *object, const gchar *key, gint64 fallback)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_INT64 ? json_node_get_int(node) : fallback;
}
static inline const gchar *radios_geometry_string(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : "";
}
static inline RadioSMediaGeometry radios_media_geometry(JsonObject *detail, gint64 duration, gboolean trim_silence)
{
    JsonObject *version = radios_geometry_object(detail, "version"), *analysis = radios_geometry_object(detail, "analysis");
    JsonObject *marks = radios_geometry_object(detail, "markers");
    gint64 rate = radios_geometry_integer(analysis, "sample_rate", 0), measured = radios_geometry_integer(analysis, "sample_frames", 0);
    const gchar *sha = radios_geometry_string(version, "sha256");
    RadioSMediaGeometry result = {.frames = (guint64)MAX(duration, 0) * 48};
    result.verified = rate >= 8000 && rate <= 384000 && measured > 0 && measured <= rate * 86400 && *sha &&
        g_str_equal(radios_geometry_string(analysis, "sha256"), sha) && fabs((gdouble)measured * 1000 / rate - duration) <= 1.000001;
    if (result.verified) result.frames = ((guint64)measured * 48000 + (guint64)rate - 1) / (guint64)rate;
    result.cue_in = (guint64)MAX(radios_geometry_integer(marks, "cue_in_ms", 0), 0) * 48;
    result.cue_out = marks ? MIN((guint64)MAX(radios_geometry_integer(marks, "out_ms", duration), 0) * 48, result.frames) : result.frames;
    JsonObject *audible = trim_silence && !marks && result.verified ? radios_geometry_object(analysis, "audible") : NULL;
    gint64 start = radios_geometry_integer(audible, "start_ms", -1), end = radios_geometry_integer(audible, "end_ms", -1);
    if (start >= 0 && end > start + 1000 && (guint64)end * 48 <= result.frames + 48) {
        result.cue_in = (guint64)start * 48; result.cue_out = MIN((guint64)end * 48, result.frames);
    }
    result.autodj_in = result.cue_in; result.autodj_out = result.cue_out; result.autodj_valid = TRUE;
    const gchar *keys[] = {"autodj_in_ms", "autodj_out_ms"};
    for (guint i = 0; marks && i < G_N_ELEMENTS(keys); ++i) {
        JsonNode *node = json_object_get_member(marks, keys[i]);
        if (!node || JSON_NODE_HOLDS_NULL(node)) continue;
        if (!JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64) { result.autodj_valid = FALSE; continue; }
        gint64 value = json_node_get_int(node);
        if (value < MAX(radios_geometry_integer(marks, "cue_in_ms", 0), 0) || value > radios_geometry_integer(marks, "out_ms", duration)) {
            result.autodj_valid = FALSE; continue;
        }
        guint64 frame = MIN((guint64)value * 48, result.cue_out);
        if (i) { result.autodj_out = frame; result.has_autodj_out = TRUE; }
        else { result.autodj_in = frame; result.has_autodj_in = TRUE; }
    }
    result.autodj_valid &= result.autodj_in < result.autodj_out;
    return result;
}
/* Automation never airs a file's trailing silence: its source span ends where
 * the sound ends when that is over 250 ms before the cue OUT, so EOF and
 * crossfade exits are measured from the music itself. Unmarked media already
 * lost the silence above. A kept frame (an AutoDJ OUT in use) stays inside. */
static inline guint64 radios_media_geometry_sound_out(JsonObject *detail, const RadioSMediaGeometry *geometry,
    guint64 in, guint64 keep)
{
    JsonObject *audible = radios_geometry_object(radios_geometry_object(detail, "analysis"), "audible");
    gint64 end_ms = geometry->verified ? radios_geometry_integer(audible, "end_ms", -1) : -1;
    /* An explicit mix point alone says nothing about the physical tail. */
    if (end_ms <= 0 || (guint64)end_ms > geometry->frames / 48 + 1) return geometry->cue_out;
    guint64 end = MAX((guint64)end_ms * 48, keep);
    return end > in && end + 250 * 48 < geometry->cue_out ? end : geometry->cue_out;
}

#endif
