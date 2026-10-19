# HTTP ve uç taşıma sözleşmesi

`sdbase --db RadioS.sdb serve` varsayılan olarak `127.0.0.1:8000` üzerinde çalışır. Yanıtlar JSON, medya gövdesi `application/octet-stream` olur. Tüm `/v1` çağrıları `Authorization: Bearer TOKEN` ister. Uç tokenı yalnız kendi endpoint kimliğinin `operate` işlemlerinde geçerlidir; yönetici tokenı başka işlemler için yetki kaydıyla denetlenir.

`/v1/capabilities`, `desktop_api_version`, `desktop_configuration_kinds` ve
`source_digest` alanlarını da verir. `source_digest`, servis açılırken doğrudan
`sdbase/*.py` dosyalarından alınan SHA-256 özetidir: dosya adına göre sıralı her
dosyanın UTF-8 adı, NUL, ham içeriği ve NUL özete eklenir. Diskteki kod sonradan
değişse de çalışan servisin özeti değişmez; istemci eski süreçleri ayırt edebilir.

| Yol | Kullanım |
|---|---|
| `GET /health`, `GET /v1/capabilities`, `GET /v1/openapi.json` | Durum, desteklenen okuyucu/format ve yetkili OpenAPI 3.1 sözleşmesi. |
| `GET/POST /v1/items`, `POST /v1/items/{id}/versions` | Katalog arama/oluşturma; aramada kategori, doğrulanmış medya süresi, sürüm, uç hazır olma ve cursor filtresi. |
| `POST /v1/versions/{id}/media?station_id={id}`, `POST /v1/media/{id}/verify` | Ham medya yükleme ve SHA/teknik inceleme. |
| `POST /v1/versions/{id}/approve`, `POST /v1/items/{id}/eligibility`, `/lifecycle`, `/share`, `POST /v1/rights` | İçeriği ayrı onay/istasyon/hak adımlarından geçirir. |
| `POST /v1/access/users`, `POST /v1/access/users/{id}/tokens` | İstasyon yetkili kullanıcı oluşturma ve yeni kullanıcı tokenı. |
| `GET /v1/access/tokens/{id}`, `POST /v1/access/tokens/{id}/revoke` | Yetkili yöneticinin token revizyonunu görmesi ve iptali. |
| `POST /v1/endpoints/{id}/tokens`, `POST /v1/monitors/tokens` | Uç ve bağımsız monitör için ayrı, uç kapsamlı kimlik bilgisi. |
| `GET/POST /v1/import-profiles`, `POST /v1/import-profiles/{id}` | İstasyonun sürümlü Rekordbox aktarım profili; güncellemede `expected_revision` gerekir. |
| `POST /v1/imports/rekordbox/preview`, `/apply` | Profil kimliği ve `xml_base64` ile salt okunur XML önizleme / atomik uygulama. Uygulama `idempotency_key` ve `expected_preview_hash` ister. |
| `GET/POST /v1/views` | Kullanıcının istasyona özgü sütun ve filtre tercihi. |
| `POST /v1/plans`, `/v1/plans/{id}/entries`, `/approve`, `/distribute` | Taslak, onay ve uç için değişmez snapshot. |
| `GET /v1/endpoints/{id}/state` | Kabul edilen ve bekleyen plan kimliği, etkinleşme zamanı. |
| `GET /v1/endpoints/{id}/snapshots/{revision}` | İstasyon/uç/revizyon/medya SHA manifesti. |
| `GET /v1/endpoints/{id}/media/{file}` | Merkezde doğrulanmış medya akışı. |
| `POST /v1/endpoints/{id}/media/{file}/verify` | Uç ajanının kendi SHA raporu. |
| `POST /v1/endpoints/{id}/deliveries/{revision}/ack` | Tam snapshot ve tüm medya eşleşince teslim bildirimi. |
| `POST /v1/endpoints/{id}/plans/{revision}/accept`, `/heartbeat` | Yayın günü geldikten sonra motor dönemiyle plan kabulü ve sonraki günün çevrimdışı izni için taze sahiplik bildirimi. |
| `GET /v1/endpoints/{id}/commands`, `POST /commands/{command}/ack` | Süreli komut alımı, accepted/executed ayrımı. |
| `POST /v1/playout/events`, `POST /v1/observations`, `POST /v1/engine/commands` | Motor olayı, ayrı monitör gözlemi ve yönetici komutu. |
| `POST /v1/workflows/{alan}/{islem}` | Aşağıdaki uzman veri akışları; yalnız listelenen işlemler çağrılır. |

