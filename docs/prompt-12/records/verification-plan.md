# Prompt 12 bağımsız kabul planı

Bu plan mevcut source/test salt okunur analizinden hazırlanmıştır; çalıştırılmış test sonucu değildir.

| Kaynak gereksinimi | Uygun doğrulama | Kabul kanıtı |
|---|---|---|
| PDF s1 drop → Playliste ekle | Gerçek GTK URI payload, sentetik yerel dosya, izole DB | Görünür Playliste ekle eylemi; seçilen mevcut listeye sıra korunarak ekleme; kaynak byte/hash korunur |
| Şema1 playlist tasarımı | Native GTK widget inspection + screenshot | Üst aktif playlist tablosu, alt kaynak/playlist şarkı tablosu; okunur time/artist/title/intro/duration/marker alanları |
| PDF s1 tıklama → analiz → Şema2 | Sentetik ölçülebilir medya, gerçek analysis/marker editor | Tek satır tıklamasında async analiz; doğru media/version kimliği, ölçülen duration/waveform; placeholder medya doğrulanmış sayılmaz |
| Şema3 mevcut playlist seçici ve alt şarkılar | En az iki gerçek farklı playlist + boş playlist | Combo kimliği değiştirdiğinde alt tablo doğru listede kalır; üst hedef değişmez; seçilen kaynak parçaları üst listeye eklenebilir |
| PDF s2 karışık Apple/YouTube/tüm provider/local | DB entry-save/list/reopen; GTK provider form ve existing source list | Tek playlist içinde sıra ve kaynak kimliği korunur; provider dış URL ve local media binding ayrı doğrulanır |
| Eşzamanlı sahiplik/CAS | Eski revision, station mismatch, malformed provider/URL | Eski kayıt/başka station veya bozuk provider mutation reddi; idempotent replay aynı sırayı korur |

Mevcut gerçek CMake hedefleri: native_controls (playlist session ve markers validation/history/cancel), native_media_drop (6 malformed/limit/busy/close dahil URI kabulü), native_media, native_data ve data_playlists. ROOT yeni prompt12 GUI testlerini ayrı kaynağa koyar; CMakeLists.txt bu ekip tarafından değiştirilmez. run_data_ui.py native testleri unique TemporaryDirectory içinde gerçek SDBASE/assets kopyasıyla çalıştırır. Parent build dizini PROJECT/build olarak sabittir; temp isimleri benzersizdir.

Mevcut native_controls playlist_session ilk `tree-view` bulgusunda aktif target tablosu bekler; iki tablo eklenirken eski aktif tablo widget sözleşmesi korunmalıdır. Mevcut native_media_drop global drop sonrasında media-library-dialog açılmasını bekler; yeni Playliste ekle eylemi ile bu regresyon kabulü arasındaki gerekli davranış/test değişikliği root tarafından değerlendirilir.

Test edilmeyen alanlar: gerçek kullanıcı hizmet hesabı/aboneliği ve fiziksel dış sağlayıcı playback. URL metadata saklamak analiz/indirilebilirlik/çalınabilirlik kanıtı değildir; bu kapsam mevcut provider entegrasyonlarıyla uyumlu değerlendirilecektir.
