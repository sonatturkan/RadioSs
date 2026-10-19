"""Native specialist workflow discovery and bounded station-scoped selectors.

This module exposes display metadata and immutable catalogue identities only.
Mutations continue through each existing named workflow's own permission,
revision, preview, and idempotency checks. It never dispatches a raw write.
"""
from __future__ import annotations

import base64
import binascii
from copy import deepcopy
import json

from . import desktop_api as api
from .identity import Forbidden, require
from .storage import Conflict, StoreError

CONTRACT_VERSION = 1
FAMILIES = {
    "transfers": "Diğer radyo programlarından aktarım",
    "services": "Yayın hizmetleri", "catalog": "Kategoriler ve katkılar",
    "metadata": "Özel medya bilgileri", "bulk": "Toplu düzenleme",
    "carts": "Cart ve kesit bankası", "rotation": "Rotasyon kuralları",
    "schedules": "Tekrarlayan yayınlar", "cues": "Onaylı radyo işaretleri",
    "markers": "Çalma işaretleri", "retention": "Geçmiş arşivi",
    "production": "Prodüksiyon ve geçişler", "plans": "Yayın planları",
    "media": "Medya doğrulama", "traffic": "Reklam ve telafi yayınları",
    "outputs": "İstasyon çıkışları", "diagnostics": "Olay ve hata kayıtları",
    "requests": "Dinleyici istekleri", "listeners": "Dinleyici istatistikleri",
    "libretime": "LibreTime aktarımı", "playlists": "Playlist sürümleri",
    "smart-blocks": "Akıllı müzik blokları", "podcasts": "Podcast yayınları",
    "clocks": "Saat formatları", "alerts": "Acil yayın bildirimleri",
    "recording": "Zamanlı kayıt işleri", "now-playing": "Yayında çalan bilgiler",
    "asrun": "Yayın gerçekleşme raporları", "live": "Canlı yayın müdahaleleri",
    "stations": "İstasyon ve uç noktalar", "capacity": "Kapasite ve teslimat sağlığı",
}
OPERATIONS = {
    "mairlist-preview":"mAirList CSV önizle", "mairlist-apply":"mAirList CSV içe aktar",
    "mairlist-export":"mAirList CSV dışa aktar", "radioboss-preview":"RadioBOSS M3U8 önizle",
    "radioboss-apply":"RadioBOSS M3U8 içe aktar", "radioboss-export":"RadioBOSS M3U8 dışa aktar",
    "create": "Oluştur", "list": "Listele", "get": "Ayrıntıları getir",
    "status": "Durumu değiştir", "preview": "Önizle", "apply": "Uygula",
    "undo": "Geri al", "update": "Güncelle", "publish": "Yayımla",
    "fork": "Yeni taslak oluştur", "materialize": "Plana yerleştir",
    "item-create": "Taslak içerik oluştur", "version-create": "İçerik sürümü oluştur",
    "item-share": "İçeriği istasyona bağla", "item-eligibility": "İstasyon kullanımını ayarla",
    "item-lifecycle": "İçerik durumunu değiştir", "version-approve": "İçerik sürümünü onayla",
    "category-create": "Kategori oluştur", "category-update": "Kategoriyi düzenle",
    "item-category": "Parçanın kategorisini değiştir", "person-create": "Kişi veya grup oluştur",
    "contribution-add": "Katkıda bulunan ekle", "group-membership-add": "Grup üyesi ekle",
    "define": "Bilgi alanı tanımla", "set": "Bilgi değerini kaydet", "review": "Öneriyi değerlendir",
    "put-cut": "Kesit kaydet", "list-cuts": "Kesitleri listele", "preview-cut": "Kesit seçimini önizle",
    "resolve-entry": "Kesiti yayın girdisine dönüştür", "put-rule": "Kural kaydet",
    "evaluate": "Kuralları değerlendir", "record-decision": "Karar kaydet",
    "edit": "Düzenle", "propose-dj": "DJ işaretini öner", "approve": "Onayla", "withdraw": "Geri çek",
    "policy-put": "Saklama politikası kaydet", "policy-get": "Saklama politikasını getir",
    "query": "Arşivde ara", "create-transition": "Geçiş oluştur", "create-placement": "Katman ekle",
    "approve-placement": "Katmanı onayla", "reconcile": "Tutarlılığı denetle",
    "rebind-transition": "Geçişi yeniden bağla", "withdraw-placement": "Katmanı geri çek",
    "withdraw-transition": "Geçişi geri çek", "fork-draft": "Plan taslağı oluştur",
    "entry-put": "Plan girdisini kaydet", "distribute": "Uç noktaya dağıt",
    "advertiser-create": "Reklamveren oluştur", "campaign-create": "Kampanya oluştur",
    "campaign-approve": "Kampanyayı onayla", "spot-approve": "Spotu onayla",
    "obligation-create": "Yayın yükümlülüğü oluştur", "preview-break": "Reklam kuşağını önizle",
    "reserve-break": "Reklam kuşağını ayır", "makegood-propose": "Telafi yayını öner",
    "makegood-review": "Telafi önerisini değerlendir", "makegood-preview": "Telafi yayınını önizle",
    "makegood-reserve": "Telafi yayınını ayır", "propose": "Öneri oluştur",
    "apply-target": "Hedefe uygula", "supersede-target": "Hedefi geçersiz kıl",
    "submit": "İsteği gönder", "decide": "İsteği değerlendir", "queue": "Yayın sırasına al",
    "fulfill": "İsteği tamamla", "mount-create": "Dinleyici kaynağı oluştur",
    "mount-update": "Dinleyici kaynağını düzenle", "mount-list": "Dinleyici kaynaklarını listele",
    "sample-record": "Dinleyici ölçümü kaydet", "stats-query": "İstatistik sorgula",
    "mount-status": "Kaynağın durumunu getir", "project": "Aktarım önizlemesi oluştur",
    "versions": "Sürümleri listele", "put-entry": "Girdi kaydet", "remove-entry": "Girdiyi kaldır",
    "put-draft": "Taslağı kaydet", "register": "Kaynak kaydet", "feeds": "Kaynakları listele",
    "auto-download": "Otomatik indirmeyi ayarla", "sync": "Kaynağı eşitle",
    "preview-url": "Bağlantıdan önizle", "preview-text": "XML dosyasından önizle",
    "commit-url": "Bağlantıdan içe aktar", "commit-text": "XML dosyasından içe aktar",
    "latest": "Son bölümü getir", "download": "Bölümü indir", "recover": "İndirmeyi kurtar",
    "put-latest": "Son bölümü plana ekle", "put-slot": "Saat dilimi girdisi kaydet",
    "assign": "Saat formatını ata", "exception": "İstisna ekle",
    "auto-preview": "Otomatik saat akışını önizle", "auto-materialize": "Otomatik saat akışını plana ekle",
    "cancel": "İptal et", "start": "Başlat", "finish": "Tamamla",
    "link-observation": "Çıkış gözlemini bağla", "project-event": "Çalan parça olayını işle",
    "next": "Sıradaki parçayı ayarla", "talk": "Konuşma durumunu ayarla", "current": "Güncel yayını getir",
    "export": "Dışa aktar", "poll": "Bekleyen müdahaleleri getir", "acknowledge": "Müdahaleyi kabul et",
    "report-result": "Sonucu bildir", "link-evidence": "Kanıt bağla", "clone-preview": "İstasyon kopyasını önizle",
    "clone-apply": "İstasyon kopyasını oluştur", "endpoint-create": "Uç nokta oluştur",
    "activate-clone": "Kopyalanan istasyonu etkinleştir", "grant-permission": "İstasyon yetkisi ver",
    "inspect": "Kapasiteyi incele", "delivery-inspect": "Teslimat sağlığını incele",
    "delivery-evaluate": "Teslimat sağlığını değerlendir", "record": "Olay kaydet",
}
LABELS = {
    "source_base64":"Kaynak dosya", "encoding":"Dosya kodlaması", "items":"Dışa aktarılacak parçalar",
    "paths":"Medya dosyası yolları", "path":"Dışa aktarımdaki dosya yolu", "path_bindings":"Kaynak dosya yollarını doğrulanmış medyayla eşleştir",
    "expected_source_sha256":"İncelenen dosya", "station_id":"İstasyon", "source_station_id":"Kaynak istasyon", "endpoint_id":"Uç nokta",
    "code":"Kod", "name":"Ad", "title":"Başlık", "label":"Görünen ad", "key":"Alan anahtarı",
    "scope":"Kapsam", "data_type":"Veri türü", "metadata_class":"Bilgi sınıfı",
    "validation":"Doğrulama kuralları", "required_for":"Gerekli olduğu işlemler",
    "source_policies":"Kaynak politikaları", "field_key":"Bilgi alanı", "target_id":"Hedef kayıt",
    "value":"Değer", "station_override":"İstasyona özel değer", "source_kind":"Kaynak türü",
    "approval_status":"Onay durumu", "clear":"Değeri temizle", "suggestion_id":"Öneri",
    "accept":"Kabul et", "reason":"Gerekçe", "target":"Yeni durum", "status":"Durum",
    "eligible":"Bu istasyonda kullanılabilir", "recording_id":"Kayıt kimliği", "item_id":"Parça", "version_id":"İçerik sürümü", "category_id":"Kategori", "person_id":"Kişi",
    "group_id":"Grup", "display_name":"Görünen ad", "kind":"Tür", "role":"Katkı rolü",
    "cart_id":"Cart", "cut_id":"Kesit", "selection_mode":"Seçim yöntemi", "ordinal":"Sıra",
    "weight":"Ağırlık", "is_fallback":"Yedek kesit", "plan_id":"Yayın planı", "revision_id":"Plan sürümü",
    "plan_revision_id":"Plan sürümü", "entry_id":"Plan girdisi", "from_entry_id":"Önceki girdi",
    "to_entry_id":"Sonraki girdi", "anchor_entry_id":"Bağlanılan girdi", "break_entry_id":"Reklam kuşağı",
    "rule_id":"Rotasyon kuralı", "config":"Kural ayarları", "candidates":"Adaylar",
    "candidate_version_ids":"Aday sürümleri", "evaluated_at":"Değerlendirme zamanı", "decisions":"Kararlar",
    "starts_on":"Başlangıç tarihi", "ends_on":"Bitiş tarihi", "local_time":"Yerel saat",
    "rule":"Tekrar kuralı", "entry_template":"Yayın girdisi", "dst_fold":"Saat değişimi tercihi",
    "service_key":"Yayın hizmeti", "series_id":"Yayın serisi", "service_date":"Yayın tarihi",
    "action":"İşlem", "effective_on":"Geçerlilik tarihi", "cue_id":"Radyo işareti",
    "cue_kind":"İşaret türü", "unit":"Birim", "position_value":"Konum", "position_ms":"Konum (ms)",
    "source_reference":"Kaynak açıklaması", "media_file_id":"Medya dosyası", "media_sha256":"Medya özeti",
    "marker_contract_version":"İşaret sözleşmesi", "cue_in_ms":"Giriş (ms)", "cue_out_ms":"Çıkış (ms)",
    "segue_in_ms":"Geçiş girişi (ms)", "segue_out_ms":"Geçiş çıkışı (ms)", "fade_in_ms":"Açılış (ms)",
    "fade_out_ms":"Kapanış (ms)", "cues":"İşaretler", "selection_source":"Seçim kaynağı",
    "keep_days":"Saklama günleri", "policy":"Politika", "preview_hash":"Önizleme doğrulaması",
    "transition_id":"Geçiş", "content_version_id":"Katman içeriği", "start_reference":"Başlangıç bağı",
    "offset_ms":"Başlangıç farkı (ms)", "gain_db":"Ses düzeyi (dB)", "payload":"Yayın içeriği",
    "event_kind":"Yayın türü", "duration_kind":"Süre türü", "planned_duration_ms":"Yayın süresi (ms)",
    "timing":"Başlama davranışı", "transition":"Geçiş davranışı", "mode":"Yöntem", "window_ms":"Esneklik (ms)",
    "late_policy":"Geç kalma davranışı", "overlap_ms":"Geçiş süresi (ms)", "frequency":"Tekrar sıklığı",
    "weekday":"Haftanın günü", "anchor_date":"Referans tarihi", "nth":"Ayın kaçıncı haftası",
    "input_id":"Ses girişi", "uri":"Yayın adresi", "device_id":"Cihaz", "line":"GPI hattı",
    "delay_ms":"Gecikme (ms)", "duration_ms":"Süre (ms)", "grace_ms":"Gecikme payı (ms)",
    "max_duration_ms":"En uzun süre (ms)", "recording_gap_minutes":"Aynı kaydı beklet (dk)",
    "artist_gap_minutes":"Aynı sanatçıyı beklet (dk)", "group_gap_minutes":"Aynı grubu beklet (dk)",
    "allowed_categories":"İzin verilen kategoriler", "allowed_formats":"İzin verilen biçimler",
    "require_output_observation":"Çıkış gözlemi gerekli", "field":"Bilgi alanı", "op":"Karşılaştırma",
    "all":"Bütün koşullar", "any":"Koşullardan biri", "not":"Koşulun tersi", "ducking_envelope":"Ses kısma eğrisi", "preview":"İncelenen önizleme",
    "expected_preview_hash":"İncelenen önizleme", "expected_content_sha256":"İncelenen podcast içeriği",
    "expected_recommendation_hash":"İncelenen telafi önerisi", "expected_sequence":"İncelenen veri sürümü", "manual_lock":"Elle kilitle",
    "campaign_id":"Kampanya", "advertiser_id":"Reklamveren", "spot_id":"Spot", "obligation_id":"Yükümlülük",
    "request_id":"İstek", "output_id":"Çıkış", "region_id":"Bölge", "required_plays":"Yayın adedi",
    "window_start":"Aralık başlangıcı", "window_end":"Aralık bitişi", "starts_at":"Başlangıç zamanı",
    "ends_at":"Bitiş zamanı", "effective_at":"Geçerlilik zamanı", "old_spot_id":"Önceki spot",
    "new_spot_id":"Yeni spot", "change_id":"Spot değişikliği", "target_id":"Hedef kayıt",
    "mount_id":"Dinleyici kaynağı", "mount_key":"Kaynak kodu", "mount_path":"Yayın adresi yolu",
    "source_namespace":"Kaynak adı", "stale_after_seconds":"Ölçüm geçerliliği (sn)",
    "source_sample_key":"Ölçüm anahtarı", "observed_at":"Gözlem zamanı", "source_state":"Kaynak durumu",
    "listeners":"Dinleyici sayısı", "listener_count":"Dinleyici sayısı", "start":"Başlangıç",
    "end":"Bitiş", "limit":"Kayıt sınırı", "cursor":"Devam noktası", "playlist_id":"Playlist",
    "version_label":"Sürüm adı", "block_id":"Akıllı blok", "rule_tree":"Seçim kuralları",
    "target_duration_ms":"Hedef süre (ms)", "max_overshoot_ms":"İzin verilen süre aşımı (ms)",
    "max_items":"En fazla parça", "selection_order":"Parça sıralaması", "feed_url":"Podcast adresi",
    "feed_id":"Podcast kaynağı", "episode_id":"Podcast bölümü", "xml_text":"XML içeriği",
    "enabled":"Etkin", "template_id":"Saat formatı", "slot_id":"Saat girdisi", "slot_kind":"Girdi türü",
    "assignment_id":"Saat ataması", "local_hour":"Yerel saat", "weekdays":"Haftanın günleri",
    "alert_id":"Acil yayın", "purpose":"Amaç", "geo_codes":"Bölge kodları", "output_ids":"Çıkışlar",
    "tts_text":"Okunacak metin", "tts_language":"Dil", "attempt_id":"Çalma girişimi",
    "job_id":"Kayıt işi", "destination_version_id":"Kayıt hedef sürümü", "source":"Ses kaynağı",
    "schedule":"Zamanlama", "start_trigger":"Başlatma tetikleyicisi", "end_trigger":"Bitirme tetikleyicisi",
    "allow_multiple":"Birden fazla kayda izin ver", "one_shot":"Tek seferlik", "report_id":"Rapor",
    "report_run_id":"Rapor", "format":"Dosya biçimi", "override_id":"Canlı müdahale",
    "new_code":"Yeni istasyon kodu", "new_name":"Yeni istasyon adı", "timezone_name":"Saat dilimi",
    "copy_catalogue_links":"Katalog bağlantılarını kopyala", "user_id":"Kullanıcı", "permission":"Yetki",
    "incident_id":"Olay", "component":"Bileşen", "severity":"Önem derecesi", "impact":"Yayın etkisi",
    "condition":"Koşul", "message":"Açıklama", "details":"Ek ayrıntılar", "occurred_at":"Olay zamanı",
    "correlation_key":"Olay grubu", "event_key":"Olay anahtarı", "reporter_id":"Bildiren",
    "idempotency_key":"İşlem kimliği", "observation_key":"Gözlem kimliği", "external_request_key":"Dış istek kimliği",
}

