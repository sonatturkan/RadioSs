#include "radios/autodj_automation_ui.h"
#include "radios/transition_distribution.h"
#include "radios/transition_preset.h"
#include "radios/widgets.h"
#include "radios/data.h"
#include <math.h>
#include <string.h>

/* Designed AutoDJ transitions in normal form: a control appears only once an
 * earlier choice makes it meaningful — the switch shows the presets, a
 * selected preset shows its fields, its transition type (the preset's
 * structure, see radios_transition_preset_type) shows the effect row or the
 * filter row, an effect shows its depth/side/tempo and curve, a filter shows
 * its curve, a second preset shows the rotation, a schedule row shows the
 * table and form — so the page carries no explanatory text. The page keeps
 * owned working copies of the two JSON members and reports every accepted
 * edit through hooks.changed(); the rotation editor re-reads them into
 * settings ("autodj_presets", "autodj_automation"). The "Türler" rows also
 * carry the parent's per-kind rule and players (kind_transitions,
 * autodj_players), which the parent alone reads and writes. */
enum { CURVE_VOLUME, CURVE_BASS, CURVE_TONE, CURVE_EFFECT, CURVE_COUNT };
enum { ROW_ID, ROW_NAME, ROW_DAYS, ROW_TIME, ROW_KIND, ROW_PRESETS, ROW_ROTATION, ROW_COLUMNS };
#define KIND_COUNT RADIOS_TRANSITION_KIND_COUNT
static const gchar *const curve_keys[] = {"volume", "bass", "tone", "mix"};
static const gchar *const curve_labels[] = {"Ses", "Bas", "Ton", "Efekt"};
static const gchar *const curve_colors[] = {"#59d1ff", "#f2c94c", "#ff7a45", "#7ddb6b"};
static const gchar *const side_keys[] = {"out", "in"};
static const gchar *const day_labels[] = {"Pzt", "Sal", "Çar", "Per", "Cum", "Cmt", "Paz"};
static const gchar *const tone_ids[] = {"none", "lowpass", "highpass"};
static const gchar *const tone_labels[] = {"Filtre yok", "Alçak geçiren", "Yüksek geçiren"};
static const gchar *const rotation_ids[] = {"sequential", "random"}, *const rotation_labels[] = {"Sırayla", "Rastgele"};

typedef struct {
    RadioSViewContext *ctx;
    RadioSAutodjAutomationHooks hooks;
    GtkWidget *root, *enabled, *body, *status;
    GtkWidget *chips, *add_popover, *delete_button, *name, *type, *duration, *beat_sync;
    GtkWidget *effect_row, *effect_type, *effect_params, *effect_depth, *effect_side, *effect_bpm, *filter_row, *tone[2];
    GtkWidget *designer, *curve_buttons[CURVE_COUNT], *play, *shift[2][2], *design_box;
    GtkWidget *kind_rows[KIND_COUNT], *kind_rotation[KIND_COUNT], *kind_menus[KIND_COUNT], *probability_button;
    GtkWidget *kinds_row, *policy, *kind_captions[2], *distributions[KIND_COUNT];
    GtkWidget *schedule_box, *schedule_view, *schedule_form, *schedule_name, *schedule_days[7], *schedule_start, *schedule_end;
    GtkWidget *schedule_kind, *schedule_presets, *schedule_rotation_box, *schedule_rotation, *schedule_remove;
    GtkListStore *schedule_store;
    JsonArray *presets;
    JsonObject *automation;
    gchar *selected, *selected_row;
    guint curve, updating, play_source;
    gdouble play_progress;
    gboolean touched, dragging;
    guint drag_side, drag_point;
} AutomationPage;

static void commit(AutomationPage *page);
static void designer_redraw(AutomationPage *page);
static void chips_render(AutomationPage *page);
static void fields_render(AutomationPage *page);
static void kinds_render(AutomationPage *page);
static void schedule_render(AutomationPage *page);
static void schedule_form_render(AutomationPage *page);

