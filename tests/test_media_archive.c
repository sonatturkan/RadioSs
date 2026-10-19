#define _XOPEN_SOURCE 700
#include "radios/media_archive.h"
#include <glib/gstdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct { gchar *temporary, *root, *archive_a, *archive_b, *index; } Fixture;

static void remove_tree(const gchar *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_SYMLINK) || !g_file_test(path, G_FILE_TEST_IS_DIR)) { g_assert_cmpint(g_remove(path), ==, 0); return; }
    GDir *directory = g_dir_open(path, 0, NULL); g_assert_nonnull(directory);
    const gchar *name;
    while ((name = g_dir_read_name(directory)) != NULL) { gchar *child = g_build_filename(path, name, NULL); remove_tree(child); g_free(child); }
    g_dir_close(directory);
    g_assert_cmpint(g_rmdir(path), ==, 0);
}

static void write_file(const gchar *path, const gchar *contents)
{
    gchar *directory = g_path_get_dirname(path); g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0); g_free(directory);
    GError *error = NULL; g_file_set_contents(path, contents, -1, &error); g_assert_no_error(error);
}

static gchar *child(const gchar *root, const gchar *relative)
{ return g_build_filename(root, relative, NULL); }

static void setup(Fixture *fixture, gconstpointer data)
{
    (void)data; GError *error = NULL;
    gchar *made = g_dir_make_tmp("radios-media-archive-XXXXXX", &error); g_assert_no_error(error);
    gchar *physical = realpath(made, NULL); g_assert_nonnull(physical);
    fixture->temporary = g_strdup(physical); free(physical); g_free(made);
    fixture->root = child(fixture->temporary, "RadioS proje");
    gchar *config = child(fixture->root, "config"); g_assert_cmpint(g_mkdir_with_parents(config, 0700), ==, 0); g_free(config);
    fixture->archive_a = child(fixture->temporary, "arşiv A");
    fixture->archive_b = child(fixture->temporary, "arşiv B");
    fixture->index = child(fixture->root, "data/cache/media-archive-index.json");
    gchar *path;
    path = child(fixture->archive_a, "x.mp3"); write_file(path, "mp3-x"); g_free(path);
    path = child(fixture->archive_a, "top.OGG"); write_file(path, "ogg-top"); g_free(path);
    path = child(fixture->archive_a, "notes.txt"); write_file(path, "not ses"); g_free(path);
    path = child(fixture->archive_a, ".hidden.mp3"); write_file(path, "gizli"); g_free(path);
    path = child(fixture->archive_a, "sub/y.wav"); write_file(path, "wav-y"); g_free(path);
    path = child(fixture->archive_a, "sub/copy.flac"); write_file(path, "mp3-x"); g_free(path); /* same bytes as x.mp3 */
    path = child(fixture->archive_b, "z.flac"); write_file(path, "flac-z"); g_free(path);
    gchar *target = child(fixture->archive_a, "x.mp3"), *link = child(fixture->archive_b, "link.mp3");
    g_assert_cmpint(symlink(target, link), ==, 0); g_free(target); g_free(link);
}

static void teardown(Fixture *fixture, gconstpointer data)
{
    (void)data; remove_tree(fixture->temporary);
    g_free(fixture->temporary); g_free(fixture->root); g_free(fixture->archive_a); g_free(fixture->archive_b); g_free(fixture->index);
}

static void test_settings_validate(Fixture *fixture, gconstpointer data)
{
    (void)data; RadioSMediaArchiveSettings settings; radios_media_archive_settings_init(&settings);
    GError *error = NULL;
    g_assert_true(radios_media_archive_settings_validate(&settings, &error)); g_assert_no_error(error);
    g_assert_cmpstr(settings.kind, ==, "music"); g_assert_cmpuint(settings.watch_minutes, ==, 30); g_assert_false(settings.watch);
    g_ptr_array_add(settings.folders, g_strdup(fixture->archive_a));
    gchar *nested = child(fixture->archive_a, "sub"); g_ptr_array_add(settings.folders, nested);
    g_assert_false(radios_media_archive_settings_validate(&settings, &error)); g_assert_nonnull(strstr(error->message, "iç içe")); g_clear_error(&error);
    g_ptr_array_remove_index(settings.folders, 1);
    g_ptr_array_add(settings.folders, g_strdup(fixture->archive_a));
    g_assert_false(radios_media_archive_settings_validate(&settings, &error)); g_assert_nonnull(strstr(error->message, "iki kez")); g_clear_error(&error);
    g_ptr_array_remove_index(settings.folders, 1);
    g_ptr_array_add(settings.folders, g_strdup("göreli/yol"));
    g_assert_false(radios_media_archive_settings_validate(&settings, &error)); g_clear_error(&error);
    g_ptr_array_remove_index(settings.folders, 1);
    g_free(settings.kind); settings.kind = g_strdup("video");
    g_assert_false(radios_media_archive_settings_validate(&settings, &error)); g_clear_error(&error);
    g_free(settings.kind); settings.kind = g_strdup("jingle");
    settings.watch_minutes = 4;
    g_assert_false(radios_media_archive_settings_validate(&settings, &error)); g_clear_error(&error);
    settings.watch_minutes = 1440;
    g_assert_true(radios_media_archive_settings_validate(&settings, &error)); g_assert_no_error(error);
    radios_media_archive_settings_clear(&settings);
}

