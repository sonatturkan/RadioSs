# 6. aşama — C arayüzü ile kalıcı veri bağlantısı

RadioS 0.6.0, native C/GTK arayüzünden gerçek SDBASE servisini okur ve yazar. Normal açılış, projeye ait kimlik dosyası mevcutsa yerel servis yöneticisini çalıştırır; depo UUID'si ve şema 4 doğrulandıktan sonra istasyon ve kayıtlar yüklenir. Pencere kapanınca veri işleri iptal edilir; yerel servis yeniden açılış için çalışmaya devam eder. Kaynaklar, servis ve çalışma verileri RadioS klasöründedir.

## Native ekranların veri kapsamı

| Ekran | Kalıcı işlem | Veri sınırı |
|---|---|---|
| Ortak alanlar | Gerçek istasyon adı, veri bağlantısı ve yenileme zamanı | Ses/encoder/cihaz durumu veri servisi bağlantısından türetilmez |
| Yayın | Program/event metadata taslağı, event düzenleme/silme/sırası, oynatıcı sayısı; gerçek katalog sorgusu | Taslak event, onaylanmış yayın planı veya oynatma emri değildir |
| Playlist | Playlist oluşturma/adlandırma/silme; parça metadata ekleme/düzenleme/sıralama/kaldırma | Gerçek playlist/version UUID'leri; parça satırları doğrulanmamış metadata kayıtlarıdır |
| Otomasyon | Playlist UUID'leri, kaynak ağırlıkları, gün/saat ve kural tercihleri | Motor kuralları yürütmez; silinen playlist referansı doğrulamada reddedilir |
| DJ | Kanal sayısı, sampler pad başlığı ve dosya referansı | Dosya referansı analiz edilmiş medya veya çalınabilir cart sayılmaz; DSP sonraki aşamadadır |
| Acil | İki hazırlık sırası, ayarlar, iletişim/duyuru/anahtar/devir/tatbikat hazırlık satırları ve taslak günlüğü | Fiziksel devralma, yetki anahtarı veya gerçek acil yayın oluşturmaz |
| Reklam | Gerçek reklamveren, taslak kampanya/sözleşme ve taslak spot/version kayıtları; kuşak tercihleri | Doğrulanmış medya, kuşak yerleştirmesi veya yayın kanıtı oluşturmaz |
| Teknik yönetim | Sunucu ve web profili oluşturma/düzenleme; kullanıcı ve istasyon yetkilerini okuma; kullanıcı oluşturma | Kullanıcının özel anahtarı yerel özel dosyaya yazılır; vardiya/cihaz/program kısıtları aşama 13'tedir |
| Düzenleyiciler | Parça işareti ve geçiş alanları/dosya referansları/seviye tercihi | Doğrulanmış sürüm için onaylı marker set veya gerçek ses geçişi oluşturmaz |

Medya dosyası doğrulama/içe aktarma aşama 7; gerçek oynatma/çıkışlar aşama 8; mixer/DSP aşama 9; onaylı plan ve otomasyon yürütmesi aşama 10; reklam teslimi/kanıt aşama 11; acil devralma aşama 12 kapsamında kalır. Bir taslağın kalıcı olması bu işlemlerin çalıştığı anlamına gelmez.

## C istemcisi ve yaşam döngüsü

`src/core/data_client.c`, libcurl ve JSON-GLib ile kimlikli HTTP/JSON istekleri yapar. `include/radios/data_client.h` sonuç/bellek sahipliğini tanımlar. Kimlik dosyası özel izin, boyut, UTF-8, UUID, token ve proje içi DB yolu kontrollerinden geçer. Yapılandırılmış `RADIOS_SDBASE_TOKEN` ortam değişkeni kullanıcı tokenını değiştirebilir; token terminale, arayüze, hata metnine veya işlem argümanlarına yazılmaz.

HTTP yalnız loopback adresinde kabul edilir; HTTPS sertifikası doğrulanır. İstemci yönlendirme/proxy kullanmaz, istek adresini sınırlar ve yanıtı en çok 2 MiB kabul eder. Zaman aşımı, iptal, bozuk JSON, 403 yetki, 404 kayıt, 409 çakışma, 422 doğrulama, 429 sınır ve belirsiz tamamlanma ayrı hata türleridir. İstekler GUI iş parçacığında çalışmaz.

`src/ui/data.c`, mevcut sınırlı çalışma kuyruğuna değişmez JSON kopyaları teslim eder ve sonuçları GTK ana akışına döndürür. Kapanmış widget için callback çalıştırılmaz. Normal pencere veri bağlantısını açar; `--smoke-test` ve bağımsız eski GUI kontrol fixture'ı gerçek kullanıcı DB'sine bağlanmaz.