static AutomationPage *page_get(GtkWidget *widget)
{ return widget ? g_object_get_data(G_OBJECT(widget), "radios-autodj-automation") : NULL; }
static GtkWidget *named(GtkWidget *widget, const gchar *suffix)
{
    gchar *name = g_strconcat("rotation-editor-automation-", suffix, NULL);
    gtk_widget_set_name(widget, name); g_free(name); return widget;
}
static GtkWidget *caption(const gchar *text, const gchar *class_name)
{
    GtkWidget *label = radios_ui_label(text, class_name);
    gtk_label_set_line_wrap(GTK_LABEL(label), FALSE); return label;
}
/* Dependent controls are hidden, not greyed: nothing asks to be explained. */
static void show(GtkWidget *widget, gboolean visible)
{
    if (!widget) return;
    gtk_widget_set_no_show_all(widget, !visible);
    if (visible) gtk_widget_show_all(widget); else gtk_widget_hide(widget);
}
static JsonObject *object_member(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonArray *array_member(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
}
static gboolean bool_member(JsonObject *object, const gchar *key)
{
    JsonNode *node = object ? json_object_get_member(object, key) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN && json_node_get_boolean(node);
}
static JsonNode *deep_copy(JsonNode *node)
{
    if (!node) return NULL;
    gchar *text = json_to_string(node, FALSE); JsonParser *parser = json_parser_new();
    gboolean loaded = json_parser_load_from_data(parser, text, -1, NULL); g_free(text);
    JsonNode *copy = loaded ? json_parser_steal_root(parser) : NULL; g_object_unref(parser); return copy;
}
static JsonObject *copy_object(JsonObject *object)
{
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, object);
    JsonNode *copy = deep_copy(node); json_node_free(node);
    JsonObject *result = copy ? json_object_ref(json_node_get_object(copy)) : json_object_new();
    if (copy) json_node_free(copy);
    return result;
}
static JsonArray *copy_array(JsonArray *array)
{
    JsonNode *node = json_node_new(JSON_NODE_ARRAY); json_node_set_array(node, array);
    JsonNode *copy = deep_copy(node); json_node_free(node);
    JsonArray *result = copy ? json_array_ref(json_node_get_array(copy)) : json_array_new();
    if (copy) json_node_free(copy);
    return result;
}
static guint preset_count(AutomationPage *page)
{ return page->presets ? json_array_get_length(page->presets) : 0; }
static JsonObject *preset_at(AutomationPage *page, guint index)
{
    JsonNode *node = index < preset_count(page) ? json_array_get_element(page->presets, index) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonObject *preset_by_id(AutomationPage *page, const gchar *id, guint *index)
{
    for (guint i = 0; id && i < preset_count(page); ++i) {
        JsonObject *preset = preset_at(page, i);
        if (preset && !g_strcmp0(radios_json_string(preset, "id", ""), id)) { if (index) *index = i; return preset; }
    }
    return NULL;
}
static JsonObject *current_preset(AutomationPage *page)
{ return preset_by_id(page, page->selected, NULL); }
static JsonArray *curve_array(JsonObject *preset, guint side, guint curve)
{
    if (!preset) return NULL;
    if (curve == CURVE_EFFECT) return array_member(object_member(preset, "effect"), "mix");
    return array_member(object_member(preset, side_keys[side]), curve_keys[curve]);
}
static void curve_load(JsonArray *points, RadioSAudioCurve *curve)
{
    memset(curve, 0, sizeof *curve);
    for (guint i = 0; points && i < json_array_get_length(points) && i < RADIOS_AUDIO_CURVE_POINTS; ++i) {
        JsonNode *node = json_array_get_element(points, i);
        JsonArray *pair = node && JSON_NODE_HOLDS_ARRAY(node) ? json_node_get_array(node) : NULL;
        if (!pair || json_array_get_length(pair) != 2) continue;
        curve->t[curve->count] = CLAMP(json_array_get_double_element(pair, 0), 0, 1);
        curve->v[curve->count] = CLAMP(json_array_get_double_element(pair, 1), 0, 1); ++curve->count;
    }
    if (curve->count < 2) { curve->count = 2; curve->t[0] = 0; curve->v[0] = 1; curve->t[1] = 1; curve->v[1] = 1; }
}
static JsonArray *curve_store(const RadioSAudioCurve *curve)
{
    JsonArray *points = json_array_new();
    for (guint i = 0; i < curve->count; ++i) {
        JsonArray *pair = json_array_new();
        json_array_add_double_element(pair, floor(curve->t[i] * 1000 + .5) / 1000);
        json_array_add_double_element(pair, floor(curve->v[i] * 1000 + .5) / 1000);
        json_array_add_array_element(points, pair);
    }
    return points;
}
static void curve_save(JsonObject *preset, guint side, guint curve, const RadioSAudioCurve *values)
{
    if (!preset) return;
    JsonObject *owner = curve == CURVE_EFFECT ? object_member(preset, "effect") : object_member(preset, side_keys[side]);
    if (!owner) return;
    json_object_set_array_member(owner, curve == CURVE_EFFECT ? "mix" : curve_keys[curve], curve_store(values));
}
static gint effect_type(JsonObject *preset)
{ return radios_transition_effect_type(radios_json_string(object_member(preset, "effect"), "type", "none")); }
static guint effect_side(JsonObject *preset)
{ return !g_strcmp0(radios_json_string(object_member(preset, "effect"), "side", "out"), "in") ? 1 : 0; }
static gboolean tone_used(JsonObject *preset)
{
    for (guint side = 0; side < 2; ++side)
        if (g_strcmp0(radios_json_string(object_member(preset, side_keys[side]), "tone_kind", "none"), "none")) return TRUE;
    return FALSE;
}

/* ---- designer -------------------------------------------------------- */
static void lane_geometry(GtkWidget *area, guint side, gdouble *x0, gdouble *y0, gdouble *width, gdouble *height)
{
    gdouble w = gtk_widget_get_allocated_width(area), h = gtk_widget_get_allocated_height(area), gap = 14, margin = 12;
    *x0 = margin; *width = MAX(10.0, w - 2 * margin);
    *height = MAX(10.0, (h - 3 * gap) / 2);
    *y0 = side ? 2 * gap + *height : gap;
}
static gdouble noise(guint side, gint x)
{
    guint32 value = (guint32)(x * 2654435761u) ^ (side + 1) * 40503u; value ^= value >> 13; value *= 0x5bd1e995u; value ^= value >> 15;
    return (value % 1000) / 1000.0;
}
static void color_parse(const gchar *hex, gdouble *r, gdouble *g, gdouble *b)
{
    GdkRGBA rgba; gdk_rgba_parse(&rgba, hex); *r = rgba.red; *g = rgba.green; *b = rgba.blue;
}
static void draw_curve(cairo_t *cr, const RadioSAudioCurve *curve, gdouble x0, gdouble y0, gdouble width, gdouble height,
    const gchar *color, gdouble line_width, gboolean handles)
{
    gdouble r, g, b; color_parse(color, &r, &g, &b);
    cairo_set_source_rgba(cr, r, g, b, handles ? 1 : .55); cairo_set_line_width(cr, line_width);
    for (guint i = 0; i < curve->count; ++i) {
        gdouble x = x0 + curve->t[i] * width, y = y0 + (1 - curve->v[i]) * height;
        if (i) cairo_line_to(cr, x, y); else cairo_move_to(cr, x, y);
    }
    cairo_stroke(cr);
    if (!handles) return;
    for (guint i = 0; i < curve->count; ++i) {
        gdouble x = x0 + curve->t[i] * width, y = y0 + (1 - curve->v[i]) * height;
        cairo_arc(cr, x, y, 6, 0, 2 * G_PI); cairo_set_source_rgb(cr, 1, 1, 1); cairo_fill_preserve(cr);
        cairo_set_source_rgb(cr, r, g, b); cairo_set_line_width(cr, 2); cairo_stroke(cr);
    }
}
static gboolean designer_draw(GtkWidget *area, cairo_t *cr, gpointer data)
{
    AutomationPage *page = data; JsonObject *preset = current_preset(page);
    gdouble w = gtk_widget_get_allocated_width(area), h = gtk_widget_get_allocated_height(area);
    cairo_set_source_rgb(cr, .106, .165, .196); cairo_rectangle(cr, 0, 0, w, h); cairo_fill(cr);
    for (guint side = 0; side < 2; ++side) {
        gdouble x0, y0, width, height; lane_geometry(area, side, &x0, &y0, &width, &height);
        cairo_set_source_rgb(cr, .16, .22, .26); cairo_rectangle(cr, x0, y0, width, height); cairo_fill(cr);
        cairo_set_source_rgb(cr, .25, .33, .38); cairo_set_line_width(cr, 1);
        cairo_move_to(cr, x0, y0 + height / 2); cairo_line_to(cr, x0 + width, y0 + height / 2); cairo_stroke(cr);
        if (!preset) continue;
        RadioSAudioCurve volume, bass, tone, mix;
        curve_load(curve_array(preset, side, CURVE_VOLUME), &volume); curve_load(curve_array(preset, side, CURVE_BASS), &bass);
        curve_load(curve_array(preset, side, CURVE_TONE), &tone); curve_load(curve_array(preset, side, CURVE_EFFECT), &mix);
        /* A stylised programme waveform: outer band follows the volume curve,
         * the inner (low frequency) core follows the bass curve. */
        for (gint x = 0; x < (gint)width; x += 2) {
            gdouble t = x / width, base = .25 + .6 * noise(side, x / 2) * (.6 + .4 * noise(side, x / 7 + 11));
            gdouble gain = radios_audio_curve_value(&volume, t), outer = base * gain * height * .48, inner = outer * .5 * radios_audio_curve_value(&bass, t);
            cairo_set_source_rgb(cr, .13, .29, .72); cairo_rectangle(cr, x0 + x, y0 + height / 2 - outer, 1.4, outer * 2); cairo_fill(cr);
            cairo_set_source_rgb(cr, .83, .45, .13); cairo_rectangle(cr, x0 + x, y0 + height / 2 - inner, 1.4, inner * 2); cairo_fill(cr);
        }
        /* Only curves that an earlier choice made meaningful are drawn. */
        gboolean effect_here = effect_type(preset) >= 0 && effect_side(preset) == side;
        draw_curve(cr, &bass, x0, y0, width, height, curve_colors[CURVE_BASS], 2, page->curve == CURVE_BASS);
        if (tone_used(preset)) draw_curve(cr, &tone, x0, y0, width, height, curve_colors[CURVE_TONE], 2, page->curve == CURVE_TONE);
        if (effect_here) draw_curve(cr, &mix, x0, y0, width, height, curve_colors[CURVE_EFFECT], 2, page->curve == CURVE_EFFECT);
        draw_curve(cr, &volume, x0, y0, width, height, curve_colors[CURVE_VOLUME], 2.5, page->curve == CURVE_VOLUME);
        /* Pango with the registered Inter face: the cairo toy font API picks
         * one GDI face on Windows (Arial) and never sees the design font. */
        cairo_set_source_rgb(cr, .86, .91, .93); cairo_move_to(cr, x0 + 6, y0 + 14);
        radios_ui_cairo_show_text(cr, side ? "Giren parça" : "Çıkan parça", 12, PANGO_WEIGHT_NORMAL, FALSE);
    }
    if (page->play_source) {
        gdouble x0, y0, width, height; lane_geometry(area, 0, &x0, &y0, &width, &height);
        gdouble x = x0 + page->play_progress * width;
        cairo_set_source_rgb(cr, 1, 1, 1); cairo_set_line_width(cr, 1.5); cairo_move_to(cr, x, 4); cairo_line_to(cr, x, h - 4); cairo_stroke(cr);
    }
    return FALSE;
}
static void designer_redraw(AutomationPage *page)
{ if (page->designer) gtk_widget_queue_draw(page->designer); }
static gboolean designer_hit(AutomationPage *page, gdouble x, gdouble y, guint *side, RadioSAudioCurve *curve, gint *point, gdouble *t, gdouble *v)
{
    JsonObject *preset = current_preset(page); if (!preset) return FALSE;
    for (guint s = 0; s < 2; ++s) {
        gdouble x0, y0, width, height; lane_geometry(page->designer, s, &x0, &y0, &width, &height);
        if (y < y0 - 8 || y > y0 + height + 8) continue;
        *side = s; *t = CLAMP((x - x0) / width, 0, 1); *v = CLAMP(1 - (y - y0) / height, 0, 1); *point = -1;
        curve_load(curve_array(preset, s, page->curve), curve);
        gdouble best = 10;
        for (guint i = 0; i < curve->count; ++i) {
            gdouble dx = x0 + curve->t[i] * width - x, dy = y0 + (1 - curve->v[i]) * height - y, distance = sqrt(dx * dx + dy * dy);
            if (distance < best) { best = distance; *point = (gint)i; }
        }
        return TRUE;
    }
    return FALSE;
}
static gboolean curve_editable(AutomationPage *page, guint side)
{
    JsonObject *preset = current_preset(page);
    if (!preset) return FALSE;
    if (page->curve == CURVE_EFFECT) return effect_type(preset) >= 0 && effect_side(preset) == side;
    if (page->curve == CURVE_TONE) return tone_used(preset);
    return TRUE;
}
static gboolean designer_press(GtkWidget *area, GdkEventButton *event, gpointer data)
{
    (void)area; AutomationPage *page = data; guint side; RadioSAudioCurve curve; gint point; gdouble t, v;
    if (!designer_hit(page, event->x, event->y, &side, &curve, &point, &t, &v) || !curve_editable(page, side)) return FALSE;
    JsonObject *preset = current_preset(page);
    if (event->button == 3) {
        if (point > 0 && point + 1 < (gint)curve.count) {
            for (guint i = (guint)point; i + 1 < curve.count; ++i) { curve.t[i] = curve.t[i + 1]; curve.v[i] = curve.v[i + 1]; }
            --curve.count; curve_save(preset, side, page->curve, &curve); commit(page); designer_redraw(page);
        }
        return TRUE;
    }
    if (event->button != 1) return FALSE;
    if (point < 0 && event->type == GDK_2BUTTON_PRESS) {
        if (curve.count >= RADIOS_AUDIO_CURVE_POINTS) return TRUE;
        guint at = 1; while (at < curve.count && curve.t[at] < t) ++at;
        for (guint i = curve.count; i > at; --i) { curve.t[i] = curve.t[i - 1]; curve.v[i] = curve.v[i - 1]; }
        curve.t[at] = t; curve.v[at] = v; ++curve.count;
        curve_save(preset, side, page->curve, &curve); commit(page); designer_redraw(page); return TRUE;
    }
    if (point >= 0 && event->type == GDK_BUTTON_PRESS) { page->dragging = TRUE; page->drag_side = side; page->drag_point = (guint)point; }
    return TRUE;
}
static gboolean designer_motion(GtkWidget *area, GdkEventMotion *event, gpointer data)
{
    (void)area; AutomationPage *page = data; if (!page->dragging) return FALSE;
    JsonObject *preset = current_preset(page); if (!preset) { page->dragging = FALSE; return FALSE; }
    RadioSAudioCurve curve; curve_load(curve_array(preset, page->drag_side, page->curve), &curve);
    guint i = page->drag_point; if (i >= curve.count) { page->dragging = FALSE; return FALSE; }
    gdouble x0, y0, width, height; lane_geometry(page->designer, page->drag_side, &x0, &y0, &width, &height);
    gdouble t = CLAMP((event->x - x0) / width, 0, 1), v = CLAMP(1 - (event->y - y0) / height, 0, 1);
    if (i == 0) t = 0; else if (i + 1 == curve.count) t = 1; else t = CLAMP(t, curve.t[i - 1], curve.t[i + 1]);
    curve.t[i] = t; curve.v[i] = v; curve_save(preset, page->drag_side, page->curve, &curve); designer_redraw(page);
    return TRUE;
}
static gboolean designer_release(GtkWidget *area, GdkEventButton *event, gpointer data)
{
    (void)area; (void)event; AutomationPage *page = data;
    if (!page->dragging) return FALSE;
    page->dragging = FALSE; commit(page); return TRUE;
}
static void curve_selected(GtkToggleButton *button, gpointer data)
{
    AutomationPage *page = data; if (!gtk_toggle_button_get_active(button)) return;
    for (guint i = 0; i < CURVE_COUNT; ++i) if (GTK_WIDGET(button) == page->curve_buttons[i]) page->curve = i;
    designer_redraw(page);
}
static void shift_clicked(GtkButton *button, gpointer data)
{
    AutomationPage *page = data; JsonObject *preset = current_preset(page); if (!preset) return;
    guint side = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "side"));
    gdouble delta = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "direction")) * .05;
    if (!curve_editable(page, side)) return;
    RadioSAudioCurve curve; curve_load(curve_array(preset, side, page->curve), &curve);
    for (guint i = 1; i + 1 < curve.count; ++i) curve.t[i] = CLAMP(curve.t[i] + delta, 0, 1);
    curve_save(preset, side, page->curve, &curve); commit(page); designer_redraw(page);
}
static gboolean play_tick(gpointer data)
{
    AutomationPage *page = data; JsonObject *preset = current_preset(page);
    gdouble duration = preset ? MAX(500.0, (gdouble)radios_json_int(preset, "duration_ms", 6000)) : 6000;
    page->play_progress += 40.0 / duration;
    if (page->play_progress >= 1 || !preset) {
        page->play_progress = 0; page->play_source = 0;
        gtk_button_set_label(GTK_BUTTON(page->play), "▶"); designer_redraw(page); return G_SOURCE_REMOVE;
    }
    designer_redraw(page); return G_SOURCE_CONTINUE;
}
static void play_clicked(GtkButton *button, gpointer data)
{
    AutomationPage *page = data;
    if (page->play_source) { g_source_remove(page->play_source); page->play_source = 0; page->play_progress = 0; gtk_button_set_label(button, "▶"); }
    else if (current_preset(page)) { page->play_progress = 0; page->play_source = g_timeout_add(40, play_tick, page); gtk_button_set_label(button, "■"); }
    designer_redraw(page);
}

