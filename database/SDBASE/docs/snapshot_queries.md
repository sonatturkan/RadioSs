# Tutarlı okuma ve belge sorgu planlama

SDBASE'in `read_snapshot.py` ve `query.py` modülleri, SQL kullanmadan bir
commit'e ait sabit okuma görünümü ve indeks seçimi sağlar.

## İncelenen açık kaynak uygulamaları

| Kaynak | İncelenen dosya / işlev | SDBASE'e uyarlanan ilke |
| --- | --- | --- |
| PostgreSQL | `src/backend/utils/time/snapmgr.c`, `GetTransactionSnapshot`, kayıtlı snapshot ömrü | Birden fazla ilişkili okuma aynı görünür commit'e dayanır; görünümün ömrü açıkça kapatılır. |
| PostgreSQL | `src/backend/optimizer/path/indxpath.c`, `choose_bitmap_and` | Birden fazla kullanılabilir indeksten elde edilen aday kimlikler, seçicilik dikkate alınarak kesiştirilir. |
| SQLite | `src/wal.c`, `walTryBeginRead`, WAL okuma açıklamaları | Başlayan bir okuma tamamlanmış işlemleri görür; sonradan yazılan kayıtlar eski görünümü değiştirmez. |
| SQLite | `src/where.c`, `sqlite3WhereBegin`, `wherePathSolver` | Filtreler ve sıralama için kullanılabilir indeks yolları değerlendirilir; yapılan iş açıklanabilir. |

İncelenen yerel kaynakların SHA-256 değerleri:

- PostgreSQL `snapmgr.c`: `1f97dd99daddc83ed0f0bb90bb6447912a9071750dfb49e31e7de74c4c1300c1`
- PostgreSQL `indxpath.c`: `fc3205456a2f83536e8035d4b63d153c0d19fca54fb2377e73f8e7507ba46fa4`
- SQLite `wal.c`: `11ab3050c715b8d4b05f20c9030ea96ed70475080ba62fe3f60e441ac765eb47`
- SQLite `where.c`: `4a4c067cf68d08fa6c8894da0a4e6bd43e88c9859e57dd34fe365981262d1333`

