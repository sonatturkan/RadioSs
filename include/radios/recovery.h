#ifndef RADIOS_RECOVERY_H
#define RADIOS_RECOVERY_H
#include <json-glib/json-glib.h>
#include "radios/config.h"
G_BEGIN_DECLS
typedef struct RadioSRecovery RadioSRecovery;
/* Private durable, identity-bound outbox. Contains exact idempotent request
 * bodies, including explicitly saved encoder passwords only in supported
 * stream settings; never authorization headers, tokens or response secrets. */
RadioSRecovery *radios_recovery_new(const RadioSConfig *config, const gchar *store,
    const gchar *station, const gchar *user, const gchar *endpoint);
void radios_recovery_free(RadioSRecovery *recovery);
gboolean radios_recovery_eligible(const gchar *method, const gchar *path, JsonNode *body);
gboolean radios_recovery_prepare(RadioSRecovery *recovery, const gchar *method,
    const gchar *path, JsonNode *body, GError **error);
gboolean radios_recovery_finish(RadioSRecovery *recovery, JsonNode *body,
    gboolean acknowledged, gint rejection_code, GError **error);
/* Owned array of envelopes; invalid files fail closed and remain intact. */
JsonNode *radios_recovery_pending(RadioSRecovery *recovery, GError **error);
JsonNode *radios_recovery_status(RadioSRecovery *recovery, GError **error);
/* Shared process lifetime lock on data\database-service\gui.lock (LockFileEx
 * region shared with the Python tools); release the returned fd with
 * radios_fd_close() after GUI shutdown. Offline restore holds the exclusive
 * lock. Refuse a pending restore even after crash. */
gint radios_recovery_gui_lock(const RadioSConfig *config, GError **error);
G_END_DECLS
#endif
