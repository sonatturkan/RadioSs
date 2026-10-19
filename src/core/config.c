#include "radios/config.h"
#include "radios/platform.h"
#include "radios/storage_locations.h"

#include <errno.h>
#include <gio/gio.h>
#include <string.h>

G_DEFINE_QUARK(radios-config-error-quark, radios_config_error)

/* Bound for the root-discovery walk: no Windows path has more components
 * than this (32767 UTF-16 units), so a non-converging dirname can never hang. */
#define WALK_LIMIT 4096u

/* Every component below the project root must be a valid Windows name: no
 * reserved device (NUL, CON, ...), alternate stream (":"), wildcard or
 * trailing dot/space, which Win32 would silently reinterpret. */
static gboolean
components_valid(const gchar *root, const gchar *path)
{
    gsize root_length = strlen(root);
    gboolean valid = TRUE;
    gchar **parts;
    if (strlen(path) < root_length)
        return FALSE;
    parts = radios_path_split(path + root_length);
    for (gsize i = 0; parts[i] != NULL && valid; ++i)
        valid = radios_path_component_valid(parts[i]);
    g_strfreev(parts);
    return valid;
}

/* Existing prefixes are resolved physically so even a missing destination
 * below a link (symbolic link or junction) cannot redirect a configured path
 * outside RadioS. */
static gchar *
resolve_inside_root(const gchar *root, const gchar *value, GError **error)
{
    gchar *normalized = g_canonicalize_filename(value, root);
    gchar *existing = NULL;
    gchar *physical = NULL;
    gchar *resolved = NULL;
    RadioSFileInfo info;

    if (normalized == NULL || !radios_path_is_inside(root, normalized)) {
        g_set_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID,
                    "Yapılandırma yolu RadioS dışına çıkıyor: %s", value);
        goto done;
    }
    if (!components_valid(root, normalized)) {
        g_set_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID,
                    "Yapılandırma yolu Windows'ta geçersiz bir ad içeriyor: %s", value);
        goto done;
    }

    existing = g_strdup(normalized);
    while (radios_fs_stat(existing, FALSE, &info) != 0) {
        gchar *parent;
        if (errno != ENOENT) {
            g_set_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_IO,
                        "Yol incelenemedi: %s (%s)", existing,
                        g_strerror(errno));
            goto done;
        }
        parent = g_path_get_dirname(existing);
        /* The root itself exists, so the walk ends inside it; a vanished root
         * (or a dirname that stops converging) is an error, never a loop. */
        if (g_str_equal(parent, existing) || !radios_path_is_inside(root, parent)) {
            g_free(parent);
            g_set_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_IO,
                        "Yolun mevcut üst dizini bulunamadı: %s", value);
            goto done;
        }
        g_free(existing);
        existing = parent;
    }

    physical = radios_fs_realpath(existing, NULL);
    if (physical == NULL) {
        g_set_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_IO,
                    "Yol çözülemedi: %s (%s)", existing, g_strerror(errno));
        goto done;
    }
    if (!radios_path_is_inside(root, physical)) {
        g_set_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID,
                    "Sembolik bağlantı RadioS dışına çıkıyor: %s", value);
        goto done;
    }
    {
        /* Append the missing tail to the physical prefix.  A drive root
         * already ends with its separator; a subst or mapped prefix may
         * resolve to a longer physical path, so the separator is explicit. */
        const gchar *suffix = normalized + strlen(existing);
        gsize physical_length = strlen(physical);
        while (G_IS_DIR_SEPARATOR(*suffix))
            ++suffix;
        if (*suffix == '\0')
            resolved = g_strdup(physical);
        else
            resolved = g_strconcat(physical,
                                   physical_length > 0 && G_IS_DIR_SEPARATOR(physical[physical_length - 1]) ?
                                   "" : G_DIR_SEPARATOR_S, suffix, NULL);
    }

done:
    g_free(normalized);
    g_free(existing);
    g_free(physical);
    return resolved;
}

