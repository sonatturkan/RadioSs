# Windows port spec — area `audio-routing-capture`

Project analysed: `C:\Users\sonatturkan\Desktop\RadioS` (read-only). All `file:line` references are to that copy.

Files in scope:

| File | Lines | Role |
|---|---|---|
| `src/audio/patchbay.c` | 956 | Live runtime of the routing page (“Ses yönlendirme”): Pulse connection, inventory, capture/playback links, virtual null-sinks, app moves, 10 ms graph worker, WAV recorders, Icecast/SHOUTcast streams |
| `include/radios/audio_patchbay.h` | 105 | Public patchbay API + snapshot structs |
| `src/audio/channel_input.c` | 541 | Mixer-channel live input (R5.1): sound-card capture (private Pulse connection) or FFmpeg network URL |
| `include/radios/audio_channel_input.h` | 76 | Channel input API |
| `src/core/routing_graph.c` / `include/radios/routing_graph.h` | 579 / 134 | Graph model, validation, loop detection (encodes Pulse device-name conventions), JSON, legacy schema 1 |
| `src/ui/routing.c` | 1456 | GTK routing page: configs, panels, run/record/stream buttons, authorization, app chooser |
| `src/ui/routing_graph_ui.c` / `include/radios/routing_graph_ui.h` | 600 / 44 | Cairo canvas editor (portable) |
| `src/ui/media_routing.c` / `include/radios/media_routing.h` | 298 / 11 | Media→playlist “routing” panel. **No audio, no platform code** — portable as is |
| `src/ui/audio_ui.c` (routing/capture parts) | 1829 | Channel inputs (extras), capture device list, network sources, device combos |
| `database/SDBASE/src/sdbase/desktop_api.py:393-679` | — | Server-side validator mirroring `routing_graph.c` (cross-area, but contract lives here) |
| Tests | — | `tests/test_patchbay.c`, `tests/test_channel_input.c`, `tests/test_routing_graph.c`, `tests/test_routing_ui.c` + `tests/test_routing_graph_ui.c` (both include `tests/routing_patchbay_ui_test.inc`), `tests/run_routing_ui.py`, `tests/run_routing_graph_ui.py`, `tests/test_routing_graph_schema.py`, `tests/test_desktop_routing.py`, `tests/run_audio_routing.py` (orphan + helper module), `tests/run_routing_host.py` (orphan) |
| Doc | — | `docs/ses-yonlendirme.md` (partly stale: describes the removed schema-1 “application + compressor” engine `src/audio/routing.c`, which no longer exists) |

---

## 0. Executive summary

* The routing area is the only place in RadioS that uses **PulseAudio server-side features** beyond plain playback/record: introspection of sinks/sources/sink-inputs, `module-null-sink` and `module-remap-source` creation (“Sanal ses arayüzü” + virtual microphone), `move-sink-input` of another application's stream, monitor sources (`<sink>.monitor`) and event subscription. Windows has **no user-mode way to create audio endpoints or move another process's stream**. Everything else maps cleanly to WASAPI.
* Recommended Windows design:
  1. A shared internal **audio host** layer (`src/audio/audio_host.h`, implementations `audio_host_wasapi.c`, `audio_host_fixture.c`, optional `audio_host_pulse.c`) that the engine backend (other area), the patchbay and channel inputs all use, so device identifiers are identical everywhere.
  2. **Device naming contract** that keeps every persisted/validated rule working unchanged: render/capture endpoints are named by their `IMMDevice::GetId()` string, the loopback (“monitor”) of a render endpoint is `"<render id>.monitor"`, `"@default"` and `"@talk"` keep their meaning, `"radios_virtual_<id>"` stays a reserved logical name. Then `routing_graph.c implicit()` and `desktop_api.py implicit()` work untouched.
  3. **Per-application capture** via process loopback (`ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, …, AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK, INCLUDE_TARGET_PROCESS_TREE)`); applications listed with `IAudioSessionManager2`/`IAudioSessionEnumerator` (PID → exe basename / display name), identity = PID + process creation time, pinned by an open process handle.
  4. The `RG_VIRTUAL` block becomes an **application-capture block** on Windows (no endpoint is created, nothing is moved, `as_microphone` unsupported); cables into it are summed internally.
  5. Endpoint capture / loopback / render via shared-mode WASAPI, 48 kHz float stereo at the API boundary (`AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | SRC_DEFAULT_QUALITY`, explicit up/down-mix), one MMCSS I/O thread per stream feeding the existing lock-free rings.
  6. A deterministic **fixture audio host** (`RADIOS_AUDIO_HOST=fixture:<json>`) replaces the “private PulseAudio daemon + null sinks + pacat/parec” test fixtures, because Windows has no private audio server and CI runners usually have no audio endpoints. Real-WASAPI acceptance is opt-in.
