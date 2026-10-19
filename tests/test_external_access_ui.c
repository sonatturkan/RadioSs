#include "radios/data.h"
#include "radios/external_access.h"
#include <string.h>
static RadioSConfig config;
static gint held, entered, finished;
static guint browser_launches;
static gchar *last_uri;
typedef struct { RadioSViewContext ctx; GtkWidget *window, *page; } Harness;
/* Only this acceptance executable wraps network/browser calls. Actual TLS
 * behavior is independently exercised by run_external_access.py. */
gboolean __wrap_radios_external_probe_sync(const gchar *url, const gchar *ca,
    GCancellable *cancel, glong *status, GError **error)
{
    (void)ca; g_assert_true(radios_external_https_url_valid(url)); g_atomic_int_set(&entered, 1);
    gint64 limit = g_get_monotonic_time() + 5 * G_TIME_SPAN_SECOND;
    while (g_atomic_int_get(&held) && !g_cancellable_is_cancelled(cancel) && g_get_monotonic_time() < limit) g_usleep(1000);
    g_atomic_int_set(&finished, 1);
    if (g_cancellable_set_error_if_cancelled(cancel, error)) return FALSE;
    *status = 204; return TRUE;
}
gboolean __wrap_gtk_show_uri_on_window(GtkWindow *window, const gchar *uri, guint32 stamp, GError **error)
{
    (void)window; (void)stamp; (void)error;
    ++browser_launches; g_free(last_uri); last_uri = g_strdup(uri); return TRUE;
}
static GtkWidget *find(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = find(item->data, name);
    g_list_free(children); return found;
}
static void pump(void) { while (g_main_context_iteration(NULL, FALSE)) {} g_usleep(1000); }
typedef gboolean (*Condition)(gpointer);
static void wait_for(Condition condition, gpointer data)
{
    gint64 end = g_get_monotonic_time() + 15 * G_TIME_SPAN_SECOND;
    while (!condition(data) && g_get_monotonic_time() < end) pump();
    g_assert_true(condition(data));
}
static gboolean ready(gpointer data)
{
    Harness *h = data; if (!radios_data_connected(&h->ctx)) return FALSE;
    GtkWidget *entry = find(h->page, "technical-management-url");
    for (GtkWidget *parent = entry; parent; parent = gtk_widget_get_parent(parent)) {
        RadioSDocument *doc = g_object_get_data(G_OBJECT(parent), "technical-web-document");
        if (doc) return radios_data_document_ready(doc);
    }
    return FALSE;
}
static Harness *open_harness(void)
{
    Harness *h = g_new0(Harness, 1); h->ctx.config = &config; h->ctx.runtime = radios_runtime_new(64, 128);
    h->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); h->ctx.parent = GTK_WINDOW(h->window);
    h->ctx.data = radios_data_new(&h->ctx); h->page = radios_technical_new(&h->ctx);
    gtk_container_add(GTK_CONTAINER(h->window), h->page); gtk_widget_show_all(h->window);
    radios_data_connect(&h->ctx); wait_for(ready, h); return h;
}
static void close_harness(Harness *h)
{
    gtk_widget_destroy(h->window); radios_data_close(h->ctx.data); radios_runtime_free(h->ctx.runtime); radios_data_unref(h->ctx.data);
    for (guint i = 0; i < 25; ++i) pump();
    g_free(h);
}
static void set(Harness *h, const gchar *id, const gchar *value)
{ GtkWidget *entry = find(h->page, id); g_assert_true(GTK_IS_ENTRY(entry)); gtk_entry_set_text(GTK_ENTRY(entry), value); }
static void click(Harness *h, const gchar *id)
{ GtkWidget *button = find(h->page, id); g_assert_true(GTK_IS_BUTTON(button)); g_assert_true(gtk_widget_is_sensitive(button)); gtk_button_clicked(GTK_BUTTON(button)); }
static const gchar *fact(Harness *h, guint index)
{ gchar *id = g_strdup_printf("external-access-fact-%u", index); GtkWidget *label = find(h->page, id); g_free(id); g_assert_true(GTK_IS_LABEL(label)); return gtk_label_get_text(GTK_LABEL(label)); }
static void persistence(void)
{
    Harness *h = open_harness();
    set(h, "technical-management-url", "https://radio.example/panel#invalid"); click(h, "external-access-open"); g_assert_cmpuint(browser_launches, ==, 0);
    set(h, "technical-management-url", "https://radio.example/panel");
    set(h, "technical-listener-url", "https://radio.example/live");
    set(h, "technical-site-url", "https://radio.example");
    g_assert_cmpstr(fact(h, 0), ==, "https://radio.example/panel");
    click(h, "external-access-open"); g_assert_cmpuint(browser_launches, ==, 1); g_assert_cmpstr(last_uri, ==, "https://radio.example/panel");
    click(h, "technical-web-save"); wait_for(ready, h); close_harness(h);
    h = open_harness(); g_assert_cmpstr(gtk_entry_get_text(GTK_ENTRY(find(h->page, "technical-management-url"))), ==, "https://radio.example/panel");
    g_assert_cmpstr(fact(h, 1), ==, "https://radio.example/live"); g_assert_null(strstr(fact(h, 6), "TLS doğrulandı")); close_harness(h);
}
static gboolean started(gpointer unused) { (void)unused; return g_atomic_int_get(&entered); }
static gboolean ended(gpointer unused) { (void)unused; return g_atomic_int_get(&finished); }
static gboolean result_visible(gpointer data) { return strstr(fact(data, 6), "HTTP 204") != NULL; }
static void reset_worker(void) { g_atomic_int_set(&held, 1); g_atomic_int_set(&entered, 0); g_atomic_int_set(&finished, 0); }
static void stale_and_cancel(void)
{
    Harness *h = open_harness(); reset_worker(); click(h, "external-access-test"); wait_for(started, NULL);
    set(h, "technical-management-url", "https://updated.example/panel"); wait_for(ended, NULL);
    for (guint i = 0; i < 25; ++i) pump();
    g_assert_null(strstr(fact(h, 6), "HTTP 204"));
    reset_worker(); click(h, "external-access-test"); wait_for(started, NULL); click(h, "external-access-cancel"); wait_for(ended, NULL);
    for (guint i = 0; i < 25; ++i) pump();
    g_assert_nonnull(strstr(fact(h, 6), "iptal edildi"));
    reset_worker(); click(h, "external-access-test"); wait_for(started, NULL); g_atomic_int_set(&held, 0); wait_for(result_visible, h);
    close_harness(h);
}
static void close_pending(void)
{ Harness *h = open_harness(); reset_worker(); click(h, "external-access-test"); wait_for(started, NULL); close_harness(h); wait_for(ended, NULL); for (guint i = 0; i < 25; ++i) pump(); }
int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL); g_assert_cmpint(argc, ==, 2); GError *error = NULL;
    g_assert_true(radios_config_load(argv[1], NULL, &config, &error)); g_assert_no_error(error); g_assert_true(gtk_init_check(NULL, NULL));
    g_test_add_func("/external-access-ui/persist-and-open", persistence); g_test_add_func("/external-access-ui/cancel-and-stale", stale_and_cancel);
    g_test_add_func("/external-access-ui/close-pending", close_pending);
    int result = g_test_run(); radios_config_clear(&config); g_free(last_uri); return result;
}
