# Prompt 8 — Oynatıcı kartları

Kaynak kapsamı yalnız `../prompt/8/8.pdf` (1 sayfa) ve
`../prompt/8/figures/sema_1.png` (801 × 146). PDF'nin bütün metni ve şeması
incelendi. Diğer prompt oturumlarının aynı projedeki işleri bu kaydın kabul
kapsamına dahil değildir. Ortak koordinasyon:
`../prompt/agent/AGENT.md`, oturum `OTURUM-P8-20261009-0148`.

PDF başlığı playlist/otomasyon sekmelerini anıyor. Şema 1'deki zaman,
konum, ses, CUE, SYNC, Ana deck ve BPM kontrol grubunun mevcut kaynakta
karşılığı `broadcast.c` içindeki oynatıcı kartlarıdır. Gereksinimler bu
kontrol grubuna uygulanmıştır; PDF'de başka bir arayüz şeması bulunmuyor.

## Gereksinim eşleştirmesi

| Kimlik | PDF sayfa 1 / Şema 1 gereksinimi | Uygulama | Doğrulama |
|---|---|---|---|
| P8-01 | Gösterilen kontrollerin üstünde, veritabanı waveform'u ve cue noktaları | `src/ui/player_waveform.c`: 76 px native çizim; gerçek min/max örnekleri, IN/VO/OUT/F1/F2/AUTO ve gerçek oynatma konumu. `broadcast.c` transport ile zaman/konum/ses kontrolleri arasına yerleştirir. | Gerçek HTTP marker kaydı, waveform/Cairo örnek karşılaştırması ve cue çizgilerinin zaman konumları için native test eklendi; çalıştırma sonucu aşağıda. |
| P8-02 | Her oynatıcıdan Analiz düğmesi kalkar; analiz otomatik yapılır | `broadcast.c` yüklü medya ve yerel playlist girdilerini worker'a verir. `rhythm_jobs.c` kayıtlı analizi okur, yalnız eski eksik kayıtları yeniden çözüp CAS ile saklar. İçe aktarma zaten waveform/rhythm üretir. | Gerçek rhythm'sız eski format importu, yükleme sonrası kalıcı analiz revision/BPM ve bilinmeyen BPM'de tekrar analiz olmaması sınanır. |
| P8-03 | Sync, BPM'nin yanında | BPM ve SYNC aynı yatay satırda bitişiktir; BPM tek satırdır ve satırın boş alanına genişlemez. | GTK çocuk sırası, aynı parent ve gerçek metin sınırı ile SYNC arasında 0–8 px boşluk denetimi. |
| P8-04 | Ana deck alanı kalkar; alttaki AutoDJ aynı görevi üstlenir | Tek AutoDJ seçicisi hem geçiş hem SYNC hedefidir. SYNC master'ı etkinleştirme anında motor doğrular; pasif AutoDJ seçimleri iki yönlü olabilir. Eski ayrı master tercihi seçim değişiminde temizlenir. | Seçilen çiftin gerçek PCM SYNC/AutoDJ akışı, ayarların DB'de saklanması ve yeniden açılışı. |
| P8-05 | Oynatıcılar yatay dikdörtgen yerine kare | GTK aspect frame oranı 1:1, minimum 420 × 420, homojen flowbox; kompakt erişilebilir transport ve kaynak düğmeleri. Oynatıcı ekleme/silme ve boş kart seçimi yeni sarmalayıcıyı tanır. | İki pencere boyutunda gerçek kart allocation oranı ve görünür bütün doğrudan çocukların kart sınırları içinde kalması denetimi. |

## Veri ve yaşam döngüsü

Ölçülmüş waveform ve cue verileri mevcut `desktop-media/get` sözleşmesinden
gelir. Sürüm, istasyon, SHA ve marker aidiyeti doğrulanır; çizim örnek waveform
üretmez. Kartın mevcut detay kaydı hemen kullanılır. Program motorunun yüklediği
ve kartta önbelleği olmayan medya arka planda okunur. Otuz saniyelik sınırlı
yenileme ve mevcut veri yenileme bildirimi marker değişikliklerini getirir.
Yeni medya/sürüm/nesil, hesap bağlantısı değişimi veya kapanış eski cevabı
geçersiz kılar ve isteği iptal eder. Çevrimiçi girdiye ait olmayan eski yerel
waveform gösterilmez.