* Header availability (verified in `C:\msys64\ucrt64`): `mmdeviceapi.h` declares `ActivateAudioInterfaceAsync` + `IActivateAudioInterface*`; `libmmdevapi.a` exports `ActivateAudioInterfaceAsync`; `audioclient.h`, `audiopolicy.h`, `audiosessiontypes.h` (LOOPBACK/AUTOCONVERTPCM/SRC_DEFAULT_QUALITY), `functiondiscoverykeys_devpkey.h`, `objidl.h` (`IID_IAgileObject`), `avrt.h`, `processthreadsapi.h` (power throttling), `synchapi.h` (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`) are present. **`audioclientactivationparams.h` is missing** → declare manually (§5.5).
* MSYS2 FFmpeg is **9.0.2** (`avformat-63.dll`), TLS via GnuTLS, `--enable-libsrt` → `srt://` is accepted on Windows (test path “srt available” is taken).

---

## 1. What the Linux code does (inventory of every Linux/Pulse-specific construct)

### 1.1 `src/audio/patchbay.c`

| Lines | Construct | What/why |
|---|---|---|
| 1 | `#define _POSIX_C_SOURCE 200809L` | Harmless on MinGW but keep WASAPI code in a separate TU (no POSIX macro, `COBJMACROS`, `WIN32_LEAN_AND_MEAN`). |
| 4 | `#include <pulse/pulseaudio.h>` | Entire Pulse dependency. |
| 9-20 | `RATE 48000, BLOCK 480, FRAME_BYTES 8, RING_FRAMES 48000, PRIME 960, LIMIT 9600, KEEP 1920, STEP_US 10000, RETRY_US 5 s, RESYNC_US 3 s` | **Contract constants — keep identical on Windows.** |
| 22-31 | `Link {pa_stream*, RadioSAudioRing ring, device[513], epoch, playback, primed, ready, failed}` | One stream + SPSC ring. Windows: replace `pa_stream*` with `RadioSAudioHostStream*`, keep the rest. |
| 32-62 | `Live` incl. `guint32 module, mic_module; module_epoch; module_name; module_mic` | Pulse module indices of the virtual null-sink and remap-source. Windows: replace with `application binding` (pid/start/handle) — §6.3. |
| 85-99 | `pa_threaded_mainloop *loop; pa_context *context; epoch; reconnect_at; connected; cleaned; staged_* lists` | Pulse connection state. Windows: `RadioSAudioHost *host; guint64 host_epoch; connected`. `cleaned` (stale module cleanup) has no Windows equivalent (nothing survives the process). |
| 101, 125-138 | `Query` + `wait_query()` — polls a `pa_operation` with 2 ms unlock/sleep up to 3 s | Synchronous wrapper for async Pulse ops. Windows: not needed (WASAPI calls are synchronous); keep the **3 s open timeout** semantics in `link_open`. |
| 113-114 | `own_application()` — hides sink-inputs whose `application.name` starts with `RadioS` or `application.process.binary == "radios"` | Windows: hide sessions with `pid == GetCurrentProcessId()`, exe basename `radios.exe` (case-insensitive) or display name starting `RadioS`. |
| 115 | `virtual_sink()` → `"radios_virtual_%u"` | Reserved logical name; keep (used by loop validator and DB validator). |
| 116-122 | `pulse_quoted()` — module-argument quoting | Linux only. |
| 139-140 | `pulse_ready()` | → `radios_audio_host_status()`. |
| 142-147 | `link_state()` — `PA_STREAM_READY`→ready, `FAILED/TERMINATED`→failed | → host stream `running` / `failed` atomics. |
| 148-161 | `link_read()` — `pa_stream_peek`; a NULL data pointer (“hole”) is written as **silence**; frames must be multiple of 8 bytes | Windows: `AUDCLNT_BUFFERFLAGS_SILENT` → write zeros; same 8-byte frame. |
| 162-176 | `link_write()` — `pa_stream_begin_write`, priming (`PRIME` 20 ms), trimming (`> LIMIT` 200 ms → keep `KEEP` 40 ms), zero-fill on underrun | Same algorithm inside the WASAPI render callback (fill `GetBuffer(available)`). |
| 177-211 | `link_open()` — `pa_sample_spec {FLOAT32NE, 48000, 2}`; proplist `media.name="RadioS · <title>"`, `media.role="production"`; playback `pa_buffer_attr{maxlength=-1, tlength=KEEP*8*2 (80 ms), prebuf=-1, minreq=BLOCK*8 (10 ms)}` + `PA_STREAM_ADJUST_LATENCY | PA_STREAM_DONT_MOVE`; record `{maxlength=RATE*8 (1 s), fragsize=BLOCK*8 (10 ms)}` + `ADJUST_LATENCY | DONT_MOVE`; waits ≤3 s for READY else “Aygıt açılamadı: %s” | §5.6/§5.7 recipes. `DONT_MOVE` ≙ WASAPI streams opened by explicit endpoint id never follow default changes (same). |
| 212-223 | `link_close()` — clears callbacks, disconnects only if same epoch & READY | → `radios_audio_host_stream_close()` (joins I/O thread; no callback after return). |
| 224-234 | `module_load/unload` via `pa_context_load_module/unload_module` | Linux only. |
| 237-261 | `device_entry/sink_listed/source_listed` — names ≤512 bytes, `monitor = info->monitor_of_sink != PA_INVALID_INDEX`, publishes atomically at list end | Windows: §5.2 enumeration; publish atomically (copy staged list under mutex). |
| 262-278 | `application_listed()` — per **sink-input**: `application.name` (fallback stream name, fallback “Uygulama”), `application.process.binary`, `index`, `sink` | Windows: per **audio session** (dedup by PID): §5.3. |
| 279-288 | `inventory()` — 3 async list ops | → host snapshot copy. |
| 289-293 | `subscribed()` — any SINK/SOURCE/SINK_INPUT event → `refresh_requested` | → host change callback (IMMNotificationClient + session poll). |
| 301-306, 341-355 | Stale-module cleanup: unload modules whose argument contains `sink_name=radios_virtual_` / `source_name=radios_virtual_` (crash leftovers) | **Not needed on Windows** — RadioS creates no persistent server objects. |
| 308-360 | `connection()` — new context every 2 s while failed, proplist `application.name="RadioS Patchbay"`, **`media.category="Manager"`** (PipeWire requires this role to move other apps' streams), `PA_CONTEXT_NOFAIL`; on READY: `++epoch`, subscribe `SINK|SOURCE|SINK_INPUT`, stale cleanup, inventory | Windows: host acquire; `connected` = enumerator OK && Audiosrv running; `epoch` increments on every recovery. No “manager” role exists/needed. |
| 363-397 | `retire/path_taken/record_path/record_open` — folder must be `g_path_is_absolute`, `g_mkdir_with_parents(folder, 0700)`, file `"<prefix>-YYYYMMDD-HHMMSS.wav"` then `-2..-999`, collision check against existing files **and** other live recorders' paths (string compare) | §6.5 Windows path rules. |
| 398-456 | recorder/stream start/stop, intents (requests kept until the pending apply creates the block) | Portable. Keep. |
| 459-520 | `pull()`/`tick_locked()` — 10 ms graph step in topological order, gain −60..+12 dB, PTT gate ramp, clamp ±8, sends to engine line or playback ring, recorder push, auto-split by minutes, stream push, peak/RMS accumulation | Portable. One Windows change: `RG_VIRTUAL` sums its cabled input into its output (§6.3). |
| 521-532 | `worker()` — 10 ms clock via `g_cond_wait_until`, catch-up if >100 ms late | Windows timer granularity (15.6 ms default) → use high-resolution waitable timer (§8). |
| 536-550 | `live_close()` — unload virtual modules if same epoch | Windows: close streams, close process handle. |
| 553-679 | `reconcile()` — makes the live plan match the desired graph; virtual module (re)creation (579-602: `module-null-sink sink_name=radios_virtual_<id> rate=48000 channels=2 sink_properties='device.description="RadioS · <title>"'` and, if `as_microphone`, `module-remap-source master=<sink>.monitor source_name=<sink>_mic …`); when running: capture for INPUT/MICROPHONE (≠ `@talk`) (605-607), capture `<sink>.monitor` + playback into `<sink>` for VIRTUAL (608-611), playback for OUTPUT (612-614); engine taps/lines (624-645); link reuse if same device/epoch and not failed | §6.2 per-branch Windows mapping. |
| 680-705 | `housekeeping()` — link failed → “Aygıt bağlantısı koptu: %s” + resync (3 s); stream reconnect every 5 s | Portable; add bound-process exit check (§6.3). |
| 706-729 | `control()` thread — wakes on serial change/refresh/resync/retired or every 500 ms | Windows: `CoInitializeEx(MTA)` at thread start. |
| 731-767 | `new/unref` — `pa_threaded_mainloop_new/start/stop/free` | → host acquire/release. |
| 888-902 | `radios_audio_patchbay_move_application()` — `pa_context_move_sink_input_by_name(application, "radios_virtual_<node>")` | Windows: **bind** the app as the block's capture source (§6.3). |
| 903-955 | `snapshot()` — virtual `sink`/`source` names (922-926); state rules (929-933) | Windows: empty `sink`/`source`, new app fields (§6.4). |

### 1.2 `src/audio/channel_input.c`

| Lines | Construct | Windows |
|---|---|---|
| 1, 5 | `_POSIX_C_SOURCE`, `#include <pulse/pulseaudio.h>` | Remove Pulse include on Windows; capture via audio host. |
| 12-30 | `PACE_FRAMES 4800 (100 ms)`, `CONNECT_TIMEOUT_US 15 s`, `BLOCK_FRAMES 4096`, `FRAME_BYTES 8`, protocol whitelist `http,https,tls,tcp,udp,rtp,rtsp,srt`, `CLOSE_WAIT_US 250 ms`, `NO_DATA_US 5 s`, `PEAK_HOLD_US 500 ms` | Keep. |
| 38 (header) | `device`: “Pulse source name, or the stream URL” | Capture: endpoint id, `<render id>.monitor`, `@default` or `""`. |
| 74-147 | Scheme table + `avio_enum_protocols()` probing; SRT refused when FFmpeg lacks it | Portable. MSYS2 FFmpeg 9.0.2 has srt. |
| 168-189 | `deliver()` — pacing with `g_usleep(5000)/g_usleep(2000)` while line queue > 4800 | Portable (15.6 ms sleep granularity is tolerable for a 100 ms pace). |
| 191-297 | `Capture`/`capture_run()` — **private `pa_mainloop` + `pa_context`** per input (`application.name="RadioS"`, `application.id="org.radios.channel-input"`, `PA_CONTEXT_NOAUTOSPAWN`), record stream `media.name="RadioS kanal girişi"`, `media.role="production"`, `fragsize = 480 frames`, `PA_STREAM_ADJUST_LATENCY` (**no** `DONT_MOVE`), `""`/`"@default"` → NULL = default source; loop `pa_mainloop_prepare(50 ms)/poll/dispatch` checking `closing`; READY → `status READY`; not ready within 15 s → “Ses kartı girişi zamanında açılmadı.”; failures → messages “Ses kartı girişi açılamadı: %s”, “Ses kartı girişi kapandı.”, “Ses sunucusuna bağlanılamadı: %s”, “Ses sunucusu bağlantısı kapandı.” | §7. |
| 299-462 | FFmpeg network input (interrupt callback, `rw_timeout`, rtsp `?tcp`→`rtsp_transport=tcp`, swresample to 48 k float stereo, mono duplicated) | Portable; verify compile against FFmpeg 9 (avformat 63). |
| 472-540 | worker thread, 5 s retry, 250 ms bounded free with detached-teardown fallback | Portable. |

### 1.3 Pulse naming conventions baked into validators (must survive)

* `src/core/routing_graph.c:126-131` `virtual_names()`: sink `radios_virtual_<id>`, monitor `radios_virtual_<id>.monitor`, virtual mic `radios_virtual_<id>_mic`.
* `src/core/routing_graph.c:134-150` `implicit()`: an OUTPUT block whose `device` is X implicitly feeds any INPUT/MICROPHONE block whose device is `X + ".monitor"`; a VIRTUAL block feeds INPUT/MICROPHONE using its `.monitor`/`_mic`; an OUTPUT whose device equals `radios_virtual_<b.id>` feeds VIRTUAL b. **Latent bug:** `gchar sink[64]` and `g_strlcpy(sink, a->device, sizeof sink)` (line 139/142) truncate device names > 63 bytes (real Pulse names already exceed this, e.g. `alsa_output.usb-Focusrite_…analog-stereo`), so loop detection silently fails for long names. Fix on both platforms: use `gchar sink[RADIOS_ROUTING_GRAPH_TEXT_SIZE], monitor[RADIOS_ROUTING_GRAPH_TEXT_SIZE + 8], microphone[…]`.
* `database/SDBASE/src/sdbase/desktop_api.py:490-510` `implicit()` mirrors the same rule (`f"radios_virtual_{id}"`, `+ ".monitor"`, `+ "_mic"`).
* `tests/test_routing_graph.c:104,108-112` and `tests/test_routing_graph_schema.py:129-138` test exactly these strings (`radios_virtual_%u.monitor`, `alsa_output.card(.monitor)`, `test_sink(.monitor)`).

→ The Windows naming contract (§4) keeps the suffix rule so none of this changes.

### 1.4 UI (`src/ui/routing.c`, `src/ui/routing_graph_ui.c`, `src/ui/audio_ui.c`)

* `routing.c:646-677` device combos list `snapshot->inputs/outputs`; monitors labelled “Çıkış monitörü · %s”; a saved but absent device is shown as `missing:<name>` → “Kullanılamıyor · <name>” and preserved (doc: “Bulunamayan kayıtlı aygıt tercihi korunur ve belirtilir”).
* `routing.c:678-692` application combo: id = `app->index` (decimal), label `name · binary`.
* `routing.c:693-700` inventory hash (rebuild combos only when content changes).
* `routing.c:701-715` folder chooser (`GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER`).
* `routing.c:772-782` “Buraya yönlendir” → `radios_audio_patchbay_move_application()`; notice “Uygulamanın sesi sanal arayüze yönlendirildi; …”.
* `routing.c:886-896` VIRTUAL panel: `virtual_info`, checkbox “Diğer uygulamalara mikrofon olarak da sun” (`as_microphone`), app combo, “Buraya yönlendir”.
* `routing.c:1166, 1203-1207` VIRTUAL detail “RadioS · %s”, “Diğer uygulamalarda çıkış: “RadioS · %s” (%s)\nKayıt kaynağı: %s” / “Sanal aygıt ses sunucusunda oluşturuluyor…”.
* `routing.c:1303-1306` run state “SES SUNUCUSU BEKLENİYOR” when `!snapshot->connected`.
* `routing.c:1382-1403` `radios_ui_routing_open()` (declared in `routing_ui.h`, currently not called from `src/`): creates/selects a VIRTUAL block “<provider> sesi” and instructs “Buraya yönlendir”.
* `routing.c:1447-1453` patchbay created with `radios_audio_patchbay_new(NULL)` (default server) only when the DB is connected.
* `routing_graph_ui.c:31` VIRTUAL caption “Diğer uygulamaların görebildiği RadioS sanal ses aygıtı”; `:69` font `"Inter, Sans"`; `:124` glyph “✎”; `:466` CSS `assets/themes/routing-graph.css` via `g_build_filename`; `:520-541` same-app GTK DnD. All portable.
* `audio_ui.c:326-371` channel input specs from the `dj_mixer` document → `radios_audio_channel_input_new(engine, i, spec, NULL, …)` (NULL = default server).
* `audio_ui.c:590-601` `radios_audio_ui_capture_devices()` = engine snapshot inputs, monitors last (feeds the DJ “in:<name>” choices, `src/ui/dj.c:1971-1978, 1995-2002`).
* `audio_ui.c:642-650` sources hash; `audio_ui.c:1538-1558` device combos with “@default” = “Sistem varsayılan girişi/çıkışı”, monitor suffix “ · Çıkış geri dinlemesi”; `:1711` “Ses sunucusu bağlı değil; ses servisini kontrol edip uygulamayı yeniden aç.”

### 1.5 User features mapped (current code vs. doc)

| Feature | Current implementation | Notes |
|---|---|---|
| Capture a selected application | `RG_VIRTUAL` block = null sink; app stream **moved** into it (“taşınır”: no longer heard on its previous output); block output = sink monitor | Windows: process loopback, app keeps playing on its own device (§6.3). |
| Microphone / line input | `RG_MICROPHONE` / `RG_INPUT` with a Pulse source name; `@talk` = engine's on-air mic tap with PTT gate | Windows: capture endpoint id; `@talk` unchanged (engine). |
| Output monitor capture | `RG_INPUT` device `<sink>.monitor` | Windows: `<render id>.monitor` = endpoint loopback. |
| Virtual microphone for other apps | `as_microphone` → `module-remap-source` `radios_virtual_<id>_mic` | **Not possible on Windows without a driver** (§6.3). |
| Compressor (Kapalı/Konuşma/Müzik) | Only in legacy schema 1; `routing_graph_from_legacy()` (`routing_graph.c:532-578`) drops it and turns a compressed mic path into a direct cable | Nothing to port. |
| Peak/RMS meters | `tick_locked()` accumulation + `snapshot()` (`patchbay.c:513-518, 917-921`); `RG_METER` block; UI `meter_draw` | Portable. |
| WAV recording start/pause/resume/split/stop, auto-split | `RG_RECORDER`; `radios_audio_output_start_record()` (output.c, PCM16 48 k stereo, `O_CREAT|O_EXCL|O_CLOEXEC 0600`, RIFF 4 GiB cap) | Recorder logic portable; file open is cross-area (output.c). Doc says split files are `-002.wav`; **current code** opens a new time-stamped file (`-N` only on same-second collision). |
| Channel line inputs | `RG_MIXER` (engine line/tap) + channel inputs (channel_input.c) | Engine API unchanged. |
| Icecast / SHOUTcast 1/2 | `RG_STREAM` → `radios_audio_output_start_server()` (libcurl PUT, GSocket SHOUTcast), 5 s reconnect, metadata | Portable (cross-area notes in §6.6). |

---

## 2. PulseAudio feature usage — exhaustive list

| Pulse feature | Where | Purpose | Windows equivalent |
|---|---|---|---|
| `pa_threaded_mainloop` + `pa_context` (+`PA_CONTEXT_NOFAIL`, proplist `application.name`, `media.category=Manager`) | patchbay.c:308-360, 731-767 | Long-lived server connection | `RadioSAudioHost` (COM MTA, `IMMDeviceEnumerator`) |
| `pa_mainloop` + private `pa_context` (`PA_CONTEXT_NOAUTOSPAWN`, `application.id`) | channel_input.c:264-297 | One connection per channel input | Host stream on the input's worker thread |
| Introspection: `get_sink_info_list` (name, description, index) | patchbay.c:245-251 | Output device list | `EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE)` + `PKEY_Device_FriendlyName` |
| `get_source_info_list` (`monitor_of_sink`) | patchbay.c:252-261 | Input + monitor list | `EnumAudioEndpoints(eCapture)` + synthesized `<render id>.monitor` entries |
| `get_sink_input_info_list` (`application.name`, `application.process.binary`, `index`, `sink`) | patchbay.c:262-278 | Per-application streams | `IAudioSessionManager2::GetSessionEnumerator` per render endpoint, `IAudioSessionControl2::GetProcessId/IsSystemSoundsSession`, `GetState`, `GetDisplayName`; `QueryFullProcessImageNameW`, version-info `FileDescription` |
| `get_module_info_list` / `load_module` / `unload_module` (`module-null-sink`, `module-remap-source`) | patchbay.c:224-234, 301-306, 341-355, 579-602, 542-546 | Virtual sink/mic, crash cleanup | **None** (no user-mode endpoint creation). Block semantics changed (§6.3). |
| `move_sink_input_by_name` | patchbay.c:888-902 | Move app into virtual sink | **None** (undocumented `IAudioPolicyConfig` per-app device routing is rejected — persisted host change). Replaced by process-loopback binding. |
| `pa_context_subscribe(SINK|SOURCE|SINK_INPUT)` | patchbay.c:336-340 | Hot-plug / app list refresh | `IMMNotificationClient` (devices) + 1 s session poll (or `IAudioSessionNotification`) |
| Record streams `pa_stream_connect_record` (FLOAT32NE/48k/2, fragsize, ADJUST_LATENCY, DONT_MOVE), peek/drop, holes | patchbay.c:148-161, 194-197; channel_input.c:204-219, 247-250 | Device capture | `IAudioClient` shared, `IAudioCaptureClient`, event-driven, AUTOCONVERTPCM |
| Monitor sources `<sink>.monitor` | patchbay.c:608-609; routing_graph.c | Capture what an output plays | `AUDCLNT_STREAMFLAGS_LOOPBACK` on the render endpoint |
| Playback streams `pa_stream_connect_playback` (tlength 80 ms, minreq 10 ms, ADJUST_LATENCY, DONT_MOVE), begin_write/write | patchbay.c:162-176, 190-193 | Output blocks, feed virtual sink | `IAudioClient` shared render + `IAudioRenderClient`, event-driven |
| Stream properties `media.name`, `media.role=production` | patchbay.c:183-186; channel_input.c:239-243 | Labels in mixer / role routing | `IAudioClient2::SetClientProperties{eCategory=AudioCategory_Media}` + session `SetDisplayName("RadioS · …")` + `SetDuckingPreference(TRUE)` |
| `pa_strerror(pa_context_errno())` | patchbay.c:318; channel_input.c:226, 251, 257 | Error text | HRESULT → Turkish message table (§9) |
| Server string (`unix:/…`, `PULSE_SERVER`) | patchbay `new(pulse_server)`, channel_input `new(…, pulse_server)`, tests | Select server / keep tests off host audio | Host spec string + `RADIOS_AUDIO_HOST` env (§4.4) |
| Latency: Pulse `ADJUST_LATENCY` | as above | Low latency | WASAPI buffer durations (§5.6) |

---

## 3. Target architecture

```
                 +----------------------------------------------+
 engine backend  |  src/audio/audio_host.h  (internal, shared)  |   tools/radios_hardware_check (optional CLI)
 (other area) -->|  devices / applications / streams / status   |<-- patchbay.c, channel_input.c
                 +-------+-------------------+------------------+
                         |                   |                  |
             audio_host_wasapi.c   audio_host_fixture.c   audio_host_pulse.c (Linux; existing code moved)
             (Windows product)     (tests, CI, all OSes)  (optional: or keep #ifdef'd Pulse code)
```

* Keep the portable graph runtime (`reconcile/tick/recorders/streams/intents/snapshot`) in `patchbay.c`; move everything Pulse-specific behind the host API. Linux keeps working by moving existing Pulse code into `audio_host_pulse.c` (cheap: link_open/close, inventory, modules, move) — or, if the lead prefers minimum Linux churn, guard the current Pulse code with `#ifndef G_OS_WIN32` and add `patchbay_host_win.c`. The spec assumes the host API.
* WASAPI code lives in its own TU(s) compiled only on `WIN32`, with `#define COBJMACROS`, `#define WIN32_LEAN_AND_MEAN`, `#include <initguid.h>` in exactly one TU (defines `IID_IAudioClient`, `IID_IAudioCaptureClient`, `IID_IAudioRenderClient`, `IID_IAudioClient2`, `IID_IAudioSessionManager2`, `IID_IAudioSessionControl2`, `IID_IMMNotificationClient`, `CLSID_MMDeviceEnumerator`, `IID_IMMDeviceEnumerator`, `PKEY_Device_FriendlyName`, `KSDATAFORMAT_SUBTYPE_IEEE_FLOAT`, `IID_IAgileObject` …). Never include `<windows.h>` in `patchbay.c`/`channel_input.c` (macro pollution: `interface`, `ERROR`, `small`, …).

### 3.1 Proposed internal API `src/audio/audio_host.h`

```c
#define RADIOS_AUDIO_HOST_RATE 48000u      /* every stream is 48 kHz float32 interleaved stereo */
typedef struct RadioSAudioHost RadioSAudioHost;
typedef struct RadioSAudioHostStream RadioSAudioHostStream;

typedef struct {
    gchar name[RADIOS_AUDIO_DEVICE_NAME_SIZE];        /* persisted identifier — naming contract §4 */
    gchar description[RADIOS_AUDIO_DEVICE_NAME_SIZE]; /* UTF-8 friendly name */
    guint channels;                                    /* endpoint mix-format channels (info) */
    gboolean monitor;                                  /* capture list: loopback of a render endpoint */
    gboolean is_default;                               /* eConsole default of its flow */
    guint32 index;                                     /* stable for the host lifetime (id→counter map) */
} RadioSAudioHostDevice;

typedef struct {
    guint32 pid;                         /* Windows: process id; Pulse: sink-input index */
    guint64 start_time;                  /* process creation FILETIME (100 ns); 0 on Pulse */
    gchar name[RADIOS_PATCHBAY_NAME_SIZE];   /* display name */
    gchar binary[257];                   /* "spotify.exe" (Windows) / "firefox" (Linux) */
    guint32 endpoint_index;              /* RadioSAudioHostDevice.index of its render endpoint */
    gboolean active;                     /* AudioSessionStateActive */
} RadioSAudioHostApplication;

typedef struct {
    const gchar *device;      /* capture: "<capture id>" | "<render id>.monitor" | "@default" | ""; render: "<render id>" | "@default" */
    guint32 pid;              /* != 0 → application (process-loopback) capture; device ignored */
    guint64 start_time;       /* must match the process, else fail "Uygulama kimliği değişti…" */
    gboolean include_tree;    /* PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE (default TRUE) */
    const gchar *title;       /* "RadioS · <block title>" → session display name / Pulse media.name */
    guint period_frames;      /* 480 (10 ms) */
    guint buffer_frames;      /* capture: endpoint buffer (≥ 4800 = 100 ms); render: total latency (3840 = 80 ms) */
} RadioSAudioHostStreamSpec;

/* NULL/"" → $RADIOS_AUDIO_HOST if set, else system (WASAPI on Windows, default Pulse on Linux).
 * "fixture:<json>" → in-process fixture; "none:" (and, on Windows, any "unix:…") → never connects. */
RadioSAudioHost *radios_audio_host_acquire(const gchar *spec);            /* refcounted, shared per spec */
void radios_audio_host_release(RadioSAudioHost *host);
gboolean radios_audio_host_status(RadioSAudioHost *host, guint64 *epoch, gchar error[256]); /* thread-safe */
guint radios_audio_host_devices(RadioSAudioHost *host, gboolean capture, RadioSAudioHostDevice *out, guint capacity);
guint radios_audio_host_applications(RadioSAudioHost *host, RadioSAudioHostApplication *out, guint capacity);
gboolean radios_audio_host_describe(RadioSAudioHost *host, const gchar *name,
    gchar description[RADIOS_AUDIO_DEVICE_NAME_SIZE], gboolean *present); /* friendly name of an unplugged endpoint */
gboolean radios_audio_host_application_capture_supported(RadioSAudioHost *host, gchar reason[256]);
void radios_audio_host_refresh(RadioSAudioHost *host);                   /* async rescan */
guint radios_audio_host_watch(RadioSAudioHost *host, void (*changed)(gpointer), gpointer data); /* host thread */
void radios_audio_host_unwatch(RadioSAudioHost *host, guint id);         /* returns after any running callback */

typedef void (*RadioSAudioHostCaptureFunc)(const gfloat *stereo /* NULL = silence */, guint frames, gpointer data);
typedef void (*RadioSAudioHostRenderFunc)(gfloat *stereo, guint frames, gpointer data); /* must fill all frames */
/* Blocks until running or timeout (patchbay 3 s, channel input 15 s). NULL + Turkish error on failure.
 * Callbacks run on the stream's own I/O thread (MMCSS "Pro Audio"). */
RadioSAudioHostStream *radios_audio_host_open_capture(RadioSAudioHost *host, const RadioSAudioHostStreamSpec *spec,
    RadioSAudioHostCaptureFunc func, gpointer data, gint64 timeout_us, gchar error[256]);
RadioSAudioHostStream *radios_audio_host_open_render(RadioSAudioHost *host, const RadioSAudioHostStreamSpec *spec,
    RadioSAudioHostRenderFunc func, gpointer data, gint64 timeout_us, gchar error[256]);
gboolean radios_audio_host_stream_running(RadioSAudioHostStream *stream);              /* atomic */
gboolean radios_audio_host_stream_failed(RadioSAudioHostStream *stream, gchar error[256]); /* atomic + message */
void radios_audio_host_stream_close(RadioSAudioHostStream *stream); /* stop + join; no callback after return */
```

Ownership/threading contract: every host call is thread-safe; no host call may be made from the GTK thread for WASAPI work except the cheap snapshot getters (they only copy cached lists under a mutex). The engine area must use the same `radios_audio_host_devices()` for its `RadioSAudioSnapshot.inputs/outputs` so DJ “in:<name>”, audio preferences and routing all share identifiers.

---

## 4. Identifier & persistence contract

### 4.1 What is persisted today (Linux)

| Store | Field | Linux value | Ephemeral data explicitly **not** stored |
|---|---|---|---|
| DB `audio_routing` schema 2 (`routing.c:176-192`) | `configs[].graph.nodes[].settings.device` for input/microphone/output | Pulse sink/source name, e.g. `alsa_input.usb-…`, `alsa_output.x.monitor`, `radios_virtual_5_mic`, `@talk` | — |
| same | VIRTUAL `settings` | `{"as_microphone": bool}` only — **the chosen application is not persisted** (move is a live action) | Pulse indices, running/recording state, file path, passwords (unless “Şifreyi kaydet”) |
| same | RECORDER `settings.folder` | absolute POSIX path or `""` | current file path, counters |
| DB `audio_routing` schema 1 (legacy, `desktop_api.py:656-679`, `test_desktop_routing.py`) | `application_name` (≤512), `application_binary` (≤256), `output_sink`, `microphone_source` (≤512), gains, `compressor` | Pulse names / process binary | `application_index`, `sink_input_index`, `running`, `recording`, `recording_path`, `password`, `api_token` are **rejected** (tests assert 422) |
| DB `dj_mixer` (`audio_ui.c:277-282`) | `channels[i].input = {kind:"capture"|"network", device, name}` | Pulse source name or URL | runner state |
| DB `audio_preferences` (engine area, `audio_ui.c:165-187`) | `main_sink`, `cue_sink`, `monitor_sink`, `microphone_source` | Pulse names or `"@default"` | — |
| INI (`config/radios.ini`) | `[paths] recording_root` | relative to RadioS root (config area) | — |

### 4.2 Windows naming contract (all persisted strings)

| Logical device | Persisted `name` | `description` | Notes |
|---|---|---|---|
| Render endpoint | `IMMDevice::GetId()` verbatim, e.g. `{0.0.0.00000000}.{8f2b…-…}` (55 ASCII chars) | `PKEY_Device_FriendlyName` (UTF-16→UTF-8) | Stable across reboots; may change after driver reinstall or (USB without serial) port change. |
| Capture endpoint | `{0.0.1.00000000}.{…}` | friendly name | |
| Loopback (“monitor”) of a render endpoint | `<render id>.monitor` | friendly name (UI adds “Çıkış monitörü · ” / “ · Çıkış geri dinlemesi”) | `monitor = TRUE`. Keeps `implicit()` loop rule valid. |
| System default | `@default` | “Sistem varsayılan girişi/çıkışı” | `GetDefaultAudioEndpoint(flow, eConsole)` resolved at open time. |
| On-air talk mic | `@talk` | unchanged | engine tap. |
| RadioS virtual block | `radios_virtual_<id>` / `.monitor` / `_mic` | — | Reserved logical names; **not offered as devices on Windows**; saved references show as “Kullanılamıyor”. |
| Fixture host | free strings (e.g. `routing_original`, `routing_original.monitor`) | from JSON | Lets the existing UI tests keep their ids. |

Rules:
* **No prefixes** (`wasapi:` etc.) — `routing_graph.c` copies device names into fixed buffers; keep names ≤ 512 bytes (TEXT_SIZE) and fix the 64/80-byte buffers (§1.3).
* **No silent substitution:** a saved id that is not present stays selected and is shown as unavailable (use `radios_audio_host_describe()` to show the friendly name of an unplugged endpoint: “Bağlı değil · Hoparlör (USB Audio)” instead of the raw GUID). Never fall back to “same friendly name” or to the default device.
* Linux documents opened on Windows (and vice versa) simply show unavailable devices; nothing is rewritten automatically.

### 4.3 Application identity

* Runtime identity (never persisted): `pid` + `start_time` (creation FILETIME from `GetProcessTimes`). `RadioSPatchApplication.index` = PID (fits `guint32`; UI combo id stays the decimal index).
* While bound, the patchbay holds `OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION)`; an open handle **prevents PID reuse**, and `WaitForSingleObject(h, 0) == WAIT_OBJECT_0` detects exit (“Kaynak kaybı … durdurur. Otomatik yeniden yakalama yoktur.”).
* Optional persisted **selection hint** (recommended, needs SDBASE + C validator change): add optional VIRTUAL settings `application_binary` (exe basename, lower-cased, ≤256 bytes, no `/` or `\`) and `application_name` (≤512). Written only when non-empty (so existing documents and `graph_equal()` stay byte-identical). Used only to **preselect** the app combo on reopen; capture always needs the explicit operator click (mirrors Linux, where a restart leaves the app outside the virtual sink). Auto-bind on “Yönlendirmeyi başlat” when exactly one matching session exists is a possible later enhancement — lead decision.
  * C: `routing_graph.h` node gets `gchar application_binary[257], application_name[513]`; `settings_from_json` VIRTUAL switches from `exact_keys` to `keys_with_optional(…, {"application_binary","application_name"})`; `settings_to_json` emits them only if non-empty; `node_valid` checks `text()` and basename rule.
  * Python `desktop_api.py:391-392` `_ROUTING_SETTINGS_OPTIONAL["virtual"] = {"application_binary", "application_name"}` + same checks.
  * Still forbidden in any document: PID, start time, session GUIDs, endpoint indices.

### 4.4 Host spec strings (replaces `pulse_server`)

* `radios_audio_patchbay_new(const gchar *pulse_server)` and `radios_audio_channel_input_new(…, const gchar *pulse_server, …)`: rename the parameter to `audio_host`; semantics: NULL/"" → `$RADIOS_AUDIO_HOST` if set, else system host. `RADIOS_AUDIO_HOST` is the Windows analogue of `PULSE_SERVER` (libpulse reads `PULSE_SERVER` implicitly, which is how the Linux test runners redirect the whole process).
* `"none:"` → never connects (`connected=FALSE`, error “Ses sistemine bağlanılamadı.”). On Windows, any `"unix:…"` spec is treated as `"none:"` so `tests/test_patchbay.c:149,214` (`"unix:/nonexistent/radios-patchbay-test"`, used to keep the test off host audio) keeps its meaning without edits.
* `"fixture:<absolute json path>"` → fixture host (§10.2).

---

## 5. WASAPI host implementation (`audio_host_wasapi.c`)

### 5.1 Threads and COM

* One host **service thread** per process (created on first acquire): `CoInitializeEx(NULL, COINIT_MULTITHREADED)`, creates `IMMDeviceEnumerator` (`CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, …)`), registers `IMMNotificationClient`, performs (re)enumeration, polls sessions every 1 s while any watcher exists, polls Audiosrv state.
* One **I/O thread per stream**: `CoInitializeEx(MTA)`, `AvSetMmThreadCharacteristicsW(L"Pro Audio", &task)` (avrt), opens & initializes the client itself, signals “running” or failure to the opener (opener waits ≤ timeout on an event), then loops `WaitForMultipleObjects({stream_event, stop_event}, FALSE, 10 ms)`. Close = `SetEvent(stop)`, join, `AvRevertMmThreadCharacteristics`, `CoUninitialize`.
* Max simultaneous streams in the patchbay: 8 inputs + 8 mics + 8 outputs + 8 virtual (+ optional keep-alive renders) ≈ 40 threads — acceptable; a single multiplexed thread is limited to 64 handles and adds complexity.
* The GTK thread is an OLE STA (GDK-Win32 calls `OleInitialize` for DnD). **Never** create or call WASAPI objects from the GTK thread; `radios_audio_patchbay_move_application()` must only validate, open the process handle and record the binding (no COM) — the I/O thread does the activation.
* Callbacks from `IMMNotificationClient` arrive on arbitrary MTA threads: only bump a generation counter and signal (`g_cond_signal`); never block, never call `Release` on the enumerator, never re-enter enumeration there. Unregister before releasing the enumerator.

### 5.2 Device enumeration

```
EnumAudioEndpoints(eRender,  DEVICE_STATE_ACTIVE) → outputs[]  {GetId, FriendlyName, mix channels}
EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE) → inputs[]   {…, monitor=FALSE}
for each output: inputs[] += {name = id + ".monitor", description = friendly, monitor = TRUE}
GetDefaultAudioEndpoint(eRender/eCapture, eConsole) → is_default
```
* Limit 64 per list (`RADIOS_PATCHBAY_MAX_DEVICES`, `RADIOS_AUDIO_MAX_DEVICES`); skip names ≥ 513 bytes (cannot happen with GUID ids).
* Publish atomically (copy staged arrays under the host mutex), like `sink_listed/source_listed` publish at list end and `pulse_backend.c` hot-plug lists (`tests/test_pulse_hotplug.c` contract: never a half-filled list).
* Coalesce bursts: notification → mark due in 250 ms (same as `pulse_backend.c:258-264`).
* `index`: a per-host `GHashTable id→guint32` counter so the same endpoint keeps its index across rescans.

### 5.3 Application (session) enumeration

For every active render endpoint (cache `IAudioSessionManager2` per endpoint):
```c
IMMDevice_Activate(dev, &IID_IAudioSessionManager2, CLSCTX_ALL, NULL, (void **)&mgr);
IAudioSessionManager2_GetSessionEnumerator(mgr, &list);   /* also needed once before RegisterSessionNotification */
IAudioSessionEnumerator_GetCount(list, &n);
for i: GetSession(i,&ctl); QI IAudioSessionControl2;
   if (IAudioSessionControl2_IsSystemSoundsSession(ctl2) == S_OK) skip;          /* system sounds, PID 0 */
   hr = IAudioSessionControl2_GetProcessId(ctl2, &pid);                           /* AUDCLNT_S_NO_SINGLE_PROCESS → skip */
   IAudioSessionControl_GetState(ctl, &state);                                    /* list Active; keep Inactive only if currently bound */
   IAudioSessionControl_GetDisplayName(ctl, &wname);                              /* may be "" or "@%SystemRoot%\…,-123" */
```
Per PID (dedupe across endpoints, `endpoint_index` = first endpoint where Active):
* `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION)` → `QueryFullProcessImageNameW` → basename (`binary`, UTF-8, keep original case for display; compare case-insensitively) and `GetProcessTimes` → `start_time`.
* Display `name`: session display name (resolve `@`-resources with `SHLoadIndirectString`), else version-info `FileDescription` (`GetFileVersionInfoSizeW/GetFileVersionInfoW/VerQueryValueW("\\VarFileInfo\\Translation")` then `"\\StringFileInfo\\%04x%04x\\FileDescription"`), else basename without `.exe`, else “Uygulama”. Access denied (protected processes) → `name = "PID <n>"`, `binary=""`.
* Filter own process (`pid == GetCurrentProcessId()`), basename `radios.exe` (case-insensitive) and names starting with `RadioS` (parity with `own_application()`).
* Sort deterministically (name, then pid) so the UI inventory hash does not churn.
* Browsers render from a child audio process (e.g. `chrome.exe` utility, `msedge.exe`, `firefox.exe` RDD): the session PID is that child; capturing it with `INCLUDE_TARGET_PROCESS_TREE` captures the browser's audio. Fine.
* Change detection: session list polled every 1 s by the service thread (only while a watcher exists); generation bumps only if the published content differs. Optionally add `IAudioSessionNotification::OnSessionCreated` for faster appearance.

### 5.4 Status / connection

* `connected = enumerator != NULL && Audiosrv running` (`OpenSCManagerW(NULL,NULL,SC_MANAGER_CONNECT)` + `OpenServiceW(L"Audiosrv", SERVICE_QUERY_STATUS)` + `QueryServiceStatusEx`, cached 1 s; read-only, allowed for standard users). Also treat `AUDCLNT_E_SERVICE_NOT_RUNNING` from any client as “disconnected”.
* Retry creating the enumerator every 2 s if it failed (parity with patchbay.c:323-324).
* `epoch` increments each time the host goes connected again (service restart) → `reconcile()` reopens every link (parity with `++pb->epoch` on Pulse READY).

### 5.5 Process loopback (per-application capture)

Missing header → declare manually (C11 anonymous union is fine with GCC):

```c
#if defined(__has_include)
#  if __has_include(<audioclientactivationparams.h>)
#    include <audioclientactivationparams.h>
#    define RADIOS_HAVE_ACTIVATION_PARAMS 1
#  endif
#endif
#ifndef RADIOS_HAVE_ACTIVATION_PARAMS
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
typedef enum AUDIOCLIENT_ACTIVATION_TYPE {
    AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
} AUDIOCLIENT_ACTIVATION_TYPE;
typedef enum PROCESS_LOOPBACK_MODE {
    PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} PROCESS_LOOPBACK_MODE;
