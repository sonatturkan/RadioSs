#include "radios/window.h"
#include "radios/automation_calendar.h"
#include "radios/preferences.h"
#include "radios/views.h"
#include "radios/widgets.h"
#include "radios/diagnostics.h"
#include "radios/ui_preferences.h"
#include "radios/content_scale.h"
#include "radios/data.h"
#include "radios/media_ui.h"
#include "radios/media_archive_ui.h"
#include "radios/silence_guard.h"
#include "radios/midi_ui.h"
#include "radios/settings_sync.h"
#include "radios/audio_ui.h"
#include "radios/playout_ui.h"
#include "radios/advert_ui.h"
#include "radios/broadcast_ui.h"
#include "radios/broadcast_header.h"
#include "radios/recovery.h"
#include "radios/header_navigation.h"
#include "radios/player_workspace.h"
#include <fontconfig/fontconfig.h>
#include <unistd.h>

extern const gchar *const radios_reference_css_parts[];

typedef GtkWidget *(*ViewBuilder)(RadioSViewContext *);
static const struct {
    const gchar *id, *name, *widget_name;
    ViewBuilder build;
} profiles[] = {
    { "broadcast", "Yayın", "live-task-view", radios_broadcast_new },
    { "playlist", "Playlist", "playlist-task-view", radios_playlist_new },
    { "automation", "Otomasyon", "automation-task-view", radios_automation_new },
    { "dj", "DJ", "dj-task-view", radios_dj_new },
    { "emergency", "Acil", "emergency-task-view", radios_emergency_new },
    { "advert", "Reklam", "advert-task-view", radios_advert_new },
    { "technical", "Teknik yönetim", "technical-task-view", radios_technical_new },
    { "routing", "Ses yönlendirme", "routing-task-view", radios_routing_new }
};

struct RadioSWindow {
    GtkApplication *application;
    const RadioSConfig *config;
    RadioSRuntime *runtime;
    RadioSViewContext views;
    GtkWidget *window, *stack, *status, *details, *scan_button;
    GtkWidget *navigation[G_N_ELEMENTS(profiles)];
    GtkCssProvider *theme;
    gchar *css_source;
    RadioSUIPreferences preferences;
    guint startup_last_profile;
    guint explicit_profile;
    GtkWidget *text_size, *default_profile, *content_scale, *dj_page;
    GtkAdjustment *content_scale_adjustment, *mixer_scale_adjustment, *effector_scale_adjustment,
        *mixer_height_adjustment, *effector_height_adjustment;
    GdkScreen *screen;
    guint event_source, smoke_source, smoke_step;
    guint64 diagnostic_id;
    gint64 smoke_start, title_updated_at;
    gboolean diagnostic_complete, diagnostic_failed, closing, smoke_passed, selecting, preferences_saved, smoke_test;
    gboolean applying_preferences;
    gint gui_lock;
    gchar *capture_path;
};

static void
style(GtkWidget *widget, const gchar *name)
{
    gtk_style_context_add_class(gtk_widget_get_style_context(widget), name);
}

static void
notify(gpointer data, const gchar *message)
{
    RadioSWindow *ui = data;
    if (!ui->closing)
        gtk_label_set_text(GTK_LABEL(ui->status), message);
}

static guint
initial_profile(const RadioSWindow *ui)
{
    if (ui->explicit_profile)
        return ui->explicit_profile - 1;
    if (ui->preferences_saved)
        return ui->preferences.default_profile == 0 ? ui->startup_last_profile : ui->preferences.default_profile - 1;
    for (guint i = 0; i < G_N_ELEMENTS(profiles); ++i)
        if (g_str_equal(profiles[i].id, ui->config->initial_profile))
            return i;
    return 0;
}

static void
select_profile(RadioSWindow *ui, guint index)
{
    ui->selecting = TRUE;
    GtkStyleContext *context = gtk_widget_get_style_context(ui->window);
    if (index == 0)
        gtk_style_context_add_class(context, "broadcast");
    else
        gtk_style_context_remove_class(context, "broadcast");
    for (guint i = 0; i < G_N_ELEMENTS(profiles); ++i)
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui->navigation[i]), i == index);
    gtk_stack_set_visible_child_name(GTK_STACK(ui->stack), profiles[index].id);
    ui->preferences.last_profile = index;
    ui->selecting = FALSE;
}

static GtkWidget *
find_widget(GtkWidget *root, const gchar *name)
{
    if (g_str_equal(gtk_widget_get_name(root), name))
        return root;
    if (GTK_IS_MENU_BUTTON(root)) {
        GtkWidget *popover = GTK_WIDGET(gtk_menu_button_get_popover(GTK_MENU_BUTTON(root)));
        GtkWidget *found = popover ? find_widget(popover, name) : NULL;
        if (found) return found;
    }
    if (!GTK_IS_CONTAINER(root))
        return NULL;
    GtkWidget *result = NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(root));
    for (GList *item = children; item != NULL && result == NULL; item = item->next)
        result = find_widget(item->data, name);
    g_list_free(children);
    return result;
}

