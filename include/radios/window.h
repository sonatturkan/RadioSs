#ifndef RADIOS_WINDOW_H
#define RADIOS_WINDOW_H

#include <gtk/gtk.h>
#include "radios/config.h"
#include "radios/runtime.h"
#include "radios/views.h"

typedef struct RadioSWindow RadioSWindow;

RadioSWindow *radios_window_new(GtkApplication *application,
                               const RadioSConfig *config,
                               RadioSRuntime *runtime, gboolean smoke_test,
                               const gchar *capture_path, GError **error);
void radios_window_present(RadioSWindow *window);
gboolean radios_window_select_profile(RadioSWindow *window, const gchar *profile);
/* Borrowed GTK-thread context; valid until window_free. */
RadioSViewContext *radios_window_view_context(RadioSWindow *window);
gboolean radios_window_smoke_passed(const RadioSWindow *window);
void radios_window_free(RadioSWindow *window);

#endif
