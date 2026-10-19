# Prompt 7 — Kaynak incelemesi ve canlı oynatıcı kabul kaydı

İnceleyen ajan: `/root/pdf_audit`. İlk doğrulama: 2026-10-09 01:55:07 UTC.
Kesinti sonrası kayıt sahibi: `/root/requirements_resume`. Son kapsam kabulü: 2026-10-09 02:44:54 UTC.
Uygulama hedefi yalnız RadioS; gereksinim kaynağı yalnız `prompt/7/`.
Koordinasyon: `prompt/agent/AGENT.md` içindeki `SESSION-PROMPT-7` kayıtları.

## Eksiksiz kaynak envanteri

| Kaynak | Boyut / kapsam | İnceleme kanıtı |
| --- | --- | --- |
| `prompt/7/7.pdf` | 28.782 bayt; 1 sayfa; A4, 595,2756 × 841,8898 pt | pypdf ile bütün sayfa metni ve kaynak nesneleri; PyMuPDF ile bütün sayfa renderi; render `view_image` ile incelendi |
| `prompt/7/figures/` | Dizin mevcut, boş | Recursive envanterde hiçbir görsel veya başka dosya bulunmadı |

PDF SHA-256: `1f248b7178a16590b16df4b8e6776f786c143f8b58fd26c49e38b67db734f8fa`.
Kaynak PDF değiştirilmedi. İnceleme çıktıları geçici
`/tmp/radios-prompt7-audit/` dizinindedir: `text.txt`, `page-1.png`.
Geçici render araçları aynı dizinin `python/` altına kuruldu; proje bağımlılığı
olarak eklenmedi.

Sayfa 1'de görünen içeriğin tamamı iki satırdır:

> Yayın kısmında eksik kalan diğer yerler.
>
> Canlı oynatıcılar.

İlk satır kalın bir başlık, ikinci satır normal metindir. Sayfanın kalan
bölümü boştur. PDF kaynaklarında yalnız Font ve ProcSet bulunur; gömülü
görsel sayısı **0**, çizim nesnesi sayısı **0**. Metin çıkarımı ile görsel
render aynı iki satırı göstermektedir. Bu kaynakta arayüz şeması, ölçü,
renk, örnek ekran, düğme listesi veya teknik algoritma bulunmamaktadır.

## Gereksinimler ve açık belirsizlikler

| Kimlik | PDF referansı ve açık gereksinim | Kabul / mevcut uygulama değerlendirmesi | Belirsizlik |
| --- | --- | --- | --- |
| P7-R01 | `7.pdf`, s.1, başlık: yayın kısmındaki eksik kalan yerleri tamamlamak | Yayın içindeki canlı oynatıcı akışları kaynak koduyla karşılaştırılır; bulunan somut hatalar düzeltilip uygun testlerle kanıtlanır. Bulunmayan ayrıntılar tamamlanmış gibi gösterilmez. | Hangi bütün yayın eksiklerinin kastedildiği sayfada listelenmemiştir. Diğer prompt klasörleri bu oturumun yetkili kaynağı değildir. |
| P7-R02 | `7.pdf`, s.1, ikinci satır: canlı oynatıcılar | RadioS'un mevcut Canlı oynatıcılar bölümü ve gerçek yükleme/oynatma iş akışları temel alınır. Bu oturum kaynakta saptanan liste seçimi ve oynatıcı sıra/asenkron iş hatalarını gidermektedir. | Oynatıcı sayısı, görünümü, özel denetimler ve davranış ayrıntıları PDF'de tanımlanmamıştır. Aşağıdaki mevcut özellikler PDF'nin ilave talimatları olarak sunulmaz. |

Bu iki ifade, incelemesi tamamlanan kaynağın bütün gereksinim içeriğidir.
Belirli bir görsel tasarıma birebir uyum iddiası doğrulanamaz; karşılaştırılacak
PDF tasarımı veya figures görseli yoktur. Kullanıcının kaynağın dışında
verebileceği ayrıntılar ayrı bir sonraki gereksinim olarak kaydedilmelidir.

## Mevcut RadioS uygulamasıyla eşleştirme

