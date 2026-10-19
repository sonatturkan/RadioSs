# Prompt 13 — Otomasyon takvimi

Kaynak: `../prompt/13/13.pdf`, sayfa 1 ve `figures/sema_1.png`. Tam kaynak
envanteri ve 22 kabul ölçütü [requirements.md](requirements.md) dosyasındadır.
Önceki koordinasyon kayıtları [coordination.md](coordination.md) ve ortak
`../prompt/agent/AGENT.md` P13 bölümlerinde korunur. Kullanıcının son talimatıyla
AGENT.md yazımı durdurulmuştur; bu değişikliğin kanıtları yalnız yerel dosyalardadır.

## Kullanım

Kullanıcının 2026-10-10 tarihli açık düzen talimatına göre otomasyon ekranında
yedi günlük saat takvimi solda, gelişmiş DJ ayarları doğrudan sağda görünür.
Seçili program düzenleyicisi takvimin altındadır; ayrı ayarlar düğmesi veya
AutoDJ ayar penceresi yoktur. Gün başlıkları kaydırma sırasında sabit kalır.
Hücreleri fareyle sürükleyerek veya ok/Shift tuşlarıyla seçin; takvimin altındaki
kompakt araç satırı seçimin gerçek tarih ve saat aralığını gösterir. `+` ve `−` zaman ölçeğini 15/30/60 dakikaya değiştirir.
**Seçileni kaydet** program adını ve açık mavi/yeşil rengini sorar. Programın
yüksekliği kendi zaman aralığına bağlıdır; 24:00 gün sonu desteklenir.

Bloka tıklamak takvimin altındaki program düzenleyicisini gösterir. Program adı/renk düzenlenebilir.
**Rotasyon tanımla** program düzenleyicisinin alt sağındadır; kayıtlı playlist, sıklık ve
dört ayrı rastgelelik ayarı seçilir. Onaylanan rotasyon hemen satır olarak
eklenir. Satırlar sürükleme veya oklarla sıralanır, çoklu seçilip başka programa
kopyalanabilir. Ctrl ile birden fazla takvim eventi seçilip diğer gün/saat
aralığına kopyalanabilir. Kaynak event ve rotasyonları korunur; kopyaların bütün
kimlikleri yenidir. Çakışan çoklu yapıştırma hiçbir kısmi kayıt oluşturmaz.

**Önizle**, playlist kaynaklarını mevcut authenticated API ile hazırlar;
rotasyon ayarlarıyla gerçek medya sırasını üretir; ardından program medyasını
aynı API ile yeniden doğrular ve mevcut native planlayıcıdan geçirir. Önizleme
ses başlatmaz. **Yayın sırasına ekle** doğrulanmış sonucu mevcut programa ekler;
**Zamanla** gelecekteki seçili UTC başlangıcı için mevcut yerel yürütücüyü
kullanır. Program veya veri oturumu değişirse eski hazırlık kullanılamaz.

Sağdaki **Gelişmiş DJ** alanı önceki rotasyon, deck, geçiş, geçmiş, önizleme
ve taşıma araçlarının tek gerçek örneğini doğrudan gösterir. Formun kendi
Rotasyon, Saat ve geçiş, Deck ve güvenlik bölümleri korunur. Alan bağımsız
kaydırılabilir; dar sütuna uyması için ayarlar dikey düzenlenmiştir. Takvimin
altındaki **Takvimi kaydet** düğmesi bağlantı kesildikten sonra da taslağı
yeniden kaydetme yolunu korur. Kaydedilmiş takvim uygulama açıldığında
kendiliğinden ses başlatmaz.

## Açık kaynak kararları

- Şemadaki tarih sütunları ve dikey saat ekseni kullanılır. Metindeki çelişkili
  “dikey tarih” ifadesi, gün/saat üst bilgisini kaldırmak için kullanılmaz.
- Kaynak iki alanın yönünü belirtmez. 2026-10-10 kullanıcı düzeltmesiyle
  takvim solda, gelişmiş DJ sağda; program düzenleyicisi takvimin altında
  yerleştirilmiştir. Bu açık talimat önceki yatay takvim/program düzenini değiştirir.
- Sıklık **her rotasyon turundaki parça adedidir** (1–100). Rotasyon rastgeleliği
  o satırın turdaki yerlerini dağıtır; sabit satırların göreli sırası korunur.
- Playlist içi, reklam ve sweeper sırası bağımsız havuzlar üzerinden seçilir.
  Bir havuz tükenmeden kendi içeriği yeniden kullanılmaz. Playlistin tür
  konumları korunur; başka türün rastgelelik ayarı bu havuzun sırasını değiştirmez.
