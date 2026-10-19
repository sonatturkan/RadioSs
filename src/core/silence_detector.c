#include "radios/silence_detector.h"
#include <math.h>
#include <string.h>

static const gchar *const action_labels[] = {"Ek işlem yok", "Yalnız alarm", "İkinci deck · yedek oynatıcı", "Acil liste"};

static gboolean fail(GError **error, const gchar *message)
{
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, message);
    return FALSE;
}

void radios_silence_settings_default(RadioSSilenceSettings *settings)
{
    g_return_if_fail(settings != NULL);
    memset(settings, 0, sizeof *settings);
    settings->threshold_db = -50; settings->seconds = 20;
    settings->step1 = RADIOS_SILENCE_ACTION_ALARM;
    settings->step2_seconds = 0; settings->step2 = RADIOS_SILENCE_ACTION_NONE;
    settings->restore_db = -40; settings->restore_seconds = 5;
    settings->both_channels = TRUE; settings->measure = RADIOS_SILENCE_MEASURE_RMS;
    g_strlcpy(settings->monitor, "Program çıkışı", sizeof settings->monitor);
}

const gchar *radios_silence_action_label(RadioSSilenceAction action)
{ return action <= RADIOS_SILENCE_ACTION_EMERGENCY_LIST ? action_labels[action] : "?"; }

gboolean radios_silence_settings_local(const RadioSSilenceSettings *settings)
{ return settings && g_str_equal(settings->monitor, "Program çıkışı"); }

static const gchar *field(JsonObject *preparation, const gchar *label)
{
    gchar *key = g_strconcat("silence/", label, NULL);
    JsonNode *node = preparation ? json_object_get_member(preparation, key) : NULL;
    g_free(key);
    const gchar *value = node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_STRING ?
        json_node_get_string(node) : NULL;
    return value && *g_strstrip((gchar *)value) ? value : NULL;
}

/* Entry text: optional sign, Turkish or dotted decimals, surrounding space. */
static gboolean number(const gchar *text, gdouble *out)
{
    if (!text) return FALSE;
    gchar *copy = g_strdup(text); g_strstrip(copy);
    for (gchar *p = copy; *p; ++p) if (*p == ',') *p = '.';
    gchar *end = NULL; gdouble value = g_ascii_strtod(copy, &end);
    gboolean ok = *copy && end && !*end && isfinite(value);
    g_free(copy);
    if (ok) *out = value;
    return ok;
}

static gboolean action_of(const gchar *text, RadioSSilenceAction *out)
{
    for (guint i = 0; i < G_N_ELEMENTS(action_labels); ++i)
        if (g_str_equal(text, action_labels[i])) { *out = (RadioSSilenceAction)i; return TRUE; }
    return FALSE;
}

gboolean radios_silence_settings_from_preparation(JsonObject *preparation, RadioSSilenceSettings *out, GError **error)
{
    g_return_val_if_fail(out != NULL, FALSE);
    g_return_val_if_fail(error == NULL || *error == NULL, FALSE);
    RadioSSilenceSettings s; radios_silence_settings_default(&s);
    const gchar *text;
    if ((text = field(preparation, "Sessizlikte otomatik devralma"))) s.enabled = g_str_equal(text, "1");
    if ((text = field(preparation, "Eşik · dB")) && !number(text, &s.threshold_db)) return fail(error, "Sessizlik eşiği sayı olmalı (ör. -50).");
    if ((text = field(preparation, "Süre · sn")) && !number(text, &s.seconds)) return fail(error, "Sessizlik süresi saniye cinsinden sayı olmalı.");
    if ((text = field(preparation, "2 · Sürerse · sn")) && !number(text, &s.step2_seconds)) return fail(error, "İkinci basamak süresi saniye cinsinden sayı olmalı.");
    if ((text = field(preparation, "Ses dönünce eşik · dB")) && !number(text, &s.restore_db)) return fail(error, "Dönüş eşiği sayı olmalı (ör. -40).");
    if ((text = field(preparation, "Dönüş süresi · sn")) && !number(text, &s.restore_seconds)) return fail(error, "Dönüş süresi saniye cinsinden sayı olmalı.");
    if ((text = field(preparation, "1 · Süre dolunca")) && (!action_of(text, &s.step1) || s.step1 == RADIOS_SILENCE_ACTION_NONE))
        return fail(error, "Süre dolunca yapılacak işlem tanınmadı.");
    if ((text = field(preparation, "Sonra")) && (!action_of(text, &s.step2) || s.step2 == RADIOS_SILENCE_ACTION_NEXT_DECK))
        return fail(error, "İkinci basamak işlemi tanınmadı.");
    if ((text = field(preparation, "Sessiz sayılması"))) {
        if (g_str_equal(text, "İki kanal da")) s.both_channels = TRUE;
        else if (g_str_equal(text, "Herhangi bir kanal")) s.both_channels = FALSE;
        else return fail(error, "Sessiz sayılma kuralı tanınmadı.");
    }
    if ((text = field(preparation, "Ölçüm"))) {
        if (g_str_equal(text, "RMS")) s.measure = RADIOS_SILENCE_MEASURE_RMS;
        else if (g_str_equal(text, "Tepe")) s.measure = RADIOS_SILENCE_MEASURE_PEAK;
        else return fail(error, "Ölçüm türü RMS ya da Tepe olmalı.");
    }
    if ((text = field(preparation, "İzlenen nokta"))) g_strlcpy(s.monitor, text, sizeof s.monitor);
    if (s.enabled && !radios_silence_settings_validate(&s, error)) return FALSE;
    *out = s;
    return TRUE;
}

