#include "radios/header_status.h"
#include "radios/audio_ui.h"
#include "radios/broadcast_header.h"
#include "radios/broadcast_ui.h"
#include "radios/playout_ui.h"
#include "radios/routing_ui.h"
#include "radios/ui_preferences.h"
#include "radios/station_time.h"
#include "radios/platform.h"

#include <math.h>
#include <string.h>

enum { PROGRAMME = 0, MUSIC, REMAINING, FIELD_COUNT };
static const guint hidden_bits[FIELD_COUNT] = {
    RADIOS_HEADER_HIDE_PROGRAMME, RADIOS_HEADER_HIDE_TRACK, RADIOS_HEADER_HIDE_REMAINING
};
static const gchar *const field_names[FIELD_COUNT] = {"programme", "music", "remaining"};
static const gchar *const field_titles[FIELD_COUNT] = {"MEVCUT PROGRAM", "ÇALAN MÜZİK", "KALAN SÜRE"};
static const gchar *const setting_titles[FIELD_COUNT] = {"Mevcut programı göster", "Çalan müziği göster", "Kalan süreyi göster"};

/* Internet stream light: the worker state of every started stream (routing
 * page blocks and the Yayın page profile), never a timer or a guess. */
enum { STREAM_OFF = 0, STREAM_CONNECTING, STREAM_ON, STREAM_ERROR };
typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *header, *panel, *fields[FIELD_COUNT], *values[FIELD_COUNT];
    GtkWidget *playlist, *programme_end, *playlist_end, *track_end, *stream_light;
    GPtrArray *settings;
    GtkCssProvider *style;
    GdkScreen *screen;
    guint hidden, timer;
    gboolean changing, disposed;
    gint stream_state;
    gboolean stream_dim;
} HeaderStatus;

typedef struct {
    HeaderStatus *state;
    GtkWidget *buttons[FIELD_COUNT];
} HeaderSettings;

static GtkWidget *
find(GtkWidget *root, const gchar *name, gboolean by_class)
{
    if (by_class ? gtk_style_context_has_class(gtk_widget_get_style_context(root), name) :
                   g_str_equal(gtk_widget_get_name(root), name)) return root;
    if (GTK_IS_MENU_BUTTON(root)) {
        GtkWidget *popover = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(root)));
        GtkWidget *result = popover ? find(popover, name, by_class) : NULL;
        if (result) return result;
    }
    if (!GTK_IS_CONTAINER(root)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    GtkWidget *result = NULL;
    for (GList *child = children; child && !result; child = child->next)
        result = find(child->data, name, by_class);
    g_list_free(children);
    return result;
}

static GtkWidget *
label_new(const gchar *text, const gchar *name, const gchar *style)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_name(label, name);
    gtk_style_context_add_class(gtk_widget_get_style_context(label), style);
    return label;
}

/* The existing NEXT captions must stay readable beside the added fields.
 * Restrict this normalization to the target strip; other header sections
 * retain their own wrapping behavior and source ownership. */
static void
next_labels_normalize(GtkWidget *widget)
{
    GtkStyleContext *style = gtk_widget_get_style_context(widget);
    if (GTK_IS_LABEL(widget)) {
        gboolean caption = gtk_style_context_has_class(style, "broadcast-label");
        gboolean value = gtk_style_context_has_class(style, "header-next-title") ||
            gtk_style_context_has_class(style, "header-next-advert");
        gboolean countdown = gtk_style_context_has_class(style, "header-countdown");
        if (caption || value || countdown) {
            gtk_label_set_line_wrap(GTK_LABEL(widget), FALSE);
            gtk_label_set_single_line_mode(GTK_LABEL(widget), TRUE);
            if (value) gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_END);
        }
    }
    if (!GTK_IS_CONTAINER(widget)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (GList *child = children; child; child = child->next) next_labels_normalize(child->data);
    g_list_free(children);
}

static void
set_value(GtkWidget *label, const gchar *text)
{
    const gchar *value = text && *text ? text : "—";
    gtk_label_set_text(GTK_LABEL(label), value);
    gtk_widget_set_tooltip_text(label, g_str_equal(value, "—") ? NULL : value);
}