static gboolean
scale_font_rule(const GMatchInfo *match, GString *result, gpointer data)
{
    const gdouble *factor = data;
    gchar *size = g_match_info_fetch(match, 1);
    gchar number[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(number, sizeof(number), "%.3f", g_ascii_strtod(size, NULL) * *factor);
    g_string_append_printf(result, "font-size: %spx", number);
    g_free(size);
    return FALSE;
}

static void
text_size_changed(GtkComboBox *combo, gpointer data)
{
    RadioSWindow *ui = data;
    gint index = gtk_combo_box_get_active(combo);
    if (index < 0 || index > 2)
        return;
    const gdouble factors[] = { 0.90, 1.0, 1.15 };
    GRegex *pattern = g_regex_new("font-size\\s*:\\s*([0-9]+(?:\\.[0-9]+)?)px", 0, 0, NULL);
    gchar *scaled = g_regex_replace_eval(pattern, ui->css_source, -1, 0, 0,
                                         scale_font_rule, (gpointer)&factors[index], NULL);
    GError *error = NULL;
    if (!gtk_css_provider_load_from_data(ui->theme, scaled, -1, &error)) {
        notify(ui, error->message);
        g_clear_error(&error);
    }
    g_free(scaled);
    g_regex_unref(pattern);
}

/* Database preferences (settings_sync) arrive here: update the controls and
 * the remembered values the close-time save writes back. */
static void
window_preferences_apply(gpointer data, const RadioSUIPreferences *preferences)
{
    RadioSWindow *ui = data;
    if (ui->closing)
        return;
    ui->applying_preferences = TRUE;
    ui->preferences.text_size = preferences->text_size;
    ui->preferences.default_profile = preferences->default_profile;
    ui->preferences.header_hidden_fields = preferences->header_hidden_fields;
    ui->preferences.content_scale_percent = preferences->content_scale_percent;
    ui->preferences.mixer_scale_percent = preferences->mixer_scale_percent;
    ui->preferences.effector_scale_percent = preferences->effector_scale_percent;
    ui->preferences.mixer_height_percent = preferences->mixer_height_percent;
    ui->preferences.effector_height_percent = preferences->effector_height_percent;
    if (ui->text_size)
        gtk_combo_box_set_active(GTK_COMBO_BOX(ui->text_size), (gint)preferences->text_size);
    if (ui->default_profile)
        gtk_combo_box_set_active(GTK_COMBO_BOX(ui->default_profile), (gint)preferences->default_profile);
    gtk_adjustment_set_value(ui->content_scale_adjustment, preferences->content_scale_percent);
    gtk_adjustment_set_value(ui->mixer_scale_adjustment, preferences->mixer_scale_percent);
    gtk_adjustment_set_value(ui->effector_scale_adjustment, preferences->effector_scale_percent);
    gtk_adjustment_set_value(ui->mixer_height_adjustment, preferences->mixer_height_percent);
    gtk_adjustment_set_value(ui->effector_height_adjustment, preferences->effector_height_percent);
    radios_content_scale_set_percent(ui->content_scale, preferences->content_scale_percent);
    radios_dj_set_scale(ui->dj_page, preferences->mixer_scale_percent, preferences->mixer_height_percent,
        preferences->effector_scale_percent, preferences->effector_height_percent);
    ui->applying_preferences = FALSE;
}

static void preferences_save(GtkButton *button, gpointer data);

/* Every change is kept at once; settings_sync mirrors the file to the database. */
static void
preferences_autosave(GtkComboBox *combo, gpointer data)
{
    RadioSWindow *ui = data;
    (void)combo;
    if (!ui->closing && !ui->applying_preferences && gtk_combo_box_get_active(GTK_COMBO_BOX(ui->text_size)) >= 0 &&
        gtk_combo_box_get_active(GTK_COMBO_BOX(ui->default_profile)) >= 0)
        preferences_save(NULL, ui);
}

/* Header visibility can be saved from Preferences independently of this
 * window's font/profile form. Merge its latest value before either save. */
static gboolean
merge_header_preferences(RadioSWindow *ui, RadioSUIPreferences *preferences, GError **error)
{
    RadioSUIPreferences latest;
    if (!radios_ui_preferences_load(ui->config->project_root, &latest, error)) return FALSE;
    preferences->header_hidden_fields = latest.header_hidden_fields;
    return TRUE;
}

static void
preferences_save(GtkButton *button, gpointer data)
{
    RadioSWindow *ui = data;
    (void)button;
    if (ui->closing || ui->applying_preferences) return;
    RadioSUIPreferences proposed = ui->preferences;
    proposed.text_size = (guint)gtk_combo_box_get_active(GTK_COMBO_BOX(ui->text_size));
    proposed.default_profile = (guint)gtk_combo_box_get_active(GTK_COMBO_BOX(ui->default_profile));
    proposed.content_scale_percent = (guint)(gtk_adjustment_get_value(ui->content_scale_adjustment) + .5);
    proposed.mixer_scale_percent = (guint)(gtk_adjustment_get_value(ui->mixer_scale_adjustment) + .5);
    proposed.effector_scale_percent = (guint)(gtk_adjustment_get_value(ui->effector_scale_adjustment) + .5);
    proposed.mixer_height_percent = (guint)(gtk_adjustment_get_value(ui->mixer_height_adjustment) + .5);
    proposed.effector_height_percent = (guint)(gtk_adjustment_get_value(ui->effector_height_adjustment) + .5);
    GError *error = NULL;
    if (!merge_header_preferences(ui, &proposed, &error) ||
        !radios_ui_preferences_save(ui->config->project_root, &proposed, &error)) {
        notify(ui, error->message);
        g_clear_error(&error);
        return;
    }
    ui->preferences = proposed;
    ui->preferences_saved = TRUE;
    notify(ui, "Görünüm tercihleri kaydedildi.");
}

static void
content_scale_changed(GtkAdjustment *adjustment, gpointer data)
{
    RadioSWindow *ui = data;
    if (ui->closing) return;
    guint percent = (guint)(gtk_adjustment_get_value(adjustment) + .5);
    radios_content_scale_set_percent(ui->content_scale, percent);
    if (!ui->applying_preferences) preferences_save(NULL, ui);
}

static void
dj_scale_changed(GtkAdjustment *adjustment, gpointer data)
{
    (void)adjustment;
    RadioSWindow *ui = data;
    if (ui->closing || ui->applying_preferences) return;
    radios_dj_set_scale(ui->dj_page,
        (guint)(gtk_adjustment_get_value(ui->mixer_scale_adjustment) + .5),
        (guint)(gtk_adjustment_get_value(ui->mixer_height_adjustment) + .5),
        (guint)(gtk_adjustment_get_value(ui->effector_scale_adjustment) + .5),
        (guint)(gtk_adjustment_get_value(ui->effector_height_adjustment) + .5));
    preferences_save(NULL, ui);
}

/* Keep the ordinary available height as a useful lower bound on short
 * windows. At larger zoom the surrounding viewport scrolls the complete body
 * instead of compressing the page between its navigation and status rows. */
static void
content_scale_size(GtkWidget *widget, GtkAllocation *allocation, gpointer body)
{
    (void)widget;
    gint width, height;
    gtk_widget_get_size_request(GTK_WIDGET(body), &width, &height);
    gint minimum = MIN(360, MAX(1, allocation->height));
    if (height != minimum) gtk_widget_set_size_request(GTK_WIDGET(body), width, minimum);
}

static void
content_scale_reset(GtkButton *button, GtkAdjustment *adjustment)
{
    (void)button;
    gtk_adjustment_set_value(adjustment, 100);
}

static void
navigation_clicked(GtkButton *button, gpointer data)
{
    RadioSWindow *ui = data;
    if (!ui->selecting)
        select_profile(ui, GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "profile-index")) - 1);
}

