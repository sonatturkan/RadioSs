#define _POSIX_C_SOURCE 200809L
#include "radios/audio_patchbay.h"
#include "audio_internal.h"
#include <pulse/pulseaudio.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#define RATE 48000u
#define BLOCK 480u
#define FRAME_BYTES (2u * sizeof(gfloat))
#define RING_FRAMES 48000u
/* Sources start after 20 ms of audio and are trimmed back to 40 ms when
 * their clock runs ahead of the 10 ms step clock. */
#define PRIME 960u
#define LIMIT 9600u
#define KEEP 1920u
#define STEP_US 10000
#define RETRY_US 5000000
#define RESYNC_US 3000000

/* One Pulse capture or playback stream and its lock-free ring. */
typedef struct {
    struct RadioSAudioPatchbay *owner;
    pa_stream *stream;
    RadioSAudioRing ring;
    gchar device[RADIOS_ROUTING_GRAPH_TEXT_SIZE];
    guint64 epoch;
    gboolean playback, primed;
    gint ready, failed;
} Link;
typedef struct {
    guint id;
    RadioSRoutingGraphNode config;
    gboolean sends, feeds;
    Link *capture, *playback;
    RadioSAudioTap *tap;
    gint tap_source;
    gboolean tap_primed;
    RadioSAudioLine *line;
    gint line_target;
    guint32 module, mic_module;
    guint64 module_epoch;
    gchar module_name[RADIOS_ROUTING_GRAPH_NAME_SIZE];
    gboolean module_mic;
    RadioSAudioOutput *recorder;
    gboolean recording, paused;
    guint segment;
    guint64 segment_frames, total_frames;
    gchar path[4097];
    RadioSAudioOutput *stream;
    gboolean stream_wanted;
    gchar *password;
    gint64 retry_at, connected_us;
    guint reconnects;
    gdouble gate;
    gfloat in[BLOCK * 2], out[BLOCK * 2];
    gfloat peak[2];
    gdouble squares[2];
    guint64 measured;
    gchar error[256];
} Live;
typedef struct { guint id; gboolean stream; gchar *password; } Intent;

struct RadioSAudioPatchbay {
    gatomicrefcount refs;
    GMutex mutex;
    GCond tick, control_wake;
    GThread *worker, *control;
    gboolean closing;
    /* Requested state, guarded by mutex. */
    RadioSRoutingGraph *desired;
    gboolean desired_running, refresh_requested, resync;
    guint64 desired_serial, applied_serial;
    gint64 resync_at;
    GPtrArray *intents, *retired;
    /* Plan run by the worker; resources are owned by the control thread. */
    Live *live[RADIOS_ROUTING_GRAPH_MAX_NODES];
    guint live_count, order[RADIOS_ROUTING_GRAPH_MAX_NODES];
    RadioSRoutingGraphEdge edges[RADIOS_ROUTING_GRAPH_MAX_EDGES];
    guint edge_count;
    gboolean running, busy;
    RadioSAudioEngine *engine;
    gchar *recording_root;
    /* Pulse; callbacks run on the mainloop thread with its lock held. */
    gchar *server;
    pa_threaded_mainloop *loop;
    pa_context *context;
    guint64 epoch;
    gint64 reconnect_at;
    gboolean connected, cleaned;
    RadioSPatchDevice staged_outputs[RADIOS_PATCHBAY_MAX_DEVICES], staged_inputs[RADIOS_PATCHBAY_MAX_DEVICES];
    RadioSPatchApplication staged_apps[RADIOS_PATCHBAY_MAX_DEVICES];
    guint staged_output_count, staged_input_count, staged_app_count;
    RadioSPatchDevice outputs[RADIOS_PATCHBAY_MAX_DEVICES], inputs[RADIOS_PATCHBAY_MAX_DEVICES];
    RadioSPatchApplication apps[RADIOS_PATCHBAY_MAX_DEVICES];
    guint output_count, input_count, app_count;
    gchar error[256];
    guint64 underruns, trimmed;
};
typedef struct { gboolean done, success; guint32 index; } Query;

static gboolean fail(GError **error, const gchar *message)
{ g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, message); return FALSE; }
static void ring_new(RadioSAudioRing *ring)
{ ring->capacity = RING_FRAMES; ring->samples = g_new0(gfloat, RING_FRAMES * 2); ring->tags = NULL; atomic_init(&ring->read, 0); atomic_init(&ring->write, 0); }
static guint64 queued(RadioSAudioRing *ring)
{ return atomic_load(&ring->write) - atomic_load(&ring->read); }
static void skip(RadioSAudioRing *ring, guint64 frames)
{ guint64 read = atomic_load(&ring->read); atomic_store(&ring->read, read + MIN(frames, atomic_load(&ring->write) - read)); }
static void secure_free(gchar *text)
{ if (text) { memset(text, 0, strlen(text)); g_free(text); } }
static gboolean own_application(const gchar *name, const gchar *binary)
{ return (name && g_str_has_prefix(name, "RadioS")) || (binary && !g_ascii_strcasecmp(binary, "radios")); }
static void virtual_sink(guint id, gchar *sink, gsize size) { g_snprintf(sink, size, "radios_virtual_%u", id); }
static gchar *pulse_quoted(const gchar *text)
{
    /* Module arguments are parsed twice; keep descriptions free of quotes. */
    GString *value = g_string_new(NULL);
    for (const gchar *c = text; *c; ++c) g_string_append_c(value, *c == '\'' || *c == '"' || *c == '\\' ? ' ' : *c);
    return g_string_free(value, FALSE);
}

/* Pulse helpers (control thread, loop locked). */
static gboolean wait_query(RadioSAudioPatchbay *pb, pa_operation *operation, Query *query)
{
    if (!operation) return FALSE;
    gint64 deadline = g_get_monotonic_time() + 3000000;
    while (!query->done && pa_operation_get_state(operation) == PA_OPERATION_RUNNING && g_get_monotonic_time() < deadline) {
        pa_threaded_mainloop_unlock(pb->loop); g_usleep(2000); pa_threaded_mainloop_lock(pb->loop);
    }
    if (pa_operation_get_state(operation) == PA_OPERATION_RUNNING) pa_operation_cancel(operation);
    pa_operation_unref(operation); return query->done && query->success;
}
static void query_success(pa_context *context, int success, void *data)
{ (void)context; Query *query = data; query->success = success != 0; query->done = TRUE; }
static void query_index(pa_context *context, uint32_t index, void *data)
{ (void)context; Query *query = data; query->index = index; query->success = index != PA_INVALID_INDEX; query->done = TRUE; }
static gboolean pulse_ready(RadioSAudioPatchbay *pb)
{ return pb->context && pa_context_get_state(pb->context) == PA_CONTEXT_READY; }

