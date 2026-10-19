# RadioBOSS düz M3U8 aktarımı

Bu adaptör, [yerel RadioBOSS kılavuzunun](<../../../kullanım klıvazu/RadioBOSS_UserManual_en.pdf#page=82>) 82–83. PDF sayfalarında tarif edilen `.m3u8` çıktı dosyasını ve 159–160. sayfalardaki `#EXTM3U` / `#EXTINF` örneğini esas alır. Üreticinin [Playlist Generator CLI kılavuzu](https://manual.djsoft.net/radioboss/en/plgen_command_line_interface.htm), [reklam zamanlayıcısı biçim seçimi](https://manual.djsoft.net/radioboss/en/ads_general.htm) ve [M3U8 örneği](https://manual.djsoft.net/radioboss/en/automatic_weather_announcement.htm) aynı sınıra ilişkin resmî çevrimiçi kaynaklardır. Bu belgeler RadioBOSS'un tüm playlist türlerinin ayrıntılı ikili/özel semantiğini açıklamaz; adaptör yalnız bu **düz dosya yolları** alt kümesini destekler.

## Güvenli önizleme

Yerel CLI'de `sdbase radioboss-preview --playlist playlist.m3u8` aynı salt okunur raporu verir. Veri dosyası gerekmez.

```python
from sdbase.radioboss_m3u8 import preview_radioboss_m3u8

report = preview_radioboss_m3u8("playlist.m3u8")
print(report["source_sha256"], report["entries"], report["issues"])
```

Okuyucu UTF-8 (isteğe bağlı BOM), ilk `#EXTM3U` satırı, `#EXTINF:saniye,görünen başlık` ve açık POSIX/Windows/UNC yerel dosya yollarını kabul eder. M3U8 içindeki yol **kaynak kimliği değildir**: önizleme hiçbir ses dosyasını açmaz veya `.sdb` yazmaz. Göreli yol, URL, RadioBOSS değişkeni, komut, konteyner ve bilinmeyen direktif uygulamayı durdurur; satır/sorun raporda kalır. `#EXTINF` süresi ve başlığı yalnız kaynak iddiasıdır. Okuma sınırı 2 MiB, 10.000 parça ve satır başına 8192 karakterdir.

## İncelenmiş dosyadan taslak plan

CLI için kaynak yollarını anahtar, her yolun `version_id`/`media_file_id` çiftini değer yapan bir JSON dosyası hazırlayın. `sdbase --db /veri/RadioS.sdb radioboss-apply --playlist playlist.m3u8 --token TOKEN --station-id UUID --namespace studio-a --expected-sha256 ONIZLEME_SHA --idempotency-key sabit-anahtar --service-date 2026-10-01 --start-at 2026-10-01T08:00:00+03:00 --bindings-json bindings.json --service-key default --territory TR` taslak oluşturur. JSON dosyasında mükerrer anahtar reddedilir.

```python
from sdbase.radioboss_m3u8_import import apply_radioboss_m3u8

draft = apply_radioboss_m3u8(
    store, principal, station_id, "playlist.m3u8",
    source_namespace="studio-a",
    expected_source_sha256=report["source_sha256"],
    idempotency_key="2026-10-01-morning",
    service_date="2026-10-01",
    start_at="2026-10-01T08:00:00+03:00",
    territory="TR",  # Kaynak playlist bölge bilgisi taşımadığı için açık seçim.
    path_bindings={
        "C:\\Radio\\song.wav": {
            "version_id": "<onaylı-sürüm-UUID>",
            "media_file_id": "<doğrulanmış-medya-UUID>",
        },
    },
)
```

Her farklı yol için açık sürüm/medya eşlemesi zorunludur. Bu eşleme, kaynağın gerçekten o sesi içerdiğine ilişkin **operatör incelemesidir**; adaptör dış ses dosyasının baytlarını okumaz. Seçilen sürüm onaylı, medya doğrulanmış ve SDBASE nesne deposunda mevcut olmalıdır. Reklam girdileri ayrıca kampanya/trafik denetimi gerektirdiği için bu genel içe almaya alınmaz. Uygulama, SDBASE'deki teknik inceleme kaydının süreleriyle ardışık saatler kurar ve tüm girdiler, taslak plan, kaynak satırları, kayıp ve hazır oluş raporunu tek `.sdb` işleminde yazar. Plan onaylanmaz veya uçta etkinleştirilmez. Belirli servis için `service_key` belirtilebilir; girdiler servisin çıkışına bağlanır. Bölgesel haklar için `territory` operatör tarafından açıkça verilir; playlist içeriğinden tahmin edilmez.

Aynı anahtar, aynı dosya hash'i, başlangıç ve eşlemeyle tekrarlandığında önceki sonuç döner. Kaynak veya eşleme değişirse çakışma döner. Uygulama sınırı 1 MiB/1000 satırdır. `#EXTINF` ile teknik kayıt süresinin farkı, başlık farkı, eksik `#EXTINF` ve henüz sağlanmamış yayın hazırlığı raporlanır; tahmin edilen veya eksik süreyle plan oluşturulmaz. SDBASE medya onayı baytların hash'ini doğrular; teknik sürenin ses çözücüsüyle bağımsız ölçümü bu adaptörde yapılmaz.

## Seçilmiş onaylı planı dışa aktarma

CLI'de `sdbase --db /veri/RadioS.sdb radioboss-export --token TOKEN --station-id UUID --plan-id UUID --revision-id UUID --paths-json media_paths.json` kullanılır. `media_paths.json`, `media_file_id` → hedef mutlak dosya yolu eşlemesidir. Komut M3U8 metnini ve kayıp raporunu JSON içinde döndürür; ses dosyalarını yazmaz.

```python
from sdbase.radioboss_m3u8_export import export_radioboss_m3u8

out = export_radioboss_m3u8(
    store, principal, station_id, plan_id, approved_revision_id,
    {media_file_id: "C:\\Radio\\song.wav"},
)
print(out["complete"], out["skipped"], out["losses"])
playlist_text = out["m3u8_text"]
```

Yalnız istasyonun geçerli, onaylı plan revizyonu seçilir. Dışa aktarım her ses girdisini yeniden hazır oluş denetiminden geçirir, hedef mutlak dosya yolunu açıkça ister ve UTF-8 `#EXTM3U/#EXTINF` metnini döndürür. Atlama varsa `complete=false`; kısmi liste tam yayın planı gibi gösterilmez. Ses baytları kopyalanmaz ve hedef RadioBOSS makinesindeki yol doğrulanmaz. Başka olay türleri, yayın saatleri, geçişler, haklar, kampanya borcu, sürüm/medya hash'i, onay ve gerçekleşme geçmişi M3U8 içinde temsil edilmez; rapor bunları açıkça sayar. Üreticide çalma doğrulaması ve gerekli ses kopyalama ayrı kabul işidir.
