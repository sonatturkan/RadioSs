# SDBASE 0.5.0 ile gelen özellikler

Bu belge önceki 0.5.0 eklemelerinin kapsamını korur. Güncel 0.6.0 / şema v3, 95 koleksiyon ve 160 iş akışı/208 HTTP yolu kullanır; yeni sözleşme [marker belgesinde](playback_markers.md), üretim denemeleri [kabul belgesinde](production_acceptance.md) açıklanır.

0.5.0, açık kaynak radyo uygulaması karşılaştırmasında belirlenen içerik yeniden kullanımı, podcast ve dinleyici sayımı eksiklerini RadioS'in kendi `.sdb` motoruna ekler. SQL çalışma bağımlılığı yoktur. Şema v2 içinde **92 iş koleksiyonu**, HTTP sözleşmesinde **152 adlandırılmış iş akışı ve 191 yol** bulunur. Yeni kod ayrı `playlists.py`, `smart_blocks.py`, `podcast.py`, `listener_stats.py`, `libretime_adapter.py` ve `audio_playout.py` dosyalarındadır.

## Yeniden kullanılabilir playlist

Playlist kimliği bir istasyona aittir. `create` yeni kimliği ve sürüm 1 taslağını oluşturur; `put-entry` sıraya sabit bir plan olayı, yayımlanmış smart block sürümü veya podcast beslemesi koyar. Sabit olay `source_kind: "event"` ile `entry_template` taşır; blok girdisi `source_kind: "smart_block"` ile `smart_block_version_id` taşır. Podcast girdisi `source_kind: "podcast_feed"` ile `podcast_feed_id` taşır ve `entry_template` boş bırakılır. Önizleme, bu beslemenin en yeni onaylı/doğrulanmış bölümünü tam teknik süresiyle seçer; hak ve medya hazır olma denetiminden geçirir. Somutlaştırılan plan sabit bölüm/sürüm/medya ve besleme kökenini taşır; sonradan gelen yeni bölüm onaylı planı kendiliğinden değiştirmez. `remove-entry` yalnız güncel taslakta girdiyi kaldırılmış durumuna alır; geçmiş/audit izi korunur. Yayımlanmış sürüm düzenlenmez; `fork` sonraki taslağı oluşturur.

`preview`, yayımlanmış listeyi `start_at` anından başlayarak ardışık somut plan olaylarına dönüştürür. Çıkış, bölge, servis, yerel yayın günü, hak ve medya hazır olma kontrolü yapılır. `materialize` önizlemenin `sequence` değerini `expected_sequence` olarak alır; veri bu arada değişmişse yazmaz. Bütün olaylar aynı taslak plana tek işlemle eklenir. Süre/sıra çakışması veya blok çözümleme hatasında kısmi liste yazılmaz. Plan onayı ve uç dağıtımı ayrı adımlardır.

| HTTP iş akışı | Kullanım |
|---|---|
| `playlists/create`, `list`, `versions`, `status` | Kimlik, istasyon listesi, sürüm/girdi listesi ve active/disabled durumu. |
| `playlists/put-entry`, `remove-entry` | Güncel taslağın sırası ve karma içeriği. |
| `playlists/publish`, `fork` | Ayrı `approve` yetkisiyle yayımlama; yeni taslak sürüm. |
| `playlists/preview`, `materialize` | Açıklamalı çözümleme ve atomik taslak plan yazımı. |

Liste en çok 500 etkin/çözümlenmiş girdi taşır. Grafik düzenleyici, dosya olmayan canlı girişin ses yürütücüsü ve üreticilerin bütün playlist komutları bu özelliğin kapsamına girmez.

## Statik ve dinamik smart block

Smart block, ayrı kimlik ve taslak/yayımlanmış sürüm taşır. `selection_mode: "static"` yayımlama anındaki seçilmiş katalog sürümlerini dondurur. `"dynamic"` her önizlemede uygun katalog adaylarını yeniden seçer. Statik içerik için de somut yayın slotunun güncel hak/medya uygunluğu tekrar denetlenir.

