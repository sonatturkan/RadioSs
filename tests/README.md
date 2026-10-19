# Doğrulama

## Başlık Ayarlar menüsü

`native_controls`, dişli simgesini, gizli müzik menü öğesini ve **Ses ve görünüm**
seçeneğinin doğrudan aygıt/görünüm sayfasını açmasını sınar. Yazı boyutu ve
başlangıç profili aynı gerçek tercih dosyasında kaydedilip yeniden açılır.
## Yayın Playlist sekmesi ve program barı

`native_broadcast_catalogue`, gerçek izole DB ile Playlist sekmesinin doğrudan
liste göstermesini, medya tablosunu gizlemesini, program adı/tarihi ve düzenleme
kontrollerinin barın sağında kalmasını, kapalı/açık program menüsünü ve sekme
değiştirmenin programı/yayını değiştirmemesini ve gerçek playlist parçalarını
tek event olarak program sırasına kalıcı eklemeyi sınar. Sekmedeki görünür
**Playlist oluştur** düğmesi, iptal sırasında kayıt oluşturmama, başarılı
oluşturmada arama filtresini temizleyip gerçek kayıt kimliğini seçme ve yeniden
açıldığında listeye erişme de denetlenir. Mevcut katalog/event/CAS/SHA/owner-close
kabulü korunur.
`native_programme` onay ve kullanım hakları akışını yeni menü üzerinden sınar.

`native_programme_queue`, playlist/program bırakmanın genel event listesinde
hedefin ardını; açık/boş eventte satır önünü/arkasını veya listenin sonunu
seçmesini doğrular. `native_broadcast_catalogue` gerçek GTK drag payload'larını
ve izole HTTP/DB'yi kullanarak otomasyondan gelen eventin ardından playlist,
açılmış evente bütün playlist/tek parça/kayıtlı program, anında görünür sıra,
seçim ve kalıcı yeniden açılışı sınar. Gerçek hazırlık yanıtı bekletildiğinde
hedef değişikliğinin ve owner kapanışının programı değiştirmediği de denetlenir.
Clipboard tabanlı bu drag testleri X11 kullanır; CTest `GDK_BACKEND=x11` ayarlar.
Python wrapper doğrudan çalıştırılırken aynı ortam değişkenini kullanın.

## Çevrimiçi müzik kabulü (kaldırıldı)

17.10.2026'da YouTube Music, Spotify ve Apple Music altyapısı projeden çıkarıldı;
`youtube_music_*`, `native_youtube_music`, `native_broadcast_online`, `music_match`
ve `native_online_services` hedefleri artık yoktur. Kalan kabul yalnız yerel medya
arşivini sınar.

## Diğer kabul senaryoları

`native_programme_queue`, aynı tablodaki event/içerik geçişini yedi GTK
senaryosuyla sınar: düz liste ve numaralama, parça/event sıralama, boş event ve
liste sonuna bırakma, silinen event, bırakma sonrasında satıra tıklama ve bekleyen
geçiş sırasında kapanış. Test kendi özel X11 seçim alanını kullanır; kullanıcının
panosunu değiştirmez.

`native_broadcast_catalogue`, sağdaki Programlar listesinin gerçek sürükleme
verisini yayın sırasına teslim eder. Bütün içeriğin tek program eventine
kopyalanması, yeni öğe kimlikleri, kaynak kaydın korunması, ağaçsız içerik
görünümü ve yeniden açılışta kalıcılık özel HTTP/DB üzerinde doğrulanır.

```sh
ctest --test-dir build --output-on-failure -R '^(programme_model|native_programme_queue|native_broadcast_catalogue|native_controls)$'
```

`native_routing`, açık kullanıcı işlemiyle seçilen uygulamayı gerçek MAIN
motoruna bağlar. MAIN başlatma WAV yolu, kayıt çıkışı veya mikrofon istemez;
mikrofon checkbox'ı açık olsa bile bu aktarım mikrofon yakalamaz. Güncel
aktif kullanıcı, istasyon ve `operate` yetkisi her başlangıçta doğrulanır.
Eski yetkili kullanıcının gerçek HTTP yanıtı bekletilirken aynı veri
nesnesine yeni izleyici hesabı bağlanır. Özel testin ikinci gerçek iş
kuyruğu, yeni bağlantının eski yanıt dönmeden tamamlanmasını sağlar; sonra
eski yanıt serbest bırakılır. WAV ve MAIN başlangıçlarının reddedildiği,
dosya, mikrofon yakalaması veya dış MAIN beslemesi oluşmadığı ölçülür.
İptal ve pencere kapanışı eski başlangıç niyetini geçersiz kılar.

C ses kabulü dış MAIN oturumunun token sahipliğini, eski token reddini,
PCM örnek/blok/kuyruk sınırlarını ve ortak MASTER DSP/kazancının
uygulanmasını sınar. Özel Pulse koşucusu yalnız seçilen uygulamanın PCM'sini
MAIN'e aktarır; diğer uygulama ve mikrofon karışımını bağımsız frekans
ölçümüyle reddeder. Gerçek MAIN WAV kaydı ve özel HTTP MP3 kaynak alıcısından
çözülmüş ses ayrıca ölçülür. Kaynak kaybı, durdurma, başlangıç çıkışının
geri yüklenmesi ve kendi geçici sink/modülünün temizliği denetlenir. Bu
kapsam üretim kullanıcı verisini, gerçek hizmet hesabını veya fiziksel
mikrofonu test kaynağı yapmaz; ana sistem ses kabulünün yerine geçmez.

Son odaklı GTK koşusunda **iki hedef / dokuz senaryo**, başarısız/atlanan
test olmadan **12,77 saniyede** geçti: çevrimiçi hizmetler **4** senaryo
**2,16 saniye**, yönlendirme **5** senaryo **10,61 saniye**. Kanıt:
`build/online-018-native-final.xml`. Birleşik normal ve sanitizer sonuçları
bu odaklı koşunun kapsamına dahil değildir.

İlk yönlendirme koşusu bağımsız WAV PCM ölçümünde başarısız oldu.
Korunan WAV'da ilk **1,420 saniye** sıfır PCM, ardından seçilen 440 Hz
kaynakta **0,14159**, 880 Hz sanal mikrofon kaynağında **0,25059** genlik
ölçüldü; ikinci WAV baştan sona normaldi. Değişmeyen 0,5–1,5 saniye ölçüm
penceresi henüz ses üretmeyen yeni sentetik Pulse kaynaklarının başlangıç
sessizliğine denk geliyordu. Native özel koşucu, diğer özel yönlendirme
koşucusundaki **2,3 saniyelik kaynak hazırlığını** kullanır; WAV ölçüm
penceresi, bütün sinyal/ayrıştırma eşikleri ve üretim zamanlaması korunur.
İlk başarısız koşu ve PCM çözümlemesi
`build/online-018-routing-ui-diagnostic/acceptance.json` ve
`build/online-018-routing-ui-diagnostic/pcm-analysis.json` içindedir.

0.17.1 sürümünde GUI kabulü açıkken **56 CTest hedefi** vardır; eklenen
`native_data_connection` ve `native_media_drop` gerçek bağlantı ve dosya
bırakma akışını sınar. Yeni odaklı koşuların kapsamı
[sürükle bırak belgesindedir](../docs/medya-surukle-birak.md).

0.17.0 birleşik koşusu **54/54**, **331,91 saniyede**, atlanan test olmadan geçti.
Birleşik sonuç `build/closure-full.xml` içinde tutulur. Önceki aşama sayıları
aşağıda tarihsel kabul kayıtlarıdır.