# Only display identities are read; arbitrary collections and credentials are
# never exposed. Indirect records must resolve through their real station owner.
DIRECT = {
    "categories", "rotation_rules", "schedule_series", "radio_cues", "playback_marker_sets",
    "plans", "carts", "playlists", "smart_blocks", "podcast_feeds", "podcast_episodes",
    "clock_templates", "clock_assignments", "station_items", "recording_jobs", "recording_attempts",
    "listener_requests", "listener_mounts", "listener_samples", "incidents", "diagnostic_events",
    "station_outputs", "broadcast_services", "endpoints", "bulk_jobs", "spot_changes", "spot_change_targets",
    "makegood_requests", "active_acceptances", "asrun_report_runs", "live_overrides", "emergency_alerts", "desktop_media_library",
    "desktop_playlist_entries", "production_placements", "plan_transitions", "obligations",
}
PARENTS = {
    "versions": ("items", "item_id"), "dj_analysis": ("versions", "version_id"), "media_files": ("versions", "version_id"),
    "playlist_versions": ("playlists", "playlist_id"), "playlist_entries": ("playlist_versions", "playlist_version_id"),
    "smart_block_versions": ("smart_blocks", "block_id"), "clock_template_versions": ("clock_templates", "template_id"),
    "clock_slots": ("clock_template_versions", "template_version_id"),
    "clock_exceptions": ("clock_assignments", "assignment_id"), "cart_cuts": ("carts", "cart_id"),
    "schedule_segments": ("schedule_series", "series_id"), "schedule_exceptions": ("schedule_series", "series_id"),
    "plan_revisions": ("plans", "plan_id"), "plan_entries": ("plan_revisions", "plan_revision_id"),
    "spots": ("campaigns", "campaign_id"), "placements": ("obligations", "obligation_id"),
}
GLOBAL = {"field_definitions", "people", "person_memberships", "contributions", "advertisers", "campaigns", "retention_policies"}
COLLECTIONS = DIRECT | set(PARENTS) | GLOBAL | {"items", "stations", "metadata_values", "users"}
REFERENCES = {
    "station_id":"stations", "source_station_id":"stations", "endpoint_id":"endpoints",
    "user_id":"users", "assigned_user_id":"users", "analysis_id":"dj_analysis",
    "item_id":"items", "version_id":"versions", "content_version_id":"versions", "destination_version_id":"versions",
    "category_id":"categories", "person_id":"people", "group_id":"people", "cart_id":"carts", "cut_id":"cart_cuts",
    "rule_id":"rotation_rules", "series_id":"schedule_series", "cue_id":"radio_cues", "plan_id":"plans",
    "revision_id":"plan_revisions", "plan_revision_id":"plan_revisions", "entry_id":"plan_entries",
    "from_entry_id":"plan_entries", "to_entry_id":"plan_entries", "anchor_entry_id":"plan_entries", "break_entry_id":"plan_entries",
    "transition_id":"plan_transitions", "campaign_id":"campaigns", "advertiser_id":"advertisers", "spot_id":"spots",
    "old_spot_id":"spots", "new_spot_id":"spots", "obligation_id":"obligations", "request_id":"listener_requests",
    "mount_id":"listener_mounts", "playlist_id":"playlists", "block_id":"smart_blocks", "feed_id":"podcast_feeds",
    "episode_id":"podcast_episodes", "template_id":"clock_templates", "slot_id":"clock_slots", "assignment_id":"clock_assignments",
    "alert_id":"emergency_alerts", "job_id":"recording_jobs", "incident_id":"incidents", "change_id":"spot_changes",
    "media_file_id":"media_files", "report_run_id":"asrun_report_runs", "override_id":"live_overrides",
}
PUBLIC_FIELDS = {
    "_id", "_rev", "username", "eligible", "station_id", "name", "title", "label", "code", "kind", "status", "state", "lifecycle",
    "key", "scope", "data_type", "metadata_class", "display_name", "selection_mode", "purpose", "service_key",
    "item_id", "version_id", "playlist_id", "playlist_version_id", "block_id", "template_id", "feed_id",
    "plan_id", "plan_revision_id", "ordinal", "scheduled_at", "event_kind", "version_no", "draft_version_id",
    "current_version_id", "published_version_id", "draft_revision_id", "current_revision_id", "endpoint_id",
    "output_id", "category_id", "person_id", "group_id", "role", "cart_id", "cut_id", "campaign_id",
    "advertiser_id", "obligation_id", "spot_id", "rule_id", "series_id", "starts_on", "ends_on", "service_date",
    "start_time", "end_time", "starts_at", "ends_at", "duration_ms", "media_file_id", "mount_key", "mount_path",
    "source_namespace", "condition", "severity", "impact", "component", "message", "updated_at", "created_at",
    "field_id", "target_collection", "target_id", "station_scope", "approval_status", "is_cleared", "value",
    "selection_order", "target_duration_ms", "max_items", "max_overshoot_ms", "content_sha256", "source_fingerprint",
}


