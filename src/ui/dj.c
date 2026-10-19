#include "radios/widgets.h"
#include "radios/data.h"
#include "radios/audio_ui.h"
#include "radios/audio_dsp.h"
#include "radios/media_ui.h"
#include "radios/player_transport.h"
#include "radios/content_scale.h"
#include "radios/ui_preferences.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define DJ_FX_PEDALS 4u

typedef struct ChannelUI ChannelUI;
typedef struct {
    GtkWidget *type, *depth, *cue, *power, *tempo;
    GtkWidget *display_effect, *display_state, *display_cue, *display_bpm, *display_depth, *display_beat, *display_depth_bar;
    gdouble display_bpm_value;
} FxUI;
typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *channels, *count, *rehearsal_time, *rehearsal_toggle;
    guint channel_count, next_channel, timer, channel_save_timer;
    gint64 rehearsal_started, rehearsal_elapsed;
    gboolean rehearsal_running, disposed;
    GtkWidget *fx_incoming[DJ_FX_PEDALS];
    GtkWidget *master, *rack_view;
    gint strip_width;
    guint strip_width_source;
    GtkWidget *page, *mixer;
    GtkWidget *mixer_scale, *effector_scale;
    GtkWidget *mixer_dock, *mixer_placeholder, *mixer_window, *mixer_window_toggle;
    gboolean mixer_drag_pending, closing;
    gint mixer_drag_x, mixer_drag_y;
    GPtrArray *pads;
    RadioSDocument *sampler, *channel_document;
    guint building_channel;
    gboolean loading, channel_pending;
    gboolean syncing;
    guint audio_timer;
    guint source_hash; /* capture devices + saved network sources shown in the source menus */
    FxUI fx[DJ_FX_PEDALS];
    GtkWidget *crossfader, *mixer_reload, *microphone_gate, *on_air, *push_to_talk, *speech_level, *speech_ducking, *speech_status, *speech_effector, *speech_to_master, *speech_channel;
    gboolean talking;
    GtkCssProvider *pedal_theme;
    GdkScreen *theme_screen;
    gchar *theme_error;
    guint theme_notice;
} DJState;

struct ChannelUI {
    DJState *owner;
    GtkWidget *widget, *name, *source, *effector, *send, *return_gain, *pan, *eq[3], *fader, *meter;
    GtkWidget *cue, *mute, *solo, *cross_group, *load, *play, *pause, *stop, *seek, *position;
    GtkWidget *depth, *power;
    GtkWidget *send_target, *return_source, *route_send, *route_return;
    GtkWidget *normalize, *normalization;
    GtkWidget *label; /* DJ / Konuşma / Müzik (R5.2) */
    guint index;
    gboolean master, disposed;
};

typedef struct {
    DJState *owner;
    GtkWidget *button, *edit, *title_label, *status_label, *dialog;
    gchar *title, *file, *channel_name;
    guint number;
    gchar *key;
    gboolean selected, disposed;
    gchar *media_id, *version_id, *media_file_id, *media_sha256;
    guint channel_number;
    guint64 play_epoch;
    guint64 generation;
    gboolean pending, loading;
} PadState;

typedef struct {
    PadState *pad;
    GtkWidget *title, *file, *error;
    GtkWidget *library;
    gchar *media_id, *version_id, *media_file_id, *media_sha256;
} PadEditor;

static void sampler_save(DJState *state);
static void sampler_apply(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data);
static void channel_save(DJState *state);
static void channels_apply(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data);
static void channels_ensure(DJState *state, guint count);
static void pad_editor_changed(GtkWidget *widget, gpointer data);
static gboolean audio_tick(gpointer data);
static gboolean any_cue(const RadioSAudioMixerConfig *config);
static void channel_control_changed(GtkWidget *widget, gpointer data);
static void channel_gain_changed(GtkRange *range, gpointer data);
static void fx_control_changed(GtkWidget *widget, gpointer data);
static void speech_changed(GtkWidget *widget, gpointer data);
static void microphone_changed(GtkToggleButton *widget, gpointer data);
static void talk_release(DJState *state);
static gboolean talk_window_focus_lost(GtkWidget *widget, GdkEventFocus *event, gpointer data);
static void channel_transport(GtkButton *widget, gpointer data);
static void channel_load(GtkButton *widget, gpointer data);
static gboolean channel_seek(GtkRange *range, GtkScrollType type, gdouble position, gpointer data);
static void channel_label_changed(GtkComboBox *combo, gpointer data);
static void source_options_fill(DJState *state, ChannelUI *ui);
static void channel_input_select(ChannelUI *ui, const gchar *id);
static void network_source_dialog_open(ChannelUI *ui);
static GtkWidget *channel_find(DJState *state, guint number)
{
    GList *children = gtk_container_get_children(GTK_CONTAINER(state->channels)); GtkWidget *found = NULL;
    for (GList *item = children; item; item = item->next) {
        ChannelUI *ui = g_object_get_data(G_OBJECT(item->data), "dj-channel-state");
        if (ui && ui->index + 1 == number) { found = item->data; break; }
    }
    g_list_free(children); return found;
}
static void channel_ui_destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget; ChannelUI *channel = data; channel->disposed = TRUE;
    if (channel->owner->ctx->audio)
        radios_audio_ui_stop(channel->owner->ctx, channel->master ? RADIOS_AUDIO_MASTER_SAMPLER : RADIOS_AUDIO_SAMPLER_BASE + channel->index, NULL);
}
static void dj_free(gpointer data) { DJState *state = data; g_free(state->theme_error); g_ptr_array_unref(state->pads); g_free(state); }
/* The strips share the mixer row: every channel and MASTER get the same
 * width, so a wide or zoomed-out mixer fills its row instead of opening a gap
 * before MASTER. A strip never goes under 162 px; more channels than fit
 * scroll inside the rack. The split follows the rack viewport and the number
 * of channels, so it is redone after a resize, an added or a removed channel. */
static gboolean
strips_resize(gpointer data)
{
    DJState *state = data;
    state->strip_width_source = 0;
    gint view = state->rack_view ? gtk_widget_get_allocated_width(state->rack_view) : 0;
    if (state->disposed || view <= 1) return G_SOURCE_REMOVE;
    GList *strips = gtk_container_get_children(GTK_CONTAINER(state->channels));
    gint channels = 0;
    for (GList *item = strips; item; item = item->next) if (gtk_widget_get_visible(item->data)) ++channels;
    gint gaps = MAX(0, channels - 1) * gtk_box_get_spacing(GTK_BOX(state->channels)) +
        gtk_box_get_spacing(GTK_BOX(gtk_widget_get_parent(state->channels)));
    gint width = MAX(162, (view - gaps) / (channels + 1));
    if (state->master) strips = g_list_append(strips, state->master);
    if (width != state->strip_width) {
        state->strip_width = width;
        for (GList *item = strips; item; item = item->next) gtk_widget_set_size_request(item->data, width, -1);
    }
    g_list_free(strips);
    return G_SOURCE_REMOVE;
}
static void
strips_changed(DJState *state)
{
    if (!state->disposed && !state->strip_width_source) state->strip_width_source = g_idle_add(strips_resize, state);
}
static void
rack_view_allocated(GtkWidget *view, GtkAllocation *allocation, gpointer data)
{ (void)view; (void)allocation; strips_changed(data); }
static const gchar *const effects[] = {
    "Delay", "Echo", "Spiral", "Reverb", "Trans", "Filter", "Flanger", "Phaser",
    "Robot", "Melodic", "Slip Roll", "Roll", "Rev Roll", "MIDI LFO", NULL
};

static void
pack(GtkWidget *box, GtkWidget *widget, gboolean expand)
{
    gtk_box_pack_start(GTK_BOX(box), widget, expand, expand, 0);
}

/* The controller stays on the DJ page; only its live mixer changes parent. */
static void
mixer_move_to(DJState *state, GtkWidget *box)
{
    GtkWidget *parent = gtk_widget_get_parent(state->mixer);
    g_object_ref(state->mixer);
    if (parent) gtk_container_remove(GTK_CONTAINER(parent), state->mixer);
    pack(box, state->mixer, FALSE);
    g_object_unref(state->mixer);
}

static void
mixer_dialogs_parent(DJState *state, GtkWindow *parent)
{
    for (guint i = 0; i < state->pads->len; ++i) {
        PadState *pad = g_ptr_array_index(state->pads, i);
        if (!pad->disposed && pad->dialog)
            gtk_window_set_transient_for(GTK_WINDOW(pad->dialog), parent);
    }
}

static void
mixer_redock(DJState *state)
{
    if (!state->mixer_window || state->disposed) return;
    GtkWidget *window = state->mixer_window;
    state->mixer_window = NULL;
    state->mixer_drag_pending = FALSE;
    talk_release(state);
    mixer_move_to(state, state->mixer_dock);
    mixer_dialogs_parent(state, state->ctx->parent);
    gtk_widget_hide(state->mixer_placeholder);
    gtk_button_set_label(GTK_BUTTON(state->mixer_window_toggle), "Pencerede aç");
    gtk_widget_set_tooltip_text(state->mixer_window_toggle, "Mixer'i ayrı ve taşınabilir bir pencerede aç.");
    gtk_widget_show_all(state->mixer);
    g_signal_handlers_disconnect_by_data(window, state);
    gtk_widget_destroy(window);
}

static gboolean
mixer_window_delete(GtkWidget *widget, GdkEvent *event, gpointer data)
{
    (void)widget; (void)event;
    mixer_redock(data);
    return TRUE;
}

static void
mixer_window_destroyed(GtkWidget *widget, gpointer data)
{
    DJState *state = data;
    if (state->mixer_window == widget) mixer_redock(state);
}

static void
mixer_detach(DJState *state, GdkEventMotion *drag)
{
    if (state->disposed || state->closing) return;
    if (state->mixer_window) {
        gtk_window_present(GTK_WINDOW(state->mixer_window));
        return;
    }
    talk_release(state);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *body = radios_ui_box(GTK_ORIENTATION_VERTICAL, 0, "dj-view task-view");
    state->mixer_window = window;
    gtk_widget_set_name(window, "dj-mixer-window");
    gtk_widget_set_name(scroll, "dj-mixer-window-scroll");
    /* Pedal styling uses this ancestor in both the page and the window. */
    gtk_widget_set_name(body, "dj-task-view");
    gtk_style_context_add_class(gtk_widget_get_style_context(window), "radios");
    gtk_window_set_title(GTK_WINDOW(window), "RadioS · Mixer");
    gtk_window_set_screen(GTK_WINDOW(window), gtk_widget_get_screen(state->page));
    gtk_window_set_resizable(GTK_WINDOW(window), TRUE);
    if (state->ctx->parent) {
        gtk_window_set_transient_for(GTK_WINDOW(window), state->ctx->parent);
        gtk_window_set_destroy_with_parent(GTK_WINDOW(window), TRUE);
        GtkApplication *application = gtk_window_get_application(state->ctx->parent);
        if (application) gtk_window_set_application(GTK_WINDOW(window), application);
    }
    GdkDisplay *display = gtk_widget_get_display(state->page);
    GdkWindow *parent_window = state->ctx->parent ? gtk_widget_get_window(GTK_WIDGET(state->ctx->parent)) : NULL;
    GdkMonitor *monitor = drag ? gdk_display_get_monitor_at_point(display, (gint)drag->x_root, (gint)drag->y_root) :
        parent_window ? gdk_display_get_monitor_at_window(display, parent_window) : gdk_display_get_primary_monitor(display);
    if (!monitor && gdk_display_get_n_monitors(display) > 0) monitor = gdk_display_get_monitor(display, 0);
    GdkRectangle area = { 0, 0, 1280, 800 };
    if (monitor) gdk_monitor_get_workarea(monitor, &area);
    GtkAllocation allocation;
    gtk_widget_get_allocation(state->mixer, &allocation);
    gint width = MIN(CLAMP(allocation.width, 640, 1500), MAX(1, area.width - 32));
    gint height = MIN(CLAMP(allocation.height, 480, 900), MAX(1, area.height - 64));
    gtk_window_set_default_size(GTK_WINDOW(window), width, height);
    if (drag) {
        gint x = CLAMP((gint)drag->x_root - state->mixer_drag_x, area.x, area.x + MAX(0, area.width - width));
        gint y = CLAMP((gint)drag->y_root - state->mixer_drag_y, area.y, area.y + MAX(0, area.height - height));
        gtk_window_move(GTK_WINDOW(window), x, y);
    } else {
        gtk_window_set_position(GTK_WINDOW(window), state->ctx->parent ? GTK_WIN_POS_CENTER_ON_PARENT : GTK_WIN_POS_CENTER);
    }
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_shadow_type(GTK_SCROLLED_WINDOW(scroll), GTK_SHADOW_NONE);
    gtk_widget_set_margin_start(body, 12);
    gtk_widget_set_margin_end(body, 12);
    gtk_widget_set_margin_top(body, 4);
    gtk_widget_set_margin_bottom(body, 12);
    gtk_container_add(GTK_CONTAINER(window), scroll);
    gtk_container_add(GTK_CONTAINER(scroll), body);
    mixer_move_to(state, body);
    mixer_dialogs_parent(state, GTK_WINDOW(window));
    gtk_button_set_label(GTK_BUTTON(state->mixer_window_toggle), "Programa geri yerleştir");
    gtk_widget_set_tooltip_text(state->mixer_window_toggle, "Aynı Mixer'i ana programa geri yerleştir.");
    gtk_widget_show(state->mixer_placeholder);
    g_signal_connect(window, "delete-event", G_CALLBACK(mixer_window_delete), state);
    g_signal_connect(window, "destroy", G_CALLBACK(mixer_window_destroyed), state);
    g_signal_connect(window, "focus-out-event", G_CALLBACK(talk_window_focus_lost), state);
    gtk_widget_show_all(window);
    gtk_window_present(GTK_WINDOW(window));
}

static void
mixer_window_toggle_clicked(GtkButton *button, gpointer data)
{
    (void)button; DJState *state = data;
    if (state->mixer_window) mixer_redock(state);
    else mixer_detach(state, NULL);
}

static void
mixer_window_show_clicked(GtkButton *button, gpointer data)
{
    (void)button; mixer_detach(data, NULL);
}

static void
mixer_window_restore_clicked(GtkButton *button, gpointer data)
{
    (void)button; mixer_redock(data);
}

static void
mixer_handle_realized(GtkWidget *widget, gpointer data)
{
    (void)data;
    GdkCursor *cursor = gdk_cursor_new_from_name(gtk_widget_get_display(widget), "grab");
    if (cursor) {
        gdk_window_set_cursor(gtk_widget_get_window(widget), cursor);
        g_object_unref(cursor);
    }
}

static gboolean
mixer_handle_pressed(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)widget; DJState *state = data;
    if (event->button != 1 || state->disposed || state->closing) return FALSE;
    if (state->mixer_window) {
        gtk_window_begin_move_drag(GTK_WINDOW(state->mixer_window), 1, (gint)event->x_root, (gint)event->y_root, event->time);
    } else {
        state->mixer_drag_pending = TRUE;
        state->mixer_drag_x = (gint)event->x;
        state->mixer_drag_y = (gint)event->y;
    }
    return TRUE;
}

static gboolean
mixer_handle_motion(GtkWidget *widget, GdkEventMotion *event, gpointer data)
{
    DJState *state = data;
    if (!state->mixer_drag_pending || !(event->state & GDK_BUTTON1_MASK)) return FALSE;
    if (!gtk_drag_check_threshold(widget, state->mixer_drag_x, state->mixer_drag_y, (gint)event->x, (gint)event->y)) return FALSE;
    mixer_detach(state, event);
    state->mixer_drag_pending = FALSE;
    if (state->mixer_window)
        gtk_window_begin_move_drag(GTK_WINDOW(state->mixer_window), 1, (gint)event->x_root, (gint)event->y_root, event->time);
    return TRUE;
}

static gboolean
mixer_handle_released(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    (void)widget; DJState *state = data;
    if (event->button != 1) return FALSE;
    state->mixer_drag_pending = FALSE;
    return TRUE;
}

static void
draft_switch_changed(GtkToggleButton *button, gpointer data)
{
    DJState *state = data;
    if (state->disposed)
        return;
    GtkStyleContext *style = gtk_widget_get_style_context(GTK_WIDGET(button));
    if (gtk_toggle_button_get_active(button))
        gtk_style_context_add_class(style, "is-active");
    else
        gtk_style_context_remove_class(style, "is-active");
    if (g_object_get_data(G_OBJECT(button), "fx-power") != NULL)
        gtk_button_set_label(GTK_BUTTON(button), gtk_toggle_button_get_active(button) ? "ON" : "OFF");
    if (!state->ctx->audio && state->ctx->notify != NULL)
        state->ctx->notify(state->ctx->notify_data,
            "Mixer seçimi arayüz taslağında güncellendi; ses motoru ve DSP henüz bağlı değil.");
}

static GtkWidget *
draft_switch(DJState *state, const gchar *title, const gchar *style, gboolean power)
{
    GtkWidget *button = gtk_toggle_button_new_with_label(title);
    gtk_style_context_add_class(gtk_widget_get_style_context(button), "ld-btn");
    gtk_style_context_add_class(gtk_widget_get_style_context(button), style);
    gtk_widget_set_tooltip_text(button, state->ctx->audio ? "Gerçek ses motoru kontrolü." : "Arayüz taslağı seçimi; ses motoru / DSP henüz bağlı değil.");
    if (power)
        g_object_set_data(G_OBJECT(button), "fx-power", GINT_TO_POINTER(1));
    g_signal_connect(button, "toggled", G_CALLBACK(draft_switch_changed), state);
    return button;
}

static GtkWidget *
compact_knob(const gchar *name, double min, double max, double step, double value, gint size)
{
    GtkWidget *knob = radios_knob_new(name, min, max, step, value);
    GtkWidget *dial = g_object_get_data(G_OBJECT(knob), "dial");
    gtk_widget_set_size_request(dial, size, size);
    gtk_widget_set_halign(knob, GTK_ALIGN_CENTER);
    return knob;
}

static gdouble knob_value(GtkWidget *widget)
{ return gtk_adjustment_get_value(g_object_get_data(G_OBJECT(widget), "adjustment")); }
static void knob_set(GtkWidget *widget, gdouble value)
{ gtk_adjustment_set_value(g_object_get_data(G_OBJECT(widget), "adjustment"), value); }
static gint combo_value(GtkWidget *widget, gint fallback)
{
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(widget)); if (!id) return fallback;
    gchar *end = NULL; gint64 number = g_ascii_strtoll(id, &end, 10);
    return end && !*end && number >= G_MININT && number <= G_MAXINT ? (gint)number : fallback;
}
static void combo_set(GtkWidget *widget, gint value)
{ gchar id[20]; g_snprintf(id, sizeof id, "%d", value); gtk_combo_box_set_active_id(GTK_COMBO_BOX(widget), id); }
static void widget_name(GtkWidget *widget, const gchar *prefix, guint number, const gchar *suffix)
{ gchar *name = g_strdup_printf("%s-%u-%s", prefix, number, suffix); gtk_widget_set_name(widget, name); g_free(name); }
static void knob_connect(GtkWidget *knob, GCallback callback, GtkWidget *owner)
{
    GtkAdjustment *adjustment = g_object_get_data(G_OBJECT(knob), "adjustment"); g_object_set_data(G_OBJECT(adjustment), "dj-knob-widget", knob);
    g_signal_connect_object(adjustment, "value-changed", callback, owner, 0);
    GtkWidget *dial = g_object_get_data(G_OBJECT(knob), "dial"); gtk_widget_set_tooltip_text(dial, "Mixer / DSP değişiklikleri otomatik kaydedilir.");
}
static GtkWidget *control_widget(gpointer sender)
{ return GTK_IS_WIDGET(sender) ? sender : g_object_get_data(G_OBJECT(sender), "dj-knob-widget"); }
static void audio_error(DJState *state, GError *error)
{ if (error) { if (state->ctx->notify) state->ctx->notify(state->ctx->notify_data, error->message); g_error_free(error); } }
static void mixer_reload_clicked(GtkButton *button, gpointer data)
{
    (void)button; DJState *state = data;
    if (!state->disposed) {
        radios_audio_ui_mixer_reload(state->ctx);
        radios_data_document_reload(state->channel_document);
    }
}
static void crossfader_changed(GtkRange *range, gpointer data)
{
    DJState *state = data; if (state->disposed || state->syncing) return;
    RadioSAudioMixerConfig config; if (!radios_audio_ui_get_mixer(state->ctx, &config, NULL, NULL)) return;
    config.crossfader = gtk_range_get_value(range); GError *error = NULL; radios_audio_ui_set_mixer(state->ctx, &config, &error); audio_error(state, error);
}


