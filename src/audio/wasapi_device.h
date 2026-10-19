#ifndef RADIOS_WASAPI_DEVICE_H
#define RADIOS_WASAPI_DEVICE_H

/* Shared WASAPI layer of the RadioS audio module (Windows 10/11).
 *
 * Every RadioS component that talks to Windows audio endpoints goes through
 * this layer: the engine backend (MAIN/CUE/MONITOR outputs and the talk
 * microphone), the routing patchbay and the mixer channel inputs. They all see
 * the same device identifiers and the same Turkish error texts.
 *
 * The header deliberately exposes GLib types only: no <windows.h>, no COM
 * types, so portable files can include it without Win32 macro pollution.
 *
 * Identifiers (persisted in preferences and routing documents):
 *   "<IMMDevice::GetId()>"          a render or capture endpoint, e.g.
 *                                   "{0.0.0.00000000}.{5f2a…}" (UTF-8)
 *   "<render id>.monitor"           loopback of what that render endpoint plays
 *   "@default"                      Windows' default endpoint (eConsole role) of
 *                                   the needed direction, resolved when a stream
 *                                   opens and never followed afterwards
 *   "@default.monitor"              loopback of the default render endpoint
 * Explicit identifiers never fall back to another device: an identifier that
 * Windows does not know (for example a PulseAudio name saved on Linux) fails
 * with the "device not found" text and RADIOS_WASAPI_LOST.
 *
 * Threads: functions marked [MTA] must run on a thread that called
 * radios_wasapi_thread_enter(). Functions marked [any] may run anywhere,
 * including the GTK thread (they never call COM). A stream belongs to the
 * thread that opened it; only that thread may use or close it. */

#include <glib.h>

G_BEGIN_DECLS

#define RADIOS_WASAPI_ID_SIZE 513u
#define RADIOS_WASAPI_NAME_SIZE 513u
#define RADIOS_WASAPI_ERROR_SIZE 256u
#define RADIOS_WASAPI_DEFAULT_ID "@default"
#define RADIOS_WASAPI_MONITOR_SUFFIX ".monitor"
/* Largest frame count accepted by one stream read/write call. */
#define RADIOS_WASAPI_MAX_FRAMES 16384u

typedef enum {
    RADIOS_WASAPI_OK = 0,
    RADIOS_WASAPI_LOST,          /* endpoint unknown, removed, disabled or unplugged */
    RADIOS_WASAPI_REOPEN,        /* stream invalidated while its endpoint is still active
                                    (format change, resume, driver restart): reopen it */
    RADIOS_WASAPI_SERVICE_DOWN,  /* Windows Audio service (Audiosrv) is not running */
    RADIOS_WASAPI_FAILED         /* anything else; the error text names the HRESULT */
} RadioSWasapiStatus;

typedef enum { RADIOS_WASAPI_FLOW_RENDER, RADIOS_WASAPI_FLOW_CAPTURE } RadioSWasapiFlow;

/* ------------------------------------------------------------------ threads */

/* [any] Enters the COM multithreaded apartment for the calling thread.
 * FALSE (with a Turkish error) when the thread already is a single-threaded
 * apartment (the GTK thread) or COM failed; radios_wasapi_thread_leave()
 * must then not be called. Pairs nest like CoInitializeEx/CoUninitialize. */
gboolean radios_wasapi_thread_enter(gchar error[RADIOS_WASAPI_ERROR_SIZE]);
void radios_wasapi_thread_leave(void);

/* [any] Registers the calling thread with MMCSS as a "Pro Audio" task (falls
 * back to THREAD_PRIORITY_HIGHEST). Never NULL; end it on the same thread. */
gpointer radios_wasapi_audio_priority_begin(void);
void radios_wasapi_audio_priority_end(gpointer cookie);

/* [any] Process-wide realtime conditions, reference counted: 1 ms timer
 * resolution (timeBeginPeriod), opt-out of power throttling/EcoQoS including
 * the timer-resolution throttling Windows 11 applies to minimised windows,
 * and a "system required" power request (no idle sleep while on air). The
 * last release restores the defaults. */
