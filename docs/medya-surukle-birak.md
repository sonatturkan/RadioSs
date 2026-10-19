# RadioS 0.17.1 — Servis bağlantısı ve medya sürükle bırak

Dosya yöneticisinden yerel ses dosyalarını RadioS ana penceresine veya açık
medya kütüphanesine bırak. Ana pencereye bırakmak kütüphaneyi açar; dosyalar
Müzik türünde sırayla içe aktarılır. Tür ve etiketler kütüphaneden düzenlenebilir.
**+ Ses dosyaları** düğmesiyle tür seçerek dosya ekleme de korunur.

Her dosya mevcut C/FFmpeg analizi, SHA-256 doğrulaması, yönetilen medya kopyası
ve kalıcı SDBASE kayıt akışından geçer. Sonuçta başarılı ve başarısız dosya
sayıları gösterilir. Kaynak dosya taşınmaz veya değiştirilmez; içe aktarma
oynatmayı başlatmaz. **İçe aktarmayı iptal et** kalan işleri durdurur;
tamamlanmış kayıtlar korunur.

Tek bırakmada en fazla 256 dosya kabul edilir. Uzak bağlantı, klasör, eksik
dosya, bozuk URI ve aynı partide yinelenen yol reddedilir. İçeriğin gerçekten
çözümlenebilmesi gerekir; dosya uzantısı doğrulama yerine geçmez. Bağlantı yokken,
dosya seçicisi açıkken veya aktarım sürerken ikinci parti başlatılmaz. Pencere
kapandıktan sonra gelen bırakma olayları artık serbest bırakılmış bağlama erişmez.

## Veri servisinin açılışı

Önce çalışan servise kimlikli bağlantı kurulur; depo, şema ve istasyon bağlamı
doğrulanır. Servis erişilemiyorsa yerel başlatıcı devreye girer. SDK içindeki
arayüz, servis yaşam döngüsü işlemini ana sistemde çalıştırır; ana sistem
başlatıcısının süreç sahipliği kontrolü korunur. SDK komutu bu işlem için
`org.freedesktop.Flatpak` erişimini içerir. Yetki, kimlik veya şema hatası,
servisi yeniden başlatmaya dönüştürülmez.

0.17.0 başlatıcısı ana sistem servisini açıyor, ancak GUI kendi PID ortamından
bu servisi tekrar başlatmaya çalışarak yabancı süreç hatası alıyordu. Yeni
bağlantı önce doğrudan kimlik doğrular; eski çalışma biçimini gerçek farklı PID
ortamlarıyla sınayan regresyon testi eklendi. Dağıtım kabulü de yalnız pencere
kilidini ve dışarıdan `/health` çağrısını yeterli saymaz: her gerçek GUI açılışının
kimlikli `desktop/context` isteğinin HTTP 200 sonucu beklenir.

## Doğrulama

Strict C11 Debug ve optimize Release derlemeleri başarılıdır. Yedi etkilenen
CTest hedefinin son sonuçları geçti: `release_install`, `native_data_connection`,
`native_media_drop`, `native_application_shutdown`, `native_controls`,
`native_data`, `native_media`. İlk altı başarılı sonuçla birlikte son bağımsız
`native_data` koşusu (`19,40` saniye) değerlendirilir; 56 hedefin tamamı bu
sürümde yeniden çalıştırılmadı. Birleştirilmiş kanıt
`build/service-drop-verification.json` içindedir. Yeni bağlantı ve bırakma
hedefleri ASan/UBSan altında **2/2**, **6,30 saniyede** geçti; GTK süreç global
verileri nedeniyle leak denetimi bu native koşuda kapalıdır.

Genel veri testlerinin yedi grubu artık bağımsız özel kurulumlarla çalışır;
senaryo içindeki yeniden açılışlar aynı veritabanını kullanır. Keşfedilen her
GLib grubunun tam olarak çalışması ve atlanmaması denetlenir. LIST yarış testi
kendi ön koşulunu native GUI üzerinden kurar ve doğru UUID/revision onayını
bekler. Önceki paylaşılan koşudaki kullanıcı kaydı zaman aşımının kesin nedeni
kanıtlanmadı; üretim kota sınırı veya yazma tekrar davranışı değiştirilmedi.

`native_data_connection` çalışan ana sistem servisine yaşam döngüsü başlatıcısı
olmadan bağlanmayı, gerçek WAV kaydı ve tekrar açılışı, durmuş servisin ana
sistemde başlatılmasını ve yetki hatasında başlatmama davranışını sınar.

`native_media_drop` gerçek GTK URI seçim verisini bırakma işleyicisine teslim
eder. Çoklu Türkçe/boşluklu dosya, SHA ve yönetilen kopya, karışık geçerli/geçersiz
girdi, bağlantısızlık, meşgul durum, iptal, boyut/sayı sınırı ve pencere
kapandıktan sonra geç gelen bırakma olayları ayrı gruplardır. Bu testler dosya
yöneticisinin fare hareketini otomatikleştirmez.

Kullanıcının istediği canlı denemede İndirilenler'deki **Shouse – Won't Forget You
(John Junior, Silviu Andrei & Arty Violin Edit)** MP3 dosyası gerçek native GTK
bırakma sinyaliyle kütüphaneye eklendi. 4.733.939 bayt kaynak ve yönetilen kopya
aynı SHA-256 ile doğrulandı; ölçülen süre 287.417 ms, ses 44.100 Hz stereo.
Kütüphane satırı/ayrıntıları ve yeniden açılışta kalıcılık doğrulandı; oynatma
ve mikrofon başlamadı. Kanıt `build/live-music-validation/result.json` içindedir.
Bu canlı işlem, kullanıcının kendi parçasını kalıcı veritabanına ekler; ayrı
regresyon testleri yalnız özel test veritabanlarını kullanır.
