#ifndef RADIOS_PLAYER_WAVEFORM_H
#define RADIOS_PLAYER_WAVEFORM_H

#include "radios/audio_ui.h"

/* Compact view of the database's measured waveform and version-bound cue
 * markers, with zoom, horizontal inspection and a larger grid/marker editor.
 * The returned overlay owns the existing deck-N-waveform drawing area.
 * Clicking selects a position; AutoDJ IN/OUT actions explicitly save it with
 * media and marker revision checks. Inspection never seeks audio. The card owns the media
 * detail cache and must outlive the view. */
GtkWidget *radios_player_waveform_new(RadioSViewContext *ctx, GtkWidget *card, guint deck);
void radios_player_waveform_update(GtkWidget *wave, const RadioSAudioUiSnapshot *snapshot,
                                  gboolean online);
/* Opens the inspection window on its BPM / bar grid editor. FALSE until the
 * deck's measured waveform has loaded. Saving writes a manual grid that
 * automatic analysis never replaces, and applies it to the loaded deck. */
gboolean radios_player_waveform_edit_grid(GtkWidget *wave);

#endif