Kaynaklar salt okunur incelendi; uygulama sahipliği alınan dosyalar aşağıda
ayrıca belirtilmiştir. Mevcut özellikler yalnız kaynak kodundaki durumu
anlatır; her birinin bu oturumda uçtan uca test edildiği anlamına gelmez.

| Alan | Mevcut kaynak / davranış |
| --- | --- |
| Yayın sekmesi ve oynatıcı kartları | `src/ui/broadcast.c`, `players_view`, `player_add`: Canlı oynatıcılar sekmesi, başlangıç kartları, kart ekleme/kaldırma, en fazla 32 deck; Oynat/Duraklat/Durdur/Sonraki, başlık/sanatçı, sıradaki parça, geçen/toplam süre, seviye ve CUE |
| Gerçek girdiler ve sıra | Aynı dosyanın `PlayerAudio` ve `player_*` bölümleri: doğrulanmış medya bağlantıları, sağlayıcı girdileri, drag/drop, sıraya ekleme/yeniden yükleme, sıra temizleme ve sıradaki girdiye geçiş |
| Playlist yükleme | `src/ui/player_loader.c`, `include/radios/player_loader.h`: Kütüphane, Sağlayıcılar, Otomasyon ve Program sekmeleri; liste/parça önizlemesi; Yükle ve Sona ekle |
| Girdi dönüşümü | `radios_player_input_new`: gerçek medya kimlik/SHA bağlantıları, YouTube Music ID'si veya Spotify/Apple Music kaydı; kimlik/başlık taşımayan oynatılamaz girdilerin atlanması |
| Ses ve sağlayıcı uygulaması | `src/ui/audio_ui.c`, `src/ui/playout_ui.c`, `src/ui/broadcast_online.c`, `src/audio/`: yerel deck kontrolü, gerçek çözümleme/yürütme ve çevrimiçi sağlayıcı yaşam döngüsü |
| Yerel medya belgeleri | `docs/07-medya-kutuphanesi.md`, `docs/08-ses-motoru.md`: mevcut mimari/uygulama bağlamı. Bunlar `7.pdf` yerine ilave PDF gereksinim kaynağı olarak kullanılmadı. |

Projenin `design/reference/tasarrım/index.html`, `live-player-gui.js` ve
`live-player-gui.css` referansları mevcut kart/girdi/program-playlist
seçimi bağlamı için karşılaştırıldı. HTML/JS'de kart ekleme/kaldırma, sıra,
transport, süre, seviye ve CUE yapıları vardır. Native arayüz gerçek medya ve
sağlayıcı uygulamasıyla bu kavramları zaten kullanır. Bu referanslar PDF'nin
tanımlamadığı yeni ölçü veya birebir çizim emri olarak yorumlanmadı.

## Bu ajan tarafından düzeltilen somut yükleyici hataları

Dosya sahibi: `/root/pdf_audit`; kaynak: `src/ui/player_loader.c`.
Yeni kabul testleri: `tests/test_player_loader_ui.c`.
Bu kusurlar PDF'de ayrı davranış maddeleri olarak yazılmamıştır; mevcut canlı
oynatıcının çalışmasını engelleyen, yeniden üretilebilir uygulama eksikleridir.

| Kimlik | Önceki hata ve tetikleyici | Düzeltme / kabul kanıtı |
| --- | --- | --- |
| P7-L01 | Yükleyicide liste seç, başka sekmeye geç, geri dön: seçili satır ve parça önizlemesi kalırken Yükle/Sona ekle pasif olur. | Sekmeye dönülünce seçili kaynağın girdileri yeniden hazırlanır. `tab-restore` seçimi, yeniden etkin yüklemeyi ve doğru listeyle sona ekleme sonucunu sınar. |
| P7-L02 | Kaydedilmiş sağlayıcı listesi seçiliyken hesap listelerinin geç yanıtı gelir: model yeniden kurulurken seçim/girdiler kaybolur. | Liste modeli yeniden kurulduğunda kimliğe göre seçim korunur; yeniden çizim sinyalleri aktif girdileri temizlemez. `provider-refresh-keeps-selection` doğru seçimi ve yükleme sonucunu sınar. |
| P7-L03 | Aktif sağlayıcı listesi için parçalar beklenirken gizli Kütüphane seçimi değişir: başka sekmenin sinyali aktif sekme olarak işlenir, aynı parça isteği tekrar başlatılır ve önceki yanıtın generation'ı bozulur. | Selection sinyalinin gerçek kaynak sayfası belirlenir; gizli sayfa ve model yeniden çizim sinyalleri aktif yüklemeye karışmaz. `hidden-selection-isolation` tek istek ve doğru hesap parçası yüklemesini sınar. |
| P7-L04 | Sekme değiştikten sonra önceki sağlayıcı parça yanıtı gelir. | Mevcut generation koruması korunur. `late-tracks-current-tab` geç yanıtın güncel Kütüphane seçimini ezmediğini sınar. |
| P7-L05 | Yükleyici sahibi kapanırken liste yanıtları hâlâ bekler. | Mevcut weak-owner yaşam döngüsü korunur. `destroyed-owner` kapanmış diyaloga/sahibe sonuç yüklenmediğini sınar. |