static void link_state(pa_stream *stream, void *data)
{
    Link *link = data; pa_stream_state_t state = pa_stream_get_state(stream);
    if (state == PA_STREAM_READY) g_atomic_int_set(&link->ready, 1);
    if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED) { g_atomic_int_set(&link->ready, 0); g_atomic_int_set(&link->failed, 1); }
}
static void link_read(pa_stream *stream, size_t bytes, void *data)
{
    (void)bytes; Link *link = data; const void *samples = NULL; size_t size = 0;
    if (pa_stream_peek(stream, &samples, &size) < 0) { g_atomic_int_set(&link->failed, 1); return; }
    if (size % FRAME_BYTES == 0) {
        guint frames = (guint)(size / FRAME_BYTES);
        if (samples) (void)radios_audio_ring_write(&link->ring, samples, frames, 0);
        else {
            gfloat silence[BLOCK * 2] = {0};
            for (guint done = 0; done < frames;) { guint count = MIN(frames - done, BLOCK); (void)radios_audio_ring_write(&link->ring, silence, count, 0); done += count; }
        }
    }
    if (size) (void)pa_stream_drop(stream);
}
static void link_write(pa_stream *stream, size_t bytes, void *data)
{
    Link *link = data; void *buffer = NULL; size_t size = bytes - bytes % FRAME_BYTES;
    if (!size || pa_stream_begin_write(stream, &buffer, &size) < 0 || !buffer) return;
    size -= size % FRAME_BYTES;
    guint frames = (guint)(size / FRAME_BYTES), got = 0; guint64 waiting = queued(&link->ring);
    if (!link->primed && waiting >= PRIME) link->primed = TRUE;
    if (link->primed) {
        if (waiting > LIMIT) skip(&link->ring, waiting - KEEP);
        got = radios_audio_ring_read(&link->ring, buffer, frames, NULL);
        if (got < frames) link->primed = FALSE;
    }
    if (got < frames) memset((gfloat *)buffer + got * 2, 0, (frames - got) * FRAME_BYTES);
    if (pa_stream_write(stream, buffer, frames * FRAME_BYTES, NULL, 0, PA_SEEK_RELATIVE) < 0) g_atomic_int_set(&link->failed, 1);
}
static Link *link_open(RadioSAudioPatchbay *pb, const gchar *device, gboolean playback, const gchar *title, gchar error[256])
{
    if (!pulse_ready(pb)) { g_strlcpy(error, "Ses sunucusuna bağlanılıyor…", 256); return NULL; }
    Link *link = g_new0(Link, 1); link->owner = pb; link->playback = playback; link->epoch = pb->epoch;
    g_strlcpy(link->device, device, sizeof link->device); ring_new(&link->ring);
    pa_sample_spec spec = {PA_SAMPLE_FLOAT32NE, RATE, 2};
    pa_proplist *properties = pa_proplist_new();
    pa_proplist_sets(properties, PA_PROP_MEDIA_NAME, title);
    pa_proplist_sets(properties, PA_PROP_MEDIA_ROLE, "production");
    link->stream = pa_stream_new_with_proplist(pb->context, title, &spec, NULL, properties); pa_proplist_free(properties);
    if (!link->stream) { g_strlcpy(error, "Ses akışı oluşturulamadı.", 256); g_free(link->ring.samples); g_free(link); return NULL; }
    pa_stream_set_state_callback(link->stream, link_state, link);
    gint result;
    if (playback) {
        pa_buffer_attr attr = {(uint32_t)-1, KEEP * FRAME_BYTES * 2, (uint32_t)-1, BLOCK * FRAME_BYTES, (uint32_t)-1};
        pa_stream_set_write_callback(link->stream, link_write, link);
        result = pa_stream_connect_playback(link->stream, device, &attr, PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE, NULL, NULL);
    } else {
        pa_buffer_attr attr = {RATE * FRAME_BYTES, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, BLOCK * FRAME_BYTES};
        pa_stream_set_read_callback(link->stream, link_read, link);
        result = pa_stream_connect_record(link->stream, device, &attr, PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE);
    }
    gint64 deadline = g_get_monotonic_time() + 3000000;
    while (result >= 0 && pa_stream_get_state(link->stream) == PA_STREAM_CREATING && g_get_monotonic_time() < deadline) {
        pa_threaded_mainloop_unlock(pb->loop); g_usleep(2000); pa_threaded_mainloop_lock(pb->loop);
    }
    if (result < 0 || pa_stream_get_state(link->stream) != PA_STREAM_READY) {
        g_snprintf(error, 256, "Aygıt açılamadı: %s", device);
        pa_stream_set_state_callback(link->stream, NULL, NULL); pa_stream_set_read_callback(link->stream, NULL, NULL);
        pa_stream_set_write_callback(link->stream, NULL, NULL);
        (void)pa_stream_disconnect(link->stream); pa_stream_unref(link->stream);
        g_free(link->ring.samples); g_free(link); return NULL;
    }
    return link;
}
static void link_close(RadioSAudioPatchbay *pb, Link *link)
{
    if (!link) return;
    if (link->stream) {
        pa_threaded_mainloop_lock(pb->loop);
        pa_stream_set_state_callback(link->stream, NULL, NULL); pa_stream_set_read_callback(link->stream, NULL, NULL);
        pa_stream_set_write_callback(link->stream, NULL, NULL);
        if (link->epoch == pb->epoch && pa_stream_get_state(link->stream) == PA_STREAM_READY) (void)pa_stream_disconnect(link->stream);
        pa_stream_unref(link->stream); pa_threaded_mainloop_unlock(pb->loop);
    }
    g_free(link->ring.samples); g_free(link);
}
static gboolean module_load(RadioSAudioPatchbay *pb, const gchar *name, const gchar *arguments, guint32 *index)
{
    Query query = {0};
    if (!wait_query(pb, pa_context_load_module(pb->context, name, arguments, query_index, &query), &query)) return FALSE;
    *index = query.index; return TRUE;
}
static void module_unload(RadioSAudioPatchbay *pb, guint32 index)
{
    if (index == PA_INVALID_INDEX || !pulse_ready(pb)) return;
    Query query = {0}; (void)wait_query(pb, pa_context_unload_module(pb->context, index, query_success, &query), &query);
}

