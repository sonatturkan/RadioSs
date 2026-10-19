#include "radios/views.h"
#include "radios/header_progress.h"
#include "radios/header_status.h"
#include "radios/widgets.h"
#include "radios/audio_ui.h"
#include "radios/header_navigation.h"
#include "radios/data.h"
#include "radios/station_time.h"
#include "radios/broadcast_header.h"
#include "radios/broadcast_ui.h"
#include "radios/player_workspace.h"
#include "radios/player_binding.h"
#include "radios/routing_ui.h"
#include <math.h>
#include <string.h>

/* The shared strip follows index.html:359–727 and header-layout.css.
 * Status fields stay unknown until a real backend supplies an observation. */
typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *clock_label, *analog;
    GtkWidget *grid;
    GtkWidget *sections[6];
    GtkWidget *time;
    GtkWidget *calendar;
    GtkWidget *onair_title, *source, *destination, *next_title, *countdown, *advert;
    GtkWidget *indicators[3];
    gchar next_entry[RADIOS_AUDIO_NORMAL_DECKS][37], next_cached[RADIOS_AUDIO_NORMAL_DECKS][801];
    guint64 next_generation[RADIOS_AUDIO_NORMAL_DECKS];
    guint timer, live_timer, layout_idle;
    gint layout, pending_layout, audio_columns, pending_audio_columns;
    gboolean compact, pending_compact;
    gboolean disposed;
} HeaderState;

enum { BRAND, ONAIR, BEHAVIOR, NEXT, TIME, AUDIO };

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *source, *destination, *toggle;
    gboolean headphone, resetting;
} MonitorState;

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *dialog, *note, *status, *chooser;
} SupportState;

static void support_open(GtkButton *button, gpointer data);

static void
pack(GtkWidget *box, GtkWidget *child, gboolean expand)
{
    gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0);
}

static gboolean
indicator_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    (void)data;
    guint status = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(widget), "live-state"));
    if (status == 1) cairo_set_source_rgb(cr, 0.20, 0.78, 0.42);
    else if (status == 2) cairo_set_source_rgb(cr, 0.91, 0.65, 0.19);
    else if (status == 3) cairo_set_source_rgb(cr, 0.88, 0.28, 0.25);
    else cairo_set_source_rgb(cr, 0.15, 0.20, 0.24);
    cairo_arc(cr, 3, 3, 2.5, 0, 6.283185307179586);
    cairo_fill_preserve(cr);
    cairo_set_source_rgb(cr, 0.41, 0.47, 0.53);
    cairo_set_line_width(cr, 0.7);
    cairo_stroke(cr);
    return FALSE;
}

static GtkWidget *
indicator(const gchar *name, const gchar *id)
{
    GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 5, "header-live-indicator");
    GtkWidget *led = gtk_drawing_area_new();
    gtk_widget_set_size_request(led, 6, 6);
    gtk_widget_set_valign(led, GTK_ALIGN_CENTER);
    g_signal_connect(led, "draw", G_CALLBACK(indicator_draw), NULL);
    pack(row, led, FALSE);
    pack(row, radios_ui_label(name, "header-live-name"), FALSE);
    GtkWidget *value = radios_ui_label("—", "header-live-state");
    gtk_widget_set_name(value, id);
    g_object_set_data(G_OBJECT(row), "live-value", value);
    g_object_set_data(G_OBJECT(row), "live-led", led);
    pack(row, value, FALSE);
    return row;
}

static GtkWidget *
brand_new(void)
{
    GtkWidget *brand = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 1, "header-brand-area");
    GtkWidget *copy = radios_ui_box(GTK_ORIENTATION_VERTICAL, 1, "brand-copy");
    GtkWidget *letter = radios_ui_label("S", "brand-letter");
    GtkWidget *line = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_style_context_add_class(gtk_widget_get_style_context(line), "brand-rule");
    pack(copy, radios_ui_label("Radio", "brand-name"), FALSE);
    pack(copy, line, FALSE);
    pack(copy, radios_ui_label("Automation software", "brand-subtitle"), FALSE);
    pack(brand, copy, FALSE);
    pack(brand, letter, FALSE);
    gtk_widget_set_valign(letter, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(copy, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(brand, GTK_ALIGN_FILL);
    gtk_widget_set_size_request(brand, 148, -1);
    return brand;
}

static GtkWidget *
onair_new(HeaderState *state)
{
    GtkWidget *section = radios_ui_box(GTK_ORIENTATION_VERTICAL, 3, "header-onair");
    GtkWidget *target = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 14, "broadcast-target");
    GtkWidget *chain = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 14, "header-live-indicators");
    g_object_set_data(G_OBJECT(section), "header-onair-target", target);
    g_object_set_data(G_OBJECT(section), "header-onair-chain", chain);
    pack(section, radios_ui_label("YAYINDA", "broadcast-label"), FALSE);
    state->onair_title = radios_ui_label("—", "broadcast-content");
    state->source = radios_ui_label("Aktif kaynak  —", "broadcast-target-value");
    state->destination = radios_ui_label("Hedef çıkış  —", "broadcast-target-value");
    gtk_widget_set_name(state->onair_title, "header-onair-title");
    gtk_widget_set_name(state->source, "header-active-source");
    gtk_widget_set_name(state->destination, "header-target-output");
    gtk_label_set_max_width_chars(GTK_LABEL(state->onair_title), 34);
    gtk_label_set_max_width_chars(GTK_LABEL(state->source), 24);
    gtk_label_set_max_width_chars(GTK_LABEL(state->destination), 28);
    pack(section, state->onair_title, FALSE);
    pack(target, state->source, FALSE);
    pack(target, state->destination, FALSE);
    pack(section, target, FALSE);
    state->indicators[0] = indicator("İnternet", "header-live-internet");
    state->indicators[1] = indicator("MAIN", "header-live-main");
    state->indicators[2] = indicator("MONITOR", "header-live-monitor");
    for (guint i = 0; i < G_N_ELEMENTS(state->indicators); ++i) pack(chain, state->indicators[i], FALSE);
    gtk_widget_set_tooltip_text(state->indicators[1], "Yerel MAIN ses çıkışının gözlenen durumu.");
    gtk_widget_set_tooltip_text(state->indicators[2], "Yerel MONITOR dinleme çıkışının gözlenen durumu.");
    pack(section, chain, FALSE);
    gtk_widget_set_hexpand(section, TRUE);
    return section;
}

static GtkWidget *
dialog_body(GtkWidget *dialog)
{
    GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(body), 18);
    gtk_box_set_spacing(GTK_BOX(body), 10);
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "ops-dialog");
    gtk_style_context_add_class(gtk_widget_get_style_context(dialog), "radios");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 470, -1);
    return body;
}

static void
support_export_response(GtkDialog *dialog, gint response, gpointer data)
{
    SupportState *state = data;
    if (response == GTK_RESPONSE_ACCEPT) {
        gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        GtkTextIter start, end;
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->note));
        gtk_text_buffer_get_bounds(buffer, &start, &end);
        gchar *note = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
        GError *error = NULL;
        if (filename != NULL && g_file_set_contents(filename, note, -1, &error)) {
            gtk_label_set_text(GTK_LABEL(state->status), "Bildirim taslağı dosyaya kaydedildi. Gönderim yapılmadı.");
            if (state->ctx->notify != NULL)
                state->ctx->notify(state->ctx->notify_data, "Bildirim taslağı indirildi; gönderim hedefi tanımlı değil.");
        } else
            gtk_label_set_text(GTK_LABEL(state->status), error != NULL ? error->message : "Dosya yolu seçilemedi.");
        g_clear_error(&error);
        g_free(filename);
        g_free(note);
    }
    state->chooser = NULL;
    gtk_widget_destroy(GTK_WIDGET(dialog));
}

