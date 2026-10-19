# Playlist program teması görsel doğrulaması

Kayıt: 2026-10-10 23:17:36 UTC. Sorumlu: `/root/theme_visual_qa_12`. Üretim ve test dosyaları salt okunur incelendi. Bu belge yalnız yeni playlist tema isteğinin görsel kontrolünü kapsar.

Kullanıcı talimatı: “playlist sekmesini burada program temasında yap”. Yeni görsel kabul kaynağı, mevcut RadioS koyu program temasıdır. Önceki PDF şemasının beyaz/turkuaz satırları ve zeytin seçimi tarihli önceki kanıtta korunur; bu istekte koyu program paletine uyarlanır. Playlist işlevleri, iki tablo ve gerçek marker analizi korunur.

## İncelenen kaynaklar

- `RadioS/AGENTS.md`, `docs/prompt-12/STATUS.md` ve ortak AGENT son sahiplik/kurulum kayıtları.
- `tests/test_prompt12_playlist_ui.c`: `screenshot_style`, `screenshot`, `/prompt12/inline-analysis-marker-stale-selection`.
- `tests/run_data_ui.py`: RadioS/build altında özel test dizini, gerçek SDBASE init/start/stop, özel config ve assets kopyası; üretim veritabanı kullanılmaz.
- `assets/themes/radios.css`: mevcut siyah/koyu yüzeyler, açık gri metin, cyan vurgular, Inter yazı tipi, hover/focus/disabled stilleri.
- [Önceki playlist ekranı](screenshots/playlist-with-markers.png): 1280×2040; açık yüzeyler ile koyu marker paneli birlikte görülür.
- [Program koyu tema referansı](../../prompt-2/screenshots/final-20261009T023434Z/main-normal.png): siyah sayfa, koyu paneller, açık gri kontroller ve cyan başlık vurguları.

## Gerçek capture yöntemi

Mevcut native test, ölçülmüş gerçek WAV import/analiz/marker kayıtlarını yaptıktan sonra seçili üst tablo satırı ve alt tabloyla bütün marker panelini yakalar. Pencere varsayılanı 1280×1200; panelin doğal minimum yüksekliği önceki capture’da 2040 piksele büyür. Bu uzun ekran tüm kontrolleri görsel incelemek içindir; kullanıcı masaüstü viewport kabulü ayrıca değerlendirilmelidir.

Test `RADIOS_P12_SCREENSHOT_ROOT` varsa `radios_reference_css_parts` ve `assets/themes/radios.css` birleşimini APPLICATION önceliğinde yükler, dark-theme tercihini açar ve `assets/fonts/InterVariable.woff2` kaydını fontconfig’e ekler. Screenshot seçili satır 0 ve 150 ms paint bekleyişinden sonra gerçek GdkWindow’dan alınır.

```sh
GDK_BACKEND=x11 PYTHONDONTWRITEBYTECODE=1 \
RADIOS_P12_SOURCE_ROOT="$PWD" \
RADIOS_P12_SCREENSHOT_ROOT="$PWD/docs/prompt-12/review/screenshots/theme-20261010" \
python3 tests/run_data_ui.py build/<fresh>/test_prompt12_playlist_ui \
-p /prompt12/inline-analysis-marker-stale-selection
```

Üretilen dosya: `playlist-with-markers.png`. Build/test/capture root sorumluluğundadır. QA ajanı yeni test yazmaz ve üretim kaynaklarını değiştirmez.

## Güncel sonuç

Bağımsız görsel inceleme **ACCEPT** — 2026-10-10 23:23:28 UTC. [Gerçek yeni native ekran](screenshots/theme-20261010/playlist-with-markers.png) `view_image` ile program koyu tema referansına karşı incelendi. Gerçek boyut **1280×2041**; PNG SHA256 **ac0be232b58f3fa335bf09eddf90096c28a46979db81778c89c42fa8dd811452** bağımsız hesaplandı. Bir piksel yükseklik farkı yeni başlık alt kenarlığıyla tutarlıdır.