void radios_wasapi_realtime_acquire(void);
void radios_wasapi_realtime_release(void);

/* [any] TRUE while the Windows Audio service runs (also TRUE when its state
 * cannot be read: stream errors then report the outage). */
gboolean radios_wasapi_service_running(void);

/* ------------------------------------------------------------------ errors */

/* [any] Copies a UTF-8 text into a fixed buffer, cutting only at a character
 * boundary (GTK and G_DEBUG=fatal-warnings never see broken UTF-8). */
void radios_wasapi_copy_text(gchar *destination, gsize size, const gchar *text);

/* ------------------------------------------------------------------ endpoints */

typedef struct {
    gchar id[RADIOS_WASAPI_ID_SIZE];     /* stable identifier (see above) */
    gchar name[RADIOS_WASAPI_NAME_SIZE]; /* friendly name, fallback description, fallback id */
    RadioSWasapiFlow flow;
    guint channels, rate;                /* shared-mode mix format; 0 when unknown */
    guint32 state;                       /* DEVICE_STATE_* bits; 1 = active */
    gboolean is_default;                 /* default of its flow, eConsole role */
    gboolean is_default_communications;  /* default of its flow, eCommunications role */
} RadioSWasapiEndpoint;

typedef struct RadioSWasapiEnumerator RadioSWasapiEnumerator;

/* [MTA] IMMDeviceEnumerator. NULL + error when the audio system cannot be
 * reached. The object may be used from any MTA thread; free it on one. */
RadioSWasapiEnumerator *radios_wasapi_enumerator_new(gchar error[RADIOS_WASAPI_ERROR_SIZE]);
void radios_wasapi_enumerator_free(RadioSWasapiEnumerator *enumerator);

/* [MTA] Active endpoints of one direction, in Windows' order. cancelled (may
 * be NULL) is polled between endpoints; when it returns TRUE the call stops
 * and returns OK with *cancelled_out = TRUE. Endpoints whose identifier
 * exceeds 512 bytes are skipped. FAILED/SERVICE_DOWN leave *count at 0. */
RadioSWasapiStatus radios_wasapi_enumerator_list(RadioSWasapiEnumerator *enumerator, RadioSWasapiFlow flow,
    RadioSWasapiEndpoint *out, guint capacity, guint *count,
    gboolean (*cancelled)(gpointer data), gpointer data, gboolean *cancelled_out,
    gchar error[RADIOS_WASAPI_ERROR_SIZE]);
/* [MTA] One endpoint by identifier, in any state (unplugged endpoints keep
 * their friendly name: "Bağlı değil · Hoparlör (USB)"). FALSE when unknown. */
gboolean radios_wasapi_enumerator_lookup(RadioSWasapiEnumerator *enumerator, const gchar *id,
    RadioSWasapiEndpoint *out);
/* [MTA] Identifier of the current default endpoint (eConsole). FALSE: none. */
gboolean radios_wasapi_enumerator_default(RadioSWasapiEnumerator *enumerator, RadioSWasapiFlow flow,
    gchar id[RADIOS_WASAPI_ID_SIZE]);
/* [MTA] Highest rate one render endpoint accepts: exclusive-mode PCM support
 * (44.1 kHz..768 kHz, 24-in-32 or 16-bit stereo) or its mix-format rate.
 * Activates IAudioClient but never opens a stream. 0 when unknown; *in_use is
 * TRUE when another application holds the endpoint exclusively. */
guint radios_wasapi_endpoint_max_rate(RadioSWasapiEnumerator *enumerator, const gchar *id, gboolean *in_use);

/* ------------------------------------------------------------------ hotplug */

typedef enum {
    RADIOS_WASAPI_CHANGE_ADDED,
    RADIOS_WASAPI_CHANGE_REMOVED,
    RADIOS_WASAPI_CHANGE_STATE,     /* new_state carries DEVICE_STATE_* */
    RADIOS_WASAPI_CHANGE_DEFAULT,   /* flow/role; endpoint_id NULL when no default remains */
    RADIOS_WASAPI_CHANGE_PROPERTY   /* relevant: friendly name, description or mix format */
} RadioSWasapiChangeKind;