static void appearance_combo_sync(GtkComboBox *source, GtkComboBox *target)
{
    gint selected = gtk_combo_box_get_active(source);
    if (selected < 0 || gtk_widget_in_destruction(GTK_WIDGET(source)) || gtk_widget_in_destruction(GTK_WIDGET(target))) return;
    if (selected != gtk_combo_box_get_active(target)) gtk_combo_box_set_active(target, selected);
}

static void appearance_combo_destroyed(GtkWidget *source, GtkWidget *target)
{
    g_signal_handlers_disconnect_by_data(target, source);
    g_signal_handlers_disconnect_by_data(source, target);
}

static void appearance_save_clicked(GtkButton *button, GtkButton *target)
{
    if (gtk_widget_get_visible(GTK_WIDGET(button)) && !gtk_widget_in_destruction(GTK_WIDGET(target)))
        gtk_button_clicked(target);
}

static void appearance_scale_row(GtkWidget *box, const gchar *caption, const gchar *name, GtkAdjustment *adjustment)
{
    GtkWidget *scale_title = radios_ui_label(caption, NULL);
    gtk_box_pack_start(GTK_BOX(box), scale_title, FALSE, FALSE, 0);
    GtkWidget *scale_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *scale = gtk_scale_new(GTK_ORIENTATION_HORIZONTAL, adjustment);
    gtk_widget_set_name(scale, name);
    gtk_widget_set_hexpand(scale, TRUE);
    gtk_scale_set_draw_value(GTK_SCALE(scale), FALSE);
    gtk_range_set_round_digits(GTK_RANGE(scale), 0);
    const guint marks[] = {50, 75, 100, 125, 150};
    for (guint i = 0; i < G_N_ELEMENTS(marks); ++i) {
        gchar *mark = g_strdup_printf("%u%%", marks[i]);
        gtk_scale_add_mark(GTK_SCALE(scale), marks[i], GTK_POS_BOTTOM, mark);
        g_free(mark);
    }
    gtk_box_pack_start(GTK_BOX(scale_row), scale, TRUE, TRUE, 0);
    GtkWidget *percent = gtk_spin_button_new(adjustment, 1, 0);
    gchar *percent_name = g_strdup_printf("%s-percent", name);
    gtk_widget_set_name(percent, percent_name); g_free(percent_name);
    gtk_spin_button_set_numeric(GTK_SPIN_BUTTON(percent), TRUE);
    gtk_spin_button_set_update_policy(GTK_SPIN_BUTTON(percent), GTK_UPDATE_IF_VALID);
    gtk_entry_set_width_chars(GTK_ENTRY(percent), 4);
    gtk_box_pack_start(GTK_BOX(scale_row), percent, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(scale_row), radios_ui_label("%", NULL), FALSE, FALSE, 0);
    GtkWidget *reset = gtk_button_new_with_label("%100'e dön");
    gchar *reset_name = g_strdup_printf("%s-reset", name);
    gtk_widget_set_name(reset, reset_name); g_free(reset_name);
    g_signal_connect_object(reset, "clicked", G_CALLBACK(content_scale_reset), adjustment, 0);
    gtk_box_pack_start(GTK_BOX(scale_row), reset, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), scale_row, FALSE, FALSE, 0);
}

static GtkWidget *appearance_preferences_new(RadioSViewContext *ctx, gpointer data)
{
    (void)ctx; RadioSWindow *ui = data;
    if (ui->closing || !ui->text_size || !ui->default_profile) return NULL;
    GtkWidget *save = find_widget(ui->window, "task-preferences-save");
    if (!save) return NULL;
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    appearance_scale_row(box, "Çalışma alanı ölçeği", "preferences-content-scale", ui->content_scale_adjustment);
    appearance_scale_row(box, "Mixer genişliği", "preferences-mixer-scale", ui->mixer_scale_adjustment);
    appearance_scale_row(box, "Mixer yüksekliği", "preferences-mixer-height", ui->mixer_height_adjustment);
    appearance_scale_row(box, "Efektör genişliği", "preferences-effector-scale", ui->effector_scale_adjustment);
    appearance_scale_row(box, "Efektör yüksekliği", "preferences-effector-height", ui->effector_height_adjustment);
    const gchar *const sizes[] = {"Kompakt", "Standart", "Büyük", NULL};
    const gchar *const startup[] = {"Son kullanılan profil", "Yayın", "Playlist", "Otomasyon",
        "DJ", "Acil", "Reklam", "Teknik yönetim", "Ses yönlendirme", NULL};
    const gchar *captions[] = {"Çalışma alanı yazı boyutu", "Başlangıç çalışma profili"};
    const gchar *names[] = {"preferences-text-size", "preferences-default-profile"};
    GtkWidget *native[] = {ui->text_size, ui->default_profile};
    for (guint i = 0; i < G_N_ELEMENTS(native); ++i) {
        GtkWidget *label = radios_ui_label(captions[i], NULL);
        gtk_label_set_line_wrap(GTK_LABEL(label), FALSE);
        gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0);
        GtkWidget *combo = radios_ui_combo(i ? startup : sizes, gtk_combo_box_get_active(GTK_COMBO_BOX(native[i])));
        gtk_widget_set_name(combo, names[i]);
        GList *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(combo));
        for (GList *item = cells; item; item = item->next)
            if (GTK_IS_CELL_RENDERER_TEXT(item->data))
                g_object_set(item->data, "ellipsize", PANGO_ELLIPSIZE_END, "width-chars", 20, "max-width-chars", 24, NULL);
        g_list_free(cells);
        gtk_box_pack_start(GTK_BOX(box), combo, FALSE, FALSE, 0);
        /* Both views edit the same controls; the existing live font and
         * atomic preferences save handlers remain the sole implementation. */
        g_signal_connect_object(combo, "changed", G_CALLBACK(appearance_combo_sync), native[i], 0);
        g_signal_connect_object(native[i], "changed", G_CALLBACK(appearance_combo_sync), combo, 0);
        g_signal_connect_object(combo, "destroy", G_CALLBACK(appearance_combo_destroyed), native[i], 0);
    }
    GtkWidget *apply = gtk_button_new_with_label("Görünüm tercihlerini kaydet");
    gtk_widget_set_name(apply, "preferences-appearance-save");
    gtk_widget_set_halign(apply, GTK_ALIGN_START);
    g_signal_connect_object(apply, "clicked", G_CALLBACK(appearance_save_clicked), save, 0);
    gtk_box_pack_start(GTK_BOX(box), apply, FALSE, FALSE, 0);
    return box;
}

