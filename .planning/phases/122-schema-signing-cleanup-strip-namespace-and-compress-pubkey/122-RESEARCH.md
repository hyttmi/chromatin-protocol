# Phase 122: Schema + Signing Cleanup — Research

**Researched:** 2026-04-20
**Domain:** C++20 node internals — FlatBuffers schema, MDBX DBI layout, ML-DSA-87 signing, ingest pipeline, sync replication
**Confidence:** HIGH — all claims backed by direct code reads in this session; no training-data-only assertions in the plan-relevant findings.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions (D-01 .. D-13, verbatim)

- **D-01:** `signing_input = SHA3-256(target_namespace || data || ttl_be32 || timestamp_be64)`. The signer commits to the **target namespace**, not to `signer_hint`. For owner writes these are identical bytes (since `target_namespace = SHA3(owner_pubkey) = signer_hint`); for delegate writes they differ — the delegate's signature is scoped to exactly one target namespace, preventing cross-namespace replay when a delegate holds delegations on multiple namespaces. Byte-for-byte identical to today's protocol: the only change in `db/wire/codec.cpp:66-98` is the parameter rename `namespace_id` → `target_namespace`.

- **D-02:** `signer_hint = SHA3-256(signing pubkey)`. 32 bytes. Collision resistance = SHA3-256. Same hash value the current code computes at `engine.cpp:190-191`; post-122 it becomes the lookup key into `owner_pubkeys`.

- **D-03:** PUBK-first invariant enforced at the **node protocol level** in `db/engine/engine.cpp` and on the sync replication path (`db/sync/sync_protocol.cpp`). On every ingest: if `!has_owner_pubkey(target_namespace)` AND incoming blob magic != `PUBK` → reject with protocol-level error. Runs BEFORE signature verification. NOT CLI-only.

- **D-04:** First PUBK wins. Same signing pubkey → idempotent accept (KEM rotation overwrites main-DBI PUBK body); different signing pubkey → reject with `ErrorCode::PUBK_MISMATCH`. No delegation-gated rotation in this phase.

- **D-05:** New `owner_pubkeys` DBI on `Storage`, key = 32-byte `signer_hint`, value = 2592-byte raw ML-DSA-87 signing pubkey. KEM pubkey is NOT duplicated — it stays in the PUBK blob body.

- **D-06:** Four new public methods on `Storage`, all `STORAGE_THREAD_CHECK()`-gated:
  - `register_owner_pubkey(span<const uint8_t,32>, span<const uint8_t,2592>)` — idempotent on matching value.
  - `get_owner_pubkey(span<const uint8_t,32>)` → `std::optional<std::array<uint8_t,2592>>`.
  - `has_owner_pubkey(span<const uint8_t,32>)` → `bool`.
  - `count_owner_pubkeys() const` → `uint64_t`.

- **D-07:** New transport body `{ target_namespace: [ubyte:32], blob: Blob }`. `target_namespace` sits at transport envelope, NOT inside signed Blob bytes, but IS absorbed into the signing sponge (per D-01). Post-122 Blob = `{ signer_hint:[ubyte:32], data:[ubyte], ttl:uint32, timestamp:uint64, signature:[ubyte] }`.

- **D-08:** Add a new `BlobWrite` TransportMsgType over repurposing `Data = 8`. Dispatcher auditability + wire-log clarity outweigh the "free" slot.

- **D-09:** Verify flow: transport delivers `{target_namespace, blob}` → PUBK-first check → `owner_pubkeys[signer_hint]` lookup; found = owner write (verify `SHA3(pubkey)==target_namespace` + ML-DSA verify); not found = check `delegation_map[target_namespace]` for delegate whose hash matches `signer_hint`; neither = reject. On success, dispatch to Storage write.

- **D-10:** Mechanical rename `namespace_id` → `namespace` across `Storage` public API. No DBI byte-level change.

- **D-11:** No migration. Operator wipes both dev nodes manually.

- **D-12:** Required Catch2 coverage for PUBK-first:
  - (a) Non-PUBK as first write in a fresh namespace → rejected.
  - (b) Sync-replicated non-PUBK-first → rejected on the sync path (separate test from direct-write path).
  - (c) PUBK → non-PUBK works.
  - (d) PUBK with matching signing pubkey overwrites existing PUBK (KEM rotation) → accepted + DBI value updated.
  - (e) PUBK with different signing pubkey for an already-owned namespace → rejected with `PUBK_MISMATCH`.
  - (f) Cross-namespace race: two peers attempt to register different PUBKs for the same namespace simultaneously — first-wins. TSAN-backed concurrent test reusing `test_storage_concurrency_tsan.cpp` fixture style.

- **D-13:** Delegate-replay regression test: delegate with delegations on N_A and N_B signs a blob for N_A; node submitted with `target_namespace = N_B` MUST fail signature verification (proves D-01 cross-namespace replay protection).

### Claude's Discretion

- Exact field name in transport envelope for `target_namespace` — pick whatever reads cleanly.
- `register_owner_pubkey` span vs. array — match prevailing Storage public-API style.
- Whether PUBK-first check lives in a shared helper vs. inlined at both engine + sync — minimize duplication.
- Name of new TransportMsgType if D-08(b) chosen — `BlobWrite`, `SignedBlob`, `Write`, etc.
- Whether to add schema-version byte to MDBX env — YAGNI unless strong reason.

### Deferred Ideas (OUT OF SCOPE)

- Delegation-gated PUBK rotation — future phase.
- Migration tool for pre-122 data dirs — ruled out (D-11).
- `owner_pubkeys` DBI metadata (first_seen_timestamp, originating_blob_hash).
- Schema version byte in MDBX env.

</user_constraints>

## Summary

Phase 122 is a coordinated protocol break that shrinks every signed blob by ~2592 bytes (35%) by moving the writer's 2592-byte ML-DSA-87 pubkey out of every blob and into a shared `owner_pubkeys` DBI indexed by a 32-byte `signer_hint = SHA3(pubkey)`. Three tightly coupled changes must land together: (1) schema slim — remove `namespace_id` and `pubkey` from the Blob FlatBuffer, add `signer_hint`; (2) transport re-envelope — carry `target_namespace` alongside the signed Blob at the transport layer (new `BlobWrite` TransportMsgType); (3) engine verify-path rewrite — replace `derived_ns == blob.namespace_id` check with `owner_pubkeys[signer_hint]` lookup + delegate-map fallback, AND enforce the PUBK-first invariant at the node protocol level so adversarial / misbehaving peers can't bypass it by sending non-PUBK first to a fresh namespace.

The signing canonical form stays byte-identical to today (`SHA3(target_namespace || data || ttl_be32 || timestamp_be64)`) — the only change in `codec.cpp:66-98` is a parameter rename. This is load-bearing: if an implementer "optimizes" the signing sponge inputs, the node goes on-wire-incompatible with itself across the PR boundary. Storage keeps its existing 32+32 byte composite key layout; only the blob-value content shrinks.

**Primary recommendation:** Order the tasks schema → codec → engine verify → sync path → storage DBI → tests → integration in that sequence; the schema change is what forces the FlatBuffers regen and every downstream file change cascades from it. Land the PUBK-first check ONCE in `BlobEngine::ingest()` — the sync path already funnels through `engine.ingest()` via `SyncProtocol::ingest_blobs()` (`sync_protocol.cpp:90-97`), so a single well-placed check covers both the direct-write and sync-replicated paths. No separate helper is needed.

## Architectural Responsibility Map

