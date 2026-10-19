# PDF4 gereksinimleri ve uygulama izlenebilirliği

Kayıt sahibi: `pdf4-requirements` (`/root/requirements`). Tarih: 2026-10-09 UTC.
Bu kayıt yalnız `../prompt/4/` kaynaklarını kapsar. Başka prompt oturumlarının
gereksinimlerini veya dosya sahipliğini değiştirmez.

## Tam kaynak envanteri ve inceleme kanıtı

Recursive dosya envanteri tam olarak iki dosya içerir:

| Kaynak | İçerik | İnceleme |
| --- | --- | --- |
| `../prompt/4/4.pdf` | Bir A4 sayfa; başlık, Şema 1, tek gömülü 5856×638 JPEG, iki paragraf | pypdf ile bütün metin/MediaBox/XObject incelendi; pypdfium2 ile sayfa 2.5 ölçekte render edildi ve görüntülendi |
| `../prompt/4/figures/sema_1.png` | 5856×638 header şeması | Tam görüntü ve sol/orta/sağ özgün çözünürlüklü crop'lar ayrıntılı görüntülendi; PDF içindeki görselle karşılaştırıldı |

Sayfa ölçüsü 595.2756×841.8898 pt'dir. Başka PDF sayfası, PDF veya figure yoktur.
Metin ve görseller kullanıcı kaynaklarıdır; değiştirilmedi. Render/extraction kanıtları:
`../prompt/agent/analysis/requirements/4-page-1.txt`, `4-page-1.png`,
`embedded-0-FormXob.134b3178fd90e67ef46f2677733b8996.jpg`,
`figure-left.png`, `figure-middle.png`, `figure-right.png`.

## Sayfa 1 — eksiksiz kaynak metni

Başlık: **Neon bar yerleşimi**. Görsel altı: **Şema 1.**

> Burada (Şema 1) şarkı şeyi var ya, onu ekle. Bu şarkının süresinden
> sürükleyebileyim; dolan şekilde, bu tasarımda olsun.
>
> Şarkı bitmeye başladıkça dolan ve kalan renk değiştirsin. Burada şey olsun ve
> frekansı gittikçe artsın.

## Sayfa 1 / Şema 1 gereksinim matrisi

| Kimlik | Kaynak gereksinimi | Uygulama yeri | Doğrulama | Durum |
| --- | --- | --- | --- | --- |
| PDF4-01 | Şema 1'deki neon şarkı süresi barını ekle | `src/ui/neon_progress.c`; `src/ui/header.c` entegrasyonu root'a aittir | Header içindeki görünüm ve `/neon/geometry` Cairo piksel testi | PASS |
| PDF4-02 | Şarkı süresini dolan bir tasarımda göster | Canonical position/duration; dolan ve kalan iki görünür bölüm | Gerçek render'da dolan/kalan parlaklık farkı; adapter gerçek deck snapshot testi | PASS |
| PDF4-03 | Süre barından sürükleyerek şarkı konumu seçilebilsin | Press/motion preview ve release callback; `header_progress.c` gerçek audio seek adapter'ı başka ajana aittir | `/neon/drag-once`, iptal/sınır/ret testleri ve adapter doğru-deck/kimlik testleri | PASS |
| PDF4-04 | Şarkı sona yaklaşınca dolan bölüm renk değiştirsin | Süreye bağlı beyaz/amber/kırmızı core/glow | `/neon/colours` gerçek Cairo piksel testi | PASS |
| PDF4-05 | Şarkı sona yaklaşınca kalan bölüm de renk değiştirsin | Aynı uyarı hue'sunu daha düşük ışıkla taşıyan kalan bölüm | `/neon/colours` x408 kalan bölüm piksel testi | PASS |
| PDF4-06 | Sona yaklaştıkça frekans gittikçe artsın | Görsel pulse frekansı uyarı penceresi boyunca 0.5→3 Hz | `/neon/pulse-pause` gerçek zamanlı piksel parlaklığından gözlenen tepe sayıları | PASS |