typedef struct AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
    DWORD TargetProcessId;
    PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;
typedef struct AUDIOCLIENT_ACTIVATION_PARAMS {
    AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union { AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams; };
} AUDIOCLIENT_ACTIVATION_PARAMS;
#endif
```
`ActivateAudioInterfaceAsync`, `IActivateAudioInterfaceAsyncOperation`, `IActivateAudioInterfaceCompletionHandler` are declared in MSYS2 `mmdeviceapi.h:797-955`; link `-lmmdevapi` (verified: `libmmdevapi.a` exports `ActivateAudioInterfaceAsync`).

Completion handler in C (must be agile — answer `IID_IAgileObject`, otherwise activation can fail with `E_ILLEGAL_METHOD_CALL`):
```c
typedef struct { IActivateAudioInterfaceCompletionHandler iface; LONG refs; HANDLE done; } Activation;
static HRESULT STDMETHODCALLTYPE act_qi(IActivateAudioInterfaceCompletionHandler *self, REFIID riid, void **out)
{
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IActivateAudioInterfaceCompletionHandler) ||
        IsEqualIID(riid, &IID_IAgileObject)) { *out = self; self->lpVtbl->AddRef(self); return S_OK; }
    *out = NULL; return E_NOINTERFACE;
}
/* AddRef/Release: InterlockedIncrement/Decrement; free (CloseHandle(done), g_free) at 0. */
static HRESULT STDMETHODCALLTYPE act_completed(IActivateAudioInterfaceCompletionHandler *self,
                                               IActivateAudioInterfaceAsyncOperation *op)
{ (void)op; SetEvent(((Activation *)self)->done); return S_OK; }
```
Activation on the stream's MTA I/O thread:
```c
AUDIOCLIENT_ACTIVATION_PARAMS params = {.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK,
    .ProcessLoopbackParams = {.TargetProcessId = pid,
                              .ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE}};
