# Area spec: gtk-windows-runtime (RadioS Windows port)

Analyzed copy: `C:\Users\sonatturkan\Desktop\RadioS` (read-only; line numbers refer to it).
Toolchain inspected: MSYS2 UCRT64 at `C:\msys64` — GTK **3.24.52**, GLib **2.90.1**, Pango **1.58.2**, cairo 1.18.6,
gdk-pixbuf **2.44.8**, librsvg 2.63.2, adwaita-icon-theme 51.0, adwaita-icon-theme-legacy 46.2, hicolor 0.18,
fontconfig 2.18.3, freetype 2.15.0 (+brotli), harfbuzz 14.6.0, ninja 1.13.2, cmake 4.4.4, MSYS2 python 3.14.8.

Binding inputs: `specs/DECISIONS.md` (D1 Windows-only, D2 fix real defects, D4 layout `runtime\`, D5 fonts) and
`specs/PLATFORM_FACTS.md`. This spec complements `specs/ui-platform-c.md` (owner of main.c/window.c/content_scale.c/
preferences.c edits); where both touch the same line this spec gives the GTK-runtime detail and the other spec owns the edit.

All **[probe]** facts were measured on the target machine with small read-only programs I compiled into
`C:\Users\SONATT~1\AppData\Local\Temp\claude\c--Users-sonatturkan-Desktop-radyo-projesi-kaynak-kodlar-RadioS\0ead7b3c-7fb3-425d-a304-a7629cf56b77\scratchpad\tools-gtkrt\`
(`probe_gtkrt.c` settings/icons/schemas/pixbuf/fallback, `probe_dpi.c` px-vs-pt scaling, `probe_fallback.c` and
`probe_scripts.c` font fallback, `probe_css.c` CSS parsing, `probe_filter.c` file-filter matching, `probe_dwm.c` DWM
attributes, `probe_i18n.c` translations, `probe_misc.c`/`probe_misc2.c` Nirmala fallback, XDG_CONFIG_DIRS and
GtkFileChooserButton without schemas, `probe_dlldirs.c` SetDefaultDllDirectories vs the SVG loader,
`glyph_audit.py` Inter cmap audit, `dll_closure.ps1` DLL closure), plus the
lead's probes in `scratchpad\probe*`. Facts marked "from GDK source" are from knowledge of the GTK 3.24 sources and
were not probed.

---

## 0. Executive summary

| # | Location | Linux construct / assumption | Windows effect | Replacement (section) |
|---|----------|------------------------------|----------------|-----------------------|
| 1 | tools/css_to_gtk.py:113,117,186,191,192; CMakeLists.txt:20-29 | `read_text()`/`write_text()` with locale encoding; PATH/registry Python | **Build fails**: MSYS2 Python 3.14 has `utf8_mode=0`, preferred encoding **cp1254** → `UnicodeDecodeError: byte 0x9e in position 14543` reading `index.html` **[probe]**; CMake FindPython may pick the user's PyManager 3.15 | explicit `encoding="utf-8", newline="\n"`, `-X utf8 -B`, pin `Python3_EXECUTABLE` (§3) |
| 2 | src/ui/window.c:23,735,748-751; tests/test_automation_calendar.c:10,3045-3046; tests/test_media_drop_ui.c:7,775-776; CMakeLists.txt:10,98 | `FcInit()` + `FcConfigAppFontAddFile(InterVariable.woff2)` | Pango on Windows = DirectWrite; the call "succeeds" but the font is never used (Tahoma) | `radios_ui_register_app_font()` → `pango_font_map_add_font_file(InterVariable.ttf)`; drop fontconfig link (§5, D5) |
| 3 | assets/themes/radios.css:4; src/ui/preferences.c:72 | `"Inter Variable", "Inter", sans-serif` | DirectWrite family is **"Inter Variable Text"**; Inter lacks 15 UI symbols (▾ ▸ ⏭ ⏮ ⚙ ✎ ✕ ⋯ ⠿ ⇄ ⇈ ⇊ ▤ ▦ ＋) → 4 tofu boxes + Lucida/MS Gothic fallback and +5 px line height **[probe]** | canonical family list with Segoe UI Symbol/Emoji/Segoe UI fallbacks (§5.4) |
| 4 | src/ui/routing_graph_ui.c:69; automation_calendar.c:605; player_waveform.c:795 | Pango `"Inter, Sans"`, `"Sans 10"`, `"Sans 8"` | Tahoma; pt sizes grow with DPI while the px UI does not | `radios_ui_font_description_new(px, weight)` (§5.5) |
| 5 | instruments.c:311,394; routing.c:805; autodj_automation_ui.c:245; emergency.c:1265,1270 | cairo toy text, `"sans-serif"`/default | cairo-DWrite toy face → **Arial** | optional Pango helper (§5.6) |
| 6 | assets/themes/dj-pedals.css:85,95,107 | `font-family: monospace` | Courier New **[probe]** | `"Cascadia Mono", Consolas, monospace` (§5.7) |
| 7 | (runtime) | system GTK data in /usr | nothing is installed on Windows; without schemas the first GtkFileChooserButton aborts the process, without hicolor every icon miss is a fatal-in-tests Gtk-WARNING, without the SVG loader all symbolic icons vanish **[probe]** | bundle exact runtime (§10): DLL closure, SVG pixbuf loader + `loaders.cache`, compiled schemas, Adwaita/AdwaitaLegacy/hicolor, `tr` .mo, settings.ini; fail-fast self-check (§9.2) |
| 8 | (runtime) | XDG dirs fixed by distro | GLib default data dirs put `C:\ProgramData` and `C:\Users\Public\Documents` **before** the bundle **[probe]**; user env (GTK_THEME, GTK_CSD, PANGOCAIRO_BACKEND, GTK_PATH, GTK_MODULES …) leaks in | `radios_ui_runtime_prepare_environment()` (§9) |
| 9 | (runtime, D3) | ld.so search | after `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)` PATH is ignored for `LoadLibrary`: with GTK DLLs not in the exe dir (dev/tests) the SVG loader fails ("Unable to load image-loading module … Belirtilen modül bulunamadı") and GTK warns "Could not load a pixbuf from icon theme" (fatal in tests) **[probe_dlldirs]** | `AddDllDirectory(<dir of libglib-2.0-0.dll>)` in platform init – fixes it **[probe_dlldirs]** (§9.3) |
| 10 | window.c:765 + GTK defaults | XSettings | Windows title bar stays **light** on a black app; `gtk-error-bell` → `MessageBeep` on the default audio device (can reach the on-air device) | DWM dark caption hook; code-applied GtkSettings (§8, §11.3) |
| 11 | editors.c:1242; dj.c:1194; playlist.c:2061 | `gtk_file_filter_add_mime_type("audio/*"/"application/xml")` | on Win32 the mime rule **never matches** (`audio/mpeg` → 0) **[probe]** and forces GtkFileChooserButton off the native IFileDialog | pattern-only filters (§13) |
| 12 | preferences.c:918 | undecorated settings window | cannot be moved on Windows (no Alt+drag WM convenience) | drag-to-move on the custom title bar (§11.4) |
| 13 | content_scale.c:3-5,380-382,457-490 | X11-only synchronous popup re-anchoring | popups misplaced in zoomed content | enable for `GDK_IS_WIN32_DISPLAY` (`moved-to-rect` fires synchronously on Win32 **[probe]**) — edit owned by ui-platform-c §7 (§12.4) |
| 14 | HiDPI | Xft.dpi / GNOME scale | GDK uses **integer** scale (dpi/96); CSS **px fonts stay device px at 125–175 %** while **pt fonts scale** **[probe]** | documented policy + pt→px conversions (§12) |
| 15 | tests (≈45 CMake ENVIRONMENT entries, runners) | Xvfb, `GDK_BACKEND=x11`, XTest, DISPLAY checks | cannot open a display | Windows GUI test env + interactive-desktop check (§15) |
| 16 | exe | no window icon on Linux | GDK uses the exe's first icon resource | `assets/icons/radios.ico` + `.rc` (§11.5) |

No GtkSocket/GtkPlug, no printing, no Wayland/X11 calls outside content_scale.c (guarded), no GSettings use by the app
itself (only GTK-internal), no headerbars/CSD, no custom cursors besides `"grab"` (§14). Clipboard: one call (§16).

---

## 1. Internal contracts that must be preserved

* **Reference CSS array**: `extern const gchar *const radios_reference_css_parts[]` (window.c:26) — generated
  `${CMAKE_BINARY_DIR}/generated/reference-css.c`, NULL-terminated array of ≤2000-char UTF-8 string chunks
  (css_to_gtk.py:187-190). Consumers: window.c:738-739 and tests test_automation_calendar.c:22,3040,
  test_broadcast_catalogue_ui.c:873,878, test_media_drop_ui.c:767-769, test_playlist_redesign_ui.c:359-362,
  test_rekordbox_ui.c:40,47. Byproducts `reference.css`, `css-manifest.json` (sources + sha256 + rule count).
  Current output: 31 sources, 1572 rules **[probe, run with -X utf8]**; contains **no font-family, no non-ASCII**.
* **Main theme provider** (window.c:734-770): `reference CSS + assets/themes/radios.css` concatenated into ONE provider,
  `GTK_STYLE_PROVIDER_PRIORITY_APPLICATION`, added for the screen; `ui->css_source` kept for the text-size regex
  rewrite (window.c:131-161: `font-size: N px` × {0.90, 1.0, 1.15}, `g_ascii_formatd` – locale-safe).
* **Font failure semantics** (window.c:748-751): if the design font cannot be registered, `radios_window_new` fails with
  `G_IO_ERROR_INVALID_DATA` "Tasarımın Inter fontu yüklenemedi." and the app exits 1 (main.c:67-72). Keep.
* **Dark theme**: `gtk-application-prefer-dark-theme = TRUE` (window.c:765; tests set it too); smoke test sets
  `gtk-enable-animations = FALSE` (window.c:766-767).
* **Other providers** (all keep their priorities): automation_calendar.c:2807-2811 (APPLICATION+2, file),
  dj.c:707-719 (APPLICATION+1, `project_root/assets/themes/dj-pedals.css`), header_status.c:553-574 (APPLICATION, file +
  data), routing_graph_ui.c:466-468 (file), preferences.c:926-934 (APPLICATION+30), playlist.c:806-856 (+2/+3),
  probability_editor.c:1029-1030 (+10), transfer_ui.c:90-108 (APPLICATION, +31), programme_queue.c:1448-1495 (+20,
  widget providers), player_waveform.c:975-996, broadcast.c:7296-7303, broadcast_output_ui.c:461-469,
  autodj_automation_ui.c:890-894, automation_workspace_ui.c:83-85, player_tree_ui.c:383-391, routing.c:840-848,
  rotation_editor.c:2710-2732. All parse with **0 errors** on GTK 3.24.52/Win32 for the file-based ones **[probe_css]**.
  User CSS `%LOCALAPPDATA%\gtk-3.0\gtk.css` has USER priority (800) and overrides all of these — same as Linux
  `~/.config/gtk-3.0/gtk.css`; accepted.
* **Icon names** used by the app: `document-open-recent-symbolic` (broadcast.c:3073), `emblem-system-symbolic`
  (preferences.c:1013), `audio-x-generic-symbolic`, `folder-symbolic`, `media-playback-start-symbolic`
  (player_tree_ui.c:145,156 via cell `icon-name`, 429-430), CSS `-gtk-icontheme('media-record-symbolic')`
  (preferences.c:115). All resolve to Adwaita 51 SVGs and load **[probe_gtkrt]**.
* **Widget names tests rely on** for GTK file choosers (do not switch these to GtkFileChooserNative):
  `media-import-chooser` (media.c:1032), `media-folder-chooser` (media.c:1195), `advert-export-dialog`
  (advert_editors.c:789), `pw-export-dialog` (playlist.c ~1332), `import-profile-xml-file` (import_profiles_ui.c:402),
  `transfer-*` buttons (transfer_ui.c:677,760), `transition-file-a/b` (editors.c:1239), `workflow-input-*`
  (workflow_hub.c:383). Tests: test_data_ui.c:514-550,1112-1131; test_media_drop_ui.c:666-739; test_advert_ui.c:421-422;
  test_playlist_export_ui.c:29-47; test_rekordbox_ui.c:164-294; test_transfer_ui.c:105-577; test_import_profiles_ui.c:
  106,187; test_ui.c:651-663; test_workflow_hub_ui.c:180-181.
* **Content scale** (content_scale.h API, CSS node `radioscontentscale`, test seam env
  `RADIOS_CONTENT_SCALE_POPUP_MAP_ONLY=1`, content_scale.c:235,483) unchanged.

---

## 2. Files in this area

| File | Role |
|------|------|
| tools/css_to_gtk.py | design CSS → GTK CSS + C array generator (build time) |
| CMakeLists.txt:20-29, 57 | Python discovery, custom command, `${REFERENCE_CSS_C}` in radios_ui |
| assets/themes/radios.css, automation-calendar.css, dj-pedals.css, routing-graph.css, header-status.css | runtime CSS |
| assets/fonts/InterVariable.woff2 (design source), **InterVariable.ttf** (879 708 bytes, official Inter 4.1, runtime), LICENSE.txt | font |
| assets/icons/ (empty) | app icon to be added |
| src/ui/window.c, src/app/main.c | theme/font/runtime init call sites |
| new src/ui/ui_runtime.c + include/radios/ui_runtime.h | Windows GTK runtime layer (this spec) |

---

## 3. CSS generation pipeline

### 3.1 What it does
`css_to_gtk.py` reads `design/reference/tasarrım/index.html` (inline `<style>` + `<link href=*.css>` order), strips comments
and all `@…{}` blocks (media queries), flattens custom properties that have one literal value, keeps a whitelist of
properties (lines 16-24; **font-family is not whitelisted**), maps HTML element selectors to GTK nodes, prefixes `.radios`,
converts `#rrggbbaa` to `rgba()`, expands multi-position gradient stops, rounds numeric `font-weight` to multiples of 100,
writes `reference.css`, `reference-css.c` (JSON-escaped 2000-char chunks, `ensure_ascii=False`) and `css-manifest.json`.