0.17.0 ASan/UBSan kabulü core **3/3** hedefi **20,88 saniyede**, native **8/8**
hedefi **116,50 saniyede** geçti; başarısız/atlanan test yoktur. Core leak
denetimi açık, GTK süreç global verileri için kapalıdır. Son test kaynaklarının
SHA-256 listesi `build/closure-tested-source-snapshot.json` ile bağlıdır.

0.17.0 ile gerçek Rekordbox XML preview/apply, programın formal onay/hak
akışı, katalogdan program öğesi ekle/değiştir, adlandırılmış program kopyası
sakla/yükle, acil sırayı programa aktar, dış HTTPS panelini aç/test et ve
SIGTERM/SIGINT ile normal audio/data kapanışı bağlandı. Backend program
**12/12**, Rekordbox **10/10**; native katalog **2**, Rekordbox **2**, acil
aktarım **3**, dış panel **3** ve birleşik formal program **1** GTK senaryosu
odaklı kabulde geçti. Gerçek TLS erişim testinin **5** core grubu vardır.
Normal uygulama kapanışı iki sinyalde gerçek PCM WAV başlığını tamamlar,
servisi korur, kilidi bırakır ve yeniden açıldığında kayıt başlatmaz; önceki
sinyal davranışını taklit eden negatif kontrol reddedilir.

C veri istemcisinin önceki **19** grubu gerçek HTTP 429 sonrası
salt okunur yanıtın alınmasını; beş deneme sınırını ve ilk 429 yanıtından sonra
**sekiz saniyelik** tekrar penceresini, uzun `Retry-After` bilgisinin kısaltılmamasını, iki iptal
yolunu ve yazmanın tek denemede kalmasını doğrular. Sunucunun 120 istek / 10
istek/saniye politikası değiştirilmez. Kanıt: `build/closure-read-rate-retry.json`.

Yeni testler gerçek özel HTTP/DB ve kontrollü dosya/Pulse kaynaklarını
kullanır. Commit sonrası yanıt kaybı, aynı işlem alındısı, değişen kaynak/hedef
ve kullanıcı, CAS reddi, iptal ve pencere kapanışı sınanır. Üretim DB'sine
test verisi eklenmez. ASan/UBSan core leak denetimi açık, GTK leak denetimi
kapalı çalıştırılır. [Kapanış kapsamı ve açık alanlar](../docs/proje-kapanis-denetimi.md).

USB DeviceSQL kabulünde `desktop_rekordbox_device` **10** yerel senaryoyu;
`native_rekordbox` **2** eski XML ve **1** gerçek ikili USB senaryosunu
çalıştırır. Ek C veri istemcisi grubu **3 MiB** kaynak kopyasının kayıp yanıt,
kuyruk yeniden açılışı ve birebir tekrarını; geniş limitin diğer yollara
uygulanmamasını doğrular. USB çıkarılması, kaynak byte'larının değişmemesi,
klasör/sıra/tekrarlar, Unicode, bozuk veri ve CAS özel DB'de sınanır.
`RADIOS_TEST_USB_EXPORT` ile GitHub özgün CDJ örneği seçildiğinde ek Python
ve GTK senaryoları **651 parça / 40 sıralı playlist öğesini** doğrular.
[Kullanım, örnekler ve tekrar komutları](../docs/rekordbox-usb-tracklist.md).

Veritabanının dokuz özgün koşucusu son kaynak snapshot'ında **333/333**, sıfır
başarısız/atlanan kontrol ile **67,685 saniyede** geçti. Kaynak sapması ve
üretim DB değişikliği yoktur; bu koşular fiziksel veya 24 saat yayın kabulü
değildir. Kanıt: `build/source-database-engine-suite-final/summary.json`.

Son normal ve sanitizer ses kabulleri ayrı koşulur. İkisi eşzamanlı çalışırken
bir DJ testinin PCM eşiği sağlanmadı; değiştirilmemiş beş DJ grubu ayrı özel
koşuda geçti. Kesin neden doğrulanmamıştır; bu sonuçtan bir DSP hatasının
düzeltildiği çıkarılmaz. Kanıt: `build/closure-native-dj-unchanged.json`.
Program testleri önceden görünen boş satıra veya sabit 200 ms beklemeye göre
hazır sayılmaz; gerçek UUID/revision, bağlantı ve temiz kayıt onayı beklenir.
Default native playout koşucusu GLib'in gerçek beş senaryosunu keşfeder;
her biri bağımsız özel DB kullanır, senaryo içindeki yeniden açılışlar aynı
DB'de kalır. TAP planı ve her senaryonun gerçekten çalışması denetlenir;
eksik/atlanan senaryo kabulü geçemez.

0.16.0 ile `online_services`, `desktop_online_services` ve `native_online_services` eklendi. C bağlantı katmanının **16/16** grubu yalnız özel loopback API'lerinde üç servis sözleşmesini, sayfalamayı, kısmi yazmayı, hata/anahtar korumasını, iptali, gerçek yerel PKCE dönüşünü ve portun yeniden kullanımını sınar. Gerçek HTTP/DB **9/9**, native GTK **2/2** geçti. Native test özel HTTP yanıt kapısıyla güncel yetki, iptal ve pencere kapanışını doğrular; testte browser açılışı link wrapper'ıyla engellenir. Core ASan/UBSan leak denetimi açık, GTK leak denetimi kapalıyken geçti. Canlı servis hesabı/OAuth kodu kullanılmadı. [Kapsam ve kullanım](../docs/cevrimici-muzik.md).

0.15.0 ile `audio_routing`, `desktop_routing` ve `native_routing` eklendi. Özel Pulse uygulama/mikrofon/üçüncü kaynaklarıyla bağımsız PCM analizi, gerçek kompresör, Pause/Resume/Split/Stop, dosya koruma, kaynak kaybı ve thread/FD temizliği sınanır. HTTP sözleşmesi **7/7**, GTK kabulü **3/3** geçti. GTK ayrıca kapanış sırasında bekleyen yetki isteğini ve izleyici kullanıcının reddini sınar. Motor ASan/UBSan ve leak denetimi açık, GTK aynı sanitizer'lar ve leak denetimi kapalıyken geçti. [Kapsam ve kanıtlar](../docs/ses-yonlendirme.md).

14. aşamada `packaging`, `release_install` ve `audio_soak` eklendi. `test_packaging.py` **13 grupta** özel veri dışlama, deterministik hash/manifest, arşiv yol/link/boyut/izin ve başarısız hazırlama güvenliğini; `test_release_install.py` **8 grupta** gerçek native başlatıcıyla yeni kurulum/tekrar açılış, pasif kipler, geçersiz ayar/argüman, headless kullanım, kendi servisini yeniden başlatma, yabancı port ve restore korumalarını sınar.

`run_release_acceptance.py`, üretilen iki gerçek arşivi bağımsız köklere açar; sekiz C/GTK görünümü, yeni özel kimlikler, typed HTTP/CAS/alındı ve yeniden başlatma kalıcılığını doğrular. Kaynak arşivini mevcut kaynak kökünden bağımsız strict optimize Release olarak derler. `--host-runtime`, ana sistemde eksik FFmpeg ABI'sini ve mevcut SDK ile iki gerçek açılış arasında aynı kimlik/servis sahipliğini, ana sistemden servis stop/start akışını da sınar. Testlere ait süreçler ve veriler temizlenir.

