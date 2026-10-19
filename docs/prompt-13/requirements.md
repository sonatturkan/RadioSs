# Prompt 13 — Otomasyon gereksinimleri ve kaynak inceleme kanıtı

İnceleme sahibi: `/root/p13_requirements`. Tarih: **2026-10-09 01:52 UTC**. Kapsam yalnız RadioS ve kullanıcının açıkça belirttiği `../prompt/13/` kaynaklarıdır. Eski prompt numaralarını kullanan ortak kayıtlar bu oturumun kapsamını değiştirmez. Bu belge **inceleme ve kabul ölçütüdür**; uygulamanın veya testlerin tamamlandığı anlamına gelmez.

## Eksiksiz kaynak envanteri

| Kaynak | Kapsam | İnceleme ve karşılaştırma kanıtı |
| --- | --- | --- |
| `../prompt/13/13.pdf` | **1 PDF; 1/1 sayfa; 1 gömülü görsel** | PDF'nin bütün metni `pdf-text.txt` içine PyMuPDF ile çıkarıldı. Sayfa 1 baştan sona okundu; 2x raster çıktısı `renders/13-page-01.png` doğrudan `view_image` ile incelendi. |
| `../prompt/13/figures/sema_1.png` | **1/1 figure; 808 × 631 piksel** | Figure doğrudan `view_image` ile ayrıntılı görüldü. PDF'deki Şema 1 ile karşılaştırıldı. |
| `renders/13-page-01-embedded-01.png` | PDF'den çıkarılmış Şema 1; 808 × 631 piksel | RGB piksel karşılaştırmasında kaynak figure ile **tam eşit**; farklı SHA-256 değerleri PNG dosya kodlamasından kaynaklanır. |

PDF SHA-256: `b115f26839f34ff97fb84b885c4d24c0016062aee17b9ee4a7cd1d3a5ac6288a`.
Figure SHA-256: `55a90850dc40b3b137c720831ecd6e20a64437d7767ac9f8c3764ab7657a39ad`.
Gömülü PNG SHA-256: `45de20b5ffe124764b5922f2d580317d59218eb88e1d1c8d95b406d5a2d6acf0`.

İlk `pypdf` layout çıkarımı Şema 1 altındaki "Sayfa tasarımı..." paragrafını atladı. Tam sayfa görsel kontrolü bu eksikliği ortaya çıkardı; `pdf-text.txt` **PyMuPDF tam metniyle değiştirildi**. Bu nedenle yalnız ilk metin çıkarımı inceleme kanıtı sayılmadı. Sistem Python'unda renderer bulunmadığı için `pymupdf` ve `pillow` yalnız `/tmp/radios-p13-pdf-tools` geçici araç dizinine kuruldu. Kaynak PDF ve figure değiştirilmedi.

## Sayfa 1 metin ve görsel referansları

- **P1-A:** Başlık "Otomasyon kısmı"; otomasyonun baştan tasarlanması ve sayfanın ikiye bölünmesi açıklaması.
- **P1-B:** Şema 1 ve "Sayfa tasarımı Şema 1'deki gibi olacak" paragrafı; Excel benzeri seçim ve seçimin üst bilgilerde saat/tarih karşılığı.
- **P1-C:** İlgili üst bar, seçili koordinat bilgisi; olası `+ / -`; saat gösterimi ve tarih seçimi hakkındaki konuşma dili açıklaması.
- **P1-D:** "Seçileni kaydet"; kayıt sonrası renkli blok; bloka tıklayınca program ismi bulunan sayfanın açılması.
- **P1-E:** Program altındaki satırlar; en alt sağdaki "Rotasyon tanımla"; tanımlamanın satıra anında eklenmesi; satırların birbirleri arasında değişmesi.
- **P1-F:** Rotasyon ekranında playlist, sıklık, rotasyon rastgeleliği, playlist içindeki çalma rastgeleliği, reklam ve sweeper rastgeleliği.
- **P1-G:** Bardaki çoklu event seçimi ve diğer barlara kopyalama özelliği.
- **FIG-1:** Aynı sayfadaki Şema 1 ve `figures/sema_1.png`.

## Uygulanacak davranış ve kabul matrisi