static void
end_time_set(HeaderStatus *state, GtkWidget *label, const gchar *caption, gint64 end_utc_us)
{
    gchar *text = NULL, *tooltip = NULL;
    if (end_utc_us > 0) {
        GTimeZone *zone = radios_time_zone_new(radios_station_timezone(state->ctx));
        if (!zone) zone = g_time_zone_new_utc();
        GDateTime *utc = g_date_time_new_from_unix_utc(end_utc_us / G_TIME_SPAN_SECOND);
        GDateTime *local = utc ? g_date_time_to_timezone(utc, zone) : NULL;
        if (local) {
            gchar *clock = g_date_time_format(local, "%H:%M:%S");
            gchar *date = radios_date_time_format(local, "%d.%m.%Y %H:%M:%S %Z");   /* tzdata abbreviation ("+03") */
            text = g_strdup_printf("%s · %s", caption, clock);
            tooltip = g_strdup_printf("%s · %s", caption, date);
            g_free(clock); g_free(date); g_date_time_unref(local);
        }
        if (utc) g_date_time_unref(utc);
        g_time_zone_unref(zone);
    }
    if (!text) text = g_strdup_printf("%s · —", caption);
    set_value(label, text);
    gtk_widget_set_tooltip_text(label, tooltip);
    g_free(text); g_free(tooltip);
}

static void
programme_set(HeaderStatus *state, gboolean active, const gchar *title, const gchar *playlist,
    gint64 programme_end_utc_us, gint64 playlist_end_utc_us)
{
    set_value(state->values[PROGRAMME], active ? title : NULL);
    gchar *text = g_strdup_printf("Playlist · %s", playlist && *playlist ? playlist : "—");
    set_value(state->playlist, text); g_free(text);
    end_time_set(state, state->programme_end, "Program bitişi", active ? programme_end_utc_us : 0);
    end_time_set(state, state->playlist_end, "Playlist bitişi", playlist_end_utc_us);
}

static void
track_end_set(HeaderStatus *state, gboolean current, gboolean paused, guint64 remaining_us)
{
    if (current && paused) {
        set_value(state->track_end, "Parça bitişi · Duraklatıldı");
        return;
    }
    gint64 now = g_get_real_time();
    gint64 end = current && remaining_us <= (guint64)(G_MAXINT64 - now) ? now + (gint64)remaining_us : 0;
    end_time_set(state, state->track_end, "Parça bitişi", current ? end : 0);
}

static void
visibility_apply(HeaderStatus *state)
{
    gboolean any = FALSE;
    for (guint field = 0; field < FIELD_COUNT; ++field) {
        gboolean visible = !(state->hidden & hidden_bits[field]);
        gtk_widget_set_no_show_all(state->fields[field], !visible);
        gtk_widget_set_visible(state->fields[field], visible);
        any |= visible;
    }
    gtk_widget_set_no_show_all(state->panel, !any);
    gtk_widget_set_visible(state->panel, any);
    state->changing = TRUE;
    for (guint i = 0; i < state->settings->len; ++i) {
        HeaderSettings *settings = g_ptr_array_index(state->settings, i);
        for (guint field = 0; field < FIELD_COUNT; ++field)
            gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(settings->buttons[field]), !(state->hidden & hidden_bits[field]));
    }
    state->changing = FALSE;
}

void
radios_header_status_apply_hidden(RadioSViewContext *ctx, guint hidden)
{
    GtkWidget *header = ctx && ctx->parent ? g_object_get_data(G_OBJECT(ctx->parent), "radios-header-status-root") : NULL;
    HeaderStatus *state = header ? g_object_get_data(G_OBJECT(header), "radios-header-status") : NULL;
    if (!state || state->disposed) return;
    state->hidden = hidden & (RADIOS_HEADER_HIDE_PROGRAMME | RADIOS_HEADER_HIDE_TRACK | RADIOS_HEADER_HIDE_REMAINING);
    visibility_apply(state);
}

