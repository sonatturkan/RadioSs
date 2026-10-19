#include "radios/content_scale.h"
#include <math.h>
#ifdef GDK_WINDOWING_X11
#include <gdk/gdkx.h>
#endif

struct _RadioSContentScale {
    GtkBin parent_instance;
    GdkWindow *offscreen;
    guint percent, vertical_percent; /* independent horizontal and vertical zoom */
    gboolean propagate_height, section;
};
G_DEFINE_TYPE(RadioSContentScale, radios_content_scale, GTK_TYPE_BIN)

static gdouble factor(RadioSContentScale *self) { return self->percent / 100.0; }
static gdouble vfactor(RadioSContentScale *self) { return self->vertical_percent / 100.0; }
static GtkWidget *child_of(GtkWidget *widget) { return gtk_bin_get_child(GTK_BIN(widget)); }
static gboolean visible_child(GtkWidget *widget)
{ GtkWidget *child = child_of(widget); return child && gtk_widget_get_visible(child); }
static gint scaled(gint value, gdouble ratio)
{ return (gint)MIN((gdouble)G_MAXINT, ceil(MAX(0, value) * ratio)); }
static gint logical(gint value, gdouble ratio)
{ return MAX(1, (gint)floor(MAX(1, value) / ratio)); }
/* Child layout width for a physical content width. A section zooms its own
 * 100% layout: zooming out keeps that layout (its row-wide arrangement just
 * gets narrower) and zooming in fits the row while it can; it is never laid
 * out narrower than its natural width, because wrapping labels make a minimum
 * width unreadable: the surrounding viewport scrolls instead. */
static gint layout_width(RadioSContentScale *self, gint physical)
{
    gint width = logical(physical, factor(self));
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(self));
    if (!self->section || !child || !gtk_widget_get_visible(child)) return width;
    gint natural = 0; gtk_widget_get_preferred_width(child, NULL, &natural);
    return MAX(MAX(1, natural), MIN(width, MAX(1, physical)));
}