static void
support_chooser_destroyed(GtkWidget *chooser, gpointer data)
{
    SupportState *state = data;
    if (state->chooser == chooser)
        state->chooser = NULL;
}

static void
support_response(GtkDialog *dialog, gint response, gpointer data)
{
    SupportState *state = data;
    if (response != GTK_RESPONSE_ACCEPT) {
        gtk_widget_destroy(GTK_WIDGET(dialog));
        return;
    }
    GtkTextIter start, end;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(state->note));
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    gchar *note = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);
    g_strstrip(note);
    gboolean valid = *note != '\0' && g_utf8_strlen(note, -1) <= 1500;
    g_free(note);
    if (!valid) {
        gtk_label_set_text(GTK_LABEL(state->status), "Sorun / gözlem alanına 1–1500 karakter gir.");
        gtk_widget_grab_focus(state->note);
        return;
    }
    if (state->chooser != NULL) {
        gtk_window_present(GTK_WINDOW(state->chooser));
        return;
    }
    state->chooser = gtk_file_chooser_dialog_new("Bildirim taslağını indir", GTK_WINDOW(dialog),
        GTK_FILE_CHOOSER_ACTION_SAVE, "Vazgeç", GTK_RESPONSE_CANCEL, "Kaydet", GTK_RESPONSE_ACCEPT, NULL);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(state->chooser), TRUE);
    gtk_file_chooser_set_do_overwrite_confirmation(GTK_FILE_CHOOSER(state->chooser), TRUE);
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(state->chooser), "RadioS-bildirim-taslagi.txt");
    if (state->ctx->config != NULL && state->ctx->config->project_root != NULL)
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(state->chooser), state->ctx->config->project_root);
    g_signal_connect(state->chooser, "response", G_CALLBACK(support_export_response), state);
    g_signal_connect(state->chooser, "destroy", G_CALLBACK(support_chooser_destroyed), state);
    gtk_widget_show(state->chooser);
}

static void
support_destroyed(GtkWidget *dialog, gpointer data)
{
    SupportState *state = data;
    (void)dialog;
    if (state->chooser != NULL) {
        g_signal_handlers_disconnect_by_data(state->chooser, state);
        gtk_widget_destroy(state->chooser);
        state->chooser = NULL;
    }
}

static void
support_open(GtkButton *button, gpointer data)
{
    RadioSViewContext *ctx = data;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Yöneticiye bildirim taslağı", ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT, "Vazgeç", GTK_RESPONSE_CANCEL,
        "Bildirim taslağını indir", GTK_RESPONSE_ACCEPT, NULL);
    SupportState *state = g_new0(SupportState, 1);
    GtkWidget *body = dialog_body(dialog);
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    (void)button;
    state->ctx = ctx;
    state->dialog = dialog;
    state->note = gtk_text_view_new();
    state->status = radios_ui_label("Gönderim hedefi tanımlı değil.", "ops-help");
    gtk_widget_set_name(dialog, "support-note-dialog");
    gtk_widget_set_name(state->note, "support-note");
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(state->note), GTK_WRAP_WORD_CHAR);
    gtk_widget_set_size_request(scroll, -1, 150);
    gtk_container_add(GTK_CONTAINER(scroll), state->note);
    pack(body, radios_ui_label("Sorun / gözlem", "ops-field-label"), FALSE);
    pack(body, scroll, TRUE);
    pack(body, state->status, FALSE);
    g_object_set_data_full(G_OBJECT(dialog), "support-state", state, g_free);
    g_signal_connect(dialog, "response", G_CALLBACK(support_response), state);
    g_signal_connect(dialog, "destroy", G_CALLBACK(support_destroyed), state);
    gtk_widget_show_all(dialog);
}

typedef struct {
    gboolean database, audio;
    RadioSAudioBusSnapshot main, monitor;
    RadioSRoutingOutputSnapshot routing;
    RadioSAudioOutputSnapshot stream;
    gboolean legacy_stream;
    guint streams_active, streams_pending, streams_failed;
    gchar engine_error[256], stream_error[256];
} HealthObservation;

