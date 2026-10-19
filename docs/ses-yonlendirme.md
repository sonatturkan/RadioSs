# Ses yönlendirme — 0.18.0

## Uygulama sesini MAIN'e aktarma

**Çevrimiçi müzik → RadioS yayınına aktar** bu sekmeyi açar. Resmî oynatıcıda
parçayı çal, **Yenile** ile uygulamanın gerçek ses akışını seç ve **MAIN'e aktar**
düğmesine bas. Bu işlem WAV dosyası istemez ve bir deck slotu kullanmaz;
yerel deck'ler kapalıyken de çalışır. Uygulama gain'i canlı değiştirilebilir.

Seçilen uygulama RadioS'un sahip olduğu özel sessiz çıkışa taşınır. Yalnız
bu uygulamanın monitor akışı MAIN'e karıştırılır; ses MAIN çıkışında bir kez
dinlenir. MAIN dinleme aygıtını Teknik ses ayarlarından seç. MASTER EQ/FX/gain
ve limiter bu girişe de uygulanır. Mevcut MAIN WAV kaydı ve MP3/Icecast yayın
çıkışı aynı son miksi alır; kayıt ve yayın kontrolleri ayrıca başlatılır.

MAIN aktarımı mikrofonu içermez. **MAIN aktarımını durdur** veya normal
uygulama kapanışı yakalamayı keser; kimlik/çıkış sahipliği korunmuşsa uygulama
eski çıkışına döner ve yalnız RadioS'un oluşturduğu geçici çıkış kaldırılır.
Kaynak kaybı, kullanıcı tarafından farklı çıkışa taşıma, kimlik değişimi,
kuyruk taşması ve sunucu hatası aktarımı durdurur. Otomatik yeniden yakalama
yoktur. RadioS'un kendi çıkışı kaynak olarak seçilemez; karma sistem monitor'ına
geri dönüş yapılmaz.

Servis girişi ve playlist kullanımının ayrıntıları
[çevrimiçi müzik belgesindedir](cevrimici-muzik.md).

## Bağımsız WAV kayıt akışı

Gönderilen akış görseli, RadioS'un sekizinci sekmesi **Ses yönlendirme** olarak native C/GTK arayüzüne eklendi. Turkuaz kartlar, koyu zemin, kesikli bağlantılar ve iki stereo Peak/RMS göstergesi aynı akışı temsil eder:

```text
Uygulama → Çıkış aygıtı → Peak/RMS ─┐
                                   ├→ Kayıtçı
Giriş aygıtı → Kompresör → Peak/RMS ─┘
```

RadioS kökünden bu sekmeyi doğrudan açmak için:

```sh
./run-radios.sh --profile routing
```

## Kullanım

1. Kaydetmek istediğin uygulamada ses çalmayı başlat. **Yenile** ile çalışan ses akışlarını ve gerçek aygıtları listele; uygulamayı ve dinleme çıkışını seç.
2. Mikrofon da kaydedilecekse giriş aygıtını seçip **Mikrofonu kayda ekle** kutusunu aç. Mikrofon başlangıçta kapalıdır. Uygulama kutusu da bağımsız kapatılabilir; en az bir kaynak seçilmelidir.
3. Kompresörde **Kapalı**, **Konuşma** veya **Müzik** seç. Kompresör mikrofonun gerçek PCM verisini işler. Her kaynağın kayıt seviyesi −60…+12 dB arasında ayarlanabilir.
4. Kayıt dosyasının tam yolunu veya yalnız `.wav` dosya adını gir; **Dosya seç…** de kullanılabilir. Yalnız dosya adı girildiğinde yapılandırılmış kayıt klasörü kullanılır; varsayılan `data/recordings/` dizinidir. Mevcut dosyanın üzerine yazılmaz.
5. **Kaydı başlat** iki kaynağı tek stereo WAV dosyasında birleştirir. Göstergeler gerçek Peak/RMS ölçümüdür; süre ve boyut yazılan karelerden hesaplanır.
6. **Duraklat** dosyaya yazmayı durdurur; ölçüm devam eder. **Devam et** aynı dosyayı sürdürür. **Böl** mevcut WAV'ı sonlandırıp `-002.wav`, `-003.wav` sırasıyla yeni parça açar; duraklatılmış kayıtta yeni parça da duraklatılmış kalır.
7. **Durdur** WAV başlığını tamamlar ve ses kaynaklarını serbest bırakır. Kaynak/aygıt/kompresör/dosya seçimleri kayıt sırasında değiştirilmez.

Seçilen uygulama belirtilen dinleme çıkışına taşınır; yalnız o uygulamanın akışı yakalanır. Aynı çıkıştaki diğer uygulamalar kayda alınmaz. Kayıt bitince eski çıkış, akış hâlâ aynı uygulamaya aitse ve başka araç yönlendirmeyi değiştirmediyse geri yüklenir. Mikrofon bu sekmede kayıt dosyasına karışır; hoparlöre gönderilmez. Bu kayıt RadioS'un mevcut MAIN yayın kaydından bağımsızdır.

## Kalıcı ayarlar ve ses motoru

**Akış ayarlarını kaydet**, istasyonun gerçek DB'sine `audio_routing` türünde yönetilen belge yazar. Uygulama adı/binary, giriş ve çıkış adları, kaynak seçimleri, iki gain ve kompresör modu saklanır. Sözleşme `schema_version=1` ile toplam on alandan oluşur. Geçici Pulse indeksleri, dosya yolu, canlı sayaçlar veya anahtarlar kaydedilmez. Kaydetmek `manage`, kayıt başlatmak istasyonun `operate` yetkisini gerektirir; her başlatmada güncel kullanıcı/yetki yeniden doğrulanır. Kaydedilmiş seçimler açılışta yüklenir; ses yakalama ve kayıt açık operatör komutuyla başlar.

