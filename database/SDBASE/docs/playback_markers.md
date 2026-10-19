# Onaylı ses işaretleri ve yürütme sözleşmesi

Şema v3 ve plan sözleşmesi v2, her planlanmış ses olayına değişmez bir `playback_marker_sets` kaydı bağlar. Set; içerik sürümü, fiziksel medya kimliği, medya SHA-256 değeri ve doğrulanmış süreyi birlikte taşır. `marker_set_id`, `_rev: 1`, içerik hash'i ve seçilen cue kimliği/revizyonları onay sırasında `plan_revisions.playback_bindings` içinde dondurulur. Paket bu veriyi her ses olayının `playback_markers` alanında taşır. Uç, merkez bağlantısı olmadan aynı bilgiyi `active_manifest()` ile okuyabilir.

## İşaret seçimi

`POST /v1/workflows/markers/create` için `approve` yetkisi gerekir. Gövde `station_id`, `version_id`, `cue_ids` ve isteğe bağlı `media_file_id` alır. `cue_ids`, bu istasyonda aynı sürüm ve aynı medya hash'i için onaylı cue kimliklerini belirtir. Aynı anlamdaki iki cue birlikte seçilemez. Boş `cue_ids` ile `cue_in_ms`, `cue_out_ms`, `segue_in_ms`, `segue_out_ms`, `fade_in_ms`, `fade_out_ms` değerleri açıkça onaylanabilir. Cue ile açık verilen koordinat çelişirse işlem reddedilir. Yeni bir karar yeni set kimliği oluşturur; mevcut set düzenlenemez veya silinemez.

```json
{
  "station_id": "ISTASYON_UUID",
  "version_id": "ICERIK_SURUM_UUID",
  "media_file_id": "MEDYA_UUID",
  "cue_ids": [],
  "cue_in_ms": 1000,
  "cue_out_ms": 3000,
  "fade_in_ms": 100,
  "fade_out_ms": 200
}
```

Taslak ses girdisi, `payload.playback_marker_set_id` ile seçilen seti belirtir. Setin ait olduğu sürüm, `payload.version_id` ile aynı olmalıdır. Kimlik belirtilmezse plan onayı seçilen doğrulanmış medyanın mevcut onaylı cue'larını kullanır. Bir cue türü için birden fazla onaylı aday varsa otomatik seçim reddedilir; açık set gerekir. Hiç cue yoksa girişin açık cue/fade değerleri kullanılır; başlangıç varsayılanı sıfırdır. Açık bitiş yoksa sabit süre için `cue_in + planned_duration_ms`, diğer süre türlerinde medya sonu kullanılır. Her kesit doğrulanmış medya içinde kalmalı ve sabit plan süresi kaynak cue kesitiyle eşleşmelidir. Böylece onay sırasında alınan karar dağıtım anında yeniden seçilmez.

`POST /v1/workflows/markers/list` (`station_id`, `version_id`) ve `markers/get` (`station_id`, `marker_set_id`) `view` yetkisiyle setin `_id`, `_rev` ve bütün alanlarını döndürür. Planın ayrıntı/revizyon okuyucusu da onaylı `playback_bindings` verisini içerir.

## Koordinat anlamları

| Alan | Anlam |
|---|---|
| `cue_in_ms`, `cue_out_ms` | Medyanın başından itibaren mutlak milisaniye; seçilen kesit `[cue_in, cue_out)` aralığıdır. `audio_start` ve `audio_end` cue'ları bu alanlara dönüşür. |
| `segue_in_ms`, `segue_out_ms` | Seçilen kesit içindeki kaynak geçiş referansları; `segue_in`/`segue_out` cue'ları korunur. Bu alanlar planın başlangıç saatini veya overlap kararını kendiliğinden değiştirmez. |
| `fade_in_ms`, `fade_out_ms` | Seçilen kesitin başı/sonundaki zarf süreleri. `fade_start` cue'sundan `fade_out_ms = cue_out_ms - fade_start.position_ms` türetilir. Zarflar kesit içinde üst üste gelemez. |
| `cues` | Seçilen her kaynağın cue kimliği, onay anındaki revizyonu, türü ve mutlak milisaniye konumu. Intro/vocal/hook/outro kaynak bilgisi de korunur. |
| `media_sha256`, `content_sha256` | Fiziksel ses baytlarının ve işaret setinin anlam alanlarının ayrı hash'leri. |

