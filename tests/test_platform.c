/* Tests for the Windows platform layer (include/radios/platform.h).
 * The executable doubles as its own helper process: "--platform-helper <mode> ..."
 * is used for the cross-process lock, stop-event, relaunch, log and crash tests. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shellapi.h>
#include <winioctl.h>

#include "radios/platform.h"

#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HELPER_FLAG "--platform-helper"
#define TURKISH "Türkçe ğüşiöç"

/* ------------------------------------------------------------------ helpers */

static gchar *project_root(void)
{
#ifdef RADIOS_TEST_PROJECT_ROOT
    return radios_path_normalize(RADIOS_TEST_PROJECT_ROOT);
#else
    const gchar *env = g_getenv("RADIOS_TEST_PROJECT_ROOT");
    return env && *env ? radios_path_normalize(env) : NULL;
#endif
}

static gchar *python_path(void)
{
    gchar *root = project_root();
    gchar *python = root ? g_build_filename(root, "runtime", "python", "python.exe", NULL) : NULL;
    g_free(root);
    if (python && !g_file_test(python, G_FILE_TEST_IS_REGULAR)) g_clear_pointer(&python, g_free);
    return python;
}

static gchar *make_tmp_dir(void)
{
    GError *error = NULL;
    gchar *made = g_dir_make_tmp("radios-platform-ğüş-XXXXXX", &error);
    g_assert_no_error(error);
    gchar *real = radios_fs_realpath(made, &error);
    g_assert_no_error(error);
    g_free(made);
    return real;
}

static void remove_tree(const gchar *path)
{
    RadioSFileInfo info;
    if (radios_fs_stat(path, FALSE, &info) != 0) return;
    if (info.type == RADIOS_FILE_TYPE_LINK) {
        g_assert_cmpint(radios_fs_unlinkat(RADIOS_AT_FDCWD, path,
                                           (info.attributes & FILE_ATTRIBUTE_DIRECTORY) ? RADIOS_AT_REMOVEDIR : 0), ==, 0);
        return;
    }
    if (info.type == RADIOS_FILE_TYPE_DIRECTORY) {
        gint fd = radios_fs_open(path, RADIOS_O_RDONLY | RADIOS_O_DIRECTORY | RADIOS_O_NOFOLLOW, 0);
        g_assert_cmpint(fd, >=, 0);
        GPtrArray *names = radios_fd_list_names(fd, NULL);
        g_assert_nonnull(names);
        close(fd);
        for (guint i = 0; i < names->len; ++i) {
            gchar *child = g_build_filename(path, names->pdata[i], NULL);
            remove_tree(child);
            g_free(child);
        }
        g_ptr_array_unref(names);
        g_assert_cmpint(radios_fs_unlinkat(RADIOS_AT_FDCWD, path, RADIOS_AT_REMOVEDIR), ==, 0);
        return;
    }
    g_assert_cmpint(radios_fs_unlink(path), ==, 0);
}

static void write_file(const gchar *path, const gchar *contents, guint mode)
{
    gint fd = radios_fs_open(path, RADIOS_O_WRONLY | RADIOS_O_CREAT | RADIOS_O_TRUNC, mode);
    g_assert_cmpint(fd, >=, 0);
    gsize length = strlen(contents);
    g_assert_cmpint(write(fd, contents, (unsigned)length), ==, (gint)length);
    g_assert_cmpint(radios_fd_sync(fd), ==, 0);
    g_assert_cmpint(close(fd), ==, 0);
}

static gchar *read_fd(gint fd)
{
    GString *text = g_string_new(NULL);
    gchar buffer[4096];
    for (;;) {
        gint n = read(fd, buffer, sizeof buffer);
        if (n <= 0) break;
        g_string_append_len(text, buffer, n);
    }
    return g_string_free(text, FALSE);
}

static gchar *read_file(const gchar *path)
{
    gint fd = radios_fs_open(path, RADIOS_O_RDONLY, 0);
    if (fd < 0) return NULL;
    gchar *text = read_fd(fd);
    close(fd);
    return text;
}

static gchar *self_path(void)
{
    GError *error = NULL;
    gchar *path = radios_process_executable_path(&error);
    g_assert_no_error(error);
    g_assert_true(radios_path_is_absolute(path));
    return path;
}

/* argv = [self, HELPER_FLAG, args...] */
static gchar **helper_argv(const gchar *first, ...)
{
    GPtrArray *argv = g_ptr_array_new();
    g_ptr_array_add(argv, self_path());
    g_ptr_array_add(argv, g_strdup(HELPER_FLAG));
    va_list args;
    va_start(args, first);
    for (const gchar *arg = first; arg; arg = va_arg(args, const gchar *)) g_ptr_array_add(argv, g_strdup(arg));
    va_end(args);
    g_ptr_array_add(argv, NULL);
    return (gchar **)g_ptr_array_free(argv, FALSE);
}

/* Runs a helper, returns stdout text; *exit_code receives the exit code. */
static gchar *run_helper(gchar **argv, guint timeout_ms, gint *exit_code)
{
    GError *error = NULL;
    RadioSProcess *process = radios_process_spawn((const gchar *const *)argv, NULL, NULL,
                                                  RADIOS_SPAWN_STDOUT_PIPE | RADIOS_SPAWN_STDERR_PIPE,
                                                  NULL, NULL, NULL, &error);
    g_assert_no_error(error);
    GBytes *out = NULL, *err = NULL;
    gboolean ok = radios_process_communicate(process, NULL, 1024 * 1024, timeout_ms, &out, &err, exit_code, NULL, &error);
    g_assert_no_error(error);
    g_assert_true(ok);
    gsize size = 0;
    const gchar *data = g_bytes_get_data(err, &size);
    if (size) g_test_message("helper stderr: %.*s", (int)size, data);
    data = g_bytes_get_data(out, &size);
    gchar *text = g_strndup(data, size);
    g_bytes_unref(out);
    g_bytes_unref(err);
    radios_process_free(process);
    return text;
}

static gint64 offset_at(GTimeZone *tz, gint year, gint month, gint day, gint hour, gint minute, gint second)
{
    GDateTime *utc = g_date_time_new_utc(year, month, day, hour, minute, (gdouble)second);
    GDateTime *local = g_date_time_to_timezone(utc, tz);
    gint64 offset = g_date_time_get_utc_offset(local) / G_TIME_SPAN_SECOND;
    g_date_time_unref(local);
    g_date_time_unref(utc);
    return offset;
}

/* ------------------------------------------------------------------ path strings */

static void test_path_absolute_root(void)
{
    static const struct { const gchar *path; gboolean absolute, root; } cases[] = {
        { "C:\\x", TRUE, FALSE }, { "c:/x", TRUE, FALSE }, { "C:\\", TRUE, TRUE }, { "C:/", TRUE, TRUE },
        { "z:\\", TRUE, TRUE }, { "C:", FALSE, FALSE }, { "C:x", FALSE, FALSE }, { "\\x", FALSE, FALSE },
        { "/x", FALSE, FALSE }, { "/", FALSE, FALSE }, { "x\\y", FALSE, FALSE }, { "", FALSE, FALSE },
        { "\\\\server\\share", TRUE, TRUE }, { "\\\\server\\share\\", TRUE, TRUE }, { "//server/share/", TRUE, TRUE },
        { "\\\\server\\share\\x", TRUE, FALSE }, { "\\\\server", FALSE, FALSE }, { "\\\\server\\", FALSE, FALSE },
        { "\\\\?\\C:\\x", FALSE, FALSE }, { "\\\\.\\pipe\\x", FALSE, FALSE }, { "D:\\radyo\\kayıt", TRUE, FALSE },
        { "1:\\x", FALSE, FALSE },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); ++i) {
        g_assert_cmpint(radios_path_is_absolute(cases[i].path), ==, cases[i].absolute);
        g_assert_cmpint(radios_path_is_root(cases[i].path), ==, cases[i].root);
    }
    g_assert_false(radios_path_is_absolute(NULL));
    g_assert_false(radios_path_is_root(NULL));
}

static void test_path_equal_inside(void)
{
    g_assert_true(radios_path_equal("C:\\Müzik\\Şarkı", "c:/MÜZİK/şARKI") == radios_path_equal("İ", "i"));
    g_assert_true(radios_path_equal("C:\\Müzik\\A", "c:/müzik/a"));
    g_assert_true(radios_path_equal("C:\\Kayıt\\Ğ", "C:/kayıt/ğ"));
    g_assert_true(radios_path_equal("\\\\Srv\\Share\\x", "//srv/SHARE/X"));
    g_assert_false(radios_path_equal("C:\\a", "C:\\a\\"));
    g_assert_false(radios_path_equal("C:\\a", "D:\\a"));
    g_assert_false(radios_path_equal("C:\\a", NULL));
    static const struct { const gchar *root, *path; gboolean inside; } cases[] = {
        { "C:\\", "C:\\x", TRUE }, { "C:\\", "C:\\", TRUE }, { "C:/", "c:\\x\\y", TRUE },
        { "C:\\a", "C:\\a", TRUE }, { "C:\\a", "C:\\ab", FALSE }, { "C:\\a", "c:/A/b", TRUE },
        { "C:\\a\\", "C:\\a\\b", TRUE }, { "C:\\a\\", "C:\\a", TRUE }, { "C:\\a", "C:\\", FALSE },
        { "\\\\srv\\share", "\\\\SRV\\Share\\x", TRUE }, { "\\\\srv\\share", "\\\\srv\\shared", FALSE },
        { "\\\\srv\\share\\", "\\\\srv\\share\\x", TRUE }, { "D:\\a", "C:\\a\\b", FALSE },
        { "C:\\Müzik", "c:\\MÜZIK\\Şarkı.wav", TRUE }, { "C:\\Müzik", "C:\\Müzikler", FALSE },
        { "C:\\radios\\database\\SDBASE", "C:/RadioS/Database/sdbase/src", TRUE },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); ++i) {
        if (radios_path_is_inside(cases[i].root, cases[i].path) != cases[i].inside)
            g_error("is_inside(%s, %s) != %d", cases[i].root, cases[i].path, cases[i].inside);
    }
}

