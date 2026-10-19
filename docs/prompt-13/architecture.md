# Prompt 13 mimari ve doğrulama kaydı

Son inceleme: 2026-10-09 01:52 UTC. Sorumlu: `/root/p13_architecture`.

## Kaynak ve kapsam

`prompt/13/13.pdf` tek sayfadır; tamamı `pypdf` ile okunmuştur. `figures/sema_1.png` özgün takvim görünümü incelenmiştir: gün/tarih sütunları, dikey saat ekseni, saat aralıklarına yayılan renkli program blokları. PDF ikiye bölünmüş otomasyon sayfası, Excel benzeri koordinat seçimi ve üst bilgi, seçili aralığın kaydı, renkli bloktan program/event düzenleyicisi, sıralama, rotasyon tanımlama ve çoklu eventin başka bloklara kopyalanmasını ister.

`RadioS/AGENTS.md` başka paralel prompt oturumunun prompt/2 kapsamını taşır; bu ekibin kullanıcı talimatı prompt/13 için geçerlidir. Ortak `prompt/agent/AGENT.md` başka takımlar tarafından da yazılmaktadır; güncel içerik korunarak `AGENT.md.lock` üzerinde flock protokolü kullanılır. Kaynak proje git deposu değildir; `git status` 128 ile sonuçlanır.

## Mevcut mimari

- `CMakeLists.txt`: strict C11; GTK3, GLib/GIO, JSON-GLib, curl, FFmpeg ve PulseAudio. `radios_core`, `radios_audio`, `radios_playout` ve `radios_ui` statik kütüphaneleri native uygulamaya bağlanır.
- `src/ui/automation.c`: 1.490 satırlık mevcut rotasyon formu, kaynak playlist sırası/ağırlığı/adedi, saat/gün kuralları, tekrar koruması, jingle, AutoDJ deckleri ve canlı kontroller. `automation` / `main` belgesi gerçek kalıcı CAS akışını kullanır. Mevcut sayfa form+özet düzenindedir; gün/saat hücre takvimi ve bağımsız renkli program blokları yoktur.
- `src/ui/playout_ui.c`, `src/playout/planner.c`: gerçek otomasyon önizleme ve PCM yürütme, playlist/program hazırlama. Yeni takvim kaydı için bu çalışan motorun değiştirilmesi gerekmez; kalıcı takvim ayrı belge türüyle mevcut veri koordinasyonuna bağlanabilir.
- `src/ui/data.c`, `include/radios/data.h`: asenkron GTK owner-lifetime, istasyon kimliği, belge snapshot/dirty/save, CAS ve dayanıklı tekrar altyapısı.
- `database/SDBASE/src/sdbase/desktop_api.py`: istasyon kapsamlı `KINDS`, typed validation, gerçek `desktop_configurations` kaydı, idempotency receipts, revision kontrolü, salt okunur yeniden açma ve belge listeleme. `schema.py` belge validatorünü bu modülden çağırır.
- `schedules.py`, `rotation.py`, `clocks.py` ve `smart_blocks.py` ayrı kapsamlı backend işlevleri sunar; prompt/13 için native hücre/rotasyon event modeline doğrudan hazır UI bulunmaz.

## Eksik gereksinimler ve ayrık uygulama

| Alan | Mevcut durum | Ayrık uygulama noktası |
| --- | --- | --- |
| İki bölmeli takvim ve program/event alanı | Yok | Yeni takvim UI; `automation.c` girişine küçük bağlama |
| Gün/saat aralığı seçimi ve üst koordinat | Yalnız tarih/saat formu | Native takvim hücre seçimi ve seçim üst bilgisi |
| Renkli kayıtlı program blokları | Yok | `automation_calendar` gerçek istasyon belgesi |
| Event/rotasyon satır ekleme ve sıralama | Kaynak playlistte yukarı/aşağı var | Takvim eventinin sıralı rotations dizisi |
| Playlist/sıklık/rastgelelik/reklam/sweeper ayarları | Bazı genel kaynak/jingle kuralları var | Evente ait typed rotation satırı |
| Çoklu event seçip başka bloklara kopyalama | Yok | Yeni bağımsız id'lerle kopyalama; tam belge CAS |

