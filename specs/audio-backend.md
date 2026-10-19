# Area: audio-backend — Windows (WASAPI) port specification

Source analysed (read-only): `C:\Users\sonatturkan\Desktop\RadioS`
Files read in full: `src/audio/pulse_backend.c` (600 lines), `src/audio/audio_internal.h`, `include/radios/audio.h`,
`src/audio/output_resampler.c`, `include/radios/output_resampler.h`, `include/radios/audio_runner.h`,
`include/radios/audio_output.h`, `tests/test_pulse_hotplug.c`, `tests/test_audio.c`, `tests/test_audio_recovery.c`,
`tests/test_audio_soak.c`, `tests/test_preferences_output.c`, `tests/test_output_resampler.c`,
`tests/run_audio_ui.py`, `tests/run_audio_recovery.py`, `tests/run_audio_soak.py`, `tests/run_preferences_output.py`,
`tests/run_audio_routing.py`, `cmake/Preferences.cmake`; every backend-related region of `src/audio/engine.c`
(lines 1-360, 430-610, 780-1370, 1830-2030, 2100-2200, 2285-2300, 2400-2430) and the device UI consumers in
`src/ui/audio_ui.c` / `src/ui/preferences.c`. Platform facts taken from `specs/PLATFORM_FACTS.md`
(Windows-only product, no Linux code paths; mingw-w64 has mmdeviceapi/audioclient/audiopolicy/avrt; link
`-lole32 -luuid -lavrt -lwinmm -lpropsys -lmmdevapi`; COM-from-C works with `COBJMACROS` + `initguid.h`;
`AvSetMmThreadCharacteristicsW(L"Pro Audio")` succeeds; target machine has 1 render + 1 capture endpoint,
both 48000 Hz / 2 ch / float32 mix format).

---------------------------------------------------------------------------------------------------

## 0. Decisions in one page

1. **Rename the internal interface, keep its behaviour.** `radios_pulse_backend_*` / `RadioSPulseBackend` /
   `RadioSPulseSnapshot` become `radios_audio_backend_*` / `RadioSAudioBackend` / `RadioSAudioBackendSnapshot`
   with *identical* signatures and field semantics (section 3). `engine.c` only changes names (section 5.10).
   `pulse_backend.c` and libpulse are deleted (Windows-only product).
2. **Two layers.** A platform-neutral *backend core* (`src/audio/backend_core.c`) holds everything the Pulse
   file did besides talking to Pulse: bus state machine, ring consumption, span/tag/played_tag bookkeeping,
   metering, refill-after-flush, apply/reconnect/flush decisions, device-list staging + atomic publish,
   250 ms hotplug debounce, 5 s route retry, 1/2/5 s service backoff, snapshot. It talks to an
   **audio provider vtable** (`src/audio/audio_provider.h`) that abstracts "IAudioClient + IAudioRenderClient
   / IAudioCaptureClient + IAudioClock + IMMDeviceEnumerator". Two providers: **WASAPI**
   (`src/audio/wasapi_provider.c`, the product) and **null** (`src/audio/null_provider.c`, deterministic
   virtual devices for tests and for CI machines without sound hardware).
3. **Threads (all owned by the backend, all COM MTA):** 1 manager thread (enumerator, IMMNotificationClient,
   scans, Audiosrv state), 3 bus render threads (MAIN/CUE/MONITOR, event-driven, MMCSS "Pro Audio"),
   1 capture thread (microphone or loopback, 5 ms polling, MMCSS "Pro Audio"). The engine's control worker
   keeps calling the backend exactly as today; **no backend call blocks on a device** except `flush*`
   (bounded 100 ms handshake). Device opening (`Activate`/`Initialize`, which can take seconds for
   Bluetooth) happens on the bus thread, i.e. asynchronously like `pa_stream_connect_*`.
4. **Shared-mode WASAPI, event-driven render**, stream format *always* requested as
   `WAVE_FORMAT_EXTENSIBLE` float32 stereo at the engine's configured output rate (44100/48000) with
   `AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUTOCONVERTPCM | SRC_DEFAULT_QUALITY | NOPERSIST`; Windows converts
   channels and rate to the mix format. RadioS keeps its own 48k→44.1k `RadioSOutputResampler`
   exactly as on Linux, so `snapshot.output_sample_rate` still proves the operator's choice. Fallback
   (driver rejects AUTOCONVERTPCM): open in the mix format and convert in the provider (channel map,
   sample type, and resample with a new any-rate resampler constructor).
5. **Flush = `IAudioClient::Stop` + `Reset`** on the bus thread (drops queued audio, `IAudioClock`
   position returns to 0 = new `time_base`), ring reset by the ring's consumer, refill-before-`Start`
   exactly mirroring the Pulse "refilling" rule (480 frames or 200 ms).
6. **Played position = `IAudioClock::GetPosition`** sampled by the bus thread on every device period
   (never extrapolated with the wall clock — the docs promise "aradaki zaman GUI saatiyle ileri taşınmaz").
7. **Device ids** = exact `IMMDevice::GetId()` endpoint id strings (UTF-8, e.g.
   `{0.0.0.00000000}.{5f2a...}`); loopback ("monitor") inputs = `<render id>.monitor`; `""` = unassigned,
   `"@default"` = `GetDefaultAudioEndpoint(…, eConsole)` resolved **at open time and never followed**
   afterwards (Pulse used `PA_STREAM_DONT_MOVE`; an automatic move of MAIN to a newly plugged USB headset
   would be an on-air incident). Explicit ids never fall back to another device.
8. **`server` argument** becomes a backend locator: `NULL` → env `RADIOS_AUDIO_SERVER` → default `"wasapi"`;
   `"null:"` / `"null:<spec.json>"` → null provider; anything else → backend that reports
   `connected=FALSE` + Turkish error (never crashes).
9. **Deterministic null device** for tests: virtual sinks with a real-time (or manually stepped) clock,
   `<sink>.monitor` loopback sources, optional raw capture files (replacing `parec`), hotplug and
   "server stop/restart" driven by a watched JSON spec file or an in-process C test API.
10. **Turkish operator-facing error strings** (section 5.8), ≤ 200 bytes, truncated only at UTF-8
    boundaries (GTK + `G_DEBUG=fatal-warnings` must never see broken UTF-8).
11. Broadcast safety extras on Windows: opt out of communication ducking
    (`IAudioSessionControl2::SetDuckingPreference(TRUE)`), `NOPERSIST` session volume, one named audio session
    per bus ("RadioS MAIN/CUE/MONITOR"), optional RAW mode (bypass Windows enhancements) with fallback.

---------------------------------------------------------------------------------------------------

## 1. Files in scope and what happens to them

| File | Role today | Windows action |
|---|---|---|
| `src/audio/pulse_backend.c` | libpulse backend (threaded mainloop, 3 playback + 1 record stream, device lists, hotplug, flush, timing) | **Delete.** Logic moves to `backend_core.c` (platform-neutral) + `wasapi_provider.c` + `null_provider.c`. |
| `src/audio/audio_internal.h` | `RadioSAudioRing`, ring read/write prototypes, Pulse backend API + snapshot struct, output internals | Rename types/functions (section 5.10). Keep `RadioSAudioRing` and ring functions byte-for-byte. |
| `include/radios/audio.h` | public engine API; `RadioSAudioPreferences`, `RadioSAudioDevice`, bus snapshot | Doc-only edits: parameter `pulse_server` → `server` (l.115), comment "actual ready Pulse stream" (l.88) → "actual ready device stream". Struct layouts unchanged. |
| `src/audio/engine.c` | engine; only consumer of the backend | Mechanical rename (8 call sites, 2 fields); fix `g_time_zone_new_identifier` at l.1195 (cross-area, see 5.10); optionally MMCSS on render thread. |
| `src/audio/output_resampler.c` / `include/radios/output_resampler.h` | 48k → 44.1k bridge with tag mapping (libswresample) | Unchanged public API/behaviour (tests rely on 96000 being rejected). Add internal `radios_output_resampler_new_any_rate()` for the WASAPI mix-format fallback and the null provider. |
| `include/radios/audio_runner.h` | runner API; uses `played_tag[0]` evidence indirectly | No change. |
| NEW `src/audio/backend.c` | dispatcher: parse `server`, pick provider, build core | new |
| NEW `src/audio/backend_core.c` + `backend_core.h` | contract implementation | new |
| NEW `src/audio/audio_provider.h` | provider vtable | new |
| NEW `src/audio/wasapi_provider.c` + `wasapi_guids.c` | WASAPI/MMDevice from C | new |
| NEW `src/audio/null_provider.c` + `src/audio/audio_null.h` | deterministic virtual devices + test API | new |
| `tests/test_pulse_hotplug.c` | fakes libpulse | **Replace** by `tests/test_audio_backend_hotplug.c` (fake provider, same 3 cases). |
| `tests/test_audio.c` | offline + "pulse" realtime cases | Unchanged C (realtime cases run against the null provider via the runner); see 6.2. |
| `tests/test_audio_recovery.c` + `run_audio_recovery.py` | private Pulse daemon stop/restart, pactl hotplug, parec capture | Rewrite I/O parts to null-provider test API; assertions kept (one string changes). |
| `tests/test_audio_soak.c` + `run_audio_soak.py` | real-time soak against private daemon | null provider realtime clock; `/proc` metrics → Win32. |
| `tests/test_preferences_output.c` + `run_preferences_output.py` | 44.1 kHz real output + independent capture | C unchanged; runner creates a 44100 Hz null sink with capture file. |
| `tests/run_audio_ui.py` | null sinks + parec for ~25 GUI/engine tests | Windows branch builds a null spec instead of pactl/parec. |
| `tests/test_audio_ui.c`, `test_audio_loudness_policy_ui.c` | GUI; read capture files with `open/pread/fstat` | Portability fixes only (6.4). |
| `CMakeLists.txt` l.18, 46-49, 244-256; `cmake/Preferences.cmake` l.4 | libpulse, sources, tests | see 5.1. |

---------------------------------------------------------------------------------------------------

## 2. Current (Linux) architecture

```
 GTK thread ──(engine API, engine->mutex)──► engine state ◄── snapshot copies `engine->physical`
                                                    │
 engine "radios-render" thread (engine.c:954)       │   engine "radios-devices" control thread (engine.c:990)
   render_locked() 480-frame blocks, tags           │     every ≤20 ms or on broadcast:
   writes outputs[bus] rings (only ready buses)     │       backend_snapshot()  -> engine->physical
   reads capture ring 1 frame per output frame      │       configure_output / flush / flush_buses /
                                                    │       apply / refresh / reconnect  (outside engine mutex)
                                                    ▼
 pulse_backend.c  ── pa_threaded_mainloop thread (Pulse "server thread"): write_audio() consumes
                     outputs[i] rings, read_audio() produces capture ring, state/subscribe/list callbacks.
                     All backend state is guarded by the mainloop lock; the control thread takes the
                     same lock for every API call.
```

Ring ownership (SPSC, `engine.c:313-344`): output rings — producer engine render thread (under engine mutex),
consumer backend (Pulse mainloop thread, or the control thread inside `snapshot()` while refilling, always
under the mainloop lock). Capture ring — producer backend, consumer engine render thread.

---------------------------------------------------------------------------------------------------

## 3. The backend contract (MUST be preserved bit-for-bit in behaviour)

### 3.1 Types

`RadioSAudioRing` (`audio_internal.h:7-12`): `capacity` frames; `samples` interleaved stereo float
(`capacity*2`); optional `tags` (one `guint64` per frame, `NULL` for capture/external/tap rings);
`_Atomic guint64 read, write` are *monotonic frame counters* (slot = counter % capacity).
- `radios_audio_ring_write(ring, samples, frames, first_tag)` writes `min(frames, capacity-(write-read))`
  frames; tag of frame i = `first_tag ? first_tag + i : 0`; publishes with release store on `write`.
- `radios_audio_ring_read(ring, samples, frames, &last_tag)` reads `min(frames, write-read)`, returns
  count, `*last_tag` = tag of the last frame read; release store on `read`.
- Engine creates outputs with `OUTPUT_CAPACITY` 8192 frames **with tags**, capture 8192 without tags
  (`engine.c:1115-1123`).
- A "ring reset" is `read := write` and must be done by the consumer (or while the consumer is quiescent).

`RadioSPulseSnapshot` → `RadioSAudioBackendSnapshot` (`audio_internal.h:17-25`), field by field:

