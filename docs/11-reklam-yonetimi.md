# 11. aşama — Reklam yönetimi

Reklam görünümü native C/GTK bileşenleriyle çalışır; tasarımın kampanya/ayrıntı düzeni ve kaynak CSS aktarımı korunur. Kampanya satış alanları, gerçek spot medyası, trafik rezervasyonu ve çalma sonucu ayrı kayıtlardır. Tasarımdaki örnekler çalışma veritabanına eklenmez.

## Kullanım

1. Medya kütüphanesinden gerçek ses dosyasını **Reklam** türünde içe aktar. Reklam görünümünde kampanyanın satış/sözleşme alanlarını kaydet ve **Spot bağla** ile doğrulanmış medya sürümünü seç. Dosya adı veya elle yazılan süre tek başına oynatılabilir spot oluşturmaz.
2. **Spot onayla** gerçek dosya SHA'sını yeniden kontrol eder; içerik sürümünü, katalog kaydını ve istasyon uygunluğunu onaylar. Kullanım izni için açıkça izin kutusunu seç, gerçek çıkışı ve izin/sözleşme referansını gir. İzin otomatik verilmez; geçerli izin bulunmazsa önizleme rezervasyonu engelleyen nedeni gösterir. **Kampanyayı onayla** ayrı bir satış onayıdır. Askıya alma/devam/arşivleme kampanyanın durumunu CAS ile değiştirir.
3. Yerel MAIN çıkışını açıkça oluştur veya var olan istasyon çıkışını seç. Gün, çıkış ve kuşak saatini belirle. Açılış, kapanış ve sabit anons için gerçek kütüphane dosyaları bağlanabilir; süre dosyanın ölçümünden alınır. Yalnız elle girilmiş bir süre ses dosyasının yerine geçmez. İzin verilen aşım kapasiteye eklenir; önizleme normal kapasiteyi, aşımı ve etkin sınırı ayrı gösterir.
4. **Önizle**, uygun spotları, reddedilme nedenlerini, ölçülmüş ek sesleri ve kalan kapasiteyi gösterir. **Rezerve et**, tam önizleme özeti ve plan sürümü hâlâ geçerliyse atomik trafik rezervasyonu yapar. **İptal et** geçmiş placement'ları korur; yeniden yerleştirme yeni önizleme ve rezervasyon kullanır.
5. **Çal** veya **Takvime al**, önce değişmez medya manifestini veritabanına kaydeder. Ardından gerçek C motoru MAIN kuyruğunu yürütür. Takvim istasyonun kayıtlı saat diliminden UTC'ye çevrilir. Duraklat/devam/durdur gerçek transport işlemleridir. Açılışta reklam oynatması başlamaz.
6. Tarih, seçili kampanya ve çıkış filtreleriyle rapor oluştur. Rapor kimliği ve içeriği değişmez; yeni olayları görmek için yeni rapor oluştur. Tabloda sayfalar arasında geçilebilir. CSV seçili rapor sayfasını; plan dışa aktarımı yüklenmiş kuşak planını native dosya seçicisine yazar. Kısmi CSV dışa aktarımı arayüzde açıkça belirtilir.

## Veri ve ses sözleşmesi

`desktop-advert` iş akışları mevcut SDBASE kampanya, sürüm, spot, yükümlülük, plan, placement ve as-run servislerini kullanır. Kampanya taslağındaki eski yer tutucu sürüm gerçek medya bağı olarak kabul edilmez. Onaylar ve dosya bağları aynı istasyonun gerçek UUID/SHA kimliklerini taşır. Kampanya tarih/saat/günlük adet ayarları gerçek trafik yükümlülüklerine çevrilir.

Önizleme salt okunurdur. Rezervasyon; kuşak revision'ı, plan revision'ı, depo sequence'i, gerçek dosya özeti ve önizleme hash'iyle korunur. Yazılar kullanıcı yetkisini denetler; aynı işlem anahtarının başka gövdeyle kullanılması reddedilir. Çakışma veya bağlantı hatasında form alanları korunur. Kuşak tarihi değiştirilecekse yeni kuşak kaydı gerekir; eski tarihli köprü planı sessizce taşınmaz.

Manifest, sıralı girişleri ve trafik kimliklerini, medya/sürüm/dosya UUID'lerini, SHA'ları, gerçek kare aralıklarını, çıkış/bölgeyi ve zamanlama ayarlarını dondurur. Katalog milisaniye süresi tam dosyanın son karelerini kesmez. Reklam spotu tam doğrulanmış dosyayı çalar; aynı medya için kaydedilmiş müzik cue taslağı reklamı kısaltmaz. Başlatmadan önce dosyalar tekrar doğrulanır.

Spot seçilmeyen kuşakta kaydedilmiş boş kuşak politikası uygulanır. **Açılış/kapanış çalmaz**, yalnız bağlı sabit anonsu; **Kuşak atlanır**, ses kuyruğu oluşturmadan atlamayı; **Açılış/kapanış çalar**, gerçek bağlı açılış/anons/kapanış dosyalarını seçer. Hiç ses kalmazsa kuşak atlanır ve mevcut program sürer. Sıfır spotlu rezervasyon yalnız kuşağın durumunu kaydeder; hayali spot, yükümlülük veya placement oluşturmaz.

