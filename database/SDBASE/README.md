# RadioS SDBASE

RadioS için sıfırdan geliştirilen, SQL kullanmayan bağımsız veritabanı ve veri yönetim katmanı. Ana dosya uzantısı **`.sdb`**; örnek ad `RadioS.sdb`. Açık kaynak veritabanı kaynak klasörleri değiştirilmez. Python 3.11+ gerekir; çalışma zamanı için üçüncü taraf paket zorunlu değildir.

**Nihai düzeltme sürümü 0.8.1 · veri şeması v4 · 95 iş koleksiyonu.** 0.7.0/0.8.0 dosyaları doğrudan kullanılabilir. Eski v1/v2/v3 dosyası doğrudan açılmaz; kaynağı değiştirmeyen `migrate-preview`, `migrate-copy` veya `migrate-backup` yordamını [göç belgesine](docs/schema_migration.md) göre çalıştırın.

0.8.1, temiz kurulum denetiminde bulunan boş indeks durum raporu hatasını giderir. Son kabul, paketleme ve kaynak kimliği [nihai sürüm raporundadır](docs/finalization_081.md).

0.8.0; [TLS ile sürekli ağ çoğaltma](docs/replication_stream.md), [erişilebilir eski kaynağı durdurarak otomatik devir ve sabit API adresi](docs/automatic_failover.md), [sorguya bağlı sıralı disk indeksleri](docs/disk_indexes.md) ve [yazmalar sürerken checkpoint hazırlama](docs/online_maintenance.md) ekler. Kaynak yöntemleri, uygulanan karşılıklar, ölçümler ve sınırlar [0.8.0 raporunda](docs/maturity_080.md) açıklanır.

0.7.0, olgun açık kaynak motorlarda kullanılan yöntemleri bağımsız `.sdb` biçimine uyarlar: [tutarlı okuma ve indeksli sorgu planı](docs/snapshot_queries.md), [WAL arşivi ve zamana/sıraya geri dönüş](docs/wal_recovery.md), [salt okunur replika ve kontrollü devir](docs/replication.md), [sınırlı HTTP bağlantıları, bağlantı havuzu ve izleme](docs/engine_operations.md), [salt okunur bütünlük taraması](docs/integrity.md). Kaynak kökeni, karşılaştırma ve ölçülebilir sınırlar [motor olgunluğu belgesindedir](docs/storage_maturity.md).

0.6.0, belirsiz `default` çıkışını yayına kapatır; çıkış seçimini ve onaylı cue/segue/fade bilgilerini plan revizyonuna sabitler. Item/version/plan/entry/istasyon ilişkisi ve revision okumaları tamamlanmıştır. Doğrulanmış [retention ve soğuk geçmiş](docs/retention.md), [örnek oynatıcı ve kurtarma kabulü](docs/production_acceptance.md), [çok istasyonlu yük ölçümü](docs/load_verification.md) ve [adaptör kapsam matrisi](docs/adapter_coverage.md) ayrı belgelenmiştir.

## Kurulum

```sh
python3 -m pip install -e ./SDBASE
sdbase --db /veri/RadioS.sdb init --username admin --station-code RAD --station-name RadioS --timezone Europe/Istanbul
sdbase --db /veri/RadioS.sdb serve --host 127.0.0.1 --port 8000
```

`init` yalnız ilk kullanıcı/istasyon/uç için çalışır ve kısa ömürlü yönetici tokenını bir kez gösterir. Uç bilgisayara ayrı token verilir:

```sh
sdbase --db /veri/RadioS.sdb endpoint-token --token YONETICI_TOKENI --station-id ISTASYON_UUID --endpoint-id UC_UUID
```

Uzak HTTP bağlantısı için TLS sonlandırması gerekir. Yerel API örneği `http://127.0.0.1:8000` adresindedir. API uç ajanı [taşıma sözleşmesini](docs/api.md) uygular. Uzman veri işleri kimlik doğrulamalı ve izinli `POST /v1/workflows/{domain}/{action}` yollarından çağrılır; örneğin `clocks/preview`, `traffic/propose`, `now-playing/current`. Bu yollar koleksiyonlara ham yazma sağlamaz. `audio_playout.py`, kabul edilmiş uç planındaki desteklenen PCM16 WAV seslerini tek WAV dosyasında yürütür; fiziksel ses kartı ve canlı yayın çıkışı ayrı yürütücü ister.

0.5.0, yeniden kullanılabilir sürümlü playlist, statik/dinamik smart block, RSS/Atom podcast eşitleme/otomatik taslak indirme, playlistte son onaylı bölümü çözümleme ve mount başına dinleyici zaman serisini ekler. Onaylı ses/stream planı LibreTime playout API verisine kalıcı kimliklerle dönüştürülebilir. Akışlar, kullanım ve sınırlar [yeni özellikler belgesindedir](docs/new_features.md).

