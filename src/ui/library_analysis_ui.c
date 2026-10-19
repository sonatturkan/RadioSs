#include "radios/library_analysis_ui.h"
#include "radios/library_analysis.h"
#include "radios/media_ui.h"
#include "radios/data.h"

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *page, *workers, *auto_scan, *settle, *interval, *settings_status;
    GtkWidget *run, *stop, *progress, *status;
    guint timer;
    gboolean updating, disposed;
} AnalysisPage;

static void pack(GtkWidget *box, GtkWidget *child)
{ gtk_box_pack_start(GTK_BOX(box), child, FALSE, FALSE, 0); }
static GtkWidget *styled(GtkWidget *widget, const gchar *style)
{ gtk_style_context_add_class(gtk_widget_get_style_context(widget), style); return widget; }
static GtkWidget *named(GtkWidget *widget, const gchar *name)
{ gtk_widget_set_name(widget, name); return widget; }
static GtkWidget *text(const gchar *value, const gchar *style)
{
    GtkWidget *label = gtk_label_new(value); gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_label_set_line_wrap(GTK_LABEL(label), TRUE); gtk_label_set_line_wrap_mode(GTK_LABEL(label), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(label), 90);
    return style ? styled(label, style) : label;
}
static GtkWidget *section_new(const gchar *title)
{
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); styled(section, "preferences-section");
    pack(section, text(title, "preferences-heading"));
    return section;
}
static const gchar *project_root(AnalysisPage *page)
{ return page->ctx->config ? page->ctx->config->project_root : NULL; }

static void settings_render(AnalysisPage *page, const RadioSLibraryAnalysisSettings *settings)
{
    page->updating = TRUE;
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(page->workers), settings->workers);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(page->auto_scan), settings->auto_scan);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(page->settle), settings->settle_seconds);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(page->interval), settings->interval_minutes);
    gtk_widget_set_sensitive(page->settle, settings->auto_scan);
    gtk_widget_set_sensitive(page->interval, settings->auto_scan);
    page->updating = FALSE;
}
static void settings_read(AnalysisPage *page, RadioSLibraryAnalysisSettings *settings)
{
    settings->workers = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(page->workers));
    settings->auto_scan = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(page->auto_scan));
    settings->settle_seconds = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(page->settle));
    settings->interval_minutes = (guint)gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(page->interval));
}
/* Every change is stored at once; a scan reads the file when it starts, so a
 * pass that already runs keeps the worker count it began with. */
