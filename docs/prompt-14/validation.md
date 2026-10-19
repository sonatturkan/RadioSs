# Prompt 14 uygulama ve doğrulama sonucu

2026-10-09 02:07:37 UTC nihai koşusu başarılıdır. `prompt/14/14.pdf` dosyasının tek sayfası ve Şema 1 eksiksiz incelendi. Sağlayıcı giriş penceresinde Spotify, Apple Music ve YouTube Music sekmelerinin her birinde yalnız **Giriş yap** vardır. Önceki hesap bilgileri, tarayıcı seçimi, bağlantı seçenekleri, playlistler, profil işlemleri ve diğer içerikler bu pencereden kaldırıldı.

Değişen kaynaklar: `src/ui/online_services.c` sağlayıcı giriş penceresi, `include/radios/online_services_ui.h` API açıklaması ve `tests/test_online_services_ui.c` ilgili kabul/regresyonları. Ayrı teknik yönetim playlist çalışma sayfası PDF kapsamı dışındadır; mevcut regresyonları korunarak doğrulandı.

## Kanıt

- Strict C11 / `-Wall -Wextra -Wpedantic -Werror` ile güncel kaynaklardan uygulama ve iki test hedefi derlendi: [derleme sonucu](final-current-build-result.json), [tam çıktı](final-current-build.log).
- İki native CTest grubu başarılıdır: **7 giriş/entegrasyon kontrolü + 5 YouTube Music regresyonu = 12/12**; toplam CTest süresi **7,54 saniye**. [Koşu sonucu](final-current-native-result.json), [JUnit ve bütün TAP kontrolleri](final-current-native.xml).
- Gerçek yayın katalog düğmesinin callback'i sade giriş penceresini açtı. Sağlayıcı başlıkları, tek düğme, başka içerik bulunmaması, doğru resmi URI, tarayıcı hatası, singleton, kapatma/yeniden açma ve parent yok edilmesi doğrulandı.
- Üç sekme görüntüsü bağımsız incelendi: [Spotify](reference/login/capture-spotify.png), [Apple Music](reference/login/capture-apple_music.png), [YouTube Music](reference/login/capture-youtube.png).
- Gereksinim ve kaynak eşlemesi [requirements.md](requirements.md); ayrıntılı test incelemesi [release-audit.md](release-audit.md); dosya özetleri ve makinece okunabilir son durum [validation.json](validation.json) içindedir.

## Kayıt ve sınırlar

Bu PDF kapsamındaki bekleyen gereksinim yoktur. İlk GTK içerik sayımı testi boş internal action-area'yı yanlışlıkla kullanıcı içeriği saydığı için başarısız oldu; kontrol, yalnız boş GTK container kabuğuna izin verirken diğer görünür/gizli içerikleri reddedecek şekilde düzeltildi. Başarısız koşu ve eşzamanlı diğer ekiplerin önceki geçici derleme hataları silinmedi; `build-prompt-14/evidence/` ve ortak koordinasyon günlüğünde korunur.

URI testleri kullanıcının tarayıcısını açmayan wrapper kullanır; gerçek harici hesap yetkilendirmesi sınanmış sayılmaz. Görüntüler uygulama CSS provider'ını yüklemeyen GTK harness'inden gelir; yapı kanıtıdır. Genel proje testlerinin tamamının çalıştığı iddia edilmez. Son derlenmiş uygulama `build-prompt-14/radios` içindedir.

Çalışma azami dört eşzamanlı ajanla yürütüldü. UTC saatlik ve olay bazlı ortak kayıt `prompt/agent/AGENT.md` içindeki PROMPT14 bölümünde, ayrıntılı olaylar `prompt/agent/prompt14/logs/` altında; bu projenin devam özeti [coordination.md](coordination.md) içinde tutulur.
