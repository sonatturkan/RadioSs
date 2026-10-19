# Paketleme

RadioS iki ayrı dağıtım üretir: Linux Release uygulaması ve yeniden derlenebilir kaynak paketi. Her ikisi de `RadioS/` köküyle açılır. `build/radios` native C/GTK programıdır; SDBASE kaynağı ve Python 3.11+ servis araçları paketin içindedir. Tasarım HTML/CSS/JavaScript dosyaları görsel kaynak olarak taşınır; uygulama bunların HTML/JavaScript kısmını çalıştırmaz.

RadioS dizininde:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build build/release --parallel
python3 tools/radios_package.py --build-dir build/release package
python3 tools/radios_package.py verify --archive dist/RadioS-0.19.0-linux-x86_64.tar.gz
python3 tools/radios_package.py verify --archive dist/RadioS-0.19.0-source.tar.gz
```

`--kind binary` veya `--kind source` tek paket üretir. Kaynak paketi için hazır derleme gerekmez. Paket adı CMake sürümünden, binary mimarisi gerçek ELF başlığından gelir. İkili paket yalnız Release derlemesini kabul eder; farklı sürüm, sanitizer ikilisi veya ortamına bağlı RPATH/RUNPATH reddedilir.

`dist/` arşivlerin ve dış SHA-256 kontrol dosyalarının yeridir. `packaging/manifest.json` her dosyanın özetini, boyutunu ve izinlerini listeler. Doğrulama arşivi açmadan listelenmiş dosyaları kontrol eder; eksik/fazla dosya, değişmiş içerik/izin, yinelenen yol, sembolik/hard bağlantı, yol taşması ve boyut sınırı ihlali reddedilir. Aynı kaynak ve aynı binary yeniden paketlenirse aynı arşiv SHA-256 değeri oluşur; zaman damgası, makine kullanıcısı ve mutlak proje yolu arşiv başlığına eklenmez. SHA-256 içerik bütünlüğünü gösterir; dağıtıcı imzası değildir.

Paketleme açık dosya/dizin uzantı listesini kullanır. Üretim veritabanı, erişim anahtarları, kullanıcı yapılandırması, UI tercihleri, medya, kayıtlar, outbox, yedekler, servis durumu/logları, test derlemeleri, özel anahtarlar, cache ve sanal ortamlar alınmaz. `data/` altında yalnız boş çalışma dizinleri bulunur. Kaynak dizinlerindeki sembolik bağlantılar reddedilir. Paketler geçici özel dizinde hazırlanıp doğrulandıktan sonra son dosya adlarına atomik değiştirmeyle taşınır; başarısız hazırlama önceki doğrulanmış arşivi korur.

İkili paket sistem kütüphanelerini içermez. `packaging/runtime.json`, gerçek `DT_NEEDED` kütüphane adlarını ve ELF'in istediği en yüksek GLIBC sembol sürümünü belirtir. Bu sürümün ikilisi FFmpeg 7 ABI'si (`libavformat.so.61`, `libavcodec.so.61`, `libavutil.so.59`, `libswresample.so.5`) ile üretilmiştir. FFmpeg 5.1 sisteminde bu ikili yerine kaynak paketi hedef sistemin geliştirme kütüphaneleriyle derlenir. GTK masaüstü oturumu ve erişilebilir PulseAudio/PipeWire Pulse ses servisi gerekir. CMake'in kaynak derleme minimumları `CMakeLists.txt` içindedir.

Yüklü sistem kütüphanesi eksikse `run-radios.sh`, makinede zaten bulunan `org.freedesktop.Sdk//25.08` ve codec bileşenleriyle native uygulamayı yeniden çalıştırabilir. Aynı yol açıkça `./run-radios-sdk.sh` ile seçilir. Yardımcı yeni runtime veya paket kurmaz, indirme yapmaz. Eşleşen SDK yoksa eksik bağımlılık hatası gösterilir. Yalnız native C/GTK arayüzü SDK içinde çalışır; Python veritabanı kurulumu ve servis yönetimi çağıran host ortamında, aynı PID ad alanında kalır. Böylece yeni GUI açılışları aynı sahip olunan servisi tanır; `status`, `stop` ve yeniden başlatma tutarlı çalışır. SDK çalıştırıcısı GTK pencere/ses soketlerini, yerel ağ erişimini ve ev dizinindeki dosyaları açar; medya seçimi ve proje içindeki kayıtlar bu yolla çalışır. `--version`, `--check-config` ve smoke gibi salt okuma kipleri DB kurmaz veya başlatmaz. Bu makinenin doğrudan host ortamında `libavformat.so.61` eksikti; mevcut SDK ile native çalışma kabul edildi.

Yeni bir kurulumda arşivi mevcut canlı verinin üzerine açmayın; boş hedefe açın. RadioS kökünde `./run-radios.sh` çalıştırmak, yeni veritabanını özel kimliklerle otomatik kurar ve sahip olunan yerel servisi başlatır. Mevcut kurulumda kimlikleri korur ve çalışan servisi çoğaltmaz. `./run-database.sh init` ve `./run-database.sh start` ile elle hazırlama isteğe bağlıdır. Kaynak paketinde `run-radios.sh`, açılıştan önce güncel `build/radios` hedefini otomatik derler; geliştirme bağımlılıkları eksikse zaten kurulu Freedesktop SDK 25.08 kullanılır. İlk derleme Release olarak hazırlanır, mevcut CMake seçenekleri korunur ve başarısız derlemede uygulama açılmaz. Hazır ikili paketler ve salt okuma komutları derleme yapmaz. Kullanım/kurtarma belgeleri `docs/` içindedir. Üretim yedeği program arşivinden ayrı bir operatör işlemidir.

Inter fontu `assets/fonts/LICENSE.txt` ve tasarım kopyasındaki SIL Open Font License ile birlikte taşınır. RadioS/SDBASE kaynakları için depoda ayrı bir genel dağıtım lisansı bulunmadığından burada yeni bir lisans tanımlanmaz. Dinamik bağlı sistem kütüphaneleri bu arşive kopyalanmaz.

Kabul: `PYTHONDONTWRITEBYTECODE=1 python3 tests/test_packaging.py` özel geçici proje kökünde bütünlük, özel veri dışlama, deterministik üretim, bağlantı/yol korumaları ve başarısız hazırlama temizliğini sınar. Test gerçek üretim verilerine yazmaz.