Cue geri çekildiğinde tarihsel onay bilgisi korunur. Yeni cue onayı veya yeni set, eski planın paketini değiştirmez. Medya seçimi de plan onayıyla sabittir; sonraki bir medya temsilinin katalogda öne geçmesi eski paket için yeniden seçim yapmaz. Merkez, paketi onaylı setle birebir karşılaştırır. Uç, işaret hash'i, set revizyonu ve medya manifestinin SHA bağı ile paketi doğrular; aynı plan revizyonu farklı bir paketle yeniden kurulamaz.

## Yürütme ve uyumluluk sınırı

`audio_playout.render_active_plan_wav`, kabul edilmiş v2 uç planından sabit, ardışık PCM16 WAV kesitlerini üretir. Kaynak ofsetleri en yakın PCM karesine yarım yukarı yuvarlanır. İncelenmiş fade süreleri doğrusal PCM kazancına uygulanır; fade-in'in ilk, fade-out'un son karesi sıfırdır. Sonuç set kimliğini/revizyonunu/hash'ini ve kaynak işaretlerini de raporlar. Cue sınırlarından farklı segue referansı, overlap, prodüksiyon overlay, ReplayGain/gain/ducking veya desteklenmeyen yürütme davranışı istenirse bu sınırlı üretici dosya yayımlamadan durur. Daha geniş geçiş yürütmesi ayrı oynatıcı sorumluluğudur.

LibreTime adaptörü, v2 paketindeki cue ve fade sürelerini gerçek LibreTime `cue_in`, `cue_out`, `fade_in`, `fade_out` alanlarına çevirir. İşaret provenansı ayrıca adaptör yanıtında bulunur. LibreTime fade işlemesini kendi motoru yapar; SDBASE'in PCM üreticisiyle aynı cihaz/DSP sonucu iddia edilmez. Cue sınırından farklı segue referansı, plan overlap/overlay'i ve desteklenmeyen DSP adaptörde reddedilir.

v1 plan paketleri tarihsel veri olarak saklanır. `validate_snapshot_contract(snapshot, allow_legacy=True)` yalnız açık bir tarihsel okuma yoludur. Normal dağıtım, uç kurulumu veya oynatma v1 paketi reddeder. Eski aktif v1 paketle açılan uç çalışma iznini kapatır; operatör güncel çıkış ve işaret seçimiyle yeni plan revizyonu oluşturup onaylamalıdır. Eski pakete sonradan güncel işaret eklenmez.

## Doğrulama kanıtı

2026-10-01 tarihinde geçici gerçek `.sdb` ve PCM WAV dosyalarıyla 19 odaklı kontrol geçti: denetimdeki 100/1900/1800 ms işaret örneği, fade türetimi, değişmez paket, dağıtım, SHA/cue/revizyon bozma reddi, çevrimdışı okuma ve uç yeniden açma, sonraki cue değişikliğinin eski onayı etkilememesi, açık set seçimi, PCM kesit/fade örnekleri ve şema v3 yeniden açma. Ek kontrollerde aynı türde birden fazla onaylı cue otomatik onayı günlük yazmadan reddetti; yeniden hesaplanmış hash ile yanlış kaynak cue revizyonu ve set kimliği de kabul edilmedi. Tarihsel aktif v1 uç günlüğü yeniden açıldığında kayıtları koruyarak çalışma iznini kapattı. Gerçek yerel LibreTime 4.5.0 `get_schedule` okuyucusu da ayrı süreçte 1/3 saniye cue ve 100/200 ms fade alanlarıyla bir dosya olayı üretti. Kaynak okuyucu değiştirilmedi; saat kontrollüydü, yalnız yerel kaynak paketinin sürüm metadatası import için sağlandı. Bunlar fiziksel ses cihazında yayın veya dinleyiciye teslim kanıtı değildir. Geçici veritabanları, WAV çıktıları ve bağımlılık ortamı temizlendi.
