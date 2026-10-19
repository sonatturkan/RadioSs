# Native program onayı ve kullanım hakları

2026-10-07, güncelleme 2026-10-17. Program onayı, içerik onayları ve kullanım
hakları **Teknik yönetim › Program onayı** sekmesindedir. Sekme, Yayın
sayfasında o an seçili yayın sırasının (genel sıra ya da bir oynatıcının sırası)
adını gösterir ve işlemleri onun için çalıştırır; seçili sıra değişince kendiliğinden
izler. **Yayın ve medya kullanım hakları…** düğmesi de oradadır. Yayın
sayfasındaki program şeridinde yalnız program adı ve tarihi kalır; eski **⋯**
menüsü ve yayın sırasının altındaki eski program bloğu kaldırılmıştır.
Kalıcı native taslak, içerik sürümü onayı, kullanım hakları,
formal program onayı, yerel ses ve uç dağıtımı ayrı işlemlerdir.

## Operatör akışı

1. Gerçek medyayı kütüphaneye alın; programın her ses öğesine UTC olarak
   geçerli bir başlangıç saati verin. Programı kaydedin ve DB kayıt onayını
   bekleyin. UTC saatleri formal plana aynı an olarak aktarılır. İstasyon
   saatinde farklı hizmet tarihine düşen öğe bu köprüde reddedilir. Bu
   durumda istasyon hizmet tarihini açıkça belirleyen ayrı formal uç planı
   kullanın; native tarihi değiştirmek UTC ofset farkını gidermez.
2. **Yerel onaya gönder** kayıtlı belgeyi normal SDBASE planına ve taslak
   revizyonuna dönüştürür. Bu işlem içerik veya hak onayı vermez.
   **Kesin saat** kes/bitir ayarı olan taslak formal gönderimde reddedilir;
   olayın kes/bitir veya sınırlı bekleme ayarı da sessizce dönüştürülmez.
   Özel native marker taslağı formal cue onayı değildir; çalmayı değiştiren
   taslak formal gönderimde reddedilir. Mevcut native taslak/saatli oynatma
   veya açıkça onaylanmış canonical cue ve ayrı formal uç planı kullanın.
3. **İçerik onayları ve kullanım hakları** penceresindeki medya sürümünü seçin.
   Gerçek sürüm, içerik yaşam döngüsü, istasyon uygunluğu ve mevcut hak
   kayıtları gösterilir. Kullanacağınız kararı kontrol edip ayrı ayrı
   **Sürümü onayla**, **İçeriği etkinleştir**, **İstasyona uygun yap** işlemlerini
   uygulayın. Yetki ve `_rev` kontrolleri korunur.
4. Hak kararında bölge, kayıtlı çıkış, açık UTC ofsetli başlangıç/bitiş ve
   sözleşme/izin dayanağını doldurun. **Hak kararını kaydet** seçilen izin veya
   yasak kararını gerçek hak kaydı olarak yazar. Başlangıç ve bitiş tüm gereken
   program aralığını kapsamalıdır. Kayıtlı çıkış yoksa açık operatör işlemi
   **Yerel MAIN çıkışını kaydet** bu istasyonun yerel ucuna `LOCAL_MAIN`
   tanımlar. Fiziksel aygıt/anten teslimi kurulmuş sayılmaz.
5. **Değişiklikleri incele** gerçek revizyonun ses öğelerini, durumlarını ve
   hazır olmayı engelleyen koşulları okur. **Programı onayla** korunmuş SDBASE
   doğrulamalarından geçerek değişmez bir program revizyonu oluşturur.
6. **Onaylı sırayı yerel çal** onaylı snapshot'ın sürüm, dosya SHA ve formal
   cue değerlerinden en fazla 500 öğelik gerçek native MAIN ses sırasını
   başlatır. Hem `activate` hem
   `operate` izni gerekir. Kaydedilmiş native belge onaylı kaynakla eşleşmelidir.
   Bekleyen yanıt sırasında Stop, belge değişikliği/kaydı veya pencere kapanışı
   eski sıranın başlamasını engeller.
7. Onaydan sonra düzenleme için **Yeni taslak aç** kullanın; önceki onaylı
   revizyon korunur. Değişen native taslağı yeniden gönderip onaylayın.

## Dağıtım ve yürütme sınırı

**Uca dağıt** seçili gerçek uç için SDBASE dağıtım kaydını kuyruğa alır.
`activate` izni gerekir. Kuyruğa alma uç kabulü, heartbeat, anten sesi,
reklam teslimi veya dinleyiciye ulaşma kanıtı değildir. Bu sonuçlar ilgili uç
adaptörü ve bağımsız gözlemle doğrulanmalıdır.

Yerel yükleme operatörün seçtiği anda sıralı ses yürütür. Formal snapshot'taki
UTC slotları ve istasyon hizmet tarihi değişmez kayıtta korunur; bu düğme uçtaki
mutlak zaman yürütücüsü değildir. Ses dışı olay, prodüksiyon yerleşimi,
crossfade, frozen fade zarfı, ad-break veya farklı zaman semantiği içeren plan
yerel adaptörde reddedilir; korunmuş uç yürütme API'si gerekir.

Taslak gönderimi aynı anahtar/gövde alındısıyla dayanıklı kurtarma kuyruğuna
alınır. Sürüm, yaşam döngüsü, uygunluk ve hak yazıları bilinmeyen bir yanıt
sonrasında kör tekrar edilmez. **Kayıtları yenile** ile gerçek sonucu inceleyin.
Bu pencere hukuki bir izin üretmez; operatörün açık kararını kaydeder.

## Doğrulama

`desktop_programme` on iki özel gerçek Store/HTTP senaryosunu içerir:
atomik submit/replay, eksik ön koşullar, SHA/frozen marker doğrulaması,
ayrı dağıtım, immutable fork/reopen, yetki/CAS, read-only koşullar, UTC
anının korunması, hizmet tarihi sınırı, desteklenmeyen cut/finish/bekleme ve
özel marker reddi, 501 öğelik gerçek onaylı planın yerel sınırı.
`native_programme` özel DB/HTTP ve sahip olunan Pulse MAIN/CUE/MON çıkışlarıyla
gerçek GTK kontrollerini çalıştırır: taze import, açık çıkış/sürüm/uygunluk/hak
kararları, program onayı, ölçülmüş PCM, bekleyen yanıt sırasında pencere
kapanışı, native edit/save sonrası eski yanıtın bastırılması ve fork/resubmit.
Test kapıları yalnız geçici backend kopyasında bulunur.

Uygulama: `src/ui/programme_lifecycle.c`, `src/ui/programme_prerequisites.c`,
`database/SDBASE/src/sdbase/desktop_programme.py`. Şema değiştirilmedi;
orijinal plan/onay/snapshot/hak motorları kullanılır. Bu kabul bütün DB API'leri
için ayrı native ekran bulunduğunu veya fiziksel/uzak saha kabulünün tamamlandığını
göstermez.
