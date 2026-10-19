# Native GUI işlev kapanış denetimi

2026-10-06; güncel kaynak sürümü 0.17.0. İlk ayrıntılı kontrolün 0.16.0
kaynaklarındaki 10:30 UTC katalog
açıkları bu çalışma sırasında ayrıca kapatıldı; güncel durum aşağıdadır. İlk
denetimin eksik bağlantıları ile daha sonra uygulanan bağlantılar ayrı kaydedilir.
Bu denetim, `01-islev-haritasi.md`,
`islev-envanteri.json` ve etkin C callback'lerinin karşılaştırılmasıdır. Referans
HTML'deki kontrol sayısı ve geçen test sayısı, bütün işlevlerin uygulandığı
anlamına gelmez. İlk denetimdeki satır numaraları başlangıç kanıtıdır; kapanan
bağlantılar aşağıda ayrıca belirtilir.

## Güncel kalan açıklar

| Kontrol / işlev | Kaynak kanıtı | Gerçek davranış / gereken kapanış |
|---|---|---|
| Acil duyuruyu yayına gönder / TTS | `src/ui/emergency.c:853`, `:696`, `:711` | Yayına gönder düğmesi koşulsuz kapalı. Metin, dil, bölge, tekrar ve dosya alanları yalnız hazırlık belgesine/satırına kaydediliyor. Gerçek alarm yaşam döngüsü, süre/yetki kontrolü, ses/TTS üretimi ve hedef çıkışa yürütme gerekiyor; hazırlık kaydı başarılı olsa da bunlar yürütülmüş sayılmaz. |
| USB anahtarı ekle/iptal, fiziksel kilit | `src/ui/emergency.c:483`, `:696`, `:871`, `:2079` | Anahtar etiketi/sahibi hazırlık listesine eklenir; iptal hazırlık satırını değiştirir. USB keşfi, güvenli anahtar doğrulama ve motor devralma yetkisine bağlama yok. Backend kimlik/yetki kararını fiziksel credential sağlayıcısıyla bağlamak gerekiyor. |
| MIDI denetleyici doğrulama / harici bağlantı ve geri dönüş sınaması | `src/ui/technical.c:786`, `:790`, `:805`, `:806` | Bu düğmeler kapalı, denetleyici/gerçek bağlantı sağlayıcısı yok. Native ses routing testi ayrı modülde var; bunu MIDI/GPIO veya dış yedek bağlantı kanıtı saymak doğru değildir. Gerçek sağlayıcı veya açık kapsam sınırı gerekir. |
| Dış web uygulamasının kimlik/oturum/istek/reklam işlevleri | `src/ui/technical.c`, `src/ui/external_access_ui.c` | Native profil kaydı, dış panel açılması ve TLS/HTTP sınaması gerçektir. Ayrı sitenin auth/session, arama, dinleyici istek formu ve reklam teslim modülleri için dış sağlayıcı gerekir. Native HTTPS yanıtı bunların uygulanmış olduğuna dair kanıt sayılmaz. |

## Açık sayılmaması gereken metin eşleşmeleri

- `dj.c:579`, `:591` sampler taslağı ve `dj.c:262` DSP uyarısı yalnız
  `ctx->audio == NULL` dalındadır. Canlı ses motorunda sampler katalog seçimi ve
  DSP callback'leri bağlıdır; tek başına bu metinler üretim stub'ı göstermez.
- `header.c:402`, `:435` motor bağlantısı bulunmayan fallback durumudur. Gerçek
  monitor/CUE yolu `audio_ui.c` ve routing modüllerinde bulunur.
- `data.c`, `widgets.c`, `advert.c`, `media.c`, `editors.c` içindeki bağlantı
  bulunamama mesajları hata yönetimidir. Bir kaydın başarıyla kaydedildiğini
  göstermemeleri doğru davranıştır.
- `broadcast.c` üzerindeki eski “sonraki aşama” mesajı tek başına tüm katalog
  işlevlerinin eksik olduğunun kanıtı değildir: medya binding'i, katalog arama
  ve playlist-to-programme aktarımı vardır. Güncel katalog ekle/değiştir
  callback'i ve gerçek gömülü Playlist bu çalışmada ayrıca bağlandı.
- Acil hazırlık, handover, iletişim ve tatbikat alanları DB'ye kalıcı hazırlık
  kaydı olarak yazılır. Bu kayıtlar fiziksel devralma veya USB/TTS yürütümü
  kanıtı değildir; kalıcı CRUD'nin bulunması ise gerçek uygulanan bir işlevdir.

## Denetimde kapanan bağlantılar

- Rekordbox import düğmesi canlı DB'de açıldı. Native C dialog, düzenli XML
  dosyasının en fazla 1 MiB kopyasını worker'da alır; sunucu preview'su gerçek
  kapsamı ve katalog kararlarını döndürür. Operatörün seçtiği liste yolları,
  append/copy, skip/keep ve üç cue politikası yeniden önizlenir. Apply, bu hash
  ve sabit idempotency kimliğiyle katalog, import kaynak bilgileri/DJ analysis,
  native playlistler, import job ve replay receipt'i tek transaction'da yazar.
  XML ses dosyası varlığını/onayını üretmez. `desktop_rekordbox.py` ve
  `playlist.c` etkin kaynaklardır; eski profil tabanlı importer API'leri korunur.
