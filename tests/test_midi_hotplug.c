/* R8.4 on Windows · MIDI controller unplug / replug, "Yenile", a slow device
 * system, learning and mapping controls, against an in-process fake WinMM and
 * configuration manager (after radios-f1's independent verification
 * harness). src/core/midi.c is compiled into this file with
 * RADIOS_MIDI_WIN_API() pointing at the fake table below, so the executable
 * imports no midiIn* or CM_* function and no real device is ever listed or
 * opened (WinMM input is exclusive: a test must never take a controller away
 * from a running RadioS). The fake models what the module relies on: WinMM
 * indexes in plug order (a removal shifts the later devices), one device
 * interface path per USB socket, exclusive opens, stale handles after removal,
 * a driver closing the input on its own, SysEx buffer queues, and Plug and
 * Play notifications ahead of a WinMM device list that may lag them. The
 * window's MIDI service (src/ui/midi_ui.c, from radios_ui) drives it as in the
 * application; its window is shown (off screen) only to learn controls. */
#include <gtk/gtk.h>
#include <math.h>
#include <string.h>

#include "radios/midi_ui.h"
#include "radios/views.h"

/* src/core/midi.c makes every WinMM and configuration manager call through
 * this table; it is defined after the include. */
static const struct RadioSMidiWinApi *fake_api(void);
#define RADIOS_MIDI_WIN_API() fake_api()
#include "../src/core/midi.c"

/* ---- fake WinMM and configuration manager -------------------------------- */
typedef void (CALLBACK *FakeMidiProc)(HMIDIIN handle, UINT message, DWORD_PTR data, DWORD_PTR param1, DWORD_PTR param2);
typedef struct {
    guint address;       /* the USB socket: with the model it gives the device interface path */
    guint instance;      /* new on every plug */
    gchar name[96];      /* the WinMM product name */
    gchar path[192];     /* the device interface path, "" for a driver without one */
    gint64 listed_at;    /* WinMM lists it from then on: its device list lags Plug and Play */
} FakeDevice;
typedef struct {
    guint instance;
    gchar name[96];
    FakeMidiProc callback; DWORD_PTR data;
    gboolean started, stale, driver_closed;
    GPtrArray *prepared, *queued; /* MIDIHDR*: prepared ones; handed to the driver, oldest first */
} FakeHandle;
typedef struct { PCM_NOTIFY_CALLBACK callback; PVOID context; } FakeRegistration;

static GMutex fake_lock;          /* everything below */
static GRecMutex fake_midi_lock;  /* held while MIDI callbacks run and by midiInClose: none runs after close returns */
static GMutex fake_cm_lock;       /* held while notification callbacks run and by unregister: none runs after it */
static GPtrArray *fake_devices, *fake_handles, *fake_registrations;
static guint fake_next_instance = 1, fake_unplug_on_open;
/* calls: every fake call; opens: the calls that can stall (listings and
 * opens); connects: successful opens; busy_calls: stalling calls under way;
 * gtk_stalls: stalling calls made on the GTK thread (R8.4: there must be none). */
static gint fake_calls, fake_opens, fake_connects, fake_busy_calls, fake_misuses, fake_close_failures, fake_gtk_stalls;
static GThread *fake_gtk_thread;
static gchar fake_misuse_what[160];
static gint64 fake_delay_us, fake_list_lag_us;
static gboolean fake_connect_fails, fake_notices_refused, fake_busy, fake_close_on_removal, fake_no_interface,
    fake_stale_answers, fake_removal_class_only;
static DWORD fake_clock;
#define KS_AUDIO "6994ad04-93ef-11d0-a3cc-00a0c9223196"
#define KS_CAPTURE "65e8773d-8f56-11d0-a3b9-00a0c9223196"

/* The rest is called with fake_lock held. */
static void misuse(const gchar *what) { if (!fake_misuses++) g_strlcpy(fake_misuse_what, what, sizeof fake_misuse_what); }
static FakeDevice *listed_device(UINT_PTR index)
{
    const gint64 now = g_get_monotonic_time(); UINT_PTR seen = 0;
    for (guint i = 0; i < fake_devices->len; ++i) {
        FakeDevice *device = g_ptr_array_index(fake_devices, i);
        if (device->listed_at <= now && seen++ == index) return device;
    }
    return NULL;
}
static gint listed_index(guint instance)
{
    const gint64 now = g_get_monotonic_time(); gint seen = 0;
    for (guint i = 0; i < fake_devices->len; ++i) {
        const FakeDevice *device = g_ptr_array_index(fake_devices, i);
        if (device->listed_at > now) continue;
        if (device->instance == instance) return seen;
        ++seen;
    }
    return -1;
}
static FakeDevice *device_at(guint address)
{
    for (guint i = 0; i < fake_devices->len; ++i)
        if (((FakeDevice *)g_ptr_array_index(fake_devices, i))->address == address) return g_ptr_array_index(fake_devices, i);
    return NULL;
}
static FakeHandle *handle_of(UINT_PTR value)
{
    for (guint i = 0; i < fake_handles->len; ++i) if ((UINT_PTR)g_ptr_array_index(fake_handles, i) == value) return g_ptr_array_index(fake_handles, i);
    return NULL;
}
static FakeHandle *live_handle(HMIDIIN value, const gchar *call)
{
    FakeHandle *handle = handle_of((UINT_PTR)value);
    if (!handle || handle->driver_closed) { gchar *what = g_strdup_printf("%s on a %s handle", call, handle ? "driver-closed" : "unknown"); misuse(what); g_free(what); return NULL; }
    return handle;
}
static gboolean instance_open(guint instance)
{
    for (guint i = 0; i < fake_handles->len; ++i) {
        const FakeHandle *handle = g_ptr_array_index(fake_handles, i);
        if (handle->instance == instance && !handle->driver_closed) return TRUE;
    }
    return FALSE;
}
static void handle_free(FakeHandle *handle) { g_ptr_array_unref(handle->prepared); g_ptr_array_unref(handle->queued); g_free(handle); }
static DWORD fake_tick(void) { return fake_clock += 3; }
static void fake_pname(WCHAR *pname, const gchar *name)
{
    /* WinMM keeps 31 UTF-16 units of a product name. */
    glong units = 0; gunichar2 *text = g_utf8_to_utf16(name, -1, NULL, &units, NULL);
    for (glong i = 0; text && i < units && i < MAXPNAMELEN - 1; ++i) pname[i] = text[i];
    g_free(text);
}

static UINT WINAPI fake_in_get_num_devs(void)
{
    g_mutex_lock(&fake_lock); const gint64 delay = fake_delay_us; ++fake_calls; ++fake_busy_calls;
    fake_gtk_stalls += g_thread_self() == fake_gtk_thread;
    g_mutex_unlock(&fake_lock);
    if (delay) g_usleep((gulong)delay);
    g_mutex_lock(&fake_lock);
    UINT count = 0; const gint64 now = g_get_monotonic_time();
    for (guint i = 0; i < fake_devices->len; ++i) count += ((FakeDevice *)g_ptr_array_index(fake_devices, i))->listed_at <= now;
    ++fake_opens; --fake_busy_calls;
    g_mutex_unlock(&fake_lock);
    return count;
}
static MMRESULT WINAPI fake_in_get_dev_caps(UINT_PTR device, LPMIDIINCAPSW caps, UINT size)
{
    MMRESULT code = MMSYSERR_BADDEVICEID; const gchar *name = NULL;
    g_mutex_lock(&fake_lock); ++fake_calls;
    FakeHandle *handle = handle_of(device);
    if (!caps || size != sizeof *caps) { misuse("midiInGetDevCaps with a wrong size"); code = MMSYSERR_INVALPARAM; }
    else if (handle) {
        if (handle->driver_closed) { misuse("midiInGetDevCaps on a driver-closed handle"); code = MMSYSERR_INVALHANDLE; }
        else if (handle->stale && !fake_stale_answers) code = MMSYSERR_NODRIVER;
        else name = handle->name;
    } else {
        const FakeDevice *listed = listed_device(device);
        if (listed) name = listed->name;
    }
    if (name) { memset(caps, 0, sizeof *caps); caps->wMid = 0xFF; caps->wPid = 1; caps->vDriverVersion = 0x0100; fake_pname(caps->szPname, name); code = MMSYSERR_NOERROR; }
    g_mutex_unlock(&fake_lock);
    return code;
}
/* Only the two device interface queries, which WinMM answers for a device
 * index (cast to a handle), never for an open handle. */