gboolean radios_silence_settings_validate(const RadioSSilenceSettings *s, GError **error)
{
    g_return_val_if_fail(s != NULL, FALSE);
    if (!(s->threshold_db >= -120 && s->threshold_db <= 0)) return fail(error, "Sessizlik eşiği -120 ile 0 dB arasında olmalı (ör. -50).");
    if (!(s->seconds >= 1 && s->seconds <= 3600)) return fail(error, "Sessizlik süresi 1–3600 saniye olmalı.");
    if (!(s->step2_seconds >= 0 && s->step2_seconds <= 3600)) return fail(error, "İkinci basamak süresi 0–3600 saniye olmalı.");
    if (!(s->restore_db > s->threshold_db && s->restore_db <= 0)) return fail(error, "Dönüş eşiği sessizlik eşiğinden yüksek ve en çok 0 dB olmalı.");
    if (!(s->restore_seconds >= 0 && s->restore_seconds <= 600)) return fail(error, "Dönüş süresi 0–600 saniye olmalı.");
    if (s->step1 == RADIOS_SILENCE_ACTION_NONE) return fail(error, "Süre dolunca bir işlem seçilmeli.");
    return TRUE;
}

void radios_silence_detector_init(RadioSSilenceDetector *detector)
{
    g_return_if_fail(detector != NULL);
    memset(detector, 0, sizeof *detector);
    detector->level_db[0] = detector->level_db[1] = -120;
}

void radios_silence_detector_reset(RadioSSilenceDetector *detector)
{
    guint incidents = detector->incidents;
    radios_silence_detector_init(detector);
    detector->incidents = incidents;
}

gdouble radios_silence_level_db(gdouble linear)
{
    if (!(linear > 1e-6) || !isfinite(linear)) return -120;
    return MAX(-120.0, 20.0 * log10(linear));
}

gdouble radios_silence_detector_quiet_seconds(const RadioSSilenceDetector *detector, gint64 now_us)
{
    if (!detector->quiet_since_us || now_us < detector->quiet_since_us) return 0;
    return (gdouble)(now_us - detector->quiet_since_us) / G_USEC_PER_SEC;
}

RadioSSilenceEvent radios_silence_detector_feed(RadioSSilenceDetector *d, const RadioSSilenceSettings *s,
    gint64 now_us, gboolean measurable, gdouble left_linear, gdouble right_linear)
{
    g_return_val_if_fail(d && s, RADIOS_SILENCE_EVENT_NONE);
    d->measurable = measurable;
    if (!s->enabled) { if (d->state != RADIOS_SILENCE_STATE_SOUND) radios_silence_detector_reset(d); return RADIOS_SILENCE_EVENT_NONE; }
    if (!measurable) return RADIOS_SILENCE_EVENT_NONE; /* clocks hold until the output is back */
    gdouble left = radios_silence_level_db(left_linear), right = radios_silence_level_db(right_linear);
    d->level_db[0] = left; d->level_db[1] = right;
    gboolean left_quiet = left < s->threshold_db, right_quiet = right < s->threshold_db;
    gboolean silent = s->both_channels ? (left_quiet && right_quiet) : (left_quiet || right_quiet);
    gboolean left_back = left >= s->restore_db, right_back = right >= s->restore_db;
    gboolean back = s->both_channels ? (left_back || right_back) : (left_back && right_back);
    switch (d->state) {
    case RADIOS_SILENCE_STATE_SOUND:
        if (!silent) return RADIOS_SILENCE_EVENT_NONE;
        d->state = RADIOS_SILENCE_STATE_QUIET; d->quiet_since_us = now_us;
        return RADIOS_SILENCE_EVENT_SILENCE_BEGAN;
    case RADIOS_SILENCE_STATE_QUIET:
        if (!silent) { d->state = RADIOS_SILENCE_STATE_SOUND; d->quiet_since_us = 0; return RADIOS_SILENCE_EVENT_SILENCE_ENDED; }
        if ((gdouble)(now_us - d->quiet_since_us) < s->seconds * G_USEC_PER_SEC) return RADIOS_SILENCE_EVENT_NONE;
        d->state = RADIOS_SILENCE_STATE_ALARM; d->incident_started_us = d->quiet_since_us;
        d->sound_since_us = 0; d->step2_done = FALSE; ++d->incidents;
        return RADIOS_SILENCE_EVENT_STEP1;
    case RADIOS_SILENCE_STATE_ALARM:
        if (back) {
            if (!d->sound_since_us) d->sound_since_us = now_us;
            if ((gdouble)(now_us - d->sound_since_us) < s->restore_seconds * G_USEC_PER_SEC) return RADIOS_SILENCE_EVENT_NONE;
            d->state = RADIOS_SILENCE_STATE_SOUND; d->quiet_since_us = d->sound_since_us = 0;
            return RADIOS_SILENCE_EVENT_RESTORED;
        }
        d->sound_since_us = 0;
        if (!d->step2_done && s->step2_seconds > 0 && s->step2 != RADIOS_SILENCE_ACTION_NONE &&
            (gdouble)(now_us - d->quiet_since_us) >= (s->seconds + s->step2_seconds) * G_USEC_PER_SEC) {
            d->step2_done = TRUE; return RADIOS_SILENCE_EVENT_STEP2;
        }
        return RADIOS_SILENCE_EVENT_NONE;
    }
    return RADIOS_SILENCE_EVENT_NONE;
}
