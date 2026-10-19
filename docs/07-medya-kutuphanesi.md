# 7. aşama — Gerçek medya kütüphanesi

RadioS 0.7.0, native C/GTK arayüzünden yerel ses dosyalarını içe aktarır, FFmpeg kütüphaneleriyle tamamen çözümler ve gerçek ölçümleri SDBASE'e kaydeder. Kütüphane araması, metadata düzenleme, ölçülen waveform ve medya bağlı cue taslakları çalışır. Teknik olarak doğrulanmış dosya, yayın onayı anlamına gelmez: içerik ve sürüm `draft`, istasyonun yayın uygunluğu kapalı kalır. Gerçek oynatma ve cihaz çıkışları aşama 8'dir.

## Kullanım

1. RadioS'u aç ve veri bağlantısının kurulmasını bekle. Yayın görünümündeki **Medya kütüphanesi** düğmesini aç.
2. **+ Ses dosyaları** ile bir veya birden fazla yerel ses dosyası seç; içerik türünü belirt. Dosyalar sırayla kopyalanır, çözümlenir ve kaydedilir. Sonuç alanı başarılı ve başarısız aktarım sayılarını gösterir. **İçe aktarmayı iptal et**, sıradaki dosyaları durdurur; tamamlanmış kayıtlar korunur.
3. Kütüphanede başlık, sanatçı veya etiket ara. **Önceki / Sonraki** düğmeleri gerçek kayıt sayfalarını yükler. Yayın görünümündeki katalog ayrıca içerik türü ve başlık/sanatçı/albüm alanıyla arama yapar; seçilen gerçek kaydı kütüphanede açar.
4. Bir kayıt seç; başlık, tür, sanatçı, albüm, tarz, virgülle ayrılan etiketler, yıl ve notu **Etiketleri kaydet** ile kaydet. Yıl boş veya dört rakamdır; etiketler benzersiz, en fazla 32 adettir. Başlık/tür/sanatçı değişimi taslak katalog kimliğini ve yönetilen sanatçı katkısını günceller; albüm, tarz, etiket, yıl ve not istasyona bağlı metadata'dır.
5. **Dalga biçimi / Cue** düğmesi gerçek ölçümle parça işaret düzenleyicisini açar. Süre salt okunurdur. Cue-in, intro, out, fade başlangıcı/sonu, otomatik sonraki, bindirme ve otomatik geçiş tercihi düzenlenebilir. Ekran saniye gösterir; veri sözleşmesi milisaniye kullanır. **Medya işaretlerini kaydet** gerçek medya kimliğine bağlı işaret taslağını kaydeder.

Waveform dosyanın gerçek örneklerinden ölçülür; bu aşamada ses ön dinleme veya PCM düzenleme yapmaz. İşaretler ölçülen sürenin dışına taşamaz. Kayıt başka bir işlem tarafından değişmişse 409 çakışması gösterilir; yerel girdiler korunur, güncel veriyi yükleme kullanıcı seçimiyle yapılır. Formda kaydedilmemiş alan varken arka plan yenilemesi bu alanları ezmez.

## C analizinin sınırları ve bağımlılıklar

`src/media/analyzer.c`, libavformat/libavcodec/libavutil/libswresample API'lerini doğrudan C içinden kullanır. Normal uygulama harici `ffmpeg` süreci çalıştırmaz. Geliştirme paketlerinin minimum sürümleri CMake'de denetlenir:

| Paket | Minimum API sürümü | Bu geliştirme ortamındaki sürüm |
|---|---:|---:|
| `libavformat` | 59.27.100 | 61.7.100 |
| `libavcodec` | 59.37.100 | 61.19.101 |
| `libavutil` | 57.28.100 | 59.39.100 |
| `libswresample` | 4.7.100 | 5.3.100 |
| `libavfilter` (tonu koruyan SYNC/tempo DSP) | 8.44.100 | 10.4.100 |

Minimum kararlı FFmpeg sürümü 5.1'dir. Kullanılan `AVFrame.ch_layout` ve `swr_alloc_set_opts2` API'leri 5.1'in resmi [frame.h](https://raw.githubusercontent.com/FFmpeg/FFmpeg/n5.1/libavutil/frame.h) ve [swresample.h](https://raw.githubusercontent.com/FFmpeg/FFmpeg/n5.1/libswresample/swresample.h) başlıklarıyla karşılaştırıldı. Bu başlık/API uyumluluk incelemesidir; FFmpeg 5.1 ortamında ayrıca çalışma testi yapılmadı. Çalışma ve sanitizer kabulü tablodaki kurulu kütüphanelerle yapıldı.

