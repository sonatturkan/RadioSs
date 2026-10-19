# Akım 4c gereksinim matrisi (PDF `radyo_otomasyonu_kilavuzu.pdf`, 18.10.2026)

Durum sütunu ajan raporlarından derlenir: `cekirdek.md`, `rotasyon.md`, `takvim.md`, `kume.md`.

| No | PDF | Gereksinim (özet) | Sahip | Durum |
|---|---|---|---|---|
| B1 | §02 s.2 | "Playlist ekle" → "Playlist ya da live event ekle"; alana otomasyon şeması eklenebilir; playlist ve live eventler seçilen şemadaki tanımlardan gelir | rotasyon | |
| B2 | §02 s.2 | Playlist 1→2 bağlantısına mevcut otomasyon atanır; playlist türü uygun otomasyonu belirler (`link_transitions`) | rotasyon (UI) + çekirdek (planlayıcı) | |
| B3 | §02 s.2 | Şema yüklendikten sonra "Mevcut şemayı uygula" çalan programa uygular; AutoDJ şemaya atanmış geçişleri yürütür | takvim | |
| B4 | §02 s.2 | Program (zaman + sıralı içerik) ile şema (tür + geçiş kuralları) ayrı tutulur | takvim (doğrulama) | |
| B5 | §03 s.3 | "Kanallar (oynatıcılar)" kutuları yerine kümeli oynatıcıların açılır pencereden seçimi; kümesiz tek tek; oynatıcı sayısı ve track yerleşimiyle uyumlu | rotasyon + küme (doğrulama) | |
| B6 | §03 s.3 | Yüklenen şema çalan programdaki AutoDJ geçişlerini ve kümeden kümeye geçişi düzenler | çekirdek + küme | |
| B7 | §03 s.3 | Programdan programa devam: sonraki programın hangi sırasına, hangi geçişle; olasılık seçeneği alternatif yol; yoksa bağlantı geçişi (`programme_entry`) | çekirdek (planlayıcı) + takvim (sürücü) | |
| B9 | §04 s.4-5 | "Program oluştur"da Live event seçeneği; işaretlenince canlı yayın ayarları yan tarafta | takvim | |
| B10 | §04 s.4 | Live event seçiliyken rotasyon listesi yerine rotasyon şemaları; şema event türlerini, playlistleri ve otomasyonu programa taşır | takvim | |
| B11 | §04 s.4 | Playlistlerin başlangıç/bitiş tarihleriyle yayın aralığında devreye girecek playlistler belirlenir (`start_date`/`end_date`) | rotasyon (UI) + çekirdek (planlayıcı) | |
| B12 | §04 s.4, §06 | Yayın sırası 1,2,3…: playlistler ve araya yerleştirilen live eventler; konuşma da sırada bir event | çekirdek (live blok) + takvim (projeksiyon) | |
| B13 | §04 s.4, §05 | Her yayın adımında yayına verilecek mixer kanalları ayarlarda görünür ve seçilir; DJ/Konuşma/Müzik etiketi L işlevini belirler | rotasyon (live satır) + takvim (program paneli); L/R 1f, etiket 67 | |
| B14 | §04 s.6 | "2 2 2": aktif şema + A + B iç sıra kayıtları; aynı sıra üç tarafta ve tip uygun; ilk ortak kayıt; yoksa bağlantıya atanmış varsayılan şema geçişi | çekirdek (planlayıcı) + rotasyon (iç sıra UI) | |
| B15 | §04 s.6 | Dış 3. konuşma adımı live şemasının 2. konuşma kaydına bağlanır; tip/mixer kanalı/otomasyon o kayıttan; bitiş koşulu tamamlanınca dış sıra C'den sürer | çekirdek | |
| B16 | §08 s.16 | Bozulan kümeli ve kümesiz çalışma biçimleri §3 düzenine göre düzeltilir | küme | |
| B17 | §08 s.16 | 7/24 otomasyon farklı düzenlerle sürekli; zorlanarak denenir | küme (senaryolar) | |

Diğer akımlara ait, bu akımla kesişen maddeler: §06/§07 event satırı L/R ve `live` bloğu (1f), §05 mixer etiketleri (67), §03 görünüm ayarları ve ölçekleme (67), §11/§12 (78).
