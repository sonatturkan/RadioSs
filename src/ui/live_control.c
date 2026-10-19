#include "radios/live_control.h"
#include "radios/audio_ui.h"
#include "radios/audio.h"
#include "radios/data.h"
#include <string.h>

/* Kılavuz §06–§07: the L latch and the R push-to-talk of an event row. State
 * is kept per block in a table owned by the window (ctx->parent); it dies
 * with the window and a block without an entry is neither latched nor held.
 * Only what a block opened is closed again: a strip or the latch that was
 * already on air stays as the operator (or another live block) left it. No
 * call here touches a deck's transport or the queue. */
typedef struct {
    gboolean latched, held;      /* L on; R held */
    gboolean latch_opened;       /* L switched the microphone latch on */
    guint count, channels[RADIOS_AUDIO_MIXER_CHANNELS];
    gboolean latch_channel[RADIOS_AUDIO_MIXER_CHANNELS]; /* strip went on air for L */
    gboolean talk_channel[RADIOS_AUDIO_MIXER_CHANNELS];  /* strip went on air for R */
} LiveState;
#define LIVE_STATES "radios-live-control-states"

/* Engine binding (audio_ui.h): latch, momentary push-to-talk, strip mute.
 * The on-air gate is latch || ptt inside the engine, so releasing R never
 * closes an L that is still on. Tests replace this layer with a fake. */
static gboolean backend_latch(RadioSViewContext *ctx, gboolean on, GError **error)
{ return radios_audio_ui_microphone(ctx, on, error); }
static gboolean backend_ptt(RadioSViewContext *ctx, gboolean held, GError **error)
{ return radios_audio_ui_microphone_ptt(ctx, held, error); }
static gboolean backend_channel(RadioSViewContext *ctx, guint channel, gboolean on_air, GError **error)
{ return radios_audio_ui_set_channel_on_air(ctx, channel, on_air, error); }
static gboolean backend_channel_on_air(RadioSViewContext *ctx, guint channel)
{ return radios_audio_ui_channel_on_air(ctx, channel); }
static const gchar *backend_channel_label(RadioSViewContext *ctx, guint channel)
{ return radios_audio_ui_get_channel_label(ctx, channel); }
static gboolean backend_latch_active(RadioSViewContext *ctx)
{
    RadioSAudioEngine *engine = radios_audio_ui_engine_acquire(ctx); if (!engine) return FALSE;
    RadioSAudioSnapshot *snapshot = g_new0(RadioSAudioSnapshot, 1);
    radios_audio_engine_snapshot(engine, snapshot); gboolean latch = snapshot->microphone_latch;
    g_free(snapshot); radios_audio_engine_unref(engine); return latch;
}
static const RadioSLiveControlBackend engine_backend = {backend_latch, backend_ptt, backend_channel, backend_channel_on_air, backend_latch_active, backend_channel_label};
static const RadioSLiveControlBackend *backend = &engine_backend;
void radios_live_control_set_backend(const RadioSLiveControlBackend *custom)
{ backend = custom ? custom : &engine_backend; }

static JsonObject *live_object(JsonObject *block)
{
    JsonNode *node = block ? json_object_get_member(block, "live") : NULL;
    return node && JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
}
static RadioSLiveLabel label_of(const gchar *label)
{
    if (!label) return RADIOS_LIVE_LABEL_NONE;
    if (g_str_equal(label, "dj")) return RADIOS_LIVE_LABEL_DJ;
    if (g_str_equal(label, "talk")) return RADIOS_LIVE_LABEL_TALK;
    if (g_str_equal(label, "music")) return RADIOS_LIVE_LABEL_MUSIC;
    return RADIOS_LIVE_LABEL_NONE;
}
RadioSLiveLabel radios_live_control_label(JsonObject *block)
{ return label_of(radios_json_string(live_object(block), "label", "")); }
/* channels: absent or null names no strip; otherwise distinct integers 1..32. */
static gboolean channels_parse(JsonObject *block, guint channels[RADIOS_AUDIO_MIXER_CHANNELS], guint *count)
{
    *count = 0;
    JsonObject *live = live_object(block);
    JsonNode *node = live ? json_object_get_member(live, "channels") : NULL;
    if (!node || JSON_NODE_HOLDS_NULL(node)) return TRUE;
    if (!JSON_NODE_HOLDS_ARRAY(node)) return FALSE;
    JsonArray *array = json_node_get_array(node); guint length = json_array_get_length(array);
    if (length > RADIOS_AUDIO_MIXER_CHANNELS) return FALSE;
    gboolean seen[RADIOS_AUDIO_MIXER_CHANNELS + 1] = {FALSE};
    for (guint i = 0; i < length; ++i) {
        JsonNode *element = json_array_get_element(array, i);
        if (!JSON_NODE_HOLDS_VALUE(element) || json_node_get_value_type(element) != G_TYPE_INT64) return FALSE;
        gint64 value = json_node_get_int(element);
        if (value < 1 || value > (gint64)RADIOS_AUDIO_MIXER_CHANNELS || seen[value]) return FALSE;
        seen[value] = TRUE; channels[(*count)++] = (guint)value;
    }
    return TRUE;
}
RadioSLiveLabel radios_live_control_effective_label(RadioSViewContext *ctx, JsonObject *block)
{
    RadioSLiveLabel own = radios_live_control_label(block), agreed = RADIOS_LIVE_LABEL_NONE;
    guint channels[RADIOS_AUDIO_MIXER_CHANNELS], count = 0;
    if (own == RADIOS_LIVE_LABEL_NONE || !channels_parse(block, channels, &count) || !backend->channel_label) return own;
    for (guint i = 0; i < count; ++i) {
        RadioSLiveLabel label = label_of(backend->channel_label(ctx, channels[i]));
        if (label == RADIOS_LIVE_LABEL_NONE) continue;
        if (agreed != RADIOS_LIVE_LABEL_NONE && agreed != label) return own;
        agreed = label;
    }
    return agreed != RADIOS_LIVE_LABEL_NONE ? agreed : own;
}
gboolean radios_live_control_available(JsonObject *block)
{
    guint channels[RADIOS_AUDIO_MIXER_CHANNELS], count = 0;
    return radios_live_control_label(block) != RADIOS_LIVE_LABEL_NONE && channels_parse(block, channels, &count);
}
guint radios_live_control_channels(JsonObject *block, guint channels[32])
{
    guint scratch[RADIOS_AUDIO_MIXER_CHANNELS], count = 0;
    return channels_parse(block, channels ? channels : scratch, &count) ? count : 0;
}

