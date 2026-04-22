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

---

## Engine Layer

The Engine sits between Net and Storage. It owns the ingest pipeline, the
PUBK-first gate, `signer_hint` resolution, BOMB-cascade side-effects, and the
crypto-offload discipline. It holds no mutable state of its own — everything
persistent lives in Storage; everything transient lives on the coroutine
stack of a single ingest. Source: `db/engine/engine.cpp`
(`BlobEngine::ingest`, `BlobEngine::delete_`).

### Ingest Pipeline

```
 Net (BlobWrite = 64)
       │
       ▼
 ┌─────────────────────────────────────────────────────────────┐
 │  BlobEngine::ingest(target_namespace, blob, source_conn)    │
 │                                                              │
 │  Step 0    size check         (one integer compare)          │
 │  Step 0b   fast-reject cap    (storage capacity, quota)      │
 │  Step 0c   timestamp window   (future-skew, staleness)       │
 │  Step 0d   already-expired    (ts + ttl <= now)              │
 │  Step 0e   max_ttl check      (tombstone + BOMB exempt)      │
 │  Step 1    structural decode  (signature non-empty)          │
 │  Step 1.5  PUBK-first gate    (adversarial-flood defense)    │
 │  Step 1.7  BOMB structural    (ttl=0 + shape check)          │
 │                                                              │
 │  ─── offload boundary: heavy crypto on thread pool ───       │
 │  Step 2    signer_hint resolve (owner_pubkeys vs delegation) │
 │  Step 2a   fast-reject cap recheck                           │
 │  Step 2.5  content_hash = SHA3(encoded blob)                 │
 │  Step 3    verify signature   (ML-DSA-87, ~5-10 ms)          │
 │  ─── post back to ioc_ before touching Storage ───           │
 │                                                              │
 │  Step 3.5  tombstone / BOMB side-effect (delete targets)     │
 │  Step 4    storage.store_blob (atomic: capacity+quota+blob)  │
 │  Step 4.5  PUBK register      (owner_pubkeys if PUBK blob)   │
 │                                                              │
 └─────────────────────────────────────────────────────────────┘
       │
       ▼
 WriteAck (success) or ErrorResponse (see table below)
```

