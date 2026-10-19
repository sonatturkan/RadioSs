#ifndef RADIOS_BROADCAST_OUTPUT_UI_H
#define RADIOS_BROADCAST_OUTPUT_UI_H
#include "radios/views.h"
GtkWidget *radios_broadcast_output_panel_new(RadioSViewContext *ctx);
/* "Son çalıştırmalar": the station's latest broadcast/record/emergency run
 * receipts (desktop-broadcast/overview, read-only). Requests are made only
 * while the list is on screen: when it is shown, about every ten seconds
 * while it stays visible, shortly after a local output starts and after a
 * data refresh. Without the data service the list is empty. */
GtkWidget *radios_broadcast_runs_view_new(RadioSViewContext *ctx);
#endif