def _label(name):
    if name in LABELS:
        return LABELS[name]
    if name.startswith("expected_") and "revision" in name:
        return "Güncel kayıt sürümü"
    return name.replace("_", " ").capitalize()




# Exact preview evidence remains structured data; the operator never copies
# hashes or whole API objects into a text editor to complete the next action.
RESULT_INPUTS = {
    ("transfers/mairlist-apply", "expected_source_sha256"):("transfers/mairlist-preview", ["source_sha256"]),
    ("transfers/radioboss-apply", "expected_source_sha256"):("transfers/radioboss-preview", ["source_sha256"]),
    ("bulk/apply", "expected_revisions"):("bulk/preview", ["items"]),
    ("traffic/reserve-break", "expected_preview_hash"):("traffic/preview-break", ["preview_hash"]),
    ("traffic/makegood-review", "expected_recommendation_hash"):("traffic/makegood-propose", ["request", "recommendation_hash"]),
    ("traffic/makegood-reserve", "expected_preview_hash"):("traffic/makegood-preview", ["preview_hash"]),
    ("traffic/makegood-reserve", "expected_request_revision"):("traffic/makegood-preview", ["makegood_request_revision"]),
    ("playlists/materialize", "expected_sequence"):("playlists/preview", ["sequence"]),
    ("smart-blocks/materialize", "expected_sequence"):("smart-blocks/preview", ["sequence"]),
    ("clocks/materialize", "expected_sequence"):("clocks/preview", ["sequence"]),
    ("clocks/auto-materialize", "expected_sequence"):("clocks/auto-preview", ["sequence"]),
    ("podcasts/commit-url", "expected_content_sha256"):("podcasts/preview-url", ["content_sha256"]),
    ("podcasts/commit-text", "expected_content_sha256"):("podcasts/preview-text", ["content_sha256"]),
    ("podcasts/sync", "expected_content_sha256"):("podcasts/preview-url", ["content_sha256"]),
    ("stations/clone-apply", "preview_hash"):("stations/clone-preview", ["snapshot_hash"]),
    ("retention/apply", "preview"):("retention/preview", []),
}
# These sources validate a persisted natural-key fingerprint before replay.
# Other actions retain the dispatcher's original mutation semantics.
NATURAL_REPLAY = {"diagnostics/record", "listeners/sample-record"}


