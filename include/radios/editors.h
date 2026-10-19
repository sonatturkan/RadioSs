#ifndef RADIOS_EDITORS_H
#define RADIOS_EDITORS_H
#include "radios/views.h"
#include "radios/rhythm.h"
#include <json-glib/json-glib.h>

typedef enum {
    RADIOS_EDITOR_TRANSITION,
    RADIOS_EDITOR_MARKERS
} RadioSEditorKind;

/* Without a selected media version these editors manage preparation drafts.
 * The media variant uses measured analysis and saves the version's marker set.
 * Detail is borrowed; the editor retains its own independent JSON copy. */
void radios_transition_editor_open(RadioSViewContext *ctx);
void radios_marker_editor_open(RadioSViewContext *ctx);
void radios_marker_editor_open_media(RadioSViewContext *ctx, JsonObject *detail);
/* Shared measured waveform editor for library and playlist media. The numeric
 * backing fields stay hidden; crop, AutoDJ and overlap controls edit versioned
 * marker drafts. Preview belongs to owner; saves belong to the returned panel. */
GtkWidget *radios_marker_editor_embed(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail);
gboolean radios_marker_editor_dirty(GtkWidget *panel);
/* Borrowed latest detail, including the media revision advanced by marker saves. */
JsonObject *radios_marker_editor_detail(GtkWidget *panel);
/* A deck inspector shows its beat grid on the timeline (bar lines, beat lines
 * when zoomed in); NULL clears it. With snapping on, AutoDJ IN/OUT placement
 * rounds to the nearest bar start inside the legal interval. */
void radios_marker_editor_set_grid(GtkWidget *panel, const RadioSRhythmAnalysis *grid);
void radios_marker_editor_set_snap(GtkWidget *panel, gboolean snap);
/* Timeline position in seconds; FALSE without a measured duration. */
gboolean radios_marker_editor_cursor(GtkWidget *panel, gdouble *seconds);
/* Clears AutoDJ IN/OUT as an unsaved edit that Geri al restores. */
void radios_marker_editor_clear_autodj(GtkWidget *panel);
/* Called once a media marker save is acknowledged; detail is borrowed. */
typedef void (*RadioSMarkerEditorSaved)(GtkWidget *panel, JsonObject *detail, gpointer user);
void radios_marker_editor_set_saved(GtkWidget *panel, RadioSMarkerEditorSaved saved, gpointer user);
void radios_ui_bind_editor(GtkWidget *button, RadioSViewContext *ctx, RadioSEditorKind kind);
#endif
