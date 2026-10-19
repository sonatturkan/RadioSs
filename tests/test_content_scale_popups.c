/* R3.5 / R10.3: menus, combo lists, popovers, tooltips and tree view popups of
 * widgets inside the zoomed content open at the place where their anchor is
 * drawn. The anchors live in a scrolled viewport, as on every page of the
 * application, so intermediate windows and the scroll offset are part of the
 * path. Real pointer and keyboard input: XTest on a private X server, or on
 * Wayland the virtual devices of a private headless GNOME Shell, which also
 * reports where the compositor really placed each popup
 * (content_scale_test_input.h). */
#include "radios/content_scale.h"
#include "content_scale_test_input.h"

static void pump(guint ms)
{
    gint64 end = g_get_monotonic_time() + ms * 1000;
    do { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); } while (g_get_monotonic_time() < end);
}
static void motion(gint x, gint y) { cs_motion(x, y); pump(30); }
static void mouse(guint button, gboolean press) { cs_button(button, press); pump(30); }
static void key(guint symbol) { cs_key(symbol, TRUE); pump(10); cs_key(symbol, FALSE); pump(30); }
static void escape(void) { key(XK_Escape); pump(150); }

static const guint zooms[][2] = {{100, 100}, {73, 73}, {125, 125}, {60, 140}, {140, 60}};

typedef struct {
    GtkWidget *window, *scale, *scroll, *body, *grid, *tree, *combo, *menu_button, *button_menu, *pop_button,
        *popover, *tip_button, *entry, *inner, *inner_combo, *grid_menu, *tree_menu, *activated;
    GtkTreeViewColumn *column; GtkTreePath *pressed; guint pops;
} Popups;

/* The rectangle a widget occupies on screen, through the offscreen windows. */
static GdkRectangle drawn(GtkWidget *widget) { return cs_drawn(widget); }
/* Screen origin of a widget in a popup (an unscaled window of its own). */
static void origin(GtkWidget *widget, gint *x, gint *y) { cs_popup_origin(widget, x, y); }
static void pointer(gint *x, gint *y) { cs_pointer(x, y); }
static void near(const gchar *what, gint actual_x, gint actual_y, gint want_x, gint want_y, gint tolerance)
{
    g_test_message("%-30s at (%4d,%4d) want (%4d,%4d) delta (%+d,%+d)", what, actual_x, actual_y, want_x, want_y,
        actual_x - want_x, actual_y - want_y);
    g_assert_cmpint(ABS(actual_x - want_x), <=, tolerance);
    g_assert_cmpint(ABS(actual_y - want_y), <=, tolerance);
}
static void item_activated(GtkMenuItem *item, gpointer data) { ((Popups *)data)->activated = GTK_WIDGET(item); }
static GtkWidget *menu_new(Popups *p, const gchar *prefix)
{
    GtkWidget *menu = gtk_menu_new();
    for (guint i = 0; i < 4; ++i) {
        gchar *label = g_strdup_printf("%s %u", prefix, i); GtkWidget *item = gtk_menu_item_new_with_label(label);
        g_signal_connect(item, "activate", G_CALLBACK(item_activated), p);
        gtk_menu_shell_append(GTK_MENU_SHELL(menu), item); g_free(label);
    }
    gtk_widget_show_all(menu); return menu;
}
/* Like the automation calendar: a right click pops the attached menu at the
 * pointer, the Menu key pops it under the widget. */