static gboolean
has_root_markers(const gchar *root)
{
    gchar *config_marker = g_build_filename(root, "config",
                                           "radios.example.ini", NULL);
    gchar *database_marker = g_build_filename(root, "database", "SDBASE",
                                             "pyproject.toml", NULL);
    gboolean valid = g_file_test(root, G_FILE_TEST_IS_DIR) &&
                     g_file_test(config_marker, G_FILE_TEST_IS_REGULAR) &&
                     g_file_test(database_marker, G_FILE_TEST_IS_REGULAR);
    g_free(config_marker);
    g_free(database_marker);
    return valid;
}

static gchar *
find_from_directory(const gchar *directory)
{
    gchar *candidate = g_strdup(directory);
    for (guint step = 0; candidate != NULL && step < WALK_LIMIT; ++step) {
        gchar *parent;
        if (has_root_markers(candidate))
            return candidate;
        parent = g_path_get_dirname(candidate);
        /* "C:\" and "\\server\share" are fixpoints of the walk. */
        if (g_str_equal(candidate, parent) || radios_path_is_root(candidate) ||
            !radios_path_is_absolute(parent)) {
            g_free(parent);
            break;
        }
        g_free(candidate);
        candidate = parent;
    }
    g_free(candidate);
    return NULL;
}

gchar *
radios_project_root_find(const gchar *explicit_root,
                        const gchar *executable_path, GError **error)
{
    gchar *physical;
    gchar *directory;
    gchar *found = NULL;

    if (explicit_root != NULL) {
        if (*explicit_root == '\0') {
            g_set_error_literal(error, RADIOS_CONFIG_ERROR,
                                RADIOS_CONFIG_ERROR_INVALID,
                                "RadioS proje kökü boş olamaz.");
            return NULL;
        }
        physical = radios_fs_realpath(explicit_root, NULL);
        if (physical == NULL || !has_root_markers(physical)) {
            g_set_error(error, RADIOS_CONFIG_ERROR,
                        RADIOS_CONFIG_ERROR_INVALID,
                        "Geçerli RadioS proje kökü değil: %s", explicit_root);
            g_free(physical);
            return NULL;
        }
        return physical;
    }

    if (executable_path != NULL) {
        physical = radios_fs_realpath(executable_path, NULL);
        if (physical != NULL) {
            directory = g_path_get_dirname(physical);
            found = find_from_directory(directory);
            g_free(directory);
            g_free(physical);
        }
    }
    if (found == NULL) {
        directory = g_get_current_dir();
        physical = directory != NULL ? radios_fs_realpath(directory, NULL) : NULL;
        g_free(directory);
        if (physical != NULL) {
            found = find_from_directory(physical);
            g_free(physical);
        }
    }
    if (found == NULL)
        g_set_error_literal(error, RADIOS_CONFIG_ERROR,
                            RADIOS_CONFIG_ERROR_INVALID,
                            "RadioS proje kökü bulunamadı; --project-root ile belirtin.");
    return found;
}

static gchar *
required_string(GKeyFile *file, const gchar *group, const gchar *key,
                GError **error)
{
    GError *local_error = NULL;
    gchar *value = g_key_file_get_string(file, group, key, &local_error);
    if (value == NULL) {
        g_set_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID,
                    "Yapılandırma [%s] %s okunamadı: %s", group, key,
                    local_error->message);
        g_error_free(local_error);
        return NULL;
    }
    g_strstrip(value);
    if (*value == '\0' || !g_utf8_validate(value, -1, NULL)) {
        g_set_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID,
                    "Yapılandırma [%s] %s boş veya geçersiz.", group, key);
        g_free(value);
        return NULL;
    }
    return value;
}

static gboolean
required_integer(GKeyFile *file, const gchar *group, const gchar *key,
                 gint minimum, gint maximum, gint *out, GError **error)
{
    GError *local_error = NULL;
    gint value = g_key_file_get_integer(file, group, key, &local_error);
    if (local_error != NULL || value < minimum || value > maximum) {
        g_set_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID,
                    "Yapılandırma [%s] %s, %d–%d arasında bir tamsayı olmalı.",
                    group, key, minimum, maximum);
        g_clear_error(&local_error);
        return FALSE;
    }
    *out = value;
    return TRUE;
}