### 3.2 Windows defects
1. **Encoding (build blocker)**: `Path.read_text()` (113, 117) and `write_text()` (186, 191, 192) use
   `locale.getpreferredencoding()` = cp1254. index.html (4985 non-ASCII bytes) and live-desk.css are not decodable in
   cp1254 → `UnicodeDecodeError` **[probe: exit 1]**; with `-X utf8` it succeeds (31 sources, 1572 rules) **[probe]**.
   Even when decoding "succeeds" (other code pages) the text is mojibake and `sha256(source.encode())` (138) differs from
   Linux.
2. **Line endings**: text-mode writes emit CRLF in all three outputs **[probe]**. Harmless for GCC/GTK but makes
   outputs differ from Linux; use `newline="\n"`.
3. **Interpreter**: `find_package(Python3 COMPONENTS Interpreter REQUIRED)` (CMakeLists.txt:20) on Windows searches the
   registry first → may find the user's PyManager CPython 3.15 (PLATFORM_FACTS). 3.15 would hide defect 1 (UTF-8 mode
   default) but violates the toolchain rule.
4. **Non-ASCII path** `design/reference/tasarrım`: fine — Python uses wide APIs; ninja 1.13.2 carries an
   `activeCodePage UTF-8` manifest and `ninja -t wincodepage` prints "Build file encoding: UTF-8" **[probe]**, so CMake
   writes build.ninja in UTF-8 and `DEPENDS …/tasarrım/index.html` resolves on any system ACP. (gtk-update-icon-cache and
   gdk-pixbuf-query-loaders have no UTF-8 manifest – keep staging paths ASCII.)

### 3.3 Exact changes
```python
# tools/css_to_gtk.py
html = (DESIGN / "index.html").read_text(encoding="utf-8")                       # :113
sources.append((path.name, path.read_text(encoding="utf-8")))                     # :117
source_records.append({"source": name, "sha256": hashlib.sha256(source.encode("utf-8")).hexdigest()})  # :138
(args.output_dir / 'reference.css').write_text(css, encoding="utf-8", newline="\n")                    # :186
(args.output_dir / 'reference-css.c').write_text('\n'.join(lines) + '\n', encoding="utf-8", newline="\n")  # :191
(args.output_dir / 'css-manifest.json').write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + '\n',
                                                   encoding="utf-8", newline="\n")                     # :192
```
```cmake
# CMakeLists.txt (configure with -DPython3_EXECUTABLE=C:/msys64/ucrt64/bin/python.exe, or:)
set(Python3_FIND_REGISTRY NEVER)            # before find_package(Python3) – registry holds PyManager 3.15
add_custom_command(OUTPUT "${REFERENCE_CSS_C}"
    COMMAND ${Python3_EXECUTABLE} -X utf8 -B "${CMAKE_SOURCE_DIR}/tools/css_to_gtk.py"
            --output-dir "${CMAKE_BINARY_DIR}/generated"
    DEPENDS ${DESIGN_CSS} tools/css_to_gtk.py "design/reference/tasarrım/index.html"
    BYPRODUCTS "${CMAKE_BINARY_DIR}/generated/reference.css" "${CMAKE_BINARY_DIR}/generated/css-manifest.json"
    VERBATIM)
```
`-B` prevents `tools/__pycache__` inside the source tree. GCC reads the generated C as UTF-8 (default input charset);
current output is pure ASCII anyway.

### 3.4 Tests
New CTest `css_generator_encoding` (Python, run with the MSYS2 interpreter **without** `-X utf8` so the script itself is
proven encoding-safe): generate into a temp dir twice → byte-identical outputs, LF only, `native_rules == 1572` (or
"> 0" to tolerate design updates), every manifest sha256 equals `sha256(file bytes)` of the source file. Optional
`.gitattributes` (`design/** text eol=lf`, `assets/fonts/* -text`) if the tree becomes a git checkout (CRLF checkouts
would change the manifest digests; GTK and the regexes tolerate CR).

---

## 4. Runtime CSS on Windows — behaviour differences

| Property / feature | Windows GTK 3.24 behaviour | Action |
|---|---|---|
| `font-family` | resolved by Pango-DirectWrite: DirectWrite family names ("Inter Variable Text"); generic aliases: `sans-serif`/`Sans` → Tahoma (DejaVu first if installed), `monospace` → Courier New; **no automatic system fallback** – only the families in the list and their built-in alias chains are searched **[probe]** | §5.4 |
| `font-size: Npx` | device pixels, **independent of DPI**: at font resolution 144 a `14px` label keeps 102×18 natural size; GTK converts px→pt with the current resolution (computed 7 pt @144) **[probe_dpi]** | §12 |
| `font: Npt` / Pango "Sans 10" / gtk-font-name "Segoe UI 9" | scale with the system DPI (10 pt label 47×17 → 73×25 at 144) **[probe_dpi]** | §12 |
| `font-weight` 100…900 | snapped to the DirectWrite named instances (Thin, Ultra-Light, Light, Normal, Medium, Semi-Bold, Bold, Ultra-Bold, Heavy); app uses only 100,200,300,400,500,600,700 **[probe: generated CSS]** | none |
| `font-style: italic` | not used (0 occurrences) – would be synthesized oblique (no InterVariable-Italic.ttf shipped) | none |
| `letter-spacing` (radios.css:180) | supported (Pango attribute) | none |
| `-gtk-icontheme()` (preferences.c:115) | needs bundled Adwaita + SVG pixbuf loader | §6, §7 |
| `box-shadow`, `border-radius` on menus/tooltips/combo popups | drawn into per-pixel-alpha popup windows (`UpdateLayeredWindow`, present in libgdk) – fine with DWM | none |
| Text antialiasing | main content is rendered into the RadioSContentScale **offscreen ARGB surface** → grayscale AA; dialogs/menus drawn directly → ClearType (`gtk-xft-rgba "rgb"` from the system) | accepted (visual only) |
| `:backdrop`, transitions, animations | same semantics; `gtk-enable-animations` TRUE by default | none |
| HighContrast | GDK returns `gtk-theme-name "HighContrast"` when Windows high contrast is on (overrides settings.ini) | keep (accessibility); app CSS still wins at APPLICATION priority |
| User/global CSS | `%LOCALAPPDATA%\gtk-3.0\gtk.css` (USER priority) | accepted (parity) |

All file CSS (`reference.css` + 5 theme files) parse with 0 GTK errors **[probe_css]**.

---

## 5. Fonts (decision D5, refined by measurements)

