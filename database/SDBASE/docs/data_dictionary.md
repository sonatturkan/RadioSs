# SDBASE veri sözlüğü

Bu belge, RadioS için yazılmış SQL kullanmayan merkez veri motorunun **95 iş koleksiyonlu** mevcut veri sözleşmesini açıklar. Sayı, 1 Ekim 2026 kaynak durumunda `len(sdbase.schema.REQUIRED)` ile hesaplandı; kod değişirse bu sayı yeniden üretilmelidir. Kodun bağlayıcı kaynağı **src/sdbase/schema.py** içindeki REQUIRED, REFERENCES, INDEXES ve doğrulayıcılar ile **src/sdbase/storage.py** içindeki işlem kurallarıdır. Ayrıntılı yayın kararları ilgili servis modüllerinde tutulur. Burada tablo adı yerine **koleksiyon** sözcüğü kullanılır; her koleksiyon .sdb günlüğünde anahtarlı JSON belgelerinden oluşur.

## Ortak anahtar ve tip sözleşmesi

- Her merkez belgenin kalıcı anahtarı **_id**; motorun artırdığı revizyonu **_rev** değeridir. Alanlarda belirtilen **UUID**, JSON içinde kanonik UUID metnidir. Servislerin oluşturduğu kimlikler UUID'dir; genel Store API'si bütün koleksiyon anahtarlarının UUID olmasını tek başına zorlamaz.
- **Metin**, **tam sayı**, **Boole**, **nesne** ve **liste** JSON tipleridir. **An** saat dilimi içeren ISO 8601 metni; **gün** YYYY-MM-DD metni; **SHA** 64 küçük onaltılık haneli SHA-256; **enum** izinli metin kümesidir. Bu sütunlar servislerin beklediği tipleri gösterir. REQUIRED yalnız alanın varlığını ve null olmamasını garanti eder; bütün alanlarda aynı derin tip doğrulaması henüz yoktur.
- FK sütunundaki bağlantılar REFERENCES aracılığıyla işlem sırasında UUID ve hedefin varlığı bakımından denetlenir. **Dinamik hedef** olarak belirtilen iki koleksiyonda hedef türü ayrıca doğrulanır. İsteğe bağlı kimlikler, örneğin items.recording_id veya plan girdisinin payload içindeki bağları, genel FK sayılmaz.
- **U** benzersiz ikincil indeksi, **I** benzersiz olmayan tam eşleşme indeksini gösterir. İndeks anahtarındaki null bileşenler indekslenmez; bu nedenle U, eksik/null bileşen için benzersizlik garantisi vermez. Başlık ve Türkçe metin araması bu indeksler üzerinden tam metin sorgusu değildir; search.py ayrı filtreleme yapar.
- Kayıtlar silinse bile motor kimliği tombstone olarak saklar; eski anahtar tekrar kullanılamaz. Schema'daki iş koleksiyonlarının yalnız metadata_values koleksiyonunda fiziksel belge silme açıktır. Diğerleri durum değişimiyle korunur. `_audit` ve `schema.open_radio_store()` içindeki `immutable_collections` kümesi ayrıca yalnız eklenebilir; yeni örnekler diagnostic_events, clock_materializations, emergency_alert_observation_links, recording_events, now_playing_decisions, asrun_report_runs ve live_override_reports, listener_samples ve libretime_ids'dir. Medya nesnesinin fiziksel temizliği, belge durumundan ayrı işletim işidir.
- .sdb işlemi belge güncellemelerini ve _audit kaydını tek kontrol toplamlı çerçevede yazar. Beklenen revizyon, okuma önkoşulu, sekans önkoşulu ve benzersiz indeks denetimi çakışmayı reddeder. Şema sürümü şu anda **2** olarak .meta dosyasında tutulur; farklı sürüm doğrudan açılamaz. V1 verisi için önizlemeli, boş hedefe kopyalayan [göç aracı](schema_migration.md) vardır.

Aşağıdaki alan listeleri **zorunlu** alanları verir. Ek alanlar servislerde bulunabilir; genel Store, belgede fazladan JSON alanını yasaklamaz. Bu nedenle örneğin bir alanın izinli değerlerinin servisçe denetlenmesi ile her doğrudan Store yazımında denetlenmesi aynı garanti değildir.

## Temel ilişki kardinaliteleri

Zorunlu ve REFERENCES içinde bulunan her çocuk FK'si **tam bir mevcut ebeveyne** işaret eder. Ebeveynin çocuk sahibi olması genellikle zorunlu değildir. Önemli istisnalar ve birleşik anahtarlar şunlardır:

| İlişki | Kardinalite ve bağlayıcı koşul |
|---|---|
| stations → endpoints / categories / broadcast_services / plans | Her istasyon 0..çok uç/kategori/servis/plan; servis tam bir istasyona, bir uca ve bir çıkışa bağlıdır. İstasyon+servis anahtarı ve istasyon+yayın günü+servis anahtarı birleşimleri benzersizdir. |
| stations ↔ items, station_items aracılığıyla | Çoktan çoğa; her bağlantı tam bir istasyon ve öğe taşır, istasyon+öğe çifti en fazla bir kez bulunur. |
| items → versions → media_files → media_locations | Her adımda ebeveyn 0..çok çocuk; çocuk tam bir ebeveyne bağlı. Sürüm numarası yalnız aynı item içinde benzersizdir. |
| endpoints ↔ media_files, endpoint_copies aracılığıyla | Çoktan çoğa; her uç+dosya için en fazla bir kopya kaydı. Kopyanın verified durumu bytes'ın gerçekten hazır olduğuna ilişkin ayrı doğrulama gerektirir. |
| items ↔ people, contributions aracılığıyla; people ↔ groups, person_memberships aracılığıyla | Çoktan çoğa. Rol ve tarih bağlamı tutulur; aynı kişi/öğe/rol ve üyelik aralığı için şema düzeyinde ayrı U yoktur. |
| plans → plan_revisions → plan_entries | Her ebeveyn 0..çok çocuk; plan+revizyon numarası ve revizyon+ordinal benzersiz. Onaylı plan istasyon, gün ve servis kapsamında seçilir. |
| schedule_series → segments / exceptions / occurrences | Seri 0..çok dilim, istisna ve somut gün taşır. Seri+tarih için en fazla bir istisna ve bir occurrence vardır. |
| carts → cart_cuts; controller_profiles → versions → banks → pads | Her ebeveyn 0..çok çocuk. Cart+ordinal, profil+sürüm no, sürüm+banka no, banka+pad no ve banka+MIDI adresi benzersiz. |
| plan_revisions + iki plan_entries → plan_transitions → production_placements | Bir revizyon ve sıralı iki girdi için en fazla bir transition; transition 0..çok yerleşim taşıyabilir. Girdilerin aynı revizyona ait olması doğrulanır. |
| endpoint + plan_revision → plan_distributions; endpoint → active_acceptances | Uç+revizyon başına en fazla bir dağıtım. Aktif kabul servis tarafından uç UUID anahtarıyla 0..1 tutulur; bu ikinci sınır ayrı U indeksiyle korunmaz. |
| advertisers → campaigns → spots / obligations → placements | Her reklamveren 0..çok kampanya; kampanya 0..çok spot ve yükümlülük; yerleşim tam bir yükümlülük, spot ve plan girdisi taşır. Aynı sürüm farklı kampanyaların spotu olabilir. |
| playout_attempts → playout_events | Bir girişim 0..çok faz olayı taşır; event_key tüm olaylarda benzersizdir. output_observations ayrıca tutulur, opsiyonel attempt_id için genel FK yoktur. |
| field_definitions → metadata_values / suggestions | Alan 0..çok değer/öneri taşır; alan+hedef+istasyon kapsamında en fazla bir etkin metadata_values belgesi vardır. Hedef items veya versions olarak doğrulanır. |
| playlists → playlist_versions → playlist_entries; smart_blocks → smart_block_versions | Her kimlik 0..çok sürüm taşır; sürüm no ebeveyn kapsamında benzersizdir. Playlist sürümü 0..çok olay/blok girdisi taşır; etkin sıra numarası sürüm kapsamında benzersizdir. Yayımlanmış içerik değiştirilmez; yeni taslak fork edilir. |
| podcast_feeds → podcast_episodes | Bir besleme 0..çok bölüm taşır; kaynak anahtarı, null olmayan GUID ve enclosure URL besleme kapsamında benzersizdir. Besleme ve bölümün istasyonu eşleşir. |
| listener_mounts → listener_samples | Mount tek istasyon/uç/çıkış ve kaynak yoluna bağlıdır; her mount 0..çok değişmez sayım örneği taşır. Mount+source_sample_key en fazla bir kez bulunur. |
| source_namespace + source_id → external_ids | Kaynak kapsamındaki dış kimlik en fazla bir iç hedefe eşlenir; farklı kütüphanelerde aynı source_id çakışmaz. |

## Kimlik, katalog ve medya

| Koleksiyon | Amaç; zorunlu alanlar ve beklenen tip | FK: çocuk → ebeveyn | U / I indeksleri |
|---|---|---|---|
| users | Kullanıcı; username, display_name: metin; status: enum | — | U(username) |
| stations | İstasyon; code, name: metin; timezone: IANA bölge adı; status: enum | — | U(code) |
| endpoints | Yayın noktası; station_id: UUID; code, name: metin; status: enum | station_id → stations | U(station_id, code); I(station_id) |
| broadcast_services | İstasyonun belirli servis/çıkış bağı; station_id, endpoint_id: UUID; service_key, name, output_id: metin; status: enum | station_id → stations; endpoint_id → endpoints | U(station_id, service_key), U(endpoint_id); I(station_id) |
| items | İçerik kimliği; kind, lifecycle: enum; title: metin | — | I(kind), I(lifecycle) |
| station_items | Ortak içeriğin istasyondaki kullanımı; station_id, item_id: UUID; eligible: Boole | station_id → stations; item_id → items | U(station_id, item_id); I(station_id), I(item_id) |
| versions | Editoryal sürüm; item_id: UUID; version_no: pozitif tam sayı; label: metin; status: enum | item_id → items | U(item_id, version_no); I(item_id) |
| media_files | Sürümün dosya temsili; version_id: UUID; status: enum. Doğrulanmış dosyada sha256: SHA ve size_bytes: tam sayı gerekir | version_id → versions | I(version_id), I(sha256) |
| media_locations | Merkezde yönetilen dosya adresi; media_file_id: UUID; storage_uri: metin; status: enum; sha256: SHA; size_bytes: tam sayı | media_file_id → media_files | U(media_file_id, storage_uri); I(media_file_id) |
| endpoint_copies | Yayın noktasındaki dosya kopyası; endpoint_id, media_file_id: UUID; status: enum | endpoint_id → endpoints; media_file_id → media_files | U(endpoint_id, media_file_id); I(endpoint_id) |
| people | Kişi, grup veya kurum; display_name: metin; kind: enum. aliases isteğe bağlı listedir | — | — |
| person_memberships | Kişinin gruba zaman bağlı üyeliği; person_id, group_id: UUID; valid_from/valid_to isteğe bağlı an | person_id, group_id → people | I(person_id) |
| contributions | İçeriğe katkı; item_id, person_id: UUID; role: metin/servis rol enumu | item_id → items; person_id → people | I(item_id), I(person_id) |
| categories | İstasyona özgü kategori; station_id: UUID; code, name: metin | station_id → stations | U(station_id, code) |

**Dosya yolu kimlik değildir.** Yönetilen nesne bytes'ı, RadioS.sdb.objects/ilk-iki-hane/SHA yolunda bulunur. media_locations.storage_uri değeri sdb-object:SHA biçimindedir; kayıtlı özet ve boyut medya kaydıyla eşleşir. Doğrulanmış uç kopyası aynı özeti ve verified_at değerini taşımalıdır. Aynı SHA aynı bytes'ı gösterir; aynı editoryal sürüm veya eser olduğu sonucunu vermez. items.recording_id isteğe bağlı bir değer olarak kullanılabilir, fakat ayrı eser/kayıt koleksiyonu ve ona FK bugün yoktur.

## Plan, seçim, dağıtım ve gerçekleşme

