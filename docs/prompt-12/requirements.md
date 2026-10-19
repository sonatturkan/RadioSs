# Prompt 12 — Kaynak gereksinimleri ve inceleme kanıtı

İnceleyen: `/root/requirements_12`. Tarih: 2026-10-09 UTC. Kapsam: yalnız `RadioS/`; kaynak `../prompt/12/`. Bu belge kaynak gereksinimlerinin envanterini ve ayrı tarihli somut kabul checkpoint'lerini içerir. Orijinal gereksinim tabloları uygulama başarısı iddiası değildir; güncel gerçek sonuç ve açık sınırlar son checkpoint'tedir.

## Güncel kabul checkpoint'i — 2026-10-09 02:58:43 UTC

Devam inceleyen: `/root/source_review_resume_12`. Strict Release, **7/7 playlist**, **7/7 native_data** ve varsayılan `build/radios` atomik kurulumu tamamlandı; installed SHA `2d3805e97cbdc563433479db9edffbe254208aa9d07a765a23094ac1ebcc69a4`. Release/current production girdileri aynıdır; yalnız sonraki authorized native_data test fixture delta'sı yeni ayrı gerçek7/7 koşuyla doğrulandı. Eski data6 FAIL/pending ve install pending kayıtları tarihsel olarak korunur, bu son kabul onları supersede eder.

Son PNG9320b803...64f86 Schema1/2/3 component kabulünü; 385 current source/install hash eşliği ve gerçek log/XML kabulü [`release/install inceleme kanıtında`](review/source-review-release-install-20261009T025812Z.json) doğrular. **QA03** dirty panel/source context kullanım tutarlılığı önceki koşullu kaydıyla korunur. **QA04** bağsız Apple/Spotify catalogue-only bağımsız otomatik tam ses ve canlı provider kabulü halen açıktır; gerçek verified-file analiz yolu uygulanmıştır. [`Official Apple capture feasibility`](review/official-apple-capture-feasibility.md) salt okunur source/protokol incelemesidir, yeni uygulama değildir. Aşağıdaki eski snapshot bulguları son tarihli checkpoint'lerle birlikte okunur; bütün PDF/repository tamamlandı iddiası yoktur.

| Güncel alan | Gerçek kabul / açık sınır |
|---|---|
| PDF ve üç figure | Eksiksiz envanter ve karşılaştırma;4/4 kaynak SHA değişmedi. Kaynak gereksinimleri aşağıda aynen korunur. |
| Playlist / drop / markers | Final focused5target39casePASS; son kolon ve strictRelease7/7playlistPASS; native_data ayrı7/7PASS. |
| Default uygulama | Atomik kurulmuş candidate==installed2d3805...c69a4,mode0755;385install/current source hash aynı. Source-only veya pending değildir. |
| Koşullu kapsam | QA03 önceki dirty/source context durumu korunur; QA04 bağsız Apple/Spotify otomatik fullaudio ve canlı provider kabulü açık. Verified-file gerçek analiz yolu vardır; PDF dosya önkoşulunu yazmaz. |

## Eksiksiz kaynak envanteri

| Kaynak | İçerik / boyut | İnceleme |
|---|---|---|
| `12.pdf` | 3.345.934 bayt; 2 sayfa; A4 595,2756 × 841,8898 pt | Her iki sayfanın tüm metni pypdf ile çıkarıldı; her sayfa PDFium ile 2× render edilerek bütünü görsel incelendi. |
| `figures/sema_1.png` | 2400 × 1792 px; 4.578.467 bayt | Tam görüntü incelendi; PDF sayfa 1 ile karşılaştırıldı. |
| `figures/sema_2.png` | 458 × 391 px; 46.454 bayt | Tam görüntü ve 2× inceleme büyütmesi incelendi; PDF sayfa 2 ile karşılaştırıldı. |
| `figures/sema_3.png` | 535 × 37 px; 22.429 bayt | Tam görüntü incelendi; PDF sayfa 2 ile karşılaştırıldı; Şema 1'in alt tablo başlığı/filtre şeridinin kırpımıdır. |

