"""Bounded podcast feed intake and reviewed episode media for SDBASE.

Feeds and episodes are ordinary `.sdb` records. Network retrieval connects to
vetted public numeric addresses, so a DNS change cannot redirect an approved
request to a private host. Downloads enter the existing draft catalog/media
workflow; this module never marks external audio approved or playable.
"""

from __future__ import annotations

from datetime import datetime, timedelta, timezone
from email.utils import parsedate_to_datetime
import hashlib
import http.client
import ipaddress
import json
import os
from pathlib import Path
import re
import socket
import ssl
import tempfile
import time
from typing import BinaryIO
from urllib.parse import quote, urljoin, urlsplit, urlunsplit
from uuid import NAMESPACE_URL, UUID, uuid4, uuid5
import xml.etree.ElementTree as ET

from .identity import Principal, require
from .storage import Conflict, Store, StoreError


MAX_FEED_BYTES = 2 * 1024 * 1024
MAX_AUDIO_BYTES = 256 * 1024 * 1024
MAX_EPISODES = 500
_XML_DECLARATION = re.compile(rb"<!\s*(?:DOCTYPE|ENTITY)\b", re.IGNORECASE)
_HOST = re.compile(r"[a-z0-9](?:[a-z0-9.-]*[a-z0-9])?\Z")


def _id(value: str, field: str) -> str:
    try:
        return str(UUID(str(value)))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError(f"{field} must be a UUID") from exc


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


def _body(row: dict) -> dict:
    return {key: value for key, value in row.items() if not key.startswith("_")}


def _bounded_text(value: str | None, field: str, limit: int,
                  *, required: bool = False) -> str | None:
    if value is None:
        if required:
            raise StoreError(f"{field} is required")
        return None
    if not isinstance(value, str):
        raise StoreError(f"{field} must be text")
    value = " ".join(value.split())
    if required and not value:
        raise StoreError(f"{field} is required")
    if len(value) > limit:
        value = value[:limit]
    return value or None


def _guid(value: str | None) -> str | None:
    if value is None:
        return None
    if not isinstance(value, str) or not value.strip() or len(value) > 512 or \
            any(ord(char) < 32 or ord(char) == 127 for char in value):
        raise StoreError("episode GUID must be 1..512 visible characters")
    return value.strip()


def _canonical_url(value: str) -> str:
    if (not isinstance(value, str) or not 1 <= len(value) <= 2048 or
            any(ord(c) <= 32 or ord(c) == 127 for c in value)):
        raise StoreError("podcast URL must be 1..2048 visible characters")
    try:
        parts = urlsplit(value)
        scheme = parts.scheme.lower()
        if scheme not in {"http", "https"}:
            raise ValueError
        if parts.username is not None or parts.password is not None:
            raise ValueError
        hostname = parts.hostname
        port = parts.port
        standard_port = 443 if scheme == "https" else 80
        if not hostname or port not in (None, standard_port):
            raise ValueError
        try:
            address = ipaddress.ip_address(hostname)
        except ValueError:
            host = hostname.encode("idna").decode("ascii").lower()
            if not _HOST.fullmatch(host) or ".." in host or host.endswith(
                    (".localhost", ".local", ".internal", ".test", ".invalid")) or "." not in host:
                raise ValueError
        else:
            if not address.is_global:
                raise ValueError
            host = address.compressed
        netloc = f"[{host}]" if ":" in host else host
        path = quote(parts.path or "/", safe="/:@!$&'()*+,;=%")
        query = quote(parts.query, safe="/?:@!$&'()*+,;=%")
        return urlunsplit((scheme, netloc, path, query, ""))
    except (ValueError, UnicodeError) as exc:
        raise StoreError("podcast URL must use HTTP or HTTPS, its standard port and no credentials") from exc


def _public_addresses(hostname: str, port: int) -> tuple[str, ...]:
    try:
        results = socket.getaddrinfo(hostname, port, type=socket.SOCK_STREAM)
    except OSError as exc:
        raise StoreError("podcast host cannot be resolved") from exc
    addresses = tuple(dict.fromkeys(result[4][0] for result in results))
    if not addresses or any(not ipaddress.ip_address(addr).is_global for addr in addresses):
        raise StoreError("podcast host does not resolve exclusively to public addresses")
    return addresses


class _PinnedHTTP(http.client.HTTPConnection):
    def __init__(self, host: str, address: str, *, timeout: float):
        super().__init__(host, port=80, timeout=timeout)
        self._address = address

    def connect(self) -> None:
        self.sock = socket.create_connection((self._address, 80), self.timeout)


