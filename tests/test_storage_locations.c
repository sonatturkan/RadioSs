#include "radios/storage_locations.h"
#include <glib/gstdio.h>
#include <string.h>

#include "radios_test_support.h"

typedef struct { gchar *temporary, *root, *external; } Fixture;

static void write_file(const gchar *path, const gchar *contents)
{
    gchar *directory = g_path_get_dirname(path); g_assert_cmpint(g_mkdir_with_parents(directory, 0700), ==, 0); g_free(directory);
    GError *error = NULL; g_file_set_contents(path, contents, -1, &error); g_assert_no_error(error);
}

static void setup(Fixture *fixture, gconstpointer data)
{
    (void)data; GError *error = NULL;
    /* Physical (final, long) path: storage folders must equal their realpath. */
    fixture->temporary = rt_make_tmp_dir("radios-storage-XXXXXX", &error); g_assert_no_error(error);
    g_assert_nonnull(fixture->temporary);
    fixture->root = g_build_filename(fixture->temporary, "RadioS proje", NULL);
    fixture->external = g_build_filename(fixture->temporary, "harici disk", NULL);
    const gchar *dirs[] = {"config", "data/database", "data/recordings", "data/backups", NULL};
    for (guint i = 0; dirs[i]; ++i) { gchar *path = g_build_filename(fixture->root, dirs[i], NULL); g_assert_cmpint(g_mkdir_with_parents(path, 0700), ==, 0); g_free(path); }
    g_assert_cmpint(g_mkdir_with_parents(fixture->external, 0700), ==, 0);
}

static void teardown(Fixture *fixture, gconstpointer data)
{
    (void)data; rt_remove_tree(fixture->temporary);
    g_free(fixture->temporary); g_free(fixture->root); g_free(fixture->external);
}

static void config_init(Fixture *fixture, RadioSConfig *config)
{
    memset(config, 0, sizeof *config);
    config->project_root = g_strdup(fixture->root);
    /* One component per argument: a literal "a/b" would leave a mixed-separator path. */
    config->database_file = g_build_filename(fixture->root, "data", "database", "RadioS.sdb", NULL);
    config->recording_root = g_build_filename(fixture->root, "data", "recordings", NULL);
    config->backup_root = g_build_filename(fixture->root, "data", "backups", NULL);
}

static void config_free(RadioSConfig *config)
{ g_free(config->project_root); g_free(config->database_file); g_free(config->recording_root); g_free(config->backup_root); }

static void test_path_syntax(void)
{
    GError *error = NULL;
    /* Absolute canonical Windows folders: a drive path and a UNC share path. */
    const gchar *good[] = {"D:\\Radyo\\Kayıt", "\\\\srv\\share\\Kayıt", NULL};
    for (guint i = 0; good[i]; ++i) {
        g_assert_true(radios_storage_path_check_syntax(good[i], &error)); g_assert_no_error(error);
    }
    /* Relative, drive-relative and rooted-without-drive paths, roots, dot and
     * empty components, a trailing separator, control characters, a stream
     * or reserved device name, '/' separators, a lower-case drive letter and
     * the \\?\ device prefix are not storage folders. */
    const gchar *bad[] = {"", "göreli", "C:", "C:göreli", "\\kök", "/mnt/radyo/kayıt", "C:\\", "\\\\srv\\share",
                          "C:\\a\\..\\b", "C:\\a\\.\\b", "C:\\a\\\\b", "C:\\a\\", "C:\\a\tb", "C:\\a:b",
                          "C:\\NUL\\x", "C:/a", "c:\\a", "\\\\?\\C:\\a", NULL};
    for (guint i = 0; bad[i]; ++i) {
        g_assert_false(radios_storage_path_check_syntax(bad[i], &error)); g_assert_nonnull(error); g_clear_error(&error);
    }
    g_assert_false(radios_storage_path_check_syntax(NULL, &error)); g_clear_error(&error);
    g_assert_cmpstr(radios_storage_role_key(RADIOS_STORAGE_RECORDINGS), ==, "recording_root");
    g_assert_cmpstr(radios_storage_role_key(RADIOS_STORAGE_BACKUPS), ==, "backup_root");
    g_assert_cmpstr(radios_storage_role_key(RADIOS_STORAGE_DATABASE), ==, "database_dir");
    g_assert_cmpstr(radios_storage_role_label(RADIOS_STORAGE_RECORDINGS), ==, "Kayıtlar");
}