static MMRESULT WINAPI fake_in_message(HMIDIIN handle, UINT message, DWORD_PTR param1, DWORD_PTR param2)
{
    MMRESULT code = MMSYSERR_BADDEVICEID;
    g_mutex_lock(&fake_lock); ++fake_calls;
    const FakeDevice *device = listed_device((UINT_PTR)handle);
    if (message != DRV_QUERYDEVICEINTERFACESIZE && message != DRV_QUERYDEVICEINTERFACE) { misuse("midiInMessage with another message"); code = MMSYSERR_NOTSUPPORTED; }
    else if (handle_of((UINT_PTR)handle)) { misuse("midiInMessage(DRV_QUERYDEVICEINTERFACE*) on an open handle"); code = MMSYSERR_NOTSUPPORTED; }
    else if (device && !*device->path) code = MMSYSERR_NOTSUPPORTED; /* a driver without a device interface */
    else if (device) {
        glong units = 0; gunichar2 *path = g_utf8_to_utf16(device->path, -1, NULL, &units, NULL);
        const ULONG bytes = (ULONG)((units + 1) * (glong)sizeof(WCHAR));
        if (message == DRV_QUERYDEVICEINTERFACESIZE) { *(ULONG *)param1 = bytes; code = MMSYSERR_NOERROR; }
        else if (param2 >= bytes) { memcpy((void *)param1, path, bytes); code = MMSYSERR_NOERROR; }
        else code = MMSYSERR_INVALPARAM;
        g_free(path);
    }
    g_mutex_unlock(&fake_lock);
    return code;
}
static void device_unplug(guint address);
static MMRESULT WINAPI fake_in_open(LPHMIDIIN out, UINT index, DWORD_PTR callback, DWORD_PTR data, DWORD flags)
{
    g_mutex_lock(&fake_lock);
    const gint64 delay = fake_delay_us; const guint unplug = fake_unplug_on_open; fake_unplug_on_open = 0; ++fake_calls; ++fake_busy_calls;
    fake_gtk_stalls += g_thread_self() == fake_gtk_thread;
    g_mutex_unlock(&fake_lock);
    if (delay) g_usleep((gulong)delay);
    if (unplug) device_unplug(unplug); /* the device list changes between the listing and this open */
    g_rec_mutex_lock(&fake_midi_lock); g_mutex_lock(&fake_lock);
    ++fake_opens; --fake_busy_calls;
    const FakeDevice *device = listed_device(index); FakeHandle *handle = NULL; MMRESULT code = MMSYSERR_NOERROR;
    if (!out || !callback || flags != CALLBACK_FUNCTION) { misuse("midiInOpen without CALLBACK_FUNCTION"); code = MMSYSERR_INVALPARAM; }
    else if (!device) code = MMSYSERR_BADDEVICEID;
    else if (fake_connect_fails) code = MMSYSERR_ERROR;
    else if (fake_busy || instance_open(device->instance)) code = MMSYSERR_ALLOCATED; /* classic WinMM input is exclusive */
    else {
        handle = g_new0(FakeHandle, 1); handle->instance = device->instance; g_strlcpy(handle->name, device->name, sizeof handle->name);
        handle->callback = (FakeMidiProc)callback; handle->data = data;
        handle->prepared = g_ptr_array_new(); handle->queued = g_ptr_array_new();
        g_ptr_array_add(fake_handles, handle); ++fake_connects; *out = (HMIDIIN)handle;
    }
    g_mutex_unlock(&fake_lock);
    if (handle) handle->callback((HMIDIIN)handle, MIM_OPEN, handle->data, 0, 0); /* during midiInOpen, as WinMM does */
    g_rec_mutex_unlock(&fake_midi_lock);
    return code;
}
static MMRESULT WINAPI fake_in_start(HMIDIIN value)
{
    g_mutex_lock(&fake_lock); ++fake_calls; FakeHandle *handle = live_handle(value, "midiInStart");
    if (handle) handle->started = TRUE;
    g_mutex_unlock(&fake_lock);
    return handle ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE;
}
static MMRESULT WINAPI fake_in_stop(HMIDIIN value)
{
    g_mutex_lock(&fake_lock); ++fake_calls; FakeHandle *handle = live_handle(value, "midiInStop");
    if (handle) handle->started = FALSE;
    g_mutex_unlock(&fake_lock);
    return handle ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE;
}
/* Returns every queued SysEx buffer, marked done, through MIM_LONGDATA. */
static MMRESULT WINAPI fake_in_reset(HMIDIIN value)
{
    g_rec_mutex_lock(&fake_midi_lock); g_mutex_lock(&fake_lock); ++fake_calls;
    FakeHandle *handle = live_handle(value, "midiInReset"); GPtrArray *returned = NULL; const DWORD tick = fake_tick();
    if (handle) {
        handle->started = FALSE; returned = handle->queued; handle->queued = g_ptr_array_new();
        for (guint i = 0; i < returned->len; ++i) {
            MIDIHDR *header = g_ptr_array_index(returned, i);
            header->dwFlags = (header->dwFlags & ~(DWORD)MHDR_INQUEUE) | MHDR_DONE; header->dwBytesRecorded = 0;
        }
    }
    g_mutex_unlock(&fake_lock);
    for (guint i = 0; returned && i < returned->len; ++i) handle->callback(value, MIM_LONGDATA, handle->data, (DWORD_PTR)g_ptr_array_index(returned, i), tick);
    g_rec_mutex_unlock(&fake_midi_lock);
    if (returned) g_ptr_array_unref(returned);
    return handle ? MMSYSERR_NOERROR : MMSYSERR_INVALHANDLE;
}
static MMRESULT WINAPI fake_in_prepare(HMIDIIN value, LPMIDIHDR header, UINT size)
{
    MMRESULT code = MMSYSERR_INVALHANDLE;
    g_mutex_lock(&fake_lock); ++fake_calls; FakeHandle *handle = live_handle(value, "midiInPrepareHeader");
    if (handle && (size != sizeof *header || !header->lpData || !header->dwBufferLength)) { misuse("midiInPrepareHeader with bad arguments"); code = MMSYSERR_INVALPARAM; }
    else if (handle) {
        header->dwFlags |= MHDR_PREPARED;
        if (!g_ptr_array_find(handle->prepared, header, NULL)) g_ptr_array_add(handle->prepared, header);
        code = MMSYSERR_NOERROR;
    }
    g_mutex_unlock(&fake_lock);
    return code;
}
static MMRESULT WINAPI fake_in_unprepare(HMIDIIN value, LPMIDIHDR header, UINT size)
{
    MMRESULT code = MMSYSERR_INVALHANDLE;
    g_mutex_lock(&fake_lock); ++fake_calls; FakeHandle *handle = live_handle(value, "midiInUnprepareHeader");
    if (handle && size != sizeof *header) { misuse("midiInUnprepareHeader with a wrong size"); code = MMSYSERR_INVALPARAM; }
    else if (handle && g_ptr_array_find(handle->queued, header, NULL)) code = MIDIERR_STILLPLAYING;
    else if (handle) {
        if (!g_ptr_array_remove(handle->prepared, header)) misuse("midiInUnprepareHeader of a header that is not prepared");
        header->dwFlags &= ~(DWORD)MHDR_PREPARED; code = MMSYSERR_NOERROR;
    }
    g_mutex_unlock(&fake_lock);
    return code;
}
static MMRESULT WINAPI fake_in_add_buffer(HMIDIIN value, LPMIDIHDR header, UINT size)
{
    MMRESULT code = MMSYSERR_INVALHANDLE;
    g_mutex_lock(&fake_lock); ++fake_calls; FakeHandle *handle = live_handle(value, "midiInAddBuffer");
    if (handle && (size != sizeof *header || !g_ptr_array_find(handle->prepared, header, NULL))) { misuse("midiInAddBuffer of an unprepared header"); code = MIDIERR_UNPREPARED; }
    else if (handle && g_ptr_array_find(handle->queued, header, NULL)) { misuse("midiInAddBuffer of a queued header"); code = MIDIERR_STILLPLAYING; }
    else if (handle) {
        header->dwFlags = (header->dwFlags & ~(DWORD)MHDR_DONE) | MHDR_INQUEUE; g_ptr_array_add(handle->queued, header); code = MMSYSERR_NOERROR;
    }
    g_mutex_unlock(&fake_lock);
    return code;
}
static MMRESULT WINAPI fake_in_close(HMIDIIN value)
{
    g_rec_mutex_lock(&fake_midi_lock); g_mutex_lock(&fake_lock); ++fake_calls;
    FakeHandle *handle = live_handle(value, "midiInClose"); MMRESULT code = MMSYSERR_INVALHANDLE;
    if (handle && handle->queued->len) { ++fake_close_failures; code = MIDIERR_STILLPLAYING; handle = NULL; }
    else if (handle) {
        if (handle->prepared->len) misuse("midiInClose with prepared SysEx headers");
        g_ptr_array_remove(fake_handles, handle); code = MMSYSERR_NOERROR;
    }
    g_mutex_unlock(&fake_lock);
    if (handle) { handle->callback(value, MIM_CLOSE, handle->data, 0, 0); handle_free(handle); }
    g_rec_mutex_unlock(&fake_midi_lock);
    return code;
}
static MMRESULT WINAPI fake_in_get_id(HMIDIIN value, LPUINT id)
{
    MMRESULT code = MMSYSERR_INVALHANDLE;
    g_mutex_lock(&fake_lock); ++fake_calls; FakeHandle *handle = live_handle(value, "midiInGetID");
    if (handle && handle->stale) { code = fake_stale_answers ? MMSYSERR_NOERROR : MMSYSERR_NODRIVER; if (!code) *id = 0; }
    else if (handle) { const gint index = listed_index(handle->instance); code = index >= 0 ? MMSYSERR_NOERROR : MMSYSERR_NODRIVER; if (index >= 0) *id = (UINT)index; }
    g_mutex_unlock(&fake_lock);
    return code;
}
static MMRESULT WINAPI fake_in_error_text(MMRESULT code, LPWSTR text, UINT size)
{
    gchar *message = g_strdup_printf("Sahte WinMM hatası %u", code); glong units = 0;
    gunichar2 *wide = g_utf8_to_utf16(message, -1, NULL, &units, NULL); UINT copied = 0;
    for (; size && copied + 1 < size && copied < (UINT)units; ++copied) text[copied] = wide[copied];
    if (size) text[copied] = 0;
    g_free(wide); g_free(message);
    return size ? MMSYSERR_NOERROR : MMSYSERR_INVALPARAM;
}
static CONFIGRET WINAPI fake_cm_register(PCM_NOTIFY_FILTER filter, PVOID context, PCM_NOTIFY_CALLBACK callback, PHCMNOTIFICATION out)
{
    CONFIGRET code = CR_FAILURE;
    g_mutex_lock(&fake_lock); ++fake_calls;
    if (!filter || filter->cbSize != sizeof *filter || filter->Flags != CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES ||
        filter->FilterType != CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE || !callback || !out) misuse("CM_Register_Notification arguments");
    else if (!fake_notices_refused) {
        FakeRegistration *registration = g_new0(FakeRegistration, 1); registration->callback = callback; registration->context = context;
        g_ptr_array_add(fake_registrations, registration); *out = (HCMNOTIFICATION)registration; code = CR_SUCCESS;
    }
    g_mutex_unlock(&fake_lock);
    return code;
}
static CONFIGRET WINAPI fake_cm_unregister(HCMNOTIFICATION notification)
{
    g_mutex_lock(&fake_cm_lock); /* waits for running callbacks, as the configuration manager does */
    g_mutex_lock(&fake_lock); ++fake_calls;
    const gboolean known = g_ptr_array_remove(fake_registrations, notification);
    if (!known) misuse("CM_Unregister_Notification of an unknown registration");
    g_mutex_unlock(&fake_lock); g_mutex_unlock(&fake_cm_lock);
    if (known) g_free(notification);
    return known ? CR_SUCCESS : CR_INVALID_POINTER;
}
static const RadioSMidiWinApi fake_table = {
    .in_get_num_devs = fake_in_get_num_devs, .in_get_dev_caps = fake_in_get_dev_caps, .in_open = fake_in_open,
    .in_start = fake_in_start, .in_stop = fake_in_stop, .in_reset = fake_in_reset, .in_close = fake_in_close,
    .in_prepare_header = fake_in_prepare, .in_unprepare_header = fake_in_unprepare, .in_add_buffer = fake_in_add_buffer,
    .in_message = fake_in_message, .in_get_id = fake_in_get_id, .in_get_error_text = fake_in_error_text,
    .register_notification = fake_cm_register, .unregister_notification = fake_cm_unregister,
};
static const RadioSMidiWinApi *fake_api(void) { return &fake_table; }

/* ---- the test's side of the fake: devices and their users ---------------- */
static void fake_path(gchar *path, gsize size, const gchar *name, guint address, const gchar *klass, const gchar *reference)
{
    /* A USB device without a serial number: its instance id encodes the socket. */
    g_snprintf(path, size, "\\\\?\\USB#VID_0FAE&PID_%04X&MI_00#7&2C3E8A5&0&%04u#{%s}\\%s", g_str_hash(name) & 0xFFFFu, address, klass, reference);
}
static void fake_notify(CM_NOTIFY_ACTION action, const gchar *path)
{
    glong units = 0; gunichar2 *link = g_utf8_to_utf16(path, -1, NULL, &units, NULL);
    const gsize offset = (gsize)G_STRUCT_OFFSET(CM_NOTIFY_EVENT_DATA, u.DeviceInterface.SymbolicLink), bytes = ((gsize)units + 1) * sizeof(WCHAR);
    CM_NOTIFY_EVENT_DATA *data = g_malloc0(MAX(offset + bytes, sizeof *data));
    data->FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE; memcpy((BYTE *)data + offset, link, bytes);
    g_mutex_lock(&fake_cm_lock);
    g_mutex_lock(&fake_lock); GPtrArray *targets = g_ptr_array_copy(fake_registrations, NULL, NULL); g_mutex_unlock(&fake_lock);
    for (guint i = 0; i < targets->len; ++i) {
        FakeRegistration *registration = g_ptr_array_index(targets, i);
        registration->callback((HCMNOTIFICATION)registration, registration->context, action, data, (DWORD)(offset + bytes));
    }
    g_mutex_unlock(&fake_cm_lock);
    g_ptr_array_unref(targets); g_free(data); g_free(link);
}
/* Called with fake_lock held: the paths notified for a device, the WinMM one
 * in another letter case. */
