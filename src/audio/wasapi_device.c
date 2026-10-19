/* Shared WASAPI layer; see wasapi_device.h for the contract.
 * This is the only translation unit of the audio module that defines the
 * COM GUIDs (initguid.h) and includes the Windows audio headers. */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <initguid.h>
#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <objidl.h>
#include <propidl.h>
#include <shlwapi.h>
#include <timeapi.h>
#include <winver.h>
#include "wasapi_device.h"
#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <string.h>

#define REFTIMES_PER_SECOND G_GINT64_CONSTANT(10000000)
#define FRONT_STEREO (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)

/* KSDATAFORMAT_SUBTYPE_IEEE_FLOAT / _PCM (mmreg.h only declares them for C++). */
static const GUID SUBTYPE_FLOAT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
static const GUID SUBTYPE_PCM = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

/* audioclientactivationparams.h is missing from mingw-w64: same layout as
 * the Windows SDK (AUDIOCLIENT_ACTIVATION_PARAMS, 12 bytes). */
typedef enum {
    RADIOS_ACTIVATION_TYPE_DEFAULT = 0,
    RADIOS_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
} RadioSActivationType;
typedef enum {
    RADIOS_PROCESS_LOOPBACK_INCLUDE_TREE = 0,
    RADIOS_PROCESS_LOOPBACK_EXCLUDE_TREE = 1
} RadioSProcessLoopbackMode;
typedef struct {
    DWORD TargetProcessId;
    RadioSProcessLoopbackMode ProcessLoopbackMode;
} RadioSProcessLoopbackParams;
typedef struct {
    RadioSActivationType ActivationType;
    union { RadioSProcessLoopbackParams ProcessLoopbackParams; };
} RadioSActivationParams;
G_STATIC_ASSERT(sizeof(RadioSActivationParams) == 12);
#define VIRTUAL_PROCESS_LOOPBACK L"VAD\\Process_Loopback"

/* ------------------------------------------------------------------ diagnostics */

static gint live_notify_clients, live_activation_handlers, live_streams, unreleased_clients;
static _Atomic guint64 notifications_dispatched;
static _Atomic gint injected_fault;
static _Atomic gint64 outage_until;

void radios_wasapi_debug_inject_fault(RadioSWasapiFault fault)
{ atomic_store(&injected_fault, (gint)fault); }
void radios_wasapi_debug_service_outage(guint milliseconds)
{ atomic_store(&outage_until, milliseconds ? g_get_monotonic_time() + (gint64)milliseconds * 1000 : 0); }
void radios_wasapi_debug_counters(RadioSWasapiDebugCounters *counters)
{
    if (!counters) return;
    counters->notify_clients = g_atomic_int_get(&live_notify_clients);
    counters->activation_handlers = g_atomic_int_get(&live_activation_handlers);
    counters->streams = g_atomic_int_get(&live_streams);
    counters->unreleased_clients = (guint)g_atomic_int_get(&unreleased_clients);
    counters->notifications = atomic_load(&notifications_dispatched);
}

/* ------------------------------------------------------------------ text */

void radios_wasapi_copy_text(gchar *destination, gsize size, const gchar *text)
{
    if (!destination || !size) return;
    if (!text) text = "";
    gsize length = strlen(text);
    if (length >= size) {
        length = size - 1;
        while (length && ((guchar)text[length] & 0xC0) == 0x80) --length;
    }
    memcpy(destination, text, length); destination[length] = '\0';
    const gchar *end = NULL;
    if (!g_utf8_validate(destination, -1, &end)) destination[end - destination] = '\0';
}
static void set_error(gchar *error, const gchar *format, ...) G_GNUC_PRINTF(2, 3);
static void set_error(gchar *error, const gchar *format, ...)
{
    if (!error) return;
    va_list args; va_start(args, format);
    gchar *text = g_strdup_vprintf(format, args);
    va_end(args);
    radios_wasapi_copy_text(error, RADIOS_WASAPI_ERROR_SIZE, text); g_free(text);
}
static gchar *utf8_from_wide(const WCHAR *text)
{ return text ? g_utf16_to_utf8((const gunichar2 *)text, -1, NULL, NULL, NULL) : NULL; }
static WCHAR *wide_from_utf8(const gchar *text)
{ return text ? (WCHAR *)g_utf8_to_utf16(text, -1, NULL, NULL, NULL) : NULL; }
static gulong code(HRESULT hr) { return (gulong)(guint32)hr; }

/* Operator texts: what happened and what to do. */
#define TEXT_SERVICE_DOWN "Windows ses hizmeti çalışmıyor; hizmet açılınca ses kendiliğinden bağlanır."
#define TEXT_MISSING_OUTPUT "Seçilen çıkış aygıtı bulunamadı; başka aygıta geçilmedi. Aygıtı takın ya da başka bir çıkış seçin."
#define TEXT_MISSING_INPUT "Seçilen giriş aygıtı bulunamadı; başka aygıta geçilmedi. Aygıtı takın ya da başka bir giriş seçin."
#define TEXT_NO_DEFAULT_OUTPUT "Windows'ta varsayılan çıkış aygıtı yok. Bir ses aygıtı bağlayın ya da çıkış seçin."
#define TEXT_NO_DEFAULT_INPUT "Windows'ta varsayılan giriş aygıtı yok. Bir mikrofon bağlayın ya da giriş seçin."
#define TEXT_NOT_INPUT "Bu aygıt bir giriş aygıtı değil; mikrofon için giriş ya da çıkış geri dinlemesi seçin."
#define TEXT_NOT_OUTPUT "Bu aygıt bir çıkış aygıtı değil; başka bir çıkış seçin."
#define TEXT_LOOPBACK_NOT_OUTPUT "Çıkış geri dinlemesi yalnız bir çıkış aygıtı için açılabilir; başka bir giriş seçin."
#define TEXT_REMOVED "Ses aygıtı çıkarıldı ya da devre dışı bırakıldı. Aygıt geri gelince yeniden bağlanır."
#define TEXT_IN_USE "Aygıt başka bir uygulamanın özel kullanımında. O uygulamayı kapatın; 5 saniyede bir yeniden denenir."
#define TEXT_PRIVACY "Mikrofon izni kapalı: Ayarlar › Gizlilik ve güvenlik › Mikrofon bölümünde masaüstü uygulamalarına izin verin."
#define TEXT_REOPEN "Windows ses akışını yeniledi; akış yeniden açılıyor."
#define TEXT_APP_DENIED "Bu uygulamanın sesi yakalanamıyor (erişim reddedildi)."
#define TEXT_APP_UNSUPPORTED "Bu Windows sürümü uygulama sesi yakalamayı desteklemiyor (Windows 10 sürüm 2004 veya üzeri gerekir)."

/* ------------------------------------------------------------------ threads, priority, power */

gboolean radios_wasapi_thread_enter(gchar error[RADIOS_WASAPI_ERROR_SIZE])
{
    HRESULT hr = CoInitializeEx(NULL, COINIT_MULTITHREADED);
    if (SUCCEEDED(hr)) return TRUE;
    if (hr == RPC_E_CHANGED_MODE)
        set_error(error, "Ses iş parçacığı Windows COM çok iş parçacıklı kipine geçemedi (0x%08lX).", code(hr));
    else set_error(error, "Windows COM başlatılamadı (0x%08lX). Uygulamayı yeniden başlatın.", code(hr));
    return FALSE;
}
void radios_wasapi_thread_leave(void) { CoUninitialize(); }

typedef struct { HANDLE task; gint previous; gboolean raised; } AudioPriority;
gpointer radios_wasapi_audio_priority_begin(void)
{
    AudioPriority *priority = g_new0(AudioPriority, 1); DWORD index = 0;
    priority->task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
    if (!priority->task) {
        priority->previous = GetThreadPriority(GetCurrentThread());
        priority->raised = priority->previous != THREAD_PRIORITY_ERROR_RETURN &&
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    }
    return priority;
}
void radios_wasapi_audio_priority_end(gpointer cookie)
{
    AudioPriority *priority = cookie;
    if (!priority) return;
    if (priority->task) AvRevertMmThreadCharacteristics(priority->task);
    else if (priority->raised) SetThreadPriority(GetCurrentThread(), priority->previous);
    g_free(priority);
}

static GMutex realtime_lock;
static guint realtime_count;
static gboolean realtime_timer;
static HANDLE realtime_power;
static WCHAR realtime_reason[] = L"RadioS yayın ses motoru çalışıyor";
void radios_wasapi_realtime_acquire(void)
{
    g_mutex_lock(&realtime_lock);
    if (realtime_count++ == 0) {
        realtime_timer = timeBeginPeriod(1) == TIMERR_NOERROR;
        PROCESS_POWER_THROTTLING_STATE state = {PROCESS_POWER_THROTTLING_CURRENT_VERSION,
            PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, 0};
        (void)SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof state);
        REASON_CONTEXT reason; memset(&reason, 0, sizeof reason);
        reason.Version = POWER_REQUEST_CONTEXT_VERSION; reason.Flags = POWER_REQUEST_CONTEXT_SIMPLE_STRING;
        reason.Reason.SimpleReasonString = realtime_reason;
        realtime_power = PowerCreateRequest(&reason);
        if (realtime_power == INVALID_HANDLE_VALUE) realtime_power = NULL;
        if (realtime_power && !PowerSetRequest(realtime_power, PowerRequestSystemRequired)) {
            CloseHandle(realtime_power); realtime_power = NULL;
        }
    }
    g_mutex_unlock(&realtime_lock);
}
void radios_wasapi_realtime_release(void)
{
    g_mutex_lock(&realtime_lock);
    if (realtime_count && --realtime_count == 0) {
        if (realtime_power) {
            PowerClearRequest(realtime_power, PowerRequestSystemRequired); CloseHandle(realtime_power); realtime_power = NULL;
        }
        PROCESS_POWER_THROTTLING_STATE state = {PROCESS_POWER_THROTTLING_CURRENT_VERSION, 0, 0};
        (void)SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof state);
        if (realtime_timer) timeEndPeriod(1);
        realtime_timer = FALSE;
    }
    g_mutex_unlock(&realtime_lock);
}

static SC_HANDLE audio_service;
static gpointer audio_service_open(gpointer unused)
{
    (void)unused;
    SC_HANDLE manager = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (manager) { audio_service = OpenServiceW(manager, L"Audiosrv", SERVICE_QUERY_STATUS); CloseServiceHandle(manager); }
    return NULL;
}
gboolean radios_wasapi_service_running(void)
{
    static GOnce once = G_ONCE_INIT;
    g_once(&once, audio_service_open, NULL);
    gint64 outage = atomic_load(&outage_until);
    if (outage && g_get_monotonic_time() < outage) return FALSE;
    SERVICE_STATUS status;
    if (!audio_service || !QueryServiceStatus(audio_service, &status)) return TRUE;
    return status.dwCurrentState == SERVICE_RUNNING;
}

/* ------------------------------------------------------------------ endpoints */

struct RadioSWasapiEnumerator { IMMDeviceEnumerator *object; };

