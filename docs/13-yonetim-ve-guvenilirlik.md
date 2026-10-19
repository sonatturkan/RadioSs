# 13. aşama — yönetim ve güvenilirlik

RadioS 0.13.0 yerel C/GTK uygulamasıdır. Tasarımın yedi görünümü, 30 CSS dosyası ve 1.552 native kuralı korunur. Bu aşama gerçek kullanıcı/yetki yönetimini, izlemeyi, dayanıklı veri işlemlerini, ses bağlantısı kurtarmasını ve kurulumun yedek/geri yüklemesini bağlar.

## Arayüzde kullanım

**Teknik yönetim → Kişiler:** mevcut kullanıcıların gerçek istasyon izinleri ve erişim anahtarlarının süre/iptal durumu görüntülenir. Düzenle penceresi ad, aktif/devre dışı durumu ve dokuz izin için güncel revision ile kayıt yapar. Başka oturumdaki değişiklikte form girdileri korunur; eski revision yeniden yazılmaz. Kendi yönetim oturumunu kapatma, kendi kullanıcı anahtarını iptal etme ve son kullanılabilir yöneticiyi kaldırma reddedilir. Global veya birden çok istasyona bağlı kullanıcı tek istasyon ekranında daraltılamaz. Yeni kullanıcı anahtarı mevcut özel dosya teslim akışını kullanır; anahtar ekrana veya günlüğe yazılmaz.

**Teknik yönetim → Sunucular → İzleme ve kurtarma:** gerçek servis/depo/şema/sequence, disk alanı, sınırlı DB/medya bütünlük sonucu, MAIN aygıtı, mikrofon ve kayıt/stream durumu gösterilir. Açık panel yenilenir; kapalı panel ek HTTP okumaları üretmez. İşlem kaydı yalnız istasyona ait işlem metadata'sını gösterir, kullanıcı anahtarlarını veya bütün audit gövdelerini göstermez.

Dayanıklı işlem kuyruğu bekleyen ve inceleme gereken işlemleri ayrı listeler. **Bekleyen işlemleri tekrar dene** aynı UUID, gövde ve revision ile çalışır. Yetki/validation/CAS nedeniyle reddedilmiş kayıt otomatik yeniden yazılmaz. Reddedilen dosyalar `data/recovery/<store_uuid>/<user_uuid>/` içinde inceleme için kalır; bunları değiştirmek yeni bir işlem oluşturmak değildir.

**Kesintiyi uzlaştır:** süreç kaybı sonrası terminali alınmamış yayın/reklam oturumunu açıkça seçin, artık çalışmadığını doğrulayın ve kesinti kaydını kapatın. Başka açık uygulamanın canlı yürütmesini kapatmayın. Orijinal manifest ve ölçülmüş kare/byte olayları değiştirilmez. Ayrı değişmez kurtarma kaydı son ölçülmemiş bölümü `unknown_tail: true` olarak tutar; yeni çalışma olayları ve eski manifest ile yeni yürütme engellenir. Kapanma uzak dinleyici, anten veya reklam sözleşmesi teslimi üretmez.

**Yedekler:** listeleme, yeni yedek, hash doğrulama, geri yükleme planı ve ayrı hazırlık kopyası oluşturma çalışır. Hazırlık kopyası canlı DB'ye uygulanmaz (`activated: false`). Gerçek geri yükleme uygulama ve servis tamamen kapalıyken yerel yardımcıyla yapılır.

## Dayanıklı C veri işlemleri

`src/core/recovery.c` ve `data_client.c` kabul edilen idempotent yazı gövdesini HTTP gönderiminden ve GUI iş kuyruğuna girişten önce özel dosyaya yazar. Geçici dosya → fsync → atomik rename → dizin fsync kullanılır. Dosyalar 0600, kurtarma dizinleri 0700'dür. Yol bileşenleri ve dosyalar symlink izlenmeden açılır; özel sahiplik, normal dosya, tek hardlink, JSON biçimi, boyut, identity, SHA-256 zarf özeti ve tam gövde eşitliği denetlenir. Geçerli JSON içindeki hasarlı bir alan da gönderilmez. Hash hasar denetimidir; kriptografik imza değildir. En fazla 512 canlı işlem ve istek başına 2 MiB sınırı vardır. Bozuk/yabancı dosya korunur ve kurtarma reddedilir.

