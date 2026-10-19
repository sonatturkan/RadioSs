# Prompt 3 gereksinim, uygulama ve doğrulama kaydı

İnceleme: 2026-10-09 01:49 UTC, `/root/requirements`. Son checkpoint: 02:37 UTC — kaynak ve hedefli kabul tamam. Uygulama kapsamı yalnız RadioS. Kaynak `../prompt/3/`; diğer prompt oturumlarının gereksinimleri bu matrise eklenmez.

Güncel kabul: PDF3'ün aşağıdaki dört izlenebilir gereksinimi uygulandı ve doğrulandı. Event sonunda bütün satır beyaz/siyah uyarı verir; gerçek sıradaki parça siyah başlar ve ölçülen ilerlemeyle sona doğru hızlanan biçimde beyazlaşır. Yerel/online akış ve kabul edilen programme modelinin düzenlenen taslaktan bağımsız kalması test edildi. PDF3 için açık uygulama veya kabul görevi yoktur; başka oturumların bütün repository gereksinimleri için tamamlandı iddiası verilmez. İzlenen 53 vaka: queue14 + programme_model2 + engine/runner24 + online11 + gerçek PCM1 + scheduled PCM1. Bu sayı farklı frozen snapshot'lara bağlı vaka kayıtlarının toplamıdır; 53 CTest hedefi veya bütün repository'nin son snapshot'ındaki 53 test sonucu değildir.

## Tam kaynak envanteri ve inceleme kanıtı

| Kaynak | Kapsam | İnceleme |
| --- | --- | --- |
| `../prompt/3/3.pdf` | 1 sayfa, iki şema ve açıklamaları | pypdf ile bütün sayfanın metni çıkarıldı; PyMuPDF ile 1.8 ölçek render oluşturulup tamamı görsel incelendi. |
| `../prompt/3/figures/sema_1.png` | 758 × 262, RGB; event genel listesi | Orijinal çözünürlükte incelendi; PDF embedded görseliyle RGB piksel karşılaştırması birebir aynı. |
| `../prompt/3/figures/sema_2.png` | 758 × 262, RGB; event içindeki parça listesi | Orijinal çözünürlükte incelendi; PDF embedded görseliyle RGB piksel karşılaştırması birebir aynı. |

Recursive envanterde başka PDF veya figures dosyası bulunmadı. PDF metni ve render birbiriyle tutarlı; gizli ek sayfa/gereksinim yok. PDF xobject görsel sırası şema sırasının tersi olduğu için piksel karşılaştırması her iki çapraz eşleşmeyle yapıldı.

Kaynak SHA256:

- `3.pdf`: `ea0dc22db34c64de3a675c6ab83348dad7785d881dff7f4b78a8ca5ce112c57a`
- `sema_1.png`: `4616bc597f466abecaafa62e3742a2d812eb326ca9432a76d7bf7cf130dcd4ee`
- `sema_2.png`: `fc4f438ac1d2a69d19448126c2aa95414255046830ab9c38ce70c33eacf6b4b9`

## Sayfa bazlı gereksinim matrisi