/* Inventory: list callbacks stage, the end callback publishes. */
static void device_entry(RadioSPatchDevice *list, guint *count, guint32 index, const gchar *name, const gchar *description, gboolean monitor)
{
    if (!name || *count >= RADIOS_PATCHBAY_MAX_DEVICES || strlen(name) >= RADIOS_PATCHBAY_NAME_SIZE) return;
    RadioSPatchDevice *device = &list[(*count)++]; memset(device, 0, sizeof *device);
    g_strlcpy(device->name, name, sizeof device->name);
    g_strlcpy(device->description, description && *description ? description : name, sizeof device->description);
    device->monitor = monitor; device->index = index;
}
static void sink_listed(pa_context *context, const pa_sink_info *info, int end, void *data)
{
    (void)context; RadioSAudioPatchbay *pb = data;
    if (!end && info) { device_entry(pb->staged_outputs, &pb->staged_output_count, info->index, info->name, info->description, FALSE); return; }
    g_mutex_lock(&pb->mutex); memcpy(pb->outputs, pb->staged_outputs, sizeof pb->outputs);
    pb->output_count = pb->staged_output_count; pb->staged_output_count = 0; g_mutex_unlock(&pb->mutex);
}
static void source_listed(pa_context *context, const pa_source_info *info, int end, void *data)
{
    (void)context; RadioSAudioPatchbay *pb = data;
    if (!end && info) {
        device_entry(pb->staged_inputs, &pb->staged_input_count, info->index, info->name, info->description, info->monitor_of_sink != PA_INVALID_INDEX);
        return;
    }
    g_mutex_lock(&pb->mutex); memcpy(pb->inputs, pb->staged_inputs, sizeof pb->inputs);
    pb->input_count = pb->staged_input_count; pb->staged_input_count = 0; g_mutex_unlock(&pb->mutex);
}
static void application_listed(pa_context *context, const pa_sink_input_info *info, int end, void *data)
{
    (void)context; RadioSAudioPatchbay *pb = data;
    if (!end && info) {
        const gchar *name = info->proplist ? pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_NAME) : NULL;
        const gchar *binary = info->proplist ? pa_proplist_gets(info->proplist, PA_PROP_APPLICATION_PROCESS_BINARY) : NULL;
        if (!name) name = info->name;
        if (own_application(name, binary) || pb->staged_app_count >= RADIOS_PATCHBAY_MAX_DEVICES) return;
        RadioSPatchApplication *app = &pb->staged_apps[pb->staged_app_count++]; memset(app, 0, sizeof *app);
        app->index = info->index; g_strlcpy(app->name, name ? name : "Uygulama", sizeof app->name);
        g_strlcpy(app->binary, binary ? binary : "", sizeof app->binary);
        app->sink = info->sink;
        return;
    }
    g_mutex_lock(&pb->mutex); memcpy(pb->apps, pb->staged_apps, sizeof pb->apps);
    pb->app_count = pb->staged_app_count; pb->staged_app_count = 0; g_mutex_unlock(&pb->mutex);
}
static void inventory(RadioSAudioPatchbay *pb)
{
    if (!pulse_ready(pb)) return;
    pb->staged_output_count = pb->staged_input_count = pb->staged_app_count = 0;
    pa_operation *operations[] = {
        pa_context_get_sink_info_list(pb->context, sink_listed, pb),
        pa_context_get_source_info_list(pb->context, source_listed, pb),
        pa_context_get_sink_input_info_list(pb->context, application_listed, pb)};
    for (guint i = 0; i < G_N_ELEMENTS(operations); ++i) if (operations[i]) pa_operation_unref(operations[i]);
}
static void subscribed(pa_context *context, pa_subscription_event_type_t type, uint32_t index, void *data)
{
    (void)context; (void)type; (void)index; RadioSAudioPatchbay *pb = data;
    g_mutex_lock(&pb->mutex); pb->refresh_requested = TRUE; g_cond_signal(&pb->control_wake); g_mutex_unlock(&pb->mutex);
}
static void context_state(pa_context *context, void *data)
{
    RadioSAudioPatchbay *pb = data; pa_context_state_t state = pa_context_get_state(context);
    if (state == PA_CONTEXT_READY || state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
        g_mutex_lock(&pb->mutex); g_cond_signal(&pb->control_wake); g_mutex_unlock(&pb->mutex);
    }
}
static void module_listed(pa_context *context, const pa_module_info *info, int end, void *data)
{
    (void)context; GArray *stale = data;
    if (!end && info && info->argument && (strstr(info->argument, "sink_name=radios_virtual_") || strstr(info->argument, "source_name=radios_virtual_")))
        g_array_append_val(stale, info->index);
}
/* Control thread. TRUE when a new connection became ready. */
static gboolean connection(RadioSAudioPatchbay *pb)
{
    gboolean fresh = FALSE;
    pa_threaded_mainloop_lock(pb->loop);
    pa_context_state_t state = pb->context ? pa_context_get_state(pb->context) : PA_CONTEXT_FAILED;
    g_mutex_lock(&pb->mutex);
    gboolean was = pb->connected; pb->connected = state == PA_CONTEXT_READY;
    g_mutex_unlock(&pb->mutex);
    if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
        if (pb->context) {
            g_strlcpy(pb->error, pa_strerror(pa_context_errno(pb->context)), sizeof pb->error);
            pa_context_set_state_callback(pb->context, NULL, NULL); pa_context_set_subscribe_callback(pb->context, NULL, NULL);
            pa_context_disconnect(pb->context); pa_context_unref(pb->context); pb->context = NULL;
        }
        gint64 now = g_get_monotonic_time();
        if (now >= pb->reconnect_at) {
            pb->reconnect_at = now + 2000000;
            pa_proplist *properties = pa_proplist_new();
            pa_proplist_sets(properties, PA_PROP_APPLICATION_NAME, "RadioS Patchbay");
            pa_proplist_sets(properties, "media.category", "Manager");
            pb->context = pa_context_new_with_proplist(pa_threaded_mainloop_get_api(pb->loop), "RadioS Patchbay", properties);
            pa_proplist_free(properties);
            if (pb->context) {
                pa_context_set_state_callback(pb->context, context_state, pb);
                if (pa_context_connect(pb->context, pb->server, PA_CONTEXT_NOFAIL, NULL) < 0) g_strlcpy(pb->error, "Ses sunucusuna bağlanılamadı", sizeof pb->error);
            }
        }
    } else if (state == PA_CONTEXT_READY && !was) {
        ++pb->epoch; fresh = TRUE; pb->error[0] = 0;
        pa_context_set_subscribe_callback(pb->context, subscribed, pb);
        pa_operation *operation = pa_context_subscribe(pb->context,
            PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE | PA_SUBSCRIPTION_MASK_SINK_INPUT, NULL, NULL);
        if (operation) pa_operation_unref(operation);
        if (!pb->cleaned) {
            /* Virtual devices left by a previous RadioS run that ended abruptly. */
            GArray *stale = g_array_new(FALSE, FALSE, sizeof(guint32)); Query query = {0};
            pa_operation *list = pa_context_get_module_info_list(pb->context, module_listed, stale);
            if (list) {
                gint64 deadline = g_get_monotonic_time() + 2000000;
                while (pa_operation_get_state(list) == PA_OPERATION_RUNNING && g_get_monotonic_time() < deadline) {
                    pa_threaded_mainloop_unlock(pb->loop); g_usleep(2000); pa_threaded_mainloop_lock(pb->loop);
                }
                if (pa_operation_get_state(list) == PA_OPERATION_RUNNING) pa_operation_cancel(list);
                pa_operation_unref(list);
            }
            for (guint i = 0; i < stale->len; ++i) module_unload(pb, g_array_index(stale, guint32, i));
            g_array_free(stale, TRUE); (void)query; pb->cleaned = TRUE;
        }
        inventory(pb);
    }
    pa_threaded_mainloop_unlock(pb->loop);
    return fresh;
}

