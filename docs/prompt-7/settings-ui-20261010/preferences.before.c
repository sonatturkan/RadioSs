#include "radios/preferences.h"
#include "radios/preferences_storage.h"
#include "radios/audio.h"
#include "radios/audio_ui.h"
#include "radios/header_status.h"
#include "radios/online_services_ui.h"
#include "radios/widgets.h"
#include <math.h>
#include <string.h>

#define WINDOW_KEY "radios-preferences-window"
#define NAVIGATION_KEY "radios-preferences-navigation"

typedef struct {
    RadioSPreferencesNavigateFunc callback;
    gpointer data;
} PreferencesNavigation;

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *window, *categories, *search, *content, *notebook;
    GtkWidget *device, *sample_rate, *buffer, *buffer_label, *status;
    GtkWidget *sounds[3], *volumes[3], *routing, *outputs[4], *apply;
    GtkCssProvider *style;
    GdkScreen *screen;
    RadioSAudioEngine *engine;
    guint poll_id, device_hash;
    gchar *device_selection;
    gboolean updating, storage_error;
} Preferences;

typedef struct {
    RadioSAudioEngine *engine;
    GtkWidget *details;
    guint timer;
} DriverDialog;

static const gchar *const category_names[] = {
    "PLAY", "CLOUD", "View", "Audio", "Analysis", "DJ System", "Keyboard", "Advanced"
};
static const gchar *const category_ids[] = {
    "play", "cloud", "view", "audio", "analysis", "dj-system", "keyboard", "advanced"
};

static const gchar css[] =
    "#preferences-window, #preferences-window *, #preferences-driver-dialog, #preferences-driver-dialog * {"
    " color: #151515; background-color: #fff; background-image: none; text-shadow: none; box-shadow: none; }"
    "#preferences-window { border: 1px solid #242424; font-family: Sans; font-size: 16px; }"
    "#preferences-window label { background-color: transparent; }"
    "#preferences-window .preferences-titlebar { border-bottom: 1px solid #333; padding: 3px 6px; }"
    "#preferences-window .preferences-sidebar { border-right: 1px solid #333; padding: 16px 10px; }"
    "#preferences-window entry { min-height: 30px; border: 1px solid #333; border-radius: 3px; padding: 3px 8px; }"
    "#preferences-window button, #preferences-window combobox button { border: 1px solid #333; border-radius: 3px; padding: 5px 10px; min-height: 22px; }"
    "#preferences-window button:hover { background-color: #f0f0f0; }"
    "#preferences-window button:disabled { color: #747474; border-color: #999; }"
    "#preferences-window #preferences-close { min-width: 22px; min-height: 22px; padding: 0; font-size: 22px; }"
    "#preferences-window #preferences-categories { padding-top: 20px; }"
    "#preferences-window #preferences-categories row { min-height: 40px; padding: 4px 8px; border: 1px solid transparent; }"
    "#preferences-window #preferences-categories row:selected { color: #151515; background-color: #fff; border: 1px solid #333; border-radius: 2px; }"
    "#preferences-window .preferences-section { padding: 24px 30px; border-bottom: 1px solid #333; }"
    "#preferences-window .preferences-heading { font-size: 23px; font-weight: bold; margin-bottom: 18px; }"
    "#preferences-window .preferences-volume { border-top: 1px solid #333; margin-top: 18px; padding-top: 18px; }"
    "#preferences-window notebook { border: 0; }"
    "#preferences-window notebook > header { border-bottom: 1px solid #333; padding-top: 4px; }"
    "#preferences-window notebook > header tabs tab { border: 1px solid #333; border-bottom: 0; border-radius: 3px 3px 0 0; padding: 8px 5px; margin-right: 3px; }"
    "#preferences-window notebook > header tabs tab:checked { background-color: #fff; }"
    "#preferences-window scale { padding: 16px 4px 4px; }"
    "#preferences-window scale trough { min-height: 3px; border: 1px solid #333; border-radius: 3px; }"
    "#preferences-window scale highlight, #preferences-window scale:focus highlight { border-color: #333; background-color: #fff; background-image: none; }"
    "#preferences-window scale:focus trough { border-color: #333; outline-style: none; }"
    "#preferences-window scale slider { min-width: 8px; min-height: 24px; border: 1px solid #333; border-radius: 3px; background-color: #fff; }"
    "#preferences-window radio { min-width: 17px; min-height: 17px; margin-right: 6px; color: #151515; border: 1px solid #333; border-radius: 50%; background-color: #fff; }"
    "#preferences-window radio:checked { background-image: -gtk-icontheme('media-record-symbolic'); }"
    "#preferences-window checkbutton, #preferences-window radiobutton { min-height: 26px; padding: 2px 0; }"
    "#preferences-window .preferences-footer { padding: 16px 12px 5px; }"
    "#preferences-window #preferences-reset { min-width: 265px; }"
    "#preferences-window #preferences-status { font-size: 12px; color: #555; }";

