# RadioS geliştirme aşamaları

Hedef: tasarım referansına uygun, veritabanına bağlı, gerçek ses çıkışı veren yerel C masaüstü uygulaması. Tüm geliştirmeler `kaynak kodlar/RadioS/` altında yapılır. HTML/CSS/JS yalnız görsel referanstır.

**Güncel 0.17.0 kapanış:** aşağıdaki aşama durumları tarihsel kabul kapsamlarıdır. Veritabanı motoru/sözleşmeleri korunur; bütün native tasarım işlevleri ve saha kabulü tamamlanmış değildir. Sonradan bulunan açıklar ve bağlanan işlevler [proje kapanış denetiminde](proje-kapanis-denetimi.md) kayıtlıdır.

| Aşama | İş | Tamamlanma koşulu | Durum |
|---:|---|---|---|
| 1 | İşlev haritası | Ekran/kontrol grupları → mevcut DB/API veya boşluk → C işi → kabul ölçütü; kaynak doğrulaması | Tamamlandı — 2026-10-06 |
| 2 | RadioS proje düzeni | C kaynak, arayüz kaynakları, DB kaynakları, yapılandırma, medya/veri, araç ve belge dizinleri düzenlenmiş | Tamamlandı — 2026-10-06 |
| 3 | C uygulama altyapısı | Derlenebilir yerel masaüstü iskeleti, yaşam döngüsü ve iş/durum kuyrukları | Tamamlandı — 2026-10-06 |
| 4 | GUI geliştirmesi | Yedi profil, ortak alanlar ve dinamik kontroller native C bileşenleriyle çizilmiş | Tamamlandı — 2026-10-06; veri ve ses işlevleri sonraki aşamalarda |
| 5 | Veritabanı kurulumu | RadioS içine alınan SDBASE kaynaklarıyla ilk DB/kimlik/servis kurulumu | Tamamlandı — 2026-10-06; C veri bağlantısı aşama 6 |
| 6 | C veri bağlantısı | Gerçek okuma/yazma, revision/kimlik/izin/hata ve eksik iş akışları uygulanmış | Tamamlandı — 2026-10-06; medya/ses yürütmesi sonraki aşamalarda |
| 7 | Medya kütüphanesi | İçe aktarma, arama, metadata, teknik analiz, waveform ve cue verisi çalışır | Tamamlandı — 2026-10-06; gerçek oynatma/cihazlar aşama 8 |
| 8 | Ses motoru ve cihazlar | Çoklu deck, gerçek oynatma, ana/CUE/monitor çıkışları ve mikrofon çalışır | Tamamlandı — 2026-10-06; fiziksel dinleme/uzun süre kabulü aşama 14 |
| 9 | DJ ve geçişler | Mixer, efekt, crossfade, pad/cart ve geçiş düzenleme gerçek PCM üzerinde çalışır | Tamamlandı — 2026-10-06; otomatik playlist/program yürütmesi aşama 10 |
| 10 | Playlist ve otomasyon | Playlist, program, kuyruk, takvim, rotasyon ve zamanlanmış motor yürütmesi çalışır | Tamamlandı — 2026-10-06; stream/on-air ve dayanıklı kesinti kurtarma aşama 12/13 |
| 11 | Reklam yönetimi | Kampanya/spot/kuşak ve kanıt sınıfları ayrılmış yayın raporları çalışır | Tamamlandı — 2026-10-06; yerel kaynak tüketimi, stream/uzak teslim aşama 12 |
| 12 | Yayın, kayıt ve acil akış | Stream/kayıt/metadata ve doğrulanmış acil geçiş yürür | Tamamlandı — 2026-10-06; MAIN/runtime kanıtı, dayanıklı kurtarma ve fiziksel donanım aşama 13/14 |
| 13 | Yönetim ve güvenilirlik | Yetki, yapılandırma, izleme, yedek/kurtarma ve kesinti sonrası toparlanma çalışır | Tamamlandı — 2026-10-06; fiziksel donanım/uzun süre/paket kabulü aşama 14 |
| 14 | Nihai kabul ve paketleme | Temiz kurulum, gerçek donanım ve uzun süreli yayın kabulü; çalıştırma/kurulum belgeleri | Yazılım/paket kabulü tamamlandı — 2026-10-06; fiziksel donanım ve saha süresi kabulü bekliyor |

