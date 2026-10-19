#include "radios/player_waveform.h"
#include "radios/programme_model.h"
#include "radios/media_ui.h"
#include "radios/editors.h"
#include "radios/audio.h"
#include <math.h>
#include <string.h>

#define WAVE_REFRESH_US (30 * G_TIME_SPAN_SECOND)

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *widget;
    GWeakRef card;
    guint deck;
    RadioSAudioUiSnapshot snapshot;
    gboolean online, destroyed;
    guint64 serial;
    gint64 refresh_after;
    JsonNode *detail, *seen_cache;
    RadioSDataClient *client;
    GCancellable *cancel;
    gchar *message, *marker_message;
    GtkWidget *overlay, *pan, *time, *zoom_in, *zoom_out, *reset, *inspect;
    GtkWidget *dialog, *panel;
    GtkAdjustment *range;
    GtkCssProvider *controls_theme;
    guint zoom;
    gboolean adjusting, pointer_valid;
    gdouble pointer_ms, pointer_ratio;
    /* Beat grid editor in the inspection window. `anchor_ms` is a beat that
     * starts a bar; the grid is every 60000 / bpm ms around it. */
    GtkWidget *bpm_spin, *grid_status, *grid_save, *grid_auto, *grid_selected;
    RadioSRhythmAnalysis grid;
    gdouble anchor_ms, selected_ms;
    gboolean grid_valid, grid_dirty, spin_updating, selected_valid;
    gint64 taps[8];
    guint tap_count;
    GCancellable *grid_cancel;
    GtkWidget *marker_menu, *marker_clear, *marker_snap, *marker_snap_menu, *marker_status;
    gboolean marker_busy, marker_snapping;
    guint64 marker_ticket;
} PlayerWaveform;

typedef struct {
    GWeakRef wave, card;
    RadioSDataClient *client;
    guint64 serial, generation;
    gchar media_id[37], version_id[37];
} WaveRequest;

typedef struct {
    RadioSDataClient *client;
    guint64 ticket, generation;
    gchar media_id[37], version_id[37];
} MarkerWrite;

static void view_refresh(PlayerWaveform *wave);
static void reset_view(PlayerWaveform *wave, gboolean close_inspection);
static void inspect_open(PlayerWaveform *wave);
static void grid_load(PlayerWaveform *wave);
static void marker_controls_refresh(PlayerWaveform *wave);
static void marker_action(GtkWidget *widget, gpointer data);
static void marker_menu_open(PlayerWaveform *wave, GdkEvent *event);
static void detail_merge_known(PlayerWaveform *wave, JsonObject *merged, JsonNode *known);
static void panel_refresh(PlayerWaveform *wave);
static void panel_grid_sync(PlayerWaveform *wave);

static const gchar *const marker_keys[] = {
    "cue_in_ms", "intro_ms", "out_ms", "fade_start_ms", "fade_end_ms", "auto_next_ms", "autodj_in_ms", "autodj_out_ms"
};
static const gchar *const marker_tags[] = {"IN", "VO", "OUT", "F1", "F2", "AUTO", "AutoDJ IN", "AutoDJ OUT"};
static const gchar *const marker_names[] = {
    "Cue giriş", "Intro", "Çıkış", "Fade başlangıcı", "Fade sonu", "Otomatik geçiş", "AutoDJ giriş", "AutoDJ geçiş noktası"
};