| Koleksiyon | Amaç; zorunlu alanlar ve beklenen tip | FK: çocuk → ebeveyn | U / I indeksleri |
|---|---|---|---|
| plans | İstasyonun yayın günü ve servisi; station_id: UUID; service_date: gün; service_key: metin; state: metin/servis durum değeri | station_id → stations; servis bağı ayrıca doğrulanır | U(station_id, service_date, service_key); I(station_id, service_date) |
| plan_revisions | Plan taslağı/onayı; plan_id: UUID; revision_no: pozitif tam sayı; status: enum | plan_id → plans | U(plan_id, revision_no) |
| plan_entries | Revizyondaki olay; plan_revision_id: UUID; ordinal: tam sayı; scheduled_at: an; event_kind: enum | plan_revision_id → plan_revisions | U(plan_revision_id, ordinal); I(plan_revision_id) |
| schedule_series | İstasyon/servis tekrar serisi; station_id, created_by: UUID; service_key, title, status: metin/enum; starts_on: gün | station_id → stations; created_by → users; servis bağı ayrıca doğrulanır | I(station_id), I(station_id, service_key) |
| schedule_segments | Serinin tarihe göre sürümlenen kural dilimi; series_id: UUID; starts_on: gün; rule, entry_template: nesne; local_time: yerel saat; dst_fold: 0/1; status: enum | series_id → schedule_series | I(series_id) |
| schedule_exceptions | Tek tarih için atla/değiştir/sıfırla; series_id: UUID; service_date: gün; action, status: enum | series_id → schedule_series | U(series_id, service_date) |
| schedule_occurrences | Somutlaştırılmış seri günü; series_id: UUID; service_date: gün; status: enum; source_fingerprint: metin/SHA | series_id → schedule_series; plan/revizyon/girdi bağları doluysa birlikte denetlenir | U(series_id, service_date); I(series_id) |
| carts | İstasyonun çağrılabilir materyal kabı; station_id: UUID; code, title, selection_mode, status: metin | station_id → stations | U(station_id, code) |
| cart_cuts | Cart seçeneği; cart_id, version_id: UUID; ordinal, weight: sayı; status: metin; is_fallback: Boole | cart_id → carts; version_id → versions | U(cart_id, ordinal); I(cart_id) |
| cart_states | Cart seçim durumu; cart_id: UUID | cart_id → carts | — |
| rotation_rules | İstasyon müzik kuralı; station_id: UUID; name: metin; status: enum; config: doğrulanan nesne | station_id → stations | U(station_id, name) |
| rotation_decisions | Açıklanabilir seçim izi; station_id, rule_id, created_by: UUID; evaluated_at: an; decisions, candidate_version_ids: liste | station_id → stations; rule_id → rotation_rules; created_by → users | I(station_id, evaluated_at), I(rule_id) |
| plan_distributions | Uca gönderilen sabit plan görüntüsü; station_id, endpoint_id, revision_id: UUID; status: enum; snapshot: nesne; payload_hash: SHA | station_id → stations; endpoint_id → endpoints; revision_id → plan_revisions | U(endpoint_id, revision_id); I(endpoint_id) |
| active_acceptances | Uçta kabul edilen sahiplik; station_id, endpoint_id, revision_id: UUID; engine_instance: metin; engine_epoch: tam sayı; accepted_at: an | station_id → stations; endpoint_id → endpoints; revision_id → plan_revisions | —; playout servisi belge anahtarı olarak endpoint_id kullanır |
| engine_commands | Süreli uç komutu; station_id, endpoint_id, plan_revision_id: UUID; command_key: metin; state: enum; expires_at: an; engine_epoch: tam sayı; payload: nesne | station_id → stations; endpoint_id → endpoints; plan_revision_id → plan_revisions | U(endpoint_id, command_key); I(endpoint_id) |
| radio_cues | Yayın anlamlı sürüm işareti; station_id, version_id: UUID; cue_kind, unit, source_kind, status: enum; position_value: sayı/metin; position_ms: sıfır veya pozitif tam sayı | station_id → stations; version_id → versions; media_file_id varsa aynı sürüm denetlenir | I(version_id) |
| plan_transitions | Belirli iki plan girdisinin geçişi; station_id, plan_revision_id, from_entry_id, to_entry_id: UUID; context_fingerprint: metin; status: enum; manual_lock: Boole | station_id → stations; revision → plan_revisions; iki girdi → plan_entries | U(plan_revision_id, from_entry_id, to_entry_id); I(plan_revision_id) |
| production_placements | Geçişte sweeper/voice track kullanımı; station_id, transition_id, plan_revision_id, anchor_entry_id, content_version_id: UUID; kind, start_reference, status: enum; offset_ms: tam sayı; context_fingerprint: metin; manual_lock: Boole | station → stations; transition → plan_transitions; revision → plan_revisions; anchor → plan_entries; içerik → versions | I(transition_id), I(plan_revision_id) |
| controller_profiles | İstasyona özgü cihaz/cartwall profili; station_id: UUID; code, name: metin; status: enum | station_id → stations; current_version_id doluysa yayınlanmış sürüm denetlenir | U(station_id, code) |
| controller_profile_versions | Cihaz haritasının sürümü; profile_id: UUID; version_no: pozitif tam sayı; status: enum; device_spec: doğrulanan nesne | profile_id → controller_profiles | U(profile_id, version_no); I(profile_id) |
| controller_banks | Sürümün bankası; profile_version_id: UUID; bank_no: 0..127 tam sayı; name: metin | profile_version_id → controller_profile_versions | U(profile_version_id, bank_no); I(profile_version_id) |
| controller_pads | Bankadaki MIDI adresi ve cart/sürüm hedefi; bank_id, target_id: UUID; pad_no, channel, number: aralıklı tam sayı; label: metin; midi_event, target_kind, trigger_mode: enum | bank_id → controller_banks; target_kind'a göre target_id → aynı istasyondaki carts veya versions | U(bank_id, pad_no), U(bank_id, midi_event, channel, number); I(bank_id) |
| playout_attempts | Tek çalma girişimi; station_id, endpoint_id, version_id: UUID; attempt_key: metin | station_id → stations; endpoint_id → endpoints; version_id → versions | U(endpoint_id, attempt_key); I(station_id) |
| playout_events | Girişimin start/complete/interrupt vb. fazı; attempt_id: UUID; event_key: metin; phase: metin; occurred_at: an | attempt_id → playout_attempts | U(event_key); I(attempt_id) |
| output_observations | Ayrı ses/çıkış gözlemi; station_id, endpoint_id: UUID; occurred_at: an; kind, observation_key: metin | endpoint_id → endpoints; istasyon eşleşmesi ayrıca denetlenir | U(observation_key); I(station_id) |

