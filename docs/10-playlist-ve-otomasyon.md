# Playlist, program ve otomasyon

RadioS 0.10.0, kalıcı playlist/program/otomasyon kayıtlarını gerçek C ses motorunda yürütür. HTML tasarım referansı ve otuz CSS kaynağının aktarımı korunur; masaüstü arayüz GTK/C ile çalışır. Bu aşamanın yerel çalma geçmişi MAIN cihazının gerçekten tükettiği PCM'ye dayanır. Streaming, yayın sunucusuna teslim ve reklam yayın kanıtı sonraki aşamaların kapsamındadır.

## Kullanım

Yayın görünümünde **Parçalar / Jingle / Anons / Reklam / Programlar / Playlist**
barındaki **Playlist** sekmesi veritabanındaki playlistleri doğrudan gösterir.
Solda listeler, sağda seçili listenin parçaları bulunur. Arama satırının sağındaki
**Playlist oluştur** düğmesiyle başka sayfaya geçmeden liste oluşturulur;
veritabanına kaydedilen liste burada seçilir. Başarılı oluşturmada arama filtresi
temizlenir, böylece yeni liste gizlenmez. Düzenleyiciye geçiş ve parçayı/tüm
listeyi yayın sırasına ekleme korunur.
Sekmeye geri dönüldüğünde kayıtlar yeniden okunur; yalnız sekme seçmek yayın
başlatmaz veya programı değiştirmez. Eski alt playlist expander'ı kaldırılmıştır.
Barda program adı/tarihi görünür; onay ve kullanım hakları **Teknik yönetim ›
Program onayı** sekmesindedir, cue/işaret editörleri ise satırlardaki **V**
düğmesinden açılır.

Playlist veya kayıtlı program genel event listesinde bir eventin üzerine
bırakılırsa onun **ardına** yeni event olarak eklenir; sonraki eventler ve mevcut
şarkılar korunur. Eventi açıp düz parça listesine bırakırsan yeni event oluşturmaz:
parçalar bıraktığın satırın önüne/arkasına, boş alana bırakınca o eventin sonuna
eklenir. Playlist sekmesinden tek parça da sürüklenebilir. Doğrulanan parçalar
özgün playlist sırasını korur; açık liste hemen güncellenir ve ilk eklenen parça
seçilir. Eklemek yayın başlatmaz veya çalışan otomasyonu kesmez.

Playlist bırakma işlemi gerçek `desktop-playout/prepare` ile medya/SHA kontrolü
yapar ve mevcut program belgesini revision/CAS ile kaydeder. Yanıt beklerken
hedef program/seçim değişirse veya pencere kapanırsa eski hedefe yazılmaz.

### Oynatıcıya özel yayın sıraları

Event listesinin üstündeki **Oynatıcı** seçicisi, **Genel yayın sırası** ile
gerçek oynatıcı kartlarının ayrı sıraları arasında geçiş yapar. Genel sıra
eski **Mevcut program** belgesini, iki deckli geçişleri ve AutoDJ bağlantısını
korur. **Oynatıcı 1**, **Oynatıcı 2** vb. seçimler kendi eventlerini,
seçimini, geçiş ayarını ve çalma geçmişini gösterir; seçenekler oynatıcının
kartındaki adı taşır. Seçici her zaman görünür. Seçici değiştirmek başka
bir sırayı başlatmaz veya durdurmaz. Başlat, duraklat/devam, durdur ve
önceki/sonraki yalnız seçili sıranın gerçek C yürütücüsüne gider.

Her event ve içerik satırının sağında, durumun altında hangi oynatıcıda çaldığı
sürekli yazar: içerik gerçekten çalmaya başlayınca çaldığı oynatıcı, öncesinde
kabul edilmiş planın oynatıcısı, o da yoksa seçili oynatıcı sırasının kendi
oynatıcısı; **Genel yayın sırasında** otomasyon seçene kadar "Oynatıcı · otomatik".
Event satırı içindeki tüm oynatıcıları listeler; ayrı çalışan (yarı AutoDJ)
event yürütücüsünün oynatıcısını gösterir.

Canlı oynatıcı kartının üstünde yüklenen parçanın başlığı, sanatçısı, albümü,
kaynağı ve sıradaki parça görünür. Genel yayın, AutoDJ ve oynatıcıya özel
sıralar bu bilgileri gerçekten yüklenmiş medya/sürüm kimliğiyle eşleşen,
değişmez yürütme planından alır; seçicide başka sıraya geçmek bilgileri
karıştırmaz. Kütüphaneden yüklenen medya bilgileri yükleme nesline bağlıdır.
İçerik değiştiğinde veya boşaltıldığında eski başlık tutulmaz. Uzun bilgiler
kartı büyütmeden kısaltılır; tamamı fareyle üzerine gelince gösterilir.

