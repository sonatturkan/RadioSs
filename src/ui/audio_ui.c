#include "radios/media_geometry.h"
#include "radios/audio_ui.h"
#include "radios/audio.h"
#include "radios/audio_transition.h"
#include "radios/programme_model.h"
#include "radios/player_binding.h"
#include "radios/station_time.h"
#include "radios/platform.h"
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

typedef enum { BIND_METER, BIND_GAIN, BIND_MONITOR, BIND_SETTINGS } BindingKind;
typedef struct { GWeakRef object; gulong handler; } SignalLink;
typedef struct {
    RadioSAudioController *audio;
    BindingKind kind;
    RadioSAudioUiBus bus;
    GWeakRef widget, control, destination, toggle;
    gboolean dead;
    guint device_hash;
    GPtrArray *signals;
} AudioBinding;
typedef struct {
    GWeakRef owner;
    gulong owner_handler;
    guint64 epoch, generation, duration_ms, loss_epoch;
    gchar title[801], artist[801], album[801], kind[41];
    RadioSAudioPlayerOptions player_options;
    gboolean autoplay;
} UiDeck;
typedef struct {
    RadioSAudioController *audio;
    GWeakRef owner;
    gulong owner_handler;
    RadioSAudioUiBus bus;
    gdouble gain_db;
} PreviewOutput;
/* Per-channel data beside the DSP configuration (R5.1/R5.2): the broadcast
 * label and the live input. The DSP struct belongs to the audio core and
 * stays untouched; these travel in the same dj_mixer document. */
typedef struct {
    gchar label[8]; /* "", "dj", "talk" or "music" */
    RadioSAudioChannelInputSpec input; /* kind NONE when the channel uses a player */
    RadioSAudioChannelInput *runner; /* live while the controller is open */
} ChannelExtra;
struct RadioSAudioController {
    gint references;
    RadioSViewContext *ctx;
    RadioSAudioEngine *engine;
    RadioSAudioPreferences preferences;
    ChannelExtra extras[RADIOS_AUDIO_MIXER_CHANNELS];
    RadioSAudioUiNetworkSource network[RADIOS_AUDIO_UI_NETWORK_SOURCES];
    guint network_count;
    GWeakRef parent;
    GPtrArray *bindings;
    GPtrArray *preview_outputs;
    RadioSDocument *document, *mixer_document;
    GCancellable *transition_cancel;
    GWeakRef transition_owner;
    gulong transition_handler;
    guint64 preview_intent;
    guint64 loss_epoch;
    gchar *preview_file;
    UiDeck decks[RADIOS_AUDIO_DECKS];
    guint timer, autosave_timer, preference_changes;
    gboolean closed, preparing_close, updating, settings_dirty, mixer_dirty, preferences_dirty;
    gboolean preview_active, preview_pause_pending, mixer_initialized, preferences_initialized;
};
typedef struct { RadioSAudioController *audio; guint deck; guint64 epoch; } OwnerGuard;
static void binding_update(AudioBinding *binding, const RadioSAudioSnapshot *snapshot, gboolean force);
/* Windows cannot delete a file that another handle holds open without
 * FILE_SHARE_DELETE, and an unloaded preview deck's decoder may still be
 * closing its WAV: retry on the main loop (every 200 ms, up to 10 s) instead
 * of leaving transient preview PCM in the cache. */
typedef struct { gchar *path; guint attempts; } PreviewDelete;
static gboolean preview_delete_retry(gpointer value)
{
    PreviewDelete *pending = value;
    if (g_unlink(pending->path) == 0 || errno == ENOENT || ++pending->attempts >= 50) {
        if (pending->attempts >= 50) g_warning("Önizleme dosyası silinemedi: %s", pending->path);
        g_free(pending->path); g_free(pending); return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}
static void preview_file_delete(const gchar *path)
{
    if (!path || g_unlink(path) == 0 || errno == ENOENT) return;
    PreviewDelete *pending = g_new0(PreviewDelete, 1); pending->path = g_strdup(path);
    g_timeout_add(200, preview_delete_retry, pending);
}
/* A snapshot holds 2 x 64 device names of 513 bytes (~130 KiB): it lives on
 * the heap, never on a thread stack. */
static RadioSAudioSnapshot *snapshot_take(RadioSAudioController *audio)
{
    RadioSAudioSnapshot *snapshot = g_new(RadioSAudioSnapshot, 1);
    radios_audio_engine_snapshot(audio->engine, snapshot); return snapshot;
}
static void mixer_loaded(RadioSViewContext *, GtkWidget *, JsonObject *, gpointer);
static void transition_cancel(RadioSAudioController *audio);
static void autosave_schedule(RadioSAudioController *audio, guint delay_ms);
static void preview_output_free(gpointer value);
static JsonObject *read_object(JsonObject *o, const gchar *key);
static JsonArray *read_array(JsonObject *o, const gchar *key);
static gboolean bus_ready(RadioSAudioController *audio, const RadioSAudioSnapshot *snapshot, RadioSAudioUiBus bus)
{
    if (bus == RADIOS_AUDIO_UI_MICROPHONE) return snapshot->microphone_ready;
    if (bus < RADIOS_AUDIO_UI_MAIN || bus > RADIOS_AUDIO_UI_MONITOR) return FALSE;
    const gchar *wanted = bus == RADIOS_AUDIO_UI_MAIN ? audio->preferences.main_sink :
        bus == RADIOS_AUDIO_UI_CUE ? audio->preferences.cue_sink : audio->preferences.monitor_sink;
    return wanted[0] && snapshot->buses[bus].ready &&
        (g_str_equal(wanted, "@default") || g_str_equal(wanted, snapshot->buses[bus].device));
}
static gboolean deck_output_ready(RadioSAudioController *audio, const RadioSAudioSnapshot *snapshot, guint deck)
{
    gint bus = snapshot->decks[deck].output_bus;
    return bus >= 0 && bus <= 2 && bus_ready(audio, snapshot, (RadioSAudioUiBus)bus);
}
static void owner_clear(UiDeck *deck)
{
    GObject *owner = g_weak_ref_get(&deck->owner);
    gulong handler = deck->owner_handler; deck->owner_handler = 0;
    if (owner) {
        if (handler && g_signal_handler_is_connected(owner, handler)) g_signal_handler_disconnect(owner, handler);
        g_object_unref(owner);
    }
    g_weak_ref_set(&deck->owner, NULL);
}
static void notify_audio(RadioSAudioController *audio, const gchar *text)
{ if (!audio->closed && audio->ctx && audio->ctx->notify) audio->ctx->notify(audio->ctx->notify_data, text); }
static gboolean unavailable(GError **error)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "Ses motoru hazır değil."); return FALSE; }
static RadioSAudioController *controller(RadioSViewContext *ctx)
{ return ctx && ctx->audio && !ctx->audio->closed ? ctx->audio : NULL; }
RadioSAudioEngine *radios_audio_ui_engine_acquire(RadioSViewContext *ctx)
{ RadioSAudioController *audio = controller(ctx); return audio ? radios_audio_engine_ref(audio->engine) : NULL; }
void radios_audio_ui_runner_duration(RadioSViewContext *ctx, guint deck, guint64 duration_ms)
{ RadioSAudioController *audio = controller(ctx); if (audio && deck < RADIOS_AUDIO_NORMAL_DECKS) audio->decks[deck].duration_ms = duration_ms; }
static void mark_dirty(RadioSAudioController *audio)
{ if (radios_data_document_ready(audio->document)) radios_data_document_dirty(audio->document, TRUE); }
/* Applied controls may change before the first asynchronous preferences read.
 * Keep that read available, then merge just the locally edited fields into its
 * baseline instead of replacing saved device selections with startup defaults. */
static void preferences_changed(RadioSAudioController *audio, guint fields)
{
    audio->preferences_dirty = TRUE; audio->preference_changes |= fields;
    mark_dirty(audio); autosave_schedule(audio, 250);
}
#define PREF_GAIN(bus) (1u << (bus))
#define PREF_CUE_SOURCE (1u << 4)
#define PREF_MONITOR_SOURCE (1u << 5)
#define PREF_MONITOR_SINK (1u << 6)
#define PREF_LOUDNESS (1u << 7)
#define PREF_LOUDNESS_POLICY (1u << 8)
static gdouble *gain_pointer(RadioSAudioPreferences *prefs, RadioSAudioUiBus bus)
{
    switch (bus) {
    case RADIOS_AUDIO_UI_MAIN: return &prefs->main_gain_db;
    case RADIOS_AUDIO_UI_CUE: return &prefs->cue_gain_db;
    case RADIOS_AUDIO_UI_MONITOR: return &prefs->monitor_gain_db;
    case RADIOS_AUDIO_UI_MICROPHONE: return &prefs->microphone_gain_db;
    }
    return NULL;
}
static gboolean preferences_equal(const RadioSAudioPreferences *a, const RadioSAudioPreferences *b)
{
    return g_str_equal(a->main_sink, b->main_sink) && g_str_equal(a->cue_sink, b->cue_sink) &&
        g_str_equal(a->monitor_sink, b->monitor_sink) && g_str_equal(a->microphone_source, b->microphone_source) &&
        a->main_gain_db == b->main_gain_db && a->cue_gain_db == b->cue_gain_db &&
        a->monitor_gain_db == b->monitor_gain_db && a->microphone_gain_db == b->microphone_gain_db &&
        a->cue_source == b->cue_source && a->monitor_source == b->monitor_source &&
        a->loudness_normalize == b->loudness_normalize && a->loudness_target_lufs == b->loudness_target_lufs &&
        radios_audio_loudness_policy_equal(&a->loudness_policy, &b->loudness_policy);
}
static gboolean apply_engine(RadioSAudioController *audio, GError **error)
{
    return radios_audio_engine_apply_preferences(audio->engine, &audio->preferences, error);
}
static void loudness_station_timezone(RadioSAudioController *audio, RadioSAudioLoudnessPolicy *policy)
{
    const gchar *zone_name = radios_station_timezone(audio->ctx);
    GTimeZone *zone = radios_time_zone_new(zone_name);
    if (zone && strlen(zone_name) < sizeof policy->timezone)
        g_strlcpy(policy->timezone, zone_name, sizeof policy->timezone);
    if (zone) g_time_zone_unref(zone);
}
static void loudness_empty_baseline_default(RadioSAudioController *audio)
{
    if (!audio->preferences_initialized && !(audio->preference_changes & PREF_LOUDNESS_POLICY) &&
        radios_data_document_ready(audio->document) && !*radios_data_document_id(audio->document))
        loudness_station_timezone(audio, &audio->preferences.loudness_policy);
}
static JsonNode *preferences_json(RadioSAudioController *audio)
{
    loudness_empty_baseline_default(audio);
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *object = json_object_new(); json_node_take_object(node, object);
    RadioSAudioPreferences *prefs = &audio->preferences;
    json_object_set_string_member(object, "main_sink", prefs->main_sink); json_object_set_string_member(object, "cue_sink", prefs->cue_sink);
    json_object_set_string_member(object, "monitor_sink", prefs->monitor_sink); json_object_set_string_member(object, "microphone_source", prefs->microphone_source);
    json_object_set_double_member(object, "main_gain_db", prefs->main_gain_db); json_object_set_double_member(object, "cue_gain_db", prefs->cue_gain_db);
    json_object_set_double_member(object, "monitor_gain_db", prefs->monitor_gain_db); json_object_set_double_member(object, "microphone_gain_db", prefs->microphone_gain_db);
    json_object_set_int_member(object, "cue_source", prefs->cue_source); json_object_set_int_member(object, "monitor_source", prefs->monitor_source);
    json_object_set_boolean_member(object, "loudness_normalize", prefs->loudness_normalize);
    json_object_set_double_member(object, "loudness_target_lufs", prefs->loudness_target_lufs);
    json_object_set_object_member(object, "loudness_policy", radios_audio_loudness_policy_to_json(&prefs->loudness_policy));
    JsonArray *players = json_array_new();
    for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) {
        RadioSAudioPlayerOptions options = audio->decks[deck].player_options;
        if (options.sync_master < 0 && options.autodj_target < 0) continue;
        JsonObject *row = json_object_new(); json_object_set_int_member(row, "player", deck + 1);
        json_object_set_int_member(row, "sync_master", options.sync_master + 1); json_object_set_int_member(row, "autodj_target", options.autodj_target + 1);
        json_array_add_object_element(players, row);
    }
    json_object_set_array_member(object, "player_automation", players); return node;
}
static gboolean preferences_read(JsonObject *object, RadioSAudioPreferences *prefs)
{
    radios_audio_preferences_default(prefs);
    gchar *destinations[] = {prefs->main_sink, prefs->cue_sink, prefs->monitor_sink, prefs->microphone_source};
    const gchar *names[] = {"main_sink", "cue_sink", "monitor_sink", "microphone_source"};
    for (guint i = 0; i < G_N_ELEMENTS(names); ++i) {
        const gchar *name = radios_json_string(object, names[i], NULL);
        if (!name || strlen(name) >= RADIOS_AUDIO_DEVICE_NAME_SIZE) return FALSE;
        g_strlcpy(destinations[i], name, RADIOS_AUDIO_DEVICE_NAME_SIZE);
    }
    const gchar *gains[] = {"main_gain_db", "cue_gain_db", "monitor_gain_db", "microphone_gain_db"};
    for (guint i = 0; i < G_N_ELEMENTS(gains); ++i) {
        JsonNode *value = json_object_get_member(object, gains[i]);
        if (!value || !JSON_NODE_HOLDS_VALUE(value) || (json_node_get_value_type(value) != G_TYPE_DOUBLE && json_node_get_value_type(value) != G_TYPE_INT64)) return FALSE;
        gdouble db = json_node_get_double(value); if (!isfinite(db) || db < -60 || db > (i == 3 ? 30 : 6)) return FALSE;
        *gain_pointer(prefs, (RadioSAudioUiBus)i) = db;
    }
    gint64 cue = radios_json_int(object, "cue_source", -2), monitor = radios_json_int(object, "monitor_source", -2);
    if (cue < -1 || cue > 32 || monitor < -1 || monitor > 32) return FALSE;
    prefs->cue_source = (gint)cue; prefs->monitor_source = (gint)monitor;
    /* Older documents have no loudness keys; they keep the defaults. */
    JsonNode *normalize = json_object_get_member(object, "loudness_normalize"), *target = json_object_get_member(object, "loudness_target_lufs");
    if (normalize) {
        if (!JSON_NODE_HOLDS_VALUE(normalize) || json_node_get_value_type(normalize) != G_TYPE_BOOLEAN) return FALSE;
        prefs->loudness_normalize = json_node_get_boolean(normalize);
    }
    if (target) {
        if (!JSON_NODE_HOLDS_VALUE(target) || (json_node_get_value_type(target) != G_TYPE_DOUBLE && json_node_get_value_type(target) != G_TYPE_INT64)) return FALSE;
        gdouble lufs = json_node_get_double(target); if (!isfinite(lufs) || lufs < -30 || lufs > -5) return FALSE;
        prefs->loudness_target_lufs = lufs;
    }
    JsonNode *policy = json_object_get_member(object, "loudness_policy");
    if (policy && (!JSON_NODE_HOLDS_OBJECT(policy) ||
        !radios_audio_loudness_policy_from_json(&prefs->loudness_policy, json_node_get_object(policy), NULL))) return FALSE;
    return TRUE;
}
static JsonObject *fx_json(const RadioSAudioFx *fx)
{
    JsonObject *o = json_object_new();
    json_object_set_int_member(o, "type", fx->type); json_object_set_boolean_member(o, "enabled", fx->enabled);
    json_object_set_double_member(o, "depth", fx->depth); json_object_set_double_member(o, "send_db", fx->send_db);
    json_object_set_double_member(o, "return_db", fx->return_db); json_object_set_double_member(o, "tempo_bpm", fx->tempo_bpm);
    json_object_set_int_member(o, "monitor_path", fx->monitor_path); return o;
}
/* Keep the five-slot wire format readable by older services, while the DJ
 * exposes exactly four pedals. The retired slot must never remain audible
 * through a saved channel, MASTER route or live CUE selection. */