Şemanın somut görünümü: koyu header panellerinin en alt sınırında, bütün header
genişliğini kapsayan yatay bar; küçük sol/sağ girinti; çok ince beyaz core; yuvarlak
uçlar; core etrafında güçlü yumuşak beyaz halo. Görselde ayrı bir büyük thumb,
süre etiketi veya kalın dikdörtgen slider yoktur. RadioS logosu, kaynak/çıkış,
database/clock/bitimine, monitor/headphone/level/equalizer bileşenleri barın konumunu
tanımlayan mevcut header bağlamıdır. PDF4 bu bileşenler için ayrıca davranış veya
yeni tasarım talimatı vermiyor.

## Kaynakta belirtilmeyen ayrıntılar ve açık uygulama varsayımları

Kaynak **renk kodu, eşik saniyesi/oranı, frekans birimi/değeri, pulse biçimi,
seek commit zamanı veya klavye davranışı** belirtmez. Son cümledeki “şey” açıkça
tanımlanmamıştır. “Frekans” görsel uyarının pulse hızı olarak uygulanır; şarkının
ses frekansı/pitch'i değiştirilmez. Bunlar PDF'deki birebir sayısal değerler olarak
sunulmaz. Root kullanıcıya isteğe bağlı renk/eşik sorusu yöneltti; uygulama aşağıda
açıkça kaydedilen varsayılanlarla tamamlandı.

Root'un bildirdiği varsayılanlar: normal beyaz; son 30 saniyede amber;
son 10 saniyede kırmızı; kısa şarkıda uyarı penceresi en çok şarkının dörtte biri,
kritik pencere en çok on ikide biri. Her iki bölümün rengi değişir. Pulse sadece
çalarken ve sona yaklaşınca etkindir; duraklatılmış veya süresi bilinmeyen kaynak
sakin kalır. Bar 3px core ve 24px etkileşim alanına, 10px yatay girintiye sahiptir.
Sayısal varsayımlar `NEON_WARNING_MS`, `NEON_CRITICAL_MS` ve çizim kodunda ayrıdır.

Sürüklemede hareketler yalnız önizleme üretir; bırakmada bir kez seek gönderilir.
Escape, focus/grab kaybı, süre/kaynak kimliği değişimi sürüklemeyi iptal eder.
Yetkili player snapshot konumu gerçek durumu belirler; başarısız seek sahte ilerleme
yaratmaz. Sol/sağ ok 5sn, Shift ile 10sn, Home/End başlangıç/son konumlarını ister.
Süresi sıfır/bilinmeyen kaynak seek kabul etmez. Bu teknik ayrıntılar mevcut audio
controller ile güvenli ve uyumlu etkileşim için uygulanmıştır.

## API ve dosya sahipliği

`include/radios/neon_progress.h`: new/set_state/get_state/set_seek_callback API'si.
`get_state` yalnız canonical canlı durumu verir; sürükleme önizlemesi bu API'ye
sızmaz. Callback başarı durumu döndürür. `seek-begin` sinyali hareket başlamadan
önce adapter'ın medya/generation kimliğini sabitlemesini sağlar; handler sırasında
seekability iptal edilirse hareket başlamaz.

`src/ui/neon_progress.c` ve `tests/test_neon_progress.c` bu ajanındır.
Sonradan atanan `tests/test_song_seek.c` gerçek decoder/PCM ile atomik
generation kontrolünün motor tarafındaki kabul testidir ve bu ajanındır.
`tests/test_online_progress.c` ve `tests/test_youtube_seek.c` de bu ajanın
ayrık adapter/gerçek worker kabul dosyalarıdır.
Adapter `header_progress.h/.c`, adapter testi architecture ajanındadır.
Header/CMake minimum entegrasyonu root'a aittir. Ortak kayıt yazarı coordination'dır.

## Güncel uygulama ve kabul kanıtları

