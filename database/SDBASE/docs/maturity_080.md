# SDBASE 0.8.0: dağıtım, devir, disk indeksi ve çevrimiçi bakım

Karşılaştırmadaki uygulanabilir motor eksikleri SDBASE'in kendi `.sdb` sözleşmesine uyarlandı. Ayrı Python modülleri vardır; SQL parser veya başka DB motoru bağımlılığı eklenmez. Schema **v4** korunur; 0.7.0 dosyası yeni migration istemez. v1/v2/v3 için mevcut güvenli kopya migration gerekir.

## Talep → uygulama → sınır

| Talep | 0.8.0 uygulaması | Ölçülebilir sınır |
|---|---|---|
| Site/veri çoğaltma | Ayrı authenticated TLS listener, sürekli WAL follower, kaynak kimliği/SHA zinciri, medya/cold asset aktarımı, restart/Range resume. | Asenkron tek primary; senkron uzak commit veya çok yazıcılı site merge yok. |
| Automatic failover | Kalıcı failure policy/supervisor; suffix catchup, erişilebilir kaynak fencing, yeni timeline/epoch, sabit HTTP listener'da sonraki isteğin yeni node'a geçişi. | Eski kaynak dosyaları/canonical lock erişilebilir olmalı. Quorum, donanım fencing ve erişilemeyen kaynağı zorla devralma yok. |
| PITR | Kesin sequence/effective UTC hedefi, temel yedek anchor'ı ve medya/cold history güvencesi korunur. Yeni checkpoint/aktarımla birlikte doğrulanır. | Doğrulanmış temel yedek ve WAL gerekir; bağımsız yedek konumu operatöre bağlıdır. |
| Büyük sıralı sorgular | Generation-bound ordered disk sidecar, bounded external merge, mmap/bisection, gerçek disk query yolu, checksum ve fallback. | Belgeler/hash indeksleri RAM'dedir; genel B-tree/GIN/GiST veya terabayt engine değildir. |
| Eşzamanlı istemciler | Snapshot/CAS, bounded HTTP/pool/lock; disk query bütçeleri ve online bakım sırasında concurrent writer ölçümü. | Commit tek store yazma kilidiyle sıralanır; PostgreSQL tuple MVCC/paralel commit uygulanmadı. |
| Online maintenance | Büyük checkpoint hazırlığı/fsync kilit dışında, bounded suffix/publication exclusive kilitte; retention supersede fence ve crash cleanup. | Capture RAM map kopyasıdır; publication sıfır süre değildir. Tüm retention compaction/WAL GC çevrimiçi yapılmaz. |
| Production doğrulaması | İzole kabul araçları; gerçek süreç kapatma/ağ kesintisi, TLS, cached/fresh reader, gerçek HTTP devir ve sentetik kapasite ölçümü. | Fiziksel ikinci makine, cihaz çıkışı, günlerce soak ve yıllar production geçmişi kod eklemekle sağlanamaz. |

## Ayrı dosyalar

- `replication_stream.py`: listener, follower, durable transfer ve indirme/replay.
- `failover.py`: kalıcı karar, health kimliği ve reachable-source fencing.
- `failover_server.py`: istek başına tek node seçimi ve mevcut authenticated API.
- `disk_index.py`: sidecar biçimi, external sort, generation publication, mmap ve recovery.
- `online_maintenance.py`: captured checkpoint, bounded suffix ve owned cleanup.
- `query.py`, `read_snapshot.py`, `storage.py`, `http_server.py`, `cli.py`: engine/API/komut bağlantıları.

Kurulum: [ağ çoğaltma](replication_stream.md), [otomatik devir](automatic_failover.md), [disk indeksleri](disk_indexes.md), [online checkpoint](online_maintenance.md). [0.7.0 motor raporu](storage_maturity.md) tarihsel temel sürümün kanıtını içerir.

## İncelenen açık kaynaklar

Yerel PostgreSQL **20devel** commit `b69356cd789fe963447177a0077c12c6c322c203` ve SQLite **3.54.0** commit `519e4c23a58ad3fa78926b9df31c03ad55b73d42` incelendi. Yeni kod SDBASE'in sequence, revision, immutable plan ve endpoint ownership sözleşmelerini kullanır.

