#ifndef RADIOS_AUTODJ_MARKER_JOBS_H
#define RADIOS_AUTODJ_MARKER_JOBS_H
#include "radios/data.h"
typedef void (*RadioSAutoDJMarkersProgress)(guint completed, guint total, gpointer user);
/* Source the backend records for automatically calculated AutoDJ points;
 * "manual" marks points the operator placed. Keep in step with SDBASE. */
#define RADIOS_AUTODJ_POINTS_ALGORITHM "radios-autodj-mix-v2"
/* TRUE when a verified music row (library list or media detail) still needs
 * automatic AutoDJ points: a point is missing or came from an earlier
 * automatic analysis. Manual and current automatic points are complete. */
gboolean radios_autodj_points_due(JsonObject *row);
/* Worker thread: one revision-checked markers-assign request for `row`.
 * Manual points are never replaced; `overwrite` also recalculates current
 * automatic points (after a new beat grid). Returns the response object. */
JsonNode *radios_autodj_points_assign_sync(RadioSDataClient *client, JsonObject *row, gboolean overwrite,
    gdouble threshold_db, guint out_lead_ms, const gchar *playlist_id, GCancellable *cancel, GError **error);
/* Captures the station identity and target snapshot. One revision-checked
 * request per media; cancellation stops later writes. Closing owner cancels.
 * Progress and completion run on the GTK thread. Completion returns counts,
 * partial cancellation and first_error; already acknowledged saves remain. */
GCancellable *radios_autodj_markers_scan_async(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *playlist_id, gboolean overwrite, gdouble threshold_db, guint out_lead_ms,
    RadioSAutoDJMarkersProgress progress, RadioSDataCallback callback,
    gpointer user, GDestroyNotify destroy);
#endif