| Field | Meaning (exact) |
|---|---|
| `connected` | backend has a live connection to the sound service (Pulse: context READY). |
| `ready[3]` | bus stream exists and is in its running state (Pulse: `PA_STREAM_READY` and not rejected for fallback). Stays TRUE during an in-place flush/refill. |
| `microphone_ready` | capture stream running on the requested source. |
| `played_tag[3]` | tag (engine render frame serial) of the last frame on that bus that the device has actually played; 0 = no evidence. Computed only when `ready`. |
| `consumed_frames[3]` | frames played on that bus since the stream was created **or last flushed**, expressed in 48 kHz engine frames: `min(played, written) * 48000 / stream_rate`. |
| `output_sample_rate[3]`, `output_buffer_frames[3]` | only when ready: actual stream rate; actual accepted target latency in frames at that rate (Pulse `tlength/8`). 0 otherwise. |
| `peak[3][2]`, `rms[3][2]` | meter of the newest *write span* that has been fully played. |
| `microphone_peak[2]` | per-channel abs peak of the latest captured packet (0 when none). |
| `error` | connection-level error text (empty when connected). |
| `bus_error[3]` | per-bus error text; empty while ready; kept while failed. |
| `microphone_error` | capture error text. |
| `output_count`, `input_count`, `outputs[64]`, `inputs[64]` | last **complete** device inventory (never partial). |

`RadioSAudioDevice` (`audio.h:49-53`): `name[513]` = stable device id (the value stored in preferences),
`description[513]` = human label, `channels` = device channel count, `monitor` = TRUE for a loopback source
of an output.

`RadioSAudioPreferences` device fields (`audio.h:33-37`): `main_sink`, `cue_sink`, `monitor_sink`,
`microphone_source`, each ≤512 bytes valid UTF-8 (`engine.c:1179-1185`). Defaults
(`engine.c:345-352`): `main_sink="@default"`, others `""`.

### 3.2 Functions — who calls, from which thread, what happens

All functions accept `backend == NULL` / a backend whose start failed (Pulse: `!loop_started`) and then do
nothing (snapshot returns zeros). `new` never returns NULL (errors are reported through `snapshot.error`).

| Function | Caller thread | Contract |
|---|---|---|
| `new(outputs[3], capture, server)` | engine constructor (GTK/test thread), `engine.c:1132-1137`, realtime only | Store ring pointers, defaults `output_sample_rate=48000`, `output_buffer_frames=2400`; start asynchronous connection; return immediately. Connection to the *same* server is retried later (`reconnect`). |
| `configure_output(rate, frames)` | control worker (`engine.c:1062`) | Only stores the playback format (applied by the next `apply`/`flush`). rate ∈ {44100, 48000}, frames power of two 64..4096 (validated by engine `engine.c:1222`). |
| `apply(prefs)` | control worker (`engine.c:1065-1066`), also from `reconnect` and `flush_buses` | Always store `prefs`. If not connected return. For each bus: keep it iff device name, rate and buffer are unchanged and (device is `""` or the stream is *good* = creating/ready); otherwise close, reset bookkeeping (`ready=FALSE`, `last_tag=written=span_write=0`, playback tags cleared, error cleared), and if device non-empty create a new stream (rate=`output_sample_rate`, float32 stereo, target latency `output_buffer_frames`); `@default` → server default, explicit name → that device only. Microphone: re-create iff source name changed or (non-empty and not good): close, `microphone_ready=FALSE`, **reset capture ring (`read:=write`)**, clear error, open 48 kHz float stereo record stream. Stream creation is asynchronous: readiness and errors arrive later. |
| `refresh()` | control worker (`engine.c:1067`) on `radios_audio_engine_refresh_devices` (UI "Aygıtları yenile"), and internally after hotplug | If connected: cancel any in-flight scan, start a new full scan; published lists untouched until the scan completes. |
| `flush()` = `flush_buses(0b111)` | control worker (`engine.c:1063`) | see below |
| `flush_buses(mask)` | control worker (`engine.c:1064`, preview flushes use CUE/MONITOR bits only) | For each masked bus: if the stream is still creating and has written nothing → kept; if ready → drop the device-side queued audio **in place** (stream stays ready, new time base) → kept; otherwise (or on failure) close it (`ready=FALSE`). In every case: output ring `read:=write`, `last_tag=written=span_write=0`. Then `apply(stored prefs)` (re-creates closed buses). Afterwards `snapshot()` must report `ready=TRUE` for kept buses (engine copies `settled.ready`, `engine.c:1073-1090`). |
| `reconnect()` | control worker, **every turn** (`engine.c:1068`) — "worker-only bounded reconnect to the original server; reopening streams never starts an engine deck or opens the microphone's MAIN gate" | (a) If connected and a hotplug-debounced rescan is due → refresh. (b) If connected and `now ≥ route_retry`: any configured bus/mic whose stream is missing or not good → schedule apply and set `route_retry = now + 5 s`. (c) If connected and the connection just became ready (`reapply`) → apply. (d) If disconnected and `now ≥ next_retry`: close all streams, cancel scans, **clear device lists**, create a new connection to the same server; backoff: first retry 1 s after failure (set by the failure itself), then 2 s, then 5 s (bounded). Apply runs after releasing the lock. |
| `snapshot(out)` | control worker (`engine.c:995`, `1074`); tests also call it from a test thread | memset 0, copy state under the lock. Must never wait for the device/server: hard bound < 20 ms even with 64+64 devices during rescans (`test_pulse_hotplug.c:339,381,400`). Pulse also kicked refill writes and fresh timing queries from here — implementation detail, not contract. |
| `free()` | engine unref after render & control threads joined (`engine.c:1160`) | Stop everything, release all resources. |

### 3.3 Write accounting, tags, played_tag, consumed (port verbatim)

Constants (`pulse_backend.c:7-9,126-127`): `FRAME_BYTES=8`, `BLOCK_FRAMES=4096`, `PLAYBACK_TAG_CAPACITY=65536`,
512 `WriteSpan`s per bus, `REFILL_FRAMES=480`, `REFILL_TIMEOUT=200 ms`.

Per write of `frames` (stream-rate frames) on a bus (`write_audio`, l.128-167):
- 48 kHz stream: `read = ring_read(outputs[i], samples, frames, &last_tag)`; zero-pad `frames-read`.
- other rate: `resampled_audio()` (l.95-125) pulls exactly the 48 k frames it needs
  (`needed = min(ceil((frames-filled)*48000/rate)+32, 4096)`) via `read_tagged()` (copies per-frame tags,
  zero-fills short reads) and converts; `consumed != needed` → error "Playback resampler queue overflow",
  bus not ready, stream disconnected. Output tags: `playback_tags[(written+i) % 65536] = last nonzero tag`.
- Append span `{start=written, count=read, length=frames, first_tag = last_tag ? last_tag-read+1 : 0}`;
  `written += frames`; meter (`meter()` l.72-79: per-channel abs peak and RMS over the block) copied into the span.
- Refill rule after a flush: while `refilling`, write only *fresh* audio:
  `fresh = rate==48000 ? queued : queued>32 ? (queued-32)*rate/48000 : 0`; nothing fresh → write nothing
  (no silence ahead of real audio). `refilling` ends when `written ≥ 480` or 200 ms elapsed.

Snapshot (l.544-563), per ready bus, with `played` = stream frames actually played since creation/flush:
```
consumed = min(played, written)
consumed_frames = consumed * 48000 / rate
if rate != 48000 && consumed && written-consumed < 65536:
    played_tag = playback_tags[(consumed-1) % 65536]
for each retained span (oldest→newest):
    if rate == 48000 && span.first_tag && consumed > span.start:
        played_tag = max(played_tag, span.first_tag + min(consumed-span.start, span.count) - 1)
    if span.start + span.length <= consumed: peak/rms = span.peak/rms
```
Pulse `played` = `(pa_stream_get_time - time_base) * rate / 1e6` where `time_base` is 0 for a new stream
and the post-flush write index converted to time after a flush (`flush_base_done`, l.334-347); while the
post-flush base is unknown nothing counts as played (`time_base_valid=FALSE`).

How the engine uses it: `actual_cursor_locked` (`engine.c:486-517`) maps `played_tag[bus]` through
`RenderHistory` (first_tag, frames, per-deck start/consumed) to the deck cursor; runner receipts/ends use
`played_tag[0]` (`engine.c:2111, 2183, 2410`); DSP meters shown are those of the newest fully played
history block (`engine.c:1318-1324`); after every flush the engine sets `history_write=0` and
`played_tag=0` (`engine.c:1080-1090`). **Tags are consumption evidence, never a wall clock.**

### 3.4 ready / connected / error state rules

- Connection lost (Pulse context FAILED/TERMINATED, l.278-283): `error = reason`, every bus `ready=FALSE`,
  `microphone_ready=FALSE`, `next_retry = now+1 s` if unset. Lists are cleared when the retry runs.
- Connection ready (l.268-277): `error=""`, `retries=0`, `next_retry=0`, `reapply=TRUE`, subscribe, full scan.
- Stream READY: `ready=TRUE`, error cleared — **unless** the server attached it to a different device than the
  explicitly requested one: then `ready=FALSE`, error "Requested sink is unavailable; server fallback was
  rejected", stream disconnected (l.172-176; capture l.185-190). `@default` is exempt.
- Stream FAILED/TERMINATED: keep an already set error, else server error string.
- The **engine** interprets `ready[bus]` FALSE **with** a non-empty `bus_error[bus]` or `error` as
  *loss* (`engine.c:999-1029`): increments `loss_epoch`, freezes receipts, schedules a flush, pauses
  every running runner and deck ("Ses bağlantısı kesildi…"), closes the microphone latch/PTT, drops capture,
  re-applies preferences. Not-ready **without** an error (transient re-creation) is not a loss; decks keep
  PLAYING and rendering simply waits for readiness. Decks whose bus is not ready with an error are paused
  with that error text (`engine.c:1055-1059`). `apply_preferences` treats "configured but not ready with an
  error" as a device change → forces flush + re-apply (`engine.c:1202-1206`).

### 3.5 Device enumeration contract

- Outputs = all sinks, inputs = all sources including monitors (`monitor=TRUE` for "monitor of sink").
  ≤64 each; entries with names >512 bytes are skipped; description falls back to the name; `channels` =
  device channel count.
- Lists are staged while a scan runs and swapped in only when complete; a failed query keeps the previous
  complete list; a new scan cancels and restarts an in-flight one (`refresh_locked`, l.246-254).
- Hotplug: any sink/source add/remove/change event sets `refresh_due = now + 250 ms` (trailing debounce,
  l.258-264); application-stream events are ignored. Tested: visible after ≥200 ms and <2 s, one scan per
  burst of 50 events, no partial list ever published, `snapshot` < 20 ms (`test_pulse_hotplug.c`).
- UI (`audio_ui.c:1538-1558`, `preferences.c:441-465`): combos list `""` "Atanmadı", `"@default"`
  "Sistem varsayılan çıkışı/girişi", then devices by `name` with `description` label; inputs that are
  monitors get the suffix " · Çıkış geri dinlemesi"; a configured name not in the list is shown as
  "Bağlı değil · <name>". `radios_audio_ui_capture_devices` lists non-monitor inputs first
  (`audio_ui.c:590-601`). Lists are change-detected by hashing name+description (`audio_ui.c:1531-1537`) —
  **order and descriptions should be stable** across rescans.

### 3.6 Preferences → devices, missing devices

| Preference value | Meaning |
|---|---|
| `""` | bus/mic unassigned: no stream, `ready=FALSE`, no error (engine routes decks elsewhere). |
| `"@default"` | system default device of that direction, resolved when the stream is opened; not moved later. |
| any other string | exactly that device; if absent/unavailable → `ready=FALSE` + error; **no fallback**. |
| `"<sink>.monitor"` as microphone | loopback of an output (used heavily by tests: `RADIOS_TEST_MIC_SOURCE`, `test_preferences_output.c:68`, `test_audio_recovery.c:63`). |

Missing devices are reported through `bus_error[i]` / `microphone_error` (copied to
`RadioSAudioBusSnapshot.error`, deck `error`, Preferences status "Çıkış aygıtı: %s", UI "Bağlı değil · id").
They are retried every 5 s by `reconnect()` (route retry) and immediately when preferences are re-applied.
`test_audio.c:257-260`: a nonexistent MAIN keeps `buses[0].ready == FALSE` and the mic gate closed;
`test_audio_recovery.c:91-93`: after restarting the server without the sink, MAIN not ready and error text
present.