| Kimlik | PDF sayfa / şekil | Kaynak talimatı | Mevcut kod ve boşluk | Planlanan uygulama / kabul ölçütü | Durum |
| --- | --- | --- | --- | --- | --- |
| P3-01 | 1 / Şema 1 | Event bitince beyaz-siyah uyarı yanmalı. | `src/ui/programme_queue.c` event satırları `.programme-block-row`; başlangıçta `playing_apply` yalnız çalma ve seçim durumlarını uyguluyordu. | Event'in gerçek son çalınabilir parçası tamamlanınca ait olduğu event satırının bütün arka planı sırayla beyaz/siyah olur; normal sonraki event oynarken önceki event uyarısı korunur. Akışın atladığı sessiz/dosyasız son içerikler bitiş uyarısını engellemez. Başlatma, manuel parça değişimi, hata ve durdurma doğal bitiş olarak işaretlenmez. | Tamam — queue14/14, frozen-owner/silent-tail sanitizer; gerçek yerel PCM1/1 ve doğal online UI completion+full11 PASS |
| P3-02 | 1 / Şema 2, ilk cümle | Yakında çalacak parçanın arka planı siyah. | İçerik görünümü `.programme-child-row`; başlangıçta sıradaki parça durum/ilerleme API'si yoktu. | Çalan parçanın gerçek sıradaki parçası siyah arka planla başlar. Yeni parça çalmaya başladığında onun sıradaki parçası yeniden siyahla başlar. Sonraki parça yoksa hiçbir yanlış satır işaretlenmez. | Tamam — computed siyah/RGB ve doğru next kimliği; gerçek yerel upcoming satırı, online status ve full11 PASS |
| P3-03 | 1 / Şema 2, ikinci cümle | Bitmeye yaklaştıkça siyahtan beyaza dönüşme hızlanmalı. | `RadioSPlayoutSnapshot` gerçek position/duration verir; başlangıçta queue yalnız set_playing alıyordu. Online flow status başlangıçta position/duration taşımıyordu. | Gerçek çalma ilerlemesi sıradaki satırın rengini giderek artan hızda siyah→gri→beyaz değiştirir. Yerel, YouTube Music ve karışık akışlar aynı queue API'sine gerçek ölçülen ilerlemeyi verir. | Tamam — computed p²/artış/pause/unknown-duration; gerçek cue-relative yerel ve online ölçülen ilerleme kabulü PASS |
| P3-04 | 1 / iki şeklin arayüzü | Uyarı ve sıradaki parça görselliği gösterilen event/parça tablosuna uygulanır. | `programme_queue.c` No/Event veya Parça/Süre/Kontroller, sayı butonları, S/V/Ö/×, hazırlık etiketleri ve gezinmeyi zaten sağlar. | Satır yapısı, sıralama, kontroller ve gezinme korunur; açık/koyu uyarı zemininde başlık/süre/durum okunur; seçim renginin yeni görsel durumu bastırması engellenir. | Tamam — 14 GTK regression ve 5 bağımsız PNG görsel kabulü; tam No/time satır zemini, açık/koyu metin ve kontrol kontrastı |

## Açık teknik kararlar ve kaynak sınırları

PDF, uyarı frekansı, uyarının ne kadar sürmesi gerektiği veya fade eğrisi için sayısal değer vermez. Root kararı: gerçek çalma ilerlemesi `t = clamp(position / duration, 0, 1)` üzerinden `t²` gri seviyesi kullanılır; böylece eşit zaman aralıklarında beyaza dönüşün hızı sona doğru artar. Event tamamlanma uyarısı 500 ms'de bir beyaz/siyah değişir. Bunlar kaynakta bulunmayan sayısal uygulama kararlarıdır; yeni ürün gereksinimi olarak sunulmaz.

Duraklatma sırasında ölçülen ilerleme sabit kalır; bilinmeyen sürede sıradaki satır siyah kalır. Öğe kimlikleri kullanılarak açma/kapatma, render, event/parça yeniden sıralama ve silmede doğru satır korunur. Queue yok edildiğinde zamanlayıcı kaldırılır.

Yeni queue API sözleşmesi:

- `set_progress(queue, playing, next, position_ms, duration_ms, paused)` gerçek akış ilerlemesi.
- `complete(queue, leaf)` yalnız event'in son çalınabilir parçası doğal bitmişse event uyarısı. Akışın zaten atladığı sessiz/dosyasız içerikler son çalınabilir parça hesabına katılmaz; tümden sessiz event için yapay completion üretilemez.
- `set_run_config(queue, accepted_config)` gerçek accepted akışın event/leaf modelini derin kopyalayarak draft düzenlemelerinden korur; NULL frozen modeli temizler.
- `reset_progress(queue)` yeni başlangıç/durdurma için görsel, uyarı ve frozen model durumunu temizler.

