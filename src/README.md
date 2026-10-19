# C uygulama kaynakları

Bu dizin RadioS'un yerel C masaüstü programıdır. `app/main.c`, core yapılandırma/kuyruk/denetim kaynakları, `ui/window.c` ve tasarımın ortak başlık/yedi görünüm/native kontrol kaynakları uygulanmıştır. C veri istemcisi core/data_client.c, GTK veri koordinasyonu ui/data.c içindedir. Diğer motor modülleri ilgili aşamalarda doldurulacaktır. HTML/JavaScript bu kaynak dizininde çalıştırılmaz; CSS kaynakları derleme sırasında native stile aktarılır.

| Dizin | Sorumluluk |
|---|---|
| `app/` | Uygulama başlatma/kapatma, ana akış ve çalışma bağlamı |
| `core/` | Zaman, iş/durum kuyrukları, hata ve günlükleme |
| `ui/` | Native pencere, yedi profil, kontroller ve çizim |
| `audio/` | Gerçek ses motoru, deck, mixer, routing ve DSP |
| `media/` | Dosya inceleme, decoder koordinasyonu, waveform ve cue modeli |
| `db/` | Gelecek veri adaptörleri; mevcut istemci core/data_client.c ve ui/data.c içindedir |
| `automation/` | Program, playlist, takvim ve otomasyon koordinasyonu |
| `traffic/` | Kampanya, spot, reklam kuşağı ve mutabakat |
| `emergency/` | Acil akış, sessizlik ve devralma durumları |
| `monitoring/` | Motor/cihaz/çıkış gözlemi ve teşhis |
| `integrations/` | Stream, kayıt, metadata, MIDI/GPIO ve dış servis adaptörleri |

Modüllerin dışarı açılan C başlıkları `include/radios/` altındadır. Henüz boş olan modül dizinleri uygulanmış işlev sayılmaz.
