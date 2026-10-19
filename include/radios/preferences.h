#ifndef RADIOS_PREFERENCES_H
#define RADIOS_PREFERENCES_H

#include "radios/views.h"

G_BEGIN_DECLS

/* A single native Preferences window belongs to each application window.
 * Repeated calls present that window. All calls run on the GTK thread. */
GtkWidget *radios_preferences_open(RadioSViewContext *ctx);
GtkWidget *radios_preferences_button_new(RadioSViewContext *ctx);
/* Apply stored output preferences once, after the controller is created and
 * before any playback starts. Opening Preferences itself only inspects audio. */
void radios_preferences_apply_startup(RadioSViewContext *ctx);

/* Keyboard and Advanced open the application's existing working views.
 * callback/data are borrowed until parent is destroyed or this setter
 * replaces them. Appearance controls are built separately below. */
typedef void (*RadioSPreferencesNavigateFunc)(RadioSViewContext *ctx,
                                            const gchar *category,
                                            gpointer data);
void radios_preferences_set_navigation(GtkWindow *parent,
                                      RadioSPreferencesNavigateFunc callback,
                                      gpointer data);

/* Build the application's live appearance controls inside Preferences.
 * The returned widget belongs to the Preferences page. callback/data are
 * borrowed for the lifetime of parent, as with the navigation callback. */
typedef GtkWidget *(*RadioSPreferencesAppearanceFunc)(RadioSViewContext *ctx,
                                                     gpointer data);
void radios_preferences_set_appearance(GtkWindow *parent,
                                      RadioSPreferencesAppearanceFunc callback,
                                      gpointer data);

G_END_DECLS
#endif