`test_audio_soak.c` ve `run_audio_soak.py`, özel Pulse saatinde gerçek MAIN/crossfade/EOF/kuyruk ekleme/WAV/MP3 üretimini bağımsız monitor, WAV ve HTTP alıcısından çözülmüş MP3 ile ölçer. CTest kısa koşuyu çalıştırır; ayrıca gerçek **300,011 saniyelik** koşu ve leak denetimi açık kısa ASan/UBSan koşusu geçti. Beş dakikalık kabul fiziksel dinleme veya 24 saat işletme kanıtı değildir. Komutlar ve fiziksel kabul sınırları [14. aşama belgesindedir](../docs/14-nihai-kabul-ve-paketleme.md).

AutoDJ deck geçişleri `test_playout.c` içinde decke özel EOF/crossfade/cue/manuel, gerçek başlangıç/fade süreleri, devam partisi deck fazı, sınır geçişinden öncelik, kuru jingle ve bozuk politikanın reddiyle denetlenir. `test_desktop_playout.py` eski kayıt uyumluluğu, gerçek DB kaydı/yeniden açılış, CAS çatışması ve geçersiz politika reddinin sequence değiştirmemesini kapsar. `test_playout_ui.c` native deck kontrollerini, kaydet/yeniden açmayı, sessiz önizlemeyi, iki ayrı deckten gerçek MAIN crossfade PCM'sini, EOF dönüşünü ve sonraki partide aynı deck politikasının korunmasını sınar. Bu testler üretim DB'sini veya açık uygulamanın sesini kullanmaz.

Native AutoDJ kabulü ayrıca manuel deckte gerçek EOF beklemesini ve **Sonraki decke geç** ile atanmış A deckine dönüşü ölçer. Normal program yürütülürken AutoDJ NEXT düğmesi kapalıdır; yeni kontrol başka bir sıra türünü ilerletmez. Yürütücü sahipliği yalnız seçilen iki normal deckte kalır, CUE sessizdir.

```sh
ctest --test-dir build --output-on-failure -R '^(packaging|release_install|audio_soak)$'
python3 tests/run_release_acceptance.py --binary-archive dist/RadioS-0.17.0-linux-x86_64.tar.gz --source-archive dist/RadioS-0.17.0-source.tar.gz --report build/closure-release-acceptance.json --jobs 3 --host-runtime
```

C uygulamasının yapılandırma ve eşzamanlı iş/durum kuyruğu testleri `test_config.c` ve `test_runtime.c` içindedir. `test_ui_preferences.c` sürüm/aralık/dosya sınırı, UTF-8, bağlantı kaçışı ve atomik özel görünüm dosyasını sınar. CMake/CTest bunları ayrı C hedefleri olarak derler. Testler geçici bağımsız dizinler kullanır; üretim veritabanına bağlanmaz.

`ctest --test-dir RadioS/build --output-on-failure` temel testleri çalıştırır. `RADIOS_GUI_TESTS=ON` gerçek Wayland/X11 ekranı gereken `native_window`, `native_controls`, `native_data` ve `native_media` testlerini ekler; ekransız CI ortamında kapalı bırakılır. Kontrol testindeki 10 grup: marker doğrulama/geçmiş/vazgeçme, geçiş alan/dosya/seviye korunması, başarısız kayıtta giriş koruma, bütün profiller ve açık editörlerle kapanış, playlist/pad/acil sıra etkileşimleri, görünüm/font/son profil kaydı ve yeniden açılışı. GTK uyarıları bu testte hata kabul edilir. `native_window`, sekiz profili ve arka plan durum teslimini gerçek pencerede doğrular. Fiziksel ses/yayın kabulü sonraki aşamalardadır.

Mevcut SDBASE'in kendi operasyon/kabul araçları `database/SDBASE/tools/` altında korunmuştur. Bunlar native RadioS ses cihazı testlerinin yerine geçmez.

`test_database_setup.py` 14 gerçek CLI/HTTP kabul testiyle bağımsız şema 4 kurulumu, özel kimlik dosyası, auth403, bütünlük, tekrar başlatma, kalıcı playlist yazısı, bozuk/başka depoya ait dosyalar ve yabancı port dinleyicisinin korunmasını sınar. `test_database_recovery.py` dört grupta kesilen ilk kurulumun aynı UUID/tokenlarla devamını, çakışan hedefi korumasını ve yayın öncesi hata temizliğini denetler. Fixture'lar `build/` altında, servisler ayrı loopback portlarda kurulur; gerçek RadioS DB'sine test kaydı yazılmaz. İki test CTest'e bağlıdır; aşama 7 ile GUI testleri açıkken toplam 15 kabul hedefi tanımlıdır. Python testleri Python 3.11+ standart kütüphanesini kullanır.

Aşama 6: `test_data_client.c` özel kimlik, gerçek loopback HTTP, hata/JSON/boyut sınırı, iptal, zaman aşımı, depo ve şema kontrolünü sınar; aşama 7'nin bağımsız `GCancellable` testiyle güncel kapsam 10 gruptur. `test_desktop_api.py` 11 grupta typed config/CAS/alındı/yetki/kampanya ve özel kullanıcı anahtarını; `test_data_playlists.py` 8 gerçek HTTP grubunda playlist CRUD/sıra/silme/otomasyon/reopen davranışını denetler. `test_data_ui.c` ve `run_data_ui.py`, build altındaki izole gerçek serviste native form kalıcılığı, gerçek 409 ve servis kesintisinde input koruma, bağlantı sırasında kapanışı denetler. `native_data` CTest hedefi gerçek ekran ister. Testler üretim DB’sine kayıt eklemez.

Native veri kabulünün 6 grubu: playlist kalıcılığı; gerçek 409’da girdi koruma; servis kesintisinde girdi koruma ve açık kaydetmeyle devam; bağlantı sırasında üç kapanış/yeniden açılış; tüm profil kayıtları, kanal 9 sampler ataması, acil iletişim listesi, kullanıcı/izin/özel anahtar ve kampanya yeniden açılışı; daha yeni LIST yanıtı beklerken yerel alan değişmesinin eski CAS tabanını koruması. Test için yanıt bekletme kapısı yalnız build altındaki izole backend kopyasına eklenir.

Aşama 7'nin `test_media.c` / `media` hedefi gerçek PCM süre/hız/kanal/peak/RMS ölçümünü, SHA'yı, float clipping ve waveform min/max boyutlarını; WAV, MP3, FLAC, Ogg, M4A ve AAC tam çözümlemesini; özel staging, kesilmiş içerik, normal dosya/boyut/yol sınırını, değişen kaynak ve iptal temizliğini doğrular. Format fixture'ları için `ffmpeg` komutu ve MP3/FLAC/Vorbis/AAC encoder'ları gerekir. Uygulamanın normal C analiz yolu harici süreç çalıştırmaz.

`test_desktop_media.py` / `desktop_media` 14 grupta gerçek PCM ile tek atomic katalog yayını, yönetilen baytların SHA/size eşitliği, içerik sürümünün draft kalması, kalıcı alındıdan tekrar, kesilen commit'te katalog koruma ve obje reuse, path/symlink/FIFO reddi, yanlış analiz verisi, metadata/cue CAS, sanatçı düzeltme/temizleme/yeniden eklemenin genel katalog aramasıyla tutarlılığı, paylaşılan item yetkisi/onaylı sürüm guard'ı, yönetilmeyen katkının korunması, 403/404/409/422 HTTP, gerçek medya aidiyeti, bozuk/kayıp objeye path vermeme ve 111 Unicode ağırlıklı kayıtla byte sınırlı sayfalama kabulünü yapar.

