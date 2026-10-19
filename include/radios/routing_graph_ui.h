#ifndef RADIOS_ROUTING_GRAPH_UI_H
#define RADIOS_ROUTING_GRAPH_UI_H
#include <gtk/gtk.h>
#include "radios/routing_graph.h"
G_BEGIN_DECLS
/* Patchbay editor: block settings on the left (shown while a block is
 * selected, closed with ×), the cable canvas in the middle and the block
 * palette on the right. Blocks are renamed with the pencil on their title. */
typedef struct RadioSRoutingGraphUI RadioSRoutingGraphUI;
typedef void (*RadioSRoutingGraphChanged)(gpointer data);
typedef void (*RadioSRoutingGraphSelected)(guint id, gpointer data);
typedef enum { RG_STATUS_IDLE, RG_STATUS_STARTING, RG_STATUS_ACTIVE, RG_STATUS_FAILED } RadioSRoutingNodeState;
typedef struct {
    guint id;
    RadioSRoutingNodeState state;
    gfloat level[2]; /* 0..1 meter position */
    gchar detail[160];
} RadioSRoutingNodeStatus;
RadioSRoutingGraphUI *radios_routing_graph_ui_new(RadioSRoutingGraph *graph, const gchar *assets_root,
    RadioSRoutingGraphChanged changed, RadioSRoutingGraphSelected selected, gpointer data);
GtkWidget *radios_routing_graph_ui_widget(RadioSRoutingGraphUI *ui);
RadioSRoutingGraphUI *radios_routing_graph_ui_from_widget(GtkWidget *widget);
const RadioSRoutingGraph *radios_routing_graph_ui_graph(RadioSRoutingGraphUI *ui);
/* Body of the settings column; the page fills it for the selected block. */
GtkWidget *radios_routing_graph_ui_settings(RadioSRoutingGraphUI *ui);
void radios_routing_graph_ui_style(RadioSRoutingGraphUI *ui, GtkWidget *widget);
void radios_routing_graph_ui_refresh(RadioSRoutingGraphUI *ui);
void radios_routing_graph_ui_busy(RadioSRoutingGraphUI *ui, gboolean busy);
void radios_routing_graph_ui_status(RadioSRoutingGraphUI *ui, const RadioSRoutingNodeStatus *status, guint count);
void radios_routing_graph_ui_message(RadioSRoutingGraphUI *ui, const gchar *text);
void radios_routing_graph_ui_select(RadioSRoutingGraphUI *ui, guint id);
guint radios_routing_graph_ui_selected(RadioSRoutingGraphUI *ui);
/* Adds a block of `kind` at a free spot and selects it; 0 on failure. */
guint radios_routing_graph_ui_add(RadioSRoutingGraphUI *ui, RadioSRoutingGraphKind kind);
/* Cable as drawn with the pointer; FALSE with the reason shown as hint. */
gboolean radios_routing_graph_ui_connect(RadioSRoutingGraphUI *ui, guint from, guint to);
/* Opens the rename box of a block (as the pencil does). */
void radios_routing_graph_ui_rename(RadioSRoutingGraphUI *ui, guint id);
gboolean radios_routing_graph_ui_node_geometry(RadioSRoutingGraphUI *ui, guint id,
    GdkRectangle *rect, gdouble *input_x, gdouble *input_y, gdouble *output_x, gdouble *output_y);
gboolean radios_routing_graph_ui_pencil_geometry(RadioSRoutingGraphUI *ui, guint id, GdkRectangle *rect);
G_END_DECLS
#endif
