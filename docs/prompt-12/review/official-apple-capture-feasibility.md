# Prompt 12 — Apple resmî tam ses capture uygulanabilirliği

Sorumlu: `/root/source_review_resume_12`. Checkpoint: 2026-10-09 02:58:12 UTC. Root göreviyle salt okunur kaynak/primary protokol incelemesidir; **yeni capture uygulaması veya canlı provider kabulü değildir**. Önce güncel STATUS ve 02 UTC/AGENT kararları kontrol edildi. Spotify/MPRIS özel incelemesi `/root/provider_analysis_resume_12` sahibindedir.

Mevcut RadioS API'leri, aktif Apple oynatımı/yayını değiştirmeden bağsız başka katalog şarkısını otomatik olarak baştan sona ses varlığına dönüştürmeyi garanti etmez. Gerçek authorized resmi oynatıcı PCM'i ve per-input WAV recorder vardır; PDF'deki sınırsız otomatik tıklama analizi henüz kapanmaz. Gerçek verified library dosyası ayrı analysis_media seçilirse uygulanmış ölçülen waveform/kalıcı marker yolu korunur.

## Mevcut somut kaynak bağları

| Alan | Kaynak | Sonuç / eksik sınır |
|---|---|---|
| Yetkili profile | `tools/radios_apple_music_web.py:262–310`, `Browser._port/_launch` | Tek dedicated kullanıcı profile/DevToolsActivePort, aynı RadioS stream application.name. İkinci helper yeni ayrı yetkili playback profili üretmez. |
| Tab seçimi ve queue | Aynı helper `Browser.connect:321–346`, `PLAY_JS:51–58` | İlk music.apple.com target yeniden kullanılır; onun MusicKit.getInstance queue'su değiştirilir ve play edilir. Aktif resmi şarkı/yayın korunmuş sayılmaz. |
| Cleanup | `Browser.close:353–369`, `main finally:478–479` | Browser.close bütün aynı private Chrome'u kapatır. Bağımsız capture job bu cleanup'ı kullanırsa aktif official session'ı da kapatır. Owned target-only cleanup gerekir. |
| Şarkı/seek | `STATUS_JS:40–49`, `SEEK_JS:62–83`, `seek_current:92–111` | id/catalog/position/duration/isAuthorized ve stale-track seek guard gerçek. Seek0 mevcut instance'ın konumunu değiştirir; independent job/lease/session generation yok. |
| EOM | Helper `main:458–468` | duration−0.3sn, ended/completed **veya başka şarkıya geçiş** finished üretir ve pause eder. Bu tam media EOF kanıtı değildir; son yaklaşık300ms kırpılabilir, farklı şarkı/preview/catalog süre mismatch complete sayılmamalıdır. |
| Audio identity | `official_player.c:176–223`, `routing.c:92–108` | İlk app-name match/tekChrome fallback; inventory sink-input/client/index/name/binary vardır. Tab targetID↔benzersiz audioinput/process/account bağı yoktur. Aynı browser'daki iki tab sesini yanlış route etmeme kabulü yapılmadı. |
| Hesap provenance | Helper STATUS yetkili boolean; `online_services.c:333–342` | Web profile oturumuna dair account generation/fingerprint yok. Mevcut Apple API verify storefront kaynağının id'sini account_id diye etiketler; bu bireysel hesabın benzersiz kimliği değildir. |
| Gerçek PCM | `routing.c:306–325`, `pa_stream_set_monitor_stream` | Sadece seçilmiş sink-input f32 PCM capture, mixed system monitor fallback reddedilir. Gerçek kaynak mevcuttur; catalogue API'nin ses vermemesi repo PCM'i olmadığı anlamına gelmez. |
| MAIN'e dokunmadan tee | `routing.c:327–379`, `live_application_info:150–164`, `audio_routing.h:74–87` | Ayrı recorder exact current output_sink seçerse route move yapılmadan mevcut input tee edilebilir. Farklı sink seçimi mevcut MAIN livewatch'u fail eder. Yeni private null sink otomatik create yalnız main_engine dalındadır; capture-only independent sink/controller yoktur. |
| Capture bütünlüğü | `capture_read:219–243`, `take_capture:467–476`, `worker_main:598–638` | Overrun/identity/disconnect hata sınırları var. Fakat NULL Pulse hole ve queue underfill sıfırla doldurulur; worker available dönüşünü yok sayıp BLOCK sayısını yazar. Wallclock frames/duration/seslipeak, kayıpsız tam asset kanıtı olamaz. Sourceframe/underrun/hole provenance gerekir. |
| Kalıcı medya | Mevcut playlist actualimport→analysis_media→markers | Finalized gerçek tamasset edinilirse bu uygulanmış managed import/UUID/SHA/CAS zinciri yeniden kullanılabilir. Capture tamlığı doğrulanmadan analysis_media kaydetmek yanlış kalıcı kaynak üretir. |

## Resmî protokolün desteklediği olası yeni yol

Chrome'un resmî Target protokolü yeni bir sayfa hedefi oluşturup background açmayı, onu belirli targetId ile attach/close etmeyi sağlar. createBrowserContext ise boş/incognito benzeri yeni bağlam oluşturur; var olan yetkili profile login'ini otomatik taşımış sayılmaz. Bu API'ler target kontrolü sunar; Apple eşzamanlı yetkili tam playback veya Pulse tab demux garantisi vermez. [Resmî Chrome DevTools Target tanımı](https://raw.githubusercontent.com/ChromeDevTools/devtools-protocol/master/pdl/domains/Target.pdl).