| Kaynak | İncelenen yöntem | Uyarlama |
|---|---|---|
| PostgreSQL `replication/walreceiver.c`, `access/transam/xlogrecovery.c` | Yeniden bağlanan recovery ve kaynak/timeline doğrulama. | HTTP/TLS batch ve SHA-pinned `.sdb.wal`; verified asset/durable replay. |
| PostgreSQL `doc/src/sgml/high-availability.sgml` | Yeni primary öncesinde eski primary'nin yazmasını durdurma, dış failure detector. | Reachable `.sdb`/lock fencing, kalıcı supervisor ve API yönlendirme. |
| PostgreSQL `access/nbtree/nbtsort.c`, SQLite `src/vdbesort.c` | Sıralanmış giriş ve bounded external merge. | JSONL key/ID ve binary offset sidecar; mmap/bisection, immutable generation. |
| PostgreSQL `access/transam/xlog.c`, SQLite `src/wal.c` | Recovery başlangıcı ve journal/checkpoint durability sırası. | Capture N ve N+1…M suffix; checksum/fsync/atomic replace, cold history fence. |

Vendor kaynakları yerinde okundu, değiştirilmedi; C kodu birebir taşınmadı. Kapalı Zetta/ENCO/WideOrbit kaynak kodları mevcut değildir; karşılaştırmadaki talepler uygulanmıştır. Radyo ürünleriyle format uyumu [adaptör matrisinin](adapter_coverage.md) kapsamındadır.

