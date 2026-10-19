## Manual/retiring gerçek cue-out kalan süre düzeltmesi için yeni regression22

Architecture 02:13 UTC salt okunur son aggregate API review, manual veya owned retiring/noncurrent source'ta full source duration180000ms gösterilirken actual cue-out90000ms uygulanmadığını buldu. Bu P2 gerçek kalan dk/sn davranışı için meaningful bug'dır; root üretim fix sahibi olarak yetki aldı.

Architecture yalnız sahip olduğu `tests/test_broadcast_header.c` içine iki regression ekledi: manual actual cue-out ve owned retiring/noncurrent actual cue-out. Kaynak toplam **22 case**; test SHA256 `4f10042cc5f317897cd03a499a23eb24188a31329cf9b1e633f4194a702ced7c`. Yeni22runtime pending; eski20PASS iki yeni case'i kapsamaz.

Root fix önerisi gerçek `raw.cue_out_frame` değerinin pozitif olduğunda 48frame/ms ile bound duration override'a aktarılması; absolute position korunmalıdır. Bu root üretim delta'sı henüz yeni22retetstPASS olarak raporlanmadı. Private queue salt okunur QA devam ediyor; architecture üretim kaynak sahipliğini genişletmedi.

Tamamlandı: mixer32 sourcefix read-only teyit ve gerçek20PASS önceki kanıt; yeni iki actual cueout regression source. Bekleyen: üretimcueoutfix+22case runtime; küçük/orta layout minimumwidth teşhisi; finalregression/görsel/matrix.