static void observe_health(RadioSViewContext *ctx, HealthObservation *out, RadioSAudioSnapshot *raw)
{
    memset(out, 0, sizeof *out); out->database = radios_data_connected(ctx);
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(ctx);
    if (engine) {
        RadioSAudioSnapshot *snapshot = raw ? raw : g_new0(RadioSAudioSnapshot, 1);
        radios_audio_engine_snapshot(engine, snapshot); radios_audio_engine_unref(engine);
        out->audio = TRUE; out->main = snapshot->buses[RADIOS_AUDIO_BUS_MAIN];
        out->monitor = snapshot->buses[RADIOS_AUDIO_BUS_MONITOR];
        g_strlcpy(out->engine_error, snapshot->error, sizeof out->engine_error);
        if (!raw) g_free(snapshot);
    }
    radios_ui_routing_output_snapshot(ctx, &out->routing);
    out->streams_active = out->routing.active; out->streams_pending = out->routing.pending;
    out->streams_failed = out->routing.failed;
    g_strlcpy(out->stream_error, out->routing.error, sizeof out->stream_error);
    out->legacy_stream = radios_broadcast_ui_output_snapshot(ctx, "stream", &out->stream);
    if (out->legacy_stream) {
        if (out->stream.state == RADIOS_AUDIO_OUTPUT_ACTIVE) ++out->streams_active;
        if (out->stream.state == RADIOS_AUDIO_OUTPUT_STARTING) ++out->streams_pending;
        if (out->stream.state == RADIOS_AUDIO_OUTPUT_FAILED) {
            ++out->streams_failed;
            if (!out->stream_error[0]) g_strlcpy(out->stream_error, out->stream.error, sizeof out->stream_error);
        }
    }
}
static const gchar *health_problem(const HealthObservation *state)
{
    if (!state->database) return "Veritabanı bağlı değil";
    if (!state->audio) return "Ses motoru bekleniyor";
    if (state->engine_error[0]) return state->engine_error;
    if (state->main.error[0]) return state->main.error;
    if (!state->main.ready) return "MAIN çıkışı hazır değil";
    if (!state->main.gate) return "MAIN çıkışı kapalı";
    if (state->streams_failed) return state->stream_error[0] ? state->stream_error : "İnternet yayını hatası";
    if (state->routing.metadata_failed)
        return state->routing.metadata_error[0] ? state->routing.metadata_error : "Şarkı bilgisi gönderilemedi";
    if (state->legacy_stream && state->stream.metadata_state == RADIOS_AUDIO_METADATA_FAILED)
        return state->stream.metadata_error[0] ? state->stream.metadata_error : "Şarkı bilgisi gönderilemedi";
    return NULL;
}
static const gchar *bus_state(gboolean available, const RadioSAudioBusSnapshot *bus)
{ return !available ? "Bilinmiyor" : bus->error[0] ? "Hata" : !bus->ready ? "Hazır değil" : bus->gate ? "Açık" : "Kapalı"; }
static gchar *health_details(RadioSViewContext *ctx, const HealthObservation *state)
{
    const gchar *problem = health_problem(state);
    (void)ctx;
    return g_strdup_printf("Veritabanı: %s\nMAIN çıkışı: %s\nHedef: %s\nMONITOR çıkışı: %s\n"
        "İnternet yayını: %u aktif · %u hazırlanıyor · %u hata\nSon gözlenen sorun: %s\n"
        "Gözlem: Anlık uygulama ve ses çıkışı durumu; bu ekran uyarı kabul kaydı oluşturmaz.",
        state->database ? "Bağlı" : "Bağlı değil", bus_state(state->audio, &state->main),
        state->main.device[0] ? state->main.device : "—", bus_state(state->audio, &state->monitor),
        state->streams_active, state->streams_pending, state->streams_failed, problem ? problem : "Yok");
}
typedef struct { RadioSViewContext *ctx; GtkWidget *text; guint timer; } DetailsState;
static gboolean details_tick(gpointer data)
{
    DetailsState *state = data; HealthObservation observation; observe_health(state->ctx, &observation, NULL);
    gchar *message = health_details(state->ctx, &observation); gtk_label_set_text(GTK_LABEL(state->text), message); g_free(message);
    return G_SOURCE_CONTINUE;
}
static void details_destroyed(GtkWidget *widget, gpointer data)
{ (void)widget; DetailsState *state = data; if (state->timer) { g_source_remove(state->timer); state->timer = 0; } }
static void details_open(GtkButton *button, gpointer data)
{
    (void)button; RadioSViewContext *ctx = data;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Yayın ve bağlantı ayrıntıları", ctx->parent,
        GTK_DIALOG_DESTROY_WITH_PARENT, "Kapat", GTK_RESPONSE_CLOSE, NULL);
    gtk_widget_set_name(dialog, "diagnostic-details-dialog"); GtkWidget *body = dialog_body(dialog);
    DetailsState *state = g_new0(DetailsState, 1); state->ctx = ctx;
    state->text = radios_ui_label("", "ops-data-value"); gtk_widget_set_name(state->text, "diagnostic-live-observations");
    gtk_label_set_line_wrap(GTK_LABEL(state->text), TRUE); gtk_label_set_selectable(GTK_LABEL(state->text), TRUE);
    pack(body, state->text, TRUE);
    GtkWidget *actions = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "ops-dialog-actions");
    GtkWidget *support = radios_ui_button("Yöneticiye bildir"), *accept = radios_ui_button("Uyarıyı kabul et");
    gtk_widget_set_name(support, "diagnostic-open-support"); gtk_widget_set_name(accept, "diagnostic-acknowledge");
    gtk_widget_set_sensitive(accept, FALSE);
    gtk_widget_set_tooltip_text(accept, "Kabul edilebilir doğrulanmış bir veritabanı uyarısı yok.");
    g_signal_connect(support, "clicked", G_CALLBACK(support_open), ctx);
    pack(actions, support, FALSE); pack(actions, accept, FALSE); pack(body, actions, FALSE);
    g_object_set_data_full(G_OBJECT(dialog), "diagnostic-live-state", state, g_free);
    g_signal_connect(dialog, "destroy", G_CALLBACK(details_destroyed), state);
    g_signal_connect_swapped(dialog, "response", G_CALLBACK(gtk_widget_destroy), dialog);
    details_tick(state); state->timer = g_timeout_add(250, details_tick, state); gtk_widget_show_all(dialog);
}
typedef struct { RadioSViewContext *ctx; GtkWidget *connection, *warning, *backup; guint timer; } FooterHealth;
static gboolean footer_health_tick(gpointer data)
{
    FooterHealth *state = data; HealthObservation observed; observe_health(state->ctx, &observed, NULL);
    gtk_label_set_text(GTK_LABEL(state->connection), observed.database ? "Veritabanı  Bağlı" : "Veritabanı  Bağlı değil");
    const gchar *problem = health_problem(&observed); gchar *warning = g_strdup_printf("Uyarı  %s", problem ? problem : "Yok");
    gtk_label_set_text(GTK_LABEL(state->warning), warning); gtk_widget_set_tooltip_text(state->warning, problem); g_free(warning);
    const gchar *summary = state->ctx->parent ? g_object_get_data(G_OBJECT(state->ctx->parent), "radios-backup-summary") : NULL;
    const gchar *detail = state->ctx->parent ? g_object_get_data(G_OBJECT(state->ctx->parent), "radios-backup-summary-detail") : NULL;
    gchar *backup = g_strdup_printf("Yedek  %s", summary && *summary ? summary : "Okunmadı");
    if (g_strcmp0(gtk_button_get_label(GTK_BUTTON(state->backup)), backup))
        gtk_button_set_label(GTK_BUTTON(state->backup), backup);
    gtk_widget_set_tooltip_text(state->backup, detail); g_free(backup);
    return G_SOURCE_CONTINUE;
}
static void footer_health_destroyed(GtkWidget *widget, gpointer data)
{ (void)widget; FooterHealth *state = data; if (state->timer) { g_source_remove(state->timer); state->timer = 0; } }
GtkWidget *radios_header_health_new(RadioSViewContext *ctx)
{
    GtkWidget *row = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 8, "footer-health"); gtk_widget_set_name(row, "footer-health");
    GtkWidget *behavior = radios_ui_label("Devam davranışı  —", "header-behavior-text"); gtk_widget_set_name(behavior, "header-behavior-text");
    GtkWidget *connection = radios_ui_label("Veritabanı  Bağlı değil", "header-health-value");
    GtkWidget *backup = radios_ui_button("Yedek  Okunmadı"), *warning = radios_ui_label("Uyarı  —", "header-health-value"), *detail = radios_ui_button("Ayrıntı");
    gtk_style_context_add_class(gtk_widget_get_style_context(backup), "broadcast-health-link");
    gtk_style_context_add_class(gtk_widget_get_style_context(detail), "header-detail-button");
    gtk_widget_set_name(connection, "header-database-status"); gtk_widget_set_name(backup, "header-backup-details");
    gtk_widget_set_name(warning, "footer-warning-status"); gtk_widget_set_name(detail, "header-diagnostic-details");
    gtk_label_set_max_width_chars(GTK_LABEL(warning), 44); gtk_label_set_ellipsize(GTK_LABEL(warning), PANGO_ELLIPSIZE_END);
    g_signal_connect(detail, "clicked", G_CALLBACK(details_open), ctx);
    pack(row, behavior, FALSE); pack(row, connection, FALSE); pack(row, backup, FALSE); pack(row, warning, FALSE); pack(row, detail, FALSE);
    FooterHealth *state = g_new0(FooterHealth, 1); state->ctx = ctx; state->connection = connection; state->warning = warning; state->backup = backup;
    g_object_set_data_full(G_OBJECT(row), "footer-health-state", state, g_free); g_signal_connect(row, "destroy", G_CALLBACK(footer_health_destroyed), state);
    state->timer = g_timeout_add(250, footer_health_tick, state); footer_health_tick(state); return row;
}
static GtkWidget *next_new(HeaderState *state)
{
    GtkWidget *section = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 16, "header-next");
    GtkWidget *info = radios_ui_box(GTK_ORIENTATION_VERTICAL, 3, "header-next-info"), *countdown = radios_ui_box(GTK_ORIENTATION_VERTICAL, 3, "broadcast-countdown");
    GtkWidget *label = radios_ui_label("PARÇA BİTİŞİNE", "broadcast-label"); state->countdown = radios_ui_label("—:—", "header-countdown");
    gtk_widget_set_name(state->countdown, "header-track-countdown"); gtk_label_set_xalign(GTK_LABEL(label), 1); gtk_label_set_xalign(GTK_LABEL(state->countdown), 1);
    /* A-4: the caption follows the selection (PARÇA BİTİŞİNE / EVENT SÜRESİ / PARÇA SÜRESİ). */
    gtk_widget_set_name(label, "header-track-countdown-caption"); g_object_set_data(G_OBJECT(state->countdown), "countdown-caption", label);
    state->next_title = radios_ui_label("—", "header-next-title"); state->advert = radios_ui_label("Reklama kalan  —:—", "header-next-advert");
    gtk_widget_set_name(state->next_title, "header-next-track-title"); gtk_widget_set_name(state->advert, "header-next-advert-time");
    gtk_label_set_max_width_chars(GTK_LABEL(state->next_title), 24);
    pack(info, radios_ui_label("GERÇEK SIRADAKİ", "broadcast-label"), FALSE); pack(info, state->next_title, FALSE); pack(info, state->advert, FALSE);
    pack(countdown, label, FALSE); pack(countdown, state->countdown, FALSE); pack(section, info, TRUE); pack(section, countdown, FALSE);
    gtk_widget_set_hexpand(section, TRUE); return section;
}
static void header_set(GtkWidget *label, const gchar *text)
{
    const gchar *value = text && *text ? text : "—";
    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(label)), value)) gtk_label_set_text(GTK_LABEL(label), value);
    gtk_widget_set_tooltip_text(label, value);
}
static void indicator_set(GtkWidget *row, guint state, const gchar *text)
{
    GtkWidget *value = g_object_get_data(G_OBJECT(row), "live-value"), *led = g_object_get_data(G_OBJECT(row), "live-led");
    header_set(value, text);
    if (GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(led), "live-state")) != state) {
        g_object_set_data(G_OBJECT(led), "live-state", GUINT_TO_POINTER(state)); gtk_widget_queue_draw(led);
    }
}
static gchar *countdown_text(gint64 microseconds)
{
    if (microseconds < 0) return g_strdup("—:—");
    guint64 seconds = (guint64)microseconds / G_TIME_SPAN_SECOND + (microseconds % G_TIME_SPAN_SECOND != 0);
    return g_strdup_printf("%02" G_GUINT64_FORMAT ":%02" G_GUINT64_FORMAT, seconds / 60, seconds % 60);
}
static gboolean header_live_tick(gpointer data)
{
    HeaderState *state = data; if (state->disposed) return G_SOURCE_REMOVE;
    HealthObservation health; RadioSAudioSnapshot *audio = g_new0(RadioSAudioSnapshot, 1);
    observe_health(state->ctx, &health, audio); RadioSBroadcastHeaderSnapshot current = {0};
    gboolean observed = radios_broadcast_header_snapshot(state->ctx, &current);
    gchar *title = current.current ? g_strdup_printf("%s%s%s%s", current.artist, current.artist[0] ? " · " : "", current.title,
        current.paused ? " · Duraklatıldı" : "") : NULL;
    header_set(state->onair_title, title); g_free(title);
    gchar *destination = g_strdup_printf("Hedef çıkış  %s", health.main.device[0] ? health.main.device : "—"); header_set(state->destination, destination); g_free(destination);
    gint selected = -1; RadioSPlayoutDeckMetadata metadata = {0};
    if (observed && current.current && current.local_deck && current.deck < RADIOS_AUDIO_NORMAL_DECKS &&
        current.entry_id[0] && !g_strcmp0(current.entry_id, audio->decks[current.deck].binding.entry_id)) {
        guint deck = current.deck; RadioSAudioDeckSnapshot *raw = &audio->decks[deck]; selected = (gint)deck;
        gboolean managed = radios_broadcast_player_deck_metadata(state->ctx, deck, &metadata);
        RadioSViewContext *scoped = radios_player_workspace_running_context(state->ctx->players, deck);
        if (scoped) radios_playout_ui_deck_metadata(scoped, deck, &metadata);
        if (!metadata.next_title[0] && !managed && !scoped) {
            JsonArray *items = NULL; guint index = 0, owner_deck = G_MAXUINT;
            if (radios_broadcast_player_playlist(state->ctx, current.card, &items, NULL, &index, &owner_deck) && owner_deck == deck && index < json_array_get_length(items)) {
                JsonObject *input = json_array_get_object_element(items, index);
                gboolean same = !g_strcmp0(radios_json_string(input, "entry_id", ""), raw->binding.entry_id) &&
                    !g_strcmp0(radios_json_string(input, "media_id", ""), raw->media_id);
                if (same && index + 1 < json_array_get_length(items)) {
                    JsonObject *next = json_array_get_object_element(items, index + 1);
                    g_strlcpy(metadata.next_title, radios_json_string(next, "title", ""), sizeof metadata.next_title);
                    g_strlcpy(metadata.next_artist, radios_json_string(next, "artist", ""), sizeof metadata.next_artist);
                }
            }
            g_clear_pointer(&items, json_array_unref);
        }
        gboolean retained = !g_strcmp0(state->next_entry[deck], current.entry_id) && state->next_generation[deck] == raw->generation;
        if (metadata.next_title[0] || !current.paused || !retained) {
            gchar *next = metadata.next_title[0] ? g_strdup_printf("%s%s%s", metadata.next_artist,
                metadata.next_artist[0] ? " · " : "", metadata.next_title) : g_strdup("");
            g_strlcpy(state->next_cached[deck], next, sizeof state->next_cached[deck]); g_free(next);
            g_strlcpy(state->next_entry[deck], current.entry_id, sizeof state->next_entry[deck]);
            state->next_generation[deck] = raw->generation;
        }
    }
    gchar *source = selected >= 0 ? g_strdup_printf("Aktif kaynak  Oynatıcı %d", selected + 1) :
        current.current ? g_strdup("Aktif kaynak  MAIN") : g_strdup("Aktif kaynak  —");
    header_set(state->source, source); g_free(source); header_set(state->next_title, selected >= 0 ? state->next_cached[selected] : "");
    gint64 remaining = -1;
    if (observed && current.current && !current.waiting && current.duration_ms) {
        if (current.track_remaining_us > 0) remaining = current.track_remaining_us;
        else if (current.duration_ms >= current.position_ms) remaining = (gint64)ceil((current.duration_ms - current.position_ms) * 1000.0 /
            (isfinite(current.tempo_ratio) && current.tempo_ratio > 0 ? current.tempo_ratio : 1));
    }
    /* A-4: a selected flow item that is not the sounding one shows its own
     * duration in this counter; the sounding selection, or none, keeps the
     * remaining time of the playing track. */
    GtkWidget *caption = g_object_get_data(G_OBJECT(state->countdown), "countdown-caption");
    const gchar *caption_text = "PARÇA BİTİŞİNE";
    if (observed && current.selected && !current.selected_playing && current.selected_duration_ms >= 0) {
        remaining = current.selected_duration_ms * G_TIME_SPAN_MILLISECOND;
        caption_text = current.selected_content ? "PARÇA SÜRESİ" : "EVENT SÜRESİ";
    }
    if (caption && g_strcmp0(gtk_label_get_text(GTK_LABEL(caption)), caption_text)) gtk_label_set_text(GTK_LABEL(caption), caption_text);
    gchar *clock = countdown_text(remaining); header_set(state->countdown, clock); g_free(clock);
    gchar *advert_clock = countdown_text(current.advert_timing_known ? current.advert_remaining_us : -1);
    gchar *advert = g_strdup_printf("Reklama kalan  %s%s", advert_clock,
        current.advert_timing_known && current.paused ? " · Duraklatıldı" : "");
    header_set(state->advert, advert); g_free(advert); g_free(advert_clock);
    const gchar *internet = health.streams_failed ? "Hata" : health.streams_active ? "Aktif" : health.streams_pending ? "Hazırlanıyor" : "Kapalı";
    indicator_set(state->indicators[0], health.streams_failed ? 3 : health.streams_active ? 1 : health.streams_pending ? 2 : 0, internet);
    indicator_set(state->indicators[1], health.main.error[0] ? 3 : health.main.ready && health.main.gate ? 1 : 0, bus_state(health.audio, &health.main));
    indicator_set(state->indicators[2], health.monitor.error[0] ? 3 : health.monitor.ready && health.monitor.gate ? 1 : 0, bus_state(health.audio, &health.monitor));
    g_free(audio); return G_SOURCE_CONTINUE;
}