static void style(GtkWidget *widget, const gchar *name)
{ gtk_style_context_add_class(gtk_widget_get_style_context(widget), name); }

static GtkWidget *named(GtkWidget *widget, const gchar *name)
{ gtk_widget_set_name(widget, name); return widget; }

static GtkWidget *label_new(const gchar *text)
{
    GtkWidget *label = gtk_label_new(text);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    return label;
}

static void pack(GtkWidget *box, GtkWidget *child, gboolean expand)
{ gtk_box_pack_start(GTK_BOX(box), child, expand, expand, 0); }

static void status_set(Preferences *prefs, const gchar *message)
{
    gtk_label_set_text(GTK_LABEL(prefs->status), message);
    if (prefs->ctx->notify) prefs->ctx->notify(prefs->ctx->notify_data, message);
}

static gboolean icon_draw(GtkWidget *widget, cairo_t *cr, gpointer value)
{
    (void)widget;
    guint index = GPOINTER_TO_UINT(value);
    cairo_set_source_rgb(cr, 0.1, 0.1, 0.1);
    cairo_set_line_width(cr, 1.6);
    if (index == 0) {
        cairo_arc(cr, 13, 14, 11, 0, 2 * G_PI); cairo_stroke(cr);
        cairo_move_to(cr, 10, 9); cairo_line_to(cr, 17, 14); cairo_line_to(cr, 10, 19); cairo_close_path(cr); cairo_stroke(cr);
        cairo_move_to(cr, 7, 10); cairo_line_to(cr, 7, 18); cairo_stroke(cr);
    } else if (index == 1) {
        cairo_move_to(cr, 6, 22); cairo_curve_to(cr, -1, 21, 0, 12, 7, 12);
        cairo_curve_to(cr, 7, 2, 22, 2, 22, 13); cairo_curve_to(cr, 29, 13, 29, 22, 22, 22);
        cairo_close_path(cr); cairo_stroke(cr);
        cairo_move_to(cr, 14, 10); cairo_line_to(cr, 14, 19); cairo_move_to(cr, 10, 14); cairo_line_to(cr, 18, 14); cairo_stroke(cr);
    } else if (index == 2) {
        cairo_move_to(cr, 1, 14); cairo_curve_to(cr, 8, 2, 20, 2, 27, 14);
        cairo_curve_to(cr, 20, 26, 8, 26, 1, 14); cairo_stroke(cr);
        cairo_arc(cr, 14, 14, 6, 0, 2 * G_PI); cairo_stroke(cr);
    } else if (index == 3) {
        cairo_move_to(cr, 4, 3); cairo_line_to(cr, 24, 14); cairo_line_to(cr, 4, 25); cairo_close_path(cr); cairo_stroke(cr);
    } else if (index == 4) {
        const guint heights[] = {10, 18, 26, 16, 22, 10};
        for (guint i = 0; i < G_N_ELEMENTS(heights); ++i) {
            cairo_move_to(cr, 3 + i * 4, 14 - heights[i] / 2.0); cairo_line_to(cr, 3 + i * 4, 14 + heights[i] / 2.0);
        }
        cairo_stroke(cr);
    } else if (index == 5 || index == 6) {
        cairo_rectangle(cr, 2, 3, 24, 24); cairo_stroke(cr);
        cairo_select_font_face(cr, "Sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
        cairo_set_font_size(cr, index == 5 ? 14 : 20);
        cairo_move_to(cr, index == 5 ? 4 : 7, 22); cairo_show_text(cr, index == 5 ? "DJ" : "A");
    } else {
        for (guint i = 0; i < 3; ++i) { cairo_move_to(cr, 3, 6 + i * 7); cairo_line_to(cr, 25, 6 + i * 7); }
        cairo_stroke(cr);
    }
    return FALSE;
}

static GtkWidget *section_new(const gchar *title)
{
    GtkWidget *section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    style(section, "preferences-section");
    GtkWidget *heading = label_new(title); style(heading, "preferences-heading");
    pack(section, heading, FALSE);
    return section;
}

static guint current_rate(Preferences *prefs)
{
    const gchar *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(prefs->sample_rate));
    return id ? (guint)g_ascii_strtoull(id, NULL, 10) : 44100;
}

static guint current_buffer(Preferences *prefs)
{ return 64u << (guint)gtk_range_get_value(GTK_RANGE(prefs->buffer)); }