Aşama numaraları iş paketleridir; bazı altyapı işleri paralel geliştirilebilir. Veri motorunun gerçek imzasıyla çelişen tasarım davranışı doğrudan kopyalanmaz, uygun C iş akışı ve gerekirse veri servisi genişletmesiyle çözülür.

## Aşama 1 çıktıları

- `docs/01-islev-haritasi.md`: kapsam, sorumluluklar, veri/komut sözleşmesi ve eksikler.
- `docs/yayin-dj-islevleri.md`: Yayın/DJ ve ortak alan eşlemeleri.
- `docs/playlist-otomasyon-islevleri.md`: Playlist/Otomasyon eşlemeleri.
- `docs/acil-reklam-teknik-islevleri.md`: Acil/Reklam/Teknik eşlemeleri.
- `docs/islev-envanteri.json`: kontrol referansları, workflow imzaları ve kaynak SHA-256 özetleri.
- `docs/sdbase-openapi.json`: mevcut backend'in kaynak kodundan üretilen sözleşme.
- `tools/islev_envanteri.py`: yalnız envanter/belge üretme ve doğrulama aracı.

Uygulama derleme veya yayın testleri bu aşamanın doğrulaması olarak gösterilmez. Aşama 1, üç eşleme belgesi ve 194 işlev grubu tamamlandıktan; kaynak/API envanteri, belge yolları ve kaynak atıfları doğrulandıktan sonra 2026-10-06 tarihinde tamamlandı olarak kaydedildi.

## Aşama 2 çıktıları

- `src/` ve `include/radios/`: native C modül ve başlık dizinleri.
- `database/SDBASE/`: 146 dosyalık tam veri motoru kaynak kopyası.
- `design/reference/tasarrım/`: 72 dosyalık yalnız görsel tasarım kopyası.
- `assets/`, `config/`, `data/`, `build/`, `cmake/`, `packaging/`, `tests/`: kaynak, yapılandırma, çalışma verisi ve geliştirme düzeni.
- `config/radios.example.ini`, `.gitignore` ve dizin açıklamaları.
- `docs/source-import-manifest.json`: ilk kopyanın köken/SHA-256 kaydı.
- `docs/02-proje-duzeni.md`: düzen, veri yolu sözleşmesi ve doğrulama sonuçları.
- İşlev haritası ve envanter aracında proje içi kaynak yollarına geçiş.

Aşama 2, 218 kaynak ve 2 font/lisans kopyası, 9 göreli yapılandırma yolu, 647 kaynak atfı, bağımsız proje kopyası ve kaynak/çalışma verisi ignore kuralları doğrulandıktan sonra 2026-10-06 tarihinde tamamlandı.

## Aşama 3 çıktıları

C11/CMake `radios` hedefi, gerçek GTK penceresi, INI/kök okuyucu, sınırlı iş/durum kuyrukları, iptal/join yaşam döngüsü ve kaynak denetimi uygulandı. 12 config ve 8 kuyruk test grubu, farklı dizinden gerçek config okuma ve yerel pencere kabulü geçti; CTest 4/4 başarılı. Ayrıntı [C altyapısı](03-c-uygulama-altyapisi.md) belgesindedir.

Kullanıcının görsel düzeltmesiyle aşama 4 de başlatıldı: ilk genel iskelet kaldırıldı, tasarımın yedi ana görünümü C/GTK bileşenlerine ve kaynak CSS stil akışına taşındı. [Native tasarım aktarımı](04-native-tasarim.md) mevcut kapsamı ve kalan ayrıntıları açıklar. Veri servisi ve gerçek ses motoru henüz bağlı değildir.

## Aşama 4 çıktıları

Yedi native profil ve ortak başlık; kaynak 30 CSS/1.552 kural aktarımı; program/playlist/cue/geçiş/sampler/acil/reklam/teknik düzenleyicileri ve typed alan doğrulama; kullanıcı girdisinden oturum taslakları; gerçek salt okunur XML önizleme; yerel görünüm tercihi kaydı kuruldu. Kaydedilemeyen DB işlemleri girişleri korur; gerçek ses/yayın veya hazır DB kaydı üretilmiş gibi gösterilmez. CTest 6/6, 10 UI ve 9 tercih test grubu, hedefli GTK kabulü, UI ASan/UBSan ve tercih Valgrind denetimleri geçti. GUI kapsamı 2026-10-06'da tamamlandı; veri servisi için sıradaki paket aşama 5'tir.

