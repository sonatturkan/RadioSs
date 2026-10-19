"""RadioS collection contracts and exact-match index definitions.

The storage engine is generic. These validators make a RadioS store reject
broken references, invalid lifecycle states and duplicate scoped identities.
"""

from __future__ import annotations

from datetime import date, datetime, time, timedelta, timezone
import hashlib
import json
from pathlib import Path
from typing import Any, Mapping
from urllib.parse import urlsplit
from uuid import UUID
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError

from . import _platform
from .storage import IndexSpec, Store, StoreError


SCHEMA_VERSION = 4


KINDS = {"music", "advertisement", "jingle", "sweeper", "news", "program",
         "announcement", "effect", "voice_track", "other"}
STATUS = {
    "desktop_media_library": {"active", "disabled"},
    "desktop_media_markers": {"draft"},
    "desktop_configurations": {"active", "disabled"},
    "desktop_playlist_entries": {"active", "removed"},
    "users": {"active", "disabled"},
    "stations": {"active", "disabled"},
    "broadcast_services": {"active", "disabled"},
    "endpoints": {"online", "offline", "retired"},
    "items": {"draft", "active", "archived", "withdrawn", "trashed"},
    "versions": {"draft", "review", "approved", "rejected", "withdrawn"},
    "media_files": {"uploading", "received", "technical_check", "metadata_check",
                    "awaiting_approval", "distributing", "verified", "rejected", "error", "deleted"},
    "endpoint_copies": {"pending", "copying", "present", "verified", "missing", "corrupt", "deleted", "error"},
    "plans": {"draft", "approved", "distributed", "active", "completed", "cancelled"},
    "plan_revisions": {"draft", "approved", "superseded"},
    "campaigns": {"draft", "approved", "active", "paused", "completed", "cancelled"},
    "spots": {"approved", "withdrawn"},
    "placements": {"planned", "skipped", "completed", "failed", "cancelled"},
    "makegood_requests": {"proposed", "approved", "rejected", "reserved", "cancelled"},
    "rotation_rules": {"draft", "active", "disabled"},
    "import_profiles": {"active", "disabled"},
    "schedule_series": {"active", "disabled"},
    "schedule_segments": {"active", "superseded"},
    "schedule_exceptions": {"active", "inactive"},
    "schedule_occurrences": {"materialized", "skipped"},
    "radio_cues": {"draft", "approved", "withdrawn"},
    "playback_marker_sets": {"approved"},
    "plan_transitions": {"draft", "stale", "withdrawn"},
    "production_placements": {"draft", "approved", "stale", "withdrawn"},
    "controller_profiles": {"active", "disabled"},
    "controller_profile_versions": {"draft", "published", "superseded"},
    "carts": {"active", "disabled"},
    "cart_cuts": {"active", "disabled"},
    "dj_analysis": {"source_only"},
    "import_jobs": {"completed"},
    "bulk_jobs": {"completed", "undone"},
    "metadata_suggestions": {"pending", "accepted", "rejected"},
    "spot_changes": {"pending", "applied", "superseded"},
    "spot_change_targets": {"pending", "applied", "superseded"},
    "listener_requests": {"pending", "accepted", "rejected", "queued", "fulfilled"},
    "listener_mounts": {"active", "disabled"},
    "podcast_feeds": {"active", "disabled"},
    "playlists": {"active", "disabled"},
    "playlist_versions": {"draft", "published"},
    "playlist_entries": {"active", "removed"},
    "smart_blocks": {"active", "disabled"},
    "smart_block_versions": {"draft", "published"},
    "clock_templates": {"active", "disabled"},
    "clock_template_versions": {"draft", "published", "superseded"},
    "clock_assignments": {"active", "disabled"},
    "clock_exceptions": {"active", "inactive"},
    "emergency_alerts": {"active", "cancelled"},
    "station_outputs": {"active", "disabled"},
    "recording_jobs": {"active", "disabled"},
    "recording_attempts": {"recording", "completed", "partial", "failed"},
    "asrun_report_runs": {"completed"},
}
REQUIRED = {
    "desktop_media_library": ("station_id", "item_id", "version_id", "media_file_id", "artist", "album", "genre", "tags", "year", "comment", "status", "created_by", "created_at", "updated_by", "updated_at"),
    "desktop_media_analysis": ("station_id", "item_id", "version_id", "media_file_id", "analysis", "analyzer", "created_by", "created_at"),
    "desktop_media_rhythm": ("station_id", "item_id", "version_id", "media_file_id", "media_sha256", "rhythm", "analyzer", "created_by", "created_at", "updated_by", "updated_at"),
    "desktop_media_markers": ("station_id", "item_id", "version_id", "media_file_id", "media_sha256", "media_duration_ms", "status", "cue_in_ms", "intro_ms", "out_ms", "fade_start_ms", "fade_end_ms", "auto_next_ms", "overlap_ms", "automatic", "created_by", "created_at", "updated_by", "updated_at"),
    "desktop_configurations": ("station_id", "kind", "name", "config", "status", "created_by", "created_at", "updated_by", "updated_at"),
    "desktop_mutations": ("station_id", "user_id", "idempotency_key", "operation", "request_hash", "result", "created_at"),
    "desktop_playlist_entries": ("station_id", "playlist_version_id", "ordinal", "status", "title", "artist", "album", "bpm", "genre", "key", "location", "comment", "media_state", "created_by", "created_at", "updated_by", "updated_at"),
    "users": ("username", "display_name", "status"),
    "stations": ("code", "name", "timezone", "status"),
    "broadcast_services": ("station_id", "service_key", "name", "endpoint_id",
                           "output_id", "status"),
    "endpoints": ("station_id", "code", "name", "status"),
    "items": ("kind", "title", "lifecycle"),
    "station_items": ("station_id", "item_id", "eligible"),
    "versions": ("item_id", "version_no", "label", "status"),
    "media_files": ("version_id", "status"),
    "media_locations": ("media_file_id", "storage_uri", "status", "sha256", "size_bytes"),
    "endpoint_copies": ("endpoint_id", "media_file_id", "status"),
    "people": ("display_name", "kind"),
    "person_memberships": ("person_id", "group_id"),
    "contributions": ("item_id", "person_id", "role"),
    "categories": ("station_id", "code", "name"),
    "plans": ("station_id", "service_date", "service_key", "state"),
    "plan_revisions": ("plan_id", "revision_no", "status"),
    "playback_marker_sets": ("marker_contract_version", "station_id", "version_id",
                             "media_file_id", "media_sha256", "media_duration_ms",
                             "cue_in_ms", "cue_out_ms", "segue_in_ms", "segue_out_ms",
                             "fade_in_ms", "fade_out_ms", "cues", "selection_source",
                             "status", "content_sha256", "created_by", "created_at"),
    "retention_policies": ("code", "keep_days", "created_by", "updated_at"),
    "retention_segments": ("source_store_id", "source_sequence", "created_at",
                           "files", "counts", "first_audit_sequence", "last_audit_sequence",
                           "policy_sha256", "records_sha256"),
    "plan_entries": ("plan_revision_id", "ordinal", "scheduled_at", "event_kind", "state"),
    "carts": ("station_id", "code", "title", "selection_mode", "status"),
    "cart_cuts": ("cart_id", "version_id", "ordinal", "weight", "status", "is_fallback"),
    "cart_states": ("cart_id",),
    "rotation_rules": ("station_id", "name", "status", "config"),
    "rotation_decisions": ("station_id", "rule_id", "evaluated_at", "decisions",
                           "candidate_version_ids", "created_by"),
    "plan_distributions": ("station_id", "endpoint_id", "revision_id", "status",
                           "snapshot", "payload_hash"),
    "active_acceptances": ("station_id", "endpoint_id", "revision_id",
                           "engine_instance", "engine_epoch", "accepted_at"),
    "engine_commands": ("station_id", "endpoint_id", "plan_revision_id", "command_key",
                        "state", "expires_at", "engine_epoch", "payload"),
    "advertisers": ("name",),
    "campaigns": ("advertiser_id", "name", "starts_at", "ends_at", "status"),
    "spots": ("campaign_id", "version_id", "status"),
    "obligations": ("campaign_id", "station_id", "required_plays", "window_start", "window_end"),
    "placements": ("obligation_id", "spot_id", "plan_entry_id", "state"),
    "makegood_requests": ("request_id", "station_id", "campaign_id", "obligation_id", "spot_id",
                          "version_id", "media_file_id", "media_sha256", "duration_ms",
                          "plan_id", "plan_revision_id", "break_entry_id", "output_id",
                          "scheduled_at", "status", "idempotency_key", "preview_hash",
                          "recommendation_hash", "required_plays",
                          "future_reserved_at_proposal",
                          "content_verified_plays_at_proposal",
                          "proposed_by", "proposed_at"),
    "playout_attempts": ("station_id", "endpoint_id", "attempt_key", "version_id"),
    "playout_events": ("attempt_id", "event_key", "phase", "occurred_at"),
    "output_observations": ("station_id", "endpoint_id", "occurred_at", "kind",
                            "observation_key"),
    "external_ids": ("source_namespace", "source_id", "target_collection", "target_id"),
    "import_jobs": ("station_id", "source_namespace", "source_sha256",
                    "idempotency_key", "status", "scope", "summary"),
    "import_profiles": ("station_id", "name", "library_key", "status", "config",
                        "config_version"),
    "view_profiles": ("station_id", "owner_user_id", "name", "view_kind",
                      "columns", "filters", "config_version"),
    "schedule_series": ("station_id", "service_key", "title", "status", "starts_on", "created_by"),
    "schedule_segments": ("series_id", "starts_on", "rule", "local_time",
                          "entry_template", "dst_fold", "status"),
    "schedule_exceptions": ("series_id", "service_date", "action", "status"),
    "schedule_occurrences": ("series_id", "service_date", "status",
                             "source_fingerprint"),
    "radio_cues": ("station_id", "version_id", "cue_kind", "unit",
                   "position_value", "position_ms", "source_kind", "status"),
    "plan_transitions": ("station_id", "plan_revision_id", "from_entry_id",
                         "to_entry_id", "context_fingerprint", "status", "manual_lock"),
    "production_placements": ("station_id", "transition_id", "plan_revision_id",
                              "anchor_entry_id", "content_version_id", "kind",
                              "start_reference", "offset_ms", "context_fingerprint",
                              "status", "manual_lock"),
    "controller_profiles": ("station_id", "code", "name", "status"),
    "controller_profile_versions": ("profile_id", "version_no", "status", "device_spec"),
    "controller_banks": ("profile_version_id", "bank_no", "name"),
    "controller_pads": ("bank_id", "pad_no", "label", "midi_event", "channel",
                        "number", "target_kind", "target_id", "trigger_mode"),
    "dj_analysis": ("version_id", "source_namespace", "source_track_id",
                    "marks", "tempos", "status"),
    "import_playlist_nodes": ("station_id", "source_namespace", "source_sha256",
                              "path", "kind", "name"),
    "import_playlist_entries": ("node_id", "ordinal", "source_key", "source_track_id"),
    "field_definitions": ("key", "scope", "data_type", "metadata_class"),
    "metadata_values": ("field_id", "target_collection", "target_id",
                        "station_scope", "is_cleared", "source_kind"),
    "metadata_suggestions": ("field_id", "target_collection", "target_id",
                             "proposed_value", "source_kind", "status"),
    "rights_grants": ("version_id", "station_id", "territory", "output_id",
                      "starts_at", "ends_at", "decision"),
    "permissions": ("user_id", "permission"),
    "tokens": ("user_id", "token_hash", "expires_at", "scope"),
    "bulk_jobs": ("station_id", "idempotency_key", "status", "changes"),
    "diagnostic_events": ("station_id", "incident_id", "event_key", "correlation_key",
                          "component", "condition", "severity", "impact", "code",
                          "message", "occurred_at", "recorded_at", "reporter_id",
                          "fingerprint", "details"),
    "incidents": ("station_id", "correlation_key", "component", "state", "severity",
                  "impact", "first_event_id", "last_event_id", "first_seen_at",
                  "last_seen_at", "last_condition", "occurrence_count"),
    "listener_requests": ("station_id", "source_namespace", "external_request_key",
                          "item_id", "requested_at", "status", "created_by"),
    "listener_mounts": ("station_id", "endpoint_id", "output_id", "station_output_id",
                        "mount_key", "mount_path", "source_namespace",
                        "stale_after_seconds", "status", "created_by", "created_at"),
    "listener_samples": ("mount_id", "station_id", "endpoint_id", "output_id",
                         "source_namespace", "source_sample_key", "observed_at",
                         "source_state", "recorded_at", "reported_by", "claim_fingerprint"),
    "podcast_feeds": ("station_id", "feed_url", "status", "created_at", "created_by"),
    "podcast_episodes": ("station_id", "feed_id", "source_key", "title",
                         "enclosure_url", "published_at", "content_hash",
                         "first_seen_at", "last_seen_at"),
    "playlists": ("station_id", "code", "name", "status", "created_by", "created_at"),
    "playlist_versions": ("playlist_id", "version_no", "status", "created_by", "created_at"),
    "playlist_entries": ("playlist_version_id", "ordinal", "source_kind",
                         "entry_template", "status", "created_by", "created_at"),
    "smart_blocks": ("station_id", "code", "name", "status", "created_by", "created_at"),
    "smart_block_versions": ("block_id", "version_no", "status", "rule_tree",
                             "target_duration_ms", "max_overshoot_ms", "max_items",
                             "selection_order", "selection_mode", "created_by", "created_at"),
    "libretime_ids": ("station_id", "source_kind", "source_key", "external_id", "created_at"),
    "spot_changes": ("station_id", "campaign_id", "old_spot_id", "new_spot_id",
                     "old_version_id", "new_version_id", "effective_at",
                     "idempotency_key", "created_at", "created_by", "status",
                     "impact_count"),
    "spot_change_targets": ("change_id", "station_id", "target_kind", "target_key",
                            "placement_id", "plan_id", "plan_revision_id",
                            "plan_entry_id", "scheduled_at", "old_version_id",
                            "new_version_id", "source_plan_revision",
                            "source_revision_status", "source_placement_revision",
                            "source_entry_revision", "source_consistency",
                            "status", "created_at", "updated_at"),
    "clock_templates": ("station_id", "code", "name", "status", "created_by",
                        "created_at"),
    "clock_template_versions": ("template_id", "version_no", "status",
                                "created_by", "created_at"),
    "clock_slots": ("template_version_id", "ordinal", "offset_ms", "slot_kind",
                    "entry_template"),
    "clock_assignments": ("station_id", "service_key", "template_version_id", "weekday",
                          "local_hour", "starts_on", "dst_fold", "status"),
    "clock_exceptions": ("assignment_id", "service_date", "action", "status",
                         "changed_by", "changed_at"),
    "clock_materializations": ("assignment_id", "station_id", "service_date",
                               "local_hour", "template_version_id", "source_fingerprint",
                               "plan_id", "plan_revision_id", "entry_ids",
                               "entry_fingerprints", "created_by", "created_at"),
    "emergency_alerts": ("station_id", "source_namespace", "external_alert_key",
                         "purpose", "starts_at", "ends_at", "geo_codes", "output_ids",
                         "repeat_interval_seconds", "status", "received_at", "created_by"),
    "emergency_alert_attempts": ("station_id", "endpoint_id", "alert_id",
                                 "attempt_key", "output_id", "geo_code", "purpose",
                                 "state", "started_at", "reported_by",
                                 "representation_kind", "station_output_id",
                                 "station_output_revision"),
    "emergency_alert_observation_links": ("station_id", "endpoint_id", "alert_id",
                                          "attempt_id", "observation_id",
                                          "observation_kind", "observer_kind",
                                          "linked_at", "linked_by"),
    "station_outputs": ("station_id", "endpoint_id", "output_id", "geo_codes",
                        "status", "created_by"),
    "recording_jobs": ("station_id", "endpoint_id", "code", "name", "status",
                       "source", "destination_version_id", "schedule", "start_trigger",
                       "end_trigger", "allow_multiple", "one_shot", "created_by",
                       "created_at"),
    "recording_attempts": ("job_id", "station_id", "endpoint_id", "occurrence_date",
                           "status", "start_event_key", "actual_start_at", "deadline_at",
                           "created_at", "claimed_by"),
    "recording_events": ("attempt_id", "endpoint_id", "event_key", "event_kind",
                         "occurred_at", "claim_fingerprint", "claimed_by"),
    "now_playing_states": ("station_id", "endpoint_id", "output_id", "updated_at"),
    "now_playing_decisions": ("station_id", "endpoint_id", "output_id",
                              "decision_key", "source_kind", "source_id",
                              "event_at", "received_at", "input_fingerprint",
                              "outcome", "reason", "state_revision_before",
                              "state_revision_after", "details"),
    "asrun_report_runs": ("station_id", "idempotency_key", "date_from", "date_to",
                          "as_of_at", "status", "source_sequence", "source_hash",
                          "snapshot_hash", "snapshot", "created_by", "created_at"),
    "live_overrides": ("station_id", "endpoint_id", "plan_id", "plan_revision_id",
                       "plan_entry_id", "operation", "command_key", "request_hash",
                       "snapshot_hash", "engine_instance", "engine_epoch", "state",
                       "issued_at", "expires_at", "created_by"),
    "live_override_reports": ("override_id", "endpoint_id", "event_key",
                              "report_kind", "occurred_at", "received_at",
                              "claim_fingerprint", "engine_instance", "engine_epoch"),
}
REFERENCES = {
    "desktop_media_library": (("station_id", "stations"), ("item_id", "items"), ("version_id", "versions"), ("media_file_id", "media_files"), ("created_by", "users"), ("updated_by", "users")),
    "desktop_media_analysis": (("station_id", "stations"), ("item_id", "items"), ("version_id", "versions"), ("media_file_id", "media_files"), ("created_by", "users")),
    "desktop_media_rhythm": (("station_id", "stations"), ("item_id", "items"), ("version_id", "versions"), ("media_file_id", "media_files"), ("created_by", "users"), ("updated_by", "users")),
    "desktop_media_markers": (("station_id", "stations"), ("item_id", "items"), ("version_id", "versions"), ("media_file_id", "media_files"), ("created_by", "users"), ("updated_by", "users")),
    "desktop_configurations": (("station_id", "stations"), ("created_by", "users"), ("updated_by", "users")),
    "desktop_mutations": (("station_id", "stations"), ("user_id", "users")),
    "desktop_playlist_entries": (("station_id", "stations"), ("playlist_version_id", "playlist_versions"), ("created_by", "users"), ("updated_by", "users")),
    "permissions": (("user_id", "users"),),
    "tokens": (("user_id", "users"),),
    "bulk_jobs": (("station_id", "stations"),),
    "endpoints": (("station_id", "stations"),),
    "broadcast_services": (("station_id", "stations"), ("endpoint_id", "endpoints")),
    "station_items": (("station_id", "stations"), ("item_id", "items")),
    "versions": (("item_id", "items"),),
    "media_files": (("version_id", "versions"),),
    "media_locations": (("media_file_id", "media_files"),),
    "endpoint_copies": (("endpoint_id", "endpoints"), ("media_file_id", "media_files")),
    "person_memberships": (("person_id", "people"), ("group_id", "people")),
    "contributions": (("item_id", "items"), ("person_id", "people")),
    "categories": (("station_id", "stations"),),
    "plans": (("station_id", "stations"),),
    "plan_revisions": (("plan_id", "plans"),),
    "playback_marker_sets": (("station_id", "stations"), ("version_id", "versions"),
                             ("media_file_id", "media_files"), ("created_by", "users")),
    "retention_policies": (("created_by", "users"),),
    "plan_entries": (("plan_revision_id", "plan_revisions"),),
    "carts": (("station_id", "stations"),),
    "cart_cuts": (("cart_id", "carts"), ("version_id", "versions")),
    "cart_states": (("cart_id", "carts"),),
    "rotation_rules": (("station_id", "stations"),),
    "rotation_decisions": (("station_id", "stations"), ("rule_id", "rotation_rules"),
                           ("created_by", "users")),
    "plan_distributions": (("station_id", "stations"), ("endpoint_id", "endpoints"),
                           ("revision_id", "plan_revisions")),
    "active_acceptances": (("station_id", "stations"), ("endpoint_id", "endpoints"),
                           ("revision_id", "plan_revisions")),
    "engine_commands": (("station_id", "stations"), ("endpoint_id", "endpoints"),
                        ("plan_revision_id", "plan_revisions")),
    "campaigns": (("advertiser_id", "advertisers"),),
    "spots": (("campaign_id", "campaigns"), ("version_id", "versions")),
    "obligations": (("campaign_id", "campaigns"), ("station_id", "stations")),
    "placements": (("obligation_id", "obligations"), ("spot_id", "spots"),
                   ("plan_entry_id", "plan_entries")),
    "makegood_requests": (("station_id", "stations"), ("campaign_id", "campaigns"),
                          ("obligation_id", "obligations"), ("spot_id", "spots"),
                          ("version_id", "versions"), ("media_file_id", "media_files"),
                          ("plan_id", "plans"), ("plan_revision_id", "plan_revisions"),
                          ("break_entry_id", "plan_entries"), ("proposed_by", "users")),
    "playout_attempts": (("station_id", "stations"), ("endpoint_id", "endpoints"),
                         ("version_id", "versions")),
    "playout_events": (("attempt_id", "playout_attempts"),),
    "output_observations": (("endpoint_id", "endpoints"),),
    "metadata_values": (("field_id", "field_definitions"),),
    "import_jobs": (("station_id", "stations"),),
    "import_profiles": (("station_id", "stations"),),
    "view_profiles": (("station_id", "stations"), ("owner_user_id", "users")),
    "schedule_series": (("station_id", "stations"), ("created_by", "users")),
    "schedule_segments": (("series_id", "schedule_series"),),
    "schedule_exceptions": (("series_id", "schedule_series"),),
    "schedule_occurrences": (("series_id", "schedule_series"),),
    "radio_cues": (("station_id", "stations"), ("version_id", "versions")),
    "plan_transitions": (("station_id", "stations"),
                         ("plan_revision_id", "plan_revisions"),
                         ("from_entry_id", "plan_entries"),
                         ("to_entry_id", "plan_entries")),
    "production_placements": (("station_id", "stations"),
                              ("transition_id", "plan_transitions"),
                              ("plan_revision_id", "plan_revisions"),
                              ("anchor_entry_id", "plan_entries"),
                              ("content_version_id", "versions")),
    "controller_profiles": (("station_id", "stations"),),
    "controller_profile_versions": (("profile_id", "controller_profiles"),),
    "controller_banks": (("profile_version_id", "controller_profile_versions"),),
    "controller_pads": (("bank_id", "controller_banks"),),
    "dj_analysis": (("version_id", "versions"),),
    "import_playlist_nodes": (("station_id", "stations"),),
    "import_playlist_entries": (("node_id", "import_playlist_nodes"),),
    "metadata_suggestions": (("field_id", "field_definitions"),),
    "rights_grants": (("version_id", "versions"), ("station_id", "stations")),
    "diagnostic_events": (("station_id", "stations"), ("incident_id", "incidents"),
                          ("reporter_id", "users")),
    "incidents": (("station_id", "stations"),),
    "listener_requests": (("station_id", "stations"), ("item_id", "items"),
                          ("created_by", "users")),
    "listener_mounts": (("station_id", "stations"), ("endpoint_id", "endpoints"),
                        ("station_output_id", "station_outputs"), ("created_by", "users")),
    "listener_samples": (("mount_id", "listener_mounts"), ("station_id", "stations"),
                         ("endpoint_id", "endpoints"), ("reported_by", "users")),
    "podcast_feeds": (("station_id", "stations"), ("created_by", "users")),
    "podcast_episodes": (("station_id", "stations"), ("feed_id", "podcast_feeds")),
    "playlists": (("station_id", "stations"), ("created_by", "users")),
    "playlist_versions": (("playlist_id", "playlists"), ("created_by", "users")),
    "playlist_entries": (("playlist_version_id", "playlist_versions"),
                         ("created_by", "users")),
    "smart_blocks": (("station_id", "stations"), ("created_by", "users")),
    "smart_block_versions": (("block_id", "smart_blocks"), ("created_by", "users")),
    "libretime_ids": (("station_id", "stations"),),
    "spot_changes": (("station_id", "stations"), ("campaign_id", "campaigns"),
                     ("old_spot_id", "spots"), ("new_spot_id", "spots"),
                     ("old_version_id", "versions"), ("new_version_id", "versions"),
                     ("created_by", "users")),
    "spot_change_targets": (("change_id", "spot_changes"),
                            ("station_id", "stations"), ("placement_id", "placements"),
                            ("plan_id", "plans"),
                            ("plan_revision_id", "plan_revisions"),
                            ("plan_entry_id", "plan_entries"),
                            ("old_version_id", "versions"),
                            ("new_version_id", "versions")),
    "clock_templates": (("station_id", "stations"), ("created_by", "users")),
    "clock_template_versions": (("template_id", "clock_templates"),
                                ("created_by", "users")),
    "clock_slots": (("template_version_id", "clock_template_versions"),),
    "clock_assignments": (("station_id", "stations"),
                          ("template_version_id", "clock_template_versions")),
    "clock_exceptions": (("assignment_id", "clock_assignments"),
                         ("changed_by", "users")),
    "clock_materializations": (("assignment_id", "clock_assignments"),
                               ("station_id", "stations"),
                               ("template_version_id", "clock_template_versions"),
                               ("plan_id", "plans"),
                               ("plan_revision_id", "plan_revisions"),
                               ("created_by", "users")),
    "emergency_alerts": (("station_id", "stations"), ("created_by", "users")),
    "emergency_alert_attempts": (("station_id", "stations"),
                                 ("endpoint_id", "endpoints"),
                                 ("alert_id", "emergency_alerts"),
                                 ("reported_by", "users"),
                                 ("station_output_id", "station_outputs")),
    "emergency_alert_observation_links": (("station_id", "stations"),
                                          ("endpoint_id", "endpoints"),
                                          ("alert_id", "emergency_alerts"),
                                          ("attempt_id", "emergency_alert_attempts"),
                                          ("observation_id", "output_observations"),
                                          ("linked_by", "users")),
    "station_outputs": (("station_id", "stations"), ("endpoint_id", "endpoints"),
                        ("created_by", "users")),
    "recording_jobs": (("station_id", "stations"), ("endpoint_id", "endpoints"),
                       ("destination_version_id", "versions"),
                       ("created_by", "users")),
    "recording_attempts": (("job_id", "recording_jobs"), ("station_id", "stations"),
                           ("endpoint_id", "endpoints"), ("claimed_by", "users")),
    "recording_events": (("attempt_id", "recording_attempts"),
                         ("endpoint_id", "endpoints"), ("claimed_by", "users")),
    "now_playing_states": (("station_id", "stations"), ("endpoint_id", "endpoints")),
    "now_playing_decisions": (("station_id", "stations"), ("endpoint_id", "endpoints")),
    "asrun_report_runs": (("station_id", "stations"), ("created_by", "users")),
    "live_overrides": (("station_id", "stations"), ("endpoint_id", "endpoints"),
                       ("plan_id", "plans"), ("plan_revision_id", "plan_revisions"),
                       ("plan_entry_id", "plan_entries"), ("created_by", "users")),
    "live_override_reports": (("override_id", "live_overrides"),
                              ("endpoint_id", "endpoints")),
}
INDEXES = {
    "desktop_media_library": {"station": IndexSpec(("station_id",)), "station_media": IndexSpec(("station_id", "media_file_id"), True)},
    "desktop_media_analysis": {"media": IndexSpec(("media_file_id",), True), "station": IndexSpec(("station_id",))},
    "desktop_media_rhythm": {"station_media": IndexSpec(("station_id", "media_file_id"), True), "station": IndexSpec(("station_id",))},
    "desktop_media_markers": {"station_media": IndexSpec(("station_id", "media_file_id"), True), "station": IndexSpec(("station_id",))},
    "desktop_configurations": {"station_kind": IndexSpec(("station_id", "kind")), "station": IndexSpec(("station_id",))},
    "desktop_mutations": {"request": IndexSpec(("station_id", "user_id", "idempotency_key"), True)},
    "desktop_playlist_entries": {"version_ordinal": IndexSpec(("playlist_version_id", "active_ordinal"), True), "version": IndexSpec(("playlist_version_id",)), "station": IndexSpec(("station_id",))},
    "users": {"username": IndexSpec(("username",), True)},
    "stations": {"code": IndexSpec(("code",), True)},
    "broadcast_services": {
        "station_service": IndexSpec(("station_id", "service_key"), True),
        "endpoint": IndexSpec(("endpoint_id",), True),
        "station": IndexSpec(("station_id",)),
    },
    "endpoints": {"station_code": IndexSpec(("station_id", "code"), True),
                  "station": IndexSpec(("station_id",))},
    "items": {"kind": IndexSpec(("kind",)), "lifecycle": IndexSpec(("lifecycle",))},
    "station_items": {"station_item": IndexSpec(("station_id", "item_id"), True),
                      "station": IndexSpec(("station_id",)),
                      "item": IndexSpec(("item_id",))},
    "versions": {"item_number": IndexSpec(("item_id", "version_no"), True),
                 "item": IndexSpec(("item_id",))},
    "media_files": {"version": IndexSpec(("version_id",)),
                    "digest": IndexSpec(("sha256",))},
    "media_locations": {"file": IndexSpec(("media_file_id",)),
                        "file_uri": IndexSpec(("media_file_id", "storage_uri"), True)},
    "endpoint_copies": {"endpoint_file": IndexSpec(("endpoint_id", "media_file_id"), True),
                        "endpoint": IndexSpec(("endpoint_id",))},
    "categories": {"station_code": IndexSpec(("station_id", "code"), True)},
    "contributions": {"item": IndexSpec(("item_id",)),
                      "person": IndexSpec(("person_id",))},
    "person_memberships": {"person": IndexSpec(("person_id",))},
    "plans": {"station_date_service": IndexSpec(("station_id", "service_date",
                                                  "service_key"), True),
              "station_date": IndexSpec(("station_id", "service_date"))},
    "plan_revisions": {"plan_number": IndexSpec(("plan_id", "revision_no"), True),
                       "plan": IndexSpec(("plan_id",))},
    "playback_marker_sets": {"station": IndexSpec(("station_id",)),
                             "version": IndexSpec(("version_id",)),
                             "media": IndexSpec(("media_file_id",)),
                             "content": IndexSpec(("station_id", "content_sha256"))},
    "retention_policies": {"code": IndexSpec(("code",), True)},
    "retention_segments": {"created": IndexSpec(("created_at",))},
    "plan_entries": {"revision_ordinal": IndexSpec(("plan_revision_id", "ordinal"), True),
                     "revision": IndexSpec(("plan_revision_id",))},
    "carts": {"station_code": IndexSpec(("station_id", "code"), True)},
    "cart_cuts": {"cart_ordinal": IndexSpec(("cart_id", "ordinal"), True),
                  "cart": IndexSpec(("cart_id",))},
    "rotation_rules": {"station_name": IndexSpec(("station_id", "name"), True)},
    "rotation_decisions": {"station_time": IndexSpec(("station_id", "evaluated_at")),
                           "rule": IndexSpec(("rule_id",))},
    "plan_distributions": {"endpoint_revision": IndexSpec(("endpoint_id", "revision_id"), True),
                           "endpoint": IndexSpec(("endpoint_id",))},
    "engine_commands": {"endpoint_key": IndexSpec(("endpoint_id", "command_key"), True),
                        "endpoint": IndexSpec(("endpoint_id",))},
    "spots": {"campaign_version": IndexSpec(("campaign_id", "version_id"), True),
              "version": IndexSpec(("version_id",))},
    "obligations": {"station": IndexSpec(("station_id",)),
                    "campaign": IndexSpec(("campaign_id",))},
    "placements": {"plan_entry": IndexSpec(("plan_entry_id",), True),
                   "obligation": IndexSpec(("obligation_id",)),
                   "makegood_request": IndexSpec(("makegood_request_id",), True)},
    "makegood_requests": {"station_key": IndexSpec(("station_id", "idempotency_key"), True),
                          "obligation_break": IndexSpec(("obligation_id", "break_entry_id"), True),
                          "campaign": IndexSpec(("campaign_id",))},
    "playout_attempts": {"endpoint_key": IndexSpec(("endpoint_id", "attempt_key"), True),
                         "station": IndexSpec(("station_id",))},
    "playout_events": {"event_key": IndexSpec(("event_key",), True),
                       "attempt": IndexSpec(("attempt_id",))},
    "output_observations": {"observation_key": IndexSpec(("observation_key",), True),
                            "station": IndexSpec(("station_id",))},
    "diagnostic_events": {"station_event": IndexSpec(("station_id", "event_key"), True),
                          "incident": IndexSpec(("incident_id",))},
    "incidents": {"station_correlation": IndexSpec(("station_id", "correlation_key"), True),
                  "station": IndexSpec(("station_id",))},
    "listener_requests": {"station_source_key": IndexSpec(("station_id", "source_namespace",
                                                            "external_request_key"), True),
                          "station": IndexSpec(("station_id",))},
    "listener_mounts": {
        "scope_key": IndexSpec(("station_id", "endpoint_id", "output_id", "mount_key"), True),
        "station_source_path": IndexSpec(("station_id", "source_namespace", "mount_path"), True),
        "station": IndexSpec(("station_id",)), "endpoint": IndexSpec(("endpoint_id",))},
    "listener_samples": {"mount_key": IndexSpec(("mount_id", "source_sample_key"), True),
                         "mount": IndexSpec(("mount_id",)),
                         "station": IndexSpec(("station_id",))},
    "podcast_feeds": {"station_url": IndexSpec(("station_id", "feed_url"), True),
                      "station": IndexSpec(("station_id",))},
    "podcast_episodes": {"feed_key": IndexSpec(("feed_id", "source_key"), True),
                         "feed_guid": IndexSpec(("feed_id", "guid"), True),
                         "feed_enclosure": IndexSpec(("feed_id", "enclosure_url"), True),
                         "feed": IndexSpec(("feed_id",)),
                         "feed_published": IndexSpec(("feed_id", "published_at")),
                         "station": IndexSpec(("station_id",))},
    "playlists": {"station_code": IndexSpec(("station_id", "code"), True),
                  "station": IndexSpec(("station_id",))},
    "playlist_versions": {"playlist_number": IndexSpec(("playlist_id", "version_no"), True),
                          "playlist": IndexSpec(("playlist_id",))},
    "playlist_entries": {"version_ordinal": IndexSpec(("playlist_version_id", "active_ordinal"), True),
                         "version": IndexSpec(("playlist_version_id",))},
    "smart_blocks": {"station_code": IndexSpec(("station_id", "code"), True),
                     "station": IndexSpec(("station_id",))},
    "smart_block_versions": {"block_number": IndexSpec(("block_id", "version_no"), True),
                             "block": IndexSpec(("block_id",))},
    "libretime_ids": {"source": IndexSpec(("station_id", "source_kind", "source_key"), True),
                      "external": IndexSpec(("station_id", "external_id"), True),
                      "station": IndexSpec(("station_id",))},
    "spot_changes": {"station_key": IndexSpec(("station_id", "idempotency_key"), True),
                     "old_spot": IndexSpec(("old_spot_id",))},
    "spot_change_targets": {"change_target": IndexSpec(("change_id", "target_key"), True),
                            "change": IndexSpec(("change_id",)),
                            "placement": IndexSpec(("placement_id",))},
    "clock_templates": {"station_code": IndexSpec(("station_id", "code"), True)},
    "clock_template_versions": {"template_number": IndexSpec(("template_id", "version_no"), True),
                                "template": IndexSpec(("template_id",))},
    "clock_slots": {"version_ordinal": IndexSpec(("template_version_id", "ordinal"), True),
                    "version": IndexSpec(("template_version_id",)),
                    "version_offset": IndexSpec(("template_version_id", "offset_ms"))},
    "clock_assignments": {"station_weekday_hour": IndexSpec(("station_id", "service_key", "weekday",
                                                               "local_hour")),
                          "station": IndexSpec(("station_id",))},
    "clock_exceptions": {"assignment_date": IndexSpec(("assignment_id", "service_date"), True)},
    "clock_materializations": {"assignment_date": IndexSpec(("assignment_id", "service_date"), True)},
    "emergency_alerts": {"source_key": IndexSpec(("station_id", "source_namespace",
                                                 "external_alert_key"), True),
                         "station": IndexSpec(("station_id",))},
    "emergency_alert_attempts": {"endpoint_key": IndexSpec(("endpoint_id", "attempt_key"), True),
                                 "alert": IndexSpec(("alert_id",)),
                                 "endpoint": IndexSpec(("endpoint_id",))},
    "emergency_alert_observation_links": {"observation": IndexSpec(("observation_id",), True),
                                          "attempt": IndexSpec(("attempt_id",))},
    "station_outputs": {"station_endpoint_output": IndexSpec(("station_id", "endpoint_id",
                                                                 "output_id"), True),
                        "station": IndexSpec(("station_id",)),
                        "endpoint": IndexSpec(("endpoint_id",))},
    "recording_jobs": {"station_code": IndexSpec(("station_id", "code"), True),
                       "endpoint": IndexSpec(("endpoint_id",))},
    "recording_attempts": {"job_date": IndexSpec(("job_id", "occurrence_date")),
                           "job": IndexSpec(("job_id",)),
                           "endpoint": IndexSpec(("endpoint_id",))},
    "recording_events": {"endpoint_key": IndexSpec(("endpoint_id", "event_key"), True),
                         "attempt": IndexSpec(("attempt_id",))},
    "now_playing_states": {"scope": IndexSpec(("station_id", "endpoint_id",
                                               "output_id"), True)},
    "now_playing_decisions": {"scope_key": IndexSpec(("station_id", "endpoint_id",
                                                      "output_id", "decision_key"), True),
                              "scope": IndexSpec(("station_id", "endpoint_id", "output_id"))},
    "asrun_report_runs": {"station_key": IndexSpec(("station_id", "idempotency_key"), True),
                          "station": IndexSpec(("station_id",))},
    "live_overrides": {"endpoint_key": IndexSpec(("endpoint_id", "command_key"), True),
                       "endpoint_state": IndexSpec(("endpoint_id", "state")),
                       "station": IndexSpec(("station_id",))},
    "live_override_reports": {"endpoint_key": IndexSpec(("endpoint_id", "event_key"), True),
                              "override": IndexSpec(("override_id",))},
    "external_ids": {"source": IndexSpec(("source_namespace", "source_id"), True)},
    "import_jobs": {"source_key": IndexSpec(("source_namespace", "idempotency_key"), True)},
    "import_profiles": {"station_name": IndexSpec(("station_id", "name"), True),
                        "station": IndexSpec(("station_id",))},
    "view_profiles": {"owner_station_name": IndexSpec(("owner_user_id", "station_id", "name"), True),
                      "owner_station": IndexSpec(("owner_user_id", "station_id"))},
    "schedule_series": {"station": IndexSpec(("station_id",)),
                        "station_service": IndexSpec(("station_id", "service_key"))},
    "schedule_segments": {"series": IndexSpec(("series_id",))},
    "schedule_exceptions": {"series_date": IndexSpec(("series_id", "service_date"), True)},
    "schedule_occurrences": {"series_date": IndexSpec(("series_id", "service_date"), True),
                             "series": IndexSpec(("series_id",))},
    "radio_cues": {"version": IndexSpec(("version_id",))},
    "plan_transitions": {"revision_pair": IndexSpec(("plan_revision_id", "from_entry_id", "to_entry_id"), True),
                         "revision": IndexSpec(("plan_revision_id",))},
    "production_placements": {"transition": IndexSpec(("transition_id",)),
                              "revision": IndexSpec(("plan_revision_id",))},
    "controller_profiles": {"station_code": IndexSpec(("station_id", "code"), True)},
    "controller_profile_versions": {"profile_number": IndexSpec(("profile_id", "version_no"), True),
                                    "profile": IndexSpec(("profile_id",))},
    "controller_banks": {"version_number": IndexSpec(("profile_version_id", "bank_no"), True),
                         "profile_version": IndexSpec(("profile_version_id",))},
    "controller_pads": {"bank_number": IndexSpec(("bank_id", "pad_no"), True),
                        "bank_midi": IndexSpec(("bank_id", "midi_event", "channel", "number"), True),
                        "bank": IndexSpec(("bank_id",))},
    "dj_analysis": {"source_version": IndexSpec(("source_namespace", "source_track_id", "version_id"), True)},
    "import_playlist_nodes": {"snapshot_path": IndexSpec(("station_id", "source_namespace", "source_sha256", "path"), True)},
    "import_playlist_entries": {"node_ordinal": IndexSpec(("node_id", "ordinal"), True)},
    "field_definitions": {"key": IndexSpec(("key",), True)},
    "metadata_values": {"field_target": IndexSpec(("field_id", "target_collection",
                                                       "target_id", "station_scope"), True),
                        "target": IndexSpec(("target_collection", "target_id"))},
    "metadata_suggestions": {"target": IndexSpec(("target_collection", "target_id"))},
    "rights_grants": {"version": IndexSpec(("version_id",))},
    "permissions": {"user_permission_station": IndexSpec(("user_id", "permission", "station_scope"), True),
                    "user": IndexSpec(("user_id",))},
    "tokens": {"hash": IndexSpec(("token_hash",), True)},
    "bulk_jobs": {"station_key": IndexSpec(("station_id", "idempotency_key"), True)},
}