static GPtrArray *device_paths(const FakeDevice *device)
{
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    if (!*device->path) return paths;
    if (!fake_removal_class_only) g_ptr_array_add(paths, g_ascii_strdown(device->path, -1));
    gchar other[192]; fake_path(other, sizeof other, device->name, device->address, KS_CAPTURE, "midi"); g_ptr_array_add(paths, g_strdup(other));
    return paths;
}
/* Called with fake_lock held. */
static FakeDevice *device_new(guint address, const gchar *name)
{
    FakeDevice *device = g_new0(FakeDevice, 1); device->address = address; device->instance = fake_next_instance++;
    g_strlcpy(device->name, name, sizeof device->name);
    if (!fake_no_interface) fake_path(device->path, sizeof device->path, name, address, KS_AUDIO, "global");
    device->listed_at = g_get_monotonic_time() + fake_list_lag_us;
    return device;
}
/* A device plugged into socket `address`, listed last (as WinMM does). */
static void device_plug_named(guint address, const gchar *name)
{
    g_mutex_lock(&fake_lock);
    g_assert_null(device_at(address));
    FakeDevice *device = device_new(address, name); g_ptr_array_add(fake_devices, device);
    GPtrArray *paths = device_paths(device);
    g_mutex_unlock(&fake_lock);
    for (guint i = 0; i < paths->len; ++i) fake_notify(CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL, g_ptr_array_index(paths, i));
    g_ptr_array_unref(paths);
}
static void device_plug(guint address, const gchar *name)
{ gchar *full = g_strdup_printf("%s MIDI 1", name); device_plug_named(address, full); g_free(full); }
/* Called with both locks held: the device goes, later ones move up; its
 * handles go stale and, with fake_close_on_removal, are closed by the driver. */
static GPtrArray *device_remove(FakeDevice *device, GPtrArray *closed)
{
    for (guint i = 0; i < fake_handles->len; ++i) {
        FakeHandle *handle = g_ptr_array_index(fake_handles, i);
        if (handle->instance != device->instance || handle->driver_closed) continue;
        handle->stale = TRUE; handle->started = FALSE;
        if (fake_close_on_removal) { handle->driver_closed = TRUE; g_ptr_array_add(closed, handle); }
    }
    GPtrArray *paths = device_paths(device);
    g_ptr_array_remove(fake_devices, device);
    return paths;
}
static void device_unplug(guint address)
{
    GPtrArray *closed = g_ptr_array_new(), *paths = NULL;
    g_rec_mutex_lock(&fake_midi_lock); g_mutex_lock(&fake_lock);
    FakeDevice *device = device_at(address);
    if (device) paths = device_remove(device, closed);
    g_mutex_unlock(&fake_lock);
    for (guint i = 0; i < closed->len; ++i) {
        FakeHandle *handle = g_ptr_array_index(closed, i);
        handle->callback((HMIDIIN)handle, MIM_CLOSE, handle->data, 0, 0);
    }
    g_rec_mutex_unlock(&fake_midi_lock);
    for (guint i = 0; paths && i < paths->len; ++i) fake_notify(CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL, g_ptr_array_index(paths, i));
    if (paths) g_ptr_array_unref(paths);
    g_ptr_array_unref(closed);
}
/* Out and straight back in, in the same socket and list position: no list
 * look can fall between the two. */
static void device_replug(guint address)
{
    GPtrArray *closed = g_ptr_array_new(), *gone = NULL, *back = NULL;
    g_rec_mutex_lock(&fake_midi_lock); g_mutex_lock(&fake_lock);
    FakeDevice *device = device_at(address); g_assert_nonnull(device);
    guint position = 0; g_assert_true(g_ptr_array_find(fake_devices, device, &position));
    gchar name[96]; g_strlcpy(name, device->name, sizeof name);
    gone = device_remove(device, closed);
    FakeDevice *again = device_new(address, name); again->listed_at = g_get_monotonic_time();
    g_ptr_array_insert(fake_devices, (gint)position, again); back = device_paths(again);
    g_mutex_unlock(&fake_lock);
    for (guint i = 0; i < closed->len; ++i) { FakeHandle *handle = g_ptr_array_index(closed, i); handle->callback((HMIDIIN)handle, MIM_CLOSE, handle->data, 0, 0); }
    g_rec_mutex_unlock(&fake_midi_lock);
    for (guint i = 0; i < gone->len; ++i) fake_notify(CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL, g_ptr_array_index(gone, i));
    for (guint i = 0; i < back->len; ++i) fake_notify(CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL, g_ptr_array_index(back, i));
    g_ptr_array_unref(gone); g_ptr_array_unref(back); g_ptr_array_unref(closed);
}
/* Another device's interfaces (a USB stick) coming and going. */
static void device_noise(void)
{
    const gchar *path = "\\\\?\\USBSTOR#Disk&Ven_Fake&Prod_Stick&Rev_1.00#0001#{53f56307-b6bf-11d0-94f2-00a0c91efb8b}";
    fake_notify(CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL, path); fake_notify(CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL, path);
}
/* A device's short message reaches only the started, live handles of it
 * (as Windows routes it); the number of handles it reached. */
static guint device_send(guint address, DWORD message)
{
    GPtrArray *targets = g_ptr_array_new();
    g_rec_mutex_lock(&fake_midi_lock); g_mutex_lock(&fake_lock);
    const FakeDevice *device = device_at(address);
    for (guint i = 0; device && i < fake_handles->len; ++i) {
        FakeHandle *handle = g_ptr_array_index(fake_handles, i);
        if (handle->instance == device->instance && handle->started && !handle->stale && !handle->driver_closed) g_ptr_array_add(targets, handle);
    }
    const DWORD tick = fake_tick();
    g_mutex_unlock(&fake_lock);
    for (guint i = 0; i < targets->len; ++i) {
        FakeHandle *handle = g_ptr_array_index(targets, i);
        handle->callback((HMIDIIN)handle, MIM_DATA, handle->data, message, tick);
    }
    g_rec_mutex_unlock(&fake_midi_lock);
    const guint reached = targets->len; g_ptr_array_unref(targets);
    return reached;
}
static guint device_send_cc(guint address, guint controller, guint value)
{ return device_send(address, 0xB0u | controller << 8 | value << 16); }
static guint device_send_note(guint address, guint note, guint velocity)
{ return device_send(address, (velocity ? 0x90u : 0x80u) | note << 8 | velocity << 16); }
/* A SysEx message into the first queued buffer of each live handle; the
 * number of buffers filled (none queued: the driver drops it). */
static guint device_send_sysex(guint address, const guint8 *bytes, gsize length, gboolean broken)
{
    GPtrArray *targets = g_ptr_array_new(), *headers = g_ptr_array_new();
    g_rec_mutex_lock(&fake_midi_lock); g_mutex_lock(&fake_lock);
    const FakeDevice *device = device_at(address);
    for (guint i = 0; device && i < fake_handles->len; ++i) {
        FakeHandle *handle = g_ptr_array_index(fake_handles, i);
        if (handle->instance != device->instance || !handle->started || handle->stale || handle->driver_closed || !handle->queued->len) continue;
        MIDIHDR *header = g_ptr_array_steal_index(handle->queued, 0);
        const gsize copied = MIN(length, header->dwBufferLength); memcpy(header->lpData, bytes, copied);
        header->dwBytesRecorded = (DWORD)copied; header->dwFlags = (header->dwFlags & ~(DWORD)MHDR_INQUEUE) | MHDR_DONE;
        g_ptr_array_add(targets, handle); g_ptr_array_add(headers, header);
    }
    const DWORD tick = fake_tick();
    g_mutex_unlock(&fake_lock);
    for (guint i = 0; i < targets->len; ++i) {
        FakeHandle *handle = g_ptr_array_index(targets, i);
        handle->callback((HMIDIIN)handle, broken ? MIM_LONGERROR : MIM_LONGDATA, handle->data, (DWORD_PTR)g_ptr_array_index(headers, i), tick);
    }
    g_rec_mutex_unlock(&fake_midi_lock);
    const guint filled = targets->len; g_ptr_array_unref(targets); g_ptr_array_unref(headers);
    return filled;
}
/* SysEx buffers queued on the live handles of a device. */
static guint queued_buffers(guint address)
{
    guint queued = 0;
    g_mutex_lock(&fake_lock);
    const FakeDevice *device = device_at(address);
    for (guint i = 0; device && i < fake_handles->len; ++i) {
        const FakeHandle *handle = g_ptr_array_index(fake_handles, i);
        if (handle->instance == device->instance && !handle->driver_closed) queued += handle->queued->len;
    }
    g_mutex_unlock(&fake_lock);
    return queued;
}
static guint open_handles(void)
{
    g_mutex_lock(&fake_lock); guint open = 0;
    for (guint i = 0; i < fake_handles->len; ++i) open += !((FakeHandle *)g_ptr_array_index(fake_handles, i))->driver_closed;
    g_mutex_unlock(&fake_lock);
    return open;
}
/* Open handles, notification registrations and listings or opens under way. */
static guint open_clients(void)
{
    const guint handles = open_handles();
    g_mutex_lock(&fake_lock); const guint open = handles + fake_registrations->len + (guint)fake_busy_calls; g_mutex_unlock(&fake_lock);
    return open;
}
static gint fake_count(const gint *counter) { g_mutex_lock(&fake_lock); const gint value = *counter; g_mutex_unlock(&fake_lock); return value; }
static void fake_set(gboolean *knob, gboolean value) { g_mutex_lock(&fake_lock); *knob = value; g_mutex_unlock(&fake_lock); }
static void fake_set_delay(gint64 delay_us) { g_mutex_lock(&fake_lock); fake_delay_us = delay_us; g_mutex_unlock(&fake_lock); }
/* The listed port id of the device in socket `address`, NULL when unlisted. */
static gchar *fake_port_id(guint address)
{
    GPtrArray *ports = radios_midi_list_inputs(NULL); gchar *id = NULL;
    for (guint i = 0; i < ports->len && !id; ++i) {
        const RadioSMidiPort *port = g_ptr_array_index(ports, i); guint index = 0; guint32 print = 0;
        g_assert_true(parse_id(port->id, &index, &print));
        g_mutex_lock(&fake_lock); const FakeDevice *device = listed_device(index); const gboolean match = device && device->address == address; g_mutex_unlock(&fake_lock);
        if (match) id = g_strdup(port->id);
    }
    g_ptr_array_unref(ports);
    return id;
}
/* The listed name of the device in socket `address`. */
static gchar *fake_port_name(guint address)
{
    GPtrArray *ports = radios_midi_list_inputs(NULL); gchar *name = NULL;
    for (guint i = 0; i < ports->len && !name; ++i) {
        const RadioSMidiPort *port = g_ptr_array_index(ports, i); guint index = 0; guint32 print = 0;
        g_assert_true(parse_id(port->id, &index, &print));
        g_mutex_lock(&fake_lock); const FakeDevice *device = listed_device(index); const gboolean match = device && device->address == address; g_mutex_unlock(&fake_lock);
        if (match) name = g_strdup(port->name);
    }
    g_ptr_array_unref(ports);
    return name;
}
static void fake_reset(void)
{
    g_assert_cmpuint(open_clients(), ==, 0);
    g_mutex_lock(&fake_lock);
    for (guint i = 0; i < fake_handles->len; ++i) handle_free(g_ptr_array_index(fake_handles, i)); /* abandoned by their drivers */
    g_ptr_array_set_size(fake_handles, 0);
    for (guint i = 0; i < fake_devices->len; ++i) g_free(g_ptr_array_index(fake_devices, i));
    g_ptr_array_set_size(fake_devices, 0);
    fake_unplug_on_open = 0; fake_misuses = fake_close_failures = fake_gtk_stalls = 0; fake_misuse_what[0] = '\0';
    fake_delay_us = fake_list_lag_us = 0;
    fake_connect_fails = fake_notices_refused = fake_busy = fake_close_on_removal = fake_no_interface = FALSE;
    fake_stale_answers = fake_removal_class_only = FALSE;
    g_mutex_unlock(&fake_lock);
}

