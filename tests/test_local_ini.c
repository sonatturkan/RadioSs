#define _XOPEN_SOURCE 700

#include "radios/local_ini.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Workstation files in <project>/config: media-archive.ini and storage.ini
 * share this reader/writer. The fixture is a temporary project root with a
 * non-ASCII name and an empty config directory. */
typedef struct { gchar *base, *root, *config; } Fixture;

static void remove_tree(const gchar *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_SYMLINK) || !g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_assert_cmpint(g_remove(path), ==, 0);
        return;
    }
    GDir *directory = g_dir_open(path, 0, NULL);
    g_assert_nonnull(directory);
    const gchar *name;
    while ((name = g_dir_read_name(directory))) {
        gchar *child = g_build_filename(path, name, NULL);
        remove_tree(child); g_free(child);
    }
    g_dir_close(directory); g_assert_cmpint(g_rmdir(path), ==, 0);
}

static void setup(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GError *error = NULL;
    fixture->base = g_dir_make_tmp("radios-local-ini-XXXXXX", &error);
    g_assert_no_error(error);
    fixture->root = g_build_filename(fixture->base, "Türkçe RadioS", NULL);
    fixture->config = g_build_filename(fixture->root, "config", NULL);
    g_assert_cmpint(g_mkdir_with_parents(fixture->config, 0700), ==, 0);
}

static void teardown(Fixture *fixture, gconstpointer data)
{
    (void)data;
    remove_tree(fixture->base);
    g_free(fixture->base); g_free(fixture->root); g_free(fixture->config);
}

static guint entries_in_config(Fixture *fixture)
{
    GDir *directory = g_dir_open(fixture->config, 0, NULL);
    guint count = 0;
    g_assert_nonnull(directory);
    while (g_dir_read_name(directory)) ++count;
    g_dir_close(directory);
    return count;
}