PROPVARIANT pv; PropVariantInit(&pv);
pv.vt = VT_BLOB; pv.blob.cbSize = sizeof params; pv.blob.pBlobData = (BYTE *)&params; /* do NOT PropVariantClear */
hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, &IID_IAudioClient, &pv, &handler->iface, &op);
WaitForSingleObject(handler->done, 3000);                    /* patchbay link timeout */
IActivateAudioInterfaceAsyncOperation_GetActivateResult(op, &activate_hr, &unknown);
IUnknown_QueryInterface(unknown, &IID_IAudioClient, (void **)&client);
WAVEFORMATEX fmt = {WAVE_FORMAT_IEEE_FLOAT, 2, 48000, 48000 * 8, 8, 32, 0};
hr = IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
        1000000 /* 100 ms */, 0, &fmt, NULL);
/* fallback if rejected: WAVE_FORMAT_PCM 48000/2/16 and convert s16→float */
IAudioClient_SetEventHandle(client, event); IAudioClient_GetService(client, &IID_IAudioCaptureClient, …);
IAudioClient_Start(client);
```
Pitfalls / semantics to preserve:
* Process-loopback clients do **not** support `GetMixFormat`/`GetDevicePeriod` (E_NOTIMPL) — always pass an explicit format; never pass flags as `hnsPeriodicity` (bug in Microsoft's sample).
* Verify identity before activation: `OpenProcess` + `GetProcessTimes` creation time must equal the `start_time` from the inventory, else fail “Uygulama kimliği değişti; listeyi Yenile ile güncelleyin.”.
* Version requirement: Microsoft documents process loopback for Windows 10 build 20348+/Windows 11; in practice it works from Windows 10 2004 (build 19041) (OBS “Application Audio Capture” requires 2004+). **Runtime probe**, never version-gate: on `E_NOTIMPL`, `E_INVALIDARG`, `E_NOINTERFACE` or `ERROR_*` from activation, cache “unsupported” in the host and report “Bu Windows sürümü uygulama sesi yakalamayı desteklemiyor (Windows 10 sürüm 2004 veya üzeri gerekir).”. **No fallback** to the mixed endpoint loopback (doc: “karma sistem monitor'ına geri dönüş yapılmaz”).
* Not capturable: exclusive-mode WASAPI, ASIO/WDM-KS renderers (no sessions; common in DJ software), protected (PMP/DRM) content (captured as silence).
* Whether the app's own Volume Mixer level/mute affects the capture must be verified in the spike; RadioS must **not** change session volume/mute (Windows persists per-app volume/mute → a crash would leave the user's app muted).
* Capture of elevated/protected processes from a non-elevated RadioS: verify; map `E_ACCESSDENIED` to “Bu uygulamanın sesi yakalanamıyor (erişim reddedildi).”.
* Packets may stop while the app is silent; patchbay `pull()` already treats missing data as underrun → silence.
* Optional extra (not required): `EXCLUDE_TARGET_PROCESS_TREE` with RadioS's own PID gives “all system audio except RadioS” without feedback — could be offered later as input `@system-except-radios`.

### 5.6 Endpoint capture and endpoint loopback (monitor)

```c
IMMDeviceEnumerator_GetDevice(e, wid, &dev)   /* id → device; "<id>.monitor" → render id, loopback */
IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void **)&client);
IAudioClient_GetMixFormat(client, &mix);       /* channels for our own up/down-mix */
fmt = WAVEFORMATEXTENSIBLE {float32, 48000, nChannels = mix->nChannels, channel mask = mix's};
flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY
        (| AUDCLNT_STREAMFLAGS_LOOPBACK for monitors);
IAudioClient2_SetClientProperties(c2, &(AudioClientProperties){sizeof, FALSE, AudioCategory_Media, AUDCLNT_STREAMOPTIONS_NONE});
IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, flags, 1000000 /* 100 ms */, 0, &fmt, &RADIOS_PATCHBAY_SESSION_GUID);
```
* Read loop: `GetNextPacketSize` → `GetBuffer(&data,&frames,&bufflags,…)`; `AUDCLNT_BUFFERFLAGS_SILENT` → callback with `NULL` (silence, like Pulse holes); `DATA_DISCONTINUITY` → count only; `ReleaseBuffer`.
* Channel handling (deterministic levels, Pulse parity): 1 ch → L=R=x (Pulse duplicates mono at full scale); 2 ch → as is; >2 ch (5.1/7.1 loopback) → `swresample` downmix with the standard matrix (FFmpeg is already linked). Do not rely on Windows' channel matrixer for mono→stereo levels.
* If `Initialize` returns `AUDCLNT_E_UNSUPPORTED_FORMAT`: retry with the mix format itself and resample with `swresample` to 48 k.
* Loopback specifics: event-driven loopback is not signalled while nothing is rendered on the endpoint → always wait with a 10 ms timeout and poll `GetNextPacketSize`. Recommended: while a loopback capture is open, keep a **silent keep-alive render stream** on that endpoint so the monitor clock delivers continuous (silent) packets like a Pulse monitor source (avoids priming gaps/underrun storms).
* Loopback includes RadioS's own output on that endpoint (same as Pulse monitors); the graph loop validator already forbids output→own monitor cycles.
* Errors: `AUDCLNT_E_DEVICE_INVALIDATED` (unplug) → failed; `E_ACCESSDENIED` on capture endpoints = Windows microphone privacy (“Uygulamaların mikrofona erişmesine izin ver” / desktop apps) → message §9; `AUDCLNT_E_DEVICE_IN_USE` (another app exclusive) → message.
* Pitfalls: opening a Bluetooth headset microphone switches it to HFP (output quality drop) — document; Windows mic enhancement APOs (AGC/NS) apply by default — optional advanced `AUDCLNT_STREAMOPTIONS_RAW` when `PKEY_Devices_AudioDevice_RawProcessingSupported` is TRUE (not default).
* Session hygiene: all RadioS patchbay streams use one fixed session GUID (`RADIOS_PATCHBAY_SESSION_GUID`, a constant) with `IAudioSessionControl::SetDisplayName(L"RadioS · Yönlendirme")`, and `IAudioSessionControl2::SetDuckingPreference(TRUE)` so opening a microphone never triggers Windows communications ducking of other apps (host audio must not change).

### 5.7 Render (OUTPUT block, keep-alive)

* Same activation; `AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUTOCONVERTPCM | SRC_DEFAULT_QUALITY`, `hnsBufferDuration = 800000` (80 ms ≙ Pulse `tlength = KEEP*8*2`), period 10 ms. On each event: `GetCurrentPadding` → `available = bufferFrames - padding` → `GetBuffer(available)` → render callback (the existing `link_write` priming/trim/zero-fill logic, `PRIME 960`, `LIMIT 9600`, `KEEP 1920`) → `ReleaseBuffer(available, 0)`. Pre-fill with silence before `Start`.
* Explicit endpoint id never follows default-device changes (≙ `PA_STREAM_DONT_MOVE`). `"@default"` resolved at open.
* If the endpoint mix is not stereo, AUTOCONVERTPCM upmixes stereo → N channels (acceptable for outputs).

---

## 6. Patchbay changes (`src/audio/patchbay.c`, `include/radios/audio_patchbay.h`)

### 6.1 Struct/API changes

* `audio_patchbay.h:10-13` comment “owns a Pulse connection” → “uses the audio host (WASAPI on Windows, Pulse on Linux)”.
* `audio_patchbay.h:29-32` `RadioSPatchApplication`: keep `index` (PID on Windows), `sink` (render endpoint index; on Windows the device where the session plays), `name`, `binary`; **append** `guint64 start_time; guint bound_node; gboolean active;` (in-tree struct, no ABI concerns).
* `audio_patchbay.h:39-40` `RadioSPatchNodeSnapshot.sink/source` (“Pulse names other applications see”): empty on Windows; **append** `guint32 application_pid; gchar application[RADIOS_PATCHBAY_NAME_SIZE]; gchar application_binary[257];` (bound app of a VIRTUAL block).
* `audio_patchbay.h:66` `radios_audio_patchbay_new(const gchar *pulse_server)` → parameter `audio_host` (§4.4).
* `audio_patchbay.h:98-100` `move_application` doc: “Linux: moves the sink-input into the block's virtual sink. Windows: binds the application (snapshot index = PID) as the block's capture source; nothing is moved; the application keeps playing on its own output.”
* New (Windows UI needs it, harmless on Linux): `void radios_audio_patchbay_release_application(RadioSAudioPatchbay *pb, guint node);` → “Yakalamayı bırak”.
* Replace `PA_INVALID_INDEX` uses inside the core with `RADIOS_PATCH_INVALID_INDEX` (`G_MAXUINT32`).

### 6.2 `reconcile()` (553-679) mapping

| Linux branch | Windows behaviour |
|---|---|
| 579-602 VIRTUAL: reuse or (re)create `module-null-sink` (+ `module-remap-source` if `as_microphone`), errors “Sanal ses arayüzü oluşturulamadı (module-null-sink).”, “Sanal mikrofon oluşturulamadı.”, “Ses sunucusuna bağlanılıyor…” | No endpoint creation. Nothing to do while stopped. `as_microphone` ignored (UI explains, §6.7). |
| 605-607 INPUT / MICROPHONE (device ≠ `@talk`) capture; empty device → “Bir giriş aygıtı seçin.” | `radios_audio_host_open_capture({.device = c->device})` — device may be a capture id, `<render id>.monitor` or `@default`. |
| 608-611 VIRTUAL running: capture `<sink>.monitor`; if the block has incoming cables also playback into `<sink>` | If the block has a binding: `open_capture({.pid, .start_time, .include_tree = TRUE})`. **No playback link**: in `tick_locked()`, before the gain loop, `if (kind == RG_VIRTUAL) n->out[i] += n->in[i]` (Windows: internal sum; Linux keeps the null-sink path). No binding → no capture; block output = its cabled input only. |
| 612-614 OUTPUT playback; empty device → “Bir çıkış aygıtı seçin.” | `open_render({.device = c->device})`. |
| 615 title `"RadioS · <title>"` | Same string → used for logging; session display name is the fixed patchbay name (one session). |
| 617/621 `link_reusable(old, device, epoch)` | Same rule plus, for app captures, same `pid`/`start_time`. |
| 624-645 engine taps/lines | Unchanged (engine API). |

Opening links sequentially with a 3 s timeout each can make a reconcile take long when many devices hang; recommended improvement (both OSes): start all link opens, then wait once with a common deadline.

### 6.3 Application binding (Windows replacement for `move_application`, 888-902)

```
radios_audio_patchbay_move_application(pb, application /* PID */, node, error):
  lock; n = live_find(node); require n && n->config.kind == RG_VIRTUAL  else "Uygulama sesi bloğu bulunamadı."
  app = inventory lookup by index == application   else "Uygulama artık ses çalmıyor; Yenile ile listeyi güncelleyin."
  unlock
  if (!radios_audio_host_application_capture_supported(host, reason)) fail(reason)
  watch = radios_platform_process_watch_open(app.pid, app.start_time, &err)  (identity check + pinned handle)
  lock; drop any other block's binding of the same pid (Linux "move" is exclusive);
  pb->bindings[node] = {pid, start_time, name, binary, watch}; ++desired_serial; signal control; unlock
  return TRUE  → reconcile opens the capture when running