PDF içinde sayfa 1'de 2400 × 1792 px Şema 1, sayfa 2'de 458 × 391 px Şema 2 ve 535 × 37 px Şema 3 bulunur. Figures ile içerikleri aynıdır. Kaynak klasöründeki bütün dosyalar envanterlenmiştir; başka PDF yoktur. SHA-256 manifesti `review/source-manifest.json`, tüm sayfa metni `review/12-extracted.txt`, görsel kanıtlar `review/12-page-1.png`, `review/12-page-2.png` ve `review/sema_2-inspection.png` dosyalarındadır. Render için geçici pypdfium2/Pillow kurulumu yalnız RadioS içinde kullanılmış ve ardından silinmiştir; proje bağımlılıkları değiştirilmemiştir.

## Doğrudan yazılı davranış gereksinimleri

| Kimlik | Kaynak | Gereksinim | Kabul kontrolü |
|---|---|---|---|
| P12-01 | s.1, ilk paragraf | Programın üzerine bırakılan şarkı/dosya için doğrudan **“Playliste ekle”** kısmı açılmalı. | Ana uygulama penceresindeki bırakma, kullanıcıyı playlist hedef seçimine götürür; yalnız medya kütüphanesi import penceresi açılması yeterli değildir. |
| P12-02 | s.1, Şema 1 sonrası | Playlist sayfası Şema 1'deki tasarımda olmalı. | Üst playlist tablosu, aradaki kontrol şeridi ve alt şarkı tablosu görünür; görsel maddeler aşağıda. |
| P12-03 | s.1, son paragraf | Playlistte şarkıya tıklandığında şarkı analizi başlamalı. | Tıklama gerçek seçili şarkı bağlamıyla analiz akışını başlatır; yalnız satır seçimi yapılması yeterli değildir. |
| P12-04 | s.1 son paragraf; s.2 “En altında” | Analizle birlikte Şema 2'deki **Parça işaretleri** sayfası/paneli görünmeli; altta yer almalı. | Playlistin en altındaki işaret düzenleme alanı açılır; waveform ve gerçek doğrulanmış medya alanları seçilen şarkıya bağlıdır. |
| P12-05 | s.2, Şema 3 öncesi | Şema 3'ün gösterdiği ilgili yerde **mevcut playlist seçimi** bulunmalı. | Kayıtlı playlistler seçilebilir, seçim gerçek playlist kimliğine bağlıdır. |
| P12-06 | s.2, aynı paragraf | Seçilen playlistin şarkıları seçicinin altında gelmeli. | Playlist seçimi alt tabloyu yeniler; eski playlistin veya genel medya kütüphanesinin şarkıları kalmaz. |
| P12-07 | s.2, son paragraf | **Apple Music, YouTube Music, diğer bütün sağlayıcılar ve local** aynı playlist içinde karışık kullanılabilmeli. | Tek kayıtlı playlist aynı anda farklı provider kimlikleri ve local medya bağları taşır; sağlayıcı türüne göre ayrı liste zorunluluğu veya dışlama uygulanmaz. |

## Şema 1 — Playlist sayfası görsel envanteri

Şema yatay geniş iki tabloyu dikey yerleştirir. Üst tablo yaklaşık ekranın üst yarısında, alt tablo alt yarısındadır. İki tablonun arasındaki tek satırlık kontrol şeridi Şema 3'te ayrıca gösterilmiştir. Şema 1, masaüstü pencere kabuğu (başlık `Playlist Builder`, küçült/büyüt/kapat) ve `File`, `Edit`, `Import`, `Events` menülerini de gösterir; bunlar referans görselin görünür unsurlarıdır. Mevcut RadioS pencere/menü çerçevesine uygunluk ana entegrasyon ajanı tarafından kontrol edilmelidir; bu belge uygulama marka/kabuk kararını kaynak metin adına değiştirmez.