Bir plans kaydı 0..çok plan_revisions, bir revizyon 0..çok plan_entries içerir. Plan girdisi sekiz türden biridir: audio, live, stream, macro, wait, marker, ad_break, chain. Audio girdisindeki payload.version_id mevcut sürüme işaret etmelidir. Yalnız taslak revizyona girdi yazılabilir; onaylı/superseded revizyon ve onaylı sürüm değişiklikleri koruma altında tutulur. `plans.state` şema doğrulayıcısında izinli durum kümesiyle denetlenir.

Bir schedule_series 0..çok schedule_segments, 0..çok schedule_exceptions ve 0..çok schedule_occurrences taşır. Kural haftalık, iki haftalık, ayın n. hafta günü veya son hafta günü olabilir. Somutlaştırılmış occurrence ancak aynı istasyonun plan/revizyon/girdi üçlüsüne birlikte bağlanınca materialized durumunu alabilir. DST fold ve geçersiz yerel saatler ayrı ele alınır; tarih önizlemesi, plan revizyonunun onayı değildir.

Bir endpoint ve plan revizyonu için en fazla bir dağıtım kaydı vardır. Aktif kabul playout servisi tarafından endpoint_id anahtarıyla tutulur; şemada endpoint_id üzerinde ayrı U indeksi yoktur. Merkez planı, gönderilen snapshot, uç kabulü ve komut/yürütme iddiası ayrı belgelerdir. Bir girişimin çok faz olayı olabilir; aynı event_key ikinci kez yazılamaz. Bağımsız çıkış gözlemi, akustik kanıtın veri kayıt düzeyidir; gerçek hoparlör çıktısı yalnız veritabanından kesinleşmez.

Cart seçiminde cart 1→çok cut; cut belirli sürüme bağlıdır. cart_states için cart_id üzerinde şema düzeyinde U yoktur. Rotasyon kararları geçmiş tamamlanmış çalma iddialarına ve ayarına göre yazılır; isteğe bağlı ayrı çıkış gözlemi aranır. Bu kararlar editoryal onay yerine geçmez.

radio_cues belirli sürümün, onay sırasında da doğrulanmış belirli medya dosyasının işaretidir. Bir plan_transitions belgesi aynı revizyondaki iki farklı girdiyi bağlar; production_placements somut geçişe ve anchor girdiye işaret eder. İçerik değişince context_fingerprint ile eski karar stale olur. Plan revizyonu fork edilirken girdiler kopyalanır; geçişler ve yerleşimler bilinçli olarak kopyalanmaz, yeni revizyonda yeniden kurulup onaylanır. Bu veriler MIDI komutu yürütmez veya ses üretmez.

Bir controller_profiles kaydının 0..çok sürümü, bir sürümün 0..çok bankası ve bankanın 0..çok pad'i vardır. Aynı bankada pad numarası ve MIDI olay/adres birleşimi benzersizdir. Pad hedefi aynı istasyondaki cart veya sürümdür. Yalnız taslak sürümde banka/pad yazılır; published sürümün haritası sabitlenir. Bu koleksiyonlar gelen MIDI'yi dinlemez, LED'e komut göndermez veya ses yürütmez.

## Reklam, aktarım, metadata, hak ve yönetim

| Koleksiyon | Amaç; zorunlu alanlar ve beklenen tip | FK: çocuk → ebeveyn | U / I indeksleri |
|---|---|---|---|
| advertisers | Reklamveren; name: metin | — | — |
| campaigns | Reklam kampanyası; advertiser_id: UUID; name: metin; starts_at, ends_at: an; status: enum | advertiser_id → advertisers | — |
| spots | Kampanyada sürüm kullanımı; campaign_id, version_id: UUID; status: enum | campaign_id → campaigns; version_id → versions | U(campaign_id, version_id); I(version_id) |
| obligations | Yayın yükümlülüğü; campaign_id, station_id: UUID; required_plays: pozitif tam sayı; window_start, window_end: an | campaign_id → campaigns; station_id → stations | — |
| placements | Plan girdisine reklam yerleşimi; obligation_id, spot_id, plan_entry_id: UUID; state: enum | obligation_id → obligations; spot_id → spots; plan_entry_id → plan_entries | — |
| makegood_requests | Reklam telafisi önerisi ve ayrı onay; request_id, station_id, campaign_id, obligation_id, spot_id, version_id, media_file_id, plan_id, plan_revision_id, break_entry_id: UUID; medya SHA, süre, öneri özeti/hash'i, durum ve idempotency anahtarı | Kampanya/spot/yükümlülük/sürüm/medya/plan/ara girdisi ve öneren → ilgili koleksiyonlar | U(station_id, idempotency_key); U(obligation_id, break_entry_id); I(campaign_id) |
| external_ids | Kaynak kapsamlı dış kimlik; source_namespace, source_id: metin; target_collection: enum; target_id: UUID | Dinamik hedef → items, versions, people veya campaigns | U(source_namespace, source_id) |
| import_jobs | Tekrarlanabilir aktarım işi; station_id: UUID; source_namespace, source_sha256, idempotency_key, status: metin; scope, summary: JSON | station_id → stations | U(source_namespace, idempotency_key) |
| import_profiles | İstasyon/kütüphane aktarım ayarı; station_id: UUID; name, library_key: metin; status: enum; config: nesne; config_version: pozitif tam sayı | station_id → stations | U(station_id, name); I(station_id) |
| view_profiles | Kullanıcının istasyon görünümü; station_id, owner_user_id: UUID; name, view_kind: metin/enum; columns: liste; filters: nesne; config_version: pozitif tam sayı | station_id → stations; owner_user_id → users | U(owner_user_id, station_id, name); I(owner_user_id, station_id) |
| dj_analysis | Kaynağa özgü DJ işaret/tempo verisi; version_id: UUID; source_namespace, source_track_id, status: metin; marks, tempos: liste | version_id → versions | U(source_namespace, source_track_id, version_id) |
| import_playlist_nodes | Kaynak playlist/klasör düğümü; station_id: UUID; source_namespace, source_sha256, path, kind, name: metin | station_id → stations | U(station_id, source_namespace, source_sha256, path) |
| import_playlist_entries | Kaynak sıradaki parça; node_id: UUID; ordinal: tam sayı; source_key, source_track_id: metin | node_id → import_playlist_nodes | U(node_id, ordinal) |
| field_definitions | Özelleştirilebilir alan; key: metin; scope, data_type, metadata_class: enum | — | U(key) |
| metadata_values | Alanın öğe/sürüm değeri; field_id, target_id: UUID; target_collection: enum; station_scope, source_kind: metin; is_cleared: Boole | field_id → field_definitions; dinamik hedef → items veya versions | U(field_id, target_collection, target_id, station_scope); I(target_collection, target_id) |
| metadata_suggestions | Onay bekleyen kaynak önerisi; field_id, target_id: UUID; target_collection: enum; proposed_value: JSON; source_kind, status: metin | field_id → field_definitions; dinamik hedef → items veya versions | I(target_collection, target_id) |
| rights_grants | Sürümün istasyon/bölge/çıkış izni; version_id, station_id: UUID; territory, output_id: metin; starts_at, ends_at: an; decision: allow/deny | version_id → versions; station_id → stations | I(version_id) |
| permissions | Kullanıcı yetkisi; user_id: UUID; permission: enum; isteğe bağlı station_id: UUID ve station_scope: metin | user_id → users; station_id var ise stations | U(user_id, permission, station_scope); I(user_id) |
| tokens | Kullanıcı veya uç erişim anahtarı özeti; user_id: UUID; token_hash: SHA; expires_at: an; scope: user/endpoint | user_id → users; endpoint scope ise endpoint_id → endpoints | U(token_hash) |
| bulk_jobs | Toplu düzenleme izi; station_id: UUID; idempotency_key, status: metin; changes: liste/nesne | station_id → stations | U(station_id, idempotency_key) |

