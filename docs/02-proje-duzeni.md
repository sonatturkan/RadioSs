# 2. aşama — proje düzeni

Tarih: 2026-10-06. Proje kökü:

```text
/home/sturkan/Masaüstü/radyo projesi/kaynak kodlar/RadioS
```

RadioS yerel C masaüstü programı olarak geliştirilecektir. Bu aşama kaynakların proje içine alınması, modül dizinleri, çalışma verisi yerleri ve yapılandırma örneğini kapsar. Derlenebilir native uygulama iskeleti aşama 3'tedir.

## Oluşturulan düzen

```text
RadioS/
├── README.md
├── .gitignore
├── src/
│   ├── app/             # C uygulama yaşam döngüsü
│   ├── core/            # zaman, iş/durum kuyrukları, hata/günlük
│   ├── ui/              # native pencere ve yedi çalışma profili
│   ├── audio/           # deck, mixer, routing ve DSP
│   ├── media/           # dosya analizi, decoder, waveform/cue
│   ├── db/              # C SDBASE veri istemcisi
│   ├── automation/      # playlist, program, takvim ve otomasyon
│   ├── traffic/         # reklam ve mutabakat
│   ├── emergency/       # acil yayın ve devralma
│   ├── monitoring/      # ölçüm ve teşhis
│   └── integrations/    # stream, kayıt, metadata, MIDI/GPIO
├── include/radios/      # ortak C başlıkları
├── assets/
│   ├── fonts/           # Inter WOFF2 ve orijinal lisans
│   ├── icons/
│   └── themes/
├── config/
│   ├── README.md
│   └── radios.example.ini
├── database/
│   └── SDBASE/          # mevcut Python veri motorunun tam kaynak kopyası
├── design/
│   └── reference/tasarrım/  # yalnız görsel HTML/CSS/JS referansı
├── data/
│   ├── database/
│   ├── endpoints/
│   ├── imports/
│   ├── cache/
│   ├── logs/
│   ├── recordings/
│   └── backups/
├── build/              # oluşturulacak derleme çıktıları
├── cmake/              # aşama 3 derleme desteği
├── packaging/          # aşama 14 kurulum/dağıtım
├── tests/              # bileşen testleri ileride eklenecek
├── tools/              # geliştirme araçları
└── docs/               # işlev haritası ve ilerleme kayıtları
```

Boş modül/çalışma dizinleri `.gitkeep` ile korunur. C kaynak klasörlerine örnek `.c` dosyası, sahte oynatıcı veya HTML başlangıç sayfası eklenmedi. `CMakeLists.txt` ve çalıştırılabilir C hedefi aşama 3'te yazılacaktır.

## Kaynak aktarımı

| Kaynak | RadioS içindeki hedef | İlk aktarımdaki dosya sayısı |
|---|---|---:|
| Eski `SDBASE/` | `database/SDBASE/` | 146 |
| Eski `tasarrım/` | `design/reference/tasarrım/` | 72 |
| Referanstaki font ve lisans | `assets/fonts/` | 2 ek kaynak |

218 orijinal dosyanın tamamı byte ve SHA-256 karşılaştırmasıyla kopyalandı. SDBASE'in `src/`, `docs/`, `tools/`, README, uygulama durum raporu, pyproject ve gizli `.gitignore` dosyaları alındı. Kaynakta canlı veritabanı veya Python/derleme çıktısı bulunmadı; bunlar kaynak kopyasına eklenmedi. Eski klasörler değiştirilmedi veya silinmedi.

Köken, hedef, boyut ve ilk kopyanın SHA-256 değerleri [kaynak aktarım manifestinde](source-import-manifest.json) bulunur. Bu manifest **ilk aktarımın kaydıdır**; sonraki yetkili geliştirmelerin kaynağı değiştirmesi beklenir. Geliştirme RadioS içindeki kopyalarda yapılacaktır; eski klasörlerle otomatik senkronizasyon kurulmadı.

Tasarım referansı çalışma zamanı arayüzü değildir. Native C GUI kaynakları `src/ui/` içinde geliştirilecektir. `assets/fonts/InterVariable.woff2` ile `LICENSE.txt` birlikte alındı. WOFF2'nin seçilecek native font yükleyicisinde kullanılması henüz doğrulanmadı; GUI aşamasında uygun biçim/yükleme çözülecektir.

SDBASE 0.8.1 Python 3.11+ motorudur; `pyproject.toml` boş çalışma zamanı bağımlılık listesi ve `setuptools>=68` paket derleme şartı bildirir. Mevcut `fcntl` kullanımı Linux/POSIX ortamına bağlıdır. Bu aktarım veritabanını C'ye dönüştürmez. Kaynakta olmayan ayrı bir SDBASE lisansı eklenmedi; orijinal dosyalar korundu.

## Çalışma verisi ve yol sözleşmesi

Örnek yapılandırma [config/radios.example.ini](../config/radios.example.ini) dosyasındadır. Henüz bu yapılandırmayı okuyup servis başlatan C kodu yoktur.

