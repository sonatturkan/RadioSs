#include "radios/routing_graph_ui.h"
#include "radios/widgets.h"
#include <math.h>
#include <string.h>

#define WORLD_WIDTH 960.0
#define WORLD_HEIGHT 480.0
#define NODE_WIDTH 184.0
#define NODE_HEIGHT 104.0
#define HEADER_HEIGHT 34.0
#define PORT_Y 62.0

struct RadioSRoutingGraphUI {
    RadioSRoutingGraph *graph;
    GtkWidget *root, *canvas, *settings, *settings_title, *settings_body, *close, *hint, *disconnect, *rename, *rename_entry;
    GtkWidget *palette[RG_KIND_COUNT];
    GtkCssProvider *style;
    RadioSRoutingGraphChanged changed;
    RadioSRoutingGraphSelected selected_changed;
    gpointer data;
    guint selected, pending, dragging, edge_from, edge_to, renaming, menu_node;
    gdouble offset_x, offset_y, pointer_x, pointer_y, drag_scale, origin_x, origin_y;
    gboolean busy, moved;
    RadioSRoutingNodeStatus status[RADIOS_ROUTING_GRAPH_MAX_NODES];
    guint status_count;
};
static const gchar *const captions[] = {
    "Bağlanan sesi seçtiğiniz mikser kanalına gönderir; çıkışı o kanalın sesidir",
    "Hat girişi, ses kartı girişi ya da bir çıkışın monitörü",
    "Konuşma (bas-konuş) mikrofonu ya da başka bir mikrofon",
    "Diğer uygulamaların görebildiği RadioS sanal ses aygıtı",
    "Ana yayın karışımı; girişine bağlanan ses doğrudan MAIN'e eklenir",
    "Hoparlör, kulaklık ya da ses kartı çıkışı",
    "Bağlanan sesi WAV dosyasına kaydeder",
    "Icecast / SHOUTcast sunucusuna MP3 yayın",
    "Bağlandığı noktadaki ses seviyesini gösterir; sesi değiştirmeden geçirir"};
