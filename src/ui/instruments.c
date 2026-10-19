#include "radios/views.h"
#include "radios/widgets.h"
#include "radios/audio_ui.h"
#include "radios/platform.h"

#include <math.h>

#define RADIOS_PI 3.14159265358979323846

typedef struct {
    GtkAdjustment *adjustment;
    GtkWidget *drawing;
    GtkWidget *value;
    gulong changed_handler;
    double initial;
    double drag_y;
    double drag_value;
    gboolean dragging;
    gboolean disposed;
    gboolean percentage;
    gboolean pan;
} KnobState;

typedef struct {
    guint timer;
} ClockState;

typedef struct {
    gboolean vertical, measured, clipped;
    gdouble peak[2], rms[2];
} MeterState;

static void
rgb(cairo_t *cr, guint color)
{
    cairo_set_source_rgb(cr, ((color >> 16) & 255) / 255.0,
                         ((color >> 8) & 255) / 255.0, (color & 255) / 255.0);
}

static void
knob_changed(GtkAdjustment *adjustment, gpointer data)
{
    KnobState *state = data;
    double value = gtk_adjustment_get_value(adjustment);
    gchar *text;
    if (state->disposed)
        return;
    if (state->pan)
        text = fabs(value) < 0.001 ? g_strdup("Merkez") :
            g_strdup_printf("%.0f %s", fabs(value), value < 0 ? "L" : "R");
    else if (state->percentage)
        text = g_strdup_printf("%.0f %%", value);
    else if (fabs(value - round(value)) < 0.001)
        text = g_strdup_printf("%.0f dB", value);
    else
        text = g_strdup_printf("%.1f dB", value);
    gtk_label_set_text(GTK_LABEL(state->value), text);
    g_free(text);
    gtk_widget_queue_draw(state->drawing);
}

static gboolean
knob_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    KnobState *state = data;
    double size = MIN(gtk_widget_get_allocated_width(widget),
                      gtk_widget_get_allocated_height(widget));
    double cx = gtk_widget_get_allocated_width(widget) / 2.0;
    double cy = gtk_widget_get_allocated_height(widget) / 2.0;
    double radius = size / 2.0 - 2;
    double low = gtk_adjustment_get_lower(state->adjustment);
    double high = gtk_adjustment_get_upper(state->adjustment);
    double value = gtk_adjustment_get_value(state->adjustment);
    double position = high > low ? (value - low) / (high - low) : 0.5;
    if (low < 0 && high > 0)
        position = 0.5 + 0.5 * value / (value < 0 ? -low : high);
    double angle = (-225 + position * 270) * RADIOS_PI / 180;
    const gint ticks[] = { 0, 2, 5, 8, 10 };
    cairo_set_line_width(cr, 1);
    for (guint i = 0; i < G_N_ELEMENTS(ticks); ++i) {
        double tick = (-225 + ticks[i] * 27) * RADIOS_PI / 180;
        rgb(cr, ticks[i] == 5 ? 0xb6c6d4 : 0x4c5865);
        cairo_move_to(cr, cx + cos(tick) * radius, cy + sin(tick) * radius);
        cairo_line_to(cr, cx + cos(tick) * (radius - 3), cy + sin(tick) * (radius - 3));
        cairo_stroke(cr);
    }
    rgb(cr, 0x343c45);
    cairo_arc(cr, cx, cy, radius - 4, 0, 2 * RADIOS_PI);
    cairo_stroke(cr);
    rgb(cr, 0x353c44);
    cairo_arc(cr, cx, cy, radius - 7, 0, 2 * RADIOS_PI);
    cairo_fill_preserve(cr);
    rgb(cr, 0x5b6571);
    cairo_stroke(cr);
    rgb(cr, 0xd9e2eb);
    cairo_set_line_width(cr, 1.5);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, cx + cos(angle) * (radius - 10), cy + sin(angle) * (radius - 10));
    cairo_line_to(cr, cx + cos(angle) * (radius - 15), cy + sin(angle) * (radius - 15));
    cairo_stroke(cr);
    if (gtk_widget_has_focus(widget)) {
        rgb(cr, 0xb6c6d4);
        cairo_set_line_width(cr, 1);
        cairo_arc(cr, cx, cy, radius, 0, 2 * RADIOS_PI);
        cairo_stroke(cr);
    }
    return FALSE;
}