Bir advertiser 0..çok campaign; kampanya 0..çok spot ve obligation; obligation 0..çok placement; placement tam bir spot ile tam bir plan girdisine işaret eder. Aynı version_id farklı kampanyalara ayrı spots belgeleriyle bağlanabilir. Reklam seçimi/gerçekleşme hesabı ads.py içindeki işlevlerde yapılır; bu koleksiyonlar tek başına kuşak atama veya dış çıkış kanıtı üretmez.

Bir istasyonun 0..çok import_profiles kaydı, bir kullanıcı ve istasyon çiftinin 0..çok view_profiles kaydı bulunabilir. import_profiles seçim kapsamı, alan politikası, kategori/yol eşlemesi ve cue politikasını sürümler; view_profiles izinli sütunlar ve kayıtlı filtreleri sürümler. Sıra, columns listesinden çıkar; piksel genişliği alanı henüz yoktur. Aktarım profili yalnız yetkili istasyon kullanıcısınca, görünüm yalnız sahibi tarafından değiştirilir.

Alan tanımı 0..çok değer ve öneriye sahip olabilir. metadata_values için station_scope **\*** ortak değer veya istasyon UUID'sidir. İstasyon değeri varsa önce o, sonra ortak değer okunur. Kaynak politikaları take_source, fill_empty, preserve ve review_conflict olarak metadata.py içinde uygulanır; öneri onaylı değer değildir. Rekordbox DJ cue/tempo bilgisi dj_analysis içinde durur; otomatik radyo intro veya gerçekleşmiş yayın sayılmaz.

rights_grants için karar, zaman aralığı, bölge ve çıkış birlikte değerlendirilir. permissions servis/HTTP sınırında kullanılır. tokens açık token değil SHA-256 özetini saklar. _audit, işlem kimliği anahtarına bağlı sequence, actor, committed_at ve before/after değişiklik listesini motorca ekler; istemci doğrudan değiştiremez.

## Yerel kılavuzlardan eklenen uzman koleksiyonlar

Aşağıdaki uzman koleksiyonlar daha sonra eklenen yayın iş akışlarını açıklar. `U` benzersiz, `I` tam eşleşme indeksidir. FK sütununda belirtilmeyen isteğe bağlı ilişki, otomatik yabancı anahtar garantisi değildir. Kaynak ve uygulanmış iş akışı [yerel kılavuz denetiminde](local_manual_audit.md) gösterilir.

### Saat şablonu ve reklam değişikliği

| Koleksiyon ve amaç | Zorunlu FK | U / I indeksleri | Koruma |
|---|---|---|---|
| `clock_templates`: istasyon saat şablonu kimliği | station_id → stations; created_by → users | U(station_id, code) | Durum active/disabled; yayımlanan içerik sürümde tutulur. |
| `clock_template_versions`: şablon revizyonu | template_id → clock_templates; created_by → users | U(template_id, version_no); I(template_id) | draft/published/superseded; yayımlanmış sürümün içeriği korunur. |
| `clock_slots`: sıralı ve saat ofsetli seçim/olay yuvası | template_version_id → clock_template_versions | U(template_version_id, ordinal); I(template_version_id), I(template_version_id, offset_ms) | Yalnız taslak sürüme yazılır. |
| `clock_assignments`: servis kapsamında haftanın saatine ve tarih aralığına şablon ataması; service_key zorunlu | station_id → stations; template_version_id → clock_template_versions; servis bağı ayrıca doğrulanır | I(station_id, service_key, weekday, local_hour), I(station_id) | Aynı servis/saat/etkin aralık çakışması servisçe denetlenir; active/disabled. |
| `clock_exceptions`: tek gün için atla/değiştir/sıfırla | assignment_id → clock_assignments; changed_by → users | U(assignment_id, service_date) | İstisna tarihi ve eylemi doğrulanır. |
| `clock_materializations`: şablon saatinin taslak plan girdilerine bağlanmış izi | assignment_id → clock_assignments; station_id → stations; template_version_id → clock_template_versions; plan_id → plans; plan_revision_id → plan_revisions; created_by → users | U(assignment_id, service_date) | Yalnız eklenebilir; girdi kimlikleri/fingerprint aynı revizyona bağlanır. |
| `spot_changes`: eski spot/sürüm ile yenisi arasında onay bekleyen değişiklik | station_id → stations; campaign_id → campaigns; old/new_spot_id → spots; old/new_version_id → versions; created_by → users | U(station_id, idempotency_key); I(old_spot_id) | Kimlik/değişiklik kapsamı sabit; pending → applied/superseded. |
| `spot_change_targets`: etkilenen gelecek yerleşim ve uçların tek tek durumu | change_id → spot_changes; station_id → stations; placement_id → placements; plan/revision/entry → plans/plan_revisions/plan_entries; old/new_version_id → versions | U(change_id, target_key); I(change_id), I(placement_id) | Kaynak revizyonu ve hedef kimliği sabit; yeni onaylı plan/uç kabulü olmadan applied olmaz. |

