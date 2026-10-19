# Periyodik yedekleme ve audit arşivi

`tools/ops_scheduler.py`, mevcut bir `.sdb` dosyasından aralıklı olarak iki
bağımsız işlem yürütür:

1. `create_backup(..., checkpoint=False)` ile veritabanı ve yönetilen medya
   nesnelerinin yeni bir kopyasını üretir; `verify_backup` dosya özetlerini ve
   veritabanının yeniden açılıp aynı sırada olduğunu doğrular.
2. `_audit` işlemlerini sıralı JSONL bölümlerine kopyalar; her yeni bölümün
   önceki bölümle bağını ve içeriğini `verify_archive` ile doğrular.

Zamanlayıcı hiçbir eski yedeği, arşivi, medya nesnesini veya canlı audit kaydını
silmez. Bu nedenle düzenli yedek ve arşiv almak canlı `.sdb` disk büyümesini
durdurmaz. Fiziksel ses çıkışının sağlığını ölçmez.

## Kullanım

Komutları `SDBASE` dizininden çalıştırın. Hedef dizin mevcut, başka yönetilen
`backup-` veya `archive-` çıktısı içermeyen ve yalnızca işleticiye açık
(`0700`) bir dizin olmalı; veritabanı dizininden ayrı olmalıdır.

```sh
install -d -m 700 /arsiv/radios-ops
python3 tools/ops_scheduler.py --once \
  --db /veri/RadioS.sdb --output-dir /arsiv/radios-ops \
  --backup-interval-seconds 86400 --archive-interval-seconds 3600

python3 tools/ops_scheduler.py --loop \
  --db /veri/RadioS.sdb --output-dir /arsiv/radios-ops \
  --backup-interval-seconds 86400 --archive-interval-seconds 3600 \
  --poll-seconds 30
```

İlk `--once` her iki işlemi de yürütür. Sonraki çağrılar son deneme zamanına
göre vadesi gelmeyen işleri atlar. Elle hemen yeniden çalıştırmak için yalnızca
`--once` ile `--force` verin. `--loop` sırasında `SIGINT` veya `SIGTERM`, süren
işlem tamamlandıktan sonra döngüyü durdurur. Çıktı her tur için bir JSON
raporudur; `--once` bir işlem hata verirse çıkış kodu 2 döndürür.

Bir tur en çok dört arşiv bölümü oluşturur. Varsayılan bölüm sınırı 10.000
audit işlemi ve 64 MiB'dir. `--max-archive-records`, `--max-archive-bytes`
ve `--max-archive-segments-per-run` sınırları değiştirilebilir. Sınırın
ötesinde backlog kalırsa sonraki döngü turunda devam eder; `--once` için
sonraki çağrı gerekir. Tek audit kaydı byte sınırını aşarsa hata raporlanır,
yarım bölüm yayımlanmaz.

İsteğe bağlı dağıtım ve uç medya alarmı için istasyonları açıkça seçin.
`--token-env` gerçek tokenı değil, tokenın bulunduğu ortam değişkeninin
adını alır. Token istasyonda `repair` izni bulunan kullanıcıya ait olmalı;
uç veya monitör tokenı bu değerlendirme için yeterli değildir. Örnek politika:

```json
{"policy_id":"radyo-ops","max_pending_minutes":15,
 "max_acceptance_minutes":10,"copy_lead_minutes":30,
 "max_scan_rows":100000,"max_sample_ids":20}
```

Bu nesneyi `delivery-policy.json` dosyasına koyduktan sonra örneğin:

```sh
export SDBASE_OPERATOR_TOKEN='...'
python3 tools/ops_scheduler.py --once \
  --db /veri/RadioS.sdb --output-dir /arsiv/radios-ops \
  --station-id 00000000-0000-0000-0000-000000000001 \
  --token-env SDBASE_OPERATOR_TOKEN \
  --delivery-policy delivery-policy.json \
  --delivery-interval-seconds 300
```

Seçilmeyen istasyon için alarm üretmez. Alarm değerlendirmesi hata verse bile
o turdaki yedekleme ve arşivleme kendi sonucuyla devam eder. Yeniden denemede
aynı kalıcı gözlem anahtarı kullanılır; başarılı değerlendirmeden sonra yeni
zaman diliminde yeni anahtar atanır. `unknown` tarama sonucu önceki bozulma
olayını iyileşmiş saymaz. Bu alarmlar yalnızca veritabanı kanıtını ölçer;
ses çıkışının fiziksel olarak dinlenmesini sağlamaz.

Teslimat alarmı merkezdeki `endpoint_copies` kayıtlarını güncel plan
manifestiyle karşılaştırır. Güncel onaylı plan için dağıtım hiç
hazırlanmamışsa `copy_lead_minutes` hazırlık penceresinde bunu da gecikme
olarak bildirir; birden fazla ucu olan bağlanmamış `default` serviste hedef
uç belirsizse sonuç `unknown` kalır. Uç diski kendi başına taramaz; fiziksel ses
çıkışını veya dış bildirim kanalını kanıtlamaz. JSON raporları ve kalıcı
`diagnostic_events`/`incidents` kayıtlarını işletici izleme altyapısına
bağlayın.

## Kalıcı durum ve kurtarma

Zamanlayıcı `scheduler-state.json` ve `scheduler.lock` dosyalarını hedef
dizinde tutar. Tek süreç kilidi ikinci zamanlayıcının aynı çıktı dizinine
başlamasını reddeder. İş başlamadan önce rastgele isimli, henüz bulunmayan
hedef ve deneme zamanı duruma yazılır. Süreç çıktı yayımlandıktan sonra
kesilirse sonraki çalıştırma hedefi doğrulayıp duruma ekler. Yayımlanmamış
hedefte yeniden çalışmayı güvenle dener. Yayımlanmış fakat doğrulanamayan
hedef için iş durur ve hata kaydedilir; dosya otomatik silinmez.

Durum dosyası yokken bu dizinde önceden üretilmiş `backup-` veya `archive-`
çıktısı bulunursa zamanlayıcı zinciri tahmin ederek sürdürmez; durum
kurtarılmalıdır. Durum dosyası SHA-256 özeti kazara bozulmayı yakalar fakat
yetkili bir saldırgana karşı imza değildir. Her arşiv turu kayıtlı arşiv
zincirini sıra 1'den doğrular. Çok uzun zincirde bu tarama zaman alabilir.
Son manifest özetini ayrı güvenilir yerde saklayın. Daha ayrıntılı
doğrulama ve arşiv sorguları için [operatör aracına](ops_archive.md) bakın.

Yedekler yalnızca SDBASE tarafından yönetilen `sdb-object:` medya
nesnelerini içerir; harici dosya yollarındaki ses içeriğini kapsamaz. Denetim
arşivleri eski kayıtların önce/sonra değerlerini taşıdığı için hedef dizin ve
çıktılar yetkili işleticiyle sınırlandırılmalıdır.
