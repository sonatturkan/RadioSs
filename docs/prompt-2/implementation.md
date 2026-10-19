# Prompt 2 uygulama ve kabul kaydı

Kapsam yalnız `prompt/2/2.pdf` ve `figures/sema_1.png`, uygulama `RadioS/` içindedir. Kaynakların tamamı incelenmiş ve [gereksinim matrisi](requirements.md) ile eşleştirilmiştir. Ortak görev/sahiplik geçmişi `../../../prompt/agent/AGENT.md`; UTC saatlik olaylar [coordination-log.md](coordination-log.md) ve `logs/` içinde tutulur.

## Uygulanan davranış

Şema 1'deki GERÇEK SIRADAKİ / GEÇİŞE KALAN barına **MEVCUT PROGRAM**, **ÇALAN MÜZİK**, **KALAN SÜRE** eklendi. Siyah zemin, gri başlıklar, cyan süre ve dikey ayraçlar mevcut tasarımın dilini sürdürür. Uzun Türkçe/Unicode adlar tek satırda ellipsis ile gösterilir; tam değer tooltip'te bulunur. Eski saat yanındaki sabit program/süre yer tutucu kopyası gizlenir.

Program adı kabul edilmiş ve sahipliği doğrulanmış yürütme planından alınır; seçili/düzenlenmiş taslak adı aktif programa taşınmaz. Global programlar, bağımsız oyuncu programları ve çevrimiçi akışlar salt okunur snapshot üzerinden izlenir. Özel program manuel/medya bekleme durumundayken parça/süre boşalır, kabul edilmiş program adı korunur.

Parça bilgisi gerçek MAIN rotası, mikser eşlemesi, çıkış hazırlığı/kapısı ve tutarlı medya generation/state gözlemi ile seçilir. CUE ön dinleme, READY, LOADING, EOF ve eski sahibi kaybetmiş plan bilgisi yayınmış gibi gösterilmez. Gerçek PLAYING kaynak PAUSED kaynaktan önce seçilir; birden fazla oynayan MAIN kaynakta açıklama eklenir. Yürütücünün duraklaması, decoder PLAYING state'i korusa da dikkate alınır; paused kaynak duyulan kaynak sayısına eklenmez.

Kalan dk/sn mutlak gerçek PCM konumu, gerçek cue-out ve tempo üzerinden hesaplanır. Manuel/retiring kaynaklar da gerçek cue-out ile sınırlandırılır. Bilinmeyen süre `—`, bitmiş süre sıfırdır; negatif süre uydurulmaz. PDF süreyi ayrıca tanımlamadığından bu **çalan parçanın kalan süresi** olarak uygulanmıştır; belirtilmeyen program toplam süresi eklenmemiştir.

**Preferences → View → Yayın çubuğu** altında üç bağımsız göster/gizle seçeneği vardır. Değişiklikler anında uygulanır ve atomik INI kaydıyla yeniden açılışta korunur. Son tercih dosyası her değişimde yüklenerek yazı/profil gibi diğer tercihler korunur. Başarısız kayıt önceki görünürlük/seçime döner; `show_all` gizli alanları yeniden açmaz. Eski sürüm1 INI dosyalarında yeni anahtar yoksa üç alan görünür olur.

## Kod noktaları

| Parça | Dosyalar |
| --- | --- |
| Üst bar, yaşam döngüsü, ayarlar ve stil | `src/ui/header_status.c`, `include/radios/header_status.h`, `assets/themes/header-status.css` |
| Kalıcı görünürlük maskesi | `src/core/ui_preferences.c`, `include/radios/ui_preferences.h` |
| Kabul edilmiş program adı ve salt okunur gözlem | `src/playout/planner.c`, `src/ui/playout_ui.c`, `include/radios/playout_ui.h` |
| Yerel/özel oyuncu/çevrimiçi kaynak seçimi | `src/ui/broadcast.c`, `src/ui/broadcast_online.c`, `include/radios/broadcast_header.h` |
| Mevcut pencere/Preferences bağlantısı | `src/ui/header.c`, `src/ui/preferences.c`, `src/ui/window.c` içindeki birlikte koordine edilmiş hook/merge noktaları |
| İzole derleme/test kayıtları | `cmake/Pdf2HeaderStatus.cmake`, ana CMake'de tek include |