- Programlar aynı UTC gün içinde yarı açık zaman aralıklarıdır. Bitişik bloklar
  geçerlidir, gerçek çakışma reddedilir. Kopyalama hedefin başlangıcına göre
  aynı süreleri ve eventler arasındaki farkları korur.
- Mevcut API'nin 200 öğe hazırlama sınırı korunur. Hazırlanan tam medya süreleri
  seçilen blok sonunu aşmaz. Bu hazırlık fiziksel/anten teslim kanıtı değildir.

## Veri ve dosya sınırları

Yeni `automation_calendar` belge türü `schema: 1` ve `events` içerir. Bir event
UUID, ad, UTC tarih, başlangıç/bitiş dakikaları, renk ve sıralı rotasyonlar taşır.
Takvimde en fazla 500 program, programda 50 rotasyon bulunabilir. Backend tam
tipleri, kimlik tekilliğini, istasyon/playlist/yetki sınırlarını ve çakışmaları
doğrular. Kayıtlar mevcut CAS/revision, idempotent alındı ve dayanıklı veri
işlemleriyle saklanır. Bağlantısız kullanım açıkça oturum taslağıdır.

Uygulama dosyaları `src/ui/automation_calendar.c`,
`src/core/automation_calendar_plan.c` ve iki public başlıktır. Önceki
`src/ui/automation.c` yeni takvimi açar; eski araçların kaynaklarını korur.
Backend ayrı `desktop_automation_calendar.py` doğrulayıcısıyla ve
`desktop_api.py` içindeki iki küçük entegrasyonla bağlanır.

## 2026-10-10 düzen değişikliği

Yeni düzen strict Release olarak `build/prompt13-layout-20261010` içinde
derlendi. Güncel derleme kaydı
[build-final-acceptance.json](../../build/prompt13-layout-20261010/build-final-acceptance.json)
ana uygulama ve iki native test hedefinin exit 0 sonucunu, altı izlenen
kaynağın derleme öncesi/sonrası eşitliğini içerir. Program düzenleyicisinin
alt öğeleri açıkça görünür başlatılır; koşullu görünürlük yalnız kök editörde
uygulanır. Yeni scoped CSS `index.html` bağlantısı üzerinden native temaya
dahil edilir. Ayrıntılı kaynak yedekleri `baseline/20261010T232710Z`,
`baseline/20261010T232919Z` ve `baseline/20261010T233115Z` altında saklanır.
Güncel kaynakla gerçek veritabanı kullanan **3/3 native senaryo geçti**;
atlanan test yoktur. Program kaydı, çoklu kopyalama, rotasyon önizlemesi ve
sağdaki gerçek DJ formunun bağımsız kayıt/yeniden açma davranışı doğrulandı.
Koşu öncesi/sonrası 21 kaynak ve executable hash eşitti; özel test ekranı ve
veritabanı temizlendi.
[Native kabul kaydı](../../build/prompt13-layout-20261010/acceptance/native-inline-20261010T233423Z/native-acceptance.json)
ve [gerçek inline form görüntüsü](../../build/prompt13-layout-20261010/acceptance/native-inline-20261010T233423Z/calendar-inline-real-settings.png)
kanıtları korunur. Bu ilk görüntüde 18:00 programları görünür zaman aralığının
altındadır; renkli blokların görsel kabulü bu görüntüye atfedilmez.

Varsayılan `build/radios`, koordineli P2 kurulumu ile bu dört kaynak değişikliğini
içerir. Kurulu executable SHA256:
`5cec467b6a539af8c32f8ac9ae67a82e89cc10035262341db759e244ca666ea3`.
[Kurulum makbuzu](../../build/direct-provider-delivery-20261010/install-receipt.json)
ve [bağımsız kaynak dahil etme kontrolü](logs/2026-10-10/23-build-launch-evidence/p2-source-inclusion.json)
mevcuttur. Canlı varsayılan uygulama host PID57201 olarak, XRes ile
`0x1a00004` penceresine ve aynı executable hashine bağlandı. Gerçek varsayılan pencere açıktır. Otomasyon sekmesini otomatik seçme girişimi
Wayland/XTest ve SDK erişilebilirlik sınırı nedeniyle doğrulanamadı; pencere
Yayın ekranında kaldı. Kullanıcı Otomasyon sekmesini normal olarak seçebilir.
İlk default-window-navigation.json ve automation adını taşıyan ilk default
PNG yalnız giriş denemesini kaydeder; doğru otomasyon ekranı kabulü değildir.

Geniş görüntü için yalnız test geometrisi güncellenirken mevcut ağacın başka
oturumlara ait yeni provider API değişimi ara derlemeyi link aşamasında
engelledi. Başarısız deneme
[ayrı derleme kaydında](../../build/prompt13-layout-20261010/capture-rebuild-acceptance.json)
korunur; yukarıdaki başarılı native koşu veya kurulu ürün değişikliğiyle
karıştırılmaz. Ürün kaynakları bu ek görüntü denemesinde değiştirilmedi.

