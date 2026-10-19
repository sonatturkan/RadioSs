# Akım 4c · Küme / kümesiz çalışma raporu (ajan: kume)

Kaynak: `prompt/radyo_otomasyonu_kilavuzu.pdf` §03 "Kümeler, geçişler ve görünüm", §08 "Kümeli ve kümesiz çalışma", §08 "7/24 otomasyon ve doğrulama". Derleme/koşum yalnız `build-4c`; çalışan uygulamaya ve 8000 portundaki servise yazılmadı (servis yalnız okundu: `desktop/list`, `desktop-playlists/list`, `desktop-playout/prepare`).

## Değiştirilen dosyalar

| Dosya | Değişiklik |
|---|---|
| `src/core/automation_scope.c` | (1) Küme kapsamı çözümünde `autodj_mode` üye sayısından türetilir: 2+ üye → oynatıcılar arası AutoDJ, tek üye → Yarı AutoDJ (eski: ilk üyenin bireysel bayrağı). (2) `radios_automation_scope_route_event` kapsam dışı kayıtlı havuz oynatıcılarını düşürür, boş kalan türü kapsamın tüm oynatıcılarına düşürür; biçimsiz havuz (tam sayı değil, 1..32 dışı, yinelenen, eksik tür) hata kalır. Yeni `radios_automation_scope_route_event_notice(...)` düşürülen oynatıcıları Türkçe cümleyle bildirir. |
| `include/radios/automation_scope.h` | Yeni prototip + yorumlar. |
| `src/ui/player_scope_execution.c` | `schema_resolve` uyarıyı `ctx->notify` ile iletir (aynı metin art arda yinelenmez; `ScopeJob.route_notice`). `job_new` OFF modunu `binding.autodj_mode`'dan türetir (küme → birlikte). |
| `src/ui/automation_workspace_ui.c` | `transport()` kümeli kaynakta çözülen küme modunu kullanır (Play ve "Şemayı çalıştır"). UI kontrolleri değişmedi. |
| `database/SDBASE/src/sdbase/desktop_player_topology.py` | `group_players(config, cluster_id)` ve `route_pools(config, cluster_id, pools)` yardımcıları (C ile aynı kural ve aynı Türkçe cümle). Henüz çağıran yok: `desktop_api.py` prepare (automation) ya da `desktop_automation_calendar.py:autodj()` sahibi isterse 2 satırla bağlar. |
| `tests/test_automation_scope.c` | +4 vaka (`cluster-mode-follows-membership`, `out-of-scope-pools-fall-back`, `pool-change-between-rows`, `independent-players-stay-separate`); `routing-bounds` vakası kapsam dışı havuzun artık hata olmamasına göre güncellendi. |
| `tests/test_automation_workspace_ui.c` | Sarmalayıcı `dispatched_mode` kaydeder, projeksiyon sarmalayıcısı gerçek `automation_id` döndürür; +1 vaka. |
| `tests/test_desktop_player_topology.py` | +1 vaka (`test_group_pools_clamp_to_defined_players`). |

CMakeLists.txt'e dokunulmadı (yeni ikili yok).

## PDF maddesi → bulgu → düzeltme → doğrulama

### 1. §08 "Kümeli ve kümesiz çalışma. Bozulan iki çalışma biçimi…"