static void test_settings_roundtrip(Fixture *fixture, gconstpointer data)
{
    (void)data; GError *error = NULL;
    RadioSMediaArchiveSettings loaded; radios_media_archive_settings_init(&loaded);
    g_assert_true(radios_media_archive_settings_load(fixture->root, &loaded, &error)); g_assert_no_error(error);
    g_assert_cmpuint(loaded.folders->len, ==, 0);
    gchar *file = child(fixture->root, "config/media-archive.ini");
    g_assert_false(g_file_test(file, G_FILE_TEST_EXISTS)); /* a missing file never gets written by load */
    RadioSMediaArchiveSettings settings; radios_media_archive_settings_init(&settings);
    g_ptr_array_add(settings.folders, g_strdup(fixture->archive_a)); g_ptr_array_add(settings.folders, g_strdup(fixture->archive_b));
    g_free(settings.kind); settings.kind = g_strdup("advertisement");
    settings.folder_playlists = TRUE; settings.watch = TRUE; settings.watch_minutes = 15;
    g_assert_true(radios_media_archive_settings_save(fixture->root, &settings, &error)); g_assert_no_error(error);
    GStatBuf info; g_assert_cmpint(g_stat(file, &info), ==, 0); g_assert_cmpint(info.st_mode & 0777, ==, 0600);
    g_assert_true(radios_media_archive_settings_load(fixture->root, &loaded, &error)); g_assert_no_error(error);
    g_assert_cmpuint(loaded.folders->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(loaded.folders, 0), ==, fixture->archive_a);
    g_assert_cmpstr(g_ptr_array_index(loaded.folders, 1), ==, fixture->archive_b);
    g_assert_cmpstr(loaded.kind, ==, "advertisement"); g_assert_true(loaded.folder_playlists); g_assert_true(loaded.watch);
    g_assert_cmpuint(loaded.watch_minutes, ==, 15);
    write_file(file, "[archive]\nversion=2\n");
    g_assert_false(radios_media_archive_settings_load(fixture->root, &loaded, &error)); g_assert_nonnull(error);
    g_assert_cmpstr(loaded.kind, ==, "advertisement"); /* a broken file leaves the caller's copy alone */
    g_clear_error(&error);
    write_file(file, "[archive]\nversion=1\nwatch_minutes=2\n");
    g_assert_false(radios_media_archive_settings_load(fixture->root, &loaded, &error)); g_assert_nonnull(strstr(error->message, "5–1440")); g_clear_error(&error);
    radios_media_archive_settings_clear(&settings); radios_media_archive_settings_clear(&loaded); g_free(file);
}

static void test_names_and_purposes(void)
{
    g_assert_true(radios_media_archive_is_audio_name("Şarkı.MP3")); g_assert_true(radios_media_archive_is_audio_name("a.opus"));
    g_assert_true(radios_media_archive_is_audio_name("a.m4a")); g_assert_false(radios_media_archive_is_audio_name("a.txt"));
    g_assert_false(radios_media_archive_is_audio_name("mp3")); g_assert_false(radios_media_archive_is_audio_name(NULL));
    g_assert_cmpstr(radios_media_archive_playlist_purpose("music"), ==, "music");
    g_assert_cmpstr(radios_media_archive_playlist_purpose("advertisement"), ==, "advert");
    g_assert_cmpstr(radios_media_archive_playlist_purpose("jingle"), ==, "jingle");
    g_assert_cmpstr(radios_media_archive_playlist_purpose("sweeper"), ==, "sweeper");
    g_assert_cmpstr(radios_media_archive_playlist_purpose("news"), ==, "announcement");
    g_assert_cmpstr(radios_media_archive_playlist_purpose("voice_track"), ==, "announcement");
    g_assert_cmpstr(radios_media_archive_playlist_purpose("program"), ==, "programme");
    g_assert_cmpstr(radios_media_archive_playlist_purpose("effect"), ==, "other");
    g_assert_cmpstr(radios_media_archive_playlist_purpose(NULL), ==, "music");
    g_assert_true(radios_media_archive_kind_is_valid("other")); g_assert_false(radios_media_archive_kind_is_valid("")); g_assert_false(radios_media_archive_kind_is_valid(NULL));
}

