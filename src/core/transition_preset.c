#include "radios/transition_preset.h"
#include <math.h>
#include <string.h>

const gchar *const radios_transition_kind_ids[RADIOS_TRANSITION_KIND_COUNT] = {"music", "advertisement", "jingle", "sweeper"};
const gchar *const radios_transition_kind_labels[RADIOS_TRANSITION_KIND_COUNT] = {"Müzik", "Reklam", "Jingle", "Sweeper"};
static const gchar *const effect_ids[RADIOS_FX_COUNT] = {"delay", "echo", "spiral", "reverb", "trans", "filter",
    "flanger", "phaser", "robot", "melodic", "slip_roll", "roll", "rev_roll", "midi_lfo"};
static const gchar *const tone_ids[] = {"none", "lowpass", "highpass"};
static const gchar *const rotation_ids[] = {"sequential", "random"};

static const gchar *text(JsonObject *object, const gchar *name)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : "";
}
static gboolean integer(JsonObject *object, const gchar *name, gint64 low, gint64 high, gint64 *value)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_INT64) return FALSE;
    *value = json_node_get_int(node); return *value >= low && *value <= high;
}
static gboolean number(JsonNode *node, gdouble *value)
{
    if (!node || !JSON_NODE_HOLDS_VALUE(node)) return FALSE;
    GType type = json_node_get_value_type(node);
    if (type != G_TYPE_INT64 && type != G_TYPE_DOUBLE) return FALSE;
    *value = json_node_get_double(node); return isfinite(*value);
}
static gboolean boolean(JsonObject *object, const gchar *name, gboolean *value)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    if (!node || !JSON_NODE_HOLDS_VALUE(node) || json_node_get_value_type(node) != G_TYPE_BOOLEAN) return FALSE;
    *value = json_node_get_boolean(node); return TRUE;
}
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
static gint string_index(const gchar *const *ids, guint count, const gchar *value)
{
    for (guint i = 0; i < count; ++i) if (!g_strcmp0(ids[i], value)) return (gint)i;
    return -1;
}
static gboolean keys_allowed(JsonObject *object, const gchar *const *allowed, guint count)
{
    if (!object) return FALSE;
    GList *members = json_object_get_members(object); gboolean valid = TRUE;
    for (GList *it = members; valid && it; it = it->next) valid = string_index(allowed, count, it->data) >= 0;
    g_list_free(members); return valid;
}
static gboolean name_valid(const gchar *name, glong limit)
{
    if (!name || !*name || !g_utf8_validate(name, -1, NULL)) return FALSE;
    glong length = g_utf8_strlen(name, -1); if (length > limit) return FALSE;
    for (const gchar *c = name; *c; ++c) if ((guchar)*c < 0x20) return FALSE;
    gchar *trimmed = g_strdup(name); g_strstrip(trimmed); gboolean blank = !*trimmed; g_free(trimmed);
    return !blank;
}

const gchar *radios_transition_effect_id(gint type)
{ return type >= 0 && type < (gint)RADIOS_FX_COUNT ? effect_ids[type] : "none"; }
gint radios_transition_effect_type(const gchar *id)
{ return string_index(effect_ids, RADIOS_FX_COUNT, id); }
gint radios_transition_kind_index(const gchar *kind)
{ return string_index(radios_transition_kind_ids, RADIOS_TRANSITION_KIND_COUNT, kind); }

