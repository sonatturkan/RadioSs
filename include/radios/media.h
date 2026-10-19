#ifndef RADIOS_MEDIA_H
#define RADIOS_MEDIA_H

#include <gio/gio.h>
#include <json-glib/json-glib.h>
#include "radios/config.h"
#include "radios/runtime.h"
#include "radios/rhythm.h"
#include "radios/loudness.h"

G_BEGIN_DECLS

#define RADIOS_MEDIA_MAX_SIZE_BYTES (G_GUINT64_CONSTANT(2) * 1024 * 1024 * 1024)
#define RADIOS_MEDIA_MAX_WAVEFORM_BINS 2048u

typedef enum {
    RADIOS_MEDIA_ERROR_IO, RADIOS_MEDIA_ERROR_INVALID_FILE,
    RADIOS_MEDIA_ERROR_UNSUPPORTED, RADIOS_MEDIA_ERROR_DECODE,
    RADIOS_MEDIA_ERROR_CANCELLED, RADIOS_MEDIA_ERROR_CHANGED,
    RADIOS_MEDIA_ERROR_LIMIT, RADIOS_MEDIA_ERROR_CONFINEMENT
} RadioSMediaError;
#define RADIOS_MEDIA_ERROR (radios_media_error_quark())
GQuark radios_media_error_quark(void);

typedef struct {
    gchar *path, *relative_path, *source_name, *sha256;
    gchar *codec, *format, *title, *artist, *album, *genre;
    guint64 size_bytes, sample_frames, waveform_frames_per_bin, clipped_samples;
    gint64 duration_ms, bitrate;
    guint sample_rate, channels;
    gdouble peak, rms;
    gboolean clipped;
    /* GArray(gfloat): min,max pairs across all channels. Uniform frame hop;
     * final bin may be shorter. Actual sample amplitudes, full scale 1.0;
     * float audio can exceed full scale. These are not LUFS or true-peak. */
    GArray *waveform;
    RadioSRhythmAnalysis rhythm;
    RadioSLoudness loudness;
} RadioSMediaAnalysis;

/* Worker-only, read-only, fully decode a regular local audio file. Uses custom
 * descriptor AVIO; external protocols and external media references are denied.
 * SHA-256/stat checks reject a source modified or replaced during analysis. */
RadioSMediaAnalysis *radios_media_analyze(const gchar *path,
    const RadioSCancelToken *cancel, GCancellable *cancellable, GError **error);

/* Worker-only: private UUID copy under the project's configured import_root,
 * then analyze that stable snapshot. On failure/cancel removes its own copy.
 * Success retains the snapshot; relative_path is safe to send to the backend. */
RadioSMediaAnalysis *radios_media_stage_analyze(const RadioSConfig *config,
    const gchar *source, const RadioSCancelToken *cancel,
    GCancellable *cancellable, GError **error);

/* Copy and inspect container headers only. No waveform/BPM/loudness decode;
 * the returned metadata is provisional until the library analysis pass. */
RadioSMediaAnalysis *radios_media_stage_probe(const RadioSConfig *config,
    const gchar *source, const RadioSCancelToken *cancel,
    GCancellable *cancellable, GError **error);

/* Measurement object only, exactly the backend analysis keys. Identity/path and
 * editable tags are intentionally separate packet fields. Caller owns node. */
JsonNode *radios_media_analysis_to_json(const RadioSMediaAnalysis *analysis);
/* TRUE when a stored analysis object needs no new decode: its beat grid is
 * current (present analyzer, including a measured unknown tempo, or manual)
 * and it carries a loudness measurement. */
gboolean radios_media_analysis_complete(JsonObject *analysis);
void radios_media_analysis_free(RadioSMediaAnalysis *analysis);

G_END_DECLS
#endif
