#ifndef RADIOS_MEDIA_ARCHIVE_UI_H
#define RADIOS_MEDIA_ARCHIVE_UI_H
#include "radios/views.h"

G_BEGIN_DECLS

/* Ayarlar › Medya arşivi: the folders the station's audio archive is read
 * from (config/media-archive.ini) and where RadioS writes its data
 * (config/storage.ini). "Şimdi tara" hashes the archive on a worker, skips
 * files already in the library and imports the rest through the media
 * library's own sequential import; a subfolder may become a playlist. */
GtkWidget *radios_media_archive_ui_page_new(RadioSViewContext *ctx);

/* Background watch for the lifetime of `window`: with "izle" enabled a scan
 * and import run every N minutes while the data service is connected. The
 * Preferences page shares this runner, so scans never overlap. */
void radios_media_archive_bind(RadioSViewContext *ctx, GtkWidget *window);

G_END_DECLS
#endif