| Kimlik | Şema alanı | Görsel / veri gereksinimi | Kontrol |
|---|---|---|---|
| P12-V01 | Üst başlık | Koyu gri–siyah yatay başlık şeridi; solda büyük `New Rotation Playlist`, sağda `Playlist Builder`; açık mavi metin. | Playlist çalışma alanının belirgin başlığı ve sağ kimliği. |
| P12-V02 | Üst tablo | Başlıklar sırasıyla `#`, `TIME`, `RTS`, `TITLE`, `TITRO`, `DURATION`, `EOM`, `TRIGGER`, `VT`, ikinci `TRIGGER`, `CUE`; geniş sağ alan ve satır sonunda mavi bilgi simgesi. Görselde `TITRO` yazımı böyledir; otomatik olarak başka alana çevrilmemeli. | Kolon sırası ve marker/trigger bilgileri görünür. |
| P12-V03 | Üst tablo verileri | Sıra numarası; zaman; kısa sanatçı/tür bilgisi; kısa şarkı adı; intro; süre; EOM; `START`/`END` tetikleyiciler. Zamanlar HH:MM:SS biçiminde; ekran darsa metinler üç noktayla kısalır. | Gerçek playlist satır verilerinin kolonlara bağlanması. |
| P12-V04 | Satır görünümü | Beyaz ve açık turkuaz alternatif satırlar; seçili satır zeytin yeşili, açık renk yazı; sıra hücreleri gri, bazı sıra hücreleri yeşil. İnce gri grid çizgileri. | İki tabloda aynı zebra/seçim dilinin uygulanması. |
| P12-V05 | Kaydırma | Üst ve alt tabloların sağında kendi dikey scroll alanı; tablolar birbirinden bağımsız. | Çok şarkılı playlistte iki panel kullanılabilir. |
| P12-V06 | Aradaki şerit / Şema 3 | Açık gri kontrol şeridi; solda boş arama kutusu; sağda `Any Category`, `Any Subcategory`, `Any Genre` dropdownları. Yazılı P12-05 değişikliği bu şeride **mevcut playlist seçicisi** eklenmesini/yerleştirilmesini ister. | Playlist seçicisi ilgili şeritte, şarkılar hemen altında. Kaynak kategori filtrelerini kaldırmayı açıkça emretmez; nihai tasarım P12-05'i önceliklendirir. |
| P12-V07 | Alt tablo | `ARTIST`, `TITLE`, bir başlıksız geniş veri kolonu, `INTRO`, `DURATION`; sanatçı ve şarkı adı solda, intro ve süre sağda. | Seçilmiş kayıtlı playlistin şarkı detayları. Başlıksız kolondaki örnek içerik başlık/dosya adlarıdır; kaynak yeni semantik ad belirtmez. |

## Şema 2 — Parça işaretleri görsel ve işlevsel envanteri

Panel koyu neredeyse siyah arka planlıdır. Üstte `Parça işaretleri` başlığı, altında `Parça işaretleri · Doğrulanmış medya` bağlam satırı vardır. Sonraki küçük satırlar gerçek dosya/medya adı, süre, örnekleme hızı, SHA-256 ve kırpılan örnek bilgisini gösterir. Görsel örneği `Indila · Dernière Danse (Clip Officiel)`, 214.751 sn / 44100 Hz türündedir; bunlar **örnek şarkı verisidir**, yeni analiz sonuçlarına sabit sayı olarak yazılmamalıdır. Görselin sağında kaynak ekran görüntüsüne dahil olmuş robot/kalem overlay parçaları vardır; yazılı gereksinimde ürün kontrolü olarak belirtilmez ve işaret alanı değildir.

