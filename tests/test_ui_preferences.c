#include "radios/ui_preferences.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>

#include "radios_test_support.h"

typedef struct {
    gchar *temporary, *root, *config, *file;
} Fixture;

static void
setup(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GError *error = NULL;
    fixture->temporary = g_dir_make_tmp("radios-ui-preferences-XXXXXX", &error);
    g_assert_no_error(error);
    fixture->root = g_build_filename(fixture->temporary, "Türkçe kaynak kodlar RadioS", NULL);
    fixture->config = g_build_filename(fixture->root, "config", NULL);
    fixture->file = g_build_filename(fixture->config, "ui-preferences.ini", NULL);
    g_assert_cmpint(g_mkdir_with_parents(fixture->config, 0700), ==, 0);
}

static void
teardown(Fixture *fixture, gconstpointer data)
{
    (void)data;
    rt_remove_tree(fixture->temporary);
    g_free(fixture->temporary);
    g_free(fixture->root);
    g_free(fixture->config);
    g_free(fixture->file);
}

static void
write_file(const gchar *path, const gchar *contents)
{
    GError *error = NULL;
    g_assert_true(g_file_set_contents(path, contents, -1, &error));
    g_assert_no_error(error);
}

static void
assert_preferences(const RadioSUIPreferences *value, guint text, guint default_profile, guint last,
                   guint header_hidden_fields, guint scale, guint mixer_scale, guint effector_scale,
                   guint mixer_height, guint effector_height)
{
    g_assert_cmpuint(value->text_size, ==, text);
    g_assert_cmpuint(value->default_profile, ==, default_profile);
    g_assert_cmpuint(value->last_profile, ==, last);
    g_assert_cmpuint(value->header_hidden_fields, ==, header_hidden_fields);
    g_assert_cmpuint(value->content_scale_percent, ==, scale);
    g_assert_cmpuint(value->mixer_scale_percent, ==, mixer_scale);
    g_assert_cmpuint(value->effector_scale_percent, ==, effector_scale);
    g_assert_cmpuint(value->mixer_height_percent, ==, mixer_height);
    g_assert_cmpuint(value->effector_height_percent, ==, effector_height);
}

static void
missing(Fixture *fixture, gconstpointer data)
{
    (void)data;
    RadioSUIPreferences value = {2, 7, 6, 5, 100, 83, 117, 83, 117};
    GError *error = NULL;
    g_assert_true(radios_ui_preferences_load(fixture->root, &value, &error));
    g_assert_no_error(error);
    assert_preferences(&value, 1, 0, 0, 0, 100, 100, 100, 100, 100);
    g_assert_false(g_file_test(fixture->file, G_FILE_TEST_EXISTS));
}

static void
roundtrip(Fixture *fixture, gconstpointer data)
{
    (void)data;
    RadioSUIPreferences original = {2, 8, 7, 5, 100, 100, 100, 100, 100}, value = {0};
    GError *error = NULL;
    g_assert_true(radios_ui_preferences_save(fixture->root, &original, &error));
    g_assert_no_error(error);
    rt_assert_private_file(fixture->file);
    RtFileId first = rt_file_id(fixture->file);
    g_assert_true(radios_ui_preferences_load(fixture->root, &value, &error));
    g_assert_no_error(error);
    assert_preferences(&value, 2, 8, 7, 5, 100, 100, 100, 100, 100);
    /* Saving again must produce an owner-only file even when the previous
     * one had become readable by others. */
    rt_make_world_readable(fixture->file);
    original = (RadioSUIPreferences){.content_scale_percent = 100, .mixer_scale_percent = 100, .effector_scale_percent = 100,
        .mixer_height_percent = 100, .effector_height_percent = 100};
    g_assert_true(radios_ui_preferences_save(fixture->root, &original, &error));
    g_assert_no_error(error);
    rt_assert_private_file(fixture->file);
    /* Replaced atomically (a new file), not rewritten in place. */
    RtFileId second = rt_file_id(fixture->file);
    g_assert_false(first.volume == second.volume && memcmp(first.id, second.id, sizeof first.id) == 0);
    g_assert_true(radios_ui_preferences_load(fixture->root, &value, &error));
    assert_preferences(&value, 0, 0, 0, 0, 100, 100, 100, 100, 100);
    GDir *directory = g_dir_open(fixture->config, 0, NULL);
    g_assert_nonnull(directory);
    g_assert_cmpstr(g_dir_read_name(directory), ==, "ui-preferences.ini");
    g_assert_null(g_dir_read_name(directory));
    g_dir_close(directory);
}

