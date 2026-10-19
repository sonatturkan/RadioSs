# Player paneli yerleşimi — kaynak teşhisi

Sorumlu `/root/pdf_audit`; UTC 2026-10-10 23:27. Kullanıcı açıklaması: **“Player panelinin yerleşimi bozuluyor.”** Bu görevde kaynak, test veya varsayılan binary değiştirilmedi. Tek yazılan dosya bu denetim kaydıdır. Parent root üretim düzeltmesinin sahibidir; koordinatör ortak AGENT kaydını yazar.

İlk “grafik patlıyor” ifadesi için waveform matematik/async yolları salt okunur incelendi. Kullanıcının yerleşim açıklaması sonrası çalışma kart geometrisine daraltıldı; waveform veya analyzer değişikliği önerilmiyor.

## 2026-10-10 23:32:38 UTC — bağımsız son kabul

**Kaynak düzeltmesi, hedef native regresyon ve görsel kabul tamamlandı.** `build/player-graphic/corrected-result.json`, `corrected-tests.xml`, `corrected-tests.log` ve gerçek `build/player-graphic/validation-receipt.json` bu ajan tarafından bağımsız okundu. İzole strict build exit 0; **3 CTest hedefi / 3 ayrı gerçek native vaka PASS, 0 failure, 0 skip, paralel toplam 20.39 s**. Her hedefin TAP planı 1..1'dir; hedef sayısı ve vaka sayısı burada ayrı doğrulanmıştır.

| Native vaka | Doğrulanan ilgili davranış | CTest süresi |
| --- | --- | ---: |
| `/native-playout/player-waveform-cues-square-automatic-analysis` | Boş/normal/gerçek multiline import + metadata save sonrası kart 420×420; single-line label; tüm mapped kontroller kart içinde; tam tooltipler; gerçek waveform/cue | 6.75273 s |
| `/native-playout/player-SYNC-unknown-and-late-target-preserve-PCM` | Geç hedef/analiz yanıtı yeni medya yüklemesini bozmaz, PCM koruması | 4.0852 s |
| `/native-playout/player-SYNC-analysis-persistence-chosen-pair-PCM` | Seçilen çift, analiz/persistence ve SYNC/AutoDJ PCM davranışı korunur | 20.3832 s |

Raw geometriler: ordinary ve multiline kart **420×420**; multiline title **18 px / 1 satır**, metadata **21 px / 1 satır**, waveform **76 px**. Üç ilgili source (`broadcast.c`, `player_waveform.c`, `test_playout_ui.c`) hashleri pre-build, post-build, pre-test ve post-test'te aynıdır; `source_drift=[]`. Test binary pre/post SHA256 **`d6fec6a4f911ca5fe0255eec1400659dc1352722213023d7e89a4c00865b8f00`** aynıdır. Bu kabul bütün ortak kaynak ağacının hashfence'i veya bütün proje testleri başarılı iddiası değildir.

Son `corrected-shots/player-layout-multiline.png` orijinal boyutta bağımsız `view_image` ile incelendi: **iki sütun korunur**, ilk kartın başlığı/üst satırı ve alt kontrolleri görünür; uzun/newline başlık-bilgi bir satırda, ellipsis ile sınırlıdır; waveform ve alt seek/volume/SYNC/AutoDJ kontrolleri kart dışına taşmaz. PNG SHA256 **`36f1b2e6c092f9aa8d467210ecd3cf9ad1ff629e6c72752050e858cb5ce9393a`**. Root'un bağımsız görsel kabulüyle tutarlıdır. İlk baseline 420→554 FAIL kanıtı aşağıda korunur; yeni kaynak sonucu o hatayı giderir.

**Varsayılan uygulamaya teslim sınırı:** player-graphic ekibi `build/radios` dosyasını değiştirmedi/uygulamayı yeniden açmadı. Güncel default install/reopen sahibi P2 root'tur; source inclusion ve gerçek installed/launch makbuzu onun seri görevidir ve bu denetim tarafından tamamlandı sayılmaz. Sonraki provider-routing alanı yeni runtime queue/binding hunks'ına sahiptir; onun ilerideki delta/test kabulü bu dar panel kabulüne eklenmez. Kaynak/test yazımı bu ajan tarafından yapılmadı; bu ajanın son işi own audit belgesi ve root/coordinator'a kanıt iletmektir.

## 2026-10-10 23:30 UTC — gerçek uygulama baseline ve dar düzeltme incelemesi

**Panel bozulması gerçek mevcut tema ve veri akışıyla tekrar üretildi.** Test ajanının raw `build/player-graphic/baseline-runtime.log` ve `.json` kayıtları bu ajan tarafından bağımsız okundu. Ordinary loaded kart **420×420**, title **18 px/1 satır**, metadata **17 px/1 satır** iken, gerçek multiline media import + DB metadata save ardından kart **554×554**, title **108 px/6 satır**, metadata **77 px/5 satır** oldu. Waveform her ikisinde **76 px** kaldı. Yeni anlamlı stable-width assertion `(554 == 420)` ile gerçekten başarısızdır; wrapper exit **250**, 23:28:46.997174–23:28:52.581250 UTC. Baseline binary SHA256 `788d7e27c4d812e39769cf87ad86b52bd2020589ed14d4606b73518b0efdce67`.