Uzman iş akışı çağrılarının gövdesi, ilgili Python servis işlevinin adlandırılmış JSON parametrelerini taşır. Yanıt JSON'dur; `GET` yerine `POST` kullanılır ve her işlem istasyon, kullanıcı ya da uç yetkisini kendi servisinde denetler. Kullanılabilir `{alan}/{islem}` değerleri:

| Alan | İşlemler | Veri sonucu |
|---|---|---|
| `catalog` | `category-create`, `category-update`, `item-category`, `person-create`, `contribution-add`, `group-membership-add` | Kategori, istasyona özel kategori ataması ve sanatçı/katkı ilişkisi. |
| `metadata` | `define`, `set`, `review` | Alan tanımı; `set` değeri yalnız inceleme bekleyen durumda yazar, `review` ayrı `approve` yetkisiyle onaylayan kişi/zaman izini kaydeder. Ortak değer bütün bağlı istasyonlardaki yetkiyi gerektirir. |
| `bulk` | `preview`, `apply`, `undo` | Önizlemeli atomik toplu düzenleme, revizyon ve tekrar anahtarıyla güvenli geri alma. |
| `carts` | `create`, `status`, `put-cut`, `list-cuts`, `preview-cut`, `resolve-entry` | Cart/cut tanımı ve taslak ses girdisine sürüm dondurma. |
| `playlists` | `create`, `list`, `versions`, `status`, `put-entry`, `remove-entry`, `publish`, `fork`, `preview`, `materialize` | Sürümlü karma playlist, taslakta sıralama/kaldırma, ayrı yayımlama ve sekans önkoşullu atomik plan yazımı. |
| `smart-blocks` | `create`, `list`, `versions`, `status`, `put-draft`, `publish`, `fork`, `preview`, `materialize` | AND/OR/NOT kuralları, statik/dinamik seçim, hedef süre sınırı ve tek başına ya da playlist içinde yeniden kullanım. |
| `podcasts` | `register`, `feeds`, `status`, `auto-download`, `sync`, `preview-url`, `preview-text`, `commit-url`, `commit-text`, `list`, `latest`, `download`, `recover`, `put-latest` | RSS/Atom beslemesi, GUID/enclosure tekilleştirme, otomatik son bölüm indirme politikası, tek eşitleme çağrısı, inceleme bekleyen medya ve son onaylı bölümün taslak plana alınması. |
| `listeners` | `mount-create`, `mount-update`, `mount-list`, `sample-record`, `stats-query`, `mount-status` | Mount kapsamlı değişmez kaynak sayımı, UTC kovalar, stale/offline/unknown ayrımı. |
| `libretime` | `project` | Onaylı ses/stream snapshotının kalıcı tamsayı kimliklerle LibreTime playout verisine eşlenmesi. |
| `rotation` | `put-rule`, `status`, `evaluate`, `record-decision` | Sürümlü rotasyon kuralı, nedenli seçim ve karar kaydı. |
| `schedules` | `create`, `preview`, `edit`, `status`, `materialize` | Tekrar eden takvim dizisi, istisna/seri değişimi ve taslak plana yazım. |
| `cues` | `create`, `propose-dj`, `approve`, `withdraw` | Ses dosyası/sürüme bağlı radyo cue önerisi ve ayrı onay. |
| `production` | `create-transition`, `create-placement`, `approve-placement`, `reconcile`, `rebind-transition`, `withdraw-placement`, `withdraw-transition` | Somut iki ses girdisi arasına bağlı sweeper/voice track verisi. |
| `plans`, `media` | `fork-draft`; `reconcile` | Onaylı planın yeni taslağı ve yönetilen medyanın bütünlük uzlaştırması. |
| `outputs` | `create`, `update`, `list` | Uç çıkışı ve bölge kapsamı. |
| `diagnostics` | `record`, `list`, `update` | Tekil olay, tekrarlanan arıza ve müdahale durumu. |
| `requests` | `submit`, `decide`, `queue`, `fulfill`, `list` | Dinleyici isteği ve plan bağı. |
| `clocks` | `create`, `fork`, `put-slot`, `publish`, `assign`, `exception`, `preview`, `materialize`, `auto-preview`, `auto-materialize` | Saat şablonu, istisna, otomatik sürüm seçimi ve taslak plana yazım. |
| `alerts` | `register`, `cancel`, `start`, `finish`, `link-observation`, `list` | Acil duyuru kapsamı, girişim ve harici çıkış gözlemi. |
| `recording` | `create`, `status`, `preview`, `start`, `finish` | Zamanlı kayıt tanımı, girişimi ve medya sonucu. |
| `traffic` | `advertiser-create`, `campaign-create`, `campaign-approve`, `spot-approve`, `obligation-create`, `preview-break`, `reserve-break`, `reconcile`, `makegood-propose`, `makegood-review`, `makegood-preview`, `makegood-reserve`, `propose`, `status`, `apply-target`, `supersede-target` | Kampanya/spot/yükümlülükten atomik reklam rezervasyonu, insan onaylı telafi rezervasyonu, motor/çıkış kanıtı ayrılmış rapor ve spot değişimi takibi. |
| `now-playing` | `project-event`, `next`, `talk`, `current` | Çıkışa özgü şimdi çalıyor izdüşümü. |
| `asrun` | `preview`, `create`, `get`, `export` | Kaynak sırası ve hash'i saklanan yayın sonrası rapor. |
| `live` | `propose`, `poll`, `acknowledge`, `report-result`, `link-evidence` | Kabul edilmiş uç planı üzerindeki kısa süreli atla/değiştir/ekle komutu ve ayrı yürütme iddiası. |
| `stations` | `clone-preview`, `clone-apply`, `endpoint-create`, `grant-permission`, `activate-clone` | Yeni kimlikli istasyonun sınırlı kopyası ve ayrı uç/yetkiyle kurulumu. |
| `capacity` | `inspect`, `evaluate`, `delivery-inspect`, `delivery-evaluate` | İstasyona özgü eşikle sınırlı büyüme/boş plan/disk ölçümü ve güncel planın geciken dağıtımı/eksik uç medyası; `evaluate` işlemleri durum geçişlerini teşhis olayına yazar. |
| `services` | `create`, `list` | Bir istasyondaki yayın servisini tek endpoint ve aktif çıkışa kalıcı bağlar; servis başına aynı gün ayrı plan olabilir. |