static void fx_segment(cairo_t *cr, gdouble x, gdouble y, gdouble width, gdouble height)
{
    gdouble bevel = MIN(width, height) * .45;
    cairo_move_to(cr, x + bevel, y);
    cairo_line_to(cr, x + width - bevel, y);
    cairo_line_to(cr, x + width, y + bevel);
    cairo_line_to(cr, x + width, y + height - bevel);
    cairo_line_to(cr, x + width - bevel, y + height);
    cairo_line_to(cr, x + bevel, y + height);
    cairo_line_to(cr, x, y + height - bevel);
    cairo_line_to(cr, x, y + bevel);
    cairo_close_path(cr);
    cairo_fill(cr);
}

static gboolean fx_display_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    FxUI *ui = data;
    static const guint digits[] = {0x3f, 0x06, 0x5b, 0x4f, 0x66, 0x6d, 0x7d, 0x07, 0x7f, 0x6f};
    static const gdouble segments[][4] = {
        {3, 0, 16, 3}, {19, 3, 3, 13}, {19, 19, 3, 13},
        {3, 32, 16, 3}, {0, 19, 3, 13}, {0, 3, 3, 13}, {3, 16, 16, 3}
    };
    gchar text[16];
    if (ui->display_bpm_value >= 30)
        g_ascii_formatd(text, sizeof text, "%5.1f", ui->display_bpm_value);
    else g_strlcpy(text, "---.-", sizeof text);
    GdkRGBA color;
    GtkStyleContext *style = gtk_widget_get_style_context(widget);
    gtk_style_context_get_color(style, gtk_widget_get_state_flags(widget), &color);
    gdouble width = gtk_widget_get_allocated_width(widget), height = gtk_widget_get_allocated_height(widget);
    gtk_render_background(style, cr, 0, 0, width, height);
    gdouble scale = MIN(width / 111, height / 35);
    cairo_save(cr);
    cairo_translate(cr, (width - 111 * scale) / 2, (height - 35 * scale) / 2);
    cairo_scale(cr, scale, scale);
    gdouble x = 0;
    for (const gchar *character = text; *character; ++character) {
        if (*character == '.') {
            cairo_set_source_rgba(cr, color.red, color.green, color.blue, color.alpha);
            cairo_arc(cr, x + 2, 33, 1.7, 0, 2 * G_PI); cairo_fill(cr);
            x += 7;
            continue;
        }
        guint mask = g_ascii_isdigit(*character) ? digits[*character - '0'] : *character == '-' ? 0x40 : 0;
        for (guint segment = 0; segment < G_N_ELEMENTS(segments); ++segment) {
            cairo_set_source_rgba(cr, color.red, color.green, color.blue,
                color.alpha * ((mask & (1u << segment)) ? 1 : .07));
            fx_segment(cr, x + segments[segment][0], segments[segment][1],
                segments[segment][2], segments[segment][3]);
        }
        x += 26;
    }
    cairo_restore(cr);
    return FALSE;
}

static void fx_display_update(FxUI *ui, const RadioSAudioFx *fx)
{
    gchar bpm[40], depth[24], beat[24];
    gdouble tempo = fx ? fx->tempo_bpm : -1;
    if (fx) {
        gchar number[24]; g_ascii_formatd(number, sizeof number, "%.1f", tempo);
        g_snprintf(bpm, sizeof bpm, "%s BPM", number);
        g_snprintf(depth, sizeof depth, "%u%%", (guint)llround(fx->depth * 100));
        g_snprintf(beat, sizeof beat, "%u ms", (guint)llround(60000 / tempo));
    } else {
        g_strlcpy(bpm, "BPM hazır değil", sizeof bpm);
        g_strlcpy(depth, "--%", sizeof depth);
        g_strlcpy(beat, "-- ms", sizeof beat);
    }
    gchar *effect = fx ? g_ascii_strup(radios_audio_fx_name(fx->type), -1) : g_strdup("EFEKT");
    gtk_label_set_text(GTK_LABEL(ui->display_effect), effect); g_free(effect);
    gtk_label_set_text(GTK_LABEL(ui->display_state), fx ? fx->enabled ? "ON" : "OFF" : "--");
    gtk_label_set_text(GTK_LABEL(ui->display_cue), fx && fx->cue ? "CUE" : "CUE -");
    gtk_label_set_text(GTK_LABEL(ui->display_depth), depth);
    gtk_label_set_text(GTK_LABEL(ui->display_beat), beat);
    gtk_level_bar_set_value(GTK_LEVEL_BAR(ui->display_depth_bar), fx ? fx->depth * 100 : 0);
    GtkStyleContext *power = gtk_widget_get_style_context(ui->display_state);
    GtkStyleContext *cue = gtk_widget_get_style_context(ui->display_cue);
    if (fx && fx->enabled) gtk_style_context_add_class(power, "is-on");
    else gtk_style_context_remove_class(power, "is-on");
    if (fx && fx->cue) gtk_style_context_add_class(cue, "is-active");
    else gtk_style_context_remove_class(cue, "is-active");
    if (ui->display_bpm_value != tempo) {
        ui->display_bpm_value = tempo;
        atk_object_set_name(gtk_widget_get_accessible(ui->display_bpm), bpm);
        gtk_widget_queue_draw(ui->display_bpm);
    }
}

static GtkWidget *fx_display_new(FxUI *ui, guint number)
{
    GtkWidget *screen = radios_ui_box(GTK_ORIENTATION_VERTICAL, 5, "fx-lcd");
    GtkWidget *heading = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "fx-lcd-heading");
    GtkWidget *tempo = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 5, "fx-lcd-tempo");
    GtkWidget *parameters = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "fx-lcd-parameters");
    widget_name(screen, "dj-fx", number, "display");
    ui->display_effect = radios_ui_label("EFEKT", "fx-lcd-effect");
    ui->display_state = radios_ui_label("--", "fx-lcd-state");
    ui->display_cue = radios_ui_label("CUE -", "fx-lcd-cue");
    gtk_label_set_ellipsize(GTK_LABEL(ui->display_effect), PANGO_ELLIPSIZE_END);
    pack(heading, ui->display_effect, TRUE);
    pack(heading, ui->display_cue, FALSE);
    pack(heading, ui->display_state, FALSE);
    ui->display_bpm = gtk_drawing_area_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(ui->display_bpm), "fx-lcd-digits");
    gtk_widget_set_size_request(ui->display_bpm, 111, 38);
    ui->display_bpm_value = -2;
    atk_object_set_role(gtk_widget_get_accessible(ui->display_bpm), ATK_ROLE_STATIC);
    atk_object_set_description(gtk_widget_get_accessible(ui->display_bpm), "Efektin mikserde ayarlanan temposu; dakikadaki vuruş sayısı.");
    g_signal_connect(ui->display_bpm, "draw", G_CALLBACK(fx_display_draw), ui);
    pack(tempo, ui->display_bpm, TRUE);
    GtkWidget *bpm_caption = radios_ui_label("BPM", "fx-lcd-caption");
    gtk_widget_set_valign(bpm_caption, GTK_ALIGN_END);
    pack(tempo, bpm_caption, FALSE);
    ui->display_depth = radios_ui_label("--%", "fx-lcd-value");
    ui->display_beat = radios_ui_label("-- ms", "fx-lcd-value");
    GtkWidget *depth = radios_ui_box(GTK_ORIENTATION_VERTICAL, 2, "fx-lcd-parameter");
    GtkWidget *beat = radios_ui_box(GTK_ORIENTATION_VERTICAL, 2, "fx-lcd-parameter");
    pack(depth, radios_ui_label("DEPTH", "fx-lcd-caption"), FALSE);
    pack(depth, ui->display_depth, FALSE);
    pack(beat, radios_ui_label("1 BEAT", "fx-lcd-caption"), FALSE);
    pack(beat, ui->display_beat, FALSE);
    gtk_widget_set_tooltip_text(beat, "Ayarlanan BPM'de bir vuruşun süresi; efektin delay süresi değildir.");
    pack(parameters, depth, TRUE); pack(parameters, beat, TRUE);
    ui->display_depth_bar = gtk_level_bar_new_for_interval(0, 100);
    gtk_style_context_add_class(gtk_widget_get_style_context(ui->display_depth_bar), "fx-lcd-depthbar");
    pack(screen, heading, FALSE); pack(screen, tempo, FALSE);
    pack(screen, parameters, FALSE); pack(screen, ui->display_depth_bar, FALSE);
    const gchar *ids[] = {"display-effect", "display-state", "display-cue", "display-bpm", "display-depth", "display-beat"};
    GtkWidget *widgets[] = {ui->display_effect, ui->display_state, ui->display_cue, ui->display_bpm, ui->display_depth, ui->display_beat};
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) widget_name(widgets[i], "dj-fx", number, ids[i]);
    fx_display_update(ui, NULL);
    return screen;
}

static GtkWidget *
effects_new(DJState *state)
{
    GtkWidget *section = radios_ui_box(GTK_ORIENTATION_VERTICAL, 5, "dj-effects-section");
    GtkWidget *heading = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 10, "dj-effects-heading");
    GtkWidget *rack = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 12, "dj-effects-pedals");
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_name(rack, "dj-effects-pedals");
    gtk_box_set_homogeneous(GTK_BOX(rack), TRUE);
    pack(heading, radios_ui_label("Effectorler", "dj-section-title heading"), FALSE);
    pack(heading, radios_ui_label("4 effector", "dj-muted"), FALSE);
    pack(section, heading, FALSE);
    for (guint row_index = 0; row_index < DJ_FX_PEDALS; ++row_index) {
        GtkWidget *row = radios_ui_box(GTK_ORIENTATION_VERTICAL, 7, "fx-pedal");
        GtkWidget *depth_row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "fx-pedal-depth");
        GtkWidget *switches = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "fx-pedal-switches");
        gchar *number = g_strdup_printf("FX %u", row_index + 1);
        widget_name(row, "dj-fx", row_index + 1, "pedal");
        gtk_widget_set_size_request(row, 180, -1);
        pack(row, radios_ui_label(number, "fx-pedal-title"), FALSE);
        GtkWidget *cue = draft_switch(state, "CUE", "fx-monitor-button", FALSE);
        GtkWidget *power = draft_switch(state, "OFF", "fx-power-button", TRUE);
        state->fx_incoming[row_index] = radios_ui_label("Gönderim yok", "fx-table-incoming");
        gchar *incoming_name = g_strdup_printf("dj-fx-incoming-%u", row_index + 1);
        gtk_widget_set_name(state->fx_incoming[row_index], incoming_name);
        g_free(incoming_name);
        gtk_label_set_ellipsize(GTK_LABEL(state->fx_incoming[row_index]), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(state->fx_incoming[row_index]), 24);
        FxUI *fx = &state->fx[row_index];
        pack(row, fx_display_new(fx, row_index + 1), FALSE);
        fx->type = radios_ui_combo(effects, 0);
        fx->depth = compact_knob("DEPTH", 0, 100, 1, 50, 42);
        fx->cue = cue; fx->power = power;
        const gchar *ids[] = {"type", "depth", "cue", "power"};
        GtkWidget *widgets[] = {fx->type, fx->depth, fx->cue, fx->power};
        for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) widget_name(widgets[i], "dj-fx", row_index + 1, ids[i]);
        pack(row, radios_ui_label("Efekt türü", "dj-small-label"), FALSE);
        pack(row, fx->type, FALSE);
        if (state->ctx->audio) {
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(fx->type));
            for (guint i = 0; i < 14; ++i) { gchar id[16]; g_snprintf(id, sizeof id, "%u", i); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(fx->type), id, effects[i]); }
            combo_set(fx->type, 0);
            GtkWidget *tempo = gtk_spin_button_new_with_range(30, 300, 1); gtk_spin_button_set_value(GTK_SPIN_BUTTON(tempo), 120); fx->tempo = tempo;
            widget_name(tempo, "dj-fx", row_index + 1, "tempo");
            gtk_entry_set_width_chars(GTK_ENTRY(tempo), 3); gtk_entry_set_max_width_chars(GTK_ENTRY(tempo), 3);
            gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(tempo), TRUE);
            gtk_widget_set_tooltip_text(tempo, "Efekt temposu: dakikadaki vuruş sayısı (BPM), 30–300.");
            GtkWidget *tempo_box = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, "fx-pedal-tempo");
            pack(tempo_box, radios_ui_label("BPM", "dj-small-label"), TRUE);
            pack(tempo_box, tempo, FALSE); pack(depth_row, tempo_box, TRUE);
            g_object_set_data(G_OBJECT(row), "dj-fx-index", GUINT_TO_POINTER(row_index + 1));
            g_object_set_data(G_OBJECT(row), "dj-state", state);
            g_signal_connect_object(fx->type, "changed", G_CALLBACK(fx_control_changed), row, 0);
            g_signal_connect_object(fx->power, "toggled", G_CALLBACK(fx_control_changed), row, 0);
            g_signal_connect_object(fx->cue, "toggled", G_CALLBACK(fx_control_changed), row, 0);
            g_signal_connect_object(tempo, "value-changed", G_CALLBACK(fx_control_changed), row, 0);
            knob_connect(fx->depth, G_CALLBACK(fx_control_changed), row);
        }
        pack(depth_row, fx->depth, TRUE); pack(row, depth_row, FALSE);
        pack(row, radios_ui_label("Gönderen kanallar", "dj-small-label"), FALSE);
        pack(row, state->fx_incoming[row_index], FALSE);
        pack(switches, cue, TRUE); pack(switches, power, TRUE);
        pack(row, switches, FALSE); pack(rack, row, TRUE);
        g_free(number);
    }
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_container_add(GTK_CONTAINER(scroll), rack);
    pack(section, scroll, FALSE);
    return section;
}

static void talk_pressed(GtkButton *button, gpointer data)
{
    (void)button; DJState *state = data;
    if (state->disposed || state->talking || !state->push_to_talk || !state->ctx->audio ||
        radios_audio_ui_microphone_active(state->ctx)) return;
    if (!radios_audio_ui_ready(state->ctx, RADIOS_AUDIO_UI_MAIN) ||
        !radios_audio_ui_ready(state->ctx, RADIOS_AUDIO_UI_MICROPHONE)) return;
    /* Momentary push-to-talk (R7.3): the engine's on-air gate is latch || ptt,
     * so releasing never closes a microphone the ON AIR latch opened and never
     * touches any deck's transport. */
    GError *error = NULL;
    if (!radios_audio_ui_microphone_ptt(state->ctx, TRUE, &error)) { audio_error(state, error); return; }
    state->talking = TRUE;
    gtk_style_context_add_class(gtk_widget_get_style_context(state->push_to_talk), "is-active");
}
static void talk_release(DJState *state)
{
    if (!state->talking) return;
    state->talking = FALSE;
    radios_audio_ui_microphone_ptt(state->ctx, FALSE, NULL);
    if (state->push_to_talk)
        gtk_style_context_remove_class(gtk_widget_get_style_context(state->push_to_talk), "is-active");
}
static void talk_released(GtkButton *button, gpointer data)
{ (void)button; talk_release(data); }
static gboolean talk_key_pressed(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    if (event->keyval != GDK_KEY_space && event->keyval != GDK_KEY_Return && event->keyval != GDK_KEY_KP_Enter) return FALSE;
    talk_pressed(GTK_BUTTON(widget), data); return TRUE;
}
static gboolean talk_key_released(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    (void)widget;
    if (event->keyval != GDK_KEY_space && event->keyval != GDK_KEY_Return && event->keyval != GDK_KEY_KP_Enter) return FALSE;
    talk_release(data); return TRUE;
}
static gboolean talk_focus_lost(GtkWidget *widget, GdkEventFocus *event, gpointer data)
{ (void)widget; (void)event; talk_release(data); return FALSE; }
static gboolean talk_grab_lost(GtkWidget *widget, GdkEventGrabBroken *event, gpointer data)
{ (void)widget; (void)event; talk_release(data); return FALSE; }
static void talk_grab_changed(GtkWidget *widget, gboolean was_grabbed, gpointer data)
{ (void)widget; if (!was_grabbed) talk_release(data); }
static gboolean talk_pointer_left(GtkWidget *widget, GdkEventCrossing *event, gpointer data)
{ (void)widget; if (event->detail != GDK_NOTIFY_INFERIOR) talk_release(data); return FALSE; }
static void talk_unmapped(GtkWidget *widget, gpointer data)
{ (void)widget; talk_release(data); }
static void talk_destroyed(GtkWidget *widget, gpointer data)
{ (void)widget; DJState *state = data; talk_release(state); state->push_to_talk = NULL; }
static gboolean talk_window_focus_lost(GtkWidget *widget, GdkEventFocus *event, gpointer data)
{ (void)widget; (void)event; talk_release(data); return FALSE; }

static gboolean pedal_theme_notice(gpointer data)
{
    DJState *state = data; state->theme_notice = 0;
    if (!state->disposed && state->theme_error && state->ctx->notify)
        state->ctx->notify(state->ctx->notify_data, state->theme_error);
    g_clear_pointer(&state->theme_error, g_free); return G_SOURCE_REMOVE;
}

