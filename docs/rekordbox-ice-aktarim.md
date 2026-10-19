# Rekordbox XML içe aktarımı

Playlist ekranındaki **rekordbox USB / SD oku** düğmesi varsayılan olarak
[gerçek USB Device Library](rekordbox-usb-tracklist.md) okur. Eski XML
uyumluluğu için açılan pencerede **Eski XML · uyumluluk** seçilir.
Rekordbox'tan XML koleksiyonunu dışa aktar, dosyayı seç, listeleri ve
politika seçeneklerini ayarla. **Önizlemeyi doğrula** hedef listeleri, katalog
kararlarını, eklenecek/atlanacak öğeleri ve uyarıları salt okunur gösterir.
**Seçilenleri içeri aktar** yalnız incelenen kapsamı kaydeder.

Kaynak kütüphane kimliği aynı koleksiyonun sonraki XML'lerinde korunmalıdır.
Farklı kaynak koleksiyonları için farklı kimlik kullanılır. Native kimlikler
istasyon içinde ayrılır; başka istasyonun aynı TrackID'si hedefi ele geçirmez.

**Mevcut listeye ekle** aynı adlı aktif native playlist'in düzenlenebilir
taslağına ekler. Birden çok hedef aynı adı taşıyorsa operatör yeni kopyayı
seçer. **Yeni kopya oluştur** çakışan ada `(2)`, `(3)` gibi bir ek koyar.
Tekrar atlama, çözülmüş kaynak dosya yolu; yol yoksa kaynak kütüphane/TrackID
üzerinden yürür. Yolu aynı olan kayıtlar üyelik olarak atlanabilir; bu karar
dosyanın gerçekten var olduğu anlamına gelmez. Koleksiyon seçeneği, playlist
dışındaki parçalar dahil bütün koleksiyonu ayrıca bir native listeye alır.

Cue politikası DJ kaynak bilgisini koruma, radyo cue incelemesine önerme veya
cue aktarımını atlama şeklindedir. DJ cue hiçbir zaman otomatik radyo cue
onayına dönüşmez. Source snapshot tam kaynak alanlarını saklar; native
görüntüleme sınırında kısaltılan metadata uyarıyla gösterilir. Playlist
ayrıntısı kaynak TrackID'sini, cue durumunu ve XML SHA özetini gösterir.

Native dosya sınırı **1 MiB**, XML sınırı **500 playlist / 5.000 koleksiyon
parçası**, hedef liste sınırı **500 öğe**, bir işlemde eklenecek üyelik sınırı
**5.000**'dir. Bu sınırlar native HTTP ve dayanıklı kurtarma gövdesini 2 MiB
bütçesinde tutar. Mevcut profil tabanlı `/v1/imports/rekordbox/preview,apply`
yollarının kendi daha geniş XML sözleşmesi değişmedi.

`desktop-rekordbox/preview,apply`, mevcut importer'ın kaynak/alan/cue kararları
ve yazıcılarını kullanır. Katalog, import node/entry, native playlist ve
girdileri, import job ve immutable replay receipt aynı Store transaction'ında
yazılır. Eski preview hash'i güncel hedeflerle uyuşmuyorsa hiçbir satır
yazılmaz. Yanıt kaybında aynı XML kopyası, kapsam, politika, hash ve
idempotency kimliği korunur; seçenekler bu işlemin sonucu doğrulanana kadar
kilitlenir. Yeniden açılışta exact gövde dayanıklı kurtarma kuyruğunda kalır.

XML ses dosyalarını içermez. Katalog sürümleri taslak, istasyon uygunluğu
kapalı ve native satırlar doğrulanmamış metadata olarak oluşturulur. Gerçek
ses dosyası kütüphaneye ayrıca alınır ve playlist satırına seçilerek bağlanır;
version approval, hak ve yayın hazırlığı ayrı işlemlerdir.

XML kabulü: `desktop_rekordbox` özel Store/HTTP'de 10 senaryo,
`native_rekordbox` özel gerçek serviste 2 XML GTK senaryosu, recovery testinde exact XML/body reopen
ve değişmiş politika reddi. Testler üretim veritabanına kayıt eklemez.

```sh
ctest --test-dir build --output-on-failure -R '^(recovery|desktop_rekordbox|native_rekordbox|data_playlists)$'
```