`test_data_ui.c` / `native_media`, izole gerçek servis ve gerçek C decoder/import yolu ile native kütüphane, metadata ve SHA/süreye bağlı cue kalıcılığını; gerçekten commit edilmiş POST'un yanıtı düşürüldükten sonra aynı gövde/anahtarla aynı UUID'yi almayı; iptal/kapanışı, gerçek dosya seçici/arama ve metadata/işaret 409'larında girdi korumayı beş grupta denetler. Yanıt düşürme ve istek bekletme kapıları yalnız build altındaki fixture kopyasındadır. Bekleyen C istekleri ana pencere açıkken korunur; uygulama çökmesinden sonra bellekteki import kuyruğunu otomatik kurtarma bu testlerin iddiası değildir.

Medya testlerini ayrı çalıştırmak için:

```sh
ctest --test-dir RadioS/build --output-on-failure -R '^(media|desktop_media|native_media)$'
```

Son kabul kaydı: strict C11 ve birleşik CTest 15/15 başarılı (34,31 saniye); C analiz 10/10 ve ASan/UBSan 10/10; native medya 5/5 ve son kaynaklarla ASan/UBSan 5/5 (15,18 saniye), backend medya 14/14; C veri istemcisi ve leak detection açık sanitizer kabulü 10/10 başarılı. GUI sanitizer koşusunda GTK süreç global verileri için leak detection kapalı; core analiz/istemci koşusunda açıktır. Gerçek servis/C uygulaması son sürümle açıldı; mevcut UUID'ler, sequence 2 ve bütünlük `ok` korundu, üretim DB'sinde test fixture'ı yoktur. MP3/ham ADTS AAC'de toplam örnek sayısı bilinmiyorsa tam paket sınırındaki her kısaltmanın yakalandığı iddia edilmez. Ayrıntılı kapsam ve sürüm bağımlılıkları [medya kütüphanesi belgesindedir](../docs/07-medya-kutuphanesi.md).

## Aşama 8 ses kabulü

GUI kabulü açıkken toplam 19 CTest hedefi tanımlıdır. Önceki hedeflere `audio_decoder`, `audio`, `audio_devices` ve `native_audio` eklenmiştir. Normal uygulama kütüphaneleri doğrudan C'den kullanır; ses fixture kabulünde `ffmpeg`, `pactl` ve `parec` komutları gerekir. Testler sistemin varsayılan fiziksel çıkışını veya mikrofonunu ses fixture'ı için kullanmaz.

`test_audio_decoder.c` 11 grupta 48 kHz stereo gerçek örnek karşılaştırması, 44,1 kHz mono resampling, çok kanal downmix, MP3/FLAC/Ogg/M4A/AAC tam çözümleme ve tam örnek aramayı; SHA, özel ve Unicode yol, symlink/FIFO/URL reddini; değişen/kesilmiş dosya, sınırlar, float/NaN ve çalışma sırasında iptal/yeniden kullanım davranışını sınar.

`test_audio.c` altı deterministik gerçek PCM grubunda çoklu karışım, kazanç, bağımsız bus/kapı, cue aralığı, duraklatma/arama/durdurma/EOF'dan yeniden başlatma, ön dinlemenin MAIN'e gidememesi, bozuk giriş, yeni yüklemenin eskisini geçersiz kılması ve 33 oynatıcı sınırını denetler. Yedinci gerçek Pulse grubu sanal MAIN/CUE/MON akışlarında gerçek tüketimi, giriş yakalamayı, mikrofon kapısını, tüketilen PCM ölçümlerini ve eksik adlandırılmış cihazın reddini sınar. `audio` hedefinde sanal cihaz ortamı yoksa bu son grup atlanır; `audio_devices` koşucusu cihazları hazırlayarak yedinci grubu da gerçekten çalıştırır.

`test_audio_ui.c` altı native grupta gerçek import/kütüphane → oynatıcı düğmeleri → yakalanan PCM akışını, -12 dB gain oranını ve duraklatma sessizliğini; ayrı CUE ve waveform düğmelerini; istasyon/yol/SHA/symlink reddini; tercihlerde gerçek 409, girdi koruma, açık yeniden yükleme ve yeniden açılışta kapalı canlı kontrolleri; eski generation ve pencere sahipliğiyle güvenli kapanışı sınar. Son grup, kartın kütüphane kapandıktan sonra kendi doğrulanmış kaydıyla CUE başlatmasını, daha yeni farklı medya ön dinlemesinin eski kart kaldırıldığında sürmesini, CUE kapatmanın geciken bir yanıtla geri alınamamasını ve gerçek CUE sessizliğini doğrular.

`run_audio_ui.py`, yalnız `build/` altındaki ayrı SDBASE kopyasını/istasyonu kullanır. Üç benzersiz sanal çıkış, bunların float PCM yakalamaları ve sanal bir monitor girişini hazırlar. Geciken medya GET alındısı için iki saniyelik bekletme kapısı yalnız bu özel backend kopyasına eklenir. Koşucu yalnız kendi süreçlerini ve sayısal modül kimliklerini temizler. Üretim backend'ine test kapısı eklenmez.

```sh
ctest --test-dir RadioS/build --output-on-failure -R '^(audio_decoder|audio|audio_devices|native_audio)$'
python3 RadioS/tests/run_audio_ui.py --engine-only RadioS/build/test_audio
```

Ses motorunun ve cihazların kaynak/kullanım sözleşmesi [8. aşama belgesindedir](../docs/08-ses-motoru.md).

Son 8. aşama kabulü: strict C11 ve birleşik CTest **19/19** başarılı (134,77 saniye). Çözücü **11/11**, motor **6/6** ve gerçek Pulse **1/1**, native ses **6/6**, typed API **13/13** başarılıdır. Core çözücü ve motor/Pulse aynı kapsamda ASan/UBSan + leak detection açıkken temiz; native sesin altı grubu ASan/UBSan altında (41,66 saniye test uygulaması), GTK global verileri için leak detection kapalıyken temizdir. Gerçek cihaz adları ve kayıp cihaz reddi test edildi; fiziksel dinleme/on-air/uzun süre kabulü bu sonucun kapsamı değildir.

## Aşama 9 DJ ve geçiş kabulü

GUI kabulü açıkken toplam **22 CTest hedefi** tanımlıdır. `dsp`, `transition` ve `native_dj` eklendi. Motor 32 normal oynatıcı, ayrı önizleme ve 33 sampler sesiyle toplam 66 sınırlı dahili sese çıktı. Önceki aşamanın kabul kayıtları yukarıda tarihsel kapsamıyla korunur.

`test_dsp.c` 13 grupta kaynak/kanal yönlendirmesi, çift atama, mute/solo/PFL, sampler, pan/gain ve eşit güç crossfade; ölçülmüş üç bant EQ; 14 ayrı efektin gerçek PCM cevabı, bypass/depth/BPM, send/return ve seçili efekt CUE'su; ortak MASTER EQ/aux/kendi efekti ve gerçek mikrofon seviyesine bağlı ducking davranışını sınar. Motorun dokuz deterministik grubu ile ayrı gerçek Pulse grubu kaynakların bir kez ilerlemesini, atanmamış kaynağın ilerlememesini, MAIN bulunmadığında PFL/CUE sürücüsünü ve geçici önizlemenin unload ile EMPTY olmasını da denetler.

`test_transition.c` üç grupta iki doğrulanmış WAV'ın sample değerlerini bağımsız beklentiyle karşılaştırır. Gerçek A ducking/B fade, sınırdaki seviye ve sert geçiş, SHA uyuşmazlığı, süre sınırı, iptal ve özel geçici dosya temizliği kabul edilir.