### İstek, teşhis ve çıkışa özgü uyarı

| Koleksiyon ve amaç | Zorunlu FK | U / I indeksleri | Koruma |
|---|---|---|---|
| `listener_requests`: dinleyici içerik isteği ve editoryal sonucu | station_id → stations; item_id → items; created_by → users | U(station_id, source_namespace, external_request_key); I(station_id) | pending/accepted/queued/fulfilled/rejected geçişleri ve plan bağları servisçe denetlenir. |
| `incidents`: kök nedene bağlanan açık/yanıtlanan/çözülen arıza | station_id → stations; ilk/son diagnostic event bağı ayrıca doğrulanır | U(station_id, correlation_key); I(station_id) | Bileşen, etki, sorumlu ve sayaç birlikte tutulur; her olay ayrı kayıttır. |
| `diagnostic_events`: arızanın değişmez belirtileri | station_id → stations; incident_id → incidents; reporter_id → users | U(station_id, event_key); I(incident_id) | Yalnız eklenebilir; olay anahtarı ve fingerprint tekrarları ayırır. |
| `station_outputs`: istasyon/uç/çıkış ve opaque coğrafi kod kümesi | station_id → stations; endpoint_id → endpoints; created_by → users | U(station_id, endpoint_id, output_id); I(station_id), I(endpoint_id) | Çıkış kimliği sabit; active/disabled. Kodlar coğrafi standart yorumu değildir. |
| `emergency_alerts`: kapsamı/zamanı belirli acil uyarı kaydı | station_id → stations; created_by → users | U(station_id, source_namespace, external_alert_key); I(station_id) | active/cancelled; veri kaydı CAP protokol alıcısı değildir. |
| `emergency_alert_attempts`: belirli uç/çıkış/coğrafyada uyarıyı yürütme iddiası | station_id → stations; endpoint_id → endpoints; alert_id → emergency_alerts; station_output_id → station_outputs; reported_by → users | U(endpoint_id, attempt_key); I(alert_id), I(endpoint_id) | Girişim kimliği/çıktı kapsamı korunur; durum başlangıç/sonuç olarak ayrılır. |
| `emergency_alert_observation_links`: uyarı iddiası ile ayrı çıkış gözlemi bağı | station_id → stations; endpoint_id → endpoints; alert_id → emergency_alerts; attempt_id → emergency_alert_attempts; observation_id → output_observations; linked_by → users | U(observation_id); I(attempt_id) | Yalnız eklenebilir; ilişki fiziksel yayın kanıtının kendisi değildir. |

### Kayıt işi ve şimdi çalan projeksiyonu

| Koleksiyon ve amaç | Zorunlu FK | U / I indeksleri | Koruma |
|---|---|---|---|
| `recording_jobs`: kaynak, hedef, takvim ve tetikleyici tanımı | station_id → stations; endpoint_id → endpoints; destination_version_id → versions; created_by → users | U(station_id, code); I(endpoint_id) | İşin hedefi/kaynağı ve schedule sözleşmesi denetlenir; active/disabled. |
| `recording_attempts`: bir kayıt fırsatının gerçek girişimi ve sonucu | job_id → recording_jobs; station_id → stations; endpoint_id → endpoints; claimed_by → users | I(job_id, occurrence_date), I(job_id), I(endpoint_id) | recording → completed/partial/failed; terminal kanıt ve medya eşleşmesi gerekir. |
| `recording_events`: girişimin başlangıç/bitiş iddiası | attempt_id → recording_attempts; endpoint_id → endpoints; claimed_by → users | U(endpoint_id, event_key); I(attempt_id) | Yalnız eklenebilir; aynı olay yeniden kullanılamaz. |
| `now_playing_states`: istasyon/uç/çıkış için on-air, next ve talk görüntüsü | station_id → stations; endpoint_id → endpoints; `station_outputs` eşleşmesi ayrıca denetlenir | U(station_id, endpoint_id, output_id) | Deterministik UUIDv5 anahtarı; her lane için zaman işareti geri gidemez; TTL okumada uygulanır. |
| `now_playing_decisions`: projeksiyonun uygulandı/eski/uyuşmadı/süresi doldu kararı | station_id → stations; endpoint_id → endpoints; playout kaynağı ayrıca doğrulanır | U(station_id, endpoint_id, output_id, decision_key); I(station_id, endpoint_id, output_id) | Yalnız eklenebilir; karar kaydı gerçek akustik çıkış veya RDS gönderimi değildir. |
| `asrun_report_runs`: plan, motor olayı ve çıkış gözlemini ayrı kanıt sınıflarıyla tutan tarihli rapor görüntüsü | station_id → stations; created_by → users | U(station_id, idempotency_key); I(station_id) | Yalnız eklenebilir; kaynak sekansı/özeti ve görüntü özeti saklanır, aynı anahtar farklı filtreyle kullanılamaz. CSV/JSON sunumu hukuki telif veya reklam teslim sertifikası değildir. |
| `live_overrides`: kabul edilmiş plan görüntüsündeki tek girdiye süreli skip/replace/insert isteği | station_id → stations; endpoint_id → endpoints; plan_id → plans; plan_revision_id → plan_revisions; plan_entry_id → plan_entries; created_by → users | U(endpoint_id, command_key); I(endpoint_id, state), I(station_id) | Onaylı plan sabit kalır; aktif kabul revizyonu, motor epoch'u, snapshot özeti ve seçilen medyanın uç kopyası denetlenir. pending→accepted/rejected/expired; accepted→execution_claimed/failed; execution_claimed→evidenced. |
| `live_override_reports`: uçtan gelen kabul/sonuç iddiası | override_id → live_overrides; endpoint_id → endpoints | U(endpoint_id, event_key); I(override_id) | Yalnız eklenebilir ve fingerprint kontrollü; daha sonra bağlanan playout girişimi gerçek ses kanıtı değildir. |

