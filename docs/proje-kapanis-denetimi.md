# RadioS 0.17.0 — veritabanı ve proje kapanış denetimi

2026-10-06. Denetim kaynak kopyasının bütünlüğünü, gerçek kayıtların varlığını,
veritabanı sözleşmelerini ve etkin native C callback'lerini ayrı değerlendirir.
Geçen test sayısı veya korunmuş API sayısı, bütün tasarım işlevlerinin bittiği
anlamına gelmez. Aşamaların eski kabul kayıtları tarihsel kapsamlarıyla geçerlidir.

## Veritabanı aktarımı

- İlk paketteki **146 SDBASE dosyasının tamamı** RadioS içinde bulunur. İlk
  aktarım manifesti ve özgün arşiv SHA-256 karşılaştırmaları tutarlıdır. Mevcut
  geliştirmeler özgün motorun üzerine yapılmıştır; public işlevler silinmemiştir.
- Özgün şema 4'ün **95 koleksiyonu**, **175 indeks tanımı**, **160 workflow'u** ve
  **215 HTTP işlemi** korunur. Desktop geliştirmeleri bunlara eklenir. Güncel
  kaynak sözleşmesi **220 workflow / 270 yol / 275 HTTP işlemi** içerir;
  `islev-envanteri.json` ve `sdbase-openapi.json` içinde tutulur. Özgün 146
  dosyanın 134'ü byte eşittir; 12 dosyada mevcut geliştirmeler vardır. Dokuz
  yeni desktop modülüyle güncel veritabanı paketi 155 dosyadır.
- Verilen özgün kaynak/arşiv içinde eski çalışma DB dosyası veya medya yoktur;
  belgelerdeki eski `DBDATA` dizinleri de mevcut değildir. Tasarımın örnek
  kayıtları gerçek veri olarak aktarılmaz. Dolayısıyla aktarılmamış eski müzik,
  playlist veya reklam arşivi bulunduğu iddia edilmez.
- Gerçek RadioS DB'si şema 4 / sequence 2 / bütünlük **ok** ile başlangıç
  kayıtlarını taşır: 1 istasyon, 1 kullanıcı, 1 uç, 9 izin, 2 erişim anahtarı ve
  2 audit kaydı. Yönetilen medya, müzik, playlist ve reklam kayıtları yoktur.
  Mantıksal kayıtlar checkpoint içindedir; `.sdb` dosyasının 0 byte olması
  verinin kaybolduğu anlamına gelmez.
- Bütünlük denetimi 101 domain koleksiyonunu inceler. DB dosyaları ve kimlik
  dosyasının özetleri denetim öncesi/sonrası aynıdır. Testler özel veritabanları
  kullanır; kabul kayıtları gerçek kullanıcı DB'sine yazılmaz.

Tam motorun dokuz özgün kabul koşucusu engine, sorgu/bütünlük, disk indeksleri,
online bakım, WAL/kurtarma, replikasyon, stream/TLS, failover ve operasyonel
PCM/offline senaryolarını çalıştırır. Son kaynak snapshot'ında **333/333 kontrol**
**67,685 saniyede**, başarısız ve atlanan kontrol olmadan geçmiştir. Snapshot
sonrasında backend kaynak sapması ve üretim DB değişikliği yoktur. Bu sanal
kabul gerçek fiziksel yayın, uzun işletme veya bütün
workflow'ların native ekranda temsil edildiği anlamına gelmez.

Yerel kanıtlar: `build/source-database-closure-audit.json`,
`build/source-database-api-parity.json`,
`build/source-database-live-readonly-audit.json` ve
`build/source-database-engine-suite-final/summary.json`. Kaynak envanterindeki eski
172 toplam dosya sayısı, **72 tasarım + 100 Python modülü** demektir; 172 backend
paket dosyası değildir.

## Bu denetimde bağlanan eksikler