`test_dj_ui.c` beş native grupta gerçek mixer/EQ/PFL/crossfade/metre ve yakalanan MAIN/CUE PCM'sini; kalıcı ayarlar, MASTER gain'in tek uygulanması, gerçek CAS ve yeniden açılışta canlı kapıların kapalı olmasını; kanal/MASTER pad ataması, yeniden tetikleme ve medya/sahiplik reddini; silinen kanalın ardından kalan kimliklerin korunmasını; gerçek A/B CUE miksini, Stop/EOF/owner kapanışı ve geciken hazırlığın yeni önizlemeyi ele geçirememesini sınar. Canlı CUE durumu kayıt yanıtıyla sıfırlanmaz. Uygulanmamış Teknik cihaz seçimi varken ikinci DJ kaydı bu seçimi ve `audio_preferences` revision'ını korur.

Geçiş fixture'ı 48 kHz'de **960.031 kare** içerir; katalog süresi **20.001 ms**'ye yuvarlanır. Kısa A/B önizlemenin bütün dosya sonuna ulaşması, hatasız EOF ve yakalanan yaklaşık 72.062 karelik ses aralığı kontrol edilir. Böylece yuvarlanmış sürenin gerçek kare sayısını aşması native kabulde kapsanır. `run_dj_ui.py`, mevcut özel backend/sanal ses koşucusunu kullanır; üretim DB'sine veya fiziksel ses aygıtlarına fixture göndermez.

Typed backend API'nin 18 grubu exact `dj_mixer` şeması/aralıkları, MASTER slotu, kalıcı sampler/A/B medya kimlikleri, yetki/istasyon sınırı, CAS/alındı/replay ve yeniden açılış bütünlüğünü içerir.

```sh
ctest --test-dir RadioS/build --output-on-failure -R '^(dsp|audio|transition|native_dj|desktop_api)$'
python3 RadioS/tests/run_audio_ui.py --engine-only RadioS/build/test_audio
```

Birleşik kabul **22/22** başarılı (173,78 saniye). Son controller düzeltmeleri ardından normal native ses **6/6** (27,89 saniye), ASan/UBSan native ses **6/6** (50,61 saniye), güçlendirilmiş native DJ normal **5/5** (32,73 saniye) ve ASan/UBSan **5/5** (54,77 saniye) başarılıdır. Core DSP/motor/geçiş ASan/UBSan koşusunda leak detection açık; GTK kabulünde süreç global verileri için kapalıdır. Ayrıntı [DJ ve geçişler belgesindedir](../docs/09-dj-ve-gecisler.md).

### DJ kanal bağlantıları ve ON AIR

Güncel `test_dsp.c` **17**, `test_dj_ui.c` **6** grup içerir. Ek kapsam, kanallar arası gerçek SEND/RETURN PCM seviyesini, aynı bağlantının iki kez karışmamasını, kanal sırasından bağımsız zincirleri, kendi kanalına/geri besleme döngüsüne bağlantı reddini, yeni alanların kalıcılığını ve kanal kaldırılırken bağlantı temizliğini doğrular. Yönlendirilmiş mikrofon müzik ducking'inden ayrı kalır; kapı kapandıktan sonra gecikmeli efektlerden konuşma sızmaz. Native test dört padin gerçek kare boyutlarını ve 2×2 konumlarını, kaldırılmış efektör SEND/RETURN/Monitor kontrollerini, ON AIR ile mikrofon kapısının senkronunu ve yeniden açılışta kapalı olmasını denetler.

7 Ekim 2026 kabulünde `audio`, `audio_decoder`, `dsp`, `transition`, `runner`, `output`, `playout`, `audio_recovery`, `audio_routing`, `desktop_api`, `native_controls`, `native_audio` ve `native_dj` hedeflerinin **13/13**'ü geçti (144,79 saniye). Bu koşu fiziksel mikrofon veya çıkış kullanmaz. Önceki sanitizer sonuçları tarihsel kapsamdır; bu ek yönlendirme için sanitizer kabulü iddia edilmez.

İsteğe bağlı görsel kontrol yalnız özel fixture'ın kendi mixer widget'ını çizer:

```sh
RADIOS_SHOT_DIR="$PWD/RadioS/build/dj-review" python3 RadioS/tests/run_dj_ui.py RadioS/build/test_dj_ui -p /native-dj/channel-routing-squares-ON-AIR
```

## Aşama 10 playlist ve otomasyon kabulü

GUI açıkken **26 CTest hedefi** tanımlıdır. `playout`, `runner`, `desktop_playout` ve `native_playout` eklendi. `test_playout.c`, doğrulanmış kimlik/kare/yol, UTC/gece yarısı/gün seçimi, ağırlık/döngü/tamamlama, deterministik batch devamı, gerçek overlap/cue tekrar aralığı, sınır lookahead, jingle fazı ve kalıcı checkpoint'i sınar. `test_runner.c`, gerçek PCM geçişi ve sourcegain, transport/saat/deadline/sahiplik, decoder hatası, kümülatif tüketim alındıları ve immutable queue append'i kapsar.

`test_desktop_playout.py`, ayrı SDBASE'te salt okunur hazırlığın SHA/istasyon/UUID, bağlı ve bağlanmamış kayıt, izin, sınır ve legacy uyumunu; geçmiş/rotasyon exact typed schema, gerçek CAS/alındı/replay ve yeniden açılışı denetler. `test_playout_ui.c` ve `run_playout_ui.py`, native playlist→kalıcı program→MAIN iki dosyalı geçişi, pause/resume/next/previous ve manuel deck korunmasını, kaydedilmemiş önizlemenin geçmişi değiştirmemesini, 1-müzik-partili otomasyonun gerçek A→jingle→B append/cursor/geçmişini, yeniden açılışta kapalı oynatmayı, gerçek gelecek UTC başlangıcını ve bekleyen Stop'u, stale SHA/unbound medya/backend kesintisi/kapanış/CAS giriş korumasını beş grupta sınar. Özel WAV'lar 288.031 kare/6.001 ms katalog süresi taşır. Fixture kapıları yalnız build altındaki backend kopyasında bulunur; üretim veritabanı kullanılmaz.

```sh
ctest --test-dir RadioS/build --output-on-failure -R '^(playout|runner|desktop_playout|native_playout)$'
```

Kullanım, kaynaklar ve 500 giriş/tek oynatıcı hazırlık boşluğu sınırı [10. aşama belgesindedir](../docs/10-playlist-ve-otomasyon.md). Native kabul gerçek Pulse sanal çıkışlarında PCM tüketimini ölçer; on-air/stream veya fiziksel donanım kabulü iddia etmez.

Son strict build ve birleşik CTest **26/26**, **226,46 saniye** başarılı. Planlayıcı **17/17**; runner **15 deterministik + 1 gerçek Pulse**; backend **13/13**, mevcut typed API **18/18** ve native **5/5** geçti. Core ASan/UBSan/leak açık, native ASan/UBSan **5/5** (**64,47 saniye**, leak kapalı) temizdir. Son kaynaklardan yeniden derlenen otomasyon sanitizer grubu ayrıca geçti (**23,38 saniye**). A→jingle→eklenen B ölçümünün en uzun boşluğu normalde 1 kare; ilk tam sanitizer kabulünde 624 kare/13 ms, son focused kabulde 1 karedir. UTC başlangıcından önce MAIN sessizliği ve PREVIOUS A2/B1 kalıcı gerçek çalma kayıtları kabul edildi.