## Kayıt, yetki ve çakışma sözleşmesi

Hazırlık/tercihler `desktop_configurations`, doğrulanmamış playlist parça metadata'sı `desktop_playlist_entries` içinde typed şema ile tutulur. Gerçek playlist ve kampanyalar mevcut domain koleksiyonlarını kullanır. Yeni desktop işlemleri `desktop_api.py`, `desktop_playlist.py` ve `workflow_api.py` içindedir; istemciye ham koleksiyon yazma yetkisi verilmez.

Yazılar istasyon/kullanıcı yetkisi, UUID, alan doğrulaması ve `_rev` üzerinden CAS kontrolü kullanır. İlgili parent/version/parça değişimleri aynı transaction içindedir. İstek UUID'si ve istek özetiyle değişmez `desktop_mutations` alındısı tutulur; aynı işlem yeniden teslim edildiğinde ikinci kayıt oluşturulmaz. Aynı anahtarın farklı içerikle kullanımı reddedilir.

Başarısız kayıtta form girdileri korunur. Yerel tercih çakışması, varsayılan seçimi girdileri koruyan bir native pencereyle gösterilir. Kullanıcı güncel kaydı yüklemeyi seçerse kaydedilmemiş alanlar bırakılır. Sonucu belirsiz generic belge yazısında aynı içeriğin kullanıcı tarafından tekrar kaydı aynı işlem anahtarını kullanır; farklı içerikle devam etmeden önce durumun yeniden yüklenmesi istenir. Arka planda kör yazı tekrarı yapılmaz. Kaydedilmemiş düzenleme elle yenilemeyle ezilmez; uçuşta olan okuma mevcut düzenlemenin CAS tabanını değiştirmez.

Yeni kullanıcının anahtarı DB kökünde `private-user-keys/<user_uuid>.json` dosyasına, özel `0700` dizin ve `0600` dosya izniyle yazılır. HTTP yanıtı kaybolduğunda aynı kullanıcı UUID'si ve dosya yolu alındıdan bulunur. Tokenın kendisi audit/alındı veya GUI metnine kaydedilmez. Tamamlanmamış kullanıcı transaction'ının anahtar dosyası temizlenir.

## Derleme ve kabul

C11/CMake/GTK/GLib/Fontconfig bağımlılıklarına JSON-GLib 1.10+ ve libcurl 7.85+ geliştirme paketleri eklendi. Gerçek ekran oturumunda:

```sh
cmake -S RadioS -B RadioS/build -DCMAKE_BUILD_TYPE=Debug -DRADIOS_GUI_TESTS=ON
cmake --build RadioS/build --parallel
ctest --test-dir RadioS/build --output-on-failure
./RadioS/run-radios.sh
```

CTest toplam 12 hedef içerir. `data_client` gerçek loopback fixture ile istemci/kimlik/hata sözleşmelerini; `desktop_api` typed config, yetki, CAS, kampanya ve özel kullanıcı anahtarını; `data_playlists` gerçek HTTP üzerinden playlist CRUD, sıra, alındı ve yeniden açılışı doğrular. `native_data`, `build/` altındaki ayrı gerçek SDBASE servisiyle native form/kalıcılık, çakışma, kesinti ve kapanış kabulünü çalıştırır. Üretim DB'sine örnek test kaydı eklenmez. Mevcut config/runtime/tercih/native ekran/kurulum kurtarma kontrolleri korunmuştur.

Güncel API envanteri 178 workflow, 228 HTTP yolu ve 233 HTTP işlemi içerir. Aşama 1/2 belgelerindeki sayılar ilk kaynak aktarımının tarihsel kaydıdır; güncel sözleşme `sdbase-openapi.json` ve `islev-envanteri.json` içindedir.

Son kabul: strict C11 derlemesi ve CTest 12/12 başarılı. Native veri kabulü 6/6 grupta tüm profil kayıtlarını kapatıp açarak, DJ’de eklenen 9. kanalı, acil iletişim satırını, özel kullanıcı anahtarı/izinlerini ve eşzamanlı yenileme-düzenleme CAS yarışını doğruladı. Backend typed API 11/11, playlist HTTP 8/8, C istemcisi 9/9; mevcut motor 19/19 ve sorgu/bütünlük 33/33 başarılı. Gerçek kullanıcı deposu aynı UUID ve sequence 2 ile korundu; test kayıtları ayrı fixture’larda kaldı.

Bellek denetimi: native veri kabulünün 6/6 grubu ASan+UBSan ile hatasız geçti (`halt_on_error=1`, GTK uyarıları hata). GTK süreç global verileri nedeniyle bu GUI koşusunda LeakSanitizer kapalıydı; bağımsız C veri istemcisinin 9/9 grubu leak detection açıkken temiz geçti.