static void mixer_retire_fifth_effect(RadioSAudioMixerConfig *cfg)
{
    if (cfg->speech_fx_slot == 4) cfg->speech_fx_slot = -1;
    cfg->effects[4].enabled = FALSE;
    cfg->effects[4].cue = FALSE;
    for (guint i = 0; i < RADIOS_AUDIO_MIXER_CHANNELS; ++i) {
        RadioSAudioChannelDsp *channel = &cfg->channels[i];
        if (channel->fx_slot == 4) channel->fx_slot = -1;
        if (channel->return_effect == -2) {
            if (channel->fx_slot >= 0) channel->return_db = channel->fx_return_db;
            channel->return_effect = channel->fx_slot;
        }
        if (channel->return_effect == 4) channel->return_effect = -1;
        channel->return_channel = -1;
    }
    if (cfg->master.fx_slot == 4) cfg->master.fx_slot = -1;
    if (cfg->master.return_effect == -2) {
        if (cfg->master.fx_slot >= 0) cfg->master.return_db = cfg->master.fx_return_db;
        cfg->master.return_effect = cfg->master.fx_slot;
    }
    if (cfg->master.return_effect == 4) cfg->master.return_effect = -1;
    cfg->master.return_channel = -1;
}
static const gchar *const channel_labels[] = {"dj", "talk", "music"};
static gboolean label_known(const gchar *label)
{
    for (guint i = 0; label && i < G_N_ELEMENTS(channel_labels); ++i) if (g_str_equal(label, channel_labels[i])) return TRUE;
    return FALSE;
}
/* extras NULL writes the DSP configuration alone (used for change detection);
 * the controller adds labels, inputs and the saved network sources. */
static JsonNode *mixer_json(const RadioSAudioMixerConfig *configuration, const RadioSAudioController *extras)
{
    RadioSAudioMixerConfig safe = *configuration;
    mixer_retire_fifth_effect(&safe);
    const RadioSAudioMixerConfig *cfg = &safe;
    JsonNode *node = json_node_new(JSON_NODE_OBJECT); JsonObject *o = json_object_new(); json_node_take_object(node, o);
    JsonArray *channels = json_array_new(), *effects = json_array_new();
    json_object_set_array_member(o, "channels", channels); json_object_set_array_member(o, "effects", effects);
    for (guint i = 0; i < RADIOS_AUDIO_MIXER_CHANNELS; ++i) {
        const RadioSAudioChannelDsp *c = &cfg->channels[i]; JsonObject *strip = json_object_new();
        if (extras && extras->extras[i].label[0]) json_object_set_string_member(strip, "label", extras->extras[i].label);
        if (extras && extras->extras[i].input.kind != RADIOS_CHANNEL_INPUT_NONE) {
            const RadioSAudioChannelInputSpec *spec = &extras->extras[i].input; JsonObject *input = json_object_new();
            json_object_set_string_member(input, "kind", radios_audio_channel_input_kind_name(spec->kind));
            json_object_set_string_member(input, "device", spec->device); json_object_set_string_member(input, "name", spec->name);
            json_object_set_object_member(strip, "input", input);
        }
#define SAVE_DOUBLE(name) json_object_set_double_member(strip, #name, c->name)
        SAVE_DOUBLE(eq_low_db); SAVE_DOUBLE(eq_mid_db); SAVE_DOUBLE(eq_high_db); SAVE_DOUBLE(pan); SAVE_DOUBLE(gain_db);
        SAVE_DOUBLE(fx_send_db); SAVE_DOUBLE(fx_return_db);
        SAVE_DOUBLE(send_db); SAVE_DOUBLE(return_db);
#undef SAVE_DOUBLE
        json_object_set_int_member(strip, "source_deck", c->source_deck); json_object_set_int_member(strip, "fx_slot", c->fx_slot);
        json_object_set_int_member(strip, "crossfade_group", c->crossfade_group);
        json_object_set_int_member(strip, "send_channel", c->send_channel);
        json_object_set_int_member(strip, "return_channel", -1);
        json_object_set_int_member(strip, "return_effect", c->return_effect);
        json_object_set_boolean_member(strip, "mute", c->mute); json_object_set_boolean_member(strip, "solo", c->solo);
        json_object_set_boolean_member(strip, "loudness_bypass", c->loudness_bypass);
        json_array_add_object_element(channels, strip);
    }
    const RadioSAudioMasterDsp *m = &cfg->master; JsonObject *master = json_object_new(); json_object_set_object_member(o, "master", master);
    json_object_set_double_member(master, "eq_low_db", m->eq_low_db); json_object_set_double_member(master, "eq_mid_db", m->eq_mid_db);
    json_object_set_double_member(master, "eq_high_db", m->eq_high_db); json_object_set_boolean_member(master, "mute", m->mute);
    json_object_set_double_member(master, "fx_send_db", m->fx_send_db); json_object_set_double_member(master, "fx_return_db", m->fx_return_db);
    json_object_set_int_member(master, "fx_slot", m->fx_slot);
    json_object_set_int_member(master, "send_channel", m->send_channel);
    json_object_set_int_member(master, "return_channel", -1);
    json_object_set_int_member(master, "return_effect", m->return_effect);
    json_object_set_double_member(master, "send_db", m->send_db);
    json_object_set_double_member(master, "return_db", m->return_db);
    for (guint i = 0; i < RADIOS_AUDIO_FX_SLOTS; ++i) json_array_add_object_element(effects, fx_json(&cfg->effects[i]));
    json_object_set_object_member(o, "master_fx", fx_json(&cfg->master_fx)); json_object_set_double_member(o, "crossfader", cfg->crossfader);
    JsonObject *speech = json_object_new(); json_object_set_object_member(o, "speech", speech);
    json_object_set_boolean_member(speech, "ducking", cfg->speech_ducking); json_object_set_double_member(speech, "bed_db", cfg->speech_bed_db);
    json_object_set_int_member(speech, "fx_slot", cfg->speech_fx_slot);
    json_object_set_boolean_member(speech, "to_master", cfg->speech_to_master);
    json_object_set_int_member(speech, "channel", cfg->speech_channel);
    if (extras && extras->network_count) {
        JsonArray *sources = json_array_new(); json_object_set_array_member(o, "network_sources", sources);
        for (guint i = 0; i < extras->network_count; ++i) {
            JsonObject *source = json_object_new();
            json_object_set_string_member(source, "name", extras->network[i].name); json_object_set_string_member(source, "url", extras->network[i].url);
            json_array_add_object_element(sources, source);
        }
    }
    return node;
}
/* Labels, inputs and network sources of a saved document. Unknown or invalid
 * entries are dropped rather than rejecting the whole mixer document. */
static void extras_read(JsonObject *o, ChannelExtra extras[RADIOS_AUDIO_MIXER_CHANNELS],
    RadioSAudioUiNetworkSource network[RADIOS_AUDIO_UI_NETWORK_SOURCES], guint *network_count)
{
    JsonArray *channels = read_array(o, "channels");
    for (guint i = 0; i < RADIOS_AUDIO_MIXER_CHANNELS; ++i) {
        extras[i].label[0] = '\0'; memset(&extras[i].input, 0, sizeof extras[i].input); extras[i].runner = NULL;
        JsonNode *n = channels && i < json_array_get_length(channels) ? json_array_get_element(channels, i) : NULL;
        JsonObject *strip = n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL;
        const gchar *label = radios_json_string(strip, "label", NULL);
        if (label_known(label)) g_strlcpy(extras[i].label, label, sizeof extras[i].label);
        JsonObject *input = read_object(strip, "input"); RadioSAudioChannelInputSpec spec = {0};
        if (input && radios_audio_channel_input_kind_parse(radios_json_string(input, "kind", ""), &spec.kind) &&
            g_strlcpy(spec.device, radios_json_string(input, "device", ""), sizeof spec.device) < sizeof spec.device &&
            g_strlcpy(spec.name, radios_json_string(input, "name", ""), sizeof spec.name) < sizeof spec.name &&
            radios_audio_channel_input_spec_validate(&spec, NULL))
            extras[i].input = spec;
    }
    *network_count = 0; JsonArray *sources = read_array(o, "network_sources");
    for (guint i = 0; sources && i < json_array_get_length(sources) && *network_count < RADIOS_AUDIO_UI_NETWORK_SOURCES; ++i) {
        JsonNode *n = json_array_get_element(sources, i); if (!JSON_NODE_HOLDS_OBJECT(n)) continue;
        JsonObject *source = json_node_get_object(n); RadioSAudioChannelInputSpec probe = {RADIOS_CHANNEL_INPUT_NETWORK, {0}, {0}};
        if (g_strlcpy(probe.device, radios_json_string(source, "url", ""), sizeof probe.device) >= sizeof probe.device ||
            g_strlcpy(probe.name, radios_json_string(source, "name", ""), sizeof probe.name) >= sizeof probe.name ||
            !radios_audio_channel_input_spec_validate(&probe, NULL)) continue;
        RadioSAudioUiNetworkSource *entry = &network[(*network_count)++];
        g_strlcpy(entry->name, probe.name, sizeof entry->name); g_strlcpy(entry->url, probe.device, sizeof entry->url);
    }
}
/* Starts, restarts or stops each channel's live input so the running set
 * matches the model. Nothing here touches any deck's transport. */