RadioSWasapiEnumerator *radios_wasapi_enumerator_new(gchar error[RADIOS_WASAPI_ERROR_SIZE])
{
    IMMDeviceEnumerator *object = NULL;
    HRESULT hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void **)&object);
    if (FAILED(hr) || !object) {
        set_error(error, "Windows ses aygıtlarına erişilemedi (0x%08lX); birkaç saniye içinde yeniden denenecek.", code(hr));
        return NULL;
    }
    RadioSWasapiEnumerator *enumerator = g_new0(RadioSWasapiEnumerator, 1);
    enumerator->object = object; return enumerator;
}
void radios_wasapi_enumerator_free(RadioSWasapiEnumerator *enumerator)
{
    if (!enumerator) return;
    IMMDeviceEnumerator_Release(enumerator->object); g_free(enumerator);
}
static gchar *property_text(IPropertyStore *store, const PROPERTYKEY *key)
{
    PROPVARIANT value; PropVariantInit(&value); gchar *text = NULL;
    if (SUCCEEDED(IPropertyStore_GetValue(store, key, &value)) && value.vt == VT_LPWSTR && value.pwszVal && *value.pwszVal)
        text = utf8_from_wide(value.pwszVal);
    PropVariantClear(&value); return text;
}
static gboolean device_id(IMMDevice *device, gchar id[RADIOS_WASAPI_ID_SIZE])
{
    LPWSTR wide = NULL; gboolean valid = FALSE;
    id[0] = '\0';
    if (SUCCEEDED(IMMDevice_GetId(device, &wide)) && wide) {
        gchar *text = utf8_from_wide(wide);
        if (text && strlen(text) < RADIOS_WASAPI_ID_SIZE) { memcpy(id, text, strlen(text) + 1); valid = TRUE; }
        g_free(text);
    }
    CoTaskMemFree(wide); return valid;
}
static gboolean default_id(IMMDeviceEnumerator *object, EDataFlow flow, ERole role, gchar id[RADIOS_WASAPI_ID_SIZE])
{
    IMMDevice *device = NULL; id[0] = '\0';
    if (FAILED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(object, flow, role, &device)) || !device) return FALSE;
    gboolean found = device_id(device, id);
    IMMDevice_Release(device); return found;
}
static gboolean endpoint_fill(IMMDevice *device, RadioSWasapiEndpoint *out)
{
    memset(out, 0, sizeof *out);
    if (!device_id(device, out->id)) return FALSE;
    DWORD state = 0;
    if (SUCCEEDED(IMMDevice_GetState(device, &state))) out->state = state;
    IMMEndpoint *endpoint = NULL;
    if (SUCCEEDED(IMMDevice_QueryInterface(device, &IID_IMMEndpoint, (void **)&endpoint)) && endpoint) {
        EDataFlow flow = eRender;
        if (SUCCEEDED(IMMEndpoint_GetDataFlow(endpoint, &flow)))
            out->flow = flow == eCapture ? RADIOS_WASAPI_FLOW_CAPTURE : RADIOS_WASAPI_FLOW_RENDER;
        IMMEndpoint_Release(endpoint);
    }
    IPropertyStore *store = NULL;
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(device, STGM_READ, &store)) && store) {
        gchar *name = property_text(store, &PKEY_Device_FriendlyName);
        if (!name) name = property_text(store, &PKEY_Device_DeviceDesc);
        if (name) radios_wasapi_copy_text(out->name, sizeof out->name, name);
        g_free(name);
        PROPVARIANT value; PropVariantInit(&value);
        if (SUCCEEDED(IPropertyStore_GetValue(store, &PKEY_AudioEngine_DeviceFormat, &value)) && value.vt == VT_BLOB &&
            value.blob.pBlobData && value.blob.cbSize >= sizeof(WAVEFORMATEX)) {
            WAVEFORMATEX format; memcpy(&format, value.blob.pBlobData, sizeof format);
            out->channels = format.nChannels; out->rate = format.nSamplesPerSec;
        }
        PropVariantClear(&value);
        IPropertyStore_Release(store);
    }
    if (!out->name[0]) radios_wasapi_copy_text(out->name, sizeof out->name, out->id);
    return TRUE;
}
static RadioSWasapiStatus enumeration_failure(HRESULT hr, gchar *error)
{
    if (hr == AUDCLNT_E_SERVICE_NOT_RUNNING) { set_error(error, TEXT_SERVICE_DOWN); return RADIOS_WASAPI_SERVICE_DOWN; }
    set_error(error, "Windows ses aygıtları okunamadı (0x%08lX); liste birazdan yeniden okunacak.", code(hr));
    return RADIOS_WASAPI_FAILED;
}
RadioSWasapiStatus radios_wasapi_enumerator_list(RadioSWasapiEnumerator *enumerator, RadioSWasapiFlow flow,
    RadioSWasapiEndpoint *out, guint capacity, guint *count,
    gboolean (*cancelled)(gpointer data), gpointer data, gboolean *cancelled_out,
    gchar error[RADIOS_WASAPI_ERROR_SIZE])
{
    *count = 0;
    if (cancelled_out) *cancelled_out = FALSE;
    if (!enumerator) { set_error(error, "Windows ses aygıtlarına bağlı değil."); return RADIOS_WASAPI_FAILED; }
    EDataFlow direction = flow == RADIOS_WASAPI_FLOW_CAPTURE ? eCapture : eRender;
    IMMDeviceCollection *collection = NULL;
    HRESULT hr = IMMDeviceEnumerator_EnumAudioEndpoints(enumerator->object, direction, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) return enumeration_failure(FAILED(hr) ? hr : E_POINTER, error);
    UINT total = 0;
    hr = IMMDeviceCollection_GetCount(collection, &total);
    gchar console[RADIOS_WASAPI_ID_SIZE], communications[RADIOS_WASAPI_ID_SIZE];
    (void)default_id(enumerator->object, direction, eConsole, console);
    (void)default_id(enumerator->object, direction, eCommunications, communications);
    for (UINT i = 0; SUCCEEDED(hr) && i < total && *count < capacity; ++i) {
        if (cancelled && cancelled(data)) { if (cancelled_out) *cancelled_out = TRUE; break; }
        IMMDevice *device = NULL;
        hr = IMMDeviceCollection_Item(collection, i, &device);
        if (FAILED(hr) || !device) break;
        RadioSWasapiEndpoint *entry = &out[*count];
        if (endpoint_fill(device, entry)) {
            entry->flow = flow;
            entry->is_default = console[0] && !strcmp(entry->id, console);
            entry->is_default_communications = communications[0] && !strcmp(entry->id, communications);
            ++*count;
        }
        IMMDevice_Release(device);
    }
    IMMDeviceCollection_Release(collection);
    if (FAILED(hr)) { *count = 0; return enumeration_failure(hr, error); }
    return RADIOS_WASAPI_OK;
}
static IMMDevice *device_by_id(IMMDeviceEnumerator *object, const gchar *id, HRESULT *result)
{
    WCHAR *wide = wide_from_utf8(id); IMMDevice *device = NULL;
    HRESULT hr = wide ? IMMDeviceEnumerator_GetDevice(object, wide, &device) : E_INVALIDARG;
    g_free(wide);
    if (result) *result = hr;
    if (FAILED(hr)) return NULL;
    return device;
}
gboolean radios_wasapi_enumerator_lookup(RadioSWasapiEnumerator *enumerator, const gchar *id, RadioSWasapiEndpoint *out)
{
    if (!enumerator || !id || !out) return FALSE;
    IMMDevice *device = device_by_id(enumerator->object, id, NULL);
    if (!device) return FALSE;
    gboolean found = endpoint_fill(device, out);
    IMMDevice_Release(device);
    if (found) {
        EDataFlow direction = out->flow == RADIOS_WASAPI_FLOW_CAPTURE ? eCapture : eRender;
        gchar other[RADIOS_WASAPI_ID_SIZE];
        out->is_default = default_id(enumerator->object, direction, eConsole, other) && !strcmp(other, out->id);
        out->is_default_communications = default_id(enumerator->object, direction, eCommunications, other) && !strcmp(other, out->id);
    }
    return found;
}
gboolean radios_wasapi_enumerator_default(RadioSWasapiEnumerator *enumerator, RadioSWasapiFlow flow,
    gchar id[RADIOS_WASAPI_ID_SIZE])
{
    id[0] = '\0';
    return enumerator && default_id(enumerator->object, flow == RADIOS_WASAPI_FLOW_CAPTURE ? eCapture : eRender, eConsole, id);
}
static void pcm_format(WAVEFORMATEXTENSIBLE *format, guint rate, guint channels, guint bits, guint valid, gboolean floating, DWORD mask)
{
    memset(format, 0, sizeof *format);
    format->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; format->Format.nChannels = (WORD)channels;
    format->Format.nSamplesPerSec = rate; format->Format.wBitsPerSample = (WORD)bits;
    format->Format.nBlockAlign = (WORD)(channels * bits / 8); format->Format.nAvgBytesPerSec = rate * format->Format.nBlockAlign;
    format->Format.cbSize = sizeof *format - sizeof format->Format;
    format->Samples.wValidBitsPerSample = (WORD)valid; format->dwChannelMask = mask;
    format->SubFormat = floating ? SUBTYPE_FLOAT : SUBTYPE_PCM;
}
guint radios_wasapi_endpoint_max_rate(RadioSWasapiEnumerator *enumerator, const gchar *id, gboolean *in_use)
{
    if (in_use) *in_use = FALSE;
    if (!enumerator || !id) return 0;
    IMMDevice *device = device_by_id(enumerator->object, id, NULL);
    if (!device) return 0;
    IAudioClient *client = NULL; guint best = 0;
    if (SUCCEEDED(IMMDevice_Activate(device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client)) && client) {
        WAVEFORMATEX *mix = NULL;
        if (SUCCEEDED(IAudioClient_GetMixFormat(client, &mix)) && mix) best = mix->nSamplesPerSec;
        CoTaskMemFree(mix);
        static const guint rates[] = {768000, 705600, 384000, 352800, 192000, 176400, 96000, 88200, 48000, 44100};
        static const guint widths[][2] = {{32, 24}, {16, 16}, {24, 24}};
        gboolean stop = FALSE;
        for (guint r = 0; r < G_N_ELEMENTS(rates) && !stop && rates[r] > best; ++r)
            for (guint w = 0; w < G_N_ELEMENTS(widths) && !stop; ++w) {
                WAVEFORMATEXTENSIBLE format;
                pcm_format(&format, rates[r], 2, widths[w][0], widths[w][1], FALSE, FRONT_STEREO);
                HRESULT hr = IAudioClient_IsFormatSupported(client, AUDCLNT_SHAREMODE_EXCLUSIVE, &format.Format, NULL);
                if (hr == S_OK) { best = rates[r]; stop = TRUE; }
                else if (hr == AUDCLNT_E_DEVICE_IN_USE) { if (in_use) *in_use = TRUE; stop = TRUE; }
                else if (hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) stop = TRUE;
            }
        IAudioClient_Release(client);
    }
    IMMDevice_Release(device); return best;
}

/* ------------------------------------------------------------------ IMMNotificationClient */