| Kimlik | Kaynak | Gereksinim / ayrıntılı kabul davranışı |
| --- | --- | --- |
| P13-01 | P1-A | Otomasyon ana arayüzü mevcut kurallar formundan kaynakta anlatılan takvim/program düzenine taşınır. Mevcut RadioS veri/playlist/rotasyon davranışıyla entegrasyon korunur. |
| P13-02 | P1-A | Otomasyon sayfası **iki belirgin alana** ayrılır. Seçim/takvim ile seçilen programın ayrıntı/rotasyon alanı birlikte anlaşılır olmalıdır. Kaynak iki alanın yönünü veya oranını belirtmez; uygulamadaki seçim ayrıca karar olarak kaydedilir. |
| P13-03 | P1-B, FIG-1 | Ana plan Şema 1 gibi gün/tarih başlıklarının üstte olduğu haftalık sütunlardan oluşur. Şemada **7 bitişik gün sütunu** vardır; günler soldan sağa ilerler. |
| P13-04 | FIG-1 | Saat aşağı doğru ilerler. Bir programın blok yüksekliği başlangıç/bitiş aralığına bağlıdır: 18:00–19:00, 19:30–22:00 ve 23:00–00:00 örnekleri aynı görünümde farklı yüksekliklerle gösterilir. Dakika hassasiyeti ve gün sonu 24:00 temsil edilebilmelidir. |
| P13-05 | P1-B, FIG-1 | Kullanıcı Excel'de hücre/aralık seçer gibi gün sütununda saat aralığı seçer. Görünür seçimin koordinatı ve seçilen aralık veri modeline aynı gün/saatlerle aktarılır; sürükleme, tıklama veya eşdeğer aralık kontrolü gerçek seçim durumunu değiştirir. |
| P13-06 | P1-B, P1-C | Üst bilgiler/ilgili üst bar seçilen gün/tarih ile saat aralığının karşılığını gösterir. Seçim değiştiğinde üst bilgi hemen yenilenir; kaydetme farklı veya eski bir aralığı kullanmaz. Saat aralığı özellikle görünür olmalıdır. |
| P13-07 | P1-C | `+ / -` ifade olarak "belki" denmiştir; kesin zorunlu kontrol değildir. Eklenirse zaman ölçeğini/ilgili seçimi tutarlı değiştirmeli, seçimin kaydedilen koordinatlarını bozmaz. |
| P13-08 | P1-D | Seçim üst barında tam etiketli **"Seçileni kaydet"** eylemi bulunur. Geçerli gün/saat seçimini yeni program/event olarak kaydeder; boş/geçersiz aralık kaydedilmez. |
| P13-09 | P1-D, FIG-1 | Kaydedilmiş program/event ilgili gün ve saatlerde renkli blok olarak hemen görünür. Şemadaki ana blok renkleri piksel örneklerinde yeşil `#abfe33` ve açık mavi `#abd6e4`; farklı blok renkleri görünür ayırt edilir. Kaynak renklerin anlamsal tür eşlemesini belirtmez. |
| P13-10 | P1-D, FIG-1 | Kayıtlı bloka tıklamak o programın düzenleme sayfasını/alanını açar. Blok üzerinde saat aralığı ve program ismi görülebilir; editörde program adı düzenlenebilir ve doğru evente aittir. |
| P13-11 | P1-E | Program düzenleme alanında rotasyonlar alt alta **ayrı satırlar** halinde görünür. Birden fazla rotasyon tek programda tutulabilir; boş satır alanı ile gerçek kayıtlı rotasyonlar ayrılır. |
| P13-12 | P1-E | **"Rotasyon tanımla"** eylemi program düzenlemesinin **en alt sağında** bulunur. Kullanıcı yeni rotasyon ekranını/diyaloğunu buradan açar. |
| P13-13 | P1-E | Yeni rotasyon onaylandığı anda seçili programın satır listesine eklenir; sayfayı kapatma veya yenileme gerektirmez. İptal edilen tanımlama veri değiştirmez. |
| P13-14 | P1-E | Rotasyon satırları birbirlerinin yerine taşınabilir; yukarı/aşağı veya sürükleme eylemi gerçek rotasyon sırasını değiştirir ve kaydedilen modelde aynı sıra tutulur. |
| P13-15 | P1-F | Rotasyon tanımlama ekranında **hangi playlist** kullanılacağı seçilir. Mevcut RadioS playlist kayıtlarıyla eşleştirilen kimlik/ad kullanılır; yalnız dekoratif bir alan kabul edilmez. |
| P13-16 | P1-F | Rotasyonda **sıklık** alanı bulunur, geçerli değer doğrulanır ve ilgili rotasyon kaydına yazılır. Kaynak sıklığın birimini/tipini belirtmez; mevcut rotasyon motoruyla uyumlu anlam ve varsayılan açık karara bağlanır. |
| P13-17 | P1-F | **Rotasyon/playlistler arası rastgelelik** seçeneği playlist içi rastgelelikten bağımsızdır. Kullanıcının seçimi ilgili rotasyon kaydında korunur. |
| P13-18 | P1-F | **Playlist içinde çalma rastgeleliği** ayrı seçenektir; sıra koruma ile rastgele parça seçme birbirinden ayırt edilir. |
| P13-19 | P1-F | **Reklam rastgeleliği** ve **sweeper rastgeleliği** kullanıcı tarafından ayarlanabilir. Ayrı medya türleri olduğundan iki kontrol/kayıt kullanmak kaynak cümlesindeki iki gereksinimi görünür ve izlenebilir karşılar. |
| P13-20 | P1-G | Kullanıcı birden fazla kayıtlı event seçebilir; seçili event kümesi görünürdür ve tek event açma/düzenleme hareketinden ayırt edilir. |
| P13-21 | P1-G | Seçili çoklu eventler başka gün/bar sütununa kopyalanabilir. Tüm seçili eventlerin saat aralığı, program adı ve rotasyon sırası/ayarları yeni eventlere aktarılır; yeni kimlikler üretilir ve kaynak eventler korunur. |
| P13-22 | Kullanıcının temel talimatı | Seçim, kayıt, düzenleme, sıralama ve çoklu kopyalama uygun native UI/veri testleriyle doğrulanır. İnceleme, uygulama ve test aşamaları ayrı raporlanır. Başarısız/eksik kabul ölçütü açık kalır. |

