#include "radios/silence_detector.h"
#include <math.h>
#include <string.h>

static JsonObject *preparation(const gchar *const *pairs)
{
    JsonObject *object = json_object_new();
    for (guint i = 0; pairs[i]; i += 2) {
        gchar *key = g_strconcat("silence/", pairs[i], NULL);
        json_object_set_string_member(object, key, pairs[i + 1]); g_free(key);
    }
    return object;
}

static void test_defaults_and_parsing(void)
{
    RadioSSilenceSettings s; GError *error = NULL;
    g_assert_true(radios_silence_settings_from_preparation(NULL, &s, &error)); g_assert_no_error(error);
    g_assert_false(s.enabled); g_assert_cmpfloat(s.threshold_db, ==, -50); g_assert_cmpfloat(s.seconds, ==, 20);
    g_assert_cmpint(s.step1, ==, RADIOS_SILENCE_ACTION_ALARM); g_assert_true(s.both_channels); g_assert_true(radios_silence_settings_local(&s));
    const gchar *full[] = {"Sessizlikte otomatik devralma", "1", "Eşik · dB", " -45,5 ", "Süre · sn", "12",
        "1 · Süre dolunca", "İkinci deck · yedek oynatıcı", "2 · Sürerse · sn", "30", "Sonra", "Acil liste",
        "Ses dönünce eşik · dB", "-30", "Dönüş süresi · sn", "4", "Sessiz sayılması", "Herhangi bir kanal", "Ölçüm", "Tepe",
        "İzlenen nokta", "İnternet dönüşü", NULL};
    JsonObject *object = preparation(full);
    g_assert_true(radios_silence_settings_from_preparation(object, &s, &error)); g_assert_no_error(error);
    g_assert_true(s.enabled); g_assert_cmpfloat(s.threshold_db, ==, -45.5); g_assert_cmpfloat(s.seconds, ==, 12);
    g_assert_cmpint(s.step1, ==, RADIOS_SILENCE_ACTION_NEXT_DECK); g_assert_cmpfloat(s.step2_seconds, ==, 30);
    g_assert_cmpint(s.step2, ==, RADIOS_SILENCE_ACTION_EMERGENCY_LIST); g_assert_cmpfloat(s.restore_db, ==, -30);
    g_assert_cmpfloat(s.restore_seconds, ==, 4); g_assert_false(s.both_channels); g_assert_cmpint(s.measure, ==, RADIOS_SILENCE_MEASURE_PEAK);
    g_assert_cmpstr(s.monitor, ==, "İnternet dönüşü"); g_assert_false(radios_silence_settings_local(&s));
    json_object_unref(object);
    /* Invalid values fail with a Turkish message only when the guard is enabled. */
    const gchar *bad_text[] = {"Sessizlikte otomatik devralma", "1", "Eşik · dB", "sessiz", NULL};
    object = preparation(bad_text);
    g_assert_false(radios_silence_settings_from_preparation(object, &s, &error)); g_assert_nonnull(strstr(error->message, "eşiği")); g_clear_error(&error);
    json_object_unref(object);
    const gchar *bad_restore[] = {"Sessizlikte otomatik devralma", "1", "Eşik · dB", "-40", "Ses dönünce eşik · dB", "-45", NULL};
    object = preparation(bad_restore);
    g_assert_false(radios_silence_settings_from_preparation(object, &s, &error)); g_assert_nonnull(strstr(error->message, "Dönüş eşiği")); g_clear_error(&error);
    json_object_unref(object);
    const gchar *positive[] = {"Sessizlikte otomatik devralma", "1", "Eşik · dB", "45", NULL};
    object = preparation(positive);
    g_assert_false(radios_silence_settings_from_preparation(object, &s, &error)); g_assert_nonnull(strstr(error->message, "-120")); g_clear_error(&error);
    json_object_unref(object);
    const gchar *disabled_bad[] = {"Sessizlikte otomatik devralma", "0", "Eşik · dB", "45", NULL};
    object = preparation(disabled_bad);
    g_assert_true(radios_silence_settings_from_preparation(object, &s, &error)); g_assert_false(s.enabled);
    json_object_unref(object);
    const gchar *unknown_action[] = {"Sessizlikte otomatik devralma", "1", "1 · Süre dolunca", "Ek işlem yok", NULL};
    object = preparation(unknown_action);
    g_assert_false(radios_silence_settings_from_preparation(object, &s, &error)); g_clear_error(&error);
    json_object_unref(object);
    g_assert_cmpfloat(radios_silence_level_db(1.0), ==, 0);
    g_assert_cmpfloat(fabs(radios_silence_level_db(0.1) + 20), <, 0.001);
    g_assert_cmpfloat(radios_silence_level_db(0), ==, -120);
}