## Uygulama sırasında dosya sahipliği

| Sorumlu | Dosyalar | İş |
| --- | --- | --- |
| `/root/source_audit` | `src/ui/programme_queue.c`, `include/radios/programme_queue.h` | Queue ilerleme ve tamamlanma; yalnız queue satırlarına/alt widget'larına uygulanan scoped CSS provider ile renk ve kontrast. |
| `/root` | `src/ui/broadcast.c`, `src/ui/playout_ui.c`, ilgili header ve online flow alanları | Gerçek yerel/online ilerleme ve doğal completion entegrasyonu. Ortak `assets/themes/radios.css` değiştirilmez; root bu dosyanın sahipliğini bırakmıştır. |
| `/root/requirements`, devam `/root/requirements_resume` | Bu dosya; `tests/test_programme_queue.c`; açık dar devirle yalnız online `test_flow_plays_youtube_events()` | Tam kaynak incelemesi; hedefli renk/timer/kimlik/frozen-owner; gerçek online UI completion testi. Dar source yazımı bitti ve sahiplik geri serbest bırakıldı. |
| `/root/coordination` | Ortak `../prompt/agent/AGENT.md` prompt3 bölümü, `../prompt/agent/prompt3/STATE.md` ve ayrık loglar | UTC saatli log ve sahiplik, kapsamlar arası çakışma takibi; ortak dosya dış oturumlarca değişirse ayrık devam kaydı. |

## Tarihsel doğrulama kanıtı — 2026-10-09 01:58 UTC

`tests/test_programme_queue.c` uygulama CSS dosyasını yükler ve gerçek üretim selector hiyerarşisini kurar (`.radios` → `#live-task-view` → queue). Testler yalnız CSS sınıfına bakmaz; GTK computed background RGB değerlerini ve seçili beyaz satırda başlığın koyu rengini doğrular.

Strict C11 standalone derleme (`-Wall -Wextra -Wpedantic -Werror`) başarılı. `GDK_BACKEND=x11 DISPLAY=:0 build-prompt3/test_programme_queue_standalone` sonucu **12/12 PASS**: mevcut 7 gezinme/sıralama/drag testi ve yeni 5 progress/completion vakası.

Yeni testler siyah başlangıç, %25/50/75 ilerlemede `t²` gri, beyaz bitiş, son aralığın daha hızlı dönüşmesi, seçili beyaz satırda koyu başlık, yeni sıradaki satırın tekrar siyah başlaması; duraklatma, bilinmeyen süre, yanlış/same-next kimlik, sonrasız son parça, taşan position clamp, reset, event yalnız son leaf tamamlanması, 500ms beyaz→siyah, başka event oynarken uyarının korunması, replay temizliği, render/gezinme/reorder sonrası kimliğin korunması ve yok edilirken aktif timer temizliğini doğrular.

İlk denemede ortam `GDK_BACKEND=wayland` olduğu için custom GTK clipboard seçimi başarısız oldu. Gerçek X11 backend ile bütün mevcut ve yeni vakalar geçti. Eşzamanlı testlerin aynı synthetic clipboard'a sahip olmaması için her drop benzersiz UUID atomu kullanır.

`build-prompt3/artifacts/requirements/` altındaki `next-black.png`, `next-grey.png`, `next-white-selected.png`, `event-completed-white.png`, `event-completed-black.png` orijinal çözünürlükte görsel incelendi. No/Parça veya Event/Süre/Kontroller, düğmeler ve gezinme kaynak şemalarla tutarlı. İncelemede sayı alt hücresinin önceki koyu background'ı görülünce source_audit tam satır arka planının görünmesini sağlayan şeffaf alt widget düzeltmesi yaptı.

