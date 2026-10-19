# Yapılanlar arşivi · 12–17.10.2026

Bu dosya, `docs/yapilacaklar-2026-10-12.md` listesinden 17.10.2026 gece ayıklanan **tamamlanmış** maddelerin ve notların olduğu gibi kopyasıdır (açık işler ana listede kalır).

## 17.10.2026 gece · "AutoDJ otomasyonu" sayfası (radios-cc/0d motor ve veri, radios-44 arayüz)

| # | İş | Durum |
|---|----|-------|
| O1 | AutoDJ geçiş eğrisi ve efekt ön ayarları ("AutoDJ otomasyonu" sayfası: ön ayar / eğri / efekt; `audio_transition.h`, `transition.c`, `dsp.c`, `transition_preset.c`, `autodj_automation_ui.c`, `desktop_automation_calendar.py`); kullanıcı 21:1x'te "çalışabilir yap ve aşırı sadeleştir, normal form: bir ayar diğerini gerektirsin, guide olmasın" dedi | yapıldı: kayıt hatası giderildi (düzenleyici sarmalayıcı kutu yerine veri gövdesini okuyor; önceden kayıt boşa gidiyordu — radios-0d). Sayfa normal formda (radios-44, 21:26): anahtar kapalıyken yalnız anahtar; açılınca Türler (tür başına ön ayar kutuları; Sırayla/Rastgele yalnız 2+ seçilince) → Ön ayarlar (çipler, tek "Ekle ▾" şablon menüsü, Sil) → seçili ön ayarın alanları (Efekt seçilince Derinlik/Taraf/BPM ve Efekt eğrisi; filtre seçilince Ton eğrisi) → eğri tasarımcısı (renk kodlu eğri seçiciler; ipuçları yalnız tooltip) → Saat dilimleri (tablo ve form yalnız satır varken). Veri modeli, planlayıcı ve motor yolu değişmedi. Testler: rotasyon düzenleyici GTK 17/17 (`automation-page-presets-assign-persist`, yeni `automation-page-normal-form`; `RADIOS_AUTOMATION_PAGE_CAPTURE=<önek>` ile off/on/full PNG), transition 7/7, runner 43/43, calendar_plan 56/56, playout 54/54 (radios-0d, build-cc). Paylaşılan `build/radios` 21:26'da flock altında yenilendi |

---

# Açık işler ve notlar · 12.10.2026

Bu liste kullanıcının 12.10.2026 akşamki istekleriyle a.pdf'te gönderilmemiş kalan mesajlardan derlendi. Bir iş bitince "Durum" sütunu güncellenir.

## Bu akşamki istekler

| # | İstek | Durum |
|---|-------|-------|
| A1 | AutoDJ'de ciddi hata ("buga giriyor"); bulunup düzeltilecek | düzeltildi: SYNC'li oynatıcının play'de 2 sn'ye kadar susması, aynı parçanın arka arkaya tekrarı, parça başı/sonu sessizliği (ölü hava), beatmatch'in tek parça yüzünden tüm rotasyonu reddetmesi, "MAIN'e atanmış olmalı" reddi |
| A2 | AutoDJ geçiş ilkesi Otomasyon'dan ayarlanacak: hangi kanaldan (oynatıcıdan) hangi kanala, kanal sayısına ve içerik türüne göre geçiş, beatmatching | yapıldı: Rotasyon › Ayarlar › "AutoDJ: geçiş ve kanallar" sekmesi (tür başına geçiş, süre, kanallar) |
| A3 | "MAIN'e aktarılmalı" uyarısı kaldırılacak | yapıldı: AutoDJ boş mikser kanalını kendisi atıyor; duraklatılmış/hazır oynatıcıyı devralıyor; yalnız çalan oynatıcı korunuyor |
| A4 | Otomasyon'da playlist ekleme daha açık olacak: seç, ekle | yapıldı: "Playlist:" etiketi, "Rotasyona ekle" ve aramalı çoklu seçim "Playlist seç ve ekle…" penceresi |
| A5 | Program bölümünde çok sayıda hata var; iyice araştırılıp bu dosyaya not edilecek, sonra düzeltilecek | 15.10: P1–P4 için içerik uyarıları, takvim işaretleri ve blok bağlamlı hata mesajları tamamlandı; uygulanan davranış ve doğrulama aşağıdaki devam kaydında |
| A6 | Bitince son hali derlenip teslim edilecek; yeni istem beklemeden devam edilecek | sürekli |

## a.pdf'ten gönderilmemiş mesajlar

| # | İstek | Durum |
|---|-------|-------|
| G4 | Rekordbox gibi beat analizi; AutoDJ BPM geçişinde işaretli noktada en yakın dörtlükten başlayıp diğer işarete kadar beat-sync | analiz v2 ve dörtlüğe hizalı geçiş yazıldı |
| G5 | LUFS ses seviyesi eşitleme (oynatıcı + AutoDJ) | yapıldı: BS.1770 ölçüm, kayıt, motor kazancı (tepe -1 dBFS sınırı), Ayarlar › Ses › "Ses yüksekliği eşitleme" |
| G6 | Rekordbox okuma üst çubuğa; klasör bırakınca otomatik playlist; şarkı demeti doğrudan içe aktarma; Chrome sekmeleri | yapıldı: "rekordbox USB / SD oku" Ana medya/Sağlayıcılar/Yenile çubuğunda ve medya kütüphanesinde; pencereye bırakma sayfa açmıyor (klasör → klasör adlı playlist, dosyalar → açık playlist ya da kütüphane); sekmeler Chrome biçiminde |
| G7 | Ayarlar: MIDI eşleme, bölümlerin kaldırılması, Kısayol/Lisans/Hakkında, en yüksek örnekleme hızı, master aygıt | yapıldı: Ses/MIDI/Kısayol/Gelişmiş/Lisans/Hakkında; tampon, metronom, ses düzeyi kaldırıldı; "Görünüm"deki başlık ayarları Gelişmiş'e taşındı; MIDI cihaz, harita oluştur/yükle, anahtar listesi |
| G8 | Routing sayfası yeniden tasarım, Icecast/SHOUTcast, çoklu blok, DB kayıt | yapıldı: ayrıntılar aşağıda (“Ses yönlendirme”) |
| G9 | Elle BPM ve dörtlük seçimi; parçalar otomatik analiz | ızgara düzenleyici ve arka plan analizi yazıldı |

## Program bölümü hata notları

Araştırma 12.10.2026 22:5x UTC'de gerçek veritabanı (salt okunur) ve yalıtılmış GUI testleriyle yapıldı.