| Kimlik | Alan | Gereksinim / görülen örnek |
|---|---|---|
| P12-V08 | İki kolonlu alan düzeni | Sol ve sağ kolonlar hizalı; ince lacivert sınır ve koyu input arka planı; aktif input mavi vurgulu. |
| P12-V09 | Sol ilk satır | `Parça süresi · saniye`, örnek `214.854`. |
| P12-V10 | Sağ ilk satır | `Cue giriş`, örnek `36.100`. |
| P12-V11 | Sol ikinci satır | `Vokal / intro`, örnek placeholder `İsteğe bağlı`. |
| P12-V12 | Sağ ikinci satır | `Cue çıkış`, örnek `214.854`. |
| P12-V13 | Sol üçüncü satır | `Fade başlangıcı`, örnek `214.854`. |
| P12-V14 | Sağ üçüncü satır | `Fade bitişi`, örnek `214.854`. |
| P12-V15 | Sol dördüncü satır | `Otomatik geçiş noktası`, örnek `214.854`. |
| P12-V16 | Sağ dördüncü satır | `Sonraki parçayla bindirme`, örnek `0.000`. |
| P12-V17 | Checkbox | Alanların altında `Otomatik geçiş taslağı` checkbox'ı. Küçük bitmapin büyütmesi ve mevcut kaynakla karşılaştırmasıyla yazım netleştirilmiştir. |
| P12-V18 | Marker düğmeleri | Tek satırda `Cue giriş`, `Vokal / intro`, `Cue çıkış`, `Fade başlangıcı`, `Fade bitişi`, `Otomatik geçiş noktası`. |
| P12-V19 | Konum / zoom | Düğmelerin altında `Konum 0.0 sn · 1.0x` biçimindeki küçük konum/zoom bilgisi. İlk incelemedeki `Kazanç` okuması küçük bitmapten kaynaklanan transkripsiyon hatasıydı; büyütme ve mevcut `editors.c` `Konum %.1f sn · %.1f×` satırıyla karşılaştırılarak düzeltildi. Görsel bir kazanç kontrolü istemez. |
| P12-V20 | Waveform | Alt kısımda turkuaz gerçek dalga şekli; sol başlama konumu beyaz; zaman bölüntüleri ince gri; `IN` yeşil, `OUT` kırmızı, fade işaretleri yeşil. |
| P12-V21 | Son işlem düğmeleri | Panelin en altında sağda `Vazgeç / Kapat` ve `Medya işaretlerini kaydet`. |
| P12-V22 | Gerçek bağ | Analiz ve waveform seçilen gerçek doğrulanmış medyaya ait olmalı; cue/fade/intro/auto geçiş/overlap alanları düzenlenip kalıcı kaydedilmeli. Tasarım kaynakta mevcut marker arayüzünü gösterir; işaret ve veri davranışları mevcut teknik API ile korunmalı. |

## Ayrılabilir iş paketleri ve test karşılıkları

1. **Playlist çalışma alanı:** `src/ui/playlist.c`: iki tablo, ilgili şeritte kayıtlı playlist seçicisi, seçimin alt tabloya bağlanması, Şema 1 renkleri/kolonları, seçili satır analizi ve en alt marker alanı. P12-02/03/04/05/06, V01–V22.
2. **Drop → Playliste ekle:** `src/ui/media.c`, `include/radios/media_ui.h`: global dosya bırakma anında hedef playlist seçimi; yönetilen import/doğrulama sonucunu hedef listeye kalıcı binding ile ekleme. P12-01. Dosya türü, sayı sınırı, bozuk URI, meşgul işlem ve kapatma güvenliği mevcut native_media_drop testleriyle korunmalı.
3. **Karışık sağlayıcı backend:** mevcut playlist persistence/`entry-save` API'si: Apple Music, YouTube Music, diğer provider kimlikleri ve local media tek playlistte; provider ID ve gerçek playback reference korunur. P12-07. `entry-save-many` endpoint'i mevcut değildir. Drop uygulaması her doğrulanmış dosyayı gerçek `entry-save` ile seri kaydeder; her onaylı yanıttaki `_rev` sonraki CAS isteğinin revision'ı olur.
4. **Doğrulama:** drop sonrası hedef seçici; import tamamlanınca gerçek binding ve liste yenileme; farklı kayıtlı playlistlerin doğru içerikleri; bir listede en az local+Apple+YouTube+diğer provider; satır tıklamasıyla gerçek analiz ve marker görünümü; görsel screenshot karşılaştırması.