typedef struct { guint id; RadioSWasapiChangeFunc func; gpointer data; } WatchEntry;
typedef struct {
    gint refs;          /* atomic: the watch handle and the COM object each hold one */
    GRWLock lock;       /* readers: dispatching notifications; writer: add/remove/close */
    gboolean alive;
    GArray *entries;
    guint next_id;
} WatchCore;
typedef struct {
    IMMNotificationClient iface;   /* first member: the COM pointer is the object */
    volatile LONG refs;
    WatchCore *core;
} NotifyClient;
struct RadioSWasapiWatch {
    RadioSWasapiEnumerator *enumerator;
    NotifyClient *client;
    WatchCore *core;
};

static WatchCore *watch_core_new(void)
{
    WatchCore *core = g_new0(WatchCore, 1);
    core->refs = 1; core->alive = TRUE; g_rw_lock_init(&core->lock);
    core->entries = g_array_new(FALSE, FALSE, sizeof(WatchEntry)); return core;
}
static void watch_core_unref(WatchCore *core)
{
    if (!core || !g_atomic_int_dec_and_test(&core->refs)) return;
    g_array_free(core->entries, TRUE); g_rw_lock_clear(&core->lock); g_free(core);
}
static void watch_dispatch(WatchCore *core, const RadioSWasapiChange *change)
{
    g_rw_lock_reader_lock(&core->lock);
    if (core->alive)
        for (guint i = 0; i < core->entries->len; ++i) {
            WatchEntry *entry = &g_array_index(core->entries, WatchEntry, i);
            entry->func(change, entry->data);
            atomic_fetch_add(&notifications_dispatched, 1);
        }
    g_rw_lock_reader_unlock(&core->lock);
}
static void notify_emit(IMMNotificationClient *self, RadioSWasapiChangeKind kind, LPCWSTR id, guint32 state,
    RadioSWasapiFlow flow, gint role, gboolean relevant)
{
    NotifyClient *client = (NotifyClient *)self;
    gchar *endpoint = utf8_from_wide(id);
    RadioSWasapiChange change = {kind, endpoint, state, flow, role, relevant};
    watch_dispatch(client->core, &change);
    g_free(endpoint);
}
static HRESULT STDMETHODCALLTYPE notify_query_interface(IMMNotificationClient *self, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IMMNotificationClient)) {
        *out = self; IMMNotificationClient_AddRef(self); return S_OK;
    }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE notify_add_ref(IMMNotificationClient *self)
{ return (ULONG)InterlockedIncrement(&((NotifyClient *)self)->refs); }
static ULONG STDMETHODCALLTYPE notify_release(IMMNotificationClient *self)
{
    NotifyClient *client = (NotifyClient *)self;
    LONG refs = InterlockedDecrement(&client->refs);
    if (refs == 0) {
        watch_core_unref(client->core); g_free(client);
        g_atomic_int_add(&live_notify_clients, -1);
    }
    return (ULONG)refs;
}
static HRESULT STDMETHODCALLTYPE notify_state(IMMNotificationClient *self, LPCWSTR id, DWORD state)
{ notify_emit(self, RADIOS_WASAPI_CHANGE_STATE, id, state, RADIOS_WASAPI_FLOW_RENDER, 0, TRUE); return S_OK; }
static HRESULT STDMETHODCALLTYPE notify_added(IMMNotificationClient *self, LPCWSTR id)
{ notify_emit(self, RADIOS_WASAPI_CHANGE_ADDED, id, 0, RADIOS_WASAPI_FLOW_RENDER, 0, TRUE); return S_OK; }
static HRESULT STDMETHODCALLTYPE notify_removed(IMMNotificationClient *self, LPCWSTR id)
{ notify_emit(self, RADIOS_WASAPI_CHANGE_REMOVED, id, 0, RADIOS_WASAPI_FLOW_RENDER, 0, TRUE); return S_OK; }
static HRESULT STDMETHODCALLTYPE notify_default(IMMNotificationClient *self, EDataFlow flow, ERole role, LPCWSTR id)
{
    gint value = role == eConsole ? 0 : role == eMultimedia ? 1 : 2;
    notify_emit(self, RADIOS_WASAPI_CHANGE_DEFAULT, id, 0,
        flow == eCapture ? RADIOS_WASAPI_FLOW_CAPTURE : RADIOS_WASAPI_FLOW_RENDER, value, role == eConsole);
    return S_OK;
}
static gboolean same_key(const PROPERTYKEY *a, const PROPERTYKEY *b)
{ return a->pid == b->pid && IsEqualGUID(&a->fmtid, &b->fmtid); }
static HRESULT STDMETHODCALLTYPE notify_property(IMMNotificationClient *self, LPCWSTR id, const PROPERTYKEY key)
{
    /* Volume and jack-sense properties change constantly; only what the
     * device lists show (name, description, mix format) matters. */
    gboolean relevant = same_key(&key, &PKEY_Device_FriendlyName) || same_key(&key, &PKEY_Device_DeviceDesc) ||
        same_key(&key, &PKEY_AudioEngine_DeviceFormat);
    notify_emit(self, RADIOS_WASAPI_CHANGE_PROPERTY, id, 0, RADIOS_WASAPI_FLOW_RENDER, 0, relevant);
    return S_OK;
}
static IMMNotificationClientVtbl notify_vtable = {
    notify_query_interface, notify_add_ref, notify_release,
    notify_state, notify_added, notify_removed, notify_default, notify_property
};
static NotifyClient *notify_client_new(WatchCore *core)
{
    NotifyClient *client = g_new0(NotifyClient, 1);
    client->iface.lpVtbl = &notify_vtable; client->refs = 1; client->core = core;
    g_atomic_int_inc(&core->refs); g_atomic_int_inc(&live_notify_clients);
    return client;
}
RadioSWasapiWatch *radios_wasapi_watch_new(RadioSWasapiEnumerator *enumerator, gchar error[RADIOS_WASAPI_ERROR_SIZE])
{
    if (!enumerator) { set_error(error, "Windows ses aygıtlarına bağlı değil."); return NULL; }
    WatchCore *core = watch_core_new();
    NotifyClient *client = notify_client_new(core);
    HRESULT hr = IMMDeviceEnumerator_RegisterEndpointNotificationCallback(enumerator->object, &client->iface);
    if (FAILED(hr)) {
        set_error(error, "Windows ses aygıtı bildirimlerine abone olunamadı (0x%08lX); birkaç saniye içinde yeniden denenecek.", code(hr));
        IMMNotificationClient_Release(&client->iface); watch_core_unref(core); return NULL;
    }
    RadioSWasapiWatch *watch = g_new0(RadioSWasapiWatch, 1);
    watch->enumerator = enumerator; watch->client = client; watch->core = core; return watch;
}
guint radios_wasapi_watch_add(RadioSWasapiWatch *watch, RadioSWasapiChangeFunc func, gpointer data)
{
    if (!watch || !func) return 0;
    g_rw_lock_writer_lock(&watch->core->lock);
    WatchEntry entry = {++watch->core->next_id, func, data};
    if (!entry.id) entry.id = ++watch->core->next_id;
    g_array_append_val(watch->core->entries, entry);
    g_rw_lock_writer_unlock(&watch->core->lock); return entry.id;
}
void radios_wasapi_watch_remove(RadioSWasapiWatch *watch, guint id)
{
    if (!watch || !id) return;
    g_rw_lock_writer_lock(&watch->core->lock);
    for (guint i = 0; i < watch->core->entries->len; ++i)
        if (g_array_index(watch->core->entries, WatchEntry, i).id == id) { g_array_remove_index(watch->core->entries, i); break; }
    g_rw_lock_writer_unlock(&watch->core->lock);
}
void radios_wasapi_watch_free(RadioSWasapiWatch *watch)
{
    if (!watch) return;
    (void)IMMDeviceEnumerator_UnregisterEndpointNotificationCallback(watch->enumerator->object, &watch->client->iface);
    g_rw_lock_writer_lock(&watch->core->lock); watch->core->alive = FALSE; g_rw_lock_writer_unlock(&watch->core->lock);
    IMMNotificationClient_Release(&watch->client->iface);
    watch_core_unref(watch->core); g_free(watch);
}
void radios_wasapi_debug_watch_emit(RadioSWasapiWatch *watch, RadioSWasapiChangeKind kind, const gchar *endpoint_id)
{
    if (!watch) return;
    WCHAR *wide = wide_from_utf8(endpoint_id); IMMNotificationClient *iface = &watch->client->iface;
    switch (kind) {
    case RADIOS_WASAPI_CHANGE_ADDED: IMMNotificationClient_OnDeviceAdded(iface, wide); break;
    case RADIOS_WASAPI_CHANGE_REMOVED: IMMNotificationClient_OnDeviceRemoved(iface, wide); break;
    case RADIOS_WASAPI_CHANGE_STATE: IMMNotificationClient_OnDeviceStateChanged(iface, wide, DEVICE_STATE_ACTIVE); break;
    case RADIOS_WASAPI_CHANGE_DEFAULT: IMMNotificationClient_OnDefaultDeviceChanged(iface, eRender, eConsole, wide); break;
    case RADIOS_WASAPI_CHANGE_PROPERTY: IMMNotificationClient_OnPropertyValueChanged(iface, wide, PKEY_Device_FriendlyName); break;
    }
    g_free(wide);
}

/* ------------------------------------------------------------------ waiting */

struct RadioSWasapiWaker { HANDLE event; };
RadioSWasapiWaker *radios_wasapi_waker_new(void)
{
    HANDLE event = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (!event) return NULL;
    RadioSWasapiWaker *waker = g_new0(RadioSWasapiWaker, 1); waker->event = event; return waker;
}
void radios_wasapi_waker_signal(RadioSWasapiWaker *waker) { if (waker) SetEvent(waker->event); }
void radios_wasapi_waker_free(RadioSWasapiWaker *waker)
{
    if (!waker) return;
    CloseHandle(waker->event); g_free(waker);
}

/* ------------------------------------------------------------------ streams */

typedef enum { SAMPLE_FLOAT32, SAMPLE_INT16, SAMPLE_INT24, SAMPLE_INT32 } SampleType;
struct RadioSWasapiStream {
    RadioSWasapiStreamKind kind;
    IMMDevice *device;
    IAudioClient *client;
    IAudioRenderClient *render;
    IAudioCaptureClient *capture;
    IAudioClock *clock;
    IAudioSessionControl *session;
    HANDLE event;
    UINT64 frequency;
    guint rate, buffer_frames, period_frames, mix_rate, mix_channels;
    gboolean converted, raw, raw_refused, started;
    /* layout of the endpoint buffer */
    gboolean plain;                /* float32 stereo in FL/FR order: copied as is */
    SampleType sample;
    guint channels, sample_bytes, frame_bytes;
    gfloat *left, *right;          /* capture: per-channel downmix weights */
    guint render_left, render_right;
    /* capture packet carried over between reads */
    gfloat *packet;
    guint packet_capacity, packet_offset, packet_frames, packet_flags;
    RadioSWasapiStreamStats stats;
    HRESULT hresult;
    gchar error[RADIOS_WASAPI_ERROR_SIZE];
    gchar endpoint_id[RADIOS_WASAPI_ID_SIZE], endpoint_name[RADIOS_WASAPI_NAME_SIZE];
};

