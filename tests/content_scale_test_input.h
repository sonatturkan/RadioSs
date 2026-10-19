/* Real input and screen geometry for the content-scale GTK tests.
 *
 * X11 (private Xvfb): XTest events and root coordinates.
 * Wayland: a private headless GNOME Shell started with --unsafe-mode on this
 * process's session bus (see docs/kilavuz-2026-10-18-C-C3.md). Its
 * org.gnome.Shell.Eval drives Clutter virtual input devices and reports where
 * windows really are, because a Wayland client cannot know its own position:
 * popup placement is decided by the compositor. */
#ifndef RADIOS_CONTENT_SCALE_TEST_INPUT_H
#define RADIOS_CONTENT_SCALE_TEST_INPUT_H
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <math.h>
#include <stdio.h>

static gboolean cs_wayland(void) { return !GDK_IS_X11_DISPLAY(gdk_display_get_default()); }
static Display *cs_xdisplay(void) { return GDK_DISPLAY_XDISPLAY(gdk_display_get_default()); }
/* Runs JavaScript in the test compositor and returns its JSON result. */
static gchar *cs_eval(const gchar *script)
{
    static GDBusConnection *bus;
    GError *error = NULL;
    if (!bus) { bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error); g_assert_no_error(error); }
    GVariant *reply = g_dbus_connection_call_sync(bus, "org.gnome.Shell", "/org/gnome/Shell", "org.gnome.Shell", "Eval",
        g_variant_new("(s)", script), G_VARIANT_TYPE("(bs)"), G_DBUS_CALL_FLAGS_NONE, 10000, NULL, &error);
    g_assert_no_error(error);
    gboolean ok = FALSE; gchar *result = NULL;
    g_variant_get(reply, "(bs)", &ok, &result); g_variant_unref(reply);
    if (!ok) g_error("org.gnome.Shell.Eval failed: %s\n%s", result, script);
    return result;
}
static void cs_eval_printf(const gchar *format, ...) G_GNUC_PRINTF(1, 2);
static void cs_eval_printf(const gchar *format, ...)
{
    va_list args; va_start(args, format); gchar *script = g_strdup_vprintf(format, args); va_end(args);
    g_free(cs_eval(script)); g_free(script);
}
/* New virtual devices change the seat's capabilities; give clients time to
 * bind the new pointer and keyboard before the first event. */
static void cs_devices(void)
{
    static gboolean ready;
    if (ready) return;
    gchar *created = cs_eval("(() => { const C = imports.gi.Clutter; const seat = C.get_default_backend().get_default_seat();"
        " const fresh = !globalThis.radiosPointer || !globalThis.radiosKeyboard;"
        " globalThis.radiosPointer = globalThis.radiosPointer || seat.create_virtual_device(C.InputDeviceType.POINTER_DEVICE);"
        " globalThis.radiosKeyboard = globalThis.radiosKeyboard || seat.create_virtual_device(C.InputDeviceType.KEYBOARD_DEVICE);"
        " return fresh; })()");
    if (g_str_equal(created, "true")) {
        gint64 until = g_get_monotonic_time() + 500000;
        while (g_get_monotonic_time() < until) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
    }
    g_free(created); ready = TRUE;
}
static void cs_motion(gint x, gint y)
{
    if (!cs_wayland()) { XTestFakeMotionEvent(cs_xdisplay(), -1, x, y, CurrentTime); XFlush(cs_xdisplay()); return; }
    cs_devices();
    cs_eval_printf("radiosPointer.notify_absolute_motion(imports.gi.GLib.get_monotonic_time(), %d, %d)", x, y);
}
static void cs_button(guint button, gboolean press)
{
    if (!cs_wayland()) { XTestFakeButtonEvent(cs_xdisplay(), button, press, CurrentTime); XFlush(cs_xdisplay()); return; }
    cs_devices();
    cs_eval_printf("radiosPointer.notify_button(imports.gi.GLib.get_monotonic_time(), %u, %d)", button, press ? 1 : 0);
}
static void cs_key(guint keysym, gboolean press)
{
    if (!cs_wayland()) {
        XTestFakeKeyEvent(cs_xdisplay(), XKeysymToKeycode(cs_xdisplay(), keysym), press, CurrentTime); XFlush(cs_xdisplay()); return;
    }
    cs_devices();
    cs_eval_printf("radiosKeyboard.notify_keyval(imports.gi.GLib.get_monotonic_time(), %u, %d)", keysym, press ? 1 : 0);
}
static void cs_pointer(gint *x, gint *y)
{
    if (!cs_wayland()) {
        GdkDevice *device = gdk_seat_get_pointer(gdk_display_get_default_seat(gdk_display_get_default()));
        gdk_device_get_position(device, NULL, x, y); return;
    }
    gchar *json = cs_eval("global.get_pointer()");
    g_assert_cmpint(sscanf(json, "[%d,%d", x, y), ==, 2); g_free(json);
}
/* Screen position of a toplevel's (0,0): the compositor's surface rectangle
 * of the normal window with this title, or else of the only one of this
 * program. */