static void pedal_theme_load(DJState *state)
{
    if (!state->ctx->config || !state->ctx->config->project_root) return;
    gchar *path = g_build_filename(state->ctx->config->project_root, "assets", "themes", "dj-pedals.css", NULL);
    GtkCssProvider *provider = gtk_css_provider_new(); GError *error = NULL;
    if (!gtk_css_provider_load_from_path(provider, path, &error)) {
        /* The window footer is created after its pages. Defer its notice until
         * construction finishes, and cancel it if this page is destroyed. */
        state->theme_error = g_strdup_printf("DJ pedal tasarımı yüklenemedi: %s", error ? error->message : "bilinmeyen hata");
        g_clear_error(&error); g_object_unref(provider); g_free(path); return;
    }
    g_free(path);
    GdkScreen *screen = gtk_widget_get_screen(state->page);
    if (!screen) { g_object_unref(provider); return; }
    state->pedal_theme = provider; state->theme_screen = g_object_ref(screen);
    gtk_style_context_add_provider_for_screen(screen, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
}

static gboolean
rehearsal_tick(gpointer data)
{
    DJState *state = data;
    gint64 elapsed = state->rehearsal_elapsed;
    if (state->rehearsal_running)
        elapsed += g_get_monotonic_time() - state->rehearsal_started;
    guint64 seconds = (guint64)(elapsed / G_TIME_SPAN_SECOND);
    gchar *text = g_strdup_printf("%02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT, seconds / 60, seconds % 60);
    gtk_label_set_text(GTK_LABEL(state->rehearsal_time), text);
    g_free(text);
    return G_SOURCE_CONTINUE;
}

static void
rehearsal_toggle(GtkButton *button, gpointer data)
{
    DJState *state = data;
    if (state->rehearsal_running)
        state->rehearsal_elapsed += g_get_monotonic_time() - state->rehearsal_started;
    else {
        state->rehearsal_started = g_get_monotonic_time();
        if (state->ctx->notify != NULL)
            state->ctx->notify(state->ctx->notify_data, state->ctx->audio ? "Prova sayacı başladı." : "Prova sayacı başladı; mikrofon ve ses çıkışı henüz bağlı değil.");
    }
    state->rehearsal_running = !state->rehearsal_running;
    gtk_button_set_label(button, state->rehearsal_running ? "Provayı durdur" : "Prova başlat");
    rehearsal_tick(state);
}

static void
rehearsal_reset(GtkButton *button, gpointer data)
{
    DJState *state = data;
    (void)button;
    state->rehearsal_elapsed = 0;
    state->rehearsal_started = g_get_monotonic_time();
    rehearsal_tick(state);
}

static GtkWidget *
speech_new(DJState *state)
{
    static const gchar *const sources[] = { "Mikrofon", "Dış bağlantı", "Müzik yatağı", NULL };
    GtkWidget *speech = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, "dj-speech-bar");
    GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 12, "dj-speech-main-row");
    GtkWidget *microphone = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, "dj-speech-controls");
    GtkWidget *destination = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, "dj-speech-destination");
    GtkWidget *source = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, "dj-speech-source");
    GtkWidget *bed = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, "dj-speech-bed");
    GtkWidget *level = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -36, 0, 1);
    GtkWidget *ducking = gtk_check_button_new_with_label("Konuşmada müziği kıs");
    GtkWidget *rehearsal = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 5, "dj-speech-rehearsal");
    GtkWidget *clock = radios_ui_box(GTK_ORIENTATION_VERTICAL, 2, "dj-speech-rehearsal-clock");
    GtkWidget *reset = radios_ui_button("↺");
    pack(row, radios_ui_label("Konuşma · Canlı kaynak", "dj-speech-heading"), FALSE);
    state->on_air = draft_switch(state, "ON AIR", "dj-on-air-button", FALSE);
    gtk_widget_set_name(state->on_air, "dj-speech-on-air");
    gtk_widget_set_sensitive(state->on_air, FALSE);
    gtk_widget_set_tooltip_text(state->on_air, "Canlı mikrofonu seçilen MASTER veya kanala aç / kapat.");
    if (state->ctx->audio) g_signal_connect(state->on_air, "toggled", G_CALLBACK(microphone_changed), state);
    pack(row, state->on_air, FALSE);
    state->push_to_talk = radios_ui_button("Bas konuş");
    gtk_widget_set_name(state->push_to_talk, "dj-speech-push-to-talk");
    gtk_style_context_add_class(gtk_widget_get_style_context(state->push_to_talk), "dj-push-to-talk");
    gtk_widget_set_sensitive(state->push_to_talk, FALSE);
    gtk_widget_set_tooltip_text(state->push_to_talk, "Basılı tutarken konuş; bırakınca mikrofon kapanır. Boşluk veya Enter ile de kullanılır. ON AIR açıkken önce kapatın.");
    g_signal_connect(state->push_to_talk, "pressed", G_CALLBACK(talk_pressed), state);
    g_signal_connect(state->push_to_talk, "released", G_CALLBACK(talk_released), state);
    g_signal_connect(state->push_to_talk, "key-press-event", G_CALLBACK(talk_key_pressed), state);
    g_signal_connect(state->push_to_talk, "key-release-event", G_CALLBACK(talk_key_released), state);
    g_signal_connect(state->push_to_talk, "focus-out-event", G_CALLBACK(talk_focus_lost), state);
    g_signal_connect(state->push_to_talk, "grab-broken-event", G_CALLBACK(talk_grab_lost), state);
    g_signal_connect(state->push_to_talk, "grab-notify", G_CALLBACK(talk_grab_changed), state);
    g_signal_connect(state->push_to_talk, "leave-notify-event", G_CALLBACK(talk_pointer_left), state);
    g_signal_connect(state->push_to_talk, "unmap", G_CALLBACK(talk_unmapped), state);
    g_signal_connect(state->push_to_talk, "destroy", G_CALLBACK(talk_destroyed), state);
    if (state->ctx->parent)
        g_signal_connect(state->ctx->parent, "focus-out-event", G_CALLBACK(talk_window_focus_lost), state);
    pack(microphone, state->push_to_talk, FALSE);
    pack(microphone, radios_ui_label("Efektöre gönder", "dj-small-label"), FALSE);
    state->speech_effector = gtk_combo_box_text_new();
    gtk_widget_set_name(state->speech_effector, "dj-speech-effector");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->speech_effector), "-1", "Yok");
    for (guint slot = 0; slot < DJ_FX_PEDALS; ++slot) {
        gchar id[16], name[40];
        g_snprintf(id, sizeof id, "%u", slot);
        g_snprintf(name, sizeof name, "Effector %u", slot + 1);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->speech_effector), id, name);
    }
    combo_set(state->speech_effector, -1);
    gtk_widget_set_tooltip_text(state->speech_effector,
        "Açık mikrofonu seçilen efektöre gönderir. Efekt sesini almak için kanal veya MASTER RETURN seçiminden aynı efektörü seçin. Seçim otomatik kaydedilir.");
    gtk_widget_set_sensitive(state->speech_effector, state->ctx->audio != NULL);
    if (state->ctx->audio)
        g_signal_connect(state->speech_effector, "changed", G_CALLBACK(speech_changed), state);
    pack(microphone, state->speech_effector, FALSE);
    pack(row, microphone, FALSE);
    state->speech_to_master = gtk_check_button_new_with_label("Master’a gönder");
    gtk_widget_set_name(state->speech_to_master, "dj-speech-to-master");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->speech_to_master), TRUE);
    gtk_widget_set_sensitive(state->speech_to_master, state->ctx->audio != NULL);
    gtk_widget_set_tooltip_text(state->speech_to_master,
        "İşaretliyken mevcut MASTER mikrofon yolu kullanılır. İşareti kaldırınca mikrofonu göndereceğiniz kanalı seçin.");
    pack(destination, state->speech_to_master, FALSE);
    pack(destination, radios_ui_label("Kanal seç", "dj-small-label"), FALSE);
    state->speech_channel = gtk_combo_box_text_new();
    gtk_widget_set_name(state->speech_channel, "dj-speech-channel");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->speech_channel), "-1", "Kanal seç");
    combo_set(state->speech_channel, -1);
    gtk_widget_set_sensitive(state->speech_channel, FALSE);
    gtk_widget_set_tooltip_text(state->speech_channel,
        "Mikrofonu seçilen kanalın EQ, ses seviyesi, mute ve solo kontrollerinden geçirir. Kanal seçilene kadar doğrudan mikrofon gönderimi kapalıdır. Seçim otomatik kaydedilir.");
    if (state->ctx->audio) {
        g_signal_connect(state->speech_to_master, "toggled", G_CALLBACK(speech_changed), state);
        g_signal_connect(state->speech_channel, "changed", G_CALLBACK(speech_changed), state);
    }
    pack(destination, state->speech_channel, FALSE);
    gtk_widget_set_valign(destination, GTK_ALIGN_END);
    pack(row, destination, FALSE);
    pack(source, radios_ui_label("Tercih edilen kaynak", "dj-small-label"), FALSE);
    if (state->ctx->audio) {
        state->microphone_gate = gtk_toggle_button_new_with_label("Mikrofon kapalı"); gtk_widget_set_name(state->microphone_gate, "dj-microphone-gate");
        pack(source, state->microphone_gate, FALSE); g_signal_connect(state->microphone_gate, "toggled", G_CALLBACK(microphone_changed), state);
    } else pack(source, radios_ui_combo(sources, 0), FALSE);
    pack(row, source, TRUE);
    gtk_range_set_value(GTK_RANGE(level), -18);
    gtk_scale_set_digits(GTK_SCALE(level), 0);
    gtk_scale_set_value_pos(GTK_SCALE(level), GTK_POS_RIGHT);
    gtk_widget_set_size_request(level, 220, -1);
    state->speech_level = level; gtk_widget_set_name(level, "dj-speech-bed");
    state->speech_ducking = ducking; gtk_widget_set_name(ducking, "dj-speech-ducking");
    pack(bed, radios_ui_label("Müzik yatağı", "dj-small-label"), FALSE);
    pack(bed, level, FALSE);
    pack(row, bed, TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ducking), TRUE);
    if (state->ctx->audio) {
        g_signal_connect(level, "value-changed", G_CALLBACK(speech_changed), state);
        g_signal_connect(ducking, "toggled", G_CALLBACK(speech_changed), state);
    }
    pack(row, ducking, FALSE);
    pack(clock, radios_ui_label("Prova süresi", "dj-small-label"), FALSE);
    state->rehearsal_time = radios_ui_label("00:00", "dj-rehearsal-digits");
    state->rehearsal_toggle = radios_ui_button("Prova başlat");
    pack(clock, state->rehearsal_time, FALSE);
    pack(rehearsal, clock, FALSE);
    pack(rehearsal, state->rehearsal_toggle, FALSE);
    pack(rehearsal, reset, FALSE);
    g_signal_connect(state->rehearsal_toggle, "clicked", G_CALLBACK(rehearsal_toggle), state);
    g_signal_connect(reset, "clicked", G_CALLBACK(rehearsal_reset), state);
    pack(row, rehearsal, FALSE);
    pack(speech, row, FALSE);
    state->speech_status = radios_ui_label(state->ctx->audio ? "Mikrofon kapalı · Yerel ses çıkışı" :
        "Aktif mikrofon / kaynak  —    Yayın çıkışı  —    Konuşma süresi  —:—", "dj-speech-status");
    gtk_widget_set_name(state->speech_status, "dj-speech-status"); pack(speech, state->speech_status, FALSE);
    state->timer = g_timeout_add_seconds(1, rehearsal_tick, state);
    return speech;
}

static void channel_routes_update(DJState *state)
{
    gboolean syncing = state->syncing; state->syncing = TRUE;
    GList *channels = gtk_container_get_children(GTK_CONTAINER(state->channels));
    GList *strips = g_list_copy(channels);
    if (state->master) strips = g_list_append(strips, state->master);
    for (GList *item = strips; item; item = item->next) {
        ChannelUI *ui = g_object_get_data(G_OBJECT(item->data), "dj-channel-state");
        GtkWidget *choices[] = {ui->send_target};
        for (guint choice = 0; choice < G_N_ELEMENTS(choices); ++choice) {
            gint selected = combo_value(choices[choice], -1);
            gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(choices[choice]));
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(choices[choice]), "-1", "Yok");
            for (GList *target = channels; target; target = target->next) {
                ChannelUI *other = g_object_get_data(G_OBJECT(target->data), "dj-channel-state");
                if (other == ui) continue;
                const gchar *label = g_object_get_data(G_OBJECT(other->widget), "dj-channel-name");
                gchar *id = g_strdup_printf("%u", other->index), *name = g_strdup_printf("%02u · %s", other->index + 1, label);
                gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(choices[choice]), id, name);
                g_free(id); g_free(name);
            }
            combo_set(choices[choice], selected);
            if (gtk_combo_box_get_active(GTK_COMBO_BOX(choices[choice])) < 0) combo_set(choices[choice], -1);
        }
        gint selected = combo_value(ui->return_source, -1);
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(ui->return_source));
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->return_source), "-1", "Yok");
        for (guint slot = 0; slot < DJ_FX_PEDALS; ++slot) {
            gchar id[16], name[40];
            g_snprintf(id, sizeof id, "%u", slot);
            g_snprintf(name, sizeof name, "Effector %u", slot + 1);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->return_source), id, name);
        }
        combo_set(ui->return_source, selected);
        if (gtk_combo_box_get_active(GTK_COMBO_BOX(ui->return_source)) < 0) combo_set(ui->return_source, -1);
    }
    if (state->speech_channel) {
        gint selected = combo_value(state->speech_channel, -1);
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(state->speech_channel));
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->speech_channel), "-1", "Kanal seç");
        for (GList *item = channels; item; item = item->next) {
            ChannelUI *ui = g_object_get_data(G_OBJECT(item->data), "dj-channel-state");
            const gchar *label = g_object_get_data(G_OBJECT(ui->widget), "dj-channel-name");
            gchar *id = g_strdup_printf("%u", ui->index), *name = g_strdup_printf("%02u · %s", ui->index + 1, label);
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->speech_channel), id, name);
            g_free(id); g_free(name);
        }
        combo_set(state->speech_channel, selected);
        if (gtk_combo_box_get_active(GTK_COMBO_BOX(state->speech_channel)) < 0) combo_set(state->speech_channel, -1);
    }
    g_list_free(strips); g_list_free(channels); state->syncing = syncing;
}

static void
update_count(DJState *state)
{
    gchar *text = g_strdup_printf("%u kanal + MASTER · %u sampler", state->channel_count, (state->channel_count + 1) * 4);
    gtk_label_set_text(GTK_LABEL(state->count), text);
    g_free(text);
    channel_routes_update(state);
}

static void
routes_update(DJState *state)
{
    if (state->ctx->audio) return;
    GString *names[DJ_FX_PEDALS];
    GList *children = gtk_container_get_children(GTK_CONTAINER(state->channels));
    for (guint i = 0; i < DJ_FX_PEDALS; ++i)
        names[i] = g_string_new(NULL);
    if (state->master != NULL)
        children = g_list_append(children, state->master);
    for (GList *item = children; item != NULL; item = item->next) {
        GtkWidget *effector = g_object_get_data(G_OBJECT(item->data), "dj-effector");
        GtkWidget *source = g_object_get_data(G_OBJECT(item->data), "dj-source");
        const gchar *name = g_object_get_data(G_OBJECT(item->data), "dj-channel-name");
        gint index = effector != NULL ? gtk_combo_box_get_active(GTK_COMBO_BOX(effector)) - 1 : -1;
        if (index < 0 || index >= (gint)DJ_FX_PEDALS || name == NULL ||
            (source != NULL && gtk_combo_box_get_active(GTK_COMBO_BOX(source)) <= 0))
            continue;
        if (names[index]->len != 0)
            g_string_append(names[index], ", ");
        g_string_append(names[index], name);
    }
    g_list_free(children);
    for (guint i = 0; i < DJ_FX_PEDALS; ++i) {
        gtk_label_set_text(GTK_LABEL(state->fx_incoming[i]), names[i]->len != 0 ? names[i]->str : "Gönderim yok");
        gtk_widget_set_tooltip_text(state->fx_incoming[i], names[i]->len != 0 ? names[i]->str : "Gönderim yok");
        g_string_free(names[i], TRUE);
    }
}

static void
route_changed(GtkComboBox *combo, gpointer data)
{
    (void)combo;
    routes_update(data);
}

static void
pad_render(PadState *pad)
{
    GtkStyleContext *style = gtk_widget_get_style_context(pad->button);
    gtk_label_set_text(GTK_LABEL(pad->title_label), pad->title != NULL ? pad->title : "Boş");
    gtk_label_set_text(GTK_LABEL(pad->status_label), pad->owner->ctx->audio ?
        pad->pending || pad->loading ? "Yükleniyor" : pad->selected ? "Çalıyor" : pad->media_id ? "Hazır" : pad->title ? "Medya seç" : "" :
        pad->selected ? "Seçili" : (pad->title != NULL ? "Atandı" : ""));
    if (pad->title != NULL)
        gtk_style_context_add_class(style, "is-assigned");
    else
        gtk_style_context_remove_class(style, "is-assigned");
    if (pad->selected)
        gtk_style_context_add_class(style, "is-active");
    else
        gtk_style_context_remove_class(style, "is-active");
    gchar *tooltip = g_strdup_printf("%s · Sampler %u: %s. %s",
        pad->channel_name, pad->number, pad->title != NULL ? pad->title : "Boş",
        pad->owner->ctx->audio ? pad->media_id ? "Kanalın ayrı sampler sesini başlat / durdur." : "Çalmak için doğrulanmış kütüphane medyası seç." :
        "Arayüz seçimi; ses motoru bağlı değil.");
    gtk_widget_set_tooltip_text(pad->button, tooltip);
    g_free(tooltip);
}

static void
pad_dialog_destroyed(GtkWidget *dialog, gpointer data)
{
    PadEditor *editor = data;
    if (editor->pad->dialog == dialog)
        editor->pad->dialog = NULL;
    editor->pad = NULL;
    g_signal_handlers_disconnect_by_data(editor->file, editor);
    g_signal_handlers_disconnect_by_data(editor->title, editor);
    g_signal_handlers_disconnect_by_data(editor->library, editor);
}

static void pad_editor_free(gpointer data)
{
    PadEditor *editor = data; g_free(editor->media_id); g_free(editor->version_id);
    g_free(editor->media_file_id); g_free(editor->media_sha256); g_free(editor);
}
static void pad_binding_clear(PadState *pad)
{
    g_clear_pointer(&pad->media_id, g_free); g_clear_pointer(&pad->version_id, g_free);
    g_clear_pointer(&pad->media_file_id, g_free); g_clear_pointer(&pad->media_sha256, g_free);
}
static gboolean pad_voice_owned(PadState *pad)
{
    if (!pad->owner->ctx->audio || !pad->generation || !pad->media_id) return FALSE;
    guint deck = pad->channel_number ? RADIOS_AUDIO_SAMPLER_BASE + pad->channel_number - 1 : RADIOS_AUDIO_MASTER_SAMPLER;
    RadioSAudioUiSnapshot snapshot = {0};
    return radios_audio_ui_get_snapshot(pad->owner->ctx, deck, &snapshot) && snapshot.generation == pad->generation &&
        g_str_equal(snapshot.media_id, pad->media_id) && snapshot.state != RADIOS_AUDIO_UI_EMPTY && snapshot.state != RADIOS_AUDIO_UI_ERROR;
}
static void pad_media_picked(RadioSViewContext *ctx, GtkWidget *dialog, JsonObject *detail, gpointer data)
{
    PadEditor *editor = data; if (!editor->pad || editor->pad->disposed) return;
    JsonObject *media = json_object_get_object_member(detail, "media"), *version = json_object_get_object_member(detail, "version");
    if (!media || !version || !g_str_equal(radios_json_string(media, "station_id", ""), radios_data_station_id(ctx))) return;
    if (!json_object_has_member(version, "verified") || !json_object_get_boolean_member(version, "verified") ||
        !json_object_has_member(version, "object_available") || !json_object_get_boolean_member(version, "object_available")) {
        gtk_label_set_text(GTK_LABEL(editor->error), "Medya dosyası doğrulanamıyor. Erişilebilir bir kütüphane medyası seç.");
        return;
    }
    const gchar *id = radios_json_string(media, "_id", ""), *version_id = radios_json_string(version, "_id", "");
    const gchar *file_id = radios_json_string(version, "media_file_id", ""), *sha = radios_json_string(version, "sha256", "");
    if (!g_uuid_string_is_valid(id) || !g_uuid_string_is_valid(version_id) || !g_uuid_string_is_valid(file_id) || strlen(sha) != 64) return;
    g_free(editor->media_id); editor->media_id = g_strdup(id); g_free(editor->version_id); editor->version_id = g_strdup(version_id);
    g_free(editor->media_file_id); editor->media_file_id = g_strdup(file_id); g_free(editor->media_sha256); editor->media_sha256 = g_strdup(sha);
    const gchar *title = radios_json_string(media, "title", "Medya"); gchar *label = g_utf8_substring(title, 0, MIN(g_utf8_strlen(title, -1), 160));
    gtk_entry_set_text(GTK_ENTRY(editor->title), label); g_free(label);
    gtk_button_set_label(GTK_BUTTON(editor->library), title);
    gtk_label_set_text(GTK_LABEL(editor->error), "Doğrulanmış medya seçildi. Ata ile sampler eşlemesini kaydet.");
    pad_editor_changed(dialog, editor);
}
static void pad_library_open(GtkButton *button, gpointer data)
{
    (void)button; PadEditor *editor = data;
    if (editor->pad && !editor->pad->disposed)
        radios_media_library_pick(editor->pad->owner->ctx, editor->pad->dialog, pad_media_picked, editor);
}

static void
pad_response(GtkDialog *dialog, gint response, gpointer data)
{
    PadEditor *editor = data;
    PadState *pad = editor->pad;
    if (pad == NULL || pad->disposed) {
        gtk_widget_destroy(GTK_WIDGET(dialog));
        return;
    }
    if ((response == GTK_RESPONSE_ACCEPT || response == GTK_RESPONSE_REJECT) && pad->owner->ctx->data &&
        (!radios_data_connected(pad->owner->ctx) || !radios_data_document_ready(pad->owner->sampler))) {
        gtk_label_set_text(GTK_LABEL(editor->error), "Sampler kaydı ve veri bağlantısı hazır değil; girdileriniz korunuyor.");
        return;
    }
    if (response == GTK_RESPONSE_ACCEPT) {
        gchar *title = g_strdup(gtk_entry_get_text(GTK_ENTRY(editor->title)));
        g_strstrip(title);
        if (*title == '\0') {
            gtk_label_set_text(GTK_LABEL(editor->error), "Sampler başlığı gir.");
            gtk_widget_grab_focus(editor->title);
            g_free(title);
            return;
        }
        g_free(pad->title);
        g_free(pad->file);
        pad->title = title;
        pad->file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(editor->file));
        ++pad->play_epoch;
        if (pad->owner->ctx->audio && (pad->pending || pad_voice_owned(pad)))
            radios_audio_ui_stop(pad->owner->ctx, pad->channel_number ? RADIOS_AUDIO_SAMPLER_BASE + pad->channel_number - 1 : RADIOS_AUDIO_MASTER_SAMPLER, NULL);
        pad_binding_clear(pad);
        pad->media_id = g_strdup(editor->media_id); pad->version_id = g_strdup(editor->version_id);
        pad->media_file_id = g_strdup(editor->media_file_id); pad->media_sha256 = g_strdup(editor->media_sha256);
        pad->pending = FALSE; pad->loading = FALSE; pad->generation = 0;
        pad->selected = FALSE;
        pad_render(pad);
    } else if (response == GTK_RESPONSE_REJECT) {
        ++pad->play_epoch;
        if (pad->owner->ctx->audio && (pad->pending || pad_voice_owned(pad)))
            radios_audio_ui_stop(pad->owner->ctx, pad->channel_number ? RADIOS_AUDIO_SAMPLER_BASE + pad->channel_number - 1 : RADIOS_AUDIO_MASTER_SAMPLER, NULL);
        pad_binding_clear(pad); pad->pending = FALSE; pad->loading = FALSE; pad->generation = 0;
        g_clear_pointer(&pad->title, g_free);
        g_clear_pointer(&pad->file, g_free);
        pad->selected = FALSE;
        pad_render(pad);
    }
    if ((response == GTK_RESPONSE_ACCEPT || response == GTK_RESPONSE_REJECT) && pad->owner->ctx->notify != NULL)
        pad->owner->ctx->notify(pad->owner->ctx->notify_data,
            pad->owner->ctx->audio ? "Sampler ataması veritabanına gönderiliyor." : pad->owner->ctx->data ? "Sampler ataması veritabanına gönderiliyor; ses motoru henüz bağlı değil." :
            "Sampler atama taslağı güncellendi; ses motoru henüz bağlı değil.");
    if (response == GTK_RESPONSE_ACCEPT || response == GTK_RESPONSE_REJECT) sampler_save(pad->owner);
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
pad_editor_changed(GtkWidget *widget, gpointer data)
{
    PadEditor *editor = data; (void)widget;
    if (editor->pad && !editor->pad->disposed && radios_data_document_ready(editor->pad->owner->sampler))
        radios_data_document_dirty(editor->pad->owner->sampler, TRUE);
}

