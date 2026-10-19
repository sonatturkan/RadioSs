# Prompt 5 — uygulama ve doğrulama kaydı

Kapsam: yalnız `../prompt/5/5.pdf` ve iki `figures` görseli. Kaynakların eksiksiz incelemesi ve sayfa/görsel eşlemesi [gereksinim matrisinde](prompt5-requirements.md) kayıtlıdır. Diğer prompt ekiplerinin eşzamanlı değişiklikleri bu belgenin gereksinim kaynağı değildir.

## Uygulama

Şema 1'deki devam davranışı, veritabanı bağlantısı, yedek durumu, uyarı ve Ayrıntı erişimi `status-footer` içine taşındı. Mevcut yenileme, istasyon, UTC güncelleme ve eşzamanlı Prompt 1'in eklediği tercih dişlisi korundu. Veritabanı etiketi gerçek bağlantıyı izler; yedek ve uyarı gözlemi yokken mevcut bilinmeyen değerler korunur. Ayrıntı mevcut tanı penceresini, yedek bağlantısı Acil profilini açar.

Boşalan başlık alanında önceki şarkı, çal/sürdür, duraklat, durdur, sonraki şarkı ve sonraki playlist kontrolleri vardır. Komutlar Yayın görünümünde seçilmiş genel veya bağımsız oynatıcı sırasına gider; başka profildeyken de aynı seçili sırayı kullanır. Duraklatılmış, saatli, hazırlanan, bitmiş veya son öğeye ulaşmış sıralarda ilgili geçişler pasiftir. Durdur mevcut sıranın ilerleme işaretlerini de sıfırlar.

Mevcut modelde kütüphane playlistleri program event/blokları olarak yayına eklenir. **Sonraki playlist**, çalışan sıranın mevcut bloğundan sonraki dolu bloğun ilk çalınabilir öğesine geçer; boş bloklar atlanır. Hedef, değiştirilebilir editör taslağından değil başlatılmış planın veya çevrimiçi akışın sakladığı kopyadan seçilir. Yerel sıçrama aynı runner epoch'unda sınırları doğrular ve mevcut kısa kesme zarfını kullanır; atlanan öğeler çalınmış/tamamlanmış kaydedilmez. Çevrimiçi akış mevcut sağlayıcı ve yerel medya yollarını kullanır.

| Dosya | Prompt 5 değişikliği |
|---|---|
| `src/ui/header.c`, `src/ui/header_navigation.c` | Bilgi grubunun taşınması, üst geçiş butonları ve yaşam döngüsü |
| `src/ui/window.c`, `assets/themes/radios.css` | Alt durum çubuğu ve mevcut tasarıma uygun stiller |
| `src/ui/broadcast.c` | Seçili sıra bağlamını koruyan ortak navigasyon API'si |
| `src/ui/playout_ui.c`, `src/playout/planner.c` | Başlatılmış planın blok kimlikleri ve playlist hedefi |
| `src/ui/broadcast_online.c` | Saklanmış çevrimiçi/karma akışta sonraki blok hedefi |
| `src/audio/engine.c`, `include/radios/header_navigation.h` | Sınır/epoch denetimli genel ve bağımsız oynatıcı sıçraması |
| `tests/test_header_navigation_backend.c`, `tests/test_header_navigation_ui.c`, `tests/run_header_navigation_ui.py` | Anlamlı ses, sahiplik, GTK ve bağlantı kabul testleri; her GUI senaryosu için ayrı geçici hesap/veritabanı |
| `cmake/Prompt5Tests.cmake`, `CMakeLists.txt` | Yeni kaynak ve test hedefleri |

## Nihai kabul — 2026-10-09, UTC

PDF 5 kapsamı tamamlandı. `5.pdf` tek sayfası ve iki figures PNG'si eksiksiz incelendi; PDF içine gömülü iki görüntüyle figures dosyalarının RGB verileri eşleşti. P5-01–P5-10 eşlemesi [gereksinim kaydında](prompt5-requirements.md) bulunur.

| Kontrol | Gerçek sonuç ve kanıt |
|---|---|
| Güncel GUI/backend derlemesi | Strict Debug PASS; `-Wall -Wextra -Wpedantic -Werror` etkin |
| Dört GTK senaryosu | **4/4 PASS**, 26,907 s: footer/diagnostik/yedek/gerçek DB bağlantısı; yerel ve karma akışta immutable blok geçişi ve aynı epoch ile sürdürme; seçili bağımsız sıra izolasyonu; çevrimiçi akış gezinmesi |
| Yeni motor senaryoları | Beş bare senaryo PASS; altıncı Pulse ortamı gerektirdiği için bare koşuda açık SKIP. Ayrı owned Pulse hedefinde altıncı senaryo **1/1 gerçek PASS**, 3,263 s. Böylece **altı benzersiz motor senaryosu gerçekten çalıştı** |
| Test sırasında kaynak tutarlılığı | 02:33:27–02:33:58 arasında 28 kaynak ve iki test ikilisinde SHA değişimi **yok**. Üç CTest hedefi PASS; [JSON](../build/prompt5-test/header-navigation-stable-final.json), [JUnit](../build/prompt5-test/header-navigation-results.xml), tam kayıt `build/prompt5-test/header-navigation-pass-02-stable.log` |
| Release derlemesi | 02:33:24–02:33:31 strict PASS; 304 kaynak hash'i ve sıfır kaynak değişimi [manifestte](../build/prompt5-test/release-source-snapshot.json). Release/test manifestlerinin ortak 24 uygulama kaynağında fark yok |
| Varsayılan başlatıcı | 02:35:19'da `build/radios` atomik güncellendi, önceki ikili yedeklendi. SHA-256 `b1578bb7c6c97e8667848ade86c4aa8cb615b067d4ea62a264077bff8013726e`. [Kurulum kaydı](../build/prompt5-test/installed-binary.json) |
| Kurulu ikilinin çalışması | `run-radios.sh --smoke-test` **8 profil, mapped pencere ve gerçek iş/durum teslimi PASS**. [Log](../build/prompt5-test/installed-smoke.log), [kurulu pencere görüntüsü](../build/prompt5-test/prompt5-installed-ui.png) |
| Mevcut çekirdek regresyonu | Sonraki dış engine değişimiyle 02:37:07–02:37:12 yeniden strict derleme ve **3/3 CTest PASS**, 5,63 s. Bare model 2 + runner 23 + playout 22 gerçek PASS; runner'ın ortam gerektiren tek Pulse vakası ayrı owned Pulse koşusunda **1/1 PASS**, 2,04 s. Toplam **48 benzersiz gerçek vaka**; [JUnit](../build/prompt5-test/fresh-backend-regression.xml), [fiziksel Pulse logu](../build/prompt5-test/fresh-backend-regression-runner-pulse.log) |