class _PinnedHTTPS(http.client.HTTPSConnection):
    def __init__(self, host: str, address: str, *, timeout: float):
        super().__init__(host, port=443, timeout=timeout,
                         context=ssl.create_default_context())
        self._address = address

    def connect(self) -> None:
        raw = socket.create_connection((self._address, 443), self.timeout)
        try:
            self.sock = self._context.wrap_socket(raw, server_hostname=self.host)
        except BaseException:
            raw.close()
            raise


def _clean_header(value: str | None) -> str | None:
    if value is None:
        return None
    if not isinstance(value, str) or len(value) > 512 or any(ord(c) < 32 or ord(c) == 127 for c in value):
        return None
    return value


def _fetch(url: str, *, max_bytes: int, destination: BinaryIO | None = None,
           etag: str | None = None, last_modified: str | None = None) -> dict:
    """Fetch HTTP(S) feeds or enclosures with public-IP pinning and bounds."""
    current = _canonical_url(url)
    deadline = time.monotonic() + (90 if destination is not None else 25)
    for redirect_count in range(4):
        parts = urlsplit(current)
        host = parts.hostname
        port = 443 if parts.scheme == "https" else 80
        addresses = _public_addresses(host, port)
        last_exc = None
        connection = None
        for address in addresses:
            try:
                timeout = max(0.1, min(8.0, deadline - time.monotonic()))
                connection_type = _PinnedHTTPS if parts.scheme == "https" else _PinnedHTTP
                connection = connection_type(host, address, timeout=timeout)
                target = parts.path + (("?" + parts.query) if parts.query else "")
                headers = {"Accept-Encoding": "identity", "User-Agent": "SDBASE-Podcast/1",
                           "Accept": "application/rss+xml, application/atom+xml, application/xml, audio/*, */*"}
                if _clean_header(etag):
                    headers["If-None-Match"] = etag
                if _clean_header(last_modified):
                    headers["If-Modified-Since"] = last_modified
                connection.request("GET", target, headers=headers)
                response = connection.getresponse()
                break
            except (OSError, ssl.SSLError, http.client.HTTPException) as exc:
                last_exc = exc
                if connection is not None:
                    connection.close()
                connection = None
        if connection is None:
            raise StoreError("podcast fetch failed") from last_exc
        try:
            if response.status in (301, 302, 303, 307, 308):
                location = response.getheader("Location")
                if not location or redirect_count == 3:
                    raise StoreError("podcast redirect is invalid or exceeds limit")
                current = _canonical_url(urljoin(current, location))
                continue
            if response.status == 304 and destination is None:
                return {"not_modified": True, "url": current,
                        "etag": _clean_header(response.getheader("ETag")),
                        "last_modified": _clean_header(response.getheader("Last-Modified"))}
            if response.status != 200:
                raise StoreError(f"podcast HTTP status {response.status}")
            encoding = response.getheader("Content-Encoding", "identity").lower()
            if encoding not in ("", "identity"):
                raise StoreError("compressed podcast transfer is unsupported")
            declared = response.getheader("Content-Length")
            if declared is not None:
                try:
                    if int(declared) < 0 or int(declared) > max_bytes:
                        raise StoreError("podcast response exceeds byte limit")
                except ValueError as exc:
                    raise StoreError("podcast Content-Length is invalid") from exc
            sha = hashlib.sha256()
            size = 0
            chunks = [] if destination is None else None
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise StoreError("podcast fetch exceeded time limit")
                if connection.sock is not None:
                    connection.sock.settimeout(min(8.0, remaining))
                chunk = response.read(min(64 * 1024, max_bytes + 1 - size))
                if not chunk:
                    break
                size += len(chunk)
                if size > max_bytes:
                    raise StoreError("podcast response exceeds byte limit")
                sha.update(chunk)
                if destination is None:
                    chunks.append(chunk)
                else:
                    destination.write(chunk)
            if not size:
                raise StoreError("podcast response is empty")
            return {"not_modified": False, "url": current, "size_bytes": size,
                    "sha256": sha.hexdigest(),
                    "content_type": response.getheader("Content-Type", "").split(";", 1)[0].strip().lower(),
                    "etag": _clean_header(response.getheader("ETag")),
                    "last_modified": _clean_header(response.getheader("Last-Modified")),
                    "content": b"".join(chunks) if chunks is not None else None}
        except (OSError, ssl.SSLError, http.client.HTTPException) as exc:
            raise StoreError("podcast response failed during transfer") from exc
        finally:
            connection.close()
    raise StoreError("podcast redirect is invalid")


def _local(tag: str) -> str:
    return tag.rsplit("}", 1)[-1].lower()


def _child_text(node: ET.Element, name: str) -> str | None:
    for child in node:
        if _local(child.tag) == name:
            return "".join(child.itertext()).strip()
    return None