static void buffer_label_update(Preferences *prefs)
{
    guint frames = current_buffer(prefs), rate = current_rate(prefs);
    gchar latency[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(latency, sizeof latency, "%.1f", 1000.0 * frames / rate);
    gchar *text = g_strdup_printf("%u samples (%s ms)", frames, latency);
    gtk_label_set_text(GTK_LABEL(prefs->buffer_label), text); g_free(text);
}

static gboolean audio_options_save(Preferences *prefs, gboolean apply_output)
{
    RadioSPreferencesAudio options;
    radios_preferences_audio_default(&options);
    options.sample_rate = current_rate(prefs);
    options.buffer_frames = current_buffer(prefs);
    for (guint i = 0; i < 3; ++i) {
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(prefs->sounds[i]))) options.click_sound = i;
        if (gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(prefs->volumes[i]))) options.click_volume = i;
    }
    GError *error = NULL;
    if (apply_output && prefs->engine && !radios_audio_engine_configure_output(prefs->engine,
        options.sample_rate, options.buffer_frames, &error)) {
        gchar *message = g_strdup_printf("Çıkış ayarı uygulanamadı: %s", error ? error->message : "Ses motoru hazır değil.");
        prefs->storage_error = TRUE; status_set(prefs, message); g_free(message); g_clear_error(&error); return FALSE;
    }
    if (!prefs->ctx->config || !prefs->ctx->config->project_root ||
        !radios_preferences_audio_save(prefs->ctx->config->project_root, &options, &error)) {
        gchar *message = g_strdup_printf("%sTercih kaydedilemedi: %s",
            prefs->engine && apply_output ? "Çıkış isteği kabul edildi. " : "",
            error ? error->message : "Proje ayar dizini bulunamadı.");
        prefs->storage_error = TRUE; status_set(prefs, message); g_free(message); g_clear_error(&error); return FALSE;
    }
    prefs->storage_error = FALSE;
    status_set(prefs, prefs->engine && apply_output ? "Çıkış isteği kabul edildi ve tercih kaydedildi; aygıt yanıtı bekleniyor." : "Ses tercihi kaydedildi.");
    return TRUE;
}

static void audio_options_set(Preferences *prefs, const RadioSPreferencesAudio *options)
{
    prefs->updating = TRUE;
    gchar rate[16]; g_snprintf(rate, sizeof rate, "%u", options->sample_rate);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(prefs->sample_rate), rate);
    guint index = 0;
    for (guint frames = 64; frames < options->buffer_frames; frames <<= 1) ++index;
    gtk_range_set_value(GTK_RANGE(prefs->buffer), index);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(prefs->sounds[options->click_sound]), TRUE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(prefs->volumes[options->click_volume]), TRUE);
    prefs->updating = FALSE; buffer_label_update(prefs);
}

static void audio_option_changed(GtkWidget *widget, gpointer value)
{
    (void)widget; Preferences *prefs = value;
    buffer_label_update(prefs);
    if (!prefs->updating) audio_options_save(prefs, widget == prefs->sample_rate || widget == prefs->buffer);
}

static void radio_changed(GtkToggleButton *button, gpointer value)
{
    if (gtk_toggle_button_get_active(button)) audio_option_changed(GTK_WIDGET(button), value);
}

static void routing_main_changed(GtkComboBox *combo, gpointer value)
{
    Preferences *prefs = value;
    const gchar *id = gtk_combo_box_get_active_id(combo);
    if (!id || prefs->updating) return;
    prefs->updating = TRUE;
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(prefs->device), id);
    prefs->updating = FALSE;
}

static gboolean devices_update(gpointer value)
{
    Preferences *prefs = value;
    if (!prefs->engine) return G_SOURCE_CONTINUE;
    RadioSAudioSnapshot snapshot; radios_audio_engine_snapshot(prefs->engine, &snapshot);
    if (!prefs->storage_error) {
        const RadioSAudioBusSnapshot *bus = &snapshot.buses[RADIOS_AUDIO_BUS_MAIN];
        guint rate, frames; radios_audio_engine_output_configuration(prefs->engine, &rate, &frames);
        gchar *message = NULL;
        if (*bus->error) message = g_strdup_printf("Çıkış aygıtı: %s", bus->error);
        else if (bus->ready && bus->output_sample_rate)
            message = g_strdup_printf("Ses çıkışı: %u Hz · %u samples (istenen %u)", bus->output_sample_rate, bus->output_buffer_frames, frames);
        else {
            message = g_strdup_printf("Çıkış isteği: %u Hz · %u samples. Aygıt bağlantısı bekleniyor.", rate, frames);
        }
        if (g_strcmp0(gtk_label_get_text(GTK_LABEL(prefs->status)), message)) status_set(prefs, message);
        g_free(message);
    }
    const gchar *selected = gtk_combo_box_get_active_id(GTK_COMBO_BOX(prefs->outputs[0]));
    gchar *copy = g_strdup(selected ? selected : "@default");
    guint hash = 5381u ^ snapshot.connected;
    for (guint i = 0; i < snapshot.output_count; ++i)
        hash = (hash * 33u) ^ g_str_hash(snapshot.outputs[i].name) ^ g_str_hash(snapshot.outputs[i].description);
    if (hash == prefs->device_hash && g_strcmp0(copy, prefs->device_selection) == 0) { g_free(copy); return G_SOURCE_CONTINUE; }
    prefs->device_hash = hash; g_free(prefs->device_selection); prefs->device_selection = g_strdup(copy);
    prefs->updating = TRUE;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(prefs->device));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(prefs->device), "", "Atanmadı");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(prefs->device), "@default", "Sistem varsayılan çıkışı");
    gboolean found = !*copy || g_str_equal(copy, "@default");
    for (guint i = 0; i < snapshot.output_count; ++i) {
        const RadioSAudioDevice *device = &snapshot.outputs[i];
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(prefs->device), device->name,
            device->description[0] ? device->description : device->name);
        if (g_str_equal(copy, device->name)) found = TRUE;
    }
    if (!found) {
        gchar *text = g_strdup_printf("Bağlı değil · %s", copy);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(prefs->device), copy, text); g_free(text);
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(prefs->device), copy);
    prefs->updating = FALSE; g_free(copy);
    return G_SOURCE_CONTINUE;
}

