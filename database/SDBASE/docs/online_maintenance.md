# Yazmalar sürerken checkpoint hazırlama

SDBASE 0.8.0 büyük kurtarma görüntüsünü store yazma kilidinin dışında hazırlar. Şema v4 ve `.sdb` journal biçimi korunur. Son yayın adımı sınırlı bir exclusive kilit kullanır.

```sh
sdbase --db /veri/RadioS.sdb checkpoint-online --max-tail-bytes 67108864 --max-tail-frames 10000
sdbase --db /veri/RadioS.sdb maintenance-loop --interval 60 --journal-threshold-bytes 67108864
sdbase --db /veri/RadioS.sdb maintenance-recover
sdbase --db /veri/RadioS.sdb maintenance-recover --cleanup
```

Python arabirimi `store.checkpoint_online(...)` veya `online_maintenance.checkpoint_online(store, ...)` şeklindedir. `run_online_maintenance` bir stop event ve sonuç iterator'ü kullanır. Sahip süreç sonucu kaydeder ve görev yaşam döngüsünü yönetir. CLI Ctrl+C ile durur.

## Hazırlama ve yayınlama

1. Kısa shared kilitte **N** işlem sırasının record map'leri, tombstone'ları, retired unique key'leri ve collection generation'ları alınır. Commit record envelope'ları yenileriyle değiştirir; sonraki commit eski görüntüyü değiştirmez.
2. `.sdb.maintenance/<job-uuid>/checkpoint.pending` akış halinde canonical JSON ve SHA256 ile yazılır, fsync edilir. Bütün JSON için ek encoded buffer oluşturulmaz. Başka süreçler bu sırada journal'a yazabilir.
3. Exclusive kilit alınır. Store kimliği, timeline, epoch ve schema aynı olmalıdır. Başka checkpoint veya retention yayımlanmışsa iş **deferred** olur; eski görüntü yeni sıcak depo üzerinde yayımlanmaz.
4. **N+1 … M** frame'lerinin tamlığı, checksum'u ve kesintisiz sırası doğrulanır. Yalnız suffix ayrı journal dosyasına kopyalanır. Bayt/frame bütçesi aşılmışsa ertelenir. WAL açıksa arşiv M'ye kadar mühürlenmiş olmalıdır; eksik arşiv de ertelenir.
5. Checkpoint önce, suffix journal sonra atomic replace ve directory fsync ile yayımlanır. Metadata generation güncellenir. Bakım yeni domain transaction veya audit sequence oluşturmaz.

Sonuç captured/current sequence, retained transaction sayısı, journal baytları, payload SHA256 ve capture/prepare/publish-lock-wait/publish-lock-held/total sürelerini verir. `deferred` commit kaybı değildir; yeni capture ile tekrar denenebilir. WAL hatası için [WAL bakımını](wal_recovery.md) tamamlayın.

## Süreç kapanması ve iş temizliği

Yeni checkpoint ile eski journal birlikte kalırsa replay checkpoint'in temsil ettiği frame'leri atlar. Yeni checkpoint ve suffix journal yayımlandıysa suffix son commit'e götürür. Metadata yazılmadan kapanma checkpoint inode/size/mtime değişimi sayesinde önceden açık handle'larda fark edilir.

Tek maintenance lock ağır işleri sınırlar; ikinci iş hemen reddedilir. `maintenance-recover` sadece canonical UUID, doğru format/database marker'ı ve izinli regular dosyaları olan terk edilmiş stage'leri listeler. `--cleanup` bunları kaldırır. Symlink, foreign/eksik marker ve bilinmeyen dosya korunur. DB onarılmaz veya geri alınmaz. Güncel primary rolü gerekir.

## Sınırlar

- Capture RAM map'lerini kopyalar; maliyeti kayıt sayısıyla artar. Güncel state/hash indeksleri bellekte kalır.
- Serialization ve hazırlık fsync'i kilit dışındadır; CPU/disk yükü devam eder. Suffix doğrulama/kopya/fsync ve publication kilit içindedir; sıfır duraklama garantisi yoktur.
- Varsayılan suffix 64 MiB/10.000 transaction'dır. Çok hızlı yazmalar işi erteletebilir.
- Retention compaction, managed media kopyası ve klasik `checkpoint` ayrı kilit sürelerine sahiptir. WAL GC, diskten tüm state paging ve paralel yazıcı commit uygulanmadı.

## Kaynak esini ve doğrulama

Yerel PostgreSQL `access/transam/xlog.c` içindeki `CreateCheckPoint`/`CheckPointGuts`, hazırlık ve recovery sınırı için; SQLite `src/wal.c` durability/checkpoint sırası için incelendi. SDBASE kendi sequence/checksum/journal/retention sözleşmesine göre bağımsız Python kodudur. [SQLite online backup](https://www.sqlite.org/backup.html) ayrı motorun tasarımını açıklar; SDBASE o API'yi çağırmaz.

```sh
PYTHONDONTWRITEBYTECODE=1 python3 tools/maintenance_acceptance.py --report /tmp/sdbase-maintenance.json
```

46 kontrol: ayrı süreçten beş concurrent commit, üç gerçek subprocess crash sınırı, cached/fresh handle, WAL/PITR/standby, retention interleaving, frame/byte bütçesi, bozuk journal reddi, sahipliği doğrulanmış cleanup ve rol kapısı. Üretim yolu kabul etmez. Gerçek ikinci makine, güç kesintisi veya yıllar süren yayın kanıtı değildir.
