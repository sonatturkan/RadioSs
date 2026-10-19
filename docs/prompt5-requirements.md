# Prompt 5 kaynak gereksinimleri ve kabul ölçütleri

İnceleyen: `prompt5/requirements`. İnceleme günü: 2026-10-09 UTC. Bu belge yalnız `../prompt/5/` kaynaklarını kapsar. Başka prompt numaralarının bağımsız eşzamanlı oturumları bu belgenin gereksinim kaynağı değildir. Başlangıç aşamasında bu belge kaynak analizi ve kabul ölçütü olarak yazılmıştır. Sonraki tarihli checkpoint’ler gerçek uygulama, test sonuçları ve bekleyen nihai kabulü ayrı kaydeder.

Son durum: **Prompt5'in bütün 10 kaynak/işlevsel gereksinimi, stable test/görsel kabulü ve varsayılan launcher kurulumu tamamlandı.** Son kurulum/kaynak snapshot'ı ve sınırlar bu belgenin son tarihli bölümünde kayıtlıdır.

## Tam kaynak envanteri ve inceleme yöntemi

Recursive dosya envanterinde bir PDF ve iki görsel bulundu; başka PDF veya alt klasör PDF'si yoktur.

| Kaynak | Kapsam | İnceleme |
|---|---|---|
| `../prompt/5/5.pdf` | 1 sayfa, A4 (595.2756 × 841.8898 pt), başlık `5` | pypdf ile metadata, sayfanın bütün metni, içerik akışı ve bütün görüntü nesneleri okundu |
| `../prompt/5/figures/sema_1.png` | 407 × 146 px | `view_image` ile tam görsel incelendi; PDF'nin ilgili RGB görüntüsüyle piksel eşitliği doğrulandı |
| `../prompt/5/figures/sema_2.png` | 1742 × 46 px | `view_image` ile tam görsel incelendi; PDF'nin ilgili RGB görüntüsüyle piksel eşitliği doğrulandı |

PDF'de tam olarak iki `/Image` nesnesi bulunmaktadır. Şema1 için `/FormXob.8ec63f28efbb9bcd973589adf271f659`, Şema2 için `/FormXob.5ee6417145f0b8b353cc83abeb008459` kullanılır. PNG piksel satırları standart Python `struct`/`zlib` ile çözümlenmiş, PDF `get_data()` RGB akışıyla byte düzeyinde karşılaştırılmıştır; iki karşılaştırma da `True` sonucunu vermiştir. Böylece figures dosyalarının PDF'deki şemaların aynısı olduğu doğrulanmıştır. Yerel PDF render araçlarının bulunmaması, şema incelemesinde kaynak eksikliği yaratmamıştır.

RGB piksel SHA-256 değerleri:

- Şema1: `87c18e3c8a4e2512cb085a72d13ce131bd4590eb62d7bf083ab9bd8983bb07cd`.
- Şema2: `fe639c8f5cb4ea6306353deefa5bb2c4d29855ff0b4f13ed75ee9fed3ec9cbe1`.

## PDF'nin bütün talimatları

Sayfa 1'de Şema1'in ardından: “Şema 1’deki kısmı kaldır.”

Şema2'nin ardından: “Oradaki bilgileri bu bara (Şema 2) ekle.”

Son talimat: “O kaldırdığın yere butonlar ekle. Burada mevcut butonlarla diğer şarkıya ya da bir sonraki playliste geçebileyim.”

PDF başka sayfa, teknik parametre, buton ölçüsü, renk değişikliği veya ek işlev tarif etmez. Görseller mevcut arayüzün hangi bölgelerinin değişeceğini gösterir; tamamlanmış yeni düzen çizimi değildir.

## Şemaların ayrıntılı okunması

### Şema1: kaldırılacak üst başlık bölümü

Siyah zeminli, ince kenar çizgili bölümde üstte `DEVAM DAVRANIŞI` başlığı, hemen altında mevcut davranış için `—` yer tutucusu vardır. Alt satırda soldan sağa `Veritabanı  Bağlı`, `Yedek  Bilinmiyor`, `Uyarı  —` ve sağda çerçeveli `Ayrıntı` butonu görünür. Kaldırılacak bölüm bu grubun tamamıdır.

Görselde saat/program alanı veya `GERÇEK SIRADAKİ` alanı bulunmaz. Bu alanları kaldırmak ya da taşımak Prompt5'in bir gereksinimi değildir.

### Şema2: bilgilerin ekleneceği alt bar

1742 × 46 px görsel siyaha yakın zeminli, üstünde ince çizgi bulunan yatay durum barıdır. Solda `Akış ayarları veritabanından yüklendi. Kayıt kapalı.` uygulama bildirimi görünür. Sağ tarafta sırayla `Verileri yenile` butonu, `İstasyon: RadioS` ve `Son güncelleme: 00:34:49 UTC` metni bulunur.

Taşınacak Şema1 bilgileri bu alt bara eklenmelidir. Görseldeki istasyon adı, saat ve bildirim uygulamanın o andaki örnek değerleridir; sabitlenmesi gereken yeni literal veriler değildir. Mevcut yenileme, istasyon ve UTC güncelleme öğeleri işlevini sürdürmelidir.

## Gereksinim matrisi

