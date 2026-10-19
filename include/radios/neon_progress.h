#ifndef RADIOS_NEON_PROGRESS_H
#define RADIOS_NEON_PROGRESS_H

#include <gtk/gtk.h>

G_BEGIN_DECLS

/* Called once on a completed gesture. FALSE keeps the authoritative position.
 * The widget emits "seek-begin" before a drag or keyboard gesture starts. */
typedef gboolean (*RadiosNeonProgressSeekCallback)(guint64 position_ms,
                                                  gpointer user_data);

GtkWidget *radios_neon_progress_new(void);
void radios_neon_progress_set_state(GtkWidget *widget, guint64 position_ms,
                                    guint64 duration_ms, gboolean playing,
                                    gboolean seekable);
/* Returns the authoritative position, never an uncommitted drag preview. */
void radios_neon_progress_get_state(GtkWidget *widget, guint64 *position_ms,
                                    guint64 *duration_ms, gboolean *playing,
                                    gboolean *seekable);
void radios_neon_progress_set_seek_callback(GtkWidget *widget,
                                           RadiosNeonProgressSeekCallback callback,
                                           gpointer user_data);

G_END_DECLS

#endif