### 3.7 Timing constants and latency targets (keep)

| Item | Value | Source |
|---|---|---|
| Engine render block | 480 frames (10 ms) | `engine.c:18` |
| Engine keeps ≤ 4 blocks queued per ready bus | renders when every ready bus has < 1920 queued → ring ≤ 2400 (≈50 ms) | `engine.c:961-971` |
| Render worker poll | 3 ms | `engine.c:971` |
| Control worker period | ≤ 20 ms | `engine.c:1093` |
| Flush settle | 8 ms quiet, ≤ 40 ms | `engine.c:24-25` |
| Device target latency | `output_buffer_frames` at stream rate; legacy default 2400 @48 kHz = 50 ms; configurable 64..4096 | `pulse_backend.c:290,399-401`, `audio.h:122-126` |
| Pulse write granularity (minreq) | 480 frames if 2400 else max(64, frames/4) | l.400-401 |
| Capture | 48 kHz float stereo, fragment 480 frames, max 19200 frames | l.396-397 |
| Refill after flush | 480 frames or 200 ms | l.126-127 |
| Hotplug debounce | 250 ms trailing | l.263 |
| Route retry | 5 s | l.472 |
| Connection retry | 1 s, then 2 s, then 5 s | l.282, 489-493 |
| Snapshot bound | < 20 ms | tests |
| Flush gap target | one 10 ms block (strict), < 250 ms (default) | `test_audio_recovery.c:335-339` |
| Mic gate change | no flush, effect next block | `engine.c:1908-1913` |

### 3.8 Output resampler contract (`output_resampler.h`)

Bounded, single-thread, 48 kHz → 44100|48000; one instance = one output generation; input/output ≤4096
frames per call; partial input acceptance reported through `consumed` (must retry the rest); queue never
exceeds 8192 source frames; output tag = tag of the last source frame whose position was reached
(`position = floor((n+1)*48000/rate)` with split multiplication), filter delay never counted as consumed;
NULL input tags = 0; 48 kHz path copies exactly; non-finite input rejected; `flush()` drains and ends the
generation. Errors are already Turkish ("Çıkış örneklemesi: %s", "Örnekleyici …"). No platform code; it
compiles unchanged against FFmpeg 8 (uses `AVChannelLayout`/`swr_alloc_set_opts2` already).

---------------------------------------------------------------------------------------------------

## 4. Linux-specific inventory (every item, with replacement)

| file:line | Construct | Windows replacement |
|---|---|---|
| `pulse_backend.c:3` | `#include <pulse/pulseaudio.h>` | `audio_provider.h` / WASAPI headers in the provider only. |
| `pulse_backend.c:41-68` | `pa_threaded_mainloop`, `pa_context`, `pa_operation` state | core state + `GMutex lock` + `GCond cond`; provider objects (5.4). |
| `pulse_backend.c:128-167` | `write_audio` push callback on mainloop thread | bus render thread event loop (5.5.4) with `GetCurrentPadding`/`GetBuffer`/`ReleaseBuffer`. |
| `pulse_backend.c:168-194` | `stream_state`/`capture_state` + `pa_stream_get_device_name` fallback rejection | bus/capture thread phase publication; explicit ids opened with `IMMDeviceEnumerator::GetDevice` only (no fallback possible); state ≠ `DEVICE_STATE_ACTIVE` → error. |
| `pulse_backend.c:195-211` | `read_audio` `pa_stream_peek/drop` | capture thread `GetNextPacketSize`/`GetBuffer`/`ReleaseBuffer` (5.5.5). |
| `pulse_backend.c:212-254` | sink/source info lists, staged publish, cancel | manager-thread scan via `EnumAudioEndpoints` + `IPropertyStore`; cancel flag between devices; atomic swap. |
| `pulse_backend.c:258-264` | `pa_context_subscribe` sink/source events | `IMMNotificationClient` (C vtable) → atomic `refresh_due` + `SetEvent`. |
| `pulse_backend.c:265-284` | context state (server up/down) | manager thread: COM enumerator + Audiosrv status via SCM `QueryServiceStatus`; `AUDCLNT_E_SERVICE_NOT_RUNNING` from any stream → immediate probe. |
| `pulse_backend.c:285-306` | `pa_context_connect(server, NOAUTOSPAWN)`, `pa_context_get_server` | `server` locator parse (5.2); "original server" = same provider instance. |
| `pulse_backend.c:331-371` | `pa_stream_flush` + timing query for new time base | `IAudioClient_Stop` + `IAudioClient_Reset` (clock → 0) on the bus thread. |
| `pulse_backend.c:372-381, 570-574` | `pa_stream_update_timing_info` | none: `IAudioClock_GetPosition` sampled every period on the bus thread. |
| `pulse_backend.c:396-401` | `pa_sample_spec`, `pa_buffer_attr` (tlength/minreq/prebuf/fragsize) | `WAVEFORMATEXTENSIBLE` float32 stereo, `hnsBufferDuration`, fill target, event period (5.6.5). |
| `pulse_backend.c:402,438` | stream names "RadioS MAIN/CUE/MONITOR", "RadioS microphone" | `IAudioSessionControl_SetDisplayName` per bus session GUID. |
| `pulse_backend.c:426-428,443-445` | `PA_STREAM_ADJUST_LATENCY | AUTO_TIMING_UPDATE | DONT_MOVE` | shared mode, event callback; never use `ActivateAudioInterfaceAsync(DEVINTERFACE_AUDIO_RENDER)` default-stream-switching (that would "move"). |
| `pulse_backend.c:156,178,192,279,301,429,446,497` | `pa_strerror()` English texts | Turkish table (5.8) keyed by HRESULT class. |
| `pulse_backend.c:174,188,292,295,422,447,498,120` | English literal errors | Turkish table (5.8). |
| `pulse_backend.c:529-581` snapshot | `pa_stream_writable_size` + direct `write_audio` from control thread while refilling | not needed (bus thread polls 1 ms while refilling). |
| `pulse_backend.c:582-600` free | mainloop stop/free | quit flag, `SetEvent`, `g_thread_join`, COM release on owning threads, `CoUninitialize`. |
| `audio_internal.h:16-36` | Pulse-named API | renamed neutral API. |
| `audio.h:88,112-115` | "Pulse stream", `pulse_server` | wording/param name. |
| `engine.c:171-172,995,1062-1068,1073-1074,1134,1160` | backend calls/types | rename. |
| `engine.c:1195` | `g_time_zone_new_identifier(prefs->loudness_policy.timezone)` | **Windows GLib rejects IANA ids** (PLATFORM_FACTS): use `radios_time_zone_new_identifier()` from the platform layer, or every station with an IANA loudness timezone fails `apply_preferences` ("Loudness policy time zone is unavailable") and *no device preferences are ever applied*. Same in `src/core/loudness_policy.c:27-34` (also probes a zoneinfo directory path) — core area. |
| `src/ui/preferences.c:330-359` | `hardware_max_rate()` reads `/proc/asound/card*/codec#*` / `stream*` | `radios_audio_hardware_max_rate()` provided by this area (5.6.10). |
| `src/ui/preferences.c:517` | label "PulseAudio / PipeWire" | "Windows ses (WASAPI)" or `radios_audio_engine_backend_label()` (5.11). |
| `src/ui/audio_ui.c:1711` | "Ses sunucusu bağlı değil; ses servisini kontrol edip uygulamayı yeniden aç." | Windows wording: "Windows ses hizmetine bağlı değil; hizmet açılınca kendiliğinden bağlanır." (UI area). |
| `CMakeLists.txt:18,48,246-250` | `pkg_check_modules(PULSE…)`, link, fake-libpulse test | remove; add Win32 libs (5.1). |
| `tools/radios_hardware_check.py` | `pactl` inventory | needs `RadioS.exe --audio-inventory-json` (cross-area, 5.11). |
| `tools/radios_launch.py:36`, `tools/radios_package.py:210`, `tools/radios_sdk.py:26` | libpulse dependency / flatpak socket | remove (launcher/packaging areas). |
| Tests: `pactl`, `parec`, `pacat`, `pulseaudio`, `pipewire*`, `/proc/<pid>/fd/<n>` socket alias, `kill`, `waitpid`, `setsid`, `getpid`, `/proc/self/statm`, `/proc/self/fd`, `open/pread/fstat`, `g_spawn_async_with_fds` | see section 6 | null provider + Win32 equivalents. |

No other POSIX constructs exist in the backend/engine (no signals, files, sockets, `/proc` or path
separators); `engine.c` uses only GLib, C11 atomics, `<math.h>`.

---------------------------------------------------------------------------------------------------

## 5. Windows design

### 5.1 Module layout and build

```
src/audio/audio_internal.h   (renamed API, unchanged ring)
src/audio/backend.c          radios_audio_backend_new(): parse server, create provider, core
src/audio/backend_core.h/.c  contract implementation (threads, bookkeeping) — no Win32 audio APIs,
                             uses HANDLE events + GLib threads/mutex/cond
src/audio/audio_provider.h   provider vtable (5.4)
src/audio/wasapi_provider.c  WASAPI + MMDevice via COBJMACROS
src/audio/wasapi_guids.c     the only TU that does #include <initguid.h> before the WASAPI headers
src/audio/null_provider.c    deterministic virtual devices
src/audio/audio_null.h       test-only C API of the null provider (exported from radios_audio)
```
CMake (`CMakeLists.txt:46-49`): replace `src/audio/pulse_backend.c` by the five new `.c` files; drop
`PkgConfig::PULSE`; `target_link_libraries(radios_audio PRIVATE ole32 uuid avrt winmm propsys mmdevapi advapi32)`;
`target_compile_definitions(radios_audio PRIVATE COBJMACROS WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)`.
Remove the `test_pulse_hotplug` target; add `test_audio_backend_hotplug` (links `radios_audio` objects plus
the fake provider), `test_null_provider`, and the opt-in `test_wasapi_smoke` (section 6.3).

### 5.2 `server` argument (Pulse meaning → Windows meaning)

Linux: `server` was a PulseAudio server address (`unix:/path/native`, `tcp:host`, `NULL` = libpulse default
from `$PULSE_SERVER`/client.conf). The backend remembered the server actually reached and reconnected only
to it. Production code passes `NULL` (`audio_ui.c:865`); tests pass a private daemon
(`RADIOS_TEST_PULSE_SERVER`) or rely on `$PULSE_SERVER`.

Windows has exactly one audio service per logon session (Audiosrv + AudioEndpointBuilder), so the string
becomes a *backend locator* with the same "NULL = environment, else explicit" behaviour:

```
resolve(server):
  s = server && *server ? server : getenv("RADIOS_AUDIO_SERVER")        // analog of PULSE_SERVER
  if !s || !*s || g_str_equal(s, "wasapi") || g_str_equal(s, "wasapi:")  -> WASAPI provider
  if g_str_has_prefix(s, "null:")                                         -> null provider, spec = s+5
                                                                            ("" = empty in-memory server driven by audio_null.h,
                                                                             otherwise absolute path of a UTF-8 JSON spec)
  else                                                                    -> "unsupported" provider: connected=FALSE,
                                                                             error "Bu ses sunucusu adresi Windows'ta desteklenmiyor: <s>"
```
Read the environment with `g_getenv` (UTF-8 on Windows). Public doc in `audio.h:112-115` changes to
"server: NULL uses RADIOS_AUDIO_SERVER, else the Windows audio service ('wasapi'); 'null:…' selects the
deterministic virtual devices used by tests." The same resolver function
(`radios_audio_server_resolve()`, exported) must be used by `channel_input.c` and `patchbay.c`
(cross-area) so one environment variable redirects every audio component of a test process.
Test variable `RADIOS_TEST_PULSE_SERVER` → `RADIOS_TEST_AUDIO_SERVER` (tests should accept both).

### 5.3 Core objects, threads, locks

