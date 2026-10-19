## İlk gerçek layout isteği1000×680: actual1568×680 —1000 kabulü FAIL/pending

Root actual native layout driver ilk normal1000 case EXIT0 raporladı; fakat **requested1000×680 → actual1568×680**, minimum1568, natural2249. Driver exit0 yeni caption/value geometry ve Unicode ellipsis kontrolünün geçtiğini gösterir; **1000 genişlikte kabul PASS değildir**. Pencere küçülme/legacy alan görünürlüğü için ayrı inceleme gerekir.

Coordinator `screenshots/1000-normal.png` özgün görüntüyü view_image ile bağımsız inceledi: yeni panel134px içinde üç caption/değer okunur, uzun Unicode başlık/sanatçı sonunda ellipsis, cyan `3 dk 31 sn` görünür. Eski **GERÇEK SIRADAKİ** başlığı `GERÇ-/EK/SIRA-/DAKİ` çok satırlı char-wrap, `Reklama kalan` da bölünüyor; PDF/figure'daki tek-satır bar etiket düzeni bu görünümde korunmuyor. Bu görüntü gerçek1568 genişliktir; dosya adındaki1000 gerçek geometry kabulü değildir.

## PROMPT1/header-grid ve PROMPT5/legacy NEXT layout sahiplerine bildirim

PROMPT-2 yeni paneli ayrı modülde hazır; temel header grid/legacy next_new source'unu eşzamanlı yazmayacağız. Lütfen mevcut `header-next-info` / countdown ve grid responsive dağıtımında yeni panelle birlikte eski caption'ların tek-satır/readable kalmasını sağlayacak minimal alan veya seri hunk paylaşımını ortak kayda bildirin.

Salt okunur source: `header_size_allocated` gerçek allocation genişliği >1560 / >1100 eşikleriyle grid layout seçiyor; constructor başlangıçlayout1. Requested küçüklüğe ulaşmadan minrequest actual genişliği büyütüp daha geniş grid mode seçme feedback'i **bir olasılıktır, kesin kök neden iddiası değildir**. Root/requirements baseline **header panelhidden** minchain/headermin diagnostiğini ölçmeden yeni panelin tüm1568 min'e neden olduğu söylenmez. Başka uygulama panellerinin minwidth etkisi de ölçülmelidir.

## Devam ve kabul sınırı

- İlk driver yeni alan text geometry/ellipsis EXIT0; root mapping20PASS önceki kayıt geçerli.
- Root panelhidden baseline ve minchain araştırmasını yürütüyor. Requirements optional driver diagnostiği kendi dosya sahipliğinde ekleyebilir; header.c grid source'u dış sahiplikle uzlaştırılmalıdır.
- Bekleyen: gerçek1000/1400 geometry/legacycaption correction,6size finalcaptures ve View Preferences görsel; final regression/matrix. Şu aşamada küçük/orta pencere kabulü tamamlanmış değildir.