def _published(value: str | None) -> str | None:
    if not value:
        return None
    try:
        parsed = datetime.fromisoformat(value.replace("Z", "+00:00"))
    except ValueError:
        try:
            parsed = parsedate_to_datetime(value)
        except (ValueError, TypeError, IndexError, OverflowError):
            return None
    if parsed.tzinfo is None or parsed.utcoffset() is None:
        return None
    return parsed.astimezone(timezone.utc).isoformat()


def _duration(value: str | None) -> int | None:
    if not value:
        return None
    try:
        pieces = value.split(":")
        if len(pieces) == 1:
            seconds = int(pieces[0])
        elif len(pieces) == 2:
            seconds = int(pieces[0]) * 60 + int(pieces[1])
        elif len(pieces) == 3:
            seconds = int(pieces[0]) * 3600 + int(pieces[1]) * 60 + int(pieces[2])
        else:
            return None
    except ValueError:
        return None
    return seconds * 1000 if 0 < seconds <= 24 * 3600 else None


class _FeedTreeBuilder(ET.TreeBuilder):
    def doctype(self, name: str, pubid: str | None, system: str | None) -> None:
        # The byte scan below gives an early error for UTF-8 feeds. The parser
        # callback also covers XML encodings such as UTF-16 and UTF-32.
        raise StoreError("podcast feed cannot contain DTD or entity declarations")


def _parse_feed(content: bytes) -> dict:
    if not isinstance(content, bytes) or not content or len(content) > MAX_FEED_BYTES:
        raise StoreError("podcast feed must be nonempty XML within the byte limit")
    if _XML_DECLARATION.search(content):
        raise StoreError("podcast feed cannot contain DTD or entity declarations")
    try:
        root = ET.fromstring(content, parser=ET.XMLParser(target=_FeedTreeBuilder()))
    except ET.ParseError as exc:
        raise StoreError("podcast feed XML is invalid") from exc
    if sum(1 for _ in root.iter()) > 30_000:
        raise StoreError("podcast feed has too many XML nodes")
    root_kind = _local(root.tag)
    if root_kind == "rss":
        channel = next((child for child in root if _local(child.tag) == "channel"), None)
        if channel is None:
            raise StoreError("RSS feed lacks channel")
        nodes = [child for child in channel if _local(child.tag) == "item"]
    elif root_kind == "feed":
        channel = root
        nodes = [child for child in channel if _local(child.tag) == "entry"]
    else:
        raise StoreError("only RSS 2.0 and Atom feeds are supported")
    if len(nodes) > MAX_EPISODES:
        raise StoreError("podcast feed contains more episodes than the configured limit")
    title = _bounded_text(_child_text(channel, "title"), "feed title", 300, required=True)
    description = _bounded_text(_child_text(channel, "description") or
                                _child_text(channel, "subtitle"), "feed description", 1000)
    episodes = []
    skipped_count = 0
    seen_guid: dict[str, str] = {}
    seen_url: dict[str, str | None] = {}
    for node in nodes:
        guid = _guid(_child_text(node, "guid") or _child_text(node, "id"))
        enclosure = None
        if root_kind == "rss":
            enclosure = next((child for child in node if _local(child.tag) == "enclosure"), None)
        else:
            enclosure = next((child for child in node if _local(child.tag) == "link" and
                              child.get("rel") == "enclosure"), None)
        if enclosure is None or not enclosure.get("url", enclosure.get("href")):
            skipped_count += 1
            continue
        enclosure_url = _canonical_url(enclosure.get("url", enclosure.get("href")))
        episode_title = _bounded_text(_child_text(node, "title"), "episode title", 300,
                                      required=True)
        if guid and guid in seen_guid and seen_guid[guid] != enclosure_url:
            raise StoreError("podcast feed reuses a GUID for different enclosures")
        if enclosure_url in seen_url and seen_url[enclosure_url] != guid:
            raise StoreError("podcast feed reuses an enclosure with different GUIDs")
        if enclosure_url in seen_url:
            continue
        if guid:
            seen_guid[guid] = enclosure_url
        seen_url[enclosure_url] = guid
        mime = _bounded_text(enclosure.get("type"), "enclosure type", 100)
        length = enclosure.get("length")
        try:
            length_bytes = int(length) if length is not None else None
        except ValueError:
            length_bytes = None
        if length_bytes is not None and (length_bytes < 0 or length_bytes > 10**12):
            length_bytes = None
        metadata = {"guid": guid, "title": episode_title,
                    "enclosure_url": enclosure_url,
                    "published_at": _published(_child_text(node, "pubdate") or
                                               _child_text(node, "published") or
                                               _child_text(node, "updated")),
                    "description": _bounded_text(_child_text(node, "description") or
                                                 _child_text(node, "summary"),
                                                 "episode description", 2000),
                    "duration_ms": _duration(_child_text(node, "duration")),
                    "enclosure_type": mime, "enclosure_length_bytes": length_bytes}
        metadata["content_hash"] = hashlib.sha256(json.dumps(
            metadata, ensure_ascii=False, sort_keys=True,
            separators=(",", ":")).encode("utf-8")).hexdigest()
        episodes.append(metadata)
    return {"title": title, "description": description,
            "episodes": episodes, "skipped_count": skipped_count}


