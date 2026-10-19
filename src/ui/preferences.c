#include "radios/preferences.h"
#include "radios/preferences_storage.h"
#include "radios/audio.h"
#include "radios/audio_ui.h"
#include "radios/header_status.h"
#include "radios/widgets.h"
#include "radios/programme_prerequisites.h"
#include "radios/midi_ui.h"
#include "radios/reset_ui.h"
#include "radios/copyright_settings.h"
#include "radios/loudness_settings.h"
#include "radios/autodj_settings.h"
#include "radios/media_archive_ui.h"
#include "radios/library_analysis_ui.h"
#include "radios/transfer_ui.h"
#include "radios/view_presets_ui.h"
#include <gio/gio.h>
#include <math.h>
#include <string.h>

#define WINDOW_KEY "radios-preferences-window"
#define NAVIGATION_KEY "radios-preferences-navigation"
#define APPEARANCE_KEY "radios-preferences-appearance"

typedef struct {
    RadioSPreferencesNavigateFunc callback;
    gpointer data;
} PreferencesNavigation;

typedef struct {
    RadioSPreferencesAppearanceFunc callback;
    gpointer data;
} PreferencesAppearance;

typedef struct {
    RadioSViewContext *ctx;
    GtkWidget *window, *categories, *search, *content, *notebook;
    GtkWidget *device, *sample_rate, *max_rate, *loudness, *loudness_target, *status;
    GtkWidget *routing, *outputs[4], *apply;
    RadioSPreferencesAudio stored; /* buffer and click values kept from storage */
    GtkWidget *copyright_license, *copyright_text, *copyright_source, *copyright_status;
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
    "Audio", "AutoDJ", "MediaArchive", "LibraryAnalysis", "Export", "Import", "View", "MIDI", "Keyboard", "Advanced", "Reset", "CopyrightPolicy", "Copyright", "About"
};
static const gchar *const category_labels[] = {
    "Ses", "Auto DJ", "Medya arşivi", "Kütüphane analizi", "Export", "Import", "Görünüm", "MIDI", "Kısayol", "Gelişmiş", "Sıfırlama", "Telif bilgileri", "Lisans", "Hakkında"
};
static const gchar *const category_ids[] = {
    "audio", "autodj", "media-archive", "library-analysis", "export", "import", "view", "midi", "keyboard", "advanced", "reset", "copyright-policy", "copyright", "about"
};
typedef enum { CATEGORY_AUDIO, CATEGORY_AUTODJ, CATEGORY_MEDIA_ARCHIVE, CATEGORY_LIBRARY_ANALYSIS, CATEGORY_EXPORT, CATEGORY_IMPORT, CATEGORY_VIEW, CATEGORY_MIDI, CATEGORY_KEYBOARD, CATEGORY_ADVANCED, CATEGORY_RESET, CATEGORY_COPYRIGHT_POLICY, CATEGORY_LICENSE, CATEGORY_ABOUT } Category;

G_STATIC_ASSERT(G_N_ELEMENTS(category_names) == G_N_ELEMENTS(category_labels));
G_STATIC_ASSERT(G_N_ELEMENTS(category_names) == G_N_ELEMENTS(category_ids));
G_STATIC_ASSERT(G_N_ELEMENTS(category_names) == CATEGORY_ABOUT + 1);