```c
typedef enum { BUS_IDLE, BUS_OPENING, BUS_READY, BUS_FAILED } BusPhase;   /* GOOD = OPENING|READY */

typedef struct {                       /* one per MAIN/CUE/MONITOR */
    struct RadioSAudioBackend *core; guint index;
    GThread *thread; HANDLE wake;      /* auto-reset event: commands */
    /* desired configuration — written by apply()/flush under core->lock */
    guint64 generation;                /* ++ on every re-creation / forced close */
    gchar device[RADIOS_AUDIO_DEVICE_NAME_SIZE]; guint rate, buffer_frames;
    guint64 flush_request, flush_ack; gboolean flush_kept;
    /* published state — under core->lock */
    BusPhase phase; gboolean ready; gchar error[256];
    guint actual_rate, fill_target;    /* snapshot output_sample_rate / output_buffer_frames */
    guint64 position;                  /* stream frames played since start/Reset (IAudioClock) */
    gboolean started;                  /* IAudioClient started since open/Reset */
    gboolean refilling; gint64 refill_since;
    guint64 written, last_tag, span_write;
    WriteSpan spans[512]; guint64 playback_tags[65536];
    gfloat peak[2], rms[2];
    gint reopen_attempts; gint64 reopen_window_start;
    /* bus-thread private */
    RadioSOutputResampler *resampler;
    gfloat samples[4096*2], source_samples[4096*2];
    guint64 source_tags[4096], output_tags[4096];
} Bus;

struct RadioSAudioBackend {
    const RadioSAudioProviderOps *ops; RadioSAudioProvider *provider;
    GMutex lock; GCond cond;           /* lock = Pulse mainloop lock analog; never held across device calls */
    gboolean quit, connected, reapply, service_suspect;
    gchar error[256];
    RadioSAudioPreferences preferences;
    guint output_sample_rate, output_buffer_frames;          /* 48000 / 2400 at start */
    gint64 next_retry, route_retry; guint retries;
    Bus buses[3]; RadioSAudioRing *outputs[3], *capture;
    /* microphone (same pattern as Bus, fields: generation, source, phase, ready, error, peak, last_packet_us) */
    Mic mic;
    /* devices */
    guint output_count, input_count;
    RadioSAudioDevice outputs_info[64], inputs_info[64];     /* published */
    gint64 refresh_due;                /* written by hotplug callbacks with g_atomic_*; 0 = none */
    guint scan_generation; gboolean scan_requested; guint64 device_scans;
    GThread *manager; HANDLE manager_wake;
};
```

Thread roles (all created by `radios_audio_backend_new`, named via `g_thread_new`, each calls
`CoInitializeEx(NULL, COINIT_MULTITHREADED)` first and `CoUninitialize()` last; every COM object is
created, used and released on the thread that owns it):

| Thread | Owns | Wakes on |
|---|---|---|
| `radios-audio-mgr` | provider connection (enumerator, notification client, Audiosrv handle), scans | `manager_wake`, timeout = min(1 s service poll, `refresh_due`, `next_retry`) |
| `radios-audio-main/cue/mon` | that bus's provider stream | `bus->wake`, stream event, 1 ms while refilling, 2000 ms stall guard |
| `radios-audio-mic` | capture stream | `mic.wake`, 5 ms poll |

Lock rules: `core->lock` protects every published field; device calls are made **without** the lock;
results are published with a generation check ("if `bus->generation` changed meanwhile, discard and close").
The engine control thread is the only external caller (tests may call from their thread); all API functions
take the lock briefly. Ring consumers: bus threads (outputs); producer of the capture ring: mic thread.

### 5.4 Provider interface (`src/audio/audio_provider.h`)

```c
typedef struct RadioSAudioProvider RadioSAudioProvider;
typedef struct RadioSAudioStream   RadioSAudioStream;

typedef enum {
    RADIOS_AUDIO_IO_OK = 0,
    RADIOS_AUDIO_IO_LOST = -1,         /* device removed/disabled/unplugged -> error, route retry */
    RADIOS_AUDIO_IO_REOPEN = -2,       /* invalidated but device still ACTIVE (format change, resources
                                          invalidated, stall, resume from sleep) -> silent reopen */
    RADIOS_AUDIO_IO_SERVICE_DOWN = -3, /* Audiosrv not running / null server stopped */
    RADIOS_AUDIO_IO_FAILED = -4        /* anything else; provider fills a Turkish message */
} RadioSAudioIoStatus;

typedef enum { RADIOS_AUDIO_HOTPLUG_DEVICES, RADIOS_AUDIO_HOTPLUG_DEFAULT,
               RADIOS_AUDIO_HOTPLUG_IGNORED } RadioSAudioHotplugKind;
typedef struct {
    void (*notify)(gpointer core, RadioSAudioHotplugKind kind);  /* any thread; must not block */
    gpointer core;
} RadioSAudioHotplugSink;

typedef struct {
    guint rate;            /* stream rate as opened (snapshot output_sample_rate) */
    guint buffer_frames;   /* device buffer capacity, frames at rate */
    guint period_frames;   /* device period (wake granularity), frames at rate */
} RadioSAudioStreamInfo;

typedef struct {
    const gchar *label;    /* "Windows ses (WASAPI)", "Sanal test aygıtları" */
    /* manager thread */
    RadioSAudioIoStatus (*connect)(RadioSAudioProvider *, gchar error[256]);
    void (*disconnect)(RadioSAudioProvider *);
    gboolean (*service_running)(RadioSAudioProvider *);
    RadioSAudioIoStatus (*scan)(RadioSAudioProvider *, RadioSAudioDevice outputs[64], guint *outputs_count,
                                RadioSAudioDevice inputs[64], guint *inputs_count,
                                gboolean (*cancelled)(gpointer), gpointer cancel_data);
    /* bus / mic threads (caller has COM MTA) */
    RadioSAudioIoStatus (*open)(RadioSAudioProvider *, gboolean capture, const gchar *device_id,
                                guint rate, guint buffer_frames, guint bus_index /* 0..2, 3 = mic */,
                                RadioSAudioStream **out, RadioSAudioStreamInfo *info, gchar error[256]);
    HANDLE (*event)(RadioSAudioStream *);   /* auto-reset; signalled each device period (render) */
    RadioSAudioIoStatus (*padding)(RadioSAudioStream *, guint *queued_frames);
    RadioSAudioIoStatus (*write)(RadioSAudioStream *, const gfloat *stereo, guint frames);  /* frames <= buffer-padding */
    RadioSAudioIoStatus (*read)(RadioSAudioStream *, gfloat *stereo48k, guint capacity,
                                guint *frames, gboolean *silent);   /* next packet, already 48 kHz stereo float */
    RadioSAudioIoStatus (*position)(RadioSAudioStream *, guint64 *frames_played);
    RadioSAudioIoStatus (*start)(RadioSAudioStream *);
    RadioSAudioIoStatus (*stop)(RadioSAudioStream *);
    RadioSAudioIoStatus (*reset)(RadioSAudioStream *);   /* stopped stream only: drop queue, position := 0 */
    const gchar *(*last_error)(RadioSAudioStream *);    /* Turkish text for the last non-OK status */
    void (*close)(RadioSAudioStream *);
    void (*free)(RadioSAudioProvider *);
} RadioSAudioProviderOps;

RadioSAudioProvider *radios_audio_wasapi_provider_new(const RadioSAudioHotplugSink *sink);
RadioSAudioProvider *radios_audio_null_provider_new(const gchar *spec, const RadioSAudioHotplugSink *sink);
extern const RadioSAudioProviderOps radios_audio_wasapi_ops, radios_audio_null_ops;
```

This vtable is also the portability layer the channel-input and patchbay areas should use for their own
capture/playback streams (same device ids, same error texts).

### 5.5 Core algorithms (port of pulse_backend.c)

#### 5.5.1 `radios_audio_backend_new(outputs, capture, server)`
Allocate core; `output_sample_rate=48000`, `output_buffer_frames=2400`; per bus `wake=CreateEventW(NULL,FALSE,FALSE,NULL)`;
resolve provider (5.2); start manager, 3 bus threads, mic thread. Return immediately. On any allocation
failure set `error` (5.8 row "start") and return the backend (all later calls are no-ops, snapshot shows
the error) — never NULL, like Pulse l.292-296.

#### 5.5.2 Manager thread
```
connect_attempt():
    st = ops->connect(p, err)
    if st == OK && ops->service_running(p): lock; connected=TRUE; error=""; retries=0; next_retry=0;
                                            reapply=TRUE; scan_requested=TRUE; unlock
    else: lock; error=err (5.8); if !next_retry: next_retry = now + 1 s; unlock
loop until quit:
    if !connected && now >= next_retry:              /* Pulse reconnect() l.474-499 */
        ops->disconnect(p)                            /* drop enumerator + notification registration */
        delay = retries == 0 ? 2 s : 5 s; if retries < 3: ++retries; next_retry = now + delay
        connect_attempt()
    /* service_suspect (set by a stream that saw AUDCLNT_E_SERVICE_NOT_RUNNING) only makes this check run
       immediately instead of at the next 1 s poll */
    if connected && !ops->service_running(p):
        lock; connected=FALSE; error = (5.8 "service down"); for buses: ready=FALSE, phase=FAILED,
        ++generation (threads close their streams and stay closed while !connected); same for mic;
        output_count=input_count=0; refresh_due=0; if !next_retry: next_retry = now + 1 s; unlock
        SetEvent(all bus/mic wakes)
    service_suspect = FALSE
    due = g_atomic_int64_get(&refresh_due)
    if connected && (scan_requested || (due && now >= due)): do_scan()
    WaitForSingleObject(manager_wake, min(1000 ms, due-now, next_retry-now))
do_scan():                                            /* refresh_locked + sink_info/source_info */
    lock; scan_requested=FALSE; refresh_due=0; gen=++scan_generation; ++device_scans; unlock
    st = ops->scan(p, staged_out, &n_out, staged_in, &n_in, cancelled /* gen != scan_generation */)
    if st == OK and not cancelled: lock; memcpy published; counts; unlock
                                   route_retry_if_configured_device_now_present()   /* sets route_retry = 0 */
    if cancelled: (a newer refresh bumped scan_generation) loop immediately -> rescan
    if st != OK: keep previous lists (Pulse: end < 0 keeps the last list)
```
Hotplug sink `notify(core, kind)`: `IGNORED` → nothing; otherwise
`g_atomic_int64_set(&refresh_due, g_get_monotonic_time() + 250 ms)` (trailing debounce, every event
pushes it) and `SetEvent(manager_wake)`. Must not take `core->lock` (IMMNotificationClient rule: never block
in a callback) — `refresh_due` is an atomic 64-bit value.

#### 5.5.3 `apply(prefs)` (control thread; Pulse l.389-451)
```
lock; preferences = *prefs
if !connected: unlock; return
for i in 0..2:
    want = {main,cue,monitor}_sink[i]; good = bus.phase in {OPENING, READY}
    if strcmp(bus.device, want)==0 && bus.rate==output_sample_rate && bus.buffer_frames==output_buffer_frames
       && (!*want || good): continue
    ++bus.generation; bus.ready=FALSE; bus.phase = *want ? OPENING : IDLE; bus.error[0]=0
    reset_accounting(bus)   /* written=last_tag=span_write=0, playback_tags={0}, position=0, peaks 0 */
    bus.rate=output_sample_rate; bus.buffer_frames=output_buffer_frames; strlcpy(bus.device, want)
    SetEvent(bus.wake)
mic: if strcmp(mic.source, prefs->microphone_source) || (*source && !mic_good):
    ++mic.generation; mic.ready=FALSE; mic.phase = *source ? OPENING : IDLE; mic.error[0]=0
    atomic_store(&capture->read, atomic_load(&capture->write))      /* as Pulse l.434 */
    strlcpy(mic.source, …); SetEvent(mic.wake)
unlock
```

