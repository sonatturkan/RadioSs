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
#endif