static void device_changed(GtkComboBox *combo, gpointer value)
{
    Preferences *prefs = value;
    const gchar *id = gtk_combo_box_get_active_id(combo);
    if (!id || prefs->updating) return;
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(prefs->outputs[0]), id);
    gtk_button_clicked(GTK_BUTTON(prefs->apply));
}

static gboolean driver_details_update(gpointer value)
{
    DriverDialog *driver = value;
    RadioSAudioSnapshot snapshot = {0};
    if (driver->engine) radios_audio_engine_snapshot(driver->engine, &snapshot);
    GString *info = g_string_new(snapshot.connected ? "Ses sunucusu bağlı.\n" : "Ses sunucusu bağlı değil.\n");
    const RadioSAudioBusSnapshot *bus = &snapshot.buses[RADIOS_AUDIO_BUS_MAIN];
    if (bus->ready) g_string_append_printf(info, "Çıkış: %u Hz · %u samples\n", bus->output_sample_rate, bus->output_buffer_frames);
    for (guint i = 0; i < snapshot.output_count; ++i)
        g_string_append_printf(info, "\n%s", snapshot.outputs[i].description[0] ? snapshot.outputs[i].description : snapshot.outputs[i].name);
    if (*snapshot.error) g_string_append_printf(info, "\n%s", snapshot.error);
    if (g_strcmp0(gtk_label_get_text(GTK_LABEL(driver->details)), info->str)) gtk_label_set_text(GTK_LABEL(driver->details), info->str);
    g_string_free(info, TRUE); return G_SOURCE_CONTINUE;
}

static void driver_refresh(GtkButton *button, gpointer value)
{
    (void)button; DriverDialog *driver = value;
    if (driver->engine) radios_audio_engine_refresh_devices(driver->engine);
    driver_details_update(driver);
}

static void driver_destroyed(GtkWidget *widget, gpointer value)
{
    (void)widget; DriverDialog *driver = value;
    if (driver->timer) g_source_remove(driver->timer);
    if (driver->engine) radios_audio_engine_unref(driver->engine);
    g_free(driver);
}

static void driver_clicked(GtkButton *button, gpointer value)
{
    (void)button; Preferences *prefs = value;
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Driver", GTK_WINDOW(prefs->window),
        GTK_DIALOG_DESTROY_WITH_PARENT, "Close", GTK_RESPONSE_CLOSE, NULL);
    gtk_widget_set_name(dialog, "preferences-driver-dialog");
    gtk_window_set_default_size(GTK_WINDOW(dialog), 520, 260);
    GtkWidget *body = gtk_dialog_get_content_area(GTK_DIALOG(dialog));
    gtk_container_set_border_width(GTK_CONTAINER(body), 16);
    GtkWidget *text = label_new("PulseAudio / PipeWire"); pack(body, text, FALSE);
    DriverDialog *driver = g_new0(DriverDialog, 1);
    driver->engine = prefs->engine ? radios_audio_engine_ref(prefs->engine) : NULL;
    driver->details = named(label_new(""), "preferences-driver-details"); gtk_label_set_line_wrap(GTK_LABEL(driver->details), TRUE);
    gtk_label_set_selectable(GTK_LABEL(driver->details), TRUE); pack(body, driver->details, TRUE);
    driver_details_update(driver);
    driver->timer = g_timeout_add(500, driver_details_update, driver);
    GtkWidget *refresh = named(gtk_button_new_with_label("Aygıtları yenile"), "preferences-driver-refresh"); pack(body, refresh, FALSE);
    g_signal_connect(refresh, "clicked", G_CALLBACK(driver_refresh), driver);
    g_signal_connect(dialog, "destroy", G_CALLBACK(driver_destroyed), driver);
    g_signal_connect_swapped(dialog, "response", G_CALLBACK(gtk_widget_destroy), dialog);
    gtk_widget_show_all(dialog);
}