Mevcut paylaşılan dosyalarda değişiklikler canlı içerikten minimal hunk ile, `source-edit.lock` ardından dosya kilidi altında yapılmıştır. Diğer prompt ekiplerinin header/navigation/flow/calendar/audio değişiklikleri korunmuştur.

## Nihai kabul — 2026-10-09 02:39 UTC

[Son gereksinim matrisi](requirements.md) **P2-01…P2-06 PASS** durumundadır. PDF'nin bir sayfası ve figures içindeki tek görsel bütünüyle incelenmiştir. Kaynakta belirtilen mevcut program, çalan müzik, kalan dakika bilgisi ve bunların ayarlara eklenmesi uygulanmıştır. Kaynak üç yeni alan için kesin piksel konumu veya final mockup vermediğinden görsel kabul mevcut siyah/gri/cyan bar, metinler ve ayraçlarla karşılaştırılmıştır.

| Son test | Gerçek vaka sayısı | Sonuç ve kanıt kapsamı |
| --- | ---: | --- |
| `ui_preferences` | 11 | PASS: 0…7 görünürlük maskesi, eski INI, geçersiz değerler, atomik kayıt/hata davranışı. |
| `playout` | 22 | PASS: kabul edilmiş program başlığının dış config mutasyonundan etkilenmemesi dahil. |
| `native_controls` | 10 | PASS: gerçek Preferences/View seçenekleri, kaydet/kapat/yeniden aç ve diğer tercihlerin korunması. |
| `native_header_status` | 8 | PASS: gerçek GTK widget/INI/CSS; ses ve program gözlemleri kontrollü stub. |
| `broadcast_header` | 23 | PASS: production aggregate selector; upstream motor/UI/plan snapshot'ları linker wrapper. MAIN/generation/ownership, pause önceliği, cue-out ve stale durumlar doğrulanır. |
| `header_online_status` | 4 | PASS: production online getter; salt okunur official gözlemleri wrapper. Yayımlanmış süre, finite/day sınırı, kart/fallback ve yanlış kimlik temizliği doğrulanır. |
| `native_header_private_programme` | 1 | PASS: gerçek izole DB/engine/Pulse MAIN PCM, kabul edilmiş özel program, cursor/cue-out, gerçek duraklama/sessizlik, diğer oyuncu önceliği ve stop temizliği. |
| `native_header_private_wait` | 1 | PASS: gerçek özel oynatma ve manuel geçiş beklemesi; program korunur, parça/süre temizlenir, stop sonlandırır. |
| Focused `/broadcast-online/flow-plays-youtube-events` | 1 | PASS: gerçek izole SDBASE HTTP/auth/DB ve production flow/header; dış provider/audio gözlemleri wrapper. |

Toplam **81 gerçek GLib vaka** geçti. İlk sekiz CTest hedefi **8/8 PASS, 18.96 sn**; focused online flow **1/1 PASS, 3.98 sn**. Atlama veya devre dışı test yoktur. Ham komutlar, exit kodları, JUnit, tam TAP ve binary/kaynak SHA-256 kayıtları [işlevsel kanıt klasöründedir](evidence/20261009T023257Z/manifest.json). JUnit stdout kısaltması nedeniyle vaka sayısı ayrıca [tam TAP üzerinden doğrulandı](evidence/20261009T023257Z/coordinator-tap-verification.json).

### Görsel ve gerçek pencere kabulü

