#include "radios/content_scale.h"
#include <gdk/gdkx.h>
#include <X11/extensions/XTest.h>
#include <X11/keysym.h>
#include <math.h>

typedef struct { GtkWidget *window, *header, *footer, *scale, *body, *button, *entry, *range, *scroll; guint clicks; } Fixture;
static void pump(guint ms)
{
    gint64 end = g_get_monotonic_time() + ms * 1000;
    do { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); } while (g_get_monotonic_time() < end);
}
static void clicked(GtkButton *button, gpointer user) { (void)button; ++((Fixture *)user)->clicks; }
static Display *display(void) { return GDK_DISPLAY_XDISPLAY(gdk_display_get_default()); }
static void position(Fixture *f, GtkWidget *widget, gdouble dx, gdouble dy, gint *x, gint *y)
{
    gint logical_x, logical_y, origin_x, origin_y;
    g_assert_true(gtk_widget_translate_coordinates(widget, f->body, 0, 0, &logical_x, &logical_y));
    gdk_window_get_origin(gtk_widget_get_window(f->scale), &origin_x, &origin_y);
    gdouble horizontal = radios_content_scale_get_percent(f->scale) / 100.0, vertical = radios_content_scale_get_vertical_percent(f->scale) / 100.0;
    *x = origin_x + (gint)llround((logical_x + dx) * horizontal);
    *y = origin_y + (gint)llround((logical_y + dy) * vertical);
}
static void motion(gint x, gint y) { XTestFakeMotionEvent(display(), -1, x, y, CurrentTime); XFlush(display()); pump(25); }
static void mouse(guint button, gboolean press) { XTestFakeButtonEvent(display(), button, press, CurrentTime); XFlush(display()); pump(25); }
static void click_at(Fixture *f, GtkWidget *widget)
{
    gint x, y; position(f, widget, gtk_widget_get_allocated_width(widget)/2.0, gtk_widget_get_allocated_height(widget)/2.0, &x, &y);
    motion(x, y); mouse(1, TRUE); mouse(1, FALSE);
}
static void key(KeySym symbol, gboolean press)
{ XTestFakeKeyEvent(display(), XKeysymToKeycode(display(), symbol), press, CurrentTime); XFlush(display()); pump(10); }
static void fixture_init(Fixture *f)
{
    f->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_default_size(GTK_WINDOW(f->window), 640, 520);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_container_add(GTK_CONTAINER(f->window), outer);
    f->header = gtk_drawing_area_new(); gtk_widget_set_size_request(f->header, 1, 40);
    f->footer = gtk_drawing_area_new(); gtk_widget_set_size_request(f->footer, 1, 30);
    gtk_box_pack_start(GTK_BOX(outer), f->header, FALSE, FALSE, 0);
    f->scale = radios_content_scale_new(); gtk_box_pack_start(GTK_BOX(outer), f->scale, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(outer), f->footer, FALSE, FALSE, 0);
    f->body = gtk_fixed_new(); gtk_container_add(GTK_CONTAINER(f->scale), f->body);
    f->button = gtk_button_new_with_label("Button"); gtk_widget_set_size_request(f->button, 120, 36); gtk_fixed_put(GTK_FIXED(f->body), f->button, 20, 20);
    g_signal_connect(f->button, "clicked", G_CALLBACK(clicked), f);
    f->entry = gtk_entry_new(); gtk_widget_set_size_request(f->entry, 180, 32); gtk_fixed_put(GTK_FIXED(f->body), f->entry, 20, 70);
    f->range = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 100, 1); gtk_scale_set_draw_value(GTK_SCALE(f->range), FALSE);
    gtk_widget_set_size_request(f->range, 250, 35); gtk_fixed_put(GTK_FIXED(f->body), f->range, 20, 115);
    f->scroll = gtk_scrolled_window_new(NULL, NULL); gtk_widget_set_size_request(f->scroll, 260, 110);
    GtkWidget *large = gtk_drawing_area_new(); gtk_widget_set_size_request(large, 700, 700);
    gtk_container_add(GTK_CONTAINER(f->scroll), large); gtk_fixed_put(GTK_FIXED(f->body), f->scroll, 20, 160);
    gtk_widget_show_all(f->window); gtk_window_present(GTK_WINDOW(f->window)); pump(100);
}
static void geometry_input(void)
{
    Fixture f = {0}; fixture_init(&f);
    const guint values[] = {100, 50, 150, 75, 125, 100};
    for (guint i = 0; i < G_N_ELEMENTS(values); ++i) {
        radios_content_scale_set_percent(f.scale, values[i]); pump(100);
        gdouble ratio = values[i] / 100.0;
        g_assert_cmpuint(radios_content_scale_get_percent(f.scale), ==, values[i]);
        g_assert_cmpint(gtk_widget_get_allocated_height(f.header), ==, 40);
        g_assert_cmpint(gtk_widget_get_allocated_height(f.footer), ==, 30);
        g_assert_cmpint(gtk_widget_get_allocated_width(f.body), ==, (gint)floor(gtk_widget_get_allocated_width(f.scale)/ratio));
        g_assert_cmpint(gtk_widget_get_allocated_height(f.body), ==, (gint)floor(gtk_widget_get_allocated_height(f.scale)/ratio));
        g_assert_cmpint(gtk_widget_get_allocated_width(f.button), ==, 120);
        click_at(&f, f.button); g_assert_cmpuint(f.clicks, ==, i + 1);
        click_at(&f, f.entry); g_assert_true(gtk_widget_has_focus(f.entry));
        key(XK_Control_L, TRUE); key(XK_a, TRUE); key(XK_a, FALSE); key(XK_Control_L, FALSE);
        key(XK_q, TRUE); key(XK_q, FALSE); g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(f.entry)), ==, "q");
        gtk_range_set_value(GTK_RANGE(f.range), 0);
        gint x1, y1, x2, y2;
        position(&f, f.range, 14, 17, &x1, &y1); position(&f, f.range, 220, 17, &x2, &y2);
        motion(x1, y1); mouse(1, TRUE);
        for (guint step = 1; step <= 8; ++step) motion(x1+(x2-x1)*(gint)step/8, y1+(y2-y1)*(gint)step/8);
        mouse(1, FALSE); g_assert_cmpfloat(gtk_range_get_value(GTK_RANGE(f.range)), >, 60);
        GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(f.scroll));
        gtk_adjustment_set_value(adjustment, 0); click_at(&f, f.scroll); mouse(5, TRUE); mouse(5, FALSE);
        g_assert_cmpfloat(gtk_adjustment_get_value(adjustment), >, 0);
    }
    radios_content_scale_set_percent(f.scale, 1); g_assert_cmpuint(radios_content_scale_get_percent(f.scale), ==, 50);
    radios_content_scale_set_percent(f.scale, 900); g_assert_cmpuint(radios_content_scale_get_percent(f.scale), ==, 150);
    gtk_widget_destroy(f.window); pump(30);
}
static gboolean red_draw(GtkWidget *widget, cairo_t *cr, gpointer user)
{
    (void)widget; (void)user; cairo_set_source_rgb(cr, 1, 0, 0); cairo_paint(cr); return TRUE;
}
static void drag_get(GtkWidget *widget, GdkDragContext *context, GtkSelectionData *selection, guint info, guint time, gpointer user)
{
    (void)widget; (void)context; (void)info; (void)time; (void)user;
    gtk_selection_data_set_text(selection, "live-scale-drag", -1);
}
static void drag_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y, GtkSelectionData *selection, guint info, guint time, gpointer user)
{
    (void)widget; (void)x; (void)y; (void)info;
    gchar *text = (gchar *)gtk_selection_data_get_text(selection);
    gboolean valid = g_strcmp0(text, "live-scale-drag") == 0;
    if (valid) ++*(guint *)user;
    g_free(text); gtk_drag_finish(context, valid, FALSE, time);
}
static GtkWidget *drag_box(const gchar *text)
{
    GtkWidget *box = gtk_event_box_new(); gtk_container_add(GTK_CONTAINER(box), gtk_label_new(text));
    gtk_widget_set_size_request(box, 110, 36); return box;
}
static void popup_drag_and_pixels(void)
{
    Fixture f = {0}; fixture_init(&f); guint drops = 0;
    GtkWidget *menu = gtk_menu_button_new(); gtk_button_set_label(GTK_BUTTON(menu), "Popup");
    gtk_widget_set_size_request(menu, 110, 36); gtk_fixed_put(GTK_FIXED(f.body), menu, 300, 20);
    GtkWidget *popover = gtk_popover_new(menu), *popup_entry = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(popup_entry), "popup"); gtk_container_add(GTK_CONTAINER(popover), popup_entry); gtk_widget_show(popup_entry);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(menu), popover);
    GtkWidget *source = drag_box("Drag"), *dest = drag_box("Drop");
    gtk_fixed_put(GTK_FIXED(f.body), source, 300, 100); gtk_fixed_put(GTK_FIXED(f.body), dest, 300, 165);
    GtkTargetEntry target = {"UTF8_STRING", GTK_TARGET_SAME_APP, 0};
    gtk_drag_source_set(source, GDK_BUTTON1_MASK, &target, 1, GDK_ACTION_COPY);
    gtk_drag_dest_set(dest, GTK_DEST_DEFAULT_ALL, &target, 1, GDK_ACTION_COPY);
    g_signal_connect(source, "drag-data-get", G_CALLBACK(drag_get), NULL);
    g_signal_connect(dest, "drag-data-received", G_CALLBACK(drag_received), &drops);
    GtkWidget *red = gtk_drawing_area_new(); gtk_widget_set_size_request(red, 100, 30);
    gtk_fixed_put(GTK_FIXED(f.body), red, 300, 235); g_signal_connect(red, "draw", G_CALLBACK(red_draw), NULL);
    gtk_widget_show_all(f.body);
    const guint values[] = {50, 150, 100};
    for (guint i = 0; i < G_N_ELEMENTS(values); ++i) {
        radios_content_scale_set_percent(f.scale, values[i]); pump(100);
        g_assert_cmpint(gtk_widget_get_allocated_width(f.window), ==, 640);
        g_assert_cmpint(gtk_widget_get_allocated_height(f.window), ==, 520);
        /* A real native-window capture verifies boxes, not only font metrics. */
        gint x, y; position(&f, red, 10, 15, &x, &y);
        gint wx, wy; gdk_window_get_origin(gtk_widget_get_window(f.window), &wx, &wy);
        GdkPixbuf *pixels = gdk_pixbuf_get_from_window(gtk_widget_get_window(f.window), x-wx, y-wy, 1, 1);
        g_assert_nonnull(pixels); guchar *pixel = gdk_pixbuf_get_pixels(pixels);
        g_assert_cmpuint(pixel[0], >, 240); g_assert_cmpuint(pixel[1], <, 10); g_assert_cmpuint(pixel[2], <, 10);
        g_object_unref(pixels);
        click_at(&f, menu); pump(120); g_assert_true(gtk_widget_get_visible(popover));
        g_assert_true(gtk_widget_is_drawable(popup_entry));
        GtkWidget *native = popup_entry;
        while (!gtk_widget_get_has_window(native)) native = gtk_widget_get_parent(native);
        gint px, py, nx, ny;
        g_assert_true(gtk_widget_translate_coordinates(popup_entry, native,
            gtk_widget_get_allocated_width(popup_entry)/2, gtk_widget_get_allocated_height(popup_entry)/2, &nx, &ny));
        gdk_window_get_root_coords(gtk_widget_get_window(native), nx, ny, &px, &py);
        motion(px, py); mouse(1, TRUE); mouse(1, FALSE);
        g_assert_true(gtk_widget_has_focus(popup_entry)); key(XK_End, TRUE); key(XK_End, FALSE);
        key(XK_x, TRUE); key(XK_x, FALSE); g_assert_true(g_str_has_suffix(gtk_entry_get_text(GTK_ENTRY(popup_entry)), "x"));
        key(XK_Escape, TRUE); key(XK_Escape, FALSE); pump(350);
        g_assert_false(gtk_widget_get_visible(popover));
        gint sx, sy, dx, dy; position(&f, source, 55, 18, &sx, &sy); position(&f, dest, 55, 18, &dx, &dy);
        motion(sx, sy); mouse(1, TRUE);
        for (guint step = 1; step <= 15; ++step) motion(sx+(dx-sx)*(gint)step/15, sy+(dy-sy)*(gint)step/15);
        pump(100); mouse(1, FALSE); pump(200); g_assert_cmpuint(drops, ==, i+1);
    }
    gtk_widget_destroy(f.window); pump(30);
}
static void native_position(GtkWidget *widget, gint dx, gint dy, gint *x, gint *y)
{
    GtkWidget *native = widget;
    while (!gtk_widget_get_has_window(native)) native = gtk_widget_get_parent(native);
    gint nx, ny; g_assert_true(gtk_widget_translate_coordinates(widget, native, dx, dy, &nx, &ny));
    gdk_window_get_root_coords(gtk_widget_get_window(native), nx, ny, x, y);
}
static void combo_menu_tree_selection(void)
{
    Fixture f = {0}; fixture_init(&f);
    GtkWidget *combo = gtk_combo_box_text_new(); gtk_widget_set_size_request(combo, 120, 36);
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "first", "First");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "second", "Second");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "third", "Third");
    gtk_fixed_put(GTK_FIXED(f.body), combo, 300, 20);
    GtkListStore *model = gtk_list_store_new(1, G_TYPE_STRING);
    for (guint i = 0; i < 5; ++i) {
        GtkTreeIter iter; gchar *label = g_strdup_printf("Row %u", i);
        gtk_list_store_append(model, &iter); gtk_list_store_set(model, &iter, 0, label, -1); g_free(label);
    }
    GtkWidget *tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(model)); g_object_unref(model);
    GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
    GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes("Rows", renderer, "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(tree), column);
    gtk_widget_set_size_request(tree, 120, 190); gtk_fixed_put(GTK_FIXED(f.body), tree, 300, 75);
    gtk_widget_show_all(f.body);
    const guint values[] = {50, 150};
    for (guint i = 0; i < G_N_ELEMENTS(values); ++i) {
        radios_content_scale_set_percent(f.scale, values[i]); gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 0); pump(100);
        click_at(&f, combo); pump(150);
        gboolean shown = FALSE; g_object_get(combo, "popup-shown", &shown, NULL); g_assert_true(shown);
        AtkObject *accessible = gtk_combo_box_get_popup_accessible(GTK_COMBO_BOX(combo));
        g_assert_true(GTK_IS_ACCESSIBLE(accessible));
        GtkWidget *menu = gtk_accessible_get_widget(GTK_ACCESSIBLE(accessible)); g_assert_true(GTK_IS_MENU(menu));
        g_assert_true(gtk_widget_get_mapped(menu));
        gint ax, ay, mx, my; position(&f, combo, 0, 0, &ax, &ay); native_position(menu, 0, 0, &mx, &my);
        g_test_message("zoom %u menu=(%d,%d) anchor=(%d,%d)", values[i], mx, my, ax, ay);
        g_assert_cmpint(ABS(mx-ax), <, 35); g_assert_cmpint(ABS(my-ay), <, 70);
        g_assert_cmpint(mx, >=, 0); g_assert_cmpint(my, >=, 0);
        GdkDisplay *gdk_display = gdk_display_get_default(); GdkMonitor *monitor = gdk_display_get_primary_monitor(gdk_display);
        GdkRectangle bounds; gdk_monitor_get_geometry(monitor, &bounds);
        g_assert_cmpint(mx + gtk_widget_get_allocated_width(menu), <=, bounds.x + bounds.width);
        g_assert_cmpint(my + gtk_widget_get_allocated_height(menu), <=, bounds.y + bounds.height);
        GList *items = gtk_container_get_children(GTK_CONTAINER(menu));
        GtkWidget *third = g_list_nth_data(items, 2); g_assert_true(GTK_IS_MENU_ITEM(third));
        gint tx, ty; native_position(third, gtk_widget_get_allocated_width(third)/2, gtk_widget_get_allocated_height(third)/2, &tx, &ty);
        motion(tx, ty); mouse(1, TRUE); mouse(1, FALSE); pump(100); g_list_free(items);
        g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo)), ==, "third");
        g_object_get(combo, "popup-shown", &shown, NULL); g_assert_false(shown);
        GtkTreePath *path = gtk_tree_path_new_from_indices(2, -1); GdkRectangle cell;
        gtk_tree_view_get_background_area(GTK_TREE_VIEW(tree), path, column, &cell);
        gint row_x, row_y; gtk_tree_view_convert_bin_window_to_widget_coords(GTK_TREE_VIEW(tree),
            cell.x + cell.width/2, cell.y + cell.height/2, &row_x, &row_y);
        position(&f, tree, row_x, row_y, &tx, &ty); motion(tx, ty); mouse(1, TRUE); mouse(1, FALSE);
        GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(tree));
        g_assert_true(gtk_tree_selection_path_is_selected(selection, path));
        gtk_tree_path_free(path); gtk_tree_selection_unselect_all(selection);
        g_assert_cmpint(gtk_widget_get_allocated_width(f.window), ==, 640);
        g_assert_cmpint(gtk_widget_get_allocated_height(f.window), ==, 520);
    }
    gtk_widget_destroy(f.window); pump(30);
}
static void nested_position(Fixture *outer, GtkWidget *inner, GtkWidget *widget,
    gdouble dx, gdouble dy, guint outer_percent, guint inner_percent, gint *x, gint *y)
{
    GtkWidget *body = gtk_bin_get_child(GTK_BIN(inner));
    gint logical_x, logical_y, embed_x, embed_y, origin_x, origin_y;
    g_assert_true(gtk_widget_translate_coordinates(widget, body, 0, 0, &logical_x, &logical_y));
    g_assert_true(gtk_widget_translate_coordinates(inner, outer->body, 0, 0, &embed_x, &embed_y));
    gdk_window_get_origin(gtk_widget_get_window(outer->scale), &origin_x, &origin_y);
    gdouble outside = outer_percent / 100.0, inside = inner_percent / 100.0;
    *x = origin_x + (gint)llround((embed_x + (logical_x + dx) * inside) * outside);
    *y = origin_y + (gint)llround((embed_y + (logical_y + dy) * inside) * outside);
}
static gboolean native_red_at(GtkWidget *window, gint x, gint y)
{
    gint origin_x, origin_y; gdk_window_get_origin(gtk_widget_get_window(window), &origin_x, &origin_y);
    GdkPixbuf *pixels = gdk_pixbuf_get_from_window(gtk_widget_get_window(window), x-origin_x, y-origin_y, 1, 1);
    g_assert_nonnull(pixels); guchar *pixel = gdk_pixbuf_get_pixels(pixels);
    gboolean red = pixel[0] > 240 && pixel[1] < 10 && pixel[2] < 10;
    g_object_unref(pixels); return red;
}
static void nested_geometry_input(void)
{
    Fixture outer = {0}, nested = {0}; fixture_init(&outer);
    GtkWidget *inner = radios_content_scale_new(); gtk_widget_set_size_request(inner, 300, 220);
    gtk_fixed_put(GTK_FIXED(outer.body), inner, 250, 20);
    nested.body = gtk_fixed_new(); gtk_container_add(GTK_CONTAINER(inner), nested.body);
    nested.button = gtk_button_new_with_label("Nested"); gtk_widget_set_size_request(nested.button, 120, 36);
    gtk_fixed_put(GTK_FIXED(nested.body), nested.button, 20, 20);
    g_signal_connect(nested.button, "clicked", G_CALLBACK(clicked), &nested);
    nested.entry = gtk_entry_new(); gtk_widget_set_size_request(nested.entry, 180, 32);
    gtk_fixed_put(GTK_FIXED(nested.body), nested.entry, 20, 70);
    GtkWidget *red = gtk_drawing_area_new(); gtk_widget_set_size_request(red, 100, 32);
    gtk_fixed_put(GTK_FIXED(nested.body), red, 20, 125);
    g_signal_connect(red, "draw", G_CALLBACK(red_draw), NULL);
    gtk_widget_show_all(outer.body);
    const guint percents[][2] = {{75, 125}, {150, 50}};
    /* Keep interior samples clear of both bilinear-filtered scale boundaries. */
    const gint inside[][2] = {{6, 16}, {94, 16}, {50, 6}, {50, 26}};
    const gint outside[][2] = {{-4, 16}, {104, 16}, {50, -4}, {50, 36}};
    for (guint i = 0; i < G_N_ELEMENTS(percents); ++i) {
        guint outer_percent = percents[i][0], inner_percent = percents[i][1];
        radios_content_scale_set_percent(outer.scale, outer_percent);
        radios_content_scale_set_percent(inner, inner_percent); pump(150);
        g_test_message("nested zoom outer=%u inner=%u net=%.4f", outer_percent, inner_percent,
            outer_percent * inner_percent / 10000.0);
        g_assert_cmpint(gtk_widget_get_allocated_width(outer.window), ==, 640);
        g_assert_cmpint(gtk_widget_get_allocated_height(outer.window), ==, 520);
        g_assert_cmpint(gtk_widget_get_allocated_width(nested.button), ==, 120);
        g_assert_cmpint(gtk_widget_get_allocated_height(outer.header), ==, 40);
        g_assert_cmpint(gtk_widget_get_allocated_height(outer.footer), ==, 30);
        gint x, y;
        nested_position(&outer, inner, nested.button, 60, 18, outer_percent, inner_percent, &x, &y);
        motion(x, y); mouse(1, TRUE); mouse(1, FALSE);
        g_assert_cmpuint(nested.clicks, ==, i + 1); g_assert_cmpuint(outer.clicks, ==, 0);
        nested_position(&outer, inner, nested.entry, 90, 16, outer_percent, inner_percent, &x, &y);
        motion(x, y); mouse(1, TRUE); mouse(1, FALSE); g_assert_true(gtk_widget_has_focus(nested.entry));
        key(XK_Control_L, TRUE); key(XK_a, TRUE); key(XK_a, FALSE); key(XK_Control_L, FALSE);
        key(XK_n, TRUE); key(XK_n, FALSE);
        g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(nested.entry)), ==, "n");
        for (guint point = 0; point < G_N_ELEMENTS(inside); ++point) {
            nested_position(&outer, inner, red, inside[point][0], inside[point][1], outer_percent, inner_percent, &x, &y);
            g_assert_true(native_red_at(outer.window, x, y));
            nested_position(&outer, inner, red, outside[point][0], outside[point][1], outer_percent, inner_percent, &x, &y);
            g_assert_false(native_red_at(outer.window, x, y));
        }
    }
    gtk_widget_destroy(outer.window); pump(30);
}
static void propagated_height_scrolls_to_bottom_button(void)
{
    Fixture f = {0};
    f.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_default_size(GTK_WINDOW(f.window), 300, 150);
    f.scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(f.scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(f.scroll), FALSE);
    gtk_container_add(GTK_CONTAINER(f.window), f.scroll);
    f.scale = radios_content_scale_new(); radios_content_scale_set_propagate_height(f.scale, TRUE);
    gtk_container_add(GTK_CONTAINER(f.scroll), f.scale);
    f.body = gtk_fixed_new(); gtk_widget_set_size_request(f.body, 1, 600);
    gtk_container_add(GTK_CONTAINER(f.scale), f.body);
    f.button = gtk_button_new_with_label("Bottom"); gtk_widget_set_size_request(f.button, 120, 36);
    gtk_fixed_put(GTK_FIXED(f.body), f.button, 20, 550);
    g_signal_connect(f.button, "clicked", G_CALLBACK(clicked), &f);
    gtk_widget_show_all(f.window); gtk_window_present(GTK_WINDOW(f.window)); pump(100);
    const guint percents[] = {50, 150};
    for (guint i = 0; i < G_N_ELEMENTS(percents); ++i) {
        radios_content_scale_set_percent(f.scale, percents[i]); pump(100);
        GtkAdjustment *adjustment = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(f.scroll));
        gdouble upper = gtk_adjustment_get_upper(adjustment), page = gtk_adjustment_get_page_size(adjustment);
        g_assert_cmpint(gtk_widget_get_allocated_width(f.window), ==, 300);
        g_assert_cmpint(gtk_widget_get_allocated_height(f.window), ==, 150);
        g_assert_cmpfloat(upper, >=, 600 * percents[i] / 100.0);
        g_assert_cmpfloat(upper, >, page);
        gtk_adjustment_set_value(adjustment, upper-page); pump(100);
        g_assert_cmpfloat(gtk_adjustment_get_value(adjustment), >, 0);
        gint x, y, origin_x, origin_y;
        position(&f, f.button, 60, 18, &x, &y);
        gdk_window_get_origin(gtk_widget_get_window(f.window), &origin_x, &origin_y);
        g_test_message("propagated zoom %u scroll %.0f/%.0f page %.0f bottom button (%d,%d)",
            percents[i], gtk_adjustment_get_value(adjustment), upper, page, x-origin_x, y-origin_y);
        g_assert_cmpint(x, >=, origin_x); g_assert_cmpint(x, <, origin_x+300);
        g_assert_cmpint(y, >=, origin_y); g_assert_cmpint(y, <, origin_y+150);
        motion(x, y); mouse(1, TRUE); mouse(1, FALSE);
        g_assert_cmpuint(f.clicks, ==, i+1);
    }
    gtk_widget_destroy(f.window); pump(30);
}
static gboolean magenta_draw(GtkWidget *widget, cairo_t *cr, gpointer user)
{
    (void)user; cairo_set_source_rgb(cr, 1, 0, 1); cairo_rectangle(cr, 0, 0,
        gtk_widget_get_allocated_width(widget), gtk_widget_get_allocated_height(widget)); cairo_fill(cr); return FALSE;
}
/* The DJ mixer/effector sections: zooming out shrinks the 100% layout instead
 * of spreading it over twice the row, zooming in keeps the natural layout and
 * lets the page scroll, and no gap ever shows a previous zoom's pixels. */
