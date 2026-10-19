# Prompt 14 kaynak ve arayüz kabul denetimi

Son güncelleme: 2026-10-09 02:10:52 UTC. Sorumlu: PROMPT14 `/root/release_audit14`.

**Nihai kapsam kabulü: PASS.** 02:07:23–02:07:37 UTC'de güncel kaynak strict Debug C11 uygulama ve iki test hedefi derlendi; native-online 7/7 ve native YouTube Music 5/5, toplam 12/12 GLib kontrolü geçti. Üç sağlayıcı ekran görüntüsü ayrıca bağımsız görüldü. Canlı hesap yetkilendirmesi, bütün proje CTest kümesi veya uygulama CSS'inin bu harness görüntülerinde görsel eşliği test edilmiş olarak sunulmaz.

## Kaynak ve kapsam

[14.pdf](../../../prompt/14/14.pdf) bir sayfadır. Bütün metni pypdf ile okundu: Apple Music, Spotify ve benzeri sağlayıcı sekmelerinin içeriğinde yalnız **Giriş yap** bulunması, diğer kısımların kaldırılması istenir. [figures/sema_1.png](../../../prompt/14/figures/sema_1.png) (808×631) ayrıca görsel olarak incelendi. Üç sekme Spotify, Apple Music ve YouTube Music'tir; seçili YouTube Music alanında hesap adı, tarayıcı seçimi, bağlantı/ayırma/playlist düğmeleri, açıklamalar, gelişmiş seçenekler ve playlist alanı görünür. Bunlar sadeleştirilecek sağlayıcı panelinin önceki durumudur.

Bu dosyanın önce atanmış `release-audit.md` adı, kaynak PDF'de bir paketleme veya yayın kabulü talebi bulunduğu anlamına gelmez. `tools/radios_package.py` ile eski sürüm14 kabul betikleri bu PDF'nin gereksinimi değildir; bu oturumda değiştirilmedi veya çalıştırılmadı. Eski aşama numaraları mevcut PDF14 talebinin tamamlandığının kanıtı değildir.

Kaynak SHA-256:

| Kaynak | SHA-256 |
| --- | --- |
| `prompt/14/14.pdf` | `cf1781e03a7e4e55027ac31c5115a9e9300766383909ce8228216698906270e9` |
| `prompt/14/figures/sema_1.png` | `823122cf9b39c8d398e51db00a1c7a18f4bc2209bb8bfc28fc5299e97fb820f9` |

Tam sayfa render ve PDF gömülü görsel karşılaştırması pdf14 ajanının `requirements.md` ve `reference/` kanıtlarında kayıtlıdır. Bu denetçinin kendi PDF görsel incelemesi figures görüntüsüdür; PDF render üretimi ayrıca pdf14 tarafından tamamlanmıştır.

## Mevcut kaynakla eşleme

PDF'nin sekme yüzeyi `radios_online_services_ui_open()` sağlayıcı popup'ıdır. Root `src/ui/online_services.c` dosyasının yalnız bu popup bölgesine sade `login_page_new()` ve `login_open()` ekledi. Üç panelin her birinde tam etiketli tek Giriş yap düğmesi oluşturulur. Profil, DB belge ve playlist kurucuları popup tarafından çağrılmaz. Üç resmi adres `https://open.spotify.com/`, `https://music.apple.com/` ve `https://music.youtube.com/` kullanılır. Açma başarısızlığında yalnız geçici hata diyaloğu görünür.

Ayrı `radios_online_services_ui_new()` çalışma alanı `src/ui/technical.c` tarafından kullanılır ve mevcut veri/playlist işlerini içerir. Bu ayrı çalışma alanı veya backend PDF14 popup talebi kapsamında kaldırılmadı; regresyonları korunur.

