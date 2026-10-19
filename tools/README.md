# Geliştirme araçları

`islev_envanteri.py`, proje içindeki `design/reference/tasarrım/` ve `database/SDBASE/src/` kaynaklarından kontrol/API envanteri üretir. Python dosyası yalnız geliştirme aracıdır; C masaüstü uygulamasının arayüzü veya ses motoru değildir.

RadioS kökünden:

```sh
python3 tools/islev_envanteri.py --check
```

Araç başka çalışma dizininden mutlak dosya yolu ile de çağrılabilir; varsayılan kaynak kökünü kendi dosya konumundan belirler. `--source-root` aynı RadioS dizin düzenindeki farklı bir kaynak kökünü incelemek içindir. Yeniden üretme için `--check` kaldırılır.

`css_to_gtk.py`, kaynak HTML/CSS içindeki uyumlu stilleri C derlemesine gömülecek GTK CSS'ye aktarır; uygulama HTML çalıştırmaz.

`radios_database.py` ve `radios_database_setup.py` yerel veri motorunun kurulum ve servis yöneticisidir. `../run-database.sh` üzerinden init/start/status/check/stop/backup komutları çağrılır. Python 3.11+ standart kütüphanesi ve proje içindeki SDBASE kaynakları kullanılır; terminale token yazılmaz. [5. aşama belgesi](../docs/05-veritabani-kurulumu.md) çalışma dosyalarını, ilk kimlikleri ve kesinti sonrası devamı açıklar. Masaüstü arayüz ve veri istemcisi C kaynaklarında geliştirilir.

`start`, çalışan servisin başlangıçta alınan kaynak özetini yerel SDBASE koduyla
karşılaştırır. Kod değişmişse yalnız bu kuruluma ait olduğu süreç kimliği ve depo
kimliğiyle doğrulanan sağlıklı servisi kapatıp güncel kodla yeniden açar. Başka
servisler ve sağlıksız süreçler korunur. `check`, eski kodla çalışan servisi açık
bir hata mesajıyla bildirir.

`radios_reliability.py`, tutarlı DB/medya/kurulum yedeğini oluşturur ve hash ile doğrular; geri yükleme planını ve ayrı hazırlık kopyasını üretir. Gerçek geri yükleme ve crash journal kurtarması açık uygulama/servis/DB kilitlerini koruyan offline işlemlerdir. Sahipsiz medya incelemesi ve doğrulanmış karantinaya taşıma da bu araçtadır. [13. aşama belgesi](../docs/13-yonetim-ve-guvenilirlik.md) komutları, kapsamı ve kullanıcıya ait dosyaların korunmasını açıklar.

`radios_launch.py`, normal masaüstü açılışında C yapılandırmasını doğrular, ilk kurulumu hazırlar ve kendi veri servisini başlatır. Salt okuma kipleri veri kurmaz. `radios_sdk.py`, eksik sistem ABI'sinde zaten kurulu SDK 25.08 üzerinden yalnız C arayüzünü çalıştırır; veri servisi başlatıcının süreç ortamında kalır. `../run-radios.sh` otomatik seçim, `../run-radios-sdk.sh` açık SDK seçimidir.

Kaynak kurulumunda normal açılış önce CMake ile güncel `build/radios` hedefini
derler. Mevcut CMake ayarları korunur. Derleme hatasında önceden derlenmiş
`build/radios` varsa uyarıyla o açılır; ikili yoksa açılış durur.
`RADIOS_SKIP_BUILD=1` derleme adımını atlar. Geliştirme bağımlılıkları eksikse
zaten kurulu SDK kullanılır. Hazır ikili dağıtımlar ve salt okuma komutları
derleme yapmaz. Doğrulama: `tests/test_radios_launch.py` (gerçek cmake çalıştırmaz).

`radios_package.py`, optimize Release ikilisi ve bağımsız kaynak için deterministik arşiv/manifest/SHA-256 üretir ve arşivi doğrular. Üretim verisi/kimlik/medya/ayar/yedek ve build fixture'ları alınmaz. [Paketleme komutları ve ABI gereksinimleri](../packaging/README.md).

`radios_hardware_check.py`, cihazları yalnız listeler ve operatörün fiziksel ses kabulü için kontrol listesi üretir; mikrofon kaydetmez veya hoparlöre test sesi göndermez. [14. aşama ve kabul komutları](../docs/14-nihai-kabul-ve-paketleme.md).