/* ---- harness ------------------------------------------------------------- */
static gboolean display_available;
static gint64 last_beat, worst_gap;
static gboolean heartbeat(gpointer data)
{
    (void)data; gint64 now = g_get_monotonic_time();
    if (last_beat && now - last_beat > worst_gap) worst_gap = now - last_beat;
    last_beat = now; return G_SOURCE_CONTINUE;
}
static void gap_reset(void) { last_beat = 0; worst_gap = 0; }
static void pump_context(GMainContext *context, gdouble seconds)
{
    gint64 until = g_get_monotonic_time() + (gint64)(seconds * G_USEC_PER_SEC);
    while (g_get_monotonic_time() < until) { while (g_main_context_iteration(context, FALSE)) {} g_usleep(1000); }
}
static void pump(gdouble seconds) { pump_context(NULL, seconds); }
typedef gboolean (*Condition)(gpointer data);
/* Pumps `context` until `condition` holds; the elapsed time or -1. */
static gint64 pump_context_until(GMainContext *context, Condition condition, gpointer data, gdouble seconds)
{
    const gint64 start = g_get_monotonic_time(), until = start + (gint64)(seconds * G_USEC_PER_SEC);
    while (!condition(data)) {
        if (g_get_monotonic_time() > until) return -1;
        while (g_main_context_iteration(context, FALSE)) {}
        g_usleep(1000);
    }
    return g_get_monotonic_time() - start;
}
static gint64 pump_until(Condition condition, gpointer data, gdouble seconds) { return pump_context_until(NULL, condition, data, seconds); }
static gboolean no_clients(gpointer data) { (void)data; return open_clients() == 0; }
/* Nothing held any more: close releases the device and the notification at
 * once, a worker still inside a listing finishes it (and frees the input) on
 * its own. */
static void settled(void) { g_assert_cmpint(pump_until(no_clients, NULL, 3), >=, 0); }
/* R8.4: no listing or open of a device ever ran on the GTK thread. (Counted
 * instead of timed: on a loaded Windows machine an idle GTK main loop alone
 * shows heartbeat gaps of a few hundred milliseconds.) */
static void no_gtk_stalls(void) { g_assert_cmpint(fake_count(&fake_gtk_stalls), ==, 0); }
/* Every test ends with nothing held and no call WinMM would refuse. */
static void fake_done(void)
{
    settled();
    g_mutex_lock(&fake_lock);
    const gint misuses = fake_misuses, close_failures = fake_close_failures; gchar what[160]; g_strlcpy(what, fake_misuse_what, sizeof what);
    g_mutex_unlock(&fake_lock);
    if (misuses) g_test_message("first misuse: %s", what);
    g_assert_cmpstr(what, ==, ""); g_assert_cmpint(misuses, ==, 0); g_assert_cmpint(close_failures, ==, 0);
}

typedef struct { GPtrArray *keys; GArray *values, *pressed; gint lost, ports; GThread *thread; gboolean listed_at_port; guint address; } Heard;
static void heard_init(Heard *heard)
{
    memset(heard, 0, sizeof *heard); heard->keys = g_ptr_array_new_with_free_func(g_free);
    heard->values = g_array_new(FALSE, FALSE, sizeof(gdouble)); heard->pressed = g_array_new(FALSE, FALSE, sizeof(gboolean));
}
static void heard_clear(Heard *heard) { g_ptr_array_unref(heard->keys); g_array_unref(heard->values); g_array_unref(heard->pressed); }
static void heard_event(const RadioSMidiEvent *event, gpointer user)
{
    Heard *heard = user; g_ptr_array_add(heard->keys, radios_midi_event_key(event));
    g_array_append_val(heard->values, event->value); g_array_append_val(heard->pressed, event->pressed);
    if (heard->thread) g_assert_true(g_thread_self() == heard->thread);
}
static void heard_notice(RadioSMidiNotice notice, gpointer user)
{
    Heard *heard = user;
    if (notice == RADIOS_MIDI_NOTICE_LOST) { ++heard->lost; return; }
    /* A PORT notice means the device can be listed and opened now. */
    if (heard->address && !heard->ports) { gchar *id = fake_port_id(heard->address); heard->listed_at_port = id != NULL; g_free(id); }
    ++heard->ports;
}
static gboolean heard_keys(gpointer data) { return ((Heard *)data)->keys->len > 0; }
static gboolean heard_lost(gpointer data) { return ((Heard *)data)->lost > 0; }
static gboolean heard_two_ports(gpointer data) { return ((Heard *)data)->ports >= 2; }
static gboolean heard_port(gpointer data) { return ((Heard *)data)->ports >= 1; }
typedef struct { Heard *heard; guint count; } HeardCount;
static gboolean heard_count(gpointer data) { HeardCount *wait = data; return wait->heard->keys->len >= wait->count; }
static RadioSMidiInput *open_device(guint address, Heard *heard)
{
    gchar *id = fake_port_id(address); g_assert_nonnull(id); GError *error = NULL;
    RadioSMidiInput *input = radios_midi_input_open_full(id, NULL, heard_event, heard_notice, heard, &error);
    g_assert_no_error(error); g_assert_nonnull(input); g_assert_true(radios_midi_input_alive(input));
    g_assert_cmpstr(radios_midi_input_port(input), ==, id); g_free(id);
    return input;
}

/* An open input hears its device leave at once from the device notification
 * and new devices appear; other RadioS inputs and unrelated devices are no
 * MIDI devices. A device back in the same socket is a new device, not the old
 * connection, and the lost input has given the device back. */
static void test_input_announcements(void)
{
    /* The dead handle keeps answering: only the notification can lose the
     * input in less than a second (the list check needs two misses). */
    fake_reset(); fake_set(&fake_stale_answers, TRUE); device_plug(20, "nanoKONTROL2");
    Heard heard, other_heard; heard_init(&heard); heard_init(&other_heard);
    RadioSMidiInput *input = open_device(20, &heard);
    g_assert_cmpuint(device_send_cc(20, 7, 127), ==, 1);
    g_assert_cmpint(pump_until(heard_keys, &heard, 2), >=, 0);
    g_assert_cmpstr(g_ptr_array_index(heard.keys, 0), ==, "cc:1:7");
    GError *error = NULL; GPtrArray *ports = radios_midi_list_inputs(&error);
    g_assert_no_error(error); g_assert_cmpuint(ports->len, ==, 1);
    g_assert_cmpstr(((RadioSMidiPort *)g_ptr_array_index(ports, 0))->name, ==, "nanoKONTROL2 MIDI 1");
    g_ptr_array_unref(ports);
    device_noise(); pump(0.4);
    g_assert_cmpint(heard.ports, ==, 0); g_assert_cmpint(heard.lost, ==, 0);
    device_plug(24, "Launchpad");
    g_assert_cmpint(pump_until(heard_port, &heard, 1), >=, 0);
    RadioSMidiInput *other = open_device(24, &other_heard);
    radios_midi_input_close(other);
    pump(0.3);
    g_assert_cmpint(heard.ports, ==, 1); g_assert_cmpint(heard.lost, ==, 0);
    const gint64 unplugged = g_get_monotonic_time();
    device_unplug(20);
    g_assert_cmpint(pump_until(heard_lost, &heard, 2), >=, 0);
    /* From the device notification, not the once-a-second list check. */
    g_test_message("lost %.0f ms after the removal", (g_get_monotonic_time() - unplugged) / 1000.0);
    g_assert_cmpint(g_get_monotonic_time() - unplugged, <, 900000);
    g_assert_false(radios_midi_input_alive(input));
    g_assert_cmpuint(open_handles(), ==, 0); /* released before LOST was sent */
    g_assert_cmpuint(device_send_cc(20, 7, 1), ==, 0);
    device_plug(20, "nanoKONTROL2");
    g_assert_cmpint(pump_until(heard_two_ports, &heard, 1), >=, 0);
    pump(1.3);
    g_assert_false(radios_midi_input_alive(input)); g_assert_cmpint(heard.lost, ==, 1);
    g_assert_cmpuint(device_send_cc(20, 7, 1), ==, 0);
    g_assert_cmpuint(heard.keys->len, ==, 1); g_assert_cmpuint(other_heard.keys->len, ==, 0);
    radios_midi_input_close(input);
    fake_done();
    heard_clear(&heard); heard_clear(&other_heard);
}

/* Where device notifications are refused, the worker's own check of the
 * handle still loses the input within about a second, also when the device
 * came straight back into the same socket (which is then no new device). */
static void test_input_liveness_without_announcements(void)
{
    fake_reset(); fake_set(&fake_notices_refused, TRUE); device_plug(20, "nanoKONTROL2");
    Heard heard; heard_init(&heard);
    RadioSMidiInput *input = open_device(20, &heard);
    pump(0.2);
    device_replug(20);
    const gint64 elapsed = pump_until(heard_lost, &heard, 3);
    g_assert_cmpint(elapsed, >=, 0); g_assert_cmpint(elapsed, <, 2500000);
    g_assert_false(radios_midi_input_alive(input)); g_assert_cmpint(heard.ports, ==, 0);
    g_assert_cmpuint(open_handles(), ==, 0);
    radios_midi_input_close(input);
    fake_done(); heard_clear(&heard);
}

/* No notifications and a driver whose dead handle still answers: the device
 * missing from two list checks in a row loses the input. */
static void test_input_liveness_from_list(void)
{
    fake_reset(); fake_set(&fake_notices_refused, TRUE); fake_set(&fake_stale_answers, TRUE); device_plug(20, "nanoKONTROL2");
    Heard heard; heard_init(&heard);
    RadioSMidiInput *input = open_device(20, &heard);
    pump(0.2);
    device_unplug(20);
    const gint64 elapsed = pump_until(heard_lost, &heard, 4);
    g_test_message("lost %.0f ms after the removal", elapsed / 1000.0);
    g_assert_cmpint(elapsed, >=, 0); g_assert_cmpint(elapsed, <, 3000000); /* two list checks a second apart */
    g_assert_cmpint(heard.lost, ==, 1); g_assert_cmpuint(open_handles(), ==, 0);
    radios_midi_input_close(input);
    fake_done(); heard_clear(&heard);
}

/* A driver that closes the input itself on removal (MIM_CLOSE): lost at
 * once, and the dead handle never reaches WinMM again, not even at close. */
static void test_input_driver_close(void)
{
    fake_reset(); fake_set(&fake_notices_refused, TRUE); fake_set(&fake_close_on_removal, TRUE); device_plug(20, "nanoKONTROL2");
    fake_set(&fake_stale_answers, TRUE); /* only MIM_CLOSE can lose it in less than a second */
    Heard heard; heard_init(&heard);
    RadioSMidiInput *input = open_device(20, &heard);
    pump(0.1);
    const gint64 unplugged = g_get_monotonic_time();
    device_unplug(20);
    g_assert_cmpint(pump_until(heard_lost, &heard, 2), >=, 0);
    g_test_message("lost %.0f ms after the removal", (g_get_monotonic_time() - unplugged) / 1000.0);
    g_assert_cmpint(g_get_monotonic_time() - unplugged, <, 900000);
    g_assert_false(radios_midi_input_alive(input));
    pump(1.2); /* list and handle checks after the loss */
    radios_midi_input_close(input);
    g_assert_cmpint(heard.lost, ==, 1);
    fake_done(); heard_clear(&heard);
}

/* Clock, active sensing, program change, aftertouch and SysEx map to no
 * control: only the controller arrives. SysEx buffers go back to the driver
 * (also after an incomplete SysEx) and are all returned and unprepared
 * before the handle closes. */