### 5.1 Inventory of every font reference
| file:line | code | Windows result today |
|---|---|---|
| window.c:23 | `#include <fontconfig/fontconfig.h>` | remove |
| window.c:735, 748-751 | `FcInit()` + `FcConfigAppFontAddFile(NULL, InterVariable.woff2)` | returns TRUE but Pango never sees it |
| tests/test_automation_calendar.c:10,3045-3046; tests/test_media_drop_ui.c:7,775-776 | same in test harnesses | same |
| CMakeLists.txt:10, 98 | `pkg_check_modules(FONTCONFIG …)`, `PRIVATE PkgConfig::FONTCONFIG` | remove (libfontconfig-1.dll stays in the bundle as a dependency of cairo/pangoft2/librsvg) |
| assets/themes/radios.css:4 | `.radios { font-family: "Inter Variable", "Inter", sans-serif; font-size: 14px; }` | Tahoma |
| src/ui/preferences.c:72 | `#preferences-window, #preferences-driver-dialog { … font-family: 'Inter Variable', 'Inter', sans-serif; …}` | Tahoma |
| src/ui/routing_graph_ui.c:69 | `pango_font_description_set_family(font, "Inter, Sans")` (absolute px size :70) | Tahoma |
| src/ui/automation_calendar.c:605 | `pango_font_description_from_string("Sans 10")` (calendar block text) | Tahoma, DPI-scaled |
| src/ui/player_waveform.c:795 | `"Sans 8"` (waveform marker labels) | Tahoma, DPI-scaled |
| src/ui/editors.c:333 | `gtk_widget_create_pango_layout` (widget font) | inherits CSS – OK after §5.4 |
| assets/themes/dj-pedals.css:85,95,107 | `font-family: monospace` (fx LCD) | Courier New |
| instruments.c:311-320 (meter scale), 394-401 (clock numerals); routing.c:805-814; autodj_automation_ui.c:245-246; emergency.c:1265,1270 | cairo toy API | Arial (cairo DWrite toy default) |
| tools/css_to_gtk.py | does not emit font-family | none |
| src/ui/preferences.c:628 | license entry "Inter — SIL Open Font License 1.1" → assets/fonts/LICENSE.txt | keep |

### 5.2 Measured facts
* `pango_cairo_font_map_get_default()` is `PangoCairoWin32FontMap`; libpangowin32 1.58.2 exports
  `pango_win32_font_map_add_font_file` / `pango_win32_dwrite_add_font_file` (IDWriteFontSetBuilder1) and libpangocairo
  renders through `cairo_dwrite_font_face_create_for_dwrite_fontface` **[DLL strings]**.
* TTF accepted, WOFF2 rejected ("not supported by DirectWrite"); family exposed as **"Inter Variable Text"**; "Inter
  Variable"/"Inter" alone → Tahoma (lead probe, re-run **[probe_pango3]**).
* The fallback warning `couldn't load font "… Not-Rotated 14px", falling back to "Sans …"` is printed to stderr without a
  GLib log domain → **not** fatal under `G_DEBUG=fatal-warnings` **[probe]**, but it is log noise: every family list must
  contain at least one resolvable family.
* No fontconfig initialisation happens anywhere in GTK/librsvg/Pango-win32 at runtime (`FONTCONFIG_FILE` pointed to a
  missing file + `FC_DEBUG`: zero output) **[probe]** → **no `etc\fonts\fonts.conf` is needed**.
* Inter 4.1 variable TTF: tables include `fvar gvar STAT avar`; nameID1 "Inter Variable"; 2852 code points
  **[glyph_audit.py]**. Of 61 distinct non-ASCII characters in src string literals, 15 are missing:
  U+21C4 ⇄, U+21C8 ⇈, U+21CA ⇊ (broadcast.c), U+22EF ⋯ (dj.c, playlist.c), U+23ED ⏭ (header_navigation.c,
  playout_ui.c), U+23EE ⏮ (header_navigation.c), U+25A4 ▤ (broadcast.c), U+25A6 ▦ (automation.c), U+25B8 ▸ (broadcast.c,
  transfer_ui.c), **U+25BE ▾ (13 uses: autodj_automation_ui.c, broadcast.c, header_navigation.c, media.c, midi_ui.c,
  rotation_editor.c, transfer_ui.c, transition_distribution.c, widgets.c)**, U+2699 ⚙ (broadcast.c), U+270E ✎
  (broadcast.c, routing_graph_ui.c), U+2715 ✕ (programme_queue.c), U+283F ⠿ (dj.c), U+FF0B ＋ (broadcast.c).

### 5.3 Font registration API (new, radios_ui)
Pango is a radios_ui dependency, so this lives in `src/ui/ui_runtime.c` (not in radios_core's platform.c; ui-platform-c
calls it `radios_platform_register_app_font` – same function, this name/location is preferred to keep Pango out of
radios_core):
```c
/* include/radios/ui_runtime.h */
#define RADIOS_UI_FONT_FAMILIES \
    "Inter Variable Text, Inter Variable, Inter, Segoe UI Symbol, Segoe UI Emoji, Segoe UI, Nirmala UI, sans-serif"
gboolean radios_ui_register_app_font(const gchar *assets_root, GError **error);
PangoFontDescription *radios_ui_font_description_new(gdouble pixel_size, PangoWeight weight);
```
Semantics:
* Path `<assets_root>\fonts\InterVariable.ttf` (`g_build_filename`). Call
  `pango_font_map_add_font_file(pango_cairo_font_map_get_default(), path, &inner)` on the **GTK (main) thread before any
  widget is created** (the default font map is per thread). Idempotent: a static `gboolean registered` (per process)
  returns TRUE on repeated calls (tests call it several times).
* Verify: `pango_font_map_load_font(map, ctx, "Inter Variable Text 14px")` and check
  `pango_font_description_get_family(pango_font_describe(font)) == "Inter Variable Text"`; otherwise fail.
* Failure → `g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Tasarımın Inter fontu yüklenemedi: %s", inner->message)`
  (window.c keeps aborting the window creation).
* window.c:735 and 748-751 become one call `ready = ready && radios_ui_register_app_font(config->assets_root, error);`
  and window.c:23 include is removed. Tests test_automation_calendar.c:3045-3046 and test_media_drop_ui.c:775-776 call
  `g_assert_true(radios_ui_register_app_font(config.assets_root, &error))`.
* `radios_ui_font_description_new(px, weight)`: `pango_font_description_from_string(RADIOS_UI_FONT_FAMILIES)`,
  `pango_font_description_set_absolute_size(d, px * PANGO_SCALE)`, `set_weight(weight)`.
* Requirements: Pango ≥ 1.56 (have 1.58.2), Windows 10 1703+ (IDWriteFactory5/FontSetBuilder1).
* Packaging: InterVariable.ttf must be in the assets allow-list (`tools/radios_package.py:34` has only `.woff2`) and in
  tests/test_packaging.py:35. The WOFF2 stays as the design source (not loaded).
* Emergency fallback (not default): `g_setenv("PANGOCAIRO_BACKEND","fc",TRUE)` before `gtk_init` +
  `pango_font_map_add_font_file` on the PangoFcFontMap + a bundled `etc\fonts\fonts.conf` (Appendix C). The lead's
  probe found the fc path unreliable (Verdana) and it costs a first-run scan of `C:\Windows\Fonts`; only re-evaluate if a
  DirectWrite registration failure is ever observed in the field.

### 5.4 Canonical family list (measured)
Per-run results at 14 px for "Otomasyon ▾" and the 15 missing symbols **[probe_fallback]**:

| list | ▾ ▸ source | ⏮⏭⚙⠿ | unknown glyphs | line height (plain 18 px) |
|---|---|---|---|---|
| `Inter Variable Text, Inter Variable, Inter, sans-serif` | Lucida Sans Unicode | **tofu** | 4 | **23** |
| `… Inter, Segoe UI Symbol, Segoe UI Emoji, Segoe UI, sans-serif` | Segoe UI Symbol | Segoe UI Symbol | 0 | 20 |
| `… Inter, Segoe UI, Segoe UI Symbol, Segoe UI Emoji, sans-serif` | MS UI Gothic (Segoe UI alias chain) | Segoe UI Symbol | 0 | 18 / 20 |

Decision: **Segoe UI Symbol first** (core font on every Windows 10/11 install; MS UI Gothic is an East-Asian
supplemental font and may be absent) – deterministic glyphs, +2 px only for lines containing symbols.
User metadata (track titles) in other scripts with that list **[probe_scripts]**: Turkish/Cyrillic/Greek → Inter;
Hebrew/Arabic → Segoe UI; 東京 → MS UI Gothic; 서울 → Malgun Gothic; 🔥 → Segoe UI Emoji (color); Devanagari unknown →
add **Nirmala UI** (then Devanagari → Nirmala UI, Thai → Tahoma, 0 unknown glyphs **[probe_misc]**). (Without the
extension Hebrew/Arabic fell to Tahoma and Korean was tofu.)

Exact edits:
```css
/* assets/themes/radios.css:4 */
.radios { font-family: "Inter Variable Text", "Inter Variable", "Inter", "Segoe UI Symbol", "Segoe UI Emoji",
          "Segoe UI", "Nirmala UI", sans-serif; font-size: 14px; }
```
`src/ui/preferences.c:72`: same list inside the string (single quotes). No generated CSS needs a change.
Menus/tooltips/dialogs without the `radios` class use GTK's `gtk-font-name` = "Segoe UI 9" (system font, pt) – parity
with Linux (desktop font); not overridden.

### 5.5 Pango code in custom drawing
* routing_graph_ui.c:68-70: replace `pango_font_description_new()+set_family("Inter, Sans")+set_absolute_size` with
  `radios_ui_font_description_new(size, bold ? PANGO_WEIGHT_SEMIBOLD : PANGO_WEIGHT_NORMAL)`.
* automation_calendar.c:605 `"Sans 10"` → `radios_ui_font_description_new(13.333, PANGO_WEIGHT_NORMAL)`;
  player_waveform.c:795 `"Sans 8"` → `radios_ui_font_description_new(10.667, PANGO_WEIGHT_NORMAL)`. Absolute px keeps the
  Linux 96-dpi look and stops these drawn labels from growing 1.5× at 150 % while their boxes stay 1× (§12).
* `pango_cairo_create_layout(cr)` (routing_graph_ui.c:67) uses the thread-default font map → sees the registered font
  only on the GTK thread (true today).