## Şema 1 ayrıntıları ve sınırlar

Şema 1'de gri üst araç çubuğu, koyu gri gün başlıkları, ince koyu sınırlar, 7 eşit genişlikte gün sütunu ve süreye göre büyüyen yeşil/açık mavi program blokları görülür. Her blok başlangıç-bitiş saati ve isim satırları taşır; bazı bloklarda alt program/kişi bilgisi ve tekrar simgesi vardır. Tarih örnekleri Almanca ve Aralık ayındadır; kullanıcı verisini sabit bu adlara/tarihlere dönüştürme talimatı değildir. RadioS içindeki gün/tarih karşılıklarıyla aynı yerleşim uygulanmalıdır.

Görsel üstünde sonradan eklenmiş robot/komut balonu ve siyah yüzer ekran kontrolü görülür. PDF metni bu üst kaplama için ürün gereksinimi tanımlamaz; kaynak görselin ekran yakalama katmanı olarak ayrıca kayda alınmıştır. Program takviminin parçası olduğu varsayılarak yeni bir robot arayüzü üretilmemelidir.

## Belirsizlik ve açık uygulama kararları

1. **Eksen anlatımı:** P1-C "dikey tarih bilgisi" derken FIG-1 tarihler üstte yatay sütunlardır; aynı paragraf "tarihi seçmiyorsun" dese de P1-B açıkça seçimin saat ve tarih bilgisini ister. Görseldeki haftalık sütunları ve seçilen sütundan türeyen tarih + saat üst bilgisini birlikte kullanmak tüm görünür kaynakları karşılar. Tarihi ayrı seçme zorunluluğu çıkarılmamalıdır.
2. **İki alan:** Bölünme yönü/oranı çizimde verilmemiştir. Takvim + seçili program/rotasyon düzenleme alanı uygulama kararı olarak kaydedilmelidir; rotasyonları yalnız eski genel kural ekranında bırakmak P1-E davranışını karşılamaz.
3. **Sıklık birimi:** Kaynak birim vermez. "Kaç parçada bir" gibi mevcut motorun desteklediği açık bir anlam seçilecekse koordinasyon kaydı ve veri şeması bu anlamı yazmalıdır.
4. **Rastgelelik kapsamı:** Sadece checkbox saklamak kaydın varlığını doğrular; yayın motorunun bu ayarları gerçekten uyguladığı iddiası ayrıca motor test kanıtı gerektirir. Mevcut motorla bağlanmayan kontroller uygulandı veya canlı yayında çalışıyor diye raporlanmamalıdır.
5. **Kopyalama hedefi:** "Diğer barlar" Şema 1'deki diğer gün sütunlarıyla eşleşir. Hedef seçimi, aynı saatleri yeni güne taşır. Çakışma politikası kaynakta verilmemiştir; uygulama kararı olarak kaydedilmeli, sessiz veri kaybı olmamalıdır.