static GdkWindow *pick_child(GdkWindow *window, gdouble x, gdouble y, gpointer user)
{
    (void)window; RadioSContentScale *self = user; GtkWidget *widget = GTK_WIDGET(self);
    GtkWidget *child = child_of(widget);
    if (!child || !gtk_widget_get_mapped(child)) return NULL;
    GtkAllocation allocation; gtk_widget_get_allocation(child, &allocation);
    return x >= 0 && y >= 0 && x < allocation.width * factor(self) &&
        y < allocation.height * vfactor(self) ? self->offscreen : NULL;
}
static void to_embedder(GdkWindow *window, gdouble x, gdouble y, gdouble *out_x, gdouble *out_y, gpointer user)
{
    (void)window; *out_x = x * factor(user); *out_y = y * vfactor(user);
}
static void from_embedder(GdkWindow *window, gdouble x, gdouble y, gdouble *out_x, gdouble *out_y, gpointer user)
{
    (void)window; *out_x = x / factor(user); *out_y = y / vfactor(user);
}
static void scale_realize(GtkWidget *widget)
{
    RadioSContentScale *self = RADIOS_CONTENT_SCALE(widget); GtkAllocation a;
    gtk_widget_get_allocation(widget, &a); gtk_widget_set_realized(widget, TRUE);
    guint border = gtk_container_get_border_width(GTK_CONTAINER(widget));
    GdkWindowAttr attr = {0};
    attr.window_type = GDK_WINDOW_CHILD; attr.wclass = GDK_INPUT_OUTPUT;
    attr.x = a.x + (gint)border; attr.y = a.y + (gint)border;
    attr.width = MAX(1, a.width - 2 * (gint)border); attr.height = MAX(1, a.height - 2 * (gint)border);
    attr.visual = gtk_widget_get_visual(widget);
    attr.event_mask = gtk_widget_get_events(widget) | GDK_EXPOSURE_MASK |
        GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK |
        GDK_SCROLL_MASK | GDK_SMOOTH_SCROLL_MASK | GDK_ENTER_NOTIFY_MASK | GDK_LEAVE_NOTIFY_MASK;
    GdkWindow *window = gdk_window_new(gtk_widget_get_parent_window(widget), &attr, GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL);
    gtk_widget_set_window(widget, window); gdk_window_set_user_data(window, widget);
    g_signal_connect(window, "pick-embedded-child", G_CALLBACK(pick_child), self);
    /* The same logical size the allocation uses, so the first allocation pass
     * after realizing (GTK allocates before it maps) finds it unchanged. */
    attr.window_type = GDK_WINDOW_OFFSCREEN; attr.x = attr.y = 0;
    attr.width = layout_width(self, attr.width); attr.height = logical(attr.height, vfactor(self));
    self->offscreen = gdk_window_new(gdk_screen_get_root_window(gtk_widget_get_screen(widget)), &attr,
        GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL);
    gtk_widget_register_window(widget, self->offscreen);
    GtkWidget *child = child_of(widget); if (child) gtk_widget_set_parent_window(child, self->offscreen);
    gdk_offscreen_window_set_embedder(self->offscreen, window);
    g_signal_connect(self->offscreen, "to-embedder", G_CALLBACK(to_embedder), self);
    g_signal_connect(self->offscreen, "from-embedder", G_CALLBACK(from_embedder), self);
    gdk_window_show(self->offscreen);
}
static void scale_unrealize(GtkWidget *widget)
{
    RadioSContentScale *self = RADIOS_CONTENT_SCALE(widget);
    if (self->offscreen) {
        gtk_widget_unregister_window(widget, self->offscreen);
        gdk_window_destroy(self->offscreen); self->offscreen = NULL;
    }
    GTK_WIDGET_CLASS(radios_content_scale_parent_class)->unrealize(widget);
}
static void scale_add(GtkContainer *container, GtkWidget *child)
{
    RadioSContentScale *self = RADIOS_CONTENT_SCALE(container);
    if (self->offscreen) gtk_widget_set_parent_window(child, self->offscreen);
    GTK_CONTAINER_CLASS(radios_content_scale_parent_class)->add(container, child);
}
static GtkSizeRequestMode scale_request_mode(GtkWidget *widget)
{
    return visible_child(widget) ? gtk_widget_get_request_mode(child_of(widget)) : GTK_SIZE_REQUEST_CONSTANT_SIZE;
}
static void preferred(GtkWidget *widget, gboolean width, gint for_size, gint *minimum, gint *natural)
{
    RadioSContentScale *self = RADIOS_CONTENT_SCALE(widget); gint low = 0, high = 0;
    guint border = gtk_container_get_border_width(GTK_CONTAINER(widget));
    if (visible_child(widget)) {
        GtkWidget *child = child_of(widget);
        if (for_size < 0) {
            if (width) gtk_widget_get_preferred_width(child, &low, &high);
            else gtk_widget_get_preferred_height(child, &low, &high);
        } else if (width) {
            gint space = logical(for_size - 2 * (gint)border, vfactor(self));
            gtk_widget_get_preferred_width_for_height(child, space, &low, &high);
        } else {
            gint space = layout_width(self, for_size - 2 * (gint)border);
            gtk_widget_get_preferred_height_for_width(child, space, &low, &high);
        }
    }
    /* Viewports remain compressible. Scrolled sections propagate their full
     * height so their parent scrolls instead of clipping their controls; a
     * section also requires its scaled natural width for the same reason. */
    gint kept = width ? (self->section ? high : 0) : (self->propagate_height ? low : 0);
    gdouble axis = width ? factor(self) : vfactor(self);
    if (minimum) *minimum = (kept ? MAX(1, scaled(kept, axis)) : 1) + 2 * (gint)border;
    if (natural) *natural = MAX(1, scaled(high, axis)) + 2 * (gint)border;
}
static void preferred_width(GtkWidget *widget, gint *minimum, gint *natural)
{ preferred(widget, TRUE, -1, minimum, natural); }
static void preferred_height(GtkWidget *widget, gint *minimum, gint *natural)
{ preferred(widget, FALSE, -1, minimum, natural); }
static void preferred_width_for_height(GtkWidget *widget, gint height, gint *minimum, gint *natural)
{ preferred(widget, TRUE, height, minimum, natural); }
static void preferred_height_for_width(GtkWidget *widget, gint width, gint *minimum, gint *natural)
{ preferred(widget, FALSE, width, minimum, natural); }
static void scale_allocate(GtkWidget *widget, GtkAllocation *a)
{
    RadioSContentScale *self = RADIOS_CONTENT_SCALE(widget);
    gtk_widget_set_allocation(widget, a);
    gint border = (gint)gtk_container_get_border_width(GTK_CONTAINER(widget));
    gint width = MAX(1, a->width - 2 * border), height = MAX(1, a->height - 2 * border);
    if (gtk_widget_get_realized(widget))
        gdk_window_move_resize(gtk_widget_get_window(widget), a->x + border, a->y + border, width, height);
    GtkAllocation child_a = {0, 0, layout_width(self, width), logical(height, vfactor(self))};
    gboolean resized = self->offscreen && (gdk_window_get_width(self->offscreen) != child_a.width ||
        gdk_window_get_height(self->offscreen) != child_a.height);
    /* GDK invalidates a mapped offscreen window completely on every
     * move_resize, even to its current size, and an allocation pass reaches
     * this widget whenever any widget of the window queues a resize. An
     * unchanged logical size therefore leaves the surface alone; otherwise
     * each such pass repainted the entire content and kept the thread busy. */
    if (resized) gdk_window_move_resize(self->offscreen, 0, 0, child_a.width, child_a.height);
    if (visible_child(widget)) gtk_widget_size_allocate(child_of(widget), &child_a);
    if (resized) gdk_window_geometry_changed(self->offscreen);
    /* A resized offscreen surface starts from a copy of its old pixels, and
     * the embedder repaints completely so that no pixels of a previously
     * larger content remain beside the new one. */
    if (resized) { gdk_window_invalidate_rect(self->offscreen, NULL, TRUE); gtk_widget_queue_draw(widget); }
}
/* Repainted offscreen content is blitted again by the embedder: only the
 * damaged area, scaled to physical pixels, with a margin for the bilinear
 * filter. The embedder never repaints the offscreen, so this cannot loop. */