GTK, GLib/GIO, Fontconfig, JSON-GLib ve libcurl bağımlılıkları sürer. Format kabulü için testlerde kullanılan `ffmpeg` komutu ve MP3/FLAC/Vorbis/AAC encoder'ları da geliştirme ortamında bulunmalıdır; bunlar fixture üretir.

### RadioS BPM / beatgrid analizi

Yeni yerel içe aktarmalar gerçek çözümlenen PCM üzerinde `radios-onset-grid-v1`
analizini de çalıştırır. İlk 120 saniyenin enerji yükselişleri, periyot eşleşmesi
ve ağırlıklı beat regresyonu tek bir sabit BPM, ilk beat konumu ve güven skoru
üretir. Yeterli ritim bulunamazsa BPM sıfırdır; uydurma varsayılan tempo yazılmaz.
Bu RadioS formatıdır; rekordbox'ın özel analiz algoritması veya değişken
tempolu/dinamik beatgrid, downbeat ve phrase analizi değildir.

Canlı oynatıcıdaki **Analiz**, yüklü doğrulanmış medya ve yerel playlist
öğelerini arka planda sırayla tekrar analiz eder. Dosya SHA'sı ve medya/sürüm
kimliği yeniden denetlenir. `desktop-media/analysis-save`, medya ve analiz
revision'larını CAS ile doğrular; sonucu istasyon kapsamındaki
`desktop_media_rhythm` kaydına ve işlem makbuzuna atomik yazar. Başlangıçtaki
`desktop_media_analysis`, onaylı sürüm, teknik kanıt ve ses dosyası değişmez.
Tekrar açılınca grid bu kaynaktan okunur. Eski veritabanları yeniden içe
aktarma gerektirmez. Kart kapanırsa iş iptal edilir; başka yükleme nesline ait
gecikmiş sonuç yeni parçanın gridini değiştirmez.

Yerel format kabul matrisi WAV/PCM, MP3, FLAC, Ogg/Vorbis, M4A/AAC ve ham AAC'dir. Dosya uzantısı doğrulama sayılmaz; içerik açılır ve seçilen ses akışı sonuna kadar çözümlenir. Harici protokoller ve başka dosyaya yönelen medya referansları kapalıdır. Decoder'ın saptadığı bozulma ile WAV/FLAC/Ogg/M4A bütünlük ve kesilme hataları, sonlu olmayan örnekler, normal dosya olmayan kaynaklar, bağlantı kaçışları ve analiz sırasında değişen kaynak reddedilir. Toplam örnek sayısı taşımayan MP3/ham ADTS AAC'de tam paket sınırında kısaltılmış dosyanın orijinalden eksildiği her durumda ispatlanamaz; bu formatlarda bütün kesilmelerin yakalandığı iddia edilmez.

Dosya sınırı 2 GiB, çözülen süre sınırı 24 saat, örnekleme hızı en fazla 384 kHz ve kanal sayısı en fazla 32'dir. Analiz, süre, sample frame sayısı, codec/container, bitrate, örnekleme hızı, kanal sayısı, peak, RMS ve clipping sayısını kaydeder. Waveform, tüm kanalların birlikte min/max örneklerini en fazla 2.048 zaman diliminde tutar; son dilim kısa olabilir. Float sesin genliği tam ölçeğin üstüne çıkabilir; gerçek ölçüm korunur, çizim görünür aralığa sınırlandırılabilir. Peak/RMS örnek genliğidir; LUFS veya true peak ölçümü olarak sunulmaz.

## Dosya ve transaction akışı

Kaynak dosya, yapılandırılmış `data/imports/` altında UUID adlı özel bir kopyaya alınır. Import dizini proje içindedir; kopya `0600` izinle yazılır ve fsync edilir. C analizinden önce ve sonra SHA-256, dosya kimliği ve boyut kontrol edilir. Backend dış kaynak yolunu almaz; yalnız bu dizindeki güvenli dosya adı ve orijinal dosyanın basename'i gönderilir.

Backend, yönetilen import dizinini yerel servis yapılandırmasından alır. İstemci bu kökü seçemez. `O_NOFOLLOW` ile açılan normal dosya descriptor'ı sabitlenir; mutlak yol, `..`, alt dizin, symlink veya FIFO içeri alınmaz. Mevcut medya stager'ı baytları tekrar SHA-256 hesaplayarak `<veritabanı>.objects/<sha-prefix>/<sha256>` içine kalıcılaştırır. Aynı içerik yönetilen objeyi yeniden kullanır. Analizin SHA ve boyutu bu kopyayla eşleşmeli; yayınlamadan önce yönetilen dosya ayrıca doğrulanmalıdır.

