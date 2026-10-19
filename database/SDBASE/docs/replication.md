# Salt okunur standby ve kontrollü görev devri

SDBASE 0.7.0 / schema v4, kendi `.sdb` journal kayıtlarını başka bir dizindeki fiziksel kopyaya aktarır. SQL sunucusu çalıştırmaz. PostgreSQL kaynaklarındaki sistem kimliği, timeline ve sıralı WAL denetimleri SDBASE'in belge motoruna uyarlanmıştır; uygulama bağımsız yazılmıştır.

Bu sürüm **asenkron, elle başlatılan aktarım** ve **erişilebilir eski primary'nin yazmasını durduran görev devri** sağlar. Dağıtılmış quorum, otomatik lider seçimi, senkron çoğaltma veya ulaşılamayan primary için zorla yükseltme içermez. Standby en son uygulanan kayıt kadar günceldir; source'ta commit edilip henüz aktarılmamış veriler standby'da bulunmaz.

## Kimlik ve yazma kapısı

`.meta` dosyası `node_role` (`primary`, `standby`, `fenced`), `node_id`, `timeline_id` ve `epoch` içerir. Fiziksel standby **store_id'yi korur**, kendisine yeni node_id alır. Bir Store nesnesi açıldığı andaki node/timeline/epoch değerlerine bağlanır. Her transaction, checkpoint ve retention yazması kilit altında güncel rol ve kimliği tekrar denetler. Eski açık writer nesnesi görev devrinden sonra yazamaz; yeni primary yeniden açılır.

Standby normal açılışta journal kuyruğunu tamir etmez. HTTP okuma yolları mevcut kullanıcı/istasyon izinlerini uygular; API token'ları fiziksel kopyada aynı kapsamla bulunur. API mutasyonları standby veya fenced düğümde `503` döndürür. HTTP'de çoğaltma/yükseltme mutasyon yolu yoktur: bu işlemler dosya erişimi olan işletmecinin yerel CLI bakım komutlarıdır.

## Yeni standby

Hedef dizin bulunmamalıdır. Kaynakta WAL arşivi otomatik etkinleştirilir; ardından medya ve soğuk geçmiş dosyalarını içeren fiziksel yedek alınır. Yedekle aynı kilit altında yakalanan `wal_anchor`, kopyanın sequence/hash bağlantısını sabitler. SHA-256/boyut doğrulaması ve yeniden açılış tamamlanmadan hedef yayımlanmaz.

```sh
sdbase --db /veri/primary/RadioS.sdb replica-create \
  --target-dir /veri/standby

sdbase --db /veri/standby/RadioS.sdb replica-status

sdbase --db /veri/primary/RadioS.sdb replica-catchup \
  --standby-db /veri/standby/RadioS.sdb --max-records 10000
```

`replica-catchup` bir sınırlı batch uygular. `lag_transactions` sıfır olana kadar tekrarlanabilir; çalışma sırasında yeni commit'ler gelirse lag tekrar artabilir. Eski source journal checkpoint ile küçülse bile mühürlü `RadioS.sdb.wal/` arşivi eksik aralığı sağlar. Retention gibi journal yerine checkpoint ile commit edilen kayıtlar da aktarılır.

## Ayrı aktarım paketi

Standby status içindeki `replication.last_replayed_sequence` ve `last_record_sha256` sender'a verilir. Komut, beklenen önceki hash source arşivindeki hash ile eşleşirse yeni hedef dizine paket yazar.

```sh
sdbase --db /veri/primary/RadioS.sdb replica-shipment \
  --destination /aktarim/batch-001 \
  --after-sequence SON_SEQUENCE --previous-sha256 ONCEKI_SHA256

sdbase --db /veri/standby/RadioS.sdb replica-apply \
  --bundle /aktarim/batch-001
```

Paket `manifest.json`, detached hash, sıralı `records.json` ve bu commit'lerin kullandığı medya/soğuk geçmiş baytlarından oluşur. Alıcı bütün envanteri, dosya hash/boyutlarını, record hash zincirini, source/node/timeline/epoch/schema ve managed path kurallarını doğrular. Belge/indeks/kalıcı referans tutarlılığı bellekte önceden replay edilir; sonra baytlar ve kayıtlar yayımlanır. Nesneler önce ayrı geçici dosyada doğrulanıp atomik olarak yayımlanır; süreç kopyalama sırasında kapanırsa yeni nesne yolu yarım baytlarla dolmaz. Sequence atlama, farklı source, fork, bozuk dosya veya mevcut farklı medya baytı reddedilir.

Bir batch en fazla **10000 kayıt** ve `records.json` için **256 MiB** içerir. Daha küçük batch seçilebilir; tek checkpoint bu sınırı aşıyorsa yeni fiziksel standby tabanı alınır. Medya baytları bu JSON sınırına dahil değildir; hedef disk alanı gerekir. Arşiv okuması zinciri ve nesneleri doğrulayarak tarar; düşük maliyetli ağ streaming'i veya sabit bellek tüketimi iddiası yoktur. Aynı tamamlanmış batch'in tekrar teslimi idempotenttir; eski farklı batch veya atlanan aralık hata verir.

Checksum kazara bozulmayı algılar, kaynak imzası değildir. Paketi güvenilir dosya alanı ve kimliği doğrulanmış taşıma üzerinden ulaştırın. Bu sürüm TLS/ağ aktarım servisi kurmaz; dizin başka makineye işletmecinin mevcut aktarım yöntemiyle taşınabilir.

