#include "radios/neon_progress.h"

#include <math.h>
#include <string.h>

typedef struct {
    GtkWidget *window;
    GtkWidget *bar;
    guint calls;
    guint begins;
    guint64 target;
    gboolean accept;
} Fixture;

static void drain_events(void)
{
    while (g_main_context_iteration(NULL, FALSE)) {}
}

static gboolean seek_record(guint64 position_ms, gpointer user_data)
{
    Fixture *fixture = user_data;
    fixture->calls++;
    fixture->target = position_ms;
    return fixture->accept;
}

static void begin_record(GtkWidget *widget, gpointer user_data)
{
    (void)widget;
    Fixture *fixture = user_data;
    fixture->begins++;
}

static void setup(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    fixture->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    fixture->bar = radios_neon_progress_new();
    fixture->accept = TRUE;
    gtk_window_set_default_size(GTK_WINDOW(fixture->window), 420, 24);
    gtk_container_add(GTK_CONTAINER(fixture->window), fixture->bar);
    radios_neon_progress_set_seek_callback(fixture->bar, seek_record, fixture);
    g_signal_connect(fixture->bar, "seek-begin", G_CALLBACK(begin_record), fixture);
    gtk_widget_show_all(fixture->window);
    drain_events();
    gtk_window_set_focus(GTK_WINDOW(fixture->window), NULL);
    radios_neon_progress_set_state(fixture->bar, 20000, 100000, FALSE, TRUE);
}

static void teardown(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    gtk_widget_destroy(fixture->window);
    drain_events();
}

static gboolean event_send(Fixture *fixture, GdkEventType type, double x,
                            guint keyval, GdkModifierType modifiers)
{
    GdkEvent *event = gdk_event_new(type);
    event->any.window = g_object_ref(gtk_widget_get_window(fixture->bar));
    event->any.send_event = TRUE;
    if (type == GDK_BUTTON_PRESS || type == GDK_BUTTON_RELEASE) {
        event->button.button = 1;
        event->button.x = x;
        event->button.y = 12.0;
        event->button.state = modifiers;
    } else if (type == GDK_MOTION_NOTIFY) {
        event->motion.x = x;
        event->motion.y = 12.0;
        event->motion.state = GDK_BUTTON1_MASK;
    } else if (type == GDK_KEY_PRESS) {
        event->key.keyval = keyval;
        event->key.state = modifiers;
    }
    const gboolean handled = gtk_widget_event(fixture->bar, event);
    gdk_event_free(event);
    return handled;
}

static guint64 authoritative_position(Fixture *fixture)
{
    guint64 position = 0;
    radios_neon_progress_get_state(fixture->bar, &position, NULL, NULL, NULL);
    return position;
}

static cairo_surface_t *render(Fixture *fixture)
{
    const int width = gtk_widget_get_allocated_width(fixture->bar);
    const int height = gtk_widget_get_allocated_height(fixture->bar);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                                          width, height);
    cairo_t *cr = cairo_create(surface);
    gtk_widget_draw(fixture->bar, cr);
    cairo_destroy(cr);
    cairo_surface_flush(surface);
    return surface;
}

static guint pixel(cairo_surface_t *surface, int x, int y, guint shift)
{
    guint32 value = 0;
    const unsigned char *data = cairo_image_surface_get_data(surface);
    const int stride = cairo_image_surface_get_stride(surface);
    memcpy(&value, data + y * stride + x * 4, sizeof(value));
    return (value >> shift) & 0xff;
}

static void test_geometry(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    radios_neon_progress_set_state(fixture->bar, 50000, 100000, FALSE, TRUE);
    cairo_surface_t *surface = render(fixture);
    const int middle = gtk_widget_get_allocated_height(fixture->bar) / 2;
    /* Observe the real Cairo image: strong white core, dim remaining segment,
     * rounded 10px inset, and a halo rather than a thick solid control. */
    g_assert_cmpuint(pixel(surface, 70, middle, 24), >, 235);
    g_assert_cmpuint(pixel(surface, 350, middle, 24), <, 100);
    g_assert_cmpuint(pixel(surface, 350, middle, 24), >, 20);
    g_assert_cmpuint(pixel(surface, 70, middle, 16), ==,
                     pixel(surface, 70, middle, 8));
    g_assert_cmpuint(pixel(surface, 70, middle - 5, 24), >, 0);
    g_assert_cmpuint(pixel(surface, 70, middle - 5, 24), <, 120);
    g_assert_cmpuint(pixel(surface, 0, middle, 24), <, 50);
    g_assert_cmpuint(pixel(surface, 9, middle, 24), >, 100);
    cairo_surface_destroy(surface);
}