`baseline-shots/player-layout-ordinary.png` ve `player-layout-multiline.png` orijinal boyutta bağımsız `view_image` ile karşılaştırıldı. İlkinde sağ panelde iki kart sütunu vardır; multiline sonrasında genişleyen kart yüzünden tek geniş sütun kalır ve metin alanı kontrolleri aşağı iter. Bu, panel düzeninin bozulmasını doğrular; waveform genliğinin taşması değildir. PNG SHA256: ordinary `16a29681141eb9b3fcddbe7ff800e5bfcebb53e1bd7fe31282dc878a5bfae81f`, multiline `32d5bf21ee0cebc97c2cc90a16d1e66581e103e61d18b6f0b4d3021085e6d36e`.

Root'un gerçek `docs/player-graphic/source-change.json` ve `backups/20261010T232924Z/broadcast.c.patch` delta'sı bağımsız okundu; canlı source SHA256 `0cc3dcd23e45aca31a741d013fbf45b84b91772426612957ea6ae31d6bdb63f7` makbuzla eşittir. 23:29:24.219089 UTC dar değişiklik yalnız metadata satırının boşken de görünür kalması, title/metadata/next için single-line mode ve max-width-chars=32'dir. Özgün tam metinler/tooltipler, P7 yükleme/async korumaları, P8 kare boyutu ve FlowBox/waveform/SYNC düzeni, P2 provider hunks korunmuştur. Kaynak düzeltme review **uygun**; yeni binary/native regression ve son görsel kabulü henüz bu checkpoint için **bekliyor**. Bu ajan üretim patch'ini yazmadı.

## Somut bulgular

1. `src/ui/broadcast.c::player_metadata_render` 3655–3656 civarı, metadata etiketini yalnız bilgi doluysa görünür yapıyor. `player_add` 4228 civarı aynı etiketi `no_show_all=TRUE` ile başlangıçta gizli oluşturuyor. Şarkı yüklenmesi sanatçı/albüm/kaynak bilgisini getirince aynı kartın dikey doğal boyutuna yeni bir metin satırı ve `lp-current` kutusunun 3 px aralığı ekleniyor. Bu görünürlük değişimi kaynakta kesin; bütün gerçek kartın büyüme/taşma miktarı native acceptance tarafından ölçülmelidir.
2. Aynı `player_add` 4222/4227/4231 civarı title, metadata ve sıradaki etiketleri için `line_wrap=FALSE` ve `PANGO_ELLIPSIZE_END` ayarlıyor; **single-line mode ve natural-width sınırı yok**. Wrap kapatmak, metindeki açık yeni satırları yok etmez. Ellipsize, etiketin doğal genişliğini tek başına sınırlamaz.
3. 4335–4338 civarı kart `GtkAspectFrame` 1:1, `obey_child=FALSE`, minimum 420×420 içine alınır. 4374 civarı FlowBox homogeneous olduğundan tek kartın boyut isteği diğer kartların düzenini de etkileyebilir. AspectFrame yalnız kare görünmesi için yeterli değildir: çocukların gerçek minimum boyutu ve görünür tüm kontrollerin kart içinde kalması ayrıca doğrulanmalıdır. Genişliğin/yüksekliğin uygulamada ne kadar değiştiği henüz bu ajanın yaptığı tam kart ölçümü değildir.
4. Gerçek tema `assets/themes/radios.css:246` kaynak düğmelerine 26 px minimum yükseklik ve padding verir; `:255–256` metadata/sıradaki fontları 12 px yapar. `src/ui/window.c:422–458` gerçek pencerede derlenmiş reference CSS parçalarını, bu adaptation CSS'ini ve Inter fontunu yükler. Root'un card içi compact CSS'i bazı yükseklikleri ezer; bu nedenle çıplak GTK boyutu tam uygulama kartının piksel ölçüsü olarak sunulmaz.

## Gerçek GTK boyutlandırma probu

Kaynak dosyası üretmeyen Python `ctypes` çağrılarıyla sistem GTK3 üzerinde `gtk_init_check` başarılı oldu. `GtkLabel` wrap FALSE ve ellipsize END, tıpkı karttaki üç label'ın mevcut ayarlarıyla, show edildi; `gtk_widget_get_preferred_width/height` doğrudan okundu. Prob gerçek bir GtkLabel davranış kanıtıdır; bütün uygulama kartını veya gerçek şarkıyı yükleme kabulü değildir.