static gboolean not_connected(GError **error)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "Ses motoru hazır değil."); return FALSE; }
static GHashTable *states(RadioSViewContext *ctx, gboolean create)
{
    if (!ctx || !ctx->parent) return NULL;
    GHashTable *table = g_object_get_data(G_OBJECT(ctx->parent), LIVE_STATES);
    if (!table && create) {
        table = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
        g_object_set_data_full(G_OBJECT(ctx->parent), LIVE_STATES, table, (GDestroyNotify)g_hash_table_unref);
    }
    return table;
}
static LiveState *state_get(RadioSViewContext *ctx, const gchar *id, gboolean create)
{
    GHashTable *table = states(ctx, create); if (!table || !id) return NULL;
    LiveState *state = g_hash_table_lookup(table, id);
    if (!state && create) { state = g_new0(LiveState, 1); g_hash_table_insert(table, g_strdup(id), state); }
    return state;
}
/* An idle entry that owns nothing any more leaves the table. */
static void state_settle(RadioSViewContext *ctx, const gchar *id, LiveState *state)
{
    if (!state || state->latched || state->held) return;
    for (guint i = 0; i < state->count; ++i) if (state->latch_channel[i] || state->talk_channel[i]) return;
    g_hash_table_remove(states(ctx, FALSE), id);
}
/* Binding, engine and block identity checks shared by L and R. */
static gboolean prepare(RadioSViewContext *ctx, JsonObject *block, guint channels[RADIOS_AUDIO_MIXER_CHANNELS], guint *count, const gchar **id, GError **error)
{
    RadioSLiveLabel label = radios_live_control_effective_label(ctx, block);
    if (label == RADIOS_LIVE_LABEL_NONE || !channels_parse(block, channels, count)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Eventin canlı yayın bağlantısı yok."); return FALSE;
    }
    if (label == RADIOS_LIVE_LABEL_MUSIC) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Müzik eventinde L otomasyon şemasını başlatır; bas-konuş yoktur.");
        return FALSE;
    }
    if (!ctx || !ctx->audio) return not_connected(error);
    *id = radios_json_string(block, "id", "");
    if (!**id) { g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Event kimliği yok."); return FALSE; }
    if (!ctx->parent) return not_connected(error);
    return TRUE;
}
/* Puts the bound strips that are not on air yet on air; owned[i] marks them. */
static gboolean strips_open(RadioSViewContext *ctx, const LiveState *state, gboolean owned[RADIOS_AUDIO_MIXER_CHANNELS], GError **error)
{
    for (guint i = 0; i < state->count; ++i) {
        if (owned[i] || backend->channel_on_air(ctx, state->channels[i])) continue;
        if (!backend->channel(ctx, state->channels[i], TRUE, error)) return FALSE;
        owned[i] = TRUE;
    }
    return TRUE;
}
/* Mutes the strips this owner opened. The first failure is reported and
 * those strips stay marked, so a later off/release retries them. */
static gboolean strips_close(RadioSViewContext *ctx, const LiveState *state, gboolean owned[RADIOS_AUDIO_MIXER_CHANNELS], GError **error)
{
    gboolean result = TRUE;
    for (guint i = 0; i < state->count; ++i) {
        if (!owned[i]) continue;
        GError *failure = NULL;
        if (backend->channel(ctx, state->channels[i], FALSE, &failure)) { owned[i] = FALSE; continue; }
        if (result && error) g_propagate_error(error, failure); else g_clear_error(&failure);
        result = FALSE;
    }
    return result;
}
/* Another latched or held block that names the same strip (or still needs
 * the latch) takes over what this block opened, so switching one block off
 * never cuts the other one. */
