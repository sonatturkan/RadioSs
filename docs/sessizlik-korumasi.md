# Sessizlik koruması (ölü hava dedektörü)

17.10.2026. Tasarımdaki **Acil › Sessizlikte otomatik devralma** bölümü (AC-09) o güne kadar
yalnız alan taslağıydı; arayüz ayarları kaydediyor, ama hiçbir şey MAIN çıkışını dinlemiyordu.
Profesyonel otomasyon sistemlerinin "silence sensor" standardı uygulandı: çıkış sürekli
ölçülür, eşik altı sessizlik belirlenen süreyi aşınca basamaklı müdahale yapılır, olay kaydı
tutulur ve ses dönünce olay kapanır.

## Nasıl çalışır

- **Ölçüm:** uygulama penceresi açıkken her 100 ms'de ses motorunun MAIN çıkış seviyesi okunur
  (`src/ui/silence_guard.c`). "Ölçüm" alanına göre RMS ya da tepe (dBFS). Ses çıkışı bağlı
  değilse (aygıt yok, sunucu kesildi) ölçüm "yok" sayılır; saat durur, yanlış alarm verilmez.
- **Sessizlik:** "Sessiz sayılması · İki kanal da" seçiliyken iki kanal da eşiğin altındaysa
  sessizdir; "Herhangi bir kanal"da tek kanalın susması yeter.
- **1. basamak:** sessizlik "Süre · sn" boyunca sürerse "1 · Süre dolunca" işlemi çalışır:
  *Yalnız alarm* (bildirim + kayıt), *İkinci deck · yedek oynatıcı* (etkin AutoDJ/otomasyon
  sırasında sıradaki parçaya geçilir; sıra yoksa nedeni yazılır), *Acil liste* (kayıtlı acil
  hazırlık belgesiyle `radios_broadcast_ui_emergency_start`; "Devralma davranışı" alanı
  stratejiyi belirler; belge kaydedilmemişse nedeni yazılır).
- **2. basamak:** sessizlik "2 · Sürerse · sn" kadar daha sürerse "Sonra" işlemi çalışır
  (*Acil liste*, *Yalnız alarm* ya da *Ek işlem yok*). Her basamak olay başına bir kez.
- **Toparlanma:** seviye "Ses dönünce eşik · dB" değerinin üstünde "Dönüş süresi · sn" kadar
  kesintisiz kalınca olay kapanır (histerezis: dönüş eşiği sessizlik eşiğinden yüksek olmalı).
  Acil devralma başlatıldıysa normale dönüş operatör kontrolündedir (tasarım AC-08).
- **Kayıt:** her basamak ve toparlanma `diagnostics/record` iş akışıyla olay/incident olarak
  yazılır (bileşen `output`, kod `silence` / `silence-step2` / `silence-recovered`; aynı
  olayın kayıtları ortak `correlation_key` taşır). Kullanıcının `repair` yetkisi yoksa kayıt
  bir kez reddedilir, bildirimler sürer. Her basamak üst çubuk bildirimine de düşer.
- **Ayar kaynağı:** Acil sayfasının sessizlik alanları (`emergency` belgesinin `preparation`
  haritası, `silence/<alan>` anahtarları). Belge kaydedilince ve veri görünümleri yenilenince
  ayarlar yeniden okunur; ayrıca dakikada bir. Alan değerleri Türkçe ondalık (−45,5) kabul
  eder; geçersiz değer korumayı kapatır ve nedenini sessizlik etiketi ile bildirimde yazar.
- **İzlenen nokta:** yalnız "Program çıkışı" (MAIN) bu bilgisayardan ölçülür; "İnternet
  dönüşü / Karasal / Stüdyo dönüşü" seçiliyse koruma bekler ve bunu etikette söyler.
- **Gösterim:** Acil › sessizlik sekmesindeki ölçer canlı seviyeyi, altındaki etiket
  "−48,2 / −47,9 dB · ses var · eşik −50 dB, 20 sn" biçiminde durumu gösterir.

Varsayılanlar (alan boşsa): eşik −50 dB, süre 20 sn, yalnız alarm, dönüş eşiği −40 dB,
dönüş süresi 5 sn, iki kanal da, RMS. Sınırlar: eşik −120…0 dB, süre 1–3600 sn, ikinci
basamak 0–3600 sn, dönüş süresi 0–600 sn.

## Kaynaklar

- `src/core/silence_detector.c` — ayar ayrıştırma/doğrulama ve durum makinesi (saf C).
- `src/ui/silence_guard.c` — ölçüm döngüsü, basamak işlemleri, tanı kaydı, Acil sayfası etiketi.
- `src/ui/window.c` — `radios_silence_guard_bind` pencere ömrü boyunca.
- `src/ui/emergency.c` — ölçer ve etiket adları (`em-silence-meter`, `em-silence-level`).

## Doğrulama

- `silence_detector` (çekirdek, 3 vaka): varsayılanlar, Türkçe ondalık, geçersiz alanlar;
  basamak zamanlaması, histerezis, ikinci olay; kanal kuralları, ölçülemeyen çıkış, kapatma.
- `native_silence_guard` (GTK, sahte seviye kaynağı): 1 sn sonra 1. basamak, 1 sn sonra 2.
  basamak (kayıtsız acil liste nedeni), tekrar yok, toparlanma, motor yokken ikinci deck
  nedeni, ölçülemeyen çıkışta ve kapalı ayarda alarm yok, geçersiz alanda kapatma.