/* ---- preset fields ---------------------------------------------------- */
static void fields_render(AutomationPage *page)
{
    JsonObject *preset = current_preset(page); JsonObject *effect = object_member(preset, "effect");
    ++page->updating;
    show(page->design_box, preset != NULL);
    show(page->delete_button, preset != NULL);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(page->type), radios_transition_type_ids[radios_transition_preset_type(preset)]);
    gtk_entry_set_text(GTK_ENTRY(page->name), radios_json_string(preset, "name", ""));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(page->duration), (gdouble)radios_json_int(preset, "duration_ms", 6000));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->beat_sync), bool_member(preset, "beat_sync"));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(page->effect_type), radios_json_string(effect, "type", "none"));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(page->effect_depth), (gdouble)radios_json_int(effect, "depth", 50));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(page->effect_side), radios_json_string(effect, "side", "out"));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(page->effect_bpm), (gdouble)radios_json_int(effect, "bpm", 120));
    for (guint side = 0; side < 2; ++side)
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(page->tone[side]), radios_json_string(object_member(preset, side_keys[side]), "tone_kind", "none"));
    /* The type is the preset's structure: only its rows exist. An effect
     * brings its parameters and curve; a filter brings its curve. */
    gboolean effect_on = effect_type(preset) >= 0, tone_on = tone_used(preset);
    show(page->effect_row, effect_on);
    show(page->filter_row, tone_on);
    show(page->effect_params, effect_on);
    show(page->curve_buttons[CURVE_EFFECT], effect_on);
    show(page->curve_buttons[CURVE_TONE], tone_on);
    if ((!effect_on && page->curve == CURVE_EFFECT) || (!tone_on && page->curve == CURVE_TONE))
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->curve_buttons[CURVE_VOLUME]), TRUE);
    --page->updating; designer_redraw(page);
}
static void field_changed(GtkWidget *widget, gpointer data)
{
    AutomationPage *page = data; if (page->updating) return;
    JsonObject *preset = current_preset(page); if (!preset) return;
    JsonObject *effect = object_member(preset, "effect"); gboolean structural = FALSE;
    if (widget == page->name) json_object_set_string_member(preset, "name", gtk_entry_get_text(GTK_ENTRY(widget)));
    else if (widget == page->duration) json_object_set_int_member(preset, "duration_ms", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget)));
    else if (widget == page->beat_sync) json_object_set_boolean_member(preset, "beat_sync", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget)));
    else if (widget == page->type) {
        /* Choosing a type normalises the preset to that structure. */
        gint type = radios_transition_type_index(gtk_combo_box_get_active_id(GTK_COMBO_BOX(widget)));
        if (type >= 0) radios_transition_preset_set_type(preset, (RadioSTransitionType)type);
        structural = TRUE;
    }
    else if (widget == page->effect_type && effect) {
        const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(widget));
        json_object_set_string_member(effect, "type", id ? id : "none"); structural = TRUE;
    }
    else if (widget == page->effect_depth && effect) json_object_set_int_member(effect, "depth", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget)));
    else if (widget == page->effect_side && effect) { const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(widget)); json_object_set_string_member(effect, "side", id ? id : "out"); }
    else if (widget == page->effect_bpm && effect) json_object_set_int_member(effect, "bpm", gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(widget)));
    else for (guint side = 0; side < 2; ++side) if (widget == page->tone[side]) {
        const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(widget)); JsonObject *lane = object_member(preset, side_keys[side]);
        if (lane) json_object_set_string_member(lane, "tone_kind", id ? id : "none");
        structural = TRUE;
    }
    if (widget == page->name) { chips_render(page); kinds_render(page); schedule_render(page); schedule_form_render(page); }
    if (structural) fields_render(page);
    commit(page); designer_redraw(page);
}

/* ---- preset list ------------------------------------------------------- */
static void chip_toggled(GtkToggleButton *button, gpointer data)
{
    AutomationPage *page = data; if (page->updating || !gtk_toggle_button_get_active(button)) return;
    const gchar *id = g_object_get_data(G_OBJECT(button), "preset-id");
    g_free(page->selected); page->selected = g_strdup(id); fields_render(page);
}
static void chips_render(AutomationPage *page)
{
    ++page->updating;
    GList *children = gtk_container_get_children(GTK_CONTAINER(page->chips));
    for (GList *it = children; it; it = it->next) gtk_widget_destroy(it->data);
    g_list_free(children);
    GtkWidget *group = NULL;
    if (!current_preset(page)) { g_free(page->selected); page->selected = g_strdup(radios_json_string(preset_at(page, 0), "id", "")); }
    for (guint i = 0; i < preset_count(page); ++i) {
        JsonObject *preset = preset_at(page, i); if (!preset) continue;
        GtkWidget *chip = group ? gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(group), radios_json_string(preset, "name", "Geçiş")) :
            gtk_radio_button_new_with_label(NULL, radios_json_string(preset, "name", "Geçiş"));
        if (!group) group = chip;
        gchar *suffix = g_strdup_printf("chip-%u", i); named(chip, suffix); g_free(suffix);
        gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(chip), FALSE);
        gtk_style_context_add_class(gtk_widget_get_style_context(chip), "ra-chip");
        g_object_set_data_full(G_OBJECT(chip), "preset-id", g_strdup(radios_json_string(preset, "id", "")), g_free);
        if (!g_strcmp0(radios_json_string(preset, "id", ""), page->selected)) gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(chip), TRUE);
        g_signal_connect(chip, "toggled", G_CALLBACK(chip_toggled), page);
        gtk_box_pack_start(GTK_BOX(page->chips), chip, FALSE, FALSE, 0);
    }
    gtk_widget_show_all(page->chips);
    --page->updating;
}
static gchar *unique_name(AutomationPage *page, const gchar *base)
{
    for (guint n = 1; n < 1000; ++n) {
        gchar *candidate = n == 1 ? g_strdup(base) : g_strdup_printf("%s %u", base, n); gboolean taken = FALSE;
        for (guint i = 0; i < preset_count(page); ++i) taken |= !g_strcmp0(radios_json_string(preset_at(page, i), "name", ""), candidate);
        if (!taken) return candidate;
        g_free(candidate);
    }
    return g_strdup(base);
}
static void preset_add(AutomationPage *page, JsonObject *source, const gchar *base_name)
{
    if (!page->presets) page->presets = json_array_new();
    if (json_array_get_length(page->presets) >= RADIOS_TRANSITION_PRESET_LIMIT) {
        gtk_label_set_text(GTK_LABEL(page->status), "En fazla 32 geçiş ön ayarı tanımlanabilir."); return;
    }
    JsonObject *preset = copy_object(source); gchar *id = g_uuid_string_random(), *name = unique_name(page, base_name);
    json_object_set_string_member(preset, "id", id); json_object_set_string_member(preset, "name", name);
    json_array_add_object_element(page->presets, preset);
    g_free(page->selected); page->selected = id; g_free(name);
    chips_render(page); fields_render(page); kinds_render(page); schedule_form_render(page); commit(page);
}
static void template_clicked(GtkButton *button, gpointer data)
{
    AutomationPage *page = data; const gchar *name = g_object_get_data(G_OBJECT(button), "template-name");
    gtk_popover_popdown(GTK_POPOVER(page->add_popover));
    JsonObject *base = name ? radios_transition_preset_template(name) : NULL;
    if (base) { preset_add(page, base, name); json_object_unref(base); }
}
static void copy_clicked(GtkButton *button, gpointer data)
{
    (void)button; AutomationPage *page = data; gtk_popover_popdown(GTK_POPOVER(page->add_popover));
    JsonObject *preset = current_preset(page); if (!preset) return;
    gchar *base = g_strdup_printf("%s kopyası", radios_json_string(preset, "name", "Geçiş"));
    preset_add(page, preset, base); g_free(base);
}
static void references_remove(JsonArray *ids, const gchar *id)
{
    for (guint i = 0; ids && i < json_array_get_length(ids);) {
        if (!g_strcmp0(json_array_get_string_element(ids, i), id)) json_array_remove_element(ids, i); else ++i;
    }
}
static void delete_clicked(GtkButton *button, gpointer data)
{
    (void)button; AutomationPage *page = data; guint index = 0; JsonObject *preset = preset_by_id(page, page->selected, &index);
    if (!preset) return;
    gchar *id = g_strdup(page->selected);
    json_array_remove_element(page->presets, index);
    JsonObject *kinds = object_member(page->automation, "kinds");
    for (guint k = 0; k < KIND_COUNT; ++k) references_remove(array_member(object_member(kinds, radios_transition_kind_ids[k]), "presets"), id);
    JsonArray *schedule = array_member(page->automation, "schedule");
    for (guint i = 0; schedule && i < json_array_get_length(schedule); ++i) references_remove(array_member(json_array_get_object_element(schedule, i), "presets"), id);
    g_free(id); g_clear_pointer(&page->selected, g_free);
    chips_render(page); fields_render(page); kinds_render(page); schedule_render(page); schedule_form_render(page); commit(page);
}