static gboolean scale_damage(GtkWidget *widget, GdkEventExpose *event)
{
    RadioSContentScale *self = RADIOS_CONTENT_SCALE(widget);
    GdkWindow *window = gtk_widget_get_window(widget);
    if (!window) return TRUE;
    GdkRectangle damaged = event->area, rect;
    if (damaged.width <= 0 || damaged.height <= 0) { gdk_window_invalidate_rect(window, NULL, FALSE); return TRUE; }
    rect.x = (gint)floor(damaged.x * factor(self)) - 2;
    rect.y = (gint)floor(damaged.y * vfactor(self)) - 2;
    rect.width = (gint)ceil((damaged.x + damaged.width) * factor(self)) + 2 - rect.x;
    rect.height = (gint)ceil((damaged.y + damaged.height) * vfactor(self)) + 2 - rect.y;
    gdk_window_invalidate_rect(window, &rect, FALSE);
    return TRUE;
}
/* This widget owns its window and nothing beneath it is repainted for it.
 * Paint the nearest opaque ancestor background, so transparent gaps of a new
 * zoom or a moved viewport never show pixels left from a previous frame. */
static void paint_backdrop(GtkWidget *widget, cairo_t *cr, gint width, gint height)
{
    GdkRGBA color = {0, 0, 0, 0};
    for (GtkWidget *ancestor = widget; ancestor; ancestor = gtk_widget_get_parent(ancestor)) {
        GtkStyleContext *style = gtk_widget_get_style_context(ancestor); GdkRGBA *value = NULL;
        gtk_style_context_get(style, gtk_style_context_get_state(style), GTK_STYLE_PROPERTY_BACKGROUND_COLOR, &value, NULL);
        gboolean opaque = value && value->alpha >= 0.99;
        if (opaque) color = *value;
        if (value) gdk_rgba_free(value);
        if (opaque) break;
    }
    cairo_save(cr); cairo_set_operator(cr, color.alpha > 0 ? CAIRO_OPERATOR_SOURCE : CAIRO_OPERATOR_CLEAR);
    gdk_cairo_set_source_rgba(cr, &color); cairo_rectangle(cr, 0, 0, width, height); cairo_fill(cr); cairo_restore(cr);
}
static gboolean scale_draw(GtkWidget *widget, cairo_t *cr)
{
    RadioSContentScale *self = RADIOS_CONTENT_SCALE(widget);
    if (!self->offscreen) return FALSE;
    if (gtk_cairo_should_draw_window(cr, self->offscreen)) {
        paint_backdrop(widget, cr, gdk_window_get_width(self->offscreen), gdk_window_get_height(self->offscreen));
        gtk_render_background(gtk_widget_get_style_context(widget), cr, 0, 0,
            gdk_window_get_width(self->offscreen), gdk_window_get_height(self->offscreen));
        if (visible_child(widget)) gtk_container_propagate_draw(GTK_CONTAINER(widget), child_of(widget), cr);
    }
    if (gtk_cairo_should_draw_window(cr, gtk_widget_get_window(widget))) {
        GdkWindow *window = gtk_widget_get_window(widget);
        paint_backdrop(widget, cr, gdk_window_get_width(window), gdk_window_get_height(window));
        cairo_surface_t *surface = gdk_offscreen_window_get_surface(self->offscreen);
        if (surface) {
            cairo_save(cr); cairo_scale(cr, factor(self), vfactor(self));
            cairo_set_source_surface(cr, surface, 0, 0);
            cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
            cairo_paint(cr); cairo_restore(cr);
        }
    }
    return FALSE;
}