static void option_changed(GtkWidget *widget, gpointer value)
{
    (void)widget; AnalysisPage *page = value;
    if (page->updating || page->disposed) return;
    RadioSLibraryAnalysisSettings settings; settings_read(page, &settings);
    gtk_widget_set_sensitive(page->settle, settings.auto_scan);
    gtk_widget_set_sensitive(page->interval, settings.auto_scan);
    GError *error = NULL;
    if (!project_root(page)) {
        gtk_label_set_text(GTK_LABEL(page->settings_status), "Proje klasörü bilinmiyor; ayar kaydedilemedi.");
    } else if (radios_library_analysis_settings_save(project_root(page), &settings, &error)) {
        gtk_label_set_text(GTK_LABEL(page->settings_status), "Kaydedildi.");
    } else {
        gchar *message = g_strdup_printf("Ayar kaydedilemedi: %s", error ? error->message : "bilinmeyen hata");
        gtk_label_set_text(GTK_LABEL(page->settings_status), message); g_free(message);
    }
    g_clear_error(&error);
}
static void run_clicked(GtkButton *button, gpointer value)
{
    (void)button; AnalysisPage *page = value;
    if (page->disposed) return;
    if (radios_media_rhythm_library_run_now(page->ctx)) gtk_label_set_text(GTK_LABEL(page->status), "Analiz başlatıldı…");
    else gtk_label_set_text(GTK_LABEL(page->status), "Analiz başlatılamadı: veri bağlantısı yok ya da tarama sürüyor.");
}
static void stop_clicked(GtkButton *button, gpointer value)
{
    (void)button; AnalysisPage *page = value;
    if (page->disposed) return;
    radios_media_rhythm_library_cancel(page->ctx);
    gtk_label_set_text(GTK_LABEL(page->status), "Tarama durduruluyor; tamamlanan parçalar korunur.");
}
static gboolean page_tick(gpointer value)
{
    AnalysisPage *page = value;
    if (page->disposed) return G_SOURCE_REMOVE;
    gboolean running = FALSE, connected = radios_data_connected(page->ctx);
    gchar *last = radios_media_rhythm_library_status(page->ctx, &running);
    guint done = 0, total = 0, workers = 0;
    gboolean active = radios_library_analysis_activity(&done, &total, &workers);
    if (active) {
        gchar *label = total ? g_strdup_printf("%u / %u parça · %u paralel", MIN(done, total), total, workers) : g_strdup("Parçalar taranıyor…");
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(page->progress), total ? MIN(1.0, (gdouble)done / total) : 0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(page->progress), label); g_free(label);
        gtk_label_set_text(GTK_LABEL(page->status), "Analiz sürüyor.");
    } else {
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(page->progress), 0);
        gtk_progress_bar_set_text(GTK_PROGRESS_BAR(page->progress), "Hazır");
        if (*last) gtk_label_set_text(GTK_LABEL(page->status), last);
        else if (!connected) gtk_label_set_text(GTK_LABEL(page->status), "Veritabanı bağlantısı bekleniyor…");
    }
    gtk_widget_set_sensitive(page->run, connected && !active);
    gtk_widget_set_visible(page->stop, running);
    g_free(last);
    return G_SOURCE_CONTINUE;
}
static void page_destroyed(GtkWidget *widget, gpointer value)
{
    (void)widget; AnalysisPage *page = value; page->disposed = TRUE;
    if (page->timer) { g_source_remove(page->timer); page->timer = 0; }
}
GtkWidget *radios_library_analysis_ui_page_new(RadioSViewContext *ctx)
{
    g_return_val_if_fail(ctx, NULL);
    AnalysisPage *page = g_new0(AnalysisPage, 1); page->ctx = ctx; page->updating = TRUE;
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16); gtk_container_set_border_width(GTK_CONTAINER(body), 30);
    named(body, "preferences-analysis-page"); page->page = body;
    g_object_set_data_full(G_OBJECT(body), "radios-library-analysis-page", page, g_free);
    g_signal_connect(body, "destroy", G_CALLBACK(page_destroyed), page);
    pack(body, text("Kütüphane analizi", "preferences-heading"));

    GtkWidget *parallel = section_new("Paralel analiz");
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    pack(row, text("Aynı anda analiz edilecek şarkı sayısı:", NULL));
    page->workers = named(gtk_spin_button_new_with_range(1, RADIOS_LIBRARY_ANALYSIS_MAX_WORKERS, 1), "preferences-analysis-workers");
    atk_object_set_name(gtk_widget_get_accessible(page->workers), "Aynı anda analiz edilecek şarkı sayısı");
    pack(row, page->workers); pack(parallel, row);
    gchar *hint = g_strdup_printf("Bu bilgisayarda %u işlemci çekirdeği var; önerilen sayı %u. Analiz ses motorundan düşük öncelikle çalışır.",
        g_get_num_processors(), radios_library_analysis_default_workers());
    gtk_widget_set_tooltip_text(page->workers, hint); g_free(hint);
    pack(body, parallel);

    GtkWidget *schedule = section_new("Otomatik tarama");
    page->auto_scan = named(gtk_check_button_new_with_label("Kütüphaneyi kendiliğinden tara (RadioS açıkken)"), "preferences-analysis-auto");
    pack(schedule, page->auto_scan);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    pack(row, text("İçe aktarma durduktan", NULL));
    page->settle = named(gtk_spin_button_new_with_range(3, 600, 1), "preferences-analysis-settle");
    atk_object_set_name(gtk_widget_get_accessible(page->settle), "İçe aktarma sonrası bekleme süresi (saniye)");
    pack(row, page->settle); pack(row, text("saniye sonra başla", NULL)); pack(schedule, row);
    row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    pack(row, text("Değişiklik olmasa da her", NULL));
    page->interval = named(gtk_spin_button_new_with_range(1, 1440, 1), "preferences-analysis-interval");
    atk_object_set_name(gtk_widget_get_accessible(page->interval), "Tarama aralığı (dakika)");
    pack(row, page->interval); pack(row, text("dakikada bir tara", NULL)); pack(schedule, row);
    gtk_widget_set_tooltip_text(page->auto_scan, "Kapalıyken tarama yalnız \"Tüm kütüphaneyi şimdi analiz et\" düğmesiyle başlar.");
    gtk_widget_set_tooltip_text(page->settle, "Her içe aktarma grubu bu süreyi yeniden başlatır: şarkılar önce alınır, analiz sonra toplu çalışır.");
    page->settings_status = named(text("", "preferences-subtitle"), "preferences-analysis-settings-status");
    pack(schedule, page->settings_status);
    pack(body, schedule);

    GtkWidget *now = section_new("Şimdi çalıştır");
    GtkWidget *actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    page->run = named(gtk_button_new_with_label("Tüm kütüphaneyi şimdi analiz et"), "preferences-analysis-run");
    page->stop = named(gtk_button_new_with_label("Durdur"), "preferences-analysis-stop");
    gtk_widget_set_no_show_all(page->stop, TRUE);
    pack(actions, page->run); pack(actions, page->stop); pack(now, actions);
    page->progress = named(gtk_progress_bar_new(), "preferences-analysis-progress");
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(page->progress), TRUE); gtk_progress_bar_set_text(GTK_PROGRESS_BAR(page->progress), "Hazır");
    pack(now, page->progress);
    page->status = named(text("", "preferences-subtitle"), "preferences-analysis-status"); pack(now, page->status);
    pack(body, now);

    RadioSLibraryAnalysisSettings settings; GError *error = NULL;
    if (project_root(page) && radios_library_analysis_settings_load(project_root(page), &settings, &error)) {
        settings_render(page, &settings);
    } else {
        radios_library_analysis_settings_init(&settings); settings_render(page, &settings);
        if (error) gtk_label_set_text(GTK_LABEL(page->settings_status), error->message);
    }
    g_clear_error(&error);
    page->updating = FALSE;
    g_signal_connect(page->workers, "value-changed", G_CALLBACK(option_changed), page);
    g_signal_connect(page->auto_scan, "toggled", G_CALLBACK(option_changed), page);
    g_signal_connect(page->settle, "value-changed", G_CALLBACK(option_changed), page);
    g_signal_connect(page->interval, "value-changed", G_CALLBACK(option_changed), page);
    g_signal_connect(page->run, "clicked", G_CALLBACK(run_clicked), page);
    g_signal_connect(page->stop, "clicked", G_CALLBACK(stop_clicked), page);
    page_tick(page);
    page->timer = g_timeout_add(500, page_tick, page);
    GtkWidget *scroll = named(gtk_scrolled_window_new(NULL, NULL), "preferences-analysis-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), body);
    return scroll;
}