Mevcut `tests/test_media_drop_ui.c` global drop sonrası `media-library-dialog` açılmasını bekler. Yeni davranış P12-01 için bu library worker/validation bağlamını korur ve geçerli global drop'ta hemen önüne `media-playlist-add-dialog` / `media-playlist-target` / `media-playlist-add-status` hedef seçicisini açar. Kullanıcının playlist hedefini kabulünden önce import başlamaz. Bozuk payload/disconnected/busy hataları mevcut library status alanında bildirilir. Library içi drop doğrudan import davranışını korur. `tests/test_prompt12_drop_ui.c` ayrı kabul testi seçili gerçek hedefi, iki dosyanın UUID/SHA binding ve revision zincirini, iptal/repeated-drop/owner-close davranışını sınar. Uygulama veya test başarısı sonraki ayrı kayıtlarla işaretlenecektir.

## Drop uygulaması ve somut doğrulama

- **2026-10-09 02:01 UTC:** `/root/requirements_12` `src/ui/media.c` ve `include/radios/media_ui.h` drop akışını tamamlayıp Scope11/root'a root kararıyla bütün dosya sahipliğini seri devretti. Devirden sonra bu ajan bu iki dosyayı değiştirmedi. P8 `radios_media_rhythm_ensure_async` declaration'ı korunmuştur.
- **2026-10-09 02:03:39 UTC:** Root'un latest Scope11 değişikliklerini de derlediği `build/prompt12/checks/test_prompt12_drop_ui` üzerinde `GDK_BACKEND=x11 ctest --test-dir build/prompt12/checks -R '^native_prompt12_drop$' --output-on-failure` geçti: **1/1 CTest hedefi, 3/3 gerçek izole HTTP/DB/GTK senaryo, 4,81 saniye**. Bu ilk tool stdout PASS bildirimi korunur; shared LastTest kopyası `review/drop-test.log` sonradan farklı regresyon içerdiği için kabul kanıtı olarak geçersizdir. Final test-specific log beklenir; ayrıntı `final-source-review.md` kanıt düzeltmesindedir.
- Senaryolar: `(1)` hedef kabulünden önce hiçbir import olmama, iki dosyanın yalnız seçilen kayıtlı hedefe gerçek UUID/SHA bağları ve sıralı revision'la kaydı, seçilmeyen playlistin değişmemesi, yönetilen copy ile kaynak SHA korunumu ve yeniden açılışta kalıcılık; `(2)` hedef seçimi iptali ve açık seçiciye ikinci drop'ta kayıt/kaynak korunması; `(3)` worker gate ile bekleyen import sırasında owner kapanışında DB/kaynak/staging korunması.
- Test edilen canlı `media.c` SHA-256: `6aee5fa8e1805345522e2a4d04e4835051afa9b86def6d5e8f103d1d1ed05034`; `media_ui.h`: `aebd4a945c56f3e53e408f75c26de3c26982bb4d3b2ef29cc2641e4930b6855b`; yeni test: `eb9e5e81681859ee90673ee6fb595021a9388eb83897e66300c90f90799e0559`.
- Bu doğrulama **yalnız drop gereksinimini** ve belirtilen korunma davranışlarını kapsar. Playlist görsel tasarımı, sağlayıcı karışımı, satır analizi ve marker panelinin toplam kabulü root/diğer ajanların ayrı doğrulamasıdır; bu belge onları tamamlandı saymaz.


## Önceki somut kabul checkpoint'i — 2026-10-09 02:36 UTC

Kaynak PDF'nin bütün7 davranış ve22görsel gereksinimleri yukarıda aynen korunur; aşağıdaki gerçek uygulama sonucu orijinal gereksinim metnini değiştirmez.