Apple'ın kullanıcı storefront endpoint'i, bir Storefront kaynağı döndürür. Kodda bu kaynağın id'sini account_id diye adlandırmak, iki kullanıcı arasında benzersiz account fence çıkarmaz; bu çıkarım endpoint sözleşmesi ve mevcut kodun karşılaştırmasıdır. [Apple Get a User’s Storefront](https://developer.apple.com/documentation/applemusicapi/get-a-user%27s-storefront), [Apple Get a Storefront](https://developer.apple.com/documentation/applemusicapi/get-a-storefront).

İleride mümkün olabilecek ayrıcontroller yolu şu gerçek kabul şartlarını tamamlamalıdır: own targetId-only connect/cleanup ve aynı authorized session generation; aktif official target/input lease'ini dışlama; kendi target'ını benzersiz live audio input/process/client kaynağına eşleme; authorized full playback ve preview olmadığını measured catalog duration ile doğrulama; capture input ready olduğunda baş0/seek0 admission ve gerçek başlangıç örneklerini kaybetmeme; bütün sürede id/catalog/account/position monotonic fence; gerçek ended+tail drain, source frame/hole/underrun/overrun sayımları; full-duration tolerance+finalized measuredSHA; sonra mevcut managedimport/bind/marker zinciri. Böyle bir controller repo'da mevcut değildir. Aynı profile iki MusicKit sayfasının account/playback davranışı canlı authorized hesapta doğrulanmadığından bunu uygulanabilir/etkisiz tamamlanmış kabul etmek için yeterli kanıt yoktur.

Aktif yayının şarkısını baş0'dan beri pasif tee ederek gelecekte verified capture cache üretmek farklı bir olası iyileştirmedir. Başka katalog satırına tıklanınca hemen tam bağımsız analiz gereksinimini karşılamaz; çoğu zaman capture eklenmeden önce şarkı başlamıştır. Baş0'ı tekrar seek etmek aktif oynatımı değiştirir.

## Test kanıtının gerçek sınırı

`tests/run_audio_routing.py:184–218` kendi private Pulse daemon'unda seçilmiş440Hz ve seçilmemiş997Hz/virtualmic880Hz streamler üretir; `test_audio_routing.c:236–270,362–399` gerçek demux, WAV finalize, no overwrite, selected-only MAIN, unrelated sink korunumu ve cleanup ölçer. Host probe da kendi pacat stream/sinklerini kullanır. Bu anlamlı native altyapı kanıtı **Apple live full song, account isolation, two-tab coexistence, doğal media EOF veya preview/ad exclusion** kabulü değildir. Bu inceleme hiçbir yeni native/live test çalıştırmadı, browser açmadı ve provider hesabına dokunmadı.

Sonraki somut iş, mevcut dosya yolunu koruyup target/audio/session izolasyonunu ayrı kontrollü authorized acceptance ile kanıtlamak ve ancak fullcapture guardları hazırsa yenicontroller geliştirmektir. Kullanıcının kaynak tercihi yanıtı henüz yok; elapsed süre yanıt sayılmaz. Katalog-only otomatik P12-03/04 ve canlı sağlayıcı kabulü açık kalır; bütün PDF tamamlandı gösterilmez.

## İncelenen kaynak hashleri

| Kaynak | SHA-256 |
|---|---|
| `tools/radios_apple_music_web.py` | `0d2f9d7f57aac0038f7ef8abc96099dc4248b753602d2cd337b8b79433043665` |
| `src/audio/official_player.c` | `68d1aae693f8c54e1178b27694a33cd9b52db9d07b6a86caefe7e3f5f94bf811` |
| `include/radios/official_player.h` | `360358b63839bd4fbd82928104101cc9b491feebff8e82ab12099413701deda9` |
| `src/audio/routing.c` | `37bcb29b36f74e94ff87aca347ce9cb7557e73d3c832aed577fbba3ad82d3bb0` |
| `include/radios/audio_routing.h` | `a232adcd909c5a212b72b39f3fce4d14912a941f9ec3a2ee471f7e3158706251` |
| `src/core/online_services.c` | `fd7e4461d0e095ce2685ddda877ba2d7968719a53502e68979d1bc3a44eb20c6` |
| `tests/test_audio_routing.c` | `eb9e28dcd8ace5b7a8d0b14bc2e1a9ea1c2937707f208c9bbe65d13470b5adf6` |
| `tests/run_audio_routing.py` | `7f8077d684526410fdab24524994cd3e621ffb84c201ddbfe7d25b1ef4b59449` |
| `tests/run_routing_host.py` | `cbda64a55594ae417fb9cb9dba3fa33b473a15f146dd62a960bce8645adf6634` |
| `tests/test_official_seek.c` | `db5d1578b82da8fa68d91021fd72749262d12225835d35267ae2d2e1bbc60fd8` |

Release/default install/native_data bağımsız son kanıtı [`source-review-release-install-20261009T025812Z.json`](source-review-release-install-20261009T025812Z.json); son Şema1/2/3 PNG SHA9320b803...64f86 ve kaynakların önceki manifestleri requirements/final-source-review checkpoint'lerinde korunur.

İşlem hata kaydı: İlk rg çağrısında tahmin edilen routing.h/audio_player_ui/audio_playout_ui ve sonraki music_match header/core adları mevcut değildi; çıkış2 salt okuma hatasıydı. rg --files gerçek audio_routing.h/test_audio_routing/core/online_services yollarını buldu ve inceleme doğru dosyalarla sürdü. İlk release/install rapor betiğinde fazladan ')' SyntaxError verdi; Python betik çalışmadan durdu, hiçbir belge/kaynak yazımı olmadı. Düzeltilmiş betik sonra actual artefaktları tekrar kontrol etti. Product/runtime failure veya test PASS gibi gösterilmez.