0.6.0 kaynak durumunda **160 adlandırılmış iş akışı ve 208 HTTP yolu** vardır. `GET /v1/openapi.json` bütün HTTP yollarını listeler. Uzman iş akışlarının gerekli/isteğe bağlı üst düzey gövde alanları servis işlevlerinin imzalarından üretilir; doğrudan arama/uç yollarının sorgu parametreleri de tanımlıdır. Özel medya, katalog ve uç yollarının gövde ayrıntıları bu belgededir. Sunucu her süreçte istemci IP'si ve token özeti için ayrı 120 istek patlama / saniyede 10 yenileme sınırı uygular; aşım `429` ve `Retry-After` döndürür. Çok süreçli veya çok makineli ortak sınır gerekiyorsa ağ geçidinde ayrıca yapılandırılır.

`services/create` için `station_id`, `service_key`, `name`, `endpoint_id`, `output_id` gerekir. Önce `outputs/create` ile aynı endpoint üzerindeki etkin çıkışı kaydedin. Bir endpoint yalnız bir servisle bağlanır. `POST /v1/plans` isteği `service_key` alabilir; varsayılan `default` eski tek plan akışıdır. `clocks/assign`, `clocks/preview`, `clocks/materialize`, `clocks/auto-preview`, `clocks/auto-materialize` ve `schedules/create` aynı opsiyonel `service_key` alanını alır. Bağlı serviste ses girdisinin `payload.output_id` değeri servis çıkışı olmalıdır; saat önizlemesi çıkışı otomatik sabitler. Planın snapshot'ı `service_key` taşır ve yalnız bağlı endpoint'e hazırlanır. Planlı olaylar kabul edilmiş endpoint snapshot'ındaki giriş/sürüm/çıkışla eşleşmelidir; sonradan gelen eski olay aynı `engine_event_id` ile tekilleşir.

`playlists/preview` ve `smart-blocks/preview` için `station_id`, yayımlanmış `version_id`, zaman dilimli `start_at` verilir; isteğe bağlı `service_key`, `territory`, `output_id` ve `ordinal_base` kapsamı belirler. `materialize` aynı alanlara ek olarak önizlemedeki `sequence` değerini `expected_sequence` olarak ister. Playlist `put-entry` için `source_kind=podcast_feed` ve `podcast_feed_id` ile boş `entry_template` verilmesi, önizlemede son onaylı/doğrulanmış bölümü tam teknik süresiyle çözer. Plan girdisi seçilmiş bölüm/sürüm kökeniyle sabitlenir. Araya herhangi bir commit girerse yeniden önizleme gerekir. Yayımlama beklenen ebeveyn/sürüm revizyonlarını ve ayrı `approve` yetkisini denetler; somut plan sonradan ayrıca onaylanır.

