# 9. aşama — DJ mixer ve geçişler

RadioS 0.9.0, tasarımın DJ kontrollerini C ses motoruna bağlar. Kaynak CSS aktarımı korunur; mixer, knob, fader, pad ve düzenleyiciler GTK/C bileşenleridir. Normal uygulama ses çözme ve DSP için harici komut çalıştırmaz.

## Kullanım

1. Medya kütüphanesine ses dosyalarını içe aktar. Doğrulanmış kaydı normal oynatıcıya yükle.
2. DJ kanalının **Kaynak** alanından 32 oynatıcıdan birini veya stüdyo mikrofonunu seç. Bir kaynak birden fazla kanala atanabilir; bunlar ayrı kanal katkıları olarak toplanır.
3. Kanal EQ/PAN/fader, Mute/Solo ve Effector seçimini kullan. **SEND → Kanal** ile sesi başka kanala gönder; **RETURN ← Kanal** ile başka kanaldan ses al. Yanlarındaki SEND/RETURN knobları kanal bağlantısının seviyesidir; efekt katkısının FX SEND/FX RETURN knobları kapalı **Efekt seviyesi** bölümündedir. A/B grubunu seçip crossfader ile karıştır. Atanmamış kanallar crossfader'dan etkilenmez.
4. CUE için önce Teknik yönetimden gerçek CUE çıkışını ata. Kanal CUE, seçili kanalı ana mute/solo/crossfader'dan önce dinler. Efekt CUE, efektin wet dönüşünü dinletir. Efektör tablosunda ayrıca SEND/RETURN veya monitor yolu seçicisi yoktur. Medya/geçiş önizlemesi CUE'da kendine ayrılan oynatıcıyı kullanır.
5. Her kanalda ve MASTER'da dört kare pad 2×2 dizilir; padin sağ üstündeki **⋯** düzenleyiciyi açar. Pad düzenleyicisinde **Kütüphaneden seç** ile doğrulanmış sürümü ata ve kaydet. Atanmış pad çalınır; aynı aktif pade tekrar basmak durdurur. Başka pade basmak aynı sampler sesini yeniden yükler. Farklı kanalların padleri birlikte çalabilir.
6. **Mixer ayarlarını kaydet** kalıcı DJ ayarlarını gönderir. Kaydın başarılı olduğu veri servisi yanıtıyla bildirilir. MASTER gain, ortak ses aygıtı tercihleri kaydıdır; bu kaydın `manage` yetkisi ve ayrı sonucu vardır. Teknik yönetimde uygulanmamış cihaz seçimi varsa mixer kaydı bu alanları korur ve ses tercihlerini ayrıca yazmaz; ortak gain'i Teknik yönetimde uygulama/kaydetme ile kaydet. **Yeniden yükle**, kaydedilmemiş değişiklik varsa açık seçim sunar.
7. Geçiş editöründe A/B için **Kütüphaneden seç** kullan. A süresi gerçek analizden doldurulur. Cue/out, B cue/fade, bindirme ve B sırasında A kısma değerlerini gir; **Ön dinle** yalnız CUE'da gerçek A/B miksini çalar. **Durdur** ve pencereyi kapatma hazırlığı da iptal eder. Kaydetmek kaynak UUID/SHA bağlarını ve değerleri saklar.
8. Canlı konuşmadaki **ON AIR**, mikrofonun gerçek kapısını açar/kapatır; kaynak düğmesiyle aynı durumu gösterir. Açmak için mikrofon kaynağı ve MAIN çıkışı hazır olmalıdır. Uygulama yeniden açıldığında kapalıdır. Bu düğme yerel ana miksin mikrofon kapısıdır; tek başına uzaktaki internet yayınının aktif olduğunu kanıtlamaz.

Serbest dosya yolu eski taslaklarla uyumluluk için metadata olarak korunur. Canlı pad/geçiş oynatması bu yola dayanmaz: istasyon, medya, sürüm, dosya UUID'si ve SHA-256 birlikte doğrulanır. Dosyayı önce kütüphaneye içe aktar.

## Kanal kaynakları, etiketler ve yayına verme (18.10.2026)

**Kaynak** menüsü "Kaynak yok", "Stüdyo mikrofonu" ve Oynatıcı 1–32 seçeneklerini korur; ardından gelenler:

- **Giriş: ‹aygıt›** — ses kartı girişleri (analog/dijital kayıt kaynakları). Liste ses motorunun aygıt listesinden gelir; çıkışların geri dinleme (monitor) kaynakları sonda, "· Çıkış geri dinlemesi" ekiyle durur. Aygıt takılıp çıkarılınca menüler seçimi koruyarak yenilenir.
- **Ağ: ‹ad›** — kayıtlı ağ kaynakları; her kanalın menüsünde görünür.
- **Ağ kaynağı…** — Ad ve URL soran küçük pencere. Yalnız `http`, `https`, `rtp`, `udp`, `rtsp`, `srt` adresleri kabul edilir; bunlardan sistemin FFmpeg'inde bulunmayan protokol (bu kurulumda `srt`) açık nedenle reddedilir ("Bu sistemin FFmpeg'inde SRT desteği yok…"); dosya, pipe, data gibi yerel protokoller reddedilir. Kaydedilen kaynak o kanala atanır.

Canlı giriş seçilen kanalın oynatıcı ataması kalkar (`source_deck = -1`, "+ Medya" pasif). Giriş sesi 48 kHz stereo PCM olarak kanal şeridine verilir; EQ, PAN, fader, Mute/Solo, CUE, efekt ve MAIN yönlendirmesi oynatıcı kaynağındaki gibi uygulanır. Ses kartı girişi ayrı bir Pulse/PipeWire kayıt akışıdır; ağ kaynağı FFmpeg ile açılır ve çözülür. Bağlantı açılamazsa ya da koparsa 5 saniyede bir yeniden denenir. Bağlantı sürdüğü hâlde 5 saniye ses gelmezse kanal "Veri gelmiyor" olur (seviye 0) ve ses dönünce aynı bağlantıyla "Hazır"a döner. Kanalın durum satırı "Giriş/Ağ · ‹ad› · Hazır / Bağlanıyor / Veri gelmiyor / Yeniden denenecek" gösterir; son hata metni araç ipucundadır. Oynatıcı ya da mikrofon seçmek canlı girişi durdurur.

