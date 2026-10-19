#ifndef RADIOS_MEDIA_JOBS_H
#define RADIOS_MEDIA_JOBS_H
#include "radios/data.h"

/* Main-thread entry point. Full decoding/staging and HTTP run on the existing
 * bounded worker. Returns an owned cancellation handle, or NULL when no work
 * was scheduled. The callback runs only while owner and its parent live.
 * A sent write may have committed even if cancellation interrupts its reply. */
GCancellable *radios_media_import_async(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *source, const gchar *kind, const gchar *title,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy);
GCancellable *radios_media_import_staged_async(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *source, const gchar *source_root, const gchar *kind, const gchar *title,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy);
#endif
