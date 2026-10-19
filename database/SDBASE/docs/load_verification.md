# Yük ölçümü ve eşzamanlı yazma düzeltmesi

Bu 0.6.0 koşusunun tarihsel raporudur. Disk indeksleri ve concurrent online checkpoint içeren son 0.8.0 ölçümü [güncel motor raporundadır](maturity_080.md).

**1 Ekim 2026 ölçümü:** 250.000 içerik, 1.000.000 sentetik katalog belgesi; ayrıca 60.000 attempt, 120.000 playout event ve 60.000 observation. Dört istasyon ve sekiz gerçek HTTP kullanıcı credential'ı ile sekiz işçi çalıştırıldı. İlk ölçümde bulunan gereksiz yazma çatışması düzeltildi ve aynı büyük koşu yeniden yürütüldü.

Bu sonuç **250.000 doğrulanmış ses dosyası veya bir milyon medya kaydı** anlamına gelmez. Her içerik için bir item, bir station-item ilişkisi, bir taslak version ve bir `received` media metadata satırı yaratıldı. Ses baytı, doğrulanmış medya, yayın hakkı veya onaylı playout üretmedi. Toplam **1.240.000 yük belgelerine** kurulum, kullanıcı, izin, oluşturulan plan ve otomatik audit satırları ayrıca eklenir.

## Ortam ve yöntem

| Özellik | Ölçüm |
|---|---|
| CPU | Intel Core Ultra 7 268V |
| Sistem / Python | Linux x86_64, Python 3.13.15 |
| Fiziksel RAM | `/proc/meminfo`: 32.418.548 KiB |
| Fixture diski | Kaynak çalışma dizininin bulunduğu NVMe ext4; `/tmp` tmpfs kullanılmadı |
| İşlem sınırı | Her büyük koşuda 12 GiB Linux address-space sınırı (`RLIMIT_AS`) |
| Katalog dağılımı | Dört istasyona eşit, istasyon başına 62.500 içerik |
| Seed işlemi | 100 içerik/attempt başına bir `fsync` edilmiş işlem; katalog batch'i 400 domain belgesi |
| API yükü | Üç dalga × 60 iş işlemi; dalgalar arasında normal HTTP rate limitini korumak için 12 saniye |
| Gerçek HTTP | Bearer doğrulaması, istasyon izinleri, HTTP dispatch ve normal hata/çatışma denetimleriyle 240 loopback isteği |
| Yeniden deneme | Sıfır; 409 ve 429 sonuçları rapordan gizlenmez |

Aramalar `Benchmark track` başlığı ve 1–300.000 ms süre filtresiyle 25 sonuç ister. Süre sentetik teknik metadata'dır; `ready=true`, hak/medya hazırlığı veya fuzzy arama ölçümü değildir. İlk soğuk sıralama çekirdek arama ölçümüne dahildir. HTTP plan işlemi, yeni taslak plan ve **bir wait girdisi** oluşturur; kalabalık bir clock planını onaylama, medya dağıtımı veya playout yürütme ölçümü değildir.

Operational kayıtların zamanları **2020-10-01–2026-10-01** aralığına yayılmıştır. Her attempt, start/complete event'leri ve ona bağlı `endpoint_self_report` observation ile şema kontrollü bir Store işleminde yazılmıştır. Bunlar sentetik off-plan engine iddialarıdır. Altı yıllık timestamp aralığı, altı yıl boyunca çalışan servis veya gerçek yayın kanıtı olarak sunulmaz. Seed süreleri batch yazımıdır; tek kayıt/API yazma gecikmesi ayrı ölçülür.

## Bulunan hata ve uygulanan düzeltme

İlk karma HTTP ölçümünde 60 istasyon uygunluğu güncellemesinin yalnız **8'i kabul edildi, 52'si 409 aldı**. Başka bir item veya planın commit'i, `set_station_eligibility` işleminin bütün veritabanını kapsayan sequence koşulunu geçersiz kılıyordu. Hedef ilişkiler farklı olduğunda da bağımsız kullanıcılar gereksiz çatışıyordu.