| ID | Kaynak | Zorunlu sonuç | Kabul koşulu |
|---|---|---|---|
| P5-01 | PDF s.1, Şema1 | Eski devam davranışı grubu üst başlık bölgesinden kaldırılır | Üstte eski davranış/DB/yedek/uyarı/Ayrıntı bilgi grubu görünmez |
| P5-02 | PDF s.1, Şema1 → Şema2 | Devam davranışı başlığı/anlamı ve mevcut değeri alt barda sunulur | `DEVAM DAVRANIŞI` anlamı ile gerçek değer veya `—` alt durum barında görünür |
| P5-03 | PDF s.1, Şema1 → Şema2 | Veritabanı bağlantı bilgisi alt bara taşınır | Bilinmeyen durumda `—`, doğrulanmış bağlantıda `Bağlı` uygun alt bar öğesinde görünür |
| P5-04 | PDF s.1, Şema1 → Şema2 | Yedek bilgisi ve erişilebilir işlevi alt bara taşınır | `Yedek Bilinmiyor` ya da doğrulanmış gerçek durum görünür; mevcut yedek bağlantısı Acil görünümünü açar |
| P5-05 | PDF s.1, Şema1 → Şema2 | Uyarı bilgisi alt bara taşınır | `Uyarı —` ya da doğrulanmış gerçek uyarı alt barda görünür |
| P5-06 | PDF s.1, Şema1 → Şema2 | Ayrıntı erişimi korunur ve taşınan bilgilerden kullanılabilir | Alt bardaki `Ayrıntı` mevcut `diagnostic-details-dialog` penceresini açar |
| P5-07 | PDF s.1, son talimat | Kaldırılan üst bölümün yerinde şarkı geçiş butonu bulunur | Kullanılabilir eylem mevcut aktif oynatma akışında bir sonraki gerçek şarkı/içeriğe ilerler |
| P5-08 | PDF s.1, son talimat | Aynı yerde sonraki playlist geçiş butonu bulunur | Aktif playlist/blok içindeki kalan şarkıları atlayıp sıradaki gerçek playlistin ilk çalınabilir öğesine ilerler |
| P5-09 | PDF s.1, Şema2 | Şema2'nin mevcut bildirim/yenileme/istasyon/güncelleme işlevleri korunur | Eklenen öğeler mevcut öğeleri erişilemez kılmaz; DB yenileme ve UTC güncelleme çalışır |
| P5-10 | Kullanıcı temel talimatı | Değişiklik uygun testlerle ve kaynak karşılaştırmasıyla doğrulanır | Build, ilgili işlev testleri ve görsel düzen denetimi raporlanır; uygulanmamış ölçüt tamamlandı sayılmaz |

`Ayrıntı` ve yedek bağlantısı PDF'de ayrı cümlelerle isimlendirilmez, ancak kaldırılan görselde işlevli öğeler olarak açıkça yer alır. Bilgilerin bara taşınması sırasında mevcut erişimi korumak, görselin ve mevcut proje davranışının kaybolmasını önler.

## Mevcut kaynaklarla kesin eşleme

Satır numaraları inceleme anına aittir; diğer oturumların aynı projede yetkili değişiklikleri nedeniyle ileride kayabilir.

| Arayüz öğesi | Mevcut uygulama noktası | Mevcut davranış |
|---|---|---|
| Şema1 tüm bölümü | `src/ui/header.c`, `behavior_new()`, incelemede satır 309 | `header-behavior` CSS sınıfı taşıyan dikey bölüm |
| Devam davranışı metni | Aynı fonksiyon, `header-behavior-text` | `—` başlangıç yer tutucusu; taranan kaynaklarda dinamik güncelleme bulunmadı |
| Veritabanı durumu | `header-database-status` widget'ı | `Veritabanı —` başlangıcı; `window.c:database_context_refreshed()` gerçek DB bağlamında `Veritabanı Bağlı` yazar |
| Yedek bilgisi/bağlantısı | `header-backup-details` butonu | `Yedek Bilinmiyor`; `window.c:backup_profile()` Acil profilini seçer |
| Uyarı bilgisi | `header.c:behavior_new()` içindeki `Uyarı —` label'ı | Özel widget adı yok; taranan kaynaklarda dinamik güncelleme bulunmadı |
| Ayrıntı butonu | `header-diagnostic-details` | `header.c:details_open()` statik fonksiyonuyla `diagnostic-details-dialog` açar |
| Şema2 alt bar | `src/ui/window.c`, pencere kurulumu, incelemede satır 504 | `status-footer` yatay kutusu |
| Alt bar sol bildirimi | `RadioSWindow.status`, `application-status` sınıfı | `notify()` gerçek uygulama mesajını yazar; tek satır/ellipsize |
| Alt bar sağ öğeleri | `footer-station`, `footer-updated`, `Verileri yenile` | DB bağlamından istasyon adı, UTC son güncelleme; yenileme `radios_data_connect()` |
| Bölümün başlık düzenindeki yeri | `header.c` içindeki `BEHAVIOR` section enum'u ve header yerleşim kodu | Yeni buton grubu eski devam davranışı bölümünün alanını kullanmalıdır |
| Başlık tasarım referansı | `design/reference/tasarrım/index.html`, `header-layout.css` | `header-behavior`, `header-health-row`, `broadcast-health` stilleri |

Durumların başlangıçta bilinmemesi mevcut tasarım kuralıdır. Görselde `Bağlı` yazması, doğrulanmamış DB bağlantısının yeni uygulamada bağlıymış gibi gösterilmesine gerekçe değildir.

## Mevcut şarkı geçişi ve playlist boşluğu

`src/ui/broadcast.c:programme_transport()` mevcut `programme-next` butonunun action 4 değerini `RADIOS_PLAYOUT_NEXT` komutuna bağlar. Yerel oynatmada `radios_playout_ui_action()`, çevrimiçi yayın akışında `radios_broadcast_online_flow_action()` kullanılır. `programme_context(view)` bağımsız sıra `queue_slot` bağlamını korur. Yeni üst butonun bu sahipliğe ve mevcut çevrimiçi/yerel dallara uyması gerekir.

`src/ui/broadcast.c:player_transport()` ve `deck-N-next` kart butonları ayrıca kartın yerel/çevrimiçi girdilerini veya yönetilen runner'ını ilerletir. Kartın bir sonraki girdisi ile yayın sırasının bir sonraki playlisti aynı eylem değildir.

