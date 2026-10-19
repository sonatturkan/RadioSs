#include "radios/neon_progress.h"

#include <atk/atk.h>
#include <math.h>

/* Numeric warning defaults are implementation choices: the reference specifies
 * changing colours and increasing frequency, but supplies no thresholds. */
#define NEON_INSET 10.0
#define NEON_WARNING_MS 30000.0
#define NEON_CRITICAL_MS 10000.0
#define NEON_FRAME_MS 33

typedef struct {
    GtkDrawingArea parent;
    guint64 position_ms;
    guint64 duration_ms;
    guint64 preview_ms;
    gboolean playing;
    gboolean seekable;
    gboolean dragging;
    gboolean destroyed;
    guint timer_id;
    gint64 last_frame_us;
    double phase;
    RadiosNeonProgressSeekCallback seek_callback;
    gpointer seek_data;
} RadiosNeonProgress;

typedef GtkDrawingAreaClass RadiosNeonProgressClass;

G_DEFINE_TYPE(RadiosNeonProgress, radios_neon_progress, GTK_TYPE_DRAWING_AREA)

static guint seek_begin_signal;

static RadiosNeonProgress *neon_from_widget(GtkWidget *widget)
{
    g_return_val_if_fail(G_TYPE_CHECK_INSTANCE_TYPE(widget,
                        radios_neon_progress_get_type()), NULL);
    return (RadiosNeonProgress *)widget;
}

static double warning_window(const RadiosNeonProgress *neon)
{
    return MIN(NEON_WARNING_MS, (double)neon->duration_ms * 0.25);
}

static double remaining_ms(const RadiosNeonProgress *neon)
{
    const guint64 position = neon->dragging ? neon->preview_ms : neon->position_ms;
    return (double)(neon->duration_ms - MIN(position, neon->duration_ms));
}

static gboolean should_pulse(const RadiosNeonProgress *neon)
{
    return neon->playing && neon->duration_ms > 0 &&
           remaining_ms(neon) <= warning_window(neon);
}

static double pulse_frequency(const RadiosNeonProgress *neon)
{
    const double window = warning_window(neon);
    return window > 0.0 ? 0.5 + 2.5 * (1.0 - CLAMP(remaining_ms(neon) / window,
                                                  0.0, 1.0)) : 0.0;
}

static gboolean neon_tick(gpointer user_data)
{
    RadiosNeonProgress *neon = user_data;
    if (neon->destroyed || !should_pulse(neon)) {
        neon->timer_id = 0;
        return G_SOURCE_REMOVE;
    }
    const gint64 now = g_get_monotonic_time();
    const double elapsed = MIN((double)(now - neon->last_frame_us) / 1000000.0,
                               0.25);
    neon->last_frame_us = now;
    neon->phase = fmod(neon->phase + elapsed * pulse_frequency(neon), 1.0);
    gtk_widget_queue_draw(GTK_WIDGET(neon));
    return G_SOURCE_CONTINUE;
}

static void update_animation(RadiosNeonProgress *neon)
{
    if (should_pulse(neon) && !neon->destroyed) {
        if (neon->timer_id == 0) {
            neon->last_frame_us = g_get_monotonic_time();
            neon->timer_id = g_timeout_add(NEON_FRAME_MS, neon_tick, neon);
        }
    } else {
        if (neon->timer_id != 0) {
            g_source_remove(neon->timer_id);
            neon->timer_id = 0;
        }
        neon->phase = 0.0;
    }
}

static void neon_stroke(cairo_t *cr, double start, double end, double y,
                        const double *colour, double alpha, double width)
{
    if (end <= start)
        return;
    cairo_set_source_rgba(cr, colour[0], colour[1], colour[2], alpha);
    cairo_set_line_width(cr, width);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
    cairo_move_to(cr, start, y);
    cairo_line_to(cr, end, y);
    cairo_stroke(cr);
}

static void neon_glow(cairo_t *cr, double start, double end, double y,
                      const double *colour, double strength)
{
    /* The difference between successive Gaussian coverage values gives a
     * smooth halo without a few visibly concentric capsule outlines. The 1px
     * diameter increments leave the sharp 3px core independent of the glow. */
    double previous_coverage = 0.0;
    for (gint diameter = 24; diameter >= 4; --diameter) {
        const double distance = MAX(0.0, ((double)diameter - 3.0) * 0.5);
        const double coverage = 0.40 * exp(-(distance * distance) / 30.0);
        const double alpha = (coverage - previous_coverage) /
                             (1.0 - previous_coverage);
        neon_stroke(cr, start, end, y, colour, strength * alpha,
                     (double)diameter);
        previous_coverage = coverage;
    }
}