static const gchar *const css[] = {
    "#preferences-window, #preferences-driver-dialog { color: #b5b5b5; background-color: #080808; border: 1px solid #444; font-family: 'Inter Variable', 'Inter', sans-serif; font-size: 14px; }",
    "#preferences-window *, #preferences-driver-dialog * { background-image: none; text-shadow: none; box-shadow: none; }",
    "#preferences-window label, #preferences-driver-dialog label { color: inherit; background-color: transparent; }",
    "#preferences-window box, #preferences-window stack, #preferences-window viewport, #preferences-window scrolledwindow, #preferences-driver-dialog box { background-color: transparent; }",
    "#preferences-window .preferences-titlebar { background-color: #101010; border-bottom: 1px solid #333; padding: 14px 18px; }",
    "#preferences-window .preferences-title { color: #ededed; font-size: 20px; font-weight: 400; }",
    "#preferences-window .preferences-subtitle { color: #929292; font-size: 11px; }",
    "#preferences-window .preferences-sidebar { background-color: #080808; border-right: 1px solid #333; padding: 16px 12px; }",
    "#preferences-window entry, #preferences-driver-dialog entry { color: #c8c8c8; background-color: #070707; min-height: 24px; border: 1px solid #444; border-radius: 4px; padding: 6px 8px; }",
    "#preferences-window entry:focus, #preferences-driver-dialog entry:focus { border-color: #36caff; }",
    "#preferences-window spinbutton, #preferences-window spinbutton text { color: #c8c8c8; }",
    "#preferences-window progressbar trough { background-color: #1b1b1b; border: 1px solid #3b3b3b; border-radius: 4px; min-height: 20px; }",
    "#preferences-window progressbar progress { background-color: #1d6f8c; border-radius: 3px; min-height: 20px; }",
    "#preferences-window progressbar text { color: #ededed; font-size: 12px; }",
    "#preferences-window entry selection { background-color: #26434d; color: #ededed; }",
    "#preferences-window button, #preferences-driver-dialog button { color: #c8c8c8; background-color: #101010; border: 1px solid #3b3b3b; border-radius: 4px; padding: 6px 10px; min-height: 24px; }",
    "#preferences-window combobox button { padding: 5px 8px; }",
    "#preferences-window button:hover, #preferences-driver-dialog button:hover { background-color: #222; border-color: #777; }",
    "#preferences-window button:focus, #preferences-driver-dialog button:focus { border-color: #36caff; }",
    "#preferences-window button:checked, #preferences-driver-dialog button:checked { background-color: #292929; border-color: #777; color: #ededed; }",
    "#preferences-window button:disabled, #preferences-driver-dialog button:disabled { color: #707070; background-color: #090909; border-color: #282828; }",
    "#preferences-window #preferences-close { min-width: 26px; min-height: 26px; padding: 2px; font-size: 22px; }",
    "#preferences-window #preferences-close:hover { color: #f1c0b3; background-color: #351e1b; border-color: #8a4d41; }",
    "#preferences-window #preferences-categories { background-color: transparent; padding-top: 14px; }",
    "#preferences-window #preferences-categories row { color: #929292; min-height: 32px; padding: 8px; margin-bottom: 4px; border: 1px solid transparent; border-radius: 5px; }",
    "#preferences-window #preferences-categories row:hover { color: #ddd; background-color: #151515; }",
    "#preferences-window #preferences-categories row:selected { color: #ededed; background-color: #252525; border-color: #555; border-left: 2px solid #36caff; }",
    "#preferences-window .preferences-section { background-color: #101010; padding: 16px 18px; margin: 6px 12px; border: 1px solid #333; border-radius: 5px; }",
    "#preferences-window .preferences-heading { color: #ededed; font-size: 18px; font-weight: 400; margin-bottom: 8px; }",
    "#preferences-window .preferences-volume { border-top: 1px solid #333; margin-top: 12px; padding-top: 12px; }",
    "#preferences-window notebook { background-color: transparent; border: 0; }",
    "#preferences-window notebook > header { background-color: #080808; border-bottom: 1px solid #333; padding: 10px 12px 0; }",
    "#preferences-window notebook > header tabs tab { color: #929292; background-color: transparent; border: 1px solid transparent; border-bottom: 0; border-radius: 5px 5px 0 0; padding: 9px 14px; margin-right: 4px; }",
    "#preferences-window notebook > header tabs tab:hover { color: #ddd; background-color: #151515; }",
    "#preferences-window notebook > header tabs tab:checked { color: #ededed; background-color: #101010; border-color: #444; }",
    "#preferences-window scale { padding: 10px 4px 4px; }",
    "#preferences-window scale trough { min-height: 4px; background-color: #070707; border: 1px solid #444; border-radius: 4px; }",
    "#preferences-window scale highlight { border-color: #36caff; background-color: #36caff; }",
    "#preferences-window scale slider { min-width: 12px; min-height: 18px; border: 1px solid #777; border-radius: 4px; background-color: #c8c8c8; }",
    "#preferences-window scale slider:hover, #preferences-window scale slider:focus { border-color: #36caff; background-color: #ededed; }",
    "#preferences-window radio, #preferences-window check { min-width: 15px; min-height: 15px; margin-right: 6px; color: #080808; border: 1px solid #555; background-color: #101010; }",
    "#preferences-window radio { border-radius: 50%; }",
    "#preferences-window check { border-radius: 3px; }",
    "#preferences-window radio:checked { border-color: #36caff; background-color: #36caff; background-image: -gtk-icontheme('media-record-symbolic'); }",
    "#preferences-window check:checked { border-color: #36caff; background-color: #36caff; }",
    "#preferences-window checkbutton, #preferences-window radiobutton { min-height: 24px; padding: 2px 0; }",
    "#preferences-window scrollbar { background-color: #080808; }",
    "#preferences-window scrollbar slider { min-width: 6px; min-height: 6px; border-radius: 4px; background-color: #444; }",
    "#preferences-window scrollbar slider:hover { background-color: #777; }",
    "#preferences-window textview, #preferences-window textview text { color: #c8c8c8; background-color: #070707; }",
    "#preferences-window #preferences-copyright-license-scroll { border: 1px solid #444; border-radius: 4px; }",
    "#preferences-window #copyright-policy-rules { color: #c8c8c8; background-color: #070707; font-size: 12px; }",
    "#preferences-window #copyright-policy-rules:selected { color: #ededed; background-color: #252525; }",
    "#preferences-window #copyright-policy-rules header button { color: #b5b5b5; background-color: #202020; border: 0; border-radius: 0; padding: 6px 8px; }",
    "#preferences-window .preferences-footer { background-color: #101010; border-top: 1px solid #333; padding: 12px 18px; }",
    "#preferences-window #preferences-reset { min-width: 170px; }",
    "#preferences-window #preferences-status { color: #929292; font-size: 12px; }",
    "#preferences-window menu, #preferences-driver-dialog menu { color: #c8c8c8; background-color: #101010; border: 1px solid #444; }",
    "#preferences-window menuitem:hover, #preferences-driver-dialog menuitem:hover { color: #ededed; background-color: #252525; }",
};

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

/* Sidebar icons share one 28×28 design box (centre 14,14; radius ≤ 11), one
 * 1.6 px stroke and round caps so the fourteen categories read as a set. */
static void icon_circle(cairo_t *cr, gdouble cx, gdouble cy, gdouble r)
{ cairo_new_sub_path(cr); cairo_arc(cr, cx, cy, r, 0, 2 * G_PI); cairo_stroke(cr); }

static void icon_dot(cairo_t *cr, gdouble cx, gdouble cy, gdouble r)
{ cairo_new_sub_path(cr); cairo_arc(cr, cx, cy, r, 0, 2 * G_PI); cairo_fill(cr); }

static void icon_line(cairo_t *cr, gdouble x1, gdouble y1, gdouble x2, gdouble y2)
{ cairo_move_to(cr, x1, y1); cairo_line_to(cr, x2, y2); cairo_stroke(cr); }

/* Open chevron with its tip at (x, y), pointing along the unit vector (dx, dy). */
static void icon_arrow_head(cairo_t *cr, gdouble x, gdouble y, gdouble dx, gdouble dy)
{
    const gdouble length = 3.4, width = 2.8;
    cairo_move_to(cr, x - length * dx - width * dy, y - length * dy + width * dx);
    cairo_line_to(cr, x, y);
    cairo_line_to(cr, x - length * dx + width * dy, y - length * dy - width * dx);
    cairo_stroke(cr);
}

/* Arc of the unit circle at (14,14) with radius r, clockwise from a1 to a2
 * (counter-clockwise when a2 < a1), finished with an arrow head. */
static void icon_arrow_arc(cairo_t *cr, gdouble r, gdouble a1, gdouble a2)
{
    gdouble turn = a2 > a1 ? 1 : -1;
    cairo_new_sub_path(cr);
    if (turn > 0) cairo_arc(cr, 14, 14, r, a1, a2); else cairo_arc_negative(cr, 14, 14, r, a1, a2);
    cairo_stroke(cr);
    icon_arrow_head(cr, 14 + r * cos(a2), 14 + r * sin(a2), -turn * sin(a2), turn * cos(a2));
}