| Kapsam | Güncel doğrulama / sınır |
|---|---|
| Global drop → Playliste ekle | Gerçek hedefpicker, kabul öncesi importyok, seri UUID/SHA/CAS/revision/ACK ve sourcecopy; prompt12drop3/3PASS, Scope11legacy8/8PASS. Doğru current arşivler resumed-native-tests ve full-provider-tests log/XML; yanlış initialdrop-test.log kanıt değildir. |
| Şema1/3 iki tablo / mevcutplaylist | Native gerçek selectors/providerfilter/sourcecopy, kalıcı kimlik ve doğrualtcontent testleriPASS. Son PNGf9c556...1280×2040 gray/olive/zebra/grayordinal/filledblueinfo/menu/solsağheading anatomisini doğrular. Component kabulü; parentwindow/fullviewport/pixelidentity iddiası yok. |
| Local tıklama / enalt markers | Gerçek full decode ve exactverifiedSHA/nonzerowaveform,8alan/6marker/konumzoom/rightclose+save; gerçek backendmarkerssave/readback. Native local stalecontext/pageclose/dirtyguard testleriPASS. |
| Mixed provider+local aynıplaylist | Gerçek DB+HTTP native persistence/reopen/edit/order/playerinput conversion; mevcut YouTube/AppleMusic/Spotify ve local birlikte. Backend current19/19PASS; provideranalysis ayrı4UUID/SHA assetbinding'dir, playbacksource providerID/URL korunur. Full remoteautomation/prepare desteği ayrıca uygulanmış sayılmaz. |
| YouTube unbound gerçekanaliz | Mevcut yetkili scope/cache ve fullencoded32MiBhelper→actualnative stagedecode/managedimport→idempotentCAS analysis_media→waveform/markerssave/reopen native6.casePASS. Actualcache/media bytes, sentetikprivateaccountfixture; canlı hesap/ağ kabulü iddia edilmez. Cachecold gerçek mevcutauthorizedresolver hazırlanır, auth/generation/account/fullSHA fences korunur. |
| Apple/Spotify provideranalizi | Kullanıcı doğru gerçekverifiedlibrarysesdosyasını ayrıanalysis_media seçerse aynıproviderentry'de measuredwaveform/kalıcımarkers/edit/reopen/nativeinput kimliği korunur; actualprovider nativecasesPASS. **Yalnızcataloguelink ile dosya seçmeden bağımsız otomatikfulltrack PCM edinimi henüz yoktur.** PDF bu ekönkoşulu yazmaz; genel otomatikanaliz kapsamı açık/koşulludur. |
| Helper/generation/fixture | Özel11/11PASS3.398s ve CTest11/11PASS2.420s ayrıkoşular; actualWAV/Opus completebytes, SHA/tenant/account/generation/corruption/cancel/limit/scopeguard/CLI/productionrootreject kanıtları. --generation sadece37byte scopedUUID metadata; exportaudio stdout credentials/URL/path içermez. |
| Son pending | Root yeni queuedcallback cancel/destroy/currentDBclose lifecycle7.nativecase kabulü hazırlanıyor. Broad native_data automation wrong-documentowner fixture düzeltme/retetst sahiplerinde; öncekiFAIL ve çalışmayan7.docreadcase başarı sayılmaz. Kullanıcının Apple/Spotify analizkaynağı tercih yanıtı henüz alınmadı. |

Son main-source SHA snapshotı `review/source-review-main-map-20261009T0236Z.json`; kaynakPDF+3figures4/4 unchanged. Tarihsel hatalar ve ayrıntılı source/runtime/görsel karşılıkları `final-source-review.md` içinde checkpoint sırasıyla korunur. Bu checkpoint bütünPDFişi tamamenbitti anlamına gelmez; açık/koşullu maddeler tamamlandı gösterilmez.


## Son bağımsız kabul checkpoint'i — 2026-10-09 02:45:19 UTC

Sorumlu: `/root/source_review_resume_12`. İşlem öncesi STATUS, son 02 UTC kaydı ve ortak AGENT sahiplikleri okundu. Aşağıdaki kanıtlar önceki 02:36 checkpoint'indeki bekleyen yedinci lifecycle ve son kolon görsel kabulünü günceller; tarihsel başarısızlık kayıtları korunur.

