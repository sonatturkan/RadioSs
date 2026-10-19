/* MIDI 1 input on Windows: WinMM (midiIn*) input devices read through a
 * CALLBACK_FUNCTION callback, hotplug from the configuration manager's device
 * interface notifications plus a periodic WinMM device list check.
 *
 * Threads. The WinMM callback (a driver thread) only copies channel messages,
 * returned SysEx buffers and a driver's own close into a ring and wakes the
 * input's worker; it calls no multimedia function (WinMM can deadlock) and
 * allocates nothing. The notification callback (a thread-pool thread) only
 * records removed interface paths and arrivals. The worker decodes, hands
 * SysEx buffers back to the driver, follows hotplug and liveness, and queues
 * every delivery on the owner's main context.
 *
 * Identity. A device is persisted by RadioSMidiPort.name: its WinMM product
 * name, exact duplicates numbered " #2", " #3"... in device interface path
 * order (the path of a USB device without a serial number encodes its socket,
 * so the numbers stay with the sockets across replug and reboot).
 * RadioSMidiPort.id, "<WinMM index>:<fingerprint of name and path>", is only
 * a handle into one listing; it is checked again at open, so an index shift
 * between listing and opening never opens another device. Device names saved
 * on Linux (ALSA "client:port" addresses were never saved; ALSA names such as
 * "nanoKONTROL2 MIDI 1" were) match no WinMM name: such a configuration
 * behaves exactly like a device that is not plugged in until the operator
 * selects the device again. */
#include "radios/midi.h"
#include <windows.h>
#include <mmsystem.h>
#include <cfgmgr32.h>
#include <string.h>

#ifndef DRV_QUERYDEVICEINTERFACE
#define DRV_QUERYDEVICEINTERFACE (DRV_RESERVED + 12)
#define DRV_QUERYDEVICEINTERFACESIZE (DRV_RESERVED + 13)
#endif

/* The worker checks this often that its device is still listed and its
 * handle still answers, also where device notifications cannot be had. */
#define LIVENESS_MS 1000
/* After a device notification the list is checked every BURST_STEP_MS for
 * BURST_MS: WinMM's device list follows Plug and Play with a delay. */
#define BURST_MS 3000
#define BURST_STEP_MS 250
#define RING_SIZE 1024u          /* a power of two */
#define DRAIN_CHUNK 64u
#define SYSEX_BUFFERS 4u
#define SYSEX_BYTES 1024u
#define REMOVED_MAX 256u
#define PATH_BYTES_MAX 4096u
#define DEVICES_MAX 9999u        /* the index has at most four digits in a port id */

/* Every WinMM and configuration manager call goes through this table.
 * tests/test_midi_hotplug.c defines RADIOS_MIDI_WIN_API() to a fake system
 * before including this file: the test then imports no midiIn* or CM_*
 * function and never touches a real device. */
typedef struct RadioSMidiWinApi {
    UINT (WINAPI *in_get_num_devs)(void);
    MMRESULT (WINAPI *in_get_dev_caps)(UINT_PTR device, LPMIDIINCAPSW caps, UINT size);
    MMRESULT (WINAPI *in_open)(LPHMIDIIN handle, UINT device, DWORD_PTR callback, DWORD_PTR instance, DWORD flags);
    MMRESULT (WINAPI *in_start)(HMIDIIN handle);
    MMRESULT (WINAPI *in_stop)(HMIDIIN handle);
    MMRESULT (WINAPI *in_reset)(HMIDIIN handle);
    MMRESULT (WINAPI *in_close)(HMIDIIN handle);
    MMRESULT (WINAPI *in_prepare_header)(HMIDIIN handle, LPMIDIHDR header, UINT size);
    MMRESULT (WINAPI *in_unprepare_header)(HMIDIIN handle, LPMIDIHDR header, UINT size);
    MMRESULT (WINAPI *in_add_buffer)(HMIDIIN handle, LPMIDIHDR header, UINT size);
    MMRESULT (WINAPI *in_message)(HMIDIIN handle, UINT message, DWORD_PTR param1, DWORD_PTR param2);
    MMRESULT (WINAPI *in_get_id)(HMIDIIN handle, LPUINT device);
    MMRESULT (WINAPI *in_get_error_text)(MMRESULT code, LPWSTR text, UINT size);
    CONFIGRET (WINAPI *register_notification)(PCM_NOTIFY_FILTER filter, PVOID context, PCM_NOTIFY_CALLBACK callback,
        PHCMNOTIFICATION notification);
    CONFIGRET (WINAPI *unregister_notification)(HCMNOTIFICATION notification);
} RadioSMidiWinApi;
#ifndef RADIOS_MIDI_WIN_API
static const RadioSMidiWinApi *midi_win_api(void)
{
    static RadioSMidiWinApi api; static gsize ready;
    /* Filled at run time: addresses of imported functions are no constant initializers. */
    if (g_once_init_enter(&ready)) {
        api.in_get_num_devs = midiInGetNumDevs; api.in_get_dev_caps = midiInGetDevCapsW; api.in_open = midiInOpen;
        api.in_start = midiInStart; api.in_stop = midiInStop; api.in_reset = midiInReset; api.in_close = midiInClose;
        api.in_prepare_header = midiInPrepareHeader; api.in_unprepare_header = midiInUnprepareHeader;
        api.in_add_buffer = midiInAddBuffer; api.in_message = midiInMessage; api.in_get_id = midiInGetID;
        api.in_get_error_text = midiInGetErrorTextW;
        api.register_notification = CM_Register_Notification; api.unregister_notification = CM_Unregister_Notification;
        g_once_init_leave(&ready, 1);
    }
    return &api;
}
#define RADIOS_MIDI_WIN_API() midi_win_api()
#endif

