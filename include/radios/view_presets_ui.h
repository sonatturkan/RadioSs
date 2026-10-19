#ifndef RADIOS_VIEW_PRESETS_UI_H
#define RADIOS_VIEW_PRESETS_UI_H
#include "radios/views.h"
G_BEGIN_DECLS
/* Settings › Görünüm: one row of saved views (radios_settings_views_*).
 * Choosing a view applies it at once; Kaydet stores the current settings
 * under the typed name (or over the chosen view), Sil removes the chosen
 * view; a status label reports the result. */
GtkWidget *radios_view_presets_section_new(RadioSViewContext *ctx);
G_END_DECLS
#endif