static gboolean
navigation_key(GtkWidget *widget, GdkEventKey *event, gpointer data)
{
    RadioSWindow *ui = data;
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(widget), "profile-index")) - 1;
    switch (event->keyval) {
    case GDK_KEY_Left: index = (index + G_N_ELEMENTS(profiles) - 1) % G_N_ELEMENTS(profiles); break;
    case GDK_KEY_Right: index = (index + 1) % G_N_ELEMENTS(profiles); break;
    case GDK_KEY_Home: index = 0; break;
    case GDK_KEY_End: index = G_N_ELEMENTS(profiles) - 1; break;
    default: return FALSE;
    }
    select_profile(ui, index);
    gtk_widget_grab_focus(ui->navigation[index]);
    return TRUE;
}

static void
backup_profile(GtkButton *button, gpointer data)
{
    (void)button;
    select_profile(data, 4);
}

static void
preferences_navigate(RadioSViewContext *ctx, const gchar *category, gpointer data)
{
    (void)ctx;
    RadioSWindow *ui = data;
    const gchar *profile = g_str_equal(category, "PLAY") ? "broadcast" :
        g_str_equal(category, "Analysis") ? "playlist" :
        g_str_equal(category, "DJ System") ? "dj" : "technical";
    radios_window_select_profile(ui, profile);
    if (g_str_equal(profile, "technical")) {
        GtkWidget *stack = find_widget(ui->window, "technical-settings-stack");
        if (GTK_IS_STACK(stack)) gtk_stack_set_visible_child_name(GTK_STACK(stack), "devices");
    }
}

static void database_refresh_clicked(GtkButton *button, gpointer data)
{
    (void)button;
    RadioSWindow *ui = data;
    radios_data_connect(&ui->views);
}

static void database_context_refreshed(RadioSViewContext *ctx, GtkWidget *owner)
{
    JsonObject *context = radios_data_context(ctx);
    if (!context || !json_object_has_member(context, "station")) return;
    JsonObject *station = json_object_get_object_member(context, "station");
    GtkWidget *label = find_widget(owner, "footer-station");
    gchar *text = g_strdup_printf("İstasyon: %s", radios_json_string(station, "name", "—"));
    if (label) gtk_label_set_text(GTK_LABEL(label), text);
    g_free(text);
    label = find_widget(owner, "footer-updated");
    GDateTime *now = g_date_time_new_now_utc(); text = g_date_time_format(now, "Son güncelleme: %H:%M:%S UTC");
    if (label) gtk_label_set_text(GTK_LABEL(label), text);
    g_free(text); g_date_time_unref(now);
    label = find_widget(owner, "header-database-status");
    if (label) gtk_label_set_text(GTK_LABEL(label), "Veritabanı  Bağlı");
}

static void
scan_clicked(GtkButton *button, gpointer data)
{
    RadioSWindow *ui = data;
    GError *error = NULL;
    (void)button;
    if (ui->diagnostic_id != 0)
        return;
    ui->diagnostic_complete = FALSE;
    ui->diagnostic_failed = FALSE;
    ui->diagnostic_id = radios_runtime_submit(ui->runtime, radios_diagnostics_run,
                                              (gpointer)ui->config, NULL, &error);
    if (ui->diagnostic_id == 0) {
        ui->diagnostic_failed = TRUE;
        notify(ui, error->message);
        g_clear_error(&error);
        return;
    }
    if (ui->scan_button != NULL)
        gtk_widget_set_sensitive(ui->scan_button, FALSE);
    notify(ui, "Kaynak denetimi sıraya alındı.");
}

static void window_layout_size(GtkWidget *window, GtkAllocation *allocation, gpointer main)
{
    (void)window;
    gint margin = allocation->width < 900 ? 12 : 32;
    gtk_widget_set_margin_start(GTK_WIDGET(main), margin);
    gtk_widget_set_margin_end(GTK_WIDGET(main), margin);
}

