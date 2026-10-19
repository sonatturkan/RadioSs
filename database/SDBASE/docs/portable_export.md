# Taşınabilir JSONL/CSV veri dışa aktarımı

`portable_export.py`, SDBASE'in **mevcut kayıtlarının** genel veri paketini üretir. `portable_import.py` aynı şema sürümündeki paketi **yeni, boş bir hedefte** ilişkileriyle geri okur. Bu yol, ses baytlarını kapsayan fiziksel `.sdb` yedeği veya üçüncü taraf ürüne aktarım protokolü değildir. [Fiziksel yedek](../src/sdbase/backup.py) ses nesnelerini ve kurtarma için gereken motor dosyalarını ayrıca taşır.

## Kullanım

Kurulu `sdbase` CLI ile aynı işlemler:

```sh
sdbase --db /veri/RadioS.sdb portable-export \
  --token YONETICI_TOKENI --destination /exports/radios-2026-10-01
sdbase portable-verify --package /exports/radios-2026-10-01
sdbase portable-restore --package /exports/radios-2026-10-01 \
  --target-dir /yeni/radios-copy
```

Son komut yeni yönetici tokenını yalnız bir kez JSON sonucunda döndürür.

```python
from sdbase.portable_export import create_portable_export, verify_portable_export
from sdbase.portable_import import restore_portable_export

result = create_portable_export(store, principal, "/exports/radios-2026-10-01")
check = verify_portable_export(result["destination"])
assert check["valid"]

restored = restore_portable_export(result["destination"], "/new/empty/radios-copy")
# restored["admin_token"] yalnız bir kez döner; güvenli yerde teslim edin.
```

Hedefin üst dizini mevcut olmalı; hedef dizin **önceden bulunmamalıdır**. Servis, kendi geçici dizininde tüm dosyaları yazar, `fsync` eder ve Linux `renameat2(RENAME_NOREPLACE)` ile tek adımda yayımlar. Yarışta hedef oluşursa var olan dizin üzerine yazmaz. Başarısızlıkta geçici dizin temizlenir. Kaynak `.sdb` günlüğü, meta, checkpoint ve medya nesnelerine yazmaz; aynı işlem dizisi için iki paket bayt düzeyinde aynıdır.

`principal` etkin kullanıcı kimliği olmalı ve **bütün istasyonlarda** `manage` izni bulunmalıdır. Yetki, kopyalanan kayıtlarla aynı kaynak anlık görüntüsünde denetlenir. Başka işlem sonradan yazsa bile paketteki her koleksiyon tek commit dizisinin durumunu gösterir.

## Paket düzeni ve kayıp sınırı

| Dosya | İçerik |
|---|---|
| `manifest.json`, `manifest.sha256` | Biçim, kaynak mağaza kimliği/şema sürümü/commit dizisi, koleksiyon ve satır sayıları, dosya SHA-256/boyutları, ilişki sözlüğü ve açık dışlamalar. İmza değildir. |
| `collections/<ad>.jsonl` | Her kayıt için `_id`, `_rev`, tüm `data` alanları; koleksiyon ve ID sırası sabit, JSON anahtarları sıralı. |
| `collections/<ad>.csv` | Aynı kayıtlar. `_id`, `_rev` ve `field:<alan>` sütunları; dolu her hücre türünü koruyan kanonik JSON değeri, boş hücre ise alanın yokluğu. `null` ile eksik alan ayrılır. |
| `media_manifest.json` | Her `media_file_id` için sürüm, durum, SHA-256, boyut ve bağlı yönetilen konumların kimlik/durum/URI/hash/boyutu. Medyanın bütün diğer alanları da ilgili koleksiyon dosyalarında tam saklanır. |
| `tombstones.json` | Silinmiş kayıt kimlikleri; temiz mağazada aynı kimliğin yeniden kullanılmasını önlemek için korunur. |

Koleksiyonlar arasında `items`, `versions`, `station_items`, kişiler/katkılar, plan/revizyon/girdiler, reklam ilişkileri, haklar, `radio_cues`, medya dosyaları/konumları, dış eşlemeler ve kayıtlı iş sonuçları bulunur. Mevcut kayıtlardaki ilişki UUID'leri, sürüm numaraları, ham kaynak alanları ve onay izleri değiştirilmeden yazılır. Boş tanımlı koleksiyonların da JSONL/CSV dosyaları vardır. Ses **baytları** pakete eklenmez; hash ve konumlar yalnız referanstır. Bu yüzden bu paket tek başına ses çalabilen bir kurtarma kopyası değildir.

`tokens` koleksiyonu dışarı alınmaz. Motorun `_audit` koleksiyonu eski token satırlarının önce/sonra görüntülerini de tutabildiği için dışarı alınmaz. Bu iki dışlama manifestte açıkça görünür; diğer koleksiyonlar için sessiz alan kırpma yapılmaz. Başka bir kayıtta `token`, `password`, `secret`, `authorization`, `credential`, `api_key`, `private_key` gibi adlandırılmış alan ya da `Bearer`/URL kullanıcı bilgisi örüntüsü görülürse dışa aktarım **tamamen reddedilir**. Kullanıcının sıradan başlık/açıklama alanına gizlediği her olası sırrı kesinlikle tanımak mümkün değildir; böyle alanlar yönetici tarafından ayrıca gözden geçirilmelidir. Paket, kullanıcı/izin/iş kayıtları içerdiğinden erişim denetimli yerde saklanmalıdır.