static void inputs_sync(RadioSAudioController *audio)
{
    for (guint i = 0; i < RADIOS_AUDIO_MIXER_CHANNELS; ++i) {
        ChannelExtra *extra = &audio->extras[i];
        gboolean wanted = !audio->closed && extra->input.kind != RADIOS_CHANNEL_INPUT_NONE;
        if (extra->runner) {
            RadioSAudioChannelInputSpec running; radios_audio_channel_input_spec(extra->runner, &running);
            if (!wanted || !radios_audio_channel_input_spec_equal(&running, &extra->input)) g_clear_pointer(&extra->runner, radios_audio_channel_input_free);
        }
        if (wanted && !extra->runner) {
            GError *error = NULL;
            extra->runner = radios_audio_channel_input_new(audio->engine, i, &extra->input, NULL, &error);
            if (!extra->runner) { notify_audio(audio, error ? error->message : "Kanal girişi başlatılamadı."); g_clear_error(&error); }
        }
    }
}
static gboolean autosave_submit(RadioSAudioController *audio)
{
    gboolean queued = FALSE;
    if (audio->mixer_dirty && radios_data_document_ready(audio->mixer_document)) {
        /* Automatic fader motion is runtime output, not a replacement for the
         * operator's last manual level when automatic control is disabled. */
        RadioSAudioMixerConfig cfg; radios_audio_engine_get_manual_mixer(audio->engine, &cfg);
        JsonNode *node = mixer_json(&cfg, audio);
        queued = radios_data_document_autosave(audio->mixer_document, node);
        json_node_free(node);
    }
    if (audio->preferences_dirty && radios_data_document_ready(audio->document)) {
        JsonNode *node = preferences_json(audio);
        radios_data_document_autosave(audio->document, node); json_node_free(node);
    }
    return queued;
}
static gboolean autosave_tick(gpointer value)
{
    RadioSAudioController *audio = value; audio->autosave_timer = 0;
    if (audio->closed || audio->preparing_close) return G_SOURCE_REMOVE;
    autosave_submit(audio);
    /* The flags clear only when the latest save is acknowledged. Keeping one
     * retry also covers a delayed first read, offline staging and uncertain
     * receipts; RadioSDocument retains the exact intent and latest successor. */
    if (audio->mixer_dirty || audio->preferences_dirty) autosave_schedule(audio, 1000);
    return G_SOURCE_REMOVE;
}
static void autosave_schedule(RadioSAudioController *audio, guint delay_ms)
{
    if (audio->closed || audio->preparing_close) return;
    if (audio->autosave_timer) { guint source = audio->autosave_timer; audio->autosave_timer = 0; g_source_remove(source); }
    audio->autosave_timer = g_timeout_add_full(G_PRIORITY_DEFAULT, delay_ms, autosave_tick,
        radios_audio_ui_ref(audio), (GDestroyNotify)radios_audio_ui_unref);
}
static gboolean read_double(JsonObject *o, const gchar *key, gdouble *out)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    if (!n || !JSON_NODE_HOLDS_VALUE(n) || (json_node_get_value_type(n) != G_TYPE_DOUBLE && json_node_get_value_type(n) != G_TYPE_INT64)) return FALSE;
    *out = json_node_get_double(n); return isfinite(*out);
}
static gboolean read_bool(JsonObject *o, const gchar *key, gboolean *out)
{
    JsonNode *n = o ? json_object_get_member(o, key) : NULL;
    if (!n || !JSON_NODE_HOLDS_VALUE(n) || json_node_get_value_type(n) != G_TYPE_BOOLEAN) return FALSE;
    *out = json_node_get_boolean(n); return TRUE;
}
static JsonObject *read_object(JsonObject *o, const gchar *key)
{ JsonNode *n = o ? json_object_get_member(o, key) : NULL; return n && JSON_NODE_HOLDS_OBJECT(n) ? json_node_get_object(n) : NULL; }
static JsonArray *read_array(JsonObject *o, const gchar *key)
{ JsonNode *n = o ? json_object_get_member(o, key) : NULL; return n && JSON_NODE_HOLDS_ARRAY(n) ? json_node_get_array(n) : NULL; }
static gboolean fx_read(JsonObject *o, RadioSAudioFx *fx)
{
    if (!o) return FALSE;
    fx->type = (RadioSAudioFxType)radios_json_int(o, "type", -1); fx->monitor_path = (gint)radios_json_int(o, "monitor_path", -1); fx->cue = FALSE;
    return read_bool(o, "enabled", &fx->enabled) && read_double(o, "depth", &fx->depth) && read_double(o, "send_db", &fx->send_db) &&
        read_double(o, "return_db", &fx->return_db) && read_double(o, "tempo_bpm", &fx->tempo_bpm);
}
static gboolean mixer_read(JsonObject *o, RadioSAudioMixerConfig *cfg)
{
    radios_audio_mixer_config_default(cfg);
    JsonArray *channels = read_array(o, "channels"), *effects = read_array(o, "effects");
    JsonObject *master = read_object(o, "master"), *speech = read_object(o, "speech");
    if (!channels || json_array_get_length(channels) != 32 || !effects || json_array_get_length(effects) != 5 || !master || !speech) return FALSE;
    for (guint i = 0; i < 32; ++i) {
        JsonNode *n = json_array_get_element(channels, i); if (!JSON_NODE_HOLDS_OBJECT(n)) return FALSE;
        JsonObject *strip = json_node_get_object(n); RadioSAudioChannelDsp *c = &cfg->channels[i];
#define READ_DOUBLE(name) if (!read_double(strip, #name, &c->name)) return FALSE
        READ_DOUBLE(eq_low_db); READ_DOUBLE(eq_mid_db); READ_DOUBLE(eq_high_db); READ_DOUBLE(pan); READ_DOUBLE(gain_db);
        READ_DOUBLE(fx_send_db); READ_DOUBLE(fx_return_db);
#undef READ_DOUBLE
        if (json_object_has_member(strip, "send_db") && !read_double(strip, "send_db", &c->send_db)) return FALSE;
        if (json_object_has_member(strip, "return_db") && !read_double(strip, "return_db", &c->return_db)) return FALSE;
        if (json_object_has_member(strip, "loudness_bypass") && !read_bool(strip, "loudness_bypass", &c->loudness_bypass)) return FALSE;
        gint64 send_channel = radios_json_int(strip, "send_channel", json_object_has_member(strip, "send_channel") ? -99 : -1);
        gint64 return_channel = radios_json_int(strip, "return_channel", json_object_has_member(strip, "return_channel") ? -99 : -1);
        if (send_channel < -1 || send_channel >= 32 || return_channel < -1 || return_channel >= 32) return FALSE;
        c->send_channel = (gint)send_channel; c->return_channel = -1;
        c->source_deck = (gint)radios_json_int(strip, "source_deck", -99); c->fx_slot = (gint)radios_json_int(strip, "fx_slot", -99);
        /* Older saves returned their own assigned effect implicitly. Their
         * channel-return selection is retired, not reinterpreted as an FX ID. */
        gint64 return_effect = radios_json_int(strip, "return_effect", json_object_has_member(strip, "return_effect") ? -99 : (c->fx_slot >= 0 ? c->fx_slot : -1));
        if (return_effect < -1 || return_effect >= (gint64)RADIOS_AUDIO_FX_SLOTS) return FALSE;
        c->return_effect = (gint)return_effect;
        if (!json_object_has_member(strip, "return_effect")) c->return_db = c->fx_return_db;
        c->crossfade_group = (gint)radios_json_int(strip, "crossfade_group", -99); c->pfl = FALSE;
        if (!read_bool(strip, "mute", &c->mute) || !read_bool(strip, "solo", &c->solo)) return FALSE;
    }
    RadioSAudioMasterDsp *m = &cfg->master;
    gint64 master_send = radios_json_int(master, "send_channel", json_object_has_member(master, "send_channel") ? -99 : -1);
    gint64 master_return = radios_json_int(master, "return_channel", json_object_has_member(master, "return_channel") ? -99 : -1);
    if (master_send < -1 || master_send >= 32 || master_return < -1 || master_return >= 32) return FALSE;
    m->send_channel = (gint)master_send; m->return_channel = -1;
    if (json_object_has_member(master, "send_db") && !read_double(master, "send_db", &m->send_db)) return FALSE;
    if (json_object_has_member(master, "return_db") && !read_double(master, "return_db", &m->return_db)) return FALSE;
    m->fx_slot = (gint)radios_json_int(master, "fx_slot", -99);
    gint64 master_return_effect = radios_json_int(master, "return_effect", json_object_has_member(master, "return_effect") ? -99 : (m->fx_slot >= 0 ? m->fx_slot : -1));
    if (master_return_effect < -1 || master_return_effect >= (gint64)RADIOS_AUDIO_FX_SLOTS) return FALSE;
    m->return_effect = (gint)master_return_effect;
    if (!read_double(master, "eq_low_db", &m->eq_low_db) || !read_double(master, "eq_mid_db", &m->eq_mid_db) ||
        !read_double(master, "eq_high_db", &m->eq_high_db) || !read_bool(master, "mute", &m->mute) ||
        !read_double(master, "fx_send_db", &m->fx_send_db) || !read_double(master, "fx_return_db", &m->fx_return_db)) return FALSE;
    if (!json_object_has_member(master, "return_effect")) m->return_db = m->fx_return_db;
    for (guint i = 0; i < 5; ++i) { JsonNode *n = json_array_get_element(effects, i); if (!JSON_NODE_HOLDS_OBJECT(n) || !fx_read(json_node_get_object(n), &cfg->effects[i])) return FALSE; }
    gint64 speech_slot = radios_json_int(speech, "fx_slot", json_object_has_member(speech, "fx_slot") ? -99 : -1);
    if (speech_slot < -1 || speech_slot > 3) return FALSE;
    cfg->speech_fx_slot = (gint)speech_slot;
    if (json_object_has_member(speech, "to_master") && !read_bool(speech, "to_master", &cfg->speech_to_master)) return FALSE;
    gint64 speech_channel = radios_json_int(speech, "channel", json_object_has_member(speech, "channel") ? -99 : -1);
    if (speech_channel < -1 || speech_channel >= (gint64)RADIOS_AUDIO_MIXER_CHANNELS) return FALSE;
    cfg->speech_channel = (gint)speech_channel;
    gboolean valid = fx_read(read_object(o, "master_fx"), &cfg->master_fx) && read_double(o, "crossfader", &cfg->crossfader) &&
        read_bool(speech, "ducking", &cfg->speech_ducking) && read_double(speech, "bed_db", &cfg->speech_bed_db) &&
        radios_audio_mixer_config_validate(cfg, NULL);
    if (valid) mixer_retire_fifth_effect(cfg);
    return valid;
}
static void mixer_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *o, gpointer value)
{
    (void)ctx; RadioSAudioController *audio = value; if (audio->closed) return;
    RadioSAudioMixerConfig cfg;
    if (!mixer_read(o, &cfg)) { notify_audio(audio, "Kayıtlı DJ ayarları geçersiz; mevcut güvenli ayarlar korunuyor."); return; }
    if (audio->mixer_initialized && !g_object_get_data(G_OBJECT(owner), "radios-data-explicit-reload")) {
        RadioSAudioMixerConfig live; radios_audio_engine_get_mixer(audio->engine, &live);
        for (guint i = 0; i < 32; ++i) cfg.channels[i].pfl = live.channels[i].pfl;
        for (guint i = 0; i < 5; ++i) cfg.effects[i].cue = live.effects[i].cue;
        cfg.master_fx.cue = live.master_fx.cue;
    }
    mixer_retire_fifth_effect(&cfg);
    GError *error = NULL;
    if (!radios_audio_engine_apply_mixer(audio->engine, &cfg, &error)) { notify_audio(audio, error->message); g_clear_error(&error); return; }
    audio->mixer_initialized = TRUE; audio->mixer_dirty = FALSE;
    /* The saved labels, inputs and network sources replace the model; running
     * inputs are kept when their specification is unchanged. No save follows. */
    ChannelExtra loaded[RADIOS_AUDIO_MIXER_CHANNELS];
    extras_read(o, loaded, audio->network, &audio->network_count);
    for (guint i = 0; i < RADIOS_AUDIO_MIXER_CHANNELS; ++i) {
        g_strlcpy(audio->extras[i].label, loaded[i].label, sizeof audio->extras[i].label);
        audio->extras[i].input = loaded[i].input;
    }
    inputs_sync(audio);
}
static gboolean extras_changed(RadioSAudioController *audio, GError **error)
{
    if (!radios_audio_ui_mixer_ready(audio->ctx)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "Mixer ayarları veritabanından yükleniyor; biraz bekleyin."); return FALSE;
    }
    audio->mixer_initialized = TRUE; audio->mixer_dirty = TRUE;
    if (audio->mixer_document) radios_data_document_dirty(audio->mixer_document, TRUE);
    autosave_schedule(audio, 250); return TRUE;
}
static gboolean channel_index(guint channel, guint *index, GError **error)
{
    if (channel < 1 || channel > RADIOS_AUDIO_MIXER_CHANNELS) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Mikser kanalı 1 ile 32 arasında olmalı."); return FALSE;
    }
    *index = channel - 1; return TRUE;
}
const gchar *radios_audio_ui_get_channel_label(RadioSViewContext *ctx, guint channel)
{
    RadioSAudioController *audio = controller(ctx); guint index;
    if (!audio || !channel_index(channel, &index, NULL) || !audio->extras[index].label[0]) return NULL;
    return audio->extras[index].label;
}
gboolean radios_audio_ui_set_channel_label(RadioSViewContext *ctx, guint channel, const gchar *label, GError **error)
{
    RadioSAudioController *audio = controller(ctx); guint index; if (!audio) return unavailable(error);
    if (!channel_index(channel, &index, error)) return FALSE;
    if (label && *label && !label_known(label)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Kanal etiketi DJ, Konuşma veya Müzik olmalı."); return FALSE;
    }
    const gchar *wanted = label && *label ? label : "";
    if (g_str_equal(audio->extras[index].label, wanted)) return TRUE;
    if (!radios_audio_ui_mixer_ready(ctx)) return extras_changed(audio, error);
    g_strlcpy(audio->extras[index].label, wanted, sizeof audio->extras[index].label);
    return extras_changed(audio, error);
}
gboolean radios_audio_ui_get_channel_input(RadioSViewContext *ctx, guint channel, RadioSAudioChannelInputSpec *spec)
{
    RadioSAudioController *audio = controller(ctx); guint index;
    if (!audio || !channel_index(channel, &index, NULL) || audio->extras[index].input.kind == RADIOS_CHANNEL_INPUT_NONE) {
        if (spec) memset(spec, 0, sizeof *spec);
        return FALSE;
    }
    if (spec) *spec = audio->extras[index].input;
    return TRUE;
}
gboolean radios_audio_ui_set_channel_input(RadioSViewContext *ctx, guint channel, const RadioSAudioChannelInputSpec *spec, GError **error)
{
    RadioSAudioController *audio = controller(ctx); guint index; if (!audio) return unavailable(error);
    if (!channel_index(channel, &index, error)) return FALSE;
    RadioSAudioChannelInputSpec wanted = {0};
    if (spec && spec->kind != RADIOS_CHANNEL_INPUT_NONE) {
        if (!radios_audio_channel_input_spec_validate(spec, error)) return FALSE;
        wanted = *spec;
    }
    if (radios_audio_channel_input_spec_equal(&audio->extras[index].input, &wanted)) return TRUE;
    if (!radios_audio_ui_mixer_ready(ctx)) return extras_changed(audio, error);
    /* A live input replaces the player assignment: the strip's source deck is
     * cleared so the two never add up, while fader and routing stay. */
    if (wanted.kind != RADIOS_CHANNEL_INPUT_NONE) {
        RadioSAudioMixerConfig cfg; radios_audio_engine_get_manual_mixer(audio->engine, &cfg);
        if (cfg.channels[index].source_deck != -1) {
            cfg.channels[index].source_deck = -1;
            if (!radios_audio_ui_set_mixer(ctx, &cfg, error)) return FALSE;
        }
    }
    audio->extras[index].input = wanted;
    inputs_sync(audio);
    return extras_changed(audio, error);
}
gboolean radios_audio_ui_channel_input_status(RadioSViewContext *ctx, guint channel, RadioSAudioChannelInputStatus *status)
{
    RadioSAudioController *audio = controller(ctx); guint index;
    if (status) memset(status, 0, sizeof *status);
    if (!audio || !channel_index(channel, &index, NULL) || !audio->extras[index].runner) return FALSE;
    radios_audio_channel_input_status(audio->extras[index].runner, status); return TRUE;
}
guint radios_audio_ui_capture_devices(RadioSViewContext *ctx, RadioSAudioDevice *devices, guint capacity)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return 0;
    g_autofree RadioSAudioSnapshot *snapshot = snapshot_take(audio); guint count = 0;
    for (guint pass = 0; pass < 2; ++pass)
        for (guint i = 0; i < snapshot->input_count; ++i) {
            if (snapshot->inputs[i].monitor != (pass == 1)) continue;
            if (devices && count < capacity) devices[count] = snapshot->inputs[i];
            ++count;
        }
    return devices ? MIN(count, capacity) : count;
}
guint radios_audio_ui_network_sources(RadioSViewContext *ctx, RadioSAudioUiNetworkSource *sources, guint capacity)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return 0;
    guint count = sources ? MIN(audio->network_count, capacity) : audio->network_count;
    for (guint i = 0; sources && i < count; ++i) sources[i] = audio->network[i];
    return count;
}
gboolean radios_audio_ui_add_network_source(RadioSViewContext *ctx, const gchar *name, const gchar *url, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    RadioSAudioChannelInputSpec probe = {RADIOS_CHANNEL_INPUT_NETWORK, {0}, {0}};
    if (!name || !url || g_strlcpy(probe.device, url, sizeof probe.device) >= sizeof probe.device ||
        g_strlcpy(probe.name, name, sizeof probe.name) >= sizeof probe.name) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Ağ kaynağı adı 1..128, adresi 1..512 karakter olmalı."); return FALSE;
    }
    g_strstrip(probe.name); g_strstrip(probe.device);
    if (!radios_audio_channel_input_spec_validate(&probe, error)) return FALSE;
    for (guint i = 0; i < audio->network_count; ++i) if (g_str_equal(audio->network[i].url, probe.device)) {
        if (g_str_equal(audio->network[i].name, probe.name)) return TRUE;
        if (!radios_audio_ui_mixer_ready(ctx)) return extras_changed(audio, error);
        g_strlcpy(audio->network[i].name, probe.name, sizeof audio->network[i].name); return extras_changed(audio, error);
    }
    if (audio->network_count >= RADIOS_AUDIO_UI_NETWORK_SOURCES) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "En fazla 32 ağ kaynağı kaydedilebilir."); return FALSE;
    }
    if (!radios_audio_ui_mixer_ready(ctx)) return extras_changed(audio, error);
    RadioSAudioUiNetworkSource *entry = &audio->network[audio->network_count++];
    g_strlcpy(entry->name, probe.name, sizeof entry->name); g_strlcpy(entry->url, probe.device, sizeof entry->url);
    return extras_changed(audio, error);
}
gboolean radios_audio_ui_remove_network_source(RadioSViewContext *ctx, const gchar *url, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    for (guint i = 0; url && i < audio->network_count; ++i) if (g_str_equal(audio->network[i].url, url)) {
        if (!radios_audio_ui_mixer_ready(ctx)) return extras_changed(audio, error);
        for (guint j = i + 1; j < audio->network_count; ++j) audio->network[j - 1] = audio->network[j];
        --audio->network_count; return extras_changed(audio, error);
    }
    return TRUE;
}
guint radios_audio_ui_sources_hash(RadioSViewContext *ctx)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return 0;
    g_autofree RadioSAudioSnapshot *snapshot = snapshot_take(audio); guint value = 5381;
    for (guint i = 0; i < snapshot->input_count; ++i)
        value = (value * 33) ^ g_str_hash(snapshot->inputs[i].name) ^ g_str_hash(snapshot->inputs[i].description) ^ (snapshot->inputs[i].monitor ? 1u : 0u);
    for (guint i = 0; i < audio->network_count; ++i) value = (value * 33) ^ g_str_hash(audio->network[i].name) ^ g_str_hash(audio->network[i].url);
    return value ^ snapshot->input_count ^ (audio->network_count << 8);
}
gboolean radios_audio_ui_get_mixer(RadioSViewContext *ctx, RadioSAudioMixerConfig *cfg, RadioSAudioDspMeters *meters, gdouble *master_db)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return FALSE;
    g_autofree RadioSAudioSnapshot *snap = snapshot_take(audio);
    if (cfg) *cfg = snap->mixer;
    if (meters) *meters = snap->dsp_meters;
    if (master_db) *master_db = audio->preferences.main_gain_db;
    return TRUE;
}
gboolean radios_audio_ui_mixer_ready(RadioSViewContext *ctx)
{
    RadioSAudioController *audio = controller(ctx);
    return audio && (!ctx->data || radios_data_document_ready(audio->mixer_document));
}
gboolean radios_audio_ui_set_mixer(RadioSViewContext *ctx, const RadioSAudioMixerConfig *cfg, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    if (!radios_audio_mixer_config_validate(cfg, error)) return FALSE;
    RadioSAudioMixerConfig safe = *cfg;
    mixer_retire_fifth_effect(&safe);
    RadioSAudioMixerConfig before; radios_audio_engine_get_mixer(audio->engine, &before);
    JsonNode *previous = mixer_json(&before, NULL), *next = mixer_json(&safe, NULL);
    gboolean changed = !json_node_equal(previous, next); json_node_free(previous); json_node_free(next);
    /* A full mixer snapshot cannot safely replace an unseen database baseline:
     * an early edit would otherwise also overwrite untouched saved controls. */
    if (changed && !radios_audio_ui_mixer_ready(ctx)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "Mixer ayarları veritabanından yükleniyor; biraz bekleyin.");
        return FALSE;
    }
    RadioSAudioMixerConfig manual_before, manual_after;
    radios_audio_engine_get_manual_mixer(audio->engine, &manual_before);
    if (!radios_audio_engine_apply_mixer(audio->engine, &safe, error)) return FALSE;
    radios_audio_engine_get_manual_mixer(audio->engine, &manual_after);
    previous = mixer_json(&manual_before, NULL); next = mixer_json(&manual_after, NULL);
    changed = !json_node_equal(previous, next); json_node_free(previous); json_node_free(next);
    audio->mixer_initialized = TRUE;
    if (changed || (radios_data_document_ready(audio->mixer_document) && !*radios_data_document_id(audio->mixer_document))) {
        audio->mixer_dirty = TRUE;
        if (audio->mixer_document) radios_data_document_dirty(audio->mixer_document, TRUE);
        autosave_schedule(audio, 250);
    }
    return TRUE;
}
gboolean radios_audio_ui_set_master_gain(RadioSViewContext *ctx, gdouble db, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    if (!isfinite(db) || db < -60 || db > 6) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "MASTER kazancı -60..6 dB olmalı."); return FALSE; }
    gdouble old = audio->preferences.main_gain_db; audio->preferences.main_gain_db = db;
    if (!apply_engine(audio, error)) { audio->preferences.main_gain_db = old; return FALSE; }
    if (db != old || (radios_data_document_ready(audio->document) && !*radios_data_document_id(audio->document)))
        preferences_changed(audio, PREF_GAIN(RADIOS_AUDIO_UI_MAIN));
    return TRUE;
}
gboolean radios_audio_ui_loudness(RadioSViewContext *ctx, gboolean *enabled, gdouble *target_lufs)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return FALSE;
    if (enabled) *enabled = audio->preferences.loudness_normalize;
    if (target_lufs) *target_lufs = audio->preferences.loudness_target_lufs;
    return TRUE;
}
gboolean radios_audio_ui_set_loudness(RadioSViewContext *ctx, gboolean enabled, gdouble target_lufs, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    if (!isfinite(target_lufs) || target_lufs < -30 || target_lufs > -5) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Hedef ses yüksekliği -30..-5 LUFS olmalı."); return FALSE;
    }
    RadioSAudioPreferences old = audio->preferences;
    audio->preferences.loudness_normalize = enabled; audio->preferences.loudness_target_lufs = target_lufs;
    if (!apply_engine(audio, error)) { audio->preferences = old; return FALSE; }
    if (old.loudness_normalize != enabled || old.loudness_target_lufs != target_lufs) preferences_changed(audio, PREF_LOUDNESS);
    return TRUE;
}
gboolean radios_audio_ui_loudness_policy(RadioSViewContext *ctx, RadioSAudioLoudnessPolicy *policy)
{
    RadioSAudioController *audio = controller(ctx); if (!audio || !policy) return FALSE;
    loudness_empty_baseline_default(audio);
    *policy = audio->preferences.loudness_policy; return TRUE;
}
gboolean radios_audio_ui_loudness_policy_ready(RadioSViewContext *ctx)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return FALSE;
    if (!ctx->data) return TRUE;
    return radios_data_document_ready(audio->document) &&
        (audio->preferences_initialized || !*radios_data_document_id(audio->document));
}
gboolean radios_audio_ui_set_loudness_policy(RadioSViewContext *ctx,
    const RadioSAudioLoudnessPolicy *policy, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    if (!radios_audio_loudness_policy_validate(policy, error)) return FALSE;
    RadioSAudioLoudnessPolicy previous = audio->preferences.loudness_policy;
    audio->preferences.loudness_policy = *policy;
    if (!apply_engine(audio, error)) { audio->preferences.loudness_policy = previous; return FALSE; }
    if (!radios_audio_loudness_policy_equal(&previous, policy)) preferences_changed(audio, PREF_LOUDNESS_POLICY);
    return TRUE;
}
gboolean radios_audio_ui_mixer_save(RadioSViewContext *ctx)
{
    RadioSAudioController *audio = controller(ctx);
    if (!audio || !radios_data_document_ready(audio->mixer_document)) return FALSE;
    /* Compatibility flush for callers that need an immediate checkpoint. The
     * DJ controls themselves always schedule automatic saves. */
    audio->mixer_dirty = TRUE; radios_data_document_dirty(audio->mixer_document, TRUE);
    gboolean queued = autosave_submit(audio); autosave_schedule(audio, 1000);
    return queued;
}
void radios_audio_ui_mixer_reload(RadioSViewContext *ctx)
{ RadioSAudioController *audio = controller(ctx); if (audio) radios_data_document_reload(audio->mixer_document); }
gboolean radios_audio_ui_microphone(RadioSViewContext *ctx, gboolean on, GError **error)
{ RadioSAudioController *audio = controller(ctx); return audio ? radios_audio_engine_set_microphone_gate(audio->engine, on, error) : unavailable(error); }
gboolean radios_audio_ui_microphone_active(RadioSViewContext *ctx)
{ RadioSAudioController *audio = controller(ctx); if (!audio) return FALSE; g_autofree RadioSAudioSnapshot *snap = snapshot_take(audio); return snap->microphone_gate; }
gboolean radios_audio_ui_microphone_ptt(RadioSViewContext *ctx, gboolean held, GError **error)
{ RadioSAudioController *audio = controller(ctx); return audio ? radios_audio_engine_set_microphone_ptt(audio->engine, held, error) : unavailable(error); }
gboolean radios_audio_ui_set_channel_on_air(RadioSViewContext *ctx, guint channel, gboolean on_air, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    if (channel < 1 || channel > RADIOS_AUDIO_MIXER_CHANNELS) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Mikser kanalı 1 ile 32 arasında olmalı."); return FALSE;
    }
    /* On air means the strip is not muted; the fader, routing and the deck's
     * transport stay as they are. The manual view keeps the operator's gain. */
    RadioSAudioMixerConfig cfg; radios_audio_engine_get_manual_mixer(audio->engine, &cfg);
    if (cfg.channels[channel - 1].mute == !on_air) return TRUE;
    cfg.channels[channel - 1].mute = !on_air;
    return radios_audio_ui_set_mixer(ctx, &cfg, error);
}
gboolean radios_audio_ui_channel_on_air(RadioSViewContext *ctx, guint channel)
{
    RadioSAudioController *audio = controller(ctx); if (!audio || channel < 1 || channel > RADIOS_AUDIO_MIXER_CHANNELS) return FALSE;
    RadioSAudioMixerConfig cfg; radios_audio_engine_get_mixer(audio->engine, &cfg);
    return !cfg.channels[channel - 1].mute;
}
gboolean radios_audio_ui_cue_mix(RadioSViewContext *ctx, gboolean on, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    if (on && !radios_audio_ui_ready(ctx, RADIOS_AUDIO_UI_CUE)) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "CUE çıkış aygıtı hazır değil."); return FALSE; }
    radios_audio_ui_preview_stop(ctx, NULL);
    audio->preferences.cue_source = -1;
    if (!apply_engine(audio, error)) return FALSE;
    return radios_audio_engine_set_bus_gate(audio->engine, RADIOS_AUDIO_BUS_CUE, on, error);
}
void radios_audio_ui_cue_mix_release(RadioSViewContext *ctx)
{
    RadioSAudioController *audio = controller(ctx);
    if (audio && !audio->preview_active && audio->preferences.cue_source == -1)
        radios_audio_engine_set_bus_gate(audio->engine, RADIOS_AUDIO_BUS_CUE, FALSE, NULL);
}
static void preferences_loaded(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *object, gpointer value)
{
    (void)ctx; RadioSAudioController *audio = value; if (audio->closed) return;
    RadioSAudioPreferences prefs; if (!preferences_read(object, &prefs)) { notify_audio(audio, "Kayıtlı ses ayarlarının biçimi geçersiz; güvenli mevcut seçimler korunuyor."); return; }
    if (!json_object_has_member(object, "loudness_policy"))
        loudness_station_timezone(audio, &prefs.loudness_policy);
    audio->preferences_initialized = TRUE;
    gboolean explicit_reload = g_object_get_data(G_OBJECT(owner), "radios-data-explicit-reload") != NULL;
    gboolean keep_local = !explicit_reload && audio->preferences_dirty && !preferences_equal(&audio->preferences, &prefs);
    if (keep_local) {
        for (guint bus = 0; bus < 4; ++bus)
            if (audio->preference_changes & PREF_GAIN(bus))
                *gain_pointer(&prefs, (RadioSAudioUiBus)bus) = *gain_pointer(&audio->preferences, (RadioSAudioUiBus)bus);
        if (audio->preference_changes & PREF_CUE_SOURCE) prefs.cue_source = audio->preferences.cue_source;
        if (audio->preference_changes & PREF_MONITOR_SOURCE) prefs.monitor_source = audio->preferences.monitor_source;
        if (audio->preference_changes & PREF_MONITOR_SINK) g_strlcpy(prefs.monitor_sink, audio->preferences.monitor_sink, sizeof prefs.monitor_sink);
        if (audio->preference_changes & PREF_LOUDNESS) {
            prefs.loudness_normalize = audio->preferences.loudness_normalize; prefs.loudness_target_lufs = audio->preferences.loudness_target_lufs;
        }
        if (audio->preference_changes & PREF_LOUDNESS_POLICY) prefs.loudness_policy = audio->preferences.loudness_policy;
    } else {
        audio->preferences_dirty = FALSE; audio->preference_changes = 0;
    }
    gboolean changed = !preferences_equal(&audio->preferences, &prefs); audio->preferences = prefs;
    if (explicit_reload) audio->settings_dirty = FALSE;
    for (guint deck = 0; deck < RADIOS_AUDIO_NORMAL_DECKS; ++deck) audio->decks[deck].player_options = (RadioSAudioPlayerOptions){-1, -1};
    JsonArray *players = read_array(object, "player_automation");
    for (guint index = 0; players && index < json_array_get_length(players); ++index) {
        JsonObject *row = json_array_get_object_element(players, index); gint64 deck = radios_json_int(row, "player", 0) - 1;
        gint64 master = radios_json_int(row, "sync_master", 0) - 1, target = radios_json_int(row, "autodj_target", 0) - 1;
        if (deck >= 0 && deck < 32 && master >= -1 && master < 32 && target >= -1 && target < 32 && master != deck && target != deck)
            audio->decks[deck].player_options = (RadioSAudioPlayerOptions){(gint)master, (gint)target};
    }
    if (changed) {
        radios_audio_ui_preview_stop(audio->ctx, NULL);
        radios_audio_engine_set_microphone_gate(audio->engine, FALSE, NULL);
        radios_audio_engine_set_bus_gate(audio->engine, RADIOS_AUDIO_BUS_CUE, FALSE, NULL);
        radios_audio_engine_set_bus_gate(audio->engine, RADIOS_AUDIO_BUS_MONITOR, FALSE, NULL);
    }
    GError *error = NULL;
    if (!apply_engine(audio, &error)) { notify_audio(audio, error->message); g_clear_error(&error); }
    if (keep_local) { mark_dirty(audio); autosave_schedule(audio, 250); }
    g_autofree RadioSAudioSnapshot *snapshot = snapshot_take(audio);
    for (guint i = 0; i < audio->bindings->len; ++i) binding_update(g_ptr_array_index(audio->bindings, i), snapshot, TRUE);
}
static void binding_free(gpointer value)
{
    AudioBinding *binding = value; GWeakRef *refs[] = {&binding->widget, &binding->control, &binding->destination, &binding->toggle};
    for (guint i = 0; i < binding->signals->len; ++i) {
        SignalLink *link = g_ptr_array_index(binding->signals, i); GObject *object = g_weak_ref_get(&link->object);
        if (object) { if (g_signal_handler_is_connected(object, link->handler)) g_signal_handler_disconnect(object, link->handler); g_object_unref(object); }
        g_weak_ref_clear(&link->object); g_free(link);
    }
    g_ptr_array_unref(binding->signals);
    for (guint i = 0; i < G_N_ELEMENTS(refs); ++i) {
        GObject *object = g_weak_ref_get(refs[i]); if (object) { g_signal_handlers_disconnect_by_data(object, binding); g_object_unref(object); }
        g_weak_ref_clear(refs[i]);
    }
    g_free(binding);
}
RadioSAudioController *radios_audio_ui_ref(RadioSAudioController *audio)
{ if (audio) g_atomic_int_inc(&audio->references); return audio; }
RadioSAudioController *radios_audio_ui_new(RadioSViewContext *ctx)
{
    if (!ctx || !GTK_IS_WIDGET(ctx->parent)) return NULL;
    RadioSAudioController *audio = g_new0(RadioSAudioController, 1); audio->references = 1; audio->ctx = ctx;
    audio->engine = radios_audio_engine_new(TRUE, NULL); radios_audio_preferences_default(&audio->preferences);
    RadioSAudioMixerConfig mixer; radios_audio_engine_get_mixer(audio->engine, &mixer);
    mixer_retire_fifth_effect(&mixer);
    radios_audio_engine_apply_mixer(audio->engine, &mixer, NULL);
    radios_audio_engine_set_bus_gate(audio->engine, RADIOS_AUDIO_BUS_CUE, FALSE, NULL);
    radios_audio_engine_set_bus_gate(audio->engine, RADIOS_AUDIO_BUS_MONITOR, FALSE, NULL);
    audio->bindings = g_ptr_array_new_with_free_func(binding_free); g_weak_ref_init(&audio->parent, ctx->parent);
    audio->preview_outputs = g_ptr_array_new_with_free_func(preview_output_free);
    g_weak_ref_init(&audio->transition_owner, NULL);
    for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) { g_weak_ref_init(&audio->decks[i].owner, NULL); audio->decks[i].player_options = (RadioSAudioPlayerOptions){-1, -1}; }
    g_object_set_data(G_OBJECT(ctx->parent), "radios-audio-context", ctx);
    if (ctx->data) audio->document = radios_data_document_new(ctx, GTK_WIDGET(ctx->parent), "audio_preferences", "Ses aygıtları", preferences_loaded, audio);
    if (ctx->data) audio->mixer_document = radios_data_document_new(ctx, GTK_WIDGET(ctx->parent), "dj_mixer", "DJ mixer", mixer_loaded, audio);
    return audio;
}
static void owner_guard_free(gpointer value, GClosure *closure)
{ (void)closure; OwnerGuard *guard = value; radios_audio_ui_unref(guard->audio); g_free(guard); }
static void owner_destroyed(GtkWidget *widget, gpointer value)
{
    OwnerGuard *guard = value; RadioSAudioController *audio = guard->audio;
    if (audio->closed || guard->epoch != audio->decks[guard->deck].epoch) return;
    if (guard->deck == RADIOS_AUDIO_PREVIEW_DECK) { radios_audio_ui_preview_stop(audio->ctx, widget); return; }
    if (guard->deck < RADIOS_AUDIO_NORMAL_DECKS) {
        /* A live player's transport belongs to the controller. Rebuilding or
         * hiding its card releases the widget while its queue/Play intent stays. */
        audio->decks[guard->deck].owner_handler = 0;
        g_weak_ref_set(&audio->decks[guard->deck].owner, NULL); return;
    }
    audio->decks[guard->deck].autoplay = FALSE; ++audio->decks[guard->deck].epoch;
    radios_audio_engine_stop(audio->engine, guard->deck, NULL);
}
static gboolean flag_true(JsonObject *object, const gchar *name)
{
    JsonNode *node = object ? json_object_get_member(object, name) : NULL;
    return node && JSON_NODE_HOLDS_VALUE(node) && json_node_get_value_type(node) == G_TYPE_BOOLEAN && json_node_get_boolean(node);
}
static guint64 detail_output_frames(JsonObject *detail, guint64 duration_ms);
static gboolean detail_spec(RadioSAudioController *audio, JsonObject *detail, RadioSAudioLoadSpec *spec, guint64 *duration, gchar **path, GError **error)
{
    JsonNode *media_node = detail ? json_object_get_member(detail, "media") : NULL, *version_node = detail ? json_object_get_member(detail, "version") : NULL;
    if (!media_node || !version_node || !JSON_NODE_HOLDS_OBJECT(media_node) || !JSON_NODE_HOLDS_OBJECT(version_node)) goto invalid;
    JsonObject *media = json_node_get_object(media_node), *version = json_node_get_object(version_node);
    const gchar *station = radios_json_string(media, "station_id", ""), *sha = radios_json_string(version, "sha256", ""), *managed = radios_json_string(version, "managed_path", "");
    if (!radios_data_connected(audio->ctx) || !g_str_equal(station, radios_data_station_id(audio->ctx)) ||
        !flag_true(version, "verified") || !flag_true(version, "object_available") || strlen(sha) != 64 || !radios_path_is_absolute(managed)) goto invalid;
    for (guint i = 0; i < 64; ++i) if (!g_ascii_isxdigit(sha[i])) goto invalid;
    spec->media_id = radios_json_string(media, "_id", ""); spec->version_id = radios_json_string(version, "_id", "");
    if (!g_uuid_string_is_valid(spec->media_id) || !g_uuid_string_is_valid(spec->version_id) ||
        !g_str_equal(radios_json_string(media, "version_id", ""), spec->version_id) ||
        !g_uuid_string_is_valid(radios_json_string(media, "item_id", "")) ||
        !g_str_equal(radios_json_string(media, "item_id", ""), radios_json_string(version, "item_id", "")) ||
        !g_uuid_string_is_valid(radios_json_string(media, "media_file_id", "")) ||
        !g_str_equal(radios_json_string(media, "media_file_id", ""), radios_json_string(version, "media_file_id", ""))) goto invalid;
    /* The managed object must lie strictly below "<database>.objects".  NTFS
     * names are case-insensitive and both separators are valid, so the check
     * compares normalized paths component-wise without case ("c:/X" vs "C:\x"). */
    *path = radios_path_normalize(managed);
    gchar *objects = g_strconcat(audio->ctx->config->database_file, ".objects", NULL), *root = radios_path_normalize(objects);
    gboolean contained = radios_path_is_inside(root, *path) && !radios_path_equal(root, *path); g_free(objects); g_free(root);
    if (!contained) { g_clear_pointer(path, g_free); goto invalid; }
    gint64 duration_ms = radios_json_int(version, "duration_ms", 0); if (duration_ms < 1 || duration_ms > 86400000) { g_clear_pointer(path, g_free); goto invalid; }
    *duration = (guint64)duration_ms; spec->path = *path; spec->sha256 = sha;
    JsonNode *markers = json_object_get_member(detail, "markers");
    JsonObject *marks = markers && JSON_NODE_HOLDS_OBJECT(markers) ? json_node_get_object(markers) : NULL;
    if (marks && (!g_str_equal(radios_json_string(marks, "station_id", ""), station) ||
        !g_str_equal(radios_json_string(marks, "item_id", ""), radios_json_string(media, "item_id", "")) ||
        !g_str_equal(radios_json_string(marks, "version_id", ""), spec->version_id) ||
        !g_str_equal(radios_json_string(marks, "media_file_id", ""), radios_json_string(media, "media_file_id", "")) ||
        !g_str_equal(radios_json_string(marks, "media_sha256", ""), sha) ||
        radios_json_int(marks, "media_duration_ms", 0) != duration_ms)) {
        g_clear_pointer(path, g_free); goto invalid;
    }
    guint64 in_ms = (guint64)radios_json_int(marks, "cue_in_ms", 0), out_ms = (guint64)radios_json_int(marks, "out_ms", duration_ms);
    if (in_ms >= out_ms || out_ms > *duration) { g_clear_pointer(path, g_free); goto invalid; }
    RadioSMediaGeometry geometry = radios_media_geometry(detail, duration_ms, TRUE);
    spec->cue_in_frame = geometry.cue_in; spec->cue_out_frame = geometry.cue_out;
    if (spec->cue_in_frame >= spec->cue_out_frame) { g_clear_pointer(path, g_free); goto invalid; }
    return TRUE;
invalid:
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Yalnız bu istasyonun dosyası doğrulanmış medya sürümü çalınabilir."); return FALSE;
}
static void player_metadata_text(gchar *target, gsize size, const gchar *value)
{
    g_strlcpy(target, value, size);
    const gchar *invalid = NULL;
    if (!g_utf8_validate(target, -1, &invalid)) target[invalid - target] = '\0';
}
static gboolean player_load_metadata(RadioSAudioController *audio, GtkWidget *owner,
    const RadioSAudioLoadSpec *spec, guint deck, guint64 duration, RadioSAudioPlayerBinding *metadata)
{
    if (deck >= RADIOS_AUDIO_NORMAL_DECKS || !spec->media_id || !spec->version_id) return FALSE;
    JsonArray *items = NULL; guint index = 0, actual_deck = 0;
    if (!radios_broadcast_player_playlist(audio->ctx, owner, &items, NULL, &index, &actual_deck)) return FALSE;
    JsonNode *node = json_array_get_element(items, index);
    JsonObject *selected = node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    if (actual_deck != deck || !selected ||
        g_strcmp0(radios_json_string(selected, "media_id", ""), spec->media_id) ||
        g_strcmp0(radios_json_string(selected, "version_id", ""), spec->version_id)) {
        json_array_unref(items); return FALSE;
    }
    for (guint i = 0; i < json_array_get_length(items); ++i) {
        JsonNode *item = json_array_get_element(items, i);
        if (!item || !JSON_NODE_HOLDS_OBJECT(item)) { json_array_unref(items); return FALSE; }
    }
    const gchar *provided = radios_json_string(selected, "playlist_id", "");
    gchar *playlist = g_uuid_string_is_valid(provided) ? g_strdup(provided) : NULL;
    if (!playlist) for (guint i = 0; i < json_array_get_length(items) && !playlist; ++i) {
        const gchar *known = radios_json_string(json_array_get_object_element(items, i), "playlist_id", "");
        if (g_uuid_string_is_valid(known)) playlist = g_strdup(known);
    }
    if (!playlist) playlist = g_uuid_string_random();
    gboolean normalize_playlist = !g_uuid_string_is_valid(provided);
    for (guint i = 0; i < json_array_get_length(items); ++i) {
        JsonObject *item = json_array_get_object_element(items, i);
        if (normalize_playlist || !g_uuid_string_is_valid(radios_json_string(item, "playlist_id", "")))
            json_object_set_string_member(item, "playlist_id", playlist);
        const gchar *entry = radios_json_string(item, "entry_id", "");
        if (!g_uuid_string_is_valid(entry)) entry = radios_json_string(item, "id", "");
        if (!g_uuid_string_is_valid(entry)) entry = radios_json_string(item, "_id", "");
        gchar *entry_id = g_uuid_string_is_valid(entry) ? g_strdup(entry) : g_uuid_string_random();
        json_object_set_string_member(item, "entry_id", entry_id); g_free(entry_id);
    }
    memset(metadata, 0, sizeof *metadata); metadata->deck = deck;
    g_strlcpy(metadata->playlist_id, playlist, sizeof metadata->playlist_id);
    g_strlcpy(metadata->entry_id, radios_json_string(selected, "entry_id", ""), sizeof metadata->entry_id);
    g_strlcpy(metadata->provider, "local", sizeof metadata->provider);
    g_strlcpy(metadata->source_id, spec->media_id, sizeof metadata->source_id);
    gchar *filename = spec->path ? g_path_get_basename(spec->path) : g_strdup("—");
    player_metadata_text(metadata->title, sizeof metadata->title, radios_json_string(selected, "title", filename));
    if (!metadata->title[0]) player_metadata_text(metadata->title, sizeof metadata->title, "—");
    player_metadata_text(metadata->artist, sizeof metadata->artist, radios_json_string(selected, "artist", ""));
    metadata->duration_frames = MIN(duration, G_GUINT64_CONSTANT(86400000)) * 48;
    g_free(filename); g_free(playlist); json_array_unref(items); return TRUE;
}
static gboolean load_spec(RadioSAudioController *audio, GtkWidget *owner, RadioSAudioLoadSpec *spec, guint deck, gboolean autoplay, guint64 duration, GError **error)
{
    if (!owner || deck >= RADIOS_AUDIO_DECKS) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Oynatıcı veya kayıt sahibi geçersiz."); return FALSE; }
    g_autofree RadioSAudioSnapshot *requested = snapshot_take(audio);
    RadioSAudioPlayerBinding metadata;
    gboolean has_metadata = player_load_metadata(audio, owner, spec, deck, duration, &metadata);
    if (!radios_audio_engine_load_player(audio->engine, deck, spec, has_metadata ? &metadata : NULL, error)) return FALSE;
    UiDeck *state = &audio->decks[deck]; owner_clear(state); ++state->epoch; state->autoplay = autoplay; state->duration_ms = duration; g_weak_ref_set(&state->owner, owner);
    state->title[0] = state->artist[0] = state->album[0] = state->kind[0] = '\0';
    g_autofree RadioSAudioSnapshot *snapshot = snapshot_take(audio); state->generation = snapshot->decks[deck].generation;
    state->loss_epoch = requested->loss_epoch;
    OwnerGuard *guard = g_new0(OwnerGuard, 1); guard->audio = radios_audio_ui_ref(audio); guard->deck = deck; guard->epoch = state->epoch;
    state->owner_handler = g_signal_connect_data(owner, "destroy", G_CALLBACK(owner_destroyed), guard, owner_guard_free, 0); return TRUE;
}
gboolean radios_audio_ui_load_media(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail, guint deck, gboolean autoplay, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    if (deck == RADIOS_AUDIO_PREVIEW_DECK) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Ön dinleme için ayrılmış oynatıcıyı kullan."); return FALSE; }
    RadioSAudioLoadSpec spec = {0}; guint64 duration = 0; gchar *path = NULL;
    if (!detail_spec(audio, detail, &spec, &duration, &path, error)) return FALSE;
    gboolean accepted = load_spec(audio, owner, &spec, deck, autoplay, duration, error); g_free(path);
    if (accepted) {
        UiDeck *state = &audio->decks[deck]; JsonObject *media = json_object_get_object_member(detail, "media");
        JsonNode *loaded = json_node_new(JSON_NODE_OBJECT); json_node_take_object(loaded, radios_programme_clone_object(detail));
        g_object_set_data_full(G_OBJECT(owner), "radios-deck-media-detail", loaded, (GDestroyNotify)json_node_free);
        g_strlcpy(state->kind, radios_json_string(media, "kind", ""), sizeof state->kind);
        JsonObject *analysis = read_object(detail, "analysis"); RadioSRhythmAnalysis rhythm; RadioSLoudness loudness;
        if (radios_rhythm_analysis_read(read_object(analysis, "rhythm"), &rhythm))
            radios_audio_engine_set_deck_rhythm(audio->engine, deck, state->generation, &rhythm, NULL);
        if (radios_loudness_read(analysis, &loudness))
            radios_audio_engine_set_deck_loudness(audio->engine, deck, state->generation, &loudness, NULL);
        gchar *fields[] = {state->title, state->artist, state->album}; const gchar *names[] = {"title", "artist", "album"};
        for (guint index = 0; index < G_N_ELEMENTS(fields); ++index) {
            g_strlcpy(fields[index], radios_json_string(media, names[index], ""), sizeof state->title);
            const gchar *invalid_utf8 = NULL;
            if (!g_utf8_validate(fields[index], -1, &invalid_utf8)) fields[index][invalid_utf8 - fields[index]] = '\0';
        }
    }
    return accepted;
}
/* Fills one deck's view of an engine snapshot that was already taken. */
static void deck_ui_snapshot(RadioSAudioController *audio, const RadioSAudioSnapshot *snapshot, guint deck, RadioSAudioUiSnapshot *out)
{
    const RadioSAudioDeckSnapshot *actual = &snapshot->decks[deck];
    memset(out, 0, sizeof(*out)); out->state = (RadioSAudioUiState)actual->state; out->generation = actual->generation;
    out->position_ms = actual->cursor_frames / 48; out->duration_ms = audio->decks[deck].duration_ms;
    out->runner_paused = actual->runner_paused;
    out->main_path_ready = actual->main_path_ready;
    g_strlcpy(out->main_block_reason, actual->main_block_reason, sizeof out->main_block_reason);
    out->cue_in_ms = (actual->cue_in_frame + 47) / 48;
    out->cue_out_ms = actual->cue_out_frame ? actual->cue_out_frame / 48 : out->duration_ms;
    out->peak[0] = actual->peak[0]; out->peak[1] = actual->peak[1]; out->main_enabled = actual->main_routed;
    out->output_ready = deck_output_ready(audio, snapshot, deck);
    out->bpm = actual->bpm; out->effective_bpm = actual->effective_bpm; out->tempo_ratio = actual->tempo_ratio; out->sync_master = actual->sync_master;
    out->loudness_lufs = actual->loudness_lufs; out->loudness_gain_db = actual->loudness_gain_db; out->loudness_measured = actual->loudness_measured;
    g_strlcpy(out->media_id, actual->media_id, sizeof out->media_id); g_strlcpy(out->version_id, actual->version_id, sizeof out->version_id);
    if (actual->media_id[0] && actual->generation == audio->decks[deck].generation) {
        g_strlcpy(out->title, audio->decks[deck].title, sizeof out->title);
        g_strlcpy(out->artist, audio->decks[deck].artist, sizeof out->artist);
        g_strlcpy(out->album, audio->decks[deck].album, sizeof out->album);
        g_strlcpy(out->kind, audio->decks[deck].kind, sizeof out->kind);
    }
    g_strlcpy(out->error, actual->error, sizeof out->error);
}
gboolean radios_audio_ui_get_snapshot(RadioSViewContext *ctx, guint deck, RadioSAudioUiSnapshot *out)
{
    RadioSAudioController *audio = controller(ctx); if (!audio || !out || deck >= RADIOS_AUDIO_DECKS) return FALSE;
    g_autofree RadioSAudioSnapshot *snapshot = snapshot_take(audio);
    deck_ui_snapshot(audio, snapshot, deck, out); return TRUE;
}
gboolean radios_audio_ui_get_snapshots(RadioSViewContext *ctx, RadioSAudioUiSnapshot *out, guint count)
{
    RadioSAudioController *audio = controller(ctx); if (!audio || !out || !count || count > RADIOS_AUDIO_DECKS) return FALSE;
    /* One engine snapshot (one lock of the render mutex) serves every deck. */
    g_autofree RadioSAudioSnapshot *snapshot = snapshot_take(audio);
    for (guint deck = 0; deck < count; ++deck) deck_ui_snapshot(audio, snapshot, deck, &out[deck]);
    return TRUE;
}
gboolean radios_audio_ui_ready(RadioSViewContext *ctx, RadioSAudioUiBus bus)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return FALSE; g_autofree RadioSAudioSnapshot *snapshot = snapshot_take(audio);
    return bus_ready(audio, snapshot, bus);
}
guint64 radios_audio_ui_loss_epoch(RadioSViewContext *ctx)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return 0;
    g_autofree RadioSAudioSnapshot *snapshot = snapshot_take(audio); return snapshot->loss_epoch;
}
static void deck_metadata_rebind(RadioSAudioController *audio, guint deck, const RadioSAudioSnapshot *before);
gboolean radios_audio_ui_play(RadioSViewContext *ctx, guint deck, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    g_autofree RadioSAudioSnapshot *before = snapshot_take(audio);
    GError *failure = NULL;
    if (radios_audio_engine_play(audio->engine, deck, &failure)) {
        deck_metadata_rebind(audio, deck, before); return TRUE;
    }
    if (deck < RADIOS_AUDIO_DECKS && g_error_matches(failure, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_UNAVAILABLE)) {
        g_autofree RadioSAudioSnapshot *snap = snapshot_take(audio); gint bus = snap->decks[deck].output_bus;
        RadioSAudioState state = snap->decks[deck].state; GtkWidget *owner = g_weak_ref_get(&audio->decks[deck].owner);
        /* A pause/seek recreates output streams asynchronously. Preserve this
         * explicit Play intent across that bounded readiness gap, while real
         * device errors still require an explicit retry. */
        gboolean retry = (deck < RADIOS_AUDIO_NORMAL_DECKS || (owner && !gtk_widget_in_destruction(owner))) && bus >= 0 && bus <= 2 &&
            !snap->error[0] && !snap->buses[bus].error[0] &&
            (state == RADIOS_AUDIO_READY || state == RADIOS_AUDIO_PAUSED || state == RADIOS_AUDIO_LOADING);
        g_clear_object(&owner);
        if (retry) { audio->decks[deck].autoplay = TRUE; audio->decks[deck].generation = snap->decks[deck].generation; audio->decks[deck].loss_epoch = snap->loss_epoch; g_clear_error(&failure); return TRUE; }
    }
    g_propagate_error(error, failure); return FALSE;
}
gboolean radios_audio_ui_pause(RadioSViewContext *ctx, guint deck, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    g_autofree RadioSAudioSnapshot *before = snapshot_take(audio);
    if (deck < RADIOS_AUDIO_DECKS) audio->decks[deck].autoplay = FALSE;
    gboolean accepted = radios_audio_engine_pause(audio->engine, deck, error);
    if (accepted) deck_metadata_rebind(audio, deck, before);
    return accepted;
}
static void deck_metadata_rebind(RadioSAudioController *audio, guint deck, const RadioSAudioSnapshot *before)
{
    if (deck >= RADIOS_AUDIO_DECKS || audio->decks[deck].generation != before->decks[deck].generation) return;
    g_autofree RadioSAudioSnapshot *after = snapshot_take(audio);
    if (g_str_equal(after->decks[deck].media_id, before->decks[deck].media_id) && g_str_equal(after->decks[deck].version_id, before->decks[deck].version_id))
        audio->decks[deck].generation = after->decks[deck].generation;
}
gboolean radios_audio_ui_stop(RadioSViewContext *ctx, guint deck, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    g_autofree RadioSAudioSnapshot *before = snapshot_take(audio);
    if (deck < RADIOS_AUDIO_DECKS) audio->decks[deck].autoplay = FALSE;
    gboolean accepted = radios_audio_engine_stop(audio->engine, deck, error);
    if (accepted) deck_metadata_rebind(audio, deck, before);
    return accepted;
}
gboolean radios_audio_ui_seek(RadioSViewContext *ctx, guint deck, guint64 milliseconds, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    if (deck >= RADIOS_AUDIO_DECKS || milliseconds > audio->decks[deck].duration_ms) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Konum gerçek medya süresi içinde olmalı."); return FALSE; }
    g_autofree RadioSAudioSnapshot *before = snapshot_take(audio);
    gboolean accepted = radios_audio_engine_seek(audio->engine, deck, milliseconds * 48, error);
    if (accepted) deck_metadata_rebind(audio, deck, before);
    return accepted;
}
gboolean radios_audio_ui_seek_if_generation(RadioSViewContext *ctx, guint deck,
    guint64 expected_generation, guint64 milliseconds, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    if (deck >= RADIOS_AUDIO_DECKS || milliseconds > audio->decks[deck].duration_ms || milliseconds > G_MAXUINT64 / 48) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Konum gerçek medya süresi içinde olmalı."); return FALSE;
    }
    g_autofree RadioSAudioSnapshot *before = snapshot_take(audio);
    gboolean accepted = radios_audio_engine_seek_if_generation(audio->engine, deck, expected_generation, milliseconds * 48, error);
    if (accepted) deck_metadata_rebind(audio, deck, before);
    return accepted;
}
gboolean radios_audio_ui_set_deck_gain(RadioSViewContext *ctx, guint deck, gdouble db, GError **error)
{ RadioSAudioController *audio = controller(ctx); return audio ? radios_audio_engine_set_deck_gain(audio->engine, deck, db, error) : unavailable(error); }
gboolean radios_audio_ui_set_deck_main(RadioSViewContext *ctx, guint deck, gboolean enabled, GError **error)
{ RadioSAudioController *audio = controller(ctx); return audio ? radios_audio_engine_set_deck_main(audio->engine, deck, enabled, error) : unavailable(error); }
gboolean radios_audio_ui_sync(RadioSViewContext *ctx, guint deck, gint master, gboolean enabled, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    g_autofree RadioSAudioSnapshot *before = snapshot_take(audio);
    if (!radios_audio_engine_sync(audio->engine, deck, master, enabled, error)) return FALSE;
    deck_metadata_rebind(audio, deck, before); return TRUE;
}
gboolean radios_audio_ui_player_options(RadioSViewContext *ctx, guint deck, RadioSAudioPlayerOptions *options)
{
    RadioSAudioController *audio = controller(ctx);
    if (!audio || !options || deck >= RADIOS_AUDIO_NORMAL_DECKS) return FALSE;
    *options = audio->decks[deck].player_options; return TRUE;
}
gboolean radios_audio_ui_set_player_options(RadioSViewContext *ctx, guint deck, const RadioSAudioPlayerOptions *options, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    if (!options || deck >= 32 || options->sync_master < -1 || options->sync_master >= 32 || options->autodj_target < -1 || options->autodj_target >= 32 ||
        options->sync_master == (gint)deck || options->autodj_target == (gint)deck)
        return g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Farklı bir hedef oynatıcı seçin."), FALSE;
    gint current = options->sync_master;
    for (guint step = 0; current >= 0 && step < 32; ++step) {
        if (current == (gint)deck) return g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "SYNC ana deck bağlantısı döngü oluşturamaz."), FALSE;
        current = audio->decks[current].player_options.sync_master;
    }
    if (!radios_data_document_ready(audio->document)) return g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PENDING, "Ses / AutoDJ ayarlarının veritabanı kaydı henüz hazır değil."), FALSE;
    RadioSAudioPlayerOptions previous = audio->decks[deck].player_options; audio->decks[deck].player_options = *options;
    JsonNode *config = preferences_json(audio); gboolean queued = radios_data_document_save(audio->document, config); json_node_free(config);
    if (!queued) { audio->decks[deck].player_options = previous; return g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "AutoDJ ayarı kayda gönderilemedi; mevcut seçim korundu."), FALSE; }
    return TRUE;
}
gboolean radios_audio_ui_preview_owned(RadioSViewContext *ctx, GtkWidget *owner)
{
    RadioSAudioController *audio = controller(ctx); if (!audio || !audio->preview_active) return FALSE;
    GtkWidget *actual = g_weak_ref_get(&audio->decks[32].owner); gboolean owned = actual && actual == owner; g_clear_object(&actual); return owned;
}
static void preview_output_free(gpointer value)
{
    PreviewOutput *output = value; GtkWidget *owner = g_weak_ref_get(&output->owner);
    if (owner && output->owner_handler && g_signal_handler_is_connected(owner, output->owner_handler))
        g_signal_handler_disconnect(owner, output->owner_handler);
    g_clear_object(&owner); g_weak_ref_clear(&output->owner); g_free(output);
}
static void preview_output_destroyed(GtkWidget *owner, gpointer value)
{
    PreviewOutput *output = value; RadioSAudioController *audio = output->audio;
    output->owner_handler = 0;
    if (!audio->closed) radios_audio_ui_preview_stop(audio->ctx, owner);
    g_ptr_array_remove(audio->preview_outputs, output);
}
static PreviewOutput *preview_output(RadioSAudioController *audio, GtkWidget *owner, gboolean create)
{
    if (!GTK_IS_WIDGET(owner) || gtk_widget_in_destruction(owner)) return NULL;
    for (guint i = 0; i < audio->preview_outputs->len; ++i) {
        PreviewOutput *output = g_ptr_array_index(audio->preview_outputs, i);
        GtkWidget *actual = g_weak_ref_get(&output->owner); gboolean matches = actual == owner;
        g_clear_object(&actual); if (matches) return output;
    }
    if (!create) return NULL;
    PreviewOutput *output = g_new0(PreviewOutput, 1); output->audio = audio; output->bus = RADIOS_AUDIO_UI_CUE;
    g_weak_ref_init(&output->owner, owner);
    output->owner_handler = g_signal_connect(owner, "destroy", G_CALLBACK(preview_output_destroyed), output);
    g_ptr_array_add(audio->preview_outputs, output); return output;
}
gboolean radios_audio_ui_preview_get_output(RadioSViewContext *ctx, GtkWidget *owner,
    RadioSAudioUiBus *bus, gdouble *gain_db)
{
    RadioSAudioController *audio = controller(ctx);
    if (!audio || !GTK_IS_WIDGET(owner) || gtk_widget_in_destruction(owner)) return FALSE;
    PreviewOutput *output = preview_output(audio, owner, FALSE);
    if (bus) *bus = output ? output->bus : RADIOS_AUDIO_UI_CUE;
    if (gain_db) *gain_db = output ? output->gain_db : 0;
    return TRUE;
}
gboolean radios_audio_ui_preview_set_output(RadioSViewContext *ctx, GtkWidget *owner,
    RadioSAudioUiBus bus, gdouble gain_db, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    if ((bus != RADIOS_AUDIO_UI_CUE && bus != RADIOS_AUDIO_UI_MONITOR) ||
        !isfinite(gain_db) || gain_db < -60 || gain_db > 6 || !GTK_IS_WIDGET(owner) || gtk_widget_in_destruction(owner)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Önizleme çıkışı kulaklık veya monitör; ses düzeyi -60…6 dB olmalı."); return FALSE;
    }
    gboolean owned = radios_audio_ui_preview_owned(ctx, owner);
    PreviewOutput *output = preview_output(audio, owner, TRUE);
    if (owned && output->bus != bus && !radios_audio_ui_ready(ctx, bus)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
            bus == RADIOS_AUDIO_UI_CUE ? "Kulaklık (CUE) çıkış aygıtı hazır değil." : "Monitör çıkış aygıtı hazır değil."); return FALSE;
    }
    if (owned && !radios_audio_engine_set_preview_output(audio->engine, (RadioSAudioBus)bus, gain_db, TRUE, error)) return FALSE;
    output->bus = bus; output->gain_db = gain_db; return TRUE;
}
void radios_audio_ui_preview_stop(RadioSViewContext *ctx, GtkWidget *owner)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return;
    GtkWidget *pending = g_weak_ref_get(&audio->transition_owner);
    gboolean pending_owned = pending && (!owner || pending == owner); g_clear_object(&pending);
    if (!owner || pending_owned) { ++audio->preview_intent; transition_cancel(audio); }
    if (!audio->preview_active || (owner && !radios_audio_ui_preview_owned(ctx, owner))) return;
    audio->preview_pause_pending = FALSE;
    audio->decks[32].autoplay = FALSE; ++audio->decks[32].epoch; owner_clear(&audio->decks[32]);
    if (audio->preview_file) radios_audio_engine_unload(audio->engine, 32, NULL);
    else radios_audio_engine_stop(audio->engine, 32, NULL);
    audio->preview_active = FALSE;
    radios_audio_engine_set_preview_output(audio->engine, RADIOS_AUDIO_BUS_CUE, 0, FALSE, NULL);
    if (audio->preview_file) { preview_file_delete(audio->preview_file); g_clear_pointer(&audio->preview_file, g_free); }
}
static gboolean preview_prepare(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail, guint64 start_ms, guint64 end_ms, gboolean autoplay, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    PreviewOutput *output = preview_output(audio, owner, TRUE);
    if (!output) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Önizleme sahibi geçersiz."); return FALSE; }
    if (!radios_audio_ui_ready(ctx, output->bus)) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED,
        output->bus == RADIOS_AUDIO_UI_CUE ? "Ön dinleme için Teknik yönetimden CUE çıkışını seç." : "Ön dinleme için Teknik yönetimden monitör çıkışını seç."); return FALSE; }
    RadioSAudioLoadSpec spec = {0}; guint64 duration = 0; gchar *path = NULL;
    if (!detail_spec(audio, detail, &spec, &duration, &path, error)) return FALSE;
    if (!end_ms) end_ms = duration;
    if (start_ms >= end_ms || end_ms > duration) { g_free(path); g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Ön dinleme aralığı gerçek medya süresi içinde olmalı."); return FALSE; }
    radios_audio_ui_preview_stop(ctx, NULL); audio->preview_active = TRUE;
    spec.cue_in_frame = start_ms * 48; spec.cue_out_frame = end_ms * 48;
    gboolean accepted = radios_audio_engine_set_preview_output(audio->engine, (RadioSAudioBus)output->bus, output->gain_db, TRUE, error) &&
        load_spec(audio, owner, &spec, 32, autoplay, duration, error);
    if (!accepted) radios_audio_ui_preview_stop(ctx, NULL);
    else {
        /* A catalog audition hears the same LUFS correction as the players:
         * bind the stored measurement to this preview load generation. */
        JsonObject *analysis = read_object(detail, "analysis"); RadioSLoudness loudness;
        if (radios_loudness_read(analysis, &loudness))
            radios_audio_engine_set_deck_loudness(audio->engine, RADIOS_AUDIO_PREVIEW_DECK,
                audio->decks[RADIOS_AUDIO_PREVIEW_DECK].generation, &loudness, NULL);
    }
    g_free(path); return accepted;
}

