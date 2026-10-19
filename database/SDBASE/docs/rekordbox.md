# Rekordbox uyumluluk sınırı

Rekordbox entegrasyonu ek kullanıcı gereksinimidir. Kaynak yönü **Rekordbox → RadioS**; XML kaynak dosyası salt okunur açılır. XML 1.0.0 aktarımında `TrackID`, başlık, sanatçı, BPM, tempo/işaret ve playlist sırası kaynak kapsamlı kimlikle ilişkilendirilir. Önizleme, kayıp/boş/erişilemeyen alan ayrımı ve kaynak alan politikası uygulanır; eksik kaynak alanı mevcut RadioS değerini temizlemez.

Rekordbox yerel uygulama veritabanı, Device Library ve OneLibrary ayrı formatlardır.
RadioS masaüstü köprüsü artık DeviceSQL `export.pdb` için ayrı salt okunur
tracklist okuyucusu kullanır; XML'e dönüşüm yapmaz. Yerel uygulama veritabanı
ve OneLibrary desteklenmez. Kaynak kütüphaneyi değiştiren işlem yapılmaz.
[USB kullanımı ve kabul](../../../docs/rekordbox-usb-tracklist.md).

| Kaynak / ortam | Okuyucu sürümü | Aktarım yönü ve veri grupları | Test durumu / sınır |
|---|---|---|---|
| Dışa aktarılmış XML, `DJ_PLAYLISTS Version="1.0.0"`; dosya erişilebilen Python 3.11+ ortamı | SDBASE 0.5.0 `rekordbox.py` | Rekordbox → RadioS; TrackID, temel başlık/katkı alanları, playlist klasör/sıra, TEMPO ve POSITION_MARK kaynak anlık görüntüsü | Geçici XML örneğiyle okuma, önizleme, atomik/idempotent kayıt doğrulandı. Ses dosyası kendiliğinden doğrulanmaz; akıllı liste kuralları/kapak/dalga biçimi kapsam dışıdır. |
| Yerel uygulama veritabanı | Yok | Yok | Şema ve güvenli salt okunur erişim doğrulanmadı. |
| Device Library (USB/SD), `PIONEER/rekordbox/export.pdb` | `rekordbox_device.py`, `devicesql-pdb-1` | Rekordbox → RadioS; temel metadata, klasör ve sıralı tracklist | Özel Store ve C/GTK preview/apply, bozuk veri, USB çıkarılması ve kayıp yanıttan tekrar sınanır. GitHub gerçek CDJ örneği: 651 parça / 40 sıralı öğe. ANLZ/cue ve fiziksel CDJ kabulü kapsam dışıdır. |
| OneLibrary (USB/SD) | Yok | Yok | Device Library ile aynı şema varsayılmadı; doğrulanmış okuyucu yok. |

| XML kaynağı | RadioS hedefi | Tür/birim ve koruma politikası |
|---|---|---|
| `TrackID` + kalıcı `library_key` | `external_ids` → item/sürüm | Kaynak kapsamında kimlik; başlık benzerliğiyle remiksler birleştirilmez. |
| `Name` | `items.title` | Boş hedefi doldurur; onaylı veya editoryal başlığı sessizce ezmez. |
| `Artist`, `Composer`, `Remixer`, `Album`, `Genre`, `Year`, `Comments` | Yapılandırılmış `metadata_values` veya inceleme önerisi | Alan tanımı ve kaynak politikası gerekir; yoksa kaynak anlık görüntüsünde kalır ve kayıp raporlanır. |
| `AverageBpm` | Sürüm metadata önerisi/değeri | BPM sayısı DJ kaynağıdır; editoryal alan politikası uygulanır. |
| `Location` | Kaynak URI/yol önerisi | İzinli kök eşlemesi yalnız yol önerir; medya hazır/uygun sayılmaz. |
| `TEMPO`, `POSITION_MARK` | `dj_analysis` | Ham değer ve saniye birimi korunur; radyo cue'su yalnız açık anlamlandırma ve onaydan sonra ayrı `radio_cues` kaydı olur. |
| Playlist klasörü, girdi ve sıra | `import_playlist_nodes`, `import_playlist_entries` | Sıra ve tekrar korunur. XML'de sabit node ID bulunmadığından node kimliği kaynak anlık görüntüsüne bağlıdır. |
| DJ çalma sayısı, rating ve diğer eşlenmemiş alan | Kaynak anlık görüntüsü/kayıp raporu | RadioS yayın veya reklam gerçekleşmesi olarak yazılmaz. |

Profil `settings.py` içinde sürümlüdür. Önizleme `preview_hash` verir; uygulama bu özeti ve sabit `idempotency_key` değerini ister. Kısmi seçimde görünmeyen parçalar silinmiş sayılmaz. Okuyucu en fazla 64 MB XML kabul eder; tek atomik veritabanı işlem çerçevesi 32 MiB ile sınırlıdır, daha büyük uygulama kısmi yazma yapmadan hata verir.
