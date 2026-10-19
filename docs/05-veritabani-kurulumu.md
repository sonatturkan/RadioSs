# 5. aşama — yerel veritabanı kurulumu

RadioS içindeki SDBASE 0.8.1 motoru ve şema 4 ile kalıcı çalışma veritabanı kuruldu. Masaüstü program C/GTK olarak çalışır; HTML/JavaScript çalıştırılmaz. Mevcut Python veri motoru yalnız yerel veri servisi olarak kullanılır. Motorun kaynakları değiştirilmedi ve üçüncü taraf çalışma paketi kurulmadı.

## Kurulum ve servis

`kaynak kodlar` dizininden:

```sh
./RadioS/run-database.sh init
./RadioS/run-database.sh start
./RadioS/run-database.sh check
./RadioS/run-database.sh status
./RadioS/run-database.sh backup
```

Başka bir çalışma dizininden çalıştırıcının mutlak yolu da kullanılabilir. Yapılandırma C uygulamasındaki gibi önce `config/radios.ini`, yoksa `config/radios.example.ini` içinden okunur. Farklı proje içi yapılandırma `--config config/kurulum.local.ini` ile komuttan önce seçilir. Göreli dosya yolları RadioS köküne göre çözülür; dışarı çıkan fiziksel yollar reddedilir.

İlk `init`, `admin` kullanıcısını, `RADIOS` kodlu `RadioS` istasyonunu ve `main` yayın ucunu gerçek UUID'lerle oluşturur. Dokuz yönetici izni ve ayrı kullanıcı/uç erişim anahtarları kurulur. İstasyonun saat dilimi varsayılan `UTC`'dir; ilk kurulumda `--username`, `--station-code`, `--station-name`, `--timezone Europe/Istanbul` gibi seçenekler verilebilir. Başlangıç ucu **offline** durumundadır. Medya, playlist, program, reklam veya ses çıkışı örnek kayıtlarla doldurulmaz.

Kurulu projede `init` tekrar çalıştırılınca mevcut kimlikler doğrulanır ve korunur; seçenekler mevcut istasyonun adını veya kullanıcıyı değiştirmez. Yönetilmeyen mevcut DB, kayıp/bozuk kimlik dosyası ve farklı depoya ait kimlik otomatik olarak benimsenmez ya da sıfırlanmaz. Hata sırasında mevcut dosyaların üzerine yazılmaz.

`start`, varsayılan `http://127.0.0.1:8000` adresinde arka plan servisi başlatır. Yalnız `http://127.0.0.1:PORT` yerel adresi kabul edilir. En çok 32 bağlantı ve yapılandırılmış istek süre sınırı uygulanır. `start` tekrar çalıştırılınca aynı sağlıklı süreç kullanılır; portta başka servis varsa hata verilir.

```sh
./RadioS/run-database.sh stop
```

`stop` yalnız bu RadioS kurulumunun doğrulanmış servis sürecine kapanış sinyali verir. Linux açılış kimliği, süreç başlangıç zamanı, komut ve rastgele servis örneği eşleşmesi kontrol edilir; pidfd aynı süreci sabitler. Eski PID dosyası veya başka port dinleyicisi nedeniyle ilgisiz süreç kapatılmaz. Kabul edilmiş isteklerin bitmesi beklenir. Uzun süren bir yüklemede komut kapanışın devam ettiğini bildirebilir; zorla işlem kesmez. Sonraki `start`, aynı DB ve kimlikleri açar.

Servis C penceresinden bağımsızdır; C penceresinin kapanması veri servisini kapatmaz. Otomatik açılış ve C ekranlarının veri işlemleri aşama 6'nın işidir. `run-radios.sh` mevcut C masaüstü programını açar.

## Dosyalar ve kimlik

| Yol | İçerik |
|---|---|
| `data/database/RadioS.sdb` | Kalıcı SDBASE işlem günlüğü |
| `data/database/RadioS.sdb.meta`, `.lock`, `.checkpoint` | Depo kimliği/şema, süreç kilidi ve ilk kurtarma noktası |
| `config/credentials.json` | Gerçek yönetici/istasyon/uç/depo UUID'leri, ayrı iki erişim anahtarı ve süreleri |
| `data/database-service/` | İşlem ve servis kilidi, süreç durumu; gerekirse kesilmiş kurulum günlüğü |
| `data/logs/database-service.log` | Servisin başlangıç ve HTTP günlükleri |
| `data/backups/initial-installation/` | Doğrulanmış ilk kurulum yedeği |