RadioSWasapiWait radios_wasapi_wait(RadioSWasapiStream *stream, RadioSWasapiWaker *waker, guint timeout_ms)
{
    HANDLE handles[2]; RadioSWasapiWait kinds[2]; DWORD count = 0;
    if (waker && waker->event) { handles[count] = waker->event; kinds[count++] = RADIOS_WASAPI_WAIT_WAKER; }
    if (stream && stream->event) { handles[count] = stream->event; kinds[count++] = RADIOS_WASAPI_WAIT_STREAM; }
    if (!count) { Sleep(timeout_ms); return RADIOS_WASAPI_WAIT_TIMEOUT; }
    DWORD result = WaitForMultipleObjects(count, handles, FALSE, timeout_ms);
    if (result == WAIT_TIMEOUT) return RADIOS_WASAPI_WAIT_TIMEOUT;
    if (result < WAIT_OBJECT_0 + count) return kinds[result - WAIT_OBJECT_0];
    return RADIOS_WASAPI_WAIT_FAILED;
}

static gboolean endpoint_active(RadioSWasapiStream *stream)
{
    DWORD state = 0;
    return stream->device && SUCCEEDED(IMMDevice_GetState(stream->device, &state)) && state == DEVICE_STATE_ACTIVE;
}
static RadioSWasapiStatus stream_failure(RadioSWasapiStream *stream, HRESULT hr)
{
    stream->hresult = hr;
    if (hr == AUDCLNT_E_SERVICE_NOT_RUNNING) { set_error(stream->error, TEXT_SERVICE_DOWN); return RADIOS_WASAPI_SERVICE_DOWN; }
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_RESOURCES_INVALIDATED) {
        /* Format changes, resume from sleep and driver restarts invalidate a
         * stream whose endpoint is still there; only a removal is a loss. */
        if (endpoint_active(stream)) { set_error(stream->error, TEXT_REOPEN); return RADIOS_WASAPI_REOPEN; }
        set_error(stream->error, TEXT_REMOVED); return RADIOS_WASAPI_LOST;
    }
    if (hr == AUDCLNT_E_DEVICE_IN_USE) { set_error(stream->error, TEXT_IN_USE); return RADIOS_WASAPI_FAILED; }
    set_error(stream->error, "Ses aygıtı hatası (0x%08lX). Aygıtı yeniden seçin.", code(hr));
    return RADIOS_WASAPI_FAILED;
}
static gboolean fault_injected(RadioSWasapiStream *stream, RadioSWasapiStatus *status)
{
    if (!atomic_load_explicit(&injected_fault, memory_order_relaxed)) return FALSE;
    gint fault = atomic_exchange(&injected_fault, RADIOS_WASAPI_FAULT_NONE);
    switch (fault) {
    case RADIOS_WASAPI_FAULT_INVALIDATED: *status = stream_failure(stream, AUDCLNT_E_DEVICE_INVALIDATED); return TRUE;
    case RADIOS_WASAPI_FAULT_DEVICE_GONE:
        stream->hresult = AUDCLNT_E_DEVICE_INVALIDATED; set_error(stream->error, TEXT_REMOVED);
        *status = RADIOS_WASAPI_LOST; return TRUE;
    case RADIOS_WASAPI_FAULT_SERVICE: *status = stream_failure(stream, AUDCLNT_E_SERVICE_NOT_RUNNING); return TRUE;
    default: return FALSE;
    }
}

static guint bit_count(DWORD mask) { guint n = 0; for (; mask; mask &= mask - 1) ++n; return n; }
static DWORD default_mask(guint channels)
{
    switch (channels) {
    case 1: return SPEAKER_FRONT_CENTER;
    case 2: return FRONT_STEREO;
    case 4: return FRONT_STEREO | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    case 6: return FRONT_STEREO | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    case 8: return FRONT_STEREO | SPEAKER_FRONT_CENTER | SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT |
        SPEAKER_SIDE_LEFT | SPEAKER_SIDE_RIGHT;
    default: return 0;
    }
}
/* Deterministic capture levels: mono is duplicated at full scale (Pulse
 * parity), centre/surrounds fold in at -3 dB, LFE and height are dropped. */
static void stream_layout(RadioSWasapiStream *stream, SampleType sample, guint channels, guint bits, DWORD mask)
{
    stream->sample = sample; stream->channels = channels; stream->sample_bytes = bits / 8;
    stream->frame_bytes = channels * stream->sample_bytes;
    if (bit_count(mask) != channels) mask = 0;
    stream->plain = sample == SAMPLE_FLOAT32 && channels == 2 && (!mask || mask == FRONT_STEREO);
    g_free(stream->left); g_free(stream->right);
    stream->left = g_new0(gfloat, channels); stream->right = g_new0(gfloat, channels);
    stream->render_left = 0; stream->render_right = channels > 1 ? 1 : 0;
    if (channels == 1) { stream->left[0] = stream->right[0] = 1; return; }
    if (!mask) { stream->left[0] = 1; stream->right[1] = 1; return; }
    const gfloat half = 0.70710678f;
    guint channel = 0;
    for (guint bit = 0; bit < 32 && channel < channels; ++bit) {
        DWORD speaker = (DWORD)1 << bit;
        if (!(mask & speaker)) continue;
        gfloat *l = &stream->left[channel], *r = &stream->right[channel];
        switch (speaker) {
        case SPEAKER_FRONT_LEFT: *l = 1; stream->render_left = channel; break;
        case SPEAKER_FRONT_RIGHT: *r = 1; stream->render_right = channel; break;
        case SPEAKER_FRONT_CENTER: *l = *r = half; break;
        case SPEAKER_BACK_LEFT: case SPEAKER_SIDE_LEFT: case SPEAKER_FRONT_LEFT_OF_CENTER: *l = half; break;
        case SPEAKER_BACK_RIGHT: case SPEAKER_SIDE_RIGHT: case SPEAKER_FRONT_RIGHT_OF_CENTER: *r = half; break;
        case SPEAKER_BACK_CENTER: *l = *r = 0.5f; break;
        default: break;
        }
        ++channel;
    }
}
static gfloat sample_decode(const guint8 *data, SampleType sample)
{
    switch (sample) {
    case SAMPLE_FLOAT32: { gfloat value; memcpy(&value, data, sizeof value); return isfinite(value) ? value : 0; }
    case SAMPLE_INT16: { gint16 value; memcpy(&value, data, sizeof value); return value / 32768.0f; }
    case SAMPLE_INT24: {
        gint32 value = (gint32)((guint32)data[0] << 8 | (guint32)data[1] << 16 | (guint32)data[2] << 24);
        return (gfloat)(value / 2147483648.0);
    }
    case SAMPLE_INT32: { gint32 value; memcpy(&value, data, sizeof value); return (gfloat)(value / 2147483648.0); }
    }
    return 0;
}
static void sample_encode(guint8 *data, SampleType sample, gfloat value)
{
    gdouble clamped = isfinite(value) ? CLAMP((gdouble)value, -1.0, 1.0) : 0.0;
    switch (sample) {
    case SAMPLE_FLOAT32: { gfloat out = (gfloat)clamped; memcpy(data, &out, sizeof out); break; }
    case SAMPLE_INT16: { gint16 out = (gint16)lrint(clamped * 32767.0); memcpy(data, &out, sizeof out); break; }
    case SAMPLE_INT24: {
        gint32 out = (gint32)lrint(clamped * 8388607.0);
        data[0] = (guint8)(out & 0xFF); data[1] = (guint8)((out >> 8) & 0xFF); data[2] = (guint8)((out >> 16) & 0xFF); break;
    }
    case SAMPLE_INT32: { gint32 out = (gint32)llrint(clamped * 2147483647.0); memcpy(data, &out, sizeof out); break; }
    }
}
static void capture_convert(RadioSWasapiStream *stream, const guint8 *data, gfloat *stereo, guint frames)
{
    if (stream->plain) {
        memcpy(stereo, data, (gsize)frames * 2 * sizeof(gfloat));
        for (guint i = 0; i < frames * 2; ++i) if (!isfinite(stereo[i])) stereo[i] = 0;
        return;
    }
    for (guint i = 0; i < frames; ++i) {
        const guint8 *frame = data + (gsize)i * stream->frame_bytes; gfloat l = 0, r = 0;
        for (guint c = 0; c < stream->channels; ++c) {
            gfloat value = sample_decode(frame + (gsize)c * stream->sample_bytes, stream->sample);
            l += stream->left[c] * value; r += stream->right[c] * value;
        }
        stereo[i * 2] = l; stereo[i * 2 + 1] = r;
    }
}
static void render_convert(RadioSWasapiStream *stream, const gfloat *stereo, guint8 *data, guint frames)
{
    memset(data, 0, (gsize)frames * stream->frame_bytes);
    for (guint i = 0; i < frames; ++i) {
        guint8 *frame = data + (gsize)i * stream->frame_bytes;
        if (stream->channels == 1) {
            sample_encode(frame, stream->sample, (stereo[i * 2] + stereo[i * 2 + 1]) * 0.5f); continue;
        }
        sample_encode(frame + (gsize)stream->render_left * stream->sample_bytes, stream->sample, stereo[i * 2]);
        sample_encode(frame + (gsize)stream->render_right * stream->sample_bytes, stream->sample, stereo[i * 2 + 1]);
    }
}

typedef struct { guint rate, channels, bits; DWORD mask; SampleType sample; gboolean usable; WAVEFORMATEX *format; } MixFormat;
static void mix_parse(WAVEFORMATEX *format, MixFormat *mix)
{
    memset(mix, 0, sizeof *mix); mix->format = format;
    if (!format) return;
    mix->rate = format->nSamplesPerSec; mix->channels = format->nChannels; mix->bits = format->wBitsPerSample;
    gboolean floating = format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT, pcm = format->wFormatTag == WAVE_FORMAT_PCM;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && (gsize)format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        WAVEFORMATEXTENSIBLE extensible; memcpy(&extensible, format, sizeof extensible);
        mix->mask = extensible.dwChannelMask;
        floating = IsEqualGUID(&extensible.SubFormat, &SUBTYPE_FLOAT);
        pcm = IsEqualGUID(&extensible.SubFormat, &SUBTYPE_PCM);
    }
    if (!mix->mask) mix->mask = default_mask(mix->channels);
    if (floating && mix->bits == 32) mix->sample = SAMPLE_FLOAT32;
    else if (pcm && mix->bits == 16) mix->sample = SAMPLE_INT16;
    else if (pcm && mix->bits == 24) mix->sample = SAMPLE_INT24;
    else if (pcm && mix->bits == 32) mix->sample = SAMPLE_INT32;
    else return;
    mix->usable = mix->channels > 0 && mix->channels <= 32 && (guint)format->nBlockAlign == mix->channels * mix->bits / 8;
}

