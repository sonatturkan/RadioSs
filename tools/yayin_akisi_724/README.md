# 7/24 yayın akışı kurulum betikleri

Kütüphanedeki müzikleri kategorilere ayırır, kategori paketlerini playlist olarak kaydeder,
gün bölümü rotasyonlarını ve haftalık programı veri servisine yazar. Arayüz kullanılmaz.

Çalışma dizini (DIR) içinde `media.json` (desktop-media/list çıktısı, bütün sayfalar) bulunmalı.

```sh
python3 build.py DIR plan        # kategori paketleri ve haftalık atama raporu (yazmaz)
python3 build.py DIR playlists   # paket playlistlerini oluşturur (var olanları atlar) → DIR/pack_map.json
python3 sweeper_import.py DIR    # Yeni Klasör 3'teki sweeper'ları içe aktarır (analiz: cc ile derlenen analiz.c)
python3 build.py DIR compose     # DIR/main_new.json ve DIR/weekly.json (yazmaz)
python3 build.py DIR write       # ana takvim (CAS) + "RadioS 7/24 Yayın Akışı" haftalık programı
python3 verify.py DIR DIR/weekly.json '[["ad","2026-10-19T06:02:00"], …]'   # şema yolu kuru çalıştırma (ses/yazma yok)
```

`verify.py`, `tools/otomasyon_testleri/e2e.c` ile aynı C düzeneğini (`DIR/e2e`) kullanır:
`cc -std=c11 -Iinclude -Ibuild/generated … e2e.c libradios_{playout,core,audio}.a`.

Sınırlar: bir playlist en çok 500 parça; `desktop-playout/prepare` yanıtı 1,5 MB
(≈ 320 parçalık kuşak kaynak toplamı), bu yüzden kategoriler ~50 parçalık paketlere bölünür.
