#define _POSIX_C_SOURCE 200809L
#include "radios/window.h"
#include "radios/preferences.h"
#include "radios/ui_preferences.h"
#include "radios/content_scale.h"
#include "radios/settings_sync.h"
#include "radios/schema_settings_ui.h"
#include "radios/transition_preset.h"
#include "content_scale_test_input.h"
#include <gdk/gdkx.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

typedef struct { gint x, y, width, height; } Bounds;
typedef struct {
    gchar *root;
    RadioSConfig config;
    GtkApplication *application;
    RadioSRuntime *runtime;
    RadioSWindow *ui;
    GtkWidget *window, *scale, *header, *logical, *probe;
    Bounds header_bounds;
    gint width, height, requested_width, requested_height;
    gboolean appearance_captured;
} Fixture;
static GtkApplication *shared_application;
static void pump(guint milliseconds)
{
    gint64 until = g_get_monotonic_time() + milliseconds * 1000;
    do { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); } while (g_get_monotonic_time() < until);
}
static GtkWidget *find(GtkWidget *root, const gchar *name, gboolean by_class)
{
    if (by_class ? gtk_style_context_has_class(gtk_widget_get_style_context(root), name) :
        g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = find(item->data, name, by_class);
    g_list_free(children); return found;
}
static GtkWidget *required(GtkWidget *root, const gchar *name)
{ GtkWidget *widget = find(root, name, FALSE); g_assert_nonnull(widget); return widget; }
static Bounds geometry(GtkWidget *widget, GtkWidget *ancestor)
{
    Bounds result; g_assert_true(gtk_widget_translate_coordinates(widget, ancestor, 0, 0, &result.x, &result.y));
    result.width = gtk_widget_get_allocated_width(widget); result.height = gtk_widget_get_allocated_height(widget); return result;
}
static void remove_private_tree(const gchar *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_SYMLINK)) { g_unlink(path); return; }
    if (!g_file_test(path, G_FILE_TEST_IS_DIR)) { g_unlink(path); return; }
    GDir *directory = g_dir_open(path, 0, NULL); g_assert_nonnull(directory); const gchar *name;
    while ((name = g_dir_read_name(directory))) { gchar *child = g_build_filename(path, name, NULL); remove_private_tree(child); g_free(child); }
    g_dir_close(directory); g_assert_cmpint(g_rmdir(path), ==, 0);
}
static gboolean probe_draw(GtkWidget *widget, cairo_t *cr, gpointer user)
{
    (void)user; cairo_set_source_rgb(cr, 1, 0, 1); cairo_rectangle(cr, 0, 0,
        gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget)); cairo_fill(cr); return FALSE;
}
static void header_unchanged(Fixture *f)
{
    Bounds header = geometry(f->header, f->window); gint width, height;
    gtk_window_get_size(GTK_WINDOW(f->window), &width, &height);
    g_assert_cmpint(width, ==, f->width); g_assert_cmpint(height, ==, f->height);
    g_assert_cmpint(header.x, ==, f->header_bounds.x); g_assert_cmpint(header.y, ==, f->header_bounds.y);
    g_assert_cmpint(header.width, ==, f->header_bounds.width); g_assert_cmpint(header.height, ==, f->header_bounds.height);
}
static void screenshot_and_actual_zoom(Fixture *f, guint percent)
{
    gtk_window_present(GTK_WINDOW(f->window)); pump(100);
    GdkPixbuf *image = gdk_pixbuf_get_from_window(gtk_widget_get_window(f->window), 0, 0, f->width, f->height);
    g_assert_nonnull(image); gint channels = gdk_pixbuf_get_n_channels(image), stride = gdk_pixbuf_get_rowstride(image);
    const guchar *pixels = gdk_pixbuf_read_pixels(image); Bounds color = {f->width, f->height, -1, -1};
    for (gint y = f->header_bounds.y + f->header_bounds.height; y < f->height; ++y) for (gint x = 0; x < f->width; ++x) {
        const guchar *p = pixels + y * stride + x * channels;
        if (p[0] < 250 || p[1] > 5 || p[2] < 250) continue;
        color.x = MIN(color.x, x); color.y = MIN(color.y, y); color.width = MAX(color.width, x); color.height = MAX(color.height, y);
    }
    g_assert_cmpint(color.width, >=, color.x); g_assert_cmpint(color.height, >=, color.y);
    color.width = color.width - color.x + 1; color.height = color.height - color.y + 1;
    gdouble ratio = percent / 100.0;
    g_test_message("actual painted body marker at %u%%: %dx%d, expected %.1fx%.1f; header %dx%d",
        percent, color.width, color.height, 80 * ratio, 18 * ratio, f->header_bounds.width, f->header_bounds.height);
    g_assert_cmpfloat(fabs(color.width - 80 * ratio), <=, 2); g_assert_cmpfloat(fabs(color.height - 18 * ratio), <=, 2);
    const gchar *capture = g_getenv("RADIOS_CONTENT_SCALE_CAPTURE_DIR");
    if (capture && *capture) {
        GError *error = NULL; g_assert_cmpint(g_mkdir_with_parents(capture, 0700), ==, 0);
        gchar *name = g_strdup_printf("content-scale-%u.png", percent), *path = g_build_filename(capture, name, NULL);
        g_assert_true(gdk_pixbuf_save(image, path, "png", &error, NULL)); g_assert_no_error(error); g_free(path); g_free(name);
    }
    g_object_unref(image); header_unchanged(f);
}
static void physical_body_click(Fixture *f, const gchar *name)
{
    GtkWidget *target = required(f->window, name); g_assert_true(gtk_widget_get_mapped(target));
    gint x, y, ox, oy; g_assert_true(gtk_widget_translate_coordinates(target, f->logical,
        gtk_widget_get_allocated_width(target) / 2, gtk_widget_get_allocated_height(target) / 2, &x, &y));
    gdk_window_get_origin(gtk_widget_get_window(f->scale), &ox, &oy);
    gdouble ratio = radios_content_scale_get_percent(f->scale) / 100.0;
    Display *display = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    gtk_window_present(GTK_WINDOW(f->window)); gdk_window_raise(gtk_widget_get_window(f->window)); pump(60);
    XTestFakeMotionEvent(display, -1, ox + (gint)lround(x * ratio), oy + (gint)lround(y * ratio), CurrentTime); XFlush(display); pump(30);
    XTestFakeButtonEvent(display, 1, TRUE, CurrentTime); XFlush(display); pump(30);
    XTestFakeButtonEvent(display, 1, FALSE, CurrentTime); XFlush(display); pump(120);
    if (GTK_IS_TOGGLE_BUTTON(target)) g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(target)));
}
static void short_window_body_scroll_reaches_footer(Fixture *f)
{
    GtkWidget *viewport = required(f->window, "main-content-viewport"); g_assert_true(GTK_IS_SCROLLED_WINDOW(viewport));
    GtkWidget *main = find(f->logical, "main-interface", TRUE), *stack = NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(main));
    for (GList *i = children; i; i = i->next) if (GTK_IS_STACK(i->data)) stack = i->data;
    g_list_free(children); g_assert_nonnull(stack);
    g_test_message("short-window actual profile viewport height: %d logical pixels", gtk_widget_get_allocated_height(stack));
    g_assert_cmpint(gtk_widget_get_allocated_height(stack), >=, 48);
    GtkAdjustment *vertical = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(viewport));
    gdouble maximum = gtk_adjustment_get_upper(vertical) - gtk_adjustment_get_page_size(vertical);
    g_assert_cmpfloat(maximum, >, 1);
    /* The outer viewport's left margin is outside every inner page scroller.
     * Real wheel events there must pan the complete body to its footer. */
    gint ox, oy; gdk_window_get_origin(gtk_widget_get_window(f->scale), &ox, &oy);
    gdouble ratio = radios_content_scale_get_percent(f->scale) / 100.0;
    Display *display = GDK_DISPLAY_XDISPLAY(gdk_display_get_default());
    XTestFakeMotionEvent(display, -1, ox + (gint)ceil(2 * ratio),
        oy + gtk_widget_get_allocated_height(f->scale) / 2, CurrentTime); XFlush(display); pump(60);
    for (guint i = 0; i < 30 && gtk_adjustment_get_value(vertical) < maximum - 1; ++i) {
        XTestFakeButtonEvent(display, 5, TRUE, CurrentTime); XTestFakeButtonEvent(display, 5, FALSE, CurrentTime);
        XFlush(display); pump(35);
    }
    g_assert_cmpfloat(gtk_adjustment_get_value(vertical), >=, maximum - 1);
    physical_body_click(f, "header-settings");
    GList *windows = gtk_window_list_toplevels(); GtkWidget *preferences = NULL;
    for (GList *i = windows; i && !preferences; i = i->next)
        if (g_str_equal(gtk_widget_get_name(i->data), "preferences-window") && gtk_widget_get_visible(i->data)) preferences = i->data;
    g_list_free(windows); g_assert_nonnull(preferences);
    gtk_widget_destroy(preferences); gtk_adjustment_set_value(vertical, 0); pump(100); header_unchanged(f);
}
static GtkWidget *appearance(Fixture *f)
{
    GtkWidget *preferences = radios_preferences_open(radios_window_view_context(f->ui)); g_assert_nonnull(preferences);
    GtkWidget *categories = required(preferences, "preferences-categories");
    gtk_list_box_select_row(GTK_LIST_BOX(categories), GTK_LIST_BOX_ROW(required(preferences, "preferences-category-view"))); pump(80);
    g_assert_true(gtk_widget_get_mapped(required(preferences, "preferences-appearance")));
    GtkWidget *slider = required(preferences, "preferences-content-scale"), *spin = required(preferences, "preferences-content-scale-percent");
    g_assert_true(GTK_IS_SCALE(slider)); g_assert_true(GTK_IS_SPIN_BUTTON(spin));
    GtkAdjustment *adjustment = gtk_range_get_adjustment(GTK_RANGE(slider));
    g_assert_true(adjustment == gtk_spin_button_get_adjustment(GTK_SPIN_BUTTON(spin)));
    g_assert_cmpfloat(gtk_adjustment_get_lower(adjustment), ==, 50); g_assert_cmpfloat(gtk_adjustment_get_upper(adjustment), ==, 150);
    const gchar *names[] = {"preferences-mixer-scale", "preferences-mixer-height", "preferences-effector-scale", "preferences-effector-height"};
    GtkAdjustment *previous = adjustment;
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkWidget *panel_slider = required(preferences, names[i]);
        gchar *spin_name = g_strdup_printf("%s-percent", names[i]);
        GtkWidget *panel_spin = required(preferences, spin_name); g_free(spin_name);
        g_assert_true(GTK_IS_SCALE(panel_slider)); g_assert_true(GTK_IS_SPIN_BUTTON(panel_spin));
        GtkAdjustment *panel_adjustment = gtk_range_get_adjustment(GTK_RANGE(panel_slider));
        g_assert_true(panel_adjustment == gtk_spin_button_get_adjustment(GTK_SPIN_BUTTON(panel_spin)));
        g_assert_true(panel_adjustment != adjustment); g_assert_true(panel_adjustment != previous);
        g_assert_cmpfloat(gtk_adjustment_get_lower(panel_adjustment), ==, 50);
        g_assert_cmpfloat(gtk_adjustment_get_upper(panel_adjustment), ==, 150); previous = panel_adjustment;
    }
    const gchar *capture = g_getenv("RADIOS_CONTENT_SCALE_CAPTURE_DIR");
    if (capture && *capture && !f->appearance_captured) {
        GError *error = NULL; g_assert_cmpint(g_mkdir_with_parents(capture, 0700), ==, 0);
        GdkPixbuf *image = gdk_pixbuf_get_from_window(gtk_widget_get_window(preferences), 0, 0,
            gtk_widget_get_allocated_width(preferences), gtk_widget_get_allocated_height(preferences));
        g_assert_nonnull(image); gchar *path = g_build_filename(capture, "appearance.png", NULL);
        g_assert_true(gdk_pixbuf_save(image, path, "png", &error, NULL)); g_assert_no_error(error);
        g_free(path); g_object_unref(image); f->appearance_captured = TRUE;
    }
    return preferences;
}
static void change_percent(Fixture *f, guint percent, gboolean slider)
{
    GtkWidget *preferences = appearance(f);
    if (slider) gtk_range_set_value(GTK_RANGE(required(preferences, "preferences-content-scale")), percent);
    else gtk_spin_button_set_value(GTK_SPIN_BUTTON(required(preferences, "preferences-content-scale-percent")), percent);
    pump(120); g_assert_cmpuint(radios_content_scale_get_percent(f->scale), ==, percent);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(required(preferences, "preferences-content-scale-percent"))), ==, (gint)percent);
    RadioSUIPreferences persisted; GError *error = NULL;
    g_assert_true(radios_ui_preferences_load(f->root, &persisted, &error)); g_assert_no_error(error);
    g_assert_cmpuint(persisted.content_scale_percent, ==, percent);
    g_assert_cmpuint(persisted.text_size, ==, 1); g_assert_cmpuint(persisted.header_hidden_fields, ==, 0);
    gtk_widget_destroy(preferences); pump(150);
}
static void panel_sizes(Fixture *f, guint mixer_width, guint mixer_height, guint effector_width, guint effector_height)
{
    GtkWidget *mixer_scale = required(f->window, "dj-mixer-scale"), *effector_scale = required(f->window, "dj-effector-scale");
    g_assert_true(RADIOS_IS_CONTENT_SCALE(mixer_scale)); g_assert_true(RADIOS_IS_CONTENT_SCALE(effector_scale));
    g_assert_cmpuint(radios_content_scale_get_percent(mixer_scale), ==, mixer_width);
    g_assert_cmpuint(radios_content_scale_get_vertical_percent(mixer_scale), ==, mixer_height);
    g_assert_cmpuint(radios_content_scale_get_percent(effector_scale), ==, effector_width);
    g_assert_cmpuint(radios_content_scale_get_vertical_percent(effector_scale), ==, effector_height);
}
static void assert_persisted_panels(Fixture *f, guint mixer_width, guint mixer_height, guint effector_width, guint effector_height)
{
    RadioSUIPreferences persisted; GError *error = NULL;
    g_assert_true(radios_ui_preferences_load(f->root, &persisted, &error)); g_assert_no_error(error);
    g_assert_cmpuint(persisted.mixer_scale_percent, ==, mixer_width); g_assert_cmpuint(persisted.mixer_height_percent, ==, mixer_height);
    g_assert_cmpuint(persisted.effector_scale_percent, ==, effector_width); g_assert_cmpuint(persisted.effector_height_percent, ==, effector_height);
}
static void independent_panel_preferences(Fixture *f)
{
    GtkWidget *preferences = appearance(f);
    GtkWidget *mixer = required(preferences, "preferences-mixer-scale");
    GtkWidget *effector = required(preferences, "preferences-effector-scale-percent");
    GtkWidget *mixer_height = required(preferences, "preferences-mixer-height-percent");
    GtkWidget *effector_height = required(preferences, "preferences-effector-height");
    guint content_percent = radios_content_scale_get_percent(f->scale);
    panel_sizes(f, 100, 100, 100, 100);
    gtk_range_set_value(GTK_RANGE(mixer), 150); pump(100); panel_sizes(f, 150, 100, 100, 100);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(effector), 75); pump(100); panel_sizes(f, 150, 100, 75, 100);
    g_assert_cmpuint(radios_content_scale_get_percent(f->scale), ==, content_percent);
    RadioSUIPreferences persisted; GError *error = NULL;
    g_assert_true(radios_ui_preferences_load(f->root, &persisted, &error)); g_assert_no_error(error);
    g_assert_cmpuint(persisted.mixer_scale_percent, ==, 150); g_assert_cmpuint(persisted.effector_scale_percent, ==, 75);
    g_assert_cmpuint(persisted.content_scale_percent, ==, content_percent);
    assert_persisted_panels(f, 150, 100, 75, 100);
    /* Each height has its own control and key; a width stays as it is. */
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(mixer_height), 140); pump(100); panel_sizes(f, 150, 140, 75, 100);
    gtk_range_set_value(GTK_RANGE(effector_height), 60); pump(100); panel_sizes(f, 150, 140, 75, 60);
    assert_persisted_panels(f, 150, 140, 75, 60);
    g_assert_cmpuint(radios_content_scale_get_percent(f->scale), ==, content_percent);
    gtk_button_clicked(GTK_BUTTON(required(preferences, "preferences-mixer-scale-reset"))); pump(80);
    panel_sizes(f, 100, 140, 75, 60);
    gtk_button_clicked(GTK_BUTTON(required(preferences, "preferences-effector-scale-reset"))); pump(80);
    panel_sizes(f, 100, 140, 100, 60);
    gtk_button_clicked(GTK_BUTTON(required(preferences, "preferences-mixer-height-reset"))); pump(80);
    panel_sizes(f, 100, 100, 100, 60);
    gtk_button_clicked(GTK_BUTTON(required(preferences, "preferences-effector-height-reset"))); pump(80);
    panel_sizes(f, 100, 100, 100, 100); assert_persisted_panels(f, 100, 100, 100, 100);
    gtk_range_set_value(GTK_RANGE(mixer), 125); gtk_spin_button_set_value(GTK_SPIN_BUTTON(effector), 75); pump(100);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(required(preferences, "preferences-content-scale-percent")), 150); pump(100);
    panel_sizes(f, 125, 100, 75, 100); g_assert_cmpuint(radios_content_scale_get_percent(f->scale), ==, 150);
    g_assert_true(radios_ui_preferences_load(f->root, &persisted, &error)); g_assert_no_error(error);
    g_assert_cmpuint(persisted.content_scale_percent, ==, 150);
    g_assert_cmpuint(persisted.mixer_scale_percent, ==, 125); g_assert_cmpuint(persisted.effector_scale_percent, ==, 75);
    /* A settings sync updates existing controls and both panel wrappers in
     * one application, without losing the global content scale or UI fields. */
    persisted.content_scale_percent = content_percent;
    persisted.mixer_scale_percent = 50; persisted.effector_scale_percent = 150;
    persisted.mixer_height_percent = 70; persisted.effector_height_percent = 130;
    RadioSPreferencesAudio audio;
    g_assert_true(radios_preferences_audio_load(f->root, &audio, &error)); g_assert_no_error(error);
    g_assert_true(radios_settings_sync_apply(radios_window_view_context(f->ui), &persisted, &audio, &error)); g_assert_no_error(error);
    pump(100); panel_sizes(f, 50, 70, 150, 130);
    g_assert_cmpuint(radios_content_scale_get_percent(f->scale), ==, content_percent);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(required(preferences, "preferences-mixer-scale-percent"))), ==, 50);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(effector)), ==, 150);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(mixer_height)), ==, 70);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(required(preferences, "preferences-effector-height-percent"))), ==, 130);
    /* A later ordinary appearance save must keep the values accepted above. */
    gtk_button_clicked(GTK_BUTTON(required(preferences, "preferences-appearance-save"))); pump(80);
    g_assert_true(radios_ui_preferences_load(f->root, &persisted, &error)); g_assert_no_error(error);
    g_assert_cmpuint(persisted.content_scale_percent, ==, content_percent);
    g_assert_cmpuint(persisted.mixer_scale_percent, ==, 50); g_assert_cmpuint(persisted.effector_scale_percent, ==, 150);
    g_assert_cmpuint(persisted.mixer_height_percent, ==, 70); g_assert_cmpuint(persisted.effector_height_percent, ==, 130);
    g_assert_cmpuint(persisted.text_size, ==, 1); g_assert_cmpuint(persisted.header_hidden_fields, ==, 0);
    gtk_widget_destroy(preferences); pump(100);
}
static void bind_window(Fixture *f)
{
    GError *error = NULL; f->ui = radios_window_new(f->application, &f->config, f->runtime, FALSE, NULL, &error);
    g_assert_no_error(error); g_assert_nonnull(f->ui); f->window = GTK_WIDGET(radios_window_view_context(f->ui)->parent);
    gtk_window_set_default_size(GTK_WINDOW(f->window), f->requested_width, f->requested_height); radios_window_present(f->ui); pump(200);
    /* Responsive header requisitions settle after compact layout is applied.
     * Continue the same resize gesture, as the existing live-layout test does. */
    for (guint attempt = 0; attempt < 4; ++attempt) {
        gtk_window_resize(GTK_WINDOW(f->window), f->requested_width, f->requested_height); pump(180);
        gint width, height; gtk_window_get_size(GTK_WINDOW(f->window), &width, &height);
        if (width == f->requested_width && height == f->requested_height) break;
    }
    f->scale = required(f->window, "main-content-scale"); f->header = required(f->window, "shared-header");
    g_assert_true(RADIOS_IS_CONTENT_SCALE(f->scale)); f->logical = gtk_bin_get_child(GTK_BIN(f->scale)); g_assert_nonnull(f->logical);
    g_assert_false(gtk_widget_is_ancestor(f->header, f->scale));
    GtkWidget *footer = required(f->window, "status-footer"); g_assert_true(gtk_widget_is_ancestor(footer, f->scale));
    GtkWidget *main = find(f->logical, "main-interface", TRUE); g_assert_true(GTK_IS_BOX(main));
    f->probe = gtk_drawing_area_new(); gtk_widget_set_size_request(f->probe, 80, 18); gtk_widget_set_halign(f->probe, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(main), f->probe, FALSE, FALSE, 0); gtk_box_reorder_child(GTK_BOX(main), f->probe, 1);
    g_signal_connect(f->probe, "draw", G_CALLBACK(probe_draw), NULL); gtk_widget_show(f->probe); pump(180);
    f->header_bounds = geometry(f->header, f->window); gtk_window_get_size(GTK_WINDOW(f->window), &f->width, &f->height);
    g_assert_cmpint(f->width, ==, f->requested_width); g_assert_cmpint(f->height, ==, f->requested_height);
}
static void scale_preferences_header_input_and_reopen(void)
{
    Fixture f = {0};
    const gchar *width = g_getenv("RADIOS_CONTENT_SCALE_WINDOW_WIDTH"), *height = g_getenv("RADIOS_CONTENT_SCALE_WINDOW_HEIGHT");
    f.requested_width = width ? (gint)g_ascii_strtoll(width, NULL, 10) : 960;
    f.requested_height = height ? (gint)g_ascii_strtoll(height, NULL, 10) : 720;
    g_assert_cmpint(f.requested_width, >=, 600); g_assert_cmpint(f.requested_width, <=, 2000);
    g_assert_cmpint(f.requested_height, >=, 600); g_assert_cmpint(f.requested_height, <=, 1200);
    GError *error = NULL; f.root = g_dir_make_tmp("radios-content-scale-ui-XXXXXX", &error); g_assert_no_error(error);
    gchar *config_directory = g_build_filename(f.root, "config", NULL); g_assert_cmpint(g_mkdir(config_directory, 0700), ==, 0);
    gchar *example = g_build_filename(RADIOS_TEST_PROJECT_ROOT, "config", "radios.example.ini", NULL), *text = NULL; gsize length;
    g_assert_true(g_file_get_contents(example, &text, &length, &error)); g_assert_no_error(error);
    gchar *configuration = g_build_filename(config_directory, "radios.ini", NULL); g_assert_true(g_file_set_contents(configuration, text, length, &error)); g_assert_no_error(error);
    gchar *assets = g_build_filename(RADIOS_TEST_PROJECT_ROOT, "assets", NULL), *link = g_build_filename(f.root, "assets", NULL);
    g_assert_cmpint(symlink(assets, link), ==, 0); g_free(assets); g_free(link); g_free(text); g_free(example); g_free(configuration); g_free(config_directory);
    /* Load trusted read-only resources from the real source tree, while all
     * preference, database and output destinations belong to this fixture. */
    gboolean loaded = radios_config_load(RADIOS_TEST_PROJECT_ROOT, "config/radios.example.ini", &f.config, &error);
    g_assert_no_error(error); g_assert_true(loaded);
    g_free(f.config.project_root); f.config.project_root = g_strdup(f.root);
    gchar **destinations[] = {&f.config.config_file, &f.config.database_file, &f.config.endpoint_root,
        &f.config.import_root, &f.config.cache_root, &f.config.log_root, &f.config.recording_root, &f.config.backup_root};
    const gchar *relative[] = {"config/radios.ini", "data/database/RadioS.sdb", "data/endpoints", "data/imports",
        "data/cache", "data/logs", "data/recordings", "data/backups"};
    for (guint i = 0; i < G_N_ELEMENTS(destinations); ++i) {
        g_free(*destinations[i]); *destinations[i] = g_build_filename(f.root, relative[i], NULL);
    }
    f.application = shared_application;
    f.runtime = radios_runtime_new(32, 64); bind_window(&f); g_assert_cmpuint(radios_content_scale_get_percent(f.scale), ==, 100);
    panel_sizes(&f, 100, 100, 100, 100);
    screenshot_and_actual_zoom(&f, 100);
    change_percent(&f, 50, TRUE); screenshot_and_actual_zoom(&f, 50); physical_body_click(&f, "profile-automation-tab");
    change_percent(&f, 75, FALSE); screenshot_and_actual_zoom(&f, 75); physical_body_click(&f, "profile-playlist-tab");
    change_percent(&f, 150, TRUE); screenshot_and_actual_zoom(&f, 150); physical_body_click(&f, "profile-broadcast-tab");
    if (f.requested_width <= 600 && f.requested_height <= 650) short_window_body_scroll_reaches_footer(&f);
    GtkWidget *preferences = appearance(&f); gtk_button_clicked(GTK_BUTTON(required(preferences, "preferences-content-scale-reset")));
    pump(100); g_assert_cmpuint(radios_content_scale_get_percent(f.scale), ==, 100); gtk_widget_destroy(preferences); pump(100);
    screenshot_and_actual_zoom(&f, 100);
    change_percent(&f, 125, FALSE); physical_body_click(&f, "profile-playlist-tab");
    radios_window_free(f.ui); f.ui = NULL; pump(100); bind_window(&f);
    g_assert_cmpuint(radios_content_scale_get_percent(f.scale), ==, 125); screenshot_and_actual_zoom(&f, 125);
    independent_panel_preferences(&f);
    radios_window_free(f.ui); f.ui = NULL; pump(100); bind_window(&f);
    g_assert_cmpuint(radios_content_scale_get_percent(f.scale), ==, 125); panel_sizes(&f, 50, 70, 150, 130);
    preferences = appearance(&f); g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(required(preferences, "preferences-content-scale-percent"))), ==, 125);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(required(preferences, "preferences-mixer-scale-percent"))), ==, 50);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(required(preferences, "preferences-effector-scale-percent"))), ==, 150);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(required(preferences, "preferences-mixer-height-percent"))), ==, 70);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(required(preferences, "preferences-effector-height-percent"))), ==, 130);
    /* Closing the application with the synced Appearance controls alive must
     * disconnect them and retain the last accepted percentage. */
    radios_window_free(f.ui); pump(150); radios_runtime_shutdown(f.runtime); radios_runtime_free(f.runtime);
    radios_config_clear(&f.config); remove_private_tree(f.root); g_free(f.root);
}
static GtkWidget *mixer_size_toplevel(const gchar *name)
{
    GList *windows = gtk_window_list_toplevels(); GtkWidget *found = NULL;
    for (GList *item = windows; item && !found; item = item->next)
        if (!g_strcmp0(gtk_widget_get_name(item->data), name)) found = item->data;
    g_list_free(windows); return found;
}
/* The mixer strips share their row at every zoom: channels and MASTER are
 * equally wide and no gap opens between the last channel and MASTER. */