/* ---- Popups anchored in a zoomed content ---------------------------------
 * GTK 3 places a menu from its anchor window and rectangle. When that window
 * lies inside an offscreen window (the content of every RadioSContentScale),
 * gtk_menu_position() drops the rectangle, falls back to the allocation of
 * the popup_at_widget() widget or to the pointer position in the attach
 * widget, converts that with a single gdk_window_coords_to_parent() step and
 * then uses the result as embedder coordinates. Every window in between (a
 * scrolled viewport, a widget with its own window), the scroll offset and
 * the zoom are lost, so context menus and combo lists open at the unscaled,
 * unscrolled place of their anchor, far from what was clicked. Popovers and
 * tooltips translate the anchor origin correctly but keep its logical size,
 * and the tree view search entry uses the logical size of the tree window.
 * The functions below recompute each anchor in physical coordinates of the
 * toplevel and re-anchor the popup just before its window is shown (the only
 * moment for backends that place popups asynchronously, i.e. Wayland) and,
 * on X11, whenever GTK positions the menu or tooltip window. */
#define POPUP_MAP_ONLY "RADIOS_CONTENT_SCALE_POPUP_MAP_ONLY" /* test seam: no X11 re-anchoring */
static gboolean popup_correcting;
static GtkWidget *tooltip_widget; /* weak: the widget whose query-tooltip ran last */
static gboolean tooltip_keyboard;
static GtkTreeViewSearchPositionFunc default_search_position;

static gboolean scaled_window(GdkWindow *window)
{
    for (GdkWindow *w = window; w; w = gdk_window_get_effective_parent(w)) {
        if (gdk_window_get_window_type(w) != GDK_WINDOW_OFFSCREEN) continue;
        gpointer owner = NULL; gdk_window_get_user_data(w, &owner);
        if (RADIOS_IS_CONTENT_SCALE(owner)) return TRUE;
    }
    return FALSE;
}
/* @area of @window as drawn, in coordinates of the effective toplevel. */
static GdkWindow *physical_area(GdkWindow *window, const GdkRectangle *area, GdkRectangle *out)
{
    GdkWindow *toplevel = gdk_window_get_effective_toplevel(window);
    gdouble x1 = area->x, y1 = area->y, x2 = area->x + area->width, y2 = area->y + area->height;
    for (GdkWindow *w = window; w != toplevel; w = gdk_window_get_effective_parent(w)) {
        if (!w) return NULL;
        gdk_window_coords_to_parent(w, x1, y1, &x1, &y1);
        gdk_window_coords_to_parent(w, x2, y2, &x2, &y2);
    }
    out->x = (gint)lround(x1); out->y = (gint)lround(y1);
    out->width = MAX(1, (gint)lround(x2) - out->x); out->height = MAX(1, (gint)lround(y2) - out->y);
    return toplevel;
}
/* A widget's allocation in coordinates of gtk_widget_get_window(). */
static GdkRectangle widget_area(GtkWidget *widget)
{
    GtkAllocation a; gtk_widget_get_allocation(widget, &a);
    if (gtk_widget_get_has_window(widget) && gtk_widget_get_parent(widget)) a.x = a.y = 0;
    return (GdkRectangle){a.x, a.y, a.width, a.height};
}
static gboolean tooltip_window(GtkWidget *window)
{
    return GTK_IS_WINDOW(window) && (gtk_window_get_type_hint(GTK_WINDOW(window)) == GDK_WINDOW_TYPE_HINT_TOOLTIP ||
        g_str_equal(G_OBJECT_TYPE_NAME(window), "GtkTooltipWindow"));
}

/* The anchor GtkMenu keeps privately. GtkMenuPrivate of GTK 3.24 up to the
 * fields read here; the layout is checked once against the public
 * properties stored around them before any value is trusted. */
