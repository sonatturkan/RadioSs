#ifndef RADIOS_MEDIA_UI_H
#define RADIOS_MEDIA_UI_H

#include "radios/views.h"
#include "radios/data.h"
#include "radios/rhythm.h"

typedef void (*RadioSMediaPick)(RadioSViewContext *, GtkWidget *, JsonObject *, gpointer);
/* The callback receives a borrowed verified detail. Closing the target cancels
 * the selection; a subsequent picker replaces the previous selection intent. */
void radios_media_library_pick(RadioSViewContext *ctx, GtkWidget *owner,
    RadioSMediaPick callback, gpointer user);
gboolean radios_media_library_get(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *media_id, RadioSDataCallback callback, gpointer user, GDestroyNotify destroy);
GCancellable *radios_media_rhythm_analyze_async(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *media_id, RadioSDataCallback callback, gpointer user, GDestroyNotify destroy);
/* Read the stored analysis first; decode and save only records without a
 * current grid (missing, or from an older analyzer). A measured unknown BPM
 * and an operator's manual grid are retained. */
GCancellable *radios_media_rhythm_ensure_async(RadioSViewContext *ctx, GtkWidget *owner,
    const gchar *media_id, RadioSDataCallback callback, gpointer user, GDestroyNotify destroy);
/* Stores an operator-edited grid for the version and analysis revision in the
 * borrowed `detail`; a newer analysis or version fails with a conflict. The
 * callback receives the saved media snapshot. */
GCancellable *radios_media_rhythm_save_async(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail,
    const RadioSRhythmAnalysis *grid, RadioSDataCallback callback, gpointer user, GDestroyNotify destroy);
/* Analyses, one track at a time, every verified library track without a
 * current grid. The callback receives {"analyzed", "unknown", "failed",
 * "first_error"}. */
GCancellable *radios_media_rhythm_library_scan_async(RadioSViewContext *ctx, GtkWidget *owner,
    RadioSDataCallback callback, gpointer user, GDestroyNotify destroy);
/* Keeps library grids current in the background for the lifetime of `owner`:
 * a pass shortly after the database connects, once the library has been quiet
 * after an import batch (songs are taken in first, then analysed together) and
 * otherwise at the configured interval. Besides completing BPM grids it gives
 * every music track automatic AutoDJ IN/OUT points where they are missing or
 * stale; points the operator placed are never replaced. The passes analyse
 * several tracks at the same time and follow Ayarlar › Kütüphane analizi
 * (library_analysis.h): workers, automatic scans on/off, quiet time, interval. */
void radios_media_rhythm_library_bind(RadioSViewContext *ctx, GtkWidget *owner);
/* Starts a pass now, even with automatic scans off. FALSE when the upkeep is
 * not bound, the database is not connected or a pass already runs. */
gboolean radios_media_rhythm_library_run_now(RadioSViewContext *ctx);
/* Stops the running pass; tracks already analysed stay. */
void radios_media_rhythm_library_cancel(RadioSViewContext *ctx);
/* One line about the last finished pass ("" before the first); *running tells
 * whether a pass runs now. Caller frees the text. */
gchar *radios_media_rhythm_library_status(RadioSViewContext *ctx, gboolean *running);

/* Native library and multi-file import dialog; requests run on workers. */
void radios_media_library_open(RadioSViewContext *ctx);
/* Open/reuse the library and present the existing multi-file audio chooser. */
void radios_media_import_open(RadioSViewContext *ctx);
/* Programmatic batch (Ayarlar › Medya arşivi): `paths` (owned canonical local
 * audio files) go through the library's sequential import with `kind`; with
 * playlist_id/revision the verified records are appended to that playlist
 * like a window drop. `done` runs once on the main thread when the batch
 * ends. FALSE (paths released) when the library is busy with an operator
 * action or the data service is not connected. */
typedef void (*RadioSMediaImportDone)(RadioSViewContext *ctx, guint imported, guint failed,
                                      const gchar *last_error, gpointer user);
gboolean radios_media_import_files(RadioSViewContext *ctx, GPtrArray *paths, const gchar *kind,
    const gchar *playlist_id, gint64 playlist_revision, const gchar *label,
    RadioSMediaImportDone done, gpointer user);
gboolean radios_media_import_busy(RadioSViewContext *ctx);
/* Stops the running batch after the current file; finished records stay. */
void radios_media_import_cancel(RadioSViewContext *ctx);
/* Folder chooser: each chosen folder becomes a playlist named after it and its
 * audio files are imported, as when the folder is dropped on the window. */
void radios_media_folder_import_open(RadioSViewContext *ctx);
/* Changes only after a successful import batch, so local source browsers can
 * refresh new media without resetting selection on ordinary playlist writes. */
guint radios_media_catalogue_generation(RadioSViewContext *ctx);
void radios_media_library_open_selected(RadioSViewContext *ctx, const gchar *media_id);
void radios_media_library_open_for_deck(RadioSViewContext *ctx, guint deck);
void radios_ui_bind_media_library(GtkWidget *button, RadioSViewContext *ctx);
/* Window file-manager drops first offer the existing playlist target. Verified
 * managed-copy imports append with real media/version/file/SHA identities and
 * revision-checked entry-save receipts. Library drops remain direct imports.
 * The context must outlive the widget; source files are never moved/deleted. */
void radios_ui_bind_media_drop(GtkWidget *widget, RadioSViewContext *ctx);

#endif