static void section_zoom_layout_and_gaps(void)
{
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, "#section-host { background-color: #102030; }", -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_default_size(GTK_WINDOW(window), 640, 360);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL); gtk_container_add(GTK_CONTAINER(window), scroll);
    GtkWidget *host = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_widget_set_name(host, "section-host");
    gtk_container_add(GTK_CONTAINER(scroll), host);
    GtkWidget *scale = radios_content_scale_new(); radios_content_scale_set_propagate_height(scale, TRUE);
    radios_content_scale_set_section(scale, TRUE); gtk_box_pack_start(GTK_BOX(host), scale, FALSE, FALSE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); gtk_container_add(GTK_CONTAINER(scale), row);
    GtkWidget *left = gtk_drawing_area_new(), *right = gtk_drawing_area_new();
    gtk_widget_set_size_request(left, 200, 80); gtk_widget_set_size_request(right, 200, 80);
    g_signal_connect(left, "draw", G_CALLBACK(magenta_draw), NULL); g_signal_connect(right, "draw", G_CALLBACK(magenta_draw), NULL);
    gtk_box_pack_start(GTK_BOX(row), left, FALSE, FALSE, 0); gtk_box_pack_end(GTK_BOX(row), right, FALSE, FALSE, 0);
    gtk_widget_show_all(window); gtk_window_present(GTK_WINDOW(window)); pump(150);
    const guint percents[] = {100, 150, 50, 125, 75};
    for (guint i = 0; i < G_N_ELEMENTS(percents); ++i) {
        radios_content_scale_set_percent(scale, percents[i]); pump(150);
        gdouble ratio = percents[i] / 100.0; gint width = gtk_widget_get_allocated_width(scale);
        gint layout = gtk_widget_get_allocated_width(row);
        g_test_message("section zoom %u: physical %d, logical %d", percents[i], width, layout);
        /* Never wider than the row on zoom-out, never under the natural 400. */
        g_assert_cmpint(layout, <=, MAX(400, width));
        g_assert_cmpint(layout, >=, 400);
        g_assert_cmpint(width, >=, (gint)ceil(400 * ratio));
        g_assert_cmpint(gtk_widget_get_allocated_height(scale), ==, (gint)ceil(80 * ratio));
        GdkPixbuf *image = gdk_pixbuf_get_from_window(gtk_widget_get_window(window), 0, 0,
            gtk_widget_get_allocated_width(window), gtk_widget_get_allocated_height(window));
        g_assert_nonnull(image);
        gint x = 0, y = 0; g_assert_true(gtk_widget_translate_coordinates(scale, window, 0, 0, &x, &y));
        const guchar *pixels = gdk_pixbuf_read_pixels(image); gint stride = gdk_pixbuf_get_rowstride(image), channels = gdk_pixbuf_get_n_channels(image);
        /* The gap between the two blocks and the area right of a shrunk layout. */
        gint gap_x = x + (gint)(layout * ratio / 2), right_x = x + MIN(width, gtk_widget_get_allocated_width(window) - x) - 3;
        gint gap_y = y + (gint)(40 * ratio);
        const guchar *gap = pixels + gap_y * stride + gap_x * channels;
        g_assert_cmpint(gap[0], ==, 0x10); g_assert_cmpint(gap[1], ==, 0x20); g_assert_cmpint(gap[2], ==, 0x30);
        if (ratio < 1) {
            const guchar *outside = pixels + gap_y * stride + right_x * channels;
            g_assert_cmpint(outside[0], ==, 0x10); g_assert_cmpint(outside[1], ==, 0x20); g_assert_cmpint(outside[2], ==, 0x30);
        }
        g_object_unref(image);
    }
    gtk_widget_destroy(window); pump(30);
    gtk_style_context_remove_provider_for_screen(gdk_screen_get_default(), GTK_STYLE_PROVIDER(css)); g_object_unref(css);
}
/* Width and height zoom independently: layout, size request, input and the
 * painted pixels follow the horizontal percentage along x and the vertical
 * one along y. */