static RadioSSilenceSettings alarm_settings(void)
{
    RadioSSilenceSettings s; radios_silence_settings_default(&s);
    s.enabled = TRUE; s.threshold_db = -50; s.seconds = 10; s.restore_db = -40; s.restore_seconds = 3;
    s.step2_seconds = 20; s.step2 = RADIOS_SILENCE_ACTION_EMERGENCY_LIST;
    return s;
}

#define SEC(n) ((gint64)(n) * G_USEC_PER_SEC)
static const gdouble LOUD = 0.1, QUIET = 0.001, MIDDLE = 0.005; /* -20, -60, -46 dB */

static void test_state_machine(void)
{
    RadioSSilenceSettings s = alarm_settings(); RadioSSilenceDetector d; radios_silence_detector_init(&d);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(0), TRUE, LOUD, LOUD), ==, RADIOS_SILENCE_EVENT_NONE);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(1), TRUE, QUIET, QUIET), ==, RADIOS_SILENCE_EVENT_SILENCE_BEGAN);
    g_assert_cmpint(d.state, ==, RADIOS_SILENCE_STATE_QUIET);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(5), TRUE, QUIET, QUIET), ==, RADIOS_SILENCE_EVENT_NONE);
    g_assert_cmpfloat(radios_silence_detector_quiet_seconds(&d, SEC(5)), ==, 4);
    /* Audio before the deadline ends the quiet spell without an alarm. */
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(6), TRUE, LOUD, QUIET), ==, RADIOS_SILENCE_EVENT_SILENCE_ENDED);
    g_assert_cmpint(d.state, ==, RADIOS_SILENCE_STATE_SOUND); g_assert_cmpuint(d.incidents, ==, 0);
    /* Ten seconds of silence raise step 1 exactly once. */
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(10), TRUE, QUIET, QUIET), ==, RADIOS_SILENCE_EVENT_SILENCE_BEGAN);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(19), TRUE, QUIET, QUIET), ==, RADIOS_SILENCE_EVENT_NONE);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(20), TRUE, QUIET, QUIET), ==, RADIOS_SILENCE_EVENT_STEP1);
    g_assert_cmpint(d.state, ==, RADIOS_SILENCE_STATE_ALARM); g_assert_cmpuint(d.incidents, ==, 1);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(21), TRUE, QUIET, QUIET), ==, RADIOS_SILENCE_EVENT_NONE);
    /* A level between the two thresholds is neither silence nor recovery. */
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(25), TRUE, MIDDLE, MIDDLE), ==, RADIOS_SILENCE_EVENT_NONE);
    /* Step 2 after 10 + 20 seconds of silence, once. */
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(39), TRUE, QUIET, QUIET), ==, RADIOS_SILENCE_EVENT_NONE);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(40), TRUE, QUIET, QUIET), ==, RADIOS_SILENCE_EVENT_STEP2);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(50), TRUE, QUIET, QUIET), ==, RADIOS_SILENCE_EVENT_NONE);
    /* Recovery needs restore_seconds of continuous audio above the restore level. */
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(60), TRUE, LOUD, QUIET), ==, RADIOS_SILENCE_EVENT_NONE);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(62), TRUE, LOUD, QUIET), ==, RADIOS_SILENCE_EVENT_NONE);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(62) + 1, TRUE, QUIET, QUIET), ==, RADIOS_SILENCE_EVENT_NONE); /* interrupted: clock restarts */
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(63), TRUE, LOUD, LOUD), ==, RADIOS_SILENCE_EVENT_NONE);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(65), TRUE, LOUD, LOUD), ==, RADIOS_SILENCE_EVENT_NONE);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(66), TRUE, LOUD, LOUD), ==, RADIOS_SILENCE_EVENT_RESTORED);
    g_assert_cmpint(d.state, ==, RADIOS_SILENCE_STATE_SOUND);
    /* A new incident starts the ladder again. */
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(70), TRUE, QUIET, QUIET), ==, RADIOS_SILENCE_EVENT_SILENCE_BEGAN);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(80), TRUE, QUIET, QUIET), ==, RADIOS_SILENCE_EVENT_STEP1);
    g_assert_cmpuint(d.incidents, ==, 2);
    /* Without a second step the ladder stops at the alarm. */
    s.step2_seconds = 0;
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(200), TRUE, QUIET, QUIET), ==, RADIOS_SILENCE_EVENT_NONE);
}

