#include "radios/external_access_ui.h"
#include "radios/external_access.h"
#include "radios/widgets.h"
#include <gdk/gdkwin32.h>
#include <objbase.h>
#include <shellapi.h>
#include <string.h>

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *form, *facts[7], *open, *test, *cancel_button;
    GCancellable *cancel;
    guint64 generation;
    gboolean closed, busy;
} Access;
typedef struct { gchar *url; guint64 generation; } Request;
static void pack(GtkWidget *parent, GtkWidget *child)
{ gtk_box_pack_start(GTK_BOX(parent), child, FALSE, FALSE, 0); }
static GtkWidget *control(Access *access, const gchar *id)
{ return g_object_get_data(G_OBJECT(access->form), id); }
static const gchar *text(Access *access, const gchar *id)
{ GtkWidget *entry = control(access, id); return entry ? gtk_entry_get_text(GTK_ENTRY(entry)) : ""; }
static GtkWidget *status_label(const gchar *value)
{
    GtkWidget *label = radios_ui_label(value, "technical-status");
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0); return label;
}
static void request_free(gpointer value)
{ Request *request = value; g_free(request->url); g_free(request); }
static void access_free(gpointer value)
{ Access *access = value; g_clear_object(&access->cancel); g_free(access); }
static void set_busy(Access *access, gboolean busy)
{
    access->busy = busy;
    gtk_widget_set_sensitive(access->test, !busy);
    gtk_widget_set_sensitive(access->cancel_button, busy);
}
static void canceled(GtkButton *button, gpointer value)
{
    Access *access = value; (void)button;
    ++access->generation;
    if (access->cancel) g_cancellable_cancel(access->cancel);
    set_busy(access, FALSE);
    gtk_label_set_text(GTK_LABEL(access->facts[6]), "Denetim iptal edildi; dış sunucu durumu doğrulanmadı.");
}
static void destroyed(GtkWidget *widget, gpointer value)
{
    Access *access = value; (void)widget; access->closed = TRUE;
    ++access->generation; if (access->cancel) g_cancellable_cancel(access->cancel);
}
static void worker(GTask *task, gpointer source, gpointer data, GCancellable *cancel)
{
    Request *request = data; GError *error = NULL; (void)source;
    glong *status = g_new0(glong, 1);
    if (radios_external_probe_sync(request->url, NULL, cancel, status, &error))
        g_task_return_pointer(task, status, g_free);
    else { g_free(status); g_task_return_error(task, error); }
}
static void completed(GObject *object, GAsyncResult *result, gpointer unused)
{
    (void)unused;
    Access *access = g_object_get_data(object, "external-access-controller");
    Request *request = g_task_get_task_data(G_TASK(result)); GError *error = NULL;
    glong *status = g_task_propagate_pointer(G_TASK(result), &error);
    if (access->closed || request->generation != access->generation ||
        g_strcmp0(request->url, text(access, "technical-management-url"))) {
        g_free(status); g_clear_error(&error); return;
    }
    set_busy(access, FALSE);
    gchar *message;
    if (error) message = g_strdup(error->message);
    else {
        GDateTime *now = g_date_time_new_now_local();
        gchar *stamp = g_date_time_format(now, "%Y-%m-%d %H:%M:%S");
        const gchar *meaning = *status >= 300 && *status < 400 ? "Yönlendirme yanıtı; hedef izlenmedi" :
            *status == 401 || *status == 403 ? "Sunucu erişilebilir; kimlik veya izin gerekiyor" :
            *status == 405 ? "Sunucu erişilebilir; HEAD denetimini desteklemiyor" :
            *status >= 400 ? "Sunucu erişilebilir; HTTP hata yanıtı" : "Sunucu erişilebilir";
        message = g_strdup_printf("%s · HTTP %ld · TLS doğrulandı · %s. Oturum, site işlevleri ve dinleyici sesi sınanmadı.", meaning, *status, stamp);
        g_free(stamp); g_date_time_unref(now);
    }
    gtk_label_set_text(GTK_LABEL(access->facts[6]), message);
    g_free(message); g_free(status); g_clear_error(&error);
}
static void probe(GtkButton *button, gpointer value)
{
    Access *access = value; (void)button;
    if (access->closed || access->busy) return;
    const gchar *url = text(access, "technical-management-url");
    if (!radios_external_https_url_valid(url)) {
        gtk_label_set_text(GTK_LABEL(access->facts[6]), "Geçerli bir HTTPS yönetim adresi girin; kullanıcı bilgisi ve # bölümü kabul edilmez."); return;
    }
    g_clear_object(&access->cancel); access->cancel = g_cancellable_new();
    Request *request = g_new0(Request, 1); request->url = g_strdup(url);
    request->generation = ++access->generation;
    GTask *task = g_task_new(access->form, access->cancel, completed, NULL);
    g_task_set_task_data(task, request, request_free);
    set_busy(access, TRUE);
    gtk_label_set_text(GTK_LABEL(access->facts[6]), "HTTPS bağlantısı denetleniyor… (en fazla 10 saniye)");
    g_task_run_in_thread(task, worker); g_object_unref(task);
}
/* The shell's "open" verb is the reference way to start the user's default
 * browser on Windows (UserChoice/ProgID associations included, which GLib's
 * Win32 app-info lookup behind gtk_show_uri_on_window() can miss).  The URL
 * is an already validated HTTPS address, so the shell resolves it through the
 * https protocol handler only.  Error boxes are suppressed: the caller reports
 * the failure in the panel instead of a system dialog over the on-air UI. */