static void
knob_set(KnobState *state, double value)
{
    double step = gtk_adjustment_get_step_increment(state->adjustment);
    double low = gtk_adjustment_get_lower(state->adjustment);
    double high = gtk_adjustment_get_upper(state->adjustment);
    if (step > 0)
        value = low + round((value - low) / step) * step;
    gtk_adjustment_set_value(state->adjustment, CLAMP(value, low, high));
}

static gboolean
knob_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    KnobState *state = data;
    if (event->button != 1)
        return FALSE;
    gtk_widget_grab_focus(widget);
    if (event->type == GDK_2BUTTON_PRESS) {
        knob_set(state, state->initial);
        state->dragging = FALSE;
    } else {
        state->dragging = TRUE;
        state->drag_y = event->y_root;
        state->drag_value = gtk_adjustment_get_value(state->adjustment);
        gtk_grab_add(widget);
    }
    return TRUE;
}

static gboolean
knob_release(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    KnobState *state = data;
    if (event->button != 1)
        return FALSE;
    state->dragging = FALSE;
    if (gtk_widget_has_grab(widget))
        gtk_grab_remove(widget);
    return TRUE;
}

static gboolean
knob_motion(GtkWidget *widget, GdkEventMotion *event, gpointer data)
{
    KnobState *state = data;
    if (!state->dragging)
        return FALSE;
    /* The release can be lost (Alt+Tab, a focus-stealing dialog or a lost
     * mouse capture during the drag): without the button the drag is over. */
    if (!(event->state & GDK_BUTTON1_MASK)) {
        state->dragging = FALSE;
        if (gtk_widget_has_grab(widget))
            gtk_grab_remove(widget);
        return FALSE;
    }
    knob_set(state, state->drag_value + (state->drag_y - event->y_root) *
             gtk_adjustment_get_step_increment(state->adjustment));
    return TRUE;
}

static gboolean
knob_scroll(GtkWidget *widget, GdkEventScroll *event, gpointer data)
{
    KnobState *state = data;
    double delta = 0;
    (void)widget;
    if (event->direction == GDK_SCROLL_UP || event->direction == GDK_SCROLL_RIGHT)
        delta = 1;
    else if (event->direction == GDK_SCROLL_DOWN || event->direction == GDK_SCROLL_LEFT)
        delta = -1;
    else if (event->direction == GDK_SCROLL_SMOOTH)
        delta = -event->delta_y;
    knob_set(state, gtk_adjustment_get_value(state->adjustment) + delta *
             gtk_adjustment_get_step_increment(state->adjustment));
    return TRUE;
}

static gboolean
knob_key(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    KnobState *state = data;
    double delta;
    (void)widget;
    switch (event->keyval) {
    case GDK_KEY_Up: case GDK_KEY_Right: delta = 1; break;
    case GDK_KEY_Down: case GDK_KEY_Left: delta = -1; break;
    case GDK_KEY_Page_Up: delta = 10; break;
    case GDK_KEY_Page_Down: delta = -10; break;
    case GDK_KEY_Home:
        knob_set(state, gtk_adjustment_get_lower(state->adjustment)); return TRUE;
    case GDK_KEY_End:
        knob_set(state, gtk_adjustment_get_upper(state->adjustment)); return TRUE;
    default: return FALSE;
    }
    knob_set(state, gtk_adjustment_get_value(state->adjustment) + delta *
             gtk_adjustment_get_step_increment(state->adjustment));
    return TRUE;
}

static void
knob_destroyed(GtkWidget *widget, gpointer data)
{
    KnobState *state = data;
    (void)widget;
    state->disposed = TRUE;
    if (gtk_widget_has_grab(state->drawing))
        gtk_grab_remove(state->drawing);
    g_signal_handlers_disconnect_by_data(state->drawing, state);
    if (state->changed_handler != 0) {
        g_signal_handler_disconnect(state->adjustment, state->changed_handler);
        state->changed_handler = 0;
    }
}

static void
knob_state_free(gpointer data)
{
    KnobState *state = data;
    if (state->changed_handler != 0)
        g_signal_handler_disconnect(state->adjustment, state->changed_handler);
    g_object_unref(state->adjustment);
    g_free(state);
}

