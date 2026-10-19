#ifndef RADIOS_ADVERT_VIEWS_H
#define RADIOS_ADVERT_VIEWS_H
#include "radios/data.h"

typedef struct RadioSAdvertViews RadioSAdvertViews;
/* Owned by page; all network callbacks retain only their live GTK owner. */
RadioSAdvertViews *radios_advert_views_new(RadioSViewContext *ctx, GtkWidget *page);
GtkWidget *radios_advert_views_notifications(RadioSAdvertViews *views);
GtkWidget *radios_advert_views_tools(RadioSAdvertViews *views);
void radios_advert_views_select(RadioSAdvertViews *views, JsonObject *campaign);
void radios_advert_views_refresh(RadioSAdvertViews *views);
void radios_advert_page_refresh(RadioSViewContext *ctx, GtkWidget *page);
void radios_advert_views_bind_report(RadioSAdvertViews *views,
    GtkWidget *versions, GtkWidget **counters, GtkWidget *table,
    GtkWidget *download, GtkWidget *plan_download, GtkWidget *file_status,
    GtkWidget *plan_status);
#endif