`sources_render` yeniden çizim koruması ISO C11 uyumlu sayfa bayrağı kullanır.
Sekmeye dönüşte hesap/otomasyon girdisi yeniden hazırlanırsa yeni generation
alır; eski callback aktif seçime uygulanamaz. Arayüzde yeni PDF varsayımı,
yeni medya kaydı veya sağlayıcı bağlantısı eklenmedi.

## Doğrulama ve kalan işler

2026-10-09 **01:55:07 UTC** doğrulaması:

- Önceki kaynak `/tmp/radios-prompt7-audit/player_loader.before.c` olarak
  yedeklendi; SHA-256:
  `42ea0d74991cb345631d2643d5ae95481d226efa5eab425a7381bb12c041f296`.
- Önceki ve düzeltilmiş kaynaklar aynı yeni GTK test dosyasıyla bağımsız
  derlendi. Strict C11 `-Wall -Wextra -Wpedantic -Werror` derleme ve link geçti.
- Önceki kaynakta P7-L01/L02/L03 testleri ayrı koşularda **3/3 başarısız** oldu;
  özellikle gizli seçim aynı hesabın parça isteğini 1 yerine 2 kez başlattı.
- Düzeltilmiş kaynakta **5/5 test geçti**; `G_DEBUG=fatal-warnings`, X11 ve
  gerçek `DISPLAY=:0` kullanıldı. GTK diyalogu, modeller, seçim sinyalleri,
  girdi dönüşümü ve response işleyişi gerçekti. Yalnız asenkron veri/sağlayıcı
  yanıt sınırları kontrollü test cevaplarıyla değiştirildi.
- Kalıcı test kaydı [loader-before-after.json](test-evidence/loader-before-after.json)
  içindedir. Aynı dizindeki `loader-before-after.before-*.txt` üç önceki
  başarısızlık transcriptini; `loader-before-after.after.txt` 5/5 başarı
  transcriptini korur. Gerçek ilk test zamanı **01:55:07 UTC**, kanıtları
  kalıcılaştırma zamanı **02:01:58 UTC** olarak ayrı kaydedilmiştir.
- `backups/` altında loader önceki/sonraki kaynakları ve kabul testinin
  kaynak snapshot'ı vardır; JSON dosyası SHA-256 değerlerini, gerçek test
  komutlarını, derleme/link reçetesini ve eski object/binary hash'lerini
  içerir. İlk test sırasında bağımlı arşivlerin hash'leri kaydedilmemiştir;
  bu kanıt yalnız loader davranışını doğrular. Birleşik proje derlemesi
  aşağıdaki son kapsam kabulünde root tarafından ayrıca doğrulanmıştır.

İlk bağımsız derlemede GLib'in function-pointer blocker makrosu pedantic
uyumsuzluğu ve testte misleading-indentation uyarısı bulundu; son kaynakta
ikisi de düzeltilerek strict derleme tamamlandı.

## Canlı kart transport düzeltmelerinin kaynak–kusur–test eşlemesi

Uygulayan ajan `/root`; test kaynağı sahibi `/root/media_audit`.
`src/ui/broadcast.c` içindeki PlayerAudio/transport hunks ve yeni
`include/radios/player_transport.h` kök ajanın alanıdır. Diğer ekiplerin
waveform, kare kart görünümü, otomatik analiz, SYNC/AutoDJ veya katalog
değişiklikleri prompt7 uygulama sonucu olarak sayılmaz.