İncelenen `RadioSPlayoutAction` enum'u PAUSE/RESUME/STOP/NEXT/PREVIOUS içerir; genel bir NEXT_PLAYLIST eylemi yoktur. `playlist.c` içindeki `Sonraki` butonu liste sayfalaması içindir ve bu gereksinimi karşılamaz.

Mevcut proje playlistleri yayına `blocks` ve düz `events` biçiminde ekler. Her leaf `block_id` taşır; `RadioSProgrammeTree` sıra/blok ilişkisini sunar. Oynayan akışın `entry_id` veya çevrimiçi `playing` kimliği kullanılarak gerçek mevcut blok belirlenebilir. Sonraki playlist, mevcut bloğun bir sonraki parçası değildir. Hangi blok türlerinin playlist sayıldığı uygulamada mevcut modele göre açık kaydedilmelidir; PDF rastgele kütüphane playlisti seçmeyi veya katalog sayfalamasını tarif etmez.

Çalışan plan immutable hazırlanmış snapshot'tır; editör taslağı daha sonra değişmiş olabilir. Sonraki playlist hedefini yalnız mutable taslağın sayı indekslerinden seçmek yanlış şarkıya geçirebilir. Hedef kimliği oynayan planla doğrulanmalı; sonraki playlist yoksa yanlışlıkla ilk playlisti/şarkıyı başlatmamalıdır.

## Doğrulama senaryoları

1. Büyük pencere, uygulamanın minimum 1000 × 680 boyutu ve farklı yazı boyutlarında eski Şema1 grubunun kaldırılması; aynı yerde iki anlaşılır geçiş butonunun bulunması; alt bar bilgilerinin erişilebilirliği.
2. Veri bağlantısı yokken/kurulmadan önce alt barda DB bilinmeyen durumu; gerçek bağlantı sonrasında `Bağlı`, istasyon adı ve UTC son güncellemenin görünmesi; `Verileri yenile` işlevinin korunması.
3. Alt barda davranış, yedek ve uyarı yer tutucularının korunması; sahte yeni gözlem değerlerinin üretilmemesi.
4. `Ayrıntı` ile mevcut tanı penceresinin açılması/kapanması; yedek bağlantısıyla Acil görünümünün seçilmesi.
5. Yerel aktif yayın sırası, çevrimiçi/karma akış ve bağımsız sıra sahipliği durumlarında sonraki şarkı geçişinin doğru akışı etkilemesi.
6. Bir playlistte en az iki şarkı ve sonraki playlistte en az bir şarkıyla playlist geçişinin mevcut listedeki kalan şarkıları atlaması.
7. Son playlist, boş sonraki blok, sıra yok, plan hazırlığı, bitmiş/hata durumu, taslakta oynayan kimliğin artık bulunmaması gibi uçlarda anlaşılır pasif durum/gerçek hata; yanlış hedefe geçiş veya crash olmaması.
8. Kaynak grupların taşınması uygulamanın saat, gerçek sıradaki, yayın hedefi ve ses ayarlarının yerleşimini istemeden bozmadığının görsel kontrolü.

## İnceleme sonucu ve devam

Tamamlanan: bütün Prompt5 PDF dosyalarının ve tüm sayfalarının envanteri/metin incelemesi; iki şemanın görsel incelemesi; PDF/PNG piksel eşitliği; mevcut GTK kaynaklarıyla eşleme; test kabul ölçütleri. Bulgular root, validation ve coordination ajanlarına aktarıldı.

Bu paragraf **ilk kaynak analizi aşamasının** durumunu kaydeder: o aşamada requirements ajanı kaynak kodunu değiştirmemişti; dosya sahipliği uzlaşması, uygulama ve testler bekliyordu. Aşağıdaki tarihli güncelleme bu başlangıç durumundan sonra yapılan işi gösterir.

## 2026-10-09 02:04 UTC — backend uygulama ve doğrulama güncellemesi

Root tarafından kaynak analizi sonrası verilen ayrık uygulama görevi tamamlandı. Değiştirilen/yeni dosyalar:

- [header_navigation.h](../include/radios/header_navigation.h): başlık navigasyonu/sağlık grubu, broadcast facade, immutable playlist sorgu/eylem ve bounded runner jump API'leri.
- [engine.c](../src/audio/engine.c): `radios_audio_engine_runner_jump_to()` ve bağımsız oynatıcı için `radios_audio_engine_player_runner_jump_to()`. Aynı immutable runner epoch'una, geçerli indeks ve çalan duruma sınırlandırılır; 240 örnek mevcut cut/drain mekanizmasını kullanır. Paused/scheduled/preparing/finished/error ve ikinci bekleyen cut reddedilir. Var olan NEXT/PREVIOUS davranışı değiştirilmedi.
- [planner.c](../src/playout/planner.c): kabul edilmiş programme config'inden, `entry_id` kimliğiyle eşlenen `plan.display.entries` satırlarına `block_id`, `block_kind`, `block_title` kopyalanır. Sonraki taslak değişikliği hazırlanmış planın sınırlarını değiştirmez. PROMPT-2 programme_title metadata'sı korunmuştur.
- [playout_ui.c](../src/ui/playout_ui.c): cache gecikmesine bağlı olmayan anlık runner snapshot'ı ve immutable planın sonraki farklı blok indeksine geçiş. Global runner ile bağımsız oynatıcı runner'ı ayrı tutulur; prepared/automatic sıra ve geçilebilecek blok bulunmayan durumda eylem reddedilir. PROMPT-2/P3 snapshot/completion bölümleri korunmuştur.
- [broadcast_online.c](../src/ui/broadcast_online.c): kabul edilmiş `Flow.programme` kopyasında sonraki dolu blok bulunur. Yerel segmentte en güncel runner entry_id'si immutable tam akışla eşlenir; çevrimiçi oynatıcıda gerçek PLAYING durumu ve USER_FLOW sahipliği doğrulanır. Mevcut flow_begin yoluyla hedef başlatılır. Scope9 katalog/selection ve P3 progress/status değişiklikleri korunmuştur.
- [test_header_navigation_backend.c](../tests/test_header_navigation_backend.c): gerçek PCM, terminal receipts, sahiplik ve immutable kimlik davranışlarını doğrulayan beş test.