def _provided_feed(content: bytes, url: str) -> dict:
    if not isinstance(content, bytes) or not content or len(content) > MAX_FEED_BYTES:
        raise StoreError("podcast feed must be nonempty XML within the byte limit")
    return {"content": content, "sha256": hashlib.sha256(content).hexdigest(),
            "url": url, "etag": None, "last_modified": None,
            "not_modified": False}


def _feed(store: Store, station_id: str, feed_id: str) -> dict:
    row = store.get("podcast_feeds", _id(feed_id, "feed_id"))
    if row is None or row["station_id"] != station_id:
        raise Conflict("podcast feed is not in station")
    return row


def _episode(store: Store, station_id: str, episode_id: str) -> dict:
    row = store.get("podcast_episodes", _id(episode_id, "episode_id"))
    if row is None or row["station_id"] != station_id:
        raise Conflict("podcast episode is not in station")
    return row


def register_feed(store: Store, principal: Principal, station_id: str, feed_url: str) -> dict:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "import", station_id)
    url = _canonical_url(feed_url)
    existing = store.lookup("podcast_feeds", "station_url", station_id, url)
    if existing:
        return existing[0] | {"replayed": True}
    identifier = str(uuid4())
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("podcast_feeds", identifier,
           {"station_id": station_id, "feed_url": url, "status": "active",
            "created_at": _now(), "created_by": principal.user_id,
            "title": None, "description": None, "etag": None,
            "last_modified": None, "last_checked_at": None,
            "last_imported_at": None, "last_content_hash": None,
            "auto_download_latest": False})
    try:
        tx.commit()
    except Conflict:
        winner = store.lookup("podcast_feeds", "station_url", station_id, url)
        if winner:
            return winner[0] | {"replayed": True}
        raise
    return store.get("podcast_feeds", identifier) | {"replayed": False}


def list_feeds(store: Store, principal: Principal, station_id: str, *,
                include_disabled: bool = False) -> list[dict]:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "view", station_id)
    if type(include_disabled) is not bool:
        raise StoreError("include_disabled must be boolean")
    rows = [row for row in store.lookup("podcast_feeds", "station", station_id)
            if include_disabled or row["status"] == "active"]
    rows.sort(key=lambda row: (row["feed_url"], row["_id"]))
    if store.sequence != sequence:
        raise Conflict("podcast feeds changed during listing")
    return rows


def set_feed_status(store: Store, principal: Principal, station_id: str,
                    feed_id: str, *, status: str, expected_revision: int) -> dict:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "import", station_id)
    if not isinstance(status, str) or status not in {"active", "disabled"}:
        raise StoreError("podcast feed status must be active or disabled")
    feed = _feed(store, station_id, feed_id)
    if feed["_rev"] != expected_revision:
        raise Conflict("podcast feed changed")
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("podcast_feeds", feed["_id"], _body(feed) | {"status": status},
           expected_revision=feed["_rev"])
    tx.commit()
    return store.get("podcast_feeds", feed["_id"])


def set_feed_auto_download(store: Store, principal: Principal, station_id: str,
                           feed_id: str, *, auto_download_latest: bool,
                           expected_revision: int) -> dict:
    """Set the reviewed feed's intake policy; downloaded media stays a draft."""
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "import", station_id)
    require(store, principal, "edit", station_id)
    if type(auto_download_latest) is not bool:
        raise StoreError("auto_download_latest must be boolean")
    feed = _feed(store, station_id, feed_id)
    if type(expected_revision) is not int or feed["_rev"] != expected_revision:
        raise Conflict("podcast feed changed")
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("podcast_feeds", feed["_id"], _body(feed) | {
        "auto_download_latest": auto_download_latest,
        "updated_by": principal.user_id, "updated_at": _now()},
        expected_revision=feed["_rev"])
    tx.commit()
    return store.get("podcast_feeds", feed["_id"])


