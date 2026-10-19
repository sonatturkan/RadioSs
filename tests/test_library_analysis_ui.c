/* Ayarlar › Kütüphane analizi, native page: every change is stored in
 * config/library-analysis.ini at once, a stored file is shown again on reopen,
 * a broken file shows its error and is replaced by the next change, and the
 * live progress of the library scans appears. Needs a display, not the data
 * service: the page is built against a temporary project root. */
#include "radios/library_analysis.h"
#include "radios/library_analysis_ui.h"
#include "radios/media_ui.h"
#include "radios/data.h"
#include <glib/gstdio.h>
#include <string.h>

static RadioSConfig config;
static gchar *base;
typedef struct { RadioSViewContext ctx; GtkWidget *window, *page; } Harness;

static void remove_tree(const gchar *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_SYMLINK) || !g_file_test(path, G_FILE_TEST_IS_DIR)) { g_remove(path); return; }
    GDir *directory = g_dir_open(path, 0, NULL); const gchar *name;
    while (directory && (name = g_dir_read_name(directory))) {
        gchar *child = g_build_filename(path, name, NULL); remove_tree(child); g_free(child);
    }
    if (directory) g_dir_close(directory);
    g_rmdir(path);
}
static void pump(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
static void pump_for(gint64 milliseconds)
{
    gint64 end = g_get_monotonic_time() + milliseconds * 1000;
    while (g_get_monotonic_time() < end) pump();
}
static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *result = NULL;
    for (GList *item = children; item && !result; item = item->next) result = find(item->data, name);
    g_list_free(children); return result;
}
static GtkWidget *required(Harness *h, const gchar *name)
{ GtkWidget *widget = find(h->window, name); g_assert_nonnull(widget); return widget; }
static gchar *ini_path(void) { return g_build_filename(config.project_root, "config", "library-analysis.ini", NULL); }
static Harness *open_page(void)
{
    Harness *h = g_new0(Harness, 1); h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); g_object_ref_sink(h->window);
    h->ctx = (RadioSViewContext){ .config = &config, .parent = GTK_WINDOW(h->window) };
    h->page = radios_library_analysis_ui_page_new(&h->ctx);
    gtk_container_add(GTK_CONTAINER(h->window), h->page); gtk_widget_show_all(h->window);
    pump(); return h;
}
static void close_page(Harness *h)
{
    gtk_widget_destroy(h->window);
    while (g_main_context_iteration(NULL, FALSE)) {}
    g_object_unref(h->window); g_free(h);
}
static void reset_file(void)
{
    gchar *path = ini_path(); g_unlink(path); g_free(path);
}
static RadioSLibraryAnalysisSettings stored(void)
{
    RadioSLibraryAnalysisSettings settings; GError *error = NULL;
    g_assert_true(radios_library_analysis_settings_load(config.project_root, &settings, &error)); g_assert_no_error(error);
    return settings;
}