static void test_drag_once(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    g_assert_true(event_send(fixture, GDK_BUTTON_PRESS, 110, 0, 0));
    g_assert_cmpuint(fixture->begins, ==, 1);
    g_assert_cmpuint(fixture->calls, ==, 0);
    for (guint i = 0; i < 20; i++)
        g_assert_true(event_send(fixture, GDK_MOTION_NOTIFY, 150 + i * 10, 0, 0));
    g_assert_cmpuint(fixture->calls, ==, 0);
    g_assert_cmpuint(authoritative_position(fixture), ==, 20000);
    g_assert_true(event_send(fixture, GDK_BUTTON_RELEASE, 310, 0, 0));
    g_assert_cmpuint(fixture->calls, ==, 1);
    g_assert_cmpuint(fixture->target, ==, 75000);
    g_assert_false(gtk_widget_has_grab(fixture->bar));
    g_assert_cmpuint(authoritative_position(fixture), ==, 20000);
}

static void test_cancel_and_state_change(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    event_send(fixture, GDK_BUTTON_PRESS, 310, 0, 0);
    g_assert_true(event_send(fixture, GDK_KEY_PRESS, 0, GDK_KEY_Escape, 0));
    g_assert_false(event_send(fixture, GDK_BUTTON_RELEASE, 310, 0, 0));
    g_assert_cmpuint(fixture->calls, ==, 0);
    g_assert_cmpuint(authoritative_position(fixture), ==, 20000);
    event_send(fixture, GDK_BUTTON_PRESS, 310, 0, 0);
    radios_neon_progress_set_state(fixture->bar, 12000, 100000, TRUE, FALSE);
    g_assert_false(event_send(fixture, GDK_BUTTON_RELEASE, 310, 0, 0));
    g_assert_cmpuint(fixture->calls, ==, 0);
    g_assert_false(gtk_widget_has_grab(fixture->bar));
    g_assert_cmpuint(authoritative_position(fixture), ==, 12000);
}

static void test_grab_focus_duration(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    event_send(fixture, GDK_BUTTON_PRESS, 310, 0, 0);
    event_send(fixture, GDK_FOCUS_CHANGE, 0, 0, 0);
    g_assert_false(event_send(fixture, GDK_BUTTON_RELEASE, 310, 0, 0));
    g_assert_cmpuint(fixture->calls, ==, 0);
    event_send(fixture, GDK_BUTTON_PRESS, 310, 0, 0);
    event_send(fixture, GDK_GRAB_BROKEN, 0, 0, 0);
    g_assert_false(event_send(fixture, GDK_BUTTON_RELEASE, 310, 0, 0));
    g_assert_cmpuint(fixture->calls, ==, 0);
    event_send(fixture, GDK_BUTTON_PRESS, 310, 0, 0);
    radios_neon_progress_set_state(fixture->bar, 15000, 200000, FALSE, TRUE);
    g_assert_false(event_send(fixture, GDK_BUTTON_RELEASE, 310, 0, 0));
    g_assert_cmpuint(fixture->calls, ==, 0);
    g_assert_cmpuint(authoritative_position(fixture), ==, 15000);
    g_assert_false(gtk_widget_has_grab(fixture->bar));
}

static void test_preview_survives_refresh(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    event_send(fixture, GDK_BUTTON_PRESS, 310, 0, 0);
    radios_neon_progress_set_state(fixture->bar, 22000, 100000, FALSE, TRUE);
    cairo_surface_t *surface = render(fixture);
    const int middle = gtk_widget_get_allocated_height(fixture->bar) / 2;
    g_assert_cmpuint(pixel(surface, 200, middle, 24), >, 230);
    g_assert_cmpuint(authoritative_position(fixture), ==, 22000);
    cairo_surface_destroy(surface);
    event_send(fixture, GDK_BUTTON_RELEASE, 310, 0, 0);
    g_assert_cmpuint(fixture->calls, ==, 1);
    g_assert_cmpuint(fixture->target, ==, 75000);
}

