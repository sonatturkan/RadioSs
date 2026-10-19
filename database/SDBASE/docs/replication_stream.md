# TLS üzerinden artımlı çoğaltma ve sürekli follower

SDBASE 0.8.0 / schema v4, doğrulanmış WAL paketlerini artık gerçek HTTP/TLS bağlantısı üzerinden aktarır. Kaynak ve alıcı ayrı süreçlerde çalışabilir. Kayıt, medya nesnesi ve soğuk geçmiş baytları aynı doğrulanmış paket kapsamında gelir; SQL motoru veya başka ürünün replication protokolü kullanılmaz.

Follower yalnız önceden oluşturulmuş **standby** üzerinde çalışır. `replica-create` ile tutarlı fiziksel taban hazırlanır; başka makineye bu standby dizini güvenli dosya aktarımıyla götürülebilir. HTTP transport ilk tabanı kendiliğinden oluşturmaz ve follower kendiliğinden primary olmaz.

## Kimlik doğrulama ve TLS

Listener ayrı, opt-in süreçtir. Bearer token kullanıcı API token'ından ayrıdır ve bütün fiziksel çoğaltma verilerine erişim sağlar. En az 32, en fazla 512 boşluksuz ASCII karakter gerekir. Token ortam değişkeninden okunur; komut satırı argümanına veya erişim loguna yazılmaz. Ortam değişkenini işletim sisteminizin secret/service yapılandırmasıyla sağlayın; iki tarafta aynı özel token bulunmalıdır.

TLS sertifikası ve anahtarı varsayılan olarak zorunludur. Follower sertifika zincirini ve URL hostname'ini doğrular; `--ca-file` özel radyo ağı CA'sını ekler. `insecure`/sertifika doğrulamasını kapatma seçeneği yoktur. Token HTTP redirect ile başka adrese aktarılmaz. TLS en az 1.2'dir; TLS handshake ve aktif socket sayıları da sınırlandırılır.

```sh
sdbase --db /veri/primary/RadioS.sdb replication-serve \
  --host 192.168.10.20 --port 8443 \
  --token-env SDBASE_REPLICATION_BEARER \
  --cert /guvenlik/primary-cert.pem --key /guvenlik/primary-key.pem

sdbase --db /veri/standby/RadioS.sdb replication-pull \
  --source-url https://primary.radyo.local:8443 \
  --token-env SDBASE_REPLICATION_BEARER --ca-file /guvenlik/radyo-ca.pem

sdbase --db /veri/standby/RadioS.sdb replication-follow \
  --source-url https://primary.radyo.local:8443 \
  --token-env SDBASE_REPLICATION_BEARER --ca-file /guvenlik/radyo-ca.pem \
  --poll-interval 1
```

`replication-pull` tek sınırlı batch alır. `replication-follow` gerideyken hemen sonraki batch'i ister, güncelken poll aralığını bekler. Ağ kesintisi, timeout, 404 ile süresi dolmuş cache veya 503 kapasite reddinde artan bekleme uygulanır; üst bekleme 30 saniyedir. Kimlik, history/hash, token, sertifika veya byte tutarlılığı hatası follower'ı durdurur. Bu hatalar ağ kesintisi gibi gizlenmez.

Yalnız geliştirme/kabul ortamında hem sender hem client'a `--allow-loopback-http` verilerek **literal loopback IP** üzerinde plaintext HTTP kullanılabilir. Bu seçenek `0.0.0.0`, LAN IP'si veya dış hostname için TLS şartını kaldırmaz.

## Protokol

Tüm yollar bearer ister:

| İstek | İşlem |
|---|---|
| `GET /replication/v1/status` | Kaynak store/node/timeline/epoch/schema kimliği, sequence ve limitler. |
| `POST /replication/v1/batches` | Beklenen kaynak kimliği, `after_sequence`, önceki archive SHA ve `previous_effective_at`, kayıt ve toplam byte bütçesiyle paket isteme. |
| `POST /replication/v1/anchor` | 0.7 standby için mevcut sequence/SHA'ya bağlı timestamp'i bir kez, sabit bellekle doğrulama. |
| `GET /replication/v1/batches/BATCH_ID/files/ENCODED_PATH` | Manifestteki tek dosya; `Range: bytes=N-` ile devam desteklenir. |
| `DELETE /replication/v1/batches/BATCH_ID` | Durably replay edilmiş paketin geçici cache alanını bırakma. |

Batch ID istek alanlarının SHA-256'ıdır; aynı başlangıç/anchor/limit isteği cache'te kaldığı sürece aynı mühürlü paketi döndürür. Cache sona erince yeni paketin manifest hash'i değişebilir; alıcı eski eksik aktarımı temizleyip yeni pakete başlar. Sender hiçbir kaynak domain kaydını POST/DELETE ile değiştirmez; bunlar yalnız özel transfer cache işlemleridir.

Kaynak kimliği physical standby metadata'sındaki `source_store_id`, `source_node_id`, `source_timeline_id`, `source_epoch`, schema ve database adıyla birebir eşleşmelidir. Sequence/hash başlama noktası da sabittir. Yanlış merkez, eski/yeni farklı timeline, gerileyen sequence veya farklı archive anchor kabul edilmez. Listener kaynağın primary rolü veya başladığı kimlik değişirse yeni paket/dosya sunmayı durdurur.