gboolean radios_audio_ui_preview(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail, guint64 start_ms, guint64 end_ms, GError **error)
{
    return preview_prepare(ctx, owner, detail, start_ms, end_ms, TRUE, error);
}

static gboolean preview_matches(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail)
{
    RadioSAudioUiSnapshot snapshot = {0};
    JsonObject *media = read_object(detail, "media"), *version = read_object(detail, "version");
    return radios_audio_ui_preview_owned(ctx, owner) &&
        radios_audio_ui_get_snapshot(ctx, RADIOS_AUDIO_PREVIEW_DECK, &snapshot) &&
        g_str_equal(snapshot.media_id, radios_json_string(media, "_id", "")) &&
        g_str_equal(snapshot.version_id, radios_json_string(version, "_id", ""));
}

/* A loaded editor interval remains authoritative while seeking or cueing.
 * Full-file previews still permit the complete measured waveform. */
static gboolean preview_position(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail,
                                 guint64 milliseconds, gboolean autoplay, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    JsonObject *version = read_object(detail, "version");
    guint64 duration = (guint64)MAX(0, radios_json_int(version, "duration_ms", 0));
    if (!duration || milliseconds > duration) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Konum gerçek medya süresi içinde olmalı."); return FALSE;
    }
    if (!preview_matches(ctx, owner, detail)) {
        if (!preview_prepare(ctx, owner, detail, 0, 0, FALSE, error)) return FALSE;
    }
    RadioSAudioUiSnapshot loaded = {0};
    if (radios_audio_ui_get_snapshot(ctx, RADIOS_AUDIO_PREVIEW_DECK, &loaded)) {
        guint64 out = loaded.cue_out_ms ? loaded.cue_out_ms : duration;
        milliseconds = CLAMP(milliseconds, loaded.cue_in_ms, out);
        if (autoplay && milliseconds >= out) milliseconds = loaded.cue_in_ms;
    }
    if (!autoplay && !radios_audio_ui_pause(ctx, RADIOS_AUDIO_PREVIEW_DECK, error)) return FALSE;
    if (!radios_audio_ui_seek(ctx, RADIOS_AUDIO_PREVIEW_DECK, milliseconds, error)) return FALSE;
    audio->preview_pause_pending = !autoplay;
    if (autoplay && !radios_audio_ui_play(ctx, RADIOS_AUDIO_PREVIEW_DECK, error)) return FALSE;
    return TRUE;
}