## Aşama 5 çıktıları

Kalıcı `data/database/RadioS.sdb`, şema 4, gerçek ilk yönetici/istasyon/offline uç ve ayrı kullanıcı/uç anahtarları kuruldu. Özel kimlik dosyası, kesilen kurulumdan aynı kimlikle devam, tekrar kurulumun mevcut dosyaları koruması, yerel servis yaşam döngüsü ve kimlik/bütünlük kabul komutları uygulandı. Gerçek HTTP yazısının yeniden başlatma sonrası kalıcılığı ve yabancı port/süreç korunması test edildi; gerçek kurulum servisi ve ilk yedeği doğrulandı. CTest 8/8; 14 servis ve 4 kurulum kurtarma testi başarılı; mevcut motor 19/19 ve sorgu/bütünlük 33/33 kabulü geçti. Ayrıntı [veritabanı kurulumu](05-veritabani-kurulumu.md) belgesindedir. Sıradaki paket aşama 6: C veri bağlantısıdır.

## Aşama 6 çıktıları

C/libcurl/JSON-GLib istemcisi, arka plan istekleri ve native sonuç teslimi; istasyon bağlamı; gerçek playlist/parça metadata CRUD ve sıra; kalıcı program/event, otomasyon, sampler, acil hazırlık, teknik profil ve reklam kayıtları bağlandı. Yetki/UUID/revision-CAS/typed alan doğrulaması ve değişmez işlem alındıları eklendi. Başarısız yazıda alan koruma, kullanıcı seçimiyle yeniden yükleme, aynı belirsiz işlemin aynı UUID ile tekrarı ve güvenli pencere kapanışı uygulandı. Native kullanıcı oluşturma özel dosyada anahtar teslim eder. Ayrıntı ve ekranların sınırları [C veri bağlantısı](06-c-veri-baglantisi.md) belgesindedir.

Strict C11 build ve 12 CTest hedefi başarılı; gerçek backend API kabulü 11/11, playlist HTTP kabulü 8/8, veri istemcisi 9/9; motor 19/19 ve sorgu/bütünlük 33/33 geçti. Test fixture kayıtları gerçek kullanıcı DB’sine yazılmadı. Aşama 6 sonundaki envanter 178 workflow/228 yol/233 HTTP işlemi içeriyordu. Native veri kabulü 6/6 ve ASan/UBSan 6/6 başarılı; GTK global verileri için GUI leak detection kapalı, core istemcisi leak detection açıkken temizdir. Sıradaki paket aşama 7: medya kütüphanesidir.

## Aşama 7 çıktıları

Native medya seçici, C/FFmpeg tam dosya çözümlemesi, proje içi özel import kopyası, SHA doğrulanmış yönetilen objeler ve tek transaction ile gerçek katalog/metadata/analiz/işlem alındısı kuruldu. Türkçe alan/tür araması, byte sınırına göre sayfalama, gerçek waveform ve gerçek medya UUID/SHA/süresine bağlı cue taslağı kaydı çalışır. Metadata ve işaretler CAS/revision ile korunur; sanatçı düzeltmesi genel katalog katkısıyla aynı transaction içinde tutarlıdır. Yanıt kaybında aynı gövde/işlem anahtarı kullanılır. Import teknik doğrulama için `edit` + `approve` ister; içerik sürümü draft ve istasyon yayın uygunluğu kapalı kalır.

Mevcut C/GTK bağımlılıklarına FFmpeg 5.1 minimum API ailesinin libavformat/libavcodec/libavutil/libswresample geliştirme paketleri eklendi; 5.1 başlık uyumluluğu incelendi, çalışma kabulü daha yeni kurulu kütüphanelerde yapıldı. WAV/PCM, MP3, FLAC, Ogg/Vorbis, M4A/AAC ve ham AAC yerel format kabul matrisidir. Dosya/PCM/waveform sınırları, saptanabilir bozuk/değişen kaynak, güvenli yol, iptal ve kapanış uygulanmıştır. Kaynaklar ve kullanım sözleşmesi [medya kütüphanesi](07-medya-kutuphanesi.md) belgesindedir.

