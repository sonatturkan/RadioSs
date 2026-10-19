# Ayarlar › Medya arşivi

17.10.2026. Çekirdek modüller (`src/core/media_archive.c`, `src/core/storage_locations.c`,
`src/core/local_ini.c`) aynı gün yazılmış, `config.c` ve veri istemcisine bağlanmıştı; bu
belgeyle birlikte arayüz sayfası (`src/ui/media_archive_ui.c`) ve kütüphane bağlantısı
(`radios_media_import_files`) eklendi. Sayfa **Ayarlar › Medya arşivi** altındadır.

## Arşiv klasörleri (okunan yer)

- **Klasör ekle…** ile en çok 16 klasör seçilir; klasörler iç içe olamaz. Harici disk ve ağ
  klasörü kullanılabilir. Takılı olmayan klasör listede "Bulunamadı (disk takılı mı?)" diye
  görünür, taramada atlanır. Ayarlar `config/media-archive.ini` dosyasında (0600, atomik
  yazım) saklanır ve her değişiklikte kaydedilir.
- **Şimdi tara ve içe aktar**: arka plan iş parçacığı klasörleri (16 seviye, en çok 50.000
  dosya, sembolik bağlantılar izlenmez, gizli girdiler atlanır) dolaşır ve WAV/MP3/FLAC/
  Ogg/Opus/M4A/AAC dosyalarının SHA-256'sını hesaplar. Özetler `data/cache/media-archive-index.json`
  önbelleğinde tutulur; boyutu ve değişiklik zamanı aynı kalan dosya yeniden okunmaz.
  Ardından medya kütüphanesindeki bütün kayıtların özetleri okunur; kütüphanede olan ya da
  arşivin içinde tekrarlanan içerik atlanır. Kalan dosyalar medya kütüphanesinin kendi sıralı
  içe aktarmasından (analiz, yönetilen kopya, dayanıklı alındı) geçer; kaynak dosyalara
  dokunulmaz.
- **Yeni dosyaların türü**: içe aktarılan kayıtların kütüphane türü (müzik, reklam, jingle…).
- **Her alt klasör için aynı adlı playlist**: arşiv klasörünün hemen altındaki klasör adıyla
  playlist aranır (büyük/küçük harf duyarsız), yoksa türe uygun amaçla oluşturulur; o
  klasördeki (alt klasörler dahil) yeni dosyalar doğrulandıkça playliste eklenir. Üst düzeydeki
  dosyalar yalnız kütüphaneye girer.
- **RadioS açıkken yeni dosyaları izle**: 5–1440 dakikada bir, veri servisi bağlıyken ve
  kütüphane boşken aynı tarama kendiliğinden çalışır. Ayarlar sayfası ile zamanlayıcı aynı
  yürütücüyü paylaşır; iki tarama üst üste binmez. Operatörün açık içe aktarması veya seçim
  penceresi varsa arşiv içe aktarması 3 saniyede bir yeniden dener.
- **Durdur** taramayı hemen, içe aktarmayı o anki dosyadan sonra keser; tamamlanmış kayıtlar
  kalır. Sonuç satırı dosya/yeni/içe aktarılan/doğrulanamayan/kopya sayılarını ve okunamayan
  klasörleri yazar; aynı özet bildirim alanına da düşer.

## Radyo verileri (yazılan yer)

- **Kayıtlar (WAV)**: "Kayıt klasörünü seç…" seçilen klasörü hazırlar (`.radios-storage.json`
  işareti yazar) ve `config/storage.ini` içine `recording_root` olarak kaydeder. Yayın kaydı ve
  ses yönlendirme kaydedicisi bu klasörü RadioS yeniden başlatılınca kullanır; "Şimdi yeniden
  başlat" düğmesi onay sorar. İşareti olmayan (takılı olmayan) disk açılışta varsayılan
  klasöre düşer ve sayfada uyarı görünür. "Varsayılana dön" kaydı siler.
- **Yedekler** ve **Veritabanı ve yönetilen medya** da aynı biçimde seçilir (klasör,
  kullanılan alan, diskte boş/toplam; **Seç…** ve **Varsayılana dön**). Veri servisi
  başlatıcısı (`tools/radios_database.py`) `storage.ini` içindeki `database_dir`'ı aynı
  kurallarla okur ve bir sonraki başlatmada veritabanını yeni klasöre taşır; `backup_root`
  sıfırlama yedekleri için hemen geçerlidir (ayrıntı: "Veri servisi tarafı" bölümü).
  Yedek aracı (`tools/radios_reliability.py`) hâlâ `config/radios.ini` klasörünü kullanır.