static void footer_single_line(GtkWidget *widget)
{
    if (GTK_IS_LABEL(widget)) {
        gtk_label_set_line_wrap(GTK_LABEL(widget), FALSE);
        gtk_label_set_single_line_mode(GTK_LABEL(widget), TRUE);
        gtk_label_set_ellipsize(GTK_LABEL(widget), PANGO_ELLIPSIZE_END);
    }
    if (!GTK_IS_CONTAINER(widget)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (GList *item = children; item; item = item->next) footer_single_line(item->data);
    g_list_free(children);
}

static void window_footer_size(GtkWidget *window, GtkAllocation *allocation, gpointer footer)
{
    (void)window;
    const gchar *names[] = {"header-behavior-text", "footer-warning-status", "footer-station", "footer-updated"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        GtkWidget *widget = find_widget(footer, names[i]);
        if (!widget) continue;
        gboolean visible = allocation->width >= (i <= 1 ? 900 : 1100);
        gtk_widget_set_no_show_all(widget, !visible);
        gtk_widget_set_visible(widget, visible);
    }
    GtkStyleContext *style = gtk_widget_get_style_context(footer);
    if (allocation->width < 900) gtk_style_context_add_class(style, "footer-compact");
    else gtk_style_context_remove_class(style, "footer-compact");
}

/* Window-manager/task-switcher metadata follows accepted MAIN ownership,
 * independently of the selected profile or visible player card. Keep it
 * single-line and bounded, including malformed/truncated provider UTF-8. */
static gchar *window_title_part(const gchar *value)
{
    gchar *valid = g_utf8_make_valid(value ? value : "", -1);
    GString *part = g_string_new(NULL); guint characters = 0;
    gboolean space = FALSE;
    for (const gchar *at = valid; *at; at = g_utf8_next_char(at)) {
        gunichar character = g_utf8_get_char(at);
        if (g_unichar_isspace(character) || g_unichar_iscntrl(character)) {
            space = part->len > 0;
            continue;
        }
        if (characters == 80) { g_string_append(part, "…"); break; }
        if (space) { g_string_append_c(part, ' '); space = FALSE; }
        g_string_append_unichar(part, character); ++characters;
    }
    g_free(valid); return g_string_free(part, FALSE);
}

static void window_title_update(RadioSWindow *ui)
{
    if (ui->closing) return;
    RadioSBroadcastHeaderSnapshot playing = {0};
    radios_broadcast_header_snapshot(&ui->views, &playing);
    gchar *brand = window_title_part(ui->config->name);
    GString *title = g_string_new(*brand ? brand : "RadioS");
    gchar *programme = window_title_part(playing.programme_active ? playing.programme_title : "");
    gchar *artist = window_title_part(playing.current ? playing.artist : "");
    gchar *track = window_title_part(playing.current ? playing.title : "");
    if (*programme) g_string_append_printf(title, " · %s", programme);
    if (*artist || *track) {
        if (!*programme || g_strcmp0(programme, track) || *artist) {
            g_string_append(title, " · ");
            if (*artist) g_string_append(title, artist);
            if (*artist && *track) g_string_append(title, " – ");
            if (*track) g_string_append(title, track);
        }
    }
    const gchar *state = playing.paused ? " · Duraklatıldı" :
        playing.waiting && playing.programme_active && !playing.current ? " · Bekliyor" : "";
    /* Reserve status space so long metadata cannot hide a paused/waiting run. */
    glong available = 256 - g_utf8_strlen(state, -1);
    if (g_utf8_strlen(title->str, -1) > available) {
        g_string_truncate(title, (gsize)(g_utf8_offset_to_pointer(title->str, available - 1) - title->str));
        g_string_append(title, "…");
    }
    g_string_append(title, state);
    if (g_strcmp0(gtk_window_get_title(GTK_WINDOW(ui->window)), title->str))
        gtk_window_set_title(GTK_WINDOW(ui->window), title->str);
    g_free(brand); g_free(programme); g_free(artist); g_free(track); g_string_free(title, TRUE);
}

static gboolean
poll_events(gpointer data)
{
    RadioSWindow *ui = data;
    gint64 now = g_get_monotonic_time();
    if (now - ui->title_updated_at >= 250 * G_TIME_SPAN_MILLISECOND) {
        ui->title_updated_at = now;
        window_title_update(ui);
    }
    RadioSEvent *event;
    while ((event = radios_runtime_pop_event(ui->runtime)) != NULL) {
        if (event->task_id == ui->diagnostic_id) {
            if (event->kind == RADIO_S_EVENT_STARTED) {
                notify(ui, "Kaynaklar denetleniyor…");
            } else {
                ui->diagnostic_complete = event->kind == RADIO_S_EVENT_COMPLETED;
                ui->diagnostic_failed = !ui->diagnostic_complete;
                if (ui->diagnostic_failed)
                    g_printerr("RadioS kaynak denetimi: %s\n", event->message);
                notify(ui, event->message);
                if (ui->details != NULL)
                    gtk_label_set_text(GTK_LABEL(ui->details), event->message);
                if (ui->scan_button != NULL)
                    gtk_widget_set_sensitive(ui->scan_button, TRUE);
                ui->diagnostic_id = 0;
            }
        }
        radios_event_free(event);
    }
    return G_SOURCE_CONTINUE;
}

static void
remove_sources(RadioSWindow *ui)
{
    guint *sources[] = { &ui->event_source, &ui->smoke_source };
    for (guint i = 0; i < G_N_ELEMENTS(sources); ++i) {
        if (*sources[i] != 0) {
            g_source_remove(*sources[i]);
            *sources[i] = 0;
        }
    }
}

static void
window_destroyed(GtkWidget *widget, gpointer data)
{
    RadioSWindow *ui = data;
    (void)widget;
    ui->closing = TRUE;
    radios_player_workspace_close(ui->views.players);
    radios_playout_ui_close(ui->views.playout);
    radios_advert_ui_close(ui->views.advertisement);
    radios_broadcast_ui_close(ui->views.broadcast);
    radios_broadcast_prepare_close(&ui->views);
    radios_automation_calendar_prepare_close(find_widget(ui->window, "automation-calendar"));
    radios_dj_prepare_close(find_widget(ui->window, "dj-task-view"));
    radios_audio_ui_prepare_close(ui->views.audio);
    radios_data_close(ui->views.data);
    radios_audio_ui_close(ui->views.audio);
    if (ui->preferences_saved && !ui->smoke_test) {
        GError *error = NULL;
        if (!merge_header_preferences(ui, &ui->preferences, &error) ||
            !radios_ui_preferences_save(ui->config->project_root, &ui->preferences, &error)) {
            g_printerr("RadioS görünüm tercihleri: %s\n", error->message);
            g_clear_error(&error);
        }
    }
    remove_sources(ui);
}

static gboolean
capture_window(RadioSWindow *ui)
{
    if (ui->capture_path == NULL)
        return TRUE;
    /* A resize queued since the last frame (a profile switch, a label or a
     * meter update) leaves the toplevel with a pending allocation, which
     * gtk_widget_draw refuses. Settle the layout the way the frame clock
     * would, including a window size change that completes with the next
     * configure event, before the window is painted into the image. */
    for (guint attempt = 0; attempt < 5; ++attempt) {
        gtk_container_check_resize(GTK_CONTAINER(ui->window));
        for (guint step = 0; step < 100 && g_main_context_iteration(NULL, FALSE); ++step) {}
    }
    GtkAllocation allocation;
    gtk_widget_get_allocation(ui->window, &allocation);
    cairo_surface_t *surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, allocation.width, allocation.height);
    cairo_t *context = cairo_create(surface);
    gtk_widget_draw(ui->window, context);
    cairo_status_t result = cairo_status(context);
    if (result == CAIRO_STATUS_SUCCESS)
        result = cairo_surface_write_to_png(surface, ui->capture_path);
    cairo_destroy(context);
    cairo_surface_destroy(surface);
    if (result != CAIRO_STATUS_SUCCESS)
        g_printerr("RadioS pencere görüntüsü: %s\n", cairo_status_to_string(result));
    return result == CAIRO_STATUS_SUCCESS;
}