static GtkWidget *configuration_new(Preferences *prefs)
{
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget *audio = section_new("Audio"), *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    prefs->device = named(gtk_combo_box_text_new(), "preferences-audio-device");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(prefs->device), "@default", "Sistem varsayılan çıkışı");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(prefs->device), "@default");
    GtkWidget *driver = named(gtk_button_new_with_label("Driver"), "preferences-driver");
    gtk_widget_set_size_request(driver, 150, -1); pack(row, prefs->device, TRUE); pack(row, driver, FALSE);
    pack(audio, row, FALSE); pack(body, audio, FALSE);
    g_signal_connect(prefs->device, "changed", G_CALLBACK(device_changed), prefs);
    g_signal_connect(driver, "clicked", G_CALLBACK(driver_clicked), prefs);

    GtkWidget *sample = section_new("Sample Rate");
    prefs->sample_rate = named(gtk_combo_box_text_new(), "preferences-sample-rate");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(prefs->sample_rate), "44100", "44100 Hz");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(prefs->sample_rate), "48000", "48000 Hz");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(prefs->sample_rate), "44100");
    gtk_widget_set_tooltip_text(prefs->sample_rate, "Gerçek ses çıkışının örnekleme hızı. Değişiklik çalan kaynakları duraklatır; aygıtın gerçek değeri aşağıda gösterilir.");
    pack(sample, prefs->sample_rate, FALSE); pack(body, sample, FALSE);
    g_signal_connect(prefs->sample_rate, "changed", G_CALLBACK(audio_option_changed), prefs);

    GtkWidget *buffer = section_new("Buffer size");
    prefs->buffer_label = named(label_new("256 samples (5.8 ms)"), "preferences-buffer-label");
    prefs->buffer = named(gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0, 6, 1), "preferences-buffer-size");
    gtk_scale_set_draw_value(GTK_SCALE(prefs->buffer), FALSE);
    gtk_range_set_value(GTK_RANGE(prefs->buffer), 2);
    gtk_widget_set_tooltip_text(prefs->buffer, "Çıkış buffer boyutunu değiştirmek çalan kaynakları duraklatır. Ses sunucusu değeri aygıtına göre ayarlayabilir; gerçek değer aşağıda gösterilir.");
    pack(buffer, prefs->buffer_label, FALSE); pack(buffer, prefs->buffer, FALSE); pack(body, buffer, FALSE);
    g_signal_connect(prefs->buffer, "value-changed", G_CALLBACK(audio_option_changed), prefs);

    GtkWidget *metronome = section_new("Metronome");
    GSList *group = NULL;
    for (guint i = 0; i < 3; ++i) {
        gchar *label = g_strdup_printf("Click Sound %02u", i + 1), *name = g_strdup_printf("preferences-click-sound-%02u", i + 1);
        prefs->sounds[i] = named(gtk_radio_button_new_with_label(group, label), name);
        group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(prefs->sounds[i]));
        pack(metronome, prefs->sounds[i], FALSE); g_free(label); g_free(name);
        g_signal_connect(prefs->sounds[i], "toggled", G_CALLBACK(radio_changed), prefs);
    }
    GtkWidget *volume = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5); style(volume, "preferences-volume");
    GtkWidget *heading = label_new("Volume"); style(heading, "preferences-heading"); pack(volume, heading, FALSE);
    const gchar *labels[] = {"Small", "Middle", "Large"}, *names[] = {"small", "middle", "large"}; group = NULL;
    for (guint i = 0; i < 3; ++i) {
        gchar *name = g_strdup_printf("preferences-volume-%s", names[i]);
        prefs->volumes[i] = named(gtk_radio_button_new_with_label(group, labels[i]), name);
        group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(prefs->volumes[i]));
        pack(volume, prefs->volumes[i], FALSE); g_free(name);
        g_signal_connect(prefs->volumes[i], "toggled", G_CALLBACK(radio_changed), prefs);
    }
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(prefs->volumes[1]), TRUE);
    pack(metronome, volume, FALSE); pack(body, metronome, FALSE);
    GtkWidget *scroll = named(gtk_scrolled_window_new(NULL, NULL), "preferences-configuration");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), body);
    return scroll;
}

static void register_field(GtkWidget *page, GtkWidget *widget, const gchar *name)
{ gtk_widget_set_name(widget, name); g_object_set_data(G_OBJECT(page), name, widget); }

static GtkWidget *routing_new(Preferences *prefs)
{
    GtkWidget *page = named(gtk_box_new(GTK_ORIENTATION_VERTICAL, 14), "preferences-input-output");
    gtk_container_set_border_width(GTK_CONTAINER(page), 28); prefs->routing = page;
    GtkWidget *heading = label_new("Input/Output"); style(heading, "preferences-heading"); pack(page, heading, FALSE);
    const gchar *labels[] = {"MAIN", "CUE / Headphones", "Monitor", "Microphone"};
    const gchar *ids[] = {"task-main-output", "task-cue-output", "task-monitor-output", "task-microphone-input"};
    for (guint i = 0; i < 4; ++i) {
        pack(page, label_new(labels[i]), FALSE); prefs->outputs[i] = gtk_combo_box_text_new();
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(prefs->outputs[i]), "", "Atanmadı");
        gtk_combo_box_set_active(GTK_COMBO_BOX(prefs->outputs[i]), 0);
        register_field(page, prefs->outputs[i], ids[i]); pack(page, prefs->outputs[i], FALSE);
    }
    GtkWidget *microphone = gtk_toggle_button_new_with_label("Mikrofon kapalı");
    register_field(page, microphone, "task-microphone-gate"); pack(page, microphone, FALSE);
    GtkWidget *gain = radios_knob_new("Mikrofon", -60, 30, 0.5, 0);
    register_field(page, gain, "task-microphone-gain"); pack(page, gain, FALSE);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    GtkWidget *refresh = gtk_button_new_with_label("Aygıtları yenile"); prefs->apply = gtk_button_new_with_label("Uygula / Kaydet");
    register_field(page, refresh, "task-audio-refresh"); register_field(page, prefs->apply, "task-audio-apply");
    pack(row, refresh, FALSE); pack(row, prefs->apply, FALSE); pack(page, row, FALSE);
    GtkWidget *status = label_new("Ses aygıtları bekleniyor."); gtk_label_set_line_wrap(GTK_LABEL(status), TRUE);
    register_field(page, status, "task-audio-status"); pack(page, status, FALSE);
    radios_audio_ui_bind_settings(prefs->ctx, page);
    g_signal_connect(prefs->outputs[0], "changed", G_CALLBACK(routing_main_changed), prefs);
    return page;
}

