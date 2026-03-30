# Phase 73: Extended Queries & Pub/Sub - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md -- this log preserves the alternatives considered.

**Date:** 2026-03-30
**Phase:** 73-extended-queries-pub-sub
**Areas discussed:** Notification delivery API, Subscription lifecycle, Batch operation interface, Query result organization

---

## Notification Delivery API

| Option | Description | Selected |
|--------|-------------|----------|
| Async iterator | async for notif in conn.notifications() -- lazy, backpressure-friendly, Pythonic | Yes |
| Callback registration | conn.on_notification(ns, callback) -- push-based, familiar from event emitters | |
| Both | Offer both patterns, iterator as primary | |

**User's choice:** Async iterator
**Notes:** None

| Option | Description | Selected |
|--------|-------------|----------|
| Single merged stream | One conn.notifications() yields all subscribed namespaces | Yes |
| Per-namespace iterators | conn.subscribe(ns) returns an iterator for that namespace | |

**User's choice:** Single merged stream
**Notes:** None

| Option | Description | Selected |
|--------|-------------|----------|
| Namespace + blob_hash + seq_num | Minimal -- tells user what changed, they call read_blob() if needed | Yes |
| Full blob data included | Notification carries full blob data inline | |

**User's choice:** Namespace + blob_hash + seq_num
**Notes:** None

---

## Subscription Lifecycle

| Option | Description | Selected |
|--------|-------------|----------|
| Await server confirmation | subscribe() sends and waits for server ack before returning | Yes |
| Fire-and-forget | subscribe() sends and returns immediately | |

**User's choice:** Await server confirmation
**Notes:** None

| Option | Description | Selected |
|--------|-------------|----------|
| Track in a set | Client maintains set of subscribed namespaces, .subscriptions property | Yes |
| No tracking, stateless | Client doesn't remember subscriptions | |

**User's choice:** Track in a set
**Notes:** None

---

## Batch Operation Interface

| Option | Description | Selected |
|--------|-------------|----------|
| list[bytes] | Simple list of 32-byte hashes, consistent with bytes-only convention | Yes |
| Variadic *args | batch_exists(ns, hash1, hash2, hash3) | |

**User's choice:** list[bytes]
**Notes:** None

| Option | Description | Selected |
|--------|-------------|----------|
| dict[bytes, bool] | Maps each hash to existence status, O(1) lookup | Yes |
| list[bool] positional | Ordered booleans matching input positions | |

**User's choice:** dict[bytes, bool]
**Notes:** None

| Option | Description | Selected |
|--------|-------------|----------|
| BatchReadResult with truncated flag | Result with .blobs dict and .truncated bool | Yes |
| Auto-paginate internally | SDK keeps calling until all blobs fetched | |

**User's choice:** BatchReadResult with truncated flag
**Notes:** None

| Option | Description | Selected |
|--------|-------------|----------|
| No client-side limits | Let server enforce limits, truncation flag handles overflow | Yes |
| SDK-side cap (e.g. 100) | Raise ValueError if too many hashes | |

**User's choice:** No client-side limits
**Notes:** YAGNI

---

## Query Result Organization

| Option | Description | Selected |
|--------|-------------|----------|
| All in types.py | Single module, ~18 types total, one import location | Yes |
| Split into types/ package | Submodule with core.py, query.py, pubsub.py | |

**User's choice:** All in types.py
**Notes:** None

| Option | Description | Selected |
|--------|-------------|----------|
| Python-native types | str for version/address, list[str] for supported types, int for counts | Yes |
| Raw wire types | Keep bytes for everything, user decodes | |

**User's choice:** Python-native types
**Notes:** None

| Option | Description | Selected |
|--------|-------------|----------|
| All in __all__ | All new types exported from top-level __init__.py | Yes |
| Only most-used types | Export Notification, BatchReadResult; rest via chromatindb.types | |

**User's choice:** All in __all__
**Notes:** None

---

## Claude's Discretion

- Codec function organization for 10+ new encode/decode functions
- Exact field names/types for each introspection result type
- Integration test organization
- MetadataResult vs ReadResult for metadata query
- TimeRangeResult structure

## Deferred Ideas

None