Güncelleme: **2026-10-09 02:39 UTC**. Önceki geliştirme aşamalarının tarih/saat,
hata, çözüm, kaynak sahipliği ve test checkpoint'leri `../prompt/agent/AGENT.md`
PDF4 bölümünde ve `../prompt/agent/logs/pdf4-*` saatlik kayıtlarında korunur.
Burada son uygulama durumu verilir; tarihsel pending ifadeleri current sonuçların
yerine kullanılmaz.

Neon widget, yerel/çevrimiçi adapter ve gerçek transport yolları uygulanmıştır.
Şema ile karşılaştırılan son Gaussian halo'da katman halkaları görülmez;
normal/amber/kırmızı dolan ve kalan bölümler ince yuvarlak core çevresinde yumuşak
ışık taşır. Gerçek widget üç durum kanıtı `build/pdf4/neon-states.png` içindedir.

Yerel süre gerçek deck/cue/runner snapshot'ından gelir. Global sıra veya özel
AutoDJ runner duraklatılmışken fiziksel decoder PLAYING kalsa bile motorun gerçek
`runner_paused` gözlemi ve plan state'i çubuğu sakin tutar. Duraklatılmış runner'da
seek manuel devralma yapar; yeniden çalmaya başlamaz. Geçersiz cue/generation
komutu devralma veya queue değişikliğinden önce mutex altında reddedilir.

Çevrimiçi süre/konum gerçek external MAIN owner'a bağlı facade'dan gözlenir.
Bilinmeyen süre için tahmini değer veya saatle sahte ilerleme üretilmez. Kaynak
önceliği: çalan etkin global yerel sıra; çalan çevrimiçi kaynak; çalan manuel yerel
MAIN; duraklatılmış çevrimiçi kaynak; duraklatılmış/hazır yerel kaynak. Preview32
ve sampler33–65 header kaynağı taramasına girmez. Gesture source kind/ID,
generation, medya/sürüm/entry ve cue sınırlarını korur. Değişmiş kaynakla eski
release yeni şarkıya komut gönderemez.

YouTube seek güncel operate authorization ve player mutex içinde aynı
engine/video/station/user/auth/config kimliği doğrulamasından geçer. Paused
başlangıç atomiktir; MAIN rezervasyonu korunurken PCM üretilmez. Spotify MPRIS
SetPosition ve Apple MusicKit seek komutları gerçek kaynak kimliği/actual length
ve capability ile korunur. Async receipt/provider hataları sahte cursor üretmez;
sonraki gerçek gözlem gösterilir. Güncel kaynakta failure/supersede busy ownership
ve verification status temizliği de doğrulanmıştır.

| Son kaynak/test kapsamı | Vaka | Kabul kanıtı |
| --- | ---: | --- |
| GTK/Cairo neon widget | 11 | Geometry; gerçek pointer/keyboard; tek commit; iptal, ret/sınırlar; iki bölüm rengi; gözlenen artan pulse; pause/unknown sakinliği; destroy |
| Yerel header adapter | 28 | Gerçek snapshot sözleşmesi, deck/priority/cue; generation guards; kaynak değişimi; output/error/destroy; LOADING/READY seçim koruması; global ve özel runner pause |
| Çevrimiçi header adapter | 20 | Gözlenen cursor/duration; paused freeze; exact delegation; unknown/busy/output; ID/generation; kaynak öncelikleri; logical PAUSED + physical PLAYING |
| Gerçek WAV decoder/offline engine | 10 | 997Hz hedef stereo PCM; cue/generation; playing/paused; geçerli takeover ve hatalı cue'da runner koruması; paused runner sessiz/frozen seek; preview32 CUE-only public guard |
| Gerçek YouTube worker/subprocess/offline engine | 7 | 997Hz hedef PCM; paused MAIN reservation/silence/freeze; resume; video/station/user/auth stale no-mutation |
| Official C transport/async receipt fixture | 10 | Gerçek MPRIS object path/microseconds; capability/actual duration; paused Position0; obsolete/destroy/provider-error receipt; Apple nonfatal error |
| Apple protocol + üretimdeki actual JS fixture | 9 | Kimlik/duration/auth/capability, paused/playing seek, replacement ve error; skip0 |