static void category_open(GtkButton *button, gpointer value)
{
    Preferences *prefs = value;
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "category"));
    if (index == 1) { radios_online_services_ui_open(prefs->ctx); return; }
    PreferencesNavigation *navigation = g_object_get_data(G_OBJECT(prefs->ctx->parent), NAVIGATION_KEY);
    if (navigation && navigation->callback) navigation->callback(prefs->ctx, category_names[index], navigation->data);
    else status_set(prefs, "Bu ayarlara ana penceredeki ilgili çalışma alanından ulaşabilirsin.");
}

static GtkWidget *category_page_new(Preferences *prefs, guint index)
{
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20); gtk_container_set_border_width(GTK_CONTAINER(page), 30);
    GtkWidget *heading = label_new(category_names[index]); style(heading, "preferences-heading"); pack(page, heading, FALSE);
    if (index == 2) {
        GtkWidget *header_settings = radios_header_status_preferences_new(prefs->ctx);
        if (header_settings) pack(page, header_settings, FALSE);
    }
    gchar *text = g_strdup_printf("%s aç", category_names[index]);
    GtkWidget *button = gtk_button_new_with_label(text); g_free(text);
    gchar *name = g_strdup_printf("preferences-open-%s", category_ids[index]); named(button, name); g_free(name);
    gtk_widget_set_halign(button, GTK_ALIGN_START); g_object_set_data(G_OBJECT(button), "category", GUINT_TO_POINTER(index));
    g_signal_connect(button, "clicked", G_CALLBACK(category_open), prefs); pack(page, button, FALSE);
    return page;
}

static void category_selected(GtkListBox *list, GtkListBoxRow *row, gpointer value)
{
    (void)list; Preferences *prefs = value; if (!row) return;
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "category"));
    gtk_stack_set_visible_child_name(GTK_STACK(prefs->content), category_ids[index]);
}

static gboolean category_filter(GtkListBoxRow *row, gpointer value)
{
    Preferences *prefs = value;
    const gchar *text = gtk_entry_get_text(GTK_ENTRY(prefs->search));
    gchar *needle = g_utf8_casefold(text, -1);
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(row), "category"));
    gchar *haystack = g_utf8_casefold(category_names[index], -1);
    gboolean show = strstr(haystack, needle) != NULL; g_free(needle); g_free(haystack); return show;
}

static void search_changed(GtkSearchEntry *entry, gpointer value)
{
    (void)entry; Preferences *prefs = value;
    gtk_list_box_invalidate_filter(GTK_LIST_BOX(prefs->categories));
}

static void close_clicked(GtkButton *button, gpointer value)
{ (void)button; gtk_widget_destroy(((Preferences *)value)->window); }

static gboolean key_pressed(GtkWidget *widget, GdkEventKey *event, gpointer value)
{
    (void)widget;
    if (event->keyval != GDK_KEY_Escape) return FALSE;
    gtk_widget_destroy(((Preferences *)value)->window); return TRUE;
}

static void reset_clicked(GtkButton *button, gpointer value)
{
    (void)button; Preferences *prefs = value;
    RadioSPreferencesAudio defaults; radios_preferences_audio_default(&defaults);
    audio_options_set(prefs, &defaults);
    audio_options_save(prefs, TRUE);
}

static void callbacks_disconnect(GtkWidget *widget, gpointer value)
{
    g_signal_handlers_disconnect_by_data(widget, value);
    if (!GTK_IS_CONTAINER(widget)) return;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget));
    for (GList *child = children; child; child = child->next) callbacks_disconnect(child->data, value);
    g_list_free(children);
}