def _instant(value: Any, name: str) -> datetime:
    if not isinstance(value, str):
        raise StoreError(f"{name} must be an ISO timestamp string")
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError as exc:
        raise StoreError(f"{name} is not an ISO timestamp") from exc
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        raise StoreError(f"{name} needs a timezone offset")
    return parsed


def _validate_offline_grant(data: dict, state: Mapping[str, dict]) -> None:
    """Bind a next-day offline authority to one immutable delivery and owner."""
    from .playout import _local_day_start, _sha

    grant = data["offline_grant"]
    fields = {"grant_version", "grant_id", "station_id", "endpoint_id",
              "plan_revision_id", "snapshot_sha256", "base_revision_id",
              "base_engine_epoch", "engine_instance", "engine_epoch",
              "activate_after", "expires_at", "issued_at"}
    if not isinstance(grant, dict) or set(grant) != fields or \
            type(grant["grant_version"]) is not int or grant["grant_version"] != 1:
        raise StoreError("offline grant has an invalid contract")
    for field in ("grant_id", "station_id", "endpoint_id", "plan_revision_id",
                  "base_revision_id"):
        value = grant[field]
        try:
            if not isinstance(value, str) or str(UUID(value)) != value:
                raise ValueError
        except ValueError as exc:
            raise StoreError(f"offline grant {field} must be a canonical UUID") from exc
    if data["status"] not in {"sent", "accepted", "expired"} or \
            grant["station_id"] != data["station_id"] or \
            grant["endpoint_id"] != data["endpoint_id"] or \
            grant["plan_revision_id"] != data["revision_id"] or \
            grant["base_revision_id"] == data["revision_id"]:
        raise StoreError("offline grant differs from its delivery identity")
    snapshot = data["snapshot"]
    if not isinstance(snapshot, dict) or any(
            snapshot.get(field) != expected for field, expected in (
                ("station_id", data["station_id"]),
                ("endpoint_id", data["endpoint_id"]),
                ("plan_revision_id", data["revision_id"]),
                ("plan_id", data.get("plan_id")),
                ("service_date", data.get("service_date")),
                ("activate_after", data.get("activate_after")))):
        raise StoreError("offline grant snapshot differs from its delivery")
    digest = data["payload_hash"]
    if not isinstance(digest, str) or len(digest) != 64 or \
            any(ch not in "0123456789abcdef" for ch in digest) or \
            digest != grant["snapshot_sha256"] or digest != _sha(snapshot):
        raise StoreError("offline grant snapshot fingerprint differs")
    revision = state["plan_revisions"][data["revision_id"]]["data"]
    plan = state["plans"][revision["plan_id"]]["data"]
    if revision["plan_id"] != data.get("plan_id") or \
            plan["station_id"] != data["station_id"] or \
            plan["service_date"] != data.get("service_date"):
        raise StoreError("offline grant delivery differs from its plan")
    try:
        target_day = date.fromisoformat(data["service_date"])
        base_day = target_day - timedelta(days=1)
        end_day = target_day + timedelta(days=1)
    except (TypeError, ValueError, OverflowError) as exc:
        raise StoreError("offline grant service_date is invalid") from exc
    base_rows = [row["data"] for row in state.get("plan_distributions", {}).values()
                 if row["data"].get("endpoint_id") == data["endpoint_id"] and
                 row["data"].get("revision_id") == grant["base_revision_id"] and
                 row["data"].get("status") == "accepted"]
    if len(base_rows) != 1 or base_rows[0].get("station_id") != data["station_id"] or \
            base_rows[0].get("service_date") != base_day.isoformat() or \
            base_rows[0].get("payload_hash") != _sha(base_rows[0]["snapshot"]):
        raise StoreError("offline grant lacks an accepted previous-day delivery")
    if type(grant["base_engine_epoch"]) is not int or \
            grant["base_engine_epoch"] < 1 or \
            type(grant["engine_epoch"]) is not int or \
            grant["engine_epoch"] != grant["base_engine_epoch"] + 1 or \
            not isinstance(grant["engine_instance"], str) or \
            not grant["engine_instance"].strip():
        raise StoreError("offline grant has an invalid engine fence")
    issued = _instant(grant["issued_at"], "offline grant issued_at")
    activate = _instant(grant["activate_after"], "offline grant activate_after")
    expires = _instant(grant["expires_at"], "offline grant expires_at")
    station = state["stations"][data["station_id"]]["data"]
    zone = ZoneInfo(station["timezone"])
    if issued.astimezone(zone).date() != base_day or \
            activate != _instant(data["activate_after"], "activate_after") or \
            activate != _local_day_start(target_day, zone) or \
            expires != _local_day_start(end_day, zone) or \
            not issued < activate < expires:
        raise StoreError("offline grant is outside its next-day service window")
    # A sent delivery is the issuance state. Once the target becomes accepted or
    # expires, its historical grant remains valid without requiring the old
    # engine to remain the current owner.
    if data["status"] == "sent":
        owner_row = state.get("active_acceptances", {}).get(data["endpoint_id"])
        owner = owner_row["data"] if owner_row else None
        if owner is None or owner.get("state", "active") != "active" or \
                owner.get("station_id") != data["station_id"] or \
                owner.get("revision_id") != grant["base_revision_id"] or \
                owner.get("engine_instance") != grant["engine_instance"] or \
                owner.get("engine_epoch") != grant["base_engine_epoch"] or \
                owner.get("payload_hash") != base_rows[0]["payload_hash"] or \
                not owner.get("heartbeat_at"):
            raise StoreError("offline grant does not match the current engine owner")
        heartbeat = _instant(owner["heartbeat_at"], "heartbeat_at")
        if not timedelta(0) <= issued - heartbeat <= timedelta(minutes=5):
            raise StoreError("offline grant needs a fresh owner heartbeat")