Kalıcı DB alındıları aynı işlemin ikinci katalog kaydı üretmesini engeller. C tarafında belirsiz import isteği ve anahtarı ana pencere açıkken korunur; tamamen yeniden açılışta bu bekleyen kuyruğu otomatik kurtarma ve sahipsiz kopya temizliği aşama 13'tedir. Teknik medya doğrulaması onaylı marker set, gerçek oynatma veya on-air kanıtı değildir. Sıradaki paket aşama 8: ses motoru ve cihazlardır.

Son kabul kaydı: strict C11 ve CTest 15/15 başarılı (34,31 saniye); C analiz 10/10 ve ASan/UBSan 10/10; native medya 5/5 ve son kaynaklarla ASan/UBSan 5/5; backend medya 14/14, mevcut typed API 11/11, playlist HTTP 8/8, C veri istemcisi 10/10 ve leak detection açık sanitizer kabulü 10/10; motor 19/19 ve sorgu/bütünlük 33/33 başarılı. Güncel envanter 183 workflow/233 yol/238 HTTP işlemi ve 168 backend kaynak dosyasıyla üretildi, kontrolü geçti. Gerçek servis ve 0.7.0 C penceresi son kaynaklarla açıldı; mevcut depo/istasyon/uç UUID'leri, sequence 2 ve bütünlük `ok` korundu. Test fixture'ları gerçek kullanıcı DB'sine yazılmadı.

## Aşama 8 çıktıları

C/FFmpeg artımlı çözücü ve libpulse ses yolu, 32 normal oynatıcı ve ayrılmış CUE ön dinleme kuruldu. Kütüphanenin SHA/istasyon/version/obje doğrulamalı kaydı gerçek oynatıcıya yüklenir; oynat/duraklat/durdur/EOF'dan yeniden başlat, sample-exact sarma, gain ve MAIN yönlendirmesi çalışır. Ayrı MAIN/CUE/MON akışları ve mikrofon yakalama/kapı/kazanç, tüketilen PCM'den ölçüm ve gerçek ses tüketimine bağlı cursor uygulandı. Cihaz tercihleri managed `audio_preferences` belgesinde CAS/alındı/yetkiyle saklanır; yeniden açılışta canlı kapılar ve oynatma kapalıdır. İlk profil belgeleri ve ses tercihi yükleme için normal uygulamanın sınırlı arka plan kuyruğu 64 iş/128 durum kaydına çıkarıldı.

Ön dinleme sahipliği ve generation, kaynak/cihaz hatasında konum/kapı koruması ve deterministik cancel/join kapanışı kabul edildi. Eksik adlandırılmış cihazda ses servisinin başka cihaza geçmesi gerçek cihaz adı denetimiyle engellendi. Kart CUE'su kendi yüklenmiş doğrulanmış kaydını kullanır; geciken medya sorgusu yeni ön dinlemeyi veya CUE kapatmayı geri alamaz. Tam ses sunucusu kesintisinden otomatik bağlantı kurtarma sonraki güvenilirlik kapsamındadır; mevcut durumda uygulamayı yeniden açmak gerekir.

Son strict build ve birleşik CTest **19/19**, 134,77 saniye başarılı. Yeni C çözücü **11/11**, gerçek PCM motoru **6/6** ve gerçek Pulse **1/1**, native ses **6/6** başarılıdır. Core çözücü/motor/Pulse ASan/UBSan ve leak detection açıkken temiz; native ses ASan/UBSan **6/6** (41,66 saniye test uygulaması), GTK süreç global verileri için leak detection kapalıyken temizdir. Backend typed API **13/13**, önceki medya/veri hedefleri ve kaynak/API envanteri başarılıdır. Ses kabulü yalnız ayrı sanal çıkış/giriş ve özel fixture DB'de yapıldı; gerçek çalışma DB'sinin kimlikleri, sequence 2 ve bütünlüğü korundu. Güncel servis ve **0.8.0** C uygulaması açıldı. Ayrıntı [ses motoru ve cihazlar](08-ses-motoru.md) belgesindedir. Sıradaki paket aşama 9: DJ, efekt ve geçiş yürütmesidir.

## Aşama 9 çıktıları