gboolean radios_audio_ui_preview_play(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail,
                                     guint64 milliseconds, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    if (preview_matches(ctx, owner, detail)) {
        RadioSAudioUiSnapshot snap = {0};
        if (radios_audio_ui_get_snapshot(ctx, RADIOS_AUDIO_PREVIEW_DECK, &snap) &&
            (snap.state == RADIOS_AUDIO_UI_READY || snap.state == RADIOS_AUDIO_UI_PAUSED ||
             snap.state == RADIOS_AUDIO_UI_PLAYING || snap.state == RADIOS_AUDIO_UI_LOADING)) {
            audio->preview_pause_pending = FALSE;
            return radios_audio_ui_play(ctx, RADIOS_AUDIO_PREVIEW_DECK, error);
        }
    }
    JsonObject *version = read_object(detail, "version");
    guint64 duration = (guint64)MAX(0, radios_json_int(version, "duration_ms", 0));
    return preview_position(ctx, owner, detail, milliseconds >= duration ? 0 : milliseconds, TRUE, error);
}
gboolean radios_audio_ui_preview_play_range(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail,
    guint64 milliseconds, guint64 start_ms, guint64 end_ms, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    JsonObject *version = read_object(detail, "version");
    guint64 duration = (guint64)MAX(0, radios_json_int(version, "duration_ms", 0));
    if (!end_ms) end_ms = duration;
    if (!duration || start_ms >= end_ms || end_ms > duration) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Ön dinleme aralığı gerçek medya süresi içinde olmalı."); return FALSE;
    }
    RadioSAudioUiSnapshot snapshot = {0};
    gboolean same_range = preview_matches(ctx, owner, detail) &&
        radios_audio_ui_get_snapshot(ctx, RADIOS_AUDIO_PREVIEW_DECK, &snapshot) &&
        snapshot.cue_in_ms == start_ms && snapshot.cue_out_ms == end_ms;
    if (same_range && (snapshot.state == RADIOS_AUDIO_UI_READY || snapshot.state == RADIOS_AUDIO_UI_PAUSED ||
        snapshot.state == RADIOS_AUDIO_UI_PLAYING || snapshot.state == RADIOS_AUDIO_UI_LOADING)) {
        audio->preview_pause_pending = FALSE; return radios_audio_ui_play(ctx, RADIOS_AUDIO_PREVIEW_DECK, error);
    }
    if (!preview_prepare(ctx, owner, detail, start_ms, end_ms, FALSE, error)) return FALSE;
    if (milliseconds < start_ms || milliseconds >= end_ms) milliseconds = start_ms;
    return preview_position(ctx, owner, detail, milliseconds, TRUE, error);
}