/* Queued deliveries outlive a closed input; they share this flag instead. */
typedef struct {
    gint refs, stopped;
    RadioSMidiCallback callback;
    RadioSMidiNoticeCallback notice;
    gpointer user;
} Listener;
typedef enum { RING_SHORT, RING_SYSEX, RING_CLOSED } RingKind;
typedef struct { guint8 kind; guint32 message; MIDIHDR *header; } RingItem;
/* Who frees an input: close, unless it returned while the worker was still
 * inside a WinMM call; then the worker does when it gets out. */
enum { WORKER_RUNNING, WORKER_EXITED, WORKER_ORPHANED };
struct RadioSMidiInput {
    Listener *listener;
    GMainContext *context;
    gint alive;                   /* atomic: the device still feeds this input */
    gchar port_id[24];
    const RadioSMidiWinApi *api;
    GThread *thread;
    gint worker;                  /* atomic: WORKER_* */
    GMutex handle_lock;           /* handle and sysex_prepared: the worker (or the opener) and close */
    HMIDIIN handle;               /* the open device, NULL once released */
    gpointer closed_by_us;        /* atomic: the handle we close ourselves; its MIM_CLOSE is ours */
    gint driver_closed;           /* atomic: the driver closed the handle on its own */
    gchar *raw, *name, *iface;    /* the device at open: WinMM name, listed name, casefolded interface path or NULL */
    HANDLE stop, data, hotplug;   /* auto-reset events: close, ring, device notifications */
    CRITICAL_SECTION lock;        /* the ring: WinMM callback and worker */
    RingItem ring[RING_SIZE];
    guint head, tail;
    gint dropped;                 /* atomic: messages lost to a full ring */
    GMutex hotplug_lock;          /* removed: notification callback and worker */
    GPtrArray *removed;           /* casefolded paths of removed device interfaces */
    gint arrived;                 /* atomic: an interface arrived, or a removal could not be recorded */
    HCMNOTIFICATION notification;
    /* SysEx maps to no control; queued buffers let a driver hand it over and go on. */
    MIDIHDR sysex[SYSEX_BUFFERS];
    gchar sysex_data[SYSEX_BUFFERS][SYSEX_BYTES];
    guint sysex_prepared;
    GHashTable *known;            /* worker: key -> casefolded interface path (or NULL) of each listed device */
    guint misses;                 /* worker: consecutive listings without our device */
};
typedef struct { Listener *listener; gboolean notice; RadioSMidiNotice what; RadioSMidiEvent event; } Delivery;
static void listener_unref(Listener *listener)
{ if (listener && g_atomic_int_dec_and_test(&listener->refs)) g_free(listener); }
static void delivery_free(gpointer data)
{ Delivery *delivery = data; listener_unref(delivery->listener); g_free(delivery); }
/* What is left once the device and the notification are released and no
 * thread uses the input any more. */
static void input_free(RadioSMidiInput *input)
{
    if (input->stop) CloseHandle(input->stop);
    if (input->data) CloseHandle(input->data);
    if (input->hotplug) CloseHandle(input->hotplug);
    DeleteCriticalSection(&input->lock); g_mutex_clear(&input->hotplug_lock); g_mutex_clear(&input->handle_lock);
    if (input->removed) g_ptr_array_unref(input->removed);
    if (input->known) g_hash_table_unref(input->known);
    g_free(input->raw); g_free(input->name); g_free(input->iface);
    if (input->context) g_main_context_unref(input->context);
    listener_unref(input->listener); g_free(input);
}

GQuark radios_midi_error_quark(void) { return g_quark_from_static_string("radios-midi"); }
gchar *radios_midi_event_key(const RadioSMidiEvent *event)
{
    if (!event) return NULL;
    if (event->kind == RADIOS_MIDI_PITCH) return g_strdup_printf("pitch:%u", event->channel);
    return g_strdup_printf("%s:%u:%u", event->kind == RADIOS_MIDI_NOTE ? "note" : "cc", event->channel, event->number);
}
gboolean radios_midi_key_valid(const gchar *key)
{
    if (!key) return FALSE;
    gchar **parts = g_strsplit(key, ":", -1); guint count = g_strv_length(parts); gboolean valid = FALSE;
    guint64 channel = 0, number = 0;
    if ((count == 3 && (g_str_equal(parts[0], "note") || g_str_equal(parts[0], "cc"))) || (count == 2 && g_str_equal(parts[0], "pitch")))
        valid = g_ascii_string_to_unsigned(parts[1], 10, 1, 16, &channel, NULL) &&
            (count == 2 || g_ascii_string_to_unsigned(parts[2], 10, 0, 127, &number, NULL));
    g_strfreev(parts); return valid;
}
gchar *radios_midi_key_label(const gchar *key)
{
    if (!radios_midi_key_valid(key)) return g_strdup(key ? key : "");
    gchar **parts = g_strsplit(key, ":", -1); gchar *label;
    if (g_str_equal(parts[0], "pitch")) label = g_strdup_printf("Pitch bend · Kanal %s", parts[1]);
    else label = g_strdup_printf("%s %s · Kanal %s", g_str_equal(parts[0], "note") ? "Nota" : "Kontrol (CC)", parts[2], parts[1]);
    g_strfreev(parts); return label;
}
/* Note off is velocity 0 and a note on with velocity 0 stays a note (not
 * pressed); every controller, also the channel mode ones (120-127), is a
 * control; pitch bend is centred at 8192 of 0..16383. Data bytes are masked
 * to 7 bits. */
