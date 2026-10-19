# Native arayüz kaynakları

`fonts/`, `icons/` ve `themes/` RadioS'un C arayüz kaynaklarına ayrılmıştır. Tasarımın HTML/CSS/JS kopyası burada çalıştırılmaz; görsel referans `design/reference/tasarrım/` altındadır.

`fonts/InterVariable.woff2` tasarımdan lisans dosyasıyla birlikte alınmıştır. Fontconfig bu WOFF2 kaynağını native uygulamaya doğrudan yükler; mevcut ortamda font okuma ve pencere açılışı doğrulandı. Fontla birlikte `fonts/LICENSE.txt` korunur. `themes/radios.css`, build sırasında kaynak tasarım CSS'lerinden üretilen kuralları GTK düğüm/yerleşim ayrıntıları için tamamlar. Analog saat, knob ve ölçerler C/Cairo ile çizilir.