static gboolean
read_path(GKeyFile *file, const gchar *root, const gchar *group,
          const gchar *key, gboolean directory, gchar **out, GError **error)
{
    gchar *value = required_string(file, group, key, error);
    gchar *resolved;
    RadioSFileInfo info;
    if (value == NULL)
        return FALSE;
    resolved = resolve_inside_root(root, value, error);
    g_free(value);
    if (resolved == NULL)
        return FALSE;
    if (radios_fs_stat(resolved, TRUE, &info) == 0 &&
        info.type != (directory ? RADIOS_FILE_TYPE_DIRECTORY : RADIOS_FILE_TYPE_REGULAR)) {
        g_set_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID,
                    "Yapılandırma [%s] %s yanlış dosya türüne işaret ediyor.",
                    group, key);
        g_free(resolved);
        return FALSE;
    }
    *out = resolved;
    return TRUE;
}

/* [database] python_command (DECISIONS D4, identical to
 * tools/radios_platform.py resolve_python):
 *  - a value with '\', '/' or ':' is a path: absolute ("C:\...", UNC) or
 *    relative to the project root (never to the CWD), naming an .exe; a
 *    configured interpreter that is missing fails closed at spawn time
 *    (radios_config_python_executable), it is never replaced by another one;
 *  - empty is rejected by required_string(); the shipped default names
 *    python3/python[.exe] mean the bundled runtime
 *    <root>\runtime\python\python.exe, else <root>\python\python.exe
 *    (installed layout: <prefix>\bin\RadioS.exe next to <prefix>\python);
 *  - any other bare name is invalid: PATH, py.exe and the Microsoft Store
 *    "python3" alias are never consulted. */
static gboolean
python_default_name(const gchar *value)
{
    static const gchar *const names[] = {"python3", "python", "python3.exe", "python.exe"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i)
        if (g_ascii_strcasecmp(value, names[i]) == 0)
            return TRUE;
    return FALSE;
}

static gboolean
regular_file(const gchar *path)
{
    RadioSFileInfo info;
    return radios_fs_stat(path, TRUE, &info) == 0 && info.type == RADIOS_FILE_TYPE_REGULAR;
}

static gchar *
resolve_python_command(const gchar *root, const gchar *value, GError **error)
{
    if (strpbrk(value, "\\/:") != NULL) {
        gchar *candidate;
        if (radios_path_is_absolute(value)) {
            candidate = radios_path_normalize(value);
        } else if (strchr(value, ':') != NULL || G_IS_DIR_SEPARATOR(value[0])) {
            g_set_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID,
                        "python_command mutlak bir yol (örn. C:\\RadioS\\runtime\\python\\python.exe) "
                        "ya da proje köküne göre göreli bir yol olmalı: %s", value);
            return NULL;
        } else {
            gchar *native = radios_path_from_portable(value);
            gchar *joined = g_build_filename(root, native, NULL);
            candidate = radios_path_normalize(joined);
            g_free(joined);
            g_free(native);
        }
        gsize length = candidate != NULL ? strlen(candidate) : 0;
        if (length < 5 || g_ascii_strcasecmp(candidate + length - 4, ".exe") != 0 ||
            G_IS_DIR_SEPARATOR(candidate[length - 5])) {
            g_free(candidate);
            g_set_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID,
                        "python_command bir Python yorumlayıcısının .exe dosyasını göstermeli: %s", value);
            return NULL;
        }
        return candidate;
    }
    if (!python_default_name(value)) {
        g_set_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID,
                    "python_command, python.exe dosyasının yolu ya da varsayılan \"python3\" olmalı: %s", value);
        return NULL;
    }
    {
        gchar *bundled = g_build_filename(root, "runtime", "python", "python.exe", NULL);
        gchar *packaged;
        if (regular_file(bundled))
            return bundled;
        packaged = g_build_filename(root, "python", "python.exe", NULL);
        if (regular_file(packaged)) {
            g_free(bundled);
            return packaged;
        }
        g_free(packaged);
        /* Missing runtime: keep the documented location so the spawn fails
         * closed with a clear message instead of searching PATH. */
        return bundled;
    }
}

gchar *
radios_config_python_executable(const RadioSConfig *config, GError **error)
{
    const gchar *command = config != NULL ? config->python_command : NULL;
    if (command == NULL || !radios_path_is_absolute(command)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_FILENAME,
                    "Python yorumlayıcısının yolu mutlak bir Windows yolu olmalı: %s",
                    command != NULL ? command : "(yok)");
        return NULL;
    }
    if (!regular_file(command)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Python çalışma ortamı bulunamadı: %s (config\\radios.ini [database] python_command)",
                    command);
        return NULL;
    }
    return g_strdup(command);
}