Yeni backend dosyası `desktop_automation_calendar.py`; test dosyası `tests/test_desktop_automation_calendar.py` p13_architecture sahipliğindedir. `desktop_api.py` için yalnız `KINDS` ve `validate_configuration` içine iki küçük anchor, global kilit altında güncel dosya korunarak eklenir. CMake değiştirilmez.

## Doğrulama haritası ve sınırlar

Mevcut `tests/test_desktop_playout.py`, `test_desktop_api.py`, `test_playout_ui.c` ve `tests/run_playout_ui.py` gerçek bağımsız DB/HTTP/Pulse fixture'larını kullanır. Takvim backend kabulü typed alan ve sınırların reddi, istasyon/yetki sınırı, playlist referansı, overlap, kopya id tekilliği, CAS, replay, yeniden açılış ve integrity kontrolünü kapsayacaktır. Native yeni takvim davranışı ayrı GTK kabulü gerektirir; önceki otomasyon/AutoDJ kabulleri regresyon olarak korunmalıdır.

`build/CMakeCache.txt` GUI testlerini OFF gösterir; önceki geniş kabul sayıları bu oturumun test sonucu değildir. İzole yeni build tercih edilmelidir. Salt okunur mimari incelemesi uygulama veya kabul tamamlanması sayılmaz.

## Paralel oturum bulgusu

2026-10-09 01:52:39 UTC anında `src/ui/playout_ui.c` modification time aynı ana ilerledi; başka oturumun bu kaynakta değişiklik yaptığı görüldü. Prompt/13 backend ve takvim UI görevleri bu dosyadan ayrı tutulmalıdır. Kod sahiplikleri ortak koordinasyona bildirilmiştir.

## Devam checkpoint'i

- Tamamlandı: PDF/figure bağımsız içerik kontrolü; mevcut mimari ve test noktaları; kaynak çakışması bildirimi.
- Devam ediyor: `automation_calendar` validator, minimal backend entegrasyonu ve gerçek kalıcı kabul testleri.
- Bekliyor: native takvim/event tasarımının uygulanması, uygun derleme/native kabul, PDF+figure son karşılaştırması.

## Backend kabulü — 2026-10-09 01:55:58 UTC

`desktop_automation_calendar.py` eklendi; `desktop_api.py` içinde yalnız `KINDS` ve lazy validator `elif` entegrasyonu, global `AGENT.md.lock` altında güncel kayıt/dosya okunarak yapıldı. Ortak dosyadaki diğer değişiklikler korunmuştur.

Kalıcı shape: `{schema: 1, events: [...]}`. Her event `id`, `title`, `date`, `start_minute`, `end_minute`, `color`, `rotations` taşır. Her rotation `id`, `playlist_id`, `frequency`, `random`, `track_random`, `advert_random`, `sweeper_random` taşır. Alanlar zorunludur; desteklenmeyen alanlar reddedilir. En fazla 500 event ve event başına 50 rotation vardır; event+rotation UUID'leri takvim çapında tekildir. Aralıklar aynı UTC gün içinde yarı açık `[start,end)` şeklindedir; bitiş 1440 olabilir, gerçek overlap reddedilir. Başlık 160 karakter, renk `#RRGGBB`, sıklık 1..100, rastgelelik alanları boolean'dır. Playlist mevcut istasyonun aktif kaydını göstermelidir; daha sonra disabled olan playlist önceki belgenin okunmasını ve integrity denetimini bozmaz.