static gboolean
header_clock_update(gpointer data)
{
    HeaderState *state = data;
    static const gchar *const months[] = {
        "Ocak", "Şubat", "Mart", "Nisan", "Mayıs", "Haziran",
        "Temmuz", "Ağustos", "Eylül", "Ekim", "Kasım", "Aralık"
    };
    static const gchar *const days[] = { "Pzt", "Sal", "Çar", "Per", "Cum", "Cmt", "Paz" };
    GDateTime *now = radios_station_now(state->ctx);
    const gchar *zone = radios_station_timezone(state->ctx);
    gchar *caption = g_strdup_printf("SAAT  %s", zone);
    gtk_label_set_text(GTK_LABEL(state->clock_label), caption); g_free(caption);
    g_object_set_data_full(G_OBJECT(state->analog), "radios-clock-timezone", g_strdup(zone), g_free);
    gtk_widget_queue_draw(state->analog);
    gchar *time = g_date_time_format(now, "%H:%M:%S");
    gchar *date = g_strdup_printf("%d %s %d · %s", g_date_time_get_day_of_month(now),
        months[g_date_time_get_month(now) - 1], g_date_time_get_year(now),
        days[g_date_time_get_day_of_week(now) - 1]);
    gtk_label_set_text(GTK_LABEL(state->time), time);
    gtk_label_set_text(GTK_LABEL(state->calendar), date);
    g_free(time);
    g_free(date);
    g_date_time_unref(now);
    return G_SOURCE_CONTINUE;
}