static void
pad_file_set(GtkFileChooserButton *chooser, gpointer data)
{
    PadEditor *editor = data;
    pad_editor_changed(GTK_WIDGET(chooser), editor);
    gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    if (filename != NULL && *gtk_entry_get_text(GTK_ENTRY(editor->title)) == '\0') {
        gchar *basename = g_path_get_basename(filename);
        gtk_entry_set_text(GTK_ENTRY(editor->title), basename);
        g_free(basename);
    }
    g_free(filename);
}

static void
pad_editor_open(GtkButton *button, gpointer data)
{
    PadState *pad = data;
    (void)button;
    if (pad->disposed)
        return;
    if (pad->dialog != NULL) {
        gtk_window_present(GTK_WINDOW(pad->dialog));
        return;
    }
    gchar *title = g_strdup_printf("%s · Sampler %u", pad->channel_name, pad->number);
    GtkWidget *toplevel = gtk_widget_get_toplevel(pad->button);
    GtkWindow *parent = GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : pad->owner->ctx->parent;
    GtkWidget *dialog = gtk_dialog_new_with_buttons(title, parent,
        GTK_DIALOG_DESTROY_WITH_PARENT, "Atamayı kaldır", GTK_RESPONSE_REJECT,
        "Vazgeç", GTK_RESPONSE_CANCEL, "Ata", GTK_RESPONSE_ACCEPT, NULL);
    PadEditor *editor = g_new0(PadEditor, 1);
    GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    GtkWidget *fields = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, "dj-sampler-dialog-fields");
    static const gchar *const items[] = { "Girdi seç · katalog bağlı değil", NULL };
    GtkWidget *library = pad->owner->ctx->audio ? radios_ui_button(pad->media_id ? "Atanan medyayı değiştir" : "Kütüphaneden seç") : radios_ui_combo(items, 0);
    editor->library = library;
    if (pad->owner->ctx->audio) {
        GtkWidget *caption = gtk_bin_get_child(GTK_BIN(library)); gtk_label_set_ellipsize(GTK_LABEL(caption), PANGO_ELLIPSIZE_END); gtk_label_set_max_width_chars(GTK_LABEL(caption), 32);
    }
    editor->media_id = g_strdup(pad->media_id); editor->version_id = g_strdup(pad->version_id);
    editor->media_file_id = g_strdup(pad->media_file_id); editor->media_sha256 = g_strdup(pad->media_sha256);
    editor->pad = pad;
    editor->title = radios_ui_entry("Başlık");
    editor->file = gtk_file_chooser_button_new("Yerel ses dosyası", GTK_FILE_CHOOSER_ACTION_OPEN);
    editor->error = radios_ui_label("", "ops-help");
    gtk_entry_set_max_length(GTK_ENTRY(editor->title), 160);
    gtk_entry_set_text(GTK_ENTRY(editor->title), pad->title != NULL ? pad->title : "");
    gtk_widget_set_name(dialog, "dj-sampler-dialog");
    gtk_widget_set_name(editor->title, "dj-sampler-title");
    gtk_widget_set_name(library, "dj-sampler-item");
    gtk_widget_set_sensitive(library, pad->owner->ctx->audio != NULL);
    if (pad->owner->ctx->audio) { g_signal_connect(library, "clicked", G_CALLBACK(pad_library_open), editor); gtk_widget_set_sensitive(editor->file, FALSE); }
    gtk_dialog_set_response_sensitive(GTK_DIALOG(dialog), GTK_RESPONSE_REJECT, pad->title != NULL);
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "ops-dialog");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "radios");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "dj-sampler-dialog");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 450, -1);
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_container_set_border_width(GTK_CONTAINER(body), 18);
    pack(fields, radios_ui_label(pad->owner->ctx->audio ? "Kütüphane medyası" : "Program / playlist girdisi", "dj-small-label"), FALSE);
    pack(fields, library, FALSE);
    pack(fields, radios_ui_label("Sampler başlığı", "dj-small-label"), FALSE);
    pack(fields, editor->title, FALSE);
    pack(fields, radios_ui_label(pad->owner->ctx->audio ? "Dosya bilgisi · kütüphaneden seç" : "Yerel ses dosyası · isteğe bağlı", "dj-small-label"), FALSE);
    pack(fields, editor->file, FALSE);
    pack(fields, editor->error, FALSE);
    pack(body, fields, FALSE);
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Ses dosyaları");
    radios_ui_file_filter_add_audio(filter);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(editor->file), filter);
    if (pad->file != NULL)
        gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(editor->file), pad->file);
    pad->dialog = dialog;
    g_object_set_data_full(G_OBJECT(dialog), "pad-editor", editor, pad_editor_free);
    g_object_set_data(G_OBJECT(dialog), "pad-title", editor->title);
    g_signal_connect(dialog, "destroy", G_CALLBACK(pad_dialog_destroyed), editor);
    g_signal_connect(dialog, "response", G_CALLBACK(pad_response), editor);
    g_signal_connect(editor->title, "changed", G_CALLBACK(pad_editor_changed), editor);
    g_signal_connect(editor->file, "file-set", G_CALLBACK(pad_file_set), editor);
    gtk_widget_show_all(dialog);
    gtk_widget_grab_focus(editor->title);
    g_free(title);
}

typedef struct { guint64 epoch, loss_epoch; } PadPlay;
static void pad_media_resolved(RadioSViewContext *ctx, GtkWidget *owner, JsonNode *result, const GError *error, gpointer data)
{
    PadPlay *request = data; PadState *pad = g_object_get_data(G_OBJECT(owner), "dj-pad-state");
    if (!pad || pad->disposed || pad->owner->disposed || request->epoch != pad->play_epoch) return;
    pad->pending = FALSE;
    if (request->loss_epoch != radios_audio_ui_loss_epoch(ctx)) {
        if (ctx->notify) ctx->notify(ctx->notify_data, "Ses bağlantısı kesildi; sampler için yeniden oynatın.");
        pad_render(pad); return;
    }
    if (error || !result || !JSON_NODE_HOLDS_OBJECT(result)) {
        if (ctx->notify) ctx->notify(ctx->notify_data, error ? error->message : "Sampler medyası yüklenemedi.");
        pad_render(pad); return;
    }
    JsonObject *detail = json_node_get_object(result), *media = json_object_get_object_member(detail, "media"), *version = json_object_get_object_member(detail, "version");
    if (!media || !version || !g_str_equal(radios_json_string(media, "_id", ""), pad->media_id) ||
        !g_str_equal(radios_json_string(version, "_id", ""), pad->version_id) ||
        !g_str_equal(radios_json_string(version, "media_file_id", ""), pad->media_file_id) ||
        !g_str_equal(radios_json_string(version, "sha256", ""), pad->media_sha256)) {
        if (ctx->notify) ctx->notify(ctx->notify_data, "Atanan medya sürümü değişti. Sampler için kütüphaneden yeniden seç.");
        pad_render(pad); return;
    }
    GtkWidget *play_owner = pad->channel_number ? channel_find(pad->owner, pad->channel_number) : pad->owner->master;
    if (!play_owner) return;
    guint deck = pad->channel_number ? RADIOS_AUDIO_SAMPLER_BASE + pad->channel_number - 1 : RADIOS_AUDIO_MASTER_SAMPLER; GError *failure = NULL;
    if (!radios_audio_ui_load_media(ctx, play_owner, detail, deck, TRUE, &failure)) { audio_error(pad->owner, failure); pad_render(pad); return; }
    RadioSAudioUiSnapshot snapshot; if (radios_audio_ui_get_snapshot(ctx, deck, &snapshot)) pad->generation = snapshot.generation;
    pad->pending = FALSE; pad->loading = TRUE; pad->selected = FALSE; pad_render(pad);
}
static void
pad_clicked(GtkButton *button, gpointer data)
{
    PadState *pad = data;
    if (pad->disposed)
        return;
    if (pad->owner->ctx->audio) {
        if (!pad->media_id) { pad_editor_open(button, pad); return; }
        guint deck = pad->channel_number ? RADIOS_AUDIO_SAMPLER_BASE + pad->channel_number - 1 : RADIOS_AUDIO_MASTER_SAMPLER;
        RadioSAudioUiSnapshot snapshot = {0}; radios_audio_ui_get_snapshot(pad->owner->ctx, deck, &snapshot);
        gboolean stopping = pad->pending || (pad->generation == snapshot.generation && g_str_equal(snapshot.media_id, pad->media_id) &&
            (snapshot.state == RADIOS_AUDIO_UI_PLAYING || snapshot.state == RADIOS_AUDIO_UI_LOADING ||
             snapshot.state == RADIOS_AUDIO_UI_READY || snapshot.state == RADIOS_AUDIO_UI_PAUSED));
        for (guint i = 0; i < pad->owner->pads->len; ++i) {
            PadState *other = g_ptr_array_index(pad->owner->pads, i); if (other->channel_number != pad->channel_number) continue;
            ++other->play_epoch; other->pending = FALSE; other->loading = FALSE; other->selected = FALSE; other->generation = 0; pad_render(other);
        }
        radios_audio_ui_stop(pad->owner->ctx, deck, NULL); if (stopping) return;
        pad->pending = TRUE; pad_render(pad); PadPlay *request = g_new0(PadPlay, 1); request->epoch = pad->play_epoch;
        request->loss_epoch = radios_audio_ui_loss_epoch(pad->owner->ctx);
        if (!radios_media_library_get(pad->owner->ctx, pad->button, pad->media_id, pad_media_resolved, request, g_free)) { pad->pending = FALSE; pad_render(pad); }
        return;
    }
    if (pad->title == NULL) {
        pad_editor_open(button, pad);
        return;
    }
    pad->selected = !pad->selected;
    pad_render(pad);
    if (pad->owner->ctx->notify != NULL)
        pad->owner->ctx->notify(pad->owner->ctx->notify_data,
            "Sampler arayüz seçimi değişti; ses motoru bağlı olmadığı için ses çalınmadı.");
}

static void
pad_destroyed(GtkWidget *widget, gpointer data)
{
    PadState *pad = data;
    (void)widget;
    pad->disposed = TRUE;
    g_object_set_data(G_OBJECT(pad->button), "dj-pad-state", NULL);
    g_ptr_array_remove(pad->owner->pads, pad);
    g_signal_handlers_disconnect_by_data(pad->button, pad);
    g_signal_handlers_disconnect_by_data(pad->edit, pad);
    if (pad->dialog != NULL)
        gtk_widget_destroy(pad->dialog);
}

static void
pad_free(gpointer data)
{
    PadState *pad = data;
    g_free(pad->title);
    g_free(pad->file);
    g_free(pad->channel_name);
    g_free(pad->key);
    pad_binding_clear(pad);
    g_free(pad);
}

static GtkWidget *
pad_new(DJState *owner, const gchar *channel_name, guint number)
{
    PadState *pad = g_new0(PadState, 1);
    GtkWidget *wrap = gtk_overlay_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(wrap), "dj-pad");
    GtkWidget *copy = radios_ui_box(GTK_ORIENTATION_VERTICAL, 1, "dj-pad-content");
    GtkWidget *edit = radios_ui_button("⋯");
    gchar *text = g_strdup_printf("%02u", number);
    pad->owner = owner;
    pad->number = number;
    pad->channel_number = owner->building_channel;
    pad->key = g_strdup_printf("channel-%u-pad-%u", owner->building_channel, number);
    g_ptr_array_add(owner->pads, pad);
    pad->edit = edit;
    pad->channel_name = g_strdup(channel_name);
    pad->button = gtk_button_new();
    gchar *button_id = g_strdup_printf("dj-sampler-%s", pad->key);
    gtk_widget_set_name(pad->button, button_id);
    gchar *edit_id = g_strdup_printf("%s-edit", button_id);
    gtk_widget_set_name(edit, edit_id); g_free(button_id); g_free(edit_id);
    pad->title_label = radios_ui_label("Boş", "dj-pad-title");
    pad->status_label = radios_ui_label("", "dj-pad-state");
    gtk_label_set_ellipsize(GTK_LABEL(pad->title_label), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(pad->title_label), 8);
    gtk_label_set_line_wrap(GTK_LABEL(pad->title_label), FALSE);
    gtk_style_context_add_class(gtk_widget_get_style_context(pad->button), "dj-sampler-pad");
    gtk_style_context_add_class(gtk_widget_get_style_context(edit), "dj-pad-edit");
    pack(copy, radios_ui_label(text, "dj-pad-number"), FALSE);
    pack(copy, pad->title_label, FALSE);
    pack(copy, pad->status_label, FALSE);
    gtk_container_add(GTK_CONTAINER(pad->button), copy);
    gtk_container_add(GTK_CONTAINER(wrap), pad->button);
    gtk_overlay_add_overlay(GTK_OVERLAY(wrap), edit);
    gtk_widget_set_halign(edit, GTK_ALIGN_END); gtk_widget_set_valign(edit, GTK_ALIGN_START);
    gtk_widget_set_margin_top(edit, 3); gtk_widget_set_margin_end(edit, 3);
    gtk_widget_set_size_request(pad->button, 80, 80);
    gtk_widget_set_tooltip_text(edit, "Sampler içeriğini düzenle");
    g_object_set_data_full(G_OBJECT(wrap), "dj-pad-state", pad, pad_free);
    g_object_set_data(G_OBJECT(wrap), "dj-pad-button", pad->button);
    g_object_set_data(G_OBJECT(wrap), "dj-pad-edit", edit);
    g_object_set_data(G_OBJECT(pad->button), "dj-pad-state", pad);
    g_signal_connect(pad->button, "clicked", G_CALLBACK(pad_clicked), pad);
    g_signal_connect(edit, "clicked", G_CALLBACK(pad_editor_open), pad);
    g_signal_connect(wrap, "destroy", G_CALLBACK(pad_destroyed), pad);
    g_free(text);
    pad_render(pad);
    return wrap;
}

static gchar *channel_default_name(ChannelUI *ui)
{
    if (ui->master) return g_strdup("MASTER");
    if (ui->index < 6) return g_strdup_printf("Player %u", ui->index + 1);
    if (!ui->owner->ctx->audio && ui->index < 8) return g_strdup(ui->index == 6 ? "Mikrofon" : "Jingle");
    return g_strdup_printf("Kanal %u", ui->index + 1);
}

static void channel_name_update(ChannelUI *ui)
{
    gchar *name = g_strdup(gtk_entry_get_text(GTK_ENTRY(ui->name)));
    g_strstrip(name);
    if (!*name) {
        g_free(name);
        name = channel_default_name(ui);
    }
    g_object_set_data_full(G_OBJECT(ui->widget), "dj-channel-name", name, g_free);
    GtkWidget *title = g_object_get_data(G_OBJECT(ui->widget), "dj-channel-title");
    gchar *caption = ui->master ? g_strdup_printf("M  %s", name) : g_strdup_printf("%02u  %s", ui->index + 1, name);
    gtk_label_set_text(GTK_LABEL(title), caption);
    gtk_label_set_ellipsize(GTK_LABEL(title), PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(GTK_LABEL(title), 18);
    gtk_widget_set_tooltip_text(title, caption);
    g_free(caption);
    GtkWidget *grid = g_object_get_data(G_OBJECT(ui->widget), "dj-pads");
    GList *pads = gtk_container_get_children(GTK_CONTAINER(grid));
    for (GList *item = pads; item; item = item->next) {
        PadState *pad = g_object_get_data(G_OBJECT(item->data), "dj-pad-state");
        g_free(pad->channel_name); pad->channel_name = g_strdup(name);
        pad_render(pad);
        if (pad->dialog) {
            gchar *dialog_title = g_strdup_printf("%s · Sampler %u", name, pad->number);
            gtk_window_set_title(GTK_WINDOW(pad->dialog), dialog_title); g_free(dialog_title);
        }
    }
    g_list_free(pads);
}

static void channel_name_changed(GtkEditable *entry, gpointer data)
{
    (void)entry;
    ChannelUI *ui = g_object_get_data(G_OBJECT(data), "dj-channel-state");
    DJState *state = ui->owner;
    if (ui->disposed || state->disposed || state->loading) return;
    channel_name_update(ui);
    channel_routes_update(state); routes_update(state);
    channel_save(state);
}

static void
channels_reindex(DJState *state)
{
    GList *channels = gtk_container_get_children(GTK_CONTAINER(state->channels));
    guint number = 0;
    for (GList *item = channels; item; item = item->next) {
        GtkWidget *channel = item->data; ++number;
        ChannelUI *ui = g_object_get_data(G_OBJECT(channel), "dj-channel-state"); ui->index = number - 1;
        gchar *widget_id = g_strdup_printf("dj-channel-%u", number);
        gtk_widget_set_name(channel, widget_id); g_free(widget_id);
        widget_name(ui->name, "dj-channel", number, "name");
        channel_name_update(ui);
        GtkWidget *remove = g_object_get_data(G_OBJECT(channel), "dj-channel-remove");
        gchar *remove_id = g_strdup_printf("dj-channel-%u-remove", number);
        gtk_widget_set_name(remove, remove_id); g_free(remove_id);
        GtkWidget *grid = g_object_get_data(G_OBJECT(channel), "dj-pads");
        GList *pads = gtk_container_get_children(GTK_CONTAINER(grid));
        for (GList *pad_item = pads; pad_item; pad_item = pad_item->next) {
            PadState *pad = g_object_get_data(G_OBJECT(pad_item->data), "dj-pad-state");
            pad->channel_number = number;
            g_free(pad->key); pad->key = g_strdup_printf("channel-%u-pad-%u", number, pad->number);
            gchar *button_id = g_strdup_printf("dj-sampler-%s", pad->key);
            gchar *edit_id = g_strdup_printf("%s-edit", button_id);
            gtk_widget_set_name(pad->button, button_id); gtk_widget_set_name(pad->edit, edit_id);
            g_free(button_id); g_free(edit_id);
        }
        g_list_free(pads);
    }
    g_list_free(channels);
    state->channel_count = state->next_channel = number;
}

static gboolean channels_can_change(DJState *state)
{
    if (!state->ctx->data || state->loading) return TRUE;
    if (radios_data_connected(state->ctx) && radios_data_document_ready(state->channel_document) &&
        radios_data_document_ready(state->sampler)) return TRUE;
    if (state->ctx->notify) state->ctx->notify(state->ctx->notify_data,
        "DJ kanal ve sampler kayıtları yüklenmeden kanal düzeni değiştirilemez.");
    return FALSE;
}

static void
channel_remove(GtkButton *button, gpointer data)
{
    DJState *state = data;
    if (!channels_can_change(state)) return;
    if (state->channel_count <= 1) {
        if (state->ctx->notify) state->ctx->notify(state->ctx->notify_data, "DJ görünümünde en az bir kanal kalmalı.");
        return;
    }
    GtkWidget *channel = g_object_get_data(G_OBJECT(button), "channel-widget");
    if (state->ctx->audio) {
        ChannelUI *ui = g_object_get_data(G_OBJECT(channel), "dj-channel-state"); RadioSAudioMixerConfig config;
        if (radios_audio_ui_get_mixer(state->ctx, &config, NULL, NULL)) {
            config.channels[ui->index].source_deck = -1; config.channels[ui->index].mute = TRUE; config.channels[ui->index].pfl = FALSE;
            config.channels[ui->index].send_channel = config.channels[ui->index].return_channel = -1;
            config.channels[ui->index].return_effect = -1; config.channels[ui->index].fx_slot = -1;
            if (config.speech_channel == (gint)ui->index) config.speech_channel = -1;
            if (config.master.send_channel == (gint)ui->index) config.master.send_channel = -1;
            if (config.master.return_channel == (gint)ui->index) config.master.return_channel = -1;
            for (guint index = 0; index < 32; ++index) {
                if (config.channels[index].send_channel == (gint)ui->index) config.channels[index].send_channel = -1;
                if (config.channels[index].return_channel == (gint)ui->index) config.channels[index].return_channel = -1;
            }
            if (radios_audio_ui_set_mixer(state->ctx, &config, NULL)) {
                ChannelUI *master = g_object_get_data(G_OBJECT(state->master), "dj-channel-state");
                if (!any_cue(&config) && !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(master->cue)))
                    radios_audio_ui_cue_mix_release(state->ctx);
            }
        }
        /* The removed strip's live input stops and its broadcast label clears. */
        radios_audio_ui_set_channel_input(state->ctx, ui->index + 1, NULL, NULL);
        radios_audio_ui_set_channel_label(state->ctx, ui->index + 1, NULL, NULL);
    }
    gtk_widget_destroy(channel);
    if (state->ctx->audio) --state->channel_count; else channels_reindex(state);
    update_count(state);
    routes_update(state);
    channel_save(state); sampler_save(state);
}