02:01 UTC: Son source_audit düzeltmesiyle strict standalone yeniden **12/12 PASS**. Ek assertion'lar No/time alt hücresinin computed background alpha değerinin sıfır olduğunu (satır rengi görünür), %99 ilerlemenin yaklaşık 0.9801 beyazlık verdiğini, unknown-playing + valid-next ile block-next kimliklerinin sıradaki parça durumunu üretmediğini doğruladı. Güncel requirements ekran görüntüleri yeniden üretildi; beyaz No/time hücresi, koyu yazı ve düğme kontrastı ayrıca incelendi. Source_audit bağımsız strict ve ASan/UBSan run sonuçları da 12/12 PASS olarak koordinasyon kaydında bildirildi; GTK process-global leak check kapalıdır.

02:04 UTC ek inceleme: karışık akışın atladığı sessiz/dosyasız son leaf'ler nedeniyle literal son leaf kontrolü event uyarısını eksik bırakabiliyordu. Root kararıyla `complete` son **çalınabilir** leaf sınırına düzeltildi; hedef leaf'in kendisi de gerçekten çalınabilir olmalıdır. `event-completion-silent-tail` adlı 13. test sessiz son içerik, daha sonra eklenmiş gerçek son parça, düzenleme sonrası dosyasız son parçalar ve tümden sessiz event'in yapay completion'ı reddetmesini kapsar. Source_audit son kaynak/testle strict standalone **13/13 PASS**, bu yeni vakanın ASan/UBSan run'ında **1/1 PASS** bildirdi. Önceki 12 vakanın sanitizer doğrulaması ayrı kayıtlıdır.

Önceki 12/12 testle doğrulanan queue source SHA256: `c1de7e6668a16131f18da6f57eda75ae1fd78aebb05b7fd8fa00b142e58460dd`. Sessiz son içerik düzeltmesinin son 13/13 testle doğrulanan queue SHA256 değeri `24c553c3fa036b0489578c0e7b5dfaa835f433d52bb52d65629d3fed8fcd7835`; queue header SHA256 `e1e550307566ed4ee785ed975c74c9ac754369faccdcd6ec851040c828fd742b`.

Root gerçek iki WAV PCM `test_queue_pcm()` akışına sıradaki kimlik, cue/ilerleme ve doğal FINISHED completion assertion'larını ekledi. Üretim CMake GUI derlemesi ve bu entegrasyon testlerinin nihai sonuçları bekleniyor; henüz bütün gereksinimler uçtan uca doğrulandı iddiası yoktur.

## Online akış kabulü ve yeni sahiplik sınırı — 02:10 UTC

Root göreviyle önce `build-prompt3/artifacts/requirements/flow-progress-review.c` özel frozen test kopyası oluşturuldu. Shared test dosyası bu işlemde değiştirilmedi. Ninja'nın gerçek CMake compile/link komutları ve güncel core/ui/audio arşivleriyle strict derleme/link başarılı; private SDBASE fixture içindeki `/broadcast-online/flow-plays-youtube-events` **1/1 PASS** (3.18 s). Kaynak/kopya hash'leri ve exact komutlar `flow-progress-review-manifest.json` dosyasında kayıtlı.

Ardından ortak kayıt üzerinden önceki test sahipleri Root2 ve PDF10/validation yalnız `tests/test_broadcast_online_ui.c::test_flow_plays_youtube_events()` için açık dar yazma devri verdi. Requirements canonical global/file lock altında güncel dosyayı tekrar okuyup yalnız beş assertion hunk'ını minimal unified patch ile aktardı. Diğer fonksiyonlar, main, includes ve başka oturumların header identity/duration/pause/preparing/stop assertion'ları korundu. İş bitişinde dar alan sahipliği geri serbest bırakıldı. Yedek, diff ve manifest: `build/prompt3-backups/requirements/20261009T021026Z/`.