def _active_service(state: Mapping[str, dict], station_id: str,
                    service_key: str) -> dict | None:
    for envelope in state.get("broadcast_services", {}).values():
        row = envelope["data"]
        if row["station_id"] == station_id and row["service_key"] == service_key and \
                row["status"] == "active":
            return row
    return None


def _validate_execution_revision(data: dict, state: Mapping[str, dict]) -> None:
    """A new approval fixes destination and exact reviewed audio offsets."""
    from .playback_markers import freeze_marker_set, validate_frozen_markers
    from .snapshot_contract import CONTRACT_VERSION
    binding = data.get("execution_binding")
    required = {"source", "service_key", "endpoint_id", "output_id",
                "station_output_id", "broadcast_service_id"}
    if data.get("execution_contract_version") != CONTRACT_VERSION or \
            not isinstance(binding, dict) or set(binding) != required:
        raise StoreError("approved revision needs a frozen execution contract")
    plan = state["plans"][str(UUID(data["plan_id"]))]["data"]
    if binding["service_key"] != plan["service_key"] or \
            not isinstance(binding["output_id"], str) or binding["output_id"] in {"", "*"}:
        raise StoreError("approved execution output is not concrete")
    try:
        endpoint = state["endpoints"][str(UUID(binding["endpoint_id"]))]["data"]
        output = state["station_outputs"][str(UUID(binding["station_output_id"]))]["data"]
    except (KeyError, TypeError, ValueError, AttributeError) as exc:
        raise StoreError("approved execution binding references absent configuration") from exc
    if endpoint["station_id"] != plan["station_id"] or \
            any(output[field] != expected for field, expected in (
                ("station_id", plan["station_id"]), ("endpoint_id", binding["endpoint_id"]),
                ("output_id", binding["output_id"]))):
        raise StoreError("approved execution binding crosses station or output")
    if binding["source"] == "broadcast_service":
        try:
            service = state["broadcast_services"][str(UUID(binding["broadcast_service_id"]))]["data"]
        except (KeyError, TypeError, ValueError, AttributeError) as exc:
            raise StoreError("approved service is absent") from exc
        if any(service[field] != expected for field, expected in (
                ("station_id", plan["station_id"]), ("service_key", binding["service_key"]),
                ("endpoint_id", binding["endpoint_id"]), ("output_id", binding["output_id"]))):
            raise StoreError("approved service differs from frozen binding")
    elif binding["source"] != "unique_default" or plan["service_key"] != "default" or \
            binding["broadcast_service_id"] is not None:
        raise StoreError("invalid approved default destination")
    playback = data.get("playback_bindings")
    if not isinstance(playback, dict):
        raise StoreError("approved revision needs playback bindings")
    # During a transaction the revision's ID is represented by its entry parent.
    revision_ids = {key for key, row in state.get("plan_revisions", {}).items()
                    if row["data"]["plan_id"] == data["plan_id"] and
                    row["data"]["revision_no"] == data["revision_no"]}
    expected = {key: row["data"] for key, row in state.get("plan_entries", {}).items()
                if row["data"]["plan_revision_id"] in revision_ids and
                row["data"].get("state") == "planned" and
                row["data"]["event_kind"] == "audio"}
    if set(playback) != set(expected):
        raise StoreError("approved playback bindings differ from planned audio entries")
    for key, entry in expected.items():
        markers = playback[key]
        validate_frozen_markers(markers, station_id=plan["station_id"],
                                version_id=entry["payload"]["version_id"],
                                payload=entry["payload"], duration_kind=entry["duration_kind"],
                                planned_duration_ms=entry["planned_duration_ms"])
        row = state.get("playback_marker_sets", {}).get(markers["marker_set_id"])
        if row is None or freeze_marker_set(dict(row["data"]) | {
                "_id": markers["marker_set_id"], "_rev": row["revision"]}) != markers:
            raise StoreError("approved playback binding differs from immutable marker set")