| İşlev | Çalışan bağlantı ve sınırı |
|---|---|
| Rekordbox XML | C dosya seçimi, sınırlı özel dosya snapshot'ı, gerçek backend preview, açık playlist/politika seçimi ve atomik apply. Katalog, kaynak/cue bilgileri, native playlist ve işlem alındısı birlikte kaydedilir. XML ses dosyasını içermez ve doğrulanmış çalınabilir medya oluşturmaz. |
| Program onayı | Kaydedilmiş native belge gerçek plan taslağına gönderilir; içerik sürümü, lifecycle, istasyon uygunluğu ve hak kararları ayrı düğmelerle uygulanır. Program onayı mevcut motorun doğrulamalarından geçer; onaylı sürüm değişmez kalır. |
| Onaylı programı yükleme | Güncel onaylı kaynak, frozen medya/cue bilgileri ve gerçek dosya/SHA denetimi ile yerel MAIN'e sıralı çalma yüklenir. `activate` ve `operate` gerekir. Uca dağıt ayrı işlemdir; kuyruğa almak uç kabulü veya yayın teslimi değildir. UTC saatleri yeniden yorumlanmaz; desteklenmeyen zamanlama anlamı açık hata ile reddedilir. |
| Acil sıra → program | Doğrulanmış geçici sıra, açık Mevcut program hedefinin sonuna eklenir; kaynak ve hedef UUID/rev/kimliği bekleme sırasında değişirse aktarım reddedilir. Kaynak sıra korunur; çalma başlamaz. Kalıcı kayıt sonucu ayrı beklenir. |
| Yayın kataloğu → program | Seçili medya gerçek dosya/SHA kontrolünden geçerek hedef öğeden sonra eklenir veya hedef öğenin UUID/saat/notunu koruyarak değiştirilir. Geciken yanıtta hedef, seçim veya kimlik değişirse uygulanmaz; kayıt CAS ile korunur. |
| Saklanmış program taslakları | Mevcut program yeni adlandırılmış typed DB kaydı olarak kopyalanır; kayıp commit yanıtında aynı gövde/anahtar tekrar kullanılır. Kaydedilen kopya açık onayla çalışma taslağına yeniden yüklenir. Yayın ekranındaki playlist alanı gerçek native playlist yönetimini kullanır. |
| Dış panel | Doğrulanmış HTTPS adresi masaüstü tarayıcıda açılır. Arka plandaki iptal edilebilir HEAD testi TLS/HTTP erişilebilirliğini ölçer; yönlendirme izlemez, kurulum anahtarı/cookie göndermez. Özet kartı formdaki girdileri gösterir. Bu test oturum/MFA/site işlevlerinin testi değildir. |
| Normal süreç kapanışı | SIGTERM/SIGINT, GTK'nin normal audio/data kapanışını çalıştırır. Özel gerçek PCM kaydında iki sinyal de WAV başlığını tamamlar; servis/kimlik korunur, kilit bırakılır, yeniden açılış idle kalır. Önceki sinyal davranışını taklit eden negatif kontrol aynı kabulü reddeder. |
| Geçici okuma istek sınırı | Ayrı incelenmiş 19 POST okuması ve iki bootstrap GET için yalnız HTTP 429 sınırlı tekrar edilir. Toplam en fazla beş deneme; ilk yanıttan sonra sekiz saniyelik pencere, sunucunun 1–3 saniyelik bekleme bilgisine uyulur. Daha uzun bekleme erken gönderim yapılmadan hata olarak kalır. İptal kontrolü 20 ms aralıklıdır. Yazma/onay/aktarım ve diğer hatalar otomatik tekrarlanmaz; istek sınırı değiştirilmez. |

