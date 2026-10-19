## ACİL PROMPT1/preferences.c View paneline NULL guard isteği

Root2 gerçek kaynakta PROMPT1 header bind/View panel/save-close mask merge entegrasyonunu teyit etti. İşbirliği ve bu küçük bölüm entegrasyonu tamamdır; PROMPT-2 header.c/window.c yazmaz.

Bununla birlikte `radios_header_status_preferences_new(ctx)` **bound header yoksa NULL döner**. `preferences.c` View index2 içinde `pack(page, radios_header_status_preferences_new(ctx), FALSE)` doğrudan çağrı, ana header olmayan Preferences unit fixtures'da NULL pack ve fatal GTK warning üretir. **PROMPT1/preferences sahibi lütfen kendi bölümünde şu minimal guard'ı eklesin:** önce `GtkWidget *panel = radios_header_status_preferences_new(ctx);`, ardından yalnız `if (panel) pack(page, panel, FALSE);`. Bound header olmayan durumda disabled/fake kontroller üretilmeyecektir; production header hazırken normal gerçek panel döner. Sonuç/strict veya fixture testini ortak kayda bildirin.

## Yeni gerçek test sonucu

Root yeni sekiz header test case'ini GNU strict compile + gerçek GTK yürütme ile **8/8 PASS** raporladı. İlk 6/6 sonucu artık genişletilmiş suite tarafından kapsanır. `test_ui_preferences` 11/11 ve CTest playout önceki PASS sonuçları geçerlidir. Sekizli test readonly observation stubs kullanır; gerçek aggregate data path kabulü ayrıca bekler.

## Aggregate kalite denetimi ve açık düzeltmeler

- Architecture 02:01:41 salt okunur review: gerçek RAW engine gate/ready/output_bus/mapping filtresi CUE yanlış etiketi riskini çözer; flow programme shape/owned clone ve weak card/GTK lifetime güvenli.
- Kesin bug: USER_FLOW event `online_id` taşırken `item_video` yalnız katalog id alanlarını okuyordu, currentFALSE oluşabiliyordu. Ayrıca preparing sırasında program etiketi ve paused kaynakların active_sources sayımına katılması; seçili private kaynak değişince eski programme adı taşınması incelendi.
- Root online_id eşlemesini, preparing/paused sayımı ve aggregate QA düzeltmelerini uyguluyor. Bu delta henüz gerçek aggregate senaryosunda PASS sayılmaz; root test sonucu beklenir.
- Tamamlandı: ilk eight-case GTK suite PASS; root1 gerçek kaynak entegrasyon teyidi. Bekleyen: PROMPT1 NULL guard; aggregate gerçek data path; tam build/link ve native/görsel kabul.
