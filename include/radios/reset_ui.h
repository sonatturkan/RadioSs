#ifndef RADIOS_RESET_UI_H
#define RADIOS_RESET_UI_H
#include "radios/views.h"

/* Settings › Sıfırlama: general settings, playlists, imported songs, mixer,
 * automations and factory reset. Each shows what it affects, asks for
 * confirmation (factory: typing SIFIRLA) and backs the database up first by
 * default. Rows are retired, never erased; history and receipts stay. */
GtkWidget *radios_reset_ui_page_new(RadioSViewContext *ctx);
/* Closes RadioS and opens it again through its launcher. */
gboolean radios_reset_ui_restart(RadioSViewContext *ctx, GError **error);
#endif
