#define _XOPEN_SOURCE 700

#include "radios/config.h"

#include <glib/gstdio.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    gchar *temporary;
    gchar *root;
    gchar *example;
} ConfigFixture;

static const gchar template[] =
    "[application]\n"
    "config_version=1\nname=RadioS\ninitial_profile=broadcast\n"
    "auto_start_playback=false\n"
    "[paths]\nassets_root=assets\ndatabase_file=data/database/RadioS.sdb\n"
    "endpoint_root=data/endpoints\nimport_root=data/imports\n"
    "cache_root=data/cache\nlog_root=data/logs\n"
    "recording_root=data/recordings\nbackup_root=data/backups\n"
    "[database]\nsource_dir=database/SDBASE\n"
    "service_url=http://127.0.0.1:8000\npython_command=python3\n"
    "token_environment=RADIOS_SDBASE_TOKEN\n"
    "connect_timeout_seconds=5\nrequest_timeout_seconds=30\n";

static void
remove_tree(const gchar *path)
{
    if (g_file_test(path, G_FILE_TEST_IS_SYMLINK) ||
        !g_file_test(path, G_FILE_TEST_IS_DIR)) {
        g_remove(path);
        return;
    }
    GDir *directory = g_dir_open(path, 0, NULL);
    g_assert_nonnull(directory);
    const gchar *name;
    while ((name = g_dir_read_name(directory)) != NULL) {
        gchar *child = g_build_filename(path, name, NULL);
        remove_tree(child);
        g_free(child);
    }
    g_dir_close(directory);
    g_assert_cmpint(g_rmdir(path), ==, 0);
}

static void
write_file(const gchar *path, const gchar *contents)
{
    GError *error = NULL;
    g_assert_true(g_file_set_contents(path, contents, -1, &error));
    g_assert_no_error(error);
}

static void
fixture_setup(ConfigFixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    (void)data;
    fixture->temporary = g_dir_make_tmp("radios-config-XXXXXX", &error);
    g_assert_no_error(error);
    fixture->root = g_build_filename(fixture->temporary,
                                     "Türkçe kaynak kodlar RadioS", NULL);
    gchar *config_dir = g_build_filename(fixture->root, "config", NULL);
    gchar *database_dir = g_build_filename(fixture->root, "database",
                                           "SDBASE", NULL);
    g_assert_cmpint(g_mkdir_with_parents(config_dir, 0700), ==, 0);
    g_assert_cmpint(g_mkdir_with_parents(database_dir, 0700), ==, 0);
    fixture->example = g_build_filename(config_dir, "radios.example.ini", NULL);
    write_file(fixture->example, template);
    gchar *marker = g_build_filename(database_dir, "pyproject.toml", NULL);
    write_file(marker, "[project]\nname='fixture-sdbase'\n");
    g_free(marker);
    g_free(config_dir);
    g_free(database_dir);
}

static void
fixture_teardown(ConfigFixture *fixture, gconstpointer data)
{
    (void)data;
    remove_tree(fixture->temporary);
    g_free(fixture->temporary);
    g_free(fixture->root);
    g_free(fixture->example);
}

static gchar *
with_value(const gchar *group, const gchar *key, const gchar *value)
{
    GKeyFile *file = g_key_file_new();
    g_assert_true(g_key_file_load_from_data(file, template, -1,
                                           G_KEY_FILE_NONE, NULL));
    g_key_file_set_value(file, group, key, value);
    gchar *contents = g_key_file_to_data(file, NULL, NULL);
    g_key_file_unref(file);
    return contents;
}

