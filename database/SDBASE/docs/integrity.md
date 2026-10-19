# Salt okunur bütünlük taraması

`integrity.inspect_integrity` üretim dosyalarının checksum, kayıt ilişkisi,
indeks ve yönetilen nesne bütünlüğünü ölçer. Veri düzeltmesi uygulamaz.

```python
from sdbase.integrity import inspect_integrity

report = inspect_integrity(
    store,                         # Store, .sdb yolu veya DB klasörü
    max_records=100000,
    max_bytes=1024**3,
    verify_media=True,
    verify_cold=True,
    max_issues=100,
)
```

Genel motor fonksiyonunun kullanıcı yetkisi denetimi yoktur. HTTP çağrısı için
admin katmanı global yönetim yetkisi ister; CLI erişimi dosya izinlerine dayanır.

## Denetlenen şeyler

| Aşama | Denetim |
| --- | --- |
| `storage` | Mevcut metadata, checkpoint ve journal salt okunur dosya tanıtıcılarıyla açılır. Checkpoint payload SHA-256, journal frame SHA-256, uzunluk, JSON ve yeni commit sequence'ları doğrulanır. Bağımsız bellek durumuna motorun `_apply`/indeks kurallarıyla replay edilir. |
| `indexes` | Kayıtlardan indeksler yeniden kurulur; unique tekrarları replay'i başarısız yapar. Denetlenen kayıtların indeks üyeliği ve retention'ın ayırdığı unique kimliklerin tekrar kullanımı kontrol edilir. |
| `records` | Kalıcı envelope, required alanlar, status/lifecycle, şemadaki referanslar ve istasyon kapsamı; kritik opsiyonel plan/category/token referansları; item–version–audio entry ilişkisi; approved playback marker/provenance, execution revision ve distribution snapshot sözleşmesi/hash'i. |
| `records` audit | Sıcak audit sequence'ları, soğuk arşiv registry aralıkları ve toplam commit coverage arasında gap/duplicate olmaması. |
| `managed_media` | Committed media/location kimliklerinden content-addressed nesne envanteri çıkarılır. Tam dosya SHA-256 ve byte uzunluğu birebir eşleşir; eksik/unsafe/symlink dosya veya hash uyuşmazlığı hata verir. |
| `cold_history` | Kayıtlı segmentin manifest ve JSONL dosya yolları, boyutları ve SHA-256'ları; manifest–registry eşitliği, satır envelope'u, duplicate ID, koleksiyon adetleri ve audit sequence aralığı. |

Commit anındaki lifecycle geçiş validator'ları bütün tarihî kayıtlara yeniden
uygulanmaz: onaylanmış plan entry'si veya daha sonra withdrawn olmuş cue, geçerli
geçmiş olabilir. Kalıcı sözleşme kontrolleri bu ayrımı gözetir. Eski distribution
contract v1 yalnız history denetiminde okunabilir; tarama ona yayın yetkisi vermez.

Kaynak kilidi ve bütün dosyalar önceden var olmalıdır. Scrub `.lock` dosyası
oluşturmaz, journal'ı `r+b` ile açmaz, `repair_tail` çağırmaz, checkpoint yazmaz ve
metadata'yı değiştirmez. Yarım journal kuyruğu `invalid` olarak raporlanır;
kuyruk olduğu gibi kalır. Bilinen bir hash bozukluğu tekrar hash yazılarak
"düzeltilmez".

## Bütçeler ve sonuç

- `max_bytes`, metadata/checkpoint/journal için okunan ve media/soğuk arşiv için
  hash edilen byte'ların toplam üst sınırıdır. Sonraki tam dosya sığmıyorsa o
  dosya okunmadan tarama `incomplete` olur. Tam limite denk gelmek hatalı
  "budget exhausted" sonucu üretmez.
- `max_records`, sıcak kalıcı invariant kontrolü + okunan soğuk JSONL satırı
  sayısını sınırlar. JSON checkpoint ve journal replay'i önce tamamlanmış
  görüntüyü kurar; decode/replay satır sayısı bu validation bütçesinden ayrı
  olabilir. `max_bytes` ve makine RAM'i birlikte boyutlandırılmalıdır.
- `max_issues` saklanan hata ayrıntısı sayısını sınırlar. Fazla hatalar toplam
  `issue_count` içinde sayılır ve `issues_truncated=true` ile belirtilir.
- Dosya kilidi beklemesi 30 saniyede `incomplete` olur. Checksum/replay sırasında
  ortak kilit tutulur; yazıcı bu bölüm bitince ilerler. Hash kontrolü daha sonra
  bu görüntünün committed immutable nesnelerini inceler. Dışarıdan değiştirilen
  nesne, boyut/zaman/hash denetiminde hata verir.
- `verify_media=false` veya `verify_cold=false`, yapılmayan kapsama `skipped`
  yazar ve tam `ok` sonucu vermez.

| `status` | Anlam |
| --- | --- |
| `ok` | İstenen tam kapsam bitmiş ve hata bulunmamış. |
| `invalid` | En az bir somut bütünlük hatası bulunmuş; `complete` ayrıca taramanın bütün kapsamı bitirip bitirmediğini gösterir. |
| `incomplete` | Hata bulunmadığı görülen kısım dışında, bütçe/kilit/scope nedeniyle denetlenemeyen kısım kalmış. Sağlıklı olduğu varsayılmaz. |

Dönen belge `checks`, `work`, `limits` ve `issues` içerir. `work.bytes_read`,
`records_checked`, `hot_records_checked`, `cold_records_checked`, `files_read`
ve `journal_frames` gerçek yapılan işi bildirir. Tarama fiziksel ses çıkışını,
çalışan playout motorunu, network quorum'u veya arızalı donanımın fsync
sözleşmesini doğrulamaz.

## Tekrar çalıştırılabilir kabul

```sh
PYTHONDONTWRITEBYTECODE=1 python tools/query_integrity_acceptance.py
```

Araç üretim DB yolu almaz. Kendi TemporaryDirectory'sinde `.sdb`, WAV medya,
retention arşivi ve bozuk kopyalar oluşturur; çıkışta hepsini siler. Opsiyonel
`--report yeni-dosya.json` sonucu yazar; mevcut raporu ezmez.

**33/33 kabul kontrolü geçti:** 21 snapshot/query/domain davranışı ve şu 12
bütünlük davranışı:

1. Gerçek yönetilen WAV + checkpoint + cold history ile tam `ok`.
2. Bir byte bütçede `incomplete`.
3. Bir kayıt bütçede `incomplete`.
4. Tam okunan byte miktarına eşit bütçede `ok`.
5. Kapatılan medya doğrulamasıyla `incomplete`.
6. Yarım journal kuyruğunun tespiti ve byte'larının korunması.
7. Checkpoint hash'i değiştirilmeden payload değişikliğinin tespiti.
8. Hash yeniden hesaplanmış olsa da orphan station-item referansının tespiti.
9. Hash yeniden hesaplanmış olsa da duplicate unique username'in tespiti.
10. Aynı boyutta yönetilen medya bozukluğunun tespiti.
11. Aynı boyutta soğuk kayıt dosyası bozukluğunun tespiti.
12. Orijinal byte'lar geri konulunca tekrar tam `ok`.
