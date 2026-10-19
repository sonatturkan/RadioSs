#ifndef RADIOS_AUDIO_OUTPUT_H
#define RADIOS_AUDIO_OUTPUT_H

#include "radios/audio.h"

G_BEGIN_DECLS

typedef struct RadioSAudioOutput RadioSAudioOutput;
typedef enum { RADIOS_AUDIO_OUTPUT_RECORD, RADIOS_AUDIO_OUTPUT_STREAM,
               RADIOS_AUDIO_OUTPUT_COUNT } RadioSAudioOutputKind;
typedef enum { RADIOS_AUDIO_OUTPUT_IDLE, RADIOS_AUDIO_OUTPUT_STARTING,
               RADIOS_AUDIO_OUTPUT_ACTIVE, RADIOS_AUDIO_OUTPUT_STOPPING,
               RADIOS_AUDIO_OUTPUT_STOPPED, RADIOS_AUDIO_OUTPUT_FAILED } RadioSAudioOutputState;
typedef enum { RADIOS_AUDIO_METADATA_IDLE, RADIOS_AUDIO_METADATA_PENDING,
               RADIOS_AUDIO_METADATA_SUCCESS, RADIOS_AUDIO_METADATA_FAILED } RadioSAudioMetadataState;

typedef struct {
    RadioSAudioOutputKind kind;
    RadioSAudioOutputState state;
    guint64 generation, frames_received, frames_processed, bytes_written;
    guint queued_frames, overrun_frames;
    glong http_status;
    gboolean recording_finalized, response_confirmed;
    gchar target[4097], error[256]; /* never contains credentials */
    RadioSAudioMetadataState metadata_state;
    guint64 metadata_sequence;
    glong metadata_http_status;
    gchar metadata_title[1025], metadata_error[256];
} RadioSAudioOutputSnapshot;

/* An engine retains one handle of each kind until shutdown. Acquired handles
 * are caller-owned; their snapshots remain readable after engine teardown.
 * Both consume only the final MAIN mix (48 kHz stereo), after DSP/gain/gate.
 * PCM queues hold five seconds. Overrun fails the output without dropping
 * samples silently or blocking the renderer. Starts never occur implicitly. */
RadioSAudioOutput *radios_audio_engine_output_acquire(RadioSAudioEngine *engine,
    RadioSAudioOutputKind kind);
RadioSAudioOutput *radios_audio_output_ref(RadioSAudioOutput *output);
void radios_audio_output_unref(RadioSAudioOutput *output);

/* WAV PCM16 stereo 48 kHz. Absolute path required, existing files are never
 * overwritten. Worker opens/writes/finalizes asynchronously. RIFF size limit
 * is 4 GiB; reaching it fails with a valid partial WAV. */
gboolean radios_audio_output_start_record(RadioSAudioOutput *output,
    const gchar *path, GError **error);
/* Real HTTP(S) PUT/raw MP3 source upload, compatible with Icecast 2.4 mounts.
 * URL must have a nonempty mount path and no embedded credentials/query.
 * bitrate_kbps: 64, 96, 128, 160, 192, 256 or 320. No reconnect is automatic.
 * bytes_written counts libcurl-submitted network payload, not listener proof. */
gboolean radios_audio_output_start_stream(RadioSAudioOutput *output,
    const gchar *url, const gchar *username, const gchar *password,
    guint bitrate_kbps, GError **error);
/* Patchbay servers. Icecast 2 is an HTTP PUT to http://host:port/mount with
 * station headers; SHOUTcast 1 and 2 use the legacy source login on port+1
 * (version 2 appends ":#stream_id" to the password). MP3 at bitrate_kbps.
 * The password is kept only for this run and wiped when it ends. */
typedef enum { RADIOS_STREAM_ICECAST, RADIOS_STREAM_SHOUTCAST1, RADIOS_STREAM_SHOUTCAST2 } RadioSAudioStreamProtocol;
typedef struct {
    RadioSAudioStreamProtocol protocol;
    const gchar *host, *mount, *username, *password;
    guint port, stream_id, bitrate_kbps;
    const gchar *name, *genre, *description, *website;
    gboolean public_listing;
} RadioSAudioStreamTarget;
gboolean radios_audio_output_start_server(RadioSAudioOutput *output,
    const RadioSAudioStreamTarget *target, GError **error);
/* Stop is nonblocking. Poll until STOPPED/FAILED for finalized file/results. */
void radios_audio_output_stop(RadioSAudioOutput *output);
void radios_audio_output_snapshot(RadioSAudioOutput *output,
    RadioSAudioOutputSnapshot *snapshot);
/* Separate bounded network worker issues Icecast /admin/metadata updates.
 * Latest pending title replaces the previous pending title. Success requires
 * a 2xx HTTP response, independently of stream progress or local now-playing. */
gboolean radios_audio_output_metadata(RadioSAudioOutput *output,
    const gchar *title, GError **error);

G_END_DECLS
#endif