### Uygulanan playlist semantiği

Mevcut RadioS modeli kütüphaneden yayına eklenen playlisti isimli bir programme **event/block** olarak tutar; bu blok `music` veya karışık kaynakta `program` türündedir. Yeni `Sonraki playlist` eylemi mevcut immutable çalışma sırasındaki bloğun kalan şarkılarını atlayıp **sonraki dolu programme bloğunun ilk çalınabilir öğesine** geçer. Boş bloklar hedef değildir. Legacy flat programme'da her event ayrı implicit bloktur. Kütüphane/katalog sayfalaması bu eyleme bağlanmadı. Sonraki blok yoksa eylem pasiftir; sıra başına dönmez. Duraklatılmış/saatli hazırlanan sıra geçişe hazır gösterilmez.

### Mutation ve dış ekip koruması

Her paylaşılan dosya değişikliğinde önce `prompt/agent/source-edit.lock`, sonra hedef `path + .lock` kilidi alınmıştır; güncel dosya yeniden okunmuş, benzersiz preimage/minimal hunk uygulanmış ve yazmadan önce içerik tekrar doğrulanmıştır. Önceki dosyalar `build-prompt5/backend-backups/` altında saklandı. Online dosya Scope9'un açık devrinden sonra yazıldı. Engine jump uygulaması bitince dosya fiziksel yazımı bırakılmış ve koordinatöre diğer ekip için release bildirildi. Aynı dosyalardaki başka ekip bölgeleri geri alınmadı.

### Doğrulama kanıtı ve sınırları

- Backend'in dört değişen C kaynağı `cc -std=c11 -Wall -Wextra -Wpedantic -Werror -fsyntax-only` ile **4/4 PASS**.
- Yeni backend test dosyası strict derleme **PASS**; `G_DEBUG=fatal-warnings` standalone çalışma **5/5 PASS**, 0.234 s.
- PCM testi index0'dan index2'ye gerçekten farklı ses örneğiyle geçer; atlanan index1 için tüketim receipt oluşmaz; eski index0 terminali INTERRUPTED olur. Stale epoch, indeks sınırı, paused ve bekleyen cut gerçek retleri doğrulanır.
- Scheduled ret, manual wait geçişi ve bağımsız oynatıcıya geçişte diğer runner'ın index/epoch/ses tüketiminin korunması ayrıca doğrulanır.
- Planner testi farklı sıralı config.events, start_index=1 ve hazırlanmış kaynağın sonradan mutate/free edilmesine rağmen hedef blok kimliğinin korunmasını doğrular.
- İlk yeni backend fixture çalışması **FAIL**: sadece 240 örnek render edilmesi, terminal retire tag'ın tüketimi için gereken bir sonraki örneği üretmediğinden fixture yeni deck'i bekledi. Mevcut runner testiyle uyumlu olarak fixture 241 örnek render edecek şekilde düzeltildi; üretim backend kodu bu nedenle değiştirilmedi. Son strict derleme ve beş test yukarıdaki PASS sonucunu verdi.
- Root aktarımı: mevcut programme_model/runner/playout testleri **3/3 PASS**; current-tree strict GUI build/link **PASS** ve native_window smoke/görsel root kontrolü **PASS**. Bunlar root'un kanıtıdır; requirements ajanının kendi çalıştırdığı test olarak gösterilmez.
- Validation dört native senaryonun ilk koşusunda footer/local/bağımsız queue **3 PASS**, online test fixture owner lookup **FAIL** bildirdi; lookup fixture'ı düzeltildi ve CTest native+backend yeni koşusu bekleniyor. **Bu an itibarıyla nihai native kabul sonucu beklenmektedir.** Son kabul bu kayıttan ayrı tarihli sonuçla eklenmelidir.

## 2026-10-09 02:10 UTC — bağımsız kod/görsel inceleme

FooterHealth ve HeaderNavigation timer'larının widget destroy sinyaliyle kaldırıldığı, durum verisinin ancak object finalize sırasında serbest bırakıldığı ve window free'nin widget destroy'dan sonra yapıldığı incelendi; bu yaşam döngüsünde açık timer/use-after-free sorunu bulunmadı. Broadcast facade seçili queue_slot bağlamını ve bağımsız runner sahipliğini korur.

Kod incelemesi hızlı Pause → Play tıklamasında cached 100 ms controller snapshot'ının gerçek PAUSED durumunu gecikmeli görebildiğini buldu. Root facade'a local raw-PAUSED → doğrudan RESUME koruması ekledi. Root'un dar göreviyle `header_navigation.h` ve `broadcast_online.c` içine `radios_broadcast_online_navigation_paused()` salt okunur sorgusu eklendi; local flow'da aynı intent, gerçek PAUSED ve immutable segment entry_id sahipliği; çevrimiçi flow'da USER_FLOW, gerçek PAUSED ve aynı video doğrulanır. Query I/O veya komut başlatmaz; strict syntax PASS. Validation immediate Pause → Play testlerini son güncel binary'de doğrulayacaktır; önceki binary'deki regression FAIL bu korumayı içermeyen duruma aittir.

Aşağıdaki özel test fixture ekran görüntüleri `view_image` ile figures kaynaklarıyla karşılaştırıldı:

- [1800 × 1000 özel fixture](../build/prompt5-test/prompt5-footer-1800.png): eski Şema1 bilgi grubu üstten kaldırılmış; aynı yerinde şarkı transport/playlist geçişi var. Şema2 footer içinde devam davranışı, Veritabanı Bağlı, Yedek Bilinmiyor, Uyarı, Ayrıntı, mevcut yenileme, istasyon ve UTC güncelleme okunur.
- [1000 genişlik isteğiyle çekilen özel fixture](../build/prompt5-test/prompt5-footer-1000.png): GTK **gerçekte 1568 × 680** ayırdı. Prompt5 navigation/footer öğeleri bu gerçek boyutta görünür, okunur ve pencere içinde. Dosya adındaki 1000 requested hedefidir; gerçek 1000 genişlikte çalışma doğrulaması değildir. Diğer üst bölümde GERÇEK SIRADAKİ metninin dar alanda kelime kırması root'a bildirildi; Prompt5 taşıma alanında clipping gözlenmedi.

02:04 tarihli successful build kanıtından sonra başka ekipler yeni automation/output API'lerini aynı projeye ekledi; validation'ın son current-tree GUI link'i henüz tamamlanmayan `radios_automation_calendar_programme`, `radios_automation_calendar_prepare_config`, `radios_pulse_backend_configure_output` bağımlılıkları nedeniyle geçici FAIL bildirdi. Sahipleri kendi bölümlerini tamamlıyor; bu dosyalara requirements ajanı müdahale etmedi. Nihai native runtime/güncel link kabulü hâlâ beklenir.

## 2026-10-09 02:25 UTC — backend son ses kabulü ve dar hata düzeltmesi

Önceki 02:04/02:10 bölüm sonuçları tarihsel checkpoint'lerdir. Bu aşamada backend uygulaması ve altı davranış vakası tamamlanmıştır; bütün uygulamanın nihai native/Release/default binary kabulünü root ve validation ayrıca güncel kaynakla doğrular.

`radios_broadcast_online_next_playlist()` başarılı yeni flow oluşturduktan sonra `completed` kuyruğunu temizler. Böylece kullanıcı sonraki bloğa geçtiğinde önceki flow'un gecikmiş completion kaydı yeni akışa taşınmaz. Doğal `flow_begin()` yolu bu dar düzeltmede değiştirilmedi.

`radios_audio_engine_runner_jump_to()` ilk uygulaması renderer'ın ham durumuna bakıyordu. Gerçek ses cihazında kısa kuyruğun PCM verisi tamamen render edilirken son örnekler hâlâ duyulabilir; runner snapshot doğru olarak PLAYING dönerken ham renderer FINISHED olabiliyordu. Mevcut NEXT/PREVIOUS yolu bu noktada geçişi kabul eder; yeni bounded helper'ın ret vermesi yeni helper'a ait bir hataydı. Mutex altında başlangıç RunMark alınması ve aktif runner'ın duyulan RunMark'ıyla değerlendirilmesi bu uyuşmazlığı giderdi. Ham ERROR açıkça reddedilir; epoch, target sınırı, aktif/paused, pending cut/stop/jump ve scheduled korumaları korundu. Output/resample/flush/handover uygulamaları bu patch'te değiştirilmedi.

PROMPT1'in dar source-release'i ve root yetkisi üzerine global `source-edit.lock` → `engine.c.lock` altında yalnız kendi helper hunk'u güncel içerikle yazıldı; 02:23:06 fiziksel yazımı tekrar bırakıldı. Önceki engine SHA-256 `ae9af9371eae0db6c3dc16c3ce2f526aa8190d121faa2e05372a25098d30e3d7`; sonraki `be627c6dea44e656f0f5fd28670de3f90b2b5f40593937d12d229f86d6a0123c`. Yedek: `build-prompt5/backend-backups/engine.c.20261009T022306720639Z.before`.

[Gerçek backend testleri](../tests/test_header_navigation_backend.c) artık altı vakadır. Altıncı test owned private Pulse sink'lerinde son kaynak PCM EOF olduğunda gerçek audible PLAYING/index2/frame926 durumunu gözlemiş; önceki binary aynı epoch hedef0 geçişini reddetmiş ve test **FAIL** olmuştur. Sonraki output callback'in renderer EOF kaydını üretmesi için 20 ms beklenir ve gerçekten duyulan PLAYING durumu tekrar doğrulanır; üretim cihazı veya renderer yapay olarak durdurulmaz. Düzeltilmiş helper aynı koşulda hedef0'ı kabul eder ve gerçek hedef tüketimi gözlenir. Bu test helper'ın aynı aktif sıradaki geçerli herhangi bir hedef kontratını denetler; arayüzde son bloktan sonra yeni playlist varmış gibi davranılmaz.

Doğrulama ve kalıcı kanıtlar:

- Engine `-Wall -Wextra -Wpedantic -Werror` syntax **PASS**; mevcut CMake `flags.make` seçenekleriyle yeni engine object ve özel audio archive derleme/strict backend link **PASS**. Paylaşılan build artifact'leri requirements ajanı tarafından değiştirilmedi.
- [Düzeltme öncesi gerçek API ret kanıtı](../build-prompt5/realtime-before-fix.log): **FAIL**, exit250; audible index2/frame926, doğru epoch/hedef olmasına rağmen bounded jump FALSE.
- [Düzeltme sonrası aynı odaklı test](../build-prompt5/realtime-after-fix.log): **1/1 PASS**, audible index2/frame849 ve hedefin tüketimi.
- [Owned Pulse ile bütün backend](../build-prompt5/navigation-backend-owned-pulse-final.log): **6/6 gerçek vaka PASS**, skip yok, fatal-warnings etkin. PCM fence/pause/pending, scheduled, manual wait, bağımsız owner, immutable blok kimliği ve gerçek render-ahead EOF kapsanır.
- [Pulse çevresi olmadan backend](../build-prompt5/navigation-backend-bare-final.log): **5 PASS + 1 SKIP**. Bu koşu altı gerçek ses vakası geçmiş gibi gösterilmez; altıncı vaka kendi explicit owned Pulse wrapper'ıyla çalıştırılmalıdır.
- İlk realtime fixture gereksiz audible index<2 şartıyla EOF penceresini bulamamıştı: [detector fixture FAIL](../build-prompt5/realtime-before-fix-detector-failure.log). Şart gerçek son audible index2'ye daraltılınca output callback'ten önceki deneme geçişi kabul etti: [erken callback PASS](../build-prompt5/realtime-before-fix-early-callback-pass.log). Bunlar bug düzeltmesinin kanıtı olarak kullanılmaz; yukarıdaki aynı nihai fixture'ın pre-fix API FAIL / post-fix PASS karşılaştırması geçerli kanıttır.