def _validate(collection: str, data: dict, state: Mapping[str, dict]) -> None:
    for name in REQUIRED.get(collection, ()):
        if name not in data or (data[name] is None and not (
                collection == "playback_marker_sets" and name in {"segue_in_ms", "segue_out_ms"})):
            raise StoreError(f"{collection}.{name} is required")
    for name, target in REFERENCES.get(collection, ()):
        try:
            key = str(UUID(str(data[name])))
        except (KeyError, ValueError, TypeError) as exc:
            raise StoreError(f"{collection}.{name} must be a UUID") from exc
        if key not in state.get(target, {}):
            raise StoreError(f"{collection}.{name} references absent {target}/{key}")
    if "status" in data and collection in STATUS and (not isinstance(data["status"], str) or
                                                      data["status"] not in STATUS[collection]):
        raise StoreError(f"invalid {collection} status")
    if collection in {"desktop_media_library", "desktop_media_analysis", "desktop_media_markers", "desktop_media_rhythm"}:
        from .desktop_media import validate_library, validate_analysis, validate_marker_draft, validate_rhythm
        {"desktop_media_library": validate_library, "desktop_media_analysis": validate_analysis,
         "desktop_media_markers": validate_marker_draft, "desktop_media_rhythm": validate_rhythm}[collection](data, state)
    if collection == "contributions":
        from .desktop_media import validate_managed_contribution
        validate_managed_contribution(data, state)
    if collection == "desktop_configurations":
        from .desktop_api import validate_document
        validate_document(data, state)
    if collection == "desktop_playlist_entries":
        from .desktop_playlist import validate_entry
        if "duration_ms" not in data or "active_ordinal" not in data:
            raise StoreError("desktop playlist entry duration and active ordinal fields are required")
        validate_entry(data, state)
    if collection == "desktop_mutations":
        from .desktop_api import validate_mutation
        validate_mutation(data)
    if collection == "campaigns" and "desktop_fields" in data:
        from .desktop_api import _campaign_fields
        _campaign_fields(data["desktop_fields"])
        item = state.get("items", {}).get(data.get("desktop_item_id"), {}).get("data")
        version = state.get("versions", {}).get(data.get("desktop_version_id"), {}).get("data")
        if not item or not version or item["kind"] != "advertisement" or version["item_id"] != data["desktop_item_id"]:
            raise StoreError("campaign native spot draft references invalid advertisement/version")
    if collection == "campaigns" and "desktop_binding" in data:
        from .desktop_advert import validate_campaign_binding
        validate_campaign_binding(data, state)
    if collection == "items":
        if data["kind"] not in KINDS or data["lifecycle"] not in STATUS["items"]:
            raise StoreError("invalid item kind or lifecycle")
        if not isinstance(data["title"], str) or not data["title"].strip():
            raise StoreError("item title must be nonempty")
    if collection == "playback_marker_sets":
        from .playback_markers import validate_marker_set
        validate_marker_set(data, state)
    if collection == "retention_policies":
        from .retention import validate_policy
        validate_policy(data["keep_days"])
        if data["code"] != "operational":
            raise StoreError("unknown retention policy")
        _instant(data["updated_at"], "updated_at")
    if collection == "retention_segments":
        from .retention import validate_segment
        validate_segment(data)
    if collection == "plan_revisions" and data.get("status") == "approved":
        _validate_execution_revision(data, state)
    if collection == "stations":
        # Exact-case key of the bundled tz database: Windows file lookups are
        # case-insensitive and ignore trailing dots/spaces, so "europe/istanbul"
        # would otherwise load here and be persisted for the C side to reject.
        try:
            _platform.canonical_timezone(data["timezone"])
        except (ZoneInfoNotFoundError, TypeError, ValueError, OSError) as exc:
            raise StoreError("unknown IANA station timezone") from exc
    if collection in {"playlists", "smart_blocks"}:
        if any(not isinstance(data[field], str) or not data[field].strip()
               for field in ("code", "name")):
            raise StoreError("playlist or smart block code/name must be nonempty")
        for field, target, parent in (("current_version_id",
                                       "playlist_versions" if collection == "playlists" else
                                       "smart_block_versions",
                                       "playlist_id" if collection == "playlists" else "block_id"),
                                      ("draft_version_id",
                                       "playlist_versions" if collection == "playlists" else
                                       "smart_block_versions",
                                       "playlist_id" if collection == "playlists" else "block_id")):
            if data.get(field) is not None:
                try:
                    version = state[target][str(UUID(str(data[field])))]["data"]
                    owner = state[collection][str(UUID(str(version[parent])))]["data"]
                except (KeyError, TypeError, ValueError, AttributeError) as exc:
                    raise StoreError(f"{collection}.{field} is not an existing version") from exc
                if field == "current_version_id" and version["status"] != "published":
                    raise StoreError("current playlist or smart block version must be published")
                if field == "draft_version_id" and version["status"] != "draft":
                    raise StoreError("draft playlist or smart block version must be a draft")
                if owner["station_id"] != data["station_id"] or owner["code"] != data["code"]:
                    raise StoreError("playlist or smart block pointer belongs to another definition")
        _instant(data["created_at"], "created_at")
    if collection in {"playlist_versions", "smart_block_versions"}:
        if isinstance(data["version_no"], bool) or not isinstance(data["version_no"], int) or \
                data["version_no"] < 1:
            raise StoreError("playlist or smart block version number must be positive")
        if data["status"] == "published":
            if not isinstance(data.get("published_by"), str) or \
                    data["published_by"] not in state.get("users", {}) or \
                    not data.get("published_at"):
                raise StoreError("published version needs reviewer and time")
            _instant(data["published_at"], "published_at")
        _instant(data["created_at"], "created_at")
        if collection == "smart_block_versions":
            if not isinstance(data["rule_tree"], dict):
                raise StoreError("smart block rule tree must be an object")
            from .smart_blocks import validate_rule_tree
            if validate_rule_tree(data["rule_tree"]) != data["rule_tree"]:
                raise StoreError("smart block rule tree must be canonical")
            for field, minimum, maximum in (("target_duration_ms", 1, 24*60*60*1000),
                                            ("max_overshoot_ms", 0, 24*60*60*1000),
                                            ("max_items", 1, 200)):
                value = data[field]
                if isinstance(value, bool) or not isinstance(value, int) or \
                        not minimum <= value <= maximum:
                    raise StoreError(f"invalid smart block {field}")
            if not isinstance(data["selection_order"], str) or \
                    data["selection_order"] not in {"ordered", "shuffle"}:
                raise StoreError("invalid smart block selection order")
            if not isinstance(data["selection_mode"], str) or \
                    data["selection_mode"] not in {"dynamic", "static"}:
                raise StoreError("invalid smart block selection mode")
            static_ids = data.get("static_version_ids")
            if data["status"] == "published" and data["selection_mode"] == "static":
                if not isinstance(static_ids, list) or not static_ids or \
                        len(static_ids) > data["max_items"] or \
                        any(not isinstance(value, str) for value in static_ids) or \
                        len(set(static_ids)) != len(static_ids):
                    raise StoreError("static smart block needs distinct frozen versions")
                station_id = state["smart_blocks"][str(UUID(data["block_id"]))]["data"]["station_id"]
                for version_id in static_ids:
                    try:
                        version = state["versions"][str(UUID(str(version_id)))]["data"]
                    except (KeyError, TypeError, ValueError, AttributeError) as exc:
                        raise StoreError("static smart block version is absent") from exc
                    if not any(row["data"]["station_id"] == station_id and
                               row["data"]["item_id"] == version["item_id"]
                               for row in state.get("station_items", {}).values()):
                        raise StoreError("static smart block version is outside station")
            elif static_ids is not None:
                raise StoreError("only published static smart blocks may freeze versions")
            if data["target_duration_ms"] + data["max_overshoot_ms"] > 24*60*60*1000:
                raise StoreError("smart block duration plus overshoot exceeds one day")
    if collection == "playlist_entries":
        version = state["playlist_versions"][str(UUID(data["playlist_version_id"]))]["data"]
        if version["status"] != "draft":
            raise StoreError("published playlist entries are immutable")
        if isinstance(data["ordinal"], bool) or not isinstance(data["ordinal"], int) or \
                data["ordinal"] < 0:
            raise StoreError("playlist entry ordinal must be nonnegative")
        if "active_ordinal" not in data:
            raise StoreError("playlist entry active_ordinal is required")
        if data["status"] == "active" and (type(data["active_ordinal"]) is not int or
                                            data["active_ordinal"] != data["ordinal"]):
            raise StoreError("active playlist entry ordinal mismatch")
        if data["status"] == "removed" and data["active_ordinal"] is not None:
            raise StoreError("removed playlist entry retains an active ordinal")
        if data["status"] == "removed":
            if data.get("removed_by") not in state.get("users", {}):
                raise StoreError("removed playlist entry needs an existing user")
            _instant(data.get("removed_at"), "removed_at")
        if not isinstance(data["source_kind"], str) or \
                data["source_kind"] not in {"event", "smart_block", "podcast_feed"} or \
                not isinstance(data["entry_template"], dict):
            raise StoreError("invalid playlist entry source/template")
        if data["source_kind"] == "smart_block":
            try:
                block_version = state["smart_block_versions"][str(UUID(str(data["smart_block_version_id"])))]["data"]
                playlist = state["playlists"][str(UUID(version["playlist_id"]))]["data"]
                block = state["smart_blocks"][str(UUID(block_version["block_id"]))]["data"]
            except (KeyError, TypeError, ValueError, AttributeError) as exc:
                raise StoreError("playlist smart block reference is absent") from exc
            if block["station_id"] != playlist["station_id"]:
                raise StoreError("playlist smart block is outside station")
        elif data.get("smart_block_version_id") is not None:
            raise StoreError("plain playlist event cannot reference a smart block")
        if data["source_kind"] == "podcast_feed":
            try:
                feed = state["podcast_feeds"][str(UUID(str(data["podcast_feed_id"])))]["data"]
                playlist = state["playlists"][str(UUID(version["playlist_id"]))]["data"]
            except (KeyError, TypeError, ValueError, AttributeError) as exc:
                raise StoreError("playlist podcast feed reference is absent") from exc
            if feed["station_id"] != playlist["station_id"] or data["entry_template"]:
                raise StoreError("playlist podcast feed crosses station or embeds a fixed template")
        elif data.get("podcast_feed_id") is not None:
            raise StoreError("only podcast playlist entries may reference a podcast feed")
        _instant(data["created_at"], "created_at")
    if collection == "podcast_feeds":
        if "auto_download_latest" in data and type(data["auto_download_latest"]) is not bool:
            raise StoreError("podcast auto_download_latest must be boolean")
        from .podcast import _canonical_url
        if _canonical_url(data["feed_url"]) != data["feed_url"]:
            raise StoreError("podcast feed URL must be canonical public HTTP(S)")
        _instant(data["created_at"], "created_at")
    if collection == "podcast_episodes":
        feed = state["podcast_feeds"][str(UUID(data["feed_id"]))]["data"]
        if data["station_id"] != feed["station_id"]:
            raise StoreError("podcast episode station differs from feed")
        from .podcast import _canonical_url
        if _canonical_url(data["enclosure_url"]) != data["enclosure_url"]:
            raise StoreError("podcast enclosure URL must be canonical public HTTP(S)")
        if any(not isinstance(data[field], str) or not data[field].strip()
               for field in ("source_key", "title")):
            raise StoreError("podcast episode source key/title must be nonempty")
        digest = data["content_hash"]
        if not isinstance(digest, str) or len(digest) != 64 or \
                any(char not in "0123456789abcdef" for char in digest.lower()):
            raise StoreError("podcast episode content hash must be SHA-256")
        for field in ("published_at", "first_seen_at", "last_seen_at"):
            _instant(data[field], field)
        for field, target in (("item_id", "items"), ("version_id", "versions"),
                              ("media_file_id", "media_files")):
            if data.get(field) is not None:
                try:
                    linked = state[target][str(UUID(str(data[field])))]["data"]
                except (KeyError, TypeError, ValueError, AttributeError) as exc:
                    raise StoreError(f"podcast episode {field} reference is absent") from exc
                if field == "version_id" and data.get("item_id") is not None and \
                        linked["item_id"] != data["item_id"]:
                    raise StoreError("podcast version belongs to another item")
                if field == "media_file_id" and data.get("version_id") is not None and \
                        linked["version_id"] != data["version_id"]:
                    raise StoreError("podcast media belongs to another version")
    if collection == "listener_mounts":
        output = state["station_outputs"][str(UUID(data["station_output_id"]))]["data"]
        endpoint = state["endpoints"][str(UUID(data["endpoint_id"]))]["data"]
        if endpoint["station_id"] != data["station_id"] or \
                any(output[field] != data[field] for field in
                    ("station_id", "endpoint_id", "output_id")):
            raise StoreError("listener mount station/endpoint/output disagree")
        from .listener_stats import _code, _path, _source
        for field in ("output_id", "mount_key"):
            _code(data[field], field)
        _path(data["mount_path"])
        _source(data["source_namespace"])
        stale = data["stale_after_seconds"]
        if isinstance(stale, bool) or not isinstance(stale, int) or not 15 <= stale <= 86400:
            raise StoreError("listener mount stale interval must be 15..86400 seconds")
        _instant(data["created_at"], "created_at")
    if collection == "listener_samples":
        mount = state["listener_mounts"][str(UUID(data["mount_id"]))]["data"]
        if any(data[field] != mount[field] for field in
               ("station_id", "endpoint_id", "output_id", "source_namespace")):
            raise StoreError("listener sample scope differs from mount")
        from .listener_stats import _sample_key, _claim_fingerprint
        _sample_key(data["source_sample_key"])
        if not isinstance(data["source_state"], str) or \
                data["source_state"] not in {"online", "offline", "unknown"}:
            raise StoreError("invalid listener source state")
        listeners = data.get("listeners")
        if data["source_state"] == "online":
            if isinstance(listeners, bool) or not isinstance(listeners, int) or \
                    not 0 <= listeners <= 1000000000:
                raise StoreError("online listener count must be nonnegative")
        elif listeners is not None:
            raise StoreError("offline or unknown listener count must be null")
        for field in ("observed_at", "recorded_at"):
            _instant(data[field], field)
        digest = data["claim_fingerprint"]
        if not isinstance(digest, str) or len(digest) != 64 or \
                any(char not in "0123456789abcdef" for char in digest.lower()):
            raise StoreError("listener claim fingerprint must be SHA-256")
        claim_fields = ("mount_id", "station_id", "endpoint_id", "output_id",
                        "source_namespace", "source_sample_key", "observed_at",
                        "source_state", "listeners")
        if digest != _claim_fingerprint({field: data.get(field) for field in claim_fields}):
            raise StoreError("listener claim fingerprint differs from sample")
    if collection == "libretime_ids":
        if not isinstance(data["source_kind"], str) or \
                data["source_kind"] not in {"plan", "revision", "entry", "file", "webstream"} or \
                not isinstance(data["source_key"], str) or not data["source_key"].strip() or \
                len(data["source_key"]) > 256:
            raise StoreError("invalid LibreTime source identity")
        external_id = data["external_id"]
        if isinstance(external_id, bool) or not isinstance(external_id, int) or \
                not 1 <= external_id <= 2147483647:
            raise StoreError("LibreTime external ID must be a positive 32-bit integer")
        _instant(data["created_at"], "created_at")
    if collection == "versions":
        if not isinstance(data["version_no"], int) or data["version_no"] < 1:
            raise StoreError("version_no must be positive")
        if data["status"] == "approved" and not data.get("approved_at"):
            raise StoreError("approved version needs approved_at")
        for name in ("valid_from", "valid_to"):
            if data.get(name):
                _instant(data[name], name)
        if data.get("valid_from") and data.get("valid_to") and _instant(data["valid_to"], "valid_to") <= _instant(data["valid_from"], "valid_from"):
            raise StoreError("version validity interval is empty")
    if collection == "plan_entries":
        revision = state["plan_revisions"][str(UUID(data["plan_revision_id"]))]["data"]
        plan = state["plans"][str(UUID(revision["plan_id"]))]["data"]
        service = _active_service(state, plan["station_id"], plan["service_key"])
        if plan["service_key"] != "default" and not service:
            raise StoreError("plan entry broadcast service is disabled")
        if revision["status"] != "draft":
            raise StoreError("approved plan revision entries are immutable")
        if data["state"] not in {"planned", "skipped", "replaced", "cancelled"} or \
                isinstance(data["ordinal"], bool) or not isinstance(data["ordinal"], int) or \
                data["ordinal"] < 0:
            raise StoreError("invalid plan entry state or ordinal")
        _instant(data["scheduled_at"], "scheduled_at")
        if data["event_kind"] not in {"audio", "live", "stream", "macro", "wait",
                                      "marker", "ad_break", "chain"}:
            raise StoreError("unsupported plan entry kind")
        payload = data.get("payload") or {}
        if service and data["event_kind"] in {"audio", "live", "stream"} and \
                payload.get("output_id") != service["output_id"]:
            raise StoreError("plan sound output differs from its broadcast service")
        if service and payload.get("output_id") not in (None, service["output_id"]):
            raise StoreError("plan output differs from its broadcast service")
        if data["event_kind"] == "audio":
            version_id = payload.get("version_id")
            if version_id and str(UUID(version_id)) not in state.get("versions", {}):
                raise StoreError("plan audio version is absent")
        episode_id = data.get("podcast_episode_id") or payload.get("podcast_episode_id")
        feed_id = data.get("podcast_feed_id")
        if episode_id is not None or feed_id is not None:
            try:
                episode = state["podcast_episodes"][str(UUID(str(episode_id)))]["data"]
                feed = state["podcast_feeds"][episode["feed_id"]]["data"]
            except (KeyError, TypeError, ValueError, AttributeError) as exc:
                raise StoreError("plan podcast provenance references an absent episode") from exc
            if episode["station_id"] != plan["station_id"] or \
                    feed["station_id"] != plan["station_id"] or \
                    (feed_id is not None and feed_id != episode["feed_id"]) or \
                    (data.get("podcast_episode_id") is not None and
                     payload.get("podcast_episode_id") != data["podcast_episode_id"]) or \
                    data["event_kind"] != "audio" or \
                    (episode.get("item_id") is not None and
                     payload.get("item_id") != episode["item_id"]):
                raise StoreError("plan podcast provenance crosses feed, station or catalog item")
        if data.get("schedule_series_id") or data.get("schedule_date"):
            series_id = data.get("schedule_series_id")
            if not series_id or not data.get("schedule_date"):
                raise StoreError("plan schedule provenance is incomplete")
            series = state.get("schedule_series", {}).get(str(UUID(series_id)))
            if series is None:
                raise StoreError("plan schedule series is absent")
            if series["data"]["station_id"] != plan["station_id"] or \
                    series["data"]["service_key"] != plan["service_key"]:
                raise StoreError("plan schedule series belongs to another broadcast service")
            try:
                date.fromisoformat(data["schedule_date"])
            except (TypeError, ValueError) as exc:
                raise StoreError("invalid plan schedule date") from exc
    if collection == "media_files":
        digest = data.get("sha256")
        if digest is not None and (not isinstance(digest, str) or len(digest) != 64 or
                                   any(c not in "0123456789abcdef" for c in digest.lower())):
            raise StoreError("media SHA-256 must be 64 hex digits")
        if data["status"] == "verified" and (digest is None or not isinstance(data.get("size_bytes"), int) or data["size_bytes"] < 0):
            raise StoreError("verified media needs digest and size")
    if collection == "media_locations":
        media = state["media_files"][str(UUID(data["media_file_id"]))]["data"]
        if data["status"] not in {"present", "verified", "missing", "corrupt"}:
            raise StoreError("invalid media location status")
        if data["sha256"] != media.get("sha256") or data["size_bytes"] != media.get("size_bytes"):
            raise StoreError("media location digest/size differs from file")
        if not isinstance(data["storage_uri"], str) or not data["storage_uri"].startswith("sdb-object:"):
            raise StoreError("managed media URI must be sdb-object digest")
    if collection == "endpoint_copies" and data["status"] == "verified":
        file = state["media_files"][str(UUID(data["media_file_id"]))]["data"]
        if data.get("sha256") != file.get("sha256") or not data.get("verified_at"):
            raise StoreError("endpoint copy digest/verification does not match media")
    if collection == "person_memberships":
        person = state["people"][str(UUID(data["person_id"]))]["data"]
        group = state["people"][str(UUID(data["group_id"]))]["data"]
        if person["kind"] != "person" or group["kind"] != "group" or data["person_id"] == data["group_id"]:
            raise StoreError("membership requires one person and one group")
        start = _instant(data["valid_from"], "valid_from") if data.get("valid_from") else None
        end = _instant(data["valid_to"], "valid_to") if data.get("valid_to") else None
        if start and end and end <= start:
            raise StoreError("membership validity interval is empty")
    if collection == "rotation_rules":
        from .rotation import _config
        if not isinstance(data["name"], str) or not data["name"].strip():
            raise StoreError("rotation rule name must be nonempty")
        if _config(data["config"]) != data["config"]:
            raise StoreError("rotation rule config must be normalized")
    if collection == "import_profiles":
        from .settings import validate_import_config
        if not isinstance(data["name"], str) or not data["name"].strip() or \
                not isinstance(data["library_key"], str) or not data["library_key"].strip():
            raise StoreError("import profile name and library key are required")
        if validate_import_config(data["config"], persisted=True) != data["config"]:
            raise StoreError("import profile config must be normalized")
    if collection == "view_profiles":
        from .settings import _COLUMNS, _VIEWS
        columns = data["columns"]
        if data["view_kind"] not in _VIEWS or not isinstance(data["name"], str) or \
                not data["name"].strip() or not isinstance(columns, list) or not columns or \
                any(not isinstance(column, str) or column not in _COLUMNS for column in columns) or \
                len(columns) != len(set(columns)) or \
                not isinstance(data["filters"], dict):
            raise StoreError("invalid view profile")
        filters = data["filters"]
        if set(filters) - {"kind", "category_id", "lifecycle", "ready"} or \
                ("ready" in filters and not isinstance(filters["ready"], bool)):
            raise StoreError("invalid view filters")
    if collection in {"import_profiles", "view_profiles"}:
        version = data["config_version"]
        if isinstance(version, bool) or not isinstance(version, int) or version < 1:
            raise StoreError("profile config_version must be positive")
    if collection == "schedule_series":
        if not isinstance(data["service_key"], str) or not data["service_key"].strip():
            raise StoreError("schedule service_key must be nonempty")
        try:
            start = date.fromisoformat(data["starts_on"])
            end = date.fromisoformat(data["ends_on"]) if data.get("ends_on") else None
        except (ValueError, TypeError) as exc:
            raise StoreError("invalid schedule series dates") from exc
        if end is not None and end < start:
            raise StoreError("schedule series end precedes start")
        if not isinstance(data["title"], str) or not data["title"].strip():
            raise StoreError("schedule series title required")
    if collection == "broadcast_services":
        if any(not isinstance(data[name], str) or not data[name].strip()
               for name in ("service_key", "name", "output_id")):
            raise StoreError("broadcast service key, name and output must be nonempty")
        endpoint = state["endpoints"][str(UUID(data["endpoint_id"]))]["data"]
        if endpoint["station_id"] != data["station_id"]:
            raise StoreError("broadcast service endpoint belongs to another station")
        if not any(row["data"]["station_id"] == data["station_id"] and
                   row["data"]["endpoint_id"] == data["endpoint_id"] and
                   row["data"]["output_id"] == data["output_id"] and
                   row["data"]["status"] == "active"
                   for row in state.get("station_outputs", {}).values()):
            raise StoreError("broadcast service needs an active station output")
    if collection == "plans":
        if data["state"] not in STATUS["plans"]:
            raise StoreError("invalid plan state")
        if not isinstance(data["service_key"], str) or not data["service_key"].strip():
            raise StoreError("plan service_key must be nonempty")
        if data["service_key"] != "default" and not _active_service(
                state, data["station_id"], data["service_key"]):
            raise StoreError("plan broadcast service is absent or disabled")
    if collection == "schedule_segments":
        rule = data["rule"]
        if not isinstance(rule, dict) or rule.get("frequency") not in {
                "weekly", "biweekly", "monthly_nth", "monthly_last"} or \
                isinstance(rule.get("weekday"), bool) or rule.get("weekday") not in range(1, 8):
            raise StoreError("invalid schedule recurrence rule")
        frequency = rule["frequency"]
        expected_keys = ({"frequency", "weekday", "nth"} if frequency == "monthly_nth" else
                         {"frequency", "weekday", "anchor_date"} if frequency in {"weekly", "biweekly"} else
                         {"frequency", "weekday"})
        if set(rule) != expected_keys or (frequency == "monthly_nth" and
                                          (isinstance(rule.get("nth"), bool) or
                                           rule.get("nth") not in range(1, 6))):
            raise StoreError("schedule recurrence rule is not normalized")
        try:
            start = date.fromisoformat(data["starts_on"])
            end = date.fromisoformat(data["end_before"]) if data.get("end_before") else None
            if frequency in {"weekly", "biweekly"}:
                date.fromisoformat(rule["anchor_date"])
            local = time.fromisoformat(data["local_time"])
        except (ValueError, TypeError) as exc:
            raise StoreError("invalid schedule segment date or local time") from exc
        if end is not None and end <= start:
            raise StoreError("schedule segment is empty")
        if local.tzinfo is not None or data["dst_fold"] not in (0, 1) or \
                isinstance(data["dst_fold"], bool) or not isinstance(data["entry_template"], dict):
            raise StoreError("invalid schedule local time, DST fold or entry template")
    if collection in {"schedule_exceptions", "schedule_occurrences"}:
        try:
            date.fromisoformat(data["service_date"])
        except (ValueError, TypeError) as exc:
            raise StoreError("invalid schedule exception or occurrence date") from exc
    if collection == "schedule_exceptions" and data["action"] not in {"skip", "replace", "reset"}:
        raise StoreError("invalid schedule exception action")
    if collection == "schedule_occurrences":
        linked = [data.get(key) for key in ("plan_id", "plan_revision_id", "plan_entry_id")]
        if any(linked) and not all(linked):
            raise StoreError("schedule occurrence plan links must be complete")
        if data["status"] == "materialized" and not all(linked):
            raise StoreError("materialized occurrence needs plan links")
        if not isinstance(data["source_fingerprint"], str) or not data["source_fingerprint"]:
            raise StoreError("schedule occurrence needs source fingerprint")
        if all(linked):
            plan_id, revision_id, entry_id = (str(UUID(value)) for value in linked)
            plan = state.get("plans", {}).get(plan_id)
            revision = state.get("plan_revisions", {}).get(revision_id)
            entry = state.get("plan_entries", {}).get(entry_id)
            series = state["schedule_series"][str(UUID(data["series_id"]))]["data"]
            if not plan or not revision or not entry or \
                    plan["data"]["station_id"] != series["station_id"] or \
                    plan["data"]["service_key"] != series["service_key"] or \
                    revision["data"]["plan_id"] != plan_id or \
                    entry["data"]["plan_revision_id"] != revision_id:
                raise StoreError("schedule occurrence plan links are inconsistent")
    if collection == "radio_cues":
        from .radio_cues import CUE_KINDS, CUE_SOURCES, CUE_UNITS
        if data["cue_kind"] not in CUE_KINDS or data["unit"] not in CUE_UNITS or \
                data["source_kind"] not in CUE_SOURCES or \
                isinstance(data["position_ms"], bool) or \
                not isinstance(data["position_ms"], int) or data["position_ms"] < 0:
            raise StoreError("invalid radio cue semantics or position")
        version = state["versions"][str(UUID(data["version_id"]))]["data"]
        links = [row["data"] for row in state.get("station_items", {}).values()
                 if row["data"]["item_id"] == version["item_id"] and
                 row["data"]["station_id"] == data["station_id"]]
        if not links:
            raise StoreError("radio cue version is outside station")
        if data.get("media_file_id"):
            media = state.get("media_files", {}).get(str(UUID(data["media_file_id"])))
            if media is None or media["data"]["version_id"] != data["version_id"]:
                raise StoreError("radio cue media belongs to another version")
            if data["status"] == "approved" and (
                    media["data"]["status"] != "verified" or
                    data.get("media_sha256") != media["data"].get("sha256")):
                raise StoreError("approved radio cue needs matching verified media")
        elif data["status"] == "approved":
            raise StoreError("approved radio cue needs media_file_id")
    if collection == "plan_transitions":
        revision = state["plan_revisions"][str(UUID(data["plan_revision_id"]))]["data"]
        plan = state["plans"][str(UUID(revision["plan_id"]))]["data"]
        first = state["plan_entries"][str(UUID(data["from_entry_id"]))]["data"]
        second = state["plan_entries"][str(UUID(data["to_entry_id"]))]["data"]
        if plan["station_id"] != data["station_id"] or \
                data["from_entry_id"] == data["to_entry_id"] or \
                first["plan_revision_id"] != data["plan_revision_id"] or \
                second["plan_revision_id"] != data["plan_revision_id"] or \
                not isinstance(data["manual_lock"], bool):
            raise StoreError("plan transition references are inconsistent")
    if collection == "production_placements":
        from .production import PLACEMENT_KINDS, START_REFERENCES
        transition = state["plan_transitions"][str(UUID(data["transition_id"]))]["data"]
        version = state["versions"][str(UUID(data["content_version_id"]))]["data"]
        item = state["items"][str(UUID(version["item_id"]))]["data"]
        if data["station_id"] != transition["station_id"] or \
                data["plan_revision_id"] != transition["plan_revision_id"] or \
                data["anchor_entry_id"] not in {transition["from_entry_id"],
                                                 transition["to_entry_id"]} or \
                data["kind"] not in PLACEMENT_KINDS or item["kind"] != data["kind"] or \
                data["start_reference"] not in START_REFERENCES or \
                isinstance(data["offset_ms"], bool) or not isinstance(data["offset_ms"], int) or \
                not isinstance(data["manual_lock"], bool):
            raise StoreError("invalid production placement")
        if data.get("cue_id"):
            cue = state.get("radio_cues", {}).get(str(UUID(data["cue_id"])))
            if cue is None or cue["data"]["version_id"] != data["content_version_id"]:
                raise StoreError("placement cue belongs to another version")
        if data["status"] == "approved":
            media_id = data.get("media_file_id")
            media = state.get("media_files", {}).get(str(UUID(media_id))) if media_id else None
            if media is None or media["data"]["version_id"] != data["content_version_id"] or \
                    media["data"]["status"] != "verified" or \
                    data.get("media_sha256") != media["data"].get("sha256"):
                raise StoreError("approved placement needs matching verified media")
    if collection == "controller_profiles":
        if not isinstance(data["code"], str) or not data["code"].strip() or \
                not isinstance(data["name"], str) or not data["name"].strip():
            raise StoreError("controller profile code and name are required")
        current_id = data.get("current_version_id")
        if current_id:
            current = state.get("controller_profile_versions", {}).get(str(UUID(current_id)))
            if current is None or current["data"]["status"] != "published":
                raise StoreError("current controller version must be published")
    if collection == "controller_profile_versions":
        from .controller_profiles import _device_spec
        if isinstance(data["version_no"], bool) or not isinstance(data["version_no"], int) or \
                data["version_no"] < 1 or _device_spec(data["device_spec"]) != data["device_spec"]:
            raise StoreError("invalid controller version number or device specification")
    if collection == "controller_banks":
        version = state["controller_profile_versions"][str(UUID(data["profile_version_id"]))]["data"]
        if version["status"] != "draft" or isinstance(data["bank_no"], bool) or \
                not isinstance(data["bank_no"], int) or not 0 <= data["bank_no"] <= 127 or \
                not isinstance(data["name"], str) or not data["name"].strip():
            raise StoreError("controller bank needs draft version, number 0..127 and name")
    if collection == "controller_pads":
        from .controller_profiles import MIDI_EVENTS, PAD_TARGETS, TRIGGER_MODES
        bank = state["controller_banks"][str(UUID(data["bank_id"]))]["data"]
        version = state["controller_profile_versions"][str(UUID(bank["profile_version_id"]))]["data"]
        profile = state["controller_profiles"][str(UUID(version["profile_id"]))]["data"]
        if version["status"] != "draft" or \
                isinstance(data["pad_no"], bool) or not isinstance(data["pad_no"], int) or \
                not 0 <= data["pad_no"] <= 127 or \
                data["midi_event"] not in MIDI_EVENTS or \
                isinstance(data["channel"], bool) or not isinstance(data["channel"], int) or \
                not 1 <= data["channel"] <= 16 or \
                isinstance(data["number"], bool) or not isinstance(data["number"], int) or \
                not 0 <= data["number"] <= 127 or \
                data["target_kind"] not in PAD_TARGETS or \
                data["trigger_mode"] not in TRIGGER_MODES:
            raise StoreError("invalid controller pad or MIDI address")
        target_id = str(UUID(data["target_id"]))
        if data["target_kind"] == "cart":
            target = state.get("carts", {}).get(target_id)
            if target is None or target["data"]["station_id"] != profile["station_id"]:
                raise StoreError("controller pad cart belongs to another station")
        else:
            target = state.get("versions", {}).get(target_id)
            if target is None or not any(
                    row["data"]["station_id"] == profile["station_id"] and
                    row["data"]["item_id"] == target["data"]["item_id"]
                    for row in state.get("station_items", {}).values()):
                raise StoreError("controller pad version belongs to another station")
    if collection == "campaigns":
        if _instant(data["ends_at"], "ends_at") <= _instant(data["starts_at"], "starts_at"):
            raise StoreError("campaign interval is empty")
    if collection == "obligations":
        if data["required_plays"] < 1 or _instant(data["window_end"], "window_end") <= _instant(data["window_start"], "window_start"):
            raise StoreError("invalid advertising obligation")
    if collection == "placements":
        obligation = state["obligations"][str(UUID(data["obligation_id"]))]["data"]
        spot = state["spots"][str(UUID(data["spot_id"]))]["data"]
        entry = state["plan_entries"][str(UUID(data["plan_entry_id"]))]["data"]
        revision = state["plan_revisions"][str(UUID(entry["plan_revision_id"]))]["data"]
        plan = state["plans"][str(UUID(revision["plan_id"]))]["data"]
        if (spot["campaign_id"] != obligation["campaign_id"] or
                plan["station_id"] != obligation["station_id"] or
                entry["event_kind"] != "audio" or
                entry.get("payload", {}).get("version_id") != spot["version_id"] or
                not entry.get("ad_break_id")):
            raise StoreError("advertising placement links disagree")
        if data.get("output_id") and entry.get("payload", {}).get("output_id") != data["output_id"]:
            raise StoreError("advertising placement output differs from plan audio")
        if data["state"] == "planned" and revision["status"] != "draft":
            raise StoreError("new planned placement requires a draft plan revision")
        makegood_id = data.get("makegood_request_id")
        if makegood_id is not None:
            try:
                request = state.get("makegood_requests", {}).get(str(UUID(makegood_id)))
            except (TypeError, ValueError) as exc:
                raise StoreError("invalid makegood request ID on placement") from exc
            if request is None or request["data"].get("status") != "reserved":
                raise StoreError("makegood placement needs a reserved request")
            if request["data"]["obligation_id"] != data["obligation_id"] or \
                    request["data"]["spot_id"] != data["spot_id"] or \
                    request["data"]["break_entry_id"] != entry.get("ad_break_id"):
                raise StoreError("makegood placement and request disagree")
    if collection == "makegood_requests":
        campaign = state["campaigns"][str(UUID(data["campaign_id"]))]["data"]
        obligation = state["obligations"][str(UUID(data["obligation_id"]))]["data"]
        spot = state["spots"][str(UUID(data["spot_id"]))]["data"]
        media = state["media_files"][str(UUID(data["media_file_id"]))]["data"]
        plan = state["plans"][str(UUID(data["plan_id"]))]["data"]
        revision = state["plan_revisions"][str(UUID(data["plan_revision_id"]))]["data"]
        entry = state["plan_entries"][str(UUID(data["break_entry_id"]))]["data"]
        if obligation["campaign_id"] != data["campaign_id"] or \
                spot["campaign_id"] != data["campaign_id"] or \
                spot["version_id"] != data["version_id"] or \
                media["version_id"] != data["version_id"] or \
                media.get("sha256") != data["media_sha256"] or \
                (media.get("technical") or {}).get("duration_ms") != data["duration_ms"] or \
                plan["station_id"] != data["station_id"] or \
                obligation["station_id"] != data["station_id"] or \
                data["station_id"] not in (campaign.get("station_ids") or []) or \
                revision["plan_id"] != data["plan_id"] or \
                entry["plan_revision_id"] != data["plan_revision_id"] or \
                entry["event_kind"] != "ad_break":
            raise StoreError("makegood request links disagree")
        try:
            request_id = str(UUID(data["request_id"]))
        except (TypeError, ValueError) as exc:
            raise StoreError("makegood request ID must be a UUID") from exc
        if request_id not in state.get("makegood_requests", {}) or \
                state["makegood_requests"][request_id]["data"].get("request_id") != request_id or \
                (data.get("region_id") is not None and
                 not isinstance(data["region_id"], str)) or \
                not isinstance(data["output_id"], str) or not data["output_id"] or \
                not isinstance(data["idempotency_key"], str) or \
                not data["idempotency_key"].strip() or \
                not 1 <= len(data["idempotency_key"]) <= 160 or \
                not isinstance(data["preview_hash"], str) or len(data["preview_hash"]) != 64 or \
                not isinstance(data["recommendation_hash"], str) or \
                len(data["recommendation_hash"]) != 64:
            raise StoreError("invalid makegood request identity")
        if (type(data["required_plays"]) is not int or data["required_plays"] < 1 or
                type(data["duration_ms"]) is not int or data["duration_ms"] < 1 or
                type(data["future_reserved_at_proposal"]) is not int or
                data["future_reserved_at_proposal"] < 0 or
                type(data["content_verified_plays_at_proposal"]) is not int or
                data["content_verified_plays_at_proposal"] != 0):
            raise StoreError("makegood proposal cannot claim unverified delivery")
        recommendation_keys = ("station_id", "campaign_id", "obligation_id", "spot_id",
                               "version_id", "media_file_id", "media_sha256", "duration_ms",
                               "plan_id", "plan_revision_id", "break_entry_id",
                               "output_id", "region_id", "scheduled_at", "preview_hash",
                               "required_plays", "future_reserved_at_proposal",
                               "content_verified_plays_at_proposal")
        recommendation = {key: data.get(key) for key in recommendation_keys}
        actual_hash = hashlib.sha256(json.dumps(recommendation, ensure_ascii=False,
                                                sort_keys=True, separators=(",", ":"),
                                                allow_nan=False).encode()).hexdigest()
        if actual_hash != data["recommendation_hash"]:
            raise StoreError("makegood recommendation hash differs from snapshot")
        if data["status"] == "rejected" and (not data.get("review_reason") or
                                               not data.get("reviewed_by") or
                                               not data.get("reviewed_at")):
            raise StoreError("rejected makegood needs reviewer, time and reason")
        if data["status"] == "cancelled" and (not data.get("cancel_reason") or
                                                not data.get("cancelled_by") or
                                                not data.get("cancelled_at")):
            raise StoreError("cancelled makegood needs provenance")
        _instant(data["scheduled_at"], "scheduled_at")
        _instant(data["proposed_at"], "proposed_at")
        if data["status"] in {"approved", "reserved"}:
            if not data.get("approved_by") or not data.get("approved_at") or \
                    data["approved_by"] == data["proposed_by"] or \
                    data.get("reviewed_by") != data["approved_by"] or \
                    data.get("reviewed_at") != data["approved_at"]:
                raise StoreError("makegood needs an independent approver")
            if str(UUID(data["approved_by"])) not in state.get("users", {}):
                raise StoreError("makegood approver is absent")
            _instant(data["approved_at"], "approved_at")
        if data["status"] == "reserved":
            placement_id = data.get("placement_id")
            try:
                placement = (state.get("placements", {}).get(str(UUID(placement_id)))
                             if placement_id else None)
            except (TypeError, ValueError) as exc:
                raise StoreError("invalid makegood placement ID") from exc
            if placement is None or placement["data"].get("makegood_request_id") != data["request_id"] or \
                    placement["data"]["obligation_id"] != data["obligation_id"] or \
                    placement["data"]["spot_id"] != data["spot_id"]:
                raise StoreError("reserved makegood needs its linked placement")
            if not data.get("reserved_by") or \
                    str(UUID(data["reserved_by"])) not in state.get("users", {}):
                raise StoreError("reserved makegood needs a real operator")
            _instant(data["reserved_at"], "reserved_at")
    if collection == "rights_grants":
        if data["decision"] not in ("allow", "deny"):
            raise StoreError("rights decision must be allow or deny")
        if _instant(data["ends_at"], "ends_at") <= _instant(data["starts_at"], "starts_at"):
            raise StoreError("rights interval is empty")
    if collection in ("metadata_values", "metadata_suggestions"):
        field = state["field_definitions"][str(UUID(data["field_id"]))]["data"]
        target_collection = data["target_collection"]
        if target_collection != ("items" if field["scope"] == "item" else "versions"):
            raise StoreError("metadata field and target scopes differ")
        if str(UUID(data["target_id"])) not in state.get(target_collection, {}):
            raise StoreError("metadata target is absent")
        if collection == "metadata_values":
            if data["is_cleared"] != (data.get("value") is None):
                raise StoreError("metadata cleared state and value disagree")
            if data["station_scope"] != "*" and str(UUID(data["station_scope"])) not in state.get("stations", {}):
                raise StoreError("metadata station scope is absent")
    if collection == "external_ids":
        target_collection = data["target_collection"]
        if target_collection not in {"items", "versions", "people", "campaigns"} or \
                str(UUID(data["target_id"])) not in state.get(target_collection, {}):
            raise StoreError("external ID target is absent")
    if collection == "playout_attempts":
        endpoint = state["endpoints"][str(UUID(data["endpoint_id"]))]["data"]
        if endpoint["station_id"] != data["station_id"]:
            raise StoreError("attempt endpoint belongs to another station")
    if collection in {"diagnostic_events", "incidents"}:
        from .diagnostics import COMPONENTS, CONDITIONS, IMPACTS, SEVERITIES
        if data["component"] not in COMPONENTS or data["severity"] not in SEVERITIES or \
                data["impact"] not in IMPACTS:
            raise StoreError("invalid diagnostic classification")
        endpoint_id = data.get("endpoint_id")
        if endpoint_id:
            endpoint = state.get("endpoints", {}).get(str(UUID(endpoint_id)))
            if endpoint is None or endpoint["data"]["station_id"] != data["station_id"]:
                raise StoreError("diagnostic endpoint belongs to another station")
        if not isinstance(data["correlation_key"], str) or \
                not 0 < len(data["correlation_key"]) <= 160:
            raise StoreError("invalid diagnostic correlation key")
    if collection == "diagnostic_events":
        incident = state["incidents"][str(UUID(data["incident_id"]))]["data"]
        if incident["station_id"] != data["station_id"] or \
                incident["correlation_key"] != data["correlation_key"] or \
                incident["component"] != data["component"] or \
                data["condition"] not in CONDITIONS:
            raise StoreError("diagnostic event and incident disagree")
        for field, limit in (("event_key", 160), ("code", 80), ("message", 500)):
            if not isinstance(data[field], str) or not 0 < len(data[field]) <= limit:
                raise StoreError(f"invalid diagnostic {field}")
        for field in ("occurred_at", "recorded_at"):
            _instant(data[field], field)
        digest = data["fingerprint"]
        if not isinstance(digest, str) or len(digest) != 64 or \
                any(char not in "0123456789abcdef" for char in digest):
            raise StoreError("invalid diagnostic fingerprint")
        if not isinstance(data["details"], dict):
            raise StoreError("diagnostic details must be an object")
    if collection == "incidents":
        if data["state"] not in {"open", "acknowledged", "resolved"} or \
                data["last_condition"] not in CONDITIONS or \
                isinstance(data["occurrence_count"], bool) or \
                not isinstance(data["occurrence_count"], int) or \
                data["occurrence_count"] < 1:
            raise StoreError("invalid incident state or event count")
        for field in ("first_seen_at", "last_seen_at"):
            _instant(data[field], field)
        for field in ("first_event_id", "last_event_id"):
            event = state.get("diagnostic_events", {}).get(str(UUID(data[field])))
            if event is None or event["data"]["station_id"] != data["station_id"] or \
                    event["data"]["correlation_key"] != data["correlation_key"]:
                raise StoreError("incident event link is inconsistent")
        if data.get("assigned_user_id") and \
                str(UUID(data["assigned_user_id"])) not in state.get("users", {}):
            raise StoreError("incident assignee is absent")
        if data["state"] == "resolved":
            if data.get("resolution") not in {"recovered", "false_alarm", "handed_off"} or \
                    not data.get("resolved_at") or not data.get("resolved_by"):
                raise StoreError("resolved incident needs a resolution record")
            _instant(data["resolved_at"], "resolved_at")
            if str(UUID(data["resolved_by"])) not in state.get("users", {}):
                raise StoreError("incident resolver is absent")
    if collection == "listener_requests":
        from .listener_requests import REJECTION_REASONS
        import re
        if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,64}", data["source_namespace"]) or \
                not isinstance(data["external_request_key"], str) or \
                not 1 <= len(data["external_request_key"]) <= 200 or \
                any(ord(char) < 32 or ord(char) == 127
                    for char in data["external_request_key"]):
            raise StoreError("invalid listener request source key")
        if data.get("requester_fingerprint") is not None and \
                not re.fullmatch(r"[0-9a-f]{64}", data["requester_fingerprint"]):
            raise StoreError("requester fingerprint must be a lowercase HMAC digest")
        _instant(data["requested_at"], "requested_at")
        if not any(row["data"]["station_id"] == data["station_id"] and
                   row["data"]["item_id"] == data["item_id"]
                   for row in state.get("station_items", {}).values()):
            raise StoreError("listener request item is outside station")
        for field in ("requested_version_id", "selected_version_id"):
            if data.get(field):
                version = state.get("versions", {}).get(str(UUID(data[field])))
                if version is None or version["data"]["item_id"] != data["item_id"]:
                    raise StoreError("listener request version differs from item")
                if field == "selected_version_id" and version["data"]["status"] != "approved":
                    raise StoreError("listener request selected version is not approved")
        if data["status"] in {"accepted", "queued", "fulfilled"} and \
                not data.get("selected_version_id"):
            raise StoreError("accepted listener request needs selected version")
        if data["status"] == "rejected" and data.get("decision_reason") not in REJECTION_REASONS:
            raise StoreError("rejected listener request needs a reason")
        for field in ("decided_by", "queued_by", "fulfilled_by"):
            if data.get(field) and str(UUID(data[field])) not in state.get("users", {}):
                raise StoreError("listener request actor is absent")
        if data["status"] in {"queued", "fulfilled"}:
            if data.get("queue_mode") not in {"plan", "manual"} or \
                    not data.get("queued_at") or not data.get("queued_by"):
                raise StoreError("queued listener request lacks provenance")
            _instant(data["queued_at"], "queued_at")
            if data["queue_mode"] == "plan" and not data.get("plan_entry_id"):
                raise StoreError("planned listener request needs plan entry")
        if data.get("plan_entry_id"):
            entry = state.get("plan_entries", {}).get(str(UUID(data["plan_entry_id"])))
            if entry is None or entry["data"].get("payload", {}).get("version_id") != \
                    data.get("selected_version_id"):
                raise StoreError("listener request plan entry differs from selected version")
            revision = state["plan_revisions"][str(UUID(entry["data"]["plan_revision_id"]))]["data"]
            plan = state["plans"][str(UUID(revision["plan_id"]))]["data"]
            if plan["station_id"] != data["station_id"]:
                raise StoreError("listener request plan belongs to another station")
        if data["status"] == "fulfilled":
            attempt_id = data.get("fulfillment_attempt_id")
            attempt = state.get("playout_attempts", {}).get(str(UUID(attempt_id))) if attempt_id else None
            if attempt is None or attempt["data"]["station_id"] != data["station_id"] or \
                    attempt["data"]["version_id"] != data["selected_version_id"] or \
                    attempt["data"].get("plan_entry_id") != data.get("plan_entry_id") or \
                    attempt["data"].get("state") != "completed":
                raise StoreError("listener request needs matching completed attempt")
            _instant(data["fulfilled_at"], "fulfilled_at")
            if not data.get("fulfilled_by"):
                raise StoreError("listener request completion actor is absent")
    if collection == "spot_changes":
        old = state["spots"][str(UUID(data["old_spot_id"]))]["data"]
        new = state["spots"][str(UUID(data["new_spot_id"]))]["data"]
        campaign = state["campaigns"][str(UUID(data["campaign_id"]))]["data"]
        if data["old_spot_id"] == data["new_spot_id"] or \
                old["campaign_id"] != data["campaign_id"] or \
                new["campaign_id"] != data["campaign_id"] or \
                old["version_id"] != data["old_version_id"] or \
                new["version_id"] != data["new_version_id"] or \
                data["old_version_id"] == data["new_version_id"]:
            raise StoreError("spot change needs two versions of one campaign")
        targets = campaign.get("station_ids")
        if targets is not None and data["station_id"] not in targets:
            raise StoreError("spot change campaign does not target station")
        if not isinstance(data["idempotency_key"], str) or \
                not 0 < len(data["idempotency_key"]) <= 200 or \
                isinstance(data["impact_count"], bool) or \
                not isinstance(data["impact_count"], int) or data["impact_count"] < 0:
            raise StoreError("invalid spot change key or impact count")
        for field in ("effective_at", "created_at", "updated_at"):
            if field in data:
                _instant(data[field], field)
    if collection == "spot_change_targets":
        change = state["spot_changes"][str(UUID(data["change_id"]))]["data"]
        placement = state["placements"][str(UUID(data["placement_id"]))]["data"]
        revision = state["plan_revisions"][str(UUID(data["plan_revision_id"]))]["data"]
        plan = state["plans"][str(UUID(data["plan_id"]))]["data"]
        entry = state["plan_entries"][str(UUID(data["plan_entry_id"]))]["data"]
        obligation = state["obligations"][str(UUID(placement["obligation_id"]))]["data"]
        if change["station_id"] != data["station_id"] or \
                change["old_version_id"] != data["old_version_id"] or \
                change["new_version_id"] != data["new_version_id"] or \
                placement["spot_id"] != change["old_spot_id"] or \
                placement["plan_entry_id"] != data["plan_entry_id"] or \
                revision["plan_id"] != data["plan_id"] or \
                entry["plan_revision_id"] != data["plan_revision_id"] or \
                plan["station_id"] != data["station_id"] or \
                obligation["station_id"] != data["station_id"] or \
                obligation["campaign_id"] != change["campaign_id"]:
            raise StoreError("spot change target relations disagree")
        if data["target_kind"] not in {"placement", "endpoint"} or \
                not isinstance(data["target_key"], str) or not data["target_key"] or \
                data["source_revision_status"] not in {"draft", "approved"} or \
                data["source_consistency"] not in {"matching_version", "plan_entry_version_mismatch"}:
            raise StoreError("invalid spot change target kind or provenance")
        if data["target_kind"] == "endpoint":
            endpoint_id = data.get("endpoint_id")
            endpoint = state.get("endpoints", {}).get(str(UUID(endpoint_id))) if endpoint_id else None
            if endpoint is None or endpoint["data"]["station_id"] != data["station_id"]:
                raise StoreError("spot change endpoint target belongs to another station")
            if data["target_key"] != f"endpoint:{data['placement_id']}:{endpoint_id}":
                raise StoreError("spot change endpoint target key differs")
        elif data["target_key"] != f"placement:{data['placement_id']}":
            raise StoreError("spot change placement target key differs")
        if data.get("distribution_id"):
            distribution = state.get("plan_distributions", {}).get(str(UUID(data["distribution_id"])))
            if distribution is None or distribution["data"]["endpoint_id"] != data.get("endpoint_id") or \
                    distribution["data"]["revision_id"] != data["plan_revision_id"]:
                raise StoreError("spot change source distribution differs")
        for field in ("scheduled_at", "created_at", "updated_at"):
            _instant(data[field], field)
        for field in ("source_plan_revision", "source_placement_revision",
                      "source_entry_revision"):
            if isinstance(data[field], bool) or not isinstance(data[field], int) or data[field] < 1:
                raise StoreError("spot change source revision must be positive")
        if data["status"] == "applied":
            replacement_id = data.get("replacement_placement_id")
            replacement = state.get("placements", {}).get(str(UUID(replacement_id))) if replacement_id else None
            if replacement is None or replacement["data"]["spot_id"] != change["new_spot_id"] or \
                    replacement["data"]["obligation_id"] != placement["obligation_id"]:
                raise StoreError("applied spot change lacks matching replacement placement")
            if data["target_kind"] == "endpoint":
                replacement_distribution_id = data.get("replacement_distribution_id")
                accepted = (state.get("plan_distributions", {}).get(str(UUID(replacement_distribution_id)))
                            if replacement_distribution_id else None)
                if accepted is None or accepted["data"]["endpoint_id"] != data["endpoint_id"]:
                    raise StoreError("applied endpoint target lacks matching distribution")
        if data["status"] in {"applied", "superseded"} and \
                (not data.get("resolved_at") or not data.get("resolution_reason")):
            raise StoreError("final spot change target needs resolution evidence")
    if collection == "clock_templates":
        if not isinstance(data["code"], str) or not data["code"].strip() or \
                not isinstance(data["name"], str) or not data["name"].strip():
            raise StoreError("clock template needs code and name")
        _instant(data["created_at"], "created_at")
        for field, expected in (("current_version_id", "published"),
                                ("draft_version_id", "draft")):
            if data.get(field):
                version = state.get("clock_template_versions", {}).get(str(UUID(data[field])))
                if version is None or version["data"]["template_id"] not in [
                        key for key, row in state.get("clock_templates", {}).items()
                        if row["data"] is data] or version["data"]["status"] != expected:
                    raise StoreError("clock template version pointer differs")
    if collection == "clock_template_versions":
        if isinstance(data["version_no"], bool) or not isinstance(data["version_no"], int) or \
                data["version_no"] < 1:
            raise StoreError("clock version number must be positive")
        _instant(data["created_at"], "created_at")
        if data["status"] == "published":
            digest = data.get("fingerprint")
            if not isinstance(digest, str) or len(digest) != 64 or \
                    any(char not in "0123456789abcdef" for char in digest) or \
                    not data.get("published_by") or not data.get("published_at"):
                raise StoreError("published clock needs fingerprint and provenance")
            if str(UUID(data["published_by"])) not in state.get("users", {}):
                raise StoreError("clock publisher is absent")
            _instant(data["published_at"], "published_at")
    if collection == "clock_slots":
        from .clocks import _slot_template
        version = state["clock_template_versions"][str(UUID(data["template_version_id"]))]["data"]
        template = state["clock_templates"][str(UUID(version["template_id"]))]["data"]
        if version["status"] != "draft" or \
                isinstance(data["ordinal"], bool) or not isinstance(data["ordinal"], int) or \
                not 0 <= data["ordinal"] <= 999 or \
                isinstance(data["offset_ms"], bool) or not isinstance(data["offset_ms"], int) or \
                not 0 <= data["offset_ms"] < 3_600_000 or \
                data["slot_kind"] not in {"fixed", "category"} or \
                _slot_template(data["entry_template"], data["slot_kind"]) != data["entry_template"]:
            raise StoreError("invalid clock slot or published version edit")
        if data["slot_kind"] == "category":
            category_id = data.get("category_id")
            category = state.get("categories", {}).get(str(UUID(category_id))) if category_id else None
            if category is None or category["data"]["station_id"] != template["station_id"]:
                raise StoreError("clock slot category belongs to another station")
        elif data.get("category_id") is not None:
            raise StoreError("fixed clock slot cannot name a category")
    if collection == "clock_assignments":
        if not isinstance(data["service_key"], str) or not data["service_key"].strip():
            raise StoreError("clock service_key must be nonempty")
        version = state["clock_template_versions"][str(UUID(data["template_version_id"]))]["data"]
        template = state["clock_templates"][str(UUID(version["template_id"]))]["data"]
        if version["status"] != "published" or template["station_id"] != data["station_id"] or \
                isinstance(data["weekday"], bool) or data["weekday"] not in range(1, 8) or \
                isinstance(data["local_hour"], bool) or data["local_hour"] not in range(24) or \
                isinstance(data["dst_fold"], bool) or data["dst_fold"] not in (0, 1):
            raise StoreError("invalid clock assignment")
        try:
            start = date.fromisoformat(data["starts_on"])
            end = date.fromisoformat(data["ends_on"]) if data.get("ends_on") else None
        except (TypeError, ValueError) as exc:
            raise StoreError("invalid clock assignment dates") from exc
        if end and end < start:
            raise StoreError("clock assignment ends before start")
        if data["status"] == "active":
            for row in state.get("clock_assignments", {}).values():
                other = row["data"]
                if other is data or other.get("status") != "active" or \
                        any(other.get(key) != data[key] for key in ("station_id", "service_key",
                                                               "weekday", "local_hour")):
                    continue
                other_start = date.fromisoformat(other["starts_on"])
                other_end = date.fromisoformat(other["ends_on"]) if other.get("ends_on") else None
                if (end is None or other_start <= end) and \
                        (other_end is None or start <= other_end):
                    raise StoreError("active clock assignments overlap")
    if collection == "clock_exceptions":
        assignment = state["clock_assignments"][str(UUID(data["assignment_id"]))]["data"]
        try:
            day = date.fromisoformat(data["service_date"])
        except (TypeError, ValueError) as exc:
            raise StoreError("invalid clock exception date") from exc
        if day.isoweekday() != assignment["weekday"] or \
                day.isoformat() < assignment["starts_on"] or \
                (assignment.get("ends_on") and day.isoformat() > assignment["ends_on"]):
            raise StoreError("clock exception is outside assignment")
        if data["action"] not in {"skip", "replace"}:
            raise StoreError("invalid clock exception action")
        if data["action"] == "replace":
            replacement_id = data.get("replacement_version_id")
            replacement = (state.get("clock_template_versions", {}).get(str(UUID(replacement_id)))
                           if replacement_id else None)
            if replacement is None or replacement["data"]["status"] != "published":
                raise StoreError("clock exception replacement must be published")
            replacement_template = state["clock_templates"][str(UUID(replacement["data"]["template_id"]))]["data"]
            if replacement_template["station_id"] != assignment["station_id"]:
                raise StoreError("clock exception replacement belongs to another station")
        elif data.get("replacement_version_id") is not None:
            raise StoreError("skip clock exception cannot have replacement")
        _instant(data["changed_at"], "changed_at")
    if collection == "clock_materializations":
        assignment = state["clock_assignments"][str(UUID(data["assignment_id"]))]["data"]
        version = state["clock_template_versions"][str(UUID(data["template_version_id"]))]["data"]
        plan = state["plans"][str(UUID(data["plan_id"]))]["data"]
        revision = state["plan_revisions"][str(UUID(data["plan_revision_id"]))]["data"]
        entries = data["entry_ids"]
        fingerprints = data["entry_fingerprints"]
        if assignment["station_id"] != data["station_id"] or \
                assignment["local_hour"] != data["local_hour"] or \
                version["status"] != "published" or \
                plan["station_id"] != data["station_id"] or \
                plan["service_date"] != data["service_date"] or \
                plan["service_key"] != assignment["service_key"] or \
                revision["plan_id"] != data["plan_id"] or revision["status"] != "draft" or \
                not isinstance(entries, list) or not entries or \
                not isinstance(fingerprints, list) or len(entries) != len(fingerprints):
            raise StoreError("invalid clock materialization links")
        try:
            date.fromisoformat(data["service_date"])
        except (TypeError, ValueError) as exc:
            raise StoreError("invalid clock materialization date") from exc
        if not isinstance(data["source_fingerprint"], str) or \
                len(data["source_fingerprint"]) != 64:
            raise StoreError("invalid clock materialization source fingerprint")
        for entry_id, digest in zip(entries, fingerprints):
            entry = state.get("plan_entries", {}).get(str(UUID(entry_id)))
            if entry is None or entry["data"]["plan_revision_id"] != data["plan_revision_id"] or \
                    entry["data"].get("clock_assignment_id") != data["assignment_id"] or \
                    entry["data"].get("clock_source_fingerprint") != data["source_fingerprint"] or \
                    not isinstance(digest, str) or len(digest) != 64:
                raise StoreError("clock materialization entry provenance differs")
        _instant(data["created_at"], "created_at")
    if collection == "emergency_alerts":
        from .emergency_alerts import _CODE, _LANGUAGE, _NAMESPACE
        if not isinstance(data["source_namespace"], str) or \
                not _NAMESPACE.fullmatch(data["source_namespace"]) or \
                not isinstance(data["external_alert_key"], str) or \
                not _CODE.fullmatch(data["external_alert_key"]) or \
                data["purpose"] not in {"actual", "test"}:
            raise StoreError("invalid emergency alert identity or purpose")
        start, end = _instant(data["starts_at"], "starts_at"), _instant(data["ends_at"], "ends_at")
        if end <= start:
            raise StoreError("emergency alert interval is empty")
        for field in ("geo_codes", "output_ids"):
            values = data[field]
            if not isinstance(values, list) or not values or values != sorted(set(values)) or \
                    any(not isinstance(value, str) or not _CODE.fullmatch(value) for value in values):
                raise StoreError("invalid emergency alert scope codes")
        repeat = data["repeat_interval_seconds"]
        max_attempts = data.get("max_attempts")
        if isinstance(repeat, bool) or not isinstance(repeat, int) or not 0 <= repeat <= 86400 or \
                (max_attempts is not None and (isinstance(max_attempts, bool) or
                 not isinstance(max_attempts, int) or not 1 <= max_attempts <= 10000)):
            raise StoreError("invalid emergency alert repeat limits")
        if data.get("media_version_id"):
            version = state.get("versions", {}).get(str(UUID(data["media_version_id"])))
            if version is None or not any(
                    row["data"]["station_id"] == data["station_id"] and
                    row["data"]["item_id"] == version["data"]["item_id"]
                    for row in state.get("station_items", {}).values()):
                raise StoreError("emergency alert media is outside station")
        if not data.get("media_version_id") and not data.get("tts_text"):
            raise StoreError("emergency alert needs media or text")
        if data.get("tts_text"):
            if not isinstance(data["tts_text"], str) or not 0 < len(data["tts_text"]) <= 4000 or \
                    not isinstance(data.get("tts_language"), str) or \
                    not _LANGUAGE.fullmatch(data["tts_language"]):
                raise StoreError("invalid emergency alert text or language")
        elif data.get("tts_language") is not None:
            raise StoreError("emergency alert language needs text")
        _instant(data["received_at"], "received_at")
        if data["status"] == "cancelled":
            if not all(data.get(key) for key in ("cancelled_at", "cancelled_by", "cancel_reason")):
                raise StoreError("cancelled emergency alert needs provenance")
            _instant(data["cancelled_at"], "cancelled_at")
            if str(UUID(data["cancelled_by"])) not in state.get("users", {}):
                raise StoreError("emergency alert canceller is absent")
    if collection == "station_outputs":
        from .station_outputs import _CODE
        endpoint = state["endpoints"][str(UUID(data["endpoint_id"]))]["data"]
        geos = data["geo_codes"]
        if endpoint["station_id"] != data["station_id"] or \
                not isinstance(data["output_id"], str) or not _CODE.fullmatch(data["output_id"]) or \
                not isinstance(geos, list) or not geos or geos != sorted(set(geos)) or \
                any(not isinstance(code, str) or not _CODE.fullmatch(code) for code in geos):
            raise StoreError("invalid station output scope or geo codes")
    if collection == "emergency_alert_attempts":
        alert = state["emergency_alerts"][str(UUID(data["alert_id"]))]["data"]
        endpoint = state["endpoints"][str(UUID(data["endpoint_id"]))]["data"]
        station_output = state["station_outputs"][str(UUID(data["station_output_id"]))]["data"]
        if alert["station_id"] != data["station_id"] or \
                endpoint["station_id"] != data["station_id"] or \
                station_output["station_id"] != data["station_id"] or \
                station_output["endpoint_id"] != data["endpoint_id"] or \
                station_output["output_id"] != data["output_id"] or \
                isinstance(data["station_output_revision"], bool) or \
                not isinstance(data["station_output_revision"], int) or \
                data["station_output_revision"] < 1 or \
                data["purpose"] != alert["purpose"] or \
                data["output_id"] not in alert["output_ids"] or \
                data["geo_code"] not in alert["geo_codes"] or \
                data["state"] not in {"started", "completed", "failed", "interrupted"}:
            raise StoreError("emergency alert attempt scope or state differs")
        from .emergency_alerts import _CODE
        if not isinstance(data["attempt_key"], str) or not _CODE.fullmatch(data["attempt_key"]):
            raise StoreError("invalid emergency alert attempt key")
        started = _instant(data["started_at"], "started_at")
        if not (_instant(alert["starts_at"], "starts_at") <= started <
                _instant(alert["ends_at"], "ends_at")) or \
                (alert["status"] == "cancelled" and started >=
                 _instant(alert["cancelled_at"], "cancelled_at")):
            raise StoreError("emergency alert attempt started outside active window")
        if data["representation_kind"] == "audio_version":
            media_id = data.get("media_file_id")
            media = state.get("media_files", {}).get(str(UUID(media_id))) if media_id else None
            if data.get("version_id") != alert.get("media_version_id") or media is None or \
                    media["data"]["version_id"] != data["version_id"] or \
                    media["data"]["status"] != "verified":
                raise StoreError("emergency alert audio representation is invalid")
        elif data["representation_kind"] == "tts_text":
            if not alert.get("tts_text") or data.get("version_id") is not None or \
                    data.get("media_file_id") is not None:
                raise StoreError("emergency alert TTS representation is invalid")
        else:
            raise StoreError("unsupported emergency alert representation")
        if data["state"] != "started":
            ended = _instant(data["ended_at"], "ended_at")
            if ended < started or not data.get("finish_key") or not data.get("finished_by") or \
                    (data["state"] in {"failed", "interrupted"} and not data.get("finish_reason")):
                raise StoreError("terminal alert attempt lacks completion evidence")
            if str(UUID(data["finished_by"])) not in state.get("users", {}):
                raise StoreError("emergency alert finisher is absent")
    if collection == "emergency_alert_observation_links":
        alert = state["emergency_alerts"][str(UUID(data["alert_id"]))]["data"]
        attempt = state["emergency_alert_attempts"][str(UUID(data["attempt_id"]))]["data"]
        observation = state["output_observations"][str(UUID(data["observation_id"]))]["data"]
        if alert["station_id"] != data["station_id"] or \
                attempt["alert_id"] != data["alert_id"] or \
                attempt["endpoint_id"] != data["endpoint_id"] or \
                observation["station_id"] != data["station_id"] or \
                observation["endpoint_id"] != data["endpoint_id"] or \
                observation.get("details", {}).get("output_id") != attempt["output_id"] or \
                observation.get("event_id") is not None or \
                observation.get("attempt_id") is not None or \
                data["observation_kind"] != observation["kind"] or \
                data["observer_kind"] != observation.get("observer_kind"):
            raise StoreError("emergency alert observation link is not independent or scoped")
        observed = _instant(observation["occurred_at"], "occurred_at")
        start = _instant(attempt["started_at"], "started_at")
        end = (_instant(attempt["ended_at"], "ended_at")
               if attempt.get("ended_at") else datetime.now(timezone.utc))
        from datetime import timedelta
        if not start <= observed <= end + timedelta(seconds=30):
            raise StoreError("emergency alert observation is outside attempt window")
        _instant(data["linked_at"], "linked_at")
    if collection == "recording_jobs":
        from .recording_jobs import _end_trigger, _schedule, _source, _start_trigger
        endpoint = state["endpoints"][str(UUID(data["endpoint_id"]))]["data"]
        version = state["versions"][str(UUID(data["destination_version_id"]))]["data"]
        if endpoint["station_id"] != data["station_id"] or \
                not any(row["data"]["station_id"] == data["station_id"] and
                        row["data"]["item_id"] == version["item_id"]
                        for row in state.get("station_items", {}).values()):
            raise StoreError("recording job endpoint or destination is outside station")
        if not isinstance(data["code"], str) or not 0 < len(data["code"]) <= 80 or \
                not isinstance(data["name"], str) or not 0 < len(data["name"]) <= 200 or \
                type(data["allow_multiple"]) is not bool or type(data["one_shot"]) is not bool or \
                (data["allow_multiple"] and data["one_shot"]):
            raise StoreError("invalid recording job identity or mode")
        for field, normalizer in (("source", _source), ("schedule", _schedule),
                                  ("start_trigger", _start_trigger),
                                  ("end_trigger", _end_trigger)):
            if data[field] != normalizer(data[field]):
                raise StoreError(f"recording job {field} is not normalized")
        _instant(data["created_at"], "created_at")
    if collection == "recording_attempts":
        job = state["recording_jobs"][str(UUID(data["job_id"]))]["data"]
        if job["station_id"] != data["station_id"] or \
                job["endpoint_id"] != data["endpoint_id"] or \
                not isinstance(data["start_event_key"], str) or \
                not data["start_event_key"]:
            raise StoreError("recording attempt differs from job")
        try:
            date.fromisoformat(data["occurrence_date"])
        except (TypeError, ValueError) as exc:
            raise StoreError("invalid recording occurrence date") from exc
        start = _instant(data["actual_start_at"], "actual_start_at")
        deadline = _instant(data["deadline_at"], "deadline_at")
        _instant(data["created_at"], "created_at")
        if deadline <= start:
            raise StoreError("recording deadline precedes start")
        own_id = next((key for key, row in state.get("recording_attempts", {}).items()
                       if row["data"] is data), None)
        if own_id is None:
            raise StoreError("recording attempt identity is absent")
        matching_start = [row["data"] for row in state.get("recording_events", {}).values()
                          if row["data"]["attempt_id"] == own_id and
                          row["data"]["event_kind"] == "started" and
                          row["data"]["event_key"] == data["start_event_key"]]
        if len(matching_start) != 1:
            raise StoreError("recording attempt needs one matching start event")
        if data["status"] != "recording":
            ended = _instant(data["actual_end_at"], "actual_end_at")
            if ended <= start or not data.get("completion_event_key") or \
                    (data["status"] in {"partial", "failed"} and not data.get("reason_code")):
                raise StoreError("recording completion is incomplete")
            matching_end = [row["data"] for row in state.get("recording_events", {}).values()
                            if row["data"]["attempt_id"] == own_id and
                            row["data"]["event_kind"] == "finished" and
                            row["data"]["event_key"] == data["completion_event_key"] and
                            row["data"]["endpoint_id"] == data["endpoint_id"] and
                            row["data"]["outcome"] == data["status"] and
                            row["data"]["occurred_at"] == data["actual_end_at"]]
            if len(matching_end) != 1:
                raise StoreError("recording attempt needs matching finish event")
            if data["status"] == "completed":
                media_id = data.get("media_file_id")
                media = state.get("media_files", {}).get(str(UUID(media_id))) if media_id else None
                if media is None or media["data"]["version_id"] != job["destination_version_id"] or \
                        media["data"]["status"] != "verified" or \
                        media["data"].get("sha256") != data.get("media_sha256") or \
                        media["data"].get("size_bytes") != data.get("media_size_bytes"):
                    raise StoreError("completed recording needs matching verified media")
            if data["status"] == "failed" and data.get("media_file_id"):
                raise StoreError("failed recording cannot carry media")
    if collection == "recording_events":
        attempt = state["recording_attempts"][str(UUID(data["attempt_id"]))]["data"]
        if attempt["endpoint_id"] != data["endpoint_id"] or \
                data["event_kind"] not in {"started", "finished"} or \
                not isinstance(data["event_key"], str) or not data["event_key"] or \
                not isinstance(data["claim_fingerprint"], str) or \
                len(data["claim_fingerprint"]) != 64 or \
                any(char not in "0123456789abcdef" for char in data["claim_fingerprint"]):
            raise StoreError("invalid recording event identity")
        _instant(data["occurred_at"], "occurred_at")
        if data["event_kind"] == "started":
            if data["event_key"] != attempt["start_event_key"]:
                raise StoreError("recording start event key differs")
        elif data.get("outcome") != attempt["status"] or \
                data["event_key"] != attempt.get("completion_event_key"):
            raise StoreError("recording finish event differs from attempt")
    if collection in {"now_playing_states", "now_playing_decisions"}:
        from .station_outputs import _CODE
        endpoint = state["endpoints"][str(UUID(data["endpoint_id"]))]["data"]
        if endpoint["station_id"] != data["station_id"] or \
                not isinstance(data["output_id"], str) or not _CODE.fullmatch(data["output_id"]) or \
                not any(row["data"]["station_id"] == data["station_id"] and
                        row["data"]["endpoint_id"] == data["endpoint_id"] and
                        row["data"]["output_id"] == data["output_id"]
                        for row in state.get("station_outputs", {}).values()):
            raise StoreError("now-playing output is not configured in station")
    if collection == "now_playing_states":
        from uuid import NAMESPACE_URL, uuid5
        expected_id = str(uuid5(NAMESPACE_URL, f"sdbase:now-playing:{data['station_id']}:"
                                             f"{data['endpoint_id']}:{data['output_id']}"))
        row = state.get("now_playing_states", {}).get(expected_id)
        if row is None or row["data"] is not data:
            raise StoreError("now-playing state needs its deterministic scope identity")
        for field in ("on_air_watermark", "next_watermark", "talk_watermark"):
            if data.get(field):
                _instant(data[field], field)
        on_air = data.get("on_air")
        if on_air is not None:
            if not isinstance(on_air, dict) or not on_air.get("event_id") or \
                    not on_air.get("attempt_id") or not on_air.get("version_id"):
                raise StoreError("invalid now-playing on-air evidence")
            event = state.get("playout_events", {}).get(str(UUID(on_air["event_id"])))
            if event is None or event["data"]["station_id"] != data["station_id"] or \
                    event["data"]["endpoint_id"] != data["endpoint_id"] or \
                    event["data"]["attempt_id"] != on_air["attempt_id"] or \
                    event["data"]["version_id"] != on_air["version_id"] or \
                    event["data"]["phase"] not in {"start", "manual_start"}:
                raise StoreError("now-playing on-air source is not a matching start event")
            _instant(on_air["expires_at"], "expires_at")
        for field in ("next_intent", "talk_intent"):
            lane = data.get(field)
            if lane is not None:
                if not isinstance(lane, dict):
                    raise StoreError("invalid now-playing intent")
                _instant(lane["expires_at"], "expires_at")
        _instant(data["updated_at"], "updated_at")
    if collection == "now_playing_decisions":
        if data["source_kind"] not in {"playout_event", "next_intent", "talk_intent"} or \
                data["outcome"] not in {"applied", "ignored_stale", "ignored_unmatched",
                                        "ignored_expired"} or \
                not isinstance(data["decision_key"], str) or not data["decision_key"] or \
                not isinstance(data["reason"], str) or not data["reason"] or \
                not isinstance(data["details"], dict):
            raise StoreError("invalid now-playing projection decision")
        for field in ("event_at", "received_at"):
            _instant(data[field], field)
        digest = data["input_fingerprint"]
        if not isinstance(digest, str) or len(digest) != 64 or \
                any(char not in "0123456789abcdef" for char in digest):
            raise StoreError("invalid now-playing decision fingerprint")
        before = data["state_revision_before"]
        after = data["state_revision_after"]
        if isinstance(before, bool) or not isinstance(before, int) or before < 0 or \
                isinstance(after, bool) or not isinstance(after, int) or \
                after != before + int(data["outcome"] == "applied"):
            raise StoreError("now-playing state revision claim is invalid")
        if data["source_kind"] == "playout_event":
            event = state.get("playout_events", {}).get(str(UUID(data["source_id"])))
            if event is None or event["data"]["station_id"] != data["station_id"] or \
                    event["data"]["endpoint_id"] != data["endpoint_id"]:
                raise StoreError("now-playing event source differs from endpoint")
        elif not isinstance(data["source_id"], str) or not data["source_id"]:
            raise StoreError("now-playing intent source key is invalid")
    if collection == "asrun_report_runs":
        from .asrun_reports import _KEY, _OUTPUT, _hash
        if not isinstance(data["idempotency_key"], str) or \
                not _KEY.fullmatch(data["idempotency_key"]):
            raise StoreError("invalid as-run report key")
        try:
            first = date.fromisoformat(data["date_from"])
            last = date.fromisoformat(data["date_to"])
        except (TypeError, ValueError) as exc:
            raise StoreError("invalid as-run report date range") from exc
        if last < first or (last - first).days > 366:
            raise StoreError("as-run report date range is too large")
        if data.get("output_id") is not None and \
                (not isinstance(data["output_id"], str) or
                 not _OUTPUT.fullmatch(data["output_id"])):
            raise StoreError("invalid as-run report output")
        for field in ("as_of_at", "created_at"):
            _instant(data[field], field)
        if isinstance(data["source_sequence"], bool) or \
                not isinstance(data["source_sequence"], int) or data["source_sequence"] < 0 or \
                not isinstance(data["snapshot"], dict) or \
                data["snapshot"].get("station_id") != data["station_id"] or \
                data["snapshot"].get("date_from") != data["date_from"] or \
                data["snapshot"].get("date_to") != data["date_to"] or \
                data["snapshot"].get("output_id") != data.get("output_id") or \
                data["snapshot"].get("as_of_at") != data["as_of_at"]:
            raise StoreError("as-run report snapshot metadata differs")
        for field in ("source_hash", "snapshot_hash"):
            digest = data[field]
            if not isinstance(digest, str) or len(digest) != 64 or \
                    any(char not in "0123456789abcdef" for char in digest):
                raise StoreError("invalid as-run report hash")
        if _hash(data["snapshot"]) != data["snapshot_hash"]:
            raise StoreError("as-run report snapshot checksum mismatch")
    if collection == "live_overrides":
        from .live_overrides import _hash
        endpoint = state["endpoints"][str(UUID(data["endpoint_id"]))]["data"]
        plan = state["plans"][str(UUID(data["plan_id"]))]["data"]
        revision = state["plan_revisions"][str(UUID(data["plan_revision_id"]))]["data"]
        entry = state["plan_entries"][str(UUID(data["plan_entry_id"]))]["data"]
        if endpoint["station_id"] != data["station_id"] or \
                plan["station_id"] != data["station_id"] or \
                revision["plan_id"] != data["plan_id"] or \
                entry["plan_revision_id"] != data["plan_revision_id"]:
            raise StoreError("live override plan or endpoint scope differs")
        operation = data["operation"]
        if operation not in {"skip", "replace", "insert"} or \
                data["state"] not in {"pending", "accepted", "rejected", "expired",
                                      "execution_claimed", "failed", "evidenced"}:
            raise StoreError("invalid live override operation or state")
        if operation in {"skip", "replace"} and entry["event_kind"] != "audio":
            raise StoreError("live override anchor must be audio")
        if operation == "insert":
            if data.get("position") not in {"before", "after"}:
                raise StoreError("live insertion needs a position")
        elif data.get("position") is not None:
            raise StoreError("only a live insertion has a position")
        if not isinstance(data["command_key"], str) or not data["command_key"].strip() or \
                not isinstance(data["engine_instance"], str) or \
                not data["engine_instance"].strip() or \
                isinstance(data["engine_epoch"], bool) or \
                not isinstance(data["engine_epoch"], int) or data["engine_epoch"] < 0:
            raise StoreError("invalid live override command or engine identity")
        issued = _instant(data["issued_at"], "issued_at")
        expires = _instant(data["expires_at"], "expires_at")
        duration = (expires - issued).total_seconds()
        if duration != int(duration) or not 1 <= duration <= 300:
            raise StoreError("live override expires outside its TTL window")
        for field in ("request_hash", "snapshot_hash"):
            digest = data[field]
            if not isinstance(digest, str) or len(digest) != 64 or \
                    any(ch not in "0123456789abcdef" for ch in digest):
                raise StoreError("invalid live override fingerprint")
        intent = {"operation": operation, "plan_entry_id": data["plan_entry_id"],
                  "position": data.get("position"), "version_id": data.get("version_id"),
                  "ttl_seconds": int(duration)}
        if _hash(intent) != data["request_hash"]:
            raise StoreError("live override request fingerprint differs")
        if data["state"] == "pending":
            acceptance = state.get("active_acceptances", {}).get(data["endpoint_id"])
            accepted = acceptance["data"] if acceptance else None
            deliveries = [row["data"] for row in state.get("plan_distributions", {}).values()
                          if row["data"]["endpoint_id"] == data["endpoint_id"] and
                          row["data"]["revision_id"] == data["plan_revision_id"] and
                          row["data"]["status"] == "accepted"]
            if accepted is None or accepted.get("state", "active") != "active" or \
                    accepted["revision_id"] != data["plan_revision_id"] or \
                    accepted["engine_instance"] != data["engine_instance"] or \
                    accepted["engine_epoch"] != data["engine_epoch"] or \
                    len(deliveries) != 1 or \
                    deliveries[0]["payload_hash"] != data["snapshot_hash"]:
                raise StoreError("pending live override lacks accepted endpoint snapshot")
        for field, target in (("anchor_version_id", "versions"),
                              ("anchor_media_file_id", "media_files"),
                              ("version_id", "versions"),
                              ("media_file_id", "media_files"),
                              ("playout_attempt_id", "playout_attempts"),
                              ("skip_attempt_id", "playout_attempts"),
                              ("evidenced_by", "users")):
            value = data.get(field)
            if value is not None and str(UUID(value)) not in state.get(target, {}):
                raise StoreError(f"live override {field} is absent")
        if operation == "skip":
            if any(data.get(field) is not None for field in
                   ("version_id", "media_file_id", "media_sha256")):
                raise StoreError("live skip cannot select replacement audio")
        else:
            version_id, media_id = data.get("version_id"), data.get("media_file_id")
            media = state.get("media_files", {}).get(str(UUID(media_id))) if media_id else None
            if not version_id or media is None or media["data"]["version_id"] != version_id or \
                    media["data"].get("sha256") != data.get("media_sha256"):
                raise StoreError("live override selected audio differs from media")
        for field in ("acknowledged_at", "reported_at", "evidenced_at"):
            if data.get(field) is not None:
                _instant(data[field], field)
        if data["state"] == "evidenced":
            if operation in {"skip", "replace"} and not data.get("skip_attempt_id") or \
                    operation in {"insert", "replace"} and not data.get("playout_attempt_id") or \
                    not data.get("evidenced_at") or not data.get("evidenced_by"):
                raise StoreError("live override has incomplete execution evidence")
    if collection == "live_override_reports":
        from .live_overrides import _hash
        override = state["live_overrides"][str(UUID(data["override_id"]))]["data"]
        if override["endpoint_id"] != data["endpoint_id"] or \
                override["engine_instance"] != data["engine_instance"] or \
                override["engine_epoch"] != data["engine_epoch"]:
            raise StoreError("live override report comes from another engine")
        if not isinstance(data["event_key"], str) or not data["event_key"].strip() or \
                data["report_kind"] not in {"ack", "result"}:
            raise StoreError("invalid live override report identity")
        _instant(data["occurred_at"], "occurred_at")
        _instant(data["received_at"], "received_at")
        claim = {"override_id": data["override_id"],
                 "report_kind": data["report_kind"],
                 "event_key": data["event_key"],
                 "reason_code": data.get("reason_code"),
                 "occurred_at": data["occurred_at"],
                 "engine_instance": data["engine_instance"],
                 "engine_epoch": data["engine_epoch"]}
        if data["report_kind"] == "ack":
            if type(data.get("accepted")) is not bool or "outcome" in data:
                raise StoreError("invalid live override acknowledgement")
            claim["accepted"] = data["accepted"]
            if override["state"] != ("accepted" if data["accepted"] else "rejected"):
                raise StoreError("live override acknowledgement disagrees with state")
        else:
            if data.get("outcome") not in {"applied", "failed"} or "accepted" in data:
                raise StoreError("invalid live override result")
            claim["outcome"] = data["outcome"]
            if override["state"] != ("execution_claimed" if data["outcome"] == "applied"
                                     else "failed"):
                raise StoreError("live override result disagrees with state")
        if _hash(claim) != data["claim_fingerprint"]:
            raise StoreError("live override report fingerprint differs")
    if collection in {"plan_distributions", "active_acceptances", "engine_commands",
                      "endpoint_copies", "output_observations"}:
        endpoint = state["endpoints"][str(UUID(data["endpoint_id"]))]["data"]
        if data.get("station_id") and endpoint["station_id"] != data["station_id"]:
            raise StoreError(f"{collection} endpoint belongs to another station")
    if collection == "plan_distributions":
        revision = state["plan_revisions"][str(UUID(data["revision_id"]))]["data"]
        plan = state["plans"][str(UUID(revision["plan_id"]))]["data"]
        service = _active_service(state, plan["station_id"], plan["service_key"])
        if plan["station_id"] != data["station_id"] or \
                (plan["service_key"] != "default" and not service) or \
                (service and service["endpoint_id"] != data["endpoint_id"]):
            raise StoreError("plan distribution crosses broadcast service binding")
        if data["status"] not in {"pending", "sent", "accepted", "rejected", "expired"}:
            raise StoreError("invalid plan distribution status")
        if "offline_grant" in data:
            _validate_offline_grant(data, state)
    if collection == "engine_commands" and data["state"] not in {
        "pending", "accepted", "executed", "rejected", "expired"}:
        raise StoreError("invalid engine command state")
    if collection == "tokens":
        if data["scope"] not in ("user", "endpoint", "monitor"):
            raise StoreError("invalid token scope")
        _instant(data["expires_at"], "expires_at")
        if data["scope"] in {"endpoint", "monitor"}:
            endpoint_id = data.get("endpoint_id")
            if not endpoint_id or str(UUID(endpoint_id)) not in state.get("endpoints", {}):
                raise StoreError("endpoint or monitor token needs a real endpoint")
    if collection == "permissions":
        if data.get("revoked_at") is not None:
            _instant(data["revoked_at"], "revoked_at")
            if not data.get("revoked_by") or data["revoked_by"] not in state.get("users", {}):
                raise StoreError("permission revocation requires a real operator")
        if data["permission"] not in {"view", "edit", "bulk_edit", "import", "approve",
                                      "activate", "operate", "repair", "manage"}:
            raise StoreError("invalid permission")
        if data.get("station_id") and str(UUID(data["station_id"])) not in state.get("stations", {}):
            raise StoreError("permission station is absent")
        if data.get("station_scope") != (data.get("station_id") or "*"):
            raise StoreError("permission station_scope must match station_id")


