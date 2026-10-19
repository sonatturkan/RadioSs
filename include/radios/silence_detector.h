#ifndef RADIOS_SILENCE_DETECTOR_H
#define RADIOS_SILENCE_DETECTOR_H

#include <glib.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Dead-air protection (Acil › Sessizlikte otomatik devralma, design AC-09).
 * The settings come from the emergency preparation map the native Emergency
 * page saves ("silence/<field label>" keys). The detector is a pure state
 * machine fed with MAIN output levels; it never touches audio itself. */

typedef enum {
    RADIOS_SILENCE_ACTION_NONE,           /* "Ek işlem yok" */
    RADIOS_SILENCE_ACTION_ALARM,          /* "Yalnız alarm" */
    RADIOS_SILENCE_ACTION_NEXT_DECK,      /* "İkinci deck · yedek oynatıcı" */
    RADIOS_SILENCE_ACTION_EMERGENCY_LIST  /* "Acil liste" */
} RadioSSilenceAction;

typedef enum { RADIOS_SILENCE_MEASURE_RMS, RADIOS_SILENCE_MEASURE_PEAK } RadioSSilenceMeasure;

typedef struct {
    gboolean enabled;
    gdouble threshold_db;          /* silence is a level below this (-120..0) */
    gdouble seconds;               /* continuous silence before step 1 (1..3600) */
    RadioSSilenceAction step1;     /* what happens when the time is up */
    gdouble step2_seconds;         /* 0: no second step; otherwise extra seconds */
    RadioSSilenceAction step2;
    gdouble restore_db;            /* audio counts as back at or above this (> threshold) */
    gdouble restore_seconds;       /* continuous audio before the incident closes (0..600) */
    gboolean both_channels;        /* TRUE: silent only when both channels are silent */
    RadioSSilenceMeasure measure;
    gchar monitor[48];             /* "Program çıkışı" is the local MAIN output */
} RadioSSilenceSettings;

void radios_silence_settings_default(RadioSSilenceSettings *settings);
/* Missing fields keep the defaults; a present but invalid field fails. */
gboolean radios_silence_settings_from_preparation(JsonObject *preparation,
                                                  RadioSSilenceSettings *out, GError **error);
gboolean radios_silence_settings_validate(const RadioSSilenceSettings *settings, GError **error);
/* Only the programme (MAIN) output can be measured by this workstation. */
gboolean radios_silence_settings_local(const RadioSSilenceSettings *settings);
const gchar *radios_silence_action_label(RadioSSilenceAction action);

typedef enum {
    RADIOS_SILENCE_EVENT_NONE,
    RADIOS_SILENCE_EVENT_SILENCE_BEGAN,   /* level fell below the threshold */
    RADIOS_SILENCE_EVENT_SILENCE_ENDED,   /* audio returned before step 1 */
    RADIOS_SILENCE_EVENT_STEP1,           /* silence lasted `seconds` */
    RADIOS_SILENCE_EVENT_STEP2,           /* and `step2_seconds` more */
    RADIOS_SILENCE_EVENT_RESTORED         /* audio back for `restore_seconds` after an alarm */
} RadioSSilenceEvent;

typedef enum {
    RADIOS_SILENCE_STATE_SOUND, RADIOS_SILENCE_STATE_QUIET, RADIOS_SILENCE_STATE_ALARM
} RadioSSilenceState;

typedef struct {
    RadioSSilenceState state;
    gint64 quiet_since_us;     /* monotonic; 0 when not quiet */
    gint64 sound_since_us;     /* in ALARM: continuous audio since */
    gint64 incident_started_us;
    gboolean step2_done;
    guint incidents;
    gdouble level_db[2];       /* last decision levels per channel */
    gboolean measurable;
} RadioSSilenceDetector;

void radios_silence_detector_init(RadioSSilenceDetector *detector);
/* 20*log10 of a linear sample level; -120 dB floor. */
gdouble radios_silence_level_db(gdouble linear);
/* One measurement. With measurable FALSE (no MAIN output, device lost) the
 * clocks hold and nothing is reported. Disabled settings reset to SOUND. */
RadioSSilenceEvent radios_silence_detector_feed(RadioSSilenceDetector *detector,
    const RadioSSilenceSettings *settings, gint64 now_us, gboolean measurable,
    gdouble left_linear, gdouble right_linear);
/* Planned silence or operator reset: clocks and incident state cleared. */
void radios_silence_detector_reset(RadioSSilenceDetector *detector);
/* Seconds of continuous silence so far (0 when sound). */
gdouble radios_silence_detector_quiet_seconds(const RadioSSilenceDetector *detector, gint64 now_us);

G_END_DECLS
#endif