- Kaynak TrackID, cue sayısı/politikası ve kaynak SHA artık playlist ayrıntısında
  görünür. Native metadata düzenlemesi kaynak aidiyetini korur; Location'ı
  olmayan parçanın sonraki XML aktarımında tekrar atlama çalışmaya devam eder.
- İlk kontrolde Yayın katalog ekle/değiştir düğmeleri koşulsuz kapalıydı,
  program saklama ve gömülü Playlist paneli yalnız mesaj/taslak gösteriyordu.
  Güncel `broadcast.c` callback'i gerçek `desktop-media/get` ile dosyayı yeniden
  hash'ler; medya/version UUID, revision, SHA ve analysis aidiyetini kontrol
  eder. Yakalanan hedef programme UUID/revision/config/event UUID,
  katalog nesli/seçimi ve Store/kullanıcı/istasyon değişirse hiçbir öğe yazmaz.
  Ekle seçili öğenin sonrasına, seçim yoksa program sonuna ekler; değiştir
  hedef event UUID'sini, saatini ve notunu korur. Belge tek CAS kaydıyla
  gönderilir; yayın başlatılmaz.
- “Mevcut programı sakla” gerçekten adlandırılmış `programme` taslak kopyası
  yazar; recorded audio veya onay üretmez. UUID/revision etiketli native liste
  bu kopyayı tekrar okuyup açık onayla Mevcut program taslağına yükler. Kayıp
  yanıtta aynı kayıt UUID'si/body/key korunur; başka kullanıcı/Store/istasyon
  kimliğiyle tekrar gönderilmez. Gönderilmiş kayıtta “Kapat” geri alma anlamına
  gelmez ve bu durum ekranda açıklanır. Gömülü Playlist artık aynı gerçek
  `radios_playlist_new(ctx)` bileşenidir; ayrı sahte CRUD kontrolleri yoktur.
- İlk denetimde geçici acil sıra aktarımı INI dışa aktarmaya bağlıydı. Güncel
  `emergency.c:1883`–`:2000` akışı hedefi Mevcut programın sonu olarak gösterir;
  kayıtlı kaynak sırayı `desktop-broadcast/emergency-prepare` ile doğrular ve
  gerçek programme belgesine ekler. Kaynak/ hedef revizyonları, kaynak config,
  kullanıcı/istasyon/Store kimliği ve dialog intent değişmişse hiçbir öğe
  eklemez. Başarı mesajı DB kayıt onayını beklediğini açıkça söyler; ses
  oynatma başlamaz. Ayrı “İşlem taslağını indir” INI export olarak kalır.
- `programme_lifecycle.c` ve `desktop_programme.py`, native belgeyi formal
  taslak plana gönderir; ön koşul inceleme, içerik/istasyon/hak onayları, plan
  onayı, yeni taslak fork'u ve endpoint dağıtımı ayrı gerçek işlemlerdir.
  Yerel “Onaylı sırayı yerel çal” onaylı snapshot'ın sırayla operatör başlatması
  kapsamındadır; planın kesin saatli endpoint yürütümü veya teslim kanıtı
  değildir. Kaynak eşleşmesi, `activate` ve `operate`, güncel onaylı revision,
  tek aktif native medya binding'i, SHA ve frozen marker'lar denetlenir.
  Üretim yerleşimi/ad/non-audio/geçiş/fade anlamını taşımayan akış reddedilir.
  Konfigürasyonun cut/finish timing'i formal gönderimde açıkça reddedilir.
  UTC slot istasyonun başka hizmet tarihine düşerse, event cut/finish/bekleme
  anlamı veya çalmayı değiştiren özel native marker varsa sessizce dönüştürülmez.
  Yerel onaylı sıra en fazla 500 öğedir. Ayrıntılar
  [program onayı ve haklar belgesindedir](program-onayi-ve-haklar.md).
- Teknik web özeti `external_access_ui.c` üzerinden form değişiklikleri ve
  yüklenen belgeyle güncellenir. Paneli açma gerçek dış tarayıcıya bağlandı;
  HTTPS sınaması kontrollü worker'da TLS ve HTTP yanıtını ölçer, iptal ve eski
  yanıt koruması vardır. Dış panelin oturum/kimlik sağlayıcısı, site araması,
  istek formu ve reklam teslimi hâlâ dış sağlayıcı gerektirir; HTTPS yanıtı bu
  işlevlerin çalıştığının kanıtı değildir.

## Kabul kanıtı