static gboolean neon_draw(GtkWidget *widget, cairo_t *cr)
{
    RadiosNeonProgress *neon = (RadiosNeonProgress *)widget;
    const double width = (double)gtk_widget_get_allocated_width(widget);
    const double height = (double)gtk_widget_get_allocated_height(widget);
    const double start = MIN(NEON_INSET, width * 0.25);
    const double end = MAX(start, width - start);
    const double y = height * 0.5;
    const guint64 position = neon->dragging ? neon->preview_ms : neon->position_ms;
    const double ratio = neon->duration_ms > 0 ?
        CLAMP((double)position / (double)neon->duration_ms, 0.0, 1.0) : 0.0;
    const double split = start + (end - start) * ratio;
    const double window = warning_window(neon);
    const double critical = MIN(NEON_CRITICAL_MS, (double)neon->duration_ms / 12.0);
    const gboolean warning = neon->duration_ms > 0 && remaining_ms(neon) <= window;
    double colour[3] = {1.0, 1.0, 1.0};
    if (warning) {
        colour[1] = remaining_ms(neon) <= critical ? 0.19 : 0.72;
        colour[2] = remaining_ms(neon) <= critical ? 0.12 : 0.16;
    }
    const double pulse = should_pulse(neon) ?
        0.72 + 0.28 * (0.5 + 0.5 * cos(neon->phase * 2.0 * G_PI)) : 1.0;
    const double rest_alpha = (warning ? 0.32 : 0.16) * pulse;

    neon_glow(cr, start, end, y, colour, rest_alpha);
    neon_stroke(cr, start, end, y, colour, rest_alpha, 3.0);
    neon_glow(cr, start, split, y, colour, pulse);
    neon_stroke(cr, start, split, y, colour, pulse, 3.0);

    if (gtk_widget_has_focus(widget)) {
        GtkStyleContext *style = gtk_widget_get_style_context(widget);
        gtk_render_focus(style, cr, start - 3.0, 1.0,
                         MAX(1.0, end - start + 6.0), MAX(1.0, height - 2.0));
    }
    return FALSE;
}

static guint64 position_at_x(const RadiosNeonProgress *neon, double x)
{
    const double width = (double)gtk_widget_get_allocated_width(GTK_WIDGET(neon));
    const double start = MIN(NEON_INSET, width * 0.25);
    const double span = MAX(1.0, width - 2.0 * start);
    const double ratio = CLAMP((x - start) / span, 0.0, 1.0);
    if (ratio >= 1.0)
        return neon->duration_ms;
    return (guint64)((long double)neon->duration_ms * (long double)ratio);
}

static void cancel_drag(RadiosNeonProgress *neon)
{
    neon->dragging = FALSE;
    neon->preview_ms = neon->position_ms;
    if (gtk_widget_has_grab(GTK_WIDGET(neon)))
        gtk_grab_remove(GTK_WIDGET(neon));
    update_animation(neon);
    gtk_widget_queue_draw(GTK_WIDGET(neon));
}

static gboolean commit_position(RadiosNeonProgress *neon, guint64 position)
{
    if (!neon->seekable || neon->duration_ms == 0 || neon->seek_callback == NULL)
        return FALSE;
    g_object_ref(neon);
    const gboolean accepted = neon->seek_callback(MIN(position, neon->duration_ms),
                                                  neon->seek_data);
    /* The player snapshot remains authoritative until the next refresh. */
    if (!neon->destroyed)
        gtk_widget_queue_draw(GTK_WIDGET(neon));
    g_object_unref(neon);
    return accepted;
}

static gboolean neon_press(GtkWidget *widget, GdkEventButton *event)
{
    RadiosNeonProgress *neon = (RadiosNeonProgress *)widget;
    if (event->button != 1 || event->type != GDK_BUTTON_PRESS ||
        !neon->seekable || neon->duration_ms == 0)
        return FALSE;
    gtk_widget_grab_focus(widget);
    g_signal_emit(neon, seek_begin_signal, 0);
    if (!neon->seekable || neon->duration_ms == 0)
        return TRUE;
    neon->dragging = TRUE;
    neon->preview_ms = position_at_x(neon, event->x);
    gtk_grab_add(widget);
    update_animation(neon);
    gtk_widget_queue_draw(widget);
    return TRUE;
}

static gboolean neon_motion(GtkWidget *widget, GdkEventMotion *event)
{
    RadiosNeonProgress *neon = (RadiosNeonProgress *)widget;
    if (!neon->dragging)
        return FALSE;
    neon->preview_ms = position_at_x(neon, event->x);
    update_animation(neon);
    gtk_widget_queue_draw(widget);
    return TRUE;
}

static gboolean neon_release(GtkWidget *widget, GdkEventButton *event)
{
    RadiosNeonProgress *neon = (RadiosNeonProgress *)widget;
    if (event->button != 1 || !neon->dragging)
        return FALSE;
    const guint64 target = position_at_x(neon, event->x);
    cancel_drag(neon);
    (void)commit_position(neon, target);
    return TRUE;
}