Zarf depo, kullanıcı, istasyon ve uç kimliğine bağlıdır. HTTP authorization başlığı, erişim anahtarı veya stream şifresi saklanmaz. Gizli anahtar alanı/değeri reddedilir. Yeni kullanıcı için bir kez verilen anahtar oluşturma bu genel kuyruğa alınmaz; mevcut backend'in özel anahtar dosyası/alındısı kullanılır. Profil, metadata, medya import, playlist, reklam ve yayın runtime yazıları ile yönetim düzenleme/iptal/uzlaştırma exact alındı sözleşmesini kullanır.

Servisin yanıtı kaybolursa başlangıçta ve bekleyen kayıt varken sınırlı aralıkla aynı istek tekrar doğrulanır. Önce beklenen depo/şema ve gerçek authenticated kullanıcı/istasyon kontrol edilir. Ortam anahtarı başka geçerli kullanıcıya aitse o kullanıcının ayrı kuyruğu açılır; önceki kullanıcının işlemleri gönderilmez. Başarı alındısı dosyayı kaldırır. 403/404/409/422 işlemi inceleme durumuna geçirir; otomatik revision değiştirme yapılmaz. Aynı belge veya çalışma kaydının önceki işlemi reddedilmişse sonraki bekleyen işlemi de tutulur (`dependency_blocked`); başka bir oturumun ulaştığı revision yanlışlıkla ezilmez. Bağımsız kaynaklar sonraki geçişte kurtarılabilir. Rate limit ve ulaşım kesintisinde gövde korunur. GUI bağlantı nesnesi main thread'de değiştirilir; her arka plan işi kendi sabit istemci referansını tutar.

Normal kapanış gerçek runner makbuzlarını yakalar; reklam batch'lerinin ve yayın terminalinin önceki alındıdan sonraki CAS isteği de dayanıklı kuyruğa yazılır. Yeni profil/geçmiş belgesinin UUID'si C istemcisinde oluşturulur; ilk yazı revision 0, ardılı revision 1 ile aynı belgeye bağlanır. Backend ilk UUID'nin boş olduğunu atomik doğrular. Böylece ilk çalma geçmişi yazısının yanıtı henüz gelmemişken bekleyen son checkpoint de korunur. Ani süreç/enerji kaybından önce son dayanıklı yazıya ulaşmamış PCM veya yalnız RAM'deki kullanıcı girdileri yeniden üretilmez; bu son bölüm belirsiz kalır.

Kurtarma veri tutarlılığını toparlar. Uygulama yeniden açıldığında oynatma, kayıt, stream, CUE ve mikrofon kendiliğinden başlamaz. Program/otomasyon mevcut kalıcı geçmiş ve doğrulanmış medya üzerinden operatör komutuyla tekrar hazırlanır.

## Ses bağlantısı kaybı

C Pulse backend aynı yapılandırılmış sunucuya 1/2/5 saniye sınırlı aralıklarla yeniden bağlanır; sunucu otomatik oluşturulmaz. Adlandırılmış çıkış bulunmazsa başka çıkışa düşülmesi reddedilir. Ses sunucusu veya gerçekten hataya düşen bus kaybında son doğrulanmış tüketim konumu korunur, önceden tamponlanan PCM temizlenir, oynatıcılar/runner duraklatılır ve mikrofon kapanır. Aygıt hazır olduktan sonra operatör oynat/devam komutu verir.

`loss_epoch` gecikmiş medya/transition/sampler/acil CUE yanıtlarını da sınırlar; eski yanıt yeniden bağlanmış aygıtta sesi başlatamaz. Stream sunucusu yeniden bağlama bu otomatik ses aygıtı toparlanmasının parçası değildir; mevcut explicit stream stop/start ve oturum şifresi kullanılır.

## Yedek ve geri yükleme komutları

Komutları proje kökünde çalıştırın:

```sh
python3 tools/radios_reliability.py --project-root . list
python3 tools/radios_reliability.py --project-root . create
python3 tools/radios_reliability.py --project-root . verify --backup YEDEK_ADI
python3 tools/radios_reliability.py --project-root . restore-plan --backup YEDEK_ADI
python3 tools/radios_reliability.py --project-root . restore-stage --backup YEDEK_ADI
```

Snapshot motor kilidi altında tutarlı DB ve referanslı gerçek medya objelerini alır. Kurulumun özel kimlikleri, geçerli INI ve varsa görünüm tercihi hash ile bağlanır. Klasör arşivi kullanılır; tar/zip çıkarma yoktur. Eksik/fazla dosya, bozuk hash, symlink, traversal, yabancı depo/kurulum ve farklı yollar reddedilir. Hash hasar denetimidir; üçüncü kişinin değiştirdiği yedeğin kriptografik imzası değildir. Yedek anahtar içerdiği için özel dosya/dizin izinleri korunmalıdır.

Yedek bu kurulumun geri yüklenmesi içindir. Yeni kullanıcılara ayrı teslim edilen `private-user-keys` dosyaları snapshot'a eklenmez; mevcut kurulumda geri yükleme bunları silmez. Kullanıcının kendisine teslim edilen erişim anahtarının ayrıca korunması gerekir. Snapshot sonrasında oluşturulan veya iptal edilen anahtarın durumu geri yüklenen DB ile yeniden değerlendirilir.

Uygulamayı kapatıp sahip olunan servisi durdurduktan sonra, planın gösterdiği gerçek depo UUID'sini kullanın:

```sh
python3 tools/radios_database.py --project-root . stop
python3 tools/radios_reliability.py --project-root . restore --backup YEDEK_ADI --confirm-store-id DEPO_UUID --offline
```

Yabancı süreç/port, açık GUI ve kullanılan servis/DB/GUI/outbox kilitleri korunur; yardımcı başka süreci durdurmaz. Gerçek GUI yaşamı ortak `gui.lock`, offline geri yükleme özel kilit tutar. Geri yükleme journal'i varken servis/kurulum/GUI açılamaz. Staged DB, medya, credentials ve ayarlar atomik bileşen değişimleri ve dayanıklı rollback journal'i ile uygulanır. Araya enerji/süreç kaybı girerse:

```sh
python3 tools/radios_reliability.py --project-root . recover --confirm-store-id DEPO_UUID --offline
```

Tamamlanmamış değişimler özgün dosyalara döner; sonradan yapılmış yabancı düzenleme silinmez. Tamamlanmış işlem geri alınmaz. Motor restored endpoint/komutlarını kapatır; sadece yerel kurulum ucu için yeni özel token hazırlanır. Depo/istasyon/uç/kullanıcı UUID'leri korunur. Tarihsel desktop runtime oturumları ayrı fence kaydıyla yeniden çalıştırılamaz; ölçülmüş orijinal terminal kanıtı korunur. Servis ve ses otomatik başlatılmaz.

Canlı `data/recovery` snapshot içine alınmaz. Snapshot tarihinden sonraki işlemlerin geriye alınmış DB'ye tekrar uygulanmasını önlemek için mevcut store kuyruğu ayrı özel `data/database-service/restore-outbox-<transaction_uuid>` klasöründe korunur. Hatalı restore'da özgün kuyruk da geri gelir.

## Sahipsiz medya incelemesi

`media-audit` referanslı SHA'ları ve bütün kullanıcılardaki pending/blocked import yollarını koruyarak sınırlı envanter üretir. `media-cleanup` güncel audit hash'i, depo UUID'si ve offline koşulu ister. Yalnız en az 24 saat eski, özel kullanıcıya ait, doğrulanmış native import/transition dosyaları ve gerçekten referanssız yönetilen objeler ayrı manifestli karantinaya taşınır. Genç, değişmiş, bilinmeyen veya link dosyaları korunur. Byte silme yapılmaz.

```sh
python3 tools/radios_reliability.py --project-root . media-audit
python3 tools/radios_reliability.py --project-root . media-cleanup --audit-id AUDIT_SHA256 --confirm-store-id DEPO_UUID --offline
```