static gboolean grid_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    Popups *p = data;
    if (event->type != GDK_BUTTON_PRESS || event->button != 3) return FALSE;
    gtk_widget_grab_focus(widget); ++p->pops;
    gtk_menu_popup_at_pointer(GTK_MENU(p->grid_menu), (GdkEvent *)event); return TRUE;
}
static gboolean grid_key(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    Popups *p = data;
    if (event->keyval != GDK_KEY_Menu) return FALSE;
    ++p->pops;
    gtk_menu_popup_at_widget(GTK_MENU(p->grid_menu), widget, GDK_GRAVITY_SOUTH_WEST, GDK_GRAVITY_NORTH_WEST, (GdkEvent *)event);
    return TRUE;
}
static gboolean tree_press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    Popups *p = data;
    if (event->type != GDK_BUTTON_PRESS || event->button != 3) return FALSE;
    g_clear_pointer(&p->pressed, gtk_tree_path_free);
    gtk_tree_view_get_path_at_pos(GTK_TREE_VIEW(widget), (gint)event->x, (gint)event->y, &p->pressed, NULL, NULL, NULL);
    ++p->pops; gtk_menu_popup_at_pointer(GTK_MENU(p->tree_menu), (GdkEvent *)event); return TRUE;
}
static void popups_init(Popups *p)
{
    p->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_default_size(GTK_WINDOW(p->window), 900, 700);
    gtk_window_set_title(GTK_WINDOW(p->window), "radios-content-scale-popups");
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_container_add(GTK_CONTAINER(p->window), outer);
    GtkWidget *header = gtk_drawing_area_new(); gtk_widget_set_size_request(header, 1, 40);
    gtk_box_pack_start(GTK_BOX(outer), header, FALSE, FALSE, 0);
    p->scale = radios_content_scale_new(); gtk_box_pack_start(GTK_BOX(outer), p->scale, TRUE, TRUE, 0);
    p->scroll = gtk_scrolled_window_new(NULL, NULL); gtk_container_add(GTK_CONTAINER(p->scale), p->scroll);
    p->body = gtk_fixed_new(); gtk_widget_set_size_request(p->body, 1600, 1400);
    gtk_container_add(GTK_CONTAINER(p->scroll), p->body); /* a GtkViewport with its own windows in between */
    p->combo = gtk_combo_box_text_new(); gtk_widget_set_size_request(p->combo, 140, 34);
    for (guint i = 0; i < 6; ++i) {
        gchar *label = g_strdup_printf("Seçenek %u", i); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->combo), label, label); g_free(label);
    }
    gtk_fixed_put(GTK_FIXED(p->body), p->combo, 40, 70);
    p->menu_button = gtk_menu_button_new(); gtk_button_set_label(GTK_BUTTON(p->menu_button), "Menü");
    gtk_widget_set_size_request(p->menu_button, 110, 34); p->button_menu = menu_new(p, "Düğme");
    gtk_menu_button_set_popup(GTK_MENU_BUTTON(p->menu_button), p->button_menu);
    gtk_fixed_put(GTK_FIXED(p->body), p->menu_button, 220, 70);
    p->pop_button = gtk_menu_button_new(); gtk_button_set_label(GTK_BUTTON(p->pop_button), "Açılır");
    gtk_widget_set_size_request(p->pop_button, 110, 34); p->popover = gtk_popover_new(p->pop_button);
    GtkWidget *pop_label = gtk_label_new("açılır alan"); gtk_container_add(GTK_CONTAINER(p->popover), pop_label); gtk_widget_show(pop_label);
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(p->pop_button), p->popover);
    gtk_fixed_put(GTK_FIXED(p->body), p->pop_button, 360, 70);
    p->tip_button = gtk_button_new_with_label("İpucu"); gtk_widget_set_size_request(p->tip_button, 110, 34);
    gtk_widget_set_tooltip_text(p->tip_button, "ipucu metni"); gtk_fixed_put(GTK_FIXED(p->body), p->tip_button, 500, 70);
    p->entry = gtk_entry_new(); gtk_widget_set_size_request(p->entry, 150, 32); gtk_fixed_put(GTK_FIXED(p->body), p->entry, 480, 150);
    p->inner = radios_content_scale_new(); gtk_widget_set_size_request(p->inner, 260, 70);
    gtk_fixed_put(GTK_FIXED(p->body), p->inner, 40, 380);
    GtkWidget *inner_body = gtk_fixed_new(); gtk_container_add(GTK_CONTAINER(p->inner), inner_body);
    p->inner_combo = gtk_combo_box_text_new(); gtk_widget_set_size_request(p->inner_combo, 140, 34);
    for (guint i = 0; i < 4; ++i) {
        gchar *label = g_strdup_printf("İç %u", i); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(p->inner_combo), label, label); g_free(label);
    }
    gtk_fixed_put(GTK_FIXED(inner_body), p->inner_combo, 20, 14);
    p->grid = gtk_drawing_area_new(); gtk_widget_set_size_request(p->grid, 220, 110); gtk_widget_set_can_focus(p->grid, TRUE);
    gtk_widget_add_events(p->grid, GDK_BUTTON_PRESS_MASK | GDK_KEY_PRESS_MASK);
    p->grid_menu = menu_new(p, "Blok"); gtk_menu_attach_to_widget(GTK_MENU(p->grid_menu), p->grid, NULL);
    g_signal_connect(p->grid, "button-press-event", G_CALLBACK(grid_press), p);
    g_signal_connect(p->grid, "key-press-event", G_CALLBACK(grid_key), p);
    gtk_fixed_put(GTK_FIXED(p->body), p->grid, 40, 150);
    GtkListStore *model = gtk_list_store_new(1, G_TYPE_STRING);
    for (guint i = 0; i < 12; ++i) {
        GtkTreeIter iter; gchar *label = g_strdup_printf("Satır %u", i);
        gtk_list_store_append(model, &iter); gtk_list_store_set(model, &iter, 0, label, -1); g_free(label);
    }
    p->tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(model)); g_object_unref(model);
    p->column = gtk_tree_view_column_new_with_attributes("Satırlar", gtk_cell_renderer_text_new(), "text", 0, NULL);
    gtk_tree_view_append_column(GTK_TREE_VIEW(p->tree), p->column); gtk_widget_set_size_request(p->tree, 170, 200);
    p->tree_menu = menu_new(p, "Satır menüsü"); gtk_menu_attach_to_widget(GTK_MENU(p->tree_menu), p->tree, NULL);
    g_signal_connect(p->tree, "button-press-event", G_CALLBACK(tree_press), p);
    gtk_fixed_put(GTK_FIXED(p->body), p->tree, 300, 150);
    gtk_widget_show_all(p->window); gtk_window_present(GTK_WINDOW(p->window)); pump(300);
}
static void popups_free(Popups *p)
{
    g_clear_pointer(&p->pressed, gtk_tree_path_free);
    gtk_widget_destroy(p->grid_menu); gtk_widget_destroy(p->tree_menu);
    gtk_widget_destroy(p->window); pump(50);
}
/* A zoom with a scrolled viewport: the anchors are offset by the scroll
 * position, which GTK's own offscreen path loses. */