**Bulgu A (kümeli, gerçek hata).** Küme kapsamı ("Şemayı çalıştır" / Play, Otomasyon sekmesi ve üst çubuk) çalışma düzeyini kümenin **ilk üyesinin bireysel** `autodj_mode` bayrağından türetiyordu (`automation_workspace_ui.c:transport()` eski satır `mode = source->autodj_mode == OFF ? … : source->autodj_mode`; `header_navigation.c:scope_mode()` `binding.autodj_mode`). Kümeli oynatıcının bayrağı arayüzden düzenlenemez (kart ve Otomasyon formu kümelide gizli/erken dönüş) ama `broadcast.c:player_autodj_default()` bir rotasyon/playlist yüklenince Manuel oynatıcıyı **Yarı AutoDJ**'ye alır; oynatıcı sonradan kümeye taşınınca bayrak kalır. Sonuç: 2+ üyeli küme tek oynatıcıda, geçişsiz çalıyordu (`install()`: SEMI → `count = 1`, `calendar_routing` silinir). Yeniden üretim: `test_automation_scope.c:/automation-scope/cluster-mode-follows-membership` (küme {1,3}, oynatıcı 1 SEMI → eski kodda `binding.autodj_mode == SEMI`), GUI: `test_automation_workspace_ui.c:/automation-workspace/cluster-play-group-transitions` (küme düğümünde Play → eski kodda `play_programme(..., SEMI)`).
**Düzeltme.** `radios_automation_scope_resolve` küme kapsamında `autodj_mode = count > 1 ? TRANSITIONS : SEMI`; `transport()` ve `job_new()` bunu kullanır; oynatıcı düğümü kendi tanımını izler (tek oynatıcıda Yarı AutoDJ sürer). Üst çubuk (`header_navigation.c`, başkasının dosyası) aynı `binding.autodj_mode`'u okuduğu için kendiliğinden düzeldi.
**Doğrulama.** `build-4c/test_automation_scope` 12/12; `ctest -R native_automation_workspace` 9/9 (Xvfb, kullanıcı ekranına pencere yok): küme düğümü Play ve Şemayı çalıştır → TRANSITIONS; üye düğümü → SEMI; tek üyeli küme → SEMI; tanım yazılmaz (`writes` değişmez).

**Bulgu B (kümeli + kümesiz, gerçek hata).** `radios_automation_scope_route_event` kayıtlı `autodj` / `autodj_players` havuzunda kapsam dışı bir oynatıcı görünce (`distribution_valid`) "Otomasyon kanal dağılımı seçili kapsam dışında veya yineleniyor." ile NULL döndürüyor, `schema_resolve` işi **bitiriyordu** (ERROR durumu, yayın durur; neden ve hangi oynatıcı söylenmiyordu). Tipik yol: 4 oynatıcı için düzenlenmiş rotasyon {1,2,3,4} iki üyeli kümede {1,3}; ya da kümesiz tek oynatıcıda {1,2} havuzu. Yeniden üretim: `/automation-scope/out-of-scope-pools-fall-back`, `/automation-scope/independent-players-stay-separate`.
**Düzeltme.** Havuz kapsamla kesiştirilir (kayıtlı sıra korunur), boş kalan tür kapsamın tüm oynatıcılarına düşer; uyarı: "Şemadaki Oynatıcı 2, Oynatıcı 4 bu kümede yok; kümenin oynatıcıları kullanılıyor." (Kümesizler: "… Kümesizler içinde yok; Kümesizler oynatıcıları kullanılıyor.", oynatıcı: "… bu oynatıcının kapsamında yok; kapsamın oynatıcıları kullanılıyor."). `schema_resolve` uyarıyı bir kez `notify` eder ve çalmayı sürdürür. Biçimsiz havuz yine hata.

**Yeniden üretildi, hata yok (kümeli).** Geçişler küme üyeleri arasında döner: uçtan uca K1 (küme {1,2}, 31 öğe, oynatıcı 1-2-1-2…), K5 (üç üye + sweeper, 1-2-3 dönüşü), K6 (dört üye); kapsam içi kayıtlı havuz genişletilmez (ALL12 {1,2} dört oynatıcılı kümede {1,2} kalır — `yapilanlar-arsivi` 140 ile uyumlu). Küme üyesi değişince binding geçersizleşir: `stale-binding-fences`, `cluster-mode-follows-membership` (üye çıkışı → "Otomasyon ataması veya oynatıcı kapsamı değişti; yeniden hazırlayın."), çalışan/bekleyen işte topoloji değişikliği `radios_player_scope_execution_validate_topology` ile reddedilir (mevcut).