static void independent_axes_geometry_input_and_pixels(void)
{
    Fixture f = {0}; fixture_init(&f);
    GtkWidget *red = gtk_drawing_area_new(); gtk_widget_set_size_request(red, 100, 40);
    g_signal_connect(red, "draw", G_CALLBACK(magenta_draw), NULL); gtk_fixed_put(GTK_FIXED(f.body), red, 300, 200); gtk_widget_show(red);
    const guint pairs[][2] = {{100, 100}, {50, 150}, {150, 50}, {75, 125}, {125, 75}, {100, 100}};
    for (guint i = 0; i < G_N_ELEMENTS(pairs); ++i) {
        radios_content_scale_set_percents(f.scale, pairs[i][0], pairs[i][1]); pump(150);
        gdouble horizontal = pairs[i][0] / 100.0, vertical = pairs[i][1] / 100.0;
        g_assert_cmpuint(radios_content_scale_get_percent(f.scale), ==, pairs[i][0]);
        g_assert_cmpuint(radios_content_scale_get_vertical_percent(f.scale), ==, pairs[i][1]);
        g_test_message("axes %u x %u: scale %dx%d body %dx%d", pairs[i][0], pairs[i][1], gtk_widget_get_allocated_width(f.scale),
            gtk_widget_get_allocated_height(f.scale), gtk_widget_get_allocated_width(f.body), gtk_widget_get_allocated_height(f.body));
        g_assert_cmpint(gtk_widget_get_allocated_width(f.body), ==, (gint)floor(gtk_widget_get_allocated_width(f.scale) / horizontal));
        g_assert_cmpint(gtk_widget_get_allocated_height(f.body), ==, (gint)floor(gtk_widget_get_allocated_height(f.scale) / vertical));
        g_assert_cmpint(gtk_widget_get_allocated_width(f.button), ==, 120); g_assert_cmpint(gtk_widget_get_allocated_height(f.button), ==, 36);
        click_at(&f, f.button); g_assert_cmpuint(f.clicks, ==, i + 1);
        click_at(&f, f.entry); g_assert_true(gtk_widget_has_focus(f.entry));
        /* The 100x40 block at logical (300, 200) is 100*h x 40*v physical pixels. */
        GdkPixbuf *image = gdk_pixbuf_get_from_window(gtk_widget_get_window(f.window), 0, 0,
            gtk_widget_get_allocated_width(f.window), gtk_widget_get_allocated_height(f.window));
        g_assert_nonnull(image);
        gint sx = 0, sy = 0; g_assert_true(gtk_widget_translate_coordinates(f.scale, f.window, 0, 0, &sx, &sy));
        const guchar *pixels = gdk_pixbuf_read_pixels(image); gint stride = gdk_pixbuf_get_rowstride(image), channels = gdk_pixbuf_get_n_channels(image);
        gint left = sx + (gint)ceil(300 * horizontal), right = sx + (gint)floor(400 * horizontal), top = sy + (gint)ceil(200 * vertical), bottom = sy + (gint)floor(240 * vertical);
        const guchar *inside = pixels + ((top + bottom) / 2) * stride + ((left + right) / 2) * channels;
        g_assert_cmpint(inside[0], ==, 0xff); g_assert_cmpint(inside[1], ==, 0x00); g_assert_cmpint(inside[2], ==, 0xff);
        const guchar *beside = pixels + ((top + bottom) / 2) * stride + (right + 4) * channels;
        const guchar *below = pixels + (bottom + 4) * stride + ((left + right) / 2) * channels;
        g_assert_false(beside[0] == 0xff && beside[1] == 0x00 && beside[2] == 0xff);
        g_assert_false(below[0] == 0xff && below[1] == 0x00 && below[2] == 0xff);
        g_object_unref(image);
    }
    radios_content_scale_set_percents(f.scale, 1, 900);
    g_assert_cmpuint(radios_content_scale_get_percent(f.scale), ==, 50);
    g_assert_cmpuint(radios_content_scale_get_vertical_percent(f.scale), ==, 150);
    radios_content_scale_set_percent(f.scale, 80);
    g_assert_cmpuint(radios_content_scale_get_percent(f.scale), ==, 80);
    g_assert_cmpuint(radios_content_scale_get_vertical_percent(f.scale), ==, 80);
    gtk_widget_destroy(f.window); pump(30);
}
/* A section keeps its own vertical zoom: height changes only with the vertical percentage. */
static void section_vertical_zoom_is_independent(void)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_default_size(GTK_WINDOW(window), 640, 360);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL); gtk_container_add(GTK_CONTAINER(window), scroll);
    GtkWidget *host = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_container_add(GTK_CONTAINER(scroll), host);
    GtkWidget *scale = radios_content_scale_new(); radios_content_scale_set_propagate_height(scale, TRUE);
    radios_content_scale_set_section(scale, TRUE); gtk_box_pack_start(GTK_BOX(host), scale, FALSE, FALSE, 0);
    GtkWidget *block = gtk_drawing_area_new(); gtk_widget_set_size_request(block, 400, 80); gtk_container_add(GTK_CONTAINER(scale), block);
    gtk_widget_show_all(window); gtk_window_present(GTK_WINDOW(window)); pump(150);
    const guint pairs[][2] = {{100, 100}, {150, 50}, {50, 150}, {75, 125}, {100, 100}};
    for (guint i = 0; i < G_N_ELEMENTS(pairs); ++i) {
        radios_content_scale_set_percents(scale, pairs[i][0], pairs[i][1]); pump(150);
        g_test_message("section axes %u x %u: %dx%d", pairs[i][0], pairs[i][1], gtk_widget_get_allocated_width(scale), gtk_widget_get_allocated_height(scale));
        g_assert_cmpint(gtk_widget_get_allocated_height(scale), ==, (gint)ceil(80 * pairs[i][1] / 100.0));
        g_assert_cmpint(gtk_widget_get_allocated_width(scale), >=, (gint)ceil(400 * pairs[i][0] / 100.0));
        g_assert_cmpint(gtk_widget_get_allocated_height(block), ==, 80);
    }
    gtk_widget_destroy(window); pump(30);
}
/* Idle content stays idle: neither the draw, the damage nor the allocation
 * path of the scale may queue another pass by itself (a feedback loop between
 * the offscreen content and its embedder would keep the main thread busy).
 * A repaint request paints the content once, an allocation pass that changes
 * nothing repaints nothing, and a sibling label updating like a clock does
 * not repaint untouched content. */