`capacity_monitor.py` yeni koleksiyon açmadan mevcut katalog, plan, journal, dosya sistemi, güncel plan dağıtımı ve uç kopyası kayıtlarını istasyona verilen eşiklerle karşılaştırır; alarmını `diagnostic_events/incidents` içinde tutar. `ops_scheduler.py` bu değerlendirmeyi isteğe bağlı periyodik çalıştırır. `station_clone.py` de yeni koleksiyon gerektirmez: `stations`, `categories`, `rotation_rules`, isteğe bağlı `station_items` ve oluşturanın yeni istasyon yetkileri için `permissions` kayıtlarını ayrı kimliklerle tek işlemde yazar. `station_admin.py` daha sonra yeni `endpoints` ve yerel `permissions` kayıtlarıyla klonu etkinleştirebilir. Klon, kaynak yayın kimliklerini ve geçmişini çoğaltmaz.

Bu koleksiyonlar birlikte `len(sdbase.schema.REQUIRED) == 95` sonucunu verir. Sayıyı SDBASE kökünde şu komutla yeniden üretmek mümkündür: `PYTHONDONTWRITEBYTECODE=1 PYTHONPATH=src python3 -c 'from sdbase.schema import REQUIRED; print(len(REQUIRED))'`.

## Yeniden kullanılabilir içerik, podcast, dinleyici ve LibreTime eşlemesi

0.5.0 ile eklenen bu on koleksiyonun servis akışları [yeni özellikler belgesindedir](new_features.md). İçerik tanımının yayımlanması, somut günlük planın ayrı onayını ve ses/hak hazır olma denetimini kaldırmaz.

| Koleksiyon | Amaç; zorunlu alanlar ve beklenen tip | FK: çocuk → ebeveyn | U / I indeksleri |
|---|---|---|---|
| `playlists` | İstasyona özel yeniden kullanılabilir liste; station_id, created_by: UUID; code, name: metin; status: active/disabled; created_at: an | station_id → stations; created_by → users | U(station_id, code); I(station_id) |
| `playlist_versions` | Liste sürümü; playlist_id, created_by: UUID; version_no: pozitif tam sayı; status: draft/published; created_at: an | playlist_id → playlists; created_by → users | U(playlist_id, version_no); I(playlist_id) |
| `playlist_entries` | Sıralı sabit olay, smart block veya podcast beslemesi; playlist_version_id, created_by: UUID; ordinal: sıfır veya pozitif tam sayı; source_kind: event/smart_block/podcast_feed; entry_template: nesne; status: active/removed; created_at: an. active_ordinal aktif girdide ordinal, kaldırılmış girdide null olmalıdır | playlist_version_id → playlist_versions; created_by → users; smart_block_version_id veya podcast_feed_id doluysa aynı istasyonun smart_block_versions/podcast_feeds kaydı ayrıca doğrulanır | U(playlist_version_id, active_ordinal); I(playlist_version_id) |
| `smart_blocks` | İstasyonun seçim bloğu kimliği; station_id, created_by: UUID; code, name: metin; status: active/disabled; created_at: an | station_id → stations; created_by → users | U(station_id, code); I(station_id) |
| `smart_block_versions` | Seçim kuralı sürümü; block_id, created_by: UUID; version_no: pozitif tam sayı; status: draft/published; rule_tree: doğrulanan nesne; target_duration_ms, max_overshoot_ms, max_items: sınırlandırılmış tam sayı; selection_order: ordered/shuffle; selection_mode: static/dynamic; created_at: an | block_id → smart_blocks; created_by → users; yayımlanmış statik sürümde static_version_ids içindeki sürümler ve istasyon bağı ayrıca doğrulanır | U(block_id, version_no); I(block_id) |
| `podcast_feeds` | HTTP(S) RSS/Atom aboneliği; station_id, created_by: UUID; feed_url: metin; status: active/disabled; created_at: an; auto_download_latest isteğe bağlı Boole, varsayılan false | station_id → stations; created_by → users | U(station_id, feed_url); I(station_id) |
| `podcast_episodes` | Bölüm kimliği ve enclosure izleri; station_id, feed_id: UUID; source_key, title, enclosure_url: metin; published_at, first_seen_at, last_seen_at: an; content_hash: SHA. GUID ve katalog/medya/indirme alanları isteğe bağlıdır | station_id → stations; feed_id → podcast_feeds; item_id/version_id/media_file_id doluysa birlikte doğrulanır | U(feed_id, source_key), U(feed_id, guid), U(feed_id, enclosure_url); I(feed_id), I(feed_id, published_at), I(station_id) |
| `listener_mounts` | Kaynak sayım mount'u; station_id, endpoint_id, station_output_id, created_by: UUID; output_id, mount_key, mount_path, source_namespace: metin; stale_after_seconds: sınırlandırılmış tam sayı; status: active/disabled; created_at: an | station_id → stations; endpoint_id → endpoints; station_output_id → station_outputs; created_by → users; istasyon/uç/çıkış eşleşmesi ayrıca doğrulanır | U(station_id, endpoint_id, output_id, mount_key), U(station_id, source_namespace, mount_path); I(station_id), I(endpoint_id) |
| `listener_samples` | Değişmez kaynak iddiası; mount_id, station_id, endpoint_id, reported_by: UUID; output_id, source_namespace, source_sample_key: metin; observed_at, recorded_at: an; source_state: online/offline/unknown; claim_fingerprint: SHA. listeners online durumunda sıfır veya pozitif tam sayı, diğer durumlarda null | mount_id → listener_mounts; station_id → stations; endpoint_id → endpoints; reported_by → users; kaynak ve kapsam mount ile eşleşir | U(mount_id, source_sample_key); I(mount_id), I(station_id) |
| `libretime_ids` | Değişmez UUID/URI→LibreTime tamsayı kimliği; station_id: UUID; source_kind: plan/revision/entry/file/webstream; source_key: metin; external_id: 1..2147483647 tam sayı; created_at: an | station_id → stations; kaynak eşleme servis tarafından onaylı snapshot'tan oluşturulur | U(station_id, source_kind, source_key), U(station_id, external_id); I(station_id) |

Playlist ve smart block yayımlanırken `published_by` ve `published_at` kaydedilir; yayımlanmış sürüm sabittir. Statik blok belirli katalog sürümlerini yayımlama anında dondurur, dinamik blok önizlemede tekrar seçer. Her iki tür de somut yayının zaman/bölge/çıkış haklarını tekrar denetler. Önizleme sekansı değişirse plana atomik uygulama reddedilir.