gboolean radios_audio_ui_preview_pause(RadioSViewContext *ctx, GtkWidget *owner, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    if (!radios_audio_ui_preview_owned(ctx, owner)) return TRUE;
    audio->preview_pause_pending = TRUE;
    return radios_audio_ui_pause(ctx, RADIOS_AUDIO_PREVIEW_DECK, error);
}

gboolean radios_audio_ui_preview_cue(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail,
                                    guint64 cue_ms, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    if (audio->preview_active && !preview_matches(ctx, owner, detail)) return TRUE;
    return preview_position(ctx, owner, detail, cue_ms, FALSE, error);
}

gboolean radios_audio_ui_preview_seek(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *detail,
                                     guint64 milliseconds, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    /* Moving an inactive editor's cursor must never seek or replace another
     * widget's exclusive audition. Its next explicit Play uses that cursor. */
    if (!preview_matches(ctx, owner, detail)) return TRUE;
    g_autofree RadioSAudioSnapshot *before = snapshot_take(audio);
    gboolean playing = before->decks[RADIOS_AUDIO_PREVIEW_DECK].state == RADIOS_AUDIO_PLAYING ||
                       audio->decks[RADIOS_AUDIO_PREVIEW_DECK].autoplay;
    return preview_position(ctx, owner, detail, milliseconds, playing, error);
}