/* ---- assignment by content kind --------------------------------------- */
static JsonObject *kind_entry(AutomationPage *page, const gchar *kind, gboolean create)
{
    JsonObject *kinds = object_member(page->automation, "kinds");
    if (!kinds && create) { kinds = json_object_new(); json_object_set_object_member(page->automation, "kinds", kinds); }
    JsonObject *entry = object_member(kinds, kind);
    if (!entry && create && kinds) {
        entry = json_object_new(); json_object_set_array_member(entry, "presets", json_array_new());
        json_object_set_string_member(entry, "rotation", "sequential"); json_object_set_object_member(kinds, kind, entry);
    }
    return entry;
}
static gboolean references_contain(JsonArray *ids, const gchar *id)
{
    for (guint i = 0; ids && i < json_array_get_length(ids); ++i) if (!g_strcmp0(json_array_get_string_element(ids, i), id)) return TRUE;
    return FALSE;
}
static void checks_fill(AutomationPage *page, GtkWidget *flow, JsonArray *selected, const gchar *prefix, GCallback callback)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(flow));
    for (GList *it = children; it; it = it->next) gtk_widget_destroy(it->data);
    g_list_free(children);
    for (guint i = 0; i < preset_count(page); ++i) {
        JsonObject *preset = preset_at(page, i); if (!preset) continue;
        GtkWidget *check = gtk_check_button_new_with_label(radios_json_string(preset, "name", "Geçiş"));
        gchar *suffix = g_strdup_printf("%s-%u", prefix, i); named(check, suffix); g_free(suffix);
        g_object_set_data_full(G_OBJECT(check), "preset-id", g_strdup(radios_json_string(preset, "id", "")), g_free);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(check), references_contain(selected, radios_json_string(preset, "id", "")));
        g_signal_connect(check, "toggled", callback, page);
        gtk_container_add(GTK_CONTAINER(flow), check);
    }
    gtk_widget_show_all(flow);
}
static JsonArray *checks_collect(GtkWidget *flow)
{
    JsonArray *ids = json_array_new(); GList *children = gtk_container_get_children(GTK_CONTAINER(flow));
    for (GList *it = children; it; it = it->next) {
        GtkWidget *child = gtk_bin_get_child(GTK_BIN(it->data));
        if (GTK_IS_TOGGLE_BUTTON(child) && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(child)))
            json_array_add_string_element(ids, g_object_get_data(G_OBJECT(child), "preset-id"));
    }
    g_list_free(children); return ids;
}
static guint checks_selected(GtkWidget *flow)
{ JsonArray *ids = checks_collect(flow); guint count = json_array_get_length(ids); json_array_unref(ids); return count; }
/* The kind's own rule and players stand in the same row (the parent's
 * widget); designed presets replace that rule for the kind once chosen. */
static void kind_dependents_render(AutomationPage *page, guint k)
{
    if (!page->kind_menus[k]) return;
    guint selected = checks_selected(page->kind_rows[k]);
    /* The order of several presets is a question only once there are several. */
    show(page->kind_menus[k], TRUE);
    show(page->kind_rotation[k], selected >= 2);
    gchar *caption = g_strdup_printf("%u ön ayar seçili ▾", selected);
    gtk_button_set_label(GTK_BUTTON(page->kind_menus[k]), caption); g_free(caption);
    gboolean ordered = FALSE;
    for (guint i = 0; i < KIND_COUNT; ++i) ordered |= page->kind_rotation[i] && gtk_widget_get_visible(page->kind_rotation[i]);
    show(page->kind_captions[0], TRUE); show(page->kind_captions[1], ordered);
}
static void kind_changed(GtkWidget *widget, gpointer data)
{
    AutomationPage *page = data; if (page->updating) return;
    for (guint k = 0; k < KIND_COUNT; ++k) {
        gboolean mine = widget == page->kind_rotation[k] || (GTK_IS_WIDGET(widget) && gtk_widget_is_ancestor(widget, page->kind_rows[k]));
        if (!mine) continue;
        JsonObject *entry = kind_entry(page, radios_transition_kind_ids[k], TRUE);
        json_object_set_array_member(entry, "presets", checks_collect(page->kind_rows[k]));
        const gchar *rotation = gtk_combo_box_get_active_id(GTK_COMBO_BOX(page->kind_rotation[k]));
        json_object_set_string_member(entry, "rotation", rotation ? rotation : "sequential");
        kind_dependents_render(page, k);
    }
    commit(page);
}
static void kinds_render(AutomationPage *page)
{
    /* With distributions each kind lists the presets as rows of its own. */
    for (guint k = 0; page->hooks.kind_distributions && k < KIND_COUNT; ++k)
        radios_transition_distribution_set_presets(page->distributions[k], page->presets, bool_member(page->automation, "enabled"));
    if (page->hooks.kind_distributions) return;
    ++page->updating;
    for (guint k = 0; k < KIND_COUNT; ++k) {
        JsonObject *entry = kind_entry(page, radios_transition_kind_ids[k], FALSE);
        gchar *prefix = g_strdup_printf("%s-preset", radios_transition_kind_ids[k]);
        checks_fill(page, page->kind_rows[k], array_member(entry, "presets"), prefix, G_CALLBACK(kind_changed)); g_free(prefix);
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(page->kind_rotation[k]), radios_json_string(entry, "rotation", "sequential"));
        kind_dependents_render(page, k);
    }
    --page->updating;
}
/* The probability window keeps its own page; the hook opens it, or the
 * rotation editor's own footer button does. */
static GtkWidget *descendant_named(GtkWidget *root, const gchar *name)
{
    if (!root) return NULL;
    if (!g_strcmp0(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *it = children; it && !found; it = it->next) found = descendant_named(it->data, name);
    g_list_free(children); return found;
}
static void probability_clicked(GtkButton *button, gpointer data)
{
    (void)button; AutomationPage *page = data;
    if (page->hooks.open_probability) { page->hooks.open_probability(page->root, page->hooks.user); return; }
    GtkWidget *footer = descendant_named(gtk_widget_get_toplevel(page->root), "rotation-editor-probability");
    if (GTK_IS_BUTTON(footer)) gtk_button_clicked(GTK_BUTTON(footer));
}
/* A kind's "Geçişler": an edit saves with the page; "Kalan pay" opens the
 * rotation's base distribution, which heads the probability window. */
static void distribution_edited(GtkWidget *widget, gpointer data)
{ (void)widget; commit(data); }
static void distribution_base(GtkWidget *widget, gpointer data)
{ (void)widget; probability_clicked(NULL, data); }
/* Brings "Türler" into view once the page has its new allocation. */
static gboolean kinds_reveal(gpointer data)
{
    GtkWidget *kinds = data, *scroll = gtk_widget_get_ancestor(kinds, GTK_TYPE_SCROLLED_WINDOW);
    GtkWidget *content = scroll ? gtk_bin_get_child(GTK_BIN(scroll)) : NULL; gint x = 0, y = 0;
    if (GTK_IS_VIEWPORT(content)) content = gtk_bin_get_child(GTK_BIN(content));
    if (content && gtk_widget_get_mapped(kinds) && gtk_widget_translate_coordinates(kinds, content, 0, 0, &x, &y))
        gtk_adjustment_set_value(gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll)), MAX(0, y - 48));
    g_object_unref(kinds); return G_SOURCE_REMOVE;
}
void radios_autodj_automation_show_kind_policy(GtkWidget *widget)
{
    AutomationPage *page = page_get(widget); if (!page) return;
    g_idle_add(kinds_reveal, g_object_ref(page->kinds_row));
}

