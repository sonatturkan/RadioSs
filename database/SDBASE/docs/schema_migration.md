# SDBASE şema v1 / v2 / v3 → v4 göçü

SDBASE 0.7.0 şema v4 kullanır. Eski şema doğrudan güncel yazma motoruyla açılmaz. migration.py içindeki ileri göç kaydı şu üç adımı içerir; bilinmeyen veya kayıtsız sürüm reddedilir.

| Adım | Veri kararı |
|---|---|
| v1 → v2 | plans, schedule_series ve clock_assignments kayıtlarında eksik service_key alanını default olarak tamamlar. Var olan geçerli değeri korur. |
| v2 → v3 | Yeni çıkış/işaret yürütme sözleşmesini etkinleştirir ve eski onay/teslim sayılarını kayıt altına alır. Tarihsel onaylara sonradan çıkış veya cue seçimi uydurmaz. |
| v3 → v4 | Kopyaya bağımsız primary node/timeline/epoch kimliği ve eski yazıcıları engelleyen rol politikası ekler. Kaynak WAL/replication bağlantıları staging'den ayrılır. |

Her adımın kaynak/hedef sürümü, kaynak depo kimliği ve sırası, zamanı ve değişiklik özeti hedef .meta dosyasının migration_history alanında kalır. v1 kaynak üç, v2 iki, v3 bir ileri adım çalıştırır. Kaynak yerinde değiştirilmez.

## Göç işlemi

Eski uygulama süreçlerini durdurun. Ön görünüm kaynak kayıtlarını ve göç adımlarını gösterir; medya baytlarının tam taraması göç sırasında yapılır.

    sdbase --db /veri/eski/RadioS.sdb migrate-preview
    sdbase --db /veri/eski/RadioS.sdb migrate-copy --target-dir /veri/yeni

Hedef henüz var olmayan, kaynak dizinin dışındaki bir dizin olmalıdır. Yalnız doğrulanabilir eski yedek eldeyse aynı kayıt ve medya kontrolleri yedekten çalışır:

    sdbase migrate-backup --backup /yedek/eski-paket --target-dir /veri/yeni

İşlem, DB ve medyanın doğrulanmış fiziksel anlığını ayrı bir geçici alanda açar. Journal sırası ve kontrol toplamları, medya SHA-256/boyutu, kayıt başlıkları, gerekli alanlar, referanslar ve yeni indekslerin benzersizliği denetlenir. Göç adımları yalnız bu kopyayı değiştirir. Güncel sürümle yeniden açma ve yeni yönetici tokenının kullanılabildiği de yayınlamadan önce kontrol edilir.

Yeni kopyanın uçları çevrimdışı olur, eski aktif kabuller ve yürütme izinleri kapatılır. Eski tokenlar iptal edilir; yeni depo kimliği ve yeni yönetici tokenı oluşturulur. Token yalnız komut sonucunda verilir; çıktıyı güvenli saklayın. Bütün kontroller geçince hedef atomik, mevcut hedefi değiştirmeyen rename ile yayımlanır.

Kaynak .sdb, .meta, .checkpoint, medya nesneleri ve yedek paketi değişmez. Yayından önce kaynak depo kimliği ve sırası kilit altında tekrar okunur; arada eski bir istemci commit yapmışsa yeni hedef yayımlanmaz. Bu kontrol eski sürecin daha sonra yazmasını engellemez; fiziksel çıkışı yeni kuruluma bağlamadan önce eski süreç durdurulmalıdır.

## Eski plan onayları

Eski onaylı plan revizyonları, v1 paketler ve olay geçmişi tarihsel veri olarak korunur. Eksik execution_binding veya playback_bindings alanları göç sırasında güncel veriden türetilmez. Bu eski revizyon için yeni snapshot/dağıtım yapılamaz. Tek bir aktif gerçek çıkış yapılandırın veya açık yayın servisi kurun; eski planı plans/fork-draft ile yeni taslağa ayırın, medya ve işaret kararını inceleyip yeniden onaylayın. Yeni revizyon sözleşme v2, somut çıkış ve medya hash'ine bağlı işaretleri taşır.

Eski aktif v1 paketi taşıyan Endpoint.sdb yeniden açılınca çalışma izni kapatılır. Tarihsel kayıt korunur; yeni onaylı v2 paket indirilip normal kabul akışı çalışmadan ses yürütmeye izin verilmez. İleri göç geçmiş onay tarihini değiştirmez.

## Başarısız göç ve recovery