static void preferences_destroyed(GtkWidget *widget, gpointer value)
{
    Preferences *prefs = value;
    callbacks_disconnect(widget, prefs);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(prefs->categories), NULL, NULL, NULL);
    if (prefs->poll_id) g_source_remove(prefs->poll_id);
    if (g_object_get_data(G_OBJECT(prefs->ctx->parent), WINDOW_KEY) == prefs->window)
        g_object_set_data(G_OBJECT(prefs->ctx->parent), WINDOW_KEY, NULL);
    if (prefs->engine) radios_audio_engine_unref(prefs->engine);
    gtk_style_context_remove_provider_for_screen(prefs->screen, GTK_STYLE_PROVIDER(prefs->style));
    g_object_unref(prefs->style); g_object_unref(prefs->screen); g_free(prefs->device_selection); g_free(prefs);
}

void radios_preferences_set_navigation(GtkWindow *parent, RadioSPreferencesNavigateFunc callback, gpointer data)
{
    g_return_if_fail(GTK_IS_WINDOW(parent));
    PreferencesNavigation *navigation = g_new0(PreferencesNavigation, 1);
    navigation->callback = callback; navigation->data = data;
    g_object_set_data_full(G_OBJECT(parent), NAVIGATION_KEY, navigation, g_free);
}

void radios_preferences_apply_startup(RadioSViewContext *ctx)
{
    g_return_if_fail(ctx);
    if (!ctx->config || !ctx->config->project_root) return;
    RadioSPreferencesAudio options; GError *error = NULL;
    if (!radios_preferences_audio_load(ctx->config->project_root, &options, &error)) {
        if (ctx->notify) ctx->notify(ctx->notify_data, error ? error->message : "Ses tercihi okunamadı.");
        g_clear_error(&error); return;
    }
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(ctx);
    if (!engine) return;
    if (!radios_audio_engine_configure_output(engine, options.sample_rate, options.buffer_frames, &error)) {
        if (ctx->notify) ctx->notify(ctx->notify_data, error ? error->message : "Ses çıkış tercihi uygulanamadı.");
        g_clear_error(&error);
    }
    radios_audio_engine_unref(engine);
}