static JsonObject *object_member(JsonObject *object, const gchar *name)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static JsonObject *detail_object(JsonNode *node)
{ return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL; }
static gboolean number_read(JsonNode *node, gdouble *value)
{
    if (!node || !JSON_NODE_HOLDS_VALUE(node)) return FALSE;
    GType type = json_node_get_value_type(node);
    if (type != G_TYPE_DOUBLE && type != G_TYPE_INT64) return FALSE;
    *value = json_node_get_double(node); return isfinite(*value);
}
static gboolean flag_true(JsonObject *object, const gchar *name)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN && json_node_get_boolean(node);
}
static JsonNode *detail_copy(JsonNode *node)
{
    JsonObject *object = detail_object(node); if (!object) return NULL;
    JsonNode *copy = json_node_new(JSON_NODE_OBJECT);
    json_node_take_object(copy, radios_programme_clone_object(object)); return copy;
}
static gboolean detail_not_older(JsonNode *candidate, JsonNode *known)
{
    JsonObject *next = detail_object(candidate), *previous = detail_object(known);
    if (!next) return FALSE;
    if (!previous) return TRUE;
    return radios_json_int(object_member(next, "media"), "_rev", 0) >= radios_json_int(object_member(previous, "media"), "_rev", 0) &&
        radios_json_int(next, "analysis_revision", 0) >= radios_json_int(previous, "analysis_revision", 0) &&
        radios_json_int(object_member(next, "markers"), "_rev", 0) >= radios_json_int(object_member(previous, "markers"), "_rev", 0);
}
static gboolean detail_matches(PlayerWaveform *wave, JsonNode *node)
{
    JsonObject *object = detail_object(node), *media = object_member(object, "media"), *version = object_member(object, "version");
    const gchar *station = wave->client ? radios_data_client_station_id(wave->client) : radios_data_station_id(wave->ctx);
    return radios_data_connected(wave->ctx) && !wave->online && media && version && wave->snapshot.media_id[0] &&
        g_str_equal(radios_json_string(media, "_id", ""), wave->snapshot.media_id) &&
        g_str_equal(radios_json_string(media, "version_id", ""), wave->snapshot.version_id) &&
        g_str_equal(radios_json_string(version, "_id", ""), wave->snapshot.version_id) &&
        g_str_equal(radios_json_string(media, "station_id", "!"), station) &&
        flag_true(version, "verified") && flag_true(version, "object_available") &&
        strlen(radios_json_string(version, "sha256", "")) == 64 &&
        radios_json_int(version, "duration_ms", 0) > 0;
}
static JsonObject *valid_markers(PlayerWaveform *wave)
{
    JsonObject *object = detail_object(wave->detail), *markers = object_member(object, "markers");
    JsonObject *media = object_member(object, "media"), *version = object_member(object, "version");
    if (!markers || !detail_matches(wave, wave->detail)) return NULL;
    if (!g_str_equal(radios_json_string(markers, "station_id", "!"), radios_json_string(media, "station_id", "")) ||
        !g_str_equal(radios_json_string(markers, "item_id", "!"), radios_json_string(media, "item_id", "")) ||
        !g_str_equal(radios_json_string(markers, "version_id", "!"), wave->snapshot.version_id) ||
        !g_str_equal(radios_json_string(markers, "media_file_id", "!"), radios_json_string(media, "media_file_id", "")) ||
        !g_str_equal(radios_json_string(markers, "media_sha256", "!"), radios_json_string(version, "sha256", "")) ||
        radios_json_int(markers, "media_duration_ms", -1) != radios_json_int(version, "duration_ms", 0)) return NULL;
    return markers;
}
static JsonArray *valid_waveform(PlayerWaveform *wave, gint64 *hop, gint64 *rate)
{
    JsonObject *object = detail_object(wave->detail), *analysis = object_member(object, "analysis"), *version = object_member(object, "version");
    if (!analysis || !detail_matches(wave, wave->detail) ||
        !g_str_equal(radios_json_string(analysis, "sha256", "!"), radios_json_string(version, "sha256", ""))) return NULL;
    JsonNode *node = json_object_get_member(analysis, "waveform");
    if (!node || !JSON_NODE_HOLDS_ARRAY(node)) return NULL;
    JsonArray *array = json_node_get_array(node); guint length = json_array_get_length(array);
    *hop = radios_json_int(analysis, "waveform_frames_per_bin", 0); *rate = radios_json_int(analysis, "sample_rate", 0);
    if (!length || length % 2 || length > 4096 || *hop < 1 || *rate < 1 || *rate > 384000) return NULL;
    for (guint index = 0; index < length; index += 2) {
        gdouble low, high;
        /* Floating-point media can contain measured peaks outside [-1, 1].
         * Keep them; the renderer clamps the plot and marks clipped bins. */
        if (!number_read(json_array_get_element(array, index), &low) || !number_read(json_array_get_element(array, index + 1), &high) ||
            low < -1000000 || high > 1000000 || low > high) return NULL;
    }
    return array;
}
static gboolean marker_read(JsonObject *markers, guint index, gint64 duration, gint64 *value)
{
    *value = radios_json_int(markers, marker_keys[index], -1);
    return *value >= 0 && *value <= duration;
}
static gboolean stored_grid(PlayerWaveform *wave, RadioSRhythmAnalysis *grid)
{
    JsonObject *analysis = object_member(detail_object(wave->detail), "analysis");
    return analysis && detail_matches(wave, wave->detail) && radios_rhythm_analysis_read(object_member(analysis, "rhythm"), grid);
}
static void describe(PlayerWaveform *wave)
{
    GString *description = g_string_new("Ölçülmüş dalga biçimi. Tıkla: nokta seç · sağ tık: AutoDJ IN/OUT · çift tık: incele.");
    if (wave->online) g_string_assign(description, "Çevrimiçi kaynak için yerel dalga biçimi ve cue noktaları gösterilmez.");
    else if (!wave->snapshot.media_id[0]) g_string_assign(description, "Medya yüklenince dalga biçimi ve cue noktaları gösterilir.");
    else {
        JsonObject *markers = valid_markers(wave), *version = object_member(detail_object(wave->detail), "version");
        gint64 duration = radios_json_int(version, "duration_ms", 0), value;
        for (guint index = 0; index < G_N_ELEMENTS(marker_keys); ++index)
            if (marker_read(markers, index, duration, &value)) g_string_append_printf(description, " %s: %.3f s.", marker_names[index], value / 1000.0);
        if (wave->message) g_string_append_printf(description, " %s", wave->message);
    }
    gtk_widget_set_tooltip_text(wave->widget, description->str);
    atk_object_set_description(gtk_widget_get_accessible(wave->widget), description->str);
    g_string_free(description, TRUE);
}
static void adopt_detail(PlayerWaveform *wave, JsonNode *node)
{
    if (!detail_matches(wave, node)) return;
    JsonObject *before = detail_object(wave->detail), *after = detail_object(node);
    JsonNode *old_markers = before ? json_object_get_member(before, "markers") : NULL,
             *new_markers = after ? json_object_get_member(after, "markers") : NULL;
    gboolean markers_same = old_markers && new_markers ? json_node_equal(old_markers, new_markers) : old_markers == new_markers;
    if (!markers_same) g_clear_pointer(&wave->marker_message, g_free);
    g_clear_pointer(&wave->detail, json_node_free); wave->detail = detail_copy(node);
    if (!wave->marker_message) g_clear_pointer(&wave->message, g_free);
    describe(wave);
    if (wave->dialog && !wave->grid_dirty) grid_load(wave);
    if (wave->dialog && wave->panel) panel_refresh(wave);
    marker_controls_refresh(wave);
    view_refresh(wave);
}
static gboolean view_available(PlayerWaveform *wave, gint64 *duration)
{
    gint64 hop = 0, rate = 0;
    *duration = radios_json_int(object_member(detail_object(wave->detail), "version"), "duration_ms", 0);
    return *duration > 0 && valid_waveform(wave, &hop, &rate) != NULL;
}
static void view_refresh(PlayerWaveform *wave)
{
    if (!wave->range || wave->destroyed) return;
    gint64 duration = 0; gboolean available = view_available(wave, &duration);
    if (!available) { wave->zoom = 1; wave->pointer_valid = FALSE; }
    wave->zoom = CLAMP(wave->zoom, 1, 32);
    gdouble span = available ? (gdouble)duration / wave->zoom : 1;
    gdouble start = available ? CLAMP(gtk_adjustment_get_value(wave->range), 0, MAX(0, duration - span)) : 0;
    wave->adjusting = TRUE;
    gtk_adjustment_configure(wave->range, start, 0, available ? duration : 1, MAX(1, span / 20), MAX(1, span * .8), span);
    wave->adjusting = FALSE;
    gtk_widget_set_sensitive(wave->zoom_in, available && wave->zoom < 32);
    gtk_widget_set_sensitive(wave->zoom_out, available && wave->zoom > 1);
    gtk_widget_set_sensitive(wave->reset, available && wave->zoom > 1);
    gtk_widget_set_sensitive(wave->inspect, available);
    gtk_widget_set_visible(wave->pan, available && wave->zoom > 1);
    gchar *label = wave->pointer_valid && available ? g_strdup_printf("İmleç: %.3f s", wave->pointer_ms / 1000) :
        available ? g_strdup_printf("%.3f – %.3f s · ×%u", start / 1000, (start + span) / 1000, wave->zoom) : g_strdup("Dalga biçimi / Cue");
    gtk_label_set_text(GTK_LABEL(wave->time), label);
    g_free(label);
    gtk_widget_queue_draw(wave->widget);
    if (!available && wave->dialog) gtk_widget_destroy(wave->dialog);
    marker_controls_refresh(wave);
}
static void reset_view(PlayerWaveform *wave, gboolean close_inspection)
{
    wave->zoom = 1; wave->pointer_valid = FALSE;
    if (close_inspection && wave->dialog) gtk_widget_destroy(wave->dialog);
    if (wave->range) {
        wave->adjusting = TRUE; gtk_adjustment_set_value(wave->range, 0); wave->adjusting = FALSE;
        view_refresh(wave);
    }
}
static void range_changed(GtkAdjustment *adjustment, gpointer data)
{
    (void)adjustment; PlayerWaveform *wave = data;
    if (wave->adjusting || wave->destroyed) return;
    wave->pointer_valid = FALSE; view_refresh(wave);
}
static void zoom_set(PlayerWaveform *wave, guint zoom)
{
    gint64 duration = 0; if (!view_available(wave, &duration)) return;
    gdouble old_span = gtk_adjustment_get_page_size(wave->range), old_start = gtk_adjustment_get_value(wave->range);
    gdouble ratio = wave->pointer_valid ? wave->pointer_ratio : .5;
    gdouble anchor = wave->pointer_valid ? wave->pointer_ms : old_start + old_span * ratio;
    wave->zoom = CLAMP(zoom, 1, 32); gdouble span = (gdouble)duration / wave->zoom;
    wave->adjusting = TRUE;
    gtk_adjustment_configure(wave->range, CLAMP(anchor - span * ratio, 0, MAX(0, duration - span)),
        0, duration, MAX(1, span / 20), MAX(1, span * .8), span);
    wave->adjusting = FALSE; wave->pointer_valid = FALSE; view_refresh(wave);
}
static void zoom_clicked(GtkButton *button, gpointer data)
{
    PlayerWaveform *wave = data; gint direction = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "waveform-action"));
    if (direction == 2) inspect_open(wave);
    else if (!direction) reset_view(wave, FALSE);
    else zoom_set(wave, direction > 0 ? wave->zoom * 2 : MAX(1, wave->zoom / 2));
}
static gboolean waveform_motion(GtkWidget *widget, GdkEventMotion *event, gpointer data)
{
    PlayerWaveform *wave = data; gint64 duration = 0; if (!view_available(wave, &duration)) return FALSE;
    gdouble usable = MAX(1, gtk_widget_get_allocated_width(widget) - 16);
    wave->pointer_ratio = CLAMP((event->x - 8) / usable, 0, 1);
    wave->pointer_ms = gtk_adjustment_get_value(wave->range) + wave->pointer_ratio * gtk_adjustment_get_page_size(wave->range);
    wave->pointer_valid = TRUE; view_refresh(wave); return TRUE;
}
static gboolean waveform_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer data)
{
    (void)widget; PlayerWaveform *wave = data; gint64 duration = 0; if (!view_available(wave, &duration)) return FALSE;
    gdouble delta = event->direction == GDK_SCROLL_UP || event->direction == GDK_SCROLL_LEFT ? -1 :
        event->direction == GDK_SCROLL_DOWN || event->direction == GDK_SCROLL_RIGHT ? 1 : event->delta_y;
    if (fabs(delta) < .01) return FALSE;
    if (event->state & GDK_SHIFT_MASK) {
        gdouble span = gtk_adjustment_get_page_size(wave->range);
        gtk_adjustment_set_value(wave->range, gtk_adjustment_get_value(wave->range) + delta * span / 5);
    } else zoom_set(wave, delta < 0 ? wave->zoom * 2 : MAX(1, wave->zoom / 2));
    return TRUE;
}
static void grid_controls_refresh(PlayerWaveform *wave);
static gboolean waveform_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    PlayerWaveform *wave = data; gint64 duration = 0;
    if (event->button != 1 && event->button != 3) return FALSE;
    if (event->type == GDK_BUTTON_PRESS && view_available(wave, &duration)) {
        /* A click marks the point the grid editor can make beat one. */
        if (event->type != GDK_BUTTON_PRESS || !view_available(wave, &duration)) return TRUE;
        gdouble usable = MAX(1, gtk_widget_get_allocated_width(widget) - 16), ratio = CLAMP((event->x - 8) / usable, 0, 1);
        wave->selected_ms = gtk_adjustment_get_value(wave->range) + ratio * gtk_adjustment_get_page_size(wave->range);
        wave->selected_valid = TRUE;
        if (wave->marker_message) { g_clear_pointer(&wave->marker_message, g_free); g_clear_pointer(&wave->message, g_free); describe(wave); }
        grid_controls_refresh(wave); marker_controls_refresh(wave); gtk_widget_queue_draw(widget);
        if (event->button == 3) marker_menu_open(wave, (GdkEvent *)event);
        return TRUE;
    }
    if (event->type != GDK_2BUTTON_PRESS) return FALSE;
    inspect_open(wave); return TRUE;
}
static void inspection_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget; PlayerWaveform *wave = data;
    wave->dialog = NULL; wave->panel = NULL;
    wave->bpm_spin = wave->grid_status = wave->grid_save = wave->grid_auto = wave->grid_selected = NULL;
    wave->marker_clear = wave->marker_snap = wave->marker_status = NULL;
    wave->grid_dirty = wave->selected_valid = FALSE; wave->tap_count = 0;
}
static GtkWidget *wave_button(PlayerWaveform *wave, const gchar *label, const gchar *suffix, gint action)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    gchar *name = g_strdup_printf("deck-%u-waveform-%s", wave->deck, suffix); gtk_widget_set_name(button, name); g_free(name);
    gtk_style_context_add_class(gtk_widget_get_style_context(button), "lp-waveform-control");
    gtk_style_context_add_provider(gtk_widget_get_style_context(button), GTK_STYLE_PROVIDER(wave->controls_theme), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    g_object_set_data(G_OBJECT(button), "waveform-action", GINT_TO_POINTER(action));
    g_signal_connect(button, "clicked", G_CALLBACK(zoom_clicked), wave);
    return button;
}
static void waveform_events(PlayerWaveform *wave, GtkWidget *widget)
{
    gtk_widget_add_events(widget, GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_BUTTON_PRESS_MASK);
    g_signal_connect(widget, "motion-notify-event", G_CALLBACK(waveform_motion), wave);
    g_signal_connect(widget, "scroll-event", G_CALLBACK(waveform_scroll), wave);
    g_signal_connect(widget, "button-press-event", G_CALLBACK(waveform_press), wave);
}
static gboolean waveform_draw(GtkWidget *widget, cairo_t *cr, gpointer data);
static gint64 media_duration(PlayerWaveform *wave)
{ return radios_json_int(object_member(detail_object(wave->detail), "version"), "duration_ms", 0); }
static void marker_note(PlayerWaveform *wave, const gchar *message)
{
    g_free(wave->message); wave->message = g_strdup(message);
    g_free(wave->marker_message); wave->marker_message = g_strdup(message);
    if (wave->marker_status) gtk_label_set_text(GTK_LABEL(wave->marker_status), message);
    describe(wave);
}
static void marker_controls_refresh(PlayerWaveform *wave)
{
    gboolean available = !wave->destroyed && !wave->marker_busy && !wave->grid_cancel && detail_matches(wave, wave->detail);
    if (wave->marker_clear) gtk_widget_set_sensitive(wave->marker_clear, available && wave->panel != NULL);
    if (wave->marker_snap) gtk_widget_set_sensitive(wave->marker_snap, available);
    if (wave->marker_snap_menu) gtk_widget_set_sensitive(wave->marker_snap_menu, available);
    gboolean grid_idle = !wave->marker_busy && !wave->grid_cancel;
    if (wave->grid_save) gtk_widget_set_sensitive(wave->grid_save, grid_idle && wave->grid_dirty && media_duration(wave) >= 4000);
    if (wave->grid_auto) gtk_widget_set_sensitive(wave->grid_auto, grid_idle);
    if (wave->grid_selected) gtk_widget_set_sensitive(wave->grid_selected, grid_idle && wave->panel != NULL);
    if (wave->marker_status) {
        JsonObject *markers = valid_markers(wave); gint64 in = radios_json_int(markers, "autodj_in_ms", -1), out = radios_json_int(markers, "autodj_out_ms", -1);
        gchar *a = in >= 0 ? g_strdup_printf("%.3f s", in / 1000.0) : g_strdup("varsayılan"),
              *b = out >= 0 ? g_strdup_printf("%.3f s", out / 1000.0) : g_strdup("varsayılan");
        gchar *text = wave->marker_busy ? g_strdup("AutoDJ noktaları kaydediliyor…") :
            wave->marker_message ? g_strdup(wave->marker_message) : g_strdup_printf("AutoDJ IN: %s · OUT: %s", a, b);
        gtk_label_set_text(GTK_LABEL(wave->marker_status), text); g_free(text); g_free(a); g_free(b);
    }
}
static void marker_write_free(gpointer data)
{ MarkerWrite *write = data; radios_data_client_unref(write->client); g_free(write); }
static void marker_saved(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *node, const GError *error, gpointer data)
{
    MarkerWrite *write = data; PlayerWaveform *wave = g_object_get_data(G_OBJECT(owner), "radios-player-waveform");
    if (!wave || wave->destroyed) return;
    if (write->ticket != wave->marker_ticket) { wave->refresh_after = 0; return; }
    wave->marker_busy = FALSE; marker_controls_refresh(wave);
    RadioSDataClient *current = radios_data_client_acquire(ctx); RadioSAudioUiSnapshot snapshot = {0};
    gboolean same = radios_data_client_same_identity(current, write->client) &&
        radios_audio_ui_get_snapshot(ctx, wave->deck, &snapshot) && snapshot.generation == write->generation &&
        wave->snapshot.generation == write->generation && g_str_equal(snapshot.media_id, write->media_id) &&
        g_str_equal(snapshot.version_id, write->version_id) && detail_matches(wave, node);
    g_clear_pointer(&current, radios_data_client_unref);
    if (error) { wave->refresh_after = 0; marker_note(wave, error->message); return; }
    if (!same) { wave->refresh_after = 0; marker_note(wave, "Oynatıcı değişti; kaydedilen noktalar asıl parçada korunuyor."); return; }
    GtkWidget *card = g_weak_ref_get(&wave->card);
    if (card && !gtk_widget_in_destruction(card)) {
        /* Analysis and marker saves can finish in either order. A marker ACK
         * must keep its new cues while preserving any newer measured analysis. */
        JsonNode *cache = detail_copy(node); JsonObject *merged = detail_object(cache);
        detail_merge_known(wave, merged, wave->detail);
        detail_merge_known(wave, merged, g_object_get_data(G_OBJECT(card), "radios-deck-media-detail"));
        g_object_set_data_full(G_OBJECT(card), "radios-deck-media-detail", cache, (GDestroyNotify)json_node_free);
        wave->seen_cache = cache; adopt_detail(wave, cache);
        marker_note(wave, "Kayıt tamamlandı; güncel AutoDJ IN/OUT noktaları gösteriliyor.");
    }
    g_clear_object(&card);
}
static void marker_action(GtkWidget *widget, gpointer data)
{
    PlayerWaveform *wave = data; const gchar *action = g_object_get_data(G_OBJECT(widget), "autodj-marker-action");
    if (wave->destroyed || wave->marker_busy || wave->grid_cancel || !detail_matches(wave, wave->detail) || !action) return;
    RadioSAudioUiSnapshot snapshot = {0}; RadioSDataClient *current = radios_data_client_acquire(wave->ctx);
    gboolean same = radios_data_client_same_identity(current, wave->client) && radios_audio_ui_get_snapshot(wave->ctx, wave->deck, &snapshot) &&
        snapshot.generation == wave->snapshot.generation && g_str_equal(snapshot.media_id, wave->snapshot.media_id) &&
        g_str_equal(snapshot.version_id, wave->snapshot.version_id);
    g_clear_pointer(&current, radios_data_client_unref);
    if (!same) { marker_note(wave, "Parça veya veri oturumu değişti; noktayı yeniden seçin."); return; }
    JsonObject *old = valid_markers(wave); gint64 duration = media_duration(wave);
    gint64 cue_in = radios_json_int(old, "cue_in_ms", 0), cue_out = radios_json_int(old, "out_ms", duration);
    gint64 value = (gint64)llround(wave->selected_ms);
    if (!g_str_equal(action, "clear")) {
        if (!wave->selected_valid) return;
        gboolean setting_in = g_str_equal(action, "in");
        gint64 lower = setting_in ? cue_in : radios_json_int(old, "autodj_in_ms", cue_in) + 1,
               upper = setting_in ? radios_json_int(old, "autodj_out_ms", cue_out) - 1 : cue_out;
        /* Check the user's point first, then snap within the legal interval.
         * Rounding a valid IN to OUT, or OUT to IN, must not reject the edit. */
        if (value < lower || value > upper) { marker_note(wave, "AutoDJ IN, OUT'tan önce ve cue aralığında olmalı."); return; }
        RadioSRhythmAnalysis grid;
        if (wave->marker_snapping && wave->grid_dirty) { marker_note(wave, "Önce BPM ızgarasını kaydedin, ardından AutoDJ noktasını seçin."); return; }
        if (wave->marker_snapping && stored_grid(wave, &grid)) {
            gdouble period = 240000 / grid.bpm, anchor = grid.has_downbeat ? grid.downbeat_ms : grid.first_beat_ms;
            gdouble first = ceil((lower - anchor) / period), last = floor((upper - anchor) / period);
            if (first > last) { marker_note(wave, "IN/OUT aralığında dörtlük başı yok; hizalamayı kapatın."); return; }
            value = (gint64)llround(anchor + CLAMP(round((wave->selected_ms - anchor) / period), first, last) * period);
        }
        if (value < lower || value > upper) { marker_note(wave, "AutoDJ IN, OUT'tan önce ve cue aralığında olmalı."); return; }
    }
    JsonObject *markers = json_object_new();
    for (guint i = 0; i < 6; ++i) json_object_set_int_member(markers, marker_keys[i], radios_json_int(old, marker_keys[i], i == 0 || i == 1 ? 0 : duration));
    json_object_set_int_member(markers, "overlap_ms", radios_json_int(old, "overlap_ms", 0));
    json_object_set_boolean_member(markers, "automatic", flag_true(old, "automatic"));
    for (guint i = 6; i < G_N_ELEMENTS(marker_keys); ++i) {
        gint64 saved = radios_json_int(old, marker_keys[i], -1);
        if (saved >= 0) json_object_set_int_member(markers, marker_keys[i], saved);
        else json_object_set_null_member(markers, marker_keys[i]);
    }
    if (g_str_equal(action, "clear")) { json_object_set_null_member(markers, "autodj_in_ms"); json_object_set_null_member(markers, "autodj_out_ms"); }
    else json_object_set_int_member(markers, g_str_equal(action, "in") ? "autodj_in_ms" : "autodj_out_ms", value);
    JsonNode *body = radios_data_body(wave->ctx); JsonObject *object = json_node_get_object(body);
    JsonObject *media = object_member(detail_object(wave->detail), "media");
    json_object_set_string_member(object, "media_id", wave->snapshot.media_id);
    json_object_set_int_member(object, "expected_revision", radios_json_int(media, "_rev", 0));
    if (old) json_object_set_int_member(object, "expected_marker_revision", radios_json_int(old, "_rev", 0));
    else json_object_set_null_member(object, "expected_marker_revision");
    gchar *key = g_uuid_string_random(); json_object_set_string_member(object, "idempotency_key", key); g_free(key);
    json_object_set_object_member(object, "markers", markers);
    MarkerWrite *write = g_new0(MarkerWrite, 1); write->client = radios_data_client_ref(wave->client);
    write->generation = snapshot.generation; write->ticket = ++wave->marker_ticket;
    g_strlcpy(write->media_id, snapshot.media_id, sizeof write->media_id); g_strlcpy(write->version_id, snapshot.version_id, sizeof write->version_id);
    wave->marker_busy = radios_data_request(wave->ctx, wave->widget, "POST", "/v1/workflows/desktop-media/markers-save", body, marker_saved, write, marker_write_free);
    json_node_free(body); marker_controls_refresh(wave);
    if (!wave->marker_busy) marker_note(wave, "Veri bağlantısı hazır değil; noktalar kaydedilemedi.");
}
static void marker_snap_changed(GtkWidget *widget, gpointer data)
{
    PlayerWaveform *wave = data;
    wave->marker_snapping = GTK_IS_CHECK_MENU_ITEM(widget) ? gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(widget)) :
        gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget));
    if (wave->marker_snap && gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(wave->marker_snap)) != wave->marker_snapping)
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wave->marker_snap), wave->marker_snapping);
    if (wave->marker_snap_menu && gtk_check_menu_item_get_active(GTK_CHECK_MENU_ITEM(wave->marker_snap_menu)) != wave->marker_snapping)
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(wave->marker_snap_menu), wave->marker_snapping);
    if (wave->marker_message) { g_clear_pointer(&wave->marker_message, g_free); g_clear_pointer(&wave->message, g_free); describe(wave); }
    if (wave->panel) radios_marker_editor_set_snap(wave->panel, wave->marker_snapping);
    marker_controls_refresh(wave);
}
static void marker_menu_open(PlayerWaveform *wave, GdkEvent *event)
{
    if (!wave->marker_menu) {
        wave->marker_menu = gtk_menu_new(); g_object_ref_sink(wave->marker_menu); gtk_menu_attach_to_widget(GTK_MENU(wave->marker_menu), wave->widget, NULL);
        const gchar *labels[] = {"Seçili noktayı AutoDJ IN yap", "Seçili noktayı AutoDJ OUT yap", "AutoDJ noktalarını temizle"}, *actions[] = {"in", "out", "clear"};
        for (guint i = 0; i < G_N_ELEMENTS(labels); ++i) {
            GtkWidget *item = gtk_menu_item_new_with_label(labels[i]);
            g_object_set_data_full(G_OBJECT(item), "autodj-marker-action", g_strdup(actions[i]), g_free);
            g_signal_connect(item, "activate", G_CALLBACK(marker_action), wave); gtk_menu_shell_append(GTK_MENU_SHELL(wave->marker_menu), item);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(wave->marker_menu), gtk_separator_menu_item_new());
        wave->marker_snap_menu = gtk_check_menu_item_new_with_label("Dörtlük başına hizala");
        gtk_check_menu_item_set_active(GTK_CHECK_MENU_ITEM(wave->marker_snap_menu), wave->marker_snapping);
        g_signal_connect(wave->marker_snap_menu, "toggled", G_CALLBACK(marker_snap_changed), wave);
        gtk_menu_shell_append(GTK_MENU_SHELL(wave->marker_menu), wave->marker_snap_menu);
    }
    marker_controls_refresh(wave);
    gtk_widget_set_sensitive(wave->marker_menu, !wave->marker_busy && !wave->grid_cancel && detail_matches(wave, wave->detail));
    gtk_widget_show_all(wave->marker_menu); gtk_menu_popup_at_pointer(GTK_MENU(wave->marker_menu), event);
}
/* Stored phases derive from the anchor: the first beat and first bar start. */
static void grid_apply(PlayerWaveform *wave)
{
    gdouble period = 60000 / wave->grid.bpm, first = fmod(wave->anchor_ms, period), bar = fmod(wave->anchor_ms, 4 * period);
    if (first < 0) first += period;
    if (bar < 0) bar += 4 * period;
    wave->grid.first_beat_ms = first < period ? first : 0; wave->grid.downbeat_ms = bar < 4 * period ? bar : 0;
    wave->grid.has_downbeat = TRUE;
}
static void grid_controls_refresh(PlayerWaveform *wave)
{
    if (!wave->bpm_spin) return;
    wave->spin_updating = TRUE; gtk_spin_button_set_value(GTK_SPIN_BUTTON(wave->bpm_spin), wave->grid.bpm); wave->spin_updating = FALSE;
    gchar *text = wave->grid_cancel ? g_strdup("Kaydediliyor…") :
        wave->grid_dirty ? g_strdup_printf("Kaydedilmedi · %.2f BPM · 1. vuruş %.3f s", wave->grid.bpm, wave->anchor_ms / 1000) :
        !wave->grid_valid ? g_strdup("Güvenilir tempo bulunamadı. BPM girin ya da TAP ile vurun, sonra 1. vuruşu seçin.") :
        wave->grid.manual ? g_strdup_printf("Elle ayarlanmış ızgara · %.2f BPM", wave->grid.bpm) :
        g_strdup_printf("Otomatik analiz · %.2f BPM · güven %.2f", wave->grid.bpm, wave->grid.confidence);
    gtk_label_set_text(GTK_LABEL(wave->grid_status), text); g_free(text);
    gboolean idle = !wave->grid_cancel;
    gtk_widget_set_sensitive(wave->grid_save, idle && wave->grid_dirty && media_duration(wave) >= 4000);
    gtk_widget_set_sensitive(wave->grid_auto, idle);
    gtk_widget_set_sensitive(wave->grid_selected, idle && wave->panel != NULL);
    marker_controls_refresh(wave);
}
static void grid_load(PlayerWaveform *wave)
{
    RadioSRhythmAnalysis stored;
    wave->grid_valid = stored_grid(wave, &stored); wave->grid_dirty = FALSE;
    if (wave->grid_valid) { wave->grid = stored; wave->anchor_ms = stored.has_downbeat ? stored.downbeat_ms : stored.first_beat_ms; }
    else { wave->grid = (RadioSRhythmAnalysis){.bpm = 120, .confidence = 1}; wave->anchor_ms = 0; }
    grid_controls_refresh(wave); panel_grid_sync(wave);
}
static void grid_edited(PlayerWaveform *wave)
{
    wave->grid.bpm = CLAMP(wave->grid.bpm, 60, 200); wave->grid.manual = TRUE; wave->grid.confidence = 1;
    wave->grid_valid = wave->grid_dirty = TRUE; grid_apply(wave); grid_controls_refresh(wave); panel_grid_sync(wave);
}
static void bpm_changed(GtkSpinButton *spin, gpointer data)
{
    PlayerWaveform *wave = data; if (wave->spin_updating) return;
    wave->grid.bpm = gtk_spin_button_get_value(spin); grid_edited(wave);
}
/* Tap tempo: the mean interval of the last taps, folded into 60-200 BPM. */
static void grid_tap(PlayerWaveform *wave)
{
    gint64 now = g_get_monotonic_time();
    if (wave->tap_count && now - wave->taps[(wave->tap_count - 1) % 8] > 2 * G_USEC_PER_SEC) wave->tap_count = 0;
    wave->taps[wave->tap_count % 8] = now; ++wave->tap_count;
    guint count = MIN(wave->tap_count, 8);
    if (count < 4) {
        gchar *text = g_strdup_printf("TAP: müziğe eşlik ederek %u kez daha vurun.", 4 - count);
        gtk_label_set_text(GTK_LABEL(wave->grid_status), text); g_free(text); return;
    }
    gint64 first = wave->taps[(wave->tap_count - count) % 8], last = wave->taps[(wave->tap_count - 1) % 8];
    gdouble bpm = 60.0 * G_USEC_PER_SEC * (count - 1) / MAX(1, last - first);
    while (bpm < 60) bpm *= 2;
    while (bpm > 200) bpm /= 2;
    wave->grid.bpm = round(bpm * 100) / 100; grid_edited(wave);
}
static void grid_action(GtkButton *button, gpointer data)
{
    PlayerWaveform *wave = data; const gchar *action = g_object_get_data(G_OBJECT(button), "grid-action");
    gdouble period = 60000 / wave->grid.bpm;
    if (g_str_equal(action, "tap")) { grid_tap(wave); return; }
    if (g_str_equal(action, "double")) { if (wave->grid.bpm * 2 > 200) return; wave->grid.bpm *= 2; }
    else if (g_str_equal(action, "half")) { if (wave->grid.bpm / 2 < 60) return; wave->grid.bpm /= 2; }
    else if (g_str_has_prefix(action, "nudge:")) wave->anchor_ms += g_ascii_strtod(action + 6, NULL);
    else if (g_str_equal(action, "bar")) wave->anchor_ms += period;
    else if (g_str_equal(action, "selected")) {
        gdouble seconds; if (!wave->panel || !radios_marker_editor_cursor(wave->panel, &seconds)) return;
        wave->anchor_ms = seconds * 1000;
    }
    else if (g_str_equal(action, "playhead")) wave->anchor_ms = (gdouble)wave->snapshot.position_ms;
    grid_edited(wave);
}
static void detail_merge_known(PlayerWaveform *wave, JsonObject *merged, JsonNode *known)
{
    if (!detail_matches(wave, known)) return;
    JsonObject *previous = detail_object(known);
    const gchar *objects[] = {"media", "markers"};
    for (guint i = 0; i < G_N_ELEMENTS(objects); ++i) {
        JsonObject *old = object_member(previous, objects[i]), *current = object_member(merged, objects[i]);
        if (old && radios_json_int(old, "_rev", 0) > radios_json_int(current, "_rev", 0))
            json_object_set_object_member(merged, objects[i], radios_programme_clone_object(old));
    }
    if (radios_json_int(previous, "analysis_revision", 0) > radios_json_int(merged, "analysis_revision", 0)) {
        JsonNode *analysis = json_object_get_member(previous, "analysis");
        if (analysis) json_object_set_member(merged, "analysis", json_node_copy(analysis));
        json_object_set_int_member(merged, "analysis_revision", radios_json_int(previous, "analysis_revision", 0));
    }
}
static void grid_saved(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *node, const GError *error, gpointer unused)
{
    (void)unused; PlayerWaveform *wave = g_object_get_data(G_OBJECT(owner), "radios-player-waveform");
    if (!wave || wave->destroyed) return;
    g_clear_object(&wave->grid_cancel);
    if (error) {
        grid_controls_refresh(wave);
        if (wave->grid_status) {
            gchar *text = g_strdup_printf("Kaydedilemedi: %s", error->message);
            gtk_label_set_text(GTK_LABEL(wave->grid_status), text); g_free(text);
        }
        return;
    }
    JsonObject *detail = detail_object(node), *media = object_member(detail, "media");
    RadioSRhythmAnalysis saved; gboolean known = radios_rhythm_analysis_read(object_member(object_member(detail, "analysis"), "rhythm"), &saved);
    GtkWidget *card = g_weak_ref_get(&wave->card); const gchar *note = NULL; GError *local = NULL;
    if (card && !gtk_widget_in_destruction(card) && detail_matches(wave, node)) {
        /* Grid analysis and cue records have independent revisions. Keep
         * the latest of each from both the displayed detail and the card's
         * cache, which a concurrent GET may have refreshed after this ACK. */
        JsonNode *copy = detail_copy(node); JsonObject *merged = detail_object(copy);
        detail_merge_known(wave, merged, wave->detail);
        detail_merge_known(wave, merged, g_object_get_data(G_OBJECT(card), "radios-deck-media-detail"));
        detail = merged; media = object_member(detail, "media");
        known = radios_rhythm_analysis_read(object_member(object_member(detail, "analysis"), "rhythm"), &saved);
        g_object_set_data_full(G_OBJECT(card), "radios-deck-media-detail", copy, (GDestroyNotify)json_node_free);
        wave->seen_cache = copy; wave->grid_dirty = FALSE; adopt_detail(wave, copy);
        RadioSAudioUiSnapshot snapshot = {0}; RadioSAudioEngine *engine = NULL;
        /* The deck uses the new grid at once unless a running AutoDJ queue froze its own. */
        if (known && radios_audio_ui_get_snapshot(ctx, wave->deck, &snapshot) &&
            g_str_equal(snapshot.media_id, radios_json_string(media, "_id", "")) &&
            g_str_equal(snapshot.version_id, radios_json_string(media, "version_id", "")) &&
            (engine = radios_audio_ui_engine_acquire(ctx)) != NULL &&
            !radios_audio_engine_set_deck_rhythm(engine, wave->deck, snapshot.generation, &saved, &local)) note = local->message;
        if (engine) radios_audio_engine_unref(engine);
    }
    g_clear_object(&card);
    if (wave->grid_status && (!known || note)) {
        gchar *text = !known ? g_strdup("Otomatik analiz güvenilir tempo bulamadı. BPM'i elle girip 1. vuruşu seçin.") :
            g_strdup_printf("Kaydedildi. Oynatıcı yeni ızgarayı bir sonraki yüklemede kullanacak: %s", note);
        gtk_label_set_text(GTK_LABEL(wave->grid_status), text); g_free(text);
    }
    g_clear_error(&local);
}
static void grid_save_clicked(GtkButton *button, gpointer data)
{
    (void)button; PlayerWaveform *wave = data; JsonObject *detail = detail_object(wave->detail);
    if (wave->grid_cancel || !wave->grid_valid || !detail_matches(wave, wave->detail)) return;
    gint64 duration = media_duration(wave); RadioSRhythmAnalysis stored, grid = wave->grid;
    grid.analyzed_ms = stored_grid(wave, &stored) ? stored.analyzed_ms : (guint)MIN(duration, 120000);
    grid.manual = TRUE; grid.confidence = 1;
    if (grid.analyzed_ms < 4000 || grid.downbeat_ms >= grid.analyzed_ms) {
        gtk_label_set_text(GTK_LABEL(wave->grid_status), "Elle ızgara en az 4 saniyelik parçada kaydedilebilir."); return;
    }
    wave->grid_cancel = radios_media_rhythm_save_async(wave->ctx, wave->widget, detail, &grid, grid_saved, NULL, NULL);
    grid_controls_refresh(wave);
}
static void grid_auto_clicked(GtkButton *button, gpointer data)
{
    (void)button; PlayerWaveform *wave = data;
    if (wave->grid_cancel || !detail_matches(wave, wave->detail)) return;
    /* Replaces a manual grid with a fresh automatic analysis of the file. */
    wave->grid_dirty = FALSE;
    wave->grid_cancel = radios_media_rhythm_analyze_async(wave->ctx, wave->widget, wave->snapshot.media_id, grid_saved, NULL, NULL);
    grid_controls_refresh(wave);
}
static GtkWidget *grid_button(PlayerWaveform *wave, GtkWidget *row, const gchar *label, const gchar *suffix, const gchar *action, const gchar *tip)
{
    GtkWidget *button = gtk_button_new_with_label(label);
    gchar *name = g_strdup_printf("deck-%u-grid-%s", wave->deck, suffix); gtk_widget_set_name(button, name); g_free(name);
    if (tip) gtk_widget_set_tooltip_text(button, tip);
    if (action) {
        g_object_set_data_full(G_OBJECT(button), "grid-action", g_strdup(action), g_free);
        g_signal_connect(button, "clicked", G_CALLBACK(grid_action), wave);
    }
    gtk_box_pack_start(GTK_BOX(row), button, FALSE, FALSE, 0); return button;
}
static GtkWidget *grid_editor(PlayerWaveform *wave)
{
    GtkWidget *frame = gtk_frame_new("BPM ve dörtlük ızgarası"), *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gchar *name = g_strdup_printf("deck-%u-grid-editor", wave->deck); gtk_widget_set_name(frame, name); g_free(name);
    gtk_container_set_border_width(GTK_CONTAINER(box), 8); gtk_container_add(GTK_CONTAINER(frame), box);
    GtkWidget *tempo = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6), *shift = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6), *beat = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_pack_start(GTK_BOX(tempo), gtk_label_new("BPM"), FALSE, FALSE, 0);
    wave->bpm_spin = gtk_spin_button_new_with_range(60, 200, 0.01); gtk_spin_button_set_digits(GTK_SPIN_BUTTON(wave->bpm_spin), 2);
    name = g_strdup_printf("deck-%u-grid-bpm", wave->deck); gtk_widget_set_name(wave->bpm_spin, name); g_free(name);
    gtk_widget_set_tooltip_text(wave->bpm_spin, "Parçanın temposu. Izgara seçili 1. vuruşa göre yeniden aralıklanır.");
    g_signal_connect(wave->bpm_spin, "value-changed", G_CALLBACK(bpm_changed), wave);
    gtk_box_pack_start(GTK_BOX(tempo), wave->bpm_spin, FALSE, FALSE, 0);
    grid_button(wave, tempo, "×2", "double", "double", "Tempoyu iki katına çıkar");
    grid_button(wave, tempo, "÷2", "half", "half", "Tempoyu yarıya indir");
    grid_button(wave, tempo, "TAP", "tap", "tap", "Müziğe eşlik ederek en az dört kez tıklayın");
    wave->grid_status = gtk_label_new(""); gtk_label_set_xalign(GTK_LABEL(wave->grid_status), 0);
    gtk_label_set_line_wrap(GTK_LABEL(wave->grid_status), TRUE);
    name = g_strdup_printf("deck-%u-grid-status", wave->deck); gtk_widget_set_name(wave->grid_status, name); g_free(name);
    gtk_box_pack_start(GTK_BOX(tempo), wave->grid_status, TRUE, TRUE, 6);
    gtk_box_pack_start(GTK_BOX(shift), gtk_label_new("Izgarayı kaydır"), FALSE, FALSE, 0);
    grid_button(wave, shift, "◀ 10 ms", "nudge-back-10", "nudge:-10", NULL);
    grid_button(wave, shift, "◀ 1 ms", "nudge-back-1", "nudge:-1", NULL);
    grid_button(wave, shift, "1 ms ▶", "nudge-forward-1", "nudge:1", NULL);
    grid_button(wave, shift, "10 ms ▶", "nudge-forward-10", "nudge:10", NULL);
    grid_button(wave, shift, "Dörtlük başı +1 vuruş", "bar", "bar", "Vuruşlar yerinde kalır; dörtlüğün 1. vuruşu bir sonraki vuruşa geçer.");
    wave->grid_selected = grid_button(wave, beat, "Seçili nokta 1. vuruş", "selected", "selected",
        "Dalga biçiminde tıklanan noktayı dörtlüğün 1. vuruşu yap.");
    grid_button(wave, beat, "Çalma konumu 1. vuruş", "playhead", "playhead",
        "Oynatıcının şu anki konumunu dörtlüğün 1. vuruşu yap. Kick üzerinde duraklatıp kullanın.");
    GtkWidget *spacer = gtk_label_new(""); gtk_box_pack_start(GTK_BOX(beat), spacer, TRUE, TRUE, 0);
    wave->grid_auto = grid_button(wave, beat, "Otomatik analiz", "auto", NULL, "Dosyayı yeniden analiz et; elle ızgaranın yerine otomatik ızgara yazılır.");
    g_signal_connect(wave->grid_auto, "clicked", G_CALLBACK(grid_auto_clicked), wave);
    wave->grid_save = grid_button(wave, beat, "Kaydet", "save", NULL, "Elle ızgarayı veritabanına kaydet. Otomatik analiz bunu değiştirmez.");
    g_signal_connect(wave->grid_save, "clicked", G_CALLBACK(grid_save_clicked), wave);
    GtkWidget *rows[] = {tempo, shift, beat};
    for (guint index = 0; index < G_N_ELEMENTS(rows); ++index) gtk_box_pack_start(GTK_BOX(box), rows[index], FALSE, FALSE, 0);
    return frame;
}
static void marker_clear_clicked(GtkButton *button, gpointer data)
{ (void)button; PlayerWaveform *wave = data; if (wave->panel) radios_marker_editor_clear_autodj(wave->panel); }
static GtkWidget *marker_editor(PlayerWaveform *wave)
{
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4), *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    wave->marker_clear = gtk_button_new_with_label("Noktaları temizle");
    gchar *name = g_strdup_printf("deck-%u-waveform-autodj-clear", wave->deck); gtk_widget_set_name(wave->marker_clear, name); g_free(name);
    gtk_widget_set_tooltip_text(wave->marker_clear, "AutoDJ IN ve OUT noktalarını kaldır; İşaretleri kaydet ile yazılır.");
    g_signal_connect(wave->marker_clear, "clicked", G_CALLBACK(marker_clear_clicked), wave);
    gtk_box_pack_start(GTK_BOX(row), wave->marker_clear, FALSE, FALSE, 0);
    wave->marker_snap = gtk_check_button_new_with_label("Dörtlük başına hizala");
    gchar *snap_name = g_strdup_printf("deck-%u-waveform-autodj-snap", wave->deck); gtk_widget_set_name(wave->marker_snap, snap_name); g_free(snap_name);
    gtk_widget_set_tooltip_text(wave->marker_snap, "AutoDJ IN ve OUT konulurken en yakın dörtlük başına kayar.");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(wave->marker_snap), wave->marker_snapping);
    g_signal_connect(wave->marker_snap, "toggled", G_CALLBACK(marker_snap_changed), wave);
    gtk_box_pack_start(GTK_BOX(row), wave->marker_snap, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    wave->marker_status = gtk_label_new(""); gtk_label_set_xalign(GTK_LABEL(wave->marker_status), 0); gtk_label_set_line_wrap(GTK_LABEL(wave->marker_status), TRUE);
    name = g_strdup_printf("deck-%u-waveform-autodj-status", wave->deck); gtk_widget_set_name(wave->marker_status, name); g_free(name);
    gtk_box_pack_start(GTK_BOX(box), wave->marker_status, FALSE, FALSE, 0);
    marker_controls_refresh(wave); return box;
}
/* The compact card shows stored bar starts; the inspector's editor draws its own grid. */
static void grid_draw(PlayerWaveform *wave, cairo_t *cr, gdouble start, gdouble span, gdouble usable, gdouble height, gint64 duration)
{
    gdouble top = 30, bottom = MAX(top + 1, height - 18); RadioSRhythmAnalysis grid;
    if (stored_grid(wave, &grid) && grid.bpm > 0) {
        gdouble beat = 60000 / grid.bpm, pixels = usable * beat / span;
        gdouble period = beat * 4 * MAX(1, ceil(12 / MAX(.001, pixels * 4))),
                anchor = grid.has_downbeat ? grid.downbeat_ms : grid.first_beat_ms;
        gint64 first = (gint64)ceil((MAX(0, start) - anchor) / period),
               last = (gint64)floor((MIN(start + span, (gdouble)duration) - anchor) / period);
        cairo_set_source_rgba(cr, .55, .72, .77, .32); cairo_set_line_width(cr, 1);
        for (gint64 index = first; index <= last; ++index) {
            gdouble x = 8 + usable * (anchor + index * period - start) / span;
            cairo_move_to(cr, floor(x) + .5, top); cairo_line_to(cr, floor(x) + .5, bottom); cairo_stroke(cr);
        }
    }
    cairo_set_line_width(cr, 1);
    if (wave->selected_valid && wave->selected_ms >= start && wave->selected_ms <= start + span) {
        gdouble x = 8 + usable * (wave->selected_ms - start) / span, dash[] = {4, 3};
        cairo_set_source_rgba(cr, .7, .9, .92, .75); cairo_set_dash(cr, dash, 2, 0);
        cairo_move_to(cr, x, top); cairo_line_to(cr, x, bottom); cairo_stroke(cr); cairo_set_dash(cr, NULL, 0, 0);
    }
}
static void panel_grid_sync(PlayerWaveform *wave)
{ if (wave->panel) radios_marker_editor_set_grid(wave->panel, wave->grid_valid ? &wave->grid : NULL); }
static void panel_saved(GtkWidget *panel, JsonObject *detail, gpointer data)
{
    (void)panel; PlayerWaveform *wave = data; if (wave->destroyed || !detail) return;
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); json_node_set_object(node, detail);
    GtkWidget *card = g_weak_ref_get(&wave->card);
    if (!detail_matches(wave, node)) marker_note(wave, "Oynatıcı değişti; kaydedilen noktalar asıl parçada korunuyor.");
    else if (card && !gtk_widget_in_destruction(card)) {
        /* Marker and analysis saves can finish in either order: keep the newest of each. */
        JsonNode *cache = detail_copy(node); JsonObject *merged = detail_object(cache);
        detail_merge_known(wave, merged, wave->detail);
        detail_merge_known(wave, merged, g_object_get_data(G_OBJECT(card), "radios-deck-media-detail"));
        g_object_set_data_full(G_OBJECT(card), "radios-deck-media-detail", cache, (GDestroyNotify)json_node_free);
        wave->seen_cache = cache; adopt_detail(wave, cache);
        marker_note(wave, "Kayıt tamamlandı; güncel AutoDJ IN/OUT noktaları gösteriliyor.");
    }
    g_clear_object(&card); json_node_free(node);
}
static void panel_build(PlayerWaveform *wave, GtkWidget *body)
{
    wave->panel = radios_marker_editor_embed(wave->ctx, wave->dialog, detail_object(wave->detail));
    radios_marker_editor_set_saved(wave->panel, panel_saved, wave);
    radios_marker_editor_set_snap(wave->panel, wave->marker_snapping);
    panel_grid_sync(wave);
    gtk_box_pack_start(GTK_BOX(body), wave->panel, FALSE, FALSE, 0);
    gtk_box_reorder_child(GTK_BOX(body), wave->panel, 0);
    gtk_widget_show_all(wave->panel);
}
/* A detail refreshed elsewhere (library save, new analysis) replaces a clean panel. */
static void panel_refresh(PlayerWaveform *wave)
{
    JsonObject *shown = radios_marker_editor_detail(wave->panel), *current = detail_object(wave->detail);
    if (!current || radios_marker_editor_dirty(wave->panel)) return;
    gboolean same = shown &&
        radios_json_int(object_member(shown, "media"), "_rev", 0) == radios_json_int(object_member(current, "media"), "_rev", 0) &&
        radios_json_int(object_member(shown, "markers"), "_rev", 0) == radios_json_int(object_member(current, "markers"), "_rev", 0) &&
        radios_json_int(shown, "analysis_revision", 0) == radios_json_int(current, "analysis_revision", 0);
    if (same) return;
    GtkWidget *body = gtk_widget_get_parent(wave->panel);
    gtk_widget_destroy(wave->panel); wave->panel = NULL;
    panel_build(wave, body); marker_controls_refresh(wave);
}
static void inspect_open(PlayerWaveform *wave)
{
    gint64 duration = 0; if (wave->destroyed || !view_available(wave, &duration)) return;
    if (wave->dialog) { gtk_window_present(GTK_WINDOW(wave->dialog)); return; }
    gchar *title = g_strdup_printf("Oynatıcı %u · Dalga biçimini incele", wave->deck + 1);
    GtkWidget *dialog = gtk_dialog_new_with_buttons(title, wave->ctx->parent, GTK_DIALOG_DESTROY_WITH_PARENT,
        "Kapat", GTK_RESPONSE_CLOSE, NULL); g_free(title); wave->dialog = dialog;
    gchar *name = g_strdup_printf("deck-%u-waveform-dialog", wave->deck); gtk_widget_set_name(dialog, name); g_free(name);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 1000, 900);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_container_set_border_width(GTK_CONTAINER(body), 12);
    gtk_container_add(GTK_CONTAINER(scroll), body);
    gtk_box_pack_start(GTK_BOX(gtk_dialog_get_content_area(GTK_DIALOG(dialog))), scroll, TRUE, TRUE, 0);
    /* The media library's marker editor, with the deck's beat grid on its timeline. */
    gtk_box_pack_start(GTK_BOX(body), grid_editor(wave), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(body), marker_editor(wave), FALSE, FALSE, 0);
    panel_build(wave, body);
    g_signal_connect(dialog, "destroy", G_CALLBACK(inspection_destroy), wave);
    g_signal_connect_swapped(dialog, "response", G_CALLBACK(gtk_widget_destroy), dialog);
    gtk_widget_show_all(dialog); grid_load(wave); view_refresh(wave);
}