static void
setting_changed(GtkToggleButton *button, gpointer data)
{
    HeaderSettings *settings = data;
    HeaderStatus *state = settings->state;
    if (!state || state->disposed || state->changing) return;
    guint field = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "header-status-field"));
    gboolean visible = gtk_toggle_button_get_active(button);
    RadioSUIPreferences latest;
    GError *error = NULL;
    const gchar *project = state->ctx && state->ctx->config ? state->ctx->config->project_root : NULL;
    gboolean saved = project && radios_ui_preferences_load(project, &latest, &error);
    if (saved) {
        latest.header_hidden_fields = visible ? latest.header_hidden_fields & ~hidden_bits[field] :
                                                latest.header_hidden_fields | hidden_bits[field];
        saved = radios_ui_preferences_save(project, &latest, &error);
    }
    if (saved) state->hidden = latest.header_hidden_fields;
    visibility_apply(state);
    if (state->ctx && state->ctx->notify)
        state->ctx->notify(state->ctx->notify_data, saved ? "Yayın çubuğu görünüm tercihi kaydedildi." :
                          error ? error->message : "Yayın çubuğu tercihi kaydedilemedi; önceki seçim korundu.");
    g_clear_error(&error);
}

static void
settings_destroyed(GtkWidget *widget, gpointer data)
{
    HeaderSettings *settings = data;
    (void)widget;
    if (settings->state) g_ptr_array_remove_fast(settings->state->settings, settings);
    g_free(settings);
}

GtkWidget *
radios_header_status_settings_new(GtkWidget *header)
{
    HeaderStatus *state = g_object_get_data(G_OBJECT(header), "radios-header-status");
    if (!state || state->disposed) return NULL;
    HeaderSettings *settings = g_new0(HeaderSettings, 1);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_name(box, "header-status-settings");
    gtk_style_context_add_class(gtk_widget_get_style_context(box), "header-status-settings");
    GtkWidget *title = label_new("Yayın çubuğu", "header-status-settings-title", "header-status-settings-title");
    gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    settings->state = state;
    for (guint field = 0; field < FIELD_COUNT; ++field) {
        GtkWidget *button = gtk_check_button_new_with_label(setting_titles[field]);
        gchar *name = g_strdup_printf("header-show-%s", field_names[field]);
        gtk_widget_set_name(button, name); g_free(name);
        settings->buttons[field] = button;
        g_object_set_data(G_OBJECT(button), "header-status-field", GUINT_TO_POINTER(field));
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(button), !(state->hidden & hidden_bits[field]));
        g_signal_connect(button, "toggled", G_CALLBACK(setting_changed), settings);
        gtk_box_pack_start(GTK_BOX(box), button, FALSE, FALSE, 0);
    }
    g_ptr_array_add(state->settings, settings);
    g_signal_connect(box, "destroy", G_CALLBACK(settings_destroyed), settings);
    return box;
}

GtkWidget *
radios_header_status_preferences_new(RadioSViewContext *ctx)
{
    if (!ctx || !ctx->parent) return NULL;
    GtkWidget *header = g_object_get_data(G_OBJECT(ctx->parent), "radios-header-status-root");
    return header ? radios_header_status_settings_new(header) : NULL;
}

static gboolean
audible(const RadioSAudioUiSnapshot *snapshot)
{
    return snapshot->main_enabled && snapshot->output_ready &&
        (snapshot->state == RADIOS_AUDIO_UI_PLAYING || snapshot->state == RADIOS_AUDIO_UI_PAUSED);
}

static gchar *
music_title(const gchar *title, const gchar *artist, gboolean paused, guint sources)
{
    gchar *name = !*title ? g_strdup("—") :
        *artist ? g_strdup_printf("%s — %s", artist, title) : g_strdup(title);
    gchar *display = g_strdup_printf("%s%s%s", name,
        paused ? " · Duraklatıldı" : "", sources > 1 ? " · Çoklu MAIN" : "");
    g_free(name); return display;
}