| # | Bulgu | Etki | Önerilen düzeltme |
|---|-------|------|-------------------|
| P1 | Otomasyon takviminde 6 program bloğunun 4'ü yayın saatinde hazırlanamıyor. 5, 6 ve 8 Ekim 00:30–03:30 bloklarında hiç rotasyon yok; 14 Ekim 00:30–04:30 bloğunda playlist'i olmayan "Yeni rotasyon 6" atanmış. | Blok saati gelince otomasyon o bloğu çalamaz; yayında boşluk ya da durma | Kaydederken uyar; takvimde bu blokları kırmızı/kesik çizgiyle göster; yayın akışı geçersiz bloğu atlayıp bir sonrakine geçsin |
| P2 | Hata mesajı yalnız "Takvim programı, saat aralığı veya rotasyon satırları geçersiz." diyor; hangi bloğun neden geçersiz olduğu yazmıyor. | Operatör sorunu bulamıyor | Mesajda blok adı, tarih-saat ve neden olsun: "rotasyon atanmamış" / "'Yeni rotasyon 6' içinde playlist yok" |
| P3 | Yeni program bloğu penceresi yalnız ad ve renk soruyor; rotasyon ataması ayrı panelde ve kolay unutuluyor. Tüm bloklar varsayılan "Yeni program" adını taşıyor. | Boş bloklar oluşuyor (P1'in kaynağı) | Blok oluşturma penceresine rotasyon seçimi ekle; rotasyonsuz kaydı onaya bağla |
| P4 | Rotasyon düzenleyici playlist'siz rotasyonun kaydedilmesine izin veriyor ("Yeni rotasyon 3/4/6" boş). | Bu rotasyonları kullanan bloklar çalınamaz | Boş rotasyonu kaydederken uyar; takvimde atanırken "playlist yok" etiketi göster |
| P5 | Aynı playlist'i paylaşan kaynaklar ve karıştırma döngüsü sınırı aynı parçayı arka arkaya seçiyordu. | Program içinde tekrar | Düzeltildi (A1) |
| P6 | İşaretsiz parçalarda baştaki ve sondaki sessizlik çalınıyordu. | Program geçişlerinde ölü hava | Düzeltildi (A1) |
| P7 | GUI testi `save-full-weekly-programme-snapshot-source-unchanged` tek başına geçiyor; tam pakette önceki vakaların bıraktığı 10:30 etkinliğine tıklayıp düşüyor. | Test yalıtımı; ürün hatası değil | Her vaka için temiz takvim ya da çakışmayan saat |
| P8 | Yayın sayfasındaki "Mevcut program" ve oynatıcı yayın sıraları gerçek veriyle hatasız hazırlanıyor. | — | Sorun bulunmadı |
| P9 | Haftalık program kaydı yok (kullanılmamış). | — | Kullanılınca yeniden denetlenecek |

## Ses yönlendirme (G8) · 13.10.2026

Başlıksız 7.pdf'teki istekler ve yapılanlar:

| İstek | Yapılan |
|-------|---------|
| Ses blokları ile sağdaki bölümün yeri değişsin | Blok paleti sağda, blok ayarları solda |
| Bloğa tıklayınca açılsın, sağ üstte × ile kapansın | Ayar sütunu yalnız bir blok seçiliyken görünür; başlığındaki × kapatır |
| Blok adı değiştirilebilsin; üstte verilen ad, altta tür | Mavi başlıkta ad ve kalem (✎); kaleme tıklayınca ad kutusu açılır (F2 ya da sağ tık › Yeniden adlandır da çalışır); türü başlığın altında yazar |
| "WAV kayıtçı" adı "Kaydedici" olsun; kayıt dosyası çubuğu kaydedicinin ayarlarına | Kaydedici bloğu: başlat / durdur / duraklat / böl, klasör + Seç…, dosya adı öneki, otomatik bölme (dakika), süre, boyut, parça, dosya yolu |
| MAIN'e aktar / MAIN aktarımını durdur bölümü kalksın | Kaldırıldı; yerine üst çubukta tek "Yönlendirmeyi başlat / durdur" |
| Kompresör ve uygulama blokları kalksın | Kaldırıldı. Eski kayıtlar açılınca mikrofon/çıkış/MAIN/kaydedici blokları ve kabloları korunur, kompresörlü mikrofon yolu doğrudan kabloya dönüşür |
| Mikser girişleri: kanal seçip yönlendirme | "Mikser girişi" bloğu (kanal 1–32): girişine bağlanan ses o kanalın şeridinden (EQ, fader, mute, efekt) MAIN'e karışır; çıkışı o kanalın fader sonrası sesidir |
| Ses girişi çıkış olarak da miksere gönderilebilsin | Ses girişi → Mikser girişi / MAIN yayını / çıkış / kaydedici / yayın, hepsi bağlanabilir |
| Mikrofon: bas-konuş mikrofonu ve diğer mikrofonlar | "Mikrofon" bloğu: Konuşma mikrofonu (Ayarlar › Ses'teki yayın mikrofonu) ya da herhangi bir giriş; "Bas-konuş" seçiliyken ses yalnız yayın ekranında mikrofon açıkken geçer |
| Sanal ses arayüzü tanımlama | "Sanal ses arayüzü" bloğu ses sunucusunda "RadioS · ad" adlı bir çıkış aygıtı (null sink) oluşturur; istenirse diğer uygulamalara mikrofon olarak da sunulur (remap source). Çalan bir uygulamanın sesi "Buraya yönlendir" ile bu aygıta taşınır. Blok silinince ya da RadioS kapanınca aygıt kaldırılır; çökme sonrası kalanlar açılışta temizlenir |
| Nereye konursa oranın ses seviyesini ölçsün | "Seviye ölçer" bloğu sesi değiştirmeden geçirir; tepe/RMS çubukları hem blokta hem ayarlarda. Her bloğun altında da canlı seviye çubukları var |
| İnternet yayını (butt / Icecast / SHOUTcast gibi) | "İnternet yayını" bloğu: Icecast 2 (HTTP PUT, Ice-Name/Genre/Description/URL/Public başlıkları), SHOUTcast 1 ve 2 (port+1 kaynak girişi, v2'de :#akış no), MP3 64–320 kbps, istasyon adı/tür/açıklama/web, şarkı bilgisi gönderme, kopunca 5 sn'de bir yeniden bağlanma. Şifre veritabanına yazılmaz, yalnız oturum boyunca bellekte |
| Bütün kombinasyonlar çalışsın, bloklar birden çok kez kullanılsın | Her türden çok sayıda blok (toplam 48, kablo 128; aygıt blokları türü başına 8, kaydedici 8, yayın 4). Geri besleme döngüsü yaratan kablolar (MAIN → mikser, aynı kanal → kendisi, bir çıkışın monitörü → aynı çıkış, sanal arayüzün kendi monitörü) reddedilir ve nedeni yazılır |
| Yapılandırmalar veritabanına kaydedilip geri yüklensin | "Yapılandırma" listesi + Kaydet / Farklı kaydet… / Sil / Yenile. Belge şeması 2: {active, configs:[{name, graph}]} (en çok 32). Seçince yüklenir; yönlendirme çalışıyorsa canlı uygulanır |

Altyapı: `src/audio/patchbay.c` (10 ms adımlı canlı çalıştırıcı, Pulse yakalama/çalma, sanal aygıtlar, kaydedici ve yayın çıktıları), motor dinleme noktaları ve hat girişleri (`radios_audio_engine_tap_open` / `line_open`: MAIN, konuşma mikrofonu, kanal fader sonrası; kanala ya da MAIN'e canlı giriş), `src/core/routing_graph.c` (çok örnekli model, döngü denetimi, JSON, eski şema dönüşümü), arka uç şema 2 doğrulaması.

Testler: `routing_graph` (kurallar/döngüler/JSON/eski belge), `patchbay` (motor → ölçer → WAV, kanal 1 → kanal 6 hattı → WAV, Icecast PUT ve SHOUTcast kaynak girişiyle gerçek MP3, Icecast başlıkları; özel PipeWire sunucusunda: giriş → çıkış, sanal aygıt + sanal mikrofon, uygulama taşıma, aygıtların kaldırılması — `RADIOS_TEST_PULSE_SERVER` verilince), `routing_graph_schema` ve `desktop_routing` (arka uç). Yönlendirme GUI testlerini (routing_patchbay_ui_test.inc) Codex yazıyor.

## 13.10.2026 öğleden sonra istekler

| # | İstek | Durum |
|---|-------|-------|
| R1 | Ayarlar'a "Sıfırlama" bölümü: genel ayarları sıfırla, playlistleri sıfırla, içe aktarılan şarkıları sil, mikser ayarlarını sıfırla, otomasyonları sıfırla, fabrika ayarlarına döndür | yapıldı: Ayarlar › Sıfırlama. Her işlem önce neyin etkileneceğini sayar, onay ister (fabrika: SIFIRLA yazılır), varsayılan olarak data/backups altına tam veritabanı yedeği alır. Kayıtlar silinmez, devre dışı bırakılır; geçmiş, reklam/yayın kayıtları, kullanıcılar ve WAV kayıtları korunur. Sonunda "Şimdi yeniden başlat" seçeneği var. Yalnız yönetici (manage) yetkisiyle çalışır |
| R2 | Ayarlar'daki "Görünüm" bölümü geri gelsin | yapıldı (Codex 12:54): yazı boyutu, başlangıç profili, yayın çubuğu alanları |
| R3 | Radyonun ve ses yönlendirmenin bütün ayarları veritabanına kaydedilsin | yapıldı: ses yönlendirme her değişiklikten 1,5 sn sonra kendiliğinden kaydedilir. Yerel dosyalardaki tercihler (görünüm, ses paneli: örnekleme hızı, tampon) "desktop_preferences" belgesiyle veritabanına eşlenir; veritabanındaki kopya açılışta yerel dosyaya ve pencereye uygulanır. Yazı boyutu ve başlangıç profili artık değişince kaydedilir. İnternet yayını şifresi bilerek veritabanına yazılmaz (yalnız oturum belleğinde) |

Notlar (13.10.2026 akşam):
- Kullanıcı 13:08'de Codex'e "çalışan programı asla kapatma" dedi; teslimde açık RadioS'a dokunulmaz.
- Veritabanı kodu değişince başlatıcı (run-radios.sh) servisi bir sonraki açılışta kendisi yeniler.
- Testler: desktop_reset (5 vaka: şarkı silme + yedek, kapsamlar, onay/yetki, tercih belgesi, HTTP), native_preferences (Sıfırlama bölümü), GUI dışı 78 test geçti. Üretim veritabanının kopyasında altı sıfırlamanın hepsi denendi; bütünlük denetimi temiz, fabrika sıfırlaması yedekle 1,7 sn.

## 15.10.2026 · Kapanan son iki Claude sohbetinin devamı

`radios-52` (98fd3d0f…) ve `radios-38` (2333d2f3…) oturumlarının kesildiği noktalar geri okundu. PDF yeniden yorumlanmadı; mevcut kodda yarım kalan teknik işler tamamlandı. AGENT.md değiştirilmedi.

| Konu | Güncel davranış ve kanıt |
|---|---|
| P1 / P3: boş veya çalınamaz program | Rotasyon atanmaması, boş/kayıp rotasyon, boş/silinmiş/devre dışı playlist ve doğrulanmış yerel medya olmaması takvimde kırmızı kesikli sınır ve nedenli uyarı verir. Taslak oluşturulup kaydedilebilir; atama eksikliği saklanmaz. Rotasyonsuz/kaynaksız ve sıfır ağırlıklı bloklar şema seçiminde atlanır; yapısal olarak bozuk bloklar açık hata ile reddedilir. |
| P2: genel hata mesajı | Çekirdek hata mesajında blok adı, tarih, saat ve somut eksiklik var. Bozuk UTF-8, satır sonu ve çok uzun ad güvenli biçimde sınırlandırılır. |
| P4: boş rotasyon | Playlist kaynağı olmayan rotasyon düzenleyicisinde kalıcı uyarı var; taslak kaydetme korunur. Atanan boş rotasyon takvimde de görünür. |
| Olasılık ve devam noktası | %0 ile kapatılan şarkı kalan-pay yedek seçimiyle geri gelmez. Bozuk tüketim/devam kaydı sessizce kabul edilmez. |
| Geçiş ve jingle | Erken geçiş doğrulanmış AutoDJ çıkışına, sonra otomatik-sonraki işaretine, son olarak cue-out'a dayanır. Uygun olmayan kısa/konuşma geçişleri çekilişten önce elenir. Sonraki öğenin saat kuralları gerçek kaydırılmış başlangıçta değerlendirilir. Jingle doğrulanmış intro/outro penceresine bütünüyle sığar; güvenli pencere yoksa tanı ile atlanır. |
| Otomatik playlist / şema yükleme | Güncel kaynakta bekleyen HTTP yanıtı hazırlık zaman aşımını engellemez; geç yanıt mevcut sesi bozamaz. Gerçek oynatma, mevcut/gelecek/bitmiş şema durumu, program adı, bitiş ve reklam sayacı özel GTK/HTTP/Pulse testleriyle doğrulandı. |

Doğrulama kapsamları ayrı tutuldu: takvim 49 + şema 9; politika/planlayıcı/backend/ses kapsamında 114 ayrı vaka; güncel arayüzde 8 gerçek GTK/HTTP + 11 taslak regresyonu; runtime 9, geciken yükleme 1 ve gerçek üst çubuk oynatma 2 vaka. Son ana derlemede 13 zor senaryo tekrar geçti. En son yükleme/üst çubuk/olay görünümü değişiklikleriyle 8 kayıtlı gerçek GTK/HTTP/Pulse test grubu da geçti (geciken yanıt 1, üst çubuk 2, olay görünümü 4, jingle PCM 1); duraklatılmış manuel yayın aynı ses oturumunda sürdürülüyor. Bunlar bütün proje testlerinin eksiksiz çalıştırıldığı iddiası değildir.

Ana `build/radios` 0.18.0 katı derleyici uyarılarıyla derlendi, yapılandırma denetimi geçti. Açık normal pencere MAIN ve internet üzerinden gerçekten yayın yapıyordu; kapatılmadı, ikinci ses sahibi açılmadı ve veri servisi yeniden başlatılmadı. Görünen servis sürümü uyarısı için güncel servis kodunun yüklenmesi yayın sonrasındaki yeniden başlatmayı bekliyor. Bu çalışma normal veritabanını, parça işaretlerini veya program atamalarını değiştirmedi.

Güncel kanıtlar: `build/claude-recovery-20261015/RECOVERY.json`, `final/`, `calendar/context-results.json`, `policy-STATUS.json`, `ui-revalidation.json` ve `runtime/`. Eski 82 testin tamamının geçtiği ifadesi düzeltilmiştir; o tarihsel koşumda abort ve atlama bulunuyordu.

Son teslim kanıtı: `build/claude-recovery-20261015/final/delivery-snapshot.json` (kaynak kilidi altında güncel ikili, iki gerçek üst çubuk PCM testi ve yapılandırma denetimi). Tamamlanan kuyrukta yeniden çalma/tüketim sayımı ve yayına dönüş de üç gerçek ses oturumuyla geçti; uzun ömürlü kayıt-budama eşiği ayrıca zorlanmadı.

## 15.10.2026 akşam · radios-52 devamı

| # | İstek | Durum |
|---|-------|-------|
| R1 | Oynatıcıda rotasyon kısmı "normal form": tek düğme diğerlerinin yerini alsın, yer kaplamasın; ayrıntıya bakmak gerekmesin | yapıldı: Rotasyon/Şema/Program yükle ve Şema oluştur tek "Otomasyon ▾" menüsünde; kartın kendi satırı yüklü olanı ve sorunu "⚠ …" ile yazıyor; "Tekrarla" ve "Sıradaki program" yalnız program yüklüyken görünüyor (kart 596 → 516 px) |
| R2 | Otomatik playlist yüklenince çalmıyor | yapıldı: küme düzenindeki oynatıcı "Manuel"deyken rotasyon/şema yüklenince "Yarı AutoDJ"ye geçiyor, Başlat hemen etkin; dolu jingle oynatıcısı otomasyonu durdurmuyor (jingle atlanıyor) |
| R3 | İngilizce motor hataları | yapıldı: engine.c hataları Türkçe ve ne yapılacağını söylüyor |
| R4 | Olasılık çekilişi boş dönünce parça zorla "Parça sonunda" | yapıldı: rotasyonun/türün geçişi korunuyor |
| R5 | Ayarlar › Auto DJ: belirgin beat yoksa AutoDJ şarkı sonundaki sessizliği çalmasın; geçiş noktası DJ'lerin yaptığı gibi dalga biçiminin son kısmından bulunsun | tamamlandı: kalıcı IN/OUT ataması güvenilir ritim ızgarası ve waveform vuruşlarıyla ritimli çıkış bölümünü arar; uygun bölüm yoksa ölçülmüş ses sınırlarını kullanır. OUT, çok oynatıcılı geçişin başlangıcıdır; parçanın ses kuyruğu korunur. Doğrulanmış analizde 0,25 sn üstü son sessizliği fiziksel ses sınırından ayıklanır. Tek oynatıcı ritim IN/OUT noktalarında kesilmez; elle belirlenmiş otomatik Cue çıkışları ve manuel çalma korunur. Takvim ile native planlayıcı aynı ses sınırlarını kullanır. Testler: playout 49, takvim 51, olasılık 12 ve native fiziksel ses kuyruğu PCM. |
| R6 | Eski derlemeleri boşalt | yapıldı: `build/` altında 12:30'dan eski 43 derleme/test klasörü (~271 MB) silindi; `build/claude-recovery-20261015` kanıt klasörü de bunlara dahil. Disk hâlâ %99 |
| R7 | Rotasyon şeması türlere göre: sweeper / jingle / reklam gibi türlerden hangisinin şemada olmayacağı seçilebilsin (17.10 akşamı) | yapıldı: Rotasyon düzenleyici › Tekrar kuralları › "Olmayan türler" (Müzik · Reklam · Jingle · Sweeper); işaretli tür o rotasyondan/şemadan hiç çalmaz, Jingle işaretliyse şarkı içi jingle katmanları da uygulanmaz; kayıt `settings.excluded_kinds` (yalnız rotasyon düzeyi; arka uç + C planlayıcı doğrulaması); rotasyon listelerinde ve şema yükleme penceresinde "· yok: Reklam" |

Son test durumu: önceki dört arayüz testi güncel küme yürütücüsüne, gömülü AutoDJ ayar sayfasına ve gerçek oturum davranışına uyarlandı. `named-rotation-picker-AutoDJ-gates-pair-PCM`, `playlist-context-AutoDJ-role-persistence-multi-PCM`, `named-rotation-local-beatmatch-effect-PCM` ve `weekly-programme-future-countdown-due-weighted-repeat-PCM` ayrı özel veritabanları ve gerçek PCM ile geçti. Haftalık test gerçek UTC başlangıcını ve blok bitişini bekler; ilgili zaman aşımı buna göre düzenlendi.


## 15.10.2026 · Son Claude kesintilerinin tamamlanması

21:39'da kesilen `98fd3d0f…` ve 21:38'de kesilen `3b9e0a67…` oturumları kaynak kodla karşılaştırıldı.

- AutoDJ giriş/çıkış ataması ve fiziksel ses kuyruğu/takvim süreleri tamamlandı (R5). Ritimli bölüm algılama beat/waveform sezgisidir; vokal sınıflandırması değildir.
- Sıkışan AutoDJ Başlat düğmesi dar kartta ayrı tam genişlikte satıra geçer; yüksek öncelikli kart stilinde okunur genişliği korunur. Bağlı ve bağlantısız gerçek GTK testleri 1600, 1440, 900 ve 600 px genişliklerde geçti.
- Genel program seçimi GTK ana iş parçacığından immutable veriyle çalışan bir işçiye taşındı; 250 ms'lik görünüm güncellemesi sonraki programı yeniden taramak yerine geçerli önbellekten okur. Stop, niyet, kaynak kimliği, kapanış ve gecikmiş sonuç kontrolleri korunur.
- 174 program / 3480 zaman aralığı testi doğru mevcut/sonraki programı, kaynakların korunmasını ve kapanış iptalini doğruladı; en uzun GTK yanıt aralığı 76,3 ms oldu. Çekirdek haftalık plan testleri 19/19 geçti.
- Ağır kayıt ve normal genel sıra testleri CTest'te ayrı özel veritabanlarıyla çalışır. Normal veritabanında test kaydı oluşturulmadı veya silinmedi; açık uygulama/servis yeniden başlatılmaz.


- Kayıtlı içerik/oynatıcı dağılımı olan rotasyonun dört oynatıcı havuzu iki oynatıcıya daraltılmıyor. Tıklanan kaynak korunur; yalnız aynı küme/kümesiz grup içindeki gerçek kayıtlı havuz kullanılır. Hazırlık sırasında havuzdaki bir oynatıcıya yeni medya yüklenirse başlatma iptal edilir ve mevcut ses korunur. Ek çekirdek kapsam testleri 8/8 geçti.
- Son karışık içerik testi beş içeriği dört doğru oynatıcıdan çaldı, bağımsız beşinci oynatıcının sesini korudu ve CUE sessizliğini doğruladı. Diğer üç arayüz senaryosu da geçti; son kayıtlar `claude-recovery-{routing,named-rotation,local-beatmatch,weekly-real-utc}-final.{log,json}` altında.

Kanıtlar: `build/integration-completion/root/claude-autodj-*`, `claude-freeze-174-schedules.*`, `claude-recovery-autodj-layout*`; derleme kaydı `build/autoplay-fix-20261015/claude-unfinished-promotion-receipt.json`.

## 16–17.10.2026 · AutoDJ noktaları, BPM ve SYNC beatmatch

| # | İstek | Durum |
|---|-------|-------|
| D1 | AutoDJ noktalarının analizi bozuk ("patlıyor"): bazı parçalarda OUT şarkının başında/ortasındaydı (Manzara 236 sn'de 18,5 sn, Ayo Technology 26 sn, Shouse 94,7 sn…); AutoDJ şarkıları erken kesiyordu | düzeltildi: `desktop_autodj.py` yeni algoritma `radios-autodj-mix-v2`. IN ilk ölçü başı (güvenilir ızgara yoksa ilk ses). OUT yalnız şarkının son %45'inde: sondaki düşük enerji dizisi (outro, sessizleşen çıkış, uzun fade) en az 4 ölçü (8 sn) sürüyorsa onun başı; şarkı sonuna kadar güçlü sürüyorsa enerji düşmeden 2 ölçü (4 sn) önce; ızgara varsa ölçü başına hizalı. Eski "ritimli çıkış" algılayıcısı hiçbir gerçek parçada çalışmıyordu, "bölüm değişimi" algılayıcısı OUT'u şarkının başına koyabiliyordu; ikisi kaldırıldı. Düzeltme anında kütüphanedeki 27 müzik parçasının hepsinde OUT sonun %81–99'unda ölçüldü (17.10'daki kullanıcı onaylı sıfırlama kütüphaneyi boşalttı; yeniden içe aktarılan parçalara noktalar D3'teki bakımla atanır). |
| D2 | AutoDJ noktası seçimi: toplu "yeniden hesapla" dalga biçiminde elle konan noktaları eziyordu (ör. Strange 414 sn) | düzeltildi: her nokta kaynağını tutar (`autodj_source`: `manual` / `radios-autodj-mix-v2`). Dalga biçiminden veya işaret düzenleyiciden konan/taşınan nokta elle sayılır ve hiçbir otomatik hesaplama onu değiştirmez; otomatiğe döndürmek için "AutoDJ noktalarını temizle". Mevcut veride LF SYSTEM OUT (230,97 sn) ve P.I.M.P. IN elle olarak korundu. |
| D3 | Noktaların otomasyonu: yalnız Ayarlar › Auto DJ'de düğmeye basınca atanıyordu | yapıldı: `rhythm_jobs.c` arka plan bakımı her 2 dakikada eksik/eski BPM analizlerini tamamlar ve bütün müzik parçalarına eksik/eski otomatik AutoDJ noktalarını atar; yeni BPM ızgarası gelince otomatik noktalar yeniden hesaplanır (elle olanlar kalır). Hesaplanamayan parça (sessiz/erişilemez) oturum boyunca tekrar denenmez. |
| D4 | Tüm şarkılar için BPM | analizör `radios-onset-grid-v3`: açılıştaki 2 dk, ortadaki 2 dk ve 6 dk'ya kadar tamamı ayrı ayrı denenir, en güvenilir sonuç seçilir; vuruş evresi kick bandının yanında tam bant ataklarıyla da aranır (seyrek hip-hop vuruşları). Gerilemesiz: Ayo Technology 0 → 93,35, Shake That 0 → 110, P.I.M.P. 113,6 (yanlış) → 85,17 BPM. Kütüphanenin tamamı v3 ile yeniden analiz edildi. Ric Flair Drip'te güvenilir tempo bulunamadı; BPM'i oynatıcıdaki "BPM ✎" ile elle girilmeli. |
| D5 | Otomasyon geçişlerine "SYNC beatmatch" ve olasılığı | yapıldı: Rotasyon düzenleyici › "AutoDJ: geçiş ve kanallar" sekmesinde geçiş türü "SYNC beatmatch (BPM ve vuruş eşleştirerek)" ve tür başına yeni "SYNC olasılığı (%)" sütunu (`kind_transitions.<tür>.sync_percent`, 0–100; kalan geçişler aynı sürede crossfade). Çekiliş her parça çifti için tekrarlanabilir. Aynı ad Geçiş kuralları sekmesinde, Olasılık düzenleyicide ve Ayarlar › Auto DJ'de ("SYNC beatmatch · 4 vuruş") kullanılıyor; Otomasyon › Kümesizler · AutoDJ ipucu yeri gösteriyor. |
| D6 | Rotasyon düzenleyiciye "AutoDJ otomasyonu" sekmesi: djay benzeri geçiş ön ayarları (ses/bas/filtre eğrileri, mikser efektleri), türlere atama, rotasyon ve programlama; bozuk ayar sayfası düzeni | yapıldı (17.10 akşam): kitaplık alt çubuğunda "AutoDJ otomasyonu…" sayfası (`src/ui/autodj_automation_ui.c`), ön ayar şablonları ve seçim (`src/core/transition_preset.c`), motor bindirme zarfı/efekt sesi (`RadioSAudioRunItem.shape`, `RadioSAudioShapeVoice`), planlayıcı `calendar_transition.shape`, arka uç `autodj_presets`/`autodj_automation`; harf harf kırılan grid/sekme etiketleri sarmasız yapıldı, rotasyon diyaloğu 1180×820. Testler: transition 7, runner 43, calendar_plan 56, playout 54, desktop_automation_calendar (yeni vaka), rotation_editor_regressions (yeni vaka). |
| D7 | "run-radios betiğini ve build kodlarını güncelle" (17.10 gece) | yapıldı: sürüm 0.19.0 (`CMakeLists.txt`, README, packaging notu); `tools/radios_launch.py` derleme başarısız olursa önceden derlenmiş `build/radios` ile uyarı vererek açılıyor (ikili yoksa açılış durur), `RADIOS_SKIP_BUILD=1` derleme adımını atlıyor; yeni test `tests/test_radios_launch.py` (CTest `radios_launch`, 5/5). Paylaşılan `build/` flock altında bütün hedeflerle yeniden derlendi; açık uygulama kapatılmadı, 0.19.0 bir sonraki açılışta gelir. |

Testler: `test_desktop_autodj.py` 16/16 (yeni algoritma, nokta kaynağı, eski noktaların yükseltilmesi), `test_desktop_media` 18, `test_desktop_playout` 24, `test_desktop_programme` 17, `test_desktop_automation_calendar` 35 (sync_percent kabul/ret), C `test_rhythm` 5, `test_media` 11, `test_automation_calendar_plan` 55 (yeni `/automation-calendar-plan/autodj-kind-sync-percent-share`).

Yeni uçtan uca test `rhythm_upkeep` (`tests/test_rhythm_upkeep.c` + `tests/run_rhythm_upkeep.py`, GUI ve ses aygıtı kullanmaz, canlı veritabanına dokunmaz): özel bir veri servisinde sentetik 5 şarkıyla gerçek `library_scan` çalışır. Tam analizli ama noktasız şarkı, ızgarası olmayan şarkı, eski (v1) ızgaralı şarkı → hepsi v3 ızgara + otomatik IN/OUT alır (OUT beats-only outro başında, ölçü çizgisinde); elle konan OUT (30 sn) korunur ve yalnız eksik IN atanır; sessiz parça işaretsiz kalıp "çözülemez" listesine girer; ikinci geçiş hiçbir şey yazmaz. ctest: `rhythm`, `rhythm_upkeep`, `loudness*`, `media`, `playout`, `desktop_media`, `desktop_playout`, `desktop_automation_calendar`, `automation_calendar_plan`, `desktop_probability_policy`, `desktop_playout_vocal_cues`, `playout_event_blocks`, `desktop_autodj`, `automation_probability_*` hepsi geçti.

## 17.10.2026 akşam · radios-61 denetimi (derleme, GUI dışı testler, statik analiz, gerçek günlükler)

Ayrı `build-claude-review/` dizininde (Debug, katı uyarılar, GUI testleri kapalı) derlendi ve denetlendi; paylaşılan `build/` dizinine dokunulmadı.

| # | Bulgu | Durum |
|---|-------|-------|
| K1 | Çevrimiçi müzik (YouTube/Spotify/Apple) kaldırma işi arka uçta yarım kalmıştı (ilgili oturum 18:30'da kota sınırına takıldı): `desktop_playlist._metadata` silinmiş `_online_provider` / `_online_resource_id` / `_online_resource_url` yardımcılarını içe aktarıyordu → **playlist oluşturma, parça kaydetme ve toplu ekleme HTTP 500 `internal_error`**; `put_document` içinde `_online_text` NameError; silinmiş `tests/test_desktop_online_services.py` iki testten içe aktarılıyordu. GUI dışı pakette 17 test düşüyordu. | düzeltildi: `_metadata(value, persisted=False)` yeni kayıtta çevrimiçi kaynakları (provider/external_id/url/online_id/analysis_media) "online playlist entries are no longer supported; use a local media file" ile reddeder (HTTP 422), saklı eski satırları yalnız şekil olarak doğrular; `validate_entry`, `desktop_playout.prepare` ve `validate_policy_context` persisted bayrağını geçirir; `put_document`'taki `_online_text` çağrısı kaldırıldı (eski türler `unsupported configuration kind`). Altı test dosyası yerel medyaya göre yeniden yazıldı (test_playlist_selection, test_playlist_workspace, test_data_playlists, test_desktop_programme, test_desktop_api, test_desktop_playout); eski çevrimiçi satır ve program olaylarının okunur kaldığı, yenilerinin API ve HTTP'de yazmadan reddedildiği ayrıca test edildi. Veri servisi 20:04'te bu kodla açıldı (radios-53). |
| K2 | Rotasyon düzenleyici açılırken `autodj_page` SYNC olasılığı spin'ini 100'e kurarken `changed → validate_stored → status()` henüz oluşturulmamış durum etiketine yazıyordu (`gtk_label_set_text: GTK_IS_LABEL` kritik; fatal-criticals testlerinde abort). `rotation_editor_regressions` ilk vakada düşüyordu. | düzeltildi: varsayılan değer `editor->updating` altında atanıyor; `status()` etiket yokken atlıyor. 14/14 geçti. |
| K3 | `src/audio/output.c` metadata iş parçacığı Icecast mount URL'sini `g_uri_parse` ile yeniden çözerken NULL denetimi yoktu; `[` / `]` içeren sunucu adı geçersiz URL üretip GLib kritikleri ve NULL erişimine yol açabiliyordu. | düzeltildi: ayrıştırma başarısızsa metadata `CURLE_URL_MALFORMAT` ile hatalı sayılır; sunucu adında `[` `]` reddedilir. |
| K4 | GCC 15 `-fanalyzer`: core/audio/playout/media/app (35 dosya) bulgu yok; UI (62 dosya) yalnız `player_tree_ui.c` 58 ve 296'da `topology(ui)` NULL uyarısı. | yanlış pozitif: her iki yol önce `ready(ui)` ister; `radios_player_workspace_ready` doğruyken `radios_player_workspace_topology` her zaman doludur. |
| K5 | Gerçek servis günlüğü: 13.10'da 124 adet `desktop-playout/prepare` 409 (o günkü düzeltmelerden sonra tekrarlamadı); 18:16'daki açılışta `desktop/list` 3×422 (eski ikili artık kaldırılmış bir yapılandırma türünü listeliyordu); 20:04'ten sonra 321 istek, hepsi 200. Uygulama günlüğünde 13.10 21:37'de dört `invalid unclassed pointer in cast to 'GObject'` kritiği var, bugünkü açılışta yok. | izleniyor |

Doğrulama: 0 derleyici uyarısı; CTest 78 testten 76 geçti, 2 atlandı (`audio_recovery`, `audio_soak`: özel Pulse fixture `build/stage13-pulse/pulseaudio` yok). GUI testleri bu turda koşulmadı: uygulama kullanıcıda açık, testler gerçek ekranda pencere açıp ses sunucusunda sink kurar; uygulama kapalıyken `-DRADIOS_GUI_TESTS=ON` ile koşulmalı. İş bölümü: C test kalıntıları, `planner.c` / `automation_calendar_plan.c` içindeki `calendar_youtube` dalları, mikser/efektör ayarları, medya arşivi ayarları, gece planlayıcı ve başlatıcı zaman aşımı radios-a8'de; servis ve uygulama açılışı radios-53'te.

## 17.10.2026 akşam · Altyapıda olup arayüze bağlı olmayan özellikler

Tarama: `include/radios/*.h` altındaki 386 çekirdek fonksiyondan 154'ü arayüzden çağrılmıyordu; bunların büyük bölümü motor içi yardımcılar (çözücü, DSP, yeniden örnekleyici, kurtarma, ritim analizörü) ya da `set_mixer` ile değiştirilmiş eski giriş noktalarıdır. Arka uçtaki 241 iş akışından 186'sı C'den çağrılmıyor; bunlar özgün SDBASE motorunun sunucu API'si (traffic, clocks, podcasts, listeners…) ve masaüstü katmanı aynı işi `desktop-*` iş akışlarıyla yapıyor. `desktop/campaign-delete` ve `desktop/campaign-version` eski yollardır; Reklam ekranındaki "Kampanyayı iptal et" aynı işi `desktop-advert/campaign-state` ile yapar. Gerçek boşluk tek modül çiftiydi:

| # | Özellik | Durum |
|---|---------|-------|
| M1 | `media_archive.c` + `storage_locations.c` + `local_ini.c` (18:17'de yazılmış, `config.c`/`data_client.c`'ye bağlanmış, arayüzü ve testi yok) | yapıldı: **Ayarlar › Medya arşivi** sayfası (`src/ui/media_archive_ui.c`): klasör ekle/kaldır ve durumları, tür, alt klasör playlisti, izleme aralığı; arka planda SHA-256 tarama + kütüphane özetleriyle karşılaştırma + kütüphanenin sıralı içe aktarmasıyla yeni dosyaları alma (`radios_media_import_files`, `media.c`); zamanlayıcı `window.c`'de bağlı |
| M2 | `storage.ini` ile kayıt klasörü | yapıldı: aynı sayfada "Kayıt klasörünü seç… / Varsayılana dön / Şimdi yeniden başlat"; işaret dosyası ve açılış uyarısı (`radios_storage_notice`) sayfada görünür |
| M3 | `storage.ini` ile veritabanı ve yedek klasörü | yalnız görüntüleme: Python başlatıcı/yedek aracı yolların proje içinde olmasını zorunlu kılıyor (`_contained`, `_safe`); taşıma servis kapalıyken elle yapılır. Arayüzden değiştirilebilmesi için Python tarafında ayrı iş gerekir |

Testler: `media_archive` 5/5, `storage_locations` 5/5, `native_preferences` (yeni kategori + ayar dosyası), `native_media_archive` (özel veri servisi: 3 dosya → 2 yeni + playlist, ikinci tarama 1 yeni aynı playliste, üçüncü "0 yeni", bağlantısız sayfa). Belge: `docs/medya-arsivi.md`.

## 17.10.2026 gece · Oynatıcı "Dalga biçimini incele" penceresi (radios-fc)

Kullanıcı isteği: oynatıcı penceresinin arayüzü medya kütüphanesindeki "Dalga biçimi ve parça işaretleri" paneli gibi olsun.

- Pencere artık aynı bileşeni gömüyor (`radios_marker_editor_embed`): işaret düğmeleri (AutoDJ IN/OUT, Geçiş noktası, Fade başlangıcı/sonu, Intro), Konum satırı, Oynat/Duraklat/Durdur/CUE + çıkış seçici + ön dinleme düğmesi, etiketli işaretlerle zaman çizgisi, Seçim IN/OUT/Seçimi kırp, Birlikte çalma, +/−/Tüm süre, Geri al/İleri al, İşaretleri sıfırla, **İşaretleri kaydet**. Eski özel çizim, üstteki +/−/Tüm parça/İmleç çubuğu ve hemen kaydeden AutoDJ IN/OUT düğmeleri kaldırıldı.
- "BPM ve dörtlük ızgarası" çerçevesi korunuyor; ızgara düzenleyicinin zaman çizgisine çiziliyor (yakınlaşınca vuruş, uzakta dörtlük çizgileri). "Seçili nokta 1. vuruş" düzenleyicideki imleci kullanır. Altında "Noktaları temizle" (AutoDJ IN/OUT'u kaydedilmemiş düzenleme olarak siler) ve "Dörtlük başına hizala" (açıkken AutoDJ IN/OUT düzenleyicide konulurken en yakın dörtlük başına kayar) duruyor; AutoDJ IN/OUT durum satırı kalıyor.
- Kayıt akışı: düzenleyicinin "İşaretleri kaydet"i veritabanına yazar; onay gelince oynatıcı kartı önbelleği güncellenir (analiz/işaret revizyonlarının en yenisi korunur). Kütüphaneden ya da analizden gelen yeni revizyon, temiz (kaydedilmiş) paneli yeniler.
- Kompakt kart dalga biçimi ve sağ tık menüsü (AutoDJ IN/OUT/temizle/hizala, hemen kaydeden) değişmedi.
- Yeni düzenleyici API'si: `radios_marker_editor_set_grid/set_snap/cursor/clear_autodj/set_saved` (include/radios/editors.h).

Testler: `player_waveform_markers` artık radios_ui ile bağlanıp (`--wrap` ile veri/ses yedekleri) 14 vaka; iki vaka yeni akışa göre yeniden adlandırıldı (jenerasyon değişince kayıt yine medyaya yazılır ve benimsenir; sürüm değişince pencere kapanır). `native_playout` içindeki iki dalga biçimi vakası düzenleyici denetimlerine (qwf-timeline, qwf-set-8/9, qwf-markers-save) geçirildi.

## 17.10.2026 akşam · Otomasyon tasarımında tekrar eden/çelişen yerler (radios-fc)

Kullanıcı isteği: "otomasyon kısmında tasarımda çelişkili tekrar eden yerleri kaldır; kaç defa tekrar etsin özelliklerini (playlist, sanatçı vb.) Gelişmiş'te tut."

| Yer | Önce | Sonra |
|-----|------|-------|
| Rotasyon düzenleyici › Gelişmiş · genel kurallar | üç sekme: Tekrar kuralları, **Geçiş kuralları** (tür başına ağırlık/süre/efekt + doğrusal/olasılıklı), AutoDJ: geçiş ve kanallar | iki sekme: Tekrar kuralları (şarkı/sanatçı/albüm tekrarı dk, kuşak sonu taşma, gevşetme) ve AutoDJ: geçiş ve kanallar. Geçişler tek yerden: "Olasılık ve geçişler…" (müzik→müzik yüzdeleri) + AutoDJ sayfası (içerik türü ve kanallar) |
| Playlist › Gelişmiş · playlist kuralları › "Bu playlistin tekrar ve geçiş kuralları" | tekrar + geçiş sekmeleri | yalnız "Bu playlistin tekrar kuralları" (şarkı/sanatçı/albüm tekrarı dk); playlist override artık `transitions` yazmaz |
| Playlist › "Oynatıcı ayarları…" düğmesi | "Oynatıcı ve geçiş ayarları…" ile aynı sayfayı açan ikinci düğme | kaldırıldı (tek düğme kaldı; sağ tık › AutoDJ kısayolu duruyor) |
| Takvim › seçim çubuğu | "Rotasyon oluştur" programı oluşturuyordu (iki rotasyon düğmesinin yanında) | "Program oluştur"; pencere "Program oluştur / Program özellikleri"; boş durum metni güncellendi |
| Takvim › rotasyon ve kayıtlı rotasyon tabloları | gizli "Seçim / Parçalar / Reklam / Sweeper" sütunları yalnız "Olasılık / Ayarlar" yer tutucuları gösteriyordu | kaldırıldı; rotasyon tablosunda Ağırlık sütunu "Diğer seçenekler › Rotasyon ağırlığı sütununu göster" ile açılır |

Tutulanlar (Gelişmiş altında): playlist "Her N turda / Parça adedi / Ağırlık / En az aralık / Başlangıç-Bitiş / Medya filtresi", şarkı "Her N turda / En fazla / En az aralık", rotasyon ve playlist düzeyinde şarkı/sanatçı/albüm tekrar dakikaları.

Veri uyumluluğu: kayıtlı rotasyonlardaki `settings.transitions` dizisi olduğu gibi korunur (planlayıcı yine okur, varsayılan "parça sonu"); yeni rotasyon varsayılan diziyle kaydedilir; playlist override kapatılınca eski geçiş alanları da temizlenir. Backend şeması değişmedi.

Testler: `rotation_editor_regressions` (14 vaka; iki vaka tekrar-kuralı alanlarına çevrildi), `native_calendar_regressions` (5), `native_automation_probability` (9), `native_automation_calendar` ("transition-tabs…" vakası "repeat-rules-tabs-persistence-override" olarak yeniden yazıldı; sütun sayıları güncellendi).

## 17.10.2026 akşam · radios-a8: kesilen Claude oturumlarının ve Kimi'nin yarım işleri

18:30 civarında yedi Claude oturumu (radios-f8/15/a1/38/63 ve diğerleri) oturum sınırına takıldı; Kimi (k3) 5 saatlik kotasını, Codex ise kullanım sınırını (22.10'a kadar) doldurdu. Bu oturum (radios-a8) yarım kalan işleri transkriptlerden çıkardı, dört alt ajana dağıttı ve doğruladı. Eşzamanlı radios-61 arka uç çevrimiçi kalıntısını ve rotasyon düzenleyici çökmesini, radios-53 ise servis açılış süresini ve uygulamanın açılmasını üstlendi.

| # | Yarım kalan iş (kaynak oturum) | Durum |
|---|-------|-------|
| Y1 | "Program açılmıyor" (kullanıcı, 19:02; radios-a1) | kök neden: veri servisi mağaza açılışında 290 MB checkpoint'i okuduğu için ~15 sn'de hazır oluyor, başlatıcı 8 sn bekliyordu. radios-53 bekleme süresini 60 sn yaptı; uygulama 20:04'te açıldı (pid 415335), açık bırakıldı |
| Y2 | Ana iş parçacığı boşta %66–98 CPU (radios-a1'in gördüğü donma belirtisi) | düzeltildi: gdb örnekleri iki kaynağı gösterdi — yayın kartı tiki (100 ms) her seferinde pencere ağacını isimle tarıyordu (`library_find`), üst çubuk ilerleme tiki (50 ms) 32 deck için ayrı motor snapshot'ı alıyordu. Kanal kutusu/şeritler zayıf işaretçiyle önbellekte; `radios_audio_ui_get_snapshots` tek snapshot'tan bütün deckleri verir. Yeni sürüm bir sonraki açılışta gelir |
| Y3 | YouTube/çevrimiçi müzik kaldırmanın kalıntıları (radios-f8, radios-15) | tamamlandı: derlenemeyen altı test hedefi (playlist_browser_ui, player_loader_ui, live_players_ui, header_navigation_ui, header_progress, local_event_prepare_ui) yalnız yerel medya senaryolarıyla yeniden yazıldı; catalogue_playlist_ui, broadcast_catalogue_ui, playout_ui, automation_calendar_plan ve playout testlerindeki çevrimiçi vakalar silindi (yerel kapsam için yeni vakalar: `general-header-play-local-PCM`, `library-picker-skips-unbound`, `programme-local-identity-roundtrip`, `local-unknown-duration-unsupported-mapping`). Kaynakta `catalogue_playlist.c` provider dalı, `reset_ui.c` çevrimiçi tür etiketleri, `automation_calendar_plan.c`/`planner.c`'deki youtube/calendar_youtube dalları kaldırıldı. Arka uç kalıntısı (desktop_playlist/desktop_api/desktop_playout/probability_policy + 6 Python testi) radios-61'de kapandı. Doğrulama: tüm ağaç `ninja -k 0` temiz; ekransız 14 paket geçti; özel Xvfb koşucusuyla playlist_browser_ui 3/3, player_loader_ui 7/7, live_players_ui 16/16, header_progress 28/28. Çalıştırılmayanlar: native_catalogue_playlist, native_broadcast_catalogue, native_header_navigation, native_playout, native_local_event_* (koşucuları kullanıcının Pulse sunucusuna pactl uyguluyor ya da kendi Xvfb'sini açmıyor). Ardından yayın sırasındaki son kalıntı (`programme_queue.c` "✓ YouTube Music" etiketi, `RADIOS_DRAG_ONLINE` hedefi) ve `tests/README.md`'nin çevrimiçi bölümleri kaldırıldı; programme-queue testi özel ekranda 17/17 geçti. README'deki 0.18.0 notu güncellendi |
| Y4 | Mikser ve efektör için ayrı genişlik/yükseklik ayarları (radios-63, 18:21 isteği) | tamamlandı: Ayarlar › Görünüm'de dört satır (Mixer genişliği/yüksekliği, Efektör genişliği/yüksekliği, %50–150). Genişlik yalnız o bölümün yatay uzunluklarını, yükseklik yalnız dikey uzunluklarını ölçekler (RadioSContentScale x/y ayrı çarpan); efektörün iç yükseklikleri efektör yüksekliğini izler. INI'de yükseklik anahtarı yoksa genişlik değeri kullanılır; DB belgesinde yoksa yerel yükseklik korunur (settings_sync + desktop_api doğrulaması). Ayrıca `content_scale.c`'de iki verimsizlik giderildi: her tahsis geçişi offscreen içeriğin tamamını, her hasar olayı gömücünün tamamını yeniden boyatıyordu (boşta CPU'ya katkı); `window.c` kabul yakalaması çizimden önce bekleyen tahsisi bitiriyor (release_install smoke `alloc_needed` kritiği). Testler (özel Xvfb): ui_preferences 20, settings_sync 3/3, content_scale_module 10/10 (yeni boşta-yeniden-boyama-yok senaryosu), content_scale_ui 2/2, header_status 11/11, test_ui DJ ölçek senaryoları; release_install smoke 3/3 |
| Y5 | Ayarlar › Medya arşivi: arşiv klasörleri (okuma) ve radyo verilerinin yazılacağı yerler (7819922c, 00:51 isteği) | tamamlandı: sayfanın kendisini (src/ui/media_archive_ui.c, Ayarlar'da Auto DJ'den sonra "Medya arşivi") eşzamanlı oturum radios-d4 yazdı — arşiv klasörleri listesi + durum, Klasör ekle…/kaldır, yeni dosyaların türü, alt klasör başına playlist, "RadioS açıkken izle · her N dk", "Şimdi tara ve içe aktar" (GTask'ta tarama, kütüphane SHA-256 özetleriyle yeni dosya tespiti, içe aktarma ve sayımlar), "Radyo verileri" bölümünde Veritabanı/Kayıtlar/Yedekler klasörleri, kullanılan ve boş alan, yeniden başlat. Bu oturumun alt ajanı eksik kalan Python tarafını yaptı: tools/radios_database.py config/storage.ini'yi C ile aynı kurallarla okur (database_dir taşıma: .sdb yan dosyaları + .objects, eski servisi kapatma, `stale_database` durumu; backup_root), desktop_reset.py yedekleri backup_root'a yazar; testler tests/test_local_ini.c (6), tests/test_storage_locations.py (10), test_media_archive/test_storage_locations (C) ve cmake/MediaArchiveService.cmake; test_database_setup 15/15, test_desktop_reset 7/7. Ardından Veritabanı ve Yedekler satırlarına da Seç…/Varsayılana dön eklendi (radios-d4 sayfayı 12 dk'dır düzenlemiyordu; yama tam eşleşmeli, -Werror temiz); docs/medya-arsivi.md güncel. GUI testi native_media_archive bu turda koşulmadı |
| Y6 | 7/24 şema "gece" kuşağı: "Tempo ayarlı gerçek çalma sırası … tekrar süresini karşılamıyor" (radios-38) | düzeltildi: çekirdek seçim (`automation_calendar_plan.c`) aday başlangıcını Geçiş kuralları sekmesindeki ipucuna göre tahmin ederken yerel planlayıcı rotasyonun AutoDJ tür politikasının (`kind_transitions`) bindirmesini uyguluyordu; her geçişte bindirme kadar erkene kayıp 40 dk'lık parça aralığı 2383 s'ye düşüyordu. Artık `candidate_start` çift için gerçek sınırı (`kind_transition`) kullanıyor. Yerel planlayıcı (`planner.c`) tekrar/dönüş aralığı ihlalinde (trim açıkken, ilk öğe hariç) planı o öğede kesiyor (`calendar_repeat_gap_trimmed` / `calendar_turn_interval_trimmed` tanısı); `playout_ui.c` bu mesajları WOULD_BLOCK'a çevirip kapsam yürütücüsünün 2 sn sonra yeniden seçmesini sağlıyor. Testler: `/playout/calendar-repeat-gap-trims-compressed-timeline`, `/automation-calendar-plan/kind-transition-estimate-matches-native-repeat-gap`; playout 56, automation_calendar_plan 56, transition, runner, playout_policy, automation_probability_* ve playout_event_blocks paketleri geçti. Canlı 7/24 verisi sıfırlandığı için gerçek gece kuşağı yeniden koşulamadı |
| Y7 | "Playlist yetmese duruyor" ve yayın sırası oynatıcı etiketi/seçicisi (4f875c76) | kodda bitmişti; broadcast.c ekleri kaldırma sonrası yerinde (9 referans), yeni derlemeyle gelir. Panelde "Event yok / Takvimde gelecek program aranıyor" görünmesi sıfırlama sonrası program tanımlı olmadığındandır |
| Y8 | Kimi: "son 7 Claude sohbetini çıkar, yaptım demesine inanma, doğrula" | Kimi'nin üç alt ajanı da kota hatasıyla hiç çalışmadı; doğrulama bu tabloda yapıldı (her satırda kanıt) |
| Y9 | Kimi: "ChatGPT Codex yolunu sıfırla" (kullanıcı Kimi'ye TAM SIFIRLAMA seçmişti) | yapılmadı: ~/.codex (2,8 GB) ve ~/.config/Codex (213 MB) ChatGPT uygulaması ve VS Code Codex eklentisi tarafından açık; geri alınabilir yeniden adlandırma bile otomatik mod denetimince reddedildi. Kullanıcı kendisi yapmalı |
| Y10 | README'deki 0.18.0 çevrimiçi müzik notu | güncellendi: kaldırma notu, "Sağlayıcılar" paragrafı yerel arşive göre yazıldı |
| Y11 | Oynatıcı kartında "Çıkış kanalı" altına AutoDJ'nin hangi oynatıcıda olacağını seçen kutu (radios-3d, 20:44 isteği) | yapıldı: "AutoDJ oynatıcısı" satırı çıkış kanalının hemen altında; kümesiz oynatıcıda Yarı AutoDJ ve AutoDJ geçiş modlarında görünür, Manuel'de gizli (orada zaten seçilemez; kare kartın eski geometrisi korunur). Eskiden yalnız "AutoDJ geçiş" modunda "Devam oynatıcısı" adıyla görünüyordu. Seçenekler aynı gruptaki diğer oynatıcılar ve moda göre "Otomatik devam" / "Yalnız bu oynatıcı"; bir oynatıcı seçmek modu AutoDJ geçişe alır. AutoDJ çalışırken seçim kilitli (yürütücü çalışan bağlamın hedefini değiştirmeyi reddediyor; önce Durdur). Etiket tek satır. src/ui/broadcast.c. native_live_players, native_automation_workspace(+acceptance), data_client geçti. |
| Y12 | Oynatıcıda "Otomasyon · hata" ("İşlem alanları geçersiz … playback preparation exceeds the 1500000-byte response budget"; 458 parçalık Müzikler playlistleri ≈ 1,7 MB, parça başına ≈ 3,7 KB ayrıntı) — "hata diyor otomatikleştir" (21:05, radios-3d) | düzeltildi: arka uç hazırlık bütçesi 2000 satır / 1,5 MB → 10.000 satır / 40 MB (`desktop_playout.py`, mesajlar sabitten üretiliyor; `desktop_programme.py` aynı sabiti kullanır); yerel istemci `/v1/workflows/desktop-playout/prepare` ve `/v1/workflows/desktop-programme/prepare` için istek ve yanıt sınırı 40 MiB (`data_client.c` PLAYOUT_LIMIT; diğer yollar 2 MiB). `test_desktop_playout.py` sınırları patch ile sınar ve 2001 satırlık sıranın kabul edildiğini doğrular; desktop_playout 49, desktop_programme 43, data_client 19 geçti. Başlatıcı `source_digest` değişince servisi kendisi yeniden başlatır: uygulamanın bir sonraki açılışında geçerli. |

Doğrulama (radios-a8): paylaşılan `build/` dizininde 365 hedefin tamamı derlendi (`cmake --build build -- -k 0`, 20:45; `build/radios` yeni sürüm, açık uygulama kapatılmadı, bir sonraki açılışta gelir). GUI dışı CTest paketi özel dizinde (build-a8-main, GUI testleri kapalı) güncel kaynaklarla koşuldu: 83 testin 81'i geçti, 2'si atlandı (`audio_recovery`, `audio_soak`: özel Pulse fixture yok), düşen yok; 0 derleyici uyarısı. GUI kabul testleri yalnız özel Xvfb koşucusuyla koşulan paketlerle sınırlı kaldı (tabloda belirtildi); kullanıcının ekranı ve ses sunucusu kullanılmadı.

## 17.10.2026 gece · "Auto algoritması" isteklerinin izi ve sessizlik koruması

İstenen (ChatGPT 11.10 §33/§37, G2/G7; Claude 13.10 ertelenen tur, 16.10 şikâyet) ve kodda bugünkü karşılığı:

| İstek | Durum |
|---|---|
| Seek AutoDJ'yi bozmasın (K-1) | yapılmış: `engine.c` seek_command çalıştırıcıya duyarlı; `song_seek` 10/10, `runner` seek vakası |
| Dalga biçiminde AutoDJ IN/OUT, elle noktalar korunur | yapılmış (D2) |
| Sürekli 4'lük (bar) beatmatch geçiş, tempo dönüşü | yapılmış (D5/R5); `tempo`, `runner` PCM vakaları |
| "AutoDJ çoğu durumda bozuluyor" (K-2 devralma eşitliği, Y-2 takvim/plan süreleri, O-1 uzatma yeniden deneme, D-2 süre tahmini) | yapılmış: devralma kapsama kontrolü, `estimated_duration`, `extension_retry_at`; Y-2 bugün `kind_transition` tahmini ile kapandı |
| Tür başına kanal dağıtımı, geçiş kuralları (efekt / yerel beatmatch), olasılık + deterministik | yapılmış (A2, D5): Rotasyon › "AutoDJ: geçiş ve kanallar", olasılık düzenleyici (tohumlu) |
| Rekordbox benzeri beat analizi, tüm şarkılara BPM, LUFS | yapılmış (G4/G5/D3/D4); paralel analiz ve Ayarlar › Kütüphane analizi başka oturumda (17.10) |
| Geçiş eğrisi/efekt ön ayarları ("AutoDJ otomasyonu" sekmesi) | başka oturumda (radios-cc) sürüyor |

13 AutoDJ test grubu (`playout`, `runner`, `tempo`, `transition`, `automation_calendar_plan`, `automation_probability_playout`, `playout_event_blocks`, `rhythm`, `song_seek`, `desktop_autodj`, `desktop_playout`, `desktop_automation_calendar`, `desktop_probability_policy`) geçti.

Eksik bulunan endüstri standardı: **sessizlik dedektörü** (AC-09, "Sessizlikte otomatik devralma" yalnız alan taslağıydı). Yapıldı: `src/core/silence_detector.c`, `src/ui/silence_guard.c`, Acil sayfası ölçer/etiket, `window.c` bağlaması; testler `silence_detector` 3/3 ve `native_silence_guard`. Belge: `docs/sessizlik-korumasi.md`.

