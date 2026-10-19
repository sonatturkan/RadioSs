#ifndef RADIOS_HEADER_PROGRESS_H
#define RADIOS_HEADER_PROGRESS_H

#include "radios/views.h"

/* Shared MAIN song position. Deck selection and all GTK callbacks run on the
 * UI thread; actual seeks use the existing asynchronous audio controller. */
GtkWidget *radios_header_progress_new(RadioSViewContext *ctx);

#endif
