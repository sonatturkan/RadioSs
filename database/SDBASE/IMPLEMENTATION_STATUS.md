# SDBASE uygulama durumu

**0.8.1 · schema v4 · 95 iş koleksiyonu · 160 workflow · 210 HTTP yolu.** RadioS için kendi işlem günlüğüne yazan, SQL kullanmayan `.sdb` motoru ve radyo veri backend'i. Çalışma zamanı üçüncü taraf paket bağımlılığı yoktur. Merkez `RadioS.sdb`, uç `Endpoint.sdb` kullanır; kod ayrı modüllerdedir.

## Nihai düzeltme 0.8.1

Temiz wheel/CLI kurulumunda saptanan boş indeksin yanlış `stale_or_invalid` raporlanması düzeltildi; sıfır satırlı indeksin geçerliliği ve readonly restart sonrası doğru durum ayrıca kabul aracına eklendi. Son paketleme, kaynak kimliği ve sürüm kabulü [nihai rapordadır](docs/finalization_081.md).

## 0.8.0 ekleri

- Dedicated TLS listener ve sürekli follower ile SHA-pinned, bounded WAL/medya/cold history aktarımı; kesinti, süreç restart ve yarım indirme sonrası devam.
- Tek sürekli monitor, kalıcı karar, sağlık kimliği, reachable-source fencing ve yeni timeline/epoch ile otomatik servis devri. Sabit API adresi aynı keepalive bağlantısının sonraki isteğini yeni node'a taşır. Eski kaynakta endpoint operasyon okumaları da reddedilir.
- Sıralı disk sidecar indeksleri; bounded external merge, immutable generation, mmap/bisection ve gerçek query yolu. Bozuk/stale cache RAM fallback veya açık bütçe reddi üretir.
- Online checkpoint: büyük hazırlık kilit dışında, bounded suffix/publication kilit içinde; retention'ı geri getirmeyen supersede fence ve owned crash cleanup.

Kanıt ve sayısal ölçümler [0.8.0 raporundadır](docs/maturity_080.md). Kaynak dosyaları erişilemeyen uzak ağ arızasında otomatik promotion yapılmaz. Quorum, fiziksel audio failover, RAM'den bağımsız bütün engine ve yıllarca production geçmişi sağlanmış değildir.

## 0.7.0 motor olgunluğu

- Tutarlı okuma görüntüsü katalog araması ve ayrıntı API'sine bağlandı; indeks kesişimi, bileşik aralık, keyset ve explain eklendi. Eşzamanlı yazmalar sırasında açık görüntü korunur. Diskte tuple MVCC veya paralel yazıcı iddiası yoktur.
- Yeni commit'ler hash zincirli WAL arşivine alınabilir. Temel yedek ve zincirden kesin sıra/zaman geri dönüşü medya ve soğuk kayıtlarla doğrulanır; checkpoint önce arşiv güvencesini tamamlar.
- Doğrulanmış fiziksel temel kopya ve artımlı paketlerle salt okunur standby çalışır. Kontrollü devirde erişilebilir kaynak önce fenced olur, hedef yeni timeline/epoch ile açılır. Eski yazıcılar ve canlı uç sahipliği engellenir.
- Sunucu bağlantı sayısı, socket süresi ve kilit beklemesi sınırlıdır; uç taşımasında HTTP havuzu kullanılır. Yönetici metrics/integrity GET'leri kayıt içeriğini veya token özetini yayımlamaz.
- v1/v2/v3 → v4 kopya migration, eski dosya hash'leri değişmeden doğrulanmıştır. WAL/replica kimliği bağımsız restore ve migration staging'inden ayrılır.

Kanıtlar ve uygulanmayan kapsam [motor olgunluğu tablosundadır](docs/storage_maturity.md). Otomatik uzak quorum failover, disk tabanlı B-tree/GIN, tam PostgreSQL MVCC ve terabayt kapasitesi bu sürümde kanıtlanmış değildir.

## Son denetimde kapanan işler