static void
remaining_time_set(HeaderStatus *state, guint64 remaining_us)
{
    guint64 seconds = remaining_us / G_TIME_SPAN_SECOND + (remaining_us % G_TIME_SPAN_SECOND != 0);
    gchar *remaining = g_strdup_printf("%" G_GUINT64_FORMAT " dk %02" G_GUINT64_FORMAT " sn", seconds / 60, seconds % 60);
    set_value(state->values[REMAINING], remaining); g_free(remaining);
}

static void
remaining_set(HeaderStatus *state, guint64 duration, guint64 position, gdouble tempo)
{
    if (!duration) { set_value(state->values[REMAINING], NULL); return; }
    gdouble ratio = isfinite(tempo) && tempo > 0 ? tempo : 1.0;
    gdouble milliseconds = (gdouble)(duration > position ? duration - position : 0) / ratio;
    remaining_time_set(state, (guint64)ceil(milliseconds * 1000.0));
}

static gint
stream_state_of(HeaderStatus *state, gchar *text, gsize text_size, gchar *detail, gsize detail_size)
{
    RadioSRoutingOutputSnapshot routing = {0};
    RadioSAudioOutputSnapshot legacy = {0};
    gboolean blocks = radios_ui_routing_output_snapshot(state->ctx, &routing);
    gboolean profile = radios_broadcast_ui_output_snapshot(state->ctx, "stream", &legacy);
    guint active = blocks ? routing.active : 0, pending = blocks ? routing.pending : 0, failed = blocks ? routing.failed : 0;
    guint reconnects = blocks ? routing.reconnects : 0;
    const gchar *error = blocks && routing.error[0] ? routing.error : NULL;
    if (profile) {
        if (legacy.state == RADIOS_AUDIO_OUTPUT_ACTIVE && legacy.response_confirmed) ++active;
        else if (legacy.state == RADIOS_AUDIO_OUTPUT_STARTING || legacy.state == RADIOS_AUDIO_OUTPUT_ACTIVE) ++pending;
        else if (legacy.state == RADIOS_AUDIO_OUTPUT_FAILED) { ++failed; if (!error && legacy.error[0]) error = legacy.error; }
    }
    if (failed) {
        if (reconnects) g_snprintf(text, text_size, "● Yayın hatası · %u", reconnects);
        else g_strlcpy(text, "● Yayın hatası", text_size);
        g_snprintf(detail, detail_size, "İnternet yayını: %u hata%s%s%s%s", failed,
            reconnects ? " · yeniden bağlanıyor" : "", error ? "\n" : "", error ? error : "",
            active ? "\nDiğer yayınlar bağlı." : "");
        return STREAM_ERROR;
    }
    if (pending && !active) {
        g_strlcpy(text, "● Bağlanıyor…", text_size);
        g_snprintf(detail, detail_size, "İnternet yayını: %u bağlantı sunucu yanıtı bekliyor", pending);
        return STREAM_CONNECTING;
    }
    if (active) {
        if (active > 1) g_snprintf(text, text_size, "● Yayın bağlı · %u", active);
        else g_strlcpy(text, "● Yayın bağlı", text_size);
        GString *summary = g_string_new(NULL);
        g_string_printf(summary, "İnternet yayını: %u bağlı · sunucu kabul etti", active);
        if (reconnects) g_string_append_printf(summary, " · yeniden bağlanma: %u", reconnects);
        if (pending) g_string_append_printf(summary, "\n%u bağlantı daha kuruluyor", pending);
        g_strlcpy(detail, summary->str, detail_size); g_string_free(summary, TRUE);
        return STREAM_ON;
    }
    g_strlcpy(text, "● Yayın kapalı", text_size);
    g_strlcpy(detail, blocks || profile ? "İnternet yayını başlatılmadı" : "İnternet yayını hazır değil", detail_size);
    return STREAM_OFF;
}