Normal bir hata, geçici kopyayı otomatik kaldırır ve hedefi yayımlamaz. Süreç SIGKILL, makine kapanması veya benzer bir kesilme nedeniyle cleanup çalıştıramazsa sahiplik işaretli gizli staging dizinleri kalabilir. Kısmi göç kaldığı yerden yürütülmez; özgün kaynaktan yeniden başlanır.

    sdbase migrate-recover --target-dir /veri/yeni
    sdbase migrate-recover --target-dir /veri/yeni --cleanup

İlk komut yalnız adayları gösterir; --cleanup silmeyi açıkça etkinleştirir. Göç ve recovery, hedefe özgü aynı flock kilidini tüm iş boyunca tutar. Kilit başka bir süreçteyse recovery reddedilir. PID veya dosyanın yaşından çalışan süreç tahmini yapılmaz. Kilit süreç ölünce işletim sistemi tarafından serbest bırakılır.

Recovery yalnız hedefin **aynı üst dizinindeki**, .sdb-migration-stage.json sahiplik kaydı bu tam hedef ve stage yolunu gösteren staging dizinlerini seçer. İşaretin biçimi, iş kimliği, türü, durumu ve zamanı doğrulanır. İşaretsiz yollar, farklı hedefe ait işaretler, sembolik bağlantılar ve tamamlanmış hedef korunur. Silme öncesi stage inode'u ve işaret baytları tekrar kontrol edilir. Kaynak veya yedek yolu recovery tarafından açılmaz veya silinmez.

.<hedef>.sdb-migration.lock dosyası kalıcıdır; bunu çalışma sırasında silmek farklı süreçlerin farklı kilit inode'larıyla aynı hedefte çalışmasına yol açabilir. migrate-recover inceleme için de bu koordinasyon dosyasını oluşturabilir. Başarıyla yayımlanan hedefte küçük bir hazır staging makbuzu bulunur; DB göç geçmişi ayrıca .meta içinde saklanır. Recovery tamamlanmış hedefe dokunmaz.

Cleanup sonrası asıl migrate-copy veya migrate-backup komutunu yeniden çalıştırın. Var olan tamamlanmış hedefe tekrar yayın yapılmaz; yeni bir hedef seçilmelidir.

## Doğrulama ve geri dönüş

Status, yeni yönetici kimliği, medya doğrulaması, çıkış yapılandırması ve yeni plan kabulünü kontrol edin. Güncel hedefin ayrıca backup-create / backup-verify yedeğini alın. Eski kurulum geri dönüş noktası olarak kalır. Yeni kurulumda sonradan oluşan commit'ler eskiye otomatik taşınmaz; iki kurulum aynı fiziksel yayını eşzamanlı yönetmemelidir.

2026-10-01 doğrulaması:

- Önceki gerçek şema v2 ölçek deposu ileri göçten geçti: 125.101.972 bayt journal, 277.559 toplam kalıcı kayıt, 55.509 katalog öğesi. Kayıtlar korundu, yeni depo kimliğiyle v3 açıldı; özgün DB/meta/checkpoint SHA değerleri değişmedi. Tam kopya/doğrulama/yeniden açma 26,285 saniye sürdü. Bu yerel göç ölçümüdür.
- Eski onay ve v1 snapshot biçimini taşıyan kontrollü v1/v2 .sdb örnekleriyle 26 kontrol geçti: ileri adımlar, salt okuma, token iptali, eski onayın yeniden dağıtılamaması, açık fork/onay sonrası v2 paket, gerçek WAV medyası, yalnız yedekten göç, ikinci adımda hata, kaynak korunması ve temiz hedef.
- Ayrı child süreç staging aşamasında gerçekten SIGKILL ile durduruldu. Recovery kuru çalışması iki sahipsiz stage'i buldu; açık cleanup yalnız işaretli stage'leri sildi, işaretsiz dizini korudu. Özgün kaynak hash'leri değişmedi, yeniden göç geçti. Aktif iş kilidi recovery'yi engelledi; tamamlanmış hedef cleanup'tan sonra korundu.
- Ek 10 yol/yarış kontrolü de geçti: kaynak veya yedek içine hedef oluşturma, sembolik lock/stage yolları, farklı hedefe ait stage işareti ve null servis anahtarı reddedildi/korundu. Arada eski servis commit yapınca hedef yayımlanmadı; eski servisin gerçek commit'i kaybolmadı. İç içe hedef reddi kaynak/yedek dosya envanterini de değiştirmedi.

Bu kontroller kaynak dizin ve ayrı süreçler üzerinde yapılmıştır. Temiz ikinci fiziksel makinede yayın, ses cihazı kesintisizliği ve saha RPO/RTO ölçümü ayrıca doğrulanmalıdır.