static void zoom(Popups *p, guint horizontal, guint vertical)
{
    radios_content_scale_set_percents(p->scale, horizontal, vertical); radios_content_scale_set_percent(p->inner, 80); pump(250);
    GtkAdjustment *h = gtk_scrolled_window_get_hadjustment(GTK_SCROLLED_WINDOW(p->scroll));
    GtkAdjustment *v = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(p->scroll));
    gtk_adjustment_set_value(h, 12); gtk_adjustment_set_value(v, 30); pump(200);
    g_test_message("zoom %u x %u, viewport scrolled to (%.0f,%.0f)", horizontal, vertical, gtk_adjustment_get_value(h), gtk_adjustment_get_value(v));
    /* The test's own geometry: drawn rectangle = scale origin + (logical - scroll) * zoom. */
    gint sx, sy; cs_screen(gtk_widget_get_window(p->scale), 0, 0, &sx, &sy);
    GdkRectangle grid = drawn(p->grid);
    g_assert_cmpint(ABS(grid.x - (sx + (gint)llround((40 - gtk_adjustment_get_value(h)) * horizontal / 100.0))), <=, 1);
    g_assert_cmpint(ABS(grid.y - (sy + (gint)llround((150 - gtk_adjustment_get_value(v)) * vertical / 100.0))), <=, 1);
    g_assert_cmpint(ABS(grid.width - (gint)llround(220 * horizontal / 100.0)), <=, 1);
}
static GtkWidget *combo_menu(GtkWidget *combo)
{
    AtkObject *accessible = gtk_combo_box_get_popup_accessible(GTK_COMBO_BOX(combo));
    GtkWidget *menu = gtk_accessible_get_widget(GTK_ACCESSIBLE(accessible)); g_assert_true(GTK_IS_MENU(menu));
    return menu;
}
static void check_combo(Popups *p, GtkWidget *combo, const gchar *what)
{
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo), 1); pump(50);
    GdkRectangle r = drawn(combo); motion(r.x + r.width / 2, r.y + r.height / 2); mouse(1, TRUE); mouse(1, FALSE); pump(250);
    gboolean shown = FALSE; g_object_get(combo, "popup-shown", &shown, NULL); g_assert_true(shown);
    GtkWidget *menu = combo_menu(combo); g_assert_true(gtk_widget_get_mapped(menu));
    /* The active item opens over the combo: WEST of the drawn box plus GTK's own item offset. */
    gint dx = 0, dy = 0, mx, my; g_object_get(menu, "rect-anchor-dx", &dx, "rect-anchor-dy", &dy, NULL);
    origin(menu, &mx, &my); near(what, mx, my, r.x + dx, r.y + r.height / 2 + dy, 2);
    /* Choosing an item there selects it. */
    GList *items = gtk_container_get_children(GTK_CONTAINER(menu)); GtkWidget *third = g_list_nth_data(items, 3);
    gint ix, iy; origin(third, &ix, &iy); g_list_free(items);
    motion(ix + 20, iy + gtk_widget_get_allocated_height(third) / 2); mouse(1, TRUE); mouse(1, FALSE); pump(200);
    g_assert_cmpint(gtk_combo_box_get_active(GTK_COMBO_BOX(combo)), ==, 3);
    g_object_get(combo, "popup-shown", &shown, NULL); g_assert_false(shown);
    (void)p;
}