Aynı ada/binary'ye sahip birden fazla akış varsa kullanıcı yeniden seçim yapar. Bulunamayan kayıtlı aygıt tercihi korunur ve belirtilir. Ses sunucusu veya seçilen kaynak kaybolursa kayıt hata durumuna geçer ve mevcut WAV sonlandırılır. **Yenile**, sunucu döndükten sonra bağlantıyı yeniden kurar; kayıt kendiliğinden başlamaz.

Çıktı 48 kHz, stereo, PCM16 WAV'dır. Dosyalar kullanıcıya özel `0600` izinle, exclusive create ile açılır. Her kaynak için sınırlı beş saniyelik tampon vardır; yazıcı yetişmezse görünür hata oluşur. RIFF boyut sınırında geçerli son dosya korunarak kayıt durur. C motoru `libpulse` kullanır; tarayıcı veya HTML/JavaScript çalışma katmanı yoktur.

Kaynaklar: `include/radios/audio_routing.h`, `src/audio/routing.c`, `src/ui/routing.c`, `assets/themes/radios.css`; DB doğrulaması `database/SDBASE/src/sdbase/desktop_api.py` içindedir. İlk HTML tasarımının yedi profil envanteri tarihsel tasarım kapsamıdır; yeni sekmeyle çalışan uygulama sekiz profile sahiptir.

Yönlendirme bağlamı PipeWire'ın `media.category=Manager` kontrol rolünü bildirir;
bu rol uygulama çıkışını taşımak için gereklidir. Oynatma motorunun normal ses
bağlamı bu rolü almaz. Kaynak kimliği, seçilen akış ve çıkış sahipliği denetimleri
devam eder. [PipeWire anahtar belgesi](https://pipewire.pages.freedesktop.org/pipewire/group__pw__keys.html).

## 0.18.0 ana sistem kontrolü

Bu makinenin PulseAudio-on-PipeWire 1.6.8 sunucusunda yalnız testin oluşturduğu
sessiz sanal çıkışlar ve 440 Hz kaynağı kullanıldı. Gerçek MAIN WAV'ında
72.000 kare, yaklaşık 0,199997 genlik ve sıfır iç sessizlik boşluğu ölçüldü.
Kendi test uygulaması eski çıkışına döndü, geçici modüller kaldırıldı ve
varsayılan giriş/çıkış aygıtları korundu. Kullanıcı uygulamaları ve fiziksel
mikrofon yakalanmadı. Kanıt:
`build/online-018-host-routing-manager/acceptance.json`.

## Doğrulama

Strict C11 derlemesi ve GUI testleri açık birleşik CTest **42/42**, **270,13 saniye** geçti; atlanan test yoktur. Yeni `audio_routing`, `desktop_routing`, `native_routing` hedefleri gerçek kayıt motorunu, HTTP/DB kalıcılığını ve GTK düğmelerini sınar:

```sh
ctest --test-dir build --output-on-failure -R '^(audio_routing|desktop_routing|native_routing)$'
```

Bağımsız PCM analizi seçilen uygulama ve mikrofon tonlarını doğruladı; aynı çıkıştaki üçüncü uygulamanın tonu kayda karışmadı. Gerçek kompresör, duraklatmada sıfır yeni kare, aktif/duraklatılmış bölme, dosya koruma, kaynak kaybı, uygulama çıkışını koşullu geri yükleme ve thread/FD temizliği geçti. Native GTK kabulü **3/3**: kaydetme/yeniden açma ve bütün kayıt düğmeleri, izleyici kullanıcının reddi, bekleyen yetki yanıtı sırasında kapanış.

ASan/UBSan altında motor leak denetimi açıkken, native GTK üç grubu GTK global verileri nedeniyle leak denetimi kapalıyken temiz geçti. Sekiz profil pencere smoke kabulü başarılıdır. Ana sistemde **PipeWire 1.6.8** uyumu, yalnız testin kendi sıfır PCM uygulamasıyla doğrulandı; yakalama bağlantıları gerçek PipeWire grafiğinde yalnız bu uygulamaya bağlıydı. Fiziksel mikrofon yakalanmadı, sistem varsayılan aygıtları değişmedi.

Yerel kanıtlar: `build/routing-full.xml`, `build/routing-final-native/acceptance.json`, `build/routing-final-asan/acceptance.json`, `build/routing-ui-acceptance-proof/acceptance.json`, `build/routing-ui-asan-proof/acceptance.json`, `build/routing-host-compatibility.json`. Testler bağımsız DB ve ses kaynakları kullanır.

0.15.0 normal uygulama mevcut çalışma DB'siyle `--profile routing` seçilerek açıldı. Salt okuma doğrulamasında yeni ayar API'si HTTP 200, DB bütünlüğü `ok`, şema 4 ve sequence 2 elde edildi; depo/istasyon/uç/kullanıcı UUID'leri ve koleksiyon sayıları korundu. Üretim DB'sine test ayarı yazılmadı. Yakalama başlangıçta kapalıydı; sahip olunan mikrofon/source-output sayısı sıfırdı. Kanıt `build/routing-production-verification.json` içindedir. Wayland penceresinin ekranda eşlenmesi bu salt okuma X11/PID kontrolünden gözlenemedi; gerçek GTK pencere smoke kabulü ayrı testtir.