Güncel gerçek CMake `test_broadcast_online_ui` hedefi strict compile/link PASS; **shared kaynak binary** için aynı private SDBASE filtered test **1/1 PASS** (3.17 s). Bu test gerçek SDBASE/akış/UI kodunu kullanır; provider bridge ve ses donanımı mevcut wrapper'larla yalıtılır. Assertions sessiz taslağı atlayan gerçek next kimliği; 42500/180000 ms ilerleme; pause sırasında aynı ilerleme; ikinci parçada 25250/200000 ms ve sonraki parça olmaması; FINISHED/STOP sonrası playing/next/position/duration temizliği ve STOP completion kuyruğunun boşluğunu doğrular. Doğal online `take_completed` doğrudan one-shot assertion'ı normal queue tick'in bu kuyruğu tüketmesi nedeniyle race üreteceğinden eklenmedi; bu davranış test edilmiş gibi sunulmaz.

Online shared test preimage SHA256 `7d676bd86b22564579f78495aa448889397d425904d116c55d7d68fdc6ded0b1`; aktarılan hunk sonrası SHA256 `c7c3fb4f4e4907e76beffd203eb2acd2511b4e1f9d531a3acbc257bf2c494251`.

Root 02:07 UTC tam CMake üretim/test hedefleri strict compile/link PASS ve current 13-case native queue CTest PASS (6.41 s) bildirdi. Sonrasında yeni bir uç durum bulundu: oynarken düzenlenen draft parça sahipliğinin gerçek accepted akıştan farklılaşması. Source_audit `set_run_config(queue, accepted_config)` API'sini serialize/parse ile derin kopyalanmış frozen event model olarak uyguladı. Requirements 14. `event-completion-frozen-owner` testi borrowed input'u yerinde değiştirir, C'yi başka event'e taşır ve yeni audible tail ekler; tamamlanma/replay'in accepted event'e bağlı kalmasını ve accepted event draft'tan silinince yanlış satır alarmı oluşmamasını doğrular.

Son source_audit strict standalone **14/14 PASS**; yeni frozen-owner için ASan/UBSan **1/1 PASS**. Kesin sanitizer kanıtı önceki 12 vakanın tam run'ı ve sonrasında silent-tail/frozen-owner için ayrı 1'er affected vaka şeklindedir; güncel bütün 14 vakanın tek sanitizer run'ında çalıştığı iddia edilmez. Son queue SHA256 `c9ed98fb2c0840d34c64f2d2b542642f76bd3e5ae3624b345629484a7c3ab953`; header SHA256 `ab49d6e60df82231fd1b62604bddfa3f7c61fc17d95a6ec26e298586d86f70b0`.

## Bağımsız son görsel ve kaynak incelemesi — 02:14 UTC

Son frozen-owner kaynakla strict `final-queue-review` binary yeniden derlendi; yalnız yeni görsel üretmek için `upcoming-progress` ve `event-completion-alert` hedefli testleri **1/1 + 1/1 PASS** çalıştırıldı. Son 5 PNG `build-prompt3/artifacts/requirements/final-queue/` altında yeniden oluşturuldu; tamamı view_image ile orijinal çözünürlükte kaynak PDF/figures karşılaştırılarak incelendi. Tam satır ve No/time hücresi siyah/gri/beyaz; seçili beyaz satır koyu metin; sayı ve S/V/Ö/× düğmelerinde koyu zemin/açık yazı; event satırında beyaz/siyah alternansı kaynak talimatıyla tutarlı. Yeni görsel eksik bulunmadı.

Son kod incelemesi gerçek accepted modelin derin kopyasını, event/leaf kimliğiyle doğru satır seçimini, silinen event'in başka event'e aktarılmamasını, replay uyarı temizliğini, timer yok edilmesini, scope edilmiş provider önceliğini, pause/unknown-duration ve artan `t²` dönüşümü doğruladı. O an root'un yeni accepted-config binding entegrasyonu sürüyordu; gerçek PCM testinde timeout ve başka kapsamın geniş online selected-playlist vakası başarısızlığı açıktı. İlk click aşaması yorumu bu anda doğrulanmamış bir çıkarımdı; 02:16 kaynak/private-canary teşhisi timeout'ın bütün PCM/upcoming/completion assertion'ları sonrasındaki son disabled STOP cleanup click'inde olduğunu düzeltti. Bunlar tamamlanmış kabul olarak raporlanmaz; root nihai sonuçları bu matrise ekleyecek.


