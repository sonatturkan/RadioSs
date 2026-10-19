# Otomasyon: sözlük, öncelik kuralları ve kabul ölçütleri

15.10.2026 · radyo.pdf değerlendirmesindeki (masaüstü, `radyo-pdf-degerlendirmesi-2026-10-14.md`) "belgeye eklenmesini önerdiklerim" maddelerinin projedeki karşılığı.

## Sözlük

| Terim | Anlamı | Nerede |
|---|---|---|
| Program | Takvimdeki adlı yayın bloğu (ör. "Sabah Kuşağı"). Adı listeden seçilir; takvimde aynı adlı bir blok varsa rotasyonları ve olasılık ayarları yeni bloğa kopyalanır. | Otomasyon › takvim › "Program oluştur" |
| Haftalık program | Takvimin tamamının kayıtlı kopyası (Program sekmesi). Oynatıcıya "Haftalık program yükle…" ile yüklenir. | Program sekmesi |
| Rotasyon | Playlistlerden oluşan kaynak grubu ve onların çalma/geçiş kuralları. Takvim saatinden bağımsızdır, programa atanır. | "Bağımsız rotasyon oluştur" |
| Rotasyon şeması (otomasyon şeması) | Playlist/live event adımlarının türlerini, iç sıra eşleşmesini, oynatıcı kümelerini ve adımlar arasına atanmış otomasyonları tanımlar. Program yayın zamanını ve dış sırayı, AutoDJ atanmış geçişlerin yürütülmesini taşır. Şema bunlardan ayrı düzenlenir; kayıtlı şemalar listede "· Şema" ile görünür. | "Şema Ayarları" |
| Olmayan türler | Rotasyon/şema tür temelli tanımlanır: Rotasyon düzenleyici › Tekrar kuralları › "Olmayan türler" satırındaki Müzik · Reklam · Jingle · Sweeper kutularından işaretlenen tür o rotasyondan hiç çalmaz (kaynak playlistlerde olsa bile; haber gibi diğer türler müzik sayılır). Jingle işaretliyse rotasyonun şarkı içi jingle katmanları da uygulanmaz. Kayıtta `settings.excluded_kinds`; listelerde ve oynatıcının şema yükleme penceresinde "· yok: Reklam, Sweeper" ile görünür. Bütün çalınabilir medya olmayan türlerdeyse yayın başlamaz ve nedeni yazılır. | Rotasyon düzenleyici |
| Playlist | Şarkıların sıralı listesi; türü (müzik, reklam, jingle, sweeper…) rotasyon tablosunda görünür. | Playlist sekmesi |
| Küme | Birlikte AutoDJ yapan oynatıcılar. Kümesiz oynatıcı tek başına çalar. Playlistin kümesi yoktur. | Canlı oynatıcılar |

## Seçim önceliği (planlayıcıyla aynı sıra)

1. **Ağırlık 0** olan playlist ya da şarkı hiç seçilmez.
2. **Olasılık yüzdesi** (Olasılık ve geçişler penceresi): yüzde verilen öğe o olasılıkla seçilir; yüzde verilmeyenler ve "Kalanlar" satırı kalan payı sırayla paylaşır. Bir listede toplam en çok %100'dür.
3. Yüzde yoksa **ağırlık**: ağırlıklar eşitse playlistler sırayla, farklıysa ağırlık oranında seçilir. Bir ağırlık değiştirilince bu kip kendiliğinden güncellenir (gizli "Playlistler arası seçim" seçicisi kaldırıldı).
4. Hiçbiri yoksa **sırayla** (doğrusal).

Yüzdeli **playlist ve şarkı** çekilişinde, çekilen payın sahibi o an çalınamıyorsa (tekrar aralığı, süre, "jingle'dan sonra jingle olmaz" vb.) yayın durmaz: o payı çalınabilen yüzdeli öğeler oranlarıyla paylaşır; hiçbiri yoksa yüzdesiz ilk çalınabilir öğe seçilir. Açıkça %0 verilen öğe ve istenmeyen saatler yine seçilmez. **Jingle ve geçiş** yüzdeleri bundan ayrıdır: toplam %100'ün altındaysa kalan pay "hiçbiri" olarak kalır.

Kapsamlar: program › rotasyon › playlist › şarkı. Şarkı yüzdeleri rotasyon düzeyinde tutulur (playlistin kendi penceresi yalnız o playliste özel geçiş ve jingle içindir; aksi hâlde planlayıcıda rotasyonun playlist yüzdelerini ezerdi). Olasılık penceresinde hiçbir şey kalmazsa politika tamamen kaldırılır ve 3./4. madde geçerli olur.

Eleme çekilişten önce yapılır: saat penceresi, tekrar/sanatçı ayrımı, doğrulanmış medya ve telif. Hiç uygun aday kalmazsa sıradaki uygun şarkı sırayla seçilir (ölü hava yerine).

## Geçiş önceliği

