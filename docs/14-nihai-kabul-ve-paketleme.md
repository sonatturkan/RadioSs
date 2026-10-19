# 14. aşama — nihai kabul ve paketleme

Bu belgedeki 0.14.0 sonuçları tarihsel aşama kabulüdür. Güncel 0.18.0 paket
komutları [paketleme belgesinde](../packaging/README.md), hesap/MAIN aktarımı
[çevrimiçi müzik belgesinde](cevrimici-muzik.md), genel kaynak,
veritabanı ve GUI kapsamı [kapanış denetiminde](proje-kapanis-denetimi.md), test
kapsamı [test belgesinde](../tests/README.md) tutulur. Fiziksel/saha maddeleri
aşağıda açık kalır; yazılım testi bu kabulün yerine geçmez.

RadioS 0.14.0, C11/GTK 3 masaüstü uygulaması ve proje içindeki SDBASE veri servisiyle dağıtılır. Kaynaklar, paketler ve kabul kanıtları RadioS klasöründedir. HTML/JavaScript tasarım referansıdır; 30 CSS kaynağındaki 1.552 native stil kuralı C derlemesine gömülür.

## Kurulum ve ilk açılış

İkili paket `dist/RadioS-0.14.0-linux-x86_64.tar.gz`, kaynak paket `dist/RadioS-0.14.0-source.tar.gz` içindedir. Paketleri mevcut canlı çalışma klasörünün üzerine açmayın; boş hedefteki RadioS klasörünü kullanın. Dosya SHA-256 değerleri yanlarındaki `.sha256` dosyaları ve paket içindeki `packaging/manifest.json` ile doğrulanır.

```sh
./run-radios.sh
```

Normal başlatıcı önce native yapılandırmayı doğrular. Temiz kurulumun özel kullanıcı/istasyon/uç kimliğini oluşturur, yalnız kurulumun kendi yerel servisini başlatır ve C penceresini açar. Mevcut kimlik dosyası ve çalışma verileri korunur. Oynatma, kayıt, stream ve mikrofon kendiliğinden başlamaz. `--version`, `--help`, `--check-config` ve `--smoke-test` veritabanı kurmaz veya servisi başlatmaz. Servis program kapatıldıktan sonra ayrı yönetilebilir:

```sh
./run-database.sh status
./run-database.sh check
./run-database.sh stop
```

Varsayılan port `127.0.0.1:8000`'dir. Başka kurulum bu portu kullanıyorsa `config/radios.example.ini` dosyasını `config/radios.ini` olarak kopyalayıp boş bir port seçin. Başlatıcı yabancı servisi kapatmaz veya benimsemez. Geçersiz ayar/argüman önce doğrulanır; kesilmiş restore işlemi [13. aşamadaki offline recover](13-yonetim-ve-guvenilirlik.md) tamamlanmadan açılmaz.