## Oturum devamı ve doğal online completion UI kabulü — 02:21 UTC

Kullanıcının yanlışlıkla kapanan oturumu devam ettirme talimatıyla yeni requirements ajanı matrisi, ortak AGENT ve ayrık STATE/log kayıtlarını okuyarak devam etti. Kaynak envanteri ve üç SHA256 yeniden doğrulandı: 1 PDF, 1 sayfa, 2 figures kapsamı değişmedi; daha önce tamamlanmış bütün render/görsel incelemesi geçerlidir. PDF metninden doğan P3-01/02/03 talimatlarının matrix dışında bırakılmış ek talimatı bulunmadı.

`build-prompt3/artifacts/requirements/flow-progress-review.c` özel frozen kopyasında normal broadcast UI timer üzerinden doğal online completion acceptance eklendi. İlk run ilk tamamlanmış event class assertion'ında FAIL oldu. Sebep testin catalogue-only `open_harness` yardımcı fonksiyonunun `ctx.playout=NULL` kullanmasıydı; üretim `broadcast_new` bu durumda programme UI timer kurmaz. Bu ilk sonuç bir üretim completion başarısızlığı olarak yorumlanmaz. Yalnız özel kopyanın aynı flow fonksiyonunda gerçek audio ve playout controllers kuruldu; ordinary 100ms UI timer aktive edildi. Shared helper/production source değiştirilmedi.

Bu yeni private kaynak gerçek CMake/Ninja strict compile/link flags ve güncel UI/core/audio/playout arşivleriyle **compile/link PASS**; `GDK_BACKEND=x11 DISPLAY=:0 python3 tests/run_online_services_ui.py build-prompt3/artifacts/requirements/flow-progress-review -p /broadcast-online/flow-plays-youtube-events` **1/1 PASS, 4.15 s**. İlk online parça doğal FINISHED geçişi sonrası eski event overview row'u `is-event-completed`, sessiz taslak ve yeni çalan event tamamlanmamış doğrulandı. Son online parça FINISHED sonrası ilk ve son event uyarısı korunurken sessiz taslak uyarı almadı. Header/progress/next/pause/STOP kanıtları aynı vaka içinde korundu. Test provider bridge/ses donanımı mevcut wrappers'ıyla yalıtılmış gerçek SDBASE, gerçek controllers, gerçek queue ve gerçek UI timer kullanır; fiziksel YouTube PCM çalımı iddia edilmez.

Shared `tests/test_broadcast_online_ui.c` bu yeni işlemde değişmedi. Önceki dar write release geri verildiğinden yeni UI assertion hunk'ının shared source aktarımı için yeni dar release bekleniyor. Özel copy PASS, shared son source kabulünün yerine geçirilmez. Root per-player accepted run-config ve yeni playout prepare hook entegrasyonu kaynakta okundu; üretim full strict rebuild ve gerçek yerel PCM sonuçları root tarafından halen doğrulanmaktadır.


## Shared doğal completion hunk ve son frozen üretim kabulü — 02:26 UTC

Root'un önceki açık dar devirler ve güncel kayıtlar üzerinden verdiği devam talimatıyla `tests/test_broadcast_online_ui.c` içinde yalnız `test_flow_plays_youtube_events()` yeni doğal completion assertion'ları aktarıldı. Canonical global→file mutex altında önce fresh shared fonksiyon, özel test kopyasından yalnız beş yeni hunk çıkarıldığında **byte-for-byte aynı** olarak doğrulandı; bütün includes, diğer fonksiyonlar/main, Root2 header ve PDF10/Scope9 fixture assertions korundu. Backup/diff/manifest `build/prompt3-backups/requirements/20261009T022407Z/`. PreSHA `2b1c2ccdbc270c35cfc23d69d9a67d945b9cf1ab5f91083bf3e21f71e171c9ba` → postSHA `7fa4d00e8b74561bd63787d5cbdad1851aa4e4c3125e24fef0e661381c9a7603`. Fiziksel dar yazım bitti ve owner'lara açık release geri bildirildi.