static void
stream_light_refresh(HeaderStatus *state)
{
    static const gchar *const classes[] = {"header-status-light-off", "header-status-light-connecting",
        "header-status-light-on", "header-status-light-error"};
    if (!state->stream_light) return;
    gchar text[64], detail[640];
    gint next = stream_state_of(state, text, sizeof text, detail, sizeof detail);
    GtkStyleContext *style = gtk_widget_get_style_context(state->stream_light);
    if (next != state->stream_state) {
        /* Colour follows real state changes only; the timer never repaints it. */
        for (guint i = 0; i < G_N_ELEMENTS(classes); ++i) gtk_style_context_remove_class(style, classes[i]);
        gtk_style_context_add_class(style, classes[next]);
        state->stream_state = next;
        if (state->stream_dim) { gtk_style_context_remove_class(style, "header-status-light-dim"); state->stream_dim = FALSE; }
    }
    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(state->stream_light)), text)) gtk_label_set_text(GTK_LABEL(state->stream_light), text);
    gchar *tooltip = gtk_widget_get_tooltip_text(state->stream_light);
    if (g_strcmp0(tooltip, detail)) gtk_widget_set_tooltip_text(state->stream_light, detail);
    g_free(tooltip);
    /* Connecting (yellow) and error/reconnecting (red) blink at 1 Hz on the
     * monotonic clock, however often the strip is refreshed. */
    gboolean blinking = next == STREAM_CONNECTING || next == STREAM_ERROR;
    gboolean dim = blinking && (g_get_monotonic_time() / (G_TIME_SPAN_SECOND / 2)) % 2 == 1;
    if (dim != state->stream_dim) {
        if (dim) gtk_style_context_add_class(style, "header-status-light-dim");
        else gtk_style_context_remove_class(style, "header-status-light-dim");
        state->stream_dim = dim;
    }
}

void
radios_header_status_refresh(GtkWidget *header)
{
    HeaderStatus *state = g_object_get_data(G_OBJECT(header), "radios-header-status");
    if (!state || state->disposed || !state->ctx) return;
    stream_light_refresh(state);
    RadioSBroadcastHeaderSnapshot broadcast = {0};
    if (radios_broadcast_header_snapshot(state->ctx, &broadcast)) {
        programme_set(state, broadcast.programme_active, broadcast.programme_title, broadcast.playlist_title,
            broadcast.programme_end_utc_us, broadcast.playlist_end_utc_us);
        if (!broadcast.current) {
            set_value(state->values[MUSIC], NULL); set_value(state->values[REMAINING], NULL);
            track_end_set(state, FALSE, FALSE, 0); return;
        }
        gchar *display = music_title(broadcast.title, broadcast.artist, broadcast.paused, broadcast.active_sources);
        set_value(state->values[MUSIC], display); g_free(display);
        remaining_set(state, broadcast.duration_ms, broadcast.position_ms, broadcast.tempo_ratio);
        guint64 remaining_us = broadcast.track_remaining_us > 0 ? (guint64)broadcast.track_remaining_us : 0;
        if (!remaining_us && broadcast.duration_ms > broadcast.position_ms) {
            gdouble ratio = isfinite(broadcast.tempo_ratio) && broadcast.tempo_ratio > 0 ? broadcast.tempo_ratio : 1.0;
            remaining_us = (guint64)ceil((broadcast.duration_ms - broadcast.position_ms) * 1000.0 / ratio);
        }
        if (broadcast.track_remaining_us > 0) remaining_time_set(state, (guint64)broadcast.track_remaining_us);
        track_end_set(state, broadcast.duration_ms > 0 && !broadcast.waiting, broadcast.paused && !broadcast.waiting, remaining_us);
        return;
    }
    RadioSPlayoutHeaderSnapshot programme = {0};
    radios_playout_ui_header_snapshot(state->ctx, &programme);
    programme_set(state, programme.programme_active, programme.programme_title, programme.playlist_title,
        programme.programme_end_utc_us, programme.playlist_end_utc_us);

    RadioSPlayoutSnapshot playout = {0};
    gboolean planned = radios_playout_ui_get_snapshot(state->ctx, &playout);
    RadioSAudioUiSnapshot selected = {0};
    gint deck = -1;
    guint playing_count = 0;
    for (guint candidate = 0; candidate < RADIOS_AUDIO_NORMAL_DECKS; ++candidate) {
        RadioSAudioUiSnapshot actual = {0};
        if (!radios_audio_ui_get_snapshot(state->ctx, candidate, &actual) || !audible(&actual)) continue;
        if (actual.state == RADIOS_AUDIO_UI_PLAYING) ++playing_count;
        gboolean preferred = planned && playout.deck == (gint)candidate;
        if (deck < 0 || (selected.state == RADIOS_AUDIO_UI_PAUSED && actual.state == RADIOS_AUDIO_UI_PLAYING) ||
            (selected.state == actual.state && preferred)) { selected = actual; deck = (gint)candidate; }
    }
    if (deck < 0) {
        set_value(state->values[MUSIC], NULL);
        set_value(state->values[REMAINING], NULL);
        track_end_set(state, FALSE, FALSE, 0); return;
    }
    RadioSPlayoutDeckMetadata metadata = {0};
    gboolean owned = radios_playout_ui_deck_metadata(state->ctx, (guint)deck, &metadata);
    gchar *display = music_title(metadata.title[0] ? metadata.title : selected.title,
        metadata.artist[0] ? metadata.artist : selected.artist, selected.state == RADIOS_AUDIO_UI_PAUSED, playing_count);
    set_value(state->values[MUSIC], display); g_free(display);
    guint64 duration = selected.duration_ms;
    guint64 position = selected.position_ms;
    if (owned && planned && playout.deck == deck && playout.duration_ms &&
        (playout.state == RADIOS_PLAYOUT_PLAYING || playout.state == RADIOS_PLAYOUT_OVERLAP || playout.state == RADIOS_PLAYOUT_PAUSED)) {
        duration = playout.duration_ms; position = playout.position_ms;
    }
    if (selected.cue_out_ms) duration = duration ? MIN(duration, selected.cue_out_ms) : selected.cue_out_ms;
    remaining_set(state, duration, position, selected.tempo_ratio);
    gdouble ratio = isfinite(selected.tempo_ratio) && selected.tempo_ratio > 0 ? selected.tempo_ratio : 1.0;
    guint64 remaining_us = (guint64)ceil((duration > position ? duration - position : 0) * 1000.0 / ratio);
    track_end_set(state, duration > 0, selected.state == RADIOS_AUDIO_UI_PAUSED, remaining_us);
}