| Metin/ayar | Minimum genişlik | Doğal genişlik | Minimum/doğal yükseklik |
| --- | ---: | ---: | ---: |
| Boş metin | 0 | 0 | 19 |
| `Artist · Album · Medya arşivi` | 13 | 196 | 19 |
| `Artist\nAlbum\nMedya arşivi` | 13 | 89 | 57 |
| `Song title ` ×160 | 13 | 11360 | 19 |
| Aynı üç satır metin, single-line TRUE | 13 | 206 | 21 |

**Doğrulanan mekanizmalar:** Açık yeni satır mevcut ayarlarda yüksekliği üç katına çıkarır; ellipsize'a rağmen uzun metin doğal genişliği 11.360 px yapabilir. Metadata gizli→görünür değişimi kartın ihtiyaç duyduğu satır sayısını artırır. Kullanıcının gördüğü bütün kart arızasının tek nedeni olduğu iddia edilmez; root/test ajanı gerçek mevcut tema ve yükleme fixture'ıyla before/after ölçümü yapar.

## Önerilen dar düzeltme

- Yalnız `broadcast.c` player label/layout hunks: title/metadata/next etiketlerini `gtk_label_set_single_line_mode(TRUE)` ile sabit bir satıra al; ellipsize'ı koru ve doğal genişliği örneğin `max_width_chars=32` ile sınırla. Orijinal tam metin/tooltip ve erişilebilir metin korunabilir; medya metadata'sını değiştirmek gerekmez.
- Metadata etiketinin boşken de bir satırını rezerve et: oluşturmadaki `no_show_all=TRUE` ve render'daki koşullu görünürlük akışını kaldır veya eşdeğer sabit satır görünürlüğü uygula. Single-line modu boş/dolu içerikte aynı font satır yüksekliğini sağlar.
- P8 420×420 kare kart, 76 px waveform, BPM/SYNC komşuluğu, AutoDJ ve alt kontrol işlevleri; P7 load_serial/queue identity/lookup guards; P2 official provider alanları korunur. Waveform verisini veya asenkron seri guardlarını değiştiren patch bu teşhisten çıkmaz.
- Kaynak sahibi root, en güncel preimage üzerinden canonical `source-edit.lock` → `src/ui/broadcast.c.lock` içinde minimal hunk uygular. Bu ajan dosya sahipliği almadı ve kaynak yazmadı.

## Doğrulama gereği ve mevcut kapsam

`tests/test_playout_ui.c:101–104` gerçek `radios_window_new` kullanır; P8 acceptance tamamen stylesiz widget harness'i değildir. `player_square_and_controls` (1176+) gerçek kare oranını, her görünür satırın kart içinde kalmasını, waveform 60–100 px ve BPM/SYNC aralığını kontrol eder. `test_player_waveform_cues_square_automatic_analysis` boş/yüklü durumda bu yardımcısını çağırır ve 1440×1000/1100×800 boyutlarını dener. Bu önceki kabul, yeni kullanıcı arızası yeniden üretildi veya düzeltildi anlamına gelmez.

Yeni anlamlı acceptance: aynı pencerede boş→normal şarkı→uzun/yeni satırlı title/artist/album→sonraki şarkı→stop/clear; card/metadata/waveform allocation ve FlowBox preferred ölçülerini önce/sonra karşılaştır. Başka kartların boyutları/sütunları yüklenen metinden etkilenmemeli; tüm kontroller görünür kart içinde kalmalı; mevcut transport/queue ve gerçek waveform/cue kabulü korunmalıdır. Full production fixture ve görsel sonucu parent/test ajanında bu checkpoint için **bekliyor**.

## İnceleme anı kaynak kimliği

- `src/ui/broadcast.c`: `4473ab78097b3bd7fcd971d4b300d19a42b684cd40402af5b177e11f22d67a6c`
- `src/ui/player_waveform.c`: `51a375013fe468ea9dcec15ff58a44106c354c6588d805fe35500dd118e0bf1c`
- `src/ui/player_loader.c`: `e097e570e408784edde6b52f7150a41a856dd52c54e2809ac97e6c93b92b14d3`
- `include/radios/player_waveform.h`: `2609626324010aa82e8f6148209391357659e6ee36e7ad1425e78245815b8f65`
- `assets/themes/radios.css`: `5175c511f110fe68641325fa1edc5e402572aed2ff4b1e1c0e520744eb03f904`

İnceleme sırasında eski geniş aramada build snapshot dosyaları listelendi; o kopyalar güncel kaynak kanıtı olarak kullanılmadı. Varsayılan Python `gi` paketi PyGObject API'si sağlamadığından (`require_version` yok) prob ctypes ile yapıldı. Yanlış cwd'de bir analyzer rg çağrısı bulunamadı; doğru RadioS cwd'de yeniden salt okunur tarandı, kaynak/test etkisi yoktur. PDF9 geçmiş kabulü ve kayıtları yeniden uygulanmadı veya değiştirilmedi.
