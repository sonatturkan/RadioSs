# Otomatik, yerel fencing ile servis devri

SDBASE 0.8.0, primary HTTP servisi kapanınca doğrulanmış standby'ı otomatik olarak yeni primary yapabilen `FailoverSupervisor` sağlar. Eski primary'ın **bilinen `.sdb` dosyası, metadata'sı ve canonical lock'u erişilebilir kalmalıdır**. Karar ağ bölümlemesinde tahmine dayanmaz: kaynak dosya veya kilidi erişilemiyorsa otomatik devir reddedilir.

Tasarım, yerel PostgreSQL deposunun `doc/src/sgml/high-availability.sgml`, `warm-standby-failover` bölümünde açıklanan eski primary'ı durdurma/sahipliğini kaldırma ve yeniden kullanılabilir standby hazırlama ilkelerinden bağımsız olarak uyarlandı. Sabit kaynak commit'i `b69356cd789fe963447177a0077c12c6c322c203`; kod kopyalanmadı. Güncel [PostgreSQL failover kılavuzu](https://www.postgresql.org/docs/current/warm-standby-failover.html) da yeniden başlayan eski primary'ın ikinci yazıcı olmamasını ve ayrı failure detection yazılımını ele alır. SDBASE bunu kendi dosya lock'u, node UUID, timeline ve epoch yazma kapılarıyla uygular.

## Karar ve kalıcılık

Supervisor ilk kurulumda kaynak/standby dosya yollarını, kaynak URL'sini, node/store UUID'lerini, timeline, epoch ve policy'yi sabitler. Kaynak HTTP `/health` cevabında bunların eşleştiğini **en az bir kez doğrulamadan** otomatik devri etkinleştirmez. Yanlış URL'ye yönlendirilmiş servis, başka node cevabı veya eksik kimlik alanı normal outage sayılmaz; operator reset gerektiren blocked durumu olur.

Varsayılan policy:

| Alan | Varsayılan | Etkisi |
|---|---:|---|
| `failure_threshold` | 3 | Ardışık başarısız sağlık probu gerekir. Sağlıklı cevap sayacı sıfırlar. |
| `probe_timeout_seconds` | 1 | Tek HTTP sağlık denemesinin socket timeout'u. Otomatik retry yapılmaz. |
| `probe_interval_seconds` | 2 | Prob aralığı; tick'i daha sık çağırmak sayacı hızlandırmaz. |
| `cooldown_seconds` | 300 | Operator reset sonrasında yeniden devir için bekleme. |
| `catchup_max_batches` | 8 | Bir tick'teki sınırlı replay işi. Kalan çalışma sonraki tick'te sürer. |
| `catchup_max_records` | 1000 | Her replay bölümündeki en fazla işlem. |

Eşik geçilince supervisor kaynak recovery archive'ını tamamlar, standby'ı kalıcı ve hash doğrulanmış shipment'larla yakalar, primary sağlık probunu yeniden yapar. Servis bu sırada geri geldiyse devir iptal edilir. Ardından `promote_standby` her iki canonical lock altında source sequence, tam mantıksal state, archive anchor ve node kimliklerini tekrar doğrular.

**Önce eski primary fenced olur; sonra yeni primary yayınlanır.** Devir kaynakla birebir eşit standby gerektirir. Kaçırılan committed işlem, bozuk arşiv, başka timeline, veri/lock dosyası alias'ı veya erişilemeyen kaynak otomatik promote edilmez. Kaynak HTTP kapalı olsa da başka bir yerel işlem yazmaya devam ediyorsa son sequence değişimi devri reddedebilir; eski primary fenced olmadıkça supervisor eski bir niyeti iptal edip yeniden dener.

`supervisor.json` checksum içeren kalıcı iş state'idir; `supervisor.lock` karar işlemlerini seri tutar. `run()` ve `watch()` ayrıca probe aralarında da `runner.lock` tutar; aynı işe ikinci sürekli monitor katılamaz. Process kapanınca kernel lock'u bırakır, yeniden başlatma lock dosyası silmeyi gerektirmez. Her shipment uygulanmadan önce adı state'e yazılır. Kesilmiş standby journal'ı yalnız bu doğrulanmış kalıcı shipment'ın exact pending frame önekiyle onarılır. Fencing sonrası supervisor kapanırsa `promotion_pending` ve veritabanındaki handover intent birlikte yeniden açılışta tamamlanır. Eski primary otomatik olarak yeniden writable yapılmaz.

## Kullanım

Primary ve standby önce mevcut replication yordamıyla hazırlanmalıdır. Supervisor state dizini iki veritabanının kök dizininin dışında olmalıdır.

```python
from threading import Event
from sdbase.failover import FailoverPolicy, FailoverSupervisor

supervisor = FailoverSupervisor(
    "/primary/RadioS.sdb",
    "/standby/RadioS.sdb",
    "http://127.0.0.1:8000",
    "/supervisor/radio-job",
    policy=FailoverPolicy(),
)

decision = supervisor.tick()
print(decision["state"], decision["active_database"])

# Uzun süre çalışan servis bu döngüyü kullanabilir. Kapanışta event set edilir.
stop = Event()
supervisor.run(stop)
```

`watch(stop)` aynı döngüde her kalıcı kararı yield eder; erken çıkışta generator `close()` edilmelidir. `status()` kalıcı kararı okur, mevcut node'un canlı sağlık/ownership kanıtı değildir. `reset()` yalnız başarısız health sayacı ve yanlış servis kimliği engelini sıfırlar; yeni cooldown ve kimlik doğrulama gerekir. Tamamlanan devir veya bekleyen handover/replay reset edilemez. Yeni HA çifti için yeni standby ve ayrı supervisor işi hazırlanır.

Başarıdan sonra `active_database` standby dosyasını gösterir. Hazır router kalıcı kararı okur, hedef metadata'sındaki primary rolü/node UUID/timeline/epoch'u bağımsız doğrular ve yeni store handle açar. `status()` tek başına bu doğrulamanın yerini almaz. Eski standby handle'ı eski epoch'a bağlı olduğu için yazamaz. Supervisor shell komutu çalıştırmaz, keyfi bir servis başlatmaz ve IP taşımaz. Sabit API adresi bu doğrulanmış seçime bağlanır.

Hazır sabit listener `failover-serve` komutudur. Kaynak API ve router farklı portlarda çalıştırılır; probe URL'si router'ı göstermemelidir.

```sh
sdbase --db /primary/RadioS.sdb failover-serve \
  --standby-db /standby/RadioS.sdb --source-url http://127.0.0.1:8001 \
  --state-dir /supervisor/radio-job --host 127.0.0.1 --port 8000
```

Listener her isteği bir store'a sabitler; aynı keep-alive socket'in sonraki isteği devir tamamlandıktan sonra yeni node'a gider. Route kararı ayrıca rol/node/timeline/epoch ile doğrulanır. Yeni kaynağın bu kimliği kaybolursa 503 döner. Fenced eski listener'ın endpoint state/snapshot/commands GET uçları da 503 verir; eski token ve active acceptance'lar eski veritabanında hâlâ bulunabilse bile çalıştırma yetkisi üretmez. `failover-watch` yalnız monitor döngüsünü, `failover-tick/status/reset` tek işlem yönetimini sağlar.

Yeni primary'da endpoint'ler offline, medya kopyaları pending ve active acceptance'lar inactive olur; eski endpoint token'ları ve bekleyen komutlar fence edilir. Bu veri servisi devri fiziksel yayını kendiliğinden sürdürdüğü anlamına gelmez. Yayın yeniden yetkilendirilmeli ve medya/uç sahipliği doğrulanmalıdır.

## Kabul ve sınırlar

```sh
PYTHONDONTWRITEBYTECODE=1 python tools/failover_acceptance.py --output /tmp/failover-result.json
```

2026-10-01 aynı makine kabulü: **36/36 kontrol geçti**, 0,744 saniye. Gerçek primary HTTP child process'i öldürüldü; health aralıkları deterministik saatle ilerletildi. Geçici dosyalarda lag catch-up, ardışık failure eşikleri, sağlıklı servisin geri gelişi, yabancı sağlık kimliği, erişilemeyen source, fencing sonrası kesinti ve supervisor restart, eski yazıcı reddi ve yeni node yazması doğrulandı. Gerçek onaylı macro planı dağıtılıp kabul edildi; bekleyen engine komutu olan fenced kaynağın endpoint GET'leri reddedildi. Aynı keep-alive socket üzerinde yeni node/epoch seçimi, admin create/detail ve yabancı routing timeline'ının reddi de sınandı. İkinci sürekli monitor reddi ve stop sonrasında yeni monitor'un işi devralması doğrulandı. Fixture'lar kaldırılır.

Bu mekanizma erişilebilir yerel dosyalardaki servis arızasına otomatik cevap verir. Ağ partition'ında bağımsız makinelerden birini quorum/witness/lease ile seçmez; kaynak diskin tümü kaybolduğunda promote zorlanmaz. HTTP probe timeout'u toplam dosya kilidi/I/O süresini belirlemez; store lock timeout ve bounded replay ayrıca işler. Supervisor policy ve job state'i başka bir kullanıcı tarafından değiştirilemeyecek izinlerle yönetilmelidir. Fiziksel ikinci makine, elektrik kesintisi, yayındaki ses ve OS servis yöneticisi tatbikatı ayrıca gerekir.