| Kontrol | Gerçek ekran ve kaynak sonucu |
|---|---|
| Program teması | Siyah sayfa, #080808/#101010 koyu tablo yüzeyleri, #111 başlık, gri kenarlıklar; eski açık beyaz/turkuaz ve zeytin satırlar görünmüyor |
| Metin ve satırlar | Açık gri metin okunur; ilk üst satır #252525 gri seçili, normal ikinci satır koyu; alt tablo koyu zebra. Seçili metin #ededed |
| Düzen | İki tablo, dar TITLE, CUE sonrası geniş boş alan ve en sağdaki cyan bilgi ikonu, menü/araç çubuğu/playlist seçicileri korunmuş |
| Focus | Gerçek ekranın rekordbox düğmesinde cyan focus kenarlığı görünür; kaynakta entry/button focus #36caff |
| Disabled | Önceki/Sonraki ve marker transport düğmelerinde soluk gri metin/koyu zemin aktif düğmelerden ayırt edilir. Kaynakta explicit :disabled label rengi var |
| Hover/checked | Kaynakta scoped button:hover/checked ve menü hover stilleri var. Hover bu statik ekranla canlı ölçülmüş sayılmaz; satır hover iddiası yok |
| Marker paneli | Sekiz etiket/giriş, koyu panel, marker kaydet/close alt düğmeleri uyumlu; gerçek ölçülmüş cyan waveform ve yeşil marker anlam renkleri korunmuş |

Kaynak renk değerleri üzerinden hesaplanan metin kontrastı: normal tablo `#b5b5b5/#080808` **9,77:1**, alternatif satır `#b5b5b5/#101010` **9,28:1**, seçili satır `#ededed/#252525` **13,09:1**, giriş `#c8c8c8/#070707` **12,04:1**. Bu hesap kaynak renkleri için geçerlidir; bütün piksel/engelli durumları için kapsamlı erişilebilirlik kabulü değildir.

[Gerçek build/test makbuzu](theme-validation-20261010.json) bağımsız okundu: **457 kaynak before/after eşit**, `source_drift=[]`; strict Release app/native test build exit0 **3,172 sn**; native playlist **7/7 actual TAP ok**, `not ok` yok, XML bir wrapper test/0 failure/0 skip; koşu **12,85 sn**. Release ve strict warnings CMakeCache ile teyitli. Testler genuine özel SDBASE kullanır; üretim DB değişikliği iddiası yok. Güncel playlist SHA `075558333e7f0aba47f94d7cf83e46588ccd355b057bc195b9ff4bfd7b733f03`, makbuz source_after ile eşit.

Görsel kabul izole Release adayına aittir. Makbuzdaki `default_binary_written=false` sınırı korunur: bu QA kaydı default kurulum/masaüstü yeniden açılışı olarak gösterilmez. Uzun native ekran tüm marker kontrollerini gösterir; gerçek 1920×1042 masaüstü viewport ayrıca doğrulanmalıdır. Genel PDF12 Apple/Spotify catalogue-only tam ses edinimi QA04 önceki açık iştir ve tema kabulüyle tamamlanmış sayılmaz.

QA ajanı yalnız bu belgeyi yazdı; production/test/design/default dosyalarına dokunmadı. Belge güncellemesinde yalnız kendi `.lock` altında son metin kontrolü kullanıldı. Önceki hazırlık kaydındaki “capture bekliyor” durumu actual yeni kanıtla güncellendi; eski beyaz playlist ekranı tarihli referans olarak korunur.


## Normal uygulama viewport incelemesi