def _guard_broadcast_service(before: dict | None, after: dict | None) -> None:
    if before is None or after is None:
        return
    if any(before.get(name) != after.get(name) for name in
           ("station_id", "service_key", "endpoint_id", "output_id")):
        raise StoreError("broadcast service binding is immutable")


def _guard_service_scope(before: dict | None, after: dict | None) -> None:
    if before is None or after is None:
        return
    if before.get("station_id") != after.get("station_id") or \
            before.get("service_key") != after.get("service_key"):
        raise StoreError("broadcast service provenance is immutable")


def _guard_version(before: dict | None, after: dict | None) -> None:
    if before is None:
        return
    if after is None:
        raise StoreError("versions cannot be deleted")
    if before["status"] in ("approved", "withdrawn"):
        allowed = dict(before)
        allowed["status"] = "withdrawn"
        if after != before and after != allowed:
            raise StoreError("approved versions are immutable; create a new version")


def _guard_plan_revision(before: dict | None, after: dict | None) -> None:
    if before and before["status"] in ("approved", "superseded"):
        if after is None or {k: v for k, v in after.items() if k != "status"} != \
                {k: v for k, v in before.items() if k != "status"} or \
                after["status"] not in (before["status"], "superseded"):
            raise StoreError("approved plan revision cannot be edited")