def preview_feed(store: Store, principal: Principal, station_id: str,
                 feed_id: str, *, xml_bytes: bytes | None = None) -> dict:
    station_id = _id(station_id, "station_id")
    require(store, principal, "import", station_id)
    feed = _feed(store, station_id, feed_id)
    if feed["status"] != "active":
        raise Conflict("podcast feed is disabled")
    result = (_provided_feed(xml_bytes, feed["feed_url"])
              if xml_bytes is not None else _fetch(feed["feed_url"], max_bytes=MAX_FEED_BYTES,
                                                    etag=feed.get("etag"),
                                                    last_modified=feed.get("last_modified")))
    if result["not_modified"]:
        return {"feed_id": feed["_id"], "not_modified": True,
                "content_sha256": feed.get("last_content_hash"), "episodes": []}
    parsed = _parse_feed(result["content"])
    episodes = []
    for data in parsed["episodes"]:
        match = _find_existing_episode(store, feed["_id"], data)
        episodes.append(data | {"episode_id": match["_id"] if match else None,
                                "change": ("new" if match is None else
                                           "unchanged" if match["content_hash"] == data["content_hash"]
                                           else "updated")})
    return {"feed_id": feed["_id"], "not_modified": False,
            "content_sha256": result["sha256"], "title": parsed["title"],
            "description": parsed["description"], "episodes": episodes,
            "skipped_count": parsed["skipped_count"],
            "new_count": sum(row["change"] == "new" for row in episodes),
            "updated_count": sum(row["change"] == "updated" for row in episodes)}


def _xml_text_bytes(xml_text: str) -> bytes:
    if not isinstance(xml_text, str):
        raise StoreError("podcast xml_text must be a UTF-8 string")
    try:
        content = xml_text.encode("utf-8")
    except UnicodeEncodeError as exc:
        raise StoreError("podcast xml_text contains invalid Unicode") from exc
    if not content or len(content) > MAX_FEED_BYTES:
        raise StoreError("podcast xml_text exceeds the feed byte limit")
    return content


def preview_feed_text(store: Store, principal: Principal, station_id: str,
                      feed_id: str, xml_text: str) -> dict:
    """Preview caller-supplied RSS/Atom text using the same bounded parser."""
    return preview_feed(store, principal, station_id, feed_id,
                        xml_bytes=_xml_text_bytes(xml_text))


def preview_feed_url(store: Store, principal: Principal, station_id: str,
                     feed_id: str) -> dict:
    """Fetch and preview the registered remote HTTP(S) feed."""
    return preview_feed(store, principal, station_id, feed_id)


def _find_existing_episode(store: Store, feed_id: str, data: dict) -> dict | None:
    matches = []
    if data["guid"]:
        matches.extend(store.lookup("podcast_episodes", "feed_guid", feed_id, data["guid"]))
    matches.extend(store.lookup("podcast_episodes", "feed_enclosure",
                                feed_id, data["enclosure_url"]))
    unique = {row["_id"]: row for row in matches}
    if len(unique) > 1:
        raise Conflict("podcast GUID and enclosure identify different episodes")
    return next(iter(unique.values()), None)


def commit_feed(store: Store, principal: Principal, station_id: str, feed_id: str,
                *, xml_bytes: bytes | None = None,
                expected_content_sha256: str | None = None) -> dict:
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "import", station_id)
    feed = _feed(store, station_id, feed_id)
    if feed["status"] != "active":
        raise Conflict("podcast feed is disabled")
    result = (_provided_feed(xml_bytes, feed["feed_url"])
              if xml_bytes is not None else _fetch(feed["feed_url"], max_bytes=MAX_FEED_BYTES,
                                                    etag=feed.get("etag"),
                                                    last_modified=feed.get("last_modified")))
    if result["not_modified"]:
        if expected_content_sha256 is not None and \
                feed.get("last_content_hash") != expected_content_sha256:
            raise Conflict("podcast feed changed since preview")
        return {"feed_id": feed["_id"], "not_modified": True,
                "new_count": 0, "updated_count": 0, "unchanged_count": 0}
    if expected_content_sha256 is not None and result["sha256"] != expected_content_sha256:
        raise Conflict("podcast feed changed since preview")
    parsed = _parse_feed(result["content"])
    now = _now()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    counts = {"new": 0, "updated": 0, "unchanged": 0}
    for data in parsed["episodes"]:
        existing = _find_existing_episode(store, feed["_id"], data)
        if existing is None:
            identifier = str(uuid4())
            source = data["guid"] if data["guid"] else data["enclosure_url"]
            source_key = ("guid:" if data["guid"] else "url:") + hashlib.sha256(
                source.encode("utf-8")).hexdigest()
            body = {"station_id": station_id, "feed_id": feed["_id"],
                    "source_key": source_key, **data,
                    "published_at": data["published_at"] or now,
                    "published_at_source": "feed" if data["published_at"] else "first_seen",
                    "first_seen_at": now, "last_seen_at": now,
                    "item_id": None, "version_id": None, "media_file_id": None,
                    "download_state": "none", "download_sha256": None,
                    "download_claim": None, "download_error": None}
            tx.put("podcast_episodes", identifier, body)
            counts["new"] += 1
        elif existing["content_hash"] != data["content_hash"]:
            body = _body(existing) | data | {"last_seen_at": now}
            if data["published_at"] is None:
                body["published_at"] = existing["published_at"]
            else:
                body["published_at_source"] = "feed"
            # Keep the original source identity and already reviewed catalog
            # records; a changed enclosure requires explicit re-download.
            if existing["enclosure_url"] != data["enclosure_url"]:
                body.update(download_state="stale", download_error="enclosure_changed")
            tx.put("podcast_episodes", existing["_id"], body,
                   expected_revision=existing["_rev"])
            counts["updated"] += 1
        else:
            counts["unchanged"] += 1
    feed_body = _body(feed) | {"title": parsed["title"],
                              "description": parsed["description"],
                              "etag": result.get("etag"),
                              "last_modified": result.get("last_modified"),
                              "last_checked_at": now, "last_imported_at": now,
                              "last_content_hash": result["sha256"]}
    tx.put("podcast_feeds", feed["_id"], feed_body, expected_revision=feed["_rev"])
    tx.commit()
    return {"feed_id": feed["_id"], "not_modified": False,
            "content_sha256": result["sha256"],
            "skipped_count": parsed["skipped_count"],
            "new_count": counts["new"], "updated_count": counts["updated"],
            "unchanged_count": counts["unchanged"]}