Motor testleri gerçek PCM hedefini, atlanan öğenin çalınmış receipt oluşturmamasını, kesilen öğenin INTERRUPTED terminalini, eski epoch/sınır/pause/schedule/pending-cut retlerini, manual wait, bağımsız runner sahipliği ve değiştirilen taslaktan etkilenmeyen blok kimliğini denetler. Ek gerçek Pulse regresyonu renderer FINISHED olmuşken ses hâlâ duyulan PLAYING durumundaysa aynı sahipli geçerli hedefe geçişi doğrular. İlk uygulama bu durumda ret veriyordu; aynı nihai fixture düzeltme öncesinde FAIL, düzeltmeden sonra PASS verdi.

Hızlı Pause → Play, output flush tamamlanmadıysa mevcut motor sözleşmesine uygun UNAVAILABLE dönebilir. Bu ret sırasında epoch/parça korunur; çıkış hazır olduğunda başlık Play aynı parçayı sürdürür. Yerel, karma ve sentetik sağlayıcılı çevrimiçi akışta istemsiz yeniden başlatma kontrol edildi. Başarılı manuel çevrimiçi playlist geçişi eski bekleyen completion kuyruğunu temizler; doğal geçiş yolu değiştirilmedi.

## Görsel kabul ve doğrulama sınırları

[1800 × 1000](../build/prompt5-test/prompt5-footer-1800.png) ve [1000 genişlik isteğinin gerçek 1568 × 680 sonucu](../build/prompt5-test/prompt5-footer-1000.png) kök, gereksinim ve koordinasyon ajanlarınca figures ile karşılaştırıldı. Prompt 5 navigasyon ve footer bilgi/eylemleri bu gerçek boyutlarda görünür ve okunur. **Gerçek 1000 px genişlik ve farklı font boyutları doğrulanmış değildir.** PDF 5 ayrı bir 1000 px şartı içermez; dosya adındaki sayı gerçek pencere genişliği olarak sunulmaz.

Çevrimiçi test gerçek GTK/Flow/RadioS HTTP yetki akışını kullanır; YouTube sağlayıcısının durum/katalog yanıtları ve oynatıcı API'leri sentetiktir. Canlı çevrimiçi hesap/PCM kabulü iddia edilmez. Yerel ses testleri gerçek motor ve sahip olunan sanal Pulse çıkışlarında çalışır; üretim kullanıcı veritabanı test girdisi değildir.

Kurulu Release ve dört GTK senaryosu engine `c53c0e8c…` kaynak anına aittir. Daha sonraki 48 çekirdek senaryosu engine `491ffd84…` ile çalıştı; dış PDF4'ün eklediği `runner_paused` snapshot alanı kurulu eski ikilide varmış gibi gösterilmez. Kaynaklar diğer prompt ekiplerince geliştirilmeye devam eder; tarihli kabul bütün deponun gelecekteki değişikliklerini kapsamaz.

Ortak kayıtta Prompt 6'nın 02:34:33 tarihli DJ PCM-pan-gain testi başarısızdır; ilgili ekip teşhis ediyor. Bu dış görev PDF 5 testleriyle kapatılmış sayılmaz. Bütün repository testlerinin geçtiği iddia edilmez.

İlk fixture owner arama/capture/domain/precondition hataları, toplu GUI testinin özel hesabında biriken istek kotası ve diğer ekiplerin geçici provider/Release derleme hataları loglarda korunmuştur. Her GUI senaryosu ayrı geçici hesap/veritabanında çalıştırılarak kota izolasyonu sağlandı; üretim limitleri veya genel mutation retry davranışı değiştirilmedi.

## Kalıcı kayıtlar

Ortak iletişim: `../prompt/agent/AGENT.md`. Bu ekibin ayrık devam durumu: `../prompt/agent/prompt5/STATUS.md`; ayrıntılı UTC saatlik/JSONL olay kayıtları `../prompt/agent/prompt5/logs/` altındadır. Kaynak değişimleri ortak `source-edit.lock` ve dosya `.lock` altında güncel içerik korunarak yapıldı. Önce/sonra hash ve yedekler `build/prompt5-backups/root/manifest.jsonl` ve `build-prompt5/backend-backups/` altında tutulur.
