#ifndef RADIOS_LOUDNESS_H
#define RADIOS_LOUDNESS_H
#include <glib.h>
#include <json-glib/json-glib.h>

/* Programme loudness of a whole file (ITU-R BS.1770-4 / EBU R128 integrated,
 * gated 400 ms blocks) with its sample peak (linear, 1.0 = full scale). A
 * silent file has no gated block and stays unmeasured. */
typedef struct {
    gdouble integrated_lufs, peak;
    gboolean measured;
} RadioSLoudness;
#define RADIOS_LOUDNESS_ALGORITHM "radios-bs1770-v1"
#define RADIOS_LOUDNESS_DEFAULT_TARGET_LUFS (-14.0)

typedef struct RadioSLoudnessMeter RadioSLoudnessMeter;
RadioSLoudnessMeter *radios_loudness_meter_new(guint sample_rate, guint channels);
/* One interleaved frame of `channels` samples. */
void radios_loudness_meter_push_frame(RadioSLoudnessMeter *meter, const gfloat *samples);
/* FALSE when no block passes the -70 LUFS absolute gate. */
gboolean radios_loudness_meter_finish(RadioSLoudnessMeter *meter, gdouble *lufs);
void radios_loudness_meter_free(RadioSLoudnessMeter *meter);

/* {"schema":1,"algorithm":...,"integrated_lufs":x|null} for an analysis. */
JsonObject *radios_loudness_json(const RadioSLoudness *loudness);
/* Reads `analysis.loudness` and `analysis.peak`. FALSE without a current
 * measurement object; `measured` is FALSE for a measured silent file. */
gboolean radios_loudness_read(JsonObject *analysis, RadioSLoudness *loudness);
/* Gain in dB that brings the file to `target_lufs`. Boosts stop where the
 * sample peak would pass -1 dBFS, since nothing limits a deck afterwards;
 * cuts are never limited. 0 for an unmeasured file. */
gdouble radios_loudness_gain_db(const RadioSLoudness *loudness, gdouble target_lufs);
#endif