static void invalidate_on_begin(GtkWidget *widget, gpointer user_data)
{
    (void)user_data;
    radios_neon_progress_set_state(widget, 1000, 100000, FALSE, FALSE);
}

static void test_begin_invalidates(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    g_signal_connect(fixture->bar, "seek-begin", G_CALLBACK(invalidate_on_begin), NULL);
    event_send(fixture, GDK_BUTTON_PRESS, 310, 0, 0);
    g_assert_false(event_send(fixture, GDK_BUTTON_RELEASE, 310, 0, 0));
    g_assert_false(gtk_widget_has_grab(fixture->bar));
    g_assert_cmpuint(fixture->calls, ==, 0);
    radios_neon_progress_set_state(fixture->bar, 1000, 100000, FALSE, TRUE);
    event_send(fixture, GDK_KEY_PRESS, 0, GDK_KEY_End, 0);
    g_assert_cmpuint(fixture->calls, ==, 0);
}

static void test_keyboard_and_invalid(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    gtk_widget_grab_focus(fixture->bar);
    g_assert_true(event_send(fixture, GDK_KEY_PRESS, 0, GDK_KEY_Right, 0));
    g_assert_cmpuint(fixture->target, ==, 25000);
    event_send(fixture, GDK_KEY_PRESS, 0, GDK_KEY_Left, GDK_SHIFT_MASK);
    g_assert_cmpuint(fixture->target, ==, 10000);
    event_send(fixture, GDK_KEY_PRESS, 0, GDK_KEY_Home, 0);
    g_assert_cmpuint(fixture->target, ==, 0);
    event_send(fixture, GDK_KEY_PRESS, 0, GDK_KEY_End, 0);
    g_assert_cmpuint(fixture->target, ==, 100000);
    g_assert_cmpuint(fixture->begins, ==, 4);
    g_assert_cmpuint(fixture->calls, ==, 4);
    radios_neon_progress_set_state(fixture->bar, G_MAXUINT64, 0, TRUE, TRUE);
    g_assert_false(event_send(fixture, GDK_KEY_PRESS, 0, GDK_KEY_End, 0));
    g_assert_false(event_send(fixture, GDK_BUTTON_PRESS, 100, 0, 0));
    gboolean seekable = TRUE;
    radios_neon_progress_get_state(fixture->bar, NULL, NULL, NULL, &seekable);
    g_assert_false(seekable);
    g_assert_cmpuint(authoritative_position(fixture), ==, 0);
    g_assert_cmpuint(fixture->calls, ==, 4);
}

static void test_rejection_and_clamping(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    fixture->accept = FALSE;
    event_send(fixture, GDK_BUTTON_PRESS, -50, 0, 0);
    event_send(fixture, GDK_BUTTON_RELEASE, -50, 0, 0);
    g_assert_cmpuint(fixture->target, ==, 0);
    g_assert_cmpuint(authoritative_position(fixture), ==, 20000);
    event_send(fixture, GDK_BUTTON_PRESS, 600, 0, 0);
    event_send(fixture, GDK_BUTTON_RELEASE, 600, 0, 0);
    g_assert_cmpuint(fixture->target, ==, 100000);
    g_assert_cmpuint(authoritative_position(fixture), ==, 20000);
    radios_neon_progress_set_state(fixture->bar, 200000, 100000, FALSE, TRUE);
    g_assert_cmpuint(authoritative_position(fixture), ==, 100000);
}