## Yarıda replay kurtarma

Her kayıt için önce `.meta.replication.pending` yazılır, sonra journal/checkpoint fsync edilir, en son ilerleme hash'i yayımlanır. Tam frame diskteyken süreç kapanırsa aynı paket yeniden uygulanarak frame ve pending hash eşleştirilir. Aynı sequence'a farklı frame gönderilmesi kabul edilmez.

Torn frame nedeniyle normal Store açılamıyorsa özgün paketle açık bakım komutu kullanılır:

```sh
sdbase --db /veri/standby/RadioS.sdb replica-recover \
  --bundle /aktarim/batch-001
```

Bu komut yalnız kilit, doğru standby kimliği, kalıcı pending niyeti ve **tam doğrulanmış bekleyen frame'in birebir öneki** varsa eksik kuyruk baytlarını kaldırıp paketi uygular. Tam frame checksum hatası, ilgisiz eksik kuyruk veya farklı paket onarılmaz. İşlem en fazla 30 saniye store kilidini bekler. Normal standby okuyucusu kendi başına bayt kesmez.

## Kontrollü yükseltme

Primary ve standby aynı son sequence ve arşiv hash'ine gelmiş olmalıdır. Görev devri komutu iki store kilidini sabit sırada alır; tam mantıksal state, tombstone, emekli benzersiz anahtar ve koleksiyon sequence eşitliğini doğrular. Komutun beklediği sequence veya timeline değişmişse yazma başlamadan reddedilir.

```sh
sdbase --db /veri/primary/RadioS.sdb replica-promote \
  --standby-db /veri/standby/RadioS.sdb \
  --expected-sequence KONTROL_EDILEN_SEQUENCE \
  --expected-timeline KONTROL_EDILEN_TIMELINE_UUID
```

Sıra şöyledir:

1. Eski primary metadata'sı **fenced** yapılır; eski ve yeni açılacak writer'lar yazamaz.
2. Hedef standby'da fsync edilmiş bir yükseltme niyeti ve checkpoint hazırlanır. Uçlar offline, medya kopyaları pending, aktif kabuller inactive olur; bekleyen engine komutları expire edilir ve endpoint token'ları iptal edilir. Değişikliklerin before/after audit görüntüleri korunur.
3. Hedef metadata **primary**, yeni timeline UUID ve bir büyük epoch ile yayımlanır. Hedef node_id/store_id korunur. Eski source fenced kalır.

Süreç bu adımlar arasında kapanırsa aynı source/target/sequence/timeline komutu kayıtlı niyeti tamamlayabilir. Kaynak hiçbir kurtarma adımında tekrar writable yapılmaz. Başarıdan sonra komut tekrarı `already_promoted` döndürür. Yükseltme journal kaydı yerine tek atomik checkpoint commit'i ekler; endpoint sahipliği otomatik devam ediyor gibi gösterilmez.

Yeni primary servisini yeniden açın. Yönetici hesabı/token'ı kalır; endpoint'lere `endpoint-token` komutuyla yeni credential verilir, medya/planlar yeniden doğrulanır ve yayın yetkilendirmesi tekrar yapılır. Eski uçta önceden yetkilendirilmiş offline yayın fiziksel olarak durdurulmuş sayılmaz: bu komut veritabanı writer'ını ve yeni merkez yetkilerini sınırlar, uzaktaki ses cihazını kapatmaz.

Yeni timeline'da WAL config kaldırılmıştır; tekrar `wal-enable` ve yeni base backup alınır. Eski source'un mevcut arşivi önceki timeline'ın kurtarma kanıtı olarak kalır. Eski source'u yeniden standby olarak kullanmak için yeni absent dizine yeniden fiziksel kopya alınır; yerinde otomatik rewind yoktur.

## Kaynak örüntüleri ve doğrulama

Yerel PostgreSQL **20devel** kaynaklarında `src/backend/replication/walreceiver.c` içindeki source system identity/timeline kontrolü ve `src/backend/access/transam/xlogrecovery.c` içindeki standby recovery yaklaşımı incelendi. Yerel SQLite **3.54.0** `src/backup.c` tutarlı snapshot kopyasının örneğidir. Bu özellikte bu projelerden birebir kaynak kodu kopyalanmadı; PostgreSQL/SQLite çalıştırılmıyor.

2026-10-01'de `PYTHONDONTWRITEBYTECODE=1 python tools/replication_acceptance.py` ile **35 kontrol** geçti: ayrı writer ve okuyucu süreçleri, gerçek standby HTTP detay/revision okuması ve 503 yazma reddi, doğrudan medya kopyalama engeli, kopyalama sırasında gerçek süreç kapanması ve tekrar deneme, medya baytları, retention/cold dosyaları, primary checkpoint sonrası delta, tamper/gap/stale reddi, replay fsync sonrası gerçek `os._exit` süreç kapanması, 87 baytlık torn tail, görev devri checkpoint'i sonrası gerçek süreç kapanması, eski writer'ların engellenmesi, credential fence ve normal audit dışa aktarımı. Runner yalnız kendi geçici dizinlerini kullanır ve temizler; `--output RAPOR.json` sonuçları saklar.

Bu deneme aynı makinede yapıldı. İkinci fiziksel makine, güç kesintisi, ağ bölünmesi/quorum ve otomatik failover doğrulanmış değildir.
