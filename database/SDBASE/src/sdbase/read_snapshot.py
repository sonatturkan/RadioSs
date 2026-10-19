"""Immutable committed read views for SDBASE's document API.

The engine replaces committed record objects; capture copies collection maps
and freezes index memberships under one shared lock. Writers can then commit,
checkpoint, or retire hot records without changing the view. This is an
in-process read snapshot, not PostgreSQL tuple MVCC or a disk history service.
"""

from __future__ import annotations

from copy import deepcopy
from threading import Lock, RLock
from types import MappingProxyType
from typing import Iterable, TYPE_CHECKING
from uuid import uuid4
from weakref import WeakKeyDictionary

from .storage import StoreError

if TYPE_CHECKING:
    from .storage import Store


_SOURCE_OWNERS = WeakKeyDictionary()
_SOURCE_OWNER_LOCK = Lock()


def _source_owner(store: Store) -> str:
    """Pin verification to one engine handle without retaining its records."""
    with _SOURCE_OWNER_LOCK:
        owner = _SOURCE_OWNERS.get(store)
        if owner is None:
            owner = uuid4().hex
            _SOURCE_OWNERS[store] = owner
        return owner


class ReadSnapshot:
    """A closeable, read-only view of one committed engine sequence.

    Selecting collections limits capture cost. Access to any collection not
    selected is rejected rather than reading newer data from the live store.
    Returned documents are copied, including nested values.
    """

    @classmethod
    def capture(cls, store: Store, collections: Iterable[str] | None = None) -> ReadSnapshot:
        if isinstance(collections, str):
            collections = (collections,)
        try:
            names = None if collections is None else tuple(collections)
        except TypeError as exc:
            raise StoreError("snapshot collections must be iterable names") from exc
        if names is not None:
            if any(not isinstance(name, str) or not name for name in names):
                raise StoreError("snapshot collection names must be nonempty strings")
            names = tuple(dict.fromkeys(names))
        view = cls()
        with store._locked(exclusive=False):
            store._reload_locked()
            selected = tuple(set(store._state) | set(store.index_specs)) if names is None else names
            view._all_collections = names is None
            view._records = {name: dict(store._state.get(name, {})) for name in selected}
            view._sequence = store._sequence
            metadata = store._meta()
            view._store_id = metadata["store_id"]
            # Collection generations identify logical changes; the handle and
            # checkpoint incarnation also isolate cache certificates after a
            # reload/reopen of a different physical source with the same IDs.
            view._source_pin = (_source_owner(store), store._generation,
                                store._checkpoint_stamp, metadata.get("node_id"),
                                metadata.get("timeline_id"), metadata.get("epoch"))
            view._database_path = store.journal
            view._schema_version = store.schema_version
            view._generations = {name: store._collection_sequences.get(name, 0) for name in selected}
            view.index_specs = MappingProxyType({
                name: MappingProxyType(dict(store.index_specs.get(name, {})))
                for name in selected
            })
            view._indices = {
                (name, index): {key: frozenset(ids) for key, ids in
                               store._indices.get((name, index), {}).items()}
                for name in selected for index in view.index_specs[name]
            }
        view._closed = False
        view._ordered_indices = {}
        view._cache_lock = RLock()
        return view

    def _check(self, collection: str | None = None) -> None:
        if self._closed:
            raise StoreError("read snapshot is closed")
        if collection is not None:
            if not isinstance(collection, str) or not collection:
                raise StoreError("collection must be a nonempty string")
            if collection not in self._records and not self._all_collections:
                raise StoreError(f"collection {collection} was not captured in this snapshot")

    @property
    def sequence(self) -> int:
        self._check()
        return self._sequence

    @property
    def store_id(self) -> str:
        self._check()
        return self._store_id

    @property
    def database_path(self):
        self._check()
        return self._database_path

    @property
    def schema_version(self) -> int:
        self._check()
        return self._schema_version

    @property
    def collections(self) -> tuple[str, ...]:
        self._check()
        return tuple(sorted(self._records))

    def collection_generation(self, collection_names: Iterable[str]) -> int:
        if isinstance(collection_names, str):
            collection_names = (collection_names,)
        names = tuple(collection_names)
        for name in names:
            self._check(name)
        return max((self._generations.get(name, 0) for name in names), default=0)

    def count(self, collection: str) -> int:
        self._check(collection)
        return len(self._records.get(collection, {}))

    def get(self, collection: str, key: str) -> dict | None:
        self._check(collection)
        record = self._records.get(collection, {}).get(str(key))
        return deepcopy(record["data"] | {"_id": str(key), "_rev": record["revision"]}) if record else None

    def scan(self, collection: str) -> list[dict]:
        self._check(collection)
        return [deepcopy(record["data"] | {"_id": key, "_rev": record["revision"]})
                for key, record in self._records.get(collection, {}).items()]

    def lookup(self, collection: str, index_name: str, *values) -> list[dict]:
        self._check(collection)
        spec = self.index_specs.get(collection, {}).get(index_name)
        if spec is None or len(values) != len(spec.fields):
            raise StoreError("unknown index or wrong key arity")
        ids = self._indices.get((collection, index_name), {}).get(tuple(values), ())
        return [self.get(collection, key) for key in sorted(ids)]

    def query(self, collection: str, **arguments) -> dict:
        """Run a bounded document query against this view."""
        from .query import query_page
        return query_page(self, collection, **arguments)

    def explain(self, collection: str, **arguments) -> dict:
        """Describe candidate selection without fetching result documents."""
        from .query import explain_query
        return explain_query(self, collection, **arguments)

    def close(self) -> None:
        """Release retained old record bodies and derived ordered keys."""
        if not self._closed:
            # Close every disk reader even if one fails: a mapped index file
            # left open cannot be deleted or replaced on Windows, so a single
            # failure must not leak the other mappings.
            failure = None
            readers = getattr(self, "_disk_readers", {})
            for reader in readers.values():
                try:
                    reader.close()
                except Exception as exc:
                    failure = failure or exc
            readers.clear()
            self._records.clear()
            self._indices.clear()
            self._ordered_indices.clear()
            self._closed = True
            if failure is not None:
                raise failure

    def __enter__(self) -> ReadSnapshot:
        self._check()
        return self

    def __exit__(self, *_exc) -> None:
        self.close()