static void test_format_bytes(void)
{
    gchar *text = radios_storage_format_bytes(-1); g_assert_cmpstr(text, ==, "—"); g_free(text);
    text = radios_storage_format_bytes(0); g_assert_cmpstr(text, ==, "0 B"); g_free(text);
    text = radios_storage_format_bytes(1536); g_assert_cmpstr(text, ==, "1,5 KB"); g_free(text);
    text = radios_storage_format_bytes(G_GINT64_CONSTANT(3) * 1024 * 1024 * 1024); g_assert_cmpstr(text, ==, "3,0 GB"); g_free(text);
}

static void test_directory_rules(Fixture *fixture, gconstpointer data)
{
    (void)data; GError *error = NULL;
    gchar *missing = g_build_filename(fixture->temporary, "yok", NULL);
    g_assert_false(radios_storage_directory_check(fixture->root, RADIOS_STORAGE_RECORDINGS, missing, FALSE, &error));
    g_assert_nonnull(strstr(error->message, "bulunamadı")); g_clear_error(&error);
    gchar *file = g_build_filename(fixture->temporary, "dosya", NULL); write_file(file, "x");
    g_assert_false(radios_storage_directory_check(fixture->root, RADIOS_STORAGE_RECORDINGS, file, FALSE, &error));
    g_assert_nonnull(strstr(error->message, "klasör değil")); g_clear_error(&error);
    gchar *protected_dir = g_build_filename(fixture->root, "config", NULL);
    g_assert_false(radios_storage_directory_check(fixture->root, RADIOS_STORAGE_RECORDINGS, protected_dir, FALSE, &error));
    g_assert_nonnull(strstr(error->message, "kaynak veya ayar")); g_clear_error(&error);
    /* A directory link (a junction without the symbolic-link privilege). */
    gchar *link = g_build_filename(fixture->temporary, "bağ", NULL);
    g_assert_true(rt_symlink(fixture->external, link, TRUE, &error)); g_assert_no_error(error);
    g_assert_false(radios_storage_directory_check(fixture->root, RADIOS_STORAGE_RECORDINGS, link, FALSE, &error));
    g_assert_nonnull(strstr(error->message, "sembolik")); g_clear_error(&error);
    /* Without the marker the folder passes the plain check but not the mounted-disk check. */
    g_assert_true(radios_storage_directory_check(fixture->root, RADIOS_STORAGE_RECORDINGS, fixture->external, FALSE, &error)); g_assert_no_error(error);
    g_assert_false(radios_storage_directory_check(fixture->root, RADIOS_STORAGE_RECORDINGS, fixture->external, TRUE, &error));
    g_assert_nonnull(strstr(error->message, "işareti yok")); g_clear_error(&error);
    /* prepare writes the marker (0600 JSON) and creates a missing folder. */
    gchar *fresh = g_build_filename(fixture->external, "yeni", "kayıtlar", NULL);
    g_assert_true(radios_storage_directory_prepare(fixture->root, RADIOS_STORAGE_RECORDINGS, fresh, &error)); g_assert_no_error(error);
    gchar *marker = g_build_filename(fresh, RADIOS_STORAGE_MARKER, NULL);
    rt_assert_private_file(marker);
    gchar *body = NULL; g_assert_true(g_file_get_contents(marker, &body, NULL, NULL));
    g_assert_nonnull(strstr(body, "\"role\":\"recordings\"")); g_assert_nonnull(strstr(body, "radios-storage-v1")); g_free(body);
    g_assert_true(radios_storage_directory_check(fixture->root, RADIOS_STORAGE_RECORDINGS, fresh, TRUE, &error)); g_assert_no_error(error);
    /* The same folder prepared again keeps its marker; another role is refused. */
    g_assert_true(radios_storage_directory_prepare(fixture->root, RADIOS_STORAGE_RECORDINGS, fresh, &error)); g_assert_no_error(error);
    g_assert_false(radios_storage_directory_check(fixture->root, RADIOS_STORAGE_BACKUPS, fresh, TRUE, &error));
    g_assert_nonnull(strstr(error->message, "başka amaçla")); g_clear_error(&error);
    g_assert_false(radios_storage_directory_prepare(fixture->root, RADIOS_STORAGE_BACKUPS, fresh, &error)); g_clear_error(&error);
    write_file(marker, "{\"format\":\"eski\"}");
    g_assert_false(radios_storage_directory_check(fixture->root, RADIOS_STORAGE_RECORDINGS, fresh, TRUE, &error));
    g_assert_nonnull(strstr(error->message, "geçersiz")); g_clear_error(&error);
    g_free(missing); g_free(file); g_free(protected_dir); g_free(link); g_free(fresh); g_free(marker);
}