static void icon_rounded_rectangle(cairo_t *cr, gdouble x, gdouble y, gdouble w, gdouble h, gdouble r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -G_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, G_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, G_PI / 2, G_PI);
    cairo_arc(cr, x + r, y + r, r, G_PI, 3 * G_PI / 2);
    cairo_close_path(cr); cairo_stroke(cr);
}

/* Open tray shared by Export and Import. */
static void icon_tray(cairo_t *cr)
{ cairo_move_to(cr, 4, 16); cairo_line_to(cr, 4, 24); cairo_line_to(cr, 24, 24); cairo_line_to(cr, 24, 16); cairo_stroke(cr); }

static gboolean icon_draw(GtkWidget *widget, cairo_t *cr, gpointer value)
{
    Category index = GPOINTER_TO_UINT(value);
    GtkStyleContext *context = gtk_widget_get_style_context(widget);
    GdkRGBA foreground;
    gtk_style_context_get_color(context, gtk_style_context_get_state(context), &foreground);
    gdk_cairo_set_source_rgba(cr, &foreground);
    cairo_set_line_width(cr, 1.6);
    cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND); cairo_set_line_join(cr, CAIRO_LINE_JOIN_ROUND);
    /* Centre the design box in whatever the row allocates. */
    cairo_translate(cr, (gtk_widget_get_allocated_width(widget) - 28) / 2.0, (gtk_widget_get_allocated_height(widget) - 28) / 2.0);
    switch (index) {
    case CATEGORY_AUDIO:
        /* Speaker: cabinet, cone and two sound waves. */
        cairo_move_to(cr, 4.5, 10.5); cairo_line_to(cr, 8.5, 10.5); cairo_line_to(cr, 13.5, 6); cairo_line_to(cr, 13.5, 22);
        cairo_line_to(cr, 8.5, 17.5); cairo_line_to(cr, 4.5, 17.5); cairo_close_path(cr); cairo_stroke(cr);
        cairo_new_sub_path(cr); cairo_arc(cr, 13.5, 14, 4.5, -0.7, 0.7); cairo_stroke(cr);
        cairo_new_sub_path(cr); cairo_arc(cr, 13.5, 14, 8.5, -0.7, 0.7); cairo_stroke(cr);
        break;
    case CATEGORY_AUTODJ:
        /* Looping play: two arrowed arcs around a play mark. */
        icon_arrow_arc(cr, 9.5, G_PI * 1.12, G_PI * 1.88);
        icon_arrow_arc(cr, 9.5, G_PI * 0.12, G_PI * 0.88);
        cairo_move_to(cr, 11, 10); cairo_line_to(cr, 19, 14); cairo_line_to(cr, 11, 18); cairo_close_path(cr); cairo_stroke(cr);
        break;
    case CATEGORY_MEDIA_ARCHIVE:
        /* Folder holding a note. */
        cairo_move_to(cr, 3.5, 7); cairo_line_to(cr, 10.5, 7); cairo_line_to(cr, 13, 10); cairo_line_to(cr, 24.5, 10);
        cairo_line_to(cr, 24.5, 23.5); cairo_line_to(cr, 3.5, 23.5); cairo_close_path(cr); cairo_stroke(cr);
        icon_line(cr, 16, 12.5, 16, 19.5);
        cairo_move_to(cr, 16, 12.5); cairo_curve_to(cr, 18, 13, 19.2, 14.2, 19, 16.2); cairo_stroke(cr);
        icon_dot(cr, 14.3, 19.7, 1.9);
        break;
    case CATEGORY_LIBRARY_ANALYSIS: {
        /* Magnifier over a waveform. */
        static const gdouble bars[] = {3.5, 8, 12, 7, 3.5};
        for (guint i = 0; i < G_N_ELEMENTS(bars); ++i) icon_line(cr, 7 + 3.0 * i, 12 - bars[i] / 2, 7 + 3.0 * i, 12 + bars[i] / 2);
        icon_circle(cr, 13, 12, 8.5);
        icon_line(cr, 19, 18, 24.5, 23.5);
        break;
    }
    case CATEGORY_EXPORT:
        /* Tray with an arrow leaving it. */
        icon_tray(cr); icon_line(cr, 14, 19.5, 14, 4.5);
        cairo_move_to(cr, 9.5, 9); cairo_line_to(cr, 14, 4.5); cairo_line_to(cr, 18.5, 9); cairo_stroke(cr);
        break;
    case CATEGORY_IMPORT:
        /* Tray with an arrow entering it. */
        icon_tray(cr); icon_line(cr, 14, 4.5, 14, 19.5);
        cairo_move_to(cr, 9.5, 15); cairo_line_to(cr, 14, 19.5); cairo_line_to(cr, 18.5, 15); cairo_stroke(cr);
        break;
    case CATEGORY_VIEW:
        /* Monitor on a stand. */
        icon_rounded_rectangle(cr, 3.5, 5, 21, 14, 1.5);
        icon_line(cr, 14, 19, 14, 23.5); icon_line(cr, 9, 23.5, 19, 23.5);
        break;
    case CATEGORY_MIDI:
        /* DIN connector: ring, five pins and the key notch. */
        icon_circle(cr, 14, 14, 10.5);
        for (guint i = 0; i < 5; ++i) {
            gdouble angle = G_PI * (1 + 0.25 * i);
            icon_dot(cr, 14 + 6 * cos(angle), 14 + 6 * sin(angle), 1.4);
        }
        icon_line(cr, 14, 22, 14, 24.5);
        break;
    case CATEGORY_KEYBOARD:
        /* Keyboard: body, two key rows and a space bar. */
        icon_rounded_rectangle(cr, 3, 8, 22, 12, 1.5);
        for (guint i = 0; i < 5; ++i) cairo_rectangle(cr, 6.1 + 3.5 * i, 10.1, 1.8, 1.8);
        for (guint i = 0; i < 4; ++i) cairo_rectangle(cr, 7.85 + 3.5 * i, 13.1, 1.8, 1.8);
        cairo_fill(cr);
        icon_line(cr, 9.5, 17, 18.5, 17);
        break;
    case CATEGORY_ADVANCED: {
        /* Gear: eight teeth around a hub. */
        const guint teeth = 8;
        const gdouble step = 2 * G_PI / teeth, outer = 10.5, inner = 7.6, tooth = step * 0.42, flank = step * 0.08;
        for (guint i = 0; i < teeth; ++i) {
            gdouble a = i * step;
            gdouble angles[4] = {a - tooth / 2, a + tooth / 2, a + tooth / 2 + flank, a + step - tooth / 2 - flank};
            for (guint p = 0; p < 4; ++p) {
                gdouble r = p < 2 ? outer : inner, x = 14 + r * cos(angles[p]), y = 14 + r * sin(angles[p]);
                if (i == 0 && p == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
            }
        }
        cairo_close_path(cr); cairo_stroke(cr);
        icon_circle(cr, 14, 14, 3);
        break;
    }
    case CATEGORY_RESET:
        /* Arrow curling back round the circle. */
        icon_arrow_arc(cr, 9.5, G_PI * 1.38, G_PI * -0.38);
        break;
    case CATEGORY_COPYRIGHT_POLICY:
        /* © mark. */
        icon_circle(cr, 14, 14, 10.5);
        cairo_new_sub_path(cr); cairo_arc(cr, 14, 14, 5, G_PI * 0.25, G_PI * 1.75); cairo_stroke(cr);
        break;
    case CATEGORY_LICENSE:
        /* Key: bow, shaft and two teeth. */
        icon_circle(cr, 8, 20, 4.6);
        icon_line(cr, 11.3, 16.7, 23, 5);
        icon_line(cr, 23, 5, 25.3, 7.3); icon_line(cr, 19.8, 8.2, 22, 10.4);
        break;
    case CATEGORY_ABOUT:
        /* Information mark. */
        icon_circle(cr, 14, 14, 10.5);
        icon_line(cr, 14, 12.5, 14, 19.5);
        icon_dot(cr, 14, 8.8, 1.4);
        break;
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

/* The highest rate any sound card offers, from ALSA's codec and USB stream
 * descriptions (0 when unknown). RadioS mixes at 48 kHz and plays 44.1 or 48. */
static guint hardware_max_rate(void)
{
    guint best = 0; GDir *dir = g_dir_open("/proc/asound", 0, NULL); const gchar *card;
    while (dir && (card = g_dir_read_name(dir))) {
        if (!g_str_has_prefix(card, "card")) continue;
        gchar *path = g_build_filename("/proc/asound", card, NULL); GDir *entries = g_dir_open(path, 0, NULL); const gchar *entry;
        while (entries && (entry = g_dir_read_name(entries))) {
            if (!g_str_has_prefix(entry, "codec#") && !g_str_has_prefix(entry, "stream")) continue;
            gchar *file = g_build_filename(path, entry, NULL), *text = NULL;
            if (g_file_get_contents(file, &text, NULL, NULL)) {
                gchar **lines = g_strsplit(text, "\n", -1);
                for (guint i = 0; lines[i]; ++i) {
                    const gchar *rates = strstr(lines[i], "rates"); if (!rates) rates = strstr(lines[i], "Rates:");
                    if (!rates) continue;
                    gchar **words = g_strsplit_set(rates, " :,]\t", -1);
                    for (guint w = 0; words[w]; ++w) { guint64 value = g_ascii_strtoull(words[w], NULL, 10); if (value >= 8000 && value <= 768000) best = MAX(best, (guint)value); }
                    g_strfreev(words);
                }
                g_strfreev(lines);
            }
            g_free(text); g_free(file);
        }
        if (entries) g_dir_close(entries);
        g_free(path);
    }
    if (dir) g_dir_close(dir);
    return best;
}

static gboolean audio_options_save(Preferences *prefs, gboolean apply_output)
{
    RadioSPreferencesAudio options;
    radios_preferences_audio_default(&options);
    options.sample_rate = current_rate(prefs);
    options.buffer_frames = prefs->stored.buffer_frames;
    options.click_sound = prefs->stored.click_sound; options.click_volume = prefs->stored.click_volume;
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
    prefs->storage_error = FALSE; prefs->stored = options;
    status_set(prefs, prefs->engine && apply_output ? "Çıkış isteği kabul edildi ve tercih kaydedildi; aygıt yanıtı bekleniyor." : "Ses tercihi kaydedildi.");
    return TRUE;
}

static void audio_options_set(Preferences *prefs, const RadioSPreferencesAudio *options)
{
    prefs->updating = TRUE; prefs->stored = *options;
    gchar rate[16]; g_snprintf(rate, sizeof rate, "%u", options->sample_rate);
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(prefs->sample_rate), rate);
    prefs->updating = FALSE;
}

static void audio_option_changed(GtkWidget *widget, gpointer value)
{
    Preferences *prefs = value;
    if (!prefs->updating) audio_options_save(prefs, widget == prefs->sample_rate);
}

static void loudness_changed(GtkWidget *widget, gpointer value)
{
    (void)widget; Preferences *prefs = value; if (prefs->updating) return;
    gboolean enabled = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(prefs->loudness));
    gtk_widget_set_sensitive(prefs->loudness_target, enabled);
    GError *error = NULL;
    if (!radios_audio_ui_set_loudness(prefs->ctx, enabled, gtk_spin_button_get_value(GTK_SPIN_BUTTON(prefs->loudness_target)), &error)) {
        gchar *message = g_strdup_printf("Ses yüksekliği eşitleme uygulanamadı: %s", error ? error->message : "ses motoru hazır değil");
        status_set(prefs, message); g_free(message); g_clear_error(&error); return;
    }
    status_set(prefs, enabled ? "Ses yüksekliği eşitleme açık; ayar veritabanına kaydediliyor." : "Ses yüksekliği eşitleme kapalı; parçalar kendi seviyesinde çalar.");
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
            message = g_strdup_printf("Ses çıkışı: %u Hz · %u örnek (istenen %u)", bus->output_sample_rate, bus->output_buffer_frames, frames);
        else {
            message = g_strdup_printf("Çıkış isteği: %u Hz · %u örnek. Aygıt bağlantısı bekleniyor.", rate, frames);
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
    GtkWidget *dialog = gtk_dialog_new_with_buttons("Ses sürücüsü", GTK_WINDOW(prefs->window),
        GTK_DIALOG_DESTROY_WITH_PARENT, "Kapat", GTK_RESPONSE_CLOSE, NULL);
    gtk_widget_set_name(dialog, "preferences-driver-dialog");
    style(dialog, "radios");
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
    GtkWidget *audio = section_new("Master çıkışı (varsayılan ses aygıtı)"), *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    prefs->device = named(gtk_combo_box_text_new(), "preferences-audio-device");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(prefs->device), "@default", "Sistem varsayılan çıkışı");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(prefs->device), "@default");
    gtk_widget_set_tooltip_text(prefs->device, "Yayının ana (MASTER) çıkışının gittiği aygıt. Varsayılan aygıt master çıkışıdır.");
    GtkWidget *driver = named(gtk_button_new_with_label("Ses sürücüsü"), "preferences-driver");
    gtk_widget_set_size_request(driver, 140, -1); pack(row, prefs->device, TRUE); pack(row, driver, FALSE);
    pack(audio, row, FALSE); pack(body, audio, FALSE);
    g_signal_connect(prefs->device, "changed", G_CALLBACK(device_changed), prefs);
    g_signal_connect(driver, "clicked", G_CALLBACK(driver_clicked), prefs);

    GtkWidget *sample = section_new("Örnekleme hızı");
    prefs->sample_rate = named(gtk_combo_box_text_new(), "preferences-sample-rate");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(prefs->sample_rate), "44100", "44100 Hz");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(prefs->sample_rate), "48000", "48000 Hz");
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(prefs->sample_rate), "44100");
    gtk_widget_set_tooltip_text(prefs->sample_rate, "Gerçek ses çıkışının örnekleme hızı. Değişiklik çalan kaynakları duraklatır; aygıtın gerçek değeri aşağıda gösterilir.");
    pack(sample, prefs->sample_rate, FALSE);
    guint max_rate = hardware_max_rate();
    gchar *max_text = max_rate ? g_strdup_printf("Ses kartının desteklediği en yüksek örnekleme hızı: %u Hz", max_rate) :
        g_strdup("Ses kartının en yüksek örnekleme hızı okunamadı.");
    prefs->max_rate = named(label_new(max_text), "preferences-max-sample-rate"); g_free(max_text);
    style(prefs->max_rate, "preferences-subtitle"); pack(sample, prefs->max_rate, FALSE); pack(body, sample, FALSE);
    g_signal_connect(prefs->sample_rate, "changed", G_CALLBACK(audio_option_changed), prefs);

    GtkWidget *loudness = section_new("Mixer üzerinden otomatik ses eşitleme");
    prefs->loudness = named(gtk_check_button_new_with_label("Mixer kanallarında otomatik ses eşitlemeyi aç"), "preferences-loudness-normalize");
    GtkWidget *target_row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    prefs->loudness_target = named(gtk_spin_button_new_with_range(-30, -5, 0.5), "preferences-loudness-target");
    gtk_spin_button_set_digits(GTK_SPIN_BUTTON(prefs->loudness_target), 1);
    pack(target_row, label_new("Hedef"), FALSE); pack(target_row, prefs->loudness_target, FALSE); pack(target_row, label_new("LUFS"), FALSE);
    pack(loudness, prefs->loudness, FALSE); pack(loudness, target_row, FALSE); pack(body, loudness, FALSE);
    pack(loudness, radios_loudness_settings_new(prefs->ctx), FALSE);
    gboolean enabled = TRUE; gdouble target = -14;
    if (!radios_audio_ui_loudness(prefs->ctx, &enabled, &target)) gtk_widget_set_sensitive(loudness, FALSE);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(prefs->loudness), enabled);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(prefs->loudness_target), target); gtk_widget_set_sensitive(prefs->loudness_target, enabled);
    g_signal_connect(prefs->loudness, "toggled", G_CALLBACK(loudness_changed), prefs);
    g_signal_connect(prefs->loudness_target, "value-changed", G_CALLBACK(loudness_changed), prefs);

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
    GtkWidget *heading = label_new("Giriş / Çıkış"); style(heading, "preferences-heading"); pack(page, heading, FALSE);
    const gchar *labels[] = {"Ana çıkış", "CUE / Kulaklık", "Monitör", "Mikrofon"};
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
    GtkWidget *scroll = named(gtk_scrolled_window_new(NULL, NULL), "preferences-routing-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), page);
    return scroll;
}

