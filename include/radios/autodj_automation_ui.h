#ifndef RADIOS_AUTODJ_AUTOMATION_UI_H
#define RADIOS_AUTODJ_AUTOMATION_UI_H

#include "radios/views.h"
#include <json-glib/json-glib.h>

G_BEGIN_DECLS
/* "AutoDJ otomasyonu" page of the rotation editor: the rotation's AutoDJ
 * transitions in one place. Its "Türler" section holds one row per content
 * kind: the kind's AutoDJ rule (transition type, length, SYNC probability,
 * players: kind_transitions/autodj_players, built and saved by the parent
 * through kind_policy) and the kind's designed presets with their rotation.
 * Below follow the designed transition presets (volume/bass/tone curves plus
 * one mixer effect) and their clock/day schedules. The parent owns
 * persistence: changed() asks it to re-read the page into the rotation
 * settings ("autodj_presets", "autodj_automation"). */
typedef struct {
    void (*changed)(GtkWidget *page, gpointer user);
    gpointer user;
    /* Unused since the kind rule is part of this page; kept so callers built
     * against the earlier contract still compile. */
    void (*open_kind_policy)(GtkWidget *page, gpointer user);
    /* Optional. Opens the probability window ("Olasılık ve geçişler…"). While
     * NULL the page clicks the rotation editor's footer button of that name
     * ("rotation-editor-probability") found in its own window. */
    void (*open_probability)(GtkWidget *page, gpointer user);
    /* Optional. Returns the parent's per-kind AutoDJ rule: a switch, then a
     * grid with its header in row 0 and one row per kind. The page shows it
     * in "Türler" and aligns its preset column with those rows; the parent
     * keeps reading, checking and writing it, the page never copies it. */
    GtkWidget *(*kind_policy)(GtkWidget *page, gpointer user);
    /* Optional. TRUE once the planner and the data service read kind entries
     * of settings.transitions and the parent's rule keeps only the players:
     * each "Türler" row then edits the kind's "Geçişler" distribution (types
     * and designed presets with shares, the rest to the base), read() writes
     * those entries and drops the fields they replace (kind_transitions and
     * the kinds' preset lists), as the kılavuz 18.10 contract asks. */
    gboolean kind_distributions;
} RadioSAutodjAutomationHooks;

GtkWidget *radios_autodj_automation_new(RadioSViewContext *ctx, const RadioSAutodjAutomationHooks *hooks);
/* Loads the saved members; built-in templates appear when none are saved.
 * Never emits changed(). */
void radios_autodj_automation_render(GtkWidget *page, JsonObject *settings);
/* Writes both members once the page has been edited or the settings already
 * carry them; untouched rotations keep their previous settings unchanged. */
void radios_autodj_automation_read(GtkWidget *page, JsonObject *settings);
/* FALSE with an owned Turkish message while the visible draft is invalid. */
gboolean radios_autodj_automation_valid(GtkWidget *page, gchar **message);
/* TRUE when the enabled automation gives this kind at least one preset,
 * directly or through a schedule row. */
gboolean radios_autodj_automation_kind_uses_presets(GtkWidget *page, const gchar *kind);
/* Brings the "Türler" section (rules, players, presets per kind) into view
 * once the page is shown; the footer's "AutoDJ: geçiş ve kanallar…" uses it. */
void radios_autodj_automation_show_kind_policy(GtkWidget *page);
G_END_DECLS
#endif