static GtkWidget *
time_new(HeaderState *state)
{
    GtkWidget *section = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 20, "header-time");
    GtkWidget *clock = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 10, "header-clock-summary");
    GtkWidget *reading = radios_ui_box(GTK_ORIENTATION_VERTICAL, 1, "header-clock-reading");
    GtkWidget *programme = radios_ui_box(GTK_ORIENTATION_VERTICAL, 3, "header-programme");
    state->time = radios_ui_label("--:--:--", "header-clock-time");
    state->calendar = radios_ui_label("—", "header-calendar");
    state->analog = radios_analog_clock_new(); pack(clock, state->analog, FALSE);
    state->clock_label = radios_ui_label("SAAT", "header-clock-label"); pack(reading, state->clock_label, FALSE);
    pack(reading, state->time, FALSE);
    pack(reading, state->calendar, FALSE);
    pack(clock, reading, FALSE);
    pack(section, clock, FALSE);
    pack(programme, radios_ui_label("MEVCUT PROGRAM", "broadcast-label"), FALSE);
    pack(programme, radios_ui_label("—", "header-current-programme"), FALSE);
    pack(programme, radios_ui_label("Bitimine  —:—", "header-programme-end"), FALSE);
    pack(section, programme, TRUE);
    g_object_set_data(G_OBJECT(section), "header-clock-programme", programme);
    gtk_widget_set_hexpand(section, TRUE);
    return section;
}

static gboolean
audio_icon_draw(GtkWidget *widget, cairo_t *cr, gpointer data)
{
    gboolean headphone = GPOINTER_TO_INT(data);
    (void)widget;
    cairo_scale(cr, 0.5, 0.5);
    cairo_set_source_rgb(cr, 0.58, 0.66, 0.72);
    cairo_set_line_width(cr, 1.5);
    if (headphone) {
        cairo_arc(cr, 12, 11, 8, 3.141592653589793, 6.283185307179586);
        cairo_stroke(cr);
        cairo_rectangle(cr, 3, 12, 4, 8);
        cairo_rectangle(cr, 17, 12, 4, 8);
        cairo_stroke(cr);
    } else {
        cairo_move_to(cr, 3, 9); cairo_line_to(cr, 7, 9);
        cairo_line_to(cr, 12, 5); cairo_line_to(cr, 12, 19);
        cairo_line_to(cr, 7, 15); cairo_line_to(cr, 3, 15);
        cairo_close_path(cr); cairo_stroke(cr);
        cairo_arc(cr, 12, 12, 6, -0.7, 0.7); cairo_stroke(cr);
        cairo_arc(cr, 12, 12, 10, -0.7, 0.7); cairo_stroke(cr);
    }
    return FALSE;
}

static GtkWidget *
monitor_toggle_new(const gchar *title)
{
    GtkWidget *toggle = gtk_toggle_button_new_with_label(title);
    gtk_style_context_add_class(gtk_widget_get_style_context(toggle), "ld-btn");
    return toggle;
}

static void
monitor_destroyed(GtkWidget *module, gpointer data)
{
    MonitorState *state = data;
    (void)module;
    g_signal_handlers_disconnect_by_data(state->source, state);
    g_signal_handlers_disconnect_by_data(state->toggle, state);
    if (state->destination != NULL)
        g_signal_handlers_disconnect_by_data(state->destination, state);
}

static void
monitor_render(MonitorState *state)
{
    gboolean selected = gtk_combo_box_get_active(GTK_COMBO_BOX(state->source)) > 0 &&
        (state->destination == NULL || gtk_combo_box_get_active(GTK_COMBO_BOX(state->destination)) > 0);
    gchar *source = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(state->source));
    gchar *destination = state->destination != NULL ?
        gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(state->destination)) : NULL;
    gchar *status = selected ? g_strdup_printf("%s%s%s · %s %s. Arayüz taslağı; ses motoru bağlı değil.",
        source, destination != NULL ? " → " : "", destination != NULL ? destination : "",
        state->headphone ? "CUE" : "MON", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(state->toggle)) ? "seçili" : "kapalı") :
        g_strdup(state->headphone ? "Önce kaynak seç. Ses motoru bağlı değil." : "Önce kaynak ve çıkış seç. Ses motoru bağlı değil.");
    gtk_widget_set_sensitive(state->toggle, selected);
    gtk_widget_set_tooltip_text(state->toggle, status);
    atk_object_set_description(gtk_widget_get_accessible(state->toggle), status);
    g_free(source);
    g_free(destination);
    g_free(status);
}

static void
monitor_changed(GtkComboBox *combo, gpointer data)
{
    MonitorState *state = data;
    (void)combo;
    state->resetting = TRUE;
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(state->toggle), FALSE);
    state->resetting = FALSE;
    monitor_render(state);
}

static void
monitor_toggled(GtkToggleButton *button, gpointer data)
{
    MonitorState *state = data;
    (void)button;
    if (state->resetting)
        return;
    monitor_render(state);
    if (state->ctx->notify != NULL)
        state->ctx->notify(state->ctx->notify_data,
            "Monitor / CUE arayüz seçimi güncellendi; gerçek ses motoru ve çıkış bağlantısı henüz yok.");
}

