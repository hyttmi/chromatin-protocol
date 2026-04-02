# Phase 80: Targeted Blob Fetch - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-02
**Phase:** 80-targeted-blob-fetch
**Areas discussed:** BlobFetch wire format, Receive-side dedup logic, Fetch trigger placement, Failure handling

---

## BlobFetch Wire Format

| Option | Description | Selected |
|--------|-------------|----------|
| Hash only (32 bytes) | Minimal request — just SHA3-256 hash | ✓ |
| Hash + namespace (64 bytes) | Include namespace for direct key lookup | |
| You decide | Claude picks most efficient approach | |

**User's choice:** Hash only (32 bytes)
**Notes:** Matches how BlobNotify identifies blobs

---

| Option | Description | Selected |
|--------|-------------|----------|
| Full signed blob (as stored) | Complete FlatBuffer, reuses existing ingest() path | ✓ |
| Raw data + metadata fields | Custom binary, smaller but new parsing path | |
| BlobTransfer reuse | Same payload as sync type 12, different message type | |

**User's choice:** Full signed blob (as stored)
**Notes:** Reuses existing ingest() path identically to sync blobs

---

| Option | Description | Selected |
|--------|-------------|----------|
| Status byte prefix (0=found, 1=not-found) | First byte is status, clean and unambiguous | ✓ |
| Empty payload | Zero-length means not-found, relies on length convention | |
| You decide | Claude picks based on protocol patterns | |

**User's choice:** Status byte prefix (0=found, 1=not-found)

---

## Receive-side Dedup Logic

| Option | Description | Selected |
|--------|-------------|----------|
| storage_.has_blob() check | Existing key-only MDBX lookup, fast | ✓ |
| In-memory bloom filter | O(1) check but adds memory and false-negative risk | |
| You decide | Claude picks simplest correct approach | |

**User's choice:** storage_.has_blob() check

---

| Option | Description | Selected |
|--------|-------------|----------|
| Track pending fetches (hash set) | Dedup concurrent notifications, remove on complete/fail | ✓ |
| No dedup — idempotent ingest | Let duplicates happen, ingest() handles it | |
| You decide | Claude picks based on complexity vs impact | |

**User's choice:** Track pending fetches (hash set)

---

## Fetch Trigger Placement

| Option | Description | Selected |
|--------|-------------|----------|
| PeerManager::on_blob_notify() handler | New handler, owns dedup set, has engine access | ✓ |
| Inline in message_loop dispatch | Handle directly in transport layer | |
| You decide | Claude picks based on existing dispatch patterns | |

**User's choice:** PeerManager::on_blob_notify() handler

---

| Option | Description | Selected |
|--------|-------------|----------|
| Suppress during sync | If PeerInfo.syncing, skip fetch — sync handles it | ✓ |
| Allow alongside sync | Independent paths, send queue serializes safely | |
| You decide | Claude picks based on complexity and syncing flag | |

**User's choice:** Suppress during sync

---

## Failure Handling

| Option | Description | Selected |
|--------|-------------|----------|
| Silent drop | Log debug, move on, reconciliation handles it | ✓ |
| Try another peer | Try BlobFetch from different peer, more complex | |
| You decide | Claude picks simplest approach | |

**User's choice:** Silent drop (not-found response)

---

| Option | Description | Selected |
|--------|-------------|----------|
| No timeout — fire and forget | Remove from pending on response or disconnect | ✓ |
| Short timeout with cleanup | 10s timer, remove from pending, no retry | |
| You decide | Claude picks based on complexity | |

**User's choice:** No timeout — fire and forget

---

| Option | Description | Selected |
|--------|-------------|----------|
| Log warning, drop it | Same as sync — discard invalid blob, no disconnect | ✓ |
| Record strike against peer | Use strike system, accumulate for disconnect | |
| You decide | Claude picks based on existing patterns | |

**User's choice:** Log warning, drop it

---

## Claude's Discretion

- BlobFetch handler placement in dispatch switch
- Pending fetch set data structure
- BlobFetchResponse routing to ingest
- Relay filter verification for types 60/61
- Pending set cleanup on connection close

## Deferred Ideas

None
