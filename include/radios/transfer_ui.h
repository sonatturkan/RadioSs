#ifndef RADIOS_TRANSFER_UI_H
#define RADIOS_TRANSFER_UI_H
#include "radios/views.h"
G_BEGIN_DECLS
/* Ayarlar › Export: radyo ayarlarını ve medyayı tek bir .st aktarım paketi
 * olarak seçilen klasöre yazar. Kapsam: tüm ayarlar, seçili ayar grupları
 * ve/veya medya (dosyalar, analizler, albüm ilişkileri, klasör düzeni).
 * The page box is named "preferences-export-page". */
GtkWidget *radios_transfer_export_page_new(RadioSViewContext *ctx);
/* Ayarlar › Import: bir .st paketini seçer, içeriğini gösterir ve ayarları
 * ilişkileriyle geri yükler; paket medya taşıyorsa medya seçilen ana klasöre
 * albüm/alt klasör düzeniyle yerleştirilir ve analizler yeniden bağlanır.
 * The page box is named "preferences-import-page". */
GtkWidget *radios_transfer_import_page_new(RadioSViewContext *ctx);
G_END_DECLS
#endif
