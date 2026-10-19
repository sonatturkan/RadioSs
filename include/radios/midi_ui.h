#ifndef RADIOS_MIDI_UI_H
#define RADIOS_MIDI_UI_H
#include "radios/data.h"

/* MIDI controller service of the main window: loads the station's
 * "midi_mapping" document, connects to its device (also when it is plugged in
 * later) and runs the active map's actions on the player cards. */
void radios_midi_ui_bind(RadioSViewContext *ctx, GtkWidget *window);
/* Settings page: device, create / load map, and every known controller key
 * with its assignment ("Atanmamış" when free). Pressing a key selects it. */
GtkWidget *radios_midi_ui_settings_new(RadioSViewContext *ctx);
/* Operator wording of an action id such as "deck1.playpause". */
gchar *radios_midi_ui_action_label(const gchar *action);
/* Technical page: `maps` (a GtkComboBoxText) lists the saved maps and
 * activates the chosen one; `verify` opens a live controller check during
 * which presses are shown but never run their actions. */
void radios_midi_ui_bind_technical(RadioSViewContext *ctx, GtkWidget *maps, GtkWidget *verify);
/* Shows the active map again, e.g. after a form reset; nothing is saved. */
void radios_midi_ui_technical_refresh(RadioSViewContext *ctx);
#endif
