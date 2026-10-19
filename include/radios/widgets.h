#ifndef RADIOS_WIDGETS_H
#define RADIOS_WIDGETS_H
#include "radios/views.h"
#include <json-glib/json-glib.h>
GtkWidget *radios_ui_label(const gchar *text, const gchar *class_name);
GtkWidget *radios_ui_box(GtkOrientation orientation, gint spacing, const gchar *class_name);
GtkWidget *radios_ui_button(const gchar *text);
GtkWidget *radios_ui_entry(const gchar *placeholder);
GtkWidget *radios_ui_combo(const gchar *const *items, gint active);
GtkWidget *radios_ui_panel(const gchar *title, GtkWidget **body);
GtkWidget *radios_ui_table(const gchar *const *headers, guint count);
void radios_ui_bind_notice(GtkWidget *button, RadioSViewContext *ctx, const gchar *message);
void radios_ui_form_open(RadioSViewContext *ctx, const gchar *title, const gchar *const *fields);
void radios_ui_bind_form(GtkWidget *button, RadioSViewContext *ctx, const gchar *title, const gchar *const *fields);
/* Mixer channel picker (kılavuz s.4 adım 5): a summary button ("N kanal seçili")
 * whose popover tree groups the defined strips by DJ / Konuşma / Müzik /
 * Etiketsiz label. channels: [{number 1..32, name, label "dj"|"talk"|"music"|"",
 * source}] of the defined strips (borrowed, referenced). Selection is a list of
 * numbers 1..32; setters never call changed; a saved but undefined channel
 * stays listed under "Kayıtlı · tanımsız". toggle acts like the user's click. */
GtkWidget *radios_ui_channel_picker_new(const gchar *name);
void radios_ui_channel_picker_set_channels(GtkWidget *picker, JsonArray *channels);
void radios_ui_channel_picker_set_selected(GtkWidget *picker, JsonArray *numbers);
JsonArray *radios_ui_channel_picker_selected(GtkWidget *picker);
const gchar *radios_ui_channel_picker_agreed_label(GtkWidget *picker);
void radios_ui_channel_picker_toggle(GtkWidget *picker, guint number);
void radios_ui_channel_picker_set_changed(GtkWidget *picker, void (*changed)(GtkWidget *picker, gpointer user), gpointer user);

/* Family list for text drawn with Pango on a cairo context (DirectWrite family
 * names, same order as the theme CSS font-family lists): the registered Inter
 * TTF first, then Windows fonts for the symbols Inter lacks. */
#define RADIOS_UI_DRAWN_FONT_FAMILIES \
    "Inter Variable Text, Inter Variable, Inter, Segoe UI Symbol, Segoe UI Emoji, Segoe UI, Nirmala UI, sans-serif"
/* RADIOS_UI_DRAWN_FONT_FAMILIES at an absolute size in device pixels, so drawn
 * labels keep the proportions of the px-sized CSS UI at every Windows DPI
 * (point sizes would grow with the DPI while their boxes do not). */
PangoFontDescription *radios_ui_drawn_font_new(gdouble pixel_size, PangoWeight weight);
/* cairo_show_text() replacement for drawn labels: lays text out with
 * RADIOS_UI_DRAWN_FONT_FAMILIES at pixel_size (user-space units of cr, so the
 * current transformation applies as for cairo text) and, unless measure_only,
 * draws it with its baseline at the current point and advances the current
 * point by the width like cairo_show_text().  The cairo "toy" font API picks a
 * single GDI face on Windows (Arial for "sans-serif") with no glyph fallback
 * and never sees the registered Inter font.  Returns the logical width. */
gdouble radios_ui_cairo_show_text(cairo_t *cr, const gchar *text, gdouble pixel_size, PangoWeight weight, gboolean measure_only);
/* Adds the audio formats of the media importer (*.wav *.mp3 *.flac *.ogg *.opus
 * *.m4a *.aac) as patterns.  Never use mime rules for files: Windows content
 * types are file extensions, so an "audio" mime rule matches nothing there
 * (and keeps GtkFileChooserButton off the native Windows dialog).  Patterns
 * match case-insensitively on Windows. */
void radios_ui_file_filter_add_audio(GtkFileFilter *filter);
/* Save dialogs without a folder would open in the process working directory
 * (for a Start-menu launch the program's own bin directory): start them in
 * the project root like the other RadioS export dialogs. */
void radios_ui_file_chooser_default_folder(GtkFileChooser *chooser, RadioSViewContext *ctx);
/* A name for gtk_file_chooser_set_current_name() that is one valid Windows
 * file name: \ / : * ? " < > | and control characters become '_', trailing
 * dots and spaces are dropped and reserved device names (CON, NUL, COM1...)
 * get a leading '_'.  Returns a copy of fallback when nothing usable remains. */
gchar *radios_ui_file_name_suggestion(const gchar *name, const gchar *fallback);
#endif