Salt okunur kaynak araştırması — 2026-10-10 23:24:26 UTC. Mevcut app CLI `--profile playlist --smoke-test --capture PNG` normal tam program penceresinde capture yapabilir. `src/app/main.c` capture seçeneğini yalnız smoke ile kabul eder; `G_APPLICATION_NON_UNIQUE` yeni bağımsız app instance kullanır. `src/ui/window.c` smoke sırasında production data/audio/playout client bağlamaz, GUI tek-instance kilidi almaz, kapanışta UI preferences kaydetmez. Smoke gerçek mapped pencere/sekiz profil/iş tesliminden sonra explicit playlist profiline döner, paint bekler, kendi GtkWindow çizimini PNG’ye aktarır ve yalnız test instance’ını kapatır.

```sh
GDK_BACKEND=x11 G_DEBUG=fatal-warnings \
build/prompt12/release/radios --project-root "$PWD" \
--profile playlist --smoke-test \
--capture "$PWD/docs/prompt-12/review/screenshots/theme-20261010/native-playlist-window.png"
```

Program viewportu varsayılan 1800×1000 ve minimum 1000×680; gerçek masaüstü WM nedeniyle mapped boyut farklı olabilir. Tam uygulamada her profil Automatic yatay/dikey scrolledwindow içinde, natural height propagation kapalıdır. Böyle longshot doğal minimum yüksekliği program penceresini zorla büyütmez. Bu app smoke data bağlamadığından boş playlist görünümü verir; gerçek kayıtlı satır/ölçülen marker kabulü önceki genuine privatefixture shot ile korunur. Existing PID1741’e yönlendirme/tıklama/kapatma yapılmaz.

Bu bölüm yöntemin kaynak incelemesidir. QA ajanı komutu çalıştırmadı; gerçek normal viewport capture ve sonucu root/P2 sahibinden beklenir. Defaultbinary kurulum/reopen kabulüne genellenmez.


## Normal app capture başarısız koşusu ve alternatif

Kayıt: 2026-10-10 23:27:07 UTC. Root actual native `--profile playlist --smoke-test --capture` koşusu [theme-window-validation-20261010.json](theme-window-validation-20261010.json) içinde **FAIL**, exit **-5**, **2,531 sn**, screenshot **null**, before/after watched kaynaklar eşit/drift[] olarak korunur. [Ham log](theme-window-20261010.log) **23:24:57.319 UTC Gtk-CRITICAL gtk_widget_draw !alloc_needed** assertion bildirir. Fatal warnings gevşetilmedi. Bu koşu program viewport veya default kabulü değildir; önceki privatefixture görsel/7 native test kabulü ayrı kapsamıyla geçerlidir.