gboolean radios_midi_decode_message(const guint8 *bytes, gsize length, RadioSMidiEvent *event)
{
    if (!event) return FALSE;
    memset(event, 0, sizeof *event);
    if (!bytes || length < 3) return FALSE;
    const guint type = bytes[0] & 0xF0u, channel = (bytes[0] & 0x0Fu) + 1u, first = bytes[1] & 0x7Fu, second = bytes[2] & 0x7Fu;
    switch (type) {
    case 0x80: case 0x90:
        event->kind = RADIOS_MIDI_NOTE; event->number = first;
        event->value = type == 0x90 ? second / 127.0 : 0; event->pressed = event->value > 0; break;
    case 0xB0:
        event->kind = RADIOS_MIDI_CONTROL; event->number = first; event->value = second / 127.0; event->pressed = event->value >= 0.5; break;
    case 0xE0:
        event->kind = RADIOS_MIDI_PITCH; event->value = (second << 7 | first) / 16383.0; event->pressed = event->value >= 0.5; break;
    default: return FALSE; /* aftertouch, program change, system and real-time messages, a data byte as status */
    }
    event->channel = channel; return TRUE;
}

/* --- devices -------------------------------------------------------------- */
/* UTF-8 of at most `length` UTF-16 units, up to a NUL; unpaired surrogates
 * become U+FFFD, so the result is always valid UTF-8. */
static gchar *utf16_text(const WCHAR *text, gsize length)
{
    gsize units = 0;
    while (units < length && units < 32767 && text[units]) ++units;
    const int bytes = units ? WideCharToMultiByte(CP_UTF8, 0, text, (int)units, NULL, 0, NULL, NULL) : 0;
    gchar *utf8 = g_malloc((gsize)MAX(bytes, 0) + 1); utf8[0] = '\0';
    if (bytes > 0 && WideCharToMultiByte(CP_UTF8, 0, text, (int)units, utf8, bytes, NULL, NULL) == bytes) utf8[bytes] = '\0';
    return utf8;
}
/* The operator's name of a device: its WinMM product name without control
 * characters around or inside it. */
static gchar *device_name(const WCHAR *pname)
{
    gchar *name = utf16_text(pname, MAXPNAMELEN);
    for (gchar *c = name; *c; ++c) if ((guchar)*c < 0x20 || *c == 0x7F) *c = ' ';
    g_strstrip(name);
    if (!*name) { g_free(name); name = g_strdup("MIDI girişi"); }
    return name;
}
/* The casefolded device interface path of WinMM input `index`, NULL for a
 * driver without one. WinMM answers both queries itself; they take the device
 * index cast to a handle, never an open handle. Sizes are in bytes. */
static gchar *device_interface(const RadioSMidiWinApi *api, UINT index)
{
    ULONG bytes = 0;
    if (api->in_message((HMIDIIN)(UINT_PTR)index, DRV_QUERYDEVICEINTERFACESIZE, (DWORD_PTR)&bytes, 0) != MMSYSERR_NOERROR ||
        bytes < 2 * sizeof(WCHAR) || bytes > PATH_BYTES_MAX) return NULL;
    WCHAR *path = g_malloc0(bytes + sizeof(WCHAR)); gchar *iface = NULL;
    if (api->in_message((HMIDIIN)(UINT_PTR)index, DRV_QUERYDEVICEINTERFACE, (DWORD_PTR)path, bytes) == MMSYSERR_NOERROR) {
        gchar *text = utf16_text(path, bytes / sizeof(WCHAR));
        if (*text) iface = g_utf8_casefold(text, -1);
        g_free(text);
    }
    g_free(path); return iface;
}
/* The device instance part of an interface path, "\\?\<instance>#{class}\reference". */
static gsize instance_span(const gchar *path, const gchar **start)
{
    if (g_str_has_prefix(path, "\\\\?\\") || g_str_has_prefix(path, "\\\\.\\")) path += 4;
    const gchar *end = strstr(path, "#{");
    *start = path; return end ? (gsize)(end - path) : strlen(path);
}
/* Two casefolded interface paths of one device: equal, or of the same device
 * instance (another interface class or reference string of it). The
 * functions of a composite USB device (mi_00, mi_01) stay apart. */
static gboolean same_device(const gchar *a, const gchar *b)
{
    if (!a || !b) return FALSE;
    if (g_str_equal(a, b)) return TRUE;
    const gchar *x, *y; const gsize length = instance_span(a, &x);
    return length && instance_span(b, &y) == length && memcmp(x, y, length) == 0;
}
typedef struct {
    UINT index;
    gchar *raw, *name, *iface, *key; /* WinMM name, listed name, interface path, list identity */
    guint32 fingerprint;
} WinPort;
static void win_port_free(gpointer data)
{ WinPort *port = data; g_free(port->raw); g_free(port->name); g_free(port->iface); g_free(port->key); g_free(port); }
static guint32 fingerprint_of(const gchar *name, const gchar *iface)
{
    guint32 hash = 2166136261u; /* FNV-1a */
    for (const gchar *c = name; *c; ++c) hash = (hash ^ (guchar)*c) * 16777619u;
    hash = (hash ^ (guchar)'\n') * 16777619u;
    for (const gchar *c = iface ? iface : ""; *c; ++c) hash = (hash ^ (guchar)*c) * 16777619u;
    return hash;
}
static gint duplicate_order(gconstpointer a, gconstpointer b)
{
    const WinPort *x = *(WinPort *const *)a, *y = *(WinPort *const *)b;
    if (!x->iface != !y->iface) return x->iface ? -1 : 1;
    const gint order = x->iface ? strcmp(x->iface, y->iface) : 0;
    return order ? order : (x->index > y->index) - (x->index < y->index);
}
/* Windows normally tells identical units apart itself ("2- nanoKONTROL2",
 * "MIDIIN2 (...)"); where a driver does not, the second and later units of an
 * exact name get " #2", " #3"... in interface path order. A number that would
 * repeat another device's name is skipped. */
