# 8. aşama — Ses motoru ve cihazlar

RadioS ses yolu C ile çalışır. FFmpeg kütüphaneleri doğrulanmış yerel dosyayı artımlı çözer; libpulse, PulseAudio veya PipeWire'ın PulseAudio uyumluluk servisine gerçek çıkış ve giriş akışlarını açar. Sabit motor biçimi 48.000 Hz, iki kanal, float PCM'dir. Ana, CUE ve monitor çıkışları ayrı akışlardır. HTML/JavaScript çalıştırılmaz; mevcut CSS aktarımı ve native GTK ekranları kullanılır.

## Kullanım

1. **Teknik yönetim → Aygıtlar ve görünüm** alanında ana çıkışı seç. Ön dinleme için CUE, istenirse monitor çıkışını ayrıca ata. Liste gerçek ses servisinden gelir; aygıtları yenileme düğmesi listeyi tekrar okur.
2. Ses cihazlarını uygula; kalıcı tercihleri kaydet. Cihaz adları ve seviyeleri istasyonun `audio_preferences` / `Ses aygıtları` belgesinde CAS ve işlem alındısıyla saklanır. Kaydetme `manage` yetkisi ister. Kayıp cihazın adı korunur ve bağlantı hatası gösterilir.
3. **Yayın → Medya kütüphanesi → + Ses dosyaları** ile medya içe aktar. Seçili kaydı hedef oynatıcıya **Oynatıcıya yükle** ile gönder veya **Canlı oynatıcılar → Kütüphaneden yükle** kullan.
4. Oynatıcıda **Oynat / Duraklat / Durdur**, konum sürgüsü, dB seviyesi ve **Ana çıkışa gönder** kontrollerini kullan. İlk yükleme kendiliğinden başlamaz. En fazla 32 normal oynatıcı bulunur.
5. Kütüphane, oynatıcı veya **Dalga biçimi / Cue** editöründen ön dinleme başlat. Ayrı 33. oynatıcı CUE'ya ayrılmıştır; ana çıkışa yönlendirilemez. Ön dinlemeyi açan pencere kapandığında yalnız onun ön dinlemesi durur.
6. Mikrofon kaynağını ve kazancını seç; **Mikrofon kapalı** düğmesiyle girişin ana karışıma gönderilmesini açıkça başlat. Seviye göstergesi gerçek yakalanan PCM'den gelir. Yeniden açılışta mikrofon ve CUE/monitor kapıları kapalıdır; oynatıcılar otomatik başlamaz.

## C kaynakları ve sözleşmeler

- [audio_decoder.h](../include/radios/audio_decoder.h) / [decoder.c](../src/audio/decoder.c): SHA doğrulamalı dosya açma, sabit biçime çözümleme, iptal edilebilir okuma ve tam örnek konumuna arama.
- [audio.h](../include/radios/audio.h) / [engine.c](../src/audio/engine.c): çoklu oynatıcı, sınırlı PCM kuyrukları, karışım, transport, yönlendirme, gain ve durum görüntüsü.
- [pulse_backend.c](../src/audio/pulse_backend.c): gerçek cihaz listesi, üç çıkış akışı, giriş yakalama ve ses servisinin tüketim zamanını okuma.
- [audio_ui.c](../src/ui/audio_ui.c): GTK kontrol köprüsü, medya aidiyeti/yol kontrolleri, pencere sahipliği, ön dinleme ve kalıcı cihaz tercihleri.
- [broadcast.c](../src/ui/broadcast.c), [media.c](../src/ui/media.c), [editors.c](../src/ui/editors.c): normal oynatıcılar, doğrulanmış kütüphane yükleme ve waveform ön dinleme.

Normal oynatma yalnız mevcut istasyonun teknik olarak doğrulanmış ve yerel objesi bulunan medya sürümünü kabul eder. Dosya, `RadioS.sdb.objects/` altında olmalı; çözücü beklenen SHA-256'yı gerçek baytlardan doğrular. Eski playlist metadata'sındaki serbest dosya yolu bu motorun giriş sözleşmesi değildir. Kaynak URL, symlink, FIFO, değişen dosya, bozuk veri veya sınırsız PCM tamponu kabul edilmez.