/* ---- schedule ----------------------------------------------------------- */
static JsonArray *schedule_array(AutomationPage *page, gboolean create)
{
    JsonArray *schedule = array_member(page->automation, "schedule");
    if (!schedule && create) { schedule = json_array_new(); json_object_set_array_member(page->automation, "schedule", schedule); }
    return schedule;
}
static JsonObject *schedule_row(AutomationPage *page, const gchar *id, guint *index)
{
    JsonArray *schedule = schedule_array(page, FALSE);
    for (guint i = 0; id && schedule && i < json_array_get_length(schedule); ++i) {
        JsonObject *row = json_array_get_object_element(schedule, i);
        if (!g_strcmp0(radios_json_string(row, "id", ""), id)) { if (index) *index = i; return row; }
    }
    return NULL;
}
static gchar *minute_text(gint64 minute)
{ return g_strdup_printf("%02" G_GINT64_FORMAT ":%02" G_GINT64_FORMAT, minute / 60, minute % 60); }
static gboolean minute_parse(const gchar *text, gboolean ending, gint *minute)
{
    if (!text || strlen(text) != 5 || text[2] != ':') return FALSE;
    for (guint i = 0; i < 5; ++i) if (i != 2 && !g_ascii_isdigit(text[i])) return FALSE;
    gint hour = (text[0] - '0') * 10 + text[1] - '0', min = (text[3] - '0') * 10 + text[4] - '0';
    if (min > 59 || hour > (ending ? 24 : 23) || (hour == 24 && min != 0)) return FALSE;
    *minute = hour * 60 + min; return !ending || *minute > 0;
}
static gchar *row_days(JsonObject *row)
{
    JsonArray *days = array_member(row, "days"); GString *text = g_string_new(NULL);
    for (gint d = 1; d <= 7; ++d) {
        gboolean on = FALSE;
        for (guint i = 0; days && i < json_array_get_length(days); ++i) on |= json_array_get_int_element(days, i) == d;
        if (!on) continue;
        if (text->len) g_string_append(text, " ");
        g_string_append(text, day_labels[d - 1]);
    }
    return g_string_free(text, FALSE);
}
static gchar *row_presets(AutomationPage *page, JsonObject *row)
{
    JsonArray *ids = array_member(row, "presets"); GString *text = g_string_new(NULL);
    for (guint i = 0; ids && i < json_array_get_length(ids); ++i) {
        JsonObject *preset = preset_by_id(page, json_array_get_string_element(ids, i), NULL);
        if (!preset) continue;
        if (text->len) g_string_append(text, ", ");
        g_string_append(text, radios_json_string(preset, "name", "?"));
    }
    return g_string_free(text, FALSE);
}
static const gchar *kind_label(const gchar *kind)
{
    gint index = radios_transition_kind_index(kind);
    return index >= 0 ? radios_transition_kind_labels[index] : "Tüm içerik";
}
static void schedule_render(AutomationPage *page)
{
    ++page->updating; gtk_list_store_clear(page->schedule_store);
    JsonArray *schedule = schedule_array(page, FALSE); guint rows = schedule ? json_array_get_length(schedule) : 0;
    for (guint i = 0; i < rows; ++i) {
        JsonObject *row = json_array_get_object_element(schedule, i); GtkTreeIter iter;
        gchar *days = row_days(row), *start = minute_text(radios_json_int(row, "start_minute", 0)), *end = minute_text(radios_json_int(row, "end_minute", 1440));
        gchar *time = g_strdup_printf("%s–%s", start, end), *presets = row_presets(page, row);
        gtk_list_store_append(page->schedule_store, &iter);
        gtk_list_store_set(page->schedule_store, &iter, ROW_ID, radios_json_string(row, "id", ""), ROW_NAME, radios_json_string(row, "name", ""),
            ROW_DAYS, days, ROW_TIME, time, ROW_KIND, kind_label(radios_json_string(row, "kind", "all")), ROW_PRESETS, presets,
            ROW_ROTATION, !g_strcmp0(radios_json_string(row, "rotation", ""), "random") ? "Rastgele" : "Sırayla", -1);
        if (!g_strcmp0(radios_json_string(row, "id", ""), page->selected_row))
            gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(page->schedule_view)), &iter);
        g_free(days); g_free(start); g_free(end); g_free(time); g_free(presets);
    }
    /* The table exists once there is a row to show. */
    show(page->schedule_box, rows > 0);
    --page->updating;
}
static void schedule_checks_changed(GtkWidget *widget, gpointer data);
static void schedule_form_render(AutomationPage *page)
{
    JsonObject *row = schedule_row(page, page->selected_row, NULL);
    ++page->updating;
    gtk_entry_set_text(GTK_ENTRY(page->schedule_name), radios_json_string(row, "name", ""));
    JsonArray *days = array_member(row, "days");
    for (gint d = 1; d <= 7; ++d) {
        gboolean on = FALSE;
        for (guint i = 0; days && i < json_array_get_length(days); ++i) on |= json_array_get_int_element(days, i) == d;
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->schedule_days[d - 1]), on);
    }
    gchar *start = minute_text(radios_json_int(row, "start_minute", 0)), *end = minute_text(radios_json_int(row, "end_minute", 1440));
    gtk_entry_set_text(GTK_ENTRY(page->schedule_start), start); gtk_entry_set_text(GTK_ENTRY(page->schedule_end), end); g_free(start); g_free(end);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(page->schedule_kind), radios_json_string(row, "kind", "all"));
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(page->schedule_rotation), radios_json_string(row, "rotation", "sequential"));
    checks_fill(page, page->schedule_presets, array_member(row, "presets"), "schedule-preset", G_CALLBACK(schedule_checks_changed));
    /* The form belongs to a selected row; its order only to several presets. */
    show(page->schedule_form, row != NULL); show(page->schedule_remove, row != NULL);
    show(page->schedule_rotation_box, row && checks_selected(page->schedule_presets) >= 2);
    --page->updating;
}
static void schedule_selected(GtkTreeSelection *selection, gpointer data)
{
    AutomationPage *page = data; if (page->updating) return;
    GtkTreeModel *model; GtkTreeIter iter; g_clear_pointer(&page->selected_row, g_free);
    if (gtk_tree_selection_get_selected(selection, &model, &iter)) gtk_tree_model_get(model, &iter, ROW_ID, &page->selected_row, -1);
    schedule_form_render(page);
}
static void schedule_store_form(AutomationPage *page, GtkWidget *widget)
{
    JsonObject *row = schedule_row(page, page->selected_row, NULL); if (!row) return;
    if (widget == page->schedule_name) json_object_set_string_member(row, "name", gtk_entry_get_text(GTK_ENTRY(widget)));
    if (widget == page->schedule_start || widget == page->schedule_end) {
        gint start = 0, end = 0;
        if (!minute_parse(gtk_entry_get_text(GTK_ENTRY(page->schedule_start)), FALSE, &start) ||
            !minute_parse(gtk_entry_get_text(GTK_ENTRY(page->schedule_end)), TRUE, &end) || start == end) {
            gtk_label_set_text(GTK_LABEL(page->status), "Saat dilimi SS:DD biçiminde olmalı; bitiş başlangıçtan farklı olmalı."); return;
        }
        json_object_set_int_member(row, "start_minute", start); json_object_set_int_member(row, "end_minute", end);
    }
    JsonArray *days = json_array_new();
    for (gint d = 1; d <= 7; ++d) if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(page->schedule_days[d - 1]))) json_array_add_int_element(days, d);
    json_object_set_array_member(row, "days", days);
    const gchar *kind = gtk_combo_box_get_active_id(GTK_COMBO_BOX(page->schedule_kind)), *rotation = gtk_combo_box_get_active_id(GTK_COMBO_BOX(page->schedule_rotation));
    json_object_set_string_member(row, "kind", kind ? kind : "all"); json_object_set_string_member(row, "rotation", rotation ? rotation : "sequential");
    json_object_set_array_member(row, "presets", checks_collect(page->schedule_presets));
    show(page->schedule_rotation_box, checks_selected(page->schedule_presets) >= 2);
    schedule_render(page); commit(page);
}
static void schedule_field_changed(GtkWidget *widget, gpointer data)
{ AutomationPage *page = data; if (!page->updating) schedule_store_form(page, widget); }
static void schedule_checks_changed(GtkWidget *widget, gpointer data)
{ AutomationPage *page = data; if (!page->updating) schedule_store_form(page, widget); }
static void schedule_add_clicked(GtkButton *button, gpointer data)
{
    (void)button; AutomationPage *page = data; JsonArray *schedule = schedule_array(page, TRUE);
    if (json_array_get_length(schedule) >= RADIOS_TRANSITION_SCHEDULE_LIMIT) { gtk_label_set_text(GTK_LABEL(page->status), "En fazla 32 saat dilimi tanımlanabilir."); return; }
    JsonObject *row = json_object_new(); gchar *id = g_uuid_string_random();
    json_object_set_string_member(row, "id", id);
    gchar *name = g_strdup_printf("Saat dilimi %u", json_array_get_length(schedule) + 1); json_object_set_string_member(row, "name", name); g_free(name);
    JsonArray *days = json_array_new(); for (gint d = 1; d <= 7; ++d) json_array_add_int_element(days, d); json_object_set_array_member(row, "days", days);
    json_object_set_int_member(row, "start_minute", 0); json_object_set_int_member(row, "end_minute", 1440);
    json_object_set_string_member(row, "kind", "all"); json_object_set_string_member(row, "rotation", "sequential");
    JsonArray *presets = json_array_new(); if (current_preset(page)) json_array_add_string_element(presets, page->selected);
    json_object_set_array_member(row, "presets", presets);
    json_array_add_object_element(schedule, row);
    g_free(page->selected_row); page->selected_row = id;
    schedule_render(page); schedule_form_render(page); commit(page);
}
static void schedule_remove_clicked(GtkButton *button, gpointer data)
{
    (void)button; AutomationPage *page = data; guint index = 0;
    if (!schedule_row(page, page->selected_row, &index)) return;
    json_array_remove_element(schedule_array(page, TRUE), index); g_clear_pointer(&page->selected_row, g_free);
    schedule_render(page); schedule_form_render(page); commit(page);
}

/* ---- validity and commit ------------------------------------------------ */
static gchar *page_problem(AutomationPage *page)
{
    /* The kinds' entries leave room for the base's four in settings.transitions (64). */
    guint entries = 0;
    for (guint k = 0; page->hooks.kind_distributions && k < KIND_COUNT; ++k) {
        gchar *message = NULL;
        if (!radios_transition_distribution_valid(page->distributions[k], &message)) return message;
        entries += radios_transition_distribution_count(page->distributions[k]);
    }
    if (entries > 60) return g_strdup("Türlerin geçişleri birlikte en çok 60 satır olabilir.");
    for (guint i = 0; i < preset_count(page); ++i) {
        JsonObject *preset = preset_at(page, i);
        if (!radios_transition_preset_valid(preset, TRUE)) {
            const gchar *name = radios_json_string(preset, "name", ""); gchar *trimmed = g_strdup(name); g_strstrip(trimmed); gboolean blank = !*trimmed; g_free(trimmed);
            return blank || g_utf8_strlen(name, -1) > 60 ? g_strdup("AutoDJ otomasyonu: ön ayar adı boş olamaz; en fazla 60 karakter kullanın.") :
                g_strdup_printf("AutoDJ otomasyonu: “%s” ön ayarının eğrileri veya efekti geçersiz.", name);
        }
    }
    if (!radios_transition_presets_valid(page->presets ? page->presets : (page->presets = json_array_new())))
        return g_strdup("AutoDJ otomasyonu: en fazla 32 ön ayar tanımlanabilir ve adlar benzersiz kimlik taşımalı.");
    JsonArray *schedule = schedule_array(page, FALSE);
    for (guint i = 0; schedule && i < json_array_get_length(schedule); ++i) {
        JsonObject *row = json_array_get_object_element(schedule, i); JsonArray *presets = array_member(row, "presets"), *days = array_member(row, "days");
        if (!presets || !json_array_get_length(presets)) return g_strdup_printf("AutoDJ otomasyonu: “%s” saat dilimi için en az bir ön ayar seçin.", radios_json_string(row, "name", ""));
        if (!days || !json_array_get_length(days)) return g_strdup_printf("AutoDJ otomasyonu: “%s” saat dilimi için en az bir gün seçin.", radios_json_string(row, "name", ""));
        gchar *trimmed = g_strdup(radios_json_string(row, "name", "")); g_strstrip(trimmed); gboolean blank = !*trimmed; g_free(trimmed);
        if (blank) return g_strdup("AutoDJ otomasyonu: saat dilimi adı boş olamaz.");
    }
    if (!radios_transition_automation_valid(page->automation, page->presets)) return g_strdup("AutoDJ otomasyonu: atama veya saat dilimi ayarları geçersiz.");
    return NULL;
}
static void commit(AutomationPage *page)
{
    if (page->updating) return;
    page->touched = TRUE;
    gchar *problem = page_problem(page);
    gtk_label_set_text(GTK_LABEL(page->status), problem ? problem : "");
    g_free(problem);
    if (page->hooks.changed) page->hooks.changed(page->root, page->hooks.user);
}
/* The page never looks empty: "Türler", the presets and the designer are
 * always shown; the switch only decides whether the presets act on air. */
static void designed_render(AutomationPage *page)
{
    show(page->body, TRUE);
    for (guint k = 0; k < KIND_COUNT; ++k) kind_dependents_render(page, k);
}
static void enabled_toggled(GtkWidget *widget, gpointer data)
{
    AutomationPage *page = data; if (page->updating) return;
    gboolean on = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    json_object_set_boolean_member(page->automation, "enabled", on);
    designed_render(page); if (page->hooks.kind_distributions) kinds_render(page);
    commit(page);
}
/* The preview timer touches the designer and the play button: stop it when
 * the page is destroyed, not only when the last reference to the page goes
 * (a referenced page outlives its destroyed children). */
static void page_destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget; AutomationPage *page = data;
    if (page->play_source) { g_source_remove(page->play_source); page->play_source = 0; }
    page->play_progress = 0; page->dragging = FALSE;
}
static void page_free(gpointer data)
{
    AutomationPage *page = data;
    if (page->play_source) g_source_remove(page->play_source);
    if (page->presets) json_array_unref(page->presets);
    if (page->automation) json_object_unref(page->automation);
    g_clear_object(&page->schedule_store);
    g_free(page->selected); g_free(page->selected_row); g_free(page);
}