static void
test_defaults(ConfigFixture *fixture, gconstpointer data)
{
    RadioSConfig config = {0};
    GError *error = NULL;
    (void)data;
    g_assert_true(radios_config_load(fixture->root, NULL, &config, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(config.project_root, ==, fixture->root);
    g_assert_cmpstr(config.config_file, ==, fixture->example);
    g_assert_cmpstr(config.name, ==, "RadioS");
    g_assert_cmpstr(config.initial_profile, ==, "broadcast");
    g_assert_false(config.auto_start_playback);
    g_assert_cmpint(config.connect_timeout_seconds, ==, 5);
    g_assert_cmpint(config.request_timeout_seconds, ==, 30);
    g_assert_cmpstr(config.token_environment, ==, "RADIOS_SDBASE_TOKEN");
    gchar *expected = g_build_filename(fixture->root, "data", "database",
                                      "RadioS.sdb", NULL);
    g_assert_cmpstr(config.database_file, ==, expected);
    g_assert_false(g_file_test(config.database_file, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(config.backup_root, G_FILE_TEST_EXISTS));
    g_free(expected);
    radios_config_clear(&config);
    g_assert_null(config.project_root);
    radios_config_clear(&config);
}

static void
test_ini_comment_formats(ConfigFixture *fixture, gconstpointer data)
{
    const gchar *comments =
        "; RadioS yapılandırması — tam satır yorumu\n"
        "  ; Boşlukla başlayan [ignored] key=value yorumu\n"
        "\t; Sekmeyle başlayan yorum\n"
        "# GKeyFile yorumu da geçerli\n";
    gchar *values = with_value("application", "name", "RadioS; Yayın");
    gchar *lf = g_strconcat(comments, values,
                            "\n ; Son satır yorumu; değer değil", NULL);
    GString *crlf = g_string_new(NULL);
    (void)data;
    for (const gchar *p = lf; *p != '\0'; ++p) {
        if (*p == '\n')
            g_string_append(crlf, "\r\n");
        else
            g_string_append_c(crlf, *p);
    }
    const gchar *formats[] = {lf, crlf->str};
    for (gsize i = 0; i < G_N_ELEMENTS(formats); ++i) {
        RadioSConfig config = {0};
        GError *error = NULL;
        write_file(fixture->example, formats[i]);
        g_assert_true(radios_config_load(fixture->root, NULL, &config, &error));
        g_assert_no_error(error);
        g_assert_cmpstr(config.name, ==, "RadioS; Yayın");
        g_assert_cmpstr(config.initial_profile, ==, "broadcast");
        radios_config_clear(&config);
    }
    g_string_free(crlf, TRUE);
    g_free(lf);
    g_free(values);
}

static void
test_local_config(ConfigFixture *fixture, gconstpointer data)
{
    RadioSConfig config = {0};
    GError *error = NULL;
    (void)data;
    gchar *local = g_build_filename(fixture->root, "config", "radios.ini", NULL);
    gchar *contents = with_value("application", "initial_profile", "routing");
    write_file(local, contents);
    g_assert_true(radios_config_load(fixture->root, NULL, &config, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(config.config_file, ==, local);
    g_assert_cmpstr(config.initial_profile, ==, "routing");
    radios_config_clear(&config);
    g_assert_true(radios_config_load(fixture->root, "config/radios.example.ini",
                                     &config, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(config.initial_profile, ==, "broadcast");
    radios_config_clear(&config);
    g_free(local);
    g_free(contents);
}

static void
test_invalid_fields(ConfigFixture *fixture, gconstpointer data)
{
    static const struct { const gchar *group, *key, *value; } cases[] = {
        {"application", "config_version", "2"},
        {"application", "config_version", "1garbage"},
        {"application", "name", ""},
        {"application", "initial_profile", "web"},
        {"application", "auto_start_playback", "perhaps"},
        {"database", "connect_timeout_seconds", "0"},
        {"database", "request_timeout_seconds", "3601"},
        {"database", "request_timeout_seconds", "3"},
        {"database", "python_command", ""},
        {"database", "token_environment", "BAD TOKEN"},
        {"database", "service_url", "ftp://localhost:8000"},
        {"database", "service_url", "http://localhost:0"},
        {"database", "service_url", "http://localhost:65536"},
        {"database", "service_url", "http://user:secret@localhost:8000"},
        {"database", "service_url", "http://localhost:8000/v1"},
        {"database", "service_url", "http://localhost:8000?x=1"},
        {"database", "service_url", "http://bad host:8000"},
        {"database", "service_url", "http://bad^host:8000"},
        {"database", "service_url", "http://-badhost:8000"},
        {"database", "service_url", "http://999.999.999.999:8000"},
        {"paths", "assets_root", ""}
    };
    (void)data;
    for (gsize i = 0; i < G_N_ELEMENTS(cases); ++i) {
        GError *error = NULL;
        RadioSConfig config = {0};
        gchar *contents = with_value(cases[i].group, cases[i].key, cases[i].value);
        write_file(fixture->example, contents);
        g_test_message("Reject [%s] %s", cases[i].group, cases[i].key);
        g_assert_false(radios_config_load(fixture->root, NULL, &config, &error));
        g_assert_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID);
        g_assert_null(config.project_root);
        g_clear_error(&error);
        g_free(contents);
    }
}

static void
test_service_urls(ConfigFixture *fixture, gconstpointer data)
{
    const gchar *urls[] = {
        "http://localhost:8000", "http://[::1]:8000/",
        "https://radio.example.org", "https://radio.example.org.:8443"
    };
    (void)data;
    for (gsize i = 0; i < G_N_ELEMENTS(urls); ++i) {
        RadioSConfig config = {0};
        GError *error = NULL;
        gchar *contents = with_value("database", "service_url", urls[i]);
        write_file(fixture->example, contents);
        g_assert_true(radios_config_load(fixture->root, NULL, &config, &error));
        g_assert_no_error(error);
        g_assert_cmpstr(config.service_url, ==, urls[i]);
        radios_config_clear(&config);
        g_free(contents);
    }
}

static void
test_missing_or_malformed_config(ConfigFixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    RadioSConfig config = {0};
    (void)data;
    g_assert_false(radios_config_load(fixture->root, "config/missing.ini",
                                      &config, &error));
    g_assert_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_IO);
    g_clear_error(&error);
    write_file(fixture->example, "[application]\nconfig_version=1\n");
    g_assert_false(radios_config_load(fixture->root, NULL, &config, &error));
    g_assert_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID);
    g_clear_error(&error);
    write_file(fixture->example, "[broken\nnot ini");
    g_assert_false(radios_config_load(fixture->root, NULL, &config, &error));
    g_assert_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_IO);
    g_clear_error(&error);
}

static void
test_paths_stay_inside(ConfigFixture *fixture, gconstpointer data)
{
    const gchar *escapes[] = {"../escape", "/tmp/radios-escape", "../../outside/file"};
    (void)data;
    for (gsize i = 0; i < G_N_ELEMENTS(escapes); ++i) {
        GError *error = NULL;
        RadioSConfig config = {0};
        gchar *contents = with_value("paths", "database_file", escapes[i]);
        write_file(fixture->example, contents);
        g_assert_false(radios_config_load(fixture->root, NULL, &config, &error));
        g_assert_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID);
        g_clear_error(&error);
        g_free(contents);
    }
    GError *error = NULL;
    RadioSConfig config = {0};
    write_file(fixture->example, template);
    g_assert_false(radios_config_load(fixture->root, "/tmp/radios-config.ini",
                                      &config, &error));
    g_assert_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID);
    g_clear_error(&error);
}

static void
test_symlink_paths(ConfigFixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    RadioSConfig config = {0};
    (void)data;
    gchar *outside = g_build_filename(fixture->temporary, "outside", NULL);
    gchar *link = g_build_filename(fixture->root, "external", NULL);
    g_assert_cmpint(g_mkdir(outside, 0700), ==, 0);
    g_assert_cmpint(symlink(outside, link), ==, 0);
    gchar *contents = with_value("paths", "backup_root", "external/not-created");
    write_file(fixture->example, contents);
    g_assert_false(radios_config_load(fixture->root, NULL, &config, &error));
    g_assert_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID);
    g_clear_error(&error);
    g_free(contents);
    g_assert_cmpint(g_remove(link), ==, 0);
    gchar *inside = g_build_filename(fixture->root, "assets", NULL);
    g_assert_cmpint(g_mkdir(inside, 0700), ==, 0);
    g_assert_cmpint(symlink(inside, link), ==, 0);
    contents = with_value("paths", "assets_root", "external");
    write_file(fixture->example, contents);
    g_assert_true(radios_config_load(fixture->root, NULL, &config, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(config.assets_root, ==, inside);
    radios_config_clear(&config);
    g_free(contents);
    g_free(inside);
    g_free(link);
    g_free(outside);
}

static void
test_invalid_local_symlink(ConfigFixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    RadioSConfig config = {0};
    (void)data;
    gchar *local = g_build_filename(fixture->root, "config", "radios.ini", NULL);
    gchar *outside = g_build_filename(fixture->temporary, "outside.ini", NULL);
    write_file(outside, template);
    g_assert_cmpint(symlink(outside, local), ==, 0);
    g_assert_false(radios_config_load(fixture->root, NULL, &config, &error));
    g_assert_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID);
    g_clear_error(&error);
    g_assert_cmpint(g_remove(local), ==, 0);
    g_assert_cmpint(symlink("missing.ini", local), ==, 0);
    g_assert_false(radios_config_load(fixture->root, NULL, &config, &error));
    g_assert_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_IO);
    g_clear_error(&error);
    g_free(local);
    g_free(outside);
}

