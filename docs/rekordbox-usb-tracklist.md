# rekordbox USB / SD tracklist

Playlist ekranında **rekordbox USB / SD oku**, CDJ'lerin kullandığı
**Device Library / DeviceSQL `export.pdb`** dosyasını doğrudan okur. XML'e
dönüştürme veya harici dönüştürücü/Python paketi kurulması gerekmez.

## Kullanım

1. rekordbox ile Device Library dışa aktarılmış USB/SD'yi bilgisayara bağla.
2. Playlist → **rekordbox USB / SD oku** penceresini aç. Varsayılan kaynak
   **USB / SD · CDJ Device Library**'dir.
3. Bağlı aygıtı seç veya USB'nin kök klasörünü göster. `PIONEER` ve
   `PIONEER/rekordbox` klasörleri de seçilebilir.
4. `Klasör / playlist` adlarıyla gösterilen listeleri seç; mevcut listeye
   ekleme/yeni kopya ve tekrarları atlama/koruma politikasını ayarla.
   Kaynakta tekrar eden parçaları aynı sırayla almak için **Tekrarları koru**
   seçilir.
5. **Önizlemeyi doğrula** ile hedefleri ve uyarıları incele; ardından
   **Seçilenleri içeri aktar** ile düzenlenebilir native playlist oluştur.

Parça sırası fiziksel dosya/row sırasından değil, kaynak playlist'in sıra
numaralarından okunur. Klasör sırası ve Unicode metadata korunur. Kayıt
sonrasında normal playlist düzenleme/sıralama akışı kullanılır. Aynı USB'nin
sonraki aktarımlarında kaynak kütüphane kimliği korunmalıdır; aygıt UUID'si
veya manuel klasör özeti başlangıç kimliğini verir.

## Destek ve sınırlar

- Beklenen dosya: `USB/PIONEER/rekordbox/export.pdb`.
- **OneLibrary / Device Library Plus `exportLibrary.db` ve yerel rekordbox
  uygulama veritabanı desteklenmez.** USB yalnız bunları içeriyorsa açık hata
  gösterilir; XML gibi yorumlanmaz.
- ANLZ waveform, beatgrid ve hot cue dosyaları okunmaz; önizleme bu kaybı
  bildirir. DJ işaretleri veya radyo cue onayı uydurulmaz.
- Sesler otomatik kopyalanmaz/çalınmaz. Parçalar USB altındaki kaynak yolu
  ve `unverified` durumu ile kaydedilir. Çalmak için sesleri medya
  kütüphanesine ayrıca alıp playlist öğelerine bağla. Hak/onay ve yayın
  hazırlığı ayrı işlemlerdir.
- `export.pdb` en fazla **16 MiB**, sıkıştırılmış kopyası **4 MiB**, keşif
  **50.000 koleksiyon parçası / 500 playlist** kabul eder. Bir hedef liste
  **500 öğeyi**, tek işlem **5.000 yeni playlist üyeliğini** aşamaz. Büyük
  koleksiyonlarda listeleri ayrı ayrı seç.
- Yerel rekordbox preview/apply JSON isteği **6 MiB** ile sınırlıdır; diğer
  HTTP yolları ve C yanıt limiti **2 MiB** olarak kalır.
- **Eski XML · uyumluluk** ayrı kaynak seçeneğidir; varsayılan USB akışında
  XML okuyucusu kullanılmaz. Önceki XML alındılarının tekrarı korunur.

## Salt okuma ve kesinti

USB dosyaları yalnız okunur; Device Library, ANLZ veya seslere yazılmaz.
Sayfa zincirleri, silinmiş satırlar, sınır dışı işaretçiler, geçersiz playlist
referansları ve kökten kaçan medya yolları denetlenir. Dosya okunurken
değişirse kaynak yeniden seçilmelidir.

Önizleme SHA-256 bağlı kaynak kopyasını kullanır. Kayıt yanıtı kaybolduğunda
aynı kaynak, seçim, politika, hash ve işlem kimliğiyle tekrar denenir.
Önizlemeden sonra USB çıkarılsa da kopya korunur. Hedef değişmişse eski
önizleme hiçbir satır yazmadan reddedilir. Testler özel veritabanlarında
çalışır; kullanıcı çalışma veritabanına fixture yazılmaz.