`podcasts/feeds`, istasyonun beslemelerini ve kalıcı indirme politikasını keşfetmek için kullanılır. `podcasts/preview-text` ve `commit-text`, XML'i JSON metni `xml_text` ile alır. `preview-url`/`commit-url`, kaydedilmiş HTTP(S) beslemesini okur. Uygulamada `expected_content_sha256` verilmesi önizlemeden sonra değişen beslemeyi reddeder. `auto-download`, `auto_download_latest` ve `expected_revision` ile varsayılanı kapalı indirme politikasını değiştirir. `sync`, bir HTTP(S) besleme eşitlemesi yapar; politika açıksa en yeni bölümü taslak medya olarak indirir. Dış zamanlayıcı `sync` çağrılarını periyodik çalıştırabilir. `download` ve otomatik indirme yalnız taslak sürüm/inceleme bekleyen medya üretir. `put-latest`, `expected_plan_revision` yanında tarih, sıra, süre ve hak/çıkış kapsamını alır; onaysız ya da hazır olmayan bölümü planlayamaz.

`listeners/sample-record`, `source_state=online` iken `listeners` tam sayı, offline/unknown iken null ister. Mount'un kaynak kapsamı ve uç yetkisi denetlenir; aynı `source_sample_key` farklı içerikle kullanılamaz. `stats-query` aralık ve dakika/saat/gün `bucket` seçer; eksik mount varsa null toplamı sıfır olarak göstermeyin.

`libretime/project` için `station_id`, `endpoint_id`, `revision_id` ve `media_urls` nesnesi gerekir. JSON sonuç LibreTime playout okuyucusuna verilecek veridir; tam ürün HTTP sunucusu değildir. Şema ve örnek CLI dahil [özellik belgesi](new_features.md), desteklenen ses/stream olaylarını ve kayıp sınırlarını açıklar.

`asrun/export` JSON içinde CSV veya JSON içeriği döndürür; JSON ve CSV seçimi servis parametresidir. `live` komutları uç ajanına kendiliğinden iletilmez: uç tarafında bu çağrıları kullanan bir adaptör kurulmadan veri düzeyindeki komut ses yürütmez. `report-result` bir motor iddiasıdır; `link-evidence` eşleşen playout kaydını bağlar, duyulan sesi ayrıca `output_observations` kanıtlar.

Medya yükleme `application/octet-stream`, `Content-Length` ve en çok 512 MiB ister; sunucu özel geçici dosyaya akıtır ve içerik adresli medya nesnesine aktarır. `verify` teknik alanları ve tekrar SHA denetimini ister. Kullanıcı tokenları yalnız oluşturma/yenileme yanıtında açık metin görünür; `GET /v1/access/tokens/{id}` tokenın kendisini veya hash'ini döndürmez. İptal isteğinde görülen `revision`, `expected_revision` olarak gönderilir.

`/v1/observations` için `observer_kind=external_monitor` ve `evidence_ref` kullanıldığında yalnız ilgili uç için verilmiş monitör tokenı kabul edilir; `details.output_id` etkin bir istasyon çıkışı olmalıdır. Motorun `complete` olayı veya çıkışta `audio_present` algısı, belirli bir reklamın duyulduğuna tek başına kanıt değildir. Ayrı kanıt sınıfları yayın sonrası raporda korunur.

Uç, geçerli günün kabulü ve taze heartbeat'i varken ertesi yerel gün planını SHA ile tamamlayıp ACK verdiğinde yanıt `offline_grant` taşıyabilir. Ajan izni yerel günlüğüne yazıp medya bütünlüğünü tekrar denetler; merkez erişilemezse yalnız izin penceresinde sonraki plana geçer. Sonraki gece yarısında izin biter; bağlantı geri döndüğünde merkezde farklı motor/revizyon varsa uç kendini durdurur.

