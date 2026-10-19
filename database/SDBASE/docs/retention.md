# Saklama politikası ve soğuk geçmiş

0.6.0 / schema v3, tarihsel kayıtları doğrulanmış segmente taşıyıp sıcak depodan çıkarır. Normal koleksiyon silme ve değişmezlik kuralları devam eder. Bakımı yalnız **bütün istasyonlarda manage yetkisi olan etkin kullanıcı** yapabilir; audit önce/sonra görüntüleri hassas yönetim verisi içerebilir.

## Politika

`retention/policy-put`, `keep_days` nesnesini ve mevcut politika varsa `expected_revision` değerini alır. İlk yazımda revision verilmez. `policy-get` kayıt kimliği/revizyonunu döndürür. Politika yoksa aşağıdaki varsayılanlar kullanılır; bakım kendiliğinden başlamaz.

| Koleksiyon | Sıcak saklama günü | Seçim koşulu |
|---|---:|---|
| `_audit` | 365 | Zamanı geçmiş ardışık commit öneki. |
| `playout_events`, `playout_attempts` | 730 | Olay zamanı; girişim completed/failed/skipped/cancelled olmalı. |
| `output_observations`, `listener_samples` | 180 | Gözlem/kaynak örneği zamanı. |
| `plan_revisions`, `plan_entries` | 730 | Yalnız superseded revizyon; ona bağlı eski girdiler. |

Politikada bulunmayan koleksiyon sıcak kalır. Günler 1–36500 arasında tam sayıdır. Taslak/onaylı güncel plan, devam eden girişim, güncel kabul ve başka sıcak kayıtların referansları korunur. Nested nesne/liste/UUID anahtarları da taranır; UUID yazımı büyük harf olsa bile ilişki korunur. Korunan bir hedefin referansları da korunur. `based_on_id`, dağıtım, rapor veya prodüksiyon bağı olan eski revizyonlar bu nedenle sıcak kalabilir; preview `pinned_counts` bunu gösterir. Medya dosyaları bu işlemle silinmez.

## Önizleme ve uygulama

SDBASE kökünde veya kurulmuş CLI ile:

```sh
sdbase --db /veri/RadioS.sdb retention-preview \
  --token YONETICI_TOKENI --max-records 10000 > /veri/retention-preview.json

sdbase --db /veri/RadioS.sdb retention-apply \
  --token YONETICI_TOKENI --preview-json /veri/retention-preview.json

sdbase --db /veri/RadioS.sdb retention-verify

sdbase --db /veri/RadioS.sdb retention-query \
  --token YONETICI_TOKENI --collection playout_events --limit 100
```

`--policy-json` yalnız collection→days nesnesiyle bir çalıştırmalık seçim sağlar. `--as-of` açık saat kullanımı/simülasyon içindir; normal bakımda verilmez. Her uygulama en fazla `max_records` (1–100000) seçer; kalan uygun kayıtlar için yeni preview alınır. Merkezde araya yazma girerse eski preview 409/Conflict verir. HTTP karşılıkları `POST /v1/workflows/retention/preview`, `apply`, `query` yollarıdır; `apply` gövdesi `{"preview": ÖNIZLEME_NESNESI}` biçimindedir.

Arşiv dizini `RadioS.sdb.archives/SEGMENT_UUID/` altında `records.jsonl` ve `manifest.json` içerir. UUID, belge revision ve tam JSON verisi korunur. `retention_segments` sıcak kayıtları dosyaların SHA-256/boyutlarını, politika özetini, koleksiyon sayımlarını ve audit sıra aralığını saklar. Hash, kazara bozulmayı algılar; imzalı kaynak kimliği değildir.

## Atomiklik ve kurtarma

Bakım özel yazma kilidi altında seçimi tekrar hesaplar, arşiv dosyalarını fsync eder ve sıcak kayıtlarla birebir doğrular. **Atomik checkpoint değişimi commit noktasıdır.** Sonra eski journal küçültülür ve metadata kuşağı artırılır. Mevcut okuyucular checkpoint dosyasının değişimini de algılar; metadata güncellemesi yarıda kalsa bile yeni commit yeniden açılışta görülebilir.

Commit öncesi hata sıcak veriyi değiştirmez. Commit sonrası I/O hatasında sonucu yeniden açıp `retention-verify`, sequence ve segment kayıtlarıyla kontrol edin; eski preview'i tekrar uygulamayın. Commit edilmemiş bir çalışma arşivde referanssız segment bırakabilir; bu segment sıcak verinin silindiğini göstermez ve fiziksel yedeğe alınmaz. Bu sürüm soğuk segmentleri otomatik silmez.

Silinen kimlikler tombstone olur. Ayrıca eski **benzersiz idempotency anahtarlarının hash'leri** checkpoint içinde kalır: gecikmiş bir event/observation/sample tekrarının yeni UUID ile aynı kanıtı çoğaltması Conflict ile engellenir. Anahtarın sonucu soğuk sorguyla okunur. Tombstone ve bu küçük hash indeksleri büyümeye devam eder; sıcak tam kayıt/before-after gövdeleri ve normal sorgu indeksleri çıkarılır. Bellek/disk tüketiminin tamamen sabit kalacağı iddia edilmez.

## Geri okuma ve yedek

`retention-query` her segmenti hash, sayım ve audit aralığıyla doğrular; kayıtlar `_id`, `_rev`, `_segment_id` taşır. `record_id`, `segment_id`, `limit`, `offset` desteklenir. Soğuk okuma sıralı taramadır; düşük gecikmeli katalog sorgusunun yerine geçmez. Mevcut audit zinciri dışa aktarıcısı gerektiğinde sıcak ve soğuk audit kayıtlarını birleştirir.

Fiziksel `backup-create` ve restore, kayıtlı soğuk dosyaları ve emekli benzersiz anahtar hash'lerini taşır. Bozuk veya eksik arşiv paket yayımını durdurur. Ses baytları/audit taşımayan portable JSONL/CSV export, soğuk segment varsa eksik paket üretmek yerine reddedilir; bu durumda fiziksel yedek kullanılır. Yedek kendi başına önceki uç bilgisayarın yerel `Endpoint.sdb` günlüğünü içermez; temiz uç yeniden yetkilendirilir.

## Çalıştırılan doğrulama

2026-10-01'de gerçek geçici `.sdb` ile **24 kontrol** geçti: dependency pinning, eski preview, archive verification failure, sıcak kaydın gerçek çıkarılması, açık okuyucunun yenilenmesi, cold ID/revision, normal silme reddi, tombstone, endpoint erişim reddi, hot+cold audit sürekliliği/health/export, portable omission reddi, cold dosyalı backup/restore, bozuk arşiv ve yarıda checkpoint/meta yazma kurtarması. Sonraki kontroller emekli idempotency anahtarının canlı/reopen/checkpoint/restore sonrasında reddini ve UUID yazımını doğruladı. Geçici dosyalar temizlendi. Bakım bütün sıcak referansları tarar ve yazmaları kilitler; büyük kurulumda bakım penceresi ve hedef disk ayrıca ölçülmelidir.