Gerçek `items`, `versions`, `station_items`, `people`/ilk sanatçı katkısı, `media_files`, `media_locations`, native istasyon metadata'sı, teknik analiz ve değişmez işlem alındısı tek DB transaction içinde yayınlanır. Import, istasyonun hem `edit` hem `approve` yetkisini ister. `approve` burada gerçek dosyanın teknik inceleme yetkisidir; içerik sürümü onaylanmaz, dağıtım, oynatma emri veya on-air durumu oluşturulmaz.

Native ek koleksiyonlar:

| Koleksiyon | Kalıcı veri |
|---|---|
| `desktop_media_library` | Gerçek item/version/media UUID'lerine bağlı istasyon metadata'sı ve revision |
| `desktop_media_analysis` | Gerçek SHA/boyut ve teknik metadata ile eşleşen değişmez decoder ölçümü |
| `desktop_media_markers` | Gerçek medya UUID/SHA/süreye bağlı, revision kontrollü işaret taslağı |
| `desktop_mutations` | Kullanıcı/istasyon/işlem UUID'si ve istek özetiyle değişmez sonuç alındısı |

Metadata kaydı ve medya işaretleri revision/CAS kullanır. İşaret yazısı hem kütüphane hem işaret revision'ını aynı transaction içinde denetler. Başlık/tür/sanatçı düzeltmesi paylaşılan item'ın bütün istasyonlarında `edit` ister; item'ın herhangi bir onaylı/withdrawn sürümü varsa ayrı katalog sürüm iş akışı gerekir. Sanatçı düzeltmesi native import'un sahip olduğu `primary_artist` katkısını aynı transaction içinde değiştirir; sanatçı boşaltılırsa yalnız bu yönetilen bağlantı kaldırılır. İlgisiz katalog katkıları korunur, kaldırılan bağlantı audit/tombstone geçmişinde kalır. Böylece genel katalog sanatçı araması ve native metadata aynı güncel kimliği gösterir.

İstasyon aidiyeti ve izinler her işlemde doğrulanır; endpoint kimliği kütüphaneyi okuyamaz. Yanıtlar erişim anahtarı içermez. Ayrıntı okuması, yönetilen dosyanın SHA/boyutunu tekrar doğrular; kayıp, bozuk veya symlink objeye okunabilir yol döndürmez. Eksik dosya tarihsel ölçümü değiştirmez.

Cue taslakları, mevcut `radio_cues` onayı ve değişmez `playback_marker_sets` sözleşmelerinden ayrıdır. Mevcut onaylı marker işlemleri onaylanmış içerik sürümü ister. Aşama 7 işaret kaydı bu onayı taklit etmez ve onaylı marker set oluşturmaz.

## Kimlikli API sözleşmesi

Tüm eylemler `POST /v1/workflows/desktop-media/<eylem>` altında allowlist ile yönlendirilir. Ham koleksiyon yazma API'si açılmaz.

| Eylem | Girdi / sonuç |
|---|---|
| `import` | `station_id`, import dosya adı, orijinal basename, `idempotency_key`, typed metadata ve tam teknik analiz → gerçek kayıt ayrıntısı |
| `list` | İstasyon, sorgu, tür, isteğe bağlı arama alanı, limit/offset → kayıtlar, toplam, sequence ve `next_offset` |
| `get` | İstasyon ve kütüphane kayıt UUID'si → metadata, version, tam waveform analizi ve işaret taslağı |
| `metadata-save` | Kayıt UUID'si, beklenen revision, işlem anahtarı, typed metadata → yeni ayrıntı |
| `markers-save` | Kayıt UUID'si, kütüphane/işaret beklenen revision'ları, işlem anahtarı, milisaniye alanları → yeni ayrıntı |

Liste en fazla 200 kayıt ve yaklaşık 1,5 MB JSON kayıt içeriği döndürür; waveform ve yerel obje yolu yalnız ayrıntı yanıtındadır. `next_offset` sayfa boyutu veya byte sınırından sonra gerçek devam noktasını verir. Arama alanları boş/tüm alanlar, `title`, `artist`, `album`; türler mevcut katalogdaki `music`, `advertisement`, `jingle`, `sweeper`, `news`, `program`, `announcement`, `effect`, `voice_track`, `other` değerleridir. Türkçe arama anahtarı başlıkları değiştirmez.

401/403 izin, 404 aidiyet/kayıt, 409 revision veya farklı içerikle anahtar tekrar kullanımı, 422 alan/ölçüm/yol doğrulaması ve 503 geçici servis/depo hataları ayrı değerlendirilir. Teknik analiz waveform boyutu, frame/süre uyumu, min/max sırası, finite genlikler, peak/RMS ve clipping sayaçları bakımından doğrulanır.