32 mixer kanalı, kaynak seçimleri, EQ/pan/gain, mute/solo/PFL ve eşit güç A/B crossfade gerçek C ses grafiğine bağlandı. Beş ortak efekt slotunun 14 DSP işlemi, kanal/MASTER send-return ve MASTER'ın kendi efekti gerçek PCM üretir. Üst alan ve DJ MASTER aynı gain/EQ değerlerini kullanır. Ducking gerçek kapısı açık mikrofon seviyesini algılar; sabit göstergeye dayanmaz. Native BPM kontrolü efekt türünün yanında yer alır; kaynak 30 CSS/1.552 kural aktarımı korunur.

Kanal/MASTER padleri doğrulanmış UUID/SHA medya bağlarıyla ayrı sampler seslerini yönetir. Kanal silinince kalan canlı kimlikler sıkıştırılmaz. A/B geçiş editörü iki gerçek dosyadan C worker üzerinde sınırlı miks hazırlayıp yalnız CUE'da ön dinletir. Tam örnek sayısı, fade/overlap/ducking, Stop/EOF/kapanış ve geç gelen iş sahipliği denetlenir. `dj_mixer`, sampler ve geçiş bağları gerçek DB'de CAS/alındı/yetkiyle kalıcıdır; açılışta oynatma/mikrofon/PFL kapalıdır. Uygulanmamış cihaz tercihi mixer kaydında korunur.

Strict C11 ve birleşik CTest **22/22** başarılı (173,78 saniye). DSP **13/13**, motor **9/9** ve gerçek Pulse ek grubu, geçiş **3/3**, typed backend **18/18**, native DJ **5/5** geçti. Core DSP/motor/geçiş ASan/UBSan ve leak detection açıkken temizdir. Son controller düzeltmeleri normal native ses **6/6** ve ASan/UBSan **6/6** ile ayrıca doğrulandı. Kesirli dosya süresi/gerçek EOF ve bekleyen cihaz tercihini kapsayan güçlendirilmiş native DJ normal **5/5** (32,73 saniye), ASan/UBSan **5/5** (54,77 saniye) geçti; GTK global verileri için leak detection kapalıdır.

Güncel veri servisi ve **0.9.0** C penceresi mevcut çalışma DB'siyle açıldı. Depo/istasyon/uç kimlikleri ve sequence **2** korundu; bütünlük **ok**, güncel DJ API yanıtı **HTTP 200**. Üretim DB'sine test kaydı eklenmedi. Ayrıntı [DJ ve geçişler](09-dj-ve-gecisler.md) belgesindedir. Sıradaki paket aşama 10: playlist/program/kuyruk/takvim yürütmesi ve otomatik geçişlerdir.

## Aşama 10 çıktıları

Doğrulanmış playlist metadata bağları, native kütüphane seçimi ve playlist→kalıcı program aktarımı; gerçek C sıra yürütücüsü, EOF/crossfade/cue/manual geçiş, pause/resume/next/previous ve UTC takvim bağlandı. Olayların sequential/kes/bitir politikaları ve zaman pencereleri uygulanır. C planlayıcı döngü/ağırlık/tamamlama, sıra/RNG/az planlanmış parça, tekrar/gün/saat, jingle ve yedek seçimlerini yürütür. Sonraki partiler önceden hazırlanıp çalışan sıraya eklenir; 500 giriş sınırında ve tek oynatıcılı yeniden yüklemede bekleme açıkça gösterilir.

Gerçek MAIN tüketim occurrence'ları kısa jingle ve PREVIOUS yeniden çalmasını kaybetmeden toplanır. En yeni 2.000 occurrence ve typed DB geçmişi; kalıcı kaynak cursor/RNG/tamamlama/jingle checkpoint'i; read-only preview, gerçek geçmiş tablosu ve export bağlandı. Checkpoint yalnız gerçek tüketimden sonra ilerler. Saat/buffer denetimi, manuel deck korunması, geç callback sahipliği, Stop/kapanış, SHA/istasyon/path ve CAS/input koruma kabul edildi. Son henüz tamamlanmamış yazının çökme/kapanışta dayanıklı kurtarılması aşama 13'tedir; stream/on-air kanıtı aşama 12 kapsamındadır.

