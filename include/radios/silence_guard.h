#ifndef RADIOS_SILENCE_GUARD_H
#define RADIOS_SILENCE_GUARD_H
#include "radios/views.h"
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* Dead-air protection for the lifetime of the application window (design
 * AC-09). Every 100 ms the MAIN output level is read from the audio engine
 * and fed to the silence detector. The settings are the Emergency page's
 * "Sessizlikte otomatik devralma" fields, read from the saved emergency
 * document and refreshed with the data views. Step 1 and step 2 run the
 * configured action (alarm only, next deck of the active AutoDJ queue, or
 * the saved emergency list), notify the operator and record a diagnostic
 * incident; recovery closes the incident. */
void radios_silence_guard_bind(RadioSViewContext *ctx, GtkWidget *window);

/* The current line for pages and the last measured levels (dBFS). FALSE
 * when no guard is bound to the context's window. */
gboolean radios_silence_guard_status(RadioSViewContext *ctx, gchar **line, gdouble *left_db, gdouble *right_db);

/* Applies emergency preparation fields now. document_id/revision identify
 * the saved emergency document the "Acil liste" step starts; NULL/0 when
 * nothing is saved. Invalid fields disable the guard and report why. */
gboolean radios_silence_guard_apply_preparation(RadioSViewContext *ctx, JsonObject *preparation,
                                                const gchar *document_id, gint64 revision, GError **error);

/* Test hook: a level source instead of the audio engine. FALSE from the
 * source means the output is not measurable. NULL restores the engine. */
typedef gboolean (*RadioSSilenceLevelSource)(gpointer user, gdouble *left_linear, gdouble *right_linear);
void radios_silence_guard_set_level_source(RadioSViewContext *ctx, RadioSSilenceLevelSource source, gpointer user);

G_END_DECLS
#endif