def _field_schema(name, schema):
    value = deepcopy(schema)
    if name in REFERENCES:
        value["x-radios-reference"] = REFERENCES[name]
        value.setdefault("type", "string")
    if name == "idempotency_key":
        value.update(type="string", format="uuid", **{"x-radios-generated": True})
    if name.startswith("expected_") and name.endswith("revision"):
        value["minimum"] = 1
    if name in {"starts_on", "ends_on", "service_date", "effective_on"}:
        value.setdefault("type", "string")
        value["format"] = "date"
    if name.endswith("_at"):
        value.setdefault("type", "string")
        value["format"] = "date-time"
    return value




def _obj(properties, required=(), **extra):
    return {"type": "object", "properties": {key: dict(value, title=_label(key)) for key, value in properties.items()},
            "required": list(required), "additionalProperties": False, **extra}


def _str(**extra):
    return {"type": "string", **extra}


def _int(minimum=0, **extra):
    return {"type": "integer", "minimum": minimum, **extra}


def _array(items, **extra):
    return {"type": "array", "items": items, **extra}


def _ref(collection):
    return _str(**{"x-radios-reference": collection})


def _entry_schema(*, ordinal=False):
    payload = _obj({"version_id": _ref("versions"), "item_id": _ref("items"),
        "source_id": _str(), "uri": _str(format="uri"), "command": _str(),
        "label": _str(), "release_signal": _str()})
    payload["additionalProperties"] = {}
    properties = {"event_kind": _str(enum=["audio", "live", "stream", "macro", "wait", "marker"], default="audio"),
        "payload": payload, "duration_kind": _str(enum=["fixed", "estimated", "unknown", "none"], default="fixed"),
        "planned_duration_ms": _int(1, maximum=86_400_000),
        "timing": _obj({"mode": _str(enum=["absolute", "flexible"], default="absolute"),
                         "window_ms": _int(), "late_policy": _str(enum=["wait", "cut", "skip"], default="wait")}),
        "transition": _obj({"mode": _str(enum=["sequential", "overlap", "cut"], default="sequential"),
                             "overlap_ms": _int()})}
    if ordinal:
        properties["ordinal"] = _int()
    return _obj(properties, ["event_kind", "payload"] + (["ordinal"] if ordinal else []))