typedef struct {
    RadioSWasapiChangeKind kind;
    const gchar *endpoint_id;  /* UTF-8, valid during the call only; may be NULL */
    guint32 new_state;
    RadioSWasapiFlow flow;
    gint role;                 /* 0 console, 1 multimedia, 2 communications */
    gboolean relevant;         /* ADDED/REMOVED/STATE: TRUE; DEFAULT: console role; PROPERTY: listed keys */
} RadioSWasapiChange;

/* Runs on Windows' notification threads. It must return quickly, must not
 * call into the enumerator or (un)register watches, and must not wait for a
 * lock that a thread holds while it calls radios_wasapi_watch_remove/free. */
typedef void (*RadioSWasapiChangeFunc)(const RadioSWasapiChange *change, gpointer data);
typedef struct RadioSWasapiWatch RadioSWasapiWatch;

/* [MTA] Registers one IMMNotificationClient with the enumerator (which must
 * outlive the watch). Callbacks are added and removed at any time. */
RadioSWasapiWatch *radios_wasapi_watch_new(RadioSWasapiEnumerator *enumerator, gchar error[RADIOS_WASAPI_ERROR_SIZE]);
/* [any] Returns an id > 0. */
guint radios_wasapi_watch_add(RadioSWasapiWatch *watch, RadioSWasapiChangeFunc func, gpointer data);
/* [any] Returns after any running call of that callback has finished; no
 * call starts afterwards. */
void radios_wasapi_watch_remove(RadioSWasapiWatch *watch, guint id);
/* [MTA] Unregisters from Windows, waits for running callbacks, frees. A late
 * notification from Windows after this point is ignored safely. */
void radios_wasapi_watch_free(RadioSWasapiWatch *watch);

/* ------------------------------------------------------------------ waiting */

typedef struct RadioSWasapiStream RadioSWasapiStream;
/* An auto-reset event any thread can signal to wake a stream thread. */
typedef struct RadioSWasapiWaker RadioSWasapiWaker;
RadioSWasapiWaker *radios_wasapi_waker_new(void);                 /* [any] NULL on failure */
void radios_wasapi_waker_signal(RadioSWasapiWaker *waker);       /* [any] */
void radios_wasapi_waker_free(RadioSWasapiWaker *waker);         /* [any] */

typedef enum {
    RADIOS_WASAPI_WAIT_TIMEOUT, RADIOS_WASAPI_WAIT_WAKER, RADIOS_WASAPI_WAIT_STREAM, RADIOS_WASAPI_WAIT_FAILED
} RadioSWasapiWait;
/* [owner] Waits for the waker (checked first), the stream's device-period
 * event, or the timeout. Either argument may be NULL; with neither it sleeps. */
RadioSWasapiWait radios_wasapi_wait(RadioSWasapiStream *stream, RadioSWasapiWaker *waker, guint timeout_ms);

/* ------------------------------------------------------------------ streams */

typedef enum {
    RADIOS_WASAPI_STREAM_RENDER,           /* shared-mode playback, event driven */
    RADIOS_WASAPI_STREAM_CAPTURE,          /* capture endpoint: microphone, line input */
    RADIOS_WASAPI_STREAM_LOOPBACK,         /* what a render endpoint plays (endpoint loopback) */
    RADIOS_WASAPI_STREAM_PROCESS_LOOPBACK  /* what one process tree plays (Windows 10 2004+) */
} RadioSWasapiStreamKind;

