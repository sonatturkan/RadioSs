#ifndef RADIOS_LIBRARY_ANALYSIS_H
#define RADIOS_LIBRARY_ANALYSIS_H

#include <gio/gio.h>

G_BEGIN_DECLS

/* Ayarlar › Kütüphane analizi: how the background library analysis (missing
 * BPM grids, loudness, automatic AutoDJ IN/OUT points) runs. The workstation
 * file config/library-analysis.ini holds the choices (0600, atomic
 * replacement, links rejected); they belong to this computer, not the station
 * database, because the useful number of parallel workers depends on its
 * processor. */
#define RADIOS_LIBRARY_ANALYSIS_MAX_WORKERS 16u

typedef struct {
    guint workers;          /* tracks analysed at the same time, 1..16 */
    gboolean auto_scan;     /* scan on its own while RadioS runs */
    guint settle_seconds;   /* 3..600: quiet time after the last import before a scan starts */
    guint interval_minutes; /* 1..1440: pause between scans of an unchanged library */
} RadioSLibraryAnalysisSettings;

/* Half of the processor cores, at least 1 and at most 4: analysis must leave
 * room for the live audio engine on the same machine. */
guint radios_library_analysis_default_workers(void);
void radios_library_analysis_settings_init(RadioSLibraryAnalysisSettings *settings);
gboolean radios_library_analysis_settings_validate(const RadioSLibraryAnalysisSettings *settings, GError **error);
/* A missing file yields the defaults without writing. */
gboolean radios_library_analysis_settings_load(const gchar *project_root,
                                               RadioSLibraryAnalysisSettings *out, GError **error);
gboolean radios_library_analysis_settings_save(const gchar *project_root,
                                               const RadioSLibraryAnalysisSettings *settings, GError **error);
/* Never fails: a missing, unreadable or invalid file yields the defaults, so a
 * scan that must start is never blocked by a settings file. project_root may
 * be NULL (defaults). */
void radios_library_analysis_settings_current(const gchar *project_root, RadioSLibraryAnalysisSettings *out);

/* Runs task(0 .. count-1) on `workers` threads and returns when every index
 * ran or `cancel` fired (indices not yet started are then skipped). One worker
 * runs inline on the caller. With several, the threads are the caller's
 * helpers: they run at a lower priority than the audio engine and the caller
 * only waits. task must be thread-safe; it is never called twice for an index. */
typedef void (*RadioSLibraryAnalysisTask)(guint index, gpointer user);
void radios_library_analysis_run(guint count, guint workers, GCancellable *cancel,
                                 RadioSLibraryAnalysisTask task, gpointer user);

/* Live progress of every library scan in this process (background upkeep and
 * the AutoDJ marker scan), for the settings page. Counters restart when the
 * first scan begins; `total` grows as scans learn how much there is to do. */
void radios_library_analysis_activity_begin(guint workers);
void radios_library_analysis_activity_add_total(guint count);
void radios_library_analysis_activity_step(void);
void radios_library_analysis_activity_end(void);
/* TRUE while a scan runs. Any out pointer may be NULL. */
gboolean radios_library_analysis_activity(guint *done, guint *total, guint *workers);

G_END_DECLS

#endif
