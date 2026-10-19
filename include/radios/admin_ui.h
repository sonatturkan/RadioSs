#ifndef RADIOS_ADMIN_UI_H
#define RADIOS_ADMIN_UI_H
#include "radios/data.h"

/* Attaches lazy, station-scoped controls to the original native technical
 * page. Network and subprocess work starts only after an operator action. */
void radios_admin_ui_attach(RadioSViewContext *ctx, GtkWidget *page,
    GtkWidget *server_content, GtkWidget *audit_content);
void radios_admin_ui_people(RadioSViewContext *ctx, GtkWidget *page,
    JsonObject *context);
void radios_admin_ui_refresh(GtkButton *button, gpointer page);
#endif