[catalog.py](../src/sdbase/catalog.py) içindeki bu işlem artık kullanıcı, istasyon, gerçek `activate` izin kaydı ve hedef association satırını tutarlı bir paylaşımlı okuma kilidi altında yakalar. Commit, kullanıcı/istasyon/izin kaydının gerçek `_rev` değerlerini ve hedef association'ın `expected_revision` değerini denetler. İlgisiz katalog veya plan commit'i bu kararı değiştirmez. Diğer daha geniş katalog yazmalarının güvenlik koşulları korunur.

Ek transient kontrol geçti: iki istasyonda sekiz işçiyle **32 farklı association yazması 32/32 kabul edildi**. Eski hedef revision reddedildi. Okumadan sonra, commit'ten hemen önce enjekte edilen izin iptali ve kullanıcı devre dışı bırakma işlemleri Conflict verdi; hedef association değişmedi. Sonraki çağrı da eski yetkiyle yazamadı.

| Gerçek HTTP iş işlemi | Düzeltmeden önce | Düzeltmeden sonra | Son koşu başarılı işlemler p50 / p95 / en yüksek |
|---|---|---|---|
| Katalog arama | 60 kabul, 0 çatışma | 58 kabul, 2 çatışma | 349,782 / 593,479 / 710,798 ms |
| Station-item uygunluk yazması | **8 kabul, 52 çatışma** | **60 kabul, 0 çatışma** | 123,120 / 356,915 / 474,209 ms |
| Taslak plan + bir girdi | 60 kabul, 0 çatışma | 60 kabul, 0 çatışma | 693,373 / 981,532 / 1.117,262 ms |
| HTTP istek toplamı | 188 × 200, 52 × 409 | **238 × 200, 2 × 409** | Tüm 240 istek: 285,209 / 593,389 / 817,356 ms |

Son koşudaki iki arama 409'u, arama sırasında katalog generation'ının değişmesinden kaynaklanır; karışık katalog sayfası dönülmez. Bunlar otomatik tekrar edilmedi. Başarılı eşzamanlı yazmaların artması arama önbelleğini daha sık geçersiz kıldığı için son koşunun HTTP arama/plan gecikmesi yükseldi. Bu sonuçta daha az iş yapan ilk koşunun gecikmesini yüksek başarılı yazma kapasitesi gibi yorumlamamak gerekir.

HTTP mutation revision'ları çalışma başlamadan mevcut association satırlarının `_rev` değerlerinden alındı. Ölçüm, POST yazması sırasında eski bir precondition'ı gizlice yenilemez. Detail → stale edit → 409 → detail reread → yeni revision ile kabul zinciri ayrı gerçek HTTP kontrolünde doğrulanmıştır; [API sözleşmesi](api.md) bu istemci davranışını açıklar.

## Son büyük koşunun diğer ölçümleri

| İşlem | Sayı / kapsam | Süre veya gecikme |
|---|---|---|
| Katalog seed | 2.500 batch; 1.000.000 domain belge | Toplam 39,550 sn; batch p50 11,941 ms, p95 **13,328 ms** |
| Operational seed | 600 batch; 240.000 domain belge | Toplam 14,808 sn; batch p50 17,627 ms, p95 **21,480 ms** |
| Çekirdek arama | 120 sorgu, 8 işçi, soğuk sıralama dahil | p50 6,377 ms; p95 **360,639 ms**; en yüksek 492,179 ms |
| Çekirdek tek item yazması | 120 farklı item, 8 işçi, 120 kabul | p50 34,387 ms; p95 **41,485 ms**; en yüksek 64,395 ms |
| HTTP dalgaları | 180 iş işlemi / 240 istek | 33,472 sn; bu süre 24 sn dalga beklemesini içerir |
| Checkpoint | Commit sequence **3412** | **15,344 sn** |
| Yeniden açma | Aynı checkpoint sequence doğrulandı | **14,735 sn** |
| Yedek oluşturma | Metadata mağazası, gerçek medya nesnesi **0** | **1,003 sn** |
| Yedek doğrulama | 3 DB dosyası | **11,885 sn** |
| Tüm koşu | Fixture seed, ölçümler, checkpoint, reopen ve yedek | **132,517 sn** |
| Checkpoint sonrası DB dosyaları | Journal + meta + checkpoint | **709.171.390 byte** |
| İşlem tepe RSS | Yedek doğrulaması dahil | **6.908.805.120 byte**, yaklaşık **6,43 GiB** |