Aşağıdaki P7-T kimlikleri mevcut kaynak kodda bulunan kusurları izler;
PDF'de yazılı ilave teknik şartlar değildir. Son yalnız-kendi-değişiklikli
translation unit strict C11/Werror ile derlendi. **2026-10-09
02:07:37.885–02:07:50.855 UTC** aralığında `/root/media_audit` gerçek Broadcast
GTK callback'lerini kullanan son 18-case suite'i çalıştırdı: **18/18 PASS**,
exit code 0, `G_DEBUG=fatal-warnings`, `DISPLAY=:0`.
[Kalıcı v18 kanıtı](test-evidence/live-players-scoped-v18.json) komut,
stdout/stderr ve kaynak/object/archive/binary SHA-256 değerlerini içerir;
kanıt **02:08:25 UTC** tarihinde yazıldı ve bu ajan tarafından okundu.
Bu başarı kontrollü asenkron sınırlarla çalışan scoped transport testidir;
paylaşılan güncel projenin bütün derlemesi veya CTest kabulü değildir.

Test isimleri `tests/test_live_players_ui.c` içindeki `/live-players/`
prefix'inden sonraki adlardır:

| Kimlik | Yeniden üretilebilir kusur / kaynak düzeltmesi | Test eşlemesi |
| --- | --- | --- |
| P7-T01 | Eski `library_get` yanıtı değiştirilmiş/silinmiş/durdurulmuş sırayı geri yükleyebilir. Monotonic `load_serial` ve merkezi `player_input_invalidate` Stop, Next, replace, clear ve kapanışta eski niyeti geçersiz kılar. | `replacement-late-reply`, `stop-invalidates-lookup`, `stopped-next-invalidates-old-lookup`, `clear-after-stop-retires-lookup` |
| P7-T02 | Next veya current kaldırma sonrası eski yüklü PCM aynı medya UUID'sini taşısa bile başka sıra konumuna aittir. `loaded_current` sıra konumunu kimlikle birlikte bağlar; yer değişimi doğru yeni girdiyi hazırlatır. | `stopped-next-loads-current`, `current-removal-loads-current`, `repeated-media-next-requires-fresh-lookup` |
| P7-T03 | Başarısız lookup sonrası aynı current girdiye yeniden Play yapılması yeni bir niyetle denenebilmelidir. Lookup pending/running yaşam döngüsü doğru temizlenir. | `failed-lookup-retries-current` |
| P7-T04 | Dönen medya/version seçilen current ile eşleşmiyorsa eski veya farklı kayıt çalınabilir. `player_media_ready` callback'i current medya/version kimliğini denetler ve uyumsuz yanıtı reddeder. | `lookup-version-mismatch-rejected`, `lookup-media-mismatch-rejected` |
| P7-T05 | Yeni lookup beklenirken önceki PCM'nin EOF/error snapshot'ı sırayı tekrar ileri götürebilir. `lookup_pending` ve yüklenmiş-current kimliği bitiş olayını doğru niyete bağlar. | `pending-lookup-keeps-current-on-old-EOF` |
| P7-T06 | Bağımsız kütüphane yüklemesinden sonra eski queue metadata/Play veya bekleyen callback yeni medyayı ezebilir. `radios_broadcast_player_library_loaded` yeni PCM'yi korurken lookup/match niyetini ve eski queue kontrolünü bırakır. | `direct-library-retires-queue-lookup`, `external-library-load-preserves-direct-selection`, `external-library-load-retires-pending-lookup` |
| P7-T07 | Yeni Spotify/Apple Music eşleşmesi veya online başlangıç beklenirken önceki FAILED/FINISHED snapshot'ı sırayı erken ilerletebilir. Online advance koşulları `!starting` ile korunur. | `pending-online-match-ignores-old-failure`, `pending-online-match-ignores-old-finish` |
| P7-T08 | Managed playout deck'i devraldıktan sonra ilk timer tick'inden önce gelen yerel lookup/provider-match callback'i eski girdiyi yükleyebilir. Callback girişindeki managed ownership kontrolü niyeti geçersiz kılar; managed metadata yolu queue kontrolünü bırakır. | `managed-takeover-retires-local-lookup`, `managed-takeover-retires-provider-match` |
| P7-T09 | Özel oynatıcı ve genel playout havuzu ile manuel MAIN devralma denetimi mixer eşlemesini yalnız fiziksel stereo kanal sayısı (2) boyunca tarıyordu; strip 2..31 eşlemeleri görünmezdi. `choose_pool` iki dalı ve `replacement_ready` içindeki yalnız üç döngü sınırı gerçek `RADIOS_AUDIO_MIXER_CHANNELS` (32) oldu; MAIN/sahiplik korumaları korunur. | Yeni `tests/test_player_mapping_ui.c`: `private-last-strip-PCM-pause-resume-stop`, `general-last-strip-PCM`, `handover-manual-last-strip-preserved`. Gerçek PCM **3/3 PASS**; aynı test nesnesiyle yalnız eski üç sınır geri yansıtıldığında **3 beklenen kusur FAIL**. |