## Mevcut RadioS bağlantı noktaları

- Native otomasyon: `src/ui/automation.c`; yeni takvim/program sayfası bunun mevcut gelişmiş rotasyon kontrolleriyle birlikte açılabilir.
- Mevcut browser taslak referansı: `design/reference/tasarrım/automation-workspace.fragment.html`, `.css`, `.js`. Bu dosyalar inceleme sırasında eski genel rotasyon formunu gösterir; Şema 1'deki gün-saat takvimi değildir.
- Veri/motor: `database/SDBASE/src/sdbase/rotation.py`, `auto_schedule.py`, `schedules.py`; kalıcı program/event şeması ve mevcut playlist kimlikleriyle entegrasyon gerekir.

Bu bağlantı incelemesi geliştirmeden önce salt okunur yapılmıştır; bu inceleme görevi uygulama kaynaklarına değişiklik yapmamıştır. Ana ajan yeni `src/ui/automation_calendar.c`, `include/radios/automation_calendar.h` ve `automation_calendar` backend şemasını uyguladı. İnceleme ajanı daha sonra aşağıdaki ayrı sahiplikle native kabul testlerini ekledi.

## Native kabul doğrulama kaydı

**2026-10-09 02:01 UTC:** `tests/test_automation_calendar.c` ve `tests/run_automation_calendar_ui.py` sahipliği ana ajandan bu inceleme ajanına devredildi; UI kaynaklarına dokunulmadı. Strict C11 syntax kontrolü `-Wall -Wextra -Wpedantic -Werror` ile ve `build/prompt13` gerçek test executable derlemesi geçti. `ctest --test-dir build/prompt13 -R '^native_automation_calendar$' --output-on-failure` **1/1 Passed, 2.31 saniye** verdi.

Test, izole **gerçek SDBASE** kurulumunda authenticated ikinci istemciyle `_rev > 0` ve tam config readback yapar; widget kapanıp yeniden açıldığında aynı snapshot'ı doğrular. Gerçek GDK press/motion/release olaylarıyla 18:00–19:00, 19:30–22:00 ve 23:00–24:00 aralıkları seçilir; kayıtlı bloka pointer tıklaması doğru program/rotasyon alanını açar. Dört bağımsız rastgelelik ayarı, sıklık, satır taşıma/kaldırma, iki eventi diğer gün sütununa kopyalama, yeni event/rotasyon UUID'ları, kopyanın kaynağı bozmadan düzenlenmesi, iptal ve çoklu copy çakışmasında atomik korunma test edilir.

`renders/native-calendar.png` gerçek GTK ekran görüntüsü `view_image` ile incelendi. İlk görselde kaydırıldığında gün başlıklarının görünümden çıktığı tespit edildi ve ana ajana bildirildi; `automation-calendar-days` ayrı, sabit başlık alanı olarak uygulandı.

**2026-10-09 02:14 UTC — Güncel doğrulama:** strict yeniden derlemeden sonra `native_automation_calendar` **Passed, 2.63 saniye; 2/2 gerçek GLib kabul vakası**. Güncellenmiş `renders/native-calendar.png` doğrudan görüldü: yedi gün başlığı kaydırmada görünür kalır; açık mavi ve yeşil palette, 18:00–19:00 / 19:30–22:00 / 23:00–24:00 süreleri, iki alan, seçili program satırı, dört rastgelelik sütunu ve alt sağdaki "Rotasyon tanımla" görünürdür. İptal, geçersiz/ters/boş seçim, renk değerinin kopyada korunması, ana program alanından kaydetme ve gelişmiş kuralların pencere kapatma sonrası aynı içerikle tekrar açılması da geçti.