Kural ağacı `all` (AND), `any` (OR), `not` (NOT) ve `field/op/value` yapraklarından oluşur. Desteklenen alanlar `kind`, `category_id`, `rotation_code`, `item_id`, `version_id`, `title`, `artist`, `album`, `genre`, `bpm` ve `duration_ms` değerleridir. Sanatçı/albüm/tür/BPM için onaylı metadata okunur. Metin/kimlik alanları `eq` ve `in`, metin alanları ayrıca `contains`; BPM/süre `between` kullanır. Ağacın sınırı 8 derinlik ve 64 düğümdür; seçim en çok 50.000 katalog adayını değerlendirir.

Örnek kural:

```json
{
  "all": [
    {"field": "kind", "op": "eq", "value": "music"},
    {"field": "bpm", "op": "between", "value": [90, 120]},
    {"not": {"field": "title", "op": "contains", "value": "live"}}
  ]
}
```

`target_duration_ms`, `max_overshoot_ms` ve `max_items` süre/adet sınırlarını belirler; hedefe ulaşılamayan önizleme hazır sayılmaz ve plana yazılamaz. `selection_order` ordered veya deterministik shuffle olabilir. Blok tek başına somutlaştırılabilir veya playlistte yayımlanmış sürüm olarak kullanılabilir.

HTTP işlemleri: `smart-blocks/create`, `list`, `versions`, `status`, `put-draft`, `publish`, `fork`, `preview`, `materialize`. Yayımlama `approve`, taslak düzenleme/somutlaştırma `edit`, önizleme `view` yetkisi ister. Bütün LibreTime seçim operatörleri veya kendiliğinden program autoload davranışı iddia edilmez.

## RSS/Atom podcast

`podcasts/register` istasyona HTTP(S) beslemesi kaydeder. `preview-url` ağdan, `preview-text` JSON gövdesindeki `xml_text` değerinden RSS/Atom önizlemesi üretir. `commit-url` veya `commit-text` ile uygularken önizlemedeki `content_sha256` değerini `expected_content_sha256` olarak gönderin; değişen içerik reddedilir. GUID ve enclosure URL besleme kapsamında aynı bölümü bulur; çelişen iki mevcut kimlik birleştirilmez. Yeniden uygulama bölümleri çoğaltmaz.

`download`, bölümün enclosure dosyasını yönetilen medya nesnesine indirir; SHA, indirme iddiası ve bölüm→öğe/sürüm/medya bağı `.sdb` içinde tutulur. İndirilen sürüm taslaktır ve istasyon uygunluğu kapalıdır. Teknik inceleme, editoryal onay, uygunluk ve yayın hakları ayrı uygulanır. Aynı bölümün enclosure adresi değişirse önceki onay yeni sese aktarılmaz. Kesilmiş indirme için `recover`, süresi dolmuş iddiayı `repair` yetkisiyle açıkça serbest bırakır.

`latest` bölüm listesindeki en yeni kaydı verir; `approved_only` filtresiyle yalnız onaylı ve doğrulanmış medya seçilir. `put-latest`, son onaylı ve slot için hazır bölümü plan taslağına ekler; planın beklenen revizyonunu ister. Besleme yenilemesi ve indirme periyodik görevi operatör tarafından kurulmalıdır.

`auto-download`, `auto_download_latest` Boole politikasını `expected_revision` ile değiştirir; `import` ve `edit` yetkisi ister. `sync` bir sınırlandırılmış HTTP(S) besleme eşitlemesi yapar; politika açıksa en yeni bölümü indirme yordamıyla taslak medya olarak alır. `expected_content_sha256` isteğe bağlı önizleme önkoşuludur. Varsayılan politika kapalıdır. Periyodik görev `sync` çağrısını dış zamanlayıcıyla yürütür; otomatik indirme teknik/editoryal onay vermez.

