#include "radios/preferences_storage.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

#include "radios_test_support.h"

typedef struct { gchar *root, *config, *file; } Fixture;

static void setup(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GError *error = NULL;
    fixture->root = g_dir_make_tmp("radios-audio-panel-Türkçe-XXXXXX", &error);
    g_assert_no_error(error);
    fixture->config = g_build_filename(fixture->root, "config", NULL);
    fixture->file = g_build_filename(fixture->config, "audio-panel.ini", NULL);
    g_assert_cmpint(g_mkdir(fixture->config, 0700), ==, 0);
}

static void teardown(Fixture *fixture, gconstpointer data)
{
    (void)data;
    rt_remove_tree(fixture->root);
    g_free(fixture->root); g_free(fixture->config); g_free(fixture->file);
}

static void same(const RadioSPreferencesAudio *value, const RadioSPreferencesAudio *wanted)
{
    g_assert_cmpuint(value->sample_rate, ==, wanted->sample_rate);
    g_assert_cmpuint(value->buffer_frames, ==, wanted->buffer_frames);
    g_assert_cmpuint(value->click_sound, ==, wanted->click_sound);
    g_assert_cmpuint(value->click_volume, ==, wanted->click_volume);
}

static void missing(Fixture *fixture, gconstpointer data)
{
    (void)data;
    RadioSPreferencesAudio wanted, value = {48000, 4096, 2, 2};
    radios_preferences_audio_default(&wanted);
    same(&wanted, &(RadioSPreferencesAudio){44100, 256, 0, 1});
    GError *error = NULL;
    g_assert_true(radios_preferences_audio_load(fixture->root, &value, &error));
    g_assert_no_error(error); same(&value, &wanted);
    g_assert_false(g_file_test(fixture->file, G_FILE_TEST_EXISTS));
}