Güncelleme için uygulama ve veri servisi yeni kodla yeniden başlatılmalıdır.
Aktif yayın oturumu otomatik kapatılmaz.

Native HTTP köprüsü `/v1/workflows/desktop-rekordbox/preview,apply` yollarını
korur. USB isteğinde `pdb_zlib_base64` (zlib sıkıştırılmış PDB'nin base64'ü)
ve mutlak `usb_root` gönderilir; `xml_base64` gönderilmez. Tam olarak bir
kaynak seçilmelidir. Apply ayrıca önizlemenin `expected_preview_hash` ve
sabit `idempotency_key` değerlerini ister. Kaynak kapsamı `rekordbox_device`
olarak saklanır; XML kaynağının aynı TrackID alanını ele geçirmez.

## GitHub gerçek örnekleri ve kabul

Format [Deep Symmetry DeviceSQL analizinden](https://djl-analysis.deepsymmetry.org/rekordbox-export-analysis/exports.html)
ve [Kaitai şemasından](https://github.com/Deep-Symmetry/crate-digger/blob/main/src/main/kaitai/rekordbox_pdb.ksy)
doğrulanır. İndirilen örnekler XML değil, gerçek ikili `export.pdb` dosyalarıdır:

- [usr-ein/prolink, 48e7c7f](https://github.com/usr-ein/prolink/blob/48e7c7fe6359a1e7c36d0b6099656090894794a5/testdata/export.pdb):
  **651 parça**, `test_formats` playlist'inde **40 öğe**. TrackID'ler
  **612–651** kaynak sırasında doğrulanır; Unicode `Obéissance` ve
  `人々の繋がり` başlıkları ayrıca kontrol edilir.
  SHA-256: `a7876a572998daa35896db77f937d9cc70727cd75af7aa1047555a28bc59c6df`.
- [Holzhaus/rekordcrate, 14d54ed](https://github.com/Holzhaus/rekordcrate/blob/14d54eded7b0d2f6fe67c7871533dca35a73e5fa/data/complete_export/demo_tracks/PIONEER/rekordbox/export.pdb):
  **2 koleksiyon parçası**, playlist yok.
  SHA-256: `27d5a84d7688244357c467680b7e62a36a0e6f3f7eb0ab4c37b26aa312e7f8aa`.
- [fragmede/rekordbox-pdb, ee3bac2](https://github.com/fragmede/rekordbox-pdb/blob/ee3bac2f22ca11a5ce61eea35f8cb951c246eaef/tests/data/one-song-export.pdb):
  **1 parça**, `aac` playlist'i, `Super Smash Bros.` başlığı.
  SHA-256: `3097f266c887ce78497d669143733efd45ae26fbb221b7a562e9edc4050e3251`.

Ana örnek Python okuyucusu ve gerçek C/GTK → HTTP → özel Store önizleme/kayıt
akışında sınanır. Bu, fiziksel CDJ veya USB donanım kabulü değildir. Örnekler
yaklaşık 1 MiB'lık `build/rekordbox-github/` klasöründe tutulur; ayrı derleme
ağacı oluşturulmaz. RadioS kökünden tekrar çalıştırma:

```sh
mkdir -p build/rekordbox-github/usb/PIONEER/rekordbox
curl --fail --location 'https://raw.githubusercontent.com/usr-ein/prolink/48e7c7fe6359a1e7c36d0b6099656090894794a5/testdata/export.pdb' \
  -o build/rekordbox-github/usb/PIONEER/rekordbox/export.pdb
export RADIOS_TEST_USB_EXPORT="$PWD/build/rekordbox-github/usb/PIONEER/rekordbox/export.pdb"
PYTHONDONTWRITEBYTECODE=1 python3 tests/test_desktop_rekordbox_device.py
python3 tests/run_rekordbox_ui.py build/test_rekordbox_ui
```

Normal CTest uzak indirme gerektirmez. Bağımsız ikili fixture ile sıra,
klasörler, tekrarlar, Unicode, silinmiş satırlar, son kısmi row grubu,
bozuk/cyclic veri, sıkıştırma sınırları, büyük HTTP gövdesi, CAS, kalıcılık,
salt okuma, USB çıkarılması ve kayıp yanıttan aynı işlemin tekrarı sınanır.