static void strips_share_row(GtkWidget *root)
{
    GtkWidget *first = required(root, "dj-channel-1"), *master = required(root, "dj-channel-0");
    GtkWidget *channels = gtk_widget_get_parent(first);
    GList *strips = gtk_container_get_children(GTK_CONTAINER(channels));
    gint count = (gint)g_list_length(strips), width = gtk_widget_get_allocated_width(first);
    for (GList *item = strips; item; item = item->next) g_assert_cmpint(gtk_widget_get_allocated_width(item->data), ==, width);
    g_list_free(strips);
    g_assert_cmpint(gtk_widget_get_allocated_width(master), ==, width);
    gint needed = count * width + (count - 1) * gtk_box_get_spacing(GTK_BOX(channels)), gap = gtk_widget_get_allocated_width(channels) - needed;
    g_test_message("strips %d x %d, gap before MASTER %d", count, width, gap);
    g_assert_cmpint(gap, >=, 0); g_assert_cmpint(gap, <=, count);
}
/* Each section zooms on two axes of its own: a height percentage changes
 * only that section's physical height (its logical layout and the other
 * section stay as they are), and a width percentage changes only its logical
 * layout width, never a height. */
static void sections_two_axis_geometry(Fixture *f, GtkWidget *mixer, GtkWidget *effector)
{
    GtkWidget *preferences = appearance(f);
    const gchar *spins[] = {"preferences-mixer-scale-percent", "preferences-mixer-height-percent",
        "preferences-effector-scale-percent", "preferences-effector-height-percent"};
    const guint steps[][4] = {{100, 100, 100, 100}, {100, 150, 100, 100}, {100, 150, 100, 50}, {100, 100, 100, 50},
        {150, 100, 50, 100}, {50, 100, 150, 100}, {100, 100, 100, 100}};
    GtkWidget *sections[] = {mixer, effector};
    gint logical_width[2] = {0, 0}, logical_height[2] = {0, 0};
    for (guint step = 0; step < G_N_ELEMENTS(steps); ++step) {
        for (guint i = 0; i < G_N_ELEMENTS(spins); ++i)
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(required(preferences, spins[i])), steps[step][i]);
        pump(350);
        panel_sizes(f, steps[step][0], steps[step][1], steps[step][2], steps[step][3]);
        for (guint s = 0; s < G_N_ELEMENTS(sections); ++s) {
            GtkWidget *child = gtk_bin_get_child(GTK_BIN(sections[s]));
            guint width_percent = steps[step][2 * s], height_percent = steps[step][2 * s + 1];
            gint physical_width = gtk_widget_get_allocated_width(sections[s]), physical_height = gtk_widget_get_allocated_height(sections[s]);
            gint child_width = gtk_widget_get_allocated_width(child), child_height = gtk_widget_get_allocated_height(child);
            gint natural = 0; gtk_widget_get_preferred_width(child, NULL, &natural);
            g_test_message("section %u step %u %u%% x %u%%: physical %dx%d, logical %dx%d, natural width %d", s, step,
                width_percent, height_percent, physical_width, physical_height, child_width, child_height, natural);
            if (step == 0) {
                logical_width[s] = child_width; logical_height[s] = child_height;
                g_assert_cmpint(physical_height, ==, child_height);
            }
            /* The physical height is the logical height zoomed on the vertical axis only. */
            g_assert_cmpint(ABS(physical_height - (gint)ceil(child_height * height_percent / 100.0)), <=, 2);
            /* A height percentage changes no logical length of the section. */
            if (width_percent == 100) {
                g_assert_cmpint(child_width, ==, logical_width[s]);
                g_assert_cmpint(ABS(child_height - logical_height[s]), <=, 1);
            }
            /* The width percentage alone sets the logical layout width from the row. */
            gint zoomed = MAX(1, (gint)floor(MAX(1, physical_width) / (width_percent / 100.0)));
            g_assert_cmpint(child_width, ==, MAX(MAX(1, natural), MIN(zoomed, MAX(1, physical_width))));
        }
    }
    assert_persisted_panels(f, 100, 100, 100, 100);
    gtk_widget_destroy(preferences); pump(100);
}
static void mixer_size_visible_rapid_nested_detach_and_close(void)
{

    Fixture f = {0};
    const gchar *width = g_getenv("RADIOS_CONTENT_SCALE_WINDOW_WIDTH"), *height = g_getenv("RADIOS_CONTENT_SCALE_WINDOW_HEIGHT");
    f.requested_width = width ? (gint)g_ascii_strtoll(width, NULL, 10) : 960;
    f.requested_height = height ? (gint)g_ascii_strtoll(height, NULL, 10) : 720;
    g_assert_cmpint(f.requested_width, >=, 600); g_assert_cmpint(f.requested_width, <=, 2000);
    g_assert_cmpint(f.requested_height, >=, 600); g_assert_cmpint(f.requested_height, <=, 1200);
    GError *error = NULL; f.root = g_dir_make_tmp("radios-content-scale-ui-XXXXXX", &error); g_assert_no_error(error);
    gchar *config_directory = g_build_filename(f.root, "config", NULL); g_assert_cmpint(g_mkdir(config_directory, 0700), ==, 0);
    gchar *example = g_build_filename(RADIOS_TEST_PROJECT_ROOT, "config", "radios.example.ini", NULL), *text = NULL; gsize length;
    g_assert_true(g_file_get_contents(example, &text, &length, &error)); g_assert_no_error(error);
    gchar *configuration = g_build_filename(config_directory, "radios.ini", NULL); g_assert_true(g_file_set_contents(configuration, text, length, &error)); g_assert_no_error(error);
    gchar *assets = g_build_filename(RADIOS_TEST_PROJECT_ROOT, "assets", NULL), *link = g_build_filename(f.root, "assets", NULL);
    g_assert_cmpint(symlink(assets, link), ==, 0); g_free(assets); g_free(link); g_free(text); g_free(example); g_free(configuration); g_free(config_directory);
    /* Load trusted read-only resources from the real source tree, while all
     * preference, database and output destinations belong to this fixture. */
    gboolean loaded = radios_config_load(RADIOS_TEST_PROJECT_ROOT, "config/radios.example.ini", &f.config, &error);
    g_assert_no_error(error); g_assert_true(loaded);
    g_free(f.config.project_root); f.config.project_root = g_strdup(f.root);
    gchar **destinations[] = {&f.config.config_file, &f.config.database_file, &f.config.endpoint_root,
        &f.config.import_root, &f.config.cache_root, &f.config.log_root, &f.config.recording_root, &f.config.backup_root};
    const gchar *relative[] = {"config/radios.ini", "data/database/RadioS.sdb", "data/endpoints", "data/imports",
        "data/cache", "data/logs", "data/recordings", "data/backups"};
    for (guint i = 0; i < G_N_ELEMENTS(destinations); ++i) {
        g_free(*destinations[i]); *destinations[i] = g_build_filename(f.root, relative[i], NULL);
    }
    f.application = shared_application;
    f.runtime = radios_runtime_new(32, 64); bind_window(&f); g_assert_cmpuint(radios_content_scale_get_percent(f.scale), ==, 100);

    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(required(f.window, "profile-dj-tab")), TRUE); pump(250);
    GtkWidget *mixer = required(f.window, "dj-mixer-scale"), *effector = required(f.window, "dj-effector-scale");
    g_assert_true(gtk_widget_get_mapped(mixer)); g_assert_true(gtk_widget_get_realized(mixer));
    g_assert_true(gtk_widget_get_realized(effector));
    sections_two_axis_geometry(&f, mixer, effector);
    for (guint stage = 0; stage < 3; ++stage) {
        GtkWidget *detached = NULL;
        if (stage == 1) {
            gtk_button_clicked(GTK_BUTTON(required(f.window, "dj-mixer-window-toggle"))); pump(150);
            detached = mixer_size_toplevel("dj-mixer-window"); g_assert_nonnull(detached);
            g_assert_true(find(detached, "dj-mixer-scale", FALSE) == mixer);
        }
        GtkWidget *preferences = appearance(&f);
        GtkWidget *slider = required(preferences, "preferences-mixer-scale");
        GtkWidget *spin = required(preferences, "preferences-mixer-scale-percent");
        for (guint i = 0; i < 32; ++i) {
            guint percent = i % 2 ? 150 : 50;
            if (i % 3) gtk_range_set_value(GTK_RANGE(slider), percent);
            else gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), percent);
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(required(preferences, "preferences-effector-scale-percent")), 200 - percent);
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(required(preferences, "preferences-content-scale-percent")), i % 4 ? 150 : 50);
            pump(12);
            g_assert_cmpuint(radios_content_scale_get_percent(mixer), ==, percent);
            g_assert_cmpuint(radios_content_scale_get_percent(effector), ==, 200 - percent);
            g_assert_true(gtk_widget_get_realized(mixer));
            g_assert_cmpint(gtk_widget_get_allocated_width(mixer), >, 1);
            g_assert_cmpint(gtk_widget_get_allocated_height(mixer), >, 1);
            GtkWidget *section_scales[] = {mixer, effector};
            for (guint section = 0; section < G_N_ELEMENTS(section_scales); ++section) {
                GtkWidget *logical = gtk_bin_get_child(GTK_BIN(section_scales[section]));
                gint minimum_width = 0; gtk_widget_get_preferred_width(logical, &minimum_width, NULL);
                g_test_message("visible section %u stage %u zoom%u logical width%d minimum%d", section, stage,
                    radios_content_scale_get_percent(section_scales[section]), gtk_widget_get_allocated_width(logical), minimum_width);
                g_assert_cmpint(gtk_widget_get_allocated_width(logical), >=, minimum_width);
            }
        }
        gtk_range_set_value(GTK_RANGE(slider), 100);
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(required(preferences, "preferences-content-scale-percent")), 100);
        pump(80);
        const guint share_percents[] = {50, 100, 75, 125, 100};
        for (guint i = 0; i < G_N_ELEMENTS(share_percents); ++i) {
            gtk_spin_button_set_value(GTK_SPIN_BUTTON(spin), share_percents[i]); pump(300);
            strips_share_row(detached ? detached : f.window);
        }
        GtkWidget *scroller = detached ?
 required(detached, "dj-mixer-window-scroll") : required(f.window, "dj-task-view");
        g_assert_true(GTK_IS_SCROLLED_WINDOW(scroller));
        GtkAdjustment *horizontal = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(scroller));
        gdouble end = MAX(0, gtk_adjustment_get_upper(horizontal) - gtk_adjustment_get_page_size(horizontal));
        g_assert_cmpfloat(gtk_adjustment_get_upper(horizontal), >=, gtk_widget_get_allocated_width(mixer));
        gtk_adjustment_set_value(horizontal, end); pump(40);
        g_assert_cmpfloat(gtk_adjustment_get_value(horizontal), ==, end);
        gtk_adjustment_set_value(horizontal, 0);
        if (detached) {
            gtk_widget_destroy(detached); pump(150);
            g_assert_null(mixer_size_toplevel("dj-mixer-window"));
            g_assert_true(find(f.window, "dj-mixer-scale", FALSE) == mixer);
            g_assert_true(gtk_widget_get_realized(mixer));
        }
        gtk_widget_destroy(preferences); pump(80);
    }
    /* Closing with both independent windows alive exercises the offscreen
     * unrealize/reparent path and adjustment callbacks during owner teardown. */
    gtk_button_clicked(GTK_BUTTON(required(f.window, "dj-mixer-window-toggle"))); pump(100);
    GtkWidget *preferences = appearance(&f);
    gtk_range_set_value(GTK_RANGE(required(preferences, "preferences-mixer-scale")), 150); pump(50);
    radios_window_free(f.ui); pump(150); radios_runtime_shutdown(f.runtime); radios_runtime_free(f.runtime);
    radios_config_clear(&f.config); remove_private_tree(f.root); g_free(f.root);
}
/* Where a widget is drawn on screen, through the offscreen windows (X11 or,
 * with a private headless GNOME Shell, Wayland: content_scale_test_input.h). */