static void test_input_sysex_realtime(void)
{
    fake_reset(); device_plug(20, "nanoKONTROL2");
    Heard heard; heard_init(&heard);
    RadioSMidiInput *input = open_device(20, &heard);
    g_assert_cmpuint(queued_buffers(20), ==, SYSEX_BUFFERS);
    for (guint i = 0; i < 300; ++i) { g_assert_cmpuint(device_send(20, 0xF8), ==, 1); g_assert_cmpuint(device_send(20, 0xFE), ==, 1); }
    device_send(20, 0xC0u | 5u << 8); device_send(20, 0xD0u | 9u << 8); device_send(20, 0xA0u | 1u << 8 | 2u << 16);
    device_send(20, 0xF2u | 0x10u << 16); device_send(20, 0xFFu);
    const guint8 identity[] = {0xF0, 0x7E, 0x7F, 0x06, 0x01, 0xF7}, cut[] = {0xF0, 0x43, 0x10};
    g_assert_cmpuint(device_send_sysex(20, identity, sizeof identity, FALSE), ==, 1);
    g_assert_cmpuint(device_send_sysex(20, cut, sizeof cut, TRUE), ==, 1);
    g_assert_cmpuint(device_send_cc(20, 7, 64), ==, 1);
    g_assert_cmpint(pump_until(heard_keys, &heard, 2), >=, 0);
    pump(0.3);
    g_assert_cmpuint(heard.keys->len, ==, 1); g_assert_cmpstr(g_ptr_array_index(heard.keys, 0), ==, "cc:1:7");
    g_assert_cmpfloat(fabs(g_array_index(heard.values, gdouble, 0) - 64 / 127.0), <, 1e-9);
    g_assert_cmpuint(queued_buffers(20), ==, SYSEX_BUFFERS);
    for (guint i = 0; i < 3 * SYSEX_BUFFERS; ++i) { g_assert_cmpuint(device_send_sysex(20, identity, sizeof identity, FALSE), <=, 1); pump(0.01); }
    pump(0.2);
    g_assert_cmpuint(queued_buffers(20), ==, SYSEX_BUFFERS);
    radios_midi_input_close(input);
    fake_done(); /* no midiInClose with queued or prepared buffers */
    heard_clear(&heard);
}

/* A port id from a listing still opens its device after another device left
 * and the indexes moved; once the device itself is gone the id opens
 * nothing; and a list that changes between the listing and the open is
 * caught by checking the opened handle. */
static void test_input_index_shift(void)
{
    fake_reset(); device_plug(20, "nanoKONTROL2"); device_plug(24, "Launchpad");
    Heard heard; heard_init(&heard); GError *error = NULL;
    gchar *launchpad = fake_port_id(24); g_assert_true(g_str_has_prefix(launchpad, "1:"));
    device_unplug(20); /* Launchpad moves to index 0 */
    RadioSMidiInput *input = radios_midi_input_open_full(launchpad, NULL, heard_event, heard_notice, &heard, &error);
    g_assert_no_error(error); g_assert_nonnull(input);
    g_assert_cmpstr(radios_midi_input_port(input), ==, launchpad);
    g_assert_cmpuint(device_send_cc(24, 7, 127), ==, 1);
    g_assert_cmpint(pump_until(heard_keys, &heard, 2), >=, 0);
    radios_midi_input_close(input);

    device_plug(20, "nanoKONTROL2"); /* Launchpad 0, nanoKONTROL2 1 */
    gchar *nano = fake_port_id(20); g_assert_true(g_str_has_prefix(nano, "1:"));
    device_unplug(20); device_plug(30, "Twister"); /* index 1 is another device now */
    g_assert_null(radios_midi_input_open_full(nano, NULL, heard_event, heard_notice, &heard, &error));
    g_assert_error(error, RADIOS_MIDI_ERROR, RADIOS_MIDI_ERROR_CONNECT);
    g_assert_nonnull(strstr(error->message, "cihaz bulunamadı")); g_clear_error(&error);
    settled();

    device_plug(20, "nanoKONTROL2"); /* Launchpad 0, Twister 1, nanoKONTROL2 2 */
    g_free(nano); nano = fake_port_id(20); g_assert_true(g_str_has_prefix(nano, "2:"));
    /* Launchpad leaves during the open: index 2 names nothing, the second round finds the device at 1. */
    g_mutex_lock(&fake_lock); fake_unplug_on_open = 24; g_mutex_unlock(&fake_lock);
    input = radios_midi_input_open_full(nano, NULL, heard_event, heard_notice, &heard, &error);
    g_assert_no_error(error); g_assert_nonnull(input);
    g_assert_cmpuint(device_send_cc(20, 8, 1), ==, 1); g_assert_cmpuint(device_send_cc(30, 8, 1), ==, 0);
    radios_midi_input_close(input);

    device_plug(24, "Launchpad"); device_plug(40, "Fighter"); /* Twister 0, nanoKONTROL2 1, Launchpad 2, Fighter 3 */
    gchar *launchpad_again = fake_port_id(24); g_assert_true(g_str_has_prefix(launchpad_again, "2:"));
    /* Twister leaves during the open: index 2 is Fighter now; the opened handle is not the device, so it is
     * closed and Launchpad opened at its new index. */
    const gint connects = fake_count(&fake_connects);
    g_mutex_lock(&fake_lock); fake_unplug_on_open = 30; g_mutex_unlock(&fake_lock);
    input = radios_midi_input_open_full(launchpad_again, NULL, heard_event, heard_notice, &heard, &error);
    g_assert_no_error(error); g_assert_nonnull(input);
    g_assert_cmpint(fake_count(&fake_connects), ==, connects + 2);
    g_assert_cmpuint(open_handles(), ==, 1);
    g_assert_cmpuint(device_send_cc(24, 9, 1), ==, 1); g_assert_cmpuint(device_send_cc(40, 9, 1), ==, 0);
    radios_midi_input_close(input);
    pump(0.2);
    for (guint i = 0; i < heard.keys->len; ++i) g_test_message("heard %s", (gchar *)g_ptr_array_index(heard.keys, i));
    fake_done();
    g_free(launchpad); g_free(nano); g_free(launchpad_again); heard_clear(&heard);
}

/* Identical names are told apart by socket (interface path), whatever the
 * plug order; WinMM's 31-character names and non-ASCII names stay valid
 * UTF-8; drivers without an interface path are numbered in list order. */
static void test_input_duplicate_names(void)
{
    fake_reset(); device_plug(30, "X"); device_plug(20, "X");
    gchar *first = fake_port_name(20), *second = fake_port_name(30);
    g_assert_cmpstr(first, ==, "X MIDI 1"); g_assert_cmpstr(second, ==, "X MIDI 1 #2");
    g_free(first); g_free(second);
    device_unplug(30); device_unplug(20); device_plug(20, "X"); device_plug(30, "X");
    first = fake_port_name(20); second = fake_port_name(30);
    g_assert_cmpstr(first, ==, "X MIDI 1"); g_assert_cmpstr(second, ==, "X MIDI 1 #2");
    g_free(first); g_free(second);
    /* A device really called "X MIDI 1 #2" keeps its name; the copy takes the next number. */
    device_plug_named(10, "X MIDI 1 #2");
    gchar *literal = fake_port_name(10); first = fake_port_name(20); second = fake_port_name(30);
    g_assert_cmpstr(literal, ==, "X MIDI 1 #2"); g_assert_cmpstr(first, ==, "X MIDI 1"); g_assert_cmpstr(second, ==, "X MIDI 1 #3");
    g_free(literal); g_free(first); g_free(second);
    /* The duplicate opens as itself. */
    Heard heard; heard_init(&heard);
    RadioSMidiInput *input = open_device(30, &heard);
    g_assert_cmpuint(device_send_cc(30, 1, 1), ==, 1); g_assert_cmpuint(device_send_cc(20, 1, 1), ==, 0);
    radios_midi_input_close(input);
    device_plug_named(50, "Denetleyici ŞĞÜİÖÇ ile çok uzun bir MIDI adı");
    gchar *long_name = fake_port_name(50);
    g_assert_true(g_utf8_validate(long_name, -1, NULL)); g_assert_cmpint(g_utf8_strlen(long_name, -1), ==, 31);
    g_assert_true(g_str_has_prefix(long_name, "Denetleyici ŞĞÜİÖÇ ile"));
    g_free(long_name);
    fake_done(); fake_reset(); fake_set(&fake_no_interface, TRUE); device_plug(60, "Y"); device_plug(61, "Y");
    first = fake_port_name(60); second = fake_port_name(61);
    g_assert_cmpstr(first, ==, "Y MIDI 1"); g_assert_cmpstr(second, ==, "Y MIDI 1 #2");
    g_free(first); g_free(second);
    input = open_device(61, &heard);
    g_assert_cmpuint(device_send_cc(61, 2, 1), ==, 1); g_assert_cmpuint(device_send_cc(60, 2, 1), ==, 0);
    radios_midi_input_close(input);
    fake_done(); heard_clear(&heard);
}

/* Without device notifications a new device is still announced, from the
 * periodic list check. */
static void test_input_polling_port(void)
{
    fake_reset(); fake_set(&fake_notices_refused, TRUE); device_plug(20, "nanoKONTROL2");
    Heard heard; heard_init(&heard);
    RadioSMidiInput *input = open_device(20, &heard);
    heard.address = 24; device_plug(24, "Launchpad");
    const gint64 elapsed = pump_until(heard_port, &heard, 3);
    g_assert_cmpint(elapsed, >=, 0); g_assert_cmpint(elapsed, <, 2000000);
    g_assert_true(heard.listed_at_port); g_assert_true(radios_midi_input_alive(input));
    radios_midi_input_close(input);
    fake_done(); heard_clear(&heard);
}

/* WinMM lists a plugged device only some time after Plug and Play announces
 * it: PORT waits for the list (a reconnect started at once would find
 * nothing) and still comes well within a second of it. */
static void test_input_list_lag(void)
{
    fake_reset(); device_plug(20, "nanoKONTROL2");
    Heard heard; heard_init(&heard);
    RadioSMidiInput *input = open_device(20, &heard);
    g_mutex_lock(&fake_lock); fake_list_lag_us = 600000; g_mutex_unlock(&fake_lock);
    heard.address = 24; device_plug(24, "Launchpad");
    const gint64 elapsed = pump_until(heard_port, &heard, 3);
    g_test_message("PORT %.0f ms after the arrival, 600 ms list lag", elapsed / 1000.0);
    g_assert_cmpint(elapsed, >=, 550000); g_assert_cmpint(elapsed, <, 1600000);
    g_assert_true(heard.listed_at_port);
    radios_midi_input_close(input);
    fake_done(); heard_clear(&heard);
}

typedef struct { const gchar *id; GMainContext *context; Heard *heard; RadioSMidiInput *input; } OpenJob;
static gpointer open_job(gpointer data)
{
    OpenJob *job = data; GError *error = NULL;
    job->input = radios_midi_input_open_full(job->id, job->context, heard_event, heard_notice, job->heard, &error);
    g_assert_no_error(error);
    return NULL;
}
/* Opened from another thread for a given main context (as the window's
 * service does from its jobs): every kind of message arrives decoded, in
 * order, on that context only and on the thread iterating it; nothing is
 * delivered after close, even what was already queued. The plain open
 * delivers on the thread-default context of its caller. */