```
* Bindings live in a `GHashTable node→Binding` on `pb` (survive `reconcile()` replacing `Live` objects), removed when the block disappears from the desired graph, on `release_application`, on unref, or on process exit.
* `housekeeping()` (680-705): for each binding, `radios_platform_process_watch_exited()` → close capture, drop binding, set sticky node error “Uygulama kapandı; yakalama durduruldu.” (kept until a new bind / block edit), resync. No automatic re-capture.
* Snapshot: `applications[i].bound_node` = block id; for the block: `application_pid/application/application_binary`.
* State rules (929-933): Linux keeps VIRTUAL `ACTIVE` while stopped because the device exists; Windows: stopped VIRTUAL → `IDLE`; running and bound → link-driven `STARTING/ACTIVE/FAILED`; running and unbound → `ACTIVE` (existing “no resources” rule) with detail “Uygulama seçilmedi”.
* User-visible semantic difference (must be documented in UI/doc): on Linux the app is moved into a silent RadioS sink and heard once through MAIN; on Windows the app **also keeps playing on its own output device**. Operators avoid double hearing by setting that app's output (Windows “Uygulama ses düzeyi ve cihaz tercihleri”) to a device that is not monitored, or by muting it themselves. RadioS never changes per-app device routing or session mute (both are persisted by Windows → would survive a crash).
* Virtual microphone for other apps (`as_microphone`) is impossible without a kernel driver. Windows UI: checkbox hidden/disabled with tooltip “Windows'ta sanal mikrofon için VB-CABLE gibi bir sanal ses kablosu sürücüsü gerekir; bir Ses çıkışı bloğunu o kablonun girişine bağlayın.” The JSON field is kept (documents stay portable).

### 6.4 Connection/inventory/snapshot

* `connection()` (308-360) → `radios_audio_host_status(host, &epoch, error)`; on epoch change set `fresh = TRUE` and reinventory; `snapshot.error` from host when disconnected (“Windows ses hizmeti çalışmıyor.” etc.).
* `inventory()` (279-288) → copy `radios_audio_host_devices(FALSE/TRUE)` into `outputs/inputs` (`RadioSPatchDevice{index,name,description,monitor}`) and `radios_audio_host_applications()` into `apps` (`index=pid, sink=endpoint_index, name, binary, start_time, active`), filtering own process.
* `subscribed()` (289-293) → `radios_audio_host_watch(host, changed_cb, pb)` where the callback sets `refresh_requested` and signals `control_wake` under `pb->mutex`; `radios_audio_host_unwatch()` in unref **before** joining threads.
* Stale cleanup (`cleaned`, 301-306, 341-355): Linux-only.
* `control()` thread: `radios_platform_com_init_mta()` at start / uninit at end (Windows); Pulse impl ignores.

### 6.5 Recorder paths (`record_open/record_path/path_taken`, 369-397)

* Folder rule: `record_open` currently requires `g_path_is_absolute(folder)`. On Windows GLib returns TRUE for drive-relative `\foo` and `/tmp/radios` (rooted, not fully qualified). Use `radios_platform_path_is_fully_qualified(folder)` (drive letter + separator, or UNC `\\server\share\…`; reject `\\?\` and `\\.\` device paths). Linux documents with `/tmp/radios` therefore fail at **runtime** on Windows with “Kayıt klasörü bu bilgisayarda geçerli bir tam yol değil: %s” — they stay valid documents.
* Graph validation (`routing_graph.c:97-98`, `desktop_api.py:431-433`) must become **platform-neutral** so a document is valid on both OSes: folder = `""` or POSIX absolute (`/…`) or Windows drive-absolute (`^[A-Za-z]:[\\/]`) or UNC with server+share; reject `\\?\`, `\\.\`, drive-relative `C:x`, relative paths. Implement once in C (`radios_routing_folder_valid()` in routing_graph.c, no GLib platform dependence) and mirror in Python (`desktop_api.py`). `tests/test_routing_graph_schema.py:86` keeps `/tmp/radios`; add Windows cases.
* Prefix rule (`routing_graph.c:99-101`, `desktop_api.py:434-436`): additionally reject `< > : " | ? *` (a `:` would create an NTFS alternate data stream — `kayit:x-…wav` writes into stream `x-…wav` of file `kayit`!). Reserved device names (`CON`, `NUL`, …) are not an issue because `-YYYYMMDD-HHMMSS.wav` is always appended. Add defensive runtime sanitization in `record_path` (replace invalid chars with `_`) for documents written before the rule.
* `g_mkdir_with_parents(folder, 0700)`: mode ignored on Windows. To keep “kullanıcıya özel” semantics call `radios_platform_restrict_to_owner(folder, TRUE, NULL)` only for folders RadioS created (do not rewrite ACLs of operator-chosen existing folders); file-level owner-only ACL is the output area's job (WAV open).
* `path_taken()` compares live paths with `g_strcmp0`; NTFS is case-insensitive → on Windows compare with a case-insensitive ordinal (`CompareStringOrdinal(…, TRUE)` via UTF-16, or `g_utf8_casefold` both sides). Exclusive create (`CREATE_NEW`/`_O_EXCL`) remains the real guard.
* `path[4097]` is fine; Windows MAX_PATH (260) requires the manifest `longPathAware` + OS setting for longer paths (manifest owner). Otherwise fail with a clear message.
* Hazards to mention in UI errors: Defender “Controlled folder access” blocks writes to Documents/Music for unknown apps (ERROR_ACCESS_DENIED); OneDrive-redirected Documents will sync large WAVs.