## Gerçek sınırlar ve backpressure

Varsayılan sınırlar:

| Kaynak | Sınır |
|---|---:|
| Paket kayıt sayısı | 1000 |
| Paket toplam dosya baytı + manifest | 64 MiB |
| Sender cache toplamı | 512 MiB |
| Aynı anda tutulan batch | 4 |
| Tek paket hazırlayıcı | 1 |
| Aktif connection/TLS handshake | 8 |
| Socket/handshake timeout | 15 saniye |
| Cache boşta kalma süresi | 120 saniye |

CLI `--max-records`, `--max-batch-bytes`, `--max-cache-bytes`, `--max-cached-batches`, `--ttl-seconds` ile ilgili limitleri ayarlar. Follower'ın istediği sınır sender sınırından büyükse sender limitine indirilir. Byte bütçesi yalnız JSON'u değil **bütün medya/cold dosyaları ve manifesti** kapsar; tamamlanmış paket boyutu yayımlanmadan tekrar ölçülür. Tek record/checkpoint ve bağlı medya bu bütçeyi aşıyorsa yarım record gönderilmez: limit büyütülmeli veya yeni fiziksel taban alınmalıdır. Journal paket okuyucusunun `records.json` için mevcut 256 MiB üst sınırı devam eder.

Hazırlayıcı meşgulse veya aktif download'lar cache kapasitesini doldurmuşsa sender 503 ve Retry-After döndürür. İnaktif eski paketler kapasite/TTL ile çıkarılır. Aktif dosya okunurken onun cache dizini kaldırılmaz; ACK/TTL silmeyi indirme tamamlanana kadar erteler. Kaynak başına disk lock yalnız tek listener'ın cache'i yönetmesini sağlar. Standby step lock aynı anda iki indirici/replay işini engeller; ayrı runner lock sürekli follower süreçlerini tekilleştirir.

## Kesinti ve yeniden başlama

Sender cache: `RadioS.sdb.replication-stream-cache/`. Follower cache: `RadioS.sdb.replication-stream/`. Her ikisi store/node kimliğini taşıyan sahiplik marker'ı ile, private izinlerle oluşturulur. Beklenmedik/başka node'a ait klasör temizlenmez. Sender yalnız kendi işaretli transfer dizinlerini yönetir; metadata/WAL/medya deposunu TTL ile silmez. ACK veya normal kapanışta paketler bırakılır; süreç zorla kapanırsa sonraki listener mevcut mühürlü paketleri doğrular, süresi dolmuş/yarım kendi cache işlerini temizler.

Alıcı önce manifest/hash/kimlik/bütçeyi denetler; dosyaları `.part` olarak indirir. Yeniden başlatma aynı manifest için mevcut byte sayısından **HTTP Range** ile devam eder. `Content-Length`, ETag, Content-Range, tam SHA-256 ve boyut zorunludur. Baytlar tam doğrulanmadan dosya nihai incoming adına geçmez. Manifest envanteri, journal frame'leri, archive zinciri, domain referansları ve managed nesneler tekrar doğrulanır; sonra normal standby replay uygulanır. Ağ kesintisi veya tamper alıcının sequence'ını ilerletmez.

Replay sonrası kalıcı sequence/hash yayınlanmışsa source ACK gönderilir; ACK kaybolursa sender TTL temizliği yeterlidir. Alıcı cache'i tam paket uygulandıktan sonra bırakılır. Replay sırasında süreç kapanırsa sonraki `replication-pull/follow`, yereldeki aynı doğrulanmış paketi kullanarak mevcut pending frame'i veya birebir torn prefix'i recovery'den geçirir. Eksik/mismatched bir paketle journal düzeltilmez.

Sürekli follower `caught_up`, `catching_up` ve `retrying` durumlarını raporlar. Bu asenkron durum, source'un yeni commit ettiği bütün verilerin diğer makinede fsync edildiği yönünde senkron commit garantisi vermez. Fiziksel bootstrap aktarımı, ağ quorum'u ve otomatik uzak lider seçimi bu modülün kapsamı değildir.

Artımlı HTTP yolu **bütün eski WAL geçmişini her batch yeniden taramaz**. Yeni fiziksel tabanın manifesti WAL sequence/SHA ile beraber doğrulanmış `effective_at` taşır; standby bunu `last_effective_at` olarak kalıcı tutar. Her replay'de tam doğrulanmış yeni kaydın timestamp'i aynı progress metadata'sına yazılır. Sonraki batch eski predecessor'ın bütün JSON'unu yüklemez: canonical envelope'un 11 baytlık başlığı ve 77 baytlık SHA suffix'i bilinen hash ile eşleştirilir, ilk yeni timestamp kalıcı pin'den büyük olmalıdır. Archive header kimliği, seçilen yeni kayıtların bütün checksum/identity/schema/timeline/epoch, sıra/zincir/timestamp kontrolleri ve nesnelerin SHA/boyut doğrulaması korunur. Predecessor boyutu bu sabit okumanın bellek maliyetini artırmaz.