static gboolean retryable(HRESULT hr)
{
    return hr == AUDCLNT_E_UNSUPPORTED_FORMAT || hr == E_INVALIDARG || hr == AUDCLNT_E_BUFFER_SIZE_ERROR ||
        hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED || hr == AUDCLNT_E_INVALID_DEVICE_PERIOD ||
        hr == AUDCLNT_E_RAW_MODE_UNSUPPORTED || hr == E_NOTIMPL;
}
static gboolean capture_kind(RadioSWasapiStreamKind kind) { return kind != RADIOS_WASAPI_STREAM_RENDER; }
static void open_failure_text(RadioSWasapiStream *stream, HRESULT hr, gchar *error)
{
    gboolean input = capture_kind(stream->kind);
    if (hr == AUDCLNT_E_SERVICE_NOT_RUNNING) set_error(error, TEXT_SERVICE_DOWN);
    else if (hr == AUDCLNT_E_DEVICE_IN_USE) set_error(error, TEXT_IN_USE);
    else if (hr == E_ACCESSDENIED && stream->kind == RADIOS_WASAPI_STREAM_CAPTURE) set_error(error, TEXT_PRIVACY);
    else if (hr == E_ACCESSDENIED && stream->kind == RADIOS_WASAPI_STREAM_PROCESS_LOOPBACK) set_error(error, TEXT_APP_DENIED);
    else if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == E_NOTFOUND) set_error(error, TEXT_REMOVED);
    else if (hr == AUDCLNT_E_UNSUPPORTED_FORMAT)
        set_error(error, input ? "Aygıt %u Hz stereo giriş biçimini sağlayamadı. Başka bir giriş seçin." :
            "Aygıt %u Hz stereo çıkışı kabul etmedi. Başka örnekleme hızı ya da aygıt seçin.", stream->rate);
    else set_error(error, input ? "Giriş akışı açılamadı (0x%08lX). Aygıtı yeniden seçin." :
        "Çıkış akışı açılamadı (0x%08lX). Aygıtı yeniden seçin.", code(hr));
}
static RadioSWasapiStatus open_status(HRESULT hr)
{
    if (hr == AUDCLNT_E_SERVICE_NOT_RUNNING) return RADIOS_WASAPI_SERVICE_DOWN;
    if (hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == E_NOTFOUND) return RADIOS_WASAPI_LOST;
    return RADIOS_WASAPI_FAILED;
}
static void client_drop(RadioSWasapiStream *stream)
{
    if (!stream->client) return;
    if (IAudioClient_Release(stream->client)) g_atomic_int_inc(&unreleased_clients);
    stream->client = NULL;
}
static void client_properties(RadioSWasapiStream *stream, gboolean raw)
{
    if (!raw) return;
    IAudioClient2 *client2 = NULL;
    if (FAILED(IAudioClient_QueryInterface(stream->client, &IID_IAudioClient2, (void **)&client2)) || !client2) {
        stream->raw_refused = TRUE; return;
    }
    AudioClientProperties properties; memset(&properties, 0, sizeof properties);
    properties.cbSize = sizeof properties; properties.bIsOffload = FALSE;
    properties.eCategory = AudioCategory_Other; properties.Options = AUDCLNT_STREAMOPTIONS_RAW;
    if (FAILED(IAudioClient2_SetClientProperties(client2, &properties))) stream->raw_refused = TRUE;
    else stream->raw = TRUE;
    IAudioClient2_Release(client2);
}
/* Activates a fresh client and initialises it with one format/flag choice. */
typedef struct { gboolean convert, event, zero_duration; } Attempt;
static HRESULT endpoint_initialize(RadioSWasapiStream *stream, const RadioSWasapiStreamSpec *spec, const MixFormat *mix,
    const Attempt *attempt, REFERENCE_TIME duration, const GUID *session)
{
    client_drop(stream); stream->raw = FALSE;
    HRESULT hr = IMMDevice_Activate(stream->device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&stream->client);
    if (FAILED(hr) || !stream->client) { stream->client = NULL; return FAILED(hr) ? hr : E_POINTER; }
    client_properties(stream, spec->raw && !stream->raw_refused);
    WAVEFORMATEXTENSIBLE format; const WAVEFORMATEX *chosen = &format.Format;
    if (attempt->convert) {
        guint channels = capture_kind(stream->kind) && mix->usable ? mix->channels : 2;
        DWORD mask = channels == 2 ? FRONT_STEREO : (mix->usable && bit_count(mix->mask) == channels ? mix->mask : default_mask(channels));
        pcm_format(&format, stream->rate, channels, 32, 32, TRUE, mask);
        stream_layout(stream, SAMPLE_FLOAT32, channels, 32, mask);
    } else {
        chosen = mix->format;
        stream_layout(stream, mix->sample, mix->channels, mix->bits, mix->mask);
    }
    DWORD flags = AUDCLNT_STREAMFLAGS_NOPERSIST;
    if (attempt->event) flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (stream->kind == RADIOS_WASAPI_STREAM_LOOPBACK) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
    if (attempt->convert) flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = IAudioClient_Initialize(stream->client, AUDCLNT_SHAREMODE_SHARED, flags,
        attempt->zero_duration ? 0 : duration, 0, chosen, session);
    if (SUCCEEDED(hr) && attempt->event) {
        if (!stream->event) stream->event = CreateEventW(NULL, FALSE, FALSE, NULL);
        hr = stream->event ? IAudioClient_SetEventHandle(stream->client, stream->event) : HRESULT_FROM_WIN32(GetLastError());
    }
    if (SUCCEEDED(hr)) stream->converted = attempt->convert;
    return hr;
}
static RadioSWasapiStatus stream_services(RadioSWasapiStream *stream, const RadioSWasapiStreamSpec *spec, gchar *error)
{
    UINT32 frames = 0;
    HRESULT hr = IAudioClient_GetBufferSize(stream->client, &frames);
    if (SUCCEEDED(hr)) {
        stream->buffer_frames = frames;
        hr = stream->kind == RADIOS_WASAPI_STREAM_RENDER ?
            IAudioClient_GetService(stream->client, &IID_IAudioRenderClient, (void **)&stream->render) :
            IAudioClient_GetService(stream->client, &IID_IAudioCaptureClient, (void **)&stream->capture);
    }
    if (FAILED(hr)) { stream->hresult = hr; open_failure_text(stream, hr, error); return open_status(hr); }
    if (SUCCEEDED(IAudioClient_GetService(stream->client, &IID_IAudioClock, (void **)&stream->clock)) && stream->clock) {
        if (FAILED(IAudioClock_GetFrequency(stream->clock, &stream->frequency))) stream->frequency = 0;
    } else stream->clock = NULL;
    /* The session control comes from the session manager by GUID: one taken
     * through IAudioClient::GetService keeps a reference on the client after
     * every release, which would leak one client per reopen. */
    IAudioSessionManager *sessions = NULL;
    if (stream->device && SUCCEEDED(IMMDevice_Activate(stream->device, &IID_IAudioSessionManager, CLSCTX_ALL, NULL,
            (void **)&sessions)) && sessions) {
        GUID guid; const GUID *session_guid = NULL;
        if (spec->session_guid) { memcpy(&guid, spec->session_guid, sizeof guid); session_guid = &guid; }
        if (FAILED(IAudioSessionManager_GetAudioSessionControl(sessions, session_guid, 0, &stream->session))) stream->session = NULL;
        IAudioSessionManager_Release(sessions);
    }
    if (stream->session) {
        if (spec->session_name && *spec->session_name) {
            WCHAR *name = wide_from_utf8(spec->session_name);
            if (name) (void)IAudioSessionControl_SetDisplayName(stream->session, name, NULL);
            g_free(name);
        }
        /* Teams/Zoom communication streams never duck a broadcast bus. */
        IAudioSessionControl2 *control2 = NULL;
        if (SUCCEEDED(IAudioSessionControl_QueryInterface(stream->session, &IID_IAudioSessionControl2, (void **)&control2)) && control2) {
            (void)IAudioSessionControl2_SetDuckingPreference(control2, TRUE);
            IAudioSessionControl2_Release(control2);
        }
    }
    if (capture_kind(stream->kind)) {
        stream->packet_capacity = MAX(stream->buffer_frames, 4096u);
        stream->packet = g_new0(gfloat, (gsize)stream->packet_capacity * 2);
    }
    return RADIOS_WASAPI_OK;
}
static RadioSWasapiStatus open_endpoint(RadioSWasapiEnumerator *given, RadioSWasapiStream *stream,
    const RadioSWasapiStreamSpec *spec, gchar *error)
{
    const gchar *id = spec->endpoint_id && *spec->endpoint_id ? spec->endpoint_id : RADIOS_WASAPI_DEFAULT_ID;
    gchar *base;
    if (capture_kind(spec->kind) && g_str_has_suffix(id, RADIOS_WASAPI_MONITOR_SUFFIX) && strlen(id) > strlen(RADIOS_WASAPI_MONITOR_SUFFIX)) {
        stream->kind = RADIOS_WASAPI_STREAM_LOOPBACK;
        base = g_strndup(id, strlen(id) - strlen(RADIOS_WASAPI_MONITOR_SUFFIX));
    } else base = g_strdup(id);
    gboolean input = stream->kind == RADIOS_WASAPI_STREAM_CAPTURE;
    gboolean selected_input = capture_kind(stream->kind);
    RadioSWasapiEnumerator *enumerator = given, *owned = NULL;
    if (!enumerator && !(enumerator = owned = radios_wasapi_enumerator_new(error))) { g_free(base); return RADIOS_WASAPI_FAILED; }
    RadioSWasapiStatus status = RADIOS_WASAPI_OK; HRESULT hr = S_OK;
    EDataFlow direction = input ? eCapture : eRender;
    if (!strcmp(base, RADIOS_WASAPI_DEFAULT_ID)) {
        hr = IMMDeviceEnumerator_GetDefaultAudioEndpoint(enumerator->object, direction, eConsole, &stream->device);
        if (FAILED(hr) || !stream->device) {
            stream->device = NULL;
            if (hr == AUDCLNT_E_SERVICE_NOT_RUNNING) { set_error(error, TEXT_SERVICE_DOWN); status = RADIOS_WASAPI_SERVICE_DOWN; }
            else { set_error(error, input ? TEXT_NO_DEFAULT_INPUT : TEXT_NO_DEFAULT_OUTPUT); status = RADIOS_WASAPI_LOST; }
        }
    } else if (!(stream->device = device_by_id(enumerator->object, base, &hr))) {
        /* An identifier Windows does not know (unplugged, or a Linux device
         * name in old preferences) is "not found": never another device. */
        if (hr == AUDCLNT_E_SERVICE_NOT_RUNNING) { set_error(error, TEXT_SERVICE_DOWN); status = RADIOS_WASAPI_SERVICE_DOWN; }
        else { set_error(error, selected_input ? TEXT_MISSING_INPUT : TEXT_MISSING_OUTPUT); status = RADIOS_WASAPI_LOST; }
    }
    g_free(base);
    if (status == RADIOS_WASAPI_OK) {
        RadioSWasapiEndpoint endpoint;
        if (!endpoint_fill(stream->device, &endpoint) || endpoint.state != DEVICE_STATE_ACTIVE) {
            set_error(error, selected_input ? TEXT_MISSING_INPUT : TEXT_MISSING_OUTPUT); status = RADIOS_WASAPI_LOST;
        } else if (input && endpoint.flow != RADIOS_WASAPI_FLOW_CAPTURE) {
            set_error(error, TEXT_NOT_INPUT); status = RADIOS_WASAPI_FAILED;
        } else if (!input && endpoint.flow != RADIOS_WASAPI_FLOW_RENDER) {
            set_error(error, selected_input ? TEXT_LOOPBACK_NOT_OUTPUT : TEXT_NOT_OUTPUT); status = RADIOS_WASAPI_FAILED;
        } else {
            g_strlcpy(stream->endpoint_id, endpoint.id, sizeof stream->endpoint_id);
            g_strlcpy(stream->endpoint_name, endpoint.name, sizeof stream->endpoint_name);
        }
    }
    radios_wasapi_enumerator_free(owned);
    if (status != RADIOS_WASAPI_OK) return status;

    /* Mix format and device period come from a probe client. */
    IAudioClient *probe = NULL; WAVEFORMATEX *mix_format = NULL;
    REFERENCE_TIME default_period = 100000, minimum_period = 0;
    hr = IMMDevice_Activate(stream->device, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&probe);
    if (SUCCEEDED(hr) && probe) {
        if (FAILED(IAudioClient_GetMixFormat(probe, &mix_format))) mix_format = NULL;
        if (FAILED(IAudioClient_GetDevicePeriod(probe, &default_period, &minimum_period)) || default_period <= 0) default_period = 100000;
        IAudioClient_Release(probe);
    } else {
        stream->hresult = FAILED(hr) ? hr : E_POINTER; open_failure_text(stream, stream->hresult, error);
        return open_status(stream->hresult);
    }
    MixFormat mix; mix_parse(mix_format, &mix);
    stream->mix_rate = mix.rate; stream->mix_channels = mix.channels;
    stream->period_frames = MAX(1u, (guint)(default_period * stream->rate / REFTIMES_PER_SECOND));
    guint frames;
    if (stream->kind == RADIOS_WASAPI_STREAM_RENDER) {
        guint target = spec->buffer_frames ? spec->buffer_frames : 2 * stream->period_frames;
        frames = MAX(target, 2 * stream->period_frames) + stream->period_frames;
    } else frames = spec->buffer_frames ? spec->buffer_frames : stream->rate / 5;
    REFERENCE_TIME duration = (REFERENCE_TIME)frames * REFTIMES_PER_SECOND / stream->rate;
    GUID session; const GUID *session_guid = NULL;
    if (spec->session_guid) { memcpy(&session, spec->session_guid, sizeof session); session_guid = &session; }
    /* Preferred: our float stereo with Windows' converter. Then a buffer
     * size Windows picks, the endpoint's own format converted here (same
     * rate only), and for capture kinds polling instead of events. */
    const Attempt attempts[] = {
        {TRUE, TRUE, FALSE}, {TRUE, TRUE, TRUE}, {FALSE, TRUE, FALSE}, {TRUE, FALSE, FALSE}, {FALSE, FALSE, FALSE}
    };
    hr = E_FAIL;
    for (guint a = 0; a < G_N_ELEMENTS(attempts); ++a) {
        const Attempt *attempt = &attempts[a];
        if (!attempt->convert && (!mix.usable || mix.rate != stream->rate)) continue;
        if (!attempt->event && !capture_kind(stream->kind)) continue;
        hr = endpoint_initialize(stream, spec, &mix, attempt, duration, session_guid);
        if (FAILED(hr) && stream->raw) {
            stream->raw_refused = TRUE;
            hr = endpoint_initialize(stream, spec, &mix, attempt, duration, session_guid);
        }
        if (SUCCEEDED(hr) || !retryable(hr)) break;
    }
    CoTaskMemFree(mix_format);
    if (FAILED(hr)) {
        stream->hresult = hr;
        if (retryable(hr)) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
        open_failure_text(stream, hr, error); return open_status(hr);
    }
    return stream_services(stream, spec, error);
}