typedef struct {
    RadioSAudioController *audio;
    GWeakRef owner;
    guint64 intent, loss_epoch;
    RadioSAudioTransitionMedia settings;
    gchar *paths[2], *hashes[2], *directory, *result_path, *result_hash;
    guint64 frames;
    gboolean completed;
} TransitionJob;
static void transition_cancel(RadioSAudioController *audio)
{
    if (audio->transition_cancel) g_cancellable_cancel(audio->transition_cancel);
    g_clear_object(&audio->transition_cancel);
    GObject *owner = g_weak_ref_get(&audio->transition_owner);
    if (owner) {
        if (audio->transition_handler && g_signal_handler_is_connected(owner, audio->transition_handler)) g_signal_handler_disconnect(owner, audio->transition_handler);
        g_object_unref(owner);
    }
    audio->transition_handler = 0; g_weak_ref_set(&audio->transition_owner, NULL);
}
static void transition_owner_destroyed(GtkWidget *widget, gpointer value)
{ RadioSAudioController *audio = value; if (!audio->closed) radios_audio_ui_preview_stop(audio->ctx, widget); }
static void transition_job_free(gpointer value)
{
    TransitionJob *job = value;
    if (job->result_path) { preview_file_delete(job->result_path); g_free(job->result_path); }
    for (guint i = 0; i < 2; ++i) { g_free(job->paths[i]); g_free(job->hashes[i]); }
    g_free(job->directory); g_free(job->result_hash); g_weak_ref_clear(&job->owner); radios_audio_ui_unref(job->audio); g_free(job);
}
static gchar *transition_worker(gpointer value, const RadioSCancelToken *token, GError **unused)
{
    (void)unused; GTask *task = value; TransitionJob *job = g_task_get_task_data(task); GError *error = NULL;
    GCancellable *cancel = g_task_get_cancellable(task); if (radios_cancelled(token)) g_cancellable_cancel(cancel);
    job->completed = TRUE;
    if (radios_audio_transition_prepare(&job->settings, job->directory, cancel, &job->result_path, &job->result_hash, &job->frames, &error)) g_task_return_boolean(task, TRUE);
    else g_task_return_error(task, error);
    return NULL;
}
static void transition_task_destroy(gpointer value)
{
    GTask *task = value; TransitionJob *job = g_task_get_task_data(task);
    if (!job->completed) {
        job->completed = TRUE; g_task_return_new_error(task, G_IO_ERROR, G_IO_ERROR_CANCELLED, "Geçiş önizleme işi iptal edildi.");
    }
    g_object_unref(task);
}
static void transition_done(GObject *source, GAsyncResult *result, gpointer unused)
{
    (void)source; (void)unused; GTask *task = G_TASK(result); TransitionJob *job = g_task_get_task_data(task);
    RadioSAudioController *audio = job->audio; GError *error = NULL; gboolean success = g_task_propagate_boolean(task, &error);
    GtkWidget *owner = g_weak_ref_get(&job->owner);
    g_autofree RadioSAudioSnapshot *physical = snapshot_take(audio);
    gboolean current = !audio->closed && job->intent == audio->preview_intent && job->loss_epoch == physical->loss_epoch && owner && !gtk_widget_in_destruction(owner);
    if (current) {
        transition_cancel(audio);
        if (success) {
            PreviewOutput *output = preview_output(audio, owner, TRUE);
            audio->preview_active = TRUE;
            RadioSAudioLoadSpec spec = { .path = job->result_path, .sha256 = job->result_hash, .cue_out_frame = job->frames };
            /* Transient preview PCM has no catalog UUID and never grants on-air
             * evidence. The source pair was checked before worker preparation;
             * the generated private WAV is independently hashed by the decoder. */
            success = output && radios_audio_ui_ready(audio->ctx, output->bus) &&
                radios_audio_engine_set_preview_output(audio->engine, (RadioSAudioBus)output->bus, output->gain_db, TRUE, &error) &&
                load_spec(audio, owner, &spec, 32, TRUE, job->frames / 48, &error);
            if (success) audio->preview_file = g_steal_pointer(&job->result_path);
            if (!success) radios_audio_ui_preview_stop(audio->ctx, owner);
        }
        if (error && !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) notify_audio(audio, error->message);
    }
    g_clear_object(&owner); g_clear_error(&error);
}
static guint64 detail_output_frames(JsonObject *detail, guint64 duration_ms)
{ return radios_media_geometry(detail, (gint64)duration_ms, FALSE).frames; }

gboolean radios_audio_ui_transition_preview(RadioSViewContext *ctx, GtkWidget *owner,
    JsonObject *detail_a, JsonObject *detail_b, guint64 cue_a_ms, guint64 out_a_ms,
    guint64 cue_b_ms, guint64 fade_in_ms, guint64 overlap_ms, gdouble duck_db, gdouble level_db, GError **error)
{
    RadioSAudioController *audio = controller(ctx); if (!audio) return unavailable(error);
    if (!GTK_IS_WIDGET(owner) || gtk_widget_in_destruction(owner)) return unavailable(error);
    PreviewOutput *output = preview_output(audio, owner, TRUE);
    if (!output || !radios_audio_ui_ready(ctx, output->bus)) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "Geçiş önizlemesi için seçilen kulaklık veya monitör çıkışını ata."); return FALSE; }
    RadioSAudioLoadSpec specs[2] = {{0}}; guint64 durations[2] = {0}; gchar *paths[2] = {NULL};
    if (!detail_spec(audio, detail_a, &specs[0], &durations[0], &paths[0], error) ||
        !detail_spec(audio, detail_b, &specs[1], &durations[1], &paths[1], error)) { g_free(paths[0]); g_free(paths[1]); return FALSE; }
    if (cue_a_ms > durations[0] || out_a_ms > durations[0] || cue_b_ms > durations[1] || fade_in_ms > 30000 || overlap_ms > 30000) {
        g_free(paths[0]); g_free(paths[1]); g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Geçiş gerçek medya sınırlarını aşıyor."); return FALSE;
    }
    guint64 frames_a = detail_output_frames(detail_a, durations[0]), frames_b = detail_output_frames(detail_b, durations[1]);
    RadioSAudioTransitionMedia settings = { .path_a = paths[0], .sha_a = specs[0].sha256, .path_b = paths[1], .sha_b = specs[1].sha256,
        .duration_a = frames_a, .duration_b = frames_b, .cue_a = cue_a_ms * 48, .out_a = MIN(out_a_ms * 48, frames_a),
        .cue_b = cue_b_ms * 48, .fade_in = fade_in_ms * 48, .overlap = overlap_ms * 48, .duck_db = duck_db, .level_db = level_db };
    RadioSAudioTransitionPlan plan;
    if (!radios_audio_transition_plan(&settings, &plan, error)) { g_free(paths[0]); g_free(paths[1]); return FALSE; }
    radios_audio_ui_preview_stop(ctx, NULL);
    TransitionJob *job = g_new0(TransitionJob, 1); job->audio = radios_audio_ui_ref(audio); job->intent = ++audio->preview_intent;
    g_autofree RadioSAudioSnapshot *physical = snapshot_take(audio); job->loss_epoch = physical->loss_epoch;
    g_weak_ref_init(&job->owner, owner); job->settings = settings;
    for (guint i = 0; i < 2; ++i) { job->paths[i] = paths[i]; job->hashes[i] = g_strdup(specs[i].sha256); }
    job->settings.sha_a = job->hashes[0]; job->settings.sha_b = job->hashes[1];
    job->directory = g_build_filename(ctx->config->cache_root, "transition-preview", NULL);
    audio->transition_cancel = g_cancellable_new(); g_weak_ref_set(&audio->transition_owner, owner);
    audio->transition_handler = g_signal_connect(owner, "destroy", G_CALLBACK(transition_owner_destroyed), audio);
    GTask *task = g_task_new(NULL, audio->transition_cancel, transition_done, NULL);
    g_task_set_task_data(task, job, transition_job_free);
    if (!ctx->runtime || !radios_runtime_submit(ctx->runtime, transition_worker, task, transition_task_destroy, error)) {
        transition_cancel(audio); ++audio->preview_intent; transition_task_destroy(task); return FALSE;
    }
    return TRUE;
}

