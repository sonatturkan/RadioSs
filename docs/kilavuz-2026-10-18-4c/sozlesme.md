# Kılavuz akımı 4c · Şema / program / küme / planlayıcı — veri sözleşmesi

Kaynak: `prompt/radyo_otomasyonu_kilavuzu.pdf` (18.10.2026). Bu akım PDF §02, §03 (küme seçimi, kümeler arası geçiş, programdan programa devam), §04 (program oluştur, live event, şema seçimi, zaman aralığı, yayın sırası, "2 2 2", konuşma şemasına girip C playliste dönme) ve §08 "kümeli ve kümesiz çalışma" maddelerini kapsar. Diğer akımlar: 1f = yayın sırası/event listesi (S/G, 1.1 alt satırlar, L/R, süre sayaç alanı, event ayarları, yenilenme), 67 = mixer kaynak/etiket, AutoDJ normal form, ölçekleme, görünüm, çalışma sorunları, 78 = ayarlar simgeleri, Import sekmesi, .st Export/Import.

Bütün alanlar isteğe bağlıdır; eski kayıtlar değişmeden okunur. Arka uç (`desktop_automation_calendar.py`, `desktop_api.py`) ve C planlayıcı (`src/core/automation_calendar_plan.c`) aynı kuralı uygular.

## 1. Rotasyon / şema tanımı (`automation_calendar` › `rotation_library[].settings`)

| Alan | Biçim | Anlam |
|---|---|---|
| `automation_steps` | ≤32 × `{ "order": 1..99, "kind": "music"\|"talk"\|"dj"\|"advertisement"\|"jingle"\|"sweeper", "preset_id"?: uuid, "transition"?: {"mode":0..3,"overlap_ms":0..30000} }` | Şemanın **iç sıra** kayıtları (PDF "2 2 2"). `order` tanım içinde tekildir. `preset_id` aynı tanımın `autodj_presets` kimliğidir. Kayıt ya `preset_id` ya `transition` taşır (ikisi birden olamaz); hiçbiri yoksa kayıt yalnız sıra/tip eşleşmesi içindir. |
| `link_transitions` | ≤50 × `{ "from_source_id": uuid, "to_source_id": uuid, "preset_id"?: uuid, "transition"?: {...} }` | Bağlantıya (kaynak A → kaynak B) atanmış **varsayılan şema geçişi**. Kimlikler bu tanımın `sources[].id` değerleridir, çift tekildir. |
| `programme_entry` | `{ "source_order": 1..50, "preset_id"?: uuid, "transition"?: {...}, "alternatives"?: ≤8 × {"weight":0..100,"source_order":1..50,"preset_id"?,"transition"?} }` | Program bittiğinde sonraki programa **hangi sıradan ve hangi geçişle** girileceği. `alternatives` olasılık yolu: ağırlık toplamı ≤100; kalan pay ana girişindir. |

## 1b. Takvim programı (`automation_calendar` › `events[]`)

| Alan | Biçim | Anlam |
|---|---|---|
| `live` | `{ "label": "dj"\|"talk"\|"music", "channels": ≤32 tekil 1..32, "automation_id": uuid\|"", "end_rule": "manual"\|"duration", "duration_ms": 0..86400000 }` | "Program oluştur" penceresinde **Live event** işaretlenen program. `automation_id` seçilen rotasyon şeması (aynı takvimde `automation_schema: true`). Böyle bir programın rotasyon listesi yalnız şemalardan seçilir. Planlayıcı: şemanın ürettiği `kind:"live"` bloklarına kendi `live` değeri yoksa bu nesne varsayılan olarak kopyalanır; şema hiç live adımı üretmediyse programın bloklarına `live` (label/channels) yazılır ki event satırında L/R çalışsın. |
| `schema_entry_source_order` | 1..50 | (çalışma zamanı, kaydedilmez) genel sürücünün planlayıcıya verdiği giriş sırası; `programme_entry` çözümü (bkz. §4). |

## 2. Kaynak satırı (`rotation_library[].sources[]`)

| Alan | Biçim | Anlam |
|---|---|---|
| `source_type` | `"playlist"` (varsayılan) \| `"live"` | `"live"` satırda `playlist_id` **bulunmaz**; `live` nesnesi zorunludur. Diğer zorunlu alanlar (`id, frequency, random, track_random, advert_random, sweeper_random`) aynen verilir. |
| `live` | `{ "title": ≤160, "label": "dj"\|"talk"\|"music", "channels": ≤32 tekil 1..32, "automation_id": uuid\|"", "end_rule": "manual"\|"duration", "duration_ms": 0..86400000 }` | Live event adımı. `automation_id` aynı takvimdeki `automation_schema: true` bir tanımın kimliği (özel live event şeması) ya da boş. `end_rule: "duration"` iken `duration_ms` > 0 zorunlu. |
| `start_date`, `end_date` | `"YYYY-AA-GG"` | Playlistin devrede olduğu tarih aralığı (PDF §04 adım 3). Program tarihi aralığın dışındaysa kaynak o programda kullanılmaz. `start_date ≤ end_date`. |
| `settings.automation_steps` | tanımdaki gibi | Playlistin **kendi** otomasyon kayıtları (iç sıra). |

