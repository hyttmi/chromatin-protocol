# chromatindb — Architecture

> Internal implementation reference. For the wire protocol (bytes on the wire),
> see [PROTOCOL.md](PROTOCOL.md). For user-facing operator configuration, see
> [db/README.md](README.md). For the `cdb` client, see [cli/README.md](../cli/README.md).

## Overview

chromatindb is organized in three tiers:

1. **Storage** (libmdbx, 8 DBIs) — persistent state, ACID, single-writer strand.
2. **Engine** (ingest pipeline, validation, cascade) — pure business logic.
   Offloads heavy crypto (ML-DSA-87 verify, SHA3-256 hashing) to a thread pool,
   serializes every storage access via the `io_context` strand.
3. **Net** (peer/client transport, handshake, sync orchestration) — AEAD-framed
   connections, PQ or lightweight handshake, three-phase sync reconciliation,
   push-then-fetch replication.

Concurrency invariant (see [Storage Strand Model](#storage-strand-model)): all
Storage mutations execute on a single thread — the `io_context` worker that
first entered the Storage instance. Any code path that awaits on the crypto
thread pool MUST post back to `ioc_` before touching Storage. Verified under
TSAN in the concurrent-ingest ship gate (`.planning/phases/121-storage-concurrency-invariant/121-VERIFICATION.md`).

Cross-document contract: wire-level facts (byte layouts, message-type numbers,
error-code table) live in [PROTOCOL.md](PROTOCOL.md). This document links by
section anchor and does not repeat those bytes.

```
  ┌───────────────────────┐   ┌─────────────────────────┐
  │  Net (peers + clients)│   │  cdb client / SDK       │
  │  handshake, AEAD,     │◄─►│  (off-process)          │
  │  framing, sync driver │   │                         │
  └────────────┬──────────┘   └─────────────────────────┘
               │ TransportMessage envelopes
               ▼
  ┌───────────────────────┐
  │  Engine (ingest)      │   offload: ML-DSA verify, SHA3
  │  11-step pipeline     │◄─► crypto_thread_pool workers
  │  PUBK-first, BOMB     │
  └────────────┬──────────┘
               │  post back to ioc_ → STORAGE_THREAD_CHECK()
               ▼
  ┌───────────────────────┐
  │  Storage (libmdbx)    │
  │  8 DBIs, ACID, strand │
  └───────────────────────┘
```

---

## Storage Layer

Backed by [libmdbx](https://libmdbx.dqdkfa.ru/): memory-mapped B-tree with
copy-on-write, ACID single-writer + multi-reader MVCC, no compaction
stop-the-world. The node opens one environment (`data_dir/chromatindb.mdbx`)
with `max_maps = 10` (eight DBIs in use, two slots of headroom).

### Sub-databases

Eight named DBIs, each with a fixed key/value shape. The canonical shape table
lives inline in `db/storage/storage.h:100-108`; this table is the annotated
version.

| DBI name       | Key layout                                      | Value layout                                    | Access pattern                          |
|----------------|-------------------------------------------------|-------------------------------------------------|-----------------------------------------|
| `blobs`        | `namespace(32) \|\| content_hash(32)`           | encrypted Blob ciphertext (DARE envelope)       | hot read + write                        |
| `sequence`     | `namespace(32) \|\| seq_num_be(8)`              | `content_hash(32)` (zero-hash = deleted)        | hot read (sync, ls), write on ingest    |
| `expiry`       | `expiry_ts_be(8) \|\| content_hash(32)`         | `namespace(32)`                                 | scan on timer, delete on expiry         |
| `delegation`   | `namespace(32) \|\| delegate_pk_hash(32)`       | `delegation_blob_hash(32)`                      | O(1) delegate-verify lookup             |
| `tombstone`    | `namespace(32) \|\| target_content_hash(32)`    | empty (existence-only)                          | O(1) tombstone check on query + sync    |
| `cursor`       | `peer_hash(32) \|\| namespace(32)`              | `seq_num_be(8) \|\| round_count_be(4) \|\| last_sync_ts_be(8)` | per-peer per-namespace sync progress    |
| `quota`        | `namespace(32)`                                 | `total_bytes_be(8) \|\| blob_count_be(8)`       | read on every ingest                    |
| `owner_pubkeys`| `signer_hint(32)`                               | `ml_dsa_87_signing_pk(2592)`                    | read on every non-PUBK ingest           |

All keys and all numeric values are big-endian. The lexicographic byte order of
the big-endian integer encoding gives range scans over `seq_num` and
`expiry_ts` the correct numeric ordering for free.

Per-DBI notes:

- **`blobs`** — primary content store. Value is the client-side DARE envelope
  (see [PROTOCOL.md §Client-Side Envelope Encryption](PROTOCOL.md#client-side-envelope-encryption))
  wrapping the signed FlatBuffer. The node does not decrypt blob payloads; it
  stores the ciphertext verbatim. Content-hash is over the FlatBuffer bytes
  (envelope included) — see
  [PROTOCOL.md §Sending a Blob (BlobWrite = 64)](PROTOCOL.md#sending-a-blob-blobwrite--64).

- **`sequence`** — per-namespace monotonic sequence index. The ingest writer
  assigns the next `seq_num` when storing a new content-hash; reverse-lookup by
  `(namespace, seq_num)` is how sync pagination and `ls` pagination work. A
  deleted blob's `seq_num` is preserved with a zero-hash sentinel so cursor
  arithmetic stays monotonic (see `Storage::delete_blob_data`).

- **`expiry`** — chronological index for the expiry scanner. Key is
  `(expiry_ts_be, content_hash)` — a cursor seek to `now_be || 0x00×32`
  lands on the first expired blob; scan to `now_be || 0xff×32` and delete. The
  scanner runs on `expiry_scan_interval_seconds` (default 60). Entries created
  only for `ttl > 0` blobs.

- **`delegation`** — authority index for the delegate write path. Composite
  key `(namespace, delegate_pk_hash)` maps to the delegation-blob's content
  hash. On ingest of a non-PUBK blob with a `signer_hint` that misses
  `owner_pubkeys`, the verify hot path tries this DBI next via
  `get_delegate_pubkey_by_hint` — signer_hint is definitionally
  `SHA3-256(delegate_pubkey)`, so the composite-key lookup is an O(1) point
  lookup, not an iteration. See
  [PROTOCOL.md §Namespace Delegation](PROTOCOL.md#namespace-delegation).

- **`tombstone`** — existence-only index. Key presence is the semantic;
  value is empty. Populated as a side-effect of accepting a tombstone blob
  (the blob itself lives in `blobs` with magic `0xDEADBEEF`). Consulted on
  every query and sync egress so deleted content never leaks back. BOMB
  side-effect also writes here, one entry per target (see
  [Engine §BOMB Cascade](#bomb-cascade-side-effect)).

- **`cursor`** — sync resumption state. Written by `SyncOrchestrator` after
  each Phase C completes. `round_count` drives the safety-net full-resync
  every Nth round (config `full_resync_interval`, default 10). On SIGHUP the
  node resets `round_count` to zero across all cursors to force a fresh
  reconciliation after ACL changes. Stale cursors (peers not seen in
  `cursor_stale_seconds`, default 3600) are cleaned up by
  `cleanup_stale_cursors` at startup.

- **`quota`** — aggregate per-namespace usage. `Storage::store_blob` checks
  this inside the write txn before committing (atomic with the blobs write)
  so the quota cannot be overshot by concurrent writers. The aggregate is
  rebuilt from `blobs` on startup by `rebuild_quota_aggregates` to tolerate
  partial writes or format changes.

- **`owner_pubkeys`** — the post-122 DBI that holds the 2592-byte ML-DSA-87
  signing pubkey once per `signer_hint`. Before this DBI existed, every blob
  embedded its author's 2592-byte pubkey; now the wire carries only the
  32-byte hint. The node learns the pubkey from a PUBK blob
  ([PROTOCOL.md §PUBK Blob Format](PROTOCOL.md#pubk-blob-format)) on first
  write in a namespace; subsequent blobs in that namespace pay one 32-byte
  hint instead of 2592. See
  [PROTOCOL.md §owner_pubkeys DBI](PROTOCOL.md#owner_pubkeys-dbi).

### Transaction Model

libmdbx MVCC: read transactions are lock-free and snapshot-consistent, one
writer at a time. chromatindb never nests transactions. Read paths open a
read txn, scan, close. Write paths open one write txn that spans all
per-ingest side-effects:

```
  open write txn
    check capacity + quota limits (atomic within txn)
    mdbx_put(blobs, ns||hash, ciphertext)
    mdbx_put(sequence, ns||next_seq_be, hash)
    if ttl > 0: mdbx_put(expiry, expiry_ts_be||hash, ns)
    if tombstone: mdbx_put(tombstone, ns||target_hash, {})
    if BOMB: for each target: {
        mdbx_del(blobs, ns||target_hash)       // side-effect delete
        mdbx_put(tombstone, ns||target_hash, {})
    }
    if NAME: blobs write only; resolver reads `sequence` at query time
    if PUBK: mdbx_put(owner_pubkeys, signer_hint, signing_pk)
    if delegation blob: mdbx_put(delegation, ns||delegate_pk_hash, blob_hash)
    mdbx_put(quota, ns, (bytes + n, count + 1))
  commit
  (outside txn) notify pub/sub subscribers
```

Error mapping:

| libmdbx status      | Node-side handling                                           |
|---------------------|--------------------------------------------------------------|
| `MDBX_MAP_FULL`     | `store_blob` returns `CapacityExceeded` → wire `StorageFull` |
| `MDBX_TXN_FULL`     | internal error; logged + peer strike                         |
| `MDBX_KEYEXIST`     | expected on dedup — returns `Duplicate` status               |
| any other non-OK    | internal error; logged + peer strike                         |

Atomicity guarantee: a crash mid-txn rolls back cleanly (libmdbx write-ahead
design). The startup `integrity_scan()` reads every DBI to verify entry counts
cross-reference; inconsistencies are logged as warnings but do not block
startup (an operator-visible signal, not a liveness killer).

### Storage Strand Model

**Rule:** every public method on `Storage` executes on the owner thread — the
first thread that ever called into this `Storage` instance. The owner tid is
captured in `db/storage/thread_check.h` on first entry and every subsequent
public-method entry asserts identity.

```
       io_context (single-threaded owner)
             │
             ▼
       ┌────────────────┐   co_await asio::post(ioc_, use_awaitable)
       │   Storage::*   │ ◄─────────── crypto thread pool workers
       └────────────────┘              (ML-DSA-87 verify, SHA3 hash)
             │
             ▼
       STORAGE_THREAD_CHECK() → assert(tid == owner_tid)
```

**Why it matters.** libmdbx write transactions are not thread-safe — a write
txn is bound to the opening thread. Coroutines make this easy to violate
accidentally: every `co_await` is a potential thread transition, and an
awaiter that resumes on a crypto-pool worker thread can call straight into
Storage without realising the executor changed. The `STORAGE_THREAD_CHECK()`
macro (defined in `db/storage/thread_check.h:107,109`) fails fast when that
happens. Under `NDEBUG` the macro compiles to `(void)0`; debug and TSAN
builds abort on violation.

**The pattern contributors must follow.** Every time the Engine offloads work
to the crypto thread pool, it must post back to the io_context before
touching Storage:

```cpp
// 1. Heavy compute on thread pool:
auto sig_ok = co_await thread_pool.offload([&]{
    return verify_signature(signing_pk, signed_input, signature);
});
// 2. BEFORE touching Storage, post back to ioc_:
co_await asio::post(ioc_, asio::use_awaitable);
// 3. Now safe:
auto result = storage_.store_blob(target_namespace, blob, hash, encoded, ...);
```

Any code path that skips the `asio::post(ioc_, ...)` step will trip
`STORAGE_THREAD_CHECK()` on the first storage call after resumption.

**Evidence.** The concurrent-ingest TSAN ship-gate (see
`.planning/phases/121-storage-concurrency-invariant/121-VERIFICATION.md` and
`121-TSAN-RESULTS.md`) confirmed zero findings against a stress workload that
runs multiple peers ingesting to overlapping namespaces. The ship-gate is
part of the reproducible TSAN build and must stay clean for any change
touching Storage or the ingest pipeline.

**Canonical call sites.** `db/engine/engine.cpp` follows this discipline at
every offload boundary — the `BlobEngine::ingest` coroutine uses `co_await
thread_pool.offload(...)` for SHA3 content-hashing and ML-DSA verification,
then `co_await asio::post(ioc_, use_awaitable)` before every storage call.
New contributors should imitate those boundaries verbatim; do not cache a
`co_await this_coro::executor` result and try to be clever.

### Encryption at Rest

Every blob payload is encrypted with ChaCha20-Poly1305 before the libmdbx
write, using a key derived from the node-local master key. This is called
DARE (disk-at-rest encryption) and is unrelated to the
client-side envelope encryption described in
[PROTOCOL.md §Client-Side Envelope Encryption](PROTOCOL.md#client-side-envelope-encryption)
— DARE is the node's protection for stored ciphertext; client-side envelopes
protect the payload across the wire.

- **Master key:** `data_dir/master.key`, 32 bytes of CSPRNG output, mode 0600.
  The node refuses to start if the file is world-readable or world-writable.
  Auto-generated on first run and never rotated automatically; operators who
  rotate must migrate data out and in.
- **Per-blob key derivation:** `HKDF-SHA256(ikm = master_key, salt = empty,
  info = "chromatindb-dare-v1")`. See
  [PROTOCOL.md §HKDF Label Registry](PROTOCOL.md#hkdf-label-registry) for the
  label registry (shared with transport and client envelopes — each label is
  unique).
- **Envelope format:** `[version:1][nonce:12][ciphertext+tag:N]` — version
  byte reserves room for a future DARE v2.
- **Associated data:** the libmdbx key (`namespace || content_hash`). This
  binds ciphertext to its storage slot — moving a ciphertext record to a
  different key makes decryption fail.

Backup note: the master key is part of the backup surface. A backup of the
database without `master.key` is unrecoverable data. Operators are told this
in [db/README.md](README.md#encryption-at-rest-feature-bullet).

### Expiry and Quotas

**Expiry scanner.** The node runs a periodic scan on the `expiry` DBI every
`expiry_scan_interval_seconds` (default 60, minimum 10). The scanner seeks to
the first key with `expiry_ts_be <= now_be`, iterates forward, deletes each
expired blob from `blobs` and removes the expiry-index entry. `sequence` is
left intact with a zero-hash sentinel for the gap — cursor-based sync treats
a zero-hash seq as "nothing to transfer" and skips. Cost is O(expired), not
O(stored). `get_earliest_expiry()` gives the timer a next-wakeup hint so the
event loop sleeps to the first actually-interesting moment when possible.

**Evict-on-query.** Query handlers (`ReadRequest`, `ListRequest`,
`ExistsRequest`, etc.) filter expired blobs at the exit point regardless of
whether the scanner has caught up yet. See
[PROTOCOL.md §Query Path Filtering](PROTOCOL.md#query-path-filtering) for
the observable wire behaviour.

**Quotas.** Per-namespace byte and blob-count limits are enforced inside the
write transaction (`store_blob` checks `quota` DBI before committing). Global
defaults live in the `namespace_quota_bytes` and `namespace_quota_count`
config fields; per-namespace overrides live in `namespace_quotas` — an
explicit `0` override means "unlimited for this namespace" (escape hatch for
whitelisting high-write owners). When a write would exceed the limit, the
node returns `QuotaExceeded` on the wire; see
[PROTOCOL.md §Quota Signaling](PROTOCOL.md#quota-signaling).

**Saturation.** Expiry arithmetic uses saturating addition:
`expiry_ts = sat_add(timestamp, ttl)`. If the sum would overflow `uint64_t`,
the blob is treated as effectively permanent. This matches
[PROTOCOL.md §Expiry Arithmetic](PROTOCOL.md#expiry-arithmetic).

Source-of-truth files for this section:

- `db/storage/storage.h` — public Storage API and per-DBI documentation comments.
- `db/storage/thread_check.h` — `STORAGE_THREAD_CHECK()` macro definition.
- `db/storage/storage.cpp` — write-txn composition; expiry scanner loop.
- `.planning/phases/121-storage-concurrency-invariant/121-VERIFICATION.md` — TSAN ship-gate evidence.

<!-- The Engine Layer section is appended by Task 2. Net Layer + Configuration + Subsystems by Task 3. -->