static void name_duplicates(GPtrArray *ports)
{
    GHashTable *taken = g_hash_table_new(g_str_hash, g_str_equal), *done = g_hash_table_new(g_str_hash, g_str_equal);
    GPtrArray *group = g_ptr_array_new();
    for (guint i = 0; i < ports->len; ++i) g_hash_table_add(taken, ((WinPort *)g_ptr_array_index(ports, i))->raw);
    for (guint i = 0; i < ports->len; ++i) {
        const WinPort *first = g_ptr_array_index(ports, i);
        if (g_hash_table_contains(done, first->raw)) continue;
        g_hash_table_add(done, first->raw); g_ptr_array_set_size(group, 0);
        for (guint j = i; j < ports->len; ++j)
            if (g_str_equal(((WinPort *)g_ptr_array_index(ports, j))->raw, first->raw)) g_ptr_array_add(group, g_ptr_array_index(ports, j));
        g_ptr_array_sort(group, duplicate_order);
        guint ordinal = 1;
        for (guint k = 0; k < group->len; ++k) {
            WinPort *member = g_ptr_array_index(group, k);
            if (!k) { member->name = g_strdup(member->raw); continue; }
            do { g_free(member->name); member->name = g_strdup_printf("%s #%u", member->raw, ++ordinal); }
            while (g_hash_table_contains(taken, member->name));
            g_hash_table_add(taken, member->name);
        }
    }
    g_ptr_array_unref(group); g_hash_table_unref(done); g_hash_table_unref(taken);
}
/* The WinMM inputs in index order; NULL when `stopped` was set meanwhile (a
 * closing input waits at most for one WinMM call). */