static GtkWidget *field(GtkWidget *page, const gchar *name)
{ return g_object_get_data(G_OBJECT(page), name); }
static void binding_destroyed(GtkWidget *widget, gpointer value)
{ (void)widget; ((AudioBinding *)value)->dead = TRUE; }
static void bind_signal(AudioBinding *binding, gpointer object, const gchar *signal, GCallback callback)
{
    if (!object) return;
    SignalLink *link = g_new0(SignalLink, 1); g_weak_ref_init(&link->object, object);
    link->handler = g_signal_connect(object, signal, callback, binding); g_ptr_array_add(binding->signals, link);
}
static AudioBinding *binding_new(RadioSAudioController *audio, GtkWidget *widget, BindingKind kind, RadioSAudioUiBus bus)
{
    AudioBinding *binding = g_new0(AudioBinding, 1); binding->audio = audio; binding->kind = kind; binding->bus = bus;
    g_weak_ref_init(&binding->widget, widget); g_weak_ref_init(&binding->control, NULL);
    g_weak_ref_init(&binding->destination, NULL); g_weak_ref_init(&binding->toggle, NULL);
    binding->signals = g_ptr_array_new(); g_ptr_array_add(audio->bindings, binding);
    bind_signal(binding, widget, "destroy", G_CALLBACK(binding_destroyed)); return binding;
}
static guint devices_hash(const RadioSAudioSnapshot *snapshot)
{
    guint value = 5381;
    for (guint i = 0; i < snapshot->output_count; ++i) value = (value * 33) ^ g_str_hash(snapshot->outputs[i].name) ^ g_str_hash(snapshot->outputs[i].description);
    for (guint i = 0; i < snapshot->input_count; ++i) value = (value * 33) ^ g_str_hash(snapshot->inputs[i].name) ^ g_str_hash(snapshot->inputs[i].description);
    return value ^ snapshot->output_count ^ (snapshot->input_count << 8);
}
static void devices_fill(GtkWidget *combo, const RadioSAudioSnapshot *snapshot, gboolean input, const gchar *selected)
{
    if (!GTK_IS_COMBO_BOX_TEXT(combo)) return;
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "", "Atanmadı");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "@default", input ? "Sistem varsayılan girişi" : "Sistem varsayılan çıkışı");
    guint count = input ? snapshot->input_count : snapshot->output_count;
    const RadioSAudioDevice *devices = input ? snapshot->inputs : snapshot->outputs;
    gboolean found = !selected || !*selected || g_str_equal(selected, "@default");
    for (guint i = 0; i < count; ++i) {
        gchar *label = g_strdup_printf("%s%s", devices[i].description[0] ? devices[i].description : devices[i].name,
            input && devices[i].monitor ? " · Çıkış geri dinlemesi" : "");
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), devices[i].name, label); g_free(label);
        if (selected && g_str_equal(devices[i].name, selected)) found = TRUE;
    }
    if (!found) {
        gchar *label = g_strdup_printf("Bağlı değil · %s", selected);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), selected, label); g_free(label);
    }
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(combo), selected ? selected : "");
}
static void sources_fill(GtkWidget *combo)
{
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "-1", "Ana miks");
    for (guint i = 0; i < 32; ++i) {
        gchar *id = g_strdup_printf("%u", i), *label = g_strdup_printf("Oynatıcı %u", i + 1);
        gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), id, label); g_free(id); g_free(label);
    }
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo), "32", "Medya ön dinleme");
}
static void report_error(RadioSAudioController *audio, GError *error)
{ if (error) { notify_audio(audio, error->message); g_error_free(error); } }
static void gain_changed(GtkAdjustment *adjustment, gpointer value)
{
    AudioBinding *binding = value; RadioSAudioController *audio = binding->audio;
    if (binding->dead || audio->closed || audio->updating) return;
    gdouble *gain = gain_pointer(&audio->preferences, binding->bus); if (!gain) return;
    gdouble old = *gain; *gain = gtk_adjustment_get_value(adjustment);
    GError *error = NULL;
    if (!apply_engine(audio, &error)) { *gain = old; report_error(audio, error); return; }
    if (*gain != old) preferences_changed(audio, PREF_GAIN(binding->bus));
}
static void source_changed(GtkComboBox *combo, gpointer value)
{
    AudioBinding *binding = value; RadioSAudioController *audio = binding->audio;
    if (binding->dead || audio->closed || audio->updating) return;
    const gchar *id = gtk_combo_box_get_active_id(combo); if (!id) return;
    gchar *end = NULL; gint64 source = g_ascii_strtoll(id, &end, 10);
    if (!end || *end || source < -1 || source > 32) return;
    if (binding->bus == RADIOS_AUDIO_UI_CUE) {
        radios_audio_ui_preview_stop(audio->ctx, NULL); audio->preferences.cue_source = (gint)source;
    } else audio->preferences.monitor_source = (gint)source;
    preferences_changed(audio, binding->bus == RADIOS_AUDIO_UI_CUE ? PREF_CUE_SOURCE : PREF_MONITOR_SOURCE);
    GError *error = NULL; apply_engine(audio, &error); report_error(audio, error);
}
static void destination_changed(GtkComboBox *combo, gpointer value)
{
    AudioBinding *binding = value; RadioSAudioController *audio = binding->audio;
    if (binding->dead || audio->closed || audio->updating) return;
    const gchar *id = gtk_combo_box_get_active_id(combo); if (!id || strlen(id) >= RADIOS_AUDIO_DEVICE_NAME_SIZE) return;
    g_strlcpy(audio->preferences.monitor_sink, id, sizeof audio->preferences.monitor_sink);
    preferences_changed(audio, PREF_MONITOR_SINK);
    GError *error = NULL; apply_engine(audio, &error); report_error(audio, error);
}
static void bus_toggled(GtkToggleButton *toggle, gpointer value)
{
    AudioBinding *binding = value; RadioSAudioController *audio = binding->audio;
    if (binding->dead || audio->closed || audio->updating) return;
    if (audio->preview_active) {
        g_autofree RadioSAudioSnapshot *snapshot = snapshot_take(audio);
        if (snapshot->decks[RADIOS_AUDIO_PREVIEW_DECK].output_bus == (gint)binding->bus)
            radios_audio_ui_preview_stop(audio->ctx, NULL);
    }
    GError *error = NULL; radios_audio_engine_set_bus_gate(audio->engine, (RadioSAudioBus)binding->bus,
        gtk_toggle_button_get_active(toggle), &error); report_error(audio, error);
}
static void device_changed(GtkComboBox *combo, gpointer value)
{
    (void)combo; AudioBinding *binding = value; RadioSAudioController *audio = binding->audio;
    if (!binding->dead && !audio->closed && !audio->updating) { audio->settings_dirty = TRUE; mark_dirty(audio); }
}
static void microphone_toggled(GtkToggleButton *toggle, gpointer value)
{
    AudioBinding *binding = value; RadioSAudioController *audio = binding->audio;
    if (binding->dead || audio->closed || audio->updating) return;
    GError *error = NULL; radios_audio_engine_set_microphone_gate(audio->engine, gtk_toggle_button_get_active(toggle), &error); report_error(audio, error);
}
static void refresh_clicked(GtkButton *button, gpointer value)
{
    (void)button; AudioBinding *binding = value;
    if (!binding->dead && !binding->audio->closed) radios_audio_engine_refresh_devices(binding->audio->engine);
}
static void apply_clicked(GtkButton *button, gpointer value)
{
    (void)button; AudioBinding *binding = value; RadioSAudioController *audio = binding->audio;
    if (binding->dead || audio->closed) return;
    GtkWidget *page = g_weak_ref_get(&binding->widget); if (!page) return;
    RadioSAudioPreferences previous = audio->preferences, next = previous;
    const gchar *ids[] = {"task-main-output", "task-cue-output", "task-monitor-output", "task-microphone-input"};
    gchar *destinations[] = {next.main_sink, next.cue_sink, next.monitor_sink, next.microphone_source}; gboolean valid = TRUE;
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) {
        const gchar *name = gtk_combo_box_get_active_id(GTK_COMBO_BOX(field(page, ids[i])));
        if (!name || strlen(name) >= RADIOS_AUDIO_DEVICE_NAME_SIZE) { valid = FALSE; break; }
        g_strlcpy(destinations[i], name, RADIOS_AUDIO_DEVICE_NAME_SIZE);
    }
    if (!valid) { notify_audio(audio, "Geçerli bir ses aygıtı seç."); g_object_unref(page); return; }
    if (!g_str_equal(previous.cue_sink, next.cue_sink)) radios_audio_ui_preview_stop(audio->ctx, NULL);
    if (!g_str_equal(previous.microphone_source, next.microphone_source)) radios_audio_engine_set_microphone_gate(audio->engine, FALSE, NULL);
    audio->preferences = next; GError *error = NULL;
    if (!apply_engine(audio, &error)) { audio->preferences = previous; report_error(audio, error); g_object_unref(page); return; }
    mark_dirty(audio);
    if (audio->document && radios_data_document_ready(audio->document)) {
        JsonNode *config = preferences_json(audio); gboolean queued = radios_data_document_save(audio->document, config); json_node_free(config);
        if (queued) { audio->settings_dirty = FALSE; notify_audio(audio, "Ses seçimleri uygulandı; veritabanı kaydı bekleniyor."); }
    } else notify_audio(audio, "Ses seçimleri uygulandı. Veritabanı bağlantısı kurulunca kaydedebilirsin.");
    g_object_unref(page);
}
static void binding_update(AudioBinding *binding, const RadioSAudioSnapshot *snapshot, gboolean force)
{
    RadioSAudioController *audio = binding->audio; if (binding->dead || audio->closed) return;
    GtkWidget *widget = g_weak_ref_get(&binding->widget); if (!widget) return;
    gboolean was_updating = audio->updating; audio->updating = TRUE;
    if (binding->kind == BIND_METER) {
        gdouble peak[2] = {0, 0}, rms[2] = {0, 0}; gboolean ready = bus_ready(audio, snapshot, binding->bus);
        if (ready) {
            if (binding->bus == RADIOS_AUDIO_UI_MICROPHONE) { peak[0] = snapshot->microphone_peak[0]; peak[1] = snapshot->microphone_peak[1]; }
            else { for (guint i = 0; i < 2; ++i) { peak[i] = snapshot->buses[binding->bus].peak[i]; rms[i] = snapshot->buses[binding->bus].rms[i]; } }
        }
        radios_meter_set_levels(widget, peak[0], peak[1], rms[0], rms[1], peak[0] >= 1.0 || peak[1] >= 1.0);
        if (!ready) gtk_widget_set_tooltip_text(widget, "Ses çıkışı bağlı değil; ölçüm yok.");
    } else if (binding->kind == BIND_GAIN) {
        GtkAdjustment *adjustment = g_weak_ref_get(&binding->control); gdouble *gain = gain_pointer(&audio->preferences, binding->bus);
        if (adjustment && gain) gtk_adjustment_set_value(adjustment, *gain);
        g_clear_object(&adjustment);
    } else if (binding->kind == BIND_MONITOR) {
        GtkWidget *source = g_weak_ref_get(&binding->control), *destination = g_weak_ref_get(&binding->destination), *toggle = g_weak_ref_get(&binding->toggle);
        if (source) {
            gint selected = binding->bus == RADIOS_AUDIO_UI_CUE ? audio->preferences.cue_source : audio->preferences.monitor_source;
            gchar id[16]; g_snprintf(id, sizeof id, "%d", selected); gtk_combo_box_set_active_id(GTK_COMBO_BOX(source), id);
        }
        guint hash = devices_hash(snapshot);
        if (destination && (force || binding->device_hash != hash)) devices_fill(destination, snapshot, FALSE, audio->preferences.monitor_sink);
        if (destination) {
            gchar *description = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(destination));
            const gchar *name = gtk_combo_box_get_active_id(GTK_COMBO_BOX(destination));
            gchar *tooltip = g_strdup_printf("%s%s%s", description ? description : "Atanmadı", name && *name ? " · " : "", name ? name : "");
            gtk_widget_set_tooltip_text(destination, tooltip); g_free(tooltip); g_free(description);
        }
        binding->device_hash = hash;
        if (toggle) { gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(toggle), snapshot->buses[binding->bus].gate); gtk_widget_set_sensitive(toggle, bus_ready(audio, snapshot, binding->bus)); }
        gtk_widget_set_tooltip_text(widget, bus_ready(audio, snapshot, binding->bus) ? "Gerçek ses motoru çıkışı." : "Teknik yönetimden bir ses çıkışı seç.");
        g_clear_object(&source); g_clear_object(&destination); g_clear_object(&toggle);
    } else {
        guint hash = devices_hash(snapshot);
        if (force || binding->device_hash != hash) {
            const gchar *ids[] = {"task-main-output", "task-cue-output", "task-monitor-output", "task-microphone-input"};
            const gchar *configured[] = {audio->preferences.main_sink, audio->preferences.cue_sink, audio->preferences.monitor_sink, audio->preferences.microphone_source};
            for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) {
                GtkWidget *combo = field(widget, ids[i]);
                gchar *selected = g_strdup(audio->settings_dirty ? gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo)) : configured[i]);
                devices_fill(combo, snapshot, i == 3, selected); g_free(selected);
            }
            binding->device_hash = hash;
        }
        GtkWidget *microphone = field(widget, "task-microphone-gate");
        gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(microphone), snapshot->microphone_gate);
        gtk_button_set_label(GTK_BUTTON(microphone), snapshot->microphone_gate ? "Mikrofon açık" : "Mikrofon kapalı");
        gtk_widget_set_sensitive(microphone, snapshot->microphone_ready);
        gtk_widget_set_sensitive(field(widget, "task-audio-apply"), audio->document && radios_data_document_ready(audio->document));
        gchar *status = snapshot->connected ? g_strdup_printf("Ana çıkış: %s · CUE: %s · Monitör: %s · Mikrofon: %s",
            bus_ready(audio, snapshot, RADIOS_AUDIO_UI_MAIN) ? "bağlı" : "bağlı değil", bus_ready(audio, snapshot, RADIOS_AUDIO_UI_CUE) ? "bağlı" : "atanmadı / bağlı değil",
            bus_ready(audio, snapshot, RADIOS_AUDIO_UI_MONITOR) ? "bağlı" : "atanmadı / bağlı değil", snapshot->microphone_gate ? "açık" : "kapalı") :
            g_strdup("Windows ses hizmetine bağlı değil; hizmet açılınca kendiliğinden bağlanır.");
        gtk_label_set_text(GTK_LABEL(field(widget, "task-audio-status")), status); g_free(status);
    }
    audio->updating = was_updating; g_object_unref(widget);
}
void radios_audio_ui_bind_meter(RadioSViewContext *ctx, GtkWidget *meter, RadioSAudioUiBus bus)
{
    RadioSAudioController *audio = controller(ctx); if (!audio || !meter || bus < RADIOS_AUDIO_UI_MAIN || bus > RADIOS_AUDIO_UI_MICROPHONE) return;
    AudioBinding *binding = binding_new(audio, meter, BIND_METER, bus); g_autofree RadioSAudioSnapshot *snapshot = snapshot_take(audio); binding_update(binding, snapshot, TRUE);
}
void radios_audio_ui_bind_gain(RadioSViewContext *ctx, GtkWidget *knob, RadioSAudioUiBus bus)
{
    RadioSAudioController *audio = controller(ctx); if (!audio || !knob || bus < RADIOS_AUDIO_UI_MAIN || bus > RADIOS_AUDIO_UI_MICROPHONE) return;
    GtkAdjustment *adjustment = g_object_get_data(G_OBJECT(knob), "adjustment"); if (!adjustment) return;
    AudioBinding *binding = binding_new(audio, knob, BIND_GAIN, bus); g_weak_ref_set(&binding->control, adjustment);
    bind_signal(binding, adjustment, "value-changed", G_CALLBACK(gain_changed));
    GtkWidget *dial = g_object_get_data(G_OBJECT(knob), "dial"); if (dial) gtk_widget_set_tooltip_text(dial, "Gerçek ses motoru kazancı. Değişiklikler otomatik kaydedilir.");
    g_autofree RadioSAudioSnapshot *snapshot = snapshot_take(audio); binding_update(binding, snapshot, TRUE);
}
void radios_audio_ui_bind_monitor(RadioSViewContext *ctx, GtkWidget *module, GtkWidget *source, GtkWidget *destination, GtkWidget *toggle, RadioSAudioUiBus bus)
{
    RadioSAudioController *audio = controller(ctx); if (!audio || (bus != RADIOS_AUDIO_UI_CUE && bus != RADIOS_AUDIO_UI_MONITOR)) return;
    AudioBinding *binding = binding_new(audio, module, BIND_MONITOR, bus);
    g_weak_ref_set(&binding->control, source); g_weak_ref_set(&binding->destination, destination); g_weak_ref_set(&binding->toggle, toggle);
    audio->updating = TRUE; sources_fill(source); audio->updating = FALSE;
    bind_signal(binding, source, "changed", G_CALLBACK(source_changed)); bind_signal(binding, destination, "changed", G_CALLBACK(destination_changed));
    bind_signal(binding, toggle, "toggled", G_CALLBACK(bus_toggled));
    g_autofree RadioSAudioSnapshot *snapshot = snapshot_take(audio); binding_update(binding, snapshot, TRUE);
}
void radios_audio_ui_bind_settings(RadioSViewContext *ctx, GtkWidget *page)
{
    RadioSAudioController *audio = controller(ctx);
    if (!audio) {
        const gchar *ids[] = {"task-audio-refresh", "task-audio-apply", "task-microphone-gate", "task-microphone-gain"};
        for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) if (field(page, ids[i])) gtk_widget_set_sensitive(field(page, ids[i]), FALSE);
        return;
    }
    AudioBinding *binding = binding_new(audio, page, BIND_SETTINGS, RADIOS_AUDIO_UI_MAIN);
    const gchar *ids[] = {"task-main-output", "task-cue-output", "task-monitor-output", "task-microphone-input"};
    for (guint i = 0; i < G_N_ELEMENTS(ids); ++i) bind_signal(binding, field(page, ids[i]), "changed", G_CALLBACK(device_changed));
    bind_signal(binding, field(page, "task-audio-refresh"), "clicked", G_CALLBACK(refresh_clicked));
    bind_signal(binding, field(page, "task-audio-apply"), "clicked", G_CALLBACK(apply_clicked));
    bind_signal(binding, field(page, "task-microphone-gate"), "toggled", G_CALLBACK(microphone_toggled));
    radios_audio_ui_bind_gain(ctx, field(page, "task-microphone-gain"), RADIOS_AUDIO_UI_MICROPHONE);
    g_autofree RadioSAudioSnapshot *snapshot = snapshot_take(audio); binding_update(binding, snapshot, TRUE);
}
static gboolean poll_audio(gpointer value)
{
    RadioSAudioController *audio = value; if (audio->closed) return G_SOURCE_REMOVE;
    g_autofree RadioSAudioSnapshot *snapshot = snapshot_take(audio);
    if (audio->loss_epoch != snapshot->loss_epoch) {
        audio->loss_epoch = snapshot->loss_epoch;
        ++audio->preview_intent; transition_cancel(audio);
        for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) audio->decks[i].autoplay = FALSE;
        if (audio->preview_active) radios_audio_ui_preview_stop(audio->ctx, NULL);
    }
    for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) {
        UiDeck *deck = &audio->decks[i]; if (!deck->autoplay) continue;
        GtkWidget *owner = g_weak_ref_get(&deck->owner);
        gboolean needs_owner = i >= RADIOS_AUDIO_NORMAL_DECKS;
        if ((needs_owner && !owner) || deck->generation != snapshot->decks[i].generation || deck->loss_epoch != snapshot->loss_epoch) {
            deck->autoplay = FALSE; if (needs_owner && !owner) radios_audio_engine_stop(audio->engine, i, NULL);
        }
        else if ((snapshot->decks[i].state == RADIOS_AUDIO_READY || snapshot->decks[i].state == RADIOS_AUDIO_PAUSED) && deck_output_ready(audio, snapshot, i)) {
            GError *error = NULL;
            if (radios_audio_engine_play(audio->engine, i, &error)) deck->autoplay = FALSE;
            else if (!g_error_matches(error, RADIOS_AUDIO_ERROR_DOMAIN, RADIOS_AUDIO_ERROR_UNAVAILABLE)) { deck->autoplay = FALSE; report_error(audio, g_steal_pointer(&error)); }
            g_clear_error(&error);
        } else if (snapshot->decks[i].state == RADIOS_AUDIO_ERROR || snapshot->decks[i].state == RADIOS_AUDIO_EMPTY || snapshot->decks[i].state == RADIOS_AUDIO_EOF) {
            deck->autoplay = FALSE; if (snapshot->decks[i].error[0]) notify_audio(audio, snapshot->decks[i].error);
        }
        g_clear_object(&owner);
    }
    if (audio->preview_active && audio->preview_pause_pending && snapshot->decks[32].state == RADIOS_AUDIO_READY) {
        radios_audio_engine_pause(audio->engine, RADIOS_AUDIO_PREVIEW_DECK, NULL);
        audio->preview_pause_pending = FALSE;
    }
    if (audio->preview_active && (snapshot->decks[32].state == RADIOS_AUDIO_EOF || snapshot->decks[32].state == RADIOS_AUDIO_ERROR)) radios_audio_ui_preview_stop(audio->ctx, NULL);
    for (guint i = 0; i < audio->bindings->len; ++i) binding_update(g_ptr_array_index(audio->bindings, i), snapshot, FALSE);
    return G_SOURCE_CONTINUE;
}
void radios_audio_ui_start(RadioSViewContext *ctx)
{
    RadioSAudioController *audio = controller(ctx); if (!audio || audio->timer) return;
    if (ctx->data && !audio->document) audio->document = radios_data_document_new(ctx, GTK_WIDGET(ctx->parent), "audio_preferences", "Ses aygıtları", preferences_loaded, audio);
    if (ctx->data && !audio->mixer_document) audio->mixer_document = radios_data_document_new(ctx, GTK_WIDGET(ctx->parent), "dj_mixer", "DJ mixer", mixer_loaded, audio);
    if (audio->mixer_dirty && audio->mixer_document) radios_data_document_dirty(audio->mixer_document, TRUE);
    if (audio->mixer_dirty || audio->preferences_dirty) autosave_schedule(audio, 250);
    audio->timer = g_timeout_add_full(G_PRIORITY_DEFAULT, 50, poll_audio, radios_audio_ui_ref(audio), (GDestroyNotify)radios_audio_ui_unref);
}
void radios_audio_ui_prepare_close(RadioSAudioController *audio)
{
    if (!audio || audio->closed || audio->preparing_close) return;
    audio->preparing_close = TRUE;
    if (audio->autosave_timer) { guint source = audio->autosave_timer; audio->autosave_timer = 0; g_source_remove(source); }
    autosave_submit(audio);
    if (!radios_data_document_preserve_pending(audio->mixer_document))
        g_printerr("DJ mixer otomatik kayıt kuyruğu korunamadı.\n");
    if (!radios_data_document_preserve_pending(audio->document))
        g_printerr("Ses tercihleri otomatik kayıt kuyruğu korunamadı.\n");
}
void radios_audio_ui_close(RadioSAudioController *audio)
{
    if (!audio || audio->closed) return;
    radios_audio_ui_prepare_close(audio);
    audio->closed = TRUE;
    radios_audio_engine_set_preview_output(audio->engine, RADIOS_AUDIO_BUS_CUE, 0, FALSE, NULL);
    ++audio->preview_intent; transition_cancel(audio);
    if (audio->timer) { guint timer = audio->timer; audio->timer = 0; g_source_remove(timer); }
    radios_audio_engine_set_microphone_gate(audio->engine, FALSE, NULL);
    for (guint i = 0; i < RADIOS_AUDIO_BUS_COUNT; ++i) radios_audio_engine_set_bus_gate(audio->engine, (RadioSAudioBus)i, FALSE, NULL);
    for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) { audio->decks[i].autoplay = FALSE; ++audio->decks[i].epoch; owner_clear(&audio->decks[i]); radios_audio_engine_stop(audio->engine, i, NULL); }
    inputs_sync(audio); /* closed: every live input stops and releases its line */
    g_ptr_array_set_size(audio->bindings, 0);
    g_ptr_array_set_size(audio->preview_outputs, 0);
    GObject *parent = g_weak_ref_get(&audio->parent); if (parent) { g_object_set_data(parent, "radios-audio-context", NULL); g_object_unref(parent); }
    audio->ctx = NULL;
}
void radios_audio_ui_unref(RadioSAudioController *audio)
{
    if (!audio || !g_atomic_int_dec_and_test(&audio->references)) return;
    radios_audio_ui_close(audio);
    /* Final window teardown joins cancellable workers; ordinary widget closure
     * only publishes cancellation and never waits for decode/device I/O. */
    radios_audio_engine_unref(audio->engine);
    if (audio->preview_file) { preview_file_delete(audio->preview_file); g_free(audio->preview_file); }
    g_weak_ref_clear(&audio->transition_owner);
    g_ptr_array_unref(audio->bindings); g_ptr_array_unref(audio->preview_outputs); g_weak_ref_clear(&audio->parent);
    for (guint i = 0; i < RADIOS_AUDIO_DECKS; ++i) g_weak_ref_clear(&audio->decks[i].owner);
    g_free(audio);
}