static gboolean
smoke_tick(gpointer data)
{
    RadioSWindow *ui = data;
    gboolean mapped = gtk_widget_get_mapped(ui->window) && gtk_widget_get_window(ui->window) != NULL;
    if (ui->diagnostic_failed) {
        ui->smoke_source = 0;
        g_application_quit(G_APPLICATION(ui->application));
        return G_SOURCE_REMOVE;
    }
    if (mapped && ui->smoke_step < G_N_ELEMENTS(profiles)) {
        guint index = ui->smoke_step;
        gtk_button_clicked(GTK_BUTTON(ui->navigation[index]));
        gboolean selected = TRUE;
        for (guint i = 0; i < G_N_ELEMENTS(profiles); ++i)
            selected = selected &&
                gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui->navigation[i])) == (i == index);
        if (selected && g_strcmp0(gtk_stack_get_visible_child_name(GTK_STACK(ui->stack)), profiles[index].id) == 0)
            ++ui->smoke_step;
    } else if (mapped && ui->diagnostic_complete && ui->smoke_step == G_N_ELEMENTS(profiles)) {
        select_profile(ui, initial_profile(ui));
        ++ui->smoke_step;
    } else if (ui->smoke_step == G_N_ELEMENTS(profiles) + 1) {
        ++ui->smoke_step; /* Allow CSS state transitions to finish before capture. */
    } else if (mapped && ui->diagnostic_complete && ui->smoke_step > G_N_ELEMENTS(profiles) + 1) {
        ui->smoke_passed = capture_window(ui);
        ui->smoke_source = 0;
        if (ui->smoke_passed)
            g_print("Native pencere kabulü geçti: mapped window, %u profil, gerçek iş/durum teslimi.\n", (guint)G_N_ELEMENTS(profiles));
        g_application_quit(G_APPLICATION(ui->application));
        return G_SOURCE_REMOVE;
    }
    if (g_get_monotonic_time() - ui->smoke_start > 10 * G_TIME_SPAN_SECOND) {
        g_printerr("Native pencere kabulü zaman aşımı: pencere/profil/iş sonucu doğrulanamadı.\n");
        ui->smoke_source = 0;
        g_application_quit(G_APPLICATION(ui->application));
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

RadioSWindow *
radios_window_new(GtkApplication *application, const RadioSConfig *config,
                  RadioSRuntime *runtime, gboolean smoke_test,
                  const gchar *capture_path, GError **error)
{
    RadioSWindow *ui = g_new0(RadioSWindow, 1);
    ui->gui_lock = -1;
    gchar *identity_file = g_build_filename(config->project_root, "config", "credentials.json", NULL);
    gboolean installed = g_file_test(identity_file, G_FILE_TEST_EXISTS); g_free(identity_file);
    if (!smoke_test && installed) {
        ui->gui_lock = radios_recovery_gui_lock(config, error);
        if (ui->gui_lock < 0) { g_free(ui); return NULL; }
    }
    if (!radios_ui_preferences_load(config->project_root, &ui->preferences, error)) {
        if (ui->gui_lock >= 0) close(ui->gui_lock);
        g_free(ui);
        return NULL;
    }
    gchar *preferences_path = g_build_filename(config->project_root, "config", "ui-preferences.ini", NULL);
    ui->preferences_saved = g_file_test(preferences_path, G_FILE_TEST_EXISTS);
    ui->startup_last_profile = ui->preferences.last_profile;
    g_free(preferences_path);
    ui->smoke_test = smoke_test;
    gchar *theme_path = g_build_filename(config->assets_root, "themes", "radios.css", NULL);
    gchar *font_path = g_build_filename(config->assets_root, "fonts", "InterVariable.woff2", NULL);
    ui->theme = gtk_css_provider_new();
    GString *css = g_string_new(NULL);
    for (guint i = 0; radios_reference_css_parts[i] != NULL; ++i)
        g_string_append(css, radios_reference_css_parts[i]);
    gchar *adaptation = NULL;
    gboolean ready = g_file_get_contents(theme_path, &adaptation, NULL, error);
    if (ready) {
        g_string_append(css, adaptation);
        ready = gtk_css_provider_load_from_data(ui->theme, css->str, css->len, error);
    }
    g_free(adaptation);
    ui->css_source = g_string_free(css, FALSE);
    if (ready && (!FcInit() || !FcConfigAppFontAddFile(NULL, (const FcChar8 *)font_path))) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Tasarımın Inter fontu yüklenemedi.");
        ready = FALSE;
    }
    g_free(theme_path);
    g_free(font_path);
    if (!ready) {
        if (ui->gui_lock >= 0) close(ui->gui_lock);
        g_object_unref(ui->theme);
        g_free(ui->css_source);
        g_free(ui);
        return NULL;
    }
    ui->application = application;
    ui->config = config;
    ui->runtime = runtime;
    ui->capture_path = g_strdup(capture_path);
    g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);
    if (smoke_test)
        g_object_set(gtk_settings_get_default(), "gtk-enable-animations", FALSE, NULL);
    ui->screen = g_object_ref(gdk_screen_get_default());
    gtk_style_context_add_provider_for_screen(ui->screen, GTK_STYLE_PROVIDER(ui->theme),
                                             GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    ui->window = gtk_application_window_new(application);
    g_object_ref_sink(ui->window);
    gtk_window_set_title(GTK_WINDOW(ui->window), config->name);
    gtk_window_set_default_size(GTK_WINDOW(ui->window), 1800, 1000);
    gtk_widget_set_size_request(ui->window, 600, 600);
    style(ui->window, "radios");
    g_signal_connect(ui->window, "destroy", G_CALLBACK(window_destroyed), ui);
    /* Views may report initialization failures through notify. */
    ui->status = radios_ui_label("", "application-status");
    ui->views = (RadioSViewContext) { .config = config, .runtime = runtime,
        .parent = GTK_WINDOW(ui->window), .notify = notify, .notify_data = ui };
    radios_ui_bind_media_drop(ui->window, &ui->views);
    gchar *credentials = g_build_filename(config->project_root, "config", "credentials.json", NULL);
    if (!smoke_test && g_file_test(credentials, G_FILE_TEST_EXISTS)) ui->views.data = radios_data_new(&ui->views);
    g_free(credentials);
    if (ui->views.data) ui->views.audio = radios_audio_ui_new(&ui->views);
    if (ui->views.audio) radios_preferences_apply_startup(&ui->views);
    if (ui->views.audio) ui->views.playout = radios_playout_ui_new(&ui->views);
    if (ui->views.data) ui->views.players = radios_player_workspace_new(&ui->views, ui->window);
    if (!smoke_test) radios_midi_ui_bind(&ui->views, ui->window);
    if (ui->views.playout) ui->views.advertisement = radios_advert_ui_new(&ui->views);
    if (ui->views.playout) ui->views.broadcast = radios_broadcast_ui_new(&ui->views);
    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *header = radios_header_new(&ui->views);
    gtk_box_pack_start(GTK_BOX(root), header, FALSE, FALSE, 0);
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_name(body, "main-content-body");
    ui->content_scale = radios_content_scale_new();
    gtk_widget_set_name(ui->content_scale, "main-content-scale");
    gtk_widget_set_vexpand(ui->content_scale, TRUE);
    radios_content_scale_set_percent(ui->content_scale, ui->preferences.content_scale_percent);
    ui->content_scale_adjustment = g_object_ref_sink(gtk_adjustment_new(ui->preferences.content_scale_percent, 50, 150, 5, 10, 0));
    ui->mixer_scale_adjustment = g_object_ref_sink(gtk_adjustment_new(ui->preferences.mixer_scale_percent, 50, 150, 5, 10, 0));
    ui->effector_scale_adjustment = g_object_ref_sink(gtk_adjustment_new(ui->preferences.effector_scale_percent, 50, 150, 5, 10, 0));
    ui->mixer_height_adjustment = g_object_ref_sink(gtk_adjustment_new(ui->preferences.mixer_height_percent, 50, 150, 5, 10, 0));
    ui->effector_height_adjustment = g_object_ref_sink(gtk_adjustment_new(ui->preferences.effector_height_percent, 50, 150, 5, 10, 0));
    GtkWidget *body_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_widget_set_name(body_scroll, "main-content-viewport");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(body_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(body_scroll), FALSE);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(body_scroll), FALSE);
    gtk_container_add(GTK_CONTAINER(body_scroll), body);
    gtk_container_add(GTK_CONTAINER(ui->content_scale), body_scroll);
    g_signal_connect_object(ui->content_scale, "size-allocate", G_CALLBACK(content_scale_size), body, 0);
    gtk_box_pack_start(GTK_BOX(root), ui->content_scale, TRUE, TRUE, 0);
    GtkWidget *main = radios_ui_box(GTK_ORIENTATION_VERTICAL, 10, "main-interface");
    gtk_widget_set_vexpand(main, TRUE);
    gtk_widget_set_margin_start(main, 32);
    gtk_widget_set_margin_end(main, 32);
    gtk_widget_set_margin_top(main, 10);
    gtk_widget_set_margin_bottom(main, 10);
    g_signal_connect_object(body, "size-allocate", G_CALLBACK(window_layout_size), main, 0);
    GtkWidget *nav = radios_ui_box(GTK_ORIENTATION_HORIZONTAL, 0, "task-profile-nav task-profile-tabs");
    gtk_widget_set_name(nav, "task-profile-nav");
    GtkWidget *nav_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(nav_scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_NEVER);
    gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(nav_scroll), FALSE);
    gtk_container_add(GTK_CONTAINER(nav_scroll), nav);
    gtk_box_pack_start(GTK_BOX(main), nav_scroll, FALSE, FALSE, 0);
    ui->stack = gtk_stack_new();
    gtk_widget_set_vexpand(ui->stack, TRUE);
    gtk_stack_set_hhomogeneous(GTK_STACK(ui->stack), FALSE);
    gtk_stack_set_vhomogeneous(GTK_STACK(ui->stack), FALSE);
    gtk_stack_set_transition_type(GTK_STACK(ui->stack), GTK_STACK_TRANSITION_TYPE_NONE);
    for (guint i = 0; i < G_N_ELEMENTS(profiles); ++i) {
        ui->navigation[i] = gtk_toggle_button_new_with_label(profiles[i].name);
        gchar *name = g_strdup_printf("profile-%s-tab", profiles[i].id);
        gtk_widget_set_name(ui->navigation[i], name);
        g_free(name);
        atk_object_set_role(gtk_widget_get_accessible(ui->navigation[i]), ATK_ROLE_PAGE_TAB);
        g_object_set_data(G_OBJECT(ui->navigation[i]), "profile-index", GUINT_TO_POINTER(i + 1));
        g_signal_connect(ui->navigation[i], "clicked", G_CALLBACK(navigation_clicked), ui);
        g_signal_connect(ui->navigation[i], "key-press-event", G_CALLBACK(navigation_key), ui);
        gtk_box_pack_start(GTK_BOX(nav), ui->navigation[i], TRUE, TRUE, 0);
        GtkWidget *page = profiles[i].build(&ui->views);
        if (g_str_equal(profiles[i].id, "dj")) {
            ui->dj_page = page;
            radios_dj_set_scale(page, ui->preferences.mixer_scale_percent, ui->preferences.mixer_height_percent,
                ui->preferences.effector_scale_percent, ui->preferences.effector_height_percent);
        }
        GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
        gtk_widget_set_name(scroll, profiles[i].widget_name);
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
        gtk_widget_set_vexpand(scroll, TRUE);
        gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(scroll), FALSE);
        gtk_scrolled_window_set_propagate_natural_width(GTK_SCROLLED_WINDOW(scroll), FALSE);
        gtk_container_add(GTK_CONTAINER(scroll), page);
        gtk_stack_add_named(GTK_STACK(ui->stack), scroll, profiles[i].id);
        if (g_str_equal(profiles[i].id, "technical")) {
            ui->scan_button = g_object_get_data(G_OBJECT(page), "scan-button");
            ui->details = g_object_get_data(G_OBJECT(page), "diagnostic-label");
            if (ui->scan_button != NULL)
                g_signal_connect(ui->scan_button, "clicked", G_CALLBACK(scan_clicked), ui);
        }
    }
    gtk_box_pack_start(GTK_BOX(main), ui->stack, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(body), main, TRUE, TRUE, 0);
    GtkWidget *footer = radios_ui_box(GTK_ORIENTATION_VERTICAL, 4, "status-footer");
    gtk_widget_set_name(footer, "status-footer");
    GtkWidget *footer_actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *footer_health = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(footer), footer_actions, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(footer), footer_health, FALSE, FALSE, 0);
    gtk_label_set_ellipsize(GTK_LABEL(ui->status), PANGO_ELLIPSIZE_END);
    gtk_label_set_line_wrap(GTK_LABEL(ui->status), FALSE);
    gtk_box_pack_start(GTK_BOX(footer_actions), radios_preferences_button_new(&ui->views), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(footer_actions), radios_broadcast_history_button_new(&ui->views), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(footer_actions), ui->status, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(footer_health), radios_header_health_new(&ui->views), TRUE, TRUE, 0);
    GtkWidget *updated = radios_ui_label("Son güncelleme: —", NULL);
    gtk_widget_set_name(updated, "footer-updated");
    gtk_box_pack_end(GTK_BOX(footer_health), updated, FALSE, FALSE, 0);
    GtkWidget *station = radios_ui_label("İstasyon: —", NULL);
    gtk_widget_set_name(station, "footer-station");
    gtk_box_pack_end(GTK_BOX(footer_health), station, FALSE, FALSE, 0);
    GtkWidget *test = radios_ui_button("Verileri yenile");
    style(test, "footer-test-button");
    gtk_widget_set_sensitive(test, ui->views.data != NULL);
    g_signal_connect(test, "clicked", G_CALLBACK(database_refresh_clicked), ui);
    gtk_box_pack_end(GTK_BOX(footer_actions), test, FALSE, FALSE, 0);
    footer_single_line(footer);
    g_signal_connect_object(body, "size-allocate", G_CALLBACK(window_footer_size), footer, 0);
    gtk_box_pack_end(GTK_BOX(body), footer, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(ui->window), root);
    GtkWidget *backup_link = find_widget(ui->window, "header-backup-details");
    if (backup_link != NULL)
        g_signal_connect(backup_link, "clicked", G_CALLBACK(backup_profile), ui);
    radios_preferences_set_navigation(GTK_WINDOW(ui->window), preferences_navigate, ui);
    ui->text_size = find_widget(ui->window, "task-text-size");
    ui->default_profile = find_widget(ui->window, "task-default-profile");
    GtkWidget *preferences_button = find_widget(ui->window, "task-preferences-save");
    if (ui->text_size != NULL && ui->default_profile != NULL && preferences_button != NULL) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(ui->text_size), (gint)ui->preferences.text_size);
        gtk_combo_box_set_active(GTK_COMBO_BOX(ui->default_profile), (gint)ui->preferences.default_profile);
        g_signal_connect(ui->text_size, "changed", G_CALLBACK(text_size_changed), ui);
        g_signal_connect(preferences_button, "clicked", G_CALLBACK(preferences_save), ui);
        text_size_changed(GTK_COMBO_BOX(ui->text_size), ui);
        g_signal_connect(ui->text_size, "changed", G_CALLBACK(preferences_autosave), ui);
        g_signal_connect(ui->default_profile, "changed", G_CALLBACK(preferences_autosave), ui);
        radios_settings_sync_set_window_hook(GTK_WINDOW(ui->window), window_preferences_apply, ui);
    }
    g_signal_connect(ui->content_scale_adjustment, "value-changed", G_CALLBACK(content_scale_changed), ui);
    g_signal_connect(ui->mixer_scale_adjustment, "value-changed", G_CALLBACK(dj_scale_changed), ui);
    g_signal_connect(ui->effector_scale_adjustment, "value-changed", G_CALLBACK(dj_scale_changed), ui);
    g_signal_connect(ui->mixer_height_adjustment, "value-changed", G_CALLBACK(dj_scale_changed), ui);
    g_signal_connect(ui->effector_height_adjustment, "value-changed", G_CALLBACK(dj_scale_changed), ui);
    radios_preferences_set_appearance(GTK_WINDOW(ui->window), appearance_preferences_new, ui);
    select_profile(ui, initial_profile(ui));
    ui->event_source = g_timeout_add(50, poll_events, ui);
    scan_clicked(NULL, ui);
    if (ui->views.data) {
        radios_data_register_refresh(&ui->views, ui->window, database_context_refreshed);
        radios_media_rhythm_library_bind(&ui->views, ui->window);
        radios_media_archive_bind(&ui->views, ui->window);
        radios_silence_guard_bind(&ui->views, ui->window);
        radios_settings_sync_bind(&ui->views, ui->window);
        radios_data_connect(&ui->views);
        radios_audio_ui_start(&ui->views);
    }
    if (smoke_test) {
        ui->smoke_start = g_get_monotonic_time();
        ui->smoke_source = g_timeout_add(180, smoke_tick, ui);
    }
    return ui;
}