static GtkWidget *
channel_new(DJState *state, guint number, const gchar *name, gboolean master)
{
    state->building_channel = number;
    static const gchar *const sources[] = { "Kaynak seç", "Player 1", "Player 2", "Player 3", "Player 4", "Player 5", "Player 6", "Stüdyo mikrofonu", "Jingle bankası", NULL };
    static const gchar *const effectors[] = { "Effector seç", "Effector 1", "Effector 2", "Effector 3", "Effector 4", NULL };
    GtkWidget *channel = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, master ? "dj-channel dj-master-channel dj-mixer-master" : "dj-channel");
    ChannelUI *ui = g_new0(ChannelUI, 1); ui->owner = state; ui->widget = channel; ui->master = master; ui->index = master ? 0 : number - 1;
    g_object_set_data_full(G_OBJECT(channel), "dj-channel-state", ui, g_free);
    g_signal_connect(channel, "destroy", G_CALLBACK(channel_ui_destroyed), ui);
    gchar *channel_id = g_strdup_printf("dj-channel-%u", number);
    gtk_widget_set_name(channel, channel_id); g_free(channel_id);
    GtkWidget *head = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 4, "dj-channel-heading");
    GtkWidget *pads = gtk_grid_new();
    GtkWidget *buses = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 10, "dj-channel-buses");
    GtkWidget *volume = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "dj-channel-volume");
    GtkWidget *switches = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 4, "dj-channel-switches");
    gchar *title = master ? g_strdup("M  MASTER") : g_strdup_printf("%02u  %s", number, name);
    GtkWidget *title_label = radios_ui_label(title, "dj-channel-title heading");
    g_object_set_data(G_OBJECT(channel), "dj-channel-title", title_label);
    pack(head, title_label, TRUE);
    if (!master) {
        GtkWidget *remove = radios_ui_button("×");
        gchar *remove_id = g_strdup_printf("dj-channel-%u-remove", number);
        gtk_widget_set_name(remove, remove_id); g_free(remove_id);
        g_object_set_data(G_OBJECT(channel), "dj-channel-remove", remove);
        g_object_set_data(G_OBJECT(remove), "channel-widget", channel);
        g_signal_connect(remove, "clicked", G_CALLBACK(channel_remove), state);
        pack(head, remove, FALSE);
    }
    g_free(title);
    pack(channel, head, FALSE);
    pack(channel, radios_ui_label("SAMPLER", "dj-sampler-label"), FALSE);
    gtk_grid_set_column_spacing(GTK_GRID(pads), 5);
    gtk_grid_set_row_spacing(GTK_GRID(pads), 5);
    gtk_grid_set_column_homogeneous(GTK_GRID(pads), TRUE);
    gtk_grid_set_row_homogeneous(GTK_GRID(pads), TRUE);
    gtk_widget_set_halign(pads, GTK_ALIGN_CENTER);
    gtk_style_context_add_class(gtk_widget_get_style_context(pads), "dj-channel-pads");
    for (guint i = 0; i < 4; ++i) {
        GtkWidget *pad = pad_new(state, name, i + 1);
        gtk_grid_attach(GTK_GRID(pads), pad, i % 2, i / 2, 1, 1);
    }
    pack(channel, pads, FALSE);
    GtkWidget *source_box = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, "dj-channel-source");
    GtkWidget *effector_box = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, "dj-channel-source dj-channel-effector");
    pack(source_box, radios_ui_label(master ? "EFFECT" : "Kaynak", "dj-small-label"), FALSE);
    GtkWidget *source_combo = radios_ui_combo(master ? effects : sources, !master && number <= 8 ? (gint)number : 0);
    GtkWidget *effector_combo = radios_ui_combo(effectors, 0);
    ui->source = source_combo; ui->effector = effector_combo;
    if (state->ctx->audio) {
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(source_combo));
        if (master) {
            for (guint i = 0; i < 14; ++i) { gchar id[16]; g_snprintf(id, sizeof id, "%u", i); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(source_combo), id, effects[i]); }
            combo_set(source_combo, 0);
        } else {
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(source_combo), "-1", "Kaynak yok");
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(source_combo), "-2", "Stüdyo mikrofonu");
            for (guint i = 0; i < 32; ++i) { gchar id[16], text[40]; g_snprintf(id, sizeof id, "%u", i); g_snprintf(text, sizeof text, "Oynatıcı %u", i + 1); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(source_combo), id, text); }
            combo_set(source_combo, (gint)ui->index);
            source_options_fill(state, ui); /* sound-card inputs and network sources (R5.1) */
        }
        gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(effector_combo));
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(effector_combo), "-1", "Effector seç");
        for (guint i = 0; i < DJ_FX_PEDALS; ++i) { gchar id[16], text[40]; g_snprintf(id, sizeof id, "%u", i); g_snprintf(text, sizeof text, "Effector %u", i + 1); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(effector_combo), id, text); }
        combo_set(effector_combo, -1);
    }
    widget_name(source_combo, "dj-channel", number, master ? "effect-type" : "source"); widget_name(effector_combo, "dj-channel", number, "effector");
    pack(source_box, source_combo, FALSE);
    if (state->ctx->audio && !master) {
        /* The label names the strip's broadcast role in a live step (R5.2):
         * DJ opens the DJ microphone, Konuşma the talk channel and its live
         * event scheme, Müzik the automation scheme. */
        ui->label = gtk_combo_box_text_new();
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->label), "none", "—");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->label), "dj", "DJ");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->label), "talk", "Konuşma");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->label), "music", "Müzik");
        gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui->label), "none");
        widget_name(ui->label, "dj-channel", number, "label");
        gtk_widget_set_tooltip_text(ui->label, "Etiket: DJ, Konuşma veya Müzik; canlı yayın adımında kanalın işlevini belirler.");
        pack(source_box, radios_ui_label("Etiket", "dj-small-label"), FALSE);
        pack(source_box, ui->label, FALSE);
        g_signal_connect_object(ui->label, "changed", G_CALLBACK(channel_label_changed), channel, 0);
    }
    pack(channel, source_box, FALSE);
    pack(effector_box, radios_ui_label("SEND → Effector", "dj-small-label"), FALSE);
    pack(effector_box, effector_combo, FALSE);
    g_object_set_data(G_OBJECT(channel), "dj-effector", effector_combo);
    if (!master)
        g_object_set_data(G_OBJECT(channel), "dj-source", source_combo);
    g_object_set_data_full(G_OBJECT(channel), "dj-channel-name", g_strdup(name), g_free);
    g_object_set_data(G_OBJECT(channel), "dj-pads", pads);
    g_signal_connect(source_combo, "changed", G_CALLBACK(route_changed), state);
    g_signal_connect(effector_combo, "changed", G_CALLBACK(route_changed), state);
    pack(channel, effector_box, FALSE);
    ui->send = compact_knob("FX SEND", -60, 6, 0.5, 0, 38); ui->return_gain = compact_knob("FX RETURN", -60, 6, 0.5, -6, 38);
    widget_name(ui->send, "dj-channel", number, "send"); widget_name(ui->return_gain, "dj-channel", number, "return");
    gtk_widget_set_tooltip_text(ui->return_gain, master ? "MASTER EFFECT seçicisinin ayrı efekt seviyesi; paylaşılan efektör dönüşünü alttaki RETURN kontrolü ayarlar." : "Seçili efektörün dönüş seviyesi; alttaki RETURN kontrolüyle aynı ayarı kullanır.");
    pack(buses, ui->send, TRUE); pack(buses, ui->return_gain, TRUE);
    GtkWidget *fx_levels = gtk_expander_new("Efekt seviyesi");
    gtk_container_add(GTK_CONTAINER(fx_levels), buses); pack(channel, fx_levels, FALSE);
    {
        GtkWidget *routing = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, "dj-channel-routing");
        ui->send_target = gtk_combo_box_text_new(); ui->return_source = gtk_combo_box_text_new();
        widget_name(ui->send_target, "dj-channel", number, "send-target");
        widget_name(ui->return_source, "dj-channel", number, "return-source");
        pack(routing, radios_ui_label("SEND → Kanal", "dj-small-label"), FALSE); pack(routing, ui->send_target, FALSE);
        pack(routing, radios_ui_label("RETURN ← Effector", "dj-small-label"), FALSE); pack(routing, ui->return_source, FALSE);
        gtk_widget_set_tooltip_text(ui->return_source, "Dönüşün alınacağı efektörü seçer. Yalnız efektörün işlenmiş sesi RETURN seviyesinde eklenir; Yok dönüşü kapatır.");
        GtkWidget *levels = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 10, "dj-channel-route-levels");
        ui->route_send = compact_knob("SEND", -60, 6, 0.5, -6, 38);
        ui->route_return = compact_knob("RETURN", -60, 6, 0.5, 0, 38);
        widget_name(ui->route_send, "dj-channel", number, "route-send");
        widget_name(ui->route_return, "dj-channel", number, "route-return");
        gtk_widget_set_tooltip_text(ui->route_return, "Seçili efektörden alınan işlenmiş sesin dönüş seviyesi.");
        pack(levels, ui->route_send, TRUE); pack(levels, ui->route_return, TRUE); pack(routing, levels, FALSE);
        pack(channel, routing, FALSE);
        if (state->ctx->audio) {
            g_signal_connect_object(ui->send_target, "changed", G_CALLBACK(channel_control_changed), channel, 0);
            g_signal_connect_object(ui->return_source, "changed", G_CALLBACK(channel_control_changed), channel, 0);
            knob_connect(ui->route_send, G_CALLBACK(channel_control_changed), channel);
            knob_connect(ui->route_return, G_CALLBACK(channel_control_changed), channel);
        } else gtk_widget_set_sensitive(routing, FALSE);
        if (master) {
            gtk_widget_set_tooltip_text(ui->send_target, "MASTER ön miksini kanala gönderir. Hedef kanal ve devamındaki kanallar gönderime tekrar alınmaz.");
            gtk_widget_set_tooltip_text(ui->return_source, "Seçili efektörün işlenmiş sesini MASTER RETURN seviyesinde alır. Dönüş MASTER SEND'e tekrar gönderilmez.");
        }
    }
    if (master) {
        GtkWidget *controls = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 10, "dj-master-effect-controls");
        ui->depth = compact_knob("DEPTH", 0, 100, 1, 50, 38); ui->power = draft_switch(state, "OFF", "dj-master-effect-toggle", TRUE);
        widget_name(ui->depth, "dj-channel", number, "depth"); widget_name(ui->power, "dj-channel", number, "power");
        pack(controls, ui->depth, TRUE); pack(controls, ui->power, FALSE);
        pack(channel, controls, FALSE);
        if (state->ctx->audio)
            gtk_widget_set_tooltip_text(effector_combo, "MASTER için paylaşılan yardımcı efekt. EFFECT seçicisi ayrı MASTER efektini yönetir.");
    } else {
        ui->pan = compact_knob("PAN", -100, 100, 1, 0, 38); widget_name(ui->pan, "dj-channel", number, "pan"); pack(channel, ui->pan, FALSE);
    }
    if (state->ctx->audio) {
        GtkWidget *eq = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 4, "header-eq-controls");
        const gchar *labels[] = {"LOW", "MID", "HI"}, *ids[] = {"eq-low", "eq-mid", "eq-high"};
        for (guint i = 0; i < 3; ++i) { ui->eq[i] = compact_knob(labels[i], -26, 6, 0.5, 0, 30); widget_name(ui->eq[i], "dj-channel", number, ids[i]); pack(eq, ui->eq[i], TRUE); }
        pack(channel, eq, FALSE);
        if (!master) {
            ui->cross_group = gtk_combo_box_text_new(); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->cross_group), "0", "XFADE · Bağımsız");
            gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->cross_group), "1", "XFADE · A"); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->cross_group), "2", "XFADE · B"); combo_set(ui->cross_group, 0);
            widget_name(ui->cross_group, "dj-channel", number, "cross-group"); pack(channel, ui->cross_group, FALSE);
            GtkWidget *normalization = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 4, "dj-channel-switches");
            ui->normalize = gtk_toggle_button_new_with_label("OTO SES");
            gtk_style_context_add_class(gtk_widget_get_style_context(ui->normalize), "dj-auto-level-button");
            widget_name(ui->normalize, "dj-channel", number, "normalize");
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->normalize), TRUE);
            gtk_widget_set_tooltip_text(ui->normalize, "OTO SES açıkken kanalın ses sürgüsü hedef LUFS düzeyine göre otomatik ayarlanır; elle yaptığınız ayar hedefe geri alınır. Kapattığınızda son elle ayarladığınız düzey geri gelir. Hedef ve kapsam: Tercihler → Ses.");
            ui->normalization = radios_ui_label("Bekliyor", "dj-small-label");
            gtk_label_set_ellipsize(GTK_LABEL(ui->normalization), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(ui->normalization), 14);
            widget_name(ui->normalization, "dj-channel", number, "normalization");
            pack(normalization, ui->normalize, FALSE); pack(normalization, ui->normalization, TRUE);
            pack(channel, normalization, FALSE);
        }
    }
    GtkWidget *fader = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, -60, 6, 0.5);
    gtk_range_set_inverted(GTK_RANGE(fader), TRUE);
    gtk_range_set_value(GTK_RANGE(fader), 0);
    gtk_scale_set_digits(GTK_SCALE(fader), 1);
    gtk_widget_set_size_request(fader, 48, 150);
    ui->fader = fader; ui->meter = radios_meter_new(18, 150, TRUE); widget_name(fader, "dj-channel", number, "gain"); widget_name(ui->meter, "dj-channel", number, "meter");
    if (master && state->ctx->audio) radios_audio_ui_bind_meter(state->ctx, ui->meter, RADIOS_AUDIO_UI_MAIN);
    pack(volume, ui->meter, FALSE);
    pack(volume, fader, TRUE);
    pack(channel, volume, FALSE);
    const gchar *switch_names[] = { "CUE", "M", "S" };
    for (guint i = 0; i < (master ? 2u : 3u); ++i) {
        GtkWidget *toggle = draft_switch(state, switch_names[i], "dj-channel-switch", FALSE);
        const gchar *classes[] = { "dj-switch-cue", "dj-switch-mute", "dj-switch-solo" };
        gtk_style_context_add_class(gtk_widget_get_style_context(toggle), classes[i]);
        const gchar *ids[] = {"cue", "mute", "solo"}; widget_name(toggle, "dj-channel", number, ids[i]);
        if (i == 0) ui->cue = toggle; else if (i == 1) ui->mute = toggle; else ui->solo = toggle;
        pack(switches, toggle, TRUE);
    }
    pack(channel, switches, FALSE);
    if (state->ctx->audio) {
        GtkWidget *controls = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 4, "dj-channel-switches");
        ui->load = radios_ui_button("+ Medya"); widget_name(ui->load, "dj-channel", number, "load");
        ui->play = radios_ui_button("▶"); ui->pause = radios_ui_button("Ⅱ"); ui->stop = radios_ui_button("■");
        GtkWidget *buttons[] = {ui->play, ui->pause, ui->stop}; const gchar *ids[] = {"play", "pause", "stop"};
        for (guint i = 0; i < 3; ++i) { widget_name(buttons[i], "dj-channel", number, ids[i]); g_object_set_data(G_OBJECT(buttons[i]), "dj-transport-action", GUINT_TO_POINTER(i + 1)); pack(controls, buttons[i], TRUE); g_signal_connect_object(buttons[i], "clicked", G_CALLBACK(channel_transport), channel, 0); }
        ui->seek = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 1, 1); gtk_scale_set_draw_value(GTK_SCALE(ui->seek), FALSE); widget_name(ui->seek, "dj-channel", number, "seek");
        ui->position = radios_ui_label("Kaynak bekleniyor", "dj-small-label"); widget_name(ui->position, "dj-channel", number, "position"); gtk_label_set_ellipsize(GTK_LABEL(ui->position), PANGO_ELLIPSIZE_END); gtk_label_set_max_width_chars(GTK_LABEL(ui->position), 20);
        pack(channel, ui->load, FALSE);
        pack(channel, controls, FALSE); pack(channel, ui->seek, FALSE); pack(channel, ui->position, FALSE);
        g_signal_connect_object(ui->load, "clicked", G_CALLBACK(channel_load), channel, 0); g_signal_connect_object(ui->seek, "change-value", G_CALLBACK(channel_seek), channel, 0);
        g_signal_connect_object(source_combo, "changed", G_CALLBACK(channel_control_changed), channel, 0);
        g_signal_connect_object(effector_combo, "changed", G_CALLBACK(channel_control_changed), channel, 0);
        g_signal_connect_object(ui->mute, "toggled", G_CALLBACK(channel_control_changed), channel, 0);
        g_signal_connect_object(ui->cue, "toggled", G_CALLBACK(channel_control_changed), channel, 0);
        g_signal_connect_object(fader, "value-changed", G_CALLBACK(channel_gain_changed), channel, 0);
        knob_connect(ui->send, G_CALLBACK(channel_control_changed), channel); knob_connect(ui->return_gain, G_CALLBACK(channel_control_changed), channel);
        for (guint i = 0; i < 3; ++i) knob_connect(ui->eq[i], G_CALLBACK(channel_control_changed), channel);
        if (master) { knob_connect(ui->depth, G_CALLBACK(channel_control_changed), channel); g_signal_connect_object(ui->power, "toggled", G_CALLBACK(channel_control_changed), channel, 0); }
        else { knob_connect(ui->pan, G_CALLBACK(channel_control_changed), channel); g_signal_connect_object(ui->solo, "toggled", G_CALLBACK(channel_control_changed), channel, 0); g_signal_connect_object(ui->cross_group, "changed", G_CALLBACK(channel_control_changed), channel, 0); g_signal_connect_object(ui->normalize, "toggled", G_CALLBACK(channel_control_changed), channel, 0); }
    }
    GtkWidget *name_box = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, "dj-channel-name-field");
    ui->name = radios_ui_entry(master ? "MASTER adı" : "Kanal adı");
    widget_name(ui->name, "dj-channel", number, "name");
    gtk_entry_set_max_length(GTK_ENTRY(ui->name), 64);
    gtk_entry_set_width_chars(GTK_ENTRY(ui->name), 12);
    gtk_entry_set_text(GTK_ENTRY(ui->name), name);
    gtk_widget_set_sensitive(ui->name, !state->ctx->data || !state->ctx->audio || radios_data_document_ready(state->channel_document));
    gtk_widget_set_tooltip_text(ui->name, "Kanal adı otomatik olarak veritabanına kaydedilir.");
    pack(name_box, radios_ui_label("Kanal adı", "dj-small-label"), FALSE);
    pack(name_box, ui->name, FALSE); pack(channel, name_box, FALSE);
    channel_name_update(ui);
    g_signal_connect_object(ui->name, "changed", G_CALLBACK(channel_name_changed), channel, 0);
    gtk_widget_set_size_request(channel, MAX(162, state->strip_width), -1);
    return channel;
}

static void
add_channel(GtkButton *button, gpointer data)
{
    DJState *state = data;
    gchar *name;
    GtkWidget *channel;
    (void)button;
    if (!channels_can_change(state)) return;
    if (state->channel_count >= 32) {
        if (state->ctx->notify != NULL)
            state->ctx->notify(state->ctx->notify_data, "Mixer arayüzü en fazla 32 kanal gösterir.");
        return;
    }
    ++state->next_channel;
    if (state->ctx->audio) {
        state->next_channel = 1; while (state->next_channel <= 32 && channel_find(state, state->next_channel)) ++state->next_channel;
    }
    name = g_strdup_printf("Kanal %u", state->next_channel);
    channel = channel_new(state, state->next_channel, name, FALSE);
    pack(state->channels, channel, FALSE);
    gtk_widget_show_all(channel);
    ++state->channel_count;
    update_count(state);
    routes_update(state);
    channel_save(state);
    if (state->ctx->audio && !state->loading) {
        radios_audio_ui_set_channel_input(state->ctx, state->next_channel, NULL, NULL);
        RadioSAudioMixerConfig config; if (radios_audio_ui_get_mixer(state->ctx, &config, NULL, NULL)) {
            config.channels[state->next_channel - 1].source_deck = (gint)state->next_channel - 1; config.channels[state->next_channel - 1].mute = FALSE;
            radios_audio_ui_set_mixer(state->ctx, &config, NULL);
        }
    }
    g_free(name);
}

