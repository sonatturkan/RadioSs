#include "radios/external_access.h"
#include <string.h>
static const gchar *base, *ca;
static void urls(void)
{
    const gchar *invalid[] = {"", "http://localhost/", "file:///etc/passwd", "https://u:p@host/",
        "https://host/#token", "https://host/a\nb", "https://host/a\\b", "https://host:0/", "https:///", NULL};
    for (guint i = 0; invalid[i]; ++i) g_assert_false(radios_external_https_url_valid(invalid[i]));
    g_assert_true(radios_external_https_url_valid("https://example.com:8443/api/panel?q=radio"));
    GError *error = NULL; glong status = 99;
    g_assert_false(radios_external_probe_sync("http://localhost/", ca, NULL, &status, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT); g_assert_cmpint(status, ==, 0); g_clear_error(&error);
}
static void probe_path(const gchar *path, glong expected)
{
    gchar *url = g_strconcat(base, path, NULL); GError *error = NULL; glong status = 0;
    g_assert_true(radios_external_probe_sync(url, ca, NULL, &status, &error));
    g_assert_no_error(error); g_assert_cmpint(status, ==, expected); g_free(url);
}
static void responses(void)
{ probe_path("/success", 204); probe_path("/auth", 401); probe_path("/redirect", 302); probe_path("/method", 405); }
static void untrusted(void)
{
    gchar *url = g_strconcat(base, "/success", NULL); GError *error = NULL; glong status = 0;
    g_assert_false(radios_external_probe_sync(url, NULL, NULL, &status, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED); g_assert_cmpint(status, ==, 0); g_clear_error(&error); g_free(url);
}
static void bounded(void)
{
    gchar *url = g_strconcat(base, "/large", NULL); GError *error = NULL; glong status = 0;
    g_assert_false(radios_external_probe_sync(url, ca, NULL, &status, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED); g_assert_cmpint(status, ==, 0); g_clear_error(&error); g_free(url);
}
static gpointer cancel_later(gpointer value)
{ g_usleep(150000); g_cancellable_cancel(value); return NULL; }
static void cancellation(void)
{
    GCancellable *cancel = g_cancellable_new(); GError *error = NULL; glong status = 0;
    g_cancellable_cancel(cancel);
    g_assert_false(radios_external_probe_sync(base, ca, cancel, &status, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED); g_clear_error(&error);
    g_cancellable_reset(cancel);
    gchar *url = g_strconcat(base, "/slow", NULL); GThread *thread = g_thread_new("cancel-probe", cancel_later, cancel);
    gint64 began = g_get_monotonic_time();
    g_assert_false(radios_external_probe_sync(url, ca, cancel, &status, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED); g_assert_cmpint(g_get_monotonic_time() - began, <, 3 * G_TIME_SPAN_SECOND);
    g_thread_join(thread); g_clear_error(&error); g_object_unref(cancel); g_free(url);
}
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); base = g_getenv("RADIOS_TEST_ACCESS_URL"); ca = g_getenv("RADIOS_TEST_ACCESS_CA");
    g_assert_nonnull(base); g_assert_nonnull(ca);
    g_test_add_func("/external-access/urls", urls); g_test_add_func("/external-access/tls-responses-no-redirect", responses);
    g_test_add_func("/external-access/untrusted-tls", untrusted); g_test_add_func("/external-access/bounded-headers", bounded);
    g_test_add_func("/external-access/cancel", cancellation); return g_test_run();
}
