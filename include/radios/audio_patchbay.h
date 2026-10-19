#ifndef RADIOS_AUDIO_PATCHBAY_H
#define RADIOS_AUDIO_PATCHBAY_H

#include "radios/audio.h"
#include "radios/audio_output.h"
#include "radios/routing_graph.h"

G_BEGIN_DECLS

/* Live runtime of the routing page. It uses the Windows audio system
 * (WASAPI: capture and render endpoints, endpoint loopback "<render id>.monitor",
 * per-application process loopback), runs every block of the applied graph in
 * 10 ms steps (48 kHz stereo) and reaches the mixer through engine taps and
 * lines. Recorders and Internet streams start only on explicit request;
 * nothing starts when a graph is loaded. Host audio is never changed: no
 * default device, volume, mute or per-application routing is touched. */
#define RADIOS_PATCHBAY_MAX_DEVICES 64u
#define RADIOS_PATCHBAY_NAME_SIZE 513u
#define RADIOS_PATCH_INVALID_INDEX G_MAXUINT32

typedef struct RadioSAudioPatchbay RadioSAudioPatchbay;
typedef enum {
    RADIOS_PATCH_IDLE,     /* not running, or the block needs nothing live */
    RADIOS_PATCH_STARTING, /* waiting for its device, the audio system or the mixer */
    RADIOS_PATCH_ACTIVE,
    RADIOS_PATCH_FAILED
} RadioSPatchState;
typedef struct {
    guint32 index;  /* stable while the patchbay lives */
    /* Persisted identifier: endpoint id, "<render id>.monitor" for the
     * loopback of an output, or a virtual test device name. */
    gchar name[RADIOS_PATCHBAY_NAME_SIZE], description[RADIOS_PATCHBAY_NAME_SIZE];
    gboolean monitor; /* the loopback ("monitor") of an output */
} RadioSPatchDevice;
typedef struct {
    guint32 index;      /* process id; the UI and move_application use it */
    guint32 sink;       /* index of the output device it plays on, RADIOS_PATCH_INVALID_INDEX if unknown */
    gchar name[RADIOS_PATCHBAY_NAME_SIZE], binary[257]; /* display name, "spotify.exe" */
    guint64 start_time; /* process creation time (identity together with the pid) */
    guint bound_node;   /* application-audio block capturing it, 0 = none */
    gboolean active;    /* a session of the process is playing now */
} RadioSPatchApplication;
typedef struct {
    guint id;
    RadioSRoutingGraphKind kind;
    RadioSPatchState state;
    gchar error[256];
    gfloat peak[2], rms[2]; /* since the previous snapshot: output of sources, input of sinks */
    /* RG_VIRTUAL (application audio): the captured application, pid 0 = none */
    guint32 application_pid;
    gchar application[RADIOS_PATCHBAY_NAME_SIZE], application_binary[257];
    /* RG_RECORDER */
    gboolean recording, paused;
    guint segment;
    guint64 frames, bytes, total_frames;
    gchar path[4097];
    /* RG_STREAM */
    gboolean streaming; /* operator started it; reconnects every 5 s after a failure */
    RadioSAudioOutputState output_state;
    glong http_status;
    guint64 sent_bytes;
    guint reconnects;
    gint64 connected_us; /* monotonic time of the current connection, 0 if none */
    RadioSAudioMetadataState metadata_state;
} RadioSPatchNodeSnapshot;
typedef struct {
    gboolean connected, running, busy;
    gchar error[256];
    guint output_count, input_count, application_count;
    RadioSPatchDevice outputs[RADIOS_PATCHBAY_MAX_DEVICES], inputs[RADIOS_PATCHBAY_MAX_DEVICES];
    RadioSPatchApplication applications[RADIOS_PATCHBAY_MAX_DEVICES];
    guint node_count;
    RadioSPatchNodeSnapshot nodes[RADIOS_ROUTING_GRAPH_MAX_NODES];
    guint64 underruns, trimmed;
    /* Per-application capture (Windows 10 2004+ process loopback). When FALSE
     * the reason says why ("Bu Windows sürümü ...", test audio system, ...). */
    gboolean application_capture;
    gchar application_capture_reason[256];
} RadioSAudioPatchbaySnapshot;