/* ---- construction ------------------------------------------------------- */
static GtkWidget *combo_new(const gchar *suffix, const gchar *const *ids, const gchar *const *labels, guint count, AutomationPage *page, GCallback callback)
{
    GtkWidget *combo = named(gtk_combo_box_text_new(), suffix);
    for (guint i = 0; i < count; ++i) gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), ids[i], labels[i]);
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0);
    g_signal_connect(combo, "changed", callback, page); return combo;
}
static GtkWidget *spin_new(const gchar *suffix, gdouble low, gdouble high, gdouble step, AutomationPage *page, GCallback callback)
{
    GtkWidget *spin = named(gtk_spin_button_new_with_range(low, high, step), suffix);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(spin), TRUE); gtk_entry_set_width_chars(GTK_ENTRY(spin), 6);
    g_signal_connect(spin, "value-changed", callback, page); return spin;
}
static GtkWidget *flow_new(const gchar *suffix)
{
    GtkWidget *flow = named(gtk_flow_box_new(), suffix);
    gtk_flow_box_set_selection_mode(GTK_FLOW_BOX(flow), GTK_SELECTION_NONE);
    gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(flow), 8); gtk_flow_box_set_homogeneous(GTK_FLOW_BOX(flow), FALSE);
    gtk_flow_box_set_column_spacing(GTK_FLOW_BOX(flow), 4); gtk_flow_box_set_row_spacing(GTK_FLOW_BOX(flow), 2);
    gtk_widget_set_hexpand(flow, TRUE); return flow;
}
static GtkWidget *field(GtkWidget *grid, const gchar *text, GtkWidget *widget, gint column, gint row)
{
    gtk_grid_attach(GTK_GRID(grid), caption(text, "re-caption"), column * 2, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), widget, column * 2 + 1, row, 1, 1); return widget;
}
static GtkWidget *menu_item(AutomationPage *page, GtkWidget *box, const gchar *text, const gchar *suffix, GCallback callback)
{
    GtkWidget *button = named(gtk_button_new_with_label(text), suffix);
    gtk_button_set_relief(GTK_BUTTON(button), GTK_RELIEF_NONE); gtk_widget_set_halign(gtk_bin_get_child(GTK_BIN(button)), GTK_ALIGN_START);
    g_signal_connect(button, "clicked", callback, page);
    gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0); return button;
}
static void automation_style(void)
{
    static gboolean installed = FALSE; if (installed) return; installed = TRUE;
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider,
        "#rotation-editor radiobutton.ra-chip { padding: 4px 12px; border-radius: 16px; background-color: #263e49; border: 1px solid #45606d; color: #e7f2f7; }"
        "#rotation-editor radiobutton.ra-chip:checked { background-color: #197e83; border-color: #32b5b2; color: #ffffff; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}
static void table_column(GtkWidget *tree, const gchar *title, gint column, gboolean expand)
{
    GtkCellRenderer *cell = gtk_cell_renderer_text_new(); g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, "ypad", 5u, NULL);
    GtkTreeViewColumn *view = gtk_tree_view_column_new_with_attributes(title, cell, "text", column, NULL);
    gtk_tree_view_column_set_expand(view, expand); gtk_tree_view_column_set_min_width(view, expand ? 120 : 60);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), view);
}
/* The parent's rule (its switch, then a grid with the header in row 0 and
 * one row per kind) shares heights with this page's column row by row. */