static gboolean shell_open_url(GtkWindow *parent, const gchar *url)
{
    gunichar2 *wide = g_utf8_to_utf16(url, -1, NULL, NULL, NULL);
    if (!wide) return FALSE;
    HWND owner = NULL;
    GdkWindow *window = parent ? gtk_widget_get_window(GTK_WIDGET(parent)) : NULL;
    if (window && GDK_IS_WIN32_WINDOW(window)) owner = gdk_win32_window_get_handle(window);
    /* Handlers behind the verb may be COM shell extensions (documented
     * ShellExecuteEx requirement); RPC_E_CHANGED_MODE keeps the thread's model. */
    HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    SHELLEXECUTEINFOW info;
    memset(&info, 0, sizeof info);
    info.cbSize = sizeof info;
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.hwnd = owner;
    info.lpVerb = L"open";
    info.lpFile = (LPCWSTR)wide;
    info.nShow = SW_SHOWNORMAL;
    BOOL opened = ShellExecuteExW(&info);
    if (SUCCEEDED(com)) CoUninitialize();
    g_free(wide);
    return opened != FALSE;
}
static void open_panel(GtkButton *button, gpointer value)
{
    Access *access = value; (void)button;
    const gchar *url = text(access, "technical-management-url");
    if (!radios_external_https_url_valid(url)) {
        gtk_label_set_text(GTK_LABEL(access->facts[6]), "Panel için geçerli bir HTTPS adresi girin."); return;
    }
    /* GTK's launcher first (tests wrap it), the Windows shell as fallback. */
    GError *error = NULL;
    if (!gtk_show_uri_on_window(access->ctx->parent, url, GDK_CURRENT_TIME, &error)) {
        g_clear_error(&error);
        if (!shell_open_url(access->ctx->parent, url))
            gtk_label_set_text(GTK_LABEL(access->facts[6]), "Dış tarayıcı açılamadı; Windows varsayılan tarayıcı ayarını kontrol edin.");
    }
}
void radios_external_access_ui_refresh(GtkWidget *form)
{
    Access *access = g_object_get_data(G_OBJECT(form), "external-access-controller");
    if (!access || access->closed) return;
    ++access->generation;
    if (access->cancel) g_cancellable_cancel(access->cancel);
    set_busy(access, FALSE);
    const gchar *ids[] = {"technical-management-url", "technical-listener-url", "technical-site-url"};
    for (guint i = 0; i < 3; ++i) {
        const gchar *url = text(access, ids[i]);
        gtk_label_set_text(GTK_LABEL(access->facts[i]), *url ? url : "Tanımlanmadı");
    }
    const gchar *modes[] = {"Parola", "Parola + ikinci doğrulama", "Kurum kimliği"};
    gint mode = gtk_combo_box_get_active(GTK_COMBO_BOX(control(access, "technical-web-auth")));
    gchar *session = g_strdup_printf("%s · %d dakika · dış sunucu uygulamalı",
        modes[CLAMP(mode, 0, 2)], gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(control(access, "technical-web-session"))));
    gtk_label_set_text(GTK_LABEL(access->facts[3]), session); g_free(session);
    const gchar *rights[] = {"Yalnız izleme", "Yayın kontrolü", "Seçili kapsam ve yetkiler"};
    gint right = gtk_combo_box_get_active(GTK_COMBO_BOX(control(access, "technical-web-permission")));
    gtk_label_set_text(GTK_LABEL(access->facts[4]), rights[CLAMP(right, 0, 2)]);
    const gchar *checks[] = {"technical-site-search", "technical-site-requests", "technical-site-adverts"};
    const gchar *names[] = {"Arama", "İstek / iletişim", "Reklam teslimi"}; GString *selected = g_string_new(NULL);
    for (guint i = 0; i < 3; ++i) if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(control(access, checks[i])))) {
        if (selected->len) g_string_append(selected, " · ");
        g_string_append(selected, names[i]);
    }
    gtk_label_set_text(GTK_LABEL(access->facts[5]), selected->len ? selected->str : "Seçilmedi"); g_string_free(selected, TRUE);
    gtk_label_set_text(GTK_LABEL(access->facts[6]), "Denetlenmedi. Özet formdaki girdileri gösterir; web profili ayrıca kaydedilir.");
}
void radios_external_access_ui_bind(RadioSViewContext *ctx, GtkWidget *form, GtkWidget *summary)
{
    Access *access = g_new0(Access, 1); access->ctx = ctx; access->form = form;
    g_object_set_data_full(G_OBJECT(form), "external-access-controller", access, access_free);
    g_signal_connect(form, "destroy", G_CALLBACK(destroyed), access);
    const gchar *names[] = {"Yönetim paneli", "Dinleyici akışı", "Site bağlantısı", "Kimlik / oturum", "Varsayılan yetki", "Site işlevleri", "Son erişim denetimi"};
    for (guint i = 0; i < 7; ++i) {
        pack(summary, radios_ui_label(names[i], "technical-fact-name"));
        access->facts[i] = status_label("—");
        gchar *id = g_strdup_printf("external-access-fact-%u", i); gtk_widget_set_name(access->facts[i], id); g_free(id);
        pack(summary, access->facts[i]);
    }
    GtkWidget *actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 7, "technical-actions");
    access->open = radios_ui_button("Web panelini aç ↗"); access->test = radios_ui_button("HTTPS erişimini test et");
    access->cancel_button = radios_ui_button("İptal");
    GtkWidget *buttons[] = {access->open, access->test, access->cancel_button};
    const gchar *ids[] = {"external-access-open", "external-access-test", "external-access-cancel"};
    GCallback callbacks[] = {G_CALLBACK(open_panel), G_CALLBACK(probe), G_CALLBACK(canceled)};
    for (guint i = 0; i < 3; ++i) { gtk_widget_set_name(buttons[i], ids[i]); g_signal_connect(buttons[i], "clicked", callbacks[i], access); pack(actions, buttons[i]); }
    pack(summary, actions);
    radios_external_access_ui_refresh(form);
}
