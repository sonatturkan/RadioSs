#ifndef RADIOS_AUDIO_INTERNAL_H
#define RADIOS_AUDIO_INTERNAL_H
#include "radios/audio.h"
#include "radios/audio_output.h"
#include <stdatomic.h>

typedef struct {
    guint capacity;
    gfloat *samples;
    guint64 *tags;
    _Atomic guint64 read, write;
} RadioSAudioRing;
guint radios_audio_ring_read(RadioSAudioRing *ring, gfloat *samples, guint frames, guint64 *last_tag);
guint radios_audio_ring_write(RadioSAudioRing *ring, const gfloat *samples, guint frames, guint64 first_tag);

/* Physical output backend of the realtime engine (Windows audio, WASAPI).
 * Three output buses (MAIN/CUE/MONITOR) consume outputs[] and the talk
 * microphone produces 48 kHz stereo into capture. Every call is cheap and
 * never waits for a device, except flush/flush_buses (bounded 100 ms). */
typedef struct RadioSAudioBackend RadioSAudioBackend;
typedef struct {
    gboolean connected, ready[3], microphone_ready;
    guint64 played_tag[3], consumed_frames[3];
    guint output_sample_rate[3], output_buffer_frames[3];
    gfloat peak[3][2], rms[3][2], microphone_peak[2];
    gchar error[256], bus_error[3][256], microphone_error[256];
    guint output_count, input_count;
    RadioSAudioDevice outputs[64], inputs[64];
    guint64 underruns[3];   /* device buffer found empty while playing (diagnostics) */
    guint64 gap_frames[3];  /* silence inserted while playing because the engine was late (dropouts) */
    guint64 device_scans;   /* completed device inventory scans (diagnostics) */
} RadioSAudioBackendSnapshot;
/* The one sound-server locator rule of the audio module (engine backend,
 * patchbay, channel inputs): server, else $RADIOS_AUDIO_SERVER, else
 * "wasapi". TRUE for the Windows audio service ("wasapi", "wasapi:");
 * FALSE with the Turkish reason for anything else, e.g. a PulseAudio
 * address kept from Linux tests. error may be NULL. */
gboolean radios_audio_server_resolve(const gchar *server, gchar error[256]);
/* server: see radios_audio_server_resolve(). Unknown locators report
 * connected=FALSE with that Turkish error. Never NULL; start failures are
 * reported through the snapshot. */
RadioSAudioBackend *radios_audio_backend_new(RadioSAudioRing *outputs[3], RadioSAudioRing *capture, const gchar *server);
void radios_audio_backend_apply(RadioSAudioBackend *backend, const RadioSAudioPreferences *preferences);
void radios_audio_backend_configure_output(RadioSAudioBackend *backend, guint rate, guint buffer_frames);
void radios_audio_backend_refresh(RadioSAudioBackend *backend);
void radios_audio_backend_flush(RadioSAudioBackend *backend);
void radios_audio_backend_flush_buses(RadioSAudioBackend *backend, guint bus_mask);
void radios_audio_backend_snapshot(RadioSAudioBackend *backend, RadioSAudioBackendSnapshot *snapshot);
/* Worker-only bounded reconnect to the original server. Reopening streams
 * never starts an engine deck or opens the microphone's MAIN gate. */
void radios_audio_backend_reconnect(RadioSAudioBackend *backend);
void radios_audio_backend_free(RadioSAudioBackend *backend);
/* Operator name of the sound system behind this backend. */
const gchar *radios_audio_backend_label(RadioSAudioBackend *backend);

/* Provider seam: the backend core (threads, bus state machine, tags and
 * played evidence, flush/refill, hotplug debounce, retries) talks to devices
 * only through these operations. The product provider is WASAPI; tests and
 * CI machines without sound hardware plug in deterministic providers. */