**Yeniden üretildi, hata yok (kümesiz).** Tek bağımsız oynatıcı (Yarı AutoDJ): T1/T2 — havuz {1}, bütün geçişler EOF, bindirme/beatmatch istense de tek oynatıcıda sıralı; L1 gerçek 7/24 şeması canlı topolojideki küme "a" (yalnız Oynatıcı 7) ile 64 öğe. İki bağımsız oynatıcı birbirini devralmaz: `independent-players-stay-separate` — iki SEMI bağımsız oynatıcının binding'i yalnız kendisi; komşuyu adlandıran kayıtlı havuz komşuya yönlenemez (uyarıyla kendi oynatıcısına düşer); komşu yalnız kullanıcının "AutoDJ geçiş" seçimiyle havuza girer (T4: 5→6 çifti). Kümesizler toplamı: T3 (3 oynatıcı, dönüşümlü). Geçersiz oynatıcı kimlikleri: `routing-bounds` (33, yinelenen, tam sayı olmayan, boş kapsam, yinelenen deck) hata; sessiz ret/çökme yok.

### 2. §03 "Seçimler, mevcut oynatıcı sayısı ve track yerleşimiyle uyumlu olmalıdır"

Bulgu B'nin düzeltmesi bu maddeyi karşılar: uyumsuz kayıt yayını durdurmaz, Türkçe ve nedenini söyler, grubun geçerli üyelerine düşer. Doğrulama: `out-of-scope-pools-fall-back` (müzik {1,2,3,4} → {1,3}; jingle {4,2} → tüm küme {1,3}; reklam {1,3} aynen; özgün event değişmez; kapsam içi havuzda uyarı NULL), Python `test_desktop_player_topology.py::test_group_pools_clamp_to_defined_players` (aynı kural, aynı cümle). Uçtan uca K2 (dört oynatıcılı havuz iki üyeli kümede → havuz [1,2], plan 34 öğe) ve K3.

### 3. §03 "Kümeler arası geçiş … birbirini devralması"

Kapsam yürütmesi bir çalıştırmada **tek grubun** oynatıcılarını kiralar: `route_event` havuzları kapsama kırpar, `playout_ui.c:1053` (`radios_player_topology_same_group`) ve `:1091` ("Şemadaki oynatıcı dağılımı seçili kapsamın dışında") deck listesini aynı grupla sınırlar. Bu yüzden bir satırın havuzu A kümesinde, sonrakinin havuzu B kümesinde ise, A kümesinden başlatılan çalıştırma eskiden **reddediliyordu** (route_event hatası, yayın durur); şimdi B satırı A'nın oynatıcılarına düşer, çalma sürer ve devir sınırındaki `calendar_transition` ipucu (çekirdek: "2 2 2" / `link_transitions`) aynen uygulanır — `pool-change-between-rows` (satır havuzları {1,3} → {3} → {2,4}: ilk ikisi aynen, üçüncüsü {1,3}; `radios_automation_calendar_prepare_config` kabul eder), uçtan uca K3 (A {1,2}, B {3,4} → B [1,2], 31 öğe, geçişler XF) ve K4 (küme içi havuz daralması {1,2} → {2}: planlayıcı aynı fiziksel oynatıcıya düşen bindirmeyi kesmeye çevirir, `routing_overlap_dropped`, plan geçerli). Gerçek anlamda iki kümenin aynı çalıştırmada birbirini devralması (A'nın oynatıcısından B'nin oynatıcısına crossfade) `playout_ui.c` aynı-grup kuralı ve motor kiralaması değişmeden mümkün değil; bu dosyalar başka oturumun. Küme → küme devri için mevcut yol: yerel event `playback.next_cluster_id` (`local_event_ui.c`, `event_playout.c`) — event bitince seçilen küme kapsamı başlar.

### 4. §08 "7/24 otomasyon ve doğrulama"