def _smart_rule(depth=2):
    from .smart_blocks import _FIELDS
    leaf = _obj({"field": _str(enum=sorted(_FIELDS)),
                 "op": _str(enum=["eq", "contains", "in", "between"], default="eq"),
                 "value": {"oneOf": [_str(title="Metin"), _array(_str(), title="Metin listesi"),
                                      _array({"type":"number"}, minItems=2, maxItems=2, title="Sayısal aralık")]}}
                , ("field", "op", "value"), title="Alan koşulu")
    if not depth:
        return leaf
    child = _smart_rule(depth - 1)
    return {"oneOf": [leaf,
        _obj({"all": _array(child, minItems=1, maxItems=32)}, ("all",), title="Bütün koşullar"),
        _obj({"any": _array(child, minItems=1, maxItems=32)}, ("any",), title="Koşullardan biri"),
        _obj({"not": child}, ("not",), title="Koşulun tersi")]}


def _business_schema(action_id, name, original):
    """Enrich only with fields accepted by the existing domain validators."""
    family, operation = action_id.split("/", 1)
    value = deepcopy(original)
    if name == "version_id" and family in {"playlists", "smart-blocks", "clocks"}:
        value["x-radios-reference"] = {"playlists":"playlist_versions", "smart-blocks":"smart_block_versions",
                                       "clocks":"clock_template_versions"}[family]
    if name in {"template_version_id", "replacement_version_id"} and family == "clocks":
        value.update(_ref("clock_template_versions"))
    if name == "request_id" and family == "traffic":
        value.update(_ref("makegood_requests"))
    if name == "job_id" and family == "bulk":
        value.update(_ref("bulk_jobs"))
    if name == "target_id" and family == "traffic":
        value.update(_ref("spot_change_targets"))
    if name in {"entry_id"} and family == "playlists":
        value.update(_ref("playlist_entries"))
    if name == "field_key":
        value.update(_ref("field_definitions")); value["x-radios-reference-value"] = "key"
    for field, collection in {"selected_version_id":"versions", "media_version_id":"versions",
                             "smart_block_version_id":"smart_block_versions", "podcast_feed_id":"podcast_feeds",
                             "marker_set_id":"playback_marker_sets", "output_record_id":"station_outputs",
                             "plan_entry_id":"plan_entries", "placement_id":"production_placements", "value_id":"metadata_values"}.items():
        if name == field:
            value.update(_ref(collection))
    if name in {"candidate_version_ids", "item_ids", "cue_ids"}:
        value.update(_array(_ref({"candidate_version_ids":"versions", "item_ids":"items", "cue_ids":"radio_cues"}[name])))
    if name == "weekdays":
        value.update(_array(_int(1, maximum=7), minItems=1, maxItems=7, uniqueItems=True))
    if family == "transfers":
        if name == "source_base64":
            value = _str(format="file-base64", **{"x-radios-file-extensions":["csv"] if operation.startswith("mairlist") else ["m3u8", "m3u"],
                                                   "x-radios-max-bytes":1048576})
        if name == "encoding": value["enum"] = ["utf-8-sig", "utf-8", "cp1254", "cp1252", "iso8859-9"]
        if name == "path_bindings":
            value = {"type":"object", "additionalProperties":_obj({"version_id":_ref("versions"), "media_file_id":_ref("media_files")}, ("version_id", "media_file_id"))}
        if name == "items":
            value = _array(_obj({"item_id":_ref("items"), "version_id":_ref("versions"), "media_file_id":_ref("media_files"),
                "expected_item_revision":{"type":"integer", "x-radios-reference":"items", "x-radios-reference-value":"_rev", "x-radios-revision-of":"item_id"},
                "path":_str()}, ("item_id", "version_id", "media_file_id", "expected_item_revision", "path")), maxItems=1000)
        if name == "paths":
            value = _array(_obj({"media_file_id":_ref("media_files"), "path":_str()}, ("media_file_id", "path")), maxItems=1000)
    if name == "kind" and family == "catalog" and operation == "item-create":
        from .schema import KINDS
        value["enum"] = sorted(KINDS)
    if name == "kind" and family == "catalog" and operation == "person-create":
        value["enum"] = ["person", "group", "organization"]
    if name == "target" and action_id == "catalog/item-lifecycle":
        value["enum"] = ["draft", "active", "archived", "withdrawn", "trashed"]
    if name == "target_id" and family == "metadata":
        value = {"oneOf":[dict(_ref("items"), title="Parça"), dict(_ref("versions"), title="İçerik sürümü")]}
    if name == "entry_template":
        value = _entry_schema(ordinal=family == "schedules")
    if name == "rule_tree":
        value = _smart_rule()
    if name == "config" and family == "rotation":
        value = _obj({"recording_gap_minutes":_int(0, maximum=525600, default=0),
                      "artist_gap_minutes":_int(0, maximum=525600, default=0),
                      "group_gap_minutes":_int(0, maximum=525600, default=0),
                      "allowed_categories":_array(_ref("categories")), "allowed_formats":_array(_str()),
                      "require_output_observation":{"type":"boolean", "default":False}})
    if name == "rule" and family == "schedules":
        value = _obj({"frequency":_str(enum=["weekly", "biweekly", "monthly_nth", "monthly_last"], default="weekly"),
                      "weekday":_int(1, maximum=7), "anchor_date":_str(format="date"), "nth":_int(1, maximum=5)},
                     ("frequency", "weekday"))
    if family == "recording":
        if name == "source":
            value = {"oneOf":[_obj({"kind":_str(enum=["input"], default="input"), "input_id":_str()},
                                    ("kind", "input_id"), title="Ses girişi"),
                              _obj({"kind":_str(enum=["stream"], default="stream"), "uri":_str(format="uri")},
                                    ("kind", "uri"), title="İnternet yayını")]}
        if name == "schedule":
            value = _obj({"weekdays":_array(_int(1, maximum=7), minItems=1, maxItems=7, uniqueItems=True),
                          "starts_on":_str(format="date"), "ends_on":_str(format="date"),
                          "dst_fold":_int(0, maximum=1, default=0)}, ("weekdays", "starts_on"))
        if name in {"start_trigger", "end_trigger"}:
            gpi = {"kind":_str(enum=["gpi"], default="gpi"), "device_id":_str(), "line":_int(0, maximum=65535)}
            if name == "start_trigger":
                gpi.update(window_start=_str(format="time"), window_end=_str(format="time"), delay_ms=_int(0, maximum=3600000, default=0))
                gpi_required = ("kind", "device_id", "line", "window_start", "window_end")
            else:
                gpi["max_duration_ms"] = _int(1, maximum=172800000)
                gpi_required = ("kind", "device_id", "line", "max_duration_ms")
            clock = {"kind":_str(enum=["clock"], default="clock"), "local_time":_str(format="time")}
            if name == "start_trigger":
                clock["grace_ms"] = _int(0, maximum=3600000, default=60000)
            branches = [_obj(clock, ("kind", "local_time"), title="Belirli saat"),
                        _obj(gpi, gpi_required, title="GPI sinyali")]
            if name == "end_trigger":
                branches.insert(0, _obj({"kind":_str(enum=["duration"], default="duration"),
                    "duration_ms":_int(1, maximum=172800000)}, ("kind", "duration_ms"), title="Süre dolunca"))
            value = {"oneOf":branches}
    if name == "ducking_envelope":
        value = _array(_obj({"offset_ms":_int(), "gain_db":{"type":"number", "minimum":-90, "maximum":0}},
                            ("offset_ms", "gain_db")))
    if name == "keep_days":
        from .retention import DEFAULT_KEEP_DAYS
        value = _obj({key:_int(1, maximum=36500, default=days) for key,days in DEFAULT_KEEP_DAYS.items()})
    if name == "policy" and family == "capacity":
        from dataclasses import fields, MISSING
        from .capacity_monitor import CapacityPolicy, DeliveryPolicy
        policy_type = DeliveryPolicy if operation.startswith("delivery-") else CapacityPolicy
        properties, required = {}, []
        for field in fields(policy_type):
            shaped = _str() if field.name == "policy_id" else _int(0)
            if field.default is not MISSING:
                shaped["default"] = field.default
            else:
                required.append(field.name)
            properties[field.name] = shaped
        value = _obj(properties, required)
    enums = {
        ("metadata", "scope"):["item", "version"],
        ("metadata", "data_type"):["string", "integer", "decimal", "boolean", "date", "timestamp", "array", "object"],
        ("metadata", "metadata_class"):["descriptive", "editorial", "technical", "commercial", "operational"],
        ("metadata", "decision"):["approved", "rejected"], ("carts", "selection_mode"):["sequential", "weighted"],
        ("smart-blocks", "selection_order"):["ordered", "shuffle"],
        ("smart-blocks", "selection_mode"):["dynamic", "static"], ("clocks", "slot_kind"):["fixed", "category"],
        ("clocks", "action"):["skip", "replace"], ("requests", "decision"):["accepted", "rejected"],
        ("diagnostics", "component"):["database", "network", "media", "playout", "output", "scheduler", "advertising", "backup", "integration"],
        ("diagnostics", "condition"):["degraded", "failed", "recovered"], ("diagnostics", "severity"):["warning", "critical"],
        ("diagnostics", "impact"):["none", "risk", "on_air", "unknown"], ("asrun", "format"):["json", "csv"],
        ("playlists", "source_kind"):["event", "smart_block", "podcast_feed"],
        ("production", "kind"):["sweeper", "voice_track"],
    }
    if (family, name) in enums:
        value["enum"] = enums[(family, name)]
    if name == "status":
        if family in {"carts", "playlists", "smart-blocks", "podcasts", "recording", "listeners", "schedules", "outputs"}:
            value["enum"] = ["active", "disabled"]
        elif family == "rotation":
            value["enum"] = ["draft", "active", "disabled"]
    if name.startswith("expected_") and "revision" in name:
        stem = name[len("expected_"):].removesuffix("_revision")
        source_field = {"plan":"plan_id", "cart":"cart_id", "cut":"cut_id", "series":"series_id",
                        "version":"version_id", "playlist":"playlist_id", "block":"block_id", "template":"template_id",
                        "entry":"entry_id", "slot":"slot_id", "request":"request_id", "job":"job_id",
                        "target":"target_id", "old_spot":"old_spot_id", "new_spot":"new_spot_id",
                        "override":"override_id"}.get(stem)
        if name == "expected_revision":
            source_field = {"catalog/category-update":"category_id", "catalog/item-category":"item_id",
                "catalog/item-eligibility":"item_id", "catalog/item-lifecycle":"item_id", "catalog/version-approve":"version_id",
                "rotation/put-rule":"rule_id", "rotation/status":"rule_id", "cues/approve":"cue_id", "cues/withdraw":"cue_id",
                "production/approve-placement":"placement_id", "production/withdraw-placement":"placement_id",
                "production/rebind-transition":"transition_id", "production/withdraw-transition":"transition_id",
                "traffic/campaign-approve":"campaign_id", "traffic/makegood-review":"request_id",
                "outputs/update":"output_record_id", "diagnostics/update":"incident_id", "metadata/review":"value_id",
                "requests/decide":"request_id", "requests/queue":"request_id", "requests/fulfill":"request_id",
                "listeners/mount-update":"mount_id", "playlists/status":"playlist_id", "smart-blocks/status":"block_id",
                "podcasts/status":"feed_id", "podcasts/auto-download":"feed_id", "podcasts/download":"episode_id",
                "podcasts/recover":"episode_id", "clocks/assign":"assignment_id", "recording/status":"job_id",
                "alerts/cancel":"alert_id", "stations/activate-clone":"station_id"}.get(action_id)
        collection = REFERENCES.get(source_field)
        overrides = {"playlists":"playlist_versions", "smart-blocks":"smart_block_versions", "clocks":"clock_template_versions"}
        if source_field == "version_id" and family in overrides:
            collection = overrides[family]
        if source_field == "request_id" and family == "traffic": collection = "makegood_requests"
        if source_field == "entry_id" and family == "playlists": collection = "playlist_entries"
        if source_field == "job_id" and family == "bulk": collection = "bulk_jobs"
        if source_field == "target_id" and family == "traffic": collection = "spot_change_targets"
        if source_field == "placement_id": collection = "production_placements"
        if source_field == "output_record_id": collection = "station_outputs"
        if source_field == "value_id": collection = "metadata_values"
        if action_id in {"catalog/item-category", "catalog/item-eligibility"}: collection = "station_items"
        if action_id == "clocks/exception": collection = "clock_exceptions"
        if action_id == "live/propose" and name == "expected_acceptance_revision": collection = "active_acceptances"
        if collection:
            value["x-radios-reference"] = collection
            value["x-radios-reference-value"] = "_rev"
            if source_field:
                value["x-radios-revision-of"] = source_field
    if family == "production" and name == "start_reference":
        from .production import START_REFERENCES
        value["enum"] = sorted(START_REFERENCES)
    if family == "cues":
        from .radio_cues import CUE_KINDS, CUE_UNITS, CUE_SOURCES
        choices = {"cue_kind":CUE_KINDS, "unit":CUE_UNITS, "source_kind":CUE_SOURCES}
        if name in choices: value["enum"] = sorted(choices[name])
    if (action_id, name) in RESULT_INPUTS:
        source, result_path = RESULT_INPUTS[(action_id, name)]
        value["x-radios-result-source"] = source
        value["x-radios-result-path"] = result_path
        if action_id == "bulk/apply" and name == "expected_revisions":
            value["x-radios-result-transform"] = "item_revision_map"
    if name == "value" and family in {"metadata"}:
        value = {"oneOf":[_str(title="Metin"), {"type":"integer", "title":"Tam sayı"},
                 {"type":"number", "title":"Ondalıklı sayı"}, {"type":"boolean", "title":"Evet / hayır"},
                 _array(_str(), title="Değer listesi"), {"type":"object", "additionalProperties":{}, "title":"Alanlar"}]}
    value.setdefault("title", _label(name))
    return value