static void test_input_delivery(void)
{
    fake_reset(); device_plug(20, "nanoKONTROL2");
    Heard heard; heard_init(&heard); heard.thread = g_thread_self();
    GMainContext *context = g_main_context_new(); gchar *id = fake_port_id(20);
    OpenJob job = {id, context, &heard, NULL};
    g_thread_join(g_thread_new("midi-open", open_job, &job));
    g_assert_nonnull(job.input);
    const DWORD messages[] = {0x7F2490, 0x402480, 0x002490, 0x4007B2, 0x3F07B2, 0x7F7FEF, 0x4000E0, 0x3F7FE0};
    const gchar *keys[] = {"note:1:36", "note:1:36", "note:1:36", "cc:3:7", "cc:3:7", "pitch:16", "pitch:1", "pitch:1"};
    const gdouble values[] = {1, 0, 0, 64 / 127.0, 63 / 127.0, 1, 8192 / 16383.0, 8191 / 16383.0};
    const gboolean pressed[] = {TRUE, FALSE, FALSE, TRUE, FALSE, TRUE, TRUE, FALSE};
    for (guint i = 0; i < G_N_ELEMENTS(messages); ++i) g_assert_cmpuint(device_send(20, messages[i]), ==, 1);
    pump(0.3); /* the default context gets nothing */
    g_assert_cmpuint(heard.keys->len, ==, 0);
    HeardCount wait = {&heard, G_N_ELEMENTS(messages)};
    g_assert_cmpint(pump_context_until(context, heard_count, &wait, 2), >=, 0);
    for (guint i = 0; i < G_N_ELEMENTS(messages); ++i) {
        g_assert_cmpstr(g_ptr_array_index(heard.keys, i), ==, keys[i]);
        g_assert_cmpfloat(fabs(g_array_index(heard.values, gdouble, i) - values[i]), <, 1e-12);
        g_assert_cmpint(g_array_index(heard.pressed, gboolean, i), ==, pressed[i]);
    }
    /* A fader sweep: every value, in order. */
    for (guint i = 0; i < 500; ++i) g_assert_cmpuint(device_send_cc(20, 1, i % 128), ==, 1);
    wait.count = G_N_ELEMENTS(messages) + 500;
    g_assert_cmpint(pump_context_until(context, heard_count, &wait, 3), >=, 0);
    for (guint i = 0; i < 500; ++i)
        g_assert_cmpfloat(fabs(g_array_index(heard.values, gdouble, G_N_ELEMENTS(messages) + i) - (i % 128) / 127.0), <, 1e-12);
    /* Queued on the context but not run before close: never delivered. */
    const guint before = heard.keys->len;
    for (guint i = 0; i < 5; ++i) device_send_cc(20, 2, 100);
    pump(0.2);
    g_assert_true(g_main_context_pending(context));
    radios_midi_input_close(job.input);
    pump_context(context, 0.2);
    g_assert_cmpuint(heard.keys->len, ==, before);
    /* radios_midi_input_open: the caller's thread-default context. */
    GMainContext *other = g_main_context_new();
    g_main_context_push_thread_default(other);
    GError *error = NULL; RadioSMidiInput *input = radios_midi_input_open(id, heard_event, &heard, &error);
    g_main_context_pop_thread_default(other);
    g_assert_no_error(error); g_assert_nonnull(input);
    g_assert_cmpuint(device_send_note(20, 40, 100), ==, 1);
    pump(0.2); pump_context(context, 0.1);
    g_assert_cmpuint(heard.keys->len, ==, before);
    wait.count = before + 1;
    g_assert_cmpint(pump_context_until(other, heard_count, &wait, 2), >=, 0);
    g_assert_cmpstr(g_ptr_array_index(heard.keys, before), ==, "note:1:40");
    radios_midi_input_close(input);
    g_main_context_unref(other); g_main_context_unref(context); g_free(id);
    fake_done(); heard_clear(&heard);
}

static gboolean slow_call_running(gpointer data) { (void)data; return fake_count(&fake_busy_calls) > 0; }
/* Closing (on the GTK thread) while the worker is inside a WinMM listing that
 * stalls (USB enumeration, the Windows MIDI service starting): the device and
 * the notification are released at once and the device opens again right
 * away; close does not wait for the listing, the worker finishes it and frees
 * the input on its own. */
static void test_input_close_during_slow_listing(void)
{
    fake_reset(); device_plug(20, "nanoKONTROL2");
    Heard heard; heard_init(&heard);
    RadioSMidiInput *input = open_device(20, &heard);
    fake_set_delay(1500000);
    device_noise(); /* a device notification: the worker looks at the list at once */
    g_assert_cmpint(pump_until(slow_call_running, NULL, 3), >=, 0);
    const gint64 before = g_get_monotonic_time();
    radios_midi_input_close(input);
    const gint64 took = g_get_monotonic_time() - before;
    g_test_message("close took %.1f ms with the worker in a 1.5 s listing", took / 1000.0);
    g_assert_cmpint(took, <, 500000);
    g_assert_cmpuint(open_handles(), ==, 0);
    g_mutex_lock(&fake_lock); g_assert_cmpuint(fake_registrations->len, ==, 0); g_mutex_unlock(&fake_lock);
    fake_set_delay(0);
    RadioSMidiInput *again = open_device(20, &heard);
    g_assert_cmpuint(device_send_cc(20, 7, 5), ==, 1);
    HeardCount one = {&heard, 1}; g_assert_cmpint(pump_until(heard_count, &one, 2), >=, 0);
    radios_midi_input_close(again);
    fake_done(); /* the first worker gets out of its listing and frees its input */
    g_assert_cmpuint(heard.keys->len, ==, 1); g_assert_cmpint(heard.lost, ==, 0);
    heard_clear(&heard);
}

/* Open failures: ids that are no port of this listing are refused without a
 * system call (also a Linux "client:port" address); a device held by another
 * application is BUSY, also by another RadioS input (exclusive); a driver
 * error and a device that is gone are CONNECT. Nothing stays held. */
static void test_input_open_errors(void)
{
    fake_reset(); device_plug(20, "nanoKONTROL2");
    gchar *id = fake_port_id(20); GError *error = NULL; Heard heard; heard_init(&heard);
    const gint calls = fake_count(&fake_calls);
    const gchar *bad[] = {"bad", "20:0", "128:0", "0:1234567", "0:ABCDEF01", "12345:00000000", "", NULL};
    for (guint i = 0; bad[i]; ++i) {
        g_assert_null(radios_midi_input_open_full(bad[i], NULL, heard_event, heard_notice, &heard, &error));
        g_assert_error(error, RADIOS_MIDI_ERROR, RADIOS_MIDI_ERROR_PORT); g_clear_error(&error);
    }
    g_assert_null(radios_midi_input_open_full(id, NULL, NULL, heard_notice, &heard, &error));
    g_assert_error(error, RADIOS_MIDI_ERROR, RADIOS_MIDI_ERROR_PORT); g_clear_error(&error);
    g_assert_cmpint(fake_count(&fake_calls), ==, calls);

    fake_set(&fake_busy, TRUE);
    g_assert_null(radios_midi_input_open_full(id, NULL, heard_event, heard_notice, &heard, &error));
    g_assert_error(error, RADIOS_MIDI_ERROR, RADIOS_MIDI_ERROR_BUSY);
    g_assert_cmpstr(error->message, ==, "MIDI cihazına bağlanılamadı (nanoKONTROL2 MIDI 1): cihaz başka bir uygulama tarafından kullanılıyor");
    g_clear_error(&error); settled();
    fake_set(&fake_busy, FALSE);
    RadioSMidiInput *input = radios_midi_input_open_full(id, NULL, heard_event, heard_notice, &heard, &error);
    g_assert_no_error(error); g_assert_nonnull(input);
    g_assert_null(radios_midi_input_open_full(id, NULL, heard_event, heard_notice, &heard, &error));
    g_assert_error(error, RADIOS_MIDI_ERROR, RADIOS_MIDI_ERROR_BUSY); g_clear_error(&error);
    radios_midi_input_close(input);
    input = radios_midi_input_open_full(id, NULL, heard_event, heard_notice, &heard, &error);
    g_assert_no_error(error); g_assert_nonnull(input); radios_midi_input_close(input);

    fake_set(&fake_connect_fails, TRUE);
    g_assert_null(radios_midi_input_open_full(id, NULL, heard_event, heard_notice, &heard, &error));
    g_assert_error(error, RADIOS_MIDI_ERROR, RADIOS_MIDI_ERROR_CONNECT);
    g_assert_cmpstr(error->message, ==, "MIDI cihazına bağlanılamadı (nanoKONTROL2 MIDI 1): Sahte WinMM hatası 1");
    g_clear_error(&error); fake_set(&fake_connect_fails, FALSE);

    device_unplug(20);
    g_assert_null(radios_midi_input_open_full(id, NULL, heard_event, heard_notice, &heard, &error));
    g_assert_error(error, RADIOS_MIDI_ERROR, RADIOS_MIDI_ERROR_CONNECT);
    g_assert_nonnull(strstr(error->message, "cihaz bulunamadı")); g_clear_error(&error);
    pump(0.1);
    g_assert_cmpuint(heard.keys->len, ==, 0); g_assert_cmpint(heard.lost, ==, 0);
    fake_done(); g_free(id); heard_clear(&heard);
}

/* ---- the window's MIDI service ------------------------------------------- */
typedef struct {
    GtkWidget *window, *page, *play, *sync, *volume, *maps, *verify;
    RadioSViewContext ctx; gint64 bound; guint beat; gint plays; gchar *notice;
} Ui;
static GtkWidget *named(GtkWidget *widget, const gchar *name)
{
    if (g_strcmp0(gtk_widget_get_name(widget), name) == 0) return widget;
    if (!GTK_IS_CONTAINER(widget)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(widget)); GtkWidget *found = NULL;
    for (GList *item = children; item && !found; item = item->next) found = named(item->data, name);
    g_list_free(children); return found;
}
static const gchar *status_text(Ui *ui) { return gtk_label_get_text(GTK_LABEL(named(ui->page, "preferences-midi-status"))); }
static gboolean status_connected(gpointer data) { return g_str_equal(status_text(data), "Bağlı: nanoKONTROL2 MIDI 1"); }
static gboolean status_unplugged(gpointer data)
{ return g_str_equal(status_text(data), "nanoKONTROL2 MIDI 1 takılı değil. Denetleyiciyi takın; bağlantı kendiliğinden kurulur."); }
static gboolean status_busy(gpointer data)
{ return g_str_equal(status_text(data), "nanoKONTROL2 MIDI 1 başka bir uygulama tarafından kullanılıyor. Diğer uygulamayı kapatın; bağlantı kendiliğinden kurulur."); }
static gboolean device_listed(gpointer data)
{
    Ui *ui = data; GtkTreeModel *model = gtk_combo_box_get_model(GTK_COMBO_BOX(named(ui->page, "preferences-midi-device")));
    GtkTreeIter iter; gboolean found = FALSE;
    for (gboolean ok = gtk_tree_model_get_iter_first(model, &iter); ok && !found; ok = gtk_tree_model_iter_next(model, &iter)) {
        gchar *text = NULL; gtk_tree_model_get(model, &iter, 0, &text, -1); found = g_strcmp0(text, "nanoKONTROL2 MIDI 1") == 0; g_free(text);
    }
    return found;
}
/* The keys list row of `label`; TRUE with *selected when it is the selected row. */
static gboolean key_row(Ui *ui, const gchar *label, GtkTreeIter *row, gboolean *selected)
{
    GtkTreeView *tree = GTK_TREE_VIEW(named(ui->page, "preferences-midi-keys")); GtkTreeModel *model = gtk_tree_view_get_model(tree);
    gboolean found = FALSE;
    for (gboolean ok = gtk_tree_model_get_iter_first(model, row); ok && !found;) {
        gchar *text = NULL; gtk_tree_model_get(model, row, 1, &text, -1); found = g_strcmp0(text, label) == 0; g_free(text);
        if (!found) ok = gtk_tree_model_iter_next(model, row);
    }
    if (found && selected) *selected = gtk_tree_selection_iter_is_selected(gtk_tree_view_get_selection(tree), row);
    return found;
}
typedef struct { Ui *ui; const gchar *label; } KeyWait;
static gboolean key_listed(gpointer data) { KeyWait *wait = data; GtkTreeIter row; return key_row(wait->ui, wait->label, &row, NULL); }
static gboolean key_selected(gpointer data)
{ KeyWait *wait = data; GtkTreeIter row; gboolean selected = FALSE; return key_row(wait->ui, wait->label, &row, &selected) && selected; }
static gchar *key_assignment(Ui *ui, const gchar *label)
{
    GtkTreeIter row; gchar *text = NULL;
    if (key_row(ui, label, &row, NULL)) gtk_tree_model_get(gtk_tree_view_get_model(GTK_TREE_VIEW(named(ui->page, "preferences-midi-keys"))), &row, 2, &text, -1);
    return text;
}
static void ui_played(GtkButton *button, gpointer data) { (void)button; ++((Ui *)data)->plays; }
static void ui_notified(gpointer data, const gchar *message) { Ui *ui = data; g_free(ui->notice); ui->notice = g_strdup(message); }
/* Learning a control selects it in the list while the page is on screen:
 * the test window and the check dialog are shown off screen, without the
 * taskbar and without taking the focus. */