def _guard_plan_distribution(before: dict | None, after: dict | None) -> None:
    if after is None:
        raise StoreError("plan distributions cannot be deleted")
    if before is None:
        if after.get("status") != "pending" or "offline_grant" in after:
            raise StoreError("plan distribution must begin pending without an offline grant")
        return
    previous, following = before["status"], after.get("status")
    allowed = {"pending": {"sent", "expired", "rejected"},
               "sent": {"sent", "accepted", "expired", "rejected"},
               "accepted": {"accepted"}, "expired": {"expired"},
               "rejected": {"rejected"}}
    if following not in allowed.get(previous, set()):
        raise StoreError("invalid plan distribution transition")
    if before.get("offline_grant") is not None and \
            after.get("offline_grant") != before["offline_grant"]:
        raise StoreError("offline grant is immutable once issued")
    if previous in {"accepted", "expired", "rejected"}:
        if after != before:
            raise StoreError("final plan distribution is immutable")
        return
    if previous == following:
        if previous != "sent" or "offline_grant" in before or \
                "offline_grant" not in after or \
                {key: value for key, value in before.items() if key != "offline_grant"} != \
                {key: value for key, value in after.items() if key != "offline_grant"}:
            raise StoreError("sent distribution may only acquire its first offline grant")
        return
    mutable = {"status", "error_code"}
    if previous == "pending" and following == "sent":
        mutable.update(("sent_at", "acknowledged_at", "offline_grant"))
        if not after.get("sent_at") or not after.get("acknowledged_at"):
            raise StoreError("sent distribution needs delivery timestamps")
        _instant(after["sent_at"], "sent_at")
        _instant(after["acknowledged_at"], "acknowledged_at")
    elif previous == "sent" and following == "accepted":
        mutable.add("acknowledged_at")
        if not after.get("acknowledged_at"):
            raise StoreError("accepted distribution needs an acknowledgement time")
        _instant(after["acknowledged_at"], "acknowledged_at")
    if {key: value for key, value in before.items() if key not in mutable} != \
            {key: value for key, value in after.items() if key not in mutable}:
        raise StoreError("plan distribution snapshot and identity are immutable")