/* radios_path_equal() must fold case exactly like NTFS does (Turkish letters). */
static void test_path_case_matches_ntfs(void)
{
    gchar *dir = make_tmp_dir();
    static const gchar *pairs[][2] = {
        { "ı", "I" }, { "i", "I" }, { "İ", "i" }, { "ı", "i" }, { "ş", "Ş" }, { "ğ", "Ğ" }, { "ü", "Ü" },
        { "ç", "Ç" }, { "ö", "Ö" }, { "ß", "SS" }, { "a", "A" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(pairs); ++i) {
        gchar *name_a = g_strconcat("case-", pairs[i][0], ".txt", NULL);
        gchar *name_b = g_strconcat("case-", pairs[i][1], ".txt", NULL);
        gchar *a = g_build_filename(dir, name_a, NULL), *b = g_build_filename(dir, name_b, NULL);
        write_file(a, "x", 0644);
        RadioSFileInfo info;
        gboolean found = radios_fs_stat(b, FALSE, &info) == 0;
        if (found != radios_path_equal(a, b)) g_error("NTFS and radios_path_equal disagree on %s / %s", pairs[i][0], pairs[i][1]);
        g_assert_cmpint(radios_fs_unlink(a), ==, 0);
        g_free(a); g_free(b); g_free(name_a); g_free(name_b);
    }
    remove_tree(dir);
    g_free(dir);
}

static void test_path_normalize(void)
{
    static const gchar *cases[][2] = {
        { "C:/a", "C:\\a" }, { "c:\\a", "C:\\a" }, { "C:\\a\\", "C:\\a" }, { "C:\\a\\\\b", "C:\\a\\b" },
        { "C:\\a\\..\\b", "C:\\b" }, { "C:\\a\\.\\b", "C:\\a\\b" }, { "C:\\", "C:\\" }, { "c:/", "C:\\" },
        { "\\\\?\\C:\\a", "C:\\a" }, { "\\\\?\\UNC\\srv\\share\\x", "\\\\srv\\share\\x" },
        { "\\\\srv\\share\\x\\..\\y", "\\\\srv\\share\\y" }, { "D:\\radyo\\kayıt\\", "D:\\radyo\\kayıt" },
        { "D:/Müzik//Türkçe/", "D:\\Müzik\\Türkçe" },
    };
    for (guint i = 0; i < G_N_ELEMENTS(cases); ++i) {
        gchar *normal = radios_path_normalize(cases[i][0]);
        if (g_strcmp0(normal, cases[i][1]) != 0) g_error("normalize(%s) = %s, expected %s", cases[i][0], normal, cases[i][1]);
        g_free(normal);
    }
    gchar *cwd = g_get_current_dir(), *relative = radios_path_normalize("x\\y"), *expected = g_build_filename(cwd, "x", "y", NULL);
    gchar *expected_normal = radios_path_normalize(expected);
    g_assert_cmpstr(relative, ==, expected_normal);
    g_free(cwd); g_free(relative); g_free(expected); g_free(expected_normal);
}

static void test_path_components(void)
{
    static const gchar *valid[] = { "a", "kayıt.wav", TURKISH, ".hidden", "a b", "COM10", "CONSOLE", "NULL", "LPT", "x.y.z", "İSTANBUL" };
    static const gchar *invalid[] = { "", ".", "..", "a/b", "a\\b", "a:b", "a*", "a?", "a\"b", "a<", "a>", "a|b", "a\tb",
                                      "trailing.", "trailing ", "CON", "con", "Con.txt", "NUL.wav", "COM1", "lpt9.log",
                                      "COM¹", "LPT²", "com³.txt", "CONIN$", "AUX .txt", "prn" };
    for (guint i = 0; i < G_N_ELEMENTS(valid); ++i)
        if (!radios_path_component_valid(valid[i])) g_error("component should be valid: %s", valid[i]);
    for (guint i = 0; i < G_N_ELEMENTS(invalid); ++i)
        if (radios_path_component_valid(invalid[i])) g_error("component should be invalid: %s", invalid[i]);
    g_assert_false(radios_path_component_valid(NULL));
    g_assert_false(radios_path_component_valid("\xff\xfe"));
    gchar *long_ok = g_strnfill(255, 'a'), *long_bad = g_strnfill(256, 'a');
    g_assert_true(radios_path_component_valid(long_ok));
    g_assert_false(radios_path_component_valid(long_bad));
    GString *turkish = g_string_new(NULL);
    for (guint i = 0; i < 200; ++i) g_string_append(turkish, "ü");   /* 400 bytes, 200 UTF-16 units */
    g_assert_true(radios_path_component_valid(turkish->str));
    g_string_free(turkish, TRUE);
    g_free(long_ok); g_free(long_bad);

    gchar **parts = radios_path_split("a/b\\\\c//");
    g_assert_cmpuint(g_strv_length(parts), ==, 3);
    g_assert_cmpstr(parts[0], ==, "a"); g_assert_cmpstr(parts[1], ==, "b"); g_assert_cmpstr(parts[2], ==, "c");
    g_strfreev(parts);
    parts = radios_path_split("");
    g_assert_cmpuint(g_strv_length(parts), ==, 0);
    g_strfreev(parts);
    gchar *portable = radios_path_to_portable("data\\database\\RadioS.sdb");
    g_assert_cmpstr(portable, ==, "data/database/RadioS.sdb");
    gchar *native = radios_path_from_portable(portable);
    g_assert_cmpstr(native, ==, "data\\database\\RadioS.sdb");
    g_free(portable); g_free(native);
    RadioSFileInfo hidden = { 0 };
    hidden.attributes = FILE_ATTRIBUTE_HIDDEN;
    g_assert_true(radios_file_info_is_hidden("$RECYCLE.BIN", &hidden));
    g_assert_true(radios_file_info_is_hidden(".git", NULL));
    hidden.attributes = FILE_ATTRIBUTE_NORMAL;
    g_assert_false(radios_file_info_is_hidden("Müzik", &hidden));
}

/* ------------------------------------------------------------------ file system */

static void test_fs_turkish_names(void)
{
    gchar *dir = make_tmp_dir();
    gchar *sub = g_build_filename(dir, "Kayıtlar " TURKISH, NULL);
    g_assert_cmpint(radios_fs_mkdir_with_parents(sub, 0755), ==, 0);
    gchar *file = g_build_filename(sub, "şarkı.wav", NULL), *renamed = g_build_filename(sub, "Şarkı ÇĞÜ İı.wav", NULL);
    write_file(file, "veri", 0644);
    RadioSFileInfo info;
    g_assert_cmpint(radios_fs_stat(file, FALSE, &info), ==, 0);
    g_assert_cmpint(info.type, ==, RADIOS_FILE_TYPE_REGULAR);
    g_assert_cmpint(info.size, ==, 4);
    g_assert_cmpint(info.nlink, ==, 1);
    g_assert_cmpint(radios_fs_rename(file, renamed), ==, 0);
    g_assert_cmpint(radios_fs_stat(file, FALSE, &info), ==, -1);
    g_assert_cmpint(errno, ==, ENOENT);
    gchar *contents = read_file(renamed);
    g_assert_cmpstr(contents, ==, "veri");
    g_free(contents);
    gint fd = radios_fs_open(sub, RADIOS_O_RDONLY | RADIOS_O_DIRECTORY, 0);
    g_assert_cmpint(fd, >=, 0);
    GPtrArray *names = radios_fd_list_names(fd, NULL);
    g_assert_cmpuint(names->len, ==, 1);
    g_assert_cmpstr(names->pdata[0], ==, "Şarkı ÇĞÜ İı.wav");
    g_ptr_array_unref(names);
    g_assert_cmpint(radios_fs_statat(fd, "Şarkı ÇĞÜ İı.wav", FALSE, &info), ==, 0);
    g_assert_cmpint(radios_fs_unlinkat(fd, "Şarkı ÇĞÜ İı.wav", 0), ==, 0);
    g_assert_cmpint(radios_fs_statat(fd, "Şarkı ÇĞÜ İı.wav", FALSE, &info), ==, -1);
    g_assert_cmpint(errno, ==, ENOENT);
    close(fd);
    remove_tree(dir);
    g_assert_cmpint(radios_fs_stat(dir, FALSE, &info), ==, -1);
    g_free(file); g_free(renamed); g_free(sub); g_free(dir);
}

/* Paths beyond MAX_PATH work without the longPathAware manifest (NT paths). */
static void test_fs_long_paths(void)
{
    gchar *dir = make_tmp_dir();
    gchar *deep = g_strdup(dir);
    while (strlen(deep) < 320) {
        gchar *next = g_build_filename(deep, "uzun-klasör-adı-ğüşiöç-0123456789", NULL);
        g_free(deep);
        deep = next;
    }
    g_assert_cmpint(radios_fs_mkdir_with_parents(deep, 0700), ==, 0);
    gchar *file = g_build_filename(deep, "dosya.txt", NULL), *moved = g_build_filename(deep, "taşındı.txt", NULL);
    g_assert_cmpuint(strlen(file), >, 260);
    write_file(file, "uzun", 0600);
    RadioSFileInfo info;
    g_assert_cmpint(radios_fs_stat(file, FALSE, &info), ==, 0);
    g_assert_cmpint(info.size, ==, 4);
    GError *error = NULL;
    gchar *real = radios_fs_realpath(file, &error);
    g_assert_no_error(error);
    g_assert_true(radios_path_equal(real, file));
    g_free(real);
    g_assert_true(radios_fs_path_is_private(file, FALSE, RADIOS_PRIVATE_OWNER | RADIOS_PRIVATE_EXACT_FILE_MODE));
    g_assert_cmpint(radios_fs_rename(file, moved), ==, 0);
    g_assert_cmpint(radios_fs_dir_check_access(deep, TRUE), ==, 0);
    gint64 free_bytes = 0;
    g_assert_true(radios_fs_disk_space(deep, &free_bytes, NULL));
    gint fd = radios_fs_open_nofollow_path(moved, RADIOS_O_RDONLY, 0);
    g_assert_cmpint(fd, >=, 0);
    gchar *text = read_fd(fd);
    g_assert_cmpstr(text, ==, "uzun");
    g_free(text);
    close(fd);
    g_assert_cmpint(radios_fs_unlink(moved), ==, 0);
    remove_tree(dir);
    g_free(file); g_free(moved); g_free(deep); g_free(dir);
}

/* Directory listing across several 64 KiB result buffers; identity helpers. */
static void test_fs_many_names(void)
{
    gchar *dir = make_tmp_dir();
    gint dirfd = radios_fs_open(dir, RADIOS_O_RDONLY | RADIOS_O_DIRECTORY, 0);
    GHashTable *expected = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint i = 0; i < 600; ++i) {
        gchar *name = g_strdup_printf("%04u-şarkı-adı-çok-uzun-ğüşiöç-ĞÜŞİÖÇ-abcdefghijklmnopqrstuvwxyz-0123456789-%04u.wav", i, i);
        gint fd = radios_fs_openat(dirfd, name, RADIOS_O_WRONLY | RADIOS_O_CREAT | RADIOS_O_EXCL, 0644);
        g_assert_cmpint(fd, >=, 0);
        close(fd);
        g_hash_table_add(expected, name);
    }
    GPtrArray *names = radios_fd_list_names(dirfd, NULL);
    g_assert_cmpuint(names->len, ==, 600);
    for (guint i = 0; i < names->len; ++i) g_assert_true(g_hash_table_contains(expected, names->pdata[i]));
    g_ptr_array_unref(names);
    names = radios_fd_list_names(dirfd, NULL);   /* restartable on the same fd */
    g_assert_cmpuint(names->len, ==, 600);
    g_ptr_array_unref(names);
    RadioSFileInfo a, b;
    g_assert_cmpint(radios_fs_statat(dirfd, "0000-şarkı-adı-çok-uzun-ğüşiöç-ĞÜŞİÖÇ-abcdefghijklmnopqrstuvwxyz-0123456789-0000.wav", FALSE, &a), ==, 0);
    gint fd = radios_fs_openat(dirfd, "0000-şarkı-adı-çok-uzun-ğüşiöç-ĞÜŞİÖÇ-abcdefghijklmnopqrstuvwxyz-0123456789-0000.wav", RADIOS_O_RDWR, 0);
    g_assert_cmpint(radios_fd_stat(fd, &b), ==, 0);
    g_assert_true(radios_file_info_same(&a, &b));
    g_assert_cmpuint(a.inode | a.inode_high, !=, 0);
    g_assert_cmpint(write(fd, "x", 1), ==, 1);
    g_assert_cmpint(radios_fd_sync(fd), ==, 0);
    g_assert_cmpint(radios_fd_stat(fd, &b), ==, 0);
    g_assert_false(radios_file_info_same(&a, &b));
    g_assert_cmpint(b.size, ==, 1);
    close(fd);
    close(dirfd);
    g_hash_table_unref(expected);
    remove_tree(dir);
    g_free(dir);
}

static void test_fs_open_flags(void)
{
    gchar *dir = make_tmp_dir();
    gchar *file = g_build_filename(dir, "dosya.txt", NULL), *missing = g_build_filename(dir, "yok.txt", NULL);
    write_file(file, "a\nb", 0644);
    g_assert_cmpint(radios_fs_open(dir, RADIOS_O_WRONLY, 0), ==, -1);
    g_assert_cmpint(errno, ==, EISDIR);
    g_assert_cmpint(radios_fs_open(file, RADIOS_O_RDONLY | RADIOS_O_DIRECTORY, 0), ==, -1);
    g_assert_cmpint(errno, ==, ENOTDIR);
    g_assert_cmpint(radios_fs_open(file, RADIOS_O_WRONLY | RADIOS_O_CREAT | RADIOS_O_EXCL, 0600), ==, -1);
    g_assert_cmpint(errno, ==, EEXIST);
    g_assert_cmpint(radios_fs_open(missing, RADIOS_O_RDONLY, 0), ==, -1);
    g_assert_cmpint(errno, ==, ENOENT);
    gint dirfd = radios_fs_open(dir, RADIOS_O_RDONLY | RADIOS_O_DIRECTORY | RADIOS_O_NOFOLLOW, 0);
    g_assert_cmpint(dirfd, >=, 0);
    static const gchar *bad[] = { "a\\b", "a/b", "..", ".", "", "CON", "x:y" };
    for (guint i = 0; i < G_N_ELEMENTS(bad); ++i) {
        g_assert_cmpint(radios_fs_openat(dirfd, bad[i], RADIOS_O_RDONLY, 0), ==, -1);
        g_assert_cmpint(errno, ==, EINVAL);
    }
    /* A legacy device name inside a path never opens the device. */
    gchar *device = g_build_filename(dir, "NUL", NULL), *console = g_build_filename(dir, "con.txt", NULL);
    gint dev = radios_fs_open(device, RADIOS_O_WRONLY | RADIOS_O_CREAT, 0644);
    if (dev >= 0) {   /* newer Windows builds treat it as an ordinary name: it must then be a file */
        RadioSFileInfo dev_info;
        g_assert_cmpint(radios_fd_stat(dev, &dev_info), ==, 0);
        g_assert_cmpint(dev_info.type, ==, RADIOS_FILE_TYPE_REGULAR);
        close(dev);
        g_assert_cmpint(radios_fs_unlink(device), ==, 0);
    } else g_assert_cmpint(errno, ==, EINVAL);
    dev = radios_fs_open(console, RADIOS_O_RDONLY, 0);
    g_assert_cmpint(dev, ==, -1);
    g_assert_true(errno == EINVAL || errno == ENOENT);
    g_free(device); g_free(console);
    /* Binary, non-inheritable descriptor; plain open of a directory with RDONLY works. */
    gint fd = radios_fs_openat(dirfd, "dosya.txt", RADIOS_O_RDONLY, 0);
    g_assert_cmpint(fd, >=, 0);
    gchar *text = read_fd(fd);
    g_assert_cmpstr(text, ==, "a\nb");
    g_free(text);
    DWORD handle_flags = 0;
    g_assert_true(GetHandleInformation((HANDLE)_get_osfhandle(fd), &handle_flags));
    g_assert_cmpuint(handle_flags & HANDLE_FLAG_INHERIT, ==, 0);
    close(fd);
    fd = radios_fs_open(dir, RADIOS_O_RDONLY, 0);
    g_assert_cmpint(fd, >=, 0);
    RadioSFileInfo info;
    g_assert_cmpint(radios_fd_stat(fd, &info), ==, 0);
    g_assert_cmpint(info.type, ==, RADIOS_FILE_TYPE_DIRECTORY);
    close(fd);
    /* APPEND and TRUNC. */
    fd = radios_fs_openat(dirfd, "dosya.txt", RADIOS_O_WRONLY | RADIOS_O_APPEND, 0);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, "c", 1), ==, 1);
    close(fd);
    text = read_file(file);
    g_assert_cmpstr(text, ==, "a\nbc");
    g_free(text);
    fd = radios_fs_openat(dirfd, "dosya.txt", RADIOS_O_WRONLY | RADIOS_O_TRUNC, 0);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(radios_fd_stat(fd, &info), ==, 0);
    g_assert_cmpint(info.size, ==, 0);
    g_assert_cmpint(write(fd, "xyz", 3), ==, 3);
    close(fd);
    /* No-follow truncation (emulated after the reparse check) and APPEND|TRUNC. */
    fd = radios_fs_openat(dirfd, "dosya.txt", RADIOS_O_WRONLY | RADIOS_O_TRUNC | RADIOS_O_NOFOLLOW, 0);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(radios_fd_stat(fd, &info), ==, 0);
    g_assert_cmpint(info.size, ==, 0);
    close(fd);
    fd = radios_fs_openat(dirfd, "yeni.txt", RADIOS_O_WRONLY | RADIOS_O_CREAT | RADIOS_O_TRUNC | RADIOS_O_APPEND | RADIOS_O_NOFOLLOW, 0600);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, "1", 1), ==, 1);
    close(fd);
    fd = radios_fs_openat(dirfd, "yeni.txt", RADIOS_O_WRONLY | RADIOS_O_TRUNC | RADIOS_O_APPEND, 0);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(write(fd, "2", 1), ==, 1);
    close(fd);
    gchar *fresh = g_build_filename(dir, "yeni.txt", NULL);
    text = read_file(fresh);
    g_assert_cmpstr(text, ==, "2");
    g_free(text);
    g_free(fresh);
    /* AT_FDCWD accepts relative paths. */
    gchar *cwd = g_get_current_dir();
    g_assert_cmpint(g_chdir(dir), ==, 0);
    fd = radios_fs_openat(RADIOS_AT_FDCWD, "dosya.txt", RADIOS_O_RDONLY, 0);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_assert_cmpint(g_chdir(cwd), ==, 0);
    g_free(cwd);
    /* open_root */
    gchar **parts = NULL;
    gint root = radios_fs_open_root("C:\\Windows\\System32", &parts);
    g_assert_cmpint(root, >=, 0);
    g_assert_cmpuint(g_strv_length(parts), ==, 2);
    g_assert_cmpstr(parts[0], ==, "Windows");
    g_assert_cmpstr(parts[1], ==, "System32");
    g_assert_cmpint(radios_fd_stat(root, &info), ==, 0);
    g_assert_cmpint(info.type, ==, RADIOS_FILE_TYPE_DIRECTORY);
    close(root);
    g_strfreev(parts);
    g_assert_cmpint(radios_fs_open_root("relative\\x", &parts), ==, -1);
    g_assert_cmpint(errno, ==, EINVAL);
    g_assert_cmpint(radios_fs_open_root("C:\\a\\..\\b", &parts), ==, -1);
    g_assert_cmpint(errno, ==, EINVAL);
    fd = radios_fs_open_nofollow_path(file, RADIOS_O_RDONLY, 0);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    close(dirfd);
    remove_tree(dir);
    g_free(file); g_free(missing); g_free(dir);
}