static void test_colours(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    radios_neon_progress_set_state(fixture->bar, 80000, 120000, FALSE, TRUE);
    cairo_surface_t *normal = render(fixture);
    radios_neon_progress_set_state(fixture->bar, 100000, 120000, FALSE, TRUE);
    cairo_surface_t *warning = render(fixture);
    radios_neon_progress_set_state(fixture->bar, 115000, 120000, FALSE, TRUE);
    cairo_surface_t *critical = render(fixture);
    const int middle = gtk_widget_get_allocated_height(fixture->bar) / 2;
    /* Both filled (x70) and remaining (x408) parts acquire the warning colour. */
    for (int x = 70; x <= 408; x += 338) {
        g_assert_cmpuint(pixel(normal, x, middle, 16), ==,
                         pixel(normal, x, middle, 8));
        g_assert_cmpuint(pixel(warning, x, middle, 16), >,
                         pixel(warning, x, middle, 8));
        g_assert_cmpuint(pixel(critical, x, middle, 8), <,
                         pixel(warning, x, middle, 8));
    }
    cairo_surface_destroy(normal);
    cairo_surface_destroy(warning);
    cairo_surface_destroy(critical);
}

/* Count peaks in observed pixel light, rather than repeating the frequency
 * formula. One window shows the same physical bar at early and late warning. */
static guint observe_peaks(Fixture *fixture, guint64 position, guint64 duration,
                           guint observation_ms, gboolean playing)
{
    radios_neon_progress_set_state(fixture->bar, position, duration, playing, TRUE);
    const gint64 deadline = g_get_monotonic_time() + (gint64)observation_ms * 1000;
    guint peaks = 0;
    guint previous = 0;
    gboolean rising = FALSE;
    while (g_get_monotonic_time() < deadline) {
        g_usleep(20000);
        drain_events();
        cairo_surface_t *surface = render(fixture);
        const int middle = gtk_widget_get_allocated_height(fixture->bar) / 2;
        const guint value = pixel(surface, 70, middle, 24);
        cairo_surface_destroy(surface);
        if (value > previous + 1)
            rising = TRUE;
        else if (rising && value + 1 < previous) {
            peaks++;
            rising = FALSE;
        }
        previous = value;
    }
    return peaks;
}

static void test_pulse_and_pause(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    const guint early = observe_peaks(fixture, 91300, 120000, 2200, TRUE);
    const guint late = observe_peaks(fixture, 119000, 120000, 2200, TRUE);
    g_assert_cmpuint(early, >=, 1);
    g_assert_cmpuint(late, >, early);
    g_assert_cmpuint(late, <=, 8);
    const guint paused = observe_peaks(fixture, 119000, 120000, 350, FALSE);
    const guint unknown = observe_peaks(fixture, 0, 0, 350, TRUE);
    g_assert_cmpuint(paused, ==, 0);
    g_assert_cmpuint(unknown, ==, 0);
}

static void test_destroy_during_pulse(Fixture *fixture, gconstpointer unused)
{
    (void)unused;
    radios_neon_progress_set_state(fixture->bar, 119000, 120000, TRUE, TRUE);
    GtkWidget *bar = g_object_ref(fixture->bar);
    gtk_widget_destroy(bar);
    g_usleep(100000);
    drain_events();
    radios_neon_progress_set_state(bar, 100, 200, TRUE, TRUE);
    g_object_unref(bar);
}

int main(int argc, char **argv)
{
    gtk_test_init(&argc, &argv, NULL);
    g_test_add("/neon/geometry", Fixture, NULL, setup, test_geometry, teardown);
    g_test_add("/neon/drag-once", Fixture, NULL, setup, test_drag_once, teardown);
    g_test_add("/neon/cancel-state-change", Fixture, NULL, setup,
               test_cancel_and_state_change, teardown);
    g_test_add("/neon/grab-focus-duration", Fixture, NULL, setup,
               test_grab_focus_duration, teardown);
    g_test_add("/neon/preview-refresh", Fixture, NULL, setup,
               test_preview_survives_refresh, teardown);
    g_test_add("/neon/begin-invalidates", Fixture, NULL, setup,
               test_begin_invalidates, teardown);
    g_test_add("/neon/keyboard-invalid", Fixture, NULL, setup,
               test_keyboard_and_invalid, teardown);
    g_test_add("/neon/reject-clamp", Fixture, NULL, setup,
               test_rejection_and_clamping, teardown);
    g_test_add("/neon/colours", Fixture, NULL, setup, test_colours, teardown);
    g_test_add("/neon/pulse-pause", Fixture, NULL, setup,
               test_pulse_and_pause, teardown);
    g_test_add("/neon/destroy-pulse", Fixture, NULL, setup,
               test_destroy_during_pulse, teardown);
    return g_test_run();
}