`metadata/set` gövdesi `station_id`, `field_key`, `target_id`, `value` ve isteğe bağlı `station_override`, `expected_revision`, `clear`, `source_kind` alır. İstemci `approval_status=approved/rejected` gönderemez. `metadata/review` gövdesi `station_id`, `value_id`, `decision`, `expected_revision` ve isteğe bağlı `reason` alır. Onaylanan değer daha sonra düzenlenirse yeniden inceleme bekler. `activate` aşaması için zorunlu alanlar ancak onaylandıktan sonra hazır sayılır. Bu yetki garantisi servis ve HTTP yolları içindir; uygulama geliştiricisi düşük seviyeli `Store.put()` erişimini güvenilmeyen istemciye açmamalıdır.

`traffic/preview-break`, etkin `station_outputs.output_id` ve taslak plandaki sabit süreli `ad_break` girdisini kullanır. Gövde `station_id`, `plan_id`, `revision_id`, `break_entry_id`, `output_id` ve isteğe bağlı `region_id`, `max_spots`, ayrım aralıklarını taşır. `traffic/reserve-break` aynı girdilere ek olarak dönen `preview_hash`, `plan_revision` ve sabit `idempotency_key` değerlerini `expected_preview_hash`, `expected_plan_revision` olarak alır. Ses girdileri ve reklam yerleşimleri tek işlemle yazılır; başka planlayıcının aynı yükümlülüğü tüketmesi engellenir. `traffic/reconcile` `station_id` ve `campaign_id` ile plan, motor ve harici çıkış gözlemi ayrımını döndürür. `audio_present`, spotun gerçekten duyulduğunu kanıtlamaz; sonuç `content_verified_plays=0` ve doğrulanmamış kısa teslim olarak etiketlenir.

`traffic/makegood-propose` aynı kampanyanın gelecekteki boş reklam aralığı için `station_id`, `campaign_id`, `plan_id`, `revision_id`, `break_entry_id`, `output_id`, `idempotency_key` ve isteğe bağlı `region_id` alır. Kalan yükümlülükten gelecek rezervasyonları düşer, `ads.suggest_makegoods` önerisini medya/hak uygunluğuyla denetler ve değişmez öneri özeti kaydeder. `traffic/makegood-review` farklı bir kullanıcının `approve` yetkisiyle `request_id`, `expected_revision`, `expected_recommendation_hash`, `decision` (`approved`/`rejected`) ve ret için `reason` alır. `traffic/makegood-preview` onaylı isteğin o andaki `preview_hash` ve plan sürümünü döndürür. `traffic/makegood-reserve` bunları `expected_request_revision`, `expected_plan_revision`, `expected_preview_hash` ve `idempotency_key` olarak alıp telafi isteğini ses girdisi/yerleşimle aynı işlemde bağlar. Öneri, onay ve rezervasyon teslim kanıtı değildir; `traffic/reconcile` hepsini ayrı gösterir ve doğrulanmamış çıkış etkinliğini teslim saymaz.

`stations/clone-preview` isteği `source_station_id`, `new_code`, `idempotency_key` ve isteğe bağlı `new_name`, `timezone_name`, `copy_catalogue_links` alır. Dönen `snapshot_hash`, aynı alanlarla `stations/clone-apply` isteğinde `preview_hash` olarak verilir. Yeni istasyon ayrı UUID ile ve `disabled` durumunda açılır. Kategori ile taslak rotasyon kuralları aktarılır; katalog bağlantıları ancak istenirse ve `eligible=false` olarak gelir. Uçlar, tokenlar, haklar, planlar ve yayın geçmişi kopyalanmaz. Ardından `endpoint-create` ile yeni uç, `grant-permission` ile istasyona sınırlı yetki kurulabilir; `activate-clone` yeni uç oluşturulmadan etkinleşmez. Uç tokenı ayrıca verilir. Bu işlem yedekten aynı istasyonu geri yükleme işlemi değildir ve ses motorunu başlatmaz.

`capacity/inspect` ve `capacity/evaluate`, `station_id` ile bir `policy` nesnesi alır. Politika; `policy_id`, `window_hours`, katalog/son ekleme/plan revizyonu/boş revizyon/girdi sayısı eşikleri, `.sdb` günlüğü ve toplam veri boyutu sınırları, disk boş alanı uyarı/kritik eşikleri ve isteğe bağlı `max_scan_rows` taşır. `evaluate` ayrıca her örnekleme için yeni `observation_key` ister. Eksik tarih veya tarama bütçesi sonuçları `unknown` olarak görünür; alarm bir eşik aşımını bildirir, otomatik yazma engeli kurmaz.