## Kabul ve sınırlar

Özel DB'lerde gerçek izin/CAS/anahtar iptali ve son yönetici koruması; HTTP commit sonrası socket yanıt kaybı, süreç yeniden açılışı, foreign authenticated kullanıcı reddi; kapanırken bekletilen ACTIVE alındısının ardından gerçek MAIN terminalinin kurtarılması; gerçek medya içeren tutarlı snapshot, staged ve offline restore, crash/rollback, outbox quarantine ve sahipsiz medya koruması sınanır. Native GTK kabulü mevcut tasarımı ve owner kapanışı güvenliğini içerir. Ses kabulü yalnız özel daemon ve sanal cihazlarla çalışır; host ses servisi kapatılmaz.

Fiziksel donanım, uzun yayın süresi ve temiz kurulum/paket kabulü 14. aşamadır. MFA/USB giriş, gerçek GPIO/TTS/bağımsız bölgesel fiziksel çıkış, site/SaaS servisi ve anten/akustik teslim bu yerel yönetim/kurtarma kanıtının kapsamı değildir. Tasarımın bu alanlardaki hazırlık profilleri böyle bir yürütme varmış gibi gösterilmez.

Strict C11 ve birleşik CTest **36/36**, **242,54 saniye** başarılıdır. Dayanıklı kuyruk **10/10**, C veri istemcisi **15/15**, backend yönetim **26/26** ve birleşik desktop regresyonu **108/108**, yedek/kurtarma **18/18**, native yönetim **6/6** ve native yayın/kapanış kurtarması **5/5** geçti. Son bağımlılık açıklaması sonrasında normal yönetim altı grubu ayrıca **9,87 saniyede** başarılıdır. API envanteri **211 workflow / 261 yol / 266 HTTP işlemi / 172 toplam kaynak dosyası (72 tasarım + 100 Python modülü)** ile doğrulandı; kaynak CSS **30 dosya / 1.552 native kural** korunur.

Core outbox/istemci ASan/UBSan ve leak detection açıkken **2/2 hedef** (**5,58 saniye**) temizdir. Native yönetim **6/6** ve yayın/kapanış kurtarması **5/5** (**96,43 saniye CTest**) ASan/UBSan altında geçti; GTK global kaynakları için leak detection kapalıdır. Üç gerçek özel Pulse yeniden başlatmasını içeren ses kurtarması normal **1/1** (**18,50 saniye**) ve ASan/UBSan/leak açık **1/1** (**18,35 saniye**) başarılıdır. Birleşik normal koşu da ses kurtarmasını atlamadan çalıştırdı.

Güncel **0.13.0** C penceresi ve veri servisi mevcut çalışma veritabanıyla açıldı. Depo/istasyon/uç/kullanıcı UUID'leri, şema 4, sequence **2**, altı üretim koleksiyonunun sayıları ve bütünlük **ok** korundu. Gerçek yönetim, reklam ve yayın overview API'leri **HTTP 200** verdi. Üretim DB'sine fixture yazılmadı; yalnız normal MAIN akışı açık, mikrofon capture ve test cihazı/sunucusu kalmadı. Akustik sessizlik veya fiziksel teslim ölçümü bu kontrolün iddiası değildir.

Gerçek kurulum yedeği **`data/backups/radios-20261006T082436Z-f90a3ffe`** oluşturuldu ve tekrar doğrulandı: sequence 2, bütünlük ok, 3 engine dosyası, 9.641 byte. Mevcut kütüphane boş olduğu için bu yedekte medya objesi yoktur; medya içeren backup/restore özel testlerde gerçek PCM ile kabul edildi. Gerçek çalışma DB'sine geri yükleme uygulanmadı.

Kanıtlar: `build/stage13-full.xml`, `build/stage13-admin-final.xml`, `build/asan/stage13-core-asan.xml`, `build/asan/stage13-broadcast-asan.xml`, `build/stage13-backup-verification.json` ve `build/stage13-release-verification.json`. Ayrıntılı test sözleşmesi [test belgesindedir](../tests/README.md). Sıradaki aşama **14: nihai kabul ve paketleme**.