1. Olasılıklı geçiş (Olasılık ve geçişler › "Geçiş türü ve olasılığı") yalnız **müzik → müzik** arasında ve geçiş o parçaya sığıyorsa denenir.
2. Çekilişte bir geçiş seçilmezse (kalan pay, tanımsız ya da uygun değil) parça **rotasyonun geçiş kurallarıyla** biter; artık zorla "Parça sonunda" yapılmaz.
3. Rotasyonda kural yoksa içerik türüne göre AutoDJ kuralı (Ayarlar › Auto DJ) uygulanır.

"Erken geçiş (sn)" parçanın **çıkış işaretinden** (AutoDJ çıkış noktası, yoksa otomatik cue çıkışı, yoksa cue-out) geriye sayılır.

## Şarkı içinde jingle

- "Başlangıçtan sonra X sn": jingle vokal başlamadan (intro işareti ya da intro_end/vocal_start cue) bitmelidir.
- "Bitişten önce X sn": parçanın çıkış noktasından geriye sayılır; onaylı outro_start cue'sundan önce başlayamaz.
- Gerekli işaret yoksa ya da jingle sığmıyorsa **o parçada jingle atlanır**; müzik normal çalar.
- Jingle oynatıcısı dolu, CUE'da ya da MAIN'e bağlı değilse jingle atlanır, otomasyon durmaz. Müzik kısma varsayılanı -12 dB.

## Saatler

"İstenmeyen saatler" istasyon saatiyle girilir (Türkiye için Europe/Istanbul). Kural kendi saat dilimini taşır; eski kayıtlardaki UTC aralıkları ekranda istasyon saatine çevrilerek gösterilir.

## Oynatıcı kartı

Bir oynatıcının otomasyon kaynakları tek **"Otomasyon ▾"** düğmesindedir: Rotasyon yükle… · Otomasyon şeması yükle… · Haftalık program yükle… · Bu playlistten şema oluştur…. Yanındaki satır yüklü olanı ya da sorunu düz Türkçe yazar ("⚠ …"); ayrıntı penceresine bakmak gerekmez. "Tekrarla" ve "Sıradaki program" yalnız program yüklüyken görünür. Ses motorunun hataları Türkçe ve ne yapılacağını söyleyen cümlelerdir.

## Örnek

Rotasyon: Pop %60, Slow %20, Rock ve Jazz yüzdesiz. Pop içinde X şarkısı %10.
- Playlist seçimi: Pop %60, Slow %20; kalan %20 Rock ve Jazz arasında sırayla.
- Pop seçildiğinde: X %10; kalan %90 Pop'un diğer şarkıları arasında sırayla.
- Geçiş: Crossfade %40 (3 sn), Efektli inme %20; kalan %40'ta rotasyonun geçiş kuralı uygulanır.

## Kabul ölçütleri

- Olasılık penceresinde %101 ya da toplamı %100'ü aşan değer reddedilir, kayda gitmez ve neden yazılır.
- Pencere boşaltılınca rotasyon kaydında `probability_policy` kalmaz; Pay sütunu yeniden ağırlık/sıra gösterir.
- Ağırlıkları farklı bir rotasyonda Pay sütunu yüzde gösterir; eşitse "Sırayla".
- Takvimde bir program adı listeden seçilince yeni blok aynı rotasyonlarla oluşur; atama kimlikleri yenidir.
- İstenmeyen saat 02:00-03:00 (İstanbul) olan kural 23:30 UTC'de seçilmez, 02:30 UTC'de seçilebilir.
- Jingle oynatıcısı doluyken rotasyon başlatılınca müzik çalar, jingle atlanır; İngilizce hata görülmez.
- Çekiliş boş dönen bir parça rotasyonun geçiş kuralıyla biter.
- Playlistler yüzdeliyken biri (ör. az önce çalan sweeper) çalınamazsa seçim kesilmez: 6 saatlik bir kuşağın tamamı (%96'dan fazlası) dolar; Yarı AutoDJ'de de aynı. Gerçek 7/24 programı verisinde 7 kuşak × 2 kip × 8 tohumla doğrulandı (önceden Yarı AutoDJ seçimi 07:00 kuşağında 10:30 civarında bitiyordu).
- Yarı AutoDJ'de (tek oynatıcı) son parça, yalnız rotasyonun kendi **soft_end_seconds** değeri kadar kuşak sonunu aşabilir; değer yoksa parçalar kuşağa tam sığar.
- "Olmayan türler"de Reklam ve Sweeper işaretli bir şema, kaynak playlistlerinde reklam/sweeper olsa da hiç reklam/sweeper çalmaz; Jingle işaretliyse şarkı içi jingle katmanları da çıkar. `excluded_kinds` yalnız rotasyon düzeyinde, bilinen dört türden ve tekrarsız kabul edilir; playlist ezmesinde ve bilinmeyen değerde kayda gitmez (C planlayıcı ve arka uç aynı kural). Bütün medyası olmayan türlerde kalan rotasyon "Olmayan türler'den en az bir türü açın" diyerek reddedilir.

## Bilinen sınırlar

- "Efektli inme" yalnız alçak geçiren filtreyle kısmadır; yankı gibi başka efekt yoktur.
- Bir playlistin kendi olasılık penceresinde şarkı yüzdesi verilmez (yukarıdaki kapsam notu).