/* audio_host selects the sound system: NULL or "" uses $RADIOS_AUDIO_BACKEND
 * (then $RADIOS_AUDIO_SERVER, default "wasapi"); "wasapi" is the Windows
 * audio service, "virtual:" the process-wide virtual test devices and
 * "none:" (also any Linux-era "unix:..." address) no devices at all. Mixer,
 * MAIN, recorder, meter and stream blocks work with every choice. */
RadioSAudioPatchbay *radios_audio_patchbay_new(const gchar *audio_host);
RadioSAudioPatchbay *radios_audio_patchbay_ref(RadioSAudioPatchbay *patchbay);
/* The last unref stops recorders and streams (WAV files are finalized),
 * closes every device stream and drops every application capture. */
void radios_audio_patchbay_unref(RadioSAudioPatchbay *patchbay);
/* Mixer, MAIN and talk-microphone blocks need the broadcast engine. */
void radios_audio_patchbay_set_engine(RadioSAudioPatchbay *patchbay, RadioSAudioEngine *engine);
/* Folder for recorders without their own folder (absolute path). */
void radios_audio_patchbay_set_recording_root(RadioSAudioPatchbay *patchbay, const gchar *folder);
void radios_audio_patchbay_refresh(RadioSAudioPatchbay *patchbay);
/* Asynchronous. running=TRUE opens every capture, playback and mixer link;
 * FALSE closes them and stops recorders and streams. */
gboolean radios_audio_patchbay_apply(RadioSAudioPatchbay *patchbay,
    const RadioSRoutingGraph *graph, gboolean running, GError **error);
/* Recorder: WAV PCM16 48 kHz stereo, "<prefix>-YYYYMMDD-HHMMSS.wav".
 * A request for a block that the pending apply will create is kept. */
gboolean radios_audio_patchbay_record(RadioSAudioPatchbay *patchbay, guint node, GError **error);
gboolean radios_audio_patchbay_record_pause(RadioSAudioPatchbay *patchbay, guint node, gboolean paused, GError **error);
gboolean radios_audio_patchbay_record_split(RadioSAudioPatchbay *patchbay, guint node, GError **error);
void radios_audio_patchbay_record_stop(RadioSAudioPatchbay *patchbay, guint node);
/* Internet stream with the block's server settings; the password lives in
 * memory only while the operator keeps the stream started. */
gboolean radios_audio_patchbay_stream_start(RadioSAudioPatchbay *patchbay, guint node,
    const gchar *password, GError **error);
void radios_audio_patchbay_stream_stop(RadioSAudioPatchbay *patchbay, guint node);
gboolean radios_audio_patchbay_stream_metadata(RadioSAudioPatchbay *patchbay, guint node,
    const gchar *title, GError **error);
/* Read-only status of this block's current stream worker. The output
 * reference survives graph replacement during observation. FALSE clears out. */
gboolean radios_audio_patchbay_stream_metadata_status(RadioSAudioPatchbay *patchbay,
    guint node, RadioSAudioOutputSnapshot *out);
/* Binds a playing application (snapshot index = process id) as the capture
 * source of an application-audio block (RG_VIRTUAL). Nothing is moved: the
 * application keeps playing on its own output while RadioS captures its
 * audio (process loopback, its child processes included) whenever the
 * routing runs. One application feeds one block; binding it elsewhere
 * releases the previous block. The binding is not persisted and ends when
 * the application exits ("Uygulama kapandı; yakalama durduruldu."). */
gboolean radios_audio_patchbay_move_application(RadioSAudioPatchbay *patchbay,
    guint32 application, guint node, GError **error);
/* Ends the application capture of a block ("Yakalamayı bırak"). */
void radios_audio_patchbay_release_application(RadioSAudioPatchbay *patchbay, guint node);
void radios_audio_patchbay_snapshot(RadioSAudioPatchbay *patchbay, RadioSAudioPatchbaySnapshot *snapshot);

G_END_DECLS
#endif