### 6.6 Streams (RG_STREAM)

* Patchbay logic portable. Cross-area (output.c): libcurl on Windows needs a CA source for `https` Icecast — set `CURLOPT_SSL_OPTIONS |= CURLSSLOPT_NATIVE_CA` (Windows cert store) or ship `curl-ca-bundle.crt`; SHOUTcast `GSocketClient` is Winsock-backed; no SIGPIPE concerns (`CURLOPT_NOSIGNAL` already set).
* `secure_free()` (`patchbay.c:111-112`) uses `memset` before `g_free` — dead-store elimination can remove it at `-O2` on any OS. Use `radios_platform_secure_zero()` (`SecureZeroMemory` / `explicit_bzero`). Same for `routing.c:397` (`start_job_free`); `routing.c:107-113` already uses a volatile loop.

### 6.7 Worker timing (521-532)

`g_cond_wait_until()` on Windows sleeps in ~15.6 ms quanta unless the process raised the timer resolution; the catch-up logic then ticks in bursts. Windows: drive the 10 ms clock with a per-thread high-resolution waitable timer (`CreateWaitableTimerExW(NULL, NULL, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)`, Windows 10 1803+, fall back to a normal waitable timer) via `radios_platform_sleep_until(next)`, keep the existing monotonic catch-up math and check `closing` after each wait (≤10 ms exit latency). Also: MMCSS on the worker (`radios_platform_audio_thread_enter("Pro Audio")`). Process-wide (app startup owner): `timeBeginPeriod(1)` and `SetProcessInformation(ProcessPowerThrottling, {ControlMask = EXECUTION_SPEED | IGNORE_TIMER_RESOLUTION, StateMask = 0})` — Windows 11 otherwise ignores timer resolution requests of minimized/occluded windows and may apply EcoQoS.

---

## 7. Channel input changes (`src/audio/channel_input.c`, header)

* Replace `Capture`/`capture_run()` (191-297) with:
```c
static void capture_frames(const gfloat *stereo, guint frames, gpointer data)  /* host I/O thread */
{ RadioSAudioChannelInput *in = data; if (stereo) deliver(in, stereo, frames); else deliver_silence(in, frames); }
static gboolean capture_run(RadioSAudioChannelInput *input, gchar error[256])
{
    RadioSAudioHost *host = radios_audio_host_acquire(input->server);
    RadioSAudioHostStreamSpec spec = {.device = input->spec.device, .title = "RadioS kanal girişi",
                                      .period_frames = 480, .buffer_frames = 9600 /* 200 ms */};
    RadioSAudioHostStream *s = radios_audio_host_open_capture(host, &spec, capture_frames, input,
                                                              CONNECT_TIMEOUT_US /* 15 s */, error);
    if (!s) { radios_audio_host_release(host); return FALSE; }        /* "Ses kartı girişi açılamadı: …" */
    status_set(input, RADIOS_CHANNEL_INPUT_READY, NULL);
    gchar failure[256] = "";
    while (!closing(input) && !radios_audio_host_stream_failed(s, failure)) g_usleep(50000); /* 50 ms like pa poll */
    radios_audio_host_stream_close(s); radios_audio_host_release(host);
    if (failure[0] && !closing(input)) { g_strlcpy(error, failure, 256); return FALSE; }
    return TRUE;
}
```
* `deliver()` may block (pacing at 100 ms queued) inside the I/O thread; WASAPI then overruns its 200 ms buffer and reports `DATA_DISCONTINUITY` — equivalent to Pulse dropping. `deliver()` returns FALSE when closing; the capture callback must return promptly so `stream_close()` can join (stop event checked between packets).
* Device semantics: `""`/`"@default"` → default capture endpoint (`eConsole`); `"<render id>.monitor"` → loopback (+ keep-alive render, §5.6); capture id → endpoint. Keep `radios_audio_channel_input_spec_validate()` unchanged (text 1..512 bytes) — `tests/test_channel_input.c:79-95` uses `alsa_input.*` strings, still valid text.
* Error strings: keep the existing Turkish messages; map host failures: device invalidated → “Ses kartı girişi kapandı.”, service down → “Ses sunucusuna bağlanılamadı: Windows ses hizmeti çalışmıyor.”, timeout → “Ses kartı girişi zamanında açılmadı.”, privacy → §9.
* Free/teardown contract (250 ms bounded wait, detached teardown) unchanged; ensure the host stream close is reached within 50 ms of `closing`.
* Network path: portable. FFmpeg 9 notes: compile-check `AVFormatContext.max_analyze_duration`, `pkt_timebase`, `swr_alloc_set_opts2`; `udp://@:1234` / RTP listening on all interfaces triggers a Windows Defender Firewall prompt on first use → packaging should add an inbound rule for `radios.exe` (UDP/RTP/SRT listener modes); tests bind loopback only (no prompt).
* Header `audio_channel_input.h:8-19, 38, 53-57, 61-64`: replace “Pulse source” wording; parameter `pulse_server` → `audio_host`.