static void test_folder_state(Fixture *fixture, gconstpointer data)
{
    (void)data;
    g_assert_cmpint(radios_media_archive_folder_state(fixture->archive_a), ==, RADIOS_MEDIA_ARCHIVE_FOLDER_READY);
    gchar *missing = child(fixture->temporary, "yok"), *file = child(fixture->archive_a, "x.mp3");
    g_assert_cmpint(radios_media_archive_folder_state(missing), ==, RADIOS_MEDIA_ARCHIVE_FOLDER_MISSING);
    g_assert_cmpint(radios_media_archive_folder_state(file), ==, RADIOS_MEDIA_ARCHIVE_FOLDER_NOT_DIRECTORY);
    g_assert_cmpint(radios_media_archive_folder_state(NULL), ==, RADIOS_MEDIA_ARCHIVE_FOLDER_MISSING);
    g_assert_nonnull(strstr(radios_media_archive_folder_state_label(RADIOS_MEDIA_ARCHIVE_FOLDER_MISSING), "Bulunamadı"));
    g_assert_cmpstr(radios_media_archive_folder_state_label(RADIOS_MEDIA_ARCHIVE_FOLDER_READY), ==, "Hazır");
    g_free(missing); g_free(file);
}

typedef struct { guint calls, last_total; } Seen;
static gboolean progress(guint done, guint total, const gchar *path, gpointer user)
{ (void)done; (void)path; Seen *seen = user; ++seen->calls; seen->last_total = total; return TRUE; }
static gboolean cancelling(guint done, guint total, const gchar *path, gpointer user)
{ (void)done; (void)total; (void)path; (void)user; return FALSE; }

static const RadioSMediaArchiveFile *file_at(GPtrArray *files, guint index)
{ g_assert_cmpuint(index, <, files->len); return g_ptr_array_index(files, index); }