static void context_menus(gboolean map_only)
{
    if (map_only) g_setenv("RADIOS_CONTENT_SCALE_POPUP_MAP_ONLY", "1", TRUE);
    Popups p = {0}; popups_init(&p);
    for (guint z = 0; z < G_N_ELEMENTS(zooms); ++z) {
        zoom(&p, zooms[z][0], zooms[z][1]);
        gint px, py, mx, my;
        /* 1. Right click on a calendar-like grid: the menu opens at the pointer. */
        GdkRectangle g = drawn(p.grid);
        motion(g.x + g.width * 6 / 10, g.y + g.height * 4 / 10); mouse(3, TRUE); mouse(3, FALSE); pump(250);
        g_assert_true(gtk_widget_get_mapped(p.grid_menu));
        pointer(&px, &py); origin(p.grid_menu, &mx, &my); near("grid right click", mx, my, px + 1, py + 1, 3);
        /* A shown menu that GTK repositions stays there. */
        gtk_menu_reposition(GTK_MENU(p.grid_menu)); pump(100);
        origin(p.grid_menu, &mx, &my); near("grid menu repositioned", mx, my, px + 1, py + 1, 3);
        /* Its items are where they are drawn. */
        GList *items = gtk_container_get_children(GTK_CONTAINER(p.grid_menu)); GtkWidget *second = g_list_nth_data(items, 2);
        gint ix, iy; origin(second, &ix, &iy); g_list_free(items); p.activated = NULL;
        motion(ix + 20, iy + gtk_widget_get_allocated_height(second) / 2); mouse(1, TRUE); mouse(1, FALSE); pump(200);
        g_assert_true(p.activated == second); g_assert_false(gtk_widget_get_mapped(p.grid_menu));
        /* 2. The Menu key opens it under the widget. */
        motion(g.x + 4, g.y + 4); mouse(1, TRUE); mouse(1, FALSE);
        gtk_widget_grab_focus(p.grid); pump(50); key(XK_Menu); pump(250);
        g_assert_true(gtk_widget_get_mapped(p.grid_menu));
        origin(p.grid_menu, &mx, &my); near("grid Menu key (SW/NW)", mx, my, g.x, g.y + g.height, 2);
        escape();
        /* 3. Right click on a tree row: the clicked row and a menu at the pointer. */
        GtkTreePath *path = gtk_tree_path_new_from_indices(3, -1); GdkRectangle cell;
        gtk_tree_view_get_background_area(GTK_TREE_VIEW(p.tree), path, p.column, &cell);
        gint wx, wy; gtk_tree_view_convert_bin_window_to_widget_coords(GTK_TREE_VIEW(p.tree), cell.x + cell.width / 2, cell.y + cell.height / 2, &wx, &wy);
        gint rx, ry; cs_screen(gtk_widget_get_window(p.tree), wx, wy, &rx, &ry);
        motion(rx, ry); mouse(3, TRUE); mouse(3, FALSE); pump(250);
        g_assert_nonnull(p.pressed); g_assert_cmpint(gtk_tree_path_compare(p.pressed, path), ==, 0); gtk_tree_path_free(path);
        g_assert_true(gtk_widget_get_mapped(p.tree_menu));
        pointer(&px, &py); origin(p.tree_menu, &mx, &my); near("tree row right click", mx, my, px + 1, py + 1, 3);
        escape();
        /* 4. GTK's own entry context menu. */
        GdkRectangle e = drawn(p.entry); motion(e.x + e.width / 3, e.y + e.height / 2); mouse(3, TRUE); mouse(3, FALSE); pump(300);
        GList *attached = gtk_menu_get_for_attach_widget(p.entry); g_assert_nonnull(attached);
        GtkWidget *entry_menu = attached->data; g_assert_true(gtk_widget_get_mapped(entry_menu));
        pointer(&px, &py); origin(entry_menu, &mx, &my); near("entry context menu", mx, my, px + 1, py + 1, 3);
        escape();
    }
    popups_free(&p);
    if (map_only) g_setenv("RADIOS_CONTENT_SCALE_POPUP_MAP_ONLY", "0", TRUE);
}
static void context_menus_open_at_anchor(void) { context_menus(FALSE); }
/* The re-anchoring done just before a popup window is shown, which is the
 * only chance on backends that place popups asynchronously (Wayland). */