/* IActivateAudioInterfaceCompletionHandler in C. It is agile: Windows may
 * call it from any thread without marshalling (otherwise activation from an
 * MTA thread can fail with E_ILLEGAL_METHOD_CALL). */
typedef struct {
    IActivateAudioInterfaceCompletionHandler iface;
    volatile LONG refs;
    HANDLE done;
} ActivationHandler;
static HRESULT STDMETHODCALLTYPE activation_query_interface(IActivateAudioInterfaceCompletionHandler *self, REFIID iid, void **out)
{
    if (!out) return E_POINTER;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IActivateAudioInterfaceCompletionHandler) ||
        IsEqualIID(iid, &IID_IAgileObject)) {
        *out = self; IActivateAudioInterfaceCompletionHandler_AddRef(self); return S_OK;
    }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE activation_add_ref(IActivateAudioInterfaceCompletionHandler *self)
{ return (ULONG)InterlockedIncrement(&((ActivationHandler *)self)->refs); }
static ULONG STDMETHODCALLTYPE activation_release(IActivateAudioInterfaceCompletionHandler *self)
{
    ActivationHandler *handler = (ActivationHandler *)self;
    LONG refs = InterlockedDecrement(&handler->refs);
    if (refs == 0) {
        CloseHandle(handler->done); g_free(handler);
        g_atomic_int_add(&live_activation_handlers, -1);
    }
    return (ULONG)refs;
}
static HRESULT STDMETHODCALLTYPE activation_completed(IActivateAudioInterfaceCompletionHandler *self,
    IActivateAudioInterfaceAsyncOperation *operation)
{
    (void)operation; SetEvent(((ActivationHandler *)self)->done); return S_OK;
}
static IActivateAudioInterfaceCompletionHandlerVtbl activation_vtable = {
    activation_query_interface, activation_add_ref, activation_release, activation_completed
};
static ActivationHandler *activation_handler_new(void)
{
    HANDLE done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!done) return NULL;
    ActivationHandler *handler = g_new0(ActivationHandler, 1);
    handler->iface.lpVtbl = &activation_vtable; handler->refs = 1; handler->done = done;
    g_atomic_int_inc(&live_activation_handlers); return handler;
}
static gboolean activation_unsupported(HRESULT hr)
{
    return hr == E_NOTIMPL || hr == E_INVALIDARG || hr == E_NOINTERFACE || hr == REGDB_E_CLASSNOTREG ||
        hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) || hr == E_NOTFOUND;
}
static HRESULT process_client_activate(guint32 pid, gboolean exclude, guint timeout_ms, IAudioClient **client)
{
    *client = NULL;
    RadioSActivationParams params; memset(&params, 0, sizeof params);
    params.ActivationType = RADIOS_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = exclude ? RADIOS_PROCESS_LOOPBACK_EXCLUDE_TREE : RADIOS_PROCESS_LOOPBACK_INCLUDE_TREE;
    PROPVARIANT value; PropVariantInit(&value);   /* points at our stack: never PropVariantClear */
    value.vt = VT_BLOB; value.blob.cbSize = sizeof params; value.blob.pBlobData = (BYTE *)&params;
    ActivationHandler *handler = activation_handler_new();
    if (!handler) return HRESULT_FROM_WIN32(GetLastError());
    IActivateAudioInterfaceAsyncOperation *operation = NULL;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_PROCESS_LOOPBACK, &IID_IAudioClient, &value, &handler->iface, &operation);
    if (SUCCEEDED(hr) && operation) {
        if (WaitForSingleObject(handler->done, timeout_ms ? timeout_ms : 5000) != WAIT_OBJECT_0) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        else {
            HRESULT activated = E_FAIL; IUnknown *unknown = NULL;
            hr = IActivateAudioInterfaceAsyncOperation_GetActivateResult(operation, &activated, &unknown);
            if (SUCCEEDED(hr)) hr = activated;
            if (SUCCEEDED(hr) && unknown) hr = IUnknown_QueryInterface(unknown, &IID_IAudioClient, (void **)client);
            else if (SUCCEEDED(hr)) hr = E_NOINTERFACE;
            if (unknown) IUnknown_Release(unknown);
        }
    } else if (SUCCEEDED(hr)) hr = E_POINTER;
    if (operation) IActivateAudioInterfaceAsyncOperation_Release(operation);
    IActivateAudioInterfaceCompletionHandler_Release(&handler->iface);
    if (FAILED(hr) && *client) { IAudioClient_Release(*client); *client = NULL; }
    return hr;
}
static void process_failure_text(HRESULT hr, gchar *error)
{
    if (hr == AUDCLNT_E_SERVICE_NOT_RUNNING) set_error(error, TEXT_SERVICE_DOWN);
    else if (hr == E_ACCESSDENIED) set_error(error, TEXT_APP_DENIED);
    else if (activation_unsupported(hr)) set_error(error, TEXT_APP_UNSUPPORTED);
    else if (hr == HRESULT_FROM_WIN32(ERROR_TIMEOUT)) set_error(error, "Uygulama sesi yakalaması zamanında başlamadı; yeniden deneyin.");
    else set_error(error, "Uygulama sesi yakalanamadı (0x%08lX).", code(hr));
}
static RadioSWasapiStatus open_process_loopback(RadioSWasapiStream *stream, const RadioSWasapiStreamSpec *spec, gchar *error)
{
    if (!spec->process_id) { set_error(error, "Sesi yakalanacak uygulama seçilmedi."); return RADIOS_WASAPI_FAILED; }
    guint frames = spec->buffer_frames ? spec->buffer_frames : stream->rate / 5;
    REFERENCE_TIME duration = (REFERENCE_TIME)frames * REFTIMES_PER_SECOND / stream->rate;
    /* Process loopback clients have no mix format or device period: the
     * format is always explicit; 16-bit PCM is the documented fallback. */
    static const guint widths[] = {32, 16};
    HRESULT hr = E_FAIL;
    for (guint w = 0; w < G_N_ELEMENTS(widths); ++w) {
        client_drop(stream);
        hr = process_client_activate(spec->process_id, spec->exclude_process_tree, spec->activation_timeout_ms, &stream->client);
        if (FAILED(hr)) break;
        WAVEFORMATEXTENSIBLE format; gboolean floating = widths[w] == 32;
        pcm_format(&format, stream->rate, 2, widths[w], widths[w], floating, FRONT_STEREO);
        stream_layout(stream, floating ? SAMPLE_FLOAT32 : SAMPLE_INT16, 2, widths[w], FRONT_STEREO);
        hr = IAudioClient_Initialize(stream->client, AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
            duration, 0, &format.Format, NULL);
        if (SUCCEEDED(hr)) {
            if (!stream->event) stream->event = CreateEventW(NULL, FALSE, FALSE, NULL);
            hr = stream->event ? IAudioClient_SetEventHandle(stream->client, stream->event) : HRESULT_FROM_WIN32(GetLastError());
        }
        if (SUCCEEDED(hr) || !retryable(hr)) break;
    }
    if (FAILED(hr)) { stream->hresult = hr; process_failure_text(hr, error); return open_status(hr); }
    stream->converted = TRUE; stream->period_frames = MAX(1u, stream->rate / 100);
    stream->mix_rate = stream->rate; stream->mix_channels = 2;
    return stream_services(stream, spec, error);
}

