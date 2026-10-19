/* Dead-air guard against a fake MAIN level: the ladder, the notifications and
 * the status line, without a data service or an audio device. */
#include "radios/silence_guard.h"
#include "radios/silence_detector.h"
#include <string.h>

typedef struct { gdouble left, right; gboolean measurable; } Levels;
static gboolean fake_levels(gpointer user, gdouble *left, gdouble *right)
{ Levels *l = user; *left = l->left; *right = l->right; return l->measurable; }

static GString *log_text;
static void notify(gpointer data, const gchar *message) { (void)data; g_string_append(log_text, message); g_string_append_c(log_text, '\n'); }

static void pump_for(gdouble seconds)
{
    gint64 end = g_get_monotonic_time() + (gint64)(seconds * G_USEC_PER_SEC);
    while (g_get_monotonic_time() < end) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(5000); }
}

static JsonObject *preparation(const gchar *step1, const gchar *step2, const gchar *step2_seconds, gboolean enabled)
{
    JsonObject *object = json_object_new();
    json_object_set_string_member(object, "silence/Sessizlikte otomatik devralma", enabled ? "1" : "0");
    json_object_set_string_member(object, "silence/Eşik · dB", "-50");
    json_object_set_string_member(object, "silence/Süre · sn", "1");
    json_object_set_string_member(object, "silence/1 · Süre dolunca", step1);
    json_object_set_string_member(object, "silence/2 · Sürerse · sn", step2_seconds);
    json_object_set_string_member(object, "silence/Sonra", step2);
    json_object_set_string_member(object, "silence/Ses dönünce eşik · dB", "-40");
    json_object_set_string_member(object, "silence/Dönüş süresi · sn", "1");
    json_object_set_string_member(object, "takeover/Devralma davranışı", "Sıradaki olaya geç");
    return object;
}

static gchar *status_line(RadioSViewContext *ctx)
{ gchar *line = NULL; g_assert_true(radios_silence_guard_status(ctx, &line, NULL, NULL)); return line; }

static void test_ladder(void)
{
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(window);
    RadioSViewContext ctx = {.parent = GTK_WINDOW(window), .notify = notify};
    log_text = g_string_new(NULL);
    Levels levels = {0.1, 0.1, TRUE};
    g_assert_false(radios_silence_guard_status(&ctx, NULL, NULL, NULL));
    radios_silence_guard_bind(&ctx, window);
    radios_silence_guard_bind(&ctx, window); /* idempotent */
    radios_silence_guard_set_level_source(&ctx, fake_levels, &levels);
    /* Alarm only, then the emergency list without a saved document. */
    JsonObject *object = preparation("Yalnız alarm", "Acil liste", "1", TRUE); GError *error = NULL;
    g_assert_true(radios_silence_guard_apply_preparation(&ctx, object, NULL, 0, &error)); g_assert_no_error(error); json_object_unref(object);
    gchar *line = status_line(&ctx); g_assert_nonnull(strstr(line, "açık")); g_assert_nonnull(strstr(line, "Yalnız alarm")); g_free(line);
    pump_for(0.4);
    g_assert_cmpstr(log_text->str, ==, "");
    levels.left = levels.right = 0.0001; /* -80 dB */
    pump_for(0.5);
    line = status_line(&ctx); g_assert_nonnull(strstr(line, "süre sayılıyor")); g_free(line);
    g_assert_cmpstr(log_text->str, ==, "");
    pump_for(1.0);
    g_assert_nonnull(strstr(log_text->str, "1. basamak")); g_assert_nonnull(strstr(log_text->str, "Yalnız alarm"));
    g_assert_null(strstr(log_text->str, "2. basamak"));
    pump_for(1.2);
    g_assert_nonnull(strstr(log_text->str, "2. basamak")); g_assert_nonnull(strstr(log_text->str, "Acil liste"));
    g_assert_nonnull(strstr(log_text->str, "kaydedilmemiş"));
    gsize after_steps = log_text->len;
    pump_for(0.6);
    g_assert_cmpuint(log_text->len, ==, after_steps); /* no repeats while silent */
    /* Recovery needs one second above the restore level. */
    levels.left = 0.05; levels.right = 0.0001; /* one live channel is enough under "İki kanal da" */
    pump_for(0.5); g_assert_null(strstr(log_text->str, "geri geldi"));
    pump_for(1.0); g_assert_nonnull(strstr(log_text->str, "geri geldi"));
    /* Next deck without an audio engine reports the reason. */
    g_string_truncate(log_text, 0);
    object = preparation("İkinci deck · yedek oynatıcı", "Ek işlem yok", "0", TRUE);
    g_assert_true(radios_silence_guard_apply_preparation(&ctx, object, NULL, 0, &error)); g_assert_no_error(error); json_object_unref(object);
    levels.left = levels.right = 0.0001;
    pump_for(1.6);
    g_assert_nonnull(strstr(log_text->str, "İkinci deck")); g_assert_nonnull(strstr(log_text->str, "ses motoru hazır değil"));
    /* Unmeasurable output never alarms; disabled settings never alarm. */
    levels.left = levels.right = 0.1; pump_for(1.3); g_string_truncate(log_text, 0);
    levels.measurable = FALSE; levels.left = levels.right = 0.0001;
    pump_for(1.6); g_assert_cmpstr(log_text->str, ==, "");
    line = status_line(&ctx); g_free(line);
    levels.measurable = TRUE;
    object = preparation("Yalnız alarm", "Ek işlem yok", "0", FALSE);
    g_assert_true(radios_silence_guard_apply_preparation(&ctx, object, NULL, 0, &error)); json_object_unref(object);
    pump_for(1.6); g_assert_cmpstr(log_text->str, ==, "");
    line = status_line(&ctx); g_assert_nonnull(strstr(line, "kapalı")); g_free(line);
    /* Invalid fields disable the guard and say why. */
    object = preparation("Yalnız alarm", "Ek işlem yok", "0", TRUE);
    json_object_set_string_member(object, "silence/Eşik · dB", "çok");
    g_assert_false(radios_silence_guard_apply_preparation(&ctx, object, NULL, 0, &error)); g_assert_nonnull(error); g_clear_error(&error); json_object_unref(object);
    g_assert_nonnull(strstr(log_text->str, "kapalı"));
    gtk_widget_destroy(window); g_object_unref(window);
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_string_free(log_text, TRUE);
}

int main(int argc, char **argv)
{
    if (!gtk_init_check(&argc, &argv)) return 77;
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/silence-guard/ladder-recovery-reasons", test_ladder);
    return g_test_run();
}
