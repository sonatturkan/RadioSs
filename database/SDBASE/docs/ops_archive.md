# İşlem kanıtı arşivi ve sağlık ölçümü

SDBASE, her kalıcı işlem için `_audit` içinde sıra numarası, işlem kimliği,
zaman, aktör ve değişen belgelerin önce/sonra değerlerini tutar. `ops_archive.py`
bu kayıtları art arda gelen, salt okunur JSONL arşiv bölümlerine kopyalar.
Kaynak `.sdb` kayıtlarını **silmez**. Yedekleme de ayrıca gereklidir: arşiv,
ses dosyalarını, mevcut belge durumunu veya uç bilgisayarın `Endpoint.sdb`
günlüğünü içermez.

Bu tasarım [NIST SP 800-92'nin resmî log yönetimi rehberindeki](https://csrc.nist.gov/pubs/sp/800/92/final)
uzun süreli saklama, erişilebilir format ve aktarılmış kayıt bütünlüğünü
doğrulama önerilerini izler. Her bölümde kanonik JSONL dosyasının SHA-256
özeti, sıra aralığı, önceki manifest özeti ve kayıtlar boyunca ilerleyen hash
zinciri saklanır. İlk bölüm sıra 1'den başlar; sonraki bölüm için doğrulanmış
önceki dizin gerekir.

## Yerel operatör komutları

Komutları `SDBASE` dizininden çalıştırın. Hedef dizin önceden var olmamalıdır.
Yayınlanan dizin `0700`, içindeki dosyalar `0600` izinleriyle oluşturulur.

```sh
python3 tools/ops_archive.py create \
  --db /veri/RadioS.sdb --destination /arsiv/000001

python3 tools/ops_archive.py create \
  --db /veri/RadioS.sdb --previous /arsiv/000001 \
  --destination /arsiv/000002

python3 tools/ops_archive.py verify-chain /arsiv/000001 /arsiv/000002

python3 tools/ops_archive.py query --archive /arsiv/000002 \
  --previous /arsiv/000001 --collection playout_events --limit 100

python3 tools/ops_archive.py reconstruct \
  --archives /arsiv/000001 /arsiv/000002 \
  --collection items --record-id 00000000-0000-0000-0000-000000000001

python3 tools/ops_archive.py health --db /veri/RadioS.sdb \
  --archives /arsiv/000001 /arsiv/000002 \
  --max-archive-backlog-commits 1000 --min-free-bytes 10737418240 \
  --critical-free-bytes 2147483648 --max-journal-bytes 2147483648
```

`create` çıktısındaki `has_more` doğruysa sonraki bölümü oluşturun. Her
bölümde varsayılan üst sınır 10.000 işlem ve 64 MiB'dir; `--max-records`
ve `--max-bytes` kurulum ihtiyacına göre değiştirilebilir. Bir kayıt belirlenen
byte sınırından büyükse işlem hata verir, eksik bölüm yayınlanmaz. İstenirse
`--through-sequence` ile önceden belirlenen bir üst sıraya kadar çalışılabilir.
Arşivden geri okuma, önce bütün bölümün dosya ve kayıt zincirini doğrular;
`--record-id`, `--first-sequence`, `--last-sequence` ve `--limit` sorguyu daraltır.
`reconstruct` tüm arşiv zincirini baştan doğrular ve bir belgenin arşivlenmiş
son hâlini veya `--through-sequence` anındaki hâlini önce/sonra revizyonlarını
karşılaştırarak oluşturur. Bu okuma canlı veritabanına kayıt yazmaz.
İşlem değişikliklerinin içinde kişisel veya gizli metadata bulunabilir;
arşiv ve sorgu çıktıları yetkili operatörlerle sınırlandırılmalıdır.

`health` mevcut sıra, `_audit` sıra bütünlüğü, seçilmiş kanıt koleksiyonlarının
kayıt sayısı, jurnal/checkpoint boyutları, boş disk alanı, beklenen yönetilen
medya byte sayısı ve doğrulanmış arşiv kapsama sınırını döndürür. Eşikler
kuruluma özeldir. Eşik verilmediğinde `observed` sonucu bir üretim sağlık
sertifikası değildir. `_audit` taraması bütçeyi aşarsa bütünlük `unknown`
kalır. Beklenen medya byte sayısı diskteki gerçek nesneleri tek tek taramaz;
medya nesnesi bütünlüğü için `backup-verify` veya medya uzlaştırması gerekir.

## İşletim sınırları

- SHA-256 ve zincir kazara bozulmayı veya tek dosyanın değiştirilmesini yakalar.
  Saldırgan arşivle birlikte bütün manifestleri yeniden yazabiliyorsa bu
  mekanizma tek başına kimlik doğrulaması sağlamaz. Son manifest özetini
  ayrı güvenilir ortamda saklayın; gerektiğinde imzalı veya tek yazımlık
  depolama politikası uygulayın.
- Bölümler canlı motorun disk tüketimini azaltmaz. `_audit` ve kanıt
  koleksiyonlarının fiziksel tutulması sürer. Kaynak geçmişini atarak yer
  açan bir mekanizma eklenmedi; böyle bir işlem ancak saklama yükümlülüğü,
  arşiv geri okuma ve geri yükleme yordamı tanımlanıp ayrı kabul edildikten
  sonra tasarlanabilir.
- Komutlar operatörce, dış zamanlayıcıyla veya [SDBASE periyodik işletim aracıyla](ops_scheduler.md)
  çağrılır. Gerçek ses çıkışı sağlık sensörü, hukuki kanıt sertifikası ve üretim
  ölçeği kabul ölçümü bu araçların kapsamında değildir.

## 0.6.0 sıcak kayıt saklama

Bu araç audit zincirini dışa aktarır. [Retention bakımı](retention.md) ayrıca cold segmentleri doğrulayıp uygun geçmişi sıcak depodan çıkarır. Audit export gerektiğinde kayıtlı cold ve hot audit öneklerini birleştirir; health hot+archived sıra sayımını ayırır. Cold segmentler fiziksel yedekle taşınır.