static GdkRectangle drawn_rect(GtkWidget *widget) { return cs_drawn(widget); }
/* A private project root and the real application window, as in the cases above. */
static void fixture_open(Fixture *f, gint width, gint height)
{
    f->requested_width = width; f->requested_height = height;
    GError *error = NULL; f->root = g_dir_make_tmp("radios-content-scale-ui-XXXXXX", &error); g_assert_no_error(error);
    gchar *config_directory = g_build_filename(f->root, "config", NULL); g_assert_cmpint(g_mkdir(config_directory, 0700), ==, 0);
    gchar *example = g_build_filename(RADIOS_TEST_PROJECT_ROOT, "config", "radios.example.ini", NULL), *text = NULL; gsize length;
    g_assert_true(g_file_get_contents(example, &text, &length, &error)); g_assert_no_error(error);
    gchar *configuration = g_build_filename(config_directory, "radios.ini", NULL);
    g_assert_true(g_file_set_contents(configuration, text, length, &error)); g_assert_no_error(error);
    gchar *assets = g_build_filename(RADIOS_TEST_PROJECT_ROOT, "assets", NULL), *link = g_build_filename(f->root, "assets", NULL);
    g_assert_cmpint(symlink(assets, link), ==, 0);
    g_free(assets); g_free(link); g_free(text); g_free(example); g_free(configuration); g_free(config_directory);
    gboolean loaded = radios_config_load(RADIOS_TEST_PROJECT_ROOT, "config/radios.example.ini", &f->config, &error);
    g_assert_no_error(error); g_assert_true(loaded);
    g_free(f->config.project_root); f->config.project_root = g_strdup(f->root);
    gchar **destinations[] = {&f->config.config_file, &f->config.database_file, &f->config.endpoint_root,
        &f->config.import_root, &f->config.cache_root, &f->config.log_root, &f->config.recording_root, &f->config.backup_root};
    const gchar *relative[] = {"config/radios.ini", "data/database/RadioS.sdb", "data/endpoints", "data/imports",
        "data/cache", "data/logs", "data/recordings", "data/backups"};
    for (guint i = 0; i < G_N_ELEMENTS(destinations); ++i) {
        g_free(*destinations[i]); *destinations[i] = g_build_filename(f->root, relative[i], NULL);
    }
    f->application = shared_application; f->runtime = radios_runtime_new(32, 64); bind_window(f);
}
static void fixture_close(Fixture *f)
{
    radios_window_free(f->ui); pump(150); radios_runtime_shutdown(f->runtime); radios_runtime_free(f->runtime);
    radios_config_clear(&f->config); remove_private_tree(f->root); g_free(f->root);
}
/* R3.5 / PDF Şekil 18: in the real window, at the saved 73% zoom and at 100%
 * and 125%, a right click in the scrolled automation calendar opens the block
 * menu (Kopyala / Yapıştır / Sil / Veritabanından yeniden yükle) at the
 * pointer instead of at the unscaled, unscrolled place of the cell. */
