# Native tasarım aktarımı

Kullanıcının istediği görsel kaynak `design/reference/tasarrım/index.html` ve onun yükleme sırasındaki CSS dosyalarıdır. İlk genel iskelet kaldırıldı; arayüz bu tasarımın panelleri, sekmeleri, kontrolleri ve yerleşimiyle C/GTK 3 içinde yeniden kuruldu. Kaynak HTML/JS programda çalıştırılmaz.

## CSS kullanımı

`tools/css_to_gtk.py`, HTML'in style bloğunu ve bağladığı 29 CSS dosyasını sırasıyla okur. Kaynak renk, kenarlık, boşluk, font ve GTK'nin desteklediği gradyan kuralları uyarlanır. `#id` ve sınıf adları korunur; native widget adları/sınıfları bunlara bağlanır. HTML metin etiketleri GTK label içinde ayrı semantik sınıflara, selected/pressed GTK checked durumuna çevrilir. Ayrı native dialog kökleri tarayıcıdaki body atasına ihtiyaç duymadan kaynak dialog seçicileriyle eşleşir. Sekiz haneli hex renkler rgba'ya, desteklenen sabit CSS değişkenleri kendi kaynak değerlerine dönüştürülür.

Aktarım **30 kaynaktan 1.552 GTK kuralı** üretir. CMake kaynak CSS değiştiğinde otomatik tekrar üretir. Çıktı `build/generated/reference.css`; kaynak/SHA kaydı `css-manifest.json`; C içine gömülen çıktı `reference-css.c` dosyasıdır. Python yalnız build aracıdır; C masaüstü programı CSS okumak için Python veya tarayıcı başlatmaz.

GTK CSS, tarayıcı CSS motoruyla aynı değildir. Web grid/flex/media yerleşimleri native GtkGrid/GtkBox/scroll bileşenlerinde uygulanır. GTK düğüm ölçüleri ve doğal widget farkları `assets/themes/radios.css` adaptasyonuyla tamamlanır; tasarımın CSS renkleri/fonts/çerçeveleri terk edilmez. Kaynak font `assets/fonts/InterVariable.woff2`, uygulamaya Fontconfig ile doğrudan yüklenir; sistem font ayarı değiştirilmez. OFL lisansı fontun yanındadır.

## Native bileşenler

| Kaynak | Görünüm |
|---|---|
| `header.c` | Radio/S marka, yayında/devam/sıradaki, UTC saat/program, Monitor/Kulaklık, master/EQ |
| `instruments.c` | Cairo analog saat, knob ve boş stereo göstergeler; drag/scroll/klavye/reset |
| `broadcast.c` | Sol event kuyruğu + alt geçmiş; sağ katalog/canlı oynatıcı sekmeleri; transport/geçiş/yan araçlar |
| `playlist.c` | Arşiv özeti, playlist sidebar, arama/parça tablosu ve XML seçici/düzenleyiciler |
| `automation.c` | Rotasyon/plan sekmeleri, kurallar, kaynaklar, seçim politikaları ve özet |
| `dj.c` | Beş effector, konuşma şeridi, sekiz kanal + MASTER, sampler padler |
| `emergency.c` | Durum/komut şeridi, hazırlık çipleri, zincir, iki sıra ve beş ayar bölümü |
| `advert.c` | Bildirimler, kampanya tablosu/detayları, mutabakat ve kampanya düzenleyicisi |
| `technical.c` | Sunucular, yayın kişileri, web erişimi ve aygıt/görünüm sekmeleri |
| `widgets.c`, `window.c` | Ortak native form/tablo/panel, ana pencere, gezinme ve olay teslimi |
| `editors.c` | A/B dosya seçimi, cue/geçiş alanları, slider ve parça işareti zaman çizgisi; sürükleme/klavye/zoom/geri alma |
| `core/ui_preferences.c` | Görünüm tercihleri için sürümlü, sınırlı ve atomik yerel INI kaydı |

Sekmeler, girişler, seçimler, diyaloglar, yerel oyuncu/kanal ekleme-kaldırma ve knob tercihleri çalışır. DJ prova sayacı gerçek monotonic zamanı kullanır. Bunlar ses motorunu veya DB yazımını uygulanmış saymaz. Gerçek DB kayıtları yüklenmez; HTML'deki örnek yayın listesi üretim verisi yapılmadı. Kullanıcının elle eklediği playlist/parça/acil sıra kayıtları açıkça **oturum taslağıdır**. Ses ölçümü yokken göstergeler boş kalır. DB yazımı gerektiren Kaydet, bağlantının henüz kurulmadığını açıklar ve dialogdaki girişleri korur.

## Aşama 4 etkileşim kapsamı