İlk büyük koşu 129,079 sn sürdü; tepe RSS 6.910.013.440 byte, DB boyutu 709.142.270 byte ve sequence 3360 idi. Son koşunun sequence farkı, artık reddedilmeyen 52 uygunluk commit'ini de içerir. Önceki 100.000 içerikli tek istasyon/tmpfs ölçümü farklı disk, seed batch ve yük kapsamına sahip olduğundan bu tabloyla doğrudan hız karşılaştırması yapılmaz.

## Yeniden çalıştırma

[tools/benchmark.py](../tools/benchmark.py) varsayılan olarak fixture'ı işlem sonunda siler. `--temporary-parent` büyük fixture'ın diskini seçer; `--work-dir` yalnız **önceden bulunmayan** bir dizinde sonucu özellikle tutmak içindir. Her iki büyük ölçümün fixture'ları temizlendi; kalıcı test veritabanı, ses veya test paketi eklenmedi.

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tools/benchmark.py \
  --items 250000 --stations 4 --api-users 8 \
  --operational-attempts 60000 --log-years 6 --seed-batch-size 100 \
  --searches 120 --updates 120 --concurrency 8 \
  --http-requests 60 --http-rounds 3 \
  --temporary-parent . --memory-limit-mib 12288 --progress
```

JSON stdout, faz bilgisi stderr'dedir. `--memory-limit-mib` son araçta aynı 12 GiB sınırını sağlar; ölçüm koşuları bu sınırı `resource.setrlimit` ile baştan uyguladı. Rapor biçimi `sdbase-synthetic-benchmark-v2`, `production_result=false` olarak kalır.

Bu kontrol yüz binlerce içerikte ve bir milyondan fazla domain satırında somut ölçüm sağlar. Milyonlarca **item**, yoğun gerçek yayınla yıllarca kesintisiz çalışma, onlarca istasyon, yüzlerce API kullanıcısı, gerçek ses deposu, WAN, disk arızası, uzun soak testi ve bağımsız makineye felaket kurtarma kabulü ölçülmedi. Buradaki yedek süreleri medya aktarım kapasitesi değildir. Üretim hedefindeki donanım ve gerçek iş yükü için aynı araçla daha uzun/kademeli kapasite ölçümü gerekir.

## 0.7.0 snapshot/HTTP kontrol ölçümü

Yeni snapshot okuması ve keep-alive değişikliği için 2026-10-01'de ayrı küçük koşu yapıldı:

```sh
PYTHONDONTWRITEBYTECODE=1 python tools/benchmark.py \
  --items 10000 --seed-batch-size 100 --stations 4 --api-users 8 \
  --concurrency 4 --searches 12 --updates 20 \
  --operational-attempts 100 --http-requests 20
```

| Ölçüm | Sonuç |
|---|---:|
| İçerik / iş kaydı | 10.000 / 40.000 katalog-medya metadata belgesi |
| Gerçek HTTP | 26/26 HTTP 200; 7 arama, 7 optimistic yazma, 6 plan+entry işlemi |
| HTTP request p50 / p95 | 59,471 / 159,352 ms |
| Dört eşzamanlı kullanıcıyla 12 doğrudan arama p50 / p95 | 185,071 / 237,957 ms |
| 20 eşzamanlı revision yazması p95 | 2,789 ms |
| Checkpoint / yeniden açma | 311,886 / 354,216 ms |
| Toplam | 3,723 saniye |

Bu koşuda WAL ve replika açık değildir. Sentetik medya metadata'sı vardır; ses baytı ve gerçek yıllarca çalışma yoktur. Snapshot capture maliyeti kapsamın kayıt/indeks sayısıyla büyür. Önceki 0.6.0 büyük koşusunun kapasite sonucu yeni özelliklerin maliyetine otomatik taşınmaz; bu küçük kontrol üretim kapasite garantisi değildir.