Nihai integrated native dört senaryo, Release ve varsayılan launcher binary kurulumu root/validation'ın son kaynak sonucunu bekler. Gerçek1000px genişlik veya farklı font başarısı bu backend kabulüyle varsayılmaz; önceki 1800 ve actual1568 ekran kanıtının sınırları geçerlidir.

## 2026-10-09 02:28 UTC — güncel integrated ses ve bağımsız görsel checkpoint

Validation'ın 02:24:24 integrated hedef koşusu: `header_navigation_realtime` **1 PASS / 2.12479 s**, audible index2/frame1331 ve gerçek EOF penceresi; bare backend **5 PASS + 1 SKIP**. Native footer **PASS / 1.89 s**. Hızlı Pause → Play fixture'ının önceki cached-state precondition'ı aslında PREROLL durumunu kabul ettiği için failure06 oluştu; fixture gerçek state/index/precondition ile güçlendirildi ve son dört native senaryo sonucu hâlâ bekleniyor. Failure06 XML/log ve kaynak/binary drift olmadığını gösteren manifestler validation tarafından korunuyor; bu başarısız fixture native kabulü olarak gösterilmez.

02:27:41/42 tarihli yeni footer capture'ları requirements ajanı tarafından `view_image` ile yeniden incelendi. Son actual boyutlar **1800 × 1000** ve **1568 × 680**; ikinci dosyanın requested1000 adı gerçek1000 kanıtı değildir. Alt bardaki tüm taşınan bilgiler/Ayrıntı/yedek ile yenileme/istasyon/UTC; üstteki altı navigation eylemi iki actual boyutta okunur ve erişilebilir. Sonraki/Geçişe kalan başlıkları tek satırdır. Dar görüntüde RadioS logo metni başka header bölgesinde satır kırar; root'a bildirildi, bu prompt5 footer taşıma alanında clipping değildir.

Aynı dosya adları sonraki fixture koşularında yenilenebildiğinden bu incelemenin immutable kopyaları ve SHA-256'ları ayrıca saklandı:

- [1800 × 1000 inceleme kopyası](../build-prompt5/review/footer-1800-20261009T022741.png): `0985190deff69ca129a7f657f4c75150acbe244620ebdb697be8d3a37cf1b264`.
- [Actual1568 × 680 inceleme kopyası](../build-prompt5/review/footer-actual1568-20261009T022742.png): `ce8ca79fbd508f1a5e7e61a58a119335fbbab06363478bdf898706e826dec951`.

Root current-source strict Release, default launcher binary ve validation son native dört vaka kabulü henüz bu checkpoint ile tamamlandı sayılmaz.

## 2026-10-09 02:31 UTC — son kaynak değişimi sınırı

Readonly incelemede engine SHA `4f5ff31124247bcdc1471b832535cd92b9961724cb8a3fff088c540b2a5941b5` görüldü. Önceki `be627...` kaynağı backup+exact Prompt5 hunk ile yeniden kurulup hash'i doğrulanarak karşılaştırıldı. Değişiklik yalnız `actual_cursor_locked()` içinde sıfır tüketimli matching-generation history record'larını gerçek konum ilerlemesi saymayan iki satırlık silent-history filtresidir. Prompt5 audible `runner_jump_to` guard'ı ve player wrapper'ı birebir korunmuştur. Requirements ajanı bu dış değişikliği yazmadı. Ortak kayıttaki Scope6/Scope11 STOPzero race ve 02:30:21 dar seri devir talebi gerekçeyle uyumludur; actual writer/FIX READY hash receipt'i inceleme anında henüz ortak kayıtta bulunmamıştır, root/coordination'a bildirildi.

02:25 tarihli standalone **6/6 gerçek Pulse PASS**, engine **be627** snapshot'ına aittir. Bu sonuç **4f5ff** snapshot'ının nihai kabulü olarak gösterilmez. Validation failure07 source-before/after karşılaştırmasında 25 kaynaktan yalnız engine değişmiş, binary değişmemiştir; root current-source rebuild başlatmıştır. Son current integrated native/Release/smoke/default install sonucu beklenir. Bu ayrım başka ekibin mevcut STOPzero düzeltmesini geri almak veya yeni kaynağı test edilmeden kabul etmek için kullanılmaz.

## 2026-10-09 02:36 UTC — Prompt5 işlevsel nihai kabul

**PDF5'teki bütün on gereksinimin RadioS kaynak ve işlevsel uygulaması doğrulanmıştır.** Bu kabul yalnız Prompt5 kapsamıdır. Varsayılan binary'nin atomik kurulumu ve son launcher smoke'u root tarafından tamamlanmaktadır; tamamlanmadan bu belge kuruldu iddiasında bulunmaz.

Validation 02:33:27–02:33:58 UTC canonical `source-edit.lock` paylaşılan okuma kilidi altında güncel kaynakla iki test target'ını strict derledi, sonra üç CTest hedefini tek süreçte çalıştırdı: **3/3 PASS**, yaklaşık31s. Önce/sonra **28 kaynak ve 2 binary SHA-256 eşit**, drift yok. Tested engine `c53c0e8c24b1b0fb3346d33e81725312db4981733ca147fcac0f78052b01397d`.