Diğer canlı prompt1 oturumu `header.c` eski Ayarlar popover'ını kaldırdığı için önceki `header-online-music` erişimi güncel kabul yolu sayılmaz. Güncel `src/ui/broadcast_online.c` `connect_clicked()` halen public popup API'sini çağırır. Test public API üzerinden açılma/tek pencere/kapanma/yeniden açılma davranışını doğrular; diğer oturumun header tasarımı geri alınmaz.

## Test değişiklikleri ve sınırlar

Yalnız `tests/test_online_services_ui.c` değiştirildi. Başlangıç dosyası `build-prompt-14/baseline/test_online_services_ui.c` altında saklandı; SHA-256 `2cb300f33c779b0edfd871b7206b38a2109f1edb7e2bf29eefd1b87fa8d1da28`. İlk altı test 01:57 UTC'de root'a devredilip yürütüldü. Root ikinci yazma devrini verdiğinde, başarılı altı testin dosyası `build-prompt-14/baseline/test_online_services_ui.six-tests.c` olarak saklandı; SHA-256 `8f19ab6ad141ea358ec2285ea66be27a071ad054eb2e0686cf0d5e61d8f22ed6`. Yedinci test ve notebook çevresinde ek içerik olmaması kontrolü 02:02 UTC'de eklendi; ilk yedi-test dosyasının SHA-256 değeri `1ed005b7983f5f894964c9a33d38f7ab8cf18da490109dcaa6b9f84e6e67588f`. GTK'nın boş internal action-area kabuğu ayrımı düzeltildikten sonra güncel dosya SHA-256 `60f25fc47ce47a19961738c2ed46be9ad564f86d895e529c91742db6e2252726`. Dosya yeniden root'a immutable yürütme için devredildi; test sırasında bu ajan değiştirmez.

Yedi GLib kabul/regresyonu:

| Test | Doğruladığı davranış |
| --- | --- |
| `/native-online/persistence-cas-reference-validation` | Mevcut gerçek DB kayıtları, CAS çakışması, girdi koruması ve başvuru doğrulaması; önceki test korunur. |
| `/native-online/fresh-permission-cancel-owner-close` | Mevcut yetki, iptal ve pencere kapanışındaki bekleyen işlem; önceki test korunur. |
| `/native-online/basic-login-without-database-or-developer-keys` | DB/config/developer key olmadan popup; üç doğru sekme; her panelde tek Giriş yap düğmesi ve yalnız bu düğmenin label'ı; başarılı resmi URI açılışında ek metin oluşmaması; hata/yeniden açılma/ana pencere kapanışı. |
| `/native-online/login-dialog-singleton-reopen` | DB istemcisi bulunan fakat bağlanmamış bağlamda sade popup; tekrar açılışta aynı pencere; kapanışta parent kaydının temizlenmesi; yeniden açılış ve parent kapanışı. |
| `/native-online/broadcast-connect-button-opens-login-only-popup` | Gerçek `radios_broadcast_online_new()` tarafından üretilmiş Hesabı bağla düğmesinin callback'i popup'ı açar; tekrar tıklama singleton, response close/yeniden açma ve parent kapanışı çalışır. Sağlayıcı kaynağının I/O activation'ı veya ana uygulama kaynak seçimi başlatılmaz. |
| `/native-online/workspace-three-provider-lists-reopen` | Eski üç sağlayıcı playlist oluşturma/yeniden yükleme/oynatıcıya başvuru işlevi ayrı çalışma alanı üzerinden korunur. |
| `/native-online/workspace-youtube-reference-validation` | Eski YouTube yanlış alan adı reddi ve doğru resmi video URL kabulü ayrı çalışma alanına taşınarak korunur. |

URI linker wrapper'ı kullanıcının tarayıcısını açmaz. Testler doğru URI ve parent pencereyi doğrular; gerçek sağlayıcı hesabı yetkilendirmesi veya harici oturum açılışı test edilmiş sayılmaz.

