#ifndef RADIOS_HEADER_STATUS_H
#define RADIOS_HEADER_STATUS_H

#include "radios/views.h"

G_BEGIN_DECLS

/* Adds the prompt/2 fields to the existing shared next/transition strip.
 * Repeated binding is harmless. No playback command is issued. */
void radios_header_status_bind(GtkWidget *header, RadioSViewContext *ctx);
void radios_header_status_refresh(GtkWidget *header);

/* An existing Preferences dialog may use this when it replaces the original
 * header popover. The returned widget is owned by its eventual container;
 * it becomes insensitive if the bound header is destroyed. */
GtkWidget *radios_header_status_settings_new(GtkWidget *header);
/* Shows the header fields `hidden` (RadioSHeaderHiddenField bits) leaves
 * visible, as a saved preference change does; no file is written. */
void radios_header_status_apply_hidden(RadioSViewContext *ctx, guint hidden);
/* Returns NULL if this context has no bound header. */
GtkWidget *radios_header_status_preferences_new(RadioSViewContext *ctx);

G_END_DECLS
#endif