Yeni actual CMake online hedef rebuild'i 02:24 UTC **FAIL** oldu: başka kapsamın `radios_youtube_music_player_seek_if_current` henüz tamamlanmamış public header/body entegrasyonu; `playlist_provider_analyze` declared-but-not-defined ve `track_analysis_media_open` unused strict hataları. Requirements bu dış üretim alanlarını değiştirmedi veya warning kapatmadı. Build kaynak hash'i değişmedi; exact komut, stdout, exit1 ve archive/binary hash'leri `build-prompt3/artifacts/requirements/natural-online-completion-build-failed-0224.log` ve `.json` içindedir. Shell failed build sonrasında yanlışlıkla önceki başarılı binary'ı çalıştırdı: 3.70s filtered PASS **stale executable** olduğu için yeni hunk kabulü olarak sayılmaz. Manifest `acceptance:false` ile bu sınırı açık kaydeder. Yeni online current-source filtered ve bütün 11 vaka kabulü henüz bekler. 02:26 read-only kaynak kontrolünde YouTube body ve public declaration oluşmuştu; bu yalnız source-ready işaretidir, yeni build PASS değildir. Playlist provider analysis hâlâ tamamlanma bekliyordu.

Root'un son accepted-request hook + typed mixed-local-segment guard delta sonrasında frozen üretim binary'ları `build-prompt3/artifacts/root/final/manifest.json` ile kaydedildi. Son **queue14/14 PASS**, gerçek **PCM1/1 PASS15.28s**, **scheduled/cancel/start PCM1/1 PASS8.06s** ve **programme_model2/2 PASS** kanıtı coordinator/root tarafından aktarılmıştır. `queue.log`, `pcm.log`, `schedule.log` ve ayrı result JSON'ları gerçek TAP/exit kanıtlarını taşır. Bu yerel PCM vakası actual next ID + upcoming satırı, farklı iki gerçek ton ve crossfade, doğal event completion + receipt dedup, FINISHED STOP disabled ve gerçek sessizliği doğrular. Scheduled vaka accepted-request frozen binding'in gerçek planlı akış yolunu kapsar; bütün başka kapsam takvim testlerine başarı genişletilmez. Yeni online hunk sonrası latest CMake rebuild/bütün vaka kanıtı tamamlanmadan üst matriste tüm gereksinimler uçtan uca tamamlandı olarak işaretlenmez.


## Nihai hedefli kabul ve devir — 02:37 UTC

Son üretim accepted-run helper, bağımsız yeni local/scheduled run başladığında eski online flow/text'i temizler. Mixed flow'un yalnız gerçek local segment hazırlığı typed flag ile ayırt edilir; full accepted model bu durumda korunur. Yeni online flow kendi accepted modelini yeni flow pointer'ı atanmadan bağlar. Root'un bu son iki kaynak hunk'ı sonrası gerçek yerel PCM **1/1 PASS18.72s**, scheduled/cancel/start gerçek PCM **1/1 PASS6.43s**. Exact stdout, commands/result JSON ve frozen executable/source SHA manifest `build-prompt3/artifacts/root/supersession-final/` içindedir. Test edilmiş binary SHA `0ea8c264032627464e16c5c70a81f32ae13524ed451855791a28a21a2677de47`; gerçek kaynakta accepted hook ve son guard'lar bulunur.