**Etiket** (Kaynak'ın altında): —, DJ, Konuşma, Müzik. Etiket kanalın canlı yayın adımındaki işlevidir: DJ → DJ mikrofonu, Konuşma → konuşma kanalı ve live event şeması, Müzik → otomasyon şeması. Mixer etiketi yalnız saklar; yayın sırasındaki L düğmesi ve program/live event ayarları bu etiketi okur.

**Yayına verme**: `radios_audio_ui_set_channel_on_air(kanal, açık)` yalnız kanalın Mute durumunu kaldırır/koyar. Fader (elle verilen düzey), yönlendirme ve oynatıcının transportu değişmez; değişiklik şeritteki **M** düğmesinde görünür ve otomatik kaydedilir.

Kalıcılık `dj_mixer` belgesindedir: `channels[i].label` (`"dj"`, `"talk"`, `"music"`; etiketsizde alan yoktur), `channels[i].input` = `{"kind": "capture"|"network", "device": ‹kaynak adı ya da URL›, "name": ‹görünen ad›}` (bu durumda `source_deck` -1 olmalıdır) ve `network_sources` = `[{"name", "url"}]` (en çok 32). Ad 1–128, aygıt/URL 1–512 UTF-8 bayttır. Veri servisi (`_validate_dj_mixer`) aynı kuralları uygular. Belge yeniden yüklenince etiketler ve girişler ondan alınır; değişmeyen girişler kesilmeden sürer ve yükleme geri kayıt üretmez.

## C ses grafiği

32 normal oynatıcı, yalnız önizlemeye ayrılan 32 numaralı dahili oynatıcı, 32 kanal sampler sesi ve MASTER sampler sesi vardır: toplam 66 sınırlı ses. Mikrofon ayrı yakalama girişidir. Kaynak PCM her karede bir kez tüketilir; 32 mixer kanalı bu PCM'yi seçilen kaynaktan alır. Pad sesi kendi kanalına eklenir, normal parçayı değiştirmez. Dört pad bir kanalın tek sampler sesini paylaşır; MASTER'ın dört padi ayrı tek sesi paylaşır.

Canlı kanal kimlikleri silmede sıkıştırılmaz. Kaldırma yalnız ilgili kanal yönlendirmesini ve sampler sesini durdurur; kalan kanallar yerinde kalır. `channel.slots` bu boşlukları yeniden açılışta korur. Eski yalnız `deck_count` kayıtları okunabilir.

Kanallar arası SEND/RETURN yönlendirmeleri gerçek DSP ses grafiğidir. SEND seçicisi kaynak kanaldan hedefe, RETURN seçicisi seçilen kaynaktan bulunduğu kanala tek yönlü bağlantı kurar. İkisi aynı bağlantıyı tarif ederse ses iki kez eklenmez. Kaynağın post-fader/pan/crossfader/efekt katkısı SEND ve hedefin RETURN seviyesiyle çarpılır; ardından hedefin EQ/pan/fader/efektinden geçer. Kaynak kanalın doğrudan katkısı korunur; mute/solo kapalı kaynak göndermez. Kanal sırasından bağımsız işlenir. Kendi kanalına bağlantı ve dolaylı geri besleme döngüleri reddedilir; kanal kaldırılırken ona ait bağlantılar temizlenir. MASTER bu kanal grafiğinin hedefi değildir. Mikrofon katkısı müzikten ayrı tutulur; yönlendirilen konuşma ducking ile kısılmaz, kapı kapandığında kanal efektlerindeki konuşma geçmişi temizlenir.

Kanal EQ üç banttır: 200 Hz low shelf, 1 kHz peaking, 6 kHz high shelf; aralık tasarımdaki **-26..+6 dB**'dir. Stereo pan merkezde iki tarafı korur; sağa pan sol tarafı, sola pan sağ tarafı kosinüs eğrisiyle azaltır. Kanal gain/pan ve A/B crossfader değişimleri 240 örnekte, 48 kHz'de 5 ms'de yumuşatılır. A/B eşit güç eğrisidir: merkezde her grup yaklaşık -3,01 dB'dir. Mute/Solo ana katkıyı denetler; kanal CUE EQ/gain sonrasını, mute/solo/crossfader öncesini alır. Seçili CUE yolları toplanır; önizleme oynatıcısı seçilince bu toplam önizlemeye karışmaz.

Üst alan MASTER gain ile DJ MASTER fader **aynı** `audio_preferences.main_gain_db` değerini yönetir. Üst alan ve DJ MASTER EQ da aynı master işlemcisidir. Gain ikinci kez uygulanmaz. Çıkış PCM'si ±1 sınırında tutulur; bu işlem ölçülmüş true-peak limiter veya LUFS normalizasyonu iddiası değildir.

Mikrofon kullanıcı tarafından açılır. Ducking gerçek kapısı açık mikrofonun PCM seviyesini algılar; -46 dBFS yaklaşık eşiği, 100 ms hold ve yumuşak attack/release ile müzik katkısını belirlenen bed seviyesine indirir. Mikrofon kapandığında müzik seviyesi geri gelir. Prova sayacı yalnız prova sayacıdır; on-air yayın kanıtı üretmez.

## Efektler

Beş bağımsız parametre slotu ve MASTER efekt işlemcisi vardır. Kanal seçtiği slotu kendi sınırlı geçmişiyle işler; slot parametreleri ortaktır. MASTER da **Effector** ile ortak slotlardan birine kendi FX SEND/FX RETURN ve ayrı geçmişiyle katkı gönderir; MASTER'ın kendi efekt işlemcisi ayrıca çalışır. **Efekt seviyesi** bölümündeki FX SEND/FX RETURN gerçek efekt katkısını değiştirir; kanallar arası SEND/RETURN bağlantısından ayrıdır. Her kanalın en fazla bir ortak slotu etkin olabilir. Efektörün tablosu tür, tempo, DEPTH, CUE ve aç/kapat kontrollerini içerir; eski kayıtlardaki slot kazançları uyumluluk için korunur. Bypass ve DEPTH gerçek wet katkısını kapatır/değiştirir. Tempo yerel BPM parametresidir, harici beat/MIDI senkronizasyonu değildir.

| Tasarım adı | Uygulanan C işlemi |
|---|---|
| Delay | BPM/depth ile değişen tek gecikme tap'i |
| Echo | Sınırlı feedback'li gecikme |
| Spiral | Modülasyonlu gecikme, feedback ve genlik hareketi |
| Reverb | Dört farklı gecikme tap'i ve feedback |
| Trans | BPM ile ritmik gate |
| Filter | Depth ile değişen low-pass |
| Flanger | Kısa modülasyonlu gecikme ve feedback |
| Phaser | Dört all-pass aşaması |
| Robot | Ring modulation |
| Melodic | İki gecikme kafasıyla yerel pitch kaydırma |
| Slip Roll | Yakalanan kısa döngüyü tekrar; kaynak transport ilerler |
| Roll | Tekrar döngüsü aralarında yeni PCM yakalama |
| Rev Roll | Yakalanan döngüyü ters okuma |
| MIDI LFO | Yerel LFO ile genlik modülasyonu; MIDI aygıtı bağlantısı yok |

Bunlar tasarım adlarına bağlanan ayrı, sınırlı C algoritmalarıdır. Harici bir DJ cihazının özel algoritmalarının birebir emülasyonu olarak sunulmaz. Efekt geçmişi sabit boyutludur; ses callback'inde dosya okuma veya heap ayırma yapılmaz.

## Geçiş ve kalıcılık

A/B önizlemesi doğrulanmış iki çözücüyü worker üzerinde sample frame konumuyla karıştırır. A overlap boyunca cos eğrisiyle azalır, B kendi fade süresinde sin eğrisiyle artar. Ducking B girişinde A'ya uygulanır. Aynı envelope fonksiyonu hem bağımsız DSP hem gerçek iki dosyalı hazırlıkta kullanılır. Son örnek sınırı SHA ile eşleşen analizdeki sample frame sayısından hesaplanır; milisaniyeye yuvarlanmış süre dosyanın sonunu aşmaz. Önizleme A bindirmesinden önce en fazla 10 saniye ve A bittikten sonra en fazla 10 saniye içerir; toplam 90 saniye sınırı vardır. PCM sabit bloklarla özel 0600 geçici WAV'a yazılır, SHA hesaplanır ve CUE çözücüsü çıktıyı ayrıca doğrular. Durdurma, yeni önizleme, CUE kapatma veya sahip pencerenin kapanması geciken hazırlığın sesi yeniden başlatmasını engeller. Geçici dosya temizlenir; son uygulama kapanışı cancellable runtime işlerini join eder.

`dj_mixer` kaydı tam 32 kanal, MASTER EQ/mute/send/return/slot, beş efekt, MASTER efekt, crossfader ve ducking ayarlarını `edit` yetkisiyle CAS/alındı üzerinden tutar. `sampler` kayıtları isteğe bağlı dört bağlı kimliği, `transition` ise isteğe bağlı `media_a`/`media_b` bağlarını tutar. Eski biçimler geçerlidir. Açılışta çalma, mikrofon ve CUE/PFL kapalıdır; kaydetme sırasında etkin canlı CUE durumu gereksiz yere sıfırlanmaz. Yeni yerel ayarlar bekleyen eski yanıtla ezilmez.

Kanallardaki isteğe bağlı `send_channel`/`return_channel` alanları sıfır tabanlı kanal kimliğidir; `-1` bağlantıyı kapatır. `send_db`/`return_db` aralığı -60..+6 dB'dir. Eski kayıtlarda bağlantılar kapalı, gönderim -6 dB ve alım 0 dB başlar. Hem native istemci hem veri servisi kendi kendine bağlantı ve döngüleri reddeder. ON AIR canlı durumdur ve kalıcı mixer kaydına yazılmaz.

MASTER şeridinde de aynı **SEND → Kanal**, **RETURN ← Kanal** seçicileri ve seviye düğmeleri bulunur; FX seviyeleri ayrı kalır. MASTER bağlantıları `dj_mixer.master` altında aynı dört isteğe bağlı alanla kaydedilir. Kanal kaldırılınca MASTER'ın o kanala bağlı seçimleri de temizlenir. Eski kayıtlarda MASTER bağlantıları kapalıdır.

MASTER SEND, EQ/MASTER efektlerinden önceki ön miksi hedefin kanal RETURN seviyesinden geçirir. Hedef ve ona bağlı sonraki kanallar bu gönderim toplamına alınmaz: her kanal bir kez işlenir, hedefin dönüşü tekrar kendisine gönderilmez. Harici müzik, açık mikrofon ve MASTER sampler bu ön mikse dahildir; müzik ducking'i son toplamda yalnız bir kez uygulanır. MASTER RETURN, seçilen kanalın mevcut ana miks katkısını **kanal SEND × MASTER RETURN** seviyesinde alır; ikinci bir kopya eklemez. Aynı kanalın SEND ve RETURN olarak seçilmesi güvenlidir; kanallar arasındaki döngüler yine reddedilir. MASTER mute ana çıkışı ve MASTER gönderimini susturur.

Bu aşama kalıcı yerel miks taslağı ve gerçek ses önizlemesidir. Playlist/program/takvimden otomatik A/B geçiş yürütmesi aşama 10; stream/kayıt/acil yürütmesi aşama 12; fiziksel donanım ve uzun süre kabulü aşama 14 kapsamındadır. Taslak medya teknik doğrulaması on-air onayı/kanıtı üretmez.

## Doğrulama

- `test_dsp.c`: gerçek örneklerle routing/pad/PFL/solo, pan/gain/crossfade, ölçülmüş EQ cevabı, 14 ayrı efekt, send/return, kanal zinciri/tek bağlantı/geri besleme reddi, MASTER ve yönlendirilmiş mikrofon ducking/kapı/efekt geçmişi grupları.
- `test_transition.c`: gerçek iki doğrulanmış WAV'dan bağımsız beklenen sample değerleri; A ducking/B fade, seviye, sert geçiş, süre sınırı, hash/iptal ve dosya temizliği.
- `test_dj_ui.c` / `run_dj_ui.py`: izole gerçek SDBASE, native kontroller ve benzersiz sanal MAIN/CUE/MON akışlarının yakalanan PCM'si; kalıcılık/CAS, canlı CUE'nun kayıtta korunması ve açılışta kapalı olması, dört kare pad, kanal gönderim/alımının PCM seviyesi ve kalıcılığı, döngü reddi, kanal kaldırmada bağlantı temizliği, ON AIR kapısının senkronu ve açılışta kapalı olması, pad retrigger/kimlik/sahiplik, kanal boşlukları, gerçek A/B önizleme ve geciken işin yeni sesi ele geçirememesi.
- `test_desktop_api.py`: DJ exact schema/range, gerçek medya foreign key bağları, yetki/istasyon sınırı, CAS/replay ve yeniden açılış bütünlüğü.

```sh
ctest --test-dir RadioS/build --output-on-failure -R '^(dsp|transition|native_dj|desktop_api)$'
```

Test fixture'ları yalnız `RadioS/build/` altındadır; üretim DB'sine test medya/pad/mixer kaydı eklenmez. Ses testleri fiziksel çıkış veya gerçek mikrofonu kullanmaz.

Son kabul: strict C11 derlemesi, birleşik CTest **22/22** (173,78 saniye), DSP **13/13**, motor **9/9** ve ek gerçek Pulse grubu, geçiş **3/3**, typed backend **18/18** başarılıdır. Core DSP/motor/geçiş ASan/UBSan ve leak detection açıkken temizdir. Son düzeltmeler normal native ses **6/6** (27,89 saniye), ASan/UBSan native ses **6/6** (50,61 saniye), güçlendirilmiş native DJ normal **5/5** (32,73 saniye) ve ASan/UBSan **5/5** (54,77 saniye) ile ayrıca doğrulandı. GTK global verileri için native sanitizer kabulünde leak detection kapalıdır. Son A/B yakalaması 72.050 ses karesi, 1.501 ms son cursor ve hatasız gerçek EOF gösterdi.

Mevcut çalışma DB'sinin kimlikleri, sequence **2**, şema **4** ve bütünlük **ok** korundu. Güncel serviste DJ listeleme **HTTP 200** döndü; üretimde test mixer/medya kaydı yoktur. **0.9.0** C penceresi kullanıcı denemesi için açık bırakıldı.

7 Ekim 2026 kanal SEND/RETURN ve ON AIR güncellemesi: strict derleme başarılı; ilgili motor/DSP/geçiş/backend/native hedefleri **13/13**, DSP **17/17** ve native DJ **6/6** geçti. Native kabulde kare padlerin gerçek boyutları/2×2 konumları, sanal çıkıştan ölçülen kanal yönlendirmesi, kalıcılık, döngü reddi, kanal silmede bağlantı temizliği ve ON AIR kapısı doğrulandı. Üretim mikrofonu/yayını test için açılmadı. Güncel C uygulamasıyla birlikte çalışan Python veri servisi de yeniden başlatılmalıdır; eski servis yeni mixer yönlendirme alanlarını tanımaz.