typedef struct {
    RadioSWasapiStreamKind kind;
    /* RENDER/LOOPBACK: render endpoint id or "@default"; CAPTURE: capture
     * endpoint id or "@default". A "<id>.monitor" given to CAPTURE or
     * LOOPBACK opens the loopback of that render endpoint. Ignored by
     * PROCESS_LOOPBACK. */
    const gchar *endpoint_id;
    guint32 process_id;             /* PROCESS_LOOPBACK target */
    gboolean exclude_process_tree;  /* PROCESS_LOOPBACK: everything except that tree */
    guint rate;                     /* rate of the frames exchanged with the caller */
    /* Endpoint buffer at `rate`. RENDER: the latency target the caller keeps
     * queued (the buffer is made one period larger); 0 = two periods.
     * Capture kinds: 0 = 200 ms. */
    guint buffer_frames;
    const guint8 *session_guid;     /* 16 bytes (GUID layout); NULL = the process session */
    const gchar *session_name;      /* volume mixer name; NULL keeps Windows' default */
    gboolean raw;                   /* bypass endpoint effects when supported */
    guint activation_timeout_ms;    /* PROCESS_LOOPBACK activation; 0 = 5000 */
} RadioSWasapiStreamSpec;

/* Frames exchanged with the caller are always interleaved float32 stereo at
 * spec->rate. Windows converts rate, sample type and channels (AUTOCONVERTPCM
 * with default-quality SRC); when a driver refuses that, the layer converts
 * channels and sample type of the endpoint's mix format itself (mix rate
 * must then equal the requested rate). Capture channel mapping is
 * deterministic: mono is duplicated at full scale, more than two channels are
 * downmixed (centre and surrounds at -3 dB, LFE dropped). */
typedef struct {
    guint rate;
    guint buffer_frames;   /* endpoint buffer capacity at rate */
    guint period_frames;   /* device period at rate (wake granularity) */
    guint mix_rate, mix_channels;
    gboolean converted;    /* Windows' AUTOCONVERTPCM path is in use */
    gboolean event_driven;
    gboolean raw;
    gchar endpoint_id[RADIOS_WASAPI_ID_SIZE];     /* resolved ("" for process loopback) */
    gchar endpoint_name[RADIOS_WASAPI_NAME_SIZE];
} RadioSWasapiStreamInfo;

typedef struct {
    guint64 frames;            /* render: frames written; capture: frames delivered */
    guint64 underruns;         /* render: started stream found with an empty device buffer */
    guint64 discontinuities;   /* capture: packets flagged DATA_DISCONTINUITY (overrun/glitch) */
    guint64 silent_packets;    /* capture: packets flagged SILENT */
    guint64 timestamp_errors;  /* capture: packets flagged TIMESTAMP_ERROR */
} RadioSWasapiStreamStats;

#define RADIOS_WASAPI_PACKET_SILENT 1u
#define RADIOS_WASAPI_PACKET_DISCONTINUITY 2u

/* [MTA] Opens and initialises a stream (not started). enumerator may be NULL
 * (a private one is used). On failure *stream stays NULL, error holds the
 * Turkish operator text and the status tells how to react. info may be NULL. */
RadioSWasapiStatus radios_wasapi_stream_open(RadioSWasapiEnumerator *enumerator, const RadioSWasapiStreamSpec *spec,
    RadioSWasapiStream **stream, RadioSWasapiStreamInfo *info, gchar error[RADIOS_WASAPI_ERROR_SIZE]);
/* [owner] Frames queued in the endpoint buffer (render: not yet played). */
RadioSWasapiStatus radios_wasapi_stream_padding(RadioSWasapiStream *stream, guint *queued);
/* [owner] Render only: frames must fit in buffer_frames - padding. stereo
 * NULL writes silence. */
RadioSWasapiStatus radios_wasapi_stream_write(RadioSWasapiStream *stream, const gfloat *stereo, guint frames);
/* [owner] Capture kinds: copies up to capacity frames of the next packet
 * (the rest is kept for the next call). *frames 0 means no packet is ready.
 * flags receives RADIOS_WASAPI_PACKET_* bits (silent packets are zeros). */
RadioSWasapiStatus radios_wasapi_stream_read(RadioSWasapiStream *stream, gfloat *stereo, guint capacity,
    guint *frames, guint *flags);