#### 5.5.4 Bus render thread
```
COM MTA; mm = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task) (fallback SetThreadPriority HIGHEST)
active_gen = 0; stream = NULL
loop:
    handles = {bus.wake, stream ? ops->event(stream) : NULL}
    timeout = !stream ? 1000 : (bus.refilling || !bus.started) ? 1 : 2000
    r = WaitForMultipleObjects(stream ? 2 : 1, handles, FALSE, timeout)
    lock; if quit: unlock; break; want = desired config copy; flush_req = bus.flush_request; conn = connected; unlock
    if want.generation != active_gen:                         /* re-creation or forced close */
        close(stream); free resampler; active_gen = want.generation
        ring_reset_as_consumer(outputs[i])                     /* stale PCM of an old generation */
        if !*want.device || !conn: publish(phase = *device ? FAILED : IDLE keep error); continue
        open_stream(want)                                      /* below; publishes READY or FAILED */
        continue
    if flush_req != bus.flush_ack: handle_flush(flush_req); continue
    if !stream: continue
    if r == WAIT_TIMEOUT && timeout == 2000: status = REOPEN (stall) else status = render_step()
    if status != OK: handle_failure(status)

open_stream(want):
    st = ops->open(p, FALSE, want.device, want.rate, want.buffer_frames, i, &s, &info, err)
    lock; superseded = bus.generation != active_gen; unlock
    if superseded: close(s); return
    if st != OK: lock; phase=FAILED; ready=FALSE; error=err; if st==SERVICE_DOWN: service_suspect=TRUE,
                  SetEvent(manager_wake); unlock; return
    if info.rate != 48000: resampler = info.rate in {44100,48000} ? radios_output_resampler_new(info.rate)
                                                                 : radios_output_resampler_new_any_rate(info.rate)
         (failure -> FAILED with the resampler's Turkish message, as Pulse l.414-419)
    lock; reset_accounting(bus) (keeps generation); actual_rate=info.rate;
          fill_target = CLAMP(want.buffer_frames, 2*info.period_frames, info.buffer_frames);
          refilling=FALSE; started=FALSE; position=0; unlock
    st = render_step()       /* initial fill: whatever the ring has, silence-padded (Pulse new stream);
                                refilling==FALSE so render_step() also calls ops->start() exactly once */
    if st != OK: handle_failure(st); return
    lock; phase=READY; ready=TRUE; error[0]=0; unlock

render_step():                                                /* write_audio() equivalent */
    st = ops->padding(s, &queued); if st: return st
    if refilling && (written >= 480 || now - refill_since > 200 ms): refilling = FALSE
    want = fill_target > queued ? fill_target - queued : 0
    while want:
        frames = MIN(want, 4096)
        if refilling:
            q = ring.write - ring.read
            fresh = rate==48000 ? q : q > 32 ? (q-32)*rate/48000 : 0
            if !fresh: break
            frames = MIN(frames, fresh)
        produce(frames)          /* identical to write_audio l.142-153: ring_read or resampled_audio,
                                    zero pad, playback_tags; resampler failure -> FAILED, error text */
        st = ops->write(s, samples, frames); if st: return st
        lock; append span {written, read, last_tag-read+1, frames, meter}; written += frames;
              if last_tag: bus.last_tag = last_tag; unlock
        want -= frames
        if refilling && written >= 480: refilling = FALSE
    if !started && (written > 0 || !refilling): st = ops->start(s); started = TRUE   /* prebuf=0 analog */
    if started && ops->position(s, &pos) == OK: lock; position = pos; unlock
    return OK

handle_flush(req):                                            /* stream_flush_locked + flush_base_done */
    kept = FALSE
    if stream && phase == READY:
        if ops->stop(s)==OK && ops->reset(s)==OK:            /* Reset: queue dropped, clock -> 0 */
            if rate != 48000: replace resampler with a fresh instance (failure -> kept=FALSE)
            ring_reset_as_consumer(outputs[i])
            lock; reset_accounting(bus); position=0; started=FALSE; refilling=TRUE; refill_since=now; unlock
            kept = TRUE
    else if phase == OPENING && written == 0: ring_reset_as_consumer(outputs[i]); kept = TRUE
    lock; bus.flush_kept = kept; bus.flush_ack = req; g_cond_broadcast(&cond); unlock

handle_failure(status):
    REOPEN  -> if reopen_attempts < 3 within 2 s: lock; ready=FALSE (NO error: not an engine "loss");
               reset_accounting; unlock; close; open_stream(same generation, ring NOT reset, ++attempts)
               else treat as LOST with "Ses aygıtı yanıt vermiyor…"
    LOST/FAILED -> lock; ready=FALSE; phase=FAILED; error = ops->last_error(s) or table; unlock; close
               (route retry in reconnect() re-creates it within 5 s, or immediately after a hotplug scan)
    SERVICE_DOWN -> as LOST with the service text, plus service_suspect=TRUE; SetEvent(manager_wake)
```
Semantics preserved: `played` = `IAudioClock` position since open/Reset ⇒ the Pulse `time_base`
is always 0 here; while refilling the stream is stopped so position stays 0 ("until the post-flush base
is known nothing new counts as played"). `consumed = min(played, written)` absorbs any position the audio
engine advances during starvation.

#### 5.5.5 Microphone thread
```
COM MTA + MMCSS "Pro Audio"
loop: WaitForSingleObject(mic.wake, 5 ms)
   generation change -> close; if *source && connected: ops->open(p, TRUE, source, 48000, 9600 /*200 ms*/, 3, …)
                        -> publish microphone_ready / microphone_error exactly like capture_state
   while ops->read(s, buf, 4096, &frames, &silent) == OK && frames:
        if silent: memset 0                                  /* AUDCLNT_BUFFERFLAGS_SILENT */
        finite = all samples finite; peak per channel over the packet
        if finite: radios_audio_ring_write(capture, buf, frames, 0)   /* drop non-finite packet (l.200-209) */
        lock; microphone_peak = peak; last_packet = now; unlock
   if now - last_packet > 100 ms: microphone_peak = {0,0}    /* WASAPI loopback delivers NO packets on an
                                                                idle output; Pulse monitors delivered silence */
   failure -> microphone_ready=FALSE, microphone_error (5.8), route retry
```

#### 5.5.6 `flush_buses(mask)` (control thread)
```
lock
for i in mask: if buses[i].phase==READY && ready: ++flush_request; SetEvent(wake); waiting |= 1<<i
deadline = now + 100 ms
while waiting bits not all acked and g_cond_wait_until(&cond, &lock, deadline)   /* releases lock */
for i in mask:
    kept = (i in waiting && acked && flush_kept) || (phase==OPENING && written==0)
    if !kept: ready=FALSE; phase=BUS_IDLE (not GOOD, no error); ++generation   /* thread closes, resets ring;
                                                                 apply() below sees !GOOD and re-creates */
    reset_accounting(bus)                                               /* last_tag=written=span_write=0 */
prefs = preferences; unlock
apply(&prefs)                                     /* re-creates the closed ones (new generation) */
```
`flush()` = `flush_buses(0x7)`. Waiting for all masked buses in parallel keeps the worst case ≤ 100 ms.

#### 5.5.7 `reconnect()` (control thread, every turn)
```
lock; apply = connected && reapply; reapply = FALSE; now = mono
if connected && now >= route_retry:
    for each bus: if *device && phase not in {OPENING, READY}: apply = TRUE
    if *mic.source && mic.phase not in {OPENING, READY}: apply = TRUE
    if apply: route_retry = now + 5 s
prefs = preferences; unlock
if apply: apply(&prefs)
```
(Service reconnection and debounced rescans run on the manager thread; calling `reconnect` is still
required every turn because the route retry and post-connect re-apply happen here, as in Pulse.)

#### 5.5.8 `refresh()`, `configure_output()`, `snapshot()`, `free()`
- `refresh`: `if !connected return; lock; ++scan_generation; scan_requested=TRUE; refresh_due=0; unlock; SetEvent(manager_wake)`.
- `configure_output(rate, frames)`: lock; store; unlock (no device action, like Pulse l.382-388).
- `snapshot`: lock; copy `connected`, `error`; per bus `ready`, `bus_error`, and if ready compute
  consumed/played_tag/peak/rms from `position`/`written`/spans (3.3), `output_sample_rate=actual_rate`,
  `output_buffer_frames=fill_target`; mic ready/peak/error; device lists; unlock. No provider call. O(µs).
- `free`: lock; quit=TRUE; unlock; `SetEvent` every wake; join mic, bus, manager threads (each closes its own
  stream / provider objects); `ops->free`; `CloseHandle` events; free.

### 5.6 WASAPI provider (C, COM via lpVtbl / COBJMACROS)

#### 5.6.1 Headers, GUIDs, libraries
```c
#define COBJMACROS
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <propvarutil.h>   /* optional; PropVariantClear is in ole32 */
```
`wasapi_guids.c` is the only file with `#include <initguid.h>` before these headers (PLATFORM_FACTS says
this links). If a symbol is still unresolved define local copies:
`CLSID_MMDeviceEnumerator {BCDE0395-E52F-467C-8E3D-C4579291692E}`,
`IID_IMMDeviceEnumerator {A95664D2-9614-4F35-A746-DE8DB63617E6}`,
`IID_IAudioClient {1CB9AD4C-DBFA-4C32-B178-C2F568A703B2}`, `IID_IAudioClient2 {726778CD-F60A-4EDA-82DE-E47610CD78AA}`,
`IID_IAudioClient3 {7ED4EE07-8E67-4CD4-8C1A-2B7A5987AD42}`, `IID_IAudioRenderClient {F294ACFC-3146-4483-A7BF-ADDCA7C260E2}`,
`IID_IAudioCaptureClient {C8ADBD64-E71E-48A0-A4DE-185C395CD317}`, `IID_IAudioClock {CD63314F-3FBA-4A1B-812C-EF96358728E7}`,
`IID_IMMNotificationClient {7991EEC9-7E89-4D85-8390-6C703CEC60C0}`, `IID_IMMEndpoint {1BE09788-6894-4089-8586-9A2A6C265AC5}`,
`IID_IAudioSessionControl {F4B1A599-7266-4319-A8CA-E70ACB11E8CD}`, `IID_IAudioSessionControl2 {BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D}`,
`KSDATAFORMAT_SUBTYPE_IEEE_FLOAT {00000003-0000-0010-8000-00AA00389B71}`, `KSDATAFORMAT_SUBTYPE_PCM {00000001-0000-0010-8000-00AA00389B71}`,
`PKEY_Device_FriendlyName {A45C254E-DF1C-4EFD-8020-67D146A850E0},14`, `PKEY_Device_DeviceDesc …,2`,
`PKEY_AudioEngine_DeviceFormat {F19F064D-082C-4E27-BC73-6882A1BB8E4C},0` (verify against the headers).
Call style: `IMMDeviceEnumerator_GetDevice(e, wid, &dev)` ≡ `e->lpVtbl->GetDevice(e, wid, &dev)`;
`IsEqualIID(&a, &b)` with pointers in C; `REFIID` arguments take `&IID_x`.

#### 5.6.2 connect / disconnect / service_running (manager thread)
- `connect`: `CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL, &IID_IMMDeviceEnumerator, (void**)&e)`;
  `IMMDeviceEnumerator_RegisterEndpointNotificationCallback(e, &notify->iface)`;
  `OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT)` + `OpenServiceW(scm, L"Audiosrv", SERVICE_QUERY_STATUS)`
  (kept open). Failure → `FAILED` with text 5.8 "connect".
- `service_running`: `QueryServiceStatus(svc, &st) && st.dwCurrentState == SERVICE_RUNNING`; if the SCM
  handles could not be opened, return TRUE and rely on `AUDCLNT_E_SERVICE_NOT_RUNNING` from streams.
- `disconnect`: `UnregisterEndpointNotificationCallback`, release enumerator (a fresh enumerator is created
  after an Audiosrv restart — the analog of the new `pa_context`).

#### 5.6.3 IMMNotificationClient in C
```c
typedef struct { IMMNotificationClient iface; volatile LONG refs; HotplugRelay *relay; } RadiosNotify;
/* HotplugRelay: g_atomic_rc_box holding {RadioSAudioHotplugSink sink; gint alive;} so a late callback
   after Unregister never touches a freed core: provider sets alive=0 before unregistering. */
static HRESULT STDMETHODCALLTYPE n_qi(IMMNotificationClient *s, REFIID r, void **o)
{ if (IsEqualIID(r, &IID_IUnknown) || IsEqualIID(r, &IID_IMMNotificationClient)) { *o = s; IMMNotificationClient_AddRef(s); return S_OK; }
  *o = NULL; return E_NOINTERFACE; }
static ULONG STDMETHODCALLTYPE n_addref(IMMNotificationClient *s) { return (ULONG)InterlockedIncrement(&((RadiosNotify *)s)->refs); }
static ULONG STDMETHODCALLTYPE n_release(IMMNotificationClient *s)
{ LONG n = InterlockedDecrement(&((RadiosNotify *)s)->refs); if (!n) { relay_unref(((RadiosNotify *)s)->relay); g_free(s); } return (ULONG)n; }
static HRESULT STDMETHODCALLTYPE n_state(IMMNotificationClient *s, LPCWSTR id, DWORD st) { relay(s, RADIOS_AUDIO_HOTPLUG_DEVICES); return S_OK; }
static HRESULT STDMETHODCALLTYPE n_added(IMMNotificationClient *s, LPCWSTR id)          { relay(s, RADIOS_AUDIO_HOTPLUG_DEVICES); return S_OK; }
static HRESULT STDMETHODCALLTYPE n_removed(IMMNotificationClient *s, LPCWSTR id)        { relay(s, RADIOS_AUDIO_HOTPLUG_DEVICES); return S_OK; }
static HRESULT STDMETHODCALLTYPE n_default(IMMNotificationClient *s, EDataFlow f, ERole r, LPCWSTR id)
{ relay(s, r == eConsole ? RADIOS_AUDIO_HOTPLUG_DEFAULT : RADIOS_AUDIO_HOTPLUG_IGNORED); return S_OK; }
static HRESULT STDMETHODCALLTYPE n_property(IMMNotificationClient *s, LPCWSTR id, const PROPERTYKEY key)
{ gboolean relevant = IsEqualPropertyKey(key, PKEY_Device_FriendlyName) || IsEqualPropertyKey(key, PKEY_Device_DeviceDesc)
                   || IsEqualPropertyKey(key, PKEY_AudioEngine_DeviceFormat);
  relay(s, relevant ? RADIOS_AUDIO_HOTPLUG_DEVICES : RADIOS_AUDIO_HOTPLUG_IGNORED); return S_OK; }
static IMMNotificationClientVtbl notify_vtbl = { n_qi, n_addref, n_release, n_state, n_added, n_removed, n_default, n_property };
```
Rules: callbacks run on Audiosrv/RPC threads, must not block, must not call back into the enumerator,
must not (un)register. `OnPropertyValueChanged` is chatty — only name/format keys count (analog of Pulse
ignoring sink-input events). `OnDefaultDeviceChanged` triggers a rescan only; **open streams are never
moved** (decision 7).

#### 5.6.4 scan (manager thread)
```
for flow in (eRender, eCapture):
   IMMDeviceEnumerator_EnumAudioEndpoints(e, flow, DEVICE_STATE_ACTIVE, &col); IMMDeviceCollection_GetCount
   for each: if cancelled(): release; return OK-but-cancelled
       IMMDevice_GetId(dev, &wid) -> g_utf16_to_utf8 -> name (skip if > 512 bytes); CoTaskMemFree(wid)
       IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps)
       IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &pv) (VT_LPWSTR) -> description
            fallback PKEY_Device_DeviceDesc, fallback name; PropVariantClear
       IPropertyStore_GetValue(ps, &PKEY_AudioEngine_DeviceFormat, &pv) (VT_BLOB = WAVEFORMATEX) -> channels
            fallback 2
outputs = render endpoints; inputs = capture endpoints, then one loopback per render endpoint:
   {name = render_id + ".monitor", description = render description, channels = render channels, monitor = TRUE}
cap 64 each (inputs: captures first, then loopbacks); sort each class by g_utf8_collate(description) then
id; duplicate descriptions get " (2)", " (3)" in id order; truncate at UTF-8 boundary (511 bytes).
```
No `Activate` during scans (expensive, may wake Bluetooth devices). A COM failure mid-scan returns
`FAILED` → previous lists kept.

#### 5.6.5 open (bus / mic thread)
```
resolve:
  capture && g_str_has_suffix(id, ".monitor") -> loopback = TRUE; base = id without suffix
  base == "@default" -> IMMDeviceEnumerator_GetDefaultAudioEndpoint(e, loopback||!capture ? eRender : eCapture,
                        eConsole, &dev); E_NOTFOUND / HRESULT_FROM_WIN32(ERROR_NOT_FOUND) -> "no default" text
  else -> g_utf8_to_utf16(base) -> IMMDeviceEnumerator_GetDevice(e, w, &dev); not found -> "missing" text
  IMMDevice_GetState(dev, &state); state != DEVICE_STATE_ACTIVE -> "missing" text (status LOST)
  IMMDevice_QueryInterface(dev, &IID_IMMEndpoint) -> IMMEndpoint_GetDataFlow: render required for playback and
        loopback, capture required for a microphone (a render id chosen as microphone without ".monitor" ->
        "Bu aygıt bir giriş aygıtı değil." FAILED)
activate: IMMDevice_Activate(dev, &IID_IAudioClient, CLSCTX_ALL, NULL, (void**)&client)
optional RAW: QI IAudioClient2; AudioClientProperties p = {sizeof p, FALSE, AudioCategory_Media,
        AUDCLNT_STREAMOPTIONS_RAW}; IAudioClient2_SetClientProperties(c2, &p)
        (if Initialize then fails with AUDCLNT_E_RAW_MODE_UNSUPPORTED or E_INVALIDARG: re-Activate, retry without RAW)
period: IAudioClient_GetDevicePeriod(client, &def_period, &min_period)   /* allowed before Initialize */
format: WAVEFORMATEXTENSIBLE f = {0}; f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE; nChannels = 2;
        nSamplesPerSec = rate (capture: 48000); wBitsPerSample = 32; nBlockAlign = 8; nAvgBytesPerSec = rate*8;
        cbSize = 22; Samples.wValidBitsPerSample = 32; dwChannelMask = SPEAKER_FRONT_LEFT|SPEAKER_FRONT_RIGHT;
        SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
render:  period_frames = def_period*rate/10^7 (>=1)
         duration = (MAX(buffer_frames, 2*period_frames) + period_frames) * 10^7 / rate   (REFERENCE_TIME)
         flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM
               | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY | AUDCLNT_STREAMFLAGS_NOPERSIST
         IAudioClient_Initialize(client, AUDCLNT_SHAREMODE_SHARED, flags, duration, 0, (WAVEFORMATEX *)&f,
                                 &RADIOS_SESSION_GUID[bus_index])
         event = CreateEventW(NULL, FALSE, FALSE, NULL); IAudioClient_SetEventHandle(client, event)
         IAudioClient_GetService(client, &IID_IAudioRenderClient, (void **)&render)
capture: flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | SRC_DEFAULT_QUALITY | NOPERSIST | (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0)
         duration = 200 ms; no event (polled every 5 ms — event-driven loopback is not reliable on all builds)
         IAudioClient_GetService(client, &IID_IAudioCaptureClient, …)
both:    IAudioClient_GetBufferSize(client, &buffer); IAudioClient_GetService(client, &IID_IAudioClock, &clock);
         IAudioClock_GetFrequency(clock, &freq)
         IAudioClient_GetService(client, &IID_IAudioSessionControl, &ctl);
         IAudioSessionControl_SetDisplayName(ctl, L"RadioS MAIN"|L"RadioS CUE"|L"RadioS MONITOR"|L"RadioS mikrofon", NULL)
         QI IAudioSessionControl2 -> IAudioSessionControl2_SetDuckingPreference(ctl2, TRUE)   /* never ducked by Teams etc. */
info:    rate, buffer_frames = buffer, period_frames
format fallback (AUDCLNT_E_UNSUPPORTED_FORMAT or E_INVALIDARG from Initialize with AUTOCONVERTPCM):
         re-Activate; IAudioClient_GetMixFormat(client, &mix); Initialize with *mix and the same flags minus
         AUTOCONVERTPCM/SRC; remember the mix layout; write()/read() convert:
           render: L,R -> ch0,ch1, other channels 0 (mono mix: (L+R)/2); float32, PCM16 (round+clamp),
                   PCM24-in-32 (<<8), PCM32, packed 24-bit
           capture: ch0,ch1 (mono duplicated) -> float, then 48 kHz via radios_output_resampler_new_any_rate
                   if mix rate != 48000
         info.rate = mix rate for render (snapshot will then honestly show e.g. 96000)
```
Session GUIDs: four fixed constants generated once (one per bus + mic) so the Windows volume mixer shows
"RadioS MAIN / CUE / MONITOR / mikrofon" as separate sessions (parity with the Pulse stream names).

#### 5.6.6 Stream operations
- `padding`: `IAudioClient_GetCurrentPadding(client, &n)`.
- `write`: `IAudioRenderClient_GetBuffer(render, frames, &data)`; copy/convert; `IAudioRenderClient_ReleaseBuffer(render, frames, 0)`.
- `read`: `IAudioCaptureClient_GetNextPacketSize(cap, &n)`; if `n`: `GetBuffer(cap, &data, &frames, &flags, NULL, NULL)`;
  copy/convert (respect `AUDCLNT_BUFFERFLAGS_SILENT`; ignore `DATA_DISCONTINUITY` apart from logging);
  `ReleaseBuffer(cap, frames)`. Packets > capacity are split (read with a local carry buffer).
- `position`: `IAudioClock_GetPosition(clock, &pos, &qpc)`; `seconds = pos / freq`; frames =
  `(pos / freq) * rate + (pos % freq) * rate / freq` (split to avoid overflow).
- `start`/`stop`/`reset`: `IAudioClient_Start/Stop/Reset`. `Reset` requires a stopped stream and resets the
  `IAudioClock` position to 0.
- `close`: release render/capture/clock/session/client/device, `CloseHandle(event)`; zero leaks per reopen
  (the soak test checks handle growth).
- Only the owning thread touches a stream's interfaces (avoids `AUDCLNT_E_BUFFER_OPERATION_PENDING` and
  apartment questions).