def _guard_media(before: dict | None, after: dict | None) -> None:
    if before and before["status"] == "verified" and after is not None:
        if any(after.get(key) != before.get(key) for key in ("version_id", "sha256", "size_bytes")):
            raise StoreError("verified media identity and bytes are immutable")


def _guard_rotation(before: dict | None, after: dict | None) -> None:
    if before and before["status"] == "active" and after is not None:
        if any(after.get(key) != before.get(key) for key in ("name", "config")):
            raise StoreError("disable an active rotation rule before editing it")


def _guard_cart_cut(before: dict | None, after: dict | None) -> None:
    if before and after is not None:
        if any(after.get(key) != before.get(key) for key in ("cart_id", "version_id")):
            raise StoreError("cart cut identity is immutable")


def _guard_profile(before: dict | None, after: dict | None) -> None:
    if before is None and after is not None and after.get("config_version") != 1:
        raise StoreError("new profile config_version must be one")
    if before and after is not None:
        if any(after.get(key) != before.get(key) for key in ("station_id", "owner_user_id")):
            raise StoreError("profile ownership is immutable")
        if after.get("config_version") != before.get("config_version", 0) + 1:
            raise StoreError("profile config_version must advance by one")


def _guard_schedule_segment(before: dict | None, after: dict | None) -> None:
    if before and after is not None:
        if any(after.get(key) != before.get(key) for key in (
                "series_id", "starts_on", "rule", "local_time", "entry_template", "dst_fold")):
            raise StoreError("schedule segment definition is immutable")
        prior_end, next_end = before.get("end_before"), after.get("end_before")
        if prior_end is not None and (next_end is None or next_end > prior_end):
            raise StoreError("schedule segment end may only move earlier")
        if before["status"] == "superseded" and after != before:
            raise StoreError("superseded schedule segment is immutable")


