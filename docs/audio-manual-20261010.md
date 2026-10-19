# RadioS manuel audio ve tuş denetimi

Güncelleme: 2026-10-11 00:04 UTC. Dört üretim düzeltmesi, aşağıdaki kapsamlarla doğrulandı.

Bu çalışmada CMake, gcdmake, make, ninja ve CTest kullanılmadı.
Audio kaynakları ve düğme callback zincirleri elle incelendi.
Derleme doğrudan `cc`, `pkg-config` ve `ar` ile yapıldı.
Gerçek GTK düğmeleri, PCM/WAV, özel HTTP ve Pulse fixture'larıyla doğrulandı.

## Düzeltilen davranışlar

- **Tempo EOF — engine.c:** Yavaş çalmada boş PCM kuyruğuna rağmen PLAYING
  kalıyordu; hızlı çalmada kuyrukta 250 kare varken erken EOF oluşuyordu.
  İki koşul kaynak zamanına göre düzeltildi; cue-out ve EOF regresyonları geçti.
- **Loader retry — player_loader.c:** Reddedilen veya asenkron hata veren liste
  isteğinden sonra sekmeye dönüş tekrar istek başlatmıyor, Yükle/Ekle çalışmıyordu.
  Bekleyen ve başarılı yükleme ayrıldı; kısmi hatada seçim ve tekil satırlar korunuyor.
- **Görünmeyen deck — playout_ui.c:** Altı görünür MAIN kartı kapalıyken Program
  Play görünmeyen 6/7 decklerinde yayın açabiliyordu. Normal havuz yalnız canlı
  player kartlarını seçiyor; uygun kart yoksa reddediyor. PCM/mapping vakaları geçti.
- **MAIN yeniden başlatma — audio/routing.c:** STOP→PLAY sırasında fiziksel çıktı
  hazır görünse de henüz PCM tüketmiyordu; dış ses kuyruğu dolup bağlantı kesiliyordu.
  Üretici capture örneklerini almadan kapasiteyi bekliyor; STOP/CLOSE beklemeyi kesiyor.
  Aynı playlist/girdi, yeni generation ve korunmuş -12 dB gain ile PLAY geçti.

## Doğrulama durumu

| Kontrol grubu | Gerçek sonuç ve kaynak kapsamı |
|---|---|
| Son bütün uygulama | 23:57:35 kaynak freeze, 23:57:45 strict 72 object + uygulama + 21 test link PASS |
| Son çekirdek ve provider bağları | Aynı closed-audio archive ile çekirdek 102 + güncel YT 8 = 110/110 PASS, skip 0 |
| Son GTK transport/mapping | Aynı snapshotta audio 8, playout 4 ve mapping 3 gerçek PASS |
| MAIN ve kayıt yönlendirme | Aynı düzeltilmiş binary: 6/6 GTK + iki bağımsız WAV PASS, 14.21s |
| Son backend yönlendirme | Aynı binary/runner/eşikle ayrı tekrar PASS, 23.05s; MAIN gain oranı 0.4999775, boşluk 0 |
| Son YT pause | Güncel test kaynağıyla 8/8 ve bağımsız WAV 780 ms/MP3 775 ms silence PASS; >=200 ms eşik korundu |
| Önceki kontroller | 23:34–23:49 snapshotlarında loader 9, DJ 9, live 18, PTT ve ayrı seek 7 PASS |
| Yayın/kayıt/acil kontrol | Ayrı 23:42 kabulünde 5/5 gerçek GTK/PCM/HTTP PASS |
| Recovery ve uzun koşu | Önceki eşlenik 23:30 snapshotıyla recovery 1 ve gerçek 300s soak PASS |

Paralel backend koşuda 20/42.5 ms gerçek PCM boşluğu ve spektral sapma ölçüldü.
İlk hata korundu; değişmeyen binary/runner/eşikle ayrı tekrarda gain 0.4999775
ve PCM boşluğu 0 kabul edildi. PTT ilk paralel hata sonrası aynı güçlü kontrolle geçti.
Routing testinin engine oluşturma sırası, Preferences kaynak/test başlık kayması
ve YT testi tüketim fence'i fixture sorunları olarak ayrıştırıldı; ürün fixi sayılmadı.
Özel Pulse 17 yalnız proje altına açıldı; sistem kurulumu ve host audio değişikliği yok.
Son YT test kaynağının değişken adı çakışması dış sahibi tarafından giderildi;
derleme ve gerçek wrapper geçti. Test/fixture düzeltmeleri ürün fixine eklenmedi.

Kanıtlar: `build/audio-manual-20261010/`. Son kaynak/hash makbuzu
`closed-audio/source-snapshot.json` ve `final-audio-audit.json`; çekirdek/YT kanıtı
`requirements-closed-20261010T235912Z/final-summary.json`,
GTK kanıtları `validation-closed/results.json`, `root-closed-audio-ui.json` ve
`root-closed-routing-proof/acceptance.json` içindedir. Önce/sonra hata kayıtları korundu.
Eski snapshot sonuçları daha yeni kaynakların kabulü sayılmaz; özellikle 23:30 soak
kanıtı son provider geliştirmelerini kapsamıyor. Fiziksel ses donanımı ve gerçek
Apple Music/Spotify hesapları bu fixture'larda sınanmadı.