#### 5.6.7 HRESULT classification
| HRESULT | Status | Text (5.8) |
|---|---|---|
| `AUDCLNT_E_DEVICE_INVALIDATED` (0x88890004) | device still ACTIVE (re-query `GetDevice`/`GetState`, or for `@default` the default id is unchanged) → `REOPEN`, else `LOST` | "device removed" |
| `AUDCLNT_E_RESOURCES_INVALIDATED` (0x88890026) | `REOPEN` | — |
| `AUDCLNT_E_SERVICE_NOT_RUNNING` (0x88890010) | `SERVICE_DOWN` | "service down" |
| `AUDCLNT_E_DEVICE_IN_USE` (0x8889000A) | `FAILED` | "in use" |
| `AUDCLNT_E_UNSUPPORTED_FORMAT` (0x88890008) | fallback path, then `FAILED` | "format" |
| `AUDCLNT_E_BUFFER_SIZE_ERROR`/`_NOT_ALIGNED` (0x88890016/19) | retry Initialize with duration 0, then `FAILED` | generic |
| `AUDCLNT_E_RAW_MODE_UNSUPPORTED` (0x88890027) | retry without RAW | — |
| `E_NOTFOUND`, `HRESULT_FROM_WIN32(ERROR_NOT_FOUND)` (0x80070490) | `LOST` | "missing"/"no default" |
| `E_ACCESSDENIED` on capture (0x80070005) | `FAILED` | "mic privacy" |
| `AUDCLNT_E_ENDPOINT_CREATE_FAILED` (0x8889000F), `AUDCLNT_E_CPUUSAGE_EXCEEDED` (0x88890017), others | `FAILED` | generic with code |
| 2000 ms without a render event while started | `REOPEN` (resume from sleep, driver stall) | "not responding" if it repeats |

#### 5.6.8 MMCSS and timers
- Bus and mic threads: `AvSetMmThreadCharacteristicsW(L"Pro Audio", &task)`; revert with
  `AvRevertMmThreadCharacteristics` at thread exit; on failure `SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST)`.
- The engine's own render thread (`engine.c:954`) and control thread poll with 3 ms / 20 ms GLib waits.
  Windows' default timer granularity (15.6 ms) and Windows 11 power throttling of minimised/occluded apps
  would stretch those waits: the platform layer must call `timeBeginPeriod(1)` while a realtime engine exists
  and opt out of throttling with `SetProcessInformation(ProcessPowerThrottling, {Version 1, ControlMask =
  PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION, StateMask 0})`
  and give `radios-render` MMCSS "Pro Audio" too (cross-area: platform.h).
- Keep the machine awake while any bus is ready: `SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED)`
  from the platform layer (broadcast automation must not sleep).

#### 5.6.9 Windows-specific behaviours to document for operators
- `@default` follows Windows' "Varsayılan aygıt" (eConsole) at the moment the bus opens; changing the
  default while on air does not move RadioS (by design); the per-app device chosen in Windows
  "Uygulama ses düzeyi ve cihaz tercihleri" applies because it is what `GetDefaultAudioEndpoint` returns
  to the process.
- Exclusive-mode apps can lock a device (`DEVICE_IN_USE`); RadioS retries every 5 s.
- Microphone privacy (Ayarlar › Gizlilik ve güvenlik › Mikrofon › masaüstü uygulamaları) blocks capture
  with `E_ACCESSDENIED`; loopback ("Çıkış geri dinlemesi") is not affected.