gchar **
radios_config_python_argv(const RadioSConfig *config, const gchar *script,
                          const gchar *const *arguments, GError **error)
{
    static const gchar *const flags[] = {RADIOS_PYTHON_FLAGS};
    gchar *python;
    GPtrArray *argv;
    g_return_val_if_fail(script != NULL, NULL);
    python = radios_config_python_executable(config, error);
    if (python == NULL)
        return NULL;
    argv = g_ptr_array_new();
    g_ptr_array_add(argv, python);
    for (guint i = 0; i < G_N_ELEMENTS(flags); ++i)
        g_ptr_array_add(argv, g_strdup(flags[i]));
    g_ptr_array_add(argv, g_strdup(script));
    for (gsize i = 0; arguments != NULL && arguments[i] != NULL; ++i)
        g_ptr_array_add(argv, g_strdup(arguments[i]));
    g_ptr_array_add(argv, NULL);
    return (gchar **)g_ptr_array_free(argv, FALSE);
}

const gchar *const *
radios_config_python_environment_overrides(void)
{
    static const gchar *const overrides[] = {
        "PYTHONUTF8=1", "PYTHONDONTWRITEBYTECODE=1", "PYTHONIOENCODING=utf-8",
        "PYTHONHOME", "PYTHONPATH", NULL
    };
    return overrides;
}

gboolean
radios_config_profile_is_valid(const gchar *profile)
{
    static const gchar *const profiles[] = {
        "broadcast", "playlist", "automation", "dj", "emergency",
        "advert", "technical", "routing", NULL
    };
    if (!profile) return FALSE;
    for (gsize i = 0; profiles[i] != NULL; ++i)
        if (g_str_equal(profile, profiles[i]))
            return TRUE;
    return FALSE;
}

static gboolean
environment_name_is_valid(const gchar *name)
{
    if (!(g_ascii_isalpha(*name) || *name == '_'))
        return FALSE;
    for (const gchar *p = name + 1; *p != '\0'; ++p)
        if (!(g_ascii_isalnum(*p) || *p == '_'))
            return FALSE;
    return TRUE;
}

static gboolean
service_host_is_valid(const gchar *host)
{
    GInetAddress *address = g_inet_address_new_from_string(host);
    gchar *ascii;
    gchar **labels;
    gboolean valid = TRUE;
    gboolean only_digits_and_dots = TRUE;
    if (address != NULL) {
        g_object_unref(address);
        return TRUE;
    }
    ascii = g_hostname_to_ascii(host);
    if (ascii == NULL || *ascii == '\0' || strlen(ascii) > 253) {
        g_free(ascii);
        return FALSE;
    }
    for (const gchar *p = ascii; *p != '\0'; ++p)
        if (!g_ascii_isdigit(*p) && *p != '.')
            only_digits_and_dots = FALSE;
    if (only_digits_and_dots) {
        g_free(ascii);
        return FALSE;
    }
    if (ascii[strlen(ascii) - 1] == '.')
        ascii[strlen(ascii) - 1] = '\0';
    labels = g_strsplit(ascii, ".", -1);
    for (gsize i = 0; labels[i] != NULL; ++i) {
        gsize length = strlen(labels[i]);
        if (length == 0 || length > 63 ||
            !g_ascii_isalnum(labels[i][0]) ||
            !g_ascii_isalnum(labels[i][length - 1])) {
            valid = FALSE;
            break;
        }
        for (const gchar *p = labels[i]; *p != '\0'; ++p) {
            if (!g_ascii_isalnum(*p) && *p != '-') {
                valid = FALSE;
                break;
            }
        }
        if (!valid)
            break;
    }
    g_strfreev(labels);
    g_free(ascii);
    return valid;
}