static void assert_invalid_read(Fixture *fixture, const gchar *name, gsize limit)
{
    GError *error = NULL; gchar *contents = NULL;
    g_assert_false(radios_local_ini_read(fixture->root, name, limit, &contents, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error); g_assert_null(contents);
}

static void assert_invalid_write(Fixture *fixture, const gchar *name)
{
    GError *error = NULL;
    g_assert_false(radios_local_ini_write(fixture->root, name, "x=1\n", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void missing(Fixture *fixture, gconstpointer data)
{
    (void)data;
    gchar *contents = (gchar *)"stale"; GError *error = NULL;
    g_assert_true(radios_local_ini_read(fixture->root, "missing.ini", 4096, &contents, &error));
    g_assert_no_error(error); g_assert_null(contents);
    g_assert_cmpuint(entries_in_config(fixture), ==, 0);
}

static void roundtrip(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GError *error = NULL; gchar *contents = NULL;
    g_assert_true(radios_local_ini_write(fixture->root, "x.ini", "[a]\nb=Türkçe değer\n", &error));
    g_assert_no_error(error);
    g_assert_true(radios_local_ini_read(fixture->root, "x.ini", 4096, &contents, &error));
    g_assert_no_error(error); g_assert_cmpstr(contents, ==, "[a]\nb=Türkçe değer\n"); g_free(contents);
    gchar *path = g_build_filename(fixture->config, "x.ini", NULL); struct stat info;
    g_assert_cmpint(lstat(path, &info), ==, 0);
    g_assert_true(S_ISREG(info.st_mode)); g_assert_cmpuint(info.st_mode & 0777, ==, 0600);
    /* The atomic replacement leaves no temporary file behind. */
    g_assert_cmpuint(entries_in_config(fixture), ==, 1);
    g_assert_true(radios_local_ini_write(fixture->root, "x.ini", "replaced", &error));
    g_assert_no_error(error);
    g_assert_true(radios_local_ini_read(fixture->root, "x.ini", 4096, &contents, &error));
    g_assert_no_error(error); g_assert_cmpstr(contents, ==, "replaced"); g_free(contents);
    g_assert_cmpuint(entries_in_config(fixture), ==, 1);
    g_assert_true(radios_local_ini_write(fixture->root, "x.ini", "", &error));
    g_assert_no_error(error);
    g_assert_true(radios_local_ini_read(fixture->root, "x.ini", 4096, &contents, &error));
    g_assert_no_error(error); g_assert_cmpstr(contents, ==, ""); g_free(contents);
    g_free(path);
}

static void names(Fixture *fixture, gconstpointer data)
{
    (void)data;
    gchar *long_name = g_strnfill(100, 'a');
    const gchar *bad[] = {".hidden.ini", "a/b.ini", "", long_name};
    for (guint i = 0; i < G_N_ELEMENTS(bad); ++i) {
        assert_invalid_read(fixture, bad[i], 4096);
        assert_invalid_write(fixture, bad[i]);
    }
    assert_invalid_read(fixture, "x.ini", 0);
    assert_invalid_read(fixture, "x.ini", 2u * 1024u * 1024u);
    g_assert_cmpuint(entries_in_config(fixture), ==, 0);
    g_free(long_name);
}

static void links(Fixture *fixture, gconstpointer data)
{
    (void)data;
    gchar *target = g_build_filename(fixture->root, "target.ini", NULL);
    gchar *link = g_build_filename(fixture->config, "link.ini", NULL);
    g_assert_true(g_file_set_contents(target, "secret", -1, NULL));
    g_assert_cmpint(symlink(target, link), ==, 0);
    assert_invalid_read(fixture, "link.ini", 4096);
    assert_invalid_write(fixture, "link.ini");
    gchar *kept = NULL;
    g_assert_true(g_file_get_contents(target, &kept, NULL, NULL));
    g_assert_cmpstr(kept, ==, "secret"); g_free(kept);
    g_assert_true(g_file_test(link, G_FILE_TEST_IS_SYMLINK));
    g_assert_cmpuint(entries_in_config(fixture), ==, 1);
    gchar *directory = g_build_filename(fixture->config, "dir.ini", NULL);
    g_assert_cmpint(g_mkdir(directory, 0700), ==, 0);
    assert_invalid_read(fixture, "dir.ini", 4096);
    assert_invalid_write(fixture, "dir.ini");
    g_free(target); g_free(link); g_free(directory);
}

static void limits(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GError *error = NULL; gchar *contents = NULL;
    g_assert_true(radios_local_ini_write(fixture->root, "big.ini", "0123456789", &error));
    g_assert_no_error(error);
    assert_invalid_read(fixture, "big.ini", 5);
    g_assert_true(radios_local_ini_read(fixture->root, "big.ini", 10, &contents, &error));
    g_assert_no_error(error); g_assert_cmpstr(contents, ==, "0123456789"); g_free(contents);
    gchar *path = g_build_filename(fixture->config, "bytes.ini", NULL);
    g_assert_true(g_file_set_contents(path, "\xff\xfe", 2, NULL));
    assert_invalid_read(fixture, "bytes.ini", 4096);
    g_assert_true(g_file_set_contents(path, "a\0b", 3, NULL));
    assert_invalid_read(fixture, "bytes.ini", 4096);
    g_free(path);
}

static void config_escape(Fixture *fixture, gconstpointer data)
{
    (void)data;
    GError *error = NULL;
    gchar *outside = g_dir_make_tmp("radios-local-ini-outside-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_cmpint(g_rmdir(fixture->config), ==, 0);
    g_assert_cmpint(symlink(outside, fixture->config), ==, 0);
    assert_invalid_read(fixture, "x.ini", 4096);
    assert_invalid_write(fixture, "x.ini");
    GDir *directory = g_dir_open(outside, 0, NULL);
    g_assert_nonnull(directory); g_assert_null(g_dir_read_name(directory)); g_dir_close(directory);
    g_assert_cmpint(g_remove(fixture->config), ==, 0);
    /* Without a config directory nothing can be read or written either. */
    gchar *contents = NULL;
    g_assert_false(radios_local_ini_read(fixture->root, "x.ini", 4096, &contents, &error));
    g_assert_nonnull(error); g_clear_error(&error); g_assert_null(contents);
    g_assert_false(radios_local_ini_write(fixture->root, "x.ini", "x", &error));
    g_assert_nonnull(error); g_clear_error(&error);
    g_assert_cmpint(g_mkdir(fixture->config, 0700), ==, 0);
    g_assert_cmpint(g_rmdir(outside), ==, 0); g_free(outside);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/local-ini/missing-file", Fixture, NULL, setup, missing, teardown);
    g_test_add("/local-ini/atomic-roundtrip", Fixture, NULL, setup, roundtrip, teardown);
    g_test_add("/local-ini/names-and-limits", Fixture, NULL, setup, names, teardown);
    g_test_add("/local-ini/links-rejected", Fixture, NULL, setup, links, teardown);
    g_test_add("/local-ini/size-and-utf8", Fixture, NULL, setup, limits, teardown);
    g_test_add("/local-ini/config-escape", Fixture, NULL, setup, config_escape, teardown);
    return g_test_run();
}