def _result_collection(action_id):
    family, operation = action_id.split("/", 1)
    primary = {"services":"broadcast_services", "metadata":"metadata_values", "bulk":"bulk_jobs",
        "carts":"carts", "rotation":"rotation_rules", "schedules":"schedule_series", "cues":"radio_cues",
        "markers":"playback_marker_sets", "production":"production_placements", "plans":"plans", "traffic":"campaigns",
        "outputs":"station_outputs", "diagnostics":"incidents", "requests":"listener_requests", "listeners":"listener_mounts",
        "playlists":"playlists", "smart-blocks":"smart_blocks", "podcasts":"podcast_feeds", "clocks":"clock_templates",
        "alerts":"emergency_alerts", "recording":"recording_jobs", "asrun":"asrun_report_runs", "live":"live_overrides",
        "stations":"stations"}.get(family)
    exact = {"metadata/define":"field_definitions", "carts/put-cut":"cart_cuts", "carts/list-cuts":"cart_cuts",
        "catalog/item-create":"items", "catalog/version-create":"versions",
        "catalog/item-share":"station_items", "catalog/item-eligibility":"station_items",
        "catalog/item-lifecycle":"items", "catalog/version-approve":"versions",
        "catalog/category-create":"categories", "catalog/category-update":"categories", "catalog/item-category":"station_items",
        "catalog/person-create":"people", "catalog/contribution-add":"contributions", "catalog/group-membership-add":"person_memberships",
        "production/create-transition":"plan_transitions", "production/rebind-transition":"plan_transitions",
        "production/withdraw-transition":"plan_transitions", "traffic/advertiser-create":"advertisers",
        "traffic/spot-approve":"spots", "traffic/obligation-create":"obligations", "traffic/makegood-propose":"makegood_requests",
        "traffic/makegood-review":"makegood_requests", "traffic/propose":"spot_changes",
        "traffic/apply-target":"spot_change_targets", "traffic/supersede-target":"spot_change_targets",
        "listeners/sample-record":"listener_samples", "podcasts/download":"podcast_episodes", "podcasts/recover":"podcast_episodes",
        "podcasts/list":"podcast_episodes", "podcasts/latest":"podcast_episodes", "clocks/put-slot":"clock_slots",
        "clocks/assign":"clock_assignments", "clocks/exception":"clock_exceptions", "stations/endpoint-create":"endpoints"}
    return exact.get(action_id, primary)