static GPtrArray *scan(const RadioSMidiWinApi *api, const gint *stopped)
{
    GPtrArray *ports = g_ptr_array_new_with_free_func(win_port_free);
    const UINT listed = api->in_get_num_devs(), count = MIN(listed, DEVICES_MAX);
    for (UINT index = 0; index < count; ++index) {
        if (stopped && g_atomic_int_get(stopped)) { g_ptr_array_unref(ports); return NULL; }
        MIDIINCAPSW caps; memset(&caps, 0, sizeof caps);
        /* The device may have gone between the count and this query. */
        if (api->in_get_dev_caps(index, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
        WinPort *port = g_new0(WinPort, 1); port->index = index; port->raw = device_name(caps.szPname);
        port->iface = device_interface(api, index); g_ptr_array_add(ports, port);
    }
    name_duplicates(ports);
    for (guint i = 0; i < ports->len; ++i) {
        WinPort *port = g_ptr_array_index(ports, i);
        port->key = g_strconcat(port->iface ? port->iface : "", "\n", port->name, NULL);
        port->fingerprint = fingerprint_of(port->name, port->iface);
    }
    return ports;
}
GPtrArray *radios_midi_list_inputs(GError **error)
{
    (void)error; /* WinMM has nothing to open first: a listing cannot fail, it can only be empty */
    GPtrArray *found = scan(RADIOS_MIDI_WIN_API(), NULL), *ports = g_ptr_array_new_with_free_func(g_free);
    for (guint i = 0; i < found->len; ++i) {
        const WinPort *port = g_ptr_array_index(found, i); RadioSMidiPort *entry = g_new0(RadioSMidiPort, 1);
        g_snprintf(entry->id, sizeof entry->id, "%u:%08x", port->index, port->fingerprint);
        g_strlcpy(entry->name, port->name, sizeof entry->name); /* at most 31 UTF-16 units and a number: never cut */
        g_ptr_array_add(ports, entry);
    }
    g_ptr_array_unref(found); return ports;
}
/* "<index>:<fingerprint>": one to four decimal digits and eight lower-case hex digits. */
static gboolean parse_id(const gchar *id, guint *index, guint32 *fingerprint)
{
    const gchar *colon = strchr(id, ':');
    if (!colon || colon == id || colon - id > 4 || strlen(colon + 1) != 8) return FALSE;
    guint value = 0; guint32 print = 0;
    for (const gchar *c = id; c < colon; ++c) { if (!g_ascii_isdigit(*c)) return FALSE; value = value * 10 + (guint)(*c - '0'); }
    for (const gchar *c = colon + 1; *c; ++c) {
        if (!g_ascii_isxdigit(*c) || g_ascii_isupper(*c)) return FALSE;
        print = print << 4 | (guint32)g_ascii_xdigit_value(*c);
    }
    *index = value; *fingerprint = print; return TRUE;
}
/* The listed device of a port id: at its index, else wherever it moved. */
static const WinPort *find_port(GPtrArray *ports, guint index, guint32 fingerprint)
{
    const WinPort *moved = NULL;
    for (guint i = 0; i < ports->len; ++i) {
        const WinPort *port = g_ptr_array_index(ports, i);
        if (port->fingerprint != fingerprint) continue;
        if (port->index == index) return port;
        if (!moved) moved = port;
    }
    return moved;
}

/* --- delivery ------------------------------------------------------------- */
static gboolean deliver(gpointer data)
{
    Delivery *delivery = data; Listener *listener = delivery->listener;
    if (g_atomic_int_get(&listener->stopped)) return G_SOURCE_REMOVE;
    if (!delivery->notice) listener->callback(&delivery->event, listener->user);
    else if (listener->notice) listener->notice(delivery->what, listener->user);
    return G_SOURCE_REMOVE;
}
static void queue(RadioSMidiInput *input, Delivery *delivery)
{
    g_atomic_int_inc(&input->listener->refs); delivery->listener = input->listener;
    /* Always queue: g_main_context_invoke_full would run the callback
     * here, on the worker thread, whenever the main loop is idle. */
    GSource *source = g_idle_source_new(); g_source_set_priority(source, G_PRIORITY_HIGH_IDLE);
    g_source_set_callback(source, deliver, delivery, delivery_free);
    g_source_attach(source, input->context); g_source_unref(source);
}
static void notify(RadioSMidiInput *input, RadioSMidiNotice what)
{
    if (!input->listener->notice) return;
    Delivery *delivery = g_new0(Delivery, 1); delivery->notice = TRUE; delivery->what = what; queue(input, delivery);
}
static void lost(RadioSMidiInput *input)
{
    if (g_atomic_int_compare_and_exchange(&input->alive, 1, 0)) notify(input, RADIOS_MIDI_NOTICE_LOST);
}

/* --- WinMM ---------------------------------------------------------------- */
static void ring_push(RadioSMidiInput *input, RingKind kind, guint32 message, MIDIHDR *header)
{
    EnterCriticalSection(&input->lock);
    if (input->head - input->tail < RING_SIZE) {
        RingItem *item = &input->ring[input->head++ % RING_SIZE];
        item->kind = (guint8)kind; item->message = message; item->header = header;
    } else g_atomic_int_inc(&input->dropped);
    LeaveCriticalSection(&input->lock);
}
/* WinMM's callback, on a driver thread. Only critical sections and SetEvent
 * here: no multimedia function (Microsoft: deadlock) and no allocation.
 * Real-time and system messages (clock, active sensing), program change and
 * aftertouch are dropped at once; RadioS maps none of them. */
static void CALLBACK midi_in_proc(HMIDIIN handle, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR param2)
{
    (void)param2; /* milliseconds since midiInStart */
    RadioSMidiInput *input = (RadioSMidiInput *)instance;
    switch (message) {
    case MIM_DATA: {
        const DWORD_PTR type = param1 & 0xF0;
        if (type != 0x80 && type != 0x90 && type != 0xB0 && type != 0xE0) return;
        ring_push(input, RING_SHORT, (guint32)param1, NULL); break;
    }
    case MIM_LONGDATA: case MIM_LONGERROR: /* a returned SysEx buffer, complete or not */
        ring_push(input, RING_SYSEX, 0, (MIDIHDR *)param1); break;
    case MIM_CLOSE:
        if ((gpointer)handle == g_atomic_pointer_get(&input->closed_by_us)) return;
        g_atomic_int_set(&input->driver_closed, 1); ring_push(input, RING_CLOSED, 0, NULL); break;
    default: return; /* MIM_OPEN, MIM_ERROR (an invalid short message), MIM_MOREDATA */
    }
    SetEvent(input->data);
}
/* Gives the device back to WinMM: classic WinMM input is exclusive per
 * device, so a lost input must not keep it. Order: stop, reset (the driver
 * returns every queued SysEx buffer), unprepare, close. A handle the driver
 * closed on its own is no longer valid and only forgotten. */
static void release_handle(RadioSMidiInput *input, gboolean valid)
{
    const RadioSMidiWinApi *api = input->api;
    g_mutex_lock(&input->handle_lock);
    HMIDIIN handle = input->handle; input->handle = NULL;
    if (handle && valid && !g_atomic_int_get(&input->driver_closed)) {
        g_atomic_pointer_set(&input->closed_by_us, (gpointer)handle);
        (void)api->in_stop(handle);
        (void)api->in_reset(handle);
        for (guint i = 0; i < input->sysex_prepared; ++i) {
            MMRESULT code = api->in_unprepare_header(handle, &input->sysex[i], sizeof(MIDIHDR));
            if (code == MIDIERR_STILLPLAYING) { (void)api->in_reset(handle); code = api->in_unprepare_header(handle, &input->sysex[i], sizeof(MIDIHDR)); }
            if (code != MMSYSERR_NOERROR) g_debug("MIDI: SysEx buffer %u of %s not unprepared (MMRESULT %u)", i, input->name ? input->name : "a device", code);
        }
        MMRESULT code = api->in_close(handle);
        if (code == MIDIERR_STILLPLAYING) { (void)api->in_reset(handle); code = api->in_close(handle); }
        if (code == MMSYSERR_INVALHANDLE) g_debug("MIDI: %s was already closed by its driver", input->name ? input->name : "a device");
        else if (code != MMSYSERR_NOERROR)
            g_warning("MIDI: %s could not be closed (MMRESULT %u); it stays held until RadioS exits", input->name ? input->name : "a device", code);
    }
    input->sysex_prepared = 0;
    g_mutex_unlock(&input->handle_lock);
}
/* The device is gone: it is given back first, so whoever hears LOST can open
 * it again at once. */
static void lose(RadioSMidiInput *input, gboolean valid) { release_handle(input, valid); lost(input); }
/* Forgets everything a released handle left in the ring (open's second round). */
static void ring_clear(RadioSMidiInput *input)
{
    EnterCriticalSection(&input->lock); input->tail = input->head; LeaveCriticalSection(&input->lock);
    g_atomic_int_set(&input->driver_closed, 0);
}
static gboolean own_header(RadioSMidiInput *input, const MIDIHDR *header)
{
    for (guint i = 0; i < input->sysex_prepared; ++i) if (header == &input->sysex[i]) return TRUE;
    return FALSE;
}
/* Runs the ring on the worker: maps channel messages, hands SysEx buffers
 * back to the driver (outside the callback, as WinMM requires) and follows a
 * driver's own close. Messages that came before a loss are still delivered. */
static void drain(RadioSMidiInput *input)
{
    RingItem items[DRAIN_CHUNK];
    for (guint total = 0; total < RING_SIZE;) {
        guint count = 0;
        EnterCriticalSection(&input->lock);
        while (count < DRAIN_CHUNK && input->tail != input->head) items[count++] = input->ring[input->tail++ % RING_SIZE];
        LeaveCriticalSection(&input->lock);
        if (!count) break;
        total += count;
        for (guint i = 0; i < count; ++i) {
            const RingItem *item = &items[i];
            if (item->kind == RING_SHORT) {
                const guint8 bytes[3] = {(guint8)(item->message & 0xFF), (guint8)((item->message >> 8) & 0xFF), (guint8)((item->message >> 16) & 0xFF)};
                RadioSMidiEvent event;
                if (radios_midi_decode_message(bytes, sizeof bytes, &event)) {
                    Delivery *delivery = g_new0(Delivery, 1); delivery->event = event; queue(input, delivery);
                }
            } else if (item->kind == RING_SYSEX) {
                /* Its content maps to no control: the buffer only goes back to the driver. Under the
                 * handle lock: a buffer must never reach a handle that close is releasing. */
                g_mutex_lock(&input->handle_lock);
                if (input->handle && own_header(input, item->header) && !g_atomic_int_get(&input->listener->stopped)) {
                    item->header->dwBytesRecorded = 0;
                    const MMRESULT code = input->api->in_add_buffer(input->handle, item->header, sizeof(MIDIHDR));
                    if (code != MMSYSERR_NOERROR) g_debug("MIDI: SysEx buffer of %s not queued again (MMRESULT %u)", input->name, code);
                }
                g_mutex_unlock(&input->handle_lock);
            } else lose(input, FALSE); /* closed by its driver: the handle is no longer valid */
        }
    }
    const gint dropped = g_atomic_int_get(&input->dropped);
    if (dropped) { g_atomic_int_add(&input->dropped, -dropped); g_debug("MIDI: %d messages of %s dropped (input ring full)", dropped, input->name); }
}
/* WinMM errors that mean the device or its handle is gone. */
static gboolean gone(MMRESULT code)
{ return code == MMSYSERR_NODRIVER || code == MMSYSERR_INVALHANDLE || code == MMSYSERR_BADDEVICEID; }

/* --- hotplug -------------------------------------------------------------- */
/* The configuration manager's callback, on a thread-pool thread: it records
 * removed interface paths and arrivals for the worker. It never unregisters:
 * CM_Unregister_Notification waits for running callbacks (deadlock). */
static DWORD CALLBACK device_notification(HCMNOTIFICATION notification, PVOID context, CM_NOTIFY_ACTION action,
    PCM_NOTIFY_EVENT_DATA data, DWORD size)
{
    (void)notification; RadioSMidiInput *input = context;
    if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) g_atomic_int_set(&input->arrived, 1);
    else if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL) {
        /* The link runs past the declared one-element array, up to `size`. */
        const gsize offset = (gsize)G_STRUCT_OFFSET(CM_NOTIFY_EVENT_DATA, u.DeviceInterface.SymbolicLink);
        gchar *link = data && data->FilterType == CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE && size > offset ?
            utf16_text((const WCHAR *)((const BYTE *)data + offset), (size - offset) / sizeof(WCHAR)) : g_strdup("");
        gchar *path = *link ? g_utf8_casefold(link, -1) : NULL; gboolean kept = FALSE;
        g_free(link);
        g_mutex_lock(&input->hotplug_lock);
        if (path && input->removed->len < REMOVED_MAX) { g_ptr_array_add(input->removed, path); kept = TRUE; }
        g_mutex_unlock(&input->hotplug_lock);
        /* An unrecorded removal still has the device list checked soon. */
        if (!kept) { g_free(path); g_atomic_int_set(&input->arrived, 1); }
    } else return ERROR_SUCCESS;
    SetEvent(input->hotplug);
    return ERROR_SUCCESS;
}
/* Applies the recorded notifications: a removed device leaves the known list
 * (it is new again when it comes back, also in the same socket) and the
 * removal of our device loses the input and releases it at once. TRUE when
 * something arrived or left. */
