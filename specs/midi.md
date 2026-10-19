# Area spec: `midi` — ALSA sequencer → WinMM (Windows 10/11 x64)

Analysed copy: `C:\Users\sonatturkan\Desktop\RadioS` (read-only). All line numbers refer to that copy.

## Files in scope

| File | Lines | Role |
|---|---|---|
| `include/radios/midi.h` | 55 | Public MIDI input API (port listing, open/close, events, notices, mapping keys) |
| `src/core/midi.c` | 254 | ALSA sequencer implementation + platform-neutral key/label/delivery code |
| `include/radios/midi_ui.h` | 20 | MIDI service API used by the main window, the Settings page and the Technical page |
| `src/ui/midi_ui.c` | 702 | MIDI service: device follow/reconnect, background jobs, action dispatch, Settings page (learn/assign), Technical map selector, live verify dialog |
| `tests/test_midi.c` | 73 | `/midi/keys` (portable) and `/midi/virtual-controller` (real ALSA sequencer) |
| `tests/test_midi_hotplug.c` | 539 | In-process fake ALSA sequencer and `#include "../src/core/midi.c"`: 2 input tests + 3 UI tests (R8.4) |
| `CMakeLists.txt` | 19, 93–94, 191–208 | `pkg_check_modules(ALSA …)`, link, test targets |
| Callers | `src/ui/window.c:790`, `src/ui/preferences.c:57-65,257-264,778`, `src/ui/technical.c:737-739,789-797`, `src/ui/reset_ui.c:18` | Bind the service, Settings › MIDI category, Technical page, reset label |
| SDBASE | `database/SDBASE/src/sdbase/desktop_api.py:29-36,42-69,600-604`, `desktop_reset.py:21`, `desktop_transfer_package.py:67` | `midi_mapping` document validation, reset and transfer (pure Python, platform-neutral) |
| Docs | `docs/kilavuz-2026-10-18-C-C11.md:153-264` (R8.4 design record), `docs/kilavuz-2026-10-18-D.md:121,136`, `docs/kilavuz-2026-10-18-matris.md:71` | Rationale for announce port, liveness check, background jobs, test harness |

Nothing in this area uses X11, `/proc`, signals, uid checks, `file://` URIs or path separators. The Linux-specific parts are only: libasound (ALSA sequencer), `poll()`, a self-pipe (`pipe/write/close`), `EINTR`, and the ALSA-specific tests and CMake lines.

---

## 0. Summary of the Windows design (TL;DR)