The per-step table below names each reject code; the user-facing wording and
the error-response envelope layout live in
[PROTOCOL.md §ErrorResponse (Type 63)](PROTOCOL.md#errorresponse-type-63).
Source is `db/engine/engine.cpp` top-of-file through `Step 4.5`; code tags
are the inline comment markers verified against the current file.

| Step   | Check                                          | Reject → wire error                    | Source                      |
|--------|------------------------------------------------|----------------------------------------|-----------------------------|
| 0      | `blob.data.size() <= MAX_BLOB_DATA_SIZE`       | `oversized_blob`                       | engine.cpp:109              |
| 0b     | Storage capacity fast-reject (non-authoritative)| short-circuit `CapacityExceeded`       | engine.cpp:118              |
| 0c     | Timestamp window (past-staleness, future-skew) | `timestamp_rejected` (`0x04`)          | engine.cpp:129              |
| 0d     | Already-expired (`ts + ttl <= now`)            | `expired` (silent)                     | engine.cpp:148              |
| 0e     | `max_ttl` ceiling (tombstone + BOMB exempt)    | `ttl_exceeded`                         | engine.cpp:160              |
| 1      | Signature non-empty                            | `malformed_blob`                       | engine.cpp:175              |
| 1.5    | PUBK-first gate                                | `pubk_first_violation` (`0x07`)        | engine.cpp:188              |
| 1.7    | BOMB structural (ttl=0 + shape)                | `bomb_ttl_nonzero` (`0x09`)            | engine.cpp:209              |
|        |                                                | `bomb_malformed` (`0x0A`)              | engine.cpp:216              |
| 2      | Resolve signing pubkey (owner vs delegate)     | `pubk_mismatch` (`0x08`)               | engine.cpp:227              |
|        |                                                | `unknown_signer_hint`                  | engine.cpp:290              |
|        | BOMB by delegate                               | `bomb_delegate_not_allowed` (`0x0B`)   | engine.cpp:311              |
| 2a     | Storage capacity fast-reject (post-resolve)    | short-circuit `CapacityExceeded`       | engine.cpp:324              |
| 2.5    | `blob_hash = SHA3-256(encoded_blob)`           | n/a (compute step)                     | engine.cpp:345              |
| 3      | ML-DSA-87 verify (build_signing_input + verify)| `invalid_signature` (silent + strike)  | engine.cpp:365              |
| 3.5    | Tombstone / BOMB cascade side-effect           | n/a (side-effect)                      | engine.cpp:388              |
| 4      | `storage.store_blob` (atomic capacity+quota)   | `CapacityExceeded` / `QuotaExceeded`   | engine.cpp:438              |
| 4.5    | Register PUBK signing pk in `owner_pubkeys`    | n/a (idempotent write)                 | engine.cpp:421              |

Note on ordering: the PUBK-first gate runs BEFORE the crypto offload so the
adversarial flood case (unrecognised namespace, non-PUBK payload) never
reaches ML-DSA-87 verify. Every `co_await` is a potential thread transition,
so the Engine explicitly posts back to `ioc_` between offload and any
`storage_.*` call. See [Storage Strand Model](#storage-strand-model).

Cross-links for the reject semantics:
[PROTOCOL.md §ErrorResponse (Type 63)](PROTOCOL.md#errorresponse-type-63),
[PROTOCOL.md §Ingest Validation](PROTOCOL.md#ingest-validation).

### PUBK-First Enforcement

The implementation side of [PROTOCOL.md §PUBK-First Invariant](PROTOCOL.md#pubk-first-invariant).

On every ingest, Step 1.5 calls `storage_.has_owner_pubkey(target_namespace)`:

- If the row is present, fall through to `signer_hint` resolution at Step 2.
- If the row is absent and the inbound blob is a PUBK
  (`wire::is_pubkey_blob(blob.data)`), fall through — Step 4.5 will register
  the embedded signing pubkey once the signature verifies.
- If the row is absent and the inbound blob is **not** a PUBK, reject with
  `0x07 pubk_first_violation`.

This gate is the single implementation point for the invariant. Everything
else inherits it: direct writes from `cdb`, delegate writes, sync Phase C
ingest (because `SyncProtocol::ingest_blobs` delegates to `BlobEngine::ingest`
rather than reimplementing the pipeline — see
`feedback_no_duplicate_code.md`), tombstones, and BOMBs all pass through the
same `ingest` coroutine and therefore the same PUBK-first check.

Registration (Step 4.5) is idempotent on the byte-identical value. If a
malicious or buggy peer sends a PUBK blob with a signing pubkey that differs
from the one already registered for that `signer_hint`,
`register_owner_pubkey` throws `std::runtime_error` (first-wins rule). The
caller treats that as an internal error and rejects the write.

Test coverage: `db/tests/test_pubk_first.cpp` (direct ingest path),
`db/tests/test_pubk_first_sync.cpp` (sync path through
`SyncProtocol::ingest_blobs`), `db/tests/test_pubk_first_tsan.cpp`
(concurrent registration race — under TSAN, no race when two connections
ingest PUBK blobs for distinct namespaces simultaneously).

### signer_hint Resolution

Two lookup paths, tried in order at Step 2. Reference:
[PROTOCOL.md §signer_hint Semantics](PROTOCOL.md#signer_hint-semantics).

**Owner path.** `storage_.get_owner_pubkey(blob.signer_hint)` is an O(1)
point lookup on `owner_pubkeys`. A hit means "this signer_hint is the owner
of some namespace"; the Engine then checks that the owner matches
`target_namespace` — if the recovered signing pubkey hashes to
`target_namespace` (the PUBK invariant that pins `namespace =
SHA3-256(signing_pk)`) the write is accepted as an owner write. A hit that
does not match `target_namespace` means cross-namespace forgery or a stale
hint and yields `0x08 pubk_mismatch`.

**Delegate path.** When the owner lookup misses but the signer_hint is
non-zero, the Engine tries the composite-key lookup
`delegation[(target_namespace, signer_hint)]` via
`get_delegate_pubkey_by_hint`. `signer_hint` is definitionally
`SHA3-256(delegate_pubkey)`, so this is a direct point lookup — no iteration.
A hit means "this signer_hint is a delegate of target_namespace"; the Engine
marks `is_owner = false` and proceeds to signature verification with the
recovered delegate signing pubkey.

**No hit in either path.** If both lookups miss, the Engine logs a debug
line and rejects with a dedicated `unknown_signer_hint` error (no wire code
in the 0x07-0x0B range; falls to the generic `malformed_payload (0x01)`
slot).

Caching: blob-hash dedup at Step 0b + Step 2a short-circuits the common
duplicate-traffic case before ever reaching Step 2, so `owner_pubkeys` reads
are not a measured hotspot. No in-process cache today; adding one would
require careful invalidation against the PUBK first-wins rule.

### BOMB Cascade Side-Effect

BOMB is the batched-tombstone primitive (see
[PROTOCOL.md §BOMB Blob Format](PROTOCOL.md#bomb-blob-format)). On ingesting
a BOMB the Engine emits a cascade of single-target deletes; the BOMB blob
itself is stored content-addressed like any other signed blob.

**Ingest-time invariants (Steps 1.7 and 2).**

- `ttl == 0` required — non-zero ttl yields `0x09 bomb_ttl_nonzero` at
  engine.cpp:210. Enforced even for structurally-invalid BOMBs (gate uses
  `has_bomb_magic`, not `is_bomb`, to catch malformed BOMBs in the same
  branch rather than letting them silently pass as opaque blobs).
- Structural validity: `validate_bomb_structure` checks
  `[BOMB:4][count:4 BE][hash:32] × count` size consistency. Failure yields
  `0x0A bomb_malformed` at engine.cpp:216. An empty BOMB (`count = 0`) is
  structurally valid; the cascade loop runs zero iterations.
- Delegate rejection: after `signer_hint` resolution, if the resolved
  `is_owner == false` and the blob is a BOMB, yield `0x0B
  bomb_delegate_not_allowed` at engine.cpp:311. Delegates cannot emit
  batched deletes.

**Cascade side-effect (Step 3.5).** For each target hash in the BOMB payload:

```cpp
for (const auto& target_hash : wire::extract_bomb_targets(blob.data)) {
    storage_.delete_blob_data(target_namespace, target_hash);
    // (tombstone entry is written inside delete_blob_data -> store_blob's
    //  tombstone-side-effect branch when the BOMB itself is stored)
}
```

No per-target existence check — a BOMB can legitimately pre-mark a
not-yet-received blob (distributed write ordering can deliver the BOMB
before the target arrives; the tombstone-first ordering then prevents the
later arrival from being accepted).

The BOMB blob itself is then stored by Step 4 like any other blob: its own
content-hash, its own `sequence` entry, its own `blobs` entry (with magic
`0x424F4D42` in the FlatBuffer `data` field). The per-target tombstone
entries land in the `tombstone` DBI as a side-effect of the cascade loop
above.

**Wire-path note.** BOMBs ship under `TransportMsgType_BlobWrite = 64`, not
`TransportMsgType_Delete = 17`. The Delete dispatcher's input validation
expects a 36-byte single-target tombstone payload (`namespace:32 ||
target_hash:32 || …`) and rejects anything else — BOMBs are a signed blob
with a `[BOMB:4][count:4 BE][hash:32] × N` body and fail that check. Routing
BOMBs through BlobWrite keeps them on the signed-blob ingest path where
Step 1.7 can validate them. This is a load-bearing correctness rule for
external client implementers; see
[PROTOCOL.md §Sending a Blob (BlobWrite = 64)](PROTOCOL.md#sending-a-blob-blobwrite--64)
for the wire-side contract.

### Thread Pool Crypto Offload

Heavy crypto — ML-DSA-87 verification (~5-10 ms) and SHA3-256 content
hashing (sub-ms but on hot paths) — is offloaded to the
`crypto_thread_pool` (`db/crypto/thread_pool.h`). The Engine never computes
signatures or hashes on the io_context thread.

Pattern (canonical; must be followed at every new offload boundary):

```cpp
// 1. Offload heavy work. Resumption may be on any thread pool worker.
auto hash = co_await thread_pool.offload([&]{
    return sha3_256(encoded);
});

// 2. Before touching Storage, post back to ioc_.
co_await asio::post(ioc_, asio::use_awaitable);

// 3. Safe Storage access.
auto result = storage_.store_blob(ns, blob, hash, encoded, ...);
```

The Engine uses two offloads per ingest: one for the content-hash and one
for the bundled `build_signing_input + verify_signature`. Each is followed
by an explicit `asio::post(ioc_, ...)` before any `storage_.*` call. Skipping
either post-back trips `STORAGE_THREAD_CHECK()` in debug builds.

Pool sizing: `worker_threads` config (default `0` → auto-detect via
`std::thread::hardware_concurrency()`). The node clamps explicit settings to
`hardware_concurrency()`. Under-provisioning degrades ingest latency under
load; over-provisioning wastes memory but is otherwise harmless.

Critical invariant (worth repeating for new contributors): **NEVER touch
Storage from inside the offload lambda.** Always post back. The lambda runs
on a pool thread; Storage is thread-confined to the io_context thread.
Violating this is the single most common way new contributors break the
concurrency ship-gate.

### Ingest Error Codes

All user-facing error wording is owned by two places:

- `cli/src/error_decoder.cpp` — CLI-side decoder that turns wire error
  codes into the strings users see.
- [PROTOCOL.md §ErrorResponse (Type 63)](PROTOCOL.md#errorresponse-type-63)
  — the canonical wire table, including the full 0x01-0x0B range and the
  verbatim CLI wording for the drift-detector test case.

The node-side enum constants live in `db/peer/error_codes.h`:

| Wire code | Constant                             | Reject condition (engine.cpp step) |
|-----------|--------------------------------------|-------------------------------------|
| `0x07`    | `ERROR_PUBK_FIRST_VIOLATION`         | Step 1.5                            |
| `0x08`    | `ERROR_PUBK_MISMATCH`                | Step 2 (owner path mismatch)        |
| `0x09`    | `ERROR_BOMB_TTL_NONZERO`             | Step 1.7                            |
| `0x0A`    | `ERROR_BOMB_MALFORMED`               | Step 1.7                            |
| `0x0B`    | `ERROR_BOMB_DELEGATE_NOT_ALLOWED`    | Step 2 (delegate path + BOMB)       |

This table does NOT duplicate the user wording — per D-02, wording lives in
PROTOCOL.md and in the `[error_decoder]` Catch2 TEST_CASE in
`cli/tests/test_wire.cpp` (the literal-equality unit test that catches
drift).

Source-of-truth files for this section:

- `db/engine/engine.cpp` — `BlobEngine::ingest` coroutine and step comments.
- `db/peer/error_codes.h` — wire error-code constants.
- `db/crypto/thread_pool.h` — offload primitive.
- `cli/tests/test_wire.cpp` (`[error_decoder]` TEST_CASE) — CLI-side
  wording drift-detector.

---

## Net Layer

The Net layer owns every byte between the socket and the Engine: framing,
AEAD, handshakes, role signalling, and the peer-side orchestration that
drives sync rounds. All wire-level byte layouts live in
[PROTOCOL.md §Transport Layer](PROTOCOL.md#transport-layer); this section
describes the implementation side.

### Handshake State Machine

Two handshake paths, picked at connect time:

```
  Initiator                                  Responder
  ─────────                                  ─────────
    │  ── TrustedHello(nonce, pubkey) ──────► │   trusted?
    │                                          │   ├─ no  → PQRequired → initiator retries as PQ
    │                                          │   └─ yes → TrustedHello(nonce, pubkey)
    │  ◄─ TrustedHello / PQ fallback ──────── │
    │  ── AuthSignature(sig, role) ─────────► │   verify sig, switch on role
    │  ◄────── session established ────────── │
```

- **PQ path:** ML-KEM-1024 KEM handshake followed by ML-DSA-87 mutual
  authentication. Used by default for untrusted peers and client
  connections over TCP. HKDF derives the session keys from the KEM shared
  secret with an empty salt and the label `"chromatindb-session-v1"` (see
  [PROTOCOL.md §HKDF Label Registry](PROTOCOL.md#hkdf-label-registry)).
- **Lightweight path (trusted peers):** skips the KEM exchange.
  `TrustedHello = [nonce:32][pubkey:2592]` in each direction; HKDF ikm =
  concatenated nonces, salt = concatenated pubkeys. Both sides still prove
  identity with an ML-DSA-87 signature over the transcript. Allowed only
  for localhost or addresses listed in `trusted_peers` config.
- **Fallback:** if the initiator proposes the lightweight path and the
  responder does not trust the initiator's address, the responder replies
  with `PQRequired` and the initiator restarts as a full PQ handshake. This
  is how bootstrap works when both nodes are strangers but one has
  optimistically offered the cheaper path.

**Role signalling.** The `AuthSignature` message carries a one-byte role
enum (`db/net/role.h`):

| Value  | Role       | Status       |
|--------|------------|--------------|
| `0x00` | `Peer`     | implemented  |
| `0x01` | `Client`   | implemented  |
| `0x02` | `Observer` | reserved     |
| `0x03` | `Admin`    | reserved     |
| `0x04` | `Relay`    | reserved     |

Receivers MUST reject unknown values (fail-closed). The role determines
ACL routing after the session is up:
`allowed_peer_keys` for `Peer`, `allowed_client_keys` for `Client`. UDS
connections always declare `Client` regardless of the on-wire byte, since
UDS is local-trust by definition. See
[PROTOCOL.md §Role Signalling](PROTOCOL.md#role-signalling) for the wire
encoding.

**Per-connection post-handshake announcement.** Immediately after any
handshake completes, the node sends `SyncNamespaceAnnounce` (Type 62) so
the peer knows what the local node replicates; the peer MUST drain this
message even if it does not subscribe (the dispatcher swallows it as
informational). See [PROTOCOL.md §SyncNamespaceAnnounce (Type 62)](PROTOCOL.md#syncnamespaceannounce-type-62).

### AEAD Framing and Chunked Sub-Frames

Every post-handshake message is AEAD-framed:

```
  [length_be:4][AEAD_ciphertext:N]
              └───── ChaCha20-Poly1305 with 16-byte tag ─────┘
```

Nonce is a 12-byte counter: `[00 00 00 00][counter_be:8]` per direction,
starting at zero after the session is established. Each direction maintains
its own counter; both sides increment on every frame transmitted in that
direction.

Payloads larger than `STREAMING_THRESHOLD` (1 MiB) use chunked sub-frame
encoding: a single logical message is split into multiple encrypted
sub-frames, each with its own nonce. See
[PROTOCOL.md §Chunked Transport Framing](PROTOCOL.md#chunked-transport-framing)
for the wire bytes.

**Subtle invariant contributors must preserve.** Each connection has a
per-connection send queue (`db/net/connection.h` `send_queue_` +
`drain_send_queue`). Only the drain coroutine touches the socket; every
higher-level sender pushes into the queue and awaits. Without this
serialization, two coroutines on the same connection could race to write
frames and desync the AEAD nonce counter — historically a crash class
fixed by introducing the send queue. **Never bypass the queue.** Never
call `socket.async_write` directly from outside the drain coroutine. Pub/sub
notifications, sync messages, keepalive — all go through the queue.

The receive path is single-reader by construction (one coroutine loops
`async_read` on the socket), so no analogous queue is needed on the inbound
side.

### PeerManager Decomposition

The `PeerManager` facade is a thin composition over six strand-confined
components (v2.2.0 refactor):

| Component            | Responsibility                                             |
|----------------------|------------------------------------------------------------|
| `ConnectionManager`  | own peer sockets; accept, connect, disconnect, reconnect   |
| `MessageDispatcher`  | route inbound `TransportMessage` to handlers by type byte  |
| `SyncOrchestrator`   | Phase A / B / C driver per peer; per-namespace progress    |
| `MetricsCollector`   | strand-confined counters (no atomics; TSAN-clean)          |
| `PexManager`         | inline peer exchange after sync rounds                     |
| `BlobPushManager`    | BlobNotify / BlobFetch push path (primary replication)     |

Wiring: each component holds references to the collaborators it needs;
the `PeerManager` constructor does the injection. The facade preserves the
pre-refactor public API so higher layers (main, signal handlers) did not
need to change.

The strand-confinement pattern extends beyond `Storage` — `MetricsCollector`
for example increments counters on the io_context thread only, allowing
non-atomic `uint64_t` counters without TSAN complaint. Atomics would be
marginally cheaper per-op but lose the ability to reason about snapshot
consistency across related counters.

### Sync Orchestration

Sync runs as a three-phase protocol per peer connection. See
[PROTOCOL.md §Sync Protocol](PROTOCOL.md#sync-protocol).

- **Phase A — namespace exchange.** Both sides send a filtered
  `NamespaceList` (filtered by local `sync_namespaces` config). The
  intersection is the workset for Phase B.
- **Phase B — range-based set reconciliation.** For each namespace in the
  workset, the initiator drives range-split reconciliation: exchange XOR
  fingerprints over sorted content-hash ranges, recursively split mismatched
  ranges, isolate the exact set difference in O(diff) wire traffic.
- **Phase C — blob transfer.** For each missing blob identified in
  Phase B, the peer serves a `BlobTransfer` with a 32-byte
  `target_namespace` prefix followed by the BlobWriteBody envelope. (Post-
  phase-122 change: the prefix replaces the previously-embedded
  per-blob namespace field.) One blob in flight per connection; the
  `blob_transfer_timeout` config (default 600s, see `db/config/config.h:51`)
  caps stalled transfers.

The `cursor` DBI tracks per-peer per-namespace progress: after Phase C the
`SyncOrchestrator` writes `(seq_num, round_count++, last_sync_ts)` so the
next sync round starts from where the last one left off. Safety-net full
reconciliation runs every `full_resync_interval` rounds (default 10) or
after `cursor_stale_seconds` (default 3600) of inactivity, ignoring the
cursor and re-reconciling from scratch — this catches cursor drift and
post-ACL-change reconvergence.

**BlobNotify / BlobFetch is the primary path.** For routine traffic
replication, the writing node pushes a `BlobNotify` as soon as the ingest
commits; subscribers call `BlobFetch` for the full payload. Sync is the
backstop that catches everything the push path missed (packet loss,
reconnects, cursor-skip scenarios). The `BlobPushManager` component owns
the push side; the `SyncOrchestrator` owns the pull-reconciliation side.

### PEX, Reconnect, and Inactivity

**Inline peer exchange (PEX).** After Phase C completes, the
`PexManager` may send a compact peer-list to the counterparty, serialized
on the same connection through the send queue (so it cannot desync AEAD
nonces). Inline-PEX cadence is capped by the `pex_interval` config
(default 300s, `db/config/config.h:51`) to avoid chatty exchanges on
high-churn peers.

**Reconnect with jittered backoff.** When an outbound peer disconnects,
`ConnectionManager` schedules a reconnect with exponential backoff from 1s
to 60s and ±25% jitter. The first reconnect is immediate. Peers that
reject via ACL (connect, handshake, disconnect-before-any-message) get
extended backoff: three consecutive rejections bumps the delay to 600s.
SIGHUP resets both the ACL-rejection counter and the normal backoff
counter, enabling immediate retry after config changes.

**Inactivity timeout.** Receiver-side only: if the inbound socket sees no
messages for `inactivity_timeout_seconds` (default 120, minimum 30 when
enabled), the connection is dropped and its resources freed. No sender-side
Ping/Pong is emitted — bidirectional keepalive would inject write traffic
from multiple coroutines and risk nonce desync (see
[AEAD Framing and Chunked Sub-Frames](#aead-framing-and-chunked-sub-frames)
for the send-queue discipline). Peers kept alive by sync and pub-sub traffic
naturally stay inside the inactivity window.

### Role-Aware Access Control

Once `role` is known at handshake-complete time:

- `Role::Peer` → checked against `allowed_peer_keys` (empty = open).
  Rejected keys get extended backoff per above.
- `Role::Client` → checked against `allowed_client_keys` (empty = open).
- UDS connections → always `Client`, always locally trusted, ACL still
  applies.
- Unknown role byte → reject immediately.

SIGHUP reload disconnects unauthorized peers and also resets ACL
reconnection counters. Operators can tighten a live node's access list
without restarts.

Source-of-truth files for this section:

- `db/net/handshake.cpp` / `handshake.h` — PQ and lightweight state machines.
- `db/net/role.h` — Role enum + `is_implemented_role` fail-closed helper.
- `db/net/connection.h` / `connection.cpp` — per-connection state, send queue.
- `db/net/server.cpp` — accept loop + role-routed ACL.
- `db/peer/peer_manager.{cpp,h}` — facade + component wiring.
- `db/peer/sync_orchestrator.cpp` — Phase A/B/C driver.
- `db/peer/connection_manager.cpp` — reconnect + ACL-aware backoff.
- `db/peer/pex_manager.cpp` — inline PEX serialization.

---

## Configuration and Subsystems

This section is deliberately brief — the operator-facing reference lives in
[db/README.md §Configuration](README.md#configuration). The entries below
describe the implementation side only.

### Configurable Constants

Five operator-tunable knobs cover sync pacing and peer misbehaviour
handling. Source: `db/config/config.h:51-58`.

| Knob                       | Default | Reloadable | Purpose                                         |
|----------------------------|---------|------------|-------------------------------------------------|
| `blob_transfer_timeout`    | 600s    | SIGHUP     | per-blob transfer cap during sync Phase C       |
| `sync_timeout`             | 30s     | SIGHUP     | overall sync-protocol response cap              |
| `pex_interval`             | 300s    | SIGHUP     | inline-PEX minimum period                       |
| `strike_threshold`         | 10      | restart    | strikes allowed before a peer is disconnected   |
| `strike_cooldown`          | 300s    | restart    | seconds before a peer's strike counter resets   |

All values are validated at load: type, range, and coherence checks; any
invalid value fails-fast with an operator-readable error and the node
refuses to start. See `db/config/config.cpp` `validate_config` for the
exact range bounds. `strike_threshold` and `strike_cooldown` are
restart-only because the runtime strike state hangs off the `PeerManager`
and is not safely re-bindable under SIGHUP today.

### Peer Management Subcommands

Three `chromatindb` subcommands maintain the bootstrap peer list without
hand-editing the config:

- `chromatindb add-peer <host:port>` — append to `bootstrap_peers` in the
  running daemon's config file, then SIGHUP it.
- `chromatindb remove-peer <host:port>` — remove matching entries from
  `bootstrap_peers`, then SIGHUP. No format validation on the argument;
  operators can clear pre-existing malformed entries from older add-peer
  bugs.
- `chromatindb list-peers` — prints configured peers (from config) and
  currently connected peers (from the daemon via UDS). The two lists are
  reported separately so operators can spot "configured but not connected"
  at a glance.

Dispatch and implementation: `db/main.cpp` (`cmd_add_peer`,
`cmd_remove_peer`, `cmd_list_peers`). `list-peers` runs as a short-lived
UDS client that connects, declares `Role::Client`, issues `PeerInfoRequest`,
and exits. The running daemon is discovered via the pidfile under
`data_dir`.

### Identity Management

- ML-DSA-87 keypair at `data_dir/node.key` (secret, 0600) and
  `data_dir/node.pub` (public, 0644).
- Namespace derived on load: `SHA3-256(signing_pk)` — the node's own
  namespace for blobs it originates.
- `chromatindb keygen` generates the pair; `--force` overwrites. The node
  refuses to run if the secret file is world-readable or world-writable.

### Metrics and Observability

- `NodeMetrics` is strand-confined — all increments happen on the
  io_context thread; no atomics needed. TSAN-clean by construction.
- `SIGUSR1` dumps a live snapshot to log (via spdlog). Periodic dumps every
  60s run unconditionally.
- Optional Prometheus `/metrics` endpoint: enable by setting
  `metrics_bind` in config to a `host:port` string. SIGHUP reloads the
  bind address. Full metric inventory lives in
  [PROTOCOL.md §Prometheus Metrics Endpoint](PROTOCOL.md#prometheus-metrics-endpoint).

### ACL Model

- `allowed_peer_keys` and `allowed_client_keys` are independent lists of
  64-char hex namespace hashes (SHA3-256 of the declared ML-DSA-87
  pubkey). An empty list means open; a non-empty list means closed except
  for listed keys.
- Role-routed at handshake-complete time (see
  [Role-Aware Access Control](#role-aware-access-control)). A single node
  can be closed to peers but open to clients or vice versa.
- SIGHUP reload disconnects peers removed from the ACL and resets
  ACL-aware reconnection backoff counters (enabling immediate retry for
  previously-blocked keys that have been newly allowed).

Source-of-truth files for this section:

- `db/config/config.h` / `config.cpp` — struct, load, validation.
- `db/main.cpp` — subcommand dispatch.
- `db/peer/metrics_collector.{cpp,h}` — strand-confined counters.