static void rows_align(GtkWidget *widget, GtkSizeGroup *switches, GtkSizeGroup *header, GtkSizeGroup *rows)
{
    if (GTK_IS_GRID(widget)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *it = children; it; it = it->next) {
            gint top = 0; gtk_container_child_get(GTK_CONTAINER(widget), it->data, "top-attach", &top, NULL);
            gtk_size_group_add_widget(top ? rows : header, it->data);
            /* Long choice names ("SYNC beatmatch (BPM ve vuruş eşleştirerek)")
             * and row notes are ellipsized on one line, so one row of every
             * column fits the editor and the rows keep one height. */
            gint left = 0; gtk_container_child_get(GTK_CONTAINER(widget), it->data, "left-attach", &left, NULL);
            if (GTK_IS_LABEL(it->data) && top && left) {
                gtk_label_set_ellipsize(GTK_LABEL(it->data), PANGO_ELLIPSIZE_END); gtk_label_set_max_width_chars(GTK_LABEL(it->data), 20);
            }
            if (!GTK_IS_COMBO_BOX(it->data)) continue;
            gtk_widget_set_size_request(it->data, 190, -1);
            GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(it->data));
            for (GList *cell = cells; cell; cell = cell->next)
                if (GTK_IS_CELL_RENDERER_TEXT(cell->data)) g_object_set(cell->data, "ellipsize", PANGO_ELLIPSIZE_END, "max-width-chars", 18, NULL);
            g_list_free(cells);
        }
        g_list_free(children); return;
    }
    if (GTK_IS_CHECK_BUTTON(widget)) { gtk_size_group_add_widget(switches, widget); return; }
    if (!GTK_IS_CONTAINER(widget)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (GList *it = children; it; it = it->next) rows_align(it->data, switches, header, rows);
    g_list_free(children);
}
GtkWidget *radios_autodj_automation_new(RadioSViewContext *ctx, const RadioSAutodjAutomationHooks *hooks)
{
    automation_style();
    AutomationPage *page = g_new0(AutomationPage, 1); page->ctx = ctx; if (hooks) page->hooks = *hooks;
    page->presets = radios_transition_preset_templates();
    page->automation = json_object_new(); json_object_set_boolean_member(page->automation, "enabled", FALSE);
    json_object_set_object_member(page->automation, "kinds", json_object_new());
    page->root = named(radios_ui_box(GTK_ORIENTATION_VERTICAL, 10, NULL), "page");
    /* 1. "Türler": one row per content kind. The parent's rule and players
     * on the left, the designed presets on the right, aligned with those rows
     * (one height for the switches, the header row and the kind rows). */
    GtkWidget *kinds_head = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL);
    gtk_box_pack_start(GTK_BOX(kinds_head), caption("Türler", "re-heading"), TRUE, TRUE, 0);
    page->probability_button = named(radios_ui_button("Olasılık ve geçişler…"), "probability-open");
    g_signal_connect(page->probability_button, "clicked", G_CALLBACK(probability_clicked), page);
    gtk_box_pack_end(GTK_BOX(kinds_head), page->probability_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page->root), kinds_head, FALSE, FALSE, 0);
    page->kinds_row = named(radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL), "kinds");
    gtk_box_pack_start(GTK_BOX(page->root), page->kinds_row, FALSE, FALSE, 0);
    GtkSizeGroup *switches = gtk_size_group_new(GTK_SIZE_GROUP_VERTICAL), *header = gtk_size_group_new(GTK_SIZE_GROUP_VERTICAL);
    GtkSizeGroup *rows = gtk_size_group_new(GTK_SIZE_GROUP_VERTICAL);
    page->policy = page->hooks.kind_policy ? page->hooks.kind_policy(page->root, page->hooks.user) : NULL;
    if (page->policy) {
        gtk_box_pack_start(GTK_BOX(page->kinds_row), page->policy, FALSE, FALSE, 0);
        rows_align(page->policy, switches, header, rows);
    }
    GtkWidget *column = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, NULL); gtk_container_set_border_width(GTK_CONTAINER(column), 8);
    gtk_box_pack_start(GTK_BOX(page->kinds_row), column, TRUE, TRUE, 0);
    /* The switch: whether the designed presets act on air. */
    page->enabled = named(gtk_check_button_new_with_label("Tasarlanmış geçişler"), "enabled");
    g_signal_connect(page->enabled, "toggled", G_CALLBACK(enabled_toggled), page);
    gtk_size_group_add_widget(switches, page->enabled);
    gtk_box_pack_start(GTK_BOX(column), page->enabled, FALSE, FALSE, 0);
    GtkWidget *kinds = named(gtk_grid_new(), "kind-presets");
    gtk_grid_set_row_spacing(GTK_GRID(kinds), 6); gtk_grid_set_column_spacing(GTK_GRID(kinds), 8);
    gint first = page->policy ? 0 : 1;
    const gchar *heads[] = {"Ön ayarlar", "Sıra"}, *distribution_head[] = {"Geçişler"};
    const gchar *const *columns = page->hooks.kind_distributions ? distribution_head : heads;
    for (guint i = 0; i < (page->hooks.kind_distributions ? 1u : 2u); ++i) {
        GtkWidget *head = page->kind_captions[i] = caption(columns[i], "re-caption"); gtk_size_group_add_widget(header, head);
        gtk_grid_attach(GTK_GRID(kinds), head, first + (gint)i, 0, 1, 1);
    }
    RadioSTransitionDistributionHooks distribution_hooks = {distribution_edited, page, distribution_base};
    for (guint k = 0; k < KIND_COUNT; ++k) {
        gint row = (gint)k + 1;
        if (!page->policy) {
            GtkWidget *name = caption(radios_transition_kind_labels[k], "re-caption");
            gtk_size_group_add_widget(rows, name); gtk_grid_attach(GTK_GRID(kinds), name, 0, row, 1, 1);
        }
        if (page->hooks.kind_distributions) {
            /* The kind's transitions: types and designed presets with shares. */
            page->distributions[k] = radios_transition_distribution_new(radios_transition_kind_ids[k], TRUE, &distribution_hooks);
            gtk_size_group_add_widget(rows, page->distributions[k]);
            gtk_grid_attach(GTK_GRID(kinds), page->distributions[k], first, row, 1, 1);
            continue;
        }
        gchar *suffix = g_strdup_printf("%s-presets-open", radios_transition_kind_ids[k]);
        page->kind_menus[k] = named(gtk_menu_button_new(), suffix); g_free(suffix);
        GtkWidget *popover = gtk_popover_new(page->kind_menus[k]);
        gtk_menu_button_set_popover(GTK_MENU_BUTTON(page->kind_menus[k]), popover);
        suffix = g_strdup_printf("%s-presets", radios_transition_kind_ids[k]);
        page->kind_rows[k] = flow_new(suffix); g_free(suffix);
        gtk_flow_box_set_max_children_per_line(GTK_FLOW_BOX(page->kind_rows[k]), 1);
        gtk_container_set_border_width(GTK_CONTAINER(page->kind_rows[k]), 6);
        gtk_container_add(GTK_CONTAINER(popover), page->kind_rows[k]);
        gtk_size_group_add_widget(rows, page->kind_menus[k]);
        gtk_grid_attach(GTK_GRID(kinds), page->kind_menus[k], first, row, 1, 1);
        suffix = g_strdup_printf("%s-rotation", radios_transition_kind_ids[k]);
        page->kind_rotation[k] = combo_new(suffix, rotation_ids, rotation_labels, 2, page, G_CALLBACK(kind_changed)); g_free(suffix);
        gtk_size_group_add_widget(rows, page->kind_rotation[k]);
        gtk_grid_attach(GTK_GRID(kinds), page->kind_rotation[k], first + 1, row, 1, 1);
    }
    gtk_box_pack_start(GTK_BOX(column), kinds, FALSE, FALSE, 0);
    g_object_unref(switches); g_object_unref(header); g_object_unref(rows);
    page->body = named(radios_ui_box(GTK_ORIENTATION_VERTICAL, 10, NULL), "body");
    gtk_box_pack_start(GTK_BOX(page->root), page->body, TRUE, TRUE, 0);

    /* 3. The presets themselves: pick one, and its fields follow. */
    GtkWidget *preset_head = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL);
    gtk_box_pack_start(GTK_BOX(preset_head), caption("Ön ayarlar", "re-heading"), TRUE, TRUE, 0);
    GtkWidget *add = named(gtk_menu_button_new(), "add"); gtk_button_set_label(GTK_BUTTON(add), "Ekle ▾");
    page->add_popover = gtk_popover_new(add); GtkWidget *add_items = radios_ui_box(GTK_ORIENTATION_VERTICAL, 2, NULL);
    gtk_container_set_border_width(GTK_CONTAINER(add_items), 6); gtk_container_add(GTK_CONTAINER(page->add_popover), add_items);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(add), page->add_popover);
    JsonArray *templates = radios_transition_preset_templates();
    for (guint i = 0; i < json_array_get_length(templates); ++i) {
        const gchar *name = radios_json_string(json_array_get_object_element(templates, i), "name", "");
        gchar *suffix = g_strdup_printf("add-%u", i);
        GtkWidget *item = menu_item(page, add_items, name, suffix, G_CALLBACK(template_clicked)); g_free(suffix);
        g_object_set_data_full(G_OBJECT(item), "template-name", g_strdup(name), g_free);
    }
    json_array_unref(templates);
    gtk_box_pack_start(GTK_BOX(add_items), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL), FALSE, FALSE, 2);
    menu_item(page, add_items, "Seçilinin kopyası", "copy", G_CALLBACK(copy_clicked));
    gtk_widget_show_all(add_items);
    page->delete_button = named(radios_ui_button("Sil"), "delete"); g_signal_connect(page->delete_button, "clicked", G_CALLBACK(delete_clicked), page);
    gtk_box_pack_end(GTK_BOX(preset_head), page->delete_button, FALSE, FALSE, 0); gtk_box_pack_end(GTK_BOX(preset_head), add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page->body), preset_head, FALSE, FALSE, 0);
    GtkWidget *chip_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(chip_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    page->chips = named(radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL), "chips");
    gtk_container_add(GTK_CONTAINER(chip_scroll), page->chips); gtk_box_pack_start(GTK_BOX(page->body), chip_scroll, FALSE, FALSE, 0);

    page->design_box = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, NULL);
    gtk_box_pack_start(GTK_BOX(page->body), page->design_box, TRUE, TRUE, 0);
    /* The transition type comes first; length and beat alignment belong to
     * every type, the effect row and the filter row only to theirs. */
    GtkWidget *fields = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(fields), 6); gtk_grid_set_column_spacing(GTK_GRID(fields), 8);
    page->type = combo_new("type", radios_transition_type_ids, radios_transition_type_labels, RADIOS_TRANSITION_TYPE_COUNT, page, G_CALLBACK(field_changed));
    page->name = named(radios_ui_entry("Ad"), "name"); gtk_entry_set_max_length(GTK_ENTRY(page->name), 60); gtk_widget_set_hexpand(page->name, TRUE);
    g_signal_connect(page->name, "changed", G_CALLBACK(field_changed), page);
    page->duration = spin_new("duration", 500, 30000, 100, page, G_CALLBACK(field_changed));
    page->beat_sync = named(gtk_check_button_new_with_label("SYNC beatmatch"), "beat-sync");
    g_signal_connect(page->beat_sync, "toggled", G_CALLBACK(field_changed), page);
    field(fields, "Geçiş türü", page->type, 0, 0); field(fields, "Ad", page->name, 1, 0);
    field(fields, "Süre (ms)", page->duration, 0, 1);
    gtk_grid_attach(GTK_GRID(fields), page->beat_sync, 2, 1, 2, 1);
    gtk_box_pack_start(GTK_BOX(page->design_box), fields, FALSE, FALSE, 0);
    /* Effect row of the effect types. An effect brings its depth, side and
     * tempo; "Efekt yok" leaves the type, and nothing of the effect remains. */
    page->effect_row = named(radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL), "effect-row");
    const gchar *effect_ids[RADIOS_FX_COUNT + 1], *effect_labels[RADIOS_FX_COUNT + 1]; effect_ids[0] = "none"; effect_labels[0] = "Efekt yok";
    for (guint i = 0; i < RADIOS_FX_COUNT; ++i) { effect_ids[i + 1] = radios_transition_effect_id((gint)i); effect_labels[i + 1] = radios_audio_fx_name((RadioSAudioFxType)i); }
    page->effect_type = combo_new("effect", effect_ids, effect_labels, RADIOS_FX_COUNT + 1, page, G_CALLBACK(field_changed));
    gtk_box_pack_start(GTK_BOX(page->effect_row), caption("Efekt", "re-caption"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page->effect_row), page->effect_type, FALSE, FALSE, 0);
    page->effect_params = named(radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL), "effect-params");
    page->effect_depth = spin_new("effect-depth", 0, 100, 5, page, G_CALLBACK(field_changed));
    const gchar *side_ids[] = {"out", "in"}, *side_labels[] = {"Çıkan parçaya", "Giren parçaya"};
    page->effect_side = combo_new("effect-side", side_ids, side_labels, 2, page, G_CALLBACK(field_changed));
    page->effect_bpm = spin_new("effect-bpm", 30, 300, 1, page, G_CALLBACK(field_changed));
    gtk_box_pack_start(GTK_BOX(page->effect_params), caption("Derinlik (%)", "re-caption"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page->effect_params), page->effect_depth, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page->effect_params), page->effect_side, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page->effect_params), caption("BPM", "re-caption"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page->effect_params), page->effect_bpm, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page->effect_row), page->effect_params, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page->design_box), page->effect_row, FALSE, FALSE, 0);
    /* Filter row of the filter types: one filter per side; "Filtre yok" on both sides leaves the type. */
    page->filter_row = named(radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL), "filter-row");
    for (guint side = 0; side < 2; ++side) {
        page->tone[side] = combo_new(side ? "tone-in" : "tone-out", tone_ids, tone_labels, 3, page, G_CALLBACK(field_changed));
        gtk_box_pack_start(GTK_BOX(page->filter_row), caption(side ? "Giren filtre" : "Çıkan filtre", "re-caption"), FALSE, FALSE, 0);
        gtk_box_pack_start(GTK_BOX(page->filter_row), page->tone[side], FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(page->design_box), page->filter_row, FALSE, FALSE, 0);
    /* Curves: the colour of each choice is the colour of its line. */
    GtkWidget *curve_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL);
    GtkWidget *group = NULL;
    for (guint i = 0; i < CURVE_COUNT; ++i) {
        GtkWidget *button = group ? gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(group), curve_labels[i]) : gtk_radio_button_new_with_label(NULL, curve_labels[i]);
        if (!group) group = button;
        gchar *markup = g_strdup_printf("<span foreground=\"%s\" weight=\"bold\">%s</span>", curve_colors[i], curve_labels[i]);
        gtk_label_set_markup(GTK_LABEL(gtk_bin_get_child(GTK_BIN(button))), markup); g_free(markup);
        gchar *suffix = g_strdup_printf("curve-%s", curve_keys[i]); named(button, suffix); g_free(suffix);
        page->curve_buttons[i] = button; g_signal_connect(button, "toggled", G_CALLBACK(curve_selected), page);
        gtk_box_pack_start(GTK_BOX(curve_row), button, FALSE, FALSE, 0);
    }
    page->play = named(radios_ui_button("▶"), "play"); g_signal_connect(page->play, "clicked", G_CALLBACK(play_clicked), page);
    gtk_widget_set_tooltip_text(page->play, "Eğrileri gerçek süresinde oynat");
    gtk_box_pack_end(GTK_BOX(curve_row), page->play, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page->design_box), curve_row, FALSE, FALSE, 0);
    GtkWidget *designer_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 4, NULL);
    GtkWidget *left = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, NULL), *right = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, NULL);
    for (guint side = 0; side < 2; ++side) for (guint direction = 0; direction < 2; ++direction) {
        GtkWidget *button = named(radios_ui_button(direction ? "›" : "‹"), side ? (direction ? "shift-in-later" : "shift-in-earlier") : (direction ? "shift-out-later" : "shift-out-earlier"));
        g_object_set_data(G_OBJECT(button), "side", GUINT_TO_POINTER(side)); g_object_set_data(G_OBJECT(button), "direction", GINT_TO_POINTER(direction ? 1 : -1));
        gtk_widget_set_tooltip_text(button, direction ? "Ara noktaları sona kaydır" : "Ara noktaları başa kaydır");
        g_signal_connect(button, "clicked", G_CALLBACK(shift_clicked), page); page->shift[side][direction] = button;
        gtk_box_pack_start(GTK_BOX(direction ? right : left), button, TRUE, TRUE, 0);
    }
    page->designer = named(gtk_drawing_area_new(), "designer");
    gtk_widget_set_size_request(page->designer, 520, 250); gtk_widget_set_hexpand(page->designer, TRUE);
    gtk_widget_set_tooltip_text(page->designer, "Noktayı sürükle · boş yere çift tık: nokta ekle · sağ tık: nokta sil");
    gtk_widget_add_events(page->designer, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    g_signal_connect(page->designer, "draw", G_CALLBACK(designer_draw), page);
    g_signal_connect(page->designer, "button-press-event", G_CALLBACK(designer_press), page);
    g_signal_connect(page->designer, "motion-notify-event", G_CALLBACK(designer_motion), page);
    g_signal_connect(page->designer, "button-release-event", G_CALLBACK(designer_release), page);
    gtk_box_pack_start(GTK_BOX(designer_row), left, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(designer_row), page->designer, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(designer_row), right, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page->design_box), designer_row, TRUE, TRUE, 0);

    /* 4. Clock windows: one button; the table and form appear with the first row. */
    GtkWidget *schedule_head = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL);
    gtk_box_pack_start(GTK_BOX(schedule_head), caption("Saat dilimleri", "re-heading"), TRUE, TRUE, 0);
    GtkWidget *schedule_add = named(radios_ui_button("Saat dilimi ekle"), "schedule-add"); g_signal_connect(schedule_add, "clicked", G_CALLBACK(schedule_add_clicked), page);
    page->schedule_remove = named(radios_ui_button("Sil"), "schedule-remove"); g_signal_connect(page->schedule_remove, "clicked", G_CALLBACK(schedule_remove_clicked), page);
    gtk_box_pack_end(GTK_BOX(schedule_head), page->schedule_remove, FALSE, FALSE, 0); gtk_box_pack_end(GTK_BOX(schedule_head), schedule_add, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page->body), schedule_head, FALSE, FALSE, 0);
    page->schedule_box = named(radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, NULL), "schedule-box");
    gtk_box_pack_start(GTK_BOX(page->body), page->schedule_box, FALSE, FALSE, 0);
    page->schedule_store = gtk_list_store_new(ROW_COLUMNS, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    page->schedule_view = named(gtk_tree_view_new_with_model(GTK_TREE_MODEL(page->schedule_store)), "schedule");
    table_column(page->schedule_view, "Ad", ROW_NAME, TRUE); table_column(page->schedule_view, "Günler", ROW_DAYS, FALSE);
    table_column(page->schedule_view, "Saat", ROW_TIME, FALSE); table_column(page->schedule_view, "İçerik", ROW_KIND, FALSE);
    table_column(page->schedule_view, "Ön ayarlar", ROW_PRESETS, TRUE); table_column(page->schedule_view, "Sıra", ROW_ROTATION, FALSE);
    g_signal_connect(gtk_tree_view_get_selection(GTK_TREE_VIEW(page->schedule_view)), "changed", G_CALLBACK(schedule_selected), page);
    GtkWidget *schedule_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(schedule_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(schedule_scroll, -1, 110); gtk_container_add(GTK_CONTAINER(schedule_scroll), page->schedule_view);
    gtk_box_pack_start(GTK_BOX(page->schedule_box), schedule_scroll, FALSE, FALSE, 0);
    page->schedule_form = named(gtk_grid_new(), "schedule-form"); gtk_grid_set_row_spacing(GTK_GRID(page->schedule_form), 6); gtk_grid_set_column_spacing(GTK_GRID(page->schedule_form), 8);
    page->schedule_name = named(radios_ui_entry("Ad"), "schedule-name"); gtk_entry_set_max_length(GTK_ENTRY(page->schedule_name), 60);
    g_signal_connect(page->schedule_name, "changed", G_CALLBACK(schedule_field_changed), page);
    gtk_grid_attach(GTK_GRID(page->schedule_form), caption("Ad", "re-caption"), 0, 0, 1, 1); gtk_grid_attach(GTK_GRID(page->schedule_form), page->schedule_name, 1, 0, 3, 1);
    GtkWidget *days_box = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 2, NULL);
    for (guint d = 0; d < 7; ++d) {
        page->schedule_days[d] = gtk_toggle_button_new_with_label(day_labels[d]);
        gchar *suffix = g_strdup_printf("schedule-day-%u", d + 1); named(page->schedule_days[d], suffix); g_free(suffix);
        g_signal_connect(page->schedule_days[d], "toggled", G_CALLBACK(schedule_field_changed), page);
        gtk_box_pack_start(GTK_BOX(days_box), page->schedule_days[d], FALSE, FALSE, 0);
    }
    gtk_grid_attach(GTK_GRID(page->schedule_form), caption("Günler", "re-caption"), 0, 1, 1, 1); gtk_grid_attach(GTK_GRID(page->schedule_form), days_box, 1, 1, 3, 1);
    page->schedule_start = named(radios_ui_entry("SS:DD"), "schedule-start"); page->schedule_end = named(radios_ui_entry("SS:DD"), "schedule-end");
    gtk_entry_set_width_chars(GTK_ENTRY(page->schedule_start), 6); gtk_entry_set_width_chars(GTK_ENTRY(page->schedule_end), 6);
    g_signal_connect(page->schedule_start, "changed", G_CALLBACK(schedule_field_changed), page);
    g_signal_connect(page->schedule_end, "changed", G_CALLBACK(schedule_field_changed), page);
    GtkWidget *time_box = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 4, NULL);
    gtk_box_pack_start(GTK_BOX(time_box), page->schedule_start, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(time_box), caption("–", NULL), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(time_box), page->schedule_end, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(page->schedule_form), caption("Saat", "re-caption"), 0, 2, 1, 1); gtk_grid_attach(GTK_GRID(page->schedule_form), time_box, 1, 2, 1, 1);
    const gchar *kind_ids[] = {"all", "music", "advertisement", "jingle", "sweeper"}, *kind_labels[] = {"Tüm içerik", "Müzik", "Reklam", "Jingle", "Sweeper"};
    page->schedule_kind = combo_new("schedule-kind", kind_ids, kind_labels, 5, page, G_CALLBACK(schedule_field_changed));
    gtk_grid_attach(GTK_GRID(page->schedule_form), caption("İçerik", "re-caption"), 2, 2, 1, 1); gtk_grid_attach(GTK_GRID(page->schedule_form), page->schedule_kind, 3, 2, 1, 1);
    page->schedule_presets = flow_new("schedule-presets");
    gtk_grid_attach(GTK_GRID(page->schedule_form), caption("Ön ayarlar", "re-caption"), 0, 3, 1, 1); gtk_grid_attach(GTK_GRID(page->schedule_form), page->schedule_presets, 1, 3, 3, 1);
    page->schedule_rotation_box = named(radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL), "schedule-rotation-box");
    page->schedule_rotation = combo_new("schedule-rotation", rotation_ids, rotation_labels, 2, page, G_CALLBACK(schedule_field_changed));
    gtk_box_pack_start(GTK_BOX(page->schedule_rotation_box), caption("Sıra", "re-caption"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(page->schedule_rotation_box), page->schedule_rotation, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(page->schedule_form), page->schedule_rotation_box, 0, 4, 4, 1);
    gtk_box_pack_start(GTK_BOX(page->schedule_box), page->schedule_form, FALSE, FALSE, 0);
    page->status = named(radios_ui_label("", "error"), "status");
    gtk_box_pack_start(GTK_BOX(page->root), page->status, FALSE, FALSE, 0);
    g_object_set_data_full(G_OBJECT(page->root), "radios-autodj-automation", page, page_free);
    g_signal_connect(page->root, "destroy", G_CALLBACK(page_destroyed), page);
    gtk_widget_show_all(page->root);
    chips_render(page); fields_render(page); kinds_render(page); schedule_render(page); schedule_form_render(page);
    designed_render(page);
    return page->root;
}

