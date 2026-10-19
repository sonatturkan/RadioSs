# Otomasyon uçtan uca testleri

Uygulamanın **Yayın › Şema › Şemayı çalıştır** yolunu, uygulamanın kullandığı gerçek C
kütüphaneleri ve çalışan veri servisiyle adım adım çalıştırır:

kuşak seçimi → kapsam yönlendirmesi → hazırlık yapılandırması → backend `desktop-playout/prepare`
(otomasyon) → takvim planı → backend `prepare` (program) → birleştirme → yerel oynatma planı.

Ses çalmaz. Veritabanına yalnız `--kaydet` verildiğinde yazar ("TEST · …" haftalık programları).
Test playlistleri ("TEST · …") ilk çalıştırmada bir kez oluşturulur.

## Gerekenler

- Çalışan veri servisi (`127.0.0.1:8000`) ve `config/credentials.json`
- Derlenmiş kütüphaneler: `cmake --build build`

## Kullanım

```sh
tools/otomasyon_testleri/build.sh              # build/otomasyon_testleri/e2e
cd tools/otomasyon_testleri
python3 senaryolar.py                          # bütün gruplar
python3 senaryolar.py H C                      # yalnız H ve C grupları
python3 senaryolar.py --kaydet                 # ayrıca TEST şeması olarak backend'e kaydet
python3 isaretler.py                           # şarkı işaretleri × geçişler (işaretleri sonunda nötre döndürür)
```

Sonuçlar `build/otomasyon_testleri/sonuclar.json` dosyasına yazılır.

| Grup | İçerik |
|---|---|
| H | Geçiş geometrisi, zaman penceresi, tükenme, filtreler, tek oynatıcı, 50 kaynak, saat dilimi, hafta dönüşü, olasılık grupları, oynatıcı havuzları |
| K | Şarkı ortası katman (jingle/sweeper) olması gereken davranış |
| L | Canlı "RadioS 7/24 Yayın Akışı" farklı saatlerde |
| G | Geçiş türleri: cue işaretleri, 30 sn beatmatch, low-pass, olasılık toplamı, dışlama, AutoDJ tür kuralları |
| S | Seçim desenleri: doğrusal sıklık, rastgelelik bayrakları, kaynak pencereleri, blok sırası, geçmiş |
| Z | Takvim: iki haftalık program, döngü, yaz saati, gece yarısı |
| C | Süreklilik: art arda sweeper yok, yumuşak kuşak sonu (`soft_end_seconds`), kural gevşetme (`relax_gaps`) |

Satır biçimi: `✓ ad: öğe sayısı {türler} geçiş={EOF/XF/CUE/+BS/+LP} katman=N tanı=[…] son_bitiş-kuşak_sonu=±sn`.

## Salt okuma 7/24 koşumu (`senaryolar_724.py`)

`senaryolar.py` eksik "TEST · …" playlistlerini canlı veritabanında **oluşturur**. Canlı servise hiç
yazmadan doğrulamak için `senaryolar_724.py` aynı grupları (H K G S Z C) canlı "7/24 · …"
paketleriyle kurar; ayrıca L (7 gün × 8 saat + kuşak sonları), N (gece kuşakları, farklı
tohumlar) ve P (kuşak geçişleri: bitişe 90 sn / 10 sn, sonraki kuşak başı) gruplarını koşar.
Yalnız `desktop/list`, `desktop-playlists/list` ve `desktop-playout/prepare` istekleri gider.

```sh
RADIOS_BUILD_DIR=build-67 tools/otomasyon_testleri/build.sh   # e2e: build-67/otomasyon_testleri/e2e
cd tools/otomasyon_testleri
RADIOS_BUILD_DIR=build-67 python3 senaryolar_724.py           # bütün gruplar, 4 tohum
RADIOS_BUILD_DIR=build-67 python3 senaryolar_724.py L P --tohum 2
```

- `RADIOS_BUILD_DIR`: e2e ikilisi, kütüphaneler ve sonuç dosyaları bu yapı dizininde (varsayılan `build`).
- `RADIOS_E2E_PACE_S`: istekler arası en az bekleme (sn; `senaryolar_724.py` için varsayılan 1, diğerleri 0).
- Sonuçlar: `<yapı>/otomasyon_testleri/sonuclar_724.json` ve özet tablo `sonuclar_724.md`
  (doluluk, ilk parça gecikmesi, kuşak sonu sapması, art arda sweeper, şarkı/sanatçı tekrarı, tanılar).
`✗ … WOULD_BLOCK` sonuçları sığacak parça kalmadığında beklenen "bekle-yeniden dene" durumudur.

## Küme / kümesiz senaryoları (`senaryolar_kume.py`, salt okuma)

Kılavuz §08 "Kümeli ve kümesiz çalışma" / "7/24 otomasyon ve doğrulama": `docs/kilavuz-2026-10-18-4c/kume.md`
senaryoları (K1–K6 kümeli, T1–T4 kümesiz, L1–L6 canlı 7/24), S1 (dört oynatıcılı kapsamda A {1,2} → B {3,4}
havuz devri) ve LT1–LT2 (canlı oynatıcı topolojisinde canlı programa atanmış küme). Sonuç yalnız yazdırılmaz,
doğrulanır: yönlendirilen havuz = kural (kapsam dışı kayıtlı oynatıcı düşer, boş kalan tür kapsamın tamamına
döner) = senaryonun beklediği; her satırın havuzu ve seçilen fiziksel oynatıcı kapsamda; bindirmeli ardışık
satırlar farklı oynatıcıda; `pool_count`; art arda sweeper yok; geçiş türleri (sentetikte senaryonun, canlıda
rotasyonun kendi ayarı); K4 tek oynatıcıya düşen bindirme → kesme, S1 sınır geçişi, L4–L6 kuşak/bitiş sınırı.

```sh
flock build-4c/.build.lock env RADIOS_BUILD_DIR=build-4c tools/otomasyon_testleri/build.sh
cd tools/otomasyon_testleri
RADIOS_BUILD_DIR=build-4c RADIOS_E2E_PACE_S=1 python3 senaryolar_kume.py      # hepsi; ya da: ... K T S L LT
```

- Yalnız `desktop/list`, `desktop-playlists/list`, `desktop-playout/prepare` istekleri; yazma kipi yok.
- Kimlikler senaryo adından türetilir (uuid5): aynı veri ve kodla aynı seçim. L/LT sonuçları canlı programın
  revizyonuna bağlıdır (rapor başlığında). e2e ikilisi kütüphaneden eskiyse ya da kaynak derlenmemişse uyarır.
- Yarı AutoDJ tek oynatıcıyla koşulur (uygulamada `install()` SEMI'de `calendar_routing`'i siler). Kapsam
  uyarısı metni e2e'de üretilmez; "Uyarı" sütunu C kuralıyla hesaplanır (metin `test_automation_scope`'ta sınanır).
- Sonuçlar: `<yapı>/otomasyon_testleri/sonuclar_kume.json` ve `sonuclar_kume.md`; ara dosyalar `kume/` altında.
