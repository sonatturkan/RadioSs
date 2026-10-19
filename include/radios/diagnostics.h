#ifndef RADIOS_DIAGNOSTICS_H
#define RADIOS_DIAGNOSTICS_H

#include "radios/config.h"
#include "radios/runtime.h"

/* A read-only worker task. data is a const RadioSConfig, kept alive until the
 * worker has joined. Checks configured directories and DB file existence;
 * existence never implies a connected service or a working audio engine. */
gchar *radios_diagnostics_run(gpointer data, const RadioSCancelToken *cancel,
                             GError **error);

#endif