static void cs_window_origin(GtkWidget *window, gint *x, gint *y)
{
    if (!cs_wayland()) { gdk_window_get_origin(gtk_widget_get_window(window), x, y); return; }
    const gchar *title = gtk_window_get_title(GTK_WINDOW(window));
    gchar *wanted = g_strescape(title ? title : "", NULL), *app = g_strescape(g_get_prgname(), NULL);
    gchar *script = g_strdup_printf("(() => { const M = imports.gi.Meta; const all = global.get_window_actors().map(a => a.meta_window)"
        ".filter(w => w.get_window_type() === M.WindowType.NORMAL); let w = all.find(w => w.get_title() === \"%s\");"
        " if (!w) { const mine = all.filter(w => w.get_wm_class() === \"%s\"); if (mine.length === 1) w = mine[0]; }"
        " if (!w) return null; const r = w.get_buffer_rect(); return [r.x, r.y]; })()", wanted, app);
    gchar *json = cs_eval(script);
    g_assert_cmpint(sscanf(json, "[%d,%d", x, y), ==, 2);
    g_free(json); g_free(script); g_free(wanted); g_free(app);
}
/* Screen position of (x, y) in @window, any window of a toplevel, through the
 * offscreen windows of the content scale. */
static void cs_screen(GdkWindow *window, gint x, gint y, gint *sx, gint *sy)
{
    if (!cs_wayland()) { gdk_window_get_root_coords(window, x, y, sx, sy); return; }
    GdkWindow *top = gdk_window_get_effective_toplevel(window); gdouble fx = x, fy = y;
    for (GdkWindow *w = window; w && w != top; w = gdk_window_get_effective_parent(w)) gdk_window_coords_to_parent(w, fx, fy, &fx, &fy);
    gpointer owner = NULL; gdk_window_get_user_data(top, &owner); g_assert_true(GTK_IS_WINDOW(owner));
    gint ox = 0, oy = 0; cs_window_origin(GTK_WIDGET(owner), &ox, &oy);
    *sx = ox + (gint)lround(fx); *sy = oy + (gint)lround(fy);
}
/* The rectangle a widget occupies on screen. */
static GdkRectangle cs_drawn(GtkWidget *widget)
{
    GtkAllocation a; gtk_widget_get_allocation(widget, &a);
    gboolean own = gtk_widget_get_has_window(widget); GdkWindow *window = gtk_widget_get_window(widget);
    gint x1, y1, x2, y2;
    cs_screen(window, own ? 0 : a.x, own ? 0 : a.y, &x1, &y1);
    cs_screen(window, (own ? 0 : a.x) + a.width, (own ? 0 : a.y) + a.height, &x2, &y2);
    return (GdkRectangle){x1, y1, x2 - x1, y2 - y1};
}
/* The visible frame of a popup window (menu, combo list, tooltip). On
 * Wayland the newest popup surface of the compositor: its window geometry,
 * without client-side shadows, as the compositor placed it. */
static GdkRectangle cs_popup_frame(GtkWidget *popup)
{
    GdkRectangle frame = {0};
    if (!cs_wayland()) {
        gdk_window_get_origin(gtk_widget_get_window(popup), &frame.x, &frame.y);
        frame.width = gtk_widget_get_allocated_width(popup); frame.height = gtk_widget_get_allocated_height(popup);
        return frame;
    }
    gchar *json = cs_eval("(() => { const M = imports.gi.Meta; const ws = global.get_window_actors().map(a => a.meta_window)"
        ".filter(w => w.get_window_type() !== M.WindowType.NORMAL); if (!ws.length) return null;"
        " const r = ws[ws.length - 1].get_frame_rect(); return [r.x, r.y, r.width, r.height]; })()");
    g_assert_cmpint(sscanf(json, "[%d,%d,%d,%d", &frame.x, &frame.y, &frame.width, &frame.height), ==, 4); g_free(json);
    return frame;
}
/* Screen position of a widget shown in a popup window. */
static void cs_popup_origin(GtkWidget *widget, gint *x, gint *y)
{
    GtkWidget *toplevel = gtk_widget_get_toplevel(widget);
    if (!cs_wayland() || !GTK_IS_WINDOW(toplevel) || gtk_window_get_window_type(GTK_WINDOW(toplevel)) != GTK_WINDOW_POPUP) {
        GtkWidget *native = widget;
        while (!gtk_widget_get_has_window(native)) native = gtk_widget_get_parent(native);
        gint nx = 0, ny = 0; g_assert_true(gtk_widget_translate_coordinates(widget, native, 0, 0, &nx, &ny));
        cs_screen(gtk_widget_get_window(native), nx, ny, x, y); return;
    }
    GdkRectangle frame = cs_popup_frame(toplevel); GtkWidget *content = gtk_bin_get_child(GTK_BIN(toplevel));
    gint cx = 0, cy = 0;
    if (content && content != widget) g_assert_true(gtk_widget_translate_coordinates(widget, content, 0, 0, &cx, &cy));
    *x = frame.x + cx; *y = frame.y + cy;
}
#endif