Otomatik analiz mevcut başarılı sonucu, güvenilir BPM bulunamasa da korur.
Eksik eski kayıtlar GUI'yi bekletmeden analiz edilir. Geçici istek hataları beş
saniye aralıkla tekrar denenir; erişim, bulunamayan kayıt ve geçersiz veri
hataları aynı kimlik için sürekli yeniden denenmez. Hatalar mevcut bildirim
alanına aktarılır. Analiz
sonucu yeni metadata/cue revision'ını eski veriye döndürmez. Bağlantı değişince
iptal/deneme kayıtları temizlenir; deneme önbelleği sınırlıdır. Analiz veya
waveform okuma kendiliğinden ses başlatmaz.

## Dosya sahipliği

- `/root`: `broadcast.c` içindeki oynatıcı bölümü; `rhythm_jobs.c`,
  `include/radios/media_ui.h` içindeki ensure API; yeni waveform CMake bağlantısı;
  bu belge ve ayrı `build-prompt8-test`.
- `/root/pdf_audit`: `src/ui/player_waveform.c`,
  `include/radios/player_waveform.h`.
- `/root/audio_audit`: `tests/test_playout_ui.c` içindeki ilgili testlerin
  geliştirilmesi ve bağımsız incelemesi; son dar yazma sahipliği root'a devredildi.
- `/root/coordination`: ortak P8 bölümü ve
  `prompt/agent/sessions/prompt8-20261009-0148/` UTC saatlik kayıtları.

Araç sınırı root dahil dört eşzamanlı ajandır; istenen 14 eşzamanlı ajan
oluşturulamıyor. Kullanılabilir dört slot çalışma boyunca kullanılır.
Başka aktif prompt oturumlarının kayıtları ve kaynak değişiklikleri korunur;
çakışmalar ortak kayda aktarılır.

## Doğrulama durumu

- Son strict C11 Debug derleme `radios` ve `test_playout_ui` hedefleri için
  2026-10-09 02:21 UTC'de başarılı. Ayrı `build-prompt8-test` kullanılır.
- Son üç P8 hedefi **3/3 başarılı**, atlanan vaka yok, toplam 32,06 sn:
  `native_player_waveform` 5,66 sn; `native_player_sync` 21,09 sn;
  `native_player_sync_stale` 5,30 sn. Her hedef gerçek izole DB/Pulse fixture
  içinde tam bir GLib vakasını çalıştırır; boş seçim veya SKIP başarı kabul
  edilmez. Kanıt: `build-prompt8-test/prompt8-focused-final.xml`.
- Bu koşu gerçek DB waveform/cue kaydı ve raster konumlarını, BPM'siz eski
  medyanın otomatik analizi ile revision kararlılığını, kart sınırlarını,
  fiziksel BPM/SYNC komşuluğunu, AutoDJ seçiminin saklanması/yeniden açılmasını,
  seçilen çiftte gerçek PCM tempo/geçişini ve geç gelen SYNC sonucunun başka
  kaynağı bozmamasını doğruladı.
- Son P8 ekranları `build-prompt8-test/shots/player-waveform-cues.png` ve
  `player-sync-autodj.png` içinde. İlk görsel altı marker'ın, waveform'un,
  kare kartın ve bitişik BPM/SYNC kontrollerinin birlikte kanıtıdır.
- Destekleyici önceki koşu: `native_controls`, `native_audio`,
  `audio_devices` **3/3 başarılı**, 43,70 sn, atlanan vaka yok.
  Kanıt: `build-prompt8-test/prompt8-audio-controls-final.xml`. Bu sonuç
  önceki kaynak/binary anlık görüntüsüne aittir; daha sonra başka prompt
  oturumlarının yaptığı değişikliklerin kabulü sayılmaz.
