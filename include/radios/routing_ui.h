#ifndef RADIOS_ROUTING_UI_H
#define RADIOS_ROUTING_UI_H
#include "radios/data.h"
#include "radios/audio_output.h"

/* Opens the routing page and asks the operator to select the real player.
 * Opening a provider never starts capture or enables a microphone. */
void radios_ui_routing_open(RadioSViewContext *ctx, const gchar *provider_name);

/* GUI-thread read-only output observations. Reads worker output snapshots;
 * never drains patchbay meters, starts streams or changes the selected graph.
 * active counts streams the server accepted (2xx / OK reply); a started
 * encoder still waiting for the server answer counts as pending. failed
 * includes started streams waiting for their next reconnect attempt;
 * reconnects sums the reconnections of the streams the operator started. */
typedef struct {
    gboolean available, running;
    guint streams, active, pending, failed, metadata_failed, reconnects;
    gchar target[4097], error[256], metadata_error[256];
} RadioSRoutingOutputSnapshot;
gboolean radios_ui_routing_output_snapshot(RadioSViewContext *ctx,
    RadioSRoutingOutputSnapshot *snapshot);
#endif