## Doğrulama

- `media_archive` (çekirdek, 5 vaka): ayar doğrulama/kayıt, uzantı ve amaç eşlemesi, klasör
  durumu, tarama (gizli/bağlantı/eksik klasör, sıralama, grup, SHA-256, önbellekten yeniden
  kullanım, değişen dosya, iptal, yalnız eksik klasör hatası, boş liste).
- `storage_locations` (çekirdek, 5 vaka): yol söz dizimi, boyut biçimi, klasör kuralları ve
  işaret dosyası, `storage.ini` kaydı ve `radios_storage_apply`, ağaç/disk boyutları.
- `native_preferences`: yeni kategori ve sayfa bağlantısız açılır; izleme kutusu dosyaya yazar.
- `native_media_archive` (özel veri servisi, gerçek içe aktarma): üç dosyalı arşivden iki yeni
  kayıt + "Jingle Seti" playlisti; ikinci taramada tek yeni dosya aynı playliste eklenir;
  üçüncü tarama "0 yeni"; bağlantısız sayfa taramayı kapalı tutar.

## Veri servisi tarafı (`config/storage.ini`)

Aynı gün `tools/radios_database.py` de `storage.ini` dosyasını C tarafıyla aynı kurallarla
(aynı dosya, anahtarlar, yol söz dizimi ve `.radios-storage.json` işaret kuralı) okumaya başladı
(`load_storage_settings`, `storage_directory_check`, `effective_config`):

- `start`, `serve`, `status`, `check`, `stop`, `backup` komutları ve başlatıcının doğrudan
  çağırdığı `start_service` etkin yapılandırmayı kullanır; `init` her zaman proje klasörüne kurar.
- `database_dir`: işareti olmayan ya da ulaşılamayan klasör hatadır; eski bir kopyayla başlayıp
  istasyon verisini bölmemek için servis açılmaz (diski takın ya da `storage.ini` dosyasını
  kaldırın). Taşınan veritabanı için `credentials.json` içindeki proje göreli yol dosya adıyla
  eşleştirilir (C istemcisinin `database_external` kuralı) ve kimlik/anahtarlar taşınan deponun
  kendisinde doğrulanır. Seçilen klasörde veritabanı yoksa `start`, son kullanılan klasörden
  (`data/database-service/storage-last.json`) ya da proje varsayılanından `RadioS.sdb`, yan
  dosyalarını ve `RadioS.sdb.objects` yönetilen medya ağacını bir kez kopyalar; günlük dosyası en
  son yayınlanır, kesilen kopya açılmaz, kaynak kopya yerinde kalır. Eski klasörde hâlâ çalışan
  kendi servisi önce kapatılır (`status` bunu `stale_database` ile gösterir).
- `backup_root`: işaretli klasör varsa sıfırlama yedekleri oraya yazılır, yoksa varsayılan
  (`radios.ini › backup_root`) kullanılır. Servis depoya `desktop_backup_root` çözücüsü verir;
  `desktop_reset` her yedekte klasörü yeniden okur, bu yüzden değişiklik yeniden başlatma
  gerektirmez. `tools/radios_reliability.py` yedek listesi/geri yükleme hâlâ `radios.ini`
  klasörünü kullanır.

Bu değişiklikle yedek ve veritabanı klasörü de sayfadan seçilir: üç rolün her birinde
**Seç…** ve **Varsayılana dön** vardır; veritabanı ve kayıt klasörü değişikliği yeniden
başlatmayla, yedek klasörü hemen geçerli olur. Bilinen sınır: hem eski hem yeni klasörde
veritabanı varsa etkin olan açılır, kopyalar birleştirilmez.

- `local_ini` (çekirdek, 6 vaka): `<proje>/config` içindeki yerel INI dosyalarının kayıp
  dosya, atomik değiştirme ve 0600, ad/boyut sınırı, UTF-8, bağlantı reddi ve `config`
  kaçış denetimi.
- `storage_locations_service` (Python, 10 vaka, geçici proje kökü): `storage.ini` çözümleme,
  yol kuralı ve GKeyFile kaçışları, işaret ve gerçek yol kuralı, yedek klasörü geri düşmesi,
  `desktop_reset` yedeğinin klasörü, tek seferlik taşıma, bayat veritabanındaki kendi servisin
  tanınması ve taşınan depoda kimlik doğrulama.