P7-T06'nın gerçek library caller entegrasyonu **02:04:00 UTC** salt okunur
kaynak kontrolünde teyit edildi: `src/ui/media.c` yeni header'ı içerir ve
`deck_picked` başarılı `radios_audio_ui_load_media` sonrasında helper'ı çağırır;
`radios_media_library_open_for_deck` bu picker callback'ine yönlenir.
Bu kaynak kontrolü, backend/hardware ile uçtan uca çalışma testi değildir.

İkinci caller `src/ui/dj.c` içindeki normal deck'i kullanan
`channel_media_picked` akışıdır. **02:04:35 UTC** kontrolünde helper henüz
yoktu. Scope6 `/root/pdf_ui` ajanı **02:14:22 UTC** tarihinde header include
ve başarılı `!ui->master` dalının ilk helper çağrısını uyguladı; Scope6
**02:16:00 UTC** READY kaydı ile sahiplik ve SHA-256
`ed2f057567351a538dbec7c784b3842bee657f176a54c592691bb50d28f7a432`
teyit edildi. Kesinti sonrası salt okunur kontrolde include satır 6, çağrı
satır 1429 olarak bulundu. Başarısız yükleme ve reserved sampler/master
dalı helper çağırmaz. Root7 bu dosyaya mutasyon yapmadı; gereksiz aynı-hunk
yazımı no-op olarak kapandı. Kaynak caller entegrasyonu tamamdır; birleşik
runtime kabulü aşağıdaki son doğrulama kaydına bağlıdır.

P7-T09 kaynak düzeltmesi **2026-10-09 02:25:23.930330 UTC** tarihinde
root tarafından `source-edit.lock` ve `playout_ui.c.lock` altında yalnız üç
sınır token'ına uygulandı. Önceki SHA-256
`f162abe50ccabaa30949e1c9d2aa050f0cf6b4c596bba0de0b44a611be9a5b95`,
sonraki SHA-256
`1064c2bd906e53871b8cc422f543845cec3b579df44761e8591da4ef46a55f28`.
[Kaynak işlem kanıtı](test-evidence/player-mapping-patch-20261009T022523Z.json)
ve `backups/playout_ui.c.f162abe50ccabaa3.before` korunur. AutoDJ
eşlemesinin zaten doğru 32 sınırı ve diğer bütün dosya hunks'ları aynıdır.
Dar writer claim işlem bitince bırakıldı. Yeni testler deck 31
üzerinden gerçek özel playlist PCM/Pause/Resume/Stop, genel havuz PCM ve
manually PLAYING MAIN sesinin devralmada korunmasını sınar. Bu temsilci
yüksek deck sınırı testidir; bütün 32 kartın ayrı ayrı uçtan uca denendiği
iddia edilmez. Yeni kaynak/test strict syntax kanıtından sonra gerçek PCM üç vaka
**02:40:08.754711–02:40:25.289982 UTC** aralığında **3/3 PASS** oldu.
Son kabul, aynı üretim arşivleri üzerindeki **31 + 3 = 34** vaka için
iki gerçek doğrulama aşamasıdır; dört CTest hedefinin tek seferde
başarılı olduğu şeklinde sunulmaz.