Birincil belgeler: [PostgreSQL failover](https://www.postgresql.org/docs/current/warm-standby-failover.html), [standby/streaming](https://www.postgresql.org/docs/current/warm-standby.html), [SQLite online backup](https://www.sqlite.org/backup.html). Olgunluk puanları tekrar edilmez; kendi kodumuz ve ölçülen sınırları değerlendirilir.

## Tekrarlanabilir kabul

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tools/recovery_acceptance.py
PYTHONDONTWRITEBYTECODE=1 python3 tools/replication_acceptance.py
PYTHONDONTWRITEBYTECODE=1 python3 tools/query_integrity_acceptance.py
PYTHONDONTWRITEBYTECODE=1 python3 tools/engine_acceptance.py
PYTHONDONTWRITEBYTECODE=1 python3 tools/acceptance.py --offline-seconds 0
PYTHONDONTWRITEBYTECODE=1 python3 tools/stream_acceptance.py
PYTHONDONTWRITEBYTECODE=1 python3 tools/failover_acceptance.py
PYTHONDONTWRITEBYTECODE=1 python3 tools/disk_index_acceptance.py
PYTHONDONTWRITEBYTECODE=1 python3 tools/maintenance_acceptance.py
```

Araçlar yalnız kendi TemporaryDirectory alanlarında çalışır; üretim DB yolu almaz. Bozuk kaynak, gerçek process kill ve kontrollü fault injection senaryoları kullanılır. Acceptance içinde küçük fixture süresi gerçek saha RTO/RPO bütçesi olarak sunulmaz. `--offline-seconds 0` çok günlük duvar saati offline testinin yerini almaz; bu koşu sanal hizmet günü/offline/backup-only örnek PCM zincirini doğrular.

## Son kapasite koşusu: 2026-10-01

Son kaynakta **331/331 kabul kontrolü geçti**:

| Araç | Kontrol | Kanıt kapsamı |
|---|---:|---|
| `recovery_acceptance.py` | 35 | Kesin WAL/PITR, kaynak kaybı, medya/cold history ve gerçek crash. |
| `replication_acceptance.py` | 35 | Physical base, artımlı paket, gap/tamper, kopya/replay/promotion crash ve eski writer fence. |
| `query_integrity_acceptance.py` | 33 | Snapshot/query/keyset/nullable, domain yetkisi, bozuk DB/medya/cold veri ve scrub bütçesi. |
| `engine_acceptance.py` | 19 | Bounded HTTP/pool/lock, gövde framing, committed-unack ve legacy migration. |
| `acceptance.py --offline-seconds 0` | 37 | Ayrı merkez/uç HTTP süreçlerinde örnek PCM cue/transition/makro/stream/event ve backup-only yeniden yayın. |
| `stream_acceptance.py` | 53 | TLS/auth, worker/network restart, Range, identity/time/hash, bounded cache/history, 0.7 upgrade, büyük predecessor. |
| `failover_acceptance.py` | 36 | Gerçek backend kill, eski source fencing, crash recovery, aynı keepalive'da yeni node ve tek monitor. |
| `disk_index_acceptance.py` | 37 | External merge, mmap/query/budget, stale/corrupt/alias/malformed cache, build crash/recovery. |
| `maintenance_acceptance.py` | 46 | Concurrent bağımsız writer, üç publication crash sınırı, WAL/PITR/standby, retention supersede ve owned cleanup. |

Ek bağımsız incelemelerde online bakım 26, CLI entegrasyonu 43, üç disk hatasının tekrarları 3 ve yeni anchor/legacy timestamp yolu 7 kontrolle doğrulandı. Bunlar 331 toplamına eklenmedi. AST sözdizimi, sürüm/OpenAPI alanları ve yerel belge bağlantıları doğrulandı. Kaynak modüllerinde SQL driver import'u yok; 93 kaynak modülü ayrı dosyalardadır. Kaynak klasöründe fixture `.sdb`, WAV, `.pyc` veya test klasörü bırakılmadı.

Stream koşusunda **260 MiB canonical checkpoint fixture** SHA/timestamp için bir kez akış halinde doğrulandı; traced ek heap 4 MiB altında kaldı. Yeni physical base bu timestamp'i taşıdı. Sonraki **64 KiB gerçek HTTP batch**, worker'da predecessor JSON load ve stamp rescan yasakken ilerledi. Bu metadata byte sınırı deneyidir; 260 MiB gerçek radyo kataloğu veya bütün motorun 4 MiB RAM kullandığı iddiası değildir. Seçilen yeni record/`records.json` için 256 MiB paket sınırı sürer; global WAL verify/PITR tüm JSON payload'larını RAM'e yükler.

### Ölçüm parametreleri

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tools/benchmark.py \
  --items 100000 --updates 80 --searches 40 --concurrency 8 \
  --stations 4 --api-users 8 --seed-batch-size 500 \
  --operational-attempts 10000 --http-requests 48 --http-rounds 2 \
  --disk-indexes --online-checkpoint --progress
```

100.000 içerik × dört metadata belgesi, 10.000 attempt, 20.000 event ve 10.000 observation: **440.000 sentetik domain kaydı**. Audit ve küçük bootstrap/plan kayıtları sayıya dahil değildir. Gerçek medya baytları üretilmedi. Altı yıla yayılmış log timestamp'leri altı yıl çalışma anlamına gelmez. Seed transaction başına 500 içerik/attempt kullanıldı; tek kayıt başına 440.000 fsync ölçümü değildir.

| Ölçüm | Sonuç |
|---|---:|
| Dört istasyon, sekiz user/worker, gerçek HTTP | **128/128 HTTP 200**, retry 0 |
| HTTP request p95 | **2.588,152 ms** |
| 100.000 üyelikli disk indeksi hazırlama | **1.338,893 ms** |
| 20 satırlık disk range cold query | **448,833 ms** |
| Aynı görüntüde doğrulaması hazır warm query | **0,987 ms** |
| Online checkpoint hazırlığı | **10.563,663 ms**, kilit dışında |
| Capture shared kilit süresi | **12,311 ms** |
| Final publication exclusive kilit süresi | **10,940 ms** |
| Concurrent bakım yazmaları | **32/32**; p95 **1.915,472 ms** |
| Capture sonrası korunan suffix | **18 transaction**, 17.406 bayt |
| Fresh engine reopen | **5.006,729 ms** |
| Koşunun tamamında tepe RSS | **2,10 GiB**, backup verification dahil |

Cold indeks yolu checksum ve source üyeliği/sırası için 100.000 kaydı doğruladı. Warm query doğrulamayı tekrarlamadan 74 key record/148 offset probe okudu, yalnız 20 domain satırını getirdi; RAM'de sorted key dizisi kurmadı. `max_scan=max_index_keys=max_candidates=64` ile 25.000 adaylı hash bucket allocation'dan önce reddedildi; doğru sonuç küçük disk range yolundan geldi. Bu local document query ölçümüdür; bütün catalog/API aramalarının 1 ms olduğu anlamına gelmez.

Online checkpoint **N=422** görüntüsünü yayımlarken **M=440** suffix'ini korudu. Fresh reopen aynı M'yi okudu. CPU/GIL/disk rekabeti sürer; concurrent writer p95 bunu gösterir. HTTP p95 bu donanım/yükte 2,59 saniyedir; kullanıcı gecikme bütçesi olarak otomatik kabul edilmez.

`.sdb`, sidecar, backup ve log fixture'ları kaldırıldı. Bu ölçüm WAL açıkken aynı ölçek/replication throughput veya fiziksel ikinci makine sonucunu kanıtlamaz. Önceki büyük 0.6.0 ölçümü [yük raporunda](load_verification.md) tarihsel olarak tutulur.
