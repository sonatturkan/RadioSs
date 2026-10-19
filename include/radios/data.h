#ifndef RADIOS_DATA_H
#define RADIOS_DATA_H
#include "radios/views.h"
#include "radios/data_client.h"
typedef struct RadioSDocument RadioSDocument;
typedef void (*RadioSDocumentStatus)(RadioSDocument *, const gchar *, gpointer);
typedef void (*RadioSDataCallback)(RadioSViewContext *, GtkWidget *, JsonNode *, const GError *, gpointer);
typedef void (*RadioSDataRefresh)(RadioSViewContext *, GtkWidget *);
typedef void (*RadioSDocumentApply)(RadioSViewContext *, GtkWidget *, JsonObject *, gpointer);
RadioSData *radios_data_new(RadioSViewContext *ctx);
void radios_data_connect(RadioSViewContext *ctx);
void radios_data_close(RadioSData *data);
void radios_data_unref(RadioSData *data);
gboolean radios_data_connected(RadioSViewContext *ctx);
const gchar *radios_data_station_id(RadioSViewContext *ctx);
const gchar *radios_data_endpoint_id(RadioSViewContext *ctx);
JsonObject *radios_data_context(RadioSViewContext *ctx); /* borrowed */
/* Capture on the main thread; the owned immutable identity is safe on the worker. */
RadioSDataClient *radios_data_client_acquire(RadioSViewContext *ctx);
JsonNode *radios_data_recovery_status(RadioSViewContext *ctx); /* owned, summary only */
void radios_data_recovery_retry(RadioSViewContext *ctx);
gboolean radios_data_preserve(RadioSViewContext *ctx, const gchar *path, JsonNode *body);
gboolean radios_data_request(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *method, const gchar *path, JsonNode *body,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy);
void radios_data_register_refresh(RadioSViewContext *ctx, GtkWidget *owner, RadioSDataRefresh refresh);
void radios_data_refresh(RadioSViewContext *ctx);
RadioSDocument *radios_data_document_new(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *kind, const gchar *name, RadioSDocumentApply apply, gpointer user);
/* Resolve and save one immutable record identity, including a new UUID with
 * expected revision zero. Equal display names never retarget the document. */
RadioSDocument *radios_data_document_new_for_id(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *kind, const gchar *name, const gchar *identifier, RadioSDocumentApply apply, gpointer user);
/* New immutable UUID, authenticated connected baseline and expected revision
 * zero. Ready immediately for durable first autosave; occupied IDs conflict.
 * Existing record edits must use new_for_id to read their real CAS baseline. */
RadioSDocument *radios_data_document_new_record(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *kind, const gchar *name, const gchar *identifier, RadioSDocumentApply apply, gpointer user);
gboolean radios_data_document_save(RadioSDocument *document, JsonNode *config);
/* Bounded automatic form save: retry the exact uncertain first intent, then
 * submit only the latest successor after its acknowledged CAS revision.
 * Ready documents can stage against their authenticated baseline offline;
 * FALSE means not submitted/acknowledged, including durable offline staging.
 * Identity changes are retained locally and never written to another client.
 * Call dirty for each change, and preserve_pending once when closing. */
gboolean radios_data_document_autosave(RadioSDocument *document, JsonNode *config);
gboolean radios_data_document_ready(RadioSDocument *document);
/* Main-thread save feedback, including asynchronous rejection and retry. */
void radios_data_document_set_status_callback(RadioSDocument *document, RadioSDocumentStatus callback, gpointer user);
/* A revision is usable for runtime execution only after an acknowledged,
 * clean save. Busy/dirty/uncertain documents return zero. */
const gchar *radios_data_document_id(RadioSDocument *document);
gint64 radios_data_document_revision(RadioSDocument *document);
void radios_data_document_dirty(RadioSDocument *document, gboolean dirty);
/* Closing checkpoint for an acknowledged document with one pending latest
 * save. Persist its exact successor CAS after the in-flight receipt. */
gboolean radios_data_document_preserve_pending(RadioSDocument *document);
/* Existing dirty/retry intent is preserved until the explicit reload dialog
 * is accepted. Clean documents refresh directly. */
void radios_data_document_reload(RadioSDocument *document);
/* Passive, timer-safe read of this document only. Preserves edits and does not
 * reapply an unchanged record; use reload for an explicit operator restore. */
void radios_data_document_refresh(RadioSDocument *document);
JsonNode *radios_data_body(RadioSViewContext *ctx); /* owned object with station_id */
const gchar *radios_json_string(JsonObject *object, const gchar *key, const gchar *fallback);
gint64 radios_json_int(JsonObject *object, const gchar *key, gint64 fallback);
#endif