static void category_open(GtkButton *button, gpointer value)
{
    Preferences *prefs = value;
    guint index = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(button), "category"));
    PreferencesNavigation *navigation = g_object_get_data(G_OBJECT(prefs->ctx->parent), NAVIGATION_KEY);
    if (navigation && navigation->callback) navigation->callback(prefs->ctx, category_names[index], navigation->data);
    else status_set(prefs, "Bu ayarlara ana penceredeki ilgili çalışma alanından ulaşabilirsin.");
}

typedef struct {
    const gchar *name, *path;
} CopyrightLicense;

static const CopyrightLicense copyright_licenses[] = {
    {"Inter — SIL Open Font License 1.1", "assets/fonts/LICENSE.txt"},
};

static void copyright_license_changed(GtkComboBox *combo, gpointer value)
{
    Preferences *prefs = value;
    gint selected = gtk_combo_box_get_active(combo);
    if (selected < 0 || (guint)selected >= G_N_ELEMENTS(copyright_licenses)) return;
    const CopyrightLicense *license = &copyright_licenses[selected];
    gtk_label_set_text(GTK_LABEL(prefs->copyright_source), license->path);
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(prefs->copyright_text));
    gtk_text_buffer_set_text(buffer, "", 0);
    const gchar *root = prefs->ctx->config ? prefs->ctx->config->project_root : NULL;
    if (!root || !*root) {
        gtk_label_set_text(GTK_LABEL(prefs->copyright_status), "Lisans metni okunamadı: proje dizini bulunamadı.");
        return;
    }
    gchar *path = g_build_filename(root, license->path, NULL);
    GFile *file = g_file_new_for_path(path); g_free(path);
    GError *error = NULL;
    GFileInfo *info = g_file_query_info(file, G_FILE_ATTRIBUTE_STANDARD_TYPE,
        G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, NULL, &error);
    if (!info || g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR) {
        gtk_label_set_text(GTK_LABEL(prefs->copyright_status), "Lisans metni bu kurulumda bulunamadı veya okunamadı.");
        g_clear_object(&info); g_object_unref(file); g_clear_error(&error); return;
    }
    g_object_unref(info);
    GFileInputStream *stream = g_file_read(file, NULL, &error);
    g_object_unref(file);
    if (!stream) {
        gtk_label_set_text(GTK_LABEL(prefs->copyright_status), "Lisans metni bu kurulumda bulunamadı veya okunamadı.");
        g_clear_error(&error); return;
    }
    /* Fixed local notices only; bound the read without loading an arbitrary
     * oversized file into the GTK process. Preserve the original UTF-8 text. */
    const gsize limit = 128u * 1024u;
    gchar *contents = g_malloc(limit + 1);
    gsize length = 0;
    gboolean read = g_input_stream_read_all(G_INPUT_STREAM(stream), contents, limit + 1, &length, NULL, &error);
    g_object_unref(stream);
    if (!read || length > limit || !length || memchr(contents, '\0', length) ||
        !g_utf8_validate(contents, (gssize)length, NULL)) {
        gtk_label_set_text(GTK_LABEL(prefs->copyright_status), "Lisans metni okunamadı: dosya boş, geçersiz veya 128 KiB sınırını aşıyor.");
    } else {
        gtk_text_buffer_set_text(buffer, contents, (gint)length);
        gtk_label_set_text(GTK_LABEL(prefs->copyright_status), "Lisans metninin tamamı aşağıda gösteriliyor.");
    }
    g_free(contents); g_clear_error(&error);
}

