#include "radios/library_analysis.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

#include "radios_test_support.h"

/* Ayarlar › Kütüphane analizi, core part: the settings file
 * (config/library-analysis.ini), the parallel runner both library scans use
 * and the shared progress counters. The fixture is a temporary project root
 * with a non-ASCII name and an empty config directory. */
typedef struct { gchar *base, *root, *config; } Fixture;

static void setup(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GError *error = NULL;
    fixture->base = g_dir_make_tmp("radios-library-analysis-XXXXXX", &error);
    g_assert_no_error(error);
    fixture->root = g_build_filename(fixture->base, "Türkçe RadioS", NULL);
    fixture->config = g_build_filename(fixture->root, "config", NULL);
    g_assert_cmpint(g_mkdir_with_parents(fixture->config, 0700), ==, 0);
}
static void teardown(Fixture *fixture, gconstpointer data)
{
    (void)data;
    rt_remove_tree(fixture->base);
    g_free(fixture->base); g_free(fixture->root); g_free(fixture->config);
}

static void defaults_are_valid(Fixture *fixture, gconstpointer data)
{
    (void)fixture; (void)data;
    RadioSLibraryAnalysisSettings settings; radios_library_analysis_settings_init(&settings);
    GError *error = NULL;
    g_assert_true(radios_library_analysis_settings_validate(&settings, &error)); g_assert_no_error(error);
    g_assert_cmpuint(settings.workers, >=, 1); g_assert_cmpuint(settings.workers, <=, 4);
    g_assert_cmpuint(settings.workers, ==, radios_library_analysis_default_workers());
    g_assert_true(settings.auto_scan);
    g_assert_cmpuint(settings.settle_seconds, ==, 20); g_assert_cmpuint(settings.interval_minutes, ==, 10);
}
static void ranges_are_enforced(Fixture *fixture, gconstpointer data)
{
    (void)fixture; (void)data;
    const RadioSLibraryAnalysisSettings bad[] = {
        {0, TRUE, 20, 10}, {RADIOS_LIBRARY_ANALYSIS_MAX_WORKERS + 1, TRUE, 20, 10},
        {2, TRUE, 2, 10}, {2, TRUE, 601, 10}, {2, TRUE, 20, 0}, {2, TRUE, 20, 1441}};
    for (guint i = 0; i < G_N_ELEMENTS(bad); ++i) {
        GError *error = NULL;
        g_assert_false(radios_library_analysis_settings_validate(&bad[i], &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    }
    const RadioSLibraryAnalysisSettings edges[] = {
        {1, FALSE, 3, 1}, {RADIOS_LIBRARY_ANALYSIS_MAX_WORKERS, TRUE, 600, 1440}};
    for (guint i = 0; i < G_N_ELEMENTS(edges); ++i) {
        GError *error = NULL;
        g_assert_true(radios_library_analysis_settings_validate(&edges[i], &error)); g_assert_no_error(error);
    }
}
static void missing_file_gives_defaults(Fixture *fixture, gconstpointer data)
{
    (void)data;
    RadioSLibraryAnalysisSettings loaded = {99, FALSE, 99, 99}; GError *error = NULL;
    g_assert_true(radios_library_analysis_settings_load(fixture->root, &loaded, &error)); g_assert_no_error(error);
    RadioSLibraryAnalysisSettings defaults; radios_library_analysis_settings_init(&defaults);
    g_assert_cmpuint(loaded.workers, ==, defaults.workers); g_assert_true(loaded.auto_scan);
    gchar *file = g_build_filename(fixture->config, "library-analysis.ini", NULL);
    g_assert_false(g_file_test(file, G_FILE_TEST_EXISTS)); /* loading never writes */
    g_free(file);
}
static void roundtrip(Fixture *fixture, gconstpointer data)
{
    (void)data;
    RadioSLibraryAnalysisSettings settings = {7, FALSE, 45, 90}, loaded; GError *error = NULL;
    g_assert_true(radios_library_analysis_settings_save(fixture->root, &settings, &error)); g_assert_no_error(error);
    g_assert_true(radios_library_analysis_settings_load(fixture->root, &loaded, &error)); g_assert_no_error(error);
    g_assert_cmpuint(loaded.workers, ==, 7); g_assert_false(loaded.auto_scan);
    g_assert_cmpuint(loaded.settle_seconds, ==, 45); g_assert_cmpuint(loaded.interval_minutes, ==, 90);
    RadioSLibraryAnalysisSettings current; radios_library_analysis_settings_current(fixture->root, &current);
    g_assert_cmpuint(current.workers, ==, 7);
    gchar *file = g_build_filename(fixture->config, "library-analysis.ini", NULL);
    rt_assert_private_file(file);
    g_free(file);
    /* An invalid proposal is rejected and the stored file stays. */
    RadioSLibraryAnalysisSettings bad = {0, TRUE, 20, 10};
    g_assert_false(radios_library_analysis_settings_save(fixture->root, &bad, &error)); g_clear_error(&error);
    g_assert_true(radios_library_analysis_settings_load(fixture->root, &loaded, &error)); g_assert_no_error(error);
    g_assert_cmpuint(loaded.workers, ==, 7);
}
static void invalid_file_fails_load_but_not_current(Fixture *fixture, gconstpointer data)
{
    (void)data;
    gchar *file = g_build_filename(fixture->config, "library-analysis.ini", NULL);
    const gchar *bad_files[] = {
        "[analysis]\nversion=1\nworkers=40\n", "[analysis]\nversion=2\n", "[analysis]\nversion=1\nauto=maybe\n",
        "[analysis]\nversion=1\nsettle_seconds=-5\n", "not an ini file", NULL};
    for (guint i = 0; bad_files[i]; ++i) {
        GError *error = NULL; RadioSLibraryAnalysisSettings loaded;
        g_assert_true(g_file_set_contents(file, bad_files[i], -1, &error)); g_assert_no_error(error);
        g_assert_false(radios_library_analysis_settings_load(fixture->root, &loaded, &error));
        g_assert_nonnull(error); g_clear_error(&error);
        /* A scan must still start: the defaults apply. */
        RadioSLibraryAnalysisSettings current; radios_library_analysis_settings_current(fixture->root, &current);
        g_assert_cmpuint(current.workers, ==, radios_library_analysis_default_workers());
    }
    RadioSLibraryAnalysisSettings none; radios_library_analysis_settings_current(NULL, &none);
    g_assert_cmpuint(none.workers, ==, radios_library_analysis_default_workers());
    g_free(file);
}

typedef struct {
    gint ran[2000];
    gint running, peak;
    gint stop_after;
    GCancellable *cancel;
    guint sleep_us;
} Run;
static void count_task(guint index, gpointer value)
{
    Run *run = value;
    gint now = g_atomic_int_add(&run->running, 1) + 1, peak = g_atomic_int_get(&run->peak);
    while (now > peak && !g_atomic_int_compare_and_exchange(&run->peak, peak, now)) peak = g_atomic_int_get(&run->peak);
    if (run->sleep_us) g_usleep(run->sleep_us);
    g_atomic_int_inc(&run->ran[index]);
    if (run->stop_after && g_atomic_int_get(&run->ran[index]) && index == (guint)run->stop_after) g_cancellable_cancel(run->cancel);
    g_atomic_int_add(&run->running, -1);
}
static void runner_runs_each_index_once(Fixture *fixture, gconstpointer data)
{
    (void)fixture; (void)data;
    static const guint workers[] = {1, 2, 4, 16, 99};
    for (guint w = 0; w < G_N_ELEMENTS(workers); ++w) {
        Run *run = g_new0(Run, 1);
        radios_library_analysis_run(1500, workers[w], NULL, count_task, run);
        for (guint i = 0; i < 1500; ++i) g_assert_cmpint(run->ran[i], ==, 1);
        for (guint i = 1500; i < 2000; ++i) g_assert_cmpint(run->ran[i], ==, 0);
        g_free(run);
    }
    Run *empty = g_new0(Run, 1);
    radios_library_analysis_run(0, 4, NULL, count_task, empty);
    radios_library_analysis_run(5, 4, NULL, NULL, empty);
    g_assert_cmpint(empty->peak, ==, 0); g_free(empty);
}
static void runner_is_really_parallel(Fixture *fixture, gconstpointer data)
{
    (void)fixture; (void)data;
    Run *run = g_new0(Run, 1); run->sleep_us = 20000;
    gint64 started = g_get_monotonic_time();
    radios_library_analysis_run(80, 4, NULL, count_task, run);
    gint64 elapsed = g_get_monotonic_time() - started;
    g_assert_cmpint(run->peak, >=, 2); g_assert_cmpint(run->peak, <=, 4);
    /* 80 tasks of 20 ms: ~1.6 s serially, ~0.4 s with four workers. */
    g_assert_cmpint(elapsed, <, 1200 * 1000);
    g_free(run);
    run = g_new0(Run, 1); run->sleep_us = 5000;
    radios_library_analysis_run(10, 1, NULL, count_task, run);
    g_assert_cmpint(run->peak, ==, 1); /* one worker never overlaps */
    g_free(run);
}
static void runner_stops_on_cancel(Fixture *fixture, gconstpointer data)
{
    (void)fixture; (void)data;
    Run *run = g_new0(Run, 1); run->cancel = g_cancellable_new(); run->stop_after = 100; run->sleep_us = 1000;
    radios_library_analysis_run(1500, 4, run->cancel, count_task, run);
    guint done = 0;
    for (guint i = 0; i < 1500; ++i) { g_assert_cmpint(run->ran[i], <=, 1); done += (guint)run->ran[i]; }
    g_assert_cmpuint(done, >=, 100); g_assert_cmpuint(done, <, 1500);
    g_object_unref(run->cancel); g_free(run);
    /* An already cancelled run starts nothing. */
    run = g_new0(Run, 1); run->cancel = g_cancellable_new(); g_cancellable_cancel(run->cancel);
    radios_library_analysis_run(50, 4, run->cancel, count_task, run);
    g_assert_cmpint(run->peak, ==, 0);
    g_object_unref(run->cancel); g_free(run);
}
static void activity_counts_scans(Fixture *fixture, gconstpointer data)
{
    (void)fixture; (void)data;
    guint done, total, workers;
    g_assert_false(radios_library_analysis_activity(&done, &total, &workers));
    radios_library_analysis_activity_begin(3);
    radios_library_analysis_activity_add_total(10);
    for (guint i = 0; i < 4; ++i) radios_library_analysis_activity_step();
    g_assert_true(radios_library_analysis_activity(&done, &total, &workers));
    g_assert_cmpuint(done, ==, 4); g_assert_cmpuint(total, ==, 10); g_assert_cmpuint(workers, ==, 3);
    /* A second scan joins the same counters. */
    radios_library_analysis_activity_begin(2);
    radios_library_analysis_activity_add_total(5);
    g_assert_true(radios_library_analysis_activity(&done, &total, NULL));
    g_assert_cmpuint(done, ==, 4); g_assert_cmpuint(total, ==, 15);
    radios_library_analysis_activity_end();
    g_assert_true(radios_library_analysis_activity(NULL, NULL, NULL));
    radios_library_analysis_activity_end();
    g_assert_false(radios_library_analysis_activity(&done, &total, NULL));
    /* The next first scan starts from zero. */
    radios_library_analysis_activity_begin(1);
    g_assert_true(radios_library_analysis_activity(&done, &total, NULL));
    g_assert_cmpuint(done, ==, 0); g_assert_cmpuint(total, ==, 0);
    radios_library_analysis_activity_end();
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/library-analysis/defaults", Fixture, NULL, setup, defaults_are_valid, teardown);
    g_test_add("/library-analysis/ranges", Fixture, NULL, setup, ranges_are_enforced, teardown);
    g_test_add("/library-analysis/missing-file", Fixture, NULL, setup, missing_file_gives_defaults, teardown);
    g_test_add("/library-analysis/roundtrip", Fixture, NULL, setup, roundtrip, teardown);
    g_test_add("/library-analysis/invalid-file", Fixture, NULL, setup, invalid_file_fails_load_but_not_current, teardown);
    g_test_add("/library-analysis/runner-once", Fixture, NULL, setup, runner_runs_each_index_once, teardown);
    g_test_add("/library-analysis/runner-parallel", Fixture, NULL, setup, runner_is_really_parallel, teardown);
    g_test_add("/library-analysis/runner-cancel", Fixture, NULL, setup, runner_stops_on_cancel, teardown);
    g_test_add("/library-analysis/activity", Fixture, NULL, setup, activity_counts_scans, teardown);
    return g_test_run();
}