| Görünüm | Tamamlanan GUI davranışı | Veri/ses bağlantısına kalan |
|---|---|---|
| Ortak başlık | Gerçek UTC saat, kaynak/çıkışa bağlı MON/CUE tercih durumu, Ayrıntı ve destek notu penceresi, Yedek → Acil gezinmesi | Sağlık gözlemleri, fiziksel yönlendirme, bildirim teslimi |
| Yayın | Native oynatıcı ekle/kaldır ve kimlikleri; program/öğe/hazırlık/tarih/timing/revizyon düzenleyicileri; katalog odağı; ortak cue/geçiş editörleri | Kalıcı program revizyonu, katalog ve canlı transport |
| Playlist | Kullanıcı girdisiyle liste/parça taslakları, rename/silme onayı, seçim, sıralama, arama, 100 satır sayfalama, kaynak ayrıntısı, CSV/M3U8 taslak dışa aktarımı | Gerçek arşiv kimliği, medya doğrulaması, DB içe aktarma |
| XML önizleme | Salt okunur gerçek rekordbox XML listeleri/parça/cue sayımı ve seçimler; iptal edilebilir worker; 8 MiB/500 liste sınırı; normal yerel dosya şartı | Medya kopyalama ve transaction ile kalıcı içe aktarma |
| Otomasyon | Dinamik playlist kaynakları, ağırlık/adet/gün/saat ve sıra düzenleme; koşullu alanlar; takvim; native deck ve playlist taslağı seçenekleri; doğrulanan kural taslağı ve JSON dışa aktarımı | Gerçek medya seçimi, süre/tekrar hesabı ve zamanlayıcı yürütmesi |
| DJ | Kanal/pad atama, kaldırma ve seçim; FX kaynağı/ON/M/S tercihleri ve gönderim listesi; prova sayacı | Decoder, capture, sampler tetikleme, gerçek gain ve efekt DSP |
| Acil | İki sıra taslağı, ekle/düzenle/kopyala/sıralama/kaldır ve toplam süre; A/V/S/B zaman işaretleri; hazırlık/duyuru/sessizlik/erişim/devir/tatbikat formları; gerekçeli iptal ve INI dışa aktarımı | Gerçek devralma, USB/izin kontrolü, dosya doğrulaması ve kanıt |
| Reklam | Kampanya, kuşak, sürüm ve sonuç formlarının typed alanları, tarih/saat/sayı doğrulaması ve seçim gerekçeleri | Kampanya/spot kimlikleri, yetkili onay ve yayın mutabakatı |
| Teknik | Sunucu/kişi typed formları; URL/tarih/vardiya ve koşullu rol/kapsam alanları; web formu; yerel görünüm tercihi | Gerçek servis/kimlik/web erişimi, aygıt ve MIDI bağlantısı |

Parça işaretleri editöründe süre, cue sırası, fade çiftinin sırası ve bindirme sınırı doğrulanır. Zaman çizgisi ölçülmüş dalga biçimi içermez; yalnız elle girilen süre/işaretleri gösterir. Mouse ve klavye ile konum seçme, işaret sürükleme, zoom, geri al/ileri al ve sıfırlama yerel düzenleme modeline bağlıdır. Geçiş editöründe A/B dosya yolları, sayısal cue/geçiş alanları, kilit ve ön dinleme seviyesi uygulanmış oturum taslağında korunur. Vazgeç, uygulanmamış alan değişikliklerini geri alır. Ses ön dinlemesi devre dışıdır.

`config/ui-preferences.ini` sürüm 1; kompakt/standart/büyük yazı ve başlangıç/son profili saklar. Yazı ölçeği her defasında özgün CSS kaynağından 0.90/1.00/1.15 olarak üretilir, tekrar değiştirince birikmez. Kaydetme atomik, dosya izni 0600, okuma sınırı 8 KiB'dir. Token/aygıt/API ayarı bu dosyaya yazılmaz. Kapanış son profili korur; smoke testi tercih dosyası yazmaz. Bozuk dosya normal ayar hatasıyla raporlanır.

## Eventler · Yayın sırası ve katalog (2026-10-07)

Sol listedeki program, HTML tasarımındaki gibi **eventlerden** (bloklardan) oluşur. Her event satırında numara kutusu ve planlanan saat, "Event · tür", kalın ad, "tür · N parça / içerik", toplam süre, **S** (başlat), **V** (içeriği aç / cue), **Ö** (özellikler), **×** (kaldır) ve hazırlık durumu (✓ Hazır, ✕ Dosya eksik, ○ Kontrol bekliyor) bulunur. Event açılınca içerikleri 1.1, 1.2… satırları olarak görünür. Başlıkta "N event · M içerik" ve toplam süre, transport altında içeriğin nereye ekleneceği ("Event: … · Seçili içeriğin sonrasına ekle") yazar.