static gboolean off_screen(GSignalInvocationHint *hint, guint count, const GValue *values, gpointer data)
{
    (void)hint; (void)count; (void)data; GObject *object = g_value_get_object(&values[0]);
    if (GTK_IS_WINDOW(object) && gtk_window_get_window_type(GTK_WINDOW(object)) == GTK_WINDOW_TOPLEVEL) {
        gtk_window_move(GTK_WINDOW(object), -32000 + 7, -32000 + 11); gtk_window_set_skip_taskbar_hint(GTK_WINDOW(object), TRUE);
        gtk_window_set_accept_focus(GTK_WINDOW(object), FALSE); gtk_window_set_focus_on_map(GTK_WINDOW(object), FALSE);
    }
    return TRUE;
}
static void ui_open(Ui *ui)
{
    memset(ui, 0, sizeof *ui);
    ui->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); ui->ctx.parent = GTK_WINDOW(ui->window);
    ui->ctx.notify = ui_notified; ui->ctx.notify_data = ui;
    ui->bound = g_get_monotonic_time();
    radios_midi_ui_bind(&ui->ctx, ui->window);
    const gint64 before = g_get_monotonic_time();
    ui->page = radios_midi_ui_settings_new(&ui->ctx);
    /* No WinMM call on the GTK thread, even with a slow system (the first page
     * of a process also pays GTK's one-time class and theme set-up). */
    g_test_message("settings page built in %.1f ms", (g_get_monotonic_time() - before) / 1000.0);
    no_gtk_stalls();
    /* Player 1's controls, as the player cards name them, and the Technical page's MIDI fields. */
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    ui->play = gtk_button_new_with_label("Oynat"); gtk_widget_set_name(ui->play, "deck-0-play");
    g_signal_connect(ui->play, "clicked", G_CALLBACK(ui_played), ui);
    ui->sync = gtk_toggle_button_new_with_label("SYNC"); gtk_widget_set_name(ui->sync, "deck-0-sync");
    ui->volume = gtk_scale_new_with_range(GTK_ORIENTATION_VERTICAL, -60, 0, 0.1); gtk_widget_set_name(ui->volume, "deck-0-volume");
    ui->maps = gtk_combo_box_text_new(); gtk_widget_set_name(ui->maps, "task-midi-profile");
    ui->verify = gtk_button_new_with_label("Denetleyiciyi doğrula"); gtk_widget_set_name(ui->verify, "task-midi-verify");
    gtk_box_pack_start(GTK_BOX(box), ui->page, TRUE, TRUE, 0);
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_box_pack_start(GTK_BOX(row), ui->play, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(row), ui->sync, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), ui->volume, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(row), ui->maps, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), ui->verify, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(box), row, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(ui->window), box);
    radios_midi_ui_bind_technical(&ui->ctx, ui->maps, ui->verify);
    gap_reset(); ui->beat = g_timeout_add(5, heartbeat, NULL);
}
static void ui_select(Ui *ui, const gchar *device)
{ g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(named(ui->page, "preferences-midi-device")), device)); }
static void ui_close(Ui *ui)
{
    g_source_remove(ui->beat);
    gtk_widget_destroy(ui->window);
    settled(); no_gtk_stalls();
    g_free(ui->notice);
}

/* The settings page after unplug, replug and "Yenile" (radios-f1, R8.4):
 * "takılı değil" at once, a replug (another socket) reconnects without
 * waiting for the 5 s retry, and "Yenile" makes a new connection, the old
 * one no longer delivering. */
static void test_ui_unplug_replug_refresh(void)
{
    if (!display_available) { g_test_skip("GTK display required"); return; }
    /* Dead handles keep answering: "takılı değil" in less than a second can
     * only come from the device notification. */
    fake_reset(); fake_set(&fake_stale_answers, TRUE); device_plug(20, "nanoKONTROL2");
    Ui ui; ui_open(&ui);
    g_assert_cmpint(pump_until(device_listed, &ui, 2), >=, 0);
    ui_select(&ui, "nanoKONTROL2 MIDI 1");
    g_assert_cmpint(pump_until(status_connected, &ui, 2), >=, 0);
    g_assert_cmpuint(device_send_cc(20, 7, 100), ==, 1);
    KeyWait cc7 = {&ui, "Kontrol (CC) 7 · Kanal 1"}; g_assert_cmpint(pump_until(key_listed, &cc7, 2), >=, 0);

    device_unplug(20);
    const gint64 lost = pump_until(status_unplugged, &ui, 2);
    g_test_message("\"takılı değil\" %.0f ms after the removal", lost / 1000.0);
    g_assert_cmpint(lost, >=, 0); g_assert_cmpint(lost, <, 900000);
    g_assert_cmpuint(device_send_cc(20, 7, 90), ==, 0);
    /* Replug between two 5 s retries, into another socket: the next retry is
     * at least 2 s away, so a reconnect sooner comes from the device arrival. */
    while ((g_get_monotonic_time() - ui.bound) % (5 * G_USEC_PER_SEC) < 2500000 ||
           (g_get_monotonic_time() - ui.bound) % (5 * G_USEC_PER_SEC) > 3000000) pump(0.01);
    device_plug(24, "nanoKONTROL2");
    const gint64 back = pump_until(status_connected, &ui, 3);
    g_test_message("reconnected %.0f ms after the replug", back / 1000.0);
    g_assert_cmpint(back, >=, 0); g_assert_cmpint(back, <, 1900000);
    g_assert_cmpuint(device_send_cc(24, 8, 100), ==, 1);
    KeyWait cc8 = {&ui, "Kontrol (CC) 8 · Kanal 1"}; g_assert_cmpint(pump_until(key_listed, &cc8, 2), >=, 0);

    /* "Yenile" replaces even a connection that looks alive. */
    const gint connects = fake_count(&fake_connects);
    gtk_button_clicked(GTK_BUTTON(named(ui.page, "preferences-midi-refresh")));
    pump(0.3);
    g_assert_cmpint(pump_until(status_connected, &ui, 2), >=, 0);
    g_assert_cmpint(fake_count(&fake_connects), ==, connects + 1);
    g_assert_cmpuint(device_send_cc(24, 9, 100), ==, 1); /* only the new connection */
    KeyWait cc9 = {&ui, "Kontrol (CC) 9 · Kanal 1"}; g_assert_cmpint(pump_until(key_listed, &cc9, 2), >=, 0);
    g_test_message("worst main loop gap %.1f ms (machine load included)", worst_gap / 1000.0);
    ui_close(&ui);
    fake_done();
}

/* Unplugged and plugged back into the same socket while the lost input is
 * kept to watch for devices: it has released the device (exclusive), so the
 * reconnect opens it within the arrival's list checks. */
static void test_ui_replug_same_port(void)
{
    if (!display_available) { g_test_skip("GTK display required"); return; }
    fake_reset(); fake_set(&fake_stale_answers, TRUE); device_plug(20, "nanoKONTROL2");
    Ui ui; ui_open(&ui);
    g_assert_cmpint(pump_until(device_listed, &ui, 2), >=, 0);
    ui_select(&ui, "nanoKONTROL2 MIDI 1");
    g_assert_cmpint(pump_until(status_connected, &ui, 2), >=, 0);
    device_unplug(20);
    g_assert_cmpint(pump_until(status_unplugged, &ui, 2), >=, 0);
    /* Between two 5 s retries: a reconnect within 1.9 s comes from the arrival. */
    while ((g_get_monotonic_time() - ui.bound) % (5 * G_USEC_PER_SEC) < 2500000 ||
           (g_get_monotonic_time() - ui.bound) % (5 * G_USEC_PER_SEC) > 3000000) pump(0.01);
    device_plug(20, "nanoKONTROL2");
    const gint64 back = pump_until(status_connected, &ui, 3);
    g_test_message("reconnected %.0f ms after the replug", back / 1000.0);
    g_assert_cmpint(back, >=, 0); g_assert_cmpint(back, <, 1900000);
    g_assert_cmpuint(device_send_cc(20, 10, 100), ==, 1);
    KeyWait cc10 = {&ui, "Kontrol (CC) 10 · Kanal 1"}; g_assert_cmpint(pump_until(key_listed, &cc10, 2), >=, 0);
    g_assert_cmpuint(open_handles(), ==, 1);
    ui_close(&ui);
    fake_done();
}

/* A configured controller that cannot be connected while every listing and
 * open takes 300 ms (e.g. during USB enumeration): the 5 s retries list and
 * open in the background and the window keeps running. */
static void test_ui_slow_enumeration(void)
{
    if (!display_available) { g_test_skip("GTK display required"); return; }
    fake_reset(); device_plug(20, "nanoKONTROL2"); fake_set_delay(300000);
    Ui ui; ui_open(&ui);
    g_assert_cmpint(pump_until(device_listed, &ui, 3), >=, 0);
    fake_set(&fake_connect_fails, TRUE);
    ui_select(&ui, "nanoKONTROL2 MIDI 1");
    g_assert_cmpint(pump_until(status_unplugged, &ui, 3), >=, 0);
    const gint opens = fake_count(&fake_opens);
    gap_reset(); pump(7.0);
    const gint retried = fake_count(&fake_opens) - opens;
    /* At least one 5 s retry: a listing, the open's own listing and the open,
     * all on job threads (ui_close checks that none ran on the GTK thread). */
    g_test_message("%d listings and opens in 7 s, worst main loop gap %.1f ms (machine load included)", retried, worst_gap / 1000.0);
    g_assert_cmpint(retried, >=, 3);
    g_assert_true(status_unplugged(&ui));
    ui_close(&ui);
    fake_done();
}

/* The window goes while a background connect is still opening the device:
 * the job's input is closed and nothing touches the freed service. */
static void test_ui_teardown_pending_job(void)
{
    if (!display_available) { g_test_skip("GTK display required"); return; }
    fake_reset(); device_plug(20, "nanoKONTROL2"); fake_set_delay(200000);
    Ui ui; ui_open(&ui);
    g_assert_cmpint(pump_until(device_listed, &ui, 3), >=, 0);
    ui_select(&ui, "nanoKONTROL2 MIDI 1");
    pump(0.3); /* listed; the device open is under way */
    g_source_remove(ui.beat);
    gtk_widget_destroy(ui.window);
    /* Controller input reaching the job's input before it is closed. */
    const gint64 until = g_get_monotonic_time() + 3 * G_USEC_PER_SEC;
    while (open_clients() > 0 && g_get_monotonic_time() < until) { (void)device_send_cc(20, 7, 100); pump(0.005); }
    g_assert_cmpuint(open_clients(), ==, 0);
    pump(0.2);
    g_assert_cmpuint(device_send_cc(20, 7, 100), ==, 0);
    no_gtk_stalls(); g_free(ui.notice);
    fake_done();
}