static gboolean take_hotplug(RadioSMidiInput *input)
{
    GPtrArray *removed = NULL;
    g_mutex_lock(&input->hotplug_lock);
    if (input->removed->len) { removed = input->removed; input->removed = g_ptr_array_new_with_free_func(g_free); }
    g_mutex_unlock(&input->hotplug_lock);
    for (guint i = 0; removed && i < removed->len; ++i) {
        const gchar *path = g_ptr_array_index(removed, i);
        GHashTableIter iter; gpointer iface;
        g_hash_table_iter_init(&iter, input->known);
        while (g_hash_table_iter_next(&iter, NULL, &iface)) if (same_device(iface, path)) g_hash_table_iter_remove(&iter);
        if (same_device(input->iface, path)) lose(input, TRUE);
    }
    const gboolean changed = removed != NULL;
    if (removed) g_ptr_array_unref(removed);
    return g_atomic_int_compare_and_exchange(&input->arrived, 1, 0) || changed;
}
/* A look at the WinMM device list: each device not listed before is a PORT
 * notice (never straight from Plug and Play: WinMM's list lags it, and a
 * reconnect started too early would not find the device). While alive, our
 * device must still be listed (two misses in a row: a list being rebuilt for
 * another device is no removal) and its handle must still answer. */
static void rescan(RadioSMidiInput *input)
{
    GPtrArray *ports = scan(input->api, &input->listener->stopped);
    if (!ports) return;
    GHashTable *listed = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free); gboolean present = FALSE;
    for (guint i = 0; i < ports->len; ++i) {
        const WinPort *port = g_ptr_array_index(ports, i);
        if (!g_hash_table_contains(input->known, port->key)) notify(input, RADIOS_MIDI_NOTICE_PORT);
        g_hash_table_replace(listed, g_strdup(port->key), g_strdup(port->iface));
        present |= g_str_equal(port->raw, input->raw) && (!input->iface || same_device(port->iface, input->iface));
    }
    g_hash_table_unref(input->known); input->known = listed; g_ptr_array_unref(ports);
    if (!g_atomic_int_get(&input->alive)) return;
    input->misses = present ? 0 : input->misses + 1;
    /* The queries need no lock: on a handle close has just released they only
     * fail (WinMM validates handles), and the input is closed anyway. No
     * comparison of midiInGetID with a listed index: indexes shift whenever
     * another device goes, and a working controller would be dropped. */
    g_mutex_lock(&input->handle_lock); HMIDIIN handle = input->handle; g_mutex_unlock(&input->handle_lock);
    MIDIINCAPSW caps; UINT id = 0; memset(&caps, 0, sizeof caps);
    if (input->misses >= 2 || (handle && (gone(input->api->in_get_dev_caps((UINT_PTR)handle, &caps, sizeof caps)) ||
                                          gone(input->api->in_get_id(handle, &id))))) lose(input, TRUE);
}
static gpointer reader(gpointer data)
{
    RadioSMidiInput *input = data;
    HANDLE waits[3] = {input->stop, input->data, input->hotplug};
    gint64 next_scan = g_get_monotonic_time() + LIVENESS_MS * G_TIME_SPAN_MILLISECOND, burst_until = 0;
    while (!g_atomic_int_get(&input->listener->stopped)) {
        gint64 now = g_get_monotonic_time();
        const DWORD timeout = next_scan > now ? (DWORD)MIN((next_scan - now + 999) / 1000, (gint64)LIVENESS_MS) : 0;
        const DWORD woke = WaitForMultipleObjects((DWORD)G_N_ELEMENTS(waits), waits, FALSE, timeout);
        if (woke == WAIT_OBJECT_0) break;
        if (woke == WAIT_FAILED) { lose(input, TRUE); break; } /* the input cannot be followed any more */
        drain(input);
        if (take_hotplug(input)) { burst_until = g_get_monotonic_time() + BURST_MS * G_TIME_SPAN_MILLISECOND; next_scan = 0; }
        now = g_get_monotonic_time();
        if (now >= next_scan && !g_atomic_int_get(&input->listener->stopped)) {
            rescan(input);
            next_scan = now + (now < burst_until ? BURST_STEP_MS : LIVENESS_MS) * G_TIME_SPAN_MILLISECOND;
        }
    }
    /* Closed while this thread was still inside a WinMM call: ours to free. */
    if (!g_atomic_int_compare_and_exchange(&input->worker, WORKER_RUNNING, WORKER_EXITED)) input_free(input);
    return NULL;
}