static GtkWidget *
audio_module_new(RadioSViewContext *ctx, gboolean headphone)
{
    static const gchar *const sources[] = {
        "Kaynak seç", "MASTER", "Oynatıcı A", "Oynatıcı B", "Mixer sampler", "Jingle",
        "FX 1 SEND", "FX 1 RETURN", "FX 2 SEND", "FX 2 RETURN", "FX 3 SEND", "FX 3 RETURN", NULL
    };
    static const gchar *const outputs[] = { "Çıkış seç", "Monitor A", "Monitor B", "Hat çıkışı", NULL };
    GtkWidget *module = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, "header-audio-module");
    GtkWidget *heading = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 5, "cue-module-heading");
    GtkWidget *icon = gtk_drawing_area_new();
    GtkWidget *grid = gtk_grid_new();
    GtkWidget *knob = radios_knob_new("", -60, 6, 0.5, -12);
    GtkWidget *source = radios_ui_box(GTK_ORIENTATION_VERTICAL, 2, "header-monitor-source");
    GtkWidget *cue = monitor_toggle_new(headphone ? "CUE" : "MON");
    GtkWidget *combo = radios_ui_combo(sources, 0);
    GtkWidget *meter = radios_meter_new(10, 44, TRUE);
    gtk_widget_set_name(knob, headphone ? "header-cue-gain" : "header-monitor-gain");
    gtk_widget_set_name(meter, headphone ? "header-cue-meter" : "header-monitor-meter");
    MonitorState *state = g_new0(MonitorState, 1);
    state->ctx = ctx;
    state->source = combo;
    state->toggle = cue;
    state->headphone = headphone;
    gtk_widget_set_name(module, headphone ? "header-headphone-monitor" : "header-output-monitor");
    gtk_widget_set_name(combo, headphone ? "header-headphone-source" : "header-output-source");
    gtk_widget_set_name(cue, headphone ? "header-headphone-cue" : "header-output-cue");
    gtk_widget_set_size_request(icon, 12, 12);
    gtk_widget_set_valign(icon, GTK_ALIGN_CENTER);
    g_signal_connect(icon, "draw", G_CALLBACK(audio_icon_draw), GINT_TO_POINTER(headphone));
    pack(heading, icon, FALSE);
    pack(heading, radios_ui_label(headphone ? "Kulaklık" : "Monitor", "cue-module-title"), TRUE);
    pack(heading, radios_ui_label(headphone ? "CUE" : "ÇIKIŞ", "cue-module-context"), FALSE);
    pack(module, heading, FALSE);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 6);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_widget_set_valign(knob, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(meter, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(cue, GTK_ALIGN_END);
    gtk_widget_set_hexpand(combo, TRUE);
    GList *renderers = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(combo));
    for (GList *item = renderers; item != NULL; item = item->next)
        if (GTK_IS_CELL_RENDERER_TEXT(item->data))
            g_object_set(item->data, "ellipsize", PANGO_ELLIPSIZE_END,
                         "width-chars", 8, "max-width-chars", 8, NULL);
    g_list_free(renderers);
    gtk_widget_set_size_request(cue, 46, 30);
    gtk_style_context_add_class(gtk_widget_get_style_context(cue), "header-cue-button");
    GtkWidget *source_caption = radios_ui_label("MONITOR", "header-monitor-source-label");
    g_object_set_data(G_OBJECT(module), "header-audio-source-caption", source_caption);
    pack(source, source_caption, FALSE);
    pack(source, combo, FALSE);
    gtk_grid_attach(GTK_GRID(grid), knob, 0, 0, 1, 2);
    gtk_grid_attach(GTK_GRID(grid), meter, 1, 0, 1, 2);
    gtk_grid_attach(GTK_GRID(grid), source, 2, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), cue, 3, 0, 1, 1);
    if (!headphone) {
        GtkWidget *destination = radios_ui_box(GTK_ORIENTATION_VERTICAL, 2, "header-monitor-source");
        state->destination = radios_ui_combo(outputs, 0);
        gtk_widget_set_name(state->destination, "header-output-destination");
        GList *destination_renderers = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(state->destination));
        for (GList *item = destination_renderers; item != NULL; item = item->next)
            if (GTK_IS_CELL_RENDERER_TEXT(item->data))
                g_object_set(item->data, "ellipsize", PANGO_ELLIPSIZE_END,
                             "width-chars", 14, "max-width-chars", 14, NULL);
        g_list_free(destination_renderers);
        gtk_combo_box_set_popup_fixed_width(GTK_COMBO_BOX(state->destination), FALSE);
        GtkWidget *destination_caption = radios_ui_label("ÇIKIŞ", "header-monitor-source-label");
        g_object_set_data(G_OBJECT(module), "header-audio-destination-caption", destination_caption);
        pack(destination, destination_caption, FALSE);
        pack(destination, state->destination, FALSE);
        gtk_grid_attach(GTK_GRID(grid), destination, 2, 1, 2, 1);
        if (!ctx->audio) g_signal_connect(state->destination, "changed", G_CALLBACK(monitor_changed), state);
    } else {
        GtkWidget *second_row = gtk_drawing_area_new();
        gtk_widget_set_size_request(second_row, -1, 44);
        g_object_set_data(G_OBJECT(module), "header-audio-spacer", second_row);
        gtk_grid_attach(GTK_GRID(grid), second_row, 2, 1, 2, 1);
    }
    gtk_widget_set_hexpand(source, TRUE);
    pack(module, grid, FALSE);
    gtk_widget_set_size_request(module, 210, -1);
    gtk_widget_set_valign(module, GTK_ALIGN_CENTER);
    g_object_set_data_full(G_OBJECT(module), "monitor-state", state, g_free);
    g_object_set_data(G_OBJECT(module), "monitor-source", combo);
    g_object_set_data(G_OBJECT(module), "monitor-destination", state->destination);
    g_object_set_data(G_OBJECT(module), "monitor-toggle", cue);
    if (!ctx->audio) {
        g_signal_connect(combo, "changed", G_CALLBACK(monitor_changed), state);
        g_signal_connect(cue, "toggled", G_CALLBACK(monitor_toggled), state);
    } else {
        RadioSAudioUiBus bus = headphone ? RADIOS_AUDIO_UI_CUE : RADIOS_AUDIO_UI_MONITOR;
        radios_audio_ui_bind_gain(ctx, knob, bus); radios_audio_ui_bind_meter(ctx, meter, bus);
        radios_audio_ui_bind_monitor(ctx, module, combo, state->destination, cue, bus);
    }
    g_signal_connect(module, "destroy", G_CALLBACK(monitor_destroyed), state);
    if (!ctx->audio) monitor_render(state);
    return module;
}