static GtkWidget *copyright_page_new(Preferences *prefs)
{
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(body), 12);
    GtkWidget *application = section_new("Yazılım lisansı");
    GtkWidget *name = named(label_new("RadioS / SDBASE"), "preferences-copyright-application");
    pack(application, name, FALSE);
    GtkWidget *notice = named(label_new("RadioS ve SDBASE için bu kurulumda ayrı bir telif sahibi veya genel dağıtım lisansı belirtilmemiş."), "preferences-copyright-notice");
    gtk_label_set_line_wrap(GTK_LABEL(notice), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(notice), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(notice), 64);
    pack(application, notice, FALSE); pack(body, application, FALSE);
    GtkWidget *licenses = section_new("Bileşen lisansları");
    prefs->copyright_license = named(gtk_combo_box_text_new(), "preferences-copyright-license");
    for (guint i = 0; i < G_N_ELEMENTS(copyright_licenses); ++i)
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(prefs->copyright_license), copyright_licenses[i].name);
    atk_object_set_name(gtk_widget_get_accessible(prefs->copyright_license), "Bileşen lisansı");
    pack(licenses, prefs->copyright_license, FALSE);
    prefs->copyright_source = named(label_new(""), "preferences-copyright-source");
    gtk_label_set_line_wrap(GTK_LABEL(prefs->copyright_source), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(prefs->copyright_source), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(prefs->copyright_source), 64);
    gtk_label_set_selectable(GTK_LABEL(prefs->copyright_source), TRUE);
    style(prefs->copyright_source, "preferences-subtitle"); pack(licenses, prefs->copyright_source, FALSE);
    prefs->copyright_status = named(label_new(""), "preferences-copyright-status");
    gtk_label_set_line_wrap(GTK_LABEL(prefs->copyright_status), TRUE);
    gtk_label_set_max_width_chars(GTK_LABEL(prefs->copyright_status), 64);
    pack(licenses, prefs->copyright_status, FALSE);
    prefs->copyright_text = named(gtk_text_view_new(), "preferences-copyright-text");
    gtk_text_view_set_editable(GTK_TEXT_VIEW(prefs->copyright_text), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(prefs->copyright_text), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(prefs->copyright_text), GTK_WRAP_WORD_CHAR);
    gtk_text_view_set_left_margin(GTK_TEXT_VIEW(prefs->copyright_text), 12);
    gtk_text_view_set_right_margin(GTK_TEXT_VIEW(prefs->copyright_text), 12);
    gtk_text_view_set_top_margin(GTK_TEXT_VIEW(prefs->copyright_text), 10);
    gtk_text_view_set_bottom_margin(GTK_TEXT_VIEW(prefs->copyright_text), 10);
    atk_object_set_name(gtk_widget_get_accessible(prefs->copyright_text), "Telif ve lisans metni");
    GtkWidget *text_scroll = named(gtk_scrolled_window_new(NULL, NULL), "preferences-copyright-license-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(text_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_size_request(text_scroll, -1, 280);
    gtk_container_add(GTK_CONTAINER(text_scroll), prefs->copyright_text); pack(licenses, text_scroll, TRUE);
    pack(body, licenses, TRUE);
    g_signal_connect(prefs->copyright_license, "changed", G_CALLBACK(copyright_license_changed), prefs);
    gtk_combo_box_set_active(GTK_COMBO_BOX(prefs->copyright_license), 0);
    GtkWidget *scroll = named(gtk_scrolled_window_new(NULL, NULL), "preferences-copyright");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), body);
    return scroll;
}