static gboolean
service_url_is_valid(const gchar *url)
{
    GError *error = NULL;
    GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, &error);
    const gchar *scheme;
    const gchar *host;
    const gchar *path;
    gint port;
    gboolean valid;
    if (uri == NULL) {
        g_clear_error(&error);
        return FALSE;
    }
    scheme = g_uri_get_scheme(uri);
    host = g_uri_get_host(uri);
    path = g_uri_get_path(uri);
    port = g_uri_get_port(uri);
    valid = scheme != NULL &&
            (g_ascii_strcasecmp(scheme, "http") == 0 ||
             g_ascii_strcasecmp(scheme, "https") == 0) &&
            host != NULL && *host != '\0' && service_host_is_valid(host) &&
            (port == -1 || (port >= 1 && port <= 65535)) &&
            g_uri_get_userinfo(uri) == NULL &&
            g_uri_get_query(uri) == NULL &&
            g_uri_get_fragment(uri) == NULL &&
            (path == NULL || *path == '\0' || g_str_equal(path, "/"));
    g_uri_unref(uri);
    return valid;
}

/* GKeyFile recognizes '#'; existing RadioS INI files also use the standard
 * full-line ';' comment form. Normalize only the first non-space character
 * of a line, preserving semicolons in values and both LF/CRLF line endings. */
static gboolean
load_ini_file(GKeyFile *file, const gchar *path, GError **error)
{
    gchar *contents = NULL;
    gsize length = 0;
    gboolean loaded;
    if (!g_file_get_contents(path, &contents, &length, error))
        return FALSE;
    for (gchar *line = contents; line < contents + length;) {
        gchar *end = memchr(line, '\n', (gsize)(contents + length - line));
        gchar *first = line;
        if (end == NULL)
            end = contents + length;
        while (first < end && g_ascii_isspace(*first))
            ++first;
        if (first < end && *first == ';')
            *first = '#';
        line = end < contents + length ? end + 1 : end;
    }
    loaded = g_key_file_load_from_data(file, contents, length,
                                        G_KEY_FILE_NONE, error);
    g_free(contents);
    return loaded;
}

gboolean
radios_config_load(const gchar *project_root, const gchar *config_path,
                   RadioSConfig *out, GError **error)
{
    RadioSConfig config = {0};
    GKeyFile *file;
    GError *local_error = NULL;
    gint version = 0;
    gchar *selected = NULL;
    gboolean success = FALSE;
    RadioSFileInfo info;

    g_return_val_if_fail(out != NULL, FALSE);
    file = g_key_file_new();
    if (project_root == NULL) {
        g_set_error_literal(error, RADIOS_CONFIG_ERROR,
                            RADIOS_CONFIG_ERROR_INVALID,
                            "Yapılandırma için RadioS proje kökü gerekli.");
        goto done;
    }
    config.project_root = radios_project_root_find(project_root, NULL, error);
    if (config.project_root == NULL)
        goto done;
    if (config_path != NULL)
        selected = g_strdup(config_path);
    else {
        gchar *local_path = g_build_filename(config.project_root, "config",
                                             "radios.ini", NULL);
        gint status = radios_fs_stat(local_path, FALSE, &info);
        if (status != 0 && errno != ENOENT) {
            g_set_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_IO,
                        "Yerel yapılandırma dosyası incelenemedi: %s (%s)",
                        local_path, g_strerror(errno));
            g_free(local_path);
            goto done;
        }
        selected = g_strdup(status == 0 ?
                            "config/radios.ini" : "config/radios.example.ini");
        g_free(local_path);
    }
    if (*selected == '\0') {
        g_set_error_literal(error, RADIOS_CONFIG_ERROR,
                            RADIOS_CONFIG_ERROR_INVALID,
                            "Yapılandırma dosyası yolu boş olamaz.");
        goto done;
    }
    config.config_file = resolve_inside_root(config.project_root, selected, error);
    if (config.config_file == NULL)
        goto done;
    if (!load_ini_file(file, config.config_file, &local_error)) {
        g_set_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_IO,
                    "Yapılandırma dosyası okunamadı: %s (%s)",
                    config.config_file, local_error->message);
        g_clear_error(&local_error);
        goto done;
    }
    if (!required_integer(file, "application", "config_version", 1, 1,
                          &version, error))
        goto done;
    config.name = required_string(file, "application", "name", error);
    if (config.name == NULL)
        goto done;
    config.initial_profile = required_string(file, "application", "initial_profile", error);
    if (config.initial_profile == NULL)
        goto done;
    if (!radios_config_profile_is_valid(config.initial_profile)) {
        g_set_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID,
                    "Bilinmeyen başlangıç profili: %s", config.initial_profile);
        goto done;
    }
    config.auto_start_playback = g_key_file_get_boolean(file, "application",
                                                      "auto_start_playback",
                                                      &local_error);
    if (local_error != NULL) {
        g_set_error(error, RADIOS_CONFIG_ERROR, RADIOS_CONFIG_ERROR_INVALID,
                    "auto_start_playback geçerli bir boolean olmalı: %s",
                    local_error->message);
        g_clear_error(&local_error);
        goto done;
    }