- Windows audio enhancements are bypassed when RAW mode is supported; otherwise the endpoint's APOs apply.

#### 5.6.10 `guint radios_audio_hardware_max_rate(void)` (replaces `/proc/asound` scan)
Runs on a short-lived MTA worker (call from GTK thread, result cached for the process, ≤1 s budget):
for each ACTIVE render endpoint: `Activate` an `IAudioClient`; rates
{384000, 352800, 192000, 176400, 96000, 88200, 48000, 44100}: `IAudioClient_IsFormatSupported(client,
AUDCLNT_SHAREMODE_EXCLUSIVE, PCM 24-in-32 stereo @rate, NULL)` first `S_OK` wins; also include
`PKEY_AudioEngine_DeviceFormat` rate. Return the maximum or 0 (UI already prints "okunamadı" for 0).
Never opens a stream.

### 5.7 Pulse → Windows function mapping (1:1)

| Pulse backend piece | Windows |
|---|---|
| `radios_pulse_backend_new` (mainloop + context + connect NOAUTOSPAWN) | `radios_audio_backend_new`: provider from `server`; manager thread does `CoCreateInstance(MMDeviceEnumerator)` + notification registration + Audiosrv check; 3 bus + mic threads; returns at once |
| `context_state` READY | manager: connect OK and Audiosrv RUNNING → connected, reapply, rescan |
| `context_state` FAILED/TERMINATED | Audiosrv stopped / `AUDCLNT_E_SERVICE_NOT_RUNNING` → connected=FALSE, buses/mic not ready, lists cleared, 1/2/5 s retry |
| `subscribed()` | `IMMNotificationClient` → atomic `refresh_due = now+250 ms` |
| `refresh_locked` + `sink_info`/`source_info` | manager `do_scan` → `EnumAudioEndpoints` + property store, staged, atomic publish, cancel+restart |
| `radios_pulse_backend_apply` | same decision code; streams opened asynchronously by bus/mic threads |
| `pa_stream_new` + `connect_playback(dev or NULL)` | `GetDevice(id)` / `GetDefaultAudioEndpoint(eRender, eConsole)` → `Activate(IAudioClient)` → `Initialize(shared, event, autoconvert)` |
| `pa_stream_connect_record` (`.monitor` source) | capture endpoint, or `AUDCLNT_STREAMFLAGS_LOOPBACK` on the render endpoint |
| `stream_state` / `capture_state` (+ fallback rejection) | phase published by the thread; explicit ids cannot fall back |
| `write_audio` | `render_step` (`GetCurrentPadding`, `GetBuffer`, `ReleaseBuffer`) |
| `read_audio` | mic thread (`GetNextPacketSize`, `GetBuffer`, `ReleaseBuffer`) |
| `stream_flush_locked` + `flush_base_done` | `Stop` + `Reset` + consumer ring reset + refilling |
| `timing_done` / `pa_stream_update_timing_info` / `AUTO_TIMING_UPDATE` | `IAudioClock_GetPosition` each period |
| `pa_stream_get_time` | `IAudioClock_GetPosition` → frames |
| `pa_stream_get_sample_spec()->rate` | `info.rate` |
| `pa_stream_get_buffer_attr()->tlength` | `fill_target` |
| `radios_pulse_backend_configure_output` | identical |
| `radios_pulse_backend_refresh` | `scan_requested` + wake manager |
| `radios_pulse_backend_reconnect` | route retry / reapply (service reconnect on manager thread) |
| `radios_pulse_backend_flush(_buses)` | command + ≤100 ms acknowledgement |
| `radios_pulse_backend_snapshot` | identical copy, no device call |
| `radios_pulse_backend_free` | quit, join, unregister, release, `CoUninitialize` |

### 5.8 Turkish error strings (operator style: what happened + what to do; ≤ 200 bytes)

Write all of them through one helper `radios_audio_error_set(gchar dst[256], const gchar *fmt, ...)` that
formats with `g_strdup_vprintf` and truncates on a UTF-8 character boundary (`g_utf8_find_prev_char`), and
append the HRESULT as `(0x%08lX)` only where shown.

| Where (Pulse line) | Condition | Text |
|---|---|---|
| start (292, 295) | thread/event/allocation failure | `Ses arka ucu başlatılamadı. Uygulamayı yeniden başlatın.` |
| connect (301, 497, 498) | `CoCreateInstance`/registration failed | `Windows ses aygıtlarına erişilemedi (0x%08lX); birkaç saniye içinde yeniden denenecek.` |
| service down (279) | Audiosrv not running | `Windows ses hizmeti çalışmıyor; hizmet açılınca ses kendiliğinden bağlanır.` |
| unsupported server | locator not recognised | `Bu ses sunucusu adresi Windows'ta desteklenmiyor: %s` |
| missing output (174, 429) | id not found / not ACTIVE | `Seçilen çıkış aygıtı bulunamadı; başka aygıta geçilmedi. Aygıtı takın ya da başka bir çıkış seçin.` |
| missing input (188, 446) | id not found / not ACTIVE | `Seçilen giriş aygıtı bulunamadı; başka aygıta geçilmedi. Aygıtı takın ya da başka bir giriş seçin.` |
| no default output | `GetDefaultAudioEndpoint` E_NOTFOUND | `Windows'ta varsayılan çıkış aygıtı yok. Bir ses aygıtı bağlayın ya da çıkış seçin.` |
| no default input | same, capture | `Windows'ta varsayılan giriş aygıtı yok. Bir mikrofon bağlayın ya da giriş seçin.` |
| wrong direction | render id used as microphone | `Bu aygıt bir giriş aygıtı değil; mikrofon için giriş ya da çıkış geri dinlemesi seçin.` |
| stream alloc (422) | Activate/Initialize/GetService other | `Çıkış akışı açılamadı (0x%08lX). Aygıtı yeniden seçin.` |
| capture alloc (447) | same for capture | `Giriş akışı açılamadı (0x%08lX). Aygıtı yeniden seçin.` |
| device removed (156, 178) | `DEVICE_INVALIDATED` + not ACTIVE | `Ses aygıtı çıkarıldı ya da devre dışı bırakıldı. Aygıt geri gelince yeniden bağlanır.` |
| in use | `DEVICE_IN_USE` | `Aygıt başka bir uygulamanın özel kullanımında. O uygulamayı kapatın; 5 saniyede bir yeniden denenir.` |
| format | `UNSUPPORTED_FORMAT` after fallback | `Aygıt %u Hz stereo çıkışı kabul etmedi. Başka örnekleme hızı ya da aygıt seçin.` |
| mic privacy | capture `E_ACCESSDENIED` | `Mikrofon izni kapalı: Ayarlar › Gizlilik ve güvenlik › Mikrofon bölümünde masaüstü uygulamalarına izin verin.` |
| stall | repeated REOPEN | `Ses aygıtı yanıt vermiyor; yeniden açılıyor.` |
| resampler overflow (120) | `consumed != needed` | `Çıkış örnekleyici kuyruğu taştı; çıkış yeniden açılıyor.` |
| resampler create (104, 117, 418) | existing Turkish GError messages | keep the message from `output_resampler.c` |
| generic | anything else | `Ses aygıtı hatası (0x%08lX). Aygıtı yeniden seçin.` |

The deck error copied by the engine (`engine.c:1058`) and the Preferences status "Çıkış aygıtı: %s"
(`preferences.c:432`) display these verbatim. `test_audio_recovery.c:93` must check
`strstr(error, "bulunamadı")` instead of `"fallback"`/`"No such"`.

### 5.9 Null provider (deterministic virtual devices)

Purpose: run every realtime engine/GUI test (today on Pulse null sinks + `parec`) on Windows CI with no sound
hardware, deterministically, plus fast in-process hotplug/server-loss scenarios.

**Server object.** One process-global `NullServer` per spec (refcounted; created by the first null provider).
Devices:
- *sink*: `{name, description, rate (default 48000), channels (default 2), capture_path?, capture_format "f32le"|"s16le"}`.
  A sink's clock always runs while the server is running (like a Pulse null sink): every tick it
  mixes (sums, clamps nothing) all started render streams connected to it (missing frames = silence),
  appends the mixed block to its capture file (if any) and feeds every loopback stream of `<name>.monitor`.
- *source*: optional virtual microphone `{name, description, tone_hz, amplitude}` or `{…, file}` (raw f32le
  48 kHz stereo, looped); its monitor flag is FALSE.
- inputs list = sources, then one `<sink>.monitor` per sink (`monitor=TRUE`, description = sink description).
- `default_sink`, `default_source` (resolve `@default`; else first sink/source).

**Streams.** Render stream: queue capacity = requested `buffer_frames` exactly (min 64) so
`snapshot.output_buffer_frames` follows the request (`test_preferences_output.c:182-221`); `period_frames =
rate/100`; `event` signalled every tick; `position` = frames the sink actually took from this stream since
start/reset; `stop` freezes, `reset` empties and zeroes position. Stream rate ≠ sink rate → libswresample
conversion inside the provider (position counted in stream frames fed to the converter). Loopback stream:
receives the sink's mixed blocks converted to 48 kHz. Virtual source stream: generates 48 kHz.

**Clock.** `"realtime"` (default): ticker thread with `CreateWaitableTimerExW(…,
CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, …)` at 5 ms; frames due = `QPC-elapsed × rate` minus frames done
(no drift). `"manual"`: nothing moves until `radios_audio_null_advance()`; each call processes
synchronously (mix, capture, monitors, positions, events) → bit-exact tests of tags/played_tag/flush.

**Capture files** replace `parec`: opened with `CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ |
FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL)` (runner may
pre-create/truncate), one unbuffered `WriteFile` per tick so a concurrently reading test sees growth within
≤5 ms (`test_audio_ui.c:225-260` reads while writing). Raw little-endian, no header, sink rate, interleaved.

**Spec (environment `RADIOS_AUDIO_SERVER=null:<abs path>`)**, UTF-8 JSON:
```json
{ "schema": 1, "running": true, "clock": "realtime",
  "sinks": [ { "name": "radios_gui_1a2b_main", "description": "RadioS sanal MAIN", "rate": 48000, "channels": 2,
               "capture": "C:\\…\\capture-main.f32", "capture_format": "f32le" } ],
  "sources": [ { "name": "radios_virtual_mic", "description": "Sanal mikrofon", "tone_hz": 880, "amplitude": 0.5 } ],
  "default_sink": "radios_gui_1a2b_main", "default_source": "radios_gui_1a2b_monitor.monitor" }
```
The ticker polls the file's last-write time/size every 50 ms (`GetFileAttributesExW`) and applies the diff:
added/removed devices → hotplug notifications (`RADIOS_AUDIO_HOTPLUG_DEVICES`), streams on a removed sink
fail with `LOST`; `"running": false` → every stream call returns `SERVICE_DOWN`, `service_running()` FALSE,
`scan` returns nothing (equivalent to killing the Pulse daemon); back to `true` → devices reappear
(`restart`). Runners rewrite the file atomically (write temp + `os.replace`, retry on `PermissionError`).

**In-process test API (`src/audio/audio_null.h`, test-only, exported from `radios_audio`):**
```c
typedef struct { const gchar *name, *description; guint rate, channels; const gchar *capture_path; gboolean capture_s16; } RadioSAudioNullSink;
gboolean radios_audio_null_add_sink(const RadioSAudioNullSink *sink, GError **error);
gboolean radios_audio_null_add_tone_source(const gchar *name, const gchar *description, gdouble hz, gdouble amplitude, GError **error);
gboolean radios_audio_null_remove_device(const gchar *name);
void     radios_audio_null_set_running(gboolean running);           /* "stop/restart the server" */
void     radios_audio_null_set_defaults(const gchar *sink, const gchar *source);
void     radios_audio_null_set_manual_clock(gboolean manual);
void     radios_audio_null_advance(const gchar *sink /* NULL = all */, guint64 frames_at_sink_rate);
guint64  radios_audio_null_sink_frames(const gchar *sink);          /* frames its clock produced */
gpointer radios_audio_null_server_hold(void); void radios_audio_null_server_release(gpointer);  /* keep capture clocks alive across engines */
```
These act on the server selected by `null:` (empty spec) or on the spec-file server of the process.
Device names are free UTF-8 (≤503 bytes so `.monitor` fits), error texts are the same Turkish table.

### 5.10 Engine edits (exact)