def commit_feed_text(store: Store, principal: Principal, station_id: str,
                     feed_id: str, xml_text: str,
                     *, expected_content_sha256: str | None = None) -> dict:
    """Import caller-supplied RSS/Atom text with preview-hash protection."""
    return commit_feed(store, principal, station_id, feed_id,
                       xml_bytes=_xml_text_bytes(xml_text),
                       expected_content_sha256=expected_content_sha256)


def commit_feed_url(store: Store, principal: Principal, station_id: str,
                    feed_id: str, *,
                    expected_content_sha256: str | None = None) -> dict:
    """Fetch and import the registered remote HTTP(S) feed."""
    return commit_feed(store, principal, station_id, feed_id,
                       expected_content_sha256=expected_content_sha256)


def sync_feed(store: Store, principal: Principal, station_id: str,
              feed_id: str, *, expected_content_sha256: str | None = None) -> dict:
    """Run one explicit bounded HTTP(S) sync and optionally stage the latest audio.

    A caller or external scheduler chooses when to call this operation. The
    policy enables intake only; technical review and approval remain separate.
    """
    station_id = _id(station_id, "station_id")
    require(store, principal, "import", station_id)
    feed = _feed(store, station_id, feed_id)
    if feed["status"] != "active":
        raise Conflict("podcast feed is disabled")
    automatic = feed.get("auto_download_latest", False)
    if type(automatic) is not bool:
        raise StoreError("auto_download_latest must be boolean")
    if automatic:
        require(store, principal, "edit", station_id)
    imported = commit_feed_url(store, principal, station_id, feed["_id"],
                               expected_content_sha256=expected_content_sha256)
    current_feed = _feed(store, station_id, feed["_id"])
    if current_feed["status"] != "active":
        raise Conflict("podcast feed was disabled during sync")
    current_policy = current_feed.get("auto_download_latest", False)
    if type(current_policy) is not bool:
        raise StoreError("auto_download_latest must be boolean")
    automatic = automatic and current_policy
    latest, downloaded = None, None
    if automatic:
        rows = store.lookup("podcast_episodes", "feed", feed["_id"])
        rows.sort(key=lambda row: (row["published_at"], row["_id"]), reverse=True)
        latest = rows[0] if rows else None
        if latest is not None:
            downloaded = download_episode(store, principal, station_id, latest["_id"],
                                          expected_revision=latest["_rev"])
    return {"feed_id": feed["_id"], "feed_sync": imported,
            "auto_download_latest": automatic,
            "latest_episode_id": latest["_id"] if latest else None,
            "download": downloaded}


def list_episodes(store: Store, principal: Principal, station_id: str,
                  feed_id: str, *, limit: int = 100) -> list[dict]:
    station_id = _id(station_id, "station_id")
    require(store, principal, "view", station_id)
    feed = _feed(store, station_id, feed_id)
    if type(limit) is not int or not 1 <= limit <= MAX_EPISODES:
        raise StoreError("episode list limit must be 1..500")
    rows = store.lookup("podcast_episodes", "feed", feed["_id"])
    rows.sort(key=lambda row: (row["published_at"], row["_id"]), reverse=True)
    return rows[:limit]