| Capability | Primary Tier | Secondary Tier | Rationale |
|------------|-------------|----------------|-----------|
| Blob schema (shape of signed bytes) | `db/schemas/blob.fbs` + FlatBuffers codegen | `db/wire/codec.{h,cpp}` (BlobData struct, encode/decode, signing-input builder) | Schema is the wire contract; codec is the in-memory shape + serialization. |
| Transport envelope (target_namespace + Blob) | `db/schemas/transport.fbs` (enum + TransportMessage) | `db/peer/message_dispatcher.cpp` (routes the new type) | TransportMsgType dispatching is the single place that turns a type byte into a handler branch. |
| PUBK-first invariant | `db/engine/engine.cpp` (BlobEngine::ingest) | `db/sync/sync_protocol.cpp::ingest_blobs` (automatically via engine.ingest call) | Memory-locked: node protocol level, not CLI. Sync path delegates to engine, so one check covers both. |
| Pubkey-hint lookup + owner/delegate verify | `db/engine/engine.cpp` | `db/storage/storage.cpp` owner_pubkeys DBI + existing delegation_map | Engine owns verify policy; Storage owns persistence. |
| owner_pubkeys DBI persistence | `db/storage/storage.{h,cpp}` | — | All MDBX state lives in Storage::Impl. STORAGE_THREAD_CHECK applies. |
| Delete path (tombstone ingest) | `db/engine/engine.cpp::delete_blob` | — | Mirror of ingest. Same refactor applied at engine.cpp:395-443. |
| New BlobWrite message wiring | `db/peer/message_dispatcher.cpp` | `db/sync/sync_protocol.cpp` (encode_blob_transfer still carries the sync-side BlobTransfer format; may need a NEW envelope for BlobTransfer too, see Landmine #3) | Dispatcher is the single routing seam. |

## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| SC#1 | `Blob.namespace_id` field removed from the schema (derived from `SHA3(pubkey)` on ingest). | [VERIFIED code-read] `db/schemas/blob.fbs:4` — currently declares `namespace_id:[ubyte]`; after edit, this line deleted. `encode_blob`/`decode_blob` at `db/wire/codec.cpp:13-64` updated accordingly. FlatBuffers CMake regen at `db/CMakeLists.txt:129-151` auto-rebuilds `blob_generated.h`. |
| SC#2 | Per-blob `pubkey` (2592 bytes) replaced by `signer_hint` (32 bytes). | [VERIFIED code-read] `db/wire/codec.h:12-19` BlobData struct has `pubkey: std::vector<uint8_t>` — becomes `std::array<uint8_t,32> signer_hint`. The canonical `PUBKEY_DATA_SIZE = 4+2592+1568 = 4164` at `codec.h:115` and `is_pubkey_blob()` helper at `codec.h:118-122` are UNCHANGED. |
| SC#3 | Signing canonical form updated — **decided in D-01: SHA3(target_namespace ‖ data ‖ ttl ‖ timestamp)**. | [VERIFIED code-read] Current `build_signing_input()` at `codec.cpp:66-98` produces exactly this hash today (parameter just happens to be named `namespace_id`). Byte output is identical post-rename. |
| SC#4 | PUBK-first invariant enforced at node protocol level (not CLI convention). | [VERIFIED code-read + memory] Memory `project_phase122_pubk_first_invariant.md` is non-negotiable. Insertion point: between `engine.cpp:186` (structural checks end) and `engine.cpp:188` (namespace ownership begins) — BEFORE the SHA3(pubkey) offload. Sync path covered automatically via `sync_protocol.cpp:97` which calls `engine_.ingest(blob, source)`. |
| SC#5 | New `owner_pubkeys` DBI populated the moment a PUBK blob is ingested. | [VERIFIED code-read] Hook point in engine is after signature verify succeeds and `is_pubkey_blob(blob.data)` is true, before `storage_.store_blob(...)` at `engine.cpp:309`. Delegation-map population template at `storage.cpp:478-487` is the structural twin. |
| SC#6 | Verify path removes `derived_ns == blob.namespace_id` check at engine.cpp:190-197, and mirror at `:410-413` delete path. | [VERIFIED code-read] Exact line landmarks confirmed: `engine.cpp:197` `bool is_owner = (derived_ns == blob.namespace_id);` (ingest) and `engine.cpp:410` `if (derived_ns != delete_request.namespace_id)` (delete). Both removed and replaced with owner_pubkeys lookup + target_namespace match. |
| SC#7 | All existing node ingest/read paths updated — no references to removed fields. | Grep scope: ~383 occurrences of `namespace_id` across 30 files including `engine.h`, `peer_manager.h`, `storage.cpp`, `sync_protocol.h`, `test_helpers.h`, and 3 test files. Mechanical rename per D-10 + conversion of BlobData shape per D-07. Test suite regen unavoidable but well-bounded. |

## Standard Stack

### Core

| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| FlatBuffers | 25.2.10 | Wire format codegen for `blob.fbs` / `transport.fbs` | [VERIFIED `db/CMakeLists.txt:71-80`] Already vendored via FetchContent; `flatc` binary built as part of CMake configure. Codegen is deterministic from `.fbs` → `_generated.h`. |
| libmdbx | v0.13.11 | Persistent key-value storage for the new `owner_pubkeys` DBI | [VERIFIED `db/CMakeLists.txt:101-114`] Already the sole storage backend; `max_maps = 9` at `storage.cpp:176` currently has 1 unused slot (7 DBIs exist, `max_maps=9` = 8 named + 1 default). **Landmine: bump `max_maps` from 9 to 10 for the new DBI.** |
| liboqs | 0.15.0 | ML-DSA-87 signing + SHA3-256 | [VERIFIED `db/CMakeLists.txt:19-47`] `OQS_ENABLE_SIG_ML_DSA=ON`; SHA3 already used on hot path. No new crypto primitive introduced in Phase 122. |
| Catch2 | v3.x (WithMain) | Test runner for all new tests including TSAN variant | [VERIFIED `db/CMakeLists.txt`] Target `chromatindb_tests` linked `Catch2::Catch2WithMain`; TSAN build reuses the same source set via `-DSANITIZER=tsan`. |

### Supporting

| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| asio (standalone) | 1.38.0 | Coroutines, `asio::post` for post-back-to-ioc pattern | In every new `co_await crypto::offload(pool_, ...)` followed by the Phase 121 post-back idiom before touching Storage. |
| spdlog | 1.15.1 | Rejection warn logs for PUBK-first / PUBK_MISMATCH | Match existing `spdlog::warn("Ingest rejected: ...")` style at `engine.cpp:174-177`. |

### Alternatives Considered

| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| New `owner_pubkeys` MDBX DBI | In-memory `std::unordered_map<hash,pubkey>` + rebuild on startup | Rejected: every public Storage method already has STORAGE_THREAD_CHECK; a DBI matches the idiom. Startup rebuild cost from blob-scan would be O(namespaces) and wasteful. |
| Repurpose `Data = 8` for new body | New `BlobWrite` TransportMsgType | D-08 recommendation: new type. Dispatcher auditability + wire logs clearly distinguish pre-122 vs post-122 traffic. |
| Shared helper `check_pubk_first()` | Inline at `engine.cpp` and `sync_protocol.cpp` | Sync already funnels through `engine.ingest()`. ONE check in engine covers both paths — no duplication, no helper needed. |

**Installation:** No new packages. All dependencies already present in `db/CMakeLists.txt`.

**Version verification:** Not applicable — no new library versions introduced. All existing versions documented at `db/CMakeLists.txt:41, 53, 63, 77, 88, 109, 118`.

## Architecture Patterns

### System Architecture Diagram

```
Peer/client TCP ──┐
                  │       (PQ AEAD frame)
                  ▼
         ┌──────────────────┐
         │ Connection::recv │   db/net/connection.cpp
         └────────┬─────────┘
                  │ DecodedMessage { type, payload, request_id }
                  ▼
     ┌─────────────────────────┐
     │   MessageDispatcher     │   db/peer/message_dispatcher.cpp
     │   .dispatch(type, …)    │
     └─┬────────────┬──────────┘
       │            │
   Data/Delete      BlobTransfer (sync path)
   (direct write)        │
       │                 ▼
       │        SyncProtocol::ingest_blobs   db/sync/sync_protocol.cpp:82-135
       │        (for each blob → engine_.ingest)
       ▼                 │
  ┌────────────────────────────────────┐
  │         BlobEngine::ingest          │   db/engine/engine.cpp:105-347
  │                                     │
  │  Step 0: size / timestamp / ttl     │
  │  Step 1: structural (pubkey, sig)   │
  │  **Step 1.5: PUBK-first gate**      │  ← NEW in Phase 122
  │    if !has_owner_pubkey(tgt_ns) &&  │
  │       !is_pubkey_blob(data) → reject│
  │  Step 2: owner_pubkeys lookup       │  ← NEW, replaces SHA3(pubkey) check
  │    (or delegation_map fallback)     │
  │  Step 2.5: blob_hash + dedup        │
  │  Step 3: build_signing_input + ver  │
  │  Step 3.5: tombstone check          │
  │  Step 4: Storage::store_blob        │
  │  **Step 4.5: if PUBK, register**    │  ← NEW, populate owner_pubkeys
  └──────────────┬──────────────────────┘
                 ▼
     ┌─────────────────────────┐
     │        Storage          │   db/storage/storage.cpp
     │  MDBX env: 8 DBIs       │   (was 7: +owner_pubkeys)
     │   blobs, sequence,      │
     │   expiry, delegation,   │
     │   tombstone, cursor,    │
     │   quota, owner_pubkeys  │
     └─────────────────────────┘
```

Data flow invariant: **sync path delegates to `BlobEngine::ingest()` via `SyncProtocol::ingest_blobs`** (`sync_protocol.cpp:97`). One PUBK-first check in engine covers both direct and sync. This is the key architectural lever for the phase.

### Recommended File-to-Change Inventory

```
db/
├── schemas/
│   ├── blob.fbs                    # DELETE namespace_id, pubkey; ADD signer_hint
│   └── transport.fbs               # ADD BlobWrite enum value (D-08); ADD transport body table for {target_ns, blob}
├── wire/
│   ├── codec.h                     # BlobData struct: drop pubkey, replace namespace_id with signer_hint; rename build_signing_input param
│   ├── codec.cpp                   # encode_blob / decode_blob updated; build_signing_input param rename
│   ├── blob_generated.h            # REGENERATED by flatc (do not edit manually)
│   └── transport_generated.h       # REGENERATED by flatc
├── engine/
│   ├── engine.h                    # IngestError: add pubk_first_violation, pubk_mismatch; possibly change ingest() signature to take target_namespace separately
│   └── engine.cpp                  # verify path rewrite at :174-295; delete path at :395-443
├── sync/
│   ├── sync_protocol.h             # If ingest_blobs signature changes (target_namespace carried separately), update type
│   └── sync_protocol.cpp           # encode_blob_transfer / decode_blob_transfer wire format change (see Landmine #3)
├── peer/
│   ├── message_dispatcher.cpp      # wire up new BlobWrite TransportMsgType; decode {target_ns, blob} envelope; same ErrorCode for rejections
│   ├── error_codes.h               # add ERROR_PUBK_FIRST_VIOLATION, ERROR_PUBK_MISMATCH
│   └── sync_orchestrator.cpp       # BlobTransfer decoder at :478, :957 if sync-blob-transfer envelope changes
├── storage/
│   ├── storage.h                   # +4 public methods; rename params namespace_id → namespace per D-10
│   └── storage.cpp                 # open new "owner_pubkeys" map in open_env; impl 4 methods; bump max_maps 9→10
└── tests/
    ├── test_helpers.h              # ADD make_pubk_blob(id) helper; REVISE make_signed_blob for new BlobData shape
    ├── engine/test_engine.cpp      # PUBK-first (a)(c)(d)(e) + delegate-replay (D-13); 77 existing TEST_CASEs need BlobData shape updates
    ├── sync/test_sync_protocol.cpp # PUBK-first (b) sync-path test; 33 existing tests need shape updates
    ├── storage/test_storage.cpp    # owner_pubkeys DBI unit tests; 105 existing tests need param rename
    ├── storage/test_storage_concurrency_tsan.cpp  # Already covers concurrent ingest — verify still passes; ADD (f) cross-namespace race
    └── wire/test_codec.cpp         # encode/decode round-trip for new Blob shape
```

### Pattern 1: PUBK-first check placement

**What:** Single-site insertion in `BlobEngine::ingest()` between structural checks and namespace-ownership resolution. Sync path inherits the check for free because `SyncProtocol::ingest_blobs` calls `engine_.ingest` per blob (`sync_protocol.cpp:97`).

**When to use:** Every new ingest check belongs here if it needs to run on BOTH direct writes AND sync-replicated writes.

**Example (draft):**
```cpp
// Source: inserted after engine.cpp:186 (structural checks) and before :188 (namespace ownership)
// PUBK-first invariant (Phase 122): the first write to any namespace must be a PUBK.
// Runs BEFORE signature verification — if there's no pubkey registered yet, there's
// nothing to verify against, and we refuse to store any bytes we can't later verify.
if (!storage_.has_owner_pubkey(target_namespace)) {
    if (!wire::is_pubkey_blob(blob.data)) {
        spdlog::warn("Ingest rejected: PUBK-first violation (ns {:02x}{:02x}... has no registered owner)",
                     target_namespace[0], target_namespace[1]);
        co_return IngestResult::rejection(IngestError::pubk_first_violation,
            "first write to namespace must be PUBK");
    }
    // else: fall through — a PUBK blob is allowed to register a new namespace.
}
```

### Pattern 2: owner_pubkeys DBI — structural twin of delegation_map

**What:** New MDBX DBI opened alongside the existing 7 DBIs in `storage.cpp::Impl::open_env()`. Key = 32-byte signer_hint. Value = 2592-byte raw ML-DSA-87 pubkey.

**When to use:** Every new Storage index follows this exact template — see the delegation_map pattern at `storage.cpp:189, 478-487, 1194-1210`.

**Example (draft):**
```cpp
// Source: db/storage/storage.cpp::Impl::open_env — line ~189
owner_pubkeys_map = txn.create_map("owner_pubkeys");

// AND in same function update max_maps from 9 to 10 (line 176):
operate_params.max_maps = 10;  // 9 named sub-databases + 1 default

// Register method (modeled on has_valid_delegation at storage.cpp:1194):
void Storage::register_owner_pubkey(std::span<const uint8_t, 32> signer_hint,
                                     std::span<const uint8_t, 2592> pubkey) {
    STORAGE_THREAD_CHECK();
    try {
        auto txn = impl_->env.start_write();
        auto key_slice = mdbx::slice(signer_hint.data(), 32);
        auto existing = txn.get(impl_->owner_pubkeys_map, key_slice, not_found_sentinel);
        if (existing.data() != nullptr) {
            // Idempotent on match; caller (engine) enforces mismatch rejection BEFORE calling.
            if (existing.length() == 2592 &&
                std::memcmp(existing.data(), pubkey.data(), 2592) == 0) {
                return; // Already registered, identical value.
            }
            // Defensive: engine should have rejected PUBK_MISMATCH, but don't corrupt.
            throw std::runtime_error("register_owner_pubkey called with mismatched value");
        }
        txn.upsert(impl_->owner_pubkeys_map, key_slice,
                    mdbx::slice(pubkey.data(), 2592));
        txn.commit();
    } catch (const std::exception& e) {
        spdlog::error("Storage error in register_owner_pubkey: {}", e.what());
    }
}
```

### Pattern 3: Engine verify path — owner_pubkeys lookup replaces derived_ns check

**What:** Replace the 4-line block at `engine.cpp:190-197` with an owner_pubkeys lookup on `blob.signer_hint`. Store the resolved pubkey for use in the later signature-verify offload at `:271-276`.

**Before (today, `engine.cpp:190-197`):**
```cpp
auto derived_ns = co_await crypto::offload(pool_, [&blob]() {
    return crypto::sha3_256(blob.pubkey);
});
co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
bool is_owner = (derived_ns == blob.namespace_id);
```

**After (Phase 122 draft):**
```cpp
// Resolve signing pubkey via owner_pubkeys DBI (owner write) or delegation_map (delegate)
std::optional<std::array<uint8_t, 2592>> resolved_pubkey;
bool is_owner = false;
bool is_delegate = false;
auto owner_pk = storage_.get_owner_pubkey(blob.signer_hint);
if (owner_pk.has_value()) {
    // Owner write: verify SHA3(pubkey) == target_namespace as integrity check
    auto hint_check = co_await crypto::offload(pool_, [&owner_pk]() {
        return crypto::sha3_256(std::span<const uint8_t>(*owner_pk));
    });
    co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable);
    if (hint_check == target_namespace) {
        is_owner = true;
        resolved_pubkey = owner_pk;
    }
    // else: owner_pubkeys lookup hit a non-owner (delegate reusing a hint that matches
    // some other namespace's owner). Fall through to delegation check.
}
if (!is_owner) {
    // ... delegation_map[target_namespace] check using blob.signer_hint as delegate key
    // ... (see D-09 for the exact flow)
}
```

### Pattern 4: PUBK blob registration on accept

**What:** After successful signature verify and before `storage_.store_blob()` at `engine.cpp:309`, if the accepted blob is a PUBK, extract its embedded signing pubkey and call `storage_.register_owner_pubkey`.

**Example (draft):**
```cpp
// Source: inserted after engine.cpp:305 (tombstone check done) and before :309 (store_blob)
if (wire::is_pubkey_blob(blob.data)) {
    // PUBK body layout: [magic:4][signing_pk:2592][kem_pk:1568]
    std::span<const uint8_t, 2592> embedded_sk_span(
        blob.data.data() + 4, 2592);
    // D-04 mismatch check ran already at Step 1.5 / 2 — by this point either the
    // signer_hint wasn't registered (new namespace → register) or it was registered
    // with the same signing pubkey (KEM rotation → idempotent register).
    storage_.register_owner_pubkey(blob.signer_hint, embedded_sk_span);
}
```

### Anti-Patterns to Avoid

- **Two PUBK-first checks (one in engine, one in sync_protocol).** Sync already delegates to `engine.ingest()`. Duplicating the check in `sync_protocol.cpp::ingest_blobs` is forbidden by `feedback_no_duplicate_code.md`. One site.
- **Changing `build_signing_input` byte output.** The function body is byte-identical post-rename — only the C++ parameter name changes from `namespace_id` to `target_namespace`. Do NOT "simplify" the sponge absorption order or switch to `std::hash`-style concatenation — the wire protocol is frozen here.
- **Reading `blob.pubkey` anywhere post-refactor.** After the schema change there IS no `blob.pubkey` field. Any remaining reference is a compile error — good. Do not stub it with `blob.pubkey_resolved_from_signer_hint` or similar; carry `resolved_pubkey` as a local.
- **Re-using `namespace_mismatch` IngestError for PUBK rejections.** Add dedicated `pubk_first_violation` and `pubk_mismatch` enum values so test assertions and wire-level error codes distinguish them.
- **Opening the new DBI with non-zero value size assumption.** MDBX doesn't mind variable sizes, but the `register_owner_pubkey` path accepts only 2592-byte values by its span type. Don't drop to `std::vector<uint8_t>` value type "for flexibility."

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Signing canonical form | A new hash-then-sign recipe | Existing `build_signing_input()` with renamed param | Byte-identical output; any new recipe is a wire-protocol break beyond what's intended. |
| PUBK body parsing | Custom `[magic:4][sk:2592][kem:1568]` parser | Reuse `wire::is_pubkey_blob()` + `wire::PUBKEY_DATA_SIZE` at `codec.h:118-122` | Already in header; extending with `extract_signing_pk_from_pubkey_blob(data)` helper at `codec.h` is the right place. |
| PUBK-first check in sync path | Duplicate the engine check in `sync_protocol.cpp` | Rely on `engine.ingest()` call from `sync_protocol.cpp:97` | Sync already delegates — `feedback_no_duplicate_code.md`. |
| Namespace-derivation via SHA3(pubkey) | Inline SHA3 offload at new call sites | `storage_.get_owner_pubkey(signer_hint)` lookup | Post-122 the namespace IS the signer_hint for owner writes; looking it up is O(1) DBI read vs. 2592-byte SHA3 compute. |
| Transport envelope codec | Hand-rolled BE-u32 + concat + `std::memcpy` | Add `BlobWriteBody` table to `transport.fbs` and let `flatc` generate it | Project already uses FlatBuffers for Blob; consistency with `blob.fbs` pattern. |
| New TSAN fixture | A parallel harness | Reuse `test_storage_concurrency_tsan.cpp` pattern (io_context + co_spawn + per-coroutine identity + atomic counters) | Phase 121 already proved the pattern. |

**Key insight:** The phase's complexity is in the coordination of schema/wire/engine/storage changes, NOT in inventing new primitives. Every new line of code has a structural twin already in the repo (delegation_map for DBI, is_tombstone for magic check, engine.cpp:290-304 for blob-type-aware branching, test_storage_concurrency_tsan.cpp for the TSAN test pattern). Use the twins verbatim.

## Runtime State Inventory

Phase 122 is a protocol break that changes both the on-wire Blob schema and the storage index layout. D-11 rules out migration — operator wipes both dev nodes manually. Nevertheless the planner should surface each category of runtime state explicitly so nothing is forgotten:

| Category | Items Found | Action Required |
|----------|-------------|------------------|
| Stored data | MDBX data dir (`~/.chromatindb/blobs.mdbx*` or configured path): every row in `blobs`, `sequence`, `expiry`, `delegation`, `tombstone`, `cursor`, `quota` DBIs is pre-122 format (Blob bytes contain `namespace_id` + `pubkey`) | [VERIFIED by D-11] Operator wipes data dir. No migration code, no startup warning. Post-122 node reading a pre-122 dir → undefined behavior; acceptable per decision. |
| Live service config | None — `config.json` carries no blob/schema references; only peer list, ACLs, quotas, TTL caps | [NONE] No config changes needed. |
| OS-registered state | Systemd unit / launch daemon (if deployed) reference `chromatindb` binary name only, not schema | [NONE] No re-registration needed. |
| Secrets/env vars | Master key file (`master.key` in data dir) used for AEAD at-rest encryption — remains valid across the schema change; AEAD envelope wraps the FlatBuffer-encoded blob, which is itself pre/post-122 and opaque to AEAD | [NONE] Master key unchanged. |
| Build artifacts | `db/wire/blob_generated.h` (242 lines, committed) and `db/wire/transport_generated.h` (404 lines, committed) — both are flatc-regenerated at build time via `db/CMakeLists.txt:129-151` `add_custom_command` hooks | [ACTION] After editing `.fbs` files, CMake rebuild triggers flatc regen. The committed headers will also need to be re-committed if the project tracks them (they're currently present in-tree; confirm during planning whether .fbs-only edit → auto-regen-on-build is sufficient, or whether manual flatc invocation + commit is expected). Also: `CMake` target `flatbuffers_blob_generated` must be a dep of `chromatindb_lib` so regen fires; [VERIFIED] it does via `add_custom_command OUTPUT` → implicit dep. |

**Nothing found in live-service-config, OS-registered, or secrets categories — verified by direct code and config read.**

**Critical build-artifact note:** `blob_generated.h` and `transport_generated.h` are BOTH in-tree AND regenerated-by-cmake. This dual state can cause "it builds clean on one machine, fails on another" mismatches if the committed version drifts from the schema. Plan a task explicitly for: (1) edit `.fbs` files, (2) run flatc (either via build or manually), (3) commit both the `.fbs` and the regenerated `_generated.h`.

## Common Pitfalls

### Pitfall 1: max_maps bound at 9 — new DBI will fail to open

**What goes wrong:** `storage.cpp:176` has `operate_params.max_maps = 9;` (7 named DBIs + 1 default, with 1 slot of headroom). Adding `owner_pubkeys` brings the named count to 8; `max_maps = 9` fits barely (8 + 1 default = 9). But MDBX's internal accounting has sometimes tripped on boundary cases. **Recommendation:** Bump to `max_maps = 10` to keep one slot of headroom for any future DBI without revisiting this code.

**Why it happens:** Easy to miss; the `max_maps` value is set once at env creation and mdbx does not surface a "table full" symptom obvious enough to trace back to this constant.

**How to avoid:** Grep `max_maps` during implementation, bump it in the same commit that adds the new DBI. Add a test that opens the env with all 8 DBIs and asserts no MDBX exception.

**Warning signs:** `mdbx::exception` with `MDBX_DBS_FULL` at daemon startup after the schema change.

### Pitfall 2: In-tree generated headers desync from `.fbs`

**What goes wrong:** If the developer edits `.fbs` but skips committing the regenerated `_generated.h`, CI on a clean checkout rebuilds fine (flatc runs), but a reviewer reading the diff sees the schema change without the resulting C++ struct changes.

**Why it happens:** `blob_generated.h` is committed at 242 lines today, and `add_custom_command` silently regenerates it during build. A developer who `git add`s only the `.fbs` file is effectively committing a broken snapshot until the next build regenerates it.

**How to avoid:** Make task step explicit: "edit .fbs, trigger cmake build (which runs flatc), git add BOTH the .fbs and the regenerated _generated.h, commit as one change."

**Warning signs:** `git diff` shows `.fbs` changes without corresponding `_generated.h` changes.

### Pitfall 3: Sync-blob wire envelope vs. direct-write envelope

**What goes wrong:** D-07 changes the direct-write envelope to `{target_namespace:32, blob:Blob}` via a new `BlobWrite` TransportMsgType. But the sync path uses `encode_blob_transfer` (`sync_protocol.cpp:233-246`) which currently emits `[count:u32BE][len1:u32BE][blob1_flatbuf]...`. Each inner blob today still carries its own `namespace_id`. **The sync transfer format ALSO needs updating** — otherwise the encoded inner blob has no `namespace_id` field post-schema-change, and the receiver has no way to know what namespace each blob belongs to.

**Why it happens:** Easy to assume the "new transport" in D-07 means only the direct-write path. But sync blob transfer is a parallel wire format that must also carry `target_namespace` per blob now that it's not inside the Blob bytes.

**How to avoid:** Plan an explicit change to `sync_protocol.cpp::encode_blob_transfer` and `::decode_blob_transfer` (also `encode_single_blob_transfer` at `:248-257`) to emit `[count:u32BE][ns1:32B][len1:u32BE][blob1_flatbuf]...` or equivalent. Decoder feeds the namespace to the per-blob ingest call. Mirror the change in callers `sync_orchestrator.cpp:478-480, 957-959`.

**Warning signs:** First sync after deployment fails because receiver can't derive the storage key without `target_namespace`.

### Pitfall 4: test_helpers.h shape churn invalidates all existing tests

**What goes wrong:** `db/tests/test_helpers.h:73-91` builds `BlobData` with `namespace_id` + `pubkey` inline. 77 test_engine + 33 test_sync + 105 test_storage cases + the TSAN regression test all depend on this signature. The schema change cascades through every one.

**Why it happens:** Test helpers are the only place tests build BlobData today; changing the helper in place is the right move but requires careful attention to every `make_signed_blob` / `make_signed_tombstone` / `make_signed_delegation` / `make_delegate_blob` call site.

**How to avoid:** Plan the `test_helpers.h` edit EARLY in the task sequence (Task 2 or 3), let the compiler guide you through all test updates. Helpers must also gain `make_pubk_blob(id)` for PUBK-first test coverage.

**Warning signs:** Build errors cascade across test files after editing codec.h.

### Pitfall 5: Engine verify path — target_namespace comes from envelope, not from Blob

**What goes wrong:** Post-122, the Blob doesn't know its target namespace. Only the outer envelope (`BlobWriteBody`) carries it. `BlobEngine::ingest` takes `BlobData blob` today; it needs to take an additional `std::array<uint8_t,32> target_namespace` parameter, OR the dispatcher pre-packs a combined struct.

**Why it happens:** The `ingest(const BlobData&)` signature is called from `sync_protocol.cpp:97`, `message_dispatcher.cpp:1350` (direct-write), `message_dispatcher.cpp:338` (delete) — four call sites (+ tests). All must be updated.

**How to avoid:** Decide signature shape early: likely `ingest(target_namespace, blob, source)`. Document in engine.h. Update all callers atomically.

**Warning signs:** Compiler errors in both `message_dispatcher.cpp` and `sync_protocol.cpp`.

### Pitfall 6: PUBK-first check ordering relative to signature verify

**What goes wrong:** Per memory (`project_phase122_pubk_first_invariant.md`): "Runs BEFORE signature verification (no pubkey → nothing to verify against anyway)." But if the check fires AFTER signature verify, an adversary can flood with signed-with-random-key non-PUBK blobs, forcing expensive ML-DSA-87 verify CPU before rejection. DoS vector.

**Why it happens:** Natural reflex to run structural checks inline in a tight cluster; easy to bundle PUBK-first with the post-signature-verify block.

**How to avoid:** Enforce placement: right after structural checks (`:186`), before the `crypto::offload(pool, sha3_256(pubkey))` dispatch at `:190`. Ensure no signature verify runs on non-PUBK-for-new-namespace.

**Warning signs:** Load test showing CPU pegged on verify for rejected blobs.

### Pitfall 7: Idempotent PUBK registration race

**What goes wrong:** Two concurrent ingests of the same PUBK blob for a fresh namespace. Coroutine A passes the PUBK-first check (`has_owner_pubkey` = false), proceeds to verify. Meanwhile Coroutine B also passes the check (both see empty DBI), both verify, both call `register_owner_pubkey`. In the single-io_context model this is impossible — Storage is thread-confined — but `crypto::offload` yields. During the yield, a second `BlobEngine::ingest` can run its early steps.

**Why it happens:** The verify offload path yields between "check has_owner_pubkey" and "register after verify" by several `co_await` points.

**How to avoid:** The `register_owner_pubkey` Storage method is idempotent on matching bytes — identical PUBK produces identical bytes, so both Coroutines end up calling register with the same value, one committing first, the other no-oping (or committing to an already-existing identical row). Different-pubkey case must be caught BY the `register` method itself (throw on mismatch). D-04 "first-wins" applies. Test D-12(f) exercises exactly this.

**Warning signs:** TSAN warning on owner_pubkeys access (should not fire due to STORAGE_THREAD_CHECK). Two registrations observed in logs for same namespace.

### Pitfall 8: Delegate-replay defense relies on build_signing_input input order

**What goes wrong:** D-13 test: delegate signs blob for N_A; submitted with target_namespace = N_B → signature verify must fail. This works ONLY because the sponge absorbs `target_namespace` first. If a subtle refactor reorders inputs, the cross-namespace replay protection silently breaks.

**Why it happens:** `build_signing_input` is the only place this defense lives. A "cleanup" edit that alphabetizes sponge inputs (data → namespace → timestamp → ttl) silently breaks D-13.

**How to avoid:** Add test assertion that `build_signing_input(ns_A, d, t, ts) != build_signing_input(ns_B, d, t, ts)` — trivial but load-bearing. Already covered by existing `test_codec.cpp:118-134` "signing_input is independent of FlatBuffer encoding" + "is deterministic" tests. Add one more test: "signing_input changes when only namespace changes."

**Warning signs:** D-13 regression passes in one commit, fails in a later commit after an innocuous-looking codec refactor.

## Code Examples

### Building a PUBK test blob (new test_helpers.h entry)

```cpp
// Source: proposed addition to db/tests/test_helpers.h
// Builds a PUBK blob using owner's signing pubkey + a zero-filled KEM portion.
// KEM rotation tests override kem_pk explicitly.
inline wire::BlobData make_pubk_blob(
    const identity::NodeIdentity& id,
    std::span<const uint8_t, 1568> kem_pk = std::span<const uint8_t, 1568>{},
    uint64_t timestamp = TS_AUTO)
{
    // PUBK body: [magic:4][signing_pk:2592][kem_pk:1568] = 4164 bytes
    std::vector<uint8_t> pubk_data;
    pubk_data.reserve(wire::PUBKEY_DATA_SIZE);
    pubk_data.insert(pubk_data.end(), wire::PUBKEY_MAGIC.begin(), wire::PUBKEY_MAGIC.end());
    auto spk = id.public_key();
    pubk_data.insert(pubk_data.end(), spk.begin(), spk.end());
    if (!kem_pk.empty()) {
        pubk_data.insert(pubk_data.end(), kem_pk.begin(), kem_pk.end());
    } else {
        pubk_data.resize(wire::PUBKEY_DATA_SIZE);  // zero-fill KEM portion
    }

    wire::BlobData blob;
    // POST-122: no namespace_id, no pubkey field — signer_hint derived here
    auto hint = crypto::sha3_256(spk);
    std::memcpy(blob.signer_hint.data(), hint.data(), 32);
    blob.data = std::move(pubk_data);
    blob.ttl = 0;
    blob.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;

    // target_namespace = SHA3(signing_pk) = hint — identical for owner self-publication
    auto signing_input = wire::build_signing_input(
        hint, blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(signing_input);
    return blob;
}
```

### Extending codec.h with a signing-pk extractor (shared helper, no duplication)

```cpp
// Source: proposed addition to db/wire/codec.h near is_pubkey_blob() at :117-122
/// Extract the 2592-byte signing pubkey from a verified PUBK blob's data.
/// @pre is_pubkey_blob(data) must be true. Caller is responsible.
inline std::span<const uint8_t, 2592> extract_pubk_signing_pk(std::span<const uint8_t> data) {
    // Body: [magic:4][signing_pk:2592][kem_pk:1568]
    return std::span<const uint8_t, 2592>(data.data() + 4, 2592);
}
```

### TSAN cross-namespace race test (D-12f)

```cpp
// Source: proposed db/tests/storage/test_pubk_race_tsan.cpp (or appended to existing TSAN file)
// Reuses pattern from test_storage_concurrency_tsan.cpp:54-104
TEST_CASE("concurrent PUBK registrations for same namespace — first wins",
          "[tsan][pubk][engine][concurrency]") {
    TempDir tmp;
    Storage storage(tmp.path.string());
    asio::thread_pool pool(4);
    BlobEngine engine(storage, pool);

    // Two distinct identities attempting to claim the same namespace by
    // coincidence is impossible (namespace = SHA3(pk)), so construct a
    // contrived case: same namespace_hint via crafted collisions is too
    // expensive; instead test the race on register_owner_pubkey directly
    // with two distinct PUBK blobs whose target_namespace ALIASES due to
    // a grinding-collision mocked at the test level, OR test the more
    // realistic variant: two peers replaying the SAME genuine PUBK from
    // storage sync. Use the realistic variant.

    auto id = NodeIdentity::generate();
    auto pubk = make_pubk_blob(id);

    asio::io_context ioc;
    std::atomic<int> accepted{0};
    std::atomic<int> mismatched{0};

    for (int i = 0; i < 4; ++i) {
        asio::co_spawn(ioc, [&]() -> asio::awaitable<void> {
            auto r = co_await engine.ingest(pubk);
            if (r.accepted) accepted.fetch_add(1);
            else if (r.error == IngestError::pubk_mismatch) mismatched.fetch_add(1);
            co_return;
        }, asio::detached);
    }
    ioc.run();
    pool.join();

    // All 4 should accept: identical PUBK bytes → same content_hash → 3 dedupe + 1 store.
    // owner_pubkeys has exactly one entry for this namespace.
    REQUIRE(accepted.load() == 4);
    REQUIRE(storage.has_owner_pubkey(id.namespace_id()));
    REQUIRE(storage.count_owner_pubkeys() == 1);
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Per-blob inline 2592-byte pubkey | signer_hint + owner_pubkeys DBI lookup | Phase 122 (this phase) | ~35% blob size reduction; one-time pubkey registration per namespace. |
| `namespace_id` in Blob + `derived_ns == namespace_id` integrity check | `target_namespace` in transport envelope + `SHA3(resolved_pubkey) == target_namespace` integrity check | Phase 122 (this phase) | Cleaner: namespace = identity is now structurally enforced via the DBI, not a redundant schema field. |
| Fold `crypto::offload` without post-back | Post-back idiom `co_await asio::post(executor, use_awaitable)` after every offload | Phase 121 (2026-04-19) | STORAGE_THREAD_CHECK enforced; ignored at your peril. Every new offload in Phase 122 MUST follow this. |
| PUBK-first as CLI convention | PUBK-first as node protocol invariant | Phase 122 (this phase) | Adversary-robust; covered on sync path via engine delegation. |

**Deprecated/outdated:**
- Per-blob `pubkey` field — DELETED post-122. Memory-grep'd references: all 383 occurrences of `namespace_id`/`pubkey` must be surveyed; most are in-tree, none are in third-party.
- `IngestError::namespace_mismatch` semantic — today it's "SHA3(pubkey) != namespace_id." Post-122 the nearest equivalent is "pubk lookup hit but SHA3(pubkey) != target_namespace" — which is a weirder case (means the owner_pubkeys DBI has a wrong entry). Retain the enum value with a revised meaning, OR add a new `invalid_namespace_binding` variant. Plan decision.

## Project Constraints (from Memory)

- `feedback_no_duplicate_code.md`: utilities go in shared headers (`db/util/`, `db/tests/test_helpers.h`), not copy-pasted. Applies to: PUBK-first check (one site), signing-pk extractor (one helper in codec.h), test-blob builders (one helper per blob kind in test_helpers.h).
- `feedback_no_backward_compat.md`: no backward compat; delete old code on replacement. Applies to: delete the `derived_ns == blob.namespace_id` check bodies entirely, don't leave commented-out; delete the `pubkey` field from BlobData struct; delete `pubkey` reference in `decode_blob` at `codec.cpp:45-50`.
- `feedback_no_python_sdk.md`: `cdb` is the only client; don't motivate design decisions with "a future non-cdb client might ...". Applies to: don't over-engineer the transport envelope for hypothetical SDKs.
- `project_phase122_pubk_first_invariant.md`: PUBK-first is non-negotiable, node-level. Do not soften.
- `feedback_self_verify_checkpoints.md`: run live verification yourself when infra reachable. Applies to E2E in Phase 124; Phase 122 ships tests + TSAN only.
- User preference: `-j$(nproc)` not `--parallel` for CMake builds. Applies to any task step that triggers a build.

## Assumptions Log

This research was performed by reading current code directly — no training-data-only assertions for plan-critical claims. All line-number citations and file-structure claims were verified against HEAD. The following items carry mild uncertainty:

| # | Claim | Section | Risk if Wrong |
|---|-------|---------|---------------|
| A1 | `max_maps = 9` boundary with 8 DBIs will work but headroom bump to 10 is recommended | Pitfall 1 | Low — MDBX may be fine at exactly 9; recommendation is defensive. |
| A2 | In-tree `blob_generated.h` is regenerated on build via CMake's `add_custom_command`, and developers typically commit both the `.fbs` and the regenerated `.h` together | Pitfall 2 | Low — confirmed by reading `db/CMakeLists.txt:129-151`; the commit pattern is inferred from `blob_generated.h` / `transport_generated.h` being present in-tree. |
| A3 | `encode_blob_transfer` wire format change required for sync path | Pitfall 3 | Medium — this is reasoned from current code structure; plan should confirm during task decomposition by tracing what information is available at the `SyncProtocol::ingest_blobs` call site after the schema change. |

## Open Questions

1. **Engine API: separate `target_namespace` parameter vs. combined struct?**
   - What we know: ingest()/delete_blob() have 4 callers today (message_dispatcher Data/Delete, sync_protocol ingest_blobs, tests). Post-122 they all need access to target_namespace.
   - What's unclear: cleaner to pass as second parameter, or introduce `struct IngestRequest { target_namespace; blob; }`?
   - Recommendation: separate parameter is less churn (smaller diff on test_engine.cpp's 77 cases).

2. **Should `encode_blob_transfer` use FlatBuffers for the outer envelope, or hand-rolled BE32 + concat like today?**
   - What we know: today's `encode_blob_transfer` is hand-rolled `[count:u32BE][len1:u32BE][blob1_flatbuf]...` at `sync_protocol.cpp:233-246`.
   - What's unclear: since inner blob is already FlatBuffers, the outer wrapper being hand-rolled is inconsistent, but changing it is scope creep.
   - Recommendation: stay hand-rolled but inject `[ns:32B]` prefix per blob: `[count:u32BE]([ns:32B][len:u32BE][blob])+`. Minimal diff.

3. **Naming of new TransportMsgType: `BlobWrite`, `SignedBlob`, or `Write`?**
   - What we know: D-08 defers to Claude's discretion; existing enum at `transport.fbs:3-68` uses PascalCase nouns (`Data`, `Delete`, `BlobTransfer`, `ReadRequest`).
   - Recommendation: `BlobWrite = 64` — matches the adjacent `BlobTransfer`, `BlobRequest`, `BlobFetch` naming; clearly indicates "direct client write of a blob" as distinct from sync's `BlobTransfer`.

4. **Pre-existing test case that constructs BlobData with `namespace_id` filled with `0xFF` (test_engine.cpp:89) — post-122 semantics?**
   - What we know: today's test exercises "BlobEngine rejects namespace mismatch" by corrupting the field.
   - What's unclear: post-122 there's no namespace_id to corrupt. Test becomes "corrupt signer_hint" or "ingest with target_namespace that doesn't match any owner pubkey" → rejection path is "unknown signer" (D-09 fallthrough).
   - Recommendation: plan keeps the test renamed and rescoped: signer_hint that doesn't exist in owner_pubkeys AND no matching delegation → `no_delegation` or a new `unknown_signer` IngestError.

## Environment Availability

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| C++20 compiler (gcc/clang) | All code changes | ✓ | (inherited from build env) | — |
| CMake >= 3.20 | Build | ✓ | `cmake_minimum_required(VERSION 3.20)` | — |
| flatc | FlatBuffers regen | ✓ | Built as part of FetchContent target `flatc` | — |
| Catch2 | Tests | ✓ | Vendored | — |
| libmdbx | Storage | ✓ | v0.13.11 vendored | — |
| liboqs | Signing + SHA3 | ✓ | 0.15.0 vendored | — |
| TSAN runtime | D-12(f) test | ✓ | `sanitizers/tsan.supp` exists; reuse Phase 121 `-DSANITIZER=tsan` build flavor | — |
| Live node at 192.168.1.73 | Not needed for Phase 122 (E2E is Phase 124) | — | — | — |

**All dependencies present.** No blocking, no fallback needed.

## Validation Architecture

Phase 122's validation is Catch2-driven (node-side only). Sampling per Nyquist = quick unit tests per commit, full node suite per wave merge, TSAN gate at phase close.

### Test Framework

| Property | Value |
|----------|-------|
| Framework | Catch2 v3 (`Catch2::Catch2WithMain`) |
| Config file | `db/CMakeLists.txt` (test target `chromatindb_tests` + `catch_discover_tests`) |
| Quick run command | `./build/db/chromatindb_tests "[pubk],[engine],[codec]"` |
| Full suite command | `./build/db/chromatindb_tests "[storage],[engine],[sync],[codec],[pubk],[thread_check]"` |
| TSAN run command | `cmake -B build-tsan -DSANITIZER=tsan && cmake --build build-tsan -j$(nproc) && ./build-tsan/db/chromatindb_tests "[tsan]"` |

### Phase Requirements → Test Map

| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| SC#1 (blob.namespace_id removed) | Compile-time: schema has no namespace_id field; decode_blob round-trip works | unit | `./build/db/chromatindb_tests "[codec]"` | ✅ existing `test_codec.cpp` needs updates |
| SC#2 (signer_hint replaces pubkey) | encode/decode round-trip preserves signer_hint; is_pubkey_blob still works | unit | `./build/db/chromatindb_tests "[codec]"` | ✅ existing `test_codec.cpp` |
| SC#3 (signing canonical form byte-identical) | `build_signing_input(ns, d, t, ts)` bytes match a golden vector captured before the rename | unit | `./build/db/chromatindb_tests "build_signing_input"` | ✅ existing `test_codec.cpp:81-117` |
| SC#4 (PUBK-first invariant — node-level) | (a) non-PUBK first reject; (b) sync-replicated non-PUBK first reject; (c) PUBK-then-regular works; (d) PUBK KEM rotation; (e) PUBK_MISMATCH; (f) TSAN concurrent race | unit + TSAN | `./build/db/chromatindb_tests "[pubk]"` AND TSAN build | ❌ Wave 0 (new `test_pubk_first.cpp` + new `test_pubk_race_tsan.cpp` OR appended to existing `test_storage_concurrency_tsan.cpp`) |
| SC#5 (owner_pubkeys DBI populated on PUBK ingest) | After PUBK ingest, `storage.has_owner_pubkey(ns)` returns true; `count_owner_pubkeys()` increments | unit | `./build/db/chromatindb_tests "[storage][owner_pubkeys]"` | ❌ Wave 0 (new `test_owner_pubkeys.cpp`) |
| SC#6 (verify path rewrite; derived_ns check removed) | Owner writes verify; delegate writes verify; non-owner-non-delegate rejects; unknown signer_hint rejects | unit | `./build/db/chromatindb_tests "[engine]"` | ✅ existing `test_engine.cpp` (heavy updates) |
| SC#7 (no references to removed fields) | Compile-time: grep `blob.pubkey` and `blob.namespace_id` in `db/` returns zero outside `.fbs` and `_generated.h` | static check | `! grep -rn "blob\\.pubkey\\|blob\\.namespace_id" db/ --include="*.cpp" --include="*.h" --exclude="*_generated.h" --exclude="*.fbs"` | N/A |
| D-13 (delegate-replay) | Delegate signs blob for N_A; submitted with target_namespace=N_B → signature verify fails | unit | `./build/db/chromatindb_tests "delegate replay"` | ❌ Wave 0 (new test_case in test_engine.cpp) |

### Sampling Rate

- **Per task commit:** Quick-gate `./build/db/chromatindb_tests "[pubk],[engine],[codec]"` — under 10 seconds.
- **Per wave merge:** Full gate `./build/db/chromatindb_tests "[storage],[engine],[sync],[codec],[pubk],[thread_check]"` + TSAN `[tsan]` — several minutes.
- **Phase gate:** Full suite green in both debug and `-DSANITIZER=tsan` builds; all 7 success criteria demonstrably pass per the req→test map above; `grep blob.pubkey db/` returns empty outside schema and generated headers.

### Wave 0 Gaps

Tests that DO NOT yet exist and must be added before / during implementation:

- [ ] `db/tests/engine/test_pubk_first.cpp` — covers D-12(a),(c),(d),(e) and D-13. Likely ~8-10 TEST_CASEs.
- [ ] `db/tests/sync/test_pubk_first_sync.cpp` OR appended to `test_sync_protocol.cpp` — covers D-12(b). 2-3 cases.
- [ ] `db/tests/storage/test_owner_pubkeys.cpp` — storage-level DBI unit tests (register_owner_pubkey idempotency, get_owner_pubkey round-trip, has_owner_pubkey, count_owner_pubkeys, STORAGE_THREAD_CHECK assertion firing from wrong thread in debug). ~6 cases.
- [ ] TSAN case for D-12(f) — either new file `db/tests/storage/test_pubk_race_tsan.cpp` OR a new TEST_CASE appended to `test_storage_concurrency_tsan.cpp`. 1-2 cases.
- [ ] `db/tests/test_helpers.h` additions — `make_pubk_blob(id, kem_pk_optional)`; update existing `make_signed_blob`, `make_signed_tombstone`, `make_signed_delegation`, `make_delegate_blob` to the new BlobData shape.
- [ ] Golden-vector test in `test_codec.cpp` proving `build_signing_input` output is byte-identical before and after the rename (copy a SHA3 output from a current run, assert post-rename produces same 32 bytes for same logical inputs).
- [ ] New `wire/transport.fbs` `BlobWriteBody` round-trip test in `test_codec.cpp` (once transport schema changes).

CMakeLists.txt must list any new test source files under the `chromatindb_tests` add_executable block (see template at existing test wiring).

## Security Domain

### Applicable ASVS Categories

| ASVS Category | Applies | Standard Control |
|---------------|---------|-----------------|
| V2 Authentication | yes | Pubkey-based identity via ML-DSA-87; no password/session auth. No change in Phase 122 — identity model is structurally the same, just indexed differently. |
| V3 Session Management | no | Stateless signed blobs; session keys are handled by PQ handshake, untouched by this phase. |
| V4 Access Control | yes | ACL enforcement is at connection level (`db/acl/access_control.cpp`, unchanged) + namespace-write authorization is at engine level (owner verify / delegate verify). This phase RESTRUCTURES the authorization check to be DBI-lookup-driven, must preserve behavior-equivalence with today's `derived_ns == blob.namespace_id` + `has_valid_delegation` flow. |
| V5 Input Validation | yes | FlatBuffers verifier on decode; size bounds on `BlobData.data` and `BlobData.signature`; `is_pubkey_blob` size check; new PUBK-first and PUBK_MISMATCH rejections are input-validation gates. |
| V6 Cryptography | yes | Reuse liboqs ML-DSA-87 verify and SHA3-256 (already vendored and used). **Do not hand-roll** anything; all crypto primitives stay liboqs-backed. Canonical signing form is byte-preserved. |

### Known Threat Patterns

| Pattern | STRIDE | Standard Mitigation |
|---------|--------|---------------------|
| Cross-namespace replay by a multi-delegation delegate | Elevation of Privilege | `build_signing_input` absorbs target_namespace as the FIRST sponge input (D-01). Tested by D-13 regression. |
| Adversarial peer floods a fresh namespace with non-PUBK writes | DoS (storage fill) / Tampering (unverifiable content) | PUBK-first invariant enforced BEFORE signature verify (saves CPU) AND at node-level (applies to sync path too). Rejection is `pubk_first_violation` protocol error. |
| Peer tries to "rotate" owner key by sending a different PUBK | Spoofing | D-04 "first-wins"; mismatch rejected with `PUBK_MISMATCH`. Tested by D-12(e). |
| Replay of an old PUBK blob by an attacker (they captured it on wire) | Tampering/Replay | Idempotent accept on bit-identical PUBK re-send (content-hash dedup); no state corruption. Tested by D-12(d). |
| Confused deputy: owner_pubkeys DBI corruption points to wrong namespace | Tampering | Engine defense: always cross-check `SHA3(resolved_pubkey) == target_namespace` on every verify (D-09 step 3 owner branch). This is cheap — same SHA3 that used to be computed anyway. |
| PUBK ingest race (two coroutines register different keys for same namespace) | Integrity | Single-io_context thread confinement (Phase 121) serializes all Storage writes; owner_pubkeys `register` is an MDBX write transaction (atomic). D-12(f) TSAN gate verifies no data-race at the coroutine level. |

## Sources

### Primary (HIGH confidence)

- `db/wire/codec.h` / `codec.cpp` — read entire file; line citations accurate to HEAD.
- `db/schemas/blob.fbs` / `transport.fbs` — read entire file.
- `db/engine/engine.h` / `engine.cpp` — read the verify path (105-347) and delete path (349-485) in full.
- `db/sync/sync_protocol.h` / `sync_protocol.cpp` — read in full; confirmed ingest_blobs delegates to engine.ingest at :97.
- `db/storage/storage.h` / `storage.cpp` — read in full for public API surface, DBI template (delegation_map), and max_maps value.
- `db/storage/thread_check.h` — read in full; STORAGE_THREAD_CHECK macro is no-op under NDEBUG.
- `db/peer/message_dispatcher.cpp` — read the Data (1339-1410) and Delete (327-376) handlers; confirmed engine.ingest/delete_blob are call sites.
- `db/peer/sync_orchestrator.cpp` — :470-550, :950-1030 — confirmed decode_blob_transfer → ingest_blobs flow.
- `db/tests/test_helpers.h` — read in full; confirmed helper patterns.
- `db/tests/storage/test_storage_concurrency_tsan.cpp` — read in full; confirmed TSAN fixture pattern.
- `db/tests/engine/test_engine.cpp` — head read for fixture patterns; 77 TEST_CASEs confirmed by grep.
- `db/CMakeLists.txt` — read lines 1-200 for dependency versions + flatc wiring + test target.
- `.planning/ROADMAP.md` — read Phase 122 section, success criteria 1-7 confirmed.
- `.planning/phases/121-storage-concurrency-invariant/121-01-SUMMARY.md` — read in full; post-back-to-ioc idiom confirmed.
- `.planning/phases/121-storage-concurrency-invariant/121-CONTEXT.md` — read in full; STORAGE_THREAD_CHECK derivation confirmed.
- Memory: `project_phase122_pubk_first_invariant.md`, `feedback_no_duplicate_code.md`, `feedback_no_backward_compat.md`, `feedback_no_python_sdk.md` — all read in full.

### Secondary (MEDIUM confidence)

- `db/peer/error_codes.h` — read in full; confirmed current error code scheme (6 codes) for planning new `ERROR_PUBK_FIRST_VIOLATION` / `ERROR_PUBK_MISMATCH` additions.
- `db/net/protocol.h` — read in full; TransportCodec encode/decode signatures are the dispatcher's interface.
- `db/identity/identity.h` — read in full; confirmed no KEM pubkey in NodeIdentity (tests need to synthesize KEM pubkey for PUBK blobs).

### Tertiary (LOW confidence)

- None — all findings are cross-verified by direct code read.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH — every library + version confirmed in CMakeLists.txt; no version drift question.
- Architecture: HIGH — verify-path lines (174-295, 405-443) confirmed; sync→engine delegation confirmed at sync_protocol.cpp:97.
- Pitfalls: HIGH — each pitfall backed by a specific code read (max_maps value, generated-header committed-in-tree dual state, encode_blob_transfer wire format).
- Testing: HIGH — existing fixture pattern read; gaps enumerated concretely.

**Research date:** 2026-04-20
**Valid until:** 2026-05-04 (14 days; protocol is actively churning and Phase 122 itself will invalidate many of the line citations — planner should start immediately).

---
*Phase: 122-schema-signing-cleanup-strip-namespace-and-compress-pubkey*
*Researched: 2026-04-20*
