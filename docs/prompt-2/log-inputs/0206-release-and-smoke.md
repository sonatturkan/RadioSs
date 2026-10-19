## PROMPT3 / PDF10 online testine açık PROMPT-2 iş-bitiş devri

Root2 `tests/test_broadcast_online_ui.c` kendi dar include + `test_flow_plays_youtube_events` header assertions işini **bitirdi ve yazımı bıraktı**. Source-edit.lock + path.lock altında canlı içerik/minimal patch kullanıldı. Bu iş-bitiş snapshot SHA256 `76b3bdb3209021bee7dc3250a7e66b8a7ac2474ef4b5dddc8e0859b1655ab24a` (daha yeni dış hunklar varsa güncel kaynak/hash önceliklidir).

**PROMPT3 şimdi aynı case tail'ine kendi flow duration/position/next/pause/completion/STOP assertions'ını seri kilitli minimal hunk ile ekleyebilir.** PDF10 ana dosya sahipliği ve onun dar release sırası da korunmalıdır. PROMPT-2'nin direct `radios_broadcast_online_header_snapshot` identity/duration/paused/preparing/stop testleri ve `broadcast_header.h` include **silinmemeli/değiştirilmemelidir**. Root2 o dosyaya yeni yazım yapmayacak, yalnız test yürütür; yeni ihtiyacı açık seri geri devirle bildirir.

## Aggregate testi güncel kapsam

Architecture `test_broadcast_header.c` **20 case** kaynağı tamamladı. Önceki18 tarihsel kapsamdır; iki yeni rawgen7/latestUi8 ve rawPLAYING/latestUiEOF rejection, generation/state drift fence'i doğrular. Audit raporu20kapsam güncellendi. Gerçek aggregate selector test runtime hâlâ pending; source hazır olması PASS değildir.

## Ana pencere smoke ve bağımsız görüntü kabulü

- Root main-smoke artefaktı önceki **tam link edilmiş 02:03 snapshot**tan üretti; smoke sekiz profili **8/8 PASS** raporladı. Bu başarı son canlı kaynakta devam eden calendar prototype engelini kapsamaz; final source yeniden build/run bekler.
- Coordinator `docs/prompt-2/main-smoke.png` gerçek ana pencereyi view_image ile bağımsız gördü. NEXT barının sağında MEVCUT PROGRAM / ÇALAN MÜZİK / KALAN SÜRE üç alanı görünür; eski GERÇEK SIRADAKİ / GEÇİŞE KALAN alanları korunmuş. Siyah zemin, gri caption, açık değer, cyan kalan vurgu ve ince ayraç PDF/figure diliyle uyumlu. Bu wide idle görüntüde yeni alanlarda clipping veya örtüşme görülmedi; unknown/idle alanlar dash.
- Bu tek wide idle screenshot canlı program/müzik verisi doğrulaması veya 1000/1400/1800 boyları/View Preferences kabulü yerine geçmez. Requirements ayrı native layout driver'ıyla bu görüntüleri üretecek; coordinator bağımsız karşılaştırma yapacak.

## Dış kaynağın güncel build durumu ve sonraki adım

- Root 02:05 integration build'de calendar static preview_selected/append/schedule incomplete hatasının sürmesini raporladı. SCOPE13 kaynak sahibi build-ready bildirsin; PROMPT-2 yazmaz.
- Legacy broadcast form_button unused hatası sahibi tarafından düzeltildi; güncel source syntax PASS bildirildi. Önceki form_button hatası kapanmış tarihsel delta olarak korunur.
- Tamamlandı: Root2 online assertion dar patch işi ve yazım devri; ilk main smoke8profile; bağımsız wide görüntü tasarım kontrolü; aggregate20 test kaynağı.
- Bekleyen: calendar final compile → actualaggregate20/runtime + online gerçek SDBASE wrapper fixture + final main build → native regression ve üç boy/Preferences görsel → son kabul matrisi.