### Canlı kartlarda SYNC ve AutoDJ hedefi

Karttaki görünür **Girdiler** ve **Ana çıkışa gönder** alanlarının yerine
**SYNC**, ana deck seçimi, gerçek BPM/tempo bilgisi ve **AutoDJ** hedef
oynatıcısı gelir. Playlist yükleme/sürükleme ve dahili sıra korunur; MAIN
yönlendirmesi mixer altyapısında devam eder.

İki doğrulanmış yerel parça yükle; SYNC yapılacak kartta diğer oynatıcıyı
ana deck seçip **SYNC**'e bas. Her iki kaynakta güvenilir analiz gerekir.
Tempo en yakın yarım/çift BPM eşdeğerliğiyle eşleşir, hız değişimi ±%16 ile
sınırlıdır. Beat fazı kaynak konumuyla hizalanır. FFmpeg
[`atempo`](https://ffmpeg.org/ffmpeg-filters.html#atempo) filtresi gerçek decoder
işçisinde tonu koruyarak PCM üretir; slider veya örnekleme hızını değiştiren
sahte bir gösterim değildir. SYNC seçilen gridin o andaki temposuna eşler;
ana parçayı değiştirdiğinde yeniden SYNC yap. Değişken tempolu içerik için
kesintisiz dinamik grid takibi iddia edilmez.

Kaynak kartta **AutoDJ → Oynatıcı N → Başlat** seç. Kaynak playlistte kalan
en az iki yerel öğe bu iki gerçek deck arasında dönüşümlü yürür. Playlist
yoksa iki karttaki doğrulanmış parçalar kullanılır. Çalan ve aynı sürüme
bağlı kaynak PCM'i yeniden yüklenmeden devralınır; hazır hedef açık operatör
seçimiyle değiştirilebilir. Başka sıraya ait veya çalan hedef zorla alınmaz.
Çiftin kartlarındaki transport düğmeleri aynı yürütücüye gider; tek decke
manuel müdahaleyle AutoDJ'yi bozmaz. Geçiş, çıkan gridin beatinde başlayıp
tempo ayarlı incoming decke crossfade yapar. Kaynak cursor'ları ve geçmiş
gerçek MAIN tüketimine, hızlandırılmış/yavaşlatılmış kaynak frame'lerine
bağlı kalır. Genel yürütücü aynı anda yalnız bir AutoDJ çiftini yönetir;
diğer bağımsız oynatıcı sıraları çalışmaya devam edebilir.

Ana deck/hedef seçimleri `audio_preferences.player_automation` içinde
`player`, `sync_master`, `autodj_target` alanlarıyla, 1–32 kimlikleri ve 0=kapalı
olarak CAS ile saklanır. Yeniden açma ses veya SYNC başlatmaz. Hazırlanan
programın `autodj_players: [kaynak, hedef]`, `beat_sync: true` sözleşmesi de
typed DB doğrulamasından geçer; kendine atama, döngülü ana deckler, yanlış
sürüm ve eski hazırlık sonuçları reddedilir. Online sağlayıcıların ortak PCM
akışı, doğrulanmış bağımsız deck analizi değildir; bu SYNC/AutoDJ yolu yalnız
yerel SHA'ya bağlı medyayı kabul eder.

Her sıra istasyon kapsamındaki ayrı `programme` UUID/revision belgesidir:
`Oynatıcı N yayın sırası`. `queue_player` 1–32 arasındaki fiziksel oynatıcı
kimliğidir; `queue_transition` EOF=0, cue=2 veya manuel=3, `queue_overlap=0`
olarak doğrulanır. Eski programlarda bu alanlar zorunlu değildir. Ayrı
`Oynatıcı N oynatma geçmişi` kayıtları gerçek MAIN tüketim makbuzlarından
oluşur; hazırlama, liste seçme veya ön yükleme çalma geçmişi üretmez.
Bu çalışma belgeleri **Programlar** arşivinde şablon olarak gösterilmez;
arşivlenen program kopyasından oynatıcı bağlamı çıkarılır.

Her `programme` belgesi isteğe bağlı bir `view` nesnesi taşıyabilir (event
listesi penceresi): `event_fetch` 1–500, `event_show` 1–200, `content_fetch`
1–500, `content_show` 1–500, `refresh_seconds` 1–60; beşi de tam sayı ve
zorunludur, çekme adetleri gösterme adetlerinin altına inemez.
`view_by_title` aynı nesneyi program başlığına göre tutar (en çok 64 giriş,
başlık 1–160 karakter). Eski kayıtlarda bu alanlar yoktur ve olduğu gibi
geçerli kalır; `desktop-playout/prepare` yanıtının `config` alanı ikisini de
değiştirmeden döndürür.

Playlist/program bırakma hedefi o anda seçili sıradır. Yanıt gecikmişken
oynatıcı değiştirilirse eski ekleme yeni sıraya taşınmaz. Yeniden açmada
her sıranın UUID'si, sırası ve ayarları DB'den geri yüklenir; ses kendiliğinden
başlamaz. Kart kaldırıldığında kayıt silinmez, seçicide kartın kaldırıldığı
belirtilir. Seçili oynatıcı boş ve MAIN'e yönlendirilmiş değilse başlatma
reddedilir; başka boş oynatıcıya sessizce geçilmez.

Tek fiziksel oynatıcı iki parçayı bindiremez: bu sıralarda Crossfade ve
bindirme kapalıdır; iki deckli Crossfade için genel sıra kullanılır.
Mevcut çevrimiçi PCM yolu ortaktır; bağımsız oynatıcı sıraları şimdilik
doğrulanmış yerel medya içindir. Çevrimiçi içerikle başlatma açık hata verir
ve diğer sırayı durdurmaz. Motor deck sahipliğini bütün yürütücüler arasında
denetler; aynı decki genel AutoDJ veya başka bir sıra zorla ele geçiremez.

1. Medya kütüphanesinden gerçek ses dosyalarını içe aktar. Playlist düzenleyicisinde **Kütüphaneden seç** ile kayıt bağla; mevcut metadata satırına doğrulanmış medya kimliğini ekle. Bağlanmamış eski satırlar düzenlenebilir; yürütme için gerçek kayıt gerekir.
2. Playlist'teki seçili parçayı veya bütün playlist'i program kuyruğuna ekle. Program sırası yeni olay UUID'leriyle kalıcı kaydedilir. Başlat, duraklat/devam, durdur ve önceki/sonraki kontrolleri gerçek motoru yönetir. Geçiş türü EOF, bindirme, cue veya operatör geçişidir.
3. Program takviminde UTC başlangıcı seç. İleri tarihli başlatma bekler; **Durdur** hazırlığı ve takvimi iptal eder. Olayların sabit saati, kes/bitir davranışı ve sınırlı beklemesi yürütmeye uygulanır. Geçmiş bir başlangıç hemen hazırlanır; kaçırılmış izin pencereleri sessizce uygun kabul edilmez.
4. Otomasyon kaynaklarında gerçek playlist'i, oynatıcıları, kaynak sırası/ağırlığı, seçim yöntemi ve tekrar aralıklarını ayarla. Günler, hafta içi/sonu ve UTC saat pencereleri geçerlidir. İsteğe bağlı jingle, jingle oynatıcısı, kaynak geçişi ve yedek kaynak ayarlanabilir.
5. **Önizle**, henüz kaydetmediğin formun deterministik planını gösterir. Önizleme sese, kalıcı geçmişe veya rotasyon sayacına dokunmaz. **Başlat** doğrulanmış parçaları MAIN'de çalar; **Durdur** sonraki bekleyen hazırlığın sesi yeniden başlatmasını engeller. Önizleme ve gerçek geçmiş tabloları dışa aktarılabilir.

Takvim ve plan pencerelerinin saat dilimi UTC'dir; istasyonun kayıtlı saat dilimi ayrıca korunur. Gece yarısını aşan pencere önceki günün başlangıcına bağlanır. Uygulama yeniden açıldığında otomasyon ve canlı oynatma başlamaz.

## AutoDJ deck ayrıştırma

Otomasyonun **Deck** bölümünde AutoDJ'ye ayrılacak gerçek oynatıcıları seç. İki deck A/B olarak dönüşümlü kullanılabilir; dört veya daha fazla seçili deck de sırayla kullanılır. Kimlikler sayısal sıradadır. Tek deck modu bindirmesiz EOF/cue/manuel yürütme içindir. Sampler ve ön dinleme deckleri bu havuza girmez. AutoDJ sadece seçili, boş ve MAIN'e yönlendirilmiş deckleri sahiplenir; elle yüklenmiş decki devralmaz. Sahiplenilmiş decke manuel müdahale mevcut motorun otomasyon sahipliğini iptal eder.

Her seçili deckin sağında **Genel geçiş / Parça sonu / Crossfade / Cue-auto-next / Manuel** seçimi ve 0–30 saniye bindirme alanı vardır. Ayar, o deckteki parçadan sonraki parçaya geçişi yönetir; başka playlist/program oynatımlarının genel geçişini değiştirmez. EOF ve manuel seçimlerinde bindirme sıfırlanır. Cue modunda doğrulanmış medya işaretleri varsa gerçek auto-next/fade/bindirme değerleri kullanılır; yoksa deckin bindirmesi yedektir. Öncelik: kuru jingle güvenliği, decke özel geçiş, playlist sınırı geçişi, genel geçiş. Gelen parçanın süresi bindirmeyi sınırlar.

**Sonraki decke geç**, doğrudan aynı AutoDJ yürütücüsünün NEXT komutunu çalıştırır. Manuel geçişte parça sonunda bekleyen otomasyonu bir sonraki atanmış deckte sürdürür; çalarken de operatör geçişi yapılabilir. Komut manuel oynatıcı transport'una verilmez ve yürütücü sahipliğini iptal ederek başka bir decki devralmaz.

**Kaydet**, deck ayrımını ve geçişleri mevcut `automation/main` typed belgesine CAS/revision ile birlikte yazar. İsteğe bağlı alanın sözleşmesi:

```json
"deckTransitions": [
  {"deck_id": "1", "transition": 1, "overlap": 3.0},
  {"deck_id": "2", "transition": 0, "overlap": 0}
]
```

`transition`: 0 EOF, 1 crossfade, 2 cue, 3 manuel. Deck kimliği seçili `decks` listesinden tekil ve kanonik `1`–`32` olmalıdır. En fazla 32 kayıt kabul edilir; yinelenen, seçilmemiş, geçersiz kimlik/tür/süre ve EOF/manuelde sıfırdan farklı bindirme DB yazılmadan reddedilir. Alanı olmayan eski kayıtlar genel geçişle çalışır; veri deposu şema sürümünü değiştiren veya eski belgeleri topluca yeniden yazan migration gerekmez. Mevcut istasyon/yetki, idempotency ve CAS kuralları geçerlidir.

C planlayıcı deck atamasından sonra geçişi gerçek `RadioSAudioRunItem` üzerine uygular; bu yalnız önizleme notu değildir. Devam partilerinin deck fazı planlama sırasında taşınır, böylece A/B'ye ait farklı fade süreleri devam partisi eklenince başka decke kaymaz. Native önizleme tür/süreyi; canlı durum mevcut ve sıradaki fiziksel decki, türü ve bindirmeyi gösterir. Kayıt veya önizleme kendi başına yayını başlatmaz; açık otomasyonun ayarları yeni bir başlatmaya kadar değişmez.

Değişiklik, `src/ui/automation.c`, `src/playout/planner.c`, `src/ui/playout_ui.c` ve `database/SDBASE/src/sdbase/desktop_api.py` üzerinden uçtan uca bağlıdır. Servis uzun süre çalışan Python sürecidir; yeni alanı kullanmadan önce güncel backend ve native uygulamanın yeniden başlatılması gerekir. Testler özel veritabanı/Pulse cihazlarında yürür; üretim kayıtlarını dönüştürmez.

AutoDJ kabulü: strict C11/Werror tam derleme; C planlayıcı **21/21**, planlayıcı ASan/UBSan ve leak denetimi açık **21/21**, backend hazırlık/kalıcılık **15/15**, typed API **20/20**, native yürütücü **6/6** geçti. Native kabul gerçek MAIN PCM'sinde A/B crossfade, yalnız seçili iki deckin sahiplenilmesi, bindirmesiz B→A dönüşü, devam partisinde deck kuralı, kalıcı kayıt/yeniden açılış, manuel bekleme/NEXT ve normal program çalarken AutoDJ NEXT'in kapalı olmasını doğrular. Birleşik regresyon **10/10**; son manuel kontrol ve kaynaklarla hedefli yeniden kabul **5/5**, **117,81 saniye** başarılıdır. Açık üretim uygulaması/servisi yeniden başlatılmadı; mevcut ses kesilmedi.

## AutoDJ otomasyonu: tasarlanmış geçiş ön ayarları

Rotasyon düzenleyicinin kitaplık alt çubuğundaki **AutoDJ otomasyonu…** düğmesi, "AutoDJ: geçiş ve kanallar" sayfasının yanında üçüncü bir sayfa açar. Sayfa kanal, geçiş türü veya süre alanlarını tekrar göstermez; ön ayarları, türlere atamayı ve zaman dilimlerini yönetir.

**Ön ayarlar.** Çip satırı tanımlı geçişleri listeler. İlk açılışta sekiz şablon gelir: Düz geçiş, Bas dalgalı geçiş, Ton erit, Slam bas, Erime, Yankı ile çık, Filtre süpürme, Kes. **Ekle ▾** menüsü (sekiz şablon ve "Seçilinin kopyası") ile en çok 32 ön ayar tanımlanır, **Sil** seçiliyi kaldırır; ad 1–60 karakter, süre 500–30000 ms, isteğe bağlı "SYNC beatmatch" (bindirme ölçü başında başlar; BPM parça analizinden gelir, başka ayarı yoktur). Kayıt, sayfada ilk değişiklik yapılınca başlar; dokunulmamış rotasyonların ayarları değişmez.

**Geçiş türü (normal form, 18.10).** Seçili ön ayarın ilk alanı **Geçiş türü**dür ve sonraki alanları o açar: **Crossfade** yalnız süre, SYNC ve Ses/Bas eğrilerini gösterir; **Efektli geçiş** Efekt satırını (efekt, Derinlik, Çıkan/Giren parçaya, BPM) ve Efekt eğrisini; **Filtreli geçiş** Çıkan/Giren filtre satırını ve Ton eğrisini; **Efektli ve filtreli geçiş** ikisini birden. Tür, ön ayarın yapısından türetilir (`radios_transition_preset_type`: efekt var mı, filtre var mı), kayıtlı sözleşmeye yeni alan eklenmez; tür değiştirmek ön ayarı o yapıya normalleştirir (`radios_transition_preset_set_type`: efekt kazanılınca Echo · derinlik 50, filtre kazanılınca çıkan tarafta kapanan alçak geçiren; kaybedilince "none"). Efekt satırındaki **Efekt yok** ve her iki tarafta **Filtre yok** türü kendiliğinden geri düşürür; böylece gizli ama etkin bir aşama kalmaz. "Efekt yok" iken Derinlik/BPM alanları görünmez ve motor onları okumaz (`effect-none-depth-tempo-inert` vakası).

**Tasarım.** Çizim alanı üstte çıkan, altta giren parçayı stilize dalga biçimiyle gösterir; mavi eğri ses seviyesi, sarı eğri bas (200 Hz rafı, 1 düz, 0 −30 dB), turuncu eğri filtre kapanışı (seçilen türe göre alçak geçiren 18 kHz→300 Hz ya da yüksek geçiren 30 Hz→8 kHz), yeşil eğri efekt karışımıdır. Her eğri 2–8 noktalık kırık çizgidir: nokta sürüklenir, boş yere çift tıklayınca nokta eklenir, sağ tıkla silinir; aynı zamanda iki nokta basamak (ani kesme) yapar. Yan oklar seçili eğrinin ara noktalarını başa/sona yaklaştırır. **Efekt** listesi mikserin 14 efektini (Delay, Echo, Spiral, Reverb, Trans, Filter, Flanger, Phaser, Robot, Melodic, Slip Roll, Roll, Rev Roll, MIDI LFO) sunar; derinlik, hedef taraf (çıkan/giren) ve tempo seçilir. Delay/Echo/Spiral/Reverb kuru sese eklenir, diğerleri kuru/ıslak karıştırılır. **▶ Oynat** eğrileri gerçek süresinde görsel olarak oynatır; ses çıkışı yoktur.

**Atama ve rotasyon.** Müzik, reklam, jingle ve sweeper satırlarında kullanılacak ön ayarlar işaretlenir; **Sırayla** dönüşümlü, **Rastgele** parça çiftine göre tekrarlanabilir seçimdir (sıra seçimi yalnız iki ve daha çok ön ayar işaretliyken görünür). Bir türe ön ayar atanınca "AutoDJ: geçiş ve kanallar" sayfasında o türün geçiş türü/süre/SYNC olasılığı alanları pasifleşir ve "Ön ayar kullanılıyor" notu görünür; kanallar orada kalır. Ön ayarsız bir türün yanında o sayfadaki kuralı (`kind_transitions`, ör. "SYNC beatmatch · 8000 ms") salt okunur görünür; kuralın tek evi o sayfadır ve **Türler** başlığındaki **AutoDJ: geçiş ve kanallar…** düğmesi oraya, **Olasılık ve geçişler…** düğmesi müzik→müzik olasılık penceresine götürür (istasyon geneli Ayarlar › Auto DJ kuralı üçüncü sıradaki yedek olarak kendi sayfasında kalır). Ön ayarın süresi bindirme, beat sync seçeneği ise ölçü hizası olur.

**Programlama.** Zaman dilimi satırı ad, günler (Pzt–Paz), takvim saati aralığı, içerik türü (ya da tüm içerik), ön ayarlar ve sıra taşır. Sınır anı çıkan parçanın çıkış noktasıdır; türüne uyan ilk dilim tür atamasının yerine geçer. Bitişi başlangıçtan küçük dilim gece yarısını aşar. En çok 32 dilim tanımlanır.

Kalıcı sözleşme rotasyon ayarlarındadır (`rotation_library[].settings`): `autodj_presets` dizisi (`id`, `name`, `duration_ms`, `beat_sync`, `out`/`in` {`volume`, `bass`, `tone`, `tone_kind`}, `effect` {`type`, `depth`, `side`, `mix`, `bpm`}; eğriler `[[t, v], …]`) ve `autodj_automation` nesnesi (`enabled`, `kinds.<tür>` {`presets`, `rotation`}, `schedule[]`). C planlayıcı (`src/core/automation_calendar_plan.c`) ve veri servisi (`desktop_automation_calendar.py`) aynı kuralları uygular; playlist üstüne yazımında bu alanlar reddedilir. Seçilen ön ayar `calendar_transition.shape` ipucuyla native planlayıcıya, oradan `RadioSAudioRunItem.shape` olarak motora gider. Motor bindirme boyunca eşit güç/ducking zarfı yerine tasarlanmış ses eğrilerini, raf/filtre ve efekt sesini gerçek PCM'de uygular; eğri ve efekt durumu her geçişte sıfırlanır. Bindirmesiz sınır, tek oynatıcılı native transport ve olasılık kuralı seçimi şekli düşürür. Yeni ayarlar veri servisi yeniden başlayınca kabul edilir; başlatıcı arka uç özeti değişince servisi kendisi yeniden başlatır.

Kabul: `transition` (eğri/şablon doğrulaması, gerçek frekans yanıtı, şekilli önizleme PCM'si; 18.10: `designed-type-normal-form` tür türetme/normalleştirme, `effect-none-depth-tempo-inert` "Efekt yok" iken derinlik/tempo örnek örnek etkisiz), `runner` (bindirmede tasarlanmış zarf, bindirmesiz sınırda ret), `automation_calendar_plan` (sıralı/rastgele rotasyon, zaman dilimi önceliği, geçersiz ön ayar reddi), `playout` (şekil ipucu → runner öğesi, görünümde ön ayar adı, tek oynatıcıda düşme, geçersiz şekil reddi), `rotation_editor_regressions` (sayfa açılışı, etkinleştirme, atama, pasifleşen tür alanları, zaman dilimi, boş ad reddi, yeniden yükleme; 18.10: `automation-page-type-drives-fields`, `automation-page-type-round-trip`, `automation-page-kind-rule-links`) ve `desktop_automation_calendar` (kalıcılık ve ret listesi) senaryoları eklendi.

## Şema: live event, iç sıra, bağlantılar ve programa giriş (kılavuz 18.10.2026)

**Yayın adımları.** Rotasyon düzenleyicide **Playlist ya da live event ekle…** seçicisi playlist ekler; **Şemadan yükle…** kayıtlı bir şemanın adımlarını yeni kimliklerle getirir; **Live event ekle…** konuşma/DJ/müzik adımı açar (ad, etiket, yayına verilecek mixer kanalları, live event şeması, bitiş kuralı "Süre ile" ya da "Elle", süre). Her adımın isteğe bağlı başlangıç/bitiş tarihi vardır; program tarihi bu aralığın dışındaysa adım o programda kullanılmaz.

**Şema Ayarları.** Takvimdeki **Şema Ayarları** penceresi üç sekmedir. *İç sıra ve türler*: şemanın ve her playlistin iç sıra kayıtları (numara, beklenen tür, atanan otomasyon). A'dan B'ye geçerken şema, A ve B'de aynı numarada ve sınırın türüne uyan ilk kaydın otomasyonu uygulanır ("2 2 2"). *Adımlar arası geçiş*: ortak kayıt yoksa A → B bağlantısına atanmış geçiş. Bir ön ayar AutoDJ otomasyonu sayfasında türlere atanmışsa yalnız o türlere giren bağlantılarda listelenir ve uygulanır; hiçbir türe atanmamış ön ayar her bağlantıda kullanılabilir. *Programdan programa*: bu şemayla çalışan programa hangi adımdan ve hangi geçişle girileceği, olasılıklı alternatif girişlerle. Oynatıcı seçimi "N oynatıcı seçili ▾" açılır penceresindedir: kümeler başlıklarıyla (başlık kümenin tamamını seçer), altında Kümesizler ve seçili olup şu an olmayan oynatıcılar.

**Program oluştur › Live event.** Kutu işaretlenince canlı yayın ayarları pencerenin sağında açılır (etiket, mixer kanalları, live event şeması, bitiş kuralı, süre) ve programın rotasyon listesi yalnız şemaları gösterir. Planlayıcı live adımını yayın sırasına `kind:"live"` blok olarak koyar. Adımdan önceki parça adıma düz biter. "Süre ile" kuralında adım, önceki parçanın **gerçekten bittiği** andan itibaren belirtilen süre kadar sürer, sonra yayın kendiliğinden sonraki adımla devam eder; parça duraklatılsa ya da geç bitse de adım kısalmaz, ▶| adımı erken bitirir. "Elle" kuralında yayın operatörün ▶| komutunu bekler. Kanalın DJ/Konuşma/Müzik etiketi, işaretli kanalların hepsi aynı etiketi taşıyorsa adımın etiketini belirler.

**Mevcut şemayı uygula.** Takvimde seçili şemayı çalan genel programa uygular: çalan öğe kesilmez, kalan yayın sırası şemayla yeniden hazırlanıp geçişe uygun noktada değiştirilir; kayıtlı takvim değişmez. Çalan genel program yoksa durum satırı "Çalan genel program yok." der.

Sözleşme ve izlenebilirlik: [kilavuz-2026-10-18-4c/sozlesme.md](kilavuz-2026-10-18-4c/sozlesme.md), [kilavuz-2026-10-18-4c.md](kilavuz-2026-10-18-4c.md).

## C planlayıcı ve motor

`src/playout/planner.c`, kimlikli hazırlık yanıtından sınırlı bir plan üretir. Dosya yolu, istasyon, medya/sürüm/dosya UUID'si, SHA, analiz süresi ve cue bağı doğrulanır. Yönetilen kök dışındaki dosyalar kullanılmaz. Yuvarlanmış katalog süresi gerçek analiz kare sayısının yerine geçirilmez.

Kaynak seçimi döngü, ağırlıklı seçim veya kaynağın uygun benzersiz parçalarını tamamlama ile yürür. Parça sırası, deterministik rastgele seçim ve en az planlanmış seçim uygulanır. Sanatçı/albüm/parça tekrar aralığı gerçek gelen parçanın bindirme/cue başlangıcında denetlenir. Müzik için zorunlu metadata seçeneği, sanatçısı boş jingle'ı yanlışlıkla müzik saymaz. Geçişte cue işaretleri saklanır; kaynak sınırında cue'ya dönüldüğünde gerçek auto-next/fade aralığı kullanılır.

Rotasyon checkpoint'i kaynak sırası, kaynak/parça cursor'ları, RNG, tamamlanan benzersiz kayıtlar ve jingle fazını içerir. Preview veya önceden hazırlanmış parçalar bunu kalıcı ilerletmez. MAIN'de gerçek tüketim alındığında ilgili checkpoint CAS ile kaydedilir; yeniden başlatma başarıyla kaydedilmiş seçimden devam edebilir. Kaynak listesi veya playlist sürümü değişirse checkpoint yeniden oluşturulur. Yazım arka plandadır; kapanış/çökme sırasında henüz tamamlanmamış son yazıyı dayanıklı kurtarma aşama 13 kapsamındadır. Kesinti/çatışmada oturumun gerçek geçmişi bellekte korunur ve kayıt hatası gösterilir.

`src/audio/engine.c` içindeki yürütücü doğrulanmış sırayı sahiplenir, decoder'ı önceden hazırlar ve fade/ducking/overlap'ı gerçek PCM üzerinde uygular. Canlı EQ, mixer ve gain çalışmaya devam eder. Boş, MAIN'e yönlendirilmiş normal oynatıcılar kullanılır; elde yüklenmiş oynatıcı ele geçirilmez. Ön dinleme/sampler sesleri bu havuza alınmaz. Operatörün sahiplenilmiş oynatıcıya yeni yüklemesi, transport veya yönlendirme değişikliği otomasyonun ilgili sahipliğini iptal eder.

MAIN tüketim alındıları arayüzün 50 ms güncellemesinden bağımsız saklanır; kısa jingle'lar ve emekliye ayrılan oynatıcılar geçmişte kaybolmaz. Önceki parçanın yeniden çalınması aynı sıra indeksinde ayrı gerçek çalma olarak sayılır; yalnız pause/resume yeni kayıt üretmez. En yeni 2.000 tüketim olayı sınırlı motor belleğinde korunur. Pulse zaman bilgisi gerçek sunucu tüketiminden güncellenir. UTC kapısı erken PCM göndermez; fiziksel duyulma ses aygıtının tampon gecikmesini içerir.

Sonraki otomasyon planı mevcut sıranın sonuna gelmeden hazırlanıp sabit indeks/epoch korunarak eklenir. Bir çalıştırma en çok 500 giriş taşır. Bu sınıra gelince önceden hazırlanan devam planı mevcut MAIN kuyruğu gerçekten bittikten sonra yeni çalıştırmaya alınır; decoder hazırlığı kadar boşluk oluşabilir. Tek oynatıcılı EOF/cue/operatör geçişi de yeniden yükleme beklemesini açıkça gösterir. Gerçek bindirme için iki ayrı fiziksel oynatıcı gerekir. Disk/decoder/backend arızasında uygun olmayan dosya atlanarak başarı gösterilmez; mevcut doğrulanmış sıra tamamlanır ve devam hatası görünür.

## Kalıcı sözleşme ve kaynaklar

`POST /v1/workflows/desktop-playout/prepare`, playlist/program/otomasyon için gerçek metadata ve doğrulanmış medya bağlarını salt okunur döndürür. Geçerli istasyon ve `view` yetkisi gerekir; en çok 2.000 aday ve 1,5 MB yanıt kabul edilir. Waveform bu yanıtı şişirmez. Typed `playout_history` kaydı en çok 2.000 gerçek yerel çalma gözlemi ve isteğe bağlı rotasyon checkpoint'i taşır. Güncellemeler mevcut edit/yetki/revision/CAS/alındı kurallarını kullanır. Legacy bağlanmamış playlist ve eski geçmiş belgeleri geçerliliğini korur.

Ana kaynaklar: `src/playout/planner.c`, `src/ui/playout_ui.c`, `src/ui/playlist.c`, `src/ui/broadcast.c`, `src/ui/automation.c`, `src/audio/engine.c`, `src/audio/pulse_backend.c`; ortak sözleşmeler `include/radios/playout.h`, `playout_ui.h`, `audio_runner.h`; veri hazırlığı `database/SDBASE/src/sdbase/desktop_playout.py`.

## Kabul

`playout`, deterministik kaynak/rotasyon, tekrar aralığı, gerçek cue zamanlaması, takvim, checkpoint ve güvenli kimlik/yol denetimini sınar. `runner`, gerçek örnekler üzerinde otomatik geçiş/ducking, transport, saat kapısı, sahiplik, append, kısa parça tüketim alındısı ve hata/kapanış davranışını sınar. `desktop_playout`, gerçek servis/DB ile hazırlık, auth/istasyon/kimlik/SHA, sınırlar ve kalıcı geçmiş/checkpoint CAS davranışını denetler.

`native_playout`, ayrı gerçek SDBASE servisinde native playlist→program→PCM, transport/manual oynatıcı koruma, kaydedilmemiş otomasyon önizlemesi/gerçek geçmiş/yeniden açılış, UTC takvimi/Stop ve stale medya/backend/kapanış/CAS reddini sınar. Fixture WAV'lar 288.031 gerçek kare taşır; yuvarlanmış katalog süresi 6.001 ms'dir. Testler üretim DB'sine fixture yazmaz; gerçek Pulse ölçümleri ayrı sanal MAIN/CUE/MON cihazlarında yapılır.

Son hedefli kabul: strict C11 build; C planlayıcı **17/17**; C yürütücü **15 deterministik + 1 gerçek Pulse**; backend hazırlık/geçmiş **13/13** ve mevcut typed API **18/18** başarılıdır. Planlayıcı/yürütücü ve önceki audio/DSP/transition kapsamı ASan/UBSan ve leak detection açıkken temizdir.

Birleşik son CTest **26/26**, **226,46 saniye** başarılıdır; önceki veri/medya/ses/DJ hedefleri ve native pencere kabulü birlikte geçti. Kaynak/API envanteri **184 workflow / 234 yol / 239 HTTP işlemi / 169 backend kaynak dosyası** ile doğrulandı.

Native kabul **5/5**, normal **48,72 saniye**, ASan/UBSan **64,47 saniye** başarılıdır. GTK global verileri için leak detection kapalıdır. Tek müziklik partilerde gerçek A→jingle→eklenen B geçişindeki en uzun sessizlik normalde **1 kare**, sanitizer koşusunda **624 kare / 13 ms** ölçüldü; kabul sınırı 100 ms'dir. Son kaynaklar yeniden derlenerek otomasyon sanitizer grubu ayrıca başarılı oldu (**23,38 saniye**, en uzun sessizlik 1 kare). PREVIOUS sonrası A'nın iki/B'nin bir gerçek çalma kaydı, duraklat/devamın ek kayıt üretmemesi, kalıcı checkpoint ve kapalı yeniden açılış doğrulandı. UTC başlangıcından yaklaşık 99 ms önce MAIN peak **0** ve bekleyen takvimi iptalden sonra sessizlik kabul edildi.

Güncel servis ve **0.10.0** C penceresi mevcut `data/database/RadioS.sdb` ile açıldı. Gerçek depo/istasyon/uç UUID'leri, şema 4, sequence **2** ve bütünlük **ok** korundu; üretim koleksiyonlarına test kaydı eklenmedi. Güncel prepare API'si salt okunur boş programla **HTTP 200 / contract 1 / local_draft** döndürdü. Kimlik dosyası 0600; yalnız bir normal RadioS penceresi ve kendi MAIN akışı açık, fixture sanal modülleri temizdir.