typedef enum {
    RADIOS_AUDIO_IO_OK = 0,
    RADIOS_AUDIO_IO_LOST = -1,         /* device removed/unknown: error, retried */
    RADIOS_AUDIO_IO_REOPEN = -2,       /* invalidated, device still present: silent reopen */
    RADIOS_AUDIO_IO_SERVICE_DOWN = -3, /* the audio service stopped */
    RADIOS_AUDIO_IO_FAILED = -4        /* anything else; the provider names it */
} RadioSAudioIoStatus;
typedef enum {
    RADIOS_AUDIO_HOTPLUG_DEVICES, RADIOS_AUDIO_HOTPLUG_DEFAULT, RADIOS_AUDIO_HOTPLUG_IGNORED
} RadioSAudioHotplugKind;
typedef struct {
    void (*notify)(gpointer core, RadioSAudioHotplugKind kind);   /* any thread; never blocks */
    gpointer core;
} RadioSAudioHotplugSink;
typedef struct {
    guint rate;            /* stream rate as opened (snapshot output_sample_rate) */
    guint buffer_frames;   /* device buffer capacity, frames at rate */
    guint period_frames;   /* device period (wake granularity), frames at rate */
} RadioSAudioStreamInfo;
typedef struct RadioSAudioProvider RadioSAudioProvider;
typedef struct RadioSAudioProviderStream RadioSAudioProviderStream;
typedef struct {
    const gchar *label;
    /* any thread */
    RadioSAudioProvider *(*create)(const RadioSAudioHotplugSink *sink, gpointer config);
    void (*destroy)(RadioSAudioProvider *provider);
    /* called first/last on every backend thread; realtime for stream threads */
    gpointer (*thread_enter)(gboolean realtime);
    void (*thread_leave)(gpointer cookie);
    /* manager thread */
    RadioSAudioIoStatus (*connect)(RadioSAudioProvider *provider, gchar error[256]);
    void (*disconnect)(RadioSAudioProvider *provider);
    gboolean (*service_running)(RadioSAudioProvider *provider);
    RadioSAudioIoStatus (*scan)(RadioSAudioProvider *provider, RadioSAudioDevice outputs[64], guint *output_count,
        RadioSAudioDevice inputs[64], guint *input_count, gboolean (*cancelled)(gpointer data), gpointer data);
    guint (*max_rate)(RadioSAudioProvider *provider);   /* may be NULL */
    /* bus (slot 0..2) or microphone (slot 3) thread; that thread owns the stream */
    RadioSAudioIoStatus (*open)(RadioSAudioProvider *provider, gboolean capture, const gchar *device, guint rate,
        guint buffer_frames, guint slot, RadioSAudioProviderStream **stream, RadioSAudioStreamInfo *info, gchar error[256]);
    /* Waits for the stream's period event or the core's wake object (a
     * RadioSWasapiWaker of wasapi_device.h owned by the core). TRUE when the
     * stream signalled a device period, FALSE on wake or timeout. stream may
     * be NULL. A started render stream that signals nothing for 2 s counts
     * as stalled and is reopened. */
    gboolean (*wait)(RadioSAudioProviderStream *stream, gpointer wake, guint timeout_ms);
    RadioSAudioIoStatus (*padding)(RadioSAudioProviderStream *stream, guint *queued);
    RadioSAudioIoStatus (*write)(RadioSAudioProviderStream *stream, const gfloat *stereo /* NULL = silence */, guint frames);
    RadioSAudioIoStatus (*read)(RadioSAudioProviderStream *stream, gfloat *stereo48k, guint capacity, guint *frames, gboolean *silent);
    RadioSAudioIoStatus (*position)(RadioSAudioProviderStream *stream, guint64 *frames_played);
    RadioSAudioIoStatus (*start)(RadioSAudioProviderStream *stream);
    RadioSAudioIoStatus (*stop)(RadioSAudioProviderStream *stream);
    RadioSAudioIoStatus (*reset)(RadioSAudioProviderStream *stream);   /* stopped only: queue dropped, position 0 */
    guint64 (*underruns)(RadioSAudioProviderStream *stream);           /* may be NULL */
    const gchar *(*last_error)(RadioSAudioProviderStream *stream);
    void (*close)(RadioSAudioProviderStream *stream);
} RadioSAudioProviderOps;
/* A backend whose devices come from ops (tests, alternative providers). */
RadioSAudioBackend *radios_audio_backend_new_with_provider(RadioSAudioRing *outputs[3], RadioSAudioRing *capture,
    const RadioSAudioProviderOps *ops, gpointer config);

RadioSAudioOutput *radios_audio_output_new_internal(RadioSAudioOutputKind kind);
void radios_audio_output_push_internal(RadioSAudioOutput *output, const gfloat *samples, guint frames);
void radios_audio_output_shutdown_internal(RadioSAudioOutput *output);
#endif