void radios_autodj_automation_render(GtkWidget *widget, JsonObject *settings)
{
    AutomationPage *page = page_get(widget); if (!page) return;
    ++page->updating;
    JsonArray *presets = array_member(settings, "autodj_presets"); JsonObject *automation = object_member(settings, "autodj_automation");
    if (page->presets) json_array_unref(page->presets);
    page->presets = presets ? copy_array(presets) : radios_transition_preset_templates();
    if (page->automation) json_object_unref(page->automation);
    page->automation = automation ? copy_object(automation) : json_object_new();
    if (!json_object_has_member(page->automation, "enabled")) json_object_set_boolean_member(page->automation, "enabled", FALSE);
    if (!object_member(page->automation, "kinds")) json_object_set_object_member(page->automation, "kinds", json_object_new());
    page->touched = presets != NULL || automation != NULL;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->enabled), bool_member(page->automation, "enabled"));
    --page->updating;
    /* Each kind's own entries, or its earlier rule converted (still unsaved). */
    for (guint k = 0; page->hooks.kind_distributions && k < KIND_COUNT; ++k) radios_transition_distribution_render(page->distributions[k], settings);
    chips_render(page); fields_render(page); kinds_render(page); schedule_render(page); schedule_form_render(page);
    designed_render(page);
    gchar *problem = page_problem(page); gtk_label_set_text(GTK_LABEL(page->status), problem ? problem : ""); g_free(problem);
}
void radios_autodj_automation_read(GtkWidget *widget, JsonObject *settings)
{
    AutomationPage *page = page_get(widget); if (!page || !settings) return;
    gboolean pending = FALSE;
    for (guint k = 0; page->hooks.kind_distributions && k < KIND_COUNT; ++k) pending |= radios_transition_distribution_pending(page->distributions[k]);
    if (pending) {
        /* All four kinds move to settings.transitions in the same record; the
         * fields they replace go (kılavuz 18.10 contract, rule g). */
        for (guint k = 0; k < KIND_COUNT; ++k) radios_transition_distribution_read(page->distributions[k], settings);
        if (json_object_has_member(settings, "kind_transitions")) json_object_remove_member(settings, "kind_transitions");
        JsonObject *kinds = object_member(page->automation, "kinds");
        if (kinds && json_object_get_size(kinds)) { json_object_set_object_member(page->automation, "kinds", json_object_new()); page->touched = TRUE; }
    }
    gboolean stored = json_object_has_member(settings, "autodj_presets") || json_object_has_member(settings, "autodj_automation");
    if (!page->touched && !stored) return;
    json_object_set_array_member(settings, "autodj_presets", copy_array(page->presets ? page->presets : (page->presets = json_array_new())));
    json_object_set_object_member(settings, "autodj_automation", copy_object(page->automation));
}
gboolean radios_autodj_automation_valid(GtkWidget *widget, gchar **message)
{
    AutomationPage *page = page_get(widget); if (message) *message = NULL;
    if (!page) return TRUE;
    gchar *problem = page_problem(page);
    if (!problem) return TRUE;
    if (message) *message = problem; else g_free(problem);
    return FALSE;
}
gboolean radios_autodj_automation_kind_uses_presets(GtkWidget *widget, const gchar *kind)
{
    AutomationPage *page = page_get(widget); if (!page || !bool_member(page->automation, "enabled")) return FALSE;
    gint index = radios_transition_kind_index(kind);
    if (page->hooks.kind_distributions && index >= 0 && radios_transition_distribution_uses_presets(page->distributions[index])) return TRUE;
    JsonArray *ids = array_member(kind_entry(page, kind, FALSE), "presets");
    if (ids && json_array_get_length(ids)) return TRUE;
    JsonArray *schedule = schedule_array(page, FALSE);
    for (guint i = 0; schedule && i < json_array_get_length(schedule); ++i) {
        JsonObject *row = json_array_get_object_element(schedule, i); JsonArray *presets = array_member(row, "presets");
        const gchar *row_kind = radios_json_string(row, "kind", "all");
        if ((g_str_equal(row_kind, "all") || !g_strcmp0(row_kind, kind)) && presets && json_array_get_length(presets)) return TRUE;
    }
    return FALSE;
}