/* [owner] IAudioClock position in frames at rate since start or reset. */
RadioSWasapiStatus radios_wasapi_stream_position(RadioSWasapiStream *stream, guint64 *frames);
RadioSWasapiStatus radios_wasapi_stream_start(RadioSWasapiStream *stream);
RadioSWasapiStatus radios_wasapi_stream_stop(RadioSWasapiStream *stream);
/* [owner] Stopped stream only: drops queued audio, position returns to 0. */
RadioSWasapiStatus radios_wasapi_stream_reset(RadioSWasapiStream *stream);
void radios_wasapi_stream_stats(RadioSWasapiStream *stream, RadioSWasapiStreamStats *stats);
/* Turkish text and HRESULT of the last non-OK status. */
const gchar *radios_wasapi_stream_error(RadioSWasapiStream *stream);
guint32 radios_wasapi_stream_hresult(RadioSWasapiStream *stream);
/* [owner] Stops and releases every interface; NULL is ignored. */
void radios_wasapi_stream_close(RadioSWasapiStream *stream);

/* [MTA] Runtime probe (cached for the process) whether process loopback
 * capture works on this Windows. FALSE with the Turkish reason otherwise. */
gboolean radios_wasapi_process_loopback_supported(gchar reason[RADIOS_WASAPI_ERROR_SIZE]);

/* ------------------------------------------------------------------ applications */

typedef struct {
    guint32 pid;
    guint64 start_time;                        /* process creation FILETIME (100 ns); 0 unknown */
    gchar exe[RADIOS_WASAPI_NAME_SIZE];        /* image basename ("spotify.exe"); "" when not readable */
    gchar name[RADIOS_WASAPI_NAME_SIZE];       /* session name, file description, basename or "PID n" */
    gchar endpoint_id[RADIOS_WASAPI_ID_SIZE];  /* render endpoint where it plays (first active one) */
    gboolean active;                           /* a session of the process is playing */
} RadioSWasapiSession;

/* [MTA] Audio sessions of every active render endpoint, one entry per
 * process (system sounds and multi-process sessions skipped), sorted by
 * name then pid. include_self FALSE drops this process. */
RadioSWasapiStatus radios_wasapi_sessions_list(RadioSWasapiEnumerator *enumerator, gboolean include_self,
    RadioSWasapiSession *out, guint capacity, guint *count, gchar error[RADIOS_WASAPI_ERROR_SIZE]);
/* [any] Creation time of a running process (identity check before capture). */
gboolean radios_wasapi_process_start_time(guint32 pid, guint64 *start_time);

/* ------------------------------------------------------------------ diagnostics and tests */

typedef enum {
    RADIOS_WASAPI_FAULT_NONE,
    RADIOS_WASAPI_FAULT_INVALIDATED,  /* AUDCLNT_E_DEVICE_INVALIDATED, endpoint still active */
    RADIOS_WASAPI_FAULT_DEVICE_GONE,  /* AUDCLNT_E_DEVICE_INVALIDATED, endpoint removed */
    RADIOS_WASAPI_FAULT_SERVICE       /* AUDCLNT_E_SERVICE_NOT_RUNNING */
} RadioSWasapiFault;
/* [any] The next padding/read call of any stream fails once with fault. */
void radios_wasapi_debug_inject_fault(RadioSWasapiFault fault);
/* [any] radios_wasapi_service_running() reports FALSE for this long. */
void radios_wasapi_debug_service_outage(guint milliseconds);

typedef struct {
    gint notify_clients;       /* live IMMNotificationClient objects of this layer */
    gint activation_handlers;  /* live activation completion handlers */
    gint streams;              /* open streams */
    guint unreleased_clients;  /* IAudioClient objects that still had references after close */
    guint64 notifications;     /* notifications dispatched to callbacks */
} RadioSWasapiDebugCounters;
void radios_wasapi_debug_counters(RadioSWasapiDebugCounters *counters);
/* [any] Exercises QueryInterface/AddRef/Release of the C COM objects. */
gboolean radios_wasapi_debug_com_selftest(gchar error[RADIOS_WASAPI_ERROR_SIZE]);
/* [any] Delivers a notification through the registered client exactly as
 * Windows would call it (test hook). */
void radios_wasapi_debug_watch_emit(RadioSWasapiWatch *watch, RadioSWasapiChangeKind kind, const gchar *endpoint_id);

G_END_DECLS
#endif