static void hand_over(RadioSViewContext *ctx, const gchar *id, LiveState *state, gboolean owned[RADIOS_AUDIO_MIXER_CHANNELS], gboolean latch)
{
    GHashTable *table = states(ctx, FALSE); if (!table) return;
    GHashTableIter iter; gpointer key, value;
    g_hash_table_iter_init(&iter, table);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        LiveState *other = value;
        if (g_str_equal(key, id) || (!other->latched && !other->held)) continue;
        if (latch && state->latch_opened && other->latched && !other->latch_opened) { other->latch_opened = TRUE; state->latch_opened = FALSE; }
        for (guint i = 0; i < state->count; ++i) {
            if (!owned[i]) continue;
            for (guint j = 0; j < other->count; ++j) {
                if (other->channels[j] != state->channels[i]) continue;
                gboolean *flags = other->latched ? other->latch_channel : other->talk_channel;
                if (!flags[j]) { flags[j] = TRUE; owned[i] = FALSE; }
                break;
            }
        }
    }
}
gboolean radios_live_control_toggle(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *block, gboolean on, GError **error)
{
    (void)owner; /* the row widget; the state belongs to the window */
    guint channels[RADIOS_AUDIO_MIXER_CHANNELS], count = 0; const gchar *id = NULL;
    if (!prepare(ctx, block, channels, &count, &id, error)) return FALSE;
    LiveState *state = state_get(ctx, id, on);
    if (on) {
        if (state->latched) return TRUE;
        if (!state->held) { state->count = count; memcpy(state->channels, channels, count * sizeof channels[0]); }
        if (!strips_open(ctx, state, state->latch_channel, error)) {
            strips_close(ctx, state, state->latch_channel, NULL); state_settle(ctx, id, state); return FALSE;
        }
        gboolean latch_active = backend->latch_active(ctx);
        if (!latch_active && !backend->latch(ctx, TRUE, error)) {
            strips_close(ctx, state, state->latch_channel, NULL); state_settle(ctx, id, state); return FALSE;
        }
        state->latched = TRUE; state->latch_opened = !latch_active; return TRUE;
    }
    if (!state || !state->latched) return TRUE;
    /* R still held: its strips stay open until release (the engine keeps the
     * gate open through ptt once the latch drops). */
    for (guint i = 0; state->held && i < state->count; ++i)
        if (state->latch_channel[i]) { state->talk_channel[i] = TRUE; state->latch_channel[i] = FALSE; }
    hand_over(ctx, id, state, state->latch_channel, TRUE);
    gboolean closed = strips_close(ctx, state, state->latch_channel, error);
    if (state->latch_opened) {
        GError *failure = NULL;
        if (backend->latch(ctx, FALSE, &failure)) state->latch_opened = FALSE;
        else { if (closed && error) g_propagate_error(error, failure); else g_clear_error(&failure); closed = FALSE; }
    }
    if (!closed) return FALSE; /* still latched; the operator retries */
    state->latched = FALSE; state_settle(ctx, id, state); return TRUE;
}
gboolean radios_live_control_talk(RadioSViewContext *ctx, JsonObject *block, gboolean pressed, GError **error)
{
    guint channels[RADIOS_AUDIO_MIXER_CHANNELS], count = 0; const gchar *id = NULL;
    if (!prepare(ctx, block, channels, &count, &id, error)) return FALSE;
    LiveState *state = state_get(ctx, id, pressed);
    if (pressed) {
        if (state->held) return TRUE;
        if (!state->latched) { state->count = count; memcpy(state->channels, channels, count * sizeof channels[0]); }
        if (!strips_open(ctx, state, state->talk_channel, error)) {
            strips_close(ctx, state, state->talk_channel, NULL); state_settle(ctx, id, state); return FALSE;
        }
        if (!backend->ptt(ctx, TRUE, error)) {
            strips_close(ctx, state, state->talk_channel, NULL); state_settle(ctx, id, state); return FALSE;
        }
        state->held = TRUE; return TRUE;
    }
    /* Release ends only the momentary talk: the L latch and its strips stay,
     * and no transport or queue command is produced. */
    if (!state || !state->held) return TRUE;
    if (!backend->ptt(ctx, FALSE, error)) return FALSE;
    state->held = FALSE;
    hand_over(ctx, id, state, state->talk_channel, FALSE);
    gboolean closed = strips_close(ctx, state, state->talk_channel, error);
    state_settle(ctx, id, state); return closed;
}
gboolean radios_live_control_state(RadioSViewContext *ctx, JsonObject *block, gboolean *live_active, gboolean *talk_held)
{
    if (live_active) *live_active = FALSE;
    if (talk_held) *talk_held = FALSE;
    if (!radios_live_control_available(block)) return FALSE;
    LiveState *state = state_get(ctx, radios_json_string(block, "id", ""), FALSE);
    if (state) {
        if (live_active) *live_active = state->latched;
        if (talk_held) *talk_held = state->held;
    }
    return TRUE;
}