RadioSWasapiStatus radios_wasapi_stream_open(RadioSWasapiEnumerator *enumerator, const RadioSWasapiStreamSpec *spec,
    RadioSWasapiStream **out, RadioSWasapiStreamInfo *info, gchar error[RADIOS_WASAPI_ERROR_SIZE])
{
    if (out) *out = NULL;
    if (info) memset(info, 0, sizeof *info);
    if (error) error[0] = '\0';
    if (!out || !spec || spec->rate < 8000 || spec->rate > 768000 || spec->buffer_frames > spec->rate * 2) {
        set_error(error, "Ses akışı isteği geçersiz."); return RADIOS_WASAPI_FAILED;
    }
    RadioSWasapiStream *stream = g_new0(RadioSWasapiStream, 1);
    stream->kind = spec->kind; stream->rate = spec->rate;
    g_atomic_int_inc(&live_streams);
    gchar message[RADIOS_WASAPI_ERROR_SIZE] = "";
    RadioSWasapiStatus status = spec->kind == RADIOS_WASAPI_STREAM_PROCESS_LOOPBACK ?
        open_process_loopback(stream, spec, message) : open_endpoint(enumerator, stream, spec, message);
    if (status != RADIOS_WASAPI_OK) {
        if (!message[0]) set_error(message, "Ses akışı açılamadı.");
        if (error) g_strlcpy(error, message, RADIOS_WASAPI_ERROR_SIZE);
        radios_wasapi_stream_close(stream); return status;
    }
    if (info) {
        info->rate = stream->rate; info->buffer_frames = stream->buffer_frames; info->period_frames = stream->period_frames;
        info->mix_rate = stream->mix_rate; info->mix_channels = stream->mix_channels;
        info->converted = stream->converted; info->event_driven = stream->event != NULL; info->raw = stream->raw;
        g_strlcpy(info->endpoint_id, stream->endpoint_id, sizeof info->endpoint_id);
        g_strlcpy(info->endpoint_name, stream->endpoint_name, sizeof info->endpoint_name);
    }
    *out = stream; return RADIOS_WASAPI_OK;
}
RadioSWasapiStatus radios_wasapi_stream_padding(RadioSWasapiStream *stream, guint *queued)
{
    *queued = 0;
    if (!stream || !stream->client) return RADIOS_WASAPI_FAILED;
    RadioSWasapiStatus status;
    if (fault_injected(stream, &status)) return status;
    UINT32 padding = 0;
    HRESULT hr = IAudioClient_GetCurrentPadding(stream->client, &padding);
    if (FAILED(hr)) return stream_failure(stream, hr);
    if (stream->kind == RADIOS_WASAPI_STREAM_RENDER && stream->started && !padding) ++stream->stats.underruns;
    *queued = padding; return RADIOS_WASAPI_OK;
}
RadioSWasapiStatus radios_wasapi_stream_write(RadioSWasapiStream *stream, const gfloat *stereo, guint frames)
{
    if (!stream || !stream->render || frames > RADIOS_WASAPI_MAX_FRAMES) {
        if (stream) set_error(stream->error, "Ses çıkışına geçersiz blok yazıldı.");
        return RADIOS_WASAPI_FAILED;
    }
    if (!frames) return RADIOS_WASAPI_OK;
    BYTE *data = NULL;
    HRESULT hr = IAudioRenderClient_GetBuffer(stream->render, frames, &data);
    if (FAILED(hr) || !data) return stream_failure(stream, FAILED(hr) ? hr : E_POINTER);
    DWORD flags = 0;
    if (!stereo) flags = AUDCLNT_BUFFERFLAGS_SILENT;
    else if (stream->plain) memcpy(data, stereo, (gsize)frames * 2 * sizeof(gfloat));
    else render_convert(stream, stereo, data, frames);
    hr = IAudioRenderClient_ReleaseBuffer(stream->render, frames, flags);
    if (FAILED(hr)) return stream_failure(stream, hr);
    stream->stats.frames += frames; return RADIOS_WASAPI_OK;
}
RadioSWasapiStatus radios_wasapi_stream_read(RadioSWasapiStream *stream, gfloat *stereo, guint capacity,
    guint *frames, guint *flags)
{
    *frames = 0;
    if (flags) *flags = 0;
    if (!stream || !stream->capture || !stereo || !capacity) return RADIOS_WASAPI_FAILED;
    if (stream->packet_offset >= stream->packet_frames) {
        RadioSWasapiStatus status;
        if (fault_injected(stream, &status)) return status;
        UINT32 next = 0;
        HRESULT hr = IAudioCaptureClient_GetNextPacketSize(stream->capture, &next);
        if (FAILED(hr)) return stream_failure(stream, hr);
        if (!next) return RADIOS_WASAPI_OK;
        BYTE *data = NULL; UINT32 count = 0; DWORD buffer_flags = 0;
        hr = IAudioCaptureClient_GetBuffer(stream->capture, &data, &count, &buffer_flags, NULL, NULL);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) return RADIOS_WASAPI_OK;
        if (FAILED(hr)) return stream_failure(stream, hr);
        if (count > stream->packet_capacity) {
            stream->packet_capacity = count; g_free(stream->packet);
            stream->packet = g_new0(gfloat, (gsize)count * 2);
        }
        if ((buffer_flags & AUDCLNT_BUFFERFLAGS_SILENT) || !data) memset(stream->packet, 0, (gsize)count * 2 * sizeof(gfloat));
        else capture_convert(stream, data, stream->packet, count);
        hr = IAudioCaptureClient_ReleaseBuffer(stream->capture, count);
        if (FAILED(hr)) return stream_failure(stream, hr);
        stream->packet_flags = 0;
        if (buffer_flags & AUDCLNT_BUFFERFLAGS_SILENT) { stream->packet_flags |= RADIOS_WASAPI_PACKET_SILENT; ++stream->stats.silent_packets; }
        if (buffer_flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
            stream->packet_flags |= RADIOS_WASAPI_PACKET_DISCONTINUITY; ++stream->stats.discontinuities;
        }
        if (buffer_flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ++stream->stats.timestamp_errors;
        stream->packet_frames = count; stream->packet_offset = 0;
    }
    guint available = stream->packet_frames - stream->packet_offset, copied = MIN(available, capacity);
    memcpy(stereo, stream->packet + (gsize)stream->packet_offset * 2, (gsize)copied * 2 * sizeof(gfloat));
    if (flags) *flags = stream->packet_flags;
    stream->packet_offset += copied; stream->stats.frames += copied; *frames = copied;
    return RADIOS_WASAPI_OK;
}
RadioSWasapiStatus radios_wasapi_stream_position(RadioSWasapiStream *stream, guint64 *frames)
{
    *frames = 0;
    if (!stream || !stream->client) return RADIOS_WASAPI_FAILED;
    if (!stream->clock || !stream->frequency) return RADIOS_WASAPI_OK;
    UINT64 position = 0;
    HRESULT hr = IAudioClock_GetPosition(stream->clock, &position, NULL);
    if (FAILED(hr)) return stream_failure(stream, hr);
    /* seconds = position / frequency, split to stay exact for long runs */
    *frames = (position / stream->frequency) * stream->rate + (position % stream->frequency) * stream->rate / stream->frequency;
    return RADIOS_WASAPI_OK;
}
RadioSWasapiStatus radios_wasapi_stream_start(RadioSWasapiStream *stream)
{
    if (!stream || !stream->client) return RADIOS_WASAPI_FAILED;
    HRESULT hr = IAudioClient_Start(stream->client);
    if (FAILED(hr) && hr != AUDCLNT_E_NOT_STOPPED) return stream_failure(stream, hr);
    stream->started = TRUE; return RADIOS_WASAPI_OK;
}
RadioSWasapiStatus radios_wasapi_stream_stop(RadioSWasapiStream *stream)
{
    if (!stream || !stream->client) return RADIOS_WASAPI_FAILED;
    HRESULT hr = IAudioClient_Stop(stream->client);
    stream->started = FALSE;
    return FAILED(hr) ? stream_failure(stream, hr) : RADIOS_WASAPI_OK;
}
RadioSWasapiStatus radios_wasapi_stream_reset(RadioSWasapiStream *stream)
{
    if (!stream || !stream->client) return RADIOS_WASAPI_FAILED;
    HRESULT hr = IAudioClient_Reset(stream->client);
    if (FAILED(hr)) return stream_failure(stream, hr);
    stream->packet_frames = stream->packet_offset = 0;
    if (stream->event) ResetEvent(stream->event);
    return RADIOS_WASAPI_OK;
}
void radios_wasapi_stream_stats(RadioSWasapiStream *stream, RadioSWasapiStreamStats *stats)
{
    if (!stats) return;
    if (stream) *stats = stream->stats; else memset(stats, 0, sizeof *stats);
}
const gchar *radios_wasapi_stream_error(RadioSWasapiStream *stream) { return stream ? stream->error : ""; }
guint32 radios_wasapi_stream_hresult(RadioSWasapiStream *stream) { return stream ? (guint32)stream->hresult : 0; }
void radios_wasapi_stream_close(RadioSWasapiStream *stream)
{
    if (!stream) return;
    if (stream->client && stream->started) (void)IAudioClient_Stop(stream->client);
    if (stream->render) IAudioRenderClient_Release(stream->render);
    if (stream->capture) IAudioCaptureClient_Release(stream->capture);
    if (stream->clock) IAudioClock_Release(stream->clock);
    if (stream->session) IAudioSessionControl_Release(stream->session);
    client_drop(stream);
    if (stream->device) IMMDevice_Release(stream->device);
    if (stream->event) CloseHandle(stream->event);
    g_free(stream->packet); g_free(stream->left); g_free(stream->right); g_free(stream);
    g_atomic_int_add(&live_streams, -1);
}

static GMutex process_probe_lock;
static gint process_probe_state;   /* 0 unknown, 1 supported, 2 unsupported */
static gchar process_probe_reason[RADIOS_WASAPI_ERROR_SIZE];
gboolean radios_wasapi_process_loopback_supported(gchar reason[RADIOS_WASAPI_ERROR_SIZE])
{
    g_mutex_lock(&process_probe_lock);
    gint state = process_probe_state; gchar message[RADIOS_WASAPI_ERROR_SIZE] = "";
    if (!state) {
        IAudioClient *client = NULL;
        HRESULT hr = process_client_activate(GetCurrentProcessId(), FALSE, 3000, &client);
        if (client) IAudioClient_Release(client);
        if (SUCCEEDED(hr)) state = process_probe_state = 1;
        else {
            process_failure_text(hr, message);
            /* Only a definitive answer is cached; timeouts and service outages are retried. */
            if (activation_unsupported(hr) || hr == E_ACCESSDENIED) {
                state = process_probe_state = 2; g_strlcpy(process_probe_reason, message, sizeof process_probe_reason);
            } else state = 2;
        }
    } else if (state == 2) g_strlcpy(message, process_probe_reason, sizeof message);
    g_mutex_unlock(&process_probe_lock);
    if (reason) g_strlcpy(reason, state == 1 ? "" : message, RADIOS_WASAPI_ERROR_SIZE);
    return state == 1;
}

/* ------------------------------------------------------------------ applications */