def _result_id_field(action_id):
    return {"plans/create":"plan_id", "playlists/create":"playlist_id", "playlists/fork":"playlist_id", "smart-blocks/create":"block_id",
        "smart-blocks/fork":"block_id", "clocks/create":"template_id", "clocks/fork":"template_id"}.get(action_id, "_id")


@api._consistent_read
def catalogue(store, principal, station_id: str) -> dict:
    """Describe existing specialist actions without granting their permissions."""
    station_id, _ = api._station(store, principal, station_id)
    from .workflow_api import ACTIONS
    from .openapi import _request_schema
    families = []
    for family, label in FAMILIES.items():
        actions = []
        for action_id, action in ACTIONS.items():
            if action_id.split("/", 1)[0] != family:
                continue
            request_schema = _request_schema(action)
            if family == "clocks":
                request_schema["properties"].pop("selection_provenance", None)
            fields = []
            for name, schema in request_schema["properties"].items():
                shaped = _business_schema(action_id, name, _field_schema(name, schema))
                request_schema["properties"][name] = shaped
                fields.append({"name": name, "label": _label(name), "schema": shaped,
                               "required": name in request_schema["required"]})
            operation = action_id.split("/", 1)[1]
            actions.append({"id": action_id, "path": "/v1/workflows/" + action_id,
                            "label": OPERATIONS.get(operation, operation.replace("-", " ").capitalize()),
                            "request_schema": request_schema, "fields": fields,
                            "result_collection": _result_collection(action_id),
                            "result_id_field": _result_id_field(action_id),
                            "idempotent": action_id in NATURAL_REPLAY or "idempotency_key" in request_schema["properties"],
                            "prefill_from_action": {"stations/clone-apply":"stations/clone-preview", "transfers/mairlist-apply":"transfers/mairlist-preview",
                                                    "transfers/radioboss-apply":"transfers/radioboss-preview"}.get(action_id),
                            "download": {"field":"content", "filename":"radios-mairlist.csv" if operation == "mairlist-export" else "radios-radioboss.m3u8",
                                         "mime_type":"text/csv" if operation == "mairlist-export" else "audio/x-mpegurl"}
                                         if family == "transfers" and operation.endswith("export") else None})
        if actions:
            families.append({"id": family, "label": label, "actions": actions})
    return {"contract_version": CONTRACT_VERSION, "station_id": station_id,
            "sequence": store.sequence, "families": families,
            "collections": sorted(COLLECTIONS)}