Salt okunur inceleme: `src/ui/window.c:347` generic `capture_window` özel Cairo redraw çağrısıdır; smoke timer180ms profilleri değiştirip yalnız bir ilave paint tick sonrasında çağırır. `src/ui/header.c:header_layout_idle` ayrıca `gtk_widget_queue_resize` kuyruğu oluşturabilir. [Resmî GTK3 kaynak](https://raw.githubusercontent.com/GNOME/gtk/gtk-3-24/gtk/gtkwidget.c) `gtk_widget_draw` fonksiyonu current allocation preconditionlarını kontrol eder. Çalışan GTK sürümü ctypes ile **3.24.52** teyitlidir. Runtime log pending allocation bulunduğunu kanıtlar; bunu yalnız playlist renk değişikliğine bağlayan A/B trace yoktur. Tema işi generic capture methodunu değiştirmedi.

`--smoke-test` **capture olmadan** aynı native mapped/sekiz profil/iş teslimi yolunu çalıştırırken `capture_window` null path erken dönüşüyle custom draw’ı atlayabilir. Bunun sonucu root actual koşusu olmadan PASS sayılmaz. [Resmî GDK framebuffer API](https://docs.gtk.org/gdk3/func.pixbuf_get_from_window.html) `gdk_pixbuf_get_from_window` pencere piksel verisini kopyalar; mapped, ekran içinde ve başka pencereyle örtülmeyen alan gerektirir. Böyle direct read custom widgetdraw allocation preconditionını çağırmaz. Installed libX11/libgdk/libgdk_pixbuf ve gerekli C symbols salt okunur kontrolle mevcuttur; python `gi` namespace görünse de `require_version` bulunmadığından ona dayanılmadı.

Alternatif uygulama: yalnız root’un kendi immutable candidate PID’sini `_NET_WM_PID` ile doğrulayarak X11 foreignwindow üzerinden framebuffer read/screenshot almak. Capturesız smoke instance kısa süreli olduğundan döndüğü playlist profili visual kanıtla teyit edilir. Daha uzun sabit pencere için RadioS/build altında credentials içermeyen özel fixture proje/asset/config ile normal candidate açılır, gerçek production client bağlanmaz ve yalnız owned test PID kapatılır. Existing PID1741’e input/yönlendirme/close gönderilmez. QA ajanı bu alternatifleri execute etmedi; root normal viewport kanıtı beklenir. Üretim source/test/default mutasyonu yoktur.


## Capturesız actual native smoke ve boş framebuffer sonucu

Kayıt: 2026-10-10 23:28:04 UTC. [theme-framebuffer-validation-20261010.json](theme-framebuffer-validation-20261010.json) root actual komutu `--profile playlist --smoke-test`, **exit0 / 2,552 sn**; [log](theme-framebuffer-20261010.log) gerçek mapped window/8 profil/iş teslimi native kabulünü bildirir. Candidate SHA `a671b0054c5c7cba9cd0897aa39386d5c7c4a9225ee02dab7bfcc68b6260e98e`; drift[] ve default_binary_written=false/existing_application_controlled=false sınırları korunur.

**Native smoke PASS**, fakat dış `_NET_WM_PID` filtreli framebuffer araması candidate PID8340 için pencere bulmadı: XID null, frames0, screenshot null. Birleşik manifest `passed=false` doğrudur; normal viewport görüntüsü bu koşudan kabul edilmez. Önceki custom draw FAIL silinmez veya yeni PASS’a çevrilmez. Salt okunur alternatif incelemede WM yeniden ebeveynlemesinden dolayı root çocuğu yerine recursive child tree / `_NET_CLIENT_LIST` ve format32 Xlib unsignedlong strides kontrolü root’a önerildi; bu olasılıklar henüz kanıtlanmış hata kökü değildir. Unique private app title/proje/Popen ownership yöntemi root tarafından ayrıca araştırılır. QA agentı canlı app input veya candidate process kontrolü yapmadı.


## Son private viewport sınırı ve kurulu kaynak doğrulaması

Kayıt: 2026-10-10 23:34:37 UTC. Root açık delegasyonuyla kendi normal private candidate süreci oluşturuldu: immutable a671, benzersiz private title/yeni XID, no credentials/UI preferences, copied assets/source/exampleconfig, canonical source mutex altında yalnız private fixture ve QA artefaktı yazıldı. Existing üretim app/source/default dosyalarına yazma veya tıklama yapılmadı.

İlk actual private capture **23:30:08.049397→23:30:10.961742UTC**, own PID12669/XID0x1800007, **1920×1042**, 0 log/exit0/own cleanup, 7 watched input before/after eşit. [PNG](screenshots/theme-20261010/native-playlist-private-window.png) SHA **2fca196fc56199107f5bbf9f7f5f95912b52fd4e613c29808f8412a980886ad3**; actual `view_image` görünümünde nav Playlist seçili olsa da **body Yayın** idi. Bu yüzden [receipt](theme-private-window-validation-20261010.json) teknik capture başarılı/visual_profile_matches_requested=false/**passed=false** olarak düzeltildi; ilk teknik-only true metadata playlist görsel kabulü sayılmaz. Geçerli son receipt SHA **0d8d5b2b078c2537903049aa55dfb9e26d63318d1b3aac0c68db6b136f3512fe**.

Salt okunur olası neden: RadioS başlangıçta `select_profile` işlemini `gtk_widget_show_all` öncesi çağırır. [Resmî GtkStack kaynağı](https://raw.githubusercontent.com/GNOME/gtk/gtk-3-24/gtk/gtkstack.c) hidden child için visible-child seçimini uygulamaz. Bu kaynak/görsel eşleşmesi olası startup bug açıklamasıdır; production fix veya kapsamlı A/B kabulü değildir. Styling işi `window.c`/`main.c` davranışını değiştirmedi.

Root ikinci dar yetkiyle yalnız own private mapped pencereye inputla Playlist seçimine izin verdi. XSendEvent hazırlığı **23:31:36.570643→23:31:42.741545UTC**, own PID15903; input child window bulunmadı, event gönderilmedi/**FAIL**; [receipt](theme-private-window-selected-validation-20261010.json) SHA **b98340feba27ed96ddcf68989a20f635fd46e22ac61335c5f813824dd1e45229**. Own child temizlendi ve source drift[] kaldı.

Son own XTest fallback **23:32:20.038399→23:33:04.471100UTC**, unique-title/new-XID0x1c00007/own PID18859/candidate SHA doğrulandı. Own raise/focus/motion ardından XQueryPointer own window/frame göstermedi; **guard click göndermeden durdu**, receipt **FAIL**. [Receipt](theme-private-window-xtest-validation-20261010.json) SHA **4354b989d4e6c95143a0a29d8e6b9dea6e2f5efc54b78c95a1f3bd02e746e87f**; 7 watched input drift[], own PID18859 sonrasında `/proc` yok. Guard gevşetilmedi; yeni recovery araçları kurulmadı. Bu üç private deneme normal playlist viewport görsel kabulü değildir. Long privatefixture gerçek playlist+marker PNG kabulü ve 7 native test ayrı kesin kanıtlardır.

Root bildirdiği P2 kurulum makbuzu ayrıca bağımsız okundu: [install-receipt.json](../../../build/direct-provider-delivery-20261010/install-receipt.json) install_passed=true/source_drift[]/431 source inputs checked. Kurulu `build/radios` bytehash bağımsız tekrar **5cec467b6a539af8c32f8ac9ae67a82e89cc10035262341db759e244ca666ea3**, receipt ile eşit. Makbuz SHA **a9a9e0b69aa3b95b662b8a10a2fc3e10ffdd6b2c93fda182448d8d3f93e2782c**; acceptance receipt SHA referansı gerçek dosyayla eşit. İlgili acceptance before/after map hem **src/ui/playlist.c 075558333e7f0aba47f94d7cf83e46588ccd355b057bc195b9ff4bfd7b733f03** hem **design/reference/tasarrım/playlist-workspace.css ec8b5b534f2f4fa93d30e03edca9847de8abb760df3a4d04e7f5c9aa2ce84c62** içerir; güncel kaynakla eşit. Bu tema hunks'ının güncel normal binary'ye dahil edildiği kurulum kabulüdür.

P2 launch receipt reported launch_passed=true/window_visible=true/window0x600aec, UTC23:32:45.539831,1767×1040; ancak root sonraki actual host process kontrolünde görünen prosesin `build/prompt7-open-20261010-2332/radios` olduğunu, kurulu default5cec olmadığını bildirdi. **Normal default açıldı doğrulaması bu QA raporunda yapılmaz.** Root bağımsız executable/tema inclusion denetimi ayrı sürer; kurulum kabulü ile normal canlı process identity karıştırılmaz.

Son kullanıcı talimatı doğrultusunda ortak `AGENT.md` yazımı durduruldu. QA bu dosyaya hiç yazmadı; son root talimatı sonrası koordinatöre shared kayıt güncellemesi talep edilmez. Genel recovery/shared log poll ve ek capture/test tekrarı yapılmadı. QA ajanının bu yeni işi kapsamındaki son yazımları yalnız kendi review belgesi ve yetkili özel build/review/screenshot artefaktlarıdır; source/test/default mutasyonu yoktur.