| Alan | Kaynaktaki karşılık | Doğrulama |
|---|---|---|
| Çıkışa özgü yayın yasağı | Onaylı revizyonda somut endpoint/output binding; belirsiz default reddi; deny kısmi zaman kesişmesi; devre dışı istasyonda yeni execution reddi. | 16 odaklı kontrol ve süreç kabulündeki negative senaryolar. |
| Onaylı marker | Değişmez playback_marker_sets; source cue ID/revision, medya ID/SHA/süre, cue-in/out, segue-in/out, fade; plan onayında frozen playback_bindings. | 19 odaklı kontrol, ambiguity/tamper/legacy retleri; gerçek LibreTime 4.5.0 schedule reader cue/fade. [Sözleşme](docs/playback_markers.md). |
| GET ayrıntı/revision | Item/version/plan/entry/revision/station-item için 11 yol; gerçek _id/_rev, view yetkisi, istasyon sınırı ve tek commit'ten snapshot okuması. | 50 önceki HTTP isteği ve yeni eşzamanlı snapshot kontrolleri; stale düzenleme409 ve reread sonrası kabul. [API](docs/api.md). |
| Migration | v1→v2→v3→v4 registry; eski history korunur fakat reapproval olmadan yeni paket dağıtılmaz; bağımsız kimlik ve yeni credential; sahiplik marker/job lock ile SIGKILL recovery. | Önceki 36 migration/kurtarma/yol-yarışı kontrolü; v2→v3 için 55.509 içerik/277.559 kayıt ölçümü; yeni v1/v2/v3→v4 kaynak SHA koruma kontrolleri. [Göç](docs/schema_migration.md). |
| Retention | Saklama politikası, hot dependency pinning, verified cold segment ve atomic checkpoint purge, tarihsel query, cold-file backup/restore, kalıcı ID ve unique-key fence. | 24 temel kontrol; UUID canonical pinning, numeric unique-key eşitliği ve eski event replay retleri. [Bakım](docs/retention.md). |
| Ürün kapsamı | Sürüm ve alan bazlı preserved/transformed/rejected matrisi; resmî kaynaklar ve kayıp raporu. | [Adaptör matrisi](docs/adapter_coverage.md). |

## Çalışan radyo veri kapsamı

| Alan | Bulunan işlevler | Pratik sınır |
|---|---|---|
| Depolama | Checksummed atomic journal, fsync, süreç kilidi, optimistic revision, indeks/benzersizlik, koleksiyon kuşağı, checkpoint, tombstone ve audit. | Güncel belgeler/indeksler bellektedir; genel amaçlı DBMS veya sınırsız ölçek iddiası yoktur. |
| Katalog/metadata | İçerik–sürüm–medya ayrımı, station_items, kişi/grup/katkı/kategori, alan sahipliği/onayı, bulk preview/apply/undo, Türkçe arama. | Ayrı eser/kayıt varlığı ve bütün GUI akışları bulunmaz. |
| Medya | İzinli kaynak ve içerik adresli nesne, technical review, merkez/uç SHA kontrolü, reconcile. | Otomatik bütün codec analiz motoru yoktur. |
| Planlama | Sekiz olay türü, günlük service plan/revision, DST/gece yarısı, takvim tekrarı, clock/grid, cart/cut, rotasyon, hak ve hazır olma. | Gerçek zamanlı cihaz yönlendirme/kesme/filler harici oynatıcıya bağlıdır. |
| Playlist/smart block/podcast | Yayımlanmış playlist/smart block sürümleri, AND/OR/NOT, statik/dinamik seçim, RSS/Atom sync, optional latest draft download, son onaylı bölüm çözümü. | Teknik/editoryal approval otomatik verilmez; periyodik çağrıyı operatör kurar. |
| Reklam | Campaign→spot→obligation→break→reservation→plan→distribution→reconciliation; ayrı insan onaylı makegood ve spot değişimi. | Satış/fatura ve tüm trafik ekranları yoktur; tamamlanmış/makegood placement otomatik fork edilmez. |
| Prodüksiyon | Version/media-bound cues, somut transition/voice/sweeper, stale/rebind, MIDI/cartwall veri profili. | Donanım/MIDI/voice editörü yoktur; transition overlay ayrı yürütücü gerektirir. |
| Yayın kanıtı | Plan, attempt, phase event ve bağımsız output observation ayrımı; idempotency; immutable as-run görüntüleri. | Motor kaydı dinleyiciye teslim veya reklam içeriği sertifikası değildir. |
| Uç/offline | Yerel journal/cache, hash kontrolü, engine epoch, ertesi hizmet günü için önceden alınmış offline grant, command/outbox/reconnect. | Merkezden kopmuş authority anında iptal edilemez; süre sınırı geçerlidir. |
| İzleme/tercihler | Incident/diagnostic/kapasite alarmı, user view/import profiles, now-playing state, listener mount/time series, request/recording/emergency data workflows. | GUI, fiziksel sensör/kayıt cihazı, RDS/CAP gönderici ve otomatik Icecast polling tamamlanmış değildir. |
| Yedek | DB+managed media+cold history manifesti, boş hedefe restore, fencing ve bağımsız provision-copy. | Ucun eski local journal'i merkez yedeğinde değildir; yeni uç credential/medya doğrulaması gerekir. |
| Adaptörler | Rekordbox XML1.0.0, tarihsel mAirList3.1 CSV, RadioBOSS plain M3U8, LibreTime playout-v2 projection; kendi portable data package. | Native ürün DB'lerine drop-in uyumluluk veya her alanı kayıpsız alma iddiası yoktur. |