static gboolean curve_read(JsonArray *points, RadioSAudioCurve *curve)
{
    memset(curve, 0, sizeof *curve);
    if (!points || json_array_get_length(points) < 2 || json_array_get_length(points) > RADIOS_AUDIO_CURVE_POINTS) return FALSE;
    for (guint i = 0; i < json_array_get_length(points); ++i) {
        JsonNode *node = json_array_get_element(points, i);
        JsonArray *pair = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
        gdouble t, v;
        if (!pair || json_array_get_length(pair) != 2 || !number(json_array_get_element(pair, 0), &t) ||
            !number(json_array_get_element(pair, 1), &v)) return FALSE;
        curve->t[i] = t; curve->v[i] = v; curve->count = i + 1;
    }
    return radios_audio_curve_valid(curve);
}
static gboolean side_read(JsonObject *side, RadioSAudioTransitionShape *shape, guint index)
{
    const gchar *allowed[] = {"volume", "bass", "tone", "tone_kind"};
    if (!keys_allowed(side, allowed, G_N_ELEMENTS(allowed)) || json_object_get_size(side) != G_N_ELEMENTS(allowed)) return FALSE;
    gint tone = string_index(tone_ids, G_N_ELEMENTS(tone_ids), text(side, "tone_kind"));
    if (tone < 0) return FALSE;
    shape->tone_kind[index] = (RadioSAudioToneKind)tone;
    return curve_read(member_array(side, "volume"), &shape->volume[index]) &&
        curve_read(member_array(side, "bass"), &shape->bass[index]) &&
        curve_read(member_array(side, "tone"), &shape->tone[index]);
}
static gboolean effect_read(JsonObject *effect, RadioSAudioTransitionShape *shape)
{
    const gchar *allowed[] = {"type", "depth", "side", "mix", "bpm"};
    gint64 depth = 0, bpm = 120;
    if (!keys_allowed(effect, allowed, G_N_ELEMENTS(allowed)) || !json_object_has_member(effect, "type") ||
        !json_object_has_member(effect, "depth") || !json_object_has_member(effect, "side") || !json_object_has_member(effect, "mix")) return FALSE;
    const gchar *type = text(effect, "type"), *side = text(effect, "side");
    if (!g_str_equal(type, "none") && radios_transition_effect_type(type) < 0) return FALSE;
    if (!integer(effect, "depth", 0, 100, &depth) || (json_object_has_member(effect, "bpm") && !integer(effect, "bpm", 30, 300, &bpm))) return FALSE;
    if (!g_str_equal(side, "out") && !g_str_equal(side, "in")) return FALSE;
    shape->effect = g_str_equal(type, "none") ? -1 : radios_transition_effect_type(type);
    shape->effect_depth = depth / 100.0; shape->effect_bpm = (gdouble)bpm; shape->effect_side = g_str_equal(side, "in") ? 1 : 0;
    return curve_read(member_array(effect, "mix"), &shape->effect_mix);
}
static gboolean preset_read(JsonObject *preset, RadioSAudioTransitionShape *shape, gboolean require_id)
{
    const gchar *allowed[] = {"id", "name", "duration_ms", "beat_sync", "out", "in", "effect"};
    memset(shape, 0, sizeof *shape); shape->effect = -1; shape->effect_bpm = 120;
    gint64 duration = 0; gboolean sync = FALSE;
    if (!keys_allowed(preset, allowed, G_N_ELEMENTS(allowed))) return FALSE;
    if (json_object_has_member(preset, "id") && !g_uuid_string_is_valid(text(preset, "id"))) return FALSE;
    if (require_id && !json_object_has_member(preset, "id")) return FALSE;
    if (!name_valid(text(preset, "name"), 60) || !integer(preset, "duration_ms", 500, 30000, &duration)) return FALSE;
    if (json_object_has_member(preset, "beat_sync") && !boolean(preset, "beat_sync", &sync)) return FALSE;
    if (!side_read(member_object(preset, "out"), shape, 0) || !side_read(member_object(preset, "in"), shape, 1) ||
        !effect_read(member_object(preset, "effect"), shape)) return FALSE;
    shape->enabled = TRUE;
    return radios_audio_transition_shape_validate(shape, NULL);
}
gboolean radios_transition_preset_valid(JsonObject *preset, gboolean require_id)
{ RadioSAudioTransitionShape shape; return preset_read(preset, &shape, require_id); }
gboolean radios_transition_preset_to_shape(JsonObject *preset, RadioSAudioTransitionShape *shape)
{
    RadioSAudioTransitionShape parsed;
    if (!shape) return FALSE;
    if (!preset_read(preset, &parsed, FALSE)) { memset(shape, 0, sizeof *shape); shape->effect = -1; return FALSE; }
    *shape = parsed; return TRUE;
}
gboolean radios_transition_presets_valid(JsonArray *presets)
{
    if (!presets || json_array_get_length(presets) > RADIOS_TRANSITION_PRESET_LIMIT) return FALSE;
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal); gboolean valid = TRUE;
    for (guint i = 0; valid && i < json_array_get_length(presets); ++i) {
        JsonNode *node = json_array_get_element(presets, i);
        JsonObject *preset = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
        valid = radios_transition_preset_valid(preset, TRUE) && g_hash_table_add(seen, (gpointer)text(preset, "id"));
    }
    g_hash_table_unref(seen); return valid;
}
static JsonObject *preset_by_id(JsonArray *presets, const gchar *id)
{
    for (guint i = 0; presets && id && i < json_array_get_length(presets); ++i) {
        JsonNode *node = json_array_get_element(presets, i);
        JsonObject *preset = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
        if (preset && g_str_equal(text(preset, "id"), id)) return preset;
    }
    return NULL;
}
static gboolean references_valid(JsonArray *ids, JsonArray *presets, gboolean require_any)
{
    if (!ids || json_array_get_length(ids) > RADIOS_TRANSITION_PRESET_LIMIT || (require_any && !json_array_get_length(ids))) return FALSE;
    for (guint i = 0; i < json_array_get_length(ids); ++i) {
        JsonNode *node = json_array_get_element(ids, i);
        const gchar *id = node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
        if (!id || !preset_by_id(presets, id)) return FALSE;
        for (guint j = 0; j < i; ++j) if (g_str_equal(json_array_get_string_element(ids, j), id)) return FALSE;
    }
    return TRUE;
}
static gboolean assignment_valid(JsonObject *entry, JsonArray *presets, gboolean require_any)
{
    const gchar *allowed[] = {"presets", "rotation"};
    return keys_allowed(entry, allowed, G_N_ELEMENTS(allowed)) && json_object_get_size(entry) == 2 &&
        string_index(rotation_ids, G_N_ELEMENTS(rotation_ids), text(entry, "rotation")) >= 0 &&
        references_valid(member_array(entry, "presets"), presets, require_any);
}
static gboolean schedule_valid(JsonArray *schedule, JsonArray *presets)
{
    if (!schedule || json_array_get_length(schedule) > RADIOS_TRANSITION_SCHEDULE_LIMIT) return FALSE;
    const gchar *allowed[] = {"id", "name", "days", "start_minute", "end_minute", "kind", "presets", "rotation"};
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal); gboolean valid = TRUE;
    for (guint i = 0; valid && i < json_array_get_length(schedule); ++i) {
        JsonNode *node = json_array_get_element(schedule, i);
        JsonObject *row = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
        gint64 start = 0, end = 0; JsonArray *days = member_array(row, "days"); guint mask = 0;
        valid = keys_allowed(row, allowed, G_N_ELEMENTS(allowed)) && json_object_get_size(row) == G_N_ELEMENTS(allowed) &&
            g_uuid_string_is_valid(text(row, "id")) && g_hash_table_add(seen, (gpointer)text(row, "id")) &&
            name_valid(text(row, "name"), 60) && integer(row, "start_minute", 0, 1439, &start) &&
            integer(row, "end_minute", 1, 1440, &end) && start != end &&
            (g_str_equal(text(row, "kind"), "all") || radios_transition_kind_index(text(row, "kind")) >= 0) &&
            string_index(rotation_ids, G_N_ELEMENTS(rotation_ids), text(row, "rotation")) >= 0 &&
            references_valid(member_array(row, "presets"), presets, TRUE) && days && json_array_get_length(days) >= 1 && json_array_get_length(days) <= 7;
        for (guint d = 0; valid && d < json_array_get_length(days); ++d) {
            JsonNode *day = json_array_get_element(days, d);
            gint64 value = day && JSON_NODE_HOLDS_VALUE(day) && json_node_get_value_type(day) == G_TYPE_INT64 ? json_node_get_int(day) : 0;
            valid = value >= 1 && value <= 7 && !(mask & (1u << value)); mask |= 1u << value;
        }
    }
    g_hash_table_unref(seen); return valid;
}
gboolean radios_transition_automation_valid(JsonObject *automation, JsonArray *presets)
{
    const gchar *allowed[] = {"enabled", "kinds", "schedule"}; gboolean enabled = FALSE;
    if (!keys_allowed(automation, allowed, G_N_ELEMENTS(allowed)) || !boolean(automation, "enabled", &enabled)) return FALSE;
    JsonObject *kinds = member_object(automation, "kinds"); if (!kinds) return FALSE;
    GList *members = json_object_get_members(kinds); gboolean valid = TRUE;
    for (GList *it = members; valid && it; it = it->next)
        valid = radios_transition_kind_index(it->data) >= 0 && assignment_valid(member_object(kinds, it->data), presets, FALSE);
    g_list_free(members);
    if (valid && json_object_has_member(automation, "schedule")) valid = schedule_valid(member_array(automation, "schedule"), presets);
    return valid;
}

