# Çalışma verileri

Bu dizin programın kalıcı çalışma verileri içindir. Aşama 5'te gerçek SDBASE veritabanı, ilk kurulum yedeği, servis durumu ve günlükleri oluşturuldu. Örnek medya veya yayın kaydı eklenmedi.

| Dizin / yol | Kullanım |
|---|---|
| `database/RadioS.sdb` | Şema 4 ile kurulmuş kalıcı SDBASE merkez veritabanı |
| `database/private-user-keys/<user_uuid>.json` | Native yönetimden oluşturulan kullanıcının özel anahtar dosyası; 0700 dizin/0600 dosya |
| `database/RadioS.sdb.meta`, `.checkpoint`, `.lock` | DB motorunun aynı ada bağlı yan dosyaları |
| `database/RadioS.sdb.objects/` | Motorun SHA-256 ile yönettiği merkez medya nesneleri; DB adı üzerinden otomatik türetilir |
| `database-service/` | Özel servis kilitleri/durumu ve gerekirse kesilen ilk kurulum hazırlığı |
| `endpoints/<endpoint_uuid>/` | İstasyona/uç kimliğine bağlı tek yazıcılı uç günlüğü, önbellek ve staging |
| `imports/` | İçe alma öncesi dosya hazırlığı; onaylı yönetilen medya deposu değildir |
| `cache/` | Yeniden üretilebilir analiz/waveform önbelleği |
| `logs/` | Uygulama ve teşhis günlükleri |
| `recordings/` | Gerçek kayıt motorunun üreteceği ses dosyaları |
| `backups/` | DB, yönetilen medya ve gerekli geçmiş dosyalarının doğrulanmış yedekleri |

SDBASE merkez medya yolu bağımsız bir ayar değildir: `data/database/RadioS.sdb` dosyası için `data/database/RadioS.sdb.objects/` türetilir. Bu ilişki `database/SDBASE/src/sdbase/media.py` içinde tanımlıdır. DB yan dosyaları ve nesneler kendi bakım/yedek yordamlarıyla yönetilir; klasörü farklı amaçla kullanmak veri sözleşmesini bozabilir.

Uç dizini merkezi veritabanından ayrıdır. İlk yayın ucunun gerçek UUID'si ve ayrı tokenı kuruldu; uç offline durumundadır. Uç ajanı çalışmaya başladığında kendi dizininde `Endpoint.sdb`, `owner.lock`, `objects/` ve `staging/` oluşturacaktır. İlk kurulum fiziksel ses motorunu başlatmaz.

Çalışma verileri `.gitignore` ile kaynak takibinden ayrılır. Yalnız bu açıklama ve boş dizinleri koruyan `.gitkeep` dosyaları kaynak düzene dahildir.
