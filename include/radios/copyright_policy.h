#ifndef RADIOS_COPYRIGHT_POLICY_H
#define RADIOS_COPYRIGHT_POLICY_H
#include "radios/audio_runner.h"
#include <json-glib/json-glib.h>

/* Pure catalogue matching. Nonempty selectors in one rule are AND; matching
 * enabled rules intersect their UTC validity intervals. Unmatched media follow
 * the configured fallback, which defaults to allow in the operator settings. */
gboolean radios_copyright_policy_validate(JsonObject *policy, GError **error);
RadioSAudioCopyrightWindow radios_copyright_policy_window(JsonObject *policy,
    const gchar *artist, const gchar *album, const gchar *title);
gboolean radios_copyright_window_allowed(const RadioSAudioCopyrightWindow *window,
    gint64 utc_us);
#endif
