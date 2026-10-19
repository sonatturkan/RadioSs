#include "radios/window.h"
#include "radios/data.h"
#include <glib/gstdio.h>
#include <string.h>

static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (!g_strcmp0(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = find(item->data, name);
    g_list_free(children); return found;
}
static void settle(void)
{
    gint64 until = g_get_monotonic_time() + 220 * 1000;
    while (g_get_monotonic_time() < until) {
        while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000);
    }
}
static void card_bounds(GtkWidget *widget, GtkWidget *card)
{
    if (!gtk_widget_get_mapped(widget)) return;
    gint x, y; g_assert_true(gtk_widget_translate_coordinates(widget, card, 0, 0, &x, &y));
    gint width = gtk_widget_get_allocated_width(widget), height = gtk_widget_get_allocated_height(widget);
    gint card_width = gtk_widget_get_allocated_width(card), card_height = gtk_widget_get_allocated_height(card);
    if (x < 0 || y < 0 || x + width > card_width || y + height > card_height)
        g_error("Card child %s bounds %d,%d %dx%d exceed card %dx%d", gtk_widget_get_name(widget), x, y, width, height, card_width, card_height);
    if (!GTK_IS_CONTAINER(widget)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (GList *item = children; item; item = item->next) card_bounds(item->data, card);
    g_list_free(children);
}
static void summary_readable(GtkWidget *widget)
{
    if (!gtk_widget_get_mapped(widget)) return;
    if (GTK_IS_LABEL(widget))
        g_assert_cmpint(pango_layout_get_line_count(gtk_label_get_layout(GTK_LABEL(widget))), ==, 1);
    if (!GTK_IS_CONTAINER(widget)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (GList *item = children; item; item = item->next) summary_readable(item->data);
    g_list_free(children);
}
static GdkRectangle window_bounds(GtkWidget *window, GtkWidget *widget)
{
    GdkRectangle bounds = {0, 0, gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget)};
    g_assert_true(gtk_widget_translate_coordinates(widget, window, 0, 0, &bounds.x, &bounds.y));
    return bounds;
}
static void contained_in(GtkWidget *window, GtkWidget *widget, GtkWidget *viewport)
{
    GdkRectangle bounds = window_bounds(window, widget), visible = window_bounds(window, viewport);
    g_assert_true(gtk_widget_get_mapped(widget));
    g_test_message("Visible %s: %d,%d %dx%d inside %s: %d,%d %dx%d", gtk_widget_get_name(widget),
        bounds.x, bounds.y, bounds.width, bounds.height, gtk_widget_get_name(viewport),
        visible.x, visible.y, visible.width, visible.height);
    g_assert_cmpint(bounds.x, >=, visible.x); g_assert_cmpint(bounds.y, >=, visible.y);
    g_assert_cmpint(bounds.x + bounds.width, <=, visible.x + visible.width);
    g_assert_cmpint(bounds.y + bounds.height, <=, visible.y + visible.height);
}
static void independent_controls_visible(GtkWidget *window, GtkWidget *page, GtkWidget *card, GtkWidget *footer, guint width)
{
    GtkWidget *management = find(page, "live-player-management");
    g_assert_true(GTK_IS_EXPANDER(management));
    g_assert_false(gtk_expander_get_expanded(GTK_EXPANDER(management)));
    GtkWidget *definitions = find(management, "live-player-definitions"); g_assert_nonnull(definitions);
    GtkWidget *controls = find(card, "lp-independent-controls"); g_assert_nonnull(controls);
    g_assert_true(gtk_widget_get_visible(controls));
    GtkWidget *scroll = gtk_widget_get_ancestor(card, GTK_TYPE_SCROLLED_WINDOW);
    GtkWidget *profile = gtk_widget_get_ancestor(page, GTK_TYPE_SCROLLED_WINDOW);
    g_assert_true(GTK_IS_SCROLLED_WINDOW(scroll)); g_assert_true(GTK_IS_SCROLLED_WINDOW(profile));
    g_assert_true(scroll != profile);
    GtkWidget *viewport = gtk_bin_get_child(GTK_BIN(scroll)), *profile_viewport = gtk_bin_get_child(GTK_BIN(profile));
    g_assert_true(GTK_IS_VIEWPORT(viewport)); g_assert_true(GTK_IS_VIEWPORT(profile_viewport));
    GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(scroll));
    gint x, y; g_assert_true(gtk_widget_translate_coordinates(controls, viewport, 0, 0, &x, &y));
    gdouble top = y + gtk_adjustment_get_value(adjustment);
    gtk_adjustment_set_value(adjustment, MAX(0, top - 6));
    settle();
    g_test_message("Player scroll: target %.0f value %.0f upper %.0f page %.0f", top,
        gtk_adjustment_get_value(adjustment), gtk_adjustment_get_upper(adjustment), gtk_adjustment_get_page_size(adjustment));
    GtkAdjustment *profile_adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(profile));
    if (width >= 1600) {
        /* At this width the normal header leaves enough room for the rack.
         * Its controls must fit without moving the whole profile; the old
         * inherited 604px minimum fails this check despite the inner scroll. */
        g_assert_cmpfloat(gtk_adjustment_get_value(profile_adjustment), ==, 0);
        contained_in(window, controls, profile_viewport);
    }
    /* Narrower headers wrap and leave less vertical room. A user can also
     * scroll the whole profile, so bring the same control block into that
     * viewport before measuring its final visible bounds. */
    g_assert_true(gtk_widget_translate_coordinates(controls, profile_viewport, 0, 0, &x, &y));
    top = y + gtk_adjustment_get_value(profile_adjustment);
    gtk_adjustment_clamp_page(profile_adjustment, MAX(0, top - 6), top + gtk_widget_get_allocated_height(controls) + 6);
    settle();
    /* The inner viewport can itself be taller than the real profile viewport
     * when an inherited minimum height pushes content underneath the footer.
     * Require the whole control block and its real buttons in both viewports. */
    const gchar *names[] = {"lp-independent-controls", "deck-0-sync", "deck-0-sync-target", "deck-0-autodj-mode", "deck-0-autodj-start"};
    GdkRectangle footer_bounds = window_bounds(window, footer);
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkWidget *widget = find(card, names[i]); g_assert_nonnull(widget);
        contained_in(window, widget, viewport); contained_in(window, widget, profile_viewport);
        GdkRectangle bounds = window_bounds(window, widget);
        g_assert_cmpint(bounds.y + bounds.height, <=, footer_bounds.y);
    }
}
static void autodj_start_readable(GtkWidget *card)
{
    GtkWidget *controls = find(card, "lp-independent-controls");
    if (!controls || !gtk_widget_get_visible(controls)) return;
    GtkWidget *start = find(card, "deck-0-autodj-start"); g_assert_true(GTK_IS_BUTTON(start));
    GtkWidget *label = gtk_bin_get_child(GTK_BIN(start)); g_assert_true(GTK_IS_LABEL(label));
    g_assert_true(gtk_widget_get_mapped(start)); g_assert_true(gtk_widget_get_mapped(label));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(label)), ==, "Başlat");
    PangoLayout *layout = gtk_label_get_layout(GTK_LABEL(label));
    PangoRectangle logical; pango_layout_get_pixel_extents(layout, NULL, &logical);
    g_assert_false(pango_layout_is_ellipsized(layout));
    g_assert_cmpint(pango_layout_get_line_count(layout), ==, 1);
    g_assert_cmpint(gtk_widget_get_allocated_width(label), >=, logical.width);
    /* Theme dimensions follow the saved display scale; measure the text. */
    g_assert_cmpint(gtk_widget_get_allocated_width(start), >=, logical.width + 8);
    /* At narrow card widths the action has a complete row instead of being
     * squeezed to an empty square beside the mode/target selector. */
    GtkWidget *mode = find(card, "deck-0-autodj-mode");
    if (!mode) mode = find(card, "deck-0-autodj-target");
    g_assert_true(GTK_IS_COMBO_BOX(mode));
    gint start_x, start_y, mode_x, mode_y;
    g_assert_true(gtk_widget_translate_coordinates(start, card, 0, 0, &start_x, &start_y));
    g_assert_true(gtk_widget_translate_coordinates(mode, card, 0, 0, &mode_x, &mode_y));
    if (gtk_widget_get_allocated_width(card) < 360) {
        g_assert_cmpint(start_y, >=, mode_y + gtk_widget_get_allocated_height(mode));
        g_assert_cmpint(gtk_widget_get_allocated_width(start), >=, gtk_widget_get_allocated_width(mode));
    } else {
        g_assert_cmpint(start_x, >=, mode_x + gtk_widget_get_allocated_width(mode));
    }
    g_test_message("AutoDJ action: card %d, selector %d,%d %dx%d; readable Başlat %d,%d %dx%d",
        gtk_widget_get_allocated_width(card), mode_x, mode_y, gtk_widget_get_allocated_width(mode),
        gtk_widget_get_allocated_height(mode), start_x, start_y, gtk_widget_get_allocated_width(start),
        gtk_widget_get_allocated_height(start));
}
static void capture(GtkWidget *window, guint width)
{
    const gchar *directory = g_getenv("RADIOS_LIVE_LAYOUT_CAPTURE_DIR"); if (!directory) return;
    g_mkdir_with_parents(directory, 0700);
    gint w = gtk_widget_get_allocated_width(window), h = gtk_widget_get_allocated_height(window);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surface); gtk_widget_draw(window, cr); cairo_destroy(cr);
    gchar *filename = g_strdup_printf("live-layout-%u.png", width), *path = g_build_filename(directory, filename, NULL);
    g_assert_cmpint(cairo_surface_write_to_png(surface, path), ==, CAIRO_STATUS_SUCCESS);
    cairo_surface_destroy(surface); g_free(path); g_free(filename);
}
static void capture_card(GtkWidget *card, guint width)
{
    const gchar *directory = g_getenv("RADIOS_LIVE_LAYOUT_CAPTURE_DIR"); if (!directory) return;
    g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0);
    gint w = gtk_widget_get_allocated_width(card), h = gtk_widget_get_allocated_height(card);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    cairo_t *cr = cairo_create(surface); gtk_widget_draw(card, cr); cairo_destroy(cr);
    gchar *filename = g_strdup_printf("live-card-%u.png", width), *path = g_build_filename(directory, filename, NULL);
    g_assert_cmpint(cairo_surface_write_to_png(surface, path), ==, CAIRO_STATUS_SUCCESS);
    cairo_surface_destroy(surface); g_free(path); g_free(filename);
}
static void minimums(GtkWidget *widget, guint depth)
{
    if (!gtk_widget_get_visible(widget)) return;
    gint minimum, natural; gtk_widget_get_preferred_width(widget, &minimum, &natural);
    if (minimum >= 560) g_test_message("minimum %s/%s depth %u: %d/%d", gtk_widget_get_name(widget),
        G_OBJECT_TYPE_NAME(widget), depth, minimum, natural);
    if (minimum >= 500 && GTK_IS_BOX(widget)) {
        GList *parts = gtk_container_get_children(GTK_CONTAINER(widget));
        for (GList *part = parts; part; part = part->next)
            g_test_message("  child %s %s", G_OBJECT_TYPE_NAME(part->data), GTK_IS_LABEL(part->data) ? gtk_label_get_text(GTK_LABEL(part->data)) : "");
        g_list_free(parts);
    }
    if (!GTK_IS_CONTAINER(widget)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (GList *item = children; item; item = item->next) minimums(item->data, depth + 1);
    g_list_free(children);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); if (!gtk_init_check(&argc, &argv)) return 77;
    gboolean private_backend = argc > 1;
    GError *error = NULL; RadioSConfig original = {0};
    g_assert_true(radios_config_load(private_backend ? argv[1] : RADIOS_TEST_PROJECT_ROOT, NULL, &original, &error)); g_assert_no_error(error);
    gchar *root = private_backend ? g_strdup(argv[1]) : g_dir_make_tmp("radios-live-layout-XXXXXX", &error); g_assert_no_error(error);
    gchar *config_dir = g_build_filename(root, "config", NULL); g_assert_cmpint(g_mkdir_with_parents(config_dir, 0700), ==, 0);
    RadioSConfig isolated = original; isolated.project_root = root;
    GtkApplication *application = gtk_application_new("org.radios.LiveLayoutTest", G_APPLICATION_NON_UNIQUE);
    g_assert_true(g_application_register(G_APPLICATION(application), NULL, &error)); g_assert_no_error(error);
    RadioSRuntime *runtime = radios_runtime_new(64, 512);
    RadioSWindow *ui = radios_window_new(application, &isolated, runtime, FALSE, NULL, &error); g_assert_no_error(error); g_assert_nonnull(ui);
    radios_window_present(ui); g_assert_true(radios_window_select_profile(ui, "broadcast"));
    RadioSViewContext *ctx = radios_window_view_context(ui); GtkWidget *window = GTK_WIDGET(ctx->parent);
    if (private_backend) {
        gint64 until = g_get_monotonic_time() + 10 * G_TIME_SPAN_SECOND;
        while (!radios_data_connected(ctx) && g_get_monotonic_time() < until) settle();
        g_assert_true(radios_data_connected(ctx));
        for (guint i = 0; i < 5; ++i) settle();
    }
    GtkWidget *page = find(window, "live-task-view"); g_assert_nonnull(page);
    while (GTK_IS_BIN(page)) page = gtk_bin_get_child(GTK_BIN(page));
    g_assert_true(GTK_IS_BOX(page));
    GtkWidget *rack = find(page, "broadcast-players-pane"); g_assert_nonnull(rack);
    GtkWidget *header = find(window, "shared-header"), *footer = find(window, "status-footer");
    g_assert_nonnull(header); g_assert_nonnull(footer);
    GtkWidget *stack = gtk_widget_get_parent(rack); g_assert_true(GTK_IS_STACK(stack)); gtk_stack_set_visible_child_name(GTK_STACK(stack), "players");
    const guint widths[] = {1600, 1440, 900, 600, 1440};
    for (guint i = 0; i < G_N_ELEMENTS(widths); ++i) {
        gint actual = 0, actual_height;
        /* Responsive header and card requisitions settle after the first
         * resize; continue the same drag to its requested final width. */
        for (guint attempt = 0; attempt < 3 && actual != (gint)widths[i]; ++attempt) {
            gtk_window_resize(GTK_WINDOW(window), widths[i], 1000); settle();
            gtk_window_get_size(GTK_WINDOW(window), &actual, &actual_height);
        }
        g_test_message("requested %u: window %d, page %d, rack %d, orientation %d", widths[i], actual,
            gtk_widget_get_allocated_width(page), gtk_widget_get_allocated_width(rack), gtk_orientable_get_orientation(GTK_ORIENTABLE(page)));
        if (actual != (gint)widths[i]) { minimums(window, 0); capture(window, widths[i]); }
        g_assert_cmpint(actual, ==, (gint)widths[i]);
        if (gtk_widget_get_allocated_width(page) > gtk_widget_get_allocated_width(window)) { minimums(page, 0); capture(window, widths[i]); }
        g_assert_cmpint(gtk_widget_get_allocated_width(page), <=, gtk_widget_get_allocated_width(window));
        if (widths[i] <= 900) g_assert_cmpint(gtk_orientable_get_orientation(GTK_ORIENTABLE(page)), ==, GTK_ORIENTATION_VERTICAL);
        /* The added 1600px control-visibility case keeps the normal header,
         * whose programme captions may wrap. Preserve the earlier header
         * readability assertions at their original responsive widths. */
        if (widths[i] != 1600) summary_readable(header);
        summary_readable(footer);
        g_assert_cmpint(gtk_widget_get_allocated_height(footer), <=, 90);
        if (widths[i] <= 900) {
            g_assert_cmpint(gtk_widget_get_allocated_height(header), <=, 390);
            GtkWidget *master = find(header, "header-main-gain"); g_assert_nonnull(master);
            g_assert_false(gtk_widget_get_mapped(master));
            g_assert_true(gtk_widget_get_mapped(find(header, "header-headphone-cue")));
            g_assert_true(gtk_widget_get_mapped(find(header, "header-output-cue")));
        } else if (widths[i] == 1440) {
            g_assert_true(gtk_widget_get_mapped(find(header, "header-main-gain")));
        }
        g_test_message("header height %d, footer height %d", gtk_widget_get_allocated_height(header), gtk_widget_get_allocated_height(footer));
        GtkWidget *card = g_object_get_data(G_OBJECT(window), "radios-deck-owner-0"); g_assert_nonnull(card); card_bounds(card, card);
        autodj_start_readable(card);
        gint card_min, card_natural; gtk_widget_get_preferred_width(card, &card_min, &card_natural);
        g_test_message("card allocated %d, minimum %d, natural %d", gtk_widget_get_allocated_width(card), card_min, card_natural);
        GtkWidget *flow = gtk_widget_get_parent(gtk_widget_get_parent(card));
        g_test_message("flow allocation %d, columns %u", gtk_widget_get_allocated_width(flow), gtk_flow_box_get_max_children_per_line(GTK_FLOW_BOX(flow)));
        if (private_backend && widths[i] >= 1440) independent_controls_visible(window, page, card, footer, widths[i]);
        capture_card(card, widths[i]);
        gint x, y; g_assert_true(gtk_widget_translate_coordinates(card, rack, 0, 0, &x, &y));
        g_assert_cmpint(x, >=, 0); g_assert_cmpint(x, <=, 14);
        capture(window, widths[i]);
    }
    radios_window_free(ui); radios_runtime_free(runtime); g_object_unref(application); radios_config_clear(&original);
    if (!private_backend) {
        gchar *preferences = g_build_filename(config_dir, "ui-preferences.ini", NULL); g_unlink(preferences); g_free(preferences);
        g_rmdir(config_dir); g_rmdir(root);
    }
    g_free(config_dir); g_free(root); return 0;
}
