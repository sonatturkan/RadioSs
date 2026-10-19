# Otomasyon zor kombinasyon testleri ve düzeltmeler · 14–15.10.2026

Otomasyonun tamamı (rotasyon, takvim, haftalık şema, AutoDJ geçişleri, olasılık politikası,
şarkı ortası katmanlar, şarkı işaretleri) uygulamanın **Şemayı çalıştır** yolundan uçtan uca,
gerçek C kütüphaneleri ve gerçek veritabanı servisiyle sınandı. Önce en zor kombinasyonlar
çalıştırıldı, bulunan hatalar sonra düzeltildi ve her düzeltmeden sonra bütün senaryolar
yeniden koşuldu.

- Düzenek: `tools/otomasyon_testleri/` (kullanım `README.md`'de). Yaklaşık 110 senaryo +
  24 işaret kombinasyonu.
- Tarihsel koşum: otomasyonla ilgili 16 test grubu geçti. Arayüz dışı 82 testlik koşumda
  `song_seek` abort oldu, `youtube_music_audio` atlandı; bu koşum 82/82 başarılı sayılmaz.
  `song_seek` daha sonra tek başına geçti. Güncel ve yalıtılmış doğrulama aşağıda ayrıca belirtilmiştir.

## Bulunan ve düzeltilen hatalar

| # | Hata | Etki | Düzeltme |
|---|---|---|---|
| 1 | Kısa öğeye (10 sn sweeper, 3 sn cue aralığı) uzun beatmatch/crossfade bindirmesi | Bütün kuşak reddediliyordu ("Beatmatch bindirmesi … kısa olmalı") ya da zaman ilerlemiyordu (2 saatte 170 sweeper üst üste) | Bindirme kısa öğenin en çok %40'ı: `planner.c` add_entry (son öğe dahil) ve takvim planlayıcısı `candidate_start` aynı sınırla |
| 2 | Tempo uzaması beatmatch'i 30 sn'nin üstüne çıkarınca | Plan reddi | O çift crossfade'e, gerekirse 30 sn'ye düşer (`calendar_beatmatch_plan`) |
| 3 | Ardışık iki öğe aynı tek oynatıcıya bağlıyken bindirme | Plan reddi | Yalnız o geçiş kesmeye döner, zamanlar yeniden hesaplanır (`calendar_route_plan`, tanı `routing_overlap_dropped`) |
| 4 | Tek oynatıcılı planın son öğesi crossfade | "En az iki oynatıcı seçin" reddi | Son öğenin çıkış geçişi kesme olur |
| 5 | Şema yolunda kuşak sonu kırpması yoktu; seçim hazırlıktan 1–3 sn önce yapılıyordu | Kuşağa geç katılımda plan reddediliyor, **şema işi tamamen duruyordu** (dünkü canlı hata) | `playout_ui.c` şema başlatmasında `trim_calendar_end`; `player_scope_execution.c` seçim zamanı +5 sn; pencere hatası iş sonlandırmak yerine 2 sn sonra yeniden dener; kuşağın son 5 sn'sinde kuşak bitişinde yeniden dener |
| 6 | Blok (event) olasılık politikası | Backend kaydediyor, C haftalık doğrulayıcısı reddediyordu → şema hiç çalışmıyordu | `weekly_programme_plan.c` alan listesine `probability_policy` |
| 7 | Tek bir çalınamayan kaynak (boş playlist, uyuşmayan tür filtresi, yalnız YouTube, silinmiş/devre dışı playlist) | Bütün kuşak susuyordu | Kaynak tanıyla (`calendar_source_unplayable`) atlanır; hiçbir kaynak çalınamıyorsa eski hata |
| 8 | Kuşak sonunda yalnız sweeper sığınca | 12–28 sweeper art arda | Sweeper/jingle başka bir sweeper/jingle'ın ardından gelemez |
| 9 | Kuşak sonunda sığacak parça kalmayınca | 1,5–6 dk ölü hava (her kuşak sonunda) | İsteğe bağlı **yumuşak kuşak sonu**: `soft_end_seconds` (0–600, varsayılan 0 = eski sert bitiş). Son parça kuşağı en çok bu kadar aşabilir; yürütücü o parça bitene kadar keser­mez |
| 10 | Tekrar aralıkları adayları tüketince | Kuşağın kalanında sessizlik | İsteğe bağlı **kural gevşetme**: `relax_gaps` (varsayılan kapalı). Önce sanatçı/albüm, sonra rotasyon şarkı aralığı gevşer; şarkı kuralındaki açık aralık ve art arda aynı şarkı asla gevşemez (tanı `calendar_rules_relaxed`) |
| 11 | Olasılık politikası (yalnız geçiş ya da şarkı ortası katman için bile) tanımlıyken | Kaynak ağırlıkları, karıştırma ve rotasyonlar arası paylar sessizce yok sayılıyordu | Öncelik: olasılık yüzdesi > ağırlık > sırayla. Olasılık yolu yalnız ilgili kapsamda grup varken kullanılır (radios-52 ile uyumlu; arayüzdeki "Olasılıklı: belirlenen ağırlıklara göre seç" ağırlığı olasılık tanımı sayar) |
| 12 | Olasılık çekilişi kalan paya düşünce | Kaynaktan parça gelmiyordu | Uygun şarkılar arasında doğrusal yedek seçim |
| 13 | Şarkı sırası "Doğrusal" iken "Reklam/Sweeper rastgeleliği" | Kutu işaretlenebiliyor ama etkisizdi | Reklam/sweeper yerleri yerinde kalır, o yerlere giren öğeler kendi havuzundan karışık gelir |

Değişen dosyalar: `src/core/automation_calendar_plan.c`, `src/core/weekly_programme_plan.c`,
`src/playout/planner.c` (add_entry, calendar_constraints_plan değil, calendar_beatmatch_plan,
calendar_route_plan, plan sonu tek oynatıcı adımı), `src/ui/playout_ui.c`,
`src/ui/player_scope_execution.c`, `src/ui/header_navigation.c`,
`database/SDBASE/src/sdbase/desktop_automation_calendar.py`.

## Arayüz

- **Üst çubuk ▶ ve "Program akışı":** "Sonraki playlist" satırında kayıtlı haftalık programlar
  listelenir (atanmış olan "· atanmış" ile seçili gelir). ▶, bir program seçiliyse ya da seçili
  yayın sırası çalınabilir değilse o programın **o saatteki kuşağını** yükleyip başlatır
  (Yayın › Şema › Şemayı çalıştır ile aynı yol; geç katılım, bekleme ve tekrar dahil). Seçilen
  program atanmış olandan farklıysa önce doğrulanıp atanır. "Program akışı" boş bırakılırsa ▶
  eskisi gibi seçili yayın sırasını çalar.
- `soft_end_seconds` ve `relax_gaps` kontrolleri rotasyon düzenleyicisinde mevcut.
  15.10 devamında gerçek HTTP kaydı, hatalı giriş reddi ve yeniden açılışla doğrulandı.
- Arayüzde bağlanmamış düğme taraması yapıldı (otomasyon arayüz dosyaları): bağlantısız ya da
  boş işlevli düğme bulunmadı. Çalışmayan kontrol olarak yalnız #13 vardı.

## Testler

- Yeni: `continuity-no-consecutive-short-items`, `continuity-soft-end-relaxed-gaps-opt-in`
  (`test_automation_calendar_plan.c`), `event-probability-policy-accepted`
  (`test_weekly_programme_plan.c`).
- Yeni davranışa göre güncellenen: `source-zero-unavailable-reasons-fences` (etkin ama erişilemeyen
  playlist artık atlanıyor; yapısal kontroller aynı), `routed-lookahead-strict-validation` (aynı
  tek oynatıcıdaki bindirme yalnız o geçişte kesme), `schema-current-first-consumed-checkpoint`
  (devam noktası açık kalan gruplarıyla sınanıyor).
- Ortam kaynaklı düşenler (değişikliklerle ilgisiz): `tempo` ve `automation_scope_fetch`
  sistemde tr_TR C yereli olmadığı için; `runner` ve `song_seek` paralel yük altında zaman
  zaman (tek başına geçiyorlar).


## 15.10.2026 · Güncel kaynakla güvenli devam doğrulaması

Kapanan iki oturumdan kalan %0 yedek-seçim ve bozuk devam-kaydı hataları, işaret tabanlı erken geçiş, kaydırılmış gerçek başlangıç saatinde kural değerlendirmesi ve vokal güvenli jingle düzeltildi. Takvimde çalınamaz blok nedenleri görünür; çekirdek hata mesajı blok/tarih/saat içeriyor. Sade olasılık arayüzünün mevcut kontrolleri gerçek kayıt ve yeniden açılışla sınandı.

Tarihsel `tools/otomasyon_testleri/api.py` normal kimlik bilgilerini içe aktarır; bu devamda doğrudan çalıştırılmadı. Yeni `tests/run_automation_stress.py`, aynı gerçek C sürücüsünü özel veritabanı/HTTP ve üretilmiş WAV ile kullanır. Son ana derlemede 13/13 hedefli zor senaryo geçti; eski yaklaşık 110 senaryonun tamamının yeniden çalıştırıldığı iddia edilmiyor.

Güncel kanıt: `build/claude-recovery-20261015/final/targeted-ctest.log` (7/7 kayıtlı test grubu), `final/registered-native-jingle.log` (gerçek MAIN PCM), `calendar/context-results.json` (49+9), `policy-STATUS.json` (114 politika/planlayıcı/backend/ses vakası), `ui-revalidation.json` (8 UI +11 taslak) ve `runtime/` (zaman aşımı, mevcut/gelecek şema, sayaç ve gerçek ses). Ana ikili katı uyarılarla derlendi; sürüm ve yapılandırma denetimi geçti.

Aşağıdaki canlı yapılandırma ve açık sorular, önceki oturumun tarihsel gözlemidir. Bu devamda gerçek istasyon verisi değiştirilmedi; ABBA işareti, program ataması ve TEST kayıtları kendiliğinden düzenlenmedi. Açık RadioS'un MAIN ve internet yayını doğrulandı ve korunmuştur; görünen servis sürümü uyarısını giderecek yeniden başlatma canlı yayında yapılmadı.

## Canlı yapılandırma

"RadioS 7/24 Yayın Akışı" (rev 5): olasılık politikası olan rotasyonlarda kaynak payları playlist
olasılık gruplarına çevrildi; bütün rotasyonlarda `soft_end_seconds` 300 ve `relax_gaps` açık.
Uçtan uca denemede her kuşak sonu ölü hava yerine +13…+238 sn taşmayla kapanıyor; istasyon
ID'leri en az 9 dk arayla ve hiçbir zaman art arda gelmiyor.

## Bilinçli tasarım notları

- Sert kuşak sonu varsayılan kaldı: mevcut sözleşme ve testler kuşak bitişinin tam olmasını
  istiyor; yumuşak son rotasyon başına açılır.
- Playlist ve şarkı saat pencereleri katıdır; yumuşak son yalnız kuşak bitişini esnetir. İki
  playlist penceresi arasında kalan birkaç dakikada (ör. 10–11 Power, 11–12 Gold) boşluk olabilir.
- Şarkı tekrar aralığı 0 ise aynı parça art arda gelebilir (mevcut sözleşme). En az 1 dk önerilir.
- Beatmatch gelen parçanın girişini vuruşa hizaladığı için parti planından erken bitebilir;
  yürütücü yeniden doldurur, yumuşak son açıkken boşluk yalnız yeniden hazırlık süresi kadardır.

## Açık sorular

- **Gimme! Gimme! Gimme!** parçasında AutoDJ çıkış işareti 0:27'de: otomasyon şarkının yalnız ilk
  27 saniyesini çalıyor. Kasıtlı mı?
- Küme şu an "Yeni program" kopyasına (13.10 23:00, tekrarsız) bağlı; bu haftadan sonra durur.
  "RadioS 7/24 Yayın Akışı"na bağlansın mı?
- Testler için veritabanında "TEST · …" haftalık programları ve "TEST · …" playlistleri var;
  silinsin mi?