static void test_defaults_and_persistence(void)
{
    reset_file();
    Harness *h = open_page();
    GtkWidget *workers = required(h, "preferences-analysis-workers"), *automatic = required(h, "preferences-analysis-auto");
    GtkWidget *settle = required(h, "preferences-analysis-settle"), *interval = required(h, "preferences-analysis-interval");
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(workers)), ==, (gint)radios_library_analysis_default_workers());
    g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(automatic)));
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(settle)), ==, 20);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(interval)), ==, 10);
    gchar *path = ini_path(); g_assert_false(g_file_test(path, G_FILE_TEST_EXISTS)); /* opening writes nothing */
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(workers), 6);
    g_assert_true(g_file_test(path, G_FILE_TEST_EXISTS));
    g_assert_cmpuint(stored().workers, ==, 6);
    g_assert_nonnull(strstr(gtk_label_get_text(GTK_LABEL(required(h, "preferences-analysis-settings-status"))), "Kaydedildi"));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(automatic), FALSE);
    g_assert_false(stored().auto_scan);
    g_assert_false(gtk_widget_get_sensitive(settle)); g_assert_false(gtk_widget_get_sensitive(interval));
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(automatic), TRUE);
    g_assert_true(gtk_widget_get_sensitive(settle)); g_assert_true(gtk_widget_get_sensitive(interval));
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(settle), 45); gtk_spin_button_set_value(GTK_SPIN_BUTTON(interval), 90);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(automatic), FALSE);
    RadioSLibraryAnalysisSettings now = stored();
    g_assert_cmpuint(now.workers, ==, 6); g_assert_false(now.auto_scan);
    g_assert_cmpuint(now.settle_seconds, ==, 45); g_assert_cmpuint(now.interval_minutes, ==, 90);
    close_page(h);
    /* Reopening shows what was stored; a scan reads the same file. */
    h = open_page();
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(required(h, "preferences-analysis-workers"))), ==, 6);
    g_assert_false(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(required(h, "preferences-analysis-auto"))));
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(required(h, "preferences-analysis-settle"))), ==, 45);
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(required(h, "preferences-analysis-interval"))), ==, 90);
    g_assert_false(gtk_widget_get_sensitive(required(h, "preferences-analysis-settle")));
    RadioSLibraryAnalysisSettings current; radios_library_analysis_settings_current(config.project_root, &current);
    g_assert_cmpuint(current.workers, ==, 6);
    close_page(h); g_free(path); reset_file();
}
static void test_broken_file_is_shown_and_replaced(void)
{
    gchar *path = ini_path();
    g_assert_true(g_file_set_contents(path, "[analysis]\nversion=1\nworkers=99\n", -1, NULL));
    Harness *h = open_page();
    const gchar *status = gtk_label_get_text(GTK_LABEL(required(h, "preferences-analysis-settings-status")));
    g_assert_nonnull(strstr(status, "library-analysis.ini"));
    g_assert_cmpint(gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(required(h, "preferences-analysis-workers"))), ==, (gint)radios_library_analysis_default_workers());
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(required(h, "preferences-analysis-workers")), 3);
    g_assert_cmpuint(stored().workers, ==, 3); /* the next change repairs the file */
    close_page(h); g_free(path); reset_file();
}
static void test_run_needs_connection_and_progress_shows(void)
{
    reset_file();
    Harness *h = open_page();
    GtkWidget *run = required(h, "preferences-analysis-run"), *stop = required(h, "preferences-analysis-stop");
    GtkWidget *progress = required(h, "preferences-analysis-progress");
    /* No data service and no bound upkeep: nothing can start. */
    g_assert_false(gtk_widget_get_sensitive(run));
    g_assert_false(gtk_widget_get_visible(stop));
    g_assert_false(radios_media_rhythm_library_run_now(&h->ctx));
    gboolean running = TRUE; gchar *last = radios_media_rhythm_library_status(&h->ctx, &running);
    g_assert_false(running); g_assert_cmpstr(last, ==, ""); g_free(last);
    g_assert_cmpstr(gtk_progress_bar_get_text(GTK_PROGRESS_BAR(progress)), ==, "Hazır");
    /* Any library scan shows its progress. */
    radios_library_analysis_activity_begin(4);
    radios_library_analysis_activity_add_total(10);
    for (guint i = 0; i < 4; ++i) radios_library_analysis_activity_step();
    pump_for(1300);
    const gchar *text = gtk_progress_bar_get_text(GTK_PROGRESS_BAR(progress));
    g_assert_nonnull(strstr(text, "4 / 10")); g_assert_nonnull(strstr(text, "4 paralel"));
    g_assert_cmpfloat(gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(progress)), >, 0.39);
    g_assert_cmpfloat(gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(progress)), <, 0.41);
    radios_library_analysis_activity_end();
    pump_for(1300);
    g_assert_cmpstr(gtk_progress_bar_get_text(GTK_PROGRESS_BAR(progress)), ==, "Hazır");
    close_page(h);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); gtk_init(&argc, &argv);
    GError *error = NULL;
    base = g_dir_make_tmp("radios-analysis-ui-XXXXXX", &error); g_assert_no_error(error);
    config.project_root = g_build_filename(base, "Türkçe RadioS", NULL);
    gchar *config_dir = g_build_filename(config.project_root, "config", NULL);
    g_assert_cmpint(g_mkdir_with_parents(config_dir, 0700), ==, 0); g_free(config_dir);
    g_test_add_func("/library-analysis-ui/defaults-and-persistence", test_defaults_and_persistence);
    g_test_add_func("/library-analysis-ui/broken-file", test_broken_file_is_shown_and_replaced);
    g_test_add_func("/library-analysis-ui/run-and-progress", test_run_needs_connection_and_progress_shows);
    gint result = g_test_run();
    remove_tree(base);
    g_free(config.project_root); g_free(base); return result;
}
