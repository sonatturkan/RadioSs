# WAL arşivi ve belirli ana dönüş

SDBASE 0.7.0 / schema v4, kendi `.sdb` journal işlemlerini **checkpoint sonrasında da saklayan doğrulanabilir WAL arşivi** ve bu arşivden **işlem sırasına veya zamana göre gerçek geri dönüş** sağlar. SQLite/PostgreSQL çalıştırılmaz; SQL, harici veritabanı sunucusu ve bu ürünlerin dosya biçimleri kullanılmaz.

## Açık kaynaklardan uyarlanan işletim kuralları

Kaynak incelemesi yerel depolarda yapıldı. Aşağıdaki dosyalar tasarım örneğidir; yeni Python modüllerine C kodu kopyalanmadı.

| Kaynak ve sabit commit | İncelenen davranış | SDBASE karşılığı |
|---|---|---|
| [PostgreSQL pgarch.c, b69356cd](https://github.com/postgres/postgres/blob/b69356cd789fe963447177a0077c12c6c322c203/src/backend/postmaster/pgarch.c) | `pgarch_readyXlog` en eski bekleyen bölümü seçer; başarılı arşivden sonra tamamlandı işareti yazılır. | İşlem sırası atlanamaz; önce bölüm fsync edilir, sonra arşiv head'i atomik güncellenir. Head güncellemesi kesilirse mühürlenmiş kuyruk yeniden bulunur. |
| [PostgreSQL xlog.c, b69356cd](https://github.com/postgres/postgres/blob/b69356cd789fe963447177a0077c12c6c322c203/src/backend/access/transam/xlog.c) | `RemoveOldXlogFiles` ve `XLogArchiveCheckDone` eski WAL'ın geri dönüş için korunmasını denetler. | Journal, bütün committed işlemler arşive taşınmadan checkpoint sırasında küçültülemez. |
| [PostgreSQL xlogrecovery.c, b69356cd](https://github.com/postgres/postgres/blob/b69356cd789fe963447177a0077c12c6c322c203/src/backend/access/transam/xlogrecovery.c) | `recoveryStopsBefore` / `recoveryStopsAfter`, zaman ve inclusive sınırında durur; timeline kimliği ayrılır. | `target_sequence`, timezone içeren `target_time`, inclusive/exclusive seçim ve kaynak UUID/timeline/epoch doğrulaması. |
| [SQLite wal.c, 519e4c23](https://github.com/sqlite/sqlite/blob/519e4c23a58ad3fa78926b9df31c03ad55b73d42/src/wal.c) | Frame checksum, commit sınırı, checkpoint fsync sırası ve eski/yeni journal nesillerinin ayrılması. | Özgün `.sdb` frame SHA-256'sı, SHA ile zincirlenmiş arşiv bölümleri, fsync ve atomik yayınlama; önceki timeline'ın yeni yazıcıya bağlanmaması. |

PostgreSQL kaynaklarının lisansı yerel `PostgreSQL/COPYRIGHT` içindeki PostgreSQL lisansıdır. SQLite `src/wal.c` kamu malıdır; kapsam yerel `SQLite/LICENSE.md` içinde belirtilir. Bu sürüm bu dosyalardan kod veya uzun açıklama taşımadığından ürünün çalışma zamanı bunlara bağımlı değildir.

## Başlatma ve dosyalar

```sh
sdbase --db /veri/RadioS.sdb wal-enable
sdbase --db /veri/RadioS.sdb backup-create --destination /yedek/base-2026-10-01
sdbase --db /veri/RadioS.sdb wal-status
```

Arşiv açıldıktan **sonra** alınmış doğrulanmış fiziksel yedek gerekir. Yedekte `.meta` kaynak kimliği ve `wal_anchor` zincir özeti bulunur. Arşiv açılmadan önceki işlemler yeniden oluşturulmaz.

```text
RadioS.sdb.wal/
  header.json                 kaynak UUID, timeline, epoch, schema, başlangıç sırası
  head.json                   son mühürlenmiş işlem sırası, SHA, effective_at
  records/00000000000000000042.json
  assets/ab/ab...              SHA ile adreslenmiş medya/soğuk arşiv dosyası
```

Her normal commit ayrı, değiştirilmeyen bir JSON bölümüdür. Bölüm özgün transaction gövdesini, önceki bölümün SHA'sını, kaynak kimliğini ve o işlemle gerekli hâle gelen medya/soğuk dosyalarının SHA/boyutlarını içerir. Transaction sınırını bölmek mümkün değildir. Yeni medya, ilk referans verildiğinde arşive kopyalanır; temel yedekten sonra eklenen medya da kaynak makine kaybedildiğinde kurtarılabilir.

`.meta.wal_archive` yalnız arşiv UUID'si ve başlangıç sırasını tutar. Yazıcı rolü, node UUID, timeline ve epoch ayrıca denetlenir. Salt okunur standby ve eski epoch ile açık kalmış yazıcı arşivi açamaz veya ilerletemez.

## Commit, checkpoint ve hata sonucu

Normal yazma sırası şöyledir:

1. Veriyi ve revision/index koşullarını doğrula.
2. Asıl journal frame'ini yaz ve fsync et.
3. Veritabanı state'ine uygula.
4. Gerekli immutable dosyaları arşivle; bölüm ve head'i kalıcı yaz.
5. İstemciye başarı dön.

Arşivleme dördüncü adımda başarısız olursa **işlem zaten committed olabilir**. `ArchiveAcknowledgementError` kaynak transaction UUID'sini ve sequence değerini verir. İşlemi körlemesine tekrar etmeyin; sonucu okuyup `wal-capture` ile arşivi tamamlayın. Bir sonraki yazma veya checkpoint önce kalıcı journal'daki eksik arşiv işlemlerini tamamlar; tamamlayamazsa eski journal korunur.

Retention normal transaction frame'i yerine atomik checkpoint ile commit eder. Bu nedenle WAL'da ayrıca **tam state/checkpoint bölümü** vardır: sıcak state, tombstone'lar, emekli unique anahtar hash'leri, koleksiyon sequence'leri ve gerekli soğuk segment dosyaları birlikte korunur. Retention checkpoint'i kalıcı olduktan sonra, journal küçültülmeden önce bu bölüm arşivlenir. Bu arada süreç kapanırsa yeni checkpoint ve kalan eski journal üzerinden bölüm yeniden oluşturulur.

Arşiv bölümü mühürlenmeden kalmış `.partial` dosyası recovery işlemine katılmaz. Mühürlenmiş bölümün eksik/truncated/bozuk olması, sıra boşluğu, başka timeline veya eksik medya/soğuk dosyası hatadır. SHA zinciri kazara bozulmayı belirler; yetkisiz bir kişinin bütün zinciri ve yedeği yeniden yazmasına karşı dijital imza değildir.

## Geri dönüş

```sh
sdbase pitr-preview --backup /yedek/base-2026-10-01 \
  --wal-dir /yedek/RadioS.sdb.wal --target-sequence 42

sdbase pitr-restore --backup /yedek/base-2026-10-01 \
  --wal-dir /yedek/RadioS.sdb.wal --target-sequence 42 \
  --target-dir /kurtarma/RadioS-42

sdbase pitr-preview --backup /yedek/base-2026-10-01 \
  --wal-dir /yedek/RadioS.sdb.wal \
  --target-time '2026-10-01T12:00:00+00:00'
```

Tam bir target zorunludur; sequence ve time birlikte verilmez. Zaman varsayılan olarak inclusive'dir; `--exclusive` eşit zamanlı işlemin öncesinde durur. İki commit arasındaki zaman önceki tamamlanmış işlem sırasını seçer. Arşivin kanıtlayabildiği son zamanın ilerisindeki hedef ve arşiv sonunun ilerisindeki sequence reddedilir.

`effective_at`, kaynak saati geriye gitse bile her sequence'de en az bir mikrosaniye ilerleyen UTC commit zamanıdır. Özgün journal `committed_at` değeri ayrıca korunur. Saat geri alınan kurulumda hassas tarih seçimi için effective zamanı okuyun veya doğrudan sequence kullanın.

Recovery temel yedeği ve bütün zinciri doğrular, yalnız seçilen aralığı uygular, medya/soğuk dosyaları doğrular, staging dizinini tam şemayla açar ve **henüz bulunmayan hedef dizini atomik yayımlar**. Kaynak değişmez ve mevcut hedef üzerine yazılmaz. Eski schema için o schema'yı destekleyen paketle recovery yapılıp kurtarılan kopya migrate edilir.

Kurtarılan düğüm, kaynak makinenin canlı sahipliğini devralmaz: endpoint'ler offline, kopyalar pending, active acceptance'lar inactive, pending engine komutları expired olur. Eski kullanıcı ve endpoint token'ları iptal edilir; yeni bağımsız store/node/timeline kimliği ve yeni admin token'ı döner. Token'ı bir kez güvenli saklayın. Yayın tekrar onaylanıp medya/uç sahipliği yeniden doğrulanmalıdır.

Sonuçtaki `recovered_sequence` hedef kaynak sırasıdır. Fencing ve yeni admin oluşturma da kayıt yazdığı için `restored_sequence` daha büyük olabilir. Bu iki sayı birbirine karıştırılmamalıdır.

Python karşılıkları `wal_archive.enable_wal_archive`, `archive_status`, `capture_archive` ve `pitr.preview_recovery` / `recover_point_in_time` fonksiyonlarıdır. Recovery'ye `expected_preview_sha256` verilerek uygulanacak girişler okunmuş preview'ye sabitlenebilir.

## Doğrulama ve işletim sınırı

```sh
PYTHONDONTWRITEBYTECODE=1 python tools/recovery_acceptance.py --output /tmp/recovery-result.json
```

Araç üretim veritabanı yolu almaz. Geçici dizinlerde gerçek child process'i journal fsync sonrası ve retention checkpoint commit sonrası sonlandırır; checkpoint, stale head, truncated/missing/tampered bölüm, eksik dosya, kesin zaman/sıra, kaynak değişmezliği, kaynak tamamen silindikten sonra medya+soğuk geçmiş recovery ve credential fencing senaryolarını doğrular. Kaynaklar ve fixture'lar çalışma sonunda kaldırılır. Doğrulama **aynı makinede** yapılır; elektrik kesintisi, fiziksel ikinci makine ve ses cihazı testi değildir.

2026-10-01 çalıştırması: **35/35 kontrol geçti**, toplam 0,255 saniye. Bu süre küçük kabul fixture'ının süresidir; büyük veritabanı recovery süresi veya kapasite ölçümü değildir.

Arşiv aynı diskte tutuluyorsa disk kaybı temel yedek ve arşivi birlikte yok edebilir. PITR için ikisi de başka güvenilir depoya aktarılmalıdır. Bu sürüm uzaktaki depoya fsync alındığını istemciye kanıtlamaz; yerel arşiv başarıyla yazılmadan başarı dönmez. Uzak kopyanın sıklığı saha RPO'sunu belirler.

Arşiv bölümleri otomatik silinmez ve her commit için dosya/inode maliyeti vardır. Temel yedeği, ona bağlı gerekli zinciri ve bütün dosyaları doğrulamadan manuel silme yapmayın. Bu sürüm WAL sıkıştırması, archive GC, otomatik çok makineli failover veya quorum protokolü iddia etmez. Aynı bellek state'ini kullanan store'un terabayt ölçeğine uygun olduğu sonucu bu özellikten çıkarılamaz.

Tam WAL doğrulaması ve PITR ilgili kayıtların bütün JSON payload'larını belleğe yükler; kayıt başına sabit bellek/paged recovery garantisi yoktur. Network shipment'ın seçilmiş record/`records.json` için 256 MiB sınırı bu global okuyucuların sınırı değildir. HTTP follower'ın daha önce doğrulanmış büyük predecessor'ı SHA/timestamp pin ile sabit bellekle geçebilmesi ayrı bir özelliktir; büyük yeni checkpoint'in aktarım veya tam recovery maliyetini ortadan kaldırmaz. Ayrıntılar: [replication_stream.md](replication_stream.md).
