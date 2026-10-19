/* RadioS platform layer: IANA time zones on Windows.
 *
 * GLib on Windows only understands constant offsets, POSIX TZ rule strings and
 * Windows registry zone names.  RadioS persists IANA identifiers (the SDBASE
 * service uses Python zoneinfo + the bundled tzdata), so this file maps them:
 *
 *  1. "UTC" -> g_time_zone_new_utc().
 *  2. Strict IANA syntax; the id must exist in the bundled tzdata tree
 *     (runtime\share\zoneinfo, TZif v2+), exactly the database of the service.
 *  3. The system ICU (icu.dll) maps the id to a Windows zone whose registry
 *     rules carry historical DST.  The ICU mapping data (2022g on Windows 11) and
 *     GLib's registry reader are both known to be wrong for some zones (zones that
 *     changed after 2022, "UTC-02"-style names that GLib parses as POSIX strings
 *     with an inverted sign, Volgograd), so the registry zone is accepted only
 *     when it agrees with the tzdata rules from one year back to five years ahead.
 *  4. Otherwise the TZif footer (the current POSIX rule, normalised so that GLib
 *     evaluates negative DST such as Europe/Dublin correctly) is used.
 *  5. Every identifier gets a GTimeZone object of its own (GLib caches zones by
 *     the identifier string, so aliases are separated with case variants of the
 *     registry name / equivalent spellings of the footer offset); the pointer ->
 *     IANA id registry makes radios_time_zone_get_identifier() exact. */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winnls.h>

#include "radios/platform.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ ICU (system icu.dll, C API) */

typedef gint32 (*IcuWindowsIdFunc)(const gunichar2 *id, gint32 length, gunichar2 *winid, gint32 capacity, gint *status);
typedef gint32 (*IcuIanaIdFunc)(const gunichar2 *winid, gint32 length, const gchar *region, gunichar2 *id,
                                gint32 capacity, gint *status);
typedef gint32 (*IcuCanonicalFunc)(const gunichar2 *id, gint32 length, gunichar2 *result, gint32 capacity,
                                   gint8 *is_system, gint *status);
typedef gpointer (*IcuEnumOpenFunc)(gint type, const gchar *region, const gint32 *raw_offset, gint *status);
typedef const gchar *(*IcuEnumNextFunc)(gpointer en, gint32 *length, gint *status);
typedef void (*IcuEnumCloseFunc)(gpointer en);

#define ICU_ZONE_TYPE_CANONICAL 1
#define ICU_FAILURE(status) ((status) > 0)

static struct {
    IcuWindowsIdFunc windows_id;
    IcuIanaIdFunc iana_id;
    IcuCanonicalFunc canonical;
    IcuEnumOpenFunc enum_open;
    IcuEnumNextFunc enum_next;
    IcuEnumCloseFunc enum_close;
    gchar region[16];
} icu;