typedef struct { RadioSViewContext *ctx; GtkWidget *knob; guint band, timer; gboolean syncing, disposed; } HeaderEq;
static gdouble *header_eq_value(RadioSAudioMixerConfig *config, guint band)
{ return band == 0 ? &config->master.eq_high_db : band == 1 ? &config->master.eq_mid_db : &config->master.eq_low_db; }
static void header_eq_changed(GtkAdjustment *adjustment, gpointer data)
{
    GtkWidget *knob = data; HeaderEq *state = g_object_get_data(G_OBJECT(knob), "header-eq-state");
    if (!state || state->disposed || state->syncing) return;
    RadioSAudioMixerConfig config; if (!radios_audio_ui_get_mixer(state->ctx, &config, NULL, NULL)) return;
    *header_eq_value(&config, state->band) = gtk_adjustment_get_value(adjustment); GError *error = NULL;
    if (!radios_audio_ui_set_mixer(state->ctx, &config, &error) && error && state->ctx->notify) state->ctx->notify(state->ctx->notify_data, error->message);
    g_clear_error(&error);
}
static gboolean header_eq_tick(gpointer data)
{
    HeaderEq *state = data; if (state->disposed) return G_SOURCE_REMOVE;
    RadioSAudioMixerConfig config; if (!radios_audio_ui_get_mixer(state->ctx, &config, NULL, NULL)) return G_SOURCE_CONTINUE;
    state->syncing = TRUE; gtk_adjustment_set_value(g_object_get_data(G_OBJECT(state->knob), "adjustment"), *header_eq_value(&config, state->band)); state->syncing = FALSE;
    return G_SOURCE_CONTINUE;
}
static void header_eq_destroyed(GtkWidget *widget, gpointer data)
{
    (void)widget; HeaderEq *state = data; state->disposed = TRUE;
    if (state->timer) { g_source_remove(state->timer); state->timer = 0; }
}
static void header_eq_bind(RadioSViewContext *ctx, GtkWidget *knob, guint band)
{
    HeaderEq *state = g_new0(HeaderEq, 1); state->ctx = ctx; state->knob = knob; state->band = band;
    g_object_set_data_full(G_OBJECT(knob), "header-eq-state", state, g_free); g_signal_connect(knob, "destroy", G_CALLBACK(header_eq_destroyed), state);
    g_signal_connect_object(g_object_get_data(G_OBJECT(knob), "adjustment"), "value-changed", G_CALLBACK(header_eq_changed), knob, 0);
    GtkWidget *dial = g_object_get_data(G_OBJECT(knob), "dial"); gtk_widget_set_tooltip_text(dial, "Gerçek MASTER EQ. Tercihi DJ mixer ayarlarından kaydet.");
    state->timer = g_timeout_add(50, header_eq_tick, state); header_eq_tick(state);
}
static GtkWidget *
master_new(RadioSViewContext *ctx)
{
    GtkWidget *module = radios_ui_box(GTK_ORIENTATION_VERTICAL, 3, "header-master-output");
    GtkWidget *knobs = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 4, "header-eq-controls");
    const gchar *names[] = { "L", "M", "H", "MASTER" };
    const gchar *eq_ids[] = { "header-main-eq-low", "header-main-eq-mid", "header-main-eq-high" };
    const guint eq_bands[] = { 2, 1, 0 };
    pack(module, radios_ui_label("Çıkış seviyesi", "meter-caption"), FALSE);
    GtkWidget *meter = radios_meter_new(212, 34, FALSE); gtk_widget_set_name(meter, "header-main-meter");
    pack(module, meter, FALSE); radios_audio_ui_bind_meter(ctx, meter, RADIOS_AUDIO_UI_MAIN);
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkWidget *knob = radios_knob_new(names[i], i == 3 ? -60 : -26, 6, 0.5, 0);
        if (i < 3) gtk_widget_set_name(knob, eq_ids[i]);
        if (i == 3) {
            gtk_widget_set_name(knob, "header-main-gain"); radios_audio_ui_bind_gain(ctx, knob, RADIOS_AUDIO_UI_MAIN);
        } else if (ctx->audio) header_eq_bind(ctx, knob, eq_bands[i]);
        GtkWidget *dial = g_object_get_data(G_OBJECT(knob), "dial");
        if (dial != NULL)
            gtk_widget_set_size_request(dial, 30, 30);
        pack(knobs, knob, TRUE);
    }
    pack(module, knobs, FALSE);
    gtk_widget_set_size_request(module, 212, -1);
    gtk_widget_set_valign(module, GTK_ALIGN_CENTER);
    return module;
}

static GtkWidget *
audio_new(RadioSViewContext *ctx)
{
    GtkWidget *audio = gtk_grid_new();
    gtk_style_context_add_class(gtk_widget_get_style_context(audio), "header-meter");
    gtk_grid_set_column_spacing(GTK_GRID(audio), 14);
    gtk_grid_set_row_spacing(GTK_GRID(audio), 8);
    GtkWidget *modules[] = {audio_module_new(ctx, FALSE), audio_module_new(ctx, TRUE), master_new(ctx)};
    for (guint i = 0; i < G_N_ELEMENTS(modules); ++i) {
        g_object_set_data(G_OBJECT(modules[i]), "header-audio-index", GUINT_TO_POINTER(i + 1));
        gtk_grid_attach(GTK_GRID(audio), modules[i], i, 0, 1, 1);
    }
    gtk_widget_set_valign(audio, GTK_ALIGN_FILL);
    gtk_widget_set_hexpand(audio, FALSE);
    return audio;
}

static void header_audio_columns(HeaderState *state, gint columns)
{
    GtkWidget *audio = state->sections[AUDIO];
    GList *children = gtk_container_get_children(GTK_CONTAINER(audio));
    for (GList *item = children; item; item = item->next) {
        guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(item->data), "header-audio-index")) - 1;
        guint column = index % columns, row = index / columns;
        gtk_container_child_set(GTK_CONTAINER(audio), item->data, "left-attach", column,
            "top-attach", row, "width", columns == 2 && index == 2 ? 2 : 1, NULL);
    }
    g_list_free(children); state->audio_columns = columns;
}

static void header_single_line(GtkWidget *widget)
{
    /* Live programme details wrap their clocks as complete words on narrow windows. */
    if (g_str_equal(gtk_widget_get_name(widget), "header-status-panel")) return;
    if (GTK_IS_LABEL(widget)) {
        gtk_label_set_line_wrap(GTK_LABEL(widget), FALSE);
        gtk_label_set_single_line_mode(GTK_LABEL(widget), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_END);
    }
    if (!GTK_IS_CONTAINER(widget)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (GList *item = children; item; item = item->next) header_single_line(item->data);
    g_list_free(children);
}

static void header_optional(GtkWidget *widget, gboolean visible)
{
    if (!widget) return;
    gtk_widget_set_no_show_all(widget, !visible);
    gtk_widget_set_visible(widget, visible);
}
static void header_status_compact_labels(GtkWidget *widget, gboolean compact)
{
    if (GTK_IS_LABEL(widget)) {
        guint saved = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(widget), "header-status-original-label"));
        if (!saved) {
            saved = 1 | ((guint)gtk_label_get_line_wrap(GTK_LABEL(widget)) << 1) |
                ((guint)gtk_label_get_single_line_mode(GTK_LABEL(widget)) << 2) |
                ((guint)gtk_label_get_ellipsize(GTK_LABEL(widget)) << 3);
            g_object_set_data(G_OBJECT(widget), "header-status-original-label", GUINT_TO_POINTER(saved));
        }
        gtk_label_set_line_wrap(GTK_LABEL(widget), compact ? FALSE : (saved & 2) != 0);
        gtk_label_set_single_line_mode(GTK_LABEL(widget), compact || (saved & 4) != 0);
        gtk_label_set_ellipsize(GTK_LABEL(widget), compact ? PANGO_ELLIPSIZE_END : (PangoEllipsizeMode)(saved >> 3));
    }
    if (!GTK_IS_CONTAINER(widget)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (GList *item = children; item; item = item->next) header_status_compact_labels(item->data, compact);
    g_list_free(children);
}