### 5.6 Cairo toy text (optional, design fidelity)
On Windows the toy face resolves to Arial (cairo DWrite `CAIRO_FONT_FAMILY_DEFAULT`), on Linux it was the fontconfig
sans – both "system sans", so leaving it is acceptable. Recommended: helper in widgets.c
`gdouble radios_ui_cairo_show_text(cairo_t *cr, const gchar *text, gdouble px, PangoWeight weight, gboolean measure_only)`
(pango_cairo layout with `radios_ui_font_description_new`, returns logical width; caller positions with
`cairo_move_to` at the baseline − `pango_layout_get_baseline/PANGO_SCALE`). Sites: instruments.c:311-320 (9 px meter
scale, "−40" uses U+2212), 394-401 (9 px clock numerals), routing.c:805-814 (10 px), autodj_automation_ui.c:245-246
(12 px, "Çıkan parça"), emergency.c:1265,1270 (default 10 px). Toy text never falls back for missing glyphs.

### 5.7 Monospace
dj-pedals.css:85,95,107 → `font-family: "Cascadia Mono", Consolas, monospace;` (Consolas present on all Windows 10/11;
Cascadia Mono resolved on this machine though not in C:\Windows\Fonts – per-user install) **[probe]**. Plain
`monospace` gives Courier New.

---

## 6. Icons

### 6.1 Names
App names (§1) + GTK-internal names used by the widgets the app instantiates (GtkSearchEntry `edit-find/edit-clear-symbolic`,
GtkSpinButton `list-add/remove-symbolic`, combo/expander/treeview `pan-*-symbolic`, menus `open-menu-symbolic`, message
dialogs `dialog-*-symbolic`, checks `object-select-symbolic`, file chooser places/devices/mimetypes: `user-home-symbolic`,
`drive-harddisk-symbolic`, `folder`, `text-x-generic`, `audio-x-generic`, …). All found in MSYS2 Adwaita 51 and loaded
**[probe_gtkrt]**; `emblem-system-symbolic` lives in `Adwaita\symbolic\legacy`.

### 6.2 Theme chain and what to ship
`Adwaita\index.theme`: `Inherits=AdwaitaLegacy,hicolor`. Ship:
* `share\icons\Adwaita\` – `index.theme`, `icon-theme.cache`, `16x16\`, `scalable\`, `symbolic\` (≈1.0 MB, 717 SVG +
  51 PNG). **Do not ship `cursors\`** (4.0 MB of .cur/.ani): GDK-Win32 uses Windows system cursors while
  `gtk-cursor-theme-name` is NULL (default) – native look; all needed names resolve **[probe]** (§14).
* `share\icons\AdwaitaLegacy\` – complete (2.2 MB, 1810 PNG; legacy full-colour names used by the file chooser/mime icons).
* `share\icons\hicolor\index.theme` (+ regenerated empty `icon-theme.cache`); skip the gtk3-demo PNGs.
  **Missing hicolor** produces `Gtk-WARNING: Could not find the icon '…'. The 'hicolor' theme was not found either`
  (fatal in tests) **[probe: empty XDG_DATA_DIRS]**.
* Regenerate caches as the **last** staging step: `gtk-update-icon-cache.exe -q -f <prefix>\share\icons\{Adwaita,AdwaitaLegacy,hicolor}`.
  GTK ignores a cache older than its theme directory (gtkiconcache: cache mtime < dir mtime) and then scans the
  directories – functionally identical, slightly slower. Installers that recreate directories make caches "stale";
  acceptable (optional: ship gtk-update-icon-cache.exe and run it post-install).
* Without SVG support GTK silently skips SVG icons: `document-open-recent-symbolic` "not found", only a few GTK-internal
  names come from built-in resource PNGs (`/org/gtk/libgtk/icons/…`) **[probe: empty loaders.cache]** → §7 self-check.
* Search path order on Windows **[probe]**: `XDG_DATA_HOME\icons` (=%LOCALAPPDATA%\icons), `%USERPROFILE%\.icons`,
  then system data dirs (`C:\ProgramData\icons`, `C:\Users\Public\Documents\icons`, `<exe dir>\share\icons`,
  `<GTK prefix>\share\icons`). Foreign themes named "Adwaita" in the first dirs would shadow ours → §9 sets
  `XDG_DATA_DIRS`.

---

## 7. GdkPixbuf loaders

* gdk-pixbuf 2.44.8 in MSYS2 has **built-in** loaders (ani png bmp gif ico jpeg pnm tiff xpm xbm tga icns qtif + GDI+
  wmf/emf; libgdk_pixbuf imports libpng16/libjpeg-8/libtiff-6/gdiplus) **[probe: formats with an empty cache]**.
  Only **SVG** needs an external module: `lib\gdk-pixbuf-2.0\2.10.0\loaders\pixbufloader_svg.dll` (1 776 626 bytes,
  librsvg 2.63.2). The other `libpixbufloader-*.dll` are redundant – do not ship.
* `loaders.cache` location: `GDK_PIXBUF_MODULE_FILE` or `<gdk-pixbuf DLL toplevel>\lib\gdk-pixbuf-2.0\2.10.0\loaders.cache`;
  relative module paths are resolved against the toplevel (DLL dir minus `bin`) – relative, absolute and forward-slash
  forms all work **[probe]**. Ship exactly (LF line endings):
```
# GdkPixbuf Image Loader Modules file
# Automatically generated file, do not edit
# Created by gdk-pixbuf-query-loaders from gdk-pixbuf-2.44.8
#
"lib\\gdk-pixbuf-2.0\\2.10.0\\loaders\\pixbufloader_svg.dll"
"svg" 6 "gdk-pixbuf" "Scalable Vector Graphics" "LGPL"
"image/svg+xml" "image/svg" "image/svg-xml" "image/vnd.adobe.svg+xml" "text/xml-svg" "image/svg+xml-compressed" ""
"svg" "svgz" "svg.gz" ""
" <svg" "*    " 100
" <!DOCTYPE svg" "*             " 100

```
* **Pitfall [probe]**: a module block is only registered when it is terminated by an empty line; a file ending right
  after the last pattern line silently drops SVG (no warning) → all symbolic icons disappear. Generate the block by
  copying it from MSYS2's `C:\msys64\ucrt64\lib\gdk-pixbuf-2.0\2.10.0\loaders.cache` (it is already relative) or with
  `gdk-pixbuf-query-loaders.exe <prefix>\lib\gdk-pixbuf-2.0\2.10.0\loaders\pixbufloader_svg.dll` run **from the staged
  bin** (otherwise it writes an absolute MSYS2 path); always validate (below).
* A **missing** cache file is also silent in gdk-pixbuf 2.44.8 (no warning, exit 0, SVG simply unavailable)
  **[probe: GDK_PIXBUF_MODULE_FILE → nonexistent file]** – older gdk-pixbuf warned; do not rely on a warning.
* Runtime self-check (`radios_ui_runtime_init`, §9.2): `gdk_pixbuf_get_formats()` must contain a format named "svg";
  otherwise error "Simge yükleyicisi (SVG) bulunamadı: <path of loaders.cache>".
* pixbufloader_svg.dll dependencies (librsvg-2-2, libxml2-16, libcairo-2, pango*, fontconfig, freetype, harfbuzz, …)
  must be in `bin\` and must be findable by `LoadLibraryW` after `SetDefaultDllDirectories` (§9.3).
* App images: the app draws its own pictures (playlist.c:303-317 `gdk_pixbuf_get_from_surface`); PNG export uses cairo
  (window.c:662) – no loader needed.

---

## 8. GSettings schemas and GtkSettings

### 8.1 Schemas
The app never calls `g_settings_new`; GTK does: GtkFileChooserWidget (`org.gtk.Settings.FileChooser`), GtkColorChooser,
GtkEmojiChooser (GtkEntry context menu / Ctrl+. / Ctrl+;), inspector keybinding (`org.gtk.Settings.Debug`).
**Measured [probe_misc2, no schemas]: merely constructing a `GtkFileChooserButton` aborts the process**
(`GLib-GIO-ERROR **: No GSettings schemas are installed on the system`, exit 0xC0000409) – because the button wraps a
GtkFileChooserNative that immediately creates a hidden GtkFileChooserDialog. So a missing schema crashes RadioS as soon
as the transfer page, playlist import dialog, transition editor, DJ pad editor, workflow hub or import-profile dialog
is built, not only when a dialog is opened. Ship `share\glib-2.0\schemas\` with the four XML files from MSYS2
(`org.gtk.Settings.{FileChooser,ColorChooser,EmojiChooser,Debug}.gschema.xml`) and
`glib-compile-schemas.exe --strict <prefix>\share\glib-2.0\schemas` (exclude org.gtk.Demo / org.gtk.exampleapp).
Lookup: every system data dir `\glib-2.0\schemas\gschemas.compiled` (+ `GSETTINGS_SCHEMA_DIR`); missing → probe shows
all four MISSING with an empty data dir **[probe]**.
Backend: Windows default = registry (`HKCU\Software\GSettings\org\gtk\settings\file-chooser` – window size, sort, hidden
files). Tests: `GSETTINGS_BACKEND=memory` (built-in on Windows).

### 8.2 GtkSettings on Windows – measured defaults **[probe_gtkrt]**
`gtk-font-name "Segoe UI 9"`, `gtk-theme-name "Adwaita"`, `gtk-icon-theme-name "Adwaita"`, `gtk-cursor-theme-name NULL`,
`gtk-cursor-theme-size 0`, `gtk-xft-dpi -1` (screen resolution 96), `gtk-xft-antialias 1`, `gtk-xft-hinting 1`,
`gtk-xft-hintstyle "hintfull"`, `gtk-xft-rgba "rgb"`, `gtk-dialogs-use-header FALSE`, `gtk-decoration-layout
"menu:minimize,maximize,close"`, `gtk-enable-animations TRUE`, `gtk-im-module ""` (→ built-in "ime"),
`gtk-overlay-scrolling TRUE`, `gtk-alternative-button-order TRUE` (no app call to
`gtk_dialog_set_alternative_button_order`, so no effect), `gtk-double-click-time 500`, `gtk-dnd-drag-threshold 4`,
`gtk-enable-event-sounds TRUE`, `gtk-error-bell TRUE`, `gtk-recent-files-enabled TRUE`, `gtk-print-backends ""`.

Precedence **[probe with XDG_CONFIG_HOME\gtk-3.0\settings.ini]**: settings.ini **cannot** override values GDK-Win32 supplies
(`gtk-font-name`, `gtk-xft-*` stayed "Segoe UI 9"/"rgb"), but sets everything else (error-bell, sounds, prefer-dark,
theme, cursor theme). Files read: `<GTK prefix>\share\gtk-3.0\settings.ini`, `<GTK prefix>\etc\gtk-3.0\settings.ini`,
each system config dir (`C:\ProgramData\gtk-3.0\settings.ini`), then `%LOCALAPPDATA%\gtk-3.0\settings.ini` – later files win.
Therefore values that matter are **set in code** (APPLICATION source beats every ini file) – §9.2; settings.ini only
mirrors them for other GTK tools in the bundle.

`<prefix>\etc\gtk-3.0\settings.ini` (exact):
```ini
[Settings]
gtk-theme-name=Adwaita
gtk-icon-theme-name=Adwaita
gtk-application-prefer-dark-theme=true
gtk-dialogs-use-header=false
gtk-error-bell=false
gtk-enable-event-sounds=false
gtk-enable-input-feedback-sounds=false
```
Do **not** put `gtk-font-name`/`gtk-xft-*` (ignored on Windows), `gtk-cursor-theme-name` (keep native cursors) or any
CSD key (GTK 3 has no CSD setting; CSD is the `GTK_CSD` env var, §9.1).

---

## 9. Runtime environment, search paths and the new `ui_runtime` layer

### 9.1 Environment variables read by the stack **[DLL strings]**
libgtk: GTK_THEME GTK_CSD GTK_DEBUG GTK_MODULES GTK3_MODULES GTK_PATH GTK_EXE_PREFIX GTK_DATA_PREFIX GTK_IM_MODULE
GTK_IM_MODULE_FILE GTK_OVERLAY_SCROLLING GTK_SLOWDOWN GTK_USE_PORTAL GDK_DPI_SCALE GSETTINGS_SCHEMA_DIR XDG_DATA_DIRS;
libgdk: GTK_CSD GTK_USE_PORTAL GDK_SCALE GDK_BACKEND GDK_DEBUG, GDK_WIN32_{DISABLE_HIDPI,FONT_RESOLUTION,LAYERED,
TABLET_INPUT_API,USE_EXPERIMENTAL_OLE2_DND,…}; libgio: GSETTINGS_BACKEND GSETTINGS_SCHEMA_DIR GIO_EXTRA_MODULES
GIO_MODULE_DIR GIO_USE_VFS; libglib: XDG_DATA_DIRS LANGUAGE (+ XDG_CONFIG_HOME/XDG_DATA_HOME/XDG_CACHE_HOME – honoured on
Windows **[probe]**); gdk-pixbuf: GDK_PIXBUF_MODULE_FILE; pangocairo: PANGOCAIRO_BACKEND; fontconfig: FONTCONFIG_FILE/PATH.
A Windows user who installed Inkscape/GIMP/MSYS2 may have GTK_THEME, GTK_CSD, PANGOCAIRO_BACKEND=fc, GTK_PATH,
XDG_DATA_DIRS set globally → foreign modules (ABI clash), CSD windows, fontconfig backend (wrong family names, cache scan).

### 9.2 API (new `src/ui/ui_runtime.c`, `include/radios/ui_runtime.h`; Windows-only code per D1)
```c
/* Before gtk_init_check() and before anything calls g_get_system_data_dirs()/g_get_system_config_dirs()
 * (first statements of main after radios_platform_init()). */