- Native gerçek Window/GTK/private DB/owned ses fixture'ları: **4/4 PASS**, toplam26.907s. Footer/diagnostic/backup/offline3.35s; gerçek local+mixed next/immutable block ve hızlı Pause→Play5.64s; seçili bağımsız player sıra izolasyonu5.13s; online immutable blok/provider stub3.63s.
- Bare backend: **5 gerçek PASS + 1 env SKIP**, 0.824351s. Owned Pulse realtime hedef: **1 gerçek PASS**, 3.26333s. Böylece altı backend vaka gerçekten çalıştırılmıştır; bare SKIP gizlenmemiştir. Üç CTest hedefinin altında toplam10 gerçek davranış vakası vardır (4 native + 5 bare + 1 realtime).
- Requirements ajanının güncel **c53** engine immutable clone'u ile private archive strictcompile/link ve owned Pulse bütün altı test **6/6 PASS**, skip yok; engine test sonrasında da aynı c53 hash'inde. [Özel altı gerçek ses logu](../build-prompt5/navigation-backend-owned-pulse-frozen-current.log), [engine/binary/test manifesti](../build-prompt5/navigation-backend-frozen-current-manifest.json). Bu özel çalışma diğer archive objelerini sabit tutarak engine delta'sını doğrular; full project kabulü validation'ın yukarıdaki 28source fenced koşusudur.

Nihai kanıt dosyaları requirements ajanı tarafından JSON/XML olarak okundu; XML üç hedef, sıfır failure/disabled/skip target bildirir. Bare hedefin kendi env SKIP'i TAP içinde ayrıca kayıtlıdır. Full JUnit stdout dört native vaka için exact TAP1..1 sonuçlarını içerir:

- [Tam JUnit](../build/prompt5-test/header-navigation-pass-02-stable.xml), [tam CTest/TAP logu](../build/prompt5-test/header-navigation-pass-02-stable.log).
- [Sabit koşu manifesti](../build/prompt5-test/header-navigation-stable-final.json), [önceki kaynak/binary SHA](../build/prompt5-test/header-navigation-pass-02-stable-source-before.json), [sonraki kaynak/binary SHA](../build/prompt5-test/header-navigation-pass-02-stable-source-after.json).
- [Son1800×1000 capture](../build/prompt5-test/prompt5-footer-pass-02-1800.png), [son actual1568×680 capture](../build/prompt5-test/prompt5-footer-pass-02-narrow.png). İkisi de requirements ajanı tarafından `view_image` ile yeniden figures şemalarıyla karşılaştırıldı; Prompt5 üst navigation ve alt bilgi/eylem barı okunur/erişilebilir, clipping yok. Son dar görüntüde RadioS logosu yeniden tek satırdır; önceki logo satır kırma gözlemi tarihsel kalır.

| Gereksinim | Nihai Prompt5 sonucu | Kanıt |
|---|---|---|
| P5-01 | Tamamlandı: eski üst sağlık grubu kaldırıldı | Native footer + iki son capture |
| P5-02 | Tamamlandı: devam davranışı/değeri alt barda | Native footer + görsel karşılaştırma |
| P5-03 | Tamamlandı: gerçek DB bağlantısı ve disconnected değişimi alt barda | Native connected/offline assertions |
| P5-04 | Tamamlandı: yedek bilgisi ve Acil erişimi alt barda | Gerçek yedek buton click→Acil |
| P5-05 | Tamamlandı: gerçek bilinmeyen uyarı yer tutucusu alt barda | Native footer mapping + capture |
| P5-06 | Tamamlandı: tanı Ayrıntı açılır/kapanır | Gerçek diagnostic dialog assertions |
| P5-07 | Tamamlandı: aktif gerçek şarkıya üstten geçiş | Native local/selected queue/online + PCM receipt |
| P5-08 | Tamamlandı: sonraki dolu immutable programme playlist bloğuna geçiş | Native draft mutation/empty block/local-online ve planner kimlik testi |
| P5-09 | Tamamlandı: mevcut bildirim/yenileme/istasyon/UTC öğeleri korundu | Eski yenileme handler kaynak incelemesi; DB context/footer/son capture |
| P5-10 | Tamamlandı: kaynak/strictbuild/işlev/ses/görsel kabulü raporlandı | Sabit28source2binary üçCTest; PDF/PNG byte-eşitliği ve son visual review |

Kabul sınırları açık kalır: Requested1000 capture gerçekte1568×680'dir; **gerçek1000 genişlik veya farklı font başarısı iddia edilmez**. PDF ayrı1000px/font şartı tarif etmez. Online fixture gerçek RadioS Window/Flow/authorization/private DB yollarını çalıştırır, provider status/player API'lerini stub ile kontrol eder; canlı YouTube sağlayıcısından ağ/ses kabulü değildir. Gerçek local ses ve realtime render-ahead EOF ayrı owned Pulse vakalarında doğrulanır.

Önceki dış engine delta'nın yazarı artık ortak kayıtta doğrulandı: **PROMPT1** actual cursor silent-history filtresini02:28:57'de be627→4f5ff311 uygulamış;02:30:45 FIX READY ve dar RELEASE kaydı, `cursor-evidence-fix.json` ve `engine.before-cursor-evidence-20261009T022857Z.c` yedeği vardır. P6 üretim engine yazmamıştır. Daha sonraki seek-command tip düzeltmesi PDF4 sahibi tarafından kaynakta uygulanmıştır; prompt5/root girişimi en güncel doğru preimage nedeniyle NO-OP olmuştur ve kaynak byte yazımı yapmamıştır. Doğru tip son c53 strict/native snapshot'ına dahildir. Requirements ajanı bu iki dış hunk'u yazmamış; kendi audible jump/player wrapper'larının korunduğunu salt okunur doğrulamıştır.

