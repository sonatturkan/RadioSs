"""Durable live-player groups, per-owner programmes and AutoDJ preferences.

This document is separate from mixer channels and grants no playback authority.
"""
from .storage import StoreError


def validate_live_players(config, station_id, state, *, persisted=False, previous=None):
    from .desktop_api import _id, _int, _list, _object, _text
    fields = {"schema_version", "deck_count", "slots", "names", "clusters", "players"}
    _object(config, fields, {"schema_version", "deck_count"})
    _int(config["schema_version"], "live player schema_version", 1, 1)
    count = _int(config["deck_count"], "live player deck_count", 1, 32)
    slots = _list(config.get("slots", list(range(count))), "live player slots", 32)
    for slot in slots:
        _int(slot, "live player slot", 0, 31)
    if len(slots) != count or len(set(slots)) != count:
        raise StoreError("live player slots must be distinct and match deck_count")
    active = {str(slot + 1) for slot in slots}
    names = _object(config.get("names", {}), {str(number) for number in range(33)})
    for number, name in names.items():
        _text(name, f"live player {number} name", 64)
    previous = previous if isinstance(previous, dict) else {}
    previous_clusters = {cluster["id"]: cluster for cluster in previous.get("clusters", [])}
    previous_players = previous.get("players", {})

    def schema_reference(value, old_value):
        _text(value, "live player automation_id", 36)
        if not value:
            return
        identifier = _id(value, "live player automation_id")
        if identifier != value:
            raise StoreError("live player automation_id must be a canonical UUID")
        row = state.get("desktop_configurations", {}).get(identifier, {}).get("data")
        if not row or row.get("station_id") != station_id or row.get("kind") != "weekly_programme":
            raise StoreError("live player automation_id must reference a weekly programme in this station")
        if row.get("status") != "active" and not (persisted or value == old_value):
            raise StoreError("live player automation_id must reference an active weekly programme")

    clusters = {}
    cluster_names = set()
    for cluster in _list(config.get("clusters", []), "live player clusters", 32):
        _object(cluster, {"id", "name", "automation_id"}, {"id", "name"})
        identifier = _id(cluster["id"], "live player cluster id")
        name = _text(cluster["name"], "live player cluster name", 80, True)
        if identifier != cluster["id"] or identifier in clusters or name != name.strip() or name in cluster_names:
            raise StoreError("live player cluster ids and trimmed names must be distinct")
        clusters[identifier] = cluster
        cluster_names.add(name)
        schema_reference(cluster.get("automation_id", ""), previous_clusters.get(identifier, {}).get("automation_id", ""))
    players = _object(config.get("players", {}), active)
    groups = {}
    settings = {}
    for number in active:
        player = _object(players.get(number, {}), {"cluster_id", "automation_id", "autodj_mode", "autodj_target"})
        cluster_id = _text(player.get("cluster_id", ""), "live player cluster_id", 36)
        if cluster_id and cluster_id not in clusters:
            raise StoreError("live player cluster_id must name a defined cluster")
        schema_reference(player.get("automation_id", ""), previous_players.get(number, {}).get("automation_id", ""))
        mode = _int(player.get("autodj_mode", 0), "live player AutoDJ mode", 0, 2)
        target = _int(player.get("autodj_target", 0), "live player AutoDJ target", 0, 32)
        groups[number] = cluster_id
        settings[number] = mode, target
    for number, (mode, target) in settings.items():
        if target and (mode != 2 or str(target) == number or str(target) not in active or groups[number] != groups[str(target)]):
            raise StoreError("AutoDJ transitions must target another player in the same group")


def group_players(config, cluster_id=""):
    """Visible numbers (1..32) of one cluster's players, or of the independent players for ""."""
    count = config.get("deck_count", 0)
    slots = config.get("slots", list(range(count)))
    players = config.get("players", {})
    wanted = cluster_id or ""
    return sorted(slot + 1 for slot in slots if players.get(str(slot + 1), {}).get("cluster_id", "") == wanted)


def route_pools(config, cluster_id, pools):
    """Clamp saved per-kind AutoDJ pools (1-based) to one group, as the desktop scope runner does.

    Players outside the group are dropped and named in the returned Turkish notice; a kind left
    without players falls back to the complete group. Returns (routed_pools, notice_or_None).
    """
    members = group_players(config, cluster_id)
    routed, missing = {}, set()
    for kind, pool in pools.items():
        kept = [player for player in pool if player in members]
        missing.update(player for player in pool if player not in members)
        routed[kind] = kept or list(members)
    if not missing:
        return routed, None
    names = ", ".join(f"Oynatıcı {player}" for player in sorted(missing))
    where = ("bu kümede yok; kümenin oynatıcıları kullanılıyor" if cluster_id
             else "Kümesizler içinde yok; Kümesizler oynatıcıları kullanılıyor")
    return routed, f"Şemadaki {names} {where}."