static JsonArray *curve_new(const gdouble *points, guint count)
{
    JsonArray *curve = json_array_new();
    for (guint i = 0; i < count; ++i) {
        JsonArray *pair = json_array_new();
        json_array_add_double_element(pair, points[i * 2]); json_array_add_double_element(pair, points[i * 2 + 1]);
        json_array_add_array_element(curve, pair);
    }
    return curve;
}
static JsonObject *side_new(const gdouble *volume, guint volume_count, const gdouble *bass, guint bass_count,
    const gdouble *tone, guint tone_count, const gchar *tone_kind)
{
    static const gdouble flat_one[] = {0, 1, 1, 1}, flat_zero[] = {0, 0, 1, 0};
    JsonObject *side = json_object_new();
    json_object_set_array_member(side, "volume", curve_new(volume, volume_count));
    json_object_set_array_member(side, "bass", bass ? curve_new(bass, bass_count) : curve_new(flat_one, 2));
    json_object_set_array_member(side, "tone", tone ? curve_new(tone, tone_count) : curve_new(flat_zero, 2));
    json_object_set_string_member(side, "tone_kind", tone_kind ? tone_kind : "none");
    return side;
}
static JsonObject *effect_new(const gchar *type, gint depth, const gchar *side, const gdouble *mix, guint mix_count)
{
    static const gdouble ramp[] = {0, 0, 1, 1};
    JsonObject *effect = json_object_new();
    json_object_set_string_member(effect, "type", type); json_object_set_int_member(effect, "depth", depth);
    json_object_set_string_member(effect, "side", side); json_object_set_array_member(effect, "mix", mix ? curve_new(mix, mix_count) : curve_new(ramp, 2));
    json_object_set_int_member(effect, "bpm", 120);
    return effect;
}
static JsonObject *template_new(const gchar *id, const gchar *name, gint duration, gboolean sync, JsonObject *out, JsonObject *in, JsonObject *effect)
{
    JsonObject *preset = json_object_new();
    json_object_set_string_member(preset, "id", id); json_object_set_string_member(preset, "name", name);
    json_object_set_int_member(preset, "duration_ms", duration); json_object_set_boolean_member(preset, "beat_sync", sync);
    json_object_set_object_member(preset, "out", out); json_object_set_object_member(preset, "in", in);
    json_object_set_object_member(preset, "effect", effect);
    return preset;
}
JsonArray *radios_transition_preset_templates(void)
{
    static const gdouble linear_out[] = {0, 1, 1, 0}, linear_in[] = {0, 0, 1, 1};
    static const gdouble late_out[] = {0, 1, .7, 1, 1, 0}, early_in[] = {0, 0, .3, 1, 1, 1};
    static const gdouble bass_out[] = {0, 1, .5, 1, .5, 0, 1, 0}, bass_in[] = {0, 0, .5, 0, .5, 1, 1, 1};
    static const gdouble close[] = {0, 0, 1, 1}, open[] = {0, 1, 1, 0};
    static const gdouble slam_out[] = {0, 1, .5, 1, .5, 0, 1, 0}, slam_in[] = {0, 0, .5, 0, .5, 1, 1, 1};
    static const gdouble melt_out[] = {0, 1, .3, .9, 1, 0}, melt_in[] = {0, 0, .5, .6, 1, 1};
    static const gdouble echo_out[] = {0, 1, .6, .8, 1, 0}, echo_in[] = {0, 0, .4, 1, 1, 1};
    static const gdouble cut_out[] = {0, 1, .9, 1, 1, 0}, cut_in[] = {0, 0, .1, 1, 1, 1};
    JsonArray *templates = json_array_new();
    json_array_add_object_element(templates, template_new("a1e5c6d0-5f3b-4b8e-9c1a-0d2f4e6a8b10", "Düz geçiş", 6000, FALSE,
        side_new(linear_out, 2, NULL, 0, NULL, 0, NULL), side_new(linear_in, 2, NULL, 0, NULL, 0, NULL), effect_new("none", 0, "out", NULL, 0)));
    json_array_add_object_element(templates, template_new("b2f6d7e1-6a4c-4c9f-8d2b-1e3a5f7b9c21", "Bas dalgalı geçiş", 8000, TRUE,
        side_new(late_out, 3, bass_out, 4, NULL, 0, NULL), side_new(early_in, 3, bass_in, 4, NULL, 0, NULL), effect_new("none", 0, "out", NULL, 0)));
    json_array_add_object_element(templates, template_new("c3a7e8f2-7b5d-4daa-9e3c-2f4b6a8c0d32", "Ton erit", 8000, FALSE,
        side_new(linear_out, 2, NULL, 0, close, 2, "lowpass"), side_new(linear_in, 2, NULL, 0, open, 2, "highpass"), effect_new("none", 0, "out", NULL, 0)));
    json_array_add_object_element(templates, template_new("d4b8f9a3-8c6e-4ebb-8f4d-3a5c7b9d1e43", "Slam bas", 2000, TRUE,
        side_new(slam_out, 4, bass_out, 4, NULL, 0, NULL), side_new(slam_in, 4, bass_in, 4, NULL, 0, NULL), effect_new("none", 0, "out", NULL, 0)));
    json_array_add_object_element(templates, template_new("e5c9a0b4-9d7f-4fcc-9a5e-4b6d8c0e2f54", "Erime", 12000, FALSE,
        side_new(melt_out, 3, NULL, 0, NULL, 0, NULL), side_new(melt_in, 3, NULL, 0, NULL, 0, NULL), effect_new("reverb", 60, "out", NULL, 0)));
    json_array_add_object_element(templates, template_new("f6d0b1c5-ae80-4add-8b6f-5c7e9d1f3a65", "Yankı ile çık", 6000, TRUE,
        side_new(echo_out, 3, NULL, 0, NULL, 0, NULL), side_new(echo_in, 3, NULL, 0, NULL, 0, NULL), effect_new("echo", 70, "out", NULL, 0)));
    json_array_add_object_element(templates, template_new("0712c2d6-bf91-4bee-9c70-6d8f0e2a4b76", "Filtre süpürme", 8000, TRUE,
        side_new(linear_out, 2, NULL, 0, close, 2, "lowpass"), side_new(linear_in, 2, NULL, 0, open, 2, "lowpass"), effect_new("none", 0, "out", NULL, 0)));
    json_array_add_object_element(templates, template_new("1823d3e7-c0a2-4cff-8d81-7e9a1f3b5c87", "Kes", 500, FALSE,
        side_new(cut_out, 3, NULL, 0, NULL, 0, NULL), side_new(cut_in, 3, NULL, 0, NULL, 0, NULL), effect_new("none", 0, "out", NULL, 0)));
    return templates;
}
JsonObject *radios_transition_preset_template(const gchar *name)
{
    JsonArray *templates = radios_transition_preset_templates(); JsonObject *found = NULL;
    for (guint i = 0; !found && i < json_array_get_length(templates); ++i) {
        JsonObject *preset = json_array_get_object_element(templates, i);
        if (g_str_equal(text(preset, "name"), name)) found = json_object_ref(preset);
    }
    json_array_unref(templates); return found;
}
/* ---- transition type: the structure of a preset ------------------------ */
const gchar *const radios_transition_type_ids[RADIOS_TRANSITION_TYPE_COUNT] = {"crossfade", "effect", "filter", "effect_filter"};
const gchar *const radios_transition_type_labels[RADIOS_TRANSITION_TYPE_COUNT] = {"Crossfade", "Efektli geçiş", "Filtreli geçiş", "Efektli ve filtreli geçiş"};
static gboolean preset_has_effect(JsonObject *preset)
{ return radios_transition_effect_type(text(member_object(preset, "effect"), "type")) >= 0; }
static gboolean side_has_filter(JsonObject *preset, const gchar *side)
{ return string_index(tone_ids, G_N_ELEMENTS(tone_ids), text(member_object(preset, side), "tone_kind")) > 0; }
static gboolean preset_has_filter(JsonObject *preset)
{ return side_has_filter(preset, "out") || side_has_filter(preset, "in"); }
RadioSTransitionType radios_transition_preset_type(JsonObject *preset)
{
    gboolean effect = preset_has_effect(preset), filter = preset_has_filter(preset);
    if (effect && filter) return RADIOS_TRANSITION_TYPE_EFFECT_FILTER;
    if (effect) return RADIOS_TRANSITION_TYPE_EFFECT;
    return filter ? RADIOS_TRANSITION_TYPE_FILTER : RADIOS_TRANSITION_TYPE_CROSSFADE;
}
gint radios_transition_type_index(const gchar *id)
{ return string_index(radios_transition_type_ids, RADIOS_TRANSITION_TYPE_COUNT, id); }
static gboolean curve_silent(JsonArray *points)
{
    for (guint i = 0; points && i < json_array_get_length(points); ++i) {
        JsonNode *node = json_array_get_element(points, i);
        JsonArray *pair = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL; gdouble v = 0;
        if (pair && json_array_get_length(pair) == 2 && number(json_array_get_element(pair, 1), &v) && v > 0) return FALSE;
    }
    return TRUE;
}
gboolean radios_transition_preset_set_type(JsonObject *preset, RadioSTransitionType type)
{
    static const gdouble ramp[] = {0, 0, 1, 1};
    JsonObject *effect = member_object(preset, "effect"), *out = member_object(preset, "out"), *in = member_object(preset, "in");
    if (!effect || !out || !in || type >= RADIOS_TRANSITION_TYPE_COUNT) return FALSE;
    gboolean want_effect = type == RADIOS_TRANSITION_TYPE_EFFECT || type == RADIOS_TRANSITION_TYPE_EFFECT_FILTER;
    gboolean want_filter = type == RADIOS_TRANSITION_TYPE_FILTER || type == RADIOS_TRANSITION_TYPE_EFFECT_FILTER;
    gboolean changed = FALSE; gint64 depth = 0;
    if (want_effect && !preset_has_effect(preset)) {
        json_object_set_string_member(effect, "type", "echo");
        if (!integer(effect, "depth", 1, 100, &depth)) json_object_set_int_member(effect, "depth", 50);
        if (!json_object_has_member(effect, "bpm")) json_object_set_int_member(effect, "bpm", 120);
        if (curve_silent(member_array(effect, "mix"))) json_object_set_array_member(effect, "mix", curve_new(ramp, 2));
        changed = TRUE;
    } else if (!want_effect && preset_has_effect(preset)) { json_object_set_string_member(effect, "type", "none"); changed = TRUE; }
    if (want_filter && !preset_has_filter(preset)) {
        json_object_set_string_member(out, "tone_kind", "lowpass");
        if (curve_silent(member_array(out, "tone"))) json_object_set_array_member(out, "tone", curve_new(ramp, 2));
        changed = TRUE;
    } else if (!want_filter && preset_has_filter(preset)) {
        if (side_has_filter(preset, "out")) json_object_set_string_member(out, "tone_kind", "none");
        if (side_has_filter(preset, "in")) json_object_set_string_member(in, "tone_kind", "none");
        changed = TRUE;
    }
    return changed;
}