GtkWidget *radios_preferences_open(RadioSViewContext *ctx)
{
    g_return_val_if_fail(ctx && GTK_IS_WINDOW(ctx->parent), NULL);
    GtkWidget *existing = g_object_get_data(G_OBJECT(ctx->parent), WINDOW_KEY);
    if (existing) { gtk_window_present(GTK_WINDOW(existing)); return existing; }
    Preferences *prefs = g_new0(Preferences, 1); prefs->ctx = ctx; prefs->updating = TRUE;
    prefs->window = named(gtk_window_new(GTK_WINDOW_TOPLEVEL), "preferences-window");
    gtk_window_set_title(GTK_WINDOW(prefs->window), "Preferences");
    gtk_window_set_transient_for(GTK_WINDOW(prefs->window), ctx->parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(prefs->window), TRUE);
    gtk_window_set_decorated(GTK_WINDOW(prefs->window), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(prefs->window), GDK_WINDOW_TYPE_HINT_DIALOG);
    GdkRectangle area = {0, 0, 1100, 1050};
    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(ctx->parent));
    GdkWindow *parent_window = gtk_widget_get_window(GTK_WIDGET(ctx->parent));
    GdkMonitor *monitor = parent_window ? gdk_display_get_monitor_at_window(display, parent_window) : gdk_display_get_primary_monitor(display);
    if (monitor) gdk_monitor_get_workarea(monitor, &area);
    gtk_window_set_default_size(GTK_WINDOW(prefs->window), MIN(1030, MAX(720, area.width - 60)), MIN(1050, MAX(600, area.height - 60)));
    prefs->screen = g_object_ref(gtk_widget_get_screen(prefs->window)); prefs->style = gtk_css_provider_new();
    GError *error = NULL;
    if (!gtk_css_provider_load_from_data(prefs->style, css, -1, &error)) {
        g_warning("Preferences CSS: %s", error->message); g_clear_error(&error);
    }
    gtk_style_context_add_provider_for_screen(prefs->screen, GTK_STYLE_PROVIDER(prefs->style), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 30);
    GtkWidget *layout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_container_add(GTK_CONTAINER(prefs->window), layout);
    GtkWidget *titlebar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); style(titlebar, "preferences-titlebar");
    pack(titlebar, label_new("Preferences"), TRUE);
    GtkWidget *close = named(gtk_button_new_with_label("×"), "preferences-close");
    gtk_widget_set_tooltip_text(close, "Close"); pack(titlebar, close, FALSE); pack(layout, titlebar, FALSE);
    g_signal_connect(close, "clicked", G_CALLBACK(close_clicked), prefs);
    GtkWidget *workspace = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); pack(layout, workspace, TRUE);
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); style(sidebar, "preferences-sidebar");
    gtk_widget_set_size_request(sidebar, 210, -1); pack(workspace, sidebar, FALSE);
    prefs->search = named(gtk_search_entry_new(), "preferences-search");
    gtk_entry_set_placeholder_text(GTK_ENTRY(prefs->search), NULL); pack(sidebar, prefs->search, FALSE);
    prefs->categories = named(gtk_list_box_new(), "preferences-categories");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(prefs->categories), GTK_SELECTION_SINGLE);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(prefs->categories), category_filter, prefs, NULL); pack(sidebar, prefs->categories, FALSE);
    for (guint i = 0; i < G_N_ELEMENTS(category_names); ++i) {
        GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 18);
        gchar *name = g_strdup_printf("preferences-category-%s", category_ids[i]); named(row, name); g_free(name);
        g_object_set_data(G_OBJECT(row), "category", GUINT_TO_POINTER(i));
        GtkWidget *icon = gtk_drawing_area_new(); gtk_widget_set_size_request(icon, 28, 30);
        g_signal_connect(icon, "draw", G_CALLBACK(icon_draw), GUINT_TO_POINTER(i));
        pack(box, icon, FALSE); pack(box, label_new(category_names[i]), TRUE); gtk_container_add(GTK_CONTAINER(row), box);
        gtk_container_add(GTK_CONTAINER(prefs->categories), row);
    }
    g_signal_connect(prefs->search, "search-changed", G_CALLBACK(search_changed), prefs);
    GtkWidget *main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); pack(workspace, main, TRUE);
    prefs->content = named(gtk_stack_new(), "preferences-content"); pack(main, prefs->content, TRUE);
    prefs->notebook = named(gtk_notebook_new(), "preferences-notebook");
    /* Routing fields must exist before Configuration connects its device mirror. */
    GtkWidget *routing = routing_new(prefs), *configuration = configuration_new(prefs);
    gtk_notebook_append_page(GTK_NOTEBOOK(prefs->notebook), configuration, gtk_label_new("Configuration"));
    gtk_notebook_append_page(GTK_NOTEBOOK(prefs->notebook), routing, gtk_label_new("Input/Output"));
    gtk_stack_add_named(GTK_STACK(prefs->content), prefs->notebook, "audio");
    for (guint i = 0; i < G_N_ELEMENTS(category_names); ++i)
        if (i != 3) gtk_stack_add_named(GTK_STACK(prefs->content), category_page_new(prefs, i), category_ids[i]);
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); style(footer, "preferences-footer");
    GtkWidget *reset = named(gtk_button_new_with_label("Reset to defaults"), "preferences-reset");
    gtk_widget_set_halign(reset, GTK_ALIGN_START); pack(footer, reset, FALSE);
    prefs->status = named(label_new(""), "preferences-status");
    gtk_label_set_line_wrap(GTK_LABEL(prefs->status), TRUE); pack(footer, prefs->status, FALSE); pack(main, footer, FALSE);
    g_signal_connect(reset, "clicked", G_CALLBACK(reset_clicked), prefs);
    g_signal_connect(prefs->categories, "row-selected", G_CALLBACK(category_selected), prefs);
    gtk_list_box_select_row(GTK_LIST_BOX(prefs->categories), gtk_list_box_get_row_at_index(GTK_LIST_BOX(prefs->categories), 3));
    gtk_notebook_set_current_page(GTK_NOTEBOOK(prefs->notebook), 0);
    prefs->engine = radios_audio_ui_engine_acquire(ctx);
    g_object_set_data(G_OBJECT(ctx->parent), WINDOW_KEY, prefs->window);
    g_signal_connect(prefs->window, "destroy", G_CALLBACK(preferences_destroyed), prefs);
    g_signal_connect(prefs->window, "key-press-event", G_CALLBACK(key_pressed), prefs);
    prefs->updating = FALSE;
    RadioSPreferencesAudio options; radios_preferences_audio_default(&options);
    GError *load_error = NULL;
    if (ctx->config && ctx->config->project_root && !radios_preferences_audio_load(ctx->config->project_root, &options, &load_error)) {
        gchar *message = g_strdup_printf("Ses tercihi okunamadı: %s", load_error->message);
        prefs->storage_error = TRUE;
        status_set(prefs, message); g_free(message); g_clear_error(&load_error);
    }
    audio_options_set(prefs, &options);
    devices_update(prefs); prefs->poll_id = g_timeout_add(500, devices_update, prefs);
    gtk_widget_show_all(prefs->window); gtk_window_present(GTK_WINDOW(prefs->window));
    return prefs->window;
}

static void preferences_button_clicked(GtkButton *button, gpointer value)
{ (void)button; radios_preferences_open(value); }

GtkWidget *radios_preferences_button_new(RadioSViewContext *ctx)
{
    g_return_val_if_fail(ctx, NULL);
    GtkWidget *button = named(gtk_button_new(), "header-settings");
    gtk_widget_set_tooltip_text(button, "Ayarlar");
    atk_object_set_name(gtk_widget_get_accessible(button), "Ayarlar");
    gtk_button_set_image(GTK_BUTTON(button), gtk_image_new_from_icon_name("emblem-system-symbolic", GTK_ICON_SIZE_BUTTON));
    gtk_style_context_add_class(gtk_widget_get_style_context(button), "header-settings-button");
    g_signal_connect(button, "clicked", G_CALLBACK(preferences_button_clicked), ctx);
    return button;
}