Strict C11 ve birleşik CTest **26/26**, **226,46 saniye** başarılı. Planlayıcı **17/17**, runner **15 deterministik + 1 gerçek Pulse**, backend hazırlık/geçmiş **13/13**, typed API **18/18**, native kabul **5/5** başarılıdır. Core ASan/UBSan/leak açıkken ve native ASan/UBSan **5/5** (**64,47 saniye**, GTK leak kapalı) temizdir. Tek müziklik partilerde A→jingle→eklenen B PCM'sinde normal en uzun sessizlik **1 kare**, sanitizer **13 ms** ölçüldü. Son kaynakların otomasyon sanitizer kabulü ayrıca geçti. Kaynak/API envanteri **184 workflow / 234 yol / 239 HTTP işlemi / 169 backend dosyası** ile doğrulandı; kaynak 30 CSS/1.552 native kural aktarımı korundu. Ayrıntı [playlist ve otomasyon](10-playlist-ve-otomasyon.md) belgesindedir. Sıradaki paket aşama 11: reklam yönetimidir.

Güncel servis ve **0.10.0** C penceresi açıldı; mevcut depo/istasyon/uç UUID'leri, şema 4, sequence **2** ve bütünlük **ok** korundu. Gerçek prepare API **HTTP 200**, contract 1 ve local_draft döndürdü. Üretim DB'sinde fixture yok; tek normal pencere/MAIN akışı açık ve test sanal modülleri temizdir.

## Aşama 11 çıktıları

Gerçek reklam medya/sürüm bağlama ve SHA kontrolü, ayrı kampanya/spot onayı, açık kullanım izni ve gerçek trafik yükümlülükleri; güvenli satış metadata düzenleme, durum/versiyon/bildirim okuma akışları bağlandı. Kuşak tarih/çıkış/bölge köprüsü, ölçülen açılış/kapanış/anons, kapasite+aşım ve üç boş kuşak politikası gerçek önizleme/rezervasyon/iptal/yeniden yerleştirme kullanır. CAS, tam gövde alındısı, yetki iptali ve eski placement geçmişi korunur.

Değişmez manifest gerçek C yürütmesinden önce saklanır. Zamanlı kes/bitir/sınırlı bekle, gerçek 240 kare fade, mevcut programın erken bitişi, tampon emekliliği ve manuel iptal korunur. Bekleyen API/controller kuşağında Stop eski MAIN'i sürdürür. Gerçek kaynak karelerine bağlı tamamlama/kesilme/hata; sıfır PCM'li gerçek decoder hazırlık hatası; kanıt sınıfları ayrılmış filtreli değişmez rapor ve native CSV kaydı çalışır. Yerel tamamlanma formal reklam kabul/teslim sayacını ilerletmez; terminali alınmayan başlangıç belirsizdir. Son tamamlanmamış yazının çökme/kapanışta dayanıklı kurtarılması aşama 13 kapsamındadır.

Strict C11 ve birleşik CTest **28/28**, **317,42 saniye** başarılı. Backend **22/22**, native reklam **5/5** (**79,77 saniye**), planlayıcı **18/18**, yürütücü **21 deterministik + 1 gerçek Pulse** geçti. Core ASan/UBSan/leak açıkken **22/22**; native ASan/UBSan **5/5**, **122,27 saniye** test uygulaması süresiyle temizdir (GTK leak kapalı). Güncel envanter **201 workflow / 251 yol / 256 HTTP işlemi / 170 backend kaynak dosyası**; kaynak CSS **30 dosya / 1.552 native kural** korunur.

Güncel servis ve **0.11.0** C penceresi mevcut veritabanıyla açıldı; gerçek depo/istasyon/uç kimlikleri, şema 4, sequence **2**, bütünlük **ok** ve üretim koleksiyonları korundu. Gerçek reklam overview ve prepare **HTTP 200** verdi. Tek normal pencere/MAIN akışı açık; test cihazları temiz. Ayrıntı [reklam yönetimi](11-reklam-yonetimi.md) belgesindedir. Sıradaki paket aşama 12: yayın, kayıt ve acil akış yürütmesidir.

## Aşama 12 çıktıları

Son MAIN miksinden gerçek WAV kayıt, native FFmpeg MP3/Icecast PUT ve ayrı HTTP sonuçlu metadata; kaydedilmiş UUID/SHA bağlı acil liste, doğrulama boyunca eski PCM'nin sürmesi, gerçek devralma ve normal sıraya tüketilmiş konumdan dönüş bağlandı. Crossfade'de gelen öğe, otomasyonun hazır devamı ve manuel kaynak sahipliği korunur. Doğal tamamlanma bütün öğelerin gerçek COMPLETE makbuzunu ister; Next/Stop kesilme olarak kalır. Profil/manifest/revision/yetki ve append-only runtime olayları DB'de saklanır. Şifre oturumda kalır, açılışta çıkışlar başlamaz.

