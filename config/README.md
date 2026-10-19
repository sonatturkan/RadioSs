# Yapılandırma

`radios.example.ini` gerçek token/UUID içermeyen yapılandırma örneğidir. C okuyucu ve `run-database.sh` servis yöneticisi bu dosyayı kullanır; dosya tek başına veritabanı başlatmaz.

İstenirse örnek `radios.ini` olarak kopyalanır; kullanıcıya ait dosya kaynak takibine alınmaz. Mevcut kullanıcı dosyası önceliklidir; bozuksa hata verilir. `--config` proje içindeki başka bir dosyayı açıkça seçer. Tüm göreli dosya yolları RadioS köküne göre çözülür. `--check-config` ekran açmadan doğrular. `#`/`;` tam satır yorumları desteklenir.

`run-database.sh init`, gerçek kimlik ve ayrı yönetici/uç tokenlarını `credentials.json` içine kullanıcıya özel `0600` izinle yazar. Normal `run-radios.sh` ilk açılışında aynı kurulumu otomatik yapar ve yalnız bu kuruluma ait servisi başlatır; sonraki açılışlarda kimlikler/veriler korunur. `--version`, yardım, `--check-config` ve smoke kipleri kurulum veya servis başlatmaz. Yabancı port ve tamamlanmamış restore işlemi korunur. Dosya kaynak takibinden ayrıdır; içeriğini kaynak koduna veya belgelere kopyalamayın. Servis komutları tokenları dosyadan okur, terminale/argümanlara yazmaz. Tokenlar 30 gün geçerlidir. C istemcisi dosyayı doğrulayarak okur; yapılandırılan `RADIOS_SDBASE_TOKEN` ortam değişkeni kullanıcı tokenını değiştirebilir. Servis yöneticisi yalnız yerel `http://127.0.0.1:PORT` adresini kabul eder.

`ui-preferences.ini`, Teknik yönetim içindeki kaydetme işlemiyle oluşan yerel görünüm/başlangıç profili dosyasıdır. Cihaz alanlarının boş olması atanmış cihaz olmadığı anlamına gelir; örnek hayalî ses kartı kimliği içermez. [Veritabanı kurulum belgesi](../docs/05-veritabani-kurulumu.md) komutları açıklar.

`--profile routing`, yeni Ses yönlendirme sekmesini bu açılış için seçer; kullanıcı INI dosyasını veya kaydedilmiş görünüm tercihini değiştirmez. Diğer geçerli profil kimlikleri `broadcast`, `playlist`, `automation`, `dj`, `emergency`, `advert`, `technical` değerleridir. Geçersiz kimlik veritabanı kurulumundan önce reddedilir. Yerel görünüm dosyasının sürümü 1 olarak kalır; sekizinci profil eski profil indekslerini değiştirmeden eklenmiştir.

Çevrimiçi müzik profilleri ve playlist başvuruları INI içinde değil istasyon DB'sindeki `online_services` / `external_playlist` belgelerinde saklanır. Servis anahtarları yalnız native ayar ekranındaki maskeli oturum alanlarında tutulur. [Hesap kurulumu](../docs/cevrimici-muzik.md).