static void channels_ensure(DJState *state, guint count)
{
    gboolean loading = state->loading; state->loading = TRUE;
    count = CLAMP(count, 1u, 32u);
    while (state->channel_count < count) add_channel(NULL, state);
    state->loading = loading;
}

static guint assigned_highest(DJState *state)
{
    guint highest = 0;
    for (guint i = 0; i < state->pads->len; ++i) {
        PadState *pad = g_ptr_array_index(state->pads, i); guint channel = 0, number = 0;
        /* Refresh must also keep the channel of an open, unassigned editor. */
        if ((pad->title || pad->dialog) && sscanf(pad->key, "channel-%u-pad-%u", &channel, &number) == 2 && channel <= 32)
            highest = MAX(highest, channel);
    }
    return highest;
}

static JsonNode *channel_config(DJState *state)
{
    JsonNode *config = json_node_new(JSON_NODE_OBJECT); json_node_take_object(config, json_object_new());
    json_object_set_int_member(json_node_get_object(config), "deck_count", state->channel_count);
    JsonObject *names = json_object_new(); JsonArray *slots = json_array_new();
    GList *channels = gtk_container_get_children(GTK_CONTAINER(state->channels));
    if (state->master) channels = g_list_append(channels, state->master);
    for (GList *item = channels; item; item = item->next) {
        ChannelUI *ui = g_object_get_data(G_OBJECT(item->data), "dj-channel-state");
        gchar id[8]; g_snprintf(id, sizeof id, "%u", ui->master ? 0 : ui->index + 1);
        json_object_set_string_member(names, id, gtk_entry_get_text(GTK_ENTRY(ui->name)));
        if (!ui->master) json_array_add_int_element(slots, ui->index);
    }
    g_list_free(channels);
    json_object_set_object_member(json_node_get_object(config), "names", names);
    if (state->ctx->audio) json_object_set_array_member(json_node_get_object(config), "slots", slots);
    else json_array_unref(slots);
    return config;
}

static gboolean channel_autosave(gpointer data)
{
    DJState *state = data; state->channel_save_timer = 0;
    if (state->disposed || !state->channel_pending) return G_SOURCE_REMOVE;
    if (radios_data_document_revision(state->channel_document) > 0) {
        state->channel_pending = FALSE; return G_SOURCE_REMOVE;
    }
    JsonNode *config = channel_config(state);
    radios_data_document_autosave(state->channel_document, config); json_node_free(config);
    state->channel_save_timer = g_timeout_add(1000, channel_autosave, state);
    return G_SOURCE_REMOVE;
}

static void channel_save(DJState *state)
{
    if (!state->channel_document || state->loading || state->disposed) return;
    state->channel_pending = TRUE;
    radios_data_document_dirty(state->channel_document, TRUE);
    /* Stage every latest edit before returning; the document bounds in-flight
     * work and keeps its exact first intent for retry and close recovery. */
    JsonNode *config = channel_config(state);
    radios_data_document_autosave(state->channel_document, config); json_node_free(config);
    if (!state->channel_save_timer) state->channel_save_timer = g_timeout_add(1000, channel_autosave, state);
}

static void channel_names_apply(DJState *state, JsonObject *config)
{
    JsonNode *node = json_object_get_member(config, "names");
    JsonObject *names = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    GList *channels = gtk_container_get_children(GTK_CONTAINER(state->channels));
    if (state->master) channels = g_list_append(channels, state->master);
    for (GList *item = channels; item; item = item->next) {
        ChannelUI *ui = g_object_get_data(G_OBJECT(item->data), "dj-channel-state");
        gchar id[8]; g_snprintf(id, sizeof id, "%u", ui->master ? 0 : ui->index + 1);
        gchar *fallback = channel_default_name(ui);
        const gchar *name = radios_json_string(names, id, fallback);
        gtk_entry_set_text(GTK_ENTRY(ui->name), name); channel_name_update(ui);
        gtk_widget_set_sensitive(ui->name, TRUE); g_free(fallback);
    }
    g_list_free(channels);
}

static void channels_apply(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data)
{
    DJState *state = data; (void)ctx; (void)owner;
    state->channel_pending = FALSE;
    guint count = (guint)CLAMP(radios_json_int(config, "deck_count", 8), 1, 32);
    JsonNode *slots_node = json_object_get_member(config, "slots");
    if (state->ctx->audio && slots_node && JSON_NODE_HOLDS_ARRAY(slots_node)) {
        gboolean visible[32] = {FALSE}; JsonArray *slots = json_node_get_array(slots_node);
        for (guint i = 0; i < json_array_get_length(slots); ++i) { gint64 slot = json_array_get_int_element(slots, i); if (slot >= 0 && slot < 32) visible[slot] = TRUE; }
        state->loading = TRUE; GList *channels = gtk_container_get_children(GTK_CONTAINER(state->channels));
        for (GList *item = channels; item; item = item->next) { ChannelUI *ui = g_object_get_data(G_OBJECT(item->data), "dj-channel-state"); if (!visible[ui->index]) { gtk_widget_destroy(item->data); --state->channel_count; } }
        g_list_free(channels);
        for (guint i = 0; i < 32; ++i) if (visible[i] && !channel_find(state, i + 1)) {
            gchar *name = g_strdup_printf("Kanal %u", i + 1); GtkWidget *channel = channel_new(state, i + 1, name, FALSE); g_free(name);
            pack(state->channels, channel, FALSE); gtk_widget_show_all(channel); ++state->channel_count;
        }
        channel_names_apply(state, config);
        state->loading = FALSE; update_count(state); routes_update(state); return;
    }
    count = MAX(count, assigned_highest(state));
    state->loading = TRUE;
    GList *channels = gtk_container_get_children(GTK_CONTAINER(state->channels));
    while (state->channel_count > count) {
        GList *last = g_list_last(channels); GtkWidget *channel = last->data;
        channels = g_list_delete_link(channels, last); gtk_widget_destroy(channel); --state->channel_count;
    }
    g_list_free(channels); state->next_channel = state->channel_count;
    channels_ensure(state, count); channel_names_apply(state, config);
    update_count(state); routes_update(state); state->loading = FALSE;
}

void radios_dj_prepare_close(GtkWidget *page)
{
    if (!page) return;
    DJState *state = g_object_get_data(G_OBJECT(page), "dj-state");
    if (!state) {
        /* The profile scroller shares the page's widget name. Locate the
         * actual controller before the data service is closed. */
        if (GTK_IS_CONTAINER(page)) {
            GList *children = gtk_container_get_children(GTK_CONTAINER(page));
            for (GList *item = children; item; item = item->next) radios_dj_prepare_close(item->data);
            g_list_free(children);
        }
        return;
    }
    if (state->disposed) return;
    if (state->channel_save_timer) { g_source_remove(state->channel_save_timer); state->channel_save_timer = 0; }
    if (state->channel_pending) {
        JsonNode *config = channel_config(state);
        radios_data_document_autosave(state->channel_document, config); json_node_free(config);
    }
    radios_data_document_preserve_pending(state->channel_document);
    radios_data_document_preserve_pending(state->sampler);
    /* The data/audio services close next; bring detached controls back into
     * the normal page lifetime before their controller is shut down. */
    state->closing = TRUE;
    mixer_redock(state);
}

static void
dj_disconnect(GtkWidget *root, gpointer data)
{
    g_signal_handlers_disconnect_by_data(root, data);
    if (GTK_IS_CONTAINER(root)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(root));
        for (GList *item = children; item != NULL; item = item->next)
            dj_disconnect(item->data, data);
        g_list_free(children);
    }
}

static void
dj_destroyed(GtkWidget *widget, gpointer data)
{
    DJState *state = data;
    talk_release(state);
    if (state->theme_notice) { g_source_remove(state->theme_notice); state->theme_notice = 0; }
    if (state->ctx->parent) g_signal_handlers_disconnect_by_data(state->ctx->parent, state);
    if (state->pedal_theme) {
        gtk_style_context_remove_provider_for_screen(state->theme_screen, GTK_STYLE_PROVIDER(state->pedal_theme));
        g_clear_object(&state->pedal_theme); g_clear_object(&state->theme_screen);
    }
    state->disposed = TRUE;
    dj_disconnect(widget, state);
    if (state->timer != 0) {
        g_source_remove(state->timer);
        state->timer = 0;
    }
    if (state->audio_timer) { g_source_remove(state->audio_timer); state->audio_timer = 0; }
    if (state->channel_save_timer) { g_source_remove(state->channel_save_timer); state->channel_save_timer = 0; }
    if (state->strip_width_source) { g_source_remove(state->strip_width_source); state->strip_width_source = 0; }
    if (state->mixer_window) {
        GtkWidget *window = state->mixer_window;
        state->mixer_window = NULL;
        /* Direct page destruction may happen without prepare_close. Its
         * detached channels/pads must die while their owner still exists. */
        dj_disconnect(window, state);
        gtk_widget_destroy(window);
    }
}

static gboolean any_cue(const RadioSAudioMixerConfig *config)
{
    for (guint i = 0; i < 32; ++i) if (config->channels[i].pfl) return TRUE;
    for (guint i = 0; i < DJ_FX_PEDALS; ++i) if (config->effects[i].cue) return TRUE;
    return config->master_fx.cue;
}

void radios_dj_set_scale(GtkWidget *page, guint mixer_width, guint mixer_height,
                         guint effector_width, guint effector_height)
{
    if (!page) return;
    DJState *state = g_object_get_data(G_OBJECT(page), "dj-state");
    if (state) {
        if (!state->disposed) {
            radios_content_scale_set_percents(state->mixer_scale, mixer_width, mixer_height);
            radios_content_scale_set_percents(state->effector_scale, effector_width, effector_height);
        }
        return;
    }
    /* The main profile may pass its scroller rather than the DJ controller. */
    if (GTK_IS_CONTAINER(page)) {
        GList *children = gtk_container_get_children(GTK_CONTAINER(page));
        for (GList *item = children; item; item = item->next)
            radios_dj_set_scale(item->data, mixer_width, mixer_height, effector_width, effector_height);
        g_list_free(children);
    }
}
/* Source menu entries beyond the players: "in:<capture endpoint id>" for a
 * sound-card input (a WASAPI endpoint id, "<render endpoint id>.monitor" for
 * the loopback of an output), "net:<url>" for a saved network source and "net-new" for the small
 * window that saves a new one. These select a live channel input; the strip's
 * player assignment is then -1 (R5.1). */