| Gereksinim / kontrol | Gerçek sonuç ve sınır |
|---|---|
| P12-01 — Global drop | Final canonical frozen koşusunda gerçek hedef seçimi/import/binding/CAS/source copy/iptal/owner-close **3/3 PASS**; legacy media drop **8/8 PASS**. |
| P12-02, V01–V07 — Şema 1/3 | Son **1280×2040** PNG bağımsız karşılaştırıldı: dar TITLE, CUE sonrası geniş başlıksız sağ alan, en sağda dolu mavi bilgi ikonu; gri yüzey, zeytin seçili satır, beyaz/turkuaz zebra, gri sıra hücreleri, sol/sağ başlık, iki tablo ve ilgili şeritte gerçek playlist seçimi görünür. Son kolon oranı bulgusu kapandı. Kaynak normal parent pencere/viewport ve piksel birebir eşlik kanıtı değildir. |
| P12-03/04, V08–V22 — Gerçek analiz ve alttaki işaretler | Local, gerçek verified analysis_media bağlı üç provider ve bağsız YouTube full-export/import yollarının gerçek waveform, sekiz alan, altı marker, konum/zoom, sağ kapat/kaydet, backend kayıt/yeniden açma kabulü **7/7 native PASS**. Son vaka gerçek encoded export tamamlandıktan sonra callback beklerken target değişimi/retained page destroy/current client close için üç stale/cleanup sınırını kapsar. |
| P12-05/06 — Mevcut playlist/alt şarkılar | Kalıcı source kimliği, gerçek alt içerik, search/provider filtreleri ve source-copy native kabulü geçti. |
| P12-07 — Mixed local/provider | Mevcut YouTube/Apple Music/Spotify ve local tek gerçek listede; provider identity/URL korunur, analysis_media ayrı doğrulanmış varlıktır. Backend **19/19 PASS** önceki bağımsız tarihli koşudur; bütün remote automation/prepare desteği olarak genişletilmez. |
| Helper ve drop/controls korunumu | Final focused snapshot: **7 playlist +3 drop +11 helper +10 controls +8 legacy media drop =39 PASS**, **5/5 target**, 0skip; ayrı document-read-order **1/1 PASS**. 384 code/CSS source önce/sonra aynı, drift boş. |
| Son kolon değişimi | Yeni canonical validation **385 source önce/sonra aynı, drift boş**; strict app/native build exit0; yalnız değişen playlist alanı yeniden **7/7 PASS**, wrapper **13,759 sn**. Önceki 39 vaka ayrı tarihli kabul olarak korunur; tek yeni koşuda 39 çalıştı denmez. |
| Açık otomatik provider analizi | **Apple/Spotify yalnız katalog bağlantılı, analysis_media bağı olmayan satırda bağımsız otomatik tam ses edinimi uygulanmadı.** Gerçek verified library dosyası seçilince ölçülen waveform/kalıcı marker yolu kanıtlıdır. PDF bu dosya seçme önkoşulunu yazmadığından P12-03/04 genel otomatik kapsamı açık/koşullu kalır. Kullanıcının tercih yanıtı yoktur. |
| Diğer sınırlar | Canlı provider hesabı/ağ kabulü yapılmadı. Broad native_data altıncı automation yanlış belge sahibi fixture regresyonu çözüldü sayılmaz; yedinci document-read-order artık ayrı gerçek PASS'tir. |

Son playlist kaynak SHA `2d1e38223e40e4d96c4570410c8bddc5e750d68e23a486fac12816cf9c06af0a`. Son PNG SHA `9320b8031c45b31c4b464dfb2ab8ed453f9db8d30f140ac79b5d4d1d4de64f86`. PDF+3 figure tekrar **4/4 değişmedi**; tüm 12 ana kaynak yeni final kolon manifestiyle **12/12 aynı**. Ayrıntılı kaynak/boyut/sahip/test/görsel haritası [`review/source-review-main-map-20261009T024519Z.json`](review/source-review-main-map-20261009T024519Z.json); asıl bütün code/CSS no-drift kanıtları [`final-frozen-validation.json`](review/final-frozen-validation.json) ve [`final-column-validation.json`](review/final-column-validation.json). Bu kayıt bütün PDF işinin sınırsız tamamlandığı veya tüm repository testlerinin geçtiği anlamına gelmez.


## Son Release / native_data / default kurulum ve official capture devir checkpoint'i — 2026-10-09 02:58:12 UTC

Sorumlu: `/root/source_review_resume_12`. Güncel STATUS/02 kayıtlarıyla root'un gerçek tamamlanmış artefaktları bağımsız doğrulandı. Yeni source/test/binary/service yazımı yok; yalnız own rapor/kanıt belgeleri güncellendi.