Yerel PCM, kaydedilen dosya, encoder byte'ları ve server HTTP kabulü ayrı kanıtlardır; uzak/anten/akustik teslim doğrulanmış sayılmaz. Gerçek Icecast 2.4.4 loopback kabulünde MP3 dinleyici çözümlemesi ve metadata güncellemesi geçti. Genel C belge istemcisinde eski read/save-ACK sırası ve bekleyen yenileme düzeltildi; kapalı yayın paneli gereksiz profil okumaz.

Strict C11 ve birleşik CTest **31/31**, **204,50 saniye** başarılı. Yayın core **8/8**, typed backend **15/15**, native yayın/acil **4/4** başarılıdır. Native ASan/UBSan **4/4**, **79,78 saniye** temizdir (GTK leak kapalı); WAV/stream core **8/8** leak denetimi açıkken temizdir. C yürütücü/planlayıcı/output sanitizer hedefleri ayrıca **3/3** geçti. Yeni genel belge read-order testi normal ve ASan/UBSan altında geçti; korumaları kaldıran iki özel negatif kontrol beklenen gerçek hatayı gösterdi. CUE ve otomasyon regresyonları son kaynaklarla başarılıdır. Güncel API envanteri **206 workflow / 256 yol / 261 HTTP işlemi / 171 backend kaynak dosyası**, kaynak CSS **30 dosya / 1.552 native kural** korunur.

Güncel **0.12.0** C uygulaması ve veri servisi mevcut çalışma veritabanıyla açık. Depo/istasyon/uç kimlikleri, şema 4, sequence **2**, bütünlük **ok** ve üretim koleksiyonları korundu. Gerçek yayın/reklam overview ve boş program prepare API'leri **HTTP 200** verdi. Üretim DB'sine test kaydı yazılmadı; yalnız normal sessiz MAIN çıkışı açık. Sıradaki 13. aşama: yönetim, yetki, izleme, yedek/kurtarma ve dayanıklı kesinti kurtarmadır.

Ayrıntı [yayın, kayıt ve acil akış](12-yayin-kayit-acil.md) belgesindedir.

## Aşama 13 çıktıları

Native kullanıcı/izin düzenleme ve anahtar iptali; son kullanılabilir yönetici/kendi oturumu/global kullanıcı korumaları; canlı izin/anahtar doğrulaması; gerçek servis/disk/bütünlük/cihaz/audit izlemesi bağlandı. Yerel yedek yardımcıları her işlemde güncel yönetim yetkisini doğrular. Kesintili yayın/reklam manifest ve ölçümleri değişmeden ayrı kurtarma alındısıyla kapatılır; restored eski çalışmalara yeni olay eklenemez.

C dayanıklı işlem kuyruğu exact idempotency gövdesini fsync/atomik yazıyla saklar. Store/station/user/endpoint, private path/owner/link, secret ve SHA-256 zarf denetimi uygulanır. Başka kullanıcı ortam anahtarı ayrı kuyruğa bağlanır. Yetki/CAS nedeniyle reddedilen önceki yazı aynı kaynağın ardılını da tutar. İstemci UUID'si/revision 0 ilk oluşturmayı ve ilk çalma geçmişinin bekleyen son checkpoint'ini bağlar. Normal kapanış yayın/reklam terminalini ve geçmişi korur; kurtarma kendiliğinden ses başlatmaz.

Tutarlı DB/gerçek medya/kurulum yedeği, hash doğrulama, geri yükleme planı ve ayrı hazırlık kopyası; offline bileşen değişimi, crash journal/rollback ve kullanıcı outbox karantinası; sahipsiz medya audit/karantinası uygulandı. Açık servis/GUI/DB/outbox ve yabancı süreç/port korunur. Pulse aynı sunucuya 1/2/5 saniye aralıklarıyla döner; doğrulanmış imleç korunur, oyuncular duraklar, mikrofon kapanır ve gecikmiş ses başlatma yanıtları engellenir.