- Veri: `config.events` oynatılan düz sıra olarak kalır; `config.blocks` eventleri tutar ve her içerik `block_id` ile eventine bağlıdır (`desktop_api.py`: `PROGRAMME_BLOCK_KEYS`; `src/core/programme_model.c`). Bloksuz eski taslaklarda her içerik kendi eventinde, içerik kimliğinden türetilen sabit bir kimlikle gösterilir; ilk kayıtta bloklar yazılır.
- **+ Müzik / Anons / Reklam eventi** sona boş event açar. Yan düğmeler: **Araya ekle** (seçili içeriğin sonrasına), **Değiştir** (seçili içeriği), **Girdi** (eventin sonuna) ekleme kipini seçip kataloğu açar; **Temizle** seçili eventin içeriğini boşaltır. ⇈ ↑ ↓ ⇊ seçili eventi eventler arasında, seçili içeriği kendi eventinde taşır.
- Sürükle-bırak: katalog satırları ve akış satırları listeye, katalog ve akış satırları canlı oynatıcı kartlarına bırakılabilir (`src/ui/programme_queue.c`).
- Katalog tür düğmeleri: Parçalar, Jingle, Anons ve Reklam medya arşivini türe göre süzer; **Programlar** kayıtlı program taslaklarını (ardından program türündeki medyayı) listeler. Seçili programda **Programı ekle** onu tek bir program eventi olarak ekler, **Mevcut taslağı değiştir** taslağın yerine yükler. YouTube Music kaynağında yalnız Parçalar vardır; başka tür seçilince kaynak Medya arşivine döner. Seçili içeriğin sanatçı, albüm, süre ve kaynak bilgisi düğmelerin altında görünür.
- Kaldırılan tekrar: katalogdaki ayrı **Program taslakları** satırı (açılır liste + "Mevcut taslağı değiştir") kaldırıldı; aynı kayıtlar ve işlem artık **Programlar** türünde, katalog tablosundadır.
- Playlist ve Otomasyon sayfalarında GTK kutu aralığı, tasarım CSS'inin kendi kenar boşluklarının üstüne ekleniyor ve aralıkları ikiye katlıyordu; sayfa kutularının aralığı sıfırlandı, aralıklar HTML'deki CSS değerlerinden gelir. Playlist yüklendiğinde alt çubukta çıkan açıklama metni kaldırıldı.

## Görsel kabul

Geliştirme sırasında orijinal tasarım yedi profilde 1920×1080 görüntülendi; `build/reference-<profil>.png` ve `reference-metrics.json` ölçü kaydı alındı. Bu tarayıcı yalnız geliştirme karşılaştırması içindir. C uygulaması gerçek Wayland penceresinde yedi profili ayrı açılışlarla gösterdi; native görüntüler aynı boyutta alındı ve yerleşim/font/panel hataları düzeltildi. GTK CSS ayrıştırması ve katı C11 derlemesi geçti.

2026-10-06: katı C11 derlemesi ve CTest **6/6** geçti. Native etkileşim testindeki **10 grup**; alan doğrulama, dosya/seviye korunması, iptal/geri alma, taslak etkileşimleri ve açık düzenleyicilerle kapanışı kapsar. Görünüm dosyası **9 test grubu** ve Valgrind denetiminden; UI kontrolleri ASan/UBSan denetiminden geçti. Ek hedefli GTK kabulünde header kaynak/çıkış koşulları, FX kaynak listesi, tarih/saat ilişkileri, hazırlık snapshot'ı, XML boyut/liste sınırı ve iptali, sayfalama/arama/sıralama doğrulandı. Geliştirme harness'leri `build/` altında; kalıcı regresyonlar `tests/test_ui.c` ve `tests/test_ui_preferences.c` içindedir.

Aşama 4'ün GUI kapsamı tamamlandı. Gerçek arşiv/waveform/medya kabulü aşama 7, veri servisi aşama 5, C istemcisi aşama 6, gerçek ses motoru aşama 8 kapsamındadır. Ana index'e bağlı olmayan eski `settings.fragment.html` ayrıca sekizinci ekran olarak eklenmedi. Native bileşenlerdeki doğal çizim farkları nedeniyle bu kabul tarayıcı görüntüsüyle piksel eşitliği anlamına gelmez.

GTK'nin native dialog ve çizim API'leri: [GtkDialog](https://docs.gtk.org/gtk3/class.Dialog.html), [GtkDrawingArea](https://docs.gtk.org/gtk3/class.DrawingArea.html). HTML davranışları altyapı olarak çalıştırılmaz; formlar ve yerel durum C içinde tanımlanır.
