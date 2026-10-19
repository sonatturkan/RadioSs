#include "radios/external_access.h"
#include <curl/curl.h>
#include <string.h>

gboolean radios_external_https_url_valid(const gchar *url)
{
    if (!url || !*url || strlen(url) > 2048) return FALSE;
    for (const guchar *p = (const guchar *)url; *p; ++p)
        if (*p <= 32 || *p == 127 || *p == '\\') return FALSE;
    GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
    if (!uri) return FALSE;
    const gchar *scheme = g_uri_get_scheme(uri), *host = g_uri_get_host(uri);
    gint port = g_uri_get_port(uri);
    gboolean valid = scheme && g_ascii_strcasecmp(scheme, "https") == 0 &&
        host && *host && !g_uri_get_userinfo(uri) && !g_uri_get_fragment(uri) &&
        (port == -1 || (port > 0 && port <= 65535));
    g_uri_unref(uri); return valid;
}

typedef struct { GCancellable *cancel; gsize headers; gboolean too_large; } Probe;
static size_t header_received(char *bytes, size_t size, size_t count, void *data)
{
    Probe *probe = data; (void)bytes;
    if (size && count > G_MAXSIZE / size) { probe->too_large = TRUE; return 0; }
    gsize length = size * count;
    if (length > 65536 - probe->headers) { probe->too_large = TRUE; return 0; }
    probe->headers += length; return length;
}
static int progress(void *data, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d)
{
    Probe *probe = data; (void)a; (void)b; (void)c; (void)d;
    return probe->cancel && g_cancellable_is_cancelled(probe->cancel);
}

gboolean radios_external_probe_sync(const gchar *url, const gchar *ca_file,
    GCancellable *cancel, glong *http_status, GError **error)
{
    if (http_status) *http_status = 0;
    if (!radios_external_https_url_valid(url)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
            "Kullanıcı bilgisi ve # bölümü içermeyen geçerli bir HTTPS adresi gerekli."); return FALSE;
    }
    if (cancel && g_cancellable_set_error_if_cancelled(cancel, error)) return FALSE;
    static gsize initialized;
    if (g_once_init_enter(&initialized)) {
        CURLcode result = curl_global_init(CURL_GLOBAL_DEFAULT);
        g_once_init_leave(&initialized, result == CURLE_OK ? 1 : 2);
    }
    CURL *curl = initialized == 1 ? curl_easy_init() : NULL;
    if (!curl) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "HTTPS denetimi başlatılamadı."); return FALSE;
    }
    Probe probe = { .cancel = cancel };
    CURLcode setup = CURLE_OK;
#define SET(option, value) do { if (setup == CURLE_OK) setup = curl_easy_setopt(curl, option, value); } while (0)
    SET(CURLOPT_URL, url); SET(CURLOPT_NOBODY, 1L);
    SET(CURLOPT_PROTOCOLS_STR, "https"); SET(CURLOPT_REDIR_PROTOCOLS_STR, "https");
    SET(CURLOPT_FOLLOWLOCATION, 0L); SET(CURLOPT_PROXY, "");
    SET(CURLOPT_NOSIGNAL, 1L); SET(CURLOPT_CONNECTTIMEOUT_MS, 5000L); SET(CURLOPT_TIMEOUT_MS, 10000L);
    SET(CURLOPT_SSL_VERIFYPEER, 1L); SET(CURLOPT_SSL_VERIFYHOST, 2L);
    SET(CURLOPT_HEADERFUNCTION, header_received); SET(CURLOPT_HEADERDATA, &probe);
    SET(CURLOPT_NOPROGRESS, 0L); SET(CURLOPT_XFERINFOFUNCTION, progress); SET(CURLOPT_XFERINFODATA, &probe);
    SET(CURLOPT_USERAGENT, "RadioS HTTPS reachability");
    if (ca_file) SET(CURLOPT_CAINFO, ca_file);
#undef SET
    CURLcode result = setup == CURLE_OK ? curl_easy_perform(curl) : setup;
    long status = 0;
    if (result == CURLE_OK) result = curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    if (cancel && g_cancellable_set_error_if_cancelled(cancel, error)) return FALSE;
    if (result != CURLE_OK || status < 100 || status > 599) {
        const gchar *message = probe.too_large ? "Sunucu başlıkları 64 KiB sınırını aştı." :
            result == CURLE_OPERATION_TIMEDOUT ? "HTTPS denetimi zaman aşımına uğradı." :
            result == CURLE_PEER_FAILED_VERIFICATION ? "Sunucunun TLS sertifikası doğrulanamadı." :
            "HTTPS bağlantısı kurulamadı; adres, ağ ve sertifikayı kontrol edin.";
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, message); return FALSE;
    }
    if (http_status) *http_status = status;
    return TRUE;
}