typedef struct {
    GtkWidget *parent_menu_item, *old_active_menu_item;
    GtkAccelGroup *accel_group; const char *accel_path;
    GtkMenuPositionFunc position_func; gpointer position_func_data; GDestroyNotify position_func_data_destroy;
    gint position_x, position_y;
    GdkWindow *rect_window; GdkRectangle rect; GtkWidget *widget;
    GdkGravity rect_anchor, menu_anchor; GdkAnchorHints anchor_hints;
    gint rect_anchor_dx, rect_anchor_dy; GdkWindowTypeHint menu_type_hint; gboolean emulated_move_to_rect;
    guint toggle_size, accel_size; GtkWidget *toplevel;
} MenuAnchor;
static MenuAnchor *menu_anchor_of(GtkMenu *menu) { return (MenuAnchor *)menu->priv; }
static gboolean menu_layout_known(void)
{
    static gint known = -1;
    if (known >= 0) return known == 1;
    known = 0;
    if (gtk_get_major_version() != 3 || gtk_get_minor_version() != 24) return FALSE;
    GtkWidget *menu = gtk_menu_new(); g_object_ref_sink(menu);
    GtkAccelGroup *group = gtk_accel_group_new(); const gchar *path = "<RadioSContentScale>/Layout";
    GdkAnchorHints hints = GDK_ANCHOR_FLIP_X | GDK_ANCHOR_RESIZE_Y;
    gtk_menu_set_accel_group(GTK_MENU(menu), group); gtk_menu_set_accel_path(GTK_MENU(menu), path);
    g_object_set(menu, "anchor-hints", hints, "rect-anchor-dx", 7919, "rect-anchor-dy", -7907,
        "menu-type-hint", GDK_WINDOW_TYPE_HINT_DND, NULL);
    const MenuAnchor *state = menu_anchor_of(GTK_MENU(menu));
    known = state->accel_group == group && state->accel_path == g_intern_string(path) &&
        state->position_func == NULL && state->rect_window == NULL && state->widget == NULL &&
        state->anchor_hints == hints && state->rect_anchor_dx == 7919 && state->rect_anchor_dy == -7907 &&
        state->menu_type_hint == GDK_WINDOW_TYPE_HINT_DND && state->toplevel == gtk_widget_get_parent(menu);
    gtk_widget_destroy(menu); g_object_unref(menu); g_object_unref(group);
    if (!known) g_message("RadioSContentScale: GtkMenu yerleşimi tanınmadı; ölçekli menü konumu düzeltilemiyor.");
    return known == 1;
}
static GdkGravity mirrored(GdkGravity gravity)
{
    switch (gravity) {
    case GDK_GRAVITY_NORTH_WEST: return GDK_GRAVITY_NORTH_EAST;
    case GDK_GRAVITY_NORTH_EAST: return GDK_GRAVITY_NORTH_WEST;
    case GDK_GRAVITY_WEST: return GDK_GRAVITY_EAST;
    case GDK_GRAVITY_EAST: return GDK_GRAVITY_WEST;
    case GDK_GRAVITY_SOUTH_WEST: return GDK_GRAVITY_SOUTH_EAST;
    case GDK_GRAVITY_SOUTH_EAST: return GDK_GRAVITY_SOUTH_WEST;
    default: return gravity;
    }
}
/* Places a menu like GTK places it outside a zoom: its own anchors, hints and
 * offsets, applied to the drawn anchor. @size restores the window size GTK
 * had before it constrained a wrongly placed menu. */