Strict C11 ve birleşik CTest **36/36**, **242,54 saniye** başarılı. Kuyruk **10/10**, istemci **15/15**, backend yönetim **26/26** ve desktop regresyonu **108/108**, güvenilirlik **18/18**, native yönetim **6/6**, native yayın/kapanış kurtarması **5/5** geçti. Son UI açıklaması sonrası normal yönetim altı grubu ayrıca **9,87 saniyede** başarılıdır. Core leak açık ASan/UBSan **2/2 hedef** (**5,58 saniye**); native yönetim **6/6** ve yayın **5/5** (**96,43 saniye**, GTK leak kapalı) temizdir. Gerçek özel ses sunucusunun üç kez kesilmesi normal ve leak açık ASan/UBSan **1/1** ile kabul edildi; host daemon korunur. Envanter **211 workflow / 261 yol / 266 HTTP işlemi / 172 toplam kaynak dosyası (72 tasarım + 100 Python modülü)**, kaynak CSS **30/1.552** doğrulandı.

Güncel **0.13.0** C uygulaması ve servis açıldı; üretim UUID'leri, şema 4, sequence **2**, bütünlük **ok** ve koleksiyonlar korundu. Yönetim/reklam/yayın overview **HTTP 200**; fixture ve mikrofon capture yoktur. Gerçek kurulum yedeği `data/backups/radios-20261006T082436Z-f90a3ffe` tekrar doğrulandı. Ayrıntı [yönetim ve güvenilirlik](13-yonetim-ve-guvenilirlik.md) belgesindedir. Sıradaki paket **14: nihai kabul ve paketleme**.

## Aşama 14 çıktıları

0.14.0 optimize Release uygulaması ve bağımsız kaynak arşivi; özel çalışma verilerini dışlayan deterministik paketleme, dosya SHA-256/boyut/izin manifesti ve gerçek ELF bağımlılık raporu üretildi. Yeni kurulumun kimlik/DB/servis hazırlığı normal başlatıcıya bağlandı. Geçersiz argüman/ayar, yabancı port ve kesilmiş restore mevcut veriyi koruyarak reddedilir. Kurulu SDK üzerinden yalnız native C arayüzü çalışır; veri servisi ana sistemde aynı kuruluma ait süreç olarak tanınır.

Birleşik strict C11/GTK CTest **39/39**, **256,50 saniye** başarılı; paket testleri **13/13**, gerçek başlatıcı testleri **8/8** geçti. İki arşiv temiz ve Türkçe/boşluklu köklere açılarak yedi görünüm, gerçek typed HTTP/CAS/kalıcılık, bağımsız optimize kaynak derleme ve ana sistemde ilk/ikinci açılışın aynı kimlik/servisi koruması kabul edildi. Üretim verisi fixture olarak kullanılmaz.

Gerçek **300,011 saniyelik** özel Pulse koşusu **160 geçiş**, **40 canlı kuyruk ekleme**, **160 EOF + 1 Stop**, WAV sonlandırma ve bağımsız MP3/HTTP 200 ile geçti. MAIN/WAV/MP3 yollarında 500 ms çözünürlükte yayın içi sessiz boşluk ve çıkış kuyruğu taşması görülmedi; ısınma sonrası RSS yaklaşık 1 MB arttı. Kısa ASan/UBSan koşusunda leak denetimi açıktır. Bu beş dakikalık sanal cihaz kanıtı fiziksel veya 24 saatlik işletme kabulü olarak sunulmaz.

Gerçek kurulum 0.14.0 ile açıldı; mevcut şema 4/sequence 2 ve kurulum kimlikleri korunur. Salt okunur donanım denetimi ile altı operatör kontrolü hazırlandı; hoparlör, ayrı CUE, mikrofon, harici kart, gerçek uzak dinleyici ve işletme süresine uygun saha testi henüz doğrulanmadı. [Paketler, komutlar, kabul kanıtları ve fiziksel kontrol listesi](14-nihai-kabul-ve-paketleme.md).

## 0.15–0.17 ek geliştirmeler

0.15 ile gerçek uygulama/mikrofon routing ve WAV kaydı; 0.16 ile native Spotify/Apple Music/YouTube resmî hesap/playlist akışları eklendi. 0.17 kapsamlı kaynak/kayıt/API/GUI parite denetimini ve bu denetimde bulunan yazılım açıklarının bağlantılarını içerir. Güncel kabul kapsamı ve kalan işler [kapanış denetimindedir](proje-kapanis-denetimi.md).