/* Recorder and stream helpers (mutex held). */
static void retire(RadioSAudioPatchbay *pb, RadioSAudioOutput *output)
{
    if (!output) return;
    radios_audio_output_stop(output); g_ptr_array_add(pb->retired, output); g_cond_signal(&pb->control_wake);
}
/* Files open asynchronously, so names other recorders just chose count as taken. */
static gboolean path_taken(RadioSAudioPatchbay *pb, const gchar *path)
{
    if (g_file_test(path, G_FILE_TEST_EXISTS)) return TRUE;
    for (guint i = 0; i < pb->live_count; ++i) if (!g_strcmp0(pb->live[i]->path, path)) return TRUE;
    return FALSE;
}
static gchar *record_path(RadioSAudioPatchbay *pb, const gchar *folder, const gchar *prefix)
{
    GDateTime *now = g_date_time_new_now_local(); gchar *stamp = g_date_time_format(now, "%Y%m%d-%H%M%S");
    gchar *path = NULL;
    for (guint n = 1; n < 1000 && !path; ++n) {
        gchar *leaf = n == 1 ? g_strdup_printf("%s-%s.wav", prefix, stamp) : g_strdup_printf("%s-%s-%u.wav", prefix, stamp, n);
        gchar *candidate = g_build_filename(folder, leaf, NULL); g_free(leaf);
        if (!path_taken(pb, candidate)) path = candidate; else g_free(candidate);
    }
    g_free(stamp); g_date_time_unref(now); return path;
}
static gboolean record_open(RadioSAudioPatchbay *pb, Live *n, GError **error)
{
    const gchar *folder = n->config.folder[0] ? n->config.folder : pb->recording_root;
    if (!folder || !*folder || !g_path_is_absolute(folder)) return fail(error, "Kaydedici için bir kayıt klasörü seçin.");
    if (g_mkdir_with_parents(folder, 0700) != 0) return fail(error, "Kayıt klasörü oluşturulamadı.");
    gchar *path = record_path(pb, folder, n->config.prefix);
    if (!path || strlen(path) >= sizeof n->path) { g_free(path); return fail(error, "Kayıt dosyası adı oluşturulamadı."); }
    RadioSAudioOutput *output = radios_audio_output_new_internal(RADIOS_AUDIO_OUTPUT_RECORD);
    if (!radios_audio_output_start_record(output, path, error)) { radios_audio_output_unref(output); g_free(path); return FALSE; }
    retire(pb, n->recorder); n->recorder = output; n->segment_frames = 0;
    g_strlcpy(n->path, path, sizeof n->path); g_free(path); return TRUE;
}
static gboolean record_start_locked(RadioSAudioPatchbay *pb, Live *n, GError **error)
{
    if (n->config.kind != RG_RECORDER) return fail(error, "Bu blok bir kaydedici değil.");
    if (!pb->running) return fail(error, "Önce yönlendirmeyi başlatın.");
    if (n->recording) return TRUE;
    if (!record_open(pb, n, error)) return FALSE;
    n->recording = TRUE; n->paused = FALSE; n->segment = 1; n->total_frames = 0; n->error[0] = 0; return TRUE;
}
static void record_stop_locked(RadioSAudioPatchbay *pb, Live *n)
{
    if (!n->recording) return;
    retire(pb, n->recorder); n->recorder = NULL; n->recording = n->paused = FALSE;
}
static RadioSAudioStreamProtocol protocol_of(RadioSRoutingStreamProtocol protocol)
{ return protocol == RG_STREAM_SHOUTCAST1 ? RADIOS_STREAM_SHOUTCAST1 : protocol == RG_STREAM_SHOUTCAST2 ? RADIOS_STREAM_SHOUTCAST2 : RADIOS_STREAM_ICECAST; }
static gboolean stream_connect(Live *n, GError **error)
{
    const RadioSRoutingGraphNode *c = &n->config;
    RadioSAudioStreamTarget target = {protocol_of(c->protocol), c->host, c->mount, c->username, n->password,
        c->port, c->stream_id, c->bitrate_kbps, c->station, c->genre, c->description, c->website, c->public_listing};
    if (!n->stream) n->stream = radios_audio_output_new_internal(RADIOS_AUDIO_OUTPUT_STREAM);
    if (!radios_audio_output_start_server(n->stream, &target, error)) return FALSE;
    n->connected_us = g_get_monotonic_time(); n->retry_at = 0; return TRUE;
}
static gboolean stream_start_locked(RadioSAudioPatchbay *pb, Live *n, const gchar *password, GError **error)
{
    if (n->config.kind != RG_STREAM) return fail(error, "Bu blok bir internet yayını değil.");
    if (!pb->running) return fail(error, "Önce yönlendirmeyi başlatın.");
    if (!n->config.host[0]) return fail(error, "Yayın sunucusunun adresini girin.");
    if (!password || !*password) return fail(error, "Yayın şifresini girin.");
    if (n->stream_wanted) return TRUE;
    secure_free(n->password); n->password = g_strdup(password);
    if (!stream_connect(n, error)) { secure_free(n->password); n->password = NULL; return FALSE; }
    n->stream_wanted = TRUE; n->reconnects = 0; n->error[0] = 0; return TRUE;
}
static void stream_stop_locked(RadioSAudioPatchbay *pb, Live *n)
{
    n->stream_wanted = FALSE; n->retry_at = 0; n->connected_us = 0;
    n->error[0] = 0;
    secure_free(n->password); n->password = NULL;
    retire(pb, n->stream); n->stream = NULL;
}
static Live *live_find(RadioSAudioPatchbay *pb, guint id)
{
    for (guint i = 0; i < pb->live_count; ++i) if (pb->live[i]->id == id) return pb->live[i];
    return NULL;
}
static void intents_apply(RadioSAudioPatchbay *pb)
{
    for (guint i = 0; i < pb->intents->len;) {
        Intent *intent = g_ptr_array_index(pb->intents, i); Live *n = live_find(pb, intent->id);
        if (!n) { ++i; continue; }
        GError *error = NULL;
        gboolean okay = intent->stream ? stream_start_locked(pb, n, intent->password, &error) : record_start_locked(pb, n, &error);
        if (!okay) g_strlcpy(n->error, error ? error->message : "Başlatılamadı.", sizeof n->error);
        g_clear_error(&error); g_ptr_array_remove_index(pb->intents, i);
    }
}
static void intent_free(gpointer data) { Intent *intent = data; secure_free(intent->password); g_free(intent); }

/* Worker: one 10 ms step over the plan in signal order (mutex held). */
static void pull(RadioSAudioPatchbay *pb, Live *n)
{
    guint64 available; gboolean *primed;
    if (n->capture) { available = queued(&n->capture->ring); primed = &n->capture->primed; }
    else if (n->tap) { available = radios_audio_tap_available(n->tap); primed = &n->tap_primed; }
    else return;
    if (!*primed) { if (available < PRIME) return; *primed = TRUE; }
    if (available > LIMIT) {
        if (n->capture) skip(&n->capture->ring, available - KEEP); else radios_audio_tap_skip(n->tap, (guint)(available - KEEP));
        ++pb->trimmed;
    }
    guint got = n->capture ? radios_audio_ring_read(&n->capture->ring, n->out, BLOCK, NULL) : radios_audio_tap_read(n->tap, n->out, BLOCK);
    if (got < BLOCK) { *primed = FALSE; ++pb->underruns; }
}
static void tick_locked(RadioSAudioPatchbay *pb)
{
    if (!pb->running) return;
    gboolean gate = pb->engine && radios_audio_engine_microphone_open(pb->engine);
    for (guint k = 0; k < pb->live_count; ++k) {
        Live *n = live_find(pb, pb->order[k]); if (!n) continue;
        RadioSRoutingGraphKind kind = n->config.kind;
        memset(n->in, 0, sizeof n->in); memset(n->out, 0, sizeof n->out);
        for (guint e = 0; e < pb->edge_count; ++e) {
            if (pb->edges[e].to != n->id) continue;
            Live *from = live_find(pb, pb->edges[e].from); if (!from) continue;
            for (guint i = 0; i < BLOCK * 2; ++i) n->in[i] += from->out[i];
        }
        if (kind == RG_METER) memcpy(n->out, n->in, sizeof n->out); else pull(pb, n);
        gboolean output = radios_routing_graph_has_output(kind);
        gfloat *signal = output ? n->out : n->in;
        gdouble gain = n->config.gain_db <= -60 ? 0 : pow(10, n->config.gain_db / 20);
        gdouble target = kind == RG_MICROPHONE && n->config.push_to_talk ? (gate ? 1 : 0) : 1;
        for (guint i = 0; i < BLOCK; ++i) {
            n->gate += CLAMP(target - n->gate, -1.0 / BLOCK, 1.0 / BLOCK);
            for (guint c = 0; c < 2; ++c) {
                gfloat value = (gfloat)(signal[i * 2 + c] * gain * n->gate);
                signal[i * 2 + c] = isfinite(value) ? CLAMP(value, -8, 8) : 0;
            }
        }
        if (n->sends) {
            if (n->line) (void)radios_audio_line_write(n->line, n->in, BLOCK);
            if (n->playback) (void)radios_audio_ring_write(&n->playback->ring, n->in, BLOCK, 0);
        }
        if (kind == RG_RECORDER && n->recording && n->recorder && !n->paused) {
            radios_audio_output_push_internal(n->recorder, n->in, BLOCK);
            n->segment_frames += BLOCK; n->total_frames += BLOCK;
            if (n->config.split_minutes && n->segment_frames >= (guint64)n->config.split_minutes * 60u * RATE) {
                GError *error = NULL;
                if (record_open(pb, n, &error)) ++n->segment;
                else g_strlcpy(n->error, error ? error->message : "Yeni kayıt dosyası açılamadı.", sizeof n->error);
                g_clear_error(&error);
            }
        }
        if (kind == RG_STREAM && n->stream_wanted && n->stream) radios_audio_output_push_internal(n->stream, n->in, BLOCK);
        const gfloat *measured = output && (n->feeds || n->capture || n->tap || kind == RG_METER) ? n->out : n->in;
        for (guint i = 0; i < BLOCK; ++i) for (guint c = 0; c < 2; ++c) {
            gfloat value = fabsf(measured[i * 2 + c]);
            n->peak[c] = MAX(n->peak[c], value); n->squares[c] += (gdouble)value * value;
        }
        n->measured += BLOCK;
    }
}
static gpointer worker(gpointer data)
{
    RadioSAudioPatchbay *pb = data; gint64 next = g_get_monotonic_time();
    g_mutex_lock(&pb->mutex);
    while (!pb->closing) {
        gint64 now = g_get_monotonic_time();
        if (now < next) { g_cond_wait_until(&pb->tick, &pb->mutex, next); continue; }
        if (now - next > 100000) next = now;
        next += STEP_US; tick_locked(pb);
    }
    g_mutex_unlock(&pb->mutex); return NULL;
}