static gboolean
tick(gpointer data)
{
    HeaderStatus *state = data;
    if (state->disposed) return G_SOURCE_REMOVE;
    radios_header_status_refresh(state->header);
    return G_SOURCE_CONTINUE;
}

static void
destroyed(GtkWidget *widget, gpointer data)
{
    HeaderStatus *state = data;
    (void)widget;
    state->disposed = TRUE;
    if (state->ctx && state->ctx->parent &&
        g_object_get_data(G_OBJECT(state->ctx->parent), "radios-header-status-root") == state->header)
        g_object_set_data(G_OBJECT(state->ctx->parent), "radios-header-status-root", NULL);
    if (state->timer) { g_source_remove(state->timer); state->timer = 0; }
    for (guint i = 0; i < state->settings->len; ++i) {
        HeaderSettings *settings = g_ptr_array_index(state->settings, i);
        settings->state = NULL;
        for (guint field = 0; field < FIELD_COUNT; ++field) gtk_widget_set_sensitive(settings->buttons[field], FALSE);
    }
    g_ptr_array_set_size(state->settings, 0);
    if (state->style && state->screen)
        gtk_style_context_remove_provider_for_screen(state->screen, GTK_STYLE_PROVIDER(state->style));
}

static void
state_free(gpointer data)
{
    HeaderStatus *state = data;
    g_clear_object(&state->style);
    g_clear_object(&state->screen);
    g_ptr_array_unref(state->settings);
    g_free(state);
}