static void about_line(GtkWidget *section, const gchar *caption, const gchar *value, const gchar *name)
{
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12), *key = label_new(caption), *text = named(label_new(value), name);
    gtk_widget_set_size_request(key, 190, -1); style(key, "preferences-subtitle");
    gtk_label_set_selectable(GTK_LABEL(text), TRUE); gtk_label_set_line_wrap(GTK_LABEL(text), TRUE); gtk_label_set_line_wrap_mode(GTK_LABEL(text), PANGO_WRAP_WORD_CHAR);
    pack(row, key, FALSE); pack(row, text, TRUE); pack(section, row, FALSE);
}
static GtkWidget *about_page_new(Preferences *prefs)
{
    GtkWidget *body = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8); gtk_container_set_border_width(GTK_CONTAINER(body), 12);
    GtkWidget *application = section_new("RadioS");
    about_line(application, "Sürüm", RADIOS_VERSION, "preferences-about-version");
    about_line(application, "Açıklama", "Radyo yayın otomasyonu, canlı oynatıcılar, AutoDJ, reklam ve yayın yönetimi.", "preferences-about-description");
    gchar *project = g_strdup(prefs->ctx->config && prefs->ctx->config->project_root ? prefs->ctx->config->project_root : "—");
    about_line(application, "Proje dizini", project, "preferences-about-root"); g_free(project);
    JsonObject *context = radios_data_context(prefs->ctx);
    JsonObject *station = context && json_object_has_member(context, "station") ? json_object_get_object_member(context, "station") : NULL;
    gchar *service = g_strdup_printf("%s%s%s", radios_data_connected(prefs->ctx) ? "Bağlı" : "Bağlı değil",
        station ? " · İstasyon: " : "", station ? radios_json_string(station, "name", "") : "");
    about_line(application, "Veritabanı servisi", service, "preferences-about-database"); g_free(service);
    pack(body, application, FALSE);
    GtkWidget *components = section_new("Bileşenler");
    gchar *gtk = g_strdup_printf("%u.%u.%u", gtk_get_major_version(), gtk_get_minor_version(), gtk_get_micro_version());
    gchar *glib = g_strdup_printf("%u.%u.%u", glib_major_version, glib_minor_version, glib_micro_version);
    about_line(components, "GTK", gtk, "preferences-about-gtk"); about_line(components, "GLib", glib, "preferences-about-glib");
    about_line(components, "Ses analizi", "BPM ve dörtlük ızgarası (radios-onset-grid-v3), ses yüksekliği ITU-R BS.1770 (LUFS)", "preferences-about-analysis");
    g_free(gtk); g_free(glib); pack(body, components, FALSE);
    GtkWidget *scroll = named(gtk_scrolled_window_new(NULL, NULL), "preferences-about");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), body); return scroll;
}
/* Export / Import pages come from transfer_ui.c ("preferences-export-page" /
 * "preferences-import-page"); each sits in its own vertical scroller. */