typedef struct { guint area_draws, scale_draws, scale_allocations; } Counters;
static gboolean counted_area_draw(GtkWidget *widget, cairo_t *cr, gpointer user)
{ (void)widget; (void)cr; ++((Counters *)user)->area_draws; return FALSE; }
static gboolean counted_scale_draw(GtkWidget *widget, cairo_t *cr, gpointer user)
{ (void)widget; (void)cr; ++((Counters *)user)->scale_draws; return FALSE; }
static void counted_scale_allocate(GtkWidget *widget, GtkAllocation *allocation, gpointer user)
{ (void)widget; (void)allocation; ++((Counters *)user)->scale_allocations; }
static void idle_content_is_not_repainted_or_reallocated(void)
{
    const guint percents[][2] = {{100, 100}, {75, 125}, {150, 50}};
    for (guint p = 0; p < G_N_ELEMENTS(percents); ++p) {
        Counters counters = {0};
        GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_default_size(GTK_WINDOW(window), 640, 480);
        GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_container_add(GTK_CONTAINER(window), outer);
        GtkWidget *scale = radios_content_scale_new(); radios_content_scale_set_propagate_height(scale, TRUE);
        radios_content_scale_set_section(scale, p == 1); gtk_box_pack_start(GTK_BOX(outer), scale, FALSE, FALSE, 0);
        GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4); gtk_container_add(GTK_CONTAINER(scale), body);
        GtkWidget *label = gtk_label_new("00:00:00"); gtk_widget_set_halign(label, GTK_ALIGN_START);
        gtk_box_pack_start(GTK_BOX(body), label, FALSE, FALSE, 0);
        GtkWidget *area = gtk_drawing_area_new(); gtk_widget_set_size_request(area, 300, 120);
        gtk_box_pack_start(GTK_BOX(body), area, FALSE, FALSE, 0);
        g_signal_connect(area, "draw", G_CALLBACK(counted_area_draw), &counters);
        g_signal_connect(scale, "draw", G_CALLBACK(counted_scale_draw), &counters);
        g_signal_connect(scale, "size-allocate", G_CALLBACK(counted_scale_allocate), &counters);
        radios_content_scale_set_percents(scale, percents[p][0], percents[p][1]);
        gtk_widget_show_all(window); gtk_window_present(GTK_WINDOW(window)); pump(400);
        g_assert_cmpuint(counters.area_draws, >=, 1);
        counters = (Counters){0}; pump(600);
        g_test_message("idle %ux%u: area draws %u, scale draws %u, scale allocations %u", percents[p][0], percents[p][1],
            counters.area_draws, counters.scale_draws, counters.scale_allocations);
        g_assert_cmpuint(counters.area_draws, ==, 0); g_assert_cmpuint(counters.scale_allocations, ==, 0);
        g_assert_cmpuint(counters.scale_draws, <=, 2);
        counters = (Counters){0}; gtk_widget_queue_draw(area); pump(300);
        g_test_message("after one repaint request: area draws %u, scale draws %u, scale allocations %u",
            counters.area_draws, counters.scale_draws, counters.scale_allocations);
        g_assert_cmpuint(counters.area_draws, ==, 1); g_assert_cmpuint(counters.scale_allocations, ==, 0);
        g_assert_cmpuint(counters.scale_draws, >=, 2); g_assert_cmpuint(counters.scale_draws, <=, 4);
        counters = (Counters){0}; gtk_widget_queue_allocate(scale); pump(300);
        g_test_message("after an unchanged allocation: area draws %u, scale draws %u, scale allocations %u",
            counters.area_draws, counters.scale_draws, counters.scale_allocations);
        g_assert_cmpuint(counters.scale_allocations, ==, 1); g_assert_cmpuint(counters.area_draws, ==, 0);
        counters = (Counters){0};
        for (guint tick = 0; tick < 25; ++tick) {
            gchar text[16]; g_snprintf(text, sizeof text, "00:00:%02u", tick + 1);
            gtk_label_set_text(GTK_LABEL(label), text); pump(20);
        }
        g_test_message("after 25 label ticks: area draws %u, scale draws %u, scale allocations %u",
            counters.area_draws, counters.scale_draws, counters.scale_allocations);
        g_assert_cmpuint(counters.scale_allocations, >=, 1); g_assert_cmpuint(counters.area_draws, ==, 0);
        gtk_widget_destroy(window); pump(30);
    }
}
static void lifecycle(void)
{
    GtkWidget *scale = radios_content_scale_new(); g_object_ref_sink(scale);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_default_size(GTK_WINDOW(window), 300, 200); gtk_container_add(GTK_CONTAINER(window), scale);
    gtk_widget_show_all(window); pump(30);
    GtkWidget *child = gtk_entry_new(); gtk_container_add(GTK_CONTAINER(scale), child); gtk_widget_show(child); pump(30);
    radios_content_scale_set_percent(scale, 125); pump(30);
    gtk_container_remove(GTK_CONTAINER(scale), child); pump(30);
    gtk_widget_hide(window); gtk_widget_unrealize(window); pump(30);
    gtk_widget_show_all(window); pump(30);
    gtk_widget_destroy(window); g_object_unref(scale); pump(30);
}
int main(int argc, char **argv)
{
    gtk_test_init(&argc, &argv, NULL);
    g_test_add_func("/content-scale/real-pointer-keyboard-drag-scroll-geometry", geometry_input);
    g_test_add_func("/content-scale/native-pixels-popup-and-real-DnD", popup_drag_and_pixels);
    g_test_add_func("/content-scale/native-combo-menu-and-tree-selection", combo_menu_tree_selection);
    g_test_add_func("/content-scale/nested-real-pointer-keyboard-native-pixels", nested_geometry_input);
    g_test_add_func("/content-scale/propagated-height-scroll-bottom-real-click", propagated_height_scrolls_to_bottom_button);
    g_test_add_func("/content-scale/realize-late-child-remove-unrealize", lifecycle);
    g_test_add_func("/content-scale/section-zoom-layout-and-repainted-gaps", section_zoom_layout_and_gaps);
    g_test_add_func("/content-scale/independent-width-height-geometry-input-pixels", independent_axes_geometry_input_and_pixels);
    g_test_add_func("/content-scale/section-independent-vertical-zoom", section_vertical_zoom_is_independent);
    g_test_add_func("/content-scale/idle-content-no-repaint-no-reallocation-loop", idle_content_is_not_repainted_or_reallocated);
    return g_test_run();
}
