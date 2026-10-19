# 12. aşama — yayın, kayıt ve acil akış

Yerel C/GTK uygulaması son MAIN miksini gerçek WAV kaydına ve MP3 stream çıkışına verir. Doğrulanmış acil liste aynı C motorunu devralır; operatör normal yayın sırasına bu oturumun ses konumundan dönebilir. Tasarımın yedi görünümü ve kaynak CSS aktarımı korunur.

## Kullanım

**Yayın → Yayın çıkışı · Kayıt** bölümünü açın. Kayıt dosya adını `.wav` uzantısıyla girip **Kaydet** sonucunu bekleyin; **Başlat** MAIN miksini `data/recordings/` altında kaydeder. **Durdur** son PCM'yi işler ve WAV başlığını/dosyayı sonlandırır. Var olan dosya üzerine yazılmaz. Yeni kayıt için yeni ad kaydedin. Kayıt 48 kHz, stereo, PCM16 biçimindedir; CUE ayrı kalır.

İnternet yayını için `http(s)://sunucu:port/mount.mp3`, kaynak kullanıcısı ve 64/96/128/160/192/256/320 kbit/sn MP3 hızını kaydedin. Şifreyi girip **Başlat** kullanın. Şifre DB'ye yazılmaz, yalnız oturumda ve encoder worker'ında bulunur; kabul edilen başlatmadan sonra formdan temizlenir. Açılışta kayıt, stream veya mikrofon otomatik başlamaz. Bağlantı hatası açıkça gösterilir; yeniden bağlanma operatör komutudur.

**Yayın bilgisi** başlığını kaydedip **Bilgiyi gönder** kullanın. Başlık Icecast `/admin/metadata` API'sine UTF-8/URL kodlamasıyla gönderilir. Otomatik seçenek kaydedildiğinde, gerçek çalma konumu ilerleyen sıranın başlığı gönderilir. Metadata HTTP sonucu stream PCM/byte ilerlemesinden ayrı gösterilir; yeni stream oturumunda aynı başlık tekrar gönderilebilir.

**Acil → Acil liste / Geçici sıra → İçerik ekle** içinde kütüphaneden doğrulanmış medya seçin. Kaynak süresi ölçülür; gerçek medya/sürüm/dosya UUID'leri ve SHA kalıcı satıra bağlanır. Kaydedilmiş medya CUE'da dinlenebilir. **Devralmayı başlat…** içinde kayıtlı sırayı seçip MAIN kapsamını onaylayın. Native kontrol, kaydedilmemiş veya henüz onaylanmamış DB yazısını yürütmez.

Başlatma isteği kaydedilmiş revision'ı, istasyonu, kaynak bağını ve gerçek dosya SHA'sını yeniden doğrular. Normal PCM bu işlem sırasında sürer. İşlem manifest'i kalıcı olduktan ve C planı doğrulandıktan sonra sıra devralınır. Durum, gerçek MAIN tüketimi gelene kadar “hazırlanıyor” gösterir. **Normal yayına dön** bekleyen isteği iptal eder veya önceki sırayı sürdürür. Crossfade sırasında dönüş, sesi zaten tüketilmiş gelen öğeden devam eder; eski öğenin kalan kuyruğu bırakılır. Normal otomasyonun hazır devam partisi korunur; hazır parti yoksa kaydedilmiş kaynak/checkpoint'ten yeniden hazırlanır.

## Ses ve veri sözleşmesi

- `src/audio/output.c`, son DSP/gain/MAIN kapısından sonraki 48 kHz stereo PCM'yi alır. Renderer disk/ağ işlemi yapmaz; iki ayrı beş saniyelik sınırlı kuyruk ve worker kullanılır. Kuyruk taşması açık hata üretir. Fiziksel MAIN saati kesilirse stream beş saniyelik kaynak zaman aşımıyla durur.
- WAV worker'ı özel izinle, exclusive create ile açar; başlığı, ölçülen PCM boyunu ve `fsync` sonucunu doğrular. RIFF'in 4 GiB sınırında geçerli kısmi dosya ve açık hata kalır. Uzun kayıt/RF64/segment kabulü bu sürümün kapsamında değildir.
- MP3 encoder FFmpeg kütüphanesinden C ile çalışır; kaynak akışı Icecast uyumlu HTTP PUT/raw body kullanır. TLS doğrulaması açık, yönlendirme kapalıdır. Ağda submitted byte sayısı, yerel PCM frame sayısı ve alınan HTTP kabulü ayrı tutulur. Metadata ayrı sınırlı network worker'ıdır.
- `src/ui/broadcast_ui.c` profil revision'ından runtime manifest'i oluşturur, sonucu gerçek C snapshot/makbuzundan toplar. Geciken yanıt, iptal veya manuel kaynak değişimi eski isteği yeniden başlatmaz. Yerel kayıt sonuçları aynı gövde/işlem anahtarıyla yeniden denenir.
- `src/ui/playout_ui.c` acil geçişten önce kalan doğrulanmış sırayı ve gerçek tüketim cursor'ını tutar. Elle çalınan MAIN deck'i devralma/dönüş sırasında alınmaz. Pause ve otomasyon bağlamı aynı oturumda korunur. Başka sıra komutu eski dönüş kaydını geçersiz kılar.
- `desktop_broadcast.py`, `broadcast_stream`, `broadcast_record`, `broadcast_metadata` ve yönetilen `broadcast_local_run` kayıtlarını station/yetki/UUID/CAS/idempotency ile denetler. Runtime manifest'i değişmez, olaylar append-only ve kullanıcıya aittir. `overview`, `profile-save`, `run-start`, `run-event`, `emergency-prepare` typed workflow'ları bulunur.