`desktop_rekordbox`: özel gerçek Store ve authenticated HTTP ile 10 senaryo;
seçili kapsam, read-only preview, append/copy, skip/keep, koleksiyon,
cue preserve/review/ignore, atomic rollback, stale preview, yeniden açılışta
exact receipt replay, ACL, aynı library/TrackID'nin istasyon ayrımı, boyut/
üyelik/sayısal sınırları ve Location olmadan edit sonrası tekrar denetimi.
Orijinal importer'ın preview/apply çalışması da korunmuş API ile sınanır.

`native_rekordbox`: özel gerçek HTTP/DB ile 2 GTK senaryosu; seçili liste,
copy/keep/cue politikası, yeniden açılış, commit sonrası düşen yanıtın aynı
kimlikle tekrar alınması, gerçek stale-preview reddi, dialog kapanışı ve
file-worker sürerken pencere kapanışı. Test kapıları yalnız build altındaki
özel backend kopyasında bulunur. `recovery` testinde XML snapshot, preview
hash, kapsam ve politikanın exact body ile process reopen sonrasında kalması
ve aynı anahtarda değiştirilmiş politikanın reddi ayrıca sınanır.

`native_broadcast_catalogue`: özel gerçek HTTP/DB ile 2 GTK senaryosu; hedef
sonrasına ekle, UUID/saat/not koruyan replace, adlandırılmış programme taslağı
sakla/yükle/reopen, gerçek gömülü Playlist oluşturma, commit sonrası yanıt
kaybında exact receipt replay, başka kullanıcıya gerçek reconnect sırasında
eski retry'ın HTTP'ye gönderilmemesi, kaynak dosya SHA bozulması, seçili hedef
değişmesi, başka writer CAS çatışması ve bekleyen cevapta owner kapanışı.
Test kapıları yalnız build altındaki özel backend kopyasında bulunur.

`desktop_programme`: 12 gerçek backend senaryosu. `native_programme`: bir
birleşik GTK kabul grubu; gerçek ön koşul kararları ve ölçülen PCM, bekleyen
istekte hedef değişimi/kapanış ve yeni taslak fork/resubmit akışını sınar.

Rekordbox için katalog değişikliğinden önceki focused CTest çalışması 4/4 geçti
(`recovery`, `desktop_rekordbox`, `data_playlists`, `native_rekordbox`), 35,64 sn.
Native test, önizlemeden sonra özgün XML dosyası taşınmışken kaydedilmiş
snapshot'ın uygulanmasını da doğrular. Recovery suite toplam 11 gruptur;
Rekordbox snapshot/replay grubu bu suite içindedir. Kanıt dosyası
`build/rekordbox-closure-acceptance.json` kaynak/test SHA özetlerini kaydeder.
Katalog için son focused CTest çalışması 1/1 (2 GTK senaryosu), 7,75 sn geçti;
`build/closure-broadcast-catalogue.json` gerçek test kapsamını ve bu çalışmanın
source/test SHA özetlerini kaydeder. Tam test/sanitizer kabulü üst kapanış
raporunda ayrıca değerlendirilir; bu denetim kendi focused sonuçlarını aktarır.

Son toplu denetimde paylaşılan test servisine arka arkaya açılan pencerelerin
istekleri gerçek HTTP 429 yanıtları oluşturdu. Native istemci yalnız açıkça
denetlenmiş salt okunur çağrıları sunucunun Retry-After süresine uyarak,
iptal edilebilir ve sınırlı şekilde tekrar okur; yazma ve kurtarma işlemleri
otomatik tekrar gönderilmez. Gömülü Playlist nedeniyle iki bileşende bulunan
aynı adlı kontroller, pencere testlerinde görünür `playlist-task-view`
kapsamından seçilir. Bileşenin mevcut kontrol adları korunur.

Bu değişikliklerden sonraki focused çalışma `native_broadcast_catalogue`,
`native_rekordbox`, `native_data` ve `native_media` için 4/4 geçti (56,88 sn).
`native_playout` 5/5 gerçek GLib grubuyla geçti (63,81 sn): varsayılan toplu
runner, grupları özel serviste `-l` ile keşfeder ve her bağımsız gruba ayrı
fixture verir. Grup içindeki reopen/kalıcılık denetimleri aynı DB'de sürer;
açık GLib filtreleri verilirse önceki tek fixture davranışı korunur. Test
hazırlığındaki playlist oluşturma yazısı 429 aldığından bu ayrım gereklidir;
servis kotası ve işlev denetimleri gevşetilmez. Ayrı `native_audio` kanıtında
altı grup aynı fixture içinde iki gerçek 429 okumasından sonra geçti.
Güncel kaynak özetleri ve eski başarısızlığın ayrı kaydı
`build/closure-gui-load-regression.json` içinde bulunur; tam 54 testin son
kabulü üst kapanış raporunun sorumluluğundadır.

Rekordbox, katalog aktarımı, acil programme aktarımı, formal plan ve dış web
bağlantısının yürütme anlamları bağımsız kullanıcı senaryolarıyla değerlendirilir.
Şema/workflow/API paritesinin sağlanması bütün native kullanıcı akışlarının
ve fiziksel USB/TTS/MIDI işlevlerinin tamamlandığının kanıtı değildir.
