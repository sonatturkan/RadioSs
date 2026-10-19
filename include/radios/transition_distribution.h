#ifndef RADIOS_TRANSITION_DISTRIBUTION_H
#define RADIOS_TRANSITION_DISTRIBUTION_H

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS
/* "Geçiş dağılımı": shares of the transition types (Parça sonunda, Crossfade,
 * Cue, SYNC beatmatch) and, for a content kind, of the designed presets, kept
 * in settings.transitions (kılavuz 18.10 contract, B/f6 + C8):
 *  - kind NULL, the rotation's base: entries {mode, weight 0..100, overlap_ms,
 *    effect?:"lowpass"} without "kind" (1..4, distinct modes, Σweight > 0,
 *    shared out by weight, or taken in turn: transition_selection_mode);
 *  - a kind (music, advertisement, jingle, sweeper): {kind, mode, weight
 *    1..100, overlap_ms, effect?} and {kind, preset_id, mode:1, overlap_ms:0,
 *    weight 1..100}; at most 100 in all, drawn per boundary; the rest
 *    ("Kalan pay") follows the base. Preset rows act only while the designed
 *    presets are on (autodj_automation.enabled); otherwise their share is rest.
 * The owner persists: read() replaces only this scope's entries. */
typedef struct {
    void (*changed)(GtkWidget *distribution, gpointer user);
    gpointer user;
    /* Optional: opens the rotation's base distribution from a kind's "Kalan pay". */
    void (*open_base)(GtkWidget *distribution, gpointer user);
} RadioSTransitionDistributionHooks;

/* compact: a menu button whose label sums the shares up and whose popover
 * holds the table (a "Türler" row); otherwise the table itself with its
 * "Geçiş dağılımı" heading (the base on the probability page). */
GtkWidget *radios_transition_distribution_new(const gchar *kind, gboolean compact,
    const RadioSTransitionDistributionHooks *hooks);
/* Loads this scope from settings ("transitions", "transition_selection_mode",
 * "autodj_presets", "autodj_automation"). A kind without entries of its own
 * shows its earlier rule converted (kind_transitions.<kind>, or its designed
 * presets in equal shares) and reports it as pending. Never emits changed(). */
void radios_transition_distribution_render(GtkWidget *distribution, JsonObject *settings);
/* The designed presets offered as rows (kind scope) and whether they act;
 * shares of presets that no longer exist are dropped. Never emits changed(). */
void radios_transition_distribution_set_presets(GtkWidget *distribution, JsonArray *presets, gboolean enabled);
/* Writes this scope into settings.transitions, keeping every other entry;
 * the base also writes transition_selection_mode. */
void radios_transition_distribution_read(GtkWidget *distribution, JsonObject *settings);
/* FALSE with an owned Turkish message while the shown shares cannot be saved. */
gboolean radios_transition_distribution_valid(GtkWidget *distribution, gchar **message);
/* TRUE once edited, or while it shows an earlier rule converted. */
gboolean radios_transition_distribution_pending(GtkWidget *distribution);
/* Entries this scope would write (0: a kind follows the base). */
guint radios_transition_distribution_count(GtkWidget *distribution);
/* TRUE when this kind draws at least one designed preset while presets act. */
gboolean radios_transition_distribution_uses_presets(GtkWidget *distribution);
G_END_DECLS
#endif