static gboolean neon_key(GtkWidget *widget, GdkEventKey *event)
{
    RadiosNeonProgress *neon = (RadiosNeonProgress *)widget;
    if (event->keyval == GDK_KEY_Escape && neon->dragging) {
        cancel_drag(neon);
        return TRUE;
    }
    if (!neon->seekable || neon->duration_ms == 0 || neon->dragging)
        return FALSE;
    guint64 target = neon->position_ms;
    const guint64 step = (event->state & GDK_SHIFT_MASK) ? 10000 : 5000;
    switch (event->keyval) {
    case GDK_KEY_Left:
        target = target > step ? target - step : 0;
        break;
    case GDK_KEY_Right:
        target += MIN(step, neon->duration_ms - target);
        break;
    case GDK_KEY_Home:
        target = 0;
        break;
    case GDK_KEY_End:
        target = neon->duration_ms;
        break;
    default:
        return FALSE;
    }
    g_signal_emit(neon, seek_begin_signal, 0);
    (void)commit_position(neon, target);
    return TRUE;
}

static gboolean neon_focus_out(GtkWidget *widget, GdkEventFocus *event)
{
    (void)event;
    RadiosNeonProgress *neon = (RadiosNeonProgress *)widget;
    if (neon->dragging)
        cancel_drag(neon);
    return FALSE;
}

static gboolean neon_grab_broken(GtkWidget *widget, GdkEventGrabBroken *event)
{
    (void)event;
    RadiosNeonProgress *neon = (RadiosNeonProgress *)widget;
    if (neon->dragging)
        cancel_drag(neon);
    return FALSE;
}

static void neon_destroy(GtkWidget *widget)
{
    RadiosNeonProgress *neon = (RadiosNeonProgress *)widget;
    neon->destroyed = TRUE;
    cancel_drag(neon);
    GTK_WIDGET_CLASS(radios_neon_progress_parent_class)->destroy(widget);
}

static void radios_neon_progress_class_init(RadiosNeonProgressClass *klass)
{
    GtkWidgetClass *widget = GTK_WIDGET_CLASS(klass);
    widget->draw = neon_draw;
    widget->button_press_event = neon_press;
    widget->button_release_event = neon_release;
    widget->motion_notify_event = neon_motion;
    widget->key_press_event = neon_key;
    widget->focus_out_event = neon_focus_out;
    widget->grab_broken_event = neon_grab_broken;
    widget->destroy = neon_destroy;
    seek_begin_signal = g_signal_new("seek-begin", G_TYPE_FROM_CLASS(klass),
        G_SIGNAL_RUN_LAST, 0, NULL, NULL, NULL, G_TYPE_NONE, 0);
}

static void radios_neon_progress_init(RadiosNeonProgress *neon)
{
    GtkWidget *widget = GTK_WIDGET(neon);
    gtk_widget_set_name(widget, "header-neon-progress");
    gtk_widget_set_hexpand(widget, TRUE);
    gtk_widget_set_can_focus(widget, TRUE);
    gtk_widget_set_size_request(widget, 40, 24);
    gtk_widget_add_events(widget, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
                          GDK_POINTER_MOTION_MASK | GDK_KEY_PRESS_MASK |
                          GDK_FOCUS_CHANGE_MASK);
    AtkObject *accessible = gtk_widget_get_accessible(widget);
    atk_object_set_role(accessible, ATK_ROLE_SLIDER);
    atk_object_set_name(accessible, "Şarkı süresi");
    atk_object_set_description(accessible,
        "Şarkıda konum seçmek için sürükleyin. Sol ve sağ ok: 5 saniye; "
        "Shift ile 10 saniye. Home: başlangıç, End: son, Escape: iptal.");
    gtk_widget_set_tooltip_text(widget,
        "Şarkı süresi • Konum seçmek için sürükleyin veya ok tuşlarını kullanın");
}

GtkWidget *radios_neon_progress_new(void)
{
    return g_object_new(radios_neon_progress_get_type(), NULL);
}

void radios_neon_progress_set_state(GtkWidget *widget, guint64 position_ms,
                                    guint64 duration_ms, gboolean playing,
                                    gboolean seekable)
{
    RadiosNeonProgress *neon = neon_from_widget(widget);
    if (neon == NULL || neon->destroyed)
        return;
    const gboolean duration_changed = duration_ms != neon->duration_ms;
    neon->duration_ms = duration_ms;
    neon->position_ms = MIN(position_ms, duration_ms);
    neon->playing = playing;
    neon->seekable = seekable && duration_ms > 0;
    if (neon->dragging && (!neon->seekable || duration_changed))
        cancel_drag(neon);
    update_animation(neon);
    gtk_widget_queue_draw(widget);
}

void radios_neon_progress_get_state(GtkWidget *widget, guint64 *position_ms,
                                    guint64 *duration_ms, gboolean *playing,
                                    gboolean *seekable)
{
    RadiosNeonProgress *neon = neon_from_widget(widget);
    if (neon == NULL)
        return;
    if (position_ms != NULL)
        *position_ms = neon->position_ms;
    if (duration_ms != NULL)
        *duration_ms = neon->duration_ms;
    if (playing != NULL)
        *playing = neon->playing;
    if (seekable != NULL)
        *seekable = neon->seekable;
}

void radios_neon_progress_set_seek_callback(GtkWidget *widget,
                                           RadiosNeonProgressSeekCallback callback,
                                           gpointer user_data)
{
    RadiosNeonProgress *neon = neon_from_widget(widget);
    if (neon == NULL)
        return;
    neon->seek_callback = callback;
    neon->seek_data = user_data;
}