static void context_menus_open_at_anchor_before_map(void) { context_menus(TRUE); }

static void combo_and_menu_button(void)
{
    Popups p = {0}; popups_init(&p);
    for (guint z = 0; z < G_N_ELEMENTS(zooms); ++z) {
        zoom(&p, zooms[z][0], zooms[z][1]);
        check_combo(&p, p.combo, "combo list");
        check_combo(&p, p.inner_combo, "nested-scale combo list");
        GdkRectangle b = drawn(p.menu_button); motion(b.x + b.width / 2, b.y + b.height / 2); mouse(1, TRUE); mouse(1, FALSE); pump(250);
        g_assert_true(gtk_widget_get_mapped(p.button_menu));
        gint mx, my; origin(p.button_menu, &mx, &my); near("menu button menu", mx, my, b.x, b.y + b.height, 2);
        escape();
    }
    popups_free(&p);
}

/* A popover the caller points at part of its widget (the routing graph's node
 * rename): the drawn part, also when shown again or pointed elsewhere. */
static void check_pointed_popover(GtkWidget *canvas, GtkWidget *popover, GdkRectangle node, guint horizontal, guint vertical, const gchar *what)
{
    gtk_popover_set_pointing_to(GTK_POPOVER(popover), &node);
    for (guint round = 0; round < 2; ++round) {
        gtk_popover_popup(GTK_POPOVER(popover)); pump(250); g_assert_true(gtk_widget_get_mapped(popover));
        GdkRectangle c = drawn(canvas), pop = drawn(popover);
        gint want_x = c.x + (gint)lround((node.x + node.width / 2.0) * horizontal / 100.0);
        gint want_y = c.y + (gint)lround((node.y + node.height) * vertical / 100.0);
        near(what, pop.x + pop.width / 2, pop.y, want_x, want_y, 2);
        gtk_popover_popdown(GTK_POPOVER(popover)); pump(200);
    }
}
static void popover_and_tooltip(void)
{
    Popups p = {0}; popups_init(&p);
    GtkWidget *canvas = gtk_drawing_area_new(); gtk_widget_set_size_request(canvas, 200, 120);
    gtk_fixed_put(GTK_FIXED(p.body), canvas, 300, 360); gtk_widget_show(canvas);
    GtkWidget *rename = gtk_popover_new(canvas), *rename_entry = gtk_entry_new();
    gtk_popover_set_position(GTK_POPOVER(rename), GTK_POS_BOTTOM);
    gtk_container_add(GTK_CONTAINER(rename), rename_entry); gtk_widget_show(rename_entry);
    for (guint z = 0; z < G_N_ELEMENTS(zooms); ++z) {
        zoom(&p, zooms[z][0], zooms[z][1]);
        check_pointed_popover(canvas, rename, (GdkRectangle){40, 30, 60, 20}, zooms[z][0], zooms[z][1], "pointed popover");
        check_pointed_popover(canvas, rename, (GdkRectangle){120, 70, 50, 24}, zooms[z][0], zooms[z][1], "pointed popover moved");
        /* The popover points at the middle of the drawn button, just below it. */
        GdkRectangle b = drawn(p.pop_button); motion(b.x + b.width / 2, b.y + b.height / 2); mouse(1, TRUE); mouse(1, FALSE); pump(300);
        g_assert_true(gtk_widget_get_mapped(p.popover));
        GdkRectangle pop = drawn(p.popover);
        near("popover", pop.x + pop.width / 2, pop.y, b.x + b.width / 2, b.y + b.height, 2);
        escape(); pump(200); g_assert_false(gtk_widget_get_mapped(p.popover));
        /* The tooltip opens below the drawn button, centred while the pointer is near the centre. */
        GdkRectangle t = drawn(p.tip_button);
        motion(t.x + t.width / 2 - 12, t.y + t.height / 2); motion(t.x + t.width / 2 - 10, t.y + t.height / 2); pump(1100);
        GtkWidget *tip = NULL; GList *tops = gtk_window_list_toplevels();
        for (GList *i = tops; i; i = i->next)
            if (gtk_widget_get_mapped(i->data) && gtk_window_get_type_hint(i->data) == GDK_WINDOW_TYPE_HINT_TOOLTIP) tip = i->data;
        g_list_free(tops); g_assert_nonnull(tip);
        gint cursor = 0; g_object_get(gtk_settings_get_default(), "gtk-cursor-theme-size", &cursor, NULL);
        if (!cursor) cursor = gdk_display_get_default_cursor_size(gdk_display_get_default());
        gint padding = MAX(4, cursor - 32); GdkRectangle frame = cs_popup_frame(tip);
        near("tooltip", frame.x + frame.width / 2, frame.y, t.x + t.width / 2, t.y + t.height + padding, 2);
        motion(4, 4); pump(400);
    }
    popups_free(&p);
}