### AutoDJ otomasyonu: tasarlanmış geçişler

`transition` artık eğri değerlendirme/doğrulama ve sekiz şablonu, bir tarafın
gerçek frekans yanıtını (bas rafı, alçak/yüksek geçiren filtre, çıkan tarafta
yankı kuyruğu, giren tarafın kuru kalması) ve şekilli gerçek önizleme PCM'sini
sınar. `runner`, tasarlanmış zarfın bindirmede eşit güç/ducking yerine geçtiğini
ve bindirmesiz sınırda reddedildiğini; `automation_calendar_plan` sıralı/rastgele
rotasyonu, cuma zaman diliminin önceliğini ve geçersiz ön ayar/atama reddini;
`playout` şekil ipucunun runner öğesine, görünüme ve tek oynatıcıda düşmesine
bağlanmasını; `rotation_editor_regressions` sayfanın gerçek GTK kaydını;
`desktop_automation_calendar` arka uç kalıcılığını ve ret listesini denetler.

## Aşama 11 reklam kabulü

GUI açıkken **28 CTest hedefi** tanımlıdır; `desktop_advert` ve `native_advert` eklendi. Backend'in 22 grubu gerçek reklam dosyası/SHA, ayrı spot/kampanya onayı ve açık kullanım izni; kalıcı trafik yükümlülüğü, kapasite/ölçülmüş ek ses/aşım/üç boş kuşak politikası; atomik rezervasyon, iptal/yeniden yerleştirme geçmişi, CAS/alındı/yetki iptali; tam kaynak kareleri, append-only terminal olaylar ve teslim kanıtı ayrımı; filtreli değişmez rapor, CSV güvenliği/sayfalama, bildirimin kullanıcıya ait okuma durumu ve yeniden açılışı sınar. Onaylı kampanyanın güvenli satış alanı değişirken trafik alanları ve medya/spot kimlikleri korunur.

C planlayıcı 18 grupta tam dosyanın milisaniyeye yuvarlanmadan bütün kareleriyle yürütülmesini ve açık cue sınırını da denetler. Yürütücü 21 deterministik grup ve ayrı gerçek Pulse grubuyla tamamlama/kesilme/hata terminalini; gelecekteki UTC kapısını, ardıl parça başlamadan bitirme, tam 240 kare fade, sınırlı bekleme, iptal/manual takeover, pause ve eski programın erken deadline'ını sınar. `handover_completed`, manuel FINISHED/IDLE ile gerçek kuşak devrini ayırır.

Native reklam kabulünün beş grubu: gerçek kampanya/medya/izin/onay ve güvenli satış düzenlemesi; ölçülmüş 750 ms açılış/kapanış, kapasite, gerçek CAS, rezervasyon/iptal ve boş kuşakta gerçek PCM; exact MAIN kareleri, CUE sessizliği ve değişmez CSV/rapor; pause/resume, kısmi Stop ve hem API hazırlığı hem controller bekleyişi sırasında iptal edilen gelecekteki kuşakta eski MAIN'in sürmesi; bağlanmamış/değişen medya, backend gecikmesi/hatası, CAS giriş koruma, kapanış ve yeniden açılış. Son grup gerçek C decoder hatasında önceki ERROR kuyruğunun bekleyen reklamı kilitlememesini; ayrıca başarılı run-start yanıtından sonra değiştirilen gerçek dosyanın preroll hatasının sıfır kareli `failed` olarak kaydedilmesini doğrular. Başlangıç/tamamlama olayı uydurulmaz.

`run_advert_ui.py`, kapıları yalnız özel backend kopyasına ekler ve kendi sanal Pulse cihazlarını temizler. Ortak koşucunun varsayılan 100 saniye sınırı diğer hedeflerde korunur; bu kapsam için uygulama sınırı 240 saniye, CTest sınırı 300 saniyedir. Gerçek import sonrası kısa GTK settling, standart servis rate limitini değiştirmeden yoğun kabul akışını dengeler; belirsiz mutation kör tekrar edilmez.

```sh
ctest --test-dir RadioS/build --output-on-failure -R '^(desktop_advert|native_advert|runner|playout)$'
python3 RadioS/tests/run_audio_ui.py --engine-only RadioS/build/test_runner
```

Yerel kaynak tüketimi uzak yayın/sözleşme teslimi değildir; `delivery_proven: false` ve kanonik kabul sayaçlarının korunması test edilir. Uygulama çökmesi/kapanışı sırasında tamamlanmamış son yazının dayanıklı kurtarılması aşama 13 kapsamındadır; terminali alınmayan başlangıç `unknown` kalır. Kullanım ve sınırlar [reklam yönetimi belgesindedir](../docs/11-reklam-yonetimi.md).

Son strict C11 ve birleşik CTest **28/28**, **317,42 saniye** başarılı. Backend reklam **22/22**, native reklam **5/5**, **79,77 saniye**; native ASan/UBSan **5/5**, **122,27 saniye** test uygulaması süresiyle temizdir (GTK leak kapalı). Planlayıcı **18/18** ve yürütücü **21 deterministik + 1 gerçek Pulse** ASan/UBSan + leak açıkken başarılıdır. Normal gerçek Pulse yürütücü grubu ayrıca geçti. Gerçek servis/0.11.0 uygulaması açıldı; üretim kimlikleri, sequence 2, bütünlük ve koleksiyonlar korundu. Reklam overview/prepare HTTP 200 verdi; fixture cihazları temizdir.

## Aşama 12 yayın, kayıt ve acil kabulü

GUI açıkken 31 CTest hedefi tanımlıdır; `output`, `desktop_broadcast` ve `native_broadcast` eklendi. Core sekiz grupta son MAIN PCM/DSP/kapısı ve CUE ayrımı; WAV örnekleri, exclusive create ve sonlandırma; gerçek MP3/raw PUT çözümlemesi, erken HTTP kabulü, bağımsız metadata başarısı/ret cevabı, kuyruk taşması ve beş saniye kaynak saati kaybını sınar. Backend'in 15 grubu typed profil/revision, yetki ve kullanıcı sahipliği, read-only taze SHA/istasyon kontrolü, değişmez manifest/append-only terminal, idempotency, bozuk/kapalı kaynak, HTTP metot ve byte bütçesi doğrulamasını kapsar.

Native dört grup: gerçek WAV kayıt/yeniden açılış/mevcut dosya korunması ve durable sonuç; auth isteyen loopback mount'a gerçek MP3, UTF-8 metadata ve şifrenin DB dışında kalması; native acil kütüphane picker/CUE/pending iptal/devralma/dönüş; bütün öğelerin gerçek COMPLETE makbuzu ile doğal tamamlanma, Next'te gerçek INTERRUPTED, manuel kaynak değişiminden sonra gecikmiş komutun reddi ve crossfade'de gelen öğenin tüketilmiş konumundan dönüş.

`run_broadcast_ui.py` özel backend/DB kopyası ve kendi Pulse çıkışlarını kullanır. MP3 fixture'ı Icecast kaynak protokolü için chunk header'sız ham PUT gövdesi alır, canlı erken HTTP 200 verir; şifre ve başlık yalnız loopback fixture'da denetlenir. Response hold kapısı yalnız test kopyasında bulunur. Üretim veri ve fiziksel çıkışlara test yayını yapılmaz.

```sh
ctest --test-dir RadioS/build --output-on-failure -R '^(output|desktop_broadcast|native_broadcast)$'
```