gboolean radios_ui_runtime_prepare_environment(GError **error);
/* After gtk_init_check(), before the first widget (main.c, and every GUI test's main()). */
gboolean radios_ui_runtime_init(const gchar *assets_root, GError **error);
/* For --check-config: appends one Turkish line per check, returns FALSE when something is missing. */
gboolean radios_ui_runtime_check(GString *report);
void radios_ui_window_frame_apply(GtkWidget *toplevel);   /* §11.3, also used by the realize hook */
```
`radios_ui_runtime_prepare_environment`:
1. GTK prefix: `HMODULE m = GetModuleHandleW(L"libgtk-3-0.dll")` (load-time import, always loaded) →
   `prefix = g_win32_get_package_installation_directory_of_module(m)` (strips trailing `bin`). Bundle: `<root>\runtime`;
   dev/tests: `C:\msys64\ucrt64`. Never use the exe location (test exes live in `build\`).
2. Unless `RADIOS_GTK_KEEP_ENV=1` (developer escape hatch): `g_unsetenv` GTK_PATH, GTK_MODULES, GTK3_MODULES,
   GTK_EXE_PREFIX, GTK_DATA_PREFIX, GTK_IM_MODULE_FILE, GTK_CSD, GTK_THEME, GTK_USE_PORTAL, GIO_EXTRA_MODULES,
   GIO_MODULE_DIR, GIO_USE_VFS, GSETTINGS_SCHEMA_DIR, PANGOCAIRO_BACKEND, FONTCONFIG_FILE, FONTCONFIG_PATH; and
   GDK_BACKEND unless it equals "win32". Keep GDK_SCALE, GDK_DPI_SCALE, GTK_DEBUG, GDK_DEBUG, G_DEBUG, GTK_IM_MODULE,
   GSETTINGS_BACKEND, XDG_*_HOME (tests use them).
3. `g_setenv("XDG_DATA_DIRS", "<prefix>\\share", TRUE)` – removes `C:\ProgramData` / `C:\Users\Public\Documents` from
   schema and icon lookup **[probe: with XDG_DATA_DIRS set only that dir is used]**;
   `g_setenv("XDG_CONFIG_DIRS", "<prefix>\\etc", TRUE)` (default system config dirs = `[C:\ProgramData]`; with the
   variable set GLib returns only the given dir **[probe_misc]** → drops `C:\ProgramData\gtk-3.0\settings.ini`; GTK
   still reads `<prefix>\etc\gtk-3.0\settings.ini` itself, so it is listed once).
4. `g_setenv("GDK_PIXBUF_MODULE_FILE", "<prefix>\\lib\\gdk-pixbuf-2.0\\2.10.0\\loaders.cache", TRUE)`.
5. `g_setenv("LANGUAGE", "tr", FALSE)` – GTK's own strings follow the Turkish UI even on an English Windows
   (default on this machine: "İ_ptal", "_Aç", "_Kaydet", "Diğer Konumlar"; LANG=en → English; LANG=en + LANGUAGE=tr →
   Turkish **[probe_i18n]**).
6. Existence checks (fail with `G_IO_ERROR_NOT_FOUND`, message "GTK çalışma zamanı eksik: <path>. Kurulumu onarın."):
   `<prefix>\share\glib-2.0\schemas\gschemas.compiled`, `<prefix>\lib\gdk-pixbuf-2.0\2.10.0\loaders.cache`,
   `…\loaders\pixbufloader_svg.dll`, `<prefix>\share\icons\Adwaita\index.theme`, `<prefix>\share\icons\hicolor\index.theme`.
   main prints the error and exits 1 (passive `--version/--help` must not call it; `--check-config` reports it via
   `radios_ui_runtime_check`).

`radios_ui_runtime_init`:
1. `radios_ui_register_app_font(assets_root)` (§5.3).
2. Code-applied settings (`g_object_set(gtk_settings_get_default(), …)` = APPLICATION source):
   `gtk-application-prefer-dark-theme TRUE` (moves from window.c:765), `gtk-error-bell FALSE`,
   `gtk-enable-event-sounds FALSE`, `gtk-enable-input-feedback-sounds FALSE`, `gtk-dialogs-use-header FALSE`,
   `gtk-icon-theme-name "Adwaita"`; `gtk-theme-name "Adwaita"` **only if** the current value does not start with
   "HighContrast" (respect Windows high contrast).
3. Self-checks: `gdk_pixbuf_get_formats()` has "svg"; `gtk_icon_theme_has_icon(gtk_icon_theme_get_default(),
   "emblem-system-symbolic")`; `g_settings_schema_source_lookup(g_settings_schema_source_get_default(),
   "org.gtk.Settings.FileChooser", TRUE) != NULL`. Any failure → GError (Turkish) → startup error.
4. `g_signal_add_emission_hook(g_signal_lookup("realize", GTK_TYPE_WIDGET), 0, toplevel_realized, NULL, NULL)`; the hook
   (runs after the class realize, so `gtk_widget_get_window` is set – content_scale.c:491-498 relies on the same order)
   calls `radios_ui_window_frame_apply` for `GTK_IS_WINDOW && gtk_window_get_window_type == GTK_WINDOW_TOPLEVEL`.
5. Idempotent (static flag) – tests may call it once per process.

### 9.3 DLL search (cross-area, D3 owner)
D3 calls `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)` + `SetDllDirectoryW(L"")`. Load-time imports are
unaffected, but every later `LoadLibraryW` (g_module_open of `pixbufloader_svg.dll`, whose dependencies
`librsvg-2-2.dll`, `libxml2-16.dll`, … are not loaded yet) only searches the exe dir, System32 and `AddDllDirectory`
dirs. Bundle: DLLs sit next to RadioS.exe → fine. Dev/tests (exe in `build\`, DLLs from `C:\msys64\ucrt64\bin` via PATH)
→ **measured [probe_dlldirs]**: `Unable to load image-loading module: …\pixbufloader_svg.dll: Belirtilen modül
bulunamadı.` + `Gtk-WARNING: Could not load a pixbuf from icon theme` (fatal with G_DEBUG=fatal-warnings).
Requirement: right after `SetDefaultDllDirectories`, `radios_platform_init()` must `AddDllDirectory()` the directory of
the already-loaded `libglib-2.0-0.dll` (`GetModuleHandleW(L"libglib-2.0-0.dll")` + `GetModuleFileNameW`, strip the file
name) – with that single call the SVG icon loads **[probe_dlldirs]**. Do not add PATH entries (C:\MinGW\bin must stay
excluded).

---

## 10. Runtime bundle (exact list) and staging

Prefix `<P>` = `<root>\runtime` (dev, D4) or the install prefix. Layout is the standard GTK prefix; all lookups are
relative to the GTK DLL location, so the tree is relocatable.

### 10.1 `<P>\bin` – GTK3 closure (48 DLLs, 44 732 361 bytes; from libgtk-3-0, libgdk-3-0, libjson-glib-1.0-0 and the SVG loader) **[dll_closure.ps1, objdump]**
libatk-1.0-0, libbrotlicommon, libbrotlidec, libbz2-1, libcairo-2, libcairo-gobject-2, libdatrie-1, libdeflate,
libepoxy-0, libexpat-1, libffi-8, libfontconfig-1, libfreetype-6, libfribidi-0, libgcc_s_seh-1, libgdk_pixbuf-2.0-0,
libgdk-3-0, libgio-2.0-0, libglib-2.0-0, libgmodule-2.0-0, libgobject-2.0-0, libgraphite2, libgtk-3-0, libharfbuzz-0,
libiconv-2, libintl-8, libjbig-0, libjpeg-8, libjson-glib-1.0-0, liblerc, liblzma-5, libpango-1.0-0,
libpangocairo-1.0-0, libpangoft2-1.0-0, libpangowin32-1.0-0, libpcre2-8-0, libpixman-1-0, libpng16-16, librsvg-2-2
(11.8 MB), libsharpyuv-0, libstdc++-6, libthai-0, libtiff-6, libwebp-7, libwinpthread-1, libxml2-16, libzstd, zlib1
(all `.dll`). libfontconfig/libfreetype stay although the app no longer calls Fc* (imported by cairo, pangoft2,
librsvg).

The **whole application** closure (adding libcurl-4, avcodec-63, avformat-63, avutil-61, swresample-7, avfilter-12) is
**123 DLLs / 184 MB** (FFmpeg pulls libx265 22.7 MB, libshaderc_shared 10.7 MB, libaom 9.7 MB, libsvtav1enc 7.6 MB,
libjxl, libvpx, vulkan-1, libplacebo, ggml/libwhisper, gnutls, openssl …) – list in Appendix A; owned by the media/
packaging areas (a reduced FFmpeg build would shrink it). Never hard-code the list: compute it at staging time.

Also in `bin`: `gspawn-win64-helper.exe`, `gspawn-win64-helper-console.exe` (GLib 2.90.1; used by GIO when it launches
command-line URI handlers and by any remaining g_spawn). **Not shipped**: gdbus.exe (ui-platform-c §2.5),
fc-*.exe, gtk tools, glib-compile-schemas/gdk-pixbuf-query-loaders/gtk-update-icon-cache (build-time only), the
`libpixbufloader-*.dll` set, `libpixbufloader-jxl.dll`.
System DLLs referenced and NOT bundled: advapi32, api-ms-win-crt-* (UCRT, part of Windows 10+), bcrypt(primitives),
cfgmgr32, comctl32, comdlg32, crypt32, dnsapi, dwmapi, dwrite, gdi32, gdiplus, hid, imm32, iphlpapi, kernel32,
msimg32, ncrypt, ntdll, ole32, opengl32, rpcrt4, secur32, setupapi, shell32, shlwapi, user32, userenv, usp10, version,
winmm, winspool.drv, wldap32, ws2_32, wsock32.

### 10.2 Data files
| Path under `<P>` | Source (C:\msys64\ucrt64\…) | Notes |
|---|---|---|
| `lib\gdk-pixbuf-2.0\2.10.0\loaders\pixbufloader_svg.dll` | same path | only external loader |
| `lib\gdk-pixbuf-2.0\2.10.0\loaders.cache` | §7 text | trailing blank line mandatory |
| `share\glib-2.0\schemas\org.gtk.Settings.{FileChooser,ColorChooser,EmojiChooser,Debug}.gschema.xml` + `gschemas.compiled` | share\glib-2.0\schemas | compile at staging |
| `share\icons\Adwaita\{index.theme,icon-theme.cache,16x16\,scalable\,symbolic\}` | share\icons\Adwaita | no `cursors\` |
| `share\icons\AdwaitaLegacy\**` | share\icons\AdwaitaLegacy | all |
| `share\icons\hicolor\{index.theme,icon-theme.cache}` | share\icons\hicolor | cache regenerated |
| `share\locale\tr\LC_MESSAGES\{gtk30.mo (109 137), glib20.mo (141 490), gdk-pixbuf.mo (21 246), json-glib-1.0.mo (5 541)}` | share\locale\tr\LC_MESSAGES | gtk30-properties.mo not needed |
| `etc\gtk-3.0\settings.ini` | §8.2 text | |
Not needed **[probe/inspection]**: `etc\fonts` (no fontconfig init), `share\themes` (Adwaita/HighContrast built in),
`lib\gtk-3.0\3.0.0\immodules*` (ime/simple built in; MSYS2 dir empty), print backends (native printing; app does not
print), `lib\gio\modules` (empty), `share\mime` (GIO-Win32 uses the registry), `share\gtk-3.0\emoji\*.gresource` (no tr).
Project files used at runtime: `assets\fonts\InterVariable.ttf`, `assets\fonts\LICENSE.txt`, `assets\themes\*.css`.

### 10.3 Staging procedure (packaging/build area implements; deterministic, idempotent)
Preferred: `cmake/StageRuntime.cmake` behind a `stage-runtime` target (or `tools/radios_stage_runtime.py` run with MSYS2
python), preserving the already staged `runtime\python` (D4).
1. `file(GET_RUNTIME_DEPENDENCIES EXECUTABLES $<TARGET_FILE:radios> [test/tool exes] MODULES
   C:/msys64/ucrt64/lib/gdk-pixbuf-2.0/2.10.0/loaders/pixbufloader_svg.dll DIRECTORIES C:/msys64/ucrt64/bin
   PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-" POST_INCLUDE_REGEXES "^C:/msys64/ucrt64/bin/" POST_EXCLUDE_REGEXES ".*"
   RESOLVED_DEPENDENCIES_VAR deps UNRESOLVED_DEPENDENCIES_VAR missing)` with
   `CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM windows+pe`, `…_TOOL objdump`, `…_COMMAND C:/msys64/ucrt64/bin/objdump.exe`;
   fail if `missing` contains a non-system DLL. Copy `deps` + gspawn helpers to `<P>\bin`.
2. Copy the data files of §10.2; write loaders.cache and settings.ini with LF.
3. `glib-compile-schemas.exe --strict <P>\share\glib-2.0\schemas`.
4. `gtk-update-icon-cache.exe -q -f` on the three theme dirs (last step).
5. Verify (also as CTest `gtk_runtime_bundle`, runs `tests/test_gtk_runtime.c` against `<P>`): every DLL imported
   recursively by RadioS.exe and the SVG loader resolves inside `<P>\bin` or System32 (objdump), none from
   `C:\msys64`/`C:\MinGW`; checks of §15.3.

---

## 11. Windows, decorations, dialogs

### 11.1 No CSD anywhere
No `gtk_header_bar_*`/`gtk_window_set_titlebar` in src; `gtk-dialogs-use-header` FALSE on Windows (and forced FALSE in
code) → GtkDialog/GtkMessageDialog/GtkFileChooserDialog use native frames (no layered shadows, Aero Snap works).
`GTK_CSD=1` in the user environment would turn every window into a CSD window → unset in §9.2.

### 11.2 Toplevels created by the app
| file:line | window | Windows notes |
|---|---|---|
| window.c:771-775 | GtkApplicationWindow 1800×1000, min 600×600 | Windows does not constrain oversized windows → clamp to the monitor work area (ui-platform-c §6.3) |
| preferences.c:913-925 | undecorated, transient, DIALOG hint, sized from the parent monitor work area | WS_POPUP: no move/resize/snap → §11.4 |
| dj.c:231-286 | detached mixer: decorated toplevel, transient, `gtk_window_move`/`set_position`, `begin_move_drag` during the same press (dj.c:325, 343) | GDK-Win32 emulates move-drag; starting it on a window created during an implicit grab of another HWND must be verified manually; fallback: skip `begin_move_drag` when no motion arrives within 200 ms (window already placed at the pointer, dj.c:261-264) |
| dialogs (automation.c:378,1119; broadcast.c:2992-2993,3678,4019; data.c:618,639; editors.c:898; emergency.c:509,1644,1981,2005; header.c:214; import_profiles_ui.c:131-133; media.c:648,1030,1193,1366; media_archive_ui.c:573,719,749; playlist.c:1332,2057; reset_ui.c:188; rotation_editor.c:2075; routing.c:376,704; workflow_hub.c:640; dj.c:1182,2056; midi_ui.c:416) | owned (transient) windows stay above their owner; not in Alt+Tab separately; `gtk_window_present` from background may only flash the taskbar (Windows foreground lock) | accepted |

### 11.3 Dark title bar (new, D2 quality)
GDK-Win32 never calls DwmSetWindowAttribute (no import) → light caption over the black UI. `radios_ui_window_frame_apply`:
```c
#include <gdk/gdkwin32.h>
#include <dwmapi.h>           /* link -ldwmapi */
void radios_ui_window_frame_apply(GtkWidget *toplevel)
{
    GdkWindow *gw = gtk_widget_get_window(toplevel);
    if (!gw || gdk_window_get_window_type(gw) != GDK_WINDOW_TOPLEVEL) return;
    HWND hwnd = (HWND)gdk_win32_window_get_handle(gw);
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE /*20*/, &dark, sizeof dark)))
        DwmSetWindowAttribute(hwnd, 19 /* Windows 10 1809-1909 */, &dark, sizeof dark);
    COLORREF caption = RGB(0x00, 0x00, 0x00), text = RGB(0xED, 0xED, 0xED), border = RGB(0x33, 0x33, 0x33);
    DwmSetWindowAttribute(hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof caption);  /* Win11 22000+; E_INVALIDARG before */
    DwmSetWindowAttribute(hwnd, DWMWA_TEXT_COLOR, &text, sizeof text);
    DwmSetWindowAttribute(hwnd, DWMWA_BORDER_COLOR, &border, sizeof border);
}
```
All four calls return S_OK on build 26300 and read back TRUE **[probe_dwm]**; mingw-w64 `dwmapi.h` defines the
constants. Applied at realize (before the first map → no white flash). Ignored for undecorated windows.

### 11.4 Movable settings window (preferences.c:936-943)
Wrap the `preferences-titlebar` box in a `GtkEventBox` (or connect on the box with `gtk_widget_add_events(…,
GDK_BUTTON_PRESS_MASK)` via an event box) and on primary `GDK_BUTTON_PRESS` outside the close button call
`gtk_window_begin_move_drag(GTK_WINDOW(prefs->window), 1, (gint)e->x_root, (gint)e->y_root, e->time)`; return TRUE.
Escape/close behaviour unchanged (836-841, 943). Edit owned by ui-platform-c (preferences.c in its scope).

### 11.5 Application icon
GDK-Win32 registers its window classes with the **first icon resource of the executable** (ExtractIconEx index 0, from
GDK source) when the app sets no icon – RadioS sets none (no `gtk_window_set_icon*` / default icon call in src). Add `assets/icons/radios.svg` (paths only, no text – rsvg-convert has no Inter)
and a committed multi-size `assets/icons/radios.ico` (16, 20, 24, 32, 40, 48, 64, 256 px; 256 as PNG entry) referenced
by `src/app/radios.rc` as `1 ICON "…/assets/icons/radios.ico"` (lowest ordinal first; build area adds VERSIONINFO +
manifest). Allow `.ico`/`.svg` in the packaging allow-list. Taskbar grouping: AppUserModelID (ui-platform-c §2.5).

---

## 12. HiDPI and content scale

### 12.1 Measured behaviour
* Without a manifest GDK 3.24.52 makes the process **system-DPI-aware** (`AreDpiAwarenessContextsEqual(SYSTEM_AWARE)` on a
  realized toplevel) **[probe_dwm]**; the lead's PerMonitorV2 manifest (D4) overrides that (SetProcessDpiAwareness then
  fails harmlessly). GDK computes an **integer** window scale per monitor = effective dpi / 96 (GetDpiForMonitor):
  100–175 % → 1, 200–275 % → 2; WM_DPICHANGED handling only reacts when that integer changes (gdkevents-win32.c
  `handle_dpi_changed`, from GDK source; the multi-monitor case could not be probed on this single 1920×1200 @100 %
  monitor).
* Pango resolution = system DPI read once at startup (`GDK_WIN32_FONT_RESOLUTION` overrides).
* CSS `px` font sizes and all CSS lengths are device pixels at scale 1 → at 125–175 % the whole RadioS UI renders crisp
  at 100 % physical size; pt-based text (gtk-font-name "Segoe UI 9" in menus/tooltips/unstyled dialogs, Pango "Sans 8/10",
  GTK default fonts) grows with the DPI **[probe_dpi]** → mixed sizes. At 200 % everything is 2× and consistent
  (`GDK_SCALE=2` emulation: logical sizes unchanged **[probe]**).
* RadioSContentScale (content_scale.c) zooms by bilinear blitting of an offscreen surface (content_scale.c:208-213):
  usable for fitting, blurry at non-integer factors – not a HiDPI solution.

### 12.2 Policy
1. Keep PerMonitorV2 (crisp). Document: at 125–175 % the UI is drawn at 100 % physical size; users may set Windows to
   100 %/200 % for that monitor, use the workspace zoom (blurry), or start with `GDK_SCALE=2` (crisp, 2×) – expose an INI
   override `[ui] gdk_scale = auto|1|2` applied by `g_setenv("GDK_SCALE", …)` before `gtk_init` (auto = GDK default).
2. Convert pt-based drawing fonts to px (§5.5) so drawn labels stay proportional to the px UI.
3. Do not auto-change `content_scale_percent` (agrees with ui-platform-c §7.5).
4. Optional, needs lead approval (not required for the port): a "DPI text factor" = effective dpi /(96 × GDK scale)
   multiplied into the existing window.c text-size regex pass (window.c:150-153) – larger crisp text in the main
   provider only; geometry and other providers unchanged.
5. Minimum OS for these features: Windows 10 1703 (PMv2, DirectWrite font sets); dark caption attribute 19/20 needs
   1809+/2004+ (ignored when unsupported).

### 12.3 Mixed-DPI risk
GTK 3.24's per-monitor path is less exercised than system-aware (GDK's own choice). Verify manually: move the main window,
the detached mixer and an open menu between a 100 % and a 150 %/200 % monitor. If it misbehaves, the fallback manifest is
`<dpiAware>true</dpiAware>` without `<dpiAwareness>` (system-aware, what GDK requests itself).

### 12.4 content_scale.c Win32 items (edit owned by ui-platform-c §7)
* `moved-to-rect` fires synchronously inside `gdk_window_move_to_rect` on GdkWin32Display **[lead probe_moverect]** → the
  X11-only re-anchoring (content_scale.c:457-490) must also run for `GDK_IS_WIN32_DISPLAY` (include `<gdk/gdkwin32.h>`),
  keeping the `RADIOS_CONTENT_SCALE_POPUP_MAP_ONLY` seam.
* Tooltip cursor size (content_scale.c:378-382): Win32 default cursor size 32 at 100 % (device px) **[probe]** → divide by
  `gtk_widget_get_scale_factor` on Win32 too.
* Offscreen windows (GDK_WINDOW_OFFSCREEN parented to the root, content_scale.c:56-84) are backend-neutral; input
  redirection (`pick-embedded-child`, to/from-embedder) is GDK-core.
* GtkMenu private-layout probe (content_scale.c:277-311): same field sizes on LLP64; runtime check stays.

---

## 13. File choosers

### 13.1 Behaviour on Win32
* **GtkFileChooserButton** (editors.c:1238; playlist.c:2027-2028; transfer_ui.c:677,760; import_profiles_ui.c:402;
  dj.c:1168; workflow_hub.c:383) is built on GtkFileChooserNative (GTK ≥ 3.20), which keeps a hidden
  GtkFileChooserDialog (a GtkFileChooserDialog toplevel exists right after `gtk_file_chooser_button_new` **[probe_misc2]**).
  Per GtkFileChooserNative's documented Win32 rules the Windows IFileDialog is shown – **unless** a filter has a
  mime-type or custom rule, an extra widget or a preview is used; then the hidden GTK dialog is shown (not probed
  interactively: opening a modal dialog is not possible in a read-only probe).
* **GtkFileChooserDialog** (automation.c:378,1119; advert_editors.c:786; broadcast.c:2992; playlist.c:1332; header.c:214;
  media_archive_ui.c:573,719; media.c:1030 (extra widget :1038), 1193; workflow_hub.c:640; midi_ui.c:416; routing.c:704;
  emergency.c:2005) is GTK-drawn (Adwaita-dark, Turkish strings via gtk30.mo, drives under "Diğer Konumlar"), needs the
  FileChooser schema, writes `%LOCALAPPDATA%\recently-used.xbel` and registry GSettings. Keep these (tests drive them by
  widget name, §1); converting to GtkFileChooserNative is a later, optional step with a test seam.
* Pattern matching is **case-insensitive** on Win32 (`*.wav` matches `A.WAV`, `b.Wav`) **[probe_filter]**; the extra
  `*.XML` (import_profiles_ui.c:405) is harmless.
* **Mime rules never match** on Win32: `gtk_file_filter_add_mime_type(f, "audio/*")` rejects `x.mp3`/`audio/mpeg` and
  `x.flac`/`audio/x-flac` (content types are extensions: `.mp3`) **[probe_filter]**.

### 13.2 Changes
* editors.c:1242 and dj.c:1194: remove `add_mime_type("audio/*")`; use one shared list (widgets.c
  `radios_ui_audio_file_patterns[]` = `*.wav *.mp3 *.flac *.ogg *.opus *.m4a *.aac`, i.e. media.c:1035) so that the
  editor/pad pickers accept the same formats as the importer (today dj.c accepts only wav/mp3/flac, editors.c lacks
  opus/aac; on Linux "audio/*" accepted any audio type – the shared explicit list is the Windows equivalent; extend it if
  the analyzer supports more).
* playlist.c:2061: remove `add_mime_type("application/xml")` (keep `*.xml`).
* Result: all GtkFileChooserButtons open the native dialog; no dead rules. No test checks mime filters (only
  test_transfer_ui.c:135-137 checks the "*.st" filter name).
* Save dialogs without a folder start in the process CWD → set a folder (ui-platform-c §14.7).

---

## 14. Cursors, keyboard, IME, emoji

* Cursor names: only `"grab"` (dj.c:312-314, mixer handle). All CSS cursor names tested (`default pointer text grab
  grabbing col-resize row-resize move not-allowed crosshair wait progress ew-resize ns-resize all-scroll`) return a
  cursor with the default "system" theme **[probe]**. GTK-internal resize/text cursors map to IDC_* system cursors.
* Accelerators: `<Primary>q` (main.c:47) = Ctrl+Q; Alt+F4 → delete-event. Key handlers: Ctrl+C/V by keyval
  (automation_calendar.c:1475-1477), Menu / Shift+F10 (automation_calendar.c:1473, rotation_editor.c:1604), F2
  (routing_graph_ui.c:420), Esc with `gtk_accelerator_get_default_mod_mask()` (programme_queue.c:1295), mouse
  modifiers (playlist.c:522,925-926; playlist_browser.c:388-418; automation_calendar.c:763-812). GDK-Win32 reports
  AltGr (Turkish Q: @ € ₺ [ ] { } \ | ~) as `GDK_MOD2_MASK`, adding CONTROL/MOD1 only for a physically pressed RCtrl/LAlt
  (gdkevents-win32.c `build_key_event_state`; from GDK source, not probed – injecting AltGr needs SendInput on the user's
  desktop) – MOD2 is not in the default accelerator mask; ui-platform-c §14.6's extra MOD1 guard for Ctrl+C/V is
  harmless either way. Keyval checks of Latin letters fail on non-Latin layouts (same as Linux).
* IME: default module "ime" (gtk-im-module "") – Windows IME/Win+. emoji panel text arrives through it; tests force
  `GTK_IM_MODULE=gtk-im-context-simple` (built in).
* GTK emoji chooser (entry context menu) needs the EmojiChooser schema and renders through Segoe UI Emoji (color, cairo
  DWrite) – no tr emoji gresource exists (English names).
* Accessibility: GTK 3 has **no UIA/MSAA bridge on Windows** – `atk_object_set_role/name` calls (window.c:840,
  dj.c:546, preferences.c:1012, instruments.c:336, neon_progress.c:325, player_waveform.c:1005, broadcast.c:3072, …)
  are harmless no-ops; screen readers cannot read the UI (known limitation, document).

---

## 15. Tests

### 15.1 Environment for GUI tests on Windows
Replace every `GDK_BACKEND=x11` (CMakeLists.txt:150,289,312,329,349,352,369,412,419,431,437,471,480,486,492,578,665,668,
679,684, … ≈45 sites incl. cmake/*.cmake; runners run_rotation_editor_regressions.py:26, run_content_scale_ui.py:40,
run_routing_graph_ui.py:54) – a Win32-only GTK cannot open an "x11" display. Runner contract (Python helper, e.g.
`tests/gui_env.py`):
* `has_interactive_desktop()` via ctypes: `GetProcessWindowStation()` + `GetUserObjectInformationW(h, UOI_FLAGS=1, …)` →
  `USEROBJECTFLAGS.dwFlags & WSF_VISIBLE (1)`; return 77 (skip) when FALSE (service session/CI without desktop). Replaces
  the DISPLAY/WAYLAND_DISPLAY checks (run_audio_ui.py:49, run_routing_ui.py:39, run_routing_graph_ui.py:39,
  test_application_shutdown.py:55, run_release_acceptance.py:325, test_release_install.py:265-308).
* `gui_test_environment(tmp)`: `G_DEBUG=fatal-warnings`, `GTK_IM_MODULE=gtk-im-context-simple`,
  `GSETTINGS_BACKEND=memory` (no HKCU writes), private `XDG_CONFIG_HOME/XDG_DATA_HOME/XDG_CACHE_HOME` under `tmp`
  (keeps recently-used.xbel and user gtk.css/settings.ini out), remove GDK_BACKEND/GTK_THEME/GTK_CSD/PANGOCAIRO_BACKEND,
  `PYTHONDONTWRITEBYTECODE=1`; drop `NO_AT_BRIDGE`/`GTK_A11Y` (no effect on Windows), Xvfb and dbus-daemon.
* GUI test executables must call `radios_ui_runtime_prepare_environment()` (before `gtk_init_check`) and
  `radios_ui_runtime_init(assets_root)` – or at least `radios_ui_register_app_font` where they replicate the app theme
  (test_automation_calendar.c:3036-3050, test_media_drop_ui.c:765-778, test_broadcast_catalogue_ui.c:873-890,
  test_playlist_redesign_ui.c:359-381, test_rekordbox_ui.c:40-50, test_programme_queue.c:1746-1751). Optional refactor:
  `GtkCssProvider *radios_ui_theme_provider_new(const gchar *assets_root, gchar **css_source, GError **)` shared by
  window.c:734-747 and these harnesses.
* Content-scale tests (test_content_scale_ui.c:10,105,128; test_content_scale_module.c:2,14; test_content_scale_popups.c;
  content_scale_test_input.h): SendInput shim (ui-platform-c §15) on an unlocked interactive desktop; run variants with
  `GDK_SCALE=2`.

### 15.2 Failure modes the tests will surface (all fatal with G_DEBUG=fatal-warnings)
Missing hicolor → Gtk-WARNING (measured, fatal); missing schemas → `GLib-GIO-ERROR` abort as soon as any
GtkFileChooserButton is constructed (measured, exit 0xC0000409); missing **or** malformed loaders.cache (no blank line
after the last block) → silent, symbolic icons missing (measured; only the self-check detects it, and a later
"Could not find the icon" warning appears only if hicolor is missing too).

### 15.3 New tests (tests/test_gtk_runtime.c, CTest `gtk_runtime`, plus `gtk_runtime_bundle` against `runtime\`)
1. `radios_ui_runtime_prepare_environment()` + `gtk_init_check` + `radios_ui_runtime_init(<root>\assets)` succeed.
2. Font: "Inter Variable Text 14px" resolves to family "Inter Variable Text"; a layout of "Otomasyon ▾ ⏮ ⏭ ⚙ ✎ ✕ ⋯ ⠿ ⇄ ▤ ＋"
   with `RADIOS_UI_FONT_FAMILIES` has `pango_layout_get_unknown_glyphs_count == 0`.
3. Pixbuf formats contain "svg"; `gtk_icon_theme_load_icon` succeeds for all app icon names of §1.
4. All four schemas present; `g_settings_new("org.gtk.Settings.FileChooser")` works with `GSETTINGS_BACKEND=memory`.
5. Every file CSS + concatenated reference CSS parses with 0 `parsing-error` emissions.
6. GtkSettings after init: prefer-dark TRUE, error-bell FALSE, dialogs-use-header FALSE, icon theme "Adwaita".
7. With `LANGUAGE` unset before prepare: `g_dgettext("gtk30","_Cancel")` == "İ_ptal".
8. Realize a toplevel: `DwmGetWindowAttribute(DWMWA_USE_IMMERSIVE_DARK_MODE)` == TRUE.
9. File filters: the shared audio pattern filter accepts `A.WAV`, `b.Opus`, rejects `c.txt`.
10. Environment sanitising: with `GTK_CSD=1`, `PANGOCAIRO_BACKEND=fc`, `XDG_DATA_DIRS=C:\nowhere` set, prepare removes/
    overrides them (`g_get_system_data_dirs()[0]` == `<prefix>\share`).
Smoke acceptance: `native_window` (CMakeLists.txt:543-546: `radios --smoke-test --project-root … --capture …`, change
`WORKING_DIRECTORY /tmp` → `${CMAKE_BINARY_DIR}`) exercises CSS + font + icons + all profiles on the real runtime.

---

## 16. Clipboard and drag-and-drop (GTK-runtime aspects; behaviour owned by ui-platform-c §10, §12.3)
* admin_ui.c:348 `gtk_clipboard_set_text` → CF_UNICODETEXT with delayed rendering; GDK handles WM_RENDERFORMAT and
  WM_RENDERALLFORMATS (strings present) → content survives exit. Secrets end up in Win+V history/cloud clipboard unless
  the exclusion formats are added (ui-platform-c §12.3).
* External file drops: GDK 3.24.52 uses WM_DROPFILES (`DragQueryFileW`) by default, delivered as `text/uri-list`
  (`file:///C:/…`, UNC `file:////server/share/…`); OLE2 only with `GDK_WIN32_USE_EXPERIMENTAL_OLE2_DND` (do not set; the
  env sanitiser leaves it alone). Elevated RadioS cannot receive drops from normal Explorer (UIPI) → manifest asInvoker.
  In-app DnD targets (`GTK_TARGET_SAME_APP`) use GDK's local protocol – unchanged.

