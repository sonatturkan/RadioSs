#ifndef RADIOS_PROGRAMME_PREREQUISITES_H
#define RADIOS_PROGRAMME_PREREQUISITES_H
#include "radios/data.h"
void radios_programme_prerequisites_open(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *document_id);
/* All verified station-library media can be selected. Uses only explicit
 * existing /v1/rights grants; never creates media/content/outputs implicitly. */
void radios_media_rights_open(RadioSViewContext *ctx, GtkWidget *owner);
#endif