static gboolean reanchor_menu(GtkMenu *menu, const GdkRectangle *size)
{
    GtkWidget *popup = gtk_widget_get_parent(GTK_WIDGET(menu));
    if (!GTK_IS_WINDOW(popup) || !menu_layout_known()) return FALSE;
    MenuAnchor *state = menu_anchor_of(menu);
    GdkWindow *popup_window = gtk_widget_get_window(popup), *anchor = NULL;
    if (!popup_window || state->toplevel != popup || state->position_func) return FALSE;
    GdkRectangle area = {0}, rect; GtkWidget *direction = GTK_WIDGET(menu);
    if (state->rect_window) {
        anchor = state->rect_window; area = state->rect;
    } else if (state->widget && gtk_widget_get_realized(state->widget)) {
        anchor = gtk_widget_get_window(state->widget); area = widget_area(state->widget); direction = state->widget;
    }
    if (!anchor || !scaled_window(anchor)) return FALSE;
    GdkWindow *toplevel = physical_area(anchor, &area, &rect);
    if (!toplevel) return FALSE;
    GdkGravity rect_anchor = state->rect_anchor, popup_anchor = state->menu_anchor;
    if (gtk_widget_get_direction(direction) == GTK_TEXT_DIR_RTL) {
        rect_anchor = mirrored(rect_anchor); popup_anchor = mirrored(popup_anchor);
    }
    GdkAnchorHints hints = 0; gint dx = 0, dy = 0;
    g_object_get(menu, "anchor-hints", &hints, "rect-anchor-dx", &dx, "rect-anchor-dy", &dy, NULL);
    /* GTK keeps the anchor for every later gtk_menu_position() of the shown
     * menu (a combo box repositions it on each allocation). Keep it in
     * toplevel coordinates from now on, so GTK itself places the menu right
     * and never moves it away from under the pointer for a moment. GTK
     * mirrors these anchors for the menu's own text direction. */
    state->rect_window = toplevel; state->rect = rect; state->widget = NULL;
    gboolean menu_rtl = gtk_widget_get_direction(GTK_WIDGET(menu)) == GTK_TEXT_DIR_RTL;
    state->rect_anchor = menu_rtl ? mirrored(rect_anchor) : rect_anchor;
    state->menu_anchor = menu_rtl ? mirrored(popup_anchor) : popup_anchor;
    popup_correcting = TRUE;
    if (size && size->width > 0 && size->height > 0 &&
        (gdk_window_get_width(popup_window) != size->width || gdk_window_get_height(popup_window) != size->height))
        gdk_window_resize(popup_window, size->width, size->height);
    gdk_window_set_transient_for(popup_window, toplevel);
    gdk_window_move_to_rect(popup_window, &rect, rect_anchor, popup_anchor, hints, dx, dy);
    popup_correcting = FALSE;
    return TRUE;
}
/* gtk_tooltip_position() with the drawn rectangle of the tooltip widget. */
static gboolean reanchor_tooltip(GtkWidget *window)
{
    GtkWidget *widget = tooltip_widget;
    GdkWindow *tip = gtk_widget_get_window(window);
    if (!tip || !widget || !gtk_widget_get_realized(widget) || !scaled_window(gtk_widget_get_window(widget))) return FALSE;
    if (gtk_window_get_transient_for(GTK_WINDOW(window)) != GTK_WINDOW(gtk_widget_get_toplevel(widget))) return FALSE;
    GdkRectangle area = widget_area(widget), rect;
    GdkWindow *toplevel = physical_area(gtk_widget_get_window(widget), &area, &rect);
    if (!toplevel) return FALSE;
    GdkDisplay *display = gtk_widget_get_display(widget);
    gint cursor = 0; g_object_get(gtk_widget_get_settings(widget), "gtk-cursor-theme-size", &cursor, NULL);
    if (!cursor) cursor = (gint)gdk_display_get_default_cursor_size(display);
#ifdef GDK_WINDOWING_X11
    if (GDK_IS_X11_DISPLAY(display)) cursor /= gtk_widget_get_scale_factor(widget);
#endif
    GdkDevice *device = tooltip_keyboard ? NULL : gdk_seat_get_pointer(gdk_display_get_default_seat(display));
    gint padding = device ? MAX(4, cursor - 32) : 4, dx = 0;
    rect.x -= padding; rect.y -= padding; rect.width += 2 * padding; rect.height += 2 * padding;
    if (device) {
        gint x = 0, y = 0; gdk_window_get_device_position(toplevel, device, &x, &y, NULL);
        if (rect.height > 48 + 8) rect = (GdkRectangle){x - 4, y - 4, cursor, cursor};
        else {
            gint distance = x - (rect.x + rect.width / 2);
            if (distance > 32) dx = distance - 32; else if (distance < -32) dx = distance + 32;
        }
    }
    popup_correcting = TRUE;
    gdk_window_move_to_rect(tip, &rect, GDK_GRAVITY_SOUTH, GDK_GRAVITY_NORTH, GDK_ANCHOR_FLIP_Y | GDK_ANCHOR_SLIDE_X, dx, 0);
    popup_correcting = FALSE;
    return TRUE;
}
/* A popover translates its pointing rectangle's origin through the zoom but
 * keeps its size: point it at the drawn size of the same rectangle. The
 * caller's own rectangle (or none) is kept and restored outside a zoom. */