1. The public API in `midi.h` stays the same: the same functions, types, semantics and error domain. There are two additive changes: a portable raw-message decoder `radios_midi_decode_message()` and, recommended, a public error-code enum.
2. `src/core/midi.c` becomes **common code** (keys, labels, decoder, `Listener`/`Delivery`, `deliver/queue/notify/lost`) that `#include`s **one backend**: `midi_alsa.c` (today's ALSA code, unchanged in behaviour) or `midi_winmm.c` (new). Selection is at compile time with `G_OS_WIN32`. `tests/test_midi_hotplug.c` keeps working because it still includes `midi.c`.
3. The Windows backend uses WinMM `midiIn*` with **CALLBACK_FUNCTION**. The callback only pushes the 32-bit short message (or a SysEx header pointer, or "driver closed") into a CRITICAL_SECTION-guarded ring and calls `SetEvent`. A **worker thread** per input (as today) waits with `WaitForMultipleObjects` on {stop, data, hotplug} with a 1 s timeout. It decodes, then delivers to the `GMainContext` through the existing `queue()` (idle source at `G_PRIORITY_HIGH_IDLE`), so delivery code is not changed. `CALLBACK_EVENT` is **not** available for MIDI input; `CALLBACK_THREAD` is an acceptable alternative.
4. **Hotplug.** Each input registers `CM_Register_Notification` (device-interface filter, `CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES`).
   - A REMOVAL whose symbolic link matches the input's WinMM device-interface path (`midiInMessage(DRV_QUERYDEVICEINTERFACE)`, compared case-insensitively) means **LOST at once**. A driver `MIM_CLOSE` also means LOST.
   - An ARRIVAL triggers a burst of WinMM list rescans. **PORT** is emitted when the WinMM list gains a device, never straight from the PnP event, because WinMM's list lags PnP.
   - A 1 s check (stale-handle query plus presence in the list) covers systems where notifications fail. This is the analogue of the ALSA announce port plus `snd_seq_get_port_subscription`.
5. On LOST the worker **releases the `HMIDIIN` at once**, because WinMM input is exclusive per device on the classic driver stack. It keeps watching hotplug and keeps sending PORT until closed. This is the "watch" contract that `midi_ui.c` relies on.
6. **Identity.** The persisted identity stays the **display name** (`config.device`):
   - The name is the WinMM `szPname` (≤ 31 UTF-16 units, converted from the **W** API to UTF-8).
   - Exact duplicates get ` #2`, ` #3`, … ordered by device-interface path, which is stable across replug in the same USB socket.
   - The transient `RadioSMidiPort.id` becomes `"<winmm-index>:<fnv32-hex>"` (≤ 13 chars, fits `id[24]`). It is re-verified at open, so an index shift between listing and opening never opens the wrong device.
7. **Tests.**
   - `test_midi.c`: `/midi/keys` stays portable. Add a portable `/midi/decode`. `/midi/virtual-controller` on Windows runs only against an explicitly configured loopback port (environment variable) and is skipped otherwise. It never auto-opens real hardware.
   - The hotplug test gets a Windows twin backed by an **injectable WinMM/CfgMgr API table** (fake devices, index shifting, stale handles, exclusive open, PnP notifications), mirroring today's fake ALSA layer.
   - `ui-*` cases need a Windows GUI runner; the Xvfb runner returns 77 on Windows.
8. `midi_ui.c` needs **no functional change** for parity. Recommended: show "busy" (`MMSYSERR_ALLOCATED`) instead of "takılı değil", and accept `*.JSON` in the map file filter.

---

## 1. What exists today (Linux)

### 1.1 Public API and promises (`include/radios/midi.h`)

| Line | Item | Promise to callers |
|---|---|---|
| 5 | header comment | "MIDI 1 input from ALSA sequencer ports (USB controllers, virtual ports)." |
| 6-9 | `RadioSMidiPort { gchar id[24]; gchar name[192]; }` | `id` = `"client:port"` (transient address, used only to open). `name` = `"Client · Port"` as reported. **`name` is what gets persisted** (see 1.5). Freed with `g_free` (array free func). |
| 10 | `RadioSMidiKind` | `NOTE`, `CONTROL`, `PITCH` |
| 11-17 | `RadioSMidiEvent` | `channel` 1..16; `number` 0..127 (0 for pitch); `value` 0..1 (velocity/127, CC/127, bend/16383); `pressed` = note-on with velocity > 0, or CC/bend value ≥ 0.5 |
| 19-23 | `radios_midi_event_key/key_valid/key_label` | Stable mapping keys `note:CH:N`, `cc:CH:N`, `pitch:CH`; labels `"Nota 36 · Kanal 1"`, `"Kontrol (CC) 7 · Kanal 3"`, `"Pitch bend · Kanal 2"` |
| 25-28 | `radios_midi_list_inputs(GError**)` | Readable input ports except the system and "Midi Through" clients. Returns `GPtrArray` of `RadioSMidiPort*`. **Blocking: must be called off the GTK thread.** NULL + error only when the sequencer cannot be opened. |
| 30 | `RadioSMidiCallback(const RadioSMidiEvent*, gpointer)` | |
| 31-36 | `RadioSMidiNotice { LOST, PORT }`, `RadioSMidiNoticeCallback` | LOST: the device port left (USB unplug) or our subscription was dropped. PORT: another client created a port others can read (e.g. a controller plugged in). |
| 38-40 | `radios_midi_input_open(port_id, cb, user, err)` | Subscribes and reads on a worker thread. Events go to the thread-default main context **at open time**. |
| 41-47 | `radios_midi_input_open_full(port_id, ctx, cb, notice, user, err)` | Delivery on `ctx` (NULL = thread default at open time), so it can be opened from a worker thread. **LOST is sent once**; the input **stays open and keeps reporting PORT until closed**. Opening may be slow (USB enumeration): call it off the GTK thread. |
| 48 | `radios_midi_input_port` | Returns the `port_id` string given to open. |
| 49-51 | `radios_midi_input_alive` | FALSE once the device is gone or no longer feeds the input: at once via the announce port, otherwise within about 1 s. |
| 52-54 | `radios_midi_input_close` | Stops the worker. **No callback runs after it returns.** Must be called on the thread of the delivery context. NULL-safe. |

The error domain is the private quark `"radios-midi"` (`midi.c:35`). Codes: **1** = sequencer/system unavailable ("MIDI sistemi açılamadı: %s"), **2** = invalid port id or NULL callback ("Geçerli bir MIDI giriş portu seçin."), **3** = connect failed ("MIDI cihazına bağlanılamadı (%s): %s").

### 1.2 How `src/core/midi.c` uses the ALSA sequencer

**Includes (lines 1-6):** `<alsa/asoundlib.h>`, `<errno.h>`, `<poll.h>`, `<string.h>`, `<unistd.h>`.

**Platform-neutral parts (keep verbatim in the common file):**
- `LIVENESS_MS 1000` (line 10). The worker checks liveness this often.
- `Listener { refs, stopped, callback, notice, user }` (12-18). It is shared between the input and every queued delivery, so a delivery queued before close is dropped safely after close (it checks `stopped`). Ref-counted (`listener_unref` 30-31).
- `Delivery { listener, notice, what, event }` (29), `delivery_free` (32-33).
- `midi_error()` quark (35).
- Keys and labels (36-59): `radios_midi_event_key`, `radios_midi_key_valid` (split on `:`; `note|cc` with 3 parts or `pitch` with 2; channel 1..16 via `g_ascii_string_to_unsigned`; number 0..127), `radios_midi_key_label`. Invalid keys return the key itself.
- `deliver()` (87-94): on the context thread; returns at once if `listener->stopped`; otherwise calls the event or notice callback.
- `queue()` (95-103): takes a listener ref and **always** attaches a new `g_idle_source` with priority `G_PRIORITY_HIGH_IDLE` to `input->context`. The comment at 98-99 explains why not `g_main_context_invoke_full`: it would run the callback inline on the reader thread when the context is idle or unowned. Same-priority idle sources dispatch in attach order, so events stay FIFO.
- `notify()` (104-108): queues a notice only if a notice callback exists.
- `lost()` (109-112): `g_atomic_int_compare_and_exchange(&alive, 1, 0)`, then a LOST notice. This is the **exactly-once** guarantee.
- Wrappers `radios_midi_input_open` (239-240), `_port` (241), `_alive` (242).

**ALSA-specific parts:**

1. **Listing** `radios_midi_list_inputs` (60-86):
   - `snd_seq_open(&seq, "default", SND_SEQ_OPEN_INPUT, 0)`, a blocking client used only for the listing; on failure, error code 1 and NULL.
   - Iterates clients with `snd_seq_query_next_client`. Skips `SND_SEQ_CLIENT_SYSTEM` (0), its own client id, and client names starting with `"Midi Through"`.
   - For each port (`snd_seq_query_next_port`), keeps those with caps `READ|SUBS_READ` and without `NO_EXPORT`.
   - `id = "%d:%d"` (client:port).
   - `name` = the port name if it already starts with the client name, else `"%s · %s"` (client · port).
   - Closes the client. Order is by client id, then port.
2. **Open** `radios_midi_input_open_full` (207-238):
   - Validates with `sscanf("%d:%d")`, range 0..255; on failure, code 2.
   - `snd_seq_open(..., SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK)` plus `pipe(wake)`; on failure, code 1.
   - `snd_seq_set_client_name("RadioS")`.
   - `snd_seq_create_simple_port("RadioS denetleyici girişi", WRITE|SUBS_WRITE, MIDI_GENERIC|APPLICATION)`: a write-only port **owned by RadioS**.
   - `snd_seq_connect_from(own, client, port)` subscribes device→own; on failure, code 3.
   - Sets `alive = 1`.
   - **Announce subscription:** `snd_seq_connect_from(own, SND_SEQ_CLIENT_SYSTEM, SND_SEQ_PORT_SYSTEM_ANNOUNCE)`. A failure is ignored; the liveness check then covers removal.
   - Refs `context` (or the thread default), then `g_thread_new("radios-midi", reader, input)`.
   - Every failure path calls `radios_midi_input_close()` on the partially built input.
3. **Reader thread** `reader()` (175-206):
   - Gets poll descriptors (`snd_seq_poll_descriptors*`) plus the wake pipe.
   - Loop until `listener->stopped`: `poll(fds, n+1, LIVENESS_MS)`. `EINTR` → continue. Any other error → `lost()` and exit, because the descriptor is unusable. Wake pipe readable → exit.
   - Drains `snd_seq_event_input` in non-blocking mode. Events from the system client (`source.client == 0`) go to `announcement()`. Others go to `translate()`, and successful ones to `queue()`.
   - Every ≥ 1 s while alive: `subscribed()` (`snd_seq_get_port_subscription` sender=device, dest=own). A missing subscription → `lost()`. This catches unplug/replug at the **same address** even without announcements: removing a port drops its subscriptions (113-114).
4. **Announcements** `announcement()` (131-157), only for the system client:
   - `CLIENT_EXIT` of the device client → lost.
   - `PORT_EXIT` of the device port → lost.
   - `PORT_UNSUBSCRIBED` device→own → lost.
   - `PORT_START` by **another** client whose port is readable (`port_readable()` 123-130: `snd_seq_get_any_port_info` caps check) → **PORT** notice.
   - `CLIENT_START/CHANGE`, `PORT_CHANGE/SUBSCRIBED` → ignored.
   - Our own write-only ports and listing clients (which have no ports) never produce PORT. This prevents a reconnect loop.
5. **Translate** `translate()` (158-174):
   - `NOTEON/NOTEOFF` → NOTE: channel+1, `note & 127`, value `vel/127` (NOTEOFF → 0), pressed = value > 0.
   - `CONTROLLER` → CONTROL: `param & 127`, `CLAMP(value,0,127)/127`, pressed ≥ 0.5.
   - `PITCHBEND` → PITCH: `CLAMP(value+8192,0,16383)/16383`, pressed ≥ 0.5.
   - Anything else (SysEx, program change, aftertouch, clock, active sensing, …) → ignored.
   - The kernel's raw-MIDI→seq decoder turns every 0xBn into CONTROLLER (including channel-mode CCs 120-127) and keeps note-on velocity 0 as NOTEON with velocity 0. The result is the same (value 0, not pressed).
6. **Close** `radios_midi_input_close()` (243-254): `stopped = 1` → write a byte to the wake pipe → `g_thread_join` → `snd_seq_close` (drops the subscriptions; the kernel announces UNSUBSCRIBED/PORT_EXIT/CLIENT_EXIT to others) → close the pipe fds → unref the context → `listener_unref` → free. NULL-safe and safe on partially opened inputs.

**Side effects visible to the OS on Linux:** every open input is an ALSA client "RadioS" with port "RadioS denetleyici girişi" that other tools (e.g. `aconnect -l`) can see. Several subscribers per device are allowed, so two inputs or two applications can read the same controller at once. **Neither holds on Windows** (see 3.11).

### 1.3 Threading and delivery contract

- Producer: one reader thread per input. Consumer: the delivery `GMainContext`, which `midi_ui.c` sets to the window's (global default) context.
- Delivery is always asynchronous (idle source, `G_PRIORITY_HIGH_IDLE`), so it runs before GDK redraws (`GDK_PRIORITY_REDRAW = HIGH_IDLE+20`).
- No callback runs after close, because `deliver()` checks `listener->stopped` on the context thread and close is called on that thread.
- `open_full` may be called from any thread; `midi_ui.c` calls it from a GTask worker (`midi_ui.c:172-182`).

### 1.4 The MIDI service (`src/ui/midi_ui.c`): platform-neutral, relies on the contract above

- **Binding** (`radios_midi_ui_bind` 278-294, called from `window.c:790` except in `--smoke-test`):
  - Creates `MidiService` (14-35), attached to the window under `"radios-midi-service"` (9, 291).
  - Opens the SDBASE document kind **`midi_mapping`**, display name "MIDI eşlemesi" (292), when `ctx->data` exists.
  - Starts a **5 s** `reconnect_tick` (293, 247-253).
- **Background jobs** (142-216): listing and opening never run on the GTK thread (R8.4, `docs/kilavuz-2026-10-18-C-C11.md:198-217`).
  - Only one job runs at a time (`busy`); `again` re-runs a job when another was requested meanwhile.
  - `job_run` calls `radios_midi_list_inputs(NULL)`. If a device is configured and not connected, it opens the first port whose **`name` equals `config.device`** using `radios_midi_input_open_full(port->id, job->context, midi_event, midi_notice, service, NULL)` (176-180).
  - `job_done` (184-200) installs the input only if it is still alive and the name still matches. It closes `watch` before taking the new input (195). Otherwise `job_free` closes the job's input (168).
- **Watch semantics** (217-234):
  - `input_check()` moves a lost input to `watch` and clears `device`, so the status reads "… takılı değil".
  - The lost input stays open **only to hear PORT notices**. `midi_notice` (any notice) runs `input_check` and starts a job if a device is wanted. A replug therefore reconnects without waiting for the 5 s tick: the test requires < 1.2 s.
- **`connect_device(force)`** (235-246):
  - Closes the live input when the configured name changed, or on "Yenile" (`refresh_clicked` 368, `force=TRUE`), **before** starting the job.
  - Closes `watch` when no device is configured.
  - **On Windows this ordering is what makes exclusive devices reopenable.**
- **Dispatch** (80-139):
  - `midi_event` builds the key, records `pressed` edges and the `seen` list (learn), updates `last_key` and `events`.
  - When not `verifying`, it runs the bound action: `master.volume`, `microphone.toggle`, `deckN.{playpause,play,pause,stop,next,cue,sync,volume}`. `fader_db` uses a square law (96). Widgets are found by name `deck-%u-<suffix>` (90-94).
- **Settings › MIDI page** (`radios_midi_ui_settings_new` 487-549):
  - Device combo (`preferences-midi-device`). Combo ids are **names**. If the wanted device is missing, an extra entry `"Bağlı değil · <name>"` is shown (312).
  - "Yenile" (`preferences-midi-refresh`) and status label (`preferences-midi-status`). Status strings (314-317):
    - `"Bağlı: %s"`
    - `"%s takılı değil. Denetleyiciyi takın; bağlantı kendiliğinden kurulur."`
    - `"MIDI cihazları aranıyor…"` (before the first listing)
    - `"Listeden MIDI cihazını seçin."`
    - `"MIDI cihazı bulunamadı. Denetleyiciyi takıp Yenile'ye basın."`
  - Map create/load (`preferences-midi-create`/`-load`). Map file import is `*.json` through `GtkFileChooserDialog` (413-433).
  - Keys list (`preferences-midi-keys`) with **learn behaviour**: every control seen this session is listed, and the last pressed key is auto-selected while the page is mapped (333).
  - Action combo (`preferences-midi-action`; decks 1..6 = `MIDI_PLAYERS`), assign/clear buttons (`preferences-midi-assign`/`-clear`).
  - The page calls `job_start` on creation (547) and must be built in < 100 ms (test).
- **Technical page** (552-702): the map selector combo (`task-midi-profile`) activates and saves the active map. "Denetleyiciyi doğrula" (`task-midi-verify`) opens a live check dialog (`technical-midi-check*`, refreshed every 150 ms). It counts events, and **`verifying` suppresses dispatch**.

### 1.5 Persisted data and identifiers (what crosses platforms)

- **Only the SDBASE `midi_mapping` document.** There is no INI and no file in `config/` (verified: no MIDI keys in `*.ini/*.json` outside docs). Shape:

  ```json
  {"device": "nanoKONTROL2 MIDI 1",
   "active": "nanoKONTROL2 MIDI 1 haritası",
   "maps": [{"name": "nanoKONTROL2 MIDI 1 haritası", "device": "nanoKONTROL2 MIDI 1",
             "bindings": [{"control": "cc:1:7", "action": "deck1.volume"}]}]}
  ```

- **Validator** (`desktop_api.py:48-69`):
  - `device` is text ≤ 192 (matches `RadioSMidiPort.name[192]`).
  - `maps` ≤ 32; `name` 1..80, unique.
  - `bindings` ≤ 512, distinct `control` matching `_MIDI_CONTROL` (line 44: channel 1..16, number 0..127, `pitch:CH`).
  - `action` matches `_MIDI_ACTION` (line 45: `deck1..32.{play,pause,playpause,stop,next,cue,sync,volume}`, `master.volume`, `microphone.toggle`).
  - `active` must name a stored map.
  - The kind is in `KINDS`/`MANAGED` (lines 33, 36), in reset `SETTINGS_KINDS` (`desktop_reset.py:21`) and in the transfer labels (`desktop_transfer_package.py:67`).
- **Identifiers stored:**
  - The **display name** of the device (`config.device`, `maps[].device`). Only `config.device` drives connection; `maps[].device` is informational, set by `create_map` (380) and `import_map` (409).
  - The **control keys** (`note:CH:N` …), which are platform-neutral.
  - The ALSA `client:port` id is **never persisted**. Good for portability.
- **Cross-platform consequence:** a document created on Linux (e.g. via the transfer package) holds an ALSA-style name such as `"nanoKONTROL2 MIDI 1"`, while WinMM reports `"nanoKONTROL2"`. On Windows the UI shows `"Bağlı değil · nanoKONTROL2 MIDI 1"` and the operator re-selects the device once. Bindings remain valid. No SDBASE change is needed. Automatic name remapping is **not** recommended: it would need an implicit save.

### 1.6 Tests today

- `tests/test_midi.c`:
  - `/midi/keys` (57-66) is portable. It covers key validity, labels and `radios_midi_input_open("bad")` → NULL.
  - `/midi/virtual-controller` (17-56) needs a **real ALSA sequencer**. It creates a client and a READ port, checks that the port is listed under the expected name and that no "Midi Through" entry appears, opens the input, and sends note on/off, CC and pitch bend with `snd_seq_event_output_direct`. It expects the keys `note:1:36` (1.0), `note:1:36` (0), `cc:3:7` (64/127) and `pitch:16` (1.0) on the default context within 3 s. It then sends one more note, closes, and asserts **nothing is delivered after close**. It is skipped when the sequencer is missing.
- `tests/test_midi_hotplug.c`:
  - Fake `snd_*` functions (22-285) model clients, ports, subscriptions, announcements, a slow `snd_seq_open`, connect failures and refused announce subscriptions. It then `#include "../src/core/midi.c"` (287), so production code runs against the fake. With `-Wl,--as-needed`, libasound is not even loaded and no client ever touches the system sequencer.
  - **`/midi-hotplug/input-announcements`** (331-368):
    - Open on 20:0; the CC arrives as `cc:1:7`. Listing returns exactly 1 port named `"nanoKONTROL2 MIDI 1"`.
    - Plugging 24:0 gives exactly 1 PORT. Opening and closing another input (24:0) produces no PORT or LOST on the first input.
    - Unplugging 20 gives LOST in **< 300 ms** (from the announcement), `alive()==FALSE`, and the device's events are no longer routed.
    - Replugging 20:0 gives PORT #2. After 1.3 s the input is still not alive, LOST was sent exactly once, and nothing is routed.
    - After close, `open_clients()==0`.
  - **`/midi-hotplug/input-liveness-without-announcements`** (373-387): the announce port is refused; unplug and immediate replug at the same address → LOST within **< 2.5 s**, with **ports == 0**.
  - **`/midi-hotplug/ui-unplug-replug-refresh`** (446-482):
    - Real `midi_ui.c` service. Select the device; status `"Bağlı: nanoKONTROL2 MIDI 1"`; CC7 is listed as `"Kontrol (CC) 7 · Kanal 1"`.
    - Unplug → `"… takılı değil …"` in **< 0.5 s**.
    - Replug at **another address** between two 5 s ticks → reconnect in **< 1.2 s**; CC8 is listed.
    - "Yenile" → **exactly one new connect**, and CC9 is delivered once, to the new subscription only.
    - Worst main-loop gap **< 100 ms**.
  - **`/midi-hotplug/ui-slow-sequencer`** (488-506): every open takes 300 ms and connects fail → "takılı değil". In 7 s at least 3 sequencer opens (selection + ≥ 1 retry); worst gap < 100 ms.
  - **`/midi-hotplug/ui-teardown-pending-job`** (510-526): the window is destroyed while a job is opening the device; controller input keeps arriving; all clients close within 3 s and nothing touches the freed service.
  - The harness (289-441) is portable GLib/GTK, except that it uses the fake's globals (`fake_lock`, `fake_connects`, `fake_opens`) and literal ids `"20:0"`/`"24:0"`.
- CMake (191-208):
  - `test_midi` links `PkgConfig::ALSA`; plain `add_test` without `SKIP_RETURN_CODE`.
  - The hotplug cases exist only under `RADIOS_GUI_TESTS` and run through `tests/run_rotation_editor_regressions.py`, which starts a private **Xvfb** and **returns 77 when Xvfb is unavailable**, i.e. always on Windows. Linker flag `-Wl,--as-needed`.

---

## 2. Linux-specific constructs → Windows replacement

| Location | Construct | Why it exists | Windows replacement |
|---|---|---|---|
| `midi.c:2` | `#include <alsa/asoundlib.h>` | ALSA sequencer API | `<windows.h>`, `<mmsystem.h>` (mmeapi), `<cfgmgr32.h>`; link `winmm`, `cfgmgr32` |
| `midi.c:4` | `<poll.h>`, `poll()` (184) | Wait on seq fds + wake pipe with timeout | `WaitForMultipleObjects({stop,data,hotplug}, FALSE, timeout)` |
| `midi.c:6` | `<unistd.h>`, `pipe()` (218), `write()` (247), `close()` (250-251) | Self-pipe to stop the reader | Auto-reset event `stop` + `SetEvent` / `CloseHandle` |
| `midi.c:186` | `errno == EINTR` | Signals interrupt poll | Not applicable. `WAIT_FAILED` → `lost()` and exit, the same as a poll error |
| `midi.c:19-28` | `snd_seq_t *seq; gint wake[2]; self, own, client, port` | ALSA client/port state | Windows struct (3.4): `HMIDIIN handle`, events, ring, interface path, CM registration, … |
| `midi.c:60-86` | `snd_seq_query_next_client/port`, caps filter, "Midi Through"/system/self exclusion | Enumerate readable ports | `midiInGetNumDevs` + `midiInGetDevCapsW` + `midiInMessage(DRV_QUERYDEVICEINTERFACE[SIZE])`; optional exclusion table (3.2) |
| `midi.c:78` | `id = "client:port"` | Address for open | `id = "<index>:<fnv32>"` (3.2), verified at open |
| `midi.c:80-81` | name = port name or `"client · port"` | Display/persisted identity | name = `szPname` (UTF-16→UTF-8), with ` #N` for exact duplicates (3.2) |
| `midi.c:211` | `sscanf("%d:%d")`, range 0..255 | Parse the id | Strict parse `^[0-9]{1,4}:[0-9a-f]{8}$` → code 2 on mismatch |
| `midi.c:218` | `snd_seq_open(..., SND_SEQ_NONBLOCK)` | Reader client | `InitializeCriticalSection`, `CreateEventW` ×3, `CM_Register_Notification`, then `midiInOpen(CALLBACK_FUNCTION)` |
| `midi.c:222-229` | `set_client_name("RadioS")`, `create_simple_port`, `connect_from(device)` | Subscription device→own port | `midiInOpen(&h, index, (DWORD_PTR)midi_in_proc, (DWORD_PTR)input, CALLBACK_FUNCTION)`, SysEx buffers, `midiInStart` |
| `midi.c:233` | `connect_from(SYSTEM, ANNOUNCE)` (non-fatal) | Hotplug announcements | `CM_Register_Notification` (non-fatal: on failure, polling only) |
| `midi.c:115-122` | `subscribed()` via `snd_seq_get_port_subscription` | 1 s liveness check | `midiInGetDevCapsW((UINT_PTR)handle)` + `midiInGetID(handle)` (stale handle) + presence of our interface path in the WinMM list (2 consecutive misses) |
| `midi.c:123-130` | `port_readable()` via `snd_seq_get_any_port_info` | PORT only for readable foreign ports | WinMM list diff (every midiIn device is an input; none are ours) |
| `midi.c:136-157` | `announcement()` switch | LOST/PORT from the system client | CM callback (removal paths, arrival flag) + worker rescans (3.7) |
| `midi.c:158-174` | `translate(snd_seq_event_t*)` | Decode | `radios_midi_decode_message(bytes, 3, &event)` on the WinMM short message (3.9) |
| `midi.c:247-251` | wake write, `snd_seq_close`, close fds | Teardown | `SetEvent(stop)`, join, `midiInStop/Reset/UnprepareHeader/Close`, `CM_Unregister_Notification`, `CloseHandle`, `DeleteCriticalSection` |
| `test_midi.c:2,10-56` | ALSA virtual client/port and `snd_seq_event_output_direct` | Real-sequencer round trip | `midiOut*` to a **configured loopback** port (env var), else `g_test_skip` (4.1) |
| `test_midi_hotplug.c:10-17,22-285` | `_GNU_SOURCE`, `pipe2(O_NONBLOCK|O_CLOEXEC)`, `read/write/close`, fake `snd_*` | Fake ALSA | New fake WinMM/CfgMgr API table (4.2); no pipes |
| `CMakeLists.txt:19` | `pkg_check_modules(ALSA REQUIRED …)` | Find libasound | Only `if(NOT WIN32)`; MSYS2 has no `alsa.pc`, so configure would fail |
| `CMakeLists.txt:94,192` | `PkgConfig::ALSA` | Link | `winmm cfgmgr32` on WIN32 |
| `CMakeLists.txt:200` | `-Wl,--as-needed` | Keep libasound out of the test | ELF-only option. On WIN32, do not add it; isolation comes from the API-table injection (4.2, 4.5) |
| `CMakeLists.txt:203-205` | `run_rotation_editor_regressions.py` (Xvfb, `GDK_BACKEND=x11`) | Private X display | Windows GUI runner from the test-infra area (cross-area dependency) |

Not MIDI, but ALSA-related and found while scanning: `src/ui/preferences.c:330-359` (`hardware_max_rate()`) reads `/proc/asound/card*/codec#*|stream*`. It returns 0 on Windows (`g_dir_open` fails) and must be reimplemented with WASAPI/MMDevice by the **audio/preferences** owner.

---

## 3. Windows design

### 3.1 Source layout and backend selection

```text
src/core/midi.c          common code + #include of exactly one backend (single translation unit)
src/core/midi_alsa.c     today's ALSA code moved verbatim (list/open/reader/announcement/subscribed/translate/close)
src/core/midi_winmm.c    new WinMM backend
src/core/midi_winmm_api.h  injectable OS function table (used by midi_winmm.c and the Windows hotplug test)
```

`midi.c` structure:

1. Includes, `Listener`, `Delivery`, `listener_unref`, `delivery_free`, `midi_error`, keys/labels, **new `radios_midi_decode_message`**.
2. Forward declarations: `static void queue(RadioSMidiInput *, Delivery *); static void notify(RadioSMidiInput *, RadioSMidiNotice); static void lost(RadioSMidiInput *);`
3. `#ifdef G_OS_WIN32 #include "midi_winmm.c" #else #include "midi_alsa.c" #endif`. Each backend defines `struct RadioSMidiInput`, `radios_midi_list_inputs`, `radios_midi_input_open_full` and `radios_midi_input_close`.
4. Definitions of `deliver/queue/notify/lost` and the wrappers `radios_midi_input_open/port/alive`.

**Contract between common code and backends:** both structs must have the fields `Listener *listener; GMainContext *context; gint alive; gchar port_id[24];` under these exact names; the common functions use them. Document this in a comment at the top of each backend.

- The backend files carry the header comment `/* Included by midi.c; not a separate translation unit. */` and are **not** listed in `target_sources`.
- Quoted includes resolve relative to `src/core/`, so `tests/test_midi_hotplug.c`'s `#include "../src/core/midi.c"` keeps working unchanged.
- Do not name any field or variable `interface`: `<objbase.h>` (pulled in by `windows.h`) `#define`s it to `struct`. Use `iface`.

### 3.2 Injectable OS API (`src/core/midi_winmm_api.h`)

```c
#ifndef RADIOS_MIDI_WINMM_API_H
#define RADIOS_MIDI_WINMM_API_H
#include <windows.h>
#include <mmsystem.h>
#include <cfgmgr32.h>
typedef struct {
    UINT     (WINAPI *in_get_num_devs)(void);
    MMRESULT (WINAPI *in_get_dev_caps)(UINT_PTR device_or_handle, LPMIDIINCAPSW caps, UINT size);
    MMRESULT (WINAPI *in_open)(LPHMIDIIN handle, UINT device, DWORD_PTR callback, DWORD_PTR instance, DWORD flags);
    MMRESULT (WINAPI *in_start)(HMIDIIN);
    MMRESULT (WINAPI *in_stop)(HMIDIIN);
    MMRESULT (WINAPI *in_reset)(HMIDIIN);
    MMRESULT (WINAPI *in_close)(HMIDIIN);
    MMRESULT (WINAPI *in_prepare_header)(HMIDIIN, LPMIDIHDR, UINT);
    MMRESULT (WINAPI *in_unprepare_header)(HMIDIIN, LPMIDIHDR, UINT);
    MMRESULT (WINAPI *in_add_buffer)(HMIDIIN, LPMIDIHDR, UINT);
    MMRESULT (WINAPI *in_message)(HMIDIIN, UINT, DWORD_PTR, DWORD_PTR);
    MMRESULT (WINAPI *in_get_id)(HMIDIIN, LPUINT);
    MMRESULT (WINAPI *in_get_error_text)(MMRESULT, LPWSTR, UINT);
    CONFIGRET (WINAPI *register_notification)(PCM_NOTIFY_FILTER, PVOID, PCM_NOTIFY_CALLBACK, PHCMNOTIFICATION);
    CONFIGRET (WINAPI *unregister_notification)(HCMNOTIFICATION);
} RadioSMidiWinApi;
#endif
```

In `midi_winmm.c`:

```c
#ifndef RADIOS_MIDI_WIN_API            /* the Windows hotplug test defines it to its fake table */
static const RadioSMidiWinApi *midi_win_api(void)
{
    static RadioSMidiWinApi api; static gsize once;
    if (g_once_init_enter(&once)) {     /* runtime init: addresses of dllimport functions are not constant initializers */
        api.in_get_num_devs = midiInGetNumDevs; api.in_get_dev_caps = midiInGetDevCapsW; api.in_open = midiInOpen;
        api.in_start = midiInStart; api.in_stop = midiInStop; api.in_reset = midiInReset; api.in_close = midiInClose;
        api.in_prepare_header = midiInPrepareHeader; api.in_unprepare_header = midiInUnprepareHeader;
        api.in_add_buffer = midiInAddBuffer; api.in_message = midiInMessage; api.in_get_id = midiInGetID;
        api.in_get_error_text = midiInGetErrorTextW;
        api.register_notification = CM_Register_Notification; api.unregister_notification = CM_Unregister_Notification;
        g_once_init_leave(&once, 1);
    }
    return &api;
}
#define RADIOS_MIDI_WIN_API() midi_win_api()
#endif
```

When the test defines `RADIOS_MIDI_WIN_API`, the real table is compiled out. The test executable therefore imports no `midiIn*` or `CM_*` symbols (the analogue of "libasound not loaded").

Constants missing from MinGW's public headers (`mmddk.h` may be absent):

```c
#ifndef DRV_RESERVED
#define DRV_RESERVED 0x0800
#endif
#ifndef DRV_QUERYDEVICEINTERFACE
#define DRV_QUERYDEVICEINTERFACE     (DRV_RESERVED + 12)
#define DRV_QUERYDEVICEINTERFACESIZE (DRV_RESERVED + 13)
#endif
```

`CM_Register_Notification` needs `_WIN32_WINNT >= 0x0602` for the declaration in mingw `cfgmgr32.h`. The lead's global `-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00` covers it.

### 3.3 Enumeration, display names and transient ids

`static GPtrArray *scan(const RadioSMidiWinApi *api)` returns `WinPort { guint index; gchar *raw; gchar *name; gchar *iface; gchar *key; guint32 fingerprint; }` in **WinMM index order**:

1. `count = api->in_get_num_devs()`. For each `i < count`, `MIDIINCAPSW caps = {0}; if (in_get_dev_caps(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;`. The device may have vanished between the count and the query; skip it.
2. `raw = g_utf16_to_utf8(caps.szPname, wcsnlen(caps.szPname, MAXPNAMELEN), …)`, then `g_utf8_make_valid` and `g_strstrip`. Use the **W** API only. The A API truncates at 32 **bytes**, which can cut a UTF-8 sequence even with the UTF-8 manifest code page, and invalid UTF-8 in GTK labels is fatal under `G_DEBUG=fatal-warnings`. An empty name becomes `"MIDI girişi"`.
3. Optional exclusion table (the analogue of "System" and "Midi Through"):
   - `static const gchar *const excluded_prefixes[] = { NULL };`.
   - Classic WinMM has no always-present pass-through input, so start empty.
   - On a Windows MIDI Services machine, check whether built-in diagnostics endpoints appear in WinMM. Add only verified names and unit-test the predicate. Keep app loopbacks and loopMIDI ports visible: they are legitimate "virtual ports", which Linux lists too.
4. Interface path:
   - `ULONG bytes = 0; in_message((HMIDIIN)(UINT_PTR)i, DRV_QUERYDEVICEINTERFACESIZE, (DWORD_PTR)&bytes, 0)`. Size is in **bytes**, including the NUL; 0 means no interface (non-PnP driver).
   - Allocate `bytes + sizeof(WCHAR)`, call `in_message((HMIDIIN)(UINT_PTR)i, DRV_QUERYDEVICEINTERFACE, (DWORD_PTR)buf, bytes)`.
   - Store `iface = g_utf8_casefold(utf8(buf))` (Windows device paths are case-insensitive), or NULL. Cap at 4 KiB.
   - WinMM handles these two messages itself, and the first argument **must be a device ID cast to `HMIDIIN`, not an open handle**.
5. **Names and ordinal:**
   - Group entries by exact `raw`.
   - In a group of ≥ 2, sort by (`iface != NULL` first, `iface` ascending, then `index`). The first keeps `raw`; the k-th becomes `raw + " #k"`. Bump k on collision with another existing name.
   - Rationale: Windows normally disambiguates units itself. The legacy stack and MIDI Services' legacy naming prefix the 2nd+ unit with `"2- "`, and extra ports are named `"MIDIIN2 (Device)"`. Exact duplicates only occur with drivers or naming modes that do not disambiguate.
   - The interface path of a serial-less USB device encodes the hub port, so the ordinal is **stable across replug into the same sockets and across reboots**. ` (2)` is deliberately not used because MIDI Services new-style names already use it.
6. `key = iface ? iface : g_strconcat("name:", name, NULL)`. This is the identity used for PORT diffs.
7. `fingerprint = FNV-1a-32(name + "\n" + (iface ? iface : ""))`.

`radios_midi_list_inputs(GError **error)`:
- Maps `scan()` to `RadioSMidiPort { id = g_snprintf("%u:%08x", index, fingerprint), name = name }` (`g_strlcpy`, ≤ 191 bytes; WinMM names are ≤ 31 UTF-16 units, i.e. ≤ 93 UTF-8 bytes plus suffix).
- Never returns NULL on Windows, because WinMM has no "open the sequencer" step. Keep the `error` parameter unused (`(void)error`).
- It can block: under Windows MIDI Services the first call may start the `midisrv` service, and calls can stall during USB enumeration. The contract "call off the GTK thread" still holds and `midi_ui.c` already honours it.

### 3.4 Windows `struct RadioSMidiInput`

```c
#define RING_SIZE 1024u          /* power of two */
#define SYSEX_BUFFERS 4
#define SYSEX_BYTES 1024
#define BURST_MS 3000            /* rescans after a PnP arrival */
#define BURST_STEP_MS 250
typedef enum { RING_SHORT, RING_SYSEX, RING_CLOSED } RingKind;
typedef struct { guint8 kind; guint32 message; MIDIHDR *header; } RingItem;
struct RadioSMidiInput {
    Listener *listener; GMainContext *context; gint alive; gchar port_id[24];   /* shared field contract */
    const RadioSMidiWinApi *api;
    HMIDIIN handle;            /* NULL once released; owned by the worker after g_thread_new */
    gint closing;              /* atomic: our own Stop/Reset/Close in progress: ignore MIM_CLOSE */
    gchar *raw, *name, *iface; /* identity at open (iface casefolded, may be NULL) */
    HANDLE stop, data, hotplug;/* auto-reset events */
    CRITICAL_SECTION lock; gboolean lock_ready;
    RingItem ring[RING_SIZE]; guint head, tail; gint dropped;
    GPtrArray *removed;        /* casefolded symbolic links from CM removals (under lock) */
    gint arrived;              /* atomic: some interface arrived */
    HCMNOTIFICATION notification;
    MIDIHDR sysex[SYSEX_BUFFERS]; gchar sysex_data[SYSEX_BUFFERS][SYSEX_BYTES]; guint sysex_prepared;
    GHashTable *known;         /* WinPort.key of the last scan */
    guint misses;              /* consecutive scans without our identity */
    GThread *thread;
};
```

### 3.5 Opening (`radios_midi_input_open_full`)

1. Validate: `port_id && callback && parse_id(port_id, &index, &fingerprint)` (`^[0-9]{1,4}:[0-9a-f]{8}$`). Otherwise code 2, `"Geçerli bir MIDI giriş portu seçin."`. `test_keys` relies on `"bad"` → NULL **without touching WinMM**.
2. Allocate `input` and the `listener` (`refs = 1`, callbacks, user). Set `api = RADIOS_MIDI_WIN_API()`.
   - `InitializeCriticalSection` and set `lock_ready`.
   - Create `stop`/`data`/`hotplug` with `CreateEventW(NULL, FALSE, FALSE, NULL)`.
   - Create `removed`/`known`.
   - On failure: code 1, `"MIDI sistemi açılamadı: %s"` with `g_win32_error_message(GetLastError())`, then `radios_midi_input_close(input)`.
3. **Register the PnP notification first**, so a removal between scan and open is not missed.
   - Filter: `CM_NOTIFY_FILTER f = { .cbSize = sizeof f, .Flags = CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES, .FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE };` (ClassGuid zero).
   - Call `api->register_notification(&f, input, on_device_change, &input->notification)`. On anything but `CR_SUCCESS`, set `notification = NULL` and continue: this is the refused-announce-port case.
   - The callback may run **before the call returns**, so everything it touches (lock, events, `removed`) must already exist.
4. `ports = scan(api)`. Pick the entry with `index == index && fingerprint == fingerprint`, else the **first entry with the same fingerprint** (the device moved index between listing and open), else code 3, `"MIDI cihazına bağlanılamadı (%s): cihaz bulunamadı."`.
5. `rc = api->in_open(&input->handle, port->index, (DWORD_PTR)midi_in_proc, (DWORD_PTR)input, CALLBACK_FUNCTION)`.
   - Cast the function pointer straight to `DWORD_PTR`. Do not go through `void*`, which triggers a `-Wpedantic` error.
   - `MIM_OPEN` is delivered **during** `midiInOpen`; the callback ignores it.
   - On error: code 3 with the reason from 3.10.
6. **Verify identity after open.** `midiInGetDevCapsW((UINT_PTR)handle, …)` (documented to accept a cast handle) must report `port->raw`. If `port->iface` is set, `midiInGetID(handle,&id)` plus the interface path of `id` should equal it. On mismatch (a hotplug race), call `release_handle()`, rescan once and retry steps 4–6 once; otherwise code 3.
7. SysEx buffers, best effort: for each of `SYSEX_BUFFERS`, `lpData = sysex_data[i]`, `dwBufferLength = SYSEX_BYTES`, `in_prepare_header`, `in_add_buffer`, then `sysex_prepared++`. A failure is not fatal (fewer buffers).
8. `api->in_start(handle)`. On failure: `release_handle`, code 3.
9. Store `raw/name/iface` from `port`. Fill `known` with the `key` of every scanned port. Set `alive = 1` and `g_strlcpy(port_id)`. Set `context = ctx ? ref(ctx) : g_main_context_ref_thread_default()`. Start `thread = g_thread_new("radios-midi", reader, input)`. Return the input.

Every failure path ends with `radios_midi_input_close(input)`, which must cope with a partially built input (as on Linux).

### 3.6 WinMM callback and ring

```c
static void ring_push(RadioSMidiInput *input, RingKind kind, guint32 message, MIDIHDR *header)
{
    EnterCriticalSection(&input->lock);
    if (input->head - input->tail < RING_SIZE) input->ring[input->head++ % RING_SIZE] = (RingItem){ (guint8)kind, message, header };
    else g_atomic_int_inc(&input->dropped);        /* compiler intrinsic, not a system call */
    LeaveCriticalSection(&input->lock);
}
static void CALLBACK midi_in_proc(HMIDIIN handle, UINT message, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR param2)
{
    (void)handle; (void)param2;   /* param2 = timestamp (ms since midiInStart), unused */
    RadioSMidiInput *input = (RadioSMidiInput *)instance;
    switch (message) {
    case MIM_DATA: {
        const guint8 status = (guint8)(param1 & 0xFF), type = status & 0xF0;
        if (type != 0x80 && type != 0x90 && type != 0xB0 && type != 0xE0) return;   /* clock 0xF8, active sensing 0xFE, PC, AT … */
        ring_push(input, RING_SHORT, (guint32)param1, NULL); break; }
    case MIM_LONGDATA: ring_push(input, RING_SYSEX, 0, (MIDIHDR *)param1); break;
    case MIM_CLOSE: if (g_atomic_int_get(&input->closing)) return; ring_push(input, RING_CLOSED, 0, NULL); break;
    default: return;              /* MIM_OPEN, MIM_ERROR, MIM_LONGERROR, MIM_MOREDATA */
    }
    SetEvent(input->data);
}
```

Rules:
- **Never call any multimedia function inside `MidiInProc`** (`midiInAddBuffer`, `midiInReset`, `midiInClose`, …): the documentation warns of deadlock. Never call `midiInClose` from the callback.
- No GLib allocation or GMainContext work in the callback: it runs on a WinMM high-priority thread. Current docs allow other system calls, but keeping it to critical section + `SetEvent` is the portable and safe choice. As an alternative, `CALLBACK_THREAD` (`dwCallback` = worker thread id) gets `MM_MIM_DATA`/`MM_MIM_LONGDATA`/`MM_MIM_CLOSE` as thread messages. That needs a message queue before `midiInOpen` and a `MsgWaitForMultipleObjects` loop.
- `CALLBACK_EVENT` is **not** a valid `midiInOpen` mode: the docs list only FUNCTION/THREAD/WINDOW/NULL.
- Callbacks for one handle are serialized. Ring capacity of 1024 messages is far above controller rates (~1000 msg/s with the worker draining continuously). Overflow is counted in `dropped` and logged with `g_debug` (never `g_warning`: tests run with fatal warnings).

### 3.7 Reader thread

```c
static gpointer reader(gpointer data)
{
    RadioSMidiInput *input = data;
    HANDLE waits[3] = { input->stop, input->data, input->hotplug };
    gint64 next_scan = g_get_monotonic_time() + LIVENESS_MS * G_TIME_SPAN_MILLISECOND, burst_until = 0;
    while (!g_atomic_int_get(&input->listener->stopped)) {
        gint64 now = g_get_monotonic_time();
        DWORD timeout = next_scan > now ? (DWORD)((next_scan - now + 999) / 1000) : 0;
        DWORD r = WaitForMultipleObjects(3, waits, FALSE, timeout);
        if (r == WAIT_FAILED) { lost(input); release_handle(input, TRUE); break; }   /* = poll() error on Linux */
        if (r == WAIT_OBJECT_0) break;                                                  /* stop */
        drain(input);                                   /* SHORT → decode → queue; SYSEX → re-add; CLOSED → lost + forget handle */
        if (take_hotplug(input)) {                      /* removal paths processed here; TRUE when something arrived */
            burst_until = g_get_monotonic_time() + BURST_MS * G_TIME_SPAN_MILLISECOND; next_scan = 0;
        }
        now = g_get_monotonic_time();
        if (now >= next_scan && !g_atomic_int_get(&input->listener->stopped)) {
            rescan(input);
            next_scan = now + (now < burst_until ? BURST_STEP_MS : LIVENESS_MS) * G_TIME_SPAN_MILLISECOND;
        }
    }
    return NULL;
}
```

`drain()`:
- Under the lock, move all ring items to a local array; process them outside the lock.
- `RING_SHORT`: `guint8 b[3] = { m & 0xFF, (m >> 8) & 0xFF, (m >> 16) & 0xFF };` If `radios_midi_decode_message(b, 3, &event)` succeeds, `queue(input, delivery)`. Messages received before a LOST are still delivered; after `release_handle()` none can arrive.
- `RING_SYSEX`: if `handle && !closing && !stopped`, set `header->dwBytesRecorded = 0` and call `api->in_add_buffer(handle, header, sizeof *header)`. The content is ignored: SysEx is not mapped, the same as ALSA `translate()`.
- `RING_CLOSED` (a driver-initiated `MIM_CLOSE`; "the handle is no longer valid after this message"): `lost(input)`, then `release_handle(input, FALSE)`, which only forgets the handle and calls no `midiIn*`.

`take_hotplug()`:
- Swap out `removed` under the lock.
- For each path, remove matching entries from `known` (equal key, or same device instance; see `same_device` below).
- If `input->iface && same_device(path, input->iface)`: `lost(input); release_handle(input, TRUE);`.
- Return `g_atomic_int_compare_and_exchange(&input->arrived, 1, 0)`.

`same_device(a, b)`: the casefolded paths are equal, **or** their device-instance segments are equal. The instance segment is the text after the `\\?\` prefix up to `#{`, e.g. `usb#vid_0944&pid_0117&mi_00#7&2c3e…&0&0000`. This tolerates a different reference string (`\global`) or a different interface class of the same device, while composite USB functions (`mi_xx`) stay distinct.

`rescan()`:
1. Call `scan(api)`, checking `stopped` between devices so close is not held up. Build `now_keys`.
2. **PORT:** for each key in `now_keys` that is not in `known`, `notify(input, RADIOS_MIDI_NOTICE_PORT)`: one notice per new device. Then `known = now_keys`.
3. **Presence (only while alive):** `present = any port with (input->iface ? same_device(port->iface, input->iface) : g_str_equal(port->raw, input->raw))`. Then `misses = present ? 0 : misses + 1`. If `misses >= 2` → `lost` + `release_handle(TRUE)`. Two consecutive misses avoid a false LOST if WinMM's list is momentarily incomplete during another device's enumeration.
4. **Stale handle (only while alive and handle != NULL):** if `in_get_dev_caps((UINT_PTR)handle, …)` or `in_get_id(handle, &id)` fails → `lost` + `release_handle(TRUE)` at once. Do **not** compare the index returned by `midiInGetID` with an enumeration index to decide LOST: indexes shift when other devices go, and a false LOST would drop a working controller.

`release_handle(input, gboolean valid)`:

```c
if (!input->handle) return;
if (valid) {
    g_atomic_int_set(&input->closing, 1);
    api->in_stop(h); api->in_reset(h);                 /* returns queued SysEx headers (MIM_LONGDATA → ignored) */
    for (guint i = 0; i < input->sysex_prepared; ++i)
        if (api->in_unprepare_header(h, &input->sysex[i], sizeof(MIDIHDR)) == MIDIERR_STILLPLAYING) { api->in_reset(h); api->in_unprepare_header(h, &input->sysex[i], sizeof(MIDIHDR)); }
    api->in_close(h);                                  /* MIM_CLOSE arrives inside, ignored because closing */
}
input->handle = NULL; input->sysex_prepared = 0;
```

`midiInClose` fails with `MIDIERR_STILLPLAYING` while buffers are queued; `midiInReset` returns them. The handle is touched only by the worker after start, and by `close()` after the join, so no further locking is needed.

### 3.8 Hotplug and liveness: semantic mapping

| Linux trigger | Windows trigger | Result |
|---|---|---|
| `CLIENT_EXIT`/`PORT_EXIT` of device; `PORT_UNSUBSCRIBED` device→own | CM `DEVICEINTERFACEREMOVAL` whose link `same_device()` matches our interface; driver `MIM_CLOSE` | `lost()` at once (test: < 300 ms); handle released |
| 1 s `snd_seq_get_port_subscription` fails | 1 s stale-handle query fails; or 2 consecutive scans without our identity | `lost()` within ~1–2 s (test: < 2.5 s) |
| `poll()` error | `WAIT_FAILED` | `lost()` + exit |
| `PORT_START` of a readable foreign port | CM `DEVICEINTERFACEARRIVAL` → rescans every 250 ms for 3 s; or the periodic 1 s rescan → WinMM list gains a key | one `PORT` per new device |
| Own write-only ports and listing clients: no PORT | Opening/closing our own inputs creates no devices: no PORT | — |
| Device back at the same address: new port, old subscription stays dead | Device back on the same socket: same interface path, but its key was dropped from `known` on removal → PORT; the old input stays not alive | `alive()` stays FALSE; LOST stays exactly once |

- PORT is **never** emitted directly from the PnP arrival. WinMM's device list can lag the PnP event, and a job started too early would not find the device. It is emitted from the list diff.
- After LOST the input keeps running: it holds the CM registration and rescans, which is the "watch" role `midi_ui.c` relies on. It has **no** device handle.
- Notifications use `CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES` rather than a single class (`KSCATEGORY_AUDIO {6994AD04-93EF-11D0-A3CC-00A0C9223196}`): the WinMM interface class differs between the classic stack (KS) and Windows MIDI Services (software-device endpoints), and events are rare. A USB hub plug can fire dozens of arrivals; the worker coalesces them into one burst.
- The CM callback runs on a thread-pool thread. It must be short: casefold-copy the link into `removed` under the lock, or set `arrived`, then `SetEvent(hotplug)`. It must **never call `CM_Unregister_Notification`**, which waits for pending callbacks and would deadlock.
- When notifications cannot be registered, PORT still comes from the 1 s periodic diff (later than on Linux, which sends no PORT at all then). The fast replug on the same socket stays undetected for PORT purposes: the key never leaves `known`. That matches the Linux test expectation `ports == 0`.
- **Known limitation:** with notifications unavailable **and** a driver whose stale handle still answers `midiInGetDevCapsW`/`midiInGetID`, a replug faster than 1 s on the same socket is not detected. Notifications are always available on Windows 8+; this is hardware-verification item R1.

### 3.9 Decoding (common, `midi.c`; declared in `midi.h`)

```c
/* Decodes one complete MIDI 1.0 channel voice message (status byte first, no
 * running status). FALSE for anything RadioS does not map: system messages,
 * program change, (poly) aftertouch, incomplete input. */
gboolean radios_midi_decode_message(const guint8 *bytes, gsize length, RadioSMidiEvent *event);
```

| Raw (`status` = high nibble + channel n) | Kind | `channel` | `number` | `value` | `pressed` | ALSA equivalence (`midi.c:162-171`) |
|---|---|---|---|---|---|---|
| `8n kk vv` | NOTE | n+1 | `kk & 0x7F` | 0 | FALSE | NOTEOFF → 0 |
| `9n kk vv` | NOTE | n+1 | `kk & 0x7F` | `(vv & 0x7F)/127.0` | value > 0 | NOTEON (vel 0 stays NOTEON with value 0) |
| `Bn cc vv` | CONTROL | n+1 | `cc & 0x7F` | `(vv & 0x7F)/127.0` | value ≥ 0.5 (vv ≥ 64) | CONTROLLER, including CC 120-127 |
| `En ll mm` | PITCH | n+1 | 0 | `((mm&0x7F)<<7 | (ll&0x7F))/16383.0` | value ≥ 0.5 (raw ≥ 8192) | PITCHBEND: kernel value = raw − 8192; translate adds 8192 back |
| `An…`, `Cn…`, `Dn…`, `F0…FF`, length < 3, data byte as status | — | — | — | — | FALSE | ignored |

`memset(event, 0, sizeof *event)` first, as `translate()` does. WinMM packs `MIM_DATA` `dwParam1` as `status | data1 << 8 | data2 << 16` and always includes the status byte: drivers resolve running status. The ALSA backend keeps `translate()`; optionally, a Linux-only test can cross-check `translate()` against the decoder using `snd_midi_event_encode`.

### 3.10 Errors and Turkish strings (Windows)

- Code 2 (invalid id or NULL callback): unchanged text.
- Code 1 (event or critical-section creation failure): `"MIDI sistemi açılamadı: %s"` + `g_win32_error_message(GetLastError())`.
- Code 3: `"MIDI cihazına bağlanılamadı (%s): %s"` with the port id and a reason:
  - not found or fingerprint mismatch → `"cihaz bulunamadı; çıkarılmış ya da yeniden takılmış olabilir"`
  - `MMSYSERR_ALLOCATED` → `"cihaz başka bir uygulama tarafından kullanılıyor"`
  - `MMSYSERR_BADDEVICEID` → `"cihaz bulunamadı"`
  - `MMSYSERR_NODRIVER` → `"cihaz sürücüsü yanıt vermiyor"`
  - `MMSYSERR_NOMEM` → `"bellek yetersiz"`
  - anything else → `midiInGetErrorTextW` converted to UTF-8 (OS UI language), falling back to `"MMRESULT %u"`.
- Recommended additive API in `midi.h` (needed only for the busy UX in 3.11):

  ```c
  #define RADIOS_MIDI_ERROR (radios_midi_error_quark())
  GQuark radios_midi_error_quark(void);
  typedef enum { RADIOS_MIDI_ERROR_SYSTEM = 1, RADIOS_MIDI_ERROR_PORT = 2, RADIOS_MIDI_ERROR_CONNECT = 3, RADIOS_MIDI_ERROR_BUSY = 4 } RadioSMidiError;
  ```

  `MMSYSERR_ALLOCATED` → `RADIOS_MIDI_ERROR_BUSY` (Linux never produces it). `midi_error()` becomes `radios_midi_error_quark()`, with the same quark string `"radios-midi"`.

### 3.11 Exclusive access, Windows MIDI Services, naming

- **Classic WinMM MIDI input is exclusive per device.** A second `midiInOpen` returns `MMSYSERR_ALLOCATED`: from another application (DAW, vendor editor), from a second RadioS instance, or from our own lost "watch" input if it kept its handle. Hence:
  - (a) release the handle at LOST (3.7);
  - (b) `midi_ui.c` already closes the live input before reconnecting on "Yenile" and on name changes (`connect_device` 241-243);
  - (c) tests must **never** open real devices implicitly (4.1).
- **Windows MIDI Services** (in-box on new Windows 11 builds) repoints WinMM to `midisrv`. Effects:
  - Multi-client access: no `MMSYSERR_ALLOCATED`.
  - Port names follow the MIDI 1 naming rules ("legacy WinMM compatible" vs "new style" names such as `"<endpoint> <port>"`, `"(2)"` unit suffixes, `"group N"`). The 31-character limit stays.
  - Names may change when the user changes the naming mode, sets custom names, or when a MIDI 2.0 endpoint renames itself. The saved `config.device` then no longer matches; the UI shows "Bağlı değil · …" and the operator re-selects once.
  - The first WinMM call may take noticeably longer while the service starts. That is acceptable because it runs off the GTK thread.
- The UTF-8 manifest does not matter here: only W APIs are used, with explicit `g_utf16_to_utf8`.
- No privacy permission is involved (Windows does not gate MIDI). No COM initialization is needed for WinMM. If MIDI Services misbehaves in GTask threads, `CoInitializeEx(NULL, COINIT_MULTITHREADED)` in the worker is the fallback (risk R6).
- System sleep/resume typically removes and re-adds USB devices. The design handles it as LOST → PORT → reconnect.

### 3.12 `src/ui/midi_ui.c`: changes

No change is required for parity. All behaviour sits on the backend contract.

Recommended changes:
1. **Busy status** (Windows UX):
   - `MidiJob` gains `GError *error`. `job_run` passes `&job->error` to `radios_midi_input_open_full` (179) and keeps only the last error.
   - In `job_done`: if no input was taken and `g_error_matches(job->error, RADIOS_MIDI_ERROR, RADIOS_MIDI_ERROR_BUSY)`, set `service->unavailable = g_strdup_printf("%s başka bir uygulama tarafından kullanılıyor. Diğer uygulamayı kapatın; bağlantı kendiliğinden kurulur.", wanted)`. Otherwise clear it.
   - `page_render` (314-317), `combo_render` (565-567) and `check_tick` (607-609) prefer this text over "takılı değil" when it is set. Free it in `service_unref`.
   - Linux is unaffected: no BUSY code, and `ui-slow-sequencer` still expects "takılı değil" because its connect failure is not BUSY.
2. **Map file filter** (418): add `gtk_file_filter_add_pattern(filter, "*.JSON")` so upper-case extensions common on Windows are offered. Optionally switch to `GtkFileChooserNative` for the native Windows dialog. The path returned by `gtk_file_chooser_get_filename` is UTF-8 on Windows, and `json_parser_load_from_file` accepts it.
3. Comment-only: lines 142-145 mention "sequencer clients"; reword to "MIDI subsystem calls (ALSA sequencer / WinMM)".

Re-checked for Windows and fine as is:
- `sscanf("deck%u.%23s")` (51, 105) under UCRT.
- `G_GUINT64_FORMAT` (616).
- UTF-8 literals (`·`, `…`, `▾`, `✓`, `✗`).
- `gtk_dialog_run`.
- GTK widget lookup by name.
- 5 s `g_timeout_add_seconds`.
- GDK-Win32 keeps dispatching GLib sources during modal move/resize loops, so MIDI keeps working while the window is dragged.

### 3.13 Header comment updates (`include/radios/midi.h`; the API itself is unchanged)

- Line 5: "MIDI 1 input: ALSA sequencer ports on Linux, WinMM MIDI input devices on Windows (USB controllers, virtual/loopback ports)."
- Line 7: `id` is **opaque and transient** (ALSA `"client:port"`, Windows `"index:fingerprint"`), valid only until the device list changes. Persist `name`, never `id`.
- Line 8: `name` on Windows is the WinMM device name (≤ 31 characters); exact duplicates get `" #N"`.
- Lines 25-27: "…other than system pass-through ports. Blocks (sequencer client / WinMM enumeration): call it off the GTK thread."
- Lines 31-36: LOST is "the device was removed (USB unplug), its driver closed the input, or our subscription was dropped"; PORT is "a new input device or port became available".
- Lines 49-51: "seen at once from the hotplug notification (ALSA announce port / Windows PnP), else within about one to two seconds."
- Add the `radios_midi_decode_message` declaration and, if adopted, the error enum.

### 3.14 Windows pitfalls checklist

- [ ] No multimedia calls in `MidiInProc`. No `CM_Unregister_Notification` in the CM callback.
- [ ] The CM callback can fire before `CM_Register_Notification` returns: initialize first.
- [ ] Close order: `stopped = 1` → `SetEvent(stop)` → `g_thread_join` → `release_handle(TRUE)` → `CM_Unregister_Notification` (waits for running callbacks) → `CloseHandle`×3 → `DeleteCriticalSection` → free.
- [ ] `midiInReset` before unprepare/close (`MIDIERR_STILLPLAYING`). Ignore `MIM_CLOSE` caused by our own close (`closing`). Never call `midiInClose` on a handle after a driver `MIM_CLOSE`.
- [ ] Release the handle at LOST (exclusive drivers).
- [ ] `DRV_QUERYDEVICEINTERFACE*` take a **device ID cast to `HMIDIIN`**; sizes are in bytes. Compare paths case-insensitively.
- [ ] Use W APIs only. `wcsnlen(szPname, MAXPNAMELEN)`. `g_utf8_make_valid`.
- [ ] Function-pointer table initialized at run time, not as a static initializer of dllimport addresses.
- [ ] No identifier named `interface`. `windows.h` must come before `mmsystem.h`/`cfgmgr32.h`.
- [ ] Only `g_debug` for expected conditions. `g_warning` aborts tests under `G_DEBUG=fatal-warnings`.
- [ ] Rescans check `stopped` between devices, so `close()` on the GTK thread waits at most for one WinMM call.

---

## 4. Tests on Windows

### 4.1 `tests/test_midi.c`

- Guard `#include <alsa/asoundlib.h>`, `send()` and the ALSA `test_virtual_controller` with `#ifndef G_OS_WIN32`.
- `/midi/keys`: unchanged and portable. `"bad"` must return NULL before any WinMM call.
- **New `/midi/decode`** (both platforms) on `radios_midi_decode_message`:
  - `{90 24 7F}` → `note:1:36`, 1.0, pressed
  - `{90 24 00}` → 0, not pressed
  - `{80 24 40}` → 0, not pressed
  - `{B2 07 40}` → `cc:3:7`, 64/127, pressed
  - `{B2 07 3F}` → not pressed
  - `{EF 7F 7F}` → `pitch:16`, 1.0
  - `{E0 00 40}` → 8192/16383, pressed
  - `{E0 7F 3F}` → 8191/16383, not pressed
  - `{9F A4 FF}` → `note:16:36`, 1.0 (7-bit masking, like ALSA's `& 127`)
  - FALSE for `{C0 05}`, `{D0 09}`, `{A0 01 02}`, `{F8}`, `{FE}`, `{F0 7E 7F}`, a 2-byte note-on, and `{24 7F}` (data byte as status).
- **`/midi/virtual-controller` on Windows:** loopback only, never real hardware. Exclusive access means a test could steal a live studio controller from a running RadioS.
  - `RADIOS_TEST_MIDI_LOOPBACK_OUT` names a `midiOut` device. `RADIOS_TEST_MIDI_LOOPBACK_IN` names the corresponding `midiIn` name and defaults to OUT, since loopMIDI uses one name for both. If OUT is unset or not found: `g_test_skip("MIDI loopback yok (RADIOS_TEST_MIDI_LOOPBACK_OUT)")`.
  - Steps:
    - Find the input with `radios_midi_list_inputs` by `name` and assert it is listed.
    - Open it with `radios_midi_input_open` and assert `radios_midi_input_port(input) == id`.
    - `midiOutOpen(&out, index, 0, 0, CALLBACK_NULL)`, then `midiOutShortMsg` with `0x7F2490` (note on ch1 36 vel 127), `0x002480` (note off), `0x4007B2` (CC ch3 #7 = 64), `0x7F7FEF` (pitch ch16 max).
    - Expect exactly the Linux keys and values within 3 s.
    - Send `0x642890` and close immediately; assert nothing more arrives within 200 ms. Close `out`.
  - Usable providers: loopMIDI/teVirtualMIDI ports, or Windows MIDI Services app loopbacks (A→B), configured per CI machine.
- CMake: link `winmm` on WIN32 (for `midiOut*`). Optionally add `SKIP_RETURN_CODE 77`. With `/midi/keys` and `/midi/decode` always running, the binary exits 0 when only the loopback case is skipped.

### 4.2 Hotplug test with an injectable WinMM fake: `tests/test_midi_hotplug_winmm.c`

Same principle as Linux: the production `midi.c` is compiled into the test against fakes.

```c
#include "../src/core/midi_winmm_api.h"
/* fake functions … */
static const RadioSMidiWinApi fake_api = { fake_get_num_devs, fake_get_dev_caps, fake_open, fake_start, fake_stop, fake_reset,
    fake_close, fake_prepare, fake_unprepare, fake_add_buffer, fake_message, fake_get_id, fake_error_text, fake_register, fake_unregister };
#define RADIOS_MIDI_WIN_API() (&fake_api)
#include "../src/core/midi.c"
```

Fake model (all state under `GMutex fake_lock`; **invoke callbacks outside the lock**):
- **Devices:** `FakeDevice { address, port, name ("<name> MIDI 1", the same string as the Linux fake, so UI strings stay identical), path ("\\\\?\\USB#VID_FAKE&PID_%04X#loc%d#{6994ad04-93ef-11d0-a3cc-00a0c9223196}\\global"; the same address gives the same path), instance (new on every plug), present }`. The WinMM index is the position among present devices **in plug order**: removal shifts later indexes, like real WinMM.
- `fake_get_num_devs()`:
  - Sleeps `fake_open_delay_us` (slow-enumeration knob) and counts as one "open" (`fake_opens++`).
  - Tracks in-flight calls in `fake_busy_calls` so `open_clients()` sees them.
- `fake_get_dev_caps(id_or_handle)`:
  - If the value is a live fake handle pointer: a stale handle → `MMSYSERR_NODRIVER`; otherwise the device name.
  - Otherwise treat it as an index: present → name, else `MMSYSERR_BADDEVICEID`.
- `fake_message(DRV_QUERYDEVICEINTERFACESIZE/…)` for an index: size in bytes and the path. Optional knob `fake_no_interface` returns 0 (models non-PnP drivers).
- `fake_open(index, callback, instance, CALLBACK_FUNCTION)`:
  - Sleeps the delay and does `fake_opens++`.
  - Fails with `MMSYSERR_ERROR` if `fake_connect_fails`, with `MMSYSERR_ALLOCATED` if `fake_busy`, or if `fake_exclusive` (default TRUE) and another non-closed handle exists for the same instance.
  - Otherwise records a `FakeHandle { device, instance, callback, instance_data, started, stale, prepared/queued headers }`, calls `callback(MIM_OPEN)` and increments `fake_connects`.
- `fake_start/stop`. `fake_reset` returns queued headers via `MIM_LONGDATA` with `MHDR_DONE`.
- `fake_unprepare`: `MIDIERR_STILLPLAYING` if the header is still queued.
- `fake_close`: `MIDIERR_STILLPLAYING` if headers are still queued; the test asserts this never happens. Otherwise `callback(MIM_CLOSE)` and remove the handle.
- `fake_get_id`: a stale handle → `MMSYSERR_INVALHANDLE`.
- `fake_register`:
  - `fake_notices_refused` → `CR_FAILURE`.
  - Otherwise store `{ctx, cb}`. Callbacks are invoked under a separate `fake_cm_lock`, which `fake_unregister` also takes; this models "unregister waits for pending callbacks".
- Test drivers:
  - `device_plug(address, port, name)`: new instance, appended at the end, then an ARRIVAL notification with the path.
  - `device_unplug(address)`: remove (indexes shift), mark that instance's handles **stale**, optionally `callback(MIM_CLOSE)` if `fake_close_on_removal`, then a REMOVAL notification.
  - `device_send_cc(address, port, cc, value)`: `callback(MIM_DATA, 0xB0 | cc<<8 | value<<16)` for each started, non-stale handle of the present instance; returns the delivered count. This is the analogue of "the kernel routes only to live subscriptions".
  - `device_send_raw(address, msg)` and `device_send_sysex(address, bytes)` (fill the first queued header, `MHDR_DONE`, `MIM_LONGDATA`).
  - `open_clients()` = open handles + CM registrations + in-flight enumerate/open calls.
  - `fake_port_id(address, port)`: `radios_midi_list_inputs` and return the id of the entry whose name matches. Linux equivalent: `g_strdup_printf("%d:%d")`.
- `fake_reset()` asserts that no handles, registrations or in-flight calls remain (the Linux `fake_reset` asserts no open clients), then clears all knobs.

**Sharing the scenarios.** Recommended: move the harness and test bodies of `test_midi_hotplug.c` (289-526) into `tests/midi_hotplug_cases.h`. They then use only these fake hooks:
- `fake_reset`, `fake_set_delay`, `fake_set_connect_fails`, `fake_set_notices_refused`
- `fake_connect_count`, `fake_open_count`
- `device_plug`, `device_unplug`, `device_send_cc`
- `open_clients`, `fake_port_id`

Both `test_midi_hotplug.c` (ALSA fake) and `test_midi_hotplug_winmm.c` include it after their fake. This replaces direct use of `fake_lock`/`fake_connects`/`fake_opens` and the literal ids. If the Linux file must stay untouched, duplicate the bodies into the Windows file instead.

### 4.3 Case mapping and new Windows cases

| Case | Windows behaviour expected |
|---|---|
| `input-announcements` | Same assertions. Ids come from `fake_port_id`. LOST < 300 ms comes from the CM removal. Replug on the same socket → PORT #2, input still dead, LOST once, nothing routed. The other input on device 24 (open and close) causes no PORT or LOST. |
| `input-liveness-without-announcements` | `fake_set_notices_refused(TRUE)`. Unplug + immediate replug at the same address. LOST < 2.5 s via the stale-handle check. `ports == 0` (same path: no diff). |
| `ui-unplug-replug-refresh` | Unchanged, with identical strings. Reconnect < 1.2 s via the arrival burst. "Yenile" → `fake_connect_count() == before + 1`, and CC9 is delivered exactly once (old handle closed). |
| `ui-slow-sequencer` | The delay applies to enumeration and `midiInOpen`; `fake_connect_fails` → `MMSYSERR_ERROR` → still "takılı değil". `fake_open_count` rises by ≥ 3 in 7 s; worst gap < 100 ms. |
| `ui-teardown-pending-job` | Unchanged; `open_clients()` reaches 0 within 3 s. |
| **new** `input-driver-close` | `fake_close_on_removal = TRUE` with notifications refused → LOST < 300 ms from `MIM_CLOSE`. Close must not call `midiInClose` on the dead handle (the fake asserts it). |
| **new** `input-sysex-realtime` | A flood of `0xF8`/`0xFE`, plus a SysEx through `MIM_LONGDATA`, plus one CC → only the CC is delivered. Headers are re-queued; close leaves no queued or prepared headers (no `MIDIERR_STILLPLAYING`). |
| **new** `input-index-shift` | List (id for device B at index 1), unplug device A (B moves to index 0), open B's stale id → opens **B** (fingerprint fallback), never another device. With B unplugged instead → code 3, NULL. |
| **new** `input-duplicate-names` | Two devices named `"X MIDI 1"` with different paths → listed as `"X MIDI 1"` and `"X MIDI 1 #2"`, ordered by path. Replug in reverse order → same names per path. |
| **new** `input-polling-port` | Notifications refused; plug a new device → PORT within ~2 s from the periodic diff. |
| **new** `ui-replug-same-port` | Connected, unplug, replug at the **same** address while the lost input sits in `watch` → reconnect < 1.2 s. This fails if the watch kept its handle (`fake_exclusive`). |
| **new** `ui-busy-status` (if 3.12-1 is adopted) | `fake_busy = TRUE` → status `"… başka bir uygulama tarafından kullanılıyor. …"`; after `fake_busy = FALSE` the 5 s tick connects. |

Timing on Windows:
- The default timer granularity is ~15.6 ms, so `g_usleep(1000)` and `g_timeout_add(5)` run coarser. All thresholds (100 ms gap, 300 ms, 1.2 s, 2.5 s) leave room.
- Do not call `timeBeginPeriod` from the MIDI code.

### 4.4 CTest registration and runner

- **`input-*` cases need no display.** On WIN32 register them unconditionally under `BUILD_TESTING` (fast, no hardware). Register the `ui-*` cases under `RADIOS_GUI_TESTS`.
- The Linux runner `tests/run_rotation_editor_regressions.py` requires Xvfb and **returns 77 on Windows**, so every case would be silently skipped. The test-infra area must provide a Windows GUI runner:
  - run the exe directly, with `G_DEBUG=fatal-warnings`, `GTK_IM_MODULE=gtk-im-context-simple`, `NO_AT_BRIDGE` irrelevant;
  - optionally on a private desktop (`CreateDesktopW` + `STARTUPINFOW.lpDesktop`). The hotplug test never shows its window, so this is optional.
- Keep `TIMEOUT 45 SKIP_RETURN_CODE 77`.
- PATH must contain the GTK/GLib runtime DLLs (`C:\msys64\ucrt64\bin`, or the bundled runtime directory) through the CTest `ENVIRONMENT` property. This belongs to test infra.

### 4.5 Proving that no real device is touched (the analogue of "libasound not loaded")

- `objdump -p build/test_midi_hotplug.exe | findstr /I "midiIn CM_Register CM_Unregister"` must print **nothing**. Another module importing `winmm.dll` (e.g. the audio backend's `timeBeginPeriod`) is fine.
- All hotplug cases use the fake. `test_midi` touches real WinMM only for listing, plus the explicitly configured loopback.

### 4.6 Manual hardware acceptance (cannot be automated)

1. Class-compliant USB controller (e.g. KORG nanoKONTROL2, Akai) on the **classic** stack:
   - The Settings › MIDI list shows the WinMM name.
   - Select it → "Bağlı: …"; CC, note, pitch bend and actions work.
   - Unplug → "takılı değil" < 0.5 s.
   - Replug into the same socket and into another socket → reconnects without waiting for the 5 s tick.
   - "Yenile" reconnects.
   - Open the device first in another application → open fails (busy status if 3.12-1 is adopted); close the other application → connects within 5 s.
   - Sleep/resume.
2. Vendor driver (e.g. the KORG USB-MIDI driver): names and removal behaviour (whether `MIM_CLOSE` arrives; stale-handle query results).
3. Two identical controllers: names (`"2- …"` or `" #2"`), and stability after replug in reverse order.
4. loopMIDI virtual port fed by a software controller.
5. Windows 11 with Windows MIDI Services enabled: naming mode, multi-client, removal, first-call latency, a renamed (custom, non-ASCII) port name.
6. A Linux-made `midi_mapping` imported through a transfer package: the old name is shown as "Bağlı değil · …"; re-select; bindings still work.

---

## 5. CMake changes (exact)

```cmake
# line 19: was pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa>=1.2)
if(NOT WIN32)
    pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa>=1.2)
endif()

# lines 93-94
target_sources(radios_core PRIVATE src/core/midi.c)      # includes midi_alsa.c or midi_winmm.c
if(WIN32)
    target_link_libraries(radios_core PRIVATE winmm cfgmgr32)
else()
    target_link_libraries(radios_core PRIVATE PkgConfig::ALSA)
endif()

# lines 191-208
    add_executable(test_midi tests/test_midi.c)
    if(WIN32)
        target_link_libraries(test_midi PRIVATE radios_core winmm radios_warnings m)
    else()
        target_link_libraries(test_midi PRIVATE radios_core PkgConfig::ALSA radios_warnings m)
    endif()
    add_test(NAME midi COMMAND test_midi)
    set_tests_properties(midi PROPERTIES SKIP_RETURN_CODE 77)
    if(WIN32)
        add_executable(test_midi_hotplug tests/test_midi_hotplug_winmm.c)
        target_link_libraries(test_midi_hotplug PRIVATE radios_ui radios_warnings)   # no --as-needed (ELF-only)
        foreach(midi_case input-announcements input-liveness-without-announcements input-driver-close
                input-sysex-realtime input-index-shift input-duplicate-names input-polling-port)
            add_test(NAME native_midi_hotplug_${midi_case} COMMAND test_midi_hotplug -p /midi-hotplug/${midi_case})
            set_tests_properties(native_midi_hotplug_${midi_case} PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77)
        endforeach()
        if(RADIOS_GUI_TESTS)
            foreach(midi_case ui-unplug-replug-refresh ui-slow-sequencer ui-teardown-pending-job ui-replug-same-port ui-busy-status)
                add_test(NAME native_midi_hotplug_${midi_case} COMMAND ${Python3_EXECUTABLE}
                    ${CMAKE_SOURCE_DIR}/tests/<windows-gui-runner>.py $<TARGET_FILE:test_midi_hotplug> -p /midi-hotplug/${midi_case})
                set_tests_properties(native_midi_hotplug_${midi_case} PROPERTIES TIMEOUT 45 SKIP_RETURN_CODE 77)
            endforeach()
        endif()
    elseif(RADIOS_GUI_TESTS)
        # existing Linux block (lines 195-207) unchanged
    endif()
```

The global `-D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00` comes from the build/toolchain area. `winmm.dll` and `cfgmgr32.dll` are system DLLs; packaging bundles nothing for MIDI.

---

## 6. Internal contracts to preserve, and where they are tested

| # | Contract | Exercised by |
|---|---|---|
| C1 | Key format, validation, labels; must match SDBASE `_MIDI_CONTROL` (`desktop_api.py:44`) | `/midi/keys`; SDBASE desktop API tests |
| C2 | Event semantics (channel 1..16, number, value 0..1, `pressed` thresholds) identical across backends | `/midi/virtual-controller` (Linux / Windows loopback), **new** `/midi/decode` |
| C3 | Listing blocks, so it runs off the GTK thread; `name` is the persistent identity, `id` is transient and opaque; no system pass-through ports | `input-announcements` (1 port, exact name), `/midi/virtual-controller`, `ui-*` (page build < 100 ms) |
| C4 | `open*`: bad id or NULL callback → NULL (code 2) without OS calls; callable from any thread; delivery on the given or thread-default context; returned input is alive | `/midi/keys`, `ui-*` (open in GTask), `input-*` |
| C5 | Delivery FIFO through idle sources at `G_PRIORITY_HIGH_IDLE`, never inline on the reader thread; **no callback after close returns** | `/midi/virtual-controller` (note sent just before close is never delivered), `ui-teardown-pending-job` |
| C6 | LOST exactly once; the input stays open and keeps reporting PORT; a new device → one PORT; own inputs and listings → no PORT; a device returning at the same address does not revive the input | `input-announcements`, `input-liveness-*`, `input-polling-port` |
| C7 | `alive()` FALSE at once on notification, else within ~1–2 s | `input-announcements` (< 300 ms), `input-liveness-*` (< 2.5 s), `input-driver-close` |
| C8 | `close()` releases every OS resource before returning (handles, registrations); safe on partial inputs; NULL-safe | `open_clients()==0` after close; `ui_close`; `fake_reset` assertions |
| C9 | `port()` returns the id given to open | `/midi/virtual-controller`, `input-announcements` |
| C10 | Service: one job at a time, watch semantics, 5 s retry, "Yenile" makes exactly one new connection and the old one stops delivering, exact Turkish status strings, page build < 100 ms, main-loop gap < 100 ms | `ui-*` |
| C11 | `midi_mapping` schema; device stored by display name (≤ 192); maps ≤ 32; names ≤ 80; bindings ≤ 512 | SDBASE `test_desktop_*.py`, `test_reset_ui`, `test_transfer_ui` (platform-neutral) |
| C12 (Windows) | On LOST the device handle is released at once (exclusive drivers) while hotplug watching continues | `ui-replug-same-port`, `input-announcements` (nothing routed after LOST) |

---

## 7. Cross-area dependencies

- **Build/toolchain:**
  - Drop the ALSA `pkg_check_modules` on WIN32 (configure fails otherwise).
  - Link `winmm` and `cfgmgr32`.
  - Global `_WIN32_WINNT/WINVER=0x0A00`.
  - `radios_warnings` (`-Wall -Wextra -Wpedantic -Werror`) must keep the MinGW system headers as `-isystem` (the default for `/ucrt64/include`).
- **Test infrastructure:**
  - A Windows replacement for `tests/run_rotation_editor_regressions.py` (it uses Xvfb and `GDK_BACKEND=x11` and returns 77 on Windows).
  - PATH/ENVIRONMENT for GTK DLLs in CTest.
  - Confirm that the MSYS2 GLib `g_test_run` returns 77 when every case is skipped.
- **Audio/preferences area:** `src/ui/preferences.c:330-359` (`/proc/asound` sample-rate probe) is ALSA-only and needs a WASAPI equivalent. Not MIDI, flagged here.
- **SDBASE:** no change (the validator is platform-neutral). Transfer packages from Linux carry ALSA-style device names; this is documented operator behaviour.
- **Docs/l10n:**
  - New Windows operator notes: exclusive access on classic drivers, re-selecting the device after migration or MIDI Services renaming, duplicate naming.
  - Update the `midi.h` comments.
  - New Turkish error strings (3.10, 3.12).
- **Platform layer (`platform.h`):** nothing required. MIDI uses `g_win32_error_message` and `g_utf16_to_utf8` from GLib and keeps its own CM registration behind the injectable table, so tests can fake it. Optional: if the platform team centralizes PnP watching, MIDI can consume `radios_platform_device_watch_new/free` through its API table.

## 8. Risks / open questions

- **R1** Behaviour on surprise removal of an **open** `HMIDIIN` is undocumented. Whether `MIM_CLOSE` arrives, and whether `midiInGetDevCapsW(handle)`/`midiInGetID` fail on a stale handle, differ by driver (classic usbaudio, vendor drivers, MIDI Services). The design does not depend on either (PnP removal is primary), but the "no notifications + fast replug" case needs hardware verification.
- **R2** WinMM's device list lags PnP arrival by an unknown time. The 3 s burst of 250 ms rescans plus the 1 s periodic diff cover it; verify on hardware that the reconnect is < 1.2 s.
- **R3** Exclusive access on the classic stack: a device held by another application cannot be opened. Without the 3.12-1 change the operator sees the misleading "takılı değil".
- **R4** Names: 31-character truncation. Classic duplicates get `"2- "` in **plug order**, so two identical controllers can swap between sessions. MIDI Services naming modes and custom names change names. Linux-made configs never match Windows names. In all cases the operator re-selects once; no data loss.
- **R5** `close()` runs on the GTK thread and joins the worker. If the worker is inside a slow WinMM enumeration (first MIDI Services call, USB storm), the GTK thread can stall for that call's duration. Mitigation: rescans check `stopped` between devices. Fallback if it shows up in testing: release the handle and unregister in `close()`, and let a detached worker free the struct.
- **R6** Windows MIDI Services runs WinMM through a service. Unknown latency or COM quirks in GTask threads; fallback is `CoInitializeEx` in the worker.
- **R7** `CM_NOTIFY_FILTER_FLAG_ALL_INTERFACE_CLASSES` delivers many unrelated events (cheap, coalesced). If a deployment shows CPU churn, restrict to `KSCATEGORY_AUDIO` plus the MIDI Services endpoint interface class after verification.
- **R8** Windows test timing granularity (~15.6 ms) and `G_DEBUG=fatal-warnings`: invalid UTF-8 or any `g_warning` aborts. Sanitize names; log expected failures with `g_debug`.
- **R9** The fake cannot reproduce every driver quirk; the hardware checklist in 4.6 is mandatory before release.
- **R10** If the Linux hotplug test is refactored to share scenarios (4.2), it must keep its Linux assertions bit-for-bit. Otherwise duplicate the bodies for Windows.

## 9. Implementation order

1. Split `midi.c` into common + `midi_alsa.c` (no behaviour change). Add `radios_midi_decode_message` and `/midi/decode`. Run the Linux tests: `/midi/keys`, `/midi/decode`, `native_midi_hotplug_*` 5/5.
2. Add `midi_winmm_api.h` and `midi_winmm.c` (3.2–3.10). Make the CMake changes (5).
3. Write the Windows fake test (4.2) and pass all `input-*` cases, then the `ui-*` cases with the Windows runner.
4. Optional UX: busy status (3.12-1) and the `*.JSON` filter. Update header comments and docs.
5. Hardware acceptance (4.6); record the results under R1/R2.