typedef struct {
    HANDLE handle;
    guint delay_ms;
} DelayedClose;

static gpointer close_later(gpointer data)
{
    DelayedClose *close_info = data;
    g_usleep(close_info->delay_ms * 1000);
    CloseHandle(close_info->handle);
    return NULL;
}

static void test_fs_replace_open_target(void)
{
    gchar *dir = make_tmp_dir();
    gchar *target = g_build_filename(dir, "hedef.ini", NULL);
    write_file(target, "eski", 0600);
    gint dirfd = radios_fs_open(dir, RADIOS_O_RDONLY | RADIOS_O_DIRECTORY | RADIOS_O_NOFOLLOW, 0);
    g_assert_cmpint(dirfd, >=, 0);

    /* 1. Target open by a platform fd (FILE_SHARE_DELETE): replaced at once
     *    (POSIX semantics); the open fd keeps reading the old contents. */
    gint reader = radios_fs_open(target, RADIOS_O_RDONLY, 0);
    g_assert_cmpint(reader, >=, 0);
    gint temp = radios_fs_openat(dirfd, ".hedef.tmp", RADIOS_O_WRONLY | RADIOS_O_CREAT | RADIOS_O_EXCL | RADIOS_O_NOFOLLOW, 0600);
    g_assert_cmpint(temp, >=, 0);
    g_assert_cmpint(write(temp, "yeni", 4), ==, 4);
    g_assert_cmpint(radios_fd_sync(temp), ==, 0);
    close(temp);
    gint64 started = g_get_monotonic_time();
    g_assert_cmpint(radios_fs_renameat(dirfd, ".hedef.tmp", dirfd, "hedef.ini"), ==, 0);
    g_assert_cmpint((g_get_monotonic_time() - started) / 1000, <, 2000);   /* no retry budget used up */
    gchar *text = read_fd(reader);
    g_assert_cmpstr(text, ==, "eski");
    g_free(text);
    close(reader);
    text = read_file(target);
    g_assert_cmpstr(text, ==, "yeni");
    g_free(text);

    /* 2. Target held by a plain CRT handle (no FILE_SHARE_DELETE) that is closed
     *    after 400 ms: the bounded retry replaces it once it is released. */
    gunichar2 *wide = g_utf8_to_utf16(target, -1, NULL, NULL, NULL);
    gint plain = _wopen((const wchar_t *)wide, _O_RDONLY | _O_BINARY);
    g_assert_cmpint(plain, >=, 0);
    HANDLE duplicate = NULL;
    g_assert_true(DuplicateHandle(GetCurrentProcess(), (HANDLE)_get_osfhandle(plain), GetCurrentProcess(), &duplicate, 0, FALSE,
                                  DUPLICATE_SAME_ACCESS));
    close(plain);   /* the duplicate keeps the no-delete-share open alive */
    gchar *second = g_build_filename(dir, ".hedef2.tmp", NULL), *third = g_build_filename(dir, ".hedef3.tmp", NULL);
    gchar *fourth = g_build_filename(dir, ".x.tmp", NULL);
    write_file(second, "üçüncü", 0600);
    DelayedClose info = { duplicate, 400 };
    GThread *thread = g_thread_new("close-later", close_later, &info);
    started = g_get_monotonic_time();
    g_assert_cmpint(radios_fs_renameat(dirfd, ".hedef2.tmp", dirfd, "hedef.ini"), ==, 0);
    gint64 elapsed = (g_get_monotonic_time() - started) / 1000;
    g_thread_join(thread);
    g_test_message("replace waited %" G_GINT64_FORMAT " ms for the plain reader", elapsed);
    g_assert_cmpint(elapsed, >=, 250);
    text = read_file(target);
    g_assert_cmpstr(text, ==, "üçüncü");
    g_free(text);

    /* 3. Held for longer than the 2 s budget: the replace fails with EBUSY/EACCES,
     *    leaving both files intact (the caller removes its temp file). */
    plain = _wopen((const wchar_t *)wide, _O_RDONLY | _O_BINARY);
    g_assert_cmpint(plain, >=, 0);
    write_file(third, "dördüncü", 0600);
    started = g_get_monotonic_time();
    g_assert_cmpint(radios_fs_renameat(dirfd, ".hedef3.tmp", dirfd, "hedef.ini"), ==, -1);
    gint code = errno;
    elapsed = (g_get_monotonic_time() - started) / 1000;
    g_test_message("replace gave up after %" G_GINT64_FORMAT " ms with errno %d", elapsed, code);
    g_assert_true(code == EACCES || code == EBUSY);
    g_assert_cmpint(elapsed, >=, 1900);
    g_assert_cmpint(elapsed, <, 4000);
    close(plain);
    text = read_file(target);
    g_assert_cmpstr(text, ==, "üçüncü");
    g_free(text);
    RadioSFileInfo stat_info;
    g_assert_cmpint(radios_fs_statat(dirfd, ".hedef3.tmp", FALSE, &stat_info), ==, 0);
    g_assert_cmpint(radios_fs_unlinkat(dirfd, ".hedef3.tmp", 0), ==, 0);

    /* 4. A directory is never replaced. */
    g_assert_cmpint(radios_fs_mkdirat(dirfd, "klasör", 0755), ==, 0);
    write_file(fourth, "x", 0600);
    g_assert_cmpint(radios_fs_renameat(dirfd, ".x.tmp", dirfd, "klasör"), ==, -1);
    g_assert_cmpint(errno, ==, EISDIR);
    g_free(wide);
    close(dirfd);
    remove_tree(dir);
    g_free(second); g_free(third); g_free(fourth); g_free(target); g_free(dir);
}

static void test_fs_unlink_open_file(void)
{
    gchar *dir = make_tmp_dir();
    gchar *path = g_build_filename(dir, "açık.wav", NULL);
    write_file(path, "kayıt", 0644);
    gint fd = radios_fs_open(path, RADIOS_O_RDONLY, 0);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(radios_fs_unlink(path), ==, 0);
    RadioSFileInfo info;
    g_assert_cmpint(radios_fs_stat(path, FALSE, &info), ==, -1);
    g_assert_cmpint(errno, ==, ENOENT);
    /* The name is free at once (no delete-pending state). */
    gint again = radios_fs_open(path, RADIOS_O_WRONLY | RADIOS_O_CREAT | RADIOS_O_EXCL, 0644);
    g_assert_cmpint(again, >=, 0);
    close(again);
    gchar *text = read_fd(fd);
    g_assert_cmpstr(text, ==, "kayıt");
    g_free(text);
    close(fd);
    g_assert_cmpint(radios_fs_unlink(dir), ==, -1);
    g_assert_cmpint(errno, ==, EISDIR);
    g_assert_cmpint(radios_fs_unlinkat(RADIOS_AT_FDCWD, path, RADIOS_AT_REMOVEDIR), ==, -1);
    g_assert_cmpint(errno, ==, ENOTDIR);
    gchar *sub = g_build_filename(dir, "dolu", NULL), *inner = g_build_filename(sub, "x", NULL);
    g_assert_cmpint(radios_fs_mkdir_with_parents(sub, 0755), ==, 0);
    write_file(inner, "x", 0644);
    g_assert_cmpint(radios_fs_unlinkat(RADIOS_AT_FDCWD, sub, RADIOS_AT_REMOVEDIR), ==, -1);
    g_assert_cmpint(errno, ==, ENOTEMPTY);
    remove_tree(dir);
    g_free(sub); g_free(inner); g_free(path); g_free(dir);
}

static void test_fs_links(void)
{
    gchar *dir = make_tmp_dir();
    gchar *real = g_build_filename(dir, "gerçek", NULL), *inside = g_build_filename(real, "iç.txt", NULL);
    gchar *link = g_build_filename(dir, "bağlantı", NULL), *through = g_build_filename(link, "iç.txt", NULL);
    g_assert_cmpint(radios_fs_mkdir_with_parents(real, 0755), ==, 0);
    write_file(inside, "içerik", 0644);
    GError *error = NULL;
    g_assert_cmpint(radios_platform_test_symlink(real, link, TRUE, &error), ==, 0);
    g_assert_no_error(error);
    RadioSFileInfo info;
    g_assert_cmpint(radios_fs_stat(link, FALSE, &info), ==, 0);
    g_assert_cmpint(info.type, ==, RADIOS_FILE_TYPE_LINK);
    g_assert_cmpint(radios_fs_stat(link, TRUE, &info), ==, 0);
    g_assert_cmpint(info.type, ==, RADIOS_FILE_TYPE_DIRECTORY);
    g_assert_cmpint(radios_fs_open(link, RADIOS_O_RDONLY | RADIOS_O_DIRECTORY | RADIOS_O_NOFOLLOW, 0), ==, -1);
    g_assert_cmpint(errno, ==, ELOOP);
    gint dirfd = radios_fs_open(dir, RADIOS_O_RDONLY | RADIOS_O_DIRECTORY | RADIOS_O_NOFOLLOW, 0);
    g_assert_cmpint(radios_fs_openat(dirfd, "bağlantı", RADIOS_O_RDONLY | RADIOS_O_DIRECTORY | RADIOS_O_NOFOLLOW, 0), ==, -1);
    g_assert_cmpint(errno, ==, ELOOP);
    g_assert_cmpint(radios_fs_statat(dirfd, "bağlantı", FALSE, &info), ==, 0);
    g_assert_cmpint(info.type, ==, RADIOS_FILE_TYPE_LINK);
    g_assert_cmpint(radios_fs_open_nofollow_path(through, RADIOS_O_RDONLY, 0), ==, -1);
    g_assert_cmpint(errno, ==, ELOOP);
    gint fd = radios_fs_open(through, RADIOS_O_RDONLY, 0);   /* following is the default */
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    gchar *resolved = radios_fs_realpath(through, &error);
    g_assert_no_error(error);
    g_assert_true(radios_path_equal(resolved, inside));
    g_free(resolved);
    /* The no-follow unlink removes the link, never the target. */
    g_assert_cmpint(radios_fs_unlinkat(dirfd, "bağlantı", RADIOS_AT_REMOVEDIR), ==, 0);
    g_assert_cmpint(radios_fs_stat(inside, FALSE, &info), ==, 0);
    /* Directory links can also be removed with a plain unlink (they are links). */
    g_assert_cmpint(radios_platform_test_symlink(real, link, TRUE, &error), ==, 0);
    g_assert_cmpint(radios_fs_unlink(link), ==, 0);
    g_assert_cmpint(radios_fs_stat(inside, FALSE, &info), ==, 0);
    /* File symlinks need Developer Mode or the privilege. */
    gchar *file_link = g_build_filename(dir, "dosya-bağlantısı", NULL);
    if (radios_platform_test_symlink(inside, file_link, FALSE, &error) == 0) {
        g_assert_cmpint(radios_fs_stat(file_link, FALSE, &info), ==, 0);
        g_assert_cmpint(info.type, ==, RADIOS_FILE_TYPE_LINK);
        g_assert_cmpint(radios_fs_open(file_link, RADIOS_O_RDONLY | RADIOS_O_NOFOLLOW, 0), ==, -1);
        g_assert_cmpint(errno, ==, ELOOP);
        g_assert_cmpint(radios_fs_open(file_link, RADIOS_O_WRONLY | RADIOS_O_CREAT | RADIOS_O_NOFOLLOW, 0600), ==, -1);
        g_assert_cmpint(errno, ==, ELOOP);
        g_assert_cmpint(radios_fs_open(file_link, RADIOS_O_WRONLY | RADIOS_O_TRUNC | RADIOS_O_NOFOLLOW, 0), ==, -1);
        g_assert_cmpint(errno, ==, ELOOP);
        gchar *kept = read_file(inside);   /* the link target was not truncated */
        g_assert_cmpstr(kept, ==, "içerik");
        g_free(kept);
        g_assert_cmpint(radios_fs_unlink(file_link), ==, 0);
        g_assert_cmpint(radios_fs_stat(inside, FALSE, &info), ==, 0);
    } else {
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
        g_test_message("file symlinks not available (no Developer Mode): %s", error->message);
        g_clear_error(&error);
    }
    close(dirfd);
    remove_tree(dir);
    g_free(file_link); g_free(real); g_free(inside); g_free(link); g_free(through); g_free(dir);
}