## Geri okuma denetimi

`verify_portable_export()` manifest hash'ini, bildirilen bütün dosyaların hash ve boyutunu, beklenmeyen dosya/symlink olmadığını, JSONL sırasını ve CSV'nin **aynı veri ile türleri** taşıdığını denetler. Bildirilen yabancı anahtarlar ve polimorfik hedefler aranır; medya konumu hash, boyut ve URI'si dosya kaydıyla eşleşir. Bu işlem **yalnız paketi okur**; `.sdb` oluşturmaz. Bozulmuş veya eksik paket hata verir. Hash denetimi kazara bozulma ve iç tutarlılık içindir; imzalı köken kanıtı veya ses baytlarının doğrulaması değildir.

Şemada tanımlı koleksiyonların zorunlu alanları, durumları, yabancı anahtarları ve önemli istasyon/servis kapsamları geri yüklemeden önce denetlenir; bütün indeksler temiz mağazada yeniden kurulur. Yazma anına özgü geçiş kuralları tarihsel onaylı satırlara tekrar uygulanmaz. Şema dışı özel koleksiyonlar tüm alanlarıyla taşınır, fakat bilinmeyen iç içe ilişkileri otomatik keşfedilemez.

## Temiz kurulumda geri yükleme

`restore_portable_export()` önce paketin tamamını doğrular, sonra JSONL kayıtlarını beklenen hash'lerle tekrar okuyarak yeni `RadioS.sdb` oluşturur. Kaynak paket salt okunur kalır. Hedefin üst dizini bulunmalı, hedef **hiç var olmamalıdır**; mevcut bir mağazaya birleştirme veya üzerine yazma yapılmaz. Geçici mağaza RadioS indeksleriyle açılır, güvenlik durumları düzeltilir ve hedefe atomik olarak yayımlanır. Aynı şema sürümü zorunludur; eski sürüm için otomatik dönüştürme yoktur.

Koleksiyon kayıtlarının `_id`, `_rev` ve `data` değerleri ilk yüklemede korunur; ilişkiler aynı kimliklere bakar. Kaynak tombstone'ları geri yüklenir. Kaynak commit dizisi yalnız manifestte kalır: yeni mağaza işlem ve denetim günlüğüne **sıra 1** ile başlar; koleksiyon değişim dizileri sıfırlanır. Böylece eski `_audit` kayıtları olmadan sahte bir boşluk oluşmaz. Güvenlik için değiştirilen canlı durum kayıtlarının revizyonu daha sonra artar. `tokens` ve `_audit` geri gelmez. Mağaza kimliği yeni oluşturulur; sonuçtaki `admin_token` 30 gün geçerli yeni bir yönetici kimliğidir, veritabanında yalnız hash'i durur. Eski kullanıcılar ve izinleri kalır; eski anahtarları çalışmaz ve gerekirse yeniden üretilir.

Yeni hedef, kaynak motorun canlı sahipliğini devralmaz: uçlar çevrimdışı, uç medya kopyaları beklemede, kabul ve teslimler pasif, motor komutları sona ermiş durumdadır; çevrimdışı aktivasyon yetkileri kaldırılır. Canlı override komutları kapatılır; anlık çalan/sıradaki/talk gösterimleri temizlenir. Kaynaktaki etkin acil uyarılar iptal, kayıt işleri devre dışı bırakılır. Tarihsel `recording_attempts` ve `emergency_alert_attempts` kanıtları değiştirilmez; yarım kalmış bir denemeye gerçekte yaşanmamış bitiş olayı eklenmez. Yönetici bunları ve devre dışı bırakılmış işleri incelemelidir.

Ses baytları bulunmadığı için tüm hazır medya konumları `missing` olarak işaretlenir ve çalınabilirlik yeniden doğrulanana kadar engellenir. `media_manifest.json` gerekli hash/boyut ve eski konum kimliklerini verir; ses nesneleri güvenli ayrı aktarım ve mevcut `media.stage_media` ile `media.reconcile_media` akışlarıyla tamamlanmalıdır. Bu geri yükleme, **katalog ilişkilerinin temiz mağazada okunabildiğini** sağlar; tek başına çalışır radyo yayını veya fiziksel felaket kurtarma sağlamaz. Üçüncü taraf ürün formatı uyumu iddia edilmez.

## 0.6.0 marker ve cold-history sınırı

Schema v3 marker setleri ve onaylı revision binding'leri kimlik/revizyonlarıyla korunur; null segue referansları geçerlidir. Restore persistent marker/media/cue ve execution binding ilişkilerini doğrular. Retention cold segmentleri bulunan kaynak portable export yapamaz: audit ve cold baytlarını dışarıda bırakan paket tam kurtarma sayılmaz. Bu kaynak için cold-file manifestli fiziksel backup/restore kullanılır.