static void pack(GtkWidget *box, GtkWidget *child, gboolean expand)
{ gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0); }
static void name(GtkWidget *widget, const gchar *id)
{ gtk_widget_set_name(widget, id); }
static gdouble scale(RadioSRoutingGraphUI *ui)
{
    if ((ui->dragging || ui->pending) && ui->drag_scale > 0) return ui->drag_scale;
    gdouble width = gtk_widget_get_allocated_width(ui->canvas), height = gtk_widget_get_allocated_height(ui->canvas);
    gdouble world_width = WORLD_WIDTH, world_height = WORLD_HEIGHT;
    for (guint i = 0; i < ui->graph->node_count; ++i) {
        world_width = MAX(world_width, ui->graph->nodes[i].x + NODE_WIDTH + 20);
        world_height = MAX(world_height, ui->graph->nodes[i].y + NODE_HEIGHT + 20);
    }
    return MAX(.1, MIN(1.25, MIN(width / world_width, height / world_height)));
}
static void world(RadioSRoutingGraphUI *ui, gdouble x, gdouble y, gdouble *wx, gdouble *wy)
{ gdouble factor = scale(ui); *wx = x / factor; *wy = y / factor; }
void radios_routing_graph_ui_message(RadioSRoutingGraphUI *ui, const gchar *text)
{ gtk_label_set_text(GTK_LABEL(ui->hint), text); }
static void rounded(cairo_t *cr, gdouble x, gdouble y, gdouble width, gdouble height, gdouble radius)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + width - radius, y + radius, radius, -G_PI / 2, 0);
    cairo_arc(cr, x + width - radius, y + height - radius, radius, 0, G_PI / 2);
    cairo_arc(cr, x + radius, y + height - radius, radius, G_PI / 2, G_PI);
    cairo_arc(cr, x + radius, y + radius, radius, G_PI, G_PI * 1.5);
    cairo_close_path(cr);
}
static void text(cairo_t *cr, const gchar *value, gdouble x, gdouble y, gint width, gint size, gboolean bold)
{
    PangoLayout *layout = pango_cairo_create_layout(cr);
    PangoFontDescription *font = pango_font_description_new();
    pango_font_description_set_family(font, "Inter, Sans");
    pango_font_description_set_absolute_size(font, size * PANGO_SCALE);
    pango_font_description_set_weight(font, bold ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL);
    pango_layout_set_font_description(layout, font);
    pango_layout_set_text(layout, value, -1);
    pango_layout_set_width(layout, width * PANGO_SCALE);
    pango_layout_set_ellipsize(layout, PANGO_ELLIPSIZE_END);
    cairo_move_to(cr, x, y); pango_cairo_show_layout(cr, layout);
    pango_font_description_free(font); g_object_unref(layout);
}
static void cable(cairo_t *cr, gdouble x1, gdouble y1, gdouble x2, gdouble y2)
{
    gdouble curve = MAX(60, fabs(x2 - x1) * .48);
    cairo_move_to(cr, x1, y1); cairo_curve_to(cr, x1 + curve, y1, x2 - curve, y2, x2, y2);
}
static void port(cairo_t *cr, gdouble x, gdouble y, gboolean selected)
{
    cairo_set_source_rgb(cr, .10, .16, .18); cairo_arc(cr, x, y, 7, 0, 2 * G_PI); cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, selected ? 1 : .12, selected ? .78 : .76, selected ? .28 : .86);
    cairo_set_line_width(cr, 2); cairo_stroke(cr);
}
static const RadioSRoutingNodeStatus *status_of(RadioSRoutingGraphUI *ui, guint id)
{
    for (guint i = 0; i < ui->status_count; ++i) if (ui->status[i].id == id) return &ui->status[i];
    return NULL;
}
static void level_bar(cairo_t *cr, gdouble x, gdouble y, gdouble width, gdouble height, gdouble level)
{
    level = isfinite(level) ? CLAMP(level, 0, 1) : 0;
    cairo_set_source_rgb(cr, .07, .13, .15); cairo_rectangle(cr, x, y, width, height); cairo_fill(cr);
    if (level <= 0) return;
    if (level < .75) cairo_set_source_rgb(cr, .08, .79, .55);
    else if (level < .9) cairo_set_source_rgb(cr, .95, .78, .2);
    else cairo_set_source_rgb(cr, .96, .33, .28);
    cairo_rectangle(cr, x, y, width * level, height); cairo_fill(cr);
}
static void draw_node(RadioSRoutingGraphUI *ui, cairo_t *cr, const RadioSRoutingGraphNode *node)
{
    const RadioSRoutingNodeStatus *status = status_of(ui, node->id);
    gboolean selected = node->id == ui->selected;
    gdouble x = node->x, y = node->y;
    rounded(cr, x, y, NODE_WIDTH, NODE_HEIGHT, 9);
    cairo_set_source_rgb(cr, .17, .21, .23); cairo_fill_preserve(cr);
    cairo_set_line_width(cr, selected ? 2.5 : 1);
    cairo_set_source_rgb(cr, .26, selected ? .84 : .36, selected ? .95 : .40); cairo_stroke(cr);
    cairo_save(cr); rounded(cr, x + 1, y + 1, NODE_WIDTH - 2, NODE_HEIGHT - 2, 8); cairo_clip(cr);
    cairo_set_source_rgb(cr, .025, .48, .59); cairo_rectangle(cr, x, y, NODE_WIDTH, HEADER_HEIGHT); cairo_fill(cr);
    cairo_restore(cr);
    cairo_set_source_rgb(cr, .95, .99, 1);
    text(cr, radios_routing_graph_node_title(node), x + 12, y + 9, (gint)NODE_WIDTH - 62, 13, TRUE);
    /* State lamp and the rename pencil share the title bar. */
    RadioSRoutingNodeState state = status ? status->state : RG_STATUS_IDLE;
    static const gdouble lamps[][3] = {{.55, .62, .65}, {1, .78, .28}, {.2, .85, .5}, {.95, .33, .3}};
    cairo_set_source_rgb(cr, lamps[state][0], lamps[state][1], lamps[state][2]);
    cairo_arc(cr, x + NODE_WIDTH - 40, y + HEADER_HEIGHT / 2, 4.5, 0, 2 * G_PI); cairo_fill(cr);
    cairo_set_source_rgb(cr, .92, .98, 1); text(cr, "✎", x + NODE_WIDTH - 27, y + 7, 20, 15, FALSE);
    cairo_set_source_rgb(cr, .69, .79, .83);
    text(cr, radios_routing_graph_kind_title(node->kind), x + 13, y + HEADER_HEIGHT + 6, (gint)NODE_WIDTH - 26, 11, TRUE);
    cairo_set_source_rgb(cr, .55, .67, .72);
    text(cr, status && status->detail[0] ? status->detail : captions[node->kind], x + 13, y + HEADER_HEIGHT + 23, (gint)NODE_WIDTH - 26, 10, FALSE);
    gdouble left = status ? status->level[0] : 0, right = status ? status->level[1] : 0;
    if (node->kind == RG_METER) {
        level_bar(cr, x + 13, y + NODE_HEIGHT - 24, NODE_WIDTH - 26, 7, left);
        level_bar(cr, x + 13, y + NODE_HEIGHT - 14, NODE_WIDTH - 26, 7, right);
    } else {
        level_bar(cr, x + 13, y + NODE_HEIGHT - 17, NODE_WIDTH - 26, 3, left);
        level_bar(cr, x + 13, y + NODE_HEIGHT - 11, NODE_WIDTH - 26, 3, right);
    }
    if (radios_routing_graph_has_input(node->kind)) port(cr, x, y + PORT_Y, FALSE);
    if (radios_routing_graph_has_output(node->kind)) port(cr, x + NODE_WIDTH, y + PORT_Y, node->id == ui->pending);
}
static gboolean draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    RadioSRoutingGraphUI *ui = data; (void)widget;
    cairo_set_source_rgb(cr, .10, .13, .15); cairo_paint(cr);
    cairo_save(cr); gdouble factor = scale(ui); cairo_scale(cr, factor, factor);
    gdouble width = gtk_widget_get_allocated_width(ui->canvas) / factor, height = gtk_widget_get_allocated_height(ui->canvas) / factor;
    cairo_set_source_rgba(cr, .50, .63, .68, .15);
    for (gint y = 20; y < (gint)height; y += 20)
        for (gint x = 20; x < (gint)width; x += 20) { cairo_arc(cr, x, y, 1, 0, 2 * G_PI); cairo_fill(cr); }
    for (guint i = 0; i < ui->graph->edge_count; ++i) {
        const RadioSRoutingGraphEdge *edge = &ui->graph->edges[i];
        const RadioSRoutingGraphNode *from = radios_routing_graph_find(ui->graph, edge->from);
        const RadioSRoutingGraphNode *to = radios_routing_graph_find(ui->graph, edge->to);
        if (!from || !to) continue;
        gboolean selected = edge->from == ui->edge_from && edge->to == ui->edge_to;
        cairo_set_line_width(cr, selected ? 4 : 2.5);
        cairo_set_source_rgb(cr, selected ? 1 : .04, selected ? .78 : .67, selected ? .28 : .79);
        cable(cr, from->x + NODE_WIDTH, from->y + PORT_Y, to->x, to->y + PORT_Y); cairo_stroke(cr);
    }
    if (ui->pending) {
        const RadioSRoutingGraphNode *from = radios_routing_graph_find(ui->graph, ui->pending);
        if (from) {
            const gdouble dash[] = {5, 5}; cairo_set_dash(cr, dash, 2, 0);
            cairo_set_source_rgb(cr, 1, .78, .28); cairo_set_line_width(cr, 2);
            cable(cr, from->x + NODE_WIDTH, from->y + PORT_Y, ui->pointer_x, ui->pointer_y);
            cairo_stroke(cr); cairo_set_dash(cr, NULL, 0, 0);
        }
    }
    for (guint i = 0; i < ui->graph->node_count; ++i) draw_node(ui, cr, &ui->graph->nodes[i]);
    cairo_restore(cr); return FALSE;
}
static void settings_visible(RadioSRoutingGraphUI *ui, const RadioSRoutingGraphNode *node)
{
    if (node) {
        gtk_label_set_text(GTK_LABEL(ui->settings_title), radios_routing_graph_node_title(node));
        gtk_widget_set_no_show_all(ui->settings, FALSE); gtk_widget_show_all(ui->settings);
    } else gtk_widget_hide(ui->settings);
}
void radios_routing_graph_ui_select(RadioSRoutingGraphUI *ui, guint id)
{
    const RadioSRoutingGraphNode *node = radios_routing_graph_find(ui->graph, id);
    gboolean changed = (node ? id : 0) != ui->selected;
    ui->selected = node ? id : 0; ui->edge_from = ui->edge_to = 0;
    gtk_widget_set_sensitive(ui->disconnect, FALSE);
    if (changed && ui->selected_changed) ui->selected_changed(ui->selected, ui->data);
    settings_visible(ui, node);
    gtk_widget_queue_draw(ui->canvas);
}
void radios_routing_graph_ui_refresh(RadioSRoutingGraphUI *ui)
{
    if (ui->selected && !radios_routing_graph_find(ui->graph, ui->selected)) radios_routing_graph_ui_select(ui, 0);
    const RadioSRoutingGraphNode *node = radios_routing_graph_find(ui->graph, ui->selected);
    if (node) gtk_label_set_text(GTK_LABEL(ui->settings_title), radios_routing_graph_node_title(node));
    gtk_widget_queue_draw(ui->canvas);
}
static void changed(RadioSRoutingGraphUI *ui)
{
    radios_routing_graph_ui_refresh(ui);
    if (ui->changed) ui->changed(ui->data);
}
static gboolean editable(RadioSRoutingGraphUI *ui)
{
    if (!ui->busy) return TRUE;
    radios_routing_graph_ui_message(ui, "Yetki denetimi sürüyor; birazdan yeniden deneyin."); return FALSE;
}
static guint hit_node(RadioSRoutingGraphUI *ui, gdouble x, gdouble y)
{
    for (gint i = (gint)ui->graph->node_count - 1; i >= 0; --i) {
        const RadioSRoutingGraphNode *n = &ui->graph->nodes[i];
        if (x >= n->x && x <= n->x + NODE_WIDTH && y >= n->y && y <= n->y + NODE_HEIGHT) return n->id;
    }
    return 0;
}
static gboolean hit_pencil(const RadioSRoutingGraphNode *n, gdouble x, gdouble y)
{ return x >= n->x + NODE_WIDTH - 32 && x <= n->x + NODE_WIDTH - 4 && y >= n->y + 2 && y <= n->y + HEADER_HEIGHT; }
static guint hit_port(RadioSRoutingGraphUI *ui, gdouble x, gdouble y, gboolean output)
{
    for (guint i = 0; i < ui->graph->node_count; ++i) {
        const RadioSRoutingGraphNode *n = &ui->graph->nodes[i];
        if (!(output ? radios_routing_graph_has_output(n->kind) : radios_routing_graph_has_input(n->kind))) continue;
        gdouble px = n->x + (output ? NODE_WIDTH : 0);
        if (hypot(x - px, y - n->y - PORT_Y) <= 13) return n->id;
    }
    return 0;
}
static gboolean hit_edge(RadioSRoutingGraphUI *ui, gdouble x, gdouble y)
{
    for (guint i = 0; i < ui->graph->edge_count; ++i) {
        const RadioSRoutingGraphEdge *e = &ui->graph->edges[i];
        const RadioSRoutingGraphNode *a = radios_routing_graph_find(ui->graph, e->from);
        const RadioSRoutingGraphNode *b = radios_routing_graph_find(ui->graph, e->to);
        if (!a || !b) continue;
        gdouble x1 = a->x + NODE_WIDTH, y1 = a->y + PORT_Y, x2 = b->x, y2 = b->y + PORT_Y;
        gdouble c = MAX(60, fabs(x2 - x1) * .48);
        for (guint step = 0; step <= 80; ++step) {
            gdouble t = step / 80., u = 1 - t;
            gdouble px = u*u*u*x1 + 3*u*u*t*(x1+c) + 3*u*t*t*(x2-c) + t*t*t*x2;
            gdouble py = u*u*u*y1 + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*y2;
            if (hypot(x - px, y - py) < 8) { ui->edge_from = e->from; ui->edge_to = e->to; return TRUE; }
        }
    }
    return FALSE;
}
static gboolean connect_port(RadioSRoutingGraphUI *ui, guint target)
{
    GError *error = NULL;
    if (!ui->pending || !target || !editable(ui)) return FALSE;
    gboolean result = radios_routing_graph_connect(ui->graph, ui->pending, target, &error);
    radios_routing_graph_ui_message(ui, result ? "Bağlantı eklendi." : error ? error->message : "Bu bloklar bağlanamıyor.");
    ui->pending = 0; g_clear_error(&error);
    if (result) changed(ui); else gtk_widget_queue_draw(ui->canvas);
    return result;
}
gboolean radios_routing_graph_ui_connect(RadioSRoutingGraphUI *ui, guint from, guint to)
{ ui->pending = from; return connect_port(ui, to); }
void radios_routing_graph_ui_rename(RadioSRoutingGraphUI *ui, guint id)
{
    const RadioSRoutingGraphNode *node = radios_routing_graph_find(ui->graph, id);
    if (!node || !editable(ui)) return;
    ui->renaming = id;
    GdkRectangle rect; radios_routing_graph_ui_pencil_geometry(ui, id, &rect);
    gtk_popover_set_pointing_to(GTK_POPOVER(ui->rename), &rect);
    gtk_entry_set_text(GTK_ENTRY(ui->rename_entry), radios_routing_graph_node_title(node));
    gtk_widget_show_all(ui->rename); gtk_popover_popup(GTK_POPOVER(ui->rename));
    gtk_widget_grab_focus(ui->rename_entry); gtk_editable_select_region(GTK_EDITABLE(ui->rename_entry), 0, -1);
}
static void rename_apply(GtkWidget *widget, gpointer data)
{
    RadioSRoutingGraphUI *ui = data; (void)widget; GError *error = NULL;
    const RadioSRoutingGraphNode *node = radios_routing_graph_find(ui->graph, ui->renaming);
    if (!node) { gtk_popover_popdown(GTK_POPOVER(ui->rename)); return; }
    const gchar *value = gtk_entry_get_text(GTK_ENTRY(ui->rename_entry));
    /* The kind title is the default; keeping it stores no custom name. */
    const gchar *stored = g_strcmp0(value, radios_routing_graph_kind_title(node->kind)) ? value : "";
    if (radios_routing_graph_rename(ui->graph, ui->renaming, stored, &error)) {
        gtk_popover_popdown(GTK_POPOVER(ui->rename)); changed(ui);
        if (ui->selected == ui->renaming && ui->selected_changed) ui->selected_changed(ui->selected, ui->data);
    } else radios_routing_graph_ui_message(ui, error ? error->message : "Ad değiştirilemedi.");
    g_clear_error(&error);
}
static gboolean free_spot(RadioSRoutingGraphUI *ui, gdouble *x, gdouble *y)
{
    /* Four columns stay inside the default view; rows continue downwards. */
    for (guint row = 0; row < 15; ++row) for (guint column = 0; column < 4; ++column) {
        gdouble cx = 40 + column * 220., cy = 30 + row * 125.; gboolean taken = FALSE;
        for (guint i = 0; i < ui->graph->node_count && !taken; ++i)
            taken = fabs(ui->graph->nodes[i].x - cx) < NODE_WIDTH + 10 && fabs(ui->graph->nodes[i].y - cy) < NODE_HEIGHT + 10;
        if (!taken) { *x = cx; *y = cy; return TRUE; }
    }
    *x = 60 + g_random_int_range(0, 400); *y = 50 + g_random_int_range(0, 300); return FALSE;
}
static guint place(RadioSRoutingGraphUI *ui, RadioSRoutingGraphKind kind, gdouble x, gdouble y)
{
    if (!editable(ui)) return 0;
    GError *error = NULL; guint id = 0;
    x = CLAMP(x, 0, RADIOS_ROUTING_GRAPH_MAX_X); y = CLAMP(y, 0, RADIOS_ROUTING_GRAPH_MAX_Y);
    if (radios_routing_graph_add(ui->graph, kind, x, y, &id, &error)) {
        changed(ui); radios_routing_graph_ui_select(ui, id);
        radios_routing_graph_ui_message(ui, "Blok eklendi. Çıkış noktasını hedef bloğun girişine sürükleyin.");
    } else radios_routing_graph_ui_message(ui, error ? error->message : "Blok eklenemedi.");
    g_clear_error(&error); return id;
}
guint radios_routing_graph_ui_add(RadioSRoutingGraphUI *ui, RadioSRoutingGraphKind kind)
{
    gdouble x, y; free_spot(ui, &x, &y); return place(ui, kind, x, y);
}
static void duplicate(RadioSRoutingGraphUI *ui, guint id)
{
    const RadioSRoutingGraphNode *source = radios_routing_graph_find(ui->graph, id); guint copy = 0;
    if (!source || !editable(ui)) return;
    RadioSRoutingGraphNode settings = *source; GError *error = NULL;
    gdouble x = MIN(source->x + 30, RADIOS_ROUTING_GRAPH_MAX_X), y = MIN(source->y + 30, RADIOS_ROUTING_GRAPH_MAX_Y);
    if (radios_routing_graph_add(ui->graph, source->kind, x, y, &copy, &error)) {
        settings.id = copy;
        if (!radios_routing_graph_update(ui->graph, &settings, &error)) {
            radios_routing_graph_remove(ui->graph, copy, NULL); copy = 0;
        }
    }
    if (copy) { changed(ui); radios_routing_graph_ui_select(ui, copy); radios_routing_graph_ui_message(ui, "Blok çoğaltıldı."); }
    else radios_routing_graph_ui_message(ui, error ? error->message : "Blok çoğaltılamadı.");
    g_clear_error(&error);
}
static void delete_node(GtkWidget *widget, gpointer data)
{
    RadioSRoutingGraphUI *ui = data; (void)widget;
    guint id = ui->menu_node ? ui->menu_node : ui->selected; ui->menu_node = 0;
    if (!id || !editable(ui)) return;
    radios_routing_graph_remove(ui->graph, id, NULL);
    ui->pending = 0; if (ui->selected == id) radios_routing_graph_ui_select(ui, 0);
    changed(ui); radios_routing_graph_ui_message(ui, "Blok ve bağlantıları kaldırıldı.");
}
static void menu_rename(GtkWidget *widget, gpointer data)
{ RadioSRoutingGraphUI *ui = data; (void)widget; radios_routing_graph_ui_rename(ui, ui->menu_node); ui->menu_node = 0; }
static void menu_duplicate(GtkWidget *widget, gpointer data)
{ RadioSRoutingGraphUI *ui = data; (void)widget; duplicate(ui, ui->menu_node); ui->menu_node = 0; }
static void node_menu(RadioSRoutingGraphUI *ui, guint id, GdkEventButton *event)
{
    GtkWidget *menu = gtk_menu_new(); ui->menu_node = id;
    const gchar *labels[] = {"Yeniden adlandır", "Çoğalt", "Bloğu kaldır"};
    GCallback callbacks[] = {G_CALLBACK(menu_rename), G_CALLBACK(menu_duplicate), G_CALLBACK(delete_node)};
    for (guint i = 0; i < G_N_ELEMENTS(labels); ++i) {
        GtkWidget *item = gtk_menu_item_new_with_label(labels[i]);
        g_signal_connect(item, "activate", callbacks[i], ui); gtk_menu_shell_append(GTK_MENU_SHELL(menu), item);
    }
    gtk_widget_show_all(menu); gtk_menu_attach_to_widget(GTK_MENU(menu), ui->canvas, NULL);
    g_signal_connect(menu, "deactivate", G_CALLBACK(gtk_widget_destroy), NULL);
    gtk_menu_popup_at_pointer(GTK_MENU(menu), (GdkEvent *)event);
}
static gboolean press(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    RadioSRoutingGraphUI *ui = data; gdouble x, y; world(ui, event->x, event->y, &x, &y);
    gtk_widget_grab_focus(widget);
    if (event->type != GDK_BUTTON_PRESS) return TRUE;
    guint input = hit_port(ui, x, y, FALSE), output = hit_port(ui, x, y, TRUE);
    if (event->button == 1 && ui->pending && input) { connect_port(ui, input); return TRUE; }
    if (event->button == 1 && output && editable(ui)) {
        ui->drag_scale = scale(ui); ui->pending = output; ui->pointer_x = x; ui->pointer_y = y;
        radios_routing_graph_ui_select(ui, output); gtk_widget_queue_draw(widget); return TRUE;
    }
    guint id = hit_node(ui, x, y);
    const RadioSRoutingGraphNode *node = radios_routing_graph_find(ui->graph, id);
    if (event->button == 1 && node && hit_pencil(node, x, y)) {
        radios_routing_graph_ui_select(ui, id); radios_routing_graph_ui_rename(ui, id); return TRUE;
    }
    if (event->button == 3 && node) { radios_routing_graph_ui_select(ui, id); node_menu(ui, id, event); return TRUE; }
    if (event->button == 1 && node) {
        radios_routing_graph_ui_select(ui, id);
        if (editable(ui)) {
            ui->origin_x = node->x; ui->origin_y = node->y; ui->drag_scale = scale(ui); ui->dragging = id;
            ui->offset_x = x - node->x; ui->offset_y = y - node->y; ui->moved = FALSE;
        }
        return TRUE;
    }
    if (hit_edge(ui, x, y)) {
        gtk_widget_set_sensitive(ui->disconnect, !ui->busy); gtk_widget_queue_draw(widget);
        radios_routing_graph_ui_message(ui, "Bağlantı seçildi. Bağlantıyı kaldır'a basın ya da Delete tuşunu kullanın.");
        if (event->button == 3 && editable(ui)) {
            radios_routing_graph_disconnect(ui->graph, ui->edge_from, ui->edge_to, NULL);
            ui->edge_from = ui->edge_to = 0; gtk_widget_set_sensitive(ui->disconnect, FALSE); changed(ui);
        }
        return TRUE;
    }
    if (event->button == 1) { ui->pending = 0; radios_routing_graph_ui_select(ui, 0); }
    return TRUE;
}
static gboolean motion(GtkWidget *widget, GdkEventMotion *event, gpointer data)
{
    RadioSRoutingGraphUI *ui = data; gdouble x, y; world(ui, event->x, event->y, &x, &y);
    ui->pointer_x = x; ui->pointer_y = y;
    if (ui->dragging && (event->state & GDK_BUTTON1_MASK) && !ui->busy) {
        radios_routing_graph_move(ui->graph, ui->dragging,
            CLAMP(x - ui->offset_x, 0, RADIOS_ROUTING_GRAPH_MAX_X), CLAMP(y - ui->offset_y, 0, RADIOS_ROUTING_GRAPH_MAX_Y), NULL);
        ui->moved = TRUE;
    }
    if (ui->pending || ui->dragging) gtk_widget_queue_draw(widget);
    return TRUE;
}
static gboolean release(GtkWidget *widget, GdkEventButton *event, gpointer data)
{
    RadioSRoutingGraphUI *ui = data; gdouble x, y; world(ui, event->x, event->y, &x, &y);
    if (event->button != 1) return FALSE;
    if (ui->pending) { guint target = hit_port(ui, x, y, FALSE); if (target) connect_port(ui, target); }
    if (ui->dragging && ui->moved) changed(ui);
    ui->dragging = 0; ui->moved = FALSE; gtk_widget_queue_draw(widget); return TRUE;
}
static void disconnect_edge(GtkButton *button, gpointer data)
{
    RadioSRoutingGraphUI *ui = data; (void)button;
    if (!ui->edge_from || !editable(ui)) return;
    radios_routing_graph_disconnect(ui->graph, ui->edge_from, ui->edge_to, NULL);
    ui->edge_from = ui->edge_to = 0; gtk_widget_set_sensitive(ui->disconnect, FALSE); changed(ui);
    radios_routing_graph_ui_message(ui, "Bağlantı kaldırıldı.");
}
static gboolean key(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    RadioSRoutingGraphUI *ui = data; (void)widget;
    if (event->keyval == GDK_KEY_Escape) {
        if (ui->dragging && ui->moved) radios_routing_graph_move(ui->graph, ui->dragging, ui->origin_x, ui->origin_y, NULL);
        ui->pending = ui->dragging = 0; ui->moved = FALSE; gtk_widget_queue_draw(ui->canvas); return TRUE;
    }
    if (event->keyval == GDK_KEY_F2 && ui->selected) { radios_routing_graph_ui_rename(ui, ui->selected); return TRUE; }
    if (event->keyval != GDK_KEY_Delete && event->keyval != GDK_KEY_BackSpace) return FALSE;
    if (ui->edge_from) disconnect_edge(NULL, ui); else delete_node(NULL, ui);
    return TRUE;
}
static void close_settings(GtkButton *button, gpointer data)
{ (void)button; radios_routing_graph_ui_select(data, 0); }
static void palette_clicked(GtkButton *button, gpointer data)
{
    RadioSRoutingGraphKind kind = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "graph-kind"));
    radios_routing_graph_ui_add(data, kind);
}
static void drag_get(GtkWidget *widget, GdkDragContext *context, GtkSelectionData *selection, guint info, guint time, gpointer data)
{
    (void)context; (void)info; (void)time; (void)data;
    guint kind = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(widget), "graph-kind"));
    const gchar *value = radios_routing_graph_kind_name(kind);
    gtk_selection_data_set(selection, gtk_selection_data_get_target(selection), 8, (const guchar *)value, (gint)strlen(value));
}
static void drag_received(GtkWidget *widget, GdkDragContext *context, gint x, gint y,
    GtkSelectionData *selection, guint info, guint time, gpointer data)
{
    RadioSRoutingGraphUI *ui = data; (void)widget; (void)info;
    gint length = gtk_selection_data_get_length(selection); gboolean result = FALSE;
    if (length > 0 && length < 40) {
        gchar *value = g_strndup((const gchar *)gtk_selection_data_get_data(selection), length);
        RadioSRoutingGraphKind kind;
        if (radios_routing_graph_kind_from_name(value, &kind)) {
            gdouble wx, wy; world(ui, x, y, &wx, &wy); result = place(ui, kind, wx - NODE_WIDTH / 2, wy - 20) != 0;
        }
        g_free(value);
    }
    gtk_drag_finish(context, result, FALSE, time);
}
static void style_tree(GtkWidget *widget, GtkCssProvider *provider)
{
    gtk_style_context_add_provider(gtk_widget_get_style_context(widget), GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 2);
    if (!GTK_IS_CONTAINER(widget)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (GList *item = children; item; item = item->next) style_tree(item->data, provider);
    g_list_free(children);
}
void radios_routing_graph_ui_style(RadioSRoutingGraphUI *ui, GtkWidget *widget)
{ if (ui->style) style_tree(widget, ui->style); }
static void apply_style(RadioSRoutingGraphUI *ui, const gchar *assets)
{
    gchar *path = g_build_filename(assets, "themes", "routing-graph.css", NULL);
    GtkCssProvider *provider = gtk_css_provider_new(); GError *error = NULL;
    if (gtk_css_provider_load_from_path(provider, path, &error)) {
        ui->style = provider; style_tree(ui->root, provider);
        g_object_set_data_full(G_OBJECT(ui->root), "graph-style", provider, g_object_unref);
    } else { g_warning("Routing graph theme: %s", error->message); g_clear_error(&error); g_object_unref(provider); }
    g_free(path);
}
static void ui_free(gpointer data)
{
    RadioSRoutingGraphUI *ui = data;
    if (ui->rename) {
        g_object_remove_weak_pointer(G_OBJECT(ui->rename), (gpointer *)&ui->rename);
        gtk_widget_destroy(ui->rename);
    }
    g_free(ui);
}
RadioSRoutingGraphUI *radios_routing_graph_ui_new(RadioSRoutingGraph *graph, const gchar *assets_root,
    RadioSRoutingGraphChanged callback, RadioSRoutingGraphSelected selected, gpointer data)
{
    RadioSRoutingGraphUI *ui = g_new0(RadioSRoutingGraphUI, 1);
    ui->graph = graph; ui->changed = callback; ui->selected_changed = selected; ui->data = data;
    ui->root = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, "routing-graph-editor"); name(ui->root, "routing-graph-editor");
    g_object_set_data_full(G_OBJECT(ui->root), "routing-graph-ui", ui, ui_free);
    GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 10, "routing-graph-row"); pack(ui->root, row, TRUE);
    /* Left: settings of the selected block, closed with ×. */
    ui->settings = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, "routing-graph-settings"); name(ui->settings, "routing-graph-settings");
    gtk_widget_set_size_request(ui->settings, 300, -1); gtk_widget_set_hexpand(ui->settings, FALSE); pack(row, ui->settings, FALSE);
    GtkWidget *header = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL);
    ui->settings_title = radios_ui_label("Blok ayarları", "routing-graph-heading"); name(ui->settings_title, "routing-graph-selection");
    gtk_label_set_ellipsize(GTK_LABEL(ui->settings_title), PANGO_ELLIPSIZE_END); gtk_widget_set_halign(ui->settings_title, GTK_ALIGN_START);
    pack(header, ui->settings_title, TRUE);
    ui->close = gtk_button_new_with_label("×"); name(ui->close, "routing-graph-settings-close");
    gtk_widget_set_tooltip_text(ui->close, "Blok ayarlarını kapat");
    gtk_style_context_add_class(gtk_widget_get_style_context(ui->close), "routing-graph-close");
    g_signal_connect(ui->close, "clicked", G_CALLBACK(close_settings), ui); pack(header, ui->close, FALSE);
    pack(ui->settings, header, FALSE);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), FALSE);
    ui->settings_body = radios_ui_box(GTK_ORIENTATION_VERTICAL, 8, NULL); name(ui->settings_body, "routing-graph-inspector");
    gtk_container_add(GTK_CONTAINER(scroll), ui->settings_body); pack(ui->settings, scroll, TRUE);
    /* Middle: canvas and its cable tools. */
    GtkWidget *middle = radios_ui_box(GTK_ORIENTATION_VERTICAL, 6, NULL); pack(row, middle, TRUE);
    ui->canvas = gtk_drawing_area_new(); name(ui->canvas, "routing-graph-canvas");
    gtk_widget_set_size_request(ui->canvas, 480, 340); gtk_widget_set_hexpand(ui->canvas, TRUE); gtk_widget_set_vexpand(ui->canvas, TRUE);
    gtk_widget_set_can_focus(ui->canvas, TRUE);
    gtk_widget_add_events(ui->canvas, GDK_BUTTON_PRESS_MASK | GDK_BUTTON_RELEASE_MASK | GDK_POINTER_MOTION_MASK | GDK_KEY_PRESS_MASK);
    atk_object_set_name(gtk_widget_get_accessible(ui->canvas), "Ses blokları ve bağlantı düzenleyicisi");
    g_signal_connect(ui->canvas, "draw", G_CALLBACK(draw), ui);
    g_signal_connect(ui->canvas, "button-press-event", G_CALLBACK(press), ui);
    g_signal_connect(ui->canvas, "motion-notify-event", G_CALLBACK(motion), ui);
    g_signal_connect(ui->canvas, "button-release-event", G_CALLBACK(release), ui);
    g_signal_connect(ui->canvas, "key-press-event", G_CALLBACK(key), ui);
    const GtkTargetEntry target[] = {{"RADIOS_ROUTING_BLOCK", GTK_TARGET_SAME_APP, 0}};
    gtk_drag_dest_set(ui->canvas, GTK_DEST_DEFAULT_ALL, target, 1, GDK_ACTION_COPY);
    g_signal_connect(ui->canvas, "drag-data-received", G_CALLBACK(drag_received), ui); pack(middle, ui->canvas, TRUE);
    GtkWidget *tools = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, NULL);
    ui->disconnect = radios_ui_button("Bağlantıyı kaldır"); name(ui->disconnect, "routing-graph-disconnect");
    g_signal_connect(ui->disconnect, "clicked", G_CALLBACK(disconnect_edge), ui); pack(tools, ui->disconnect, FALSE);
    ui->hint = radios_ui_label("", "routing-graph-caption");
    name(ui->hint, "routing-graph-hint"); gtk_label_set_xalign(GTK_LABEL(ui->hint), 0); gtk_label_set_line_wrap(GTK_LABEL(ui->hint), TRUE);
    pack(tools, ui->hint, TRUE); pack(middle, tools, FALSE);
    /* Right: block palette; every block can be added any number of times. */
    GtkWidget *palette = radios_ui_box(GTK_ORIENTATION_VERTICAL, 7, "routing-graph-palette"); name(palette, "routing-graph-palette");
    gtk_widget_set_size_request(palette, 168, -1); gtk_widget_set_hexpand(palette, FALSE); pack(row, palette, FALSE);
    pack(palette, radios_ui_label("SES BLOKLARI", "routing-graph-heading"), FALSE);
    for (guint kind = 0; kind < RG_KIND_COUNT; ++kind) {
        GtkWidget *button = radios_ui_button(radios_routing_graph_kind_title(kind)); ui->palette[kind] = button;
        gchar *id = g_strdup_printf("routing-graph-palette-%s", radios_routing_graph_kind_name(kind)); name(button, id); g_free(id);
        gtk_style_context_add_class(gtk_widget_get_style_context(button), "routing-graph-block");
        gtk_widget_set_tooltip_text(button, captions[kind]);
        g_object_set_data(G_OBJECT(button), "graph-kind", GUINT_TO_POINTER(kind));
        gtk_drag_source_set(button, GDK_BUTTON1_MASK, target, 1, GDK_ACTION_COPY);
        g_signal_connect(button, "drag-data-get", G_CALLBACK(drag_get), ui);
        g_signal_connect(button, "clicked", G_CALLBACK(palette_clicked), ui); pack(palette, button, FALSE);
    }
    /* Rename box opened by the title pencil. */
    ui->rename = gtk_popover_new(ui->canvas);
    /* Canvas teardown can finalize its popover before the editor destructor. */
    g_object_add_weak_pointer(G_OBJECT(ui->rename), (gpointer *)&ui->rename);
    name(ui->rename, "routing-graph-rename");
    GtkWidget *rename = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 6, NULL); gtk_container_set_border_width(GTK_CONTAINER(rename), 8);
    ui->rename_entry = gtk_entry_new(); name(ui->rename_entry, "routing-graph-rename-entry");
    gtk_entry_set_max_length(GTK_ENTRY(ui->rename_entry), 80); gtk_entry_set_width_chars(GTK_ENTRY(ui->rename_entry), 22);
    g_signal_connect(ui->rename_entry, "activate", G_CALLBACK(rename_apply), ui); pack(rename, ui->rename_entry, TRUE);
    GtkWidget *ok = radios_ui_button("Adı kaydet"); name(ok, "routing-graph-rename-apply");
    g_signal_connect(ok, "clicked", G_CALLBACK(rename_apply), ui); pack(rename, ok, FALSE);
    gtk_container_add(GTK_CONTAINER(ui->rename), rename);
    gtk_widget_set_sensitive(ui->disconnect, FALSE);
    apply_style(ui, assets_root);
    gtk_widget_set_no_show_all(ui->settings, TRUE); gtk_widget_hide(ui->settings);
    return ui;
}
GtkWidget *radios_routing_graph_ui_widget(RadioSRoutingGraphUI *ui) { return ui->root; }
RadioSRoutingGraphUI *radios_routing_graph_ui_from_widget(GtkWidget *widget)
{ return g_object_get_data(G_OBJECT(widget), "routing-graph-ui"); }
const RadioSRoutingGraph *radios_routing_graph_ui_graph(RadioSRoutingGraphUI *ui) { return ui->graph; }
GtkWidget *radios_routing_graph_ui_settings(RadioSRoutingGraphUI *ui) { return ui->settings_body; }
void radios_routing_graph_ui_busy(RadioSRoutingGraphUI *ui, gboolean busy)
{
    ui->busy = busy; if (busy) ui->pending = 0;
    gtk_widget_set_sensitive(ui->disconnect, ui->edge_from && !busy);
    for (guint i = 0; i < RG_KIND_COUNT; ++i) gtk_widget_set_sensitive(ui->palette[i], !busy);
}
void radios_routing_graph_ui_status(RadioSRoutingGraphUI *ui, const RadioSRoutingNodeStatus *status, guint count)
{
    ui->status_count = MIN(count, RADIOS_ROUTING_GRAPH_MAX_NODES);
    if (ui->status_count) memcpy(ui->status, status, ui->status_count * sizeof *status);
    gtk_widget_queue_draw(ui->canvas);
}
guint radios_routing_graph_ui_selected(RadioSRoutingGraphUI *ui) { return ui->selected; }
gboolean radios_routing_graph_ui_node_geometry(RadioSRoutingGraphUI *ui, guint id, GdkRectangle *rect,
    gdouble *input_x, gdouble *input_y, gdouble *output_x, gdouble *output_y)
{
    const RadioSRoutingGraphNode *node = radios_routing_graph_find(ui->graph, id);
    if (!node) return FALSE;
    gdouble factor = scale(ui);
    if (rect) *rect = (GdkRectangle){(gint)(node->x * factor), (gint)(node->y * factor), (gint)(NODE_WIDTH * factor), (gint)(NODE_HEIGHT * factor)};
    if (input_x) *input_x = node->x * factor;
    if (output_x) *output_x = (node->x + NODE_WIDTH) * factor;
    if (input_y) *input_y = (node->y + PORT_Y) * factor;
    if (output_y) *output_y = (node->y + PORT_Y) * factor;
    return TRUE;
}
gboolean radios_routing_graph_ui_pencil_geometry(RadioSRoutingGraphUI *ui, guint id, GdkRectangle *rect)
{
    const RadioSRoutingGraphNode *node = radios_routing_graph_find(ui->graph, id);
    if (!node || !rect) return FALSE;
    gdouble factor = scale(ui);
    *rect = (GdkRectangle){(gint)((node->x + NODE_WIDTH - 30) * factor), (gint)((node->y + 4) * factor),
                           (gint)(24 * factor), (gint)((HEADER_HEIGHT - 6) * factor)};
    return TRUE;
}
