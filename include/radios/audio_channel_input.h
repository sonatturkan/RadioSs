#ifndef RADIOS_AUDIO_CHANNEL_INPUT_H
#define RADIOS_AUDIO_CHANNEL_INPUT_H

#include "radios/audio.h"

G_BEGIN_DECLS

/* A mixer channel's live source other than a player or the studio microphone
 * (R5.1): a sound-card capture (one Pulse source) or a network stream (an
 * FFmpeg URL). One input feeds one channel strip through the engine's line
 * API, so EQ, fader, mute, effects and MAIN routing apply as for any player.
 * The runner owns a worker that connects, delivers 48 kHz float stereo PCM
 * and reconnects every 5 seconds after a loss until it is freed. A network
 * source that stays connected but sends no audio for 5 seconds is reported
 * as ERROR "Ağ kaynağından veri gelmiyor." with peak 0 and returns to READY
 * on the same connection when audio arrives again (a UDP/RTP sender that
 * restarts); an HTTP stall still ends in the 10 s read timeout and a
 * reconnect. An rtsp:// URL's "tcp" query flag selects RTP over the RTSP
 * connection. */
#define RADIOS_CHANNEL_INPUT_NO_DATA_MESSAGE "Ağ kaynağından veri gelmiyor."
typedef enum {
    RADIOS_CHANNEL_INPUT_NONE,
    RADIOS_CHANNEL_INPUT_CAPTURE,
    RADIOS_CHANNEL_INPUT_NETWORK
} RadioSAudioChannelInputKind;
typedef enum {
    RADIOS_CHANNEL_INPUT_IDLE,
    RADIOS_CHANNEL_INPUT_CONNECTING,
    RADIOS_CHANNEL_INPUT_READY,
    RADIOS_CHANNEL_INPUT_ERROR
} RadioSAudioChannelInputState;
#define RADIOS_CHANNEL_INPUT_DEVICE_SIZE 513u
#define RADIOS_CHANNEL_INPUT_NAME_SIZE 129u
#define RADIOS_CHANNEL_INPUT_RETRY_SECONDS 5u

typedef struct {
    RadioSAudioChannelInputKind kind;
    gchar device[RADIOS_CHANNEL_INPUT_DEVICE_SIZE]; /* Pulse source name, or the stream URL */
    gchar name[RADIOS_CHANNEL_INPUT_NAME_SIZE];     /* operator-visible name */
} RadioSAudioChannelInputSpec;
typedef struct {
    RadioSAudioChannelInputState state;
    gchar error[256];   /* last failure, kept while reconnecting */
    gfloat peak[2];     /* last delivered block; 0 when none came for 0.5 s */
    guint64 frames;     /* frames delivered to the mixer line */
    guint attempts;     /* connection attempts so far */
} RadioSAudioChannelInputStatus;
typedef struct RadioSAudioChannelInput RadioSAudioChannelInput;

/* Wire names used by the dj_mixer document: "capture" and "network". */
const gchar *radios_audio_channel_input_kind_name(RadioSAudioChannelInputKind kind);
gboolean radios_audio_channel_input_kind_parse(const gchar *name, RadioSAudioChannelInputKind *kind);
/* A network URL must use http, https, rtp, udp, rtsp or srt, and only one
 * this FFmpeg build can open: without its protocol (the Freedesktop SDK has
 * no srt) the spec is refused with "Bu sistemin FFmpeg'inde SRT desteği yok…"
 * rather than retried forever. A capture needs a Pulse source name. Names
 * are 1..128 and devices 1..512 bytes of UTF-8. */
gboolean radios_audio_channel_input_spec_validate(const RadioSAudioChannelInputSpec *spec, GError **error);
gboolean radios_audio_channel_input_spec_equal(const RadioSAudioChannelInputSpec *a, const RadioSAudioChannelInputSpec *b);

/* Opens the engine line for mixer channel 0..31 and starts the worker. The
 * engine reference is held until free. pulse_server NULL is the default. */
RadioSAudioChannelInput *radios_audio_channel_input_new(RadioSAudioEngine *engine, guint channel,
    const RadioSAudioChannelInputSpec *spec, const gchar *pulse_server, GError **error);
guint radios_audio_channel_input_channel(const RadioSAudioChannelInput *input);
void radios_audio_channel_input_spec(const RadioSAudioChannelInput *input, RadioSAudioChannelInputSpec *spec);
void radios_audio_channel_input_status(RadioSAudioChannelInput *input, RadioSAudioChannelInputStatus *status);
/* Stops the worker (a pending connect is interrupted), closes the line and
 * releases the engine; waits at most 250 ms, after which a worker stuck in an
 * uninterruptible host lookup completes the teardown on its own. The input
 * must not be used afterwards. Never touches any deck's transport. */
void radios_audio_channel_input_free(RadioSAudioChannelInput *input);

G_END_DECLS
#endif
