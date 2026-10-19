# SDBASE 0.8.1 — nihai kaynak ve paket kontrolü

**Kontrol tarihi: 2026-10-01.** SQL kullanmayan bağımsız `.sdb` motorunun bu çalışma kapsamındaki nihai düzeltme sürümü **0.8.1** olarak hazırlandı. Schema **v4**, 95 domain koleksiyonu, 93 ayrı kaynak modülü, 160 workflow ve 210 HTTP yolu vardır. Sürüm değeri `pyproject.toml`, paket import'u ve OpenAPI yanıtında aynıdır.

## Son denetimde yapılan düzeltme

Temiz kurulumdaki boş `station_items` koleksiyonunun indeksleri başarıyla hazırlanıyor; fakat `index-status` geçerli, sıfır uzunluklu okuyucuyu boolean olarak değerlendirip yanlış `stale_or_invalid` bildiriyordu. `disk_index.py` okuyucunun varlığını `is not None` ile denetler. Boş indeksi ve readonly restart sonrası geçerliliğini sınayan iki gerçek kabul kontrolü mevcut disk aracına eklendi.

Bakım kabul raporu sürümü paket `__version__` değerinden alır. README ve uygulama durumu güncel sürümü gösterir. İndeks durumu tanı amaçlı JSON verir; `stale_or_invalid`, geçerli authoritative DB'nin bozulduğu anlamına gelmez. Optional cache'e ait bu durum sorgunun RAM fallback veya açık bütçe reddi davranışıyla değerlendirilir.

## Kabul sonucu

**333/333 kontrol başarılı:**

| Araç | Geçen kontrol |
|---|---:|
| WAL/PITR recovery | 35 |
| Physical standby/replication | 35 |
| Snapshot/query/integrity | 33 |
| HTTP/pool/lock/migration engine | 19 |
| Ayrı merkez/uç süreçleriyle örnek playout | 37 |
| TLS/bearer ağ çoğaltma, restart/Range, legacy timestamp | 53 |
| Fenced otomatik API/DB devri | 36 |
| Disk indeksleri, bütçeler, bozulma ve boş indeks | 39 |
| Concurrent online checkpoint ve crash recovery | 46 |

Araçlar kendi geçici dizinlerini kullanır. Kullanıcının üretim verisi alınmaz. Kabul çıktıları source fixture olarak paketlenmez. Senaryolar ve çalıştırma komutları [motor raporundadır](maturity_080.md). 0.8.0 ölçümünün 440.000 sentetik kaydı, 128 başarılı HTTP isteği ve gecikme/RAM sonuçları aynı tarihsel raporda kendi sürüm ve sınırlarıyla korunur.

## Temiz paket kurulumu

Geçici kaynak kopyasından **`radios_sdbase-0.8.1-py3-none-any.whl`** ağ kullanmadan oluşturuldu; temiz Python **3.13** venv'e `--no-index --no-deps` ile kuruldu. Wheel, 93/93 Python kaynak modülünü içerdi. Paket metadata'sı ve runtime sürümü 0.8.1 döndü.

Kurulu `sdbase` giriş noktasıyla `--help → init → status → checkpoint-online → index-rebuild → index-status → backup-create → backup-verify` zinciri çalıştı. Yeni DB'nin üç boş station-item indeksi, ayrı CLI sürecinden **valid** döndü. Dolu station indeksi de valid çıktı. Backup schema v4/sequence 1/üç dosya ile doğrulandı. Fixture, wheel ve venv kaldırıldı.

Python modülleri 3.11 AST dil seviyesiyle de parse edildi; minimum Python 3.11 runtime'ı ayrı süreçte çalıştırılmadı. Çalışma zamanı bağımlılık listesi boş; kaynaklarda SQL driver import'u bulunmadı. Yerel belge bağlantıları, sürüm ve health/OpenAPI kimlik alanları doğrulandı. Kaynak klasöründe `.sdb`, WAV, `.pyc`, build/dist, egg-info veya test fixture klasörü bırakılmadı.

## Kaynak kimliği

93 `src/sdbase/*.py` dosyası ile `pyproject.toml` için göreli dosya yolu ve dosya SHA256'larından oluşan canonical JSON envanterinin birleşik SHA256 değeri:

```text
2824e26d1671e6ba6390285dd30f6b6feed6d7dfa75c38580fcab8ce8745ad9a
```

Hash algoritması: göreli yolları sırala; her dosya için `{"path":…, "sha256":…}` kaydı üret; listeyi `ensure_ascii=False, sort_keys=True, separators=(',', ':')` ile UTF-8 JSON olarak kodla ve SHA256 hesapla. Belgeler/operasyon araçları bu runtime kimliğine dahil değildir. Hash sürüm dosyalarını karşılaştırır; imzalı yayın veya kimlik doğrulama sertifikası değildir.

## Doğrulanmış kapsam

Kaynak/paket ve izole süreç kabulü tamamlandı. Otomatik devir, bilinen eski kaynak dosyaları ve canonical lock erişilebilirken çalışır. TLS follower asenkron, tek primary fiziksel çoğaltma uygular. Güncel belgeler/hash indeksleri RAM'dedir; global WAL/PITR tüm JSON payload'larını yükler. İlk disk indeks doğrulaması tam tarama, checkpoint publication ise exclusive kilit kullanır.

Uzak quorum, fiziksel yayın cihazında kesintisiz devir, ikinci fiziksel makineyle saha tatbikatı ve günler/yıllar gerçek çalışma bu kabul kapsamında kanıtlanmış değildir. Bu belge belirtilen kaynak sürümünün kapanış kaydıdır; bütün radyo ürünlerinin tüm arayüz/audio/donanım özelliklerine eşdeğerlik iddiası içermez.