`tools/otomasyon_testleri/` README'deki `build.sh` çıktıyı `build/` altına yazdığı ve `senaryolar.py` "RS · …" playlistlerini beklediği (veritabanında yok; "TEST · …" de yok, oluşturulmadı) için aynı derleme komutu `build-4c/otomasyon_testleri/e2e` hedefiyle elle çalıştırıldı, Python sarmalayıcı scratchpad'e kopyalanıp `H`/`E2E` yolları build-4c ve scratchpad'e çevrildi (`--kaydet` yok, DB'ye yazma yok). Senaryolar mevcut "7/24 · …" playlistleri ve kayıtlı "RadioS 7/24 Yayın Akışı" şemasıyla kuruldu (`scratchpad/harness/kume_senaryolar.py`; ayrıca `native_decks.c` yerel planın satır başına seçtiği fiziksel oynatıcıyı yazdırır). Zincir uygulamayla aynı: occ → route_event → prepare_config → backend prepare(automation) → takvim planı → backend prepare(programme) → merge → yerel plan.

**Ön koşum (çekirdek planlayıcı değişikliği öncesi, 01:25): 16/16 geçti; WOULD_BLOCK/retry yok.**

| Vaka | Düzen | Sonuç |
|---|---|---|
| K1 | küme {1,2}, varsayılan havuz | ✓ 31 öğe, 31 XF, havuz [1,2], oynatıcı 1-2-1-2 dönüşü |
| K2 | kayıtlı havuz {1,2,3,4} iki üyeli kümede | ✓ 34 öğe, havuz [1,2] (daraltıldı), 31 XF |
| K3 | kümeler arası havuz A {1,2} / B {3,4} | ✓ 31 öğe, B [1,2]'ye düştü, geçişler sürdü |
| K4 | küme içi satır havuzu {1,2} → {2} | ✓ 31 öğe, 20 XF + 11 EOF (tek oynatıcıya düşen bindirmeler kesmeye çevrildi) |
| K5 | üç üyeli küme + sweeper | ✓ 45 öğe (33 müzik, 12 sweeper), havuz [1,2,3], 1-2-3 dönüşü |
| K6 | dört üyeli küme, dört kaynak, XF+beatmatch | ✓ 31 öğe, havuz [1,2,3,4], 15 XF + 16 XF+BS |
| T1 | tek bağımsız (Yarı), havuz {1,2} | ✓ 30 öğe, havuz [1], 30 EOF |
| T2 | tek bağımsız (Yarı), beatmatch istenmiş | ✓ 33 öğe, havuz [3], 33 EOF |
| T3 | Kümesizler 3 oynatıcı | ✓ 32 öğe, havuz [1,2,3], 32 XF |
| T4 | bağımsız 5 + devam 6 | ✓ 31 öğe, havuz [5,6], 31 XF |
| L1 | gerçek 7/24, canlı topoloji küme 'a' = Oynatıcı 7 (Yarı) 10:05 | ✓ 64 öğe, havuz [7] |
| L2 | gerçek 7/24, iki üyeli küme 03:30 | ✓ 43 öğe, havuz [1,2] |
| L3 | gerçek 7/24, dört üyeli küme 18:45 | ✓ 44 öğe, havuz [1,2,3,4] |
| L4 | gerçek 7/24, Kümesizler 23:58 (gece yarısı) | ✓ 1 öğe (kuşağa 2 dk kaldı), havuz [1] |
| L5 | gerçek 7/24, hafta dönüşü Pazar 23:55 | ✓ 3 öğe, havuz [1,2] |
| L6 | gerçek 7/24, kuşak sonuna 40 sn | ✓ 2 öğe, calendar_soft_end_overrun |