---

## 8. `routing_graph.c` / `routing_graph.h`

1. Fix truncation in `implicit()` buffers (§1.3) — both OSes.
2. Platform-neutral recorder folder validation (§6.5).
3. Prefix forbidden characters (§6.5).
4. Optional VIRTUAL `application_binary/application_name` (§4.3).
5. Kind title for `RG_VIRTUAL` (`routing_graph.c:7`) — on Windows recommend “Uygulama sesi” (wire name stays `"virtual"`; titles are not persisted because an empty `name` means “use the kind title”). Lead decision; tests use kind names, not titles.
6. `routing_graph.h:18-19` comment “Live Pulse streams …” → “Live audio streams …”.

---

## 9. HRESULT → Turkish message table (host)

| HRESULT | Message |
|---|---|
| `AUDCLNT_E_DEVICE_INVALIDATED` 0x88890004 | “Aygıt bağlantısı koptu: %s” (patchbay housekeeping keeps its own wording) |
| `AUDCLNT_E_SERVICE_NOT_RUNNING` 0x88890010 | “Windows ses hizmeti (Audiosrv) çalışmıyor.” |
| `AUDCLNT_E_DEVICE_IN_USE` 0x8889000A | “Aygıt başka bir uygulama tarafından özel modda kullanılıyor: %s” |
| `AUDCLNT_E_UNSUPPORTED_FORMAT` 0x88890008 | internal fallback (mix format + resample); if still failing “Aygıt 48 kHz biçimini desteklemiyor: %s” |
| `AUDCLNT_E_ENDPOINT_CREATE_FAILED` 0x8889000F | “Aygıt açılamadı: %s” |
| `E_ACCESSDENIED` on capture endpoint | “Windows mikrofon gizlilik ayarı erişimi engelliyor (Ayarlar › Gizlilik ve güvenlik › Mikrofon › Masaüstü uygulamalarının erişimine izin ver).” |
| `E_ACCESSDENIED` on process loopback | “Bu uygulamanın sesi yakalanamıyor (erişim reddedildi).” |
| `HRESULT_FROM_WIN32(ERROR_NOT_FOUND)` 0x80070490 / `E_NOTFOUND` from `GetDevice` | “Aygıt bulunamadı: %s” |
| process-loopback activation `E_NOTIMPL`/`E_INVALIDARG`/`E_NOINTERFACE` | “Bu Windows sürümü uygulama sesi yakalamayı desteklemiyor (Windows 10 sürüm 2004 veya üzeri gerekir).” |
| timeout | “Aygıt açılamadı: %s” (patchbay) / “Ses kartı girişi zamanında açılmadı.” (channel input) |
| other | “Aygıt açılamadı: %s (0x%08lX)” |

---

## 10. UI changes (`routing.c`, `routing_graph_ui.c`, `audio_ui.c`)

* `routing.c:886-896` (VIRTUAL panel) on Windows: info caption “Seçtiğiniz uygulamanın sesi Windows uygulama sesi yakalama ile alınır; uygulama kendi çıkışında da çalmaya devam eder.”; hide/disable `as_microphone` (tooltip §6.3); app combo label `"%s · %s (PID %u)"`; button “Uygulamayı yakala” (id `routing-virtual-move` unchanged — tests use widget names); new button “Yakalamayı bırak” (`routing-virtual-release`); if `!application_capture_supported` disable both with the reason.
* `routing.c:772-782` notices: success “Uygulamanın sesi yakalanıyor; bloğun çıkışını istediğiniz bloğa bağlayın.”; keep error passthrough.
* `routing.c:1166` node_detail VIRTUAL: Windows `"%s · %s"` (bound app name or “Uygulama seçilmedi”).
* `routing.c:1203-1207` virtual_info: Windows shows bound app/binary/PID instead of sink/source names.
* `routing.c:1303-1306` “SES SUNUCUSU BEKLENİYOR” → neutral “SES SİSTEMİ BEKLENİYOR” (or Windows-specific “WINDOWS SES HİZMETİ BEKLENİYOR”).
* `routing.c:1382-1403` `radios_ui_routing_open()` message → “Uygulamayı yakala”.
* `routing.c:653-677` device combos: for missing ids use `radios_audio_host_describe()` friendly name (“Kullanılamıyor · <friendly>”), id stays `missing:<id>`.
* `routing.c:701-715` folder chooser works on Windows (UTF-8 path); optionally `GtkFileChooserNative` for the native dialog.
* `routing_graph_ui.c:31` VIRTUAL caption → “Seçilen uygulamanın sesini yakalar” on Windows.
* `audio_ui.c:1538-1558, 1711` wording (“Ses sunucusu bağlı değil…”) → neutral/Windows wording; capture list ordering unchanged (monitors last).
* `docs/ses-yonlendirme.md` (docs owner): replace `./run-radios.sh --profile routing` by `radios.exe --profile routing`, “C motoru libpulse kullanır” → WASAPI, PipeWire `media.category=Manager` paragraph → Windows process-loopback paragraph, `0600` → owner-only ACL, add the “app also stays audible / no virtual mic” limitations and Windows version requirement.

---

## 11. Internal contracts to preserve (checklist)

1. Public API names/semantics in `audio_patchbay.h`, `audio_channel_input.h`, `routing_graph.h`, `routing_graph_ui.h`, `routing_ui.h`, `audio_ui.h` channel-input functions (only parameter rename + appended struct fields + one new function).
2. Asynchronous `apply()`: virtual blocks follow the graph immediately; `running=TRUE` opens captures/playbacks/taps/lines; `FALSE` closes them and stops recorders/streams; nothing starts on load (`tests/routing_patchbay_ui_test.inc` “saved graph never starts capture”).
3. Intents: `record()`/`stream_start()` for a block the pending apply will create are kept (`patchbay.c:804-855`).
4. Timing constants (§1.1, §1.2) and thresholds; link open timeout 3 s; reconnect 2 s; housekeeping 250 ms; control wake 500 ms; resync 3 s; stream retry 5 s.
5. Snapshot semantics: peak/RMS are “since previous snapshot” and the snapshot **drains** them (`patchbay.c:917-921`) — only `tick()` in routing.c takes the draining snapshot; `radios_ui_routing_output_snapshot()` must keep using the non-draining `stream_metadata_status()`.
6. Recorder: PCM16 48 kHz stereo, `<prefix>-YYYYMMDD-HHMMSS[-N].wav`, exclusive create, never overwrite, split keeps pause state, bytes = data + 44, RIFF limit stops safely (output.c).
7. Device-name contract (§4.2) incl. `.monitor`, `@default`, `@talk`, `radios_virtual_<id>` and the loop rules in C and Python.
8. No runtime state in documents (indices, running/recording, paths, PIDs, start times, session ids); passwords only with `password_saved`.
9. Host audio is never changed: no default-device changes, no endpoint/session volume or mute changes, no per-app routing changes, no ducking (opt-out set), and (Linux) owned virtual devices removed on unref.
10. Channel input: 5 s retry, “Ağ kaynağından veri gelmiyor.” no-data semantics, 250 ms bounded free, never touches deck transport.
11. Widget names used by tests (`routing-*`, `routing-graph-*`, `dj-network-source-*`).

---

## 12. Tests — what they exercise and the Windows plan

### 12.1 Fixture audio host (`src/audio/audio_host_fixture.c`, all OSes)

Selected by `RADIOS_AUDIO_HOST=fixture:<abs path to JSON>` (or spec string). In-process, deterministic, 10 ms virtual clock thread:
```json
{
  "render":  [{"name": "routing_original", "description": "Routing Original", "record": "C:/…/routing_original.f32"},
              {"name": "routing_output"}, {"name": "routing_microphone"}],
  "capture": [{"name": "fixture_mic", "description": "Fixture Mic", "tone": {"hz": 880, "amplitude": 0.5}}],
  "applications": [{"pid": 41001, "start_time": 1, "name": "Routing Fixture Selected", "binary": "fixture-selected.exe",
                    "endpoint": "routing_original", "tone": {"hz": 440, "amplitude": 0.2}},
                   {"pid": 41002, "start_time": 1, "name": "Routing Fixture Unrelated", "binary": "fixture-unrelated.exe",
                    "endpoint": "routing_output", "tone": {"hz": 997, "amplitude": 0.3}}],
  "control": "C:/…/control"
}
```
* Each render endpoint's mix = its apps' tones + everything RadioS plays into it; `<name>.monitor` delivers that mix; process loopback of `pid` delivers that app's tone only (isolation proof); capture endpoints deliver their tone; optional `record` writes the endpoint mix as raw float32 (replaces `parec` capture files `RADIOS_TEST_*_CAPTURE`).
* Control files (polled every 10 ms): `stop-server` → disconnected, all streams fail; `restart-server` → connected, `epoch++`; `exit-<pid>` → app session disappears and the process watch reports exit (fixture also implements the process-watch hooks).
* The platform process-watch for fixture PIDs must not call `OpenProcess` → host-provided watch functions (the patchbay asks the host, not the platform layer, when the host is a fixture).

### 12.2 Per test