static void test_scan(Fixture *fixture, gconstpointer data)
{
    (void)data; GError *error = NULL; RadioSMediaArchiveScanStats stats; Seen seen = {0};
    gchar *missing = child(fixture->temporary, "takılı değil");
    const gchar *folders[] = {fixture->archive_a, fixture->archive_b, missing, NULL};
    GPtrArray *files = radios_media_archive_scan(folders, fixture->index, NULL, progress, &seen, &stats, &error);
    g_assert_no_error(error); g_assert_nonnull(files);
    /* Hidden entries, non-audio names and the symbolic link are skipped. */
    g_assert_cmpuint(files->len, ==, 5); g_assert_cmpuint(stats.files, ==, 5); g_assert_cmpuint(stats.hashed, ==, 5);
    g_assert_cmpuint(stats.folders_missing, ==, 1); g_assert_cmpuint(stats.skipped_links, ==, 1); g_assert_cmpuint(stats.unreadable, ==, 0);
    g_assert_false(stats.truncated); g_assert_cmpuint(stats.bytes, ==, 5 + 7 + 5 + 5 + 6);
    g_assert_cmpuint(seen.calls, >=, 2); g_assert_cmpuint(seen.last_total, ==, 5);
    /* Sorted by path; the group is the first folder below the archive folder. */
    gchar *expected = child(fixture->archive_a, "sub/copy.flac");
    g_assert_cmpstr(file_at(files, 0)->path, ==, expected); g_free(expected);
    g_assert_cmpstr(file_at(files, 0)->group, ==, "sub"); g_assert_cmpstr(file_at(files, 0)->folder, ==, fixture->archive_a);
    expected = child(fixture->archive_a, "sub/y.wav"); g_assert_cmpstr(file_at(files, 1)->path, ==, expected); g_free(expected);
    expected = child(fixture->archive_a, "top.OGG"); g_assert_cmpstr(file_at(files, 2)->path, ==, expected); g_free(expected);
    g_assert_cmpstr(file_at(files, 2)->group, ==, "");
    expected = child(fixture->archive_a, "x.mp3"); g_assert_cmpstr(file_at(files, 3)->path, ==, expected); g_free(expected);
    expected = child(fixture->archive_b, "z.flac"); g_assert_cmpstr(file_at(files, 4)->path, ==, expected); g_free(expected);
    g_assert_cmpstr(file_at(files, 4)->folder, ==, fixture->archive_b);
    gchar *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "mp3-x", -1);
    g_assert_cmpstr(file_at(files, 3)->sha256, ==, digest); g_assert_cmpstr(file_at(files, 0)->sha256, ==, digest);
    g_assert_cmpint(file_at(files, 3)->size, ==, 5);
    /* New files: library digests and equal content inside the archive are skipped once. */
    GHashTable *library = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_hash_table_add(library, g_compute_checksum_for_string(G_CHECKSUM_SHA256, "wav-y", -1));
    guint duplicates = 99;
    GPtrArray *fresh = radios_media_archive_new_files(files, library, &duplicates);
    g_assert_cmpuint(fresh->len, ==, 3); g_assert_cmpuint(duplicates, ==, 1);
    g_assert_true(g_ptr_array_index(fresh, 0) == file_at(files, 0)); /* copy.flac is first by path; x.mp3 is its duplicate */
    g_assert_true(g_ptr_array_index(fresh, 1) == file_at(files, 2));
    g_assert_true(g_ptr_array_index(fresh, 2) == file_at(files, 4));
    g_ptr_array_unref(fresh); g_hash_table_unref(library);
    /* The index cache answers the second scan without hashing. */
    g_assert_true(g_file_test(fixture->index, G_FILE_TEST_IS_REGULAR));
    g_ptr_array_unref(files);
    files = radios_media_archive_scan(folders, fixture->index, NULL, NULL, NULL, &stats, &error);
    g_assert_no_error(error); g_assert_nonnull(files);
    g_assert_cmpuint(stats.files, ==, 5); g_assert_cmpuint(stats.hashed, ==, 0);
    g_assert_cmpstr(file_at(files, 3)->sha256, ==, digest);
    /* A changed file is hashed again. */
    gchar *changed = child(fixture->archive_b, "z.flac"); write_file(changed, "flac-z-yeni"); g_free(changed);
    GStatBuf before; g_stat(fixture->index, &before);
    g_ptr_array_unref(files);
    files = radios_media_archive_scan(folders, fixture->index, NULL, NULL, NULL, &stats, &error);
    g_assert_no_error(error); g_assert_nonnull(files); g_assert_cmpuint(stats.hashed, ==, 1);
    gchar *digest_z = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "flac-z-yeni", -1);
    g_assert_cmpstr(file_at(files, 4)->sha256, ==, digest_z); g_free(digest_z);
    g_ptr_array_unref(files); g_free(digest);
    /* Cancellation from the progress callback and from the cancellable. */
    files = radios_media_archive_scan(folders, fixture->index, NULL, cancelling, NULL, &stats, &error);
    g_assert_null(files); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED); g_clear_error(&error);
    GCancellable *cancel = g_cancellable_new(); g_cancellable_cancel(cancel);
    files = radios_media_archive_scan(folders, fixture->index, cancel, NULL, NULL, &stats, &error);
    g_assert_null(files); g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED); g_clear_error(&error); g_object_unref(cancel);
    /* Only missing folders: an error, not an empty success. */
    const gchar *none[] = {missing, NULL};
    files = radios_media_archive_scan(none, NULL, NULL, NULL, NULL, &stats, &error);
    g_assert_null(files); g_assert_nonnull(error); g_assert_nonnull(strstr(error->message, "takılı")); g_clear_error(&error);
    /* No folders at all is an empty result. */
    const gchar *empty[] = {NULL};
    files = radios_media_archive_scan(empty, NULL, NULL, NULL, NULL, &stats, &error);
    g_assert_no_error(error); g_assert_nonnull(files); g_assert_cmpuint(files->len, ==, 0); g_ptr_array_unref(files);
    g_free(missing);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add("/media-archive/settings-validate", Fixture, NULL, setup, test_settings_validate, teardown);
    g_test_add("/media-archive/settings-roundtrip", Fixture, NULL, setup, test_settings_roundtrip, teardown);
    g_test_add_func("/media-archive/names-and-purposes", test_names_and_purposes);
    g_test_add("/media-archive/folder-state", Fixture, NULL, setup, test_folder_state, teardown);
    g_test_add("/media-archive/scan", Fixture, NULL, setup, test_scan, teardown);
    return g_test_run();
}