static void source_select(ChannelUI *ui, const gchar *id, const gchar *fallback_text)
{
    if (!id || gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui->source), id)) return;
    if (!g_str_has_prefix(id, "in:") && !g_str_has_prefix(id, "net:")) return;
    /* A selected device that is currently absent stays visible and selected. */
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(ui->source), id, fallback_text ? fallback_text : id);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui->source), id);
}
static void source_options_fill(DJState *state, ChannelUI *ui)
{
    if (ui->master || !state->ctx->audio || !GTK_IS_COMBO_BOX_TEXT(ui->source)) return;
    gboolean syncing = state->syncing; state->syncing = TRUE;
    GtkComboBoxText *combo = GTK_COMBO_BOX_TEXT(ui->source);
    gchar *selected = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo)));
    gchar *selected_text = gtk_combo_box_text_get_active_text(combo);
    gtk_combo_box_text_remove_all(combo);
    gtk_combo_box_text_append(combo, "-1", "Kaynak yok");
    gtk_combo_box_text_append(combo, "-2", "Stüdyo mikrofonu");
    for (guint i = 0; i < 32; ++i) { gchar id[16], text[40]; g_snprintf(id, sizeof id, "%u", i); g_snprintf(text, sizeof text, "Oynatıcı %u", i + 1); gtk_combo_box_text_append(combo, id, text); }
    RadioSAudioDevice *devices = g_new0(RadioSAudioDevice, RADIOS_AUDIO_MAX_DEVICES);
    guint count = radios_audio_ui_capture_devices(state->ctx, devices, RADIOS_AUDIO_MAX_DEVICES);
    for (guint i = 0; i < count; ++i) {
        gchar *id = g_strconcat("in:", devices[i].name, NULL);
        gchar *text = g_strdup_printf("Giriş: %s%s", devices[i].description[0] ? devices[i].description : devices[i].name,
            devices[i].monitor ? " · Çıkış geri dinlemesi" : "");
        gtk_combo_box_text_append(combo, id, text); g_free(id); g_free(text);
    }
    g_free(devices);
    RadioSAudioUiNetworkSource *sources = g_new0(RadioSAudioUiNetworkSource, RADIOS_AUDIO_UI_NETWORK_SOURCES);
    guint network = radios_audio_ui_network_sources(state->ctx, sources, RADIOS_AUDIO_UI_NETWORK_SOURCES);
    for (guint i = 0; i < network; ++i) {
        gchar *id = g_strconcat("net:", sources[i].url, NULL), *text = g_strdup_printf("Ağ: %s", sources[i].name);
        gtk_combo_box_text_append(combo, id, text); g_free(id); g_free(text);
    }
    g_free(sources);
    gtk_combo_box_text_append(combo, "net-new", "Ağ kaynağı…");
    if (selected && !g_str_equal(selected, "net-new")) source_select(ui, selected, selected_text);
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(combo)) < 0) combo_set(ui->source, -1);
    g_free(selected); g_free(selected_text); state->syncing = syncing;
}
static void channel_input_select(ChannelUI *ui, const gchar *id)
{
    DJState *state = ui->owner; RadioSAudioChannelInputSpec spec = {0};
    if (g_str_has_prefix(id, "in:")) {
        spec.kind = RADIOS_CHANNEL_INPUT_CAPTURE;
        g_strlcpy(spec.device, id + 3, sizeof spec.device); g_strlcpy(spec.name, spec.device, sizeof spec.name);
        RadioSAudioDevice *devices = g_new0(RadioSAudioDevice, RADIOS_AUDIO_MAX_DEVICES);
        guint count = radios_audio_ui_capture_devices(state->ctx, devices, RADIOS_AUDIO_MAX_DEVICES);
        for (guint i = 0; i < count; ++i)
            if (g_str_equal(devices[i].name, spec.device) && devices[i].description[0]) g_strlcpy(spec.name, devices[i].description, sizeof spec.name);
        g_free(devices);
    } else {
        spec.kind = RADIOS_CHANNEL_INPUT_NETWORK;
        g_strlcpy(spec.device, id + 4, sizeof spec.device); g_strlcpy(spec.name, spec.device, sizeof spec.name);
        RadioSAudioUiNetworkSource *sources = g_new0(RadioSAudioUiNetworkSource, RADIOS_AUDIO_UI_NETWORK_SOURCES);
        guint count = radios_audio_ui_network_sources(state->ctx, sources, RADIOS_AUDIO_UI_NETWORK_SOURCES);
        for (guint i = 0; i < count; ++i) if (g_str_equal(sources[i].url, spec.device)) g_strlcpy(spec.name, sources[i].name, sizeof spec.name);
        g_free(sources);
    }
    /* A long device description is cut to 128 bytes; never keep half a UTF-8 character. */
    const gchar *valid_end = NULL;
    if (!g_utf8_validate(spec.name, -1, &valid_end)) spec.name[valid_end - spec.name] = '\0';
    GError *error = NULL;
    if (!radios_audio_ui_set_channel_input(state->ctx, ui->index + 1, &spec, &error)) audio_error(state, error);
}
typedef struct { ChannelUI *ui; GtkWidget *name, *url, *error; } NetworkSourceEditor;
static void network_source_response(GtkDialog *dialog, gint response, gpointer data)
{
    NetworkSourceEditor *editor = data; ChannelUI *ui = editor->ui;
    if (response == GTK_RESPONSE_ACCEPT && !ui->disposed && !ui->owner->disposed) {
        gchar *name = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(editor->name))));
        gchar *url = g_strstrip(g_strdup(gtk_entry_get_text(GTK_ENTRY(editor->url))));
        GError *error = NULL; RadioSAudioChannelInputSpec spec = {RADIOS_CHANNEL_INPUT_NETWORK, {0}, {0}};
        gboolean accepted = g_strlcpy(spec.device, url, sizeof spec.device) < sizeof spec.device &&
            g_strlcpy(spec.name, name, sizeof spec.name) < sizeof spec.name &&
            radios_audio_ui_add_network_source(ui->owner->ctx, name, url, &error) &&
            radios_audio_ui_set_channel_input(ui->owner->ctx, ui->index + 1, &spec, &error);
        g_free(name); g_free(url);
        if (!accepted) {
            gtk_label_set_text(GTK_LABEL(editor->error), error ? error->message : "Ağ kaynağı adı 1..128, adresi 1..512 karakter olmalı.");
            g_clear_error(&error); return;
        }
    }
    gtk_widget_destroy(GTK_WIDGET(dialog));
}
static void network_source_dialog_open(ChannelUI *ui)
{
    DJState *state = ui->owner; GtkWidget *toplevel = gtk_widget_get_toplevel(ui->widget);
    GtkWindow *parent = GTK_IS_WINDOW(toplevel) ? GTK_WINDOW(toplevel) : state->ctx->parent;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Ağ kaynağı", parent, GTK_DIALOG_DESTROY_WITH_PARENT,
        "Vazgeç", GTK_RESPONSE_CANCEL, "Ekle", GTK_RESPONSE_ACCEPT, NULL);
    NetworkSourceEditor *editor = g_new0(NetworkSourceEditor, 1); editor->ui = ui;
    editor->name = radios_ui_entry("Ad"); editor->url = radios_ui_entry("URL");
    editor->error = radios_ui_label("", "dj-small-label");
    gtk_entry_set_max_length(GTK_ENTRY(editor->name), 128); gtk_entry_set_max_length(GTK_ENTRY(editor->url), 512);
    gtk_entry_set_width_chars(GTK_ENTRY(editor->url), 40);
    gtk_label_set_line_wrap(GTK_LABEL(editor->error), TRUE);
    gtk_widget_set_name(dialog, "dj-network-source-dialog");
    gtk_widget_set_name(editor->name, "dj-network-source-name"); gtk_widget_set_name(editor->url, "dj-network-source-url");
    gtk_widget_set_name(editor->error, "dj-network-source-error");
    GtkWidget *accept = gtk_dialog_get_widget_for_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
    if (accept) gtk_widget_set_name(accept, "dj-network-source-accept");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "ops-dialog");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "radios");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 420, -1); gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog)); gtk_container_set_border_width(GTK_CONTAINER(body), 18);
    GtkWidget *fields = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, "dj-channel-source");
    pack(fields, radios_ui_label("Ad", "dj-small-label"), FALSE); pack(fields, editor->name, FALSE);
    pack(fields, radios_ui_label("URL", "dj-small-label"), FALSE); pack(fields, editor->url, FALSE);
    pack(fields, editor->error, FALSE); pack(body, fields, FALSE);
    g_object_set_data_full(G_OBJECT(dialog), "dj-network-source-editor", editor, g_free);
    /* The editor points at the strip: a strip removed while the dialog is
     * open (channel sync, page teardown) takes the dialog with it. */
    g_signal_connect_object(ui->widget, "destroy", G_CALLBACK(gtk_widget_destroy), dialog, G_CONNECT_SWAPPED);
    g_signal_connect(dialog, "response", G_CALLBACK(network_source_response), editor);
    gtk_widget_show_all(dialog); gtk_widget_grab_focus(editor->name);
}
static void channel_label_changed(GtkComboBox *combo, gpointer data)
{
    ChannelUI *ui = g_object_get_data(G_OBJECT(data), "dj-channel-state"); DJState *state = ui->owner;
    if (ui->disposed || state->disposed || state->syncing || ui->master) return;
    const gchar *id = gtk_combo_box_get_active_id(combo); GError *error = NULL;
    if (!radios_audio_ui_set_channel_label(state->ctx, ui->index + 1, id && !g_str_equal(id, "none") ? id : NULL, &error)) audio_error(state, error);
}
static void channel_control_changed(GtkWidget *widget, gpointer data)
{
    GtkWidget *sender = control_widget(widget); ChannelUI *ui = g_object_get_data(G_OBJECT(data), "dj-channel-state"); DJState *state = ui->owner;
    if (ui->disposed || state->disposed || state->syncing) return;
    if (!ui->master && sender == ui->source) {
        const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(ui->source));
        if (id && g_str_equal(id, "net-new")) { network_source_dialog_open(ui); return; }
        if (id && (g_str_has_prefix(id, "in:") || g_str_has_prefix(id, "net:"))) { channel_input_select(ui, id); return; }
    }
    RadioSAudioMixerConfig config; if (!radios_audio_ui_get_mixer(state->ctx, &config, NULL, NULL)) return;
    gboolean cue_before = any_cue(&config), cue_selected = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->cue));
    if (ui->master) {
        if (sender == ui->eq[0]) config.master.eq_low_db = knob_value(ui->eq[0]);
        else if (sender == ui->eq[1]) config.master.eq_mid_db = knob_value(ui->eq[1]);
        else if (sender == ui->eq[2]) config.master.eq_high_db = knob_value(ui->eq[2]);
        else if (sender == ui->send) config.master.fx_send_db = knob_value(ui->send);
        else if (sender == ui->return_gain) config.master.fx_return_db = knob_value(ui->return_gain);
        else if (sender == ui->send_target) config.master.send_channel = combo_value(ui->send_target, -1);
        else if (sender == ui->return_source) { config.master.return_effect = combo_value(ui->return_source, -1); config.master.return_channel = -1; }
        else if (sender == ui->route_send) config.master.send_db = knob_value(ui->route_send);
        else if (sender == ui->route_return) config.master.return_db = knob_value(ui->route_return);
        else if (sender == ui->effector) config.master.fx_slot = combo_value(ui->effector, -1);
        else if (sender == ui->mute) config.master.mute = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->mute));
        else if (sender == ui->source) config.master_fx.type = (RadioSAudioFxType)combo_value(ui->source, 0);
        else if (sender == ui->depth) config.master_fx.depth = knob_value(ui->depth) / 100;
        else if (sender == ui->power) config.master_fx.enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->power));
    } else {
        RadioSAudioChannelDsp *strip = &config.channels[ui->index];
        if (sender == ui->source) strip->source_deck = combo_value(ui->source, -1);
        else if (sender == ui->effector) strip->fx_slot = combo_value(ui->effector, -1);
        else if (sender == ui->fader) strip->gain_db = gtk_range_get_value(GTK_RANGE(ui->fader));
        else if (sender == ui->pan) strip->pan = knob_value(ui->pan) / 100;
        else if (sender == ui->eq[0]) strip->eq_low_db = knob_value(ui->eq[0]);
        else if (sender == ui->eq[1]) strip->eq_mid_db = knob_value(ui->eq[1]);
        else if (sender == ui->eq[2]) strip->eq_high_db = knob_value(ui->eq[2]);
        else if (sender == ui->send) strip->fx_send_db = knob_value(ui->send);
        else if (sender == ui->return_gain) strip->return_db = knob_value(ui->return_gain);
        else if (sender == ui->send_target) strip->send_channel = combo_value(ui->send_target, -1);
        else if (sender == ui->return_source) { strip->return_effect = combo_value(ui->return_source, -1); strip->return_channel = -1; }
        else if (sender == ui->route_send) strip->send_db = knob_value(ui->route_send);
        else if (sender == ui->route_return) strip->return_db = knob_value(ui->route_return);
        else if (sender == ui->mute) strip->mute = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->mute));
        else if (sender == ui->solo) strip->solo = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->solo));
        else if (sender == ui->cue) strip->pfl = cue_selected;
        else if (sender == ui->cross_group) strip->crossfade_group = combo_value(ui->cross_group, 0);
        else if (sender == ui->normalize) strip->loudness_bypass = !gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->normalize));
    }
    GError *error = NULL; gboolean accepted = radios_audio_ui_set_mixer(state->ctx, &config, &error); audio_error(state, error);
    if (!accepted) return;
    /* A player or microphone selection ends the strip's live input. */
    if (!ui->master && sender == ui->source) radios_audio_ui_set_channel_input(state->ctx, ui->index + 1, NULL, NULL);
    gboolean cue_after = any_cue(&config);
    if ((ui->master && sender == ui->cue) || cue_before != cue_after) {
        error = NULL;
        if (!radios_audio_ui_cue_mix(state->ctx, ui->master ? cue_selected || cue_after : cue_after || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(((ChannelUI *)g_object_get_data(G_OBJECT(state->master), "dj-channel-state"))->cue)), &error)) {
            state->syncing = TRUE; gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->cue), FALSE); state->syncing = FALSE;
            if (!ui->master) { config.channels[ui->index].pfl = FALSE; radios_audio_ui_set_mixer(state->ctx, &config, NULL); }
            audio_error(state, error);
        }
    }
}
static void channel_gain_changed(GtkRange *range, gpointer data)
{
    ChannelUI *ui = g_object_get_data(G_OBJECT(data), "dj-channel-state"); if (ui->disposed || ui->owner->disposed || ui->owner->syncing) return;
    if (!ui->master) { channel_control_changed(GTK_WIDGET(range), data); return; }
    GError *error = NULL; radios_audio_ui_set_master_gain(ui->owner->ctx, gtk_range_get_value(range), &error); audio_error(ui->owner, error);
}
static void fx_control_changed(GtkWidget *widget, gpointer data)
{
    GtkWidget *sender = control_widget(widget); DJState *state = g_object_get_data(G_OBJECT(data), "dj-state");
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(data), "dj-fx-index")) - 1;
    if (!state || state->disposed || state->syncing || index >= DJ_FX_PEDALS) return;
    RadioSAudioMixerConfig config; if (!radios_audio_ui_get_mixer(state->ctx, &config, NULL, NULL)) return;
    gboolean cue_before = any_cue(&config); FxUI *ui = &state->fx[index]; RadioSAudioFx *fx = &config.effects[index];
    if (sender == ui->type) fx->type = (RadioSAudioFxType)combo_value(ui->type, 0);
    else if (sender == ui->depth) fx->depth = knob_value(ui->depth) / 100;
    else if (sender == ui->power) fx->enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->power));
    else if (sender == ui->cue) { fx->cue = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->cue)); fx->monitor_path = 1; }
    else if (sender == ui->tempo) fx->tempo_bpm = gtk_spin_button_get_value(GTK_SPIN_BUTTON(ui->tempo));
    GError *error = NULL;
    if (!radios_audio_ui_set_mixer(state->ctx, &config, &error)) { audio_error(state, error); return; }
    if (cue_before != any_cue(&config)) {
        error = NULL;
        ChannelUI *master = g_object_get_data(G_OBJECT(state->master), "dj-channel-state");
        if (!radios_audio_ui_cue_mix(state->ctx, any_cue(&config) || gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(master->cue)), &error)) {
            state->syncing = TRUE; gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->cue), FALSE); state->syncing = FALSE;
            fx->cue = FALSE; radios_audio_ui_set_mixer(state->ctx, &config, NULL); audio_error(state, error);
        }
    }
}
static void speech_changed(GtkWidget *widget, gpointer data)
{
    (void)widget; DJState *state = data; if (state->disposed || state->syncing) return;
    RadioSAudioMixerConfig config; if (!radios_audio_ui_get_mixer(state->ctx, &config, NULL, NULL)) return;
    if (widget == state->speech_level) config.speech_bed_db = gtk_range_get_value(GTK_RANGE(state->speech_level));
    else if (widget == state->speech_ducking) config.speech_ducking = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->speech_ducking));
    else if (widget == state->speech_effector) config.speech_fx_slot = combo_value(state->speech_effector, -1);
    else if (widget == state->speech_to_master) {
        config.speech_to_master = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->speech_to_master));
        if (!config.speech_to_master) config.speech_channel = combo_value(state->speech_channel, -1);
    } else if (widget == state->speech_channel) config.speech_channel = combo_value(state->speech_channel, -1);
    GError *error = NULL; radios_audio_ui_set_mixer(state->ctx, &config, &error); audio_error(state, error);
    gtk_widget_set_sensitive(state->speech_channel, !config.speech_to_master);
}
static void microphone_changed(GtkToggleButton *widget, gpointer data)
{
    DJState *state = data; if (state->disposed || state->syncing) return;
    GError *error = NULL;
    if (GTK_WIDGET(widget) == state->on_air && gtk_toggle_button_get_active(widget) &&
        !radios_audio_ui_ready(state->ctx, RADIOS_AUDIO_UI_MAIN)) {
        state->syncing = TRUE; gtk_toggle_button_set_active(widget, FALSE); state->syncing = FALSE;
        if (state->ctx->notify) state->ctx->notify(state->ctx->notify_data, "ON AIR için ana ses çıkışı hazır olmalı.");
        return;
    }
    if (!radios_audio_ui_microphone(state->ctx, gtk_toggle_button_get_active(widget), &error)) {
        state->syncing = TRUE; gtk_toggle_button_set_active(widget, FALSE); state->syncing = FALSE; audio_error(state, error);
    }
}
static gint channel_deck(ChannelUI *ui)
{
    if (ui->master) return RADIOS_AUDIO_MASTER_SAMPLER;
    RadioSAudioMixerConfig config; if (!radios_audio_ui_get_mixer(ui->owner->ctx, &config, NULL, NULL)) return -1;
    return config.channels[ui->index].source_deck;
}
static void channel_transport(GtkButton *widget, gpointer data)
{
    ChannelUI *ui = g_object_get_data(G_OBJECT(data), "dj-channel-state"); if (ui->disposed || ui->owner->disposed) return;
    gint deck = channel_deck(ui); if (deck < 0) return;
    guint action = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(widget), "dj-transport-action")); GError *error = NULL;
    if (action == 1) radios_audio_ui_play(ui->owner->ctx, (guint)deck, &error);
    else if (action == 2) radios_audio_ui_pause(ui->owner->ctx, (guint)deck, &error);
    else radios_audio_ui_stop(ui->owner->ctx, (guint)deck, &error);
    audio_error(ui->owner, error);
}
static gboolean channel_seek(GtkRange *range, GtkScrollType type, gdouble position, gpointer data)
{
    (void)range; (void)type; ChannelUI *ui = g_object_get_data(G_OBJECT(data), "dj-channel-state");
    if (ui->disposed || ui->owner->disposed || ui->owner->syncing) return TRUE;
    gint deck = channel_deck(ui); GError *error = NULL;
    if (deck >= 0) radios_audio_ui_seek(ui->owner->ctx, (guint)deck, (guint64)MAX(0, position), &error);
    audio_error(ui->owner, error); return TRUE;
}
static void channel_media_picked(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail, gpointer data)
{
    (void)data; ChannelUI *ui = g_object_get_data(G_OBJECT(owner), "dj-channel-state"); if (!ui || ui->disposed || ui->owner->disposed) return;
    gint deck = channel_deck(ui); RadioSAudioMixerConfig config;
    if (!ui->master && deck < 0) {
        if (!radios_audio_ui_get_mixer(ctx, &config, NULL, NULL)) return;
        deck = (gint)ui->index; config.channels[ui->index].source_deck = deck; config.channels[ui->index].mute = FALSE;
        radios_audio_ui_set_mixer(ctx, &config, NULL);
    }
    GError *error = NULL; GtkWidget *play_owner = ui->master ? owner : GTK_WIDGET(ctx->parent);
    if (!radios_audio_ui_load_media(ctx, play_owner, detail, (guint)deck, FALSE, &error)) { audio_error(ui->owner, error); return; }
    if (!ui->master) {
        radios_broadcast_player_library_loaded(ctx, (guint)deck);
        gchar *key = g_strdup_printf("radios-deck-owner-%d", deck); GtkWidget *card = g_object_get_data(G_OBJECT(ctx->parent), key); g_free(key);
        if (card) {
            JsonNode *snapshot = json_node_new(JSON_NODE_OBJECT);
            json_node_set_object(snapshot, detail);
            g_object_set_data_full(G_OBJECT(card), "radios-deck-media-detail", json_node_copy(snapshot), (GDestroyNotify)json_node_free);
            json_node_free(snapshot);
        }
        GtkWidget *title = card ? g_object_get_data(G_OBJECT(card), "radios-deck-title-widget") : NULL;
        if (title) gtk_label_set_text(GTK_LABEL(title), radios_json_string(json_object_get_object_member(detail, "media"), "title", "Medya"));
        GtkWidget *main = card ? g_object_get_data(G_OBJECT(card), "radios-deck-main-widget") : NULL;
        radios_audio_ui_set_deck_main(ctx, (guint)deck, main ? gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(main)) : TRUE, NULL);
    }
}
static void channel_load(GtkButton *widget, gpointer data)
{
    (void)widget; ChannelUI *ui = g_object_get_data(G_OBJECT(data), "dj-channel-state");
    if (!ui->disposed && !ui->owner->disposed) radios_media_library_pick(ui->owner->ctx, ui->widget, channel_media_picked, NULL);
}
static void channel_sync(ChannelUI *ui, const RadioSAudioMixerConfig *config, const RadioSAudioDspMeters *meters, gdouble master_gain, gboolean normalization_enabled)
{
    if (ui->disposed) return;
    const RadioSAudioChannelDsp *strip = &config->channels[ui->index];
    gdouble low = ui->master ? config->master.eq_low_db : strip->eq_low_db, mid = ui->master ? config->master.eq_mid_db : strip->eq_mid_db;
    gdouble high = ui->master ? config->master.eq_high_db : strip->eq_high_db;
    knob_set(ui->eq[0], low); knob_set(ui->eq[1], mid); knob_set(ui->eq[2], high);
    knob_set(ui->send, ui->master ? config->master.fx_send_db : strip->fx_send_db); knob_set(ui->return_gain, ui->master ? config->master.fx_return_db : strip->return_db);
    gtk_range_set_value(GTK_RANGE(ui->fader), ui->master ? master_gain : strip->gain_db);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->mute), ui->master ? config->master.mute : strip->mute);
    if (ui->master) {
        combo_set(ui->effector, config->master.fx_slot);
        combo_set(ui->source, config->master_fx.type); knob_set(ui->depth, config->master_fx.depth * 100);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->power), config->master_fx.enabled);
    } else {
        RadioSAudioChannelInputSpec input;
        if (radios_audio_ui_get_channel_input(ui->owner->ctx, ui->index + 1, &input)) {
            gchar *id = g_strconcat(input.kind == RADIOS_CHANNEL_INPUT_CAPTURE ? "in:" : "net:", input.device, NULL);
            gchar *text = g_strdup_printf("%s: %s", input.kind == RADIOS_CHANNEL_INPUT_CAPTURE ? "Giriş" : "Ağ", input.name);
            source_select(ui, id, text); g_free(id); g_free(text);
        } else combo_set(ui->source, strip->source_deck);
        if (ui->label) {
            const gchar *label = radios_audio_ui_get_channel_label(ui->owner->ctx, ui->index + 1);
            gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui->label), label ? label : "none");
        }
        combo_set(ui->effector, strip->fx_slot); knob_set(ui->pan, strip->pan * 100);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->cue), strip->pfl); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->solo), strip->solo); combo_set(ui->cross_group, strip->crossfade_group);
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->normalize), !strip->loudness_bypass);
        /* Keep the channel preference usable before a player is assigned.
         * The adjacent status distinguishes this preference from application. */
        gtk_widget_set_sensitive(ui->normalize, TRUE);
    }
    combo_set(ui->send_target, ui->master ? config->master.send_channel : strip->send_channel);
    gint return_effect = ui->master ? config->master.return_effect : strip->return_effect;
    if (return_effect == -2) return_effect = ui->master ? config->master.fx_slot : strip->fx_slot;
    combo_set(ui->return_source, return_effect);
    knob_set(ui->route_send, ui->master ? config->master.send_db : strip->send_db);
    knob_set(ui->route_return, ui->master ? config->master.return_db : strip->return_db);
    if (!ui->master) {
        const gfloat *peak = meters->channel_peak[ui->index]; radios_meter_set_levels(ui->meter, peak[0], peak[1], 0, 0, peak[0] >= 1 || peak[1] >= 1);
    }
    gint deck = ui->master ? (gint)RADIOS_AUDIO_MASTER_SAMPLER : strip->source_deck; RadioSAudioUiSnapshot snapshot = {0};
    gboolean valid = deck >= 0 && radios_audio_ui_get_snapshot(ui->owner->ctx, (guint)deck, &snapshot);
    gboolean loaded = valid && snapshot.media_id[0] && snapshot.state != RADIOS_AUDIO_UI_EMPTY && snapshot.state != RADIOS_AUDIO_UI_LOADING && snapshot.state != RADIOS_AUDIO_UI_ERROR;
    if (!ui->master) {
        gboolean applying = normalization_enabled && !strip->loudness_bypass && meters->channel_loudness_active[ui->index];
        const gchar *reason = strip->loudness_bypass ? "Kapalı" : !normalization_enabled ? "Genel kapalı" :
            deck == -1 ? "Kaynak yok" : applying ? NULL : loaded && !snapshot.loudness_measured ? "Ölçüm yok" :
            loaded && snapshot.loudness_measured ? "Kapsam dışında" : "Bekliyor";
        gchar *normalization = applying ? g_strdup_printf("%+.1f dB", meters->channel_loudness_gain_db[ui->index]) : g_strdup(reason);
        gtk_label_set_text(GTK_LABEL(ui->normalization), normalization);
        gtk_widget_set_tooltip_text(ui->normalization, applying ? "Uygulanan toplam otomatik ses eşitlemesi." : reason);
        g_free(normalization);
        GtkStyleContext *style = gtk_widget_get_style_context(ui->normalize);
        if (!normalization_enabled) gtk_style_context_add_class(style, "is-suspended");
        else gtk_style_context_remove_class(style, "is-suspended");
        if (applying) gtk_style_context_add_class(style, "is-applying");
        else gtk_style_context_remove_class(style, "is-applying");
    }
    RadioSAudioChannelInputSpec input = {0}; RadioSAudioChannelInputStatus input_status = {0};
    gboolean has_input = !ui->master && radios_audio_ui_get_channel_input(ui->owner->ctx, ui->index + 1, &input);
    gboolean input_live = has_input && radios_audio_ui_channel_input_status(ui->owner->ctx, ui->index + 1, &input_status);
    gtk_widget_set_sensitive(ui->play, loaded && snapshot.output_ready); gtk_widget_set_sensitive(ui->pause, valid && snapshot.state == RADIOS_AUDIO_UI_PLAYING);
    gtk_widget_set_sensitive(ui->stop, valid && snapshot.media_id[0]); gtk_widget_set_sensitive(ui->seek, loaded); gtk_widget_set_sensitive(ui->load, deck != -2 && !has_input);
    gtk_range_set_range(GTK_RANGE(ui->seek), 0, MAX(1, snapshot.duration_ms)); gtk_range_set_value(GTK_RANGE(ui->seek), snapshot.position_ms);
    const gchar *states[] = {"Boş", "Yükleniyor", "Hazır", "Çalıyor", "Duraklatıldı", "Bitti", "Ses hatası"};
    gchar *position;
    if (valid) position = g_strdup_printf("%s · %02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT " / %02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT,
        states[snapshot.state], snapshot.position_ms / 60000, (snapshot.position_ms / 1000) % 60, snapshot.duration_ms / 60000, (snapshot.duration_ms / 1000) % 60);
    else if (has_input) {
        /* Short live-input status line: kind, name and connection state. */
        const gchar *kind = input.kind == RADIOS_CHANNEL_INPUT_CAPTURE ? "Giriş" : "Ağ";
        const gchar *state = !input_live ? "Kapalı" : input_status.state == RADIOS_CHANNEL_INPUT_READY ? "Hazır" :
            input_status.state == RADIOS_CHANNEL_INPUT_CONNECTING ? "Bağlanıyor" :
            input_status.state != RADIOS_CHANNEL_INPUT_ERROR ? "Kapalı" :
            g_str_equal(input_status.error, RADIOS_CHANNEL_INPUT_NO_DATA_MESSAGE) ? "Veri gelmiyor" : "Yeniden denenecek";
        position = g_strdup_printf("%s · %s · %s", kind, input.name, state);
        gtk_widget_set_tooltip_text(ui->position, input_live && input_status.error[0] ? input_status.error : position);
    } else position = g_strdup(deck == -2 ? "Stüdyo mikrofonu" : "Kaynak yok");
    gtk_label_set_text(GTK_LABEL(ui->position), position); g_free(position);
    if (snapshot.error[0]) gtk_widget_set_tooltip_text(ui->position, snapshot.error);
}
static gboolean audio_tick(gpointer data)
{
    DJState *state = data; if (state->disposed) return G_SOURCE_REMOVE;
    gtk_widget_set_sensitive(state->mixer, radios_audio_ui_mixer_ready(state->ctx));
    RadioSAudioMixerConfig config; RadioSAudioDspMeters meters; gdouble master_gain = 0;
    if (!radios_audio_ui_get_mixer(state->ctx, &config, &meters, &master_gain)) return G_SOURCE_CONTINUE;
    gboolean normalization_enabled = FALSE;
    radios_audio_ui_loudness(state->ctx, &normalization_enabled, NULL);
    state->syncing = TRUE;
    GList *channels = gtk_container_get_children(GTK_CONTAINER(state->channels));
    /* A changed device list or network source list refills every source menu
     * while the current selection is kept. */
    guint source_hash = radios_audio_ui_sources_hash(state->ctx); gboolean refill = source_hash != state->source_hash;
    state->source_hash = source_hash;
    for (GList *item = channels; item; item = item->next) {
        ChannelUI *ui = g_object_get_data(G_OBJECT(item->data), "dj-channel-state");
        gtk_widget_set_sensitive(ui->name, !state->ctx->data || radios_data_document_ready(state->channel_document));
        if (refill) source_options_fill(state, ui);
        channel_sync(ui, &config, &meters, master_gain, normalization_enabled);
    }
    ChannelUI *master = g_object_get_data(G_OBJECT(state->master), "dj-channel-state");
    gtk_widget_set_sensitive(master->name, !state->ctx->data || radios_data_document_ready(state->channel_document));
    g_list_free(channels); channel_sync(g_object_get_data(G_OBJECT(state->master), "dj-channel-state"), &config, &meters, master_gain, normalization_enabled);
    for (guint i = 0; i < DJ_FX_PEDALS; ++i) {
        FxUI *ui = &state->fx[i]; const RadioSAudioFx *fx = &config.effects[i];
        fx_display_update(ui, fx); combo_set(ui->type, fx->type);
        knob_set(ui->depth, fx->depth * 100);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(ui->tempo), fx->tempo_bpm); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->power), fx->enabled); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->cue), fx->cue);
        GString *names = g_string_new(NULL); channels = gtk_container_get_children(GTK_CONTAINER(state->channels));
        for (GList *item = channels; item; item = item->next) {
            ChannelUI *strip = g_object_get_data(G_OBJECT(item->data), "dj-channel-state");
            if (config.channels[strip->index].fx_slot == (gint)i && (meters.channel_fx_send_peak[strip->index][0] > 0.000001 || meters.channel_fx_send_peak[strip->index][1] > 0.000001)) {
                if (names->len) g_string_append(names, ", ");
                g_string_append(names, g_object_get_data(G_OBJECT(strip->widget), "dj-channel-name"));
            }
        }
        g_list_free(channels);
        if (config.master.fx_slot == (gint)i && (meters.master_fx_send_peak[0] > 0.000001 || meters.master_fx_send_peak[1] > 0.000001)) {
            if (names->len) g_string_append(names, ", ");
            g_string_append(names, g_object_get_data(G_OBJECT(state->master), "dj-channel-name"));
        }
        if (config.speech_fx_slot == (gint)i && (meters.speech_fx_send_peak[0] > 0.000001 || meters.speech_fx_send_peak[1] > 0.000001)) {
            if (names->len) g_string_append(names, ", ");
            g_string_append(names, "Mikrofon");
        }
        gtk_label_set_text(GTK_LABEL(state->fx_incoming[i]), names->len ? names->str : "Gönderim yok");
        gtk_widget_set_tooltip_text(state->fx_incoming[i], names->len ? names->str : "Gönderim yok"); g_string_free(names, TRUE);
    }
    gtk_range_set_value(GTK_RANGE(state->crossfader), config.crossfader); gtk_range_set_value(GTK_RANGE(state->speech_level), config.speech_bed_db);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->speech_ducking), config.speech_ducking);
    combo_set(state->speech_effector, config.speech_fx_slot);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->speech_to_master), config.speech_to_master);
    combo_set(state->speech_channel, config.speech_channel);
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(state->speech_channel)) < 0) combo_set(state->speech_channel, -1);
    gtk_widget_set_sensitive(state->speech_channel, !config.speech_to_master);
    gboolean microphone = radios_audio_ui_microphone_active(state->ctx); gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->microphone_gate), microphone);
    gtk_widget_set_sensitive(state->microphone_gate, radios_audio_ui_ready(state->ctx, RADIOS_AUDIO_UI_MICROPHONE));
    gboolean main_ready = radios_audio_ui_ready(state->ctx, RADIOS_AUDIO_UI_MAIN);
    gboolean microphone_ready = radios_audio_ui_ready(state->ctx, RADIOS_AUDIO_UI_MICROPHONE);
    /* Opening the gate flushes old PCM, so MAIN briefly reports not ready.
     * A held button must remain usable for release during that flush. The
     * engine closes the microphone gate on an actual MAIN or MIC loss. */
    if (state->talking && !microphone) talk_release(state);
    if (state->push_to_talk)
        gtk_widget_set_sensitive(state->push_to_talk, state->talking || (main_ready && microphone_ready && !microphone));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->on_air), microphone);
    gtk_widget_set_sensitive(state->on_air, microphone || (main_ready && radios_audio_ui_ready(state->ctx, RADIOS_AUDIO_UI_MICROPHONE)));
    gtk_button_set_label(GTK_BUTTON(state->microphone_gate), microphone ? "Mikrofon açık" : "Mikrofon kapalı");
    GtkWidget *speech_target = config.speech_to_master ? state->master : config.speech_channel >= 0 ?
        channel_find(state, (guint)config.speech_channel + 1) : NULL;
    gchar *destination = g_strdup(speech_target ? g_object_get_data(G_OBJECT(speech_target), "dj-channel-name") : "Kanal seçilmedi");
    gchar *speech = g_strdup_printf("Mikrofon %s · %s · Müzik yatağı %.1f dB · Yerel ses çıkışı", microphone ? "açık" : "kapalı", destination, 20 * log10(MAX(meters.speech_gain, 0.000001)));
    g_free(destination);
    gtk_label_set_text(GTK_LABEL(state->speech_status), speech); g_free(speech);
    RadioSAudioUiSnapshot sampler_snapshots[33] = {{0}}; gboolean sampled[33] = {FALSE};
    for (guint i = 0; i < state->pads->len; ++i) {
        PadState *pad = g_ptr_array_index(state->pads, i); if (pad->disposed || pad->pending) continue;
        if (!pad->media_id) continue;
        guint deck = pad->channel_number ? RADIOS_AUDIO_SAMPLER_BASE + pad->channel_number - 1 : RADIOS_AUDIO_MASTER_SAMPLER;
        if (!sampled[pad->channel_number]) { radios_audio_ui_get_snapshot(state->ctx, deck, &sampler_snapshots[pad->channel_number]); sampled[pad->channel_number] = TRUE; }
        RadioSAudioUiSnapshot *snapshot = &sampler_snapshots[pad->channel_number];
        gboolean same = snapshot->generation == pad->generation && g_str_equal(snapshot->media_id, pad->media_id);
        gboolean active = same && snapshot->state == RADIOS_AUDIO_UI_PLAYING;
        gboolean loading = same && (snapshot->state == RADIOS_AUDIO_UI_LOADING || snapshot->state == RADIOS_AUDIO_UI_READY);
        if (pad->selected != active || pad->loading != loading) { pad->selected = active; pad->loading = loading; pad_render(pad); }
    }
    state->syncing = FALSE; return G_SOURCE_CONTINUE;
}