| Komut | Sonuç | Kanıt kapsamı |
| --- | --- | --- |
| `python3 tests/test_desktop_automation_calendar.py` | 12/12, 0,967 s | Gerçek bağımsız Store/HTTP; CAS, replay, yeniden açılış/integrity, reorder, kopya UUID, tam typed alan/sınır, overlap, kapasite, yetki ve istasyon sınırı |
| `python3 tests/test_desktop_api.py` | 23/23, 1,449 s | Önceki typed belge/API davranışlarının regresyonu |
| `python3 tests/test_desktop_playout.py` | 15/15, 1,558 s | Önceki otomasyon/playlist hazırlama ve history davranışlarının regresyonu |

Backend görevi tamamlandı. Native UI ve yeni takvim davranışlarının kabulü henüz bu kanıtlara dahil değildir; bütün proje veya bütün PDF gereksinimleri tamamlandı iddiası yapılmaz. CMake bu ajan tarafından değiştirilmedi; yeni testin CTest entegrasyonu root görevindedir.

## Gerçek programme adaptörü — 2026-10-09 02:05–02:08 UTC

Yeni dosyalar: `src/core/automation_calendar_plan.c`, `include/radios/automation_calendar_plan.h`, `tests/test_automation_calendar_plan.c`.

Ana ajan onayıyla takvim satırları mevcut hazırlık/motor yoluna bağlandı. `radios_automation_calendar_prepare_config(event, legacy_defaults, error)` mevcut otomasyon ayarlarını derin kopyalar, eventin UTC günü/saat aralığını ve gerçek playlist kaynaklarını standart `automation` config'e yazar. UI bu config ile mevcut `/v1/workflows/desktop-playout/prepare` salt okunur çağrısını yapar. `radios_automation_calendar_programme(event, prepared, station_id, seed, limit, error)` hazırlanmış doğrulanmış medya kaynaklarından standart `programme` config üretir. Sonrasında mevcut `kind=programme` hazırlığı, C planner ve native transport aynı media/cue/hash kontrollerini uygular. `prepared_programme` yardımcı API'si mevcut C planner kabulü için standart hazırlık yanıtı da verir.

PDF rastgelelik algoritması belirtmez. Kaynaklarda istenen bağımsız seçenekleri uygulamak üzere şu davranış root tarafından onaylandı ve test edildi:

- `frequency`, her rotasyon turunda satırın parça seçim kotasıdır; hem sabit hem rastgele satır için geçerlidir.
- `random`, ilgili satırın kota slotlarını tur içinde dağıtır. Rastgele kapalı satırların kendi aralarındaki sıra korunur.
- Playlistin tür slotları korunur. `track_random`, müzik ve reklam/sweeper dışındaki içerik havuzunu; `advert_random`, `advertisement` havuzunu; `sweeper_random`, `sweeper` havuzunu ayrı karıştırır. Her satır ve havuz ayrı seed akışı kullanır; havuz tükenmeden aynı parça tekrar seçilmez. Havuz bitince yeni tur başlar.
- Önizleme en çok 200 parça üretir. Doğrulanmış tam medya sürelerinin toplamı seçili bloğun bitişini geçmez; sığmayan sonraki parçada plan kesilir ve sınır diagnostic'i üretilir. Eksik/eligible olmayan satırlar diagnostic ile atlanır; yanlış istasyon veya bozuk medya binding'i hata verir.
- Kaynak JSON'lar serialization ve yeni parse ile derin kopyalanır. Yeni programme entry UUID'leri üretilir; gerçek medya/sürüm/dosya/SHA binding'i taşınır. Önizleme kalıcı history veya çalındı kanıtı oluşturmaz.

Strict C11 `-Wall -Wextra -Wpedantic -Werror` standalone derleme geçti. İlk derlemede test dosyasındaki tek `misleading-indentation` uyarısı düzeltildi. Altı core grup hem normal hem ASan/UBSan ve leak detection açık koşulda başarılıdır. Testler gerçek çıktı medya sırasını, frequency ve reorder etkisini, kesin tur kotalarını, sabit satır sırasını, üç bayrağın bağımsızlığını, havuzun tükenmesini, süre sınırını, diagnostic ve istasyon reddini, input immutability'sini ve mevcut `radios_playout_plan_build` yolundaki frozen binding/UTC başlangıç/cue frame'lerini denetler.