- Merkez DB: `data/database/RadioS.sdb`. Gerçek dosya oluşturma ve ilk kullanıcı/uç kurulumu aşama 5'tedir.
- Merkez medya deposu SDBASE tarafından DB adına bağlı türetilir: `data/database/RadioS.sdb.objects/`. Ayrı bağımsız yönetilen medya kökü tanımlanmaz.
- `.meta`, `.checkpoint`, `.lock` ve diğer motor yan dosyaları aynı DB ile birlikte yönetilir. Motor kaynakları ve canlı dosyalar aynı dizine karıştırılmaz.
- Uç verisi: `data/endpoints/<endpoint_uuid>/`. Uç ajanının `Endpoint.sdb`, sahiplik kilidi, `objects/` ve `staging/` yapısı merkez DB'den ayrıdır.
- `data/imports/` yalnız içe alma hazırlığıdır; onaylı içerik adresli medya deposu yerine kullanılmaz.
- Göreli yollar **RadioS köküne göre** çözülecektir. Terminalin çalışma dizini DB konumunu veya kimlik kaydını değiştirmemelidir.
- Gerçek UUID, token, kullanıcı veya hayalî ses cihazı örnek dosyaya yazılmadı. Ses cihazları atanmış değildir; otomatik oynatma varsayılanı kapalıdır.

`.gitignore`, çalışma verilerini, derleme çıktısını, Python önbelleğini ve yerel kullanıcı yapılandırmasını kaynak takibinden ayırır. Bu dosya Git deposu başlatmaz; ileride kaynak sürümleme kullanıldığında aynı ayrım korunacaktır.

## İşlev haritası ve araçların konumu

1. aşamanın tablolarındaki kaynak yolları RadioS içindeki kopyalara güncellendi. `T/` kısaltması `design/reference/tasarrım/`, `DB/` kısaltması `database/SDBASE/src/sdbase/` anlamına gelir. Kopyalar aynı olduğu için kaynak satır referansları korunmuştur.

`tools/islev_envanteri.py` artık varsayılan olarak yalnız RadioS içindeki tasarım ve SDBASE kaynaklarını okur. Envanter ve OpenAPI çıktıları bu yollarla yeniden üretildi. Başka çalışma dizininden veya taşınmış bir RadioS kopyasından kullanım için dışarıdaki eski klasörler gerekmez.

RadioS kökünden doğrulama:

```sh
python3 tools/islev_envanteri.py --check
```

Bu bir geliştirme/belge aracıdır; C uygulaması veya fiziksel yayın testinin kendisi değildir.

## Aşama 2 kabul ölçütleri

1. Kaynaklar, native arayüz kaynakları, C modülleri ve çalışma verileri RadioS içinde ayrılmıştır.
2. SDBASE'in tüm kaynak paketi ve görsel referans yerel kopyalara alınmıştır; ilk kopya özetleri orijinallerle eşleşir.
3. Envanter aracı eski kardeş klasörlere bağlı kalmadan proje içi kaynakları kullanır.
4. Yapılandırmanın bütün göreli veri/kaynak yolları RadioS altında çözülür; canlı DB veya gerçek kimlik bilgisi oluşturulmaz.
5. Belgelerin dosya/satır referansları proje içinde bulunur ve yerel belge bağlantıları çözülür.
6. Çalıştırılabilir C uygulaması ve DB kurulumunun henüz sonraki aşamalarda olduğu açıkça kaydedilir.

## Tamamlanma ve doğrulama kaydı

**Aşama 2, 2026-10-06 tarihinde tamamlandı.**

| Kontrol | Sonuç |
|---|---|
| Kaynak kopyaları | 218 dosyanın orijinali, proje içi kopyası ve manifest SHA-256 değeri eşleşti. |
| Font/lisans | 2 kaynak kopyası referanstaki dosyalarla byte/SHA-256 düzeyinde eşleşti. |
| Envanter | RadioS kökünden `--check` geçti; 7 profil, 552 statik kontrol, 160 workflow, 210 yol ve 215 HTTP işlemi korundu. |
| Yapılandırma | 9 göreli kaynak/veri yolu RadioS altında çözüldü; gerekli dizinler bulundu; canlı DB dosyası oluşmadı. |
| Kaynak atıfları | 647 dosya:satır atfı RadioS içindeki dosyalarda doğrulandı. |
| Yerel belge bağlantıları | 23 bağlantı çözüldü; eksik dosya bulunmadı. |
| Bağımsız proje kopyası | RadioS geçici ayrı dizine kopyalandı; envanter aracı farklı çalışma dizininden ve eski kardeş kaynak klasörleri olmadan `--check` işlemini geçti. |
| Kaynak takibi ayrımı | Geçici kopyadaki 9 ignore örneği doğrulandı: çalışma DB/derleme/yerel config dışarıda, C/veri motoru kaynakları/örnek config/font lisansı dahil. |
| Geçici doğrulama dosyaları | Kopya ve yalnız onun içinde açılan test Git deposu temizlendi; asıl proje içinde Git deposu başlatılmadı. |

Bu sonuçlar proje düzenini doğrular. C derleme, native pencere, çalışan veritabanı kurulumu veya ses çıkışı henüz bu aşamada gerçekleştirilmedi. Sonraki iş paketi aşama 3'te gerçek C uygulama iskeletini ve derleme hedefini oluşturmaktır.
