#ifndef RADIOS_VIEWS_H
#define RADIOS_VIEWS_H
#include <gtk/gtk.h>
#include "radios/config.h"
#include "radios/runtime.h"
typedef struct RadioSData RadioSData;
typedef struct RadioSAudioController RadioSAudioController;
typedef struct RadioSPlayoutController RadioSPlayoutController;
typedef struct RadioSAdvertController RadioSAdvertController;
typedef struct RadioSBroadcastController RadioSBroadcastController;
typedef struct RadioSPlayerWorkspace RadioSPlayerWorkspace;
typedef struct {
    const RadioSConfig *config;
    RadioSRuntime *runtime;
    GtkWindow *parent;
    void (*notify)(gpointer data, const gchar *message);
    gpointer notify_data;
    RadioSData *data;
    RadioSAudioController *audio;
    RadioSPlayoutController *playout;
    RadioSAdvertController *advertisement;
    RadioSBroadcastController *broadcast;
    RadioSPlayerWorkspace *players;
} RadioSViewContext;
GtkWidget *radios_header_new(RadioSViewContext *ctx);
GtkWidget *radios_broadcast_new(RadioSViewContext *ctx);
GtkWidget *radios_broadcast_history_button_new(RadioSViewContext *ctx);
GtkWidget *radios_playlist_new(RadioSViewContext *ctx);
/* The playlist the Playlist page shows while that page is on screen (for
 * file drops); FALSE otherwise. The caller frees *id. */
gboolean radios_playlist_visible_target(RadioSViewContext *ctx, gchar **id, gint64 *revision);
/* Opens the rekordbox USB / SD / XML reader of the Playlist page. */
void radios_playlist_rekordbox_open(RadioSViewContext *ctx);
GtkWidget *radios_automation_new(RadioSViewContext *ctx);
GtkWidget *radios_dj_new(RadioSViewContext *ctx);
/* Independent visual sizes; mixer resizing does not resize the effect rack.
 * Each section has its own width percentage (horizontal lengths only) and
 * height percentage (vertical lengths only). */
void radios_dj_set_scale(GtkWidget *page, guint mixer_width, guint mixer_height,
                         guint effector_width, guint effector_height);
void radios_dj_prepare_close(GtkWidget *page);
GtkWidget *radios_emergency_new(RadioSViewContext *ctx);
GtkWidget *radios_advert_new(RadioSViewContext *ctx);
GtkWidget *radios_technical_new(RadioSViewContext *ctx);
GtkWidget *radios_routing_new(RadioSViewContext *ctx);
GtkWidget *radios_knob_new(const gchar *name, double min, double max, double step, double value);
GtkWidget *radios_meter_new(gint width, gint height, gboolean vertical);
GtkWidget *radios_analog_clock_new(void);
#endif