static void
test_file_types(ConfigFixture *fixture, gconstpointer data)
{
    RadioSConfig config = {0};
    GError *error = NULL;
    (void)data;
    gchar *asset = g_build_filename(fixture->root, "assets", NULL);
    write_file(asset, "not a directory");
    g_assert_false(radios_config_load(fixture->root, NULL, &config, &error));
    g_assert_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID);
    g_clear_error(&error);
    g_assert_cmpint(g_remove(asset), ==, 0);
    gchar *database = g_build_filename(fixture->root, "data", "database",
                                       "RadioS.sdb", NULL);
    g_assert_cmpint(g_mkdir_with_parents(database, 0700), ==, 0);
    g_assert_false(radios_config_load(fixture->root, NULL, &config, &error));
    g_assert_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID);
    g_clear_error(&error);
    g_free(asset);
    g_free(database);
}

static void
test_root_discovery(ConfigFixture *fixture, gconstpointer data)
{
    GError *error = NULL;
    (void)data;
    gchar *build = g_build_filename(fixture->root, "build", "nested", NULL);
    g_assert_cmpint(g_mkdir_with_parents(build, 0700), ==, 0);
    gchar *executable = g_build_filename(build, "radios", NULL);
    write_file(executable, "test executable path marker");
    gchar *root = radios_project_root_find(NULL, executable, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(root, ==, fixture->root);
    g_free(root);
    root = radios_project_root_find(fixture->temporary, executable, &error);
    g_assert_null(root);
    g_assert_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID);
    g_clear_error(&error);
    root = radios_project_root_find("", executable, &error);
    g_assert_null(root);
    g_assert_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID);
    g_clear_error(&error);
    g_free(executable);
    g_free(build);
}

