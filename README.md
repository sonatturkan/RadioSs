# RadioS

RadioS, **C ile geliştirilen yerel masaüstü radyo yayın uygulamasıdır**. Proje kökü `kaynak kodlar/RadioS/` klasörüdür. C altyapısı, tasarımdaki yedi GTK 3 görünümü, yeni Ses yönlendirme sekmesi, native düzenleyiciler ve kalıcı veri bağlantısı çalışır. Gerçek medya içe aktarma, analiz, waveform, çoklu oynatıcı, ayrı ana/CUE/monitor çıkışları ve mikrofon bağlıdır. DJ mixer, EQ, efektler, padler, A/B ön dinleme, playlist/program yürütmesi ve otomatik geçişler gerçek PCM üzerinde çalışır. Reklam kampanyası/spot onayı, kuşak rezervasyonu/zamanlama ve kanıt sınıfları ayrılmış yerel çalma raporu/CSV bağlıdır. MAIN miksinden WAV kayıt, MP3/Icecast stream, metadata ve doğrulanmış acil devralma/dönüş çalışır. Kullanıcı/yetki yönetimi, izleme, dayanıklı işlem kuyruğu, ses bağlantısı toparlanması ve DB/medya yedek-kurtarması bağlıdır.

`design/reference/tasarrım/` içindeki HTML/CSS/JavaScript görsel tasarım kaynağıdır. Mevcut CSS dosyalarındaki uyumlu kurallar build sırasında GTK CSS'ye aktarılıp C uygulamasına gömülür; yerleşim native C bileşenleriyle kurulur. HTML ve JavaScript uygulamada çalıştırılmaz. Tasarımın örnek kayıtları üretim verisi kabul edilmez.

**0.17.0 kapanış denetimi:** özgün veritabanı kaynakları ve sözleşmeleri korunmuştur. Rekordbox aktarımı, gerçek program onayı/hak incelemesi, acil sıradan programa aktarım, dış HTTPS panel testi ve sinyalle temiz kapanış bağlandı. Bütün tasarım işlevleri ve saha kabulü henüz tamamlanmış değildir; [güncel denetim ve açık işler](docs/proje-kapanis-denetimi.md) önceki aşama kayıtlarının kapsamını açıklığa kavuşturur.

**0.17.1:** veri servisi bağlantısının SDK çalışma ortamındaki başlangıç hatası
düzeltildi. Yerel ses dosyaları ana pencereye veya medya kütüphanesine sürüklenip
bırakılabilir; çoklu dosyalar gerçek analiz ve kalıcı kayıt akışından geçer.
[Kullanım ve doğrulama](docs/medya-surukle-birak.md).

**0.18.0 (17.10.2026):** Çevrimiçi müzik altyapısı (YouTube Music, Spotify,
Apple Music hesapları, sağlayıcı tarayıcısı ve resmî oynatıcı) projeden
kaldırıldı; yayın yalnız yerel medya arşivinden yapılır. 0.16.0 ve 0.18.0'ın
önceki çevrimiçi müzik notları yalnız sürüm geçmişidir. Ses yönlendirme sekmesi
korunur.