[Son görsel manifest](screenshots/final-20261009T023434Z/manifest.json) sekiz gerçek GTK görüntüsü içerir: **1000×680, 1400×900, 1800×1000** normal ve büyük yazıda altı bağımsız üretim header görüntüsü, **1800×1000** tam ana pencere ve **1030×1020** Preferences/View. Root ve üç ajan bunları bağımsız incelemiştir. Caption/değerler görünür; eski NEXT caption/countdown tek satır, yeni caption'lar yalnız kelime arasından kırılır; uzun Türkçe/Unicode değerler ellipsis ile sınırda kalır. [Tam pencere](screenshots/final-20261009T023434Z/main-normal.png) ve [ayar ekranı](screenshots/final-20261009T023434Z/preferences-view.png) kaydedilmiştir.

Tam pencerenin panel açık/kapalı minimum genişliği normal yazıda **1568/1568 px**, büyük yazıda **1574/1574 px**: yeni alanlar minimum genişliği artırmaz. Header-only 1000/1400 px başarısı tam uygulamanın bu genişliklere sığdığı anlamına gelmez; diğer sayfaların mevcut minimumu devam eder. Görsel driver yalnız yayın gözlemini açık sentetik uzun metin fixture'ı ile sağlar; gerçek üretim GTK/theme/font/grid/lifetime kullanılır. Gerçek yayın/PCM davranışı yukarıdaki iki özel oyuncu testinde ayrıca doğrulanmıştır.

Son sekiz capture sırasında yedi görsel kaynak SHA'sı değişmemiştir (`source_drift=[]`). Önceki altı capture, ilk dar pencere ve caption/minimum regresyonları tarihsel kanıt olarak korunur; eski görüntüler son kaynağın kanıtı sayılmaz.

### Strict Release ve kurulu uygulama

Root'un ayrı `build/prompt-2-release` dizininde **02:38:47–02:39:03 UTC** yaptığı fresh Release configure/compile/link **EXIT 0** oldu. Üretim header modülünün gerçek compile command'ında `-O3 -DNDEBUG -Wall -Wextra -Wpedantic -Werror` doğrulandı; uyarılar gevşetilmedi. Aynı binary gerçek mapped window, **8 profil** ve iş/durum teslimi smoke testinden **EXIT 0, 2.823 sn** ile geçti. [Release manifest](evidence/release-20261009T023847Z/manifest.json), ham loglar ve [1920×1042 gerçek pencere görüntüsü](evidence/release-20261009T023847Z/release-native-window.png) arşivlendi. Bu koşumda izlenen **370 kaynak before/after eşittir**, drift yoktur; `config/ui-preferences.ini` başlangıçta ve sonunda yoktur, smoke üretim tercih dosyası oluşturmadı.

İzole Release binary SHA-256: `116fc0aa16a5ce4d46cc606591188466bf6099b275cd0bcb07ef4b2dd20ab4a9`.

Varsayılan `build/radios` dosyasının tek sahibi **PROMPT5/root** uygulamayı **02:35:19 UTC** atomik kurmuştur. [Bağımsız inclusion kanıtı](evidence/default-prompt5-p2-inclusion-verification.json), kurulu binary'nin `b1578bb7c6c97e8667848ade86c4aa8cb615b067d4ea62a264077bff8013726e` SHA'sını owner receipt/candidate ile eşleştirir; son işlevsel kabuldeki **16 P2 production kaynak** bu tarihli Release manifest'inde birebir bulunur. Owner'ın kurulu launcher üzerinden native 8 profil smoke'u da PASS'tir. PROMPT2 varsayılan binary'yi yazmamıştır.