0.7 standby metadata'sında timestamp yoksa follower `/anchor` üzerinden **bir kez** upgrade yapar. Kaynak, zaten alıcıda sabit olan sequence/SHA için ham canonical payload SHA'sını 256 KiB bloklarla yeniden hesaplar; yalnız bounded root scalar'ları ayırır, checkpoint ağacını yüklemez. Scan üst sınırı 64 GiB'dir ve aynı tek hazırlayıcı/backpressure slot'unu kullanır. Kaynak kimliği/primary rolü scan sonrası tekrar kontrol edilir; alıcı da kendi rolü/kimliği/progress'i değişmemişse timestamp'i kalıcı yayınlar. Eksik timestamp'li batch doğrudan reddedilir; archive başlangıç zamanı gibi daha zayıf bir alt sınıra dönülmez. Hash/kimlik/scan doğrulaması başarısızsa alıcı ilerlemez. Eski pending replay önce kendi tam doğrulanmış paketiyle tamamlanmalıdır.

**Yeni seçilmiş** tek source record'u en fazla istenen byte bütçesi/256 MiB işlenir. Büyük sonraki record mevcut batch'e sığmıyorsa önceki tamamlanmış kayıt öneki gönderilir; ilk record sığmıyorsa reddedilir. Böyle bir kayıt üzerinden atlamak için fiziksel rebase gerekir. Büyük predecessor desteği büyük bir sonraki checkpoint'in HTTP batch sınırını kaldırmaz.

Bu kontrol alıcıda daha önce uygulanmış/tabanda sabitlenmiş SHA ve timestamp'i dayanak alır; eski source arşivinin bütünü için sertifika değildir. `wal-status` tam arşiv doğrulaması, PITR ve global integrity yolları bütün geçmiş denetimini korur. Eski ilgisiz bir arşiv dosyasının bozulması global denetimde bulunur; halihazırda doğru tabanı olan follower'ın yeni sabitlenmiş aralığını teslim edebilmesi ayrı bir güvencedir. Manuel `create_shipment` varsayılanı tam arşiv denetimine devam eder. Global WAL doğrulaması/PITR ve normal store recovery hâlâ ilgili tam JSON kayıtlarını belleğe yükler; bunlar paged recovery veya büyük checkpoint'ler için sabit bellek garantisi sunmaz.

## Kaynak örüntüleri ve çalıştırılan kabul

Yerel PostgreSQL 20devel `src/backend/replication/walsender.c` içinde receiver yanıtlarını işlerken gönderme tamponunu boşaltma, caught-up durumu ve tam record sınırında maksimum gönderim büyüklüğü incelendi. Yerel MariaDB 13 kaynaklarında `sql/slave.cc` packet/read timeout limitleri ve `sql/rpl_parallel.cc` bounded receiver/worker yaklaşımı incelendi. Bu fikirler kendi belge/WAL/bearer protokolümüze bağımsız uyarlanmıştır; bu ürünlerden kaynak kodu veya SQL motoru eklenmemiştir.

2026-10-01'de `PYTHONDONTWRITEBYTECODE=1 python tools/stream_acceptance.py` ile **53/53 kontrol** geçti; **10 gerçek worker süreci** kullanıldı. Kontroller gerçek HTTP ve OpenSSL sertifikalı, CA doğrulamalı TLS byte transferini; 64 KiB sonrası bağlantı kesilmesi ve Range resume'u; source/follower süreç restart'ını; cold checkpoint/medya bütünlüğünü; token/kimlik/hash/tamper reddini; sürekli outage backoff'unu; record/byte/cache/TTL/builder sınırlarını ve standby'ın kendiliğinden primary olmamasını kapsar. Ayrıca source worker'da bütün WAL history enumeration'ı yasaklanmışken taşıma çalıştırıldı; ilgisiz eski arşiv dosyası bozukken pinned yeni aralık aktarımı, global denetimin eski hasarı bulması ve seçilen orta frame eksikken replay'in durması doğrulandı.

Ek senaryo **260 MiB canonical checkpoint envelope fixture** kullanır: engine'in izin verdiği ek checkpoint metadata'sı boyutu büyütür, native domain state küçük kalır. Bir defalık exact SHA/timestamp extraction için Python traced allocation **4 MiB altında** ölçüldü; doğrulanmış physical rebase timestamp'i taşıdı. Ayrı gerçek HTTP worker'da predecessor JSON load ve stamp rescan açıkça yasaklanmışken **64 KiB** batch yeni transaction'ı taşıdı. Bu checkpoint boyut sınırı kanıtıdır; yüz milyon kayıt katalog/yük ölçümü değildir. 0.7 pin upgrade, divergent digest, eksik timestamp reddi ve eski manual manifest üzerinden known timestamp pin'in geriye alınamaması da koşuldu. Runner kendi geçici dosyalarını temizler, `--output RAPOR.json` destekler.

Bu doğrulama aynı fiziksel makinede ayrı süreçlerde yapıldı; ikinci fiziksel host veya otomatik network failover sonucu olarak sunulmaz.