**0.19.0 (17.10.2026):** rotasyon düzenleyiciye "AutoDJ otomasyonu" sayfası
eklendi: ses/bas/filtre eğrileri ve mikser efektleriyle tasarlanan geçiş ön
ayarları, türlere atama, sıralı/rastgele rotasyon ve gün/saat dilimleri. Motor
bindirme boyunca tasarlanmış eğrileri gerçek PCM'de uygular.
[Kullanım ve sözleşme](docs/10-playlist-ve-otomasyon.md#autodj-otomasyonu-tasarlanmış-geçiş-ön-ayarları).
Başlatıcı, derleme başarısız olursa son sağlam `build/radios` ile açılır ve
`RADIOS_SKIP_BUILD=1` ile derleme adımı atlanabilir.

**Sessizlik koruması:** Acil › **Sessizlikte otomatik devralma** artık gerçek bir ölü hava dedektörüdür: MAIN çıkışı sürekli ölçülür, eşik altı sessizlik süreyi aşınca yalnız alarm / sıradaki parça / acil liste basamakları çalışır, olay tanı kaydına yazılır ve ses dönünce kapanır. [Kullanım](docs/sessizlik-korumasi.md).

**Ayarlar › Medya arşivi:** İstasyonun ses arşivinin okunduğu klasörler (en çok 16, harici disk olabilir) burada tanımlanır. **Şimdi tara ve içe aktar**, klasörleri arka planda SHA-256 ile tarar, kütüphanede zaten bulunan ve arşiv içinde tekrarlanan dosyaları atlar, kalanları medya kütüphanesinin gerçek içe aktarmasından geçirir; istenirse her alt klasör aynı adlı playliste eklenir ve RadioS açıkken yeni dosyalar belirli aralıkla izlenir. Aynı sayfa WAV kayıt klasörünü (`config/storage.ini`) seçtirir; yedek ve veritabanı klasörleri boyutlarıyla görüntülenir. [Kullanım ve sınırlar](docs/medya-arsivi.md).

**Playlist düzenleme:** Üstte **Ana medya** arşivi doğrudan parçaları gösterir;
arama kutusu bu arşivde arar. **Alt** ile tek tek, **Shift** ile
aralık seçip parçaları alttaki playliste sürükle. Alttaki **+**, adı ve kullanım
türüyle (ana müzik, jingle, reklam, anons, program, diğer) yeni playlist oluşturur.
**+ Parça** gerçek ses dosyası importunu açar; import bitince ana medya yenilenir.
Ad değiştirme ve silme, playlist sekmelerinin yanındaki **⋯** menüsündedir.

**rekordbox USB / SD:** Playlist ekranı artık CDJ Device Library
`PIONEER/rekordbox/export.pdb` dosyasını doğrudan okur; XML dışa aktarımı
gerekmez. Klasör/playlist seçimi ve kaynak parça sırası korunur.
[Kullanım, desteklenen format ve GitHub örneği](docs/rekordbox-usb-tracklist.md).

0.17.0 strict C11 ve native GUI kabulü **54/54**, **331,91 saniyede** geçti; atlanan
test yoktur. Özgün veritabanı motorunun dokuz koşucusu ayrıca **333/333** kontrolü
geçti. Mevcut kullanıcı DB'sine test verisi yazılmadı. Program taslağını katalogdan
ekleme/değiştirme, adlandırılmış kopya sakla/yükle ve geçici okuma istek sınırından
toparlanma da gerçek native işlemlerdir. [Doğrulama kapsamı](tests/README.md).

Mevcut veritabanı motorunun kaynakları `database/SDBASE/` içine alınmıştır. Motor Python ile yazılmıştır; C uygulaması bu veri servisine bağlanır. Gerçek çalışma veritabanı `data/database/RadioS.sdb` içinde oluşturuldu; yerel servis, özel yönetici/uç kimlikleri ve doğrulanmış ilk yedek hazırdır.

**1. aşama tamamlandı:** yedi profil ve ortak alanlar için 194 işlev grubu, gerçek veri API'leri, eksik C işleri ve kabul ölçütleri belgelendi. Kontrol/API envanteri ve belge referansları doğrulandı.

**2. aşama tamamlandı:** C modülleri, başlıklar, arayüz kaynakları, yapılandırma, veritabanı kaynakları ve çalışma verileri için proje düzeni kuruldu. 146 SDBASE ve 72 tasarım dosyası proje içine kopyalandı; font/lisans birlikte alındı. Kaynak özetleri, proje içi yollar ve bağımsız kopya kontrolü geçti. Ayrıntı ve doğrulama [proje düzeni belgesindedir](docs/02-proje-duzeni.md). Bundan sonraki geliştirme RadioS içindeki kaynaklarda yapılacaktır.

**3. aşama tamamlandı:** C11/CMake, native GTK 3 pencere, INI okuyucu, yaşam döngüsü ve sınırlı iş/durum kuyrukları kuruldu; CTest 4/4 geçti. Pencerenin teknik alanındaki kaynak denetimi gerçek dosya sistemi kontrollerini arka planda yürütür. Ayrıntı: [C uygulama altyapısı](docs/03-c-uygulama-altyapisi.md).

**4. aşama tamamlandı — GUI:** ortak başlık, yedi görünüm, araç çubukları, tablolar, knob/meter çizimleri ve düzenleyiciler native C bileşenleridir. Parça işaretleri, geçiş, playlist, otomasyon, sampler ve acil sıra kontrolleri kullanıcı girdisinden oturum taslakları oluşturur. Otuz CSS kaynağından 1.552 uyumlu stil kuralı aktarılır. [GUI kapsamı ve kabul](docs/04-native-tasarim.md) etkileşimleri ve sonraki aşamaların veri/ses sınırlarını açıklar. Bu aşamanın oturum taslakları aşama 6 ile kalıcı veri kayıtlarına bağlandı; bu sürüm yayın yapmaz.

**5. aşama tamamlandı — veritabanı:** SDBASE şema 4, ilk yönetici/istasyon/offline uç kimlikleri, özel erişim dosyası, yerel servis başlatma/durum/kontrol/kapatma ve ilk yedek kuruldu. Kimlikli HTTP, kalıcı kayıt, tekrar kurulum, yol/port çakışması ve kesilen kurulumdan devam testleri uygulandı. [Kurulum ve servis belgesi](docs/05-veritabani-kurulumu.md) komutları ve kapsamı açıklar. C arayüzünün bu verileri okuması/yazması aşama 6 ile tamamlandı.

**6. aşama tamamlandı — C veri bağlantısı:** yedi profil ve düzenleyiciler gerçek kimlikli HTTP/JSON veri akışına bağlandı. UUID, yetki, revision/CAS, işlem alındısı, çakışma ve kesintide girdi koruma uygulandı; kalıcı kayıtlar yeniden açılışta yüklenir. [C veri bağlantısı belgesi](docs/06-c-veri-baglantisi.md) her ekranın kapsamını ve medya/ses aşamalarının sınırını açıklar.

**7. aşama tamamlandı — medya kütüphanesi:** native dosya seçimi ve C/FFmpeg analizinden gerçek SHA doğrulamalı yönetilen medya kaydına kadar akış bağlandı. Türkçe arama, tür/alan filtresi, sayfalama, metadata, ölçülen waveform ve medya UUID/SHA/süresine bağlı cue taslağı kalıcıdır. Teknik doğrulama yayın onayı vermez. [Medya kütüphanesi belgesi](docs/07-medya-kutuphanesi.md) kullanım, format matrisi ve kesinti/tekrar deneme sınırlarını açıklar.

**8. aşama tamamlandı — ses motoru ve cihazlar:** C/FFmpeg artımlı çözücü, 32 normal oynatıcı ve ayrı CUE ön dinleme, gerçek transport/sarma/gain, bağımsız çıkışlar ve mikrofon kapısı bağlandı. Cihaz tercihleri istasyon DB'sinde `manage` yetkisi ve CAS ile saklanır; açılışta oynatma/mikrofon başlamaz. [Ses motoru belgesi](docs/08-ses-motoru.md) kullanım, kaynaklar ve kabul kapsamını açıklar.

**9. aşama tamamlandı — DJ ve geçişler:** 32 mixer kanalı, ortak MASTER, üç bant EQ/pan/gain, mute/solo/PFL, eşit güç crossfader, 14 efekt, kanal/MASTER send-return ve gerçek mikrofon ducking bağlandı. Kanal ve MASTER padleri doğrulanmış medya sürümünü ayrı sampler seslerinde çalar. A/B editörü gerçek iki dosyalı miksini CUE'da dinletir; ayarlar ve medya bağları DB'de kalıcıdır. [DJ ve geçişler belgesi](docs/09-dj-ve-gecisler.md) kullanım ve kabul kapsamını açıklar.

**10. aşama tamamlandı — playlist ve otomasyon:** doğrulanmış playlist/program kuyruğu, gerçek otomatik EOF/crossfade/cue geçişleri, transport, UTC takvim ve kes/bitir olay politikaları C motoruna bağlandı. Kaynak döngüsü/ağırlığı/tamamlama, gün/saat/tekrar, jingle ve yedek kuralları C planlayıcısında çalışır. Gerçek MAIN tüketim geçmişi ve rotasyon checkpoint'i DB'de CAS ile saklanır; önizleme değişiklik yapmaz. [Playlist ve otomasyon belgesi](docs/10-playlist-ve-otomasyon.md) kullanım ve yürütme sınırlarını açıklar.

**11. aşama tamamlandı — reklam yönetimi:** gerçek reklam dosyası/sürüm bağlama, ayrı spot/kampanya onayı ve açık kullanım izni, güvenli satış düzenleme, ölçülmüş kuşak sesleri/kapasite/aşım/boş kuşak politikaları ve atomik önizleme/rezervasyon/iptal bağlandı. C motoru mevcut programı koruyarak saatli kes/bitir/bekle devri yapar; gerçek MAIN başlangıç/tamamlama/kesilme/hata kayıtları değişmez manifest ve filtreli rapor/CSV ile saklanır. Yerel tüketim sözleşme teslimi olarak sayılmaz. [Reklam yönetimi belgesi](docs/11-reklam-yonetimi.md) kullanım, kanıt sınıfları ve kaynakları açıklar.

**12. aşama tamamlandı — yayın, kayıt ve acil akış:** son MAIN miksinden gerçek WAV kayıt, native MP3/Icecast PUT, HTTP sonucu ayrı metadata ve doğrulanmış kütüphane medyasıyla acil devralma/dönüş bağlandı. Ayarlar ve değişmez runtime kayıtları DB'de saklanır; şifre oturumda tutulur. Normal sıra gerçek tüketim konumundan sürer; crossfade dönüşü gelen öğeyi korur. [Yayın, kayıt ve acil akış](docs/12-yayin-kayit-acil.md) kullanım, kanıt ve sınırları açıklar.

**13. aşama tamamlandı — yönetim ve güvenilirlik:** gerçek kullanıcı/izin/anahtar yönetimi, sağlık/bütünlük izlemesi, exact dayanıklı işlem kuyruğu, normal kapanış ve kesinti uzlaştırması, Pulse yeniden bağlantısı, tutarlı DB/medya/kurulum yedeği ve offline crash/rollback kurtarması bağlandı. Geri açılışta ses ve mikrofon operatör komutunu bekler. [Yönetim ve güvenilirlik](docs/13-yonetim-ve-guvenilirlik.md) kullanım ve doğrulanmış kurtarma sınırlarını açıklar.

**14. aşama — yazılım ve paket kabulü:** 0.14.0 Release ikilisi, yeniden derlenebilir kaynak paketi, hash/izin manifestleri ve otomatik ilk kurulum hazırdır. Temiz kurulum, ana sistemden SDK ile açılış/tekrar açılış, bağımsız kaynak derleme ve beş dakikalık gerçek zamanlı sanal ses/kayıt/MP3 kabulü geçti. Fiziksel hoparlör/mikrofon ve işletme süresine uygun saha denemeleri operatör kabulünü bekler. [Nihai kabul ve kurulum](docs/14-nihai-kabul-ve-paketleme.md).

**0.15.0 — Ses yönlendirme:** gönderilen akış görseli yeni sekmede C/GTK ile kuruldu. Seçilen uygulama ve mikrofon, gerçek kompresör ve bağımsız Peak/RMS göstergeleriyle tek WAV kaydına bağlanır; başlat/duraklat/devam/böl/durdur çalışır. Ayarlar gerçek DB'de saklanır. Sekmeyi doğrudan açmak için `./run-radios.sh --profile routing` kullanılır. [Kullanım ve kabul](docs/ses-yonlendirme.md).

**0.16.0 — Çevrimiçi müzik:** Teknik yönetim ayarlarına Spotify, Apple Music ve YouTube hesap/playlist yönetimi eklendi. Yerel çevrimiçi listeler ve servis profilleri istasyon DB'sinde kalıcıdır; resmî API'lerle liste okuma/içe aktarma, özel liste oluşturma ve öğe ekleme çalışır. Spotify/Connect oynatma kontrolü ve servislerin resmî oynatıcılarını açma bağlıdır. Hesap bağlantısı gereklidir; bu API'ler MAIN ses motoruna tam ses verisi sağlamaz. [Kurulum ve kullanım](docs/cevrimici-muzik.md).

## Derleme ve çalıştırma

Linux üzerinde C11 derleyici, CMake 3.20+, pkg-config, GTK 3.24+, GLib/GIO 2.66+, Fontconfig, JSON-GLib 1.10+, libcurl 7.85+, libpulse 15.0+ ve FFmpeg geliştirme paketleri gerekir. Minimum kararlı FFmpeg sürümü 5.1'dir: `libavformat>=59.27.100`, `libavcodec>=59.37.100`, `libavutil>=57.28.100`, `libswresample>=4.7.100` CMake'de denetlenir. 5.1 için kullanılan API'lerin başlık uyumluluğu incelendi; çalışma kabulü mevcut ortamın daha yeni FFmpeg kütüphanelerinde yapıldı. Format testleri fixture üretmek için `ffmpeg` komutu ve MP3/FLAC/Vorbis/AAC encoder'larını kullanır; normal uygulama FFmpeg ve Pulse kütüphanelerine doğrudan C'den bağlanır. Ses için erişilebilir PulseAudio veya PipeWire Pulse servisi gerekir. İnternet yayını için FFmpeg MP3 encoder'ı gerekir; bu ortamda libmp3lame doğrulandı. Build araçları Python 3.11+ kullanır; masaüstü arayüz C ile çalışır. Doğrulanan ortam Linux/Wayland'dir; başka işletim sistemi kabulü henüz yapılmadı.

`kaynak kodlar` dizininden:

```sh
cmake -S RadioS -B RadioS/build -DCMAKE_BUILD_TYPE=Debug
cmake --build RadioS/build --parallel
./RadioS/run-radios.sh
```

Çalıştırıcı ve `build/radios`, proje kökünü bulabilir; göreli veri yolları terminalin çalışma dizinine bağlı değildir. İstenirse `config/radios.example.ini`, `config/radios.ini` olarak kopyalanıp değiştirilebilir. Bozuk kullanıcı yapılandırmasında program hata verir. Ayar doğrulaması pencere açmaz:

Kaynak klasöründe `./run-radios.sh` normal açılıştan önce güncel kaynakları
`build/radios` olarak derler; değişmeyen dosyalar yeniden derlenmez. Derleme
başarısızsa ve önceden derlenmiş `build/radios` varsa uyarı yazılıp son sağlam
sürüm açılır; hiç ikili yoksa açılış durur. `RADIOS_SKIP_BUILD=1` derleme adımını
atlar. Sistem geliştirme bağımlılıkları eksikse mevcut
Freedesktop SDK 25.08 kullanılır. İlk derleme Release olarak hazırlanır; mevcut
CMake seçenekleri korunur. Hazır ikili paketler ile `--version`, `--help`,
`--check-config` ve `--smoke-test` komutları derleme yapmaz.

Normal `run-radios.sh`, temiz kurulumun veritabanını/özel kimliklerini oluşturur ve bu kuruluma ait servisi başlatır; mevcut verileri korur. Salt okuma kipleri kurulum yapmaz. İkilinin sistem kütüphanesi eksikse makinede zaten kurulu Freedesktop SDK 25.08 üzerinden yalnız C penceresi çalıştırılır; veri servisi başlatıcının süreç ortamında kalır. Aynı yol `run-radios-sdk.sh` ile seçilebilir. [Paketler ve bağımlılıklar](packaging/README.md).

```sh
./RadioS/run-radios.sh --check-config
ctest --test-dir RadioS/build --output-on-failure
```

Gerçek ekran oturumunda GUI kabulünü CTest'e eklemek için `cmake -S RadioS -B RadioS/build -DRADIOS_GUI_TESTS=ON` çalıştırılır. `native_controls`, düzenleyicileri ve görünüm tercihlerini; `native_data` ayrı SDBASE servisinde kalıcılık/çakışma/kesintiyi; `native_media` gerçek C import/metadata/cue ve yanıt kaybından tekrarı sınar. `native_audio`, kütüphane/oynatıcı/editör kontrolleriyle gerçek PCM, CUE ayrımı, ses tercihi CAS ve kapanışı; `audio_devices`, ayrı sanal çıkışlar ve mikrofon kaynağıyla gerçek Pulse tüketimini sınar. `dsp`, `transition` ve `native_dj`, gerçek örneklerden efekt/crossfade/ducking, iki dosyalı A/B hazırlığı ve native mixer/pad/geçiş kalıcılığını sınar. Ses kabulünde `pactl` ve `parec` gerekir. `native_window`, sekiz profile geçer ve `build/native-window.png` görüntüsünü kaydeder. Normal kullanımda pencere açık kalır; `Ctrl+Q` kapatır. Profil sekmeleri sol/sağ, Home/End tuşlarıyla da seçilir.

Teknik yönetim → Aygıtlar ve görünüm → Tercihleri kaydet, yazı boyutu ve başlangıç/son profilini `config/ui-preferences.ini` içine atomik olarak yazar. Bu dosya kaynak takibinden ayrıdır; aygıt ataması veya API anahtarı içermez. Aynı alandaki ses seçimlerini uygulama/kaydetme düğmesi gerçek çıkış ve giriş tercihlerini istasyon veritabanında saklar. Kalıcı kayıtlar yeniden açılışta yüklenir; başarısız kayıtta girilen alanlar korunur.

Yayın → **Medya kütüphanesi** → **+ Ses dosyaları**, yerel WAV, MP3, FLAC, Ogg, M4A ve AAC dosyalarını analiz ederek içe aktarır. Kayıt seçildikten sonra metadata ve **Dalga biçimi / Cue** taslağı kaydedilebilir. Sonucu belirsiz aktarım aynı dosya/tür/başlık ve aynı işlem anahtarıyla tekrar edilir. Dayanıklı kuyruktaki yazılar yeniden açılışta kimlik kontrolünden sonra aynı gövdeyle kurtarılır; yetki veya CAS nedeniyle reddedilen işlem inceleme için tutulur.

Yayın sırasındaki evente veya numarasına tıkla: aynı tablo yalnız o eventin parçalarını oynatma sırasıyla gösterir; ağaç görünümü açılmaz. **‹ Eventler** ile genel sıraya dön. Eventleri genel listede, parçaları açık eventin içinde sürükleyerek sırala. Sağdaki **Programlar** listesinden kayıtlı programı yayın sırasına sürüklemek bütün içeriğini tek event olarak ekler. Açık eventin boş alanına bırakılan parça o eventin sonuna eklenir; değişiklikler mevcut kalıcı kayıt akışını kullanır.

Kütüphanede kaydı seçip **Oynatıcıya yükle**, ardından **Canlı oynatıcılar → Oynat** kullan. Sarma/duraklatma/durdurma ve ses seviyesi gerçek motoru yönetir. **CUE ön dinle** ve waveform editörünün ön dinlemesi için Teknik yönetimden CUE cihazı ata. Mikrofon açıkça etkinleştirilir; yeniden açılışta kapalıdır. Cihaz bulunamazsa adı korunur ve hata gösterilir. Ses sunucusu kesilip döndüğünde aynı cihaz bağlantısı yeniden kurulur; ses duraklatılmış, mikrofon kapalı kalır. Cihaz hazır olduktan sonra açık oynat/devam komutu verilir.

DJ görünümünde kanal kaynağını seç, EQ/pan/fader ve A/B grubunu ayarla. Kanal **Effector** seçimi dört slotun birini bağlar; MASTER kendi efekti ve seçilen ortak slotla çalışır. Her kanalın ve MASTER'ın en altındaki **Kanal adı** alanından en fazla 64 karakterlik ad verilebilir; adlar başlık, sampler ve yönlendirme seçimlerinde görünür. Kanal adları, mixer/DSP ayarları ve MASTER gain değişiklikleri otomatik olarak veritabanına kaydedilir ve yeniden açılışta yüklenir. Hızlı değişikliklerde son değer korunur; kapanıştaki bekleyen kayıtlar kurtarma kuyruğuna alınır. Pad ve geçiş editörlerindeki **Kütüphaneden seç**, doğrulanmış kayıt bağlar; A/B **Ön dinle**, yalnız CUE'da gerçek miksini çalar. Teknik yönetimde uygulanmamış cihaz seçimi varsa otomatik kayıt bu seçimi korur.

Playlist'te gerçek kütüphane kaydı bağlayıp seçili satırları veya tüm listeyi program kuyruğuna ekle. Program kontrolleri gerçek başlatma, duraklatma, önceki/sonraki ve UTC takvim yürütmesini yönetir. Otomasyonda playlist kaynaklarını ve boş MAIN oynatıcılarını seç; ağırlık, sıra, tekrar, gün/saat, jingle ve yedek kurallarını ayarla. **Önizle**, kaydedilmemiş formun planını geçmişi değiştirmeden gösterir; **Başlat** gerçek PCM yürütür. Açılışta otomasyon kapalıdır. Bir çalıştırmadaki 500 giriş sınırında ve tek oynatıcılı yeniden yüklemede sınırlı hazırlık boşluğu oluşabilir; ayrıntı [playlist ve otomasyon belgesindedir](docs/10-playlist-ve-otomasyon.md).

Reklam'da kampanya oluştur, kütüphaneden **Reklam** türünde doğrulanmış ses bağla ve spot/kampanya onaylarını ayrı ver. Kullanım izni kutusu açıkça seçilir ve gerçek çıkış/izin referansı ister. Kuşak gün/çıkış/saatini ve gerçek açılış/kapanış/anons dosyalarını seç, **Önizle → Rezerve et → Çal/Takvime al** kullan. Tarih/kampanya/çıkış filtreli rapor değişmezdir; CSV seçili sayfayı dışa aktarır. Bekleyen kuşağı iptal etmek mevcut programı durdurmaz; [ayrıntılı kullanım](docs/11-reklam-yonetimi.md).

Teknik yönetim → **Kişiler**, gerçek kullanıcı durumunu, istasyon izinlerini ve anahtar süre/iptal bilgisini yönetir. **Sunucular → İzleme ve kurtarma**, servis/bütünlük/cihaz durumunu ve bekleyen işlemleri gösterir. **Yedekler**, tutarlı yedek oluşturur, doğrular ve geri yükleme planı/hazırlık kopyası üretir. Gerçek geri yükleme uygulama ve servis kapalıyken yapılır; [komutlar ve kesinti davranışı](docs/13-yonetim-ve-guvenilirlik.md).

7. aşama kabulü: strict C11 derlemesi ve CTest **15/15** başarılı. Gerçek C analiz **10/10**, kimlikli backend medya **14/14**, native medya ve sanitizer kabulü **5/5**; C analiz ve veri istemcisi sanitizer kabulü **10/10** başarılıdır. Güncel 0.7.0 uygulaması ve servis açıldı; gerçek depo/istasyon/uç kimlikleri ve sequence 2 korundu, bütünlük kontrolü geçti. Ayrıntılı format, CAS ve tekrar deneme kapsamı medya belgesindedir; testler üretim DB'sine fixture yazmaz.

8. aşama kabulü: strict C11 ve CTest **19/19** başarılı (134,77 saniye). Artımlı çözücü **11/11**, motor **6/6** ve gerçek Pulse **1/1**, native ses arayüzü **6/6** başarılı; aynı kapsam ASan/UBSan altında geçti. Core çözücü/motor için leak detection açık, GTK kabulünde kapalıdır. Typed backend API **13/13** ve önceki medya/veri hedefleri de başarılıdır. 0.8.0 uygulaması ve güncel servis mevcut çalışma veritabanıyla açıldı; cihaz kabulü sanal çıkışlarla yapıldı. Fiziksel dinleme ve uzun süreli yayın kabulü 14. aşamadadır.

9. aşama kabulü: strict C11 derlemesi ve birleşik CTest **22/22** başarılı (173,78 saniye). DSP **13/13**, motor **9/9** ve ek gerçek Pulse grubu, geçiş **3/3**, native DJ **5/5**, typed backend **18/18** başarılıdır. Core DSP/motor/geçiş ASan/UBSan ve leak detection açıkken; native DJ ve son native ses **6/6** ASan/UBSan altında geçti. Son düzeltmeler native hedeflerde ayrıca doğrulandı. Testler özel DB ve sanal ses cihazları kullanır; üretim kayıtlarına fixture eklemez.

10. aşama kabulü: strict C11 ve birleşik CTest **26/26** başarılı (**226,46 saniye**). C planlayıcı **17/17**, yürütücü **15 deterministik + 1 gerçek Pulse**, backend hazırlık/geçmiş **13/13**, typed API **18/18**, native playlist/otomasyon **5/5** başarılıdır. Core ve native kapsam ASan/UBSan altında geçti; native sanitizer kabulü **64,47 saniye**. Tek müziklik partilerin A→jingle→B devamı normalde 1 kare, sanitizer koşusunda 13 ms en uzun sessizlikle ölçüldü. Önceki parça yeniden çalındığında geçmiş ayrı kayıt oluşturur; pause/resume yeni kayıt üretmez. Üretim DB'sine fixture eklenmedi.

11. aşama kabulü: strict C11 ve birleşik CTest **28/28**, **317,42 saniye** başarılı. Reklam backend'i **22/22**, native reklam **5/5** (**79,77 saniye**), C planlayıcı **18/18** ve yürütücü **21 deterministik + 1 gerçek Pulse** başarılıdır. Native reklam ASan/UBSan **5/5** (**122,27 saniye** test uygulaması), core ASan/UBSan ve leak denetimi açıkken **22/22** temizdir; GTK global verileri için native leak denetimi kapalıdır. Kaynak 30 CSS/1.552 native kural aktarımı korunur; API envanteri **201 workflow / 251 yol / 256 HTTP işlemi / 170 toplam kaynak dosyası (72 tasarım + 98 Python modülü)** ile doğrulandı.

12. aşama kabulü: Strict C11 ve birleşik CTest **31/31**, **204,50 saniye** başarılı. Yayın core **8/8**, typed backend **15/15**, native yayın/acil **4/4** başarılıdır. Native ASan/UBSan **4/4**, **79,78 saniye** temizdir (GTK leak kapalı); WAV/stream core **8/8** leak denetimi açıkken temizdir. C yürütücü/planlayıcı/output sanitizer hedefleri ayrıca **3/3** geçti. Yeni genel belge read-order testi normal ve ASan/UBSan altında geçti; korumaları kaldıran iki özel negatif kontrol beklenen gerçek hatayı gösterdi. CUE ve otomasyon regresyonları son kaynaklarla başarılıdır. Bu aşamanın API envanteri **206 workflow / 256 yol / 261 HTTP işlemi / 171 toplam kaynak dosyası (72 tasarım + 99 Python modülü)**, kaynak CSS **30 dosya / 1.552 native kural** korunur.

13. aşama kabulü: strict C11 ve birleşik CTest **36/36**, **242,54 saniye** başarılı. Kuyruk **10/10**, istemci **15/15**, backend yönetim **26/26**, desktop regresyonu **108/108**, yedek/kurtarma **18/18**, native yönetim **6/6** ve yayın/kapanış kurtarması **5/5** geçti. Core, native yönetim/yayın ve gerçek özel ses sunucusu yeniden başlatması ASan/UBSan altında kabul edildi; core ve özel ses testinde leak detection açık, GTK testinde kapalıdır. Bu aşamanın envanteri **211 workflow / 261 yol / 266 HTTP işlemi / 172 toplam kaynak dosyası (72 tasarım + 100 Python modülü)**; kaynak CSS **30 dosya / 1.552 native kural** korunur.

0.16.0 çevrimiçi müzik kabulü: strict C11 ve birleşik CTest **45/45**, **280,82 saniye** geçti. Yeni C servis API'leri **16/16**, gerçek HTTP/DB **9/9**, native GTK **2/2** ve ASan/UBSan kontrolleri başarılıdır. Canlı servis hesaplarıyla kabul, kullanıcı hesabı/uygulama kimlikleriyle yapılır; testler özel yerel API/DB kullanır. [Kurulum ve kanıt kapsamı](docs/cevrimici-muzik.md).

0.15.0 ses yönlendirme kabulü: strict C11 derlemesi ve birleşik CTest **42/42**, **270,13 saniye** başarılı; atlanan test yoktur. Yeni C ses motoru ve native GTK akışı bağımsız WAV analiziyle, ASan/UBSan altında ve ana sistem PipeWire bağlantıları üzerinden doğrulandı. [Ses yönlendirme kanıtları ve kullanım](docs/ses-yonlendirme.md).

14. aşama yazılım kabulü: strict optimize Release derlemesi ve birleşik CTest **39/39**, **256,50 saniye** başarılı. Paket güvenliği **13/13**, gerçek başlatıcı/temiz kurulum **8/8** geçti. Bağımsız arşiv kabulü yedi native görünümü, kaynak paketinden yeni Release derlemesini, typed HTTP/CAS/kalıcılığı ve ana sistemden SDK kullanarak iki açılış arasında aynı kimlik/servis sahipliğini doğrular. Beş dakikalık gerçek zamanlı sanal ses koşusunda **160 geçiş**, **40 kuyruk ekleme**, sonlandırılmış WAV ve bağımsız MP3/HTTP 200 elde edildi; kısa ASan/UBSan koşusu leak denetimi açıkken geçti. Bu koşu fiziksel donanım veya 24 saatlik saha kabulü değildir.

0.16.0 kabulünde C programı mevcut veritabanıyla açıldı. Yeni servis/playlist ayar API'leri HTTP 200; kurulum UUID'leri, şema 4, sequence **2**, üretim koleksiyonları ve bütünlük **ok** korundu. Playlist testleri üretim DB'sine yazılmadı.

0.15.0 kabulünde program yeni sekme seçilerek mevcut veritabanıyla açıldı. Yeni ayar API'si HTTP 200; şema 4, sequence **2**, bütünlük **ok** ve kurulum UUID'leri/üretim koleksiyonları korundu. Ses yakalama operatörün **Kaydı başlat** komutunu bekler.

14. aşama kabulünde **0.14.0** C uygulaması ve veri servisi mevcut çalışma veritabanıyla açıldı. Depo/istasyon/uç/kullanıcı kimlikleri, şema 4, sequence **2**, bütünlük **ok** ve üretim koleksiyonları korunur. Üretim DB'sine test kaydı yazılmaz. `data/backups/radios-20261006T082436Z-f90a3ffe` yedeği oluşturulup doğrulandı. Son kabul kanıtları ve kalan fiziksel kontroller [14. aşama belgesindedir](docs/14-nihai-kabul-ve-paketleme.md).

Normal C penceresi mevcut kimliklerle yerel veri servisini açar. Servis ayrıca şu komutlarla yönetilir:

```sh
./RadioS/run-database.sh init
./RadioS/run-database.sh start
./RadioS/run-database.sh check
./RadioS/run-database.sh status
./RadioS/run-database.sh backup
./RadioS/run-database.sh stop
```

Tekrar `init` mevcut kimlikleri korur; `start` çalışan servisi çoğaltmaz. Varsayılan adres `127.0.0.1:8000`'dir. Özel kimlik dosyası `config/credentials.json` ve bütün çalışma verileri kaynak takibinden ayrıdır. Yerel servis Python 3.11+ kullanır; ek çalışma paketi gerektirmez.

## Proje düzeni

| Yol | İçerik |
|---|---|
| `src/`, `include/radios/` | C uygulama kaynakları ve ortak başlıklar |
| `assets/` | Native arayüz font, ikon ve tema kaynakları |
| `config/` | Yapılandırma örneği; kullanıcı dosyası kaynak takibinden ayrı |
| `database/SDBASE/` | Mevcut veri motorunun kaynak, belge ve kabul araçları |
| `design/reference/tasarrım/` | Yalnız görsel tasarım referansı |
| `data/` | DB, uç, içe alma, önbellek, günlük, kayıt ve yedek dizinleri |
| `CMakeLists.txt`, `build/`, `cmake/` | Derleme hedefleri, çıktılar ve derleme desteği |
| `packaging/`, `tests/`, `tools/`, `docs/` | Paketleme, doğrulama, geliştirme araçları ve belgeler |

## İşlev haritası

- [1. aşama: kapsam, mimari sınırlar ve kabul ölçütleri](docs/01-islev-haritasi.md)
- [2. aşama: proje düzeni ve kaynak aktarımı](docs/02-proje-duzeni.md)
- [3. aşama: C uygulama altyapısı ve yerel pencere](docs/03-c-uygulama-altyapisi.md)
- [Native tasarım ve mevcut CSS aktarımı](docs/04-native-tasarim.md)
- [5. aşama: veritabanı kurulumu ve yerel servis](docs/05-veritabani-kurulumu.md)
- [6. aşama: C veri bağlantısı ve kalıcı native işlemler](docs/06-c-veri-baglantisi.md)
- [7. aşama: gerçek medya kütüphanesi, analiz ve cue taslakları](docs/07-medya-kutuphanesi.md)
- [8. aşama: gerçek ses motoru, çıkışlar ve mikrofon](docs/08-ses-motoru.md)
- [9. aşama: DJ mixer, efektler, sampler ve A/B geçiş ön dinlemesi](docs/09-dj-ve-gecisler.md)
- [10. aşama: gerçek playlist/program, UTC takvim ve otomasyon](docs/10-playlist-ve-otomasyon.md)
- [11. aşama: gerçek reklam medya/onay/kuşak ve yerel raporlar](docs/11-reklam-yonetimi.md)
- [12. aşama: gerçek MAIN kayıt/stream, metadata ve acil devralma/dönüş](docs/12-yayin-kayit-acil.md)
- [13. aşama: kullanıcı/yetki, izleme, dayanıklı işlemler ve yedek-kurtarma](docs/13-yonetim-ve-guvenilirlik.md)
- [14. aşama: dağıtım paketleri, temiz kurulum ve nihai kabul](docs/14-nihai-kabul-ve-paketleme.md)
- [Yayın, DJ ve ortak ekran işlevleri](docs/yayin-dj-islevleri.md)
- [Playlist ve otomasyon işlevleri](docs/playlist-otomasyon-islevleri.md)
- [Acil, reklam ve teknik yönetim işlevleri](docs/acil-reklam-teknik-islevleri.md)
- [Kontrol envanteri ve doğrulanmış API imzaları](docs/islev-envanteri.json)
- [Mevcut SDBASE OpenAPI sözleşmesi](docs/sdbase-openapi.json)
- [14 aşamanın ilerleme durumu](docs/gelistirme-asamalari.md)

## Envanter doğrulaması

RadioS klasöründen:

```sh
python3 tools/islev_envanteri.py --check
```

Çıktılar kaynaklar değiştiğinde yeniden üretilebilir:

```sh
python3 tools/islev_envanteri.py
```

Bu Python dosyası yalnızca geliştirme sırasında belge/analiz çıktısı üretir; RadioS masaüstü programının bir bileşeni değildir. Araç RadioS içindeki kaynakları okur; veritabanı oluşturmaz, servis başlatmaz ve referans kaynakları değiştirmez. Başka bir RadioS düzenindeki kaynak kökü için `--source-root` kullanılabilir. Terminalin çalışma dizininden bağımsız olarak mutlak araç yoluyla da çağrılabilir.