## 3. Programme bloğu (`programme` › `blocks[]`)

- Yeni blok türü: `kind: "live"` (`PROGRAMME_KINDS` + `programme_model.c`: etiket "Canlı", event başlığı "Live event").
- İsteğe bağlı `live` üyesi: `{ "label", "channels": [1..32], "automation_id": uuid|"", "end_rule": "manual"|"duration"|"schema", "duration_ms" }`. `"schema"` değeri 1f'nin okuduğu takma addır: bitişi planlayıcı/şema belirler (= `duration`).
- 1f (event satırı) L ve R'yi bu nesneye göre bağlar: `label` dj/talk → `channels` + mikrofon; music → `automation_id`'li otomasyon. `live` yoksa R pasiftir.

## 4. Planlayıcı ipuçları (hazırlanan program `tracks[]`)

- Kaynak değişen her sınırda (A → B) `calendar_transition` şu öncelikle seçilir: **(1)** "2 2 2": aktif şemanın `automation_steps`, A'nın ve B'nin `settings.automation_steps` kayıtları `order` 1, 2, 3… sırasıyla taranır; üç tarafta da bulunan ve `kind`'ı sınırın beklediği tipe uyan **ilk** kayıt seçilir; şemadaki o kaydın `preset_id`/`transition`'ı uygulanır. Beklenen tip: giren öğe music ise `music`; reklam/jingle/sweeper ise o tür; giren adım live ise `live.label`. **(2)** eşleşme yoksa `link_transitions` içindeki (A.id → B.id) kaydı. **(3)** o da yoksa mevcut kural (tasarlanmış ön ayar → tür kuralı → rotasyon geçişleri).
- Live adımı `tracks[]` içinde medya satırı üretmez; `config.blocks[]`'a `kind:"live"` + `live` bloğu eklenir ve adımdan **önceki** medya satırına `live_step` ipucu yazılır: `{ "block_id", "label", "channels", "automation_id", "end_rule", "duration_ms", "outer_order": dış sıra numarası, "inner_order": eşleşen iç sıra numarası|0 }`. `end_rule: "duration"` ise adımdan sonraki ilk medya satırının `calendar_constraints.earliest_utc_us` değeri sınır + `duration_ms` olur; `"manual"` ise sonraki satır elle geçiş (operatör / L kapanışı) bekler. Konuşma adımı bitince dış sıra bir sonraki kaynaktan (C playlist) sürer.
- `programme_entry`: genel sürücü yeni programa geçerken planlayıcıya `start_source_order` verir; planlayıcı ilk turu o kaynaktan başlatır ve program sınırındaki geçişi `programme_entry` (ya da çekilen alternatif) ile işaretler.

## 5. Mixer kanalları (67 akımının sözleşmesi, okunur)

- `dj_mixer` belgesi: `channels[i].label` = `"dj" | "talk" | "music" | null`; `channels[i].source_deck`: −1 kaynak yok, −2 Stüdyo mikrofonu, 0..31 = "Oynatıcı N+1"; isteğe bağlı `channels[i].input = {"kind":"capture"|"network","device":"…","name":"…"}` (analog/ağ girişi; görünen ad `name`).
- Kanal adları `channel` türündeki belgenin `names` nesnesinde (`"1".."32"`; yoksa "Kanal N").
- Motor: L = `radios_audio_engine_set_microphone_gate` (mandal), R = `radios_audio_engine_set_microphone_ptt` (anlık, 67 ekliyor). İkisi de transporta dokunmaz.

## 6. Sonradan eklenenler (18.10.2026, Codex ve akım B)

- `automation_steps[]` kaydı isteğe bağlı `channels` (tekil 1..32) taşıyabilir: live event iç sıra kaydının mixer kanalları ("Live event kanallarını bu kayıttan al"). Yalnız `music`, `talk`, `dj` türlerinde.
- Çalışma anında genel sürücü biten programın şemasından `schema_entry_settings` (`programme_entry` + `autodj_presets`) nesnesini sonraki programın olayına koyar (kaydedilmez); planlayıcı önce bunu, yoksa olayın kendi şemasının `programme_entry` değerini kullanır.
- Tür kuralı: bağlantı ve iç sıra otomasyonlarında `preset_id`, `autodj_automation.kinds.<tür>.presets` (ya da tek türlü zaman dilimi) ile türlere atanmışsa yalnız o türlere giren sınırda uygulanır (`radios_automation_preset_kind_usable`); atanmamış ön ayar her sınırda geçerlidir. Arka uç izin verici kalır; kural arayüzde ve planlayıcıda uygulanır.
