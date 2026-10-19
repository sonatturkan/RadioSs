#ifndef RADIOS_WEEKLY_PROGRAMME_UI_H
#define RADIOS_WEEKLY_PROGRAMME_UI_H
#include "radios/views.h"
#include <json-glib/json-glib.h>
GtkWidget *radios_weekly_programme_new(RadioSViewContext *ctx, GtkWidget *main_calendar);
/* Programme picker/properties use main_calendar itself; no second grid/tab. */
GtkWidget *radios_weekly_programme_controls_new(RadioSViewContext *ctx, GtkWidget *main_calendar);
void radios_weekly_programme_prepare_close(GtkWidget *workspace);
void radios_weekly_programme_save_snapshot(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *calendar, const gchar *visible_week);
#endif
