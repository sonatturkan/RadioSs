# Kalıcı sıralı disk indeksleri

`src/sdbase/disk_index.py`, SDBASE'in kendi `.sdb` belge motoruna isteğe bağlı,
yeniden üretilebilir sıralı indeks önbellekleri ekler. SQL veya başka bir
veritabanı motoru çalıştırmaz. Geçerli önbellek varsa bileşik eşitlik öneği,
aralık ve uygun sıralama dosyadaki anahtarlara binary search ile erişir.
Tam bileşik eşitlik mevcut RAM hash indeksinin O(1) erişimini kullanır.

## Kaynak incelemesi ve uyarlama

| İncelenen kaynak | İncelenen bölüm | SDBASE uygulaması |
| --- | --- | --- |
| SQLite `src/btree.c` | Hücre konumları, `moveToRoot`, `sqlite3BtreeIndexMoveto` | Değişken uzunluktaki anahtar kayıtlarına sabit boyutlu offset tablosuyla erişim. |
| PostgreSQL `src/backend/access/nbtree/nbtsearch.c` | `_bt_search`, `_bt_binsrch` | Sıralı anahtarlarda aralık sınırının binary search ile bulunması. |

Yerel dosyaların SHA-256 değerleri:

- SQLite `btree.c`: `ec34eaeecddfc31507abe3492f4b57c7f2feb8b991e4b08e8218e32e359acb35`
- PostgreSQL `nbtsearch.c`: `1270d65b1af5c27e5b293dffa2cbcab00c983a6dd0a1e9814ff983117cdd1cc0`