## Kesinti, iptal ve tekrar deneme

İşlem alındısı DB'de kalıcıdır: aynı kullanıcı/istasyon ve işlem anahtarı aynı gövdeyle tekrar gönderildiğinde önceki kayıt UUID'leri döner. Yanıt kaybolmuş olsa da ikinci katalog kaydı oluşturulmaz. Aynı anahtarın başka içerikle kullanılması 409'dur. Obje kopyası ile DB yayını ayrı dosya işlemleridir; DB transaction'ı başarısızsa objenin tek başına kalması mümkündür. Bu obje sonraki aktarımda yeniden kullanılabilir; medya uzlaştırma aracı sahipsiz objeyi raporlar, kendiliğinden silmez.

C import yöneticisi sonucu belirsiz isteğin değişmez gövdesini ve aynı işlem anahtarını ana pencere açıkken korur. Aynı dosyayı aynı tür/başlıkla tekrar seçmek bu isteği tekrar gönderir. Önceki sonuç çözülmeden tür/başlık değiştirilerek yeni import başlatılmaz. Aynı kaynağın eşzamanlı iki aktarımı engellenir; yeni aktif ve belirsiz işlemler toplamı en fazla 64'tür. Arka planda kör POST tekrarı yapılmaz.

Gönderim öncesi iptal veya kesin başarısızlık kendi özel kopyasını temizler. Gönderimden sonra sonuç belirsizse kopya korunur; kayıt serviste tamamlanmış olabilir. Kapanmış pencereye callback teslim edilmez; iptal, tamamlanmış transaction'ı geri almaz.

**Uygulama çökmesi / tamamen yeniden açılış sınırı:** C'deki bekleyen istek gövdesi ve anahtarı henüz yalnız bellektedir. Ana uygulama kapandıktan sonra kalan import kopyasından otomatik ve aynı anahtarla devam sözleşmesi yoktur. Yeniden açılan kütüphane, tamamlanmış gerçek kayıtları DB'den okur; çözülmemiş aktarım kuyruğunun kalıcı günlüğü ve güvenli sahipsiz kopya temizliği aşama 13 kapsamındadır. Bu sürüm bunları kalıcı çökme kurtarması olarak sunmaz.

## Kütüphane analizi: önce içe aktarma, sonra paralel analiz (Ayarlar › Kütüphane analizi)

Arka plandaki kütüphane bakımı (eksik/eski BPM ve ritim ızgarası, ses şiddeti, otomatik AutoDJ IN/OUT noktaları; elle konan hiçbir şey değişmez) artık **birden çok parçayı aynı anda** işler ve ayarları **Ayarlar › Kütüphane analizi** sayfasındadır. Sıra şöyledir: şarkılar önce kütüphaneye alınır; her içe aktarma grubu bekleme süresini yeniden başlatır, kütüphane o süre boyunca sessiz kalınca analiz toplu ve paralel çalışır. Böylece içe aktarma, taramanın araya girmesiyle yavaşlamaz ve yazma çakışması yaşamaz.

| Ayar | Aralık | Varsayılan | Anlamı |
|---|---:|---:|---|
| Aynı anda analiz edilecek şarkı | 1–16 | çekirdek sayısının yarısı, en çok 4 | Tarama ve **IN / OUT noktalarını ata** taraması bu kadar parçayı aynı anda işler |
| Kendiliğinden tara | açık/kapalı | açık | Kapalıyken tarama yalnız **Tüm kütüphaneyi şimdi analiz et** düğmesiyle başlar |
| İçe aktarma durduktan … saniye sonra | 3–600 | 20 | Son içe aktarma grubundan sonra taramanın beklediği sessiz süre |
| Değişiklik olmasa da her … dakikada bir | 1–1440 | 10 | Değişmeyen kütüphanenin yeniden taranma aralığı |