Rekordbox kullanımını [içe aktarma belgesi](rekordbox-ice-aktarim.md), program
kararlarını [onay ve haklar belgesi](program-onayi-ve-haklar.md), mevcut servis
hesaplarını [çevrimiçi müzik belgesi](cevrimici-muzik.md) açıklar.
Yeni yazılım testleri `desktop_rekordbox`, `native_rekordbox`,
`desktop_programme`, `native_programme`, `native_emergency_transfer`,
`external_access`, `native_external_access`, `native_application_shutdown` ve
`native_broadcast_catalogue`
hedefleridir. Birleşik kabul sonuçları `tests/README.md` ve yerel
`build/closure-full.xml` kanıtında tutulur.
Son strict C11 derlemesi ve GUI açık birleşik kabul **54/54**, **331,91 saniye**,
sıfır başarısız ve sıfır atlanan test ile geçmiştir. Bağımsız veritabanı motoru
koşuları bunun yanında ayrı **333/333** kontrolü kapsar.
Son ASan/UBSan core **3/3** (**20,88 saniye**) ve native **8/8**
(**116,50 saniye**) geçti. Core leak denetimi açık, GTK leak denetimi kapalıdır.
Normal ve sanitizer ses koşuları ayrı yürütüldü. Nihai kurulum/paket kanıtı
`build/closure-final-acceptance.json` içinde bu test kaynaklarıyla bağlanır.
İstemcinin 19 grubu başarılıdır; dört yeni grup gerçek 429 yanıtı, süre/deneme
sınırı, iptal ve yazmanın tek denemede kalmasını sınar. Yol listesindeki
retry uygunluğu yeni GUI bağlantısı veya proje tamamlanma oranı değildir.

## Nihai tamamlanma durumu

**Veritabanı kaynak/sözleşme aktarımı tamamdır. Projenin bütün tasarım işlevleri
ve saha kabulü için hâlâ eksikler vardır; proje bütünü için yüzde 100 veya
“tamamlandı” sonucu verilmez.** Backend'i korumak, her API için yeni bir native
ekran yazıldığı anlamına gelmez. İlk doğrudan API çağrı oranı proje ilerleme
yüzdesi olarak kullanılmaz.

| Açık alan | Gereken iş veya gerçek kabul |
|---|---|
| Acil duyuru / TTS | Hazırlık kayıtları kalıcıdır; gerçek duyuru/TTS üretimi, onay/süre ve yayına dispatch kontrolü native ekranda tamamlanmamıştır. |
| USB, MIDI/GPIO, fiziksel devralma | Hazırlık etiketleri gerçek credential veya cihaz bağlantısı değildir. Fiziksel sağlayıcı, eşleme, yetki ve tek aktif çıkış kabulü gerekir. |
| Dış web kimliği ve site işlevleri | MFA/kurum kimliği, dış oturum politikası, istek formu ve reklam teslimi için çalışan dış hizmet ve ilgili protokol entegrasyonu gerekir. Kaydedilen tercih bunları başlatmaz. |
| Ayrıntılı yetki kapsamları | İstasyon izinleri çalışır; vardiya/zaman, program/deck kapsamı ve fiziksel USB yetkisi bütün kapsamlarıyla uygulanmış değildir. |
| İleri katalog / diğer adaptörlerin native ekranları | Canonical kişi/kategori/grup yönetimi, toplu/domain yönetim araçları ve mAirList/RadioBOSS/LibreTime gibi korunmuş backend adaptörlerinin tüm native iş akışları ayrı geliştirme ister. |
| Canlı Spotify/Apple Music/YouTube | Native resmî API/OAuth akışları özel fixture'larla sınanmıştır. Kullanıcıya ait gerçek hesap/uygulama kabulü yapılmamıştır. Tam servis sesi MAIN'e aktarılmaz; resmî oynatıcı açma/Spotify Connect kontrolü ayrıdır. |
| Fiziksel ve uzun süreli yayın | MAIN/CUE ayrımı, mikrofon/feedback, kullanılacak harici ses kartı, gerçek uzak dinleyici ve işletme süresine uygun kesinti/kararlılık sonuçları henüz doğrulanmamıştır. |

Tam saha kriterleri [14. aşama kabul belgesinde](14-nihai-kabul-ve-paketleme.md)
yer alır. Kesin callback kanıtları ve önce/sonra ayrımı
[GUI işlev denetiminde](gui-islev-kapanis-auditi.md) tutulur.