Geleceğe kurulan kuşak mevcut programı hazırlık sırasında durdurmaz. C motoruna kurulan geçiş kapısı hedef saatte mevcut öğeyi bitirme, kısa fade ile kesme veya sınırlı beklemeden sonra kesme politikasını uygular. Bitirme politikası ardıl parçanın araya girmesini önler; MAIN tamponunun emeklilik alındısı gelmeden yeni kuyruk kurulmaz. Manuel oynatıcılar ve CUE bu kuyruk tarafından sahiplenilmez. Kaynak/decoder hazırlığı kadar geçiş boşluğu ve cihaz tampon gecikmesi oluşabilir; fiziksel duyulma zamanı olarak UTC kaynak kapısı raporlanmaz.

`advert_local_run`, `advert_report` ve kullanıcıya ait `advert_notifications` kayıtları şema 4'ün yönetilen typed yapılandırmalarıdır. Genel config yazma/silme API'si değişmez manifesti, terminal kaydı veya raporu değiştiremez. C tüketim olayları en çok 64'lük işlem gruplarıyla kaydedilir; yanıt kaybında aynı gövde ve işlem anahtarı korunur. Veritabanı yeniden açıldığında kaydedilmiş manifest, olaylar, rapor ve bildirim okuma durumu kalıcıdır.

## Rapor kanıtları

Rapor, trafik planını, kanonik motor/çıkış gözlemlerini ve **yerel MAIN kaynak tüketimini** ayrı sınıflarla gösterir. C motoru gerçekten tüketilen PCM karelerinden başlangıç, tamamlama, kesilme ve hatayı üretir. Pause terminal sonuç oluşturmaz; Stop gerçek tüketim konumunda kesilme kaydı oluşturur. Tamamlama ancak bütün kaynak aralığı tüketilip son MAIN etiketi emekli olduğunda kabul edilir. Hiç çalınmayan girişlerin tüketimi sıfırdır; terminali alınmamış başlangıç belirsiz kalır.

Yerel sonuçta `delivery_proven: false` kalır. Mixer mute/gain, kaynak saatinin ilerlemesini engellemeyebilir; yerel tamamlanma akustik dinleme, stream, uzak çıkış veya sözleşmenin teslim kanıtı değildir. Yerel kullanıcı olayları endpoint'in kanonik playout olaylarını taklit etmez ve reklamın `accepted_plays` sayacını artırmaz. CSV formül karakterlerini güvenli hücrelere dönüştürür; kimlikler ve kanıt sınıfı raporla birlikte dışa aktarılır.

## Sınırlar ve kaynaklar

Bir çalıştırma en çok 500 giriş, motor geçmişi en yeni 2.000 tüketim occurrence'ı, yerel run en çok 4.000 olay taşır. Aynı anda en çok sekiz saklanmayı bekleyen run tutulur. Rapor sayfası en çok 500 satırdır; yanıtlar mevcut C istemcisi boyut sınırına uyar. Bölge/çıkış rezervasyon kimliği doğrulanır; fiziksel bölgesel ses/encoder dağıtımı aşama 12 kapsamındadır. Uygulama çökmesi veya kapanışı sırasında tamamlanmamış son yazının dayanıklı kurtarılması aşama 13 kapsamındadır.

Ana kaynaklar: `src/ui/advert.c`, `advert_editors.c`, `advert_ui.c`, `playout_ui.c`, `src/playout/planner.c`, `src/audio/engine.c`; başlıklar `include/radios/advert_views.h`, `advert_ui.h`, `audio_runner.h`; veri servisi `database/SDBASE/src/sdbase/desktop_advert.py` ve typed/schema guard'larıdır.

## Kabul

Son strict C11 ve birleşik CTest **28/28**, **317,42 saniye** başarılıdır. Reklam backend'i **22/22**, native reklam **5/5** (**79,77 saniye**), C planlayıcı **18/18**, yürütücü **21 deterministik + 1 gerçek Pulse** geçti. Native reklam ASan/UBSan **5/5**, **122,27 saniye** test uygulaması süresiyle temizdir; GTK global verileri için leak denetimi kapalıdır. Planlayıcı ve yürütücü ASan/UBSan + leak denetimi açıkken temizdir; gerçek Pulse dahil son yürütücü **22/22** geçti. Normal gerçek Pulse yürütücü grubu ayrıca başarılıdır.

Testler `build/` altında ayrı veritabanı ve sanal ses cihazları kullanır; mevcut kullanıcı veritabanına test kampanyası veya medya eklenmedi. Standart servis rate limiti korundu; reklam commit'leri yalnız ilgili reklam verilerini yeniler. Kaynak CSS 30 dosya/1.552 native kuralla, envanter 201 workflow/251 yol/256 HTTP işlemi/170 backend kaynak dosyasıyla doğrulandı.

Gerçek veri servisi ve **0.11.0** C penceresi açıldı. Depo/istasyon/uç UUID'leri, şema 4, sequence **2**, bütünlük **ok** ve üretim koleksiyonları değişmedi. Reklam overview ve boş program prepare **HTTP 200** verdi; kimlik dosyası 0600, tek normal pencere/MAIN akışı açık, fixture ses modülleri temizdir.