static void header_compact(HeaderState *state, gboolean compact)
{
    GtkStyleContext *style = gtk_widget_get_style_context(state->grid);
    if (compact) gtk_style_context_add_class(style, "header-compact");
    else gtk_style_context_remove_class(style, "header-compact");
    GtkWidget *next = state->sections[NEXT];
    gboolean live = gtk_style_context_has_class(gtk_widget_get_style_context(next), "header-status-bound");
    header_optional(next, live || !compact);
    GList *next_children = gtk_container_get_children(GTK_CONTAINER(next));
    for (GList *item = next_children; item; item = item->next) {
        if (g_str_equal(gtk_widget_get_name(item->data), "header-status-panel")) {
            /* Parallel live fields avoid making the entire On Air row as
             * tall as every programme/playlist clock stacked together. */
            gtk_orientable_set_orientation(GTK_ORIENTABLE(item->data), compact ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL);
            header_status_compact_labels(item->data, compact);
            continue;
        }
        header_optional(item->data, !compact);
    }
    g_list_free(next_children);
    header_optional(g_object_get_data(G_OBJECT(state->sections[ONAIR]), "header-onair-target"), !compact);
    header_optional(g_object_get_data(G_OBJECT(state->sections[ONAIR]), "header-onair-chain"), !compact);
    GList *children = gtk_container_get_children(GTK_CONTAINER(state->sections[AUDIO]));
    for (GList *item = children; item; item = item->next) {
        if (GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(item->data), "header-audio-index")) == 3)
            header_optional(item->data, !compact);
        else {
            /* The controls retain their tooltips and accessible names; the
             * compact strip omits repeated captions and the CUE filler row. */
            header_optional(g_object_get_data(G_OBJECT(item->data), "header-audio-source-caption"), !compact);
            header_optional(g_object_get_data(G_OBJECT(item->data), "header-audio-destination-caption"), !compact);
            header_optional(g_object_get_data(G_OBJECT(item->data), "header-audio-spacer"), !compact);
        }
    }
    g_list_free(children);
    gtk_orientable_set_orientation(GTK_ORIENTABLE(state->sections[BEHAVIOR]),
        compact ? GTK_ORIENTATION_HORIZONTAL : GTK_ORIENTATION_VERTICAL);
    state->compact = compact;
}

static void
header_layout(HeaderState *state, gint layout)
{
    static const gint placements[4][6][4] = {
        { {0,0,1,2}, {1,0,1,1}, {1,1,1,1}, {2,0,1,1}, {2,1,1,1}, {3,0,1,2} },
        { {0,0,1,2}, {1,0,1,1}, {1,1,1,1}, {2,0,1,1}, {2,1,1,1}, {0,2,3,1} },
        { {0,0,1,1}, {0,1,1,1}, {0,2,2,1}, {1,1,1,1}, {1,0,1,1}, {0,3,2,1} },
        { {0,0,1,1}, {0,1,1,1}, {0,2,2,1}, {1,1,1,1}, {1,0,1,1}, {0,3,2,1} }
    };
    for (guint i = 0; i < G_N_ELEMENTS(state->sections); ++i) {
        const gint *position = placements[layout][i];
        if (gtk_widget_get_parent(state->sections[i]) != NULL)
            gtk_container_remove(GTK_CONTAINER(state->grid), state->sections[i]);
        gtk_grid_attach(GTK_GRID(state->grid), state->sections[i], position[0], position[1], position[2], position[3]);
    }
    GtkWidget *progress = g_object_get_data(G_OBJECT(state->grid), "radios-song-progress");
    if (progress)
        gtk_container_child_set(GTK_CONTAINER(state->grid), progress, "width", layout >= 2 ? 2 : 4 - layout, NULL);
    state->layout = layout;
}

static gboolean
header_layout_idle(gpointer data)
{
    HeaderState *state = data;
    state->layout_idle = 0;
    if (!state->disposed && state->pending_compact != state->compact)
        header_compact(state, state->pending_compact);
    if (!state->disposed && state->pending_layout != state->layout) {
        /* Reparent outside size-allocate so GTK can allocate the new grid. */
        header_layout(state, state->pending_layout);
        gtk_widget_queue_resize(state->grid);
    }
    if (!state->disposed && state->pending_audio_columns != state->audio_columns)
        header_audio_columns(state, state->pending_audio_columns);
    return G_SOURCE_REMOVE;
}

static void
header_size_allocated(GtkWidget *widget, GtkAllocation *allocation, gpointer data)
{
    HeaderState *state = data;
    (void)widget;
    state->pending_compact = allocation->width < 1100;
    state->pending_layout = state->pending_compact ? 3 : allocation->width > 1560 ? 0 : (allocation->width > 1100 ? 1 : 2);
    state->pending_audio_columns = allocation->width > 1100 ? 3 : 2;
    if (!state->disposed && (state->pending_layout != state->layout ||
            state->pending_audio_columns != state->audio_columns ||
            state->pending_compact != state->compact) && !state->layout_idle)
        state->layout_idle = g_idle_add(header_layout_idle, state);
}

static void
header_destroyed(GtkWidget *widget, gpointer data)
{
    HeaderState *state = data;
    (void)widget;
    state->disposed = TRUE;
    if (state->layout_idle) {
        g_source_remove(state->layout_idle);
        state->layout_idle = 0;
    }
    if (state->live_timer) { g_source_remove(state->live_timer); state->live_timer = 0; }
    if (state->timer != 0) {
        g_source_remove(state->timer);
        state->timer = 0;
    }
}

static void
header_state_free(gpointer data)
{
    HeaderState *state = data;
    for (guint i = 0; i < G_N_ELEMENTS(state->sections); ++i)
        g_object_unref(state->sections[i]);
    g_free(state);
}

GtkWidget *
radios_header_new(RadioSViewContext *ctx)
{
    HeaderState *state = g_new0(HeaderState, 1); state->ctx = ctx;
    GtkWidget *header = gtk_grid_new();
    state->grid = header;
    gtk_widget_set_name(header, "shared-header");
    state->layout = -1;
    gtk_style_context_add_class(gtk_widget_get_style_context(header), "page-header");
    gtk_grid_set_row_spacing(GTK_GRID(header), 10);
    gtk_grid_set_column_spacing(GTK_GRID(header), 0);
    state->sections[BRAND] = brand_new();
    state->sections[ONAIR] = onair_new(state);
    state->sections[BEHAVIOR] = radios_header_navigation_new(ctx);
    state->sections[NEXT] = next_new(state);
    state->sections[TIME] = time_new(state);
    state->sections[AUDIO] = audio_new(ctx);
    header_audio_columns(state, 2);
    for (guint i = 0; i < G_N_ELEMENTS(state->sections); ++i)
        g_object_ref_sink(state->sections[i]);
    header_layout(state, 1);
    /* PDF4: the song timeline spans the final row in every header layout. */
    GtkWidget *progress = radios_header_progress_new(ctx);
    gtk_grid_attach(GTK_GRID(header), progress, 0, 4, 3, 1);
    g_object_set_data(G_OBJECT(header), "radios-song-progress", progress);
    header_single_line(header);
    header_clock_update(state);
    state->timer = g_timeout_add_seconds(1, header_clock_update, state);
    header_live_tick(state);
    state->live_timer = g_timeout_add(250, header_live_tick, state);
    g_object_set_data_full(G_OBJECT(header), "radios-header-state", state, header_state_free);
    g_signal_connect(header, "destroy", G_CALLBACK(header_destroyed), state);
    g_signal_connect(header, "size-allocate", G_CALLBACK(header_size_allocated), state);
    radios_header_status_bind(header, ctx);
    header_single_line(header);
    return header;
}