static gboolean window_contains(gint64 start, gint64 end, gint minute)
{ return start < end ? minute >= start && minute < end : minute >= start || minute < end; }
JsonObject *radios_transition_preset_choose(JsonObject *automation, JsonArray *presets,
    const gchar *kind, gint weekday, gint minute, guint *counter, guint32 draw)
{
    gboolean enabled = FALSE;
    if (!boolean(automation, "enabled", &enabled) || !enabled || radios_transition_kind_index(kind) < 0) return NULL;
    JsonArray *ids = NULL; const gchar *rotation = "sequential";
    JsonArray *schedule = member_array(automation, "schedule");
    for (guint i = 0; !ids && schedule && weekday >= 1 && minute >= 0 && i < json_array_get_length(schedule); ++i) {
        JsonNode *node = json_array_get_element(schedule, i);
        JsonObject *row = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
        JsonArray *days = member_array(row, "days"); gboolean today = FALSE; gint64 start = 0, end = 0;
        if (!row || (!g_str_equal(text(row, "kind"), "all") && !g_str_equal(text(row, "kind"), kind))) continue;
        for (guint d = 0; days && d < json_array_get_length(days); ++d) today |= json_array_get_int_element(days, d) == weekday;
        if (!today || !integer(row, "start_minute", 0, 1439, &start) || !integer(row, "end_minute", 1, 1440, &end) ||
            !window_contains(start, end, minute) || !member_array(row, "presets") || !json_array_get_length(member_array(row, "presets"))) continue;
        ids = member_array(row, "presets"); rotation = text(row, "rotation");
    }
    if (!ids) {
        JsonObject *entry = member_object(member_object(automation, "kinds"), kind);
        ids = member_array(entry, "presets"); rotation = text(entry, "rotation");
    }
    guint count = ids ? json_array_get_length(ids) : 0;
    if (!count) return NULL;
    guint index = g_str_equal(rotation, "random") ? draw % count : (counter ? (*counter)++ : 0) % count;
    JsonNode *node = json_array_get_element(ids, index);
    const gchar *id = node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ? json_node_get_string(node) : NULL;
    return preset_by_id(presets, id);
}

