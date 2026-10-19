#ifndef RADIOS_PLAYER_TREE_UI_H
#define RADIOS_PLAYER_TREE_UI_H

#include "radios/player_workspace.h"

G_BEGIN_DECLS
/* scope is borrowed during the callback. NULL identifies optional Genel yayın.
 * Kümeli is an expandable container; only its clusters and players are selectable.
 * Selection and definition edits never start audio. All calls use GTK thread. */
typedef void (*RadioSPlayerTreeSelection)(const RadioSAutomationScope *scope, gpointer user);
GtkWidget *radios_player_tree_ui_new(RadioSViewContext *ctx, gboolean manage,
    gboolean include_general, RadioSPlayerTreeSelection callback, gpointer user);
/* TRUE means a row is selected; general is TRUE only for the optional root.
 * When general is FALSE, scope receives the selected stable player/group. */
gboolean radios_player_tree_ui_selection(GtkWidget *widget,
    RadioSAutomationScope *scope, gboolean *general);
/* NULL selects Genel yayın when enabled. FALSE if that scope has no row yet. */
gboolean radios_player_tree_ui_select(GtkWidget *widget, const RadioSAutomationScope *scope);
/* Rebuilds only when persisted/local topology changes, preserves stable row
 * selection and expanded folders, and notifies when the selected scope or topology changes. */
void radios_player_tree_ui_refresh(GtkWidget *widget);
G_END_DECLS
#endif