İlgili birincil açıklamalar: [SQLite dosya biçimi](https://www.sqlite.org/fileformat.html)
ve [PostgreSQL B-tree indeksleri](https://www.postgresql.org/docs/current/btree.html).
SQLite `LICENSE.md` ve PostgreSQL `COPYRIGHT` dosyaları da incelendi.
Burada C kaynak kodu kopyalanmadı; veri biçimi ve Python uygulaması SDBASE'e
aittir. Bu modül SQLite sayfalarını veya PostgreSQL indeks dosyalarını okumaz.

## Kullanım

```python
from sdbase.disk_index import (
    rebuild_disk_indexes, inspect_disk_indexes, recover_disk_index_builds,
)

build = rebuild_disk_indexes(
    store,
    collections=["station_items"],
    index_names={"station_items": ["station_item"]},
    max_rows=1000000,
    max_bytes=2 * 1024**3,
    sort_chunk_rows=20000,
    lock_timeout=30.0,
)
status = inspect_disk_indexes(store)
abandoned = recover_disk_index_builds(store)
# Yalnızca SDBASE'e ait, aktif olmayan eski hazırlıkları temizler.
cleanup = recover_disk_index_builds(store, cleanup=True, lock_timeout=30.0)

with store.read_snapshot(["station_items"]) as view:
    page = view.query(
        "station_items",
        equal={"station_id": station_id},
        ranges={"item_id": {"minimum": first_item_id, "maximum": last_item_id}},
        order_by=("item_id",), limit=20,
        max_scan=64, max_index_keys=64, max_candidates=64,
    )
```

| İşlev / parametre | Sözleşme |
| --- | --- |
| `rebuild_disk_indexes` | İndeksleri sabit snapshot'tan hazırlar; kaynak hâlâ aynıysa primary node'da yayımlar. `Conflict` veya `DiskIndexError` eski manifesti değiştirmez. |
| `collections=None` | Tanımlı bütün koleksiyonlar; string veya isim listesiyle daraltılabilir. |
| `index_names=None` | Seçilen koleksiyonların bütün indeksleri; sözlükle indeks adı listeleri seçilebilir. |
| `max_rows=1000000` | İstenen indekslerin toplam kayıt üyeliği bütçesi; aynı belge iki indekste iki üyelik sayılır. |
| `max_bytes=2 GiB` | Geçici sıralama dosyaları, her merge geçişi, son dosyalar ve manifest yazıları için toplam yazılan bayt bütçesi. Son önbellek boyutundan büyüktür. |
| `sort_chunk_rows=20000` | Sıralanan bir parçada en fazla bu kadar üyelik; izin verilen aralık 256–100000. Bu parametre toplam RAM bayt kotası değildir. |
| `lock_timeout=30.0` | Hazırlama öncesi kaynak denetimi ve son yayın kilidi için sınırlı bekleme; 0–300 saniye aralığında, sıfır hariç. |
| `inspect_disk_indexes` | Manifest, dosya checksum'ları, sıralama ve gerçek kaynak üyeliğini denetler; `valid`, `stale_or_invalid` veya `unavailable`. İlk inceleme bütün indeks kayıtlarını okuyabilir. |
| `recover_disk_index_builds` | Job kilidiyle aktif hazırlığı dışlar; kendi owner marker'ı olan sahipsiz stage/eski build dizinlerini listeler. `cleanup=True` yalnız uygun dizinleri kaldırır. |

Bir rebuild yeni manifestin tamamını yayımlar: yalnız bazı indeksleri seçmek,
eski manifestteki seçilmeyen indeksleri yeni manifestte tutmaz. Kaynak
journal'ına yazmaz, sequence artırmaz. CLI karşılıkları:

```sh
sdbase --db /path/radio.sdb index-rebuild --collection station_items
sdbase --db /path/radio.sdb index-status
sdbase --db /path/radio.sdb index-recover
sdbase --db /path/radio.sdb index-recover --cleanup
```

İndeks adını seçmek Python API'sinde desteklenir; CLI `--collection` seçilen
koleksiyonun tüm indekslerini üretir. Yerel `query --query-json` komutu
`max_candidates` dahil sorgu sözlüğünü doğrudan aynı snapshot API'sine verir.
Genel koleksiyon sorgusu bir HTTP kullanıcı endpoint'i değildir.

## Dosya biçimi

```text
radio.sdb
radio.sdb.indexes/
  .build.lock
  manifest.json
  BUILD_UUID/
    build.json
    index-manifest.json
    COLLECTION_INDEX_HASH.keys
    COLLECTION_INDEX_HASH.offsets
```

- Manifest zarfı kanonik JSON payload ve SHA-256 içerir. Store UUID, schema
  version, build UUID, kaynak sequence ve her indeksin collection generation,
  alanları/unique tanımı, kayıt/anahtar sayıları ve tam dosya hash'leri sabitlenir.
- `.keys` dosyası tipli skaler anahtar dizisi ve belge ID'sini içeren JSONL
  kayıtlarıdır: `{"key":["station-uuid","item-uuid"],"id":"association-id"}`.
  Her üyelik ayrı satırdır; tek anahtar için sınırsız ID listesi serileştirilmez.
- Sayısal değerler stringlerden önce sıralanır. Bool ve tam sayı değerli float,
  mevcut hash indeksinin Python eşitliğiyle uyumlu şekilde tam sayıya çevrilir.
  Son bağlayıcı belge ID'sidir. İndeks anahtarındaki `null` alanlar mevcut motor
  davranışıyla aynı şekilde indeks dışında kalır.
- `.offsets` başlığı big endian `>8sQQ`: `SDBIDX01`, üyelik sayısı ve `.keys`
  boyutu. Ardından üyelik sayısı + 1 adet big endian uint64 offset gelir;
  son değer EOF'tur. Başlık 24 bayt, her offset 8 bayttır.
- Dosya yolları build UUID altındadır; symlink ve dizinden çıkış reddedilir.
  Manifest en fazla 4 MiB, bir anahtar kaydı en fazla 32 MiB olabilir.
  Okuyucu checksum doğrulaması her bileşen için en fazla 2 GiB destekler.

## Online üretim ve çökme recovery

Kaynak snapshot ortak kilit altında alınır; kayıt gövdelerinin ve indeks
üyeliklerinin görünümü sabittir. Anahtar üretimi ve sıralama sırasında kaynak
veritabanının exclusive kilidi tutulmaz. Sıralama parçalara ayrılır; en fazla
32 run bir merge geçişinde açılır. Daha çok run varsa birden çok merge geçişi
yapılır. Her yazı bayt bütçesine dahil edilir ve son dosyalar fsync edilir.

Son kısa exclusive bölümde primary rolü/epoch, Store UUID, snapshot'ın schema
sürümü, indeks tanımı ve ilgili collection generation yeniden denetlenir.
İlgili koleksiyon değişmişse yayın reddedilir. Başka koleksiyondaki bağımsız
yazı aynı indeksi geçersiz kılmaz. Stage dizini build UUID adına taşınır ve
manifest atomik değiştirilir; dizin fsync edilir.

Hazırlayan süreç SIGKILL ile durursa kernel job kilidi bırakır. Kendi
`build.json` işaretçisi olan `.build-UUID` dizini recovery ile bulunur.
Aktif build temizlenemez; geçerli mevcut manifestin gösterdiği build korunur.
Manifest bozuk/belirsizse tamamlanmış UUID dizinleri korunur; yalnız işaretli
stage'ler temizlenebilir. Kullanıcının işaretsiz dizinlerine dokunulmaz.

Önbellek türetilmiştir: backup/restore için gerekli değildir. Silinirse kaynak
veriler aynı kalır ve sorgu RAM indeksine döner; disk performansı için yeniden
üretilir. Güncel olmayan önbellek otomatik artımlı güncellenmez; yeniden build
gerekir. Uzun ömürlü snapshot açık mmap dosyalarını kapanana kadar tutabilir.

## Okuma, doğruluk ve iş bütçeleri

Geçerli generation/spec/kapsam için dosyalar salt okunur mmap ile açılır;
binary search yalnız aralık konumları için gereken offset/anahtarları okur.
Sorgu sıralı anahtarların tamamını Python listesine yüklemez veya sıralamaz.

İlk erişim dosya SHA-256'larını **tam okuyarak** doğrular. Ayrıca offset
sırası, kayıtların kesin anahtar/ID sırası, tekillik, anahtar sayısı ve her
üyeliğin gerçek snapshot koleksiyonuna aitliği akış halinde doğrulanır.
Dolayısıyla cold açılış O(indeks boyutu + üyelik sayısı) iş yapar. Doğru
hash'leri olan eksik, ters sıralı veya başka koleksiyona alias edilmiş bir
dosya binary search sonucu olarak kabul edilmez.

Checksum ve semantik doğrulama sertifikaları süreç içinde en fazla 256 giriş
tutar; belge gövdelerini tutmaz. Semantik sertifika dosya device/inode/size/
mtime/ctime/hash, Store/schema, collection/index/spec/generation ve kaynak
Store handle/checkpoint/node/timeline/epoch kimliğine bağlıdır. Aynı Store
görünümüyle tekrar açılan warm sorguda tam semantik tarama tekrar edilmez.
Yeni Store bağlantısı, süreç restart'ı veya checkpoint kaynak değişimi tam
semantik doğrulamayı yeniden yapabilir. ctime veya inode değişimi aynı boyut ve
geri alınmış mtime ile değiştirilmiş cache dosyalarını da geçersiz kılar.

Bozuk, eksik veya eski cache ilgili snapshot'ın normal RAM indeksine döner.
Okuma sırasında değişiklik saptanırsa sorgu aynı snapshot üzerinde RAM yoluyla
yeniden hesaplanır. Cache hatası sonucu belge düşürülmez. RAM yolu açık iş
bütçesine sığmıyorsa sistem eksik sonuç vermek yerine `QueryBudgetExceeded`
döndürür; kullanıcı filtreyi daraltabilir veya bütçeyi artırabilir.

| Sorgu bütçesi | Denetlenen iş |
| --- | --- |
| `max_scan` | Fetch edilip filtrelenen belge sayısı. |
| `max_index_keys` | RAM yolunda sıralı anahtar önbelleğine girecek bütün farklı anahtarlar; disk yolunda binary search ile seçilmiş pencerenin üyelik sayısı. Büyük indeks üzerinde küçük disk aralığı bu bütçeye sığabilir. |
| `max_candidates=100000` | Her aday yolunun materyalleştireceği ID sayısı; tam hash lookup, disk pencere, RAM aralık birleşimi ve açık collection scan için allocation öncesi kontrol. |

Bir indeks yolu bütçeyi aşarsa explain'de nedeni görülür; daha küçük başka bir
yol seçilebiliyorsa kalan filtreler belgelerde denetlenir. Geniş önek için
bütün aday ID'leri hâlâ sınırlı bir set içinde tutulur. Birden fazla seçilen
yolun setleri ve kesişimi ek bellek kullanır; bu bütçeler toplam süreç RAM kotası
değildir. Sorgu sayfalaması bütün eşleşme kümesini diskte geçici olarak saklamaz.

`explain.paths[].backend` gerçek `disk`/`memory` yolunu gösterir. `disk_work`
şunları ayrı bildirir:

- `verification_bytes`: cold checksum için okunan dosya baytları.
- `verification_records`, `verification_key_bytes`: cold semantik kapsam taraması.
- `offset_probes`, `key_records_read`, `key_bytes_read`: binary search, pencere
  seçimi ve sorgu sıralamasının gerçekten yaptığı işler.

`ordered_keys_built=0` bir disk yolunda sıralı RAM anahtar dizisi üretilmediğini
gösterir; ilk doğrulamanın ücretsiz olduğunu göstermez.

### Kalan motor sınırı

Journal replay hâlâ sıcak belge gövdelerini ve hash indeks üyeliklerini RAM'de
kurar. Snapshot capture seçilen kayıt eşlemeleri ve üyelikleri kopyalar.
Disk cache, bu maliyeti kaldırmaz. mmap ayrıca işletim sisteminin sayfa
önbelleğini kullanır. Bu ek özellik bütün motoru disk sayfalı B-tree/LSM motoruna
dönüştürmez; milyonlarca sıcak belge için kaynak motor RAM planlaması gerekir.

## Gerçekleştirilen kabul doğrulaması

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tools/disk_index_acceptance.py
PYTHONDONTWRITEBYTECODE=1 python3 tools/query_integrity_acceptance.py
```

2026-10-01 tarihindeki son çalıştırma: disk indeksi **37/37**, mevcut
snapshot/query/integrity kontrolleri **33/33** geçti. Araçlar kendi geçici
dizinlerini oluşturup siler; üretim DB yolu kabul etmez. Disk aracı 30000 belge,
iki indeksle 60000 üyelik ve 256 kayıtlık parçalarla gerçek çok geçişli merge
yapar. Gerçek ayrı süreçte eşzamanlı update sırasında stale publication reddi,
aktif job kilidi ve SIGKILL sonrası recovery doğrulanmıştır.

Kapsam ayrıca restart, iki yönde aralık/keyset, tam eşitlik hash yolu, disk/RAM
bütçe reddi, eski snapshot, nullable suffix, sayısal/string anahtarlar,
checksum bozulması, tekrar hesaplanmış hash ile ters sıralı veri, farklı
koleksiyonun dosyasına alias, checksum geçerli yanlış manifest şekli, boş
exclusive aralık, cold/warm semantik sertifika ve mmap resource kapanmasını
içerir. Bu yerel kabul donanım arızası veya tüm motorun disk tabanlı ölçeği
kanıtı değildir.
