## PROMPT7 / P8 test_playout_ui.c sahibine DAR SERİ DEVİR isteği

PROMPT-2/root **yalnız `tests/test_playout_ui.c::test_independent_player_native_pcm`** case'ine read-only public aggregate header assertions helper/calls eklemek için açık dar seri release istiyor. Calls gerçek play/pause/stop adımlarının ardından private player snapshot titre/name/duration/sourceclear davranışını kontrol edecek. Mevcut waveform/SYNC/PCM/history assertions ve bütün diğer case'ler korunacaktır.

Bu mevcut test gerçek engine/PCM kullanır; root2 exact focused runtime daha önce **1/1 PASS,5.52s**. Komut: `python3 tests/run_playout_ui.py build/prompt-2-tests/test_playout_ui -p /native-playout/independent-player-native-PCM-pause-next-stop-history`. Önceki case header aggregate'i internally assert etmediği için genuine privateView getter branch'inin kanıtı tamam değildi.

**Root2 açık owner release gelmeden bu dosyayı yazmayacak.** Güncel writer prompt7/P8 ise yalnız bu case (ve gerekli tek include/unique helper) için sırayı ortak kayda bildirsin. Canonical source-edit.lock+file.lock, latest read/preimage, minimal hunk; diğer scopes assertions/includes korunur. Alternatif root2 yeni `test_header_playout_ui.c` fixture harness kullanarak ayrı dosyada kanıt sağlayabilir; mevcut dosyaya owner izni olmadan write yapılmaz.

## Cue-out üretim fix ve aggregate22 PASS

- Root getter'da plan override **sonrasında** gerçek `actual_end_ms=raw.cue_out_frame/48` (veya UI cue_out fallback) authoritative yapıldı; manual/retiring media full duration yerine gerçek cue-out remaining hesaplanır, absolute position korunur.
- Root üretim hash progression: önceki mixer32 `03bc36888d49633ef5de5e92b8a89ab6e5380a734642cdb2027fc39652be0956` → intermediate cueout fix → final `e5e3b21d958f4288430ca3e6cc3e28adcad8fbe32017010ab50235533b64cae2`.
- Architecture test source `4f10042cc5f317897cd03a499a23eb24188a31329cf9b1e633f4194a702ced7c`; strict relink **aggregate22/22 PASS**. Eski20PASS ve ilkFAIL ayrı tarihsel kayıtlardır.
- Root cueout fix **öncesi** seçili beş CTest hedefini PASS raporladı: prefs11/playout22/nativeUI10/header8/aggregate20, toplam2.48s. Son22 sonucunun sınırı yeni aggregate cases; beş hedeften önceki başarı otomatik soncueout snapshot tümtestPASS diye genişletilmez.

## Private plan MANUAL_WAIT source QA ve devam

Architecture MANUAL_WAIT/private plan için no-current/no-programme header gap işaret etti. Root üretim fallback'ı yalnız **no-current AND no-programme** iken seçili `view->queue_slot`, sonra activeprivate planı gözlemleyecek şekilde düzeltecek; gerçek seçilmiş active audio source snapshot'ı üzerine yazılmaz. Bu QA delta henüz yeni genuine-private fixture header assertions ile kanıtlanmadı.

Tamamlandı: cueout22 gerçek regression; focused enginePCM öncekiPASS; privateheader kanıt boşluğunun açık tespiti ve dar owner release isteği. Bekleyen: privatefallbackfix/meaningfulheaderassert; baselinegeometry ve6PNG; finalsnapshotregression/matrix.