Önceki 11-case yalnız root transport translation unit'i karşılaştırması
`test-evidence/live-players-isolated-before-after.json` içinde **9 beklenen
regression başarısızlığı + 2 kontrol başarısı / 11 sonraki başarı** olarak
kayıtlıdır. Son callback/helper/slot düzeltmeleri ayrıca yukarıdaki v18
koşusunda doğrulandı; önceki 11-case arşiv ve transcriptler korunmuştur.
Kütüphane/ses/sağlayıcı/managed metadata sınırları kontrollü yanıtlarla
sınanır; bu testler PCM, cihaz, backend kalıcılığı veya gerçek sağlayıcı
oturumunun uçtan uca kabulü yerine sunulmaz.

v18 kanıtı hazırlanırken paylaşılan current-source build'i dış
engine/Pulse `radios_pulse_backend_configure_output` link değişikliği
nedeniyle tamamlanmamıştı. Diğer translation unit'ler mevcut bağımlı
arşivlerden kullanılmıştır; bütün güncel kaynak sürümlerini temsil ettikleri
iddia edilmez. Bu tarihsel hata loader veya scoped 18-case kabulünü başarısız yapmaz.
Son ayrı kaynak kopyası ve aynı üretim arşivleri üzerindeki gerçek
doğrulama aşağıdaki kapsam kabulünde tamamlanmıştır.

Diğer ekiplerin uygulama/doğrulama sonuçları kendi sahiplik/kayıtlarında
izlenir. Bu ekipte saptanan P7-L01..L05 ve P7-T01..T09 kusurları aşağıdaki
kapsamlı kaynak ve test kanıtıyla kapanmıştır.
Genel yayın eksiklerinin tam listesi ve ayrıntılı yeni oynatıcı tasarımı
PDF'de bulunmadığından kaynakta belirtilmeyen beklentilerin tamamlandığı
iddia edilmez.

Kesinti sonrası iki gerçek mutable-source koşumunda 18 canlı kart + 5
yükleyici + 8 gerçek PCM = **31 vaka, 3/3 CTest hedefi PASS**, sıfır SKIP
çıktı. `final-fenced-validation-20261009T022007Z.json` ve
`final-fenced-validation-20261009T022247Z.json` kaynak SHA drift'ini açıkça
saptadığı için `accepted=false` olarak korunur; bu runtime başarılar
yeni mutable kaynak sürümüne aktarılmaz. Kesilmiş önceki
`final-fenced-validation-20261009T021531Z-build.log` yalnız başarılı
derlemeyi kanıtlar, final test kabulü içermez. Root üretim yapılandırması/kimlik bilgileri/veritabanı/ithal medya
taşımayan public kaynak kopyasını capture öncesi-kopya-sonrası SHA
eşitliği ve ayrı fresh strict build ile doğrulamıştır. İlk yeni mapping
testi fixture şeması yüzünden başarısız olduğu için o dört CTest
koşumunun `accepted=false` kaydı korunur. Düzeltilmiş üç vaka aynı üretim
arşivleriyle ayrıca geçmiştir; son kabul aşağıdaki iki aşamalı kanıttır.


## Son kapsam kabulü — 34 gerçek vaka, iki doğrulama aşaması

Root kabulü **2026-10-09 02:43:30.392567 UTC** tarihinde
[final-acceptance.json](test-evidence/final-acceptance.json) içinde
`accepted=true`, **34 benzersiz vaka**, **0 SKIP**, **2 doğrulama aşaması**
olarak kaydedildi. Bu dosya ve referans verdiği üç kanıtın SHA-256
bağlantıları bu belge güncellenirken bağımsız doğrulandı.