def _global_manager(store, principal):
    for station in store.scan("stations"):
        require(store, principal, "manage", station["_id"])


def _visible(store, collection, row, station_id, seen=None, principal=None):
    seen = set() if seen is None else seen
    identity = (collection, row["_id"])
    if identity in seen:
        return False
    seen.add(identity)
    if collection == "stations":
        try:
            require(store, principal, "view", row["_id"])
        except Forbidden:
            return False
        return True
    if collection == "users":
        return row["_id"] == principal.user_id or any(
            grant.get("station_id") in {None, station_id} and not grant.get("revoked_at")
            for grant in store.lookup("permissions", "user", row["_id"]))
    if collection in DIRECT:
        return row.get("station_id") == station_id
    if collection == "metadata_values":
        target_collection = row.get("target_collection")
        target = store.get(target_collection, row.get("target_id")) if target_collection in {"items", "versions"} and row.get("target_id") else None
        return bool(row.get("station_scope") in {station_id, "*"} and target and
                    _visible(store, target_collection, target, station_id, seen, principal))
    if collection == "items":
        return bool(store.lookup("station_items", "station_item", station_id, row["_id"]))
    parent = PARENTS.get(collection)
    if parent:
        parent_collection, foreign_key = parent
        owner = store.get(parent_collection, row.get(foreign_key)) if row.get(foreign_key) else None
        return bool(owner and _visible(store, parent_collection, owner, station_id, seen, principal))
    return collection in GLOBAL


def _cursor(value, station_id, collection, sequence):
    if value is None:
        return None
    if not isinstance(value, str) or len(value) > 2048:
        raise StoreError("invalid workflow records cursor")
    try:
        decoded = json.loads(base64.b64decode(value.encode("ascii"), altchars=b"-_", validate=True))
    except (ValueError, UnicodeError, binascii.Error) as exc:
        raise StoreError("invalid workflow records cursor") from exc
    if not isinstance(decoded, dict) or set(decoded) != {"station_id", "collection", "sequence", "last"}:
        raise StoreError("invalid workflow records cursor")
    if decoded["station_id"] != station_id or decoded["collection"] != collection:
        raise StoreError("workflow records cursor belongs to another selection")
    if type(decoded["sequence"]) is not int or decoded["sequence"] != sequence:
        raise Conflict("workflow records changed; reload before continuing")
    return api._id(decoded["last"], "workflow records cursor identity")


@api._consistent_read
def records(store, principal, station_id: str, collection: str, limit: int = 100,
            cursor: str | None = None, record_id: str | None = None) -> dict:
    """Read bounded display rows and revisions for an allowlisted selector."""
    station_id, _ = api._station(store, principal, station_id)
    if not isinstance(collection, str) or collection not in COLLECTIONS:
        raise StoreError("unsupported workflow records collection")
    api._int(limit, "workflow records limit", 1, 200)
    if collection in GLOBAL or collection == "spots":
        # Shared definitions and advertisers are global resources in the engine;
        # showing them to a station-only viewer would cross their ownership.
        _global_manager(store, principal)
    if collection == "users":
        require(store, principal, "manage", station_id)
    sequence = store.sequence
    last = _cursor(cursor, station_id, collection, sequence)
    if record_id is not None:
        record_id = api._id(record_id, "workflow selected record_id")
        if cursor is not None:
            raise StoreError("an exact workflow record read cannot use a cursor")
    found = sorted((row for row in store.scan(collection)
                    if (last is None or row["_id"] > last) and (record_id is None or row["_id"] == record_id)
                    and _visible(store, collection, row, station_id, principal=principal)),
                   key=lambda row: row["_id"])
    page = found[:limit]
    output = []
    for row in page:
        visible = {key: deepcopy(value) for key, value in row.items() if key in PUBLIC_FIELDS
                   and (value is None or isinstance(value, (str, bool, int, float)))}
        visible["label"] = next((str(row[key]) for key in ("name", "title", "display_name", "label", "code", "key", "username")
                                 if row.get(key)), row["_id"])
        if collection == "station_items":
            item = store.get("items", row.get("item_id"))
            if item: visible["label"] = item.get("title") or visible["label"]
        output.append(visible)
    following = None
    if len(found) > limit:
        following = base64.urlsafe_b64encode(json.dumps({"station_id": station_id,
            "collection": collection, "sequence": sequence, "last": page[-1]["_id"]},
            separators=(",", ":")).encode()).decode()
    return {"contract_version": CONTRACT_VERSION, "station_id": station_id,
            "collection": collection, "sequence": sequence, "records": output,
            "next_cursor": following}


def plan_distribute(store, principal, station_id: str, endpoint_id: str, revision_id: str) -> dict:
    """Queue an approved plan revision's immutable snapshot for one endpoint,
    exactly as POST /v1/plans/<id>/distribute does."""
    from .plans import build_snapshot
    from .playout import prepare_distribution
    snapshot = build_snapshot(store, principal, station_id, endpoint_id, revision_id)
    return prepare_distribution(store, principal, station_id, endpoint_id, revision_id, snapshot)


def item_eligibility(store, principal, station_id: str, item_id: str, eligible: bool, *, expected_revision: int) -> dict:
    """Apply the existing catalog decision with the direct HTTP type contract."""
    from .catalog import set_station_eligibility
    api._bool(eligible, "eligible")
    api._int(expected_revision, "expected_revision", 1, 2**63 - 1)
    return set_station_eligibility(store, principal, station_id, item_id, eligible, expected_revision=expected_revision)