`RADIOS_TEST_LOGIN_CAPTURE_DIR` verilirse basic-login testi her sekmenin eşlenmiş GTK penceresini `capture-spotify.png`, `capture-apple_music.png`, `capture-youtube.png` olarak kaydeder. Bu harness uygulamanın CSS provider'ını yüklemez; görüntüler GTK içerik/yapı kanıtıdır, nihai uygulama tema eşleşmesi iddiası değildir.

`tests/run_online_services_ui.py` kopyalanmış backend/config ve geçici Türkçe yolda özel DB/store, geçici loopback portu kullanır. Üretim credentials, config veya kullanıcı verisi düzenlenmez. Runner'ın geçici dosyaları genel `build/` içinde benzersiz `native-online-ui-*` dizinine yazılır; genel CMake build veya kalıcı kullanıcı DB'si değiştirilmez.

## Doğrulama durumu

- **PASS:** Değişen test dosyasına izole CMake flags ile C11 `-Wall -Wextra -Wpedantic -Werror -fsyntax-only` kontrolü, ilk değişiklik 2026-10-09 01:57 UTC ve yedinci test eklemesi 02:02 UTC.
- **PASS — root yürütmesi:** İzole incremental native-online altı testin tamamı geçti. Ayrı native YouTube Music beş regresyonun tamamı geçti. Sonuçlar `prompt/agent/prompt14/logs/events.jsonl` 01:59:40 UTC olayında kayıtlıdır.
- **PASS — bağımsız görsel inceleme:** Bu denetçi 02:00 UTC'de üç PNG'nin tamamını `view_image` ile gördü. Her seçili sağlayıcıda yalnız tam etiketli Giriş yap düğmesi, üç doğru sekme ve pencere dekorasyonu vardır; önceki hesap/listeler/açıklamalar/ek eylemler yoktur. Kanıtlar [Spotify](reference/login/capture-spotify.png), [Apple Music](reference/login/capture-apple_music.png), [YouTube Music](reference/login/capture-youtube.png). GTK tema sınırı yukarıda belirtildi.
- **Önceki başarısız kaynak build'leri:** Root ilk güncel tam kaynak build'inde başka oturumun `broadcast.c` queue API çağrıları için eksik header deklarasyonlarını; ikinci denemede yeni CMake Preferences dosyalarının eksik oluşunu bildirdi. PDF14 bu kaynakları değiştirmedi. Diğer sahipler eksik dosya/API'leri tamamladıktan sonra build yeniden çalıştırıldı; geçmiş başarısızlıklar silinmedi.
- **PASS — güncel kaynak build'i:** 2026-10-09 02:02:07–02:02:12 UTC'de `cmake --build build-prompt-14 --target radios test_online_services_ui test_youtube_music_ui --parallel 2` exit 0 ile tamamlandı. `radios`, değişen yedi-test native-online ikilisi ve native YouTube Music ikilisi güncel strict Debug C11 kaynaklarıyla linklendi. Kanıtlar `build-prompt-14/evidence/current-build.log` ve `current-build-result.json`; denetçi ikisini salt okunur gördü.
- **FAILED — ilk güncel kaynak native koşusu:** 02:02:39–02:02:47 UTC'de CTest iki hedefin birini geçti, exit 8. Native YouTube Music geçti; native-online ilk iki regresyon geçti, basic-login üçüncü testte `assert_login_tabs:320` content-area çocuk sayısını 1 beklediği halde GTK'nın 2 çocuk döndürmesiyle durdu. Bu GTK'nın boş action-area kabuğunu sayan test varsayımıdır; page/notebook içeriğinin değiştiği henüz gösterilmiş değildir. Başarısız test root'a bildirildi, boş dialog kabuğunu içerikten ayıran düzeltme önerildi. Kalıcı ilk başarısızlık kanıtları `build-prompt-14/evidence/before-shell-fix-current-native-tests.log`, `before-shell-fix-current-native-result.json`, `before-shell-fix-native-online.xml` olarak korunmuştur.
- **Düzeltildi ve yeniden koşuda PASS:** Root yazma sahipliğini yeniden verdikten sonra GTK boş dialog kabuğu sayımı düzeltildi. Content içindeki gerçek page tam bir kez bulunmalıdır; page yalnız tek notebook içermelidir. Diğer content-area kardeşlerinin bütün alt ağacı yalnız boş, focusable olmayan container'lardan oluşmalıdır; button/entry/combo/tree/text/expander/notebook ve container olmayan label/image gibi yapraklar yasaktır. Visibility üzerinden istisna verilmez; gizli artık içerik de reddedilir. Strict syntax tekrar geçti; nihai güncel kaynak koşusunda yedi testin tamamı geçti. Önceki başarısızlık kayıtları korunur.
- `test_broadcast_online_ui.c` dosyası başka canlı oturumun sahipliğindedir ve değiştirilmedi; yedinci test yalnız bu ajan sahipliğindeki `test_online_services_ui.c` dosyasındadır.
- **FAILED — düzeltme sonrası ilk rebuild, tarihçe:** 02:04:34–02:04:35 UTC'de yalnız native-online hedefi yeniden derlenirken başka canlı oturumun güncel `broadcast.c:660` `form_button` işlevi kullanılmıyor uyarısı strict `-Werror` nedeniyle build'i durdurdu, exit 2. Bu dosya PDF14 sahipliğinde değildir ve değiştirilmedi. Kanıtlar `build-prompt-14/evidence/final-online-build.log`, `final-online-build-result.json`. Sonraki güncel build'de dış dosya sahibi düzeltmeleriyle engel kalktı; PDF14 bu dış kaynağı düzenlemedi.
- **PASS — nihai güncel kaynak build:** [Yerel derleme sonucu](final-current-build-result.json) 02:07:23–02:07:29 UTC, exit 0; [derleme logu](final-current-build.log) uygulama ve iki native testin güncel kaynaklarla linklendiğini gösterir. Komut `cmake --build build-prompt-14 --target radios test_online_services_ui test_youtube_music_ui --parallel 2`.
- **PASS — nihai güncel kaynak native kabul:** [Yerel koşu sonucu](final-current-native-result.json) 02:07:29–02:07:37 UTC, exit 0; [koşu logu](final-current-native.log) 2/2 CTest hedefi, 7.54 saniye. [JUnit ve tam TAP](final-current-native.xml) içinde `tests=2`, `failures=0`, `skipped=0`; system-out TAP planları native-online `1..7`, YouTube Music `1..5` ve bütün `ok` satırları ayrı ayrı incelendi. Toplam 12/12 kontrol geçti. Komut `ctest --test-dir build-prompt-14 --output-on-failure --output-junit <evidence>/final-current-native.xml -R '^native_(online_services|youtube_music)$' --parallel 1`. Yerel beş kanıt kopyası `build-prompt-14/evidence/` orijinalleriyle SHA-256 olarak eşleştirildi; özet [validation.md](validation.md) ve [validation.json](validation.json) ile tutarlıdır.
- **PASS — güncel gerçek düğme callback entegrasyonu:** `/native-online/broadcast-connect-button-opens-login-only-popup` nihai TAP içinde `ok 5` olarak geçti. Gerçek üretim `radios_broadcast_online_new()` kurucusunun Hesabı bağla düğmesi sade popup'ı açtı; singleton/close/reopen/parent destruction ve ek tarayıcı açılmaması doğrulandı. Ana uygulamanın tam kaynak seçimi ve sağlayıcı I/O akışı veya canlı hesap girişinin doğrulandığı iddia edilmez.

PDF14'e atanmış kaynak/test/denetim işleri tamamlandı. Test dosyası SHA-256 `60f25fc47ce47a19961738c2ed46be9ad564f86d895e529c91742db6e2252726` nihai evidence incelemesinde yeniden doğrulandı. Kalan doğrulanmamış konular yalnız yukarıda açık belirtilen kapsam sınırlarıdır; başka canlı oturumların değişiklikleri için bu rapor gelecekteki build başarısını garanti etmez.