GtkWidget *
radios_knob_new(const gchar *name, double min, double max, double step, double value)
{
    GtkWidget *box = radios_ui_box(GTK_ORIENTATION_VERTICAL, 2, "knob-control");
    KnobState *state = g_new0(KnobState, 1);
    GtkWidget *label = radios_ui_label(name, "knob-label");
    state->adjustment = g_object_ref_sink(gtk_adjustment_new(value, min, max, step, step * 10, 0));
    state->initial = value;
    state->percentage = min == 0 && max == 100;
    state->pan = min == -100 && max == 100;
    state->drawing = gtk_drawing_area_new();
    state->value = radios_ui_label("", "knob-value");
    gtk_label_set_xalign(GTK_LABEL(label), 0.5);
    gtk_label_set_xalign(GTK_LABEL(state->value), 0.5);
    gtk_widget_set_size_request(state->drawing, 38, 38);
    gtk_widget_set_halign(state->drawing, GTK_ALIGN_CENTER);
    gtk_widget_set_can_focus(state->drawing, TRUE);
    gtk_widget_add_events(state->drawing, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                          GDK_POINTER_MOTION_MASK | GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK |
                          GDK_KEY_PRESS_MASK);
    gtk_widget_set_tooltip_text(state->drawing, "Sürükle / kaydır / ok tuşları; çift tıklama: başlangıç değeri. Ses motoru bağlantısı bekleniyor.");
    atk_object_set_name(gtk_widget_get_accessible(state->drawing), name);
    gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), state->drawing, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), state->value, FALSE, FALSE, 0);
    if (name == NULL || *name == '\0') {
        gtk_widget_set_no_show_all(label, TRUE);
        gtk_widget_hide(label);
    }
    g_object_set_data_full(G_OBJECT(box), "adjustment", g_object_ref(state->adjustment), g_object_unref);
    g_object_set_data(G_OBJECT(box), "dial", state->drawing);
    g_object_set_data(G_OBJECT(box), "value-label", state->value);
    g_object_set_data_full(G_OBJECT(box), "radios-knob-state", state, knob_state_free);
    g_signal_connect(box, "destroy", G_CALLBACK(knob_destroyed), state);
    g_signal_connect(state->drawing, "draw", G_CALLBACK(knob_draw), state);
    g_signal_connect(state->drawing, "button-press-event", G_CALLBACK(knob_press), state);
    g_signal_connect(state->drawing, "button-release-event", G_CALLBACK(knob_release), state);
    g_signal_connect(state->drawing, "motion-notify-event", G_CALLBACK(knob_motion), state);
    g_signal_connect(state->drawing, "scroll-event", G_CALLBACK(knob_scroll), state);
    g_signal_connect(state->drawing, "key-press-event", G_CALLBACK(knob_key), state);
    state->changed_handler = g_signal_connect(state->adjustment, "value-changed", G_CALLBACK(knob_changed), state);
    knob_changed(state->adjustment, state);
    return box;
}

