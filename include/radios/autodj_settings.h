#ifndef RADIOS_AUTODJ_SETTINGS_H
#define RADIOS_AUTODJ_SETTINGS_H

#include "radios/views.h"

G_BEGIN_DECLS

/* Native preferences page. Opening it reads the station defaults and playlist
 * catalogue; automatic marker writes begin only after the operator starts. */
GtkWidget *radios_autodj_settings_new(RadioSViewContext *ctx);

G_END_DECLS
#endif