static void roundtrip(Fixture *fixture, gconstpointer data)
{
    (void)data;
    gchar *sentinel = g_build_filename(fixture->config, "ui-preferences.ini", NULL);
    g_assert_true(g_file_set_contents(sentinel, "existing UI preferences\n", -1, NULL));
    GError *error = NULL;
    RtFileId previous = {0};
    gboolean have_previous = FALSE;
    for (guint rate = 0; rate < 2; ++rate)
        for (guint frames = 64; frames <= 4096; frames *= 2)
            for (guint sound = 0; sound < 3; ++sound)
                for (guint volume = 0; volume < 3; ++volume) {
                    RadioSPreferencesAudio wanted = {rate ? 48000 : 44100, frames, sound, volume}, value = {0};
                    g_assert_true(radios_preferences_audio_save(fixture->root, &wanted, &error));
                    g_assert_no_error(error);
                    rt_assert_private_file(fixture->file);
                    /* Replaced atomically: every save is a new file, never a rewrite in place. */
                    RtFileId current = rt_file_id(fixture->file);
                    if (have_previous)
                        g_assert_false(previous.volume == current.volume &&
                                       memcmp(previous.id, current.id, sizeof current.id) == 0);
                    previous = current; have_previous = TRUE;
                    g_assert_true(radios_preferences_audio_load(fixture->root, &value, &error));
                    g_assert_no_error(error); same(&value, &wanted);
                }
    gchar *contents = NULL;
    g_assert_true(g_file_get_contents(sentinel, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, "existing UI preferences\n");
    g_free(contents); g_free(sentinel);
    GDir *directory = g_dir_open(fixture->config, 0, NULL);
    g_assert_nonnull(directory);
    guint count = 0;
    const gchar *name;
    while ((name = g_dir_read_name(directory))) {
        g_assert_true(g_str_equal(name, "audio-panel.ini") || g_str_equal(name, "ui-preferences.ini"));
        ++count;
    }
    g_assert_cmpuint(count, ==, 2); g_dir_close(directory);
}

static void invalid_values(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const gchar *sentinel = "panel sentinel\n";
    g_assert_true(g_file_set_contents(fixture->file, sentinel, -1, NULL));
    const RadioSPreferencesAudio invalid[] = {
        {0, 256, 0, 1}, {96000, 256, 0, 1}, {44100, 0, 0, 1},
        {44100, 32, 0, 1}, {44100, 255, 0, 1}, {44100, 8192, 0, 1},
        {44100, G_MAXUINT, 0, 1}, {44100, 256, 3, 1}, {44100, 256, 0, 3}
    };
    for (guint i = 0; i < G_N_ELEMENTS(invalid); ++i) {
        GError *error = NULL;
        g_assert_false(radios_preferences_audio_validate(&invalid[i], &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
        g_assert_false(radios_preferences_audio_save(fixture->root, &invalid[i], &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
        g_assert_false(radios_preferences_audio_validate(&invalid[i], NULL));
        gchar *contents = NULL;
        g_assert_true(g_file_get_contents(fixture->file, &contents, NULL, NULL));
        g_assert_cmpstr(contents, ==, sentinel); g_free(contents);
    }
}

static void malformed_files(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const gchar *files[] = {
        "[audio]\nversion=2\nsample_rate=44100\nbuffer_frames=256\nclick_sound=0\nclick_volume=1\n",
        "[audio]\nsample_rate=44100\nbuffer_frames=256\nclick_sound=0\nclick_volume=1\n",
        "[audio]\nversion=1\nbuffer_frames=256\nclick_sound=0\nclick_volume=1\n",
        "[audio]\nversion=1\nsample_rate=96000\nbuffer_frames=256\nclick_sound=0\nclick_volume=1\n",
        "[audio]\nversion=1\nsample_rate=44100\nbuffer_frames=256.0\nclick_sound=0\nclick_volume=1\n",
        "[audio]\nversion=1\nsample_rate=44100\nbuffer_frames=-256\nclick_sound=0\nclick_volume=1\n",
        "[audio]\nversion=1\nsample_rate=44100\nbuffer_frames=256\nclick_sound=3\nclick_volume=1\n",
        "[audio]\nversion=1\nsample_rate=44100\nbuffer_frames=256\nclick_sound=0\nclick_volume=18446744073709551615\n",
        "[audio]\nversion=1\nsample_rate=44100\nbuffer_frames=256\nclick_sound=no\nclick_volume=1\n",
        "[broken\nversion=1\n"
    };
    const RadioSPreferencesAudio original = {48000, 4096, 2, 2};
    for (guint i = 0; i < G_N_ELEMENTS(files); ++i) {
        g_assert_true(g_file_set_contents(fixture->file, files[i], -1, NULL));
        RadioSPreferencesAudio value = original;
        GError *error = NULL;
        g_assert_false(radios_preferences_audio_load(fixture->root, &value, &error));
        g_assert_nonnull(error); g_clear_error(&error); same(&value, &original);
        g_assert_false(radios_preferences_audio_load(fixture->root, &value, NULL));
        same(&value, &original);
    }
}

static void bounded_utf8(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GString *contents = g_string_new("[audio]\nversion=1\nsample_rate=44100\nbuffer_frames=256\nclick_sound=0\nclick_volume=1\n#");
    while (contents->len < 8192) g_string_append_c(contents, 'x');
    RadioSPreferencesAudio value = {0};
    GError *error = NULL;
    g_assert_true(g_file_set_contents(fixture->file, contents->str, contents->len, NULL));
    g_assert_true(radios_preferences_audio_load(fixture->root, &value, &error));
    g_assert_no_error(error);
    g_string_append_c(contents, 'x');
    g_assert_true(g_file_set_contents(fixture->file, contents->str, contents->len, NULL));
    g_assert_false(radios_preferences_audio_load(fixture->root, &value, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    g_string_free(contents, TRUE);
    const gchar nul[] = "[audio]\nversion=1\0sample_rate=44100\n";
    g_assert_true(g_file_set_contents(fixture->file, nul, sizeof nul - 1, NULL));
    g_assert_false(radios_preferences_audio_load(fixture->root, &value, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    const gchar invalid[] = {'[', 'a', 'u', 'd', 'i', 'o', ']', '\n', (gchar)0xff};
    g_assert_true(g_file_set_contents(fixture->file, invalid, sizeof invalid, NULL));
    g_assert_false(radios_preferences_audio_load(fixture->root, &value, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
}

static void file_links_and_special(Fixture *fixture, gconstpointer data)
{
    (void)data;
    RadioSPreferencesAudio value;
    radios_preferences_audio_default(&value);
    GError *error = NULL;
    /* Not a regular file: a directory carrying the file's name (Windows has
     * no FIFOs; this was the mkfifo() case). Needs no link privilege. */
    g_assert_cmpint(g_mkdir(fixture->file, 0700), ==, 0);
    g_assert_false(radios_preferences_audio_load(fixture->root, &value, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    g_assert_false(radios_preferences_audio_save(fixture->root, &value, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    g_assert_true(g_file_test(fixture->file, G_FILE_TEST_IS_DIR));
    g_assert_cmpint(g_rmdir(fixture->file), ==, 0);
    gchar *external = g_build_filename(fixture->root, "external.ini", NULL);
    g_assert_true(g_file_set_contents(external, "external sentinel\n", -1, NULL));
    if (!rt_symlink(external, fixture->file, FALSE, NULL)) {
        g_free(external);
        g_test_skip("symbolic links unavailable");
        return;
    }
    g_assert_false(radios_preferences_audio_load(fixture->root, &value, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    g_assert_false(radios_preferences_audio_save(fixture->root, &value, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    gchar *contents = NULL;
    g_assert_true(g_file_get_contents(external, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, "external sentinel\n"); g_free(contents);
    g_assert_true(rt_is_link(fixture->file));
    g_free(external);
}

static void config_escape(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GError *error = NULL;
    gchar *outside = g_dir_make_tmp("radios-panel-outside-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_cmpint(g_rmdir(fixture->config), ==, 0);
    /* Directory link (junction when symbolic links need a privilege). */
    g_assert_true(rt_symlink(outside, fixture->config, TRUE, &error)); g_assert_no_error(error);
    RadioSPreferencesAudio value;
    radios_preferences_audio_default(&value);
    g_assert_false(radios_preferences_audio_load(fixture->root, &value, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    g_assert_false(radios_preferences_audio_save(fixture->root, &value, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA); g_clear_error(&error);
    GDir *directory = g_dir_open(outside, 0, NULL);
    g_assert_nonnull(directory); g_assert_null(g_dir_read_name(directory)); g_dir_close(directory);
    g_assert_cmpint(g_remove(fixture->config), ==, 0);
    g_assert_cmpint(g_mkdir(fixture->config, 0700), ==, 0);
    g_assert_cmpint(g_rmdir(outside), ==, 0); g_free(outside);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/preferences-audio/missing-defaults", Fixture, NULL, setup, missing, teardown);
    g_test_add("/preferences-audio/atomic-roundtrip", Fixture, NULL, setup, roundtrip, teardown);
    g_test_add("/preferences-audio/reject-invalid-values", Fixture, NULL, setup, invalid_values, teardown);
    g_test_add("/preferences-audio/reject-malformed-file", Fixture, NULL, setup, malformed_files, teardown);
    g_test_add("/preferences-audio/bounded-utf8", Fixture, NULL, setup, bounded_utf8, teardown);
    g_test_add("/preferences-audio/file-links-fifo", Fixture, NULL, setup, file_links_and_special, teardown);
    g_test_add("/preferences-audio/config-escape", Fixture, NULL, setup, config_escape, teardown);
    return g_test_run();
}