Tarihli işlevsel/P5 Release snapshot'ından sonra paylaşılan `header.c` kaynağı `651bba34…` → `2f869609…` değişmiştir; bu yeni kaynak 02:34:34 görsel koşumunda zaten mevcuttur. Değişimin yazarı ve kesin zamanı bu kanıtlardan çıkarılmaz. Son görsel ve bizim izole Release bu yeni kaynağı içerir; varsayılan binary'nin tarihli 16 kaynak eşleşmesi son görseldeki yedi kaynağın tamamıyla eşleşme diye sunulmaz. Kalan 15 P2 production kaynak son işlevsel ve izole Release snapshot'larında aynıdır. Başka ekiplerin sonraki kaynak değişiklikleri tarihli binary'ye kendiliğinden kurulmuş sayılmaz.

### Hata geçmişi ve kabul sınırları

İşlevsel kabulün **02:32:57–02:33:24** kaynak kontrolünde canonical kilit tutulmasına rağmen dış `src/ui/playlist.c` değişti: 370 kaynaktan 369 sabit, bir drift. Bütün gerçek build/test komutları EXIT 0 iken kontrol script'i yalnız bu drift nedeniyle EXIT 1 verdi. P2 kaynakları sabitti; o test snapshot'ı için bütün-ağaç zero-drift iddiası yoktur. Son fresh Release'in ayrı zero-drift kanıtı geçmiş kontrol hatasını silmez ve hangi playlist sürümünün eski testte derlendiğini geriye dönük belirlemez.

İlk özel PCM testinde decoder'ın PAUSED olacağı varsayımı gerçek runner sözleşmesine uymadı; gerçek runner duraklaması, cursor donması ve PCM sessizliğiyle fixture düzeltildi, production aggregate logical pause'a uyarlandı. İki özel testi aynı quota fixture'ında arka arkaya çalıştırma denemesi ikinci kurulumda DATA429 verdi; her vaka ayrı yeni DB/audio fixture'ına taşındı, production quota veya assertion gevşetilmedi. Tarihsel başarısızlıklar UTC loglarda korunur; quota denemesinin eski LastTest çıktısı arşivlenmeden üzerine yazıldığı açık kaydedilmiştir.

Diğer ekiplerin geçici eksik prototype/link/provider ve strict-warning ara durumları sahiplerine iletildi; son build tamamlanana kadar başarılı gösterilmedi. İlk normal/büyük font minimum artışları daraltılmış padding ve yeni caption whole-word wrapping ile giderildi; ölçümle 0 px doğrulandı.

Bu kabul **yalnız PDF 2** kapsamındadır. Repository'nin bütün testleri veya diğer PDF ekiplerinin bütün açık işleri tamamlandı denmez. Harici online sağlayıcı hesabı/gerçek provider PCM'si denenmemiştir; ilgili gözlemler kontrollü wrapper'dır. Özel iki ses testinde gerçek DB/engine/Pulse PCM kullanılır. Kaynakta ayrıntılandırılmayan süre türü ve üç görünürlük seçeneği kaydedilmiş uygulama kararlarıdır.

## Kapanış ve süreklilik

PDF 2 için kaynak analizi, uygulama, anlamlı işlevsel testler, bağımsız görsel karşılaştırma, strict Release ve native smoke tamamdır; açık P2 kod düzeltmesi veya atlanan kaynak gereksinimi yoktur. Dört eşzamanlı ajan (root/requirements/architecture/coordinator) araç kapasitesinin tamamını kullandı. Kullanıcının istediği 14 eşzamanlı ajan bu oturumun 4 slot sınırı nedeniyle sağlanamadı; daha yüksek kapasite varmış gibi raporlanmadı.

[Durum dosyası](status.json), [mimari denetim](architecture-audit.md), [saatlik kayıtlar](coordination-log.md) ve ortak `prompt/agent/AGENT.md` devam işlemleri için kalıcı kaynaklardır. [Nihai feature manifest](evidence/final-feature-manifest.json) kaynak/binary/kanıt hash'lerini ilişkilendirir. Sonraki değişiklikler sahiplik ve canlı içerik kilit protokolüyle yürütülmeli, bu tarihli kanıtların kapsadığı kaynaklardan farklıysa etkisine uygun doğrulanmalıdır.