/* Control thread: makes the plan match the requested graph. Resources of a
 * block are kept while its device, channel and Pulse connection match. */
static void live_close(RadioSAudioPatchbay *pb, Live *n)
{
    if (!n) return;
    link_close(pb, n->capture); link_close(pb, n->playback);
    if (n->tap) radios_audio_tap_close(n->tap);
    if (n->line) radios_audio_line_close(n->line);
    if (n->module != PA_INVALID_INDEX || n->mic_module != PA_INVALID_INDEX) {
        pa_threaded_mainloop_lock(pb->loop);
        if (n->module_epoch == pb->epoch) { module_unload(pb, n->mic_module); module_unload(pb, n->module); }
        pa_threaded_mainloop_unlock(pb->loop);
    }
    if (n->recorder) { radios_audio_output_stop(n->recorder); radios_audio_output_unref(n->recorder); }
    if (n->stream) { radios_audio_output_stop(n->stream); radios_audio_output_unref(n->stream); }
    secure_free(n->password); g_free(n);
}
static gboolean link_reusable(Link *link, const gchar *device, guint64 epoch)
{ return link && link->epoch == epoch && !g_atomic_int_get(&link->failed) && g_str_equal(link->device, device); }
static void reconcile(RadioSAudioPatchbay *pb)
{
    g_mutex_lock(&pb->mutex);
    RadioSRoutingGraph *graph = pb->desired ? g_memdup2(pb->desired, sizeof *pb->desired) : g_new0(RadioSRoutingGraph, 1);
    gboolean running = pb->desired_running; guint64 serial = pb->desired_serial; pb->busy = TRUE;
    RadioSAudioEngine *engine = pb->engine ? radios_audio_engine_ref(pb->engine) : NULL;
    g_mutex_unlock(&pb->mutex);
    guint order[RADIOS_ROUTING_GRAPH_MAX_NODES] = {0};
    if (!radios_routing_graph_order(graph, order)) { memset(graph, 0, sizeof *graph); running = FALSE; }
    Live *fresh[RADIOS_ROUTING_GRAPH_MAX_NODES] = {0}, *previous[RADIOS_ROUTING_GRAPH_MAX_NODES] = {0};
    pa_threaded_mainloop_lock(pb->loop);
    gboolean ready = pulse_ready(pb); guint64 epoch = pb->epoch;
    for (guint i = 0; i < graph->node_count; ++i) {
        const RadioSRoutingGraphNode *c = &graph->nodes[i];
        Live *old = NULL;
        for (guint j = 0; j < pb->live_count; ++j)
            if (pb->live[j]->id == c->id && pb->live[j]->config.kind == c->kind) old = pb->live[j];
        Live *n = g_new0(Live, 1); n->id = c->id; n->config = *c; n->module = n->mic_module = PA_INVALID_INDEX;
        n->tap_source = n->line_target = G_MININT; n->gate = c->kind == RG_MICROPHONE && c->push_to_talk ? 0 : 1;
        for (guint e = 0; e < graph->edge_count; ++e) {
            n->sends |= graph->edges[e].to == c->id; n->feeds |= graph->edges[e].from == c->id;
        }
        previous[i] = old; fresh[i] = n;
        gchar sink[64]; virtual_sink(c->id, sink, sizeof sink);
        gchar error[256] = "";
        /* Virtual interface: exists while its block exists. */
        if (c->kind == RG_VIRTUAL) {
            if (old && old->module != PA_INVALID_INDEX && old->module_epoch == epoch && g_str_equal(old->module_name, c->name) &&
                old->module_mic == c->as_microphone) {
                n->module = old->module; n->mic_module = old->mic_module; n->module_epoch = epoch;
                g_strlcpy(n->module_name, c->name, sizeof n->module_name); n->module_mic = c->as_microphone;
            } else if (ready) {
                if (old && old->module != PA_INVALID_INDEX && old->module_epoch == epoch) {
                    module_unload(pb, old->mic_module); module_unload(pb, old->module); old->module = old->mic_module = PA_INVALID_INDEX;
                }
                gchar *label = pulse_quoted(radios_routing_graph_node_title(c));
                gchar *arguments = g_strdup_printf("sink_name=%s rate=48000 channels=2 sink_properties='device.description=\"RadioS · %s\"'", sink, label);
                if (module_load(pb, "module-null-sink", arguments, &n->module)) {
                    n->module_epoch = epoch; g_strlcpy(n->module_name, c->name, sizeof n->module_name); n->module_mic = c->as_microphone;
                    if (c->as_microphone) {
                        gchar *mic = g_strdup_printf("master=%s.monitor source_name=%s_mic source_properties='device.description=\"RadioS · %s (mikrofon)\"'",
                            sink, sink, label);
                        if (!module_load(pb, "module-remap-source", mic, &n->mic_module))
                            g_strlcpy(error, "Sanal mikrofon oluşturulamadı.", sizeof error);
                        g_free(mic);
                    }
                } else g_strlcpy(error, "Sanal ses arayüzü oluşturulamadı (module-null-sink).", sizeof error);
                g_free(arguments); g_free(label);
            } else g_strlcpy(error, "Ses sunucusuna bağlanılıyor…", sizeof error);
        }
        if (running) {
            const gchar *capture = NULL, *playback = NULL; gchar monitor[80];
            if (c->kind == RG_INPUT || (c->kind == RG_MICROPHONE && g_strcmp0(c->device, RADIOS_ROUTING_TALK_MICROPHONE))) {
                if (c->device[0]) capture = c->device; else g_strlcpy(error, "Bir giriş aygıtı seçin.", sizeof error);
            }
            if (c->kind == RG_VIRTUAL && n->module != PA_INVALID_INDEX) {
                g_snprintf(monitor, sizeof monitor, "%s.monitor", sink); capture = monitor;
                if (n->sends) playback = sink;
            }
            if (c->kind == RG_OUTPUT && n->sends) {
                if (c->device[0]) playback = c->device; else g_strlcpy(error, "Bir çıkış aygıtı seçin.", sizeof error);
            }
            gchar title[256]; g_snprintf(title, sizeof title, "RadioS · %s", radios_routing_graph_node_title(c));
            if (capture) {
                if (old && link_reusable(old->capture, capture, epoch)) { n->capture = old->capture; }
                else if (!(n->capture = link_open(pb, capture, FALSE, title, error)) && !error[0]) g_strlcpy(error, "Giriş açılamadı.", sizeof error);
            }
            if (playback) {
                if (old && link_reusable(old->playback, playback, epoch)) { n->playback = old->playback; }
                else if (!(n->playback = link_open(pb, playback, TRUE, title, error)) && !error[0]) g_strlcpy(error, "Çıkış açılamadı.", sizeof error);
            }
            gint tap = c->kind == RG_MAIN ? RADIOS_AUDIO_TAP_MAIN : c->kind == RG_MIXER ? (gint)c->channel - 1 :
                c->kind == RG_MICROPHONE && !g_strcmp0(c->device, RADIOS_ROUTING_TALK_MICROPHONE) ? RADIOS_AUDIO_TAP_MICROPHONE : G_MININT;
            gint line = (c->kind == RG_MAIN || c->kind == RG_MIXER) && n->sends ? (c->kind == RG_MAIN ? -1 : (gint)c->channel - 1) : G_MININT;
            if ((tap != G_MININT || line != G_MININT) && !engine) g_strlcpy(error, "Yayın motoru hazır değil.", sizeof error);
            if (tap != G_MININT && engine) {
                if (old && old->tap && old->tap_source == tap) { n->tap = old->tap; n->tap_primed = old->tap_primed; }
                else {
                    GError *failure = NULL; n->tap = radios_audio_engine_tap_open(engine, tap, &failure);
                    if (!n->tap) g_strlcpy(error, failure ? failure->message : "Mikser dinlenemedi.", sizeof error);
                    g_clear_error(&failure);
                }
                n->tap_source = tap;
            }
            if (line != G_MININT && engine) {
                if (old && old->line && old->line_target == line) n->line = old->line;
                else {
                    GError *failure = NULL; n->line = radios_audio_engine_line_open(engine, line, &failure);
                    if (!n->line) g_strlcpy(error, failure ? failure->message : "Miksere gönderilemedi.", sizeof error);
                    g_clear_error(&failure);
                }
                n->line_target = line;
            }
        }
        g_strlcpy(n->error, error, sizeof n->error);
    }
    pa_threaded_mainloop_unlock(pb->loop);
    /* Swap: runtime state follows its block; reused resources leave the old. */
    Live *retired[RADIOS_ROUTING_GRAPH_MAX_NODES] = {0}; guint retired_count = 0;
    g_mutex_lock(&pb->mutex);
    for (guint i = 0; i < graph->node_count; ++i) {
        Live *n = fresh[i], *old = previous[i]; if (!old) continue;
        n->recorder = old->recorder; n->recording = old->recording; n->paused = old->paused; n->segment = old->segment;
        n->segment_frames = old->segment_frames; n->total_frames = old->total_frames; g_strlcpy(n->path, old->path, sizeof n->path);
        n->stream = old->stream; n->stream_wanted = old->stream_wanted; n->password = old->password; n->retry_at = old->retry_at;
        n->connected_us = old->connected_us; n->reconnects = old->reconnects; n->gate = old->gate;
        memcpy(n->peak, old->peak, sizeof n->peak); memcpy(n->squares, old->squares, sizeof n->squares); n->measured = old->measured;
        old->recorder = old->stream = NULL; old->password = NULL;
        if (old->capture == n->capture) old->capture = NULL;
        if (old->playback == n->playback) old->playback = NULL;
        if (old->tap == n->tap) old->tap = NULL;
        if (old->line == n->line) old->line = NULL;
        if (old->module == n->module) old->module = old->mic_module = PA_INVALID_INDEX;
    }
    for (guint j = 0; j < pb->live_count; ++j) retired[retired_count++] = pb->live[j];
    for (guint i = 0; i < graph->node_count; ++i) pb->live[i] = fresh[i];
    pb->live_count = graph->node_count; memcpy(pb->order, order, sizeof pb->order);
    memcpy(pb->edges, graph->edges, sizeof pb->edges); pb->edge_count = graph->edge_count;
    pb->running = running;
    if (!running) for (guint i = 0; i < pb->live_count; ++i) { record_stop_locked(pb, pb->live[i]); if (pb->live[i]->stream_wanted) stream_stop_locked(pb, pb->live[i]); }
    intents_apply(pb);
    pb->applied_serial = serial; pb->busy = pb->desired_serial != serial;
    g_mutex_unlock(&pb->mutex);
    for (guint i = 0; i < retired_count; ++i) live_close(pb, retired[i]);
    if (engine) radios_audio_engine_unref(engine);
    g_free(graph);
}
static void housekeeping(RadioSAudioPatchbay *pb)
{
    gint64 now = g_get_monotonic_time(); gboolean resync = FALSE;
    g_mutex_lock(&pb->mutex);
    for (guint i = 0; i < pb->live_count; ++i) {
        Live *n = pb->live[i];
        Link *links[] = {n->capture, n->playback};
        for (guint l = 0; l < 2; ++l) if (links[l] && g_atomic_int_get(&links[l]->failed)) {
            g_snprintf(n->error, sizeof n->error, "Aygıt bağlantısı koptu: %s", links[l]->device); resync = TRUE;
        }
        if (pb->running && n->error[0] && !n->stream_wanted && n->config.kind != RG_RECORDER) resync = TRUE;
        if (!n->stream_wanted || !n->stream) continue;
        RadioSAudioOutputSnapshot s; radios_audio_output_snapshot(n->stream, &s);
        if (s.state == RADIOS_AUDIO_OUTPUT_FAILED || s.state == RADIOS_AUDIO_OUTPUT_STOPPED) {
            if (!n->retry_at) { n->retry_at = now + RETRY_US; n->connected_us = 0; g_strlcpy(n->error, s.error, sizeof n->error); }
            else if (now >= n->retry_at) {
                GError *error = NULL;
                if (stream_connect(n, &error)) ++n->reconnects;
                else { n->retry_at = now + RETRY_US; g_strlcpy(n->error, error ? error->message : "Yeniden bağlanılamadı.", sizeof n->error); }
                g_clear_error(&error);
            }
        } else if (s.state == RADIOS_AUDIO_OUTPUT_ACTIVE && n->error[0]) n->error[0] = 0;
    }
    if (resync && now >= pb->resync_at) { pb->resync_at = now + RESYNC_US; pb->resync = TRUE; }
    g_mutex_unlock(&pb->mutex);
}
static gpointer control(gpointer data)
{
    RadioSAudioPatchbay *pb = data; gint64 tidy = 0;
    for (;;) {
        g_mutex_lock(&pb->mutex);
        gint64 deadline = g_get_monotonic_time() + 500000;
        while (!pb->closing && pb->desired_serial == pb->applied_serial && !pb->refresh_requested && !pb->retired->len && !pb->resync &&
               g_get_monotonic_time() < deadline)
            g_cond_wait_until(&pb->control_wake, &pb->mutex, deadline);
        gboolean closing = pb->closing, refresh = pb->refresh_requested;
        gboolean apply = pb->desired_serial != pb->applied_serial || pb->resync;
        pb->refresh_requested = pb->resync = FALSE;
        GPtrArray *retired = pb->retired; pb->retired = g_ptr_array_new();
        g_mutex_unlock(&pb->mutex);
        for (guint i = 0; i < retired->len; ++i) radios_audio_output_unref(g_ptr_array_index(retired, i));
        g_ptr_array_free(retired, TRUE);
        if (closing) break;
        gboolean fresh = connection(pb);
        if (refresh) { pa_threaded_mainloop_lock(pb->loop); inventory(pb); pa_threaded_mainloop_unlock(pb->loop); }
        if (apply || fresh) reconcile(pb);
        if (g_get_monotonic_time() >= tidy) { housekeeping(pb); tidy = g_get_monotonic_time() + 250000; }
    }
    return NULL;
}