Acil `completed` ancak sıranın bütün öğeleri gerçek MAIN tüketiminden COMPLETE makbuzu aldıysa oluşur. Next/skip/Stop/başka kaynak devri `interrupted`; decoder/çıkış hatası `failed` olarak sınıflanır. Sıfır tüketimle tamamlandı veya yayında kaydı üretilmez.

## Kanıt ve sınırlar

Kayıt dosyası, encoder/stream byte'ları, metadata HTTP cevabı ve MAIN kaynak tüketimi ayrı gözlemlerdir. Runtime kayıtları `proof_class: local_main_runtime`, `delivery_proven: false` taşır. Icecast HTTP kabulü sunucunun isteği aldığını gösterir; uzak dinleyici, anten veya akustik teslim kanıtı değildir. Reklam sözleşmesindeki teslim kotası bu yerel olaylardan artırılmaz.

Bu aşamada acil sıra MAIN'i devralır; MAIN'e bağlı mevcut kayıt/stream aynı acil sesi alır. Stüdyo/internet/karasal bağımsız bölgesel kesme, dış GPI/GPO donanımı, TTS sağlayıcısı, uzak canlı giriş decoder'ı ve otomatik sessizlik failover'ı fiziksel altyapı kabulü yapılmış özellikler değildir. Eski hazırlık formları ve serbest dosya/metin alanları kaydedilebilir taslaktır; çalınabilir kaynak veya gerçekleştirilmiş yayın diye gösterilmez.

Normal dönüş kaydı aynı oturumun hafızasındadır. Uygulama çökmesinde son henüz DB'ye yazılmamış olayın dayanıklı kurtarılması/outbox ve yeniden başlatmadan devam 13. aşamadadır. Fiziksel cihaz/karasal zincir ve uzun süreli yayın kabulü 14. aşamadadır.

## Doğrulama

`test_output.c` gerçek PCM→WAV örneklerini, son MAIN DSP/kapısını, CUE ayrımını, dosyanın korunmasını, MP3 PUT/raw body çözümlemesini, HTTP/metadata hata ayrımını, kuyruk taşmasını ve kaynak saati kaybını sınar. Test sunucuları yalnız loopback üzerinde çalışır.

`test_desktop_broadcast.py` ayrı DB'de profil kalıcılığı, yetki, CAS/replay, kapalı veya bozulmuş gerçek SHA kaynağının reddi, değişmez manifest/append-only terminal, kullanıcı sahipliği, HTTP metot doğrulaması ve byte sınırını denetler.

`test_broadcast_ui.c` native form/picker üzerinden gerçek sanal Pulse MAIN/CUE çıkışlarını, kayıt/stream/metadata ve acil iptal/devralma/dönüşü sınar. `run_broadcast_ui.py` ayrı backend kopyası, özel DB, kendi ses modülleri ve auth isteyen loopback mount kullanır. Üretim DB'sine test kaydı eklenmez.

Yayın panelinin profil okumaları ilk açılışa ertelenir; kapalı panel medya içe aktarma sırasında gereksiz DB okuması yapmaz. Genel C belge istemcisinde eski okuma, yeni save ACK revision'ını/formunu ezemez. Devam eden okuma sırasında istenen yenilemeler belge başına bir sonraki okumaya birleştirilir. `/native-data/document-read-order`, gerçek özel HTTP/DB ile eski yanıt, harici CAS ve iki belgenin aynı sahipte coalescing davranışını doğrular; iki özel negatif kaynak kontrolü, koruma kaldırıldığında gerçek hatayı gösterir.

Strict C11 ve birleşik CTest **31/31**, **204,50 saniye** başarılı. Yayın core **8/8**, typed backend **15/15**, native yayın/acil **4/4** başarılıdır. Native ASan/UBSan **4/4**, **79,78 saniye** temizdir (GTK leak kapalı); WAV/stream core **8/8** leak denetimi açıkken temizdir. C yürütücü/planlayıcı/output sanitizer hedefleri ayrıca **3/3** geçti. Yeni genel belge read-order testi normal ve ASan/UBSan altında geçti; korumaları kaldıran iki özel negatif kontrol beklenen gerçek hatayı gösterdi. CUE ve otomasyon regresyonları son kaynaklarla başarılıdır. Güncel API envanteri **206 workflow / 256 yol / 261 HTTP işlemi / 171 backend kaynak dosyası**, kaynak CSS **30 dosya / 1.552 native kural** korunur.

Güncel **0.12.0** C uygulaması ve veri servisi mevcut çalışma veritabanıyla açık. Depo/istasyon/uç kimlikleri, şema 4, sequence **2**, bütünlük **ok** ve üretim koleksiyonları korundu. Gerçek yayın/reklam overview ve boş program prepare API'leri **HTTP 200** verdi. Üretim DB'sine test kaydı yazılmadı; yalnız normal sessiz MAIN çıkışı açık. Sıradaki 13. aşama: yönetim, yetki, izleme, yedek/kurtarma ve dayanıklı kesinti kurtarmadır.