#define READ_PATH(group, key, directory, member) \
    if (!read_path(file, config.project_root, group, key, directory, \
                   &config.member, error)) goto done
    READ_PATH("paths", "assets_root", TRUE, assets_root);
    READ_PATH("paths", "database_file", FALSE, database_file);
    READ_PATH("paths", "endpoint_root", TRUE, endpoint_root);
    READ_PATH("paths", "import_root", TRUE, import_root);
    READ_PATH("paths", "cache_root", TRUE, cache_root);
    READ_PATH("paths", "log_root", TRUE, log_root);
    READ_PATH("paths", "recording_root", TRUE, recording_root);
    READ_PATH("paths", "backup_root", TRUE, backup_root);
    READ_PATH("database", "source_dir", TRUE, database_source_dir);
#undef READ_PATH
    /* config/storage.ini (Ayarlar › Medya arşivi) may move the database,
     * recordings and backups to folders RadioS prepared outside the project. */
    if (!radios_storage_apply(&config, error))
        goto done;

    config.service_url = required_string(file, "database", "service_url", error);
    if (config.service_url == NULL)
        goto done;
    if (!service_url_is_valid(config.service_url)) {
        g_set_error_literal(error, RADIOS_CONFIG_ERROR,
                            RADIOS_CONFIG_ERROR_INVALID,
                            "service_url, kullanıcı bilgisi olmadan geçerli bir HTTP(S) servis adresi olmalı.");
        goto done;
    }
    {
        gchar *python_value = required_string(file, "database", "python_command", error);
        if (python_value == NULL)
            goto done;
        config.python_command = resolve_python_command(config.project_root, python_value, error);
        g_free(python_value);
        if (config.python_command == NULL)
            goto done;
    }
    config.token_environment = required_string(file, "database", "token_environment", error);
    if (config.token_environment == NULL)
        goto done;
    if (!environment_name_is_valid(config.token_environment)) {
        g_set_error_literal(error, RADIOS_CONFIG_ERROR,
                            RADIOS_CONFIG_ERROR_INVALID,
                            "token_environment geçerli bir ortam değişkeni adı olmalı.");
        goto done;
    }
    if (!required_integer(file, "database", "connect_timeout_seconds", 1, 300,
                          &config.connect_timeout_seconds, error) ||
        !required_integer(file, "database", "request_timeout_seconds", 1, 3600,
                          &config.request_timeout_seconds, error))
        goto done;
    if (config.request_timeout_seconds < config.connect_timeout_seconds) {
        g_set_error_literal(error, RADIOS_CONFIG_ERROR,
                            RADIOS_CONFIG_ERROR_INVALID,
                            "İstek zaman aşımı bağlantı zaman aşımından kısa olamaz.");
        goto done;
    }

    *out = config;
    memset(&config, 0, sizeof(config));
    success = TRUE;

done:
    radios_config_clear(&config);
    g_free(selected);
    g_key_file_unref(file);
    return success;
}

void
radios_config_clear(RadioSConfig *config)
{
    if (config == NULL)
        return;
    g_free(config->project_root);
    g_free(config->config_file);
    g_free(config->name);
    g_free(config->initial_profile);
    g_free(config->assets_root);
    g_free(config->database_file);
    g_free(config->endpoint_root);
    g_free(config->import_root);
    g_free(config->cache_root);
    g_free(config->log_root);
    g_free(config->recording_root);
    g_free(config->backup_root);
    g_free(config->database_source_dir);
    g_free(config->service_url);
    g_free(config->python_command);
    g_free(config->token_environment);
    memset(config, 0, sizeof(*config));
}