static gboolean
meter_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    MeterState *state = data;
    gboolean vertical = state->vertical;
    double width = gtk_widget_get_allocated_width(widget);
    double height = gtk_widget_get_allocated_height(widget);
    if (vertical) {
        double bar_width = MAX(2, (width - 2) / 2);
        double segment_height = MAX(1, (height - 11) / 12);
        for (guint channel = 0; channel < 2; ++channel)
            for (guint i = 0; i < 12; ++i) {
                gdouble threshold = -60 + i * 60.0 / 11;
                gdouble peak = state->peak[channel] > 0 ? 20 * log10(state->peak[channel]) : -120;
                gdouble rms = state->rms[channel] > 0 ? 20 * log10(state->rms[channel]) : -120;
                gboolean active = state->measured && peak >= threshold;
                rgb(cr, active ? threshold >= 0 ? 0xe85f67 : threshold >= -6 ? 0xd4b451 : rms >= threshold ? 0x54caa0 : 0x379574 :
                    i > 9 ? 0x362829 : i > 7 ? 0x353124 : 0x20362f);
                cairo_rectangle(cr, channel * (bar_width + 2),
                                height - (i + 1) * (segment_height + 1),
                                bar_width, segment_height);
                cairo_fill(cr);
            }
    } else {
        double cell = (width - 15 * 2) / 16;
        const gchar *scale[] = { "−40", "−30", "−20", "−15", "−10", "−6", "−3", "0", "+3" };
        for (guint channel = 0; channel < 2; ++channel)
            for (guint i = 0; i < 16; ++i) {
                const gdouble ticks[] = {-40, -30, -20, -15, -10, -6, -3, 0, 3};
                gdouble position = i * 8.0 / 15; guint tick = MIN((guint)position, 7u);
                gdouble threshold = ticks[tick] + (position - tick) * (ticks[tick + 1] - ticks[tick]);
                gdouble peak = state->peak[channel] > 0 ? 20 * log10(state->peak[channel]) : -120;
                gdouble rms = state->rms[channel] > 0 ? 20 * log10(state->rms[channel]) : -120;
                gboolean active = state->measured && peak >= threshold;
                rgb(cr, active ? threshold >= 0 ? 0xe85f67 : threshold >= -6 ? 0xd4b451 : rms >= threshold ? 0x54caa0 : 0x379574 :
                    i > 13 ? 0x362829 : i > 9 ? 0x353124 : 0x20362f);
                cairo_rectangle(cr, i * (cell + 2), channel == 0 ? 2 : height - 8, cell, 5);
                cairo_fill(cr);
            }
        /* Pango text with the registered Inter face: the cairo toy API would
         * pick one GDI face (Arial) on Windows with no glyph fallback. */
        for (guint i = 0; i < G_N_ELEMENTS(scale); ++i) {
            double x = i * width / (G_N_ELEMENTS(scale) - 1);
            rgb(cr, i == 8 ? 0x97616b : (i == 7 ? 0x9b9060 : 0x72818f));
            gdouble text_width = radios_ui_cairo_show_text(cr, scale[i], 9, PANGO_WEIGHT_NORMAL, TRUE);
            x = CLAMP(x - text_width / 2, 0, MAX(0, width - text_width));
            cairo_move_to(cr, x, height / 2 + 3);
            radios_ui_cairo_show_text(cr, scale[i], 9, PANGO_WEIGHT_NORMAL, FALSE);
        }
    }
    if (state->clipped) {
        rgb(cr, 0xf4656d); cairo_rectangle(cr, MAX(0, width - 3), 0, 3, height); cairo_fill(cr);
    }
    return FALSE;
}

GtkWidget *
radios_meter_new(gint width, gint height, gboolean vertical)
{
    GtkWidget *meter = gtk_drawing_area_new();
    MeterState *state = g_new0(MeterState, 1); state->vertical = vertical;
    gtk_widget_set_size_request(meter, width, height);
    gtk_widget_set_tooltip_text(meter, "İki kanallı seviye göstergesi; gerçek ses ölçümü bekleniyor.");
    atk_object_set_name(gtk_widget_get_accessible(meter), "İki kanallı ses seviyesi, ölçüm bekleniyor");
    g_object_set_data_full(G_OBJECT(meter), "radios-meter-state", state, g_free);
    g_signal_connect(meter, "draw", G_CALLBACK(meter_draw), state);
    return meter;
}

void radios_meter_set_levels(GtkWidget *meter, gdouble left_peak, gdouble right_peak,
    gdouble left_rms, gdouble right_rms, gboolean clipped)
{
    if (!meter) return;
    MeterState *state = g_object_get_data(G_OBJECT(meter), "radios-meter-state"); if (!state) return;
    state->peak[0] = isfinite(left_peak) ? MAX(0, left_peak) : 0;
    state->peak[1] = isfinite(right_peak) ? MAX(0, right_peak) : 0;
    state->rms[0] = isfinite(left_rms) ? MAX(0, left_rms) : 0;
    state->rms[1] = isfinite(right_rms) ? MAX(0, right_rms) : 0;
    state->measured = TRUE; state->clipped = clipped;
    gchar *text = g_strdup_printf("Ölçülen seviye · L %.1f dBFS · R %.1f dBFS%s",
        state->peak[0] > 0 ? 20 * log10(state->peak[0]) : -120,
        state->peak[1] > 0 ? 20 * log10(state->peak[1]) : -120, clipped ? " · kırpılma" : "");
    gtk_widget_set_tooltip_text(meter, text); atk_object_set_description(gtk_widget_get_accessible(meter), text); g_free(text);
    gtk_widget_queue_draw(meter);
}

