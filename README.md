# RadioS Windows taşıması — çalışma notları (kod dalı değil)

Bu dal yalnız taşıma işinin çalışma notlarını taşır; uygulama kodu `windows-port` dalındadır.

- `specs/` — Windows PC'deki oturumlarda yazılan şartnameler (DECISIONS.md bağlayıcı kararlar,
  PLATFORM_FACTS.md ölçülmüş gerçekler, alan şartnameleri) + bulut ortamı belgesi ENV_CLOUD.md.
  PC oturumunun geçici klasöründe kalmışlardı; Claude Code yedeğindeki kayıtlardan birebir geri kuruldu.
- `reports/` — ajan raporları (understand: analiz, foundation-a: platform/WASAPI/MIDI uygulama raporları ve sonrası).
- `workflows/pc-sessions` — PC oturumlarındaki iş akışı betikleri; `workflows/cloud` — bulut oturumundakiler.
- `cloud-env/` — bulut kapsayıcısında aynı MSYS2 UCRT64 araç zinciriyle (GCC 16, GTK 3.24.52, GLib 2.90.1,
  FFmpeg 9.0.2) Wine altında derleme/test ortamını yeniden kuran betikler (`setup/setup-cloud-env.sh`).
  Bunlar yalnız doğrulama ortamıdır; ürünle birlikte gönderilmez.