Bağımsız kurulum kopyası `sdbase --db /veri/RadioS.sdb provision-copy --target-dir /yeni/kurulum` ile oluşturulur. Kopyanın depo kimliği ve yönetici tokenı yenidir; kopyada eski tokenlar ve yayın kabulü devre dışıdır. Tarihsel mAirList 3.1 CSV için `mairlist31-preview` ve `mairlist31-apply` CLI işlemleri vardır. Uygulama yalnız çakışmasız kayıtları taslak ve yayına uygunsuz olarak alır; ses ve cue incelemesini otomatik onaylamaz. Biçim ve sınırlar [mAirList belgesinde](docs/mairlist31_csv.md) açıklanır.

İstasyon altında `broadcast_services` ile her servisi bir uca ve çıkışa bağlayın; aynı yerel gün için servis başına ayrı plan, tekrarlı program ve saat ataması oluşturulabilir. Servis oluşturma `POST /v1/workflows/services/create`, listeleme `services/list`; plan oluştururken isteğe bağlı `service_key` verilir. `default` taslak akışında kullanılabilir; onay ve dağıtım için tek, somut, etkin çıkışa çözülmesi gerekir. Bölgesel reklamın gerçek eşzamanlı ses yürütmesi için ayrıca uygun playout motoru gerekir.

Belgelenmiş düz RadioBOSS M3U8 biçimi için `radioboss-preview`, `radioboss-apply` ve `radioboss-export` CLI işlemleri vardır; açık dosya yolu eşlemesi ve kayıp raporu kullanılır. [Adaptör belgesi](docs/radioboss_m3u8.md) kaynak kimliği, hash ve desteklenmeyen playlist komutlarını açıklar. İşlem kanıtı arşivi ve depo sağlık ölçümü [operatör aracında](docs/ops_archive.md), doğrulanmış düzenli yedek/arşiv ve dağıtım alarmı [zamanlayıcıda](docs/ops_scheduler.md) bulunur. HTTP sözleşmesi yetkili `GET /v1/openapi.json` yolunda yayımlanır.

Genel JSONL/CSV veri taşınması için `portable-export`, `portable-verify` ve `portable-restore` komutları vardır. [Paket biçimi](docs/portable_export.md) kimlik, ilişki, revizyon, silinmiş kimlik izi ve medya manifestini taşır. Ses baytları ile eski token/audit geçmişi paket dışında kalır; geri yükleme yeni depo kimliği ve yönetici tokenıyla canlı yayın sahipliğini güvenli duruma düşürür. Sesli kurtarma için fiziksel medya yedeği ayrıca gerekir.

## Kaynak düzeni