static void test_fs_private(void)
{
    gchar *dir = make_tmp_dir();
    gchar *file = g_build_filename(dir, "kimlik.json", NULL);
    gint fd = radios_fs_open(file, RADIOS_O_WRONLY | RADIOS_O_CREAT | RADIOS_O_EXCL | RADIOS_O_NOFOLLOW, 0600);
    g_assert_cmpint(fd, >=, 0);
    g_assert_true(radios_fd_is_private(fd, RADIOS_PRIVATE_OWNER | RADIOS_PRIVATE_NO_OTHERS | RADIOS_PRIVATE_EXACT_FILE_MODE));
    close(fd);
    g_assert_true(radios_fs_path_is_private(file, FALSE, RADIOS_PRIVATE_OWNER | RADIOS_PRIVATE_NO_OTHERS | RADIOS_PRIVATE_EXACT_FILE_MODE));

    gint dirfd = radios_fs_open(dir, RADIOS_O_RDONLY | RADIOS_O_DIRECTORY, 0);
    g_assert_cmpint(radios_fs_mkdirat(dirfd, "özel", 0700), ==, 0);
    g_assert_cmpint(radios_fs_mkdirat(dirfd, "özel", 0700), ==, -1);
    g_assert_cmpint(errno, ==, EEXIST);
    gchar *private_dir = g_build_filename(dir, "özel", NULL);
    g_assert_true(radios_fs_path_is_private(private_dir, FALSE, RADIOS_PRIVATE_OWNER | RADIOS_PRIVATE_NO_OTHERS));
    g_assert_false(radios_fs_path_is_private(private_dir, FALSE, RADIOS_PRIVATE_EXACT_FILE_MODE));
    g_assert_cmpint(errno, ==, EACCES);
    /* Children of a private directory inherit its ACEs. */
    gchar *child = g_build_filename(private_dir, "alt.txt", NULL);
    write_file(child, "x", 0644);
    g_assert_true(radios_fs_path_is_private(child, FALSE, RADIOS_PRIVATE_OWNER | RADIOS_PRIVATE_NO_OTHERS));
    /* A deliberately public file. */
    gchar *public_file = g_build_filename(dir, "açık.txt", NULL);
    write_file(public_file, "x", 0644);
    GError *error = NULL;
    g_assert_true(radios_platform_test_make_public(public_file, &error));
    g_assert_no_error(error);
    g_assert_false(radios_fs_path_is_private(public_file, FALSE, RADIOS_PRIVATE_NO_OTHERS));
    g_assert_cmpint(errno, ==, EACCES);
    g_assert_true(radios_fs_path_is_private(public_file, FALSE, RADIOS_PRIVATE_OWNER));
    fd = radios_fs_open(public_file, RADIOS_O_RDONLY, 0);
    g_assert_false(radios_fd_is_private(fd, RADIOS_PRIVATE_EXACT_FILE_MODE));
    g_assert_cmpint(radios_fd_make_private(fd), ==, 0);
    g_assert_true(radios_fd_is_private(fd, RADIOS_PRIVATE_OWNER | RADIOS_PRIVATE_NO_OTHERS | RADIOS_PRIVATE_EXACT_FILE_MODE));
    close(fd);
    g_assert_true(radios_platform_test_make_public(public_file, &error));
    g_assert_true(radios_fs_path_make_private(public_file, &error));
    g_assert_no_error(error);
    g_assert_true(radios_fs_path_is_private(public_file, TRUE, RADIOS_PRIVATE_EXACT_FILE_MODE));
    /* mkdir_with_parents(0700): every new component is private. */
    gchar *deep = g_build_filename(dir, "a", "ğ", "ş", NULL);
    g_assert_cmpint(radios_fs_mkdir_with_parents(deep, 0700), ==, 0);
    g_assert_true(radios_fs_path_is_private(deep, FALSE, RADIOS_PRIVATE_OWNER | RADIOS_PRIVATE_NO_OTHERS));
    gchar *middle = g_build_filename(dir, "a", NULL);
    g_assert_true(radios_fs_path_is_private(middle, FALSE, RADIOS_PRIVATE_OWNER | RADIOS_PRIVATE_NO_OTHERS));
    g_assert_cmpint(radios_fs_mkdir_with_parents(deep, 0700), ==, 0);
    gchar *under_file = g_build_filename(public_file, "x", NULL);
    g_assert_cmpint(radios_fs_mkdir_with_parents(under_file, 0700), ==, -1);
    g_assert_cmpint(errno, ==, ENOTDIR);
    close(dirfd);
    remove_tree(dir);
    g_free(under_file); g_free(middle); g_free(deep); g_free(public_file); g_free(child); g_free(private_dir);
    g_free(file); g_free(dir);
}