static void draw_text(GtkWidget *widget, cairo_t *cr, const gchar *text, gdouble x, gdouble y, gint width)
{
    PangoLayout *layout = gtk_widget_create_pango_layout(widget, text);
    PangoFontDescription *font = pango_font_description_from_string("Sans 8");
    pango_layout_set_font_description(layout, font); pango_font_description_free(font);
    if (width > 0) { pango_layout_set_width(layout, width * PANGO_SCALE); pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END); }
    cairo_move_to(cr, x, y); pango_cairo_show_layout(cr, layout); g_object_unref(layout);
}
static gboolean waveform_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    PlayerWaveform *wave = data;
    gdouble width = gtk_widget_get_allocated_width(widget), height = gtk_widget_get_allocated_height(widget), usable = MAX(1, width - 16);
    cairo_rectangle(cr, 0, 0, width, height); cairo_clip(cr);
    cairo_set_source_rgb(cr, 0.055, 0.07, 0.075); cairo_paint(cr);
    if (wave->online || !detail_matches(wave, wave->detail)) {
        cairo_set_source_rgb(cr, 0.58, 0.62, 0.63);
        draw_text(widget, cr, wave->online ? "Çevrimiçi kaynak" : wave->snapshot.media_id[0] ?
                  (wave->message ? wave->message : "Dalga biçimi yükleniyor…") : "Dalga biçimi / Cue", 8, height / 2 - 8, (gint)usable);
        return FALSE;
    }
    gint64 duration = radios_json_int(object_member(detail_object(wave->detail), "version"), "duration_ms", 0), hop = 0, rate = 0;
    JsonArray *array = valid_waveform(wave, &hop, &rate);
    gdouble start = wave->range ? gtk_adjustment_get_value(wave->range) : 0;
    gdouble span = wave->range ? gtk_adjustment_get_page_size(wave->range) : duration;
    span = MAX(1, span);
    gdouble top = 30, bottom = MAX(top + 1, height - 18), center = (top + bottom) / 2, amplitude = MAX(1, (bottom - top) / 2 - 1);
    cairo_set_source_rgb(cr, 0.15, 0.19, 0.19); cairo_set_line_width(cr, 1);
    for (guint tick = 0; tick <= 4; ++tick) {
        gdouble x = 8 + usable * tick / 4; cairo_move_to(cr, x, top); cairo_line_to(cr, x, bottom); cairo_stroke(cr);
    }
    grid_draw(wave, cr, start, span, usable, height, duration);
    if (array) {
        guint columns = (guint)CLAMP(floor(usable), 1, 4096);
        gdouble *lows = g_new(gdouble, columns), *highs = g_new(gdouble, columns);
        for (guint column = 0; column < columns; ++column) { lows[column] = G_MAXDOUBLE; highs[column] = -G_MAXDOUBLE; }
        for (guint index = 0; index < json_array_get_length(array) / 2; ++index) {
            gdouble time_ms = (gdouble)index * hop * 1000 / rate;
            if (time_ms > duration || time_ms > start + span) break;
            if (time_ms + (gdouble)hop * 1000 / rate < start) continue;
            gdouble low = json_array_get_double_element(array, index * 2), high = json_array_get_double_element(array, index * 2 + 1);
            guint column = (guint)CLAMP(floor(columns * (time_ms - start) / span), 0, columns - 1);
            lows[column] = MIN(lows[column], low); highs[column] = MAX(highs[column], high);
        }
        for (guint column = 0; column < columns; ++column) if (lows[column] <= highs[column]) {
            gdouble x = 8 + usable * (column + .5) / columns;
            cairo_set_source_rgb(cr, .3, .7, .69); cairo_set_line_width(cr, 1);
            cairo_move_to(cr, x, center - CLAMP(highs[column], -1, 1) * amplitude);
            cairo_line_to(cr, x, center - CLAMP(lows[column], -1, 1) * amplitude); cairo_stroke(cr);
            if (lows[column] <= -1 || highs[column] >= 1) {
                cairo_set_source_rgba(cr, .86, .72, .45, .7);
                cairo_rectangle(cr, x - .5, highs[column] >= 1 ? top : bottom - 2, 1, 2); cairo_fill(cr);
            }
        }
        g_free(lows); g_free(highs);
    } else {
        cairo_set_source_rgb(cr, 0.58, 0.62, 0.63); draw_text(widget, cr, "Ölçülmüş dalga biçimi bekleniyor", 8, height / 2 - 8, (gint)usable);
    }
    JsonObject *markers = valid_markers(wave); gint64 value;
    const guint priority[] = {6, 7, 0, 2, 1, 3, 4, 5}; guint occupied_count[2] = {0, 0}; gdouble occupied[2][8], occupied_width[2][8];
    for (guint position_index = 0; position_index < G_N_ELEMENTS(priority); ++position_index) {
        guint index = priority[position_index];
        if (index != 0 && index != 2 && index < 6) continue;
        if (!marker_read(markers, index, duration, &value)) continue;
        if (value < start || value > start + span) continue;
        gdouble x = 8 + usable * (value - start) / span;
        if (index == 6) cairo_set_source_rgb(cr, .32, .88, .7);
        else if (index == 7) cairo_set_source_rgb(cr, .7, .65, .94);
        else cairo_set_source_rgba(cr, .62, .75, .76, .7);
        cairo_set_line_width(cr, 1); cairo_move_to(cr, x, top); cairo_line_to(cr, x, bottom); cairo_stroke(cr);
        gint label_width = index >= 6 ? 74 : 28;
        for (guint lane = 0; lane < 2; ++lane) {
            gdouble right = lane ? width - 8 : width - 110;
            if (right < 8 + label_width) continue;
            gdouble label_x = CLAMP(x - label_width / 2.0, 8, right - label_width); gboolean collision = FALSE;
            for (guint i = 0; i < occupied_count[lane]; ++i)
                if (label_x < occupied[lane][i] + occupied_width[lane][i] + 4 && label_x + label_width + 4 > occupied[lane][i]) collision = TRUE;
            if (collision) continue;
            occupied[lane][occupied_count[lane]] = label_x; occupied_width[lane][occupied_count[lane]++] = label_width;
            draw_text(widget, cr, marker_tags[index], label_x, 1 + lane * 14, label_width); break;
        }
    }
    gdouble position_ms = MIN(wave->snapshot.position_ms, (guint64)duration);
    if (position_ms < start || position_ms > start + span) return FALSE;
    gdouble position = 8 + usable * (position_ms - start) / span;
    cairo_set_source_rgb(cr, 0.97, 0.97, 0.94); cairo_set_line_width(cr, 1.5);
    cairo_move_to(cr, position, top); cairo_line_to(cr, position, bottom); cairo_stroke(cr);
    return FALSE;
}
static void cancel_request(PlayerWaveform *wave)
{
    ++wave->serial;
    if (wave->cancel) g_cancellable_cancel(wave->cancel);
    g_clear_object(&wave->cancel);
}
static void wave_request_free(gpointer data)
{
    WaveRequest *request = data; g_weak_ref_clear(&request->wave); g_weak_ref_clear(&request->card);
    radios_data_client_unref(request->client); g_free(request);
}
static void wave_work(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    (void)source; WaveRequest *request = data; GError *error = NULL;
    JsonNode *body = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new(); json_node_take_object(body, object);
    json_object_set_string_member(object, "station_id", radios_data_client_station_id(request->client));
    json_object_set_string_member(object, "media_id", request->media_id);
    JsonNode *detail = radios_data_client_request_cancellable_sync(request->client, "POST", "/v1/workflows/desktop-media/get", body, NULL, cancel, &error);
    json_node_free(body);
    if (error) { g_clear_pointer(&detail, json_node_free); g_task_return_error(task, error); }
    else g_task_return_pointer(task, detail, (GDestroyNotify)json_node_free);
}
static void wave_done(GObject *source, GAsyncResult *result, gpointer unused)
{
    (void)source; (void)unused; GTask *task = G_TASK(result); WaveRequest *request = g_task_get_task_data(task);
    GError *error = NULL; JsonNode *detail = g_task_propagate_pointer(task, &error);
    GtkWidget *widget = g_weak_ref_get(&request->wave), *card = g_weak_ref_get(&request->card);
    PlayerWaveform *wave = widget ? g_object_get_data(G_OBJECT(widget), "radios-player-waveform") : NULL;
    if (wave && card && !wave->destroyed && !gtk_widget_in_destruction(card) && request->serial == wave->serial) {
        g_clear_object(&wave->cancel);
        RadioSDataClient *current = radios_data_client_acquire(wave->ctx); RadioSAudioUiSnapshot snapshot = {0};
        gboolean same = radios_data_connected(wave->ctx) && current == request->client && !wave->online &&
            radios_audio_ui_get_snapshot(wave->ctx, wave->deck, &snapshot) && snapshot.generation == request->generation &&
            g_str_equal(snapshot.media_id, request->media_id) && g_str_equal(snapshot.version_id, request->version_id) &&
            wave->snapshot.generation == request->generation && g_str_equal(wave->snapshot.media_id, request->media_id) &&
            g_str_equal(wave->snapshot.version_id, request->version_id);
        if (same && !error && detail_matches(wave, detail)) {
            JsonNode *cache = g_object_get_data(G_OBJECT(card), "radios-deck-media-detail");
            if ((!detail_matches(wave, cache) || detail_not_older(detail, cache)) && detail_not_older(detail, wave->detail)) {
                adopt_detail(wave, detail);
                JsonNode *copy = detail_copy(detail);
                g_object_set_data_full(G_OBJECT(card), "radios-deck-media-detail", copy, (GDestroyNotify)json_node_free);
                wave->seen_cache = copy;
            } else if (detail_matches(wave, cache) && detail_not_older(cache, wave->detail)) {
                adopt_detail(wave, cache); wave->seen_cache = cache;
            }
        } else if (same && error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_free(wave->message); wave->message = g_strdup("Dalga biçimi güncellenemedi."); describe(wave); gtk_widget_queue_draw(widget);
        }
        if (current) radios_data_client_unref(current);
    }
    g_clear_object(&card); g_clear_object(&widget); g_clear_error(&error); g_clear_pointer(&detail, json_node_free);
}
static void fetch_detail(PlayerWaveform *wave, GtkWidget *card)
{
    if (!wave->client || wave->cancel || wave->destroyed || wave->online || !g_uuid_string_is_valid(wave->snapshot.media_id) ||
        !g_uuid_string_is_valid(wave->snapshot.version_id)) return;
    wave->refresh_after = g_get_monotonic_time() + WAVE_REFRESH_US;
    WaveRequest *request = g_new0(WaveRequest, 1); request->client = radios_data_client_ref(wave->client);
    request->serial = wave->serial; request->generation = wave->snapshot.generation;
    g_strlcpy(request->media_id, wave->snapshot.media_id, sizeof request->media_id); g_strlcpy(request->version_id, wave->snapshot.version_id, sizeof request->version_id);
    g_weak_ref_init(&request->wave, wave->widget); g_weak_ref_init(&request->card, card);
    wave->cancel = g_cancellable_new(); GTask *task = g_task_new(NULL, wave->cancel, wave_done, NULL);
    g_task_set_task_data(task, request, wave_request_free); g_task_run_in_thread(task, wave_work); g_object_unref(task);
}
static void wave_refresh(RadioSViewContext *ctx, GtkWidget *widget)
{
    (void)ctx; PlayerWaveform *wave = g_object_get_data(G_OBJECT(widget), "radios-player-waveform");
    if (wave && !wave->destroyed) wave->refresh_after = 0;
}
static void wave_destroy(GtkWidget *widget, gpointer data)
{
    (void)widget; PlayerWaveform *wave = data; wave->destroyed = TRUE;
    if (wave->dialog) gtk_widget_destroy(wave->dialog);
    if (wave->marker_menu) gtk_widget_destroy(wave->marker_menu);
    g_object_set_data(G_OBJECT(wave->overlay), "radios-player-waveform", NULL);
    cancel_request(wave);
    if (wave->grid_cancel) g_cancellable_cancel(wave->grid_cancel);
}
static void wave_free(gpointer data)
{
    PlayerWaveform *wave = data; cancel_request(wave); g_weak_ref_clear(&wave->card); g_clear_object(&wave->grid_cancel);
    g_clear_object(&wave->marker_menu);
    if (wave->client) radios_data_client_unref(wave->client);
    g_clear_pointer(&wave->detail, json_node_free); g_clear_object(&wave->range); g_clear_object(&wave->controls_theme); g_free(wave->message); g_free(wave->marker_message); g_free(wave);
}
GtkWidget *radios_player_waveform_new(RadioSViewContext *ctx, GtkWidget *card, guint deck)
{
    g_return_val_if_fail(ctx && GTK_IS_WIDGET(card) && deck < 32, NULL);
    GtkWidget *widget = gtk_drawing_area_new(); PlayerWaveform *wave = g_new0(PlayerWaveform, 1);
    wave->ctx = ctx; wave->widget = widget; wave->deck = deck; wave->zoom = 1; wave->marker_snapping = TRUE; g_weak_ref_init(&wave->card, card);
    wave->overlay = gtk_overlay_new(); gtk_container_add(GTK_CONTAINER(wave->overlay), widget);
    g_object_set_data(G_OBJECT(wave->overlay), "radios-player-waveform", wave);
    wave->range = gtk_adjustment_new(0, 0, 1, 1, 1, 1); g_object_ref_sink(wave->range);
    g_signal_connect(wave->range, "value-changed", G_CALLBACK(range_changed), wave);
    wave->controls_theme = gtk_css_provider_new();
    gtk_css_provider_load_from_data(wave->controls_theme,
        ".lp-waveform-control {min-width:16px;min-height:16px;padding:0px 2px;margin:0px;font-size:10px;}"
        ".lp-waveform-pan {min-height:9px;} .lp-waveform-pan slider {min-height:7px;min-width:14px;}"
        ".lp-waveform-time {font-size:11px;color:#d0dcdf;background-color:rgba(8,13,15,0.78);padding:1px 4px;}", -1, NULL);
    GtkWidget *controls = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 2);
    gtk_widget_set_halign(controls, GTK_ALIGN_END); gtk_widget_set_valign(controls, GTK_ALIGN_START);
    wave->zoom_in = wave_button(wave, "+", "zoom-in", 1); wave->zoom_out = wave_button(wave, "−", "zoom-out", -1);
    wave->reset = wave_button(wave, "Tümü", "reset", 0); wave->inspect = wave_button(wave, "İncele", "inspect", 2);
    gtk_widget_set_tooltip_text(wave->zoom_in, "Dalga biçimini yakınlaştır"); gtk_widget_set_tooltip_text(wave->zoom_out, "Dalga biçimini uzaklaştır");
    gtk_widget_set_tooltip_text(wave->reset, "Tüm parçayı göster"); gtk_widget_set_tooltip_text(wave->inspect, "Büyük dalga biçimi penceresinde incele");
    GtkWidget *buttons[] = {wave->zoom_in, wave->zoom_out, wave->reset, wave->inspect};
    for (guint i = 0; i < G_N_ELEMENTS(buttons); ++i) gtk_box_pack_start(GTK_BOX(controls), buttons[i], FALSE, FALSE, 0);
    gtk_overlay_add_overlay(GTK_OVERLAY(wave->overlay), controls);
    wave->pan = gtk_scrollbar_new(GTK_ORIENTATION_HORIZONTAL, wave->range);
    gtk_widget_set_halign(wave->pan, GTK_ALIGN_FILL); gtk_widget_set_valign(wave->pan, GTK_ALIGN_END);
    gtk_widget_set_no_show_all(wave->pan, TRUE); gtk_style_context_add_class(gtk_widget_get_style_context(wave->pan), "lp-waveform-pan");
    gtk_style_context_add_provider(gtk_widget_get_style_context(wave->pan), GTK_STYLE_PROVIDER(wave->controls_theme), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    gtk_overlay_add_overlay(GTK_OVERLAY(wave->overlay), wave->pan);
    wave->time = gtk_label_new("Dalga biçimi / Cue"); gtk_widget_set_halign(wave->time, GTK_ALIGN_START); gtk_widget_set_valign(wave->time, GTK_ALIGN_END);
    gtk_style_context_add_class(gtk_widget_get_style_context(wave->time), "lp-waveform-time");
    gtk_style_context_add_provider(gtk_widget_get_style_context(wave->time), GTK_STYLE_PROVIDER(wave->controls_theme), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    gtk_widget_set_margin_bottom(wave->time, 11); gtk_label_set_ellipsize(GTK_LABEL(wave->time), PANGO_ELLIPSIZE_END);
    gtk_overlay_add_overlay(GTK_OVERLAY(wave->overlay), wave->time); gtk_overlay_set_overlay_pass_through(GTK_OVERLAY(wave->overlay), wave->time, TRUE);
    g_object_set_data_full(G_OBJECT(widget), "radios-player-waveform", wave, wave_free);
    gchar *name = g_strdup_printf("deck-%u-waveform", deck); gtk_widget_set_name(widget, name); g_free(name);
    name = g_strdup_printf("deck-%u-waveform-pan", deck); gtk_widget_set_name(wave->pan, name); g_free(name);
    name = g_strdup_printf("deck-%u-waveform-time", deck); gtk_widget_set_name(wave->time, name); g_free(name);
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), "lp-waveform");
    gtk_widget_set_size_request(widget, 120, 76); gtk_widget_set_hexpand(widget, TRUE);
    AtkObject *accessible = gtk_widget_get_accessible(widget); atk_object_set_role(accessible, ATK_ROLE_IMAGE);
    name = g_strdup_printf("Oynatıcı %u dalga biçimi ve cue noktaları", deck + 1); atk_object_set_name(accessible, name); g_free(name);
    g_signal_connect(widget, "draw", G_CALLBACK(waveform_draw), wave); g_signal_connect(widget, "destroy", G_CALLBACK(wave_destroy), wave);
    waveform_events(wave, widget);
    radios_data_register_refresh(ctx, widget, wave_refresh); describe(wave); view_refresh(wave); return wave->overlay;
}
void radios_player_waveform_update(GtkWidget *widget, const RadioSAudioUiSnapshot *snapshot, gboolean online)
{
    g_return_if_fail(GTK_IS_WIDGET(widget) && snapshot);
    PlayerWaveform *wave = g_object_get_data(G_OBJECT(widget), "radios-player-waveform"); if (!wave || wave->destroyed) return;
    RadioSDataClient *current = radios_data_client_acquire(wave->ctx);
    gboolean identity_changed = online != wave->online || current != wave->client ||
        !g_str_equal(snapshot->media_id, wave->snapshot.media_id) || !g_str_equal(snapshot->version_id, wave->snapshot.version_id);
    gboolean generation_changed = snapshot->generation != wave->snapshot.generation;
    if (identity_changed || generation_changed) {
        ++wave->marker_ticket; wave->marker_busy = FALSE; wave->selected_valid = FALSE;
        if (wave->marker_message) g_clear_pointer(&wave->message, g_free);
        g_clear_pointer(&wave->marker_message, g_free);
        cancel_request(wave);
        if (identity_changed) {
            reset_view(wave, TRUE);
            wave->refresh_after = 0;
            g_clear_pointer(&wave->detail, json_node_free); wave->seen_cache = NULL; g_clear_pointer(&wave->message, g_free);
        } else if (!wave->detail) {
            /* Dragging the seek bar changes the load generation repeatedly.
             * Cancel obsolete replies and wait for the gesture to settle. A
             * measured cache for the same immutable version remains valid. */
            wave->refresh_after = g_get_monotonic_time() + G_TIME_SPAN_SECOND;
        }
    }
    if (current != wave->client) {
        if (wave->client) radios_data_client_unref(wave->client);
        wave->client = current; current = NULL;
    }
    if (current) radios_data_client_unref(current);
    wave->snapshot = *snapshot; wave->online = online;
    GtkWidget *card = g_weak_ref_get(&wave->card);
    if (card && !gtk_widget_in_destruction(card) && !online && snapshot->media_id[0]) {
        JsonNode *cache = g_object_get_data(G_OBJECT(card), "radios-deck-media-detail");
        if (cache != wave->seen_cache && detail_matches(wave, cache) && detail_not_older(cache, wave->detail)) {
            gboolean first_detail = wave->detail == NULL;
            wave->seen_cache = cache; adopt_detail(wave, cache);
            if (first_detail) wave->refresh_after = g_get_monotonic_time() + WAVE_REFRESH_US;
        }
        if (g_get_monotonic_time() >= wave->refresh_after) fetch_detail(wave, card);
    }
    g_clear_object(&card);
    if (identity_changed || generation_changed) describe(wave);
    view_refresh(wave);
}
gboolean radios_player_waveform_edit_grid(GtkWidget *widget)
{
    g_return_val_if_fail(GTK_IS_WIDGET(widget), FALSE);
    PlayerWaveform *wave = g_object_get_data(G_OBJECT(widget), "radios-player-waveform"); gint64 duration = 0;
    if (!wave || wave->destroyed || !view_available(wave, &duration)) return FALSE;
    inspect_open(wave);
    if (wave->bpm_spin) gtk_widget_grab_focus(wave->bpm_spin);
    return TRUE;
}