static void calendar_block_menu_opens_at_pointer(void)
{
    Fixture f = {0}; fixture_open(&f, 1800, 1150);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(required(f.window, "profile-automation-tab")), TRUE); pump(300);
    GtkWidget *grid = required(f.window, "automation-calendar-grid"); g_assert_true(gtk_widget_get_mapped(grid));
    GtkWidget *scroll = gtk_widget_get_ancestor(grid, GTK_TYPE_SCROLLED_WINDOW); g_assert_nonnull(scroll);
    const guint percents[] = {73, 100, 125};
    for (guint i = 0; i < G_N_ELEMENTS(percents); ++i) {
        radios_content_scale_set_percent(f.scale, percents[i]); pump(300);
        GtkAdjustment *vertical = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
        gtk_adjustment_set_value(vertical, MIN(240, gtk_adjustment_get_upper(vertical) - gtk_adjustment_get_page_size(vertical)));
        pump(200);
        gint rx, ry, px, py, mx, my;
        /* A point of the grid that is drawn inside the calendar scroller and the window. */
        GdkRectangle drawn_grid = drawn_rect(grid), drawn_scroll = drawn_rect(scroll), shown, window_rect;
        cs_window_origin(f.window, &window_rect.x, &window_rect.y);
        window_rect.width = f.width; window_rect.height = f.height;
        g_assert_true(gdk_rectangle_intersect(&drawn_grid, &drawn_scroll, &shown));
        g_assert_true(gdk_rectangle_intersect(&shown, &window_rect, &shown));
        rx = shown.x + shown.width * 2 / 3; ry = shown.y + MIN(shown.height / 2, 60);
        g_assert_cmpint(drawn_grid.y, <, drawn_scroll.y); /* the calendar is scrolled */
        gtk_window_present(GTK_WINDOW(f.window)); pump(60);
        cs_motion(rx, ry); pump(40); cs_button(3, TRUE); pump(40); cs_button(3, FALSE); pump(300);
        GList *menus = gtk_menu_get_for_attach_widget(grid); g_assert_nonnull(menus);
        GtkWidget *menu = menus->data; g_assert_true(gtk_widget_get_mapped(menu));
        cs_pointer(&px, &py); cs_popup_origin(menu, &mx, &my);
        g_test_message("calendar block menu at %u%% (grid drawn from y %d, scroller from y %d): menu (%d,%d), pointer (%d,%d)",
            percents[i], drawn_grid.y, drawn_scroll.y, mx, my, px, py);
        g_assert_cmpint(ABS(mx - (px + 1)), <=, 3); g_assert_cmpint(ABS(my - (py + 1)), <=, 3);
        g_assert_nonnull(find(menu, "automation-calendar-copy", FALSE));
        gtk_menu_popdown(GTK_MENU(menu)); pump(150);
    }
    fixture_close(&f);
}
static void mapped_combos(GtkWidget *widget, gpointer data)
{
    if (GTK_IS_COMBO_BOX(widget) && gtk_widget_get_mapped(widget)) g_ptr_array_add(data, widget);
    else if (GTK_IS_CONTAINER(widget)) gtk_container_forall(GTK_CONTAINER(widget), mapped_combos, data);
}
/* The schema settings page (Otomasyon şeması, inside the rotation editor of
 * the zoomed workspace): its drop-down lists open at their combo box. */