Ayarlar iş istasyonuna aittir (CPU'ya bağlıdır): `config/library-analysis.ini` dosyasında (`[analysis] version=1, workers, auto, settle_seconds, interval_minutes`; 0600, atomik yazım, bağlantılar reddedilir) saklanır, her değişiklikte hemen yazılır ve bir tarama başlarken okunur; süren tarama başladığı ayarla biter. Dosya yoksa varsayılanlar geçerlidir, yazılmaz; bozuk/aralık dışı dosya sayfada hata olarak görünür ve tarama yine varsayılanla çalışır, sonraki değişiklik dosyayı onarır. Sayfa canlı ilerlemeyi (`işlenen / toplam parça · N paralel`), son taramanın özetini ve **Durdur** düğmesini gösterir; ilerleme AutoDJ sayfasındaki işaret taramasını da kapsar.

Paralel çalışma kuralları:
- İşçi iş parçacıkları ses motorundan düşük öncelikle (nice 10) çalışır; tek işçide iş çağıran iş parçacığında yürür, eski sıralı davranış korunur.
- Backend'de ritim kaydı (`analysis-save`) ve `markers-assign` hazırlandıkları anki veritabanı sırasına bağlıdır: başka bir işlemin araya girmesi "database changed since preview" çakışması üretir. Tarama bu çakışmada parçayı yeniden okuyup en çok 6 kez tekrar dener (aynı sürüm ve, operatör kaydında, aynı revizyonlar koşuluyla); sonunda başarısız kalan parça sonraki taramaya kalır. Gerçek bir revizyon çakışması eskisi gibi hata olarak görünür.
- Bir işçinin bağlantı hatası bütün tarama turunu durdurur; iptal, başlamamış parçaları atlar.

Dosyalar: `include/radios/library_analysis.h`, `src/core/library_analysis.c` (ayar dosyası, paralel çalıştırıcı, ilerleme sayacı), `src/ui/library_analysis_ui.c` (sayfa), `src/ui/rhythm_jobs.c` (kütüphane taraması ve arka plan bakımı), `src/ui/autodj_marker_jobs.c` (işaret taraması). Testler: `library_analysis` (ayarlar, her indeksin bir kez çalışması, gerçek eşzamanlılık, iptal, sayaç), `rhythm_upkeep` (gerçek taramayı özel servise karşı sınar; 5 şarkı 4 işçiyle ve 24 şarkı 6 işçiyle, çakışmalı yarışta hepsi işaretlenir), `native_library_analysis` (sayfa: kalıcılık, bozuk dosya, ilerleme).

## Derleme ve doğrulama

`kaynak kodlar` dizininden, gerçek ekran oturumunda:

```sh
cmake -S RadioS -B RadioS/build -DCMAKE_BUILD_TYPE=Debug -DRADIOS_GUI_TESTS=ON
cmake --build RadioS/build --parallel
ctest --test-dir RadioS/build --output-on-failure
./RadioS/run-radios.sh
```

Yalnız medya kabulünü çalıştırmak için:

```sh
ctest --test-dir RadioS/build --output-on-failure -R '^(media|desktop_media|native_media)$'
```

`media`, gerçek PCM ölçümü, float clipping, yerel format matrisi, özel kopya, saptanabilir bozuk/kesilmiş kaynak, kaynak değişimi, sınırlar, confinement ve iptali sınar. `desktop_media`, gerçek PCM baytlarıyla atomik DB yayını, SHA, tekrar/kurtarma, yetki/aidiyet, CAS, sanatçı katkısı ve genel katalog arama tutarlılığı, işaret süre sınırı, gerçek HTTP ve 111 Unicode ağırlıklı kayıtla sayfalama kabulünü yapar. `native_media`, build altında ayrı gerçek serviste C import, metadata/cue kalıcılığı, yanıt kaybından aynı kayıtla devam, iptal/kapanış, gerçek dosya seçici/arama ve 409 girdi koruma kabulünü çalıştırır. Diğer native veri ve kurulum testleri korunur; fixture'lar üretim DB'sine yazılmaz.

Son kabul kaydı: strict C11 derlemesi ve CTest 15/15 başarılı (34,31 saniye). C analiz kabulü 10/10 ve ASan/UBSan 10/10, native medya 5/5 ve son kaynaklarla ASan/UBSan 5/5 (15,18 saniye), backend medya 14/14, mevcut typed API 11/11, playlist HTTP 8/8, C veri istemcisi 10/10 ve leak detection açık sanitizer kabulü 10/10, motor 19/19, sorgu/bütünlük 33/33 başarılı. GUI sanitizer koşusunda GTK süreç global verileri nedeniyle leak detection kapalıdır; core analiz/istemci koşusunda açıktır. Güncel kaynak envanteri 183 workflow, 233 HTTP yolu, 238 HTTP işlemi ve 168 backend kaynak dosyası içerir; üretme ve `--check` doğrulaması geçti.

Gerçek yerel servis son kaynaklarla yeniden başlatıldı, 0.7.0 C uygulaması açıldı. Depo, istasyon ve uç UUID'leri aynı kaldı; sequence 2 ve bütünlük `ok` doğrulandı. Üretim DB'sinde yalnız ilk kurulumun kimlik/izin/token/audit kayıtları bulunur; kabul fixture'ları veya örnek medya eklenmedi.