static void
test_cwd_independence(ConfigFixture *fixture, gconstpointer data)
{
    RadioSConfig config = {0};
    GError *error = NULL;
    (void)data;
    gchar *original = g_get_current_dir();
    g_assert_cmpint(g_chdir(fixture->temporary), ==, 0);
    const gchar *relative_root = "Türkçe kaynak kodlar RadioS";
    g_assert_true(radios_config_load(relative_root, NULL, &config, &error));
    g_assert_no_error(error);
    gchar *expected = g_build_filename(fixture->root, "assets", NULL);
    g_assert_cmpstr(config.assets_root, ==, expected);
    g_assert_cmpint(g_chdir(original), ==, 0);
    radios_config_clear(&config);
    g_free(original);
    g_free(expected);
}

int
main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
#define ADD_TEST(name, function) \
    g_test_add("/config/" name, ConfigFixture, NULL, fixture_setup, \
               function, fixture_teardown)
    ADD_TEST("defaults-without-side-effects", test_defaults);
    ADD_TEST("ini-comment-formats", test_ini_comment_formats);
    ADD_TEST("local-file-and-explicit-selection", test_local_config);
    ADD_TEST("invalid-fields", test_invalid_fields);
    ADD_TEST("service-url-formats", test_service_urls);
    ADD_TEST("missing-or-malformed", test_missing_or_malformed_config);
    ADD_TEST("paths-stay-inside", test_paths_stay_inside);
    ADD_TEST("symlink-paths", test_symlink_paths);
    ADD_TEST("invalid-local-symlink", test_invalid_local_symlink);
    ADD_TEST("file-types", test_file_types);
    ADD_TEST("root-discovery", test_root_discovery);
    ADD_TEST("cwd-independence", test_cwd_independence);
#undef ADD_TEST
    return g_test_run();
}
