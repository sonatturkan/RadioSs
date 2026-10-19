#ifndef RADIOS_DATA_CLIENT_H
#define RADIOS_DATA_CLIENT_H

#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include "radios/config.h"
#include "radios/runtime.h"

G_BEGIN_DECLS

typedef struct RadioSDataClient RadioSDataClient;

typedef enum {
    RADIOS_DATA_CLIENT_ERROR_INVALID_CONFIG,
    RADIOS_DATA_CLIENT_ERROR_CREDENTIALS,
    RADIOS_DATA_CLIENT_ERROR_TRANSPORT,
    RADIOS_DATA_CLIENT_ERROR_TIMEOUT,
    RADIOS_DATA_CLIENT_ERROR_CANCELLED,
    RADIOS_DATA_CLIENT_ERROR_INVALID_RESPONSE,
    RADIOS_DATA_CLIENT_ERROR_RESPONSE_TOO_LARGE,
    RADIOS_DATA_CLIENT_ERROR_FORBIDDEN,
    RADIOS_DATA_CLIENT_ERROR_NOT_FOUND,
    RADIOS_DATA_CLIENT_ERROR_CONFLICT,
    RADIOS_DATA_CLIENT_ERROR_VALIDATION,
    RADIOS_DATA_CLIENT_ERROR_RATE_LIMITED,
    RADIOS_DATA_CLIENT_ERROR_UNAVAILABLE,
    RADIOS_DATA_CLIENT_ERROR_COMMITTED,
    RADIOS_DATA_CLIENT_ERROR_HTTP,
    RADIOS_DATA_CLIENT_ERROR_STORE_MISMATCH,
    RADIOS_DATA_CLIENT_ERROR_SCHEMA_MISMATCH
} RadioSDataClientError;

#define RADIOS_DATA_CLIENT_ERROR (radios_data_client_error_quark())
GQuark radios_data_client_error_quark(void);

/* Owns copies of configuration/identity fields. Reads only the project-local
 * 0600 credentials file. An explicitly configured token environment variable
 * overrides the user token, with validation. Tokens have no public getter. */
RadioSDataClient *radios_data_client_new(const RadioSConfig *config, GError **error);
RadioSDataClient *radios_data_client_ref(RadioSDataClient *client);
void radios_data_client_unref(RadioSDataClient *client);
const gchar *radios_data_client_station_id(const RadioSDataClient *client);
/* Interned timezone name from authenticated station context, defaults to UTC. */
const gchar *radios_data_client_station_timezone(const RadioSDataClient *client);
const gchar *radios_data_client_endpoint_id(const RadioSDataClient *client);
const gchar *radios_data_client_store_id(const RadioSDataClient *client);
const gchar *radios_data_client_user_id(const RadioSDataClient *client);
const gchar *radios_data_client_service_url(const RadioSDataClient *client);
/* Thread-safe comparison, including an adopted authenticated user. */
gboolean radios_data_client_same_identity(const RadioSDataClient *a, const RadioSDataClient *b);
/* Remaining server Retry-After delay, rounded up; safe on the main thread. */
guint radios_data_client_retry_delay(RadioSDataClient *client);

/* Blocking worker-only HTTP request; object/array JSON result owned by caller.
 * No redirects or proxies. An explicit bootstrap/desktop read whitelist and receipt-backed
 * idempotent mutations retry HTTP 429: at most five attempts and an eight-second retry window,
 * with 1–3 second waits and cancellation between sends. A longer Retry-After
 * is returned to the caller without sending early.
 * Non-idempotent mutations and other errors never automatically retry here.
 * Errors never expose credentials.
 * Paths are API paths beginning with '/', optionally with an encoded query;
 * absolute URLs, fragments and control characters are rejected.
 * A COMMITTED error means the service says a write committed but its follow-up
 * failed: refresh and reconcile rather than blindly retrying that write. */
JsonNode *radios_data_client_request_sync(RadioSDataClient *client,
                                        const gchar *method,
                                        const gchar *path,
                                        JsonNode *body,
                                        const RadioSCancelToken *cancel,
                                        GError **error);
/* Additionally supports one operation's cancellation, without stopping the runtime. */
JsonNode *radios_data_client_request_cancellable_sync(RadioSDataClient *client,
    const gchar *method, const gchar *path, JsonNode *body,
    const RadioSCancelToken *cancel, GCancellable *cancellable, GError **error);

/* GET /health checks the expected store UUID; GET /v1/capabilities verifies
 * schema 4, desktop API version and installed source digest. Call on the worker before exposing connected application state. */
gboolean radios_data_client_handshake_sync(RadioSDataClient *client,
                                         const RadioSCancelToken *cancel,
                                         GError **error);
/* TRUE when the last verified handshake's /v1/capabilities "workflow_features"
 * listed name (e.g. "kind_transition_distribution"); FALSE before any
 * handshake or when the service lists none. Thread-safe; each handshake
 * (also on reconnect) replaces the list. */
gboolean radios_data_client_has_feature(RadioSDataClient *client, const gchar *name);
/* Persist before asynchronous queue admission. Safe read/non-idempotent
 * operations are ignored; a durable write failure prevents submission. */
gboolean radios_data_client_prepare(RadioSDataClient *client, const gchar *method,
    const gchar *path, JsonNode *body, GError **error);
JsonNode *radios_data_client_recovery_status(RadioSDataClient *client, GError **error);
/* Worker-only, current identity handshake required; replays exact pending
 * receipt-backed bodies. Never starts audio or rebases rejected revisions. */
guint radios_data_client_recover_sync(RadioSDataClient *client,
    const RadioSCancelToken *cancel, guint limit, GError **error);

G_END_DECLS
#endif