static void schema_settings_lists_open_at_combo(void)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_default_size(GTK_WINDOW(window), 1200, 900);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_container_add(GTK_CONTAINER(window), outer);
    GtkWidget *header = gtk_drawing_area_new(); gtk_widget_set_size_request(header, 1, 40); gtk_box_pack_start(GTK_BOX(outer), header, FALSE, FALSE, 0);
    GtkWidget *scale = radios_content_scale_new(); gtk_box_pack_start(GTK_BOX(outer), scale, TRUE, TRUE, 0);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL); gtk_container_add(GTK_CONTAINER(scale), scroll);
    JsonParser *parser = json_parser_new(); GError *error = NULL;
    g_assert_true(json_parser_load_from_data(parser, "{\"id\":\"11111111-1111-4111-8111-111111111111\",\"name\":\"Deneme şeması\",\"sources\":["
        "{\"id\":\"22222222-2222-4222-8222-222222222222\",\"playlist_id\":\"44444444-4444-4444-8444-444444444444\",\"frequency\":1,\"count\":1,"
        "\"weight\":100,\"start_minute\":0,\"end_minute\":1440,\"interval_minutes\":0,\"random\":false,\"track_random\":false,"
        "\"advert_random\":false,\"sweeper_random\":false}],\"settings\":{\"automation_schema\":true,\"selection_mode\":\"linear\","
        "\"track_gap_minutes\":5,\"artist_gap_minutes\":10,\"album_gap_minutes\":0,\"transitions\":[{\"mode\":0,\"weight\":100,\"overlap_ms\":0}],"
        "\"autodj_players\":{\"music\":[12,31],\"advertisement\":[12,31],\"jingle\":[12,31],\"sweeper\":[12,31]},\"excluded_kinds\":[\"advertisement\"]}}",
        -1, &error));
    g_assert_no_error(error);
    JsonObject *definition = json_node_get_object(json_parser_get_root(parser));
    json_object_set_array_member(json_object_get_object_member(definition, "settings"), "autodj_presets", radios_transition_preset_templates());
    GtkWidget *editor = radios_schema_settings_new(definition, NULL, NULL); g_object_unref(parser);
    gtk_container_add(GTK_CONTAINER(scroll), editor);
    gtk_window_set_title(GTK_WINDOW(window), "radios-schema-settings-scale");
    gtk_widget_show_all(window); gtk_window_present(GTK_WINDOW(window)); pump(300);
    /* The lists of the "İç sıra ve türler" page (its step target list sits at
     * the top); the "Yayın sırası ve geçişler" page before it opens with a table. */
    GtkWidget *tabs = required(editor, "schema-settings-tabs"), *page = required(editor, "schema-settings-step-target");
    while (page && gtk_widget_get_parent(page) != tabs) page = gtk_widget_get_parent(page);
    g_assert_nonnull(page); gtk_notebook_set_current_page(GTK_NOTEBOOK(tabs), gtk_notebook_page_num(GTK_NOTEBOOK(tabs), page)); pump(300);
    const guint percents[] = {73, 125}; guint checked = 0;
    for (guint i = 0; i < G_N_ELEMENTS(percents); ++i) {
        radios_content_scale_set_percent(scale, percents[i]); pump(300);
        GtkAdjustment *vertical = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
        gtk_adjustment_set_value(vertical, MIN(20, gtk_adjustment_get_upper(vertical) - gtk_adjustment_get_page_size(vertical))); pump(200);
        GPtrArray *combos = g_ptr_array_new(); mapped_combos(editor, combos);
        gint wx, wy; cs_window_origin(window, &wx, &wy);
        guint tried = 0;
        for (guint c = 0; c < combos->len && tried < 3; ++c) {
            GtkWidget *combo = combos->pdata[c];
            if (gtk_combo_box_get_has_entry(GTK_COMBO_BOX(combo)) || gtk_combo_box_get_wrap_width(GTK_COMBO_BOX(combo)) > 0 ||
                !gtk_widget_is_sensitive(combo)) continue;
            GdkRectangle r = drawn_rect(combo);
            if (r.y < wy + 60 || r.y + r.height > wy + 420 || r.x + r.width > wx + 1100) continue; /* room for the list */
            ++tried;
            cs_motion(r.x + r.width / 2, r.y + r.height / 2); pump(40); cs_button(1, TRUE); pump(40); cs_button(1, FALSE); pump(250);
            gboolean shown = FALSE; g_object_get(combo, "popup-shown", &shown, NULL); g_assert_true(shown);
            GtkWidget *menu = gtk_accessible_get_widget(GTK_ACCESSIBLE(gtk_combo_box_get_popup_accessible(GTK_COMBO_BOX(combo))));
            gint dx = 0, dy = 0, mx, my; g_object_get(menu, "rect-anchor-dx", &dx, "rect-anchor-dy", &dy, NULL);
            cs_popup_origin(menu, &mx, &my);
            g_test_message("schema list %s at %u%%: menu (%d,%d) want (%d,%d)", gtk_widget_get_name(combo), percents[i],
                mx, my, r.x + dx, r.y + r.height / 2 + dy);
            g_assert_cmpint(ABS(mx - (r.x + dx)), <=, 2); g_assert_cmpint(ABS(my - (r.y + r.height / 2 + dy)), <=, 2);
            cs_key(XK_Escape, TRUE); pump(10); cs_key(XK_Escape, FALSE); pump(200);
            g_object_get(combo, "popup-shown", &shown, NULL); g_assert_false(shown);
            ++checked;
        }
        g_ptr_array_unref(combos);
    }
    g_test_message("schema settings lists checked: %u", checked); g_assert_cmpuint(checked, >=, 2);
    gtk_widget_destroy(window); pump(100);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); if (!gtk_init_check(&argc, &argv)) return 77;
    gint event, error, major, minor;
    if (!cs_wayland() && !XTestQueryExtension(cs_xdisplay(), &event, &error, &major, &minor)) return 77;
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
    /* One registered application serves every case: a registered GApplication
     * that never runs keeps its D-Bus export for the rest of the process, so
     * a second registration in the same process cannot succeed. */
    GError *registration = NULL;
    shared_application = gtk_application_new("org.radios.ContentScaleUIAcceptance", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(shared_application), NULL, &registration)); g_assert_no_error(registration);
    g_test_add_func("/content-scale-ui/appearance-real-zoom-fixed-header-input-persistence", scale_preferences_header_input_and_reopen);
    g_test_add_func("/content-scale-ui/visible-mixer-rapid-size-nested-detach-close", mixer_size_visible_rapid_nested_detach_and_close);
    g_test_add_func("/content-scale-ui/automation-calendar-block-menu-at-pointer", calendar_block_menu_opens_at_pointer);
    g_test_add_func("/content-scale-ui/schema-settings-lists-open-at-combo", schema_settings_lists_open_at_combo);
    gint result = g_test_run();
    g_object_unref(shared_application);
    return result;
}
