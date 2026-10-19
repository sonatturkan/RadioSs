# PDF4 — mimari, entegrasyon ve doğrulama

Sorumlu: `pdf4-architecture`. Güncelleme: 2026-10-09 02:38 UTC. Kapsam yalnız `RadioS/` ve `prompt/4/`.

## Kaynakların incelenmesi

Envanter `prompt/4/4.pdf` (tek sayfa) ve `prompt/4/figures/sema_1.png` (5856×638 tek şema) olarak doğrulandı. Tam PDF metni pypdf ile okundu; özgün şema, gömülü PDF görseli ve tam sayfa render takım tarafından karşılaştırıldı. Kaynak sayfada ek teknik talimat yoktur.

PDF, ortak header altına ince neon şarkı ilerleme çizgisi, süre üzerinden sürükleyerek konum değiştirme, ilerlemeyle dolma, sona yaklaşırken dolan/kalan bölüm renklerinin değişmesi ve artan görsel frekans ister. Şema ince beyaz, yuvarlak uçlu ve yumuşak güçlü ışık çizgisidir. Renk eşikleri ve sayısal pulse frekansı PDF'de verilmemiştir; kullanılan sayılar ve kaynak karşılaştırması `docs/pdf4-requirements.md` içindedir. Pulse yalnız görsel animasyondur.

## Mevcut mimari ve dosya sahipliği

RadioS C11/GTK3 kullanır. CMake `radios_core`, `radios_audio`, `radios_playout`, `radios_ui` kütüphanelerini ve uygulamayı kurar. Gerçek decoder/PCM motoru arka planda çalışır; UI snapshot/komut controller'larıyla iletişim kurar. Ortak `radios_header_new()` bütün profil sayfalarının üstünde bulunur.

| Alan | Sahip | İşlev |
| --- | --- | --- |
| `include/radios/header_progress.h`, `src/ui/header_progress.c` | architecture | Yerel/çevrimiçi kaynak seçimi, gesture kimliği ve gerçek seek adapter'ı |
| `tests/test_header_progress.c` | architecture | 28 yerel controller/gesture/ömür/paused-source fixture senaryosu |
| `official_player.c/.h`, `tools/radios_apple_music_web.py` dar seek bölgeleri | architecture | Spotify MPRIS ve Apple MusicKit kimlik korumalı gerçek transport |
| `tests/test_official_seek.c`, `tests/test_apple_music_seek.py` | architecture | Provider komutu, async receipt ve gerçek JS guard fixture'ları |
| `neon_progress.h/.c`, `tests/test_neon_progress.c` | requirements | Saf Cairo widget, Gaussian halo, renk/pulse ve pointer/keyboard davranışı |
| `tests/test_song_seek.c`, `tests/test_online_progress.c`, `tests/test_youtube_seek.c` | requirements | Gerçek yerel PCM, online adapter ve gerçek YouTube worker fixture'ları |
| `header.c` neon/layout, `cmake/Pdf4Neon.cmake`, minimum CMake include | root | Ortak header ve hedef entegrasyonu |
| `audio.h/engine.c`, `audio_ui.h/.c` dar cue/generation API | root | Mutex altında kimlik/cue doğrulama ve paused runner seek koruması |
| `header_online_progress.h`, broadcast benzersiz facade/Play-offset hunks, YouTube guarded API | root | Gerçek external MAIN gözlemi ve güncel yetkiyle kaynak korumalı seek |
| `docs/pdf4-architecture.md` | architecture | Bu kalıcı teknik kayıt |
| `prompt/agent/AGENT.md` PDF4 bölümü ve saatlik PDF4 logları | coordination | Sahiplik, tarih/saat, karar, hata/çözüm ve süreklilik |

Diğer prompt oturumları aynı workspace'te etkin olduğundan ortak kaynak değişiklikleri canonical `prompt/agent/source-edit.lock`, ardından gerçek kaynak `.lock` altında canlı preimage/anchor kontrolüyle yapılır. Eski snapshot ile diğer oturum hunks'ı silinmez. Kaynak yedekleri ve pre/post hash'leri `build/pdf4/backups/` ve saatlik loglarda tutulur. Architecture ortak header/CMake/engine/broadcast kaynaklarını değiştirmez.

## Header adapter'ın güncel davranışı