/* --- open and close ------------------------------------------------------- */
static void open_failed(GError **error, const RadioSMidiWinApi *api, const gchar *device, MMRESULT code)
{
    gchar *reason = NULL;
    switch (code) {
    case MMSYSERR_ALLOCATED: reason = g_strdup("cihaz başka bir uygulama tarafından kullanılıyor"); break;
    case MMSYSERR_BADDEVICEID: reason = g_strdup("cihaz bulunamadı"); break;
    case MMSYSERR_NODRIVER: reason = g_strdup("cihaz sürücüsü yanıt vermiyor"); break;
    case MMSYSERR_NOMEM: reason = g_strdup("bellek yetersiz"); break;
    default: {
        WCHAR text[MAXERRORLENGTH]; memset(text, 0, sizeof text);
        if (api->in_get_error_text(code, text, MAXERRORLENGTH) == MMSYSERR_NOERROR) reason = g_strstrip(utf16_text(text, MAXERRORLENGTH));
        if (!reason || !*reason) { g_free(reason); reason = g_strdup_printf("MMRESULT %u", code); }
    } }
    g_set_error(error, RADIOS_MIDI_ERROR, code == MMSYSERR_ALLOCATED ? RADIOS_MIDI_ERROR_BUSY : RADIOS_MIDI_ERROR_CONNECT,
        "MIDI cihazına bağlanılamadı (%s): %s", device, reason);
    g_free(reason);
}
/* The open handle is the listed device: right after a hotplug the index may
 * already name another one. midiInGetDevCaps takes a handle cast to an id. */