static void test_settings_and_apply(Fixture *fixture, gconstpointer data)
{
    (void)data; GError *error = NULL;
    RadioSStorageSettings settings; radios_storage_settings_init(&settings);
    g_assert_true(radios_storage_settings_load(fixture->root, &settings, &error)); g_assert_no_error(error);
    for (guint role = 0; role < RADIOS_STORAGE_ROLE_COUNT; ++role) g_assert_null(settings.path[role]);
    gchar *recordings = g_build_filename(fixture->external, "kayıtlar", NULL);
    g_assert_true(radios_storage_directory_prepare(fixture->root, RADIOS_STORAGE_RECORDINGS, recordings, &error)); g_assert_no_error(error);
    settings.path[RADIOS_STORAGE_RECORDINGS] = g_strdup(recordings);
    g_assert_true(radios_storage_settings_save(fixture->root, &settings, &error)); g_assert_no_error(error);
    gchar *file = g_build_filename(fixture->root, "config", RADIOS_STORAGE_FILE, NULL);
    rt_assert_private_file(file);
    RadioSStorageSettings loaded; radios_storage_settings_init(&loaded);
    g_assert_true(radios_storage_settings_load(fixture->root, &loaded, &error)); g_assert_no_error(error);
    rt_assert_path_equal(loaded.path[RADIOS_STORAGE_RECORDINGS], recordings); g_assert_null(loaded.path[RADIOS_STORAGE_BACKUPS]);
    RadioSStorageSettings copy; radios_storage_settings_init(&copy); radios_storage_settings_copy(&copy, &loaded);
    rt_assert_path_equal(copy.path[RADIOS_STORAGE_RECORDINGS], recordings); radios_storage_settings_clear(&copy);
    /* apply: recordings move, backups keep the default, the database stays. */
    RadioSConfig config; config_init(fixture, &config);
    gchar *default_database = g_strdup(config.database_file), *default_backups = g_strdup(config.backup_root);
    g_assert_true(radios_storage_apply(&config, &error)); g_assert_no_error(error);
    rt_assert_path_equal(config.recording_root, recordings); rt_assert_path_equal(config.backup_root, default_backups);
    rt_assert_path_equal(config.database_file, default_database); g_assert_false(config.database_external);
    gchar *current = radios_storage_current_dir(&config, RADIOS_STORAGE_RECORDINGS); rt_assert_path_equal(current, recordings); g_free(current);
    current = radios_storage_current_dir(&config, RADIOS_STORAGE_BACKUPS); rt_assert_path_equal(current, default_backups); g_free(current);
    gchar *database_dir = g_path_get_dirname(default_database);
    current = radios_storage_current_dir(&config, RADIOS_STORAGE_DATABASE); rt_assert_path_equal(current, database_dir); g_free(current);
    gchar *notice = radios_storage_notice(&config); g_assert_null(notice);
    /* An unmounted recording disk falls back with a notice; a database folder without the marker refuses to start. */
    gchar *marker = g_build_filename(recordings, RADIOS_STORAGE_MARKER, NULL); g_assert_cmpint(g_remove(marker), ==, 0);
    current = radios_storage_current_dir(&config, RADIOS_STORAGE_RECORDINGS); rt_assert_path_equal(current, config.recording_root); g_free(current);
    notice = radios_storage_notice(&config); g_assert_nonnull(notice); g_assert_nonnull(strstr(notice, "Kayıtlar")); g_assert_nonnull(strstr(notice, "Varsayılan")); g_free(notice);
    config_free(&config); config_init(fixture, &config);
    g_assert_true(radios_storage_apply(&config, &error)); g_assert_no_error(error);
    gchar *default_recordings = g_build_filename(fixture->root, "data", "recordings", NULL);
    rt_assert_path_equal(config.recording_root, default_recordings); g_free(default_recordings); /* unchanged default */
    gchar *database_external = g_build_filename(fixture->external, "veritabanı", NULL);
    g_assert_cmpint(g_mkdir_with_parents(database_external, 0700), ==, 0);
    loaded.path[RADIOS_STORAGE_DATABASE] = g_strdup(database_external);
    g_assert_true(radios_storage_settings_save(fixture->root, &loaded, &error)); g_assert_no_error(error);
    config_free(&config); config_init(fixture, &config);
    g_assert_false(radios_storage_apply(&config, &error)); g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "Veritabanı klasörü kullanılamıyor")); g_clear_error(&error);
    rt_assert_path_equal(config.database_file, default_database);
    g_assert_true(radios_storage_directory_prepare(fixture->root, RADIOS_STORAGE_DATABASE, database_external, &error)); g_assert_no_error(error);
    config_free(&config); config_init(fixture, &config);
    g_assert_true(radios_storage_apply(&config, &error)); g_assert_no_error(error);
    gchar *moved = g_build_filename(database_external, "RadioS.sdb", NULL);
    rt_assert_path_equal(config.database_file, moved); g_assert_true(config.database_external); g_free(moved);
    /* A broken storage.ini is a configuration error. */
    write_file(file, "[storage]\nversion=1\nrecording_root=göreli\n");
    config_free(&config); config_init(fixture, &config);
    g_assert_false(radios_storage_apply(&config, &error)); g_assert_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID); g_clear_error(&error);
    notice = radios_storage_notice(&config); g_assert_nonnull(notice); g_assert_nonnull(strstr(notice, "storage.ini")); g_free(notice);
    config_free(&config);
    radios_storage_settings_clear(&settings); radios_storage_settings_clear(&loaded);
    g_free(recordings); g_free(file); g_free(default_database); g_free(default_backups); g_free(database_dir); g_free(marker); g_free(database_external);
}