| Test (CTest name) | Exercises | Linux-specific bits | Windows plan |
|---|---|---|---|
| `routing_graph` (`tests/test_routing_graph.c`) | Graph rules, loops incl. `radios_virtual_%u.monitor` and `alsa_output.card(.monitor)`, JSON round trip, legacy schema 1, stream password rules | none | Portable as is. Add cases: Windows folder forms (`C:\\Kayıt`, `\\\\nas\\yayin`), rejected `C:kayit`, `\\\\?\\C:\\x`, prefix with `:`/`*`; long device names (>63 bytes) still detect monitor loops (regression for the buffer fix). |
| `routing_graph_schema` (`tests/test_routing_graph_schema.py`) | SDBASE validation of schema 1 graph and schema 2 patchbay, monitor feedback rejection | `"/tmp/radios"` folder | Keep (validator stays platform-neutral). Add Windows absolute folders and forbidden prefix chars; optional VIRTUAL application fields. |
| `desktop_routing` (`tests/test_desktop_routing.py`) | Schema-1 document HTTP/DB acceptance, rejection of ephemeral fields | Pulse-looking strings are plain text | Portable (depends on SDBASE/Windows HTTP fixture of other areas). |
| `patchbay` (`tests/test_patchbay.c`) | `/patchbay/live-mixer-recorder-streams` (offline engine tone → mixer tap → meter → WAV recorders, channel 1→6 line, split, Icecast PUT + SHOUTcast login with local GSocket servers, metadata), `/patchbay/icecast-headers`, `/patchbay/pulse-devices` (private Pulse: `pactl load-module module-null-sink`, `pacat` fixture player, monitor capture → output, virtual sink + `_mic`, app move, cleanup) | `#include <signal.h>` + `kill(player, SIGTERM)` (line 319: **does not compile on MinGW**), `env PULSE_SERVER=… pactl` (243), `"unix:/nonexistent/…"` (149, 214), `RADIOS_TEST_PULSE_SERVER` | First two cases portable (spec `"unix:…"` → none-host on Windows). Guard `pulse_devices` with `#ifndef G_OS_WIN32`; add `/patchbay/fixture-devices` (runs on all OSes when `RADIOS_TEST_AUDIO_FIXTURE` is set by CTest env): fixture JSON written by the test itself, INPUT `routing_original.monitor` → OUTPUT `routing_output`, monitor recorder, VIRTUAL block + `move_application(41001)` → recorder WAV must contain 440 Hz and **no** 997 Hz, `exit-41001` → block error + binding dropped, `stop-server`/`restart-server` → epoch reconnect; WAV analysis as today. Optional `/patchbay/wasapi-process-loopback` gated by `RADIOS_TEST_REAL_WASAPI=1` (§12.3). |
| `channel_input` (`tests/test_channel_input.c`) | spec rules, network PCM + reconnect + bounded free, close while connecting, SRT availability, no-data reporting over RTP/HTTP, capture PCM (needs `RADIOS_TEST_MAIN_SINK`, `RADIOS_TEST_MIC_SOURCE` from `run_audio_ui.py --engine-only`) | Capture case needs owned Pulse sinks | Network cases portable (GSocket loopback, FFmpeg 9; SRT accepted since MSYS2 FFmpeg has libsrt → the `srt` branch runs). Capture case: CTest sets `RADIOS_AUDIO_HOST=fixture:…` and the env vars to fixture names (`…_monitor`, `…_monitor.monitor`); the realtime engine (`radios_audio_engine_new(TRUE, NULL)`) must honour `RADIOS_AUDIO_HOST` (engine area). Timing asserts (free < 500 ms) unaffected. |
| `native_routing` (`test_routing_ui.c` + `run_routing_ui.py`) | Real GTK page with private DB: save/reopen, record/pause/split/stop, viewer denied, close during pending authorization, reconnect rejects stale authorization, SHOUTcast credential edits, metadata settings | Runner: private `pulseaudio` daemon, `pactl`, `pacat`, `os.O_DIRECTORY` + `/proc/<pid>/fd/<fd>/native` socket path (lines 59-60), `DBUS_SESSION_BUS_ADDRESS`, `DISPLAY/WAYLAND_DISPLAY` check (39-41); C: `st_mode & 0777 == 0600` (`routing_patchbay_ui_test.inc:175`) | Runner rewrite: drop Pulse/proc/dbus; write the fixture JSON (same names `routing_original/routing_output/routing_microphone` + apps), set `RADIOS_AUDIO_HOST`; keep DB init via `tools/radios_database.py` (tools area); GUI availability check on Windows = interactive desktop (skip 77 if `GetDesktopWindow`/session 0). C: replace the mode check with `radios_platform_path_is_owner_only(path)` on Windows. Widget-level flow unchanged. |
| `native_routing_graph` (`test_routing_graph_ui.c` + `run_routing_graph_ui.py`) | Same harness with **real pointer input** (XTest) dragging cables, palette, WAV isolation, and “private application routes restored” check via `pactl list sink-inputs` (204-210) | `<gdk/gdkx.h>`, `<X11/extensions/XTest.h>`, `XTestFakeMotionEvent/ButtonEvent`, `XQueryPointer`, `GDK_BACKEND=x11` (CMake 405-412, `PkgConfig::XTST`) | Windows variant: `GDK_IS_WIN32_DISPLAY`, coordinates = GDK logical × `gdk_window_get_scale_factor()` (per-monitor DPI aware), `SetCursorPos` + `SendInput(MOUSEEVENTF_LEFTDOWN/LEFTUP)` (or `MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK` normalized 0..65535), `GetCursorPos` to save/restore, `SetForegroundWindow(gdk_win32_window_get_handle())` and verify `WindowFromPoint` is ours else return 77 (foreground-lock). CMake: link `user32` instead of XTST; no `GDK_BACKEND`. “Routes restored” check becomes “fixture app bindings released and no host routing API was called” (Windows never moves apps). |
| `application_shutdown` (`tests/test_application_shutdown.py`, `tests/application_shutdown_driver.c:55-89`) | App close during routing recording (uses device `routing_microphone.monitor`) | Private Pulse fixture (172-203) | Cross-area (shutdown owner): same fixture JSON approach; device names unchanged. |
| `pulse_hotplug` (`tests/test_pulse_hotplug.c`) | Engine backend list atomicity | Linux only | Engine area; equivalent WASAPI/fixture hot-plug test (atomic publish, coalescing) also covers `audio_host` device lists. |
| `run_audio_routing.py` | **Orphan** (not in CMake; its native executable/`engine-proof.json` belonged to the removed `src/audio/routing.c`). Imported as helper module by `run_routing_ui.py`, `run_routing_graph_ui.py`, `run_routing_host.py`, `test_application_shutdown.py` (`PROJECT`, `terminate`, `tone_amplitudes`, `tone_file`) | `main()` uses `/proc`, Pulse | Move helpers to `tests/audio_test_support.py` (portable: `array`, `wave`, `math`; `terminate()` uses `Popen.terminate()` = `TerminateProcess` on Windows); keep `run_audio_routing.py` as a thin re-export or delete `main()`. |
| `run_routing_host.py` | **Orphan** host Pulse/PipeWire compatibility probe (`/run/user/<uid>/pulse/native`, `pactl load-module`) | Linux only | Do not port; replace by the opt-in real-WASAPI acceptance (§12.3). |
| `test_audio_ui.c` (`native-audio`) | Engine bus routing (MAIN/CUE/MONITOR captures) | parec captures | Engine area; uses fixture `record` files instead of `parec`. |

### 12.3 Opt-in real-WASAPI acceptance (not in default CTest)

* Helper `tests/win/tone_player.c` (WASAPI shared render, given endpoint id or default, sine Hz/amplitude, session display name) built only on Windows.
* Requires `RADIOS_TEST_WASAPI_ENDPOINT` = a render endpoint that is not audible (e.g. a virtual-cable “CABLE Input”), else exit 77 — tests must not play tones on the operator's speakers or change host settings.
* Scenario: two tone players (440 Hz “selected”, 997 Hz “unrelated”) on that endpoint; patchbay VIRTUAL block bound to the selected PID records ≥2 s; independent WAV analysis (`tone_amplitudes`): 440 ≥ 0.07, 997 ≤ 0.005; `<endpoint>.monitor` recorder contains both; kill selected player (`TerminateProcess`) → block error “Uygulama kapandı…”, no re-capture; defaults/volumes unchanged (read `GetDefaultAudioEndpoint` before/after).
* Also verifies the version probe on the CI image and whether session volume affects process loopback (records the finding in `acceptance.json`).

---

## 13. Build (CMake) changes for this area

* `CMakeLists.txt:46` `radios_audio`: on `WIN32` add `src/audio/audio_host_wasapi.c`, always `src/audio/audio_host_fixture.c` (+ `audio_host_pulse.c` on Linux if refactored); link `ole32 uuid mmdevapi avrt winmm version shlwapi advapi32` (advapi32 for `QueryServiceStatusEx`; kernel32 provides `QueryFullProcessImageNameW`/`GetProcessTimes`); do **not** require `libpulse` on `WIN32` (MSYS2 UCRT64 has no `libpulse` installed — verified no `include/pulse`).
* `CMakeLists.txt:244-250` `test_pulse_hotplug`: Linux only.
* `CMakeLists.txt:257-261` `test_patchbay`: add `ENVIRONMENT "RADIOS_TEST_AUDIO_FIXTURE=1"` for the fixture case.
* `CMakeLists.txt:405-412` `test_routing_graph_ui`: `XTST` and `GDK_BACKEND=x11` only on non-WIN32; on WIN32 link `user32`.
* `CMakeLists.txt:420-426` `native_routing`: unchanged command; runner rewritten.
* Compile flags for WASAPI TUs: `-DCOBJMACROS -DWIN32_LEAN_AND_MEAN -D_WIN32_WINNT=0x0A00 -DNTDDI_VERSION=0x0A00000B` (NTDDI only informational; the activation params are declared locally).

---

## 14. Effort and sequencing

1. **Spike (first, 1–2 days):** process-loopback capture of a PID on Windows 10 22H2 and 11 (format, packets during silence, session-volume effect, elevated targets, browsers), endpoint loopback silence behaviour, AUTOCONVERTPCM mono handling.
2. `audio_host.h` + WASAPI enumeration/sessions/streams (shared with engine area) — largest piece.
3. Fixture host (needed by both this area and the engine area's tests).
4. Patchbay refactor (host calls, bindings, VIRTUAL semantics, worker timer) + channel input capture refactor.
5. routing_graph.c + desktop_api.py validator changes (+ tests).
6. UI wording/panels; docs.
7. Test rewrites (C fixture cases, runner rewrites, SendInput graph test, opt-in real WASAPI).

Estimated: large for this area alone (≈1.5–2.5 k lines C, ≈0.8 k lines Python test changes) — very large if the shared WASAPI host is built here rather than in the engine area.

---

## 15. Risks / open questions for the lead

1. Process loopback support on Windows 10 19041–19045 is undocumented-but-working; behaviour differences must be confirmed in the spike. Runtime probe + clear message; no fallback to mixed monitor.
2. Product semantics change for “Sanal ses arayüzü”: no app move (app stays audible), no virtual microphone, no RadioS device visible to other apps. Needs lead/UX sign-off and doc update. A signed virtual audio driver (SysVAD-based) would be the only way to restore parity — out of scope.
3. Apps using exclusive mode / ASIO / protected content cannot be captured.
4. Device id stability (USB port moves, driver reinstall) → saved selections become “Kullanılamıyor”; by contract no auto-substitution.
5. Windows timer resolution, MMCSS and Windows 11 power throttling when RadioS is minimized — glitches unless the process-wide opt-outs are implemented (startup owner).
6. Microphone privacy blocks (E_ACCESSDENIED) and communications ducking (must opt out) — host audio must not change.
7. Test infrastructure: no private audio server on Windows and CI often has no endpoints → fixture host is mandatory; GUI pointer tests need an interactive desktop and correct DPI scaling.
8. Cross-platform documents: validators must accept both path syntaxes; Linux recorder folders are unusable at runtime on Windows (clear error, not silent).
9. COM apartment misuse (any WASAPI call from the GTK STA thread) causes hard-to-debug failures — keep all COM on host/I/O threads.
10. Latent bugs found (fix on both OSes): `routing_graph.c:139-142` 64/80-byte buffers truncate device names in loop detection; `secure_free()` memset may be optimized away; doc `ses-yonlendirme.md` describes removed schema-1 behaviour and `-002.wav` split naming that the current code does not produce.