static gboolean handle_is(RadioSMidiInput *input, const WinPort *port)
{
    MIDIINCAPSW caps; UINT id = 0; memset(&caps, 0, sizeof caps);
    if (input->api->in_get_dev_caps((UINT_PTR)input->handle, &caps, sizeof caps) != MMSYSERR_NOERROR) return FALSE;
    gchar *raw = device_name(caps.szPname); gboolean same = g_str_equal(raw, port->raw);
    g_free(raw);
    if (!same || !port->iface) return same;
    if (input->api->in_get_id(input->handle, &id) != MMSYSERR_NOERROR) return FALSE;
    gchar *iface = device_interface(input->api, id); same = iface && g_str_equal(iface, port->iface);
    g_free(iface); return same;
}
/* Best effort: a driver that refuses SysEx buffers still delivers channel messages. */
static void sysex_queue(RadioSMidiInput *input)
{
    while (input->sysex_prepared < SYSEX_BUFFERS) {
        MIDIHDR *header = &input->sysex[input->sysex_prepared]; memset(header, 0, sizeof *header);
        header->lpData = input->sysex_data[input->sysex_prepared]; header->dwBufferLength = SYSEX_BYTES;
        MMRESULT code = input->api->in_prepare_header(input->handle, header, sizeof *header);
        if (code == MMSYSERR_NOERROR) {
            ++input->sysex_prepared; /* unprepared on release, also when queueing it fails below */
            code = input->api->in_add_buffer(input->handle, header, sizeof *header);
        }
        if (code != MMSYSERR_NOERROR) { g_debug("MIDI: %u SysEx buffers for %s (MMRESULT %u)", input->sysex_prepared, input->name, code); break; }
    }
}
RadioSMidiInput *radios_midi_input_open_full(const gchar *port_id, GMainContext *context, RadioSMidiCallback callback,
    RadioSMidiNoticeCallback notice, gpointer user, GError **error)
{
    guint index = 0; guint32 fingerprint = 0;
    if (!port_id || !callback || !parse_id(port_id, &index, &fingerprint)) {
        g_set_error_literal(error, RADIOS_MIDI_ERROR, RADIOS_MIDI_ERROR_PORT, "Geçerli bir MIDI giriş portu seçin."); return NULL;
    }
    RadioSMidiInput *input = g_new0(RadioSMidiInput, 1);
    input->listener = g_new0(Listener, 1); input->listener->refs = 1;
    input->listener->callback = callback; input->listener->notice = notice; input->listener->user = user;
    input->api = RADIOS_MIDI_WIN_API();
    InitializeCriticalSection(&input->lock); g_mutex_init(&input->hotplug_lock); g_mutex_init(&input->handle_lock);
    input->removed = g_ptr_array_new_with_free_func(g_free);
    input->known = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    if (!(input->stop = CreateEventW(NULL, FALSE, FALSE, NULL)) || !(input->data = CreateEventW(NULL, FALSE, FALSE, NULL)) ||
        !(input->hotplug = CreateEventW(NULL, FALSE, FALSE, NULL))) {
        gchar *reason = g_win32_error_message((gint)GetLastError());
        g_set_error(error, RADIOS_MIDI_ERROR, RADIOS_MIDI_ERROR_SYSTEM, "MIDI sistemi açılamadı: %s", reason);
        g_free(reason); radios_midi_input_close(input); return NULL;
    }
    /* Notifications first, so a removal between the listing below and the
     * open is not missed; the callback may run before the call returns. Where
     * they are refused, the worker's list check still sees removals and new
     * devices, only later. */
    CM_NOTIFY_FILTER filter; memset(&filter, 0, sizeof filter);
    filter.cbSize = sizeof filter; filter.Flags = CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES;
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    if (input->api->register_notification(&filter, input, device_notification, &input->notification) != CR_SUCCESS) {
        input->notification = NULL; g_debug("MIDI: no device notifications; the device list is checked every second");
    }
    GPtrArray *ports = NULL; const WinPort *port = NULL; MMRESULT code = MMSYSERR_NOERROR;
    /* A second round when the device list changed between the listing and the open. */
    for (guint round = 0; round < 2 && !input->handle; ++round) {
        if (ports) g_ptr_array_unref(ports);
        ports = scan(input->api, NULL); port = find_port(ports, index, fingerprint); code = MMSYSERR_NOERROR;
        if (!port) break;
        code = input->api->in_open(&input->handle, port->index, (DWORD_PTR)midi_in_proc, (DWORD_PTR)input, CALLBACK_FUNCTION);
        if (code != MMSYSERR_NOERROR) { input->handle = NULL; if (code == MMSYSERR_BADDEVICEID) continue; break; }
        if (handle_is(input, port)) break;
        release_handle(input, TRUE); ring_clear(input); port = NULL; /* another device: what it sent is dropped */
    }
    if (!input->handle) {
        if (code != MMSYSERR_NOERROR) open_failed(error, input->api, port ? port->name : port_id, code);
        else g_set_error(error, RADIOS_MIDI_ERROR, RADIOS_MIDI_ERROR_CONNECT,
            "MIDI cihazına bağlanılamadı (%s): cihaz bulunamadı; çıkarılmış ya da yeniden takılmış olabilir", port_id);
        g_ptr_array_unref(ports); radios_midi_input_close(input); return NULL;
    }
    input->raw = g_strdup(port->raw); input->name = g_strdup(port->name); input->iface = g_strdup(port->iface);
    sysex_queue(input);
    if ((code = input->api->in_start(input->handle)) != MMSYSERR_NOERROR) {
        open_failed(error, input->api, port->name, code); g_ptr_array_unref(ports); radios_midi_input_close(input); return NULL;
    }
    for (guint i = 0; i < ports->len; ++i) {
        const WinPort *listed = g_ptr_array_index(ports, i);
        g_hash_table_replace(input->known, g_strdup(listed->key), g_strdup(listed->iface));
    }
    g_ptr_array_unref(ports);
    g_atomic_int_set(&input->alive, 1);
    g_strlcpy(input->port_id, port_id, sizeof input->port_id);
    input->context = context ? g_main_context_ref(context) : g_main_context_ref_thread_default();
    GError *failure = NULL;
    if (!(input->thread = g_thread_try_new("radios-midi", reader, input, &failure))) {
        g_set_error(error, RADIOS_MIDI_ERROR, RADIOS_MIDI_ERROR_SYSTEM, "MIDI sistemi açılamadı: %s", failure->message);
        g_error_free(failure); radios_midi_input_close(input); return NULL;
    }
    return input;
}
RadioSMidiInput *radios_midi_input_open(const gchar *port_id, RadioSMidiCallback callback, gpointer user, GError **error)
{ return radios_midi_input_open_full(port_id, NULL, callback, NULL, user, error); }
const gchar *radios_midi_input_port(RadioSMidiInput *input) { return input ? input->port_id : NULL; }
gboolean radios_midi_input_alive(RadioSMidiInput *input) { return input && g_atomic_int_get(&input->alive); }
/* Safe on partly opened inputs. The device and the notification are released
 * before this returns (no WinMM callback runs after midiInClose, none of the
 * configuration manager after unregistering), so "Yenile" can open the device
 * again at once; deliveries still queued are dropped (the listener stopped).
 * The worker is not waited for while it is inside a WinMM listing, which can
 * stall during USB enumeration or while the Windows MIDI service starts: this
 * runs on the GTK thread, and the worker frees the input when it gets out. */
void radios_midi_input_close(RadioSMidiInput *input)
{
    if (!input) return;
    g_atomic_int_set(&input->listener->stopped, 1);
    if (input->stop) SetEvent(input->stop);
    release_handle(input, TRUE);
    if (input->notification) input->api->unregister_notification(input->notification);
    input->notification = NULL;
    GThread *thread = input->thread; /* not read again once the worker may free the input */
    if (thread && g_atomic_int_compare_and_exchange(&input->worker, WORKER_RUNNING, WORKER_ORPHANED)) { g_thread_unref(thread); return; }
    if (thread) g_thread_join(thread);
    input_free(input);
}