- `audio_internal.h:16-36`: `typedef struct RadioSAudioBackend RadioSAudioBackend;`
  `typedef struct {…same fields…} RadioSAudioBackendSnapshot;` functions
  `radios_audio_backend_{new,apply,configure_output,refresh,flush,flush_buses,snapshot,reconnect,free}` with the
  same parameters; keep the comment on `reconnect`. Add `const gchar *radios_audio_backend_label(RadioSAudioBackend *)`.
- `engine.c:171` `RadioSPulseBackend *pulse;` → `RadioSAudioBackend *backend;`; `engine.c:172` snapshot type.
- `engine.c:995, 1062, 1063, 1064, 1065, 1066, 1067, 1068, 1073, 1074, 1134, 1160`: renamed calls/types.
- `engine.c:1098` and `audio.h:115`: parameter `pulse_server` → `server`.
- `engine.c:1195`: `radios_time_zone_new_identifier()` (platform wrapper; mandatory, see section 4).
- Optional: at the top of `render_worker` (`engine.c:954`) call the platform MMCSS helper.
- No other engine logic changes: loss detection, flush settling, pacing and history are backend-agnostic.

### 5.11 Other consumers (cross-area)
- `preferences.c:330-359,552`: call `radios_audio_hardware_max_rate()`; `preferences.c:517`: show
  `radios_audio_engine_backend_label()` (new tiny engine API returning the provider label) instead of
  "PulseAudio / PipeWire".
- `channel_input.c` (one Pulse record stream per mixer channel, ids `in:<source>` from `dj.c:1948`) and
  `patchbay.c` (Pulse modules: null sinks visible to other apps, loopbacks, moving other apps' sink-inputs)
  must use the same provider vtable, device ids and `radios_audio_server_resolve()`. Per-application capture
  on Windows = process loopback (`ActivateAudioInterfaceAsync` with `VAD\Process_Loopback`, Win10 20348+,
  header missing in mingw — see PLATFORM_FACTS). Virtual devices visible to other applications have no
  user-mode WASAPI equivalent.
- `tools/radios_hardware_check.py`: needs `RadioS.exe --audio-inventory-json` (uses this area's scan:
  outputs/inputs with id, description, channels, monitor, state, mix format) instead of `pactl`.
- `docs/08-ses-motoru.md` lines 3, 18, 30, 32, 44 describe libpulse/pactl/parec — docs area.

---------------------------------------------------------------------------------------------------

## 6. Tests

### 6.1 Run on Windows without any virtual device (unchanged source)
- `test_output_resampler` (pure FFmpeg).
- `test_audio` offline cases: mix, transport, preview-isolation, validation, replacement, 33-decks,
  sampler-voice-isolation, transient-unload (needs `g_unlink` of a file the decoder may still hold open —
  decoder area must open with `FILE_SHARE_DELETE` or close first), mixer-graph-pfl, all loudness cases,
  microphone-latch-ptt-keeps-transport, on-air-mute-keeps-manual-fader, external-main-session,
  external-stop-clears-master-tail. Its realtime cases `g_test_skip` when `RADIOS_TEST_MAIN_SINK` is unset.
- `test_runner` (its single realtime case skips without `RADIOS_TEST_*_SINK`, `test_runner.c:457-458`) and the
  other `foreach(component …)` engine tests built at `CMakeLists.txt:226-232` (`test_dsp`, `test_tempo`,
  `test_transition`, `test_effect_return`) as far as they use the offline engine (not individually audited
  here). `test_audio_decoder` needs decoder-area fixes
  (`ftruncate`, `pwrite`, `open` without `O_BINARY`).

### 6.2 Need the null provider (runner/source changes)
| Test | Change |
|---|---|
| `audio_devices` = `run_audio_ui.py --engine-only test_audio` and every `run_audio_ui.py` GUI test | Windows branch: skip `pactl`/`parec` checks; build `unique="radios_gui_"+hex`; write `<root>/null-audio.json` with three sinks `<unique>_main/_cue/_monitor` (48000 Hz, `capture-<bus>.f32`, f32le); set `RADIOS_AUDIO_SERVER=null:<json>`, `RADIOS_TEST_{MAIN,CUE,MONITOR}_SINK`, `RADIOS_TEST_{…}_CAPTURE`, `RADIOS_TEST_MIC_SOURCE=<unique>_monitor.monitor`; pre-create capture files; drop the "capture process stopped" check and module unloading; DISPLAY check → none on Windows. The preferences JSON posted to SDBASE stays identical. |
| `preferences_output` (`run_preferences_output.py`) | one 44100 Hz sink `radios_output_accept_<hex>` with `capture=actual-44100.f32`; env as today; C test unchanged (uses `<sink>.monitor` mic at −60 dB). Initial 48 kHz stream on the 44.1 kHz sink exercises the provider SRC path. |
| `audio_recovery` | Windows runner just executes the test with `RADIOS_TEST_AUDIO_SERVER=null:` (in-memory). `test_audio_recovery.c`: replace `signal_daemon("stop-N")`/markers by `radios_audio_null_set_running(FALSE/TRUE)` plus add/remove of `radios_recovery_main/_cue` vs `fallback_only` sinks; replace `pactl load/unload-module` hotplug with `radios_audio_null_add_sink/remove_device` (description `Takılan_aygıt`); replace `parec` + `kill/waitpid` with a sink `capture_path`; `getpid()` → `GetCurrentProcessId()` via GLib-free platform helper or `g_random`; error check → `"bulunamadı"`. All timing assertions remain. |
| `audio_soak` | null spec with `radios_soak_main` (capture `monitor.pcm`, **s16le**, which `pcm_statistics` expects) and `radios_soak_cue`; `RADIOS_TEST_AUDIO_SERVER`; ffmpeg/ffprobe from MSYS2 still used for the MP3 check. `rss_bytes()` → `GetProcessMemoryInfo(… PROCESS_MEMORY_COUNTERS_EX).WorkingSetSize` (psapi); `fd_count()` → `GetProcessHandleCount` (re-baseline the +12/+6 tolerance: COM/threads add handles at start, the check is for leaks across the run). |
| `pulse_hotplug` | replaced by 6.3 (a). |
| `test_runner_pool_preload`, `test_channel_input`, `test_header_navigation_backend`, `test_stream_metadata` (window hotplug uses `pactl` at `test_stream_metadata.c:787-803` → null API), `test_dj_ui` (`.monitor` inputs) | same env contract; hotplug code paths switch to `audio_null.h`. |
| `run_audio_routing.py` | patchbay area (applications, `pacat`, sink-inputs) — out of scope here; needs process-loopback design. |

### 6.3 New tests
a. **`test_audio_backend_hotplug.c`** (replaces `test_pulse_hotplug.c`): a fake provider (`RadioSAudioProviderOps`
   implemented in the test, ~200 lines, like the fake libpulse) with configurable sink/source lists, per-item
   scan delay that checks `cancelled()` between items, `notify()` emitter (DEVICES/IGNORED kinds) and a scan
   counter. Port the three cases verbatim: atomic publish (seen sizes ⊂ {old,new}), hotplug followed without
   refresh (latency ≥200 ms and <2 s, exactly one scan per 50-event burst, IGNORED events cause no scan),
   snapshot < 20 ms with 64+64 devices under continuous rescans. Plus: service down/up → lists cleared,
   `connected` toggles, `reapply` re-opens buses; route retry after 5 s; flush keeps a READY bus ready and
   resets counters; flush of a stalled bus (fake never acks) returns within ~100 ms with `ready=FALSE`.
b. **`test_null_provider.c`** (manual clock, fully deterministic): open MAIN on a null sink via the engine in
   realtime mode, `radios_audio_null_advance(…)` by exact frame counts and assert `consumed_frames`,
   `played_tag` (= last tag of consumed frames), spans/peaks, refill rule after `flush` (no silence before
   fresh audio; position stays 0 until started), 44.1 kHz path tag mapping, loopback mic peak, capture file
   bytes, removed sink → `bus_error` contains "bulunamadı" and the engine increments `loss_epoch`.
c. **`test_wasapi_smoke.c`** (label `hardware`, returns 77 unless `RADIOS_TEST_WASAPI=1` and Audiosrv runs):
   enumerate (≥1 output on the target machine), open MAIN on `@default` at 48000/2400 and 44100/256, render a
   −50 dBFS 997 Hz tone through the realtime engine for 1.5 s, assert `consumed_frames` ≈ 1.5 s ±10 %,
   `output_sample_rate` = requested, `output_buffer_frames` > 0, and a loopback microphone `<default id>.monitor`
   sees the tone (Goertzel); unplug cases stay manual (operator checklist).

### 6.4 Test-source portability items (in my area's tests)
- `test_audio_ui.c:225-260`: `open/fstat/pread` → `_wopen` with `O_RDONLY|O_BINARY` + `_lseeki64` + `_read`,
  or `GFile`/`g_file_get_contents` with offset; `struct stat st_size` → `GStatBuf` via `g_stat` (64-bit).
- `test_audio_recovery.c:1-9,19,120-121,271-300`: remove `<signal.h>`, `<sys/wait.h>`, `<unistd.h>`,
  `g_chmod` (no-op on Windows, keep or drop), `g_spawn_sync("pactl")`, `open(O_CREAT)`, `kill`, `waitpid`.
- `test_audio_soak.c:9,28-43`: `/proc/self/statm`, `sysconf`, `/proc/self/fd`.
- `_POSIX_C_SOURCE` defines in these tests are harmless with mingw-w64.
- Runners: no `os.O_DIRECTORY`, no `/proc/<pid>/fd` socket aliases, no `setsid`/`kill -0`.

---------------------------------------------------------------------------------------------------

## 7. Risks and open questions

1. **IAudioClock during starvation** (shared mode) may advance; mitigated by `min(played, written)`;
   at most a few ms of early `played_tag` after an underrun (same order as Pulse).
2. **AUTOCONVERTPCM double conversion** when the operator picks 44.1 kHz on a 48 kHz device (RadioS
   48k→44.1k + Windows 44.1k→48k). Kept deliberately for contract fidelity; document "choose the device's
   native rate".
3. **Minimum shared-mode latency** is ~2 device periods (≈20 ms); `output_buffer_frames` < that is raised
   (reported honestly). True low latency needs `IAudioClient3::InitializeSharedAudioStream` (format must equal
   the mix format, own SRC) — phase 2.
4. **Silent reopen** on `DEVICE_INVALIDATED` with the device still present (format change, sleep/resume)
   loses ≤ one device buffer of audio but avoids pausing the whole broadcast; differs from Pulse (where such
   events did not exist). If the lead prefers strict Pulse semantics, map `REOPEN` to `LOST`.
5. **Loopback delivers no packets on an idle output** → mic peak decay rule added; tests using
   `<sink>.monitor` as microphone are deterministic only on the null provider.
6. **Windows timer resolution / power throttling** affect the engine's 3 ms render poll (section 5.6.8) —
   needs the platform layer; without it underruns appear when the window is minimised.
7. **GLib monotonic clock**: verify `g_get_monotonic_time()` resolution on GLib 2.90/Windows (QPC expected);
   refill timeouts, debounce and the hotplug latency assertions depend on ≤1 ms resolution.
8. **IANA time zone** at `engine.c:1195` blocks preference application on Windows unless the platform wrapper
   is used.
9. Notification-client lifetime races (callback after unregister) — handled by the refcounted relay with an
   `alive` flag.
10. Device descriptions are localized by Windows ("Hoparlör …"); ids are stable across reboots but change if
    a USB device moves to another port (Pulse names had the same property).
11. Null-provider capture is in-process (not an "independent" process like `parec`); it is still a separate
    code path from the engine. Hardware acceptance stays manual (`test_wasapi_smoke` + operator checklist).

## 8. Implementation order

1. Rename API in `audio_internal.h`/`engine.c`; add `backend.c` locator and an "unsupported" stub provider
   so the engine builds and offline tests pass.
2. `backend_core.c` + `audio_provider.h` + fake-provider `test_audio_backend_hotplug.c`.
3. `null_provider.c` + `audio_null.h` + `test_null_provider.c`; port runners (6.2) → realtime suites green
   on CI without hardware.
4. `wasapi_provider.c` (enumeration + notification first, then render, capture/loopback, flush, errors,
   fallback format) + `test_wasapi_smoke.c` on the target machine.
5. Cross-area hooks: `radios_audio_hardware_max_rate`, backend label, platform timer/MMCSS/power helpers,
   time-zone wrapper.
