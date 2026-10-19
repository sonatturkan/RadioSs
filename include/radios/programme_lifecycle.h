#ifndef RADIOS_PROGRAMME_LIFECYCLE_H
#define RADIOS_PROGRAMME_LIFECYCLE_H
#include "radios/data.h"
/* The broadcast owner retains the document slot until its widget is destroyed. */
GtkWidget *radios_programme_lifecycle_new(RadioSViewContext *ctx,
    GtkWidget *programme_owner, RadioSDocument **document);
#endif