static void test_channel_rules_and_measurability(void)
{
    RadioSSilenceSettings s = alarm_settings(); RadioSSilenceDetector d; radios_silence_detector_init(&d);
    /* Both channels: one live channel is enough for sound. */
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(0), TRUE, LOUD, QUIET), ==, RADIOS_SILENCE_EVENT_NONE);
    g_assert_cmpint(d.state, ==, RADIOS_SILENCE_STATE_SOUND);
    s.both_channels = FALSE; /* any silent channel counts */
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(1), TRUE, LOUD, QUIET), ==, RADIOS_SILENCE_EVENT_SILENCE_BEGAN);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(11), TRUE, LOUD, QUIET), ==, RADIOS_SILENCE_EVENT_STEP1);
    /* In the any-channel rule, recovery needs both channels back. */
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(12), TRUE, LOUD, QUIET), ==, RADIOS_SILENCE_EVENT_NONE);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(20), TRUE, LOUD, QUIET), ==, RADIOS_SILENCE_EVENT_NONE);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(21), TRUE, LOUD, LOUD), ==, RADIOS_SILENCE_EVENT_NONE);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(24), TRUE, LOUD, LOUD), ==, RADIOS_SILENCE_EVENT_RESTORED);
    /* An unmeasurable output holds the clocks instead of alarming. */
    radios_silence_detector_init(&d); s.both_channels = TRUE;
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(0), TRUE, QUIET, QUIET), ==, RADIOS_SILENCE_EVENT_SILENCE_BEGAN);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(100), FALSE, 0, 0), ==, RADIOS_SILENCE_EVENT_NONE);
    g_assert_cmpint(d.state, ==, RADIOS_SILENCE_STATE_QUIET); g_assert_false(d.measurable);
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(101), TRUE, QUIET, QUIET), ==, RADIOS_SILENCE_EVENT_STEP1);
    /* Disabling resets; a reset keeps the incident count for the log. */
    s.enabled = FALSE;
    g_assert_cmpint(radios_silence_detector_feed(&d, &s, SEC(102), TRUE, QUIET, QUIET), ==, RADIOS_SILENCE_EVENT_NONE);
    g_assert_cmpint(d.state, ==, RADIOS_SILENCE_STATE_SOUND); g_assert_cmpuint(d.incidents, ==, 1); /* init above zeroed the earlier count */
    radios_silence_detector_reset(&d); g_assert_cmpuint(d.incidents, ==, 1); g_assert_cmpint(d.quiet_since_us, ==, 0);
    g_assert_cmpstr(radios_silence_action_label(RADIOS_SILENCE_ACTION_EMERGENCY_LIST), ==, "Acil liste");
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/silence/defaults-and-parsing", test_defaults_and_parsing);
    g_test_add_func("/silence/state-machine", test_state_machine);
    g_test_add_func("/silence/channel-rules-and-measurability", test_channel_rules_and_measurability);
    return g_test_run();
}