Ek geniş görüntü kabulü başarıyla tamamlandı: kabul edilmiş P2 arşivleriyle
yalnız görüntü geometrisi güncellenmiş test ayrı executable olarak bağlandı;
**3/3 senaryo yeniden geçti**, girdiler ve arşivler sabit kaldı, özel ekran
temizlendi. Bu yöntem değişen bütün kaynak ağacının yeniden derleme kabulü
değildir.
[1920×1042 gerçek görüntü](../../build/prompt13-layout-20261010/acceptance/wide-capture-20261010T233819Z/calendar-wide-real-settings.png)
ve [geniş görüntü kabulü](../../build/prompt13-layout-20261010/acceptance/wide-capture-20261010T233819Z/wide-capture-acceptance.json)
yedi tarih sütununu, süreleri farklı açık mavi/yeşil programları, sağdaki
inline DJ ayarlarını ve takvim altındaki program editörünü gösterir. Root
görüntüyü doğrudan inceledi. Düzen değişikliği, kurulum ve anlamlı işlev
testleri tamamlandı; otomatik sekme seçimi sınırlaması açık kaydedildi.

## 2026-10-09 önceki doğrulama

Strict C11 (`-Wall -Wextra -Wpedantic -Werror`) ana uygulama ve kabul hedefleri
`build/prompt13` içinde derlendi. Backend 12 takvim, 23 veri API ve 15 playout
senaryosu başarılıdır. Yeni core adapter 6 senaryo ve kendi ASan/UBSan denetimini
geçti; sanitizer yalnız yeni adapter/test kaynaklarını kapsar. Native controls
10 senaryo ve önceki iki gerçek DB/HTTP/Pulse AutoDJ regresyonu geçti.

Ana uygulamanın son strict derlemesi **02:27:09–02:27:10 UTC** aralığında
exit 0 verdi; komut, çıktı ve executable SHA
[derleme kaydında](../../build/prompt13/production-build-final.json) saklandı.

Son kaynakla native takvim kabulü **2026-10-09 02:26:46–02:26:49 UTC**
aralığında **1/1 CTest hedefi, 2/2 GLib senaryosu, 2,52 saniye PASS** verdi;
hiçbir test atlanmadı. Kabul gerçek authenticated DB kayıt/teyit/yeniden açma,
çoklu kopyada yeni kimlik ve atomiklik, sıralama, dört rotasyon ayarı, eski
araçların kapatılıp açılması, yakınlaştırmanın koordinatları koruması ve
9999 yılı sınırındaki haftayı kapsar. Rotasyon onayı sonrası satırın herhangi
bir asenkron işlem beklenmeden eklenmesi ve rotasyon iptalinde satır/model
değişmezliği doğrudan doğrulandı. Gerçek iki saniyelik doğrulanmış PCM
medyası iki hazırlama isteğinden geçirilerek bir dakikalık blokta **30 öğe**
üretir; blok sonu açıklaması gösterilir. Önizleme veritabanı revision'ını
veya takvimi değiştirmez; program değişikliği eski aktarımı geçersiz kılar.

Kaynak ve test executable için 13 hash'in koşu öncesi/sonrası değişmediği
[native kabul kaydında](../../build/prompt13/native-calendar-final-acceptance.json)
belgelenmiştir. Ayrıntılı TAP çıktısı
[koşu günlüğünde](../../build/prompt13/p13-native-calendar-final.log), CTest
sonucu [JUnit dosyasında](../../build/prompt13/p13-native-calendar-final.xml)
bulunur. Önceki hedefli regresyonun
[JUnit kanıtı](../../build/prompt13/p13-regression.xml) ayrı korunur.

[Son gerçek GTK görüntüsü](renders/native-calendar.png) ana ajan, test ajanı
ve koordinatör tarafından kaynak şemayla karşılaştırıldı: tam görünen yedi
sabit tarih başlığı, süreye bağlı mavi/yeşil bloklar, seçim üst bilgisi, program
satırları ve alt sağdaki Rotasyon tanımla eylemi doğrulandı. Başarısız ara
koşular ve çözümleri saatlik kayıtta korunur.

Doğrulama sınırı: yeni takvimden zamanlama/aktarma yolu mevcut yürütücüye
bağlandı ve kaynak sözleşmesi incelendi; bu yeni yoldan fiziksel ses çıkışı
ayrıca çalıştırılmadı. İki gerçek PCM regresyonu önceki AutoDJ yollarını
kapsar. Bütün projenin veya diğer prompt oturumlarının testleri geçtiği
iddia edilmez.