static void test_fs_realpath(void)
{
    GError *error = NULL;
    const gchar *tmp = g_get_tmp_dir();
    gchar *short_form = g_strdup(tmp);
    if (!strchr(short_form, '~')) {
        gunichar2 *wide = g_utf8_to_utf16(tmp, -1, NULL, NULL, NULL);
        WCHAR *buffer = g_new(WCHAR, 32768);
        DWORD n = GetShortPathNameW((LPCWSTR)wide, buffer, 32768);
        if (n > 0 && n < 32768) { g_free(short_form); short_form = g_utf16_to_utf8((gunichar2 *)buffer, n, NULL, NULL, NULL); }
        g_free(buffer);
        g_free(wide);
    }
    g_test_message("temp (short form): %s", short_form);
    gchar *real = radios_fs_realpath(short_form, &error);
    g_assert_no_error(error);
    g_test_message("realpath: %s", real);
    g_assert_null(strchr(real, '~'));
    g_assert_true(g_ascii_isupper(real[0]) && real[1] == ':' && real[2] == '\\');
    gunichar2 *wide = g_utf8_to_utf16(short_form, -1, NULL, NULL, NULL);
    WCHAR *long_buffer = g_new(WCHAR, 32768);
    DWORD n = GetLongPathNameW((LPCWSTR)wide, long_buffer, 32768);
    g_free(wide);
    g_assert_cmpuint(n, >, 0);
    gchar *long_form = g_utf16_to_utf8((gunichar2 *)long_buffer, n, NULL, NULL, NULL);
    g_free(long_buffer);
    gchar *long_normal = radios_path_normalize(long_form);
    g_assert_true(radios_path_equal(real, long_normal));
    gchar *with_separator = g_strconcat(short_form, "\\", NULL);
    gchar *again = radios_fs_realpath(with_separator, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(again, ==, real);
    g_free(again);
    gchar *missing = g_build_filename(real, "radios-yok-ğüş", "x", NULL);
    g_assert_null(radios_fs_realpath(missing, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_assert_cmpint(errno, ==, ENOENT);
    g_clear_error(&error);
    gchar *root = radios_fs_realpath("c:\\", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(root, ==, "C:\\");
    gchar *cwd = g_get_current_dir();
    gchar *dot = radios_fs_realpath(".", &error), *cwd_real = radios_fs_realpath(cwd, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(dot, ==, cwd_real);
    g_free(dot); g_free(cwd_real); g_free(cwd); g_free(root); g_free(missing); g_free(with_separator);
    g_free(long_form); g_free(long_normal); g_free(real); g_free(short_form);
}

static void test_fs_space_access(void)
{
    gchar *dir = make_tmp_dir();
    gint64 free_bytes = -1, total_bytes = -1;
    g_assert_true(radios_fs_disk_space(dir, &free_bytes, &total_bytes));
    g_assert_cmpint(free_bytes, >, 0);
    g_assert_cmpint(total_bytes, >=, free_bytes);
    gint64 untouched = 42;
    g_assert_false(radios_fs_disk_space("Q:\\yok\\böyle", &untouched, NULL));
    g_assert_cmpint(untouched, ==, 42);
    g_assert_cmpint(radios_fs_dir_check_access(dir, TRUE), ==, 0);
    gint fd = radios_fs_open(dir, RADIOS_O_RDONLY | RADIOS_O_DIRECTORY, 0);
    GPtrArray *names = radios_fd_list_names(fd, NULL);
    g_assert_cmpuint(names->len, ==, 0);   /* the probe file is gone */
    g_ptr_array_unref(names);
    close(fd);
    gchar *missing = g_build_filename(dir, "yok", NULL), *file = g_build_filename(dir, "dosya", NULL);
    g_assert_cmpint(radios_fs_dir_check_access(missing, FALSE), ==, -1);
    g_assert_cmpint(errno, ==, ENOENT);
    write_file(file, "x", 0644);
    g_assert_cmpint(radios_fs_dir_check_access(file, FALSE), ==, -1);
    g_assert_cmpint(errno, ==, ENOTDIR);
    /* ACL-accurate: a directory the user may list but not write to. */
    gchar *read_only = g_build_filename(dir, "salt-okunur", NULL);
    g_assert_cmpint(radios_fs_mkdir_with_parents(read_only, 0755), ==, 0);
    HANDLE token = NULL;
    g_assert_true(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token));
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, NULL, 0, &size);
    TOKEN_USER *user = g_malloc(size);
    g_assert_true(GetTokenInformation(token, TokenUser, user, size, &size));
    LPWSTR sid = NULL;
    g_assert_true(ConvertSidToStringSidW(user->User.Sid, &sid));
    gchar *sid_text = g_utf16_to_utf8((gunichar2 *)sid, -1, NULL, NULL, NULL);
    gchar *sddl = g_strdup_printf("D:P(A;;0x1200a9;;;%s)", sid_text);   /* FILE_GENERIC_READ | FILE_EXECUTE */
    gunichar2 *sddl_w = g_utf8_to_utf16(sddl, -1, NULL, NULL, NULL);
    PSECURITY_DESCRIPTOR sd = NULL;
    g_assert_true(ConvertStringSecurityDescriptorToSecurityDescriptorW((LPCWSTR)sddl_w, SDDL_REVISION_1, &sd, NULL));
    gunichar2 *ro_w = g_utf8_to_utf16(read_only, -1, NULL, NULL, NULL);
    g_assert_true(SetFileSecurityW((LPCWSTR)ro_w, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, sd));
    g_assert_cmpint(radios_fs_dir_check_access(read_only, FALSE), ==, 0);
    g_assert_cmpint(radios_fs_dir_check_access(read_only, TRUE), ==, -1);
    g_assert_cmpint(errno, ==, EACCES);
    /* The owner may always rewrite the DACL: restore access for the cleanup. */
    GError *error = NULL;
    gboolean restored = radios_fs_path_make_private(read_only, &error);
    g_assert_no_error(error);
    g_assert_true(restored);
    g_assert_cmpint(radios_fs_dir_check_access(read_only, TRUE), ==, 0);
    LocalFree(sd); LocalFree(sid);
    g_free(ro_w); g_free(sddl_w); g_free(sddl); g_free(sid_text); g_free(user);
    CloseHandle(token);
    remove_tree(dir);
    g_free(read_only); g_free(missing); g_free(file); g_free(dir);
}

static void test_fs_large_offsets(void)
{
    gchar *dir = make_tmp_dir();
    gchar *path = g_build_filename(dir, "seyrek.bin", NULL);
    gint fd = radios_fs_open(path, RADIOS_O_RDWR | RADIOS_O_CREAT | RADIOS_O_EXCL, 0644);
    g_assert_cmpint(fd, >=, 0);
    DWORD returned = 0;
    g_assert_true(DeviceIoControl((HANDLE)_get_osfhandle(fd), FSCTL_SET_SPARSE, NULL, 0, NULL, 0, &returned, NULL));
    const gint64 big = G_GINT64_CONSTANT(2147483648) + 100;   /* > 2 GiB */
    g_assert_cmpint(radios_fd_pwrite(fd, "uzak", 4, big), ==, 4);
    g_assert_cmpint(radios_fd_seek(fd, 0, SEEK_CUR), ==, 0);   /* position restored */
    gchar buffer[8] = { 0 };
    g_assert_cmpint(radios_fd_pread(fd, buffer, 4, big), ==, 4);
    g_assert_cmpstr(buffer, ==, "uzak");
    g_assert_cmpint(radios_fd_pread(fd, buffer, 4, big + 100), ==, 0);   /* EOF */
    g_assert_cmpint(radios_fd_truncate(fd, G_GINT64_CONSTANT(2147483649)), ==, 0);
    RadioSFileInfo info;
    g_assert_cmpint(radios_fd_stat(fd, &info), ==, 0);
    g_assert_cmpint(info.size, ==, G_GINT64_CONSTANT(2147483649));
    g_assert_cmpint(radios_fd_seek(fd, -1, SEEK_END), ==, G_GINT64_CONSTANT(2147483648));
    g_assert_cmpint(radios_fd_pread(fd, buffer, 1, -1), ==, -1);
    g_assert_cmpint(errno, ==, EINVAL);
    close(fd);
    remove_tree(dir);
    g_free(path); g_free(dir);
}

/* ------------------------------------------------------------------ locks */

typedef struct {
    const gchar *path;
    gint64 acquired_us;
} LockThread;

static gpointer lock_in_thread(gpointer data)
{
    LockThread *thread = data;
    gint fd = radios_fs_open(thread->path, RADIOS_O_RDWR, 0);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(radios_fd_lock(fd, RADIOS_LOCK_EX), ==, 0);   /* blocks */
    thread->acquired_us = g_get_monotonic_time();
    radios_fd_close(fd);
    return NULL;
}

static void test_locks(void)
{
    gchar *dir = make_tmp_dir();
    gchar *path = g_build_filename(dir, "gui.lock", NULL);
    gint a = radios_fs_open(path, RADIOS_O_RDWR | RADIOS_O_CREAT | RADIOS_O_NOFOLLOW, 0600);
    gint b = radios_fs_open(path, RADIOS_O_RDWR, 0), c = radios_fs_open(path, RADIOS_O_RDWR, 0);
    g_assert_cmpint(a, >=, 0); g_assert_cmpint(b, >=, 0); g_assert_cmpint(c, >=, 0);
    g_assert_cmpint(radios_fd_lock(a, RADIOS_LOCK_SH | RADIOS_LOCK_NB), ==, 0);
    g_assert_cmpint(radios_fd_lock(a, RADIOS_LOCK_SH | RADIOS_LOCK_NB), ==, 0);   /* idempotent */
    g_assert_cmpint(radios_fd_lock(b, RADIOS_LOCK_SH | RADIOS_LOCK_NB), ==, 0);
    g_assert_cmpint(radios_fd_lock(c, RADIOS_LOCK_EX | RADIOS_LOCK_NB), ==, -1);
    g_assert_true(errno == EWOULDBLOCK || errno == EAGAIN);
    g_assert_cmpint(radios_fd_close(a), ==, 0);
    g_assert_cmpint(radios_fd_lock(c, RADIOS_LOCK_EX | RADIOS_LOCK_NB), ==, -1);
    g_assert_cmpint(radios_fd_lock(b, RADIOS_LOCK_UN), ==, 0);
    g_assert_cmpint(radios_fd_lock(b, RADIOS_LOCK_UN), ==, 0);   /* unlocking twice is fine */
    g_assert_cmpint(radios_fd_lock(c, RADIOS_LOCK_EX | RADIOS_LOCK_NB), ==, 0);
    /* The lock region lies beyond the data: reads and writes are not blocked. */
    g_assert_cmpint(write(b, "veri", 4), ==, 4);
    g_assert_cmpint(radios_fd_seek(b, 0, SEEK_SET), ==, 0);
    gchar buffer[8] = { 0 };
    g_assert_cmpint(read(b, buffer, 4), ==, 4);
    g_assert_cmpstr(buffer, ==, "veri");
    g_assert_cmpint(radios_fd_lock(b, RADIOS_LOCK_SH | RADIOS_LOCK_NB), ==, -1);
    /* EX -> SH conversion lets other shared holders in. */
    g_assert_cmpint(radios_fd_lock(c, RADIOS_LOCK_SH), ==, 0);
    g_assert_cmpint(radios_fd_lock(b, RADIOS_LOCK_SH | RADIOS_LOCK_NB), ==, 0);
    g_assert_cmpint(radios_fd_lock(b, RADIOS_LOCK_UN), ==, 0);
    /* Blocking EX waits for another thread's release. */
    g_assert_cmpint(radios_fd_lock(c, RADIOS_LOCK_EX), ==, 0);
    LockThread thread = { path, 0 };
    GThread *waiter = g_thread_new("lock-waiter", lock_in_thread, &thread);
    g_usleep(300 * 1000);
    gint64 released = g_get_monotonic_time();
    g_assert_cmpint(thread.acquired_us, ==, 0);
    g_assert_cmpint(radios_fd_lock(c, RADIOS_LOCK_UN), ==, 0);
    g_thread_join(waiter);
    g_assert_cmpint(thread.acquired_us, >=, released);
    g_assert_cmpint(radios_fd_lock(c, 0), ==, -1);
    g_assert_cmpint(errno, ==, EINVAL);
    g_assert_cmpint(radios_fd_lock(c, RADIOS_LOCK_SH | RADIOS_LOCK_EX), ==, -1);
    radios_fd_close(b);
    radios_fd_close(c);
    remove_tree(dir);
    g_free(path); g_free(dir);
}

static void test_locks_two_processes(void)
{
    gchar *dir = make_tmp_dir();
    gchar *path = g_build_filename(dir, "kilit ğ.lock", NULL);
    gint fd = radios_fs_open(path, RADIOS_O_RDWR | RADIOS_O_CREAT, 0600);
    g_assert_cmpint(fd, >=, 0);
    /* The helper holds SH until its stdin closes. */
    gchar **argv = helper_argv("lock", path, "SH", NULL);
    gint in = -1, out = -1;
    GError *error = NULL;
    RadioSProcess *holder = radios_process_spawn((const gchar *const *)argv, NULL, NULL,
                                                 RADIOS_SPAWN_STDIN_PIPE | RADIOS_SPAWN_STDOUT_PIPE, &in, &out, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpint(in, >=, 0); g_assert_cmpint(out, >=, 0);
    gchar line[16] = { 0 };
    g_assert_cmpint(read(out, line, 7), ==, 7);
    g_assert_cmpstr(line, ==, "locked\n");
    g_assert_cmpint(radios_fd_lock(fd, RADIOS_LOCK_EX | RADIOS_LOCK_NB), ==, -1);
    g_assert_true(errno == EWOULDBLOCK || errno == EAGAIN);
    g_assert_cmpint(radios_fd_lock(fd, RADIOS_LOCK_SH | RADIOS_LOCK_NB), ==, 0);
    g_assert_cmpint(radios_fd_lock(fd, RADIOS_LOCK_UN), ==, 0);
    close(in);   /* EOF: the helper releases and exits */
    gint exit_code = -1;
    g_assert_true(radios_process_wait(holder, 10000, &exit_code));
    g_assert_cmpint(exit_code, ==, 0);
    close(out);
    radios_process_free(holder);
    g_strfreev(argv);
    g_assert_cmpint(radios_fd_lock(fd, RADIOS_LOCK_EX | RADIOS_LOCK_NB), ==, 0);
    /* Our EX blocks another process; after UN it gets the lock. */
    argv = helper_argv("try-lock", path, "SH", NULL);
    g_free(run_helper(argv, 10000, &exit_code));
    g_assert_cmpint(exit_code, ==, 11);
    g_assert_cmpint(radios_fd_lock(fd, RADIOS_LOCK_UN), ==, 0);
    g_free(run_helper(argv, 10000, &exit_code));
    g_assert_cmpint(exit_code, ==, 0);
    g_strfreev(argv);
    radios_fd_close(fd);
    remove_tree(dir);
    g_free(path); g_free(dir);
}

/* ------------------------------------------------------------------ time zones */

static void test_tz_istanbul_new_york(void)
{
    GTimeZone *istanbul = radios_time_zone_new("Europe/Istanbul");
    g_assert_nonnull(istanbul);
    g_assert_cmpint(offset_at(istanbul, 2015, 1, 15, 12, 0, 0), ==, 2 * 3600);
    g_assert_cmpint(offset_at(istanbul, 2015, 7, 15, 12, 0, 0), ==, 3 * 3600);
    g_assert_cmpint(offset_at(istanbul, 2026, 10, 15, 12, 0, 0), ==, 3 * 3600);
    g_assert_cmpint(offset_at(istanbul, 2026, 1, 15, 12, 0, 0), ==, 3 * 3600);
    g_assert_cmpstr(radios_time_zone_get_identifier(istanbul), ==, "Europe/Istanbul");
    GTimeZone *again = radios_time_zone_new("Europe/Istanbul");
    g_assert_true(again == istanbul);
    g_time_zone_unref(again);
    GTimeZone *ny = radios_time_zone_new("America/New_York");
    g_assert_nonnull(ny);
    g_assert_cmpint(offset_at(ny, 2026, 1, 15, 12, 0, 0), ==, -5 * 3600);
    g_assert_cmpint(offset_at(ny, 2026, 7, 15, 12, 0, 0), ==, -4 * 3600);
    g_assert_cmpint(offset_at(ny, 2026, 3, 8, 6, 59, 59), ==, -5 * 3600);   /* DST starts 2026-03-08 07:00 UTC */
    g_assert_cmpint(offset_at(ny, 2026, 3, 8, 7, 0, 0), ==, -4 * 3600);
    g_assert_cmpint(offset_at(ny, 2026, 11, 1, 5, 59, 59), ==, -4 * 3600);  /* ends 2026-11-01 06:00 UTC */
    g_assert_cmpint(offset_at(ny, 2026, 11, 1, 6, 0, 0), ==, -5 * 3600);
    g_assert_cmpstr(radios_time_zone_get_identifier(ny), ==, "America/New_York");
    GTimeZone *berlin = radios_time_zone_new("Europe/Berlin");
    g_assert_cmpint(offset_at(berlin, 2026, 3, 29, 0, 59, 59), ==, 3600);
    g_assert_cmpint(offset_at(berlin, 2026, 3, 29, 1, 0, 0), ==, 7200);
    /* Wall-clock interpretation in the zone (gap and folds handled by GLib). */
    GDateTime *local = g_date_time_new(istanbul, 2026, 10, 13, 23, 30, 0);
    gchar *text = radios_date_time_format(local, "%d.%m.%Y %H:%M:%S %Z");
    g_assert_cmpstr(text, ==, "13.10.2026 23:30:00 +03");
    g_free(text);
    text = radios_date_time_format(local, "%%Z %H");
    g_assert_cmpstr(text, ==, "%Z 23");
    g_free(text);
    g_date_time_unref(local);
    local = g_date_time_new(berlin, 2026, 7, 1, 12, 0, 0);
    text = radios_date_time_format(local, "%Z");
    g_assert_cmpstr(text, ==, "CEST");
    g_free(text);
    g_date_time_unref(local);
    local = g_date_time_new(ny, 2026, 1, 15, 12, 0, 0);
    text = radios_date_time_format(local, "%H:%M %Z");
    g_assert_cmpstr(text, ==, "12:00 EST");
    g_free(text);
    g_date_time_unref(local);
    g_time_zone_unref(berlin);
    g_time_zone_unref(ny);
    g_time_zone_unref(istanbul);
}

static void test_tz_utc_and_invalid(void)
{
    GTimeZone *utc = radios_time_zone_new("UTC"), *glib_utc = g_time_zone_new_utc();
    g_assert_true(utc == glib_utc);
    g_assert_cmpstr(radios_time_zone_get_identifier(utc), ==, "UTC");
    GTimeZone *etc = radios_time_zone_new("Etc/UTC");
    g_assert_nonnull(etc);
    g_assert_cmpint(offset_at(etc, 2026, 7, 1, 0, 0, 0), ==, 0);
    g_assert_cmpstr(radios_time_zone_get_identifier(etc), ==, "Etc/UTC");
    GDateTime *now = g_date_time_new_now(etc);
    gchar *abbreviation = radios_date_time_format(now, "%Z");
    g_assert_cmpstr(abbreviation, ==, "UTC");
    g_free(abbreviation);
    g_date_time_unref(now);
    g_time_zone_unref(etc);
    g_time_zone_unref(glib_utc);
    g_time_zone_unref(utc);
    static const gchar *invalid[] = { "Mars/Olympus_Mons", "../x", "a\\b", "C:x", "/UTC", "Europe//Istanbul", "Europe/",
                                      "", "Europe/Istanbul ", "Turkey Standard Time", "+03:00", "zone.tab", "leapseconds",
                                      "Europe/./Istanbul", "Europe/../Europe/Istanbul", "tzdata.zi", "PST",
                                      "Europe/Istanbul/x", "europe\\istanbul" };
    for (guint i = 0; i < G_N_ELEMENTS(invalid); ++i) {
        GTimeZone *tz = radios_time_zone_new(invalid[i]);
        if (tz) g_error("time zone should be rejected: '%s'", invalid[i]);
        if (radios_time_zone_identifier_exists(invalid[i])) g_error("identifier should not exist: '%s'", invalid[i]);
    }
    g_assert_true(radios_time_zone_identifier_exists("UTC"));
    g_assert_true(radios_time_zone_identifier_exists("Europe/Istanbul"));
    g_assert_true(radios_time_zone_identifier_exists("America/Argentina/Buenos_Aires"));
    g_assert_false(radios_time_zone_identifier_exists(NULL));
    GTimeZone *local = radios_time_zone_new(NULL);   /* g_time_zone_new_identifier(NULL) semantics */
    g_assert_nonnull(local);
    const gchar *local_id = radios_time_zone_get_identifier(local);
    g_test_message("local zone %s -> %s", g_time_zone_get_identifier(local), local_id);
    g_assert_true(radios_time_zone_identifier_exists(local_id));
    g_time_zone_unref(local);
}

static void test_tz_aliases_and_fallbacks(void)
{
    /* Aliases that map to one Windows zone still get their own object. */
    static const gchar *ids[] = { "Europe/Istanbul", "Asia/Istanbul", "Turkey", "Europe/Berlin", "Europe/Amsterdam",
                                  "Europe/Vienna", "Etc/GMT", "GMT", "Etc/GMT-12", "Pacific/Tarawa", "Pacific/Majuro" };
    GTimeZone *zones[G_N_ELEMENTS(ids)];
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) {
        zones[i] = radios_time_zone_new(ids[i]);
        g_assert_nonnull(zones[i]);
    }
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) {
        g_assert_cmpstr(radios_time_zone_get_identifier(zones[i]), ==, ids[i]);
        for (guint j = 0; j < i; ++j) g_assert_true(zones[i] != zones[j]);
    }
    g_assert_cmpint(offset_at(zones[8], 2026, 7, 1, 0, 0, 0), ==, 12 * 3600);
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) g_time_zone_unref(zones[i]);
    /* GLib would read the Windows name "UTC-02" as a POSIX rule with the sign inverted. */
    GTimeZone *noronha = radios_time_zone_new("America/Noronha");
    g_assert_cmpint(offset_at(noronha, 2026, 7, 1, 0, 0, 0), ==, -2 * 3600);
    g_time_zone_unref(noronha);
    /* Negative DST (winter time is the "DST" in tzdata). */
    GTimeZone *dublin = radios_time_zone_new("Europe/Dublin");
    g_assert_cmpint(offset_at(dublin, 2026, 1, 15, 12, 0, 0), ==, 0);
    g_assert_cmpint(offset_at(dublin, 2026, 7, 15, 12, 0, 0), ==, 3600);
    GDateTime *winter = g_date_time_new(dublin, 2026, 1, 15, 12, 0, 0), *summer = g_date_time_new(dublin, 2026, 7, 15, 12, 0, 0);
    gchar *w = radios_date_time_format(winter, "%Z"), *s = radios_date_time_format(summer, "%Z");
    g_assert_cmpstr(w, ==, "GMT");
    g_assert_cmpstr(s, ==, "IST");
    g_free(w); g_free(s);
    g_date_time_unref(winter); g_date_time_unref(summer);
    g_time_zone_unref(dublin);
    /* Zones the system ICU cannot map fall back to the TZif rules. */
    GTimeZone *coyhaique = radios_time_zone_new("America/Coyhaique");
    g_assert_nonnull(coyhaique);
    g_assert_cmpint(offset_at(coyhaique, 2026, 7, 1, 0, 0, 0), ==, -3 * 3600);
    g_assert_cmpint(offset_at(coyhaique, 2026, 1, 1, 0, 0, 0), ==, -3 * 3600);
    g_assert_cmpstr(radios_time_zone_get_identifier(coyhaique), ==, "America/Coyhaique");
    g_time_zone_unref(coyhaique);
    GTimeZone *troll = radios_time_zone_new("Antarctica/Troll");
    g_assert_cmpint(offset_at(troll, 2026, 1, 15, 0, 0, 0), ==, 0);
    g_assert_cmpint(offset_at(troll, 2026, 7, 15, 0, 0, 0), ==, 2 * 3600);
    g_time_zone_unref(troll);
}

static void test_tz_list_and_all_zones(void)
{
    gchar **list = radios_time_zone_list_identifiers();
    guint count = g_strv_length(list);
    g_test_message("%u identifiers listed", count);
    g_assert_cmpuint(count, >, 400);
    gboolean istanbul = FALSE, utc = FALSE, new_york = FALSE;
    for (guint i = 0; i < count; ++i) {
        if (i > 0 && strcmp(list[i - 1], list[i]) >= 0) g_error("list not sorted/unique at %s", list[i]);
        if (!radios_time_zone_identifier_exists(list[i])) g_error("listed id does not exist: %s", list[i]);
        istanbul |= g_str_equal(list[i], "Europe/Istanbul");
        utc |= g_str_equal(list[i], "UTC");
        new_york |= g_str_equal(list[i], "America/New_York");
    }
    g_assert_true(istanbul && utc && new_york);
    g_strfreev(list);
    /* Every bundled TZif zone loads and round-trips its identifier. */
    const gchar *root = radios_time_zone_directory();
    g_test_message("zoneinfo: %s", root);
    GQueue pending = G_QUEUE_INIT;
    g_queue_push_tail(&pending, g_strdup(""));
    guint zones = 0;
    while (!g_queue_is_empty(&pending)) {
        gchar *relative = g_queue_pop_head(&pending);
        gchar *dir = *relative ? g_build_filename(root, relative, NULL) : g_strdup(root);
        GDir *handle = g_dir_open(dir, 0, NULL);
        const gchar *name;
        while (handle && (name = g_dir_read_name(handle))) {
            gchar *id = *relative ? g_strconcat(relative, "/", name, NULL) : g_strdup(name);
            gchar *path = g_build_filename(dir, name, NULL);
            if (g_file_test(path, G_FILE_TEST_IS_DIR)) { g_queue_push_tail(&pending, id); id = NULL; }
            else if (radios_time_zone_identifier_exists(id)) {
                GTimeZone *tz = radios_time_zone_new(id);
                if (!tz) g_error("bundled zone does not load: %s", id);
                if (g_strcmp0(radios_time_zone_get_identifier(tz), id) != 0) g_error("identifier round trip failed: %s", id);
                g_time_zone_unref(tz);
                ++zones;
            }
            g_free(path); g_free(id);
        }
        if (handle) g_dir_close(handle);
        g_free(dir); g_free(relative);
    }
    g_test_message("%u bundled zones load", zones);
    g_assert_cmpuint(zones, >, 590);
}

static gpointer create_zones(gpointer data)
{
    static const gchar *ids[] = { "Asia/Tokyo", "Europe/Paris", "Australia/Sydney", "America/Chicago", "Africa/Cairo" };
    GPtrArray *zones = data;
    for (guint round = 0; round < 50; ++round)
        for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) {
            GTimeZone *tz = radios_time_zone_new(ids[i]);
            g_assert_nonnull(tz);
            if (round == 0) g_ptr_array_add(zones, tz); else g_time_zone_unref(tz);
        }
    return NULL;
}