RadioSAudioPatchbay *radios_audio_patchbay_new(const gchar *pulse_server)
{
    RadioSAudioPatchbay *pb = g_new0(RadioSAudioPatchbay, 1); g_atomic_ref_count_init(&pb->refs);
    g_mutex_init(&pb->mutex); g_cond_init(&pb->tick); g_cond_init(&pb->control_wake);
    pb->server = g_strdup(pulse_server && *pulse_server ? pulse_server : NULL);
    pb->intents = g_ptr_array_new_with_free_func(intent_free); pb->retired = g_ptr_array_new();
    pb->loop = pa_threaded_mainloop_new();
    if (pb->loop && pa_threaded_mainloop_start(pb->loop) < 0) { pa_threaded_mainloop_free(pb->loop); pb->loop = NULL; }
    if (!pb->loop) g_strlcpy(pb->error, "Ses sunucusu döngüsü başlatılamadı", sizeof pb->error);
    pb->worker = g_thread_new("radios-patchbay", worker, pb);
    if (pb->loop) pb->control = g_thread_new("radios-patchbay-control", control, pb);
    return pb;
}
RadioSAudioPatchbay *radios_audio_patchbay_ref(RadioSAudioPatchbay *patchbay)
{ g_return_val_if_fail(patchbay, NULL); g_atomic_ref_count_inc(&patchbay->refs); return patchbay; }
void radios_audio_patchbay_unref(RadioSAudioPatchbay *pb)
{
    if (!pb || !g_atomic_ref_count_dec(&pb->refs)) return;
    g_mutex_lock(&pb->mutex); pb->closing = TRUE; g_cond_broadcast(&pb->tick); g_cond_broadcast(&pb->control_wake); g_mutex_unlock(&pb->mutex);
    if (pb->worker) g_thread_join(pb->worker);
    if (pb->control) g_thread_join(pb->control);
    for (guint i = 0; i < pb->live_count; ++i) {
        record_stop_locked(pb, pb->live[i]);
        if (pb->loop) live_close(pb, pb->live[i]); else g_free(pb->live[i]);
    }
    for (guint i = 0; i < pb->retired->len; ++i) radios_audio_output_unref(g_ptr_array_index(pb->retired, i));
    g_ptr_array_free(pb->retired, TRUE); g_ptr_array_free(pb->intents, TRUE);
    if (pb->loop) {
        pa_threaded_mainloop_lock(pb->loop);
        if (pb->context) { pa_context_set_state_callback(pb->context, NULL, NULL); pa_context_set_subscribe_callback(pb->context, NULL, NULL);
                           pa_context_disconnect(pb->context); pa_context_unref(pb->context); }
        pa_threaded_mainloop_unlock(pb->loop); pa_threaded_mainloop_stop(pb->loop); pa_threaded_mainloop_free(pb->loop);
    }
    if (pb->engine) radios_audio_engine_unref(pb->engine);
    g_free(pb->desired); g_free(pb->recording_root); g_free(pb->server);
    g_cond_clear(&pb->tick); g_cond_clear(&pb->control_wake); g_mutex_clear(&pb->mutex); g_free(pb);
}
void radios_audio_patchbay_set_engine(RadioSAudioPatchbay *pb, RadioSAudioEngine *engine)
{
    g_return_if_fail(pb);
    g_mutex_lock(&pb->mutex);
    if (pb->engine != engine) {
        RadioSAudioEngine *old = pb->engine; pb->engine = engine ? radios_audio_engine_ref(engine) : NULL;
        ++pb->desired_serial; g_cond_signal(&pb->control_wake);
        g_mutex_unlock(&pb->mutex); if (old) radios_audio_engine_unref(old); return;
    }
    g_mutex_unlock(&pb->mutex);
}
void radios_audio_patchbay_set_recording_root(RadioSAudioPatchbay *pb, const gchar *folder)
{
    g_return_if_fail(pb);
    g_mutex_lock(&pb->mutex); g_free(pb->recording_root); pb->recording_root = g_strdup(folder); g_mutex_unlock(&pb->mutex);
}
void radios_audio_patchbay_refresh(RadioSAudioPatchbay *pb)
{
    g_return_if_fail(pb);
    g_mutex_lock(&pb->mutex); pb->refresh_requested = TRUE; ++pb->desired_serial; g_cond_signal(&pb->control_wake); g_mutex_unlock(&pb->mutex);
}
gboolean radios_audio_patchbay_apply(RadioSAudioPatchbay *pb, const RadioSRoutingGraph *graph, gboolean running, GError **error)
{
    if (!pb || !graph) return fail(error, "Yönlendirme bulunamadı.");
    if (!radios_routing_graph_validate(graph, error)) return FALSE;
    g_mutex_lock(&pb->mutex);
    if (!pb->desired) pb->desired = g_new0(RadioSRoutingGraph, 1);
    *pb->desired = *graph; pb->desired_running = running; ++pb->desired_serial; pb->busy = TRUE;
    g_cond_signal(&pb->control_wake); g_mutex_unlock(&pb->mutex);
    return TRUE;
}
static gboolean desired_has(RadioSAudioPatchbay *pb, guint node, RadioSRoutingGraphKind kind)
{
    const RadioSRoutingGraphNode *n = pb->desired ? radios_routing_graph_find_const(pb->desired, node) : NULL;
    return n && n->kind == kind && pb->desired_running;
}
gboolean radios_audio_patchbay_record(RadioSAudioPatchbay *pb, guint node, GError **error)
{
    if (!pb) return fail(error, "Yönlendirme hazır değil.");
    g_mutex_lock(&pb->mutex);
    Live *n = live_find(pb, node); gboolean okay;
    if (n && pb->running && pb->applied_serial == pb->desired_serial) okay = record_start_locked(pb, n, error);
    else if (desired_has(pb, node, RG_RECORDER)) {
        Intent *intent = g_new0(Intent, 1); intent->id = node; g_ptr_array_add(pb->intents, intent); okay = TRUE;
    } else okay = fail(error, "Kaydedici henüz hazır değil; yönlendirmeyi başlatın.");
    g_mutex_unlock(&pb->mutex); return okay;
}
gboolean radios_audio_patchbay_record_pause(RadioSAudioPatchbay *pb, guint node, gboolean paused, GError **error)
{
    if (!pb) return fail(error, "Yönlendirme hazır değil.");
    g_mutex_lock(&pb->mutex); Live *n = live_find(pb, node);
    gboolean okay = n && n->recording;
    if (okay) n->paused = paused;
    g_mutex_unlock(&pb->mutex);
    return okay || fail(error, "Kayıt sürmüyor.");
}
gboolean radios_audio_patchbay_record_split(RadioSAudioPatchbay *pb, guint node, GError **error)
{
    if (!pb) return fail(error, "Yönlendirme hazır değil.");
    g_mutex_lock(&pb->mutex); Live *n = live_find(pb, node);
    gboolean okay = n && n->recording && record_open(pb, n, error);
    if (okay) ++n->segment;
    else if (!(n && n->recording)) { g_mutex_unlock(&pb->mutex); return fail(error, "Kayıt sürmüyor."); }
    g_mutex_unlock(&pb->mutex); return okay;
}
void radios_audio_patchbay_record_stop(RadioSAudioPatchbay *pb, guint node)
{
    if (!pb) return;
    g_mutex_lock(&pb->mutex); Live *n = live_find(pb, node);
    if (n) record_stop_locked(pb, n);
    for (guint i = 0; i < pb->intents->len; ++i) {
        Intent *intent = g_ptr_array_index(pb->intents, i);
        if (intent->id == node && !intent->stream) { g_ptr_array_remove_index(pb->intents, i); break; }
    }
    g_mutex_unlock(&pb->mutex);
}
gboolean radios_audio_patchbay_stream_start(RadioSAudioPatchbay *pb, guint node, const gchar *password, GError **error)
{
    if (!pb) return fail(error, "Yönlendirme hazır değil.");
    g_mutex_lock(&pb->mutex);
    Live *n = live_find(pb, node); gboolean okay;
    if (n && pb->running && pb->applied_serial == pb->desired_serial) okay = stream_start_locked(pb, n, password, error);
    else if (desired_has(pb, node, RG_STREAM) && password && *password) {
        Intent *intent = g_new0(Intent, 1); intent->id = node; intent->stream = TRUE; intent->password = g_strdup(password);
        g_ptr_array_add(pb->intents, intent); okay = TRUE;
    } else okay = fail(error, "Yayın bloğu henüz hazır değil; yönlendirmeyi başlatın ve şifreyi girin.");
    g_mutex_unlock(&pb->mutex); return okay;
}
void radios_audio_patchbay_stream_stop(RadioSAudioPatchbay *pb, guint node)
{
    if (!pb) return;
    g_mutex_lock(&pb->mutex); Live *n = live_find(pb, node);
    if (n) stream_stop_locked(pb, n);
    for (guint i = pb->intents->len; i > 0; --i) {
        Intent *intent = g_ptr_array_index(pb->intents, i - 1);
        if (intent->id == node && intent->stream) g_ptr_array_remove_index(pb->intents, i - 1);
    }
    g_mutex_unlock(&pb->mutex);
}
gboolean radios_audio_patchbay_stream_metadata(RadioSAudioPatchbay *pb, guint node, const gchar *title, GError **error)
{
    if (!pb) return fail(error, "Yönlendirme hazır değil.");
    g_mutex_lock(&pb->mutex); Live *n = live_find(pb, node);
    gboolean okay = n && n->stream_wanted && n->stream ? radios_audio_output_metadata(n->stream, title, error) :
        fail(error, "Yayın bağlı değil.");
    g_mutex_unlock(&pb->mutex); return okay;
}
gboolean radios_audio_patchbay_stream_metadata_status(RadioSAudioPatchbay *pb, guint node, RadioSAudioOutputSnapshot *out)
{
    if (!out) return FALSE;
    memset(out, 0, sizeof *out);
    if (!pb) return FALSE;
    g_mutex_lock(&pb->mutex); Live *n = live_find(pb, node);
    RadioSAudioOutput *stream = n && n->config.kind == RG_STREAM && n->stream ?
        radios_audio_output_ref(n->stream) : NULL;
    g_mutex_unlock(&pb->mutex);
    if (!stream) return FALSE;
    radios_audio_output_snapshot(stream, out); radios_audio_output_unref(stream);
    return TRUE;
}
gboolean radios_audio_patchbay_move_application(RadioSAudioPatchbay *pb, guint32 application, guint node, GError **error)
{
    if (!pb || !pb->loop) return fail(error, "Ses sunucusu hazır değil.");
    g_mutex_lock(&pb->mutex); Live *n = live_find(pb, node);
    gboolean present = n && n->config.kind == RG_VIRTUAL && n->module != PA_INVALID_INDEX;
    g_mutex_unlock(&pb->mutex);
    if (!present) return fail(error, "Sanal ses arayüzü henüz oluşturulmadı.");
    gchar sink[64]; virtual_sink(node, sink, sizeof sink); Query query = {0};
    pa_threaded_mainloop_lock(pb->loop);
    gboolean okay = pulse_ready(pb) &&
        wait_query(pb, pa_context_move_sink_input_by_name(pb->context, application, sink, query_success, &query), &query);
    pa_threaded_mainloop_unlock(pb->loop);
    if (okay) radios_audio_patchbay_refresh(pb);
    return okay || fail(error, "Uygulamanın sesi sanal arayüze taşınamadı.");
}
void radios_audio_patchbay_snapshot(RadioSAudioPatchbay *pb, RadioSAudioPatchbaySnapshot *snapshot)
{
    g_return_if_fail(pb && snapshot); memset(snapshot, 0, sizeof *snapshot);
    g_mutex_lock(&pb->mutex);
    snapshot->connected = pb->connected; snapshot->running = pb->running && pb->desired_running;
    snapshot->busy = pb->busy || pb->desired_serial != pb->applied_serial;
    g_strlcpy(snapshot->error, pb->connected ? "" : pb->error, sizeof snapshot->error);
    snapshot->output_count = pb->output_count; snapshot->input_count = pb->input_count; snapshot->application_count = pb->app_count;
    memcpy(snapshot->outputs, pb->outputs, sizeof snapshot->outputs); memcpy(snapshot->inputs, pb->inputs, sizeof snapshot->inputs);
    memcpy(snapshot->applications, pb->apps, sizeof snapshot->applications);
    snapshot->underruns = pb->underruns; snapshot->trimmed = pb->trimmed;
    for (guint i = 0; i < pb->live_count; ++i) {
        Live *n = pb->live[i]; RadioSPatchNodeSnapshot *out = &snapshot->nodes[snapshot->node_count++];
        out->id = n->id; out->kind = n->config.kind; g_strlcpy(out->error, n->error, sizeof out->error);
        for (guint c = 0; c < 2; ++c) {
            out->peak[c] = n->peak[c]; out->rms[c] = n->measured ? (gfloat)sqrt(n->squares[c] / n->measured) : 0;
            n->peak[c] = 0; n->squares[c] = 0;
        }
        n->measured = 0;
        if (n->config.kind == RG_VIRTUAL && n->module != PA_INVALID_INDEX) {
            virtual_sink(n->id, out->sink, sizeof out->sink);
            if (n->config.as_microphone && n->mic_module != PA_INVALID_INDEX) g_snprintf(out->source, sizeof out->source, "%s_mic", out->sink);
            else g_snprintf(out->source, sizeof out->source, "%s.monitor", out->sink);
        }
        gboolean live = (!n->capture || g_atomic_int_get(&n->capture->ready)) && (!n->playback || g_atomic_int_get(&n->playback->ready));
        gboolean resources = n->capture || n->playback || n->tap || n->line;
        out->state = n->error[0] ? RADIOS_PATCH_FAILED : !pb->running ?
            (n->config.kind == RG_VIRTUAL && n->module != PA_INVALID_INDEX ? RADIOS_PATCH_ACTIVE : RADIOS_PATCH_IDLE) :
            resources ? (live ? RADIOS_PATCH_ACTIVE : RADIOS_PATCH_STARTING) :
            n->config.kind == RG_METER || n->config.kind == RG_RECORDER || n->config.kind == RG_STREAM || n->config.kind == RG_VIRTUAL ?
                RADIOS_PATCH_ACTIVE : RADIOS_PATCH_IDLE;
        out->recording = n->recording; out->paused = n->paused; out->segment = n->segment;
        out->total_frames = n->total_frames; g_strlcpy(out->path, n->path, sizeof out->path);
        if (n->recorder) {
            RadioSAudioOutputSnapshot s; radios_audio_output_snapshot(n->recorder, &s);
            out->frames = s.frames_processed; out->bytes = s.bytes_written ? s.bytes_written + 44 : 0;
            if (s.state == RADIOS_AUDIO_OUTPUT_FAILED) {
                g_strlcpy(out->error, s.error[0] ? s.error : "Kayıt dosyası yazılamadı.", sizeof out->error); out->state = RADIOS_PATCH_FAILED;
            }
        }
        out->streaming = n->stream_wanted; out->reconnects = n->reconnects;
        if (n->stream) {
            RadioSAudioOutputSnapshot s; radios_audio_output_snapshot(n->stream, &s);
            out->output_state = s.state; out->http_status = s.http_status; out->sent_bytes = s.bytes_written;
            out->metadata_state = s.metadata_state;
            out->connected_us = n->stream_wanted && s.state == RADIOS_AUDIO_OUTPUT_ACTIVE ? n->connected_us : 0;
            if (n->stream_wanted && s.state == RADIOS_AUDIO_OUTPUT_FAILED && s.error[0]) {
                g_strlcpy(out->error, s.error, sizeof out->error); out->state = RADIOS_PATCH_FAILED;
            }
        }
    }
    g_mutex_unlock(&pb->mutex);
}