Yeniden kullanılabilir playlistte `podcast_feed` girdisi kullanıldığında önizleme en yeni **onaylı/doğrulanmış** bölümü seçer; otomatik indirilen taslak bu şartları sağlamadan yayına alınamaz.

HTTP işlemleri: `podcasts/register`, `feeds`, `status`, `auto-download`, `sync`, `preview-url`, `preview-text`, `commit-url`, `commit-text`, `list`, `latest`, `download`, `recover`, `put-latest`.

İçe alma 2 MiB besleme, 500 bölüm ve 256 MiB enclosure sınırı uygular. DTD/entity içeren XML, kimlik bilgili URL ve özel/yerel IP hedefleri reddedilir; HTTP 80 ve HTTPS 443 kaynakları desteklenir; URL şeması korunur ve her bağlantıda çözümlenen genel IP sabitlenir. Bu sınırlar, otomatik ses onayı veya bütün besleme biçimleriyle uyumluluk anlamına gelmez.

## Mount başına dinleyici sayımı

`listeners/mount-create` sayım kaynağını bir istasyon/uç/aktif çıkışa bağlar. `sample-record` her örneği `source_namespace`, `source_sample_key` ve `observed_at` ile kaydeder. Aynı anahtar ve aynı iddia mevcut kayda döner; değişmiş iddia çakışır. Örnekler ve fingerprint değişmezdir.

`source_state: "online"` için `listeners` sıfır veya pozitif tam sayıdır. Offline/unknown sayısı null olmalıdır. `mount-status`, örnek yaşını `stale_after_seconds` ile karşılaştırır. `stats-query`, UTC dakika/saat/gün kovaları ve her mount'un son örneğini döndürür; bir mount eksik/offline/unknown ise toplam null kalır. Kova değeri süre ağırlıklı ortalama değildir.

HTTP işlemleri: `listeners/mount-create`, `mount-update`, `mount-list`, `sample-record`, `stats-query`, `mount-status`. Haricî Icecast/Shoutcast sayım okuyucusu `sample-record` çağırmalıdır. Otomatik polling, grafik ekranı ve benzersiz insan sayımı eklenmiş değildir.

## LibreTime playout veri adaptörü

`libretime/project`, onaylı RadioS planından takvim, show instance, show, file, webstream ve stream preferences verisi üretir. UUID/URI→tamsayı eşlemeleri `libretime_ids` koleksiyonunda kalıcıdır; aynı kaynağa sonraki çağrılarda aynı kimlik verilir. İşlem `manage` yetkisi ister ve girdi değişirse çakışmayla durur. `LibreTimeSnapshotClient`, bu veriyi LibreTime playout takvim okuyucusunun beklediği süreç içi istemci çağrılarına sunar.

`media_urls` nesnesi `media_file_id` anahtarlarını doğrulanmış yerel `file://` URI veya operatörün yayımladığı HTTPS URL'ye eşler. Yerel dosyada snapshot boyutu ve SHA eşleşmek zorundadır. HTTPS dosyanın servis edilmesi ve indirildikten sonra doğrulanması haricî bileşenin işidir.

```sh
sdbase --db /veri/RadioS.sdb libretime-project \
  --token YONETICI_TOKENI --station-id ISTASYON_UUID \
  --endpoint-id UC_UUID --revision-id PLAN_REVIZYON_UUID \
  --media-urls-json /veri/libretime-medya.json --show-name RadioS
```

Adaptör planlanmış, sabit süreli, mutlak ve ardışık ses/HTTP(S) stream olaylarıyla sınırlıdır. `cue_in_ms`/`cue_out_ms` kaynak kesiti, plan süresi ve doğrulanmış medya süresiyle eşleşirse LibreTime cue alanlarına çevrilir; stream üzerinde dosya cue offset kullanılamaz. Canlı giriş, uyarlamalı zamanlama, prodüksiyon geçişi/overlay, overlap ve istenmiş fade/gain/ducking/ReplayGain davranışı reddedilir. LibreTime okuyucusu takvim sınırlarını tam saniyeye düşürdüğü için kesirli saniyede başlangıç/bitiş de reddedilir (`no_subsecond_schedule_boundaries`); milisaniyelik kaynak cue kesiti korunur. Radyo cue kimliği çözümleme, DSP işleme, as-run/trafik kanıtı eşitlemesi ve ürünün bütün API/SQL sunucu yolları yoktur; yanıttaki `limitations` bu sınırları taşır. Tam LibreTime sunucusunun `.sdb` dosyasını veritabanı olarak doğrudan açtığı iddia edilmez.