## Uçtan uca ve kapasite kanıtı

[Operasyonel kabul aracı](docs/production_acceptance.md) **37 kontrol**, **84 yönetici HTTP çağrısı** ve **84 worker süreciyle** çalıştı. Ayrı HTTP merkez/uç süreçleriyle gerçek PCM cue/fade/250ms overlap, izinli makro, bounded local HTTP stream ve katalog event log zinciri yürüdü. Merkez gerçekten **60 saniye kapalı kaldı**. Ertesi hizmet günü/saat sapması/grant expiry/restart/media loss/command crash/reconnect/new revision senaryoları kontrollü saatle doğrulandı. Kaynak DB/medya/uç silindikten sonra yalnız yedekten temiz ayrı süreçte aynı PCM hash'i üretildi. Fiziksel ikinci makine veya yayın cihazı kullanılmadı.

[Çok istasyonlu yük ölçümü](docs/load_verification.md): **250.000 içerik + 240.000 operasyon kaydı = 1.240.000 sentetik domain kaydı**, dört istasyon, sekiz API kullanıcısı/işçisi, 240 gerçek HTTP isteği. İlgisiz yazmalardan kaynaklanan eligibility seqfence düzeltmesiyle uygunluk60/60 ve plan+entry60/60 kabul edildi. Genel238 başarı/2 katalog generation409; retry yok. Son koşu132,517sn, tepe RSS6,43GiB. Bunlar milyon doğrulanmış medya dosyası veya uzun yıllar gerçek çalışma sonucu değildir.

## Kalan saha kabulü

1. Gerçek ikinci makine ve yayın cihazında temiz yedekten dönüş, ses çıkışı ve RPO/RTO.
2. Günlerce gerçek duvar saati offline soak, saha ağlarının kesilmesi ve fiziksel tek aktif çıkış/failover.
3. Hedef donanımda gerçek dosya sayısı, kullanıcı/istasyon/uç sayısı ve belirlenmiş p95/p99 gecikme bütçesi.
4. Hedef oynatıcı ürününün tam servisi, bütün gerekli DSP/overlay/codec ve cihaz adaptörleri.

Database/backend çekirdeği ve rapordaki üç doğrudan kod eksiği kapandı. Mevcut kanıtlar bütün radyo ürünlerinin tüm GUI/audio/protokol özelliklerinin veya bütün üretim kurulumlarının tamamlandığı anlamına gelmez. Detaylı geçmiş [doğrulama](docs/verification.md) ve [denetim matrisi](docs/audit_closure.md) içindedir. Kalıcı otomatik test klasörü eklenmedi; yeniden ölçülebilen operasyon araçları `tools` altında tutuldu.