static void test_tz_threads(void)
{
    GPtrArray *results[6];
    GThread *threads[6];
    for (guint i = 0; i < G_N_ELEMENTS(threads); ++i) {
        results[i] = g_ptr_array_new_with_free_func((GDestroyNotify)g_time_zone_unref);
        threads[i] = g_thread_new("tz", create_zones, results[i]);
    }
    for (guint i = 0; i < G_N_ELEMENTS(threads); ++i) g_thread_join(threads[i]);
    for (guint i = 1; i < G_N_ELEMENTS(threads); ++i)
        for (guint j = 0; j < results[0]->len; ++j) g_assert_true(results[i]->pdata[j] == results[0]->pdata[j]);
    for (guint i = 0; i < G_N_ELEMENTS(threads); ++i) g_ptr_array_unref(results[i]);
}

/* ------------------------------------------------------------------ processes */

static void test_process_python_utf8(void)
{
    gchar *python = python_path();
    if (!python) { g_test_skip("runtime\\python\\python.exe bulunamadı"); return; }
    const gchar *argv[] = { python, "-E", "-s", "-B", "-X", "utf8", "-c", "print('" TURKISH "')", NULL };
    GError *error = NULL;
    /* Owned (overlapped) pipe consumed by communicate(). */
    RadioSProcess *process = radios_process_spawn(argv, NULL, NULL, RADIOS_SPAWN_STDOUT_PIPE | RADIOS_SPAWN_STDERR_PIPE,
                                                  NULL, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(radios_process_get_id(process), >, 0);
    GBytes *out = NULL, *err = NULL;
    gint exit_code = -1;
    g_assert_true(radios_process_communicate(process, NULL, 4096, 30000, &out, &err, &exit_code, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpint(exit_code, ==, 0);
    gsize size = 0;
    const gchar *data = g_bytes_get_data(out, &size);
    gchar *text = g_strndup(data, size);
    g_assert_true(g_utf8_validate(text, -1, NULL));
    g_strchomp(text);
    g_assert_cmpstr(text, ==, TURKISH);
    g_assert_cmpuint(g_bytes_get_size(err), ==, 0);
    g_free(text);
    g_bytes_unref(out); g_bytes_unref(err);
    radios_process_free(process);
    /* Caller-owned synchronous CRT fd. */
    gint fd = -1;
    process = radios_process_spawn(argv, NULL, NULL, RADIOS_SPAWN_STDOUT_PIPE, NULL, &fd, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpint(fd, >=, 0);
    text = read_fd(fd);
    close(fd);
    g_assert_true(radios_process_wait(process, 30000, &exit_code));
    g_assert_cmpint(exit_code, ==, 0);
    g_strchomp(text);
    g_assert_cmpstr(text, ==, TURKISH);
    g_free(text);
    radios_process_free(process);
    /* stdin -> stdout, 1 MiB both ways (no pipe deadlock), and the output cap. */
    const gchar *echo[] = { python, "-E", "-s", "-B", "-X", "utf8", "-c",
                            "import sys; d = sys.stdin.buffer.read(); sys.stdout.buffer.write(d); sys.exit(3)", NULL };
    gsize big = 1024 * 1024;
    guint8 *payload = g_malloc(big);
    for (gsize i = 0; i < big; ++i) payload[i] = (guint8)(i * 7 + 1);
    GBytes *input = g_bytes_new_take(payload, big);
    process = radios_process_spawn(echo, NULL, NULL, RADIOS_SPAWN_STDIN_PIPE | RADIOS_SPAWN_STDOUT_PIPE, NULL, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_true(radios_process_communicate(process, input, big + 1, 60000, &out, NULL, &exit_code, NULL, &error));
    g_assert_no_error(error);
    g_assert_cmpint(exit_code, ==, 3);
    g_assert_true(g_bytes_equal(out, input));
    g_bytes_unref(out);
    radios_process_free(process);
    process = radios_process_spawn(echo, NULL, NULL, RADIOS_SPAWN_STDIN_PIPE | RADIOS_SPAWN_STDOUT_PIPE, NULL, NULL, NULL, &error);
    g_assert_true(radios_process_communicate(process, input, 1000, 60000, &out, NULL, &exit_code, NULL, &error));
    g_assert_cmpuint(g_bytes_get_size(out), ==, 1000);
    g_assert_cmpint(exit_code, ==, 3);
    g_bytes_unref(out);
    radios_process_free(process);
    g_bytes_unref(input);
    /* A child-side environment block: PYTHONUTF8 etc., Turkish values, duplicate names. */
    gchar **env = g_get_environ();
    env = g_environ_setenv(env, "RADIOS_TEST_DEĞER", "çalışıyor ğüş", TRUE);
    env = g_environ_unsetenv(env, "PYTHONPATH");
    GPtrArray *full = g_ptr_array_new();
    for (guint i = 0; env[i]; ++i) g_ptr_array_add(full, env[i]);
    g_ptr_array_add(full, (gpointer)"radios_test_case=ilk");
    g_ptr_array_add(full, (gpointer)"RADIOS_TEST_CASE=son");
    g_ptr_array_add(full, NULL);
    const gchar *show[] = { python, "-E", "-s", "-B", "-X", "utf8", "-c",
                            "import os; print(os.environ['RADIOS_TEST_DEĞER'] + '|' + os.environ['RADIOS_TEST_CASE'])", NULL };
    process = radios_process_spawn(show, NULL, (const gchar *const *)full->pdata, RADIOS_SPAWN_STDOUT_PIPE, NULL, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_true(radios_process_communicate(process, NULL, 4096, 30000, &out, NULL, &exit_code, NULL, &error));
    text = g_strndup(g_bytes_get_data(out, NULL), g_bytes_get_size(out));
    g_strchomp(text);
    g_assert_cmpstr(text, ==, "çalışıyor ğüş|son");
    g_free(text);
    g_bytes_unref(out);
    radios_process_free(process);
    g_ptr_array_unref(full);
    g_strfreev(env);
    /* radios_process_environment(): case-insensitive set/remove like Windows. */
    g_setenv("radios_test_remove_me", "x", TRUE);
    g_setenv("radios_test_case", "ilk", TRUE);
    const gchar *overrides[] = { "RADIOS_TEST_DEĞER=çalışıyor ğüş", "RADIOS_TEST_CASE=ara", "RADIOS_TEST_REMOVE_ME",
                                 "Radios_Test_Case=son", NULL };
    env = radios_process_environment(overrides);
    guint matches = 0;
    for (guint i = 0; env[i]; ++i) matches += g_ascii_strncasecmp(env[i], "RADIOS_TEST_CASE=", 17) == 0;
    g_assert_cmpuint(matches, ==, 1);
    const gchar *show_more[] = { python, "-E", "-s", "-B", "-X", "utf8", "-c",
                                 "import os; print(os.environ['RADIOS_TEST_DEĞER'] + '|' + os.environ['RADIOS_TEST_CASE'] + '|' + "
                                 "str('RADIOS_TEST_REMOVE_ME' in os.environ))", NULL };
    process = radios_process_spawn(show_more, NULL, (const gchar *const *)env, RADIOS_SPAWN_STDOUT_PIPE, NULL, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_true(radios_process_communicate(process, NULL, 4096, 30000, &out, NULL, &exit_code, NULL, &error));
    text = g_strndup(g_bytes_get_data(out, NULL), g_bytes_get_size(out));
    g_strchomp(text);
    g_assert_cmpstr(text, ==, "çalışıyor ğüş|son|False");
    g_free(text);
    g_bytes_unref(out);
    radios_process_free(process);
    g_strfreev(env);
    g_unsetenv("radios_test_remove_me");
    g_unsetenv("radios_test_case");
    /* Working directory with Turkish characters. */
    gchar *dir = make_tmp_dir();
    const gchar *cwd[] = { python, "-E", "-s", "-B", "-X", "utf8", "-c", "import os; print(os.getcwd())", NULL };
    process = radios_process_spawn(cwd, dir, NULL, RADIOS_SPAWN_STDOUT_PIPE, NULL, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_true(radios_process_communicate(process, NULL, 4096, 30000, &out, NULL, &exit_code, NULL, &error));
    text = g_strndup(g_bytes_get_data(out, NULL), g_bytes_get_size(out));
    g_strchomp(text);
    g_assert_true(radios_path_equal(text, dir));
    g_free(text);
    g_bytes_unref(out);
    radios_process_free(process);
    remove_tree(dir);
    g_free(dir);
    g_free(python);
}

static void test_process_exit_timeout(void)
{
    gint exit_code = -1;
    gchar **argv = helper_argv("exit", "7", NULL);
    g_free(run_helper(argv, 20000, &exit_code));
    g_assert_cmpint(exit_code, ==, 7);
    g_strfreev(argv);
    argv = helper_argv("console-window", NULL);
    gchar *text = run_helper(argv, 20000, &exit_code);
    g_assert_cmpstr(text, ==, "console-window=0");
    g_free(text);
    g_strfreev(argv);
    argv = helper_argv("sleep", "30000", NULL);
    GError *error = NULL;
    RadioSProcess *process = radios_process_spawn((const gchar *const *)argv, NULL, NULL, RADIOS_SPAWN_STDOUT_PIPE,
                                                  NULL, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_false(radios_process_wait(process, 0, &exit_code));
    gint64 started = g_get_monotonic_time();
    GBytes *out = NULL;
    g_assert_false(radios_process_communicate(process, NULL, 1024, 300, &out, NULL, &exit_code, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT);
    g_clear_error(&error);
    g_assert_null(out);
    g_assert_cmpint((g_get_monotonic_time() - started) / 1000, <, 5000);
    g_assert_true(radios_process_wait(process, 10000, &exit_code));
    g_assert_cmpint(exit_code, ==, 1);
    g_assert_true(radios_process_terminate(process, 9));   /* already exited: fine */
    radios_process_free(process);
    /* Terminate a running child. */
    process = radios_process_spawn((const gchar *const *)argv, NULL, NULL, RADIOS_SPAWN_NULL_STDIO, NULL, NULL, NULL, &error);
    g_assert_no_error(error);
    g_assert_true(radios_process_terminate(process, 42));
    g_assert_true(radios_process_wait(process, 10000, &exit_code));
    g_assert_cmpint(exit_code, ==, 42);
    radios_process_free(process);
    /* Cancellation leaves the child running. */
    process = radios_process_spawn((const gchar *const *)argv, NULL, NULL, RADIOS_SPAWN_STDOUT_PIPE, NULL, NULL, NULL, &error);
    GCancellable *cancellable = g_cancellable_new();
    g_cancellable_cancel(cancellable);
    g_assert_false(radios_process_communicate(process, NULL, 1024, 10000, &out, NULL, &exit_code, cancellable, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_assert_false(radios_process_wait(process, 0, &exit_code));
    g_assert_true(radios_process_terminate(process, 1));
    g_assert_true(radios_process_wait(process, 10000, &exit_code));
    g_object_unref(cancellable);
    radios_process_free(process);
    g_strfreev(argv);
    /* Detached children outlive us; this one just exits. */
    argv = helper_argv("sleep", "100", NULL);
    g_assert_true(radios_process_spawn_detached((const gchar *const *)argv, NULL, &error));
    g_assert_no_error(error);
    g_strfreev(argv);
}

static void test_process_validation_and_quoting(void)
{
    GError *error = NULL;
    const gchar *relative[] = { "python.exe", "-c", "1", NULL };
    g_assert_null(radios_process_spawn(relative, NULL, NULL, 0, NULL, NULL, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    const gchar *batch[] = { "C:\\Windows\\x.CMD", NULL };
    g_assert_null(radios_process_spawn(batch, NULL, NULL, 0, NULL, NULL, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_clear_error(&error);
    const gchar *missing[] = { "C:\\RadioS-yok\\olmayan.exe", NULL };
    g_assert_null(radios_process_spawn(missing, NULL, NULL, 0, NULL, NULL, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    const gchar *bad_env[] = { "NO_EQUALS_SIGN", NULL };
    gchar *self = self_path();
    const gchar *argv[] = { self, HELPER_FLAG, "exit", "0", NULL };
    g_assert_null(radios_process_spawn(argv, NULL, bad_env, 0, NULL, NULL, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    /* Quoting round-trips through CommandLineToArgvW. */
    const gchar *tricky[] = { "C:\\Program Files\\RadioS\\RadioS.exe", "a b", "", "x\"y", "trail\\", "back\\\\\"q",
                              "plain", "\\\\server\\share\\", TURKISH, "tab\there", "\"", "\\", NULL };
    gchar *line = radios_process_quote_command_line(tricky);
    gunichar2 *wide = g_utf8_to_utf16(line, -1, NULL, NULL, NULL);
    int count = 0;
    LPWSTR *parsed = CommandLineToArgvW((LPCWSTR)wide, &count);
    g_assert_nonnull(parsed);
    g_assert_cmpint(count, ==, (int)g_strv_length((gchar **)tricky));
    for (int i = 0; i < count; ++i) {
        gchar *arg = g_utf16_to_utf8((gunichar2 *)parsed[i], -1, NULL, NULL, NULL);
        if (g_strcmp0(arg, tricky[i]) != 0) g_error("argument %d: '%s' != '%s' (line %s)", i, arg, tricky[i], line);
        g_free(arg);
    }
    LocalFree(parsed);
    g_free(wide); g_free(line);
    /* The same quoting reaches a real child (CRT argv of the helper). */
    gchar **echo = helper_argv("argv", "a b", "", "x\"y", "trail\\", TURKISH, NULL);
    gint exit_code = -1;
    gchar *text = run_helper(echo, 20000, &exit_code);
    g_assert_cmpstr(text, ==, "a b|<empty>|x\"y|trail\\|" TURKISH);
    g_free(text);
    g_strfreev(echo);
    g_free(self);
}

static void test_process_handle_isolation(void)
{
    /* Even an inheritable handle of ours never reaches a child. */
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE inheritable = CreateEventW(&sa, TRUE, FALSE, NULL);
    g_assert_nonnull(inheritable);
    gchar *value = g_strdup_printf("%" G_GUINT64_FORMAT, (guint64)(guintptr)inheritable);
    gchar **argv = helper_argv("handle-check", value, NULL);
    gint exit_code = -1;
    gchar *text = run_helper(argv, 20000, &exit_code);
    g_assert_cmpstr(text, ==, "absent");
    g_free(text);
    g_strfreev(argv);
    g_free(value);
    CloseHandle(inheritable);
}

typedef struct {
    GMainLoop *loop;
    gint exit_code;
    gboolean fired;
} WatchResult;

static void watch_callback(RadioSProcess *process, gint exit_code, gpointer data)
{
    WatchResult *result = data;
    g_assert_nonnull(process);
    result->exit_code = exit_code;
    result->fired = TRUE;
    g_main_loop_quit(result->loop);
}

static gboolean loop_timeout(gpointer data)
{
    g_main_loop_quit(data);
    return G_SOURCE_REMOVE;
}

static void communicate_done(GObject *source, GAsyncResult *result, gpointer data)
{
    (void)source;
    gpointer *slots = data;
    GError *error = NULL;
    GBytes *out = NULL;
    gint exit_code = -1;
    g_assert_true(radios_process_communicate_finish(slots[0], result, &out, NULL, &exit_code, &error));
    g_assert_no_error(error);
    slots[1] = g_strndup(g_bytes_get_data(out, NULL), g_bytes_get_size(out));
    g_bytes_unref(out);
    g_main_loop_quit(slots[2]);
}

static void test_process_watch_async(void)
{
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    gchar **argv = helper_argv("exit", "5", NULL);
    GError *error = NULL;
    RadioSProcess *process = radios_process_spawn((const gchar *const *)argv, NULL, NULL, RADIOS_SPAWN_NULL_STDIO,
                                                  NULL, NULL, NULL, &error);
    g_assert_no_error(error);
    WatchResult result = { loop, -1, FALSE };
    g_assert_cmpuint(radios_process_watch(process, NULL, watch_callback, &result), >, 0);
    radios_process_free(process);   /* the watch keeps it alive */
    guint timeout = g_timeout_add(20000, loop_timeout, loop);
    g_main_loop_run(loop);
    g_source_remove(timeout);
    g_assert_true(result.fired);
    g_assert_cmpint(result.exit_code, ==, 5);
    g_strfreev(argv);
    argv = helper_argv("argv", "merhaba", NULL);
    process = radios_process_spawn((const gchar *const *)argv, NULL, NULL, RADIOS_SPAWN_STDOUT_PIPE, NULL, NULL, NULL, &error);
    g_assert_no_error(error);
    gpointer slots[3] = { process, NULL, loop };
    radios_process_communicate_async(process, NULL, 4096, 20000, NULL, communicate_done, slots);
    timeout = g_timeout_add(20000, loop_timeout, loop);
    g_main_loop_run(loop);
    g_source_remove(timeout);
    g_assert_cmpstr(slots[1], ==, "merhaba");
    g_free(slots[1]);
    radios_process_free(process);
    g_strfreev(argv);
    g_main_loop_unref(loop);
}

typedef struct {
    GMainLoop *loop;
    guint calls;
} StopResult;

static gboolean stop_requested(gpointer data)
{
    StopResult *result = data;
    ++result->calls;
    g_main_loop_quit(result->loop);
    return G_SOURCE_CONTINUE;
}

static void test_process_stop_event(void)
{
    gchar *name = radios_process_stop_event_name(1234);
    g_assert_cmpstr(name, ==, "Local\\RadioS-stop-1234");
    g_free(name);
    GError *error = NULL;
    g_assert_false(radios_process_request_stop(0x7FFFFFF0u, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    StopResult result = { loop, 0 };
    guint watch = radios_process_add_termination_watch(stop_requested, &result, NULL);
    g_assert_cmpuint(watch, >, 0);
    gchar *pid = g_strdup_printf("%lu", (gulong)radios_process_id());
    gchar **argv = helper_argv("request-stop", pid, NULL);
    for (guint round = 1; round <= 2; ++round) {
        gint exit_code = -1;
        g_free(run_helper(argv, 20000, &exit_code));
        g_assert_cmpint(exit_code, ==, 0);
        guint timeout = g_timeout_add(10000, loop_timeout, loop);
        g_main_loop_run(loop);
        g_source_remove(timeout);
        g_assert_cmpuint(result.calls, ==, round);   /* the event is re-armed after each request */
    }
    /* No spurious dispatch afterwards. */
    guint timeout = g_timeout_add(200, loop_timeout, loop);
    g_main_loop_run(loop);
    g_assert_cmpuint(result.calls, ==, 2);
    (void)timeout;
    radios_process_remove_termination_watch(watch);
    radios_process_remove_termination_watch(watch);   /* idempotent */
    g_strfreev(argv);
    g_free(pid);
    g_main_loop_unref(loop);
}

static void test_startup_helpers(void)
{
    gchar **argv = radios_platform_get_argv(0, NULL);
    g_assert_nonnull(argv);
    g_assert_true(g_str_has_suffix(argv[0], ".exe") || g_str_has_suffix(argv[0], ".EXE") || strstr(argv[0], "test_platform"));
    radios_process_remember_argv(0, NULL);
    gchar **remembered = radios_process_argv();
    g_assert_cmpuint(g_strv_length(remembered), ==, g_strv_length(argv));
    g_strfreev(remembered);
    g_strfreev(argv);
    gchar *self = self_path();
    RadioSFileInfo info;
    g_assert_cmpint(radios_fs_stat(self, TRUE, &info), ==, 0);
    g_assert_cmpint(info.type, ==, RADIOS_FILE_TYPE_REGULAR);
    g_free(self);
    /* The test runs with usable std handles: nothing to attach. */
    g_assert_false(radios_platform_attach_parent_console());
    g_assert_true(radios_platform_stdio_is_usable(2));
    g_assert_false(radios_platform_relaunch_wait(0));   /* nothing inherited */
    /* Relaunch: B gets a handle to A and waits for A's exit. */
    gchar **relaunch = helper_argv("relaunch-parent", NULL);
    gint exit_code = -1;
    gchar *text = run_helper(relaunch, 30000, &exit_code);
    g_assert_cmpint(exit_code, ==, 0);
    g_assert_cmpstr(text, ==, "waited=1 variable=unset");
    g_free(text);
    g_strfreev(relaunch);
    /* Log sink: a helper without std handles redirects them to the log. */
    gchar *dir = make_tmp_dir();
    gchar *logs = g_build_filename(dir, "data", "logs", NULL);
    g_assert_cmpint(radios_fs_mkdir_with_parents(logs, 0755), ==, 0);
    gchar *log = g_build_filename(logs, "radios-desktop.log", NULL), *rotated = g_strconcat(log, ".1", NULL);
    gint fd = radios_fs_open(log, RADIOS_O_WRONLY | RADIOS_O_CREAT, 0644);
    g_assert_cmpint(radios_fd_truncate(fd, 5 * 1024 * 1024 + 1), ==, 0);
    close(fd);
    gchar **sink = helper_argv("log-sink", logs, NULL);
    g_free(run_helper(sink, 20000, &exit_code));
    g_assert_cmpint(exit_code, ==, 0);
    g_assert_cmpint(radios_fs_stat(rotated, FALSE, &info), ==, 0);
    g_assert_cmpint(info.size, ==, 5 * 1024 * 1024 + 1);
    gchar *contents = read_file(log);
    g_assert_nonnull(strstr(contents, "günlük satırı ğüş\n"));
    g_assert_nonnull(strstr(contents, "çıktı satırı\n"));
    g_assert_null(strstr(contents, "\r\n"));
    g_free(contents);
    g_free(run_helper(sink, 20000, &exit_code));   /* appends, no rotation now */
    contents = read_file(log);
    g_assert_nonnull(strstr(strstr(contents, "günlük satırı") + 1, "günlük satırı"));
    g_free(contents);
    g_strfreev(sink);
    remove_tree(dir);
    g_free(log); g_free(rotated); g_free(logs); g_free(dir);
}

static guint count_dumps(const gchar *crash_dir, gint64 *largest)
{
    gint fd = radios_fs_open(crash_dir, RADIOS_O_RDONLY | RADIOS_O_DIRECTORY, 0);
    if (fd < 0) return 0;
    GPtrArray *names = radios_fd_list_names(fd, NULL);
    guint count = 0;
    for (guint i = 0; names && i < names->len; ++i) {
        const gchar *name = names->pdata[i];
        if (!g_str_has_prefix(name, "RadioS-") || !g_str_has_suffix(name, ".dmp")) continue;
        ++count;
        RadioSFileInfo info;
        if (largest && radios_fs_statat(fd, name, FALSE, &info) == 0 && info.size > *largest) *largest = info.size;
    }
    if (names) g_ptr_array_unref(names);
    close(fd);
    return count;
}

static void test_crash_dumps(void)
{
    gchar *dir = make_tmp_dir();
    gchar *logs = g_build_filename(dir, "logs", NULL), *crash_dir = g_build_filename(logs, "crash", NULL);
    g_assert_cmpint(radios_fs_mkdir_with_parents(crash_dir, 0700), ==, 0);
    for (guint i = 0; i < 12; ++i) {   /* old dumps beyond the retention limit */
        gchar *name = g_strdup_printf("RadioS-20000101T0000%02uZ-1.dmp", i);
        gchar *path = g_build_filename(crash_dir, name, NULL);
        write_file(path, "eski", 0600);
        g_free(path); g_free(name);
    }
    gchar **argv = helper_argv("crash", logs, NULL);
    gint exit_code = 0;
    g_free(run_helper(argv, 60000, &exit_code));
    g_test_message("crashing helper exit code 0x%08x", (guint)exit_code);
    g_assert_cmpuint((guint)exit_code, ==, 0xC0000005u);
    gint64 largest = 0;
    g_assert_cmpuint(count_dumps(crash_dir, &largest), ==, 10);   /* 9 kept + the new one */
    g_assert_cmpint(largest, >, 4096);
    g_strfreev(argv);
    argv = helper_argv("abort", logs, NULL);
    g_free(run_helper(argv, 60000, &exit_code));
    g_test_message("aborting helper exit code %d", exit_code);
    g_assert_cmpint(exit_code, ==, 3);
    g_assert_cmpuint(count_dumps(crash_dir, NULL), ==, 10);   /* retention again: 9 + 1 */
    g_strfreev(argv);
    /* Dumps hold process memory: private. */
    gint fd = radios_fs_open(crash_dir, RADIOS_O_RDONLY | RADIOS_O_DIRECTORY, 0);
    GPtrArray *names = radios_fd_list_names(fd, NULL);
    for (guint i = 0; i < names->len; ++i) {
        const gchar *name = names->pdata[i];
        if (g_str_has_prefix(name, "RadioS-2000")) continue;
        gint dump = radios_fs_openat(fd, name, RADIOS_O_RDONLY, 0);
        g_assert_cmpint(dump, >=, 0);
        g_assert_true(radios_fd_is_private(dump, RADIOS_PRIVATE_OWNER | RADIOS_PRIVATE_NO_OTHERS));
        gchar magic[4];
        g_assert_cmpint(read(dump, magic, 4), ==, 4);
        g_assert_true(memcmp(magic, "MDMP", 4) == 0);
        close(dump);
    }
    g_ptr_array_unref(names);
    close(fd);
    remove_tree(dir);
    g_free(crash_dir); g_free(logs); g_free(dir);
}

/* ------------------------------------------------------------------ helper modes */

static int helper_main(gchar **args)
{
    guint count = g_strv_length(args);
    const gchar *mode = args[0];
    if (g_str_equal(mode, "exit") && count == 2) return (int)g_ascii_strtoll(args[1], NULL, 10);
    if (g_str_equal(mode, "sleep") && count == 2) { g_usleep((gulong)g_ascii_strtoull(args[1], NULL, 10) * 1000); return 0; }
    if (g_str_equal(mode, "console-window")) { printf("console-window=%d", GetConsoleWindow() != NULL); return 0; }
    if (g_str_equal(mode, "argv")) {
        for (guint i = 1; i < count; ++i) printf("%s%s", i > 1 ? "|" : "", *args[i] ? args[i] : "<empty>");
        return 0;
    }
    if (g_str_equal(mode, "handle-check") && count == 2) {
        HANDLE h = (HANDLE)(guintptr)g_ascii_strtoull(args[1], NULL, 10);
        DWORD flags = 0;
        printf("%s", GetHandleInformation(h, &flags) ? "present" : "absent");
        return 0;
    }
    if ((g_str_equal(mode, "lock") || g_str_equal(mode, "try-lock")) && count == 3) {
        gint fd = radios_fs_open(args[1], RADIOS_O_RDWR, 0);
        if (fd < 0) return 1;
        gint operation = g_str_equal(args[2], "EX") ? RADIOS_LOCK_EX : RADIOS_LOCK_SH;
        if (g_str_equal(mode, "try-lock")) {
            if (radios_fd_lock(fd, operation | RADIOS_LOCK_NB) == 0) { radios_fd_close(fd); return 0; }
            gint code = errno;
            radios_fd_close(fd);
            return code == EWOULDBLOCK || code == EAGAIN ? 11 : 1;
        }
        if (radios_fd_lock(fd, operation) != 0) return 1;
        printf("locked\n");
        fflush(stdout);
        gchar buffer[16];
        while (read(0, buffer, sizeof buffer) > 0) {}
        radios_fd_close(fd);
        return 0;
    }
    if (g_str_equal(mode, "request-stop") && count == 2) {
        GError *error = NULL;
        if (radios_process_request_stop((guint32)g_ascii_strtoull(args[1], NULL, 10), &error)) return 0;
        fprintf(stderr, "%s\n", error->message);
        g_error_free(error);
        return 2;
    }
    if (g_str_equal(mode, "relaunch-parent")) {
        gchar **argv = helper_argv("relaunch-wait", "20000", NULL);
        GError *error = NULL;
        RadioSProcess *child = radios_process_spawn((const gchar *const *)argv, NULL, NULL, RADIOS_SPAWN_PASS_SELF_HANDLE,
                                                    NULL, NULL, NULL, &error);
        g_strfreev(argv);
        if (!child) { fprintf(stderr, "%s\n", error->message); return 1; }
        radios_process_free(child);
        g_usleep(500 * 1000);   /* let the child start waiting before we exit */
        return 0;
    }
    if (g_str_equal(mode, "relaunch-wait") && count == 2) {
        gboolean waited = radios_platform_relaunch_wait((guint)g_ascii_strtoull(args[1], NULL, 10));
        printf("waited=%d variable=%s", waited, g_getenv("RADIOS_RELAUNCH_WAIT_HANDLE") ? "set" : "unset");
        return 0;
    }
    if (g_str_equal(mode, "log-sink") && count == 2) {
        /* Simulate a GUI start from Explorer: no std handles at all. */
        SetStdHandle(STD_OUTPUT_HANDLE, NULL);
        SetStdHandle(STD_ERROR_HANDLE, NULL);
        _close(1);
        _close(2);
        GError *error = NULL;
        if (radios_platform_stdio_is_usable(2) || !radios_platform_redirect_stdio_to_log(args[1], &error)) return 1;
        g_printerr("günlük satırı ğüş\n");
        g_print("çıktı satırı\n");
        return 0;
    }
    if ((g_str_equal(mode, "crash") || g_str_equal(mode, "abort")) && count == 2) {
        SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX);   /* no WER dialog in tests */
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        GError *error = NULL;
        if (!radios_platform_install_crash_handler(args[1], &error)) { fprintf(stderr, "%s\n", error->message); return 1; }
        if (g_str_equal(mode, "abort")) abort();
        volatile gint *nowhere = NULL;
        *nowhere = 1;
        return 0;
    }
    fprintf(stderr, "unknown helper mode %s\n", mode);
    return 64;
}

int main(int argc, char **argv)
{
    radios_platform_init();
    if (argc >= 3 && g_str_equal(argv[1], HELPER_FLAG)) {
        gchar **args = radios_platform_get_argv(argc, argv);
        int result = helper_main(args + 2);
        fflush(stdout);
        g_strfreev(args);
        return result;
    }
    if (!g_getenv("TZDIR")) {
        gchar *root = project_root();
        gchar *zoneinfo = root ? g_build_filename(root, "runtime", "share", "zoneinfo", NULL) : NULL;
        if (zoneinfo && g_file_test(zoneinfo, G_FILE_TEST_IS_DIR)) g_setenv("TZDIR", zoneinfo, TRUE);
        g_free(zoneinfo); g_free(root);
    }
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/platform/path/absolute-root", test_path_absolute_root);
    g_test_add_func("/platform/path/equal-inside", test_path_equal_inside);
    g_test_add_func("/platform/path/case-matches-ntfs", test_path_case_matches_ntfs);
    g_test_add_func("/platform/path/normalize", test_path_normalize);
    g_test_add_func("/platform/path/components", test_path_components);
    g_test_add_func("/platform/fs/turkish-names", test_fs_turkish_names);
    g_test_add_func("/platform/fs/long-paths", test_fs_long_paths);
    g_test_add_func("/platform/fs/many-names", test_fs_many_names);
    g_test_add_func("/platform/fs/open-flags", test_fs_open_flags);
    g_test_add_func("/platform/fs/replace-open-target", test_fs_replace_open_target);
    g_test_add_func("/platform/fs/unlink-open-file", test_fs_unlink_open_file);
    g_test_add_func("/platform/fs/links", test_fs_links);
    g_test_add_func("/platform/fs/private", test_fs_private);
    g_test_add_func("/platform/fs/realpath", test_fs_realpath);
    g_test_add_func("/platform/fs/space-access", test_fs_space_access);
    g_test_add_func("/platform/fs/large-offsets", test_fs_large_offsets);
    g_test_add_func("/platform/lock/one-process", test_locks);
    g_test_add_func("/platform/lock/two-processes", test_locks_two_processes);
    g_test_add_func("/platform/tz/istanbul-new-york", test_tz_istanbul_new_york);
    g_test_add_func("/platform/tz/utc-invalid", test_tz_utc_and_invalid);
    g_test_add_func("/platform/tz/aliases-fallbacks", test_tz_aliases_and_fallbacks);
    g_test_add_func("/platform/tz/list-all-zones", test_tz_list_and_all_zones);
    g_test_add_func("/platform/tz/threads", test_tz_threads);
    g_test_add_func("/platform/process/python-utf8", test_process_python_utf8);
    g_test_add_func("/platform/process/exit-timeout", test_process_exit_timeout);
    g_test_add_func("/platform/process/validation-quoting", test_process_validation_and_quoting);
    g_test_add_func("/platform/process/handle-isolation", test_process_handle_isolation);
    g_test_add_func("/platform/process/watch-async", test_process_watch_async);
    g_test_add_func("/platform/process/stop-event", test_process_stop_event);
    g_test_add_func("/platform/startup/helpers", test_startup_helpers);
    g_test_add_func("/platform/startup/crash-dumps", test_crash_dumps);
    return g_test_run();
}