İkinci vaka gerçek 2 saniyelik PCM dosyasını mevcut import/analiz yolundan geçirir; SHA kimliğine bağlı `verified` / `object_available` kaydını gerçek playlist girişine bağlar. Native **Önizle** düğmesi iki gerçek `desktop-playout/prepare` isteği ve native planner üzerinden 200 öğelik salt okunur önizleme üretir. Gerçek medya başlığı, takvim config'inin değişmemesi, DB revision'ın artmaması ve program değişince eski önizlemenin aktarım/zamanlama kontrollerinin kapanması doğrulanır. Bu test ses yayını başlatmaz ve yayın gerçekleştiği iddiasını doğrulamaz.

Aradaki başarısız koşular ayrı hata kayıtlarıdır: JSON shallow copy riski ana ajan tarafından deep copy ile giderildi; async liste yenilemesinin seçili rotasyonları kaybetmesi UUID bazlı seçimi koruyarak düzeltildi. Son inline-save timeout'ının kesin nedeni **test harness'inin** 32 kapasiteli runtime status kuyruğunu tüketmemesiydi; runtime sözleşmesindeki backpressure worker'ı bekletiyordu. Test pump'ı gerçek pencere gibi `radios_runtime_pop_event` / `radios_event_free` kullanır. Bekleme ayrıca yalnız sunucudaki eşit config'e bakmak yerine GTK tarafındaki temiz, teyit edilmiş `radios_data_document_revision > 0` durumunu bekler; sonra ikinci istemciyle bir kez tam readback yapar. Production limitleri ve kapasite politikası değiştirilmedi.

## Son kaynakla kabul — 2026-10-09 02:26 UTC

Kesinti sonrası devam eden inceleme/test sahibi `/root/p13_requirements_final`; üretim kaynaklarına yazmadı. Ana ajanın iki satırlı gün/tarih başlığı ve görünür önizleme teşhisleri tamamlandıktan sonra strict executable yeniden derlendi. Son test genişletmesiyle **02:26:46–02:26:49 UTC** odak koşumu `native_automation_calendar`: **1/1 CTest hedefi PASS, 2,52 saniye; 2/2 gerçek GLib vakası PASS, atlanan vaka yok**. Önceki 02:14 ve 02:22 kabulleri yeni assertionların sonucu yerine kullanılmadı.

Kalıcı kanıtlar:

- `build/prompt13/p13-native-calendar-final.xml`: ayrı JUnit sonucu.
- `build/prompt13/p13-native-calendar-final.log`: ayrı tam CTest/TAP ve gerçek kayıt bildirimleri.
- `build/prompt13/native-calendar-final-acceptance.json`: UTC başlama/bitiş, komut, sonuçlar, 13 kaynak/binary dosyasının koşum öncesi/sonrası SHA-256 değerleri. **Koşum sırasında izlenen hiçbir dosya değişmedi.**
- `renders/native-calendar.png`: **02:26:48.785642 UTC** gerçek GTK yakalaması. Doğrudan görüntülendi: yedi gri başlıkta ayrı gün ve tam tarih satırları, görünür saat ekseni, açık mavi/yeşil programlar, üç farklı süre örneği, seçili event, sağ rotasyon satırı ve dört bağımsız seçim sütunu, en alt sağdaki "Rotasyon tanımla" ve veritabanıyla eşitlenmiş durum metni teyit edildi. Yakalama GTK frame clock'unun teyit edilmiş durumu çizmesine izin verildikten sonra alınır.