Kalıcı kanıt: `build/prompt13/adapter-acceptance.json`. Executable'lar `build/prompt13/test_automation_calendar_plan_standalone` ve `build/prompt13/test_automation_calendar_plan_asan`. Sanitizer yeni adaptör/test translation unit'lerini kapsar; bağlı mevcut static kütüphaneler bu standalone koşuda yeniden instrument edilmemiştir. Bu kabul gerçek PCM tüketimi veya bütün motor kabulü sayılmaz.

Bekleyen: root CMake bağlantısı, native genuine-media önizleme/append/transport kabulü ve PDF/figure son tasarım karşılaştırması. `planner.c`, `playout_ui.c`, `desktop_playout.py` bu ajan tarafından değiştirilmedi; paralel oturum kaynakları korunmuştur.

PDF bağımsız pypdf çıktısı Excel/üst koordinat paragrafını içerir. Requirements ajanının bütün sayfa renderı ve PyMuPDF sıralı metni ek kanıttır: `docs/prompt-13/pdf-text.txt`, `docs/prompt-13/renders/13-page-01.png`; gömülü şema ile figures kaynağının RGB piksel eşliği de requirements tarafından doğrulanmıştır.

## CMake ve mevcut AutoDJ native regresyonu — 2026-10-09 02:09:31 UTC

Root yeni core source ve test hedefini CMake'a ekledi; strict `build/prompt13` radios, core test ve native test hedeflerini başarıyla derledi. `ctest --test-dir build/prompt13 --output-on-failure -R '^automation_calendar_plan$'` gerçek CMake hedefinde **1/1**, altı core grupta **6/6**, **0,15 saniye** başarılıdır.

İki mevcut legacy otomasyon vakası sırayla, ayrı genuine DB/HTTP/Pulse fixture'larında `G_DEBUG=fatal-warnings GDK_BACKEND=x11 python3 tests/run_playout_ui.py build/prompt13/test_playout_ui -p <case>` ile çalıştırıldı:

| Vaka | Sonuç | Kalıcı kanıt |
| --- | --- | --- |
| `/native-playout/automation-preview-history-reopen` | 1/1 PASS; test 25,72 s, wrapper 28,50 s | `build/prompt13/legacy-automation-preview-acceptance.json` |
| `/native-playout/autodj-deck-policy-save-reopen-PCM-append` | 1/1 PASS; test 30,30 s, wrapper 32,67 s | `build/prompt13/legacy-autodj-deck-acceptance.json` |

İlk vaka music→jingle ve tek müzikli devam partisi geçişlerinde en uzun sessiz aralığı **1 PCM frame** ölçtü. Bu iki vaka mevcut legacy AutoDJ PCM, önizleme/history, deck policy, kaydetme ve yeniden açma regresyonudur. Yeni takvim arayüzünün genuine-media kabulü requirements ajanının ayrı görevidir; bütün native/PCM/motor veya bütün proje kabulü değildir.

## Bağımsız native kayıt ve önizleme QA — 2026-10-09 02:14:22 UTC

Bu aşama yalnız salt okunur kaynak incelemesidir; uygulama veya test kaynakları bu ajan tarafından değiştirilmedi. `automation_calendar.c` SHA256 `5491e67aa575c3e5ed4a339503afef2db3835376b517d362502286d8106cbba8`, `data.c` SHA256 `e3199d197ba74a343ff8b6537ec8b585a98dd1965fa367fae6d71e2aee7edb1c`, adapter SHA256 `abbddc2a3afe8dc0f8ed04ba42b3b4b4a15bee2ae49220966bd86f350c18b389` inceleme anındaki kaynak kimlikleridir.