static gpointer icu_load(gpointer unused)
{
    (void)unused;
    radios_platform_init();
    HMODULE calendar = LoadLibraryExW(L"icu.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32), common = calendar;
    if (!calendar) {
        /* Windows 10 1703-1809 shipped the same C API split in two DLLs. */
        calendar = LoadLibraryExW(L"icuin.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        common = LoadLibraryExW(L"icuuc.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    if (calendar) {
        icu.windows_id = (IcuWindowsIdFunc)(void (*)(void))GetProcAddress(calendar, "ucal_getWindowsTimeZoneID");
        icu.iana_id = (IcuIanaIdFunc)(void (*)(void))GetProcAddress(calendar, "ucal_getTimeZoneIDForWindowsID");
        icu.canonical = (IcuCanonicalFunc)(void (*)(void))GetProcAddress(calendar, "ucal_getCanonicalTimeZoneID");
        icu.enum_open = (IcuEnumOpenFunc)(void (*)(void))GetProcAddress(calendar, "ucal_openTimeZoneIDEnumeration");
    }
    if (common) {
        icu.enum_next = (IcuEnumNextFunc)(void (*)(void))GetProcAddress(common, "uenum_next");
        icu.enum_close = (IcuEnumCloseFunc)(void (*)(void))GetProcAddress(common, "uenum_close");
    }
    WCHAR geo[16] = { 0 };
    if (GetUserDefaultGeoName(geo, G_N_ELEMENTS(geo)) > 1) {
        gchar *region = g_utf16_to_utf8((const gunichar2 *)geo, -1, NULL, NULL, NULL);
        gboolean plain = region && strlen(region) == 2 && g_ascii_isalpha(region[0]) && g_ascii_isalpha(region[1]);
        if (plain) g_strlcpy(icu.region, region, sizeof icu.region);
        g_free(region);
    }
    return NULL;
}

static void icu_init(void)
{
    static GOnce once = G_ONCE_INIT;
    g_once(&once, icu_load, NULL);
}

/* IANA -> Windows zone name, NULL when ICU has no mapping. */
static gchar *icu_windows_zone(const gchar *iana)
{
    icu_init();
    if (!icu.windows_id) return NULL;
    gunichar2 *wide = g_utf8_to_utf16(iana, -1, NULL, NULL, NULL);
    gunichar2 out[128];
    gint status = 0;
    gint32 length = wide ? icu.windows_id(wide, -1, out, G_N_ELEMENTS(out), &status) : 0;
    g_free(wide);
    if (ICU_FAILURE(status) || length <= 0 || length >= (gint32)G_N_ELEMENTS(out)) return NULL;
    return g_utf16_to_utf8(out, length, NULL, NULL, NULL);
}

/* Windows zone name -> IANA id (user's region first, then the CLDR golden zone). */
static gchar *icu_iana_zone(const gchar *windows)
{
    icu_init();
    if (!icu.iana_id) return NULL;
    gunichar2 *wide = g_utf8_to_utf16(windows, -1, NULL, NULL, NULL);
    gchar *result = NULL;
    for (guint pass = 0; wide && !result && pass < 2; ++pass) {
        const gchar *region = pass == 0 ? (icu.region[0] ? icu.region : NULL) : NULL;
        if (pass == 1 && !icu.region[0]) break;
        gunichar2 out[128];
        gint status = 0;
        gint32 length = icu.iana_id(wide, -1, region, out, G_N_ELEMENTS(out), &status);
        if (!ICU_FAILURE(status) && length > 0 && length < (gint32)G_N_ELEMENTS(out))
            result = g_utf16_to_utf8(out, length, NULL, NULL, NULL);
    }
    g_free(wide);
    return result;
}

/* ICU canonical form of a known (system) id, else NULL. */
static gchar *icu_canonical_zone(const gchar *iana)
{
    icu_init();
    if (!icu.canonical) return NULL;
    gunichar2 *wide = g_utf8_to_utf16(iana, -1, NULL, NULL, NULL);
    gunichar2 out[128];
    gint8 is_system = 0;
    gint status = 0;
    gint32 length = wide ? icu.canonical(wide, -1, out, G_N_ELEMENTS(out), &is_system, &status) : 0;
    g_free(wide);
    if (ICU_FAILURE(status) || !is_system || length <= 0 || length >= (gint32)G_N_ELEMENTS(out)) return NULL;
    return g_utf16_to_utf8(out, length, NULL, NULL, NULL);
}

/* ------------------------------------------------------------------ identifiers and the bundled tree */

static gboolean tz_syntax_valid(const gchar *id)
{
    if (!id) return FALSE;
    gsize length = strlen(id);
    if (length == 0 || length > 64 || id[0] == '/' || id[length - 1] == '/') return FALSE;
    for (gsize i = 0; i < length; ++i) {
        gchar c = id[i];
        if (c == '/') { if (id[i + 1] == '/') return FALSE; continue; }
        if (!g_ascii_isalnum(c) && c != '_' && c != '+' && c != '-') return FALSE;
    }
    return TRUE;
}

static gchar *module_directory(HMODULE module)
{
    gchar *directory = NULL;
    DWORD capacity = MAX_PATH;
    for (guint attempt = 0; attempt < 6 && !directory; ++attempt, capacity *= 4) {
        WCHAR *buffer = g_new(WCHAR, capacity);
        DWORD length = GetModuleFileNameW(module, buffer, capacity);
        if (length > 0 && length < capacity) {
            gchar *path = g_utf16_to_utf8((const gunichar2 *)buffer, length, NULL, NULL, NULL);
            if (path) directory = g_path_get_dirname(path);
            g_free(path);
            g_free(buffer);
            break;
        }
        g_free(buffer);
        if (length == 0) break;
    }
    return directory;
}

/* <prefix>\bin\RadioS.exe -> <prefix>\share\zoneinfo (installed and dev runtime);
 * executables elsewhere below the project (build\, tests) find
 * <root>\runtime\share\zoneinfo by walking up; the GLib prefix comes last. */
static gpointer tz_find_bundled(gpointer unused)
{
    (void)unused;
    gchar *exe = module_directory(NULL);
    gchar *glib = module_directory(GetModuleHandleW(L"libglib-2.0-0.dll"));
    GPtrArray *candidates = g_ptr_array_new_with_free_func(g_free);
    if (exe) {
        g_ptr_array_add(candidates, g_build_filename(exe, "..", "share", "zoneinfo", NULL));
        g_ptr_array_add(candidates, g_build_filename(exe, "share", "zoneinfo", NULL));
        gchar *ancestor = g_strdup(exe);
        for (guint level = 0; level < 8; ++level) {
            g_ptr_array_add(candidates, g_build_filename(ancestor, "runtime", "share", "zoneinfo", NULL));
            gchar *parent = g_path_get_dirname(ancestor);
            gboolean top = g_str_equal(parent, ancestor);
            g_free(ancestor);
            ancestor = parent;
            if (top) break;
        }
        g_free(ancestor);
    }
    if (glib) g_ptr_array_add(candidates, g_build_filename(glib, "..", "share", "zoneinfo", NULL));
    const gchar *found = NULL;
    for (guint i = 0; i < candidates->len && !found; ++i) {
        gchar *marker = g_build_filename(candidates->pdata[i], "Etc", "UTC", NULL);
        if (g_file_test(marker, G_FILE_TEST_IS_REGULAR)) {
            gchar *normal = radios_path_normalize(candidates->pdata[i]);
            found = g_intern_string(normal ? normal : candidates->pdata[i]);
            g_free(normal);
        }
        g_free(marker);
    }
    if (!found) {
        gchar *fallback = exe ? g_build_filename(exe, "..", "share", "zoneinfo", NULL) : g_strdup("share\\zoneinfo");
        gchar *normal = radios_path_normalize(fallback);
        found = g_intern_string(normal ? normal : fallback);
        g_free(normal); g_free(fallback);
    }
    g_ptr_array_unref(candidates);
    g_free(exe); g_free(glib);
    return (gpointer)found;
}

const gchar *radios_time_zone_directory(void)
{
    const gchar *env = g_getenv("TZDIR");
    if (env && *env) return g_intern_string(env);
    static GOnce once = G_ONCE_INIT;
    return g_once(&once, tz_find_bundled, NULL);
}

static gboolean tz_tree_available(void)
{
    gchar *marker = g_build_filename(radios_time_zone_directory(), "Etc", "UTC", NULL);
    gboolean available = g_file_test(marker, G_FILE_TEST_IS_REGULAR);
    g_free(marker);
    return available;
}

/* ------------------------------------------------------------------ TZif (RFC 8536) */

typedef struct {
    gint64 *times;        /* transition instants, ascending */
    guint8 *indices;      /* local time type of each transition */
    gint32 *offsets;      /* per type: seconds east of UTC */
    guint8 *designations; /* per type: index into chars */
    gchar *chars;         /* NUL-separated abbreviations, NUL-terminated */
    guint timecnt, typecnt, charcnt;
    gchar *footer;        /* POSIX TZ rule for instants after the last transition, or NULL */
    gchar *glib_footer;   /* footer rewritten so GLib evaluates it correctly, or NULL */
    GTimeZone *footer_zone;
} TzData;

static guint32 be32(const guint8 *p)
{
    return ((guint32)p[0] << 24) | ((guint32)p[1] << 16) | ((guint32)p[2] << 8) | (guint32)p[3];
}

static gint64 be64(const guint8 *p)
{
    return (gint64)(((guint64)be32(p) << 32) | (guint64)be32(p + 4));
}

typedef struct { guint32 isutcnt, isstdcnt, leapcnt, timecnt, typecnt, charcnt; } TzifCounts;

static gboolean tzif_header(const guint8 *data, gsize length, gsize offset, TzifCounts *counts)
{
    if (offset > length || length - offset < 44 || memcmp(data + offset, "TZif", 4) != 0) return FALSE;
    const guint8 *p = data + offset + 20;
    counts->isutcnt = be32(p); counts->isstdcnt = be32(p + 4); counts->leapcnt = be32(p + 8);
    counts->timecnt = be32(p + 12); counts->typecnt = be32(p + 16); counts->charcnt = be32(p + 20);
    return counts->typecnt >= 1 && counts->typecnt <= 256 && counts->charcnt >= 1 && counts->charcnt <= 65536 &&
           counts->timecnt <= 1000000 && counts->leapcnt <= 100000 &&
           (counts->isstdcnt == 0 || counts->isstdcnt == counts->typecnt) &&
           (counts->isutcnt == 0 || counts->isutcnt == counts->typecnt);
}

static guint64 tzif_block_size(const TzifCounts *c, guint time_size)
{
    return (guint64)c->timecnt * time_size + c->timecnt + (guint64)c->typecnt * 6 + c->charcnt +
           (guint64)c->leapcnt * (time_size + 4) + c->isstdcnt + c->isutcnt;
}

static void tz_data_free(TzData *d)
{
    if (!d) return;
    g_free(d->times); g_free(d->indices); g_free(d->offsets); g_free(d->designations);
    g_free(d->chars); g_free(d->footer); g_free(d->glib_footer);
    if (d->footer_zone) g_time_zone_unref(d->footer_zone);
    g_free(d);
}

/* POSIX TZ string pieces (only what the normalisation needs). */
typedef struct {
    gchar *std_name, *dst_name;  /* as written, including <> quotes */
    gint32 std_offset, dst_offset; /* POSIX sign: seconds WEST of UTC */
    gboolean has_dst, explicit_dst_offset;
    gchar *start_rule, *end_rule;  /* "M3.5.0/3" etc., NULL without rules */
} PosixTz;

static void posix_tz_clear(PosixTz *tz)
{
    g_free(tz->std_name); g_free(tz->dst_name); g_free(tz->start_rule); g_free(tz->end_rule);
    memset(tz, 0, sizeof *tz);
}

static gboolean posix_name(const gchar **pos, gchar **name)
{
    const gchar *p = *pos, *start = p;
    if (*p == '<') {
        ++p;
        while (*p && *p != '>') { if (!g_ascii_isalnum(*p) && *p != '+' && *p != '-') return FALSE; ++p; }
        if (*p != '>' || p - start < 4) return FALSE;
        ++p;
    } else {
        while (g_ascii_isalpha(*p)) ++p;
        if (p - start < 3) return FALSE;
    }
    *name = g_strndup(start, (gsize)(p - start));
    *pos = p;
    return TRUE;
}

/* [+-]hh[:mm[:ss]] -> seconds (sign as written). */
static gboolean posix_offset(const gchar **pos, gint32 *seconds)
{
    const gchar *p = *pos;
    gint sign = 1;
    if (*p == '+' || *p == '-') { if (*p == '-') sign = -1; ++p; }
    if (!g_ascii_isdigit(*p)) return FALSE;
    gint value[3] = { 0, 0, 0 };
    for (guint part = 0; part < 3; ++part) {
        if (part > 0) { if (*p != ':') break; ++p; if (!g_ascii_isdigit(*p)) return FALSE; }
        guint digits = 0;
        while (g_ascii_isdigit(*p) && digits < 3) { value[part] = value[part] * 10 + (*p - '0'); ++p; ++digits; }
        if (g_ascii_isdigit(*p)) return FALSE;
    }
    if (value[0] > 167 || value[1] > 59 || value[2] > 59) return FALSE;
    *seconds = sign * (value[0] * 3600 + value[1] * 60 + value[2]);
    *pos = p;
    return TRUE;
}

static gboolean posix_tz_parse(const gchar *text, PosixTz *tz)
{
    memset(tz, 0, sizeof *tz);
    const gchar *p = text;
    if (!posix_name(&p, &tz->std_name) || !posix_offset(&p, &tz->std_offset)) goto invalid;
    if (*p == '\0') return TRUE;
    if (!posix_name(&p, &tz->dst_name)) goto invalid;
    tz->has_dst = TRUE;
    tz->dst_offset = tz->std_offset - 3600;
    if (*p && *p != ',') {
        if (!posix_offset(&p, &tz->dst_offset)) goto invalid;
        tz->explicit_dst_offset = TRUE;
    }
    if (*p == '\0') return TRUE;
    if (*p != ',') goto invalid;
    const gchar *comma = strchr(p + 1, ',');
    if (!comma || strchr(comma + 1, ',') || comma == p + 1 || comma[1] == '\0') goto invalid;
    tz->start_rule = g_strndup(p + 1, (gsize)(comma - p - 1));
    tz->end_rule = g_strdup(comma + 1);
    return TRUE;
invalid:
    posix_tz_clear(tz);
    return FALSE;
}

/* Spelling variant of an offset ("[+-]h[h][:mm[:ss]]", sign as given).
 * Variant 0 is the minimal form; the others are equivalent spellings, used to
 * obtain distinct GLib objects for zones that share one rule. */
static gchar *posix_offset_text(gint32 seconds, guint variant)
{
    gboolean negative = seconds < 0;
    guint32 value = negative ? (guint32)(-(gint64)seconds) : (guint32)seconds;
    guint hours = value / 3600, minutes = value / 60 % 60, secs = value % 60;
    /* forms: 0 "h", 1 "hh", 2 "h:mm", 3 "hh:mm", 4 "h:mm:ss", 5 "hh:mm:ss" */
    guint first = secs ? 4 : minutes ? 2 : 0, forms = 6 - first;
    guint signs = negative ? 1 : 2;
    if (variant >= forms * signs) return NULL;
    guint form = first + variant % forms;
    const gchar *sign = negative ? "-" : (variant / forms ? "+" : "");
    switch (form) {
    case 0: return g_strdup_printf("%s%u", sign, hours);
    case 1: return g_strdup_printf("%s%02u", sign, hours);
    case 2: return g_strdup_printf("%s%u:%02u", sign, hours, minutes);
    case 3: return g_strdup_printf("%s%02u:%02u", sign, hours, minutes);
    case 4: return g_strdup_printf("%s%u:%02u:%02u", sign, hours, minutes, secs);
    default: return g_strdup_printf("%s%02u:%02u:%02u", sign, hours, minutes, secs);
    }
}

/* GLib constant-offset spellings ("+03", "+03:00", "+0300", ...) of an offset
 * east of UTC; never "UTC" (that one is the shared UTC singleton). */
static void constant_offset_spellings(gint32 east, GPtrArray *out)
{
    gboolean negative = east < 0;
    guint32 value = negative ? (guint32)(-(gint64)east) : (guint32)east;
    guint hours = value / 3600, minutes = value / 60 % 60, secs = value % 60;
    if (hours > 24) return;
    for (guint sign = 0; sign < (negative ? 1u : 2u); ++sign) {
        const gchar *s = negative ? "-" : sign ? "" : "+";
        if (!minutes && !secs) {
            g_ptr_array_add(out, g_strdup_printf("%s%02u", s, hours));
            g_ptr_array_add(out, g_strdup_printf("%s%02u:00", s, hours));
            g_ptr_array_add(out, g_strdup_printf("%s%02u00", s, hours));
        }
        if (!secs) {
            g_ptr_array_add(out, g_strdup_printf("%s%02u:%02u", s, hours, minutes));
            g_ptr_array_add(out, g_strdup_printf("%s%02u%02u", s, hours, minutes));
        }
        g_ptr_array_add(out, g_strdup_printf("%s%02u:%02u:%02u", s, hours, minutes, secs));
        g_ptr_array_add(out, g_strdup_printf("%s%02u%02u%02u", s, hours, minutes, secs));
    }
}

/* GLib-safe spelling of a footer.  GLib treats an explicit DST offset of 0 as
 * "missing" and mis-evaluates negative DST (Europe/Dublin "IST-1GMT0,M10.5.0,
 * M3.5.0/1"); the equivalent positive form swaps standard/daylight time and the
 * two rules (the start time is in the old local time, so the times move with
 * their rules unchanged). */
static gchar *footer_for_glib(const gchar *footer, guint variant)
{
    PosixTz tz;
    if (!posix_tz_parse(footer, &tz)) return NULL;
    gchar *result = NULL;
    if (!tz.has_dst) {
        gchar *offset = posix_offset_text(tz.std_offset, variant);
        if (offset) result = g_strconcat(tz.std_name, offset, NULL);
        g_free(offset);
    } else if (tz.start_rule) {
        gboolean negative_dst = tz.dst_offset > tz.std_offset;
        const gchar *std_name = negative_dst ? tz.dst_name : tz.std_name;
        const gchar *dst_name = negative_dst ? tz.std_name : tz.dst_name;
        gint32 std_offset = negative_dst ? tz.dst_offset : tz.std_offset;
        gint32 dst_offset = negative_dst ? tz.std_offset : tz.dst_offset;
        const gchar *start = negative_dst ? tz.end_rule : tz.start_rule;
        const gchar *end = negative_dst ? tz.start_rule : tz.end_rule;
        /* An explicit 0 DST offset is only safe when it equals GLib's default. */
        if (dst_offset != 0 || std_offset == 3600) {
            gchar *offset = posix_offset_text(std_offset, variant);
            gchar *daylight = posix_offset_text(dst_offset, 0);
            if (offset && daylight) result = g_strdup_printf("%s%s%s%s,%s,%s", std_name, offset, dst_name, daylight, start, end);
            g_free(offset); g_free(daylight);
        }
    }
    posix_tz_clear(&tz);
    return result;
}

static TzData *tzif_parse(const guint8 *data, gsize length)
{
    TzifCounts v1, v2;
    if (!tzif_header(data, length, 0, &v1) || data[4] < '2') return NULL;
    guint64 second = 44 + tzif_block_size(&v1, 4);
    if (second > length || !tzif_header(data, length, (gsize)second, &v2)) return NULL;
    guint64 block = tzif_block_size(&v2, 8), start = second + 44;
    if (start + block > length) return NULL;
    const guint8 *p = data + start;
    TzData *d = g_new0(TzData, 1);
    d->timecnt = v2.timecnt; d->typecnt = v2.typecnt; d->charcnt = v2.charcnt;
    d->times = g_new(gint64, MAX(d->timecnt, 1));
    d->indices = g_new(guint8, MAX(d->timecnt, 1));
    d->offsets = g_new(gint32, d->typecnt);
    d->designations = g_new(guint8, d->typecnt);
    for (guint i = 0; i < d->timecnt; ++i) {
        d->times[i] = be64(p + (gsize)i * 8);
        if (i > 0 && d->times[i] <= d->times[i - 1]) goto invalid;
    }
    p += (gsize)d->timecnt * 8;
    for (guint i = 0; i < d->timecnt; ++i) {
        d->indices[i] = p[i];
        if (p[i] >= d->typecnt) goto invalid;
    }
    p += d->timecnt;
    for (guint i = 0; i < d->typecnt; ++i, p += 6) {
        gint32 offset = (gint32)be32(p);
        if (offset < -89999 || offset > 93599 || p[4] > 1 || p[5] >= d->charcnt) goto invalid;
        d->offsets[i] = offset;
        d->designations[i] = p[5];
    }
    d->chars = g_malloc0(d->charcnt + 1);
    memcpy(d->chars, p, d->charcnt);
    p += d->charcnt;
    p += (gsize)v2.leapcnt * 12 + v2.isstdcnt + v2.isutcnt;
    const guint8 *end = data + length;
    if (p < end && *p == '\n') {
        const guint8 *close = memchr(p + 1, '\n', (gsize)(end - p - 1));
        if (close && close > p + 1) {
            d->footer = g_strndup((const gchar *)p + 1, (gsize)(close - p - 1));
            if (!g_utf8_validate(d->footer, -1, NULL)) g_clear_pointer(&d->footer, g_free);
        }
    }
    if (d->footer) {
        d->glib_footer = footer_for_glib(d->footer, 0);
        if (d->glib_footer) d->footer_zone = g_time_zone_new_identifier(d->glib_footer);
        if (!d->footer_zone) g_debug("RadioS saat dilimi: TZif kuralı GLib ile yorumlanamadı: %s", d->footer);
    }
    return d;
invalid:
    tz_data_free(d);
    return NULL;
}

/* Reads <tree>\<id> without following links; NULL when absent or not TZif v2+.
 * The identifier must match the on-disk spelling exactly ("europe/istanbul" is
 * not an IANA id even though NTFS would open the file). */
static TzData *tzif_load(const gchar *id)
{
    gchar *native = g_strdelimit(g_strdup(id), "/", '\\');
    gchar *path = g_build_filename(radios_time_zone_directory(), native, NULL);
    TzData *d = NULL;
    gint fd = radios_fs_open(path, RADIOS_O_RDONLY | RADIOS_O_NOFOLLOW, 0);
    gchar *real = fd >= 0 ? radios_fs_realpath(path, NULL) : NULL;
    gchar *tail = g_strconcat("\\", native, NULL);
    gboolean exact = real && g_str_has_suffix(real, tail);
    g_free(tail); g_free(real); g_free(path); g_free(native);
    if (fd >= 0 && !exact) { close(fd); fd = -1; }
    if (fd < 0) return NULL;
    RadioSFileInfo info;
    if (radios_fd_stat(fd, &info) == 0 && info.type == RADIOS_FILE_TYPE_REGULAR && info.size >= 44 &&
        info.size <= 1024 * 1024) {
        gsize size = (gsize)info.size, used = 0;
        guint8 *buffer = g_malloc(size);
        while (used < size) {
            gssize n = read(fd, buffer + used, (guint)MIN(size - used, (gsize)G_MAXINT));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            used += (gsize)n;
        }
        if (used == size) d = tzif_parse(buffer, size);
        g_free(buffer);
    }
    close(fd);
    return d;
}

static gint tzdata_type_at(const TzData *d, gint64 t, gboolean *use_footer)
{
    *use_footer = FALSE;
    if (d->timecnt == 0) { *use_footer = d->footer_zone != NULL; return 0; }
    if (t < d->times[0]) return 0;
    if (t >= d->times[d->timecnt - 1]) {
        *use_footer = d->footer_zone != NULL;
        return d->indices[d->timecnt - 1];
    }
    guint low = 0, high = d->timecnt - 1; /* times[low] <= t < times[high] */
    while (high - low > 1) {
        guint mid = low + (high - low) / 2;
        if (d->times[mid] <= t) low = mid; else high = mid;
    }
    return d->indices[low];
}

/* Seconds east of UTC at instant t according to tzdata. */
static gint32 tzdata_offset_at(const TzData *d, gint64 t)
{
    gboolean footer;
    gint type = tzdata_type_at(d, t, &footer);
    if (footer) return g_time_zone_get_offset(d->footer_zone, g_time_zone_find_interval(d->footer_zone, G_TIME_TYPE_UNIVERSAL, t));
    return d->offsets[type];
}

static gchar *tzdata_abbreviation_at(const TzData *d, gint64 t, gint32 *offset)
{
    gboolean footer;
    gint type = tzdata_type_at(d, t, &footer);
    if (footer) {
        gint interval = g_time_zone_find_interval(d->footer_zone, G_TIME_TYPE_UNIVERSAL, t);
        *offset = g_time_zone_get_offset(d->footer_zone, interval);
        return g_strdup(g_time_zone_get_abbreviation(d->footer_zone, interval));
    }
    *offset = d->offsets[type];
    return g_strdup(d->chars + d->designations[type]);
}

static gint32 zone_offset_at(GTimeZone *tz, gint64 t)
{
    return g_time_zone_get_offset(tz, g_time_zone_find_interval(tz, G_TIME_TYPE_UNIVERSAL, t));
}

/* First instant in (low, high] whose offset differs from the offset at low. */
typedef gint32 (*OffsetFunc)(gconstpointer zone, gint64 t);
static gint32 offset_of_zone(gconstpointer zone, gint64 t) { return zone_offset_at((GTimeZone *)zone, t); }
static gint32 offset_of_data(gconstpointer zone, gint64 t) { return tzdata_offset_at(zone, t); }

static gint64 change_instant(OffsetFunc offset, gconstpointer zone, gint64 low, gint64 high)
{
    gint32 before = offset(zone, low);
    while (high - low > 1) {
        gint64 mid = low + (high - low) / 2;
        if (offset(zone, mid) == before) low = mid; else high = mid;
    }
    return high;
}

/* Number of disagreements between candidate and tzdata in the validation window
 * (one sample per day plus the exact instant of every offset change). */
static guint tz_disagreements(GTimeZone *candidate, const TzData *d)
{
    const gint64 day = 86400;
    gint64 now = g_get_real_time() / G_USEC_PER_SEC;
    gint64 start = now - 366 * day, end = now + 5 * 366 * day;
    guint mismatches = 0;
    gint32 previous_a = 0, previous_b = 0;
    for (gint64 t = start; t <= end; t += day) {
        gint32 a = zone_offset_at(candidate, t), b = tzdata_offset_at(d, t);
        if (a != b) ++mismatches;
        if (t > start && a == b && (a != previous_a || b != previous_b)) {
            gint64 ca = a != previous_a ? change_instant(offset_of_zone, candidate, t - day, t) : G_MININT64;
            gint64 cb = b != previous_b ? change_instant(offset_of_data, d, t - day, t) : G_MININT64;
            if (ca != cb) ++mismatches;
        }
        previous_a = a; previous_b = b;
    }
    return mismatches;
}

/* ------------------------------------------------------------------ cache + registry */

/* Known zones are cached for the process lifetime (bounded by the tzdata tree);
 * unknown identifiers are never interned and only kept in a small bounded set,
 * so arbitrary input cannot grow memory. */
static GMutex tz_lock;
static GHashTable *tz_zones;      /* id (string key) -> GTimeZone* (owned ref, never freed) */
static GHashTable *tz_ids;        /* GTimeZone* -> interned id */
static GHashTable *tz_data_cache; /* id -> TzData* */
static GHashTable *tz_misses;     /* ids without a TZif file (bounded) */
static GHashTable *tz_reverse;    /* interned Windows id -> interned IANA id or tz_missing */
static gchar tz_missing_marker;
#define tz_missing ((gpointer)&tz_missing_marker)
#define TZ_MISSES_MAX 256

static void tz_tables(void)
{
    if (tz_zones) return;
    tz_zones = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    tz_ids = g_hash_table_new(g_direct_hash, g_direct_equal);
    tz_data_cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    tz_misses = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    tz_reverse = g_hash_table_new(g_direct_hash, g_direct_equal);
}

/* Locked. */
static const TzData *tz_data_lookup(const gchar *id)
{
    TzData *d = g_hash_table_lookup(tz_data_cache, id);
    if (d) return d;
    if (g_hash_table_contains(tz_misses, id)) return NULL;
    d = tzif_load(id);
    if (d) g_hash_table_insert(tz_data_cache, g_strdup(id), d);
    else {
        if (g_hash_table_size(tz_misses) >= TZ_MISSES_MAX) g_hash_table_remove_all(tz_misses);
        g_hash_table_add(tz_misses, g_strdup(id));
    }
    return d;
}

static gboolean posix_like_windows_name(const gchar *name)
{
    /* GLib tries POSIX TZ strings before registry names: "UTC-02" would be read
     * as POSIX (sign inverted) instead of the registry zone "UTC-02". */
    const gchar *p = name;
    while (g_ascii_isalpha(*p)) ++p;
    return p - name >= 3 && (*p == '+' || *p == '-' || g_ascii_isdigit(*p));
}

/* Registry key names are case-insensitive while GLib caches zones by the exact
 * identifier string: case variants give one GTimeZone per IANA alias. */
static gchar *case_variant(gconstpointer base, guint variant)
{
    gchar *copy = g_strdup(base);
    for (gchar *p = copy; *p && variant; ++p) {
        if (!g_ascii_isalpha(*p)) continue;
        if (variant & 1u) *p = g_ascii_isupper(*p) ? g_ascii_tolower(*p) : g_ascii_toupper(*p);
        variant >>= 1;
    }
    if (variant) { g_free(copy); return NULL; }  /* more variants than letters */
    return copy;
}

static gchar *listed_spelling(gconstpointer base, guint variant)
{
    const GPtrArray *list = base;
    return variant < list->len ? g_strdup(list->pdata[variant]) : NULL;
}

/* Equivalent GLib spellings of a footer rule, distinct strings, minimal first. */
static GPtrArray *footer_spellings(const gchar *footer)
{
    GPtrArray *raw = g_ptr_array_new_with_free_func(g_free), *list = g_ptr_array_new_with_free_func(g_free);
    for (guint variant = 0; variant < 64; ++variant) {
        gchar *spelling = footer_for_glib(footer, variant);
        if (!spelling) break;
        g_ptr_array_add(raw, spelling);
    }
    PosixTz tz;
    if (raw->len && posix_tz_parse(footer, &tz)) {
        if (!tz.has_dst) constant_offset_spellings(-tz.std_offset, raw);
        posix_tz_clear(&tz);
    }
    for (guint i = 0; i < raw->len; ++i) {
        gboolean seen = FALSE;
        for (guint j = 0; j < list->len && !seen; ++j) seen = g_str_equal(list->pdata[j], raw->pdata[i]);
        if (!seen) g_ptr_array_add(list, g_strdup(raw->pdata[i]));
    }
    g_ptr_array_unref(raw);
    return list;
}

/* Locked.  The first spelling whose GTimeZone is not registered to another id;
 * when every spelling is taken, the zone of the first one (shared). */
typedef gchar *(*SpellingFunc)(gconstpointer base, guint variant);
static GTimeZone *tz_unregistered(gconstpointer base, SpellingFunc spelling_of)
{
    GTimeZone *shared = NULL;
    for (guint variant = 0; variant < 512; ++variant) {
        gchar *name = spelling_of(base, variant);
        if (!name) break;
        GTimeZone *tz = g_time_zone_new_identifier(name);
        g_free(name);
        if (!tz) { if (variant == 0) break; continue; }
        if (!g_hash_table_contains(tz_ids, tz)) {
            if (shared) g_time_zone_unref(shared);
            return tz;
        }
        if (!shared) shared = tz; else g_time_zone_unref(tz);
    }
    return shared;
}

/* Locked.  Builds the zone of a known identifier (d == NULL without a bundled tree). */
static GTimeZone *tz_create(const gchar *id, const TzData *d)
{
    GTimeZone *best = NULL;
    guint best_mismatches = G_MAXUINT;
    gchar *windows = icu_windows_zone(id);
    if (windows && !posix_like_windows_name(windows) && !g_str_equal(windows, "UTC")) {
        GTimeZone *tz = tz_unregistered(windows, case_variant);
        if (tz) {
            guint mismatches = d ? tz_disagreements(tz, d) : 0;
            if (mismatches == 0) { g_free(windows); return tz; }
            best = tz; best_mismatches = mismatches;
        }
    }
    g_free(windows);
    gchar *fixed = NULL;
    if (d && !d->footer_zone) {
        /* No usable rule string: the last local time type as a fixed rule. */
        gint type = d->timecnt ? d->indices[d->timecnt - 1] : 0;
        const gchar *abbreviation = d->chars + d->designations[type];
        gboolean alphabetic = strlen(abbreviation) >= 3, quotable = alphabetic;
        for (const gchar *p = abbreviation; *p; ++p) {
            alphabetic &= g_ascii_isalpha(*p) != 0;
            quotable &= g_ascii_isalnum(*p) || *p == '+' || *p == '-';
        }
        gchar *offset = posix_offset_text(-d->offsets[type], 0);
        fixed = alphabetic ? g_strconcat(abbreviation, offset, NULL)
              : quotable ? g_strdup_printf("<%s>%s", abbreviation, offset) : g_strdup_printf("<LMT>%s", offset);
        g_free(offset);
    }
    const gchar *rule = d ? (d->footer_zone ? d->footer : fixed) : NULL;
    if (rule) {
        GPtrArray *spellings = footer_spellings(rule);
        GTimeZone *tz = spellings->len ? tz_unregistered(spellings, listed_spelling) : NULL;
        g_ptr_array_unref(spellings);
        if (tz) {
            guint mismatches = tz_disagreements(tz, d);
            if (mismatches < best_mismatches) {
                if (best) g_time_zone_unref(best);
                best = tz; best_mismatches = mismatches;
            } else g_time_zone_unref(tz);
        }
    }
    g_free(fixed);
    if (best && best_mismatches > 0 && best_mismatches != G_MAXUINT)
        g_debug("RadioS saat dilimi %s: kurallar tzdata ile %u noktada ayrışıyor; en yakın kural kullanılıyor.",
                id, best_mismatches);
    return best;
}

/* Locked.  The GLib UTC singleton is always registered as "UTC". */
static GTimeZone *tz_utc(void)
{
    GTimeZone *utc = g_time_zone_new_utc();
    if (!g_hash_table_contains(tz_zones, "UTC")) {
        g_hash_table_insert(tz_zones, g_strdup("UTC"), g_time_zone_ref(utc));
        g_hash_table_insert(tz_ids, utc, (gpointer)g_intern_static_string("UTC"));
    }
    return utc;
}

GTimeZone *radios_time_zone_new(const gchar *identifier)
{
    if (!identifier) return g_time_zone_new_local();
    if (!tz_syntax_valid(identifier)) return NULL;
    radios_platform_init();
    g_mutex_lock(&tz_lock);
    tz_tables();
    if (g_str_equal(identifier, "UTC")) {
        GTimeZone *utc = tz_utc();
        g_mutex_unlock(&tz_lock);
        return utc;
    }
    GTimeZone *tz = g_hash_table_lookup(tz_zones, identifier);
    if (tz) { g_time_zone_ref(tz); g_mutex_unlock(&tz_lock); return tz; }
    const TzData *d = tz_data_lookup(identifier);
    gboolean known = d != NULL;
    if (!known && !tz_tree_available()) {
        gchar *canonical = icu_canonical_zone(identifier);
        known = canonical != NULL;
        g_free(canonical);
    }
    if (known) tz = tz_create(identifier, d);
    if (!tz && known && !d) {
        /* No bundled tree: UTC aliases still work (shared with "UTC"). */
        gchar *windows = icu_windows_zone(identifier);
        if (windows && g_str_equal(windows, "UTC")) tz = tz_utc();
        g_free(windows);
    }
    if (tz) {
        if (!g_hash_table_contains(tz_ids, tz)) g_hash_table_insert(tz_ids, tz, (gpointer)g_intern_string(identifier));
        g_hash_table_insert(tz_zones, g_strdup(identifier), tz);   /* the cache keeps this reference */
        g_time_zone_ref(tz);
    }
    g_mutex_unlock(&tz_lock);
    return tz;
}

gboolean radios_time_zone_identifier_exists(const gchar *identifier)
{
    if (!identifier) return FALSE;
    if (g_str_equal(identifier, "UTC")) return TRUE;
    if (!tz_syntax_valid(identifier)) return FALSE;
    radios_platform_init();
    g_mutex_lock(&tz_lock);
    tz_tables();
    gboolean exists = tz_data_lookup(identifier) != NULL;
    g_mutex_unlock(&tz_lock);
    if (exists || tz_tree_available()) return exists;
    gchar *canonical = icu_canonical_zone(identifier);
    exists = canonical != NULL;
    g_free(canonical);
    return exists;
}

const gchar *radios_time_zone_get_identifier(GTimeZone *tz)
{
    g_return_val_if_fail(tz != NULL, NULL);
    g_mutex_lock(&tz_lock);
    tz_tables();
    const gchar *id = g_hash_table_lookup(tz_ids, tz);
    g_mutex_unlock(&tz_lock);
    if (id) return id;
    const gchar *name = g_time_zone_get_identifier(tz);
    if (!name) return NULL;
    if (g_str_equal(name, "UTC")) return g_intern_static_string("UTC");
    const gchar *windows = g_intern_string(name);
    g_mutex_lock(&tz_lock);
    gpointer cached = g_hash_table_lookup(tz_reverse, windows);
    g_mutex_unlock(&tz_lock);
    if (!cached) {
        gchar *iana = icu_iana_zone(name);
        cached = iana ? (gpointer)g_intern_string(iana) : tz_missing;
        g_free(iana);
        g_mutex_lock(&tz_lock);
        g_hash_table_insert(tz_reverse, (gpointer)windows, cached);
        g_mutex_unlock(&tz_lock);
    }
    return cached == tz_missing ? windows : cached;
}

/* zone.tab / zone1970.tab third column (canonical tzdata names). */
static void tab_identifiers(const gchar *file, GHashTable *out)
{
    gchar *path = g_build_filename(radios_time_zone_directory(), file, NULL);
    gchar *contents = NULL;
    gsize length = 0;
    if (g_file_get_contents(path, &contents, &length, NULL) && length < 1024 * 1024) {
        gchar **lines = g_strsplit(contents, "\n", -1);
        for (guint i = 0; lines[i]; ++i) {
            if (lines[i][0] == '#' || !lines[i][0]) continue;
            gchar **columns = g_strsplit(lines[i], "\t", 4);
            if (g_strv_length(columns) >= 3) {
                g_strstrip(columns[2]);
                if (tz_syntax_valid(columns[2])) g_hash_table_add(out, (gpointer)g_intern_string(columns[2]));
            }
            g_strfreev(columns);
        }
        g_strfreev(lines);
    }
    g_free(contents); g_free(path);
}

static gint compare_strings(gconstpointer a, gconstpointer b)
{
    return strcmp(*(const gchar *const *)a, *(const gchar *const *)b);
}

gchar **radios_time_zone_list_identifiers(void)
{
    radios_platform_init();
    icu_init();
    GHashTable *set = g_hash_table_new(g_direct_hash, g_direct_equal);
    if (icu.enum_open && icu.enum_next && icu.enum_close) {
        gint status = 0;
        gpointer en = icu.enum_open(ICU_ZONE_TYPE_CANONICAL, NULL, NULL, &status);
        if (en && !ICU_FAILURE(status)) {
            const gchar *zone;
            gint32 length = 0;
            while ((zone = icu.enum_next(en, &length, &status)) != NULL && !ICU_FAILURE(status)) {
                gchar *copy = g_strndup(zone, (gsize)MAX(length, 0));
                if (radios_time_zone_identifier_exists(copy)) g_hash_table_add(set, (gpointer)g_intern_string(copy));
                g_free(copy);
            }
        }
        if (en) icu.enum_close(en);
    }
    GHashTable *tab = g_hash_table_new(g_direct_hash, g_direct_equal);
    tab_identifiers("zone1970.tab", tab);
    tab_identifiers("zone.tab", tab);
    GHashTableIter iter;
    gpointer key;
    g_hash_table_iter_init(&iter, tab);
    while (g_hash_table_iter_next(&iter, &key, NULL)) {
        const gchar *id = key;
        if (!radios_time_zone_identifier_exists(id)) continue;
        gchar *canonical = icu_canonical_zone(id);
        if (!canonical) g_hash_table_add(set, (gpointer)id);               /* newer than the ICU data */
        else if (!g_str_equal(canonical, id)) {
            const gchar *old = g_intern_string(canonical);
            if (g_hash_table_remove(set, old)) g_hash_table_add(set, (gpointer)id); /* tzdata's current name */
        }
        g_free(canonical);
    }
    g_hash_table_add(set, (gpointer)g_intern_static_string("UTC"));
    GPtrArray *list = g_ptr_array_new();
    g_hash_table_iter_init(&iter, set);
    while (g_hash_table_iter_next(&iter, &key, NULL)) g_ptr_array_add(list, g_strdup(key));
    g_ptr_array_sort(list, compare_strings);
    g_ptr_array_add(list, NULL);
    g_hash_table_unref(tab);
    g_hash_table_unref(set);
    return (gchar **)g_ptr_array_free(list, FALSE);
}

gchar *radios_date_time_format(GDateTime *datetime, const gchar *format)
{
    g_return_val_if_fail(datetime != NULL && format != NULL, NULL);
    if (!strstr(format, "%Z")) return g_date_time_format(datetime, format);
    const gchar *id = radios_time_zone_get_identifier(g_date_time_get_timezone(datetime));
    gchar *abbreviation = NULL;
    if (id && tz_syntax_valid(id)) {
        g_mutex_lock(&tz_lock);
        tz_tables();
        const TzData *d = tz_data_lookup(id);
        gint32 offset = 0;
        if (d) abbreviation = tzdata_abbreviation_at(d, g_date_time_to_unix(datetime), &offset);
        g_mutex_unlock(&tz_lock);
        if (abbreviation && (gint64)offset * G_TIME_SPAN_SECOND != g_date_time_get_utc_offset(datetime))
            g_clear_pointer(&abbreviation, g_free);
    }
    if (!abbreviation) return g_date_time_format(datetime, format);
    GString *rewritten = g_string_sized_new(strlen(format) + 16);
    for (const gchar *p = format; *p; ++p) {
        if (p[0] == '%' && p[1] == '%') { g_string_append(rewritten, "%%"); ++p; continue; }
        if (p[0] == '%' && p[1] == 'Z') {
            for (const gchar *a = abbreviation; *a; ++a) {
                if (*a == '%') g_string_append(rewritten, "%%"); else g_string_append_c(rewritten, *a);
            }
            ++p;
            continue;
        }
        g_string_append_c(rewritten, *p);
    }
    gchar *result = g_date_time_format(datetime, rewritten->str);
    g_string_free(rewritten, TRUE);
    g_free(abbreviation);
    return result;
}
