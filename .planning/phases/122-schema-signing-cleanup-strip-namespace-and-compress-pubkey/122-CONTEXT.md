# Phase 122: Schema + Signing Cleanup — Context

**Gathered:** 2026-04-20
**Status:** Ready for planning

<domain>
## Phase Boundary

One coordinated protocol-breaking change that (a) strips the `namespace_id` field from the signed Blob schema, (b) replaces the per-blob 2592-byte `pubkey` with a 32-byte `signer_hint` resolved via a new `owner_pubkeys` DBI (owner writes) or the existing `delegation_map` (delegate writes), and (c) hardens the first-write-must-be-PUBK invariant at the node protocol level across direct writes, delegated writes, and sync-replicated writes. Target outcome: ~35% smaller signed blobs, ~2592-byte reduction per blob, no redundant fields in the v1 wire format.

**This phase DOES:**
- Remove `namespace_id` field from `db/schemas/blob.fbs`.
- Remove per-blob `pubkey` from `db/schemas/blob.fbs`; add `signer_hint` (32 bytes).
- Add a new transport body schema carrying `target_namespace` alongside the Blob (transport-level, outside the signed blob).
- Create a new `owner_pubkeys` DBI in `Storage` (key: 32-byte `signer_hint`, value: 2592-byte ML-DSA-87 signing pubkey).
- Add node-level enforcement of PUBK-first on every ingest path (direct + sync-replicated).
- Update `build_signing_input()` to absorb `target_namespace` (byte-for-byte identical to today's protocol — pure parameter rename).
- Remove the now-redundant `derived_ns == blob.namespace_id` checks at `db/engine/engine.cpp:190-193` and `:405-412`.
- Mechanical rename `namespace_id` → `namespace` in Storage's public method signatures.

**This phase does NOT:**
- Support PUBK overwrite / key rotation beyond idempotent-match (that's a future phase if ever needed).
- Build a migration tool for pre-122 data dirs — operator wipes both nodes manually.
- Touch PROTOCOL.md or README.md (that's Phase 125).
- Touch CLI — `cdb` adaptation to the new wire format is Phase 124.
- Introduce any storage-layer byte format change — same 32-byte namespace + 32-byte content_hash composite key, same blob value layout (minus the removed fields).

</domain>

<decisions>
## Implementation Decisions

### Signing Canonical Form

- **D-01:** `signing_input = SHA3-256(target_namespace || data || ttl_be32 || timestamp_be64)`. The signer commits to the **target namespace**, not to `signer_hint`. For owner writes these are identical bytes (since `target_namespace = SHA3(owner_pubkey) = signer_hint`); for delegate writes they differ — the delegate's signature is scoped to exactly one target namespace, preventing cross-namespace replay when a delegate holds delegations on multiple namespaces. Byte-for-byte identical to today's protocol: the only change in `db/wire/codec.cpp:66-98` is the parameter rename `namespace_id` → `target_namespace`.

### signer_hint Derivation

- **D-02:** `signer_hint = SHA3-256(signing pubkey)`. 32 bytes. Collision resistance = SHA3-256 (≈2^128). This is literally the computation today's code does at `db/engine/engine.cpp:190-191` to check `derived_ns == blob.namespace_id` — post-122 the same value is consumed as the lookup key into `owner_pubkeys` instead of being checked against a transport-carried namespace_id. Zero new hash primitive introduced.

### PUBK-First Invariant + Rotation Semantics

- **D-03:** PUBK-first invariant enforced at the **node protocol level**, in the ingest handler (`db/engine/engine.cpp`) and on the sync-replication path (`db/sync/sync_protocol.cpp`). On every ingest: if `!has_owner_pubkey(target_namespace_derived_from_no_pubk)` (i.e. target namespace has no registered PUBK yet) AND incoming blob magic != `PUBK` → reject with protocol-level error. This runs BEFORE signature verification (no pubkey → nothing to verify against anyway). NOT a CLI-only check — a misbehaving client or adversarial peer must not be able to bypass it. See `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/project_phase122_pubk_first_invariant.md` for full rationale; do not soften this during planning.
- **D-04:** First PUBK wins. On an ingest where `owner_pubkeys[signer_hint]` already exists:
  - If the incoming blob is a PUBK with **identical** signing pubkey bytes → accept idempotently. Content-hash dedup handles bit-identical blobs; a PUBK with same signing pubkey but different KEM pubkey (KEM rotation) is accepted and overwrites the main-DBI PUBK blob entry.
  - If the incoming blob is a PUBK with **different** signing pubkey → reject with `ErrorCode::PUBK_MISMATCH` ("namespace already owned by different signing key"). No delegation-gated rotation in this phase.
  - This aligns with chromatindb's identity model: namespace = SHA3(signing pubkey), the signing key IS the identity, loss of key = loss of namespace, consistent with content-addressed everything.

### owner_pubkeys DBI

- **D-05:** New DBI on `Storage`, key = 32-byte `signer_hint`, value = 2592-byte raw ML-DSA-87 signing pubkey. KEM pubkey is **not** duplicated here — it stays in the PUBK blob body (main blob DBI), which peers sync like any other blob. Clients that need a recipient's KEM pubkey fetch the PUBK blob via the existing blob-fetch API. `owner_pubkeys` is a pure node-internal verify-path index; clients never read it.
- **D-06:** Four new public methods on `Storage`, all gated by `STORAGE_THREAD_CHECK()` per Phase 121 D-07:
  - `void register_owner_pubkey(std::span<const uint8_t, 32> signer_hint, std::span<const uint8_t, 2592> pubkey)` — idempotent on matching value; called from ingest handler after a PUBK blob verifies.
  - `std::optional<std::array<uint8_t, 2592>> get_owner_pubkey(std::span<const uint8_t, 32> signer_hint)` — called on the verify hot path for every non-PUBK ingest.
  - `bool has_owner_pubkey(std::span<const uint8_t, 32> signer_hint)` — PUBK-first invariant check + duplicate-PUBK detection.
  - `uint64_t count_owner_pubkeys() const` — for metrics.

### Transport Wire Format

- **D-07:** A new transport message body schema carries `target_namespace` alongside the signed Blob: `{ target_namespace: [ubyte:32], blob: Blob }`. The `Blob` inside has fields `{ signer_hint: [ubyte:32], data: [ubyte], ttl: uint32, timestamp: uint64, signature: [ubyte] }`. `target_namespace` is NOT part of the signed blob bytes (not in the Blob schema) — it sits at the transport envelope layer, but IS absorbed into the signing sponge (see D-01).
- **D-08:** Planning should decide whether to (a) repurpose the existing `Data = 8` TransportMsgType payload with the new body schema, or (b) add a new `BlobWrite` TransportMsgType for auditable separation. Recommend (b) — the dispatcher already has 63 types, one more is free, and a clean new type makes the protocol break obvious in wire logs and dispatcher code.

### Verify Flow (post-122)

- **D-09:** Verify logic on every blob ingest (engine.cpp + sync_protocol.cpp):
  1. Transport delivers `{ target_namespace, blob }`. `target_namespace` is trusted only insofar as the node uses it as the storage key and signing-sponge input — if it's wrong, signature verification fails.
  2. **PUBK-first check:** if `!has_owner_pubkey(target_namespace_derived_from_PUBK_registration)` AND blob is non-PUBK → reject. (Derivation note: PUBK registration is triggered by blob magic; the target_namespace for a valid PUBK write must equal SHA3(pubkey-in-PUBK-body). Rejection fires on the "namespace has no row in owner_pubkeys yet AND incoming magic != PUBK" condition.)
  3. Lookup `owner_pubkeys[blob.signer_hint]`:
     - **Found**: verify `SHA3(pubkey) == target_namespace` (owner write). Verify ML-DSA-87 signature using `signing_input = SHA3(target_namespace || data || ttl_be32 || timestamp_be64)`.
     - **Not found**: check `delegation_map[target_namespace]` for an entry whose delegate_pubkey hashes to `blob.signer_hint`. If found → delegate write, verify signature with delegate_pubkey; if not found → reject (unknown signer OR delegate not authorized for this namespace).
  4. If verified, dispatch to Storage write path as today.

### Storage API Vocabulary

- **D-10:** Mechanical rename `namespace_id` → `namespace` across all `Storage` public method signatures. No DBI byte-level change, no storage value layout change. The 32-byte namespace identifier is still the primary-key prefix; only the parameter name gains clarity ("namespace" is now a derived identity, not a wire field with `_id` suffix).

### Migration

- **D-11:** No migration code. No detection of pre-122 data dirs. No startup warning. Operator manually wipes both existing nodes before deploying the 122 build. (User-confirmed: "I have only 2 nodes running, I'll wipe manually before upgrade.") If the post-122 node happens to read a pre-122 data dir, behavior is undefined — that's the operator's problem, not something the node defends against.

### Testing

- **D-12:** Required Catch2 coverage for PUBK-first invariant (from memory `project_phase122_pubk_first_invariant.md`):
  - (a) Non-PUBK as first write in a fresh namespace → rejected.
  - (b) Sync-replicated non-PUBK-first → rejected on the sync path (separate test from direct-write path).
  - (c) PUBK → non-PUBK works (post-PUBK writes to established namespace succeed).
  - (d) PUBK with matching signing pubkey overwrites existing PUBK (KEM rotation) → accepted + DBI value updated.
  - (e) PUBK with different signing pubkey for an already-owned namespace → rejected with `PUBK_MISMATCH`.
  - (f) Cross-namespace race: two peers attempt to register different PUBKs for the same namespace simultaneously — first-wins, second rejected. (Exact semantics around "simultaneous" via TSAN-backed concurrent test; reuse `test_storage_concurrency_tsan.cpp` fixture style from Phase 121.)
- **D-13:** Delegate-replay regression test: a delegate holding delegations on two namespaces signs a blob for namespace N_A. Node with `target_namespace = N_B` submitted → signature verification MUST fail because the sponge input differs. This test proves D-01's cross-namespace replay protection.

### Claude's Discretion

- Exact field name in the transport envelope for `target_namespace` (e.g., `target_namespace` vs `ns` vs `namespace`) — pick whatever reads cleanly.
- Whether `register_owner_pubkey` takes a `std::span<const uint8_t, 2592>` or `std::array<uint8_t, 2592>` — pick whichever matches the prevailing style in Storage's public API.
- Whether the PUBK-first check lives in a shared helper or is inlined at both the engine ingest path and the sync ingest path — pick the form that minimizes duplication (per `feedback_no_duplicate_code.md`).
- Naming of the new TransportMsgType if D-08(b) is chosen — `BlobWrite`, `SignedBlob`, `Write`, etc.
- Whether to add a one-line schema-version byte to the blob DBI env at the MDBX level (just for future forensics — not for migration, since D-11 rules out runtime migration). YAGNI unless planner sees a strong reason.

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Project Policy / Memory (non-negotiable constraints)
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/project_phase122_pubk_first_invariant.md` — PUBK-first enforced at node protocol level (not CLI), applies to direct + delegated + sync paths, do not soften during planning.
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/feedback_no_backward_compat.md` — No backward compat on either binary; protocol changes are fine.
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/feedback_no_duplicate_code.md` — Utilities go into shared headers (`db/util/`, `db/tests/test_helpers.h`), not copy-pasted.
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/feedback_no_python_sdk.md` — Do not design for a hypothetical future non-cdb client; `cdb` is the only client.

### Roadmap
- `.planning/ROADMAP.md` § Phase 122 — Goal, success criteria #1-7, depends-on Phase 121.

### Prior Phase Context
- `.planning/phases/121-storage-concurrency-invariant/121-CONTEXT.md` — D-03 call-site inventory (sync path, engine path, peer_manager path) reused here since those same paths are touched for PUBK-first enforcement. D-07 STORAGE_THREAD_CHECK pattern applies to the four new `owner_pubkeys` Storage methods.
- `.planning/phases/121-storage-concurrency-invariant/121-01-SUMMARY.md` — Post-back-to-ioc pattern at crypto::offload boundaries; new owner_pubkeys lookups on the verify path MUST follow the same `co_await asio::post(co_await asio::this_coro::executor, asio::use_awaitable)` discipline.

### Existing Code — schema + wire
- `db/schemas/blob.fbs` — current Blob table (6 fields); remove `namespace_id` and `pubkey`, add `signer_hint:[ubyte:32]`.
- `db/schemas/transport.fbs` — TransportMessage envelope + 63-entry TransportMsgType enum (D-08 either repurposes `Data = 8` or adds a new `BlobWrite` type).
- `db/wire/codec.h:1-50` — public codec API (build_signing_input signature); D-01 renames `namespace_id` parameter.
- `db/wire/codec.h:92-121` — DELEGATION_* constants, PUBKEY_MAGIC ("PUBK" = 0x50 0x55 0x42 0x4B), PUBKEY_DATA_SIZE (4164 = 4+2592+1568), `is_pubkey_blob()` helper. All reusable as-is.
- `db/wire/codec.cpp:66-98` — current `build_signing_input()`; 4-line diff (parameter rename + comment update). Byte-output identical.

### Existing Code — engine verify path (the main delta)
- `db/engine/engine.cpp:174-295` — ingest / verify / store flow. Focal changes:
  - `:174-178` — pubkey size check (remove; no inline pubkey post-122).
  - `:190-197` — `derived_ns = SHA3(blob.pubkey)` + `is_owner = (derived_ns == blob.namespace_id)` — REMOVE this derivation, replace with `get_owner_pubkey(blob.signer_hint)` lookup; owner check becomes `SHA3(pubkey) == target_namespace`.
  - `:202-205` — delegation path lookup; refactor to go via `delegation_map[target_namespace]` and compare delegate pubkey hash to `blob.signer_hint`.
  - `:273-274` — `build_signing_input(blob.namespace_id, ...)` + `Signer::verify(si, blob.signature, blob.pubkey)` → become `build_signing_input(target_namespace, ...)` + `Signer::verify(si, blob.signature, resolved_pubkey)`.
- `db/engine/engine.cpp:405-412` — delete path's `derived_ns != delete_request.namespace_id` check. Same refactor as the ingest path.
- `db/engine/engine.h` — may need minor signature changes if verify APIs take `target_namespace` + `signer_hint` separately vs. packed into the Blob.

### Existing Code — sync path (PUBK-first must apply here too)
- `db/sync/sync_protocol.h` — ingest coroutines on the sync path; PUBK-first check MUST fire here on replicated writes, not just direct writes. If the check lives only in engine.cpp, sync can bypass it.
- `db/sync/sync_protocol.cpp` (whichever file contains `ingest_blobs` or equivalent) — replicated blob dispatch to Storage. Add PUBK-first gate + owner_pubkeys lookup mirroring engine.cpp's flow.

### Existing Code — storage (new DBI home)
- `db/storage/storage.h` — Storage public API surface; add 4 new methods per D-06 (with STORAGE_THREAD_CHECK). Keep existing `namespace_id` parameter names renamed to `namespace` per D-10 (mechanical rename across the full Storage API).
- `db/storage/storage.cpp` — MDBX DBI handles in `Impl`; add `owner_pubkeys` DBI alongside existing `blob`, `tombstone`, `delegation_map`, `cursor`, `quota` DBIs. Pattern is structurally identical to `delegation_map`.

### Existing Code — PUBK body schema (unchanged, but must stay readable)
- `db/wire/codec.h:112-121` — PUBKEY_MAGIC + PUBKEY_DATA_SIZE + `is_pubkey_blob()`. A PUBK blob's DATA field remains `[PUBK:4][signing_pk:2592][kem_pk:1568]` = 4164 bytes. Phase 122 does not change the PUBK body; only where it gets indexed (new `owner_pubkeys` DBI).

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `db/wire/codec.h` PUBKEY_MAGIC + `is_pubkey_blob()` helper — reused as-is for the PUBK-first check on the ingest path.
- `db/wire/codec.cpp` `build_signing_input()` — reused with parameter rename; output byte-identical to today.
- `db/storage/storage.*` `delegation_map` DBI — structural template for the new `owner_pubkeys` DBI (both are 32-byte-key → pubkey-value MDBX DBIs).
- Phase 121 `STORAGE_THREAD_CHECK()` macro + ThreadOwner — applied to all four new `owner_pubkeys` Storage methods.
- Phase 121 post-back-to-ioc pattern after `crypto::offload()` — applied to the new `get_owner_pubkey` lookup since it sits on the verify path after SHA3 offload.

### Established Patterns
- Big-endian everywhere, TTL/timestamp are uint32_be / uint64_be. ttl=0 is permanent. (Memory constraints.)
- Hash-then-sign: ML-DSA-87 signs SHA3-256 digest, not raw concat. This phase preserves that — just reframes the digest input.
- FlatBuffers schema changes require regen (the `blob_generated.h` and `transport_generated.h` artifacts will need to be rebuilt when `.fbs` files change).
- Node runs on a single io_context thread; every public `Storage::*` method carries STORAGE_THREAD_CHECK (Phase 121). Any code on the verify path that introduces an off-thread access would fail that assertion in debug builds.

### Integration Points
- `db/engine/engine.cpp` verify path at `:174-295` — the main code delta for this phase.
- `db/engine/engine.cpp` delete path at `:405-443` — mirror the same refactor (delete_request also carried namespace_id + pubkey).
- `db/sync/sync_protocol.cpp` blob ingest — MUST include the PUBK-first gate (memory: node-level, applies to sync too).
- `db/storage/storage.{h,cpp}` `Impl` struct — new DBI handle + 4 new public methods (all with STORAGE_THREAD_CHECK).
- `db/main.cpp` — no changes expected; Storage construction is unchanged.
- `cdb/` CLI — out of scope (Phase 124 adapts the CLI).

</code_context>

<specifics>
## Specific Ideas

- The protocol break is the right moment to also **mechanical-rename** `namespace_id` → `namespace` in the Storage public API (D-10). Both changes land in one phase rather than paying two rounds of churn.
- Favor a new `BlobWrite` TransportMsgType over repurposing `Data = 8` (D-08 recommendation). Dispatcher auditability + wire-log clarity outweigh the "free" slot in the existing enum.
- Use the same TSAN test-fixture style as `db/tests/storage/test_storage_concurrency_tsan.cpp` from Phase 121 for the cross-namespace race regression test in D-12(f).

</specifics>

<deferred>
## Deferred Ideas

- **Delegation-gated PUBK rotation** (Area 3 Option C) — allows a namespace owner to rotate their ML-DSA signing key by publishing a new PUBK accompanied by a rotation-authorization blob signed by the old key. Useful for long-lived deployments where PQ key rotation becomes necessary. Not needed for MVP; ML-DSA-87 is designed to be long-lived. If added later, it's a new blob magic + new verify path — no conflict with D-04's "first PUBK wins" rule (the rotation blob would be a separate protocol surface).
- **Migration tool for pre-122 data dirs** (Area 6 Option B) — would let operators preserve blob data across the protocol break. Ruled out because there are only 2 dev/test nodes running and the operator will manually wipe. Revisit if chromatindb ever gets a real production deployment that needs data preservation.
- **owner_pubkeys DBI metadata** (first_seen_timestamp, originating_blob_hash) (Area 4 Option C) — ~40 bytes per namespace of observability data. Not needed for correctness; can be added later by wrapping the DBI value in a small struct without breaking any public API.
- **Schema version byte in the MDBX env** — for future forensics / "what version wrote this data dir?" queries. YAGNI now since D-11 rules out runtime migration, but a harmless future addition if an operator tool ever wants to query it.

</deferred>

---

*Phase: 122-schema-signing-cleanup-strip-namespace-and-compress-pubkey*
*Context gathered: 2026-04-20*