| Ölçütler | Son native / diğer anlamlı kanıt |
| --- | --- |
| P13-01–04 | Gerçek iki alanlı GTK sayfası ve kaynak Şema 1 ile karşılaştırılmış son görüntü; yedi sabit başlık; 18:00–19:00, 19:30–22:00 ve 23:00–24:00 gerçek model/süre doğrulaması. Eski AutoDJ için ayrıca architecture kaydındaki iki gerçek PCM regresyon vakası. |
| P13-05–06, P13-08–10 | Gerçek GDK press/motion/release ile aralık seçimi ve blok tıklaması; üst koordinatın seçime eşliği; doğru programa kayıt; açık mavi/yeşil renk readback; program adı ve ayrıntı penceresiyle bağımsız düzenleme. |
| P13-07 | Gerçek `+` / `−` düğmeleri grid yüksekliğini artırıp geri getirir; seçili koordinat metni ve bütün kalıcı config değişmez. |
| P13-11–16 | Gerçek playlist ID seçimi, dört seçenek ve sıklık; onay sonrası **main context pump ve sunucu ACK öncesinde** satır sayısının anında artması; gerçek yukarı/aşağı/kaldırma ve kayıtlı sıra; ayrı rotasyon tanımlama iptali satır sayısını veya config'i değiştirmez. |
| P13-17–19 | Native bağımsız boolean readback, kopyada korunma; core adapter'ın 6 test grubunda gerçek sıklık kotası ve bağımsız medya havuzu/sıra davranışı. Son native gerçek doğrulanmış PCM önizlemesi bu adapter ve iki authenticated prepare yanıtını native planner'a bağlar. |
| P13-20–21 | İki eventin çoklu seçimi, başka gün sütununa atomik kopyalama, yeni event ve rotasyon UUID'ları, kaynak korunması, kopyanın ayrı düzenlenmesi; çakışan hedefin hiçbir kısmi kopya oluşturmaması. |
| P13-22 | Gerçek veritabanı ACK + ikinci authenticated istemci tam readback + pencereyi kapatıp yeniden açma eşliği. Ayrı final TAP/JUnit/SHA kaydı; önceki hatalar yukarıda korunur. |

Ek sınır kabulü: `9999-12-31` tarihi haftayı `9999-12-27` pazartesisine taşır; tarih aralığının dışındaki cumartesi/pazar sütunları seçilemez, sonraki hafta düğmesi no-op olur. Son geçerli cuma seçilebilir. Yedi sütun ve başlık gerçek GTK'da çizildi; `G_DEBUG=fatal-warnings` altında uyarı/critical oluşmadı ve config değişmedi. Gelişmiş kuralların gerçek pencere X-kapatma/hide ve aynı widget ile yeniden açılması da geçti.

Son medya vakası, kaynak Şema 1 dışında bir veri sınırını da görünür doğrular: gerçek **2 saniyelik doğrulanmış PCM** ile **10:00–10:01** blok hazırlığı tam **30 öğe** üretir; sonraki parça blok sonunu aşacağı için kesilir ve önizlemede `automation-calendar-preview-notes` etiketi **"blok sonunu"** açıklamasını gösterir. Config ve kayıt revision'ı önizlemede değişmez; program adı düzenlenince eski aktarım/zamanlama eylemleri kapanır. Önceki 200 öğelik pozitif native kabul ve core cap/süre testleri tarihsel ayrı kanıttır. Bu son test **canlı ses yayını veya fiziksel ses donanımı kabulü değildir**; gerçek eski AutoDJ ses testleri ve yeni core/planner kanıtı ayrıca raporlanır.

Kaynakta belirtilmeyen yatay iki alan oranı, UTC haftası, sıklığın her rotasyon turundaki parça kotası olması, zaman adımı, renklerin tür eşlemesi olmaması ve atomik çakışma reddi `implementation.md` / `architecture.md` içinde açık uygulama kararlarıdır. Bu belgeler ve koordinatörün bağımsız nihai matrisi uygulama sonucu ile test sınırlarını birlikte kaydeder.

P13-13'ün anında ekleme/rotasyon iptali koşulunu doğrudan kanıtlayan son iki assertion ayrı test değişikliğidir; mevcut model kontrolünü sadece ACK sonrasında yapmak yeterli kabul edilmedi. İlk strict yeniden derleme dış canlı sahibin `broadcast_online.c:497` guarded YouTube seek tüketicisini public declaration/gerçek provider yayımlanmadan önce okudu ve `-Werror=implicit-function-declaration` ile başarısız oldu. P13 yabancı kaynağı değiştirmedi veya uyarı koşulunu gevşetmedi. İlgili sahibi public `youtube_music.h:48` ve gerçek `src/audio/youtube_music.c:747` eşliğini yayımlayınca **strict yeniden derleme exit 0** ve yukarıdaki güncel iki native vaka geçti. Önceki 02:22 kaynak kabulünün XML/TAP/JSON ve PNG'si `-20261009T022233Z` tarih ekli dosyalarda ayrıca korundu; başarısız ara build nihai başarı olarak raporlanmadı.
