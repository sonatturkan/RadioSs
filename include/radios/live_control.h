#ifndef RADIOS_LIVE_CONTROL_H
#define RADIOS_LIVE_CONTROL_H
#include "radios/views.h"
#include <json-glib/json-glib.h>

G_BEGIN_DECLS

/* L and R of an event row (kılavuz §06–§07). block is the programme block
 * (event) JSON; its optional "live" object comes from the schema/programme
 * stream: {label: "dj"|"talk"|"music", channels: [1..32], automation_id:
 * uuid|"", end_rule: "schema"|"manual"}. Without a live binding every call
 * reports unavailability and changes nothing. None of these touch the
 * transport: releasing R or switching L off never skips to the next song. */
typedef enum { RADIOS_LIVE_LABEL_NONE, RADIOS_LIVE_LABEL_DJ, RADIOS_LIVE_LABEL_TALK, RADIOS_LIVE_LABEL_MUSIC } RadioSLiveLabel;
RadioSLiveLabel radios_live_control_label(JsonObject *block);
/* The label that decides what L does now (kılavuz s.4 adım 5, s.7): when
 * every labelled mixer strip the block names carries the same DJ/Konuşma/
 * Müzik label, that label; otherwise (none labelled, or mixed) the block's
 * own label. NONE without a usable binding. */
RadioSLiveLabel radios_live_control_effective_label(RadioSViewContext *ctx, JsonObject *block);
/* TRUE when the block carries a usable live binding. */
gboolean radios_live_control_available(JsonObject *block);
/* Mixer channels (one-based, ≤32) named by the binding; returns the count. */
guint radios_live_control_channels(JsonObject *block, guint channels[32]);
/* L: DJ/Konuşma labels open (on) or close (off) the bound mixer channels and
 * the on-air microphone latch; the previous channel state is restored on off.
 * The Müzik label is not handled here: the caller starts the event's
 * automation and this returns FALSE with G_IO_ERROR_NOT_SUPPORTED. */
gboolean radios_live_control_toggle(RadioSViewContext *ctx, GtkWidget *owner, JsonObject *block, gboolean on, GError **error);
/* R: push-to-talk. pressed opens the microphone for the bound DJ/Konuşma
 * channel while held; released closes only what press opened. */
gboolean radios_live_control_talk(RadioSViewContext *ctx, JsonObject *block, gboolean pressed, GError **error);
/* Current state for this block: L latched, R held. FALSE without a binding. */
gboolean radios_live_control_state(RadioSViewContext *ctx, JsonObject *block, gboolean *live_active, gboolean *talk_held);
/* Engine binding used by toggle/talk: the microphone latch, the momentary
 * push-to-talk and the mixer strip on-air state of audio_ui.h (the engine's
 * gate is latch || ptt). Tests install a fake; NULL restores the engine. */
typedef struct {
    gboolean (*latch)(RadioSViewContext *ctx, gboolean on, GError **error);
    gboolean (*ptt)(RadioSViewContext *ctx, gboolean held, GError **error);
    gboolean (*channel)(RadioSViewContext *ctx, guint channel_1_32, gboolean on_air, GError **error);
    gboolean (*channel_on_air)(RadioSViewContext *ctx, guint channel_1_32);
    gboolean (*latch_active)(RadioSViewContext *ctx);
    /* "dj"/"talk"/"music" of a mixer strip, or NULL when it has none. */
    const gchar *(*channel_label)(RadioSViewContext *ctx, guint channel_1_32);
} RadioSLiveControlBackend;
void radios_live_control_set_backend(const RadioSLiveControlBackend *backend);

G_END_DECLS
#endif
