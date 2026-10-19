# SDBASE üretim kabul yordamı

`tools/acceptance.py`, kendisine ait geçici `.sdb` ve medya dizinlerinde gerçek HTTP merkezini, uç ajanını ve sınırlı örnek oynatıcıyı **aynı makinede ayrı süreçler** olarak çalıştırır. Üretim veritabanı yolu almaz; kaynak kod klasörüne fixture/veri/ses bırakmaz. JSON raporu istenirse belirtilen yola yazılır. Örnek oynatıcının kaynak kodu ayrı `tools/acceptance_playout.py` dosyasındadır.

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tools/acceptance.py \
  --offline-seconds 60 --report /tmp/sdbase-production-acceptance.json
```

`--offline-seconds`, merkez süreci kapalıyken **gerçek geçen süreyi** belirler. Kabul yordamı ayrıca kontrollü saatle iki yerel servis gününü ve sonraki gece grant süresinin dolmasını ilerletir. Kontrollü tarih ilerletme, günlerce kesintisiz gerçek zaman çalışmanın kanıtı değildir. Uzun süreli tekrar için aynı komut `--offline-seconds 86400` ile çalıştırılabilir; rapor iki süreyi ayrı tutar. Varsayılan gerçek offline süre 2 saniyedir.

## Kapsam

| Zincir / senaryo | Yordamın uyguladığı kontrol |
|---|---|
| HTTP plan → medya → uç | Ses HTTP üzerinden yüklenir, yönetilen SHA nesnesi teknik/editoryal onaylanır; ayrı uç süreci `HTTPTransport` ile paketi/medyayı indirir ve kabul eder. |
| Cue / segue / out / fade | Onaylı radyo cue kayıtları sözleşme 2'de sürüm/medya/SHA ve marker set kimliğiyle görünür. Örnek oynatıcı kaynak kesitini ve doğrusal fade zarflarını gerçek PCM örneklerine uygular. |
| Geçiş | İki sabit parçanın açıkça planlanmış 250 ms örtüşmesi PCM dosyasında karıştırılır. Ortadaki örnekler ve toplam frame sayısı ölçülür. Segue koordinatları kaynağa aittir; kendi başına plan saatini değiştirmez. |
| Makro | Yalnız `acceptance.set_flag` isimli süreç içi makro Boolean bir bayrağı değiştirir. Kabuk komutu veya genel ürün makro dili çalıştırılmaz. |
| Stream | Açık izin listesindeki yerel HTTP adresinden gerçek, sonlu PCM WAV cevabı okunur ve planlanmış bir saniyesi karışıma girer. Genel canlı stream decoder desteği değildir. |
| Event log | Katalog seslerinin start/complete olayları dayanıklı uç outbox'ına yazılır ve HTTP üzerinden merkez `playout_events` kayıtlarına gönderilir. Makro/stream yürütme makbuzları ayrı fsync edilmiş yerel JSON artifactinde tutulur; bu API katalog dışı olaylar için sahte version ID üretmez. |
| Çıkış yasağı | `default` ve `output_id: "*"` ile kısmen kesişen `main` yasağı onayı engeller; başarısız işlem sequence artırmaz. İki aktif çıkışta belirsiz hedef reddedilir; açık `secondary` hedefi benzersiz çözümlenir. |
| Ayrıntı/revision API | Item, version, plan entries/revisions, istasyon–item ilişkisi gerçek HTTP ile okunur; eski ilişki `_rev` değeriyle ikinci yazma 409 alır. |
| Komut sırasında süreç ölümü | Uç süreci komutu kabul edip completion vermeden `os._exit` ile kapanır. Açılışta komut `outcome_unknown` olur ve yeniden yürütülmez. |
| Saat sapması | Yeni offline grant alınırken uç saati merkezden 10 dakika ileri alınır; grant kabulü reddedilir. Saat düzeltildikten sonra normal prefetch geçer. |
| Merkez kapalı / ertesi gün | Önceden indirilmiş, doğrulanmış ve yetkilendirilmiş plan grant ile yerel olarak devralınır. Merkez süreci gerçekten kapalıdır; kontrol HTTP bağlantısının başarısızlığını görür. |
| Eksik medya / restart | Gerekli yerel nesne fiziksel olarak saklanır; offline devralma engellenir. Baytlar geri konunca devralma geçer. Her uç işlemi yeni süreçte journal'ı tekrar açar. |
| Offline çıktı / outbox | Ertesi günün kabul edilmiş ses planı PCM dosyasına yürütülür; iki olay outbox'ta restart boyunca kalır ve ağ geri gelince gönderilir. |
| Grant expiry | Ayrı bir uç journal kopyasında sonraki yerel geceye ilerlenir; grant biter ve üçüncü gün kendiliğinden yetkilendirilemez. |
| Ağın dönmesi / çakışan plan | Yerel grant merkez kabulüne çevrilir. Bir sonraki kesintide merkez yeni plan revizyonu onaylar; reconnect yeni revizyonu daha yüksek engine epoch ile alır. Başka motorun sahipliği eski ucu fence eder. |
| Backup / restore | Yedek ve restore ayrı süreçlerde yapılır. Provision edilen restore yeni store kimliği ve yönetici credential'i alır; önceki token'lar iptal, sahiplik inactive olur. |
| Yalnız yedekten devam | Kaynak `.sdb`, medya ve eski uç dizini silindikten sonra restore merkezinde yeni uç credential'i, fork/onay/dağıtım yapılır. Yeni uç yedekteki medyadan çıktıyı üretir; PCM SHA öncekiyle aynıdır. |

## Ölçüm ve sınırlar

2026-10-01 tarihindeki çalıştırmada **37 kabul kontrolü**, **84 yönetici HTTP çağrısı**, backup/restore dahil **84 kısa ömürlü worker süreci** geçti. **Merkez gerçekten 60 saniye kapalı kaldı**; çalıştırmanın toplam süresi 65,784 saniyeydi. Uçların `HTTPTransport` istekleri 84 çağrı sayısına dahil değildir; 84 worker'ın 82'si uç, ikisi backup/restore sürecidir. Merkez süreçlerinin yeniden açılması ayrıca gerçekleşir. Örnek çıktı **22.000 frame**, 8 kHz mono PCM16 ve 2.750 ms'dir. Kaynak cue kesiti 500–1.500 ms, parçalar arası örtüşme 250 ms, stream kesiti 1.000 ms'dir. Çıktı SHA-256: `d3e45409393828715cd8981e0d1995eca0b2f598371080ec37a0beb149ca5e0e`.

Referans WAV üretimi ses kartı, fiziksel verici veya dinleyiciye ulaşma kanıtı değildir. Motor olayları `sink: "reference_pcm_file"` ve `on_air_proven: false` taşır. İkinci **fiziksel** makine veya ayrı işletim sistemi kurulumu bu ortamda sınanmadı; restore kabulü aynı host'ta ayrı süreç ve temiz dizinlerle yapılır. Harici stream kaynağı yedek paketinin parçası değildir.

Örnek oynatıcı sabit mutlak takvim, aynı formatta PCM16, açık sequential/overlap geçişleri ve onaylı trim/linear fade davranışını destekler. Canlı cihaz girişi, genel codec/stream protokolleri, cut/adaptive timing, ReplayGain/gain/ducking ve production overlay bu kapsamda doğrulanmış değildir. PCM kaynak 16 MiB, stream cevabı 8 MiB, çıktı sekiz milyon örnek ve varsayılan 60 saniye ile sınırlıdır. Üretim ses motorunun kendi decoder, tampon, gecikme, drop-out, cihaz ve bağımsız output observation kabulü ayrıca ölçülmelidir.