typedef struct { GdkRectangle written, original; gboolean has_original; } PopoverAnchor;
static void reanchor_popover(GtkPopover *popover)
{
    GtkWidget *relative = gtk_popover_get_relative_to(popover);
    PopoverAnchor *stored = g_object_get_data(G_OBJECT(popover), "radios-content-scale-pointing");
    GdkRectangle current; gboolean has = gtk_popover_get_pointing_to(popover, &current);
    gboolean ours = stored && has && gdk_rectangle_equal(&current, &stored->written);
    GdkRectangle base = ours ? stored->original : current; gboolean has_base = ours ? stored->has_original : has;
    if (!relative || !gtk_widget_get_realized(relative) || !scaled_window(gtk_widget_get_window(relative))) {
        if (ours) {
            if (has_base) gtk_popover_set_pointing_to(popover, &base); else g_object_set(popover, "pointing-to", NULL, NULL);
        }
        g_object_set_data(G_OBJECT(popover), "radios-content-scale-pointing", NULL); return;
    }
    if (!has_base) { GtkAllocation a; gtk_widget_get_allocation(relative, &a); base = (GdkRectangle){0, 0, a.width, a.height}; }
    GdkRectangle in_window = base, drawn, origin = widget_area(relative);
    in_window.x += origin.x; in_window.y += origin.y;
    if (!physical_area(gtk_widget_get_window(relative), &in_window, &drawn)) return;
    GdkRectangle wanted = {base.x, base.y, drawn.width, drawn.height};
    PopoverAnchor *next = g_new(PopoverAnchor, 1); *next = (PopoverAnchor){wanted, base, has_base};
    g_object_set_data_full(G_OBJECT(popover), "radios-content-scale-pointing", next, g_free);
    if (!has || !gdk_rectangle_equal(&current, &wanted)) gtk_popover_set_pointing_to(popover, &wanted);
}
/* gtk_tree_view_search_position_func() with the drawn size of the tree. */
static void search_position(GtkTreeView *tree, GtkWidget *search, gpointer data)
{
    (void)data;
    GtkWidget *widget = GTK_WIDGET(tree); GdkWindow *window = gtk_widget_get_window(widget);
    GdkRectangle area = {0, 0, gdk_window_get_width(window), gdk_window_get_height(window)}, rect = area;
    GdkWindow *toplevel = physical_area(window, &area, &rect);
    gint ox = 0, oy = 0;
    if (toplevel) gdk_window_get_origin(toplevel, &ox, &oy); else gdk_window_get_origin(window, &ox, &oy);
    gint tree_x = ox + rect.x, tree_y = oy + rect.y, x, y;
    gtk_widget_realize(search);
    GdkRectangle work; gdk_monitor_get_workarea(gdk_display_get_monitor_at_window(gtk_widget_get_display(widget), window), &work);
    GtkRequisition size; gtk_widget_get_preferred_size(search, &size, NULL);
    if (tree_x + rect.width > work.x + work.width) x = work.x + work.width - size.width;
    else if (tree_x + rect.width - size.width < work.x) x = work.x;
    else x = tree_x + rect.width - size.width;
    if (tree_y + rect.height + size.height > work.y + work.height) y = work.y + work.height - size.height;
    else if (tree_y + rect.height < work.y) y = work.y;
    else y = tree_y + rect.height;
    gtk_window_move(GTK_WINDOW(search), x, y);
}
static void scale_tree_search(GtkTreeView *tree)
{
    if (!default_search_position) {
        GtkWidget *probe = gtk_tree_view_new(); g_object_ref_sink(probe);
        default_search_position = gtk_tree_view_get_search_position_func(GTK_TREE_VIEW(probe));
        gtk_widget_destroy(probe); g_object_unref(probe);
    }
    if (gtk_tree_view_get_search_position_func(tree) == default_search_position &&
        scaled_window(gtk_widget_get_window(GTK_WIDGET(tree))))
        gtk_tree_view_set_search_position_func(tree, search_position, NULL, NULL);
}
#ifdef GDK_WINDOWING_X11
/* X11 places popups synchronously: correct GTK's placement right away,
 * before GTK's own handler sees it, and also for a menu already shown. */
static void popup_window_moved(GdkWindow *window, const GdkRectangle *flipped, const GdkRectangle *final_rect,
    gboolean flipped_x, gboolean flipped_y, gpointer data)
{
    (void)final_rect; (void)flipped_x; (void)flipped_y; (void)data;
    if (popup_correcting) return;
    gpointer owner = NULL; gdk_window_get_user_data(window, &owner);
    if (!GTK_IS_WINDOW(owner)) return;
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(owner));
    gboolean placed = GTK_IS_MENU(child) ? reanchor_menu(GTK_MENU(child), flipped) :
        tooltip_window(owner) && reanchor_tooltip(owner);
    if (placed) g_signal_stop_emission_by_name(window, "moved-to-rect");
}
#endif
static gboolean popup_watched(GtkWidget *window)
{
    GdkWindow *native = gtk_widget_get_window(window);
    return native && g_object_get_data(G_OBJECT(native), "radios-content-scale-watch");
}
static void watch_popup(GtkWidget *window)
{
#ifdef GDK_WINDOWING_X11
    GdkWindow *native = gtk_widget_get_window(window);
    if (!native || popup_watched(window) ||
        !GDK_IS_X11_DISPLAY(gdk_window_get_display(native)) || !g_strcmp0(g_getenv(POPUP_MAP_ONLY), "1")) return;
    if (!GTK_IS_MENU(gtk_bin_get_child(GTK_BIN(window))) && !tooltip_window(window)) return;
    g_object_set_data(G_OBJECT(native), "radios-content-scale-watch", GINT_TO_POINTER(1));
    g_signal_connect(native, "moved-to-rect", G_CALLBACK(popup_window_moved), NULL);
#else
    (void)window;
#endif
}
static gboolean popup_realized(GSignalInvocationHint *hint, guint n, const GValue *values, gpointer data)
{
    (void)hint; (void)n; (void)data;
    GObject *widget = g_value_get_object(&values[0]);
    if (GTK_IS_WINDOW(widget) && gtk_window_get_window_type(GTK_WINDOW(widget)) == GTK_WINDOW_POPUP) watch_popup(GTK_WIDGET(widget));
    else if (GTK_IS_TREE_VIEW(widget)) scale_tree_search(GTK_TREE_VIEW(widget));
    return TRUE;
}
/* Every popup maps its content before its own window is shown. */
static gboolean popup_mapped(GSignalInvocationHint *hint, guint n, const GValue *values, gpointer data)
{
    (void)hint; (void)n; (void)data;
    GtkWidget *widget = GTK_WIDGET(g_value_get_object(&values[0])), *parent = gtk_widget_get_parent(widget);
    if (GTK_IS_MENU(widget) && GTK_IS_WINDOW(parent)) {
        if (!popup_watched(parent)) { watch_popup(parent); reanchor_menu(GTK_MENU(widget), NULL); }
    } else if (GTK_IS_POPOVER(widget)) {
        reanchor_popover(GTK_POPOVER(widget));
    } else if (tooltip_window(parent) && !popup_watched(parent)) {
        watch_popup(parent); reanchor_tooltip(parent);
    }
    return TRUE;
}
static gboolean tooltip_queried(GSignalInvocationHint *hint, guint n, const GValue *values, gpointer data)
{
    (void)hint; (void)data;
    g_set_weak_pointer(&tooltip_widget, GTK_WIDGET(g_value_get_object(&values[0])));
    tooltip_keyboard = n > 3 && g_value_get_boolean(&values[3]);
    return TRUE;
}
static void popups_install(void)
{
    (void)menu_layout_known(); /* checked once here, not in the middle of a popup */
    g_signal_add_emission_hook(g_signal_lookup("realize", GTK_TYPE_WIDGET), 0, popup_realized, NULL, NULL);
    g_signal_add_emission_hook(g_signal_lookup("map", GTK_TYPE_WIDGET), 0, popup_mapped, NULL, NULL);
    g_signal_add_emission_hook(g_signal_lookup("query-tooltip", GTK_TYPE_WIDGET), 0, tooltip_queried, NULL, NULL);
}

