# Aşama 3 — C uygulama altyapısı

2026-10-06 tarihinde tamamlandı. RadioS 0.3.0, C11 ile derlenen ve gerçek GTK 3 masaüstü penceresi açan uygulamadır. Kullanıcının görsel düzeltmesiyle ilk boş iskelet yerine HTML/CSS tasarımındaki yedi ekranın native aktarımı da başlatıldı; [tasarım aktarımı](04-native-tasarim.md) ayrı belgelenmiştir.

| Kaynak | İş |
|---|---|
| `CMakeLists.txt` | Yalnız C hedefleri, bağımlılıklar, katı derleme ve CTest |
| `src/app/main.c` | Komut satırı, config yükleme, startup/activate/shutdown |
| `src/core/config.c` | INI, kök keşfi, alan/URI ve proje içi yol doğrulaması |
| `src/core/runtime.c` | Worker, sınırlı iş/olay kuyrukları, iptal, sahiplik, join |
| `src/core/diagnostics.c` | Dokuz dizin ve DB dosyasının salt okunur denetimi |
| `src/ui/window.c` | Native pencere, profil seçimi, olay teslimi ve pencere kabulü |
| `include/radios/` | Modül sözleşmeleri |
| `tests/test_config.c`, `tests/test_runtime.c` | C yapılandırma/eşzamanlılık testleri |
| `run-radios.sh` | Kendi dizininden proje kökünü bulup C ikilisini açar |

İş kuyruğu 16 bekleyen iş, olay kuyruğu 32 olay alır. Çalışan iş bekleyen kapasiteden ayrıdır. Worker GTK çağırmaz; ana thread olayları 50 ms aralıkla tüketir. Dolu iş kuyruğu açık hata verir. Dolu olay kuyruğu normal çalışmada kayıp yerine backpressure uygular. Başarılı submit sahipliği aktarır, başarısız submit sahipliği çağıranda bırakır; kabul edilen veriler bir kez temizlenir.

Kapanışta UI zamanlayıcıları kaldırılır, pencere ve bağlı diyaloglar yok edilir, yeni iş kabulü durdurulur, kooperatif iptal istenir, bekleyen işler temizlenir, kuyruk bekleyişleri uyandırılır ve worker join edilir. Config ancak join sonrasında temizlenir. Dolu durum kuyruğu kapanışı kilitlemez. Kapanış yeni olay teslimini durdurur. İşlerin sınırlı I/O kullanması ve iptali kontrol etmesi gerekir; iptal keyfî üçüncü taraf bloklamasını kesmez. Shutdown/free sahip thread'de yürütülür.

Proje kökü açık `--project-root`, gerçek executable üst dizinleri, ardından çalışma dizini üst dizinlerinden bulunur. İşaretler `config/radios.example.ini` ve `database/SDBASE/pyproject.toml` dosyalarıdır. Kardeş orijinal kaynaklara bağımlılık yoktur. Kullanıcı config'i mevcutsa `config/radios.ini`, yoksa örnek INI okunur; `--config` açık seçim yapar. Bozuk kullanıcı dosyasında örneğe sessiz düşülmez. `#`/`;` tam satır yorumları, LF/CRLF desteklenir; değer içindeki `;` korunur. Göreli yollar RadioS köküne göre çözülür; `..` ve mevcut symlink ile dışa kaçış reddedilir.

`--check-config` ekran açmaz. Config sürümü/profil/boolean/timeout, servis URI'si ve ortam değişkeni adı doğrulanır. Bu aşama DB dosyası/dizin oluşturmaz, token okumaz ve servis başlatmaz. `auto_start_playback=true` henüz olmayan ses motorunu başlatmaz.

Doğrulanan ortam Linux/Wayland, GCC 15.2.0, CMake 4.4.3, GTK 3.24.52 ve GLib/GIO 2.84.4'tür. C hedefleri C11, `-Wall -Wextra -Wpedantic -Werror` ile geçti. 12 config test grubu (21 bozuk alan örneği dahil), 8 kuyruk test grubu, `/tmp` üzerinden gerçek config okuma ve gerçek mapped-window kabulü geçti. **CTest 4/4 geçti.** Ek native görsel kabulde yedi profil ayrı başlangıç profiliyle açılıp kapatıldı; `G_DEBUG=fatal-warnings` altında her biri çıkış 0 verdi.

Build/çalıştırma komutları [README](../README.md) içindedir. Pencere testi gerçek ekran ister; `RADIOS_GUI_TESTS` varsayılan OFF'tur. Kanıtlar `build/Testing/Temporary/LastTest.log`, `build/native-window.png` ve `build/native-<profil>.png` altındadır. Gerçek DB/ses/yayın kabulü sonraki aşamalara aittir.

Resmi API kaynakları: [GtkApplication](https://docs.gtk.org/gtk3/class.Application.html), [Application.quit](https://docs.gtk.org/gio/method.Application.quit.html), [Application.shutdown](https://docs.gtk.org/gio/signal.Application.shutdown.html), [Widget.draw](https://docs.gtk.org/gtk3/method.Widget.draw.html), [ToggleButton.set_active](https://docs.gtk.org/gtk3/method.ToggleButton.set_active.html). Programatik toggle güncellemesinin tekrar clicked üretmesi seçim korumasıyla ele alınmıştır.