Kimlik dosyası kullanıcıya ait `0600`, özel servis dizini `0700` izinlidir. Çalışma DB dosyaları ve servis günlüğü özel izinle oluşturulur. Ham anahtarlar terminal çıktısında veya süreç argümanlarında gösterilmez; DB yalnız anahtar özetlerini tutar. Kimlik ve çalışma verileri `.gitignore` ile kaynak takibinden ayrılmıştır.

İki anahtar da ilk kurulumdan itibaren 30 gün geçerlidir. Süresi dolan veya iptal edilen anahtar açık hata üretir; kurulum kendiliğinden yönetici yetkisini yenilemez. Yenileme, SDBASE'in kimlikli erişim yönetimi API'leriyle yapılır; C erişim akışı sonraki aşamadadır. Anahtarların açık metinlerini belgelere veya kaynak koduna kopyalamayın.

İlk DB önce özel hazırlık dizininde kurulur. Anahtarlar kalıcı dosyaya yazılıp dosya özetleri doğrulanmadan canlı DB yayınlanmaz. Canlı bileşenler mevcut hedefleri değiştirmeden yayınlanır; kimlik dosyası en son gelir. Yayın ortasında kesinti olursa sonraki `init`, `setup-pending.json` günlüğündeki aynı kimlik ve anahtarlarla tamamlar. Çakışan hedef dosyası varsa işlem durur ve hazırlık dosyaları korunur.

`backup`, ilk kurulum anlığını salt okunur DB üzerinden oluşturur ve bütün dosya özetlerini/depo kimliğini doğrular. Aynı yedek mevcutsa üzerine yazmaz, doğrular. Bu yedek sürekli değişen yayın verisinin güncel yedeği değildir. `credentials.json` bu DB yedeğinin dışında özel tutulmalıdır. Yedek/geri yükleme ve anahtar yenilemenin kapsamlı işletim akışları aşama 13'tedir.

## Kabul ve sınırlar

`check`, bu kurulumun servis süreç sahipliğini ve `/health` depo kimliğini karşılaştırır; kimlikli `/v1/capabilities`, `/v1/operations/metrics`, `/v1/operations/integrity` çağrılarıyla şema 4, yetki ve tamamlanmış bütünlük kabulünü doğrular. `status`, çalışan/durmuş/başka servis/sağlıksız durumlarını ayırır. `/health` kimlik istemeyen yerel sağlık yoludur; `/v1` çağrıları erişim anahtarı ister.

Kabul testleri gerçek projeye kayıt eklemez; `build/` altında bağımsız kopya ve ayrı yerel port kullanır. `test_database_setup.py` içindeki 14 gerçek CLI/HTTP testi ve `test_database_recovery.py` içindeki dört kurtarma testi geçti. Gerçek HTTP ile playlist oluşturma, servisi kapatma/yeniden açma ve aynı kaydı okuma; yetkisiz erişimin reddi, ilk kimliklerin korunması, bozuk dosyalar, yol kaçışları ve ilgisiz port dinleyicisinin korunması doğrulanır. Kesilen ilk kurulum aynı kimlik/tokenlarla tamamlanır; çakışan hedef korunur. SDBASE'in mevcut motor kabulü 19/19, sorgu/bütünlük kabulü 33/33 geçti.

Gerçek RadioS veritabanı oluşturuldu; yerel servis kimlikli bütünlük kabulünü geçti ve ilk yedek doğrulandı. Aşama 5 tamamlanmıştır. C ekranlarının gerçek kayıtları okuması/yazması aşama 6, gerçek ses cihazı ve yayın ise daha sonraki aşamalardadır. Bu aşama uç durumunu online veya yayına hazır olarak değiştirmez.

Son derleme katı C uyarılarıyla geçti; GUI kabulü açık CTest **8/8** başarılıdır. Gerçek kurulum ayrıca stop/start/check ile yeniden açıldı; depo/istasyon/uç kimlikleri ve işlem sırası korundu. İlk yedek ikinci çağrıda tekrar doğrulandı. RadioS masaüstü sürümü `0.5.0`'dır.

Linux/POSIX ve Python 3.11+ doğrulandı. Servis yönetimi `/proc` ve Linux pidfd desteği kullanır; Windows/macOS servis kabulü yapılmadı. Süreçlerin kabuk olmadan başlatılması ve sunucu kapanış davranışı Python'un [subprocess](https://docs.python.org/3.11/library/subprocess.html) ve [socketserver](https://docs.python.org/3.11/library/socketserver.html) sözleşmelerini kullanır.
