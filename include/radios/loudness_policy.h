#ifndef RADIOS_LOUDNESS_POLICY_H
#define RADIOS_LOUDNESS_POLICY_H
#include <gio/gio.h>
#include <json-glib/json-glib.h>

G_BEGIN_DECLS
#define RADIOS_AUDIO_LOUDNESS_MAX_AUTOMATIONS 64u
#define RADIOS_AUDIO_LOUDNESS_MAX_WINDOWS 16u
#define RADIOS_AUDIO_LOUDNESS_ALL_PLAYERS G_MAXUINT32
#define RADIOS_AUDIO_LOUDNESS_ALL_CONTEXTS 7u
typedef enum {
    RADIOS_AUDIO_LOUDNESS_MANUAL = 1,
    RADIOS_AUDIO_LOUDNESS_AUTOMATION = 2,
    RADIOS_AUDIO_LOUDNESS_AUTODJ = 4
} RadioSAudioLoudnessContext;
typedef enum {
    RADIOS_AUDIO_LOUDNESS_PHASE_ALWAYS,
    RADIOS_AUDIO_LOUDNESS_PHASE_TRANSITION,
    RADIOS_AUDIO_LOUDNESS_PHASE_STEADY
} RadioSAudioLoudnessPhase;
typedef enum {
    RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_ALL,
    RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_ASSIGNED,
    RADIOS_AUDIO_LOUDNESS_AUTOMATIONS_SELECTED
} RadioSAudioLoudnessAutomationMode;
typedef enum {
    RADIOS_AUDIO_LOUDNESS_PAIRS_ALL,
    RADIOS_AUDIO_LOUDNESS_PAIRS_SELECTED
} RadioSAudioLoudnessPairsMode;
typedef struct { guint days, start_minute, end_minute; } RadioSAudioLoudnessWindow;
typedef struct {
    guint32 players; /* Engine slots0..31; JSON player numbers1..32. */
    guint contexts;
    RadioSAudioLoudnessPhase phase;
    RadioSAudioLoudnessAutomationMode automation_mode;
    guint automation_count;
    gchar automation_ids[RADIOS_AUDIO_LOUDNESS_MAX_AUTOMATIONS][37];
    RadioSAudioLoudnessPairsMode pairs_mode;
    guint32 pairs[32]; /* Directed source row -> target bits; self pairs invalid. */
    gboolean schedule_enabled;
    gchar timezone[128];
    guint window_count;
    RadioSAudioLoudnessWindow windows[RADIOS_AUDIO_LOUDNESS_MAX_WINDOWS];
} RadioSAudioLoudnessPolicy;
typedef struct {
    guint deck, context;
    gboolean transition, assigned;
    const gchar *automation_id; /* immutable accepted origin; empty for manual */
    gint source, target; /* actual directed transition pair, -1 when absent */
} RadioSAudioLoudnessExecution;
void radios_audio_loudness_policy_default(RadioSAudioLoudnessPolicy *policy);
gboolean radios_audio_loudness_policy_validate(const RadioSAudioLoudnessPolicy *policy, GError **error);
gboolean radios_audio_loudness_policy_equal(const RadioSAudioLoudnessPolicy *a, const RadioSAudioLoudnessPolicy *b);
gboolean radios_audio_loudness_policy_from_json(RadioSAudioLoudnessPolicy *policy, JsonObject *object, GError **error);
JsonObject *radios_audio_loudness_policy_to_json(const RadioSAudioLoudnessPolicy *policy);
gboolean radios_audio_loudness_policy_unrestricted(const RadioSAudioLoudnessPolicy *policy);
/* Allocation-free matching. Monday=1..Sunday=7; overnight windows belong to
 * their start weekday, and equal endpoints mean that entire selected day. */
gboolean radios_audio_loudness_policy_time_matches(const RadioSAudioLoudnessPolicy *policy, guint weekday, guint minute);
gboolean radios_audio_loudness_policy_matches(const RadioSAudioLoudnessPolicy *policy,
    const RadioSAudioLoudnessExecution *execution, gboolean time_matches);
G_END_DECLS
#endif