/* ---- pure curve/shape helpers declared in radios/audio_dsp.h ----------- */
static gboolean unit(gdouble value, gdouble low, gdouble high)
{ return isfinite(value) && value >= low && value <= high; }
static gboolean shape_invalid(GError **error, const gchar *message)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, message); return FALSE; }
gdouble radios_audio_curve_value(const RadioSAudioCurve *curve, gdouble t)
{
    if (!curve || !curve->count) return 1;
    if (!isfinite(t)) t = 0;
    t = CLAMP(t, 0, 1);
    if (t <= curve->t[0]) return curve->v[0];
    for (guint i = 1; i < curve->count; ++i) {
        if (t > curve->t[i]) continue;
        gdouble span = curve->t[i] - curve->t[i - 1];
        if (span <= 0) return curve->v[i];
        return curve->v[i - 1] + (curve->v[i] - curve->v[i - 1]) * (t - curve->t[i - 1]) / span;
    }
    return curve->v[curve->count - 1];
}
gboolean radios_audio_curve_valid(const RadioSAudioCurve *curve)
{
    if (!curve || curve->count < 2 || curve->count > RADIOS_AUDIO_CURVE_POINTS) return FALSE;
    for (guint i = 0; i < curve->count; ++i) {
        if (!unit(curve->t[i], 0, 1) || !unit(curve->v[i], 0, 1)) return FALSE;
        if (i && curve->t[i] < curve->t[i - 1]) return FALSE;
    }
    return curve->t[0] == 0 && curve->t[curve->count - 1] == 1;
}
gboolean radios_audio_transition_shape_validate(const RadioSAudioTransitionShape *shape, GError **error)
{
    if (!shape) return shape_invalid(error, "Invalid designed transition shape");
    if (!shape->enabled) return TRUE;
    for (guint side = 0; side < 2; ++side)
        if (!radios_audio_curve_valid(&shape->volume[side]) || !radios_audio_curve_valid(&shape->bass[side]) ||
            !radios_audio_curve_valid(&shape->tone[side]) ||
            shape->tone_kind[side] < RADIOS_AUDIO_TONE_NONE || shape->tone_kind[side] > RADIOS_AUDIO_TONE_HIGHPASS)
            return shape_invalid(error, "Invalid designed transition curve");
    if (shape->effect < -1 || shape->effect >= (gint)RADIOS_FX_COUNT || shape->effect_side > 1 ||
        (shape->effect >= 0 && (!radios_audio_curve_valid(&shape->effect_mix) ||
            !unit(shape->effect_depth, 0, 1) || !unit(shape->effect_bpm, 30, 300))))
        return shape_invalid(error, "Invalid designed transition effect");
    return TRUE;
}
void radios_audio_transition_shape_gains(const RadioSAudioTransitionShape *shape, gdouble progress, gdouble *outgoing, gdouble *incoming)
{
    *outgoing = radios_audio_curve_value(shape ? &shape->volume[0] : NULL, progress);
    *incoming = radios_audio_curve_value(shape ? &shape->volume[1] : NULL, progress);
}