---

## 17. Implementation order (this area)
1. css_to_gtk.py encodings + CMake `-X utf8 -B` + Python pin (unblocks the build).
2. `ui_runtime.c/.h` (prepare/init/check/font/frame/font description helper) + CMake: add source, `-ldwmapi`, remove
   FONTCONFIG; main.c/window.c call sites (with ui-platform-c).
3. CSS family edits (radios.css:4, preferences.c:72, dj-pedals.css), Pango sites (routing_graph_ui.c:69,
   automation_calendar.c:605, player_waveform.c:795); optional toy-text helper.
4. File filter cleanup (editors.c, dj.c, playlist.c) + shared pattern list.
5. Staging (§10.3) + settings.ini + loaders.cache + schemas + icon caches; packaging allow-list (.ttf/.ico/.svg).
6. Tests (§15) and test-env migration; docs: assets/README.md:5, docs/04-native-tasarim.md:11, docs/02-proje-duzeni.md:31,71
   ("Fontconfig ile WOFF2" → "DirectWrite/Pango ile TTF"), packaging/README.md:27.

---

## Appendix A – full application DLL closure (123, 184 087 263 bytes; MSYS2 state 2026-10-19)
GTK closure (48, §10.1) plus: avcodec-63, avfilter-12, avformat-63, avutil-61, swresample-7, swscale-10, libcurl-4,
ggml-0, ggml-base-0, libaom, libass-9, libbluray-4, libbrotlienc, libcrypto-3-x64, libdav1d-7, libdovi, libgme,
libgmp-10, libgnutls-30, libgomp-1, libgsm, libhogweed-7, libhwy, libidn2-0, libjxl, libjxl_cms, libjxl_threads,
liblc3-1, liblcms2-2, libmodplug-1, libmp3lame-0, libnettle-9, libnghttp2-14, libnghttp3-9, libngtcp2-16,
libngtcp2_crypto_ossl-0, libogg-0, libopencore-amrnb-0, libopencore-amrwb-0, libopenjp2-7, libopus-0, libp11-kit-0,
libplacebo-360, libpsl-5, librav1e, librtmp-1, libshaderc_shared, libsoxr, libspeex-1, libspirv-cross-c-shared, libsrt,
libssh, libssh2-1, libssl-3-x64, libsvtav1enc-4, libtasn1-6, libtheoradec-2, libtheoraenc-2, libunibreak-7,
libunistring-5, libva, libva_win32, libvidstab, libvorbis-0, libvorbisenc-2, libvpl-2, libvpx-1, libwebpmux-3,
libwhisper-1, libx264-165, libx265-217, libzimg-2, libzvbi-0, vulkan-1, xvidcore. (The bundled Python's own
`libcrypto-3.dll`/`libssl-3.dll` in `runtime\python` have different names – no clash.)