def latest_episode(store: Store, principal: Principal, station_id: str,
                   feed_id: str, *, approved_only: bool = False) -> dict | None:
    station_id = _id(station_id, "station_id")
    require(store, principal, "view", station_id)
    feed = _feed(store, station_id, feed_id)
    rows = store.lookup("podcast_episodes", "feed", feed["_id"])
    rows.sort(key=lambda row: (row["published_at"], row["_id"]), reverse=True)
    if not approved_only:
        return rows[0] if rows else None
    for row in rows:
        if row.get("download_state") != "staged" or not row.get("version_id") or \
                not row.get("media_file_id"):
            continue
        version = store.get("versions", row["version_id"])
        media = store.get("media_files", row["media_file_id"])
        if version and media and version.get("status") == "approved" and \
                media.get("status") == "verified" and \
                media.get("version_id") == version["_id"]:
            return row
    return None


def download_episode(store: Store, principal: Principal, station_id: str,
                     episode_id: str, *, expected_revision: int | None = None) -> dict:
    """Fetch an enclosure into a draft catalog version, never auto-approve it."""
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "import", station_id)
    require(store, principal, "edit", station_id)
    episode = _episode(store, station_id, episode_id)
    feed = _feed(store, station_id, episode["feed_id"])
    if feed["status"] != "active":
        raise Conflict("podcast feed is disabled")
    if expected_revision is not None and episode["_rev"] != expected_revision:
        raise Conflict("podcast episode changed")
    if episode.get("download_state") == "staged" and \
            episode.get("media_file_id") and \
            episode.get("download_source_url") == episode["enclosure_url"]:
        media = store.get("media_files", episode["media_file_id"])
        if media and media.get("sha256") == episode.get("download_sha256"):
            return {"episode_id": episode["_id"], "item_id": episode["item_id"],
                    "version_id": episode["version_id"],
                    "media_file_id": episode["media_file_id"],
                    "sha256": episode["download_sha256"], "replayed": True}
    if episode.get("download_state") == "pending":
        raise Conflict("episode download is already pending; recover stale claim explicitly")
    item_id = episode.get("item_id") or str(uuid5(NAMESPACE_URL, "sdbase:podcast:item:" + episode["_id"]))
    version_id = str(uuid5(NAMESPACE_URL, "sdbase:podcast:version:" +
                           episode["_id"] + ":" + episode["enclosure_url"]))
    association_id = str(uuid5(NAMESPACE_URL, "sdbase:podcast:station-item:" + episode["_id"]))
    claim = str(uuid4())
    now = _now()
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    existing_item = store.get("items", item_id)
    existing_version = store.get("versions", version_id)
    if existing_item is None:
        tx.put("items", item_id, {"kind": "program", "title": episode["title"],
                                   "lifecycle": "draft", "recording_id": None,
                                   "created_at": now})
        tx.put("station_items", association_id,
               {"station_id": station_id, "item_id": item_id,
                "category_id": None, "eligible": False, "rotation_code": None})
    elif not store.lookup("station_items", "station_item", station_id, item_id):
        raise Conflict("podcast catalog item is outside station")
    else:
        from .catalog import require_shared_item_permission
        require_shared_item_permission(store, principal, item_id, "edit")
    if existing_version is None:
        number = max((row["version_no"] for row in store.lookup("versions", "item", item_id)),
                     default=0) + 1
        tx.put("versions", version_id,
               {"item_id": item_id, "version_no": number, "label": "Podcast episode",
                "status": "draft", "valid_from": None, "valid_to": None,
                "approved_at": None, "approved_by": None, "created_at": now})
    elif existing_version["item_id"] != item_id or existing_version["status"] in {"approved", "withdrawn"}:
        raise Conflict("podcast version cannot accept an unreviewed download")
    tx.put("podcast_episodes", episode["_id"], _body(episode) | {
        "item_id": item_id, "version_id": version_id,
        "media_file_id": None, "download_sha256": None,
        "download_state": "pending", "download_claim": claim,
        "download_started_at": now,
        "download_error": None}, expected_revision=episode["_rev"])
    tx.commit()
    path: Path | None = None
    try:
        fd, name = tempfile.mkstemp(prefix=".podcast-", suffix=".download", dir=store.root)
        path = Path(name)
        with os.fdopen(fd, "wb") as output:
            result = _fetch(episode["enclosure_url"], max_bytes=MAX_AUDIO_BYTES,
                            destination=output)
            output.flush()
            os.fsync(output.fileno())
        if result["content_type"] in {"text/html", "application/xhtml+xml",
                                      "application/xml", "text/xml", "application/rss+xml",
                                      "application/atom+xml"}:
            raise StoreError("podcast enclosure returned a document rather than audio")
        from .media import stage_media
        staged = stage_media(store, principal, station_id, version_id, path, [store.root])
        current = _episode(store, station_id, episode["_id"])
        if current.get("download_claim") != claim or current["enclosure_url"] != episode["enclosure_url"]:
            raise Conflict("podcast enclosure changed while download was staged")
        tx = store.begin(actor=principal.user_id)
        tx.expect_sequence(store.sequence)
        tx.put("podcast_episodes", current["_id"], _body(current) | {
            "download_state": "staged", "download_claim": None,
            "download_source_url": episode["enclosure_url"],
            "download_sha256": staged["sha256"],
            "media_file_id": staged["media_file_id"],
            "downloaded_at": _now(), "download_error": None},
            expected_revision=current["_rev"])
        tx.commit()
        return {"episode_id": episode["_id"], "item_id": item_id,
                "version_id": version_id, "media_file_id": staged["media_file_id"],
                "sha256": staged["sha256"], "replayed": False,
                "review_required": True}
    except Exception as exc:
        current = store.get("podcast_episodes", episode["_id"])
        if current and current.get("download_claim") == claim:
            try:
                tx = store.begin(actor=principal.user_id)
                tx.put("podcast_episodes", current["_id"], _body(current) | {
                    "download_state": "error", "download_claim": None,
                    "download_error": type(exc).__name__,
                    "last_download_attempt_at": _now()},
                    expected_revision=current["_rev"])
                tx.commit()
            except StoreError:
                pass
        raise
    finally:
        if path is not None:
            path.unlink(missing_ok=True)