Native ilk vakada ilk 12 kayıttan sonra son inline başlık kaydı zaman aşımına uğradı: yerel başlık `Kopya program düzenlendi`, DB'deki önceki başlık `Kopya program bilgisi`, yerel durum kaydediliyor olarak kaldı. `data.c` bekleyen içerikleri özyinelemeli derin kopyalar; eski onayda bekleyen değişiklikleri temizlemez ve sonraki isteğe yeni revision aktarır. Bu akışta somut CAS, başlık kaybı veya owner-lifetime hatası bulunmadı. Girdi setter'ı geçici boş başlık oluştursa da `save_programme` son boş başlığı reddeder; refresh sırasında `updating` koruması vardır. Salt `ready` kayıt onayı değildir; `revision()` busy/dirty/pending/retry durumunda sıfır döndürür.

Requirements ajanı kesin test düzeneği nedenini buldu; bağımsız kaynak karşılaştırması doğruladı: `radios_runtime_new(16,32)` durum olaylarının hiçbirini tüketmiyordu. Runtime dolu olay kuyruğunda worker'a backpressure uygular; `publish_event` STARTED olayını bile HTTP işinden önce bekletebilir. Gerçek `window.c` timer'ı 50 ms'de olayları pop/free eder. Testte 32 olay birikince son PUT başlamadığı için hata callback'i de oluşmadı. Requirements kendi `pump()` düzeneğine gerçek pencereye denk olay tüketimi ekliyor; yeni native sonucu beklenir. Önceki rate-limit varsayımı kesin neden değildir ve bu bulguyla kapanmıştır. Backend quota veya production veri kodu değiştirilmez.

Önizlemede iki API çağrısı aynı derin event snapshot'ı, generation ve mevcut data-client pointer kimliğiyle doğrulanır. Program veya oturum değiştiğinde geç gelen yanıtın uygulanması engellenir; append ve schedule tekrar aynı koşulları denetler. Mevcut legacy config helper'ından alınan ayarlar derin kopyalanır; event kaynak/zaman ve güvenli preview alanları dışında deck, geçiş ve tekrar koruması alanları korunur. Adapter 200 öğe sınırını ve doğrulanmış tam sürelerin seçili aralığa sığmasını uygular. İkinci `programme` prepare ve C planner gerçek medya/sürüm/dosya/SHA binding'ini tekrar doğrular; readonly hazırlık çalındı/history kanıtı oluşturmaz.

Somut kalan görünürlük farkı root'a ve koordinatöre bildirildi: UI ilk callback'te yalnız config döndüren `radios_automation_calendar_programme` yardımcı API'sini kullanır; adapter'ın `calendar_media_unavailable` ve `calendar_end_boundary` diagnostics'i bu adımda kaybolur. İkinci backend prepare kendi diagnostics dizisini üretir; mevcut readonly önizleme yalnız öğe sayısını gösterir. Root için küçük çözüm önerisi `prepared_programme` sonucundaki diagnostics'i job'da bağımsız kopyayla taşımak ve önizlemede kullanıcıya özetlemektir. Bu fark medya güvenlik kontrolünün geçersiz olduğu anlamına gelmez; kullanıcıya atlama ve süre sınırı açıklaması bekleyen iştir.

Güncel kalan işler: requirements'ın düzeltilmiş harness ile native iki vaka/genuine-media sonucu, root'un diagnostic görünürlüğü kararı ve son PDF/figure karşılaştırması. Geçmiş PASS sonuçları tekrar çalıştırılmadı veya yeni native kaynak kabulü olarak sunulmadı.

## Kesinti sonrası bağımsız son kaynak QA — 2026-10-09 02:21:44 UTC