Root'un [son fenced Release manifesti](../build/prompt5-test/release-source-snapshot.json)02:33:24–02:33:31 strictbuild exit0 ve source drift olmadan candidate üretildiğini bildirir. [Release derleme logu](../build/prompt5-test/release-build-fenced-final.log); binary SHA-256 `b1578bb7c6c97e8667848ade86c4aa8cb615b067d4ea62a264077bff8013726e`. Root relevant24 Release kaynak hash'inin validation baseline'ıyla sıfır farkını teyit etti. **Default atomik install ve launcher8-profile smoke sonucu root'un final kurulum kaydına bırakılmıştır.**

## 2026-10-09 02:40 UTC — kurulum doğrulaması ve son bağlam kapanışı

**Prompt5 kaynak/işlev/görsel kabulü ve varsayılan launcher kurulumu tamamlandı.** Önceki checkpoint'lerde bekleyen install ifadeleri bu tarihli receipt ile kapanmıştır. Root02:35:19.391782 UTC strict/fenced/tested Release candidate'ini `build/radios` üzerine atomik kurmuştur. Requirements ajanı [kurulum receipt'ini](../build/prompt5-test/installed-binary.json) okudu ve canlı kurulu dosyanın SHA-256'sını bağımsız hesapladı: `b1578bb7c6c97e8667848ade86c4aa8cb615b067d4ea62a264077bff8013726e`; candidate/receipt ile eşit. Önceki binary `build/prompt5-backups/root/radios-before-install-023519` yedeğinde saklıdır.

[Kurulu binary launcher smoke'u](../build/prompt5-test/installed-smoke.log) gerçek mapped window, **8 profil** ve gerçek iş/durum teslimi **PASS** bildirir. [Kurulu1920×1080 ekran](../build/prompt5-test/prompt5-installed-ui.png) requirements ajanı tarafından `view_image` ile figures şemalarıyla karşılaştırıldı: eski sağlık grubu üstte yok; transport/playlist aynı alanda; alt barda devam davranışı, doğrulanmamış gerçek DB `Bağlı değil`, yedek/uyarı/Ayrıntı ve cog/yenileme/istasyon/güncelleme alanları görünür ve clipping yok. Private fixture'daki `Bağlı` ile launcher'ın bağlantısız değeri farklı gerçek koşullardır; veri durumu sahte sabitlenmemiştir.

Seek tip düzeltmesinin önceki atfı düzeltildi: **PDF4 sahibi** kaynakta doğru `AudioRunner` tipini uygulamıştır. Prompt5/root'un02:34:12 girişimi en güncel doğru preimage nedeniyle **NO-OP** olmuştur; engine/yedek için byte yazmamıştır. Root yetki mesajını uygulanmış patch sanan önceki doc atfı düzeltilmiştir; gerçek source writer'ın yerine prompt5/root yazılmaz.

Kurulu Release ve stable native kabulü **c53** kaynak baseline'ına aittir. Sonraki PDF4 snapshot eklentisi current engine'i `491ffd84f9f484634ef76e4e3b8ac3f9d6b906fc9eebbe30b592ad86c902901d` yapmıştır: c53'e göre yalnız deck snapshot loop'unda `runner_paused` flag'ini yayınlayan tek satır vardır. Shared AGENT PDF4 root dar snapshot claim'i,02:35–36 paused adapter ve ABI/sanitizer kabulüyle yazarı kaydeder. Prompt5 bounded jump ve bağımsız player wrapper'ı birebir aynıdır; navigation `RadioSAudioRunSnapshot` kullanır ve bu yeni deck flag'ini doğrudan kullanmaz. Yeni Prompt5 riski bulunmamıştır; requirements ajanı üretim dosyalarını bu fazda değiştirmemiştir.

Validation son 49 motor kaynağıyla canonical shared readfreeze altında `test_programme_model`, `test_runner`, `test_playout` hedeflerini strict yeniden derleyip **3/3 PASS / 5.63s** aldı; 18 kaynak ve 3 binary hash'leri değişmedi. Bare koşuda 2 programme model + 23 runner + 22 playout = **47 gerçek PASS**, `/runner/pulse` bir env SKIP idi. Aynı binary'nin bu Pulse vakası ayrıca owned private sink ortamında **1 gerçek PASS / 2.04s**, skip yok ve 4 kaynak/binary hash'i sabit olarak çalıştırıldı. Böylece **48 benzersiz gerçek core vaka** geçmiştir; tek bare koşuda 48 skip olmayan vaka geçmiş gibi gösterilmez. [Ayrı gerçek runner Pulse logu](../build/prompt5-test/fresh-backend-regression-runner-pulse.log), [Pulse manifesti](../build/prompt5-test/fresh-backend-regression-runner-pulse-manifest.json). [Son core regresyon manifesti](../build/prompt5-test/fresh-backend-regression-manifest.json), [JUnit](../build/prompt5-test/fresh-backend-regression.xml), [tam log](../build/prompt5-test/fresh-backend-regression-full.log). Bu son kaynak testi, kurulu b157 binary'nin daha sonraki49 eklentisini kendiliğinden içerdiği anlamına gelmez. Gereksizce eski native4 kanıtı üzerine yeni source hash yazılmadı.

Tamamlanan Prompt5 işleri: bütün PDF/figures incelemesi ve piksel karşılaştırması; 10 gereksinim matrisi; health/footer ve navigation entegrasyonu; immutable local/online/player playlist geçişleri; gerçek PCM/state/epoch fencing; hızlı Pause→Play ve render-ahead regresyon düzeltmeleri; stable strict/native/ses/görsel kabulü; kurulum/launcher receipt; kaynak yedekleri ve saatlik önemli-olay logları. **Prompt5 requirements/backend kaynağında bekleyen görev yoktur.** Actual1000/farklı font ve canlı YouTube testleri yapılmış gibi gösterilmez. Bu sonuç tüm deponun bütün ekip testlerinin başarılı olduğu iddiası değildir; diğer kapsamda bildirilen P6 DJ pan/crossfade başarısızlığı kendi ekibinin açık kaydıdır ve bu Prompt5 kabulüyle kapatılmaz.