void
radios_window_present(RadioSWindow *ui)
{
    gtk_widget_show_all(ui->window);
    gtk_window_present(GTK_WINDOW(ui->window));
}

gboolean
radios_window_select_profile(RadioSWindow *ui, const gchar *profile)
{
    if (!ui || !profile || ui->closing) return FALSE;
    for (guint i = 0; i < G_N_ELEMENTS(profiles); ++i) {
        if (g_strcmp0(profiles[i].id, profile) == 0) {
            ui->explicit_profile = i + 1;
            select_profile(ui, i);
            return TRUE;
        }
    }
    return FALSE;
}

RadioSViewContext *radios_window_view_context(RadioSWindow *ui)
{
    return ui ? &ui->views : NULL;
}

gboolean
radios_window_smoke_passed(const RadioSWindow *ui)
{
    return ui != NULL && ui->smoke_passed;
}

void
radios_window_free(RadioSWindow *ui)
{
    if (ui == NULL)
        return;
    remove_sources(ui);
    if (!ui->closing)
        gtk_widget_destroy(ui->window);
    radios_player_workspace_free(ui->views.players);
    ui->views.players = NULL;
    radios_broadcast_ui_close(ui->views.broadcast);
    radios_broadcast_ui_unref(ui->views.broadcast);
    ui->views.broadcast = NULL;
    radios_advert_ui_close(ui->views.advertisement);
    radios_advert_ui_unref(ui->views.advertisement);
    ui->views.advertisement = NULL;
    radios_playout_ui_close(ui->views.playout);
    radios_playout_ui_unref(ui->views.playout);
    ui->views.playout = NULL;
    radios_data_close(ui->views.data);
    radios_audio_ui_close(ui->views.audio);
    radios_audio_ui_unref(ui->views.audio);
    ui->views.audio = NULL;
    radios_data_unref(ui->views.data);
    g_signal_handlers_disconnect_by_data(ui->content_scale_adjustment, ui);
    g_clear_object(&ui->content_scale_adjustment);
    g_signal_handlers_disconnect_by_data(ui->mixer_scale_adjustment, ui);
    g_clear_object(&ui->mixer_scale_adjustment);
    g_signal_handlers_disconnect_by_data(ui->effector_scale_adjustment, ui);
    g_clear_object(&ui->effector_scale_adjustment);
    g_signal_handlers_disconnect_by_data(ui->mixer_height_adjustment, ui);
    g_clear_object(&ui->mixer_height_adjustment);
    g_signal_handlers_disconnect_by_data(ui->effector_height_adjustment, ui);
    g_clear_object(&ui->effector_height_adjustment);
    g_object_unref(ui->window);
    gtk_style_context_remove_provider_for_screen(ui->screen, GTK_STYLE_PROVIDER(ui->theme));
    g_object_unref(ui->theme);
    g_object_unref(ui->screen);
    g_free(ui->capture_path);
    g_free(ui->css_source);
    if (ui->gui_lock >= 0) close(ui->gui_lock);
    g_free(ui);
}