1. Öncelik: çalan etkin global yerel playout; çalan çevrimiçi kaynak; çalan manuel MAIN yerel kaynak; duraklatılmış çevrimiçi kaynak; duraklatılmış/hazır yerel kaynak. Yerel tarama yalnız deck0–31'dir; CUE/preview32 ve sampler33–65 dahil edilmez.
2. Fiziksel/UI snapshot'ta gerçek associated runner'ın `runner_paused` durumu ayrıca yayınlanır. Global veya özel per-player AutoDJ owner duraklatılmışsa decoder fiziksel PLAYING kalsa bile çubuk PAUSED olarak sıralanıp gösterilir. Global sıra WAITING/MANUAL_WAIT de sessiz durumdur. Sıranın seçilmiş deck'i ve duraklatılmış overlap partner'ı bağımsız çalan manuel deck gibi yeniden seçilmez. Gerçek cursor değiştirilmez.
3. Başarılı yerel seek decoder'ı LOADING ardından READY durumuna geçirirse açıkça seçilmiş şarkı korunur; başka duraklatılmış yerel deck'e atlamaz. LOADING sırasında seek devre dışıdır. Bu tercih actual generation/medya/sürüm değişiminde silinir; üst sıradaki çalan kaynaklar her zaman önceliklidir.
4. Yerel kaynak gerçek generation, medya/sürüm kimliği, MAIN yönlendirmesi ve süre taşımalıdır. Gerçek `cue_in_ms..cue_out_ms` aralığı kullanılır; cue_out bilinmiyorsa gerçek medya süresi sınırdır. Global plan cue aralığı fiziksel cue aralığıyla kesişir. Gösterilen zaman cue başlangıcına göre hesaplanıp aralığa sıkıştırılır.
5. Online kaynak `RadioSHeaderOnlineProgress` facade'ından gözlenen `current/playing/seekable/generation/position_ms/duration_ms/source_id/title/artist` değerlerini alır. Duration0 için uydurma süre verilmez; görünüm0/0 ve seek devre dışı kalır. `current=FALSE` kaynak seçilmez. Facade gerçek external MAIN owner ve hazırlığına bağlıdır.
6. `seek-begin` deck veya online kaynak türü/ID'si, generation, medya/sürüm/entry ve cue sınırlarını sabitler. Commit öncesi yeniden seçim/kimlik denetimi yapılır. Eski şarkı release'i yeni şarkıya seek gönderemez. 50ms yenileme kaynak değişiminde `seekable=FALSE` verip pointer grab/preview'ı iptal eder.
7. Yerel commit cue başlangıcını ekleyip gerçek `radios_audio_ui_seek_if_generation()` çağırır. Online commit exact source ID/generation/konumla `radios_broadcast_header_online_seek()` çağırır. Callback hatası `ctx.notify` ile iletilir. Komut kabulü oynatılmış zaman gibi gösterilmez; sonraki gerçek snapshot yetkilidir.
8. Destroy timer'ı kaldırır, pending gesture ve seek callback'ini temizler. Saf widget canonical konumu preview'dan ayrı tutar; callback kabulü actual playback cursor'ını taklit etmez.

## Gerçek transport ve kimlik güvenliği

Yerel motor beklenen generation ve gerçek cue hedefini mutex altında, runner devralma/queue iptali öncesinde denetler. Geçersiz hedef eski sırayı etkilemez. Geçerli manuel seek, runner'ın deck'ini devralıp otomatik sırayı durdurur; bu mevcut manuel komut davranışıdır. Önceden çalan runner çalmayı sürdürür; duraklatılmış owner için captured play intent FALSE tutulur ve seek otomatik resume yapmaz. Preview32 public motor guard'ında normal runner owner dizinine sınır dışında erişilmez ve CUE-only davranışı korunur.

YouTube offset seek fresh operate authorization içinden geçer. Callback source/user/card/index/play-intent/paused-state bilgilerini yeniden denetler; reddedilen veya superseded istek mevcut audio/queue'yu korur. Asıl player guard engine/video/station/user/auth/config kimliğini mutex içinde cancellation öncesinde doğrular. Paused seek başlangıcı atomiktir, actual external MAIN reservation korunur ve PCM tüketilmez. Başarılı seek helper subprocess'ine gerçek offset_seconds gönderir.