Uç ajanı `endpoint_protocol.py` arayüzünü kullanır; `transport.py` standart kütüphaneli HTTP uygulamasıdır. Aynı çağrının tekrarı kimlikle tekilleştirilir. Yeni çalma girişimi yeni `attempt_key` taşır; `start` ve `complete` aynı girişime bağlanır. Çıkış gözlemi ayrı kayıt ve ayrı kanıt düzeyidir.

XML çağrısında önce `POST /v1/import-profiles` ile `station_id`, `name`, kalıcı `library_key` ve `config` verilir. Önizlemede `station_id`, `profile_id`, `xml_base64` gönderilir. `apply` aynı alanlara ek olarak önizlemeden dönen `preview_hash` değerini `expected_preview_hash` ve yeniden deneme için sabit `idempotency_key` olarak alır. XML aktarımı kaynağı değiştirmez; medya dosyasını onaylamaz. JSON gövdesi için aktarım sınırı 90 MB, çözülmüş XML için okuyucu sınırı 64 MB'dir. Daha büyük kütüphaneler küçük seçilmiş dışa aktarımlarla işlenmelidir.

`schedules.py` ve `production.py` servisleri Python API'siyle erişilir. Onaylı plan görüntüsünde `production.placements` veri kararları ve gereken medya SHA manifesti taşınır; `execution_capability: data_only` gerçek ses ya da MIDI yürütmesi vaadi değildir.

## Ayrıntı ve revision okuma

Aşağıdaki GET yolları user credential ve istasyonda `view` yetkisi ister. İstasyon yolunun dışında `?station_id=UUID` zorunludur.

| Nesne | Yol |
|---|---|
| İçerik / sürümleri | `/v1/items/{item_id}`, `/v1/items/{item_id}/versions` |
| İçerik sürümü | `/v1/versions/{version_id}` |
| Plan / girdileri / tek girdi | `/v1/plans/{plan_id}`, `/v1/plans/{plan_id}/entries`, `/v1/plans/{plan_id}/entries/{entry_id}` |
| Plan revizyonları / tek revizyon / revizyon girdileri | `/v1/plans/{plan_id}/revisions`, `/v1/plans/{plan_id}/revisions/{revision_id}`, `/v1/plans/{plan_id}/revisions/{revision_id}/entries` |
| İstasyon içerik ilişkileri / tek ilişki | `/v1/stations/{station_id}/items`, `/v1/stations/{station_id}/items/{item_id}` |

Yanıt `sequence` ve ilgili `item`, `version`, `plan`, `revision`, `entry`, `station_item` veya liste alanlarını içerir. Her belge gerçek `_id`/`_rev` taşır. `revision_no` plan sürüm numarasıdır; optimistic concurrency için belgenin `_rev` değeri kullanılır. Girdi yazımında `expected_plan_revision=plan._rev`, `expected_entry_revision=entry._rev` gönderilir. Eski değer 409 verir; güncel ayrıntı okunup değişiklik uzlaştırılır. 0.7.0'da yetki ve ayrıntı aynı commit görüntüsünden okunur; arada başka bir commit olsa da açık görüntü tutarlı kalır. Olmayan veya başka istasyonun nesnesi 404; yetkisiz istasyon veya endpoint credential 403 döner.

Girdi listesi varsayılan olarak güncel taslağı, yoksa onaylı revizyonu okur; `revision_id` ile açık seçim yapılabilir. İstasyon ilişkisi listesinde `limit` 1–1000 ve `cursor` vardır; ilişki kuşağı değişince cursor 409 ile geçersizleşir.

## Motor yönetimi

`GET /v1/operations/metrics` ve `GET /v1/operations/integrity` etkin global `manage` kullanıcısı ister. Integrity query alanları `max_records`, `max_bytes`, `verify_media`, `verify_cold` değerleridir. Ayrıntılar [izleme yordamında](engine_operations.md) ve [scrub sözleşmesindedir](integrity.md). Standby/fenced HTTP mutasyonları 503 verir; journal kalıcıyken WAL arşiv onayı başarısızsa ayrı 503 yanıtı `committed=true`, `sequence`, `transaction_id` taşır. İkinci durumda önce işlem sonucu okunur.

`markers/create`, `markers/list`, `markers/get` [değişmez marker setlerini](playback_markers.md) yönetir. `retention/policy-put`, `policy-get`, `preview`, `apply`, `query` [soğuk geçmişi](retention.md) yalnız tüm istasyonlarda yönetim yetkisi olan kullanıcıya açar.