Dosya okuma ve FFmpeg çözümleme worker'da yürür. Ses callback'i önceden ayrılmış PCM halkasından okur veya giriş halkasına yazar. GTK durumları motorun gerçek verisinden yenilenir; transport ilerlemesi ses servisinin tüketimine bağlanır. Arama aynı doğrulanmış dosyayı baştan çözüp hedef örneğe kadar atlayarak doğru codec başlangıcını ve resampler fazını korur; maliyeti hedef konuma bağlıdır ve iptal edilebilir.

WAV/PCM, MP3, FLAC, Ogg/Vorbis, M4A/AAC ve ham AAC kabul matrisidir. Taslak cue giriş/çıkışları yerel oynatma sınırı olarak kullanılabilir. Fade, overlap, efekt ve otomatik geçiş yürütmesi aşama 9/10'dadır. Yerel PCM oynatılması istasyon yayın onayı, internet yayını veya on-air alındısı üretmez.

## Derleme ve kabul

Önceki bağımlılıklara `libpulse>=15.0` eklenmiştir. Çalışma oturumunda erişilebilir PulseAudio/PipeWire Pulse servisi gerekir. C uygulamasının sesi harici `ffmpeg`, `paplay` veya tarayıcı süreci üzerinden çalınmaz.

`audio_decoder` gerçek PCM karşılaştırmasıyla format, resampling, arama, iptal, hash ve kaynak sınırlarını; `audio` motorun gerçek karışım ve transport sonuçlarını sınar. GUI kabulü açıkken `audio_devices` ve `native_audio`, yalnız kendilerinin oluşturduğu ayrı sanal çıkışların PCM'sini yakalar; fiziksel varsayılan aygıtlar ve üretim veritabanı test verisi için kullanılmaz. Bu testlerde `pactl` ve `parec` araçları gerekir.

```sh
cmake -S RadioS -B RadioS/build -DRADIOS_GUI_TESTS=ON
cmake --build RadioS/build --parallel
ctest --test-dir RadioS/build --output-on-failure
```

Son kabul: strict C11 ve birleşik CTest **19/19**, 134,77 saniye başarılı. Çözücü **11/11**; gerçek PCM motoru **6/6** ve gerçek Pulse **1/1**; native ses **6/6**; typed backend API **13/13** başarılıdır. Çözücü/motor/Pulse ASan/UBSan ve leak detection açıkken temizdir. Native sesin altı grubu ASan/UBSan altında 41,66 saniyede geçti; GTK süreç global verileri için leak detection kapalıdır.

Native kabul, gerçek kütüphane/kart/editör düğmeleriyle yakalanan PCM'yi ve CUE ayırmasını; sarma sonrası çıkışın yeniden hazır olmasını; eski kartın yeni ön dinlemeyi durduramamasını ve geciken yanıtın CUE kapatmayı geri alamamasını da kapsar. Test kapıları yalnız özel backend kopyasına eklenir. Gerçek cihaz listesi okundu; mevcut servis ve 0.8.0 uygulaması çalışma DB'sinin kimlikleri/sequence 2/bütünlüğü korunarak açıldı. Fiziksel donanımda dinleme ve uzun süreli yayın kabulü 14. aşamanın işidir.

Ses sunucusu bütünüyle kapanırsa bağlantıyı otomatik yeniden kurma bu aşamada uygulanmadı: servisi kontrol edip uygulamayı yeniden aç. Cihaz listesi yenileme ve kayıp adlandırılmış cihazı açıkça yeniden uygulama mevcut cihaz akışında çalışır. Transport ve ölçümler sunucunun gerçek tüketim bildirimlerinde güncellenir; aradaki zaman GUI saatiyle ileri taşınmaz.