Resmî açıklamalar: [PostgreSQL concurrency control](https://www.postgresql.org/docs/current/mvcc.html),
[SQLite isolation](https://www.sqlite.org/isolation.html),
[SQLite query planning](https://www.sqlite.org/queryplanner.html).

Bu ilkeler SDBASE'in Python belge motoruna yeniden uygulanmıştır. C kaynak
kodları, SQL ayrıştırıcıları, SQLite sayfa biçimi ve PostgreSQL tuple görünürlük
algoritması pakete aktarılmamıştır. Kaynak projelerin lisansları kendi kaynak
klasörlerinde kalır; bu iki modülde üçüncü taraf kaynak kodu parçası bulunmaz.

## Snapshot sözleşmesi

```python
with store.read_snapshot(["users", "permissions", "plans"]) as view:
    require(view, principal, "view", station_id)
    page = view.query(
        "plans",
        equal={"station_id": station_id},
        ranges={"service_date": {
            "minimum": "2026-10-01", "maximum": "2026-10-31"
        }},
        order_by=("service_date", "service_key"),
        limit=100,
        max_scan=2000,
        max_index_keys=100000,
        max_candidates=100000,
    )
```

- Capture sırasında ortak dosya kilidi altında journal yenilenir. Seçilen
  koleksiyonların kayıt eşlemeleri kopyalanır; indeks üyelikleri dondurulur.
- Commit edilmiş kayıt gövdeleri motor tarafından değiştirilmeden yeni nesneyle
  değiştirilir. Görünüm eski gövdeyi tutar. Sonraki commit, checkpoint, silme ve
  retention'ın sıcak depodan taşıması bu görünümü değiştirmez.
- Kilit capture tamamlanınca bırakılır. Görünüm okunurken yazar ilerleyebilir.
- `get`, `lookup`, `scan` ve sorgu sonuçları iç içe alanlarla birlikte kopyalanır;
  istemcinin sonucu değiştirmesi depoyu veya görünümü değiştirmez.
- Seçilmeyen koleksiyon okunamaz. `close()` eski gövde referanslarını ve türetilmiş
  sıralı indeks anahtarlarını bırakır. Kapanmış görünüm tekrar kullanılamaz.
- Yeni HTTP item/version/plan/revision okumaları ve katalog araması, yetki,
  istasyon ilişkisi, metadata ve readiness kontrollerini aynı görünümde yapar.
  Başka bir koleksiyona yazılan playout olayı detail okumasını 409'a düşürmez.
- Bir görünüm, başladığı anda geçerli olan yetkiyi taşır. Yetki kaldırılınca yeni
  görünüm erişimi reddeder. Görünümleri kullanıcı oturumları arasında saklamak
  yerine her işlem için yeniden açmak gerekir.
- Yazma aynı kayıt `_rev` değerini compare-and-swap ile denetler. Eski görünümün
  güncelliğini yazma işlemine taşımak için `expect` / `expect_sequence` kullanılır.

### Bellek ve eşzamanlılık sınırları

Capture maliyeti seçilen koleksiyonların kayıt sayısı ve indeks üyeliğiyle
orantılıdır. Gövdeler capture sırasında derin kopyalanmaz; değiştirilen veya
retention ile çıkarılan eski gövdeler görünüm kapanıncaya kadar bellekte kalır.
Çok büyük koleksiyonları gereksiz seçmemek ve uzun ömürlü görünüm biriktirmemek
önemlidir. Capture süresince yazar ortak kilidi bekler; sorgu boyunca beklemez.

Bu, süreç içindeki tutarlı okuma görünümüdür. Diskte geçmiş her tuple'ı tutan
MVCC, paralel yazıcı commit'i, predicate lock veya SQL serializable isolation
uygulaması olarak tanımlanmaz. Yazıcılar mevcut dosya kilidiyle sırayla commit
eder; optimistic revision kontrolleri korunur.

## Sorgu sözleşmesi

`view.query(collection, ...)` ve `view.explain(collection, ...)` aynı planlama
kurallarını kullanır. Genel motor API'si kullanıcı yetkisini kendi başına
uygulamaz; domain/API katmanı istasyon yetkisini ve ilişkiyi aynı görünümde
kontrol eder. Genel koleksiyon sorgusu HTTP üzerinden dışarı açılmamıştır.

| Özellik | Davranış |
| --- | --- |
| `equal` | Alan/değer eşitlikleri; tam bileşik hash indeks veya birden fazla indeksin kesişimi. |
| `ranges` | Sayı veya string için alt/üst sınır ve sınırların dahil edilmesi. Uygun bileşik indekste sabit önek + sonraki alan aralığı. |
| `order_by` | En fazla sekiz skaler alan; eşit değerlerde kayıt ID'si bağlayıcıdır. İndeks sırası uyarsa o sırayla okunur. |
| `descending` | Sıralama ve ID bağlayıcısı birlikte ters döner. |
| `cursor` | Filtre, sıra, store ID ve ilgili koleksiyon generation'ına bağlı keyset cursor. Koleksiyon değişince 409; başka koleksiyon yazısı cursor'u bozmaz. |
| `allow_scan` | Varsayılan `false`. Kullanılabilir indeks yoksa açıkça tarama izni gerekir. |
| `max_scan` | Alınıp filtrelenen belge sayısı üst sınırı. Sınır aşılırsa eksik sonuç dönmek yerine `QueryBudgetExceeded`. |
| `max_index_keys` | RAM yolunda sıralı önbelleğe alınacak bütün farklı indeks anahtarlarının, disk yolunda seçilmiş aralık penceresinin kayıt üyeliklerinin üst sınırı. Daha büyük yol atlanır ve explain'e yazılır. |
| `max_candidates` | Her aday yolunun materyalleştireceği ID sayısının üst sınırı; varsayılan 100000. Tam hash erişimi, RAM/disk aralık yolu ve açık collection scan için allocation öncesi kontrol edilir. |
| `explain` | Koleksiyon/adayı satır sayısı, seçilen yollar, anahtar sayısı, sıralama yolu, bütçeler; çalışan sorguda gerçek `rows_examined` ve `rows_returned`. |

Geçerli [sıralı disk önbelleği](disk_indexes.md) varsa aralık indeksinde mmap ve
binary search kullanılır; kaynak aynı generation'a ait değilse veya cache
bozuksa aynı snapshot'ın RAM indeksine dönülür. RAM aralık indeksi görünüm
içinde gerektiğinde sıralanır ve sonraki sorgularda tekrar kullanılır. Bileşik
indeksin sonraki alanı
aralıkta kullanılırken önündeki alanlar eşitlikle sabitlenmelidir. Aralıktan
sonra kalan filtreler belgede tekrar denetlenir. Bileşik hash indeksi suffix
alanı `null` olan kayıtları içermez. Bu nedenle sadece öneğe dayanan yol,
yalnızca bütün kayıtları kapsadığı veya eksik suffix'in sorguyu zaten
geçemeyeceği doğrulanınca aday kümesini daraltır. Böylece GUID'siz podcast
episode ve silinmiş playlist entry gibi kayıtlar sonuçtan düşmez.

`max_scan` belge fetch sayısını, `max_index_keys` RAM anahtar sıralaması veya
seçilmiş disk penceresini, `max_candidates` tek yoldaki ID setini sınırlar.
Snapshot capture, birden fazla aday ID setinin toplamı/kesişimi ve ID sıralaması
ayrıca bellek/CPU kullanır; bu bütçeler toplam süreç RAM'i veya bütün planlama
CPU süresi için sert kota değildir. Disk cache'in ilk checksum/semantik
doğrulaması bütün indeks dosyasını okur; warm aralık sorgusunun işi explain'de
ayrı gösterilir. Genel sorgular sonsuz
geçmiş sayfasını RAM'de tutmaz; cursor eski görünümü diskte yeniden oluşturmaz.

## Gerçekleştirilen doğrulama

`PYTHONDONTWRITEBYTECODE=1 python tools/query_integrity_acceptance.py`
komutu 21 snapshot/query/domain kontrolü ve 12 integrity kontrolüyle **33/33**
geçti. Araç kendi geçici dizinlerini siler; üretim DB yolu kabul etmez.
Aşağıdaki snapshot kontrolleri bu kabul aracındadır:

- Bileşik indeks üzerinde dahil/haricî aralık sınırı ve iki yönde sıralama.
- Her sayfada en fazla dört belge fetch ederek üçlü keyset sayfalama.
- İki bağımsız indeksin AND kesişimi.
- Nullable bileşik suffix nedeniyle GUID'siz episode veya silinmiş playlist
  entry'nin sonuçtan düşmemesi; non-null range için güvenli sparse index seçimi.
- İç içe dönen sözlük/listenin değiştirilmesinin görünümü etkilememesi.
- Update, gerçek delete ve checkpoint sonrasında eski kayıt ve indeksin kalması.
- Eski `_rev` ile compare-and-swap yazmasının reddedilmesi.
- İndekssiz örtülü taramanın ve izin verilen iş bütçesi aşımının reddedilmesi.
- Yeni görünümde değişen koleksiyon generation'ı için cursor reddi.
- Seçilmeyen koleksiyon ve kapanmış görünüm okumalarının reddi.
- İki Store bağlantısı: 80 atomik iki-kayıt yazması ve dört checkpoint sırasında
  açılan 120 görünümde iki alanın hep aynı commit'e ait olması; sonra bütün eski
  görünümlerin tekrar doğrulanması.
- Gerçek domain item okuması, istasyon dışı içeriğin gizlenmesi ve endpoint
  credential ile detail erişiminin reddi.
- Snapshot sonrası item değişikliği ve view yetkisinin başka istasyona daraltılması:
  eski görünüm tutarlı eski sonucu verir; yeni görünüm yetkiyi reddeder.
- Gerçek `apply_retention` ile audit kayıtları sıcak depodan çıkarılırken eski
  audit görünümünün bütün kayıtlara erişmeye devam etmesi.
