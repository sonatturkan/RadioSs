# Area spec: docs-history (RadioS Windows port)

Analyzed tree: `C:\Users\sonatturkan\Desktop\RadioS` (read-only, 2026-10-19). Binding inputs honoured:
`specs/DECISIONS.md` (D1 Windows only / Linux wording becomes Windows wording, D2b no useless leftovers +
"report, do not delete outside ownership", D4 layout/launchers/Python invocation, D5 fonts, D6 newline/ACL/
lock/stop-event rules, D7 toolchain) and `specs/PLATFORM_FACTS.md`. Where this spec touches files owned by
other areas (tools/README.md, config/README.md, packaging/README.md, tools/*.py, tests/*) it only lists the
doc-history items; the Windows *content* for those files comes from `specs/python-tools-launcher.md`
(§4.17-4.19), `specs/tests-c.md`, `specs/audio-*.md`, `specs/gtk-windows-runtime.md`.

The tree is being edited concurrently (example: `src/audio/pulse_backend.c` disappeared during this
analysis and `src/audio/wasapi_backend.c` / `wasapi_device.c` appeared). Every file:line below was taken
on 2026-10-19 ~02:00; re-run the read-only scripts of Appendix C before applying the manifest.

---------------------------------------------------------------------------------------------------------

## 1. Key findings

1. `docs/` holds **442 files / ~17.3 MB**; **23 files (~1.93 MB, of which 1.74 MB are the three generated
   JSON files) are current documentation**, 6 more contain extractable contracts, the remaining
   **413 files (~15.2 MB)** are development history: prompt-N evidence trees (screenshots, JUnit XML,
   JSON receipts, `.before/.after/.patch` source backups, copies of `broadcast.c`/`automation.c`/
   `CMakeLists.txt`), dated agent work logs (`kilavuz-2026-10-18-*`), audits, todo/done archives,
   prompt requirement records.
2. Packaging exposure: `tools/radios_package.py:36` ships every `docs/**/*.md|*.json` in both packages
   (`COMMON_TREES`), i.e. today **303 files / 6.6 MB**, of which **280 files / 4.7 MB are history**
   (agent logs with `/home/sturkan/...` paths, `flatpak-spawn` commands, evidence JSON). The planned Windows
   portable/NSIS layout ships `docs\**` and `design\**` too (python-tools-launcher §4.17.2).
3. Nothing in code *reads* a history doc. Functional dependencies on `docs/`/`design/` are exactly:
   CMake + `tools/css_to_gtk.py` (design CSS/HTML → embedded GTK CSS), `tools/islev_envanteri.py`
   (design + SDBASE → `docs/islev-envanteri.json`, `docs/sdbase-openapi.json`), `tools/radios_package.py`
   (packages `docs`, `design`, `database/SDBASE/docs`, requires `database/SDBASE/IMPLEMENTATION_STATUS.md`),
   `tests/test_packaging.py` (needs `design/reference/tasarrım/index.html` and a `docs` tree), the
   "protected directory" lists (`src/core/storage_locations.c:86-88`, `tools/radios_database.py:100-101`,
   `database/SDBASE/src/sdbase/desktop_transfer_package.py:82-83`) and `docs/api.md` named by
   `database/SDBASE/src/sdbase/openapi.py:252` (comment) and `:298`, `:350` (OpenAPI description strings
   served by `GET /v1/openapi.json`). Everything else is a comment (9 path pointers, §4.2).
4. `docs/islev-envanteri.json` is **already stale** (generated 2026-10-06): its `source_hashes` list 72
   design files (73 exist; `index.html` and `playlist-workspace.css` changed) and 102 SDBASE modules
   (117 exist; 14 changed). Keep it, but regenerate after the port (§8 step 7).
5. Verified on the target machine (§5): MSYS2 CMake 4.4.4 + Ninja 1.13.2 track the non-ASCII
   `design/reference/tasarrım` path correctly (no rename needed); MSYS2 Python 3.14.8 defaults to
   **cp1254** and `tools/css_to_gtk.py` (`read_text()` without encoding) **fails** on `index.html`;
   `tools/islev_envanteri.py --check` cannot run on Windows until SDBASE stops importing `fcntl`
   (`storage.py:17`); Ninja fails with `mkdir ... No such file or directory` when the build path exceeds
   ~250 characters (keep build/root paths short).
6. Surviving docs/READMEs contain ~250 Linux statements (Pulse/PipeWire, pactl/parec, X11/Wayland/Xvfb,
   Flatpak/Freedesktop SDK, `.sh` launchers, `python3`, `/veri`, `/tmp`, `/home`, 0600/0700, fsync/rename,
   pidfd/`/proc`, SIGTERM/SIGKILL, ELF/GLIBC/tar.gz) and version-by-version acceptance narration; all are
   listed in §6 with the Windows wording of §6.0.
7. Already-broken links today: `docs/cevrimici-muzik.md` (deleted with the online-music removal) is linked
   from `README.md:82,146`, `config/README.md:13`, `tests/README.md:157`, `docs/ses-yonlendirme.md:25`,
   `docs/14-nihai-kabul-ve-paketleme.md:5`; `docs/08-ses-motoru.md:18` and
   `docs/10-playlist-ve-otomasyon.md:209` name the removed `src/audio/pulse_backend.c`;
   `docs/ses-yonlendirme.md:63` names non-existent `include/radios/audio_routing.h`, `src/audio/routing.c`;
   two SDBASE docs link PDFs outside the project (`adapter_coverage.md:19`, `radioboss_m3u8.md:3`).

---------------------------------------------------------------------------------------------------------

## 2. Classification rules

* **KEEP** – current user/developer documentation of a feature, data/API contract, or a generated
  inventory that a tool writes/checks or a surviving README links. Content still needs the §6 edits.
* **KEEP→RENAME** – KEEP, but the file name is a development-stage artefact (`NN-` stage prefix, date
  suffix, `kilavuz-…` agent-stream folder). The new name is topic-only.
* **EXTRACT→DROP** – mostly history, but specific line ranges are a contract/behaviour that code or
  surviving docs rely on. Copy those lines into the named target first (§3.3), then delete the source.
* **DROP** – development history: dated work logs, agent/akım reports, evidence/screenshots/logs/backups,
  prompt-N records, audits/closure reports, todo/done archives, test reports, stage plans, stored tool
  output that a tool regenerates.

Line numbers are 1-based and refer to the files as they are on 2026-10-19.

---------------------------------------------------------------------------------------------------------

## 3. Manifest

### 3.1 `docs/` top-level files (61 = 58 `.md` + 3 `.json`)

| # | Path | Action | New path / target | One-line reason |
|---|---|---|---|---|
| 1 | docs/01-islev-haritasi.md | EXTRACT→DROP | → docs/mimari.md (§3.3-A) | Stage-1 plan dated 2026-10-06 with "proposed" C modules; its boundary, component, data-contract and error tables are still the architecture. |
| 2 | docs/02-proje-duzeni.md | DROP | – | Stage-2 import record, Linux root `/home/sturkan/...`; layout lives in README, data paths in data/README.md + config/README.md. |
| 3 | docs/03-c-uygulama-altyapisi.md | KEEP→RENAME | docs/c-uygulama-altyapisi.md | Current C core contracts (queue sizes, shutdown order, root discovery, INI rules, `--check-config`). |
| 4 | docs/04-native-tasarim.md | KEEP→RENAME | docs/native-tasarim.md | CSS→GTK pipeline, native component map, `ui-preferences.ini` contract. |
| 5 | docs/05-veritabani-kurulumu.md | KEEP→RENAME | docs/veri-servisi.md | Data-service commands, files, identities, re-init/interrupted-setup contract (rewritten for Windows). |
| 6 | docs/06-c-veri-baglantisi.md | KEEP→RENAME | docs/c-veri-baglantisi.md | C data-client contract (loopback only, 2 MiB, error classes, CAS, receipts, private user keys). |
| 7 | docs/07-medya-kutuphanesi.md | KEEP→RENAME | docs/medya-kutuphanesi.md | Media import/analysis/API contract, library-analysis settings. |
| 8 | docs/08-ses-motoru.md | KEEP→RENAME | docs/ses-motoru.md | Audio engine usage/contract (Pulse text → WASAPI). |
| 9 | docs/09-dj-ve-gecisler.md | KEEP→RENAME | docs/dj-ve-gecisler.md | DJ mixer/effects/transition and `dj_mixer` document contract. |
| 10 | docs/10-playlist-ve-otomasyon.md | KEEP→RENAME | docs/playlist-ve-otomasyon.md | Playlist/programme/automation/AutoDJ/schema usage and contracts. |
| 11 | docs/11-reklam-yonetimi.md | KEEP→RENAME | docs/reklam-yonetimi.md | Advert workflow, data and evidence-class contract. |
| 12 | docs/12-yayin-kayit-acil.md | KEEP→RENAME | docs/yayin-kayit-acil.md | WAV/MP3/Icecast/metadata/emergency takeover contract. |
| 13 | docs/13-yonetim-ve-guvenilirlik.md | KEEP→RENAME | docs/yonetim-ve-guvenilirlik.md | Users/permissions, durable queue, audio loss, backup/restore commands. |
| 14 | docs/14-nihai-kabul-ve-paketleme.md | EXTRACT→DROP | → docs/fiziksel-kabul.md, tests/README.md (§3.3-E) | Says itself "tarihsel aşama kabulü"; Linux tar.gz/ELF/Freedesktop-SDK packaging. |
| 15 | docs/acil-reklam-teknik-islevleri.md | DROP | – | Stage-1 function map; "proposed" modules (`acil.c`, `reklam.c`…) never existed; line refs stale. |
| 16 | docs/audio-manual-20261010.md | DROP | – | Dated manual audit log (build evidence, removed YT/Apple fixtures). |
| 17 | docs/codex-yarim-isler-2026-10-18.md | DROP | – | Codex "unfinished chats" agent notes. |
| 18 | docs/gelistirme-asamalari.md | DROP | – | 14-stage development progress table. |
| 19 | docs/gui-islev-kapanis-auditi.md | DROP | – | Closure audit report. |
| 20 | docs/islev-envanteri.json | KEEP | – | Written/checked by `tools/islev_envanteri.py:204,209,214`; README link. Stale → regenerate (§8). |
| 21 | docs/kilavuz-2026-10-18-4c.md | EXTRACT→DROP | → docs/playlist-ve-otomasyon.md (§3.3-F) | Akım-B traceability/work log; only the "üst çubuk ▶ + küme" behaviour (L97-99) survives. |
| 22 | docs/kilavuz-2026-10-18-A.md | DROP | – | Akım-A work log. |
| 23 | docs/kilavuz-2026-10-18-C.md | DROP | – | Akım-C traceability log. |
| 24 | docs/kilavuz-2026-10-18-C-C1.md | DROP | – | Agent C1 report (mixer sources/PTT/seek investigation). |
| 25 | docs/kilavuz-2026-10-18-C-C10.md | DROP | – | Agent C10 report; its "Sözleşme" block only repeats prototypes already in `include/radios/playout_ui.h`/`header_navigation.h`. |
| 26 | docs/kilavuz-2026-10-18-C-C11.md | DROP | – | Agent C11 report (srt/UDP/MIDI fixes on Linux SDK). |
| 27 | docs/kilavuz-2026-10-18-C-C2.md | DROP | – | Agent C2 report. |
| 28 | docs/kilavuz-2026-10-18-C-C3.md | DROP | – | Agent C3 report (X11/Wayland/GNOME-Shell test rig); the rationale is already in `src/ui/content_scale.c:219-234`. |
| 29 | docs/kilavuz-2026-10-18-C-C4.md | DROP | – | Agent C4 report (Pulse flush, 7/24 run). |
| 30 | docs/kilavuz-2026-10-18-C-C5.md | DROP | – | Agent C5 report. |
| 31 | docs/kilavuz-2026-10-18-C-C6.md | DROP | – | Agent C6 report. |
| 32 | docs/kilavuz-2026-10-18-C-C7.md | DROP | – | Agent C7 report. |
| 33 | docs/kilavuz-2026-10-18-C-C8.md | EXTRACT→DROP | → docs/gecis-dagilimi-sozlesmesi.md (§3.3-C) | Agent C8 report; §3-4 (L33-58) is the `settings.transitions` contract that code comments cite as "kılavuz 18.10 contract … rule g". |
| 34 | docs/kilavuz-2026-10-18-C-C9.md | DROP | – | Agent C9 report. |
| 35 | docs/kilavuz-2026-10-18-D-R10.1.md | DROP | – | Stored output of `tools/gui_backend_coverage.py` (regenerable; enforced by ctest `gui_backend_coverage`). |
| 36 | docs/kilavuz-2026-10-18-D.md | EXTRACT→DROP | → docs/aktarim-paketi.md (§3.3-D) | Akım-D log; `.st` package/back-end/widget contract (L34-67 + L94,100,101,115) is cited by `src/ui/transfer_ui.c:9`. |
| 37 | docs/kilavuz-2026-10-18-matris.md | DROP | – | PDF requirement matrix (traceability). |
| 38 | docs/medya-arsivi.md | KEEP | – | Settings › Media archive + `storage.ini` contract. |
| 39 | docs/medya-surukle-birak.md | KEEP | – | Drag-and-drop usage/limits (SDK/service history removed). |
| 40 | docs/otomasyon-sozluk-ve-kurallar-2026-10-15.md | KEEP→RENAME | docs/otomasyon-sozluk-ve-kurallar.md | Glossary, selection/transition priority rules, acceptance criteria (rules). |
| 41 | docs/otomasyon-test-raporu-2026-10-15.md | DROP | – | Dated test report. |
| 42 | docs/pdf4-architecture.md | DROP | – | Prompt/PDF-4 record. |
| 43 | docs/pdf4-requirements.md | DROP | – | Prompt/PDF-4 requirement record. |
| 44 | docs/playlist-otomasyon-islevleri.md | DROP | – | Stage-1 function map. |
| 45 | docs/program-onayi-ve-haklar.md | KEEP | – | Programme approval / usage-rights workflow. |
| 46 | docs/proje-kapanis-denetimi.md | DROP | – | 0.17.0 closure audit. |
| 47 | docs/prompt-8-uygulama.md | DROP | – | Prompt-8 record. |
| 48 | docs/prompt10-requirements.md | DROP | – | Prompt-10 record. |
| 49 | docs/prompt11-requirements.md | DROP | – | Prompt-11 record (links into `build/`). |
| 50 | docs/prompt5-implementation.md | DROP | – | Prompt-5 record. |
| 51 | docs/prompt5-requirements.md | DROP | – | Prompt-5 record. |
| 52 | docs/rekordbox-ice-aktarim.md | KEEP | – | rekordbox XML import usage/limits. |
| 53 | docs/rekordbox-usb-tracklist.md | KEEP | – | rekordbox USB/SD `export.pdb` usage/limits; linked from SDBASE `docs/rekordbox.md:9`. |
| 54 | docs/sdbase-openapi.json | KEEP | – | Written/checked by `tools/islev_envanteri.py:205`; README link. Regenerate (§8). |
| 55 | docs/ses-yonlendirme.md | KEEP | – | Audio-routing tab usage/contract (large Windows rewrite, §6.3). |
| 56 | docs/sessizlik-korumasi.md | KEEP | – | Silence (dead-air) guard behaviour. |
| 57 | docs/source-import-manifest.json | KEEP | – | Provenance (origin→destination+SHA) of the imported design and SDBASE copies (licence/provenance trail); linked by design/README.md:7, database/README.md:3; no code reads it. Optional later DROP = delete those two sentences. |
| 58 | docs/yapilacaklar-2026-10-12.md | DROP | – | Todo list. |
| 59 | docs/yapilanlar-arsivi-2026-10-12-17.md | DROP | – | Done archive. |
| 60 | docs/yayin-akisi-724-2026-10-17.md | EXTRACT→DROP | → tools/yayin_akisi_724/README.md (§3.3-G) | Dated record of a data operation on the Linux station DB; the day-part/rule description (L25-39) documents what the tool builds. |
| 61 | docs/yayin-dj-islevleri.md | DROP | – | Stage-1 function map. |

Totals: KEEP 10, KEEP→RENAME 12 (+1 from a subfolder), EXTRACT→DROP 6, DROP 33.

### 3.2 `docs/` subdirectories (all removed after the one move)

| Path | Files / bytes | Action | Reason |
|---|---|---|---|
| docs/effector-display/ | 1 / 7.9 KB | DROP | Visual-review record (2026-10-10). |
| docs/kilavuz-2026-10-18-4c/sozlesme.md | 1 / 8.0 KB | KEEP→RENAME → docs/sema-akisi-sozlesmesi.md | Schema/programme/cluster data contract; cited by `tests/test_live_step_wait_ui.c:41`, `tests/test_live_row_ui.c:53` and by "contract §1b/§2/§5" comments (§4.2). |
| docs/kilavuz-2026-10-18-4c/durum.md | 3.5 KB | DROP | Agent status log (`flock durum.lock`). |
| docs/kilavuz-2026-10-18-4c/gereksinimler.md | 3.3 KB | DROP | Requirement matrix of an agent stream. |
| docs/kilavuz-2026-10-18-4c/kume.md | 15.9 KB | DROP | Agent report; scenarios K1-K6/T1-T4/L1-L6 are coded in `tools/otomasyon_testleri/senaryolar_kume.py:183+`; fix the two pointers (§4.2). |
| docs/player-graphic/ | 5 / 335 KB | DROP | Audit records + `broadcast.c.before` + patch. |
| docs/prompt-2/ | 81 / 2.52 MB | DROP | Prompt-2 evidence (26 PNG, logs, `log_event.py`). |
| docs/prompt-6/ | 19 / 218 KB | DROP | Prompt-6 coordination/history. |
| docs/prompt-7/ | 121 / 3.65 MB | DROP | Prompt-7 evidence, source backups (`*.before/*.after/*.patch`), scripts. |
| docs/prompt-9/ | 33 / 2.65 MB | DROP | Prompt-9 source backups + validation screenshots. |
| docs/prompt-12/ | 56 / 2.66 MB | DROP | Prompt-12 audit/review/screenshots. |
| docs/prompt-13/ | 45 / 2.04 MB | DROP | Prompt-13 baselines (copies of `automation.c`, `CMakeLists.txt`, `index.html`), logs, renders. |
| docs/prompt-14/ | 16 / 363 KB | DROP | Prompt-14 (removed online-service login UI) records. |

### 3.3 New documents created by extraction (create BEFORE deleting the sources)

All new/edited docs: UTF-8 without BOM, LF line endings (D6), Turkish.

**A. `docs/mimari.md`** — "RadioS mimarisi ve ortak sözleşmeler". Source `docs/01-islev-haritasi.md`:
L5-10 and L12 (boundaries; drop L11 = stage-1/2 import sentence), L38-48 (component table; drop L36
"önerilen bileşenlerdir"), L50-62 (mermaid flow), L64 (threading/queue rule), L66-85 (identity/version/
command contract table + paragraph), L87-100 (error/connection behaviour table + source pointer). Edits:
present tense; L7 "geliştirilecektir" → "geliştirilir"; L10 keep; add one sentence: "Windows'ta veri
servisi paketli `runtime\python\python.exe` ile çalışır; C uygulaması `RadioS.exe`'dir." Links in L100
(`sdbase-openapi.json`, `islev-envanteri.json`) stay valid (same folder).

**B. `docs/sema-akisi-sozlesmesi.md`** — move of `docs/kilavuz-2026-10-18-4c/sozlesme.md` (whole file).
Keep the section numbers §1, §1b, §2, §3, §4, §5, §6 unchanged (code comments cite "contract §1b", "§2",
"§5"). Edits: L1 → `# Şema / program / küme / planlayıcı veri sözleşmesi`; replace L3 (PDF path +
agent streams 1f/67/78) with: "Kapsam: rotasyon şeması, live event, program bloğu, planlayıcı ipuçları ve
mixer kanal alanları. Kaynak gereksinim: `radyo_otomasyonu_kilavuzu.pdf` (§02-§04, §08)."; §5 heading
"(67 akımının sözleşmesi, okunur)" → "(okunan alanlar)"; §6 heading "Sonradan eklenenler (18.10.2026,
Codex ve akım B)" → "Ek alanlar".

**C. `docs/gecis-dagilimi-sozlesmesi.md`** — "Geçiş dağılımı (`settings.transitions`) sözleşmesi".
Source `docs/kilavuz-2026-10-18-C-C8.md` L35-44 (lettered rules (a)-(h) — keep the letters, code says
"rule g": `include/radios/transition_distribution.h:10`, `include/radios/autodj_automation_ui.h:37`,
`src/ui/autodj_automation_ui.c:1204`, `src/ui/rotation_editor.c:406,2309`,
`tests/test_rotation_editor_regressions.c:1200`), L45 rewritten without agent names/test counts
("Arka uç `desktop_automation_calendar.py` aynı kabul/ret kurallarını uygular; planlayıcı
(`automation_calendar_plan.c`) kind'siz girdileri `transition_selection_mode` ile, kind'li girdileri medya
çifti özetinden deterministik seçer; öncelik aşağıdaki tablodadır."), L49-58 (priority table; delete the
"Kullanıcı kararı önerisi…" sentence in L52). Heading L33 "(f6 / d4 kabulü, değişiklikleriyle — aynen)"
dropped.

**D. `docs/aktarim-paketi.md`** — "Ayar ve medya aktarım paketi (`.st`)". Source
`docs/kilavuz-2026-10-18-D.md` L34-43 (package layout), L45-59 (back-end operations table + rules),
L61-67 (widget names), plus these facts rewritten as bullets: L94 (groups `local_settings`
[`ui-preferences.ini`, `audio-panel.ini`, `media-archive.ini`, `library-analysis.ini`, `storage.ini`],
`view_profiles`, `import_profiles`; provenance fields `source_root`, `source_path`,
`source_relative_path`; package path `medya/<arşiv altındaki yol>` else `<Albüm>/<dosya>`), L100 (backup
and `.st` export/import check free space first; targets on the same disk are summed; ≥ 512 MB must
remain; Turkish refusal), L101 (single-file provenance → `medya/<Albüm>/<dosya>`), L115 ("Reklam
kampanyaları" group: sales fields, state, spot SHA-256; import updates an open campaign of the same name
or creates a draft; approval is never carried, a warning is shown; spot re-bound by SHA; per-record
selection). Add a "Windows" subsection (contract for the SDBASE/transfer owner, see §10): ZIP entry names
always `/`; on import every path component is also cleaned of Windows-invalid names (`< > : " | ? *`,
control chars, trailing dot/space, `CON PRN AUX NUL COM1-9 LPT1-9`, case-insensitive collisions get
` (2)`); long paths require the `longPathAware` manifest/`\\?\` handling; `os.replace` of the finished
`.st` retries on sharing violations (antivirus/indexer); the destination rule "mutlak yol" means a drive
or UNC path; JSON stays UTF-8, `ensure_ascii=False`, `sort_keys=True`, `\n`.

**E. From `docs/14-nihai-kabul-ve-paketleme.md`:**
* `docs/fiziksel-kabul.md` — "Fiziksel ses ve saha kabulü": L75-77 (command → Windows form
  `runtime\python\python.exe -E -s -B -X utf8 tools\radios_hardware_check.py --output build\hardware.json`,
  enumeration now WASAPI/WinMM per tools spec), L79 without "Üç HDMI portu kullanılamıyor olarak
  raporlanır" (machine-specific), L81-86 (operator checklist), L71 (limits sentence), L88 reworded to
  "Bu maddeler her kurulumda operatör tarafından doğrulanır."
* `tests/README.md` (soak section, owner tests area): L63 (Pulse clock → virtual audio clock) and L65-67
  (command → Windows form). L69 (measured numbers) is history → not copied.

**F. Into `docs/playlist-ve-otomasyon.md`** (new subsection "Üst çubuk ▶ ve kümeye atanmış program"):
behaviour from `docs/kilavuz-2026-10-18-4c.md:99` sentences 2-3 and `docs/kilavuz-2026-10-18-D.md:134`
(rule only): "Sürekli kipte üst çubuktaki ▶ programı genel sırada başlatır; liste taşımayı izler. Genel
sıra, programın atandığı tek kümenin oynatıcılarında çalar: müzik kümede; rotasyonda reklam/jingle/sweeper
için kümesiz yardımcı oynatıcı seçildiyse o türler orada. Aynı program genel sırada ve kümenin kendi
kapsam akışında aynı anda çalmaz; ikinci başlatma sahiplik iletisiyle reddedilir."

**G. Into `tools/yayin_akisi_724/README.md`** (owner tools area; section "Üretilen yayın akışı biçimi"):
`docs/yayin-akisi-724-2026-10-17.md` L25-32 (day-part table, keep "UTC, istasyon saat dilimi UTC" note)
and L34-39 (rotation rules). Not L3-4, L6-23 (user-library statistics), L41-60 (DB revisions, dated
validation).

**H. Recommended new `docs/windows-platformu.md`** (developer reference, written by the integrator from
DECISIONS D3-D7 + PLATFORM_FACTS): root discovery, `runtime\` prefix, Python invocation
(`-E -s -B -X utf8`, never PATH python), private files (ACL definition), locks (`LockFileEx` at
0x4000000000000000), graceful stop (`Local\RadioS-stop-<pid>`), IANA time zones via ICU, fonts
(`InterVariable.ttf` / "Inter Variable Text"), WASAPI/WinMM, NTFS requirement, long paths,
Defender/Controlled Folder Access/OneDrive notes. It replaces the Linux platform paragraphs that §6
removes from individual docs.

### 3.4 `design/`

| Path | Action | Reason |
|---|---|---|
| design/README.md | KEEP (edit, §6.2) | Explains the reference and its build-time role. |
| design/prompt3-requirements.md | DROP | Prompt-3 requirement/acceptance record (2026-10-09). Not inside `tasarrım/`, so the inventory hashes are unaffected. |
| design/reference/tasarrım/** (73 files: `index.html`, 14 `*.fragment.html`, 30 `*.css`, 25 `*.js`, `ust_orta_alan_analizi.md`, `fonts/InterVariable.woff2`, `fonts/LICENSE.txt`) | KEEP, byte-for-byte unchanged | Build input (CSS + `index.html` → `reference-css.c`) and inventory input (every file is hashed). See §5. |

### 3.5 `database/SDBASE/`

| Path | Action | Reason / edit |
|---|---|---|
| database/SDBASE/README.md | KEEP (rewrite, §6.4) | Engine overview, CLI, module table. |
| database/SDBASE/IMPLEMENTATION_STATUS.md | KEEP (rewrite to current scope) | Required by `tools/radios_package.py:30` (`COMMON_FILES`) and `verify()` (L335), copied by `tests/test_packaging.py:29-32`. Keep L1-3 (header, replace the stale "160 workflow · 210 HTTP yolu" by "güncel sayılar `docs/islev-envanteri.json`"), L39-54 (scope/limits table), L62-67 (field acceptance still required); drop L5-37, L56-60, L69 (release/acceptance history and links to dropped docs). |
| database/SDBASE/docs/api.md | KEEP | HTTP/endpoint transport contract; named in `openapi.py:252,298,350`. |
| …/architecture.md | KEEP | Engine design; drop the dropped-doc clauses in L23 ("[yük ölçümü](load_verification.md)") and L30 ("[motor olgunluğu matrisinde](storage_maturity.md)"). |
| …/automatic_failover.md | KEEP | Feature/procedure. |
| …/data_dictionary.md | KEEP | 95-collection data contract; L138 drop link to local_manual_audit.md. |
| …/disk_indexes.md | KEEP | Feature/procedure. |
| …/engine_operations.md | KEEP | Limits/monitoring procedure. |
| …/integrity.md | KEEP | Scrub contract. |
| …/mairlist31_csv.md | KEEP | Adapter format contract. |
| …/new_features.md | KEEP (strip L1-5 version narration, retitle "Playlist, smart block, podcast ve dinleyici sayımı"; drop L96 dated validation narrative) | Only usage doc for these workflows. |
| …/online_maintenance.md | KEEP | Procedure. |
| …/operations.md | KEEP | Operations; L35 drop the load_verification clause. |
| …/ops_archive.md, …/ops_scheduler.md | KEEP | Procedures. |
| …/playback_markers.md | KEEP | Marker contract. |
| …/portable_export.md | KEEP | Package format contract. |
| …/production_acceptance.md | KEEP (drop L36 dated measurement) | Procedure of `database/SDBASE/tools/acceptance.py`. |
| …/radioboss_m3u8.md | KEEP (L3 drop the local-PDF link `../../../kullanım klıvazu/…`) | Adapter contract. |
| …/rekordbox.md | KEEP | Adapter boundary; links `../../../docs/rekordbox-usb-tracklist.md` (stays valid). |
| …/replication.md, …/replication_stream.md, …/retention.md, …/schema_migration.md, …/snapshot_queries.md, …/wal_recovery.md | KEEP (drop the "2026-10-01'de … geçti" result sentences: replication.md:88, replication_stream.md:90) | Feature/procedure contracts. |
| …/adapter_coverage.md | KEEP (L13,L23 drop the open_source_app_verification links; L15 drop local_manual_audit/gap_matrix links; L19 drop the local-PDF link) | Adapter coverage matrix. |
| …/audit_closure.md | DROP | "30 Eylül 2026 denetiminin kapatma matrisi". |
| …/finalization_081.md | DROP | Release check of 0.8.1 (2026-10-01). |
| …/gap_matrix.md | DROP | Requirement gap/acceptance planning matrix vs. competitor products. |
| …/load_verification.md | DROP | "Bu 0.6.0 koşusunun tarihsel raporudur" (Linux `/proc/meminfo`, RLIMIT_AS). |
| …/local_manual_audit.md | DROP | Audit of local PDF manuals outside the project (`/home/sturkan/...`). |
| …/maturity_080.md | DROP | 0.8.0 maturity/measurement report (features are in the feature docs). |
| …/open_source_app_verification.md | DROP | LibreTime/OBPlayer clone experiments (`/home/sturkan/Desktop/radio project/clonlar/...`). |
| …/requirements.md | DROP | Thesis requirement mapping. |
| …/research.md | DROP | Product-manual research (2026-09-30). |
| …/storage_maturity.md | DROP | "0.7.0 … tarihsel raporudur". |
| …/verification.md | DROP | Verification history. |

`.md` changes under `database/SDBASE` do not affect the service `source_digest`
(`database/SDBASE/src/sdbase/runtime_version.py:13` hashes `sdbase/*.py` only).

### 3.6 Other READMEs (all KEEP, rewrite per §6.1-6.2)

`README.md`, `src/README.md`, `include/README.md`, `config/README.md`, `assets/README.md`,
`data/README.md`, `tests/README.md`, `tools/README.md`, `cmake/README.md`, `packaging/README.md`,
`database/README.md`, `design/README.md` (+ tool sub-READMEs `tools/otomasyon_testleri/README.md`,
`tools/yayin_akisi_724/README.md`, owned by the tools area). `README.md`, `config/README.md`,
`database/README.md`, `database/SDBASE/README.md`, `database/SDBASE/IMPLEMENTATION_STATUS.md` are
**mandatory files** of `tools/radios_package.py` (`COMMON_FILES` L27-31) — never delete/rename them.

### 3.7 Non-doc leftovers noticed (report only; owners decide — D2b)

* `config/credentials.json` (703 B) is present in the Windows copy although `data/database` is empty:
  a stale private identity file from the Linux installation (gitignored). First-run `init` recreates it;
  candidate for removal by the config/launcher owner (content not read here).
* `config/ui-preferences.ini`, `config/audio-panel.ini`: per-user state carried from Linux (gitignored).
* `src/{automation,db,emergency,integrations,monitoring,traffic}/` contain only `.gitkeep` (placeholder
  module dirs announced in `src/README.md` as "henüz boş"); `assets/icons/` only `.gitkeep`.
* History-labelled build files `cmake/Prompt5Tests.cmake`, `Prompt7Tests.cmake`, `Prompt8Tests.cmake`,
  `Prompt10Playlist.cmake`, `Pdf2HeaderStatus.cmake`, `Pdf4Neon.cmake`, `Pdf12Playlist.cmake` (names
  only; build area may rename). Test artefact names `prompt5-footer-%d.png`/thread `prompt5-private-HTTP`
  in `tests/test_header_navigation_ui.c:117,161`.
* `.gitignore` lacks `/runtime/` (staged dev prefix, rebuilt by `radios_package.py runtime`),
  `/build-*/`, and a decision on `/packaging/windows/cache/` (pinned python embed zip + tzdata wheel).
  Recommend adding a `.gitattributes` (§5.4).

---------------------------------------------------------------------------------------------------------

## 4. References to `docs/` and `design/` from code, tests, tools, CMake, database

### 4.1 Functional (behaviour, build or tests depend on them)

| Where | What | Consequence for this area | Required action |
|---|---|---|---|
| CMakeLists.txt:21 | `file(GLOB DESIGN_CSS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/design/reference/tasarrım/*.css")` | design CSS are build inputs | keep folder + files (verified OK with Ninja, §5.2) |
| CMakeLists.txt:23-29 | custom command runs `${Python3_EXECUTABLE} tools/css_to_gtk.py --output-dir build/generated`, DEPENDS css + `design/reference/tasarrım/index.html`; outputs `reference-css.c`, `reference.css`, `css-manifest.json`; `reference-css.c` is a source of `radios_ui` (L57) | build fails if index.html/CSS missing or undecodable | build area: add `-X utf8` to the COMMAND (belt and braces) |
| tools/css_to_gtk.py:15 | `DESIGN = ROOT / "design/reference/tasarrım"` | path contract | none |
| tools/css_to_gtk.py:113,117 | `read_text()` without encoding | **fails on Windows** (verified: cp1254, `UnicodeDecodeError` at byte 0x9e) | tools area: `encoding="utf-8"` |
| tools/css_to_gtk.py:186,191,192 | `write_text()` without encoding/newline | cp1254 output + CRLF; `reference-css.c` holds `ensure_ascii=False` JSON strings → mojibake | tools area: `encoding="utf-8", newline="\n"` |
| src/ui/window.c:26,738-739; tests/test_rekordbox_ui.c:40,47; tests/test_automation_calendar.c:22,3040; tests/test_playlist_redesign_ui.c:359-362; tests/test_media_drop_ui.c:767-769; tests/test_broadcast_catalogue_ui.c:873-878 | consume `radios_reference_css_parts[]` generated from design CSS | any byte change in design CSS changes the app look and these tests | do not edit design files |
| tools/islev_envanteri.py:7,125-128 | requires `design/reference/tasarrım/index.html` + `database/SDBASE/src/sdbase/openapi.py` | — | keep both |
| tools/islev_envanteri.py:131 | parses every `design/reference/tasarrım/*.html` (sorted) | — | keep fragments |
| tools/islev_envanteri.py:141-144 | imports `sdbase.openapi`, `sdbase.workflow_api` | **cannot run on Windows until `sdbase/storage.py:17` stops importing `fcntl`** (verified) | SDBASE engine area |
| tools/islev_envanteri.py:165-169 | hashes *all* files under `design/reference/tasarrım` + `sdbase/*.py` (`read_bytes`) | CRLF checkout or any edit changes hashes; `sorted()` of `WindowsPath` is case-insensitive (order may differ from Linux) | tools area: sort by `relative_to(root).as_posix()`; regenerate after port; `.gitattributes` (§5.4) |
| tools/islev_envanteri.py:204-205 | writes `docs/islev-envanteri.json`, `docs/sdbase-openapi.json` | the two JSON must stay in `docs/` | KEEP both |
| tools/islev_envanteri.py:209 | `--check` compares `read_text(encoding="utf-8")` (universal newlines) | tolerant to CRLF | – |
| tools/islev_envanteri.py:214 | `write_text(content, encoding="utf-8")` → CRLF on Windows | regenerated JSON differs byte-wise from Linux | tools area: `newline="\n"` |
| tools/radios_package.py:27-31 | `COMMON_FILES` = README.md, config/README.md, database/README.md, database/SDBASE/README.md, database/SDBASE/IMPLEMENTATION_STATUS.md (+ `.sh` launchers, CMakeLists, ini, pyproject) | these READMEs are mandatory; `_read` raises if missing | keep them (§3.6) |
| tools/radios_package.py:34-41 | `COMMON_TREES` ships `design` (.css .html .js .woff2 .txt .md .svg .png), `docs` (.md .json), `database/SDBASE/docs` (.md) | history ships to users today | apply this manifest; tools area keeps the trees |
| tools/radios_package.py:335-340 | `verify()` required set includes `COMMON_FILES` | same | same |
| tests/test_packaging.py:29-41 | copies `COMMON_FILES` and `design/reference/tasarrım/index.html` from the real tree, creates every `COMMON_TREES` dir | needs the files above and a `docs` tree entry | keep `docs/` in `COMMON_TREES` |
| tests/test_packaging.py:87 | asserts `RadioS/design/reference/tasarrım/index.html` in the archive | design kept | – |
| tests/test_packaging.py:129 | synthetic `RadioS/docs/unreviewed.json` must be rejected | relies on manifest allowlist | – |
| tests/test_packaging.py:196-205 | symlinks `docs/leak.json`, `docs/leakdir` | Windows: `symlink_to` needs Developer Mode/privilege (WinError 1314 not caught) | tests/tools area |
| tests/test_packaging.py:218 | `os.link(private, docs/not-a-safe-document.json)` | works on NTFS | – |
| src/core/storage_locations.c:86-88 | protected names `config src include tools tests docs design assets vendor packaging cmake database/SDBASE` | keep `docs` and `design` in the list | core area: separator/case-insensitive compare on Windows (`database/SDBASE` has `/`, `path_inside` uses `G_DIR_SEPARATOR`) |
| tools/radios_database.py:100-101,176 | same list (Python) | same | tools area: `os.path.normcase` comparison |
| database/SDBASE/src/sdbase/desktop_transfer_package.py:82-84,249 | same list | same | SDBASE app area |
| database/SDBASE/src/sdbase/openapi.py:252,298,350 | OpenAPI text "docs/api.md" | `database/SDBASE/docs/api.md` must exist | KEEP api.md |

### 4.2 Comment / docstring references (fix the pointer; no behaviour)

| Where | Points to | New text |
|---|---|---|
| src/ui/transfer_ui.c:9 | docs/kilavuz-2026-10-18-D.md | `docs/aktarim-paketi.md` |
| tests/test_transfer_ui.c:8 | docs/kilavuz-2026-10-18-D.md | `docs/aktarim-paketi.md` |
| tests/run_transfer_ui.py:5 | docs/kilavuz-2026-10-18-D.md | `docs/aktarim-paketi.md` |
| tests/test_live_step_wait_ui.c:41 | docs/kilavuz-2026-10-18-4c/sozlesme.md §2 | `docs/sema-akisi-sozlesmesi.md §2` |
| tests/test_live_row_ui.c:53 | docs/kilavuz-2026-10-18-4c/sozlesme.md §2 | `docs/sema-akisi-sozlesmesi.md §2` |
| tests/content_scale_test_input.h:5 | docs/kilavuz-2026-10-18-C-C3.md | file is X11/Wayland-only and is replaced by the tests area (Win32 input); if kept, drop the pointer (rationale lives in `src/ui/content_scale.c:219-234`) |
| tools/otomasyon_testleri/senaryolar_kume.py:3 | docs/kilavuz-2026-10-18-4c/kume.md | "Senaryo tanımları bu dosyadadır (K1–K6, T1–T4, L1–L6)." |
| tools/otomasyon_testleri/README.md:65 | docs/kilavuz-2026-10-18-4c/kume.md | "senaryolar `senaryolar_kume.py` içinde tanımlıdır" |
| tools/otomasyon_testleri/senaryolar_724.py:27 | docs/yayin-akisi-724-2026-10-17.md | `tools/yayin_akisi_724/README.md` ("Üretilen yayın akışı biçimi") |
| "contract §1b/§2/§5" without path: src/ui/automation_calendar.c:153,916,1539; src/core/weekly_programme_plan.c:92,546; src/ui/event_playout.c:33,438 ("4c schema contract") | sozlesme.md sections | still valid because §3.3-B keeps the numbering; optional: mention `docs/sema-akisi-sozlesmesi.md` once per file |
| "kılavuz 18.10 contract / rule g / settings.transitions": include/radios/transition_distribution.h:10; include/radios/autodj_automation_ui.h:37; src/ui/autodj_automation_ui.c:1204; src/ui/rotation_editor.c:406,2309; tests/test_rotation_editor_regressions.c:1200 | C-C8 §3 | still valid because §3.3-C keeps letters (a)-(h); optional: name `docs/gecis-dagilimi-sozlesmesi.md` |

All other "Kılavuz §NN / s.NN / R10.1 / A-12" comments (≈90 in src/, include/, tests/, CMakeLists.txt,
`database/SDBASE/src/sdbase/workflow_api.py:175`, `desktop_transfer_package.py:48`,
`desktop_automation_calendar.py:34`, `tools/gui_backend_coverage.py:2`, `tests/test_gui_backend_coverage.py:2`)
cite the requirement PDF `radyo_otomasyonu_kilavuzu.pdf`, which is outside the project; they are
requirement IDs, not docs references — leave them.

### 4.3 Doc-to-doc links in surviving files that must change

| File:line | Current target | Replacement |
|---|---|---|
| README.md:7,54,56,58,60,62,64,66,68,70,72,74,76,78,82,126,128,130,146,156,186-205 | stage docs, closure audit, function maps, progress, cevrimici-muzik | README rewritten (§7); use the new names |
| README.md:24 | docs/10-playlist-ve-otomasyon.md#autodj-otomasyonu-tasarlanmış-geçiş-ön-ayarları | docs/playlist-ve-otomasyon.md#autodj-otomasyonu-tasarlanmış-geçiş-ön-ayarları |
| config/README.md:9 | ../docs/05-veritabani-kurulumu.md | ../docs/veri-servisi.md |
| config/README.md:13 | ../docs/cevrimici-muzik.md (dangling) | delete the paragraph (feature removed) |
| database/README.md:7 | ../docs/05-…, ../docs/06-… | ../docs/veri-servisi.md, ../docs/c-veri-baglantisi.md |
| tools/README.md:15 | ../docs/05-veritabani-kurulumu.md | ../docs/veri-servisi.md |
| tools/README.md:23 | ../docs/13-yonetim-ve-guvenilirlik.md | ../docs/yonetim-ve-guvenilirlik.md |
| tools/README.md:36 | ../docs/14-nihai-kabul-ve-paketleme.md | ../docs/fiziksel-kabul.md |
| tests/README.md:97,129,139,157,165,200,219,242,266,297,315,337 | medya-surukle-birak, proje-kapanis-denetimi, rekordbox-usb-tracklist, cevrimici-muzik, 14-…, 07…13 | tests/README rewritten (§7.2); new names; drop closure/online links |
| docs/03-c-uygulama-altyapisi.md:3 | 04-native-tasarim.md | native-tasarim.md |
| docs/10-playlist-ve-otomasyon.md:189 | kilavuz-2026-10-18-4c/sozlesme.md, kilavuz-2026-10-18-4c.md | sema-akisi-sozlesmesi.md, gecis-dagilimi-sozlesmesi.md |
| docs/ses-yonlendirme.md:25 | cevrimici-muzik.md (dangling) | delete |
| docs/rekordbox-ice-aktarim.md:4 | rekordbox-usb-tracklist.md | unchanged |
| database/SDBASE/README.md:7,9,11,13,70 | finalization_081, maturity_080, storage_maturity, load_verification, local_manual_audit, open_source_app_verification, research | remove those links (keep links to KEEP docs) |
| database/SDBASE/IMPLEMENTATION_STATUS.md:7,16,26,60,69 | dropped SDBASE docs | removed with the rewrite (§3.5) |
| database/SDBASE/docs/architecture.md:23,30; data_dictionary.md:138; operations.md:35; adapter_coverage.md:13,15,19,23; new_features.md:96; radioboss_m3u8.md:3 | dropped SDBASE docs / local PDFs | remove the clause or link (§3.5) |

---------------------------------------------------------------------------------------------------------

## 5. `design/` — build-time contract and Windows facts

### 5.1 Contract to preserve
* `design/reference/tasarrım/index.html` + the 30 CSS files it links (in `<link>` order; all 30 CSS in the
  folder are linked: knobs, broadcast, workbench, neon-tabs, panel-selection, header-clock, header-layout,
  cue-controls, programme-gui, playlist-gui, operations-ui, transition-editor, waveform-edit-gui,
  task-views, live-desk, broadcast-black, broadcast-workspace, live-player-gui, emergency-workspace,
  dj-mixer-gui, technical-management, advert-workspace, programme-blocks, queue-waveform,
  dj-performance-layout, readability, playlist-workspace, automation-workspace, automation-calendar,
  programme-transition-controls) plus the single `<style>` block are the source of
  `radios_reference_css_parts[]` (window.c + 5 GUI tests).
* HTML/JS are never executed; 15 HTML files are parsed by the inventory tool; every file is hashed.
* Files are UTF-8 (index.html has 2,420 non-ASCII characters) with LF line endings (0 CRLF) — keep it so.
* `css_to_gtk.py` emits only `color/background*/border*/padding*/margin*/font-size/font-weight/font-style/
  opacity/min-width/min-height` (`PROPERTIES`, L16-24); **it never emits `font-family`**, so D5
  ("'Inter Variable Text' first") needs **no** design edit — font families live in
  `assets/themes/*.css` and C strings (UI/runtime areas).
* Never edit, reformat or re-save design files (that changes the shipped look, the generated CSS and the
  inventory). The only design change in this manifest is dropping `design/prompt3-requirements.md`.

### 5.2 Verified on the target machine (scratchpad experiment, project untouched)
* Tiny CMake project reproducing `CMakeLists.txt:21-29` with the folder `design/reference/tasarrım`:
  configure OK, first build runs the generator, second build "no work to do", touching `a.css` or
  `index.html` re-runs it; `ninja -t query` shows the `…/tasarrım/…` inputs. Toolchain
  `C:\msys64\ucrt64\bin\cmake.exe` 4.4.4, `ninja.exe` 1.13.2. ⇒ keep the folder name.
* With a ~264-character build path, `ninja` failed at `mkdir(CMakeFiles/...)` and CMake warned about
  `CMAKE_OBJECT_PATH_MAX`; with a short path everything worked ⇒ document "kısa proje/derleme yolu"
  (root `C:\Users\<kullanıcı>\Desktop\RadioS` is fine; avoid deep scratch/temp build dirs).
* `C:\msys64\ucrt64\bin\python.exe` 3.14.8: `locale.getpreferredencoding()` = `cp1254`, `utf8_mode` 0;
  `Path("…/index.html").read_text()` → `UnicodeDecodeError: 'charmap' codec can't decode byte 0x9e`;
  with `-X utf8` it decodes correctly ⇒ the build is broken until §4.1 css_to_gtk fixes land.
* `runtime\python\python.exe -E -s -B -X utf8 tools\islev_envanteri.py --check` →
  `ModuleNotFoundError: No module named 'fcntl'` (`sdbase/storage.py:17`); no files written.

### 5.3 Other Windows pitfalls for design consumers
* Case-insensitive file system: `css_to_gtk.py:116` resolves `href` names; a case typo that fails on
  Linux would silently pass on Windows — keep names as they are.
* `islev_envanteri.py:131,165` `sorted()` on `WindowsPath` compares case-folded parts (different order than
  POSIX for mixed-case names; today only `fonts/InterVariable.woff2`/`fonts/LICENSE.txt`, same order) —
  sort by `as_posix()` string to be deterministic across OSes.
* The non-ASCII folder appears in CMake (`CMakeLists.txt:21,26`), Python (`css_to_gtk.py:15`,
  `islev_envanteri.py:125`), tests (`test_packaging.py:38,87`). Any `.ps1`/`.cmd` that names it must be
  UTF-8 **with BOM** (PowerShell 5.1 reads BOM-less scripts as ANSI) or avoid the literal (glob).

### 5.4 Recommended `.gitattributes` (repo hygiene, integrator)
```
* text=auto eol=lf
*.cmd text eol=crlf
*.bat text eol=crlf
*.ps1 text eol=crlf
design/** -text
docs/*.json -text
*.png binary
*.woff2 binary
*.ttf binary
*.zip binary
*.whl binary
```
Reason: the inventory hashes raw bytes of design files and `sdbase/*.py`; a CRLF checkout would make
`islev_envanteri.py --check` fail and change `source_hashes`.

---------------------------------------------------------------------------------------------------------

## 6. Linux statements and history narration in surviving docs → rewrite list

### 6.0 Replacement vocabulary (Turkish wording to write)

| Linux wording | Windows wording to use |
|---|---|
| `./run-radios.sh [arg]` | `run-radios.cmd [arg]` (PowerShell: `.\run-radios.ps1 [arg]`); kurulu/hazır kopyada `bin\RadioS.exe` veya Başlat menüsü kısayolu |
| `./run-database.sh <komut>` | `run-database.cmd <komut>` (`.\run-database.ps1 <komut>`; aynı alt komutlar) |
| `run-radios-sdk.sh`, `radios_sdk.py`, Freedesktop SDK 25.08, Flatpak, `flatpak-spawn`, "ana sistem/SDK PID ad alanı" | (sil — Windows karşılığı yok, D1) |
| `python3 tools/X.py …`, `PYTHONDONTWRITEBYTECODE=1 python3 …` | `runtime\python\python.exe -E -s -B -X utf8 tools\X.py …` (proje kökünden; PATH'teki python kullanılmaz) |
| `sdbase --db /veri/RadioS.sdb <komut>` | `database\SDBASE\src` dizininde `..\..\..\runtime\python\python.exe -E -s -B -X utf8 -m sdbase.cli --db D:\RadioSVeri\RadioS.sdb <komut>` |
| `export AD=değer`, `AD=değer komut`, `$PWD/…` | `$env:AD = 'değer'; komut`, `$PWD\…` |
| `mkdir -p`, `curl … -o` | `New-Item -ItemType Directory -Force …`, `curl.exe … -o` |
| `/tmp/x`, `/veri/…`, `/yedek/…`, `/arsiv/…`, `/guvenlik/ca.pem` | `$env:TEMP\x`, `D:\RadioSVeri\…`, `D:\RadioSYedek\…`, `D:\RadioSArsiv\…`, `D:\RadioSGuvenlik\ca.pem` (örnek) |
| `/run/media`, `/media`, `/mnt`, "USB kökü" | sürücü harfi (`E:\`) veya UNC (`\\sunucu\paylaşım`) |
| PulseAudio, PipeWire (Pulse uyumluluğu), libpulse, "ses sunucusu" | Windows ses aygıtları (WASAPI paylaşımlı mod), "Windows Ses hizmeti" |
| `pactl`/`parec`, "özel Pulse daemon", "sanal Pulse çıkışları" | RadioS'un sanal test ses arka ucu (`RADIOS_TEST_AUDIO_*`; fiziksel aygıt kullanılmaz) |
| "monitor kaynağı / çıkış geri dinlemesi" | çıkış aygıtının WASAPI geri döngü (loopback) yakalaması |
| "uygulama özel sessiz çıkışa taşınır" | uygulama sesi Windows işlem geri döngüsüyle (process loopback, Windows 11 / build 20348+) yakalanır; uygulama taşınmaz, kendi aygıtında duyulmaya devam eder |
| ALSA sıralayıcı, libasound | WinMM MIDI |
| X11, Wayland, Xvfb, XTest, `GDK_BACKEND=x11`, "ekran oturumu" | etkileşimli Windows oturumu; GUI testleri özel test masaüstünde (tests area) |
| "Dosya yöneticisi" | "Dosya Gezgini" |
| `0600` dosya / `0700` dizin, "kullanıcıya özel izin" | "yalnız kullanıcıya özel (Windows ACL: sahip kullanıcı; erişim yalnız kullanıcı, SYSTEM ve Administrators; kalıtım kapalı). Proje NTFS/ReFS biriminde olmalıdır." |
| fsync, atomik rename, dizin fsync | `FlushFileBuffers` + `MoveFileExW(REPLACE_EXISTING, WRITE_THROUGH)` (paylaşım ihlalinde sınırlı yeniden deneme); Windows'ta dizin fsync yapılmaz |
| `O_NOFOLLOW`, symlink/sembolik bağlantı | yeniden ayrıştırma noktaları (sembolik bağlantı, junction) izlenmez/reddedilir |
| FIFO | normal dosya olmayan yollar (aygıt, adlandırılmış kanal) reddedilir |
| tek hardlink | NTFS bağlantı sayısı 1 |
| `flock`, süreç kilidi | `LockFileEx` (aynı kilit dosyası adları) |
| SIGTERM/SIGINT, "kapanış sinyali" | kapanış isteği: adlandırılmış olay `Local\RadioS-stop-<pid>`, Ctrl+C, pencereyi kapatma |
| SIGKILL | süreç sonlandırma (`TerminateProcess`, Görev Yöneticisi) |
| pidfd, `/proc`, "Linux açılış kimliği" | PID + süreç oluşturma zamanı |
| `nice 10` | düşük iş parçacığı önceliği |
| ELF, readelf, GLIBC, DT_NEEDED, RPATH, `.so.NN` | PE ve paketlenmiş DLL kapanışı (`bin\`) |
| `tar.gz` ikili paket, `.desktop` | `RadioS-<sürüm>-windows-x86_64.zip`, NSIS `…-setup.exe` |
| Fontconfig ile WOFF2 | Pango (`pango_font_map_add_font_file`) ile `assets\fonts\InterVariable.ttf`; aile adı "Inter Variable Text" (D5) |
| `renameat2(RENAME_NOREPLACE)` | `MoveFileExW` (REPLACE_EXISTING olmadan: hedef varsa başarısız) |
| "Linux/Wayland, GCC 15.2 … doğrulandı" | (sil) ya da "Windows 10/11 x64, MSYS2 UCRT64" |

User-facing paths in prose use `\`; stored/portable values in contracts keep `/` (D6).

### 6.1 `README.md` (221 lines) — rewritten per §7; source line disposition

| Lines | Content | Disposition |
|---|---|---|
| 1, 3 | title, product intro (L3 "Proje kökü `kaynak kodlar/RadioS/`") | keep intro; replace root sentence ("Proje kökü bu klasördür; …") |
| 5 | design reference statement | keep |
| 7 | 0.17.0 closure (signals, link to closure audit) | drop |
| 9-12 | 0.17.1 SDK fix + drag-drop | keep drag-drop as feature bullet (→ medya-surukle-birak.md); drop SDK |
| 14-18 | 0.18.0 online music removal | drop; one current-fact sentence: "Çevrimiçi müzik servisleri desteklenmez; yayın yerel medya arşivinden yapılır." |
| 20-24 | 0.19.0 AutoDJ automation page | feature bullet (new link) |
| 25-26 | launcher builds, falls back to last good build, `RADIOS_SKIP_BUILD=1` | keep in "Çalıştırma" for `run-radios.cmd` |
| 28, 30, 32-37, 39-42 | silence guard, media archive, playlist editing, rekordbox USB | feature bullets |
| 44-48 | 0.17.0 acceptance numbers | drop (keep the one feature sentence of L46-48 as bullet "Program taslağı…") |
| 50 | SDBASE statement + "gerçek çalışma veritabanı oluşturuldu" | keep first two sentences; drop history |
| 52-82 | stage 1-14 and 0.15/0.16 narration | drop |
| 84-112 | Linux build/run (L86 Linux/libpulse/Wayland, L91-93, L98-105 SDK, L107 SDK, L110-111) | replace by §7 "Kaynaktan derleme", "Çalıştırma" |
| 114 | GUI tests (`pactl`/`parec`, X11) | → "Testler" (Windows) |
| 116-130 | usage notes (preferences, media library, queue, players, DJ, playlist, advert, people/monitoring) | keep as "Kullanım özeti"; L122 "Ses sunucusu kesilip döndüğünde" → "Ses aygıtı kaybolup döndüğünde" |
| 132-156 | per-stage acceptance numbers, `data/backups/radios-20261006T…` | drop |
| 158-169 | `run-database.sh` commands, 127.0.0.1:8000, Python 3.11+ | rewrite (`run-database.cmd`; paketli Python 3.14) |
| 171-182 | layout table | keep, update (§7) |
| 184-205 | stage doc index | replace by "Belgeler" index (§7) |
| 207-221 | inventory check (`python3 tools/islev_envanteri.py`) | keep under "Geliştirici araçları" with Windows command |

### 6.2 Other READMEs

* `src/README.md` L3 ("diğer motor modülleri ilgili aşamalarda doldurulacaktır") → present tense; table:
  add `playout/`, `media/`, `audio/` (WASAPI backend `wasapi_backend.c`, `wasapi_device.c`),
  `core/platform.c`; L12-17 placeholder dirs → remove rows once the owners delete the dirs (§3.7).
* `include/README.md` L3 "Aşama 3'te…", L5 "Aşama 6 ile…" → neutral; mention `radios/platform.h`
  (Windows platform layer).
* `config/README.md` L3 `run-database.sh` → `run-database.cmd`; L7 `run-database.sh init`, `0600`,
  `run-radios.sh` → §6.0 wording (+ `repair-permissions` from tools spec); L9 link → veri-servisi.md;
  L13 online-music paragraph → delete.
* `assets/README.md` L5 "Fontconfig bu WOFF2 kaynağını … yükler" → D5 (TTF via Pango/DirectWrite; WOFF2
  kept as design source); mention `fonts/InterVariable.ttf`.
* `data/README.md` L3 "Aşama 5'te … oluşturuldu" → "İlk açılışta oluşturulur"; L8 "0700 dizin/0600
  dosya" → private ACL; L21 "İlk yayın ucunun gerçek UUID'si … kuruldu" → generic; add `logs\crash\`
  (minidumps, D3) and `recovery\`.
* `database/README.md` L3 keep (import provenance + manifest link); L5 "POSIX `fcntl` … Linux ortamını
  hedefler" → "Windows'ta dosya kilidi ve özel dosya denetimleri `sdbase/_platform.py` üzerinden
  (`LockFileEx`, ACL) yapılır; motor paketli Python 3.14 ile çalışır"; L7 history + links → new names.
* `design/README.md` L5 "src/ui/ altında geliştirilecektir" → present; add: "Derleme sırasında
  `tools/css_to_gtk.py` `index.html`'in style bloğunu ve bağladığı CSS dosyalarını GTK CSS'ye çevirir;
  `tools/islev_envanteri.py` bütün dosyaların özetini envantere yazar. Bu klasördeki dosyalar
  değiştirilmez." Keep L7 manifest link.
* `cmake/README.md` L3 keep; add MSYS2 UCRT64/Ninja note and the `-X utf8` generator note.
* `tools/README.md` L3-11 (`python3` → §6.0), L15 (`run-database.sh`, link), L25 (`radios_sdk.py`, SDK
  → delete), L27-32 (SDK fallback → delete; keep build-before-run + `RADIOS_SKIP_BUILD`), L34 (packaging
  link), L36 (link → fiziksel-kabul.md). Windows content: python-tools-launcher §4.19.
* `packaging/README.md` — entire file is Linux (L3 Linux release, L8-12 commands/`tar.gz`, L15 ELF,
  L21 DT_NEEDED/GLIBC/`.so.61`/Pulse, L23 `org.freedesktop.Sdk`, `run-radios-sdk.sh`, L25
  `run-radios.sh`/SDK, L27 fonts, L29 `python3`) → replace with python-tools-launcher §4.17 (windows
  zip/NSIS, runtime preparation, DLL closure, `runtime.json`), keep L17/L19 integrity/exclusion rules.
* `tools/otomasyon_testleri/README.md` L20/L51/L74 `build.sh`, `flock`; L22-25/L53-54/L76 `python3`,
  `RADIOS_BUILD_DIR=… cmd` syntax; L65 doc link (§4.2) — owner tools area.
* `tools/yayin_akisi_724/README.md` L9-14 `python3`, L11 hard-coded "Yeni Klasör 3", L18 `cc …` —
  owner tools area; plus §3.3-G insertion.

### 6.3 Surviving `docs/` files (old name → line: issue → action)

**c-uygulama-altyapisi.md (03)**: L1 title → "C uygulama altyapısı"; L3 "2026-10-06 tarihinde tamamlandı.
RadioS 0.3.0 … Kullanıcının görsel düzeltmesiyle…" → present-tense description; L15 `run-radios.sh` row →
`run-radios.cmd` / `bin\RadioS.exe`; L21 root discovery ("gerçek executable üst dizinleri") → exe
directory upward (`runtime\bin` → kök), "mevcut symlink ile dışa kaçış" → reparse points; L25 "Doğrulanan
ortam Linux/Wayland, GCC 15.2.0 … `/tmp` üzerinden … CTest 4/4" → drop; L27 evidence paths → keep only
"CTest günlüğü `build\Testing\Temporary\LastTest.log`".

**native-tasarim.md (04)**: L7 "29 CSS dosyasını" and L9 "30 kaynaktan 1.552 GTK kuralı", "CMake kaynak
CSS değiştiğinde…" → "kaynak listesi ve kural sayısı `build\generated\css-manifest.json`'dadır"; L11
Fontconfig/WOFF2 → D5; L30 ("Gerçek DB kayıtları yüklenmez … oturum taslağıdır" — stage-4 state, now
false) → drop; L32-46 "Aşama 4 etkileşim kapsamı" table → drop; L48 "dosya izni 0600" → private ACL;
L50 heading "(2026-10-07)" → drop date; L57 sentence "YouTube Music kaynağında…" → drop; L61-67 visual
acceptance (Wayland, CTest 6/6, Valgrind) → drop; keep L69.

**veri-servisi.md (05)**: L1 → "Yerel veri servisi (SDBASE)"; L3 history → present; L7-15, L25-27 →
`run-database.cmd init|start|check|status|backup|stop` ("proje kökünden" instead of "`kaynak kodlar`
dizininden"); L17 `--config config\kurulum.local.ini`; L23 keep; L29 "Linux açılış kimliği … pidfd …
kapanış sinyali" → "yalnız bu kurulumun doğrulanmış servis sürecine (PID + oluşturma zamanı + komut +
servis örneği) `Local\RadioS-stop-<pid>` kapanış isteği gönderilir; zorla kesmez (`--force` ayrı)";
L31 `run-radios.sh`, "aşama 6'nın işidir" → present; L35-42 table paths with `\`; L44 0600/0700 →
private ACL + NTFS; L56 test counts/"19/19, 33/33" → only test file names; L58-60 → drop; L62
"Linux/POSIX … `/proc` ve Linux pidfd … Windows/macOS servis kabulü yapılmadı" → "Windows 10/11; süreç
kimliği PID + oluşturma zamanı; servis kabuksuz `CreateProcessW` ile başlatılır".

**c-veri-baglantisi.md (06)**: L3 "RadioS 0.6.0" → present; L19 stage references → drop; L37 0700/0600 →
private ACL; L41-48 Linux build commands → pointer to README "Kaynaktan derleme"; L50 "CTest toplam 12
hedef" → test list without count; L52 inventory counts → drop; L54-56 acceptance → drop.

**medya-kutuphanesi.md (07)**: L1 title; L3 "RadioS 0.7.0 … aşama 8'dir" → present; L19-25 column "Bu
geliştirme ortamındaki sürüm" (Linux FFmpeg 61.x) → drop column (versions come from MSYS2; minima from
`CMakeLists.txt`); L27 FFmpeg 5.1 compatibility paragraph → keep only if the build area keeps those
minima; L29 `ffmpeg` command → MSYS2 `ffmpeg.exe`; L56 "0600 izinle … fsync" → private ACL +
FlushFileBuffers; L58 `O_NOFOLLOW`, symlink, FIFO → §6.0; L101 "aşama 13 kapsamındadır … kalıcı çökme
kurtarması olarak sunmaz" → stale (durable recovery exists, `src/core/recovery.c`) → rewrite to current
behaviour or drop; L114 0600 → ACL; L117 "nice 10" → low thread priority; L125-138 commands → Windows;
L142-144 acceptance → drop.

**ses-motoru.md (08)**: L1 title; L3 libpulse/PulseAudio/PipeWire → WASAPI shared mode (engine format
48 kHz/2 ch/float stays); L7 "gerçek ses servisinden" → Windows audio devices; L18 `pulse_backend.c` →
`src/audio/wasapi_backend.c`, `wasapi_device.c` (verify names at apply time); L30 `libpulse>=15.0`,
"PulseAudio/PipeWire Pulse servisi", `paplay` → "WASAPI; harici süreç kullanılmaz"; L32 `pactl`/`parec` →
virtual test backend; L34-38 commands → Windows; L40-42 acceptance → drop; L44 "otomatik yeniden kurma
bu aşamada uygulanmadı" → stale; describe device-loss recovery as in yonetim-ve-guvenilirlik.md.

**dj-ve-gecisler.md (09)**: L1 title; L3 "RadioS 0.9.0" → present; L18 heading date → drop; L22 output
monitor sources → WASAPI loopback wording; L24 "(bu kurulumda `srt`)" → drop the example (MSYS2 FFmpeg
9.0.2 lists `srt` as an input protocol — verified); keep the generic rule; L26 "ayrı bir Pulse/PipeWire
kayıt akışıdır" → "ayrı bir WASAPI yakalama akışıdır"; L73 "özel 0600 geçici WAV" → private temp file;
L83 stage references → drop; L92-94 command → Windows; L98-102 acceptance + dated update → drop.

**playlist-ve-otomasyon.md (10)**: L3 "RadioS 0.10.0 … sonraki aşamaların kapsamındadır" → present;
the online-provider sentences in L92-94 ("Online sağlayıcıların ortak PCM akışı…") and L123-125
("Mevcut çevrimiçi PCM yolu ortaktır…", "Çevrimiçi içerikle başlatma…") → drop (feature removed; keep
the "Motor deck sahipliğini…" sentence of L125-126); L157 "özel veritabanı/Pulse
cihazlarında" → virtual devices; L159 AutoDJ acceptance → drop; L167 "(normal form, 18.10)" and the
"18.10:" markers in L177 → drop; L179 heading "(kılavuz 18.10.2026)" → drop; L189 links → new contract
docs; L197 "aşama 13 kapsamındadır" → current (durable outbox); L201 "Pulse zaman bilgisi" → audio
device clock; L209 `src/audio/pulse_backend.c` → WASAPI files; L213-215 keep (Pulse → virtual);
L217-223 → drop; add subsection §3.3-F.

**reklam-yonetimi.md (11)**: L1 title; L36 stage references ("aşama 12/13 kapsamındadır") → current
behaviour; L40-46 acceptance (Pulse, CTest counts, 0.11.0 window, "kimlik dosyası 0600") → replace with
a short test list (`desktop_advert`, `native_advert`, `runner`, `playout`).

**yayin-kayit-acil.md (12)**: L1 title; L7 `data/recordings/` → `data\recordings\` (or `storage.ini`
`recording_root`); L20 "exclusive create … `fsync`" → `CREATE_NEW` + FlushFileBuffers; L32-34 "Bu
aşamada … 13./14. aşamadadır" → current limits; L42 "gerçek sanal Pulse MAIN/CUE çıkışlarını" → virtual
devices; L46-48 → drop.

**yonetim-ve-guvenilirlik.md (13)**: L1 title; L3 "RadioS 0.13.0 … Bu aşama" → present; L19 fsync/atomic
rename/dir fsync/0600/0700/symlink/hardlink → §6.0; L31 "C Pulse backend aynı yapılandırılmış sunucuya
1/2/5 saniye" → WASAPI device re-activation (keep the 1/2/5 s backoff only if the audio area keeps it);
L39-45, L53-56, L60-62, L72-75 commands → `runtime\python\python.exe -E -s -B -X utf8 tools\…` (and
`run-database.cmd stop` for L54); L58 locks (`gui.lock` …) → `LockFileEx` wording; L79 "özel daemon ve
sanal cihazlar; host ses servisi kapatılmaz" → virtual audio backend; L81 "14. aşamadır" → drop;
L83-91 → drop.

**medya-arsivi.md**: L3-6 ("17.10.2026. Çekirdek modüller aynı gün yazılmış…") → "Sayfa **Ayarlar ›
Medya arşivi** altındadır."; L11 "Harici disk ve ağ klasörü" → drive letters / UNC; L12-13, L90 "0600" →
private ACL; L15 "sembolik bağlantılar izlenmez, gizli girdiler atlanır" → reparse points not followed;
hidden = dot-prefixed or Windows hidden/system attribute (confirm with media area); L44, L81
(`tools/radios_database.py`, `tools/radios_reliability.py` facts) keep.

**medya-surukle-birak.md**: L1 "RadioS 0.17.1 — Servis bağlantısı ve medya sürükle bırak" → "Medya
sürükle-bırak"; L3 "Dosya yöneticisinden" → "Dosya Gezgini'nden"; L14-18 keep (check UNC `file://`
semantics with the UI area: today "uzak bağlantı reddedilir"); L20-27 rewrite without SDK/Flatpak:
"Bağlantı kurulamıyorsa yerel başlatıcı veri servisini başlatır; yetki/kimlik/şema hatası servisi yeniden
başlatmaya dönüştürülmez."; L29-34 → drop; L36-72 → drop except a two-line test list
(`native_data_connection`, `native_media_drop`).

**otomasyon-sozluk-ve-kurallar.md**: L3 (date + desktop evaluation file) → drop; L69 parenthetical
"(önceden Yarı AutoDJ seçimi … bitiyordu)" and "Gerçek 7/24 programı verisinde … doğrulandı" → drop.

**program-onayi-ve-haklar.md**: L3 "2026-10-07, güncelleme 2026-10-17." → drop; L76 "sahip olunan Pulse
MAIN/CUE/MON çıkışlarıyla" → virtual devices.

**rekordbox-usb-tracklist.md**: L12 "USB'nin kök klasörünü" → "USB sürücüsünün kökünü (ör. `E:\`)";
L29 `USB/PIONEER/rekordbox/export.pdb` → `E:\PIONEER\rekordbox\export.pdb`; L66 "mutlak `usb_root`" →
Windows absolute path; L94-101 commands → PowerShell:
`New-Item -ItemType Directory -Force build\rekordbox-github\usb\PIONEER\rekordbox`,
`curl.exe --fail --location '<url>' -o build\rekordbox-github\usb\PIONEER\rekordbox\export.pdb`,
`$env:RADIOS_TEST_USB_EXPORT = "$PWD\build\rekordbox-github\usb\PIONEER\rekordbox\export.pdb"`,
`runtime\python\python.exe -E -s -B -X utf8 tests\test_desktop_rekordbox_device.py`, …

**ses-yonlendirme.md** (major rewrite; semantics from `specs/audio-routing-capture.md`): L1 "— 0.18.0" →
drop; L3-25 "Uygulama sesini MAIN'e aktarma": remove the "Çevrimiçi müzik → RadioS yayınına aktar" entry
point and the L25 dangling link; describe the routing-tab entry; L10-14 "özel sessiz çıkışa taşınır …
monitor akışı" → process loopback (app not moved, stays audible; RadioS' own process excluded); L21-22
"karma sistem monitor'ına geri dönüş yapılmaz" → "sistem geneli geri döngü kullanılmaz"; L40
`./run-radios.sh --profile routing` → `run-radios.cmd --profile routing` / `bin\RadioS.exe --profile
routing`; L45 "çalışan ses akışlarını" → active audio sessions (process name/PID); L53 move/restore
paragraph → "nothing is moved, nothing restored"; L57 "Geçici Pulse indeksleri" → "geçici oturum/işlem
kimlikleri"; L59 "Ses sunucusu" → "Windows ses hizmeti/aygıt"; L61 "0600 … exclusive create … libpulse"
→ ACL, `CREATE_NEW`, WASAPI; L63 stale file names → current (`include/radios/audio_patchbay.h`,
`src/audio/patchbay.c`, `src/ui/routing.c` — verify at apply time); L65-68 PipeWire `media.category`
role → drop; L70-94 host check/acceptance (PipeWire 1.6.8, X11/PID, build evidence) → drop except a test
list (`desktop_routing`, `native_routing`). Add requirement: Windows 11 or build ≥ 20348.

**sessizlik-korumasi.md**: L3-7 history ("17.10.2026 … o güne kadar yalnız alan taslağıydı") → neutral
intro; L13 "sunucu kesildi" → "ses aygıtı/hizmeti kesildi".

**rekordbox-ice-aktarim.md**: no Linux statement; keep.

**sema-akisi-sozlesmesi.md**, **gecis-dagilimi-sozlesmesi.md**, **aktarim-paketi.md**, **mimari.md**,
**fiziksel-kabul.md**: as §3.3.

### 6.4 `database/SDBASE` docs kept

* README.md: L5-13 version narration (0.8.1/0.8.0/0.7.0/0.6.0) → one line "SDBASE 0.8.1 · veri şeması v4 ·
  95 iş koleksiyonu" + feature list linking only KEEP docs; L17-21 `python3 -m pip install -e ./SDBASE`,
  `sdbase --db /veri/…` → "RadioS içinde kurulum gerekmez (paketli Python); doğrudan CLI §6.0 biçimiyle";
  L26, L33 `/veri`, `/yeni` paths; L31 "0.5.0, …" → neutral; L57-68 module table keep; L70 drop links to
  dropped docs.
* IMPLEMENTATION_STATUS.md: §3.5.
* engine_operations.md L8, L43-44 (`/veri`), L58 (`python tools/…` → runtime python);
  online_maintenance.md L6-9, L42 (`/tmp`); ops_archive.md L20 (0700/0600), L23-39 (`python3`, `/veri`,
  `/arsiv`); ops_scheduler.md L20 (0700), L24-29, L36 (`SIGINT`/`SIGTERM` → Ctrl+C / stop event),
  L61-65 (`export`); portable_export.md L10-14, L31 (`renameat2(RENAME_NOREPLACE)`); radioboss_m3u8.md
  L20, L48 (`/veri`); replication.md L18-67 (`/veri/primary`, `/veri/standby`), L88 (dated result →
  drop); replication_stream.md L14-25 (`--token-env`, `/guvenlik/…pem`), L90 (dated → drop);
  retention.md L23-31; schema_migration.md L17-22, L40-41 (`/veri/eski`, `/yedek`), L38, L59 (SIGKILL →
  process termination), L43 (`flock` → `LockFileEx`); wal_recovery.md L21-23 (`/yedek/base-2026-10-01`),
  L86 (`/tmp`); automatic_failover.md L71 (`/tmp`); disk_indexes.md L131, L213 (SIGKILL), L204-205;
  integrity.md L78; snapshot_queries.md L125; data_dictionary.md L180 (`PYTHONPATH=src python3 -c …` →
  `Set-Location database\SDBASE\src; ..\..\..\runtime\python\python.exe -E -s -B -X utf8 -c "…"`);
  production_acceptance.md L6-7 (`python3`, `/tmp`), L36 drop; new_features.md L75-87 (`/veri`), L96
  drop. The upstream code citations (`src/backend/...` of PostgreSQL, `src/wal.c` of SQLite) are external
  references — leave them.

### 6.5 `tests/README.md` (444 lines; owner tests area) — doc-history items

* Drop as history: L31-36 (online music removal), L76-92 (online-018 focused run, first failing routing
  run), L94-106 (0.17.x counts), L108-123 last half (0.17.0 narrative; keep the test-behaviour sentences),
  L141-149 (engine suite 333/333, flaky DJ note), L157 (online services — targets no longer exist), L159
  (0.15.0 narrative), L161-165 (stage-14 narrative; keep target descriptions), L173 (Linux archive
  command), L200, L221, L242, L248, L268, L299, L313 (real Icecast 2.4.4 experiment), L317, L339,
  L341-381 (online search/YouTube/Spotify targets removed except L353-371 player-queue behaviour),
  L373-376, L409-420, L435-439 (dated acceptance runs).
* Linux statements: L28-29, L43 (X11 clipboard / `GDK_BACKEND=x11`), L68, L86, L125, L159, L165, L204,
  L208, L227, L266, L286, L290, L307, L331, L359, L395 (Pulse/pactl/parec/daemon), L111, L115, L117
  (SIGTERM/SIGINT → stop event), L178 (Wayland/X11), L216, L239, L253, L294, L402, L443 (`python3`,
  `$PWD`, env-prefix syntax), L182 ("Python 3.11+"), L323 (0600/0700/symlink/hardlink).
* Stage headings L202, L223, L256, L282, L301, L319 ("Aşama N … kabulü") → topic headings.
* Links: see §4.3.

---------------------------------------------------------------------------------------------------------

## 7. Proposed documents

### 7.1 New `README.md` (Turkish) — outline

No version-by-version narration, no acceptance counts, no stage list. One line names the version source
("Sürüm `CMakeLists.txt` içindeki `project(RadioS VERSION …)` değeridir.").

```
# RadioS
Kısa tanım (README L3 + L5): Windows 10/11 x64 için C11 + GTK 3 yerel masaüstü radyo yayın otomasyonu;
yerel SDBASE veri servisi (paketli Python); tasarım referansının CSS'i derlemede GTK CSS'ye çevrilir.
Çevrimiçi müzik servisleri desteklenmez; yayın yerel medya arşivinden yapılır.

## Özellikler                (her madde 1-3 cümle + belge bağlantısı)
- Yayın sırası, canlı oynatıcılar, SYNC ve AutoDJ          → docs/playlist-ve-otomasyon.md
- Medya kütüphanesi, analiz, dalga biçimi/cue; medya arşivi; sürükle-bırak
                                   → docs/medya-kutuphanesi.md, docs/medya-arsivi.md, docs/medya-surukle-birak.md
- Playlist, program, takvim, rotasyon şeması, kümeler, AutoDJ ön ayarları
                                   → docs/playlist-ve-otomasyon.md, docs/otomasyon-sozluk-ve-kurallar.md,
                                     docs/sema-akisi-sozlesmesi.md, docs/gecis-dagilimi-sozlesmesi.md
- DJ mixer, EQ, 14 efekt, sampler, A/B geçiş ön dinleme     → docs/dj-ve-gecisler.md
- Reklam yönetimi                                          → docs/reklam-yonetimi.md
- Yayın çıkışı (WAV kayıt, MP3/Icecast, metadata), acil devralma, sessizlik koruması
                                   → docs/yayin-kayit-acil.md, docs/sessizlik-korumasi.md
- Ses yönlendirme (uygulama sesi yakalama, mikrofon, kompresör, WAV) → docs/ses-yonlendirme.md
- rekordbox USB/SD ve XML içe aktarma → docs/rekordbox-usb-tracklist.md, docs/rekordbox-ice-aktarim.md
- Program onayı ve kullanım hakları  → docs/program-onayi-ve-haklar.md
- Ayar/medya aktarım paketi (.st)   → docs/aktarim-paketi.md
- Kullanıcı/yetki, izleme, dayanıklı işlem kuyruğu, yedek/geri yükleme → docs/yonetim-ve-guvenilirlik.md

## Sistem gereksinimleri
- Windows 10 (1809+) veya Windows 11, x64; proje/veri NTFS (veya ReFS) biriminde.
- Ses: WASAPI paylaşımlı mod aygıtları; uygulama sesi yakalama için Windows 11 / build 20348+.
- MIDI: WinMM. Ekran: monitör başına DPI ölçekleme desteklenir.
- Kısa kurulum yolu önerilir (uzun yol sınırları).

## Kurulum (kullanıcı)
- `RadioS-<sürüm>-windows-x86_64-setup.exe` (NSIS, yönetici izni gerekmez,
  varsayılan `%LOCALAPPDATA%\Programs\RadioS`) veya taşınabilir `…-windows-x86_64.zip`.
- İlk açılış: özel kimlikler + yerel veri servisi otomatik kurulur; oynatma/kayıt/mikrofon
  kendiliğinden başlamaz.
- Güncelleme/kaldırma: `data\` ve `config\` içindeki kullanıcı dosyaları korunur.
- Windows notları: SmartScreen; Microsoft Defender / Denetimli Klasör Erişimi (Masaüstü/Belgeler);
  OneDrive ile eşitlenen klasörlerde çalıştırmayın; 7/24 otomatik başlatma Başlangıç klasörü veya
  HKCU Run ile (Görev Zamanlayıcı'nın 72 saat sınırı uyarısı).

## Kaynaktan derleme (MSYS2 UCRT64)
- MSYS2 kurulumu, `pacman -S --needed` paket listesi (gcc, cmake, ninja, pkgconf, gtk3, json-glib,
  curl, ffmpeg, python …) — liste build alanından.
- PATH: `C:\msys64\ucrt64\bin` önce (C:\MinGW çakışma uyarısı, PLATFORM_FACTS).
- `cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=C:/msys64/ucrt64/bin/gcc.exe`
  `cmake --build build`
- Çalışma zamanı hazırlığı: `tools\radios_package.py runtime` (paketli Python + tzdata) ve
  `runtime\bin` DLL/GTK düzeni (gtk-windows-runtime alanı).

## Çalıştırma
- `run-radios.cmd` (gerekirse derler; derleme başarısızsa son sağlam ikiliyle açar;
  `RADIOS_SKIP_BUILD=1`), doğrudan `runtime\bin\RadioS.exe`.
- Seçenekler: `--version`, `--help`, `--check-config`, `--smoke-test`, `--profile <ad>`
  (broadcast, playlist, automation, dj, emergency, advert, technical, routing), `--config`, `--project-root`.
- Kısayollar (Ctrl+Q, profil sekmeleri) — README L114 sonu.

## Kullanım özeti      (README L116-130, Windows sözcükleriyle)

## Veri ve yapılandırma
- Kök keşfi (exe dizininden yukarı; işaretler `config\radios.example.ini`, `database\SDBASE\pyproject.toml`).
- `config\radios.ini` (örnekten kopya), `credentials.json` (özel ACL), `ui-preferences.ini`,
  `storage.ini`, `media-archive.ini`, `library-analysis.ini`, `audio-panel.ini`.
- `data\` tablosu → data/README.md; depolama konumları Ayarlar › Medya arşivi.

## Veri servisi (SDBASE)
- `run-database.cmd init|start|status|check|backup|stop` (PowerShell `.ps1` eşdeğeri);
  `127.0.0.1:8000`; GUI kapanınca servis sürer; `stop` nazik kapanış isteği, `--force` ayrı.
- Python: `runtime\python\python.exe` (3.14, `-E -s -B -X utf8`); PATH'teki Python kullanılmaz.
- Ayrıntı → docs/veri-servisi.md, docs/c-veri-baglantisi.md, database/SDBASE/README.md.

## Yedekleme ve kurtarma
- `runtime\python\python.exe -E -s -B -X utf8 tools\radios_reliability.py --project-root . list|create|…`
  → docs/yonetim-ve-guvenilirlik.md.

## Paketleme
- `tools\radios_package.py package --kind windows [--installer]`, `verify`; kaynak paketi
  → packaging/README.md.

## Testler
- `ctest --test-dir build --output-on-failure`; `-DRADIOS_GUI_TESTS=ON` (özel test masaüstü);
  ses testleri sanal ses arka ucuyla (fiziksel aygıt kullanılmaz); Python testleri paketli Python ile
  → tests/README.md. Fiziksel kabul → docs/fiziksel-kabul.md.

## Geliştirici araçları
- `tools\islev_envanteri.py [--check]` (envanter + OpenAPI), `tools\css_to_gtk.py` (derleme),
  `tools\gui_backend_coverage.py` (arka uç → GUI kapsamı), `tools\otomasyon_testleri\`, `tools\yayin_akisi_724\`.

## Proje düzeni   (README L171-182 tablosu + `runtime\`, `packaging\windows\`; `build\`, `dist\` üretilir)

## Belgeler       (docs dizini: mimari, c-uygulama-altyapisi, native-tasarim, veri-servisi,
                   c-veri-baglantisi, …, windows-platformu; database/SDBASE/docs)

## Lisanslar      (Inter OFL; RadioS/SDBASE için ayrı genel dağıtım lisansı tanımlanmamıştır — packaging L27)
```

### 7.2 `tests/README.md` (outline for the tests area)
`# Doğrulama` → "Hızlı başlangıç (CTest, GUI açık/kapalı, Python testleri)", "Ortam (sanal ses arka ucu,
özel test masaüstü, fixture'lar `build\` altında, üretim DB'sine yazılmaz)", then one section per area
with target names and what they check (from the current descriptive sentences): Çekirdek ve yapılandırma,
Veri istemcisi ve servis, Medya, Ses motoru ve cihazlar, DJ/DSP/geçiş, Playlist/otomasyon/planlayıcı,
Reklam, Yayın/kayıt/acil, Yönetim/kurtarma, rekordbox, Aktarım paketi, Paketleme ve başlatıcı,
Uzun süreli ses testi (from §3.3-E), Görsel inceleme (`RADIOS_SHOT_DIR`). No counts, durations or dates.

### 7.3 `docs/` index after the change (27 files)
mimari, c-uygulama-altyapisi, native-tasarim, veri-servisi, c-veri-baglantisi, medya-kutuphanesi,
medya-arsivi, medya-surukle-birak, ses-motoru, ses-yonlendirme, dj-ve-gecisler, playlist-ve-otomasyon,
otomasyon-sozluk-ve-kurallar, sema-akisi-sozlesmesi, gecis-dagilimi-sozlesmesi, reklam-yonetimi,
yayin-kayit-acil, sessizlik-korumasi, program-onayi-ve-haklar, rekordbox-ice-aktarim,
rekordbox-usb-tracklist, aktarim-paketi, yonetim-ve-guvenilirlik, fiziksel-kabul, windows-platformu
(`.md`), islev-envanteri.json, sdbase-openapi.json, source-import-manifest.json.

---------------------------------------------------------------------------------------------------------

## 8. Application procedure (ordered) and verification

1. Re-run Appendix C scripts; reconcile any file:line drift.
2. Create the extracted documents of §3.3 (A-G) from the still-present sources (copy the listed line
   ranges, then apply the listed edits). Write UTF-8 without BOM, LF (never `Set-Content`/`Out-File` in
   PowerShell 5.1 — they add a BOM/ANSI; use `[IO.File]::WriteAllText(path, text, [Text.UTF8Encoding]::new($false))`).
3. Rename (Appendix A2) with `Move-Item -LiteralPath` (or `git mv`).
4. Delete Appendix A1 paths.
5. Edit contents (§6) and rewrite README.md / tests/README.md (§7).
6. Update every pointer in §4.2/§4.3.
7. After the SDBASE `fcntl` port and the tools-area encoding fixes: regenerate
   `runtime\python\python.exe -E -s -B -X utf8 tools\islev_envanteri.py`, then run it with `--check`
   (must print "Kaynak ve sözleşme doğrulaması başarılı."). Commit the regenerated JSON with LF endings.
8. Verify:
   * link/path scan (Appendix C) reports no `DANGLING`, `TO-DROPPED-*`, `OUTSIDE-PROJECT`;
   * `rg -n -i "aşama [0-9]|[0-9]+\. aşama|0\.1[0-9]\.[0-9] kabul|pactl|parec|pulse|pipewire|wayland|x11|xvfb|flatpak|freedesktop|\.sh\b|python3|/home/|/tmp/|/veri/|pidfd|/proc|SIGTERM|SIGKILL|0600|0700|readelf|glibc|tar\.gz" README.md */README.md docs database/SDBASE/*.md database/SDBASE/docs`
     returns only intentional mentions (e.g. `.gitattributes` text, external citations);
   * `tests/test_packaging.py` (Windows-ported) passes; a fresh build regenerates
     `build\generated\reference-css.c`;
   * `ctest -R gui_backend_coverage` still passes (the stored R10.1 report is not needed).

## 9. Contracts to preserve and how tests exercise them

| Contract | Exercised by |
|---|---|
| `design/reference/tasarrım/index.html` + linked CSS → `radios_reference_css_parts[]` | build (CMake custom command), `src/ui/window.c`, GUI tests test_rekordbox_ui, test_automation_calendar, test_playlist_redesign_ui, test_media_drop_ui, test_broadcast_catalogue_ui |
| `docs/islev-envanteri.json` + `docs/sdbase-openapi.json` equal the generator output | `tools/islev_envanteri.py --check` (manual/CI; not a CTest) |
| Mandatory package files (README.md, config/README.md, database/README.md, database/SDBASE/README.md, database/SDBASE/IMPLEMENTATION_STATUS.md) and trees `docs`, `design`, `database/SDBASE/docs` | `tests/test_packaging.py` (`packaging` component), `tools/radios_package.py verify` |
| "Protected" project directories include `docs`, `design` | storage-location tests (`test_storage_locations.py`, core `storage_locations`), transfer package tests |
| OpenAPI descriptions cite `docs/api.md` | `GET /v1/openapi.json` consumers; inventory |
| Section numbering of sema-akisi-sozlesmesi.md (§1, §1b, §2, §3, §4, §5, §6) and rule letters (a)-(h) of gecis-dagilimi-sozlesmesi.md | code comments listed in §4.2 |

## 10. Cross-area dependencies and risks (summary)

* tools area: `css_to_gtk.py` and `islev_envanteri.py` encoding/newline/sort fixes (build is broken on
  Windows until then); `radios_package.py` keeps `docs`/`design`/`database/SDBASE/docs` trees and the
  README files in `COMMON_FILES`; tools/README, config/README, packaging/README Windows content;
  `otomasyon_testleri` and `yayin_akisi_724` README edits (§3.3-G, §4.2).
* SDBASE engine area: `fcntl` removal so the inventory tool can import `sdbase`; Windows CLI invocation
  wording for SDBASE docs; `.st` Windows path rules (§3.3-D).
* build area: `-X utf8` on the CSS generator command; keep `design/reference/tasarrım` paths; MSYS2
  package list and minimum FFmpeg versions for README; optional rename of `Prompt*/Pdf*` cmake files.
* tests area: tests/README rewrite; comment pointers (§4.2); `content_scale_test_input.h` replacement;
  soak-test section.
* audio areas: final file names (`wasapi_*`), device-loss/backoff behaviour and routing semantics used by
  ses-motoru.md, ses-yonlendirme.md, yonetim-ve-guvenilirlik.md.
* core area: storage protected-list comparison on Windows; Linux wording in UI string
  `src/core/storage_locations.c:68` ("mutlak olmalı (/ ile başlamalı)").
* Risks: deleting a doc that a later-written comment cites (mitigation: Appendix C scan before delete);
  stale file names in kept docs while code is still moving (verify at apply time); history creeping back
  via agent reports written into `docs/` (rule: work logs go to the scratchpad, never `docs/`).

---------------------------------------------------------------------------------------------------------

## Appendix A — mechanical lists (paths relative to the project root)

### A1. DELETE
```
docs/02-proje-duzeni.md
docs/acil-reklam-teknik-islevleri.md
docs/audio-manual-20261010.md
docs/codex-yarim-isler-2026-10-18.md
docs/gelistirme-asamalari.md
docs/gui-islev-kapanis-auditi.md
docs/kilavuz-2026-10-18-A.md
docs/kilavuz-2026-10-18-C.md
docs/kilavuz-2026-10-18-C-C1.md
docs/kilavuz-2026-10-18-C-C2.md
docs/kilavuz-2026-10-18-C-C3.md
docs/kilavuz-2026-10-18-C-C4.md
docs/kilavuz-2026-10-18-C-C5.md
docs/kilavuz-2026-10-18-C-C6.md
docs/kilavuz-2026-10-18-C-C7.md
docs/kilavuz-2026-10-18-C-C9.md
docs/kilavuz-2026-10-18-C-C10.md
docs/kilavuz-2026-10-18-C-C11.md
docs/kilavuz-2026-10-18-D-R10.1.md
docs/kilavuz-2026-10-18-matris.md
docs/otomasyon-test-raporu-2026-10-15.md
docs/pdf4-architecture.md
docs/pdf4-requirements.md
docs/playlist-otomasyon-islevleri.md
docs/proje-kapanis-denetimi.md
docs/prompt-8-uygulama.md
docs/prompt10-requirements.md
docs/prompt11-requirements.md
docs/prompt5-implementation.md
docs/prompt5-requirements.md
docs/yapilacaklar-2026-10-12.md
docs/yapilanlar-arsivi-2026-10-12-17.md
docs/yayin-dj-islevleri.md
# after extraction (A3):
docs/01-islev-haritasi.md
docs/14-nihai-kabul-ve-paketleme.md
docs/kilavuz-2026-10-18-4c.md
docs/kilavuz-2026-10-18-C-C8.md
docs/kilavuz-2026-10-18-D.md
docs/yayin-akisi-724-2026-10-17.md
# folders (after moving sozlesme.md out):
docs/kilavuz-2026-10-18-4c/
docs/effector-display/
docs/player-graphic/
docs/prompt-2/
docs/prompt-6/
docs/prompt-7/
docs/prompt-9/
docs/prompt-12/
docs/prompt-13/
docs/prompt-14/
design/prompt3-requirements.md
database/SDBASE/docs/audit_closure.md
database/SDBASE/docs/finalization_081.md
database/SDBASE/docs/gap_matrix.md
database/SDBASE/docs/load_verification.md
database/SDBASE/docs/local_manual_audit.md
database/SDBASE/docs/maturity_080.md
database/SDBASE/docs/open_source_app_verification.md
database/SDBASE/docs/requirements.md
database/SDBASE/docs/research.md
database/SDBASE/docs/storage_maturity.md
database/SDBASE/docs/verification.md
```

### A2. RENAME
```
docs/03-c-uygulama-altyapisi.md                 -> docs/c-uygulama-altyapisi.md
docs/04-native-tasarim.md                       -> docs/native-tasarim.md
docs/05-veritabani-kurulumu.md                  -> docs/veri-servisi.md
docs/06-c-veri-baglantisi.md                    -> docs/c-veri-baglantisi.md
docs/07-medya-kutuphanesi.md                    -> docs/medya-kutuphanesi.md
docs/08-ses-motoru.md                           -> docs/ses-motoru.md
docs/09-dj-ve-gecisler.md                       -> docs/dj-ve-gecisler.md
docs/10-playlist-ve-otomasyon.md                -> docs/playlist-ve-otomasyon.md
docs/11-reklam-yonetimi.md                      -> docs/reklam-yonetimi.md
docs/12-yayin-kayit-acil.md                     -> docs/yayin-kayit-acil.md
docs/13-yonetim-ve-guvenilirlik.md              -> docs/yonetim-ve-guvenilirlik.md
docs/otomasyon-sozluk-ve-kurallar-2026-10-15.md -> docs/otomasyon-sozluk-ve-kurallar.md
docs/kilavuz-2026-10-18-4c/sozlesme.md          -> docs/sema-akisi-sozlesmesi.md
```

### A3. CREATE (source ranges, see §3.3)
```
docs/mimari.md                    <- docs/01-islev-haritasi.md L5-10,L12,L38-48,L50-62,L64,L66-85,L87-100
docs/gecis-dagilimi-sozlesmesi.md <- docs/kilavuz-2026-10-18-C-C8.md L35-45(rewritten L45),L49-58
docs/aktarim-paketi.md            <- docs/kilavuz-2026-10-18-D.md L34-43,L45-59,L61-67 + facts L94,L100,L101,L115
docs/fiziksel-kabul.md            <- docs/14-nihai-kabul-ve-paketleme.md L71,L75-77,L79(edited),L81-86,L88(reworded)
docs/windows-platformu.md         <- new (DECISIONS D3-D7, PLATFORM_FACTS)
docs/playlist-ve-otomasyon.md     += subsection from docs/kilavuz-2026-10-18-4c.md L99 + docs/kilavuz-2026-10-18-D.md L134 (rule only)
tests/README.md                   += soak section from docs/14-nihai-kabul-ve-paketleme.md L63,L65-67
tools/yayin_akisi_724/README.md   += docs/yayin-akisi-724-2026-10-17.md L25-32,L34-39
```

### A4. KEEP unchanged names (content edits per §6)
```
docs/medya-arsivi.md  docs/medya-surukle-birak.md  docs/program-onayi-ve-haklar.md
docs/rekordbox-ice-aktarim.md  docs/rekordbox-usb-tracklist.md  docs/ses-yonlendirme.md
docs/sessizlik-korumasi.md  docs/islev-envanteri.json  docs/sdbase-openapi.json  docs/source-import-manifest.json
design/README.md  design/reference/tasarrım/**  (no edits)
database/SDBASE/README.md  database/SDBASE/IMPLEMENTATION_STATUS.md
database/SDBASE/docs/{adapter_coverage,api,architecture,automatic_failover,data_dictionary,disk_indexes,
  engine_operations,integrity,mairlist31_csv,new_features,online_maintenance,operations,ops_archive,
  ops_scheduler,playback_markers,portable_export,production_acceptance,radioboss_m3u8,rekordbox,
  replication,replication_stream,retention,schema_migration,snapshot_queries,wal_recovery}.md
README.md src/README.md include/README.md config/README.md assets/README.md data/README.md tests/README.md
tools/README.md cmake/README.md packaging/README.md database/README.md
```

### A5. Pointer replacements (exact strings)
```
src/ui/transfer_ui.c:9                    docs/kilavuz-2026-10-18-D.md            -> docs/aktarim-paketi.md
tests/test_transfer_ui.c:8                docs/kilavuz-2026-10-18-D.md            -> docs/aktarim-paketi.md
tests/run_transfer_ui.py:5                docs/kilavuz-2026-10-18-D.md            -> docs/aktarim-paketi.md
tests/test_live_step_wait_ui.c:41         docs/kilavuz-2026-10-18-4c/sozlesme.md  -> docs/sema-akisi-sozlesmesi.md
tests/test_live_row_ui.c:53               docs/kilavuz-2026-10-18-4c/sozlesme.md  -> docs/sema-akisi-sozlesmesi.md
tools/otomasyon_testleri/senaryolar_kume.py:3   docs/kilavuz-2026-10-18-4c/kume.md -> (scenarios defined in this file)
tools/otomasyon_testleri/README.md:65     `docs/kilavuz-2026-10-18-4c/kume.md`    -> `senaryolar_kume.py`
tools/otomasyon_testleri/senaryolar_724.py:27   docs/yayin-akisi-724-2026-10-17.md -> tools/yayin_akisi_724/README.md
tests/content_scale_test_input.h:5        docs/kilavuz-2026-10-18-C-C3.md         -> (remove; see src/ui/content_scale.c:219-234)
config/README.md:9                        ../docs/05-veritabani-kurulumu.md       -> ../docs/veri-servisi.md
database/README.md:7                      ../docs/05-veritabani-kurulumu.md       -> ../docs/veri-servisi.md
database/README.md:7                      ../docs/06-c-veri-baglantisi.md         -> ../docs/c-veri-baglantisi.md
tools/README.md:15                        ../docs/05-veritabani-kurulumu.md       -> ../docs/veri-servisi.md
tools/README.md:23                        ../docs/13-yonetim-ve-guvenilirlik.md   -> ../docs/yonetim-ve-guvenilirlik.md
tools/README.md:36                        ../docs/14-nihai-kabul-ve-paketleme.md  -> ../docs/fiziksel-kabul.md
docs/c-uygulama-altyapisi.md:3            04-native-tasarim.md                    -> native-tasarim.md
docs/playlist-ve-otomasyon.md:189         kilavuz-2026-10-18-4c/sozlesme.md       -> sema-akisi-sozlesmesi.md
docs/playlist-ve-otomasyon.md:189         kilavuz-2026-10-18-4c.md                -> gecis-dagilimi-sozlesmesi.md
```

## Appendix B — PowerShell apply skeleton (review before running; PowerShell 5.1, project root)

```powershell
$ErrorActionPreference = 'Stop'
$root = 'C:\Users\sonatturkan\Desktop\RadioS'
$utf8 = New-Object System.Text.UTF8Encoding($false)
function Lines([string]$rel) { [IO.File]::ReadAllLines((Join-Path $root $rel), [Text.Encoding]::UTF8) }
function Take([string[]]$l, [int]$from, [int]$to) { $l[($from-1)..($to-1)] }   # 1-based inclusive
function Save([string]$rel, [string[]]$body) {
  [IO.File]::WriteAllText((Join-Path $root $rel), (($body -join "`n") + "`n"), $utf8) }
# 1) extraction drafts (editing per §3.3 follows by hand)
$s = Lines 'docs\kilavuz-2026-10-18-C-C8.md'
Save 'docs\gecis-dagilimi-sozlesmesi.md' (@('# Geçiş dağılımı (settings.transitions) sözleşmesi','') + (Take $s 35 45) + @('') + (Take $s 49 58))
$s = Lines 'docs\kilavuz-2026-10-18-D.md'
Save 'docs\aktarim-paketi.md' (@('# Ayar ve medya aktarım paketi (.st)','') + (Take $s 34 43) + @('') + (Take $s 45 59) + @('') + (Take $s 61 67))
$s = Lines 'docs\01-islev-haritasi.md'
Save 'docs\mimari.md' (@('# RadioS mimarisi ve ortak sözleşmeler','') + (Take $s 5 10) + (Take $s 12 13) + (Take $s 38 48) + @('') + (Take $s 50 62) + @('') + (Take $s 64 64) + @('') + (Take $s 66 85) + @('') + (Take $s 87 100))
$s = Lines 'docs\14-nihai-kabul-ve-paketleme.md'
Save 'docs\fiziksel-kabul.md' (@('# Fiziksel ses ve saha kabulü','') + (Take $s 75 86) + @('') + (Take $s 71 71))
# 2) renames
$map = @{
 'docs\03-c-uygulama-altyapisi.md'='docs\c-uygulama-altyapisi.md'; 'docs\04-native-tasarim.md'='docs\native-tasarim.md';
 'docs\05-veritabani-kurulumu.md'='docs\veri-servisi.md'; 'docs\06-c-veri-baglantisi.md'='docs\c-veri-baglantisi.md';
 'docs\07-medya-kutuphanesi.md'='docs\medya-kutuphanesi.md'; 'docs\08-ses-motoru.md'='docs\ses-motoru.md';
 'docs\09-dj-ve-gecisler.md'='docs\dj-ve-gecisler.md'; 'docs\10-playlist-ve-otomasyon.md'='docs\playlist-ve-otomasyon.md';
 'docs\11-reklam-yonetimi.md'='docs\reklam-yonetimi.md'; 'docs\12-yayin-kayit-acil.md'='docs\yayin-kayit-acil.md';
 'docs\13-yonetim-ve-guvenilirlik.md'='docs\yonetim-ve-guvenilirlik.md';
 'docs\otomasyon-sozluk-ve-kurallar-2026-10-15.md'='docs\otomasyon-sozluk-ve-kurallar.md';
 'docs\kilavuz-2026-10-18-4c\sozlesme.md'='docs\sema-akisi-sozlesmesi.md' }
foreach ($k in $map.Keys) { Move-Item -LiteralPath (Join-Path $root $k) -Destination (Join-Path $root $map[$k]) }
# 3) deletions: iterate Appendix A1 (files with Remove-Item -LiteralPath, folders with -Recurse)
```

## Appendix C — read-only verification scripts

Kept in the scratchpad (`docs-history-work\`), reusable after renames by editing their KEEP lists:
* `scan-links.ps1` — markdown link + backticked path integrity for surviving docs (flags `DANGLING`,
  `TO-DROPPED-DOC`, `TO-DROPPED-SDBASE-DOC`, `OUTSIDE-PROJECT`, `STALE-PATH`, `BUILDREF`).
* `scan-linux.ps1` — Linux/history vocabulary per line (categories `launcher-sh`, `audio-linux`,
  `display-linux`, `sandbox-sdk`, `posix-paths`, `proc-signals`, `perm-bits`, `elf-abi`, `archive-fmt`,
  `python3`, `links-fs`, `fsync-rename`, `os-names`, `shell-syntax`, `dead-feature`).
* `scan-refs.ps1` — every surviving mention of a renamed/dropped doc name (source of A5).
* `manifest.ps1` — per-file classification with totals.
Results of the 2026-10-19 run: `linux-hits.txt`, `link-hits.txt`, `refs.txt`, `manifest.txt` in the same
folder (full path: `C:\Users\sonatturkan\AppData\Local\Temp\claude\c--Users-sonatturkan-Desktop-radyo-projesi-kaynak-kodlar-RadioS\0ead7b3c-7fb3-425d-a304-a7629cf56b77\scratchpad\docs-history-work`).