| Aşama | Gerçek sonuç ve kaynak sınırı |
| --- | --- |
| Kaynak kopyası, fresh uygulama derlemesi ve ilk 31 vaka | **02:33:03 UTC** capture: 421 public dosya, ön/kopya/son SHA birebir aynı. Strict Debug ISO C11/Wall/Wextra/Wpedantic/Werror configure ve tam uygulama + dört test executable derlemesi **exit 0**. **02:33:15.823048–02:34:20.768532 UTC** ilk CTest kaydında `native_audio` 8, `native_live_players` 18, `native_player_loader` 5 vaka geçti. Kaynak kopyası ve altı binary/arşiv SHA değişmedi. Aynı ilk koşumdaki yeni mapping hedefi fixture kurulurken başarısız olduğu için bütün dört hedefin kabulü **false / exit 8** olarak korunur. [İlk kaynak-kopyası kanıtı](test-evidence/snapshot-validation-20261009T023303147616Z.json). |
| Düzeltilmiş üç yüksek deck PCM vakası | **02:40:08.754711–02:40:25.289982 UTC**, **16,535 saniye**, gerçek private backend/DB/null audio çıkışlarında **3/3 PASS, exit 0, sıfır SKIP**. İlk kaynaktaki dört üretim arşivi byte-eşit kullanıldı; yalnız yeni testin zorunlu programme/event şeması düzeltildi ve her vaka için ayrı fixture açan yeni adapter eklendi. 422 dosyalı düzeltilmiş test kaynağı, derlenmiş object ve binary SHA ön/son eşittir. [Düzeltilmiş üç vaka kanıtı](test-evidence/player-mapping-corrected-after.json). |
| P7-T09 nedensel önce/sonra kontrolü | **02:41:04.319388–02:41:12.728207 UTC**, aynı düzeltilmiş test nesnesi ile aynı üretim kaynak kopyasında yalnız üç doğru mixer-32 sınırı eski stereo-2 sınırına yansıtıldı. Private/general havuzun yanlış reddi ve manuel MAIN devralmanın yanlış kabulü **üç özel hata imzasıyla FAIL** oldu. Arşivlerde aynı sıradaki 42 üyeden yalnız `playout_ui.c.o` değişti; mevcut doğru AutoDJ-32 döngüsü ve başka tüm üretim kaynakları korundu. Üç düzeltilmiş vaka yukarıdaki sonraki aşamada geçti. [Nedensel önce/sonra kanıtı](test-evidence/player-mapping-isolated-before-corrected.json). |

İlk yeni fixture programme `note` alanını ve olayın zorunlu string
alanlarını eksik gönderdiğinden mapping/PCM yürütümüne ulaşmamıştı.
Önceki özel arşivdeki aynı kurulum hataları da gerçek mapping regression
olarak sayılmaz. `player-mapping-isolated-before.json` bu nedenle
`finished_setup_failed` ve **0 doğrulanmış kusur** olarak düzeltildi;
önceki sınıflandırma, stdout ve zamanlar ayrı dosyada korunur. Yeni
nedensel kanıt aynı düzeltilmiş fixture ile özel pool/manuel MAIN
hatalarına ulaştığını doğrular.

Yeni son test kaynağı SHA-256:
`dcf8641951446bf54a3b8291e75523247c7d581b7e9ffd33a625f05aeb6f0192`.
Yeni adapter `tests/run_player_mapping_ui.py` SHA-256:
`6d326401e290e3361e04a79d84439c5ecc5ac851a6c5425a5fc4b201ed2aecf1`.
Mevcut `tests/run_audio_ui.py` başka ekibin kaynağı olarak değiştirilmedi.
Root kendi `cmake/Prompt7Tests.cmake` modülüne bu adapter'ı kaydetti;
son generated CMake test komutu ile ISO C11 ve Werror bayrakları ayrıca
configure edilip doğrulandı. Varsayılan `build/radios` yazma sahibi
PROMPT5/root'tur; bu ekip varsayılan binary'yi değiştirmedi.

Bu kabul **23 gerçek GTK vaka** (asenkron veri/sağlayıcı sınırları
kontrollü) ve **11 gerçek PCM vaka** (8 mevcut + 3 yeni yüksek deck)
için kaydedilen kaynak/üretim arşivlerini doğrular. Sonraki paralel
shared-tree değişikliklerinin tamamına, gerçek çevrimiçi sağlayıcı
hesaplarına, üretim veritabanına veya fiziksel ses donanımına kabul
aktarılmaz. PDF'de ayrıntılı yayın eksikleri veya çizim bulunmadığı için
bunların varsayımsal biçimde tamamlandığı iddia edilmez. Bu oturumda
belirlenen somut canlı oynatıcı, loader ve mixer eşleme işleri
**uygulanmış ve uygun testlerle doğrulanmıştır**.