## Kabul edilmiş uç planından WAV üretimi

```sh
sdbase audio-render --endpoint-dir /veri/yayin-ucu \
  --output /veri/cikti/yayin.wav --max-bytes 1000000000
```

`render_active_plan_wav`, gerçek `EndpointAgent` kabulü ve aktif medya manifestiyle çalışır. Kaynak SHA/boyutu kontrol edilir; cue kesitleri alınır, iki planlı başlangıç arasındaki boşluğa sessizlik yazılır. Üretim tamamlanmadan plan revizyonu ve motor dönemi tekrar denetlenir; mevcut dosya üzerine yazılmaz ve yeni WAV atomik yayımlanır. JSON sonuçta çıktı SHA/boyutu, süre, PCM özellikleri ve girdi/cue izi vardır.

Desteklenen sesler aynı örnekleme hızında mono/stereo 16 bit PCM WAV, olaylar sabit süreli mutlak ve ardışık ses olaylarıdır. Stream/live, uyarlamalı zamanlama, overlap/cut, prodüksiyon overlay, fade/DSP ve ses kartı çıkışı desteklenmez. WAV dosyası oluşturmak, sesin yayınlandığını veya dinleyiciye ulaştığını kanıtlamaz; bu yordam playout/monitör teslim kaydı üretmez.

## Yapılmış doğrulama ve kalan kabul

Geçici gerçek `.sdb` kontrollerinde playlist ve statik/dinamik blok farkı, tek işlemde plan yazımı, değişmiş sekans reddi, podcast tekilleştirme/gerçek medya nesnesi/ayrı onay/son bölüm planı ve mount başına değişmez örnekler denetlendi. Medya indirme/onay denemesinde ağ yanıtı taklit edildi; ayrıca gerçek BBC RSS ağıyla 282 bölüm önizleme/uygulama/tekrar ve yeniden açılış geçti. Gerçek kabul edilmiş `EndpointAgent` planında 12.000 frame/1.500 ms PCM WAV ve 500 ms boşluk doğrulandı; 14 olumlu/ret senaryosu geçti. LibreTime projeksiyonu `LibreTimeSnapshotClient` ile gerçek `get_schedule` fonksiyonunda WAV `FileEvent` ve dört webstream olayı üretti; 0,5–1,5 saniye kaynak cue kesitinin bir saniyelik plan süresinde korunduğu, tarih filtresi, tekrar ve yeniden açılış sonrası aynı tamsayı kimlikleri denetlendi. DSP, uyarlamalı zamanlama, kaynak dışı cue, kesirli takvim sınırı ve bozuk medya retleri geçti. Son HTTP kontrolünde 17 ek çağrı; besleme keşfi, otomatik indirme politikası/eşitleme/tekrar ve onaysız podcast playlist reddi geçti. Yeni bölümün onayı gelecekteki playlist çözümünü değiştirdi; önceki somut planın sürüm kimlikleri sabit kaldı. Ağ cevabı bu medya indirme kontrolünde taklit edildi. Ayrıntılı kanıt [doğrulama belgesinde](verification.md), ürün kapsamı [açık kaynak karşılaştırmasında](open_source_app_verification.md) bulunur.

Fiziksel ses cihazı, Liquidsoap/Icecast yayın zinciri, gerçek podcast enclosure ses indirmesi ve onayı, çok makine failover ve uzun süreli dinleyici zaman serisi yükü için ayrı kabul gerekir. Kaynak klasörüne kalıcı test/veri/ses dosyası eklenmedi.
