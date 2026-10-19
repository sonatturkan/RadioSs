# Motor sınırları, bağlantılar ve izleme

SDBASE 0.7.0 bağlantı kabulünü, kilit beklemesini ve istek gövdesi süresini sınırlar. Kaynak izinleri olan yerel CLI bakım işleri ile bütün veritabanını görebilen HTTP yöneticisi ayrı erişim yollarıdır. HTTP'de ham replika yükseltme veya ham koleksiyon yazma yolu bulunmaz.

## Sunucu ve uç bağlantıları

```sh
sdbase --db /veri/RadioS.sdb --lock-timeout 5 serve \
  --host 127.0.0.1 --port 8000 --max-connections 32 --request-timeout 30
```

`BoundedHTTPServer`, aktif ve boşta bekleyen keep-alive socket'leri birlikte sayar. Varsayılan en fazla 32 bağlantı için worker oluşturulur. Doluyken yeni bağlantıya `503`, `Retry-After: 1` ve `Connection: close` döner. Socket süre sınırı boşta bekleyen bağlantıları kapatır. TLS doğrulaması gereken uzak erişimde mevcut gateway yordamı geçerlidir.

JSON gövdesi varsayılan 2 MiB; Rekordbox XML aktarımı özel 90 MB sınırındadır. JSON aktarımının toplam süresi `request_timeout` ile sınırlıdır. Medya aktarımı en fazla 512 MiB, toplam 300 saniye ve her okuma için en fazla 60 saniyedir. Eksik Content-Length gövdesi, birden çok Content-Length, Transfer-Encoding ve GET gövdeleri reddedilir. Hatalı isteğin bağlantısı kapatılır; okunmamış baytlar sonraki istek olarak işlenmez.

`HTTPTransport`, uç protokolünde `HTTPConnectionPool` kullanır. Varsayılan en fazla dört bağlantı vardır; aynı URL'ye yapılan istekler tüketilmiş yanıtın socket'ini kullanabilir. JSON ve medya yanıtları aynı havuzdan geçer. Checkout süresi `timeout` ile sınırlıdır. Erken kapanan veya hatalı aktarımın socket'i havuza dönmez. HTTPS, Python'un sertifika doğrulayan normal `HTTPSConnection` yolunu kullanır.

Taşıma hiçbir isteği otomatik yeniden göndermez. Bağlantı kopmadan önce sunucuda commit oluşmuş olabilir. Uç workflow'larının mevcut idempotency/reconciliation yordamı sonucunu tekrar okuyarak karar verir. Uzun ömürlü taşıma `close()` veya context manager ile kapatılır; `pool.stats()` bağlantı sayısı, açılma, kullanım ve bırakılma sayaçlarını verir. Havuz transaction oturumu veya haricî DB bağlantısı tutmaz.

## Kilit ve commit sonucu

Store varsayılan 30 saniyede thread/process kilidini alamazsa `LockTimeout` üretir. Süre bekleyen işlem journal'a yazılmadan sona erer. CLI `--lock-timeout`, Python `open_radio_store(..., lock_timeout=5)` ile değiştirilebilir. Her commit, checkpoint, retention ve yönetilen medya yayımlama güncel node rolünü ve açık writer'ın timeline/epoch değerini denetler.

Yönetilen medya kopyalama sırasında paylaşılan store kilidi tutulur; bu sırada görev devri rolü değiştiremez. Büyük medya kopyası başka yazıcıların kilit beklemesini uzatabilir. Başarıyla kopyalanmış fakat DB commit'i reddedilmiş bir içerik adresli nesne, önceki medya modelinde olduğu gibi sahipsiz kalabilir; otomatik onaylanmış kayıt oluşturmaz.

HTTP rol/kilit reddi `503 temporarily_unavailable` döndürür. WAL açılmışsa journal fsync sonrası arşiv yazma hatası ayrı sonuçtur:

```json
{
  "error": "archive_acknowledgement_failed",
  "committed": true,
  "sequence": 42,
  "transaction_id": "işlem-uuid",
  "detail": "..."
}
```

Bu yanıt `503` olsa da işlem kaynak journal'da kalıcıdır; sıradan bir tekrar deneme önerisi taşımaz. Önce sonucu okuyun, arşiv hatasını giderin ve `wal-capture`/`checkpoint` ile zinciri tamamlayın. Başarılı arşiv güvencesi olmadan journal checkpoint'te atılmaz.

## İzleme ve bütünlük

```sh
sdbase --db /veri/RadioS.sdb metrics
sdbase --db /veri/RadioS.sdb integrity --max-records 100000 --max-bytes 1073741824
```

`GET /v1/operations/metrics` ve `GET /v1/operations/integrity`, etkin kullanıcı ve global `manage` yetkisi ister. Endpoint token'ı veya tek istasyon yönetimi bütün motoru göremez. Integrity sorgusuna `max_records`, `max_bytes`, `verify_media`, `verify_cold` verilebilir. Sonuç tamamlanmamışsa başarılı bütünlük onayı sayılmaz; [tarama sözleşmesini](integrity.md) izleyin.

Metrics kayıt gövdelerini, token hash'lerini ve izin kayıtlarını yayımlamaz. Sequence/generation, primary/standby/fenced kimliği, koleksiyon sayıları, journal/checkpoint ve boş disk baytları, WAL ilerlemesi, replika kaynak/ilerleme bilgisi ve HTTP kapasitesi bulunur. Replikanın durumu son uygulanan sequence'ı gösterir; güncel primary lag'i sender ile catchup sonucunda ölçülür. Bölümler kendi okuma kilidi altında toplanır; bütün metrics yanıtı tek tarihsel transaction görüntüsü değildir.

`EngineMetrics` aynı süreçte commit/checkpoint/kilit/HTTP sürelerini milisaniye cinsinden tutar; her süre türünde en fazla 1024 örnek ve p50/p95/max vardır. Commit, çatışma, kilit timeout, reddedilen yazma, yazılan journal baytı ve HTTP yanıt kodu sayaçları tutulur. Restart'ta bu sayaçlar sıfırlanır. Ayrı Store nesnelerinin sayaçları otomatik birleştirilmez. Dış izleme sistemi örnekleri ayrıca saklayabilir. `/health` okunabilir merkez yanıtını gösterir; bütün medya/ilişki/ses çıkışı doğrulaması değildir.

0.8.0 `/health` yanıtı `store_id`, `node_role`, `node_id`, `timeline_id`, `epoch` de verir. [Supervisor](automatic_failover.md) bu kimliği pinler. `/health` başarı kodu tek başına yayın yetkisi veya audio çıkışı değildir. Primary olmayan node endpoint credential isteklerini ve bütün `/v1/endpoints/…` operasyon yollarını `503` ile reddeder. User credential ile izinli katalog ayrıntı okumaları standby'da devam edebilir.

## Tekrarlanabilir işletim kabulü

```sh
PYTHONDONTWRITEBYTECODE=1 python tools/engine_acceptance.py
```

Araç yalnız geçici dizinleri kullanır. Kalıcı HTTP bağlantı kullanımı, havuz checkout sınırı, eksik yanıt bırakma, idle bağlantı kabul sınırı, yönetici/endpoint ayrımı, hatalı gövde/pipeline reddi, kalıcı fakat arşivi onaysız commit, fenced HTTP yazma reddi, ayrı süreçte gerçek kilit beklemesi ve v1/v2/v3→v4 kaynak hash korumasını sınar. Üretim yolu almaz; temizler. Saha güvenilirliği ve fiziksel ses cihazı için ayrı tatbikat gerekir.