**Release:**02:48:40.405226→02:49:05.662919UTC,385 code/CSS frozen/drift[], strict O3/NDEBUG/C11/Wall/Wextra/Wpedantic/Werror app/native build ve **7/7 playlist actualPASS**,0skip/wrapper12,742sn. Candidate SHA `2d3805e97cbdc563433479db9edffbe254208aa9d07a765a23094ac1ebcc69a4`.

**Önceki broad native_data6 hatası gerçek kabul ile kapandı:** authorized generic automation document_owner fixture b82fda...1ab0b→**50d17d96a0dbb2683b7f7e19e9602dd80ca0d0a32e42adf373491bbee542ebae** sonrası02:51:16.756027→02:51:39.607452UTC **7/7 actualPASS**,0skip/wrapper22,389sn,385source0drift. Gerçek revision/CAS/readback/transport/profile/owner shutdown ve document-read-order vaka sonuçları logdadır. Önceki FAIL silinmez; artık data6 pending/data7 notrun denmez. Release snapshot eski b82fda testini taşır; bu yeni data retest ayrı kabulüdür. Diğer384 source ve bütün production girdileri Release ile aynı.

**Varsayılan binary atomik kurulumu gerçek teyitli:**02:52:11.486174→02:52:11.505831UTC `default-install-receipt.json`; eski backup b1578bb7...3726e sameSHA; Releasecandidate==installed `build/radios`==2d3805e9...c69a4,mode0755. Receipt referenced releaseSHA doğru; install385sourcebefore==after, current385/385 aynı, native_data kabul snapshotıyla aynı. Canonical source/defaultbinary/realpath kilitleri receipt'te kayıtlı. Reviewer launch smoke yürütmedi; hash/receipt böyle bir smoke yerine gösterilmez. Son playlist2d1e382...af0a, helper5c2fc16...1368/native7test38b053...96cd ve PNG9320b803...64f86 sabittir. Bağımsız tam hash/flags/evidence sonucu [`review/source-review-release-install-20261009T025812Z.json`](review/source-review-release-install-20261009T025812Z.json).

**Official Apple capture source incelemesi tamamlandı, uygulama değil:** [`review/official-apple-capture-feasibility.md`](review/official-apple-capture-feasibility.md) tüm gerçek function/path+sourceSHA+primary CDP/Apple kaynaklarını taşır. Browser ilk mevcut target'ı reuse edip kuyruğunu değiştirir; cleanup bütün browser'ı kapatır. CDP yeni owned target açabilir, fakat same-profile bağımsız authorized playback, target↔uniquePulseinput/accountgeneration/provenance, atomikstart0, gerçekEOF/taildrain/full-duration, hole/underrun kabulü repo'da hazır değildir. Şu recorder wallclockframe boşluklarını sıfırla doldurur; duration/peak tam asset kanıtı değildir. Yeni source yazılmadı; aktif MAIN/yayını etkilemeden bağımsız TAM ses garantisi bulunmadı.

**Açık kalması gereken gerçek iş:** analysis_media bağı olmayan Apple/Spotify catalogue-only otomatik P12-03/04 genel kapsamı ve canlı provider kabulü açık; gerçek verified library dosyası seçilirse uygulanmış measuredwaveform/kalıcımarkers yolu korunur. Kullanıcı kaynak tercihi yanıtı yok. Sonraki adım currentdurumdan ayrı capture controller'ın target/audio/session/fullasset kabul koşullarını değerlendirmektir; mevcut actual local/YouTube/filebound başarılarıyla fullPDF bitmiş sayılmaz. Şema3 strip yazılı “playlist kısmını değiştir” talebiyle playlist/search/provider olarak düzenlenmiştir; kaynak bitmapteki üç AnyCategory/Subcategory/Genre kontrolü aynı kontroller olarak mevcut değildir; saf piksel aynılığı kabulü iddia edilmez.

İşlem hataları sourcefeasibility raporunda açık: yanlış tahminli readpath rg2 ve düzeltilen raporbetiği SyntaxError, hiçbir source/binary side effect olmadan düzeltildi. Historical sourcefreeze/protocol/FAIL kanıtları korunur.