Requirements son actual-source pipeline'ı: **strict radios + test_broadcast_online_ui + test_playout_ui build PASS**,41 gerçek compile/link output adımı; new flow filtered **1/1 PASS4.26s**; full `native_broadcast_online` **11/11 GLib vaka +1/1 CTest hedef PASS12.35s**. Full suite içindeki own doğal event completion vakası **PASS3.96s**. Source before/after build ve test hash'leri eşit: `sources_unchanged_during_build:true`, `source_drift_while_testing:false`. Böylece önceki private copy, stale executable veya sonraki kaynak değişikliğiyle sonuç karıştırılmadı. Son test source SHA `bb9715ded8caaafcaaf1bc6fe53bbfd91c57777687cd6aee91de0865aa8d2e23`; actual frozen online binary SHA `35d247eace1775c1dbb2c8a44e59006fa4d6679e7ad626db3ef192dac23652d8`. Son app binary `build-prompt3/radios`,SHA `52514d81b36f631511d8104fae562e50fa9439474d746883faa125c8b357f635`; default binary kurulumu bu kaydın işi değildir.

Tekrarlanabilir fail-fast komut: `python3 build-prompt3/artifacts/requirements/verify-natural-online-completion.py`. Build/source/binary manifest `natural-online-completion-final-manifest.json`, strict stdout `natural-online-completion-fixed-build.log`, focused stdout `natural-online-completion-filtered.log`, full TAP/CTest stdout `natural-online-completion-full.log`, JUnit `natural-online-completion-full.xml`; tamamı `build-prompt3/artifacts/requirements/` içindedir. Bütün native testler X11 kullanır. Online test gerçek SDBASE, audio/playout controllers, programme save ACK, document apply render, accepted flow hook ve UI timer kullanır; provider bridge/hardware mevcut wrappers'ıyla yalıtıldığı için fiziksel YouTube PCM kabulü iddia edilmez. Yerel PCM kabulü yukarıdaki gerçek Pulse/native vakalarında ayrıdır.

Fixture düzeltme kanıtı açık korunur: ilk manual UI render, önceki case'in kalıcı taslağı refresh edildiğinde yanlış modele dönüyordu; gerçek programme save+ACK ve normal document apply'ye geçildi. Lightweight direct-flow JSON'u kalıcı programme validator'ının zorunlu event alanlarını taşımadığından ilk ACK denemesi rev0→0 FAIL oldu; gerçek required fields ve tam YouTube provider/watch URL eklendi. Sonraki full run'da own flow geçti, Spotify case'inin legacy config.blocks sayısı ile UI migration sayısı farklı olduğundan case6 FAIL oldu; yalnız own fixture gerçek uygulamadaki `radios_programme_tree_config` canonical export'una geçirildi. Diğer case6/selected-provider functions değişmedi ve son full11 geçti. Önceki failed stdout/JUnit/manifestler `*-failed-inconsistent-model-0228.*`, `*-failed-schema-0230.*`, `*-failed-legacy-counts-0233.*`, external-build failure `*-failed-0224.*` altında saklanır. Başarısız denemeler silinmedi veya PASS sayılmadı.

Son queue üretim kaynağı değişmedi: strict/native **14/14 PASS**,5 PNG bağımsız görsel incelemesi; önceki12 tam sanitizer ve silent-tail/frozen-owner için ayrı1'er affected ASan+UBSan PASS. Bütün14'ün tek sanitizer run'ında çalıştığı iddia edilmez. Source/backend runner için24/24 zero-skip ve programme_model2/2 sonuçları kendi frozen snapshot manifest/loglarına bağlı ek regresyon kanıtıdır; bütün repository'nin güncel bütün testleri geçti anlamına gelmez.

Final hunk backup/diff/manifestleri: `build/prompt3-backups/requirements/20261009T022407Z/`, `20261009T022946Z/`, `20261009T023142Z/`, `20261009T023458Z/`. Her biri canonical global→file mutex, latest preimage ve yalnız own flow function minimal patch ile oluştu. Bütün diğer includes, functions/main, Root2 header ve PDF10/Scope9 assertions korundu. Fiziksel dar write claim kapanmıştır; root ve ortak kayıt koordinatörüne final SHA/sonuç ve sahiplik serbest bildirildi. PDF3 kapsamında bekleyen gereksinim/test yoktur.
