#ifndef RADIOS_EXTERNAL_ACCESS_H
#define RADIOS_EXTERNAL_ACCESS_H
#include <gio/gio.h>

/* Header-only reachability check, not authentication or an audio proof.
 * Never sends installation/provider credentials or follows redirects. */
gboolean radios_external_https_url_valid(const gchar *url);
gboolean radios_external_probe_sync(const gchar *url, const gchar *ca_file,
    GCancellable *cancel, glong *http_status, GError **error);
#endif
