#ifndef RADIOS_LIBRARY_ANALYSIS_UI_H
#define RADIOS_LIBRARY_ANALYSIS_UI_H
#include "radios/views.h"

G_BEGIN_DECLS

/* Ayarlar › Kütüphane analizi: how many tracks the background analysis
 * handles at the same time, whether it scans on its own, how long the
 * library must be quiet after an import before it starts, and how often it
 * rescans (config/library-analysis.ini; changes apply to the next scan).
 * "Tüm kütüphaneyi şimdi analiz et" starts a pass at once and shows the live
 * progress of every library scan. */
GtkWidget *radios_library_analysis_ui_page_new(RadioSViewContext *ctx);

G_END_DECLS
#endif
