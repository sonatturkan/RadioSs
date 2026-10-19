#ifndef RADIOS_LOUDNESS_SETTINGS_H
#define RADIOS_LOUDNESS_SETTINGS_H
#include "radios/views.h"
/* Read-only baseline until loaded; edits change LUFS scope without transport commands. */
GtkWidget *radios_loudness_settings_new(RadioSViewContext *ctx);
#endif