Çıktı: `scratchpad/out/sonuclar_kume.json` (ajan scratchpad'i); `calendar_no_legal_candidate` tanısı sentetik rotasyonlarda boş geçmişle beklenen seçim tanısıdır, hata değil.

## Koşulan komutlar ve sonuç satırları

```
cmake --build build-4c --target test_automation_scope test_automation_workspace_ui test_player_topology test_player_tree_ui test_automation_scope_ui radios --parallel 4   → uyarı/hata yok (01:17)
cd build-4c && ctest -R "^(player_topology|automation_scope|desktop_player_topology|native_player_tree|native_automation_workspace|automation_scope_fetch|automation_calendar_plan)$"
   → 100% tests passed, 7/7 (automation_scope 12/12 vaka; native_automation_workspace 9/9 vaka, özel Xvfb)
build-4c/test_automation_scope   → ok 1..12 (yeni: 8 cluster-mode-follows-membership, 9 out-of-scope-pools-fall-back, 10 pool-change-between-rows, 11 independent-players-stay-separate)
make -C build-4c CMakeFiles/test_player_workspace_ui.dir/tests/test_player_workspace_ui.c.o CMakeFiles/test_automation_workspace_acceptance.dir/tests/test_automation_workspace_acceptance.c.o → derlendi (PCM testleri koşulmadı)
cc … tools/otomasyon_testleri/e2e.c … build-4c/libradios_{playout,core,audio}.a -o build-4c/otomasyon_testleri/e2e   (build.sh'nin build/ yerine build-4c sürümü)
python3 scratchpad/harness/kume_senaryolar.py   → 16/16 geçti (ön koşum 01:25; tablo yukarıda)
```

## Doğrulanamayanlar / notlar

- Gerçek PCM gerektiren `native_player_workspace`, `native_automation_workspace_acceptance` (CMakeLists'te `run_audio_ui.py` ile tanımlı: pactl null-sink + gerçek ses) ve `native_playout` koşulmadı (kural). Kodları derlenir durumda; davranış değişikliği yalnız küme kapsamı modunda ve kapsam dışı havuzun artık hata olmamasında.
- O2(c) `AutoDJ-interactive-marker-save-playlist-pair-roundtrip-MAIN-PCM` ("expected state3; actual state0"): kapsam/topoloji katmanı bu vakada {0,1} çiftini TRANSITIONS ile kabul ediyor (`player_options_changed` → `set_autodj(…TRANSITIONS, hedef)`, `player_scope_start` → `play_programme(PLAYER 0, TRANSITIONS)` → `install` count 2). Başlamama, kiralama aşamasında olmalı: `playout_ui.c:replacement_ready()` (satır ~693-708) yalnız `owned || EMPTY` deck'leri kullanılabilir sayar; testte hem deck 0 (a) hem deck 1 (b) READY dolu yüklü → "Devralma için boş veya sıraya ait MAIN oynatıcısı gerekli" ya da `install_plan` eşdeğer koşulu. Doğrulanamadı (PCM); playout_ui.c sahibine ipucu.
- Küme üyesinin bireysel `autodj_mode`/`autodj_target` bayrakları veri modelinde kalıyor (fingerprint'e giriyor); küme çalışması artık onları okumuyor. Tek oynatıcı kümesi her zaman Yarı AutoDJ'dir.
- `desktop_player_topology.py` yardımcıları arka uçta henüz çağrılmıyor (sahibi karar verir).
- 01:28-01:33 arasında başka oturumların `src/audio/channel_input.c` ve `src/ui/audio_ui.c` (`read_array`/`read_object` örtük bildirimi, satır 327/334) dosyaları build-4c'de derlenmedi; radios_ui/radios_audio'ya bağlı ikililer (`radios`, GUI testleri) yeniden bağlanamadı. Benim GUI test ikililerim (01:21, bütün kaynak değişikliklerimi içeriyor) 7/7 geçti; çekirdek kütüphane (`radios_core`) bağımsız olduğundan `test_automation_scope`/`test_player_topology` güncel derlendi.
- 01:23'te başka bir oturum `CMakeLists.txt`'e `tests/test_live_control.c` ve `tests/test_programme_window.c` kaynakları olmayan iki `add_executable` ekledi; `cmake --build build-4c` o andan sonra configure aşamasında duruyor (CMake Error: Cannot find source file). Benim son ctest koşumlarım bu değişiklikten önceydi; `independent-players-stay-separate` vakası aynı kütüphanelerle elle (`cc -Wall -Wextra -Werror … build-4c/libradios_core.a`) derlenip 12/12 doğrulandı. Kaynaklar eklenince `cmake --build build-4c --target test_automation_scope test_automation_workspace_ui` + ctest yeniden koşulmalı.