void
radios_header_status_bind(GtkWidget *header, RadioSViewContext *ctx)
{
    g_return_if_fail(GTK_IS_WIDGET(header));
    if (g_object_get_data(G_OBJECT(header), "radios-header-status")) return;
    GtkWidget *next = find(header, "header-next", TRUE);
    if (!GTK_IS_BOX(next)) return;
    /* The measured no-wrap strip needs 50 px less spacing to retain the
     * existing native minimum width while keeping its captions complete. */
    gtk_style_context_add_class(gtk_widget_get_style_context(next), "header-status-bound");
    gtk_box_set_spacing(GTK_BOX(next), 8);
    next_labels_normalize(next);
    /* The old clock-side programme block contains static placeholders.
     * Move its role to the accepted live programme field in the target strip,
     * so visibility preferences have one consistent programme display. */
    GtkWidget *legacy_programme = find(header, "header-programme", TRUE);
    if (legacy_programme) {
        gtk_widget_set_no_show_all(legacy_programme, TRUE);
        gtk_widget_hide(legacy_programme);
    }
    HeaderStatus *state = g_new0(HeaderStatus, 1);
    state->ctx = ctx; state->header = header; state->settings = g_ptr_array_new();
    state->panel = gtk_box_new(GTK_ORIENTATION_VERTICAL, 7);
    gtk_widget_set_name(state->panel, "header-status-panel");
    gtk_style_context_add_class(gtk_widget_get_style_context(state->panel), "header-status-panel");
    for (guint field = 0; field < FIELD_COUNT; ++field) {
        gchar *name = g_strdup_printf("header-status-%s", field_names[field]);
        state->fields[field] = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_name(state->fields[field], name); g_free(name);
        GtkWidget *title = label_new(field_titles[field], "header-status-caption", "header-status-caption");
        /* Keep large-font minimum requests bounded by whole words. With
         * sufficient space these captions retain their single-line layout. */
        gtk_label_set_line_wrap(GTK_LABEL(title), TRUE);
        gtk_label_set_line_wrap_mode(GTK_LABEL(title), PANGO_WRAP_WORD);
        name = g_strdup_printf("header-status-%s-value", field_names[field]);
        state->values[field] = label_new("—", name, field == REMAINING ? "header-status-remaining-value" : "header-status-value");
        g_free(name);
        gtk_label_set_ellipsize(GTK_LABEL(state->values[field]), PANGO_ELLIPSIZE_END);
        gtk_label_set_max_width_chars(GTK_LABEL(state->values[field]), 30);
        gtk_box_pack_start(GTK_BOX(state->fields[field]), title, FALSE, FALSE, 0);
        GtkWidget *value_row = state->values[field];
        if (field == REMAINING) {
            /* The Internet stream light shares the remaining-time row: the
             * strip keeps its height, the caption stays the field's first
             * child and details live in the tooltip. */
            value_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
            gtk_widget_set_name(value_row, "header-status-remaining-row");
            gtk_box_pack_start(GTK_BOX(value_row), state->values[field], TRUE, TRUE, 0);
            state->stream_light = label_new("● Yayın kapalı", "header-status-stream-light", "header-status-light");
            gtk_style_context_add_class(gtk_widget_get_style_context(state->stream_light), "header-status-light-off");
            gtk_label_set_xalign(GTK_LABEL(state->stream_light), 1);
            gtk_label_set_single_line_mode(GTK_LABEL(state->stream_light), TRUE);
            gtk_label_set_ellipsize(GTK_LABEL(state->stream_light), PANGO_ELLIPSIZE_END);
            gtk_label_set_max_width_chars(GTK_LABEL(state->stream_light), 16);
            gtk_widget_set_tooltip_text(state->stream_light, "İnternet yayını hazır değil");
            gtk_box_pack_end(GTK_BOX(value_row), state->stream_light, FALSE, FALSE, 0);
        }
        gtk_box_pack_start(GTK_BOX(state->fields[field]), value_row, FALSE, FALSE, 0);
        if (field == PROGRAMME || field == MUSIC) {
            const gchar *names[] = {"header-status-playlist-value", "header-status-programme-end-value",
                "header-status-playlist-end-value", "header-status-track-end-value"};
            const gchar *initial[] = {"Playlist · —", "Program bitişi · —", "Playlist bitişi · —", "Parça bitişi · —"};
            GtkWidget **details[] = {&state->playlist, &state->programme_end, &state->playlist_end, &state->track_end};
            for (guint i = field == PROGRAMME ? 0 : 3; i < (field == PROGRAMME ? 3 : 4); ++i) {
                *details[i] = label_new(initial[i], names[i], "header-status-detail");
                if (i == 0) gtk_label_set_ellipsize(GTK_LABEL(*details[i]), PANGO_ELLIPSIZE_END);
                else {
                    gtk_label_set_line_wrap(GTK_LABEL(*details[i]), TRUE);
                    gtk_label_set_line_wrap_mode(GTK_LABEL(*details[i]), PANGO_WRAP_WORD);
                }
                gtk_label_set_max_width_chars(GTK_LABEL(*details[i]), 30);
                gtk_box_pack_start(GTK_BOX(state->fields[field]), *details[i], FALSE, FALSE, 0);
            }
        }
        gtk_box_pack_start(GTK_BOX(state->panel), state->fields[field], FALSE, FALSE, 0);
    }
    gtk_box_pack_start(GTK_BOX(next), state->panel, TRUE, TRUE, 0);
    g_object_set_data_full(G_OBJECT(header), "radios-header-status", state, state_free);
    if (ctx && ctx->parent)
        g_object_set_data(G_OBJECT(ctx->parent), "radios-header-status-root", header);
    g_signal_connect(header, "destroy", G_CALLBACK(destroyed), state);
    RadioSUIPreferences preferences;
    GError *error = NULL;
    if (ctx && ctx->config && ctx->config->project_root &&
        radios_ui_preferences_load(ctx->config->project_root, &preferences, &error)) state->hidden = preferences.header_hidden_fields;
    else if (error && ctx && ctx->notify) ctx->notify(ctx->notify_data, error->message);
    g_clear_error(&error);
    if (ctx && ctx->config && ctx->config->assets_root) {
        gchar *path = g_build_filename(ctx->config->assets_root, "themes", "header-status.css", NULL);
        state->style = gtk_css_provider_new();
        if (gtk_css_provider_load_from_path(state->style, path, &error)) {
            state->screen = g_object_ref(gtk_widget_get_screen(header));
            gtk_style_context_add_provider_for_screen(state->screen, GTK_STYLE_PROVIDER(state->style), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
        } else if (ctx->notify) ctx->notify(ctx->notify_data, error->message);
        g_clear_error(&error); g_free(path);
    }
    {
        /* Stream light colours: off grey, connecting yellow, connected green,
         * error red; "dim" is the off phase of the 1 Hz blink. */
        static gsize installed = 0;
        if (g_once_init_enter(&installed)) {
            GtkCssProvider *lights = gtk_css_provider_new();
            gtk_css_provider_load_from_data(lights,
                ".header-status-light { font-size: 0.85em; }"
                ".header-status-light-off { color: #5b6672; }"
                ".header-status-light-connecting { color: #f2c230; }"
                ".header-status-light-on { color: #3ddc84; }"
                ".header-status-light-error { color: #ff5252; }"
                ".header-status-light-dim { opacity: 0.3; }", -1, NULL);
            gtk_style_context_add_provider_for_screen(gtk_widget_get_screen(header), GTK_STYLE_PROVIDER(lights), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
            g_object_unref(lights);
            g_once_init_leave(&installed, 1);
        }
    }
    GtkWidget *popover = find(header, "header-settings-menu", FALSE);
    GtkWidget *items = popover && GTK_IS_BIN(popover) ? gtk_bin_get_child(GTK_BIN(popover)) : NULL;
    if (GTK_IS_BOX(items)) {
        GtkWidget *settings = radios_header_status_settings_new(header);
        gtk_box_pack_start(GTK_BOX(items), settings, FALSE, FALSE, 0);
        gtk_widget_show_all(settings);
    }
    gtk_widget_show_all(state->panel);
    visibility_apply(state);
    radios_header_status_refresh(header);
    state->timer = g_timeout_add(250, tick, state);
}