def _guard_radio_cue(before: dict | None, after: dict | None) -> None:
    if before is None or after is None:
        return
    if before["status"] == "withdrawn" and after != before:
        raise StoreError("withdrawn radio cue is immutable")
    if before["status"] == "approved":
        if after.get("status") == "approved" and after != before:
            raise StoreError("approved radio cue is immutable")
        if after.get("status") == "withdrawn":
            audit_keys = {"status", "withdrawn_at", "withdrawn_by", "withdrawn_reason"}
            if ({key: value for key, value in after.items() if key not in audit_keys} !=
                    {key: value for key, value in before.items() if key not in audit_keys}) or \
                    any(not after.get(key) for key in ("withdrawn_at", "withdrawn_by",
                                                       "withdrawn_reason")):
                raise StoreError("radio cue withdrawal must preserve approved data")
        elif after.get("status") != "approved":
            raise StoreError("approved radio cue may only be withdrawn")


def _guard_placement(before: dict | None, after: dict | None) -> None:
    if before and after is not None:
        if any(after.get(key) != before.get(key) for key in (
                "station_id", "transition_id", "plan_revision_id", "anchor_entry_id",
                "content_version_id", "kind", "start_reference", "offset_ms")):
            raise StoreError("production placement identity is immutable")
        if before["status"] == "approved":
            if after.get("status") == "approved" and after != before:
                raise StoreError("approved production placement is immutable")
            if after.get("status") in {"stale", "withdrawn"}:
                audit_keys = {"status", "stale_reason", "invalidated_at",
                              "withdrawn_reason", "withdrawn_at", "withdrawn_by"}
                if ({key: value for key, value in after.items() if key not in audit_keys} !=
                        {key: value for key, value in before.items() if key not in audit_keys}):
                    raise StoreError("placement status change must preserve approved data")
            elif after.get("status") != "approved":
                raise StoreError("approved placement may only become stale or withdrawn")
        if before["status"] == "stale" and after.get("status") == "draft":
            if after.get("reapproval_required") is not True or any(
                    after.get(key) for key in ("approved_at", "approved_by",
                                               "media_file_id", "media_sha256")):
                raise StoreError("stale placement needs explicit draft reapproval")


def _guard_transition(before: dict | None, after: dict | None) -> None:
    if before is None or after is None:
        return
    identity = ("station_id", "plan_revision_id", "from_entry_id", "to_entry_id",
                "manual_lock")
    if any(after.get(key) != before.get(key) for key in identity):
        raise StoreError("plan transition identity is immutable")
    if before["status"] == "withdrawn" and after != before:
        raise StoreError("withdrawn plan transition is immutable")
    if before["status"] == "draft":
        if after.get("status") == "draft" and after != before:
            raise StoreError("draft transition context must be rebound after invalidation")
        if after.get("status") == "stale":
            audit_keys = {"status", "stale_reason", "invalidated_at"}
            if ({key: value for key, value in after.items() if key not in audit_keys} !=
                    {key: value for key, value in before.items() if key not in audit_keys}):
                raise StoreError("transition invalidation must preserve context")
    if before["status"] == "stale" and after.get("status") == "draft":
        if any(not after.get(key) for key in ("rebind_reason", "rebound_by", "rebound_at")):
            raise StoreError("transition rebind needs explicit review provenance")


def _guard_controller_version(before: dict | None, after: dict | None) -> None:
    if before and after is not None and before["status"] in {"published", "superseded"}:
        if before["status"] == "superseded" and after != before:
            raise StoreError("superseded controller mapping is immutable")
        if before["status"] == "published":
            if after.get("status") == "published" and after != before:
                raise StoreError("published controller mapping is immutable")
            if after.get("status") == "superseded" and \
                    (after | {"status": "published"}) != before:
                raise StoreError("controller supersession may only change status")
            if after.get("status") not in {"published", "superseded"}:
                raise StoreError("published controller mapping cannot return to draft")


def _guard_listener_request(before: dict | None, after: dict | None) -> None:
    if after is None:
        return
    if before is None:
        if after.get("status") != "pending":
            raise StoreError("listener request must begin pending")
        return
    if any(after.get(key) != before.get(key) for key in (
            "station_id", "source_namespace", "external_request_key", "item_id",
            "requested_version_id", "requested_at", "requester_fingerprint",
            "created_by")):
        raise StoreError("listener request identity is immutable")
    allowed = {"pending": {"accepted", "rejected"}, "accepted": {"queued"},
               "queued": {"fulfilled"}, "rejected": set(), "fulfilled": set()}
    if after.get("status") not in allowed.get(before.get("status"), set()):
        raise StoreError("invalid listener request state transition")


def _guard_spot_change(before: dict | None, after: dict | None) -> None:
    if before is None or after is None:
        return
    identity = ("station_id", "campaign_id", "old_spot_id", "new_spot_id",
                "old_version_id", "new_version_id", "effective_at", "idempotency_key",
                "created_at", "created_by", "impact_count")
    if any(after.get(key) != before.get(key) for key in identity):
        raise StoreError("spot change identity is immutable")
    if before["status"] != "pending" or after.get("status") not in {"applied", "superseded"}:
        raise StoreError("spot change has an invalid state transition")


def _guard_spot_change_target(before: dict | None, after: dict | None) -> None:
    if before is None or after is None:
        return
    if before["status"] != "pending" or after.get("status") not in {"applied", "superseded"}:
        raise StoreError("spot change target is already final")
    mutable = {"status", "updated_at", "resolved_at", "resolution_reason",
               "replacement_placement_id", "replacement_plan_revision_id",
               "replacement_plan_entry_id", "replacement_distribution_id"}
    if {key: value for key, value in after.items() if key not in mutable} != \
            {key: value for key, value in before.items() if key not in mutable}:
        raise StoreError("spot change target provenance is immutable")


def _guard_clock_version(before: dict | None, after: dict | None) -> None:
    if before is None or after is None:
        return
    if any(after.get(key) != before.get(key) for key in
           ("template_id", "version_no", "created_by", "created_at", "based_on_id")):
        raise StoreError("clock version identity is immutable")
    if before["status"] != "draft" or after.get("status") != "published":
        raise StoreError("published clock version cannot be edited")


def _guard_emergency_alert(before: dict | None, after: dict | None) -> None:
    if before is None or after is None:
        return
    mutable = {"status", "cancelled_at", "cancelled_by", "cancel_reason"}
    if before["status"] != "active" or after.get("status") != "cancelled" or \
            {key: value for key, value in before.items() if key not in mutable} != \
            {key: value for key, value in after.items() if key not in mutable}:
        raise StoreError("emergency alert can only be cancelled with preserved identity")


def _guard_emergency_attempt(before: dict | None, after: dict | None) -> None:
    if before is None or after is None:
        return
    mutable = {"state", "ended_at", "finish_key", "finish_reason", "finished_by"}
    if before["state"] != "started" or after.get("state") not in {
            "completed", "failed", "interrupted"} or \
            {key: value for key, value in before.items() if key not in mutable} != \
            {key: value for key, value in after.items() if key not in mutable}:
        raise StoreError("emergency alert attempt may only acquire a terminal result")


def _guard_station_output(before: dict | None, after: dict | None) -> None:
    if before and after and any(after.get(key) != before.get(key) for key in
                                ("station_id", "endpoint_id", "output_id", "created_by")):
        raise StoreError("station output identity is immutable")


def _guard_recording_job(before: dict | None, after: dict | None) -> None:
    if before is None or after is None:
        return
    mutable = {"status", "status_reason", "status_changed_at", "status_changed_by"}
    if {key: value for key, value in before.items() if key not in mutable} != \
            {key: value for key, value in after.items() if key not in mutable}:
        raise StoreError("recording job definition is immutable; create a new job")


def _guard_recording_attempt(before: dict | None, after: dict | None) -> None:
    if before is None or after is None:
        return
    mutable = {"status", "actual_end_at", "completion_event_key", "media_file_id",
               "media_sha256", "media_size_bytes", "reason_code", "completed_by"}
    if before["status"] != "recording" or after.get("status") not in {
            "completed", "partial", "failed"} or \
            {key: value for key, value in before.items() if key not in mutable} != \
            {key: value for key, value in after.items() if key not in mutable}:
        raise StoreError("recording attempt may only acquire one terminal result")


def _guard_now_playing_state(before: dict | None, after: dict | None) -> None:
    if before is None or after is None:
        return
    if any(after.get(field) != before.get(field) for field in
           ("station_id", "endpoint_id", "output_id")):
        raise StoreError("now-playing output scope is immutable")
    for field in ("on_air_watermark", "next_watermark", "talk_watermark"):
        old = before.get(field)
        new = after.get(field)
        if old and (not new or _instant(new, field) < _instant(old, field)):
            raise StoreError("now-playing watermark cannot move backward")


def _guard_live_override(before: dict | None, after: dict | None) -> None:
    if after is None:
        return
    if before is None:
        if after.get("state") != "pending":
            raise StoreError("live override must begin pending")
        return
    transitions = {"pending": {"accepted", "rejected", "expired"},
                   "accepted": {"execution_claimed", "failed"},
                   "execution_claimed": {"evidenced"}}
    previous, following = before["state"], after.get("state")
    if following not in transitions.get(previous, set()):
        raise StoreError("invalid live override state transition")
    mutable = {"state", "error_code"}
    if previous == "pending":
        if following != "expired":
            mutable.add("acknowledged_at")
    elif previous == "accepted":
        mutable.add("reported_at")
    else:
        mutable.update(("playout_attempt_id", "skip_attempt_id", "evidenced_at",
                        "evidenced_by"))
    if {key: value for key, value in before.items() if key not in mutable} != \
            {key: value for key, value in after.items() if key not in mutable}:
        raise StoreError("live override provenance is immutable")
    if following in {"accepted", "rejected"} and not after.get("acknowledged_at") or \
            following in {"execution_claimed", "failed"} and not after.get("reported_at"):
        raise StoreError("live override transition lacks timestamp")


def _guard_makegood_request(before: dict | None, after: dict | None) -> None:
    if after is None:
        raise StoreError("makegood requests cannot be deleted")
    if before is None:
        if after.get("status") != "proposed":
            raise StoreError("makegood request must begin proposed")
        return
    transitions = {"proposed": {"approved", "rejected"},
                   "approved": {"reserved", "cancelled"}}
    previous, following = before["status"], after.get("status")
    if following not in transitions.get(previous, set()):
        raise StoreError("invalid makegood request transition")
    mutable = {"status"}
    if previous == "proposed":
        mutable.update(("approved_by", "approved_at", "reviewed_by", "reviewed_at",
                        "review_reason"))
    else:
        mutable.update(("placement_id", "reserved_by", "reserved_at",
                        "reserved_preview_hash", "cancelled_by", "cancelled_at",
                        "cancel_reason"))
    if {key: value for key, value in before.items() if key not in mutable} != \
            {key: value for key, value in after.items() if key not in mutable}:
        raise StoreError("makegood recommendation and identity are immutable")


def _guard_content_version(before: dict | None, after: dict | None) -> None:
    """Draft content may change; publication freezes its final revision."""
    if before is None or after is None:
        return
    if before["status"] != "draft" or after["status"] not in {"draft", "published"}:
        raise StoreError("published playlist or smart block version is immutable")
    for field in ("playlist_id", "block_id", "version_no", "based_on_id",
                  "created_by", "created_at"):
        if before.get(field) != after.get(field):
            raise StoreError("playlist or smart block version identity is immutable")
    if after["status"] == "published":
        mutable = {"status", "published_by", "published_at", "fingerprint",
                   "static_version_ids"}
        if {key: value for key, value in before.items() if key not in mutable} != \
                {key: value for key, value in after.items() if key not in mutable}:
            raise StoreError("playlist or smart block content changed during publication")


def _guard_content_definition(before: dict | None, after: dict | None) -> None:
    if before and after and any(before.get(field) != after.get(field) for field in
                                ("station_id", "code", "created_by", "created_at")):
        raise StoreError("playlist or smart block identity is immutable")


def _guard_playlist_entry(before: dict | None, after: dict | None) -> None:
    if before and after and any(before.get(field) != after.get(field) for field in
                                ("playlist_version_id", "created_by", "created_at")):
        raise StoreError("playlist entry version and creation identity are immutable")


def _guard_listener_mount(before: dict | None, after: dict | None) -> None:
    if before is None or after is None:
        return
    mutable = {"status", "stale_after_seconds", "updated_at", "updated_by"}
    if {key: value for key, value in before.items() if key not in mutable} != \
            {key: value for key, value in after.items() if key not in mutable}:
        raise StoreError("listener mount identity and source are immutable")


def _guard_podcast_feed(before: dict | None, after: dict | None) -> None:
    if before is None or after is None:
        return
    mutable = {"status", "name", "title", "description", "last_checked_at",
               "last_synced_at", "last_error", "updated_at", "updated_by",
               "auto_download_latest", "etag", "last_modified",
               "last_imported_at", "last_content_hash"}
    if {key: value for key, value in before.items() if key not in mutable} != \
            {key: value for key, value in after.items() if key not in mutable}:
        raise StoreError("podcast feed station and URL are immutable")


def _guard_podcast_episode(before: dict | None, after: dict | None) -> None:
    if before is None or after is None:
        return
    for field in ("station_id", "feed_id", "source_key", "first_seen_at"):
        if before.get(field) != after.get(field):
            raise StoreError("podcast episode source identity is immutable")


def open_radio_store(root: str | Path, *, read_only: bool = False,
                     lock_timeout: float | None = 30.0) -> Store:
    from .desktop_api import guard_document
    from .desktop_playlist import guard_entry
    from .desktop_media import guard_bound, guard_contribution
    validators = {name: (lambda data, state, collection=name: _validate(collection, data, state))
                  for name in REQUIRED}
    return Store(Path(root), indexes=INDEXES, validators=validators,
                 immutable_collections={"playout_events", "output_observations",
                                        "desktop_media_analysis",
                                        "desktop_mutations",
                                        "rotation_decisions", "diagnostic_events",
                                        "clock_materializations",
                                        "emergency_alert_observation_links",
                                        "recording_events", "now_playing_decisions",
                                        "asrun_report_runs", "live_override_reports",
                                        "listener_samples", "libretime_ids",
                                        "playback_marker_sets", "retention_segments"},
                 transition_guards={"broadcast_services": _guard_broadcast_service,
                                    "contributions": guard_contribution,
                                    "desktop_media_library": guard_bound,
                                    "desktop_media_rhythm": guard_bound,
                                    "desktop_media_markers": guard_bound,
                                    "desktop_configurations": guard_document,
                                    "desktop_playlist_entries": guard_entry,
                                    "clock_assignments": _guard_service_scope,
                                    "schedule_series": _guard_service_scope,
                                    "versions": _guard_version,
                                    "plan_revisions": _guard_plan_revision,
                                    "plan_distributions": _guard_plan_distribution,
                                    "media_files": _guard_media,
                                    "rotation_rules": _guard_rotation,
                                    "cart_cuts": _guard_cart_cut,
                                    "import_profiles": _guard_profile,
                                    "view_profiles": _guard_profile,
                                    "schedule_segments": _guard_schedule_segment,
                                    "radio_cues": _guard_radio_cue,
                                    "production_placements": _guard_placement,
                                    "plan_transitions": _guard_transition,
                                    "controller_profile_versions": _guard_controller_version,
                                    "listener_requests": _guard_listener_request,
                                    "spot_changes": _guard_spot_change,
                                    "spot_change_targets": _guard_spot_change_target,
                                    "clock_template_versions": _guard_clock_version,
                                    "emergency_alerts": _guard_emergency_alert,
                                    "emergency_alert_attempts": _guard_emergency_attempt,
                                    "station_outputs": _guard_station_output,
                                    "recording_jobs": _guard_recording_job,
                                    "recording_attempts": _guard_recording_attempt,
                                    "now_playing_states": _guard_now_playing_state,
                                    "live_overrides": _guard_live_override,
                                    "makegood_requests": _guard_makegood_request,
                                    "playlist_versions": _guard_content_version,
                                    "smart_block_versions": _guard_content_version,
                                    "playlists": _guard_content_definition,
                                    "smart_blocks": _guard_content_definition,
                                    "playlist_entries": _guard_playlist_entry,
                                    "listener_mounts": _guard_listener_mount,
                                    "podcast_feeds": _guard_podcast_feed,
                                    "podcast_episodes": _guard_podcast_episode},
                 non_deletable_collections=set(REQUIRED) - {"metadata_values", "contributions"},
                 schema_version=SCHEMA_VERSION, read_only=read_only,
                 lock_timeout=lock_timeout)
