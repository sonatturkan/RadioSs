# Veritabanı kaynakları

`SDBASE/` mevcut veritabanı motorunun bütün kaynak, belge ve operasyon araçlarının proje içindeki kopyasıdır. İlk aktarım 0.8.1 kaynak durumundan byte/SHA-256 doğrulamasıyla yapıldı; köken kaydı `docs/source-import-manifest.json` içindedir.

Motor Python 3.11+ kullanır; çalışma zamanı üçüncü taraf paket bağımlılığı bildirmez. Paket derleme gereksinimi `setuptools>=68` olarak tanımlıdır. POSIX `fcntl` kullanımı nedeniyle mevcut kaynaklar Linux ortamını hedefler. C uygulaması bu veri motoruna bağlanır; motor C'ye çevrilmiş değildir.

Bu klasör kaynak kod içindir. Çalışma veritabanı `data/database/RadioS.sdb` içinde şema 4 ile oluşturuldu. İlk kullanıcı/istasyon/offline uç ve özel kimlik dosyası kuruldu; `run-database.sh` yerel servis ve ilk yedeği yönetir. [5. aşama belgesi](../docs/05-veritabani-kurulumu.md) kurulumu ve kabulü açıklar. [6. aşama](../docs/06-c-veri-baglantisi.md) C ekranlarının gerçek veri bağlantısını uygular; uç günlüğü/ses yürütücüsü henüz başlatılmaz.

İlk aktarım kaynağında ayrı LICENSE/COPYING dosyası veya pyproject license alanı bulunmadı; mevcut olmayan bir lisans eklenmedi. Kaynağın kendi README, durum raporu, pyproject ve gizli `.gitignore` dosyaları korundu.

`desktop_api.py` ve `desktop_playlist.py`, native tercih/hazırlık işlemlerini typed şema, istasyon yetkisi, CAS ve transaction alındılarıyla genişletir. İlk kaynak aktarım manifesti tarihsel köken kaydı olarak korunur; güncel API/kaynak özetleri işlev envanterinde yeniden üretilir.