def recover_download(store: Store, principal: Principal, station_id: str,
                     episode_id: str, *, expected_revision: int) -> dict:
    """Release a crashed download claim after its bounded network lease."""
    station_id = _id(station_id, "station_id")
    sequence = store.sequence
    require(store, principal, "repair", station_id)
    episode = _episode(store, station_id, episode_id)
    if episode["_rev"] != expected_revision or episode.get("download_state") != "pending":
        raise Conflict("episode is not pending at expected revision")
    started = episode.get("download_started_at")
    if started:
        try:
            if datetime.fromisoformat(started) + timedelta(minutes=10) > datetime.now(timezone.utc):
                raise Conflict("episode download lease has not expired")
        except ValueError as exc:
            raise StoreError("episode download timestamp is invalid") from exc
    tx = store.begin(actor=principal.user_id)
    tx.expect_sequence(sequence)
    tx.put("podcast_episodes", episode["_id"], _body(episode) | {
        "download_state": "error", "download_claim": None,
        "download_error": "stale_claim_recovered",
        "last_download_attempt_at": _now()},
        expected_revision=episode["_rev"])
    tx.commit()
    return store.get("podcast_episodes", episode["_id"])


def put_latest_episode(store: Store, principal: Principal, station_id: str,
                       feed_id: str, plan_id: str, revision_id: str, *,
                       expected_plan_revision: int, ordinal: int,
                       scheduled_at: str, duration_ms: int,
                       territory: str = "*", output_id: str = "*") -> dict:
    """Place the latest reviewed, ready episode in a draft; plan approval is separate."""
    station_id = _id(station_id, "station_id")
    require(store, principal, "edit", station_id)
    if _feed(store, station_id, feed_id)["status"] != "active":
        raise Conflict("podcast feed is disabled")
    if type(duration_ms) is not int or not 1 <= duration_ms <= 24 * 3600 * 1000:
        raise StoreError("podcast plan duration_ms must be 1..86400000")
    selected = latest_episode(store, principal, station_id, feed_id, approved_only=True)
    if selected is None:
        raise Conflict("podcast feed has no approved, verified episode")
    from .readiness import readiness_at
    try:
        start = datetime.fromisoformat(scheduled_at.replace("Z", "+00:00"))
    except (TypeError, ValueError, AttributeError) as exc:
        raise StoreError("scheduled_at must be an ISO 8601 timestamp") from exc
    if start.tzinfo is None or start.utcoffset() is None:
        raise StoreError("scheduled_at requires a timezone")
    ready = readiness_at(store, station_id, selected["version_id"],
                         slot_start=start, slot_end=start + timedelta(milliseconds=duration_ms),
                         require_endpoint_copy=False, territory=territory,
                         output_id=output_id)
    if not ready["ready"]:
        raise Conflict("podcast episode is not distributable: " +
                       ", ".join(ready["blocked_reasons"]))
    from .plans import put_entry
    result = put_entry(store, principal, station_id, plan_id, revision_id,
                       {"ordinal": ordinal, "scheduled_at": start.isoformat(),
                        "event_kind": "audio", "state": "planned",
                        "planned_duration_ms": duration_ms, "duration_kind": "fixed",
                        "payload": {"item_id": selected["item_id"],
                                    "version_id": selected["version_id"],
                                    "podcast_episode_id": selected["_id"],
                                    "territory": territory, "output_id": output_id}},
                       expected_plan_revision=expected_plan_revision)
    return {"episode_id": selected["_id"], "version_id": selected["version_id"],
            "plan_entry": result}