static gboolean
clock_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    const gchar *identifier = g_object_get_data(G_OBJECT(widget), "radios-clock-timezone");
    GTimeZone *zone = radios_time_zone_new(identifier ? identifier : "UTC");
    if (!zone) zone = g_time_zone_new_utc();
    GDateTime *now = g_date_time_new_now(zone); g_time_zone_unref(zone);
    if (!now) return FALSE;
    double width = gtk_widget_get_allocated_width(widget);
    double height = gtk_widget_get_allocated_height(widget);
    double size = MIN(width, height);
    const double radii[] = { 25, 39, 41 };
    double seconds = g_date_time_get_second(now);
    double minutes = g_date_time_get_minute(now) + seconds / 60;
    double hours = g_date_time_get_hour(now) % 12 + minutes / 60;
    double angles[] = { hours * RADIOS_PI / 6, minutes * RADIOS_PI / 30,
                        seconds * RADIOS_PI / 30 };
    const guint colors[] = { 0xd4d9e0, 0xe8ecf0, 0x90a4b7 };
    (void)data;
    cairo_translate(cr, width / 2, height / 2);
    cairo_scale(cr, size / 120, size / 120);
    rgb(cr, 0x0b0d11);
    cairo_arc(cr, 0, 0, 57, 0, 2 * RADIOS_PI);
    cairo_fill_preserve(cr);
    rgb(cr, 0x343941);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);
    for (guint i = 0; i < 12; ++i) {
        double angle = i * RADIOS_PI / 6;
        rgb(cr, 0x68717d);
        cairo_move_to(cr, sin(angle) * 49, -cos(angle) * 49);
        cairo_line_to(cr, sin(angle) * 43, -cos(angle) * 43);
        cairo_stroke(cr);
    }
    const gchar *numbers[] = { "12", "3", "6", "9" };
    const double positions[][2] = { { 0, -31 }, { 34, 4 }, { 0, 39 }, { -34, 4 } };
    rgb(cr, 0x929aa5);
    for (guint i = 0; i < G_N_ELEMENTS(numbers); ++i) {
        gdouble text_width = radios_ui_cairo_show_text(cr, numbers[i], 9, PANGO_WEIGHT_NORMAL, TRUE);
        cairo_move_to(cr, positions[i][0] - text_width / 2, positions[i][1]);
        radios_ui_cairo_show_text(cr, numbers[i], 9, PANGO_WEIGHT_NORMAL, FALSE);
    }
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    for (guint i = 0; i < G_N_ELEMENTS(radii); ++i) {
        rgb(cr, colors[i]);
        cairo_set_line_width(cr, 3 - i);
        cairo_move_to(cr, -sin(angles[i]) * (4 + i * 2), cos(angles[i]) * (4 + i * 2));
        cairo_line_to(cr, sin(angles[i]) * radii[i], -cos(angles[i]) * radii[i]);
        cairo_stroke(cr);
    }
    rgb(cr, 0xc9d1db);
    cairo_arc(cr, 0, 0, 3, 0, 2 * RADIOS_PI);
    cairo_fill(cr);
    g_date_time_unref(now);
    return FALSE;
}

static gboolean
clock_tick(gpointer data)
{
    gtk_widget_queue_draw(GTK_WIDGET(data));
    return G_SOURCE_CONTINUE;
}

static void
clock_destroyed(GtkWidget *widget, gpointer data)
{
    ClockState *state = data;
    (void)widget;
    if (state->timer != 0) {
        g_source_remove(state->timer);
        state->timer = 0;
    }
}

GtkWidget *
radios_analog_clock_new(void)
{
    GtkWidget *clock = gtk_drawing_area_new();
    ClockState *state = g_new0(ClockState, 1);
    gtk_widget_set_size_request(clock, 46, 46);
    gtk_widget_set_halign(clock, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(clock, GTK_ALIGN_CENTER);
    g_signal_connect(clock, "draw", G_CALLBACK(clock_draw), NULL);
    g_signal_connect(clock, "destroy", G_CALLBACK(clock_destroyed), state);
    g_object_set_data_full(G_OBJECT(clock), "clock-state", state, g_free);
    state->timer = g_timeout_add_seconds(1, clock_tick, clock);
    return clock;
}