static void test_sizes(Fixture *fixture, gconstpointer data)
{
    (void)data; GError *error = NULL;
    gchar *directory = g_build_filename(fixture->external, "a", NULL);
    gchar *file = g_build_filename(directory, "b.wav", NULL); write_file(file, "0123456789");
    /* A directory link (junction without the symbolic-link privilege) to the
     * folder holding the only file: following it would count 20 bytes. */
    gchar *link = g_build_filename(fixture->external, "bağ", NULL);
    g_assert_true(rt_symlink(directory, link, TRUE, &error)); g_assert_no_error(error);
    g_assert_cmpint(radios_storage_tree_bytes(fixture->external), ==, 10); /* links are not followed */
    g_assert_cmpint(radios_storage_tree_bytes(file), ==, -1); g_assert_cmpint(radios_storage_tree_bytes(NULL), ==, -1);
    g_assert_cmpint(radios_storage_free_bytes(fixture->external), >, 0);
    g_assert_cmpint(radios_storage_total_bytes(fixture->external), >=, radios_storage_free_bytes(fixture->external));
    g_assert_cmpint(radios_storage_free_bytes("/yok/böyle/bir/yer"), ==, -1);
    gchar *missing = g_build_filename(fixture->temporary, "yok", "böyle", "bir", "yer", NULL);
    g_assert_cmpint(radios_storage_free_bytes(missing), ==, -1); g_free(missing);
    g_free(directory); g_free(file); g_free(link);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/storage/path-syntax", test_path_syntax);
    g_test_add_func("/storage/format-bytes", test_format_bytes);
    g_test_add("/storage/directory-rules", Fixture, NULL, setup, test_directory_rules, teardown);
    g_test_add("/storage/settings-and-apply", Fixture, NULL, setup, test_settings_and_apply, teardown);
    g_test_add("/storage/sizes", Fixture, NULL, setup, test_sizes, teardown);
    return g_test_run();
}