Sorumlu: `/root/p13_architecture_final`. Önceki mimari ve ortak P13 kayıtları tekrar okunmuştur. Bu ajan yalnız bu mimari belgesine ek yazmıştır; uygulama, test, CMake ve backend kaynaklarını değiştirmemiştir. Başarılı testler sırf tekrar için çalıştırılmamıştır.

`build/prompt13/p13-regression.xml` bağımsız olarak okunmuştur: 5 hedef, sıfır failure/skip; desktop_api, desktop_playout, native_controls, desktop_automation_calendar ve automation_calendar_plan sonuçları önceki kayıtla tutarlıdır. Düzeltilmiş harness'ın 02:14 native kaydı gerçek `1..2`, iki `ok` ve 2,63 s PASS içerir. Yeni diagnostic ve iki satırlı gün başlığı değişikliklerinden sonraki taze native kabulü bu belge yazılırken beklenmektedir.

Root'un yeni `automation_calendar.c` SHA256 `641b68c93e015fc347ef8248e1ccdfacb58ccd5a8ce6e64db5448ea83dd38a0e` kaynağı bağımsız incelenmiştir:

- İlk authenticated automation hazırlığı artık `radios_automation_calendar_prepared_programme` sonucunu kullanır. Bu owned object'teki `config` object ve `diagnostics` array yeni parse ile derin kopyalanır. Adapter kaydı `{reason, source_id, entry_id}` şeklindedir; adapter diagnostics'ine ilk API'nin generic diagnostics'i bağımsız kopyalarla eklenir. İkinci authenticated `kind=programme` hazırlığı gerçek medya bağlarını tekrar doğrular.
- Borrowed API yanıtları değiştirilmez. `view->prepared` yalnız gerçek ikinci API yanıtının derin kopyasıdır; adapter'ın yerel diagnostic'leri gerçek API sözleşmesine sahte alan olarak yazılmaz. İnsan tarafından okunabilen özet, iki API ve planner diagnostic'lerini `reason`/`code` alanlarını destekleyerek birleştirir. Atlanan doğrulanamayan medya ve blok sonunu aşan sonraki öğe önizleme dialog'unda açıklanır.
- `PreviewJob` event/diagnostics/client sahipliğini doğru tutar. `radios_data_request` reddi destroy callback'ini çağırır; normal completion veya owner kapanışında aynı callback job'u bir kez temizler. Generation, event derin eşitliği ve immutable data-client pointer kimliği kontrolleri geç/stale yanıtın uygulanmasını engeller. `preview_notes` hem invalidate hem view cleanup yolunda serbest bırakılır; GTK label metni kendi kopyasını saklar. Yeni yaşam döngüsü veya paylaşılmış JSON mutasyonu hatası bulunmamıştır.
- Ayrı gün başlıkları gün ve tam tarihi iki satır halinde 41 px header alanında çizer; dikey scroll'dan bağımsız yapı korunur. Son native ve ekran görüntüsü bu yeni snapshot için ayrıca doğrulanmalıdır.

PROMPT3'ün frozen broadcast ownership notu da kaynak üzerinden cevaplanmıştır. `radios_broadcast_queue_run_config(ctx, config)` runner'ı playout pointer kimliğiyle eşler ve config'i derin kopyalar. Yeni takvim `preview_schedule` yolu `radios_playout_ui_start_prepared` kullanır; başarılı plan kurulumundan sonra `radios_broadcast_queue_run_config(ctx, NULL)` eski yayın sırasının event kimliklerine sahipliği temizler. Böylece yeni takvim entry UUID'leri eski sıra başlığına veya event'ine yanlış bağlanmaz. Kabul edilen takvim planı controller'ın immutable `p->plan` kaydıdır. Programa ekleme mevcut append akışına gider; sonraki programme start kendi accepted config'ini bağlar. Bu inceleme fiziksel veya takvimden gerçek PCM yürütme kabulü değildir.

Değişmeyen sahipli kaynak hashleri:

| Dosya | SHA256 |
| --- | --- |
| `src/core/automation_calendar_plan.c` | `abbddc2a3afe8dc0f8ed04ba42b3b4b4a15bee2ae49220966bd86f350c18b389` |
| `include/radios/automation_calendar_plan.h` | `d2a9b67acaa2b3d88709ebdb5da7c10100c3cfdc1f009bb2dafec95c66e2a486` |
| `tests/test_automation_calendar_plan.c` | `3a7c628157a7c1b795c0659e6c2b0862b105d60cd52728c5ea61dc75411596db` |
| `database/SDBASE/src/sdbase/desktop_automation_calendar.py` | `9f2c6235d994c2246aff8a15f0d0eea48ae418eb81de888f32e374d38467ae3c` |
| `tests/test_desktop_automation_calendar.py` | `b780b30121dfd8ee4de7ad59e2d16f8884774f93aac21ccd9074ed4213485e65` |

Geçerli kabul sınırları korunur: adapter ASan/UBSan yalnız yeni adapter ve test translation unit'lerini kapsar; bütün linked legacy kütüphaneleri kapsamaz. Native yeni takvimden gerçek medya hazırlığı ve kalıcılık test edilir; fiziksel çıkışla yeni takvim oynatma testi yapılmamıştır. İki eski AutoDJ gerçek PCM fixture regresyonu ayrı PASS kanıtlarıdır. Bütün proje testlerinin tamamının geçtiği veya başka prompt oturumlarının bütün değişikliklerinin kabul edildiği iddia edilmez.

## Son native ve görsel kabul teyidi — 2026-10-09 02:23:22 UTC

Önceki checkpoint'te beklenen yeni snapshot kabulü tamamlanmıştır. `build/prompt13/native-calendar-final-acceptance.json`, `p13-native-calendar-final.xml` ve `.log` bağımsız olarak okunmuştur. Gerçek koşu 02:22:33.847199–02:22:36.499626 UTC arasında **exit0**, **1/1 CTest hedefi, 2/2 GLib vakası, 2,64 s**, sıfır failure/skip ile geçmiştir. Takvim/native/core/backend/CMake kaynakları ile native executable'ı kapsayan 13 SHA256 kaydı koşu öncesi ve sonrasında aynıdır; `changed_during_test` boştur.

Gerçek 2 sn PCM medyası 1 dk bloğa tam **30 öğe** üretmiştir; GUI `automation-calendar-preview-notes` label'ında `blok sonunu` açıklamasının görünmesi, takvim revision/config read-only kalması ve sonradan program düzenlenince append/schedule eylemlerinin geçersizleşmesi başarıyla doğrulanmıştır. Maxdate 9999 uç haftası, geçersiz trailing günler ve opsiyonel zoom davranışları da son native test kapsamındadır. Önceki pending diagnostic/header kabulü bu taze sonuçla kapanmıştır.

Son `docs/prompt-13/renders/native-calendar.png` dosyası ayrıca görüntülenmiştir: SHA256 `b888627b39c08ed6c1ebd118d6453b63db43bf0ea7856d27cbeb19b4924ac077`, mtime 02:22:35.823897 UTC. Yedi günün tam tarihi iki satırda okunur; sabit başlık, mavi ve yeşil renkler, farklı süreli program blokları, sağ rotasyon satırı ve alt sağ `Rotasyon tanımla` kaynak şemayla tutarlıdır. Durum metni `Takvim veritabanıyla eşitlendi.` gösterir. Root'un son strict beş-target build exit0 kaydı ortak koordinasyonda yer alır.

Bu ajan açısından mimari sözleşmesi, son yaşam döngüsü incelemesi ve gerçek kabul kanıtı doğrulaması tamamlandı; üretim veya test kaynaklarına ek değişiklik gerekmedi. Yukarıdaki fiziksel transport, bütün proje ve sanitizer kapsamı sınırları aynı şekilde geçerlidir.
