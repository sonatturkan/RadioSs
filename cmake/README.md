# Derleme desteği

`../CMakeLists.txt` RadioS'un C11 kütüphane, uygulama, araç ve test hedeflerini kurar. Yalnız
Windows 10/11 x64 ve MSYS2 UCRT64 araç zinciri (`C:\msys64\ucrt64`: GCC, windres, Ninja, pkgconf,
GTK 3, GLib, JSON-GLib, libcurl, FFmpeg) desteklenir. Ön ayarlar `../CMakePresets.json` içindedir
ve araçları tam yollarıyla sabitler (`C:\MinGW` altındaki başka GCC ve PATH'teki Python'lar
kullanılmaz). PowerShell'de, RadioS kökünde:

```powershell
$env:PATH = 'C:\msys64\ucrt64\bin;' + $env:PATH
cmake --preset windows-debug            # build\ (testler; GUI testleri kapalı)
cmake --build --preset windows-debug
ctest --preset windows-debug            # "hardware" etiketli testler hariç
```

| Ön ayar | Dizin | Amaç |
|---|---|---|
| `windows-debug` | `build\` | Geliştirme ve testler |
| `windows-debug-gui` | `build\gui\` | GTK arayüz kabul testleriyle (etkileşimli masaüstü gerekir) |
| `windows-release` | `build\release\` | `RadioS.exe`, `runtime\`, taşınabilir ZIP, kurulum (LGPL FFmpeg) |
| `windows-release-tests` | `build\release-tests\` | Release + bütün testler (kabul) |

Derleme ön ayarları: `windows-runtime` (geliştirme çalışma ortamı `runtime\`), `windows-portable`,
`windows-installer`; test ön ayarı `windows-hardware` yalnız gerçek ses aygıtlarıyla, operatör
onayıyla koşturulur (en çok -60 dBFS).

## Modüller

- `RadioSToolchain.cmake`: araç zinciri denetimi kimliğe dayanır (GNU GCC ≥ 16.2, "Built by MSYS2
  project", `-dumpmachine` = `x86_64-w64-mingw32`, `_UCRT` tanımlı, GNU windres; `C:\MinGW`
  altındaki derleyiciler reddedilir). `RADIOS_MSYS2_PREFIX` boşsa `gcc.exe`'nin üst dizinidir.
  pkg-config yalnız bu kökü (ve `RADIOS_FFMPEG_PREFIX` verilmişse LGPL FFmpeg kökünü, DECISIONS
  D10) görür. Genel tanımlar (`_FILE_OFFSET_BITS=64`, `_WIN32_WINNT=WINVER=0x0A00`, `UNICODE`,
  `NOMINMAX`, `GLIB_VERSION_MIN_REQUIRED`), yalnız C'ye giden katı uyarılar (`radios_warnings`),
  Release'te `-O2`, CodeView hata ayıklama bilgisi ve `RadioS.pdb` (D11), yeniden üretilebilir yollar.
  Derleme zamanı üreteçleri MSYS2 `python.exe` ile `-X utf8 -B -I` kipinde çalışır.
- `RadioSWindowsExe.cmake`: ertelenmiş bir son adım her programa (uygulama, testler, araçlar)
  `src/app/radios.manifest` (UTF-8 kod sayfası, PerMonitorV2, longPathAware, asInvoker,
  Windows 10/11, Common Controls v6), simge, `project(VERSION)`'dan VERSIONINFO, ikili dosya kipi
  (`src/app/crt_binmode.c`) ve 8 MiB yığın ekler. Yalnız `RadioS.exe` GUI alt sistemindedir.
  `radios_wrap()` `--wrap` dikişlerini kurar (DLL'den gelen işlevler için `DLLIMPORT`); `src/`
  altındaki her `.c` dosyasının bir ürün hedefinde olduğu yapılandırmada denetlenir. `exe_manifest`
  ve `exe_pe_*` testleri bunları çalışan programda ve PE başlığında doğrular.
- `RadioSTesting.cmake`: testlerin Python'u gömülü CPython 3.14.8'dir
  (`runtime\python\python.exe -X utf8 -B -s`). Her teste `PATH` (MSYS2 `bin` önde), `TZDIR`
  (`runtime\share\zoneinfo`), UTF-8 Python ortamı ve `RADIOS_AUDIO_BACKEND=none` (D8; `hardware`
  etiketliler hariç) eklenir; GLib'in sıfır testlik `1..0` planı başarısızlık sayılır, zaman
  aşımları `RADIOS_TEST_TIMEOUT_SCALE` ile çarpılır. `radios_add_glib_cases()` vaka adlarını kaynakta
  doğrular, `radios_add_glib_program()` bir programın her vakasını ayrı süreçte koşturur; tek
  vakalık süreçlerde `g_test_skip()` CTest'te "Skipped" görünür (başarılı sayılmaz).
  `tools/win32/radios-test-desktop.c` varsa GUI testleri özel masaüstünde çalışır (D9).
- `RadioSRuntime.cmake`, `RadioSInstaller.cmake`: çalışma ortamı `runtime\`, paket aşaması,
  taşınabilir ZIP ve NSIS kurulumu (`packaging/windows/`).
- `RadioSCheckPe.cmake`, `RadioSCheckImports.cmake`: `cmake -P` betikleri; PE başlığını (alt sistem,
  yığın, ASLR/NX) ve bir test programının içe aktardığı işlevleri `objdump` ile denetler.
- Diğer `*.cmake` dosyaları özellik gruplarının hedef ve testleridir.

Taşıma sürerken beklenen dosyalar varsa kendiliğinden eklenir: `src/audio/virtual_backend.c`,
`tests/radios_test_support.c`, `tests/support/radios_test_runtime_win32.c`,
`tests/header_status_clock.c`, `tests/test_wasapi_hotplug.c`, `tests/test_wasapi_recovery.c`
(`test_audio_recovery.c`'nin yerine), `tools/win32/radios-test-desktop.c`. Başka yeni bir `src/`
dosyası yapılandırmayı durdurur; `CMakeLists.txt`'ye eklenmelidir.

Çapraz derleme (Windows dışı bir derleme makinesi) yalnız genel CMake kavramlarıyla desteklenir:
araç zinciri dosyası `RADIOS_MSYS2_PREFIX`'i ve `CMAKE_CROSSCOMPILING_EMULATOR`'ı verir; derleme
zamanı Windows araçları (MSYS2 `python.exe`, testler) bu emülatörle çalışır. Test programlarına
gömülen kaynak yolu `RADIOS_TEST_SOURCE_DIR` ile verilir (Windows'ta her zaman `CMAKE_SOURCE_DIR`).

Tasarım CSS'i (`design/reference/tasarrım`) derleme sırasında `tools/css_to_gtk.py` ile C'ye gömülür.
GUI testleri (`RADIOS_GUI_TESTS=ON`) pencere açar; yayın bilgisayarında koşturmayın. Kaynak ve
derleme dizinlerini kısa ve ASCII tutun: binutils ve windres UTF-8 yol desteklemez, uzun yol
ilkesi kapalı makinelerde MAX_PATH (260) sınırı geçerlidir.