Spotify public guard expected official ID, actual provider duration ve observed CanSeek/object path denetler. Komut MPRIS `SetPosition` `(object-path, microseconds)` ile gönderilir; track ID gerçek `mpris:trackid` değeridir, provider catalogue ID'si object path yerine kullanılmaz. Bağımsız refcounted async receipt player destruction/replacement sonrası güvenlidir; provider reddi snapshot.error'a ulaşır. Yeni cursor ancak actual Position gözleminden alınır; paused Position0 da işlenir. [Resmi MPRIS Player specification](https://specifications.freedesktop.org/mpris/latest/Player_Interface.html).

Apple stdin seek komutu actual numeric song ID ve finite konumu taşır. Üretimdeki `SEEK_JS`, MusicKit authorization/actual duration/capability ve nowPlayingItem kimliğini seek öncesi ve sonrası denetler, `seekToTime` çağırır ve önce duraklatılmış aynı şarkıyı duraklatılmış tutar. Değişmiş yeni şarkı duraklatılmaz. Helper hatası nonfatal seek-error event'idir; mevcut route/song korunur. Observed currentPlaybackTime gerçek cursor kaynağıdır. Apple'ın [resmi MusicKit v3 SDK](https://js-cdn.music.apple.com/musickit/v3/musickit.js) `seekToTime` ve paused enum3 değerini doğrular; yerel kaynak kanıtı `build/pdf4/musickit-v3-source.js` içindedir.

Official duration getter yalnız finite `(0,86400]` actual provider length döndürür; diğer değerlerde0 verir. Direct seek de nonfinite/unknown actual length ve sınır dışı hedefi reddeder. Bu alan metadata kartının tahmini uzunluğuyla doldurulmaz.

## Doğrulama kanıtları

İlk ortak build salt okunur envanterinde Release/Ninja/strict ON, BUILD_TESTING ve GUI testleri OFF idi; `ctest -N`0 verdi. Bu bir test başarısı değildi. Uygulama/build doğrulaması root'un `build/pdf4/checks`, sanitizer doğrulaması architecture'ın `build/pdf4/sanitizers` izole ağaçlarında yapılır; ortak build yeniden configure edilmez.

Son sanitizer yapılandırması Debug, BUILD_TESTING/GUI/strict ON; C flags `-fsanitize=address,undefined -fno-omit-frame-pointer`, executable linker `-fsanitize=address,undefined`.

| Son actual source kapsamı | Case | Sonuç ve kalıcı kanıt |
| --- | ---: | --- |
| Saf neon widget | 11 | PASS; renk, artan pulse, pause freeze, geometry, pointer/keyboard, cancel/destroy |
| Yerel header adapter | 28 | PASS; deck/cue/rank, atomic generation, stale gesture, output/error/destroy, accepted-source retention, gerçek logical pause |
| Online header adapter | 20 | PASS; actual cursor/freeze, exact seek, unknown/busy/output, ID/generation guards, source priority ve paused global |
| Gerçek engine/decoder | 10 | PASS;997Hz hedef PCM, playing/paused, cue/generation, runner takeover, paused owner silence/freeze, preview CUE isolation |
| Gerçek YouTube worker/engine fixture | 7 | PASS; offset997Hz PCM, paused MAIN reservation/silence/freeze, video/station/user/auth stale no-mutation |
| Official actual C transport/receipt fixture | 10 | PASS; observed MPRIS path/microseconds, capability/actual length, pausedzero actualobservation, error/obsolete/destroy receipt ve Apple nonfatal error |
| Apple protocol + actual production JS fixture | 9 | PASS, skip0; stale/invalid/helpererror, paused/playing seek, replacement/auth/duration/capability guards |

Son adapter GTK CTest2/2 PASS2.85s,48 case (28yerel+20online); `ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 G_DEBUG=fatal-warnings GDK_BACKEND=x11`. Yeni runner_paused ABI ve özel AutoDJ display/delegation/resume case'i dahildir. GTK global allocation'ları nedeniyle leakcheck kapalıdır; UAF/UB hatası görülmedi. Kalıcı stdout/sourcehash kanıtı `build/pdf4/sanitizer-observed-pause-adapters-20261009T023627Z.log`. Değişmemiş Gaussian widget11 PASS kanıtı önceki `build/pdf4/sanitizer-timelines-20261009T023120Z.log` içindedir.

Son engine/provider CTest4/4 PASS3.05s,36 case; C hedeflerinde `ASAN_OPTIONS=detect_leaks=1 UBSAN_OPTIONS=halt_on_error=1`. Actual runner_paused TRUE→FALSE takeover flag testi ve güncel struct ABI dahildir. ASan/UBSan/LeakSanitizer hatası görülmedi. Kalıcı stdout/source SHA kanıtı `build/pdf4/sanitizer-observed-pause-core-20261009T023612Z.log`. Son farklı kaynak kapsamları birlikte7 CTest ve95 kabul senaryosudur (11+28+20+10+7+10+9). Önceki18/22/24/27 yerel,18 online ve7/8 engine sonuçları saatlik logdaki tarihsel checkpoint'lerdir; son delta yerine kullanılmaz.

Apple actual JS fixture QuickJS1.19.4 ile yürütüldü; paket yalnız `build/pdf4/test-js-runtime` içine kuruldu, product dependency değiştirilmedi. Test QuickJS veya Node varsa exact üretim script'ini çalıştırır; ikisi yoksa JS case'leri açıkça skip edilir. Bu ortamda skip0'dır. İlk Node absence skip ve QuickJS global alias ReferenceError aşamaları loglandı; globalThis harness düzeltmesi sonrası kabul tekrar çalıştırılıp geçti.

Root ayrıca strict whole-app link, mevcut audio/runner/native_controls/native_window/native_audio regresyonları ve fresh authorization pipeline8 gerçek privateDB/context case PASS raporladı. Bunların en son yeni kaynak full regresyon ve responsive screenshot kabulü root/requirements kayıtlarında ayrıca izlenir. Architecture bu fixture sonuçlarını canlı Spotify/Apple hesabı, DRM, gerçek ağ veya cihaz/onair kabulü olarak sunmaz.

## Son entegrasyon kabulü ve doğrulama sınırları

Root'un `build/pdf4/final-validation.json` kaydı (2026-10-09 02:37:50 UTC) güncel strict13 hedefin derlendiğini, kritik production kaynaklarında derleme sonrası hash drift0 olduğunu ve8 feature CTest/103 senaryonun PASS sonucunu doğrular. Son suite listesi10engine+11widget+28localadapter+7YouTube+10official+9Apple+20onlineadapter+8freshauthorization UI'dır. Kalıcı kaynak kanıtları `build/pdf4/final-feature-cases.log` ve `build/pdf4/final-feature-tests.xml` içindedir; feature toplam runtime16.13s. Fresh authorization UI gerçek privateDB/context response guard, source/pause/STOP değişimi, permission denial ve busy owner davranışını sınar; provider bridge bu testte kontrollü fixture'dır.

Beş mevcut regresyon suite PASS; `build/pdf4/final-regression.log` ayrıntıları içerir. Eski `youtube_music_audio` suite'i private Pulse daemon gerektirdiğinden **SKIP**: bu ortamda `pulseaudio` executable yoktur. Atlanan suite başarılı gösterilmez. Yeni YouTube7 gerçek worker/offlineengine/targetPCM fixture'ı ve native_audio kabulü ayrı PASS kanıtlarıdır; bunlar atlanan private Pulse testinin yerine canlı cihaz kabulü sayılmaz.

Production header + actual CSS/Inter font ile aynı pencere1920→1280→1000→1920 akışındaki42 section allocation>1 ölçüldü; wide yüksekliği267'ye geri döndü, metadata ve gerçeksectiongeometrisi uyumludur. Neon row4 span4/3/2 ve24px allocation korunur; container48+inner10 inset dengelidir. İlk1280 resize eski wide GTK minimumuyla1514'e clamp olur; yeniden request/freshwindow1280 gerçekleşir. Tek ilk request1280'in uygulandığı iddia edilmez. Pending layout idle sırasında immediate destroy ve50ms event pump fatalwarning vermedi. Kanıt `prompt/agent/analysis/requirements/header-final-20261009T023646Z/`; root yeni native-window whole-app capture02:36:19'u ayrıca inceledi.

Global ve özel per-player AutoDJ logical pause gözlemi, gerçek guarded seek, provider identity/receipt ve kaynak değişimindeki stale gesture korumaları son current-source runtime/sanitizer kanıtlarıyla tamamdır. Canlı Spotify/Apple hesabı, DRM, gerçek provider ağı ve cihaz/onair kabulü bu offlinefixture çalışmalarıyla doğrulanmış sayılmaz. Ayrıntılı mutation/karar/hata/çözüm saatlik kayıtları ortak AGENT PDF4 bölümünden izlenir.