Gerçek Icecast 2.4.4 sunucusu da yalnız loopback üzerinde build altında derlenerek sınandı: kaynak ve metadata HTTP 200; bağımsız dinleyiciden 42.000 MP3 baytı, 125.568 çözülmüş kare ve 0,140 RMS elde edildi. Sunucu durduruldu; sistem kurulumu yapılmadı. Kanıtlar `build/stage12-icecast/real-proof.txt` ve `real-proof.json` içindedir. Yerel PCM, sunucu kabulü ve uzak/akustik teslim kanıtı ayrı kalır.

Kullanım ve aşama 13/14 sınırları [yayın, kayıt ve acil akış](../docs/12-yayin-kayit-acil.md) belgesindedir.

Strict C11 ve birleşik CTest **31/31**, **204,50 saniye** başarılı. Yayın core **8/8**, typed backend **15/15**, native yayın/acil **4/4** başarılıdır. Native ASan/UBSan **4/4**, **79,78 saniye** temizdir (GTK leak kapalı); WAV/stream core **8/8** leak denetimi açıkken temizdir. C yürütücü/planlayıcı/output sanitizer hedefleri ayrıca **3/3** geçti. Yeni genel belge read-order testi normal ve ASan/UBSan altında geçti; korumaları kaldıran iki özel negatif kontrol beklenen gerçek hatayı gösterdi. CUE ve otomasyon regresyonları son kaynaklarla başarılıdır. Bu aşamanın API envanteri **206 workflow / 256 yol / 261 HTTP işlemi / 171 toplam kaynak dosyası (72 tasarım + 99 Python modülü)**, kaynak CSS **30 dosya / 1.552 native kural** korunur.

## Aşama 13 yönetim ve güvenilirlik kabulü

GUI açıkken **36 CTest hedefi** tanımlıdır. `recovery`, `desktop_admin`, `reliability`, `native_admin` ve `audio_recovery` eklendi. Kabul fixture'ları build altındaki ayrı DB/medya/anahtar dizinlerini kullanır; üretim veritabanına kayıt yazmaz.

`test_recovery.c` 10 grupta ayrı süreçten dayanıklı exact gövde kurtarmasını, 0600 dosya/0700 dizin, secret/identity/symlink/hardlink ve hasarlı JSON reddini, concurrent prepare, GUI/restore süreç ve crash kilidini, reddedilmiş önceki yazının ardıl CAS'ı tutmasını ve geçerli JSON içindeki değişmiş gövdenin SHA-256 ile reddini doğrular. `test_data_client.c` 15 grupta önceki transport kabulüne commit sonrası gerçek socket yanıt kaybı/yeniden açılış, engellenmiş işlemin tekrar edilmemesi, yabancı kullanıcı reddi, başka kullanıcı ortam anahtarının ayrı kuyruğu ve gerçek HTTP 409'dan sonraki yazının gönderilmemesini ekler.

`test_desktop_admin.py` 26 grupta kullanıcı/izin düzenleme ve canlı yetki kaldırmayı, kendi/son yönetici korumasını, legacy anahtar iptal yolunun aynı korumaları uygulamasını, istasyon/typed alan/CAS/alındı sınırlarını, gerçek izlemeyi, çalışma kesintisi uzlaştırmasını ve restore fence'i sınar. İstemcinin ilk UUID'sini revision 0 ile kaydetme, exact replay ve revision 1 ardılı ile aynı UUID'yi yarışan yabancı claim'den koruma da kapsanır. Mevcut desktop backend testlerinin birleşik kapsamı **108 gruptur**.

`test_reliability.py` 18 grupta gerçek PCM medya içeren tutarlı snapshot ve concurrent writer, hash/yol/symlink/kurulum/port/lock korumaları, restore planı/staged kopya/offline uygulama, crash/rollback ve yabancı düzenleme koruması, kullanıcı outbox karantinası, restored DB'nin gerçek HTTP ile yeniden açılışı ve sahipsiz medya karantinasını sınar. Orijinal veri ve byte'lar kaybolmadan korunur.

`test_admin_ui.c` altı native grupta gerçek user CAS/anahtar iptali, kesinti uzlaştırma, yedek/plan/stage, owner kapanışı, kurtarma özeti ve salt görüntüleme yetkisiyle yerel yedek yardımcısının başlatılamamasını denetler. Yetki her yardımcı çağrısından önce gerçek backend'de doğrulanır. `native_broadcast` beşinci grubu ACTIVE alındısını gerçekten bekletip MAIN tüketimi sürerken pencereyi kapatır; ilk işlem ve terminal/history kuyruğunu yeni pencerede kurtarır, son ölçülmüş kareleri ve `interrupted` sonucunu doğrular. Yeni pencerede oynatma başlamaz.

`test_audio_recovery.c` / `run_audio_recovery.py`, ayrı gerçek Pulse daemon'unu üç kez kesip açar. Son doğrulanmış PCM konumu ve runner makbuzları, mikrofonun kapanması, yeniden bağlantı sonrası sessizlik, mevcut olmayan açık aygıtta fallback reddi ve operatör resume komutundan sonra gerçek tüketimin devamı sınanır. Host ses sunucusu durdurulmaz. Daemon yoksa CTest 77 ile bu hedefi atlar; mevcut kabul ortamında özel daemon hazırlanmış ve test gerçekten çalıştırılmıştır. Ortama özgü özel daemon ve resmi paket/SHA kaynakları `build/stage13-pulse/provenance.json` içindedir.

```sh
ctest --test-dir RadioS/build --output-on-failure -R '^(recovery|data_client|desktop_admin|reliability|native_admin|audio_recovery|native_broadcast)$'
```

ASan/UBSan kabulünde core için leak detection açık, GTK süreç global kaynakları için native hedeflerde kapalıdır. Fiziksel ses, uzun yayın süresi ve paket/temiz kurulum kabulü aşama 14'tür. [Kullanım, güvenilirlik sözleşmeleri ve sınırlar](../docs/13-yonetim-ve-guvenilirlik.md).

Son birleşik normal kabul **36/36**, **242,54 saniye** başarılıdır. Son UI açıklamasıyla native yönetim **6/6** normalde **9,87 saniye** ve ASan/UBSan altında geçti. Core kuyruk/istemci leak açık sanitizer **2/2 hedef** (**5,58 saniye**), native yayın/kapanış kurtarması **5/5** (**96,43 saniye**, GTK leak kapalı), üç private Pulse restart'ı normal **1/1** (**18,50 saniye**) ve leak açık sanitizer **1/1** (**18,35 saniye**) başarılıdır. Gerçek kurulum/veritabanı korunarak 0.13.0 uygulaması açıldı ve kurulum yedeği doğrulandı; testlerde üretim fixture'ı yoktur.

## Çoklu arama ve bağımsız oynatıcı sıraları

`online_services` gerçek özel HTTP sunucusunda Spotify/Apple Music müzik ve
playlist sorgularını, query encoding, kanonik URL, null kayıt süzme ve Apple
katalog JWT / kişisel kullanıcı belirteci ayrımını denetler. `youtube_music_bridge`
ayrı şarkı/playlist filtrelerini ve güvenli playlist kimliklerini sınar.
`native_broadcast_online` aynı native tablodaki çoklu kaynak birleşmesini,
sağ kaynak özetini, kayıtlı Apple fallback etiketini, playlist açılışını ve
aramada ses başlamamasını doğrular. Canlı sağlayıcı hesabı bu fixture'lar değildir.
Oklu **Arama kaynağı** menüsünün dört seçeneği, erişilebilir adı, servise/kapsama
göre değişen arama yer tutucusu ve kaynak değişiminde sorgunun korunması da sınanır.