Kaynak paketini hedef sistemde derlemek için:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build --parallel
./run-radios.sh
```

İkili paket sistem kütüphanelerini kopyalamaz. Gerçek ABI ve GLIBC gereksinimleri `packaging/runtime.json` içindedir. Bu derleme FFmpeg 7 ABI'siyle yapılmıştır; farklı FFmpeg ABI'sinde hedef sistemde kaynak derleme gerekir. Mevcut bilgisayarda ana sistemin `libavformat.so.61` kütüphanesi eksik, kurulu Freedesktop SDK 25.08 ise aynı ikiliyi çalıştırır. Başlatıcı bu eksik yükleyici durumunda kurulu SDK'yı kullanır; SDK veya sistem paketi indirme/kurma işlemi yapmaz. Aynı yol açıkça seçilebilir:

```sh
./run-radios-sdk.sh
```

SDK çalıştırmasında yalnız native C arayüzü SDK'nın süreç ortamına girer. Python DB kurulumu ve servisi başlatıcının ana sistemdeki süreç ortamında kalır; ikinci açılış ve `run-database.sh status/stop/start` aynı servisin sahipliğini doğrular. Gerçek GTK penceresi ve aynı RadioS çalışma kökü kullanılır. Masaüstü ekranı, Pulse ses bağlantısı, ağ ve ev klasörü erişimi dosya seçimi/ithal/rapor ve kayıt işlemlerini sağlar. Çalışma verileri yeniden oluşturulmaz. Kaynak build minimumları ve paketleme ayrıntıları [paketleme belgesindedir](../packaging/README.md).

## Otomatik kabul

Temiz kaynak ve ikili arşivler ayrı Türkçe/boşluklu köklere açılır; gerçek üretim UUID/token/verisi bulunmadığı doğrulanır. Farklı çalışma dizininden ilk açılış, 0600 kimlik dosyası, kendi servisi, tekrar açılışta aynı kimlikler, gerçek typed HTTP/CAS/alındı/revision çakışması, servis yeniden başlatıldıktan sonra kalıcılık ve bütünlük denetlenir. Native kabul yedi görünümün gerçek CSS/font/pencere akışını çalıştırır. Kaynak arşivi mevcut proje kökünden bağımsız, uyarıları hata sayan optimize Release derlemesiyle yeniden derlenir.

```sh
python3 tests/run_release_acceptance.py --binary-archive dist/RadioS-0.14.0-linux-x86_64.tar.gz --source-archive dist/RadioS-0.14.0-source.tar.gz --report build/stage14-release-acceptance.json --jobs 3 --host-runtime
ctest --test-dir build --output-on-failure --parallel 2
```

Paket testleri deterministik üretim, tam hash/boyut/izin manifesti, yol/link/duplicate/boyut sınırı, gizli verinin dışlanması ve başarısız hazırlamanın eski arşivi korumasını sınar. Üretim DB, medya, anahtar, ayar, yedek, outbox ve logları paketlenmez; yeni kurulumda yalnız boş veri dizinleri vardır. Paket bütünlüğü dağıtıcı imzası yerine geçmez.

Strict optimize Release derlemesi ve birleşik GUI açık CTest **39/39**, **256,50 saniye** geçti; test atlanmadı. Paket güvenliği **13/13**, gerçek başlatıcı **8/8** başarılıdır. Ana sistemin doğrudan ELF yükleyicisindeki eksik FFmpeg ABI'si açıkça kaydedildi; mevcut SDK ile gerçek ilk/ikinci GTK açılışı, değişmeyen özel kimlikler ve ana sistemden servis durum/durdur/başlat sahipliği geçti. Kaynak paketinin bağımsız Release derlemesi mevcut SDK'nın geliştirme kütüphaneleriyle doğrulandı; ana sistemin eksik geliştirme paketleriyle kaynak derleme iddia edilmez.

Yerel kabul kanıtları `build/stage14-full.xml`, `build/stage14-package-verification.json`, `build/stage14-release-acceptance.json`, `build/stage14-soak-proof/acceptance.json` ve `build/stage14-release-verification.json` içinde tutulur. Son ikili/kaynak arşiv SHA-256 değerleri `.sha256` dosyalarında ve arşiv kabul raporunda aynı dosyalar için doğrulanır. Bu çalışma kanıtları ve üretim yedeği dağıtım paketlerine alınmaz.

## Sürekli ses testi

`test_audio_soak.c` gerçek Pulse saatinde programı yürütür, kuyruk ekler, crossfade/EOF makbuzlarını toplar, WAV kaydeder ve MP3'ü ayrı HTTP alıcısına gönderir. Wrapper bağımsız MAIN monitorü, WAV ve alınan MP3 çözümünü karşılaştırır; Stop sonrası sessiz kuyruğu yayın içi boşluk saymaz. RSS/FD/cursor ve çıkış taşması izlenir. Yalnız özel daemon ve sanal cihazlar kullanılır; masaüstü ses sunucusu kapatılmaz, gerçek mikrofon kaydedilmez.

```sh
python3 tests/run_audio_soak.py build/test_audio_soak --seconds 300 --report-dir build/stage14-soak-proof
```

Gerçek **300,011 saniyelik** koşu geçti: **160 geçiş**, **40 canlı kuyruk ekleme**, **160 EOF tamamlama + 1 açık Stop kesilme**, **14.401.001 MAIN karesi**, **57.646.080 WAV PCM byte** ve **4.804.224 bağımsız MP3 byte** ölçüldü. WAV sonlandırıldı, HTTP 200 doğrulandı. Üç bağımsız ses yolunda 500 ms ölçüm çözünürlüğünde yayın içi sessiz boşluk görülmedi; kayıt/stream taşması sıfırdır. Isınma sonrası RSS yaklaşık **1 MB** arttı, açık FD sayısı en fazla **12** olup Stop sonrası **7**'ye indi. Motor genel bir underrun sayacı sunmaz; bu raporun underrun kanıtı bağımsız ses ölçümleri ve özel daemon günlüğüyle sınırlıdır. Kısa koşu ASan/UBSan ve leak detection açıkken de geçti.

Beş dakikalık bu kanıt 24 saatlik işletme, fiziksel kablo/hoparlör/mikrofon veya gerçek yayın sunucusunun uzun süreli kabulü anlamına gelmez.

## Fiziksel kabul

```sh
python3 tools/radios_hardware_check.py --output build/stage14-hardware.json
```

Salt okunur denetim mevcut gerçek dahili hoparlör ve mikrofonu listeler. Üç HDMI portu kullanılamıyor olarak raporlanır. Donanımın listelenmesi sesi duyduğumuzu veya mikrofon akustik kalitesini kanıtlamaz. Operatör aşağıdaki sonuçları gerçek kurulumda kaydetmelidir:

- MAIN'de doğru fiziksel çıkış, iki kanal ve güvenli seviye.
- Ayrı CUE kulaklık yolu ve MAIN'e karışmama.
- Mikrofonun açık/kapalı davranışı, doğru giriş ve feedback olmaması.
- Kullanılacak harici kartta cihaz kaybı/dönüşünde kapalı kapılar ve açık resume komutu.
- Gerçek yayın sunucusundan bağımsız dinleyicide ses/metadata ve Stop.
- İşletme süresine uygun gerçek cihaz/sunucu koşusunda kopma, gecikme ve kaynak kullanımı.

Bu fiziksel/saha maddeleri henüz operatör tarafından doğrulanmadı. Yazılım ve paket kabulü tamamlandı; bütün 14. aşamanın fiziksel/saha kabulü bu kontrol sonuçlarını bekler.