/* Another application holds the device (WinMM input is exclusive): the page
 * says so instead of "takılı değil", and the 5 s retry connects once the
 * device is free. */
static void test_ui_busy_status(void)
{
    if (!display_available) { g_test_skip("GTK display required"); return; }
    fake_reset(); device_plug(20, "nanoKONTROL2"); fake_set(&fake_busy, TRUE);
    Ui ui; ui_open(&ui);
    g_assert_cmpint(pump_until(device_listed, &ui, 2), >=, 0);
    ui_select(&ui, "nanoKONTROL2 MIDI 1");
    g_assert_cmpint(pump_until(status_busy, &ui, 2), >=, 0);
    gchar *tip = gtk_widget_get_tooltip_text(ui.maps);
    g_assert_cmpstr(tip, ==, "nanoKONTROL2 MIDI 1 başka bir uygulama tarafından kullanılıyor. Seçilen harita hemen etkinleşir ve kaydedilir.");
    g_free(tip);
    fake_set(&fake_busy, FALSE);
    const gint64 connected = pump_until(status_connected, &ui, 6);
    g_assert_cmpint(connected, >=, 0);
    /* Unplugged afterwards: plain "takılı değil" again. */
    device_unplug(20);
    g_assert_cmpint(pump_until(status_unplugged, &ui, 2), >=, 0);
    ui_close(&ui);
    fake_done();
}

typedef struct { Ui *ui; gint plays; } Plays;
static gboolean played(gpointer data) { Plays *wait = data; return wait->ui->plays >= wait->plays; }
typedef struct { GtkRange *range; gdouble value; } RangeWait;
static gboolean range_at(gpointer data) { RangeWait *wait = data; return fabs(gtk_range_get_value(wait->range) - wait->value) < 0.01; }
typedef struct { GtkToggleButton *toggle; gboolean active; } ToggleWait;
static gboolean toggled_to(gpointer data) { ToggleWait *wait = data; return gtk_toggle_button_get_active(wait->toggle) == wait->active; }
static void ui_assign(Ui *ui, const gchar *action)
{
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(named(ui->page, "preferences-midi-action")), action));
    GtkWidget *assign = named(ui->page, "preferences-midi-assign"); g_assert_true(gtk_widget_get_sensitive(assign));
    gtk_button_clicked(GTK_BUTTON(assign));
}
static gboolean check_open(gpointer data) { return g_object_get_data(G_OBJECT(((Ui *)data)->verify), "radios-midi-check") != NULL; }
/* Learning and mapping: each control pressed is listed and selected while the
 * page is on screen; a new map takes actions for the selected control; a
 * mapped control then runs its player action on its edge (buttons), follows
 * it (faders) or toggles (SYNC); the Technical page's check shows presses
 * without running them, and a map switched off runs nothing. */
static void test_ui_learn_and_map(void)
{
    if (!display_available) { g_test_skip("GTK display required"); return; }
    fake_reset(); device_plug(20, "nanoKONTROL2");
    const gulong hook = g_signal_add_emission_hook(g_signal_lookup("realize", GTK_TYPE_WIDGET), 0, off_screen, NULL, NULL);
    Ui ui; ui_open(&ui); gtk_widget_show_all(ui.window);
    g_assert_cmpint(pump_until(device_listed, &ui, 2), >=, 0);
    ui_select(&ui, "nanoKONTROL2 MIDI 1");
    g_assert_cmpint(pump_until(status_connected, &ui, 2), >=, 0);

    g_assert_cmpuint(device_send_cc(20, 7, 100), ==, 1);
    KeyWait cc7 = {&ui, "Kontrol (CC) 7 · Kanal 1"}; g_assert_cmpint(pump_until(key_selected, &cc7, 2), >=, 0);
    g_assert_cmpuint(device_send_note(20, 36, 127), ==, 1); g_assert_cmpuint(device_send_note(20, 36, 0), ==, 1);
    KeyWait note = {&ui, "Nota 36 · Kanal 1"}; g_assert_cmpint(pump_until(key_selected, &note, 2), >=, 0);
    g_assert_true(key_listed(&cc7));
    gchar *assignment = key_assignment(&ui, "Nota 36 · Kanal 1"); g_assert_cmpstr(assignment, ==, "Atanmamış"); g_free(assignment);
    g_assert_false(gtk_widget_get_sensitive(named(ui.page, "preferences-midi-assign"))); /* no map yet */

    gtk_button_clicked(GTK_BUTTON(named(ui.page, "preferences-midi-create")));
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(named(ui.page, "preferences-midi-active"))), ==, "Etkin harita: nanoKONTROL2 MIDI 1 haritası");
    g_assert_true(key_selected(&note));
    ui_assign(&ui, "deck1.play");
    assignment = key_assignment(&ui, "Nota 36 · Kanal 1"); g_assert_cmpstr(assignment, ==, "Oynatıcı 1 · Oynat"); g_free(assignment);
    g_assert_cmpstr(gtk_combo_box_get_active_id(GTK_COMBO_BOX(ui.maps)), ==, "nanoKONTROL2 MIDI 1 haritası");

    /* A button runs on its press edge only. */
    Plays one = {&ui, 1}, two = {&ui, 2};
    device_send_note(20, 36, 127); g_assert_cmpint(pump_until(played, &one, 2), >=, 0);
    device_send_note(20, 36, 0); device_send_note(20, 36, 0); pump(0.2); g_assert_cmpint(ui.plays, ==, 1);
    device_send_note(20, 36, 90); g_assert_cmpint(pump_until(played, &two, 2), >=, 0);
    device_send_note(20, 36, 0); pump(0.1);

    /* A fader follows its control: square law, full travel 0 dB. */
    GtkTreeIter row; g_assert_true(key_row(&ui, "Kontrol (CC) 7 · Kanal 1", &row, NULL));
    gtk_tree_selection_select_iter(gtk_tree_view_get_selection(GTK_TREE_VIEW(named(ui.page, "preferences-midi-keys"))), &row);
    ui_assign(&ui, "deck1.volume");
    device_send_cc(20, 7, 127); RangeWait full = {GTK_RANGE(ui.volume), 0}; g_assert_cmpint(pump_until(range_at, &full, 2), >=, 0);
    device_send_cc(20, 7, 64); RangeWait half = {GTK_RANGE(ui.volume), 40 * log10(64 / 127.0)}; g_assert_cmpint(pump_until(range_at, &half, 2), >=, 0);
    device_send_cc(20, 7, 0); RangeWait closed = {GTK_RANGE(ui.volume), -60}; g_assert_cmpint(pump_until(range_at, &closed, 2), >=, 0);

    /* Pitch bend learnt and mapped to SYNC, which toggles on each push past the centre. */
    device_send(20, 0x0000E0);
    KeyWait bend = {&ui, "Pitch bend · Kanal 1"}; g_assert_cmpint(pump_until(key_selected, &bend, 2), >=, 0);
    ui_assign(&ui, "deck1.sync");
    ToggleWait on = {GTK_TOGGLE_BUTTON(ui.sync), TRUE}, off = {GTK_TOGGLE_BUTTON(ui.sync), FALSE};
    device_send(20, 0x7F7FE0); g_assert_cmpint(pump_until(toggled_to, &on, 2), >=, 0);
    device_send(20, 0x7F7FE0); device_send(20, 0x0000E0); pump(0.2); g_assert_true(gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(ui.sync)));
    device_send(20, 0x7F7FE0); g_assert_cmpint(pump_until(toggled_to, &off, 2), >=, 0);

    /* The Technical page's check shows presses but never runs them. */
    gtk_button_clicked(GTK_BUTTON(ui.verify)); g_assert_true(check_open(&ui));
    GtkWidget *dialog = g_object_get_data(G_OBJECT(ui.verify), "radios-midi-check");
    device_send_note(20, 36, 127); device_send_note(20, 36, 0); pump(0.4);
    g_assert_cmpint(ui.plays, ==, 2);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(named(dialog, "technical-midi-check-count"))), ==, "Bu pencere açıldığından beri alınan MIDI mesajı: 2");
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(named(dialog, "technical-midi-check-last"))), ==, "Son kontrol: Nota 36 · Kanal 1 → Oynatıcı 1 · Oynat");
    gtk_widget_destroy(dialog); g_assert_false(check_open(&ui));
    Plays three = {&ui, 3};
    device_send_note(20, 36, 127); g_assert_cmpint(pump_until(played, &three, 2), >=, 0);
    device_send_note(20, 36, 0);

    /* No active map: nothing runs. */
    g_assert_true(gtk_combo_box_set_active_id(GTK_COMBO_BOX(ui.maps), ""));
    g_assert_cmpstr(ui.notice, ==, "MIDI haritası kapatıldı; denetleyici eylem çalıştırmaz.");
    device_send_note(20, 36, 127); device_send_note(20, 36, 0); pump(0.3);
    g_assert_cmpint(ui.plays, ==, 3);
    g_assert_cmpstr(gtk_label_get_text(GTK_LABEL(named(ui.page, "preferences-midi-active"))), ==,
        "Etkin harita yok. Harita oluşturun ya da mevcut bir haritayı yükleyin.");
    ui_close(&ui);
    g_signal_remove_emission_hook(g_signal_lookup("realize", GTK_TYPE_WIDGET), hook);
    fake_done();
}

int main(int argc, char **argv)
{
    fake_devices = g_ptr_array_new(); fake_handles = g_ptr_array_new(); fake_registrations = g_ptr_array_new();
    fake_gtk_thread = g_thread_self();
    display_available = gtk_init_check(&argc, &argv);
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/midi-hotplug/input-announcements", test_input_announcements);
    g_test_add_func("/midi-hotplug/input-liveness-without-announcements", test_input_liveness_without_announcements);
    g_test_add_func("/midi-hotplug/input-liveness-from-list", test_input_liveness_from_list);
    g_test_add_func("/midi-hotplug/input-driver-close", test_input_driver_close);
    g_test_add_func("/midi-hotplug/input-sysex-realtime", test_input_sysex_realtime);
    g_test_add_func("/midi-hotplug/input-index-shift", test_input_index_shift);
    g_test_add_func("/midi-hotplug/input-duplicate-names", test_input_duplicate_names);
    g_test_add_func("/midi-hotplug/input-polling-port", test_input_polling_port);
    g_test_add_func("/midi-hotplug/input-list-lag", test_input_list_lag);
    g_test_add_func("/midi-hotplug/input-delivery", test_input_delivery);
    g_test_add_func("/midi-hotplug/input-close-during-slow-listing", test_input_close_during_slow_listing);
    g_test_add_func("/midi-hotplug/input-open-errors", test_input_open_errors);
    g_test_add_func("/midi-hotplug/ui-unplug-replug-refresh", test_ui_unplug_replug_refresh);
    g_test_add_func("/midi-hotplug/ui-replug-same-port", test_ui_replug_same_port);
    g_test_add_func("/midi-hotplug/ui-slow-enumeration", test_ui_slow_enumeration);
    g_test_add_func("/midi-hotplug/ui-teardown-pending-job", test_ui_teardown_pending_job);
    g_test_add_func("/midi-hotplug/ui-busy-status", test_ui_busy_status);
    g_test_add_func("/midi-hotplug/ui-learn-and-map", test_ui_learn_and_map);
    return g_test_run();
}