`desktop_api` ayrı oynatıcı belgelerinde doğrulama/CAS/reopen ve eski program
uyumluluğunu; `native_broadcast_catalogue` gerçek DB/GTK sürüklemesiyle ayrı
sıraları, bağımsız geçiş kaydını, yeniden açmayı ve oynatıcı değişiminden sonra
gecikmiş playlist bırakmanın iki sırayı da değiştirmemesini sınar.
`runner` iki gerçek WAV'ın toplam PCM'sini ve bağımsız pause/next/stop,
deck sahipliği, epoch ve tüketim makbuzlarını ölçer. `native_playout` özel MAIN
Pulse yakalamasında iki seçili oynatıcının 600/1700 Hz sesini ölçer; birinin
duraklatılması/durdurulması diğerinin sesini, indeksini veya epoch'unu kesmez.
Gerçek per-player geçmişleri genel geçmişten ayrıdır. Üretim sesine veya
veritabanına test fixture'ı yazılmaz.
Kart üstündeki başlık/sanatçı/albüm/kaynak ve sıradaki parça da gerçek
genel yayın/AutoDJ crossfade'inde ve iki bağımsız oynatıcıda doğrulanır.
Manuel kart vakası gerçek metadata CAS kaydını, yükleme nesli değişince eski
bilginin temizlenmesini, reddedilmiş yüklemede mevcut içeriğin korunmasını
ve boşaltmayı sınar. Çevrimiçi kartın parça değişiminde kendi bilgilerini
güncellemesi ve diğer kartı değiştirmemesi sağlayıcı fixture'ıyla denetlenir.
Native catalogue vakaları ayrı servis/DB fixture'larında çalışır; önceki
vakanın servis kotasını veya program sırasını miras almaz. Aynı vaka içindeki
yeniden açma ve CAS denemeleri aynı DB'yi kullanır.

2026-10-08 SYNC öncesi birleşik kabul: strict derleme başarılı, **66/66 CTest hedefi**
**198,40 saniyede** geçti. Sonuç dosyası `build/final-player-queues.xml`.
Canlı Spotify/Apple hesaplarıyla katalog kabulü bu özel HTTP fixture'larının
yerine geçmez; bağımsız çevrimiçi ses desteği iddia edilmez.

```sh
ctest --test-dir build --output-on-failure -R '^(online_services|youtube_music_bridge|desktop_api|runner|native_broadcast_online|native_broadcast_catalogue|native_playout)$'
```

### Canlı oynatıcı SYNC / beatgrid kabulü

`rhythm`, bilinen 80–180 BPM pulse PCM'lerinde BPM/ilk beat/güven ve ritimsiz
kaynak sınırlarını denetler. `tempo`, gerçek C decoder ve `atempo` ile
0,84/0,90/1,10/1,16 hızlarında çıktı süresini ve 997 Hz tonunun korunmasını,
iptali, SYNC fazını, tempo ayarlı kaynak saatini, güvenli kaynak devralmayı,
0→2 benzeri seçili deck havuzunu, beatte crossfade'i, deck geri dönüşünü ve
doğal tamamlanmada tam kaynak frame'li tüketim makbuzlarını sınar.

`desktop_media` türetilmiş grid CAS/replay/reopen/yetki/yanlış SHA denetimini
ve başlangıç analizinin değişmezliğini; `desktop_api` player seçimlerinin
kalıcılığını, döngü ve kendine atama reddini doğrular. `playout`, donmuş grid,
±%16 tempo ve seçilen çift planını test eder. `native_playout` gerçek özel
DB/HTTP/PulseAudio fixture'larında Analiz, SYNC, kayıt/yeniden açma, 1→3
AutoDJ, başka deckin korunması, iki karttan Pause/Resume/Stop, gerçek MAIN
PCM'inde ton korunması ve crossfade'i denetler. BPM'siz kaynak ve hazırlık
sırasında değişen hedef reddedilirken çalan kaynak/yeni hedef korunur.

```sh
ctest --test-dir build --output-on-failure -R '^(rhythm|media|audio|tempo|runner|playout|desktop_media|desktop_api|native_playout)$'
RADIOS_SHOT_DIR="$PWD/build/player-card-review" python3 tests/run_playout_ui.py build/test_playout_ui -p /native-playout/player-SYNC-analysis-persistence-chosen-pair-PCM
```

Bu kabul sabit grid ve doğrulanmış yerel PCM içindir; değişken tempolu
profesyonel beat/downbeat analizi veya bağımsız DRM sağlayıcı deckleri
iddia edilmez. `libavfilter>=8.44.100` artık derleme bağımlılığıdır.

2026-10-08 SYNC kabul kaydı: ilk genel regresyon **68/68**, **587,52 saniye**
geçti. Son metadata/transport düzenlemesi sonrası **7/7 hedefli CTest**
**143,43 saniyede** geçti; `native_playout` içindeki on ayrı native vaka
buna dahildir. Sonuç `build/final-player-sync-metadata.xml` dosyasındadır.
Medya/DB/plan/tempo birleşik hedefli denetimi ayrıca **8/8** geçti.

Daha geniş son koşuda **15/16** başarılıdır: `native_dj` içindeki ON AIR
mikrofon assertion'ı toplu koşuda iki kez başarısız oldu. Aynı
`/native-dj/channel-routing-squares-ON-AIR` vakası kendi fixture'ında
**6,40 saniyede** geçti. Bu nedenle son bütün DJ grubunun her koşuda
başarılı olduğu iddia edilmez; SYNC dışında bu toplu-koşu kararsızlığı
bu değişiklikte düzeltilmedi.

### MASTER kanal SEND/RETURN

MASTER şeridine kanal seçicileri ve ayrı SEND/RETURN seviyeleri eklendi.
`test_dsp.c` artık **20**, `test_dj_ui.c` **7** grup içerir. Yeni native
vaka gerçek MAIN PCM'sinde gönderim kazancını, aynı kanaldan çift saymadan
dönüşü, RETURN seviyesini, MASTER mute'u, DB kaydı/yeniden açılışı ve hedef
kanal silinince bağlantı/seçenek temizliğini doğrular. DSP kabulü hedefin
devamındaki kanalları ve hedefin kendi kaynağını tekrar gönderime almadan
işler; yüksek numaralı kaynakları, harici müziği, mikrofonu, MASTER sampler'ı,
tek ducking aşamasını ve geçersiz MASTER ayarlarının atomik reddini denetler.
Eski MASTER kayıtlarının bağlantısız açılması korunur. Veri servisi yeni
isteğe bağlı alanları tip/aralık, CAS ve yeniden açılış bütünlüğüyle doğrular.

2026-10-08 MASTER kabulünde `dsp`, `desktop_api`, `native_dj` **3/3** hedef
**46,31 saniyede** geçti; bu koşuda mevcut ON AIR vakası da başarılıdır.
Bu, önceki koşunun kararsızlığına yönelik bir düzeltme iddiası değildir.
`audio`, `transition`, `runner`, `audio_routing`, `native_audio`,
`native_window` ayrıca **6/6** geçti (**54,63 saniye**).

```sh
ctest --test-dir build --output-on-failure -R '^(dsp|desktop_api|native_dj)$'
RADIOS_SHOT_DIR="$PWD/build/master-routing-review" python3 tests/run_dj_ui.py build/test_dj_ui -p /native-dj/master-channel-routing-PCM-persistence-cleanup
```