static GtkWidget *transfer_page_new(Preferences *prefs, guint index)
{
    gboolean import = index == CATEGORY_IMPORT;
    GtkWidget *page = import ? radios_transfer_import_page_new(prefs->ctx) : radios_transfer_export_page_new(prefs->ctx);
    GtkWidget *scroll = named(gtk_scrolled_window_new(NULL, NULL), import ? "preferences-import" : "preferences-export");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), page); return scroll;
}
static GtkWidget *category_page_new(Preferences *prefs, guint index)
{
    if (index == CATEGORY_EXPORT || index == CATEGORY_IMPORT) return transfer_page_new(prefs, index);
    if (index == CATEGORY_AUTODJ) return radios_autodj_settings_new(prefs->ctx);
    if (index == CATEGORY_MEDIA_ARCHIVE) return radios_media_archive_ui_page_new(prefs->ctx);
    if (index == CATEGORY_LIBRARY_ANALYSIS) return radios_library_analysis_ui_page_new(prefs->ctx);
    if (index == CATEGORY_COPYRIGHT_POLICY) return radios_copyright_settings_new(prefs->ctx);
    if (index == CATEGORY_LICENSE) return copyright_page_new(prefs);
    if (index == CATEGORY_ABOUT) return about_page_new(prefs);
    if (index == CATEGORY_MIDI) return radios_midi_ui_settings_new(prefs->ctx);
    if (index == CATEGORY_RESET) return radios_reset_ui_page_new(prefs->ctx);
    GtkWidget *page = gtk_box_new(GTK_ORIENTATION_VERTICAL, 20); gtk_container_set_border_width(GTK_CONTAINER(page), 30);
    GtkWidget *heading = label_new(category_labels[index]); style(heading, "preferences-heading"); pack(page, heading, FALSE);
    if (index == CATEGORY_VIEW) {
        /* Saved views first: choosing one applies all the settings below. */
        GtkWidget *presets = radios_view_presets_section_new(prefs->ctx);
        if (presets) pack(page, presets, FALSE);
        PreferencesAppearance *appearance = g_object_get_data(G_OBJECT(prefs->ctx->parent), APPEARANCE_KEY);
        GtkWidget *controls = appearance && appearance->callback ? appearance->callback(prefs->ctx, appearance->data) : NULL;
        if (controls) pack(page, controls, FALSE);
        GtkWidget *header_settings = radios_header_status_preferences_new(prefs->ctx);
        if (header_settings) pack(page, header_settings, FALSE);
        GtkWidget *scroll = named(gtk_scrolled_window_new(NULL, NULL), "preferences-appearance");
        gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
        gtk_container_add(GTK_CONTAINER(scroll), page);
        return scroll;
    }
    gchar *text = g_strdup_printf("%s çalışma alanını aç", category_labels[index]);
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
    gchar *labels = g_strdup_printf("%s %s%s", category_labels[index], category_names[index],
        g_str_equal(category_ids[index], "copyright") ? " lisans license" :
        g_str_equal(category_ids[index], "media-archive") ? " arşiv klasör depolama kayıt yedek veritabanı" :
        g_str_equal(category_ids[index], "export") ? " dışa aktar dışa aktarım aktarma" :
        g_str_equal(category_ids[index], "import") ? " içe aktar içe aktarım geri yükle aktarma" : "");
    gchar *haystack = g_utf8_casefold(labels, -1);
    g_free(labels);
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

void radios_preferences_set_appearance(GtkWindow *parent, RadioSPreferencesAppearanceFunc callback, gpointer data)
{
    g_return_if_fail(GTK_IS_WINDOW(parent));
    PreferencesAppearance *appearance = g_new0(PreferencesAppearance, 1);
    appearance->callback = callback; appearance->data = data;
    g_object_set_data_full(G_OBJECT(parent), APPEARANCE_KEY, appearance, g_free);
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
    style(prefs->window, "radios");
    gtk_window_set_title(GTK_WINDOW(prefs->window), "Ayarlar");
    gtk_window_set_transient_for(GTK_WINDOW(prefs->window), ctx->parent);
    gtk_window_set_destroy_with_parent(GTK_WINDOW(prefs->window), TRUE);
    gtk_window_set_decorated(GTK_WINDOW(prefs->window), FALSE);
    gtk_window_set_type_hint(GTK_WINDOW(prefs->window), GDK_WINDOW_TYPE_HINT_DIALOG);
    GdkRectangle area = {0, 0, 1100, 1050};
    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(ctx->parent));
    GdkWindow *parent_window = gtk_widget_get_window(GTK_WIDGET(ctx->parent));
    GdkMonitor *monitor = parent_window ? gdk_display_get_monitor_at_window(display, parent_window) : gdk_display_get_primary_monitor(display);
    if (monitor) gdk_monitor_get_workarea(monitor, &area);
    gtk_window_set_default_size(GTK_WINDOW(prefs->window), MIN(980, MAX(660, area.width - 60)), MIN(820, MAX(540, area.height - 60)));
    prefs->screen = g_object_ref(gtk_widget_get_screen(prefs->window)); prefs->style = gtk_css_provider_new();
    GError *error = NULL;
    GString *stylesheet = g_string_new(NULL);
    for (guint i = 0; i < G_N_ELEMENTS(css); ++i) g_string_append(stylesheet, css[i]);
    if (!gtk_css_provider_load_from_data(prefs->style, stylesheet->str, (gssize)stylesheet->len, &error)) {
        g_warning("Preferences CSS: %s", error->message); g_clear_error(&error);
    }
    g_string_free(stylesheet, TRUE);
    gtk_style_context_add_provider_for_screen(prefs->screen, GTK_STYLE_PROVIDER(prefs->style), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 30);
    GtkWidget *layout = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); gtk_container_add(GTK_CONTAINER(prefs->window), layout);
    GtkWidget *titlebar = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8); style(titlebar, "preferences-titlebar");
    GtkWidget *title = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3);
    GtkWidget *title_label = label_new("Ayarlar"); style(title_label, "preferences-title");
    GtkWidget *subtitle = label_new("RadioS çalışma tercihleri"); style(subtitle, "preferences-subtitle");
    pack(title, title_label, FALSE); pack(title, subtitle, FALSE); pack(titlebar, title, TRUE);
    GtkWidget *close = named(gtk_button_new_with_label("×"), "preferences-close");
    gtk_widget_set_tooltip_text(close, "Kapat (Esc)"); pack(titlebar, close, FALSE); pack(layout, titlebar, FALSE);
    g_signal_connect(close, "clicked", G_CALLBACK(close_clicked), prefs);
    GtkWidget *workspace = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); pack(layout, workspace, TRUE);
    GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); style(sidebar, "preferences-sidebar");
    gtk_widget_set_size_request(sidebar, 200, -1); pack(workspace, sidebar, FALSE);
    prefs->search = named(gtk_search_entry_new(), "preferences-search");
    gtk_entry_set_placeholder_text(GTK_ENTRY(prefs->search), "Ayar ara…"); pack(sidebar, prefs->search, FALSE);
    prefs->categories = named(gtk_list_box_new(), "preferences-categories");
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(prefs->categories), GTK_SELECTION_SINGLE);
    gtk_list_box_set_filter_func(GTK_LIST_BOX(prefs->categories), category_filter, prefs, NULL);
    GtkWidget *category_scroll = named(gtk_scrolled_window_new(NULL, NULL), "preferences-categories-scroll");
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(category_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(category_scroll), prefs->categories); pack(sidebar, category_scroll, TRUE);
    for (guint i = 0; i < G_N_ELEMENTS(category_names); ++i) {
        GtkWidget *row = gtk_list_box_row_new(), *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gchar *name = g_strdup_printf("preferences-category-%s", category_ids[i]); named(row, name); g_free(name);
        g_object_set_data(G_OBJECT(row), "category", GUINT_TO_POINTER(i));
        GtkWidget *icon = gtk_drawing_area_new(); gtk_widget_set_size_request(icon, 28, 30);
        g_signal_connect(icon, "draw", G_CALLBACK(icon_draw), GUINT_TO_POINTER(i));
        pack(box, icon, FALSE); pack(box, label_new(category_labels[i]), TRUE); gtk_container_add(GTK_CONTAINER(row), box);
        gtk_container_add(GTK_CONTAINER(prefs->categories), row);
    }
    g_signal_connect(prefs->search, "search-changed", G_CALLBACK(search_changed), prefs);
    GtkWidget *main = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0); pack(workspace, main, TRUE);
    prefs->content = named(gtk_stack_new(), "preferences-content"); pack(main, prefs->content, TRUE);
    prefs->notebook = named(gtk_notebook_new(), "preferences-notebook");
    /* Routing fields must exist before Configuration connects its device mirror. */
    GtkWidget *routing = routing_new(prefs), *configuration = configuration_new(prefs);
    gtk_notebook_append_page(GTK_NOTEBOOK(prefs->notebook), configuration, gtk_label_new("Yapılandırma"));
    gtk_notebook_append_page(GTK_NOTEBOOK(prefs->notebook), routing, gtk_label_new("Giriş / Çıkış"));
    gtk_stack_add_named(GTK_STACK(prefs->content), prefs->notebook, "audio");
    for (guint i = 0; i < G_N_ELEMENTS(category_names); ++i)
        if (i != CATEGORY_AUDIO) gtk_stack_add_named(GTK_STACK(prefs->content), category_page_new(prefs, i), category_ids[i]);
    GtkWidget *footer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6); style(footer, "preferences-footer");
    GtkWidget *reset = named(gtk_button_new_with_label("Ses ayarlarını sıfırla"), "preferences-reset");
    gtk_widget_set_halign(reset, GTK_ALIGN_START); pack(footer, reset, FALSE);
    prefs->status = named(label_new(""), "preferences-status");
    gtk_label_set_line_wrap(GTK_LABEL(prefs->status), TRUE);
    gtk_label_set_line_wrap_mode(GTK_LABEL(prefs->status), PANGO_WRAP_WORD_CHAR);
    gtk_label_set_max_width_chars(GTK_LABEL(prefs->status), 72); pack(footer, prefs->status, FALSE); pack(main, footer, FALSE);
    g_signal_connect(reset, "clicked", G_CALLBACK(reset_clicked), prefs);
    g_signal_connect(prefs->categories, "row-selected", G_CALLBACK(category_selected), prefs);
    gtk_list_box_select_row(GTK_LIST_BOX(prefs->categories), gtk_list_box_get_row_at_index(GTK_LIST_BOX(prefs->categories), CATEGORY_AUDIO));
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