static gchar *process_image(HANDLE process)
{
    DWORD size = 32768; WCHAR *path = g_new0(WCHAR, size);
    gchar *image = QueryFullProcessImageNameW(process, 0, path, &size) ? utf8_from_wide(path) : NULL;
    g_free(path); return image;
}
static gchar *file_description(const gchar *path)
{
    WCHAR *wide = wide_from_utf8(path); gchar *description = NULL;
    DWORD ignored = 0, size = wide ? GetFileVersionInfoSizeW(wide, &ignored) : 0;
    if (size) {
        guint8 *data = g_malloc(size); WORD *translation = NULL; UINT length = 0;
        if (GetFileVersionInfoW(wide, 0, size, data) &&
            VerQueryValueW(data, L"\\VarFileInfo\\Translation", (LPVOID *)&translation, &length) && translation && length >= 4) {
            gchar *key = g_strdup_printf("\\StringFileInfo\\%04x%04x\\FileDescription", translation[0], translation[1]);
            WCHAR *wide_key = wide_from_utf8(key); WCHAR *value = NULL; UINT value_length = 0;
            if (wide_key && VerQueryValueW(data, wide_key, (LPVOID *)&value, &value_length) && value && value_length > 1)
                description = utf8_from_wide(value);
            g_free(wide_key); g_free(key);
        }
        g_free(data);
    }
    g_free(wide);
    if (description) g_strstrip(description);
    if (description && !*description) g_clear_pointer(&description, g_free);
    return description;
}
gboolean radios_wasapi_process_start_time(guint32 pid, guint64 *start_time)
{
    if (start_time) *start_time = 0;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return FALSE;
    FILETIME created, exited, kernel, user;
    gboolean known = GetProcessTimes(process, &created, &exited, &kernel, &user);
    CloseHandle(process);
    if (known && start_time) *start_time = (guint64)created.dwHighDateTime << 32 | created.dwLowDateTime;
    return known;
}
static gchar *session_display_name(IAudioSessionControl *control)
{
    LPWSTR name = NULL; gchar *text = NULL;
    if (SUCCEEDED(IAudioSessionControl_GetDisplayName(control, &name)) && name && *name) {
        if (name[0] == L'@') {
            WCHAR resolved[512];
            if (SUCCEEDED(SHLoadIndirectString(name, resolved, G_N_ELEMENTS(resolved), NULL)) && resolved[0])
                text = utf8_from_wide(resolved);
        } else text = utf8_from_wide(name);
    }
    CoTaskMemFree(name); return text;
}
static gint session_compare(gconstpointer a, gconstpointer b)
{
    const RadioSWasapiSession *x = a, *y = b;
    gint order = g_utf8_collate(x->name, y->name);
    return order ? order : x->pid < y->pid ? -1 : x->pid > y->pid;
}
RadioSWasapiStatus radios_wasapi_sessions_list(RadioSWasapiEnumerator *enumerator, gboolean include_self,
    RadioSWasapiSession *out, guint capacity, guint *count, gchar error[RADIOS_WASAPI_ERROR_SIZE])
{
    *count = 0;
    if (!enumerator) { set_error(error, "Windows ses aygıtlarına bağlı değil."); return RADIOS_WASAPI_FAILED; }
    IMMDeviceCollection *collection = NULL;
    HRESULT hr = IMMDeviceEnumerator_EnumAudioEndpoints(enumerator->object, eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr) || !collection) return enumeration_failure(FAILED(hr) ? hr : E_POINTER, error);
    GArray *items = g_array_new(FALSE, TRUE, sizeof(RadioSWasapiSession));
    GHashTable *by_pid = g_hash_table_new(NULL, NULL);
    UINT total = 0; (void)IMMDeviceCollection_GetCount(collection, &total);
    DWORD self = GetCurrentProcessId();
    for (UINT d = 0; d < total; ++d) {
        IMMDevice *device = NULL; gchar id[RADIOS_WASAPI_ID_SIZE];
        if (FAILED(IMMDeviceCollection_Item(collection, d, &device)) || !device) continue;
        IAudioSessionManager2 *manager = NULL; IAudioSessionEnumerator *sessions = NULL;
        if (!device_id(device, id) ||
            FAILED(IMMDevice_Activate(device, &IID_IAudioSessionManager2, CLSCTX_ALL, NULL, (void **)&manager)) || !manager ||
            FAILED(IAudioSessionManager2_GetSessionEnumerator(manager, &sessions)) || !sessions) {
            if (manager) IAudioSessionManager2_Release(manager);
            IMMDevice_Release(device); continue;
        }
        int sessions_count = 0; (void)IAudioSessionEnumerator_GetCount(sessions, &sessions_count);
        for (int s = 0; s < sessions_count; ++s) {
            IAudioSessionControl *control = NULL; IAudioSessionControl2 *control2 = NULL;
            if (FAILED(IAudioSessionEnumerator_GetSession(sessions, s, &control)) || !control) continue;
            DWORD pid = 0; AudioSessionState state = AudioSessionStateInactive;
            if (SUCCEEDED(IAudioSessionControl_QueryInterface(control, &IID_IAudioSessionControl2, (void **)&control2)) && control2 &&
                IAudioSessionControl2_IsSystemSoundsSession(control2) != S_OK &&
                IAudioSessionControl2_GetProcessId(control2, &pid) == S_OK && pid && (include_self || pid != self)) {
                (void)IAudioSessionControl_GetState(control, &state);
                gpointer slot = g_hash_table_lookup(by_pid, GUINT_TO_POINTER(pid));
                RadioSWasapiSession *item;
                if (!slot) {
                    RadioSWasapiSession fresh; memset(&fresh, 0, sizeof fresh); fresh.pid = pid;
                    g_array_append_val(items, fresh);
                    g_hash_table_insert(by_pid, GUINT_TO_POINTER(pid), GUINT_TO_POINTER(items->len));
                    item = &g_array_index(items, RadioSWasapiSession, items->len - 1);
                } else item = &g_array_index(items, RadioSWasapiSession, GPOINTER_TO_UINT(slot) - 1);
                gboolean active = state == AudioSessionStateActive;
                if (!item->endpoint_id[0] || (active && !item->active)) g_strlcpy(item->endpoint_id, id, sizeof item->endpoint_id);
                item->active |= active;
                if (!item->name[0]) {
                    gchar *name = session_display_name(control);
                    if (name) radios_wasapi_copy_text(item->name, sizeof item->name, name);
                    g_free(name);
                }
            }
            if (control2) IAudioSessionControl2_Release(control2);
            IAudioSessionControl_Release(control);
        }
        IAudioSessionEnumerator_Release(sessions); IAudioSessionManager2_Release(manager); IMMDevice_Release(device);
    }
    IMMDeviceCollection_Release(collection);
    for (guint i = 0; i < items->len; ++i) {
        RadioSWasapiSession *item = &g_array_index(items, RadioSWasapiSession, i);
        HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, item->pid);
        gchar *path = NULL;
        if (process) {
            FILETIME created, exited, kernel, user;
            if (GetProcessTimes(process, &created, &exited, &kernel, &user))
                item->start_time = (guint64)created.dwHighDateTime << 32 | created.dwLowDateTime;
            path = process_image(process); CloseHandle(process);
        }
        if (path) {
            const gchar *slash = strrchr(path, '\\');
            radios_wasapi_copy_text(item->exe, sizeof item->exe, slash ? slash + 1 : path);
            if (!item->name[0]) {
                gchar *description = file_description(path);
                if (description) radios_wasapi_copy_text(item->name, sizeof item->name, description);
                g_free(description);
            }
        }
        g_free(path);
        if (!item->name[0] && item->exe[0]) {
            gchar *name = g_strdup(item->exe); gsize length = strlen(name);
            if (length > 4 && !g_ascii_strcasecmp(name + length - 4, ".exe")) name[length - 4] = '\0';
            radios_wasapi_copy_text(item->name, sizeof item->name, name); g_free(name);
        }
        if (!item->name[0]) g_snprintf(item->name, sizeof item->name, "PID %u", (guint)item->pid);
    }
    g_array_sort(items, session_compare);
    for (guint i = 0; i < items->len && *count < capacity; ++i) out[(*count)++] = g_array_index(items, RadioSWasapiSession, i);
    g_hash_table_unref(by_pid); g_array_free(items, TRUE);
    return RADIOS_WASAPI_OK;
}

/* ------------------------------------------------------------------ COM self-test */

gboolean radios_wasapi_debug_com_selftest(gchar error[RADIOS_WASAPI_ERROR_SIZE])
{
    gint clients = g_atomic_int_get(&live_notify_clients), handlers = g_atomic_int_get(&live_activation_handlers);
    WatchCore *core = watch_core_new(); NotifyClient *client = notify_client_new(core);
    IMMNotificationClient *notify = &client->iface; void *out = NULL; gboolean valid = TRUE;
    valid &= notify_query_interface(notify, &IID_IUnknown, &out) == S_OK && out == notify && client->refs == 2;
    valid &= IMMNotificationClient_Release(notify) == 1;
    valid &= IMMNotificationClient_QueryInterface(notify, &IID_IMMNotificationClient, &out) == S_OK && out == notify;
    valid &= IMMNotificationClient_Release(notify) == 1;
    valid &= IMMNotificationClient_QueryInterface(notify, &IID_IAudioClient, &out) == E_NOINTERFACE && !out;
    valid &= IMMNotificationClient_QueryInterface(notify, &IID_IAudioClient, NULL) == E_POINTER;
    valid &= IMMNotificationClient_AddRef(notify) == 2 && IMMNotificationClient_Release(notify) == 1;
    valid &= g_atomic_int_get(&live_notify_clients) == clients + 1;
    valid &= IMMNotificationClient_Release(notify) == 0 && g_atomic_int_get(&live_notify_clients) == clients;
    valid &= g_atomic_int_get(&core->refs) == 1;
    watch_core_unref(core);
    ActivationHandler *handler = activation_handler_new();
    if (!handler) { set_error(error, "Etkinleştirme olayı oluşturulamadı."); return FALSE; }
    IActivateAudioInterfaceCompletionHandler *completion = &handler->iface;
    valid &= IActivateAudioInterfaceCompletionHandler_QueryInterface(completion, &IID_IAgileObject, &out) == S_OK && out == completion;
    valid &= IActivateAudioInterfaceCompletionHandler_QueryInterface(completion, &IID_IActivateAudioInterfaceCompletionHandler, &out) == S_OK;
    valid &= IActivateAudioInterfaceCompletionHandler_QueryInterface(completion, &IID_IUnknown, &out) == S_OK && handler->refs == 4;
    valid &= IActivateAudioInterfaceCompletionHandler_Release(completion) == 3 && IActivateAudioInterfaceCompletionHandler_Release(completion) == 2;
    valid &= IActivateAudioInterfaceCompletionHandler_Release(completion) == 1;
    valid &= IActivateAudioInterfaceCompletionHandler_QueryInterface(completion, &IID_IMMNotificationClient, &out) == E_NOINTERFACE && !out;
    valid &= WaitForSingleObject(handler->done, 0) == WAIT_TIMEOUT;
    valid &= IActivateAudioInterfaceCompletionHandler_ActivateCompleted(completion, NULL) == S_OK && WaitForSingleObject(handler->done, 0) == WAIT_OBJECT_0;
    valid &= g_atomic_int_get(&live_activation_handlers) == handlers + 1;
    valid &= IActivateAudioInterfaceCompletionHandler_Release(completion) == 0 && g_atomic_int_get(&live_activation_handlers) == handlers;
    if (!valid) set_error(error, "WASAPI COM nesnelerinin referans sayımı tutarsız.");
    return valid;
}