Playlist podcast_feed girdisi önizleme sırasında son onaylı/doğrulanmış bölümü tam teknik süre ve slot haklarıyla çözer; somut plan seçilmiş bölüm/sürümü sabit kökenle taşır. Podcast sync, auto_download_latest açıksa en yeni bölümün medya indirmesini yürütür. Podcast indirmesi medya nesnesine SHA ile alınır ve katalog sürümü taslak kalır; değişen enclosure eski onayı yeni sese taşımaz. Dinleyici sayımı kaynak ölçümüdür, benzersiz kişi veya işitilen ses kanıtı değildir. Eksik/offline/eski veri sıfır dinleyiciyle eşitlenmez. LibreTime tamsayı kimlikleri RadioS istasyonu kapsamındadır; başka bir LibreTime veritabanının mevcut satır kimlikleri oldukları anlamına gelmez.

## Yerel uç verisi ve fiziksel dosyalar

Merkez RadioS.sdb; RadioS.sdb.meta, RadioS.sdb.checkpoint ve RadioS.sdb.lock ile birlikte çalışır. Yönetilen ses bytes'ı RadioS.sdb.objects içinde bulunur. Bu bytes, JSON belge içine gömülmez.

Yayın noktasındaki **Endpoint.sdb**, merkez koleksiyonlarının aynısı değildir. endpoint_journal.py ayrı tek yazarlı, hash zincirli günlüğünde initialize, snapshot, media_status, activate, fence, command, command_status, ack_queued, ack_status, event, event_attempt ve event_delivered kayıtlarını tutar. Yeniden açılışta kimlik, bekleyen ve aktif snapshot, medya doğrulama durumu, komut, ack ve gönderilmemiş olay durumları yeniden kurulur. endpoint_media.py yerel bytes'ı geçici alana indirip SHA ile doğrular.

Yedek paketi backup.py tarafından merkez .sdb/.meta/.checkpoint anını kısa ortak kilit altında alır; yalnız commit edilmiş media_locations referanslarının nesnelerini kayıtlı SHA/boyutla dahil eder. Manifest doğrulaması ve boş hedefe geri yükleme sonrasında uç kabulü pasifleştirilir; uçlar offline, kopyalar pending, eski uç tokenları geçersiz olur. Bu merkez yedeği yerel Endpoint.sdb günlüğünün veya gerçek ses cihazının otomatik yedeği değildir.

[Genel JSONL/CSV veri paketi](portable_export.md), bütün tanımlı koleksiyonları ve mevcut ek koleksiyonları `_id`/`_rev` ile, silinmiş kimlik izlerini ve medya referans manifestini taşır. Güvenli boş v2 hedefe geri okuma, kaynağın commit dizisini yeni audit dizisi olarak kopyalamaz; yeni depo kimliği ve yönetici tokenı üretir. Eski token/audit geçmişi ve ses baytları bu pakette bulunmaz. Medya konumları yeniden sağlanıp doğrulanana dek `missing` olur.

## Açık model ve uygulama sınırları

- Ayrı **eser ve kayıt** varlıkları, doğrulanan items.recording_id ilişkisi ve kayıtlar arası yapım bağı yoktur.
- İçe alma profili ve kişisel sütun/filtre görünümü vardır; hazır istasyon/program profilleri, sütun genişliği, profil aktarımı ve sistem→istasyon→program→kullanıcı öncelik kuralı henüz yoktur.
- Bütün JSON alanlarının türü, enumu ve aralıkları merkezi şema tarafından aynı sertlikte denetlenmez. Bazı opsiyonel kimlikler ve hizmete özgü ek alanlar doğrudan Store yazımında servis kontrollerini aşabilir; kritik akışlar ilgili servis API'siyle yürütülmelidir.
- cart_states.cart_id ile active_acceptances.endpoint_id için doğrudan U indeksi bulunmaz; ilgili servislerin tek kayıt sözleşmesi doğrudan Store yazımıyla aşılabilir.
- Takvim serisi, prodüksiyon geçişi/cue ve cihaz profili için servisler ile geçici dizin smoke kontrolleri vardır; uzun süreli/saha ürün kabulü tamamlanmadan tüm yayın işlerinde hazır sayılmaz. Trafik UI ve gerçek MIDI/ses yürütmesi ayrıca açıktır.
- Geçmiş arşivinin boyutu, indeks belleği, eşzamanlı okuma gecikmesi, v2 sonrası şema yükseltmeleri, failover ve gerçek ses çıkışı saha/yük kabulü gerektirir. Bütün otomasyon ürünlerinin bütün veri işlerini karşılamak bu sözlükten çıkmaz.
- Kapasite ölçümü yazma öncesi fren değildir; kurulum eşikleri ve periyodik örnekleme görevi operatörce yapılandırılmalıdır. Sınırlı klon yeni istasyona ses/çıkış/plan kurmaz ve fiziksel `.sdb` dosyası kopyasına yeni veritabanı kimliği vermez.

## 0.6.0 onaylı marker ve retention

| Koleksiyon | Alanlar ve ilişki | İndeks / yaşam döngüsü |
|---|---|---|
| `playback_marker_sets` | station_id, version_id, media_file_id, created_by UUID ilişkileri; medya SHA/süresi, cue/segue/fade milisaniyeleri, kaynak cue ID/revision listesi, selection_source ve content_sha256 | I(station_id), I(version_id), I(media_file_id), I(station_id, content_sha256); yalnız yeni approved kayıt; değişmez `_rev=1`. Segue alanları null olabilir. |
| `retention_policies` | code=operational, collection→keep_days, created_by→users, updated_at | U(code); tüm istasyonlarda yönetim yetkisi ve kayıt revision önkoşulu. |
| `retention_segments` | kaynak depo/sıra, zaman, dosya hash/boyut listesi, koleksiyon sayımları, audit sıra aralığı, politika ve kayıt hash'leri | I(created_at); doğrulanmış bakım commit'i oluşturur; değişmez kayıt. Fiziksel yedek dosyalarını taşır. |

Onaylı `plan_revisions` ayrıca `execution_contract_version=2`, somut `execution_binding` ve audio entry UUID→frozen marker `playback_bindings` içerir. Eski göç etmiş revizyonlar geçmiş olarak kalır; yeni dağıtım için fork ve yeniden onay gerekir. `plan_revisions.plan` indeksi planın revizyon okumalarını destekler.