| Modül | İşlev |
|---|---|
| `storage.py`, `schema.py` | `.sdb` işlem günlüğü, kontrol noktası, revizyon, indeks ve ilişki kuralları. |
| `read_snapshot.py`, `query.py` | Tek commit'ten tutarlı okuma, indeks kesişimi/aralık, keyset sayfalama ve sorgu planı/iş ölçümü. |
| `wal_archive.py`, `pitr.py`, `replication.py` | Kalıcı işlem arşivi, kesin sequence/zaman kurtarması, doğrulanmış replika aktarımı ve kaynak fencing ile kontrollü devir. |
| `api_server.py`, `http_pool.py`, `engine_metrics.py`, `monitoring.py`, `integrity.py` | Sınırlı sunucu bağlantıları, uç HTTP havuzu, kilit bekleme/commit ölçümü ve yöneticiye açık bütünlük taraması. |
| `catalog.py`, `catalog_admin.py`, `metadata.py`, `bulk.py`, `search.py`, `rights.py` | İçerik/sürüm, kategori ve alan sahipliği, güvenli toplu işlem, Türkçe arama ve haklar. |
| `planning.py`, `plans.py`, `broadcast_services.py`, `snapshot_contract.py`, `schedules.py`, `clocks.py`, `auto_schedule.py`, `carts.py`, `rotation.py`, `ads.py`, `ads_service.py`, `makegoods.py`, `traffic_changes.py` | Servise bağlı ayrı yayın planı, kayıpsız sürümlü plan paketi, tekrar takvimi, saat şablonundan izli otomatik seçim, cart/cut, reklam rezervasyonu ve spot etkisi. |
| `playlists.py`, `smart_blocks.py` | Karma playlist sürümü, AND/OR/NOT kriterleri, statik/dinamik seçim, hazır olma denetimi ve atomik taslak plan yazımı. |
| `podcast.py`, `listener_stats.py` | RSS/Atom beslemesi, GUID/enclosure tekilleştirmesi, son bölüm eşitleme/otomatik taslak medya indirme, son onaylı bölümün plana alınması ve mount kapsamlı dinleyici sayımı. |
| `libretime_adapter.py`, `audio_playout.py` | Onaylı planın LibreTime playout verisine eşlenmesi ve kabul edilmiş uç planından doğrulanmış PCM WAV üretimi. |
| `radio_cues.py`, `playback_markers.py`, `production.py`, `controller_profiles.py` | Onaylı radyo cue'ları, somut geçişte sweeper/voice track ve sürümlü MIDI/cartwall haritası. |
| `read_api.py` | İstasyon kapsamında item/version/plan/revision/entry ayrıntısı ve `_id`/`_rev` okuması. |
| `retention.py` | Saklama politikası, bağımlılık denetimi, doğrulanmış soğuk arşiv, sıcak kaydı çıkarma ve tarihsel geri okuma. |
| `tools/acceptance.py`, `tools/acceptance_playout.py` | Ayrı HTTP merkez/uç süreçleri, örnek PCM geçiş/makro/stream oynatıcısı, offline ve temiz kurtarma kabul aracı. |
| `settings.py` | İstasyon içe alma profilleri ve kullanıcı görünüm tercihleri. |
| `media.py`, `readiness.py`, `backup.py`, `migration.py`, `store_provision.py` | Medya nesnesi, çıkış/bölgeye göre hazırlık, tutarlı kurtarma paketi, v1→v2→v3→v4 güvenli kopya göçü ve bağımsız depo kopyası. |
| `playout.py`, `endpoint*.py`, `transport.py`, `http_server.py`, `http_limits.py`, `openapi.py`, `workflow_api.py`, `now_playing.py`, `asrun_reports.py`, `live_overrides.py` | Servis/uç ayrımı, dağıtım, uç önbelleği, süreli komut, olay/çıkış kanıtı, CSV/JSON raporu, canlı müdahale, HTTP hız sınırı ve OpenAPI. |
| `station_outputs.py`, `access_admin.py`, `emergency_alerts.py`, `recording_jobs.py`, `listener_requests.py`, `diagnostics.py`, `capacity_monitor.py`, `station_clone.py`, `station_admin.py` | Çıkış/coğrafi kod, istasyon kullanıcısı/token yönetimi, acil uyarı, kayıt/istek, arıza/kapasite alarmı ve yeni kimlikli istasyon kurulumu. |
| `rekordbox.py`, `importer.py`, `mairlist31_*.py`, `radioboss_m3u8*.py` | Rekordbox XML, tarihsel mAirList 3.1 CSV ve düz RadioBOSS M3U8 aktarımı, kaynak/kayıp/çatışma raporu. |
| `portable_export.py`, `portable_import.py` | JSONL/CSV ve medya manifestli taşınabilir veri paketi, doğrulama ve boş hedefe güvenli geri okuma. |
| `ops_archive.py`, `store_health.py`, `ops_scheduler.py`, `tools/ops_archive.py`, `tools/ops_scheduler.py` | Değişmez audit kopya arşivi, doğrulanmış zincir/sorgu, ölçümlü depo sağlığı ve periyodik yedek/arşiv. |
| `tools/benchmark.py` | Geçici `.sdb` ile sentetik kapasite ölçümü; üretim kabulü yerine geçmez. |
| `replication_stream.py`, `failover.py`, `failover_server.py`, `disk_index.py`, `online_maintenance.py` | Sürekli TLS delta/media aktarımı, fenced otomatik devir ve sabit API, generation-bound disk sorgusu, online checkpoint hazırlama. |
| `tools/stream_acceptance.py`, `tools/failover_acceptance.py`, `tools/disk_index_acceptance.py`, `tools/maintenance_acceptance.py` | Geçici dizinlerde gerçek ağ/süreç kesintisi, devir, disk indeksi ve concurrent bakım kabulü. |
| `tools/recovery_acceptance.py`, `tools/replication_acceptance.py`, `tools/query_integrity_acceptance.py`, `tools/engine_acceptance.py` | Geçici dizinlerde işletim kabulü: gerçek süreç kesilmesi, geri dönüş, replika, okuma tutarlılığı, bozulma ve kaynak sınırları. Üretim verisi almaz. |

Uygulama durumu ve doğrulanmamış sınırlar [durum belgesinde](IMPLEMENTATION_STATUS.md); motor tasarımı [mimaride](docs/architecture.md), **18 yerel PDF ile rapor/tez eşlemesi** [yerel kılavuz denetiminde](docs/local_manual_audit.md), LibreTime ve OpenBroadcaster kaynaklarıyla yürütülen dar denemeler ile işlev farkları [açık kaynak uygulama karşılaştırmasında](docs/open_source_app_verification.md), çevrimiçi resmî kaynak araştırması [araştırma belgesinde](docs/research.md), kurulum/geri yükleme [işletim belgesinde](docs/operations.md) bulunur.