GtkWidget *
radios_dj_new(RadioSViewContext *ctx)
{
    DJState *state = g_new0(DJState, 1);
    GtkWidget *page = radios_ui_box(GTK_ORIENTATION_VERTICAL, 0, "dj-view task-view");
    GtkWidget *mixer = radios_ui_box(GTK_ORIENTATION_VERTICAL, 0, "dj-mixer");
    GtkWidget *mixer_body = radios_ui_box(GTK_ORIENTATION_VERTICAL, 0, "dj-mixer-body");
    GtkWidget *toolbar = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 10, "dj-mixer-toolbar");
    GtkWidget *handle = gtk_event_box_new();
    GtkWidget *tools = radios_ui_box(GTK_ORIENTATION_VERTICAL, 12, "dj-performance-tools quick-tools");
    GtkWidget *effects_tools = radios_ui_box(GTK_ORIENTATION_VERTICAL, 0, "dj-performance-tools quick-tools");
    GtkWidget *rack = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 10, "dj-mixer-rack");
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    GtkWidget *add = radios_ui_button("+ Kanal ekle");
    gtk_widget_set_name(add, "dj-add-channel");
    state->ctx = ctx;
    state->page = page; state->mixer = mixer; state->pads = g_ptr_array_new();
    state->mixer_scale = radios_content_scale_new();
    state->effector_scale = radios_content_scale_new();
    radios_content_scale_set_propagate_height(state->mixer_scale, TRUE);
    radios_content_scale_set_propagate_height(state->effector_scale, TRUE);
    /* Mixer/effector sizes zoom their own 100% layout: smaller never stretches
     * the strips apart, larger never squeezes them under their minimum. */
    radios_content_scale_set_section(state->mixer_scale, TRUE);
    radios_content_scale_set_section(state->effector_scale, TRUE);
    gtk_widget_set_name(state->mixer_scale, "dj-mixer-scale");
    gtk_widget_set_name(state->effector_scale, "dj-effector-scale");
    gtk_container_add(GTK_CONTAINER(state->mixer_scale), mixer_body);
    gtk_container_add(GTK_CONTAINER(state->effector_scale), effects_tools);
    if (ctx->audio) gtk_widget_set_sensitive(mixer, radios_audio_ui_mixer_ready(ctx));
    pedal_theme_load(state);
    state->count = radios_ui_label("", "dj-muted output");
    state->channels = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 5, "dj-mixer-channels");
    gtk_widget_set_name(page, "dj-task-view");
    gtk_widget_set_name(mixer, "dj-mixer");
    gtk_widget_set_margin_start(page, 28);
    gtk_widget_set_margin_end(page, 28);
    gtk_widget_set_margin_top(page, 4);
    gtk_widget_set_name(handle, "dj-mixer-drag-handle");
    gtk_event_box_set_visible_window(GTK_EVENT_BOX(handle), TRUE);
    gtk_widget_add_events(handle, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK);
    gtk_container_add(GTK_CONTAINER(handle), radios_ui_label("⠿  Mixer", "dj-title heading"));
    gtk_widget_set_tooltip_text(handle, "Mixer'i ayrı pencerede taşımak için buradan sürükle.");
    g_signal_connect(handle, "realize", G_CALLBACK(mixer_handle_realized), state);
    g_signal_connect(handle, "button-press-event", G_CALLBACK(mixer_handle_pressed), state);
    g_signal_connect(handle, "motion-notify-event", G_CALLBACK(mixer_handle_motion), state);
    g_signal_connect(handle, "button-release-event", G_CALLBACK(mixer_handle_released), state);
    pack(toolbar, handle, TRUE);
    state->mixer_window_toggle = radios_ui_button("Pencerede aç");
    gtk_widget_set_name(state->mixer_window_toggle, "dj-mixer-window-toggle");
    gtk_widget_set_tooltip_text(state->mixer_window_toggle, "Mixer'i ayrı ve taşınabilir bir pencerede aç.");
    pack(toolbar, state->mixer_window_toggle, FALSE);
    g_signal_connect(state->mixer_window_toggle, "clicked", G_CALLBACK(mixer_window_toggle_clicked), state);
    pack(toolbar, state->count, FALSE);
    if (ctx->audio) {
        pack(toolbar, radios_ui_label("Otomatik kayıt", "dj-muted"), FALSE);
        state->mixer_reload = radios_ui_button("Yeniden yükle");
        gtk_widget_set_name(state->mixer_reload, "dj-mixer-reload");
        pack(toolbar, state->mixer_reload, FALSE);
        g_signal_connect(state->mixer_reload, "clicked", G_CALLBACK(mixer_reload_clicked), state);
    }
    pack(toolbar, add, FALSE);
    g_signal_connect(add, "clicked", G_CALLBACK(add_channel), state);
    pack(mixer, toolbar, FALSE);
    pack(effects_tools, effects_new(state), FALSE);
    pack(mixer, state->effector_scale, FALSE);
    pack(tools, speech_new(state), FALSE);
    pack(mixer_body, tools, FALSE);
    for (guint i = 0; i < 8; ++i) {
        gchar *name = i < 6 ? g_strdup_printf("Player %u", i + 1) : ctx->audio ? g_strdup_printf("Kanal %u", i + 1) : g_strdup(i == 6 ? "Mikrofon" : "Jingle");
        pack(state->channels, channel_new(state, i + 1, name, FALSE), FALSE);
        g_free(name);
    }
    state->channel_count = state->next_channel = 8;
    update_count(state);
    pack(rack, state->channels, TRUE);
    state->master = channel_new(state, 0, "MASTER", TRUE);
    pack(rack, state->master, FALSE);
    channel_routes_update(state);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_container_add(GTK_CONTAINER(scroll), rack);
    state->rack_view = gtk_bin_get_child(GTK_BIN(scroll));
    g_signal_connect(state->rack_view, "size-allocate", G_CALLBACK(rack_view_allocated), state);
    pack(mixer_body, scroll, FALSE);
    if (ctx->audio) {
        GtkWidget *cross = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 10, "dj-channel-volume");
        state->crossfader = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, -1, 1, 0.01); gtk_scale_set_digits(GTK_SCALE(state->crossfader), 2);
        gtk_widget_set_name(state->crossfader, "dj-crossfader"); gtk_widget_set_hexpand(state->crossfader, TRUE);
        pack(cross, radios_ui_label("A", "dj-small-label"), FALSE); pack(cross, state->crossfader, TRUE); pack(cross, radios_ui_label("B", "dj-small-label"), FALSE); pack(mixer_body, cross, FALSE);
        g_signal_connect(state->crossfader, "value-changed", G_CALLBACK(crossfader_changed), state);
    }
    pack(mixer, state->mixer_scale, FALSE);
    state->mixer_dock = radios_ui_box(GTK_ORIENTATION_VERTICAL, 0, "dj-mixer-dock");
    gtk_widget_set_name(state->mixer_dock, "dj-mixer-dock");
    state->mixer_placeholder = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 10, "dj-mixer-toolbar");
    gtk_widget_set_name(state->mixer_placeholder, "dj-mixer-detached-placeholder");
    GtkWidget *show = radios_ui_button("Pencereyi göster");
    GtkWidget *restore = radios_ui_button("Programa geri yerleştir");
    gtk_widget_set_name(show, "dj-mixer-window-show");
    gtk_widget_set_name(restore, "dj-mixer-window-restore");
    pack(state->mixer_placeholder, radios_ui_label("Mixer ayrı pencerede", "dj-muted"), TRUE);
    pack(state->mixer_placeholder, show, FALSE);
    pack(state->mixer_placeholder, restore, FALSE);
    g_signal_connect(show, "clicked", G_CALLBACK(mixer_window_show_clicked), state);
    g_signal_connect(restore, "clicked", G_CALLBACK(mixer_window_restore_clicked), state);
    pack(state->mixer_dock, state->mixer_placeholder, FALSE);
    gtk_widget_show_all(state->mixer_placeholder);
    gtk_widget_set_no_show_all(state->mixer_placeholder, TRUE);
    gtk_widget_hide(state->mixer_placeholder);
    pack(state->mixer_dock, mixer, FALSE);
    pack(page, state->mixer_dock, FALSE);
    g_object_set_data_full(G_OBJECT(page), "dj-state", state, dj_free);
    RadioSUIPreferences preferences;
    radios_ui_preferences_default(&preferences);
    if (ctx->config && ctx->config->project_root)
        radios_ui_preferences_load(ctx->config->project_root, &preferences, NULL);
    radios_dj_set_scale(page, preferences.mixer_scale_percent, preferences.mixer_height_percent,
                        preferences.effector_scale_percent, preferences.effector_height_percent);
    g_object_set_data(G_OBJECT(page), "dj-channel-box", state->channels);
    g_object_set_data(G_OBJECT(page), "dj-add-button", add);
    g_signal_connect(page, "destroy", G_CALLBACK(dj_destroyed), state);
    if (state->theme_error) state->theme_notice = g_idle_add(pedal_theme_notice, state);
    state->channel_document = radios_data_document_new(ctx, page, "channel", "DJ kanalları", channels_apply, state);
    state->sampler = radios_data_document_new(ctx, page, "sampler", "DJ sampler", sampler_apply, state);
    if (ctx->audio) { state->audio_timer = g_timeout_add(50, audio_tick, state); audio_tick(state); }
    return page;
}

static void sampler_save(DJState *state)
{
    if (!state->sampler || state->loading || state->disposed) return;
    JsonNode *config = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new(); JsonArray *pads = json_array_new();
    json_node_take_object(config, object); json_object_set_array_member(object, "pads", pads);
    for (guint i = 0; i < state->pads->len; ++i) {
        PadState *pad = g_ptr_array_index(state->pads, i); if (!pad->title) continue;
        JsonObject *record = json_object_new(); json_object_set_string_member(record, "key", pad->key);
        json_object_set_string_member(record, "title", pad->title); json_object_set_string_member(record, "file", pad->file ? pad->file : "");
        if (pad->media_id) {
            json_object_set_string_member(record, "media_id", pad->media_id); json_object_set_string_member(record, "version_id", pad->version_id);
            json_object_set_string_member(record, "media_file_id", pad->media_file_id); json_object_set_string_member(record, "media_sha256", pad->media_sha256);
        }
        json_array_add_object_element(pads, record);
    }
    radios_data_document_dirty(state->sampler, TRUE);
    radios_data_document_save(state->sampler, config); json_node_free(config);
}
static void sampler_apply(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *config, gpointer data)
{
    (void)ctx; (void)owner; DJState *state = data; JsonArray *pads = json_object_get_array_member(config, "pads");
    guint highest = 0;
    for (guint i = 0; pads && i < json_array_get_length(pads); ++i) {
        JsonObject *record = json_array_get_object_element(pads, i); guint channel = 0, number = 0;
        if (sscanf(radios_json_string(record, "key", ""), "channel-%u-pad-%u", &channel, &number) == 2 &&
            channel <= 32 && number >= 1 && number <= 4) highest = MAX(highest, channel);
    }
    if (highest && !state->ctx->audio) channels_ensure(state, highest);
    if (state->ctx->audio) {
        gboolean loading = state->loading; state->loading = TRUE;
        for (guint i = 0; pads && i < json_array_get_length(pads); ++i) {
            JsonObject *record = json_array_get_object_element(pads, i); guint channel = 0, number = 0;
            if (sscanf(radios_json_string(record, "key", ""), "channel-%u-pad-%u", &channel, &number) == 2 && channel >= 1 && channel <= 32 && !channel_find(state, channel)) {
                gchar *name = g_strdup_printf("Kanal %u", channel); GtkWidget *widget = channel_new(state, channel, name, FALSE); g_free(name);
                pack(state->channels, widget, FALSE); gtk_widget_show_all(widget); ++state->channel_count;
            }
        }
        state->loading = loading; update_count(state);
    }
    for (guint i = 0; i < state->pads->len; ++i) {
        PadState *pad = g_ptr_array_index(state->pads, i); JsonObject *match = NULL;
        for (guint j = 0; pads && j < json_array_get_length(pads); ++j) {
            JsonObject *record = json_array_get_object_element(pads, j);
            if (g_str_equal(pad->key, radios_json_string(record, "key", ""))) { match = record; break; }
        }
        gboolean changed = g_strcmp0(pad->media_id, radios_json_string(match, "media_id", NULL)) ||
            g_strcmp0(pad->version_id, radios_json_string(match, "version_id", NULL)) ||
            g_strcmp0(pad->media_file_id, radios_json_string(match, "media_file_id", NULL)) ||
            g_strcmp0(pad->media_sha256, radios_json_string(match, "media_sha256", NULL));
        if (changed) {
            ++pad->play_epoch;
            if (state->ctx->audio && (pad->pending || pad_voice_owned(pad)))
                radios_audio_ui_stop(state->ctx, pad->channel_number ? RADIOS_AUDIO_SAMPLER_BASE + pad->channel_number - 1 : RADIOS_AUDIO_MASTER_SAMPLER, NULL);
            pad->selected = FALSE; pad->pending = FALSE; pad->loading = FALSE; pad->generation = 0;
        }
        g_clear_pointer(&pad->title, g_free); g_clear_pointer(&pad->file, g_free); pad_binding_clear(pad);
        if (match) {
            pad->title = g_strdup(radios_json_string(match, "title", "")); pad->file = g_strdup(radios_json_string(match, "file", ""));
            pad->media_id = g_strdup(radios_json_string(match, "media_id", NULL)); pad->version_id = g_strdup(radios_json_string(match, "version_id", NULL));
            pad->media_file_id = g_strdup(radios_json_string(match, "media_file_id", NULL)); pad->media_sha256 = g_strdup(radios_json_string(match, "media_sha256", NULL));
        }
        if (!state->ctx->audio) pad->selected = FALSE;
        pad_render(pad);
    }
}