static void radios_content_scale_class_init(RadioSContentScaleClass *klass)
{
    GtkWidgetClass *widget = GTK_WIDGET_CLASS(klass);
    widget->realize = scale_realize; widget->unrealize = scale_unrealize;
    widget->get_request_mode = scale_request_mode;
    widget->get_preferred_width = preferred_width; widget->get_preferred_height = preferred_height;
    widget->get_preferred_width_for_height = preferred_width_for_height;
    widget->get_preferred_height_for_width = preferred_height_for_width;
    widget->size_allocate = scale_allocate; widget->draw = scale_draw; widget->damage_event = scale_damage;
    GTK_CONTAINER_CLASS(klass)->add = scale_add;
    gtk_widget_class_set_css_name(widget, "radioscontentscale");
    popups_install();
}
static void radios_content_scale_init(RadioSContentScale *self)
{ self->percent = self->vertical_percent = 100; gtk_widget_set_has_window(GTK_WIDGET(self), TRUE); }
GtkWidget *radios_content_scale_new(void) { return g_object_new(RADIOS_TYPE_CONTENT_SCALE, NULL); }
void radios_content_scale_set_percents(GtkWidget *widget, guint horizontal, guint vertical)
{
    g_return_if_fail(RADIOS_IS_CONTENT_SCALE(widget));
    RadioSContentScale *self = RADIOS_CONTENT_SCALE(widget); horizontal = CLAMP(horizontal, 50, 150); vertical = CLAMP(vertical, 50, 150);
    if (self->percent == horizontal && self->vertical_percent == vertical) return;
    self->percent = horizontal; self->vertical_percent = vertical; gtk_widget_queue_resize(widget); gtk_widget_queue_draw(widget);
    if (self->offscreen) { gdk_window_geometry_changed(self->offscreen); gdk_window_invalidate_rect(self->offscreen, NULL, TRUE); }
}
void radios_content_scale_set_percent(GtkWidget *widget, guint percent)
{ radios_content_scale_set_percents(widget, percent, percent); }
guint radios_content_scale_get_percent(GtkWidget *widget)
{ g_return_val_if_fail(RADIOS_IS_CONTENT_SCALE(widget), 100); return RADIOS_CONTENT_SCALE(widget)->percent; }
guint radios_content_scale_get_vertical_percent(GtkWidget *widget)
{ g_return_val_if_fail(RADIOS_IS_CONTENT_SCALE(widget), 100); return RADIOS_CONTENT_SCALE(widget)->vertical_percent; }

void radios_content_scale_set_propagate_height(GtkWidget *widget, gboolean propagate)
{
    g_return_if_fail(RADIOS_IS_CONTENT_SCALE(widget));
    RadioSContentScale *self = RADIOS_CONTENT_SCALE(widget);
    propagate = !!propagate;
    if (self->propagate_height == propagate) return;
    self->propagate_height = propagate; gtk_widget_queue_resize(widget);
}
void radios_content_scale_set_section(GtkWidget *widget, gboolean section)
{
    g_return_if_fail(RADIOS_IS_CONTENT_SCALE(widget));
    RadioSContentScale *self = RADIOS_CONTENT_SCALE(widget);
    section = !!section;
    if (self->section == section) return;
    self->section = section; gtk_widget_queue_resize(widget);
}