## Appendix B – exact loaders.cache, settings.ini: see §7 and §8.2.

## Appendix C – emergency fontconfig fallback (NOT shipped by default)
`<P>\etc\fonts\fonts.conf` (fontconfig finds `<dll dir minus bin>\etc\fonts`):
```xml
<?xml version="1.0"?>
<!DOCTYPE fontconfig SYSTEM "urn:fontconfig:fonts.dtd">
<fontconfig>
  <dir>WINDOWSFONTDIR</dir>
  <dir>WINDOWSUSERFONTDIR</dir>
  <cachedir>LOCAL_APPDATA_FONTCONFIG_CACHE</cachedir>
  <include ignore_missing="yes">conf.d</include>
  <alias><family>sans-serif</family><prefer><family>Segoe UI</family><family>Segoe UI Symbol</family></prefer></alias>
  <alias><family>monospace</family><prefer><family>Consolas</family></prefer></alias>
</fontconfig>
```
plus `PANGOCAIRO_BACKEND=fc` before gtk_init and the app font added to the PangoFcFontMap. First run scans
C:\Windows\Fonts (seconds). Family name there is "Inter Variable" (already in the CSS list).

## Appendix D – risks / open points
1. PerMonitorV2 with GTK 3.24 on mixed-DPI setups (§12.3) – manual verification required.
2. Fractional scaling (125–175 %): UI at 100 % physical size – product decision/documentation (§12.2).
3. Detached-mixer `begin_move_drag` immediately after creating the window (dj.c:325,343) – verify on Windows.
4. Bundle size (184 MB, mostly FFmpeg codecs/filters) – media/packaging decision.
5. Icon caches go stale after installer extraction (performance only).
6. Line-height +2 px for labels that contain fallback symbols (Segoe UI Symbol metrics).
7. No screen-reader support in GTK 3 on Windows.
8. A missing/corrupt runtime file is fatal or silent depending on the file (schemas → abort at the first
   GtkFileChooserButton; malformed loaders.cache → icons silently missing) – hence the fail-fast checks of §9.2 and the
   bundle test of §15.3.