/* Typing in a tree view opens GTK's search entry at the drawn bottom-right corner. */
static void tree_search_entry(void)
{
    Popups p = {0}; popups_init(&p);
    for (guint z = 0; z < G_N_ELEMENTS(zooms); ++z) {
        zoom(&p, zooms[z][0], zooms[z][1]);
        GdkRectangle t = drawn(p.tree);
        GtkTreePath *path = gtk_tree_path_new_from_indices(1, -1); GdkRectangle cell;
        gtk_tree_view_get_background_area(GTK_TREE_VIEW(p.tree), path, p.column, &cell); gtk_tree_path_free(path);
        gint wx, wy; gtk_tree_view_convert_bin_window_to_widget_coords(GTK_TREE_VIEW(p.tree), cell.x + 10, cell.y + cell.height / 2, &wx, &wy);
        gint rx, ry; cs_screen(gtk_widget_get_window(p.tree), wx, wy, &rx, &ry);
        motion(rx, ry); mouse(1, TRUE); mouse(1, FALSE); pump(100); g_assert_true(gtk_widget_has_focus(p.tree));
        key(XK_s); pump(300);
        GtkWidget *search = NULL; GList *tops = gtk_window_list_toplevels();
        for (GList *i = tops; i; i = i->next)
            if (i->data != p.window && gtk_widget_get_mapped(i->data) && gtk_window_get_window_type(i->data) == GTK_WINDOW_POPUP &&
                gtk_window_get_type_hint(i->data) == GDK_WINDOW_TYPE_HINT_UTILITY) search = i->data;
        g_list_free(tops); g_assert_nonnull(search);
        /* Wayland maps this window as a subsurface at its position in the parent surface. */
        gint sx, sy; gdk_window_get_origin(gtk_widget_get_window(search), &sx, &sy);
        if (cs_wayland()) { gint ox, oy; cs_window_origin(p.window, &ox, &oy); sx += ox; sy += oy; }
        GtkRequisition size; gtk_widget_get_preferred_size(search, &size, NULL);
        near("tree search entry", sx, sy, t.x + t.width - size.width, t.y + t.height, 2);
        escape();
    }
    popups_free(&p);
}

int main(int argc, char **argv)
{
    gtk_test_init(&argc, &argv, NULL);
    g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
    g_test_add_func("/content-scale-module/popup-context-menus-at-pointer-and-widget", context_menus_open_at_anchor);
    g_test_add_func("/content-scale-module/popup-context-menus-before-map", context_menus_open_at_anchor_before_map);
    g_test_add_func("/content-scale-module/popup-combo-lists-and-menu-button", combo_and_menu_button);
    g_test_add_func("/content-scale-module/popup-popover-and-tooltip", popover_and_tooltip);
    g_test_add_func("/content-scale-module/popup-tree-search-entry", tree_search_entry);
    return g_test_run();
}