Son strict Debug feature koşusunda **8 CTest / 103 vaka PASS16.13sn**, skip0 ve
failure0'dır:10 engine +11 widget +28 yerel adapter +7 YouTube worker +10 official
+9 Apple +20 online adapter +8 fresh authorization pipeline. Ayrıntılı TAP
`build/pdf4/final-feature-cases.log`; CTest özeti
`build/pdf4/final-feature-tests.log` içindedir. Bu dosyaların vaka sayıları bağımsız
okunarak doğrulandı.

Bunların provider/root authorization dışındaki **95 vaka** için ayrıca
strict Debug/ASan/UBSan fixture kabulü vardır. Son actual
runner_paused ABI ve core10 flag assertion'ları kapsanır. Core/provider CTest4/4
PASS3.05sn kanıtı `build/pdf4/sanitizer-observed-pause-core-20261009T023612Z.log`;
C leak detection açık ve halt_on_error etkindir. Yerel28 + online20 CTest2/2
PASS2.85sn kanıtı
`build/pdf4/sanitizer-observed-pause-adapters-20261009T023627Z.log` içindedir.
Widget11 son kaynak Gaussian sürümü frozen'dır; GTK sanitizer kabulü
`build/pdf4/sanitizer-timelines-20261009T023120Z.log` içinde ayrıca korunur.
GTK global toolkit cache'leri nedeniyle leak detection kapalıdır; UAF/UB
kontrolleri ve fatal GTK/GLib warning koşulu açıktır. Apple üretim JS'si yalnız
özel test runtime QuickJS1.19.4 ile yürütüldü; product dependency değişmedi.

Fresh operate authorization pipeline'ın **8 vaka** gerçek privateDB/context
kabulü root tarafından çalıştırılmıştır. Son feature koşusunda PASS5.38sn ve
actual1..8 TAP `build/pdf4/final-feature-cases.log` içindedir. Önceki ayrı
PASS4.14sn kabulü `build/pdf4/auth-cases.log` içinde tarihsel kanıt olarak korunur. Yeni kaynak, pause veya STOP değişimi sırasında bekleyen
seek; permission reddinde mevcut parçanın korunması; actual MAIN metadata;
stale bounds; busy/status cleanup doğrulanır. Sağlayıcı gözlemleri bu fixture'da
kontrollüdür; bu test gerçek kullanıcı hesabıyla DRM/provider erişimi değildir.

WAV ve YouTube helper dosyaları yalnız `build/pdf4/song-seek-*` ve
`build/pdf4/youtube-seek-*` özel geçici dizinlerde oluşur ve teardown temizler.
Testler kullanıcı config/credentials dosyalarını veya gerçek hesabı değiştirmez.

## Gerçek header yerleşimi ve görsel karşılaştırma

Production `radios_header_new()`, derlenmiş reference CSS, mevcut `radios.css`,
header status CSS ve gerçek Inter fontuyla bağımsız scratch harness yürütüldü.
Uygulama kaynakları ve kütüphanelerin SHA/mtime manifesti, gerçek allocations ve
PNG'ler `../prompt/agent/analysis/requirements/header-final-20261009T023646Z/`
içindedir. Strict C11 compile ve fatal-warning X11 execution exit0; input hash
drift0; 42 section gözleminin tümü gerçek 1px'ten büyük allocation taşır.

İlk denetimde during-size-allocate reparent sonucunda section'lar 1×1 allocation
ve eski child konumlarında kalıyordu. Root layout işini deferred idle'a taşıyıp
resize kuyruğunu ve destroy cleanup'ını düzeltti. Son aynı-pencere roundtrip:

| Senaryo | Gerçek genişlik | Header yüksekliği | Bar genişliği | Grid span |
| --- | ---: | ---: | ---: | ---: |
| İlk geniş yerleşim | 1920 | 267 | 1824 | 4 |
| Orta yerleşim, ikinci resize | 1280 | 417 | 1184 | 3 |
| Compact | 1000 | 502 | 904 | 2 |
| Aynı pencere tekrar geniş | 1920 | 267 | 1824 | 4 |

Fresh1280 ve fresh1000 aynı doğru yerleşimleri verir. Bar her yerleşimde row4'te,
24px etkileşim alanıyla bütün header içerik genişliğini kapsar. Sağ/sol container
padding48px ve iç core inset10px eşittir; core yaklaşık58px ekran girintisinden
başlar. Bölümlerin gerçek allocation'ları yeni grid konumlarına uygundur.
Wide→compact→wide yüksekliği267→502→267 olarak eski yüksekliğe döner; büyüyen
stale layout kalmaz. Bekleyen layout allocation'ı hemen destroy edildiğinde idle
cleanup ve 50ms event drain fatal warning üretmez.

Ölçülen GTK minimum sınırı açık tutulur: 1920px geniş yerleşimden tek1280px resize
isteği önceki1514px minimumda kalır; orta layout oluştuktan sonra aynı1280px
isteği gerçek1280px olur. Fresh1280 de gerçek1280px'tir. İlk isteği1280px olmuş
gibi raporlamıyoruz. Bu kaynak framework minimum negotiation gözlemidir;
bar geometry veya deferred-layout regression başarısızlığı değildir.

Final header1920/1280/1000 PNG'leri görüntülenip Şema1 ile karşılaştırıldı.
Neon bar doğru alt sınırda, eşit yatay girintili, ince yuvarlak core ve yumuşak
halo taşır. Unknown/idle kaynakta sakin düşük ışıklı kalan bölüm gösterilir;
actual playing source normal ve uyarı renklerini widget piksel/üç durum kanıtı
ayrıca doğrular.

## Son doğrulama sınırı

PDF4'ün altı kaynak gereksinimi uygulanmış ve kapsamına uygun test/görsel kanıtıyla
karşılanmıştır. Root son strict13 hedef derlemesini source-edit mutex altında
başarıyla tamamladı; `build/pdf4/verified-build.log` ve
`build/pdf4/compiled-source-manifest.json` exit0, drift0 ve source/binary SHA
kanıtını taşır. Bu izole PDF4 build'i varsayılan `build/radios` kurulumunu değiştirmez.

Mevcut regresyonlardan `audio`, `runner`, `native_controls`, `native_audio` ve
`native_window` **PASS**; `native_audio` actual virtual Pulse koşusu40.87sn.
`build/pdf4/final-regression.log` ve ayrıntılı `regression-cases.log` kalıcı
kanıtlardır. Eski `youtube_music_audio` hedefi özel Pulse daemon executable'ı
bulunmadığından **SKIP** olmuş, çalışmış/PASS olarak gösterilmemiştir. Yeni7
YouTube seek vakası gerçek worker + offline PCM engine ile ayrıca PASS'tir.

Fresh whole-app `build/pdf4/checks/native-window.png`02:36:19 capture'ı bu ajan
son kez görüntüleyip Şema1 ile karşılaştırdı: timeline bütün header'ın altına,
ince rounded soft glow ve eşit girintilerle doğru yerleşmiştir. Yeni deferred
layout ve gerçek section geometry ile tutarlıdır. Şemada white full bar,
uygulamanın unknown/idle capture'ında sakin düşük ışık; actual filled/warning
halleri ayrı gerçek widget Cairo/PNG kanıtlarında doğrulanmıştır.

Canlı YouTube/Spotify/Apple hesabı, DRM ve gerçek sağlayıcı ağı üzerinde seek
kabulü yapılmamıştır. Kaynak/UI, gerçek kontrollü PCM worker ve protocol
fixture kabulü bu ortam sınırından ayrı ve açık şekilde kaydedilir. PDF4'e ait
başka uygulanmamış kaynak gereksinimi bu son checkpoint'te kalmamıştır.