- İlk çekirdek koşusu: `rhythm`, `audio`, `playout` **3/3 başarılı**, 0,89 sn.
  `audio` içindeki fiziksel/sanal Pulse alt senaryosu bu doğrudan koşuda
  atlandı; yukarıdaki `audio_devices` koşucusunda gerçek sanal çıkışlar
  ayrıca başarıyla sınandı.
- `desktop_media` 1/1, `native_broadcast_online` 1/1 ve
  `native_broadcast_catalogue` 1/1 önceki regresyon koşularında başarılı.
- Yeni waveform modülü, rhythm worker ve yeni test dosyası
  `-Wall -Wextra -Wpedantic -Werror -fsyntax-only` denetiminden geçti.
- İlk GUI derleme denemelerindeki dış API/Preferences/navigation eksikleri
  ilgili sahipler tarafından tamamlandı; son strict derleme başarılıdır.
  Geçmiş başarısız denemeler saatlik kayıtlarda korunur.
- Önceki genel `native_playout` koşusu doğal tamamlanma sonrası devre dışı
  kalan Durdur düğmesine basan P3 test adımında başarısızdı. P3 sahibi bu
  adımı devre dışı düğme ve gerçek PCM sessizlik denetimine çevirdi.
  Güncel bütün grup **1/1 CTest hedefi, 11/11 gerçek GLib vakası başarılı**,
  126,37 sn, atlanan vaka yok. Kanıt:
  `build-prompt8-test/prompt8-playout-resumed.xml` ve korunmuş
  `../prompt/agent/sessions/prompt8-20261009-0148/evidence/20261009T022734Z/general-LastTest.log`.
  Bütün vakalar aynı yukarıdaki son test binary'siyle
  ayrı fixture içinde çalıştı; önceki genel başarısızlık bu koşuda giderildi.

İki bağımsız ajan son cue/SYNC görüntülerini kaynak Şema 1 ile karşılaştırdı.
Görünür kartlar kare, bütün kontroller sınır içinde, waveform ve altı cue
işareti doğru konumda, SYNC BPM'ye bitişik ve yalnız tek AutoDJ seçicisi var.
Mevcut P7 davranışında girdi expander'ı gizlidir; playlist yüklenince yalnız
genişleme durumu değişir, görünür olmaz. Bu davranış değiştirilmedi.
Waveform'un gecikmeli HTTP yanıtına ilişkin bütün iptal/revision korumaları
kaynak incelemesinde denetlendi; ayrı bir waveform HTTP yarış testi yapılmadı.

Varsayılan `build/radios` uygulama binary'sinin seri yazma sahipliği ortak
kayıtta PROMPT5/root'a aittir. P8 son kaynak ve kabul sonuçlarını bu sahibe
iletti; P8 yalnız yukarıdaki ayrı Debug build'i oluşturdu.

Test edilen `test_playout_ui` SHA256:
`fe4d64ed5291d8e1d42c7ee7ba53a9c2c7a2d72989572fd8d1ad5a63c32e569c`.
Test edilen `radios` SHA256:
`d58c2b73fc85f3a6ead5c642cba85c6f113ba71473b1c79b656a375bcb34a7ab`.

Odaklı komutlar:

```sh
cmake -S RadioS -B RadioS/build-prompt8-test -DRADIOS_GUI_TESTS=ON -DBUILD_TESTING=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build RadioS/build-prompt8-test --parallel 4 --target radios test_playout_ui
ctest --test-dir RadioS/build-prompt8-test --output-on-failure --output-junit prompt8-focused-final.xml -L prompt8
ctest --test-dir RadioS/build-prompt8-test --output-on-failure --output-junit prompt8-playout-resumed.xml -R '^native_playout$'
```

Native testler ayrı geçici DB ve kendi oluşturdukları sanal ses çıkışlarıyla
çalışır; kullanıcı veritabanına test kaydı yazılmaz. Fiziksel donanım kabulü
bu GUI değişikliklerinin otomatik test kapsamına dahil değildir.