static void
header_fields(Fixture *fixture, gconstpointer data)
{
    (void)data;
    g_assert_cmpuint(RADIOS_HEADER_HIDE_PROGRAMME, ==, 1);
    g_assert_cmpuint(RADIOS_HEADER_HIDE_TRACK, ==, 2);
    g_assert_cmpuint(RADIOS_HEADER_HIDE_REMAINING, ==, 4);
    for (guint hidden = 0; hidden <= 7; ++hidden) {
        RadioSUIPreferences original = {2, 8, 7, hidden, 100, 100, 100, 100, 100}, value = {0};
        GError *error = NULL;
        g_assert_true(radios_ui_preferences_save(fixture->root, &original, &error));
        g_assert_no_error(error);
        g_assert_true(radios_ui_preferences_load(fixture->root, &value, &error));
        g_assert_no_error(error);
        assert_preferences(&value, 2, 8, 7, hidden, 100, 100, 100, 100, 100);
    }
}

static void
legacy_header_defaults(Fixture *fixture, gconstpointer data)
{
    (void)data;
    write_file(fixture->file, "[ui]\nversion=1\ntext_size=2\ndefault_profile=8\nlast_profile=7\n");
    RadioSUIPreferences value = {0, 0, 0, 7, 100, 100, 100, 100, 100};
    GError *error = NULL;
    g_assert_true(radios_ui_preferences_load(fixture->root, &value, &error));
    g_assert_no_error(error);
    assert_preferences(&value, 2, 8, 7, 0, 100, 100, 100, 100, 100);
    /* Loading the old format does not rewrite the file. Saving it adopts the
     * optional key while retaining its existing version and preferences. */
    gchar *contents = NULL;
    g_assert_true(g_file_get_contents(fixture->file, &contents, NULL, NULL));
    g_assert_null(strstr(contents, "header_hidden_fields"));
    g_free(contents);
    g_assert_true(radios_ui_preferences_save(fixture->root, &value, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_get_contents(fixture->file, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "version=1\n"));
    g_assert_nonnull(strstr(contents, "header_hidden_fields=0\n"));
    g_free(contents);
}

static void
content_scale_roundtrip(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const guint scales[] = {50, 53, 75, 100, 125, 143, 150, 143, 150};
    for (guint i = 0; i < G_N_ELEMENTS(scales); ++i) {
        RadioSUIPreferences original = {2, 8, 7, 5, scales[i], 100, 100, 100, 100}, loaded = {0};
        GError *error = NULL;
        g_assert_true(radios_ui_preferences_save(fixture->root, &original, &error));
        g_assert_no_error(error);
        g_assert_true(radios_ui_preferences_load(fixture->root, &loaded, &error));
        g_assert_no_error(error);
        assert_preferences(&loaded, 2, 8, 7, 5, scales[i], 100, 100, 100, 100);
    }
}

static void
legacy_content_scale_defaults(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const gchar *legacy = "[ui]\nversion=1\ntext_size=2\ndefault_profile=8\nlast_profile=7\nheader_hidden_fields=6\n";
    write_file(fixture->file, legacy);
    RadioSUIPreferences loaded = {0};
    GError *error = NULL;
    g_assert_true(radios_ui_preferences_load(fixture->root, &loaded, &error));
    g_assert_no_error(error);
    assert_preferences(&loaded, 2, 8, 7, 6, 100, 100, 100, 100, 100);
    gchar *contents = NULL;
    g_assert_true(g_file_get_contents(fixture->file, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, legacy);
    g_free(contents);
    g_assert_true(radios_ui_preferences_save(fixture->root, &loaded, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_get_contents(fixture->file, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "version=1\n"));
    g_assert_nonnull(strstr(contents, "content_scale_percent=100\n"));
    g_free(contents);
}

static void
panel_scales_roundtrip(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const guint scales[] = {50, 53, 75, 100, 125, 143, 150, 143, 150};
    for (guint i = 0; i < G_N_ELEMENTS(scales); ++i) {
        guint mixer = scales[i], effector = scales[G_N_ELEMENTS(scales) - 1 - i];
        RadioSUIPreferences original = {2, 8, 7, 5, 85, mixer, effector, mixer, effector}, loaded = {0};
        GError *error = NULL;
        g_assert_true(radios_ui_preferences_save(fixture->root, &original, &error));
        g_assert_no_error(error);
        g_assert_true(radios_ui_preferences_load(fixture->root, &loaded, &error));
        g_assert_no_error(error);
        assert_preferences(&loaded, 2, 8, 7, 5, 85, mixer, effector, mixer, effector);
    }
}

static void
legacy_panel_scales_defaults(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const gchar *legacy = "[ui]\nversion=1\ntext_size=2\ndefault_profile=8\nlast_profile=7\nheader_hidden_fields=6\ncontent_scale_percent=85\n";
    write_file(fixture->file, legacy);
    RadioSUIPreferences loaded = {0};
    GError *error = NULL;
    g_assert_true(radios_ui_preferences_load(fixture->root, &loaded, &error));
    g_assert_no_error(error);
    assert_preferences(&loaded, 2, 8, 7, 6, 85, 100, 100, 100, 100);
    gchar *contents = NULL;
    g_assert_true(g_file_get_contents(fixture->file, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, legacy); g_free(contents);
    g_assert_true(radios_ui_preferences_save(fixture->root, &loaded, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_get_contents(fixture->file, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "version=1\n"));
    g_assert_nonnull(strstr(contents, "mixer_scale_percent=100\n"));
    g_assert_nonnull(strstr(contents, "effector_scale_percent=100\n"));
    g_assert_nonnull(strstr(contents, "mixer_height_percent=100\n"));
    g_assert_nonnull(strstr(contents, "effector_height_percent=100\n")); g_free(contents);
    const gchar *keys[] = {"mixer_scale_percent", "effector_scale_percent"};
    for (guint i = 0; i < G_N_ELEMENTS(keys); ++i) {
        contents = g_strdup_printf("%s%s=143\n", legacy, keys[i]);
        write_file(fixture->file, contents); g_free(contents);
        g_assert_true(radios_ui_preferences_load(fixture->root, &loaded, &error));
        g_assert_no_error(error);
        assert_preferences(&loaded, 2, 8, 7, 6, 85, i == 0 ? 143 : 100, i == 1 ? 143 : 100, i == 0 ? 143 : 100, i == 1 ? 143 : 100);
    }
}

/* Width and height of a section are saved separately and survive a roundtrip. */
static void
panel_heights_roundtrip(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const guint widths[] = {50, 75, 100, 125, 150}, heights[] = {150, 60, 100, 53, 143};
    for (guint i = 0; i < G_N_ELEMENTS(widths); ++i) {
        guint effector_width = widths[G_N_ELEMENTS(widths) - 1 - i], effector_height = heights[G_N_ELEMENTS(heights) - 1 - i];
        RadioSUIPreferences original = {2, 8, 7, 5, 85, widths[i], effector_width, heights[i], effector_height}, loaded = {0};
        GError *error = NULL;
        g_assert_true(radios_ui_preferences_save(fixture->root, &original, &error));
        g_assert_no_error(error);
        g_assert_true(radios_ui_preferences_load(fixture->root, &loaded, &error));
        g_assert_no_error(error);
        assert_preferences(&loaded, 2, 8, 7, 5, 85, widths[i], effector_width, heights[i], effector_height);
    }
    gchar *contents = NULL;
    g_assert_true(g_file_get_contents(fixture->file, &contents, NULL, NULL));
    g_assert_nonnull(strstr(contents, "mixer_height_percent=")); g_assert_nonnull(strstr(contents, "effector_height_percent="));
    g_free(contents);
}

/* Files from before the separate height settings kept one uniform scale: a
 * missing height follows its width; a saved height is used as it is. */
static void
legacy_heights_follow_widths(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const gchar *legacy = "[ui]\nversion=1\ntext_size=2\ndefault_profile=8\nlast_profile=7\nheader_hidden_fields=6\ncontent_scale_percent=85\n";
    RadioSUIPreferences loaded = {0};
    GError *error = NULL;
    gchar *contents = g_strdup_printf("%smixer_scale_percent=75\neffector_scale_percent=130\n", legacy);
    write_file(fixture->file, contents); g_free(contents);
    g_assert_true(radios_ui_preferences_load(fixture->root, &loaded, &error)); g_assert_no_error(error);
    assert_preferences(&loaded, 2, 8, 7, 6, 85, 75, 130, 75, 130);
    contents = g_strdup_printf("%smixer_scale_percent=75\neffector_scale_percent=130\nmixer_height_percent=140\n", legacy);
    write_file(fixture->file, contents); g_free(contents);
    g_assert_true(radios_ui_preferences_load(fixture->root, &loaded, &error)); g_assert_no_error(error);
    assert_preferences(&loaded, 2, 8, 7, 6, 85, 75, 130, 140, 130);
    contents = g_strdup_printf("%seffector_height_percent=60\n", legacy);
    write_file(fixture->file, contents); g_free(contents);
    g_assert_true(radios_ui_preferences_load(fixture->root, &loaded, &error)); g_assert_no_error(error);
    assert_preferences(&loaded, 2, 8, 7, 6, 85, 100, 100, 100, 60);
}

static void
invalid_panel_heights_unchanged(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const gchar *keys[] = {"mixer_height_percent", "effector_height_percent"};
    const gchar *invalid[] = {"0", "49", "151", "-1", "100.5", "no", "true", "18446744073709551615"};
    for (guint field = 0; field < G_N_ELEMENTS(keys); ++field) {
        for (guint i = 0; i < G_N_ELEMENTS(invalid); ++i) {
            gchar *contents = g_strdup_printf("[ui]\nversion=1\ntext_size=1\ndefault_profile=0\nlast_profile=0\ncontent_scale_percent=125\n%s=%s\n",
                keys[field], invalid[i]);
            write_file(fixture->file, contents); g_free(contents);
            RadioSUIPreferences value = {2, 8, 7, 5, 125, 83, 117, 61, 139};
            GError *error = NULL;
            g_assert_false(radios_ui_preferences_load(fixture->root, &value, &error));
            g_assert_nonnull(error); g_clear_error(&error);
            assert_preferences(&value, 2, 8, 7, 5, 125, 83, 117, 61, 139);
        }
    }
}

static void
invalid_panel_scales_unchanged(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const gchar *keys[] = {"mixer_scale_percent", "effector_scale_percent"};
    const gchar *invalid[] = {"0", "49", "151", "-1", "100.5", "no", "true", "18446744073709551615"};
    for (guint field = 0; field < G_N_ELEMENTS(keys); ++field) {
        for (guint i = 0; i < G_N_ELEMENTS(invalid); ++i) {
            gchar *contents = g_strdup_printf("[ui]\nversion=1\ntext_size=1\ndefault_profile=0\nlast_profile=0\ncontent_scale_percent=125\n%s=%s\n",
                keys[field], invalid[i]);
            write_file(fixture->file, contents); g_free(contents);
            RadioSUIPreferences value = {2, 8, 7, 5, 125, 83, 117, 83, 117};
            GError *error = NULL;
            g_assert_false(radios_ui_preferences_load(fixture->root, &value, &error));
            g_assert_nonnull(error); g_clear_error(&error);
            assert_preferences(&value, 2, 8, 7, 5, 125, 83, 117, 83, 117);
            g_assert_false(radios_ui_preferences_load(fixture->root, &value, NULL));
            assert_preferences(&value, 2, 8, 7, 5, 125, 83, 117, 83, 117);
        }
    }
}

static void
invalid_content_scale_unchanged(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const gchar *invalid[] = {"0", "49", "151", "-1", "100.5", "no", "true", "18446744073709551615"};
    for (guint i = 0; i < G_N_ELEMENTS(invalid); ++i) {
        gchar *contents = g_strdup_printf("[ui]\nversion=1\ntext_size=1\ndefault_profile=0\nlast_profile=0\ncontent_scale_percent=%s\n", invalid[i]);
        write_file(fixture->file, contents);
        g_free(contents);
        RadioSUIPreferences value = {2, 8, 7, 5, 125, 100, 100, 100, 100};
        GError *error = NULL;
        g_assert_false(radios_ui_preferences_load(fixture->root, &value, &error));
        g_assert_nonnull(error);
        g_clear_error(&error);
        assert_preferences(&value, 2, 8, 7, 5, 125, 100, 100, 100, 100);
        g_assert_false(radios_ui_preferences_load(fixture->root, &value, NULL));
        assert_preferences(&value, 2, 8, 7, 5, 125, 100, 100, 100, 100);
    }
}

static void
invalid_existing(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const gchar *invalid_files[] = {
        "[ui]\nversion=2\ntext_size=1\ndefault_profile=0\nlast_profile=0\n",
        "[ui]\ntext_size=1\ndefault_profile=0\nlast_profile=0\n",
        "[ui]\nversion=1\ndefault_profile=0\nlast_profile=0\n",
        "[ui]\nversion=1\ntext_size=3\ndefault_profile=0\nlast_profile=0\n",
        "[ui]\nversion=1\ntext_size=1\ndefault_profile=9\nlast_profile=0\n",
        "[ui]\nversion=1\ntext_size=1\ndefault_profile=0\nlast_profile=8\n",
        "[ui]\nversion=1\ntext_size=-1\ndefault_profile=0\nlast_profile=0\n",
        "[ui]\nversion=1\ntext_size=1.5\ndefault_profile=0\nlast_profile=0\n",
        "[ui]\nversion=1\ntext_size=18446744073709551615\ndefault_profile=0\nlast_profile=0\n",
        "[broken\nversion=1\n",
        "[ui]\nversion=1\ntext_size=1\ndefault_profile=no\nlast_profile=0\n",
        "[ui]\nversion=1\ntext_size=1\ndefault_profile=0\nlast_profile=0\nheader_hidden_fields=8\n",
        "[ui]\nversion=1\ntext_size=1\ndefault_profile=0\nlast_profile=0\nheader_hidden_fields=-1\n",
        "[ui]\nversion=1\ntext_size=1\ndefault_profile=0\nlast_profile=0\nheader_hidden_fields=1.5\n",
        "[ui]\nversion=1\ntext_size=1\ndefault_profile=0\nlast_profile=0\nheader_hidden_fields=no\n",
        "[ui]\nversion=1\ntext_size=1\ndefault_profile=0\nlast_profile=0\nheader_hidden_fields=18446744073709551615\n"
    };
    for (guint i = 0; i < G_N_ELEMENTS(invalid_files); ++i) {
        write_file(fixture->file, invalid_files[i]);
        RadioSUIPreferences value = {2, 7, 6, 5, 100, 100, 100, 100, 100};
        GError *error = NULL;
        g_assert_false(radios_ui_preferences_load(fixture->root, &value, &error));
        g_assert_nonnull(error);
        g_clear_error(&error);
        assert_preferences(&value, 2, 7, 6, 5, 100, 100, 100, 100, 100);
        /* The error-less API must also reject malformed fields. */
        g_assert_false(radios_ui_preferences_load(fixture->root, &value, NULL));
        assert_preferences(&value, 2, 7, 6, 5, 100, 100, 100, 100, 100);
    }
}

static void
invalid_save(Fixture *fixture, gconstpointer data)
{
    (void)data;
    const gchar *original = "[ui]\nversion=1\ntext_size=1\ndefault_profile=0\nlast_profile=0\n";
    write_file(fixture->file, original);
    const RadioSUIPreferences invalid_values[] = {
        {3, 0, 0, 0, 100, 100, 100, 100, 100}, {1, 9, 0, 0, 100, 100, 100, 100, 100}, {1, 0, 8, 0, 100, 100, 100, 100, 100}, {G_MAXUINT, 0, 0, 0, 100, 100, 100, 100, 100},
        {1, 0, 0, 8, 100, 100, 100, 100, 100}, {1, 0, 0, G_MAXUINT, 100, 100, 100, 100, 100},
        {1, 0, 0, 0, 0, 100, 100, 100, 100}, {1, 0, 0, 0, 49, 100, 100, 100, 100}, {1, 0, 0, 0, 151, 100, 100, 100, 100}, {1, 0, 0, 0, G_MAXUINT, 100, 100, 100, 100},
        {1, 0, 0, 0, 100, 0, 100, 0, 100}, {1, 0, 0, 0, 100, 49, 100, 49, 100}, {1, 0, 0, 0, 100, 151, 100, 151, 100}, {1, 0, 0, 0, 100, G_MAXUINT, 100, G_MAXUINT, 100},
        {1, 0, 0, 0, 100, 100, 0, 100, 0}, {1, 0, 0, 0, 100, 100, 49, 100, 49}, {1, 0, 0, 0, 100, 100, 151, 100, 151}, {1, 0, 0, 0, 100, 100, G_MAXUINT, 100, G_MAXUINT},
        {1, 0, 0, 0, 100, 100, 100, 0, 100}, {1, 0, 0, 0, 100, 100, 100, 49, 100}, {1, 0, 0, 0, 100, 100, 100, 151, 100}, {1, 0, 0, 0, 100, 100, 100, G_MAXUINT, 100},
        {1, 0, 0, 0, 100, 100, 100, 100, 0}, {1, 0, 0, 0, 100, 100, 100, 100, 49}, {1, 0, 0, 0, 100, 100, 100, 100, 151}, {1, 0, 0, 0, 100, 100, 100, 100, G_MAXUINT}
    };
    for (guint i = 0; i < G_N_ELEMENTS(invalid_values); ++i) {
        GError *error = NULL;
        g_assert_false(radios_ui_preferences_save(fixture->root, &invalid_values[i], &error));
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_clear_error(&error);
        gchar *contents = NULL;
        g_assert_true(g_file_get_contents(fixture->file, &contents, NULL, NULL));
        g_assert_cmpstr(contents, ==, original);
        g_free(contents);
    }
}

static void
bounded_read(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GString *contents = g_string_new("[ui]\nversion=1\ntext_size=1\ndefault_profile=0\nlast_profile=0\n#");
    while (contents->len < 8192)
        g_string_append_c(contents, 'x');
    GError *error = NULL;
    RadioSUIPreferences value = {2, 7, 6, 5, 100, 100, 100, 100, 100};
    write_file(fixture->file, contents->str);
    g_assert_true(radios_ui_preferences_load(fixture->root, &value, &error));
    g_assert_no_error(error);
    assert_preferences(&value, 1, 0, 0, 0, 100, 100, 100, 100, 100);
    g_string_append_c(contents, 'x');
    write_file(fixture->file, contents->str);
    value = (RadioSUIPreferences){2, 7, 6, 5, 100, 100, 100, 100, 100};
    g_assert_false(radios_ui_preferences_load(fixture->root, &value, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    assert_preferences(&value, 2, 7, 6, 5, 100, 100, 100, 100, 100);
    g_string_free(contents, TRUE);
    const gchar binary[] = "[ui]\nversion=1\0text_size=1\n";
    g_assert_true(g_file_set_contents(fixture->file, binary, sizeof binary - 1, NULL));
    g_assert_false(radios_ui_preferences_load(fixture->root, &value, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    const gchar non_utf8[] = {'[', 'u', 'i', ']', '\n', (gchar)0xff, '\0', (gchar)0xff, '\0'};
    write_file(fixture->file, non_utf8);
    g_assert_false(radios_ui_preferences_load(fixture->root, &value, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void
file_links(Fixture *fixture, gconstpointer data)
{
    (void)data;
    gchar *external = g_build_filename(fixture->temporary, "external.ini", NULL);
    write_file(external, "external sentinel\n");
    if (!rt_symlink(external, fixture->file, FALSE, NULL)) {
        g_free(external);
        g_test_skip("symbolic links unavailable");
        return;
    }
    RadioSUIPreferences value = {2, 7, 6, 5, 100, 100, 100, 100, 100};
    GError *error = NULL;
    g_assert_false(radios_ui_preferences_load(fixture->root, &value, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    assert_preferences(&value, 2, 7, 6, 5, 100, 100, 100, 100, 100);
    g_assert_false(radios_ui_preferences_save(fixture->root, &value, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    gchar *contents = NULL;
    g_assert_true(g_file_get_contents(external, &contents, NULL, NULL));
    g_assert_cmpstr(contents, ==, "external sentinel\n");
    g_free(contents);
    g_assert_true(rt_is_link(fixture->file));
    g_assert_cmpint(g_remove(external), ==, 0);
    /* Dangling links must not be mistaken for a missing preference file. */
    g_assert_false(radios_ui_preferences_load(fixture->root, &value, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_free(external);
}

static void
directory_links(Fixture *fixture, gconstpointer data)
{
    (void)data;
    gchar *external = g_build_filename(fixture->temporary, "outside", NULL);
    g_assert_cmpint(g_mkdir(external, 0700), ==, 0);
    g_assert_cmpint(g_rmdir(fixture->config), ==, 0);
    GError *error = NULL;
    /* Directory links fall back to junctions without the symbolic-link privilege. */
    g_assert_true(rt_symlink(external, fixture->config, TRUE, &error));
    g_assert_no_error(error);
    RadioSUIPreferences value = {2, 7, 6, 5, 100, 100, 100, 100, 100};
    g_assert_false(radios_ui_preferences_load(fixture->root, &value, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_assert_false(radios_ui_preferences_save(fixture->root, &value, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    assert_preferences(&value, 2, 7, 6, 5, 100, 100, 100, 100, 100);
    gchar *outside_file = g_build_filename(external, "ui-preferences.ini", NULL);
    g_assert_false(g_file_test(outside_file, G_FILE_TEST_EXISTS));
    g_free(outside_file);
    g_assert_cmpint(g_remove(fixture->config), ==, 0);
    gchar *internal = g_build_filename(fixture->root, "local config", NULL);
    g_assert_cmpint(g_mkdir(internal, 0700), ==, 0);
    /* Relative link inside the root (a junction fallback is absolute, still inside). */
    g_assert_true(rt_symlink("local config", fixture->config, TRUE, &error));
    g_assert_no_error(error);
    g_assert_true(radios_ui_preferences_save(fixture->root, &value, &error));
    g_assert_no_error(error);
    value = (RadioSUIPreferences){0};
    g_assert_true(radios_ui_preferences_load(fixture->root, &value, &error));
    assert_preferences(&value, 2, 7, 6, 5, 100, 100, 100, 100, 100);
    g_free(internal);
    g_free(external);
}

static void
missing_directory(Fixture *fixture, gconstpointer data)
{
    (void)data;
    g_assert_cmpint(g_rmdir(fixture->config), ==, 0);
    RadioSUIPreferences value = {2, 7, 6, 5, 100, 100, 100, 100, 100};
    GError *error = NULL;
    g_assert_false(radios_ui_preferences_load(fixture->root, &value, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(radios_ui_preferences_save(fixture->root, &value, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    assert_preferences(&value, 2, 7, 6, 5, 100, 100, 100, 100, 100);
    g_assert_false(g_file_test(fixture->config, G_FILE_TEST_EXISTS));
}

static void
relative_root(Fixture *fixture, gconstpointer data)
{
    (void)data;
    gchar *cwd = g_get_current_dir();
    g_assert_cmpint(g_chdir(fixture->temporary), ==, 0);
    RadioSUIPreferences value = {0, 2, 4, 6, 100, 100, 100, 100, 100}, result = {1, 0, 0, 0, 100, 100, 100, 100, 100};
    GError *error = NULL;
    g_assert_true(radios_ui_preferences_save("Türkçe kaynak kodlar RadioS", &value, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_chdir("/"), ==, 0);
    g_assert_true(radios_ui_preferences_load(fixture->root, &result, &error));
    g_assert_no_error(error);
    assert_preferences(&result, 0, 2, 4, 6, 100, 100, 100, 100, 100);
    g_assert_cmpint(g_chdir(cwd), ==, 0);
    g_free(cwd);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/ui-preferences/missing", Fixture, NULL, setup, missing, teardown);
    g_test_add("/ui-preferences/roundtrip-private-atomic", Fixture, NULL, setup, roundtrip, teardown);
    g_test_add("/ui-preferences/header-fields-roundtrip", Fixture, NULL, setup, header_fields, teardown);
    g_test_add("/ui-preferences/legacy-header-defaults", Fixture, NULL, setup, legacy_header_defaults, teardown);
    g_test_add("/ui-preferences/content-scale-roundtrip", Fixture, NULL, setup, content_scale_roundtrip, teardown);
    g_test_add("/ui-preferences/legacy-content-scale-defaults", Fixture, NULL, setup, legacy_content_scale_defaults, teardown);
    g_test_add("/ui-preferences/panel-scales-independent-roundtrip", Fixture, NULL, setup, panel_scales_roundtrip, teardown);
    g_test_add("/ui-preferences/legacy-panel-scales-defaults", Fixture, NULL, setup, legacy_panel_scales_defaults, teardown);
    g_test_add("/ui-preferences/panel-heights-independent-roundtrip", Fixture, NULL, setup, panel_heights_roundtrip, teardown);
    g_test_add("/ui-preferences/legacy-heights-follow-widths", Fixture, NULL, setup, legacy_heights_follow_widths, teardown);
    g_test_add("/ui-preferences/invalid-panel-heights-unchanged", Fixture, NULL, setup, invalid_panel_heights_unchanged, teardown);
    g_test_add("/ui-preferences/invalid-panel-scales-unchanged", Fixture, NULL, setup, invalid_panel_scales_unchanged, teardown);
    g_test_add("/ui-preferences/invalid-content-scale-unchanged", Fixture, NULL, setup, invalid_content_scale_unchanged, teardown);
    g_test_add("/ui-preferences/invalid-existing-unchanged", Fixture, NULL, setup, invalid_existing, teardown);
    g_test_add("/ui-preferences/invalid-save-unchanged", Fixture, NULL, setup, invalid_save, teardown);
    g_test_add("/ui-preferences/bounded-read", Fixture, NULL, setup, bounded_read, teardown);
    g_test_add("/ui-preferences/file-symlinks", Fixture, NULL, setup, file_links, teardown);
    g_test_add("/ui-preferences/directory-symlinks", Fixture, NULL, setup, directory_links, teardown);
    g_test_add("/ui-preferences/missing-directory", Fixture, NULL, setup, missing_directory, teardown);
    g_test_add("/ui-preferences/relative-space-utf8-root", Fixture, NULL, setup, relative_root, teardown);
    return g_test_run();
}
