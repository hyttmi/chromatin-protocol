# Phase 124: CLI Adaptation to New Protocol — Context

**Gathered:** 2026-04-21
**Status:** Ready for planning

<domain>
## Phase Boundary

Migrate `cdb` end-to-end to the post-122 wire format and post-123 flows so every `cdb` subcommand (`put`, `get`, `rm`, `ls`, `publish`, `delegate`, `revoke`, `put --name`, `get <name>`, multi-target `rm`, `put --name --replace`, chunked put/get) works against a post-122+123 node binary. Deliver auto-PUBK so fresh-namespace writes don't trip the node's PUBK-first gate. Live-node E2E against both local and `192.168.1.73` (home) nodes — including multi-node sync — after the user deploys the new node binary.

**This phase DOES:**
- Replace `cli/src/wire.h`'s `BlobData { namespace_id:[32], pubkey:[2592], data, ttl, timestamp, signature }` with the post-122 shape `{ signer_hint:[32], data, ttl, timestamp, signature }`.
- Rename `build_signing_input(namespace_id, ...)` → `build_signing_input(target_namespace, ...)`. Output byte-identical; parameter rename only (matches Phase 122 D-01).
- Add `cli/src/wire.{h,cpp}` encoder for the `BlobWriteBody` envelope `{ target_namespace:32, blob:Blob }` used by `TransportMsgType_BlobWrite = 64`.
- Add the central helper (see D-03) that computes `signer_hint = SHA3(signing_pubkey)`, builds the signing input, signs, and emits the `(target_namespace, BlobData)` pair.
- Migrate all ~39 call sites in `cli/src/commands.cpp` + ~3 in `cli/src/chunked.cpp` that currently set `blob.namespace_id + blob.pubkey` over to the helper.
- Switch every send path that writes a blob from `MsgType::Data = 8` to the new `BlobWrite = 64` envelope.
- Handle the new node error codes `ERROR_PUBK_FIRST_VIOLATION = 0x07` and `ERROR_PUBK_MISMATCH = 0x08` with user-facing messages.
- Implement per-session auto-PUBK (D-01): before the first owner-write to a namespace within a `cdb` invocation, probe via `ListRequest + type_filter=PUBK_MAGIC` and publish a PUBK blob if the namespace has none.
- Update `cmd::rm_batch` to cascade BOMBs across CPAR manifests (deferred from Phase 123-03 handoff — see D-06).
- Update `cli/tests/test_wire.cpp` blob roundtrip TEST_CASE to the new schema; add new TEST_CASEs for `BlobWriteBody` envelope, the build helper, NAME/BOMB payload roundtrips, and auto-PUBK probe logic.
- Live-node E2E against two nodes — local + `home` (192.168.1.73) — after user redeploys. Matrix: put → get → put `--name` → get `<name>` → `rm` (batched) → `ls` → multi-node sync → chunked (>500 MiB CDAT/CPAR) → delegate (`--share @contact`) → `--replace` (BOMB-of-1).

**This phase does NOT:**
- Touch node-side wire/protocol code (Phase 122+123 already landed those changes).
- Add a new node-side `ListByMagic` TransportMsgType — Phase 123 VERIFICATION confirms it reuses Phase 117's `ListRequest + type_filter`; the auto-PUBK probe reuses that same primitive.
- Introduce a persistent PUBK cache at `~/.chromatindb/pubk_cache.json` (rejected — see `<deferred>`).
- Add `--as <owner_ns>` or any new owner/delegate flag (rejected — see D-02).
- Touch PROTOCOL.md / README.md / cli/README.md (Phase 125).
- Build a local-fork-a-node integration test harness (overlaps with backlog phase 999.4).
- Persist or warm any `~/.cdb/` state beyond what already exists (config, identity, contacts DB).

</domain>

<decisions>
## Implementation Decisions

### Auto-PUBK Strategy

- **D-01:** Per-session, per-namespace, **on-first-owner-write** auto-PUBK. Before the first owner-write from a single `cdb` invocation to a given `target_namespace`, `cdb` probes the node with `ListRequest + type_filter=PUBK_MAGIC_CLI + namespace=target_namespace + limit=1`. If the response has zero blobs, `cdb` signs and sends a PUBK blob (data = `[PUBK magic:4][signing_pk:2592][kem_pk:1568]` = 4164 bytes, ttl=0, timestamp=now) via the `BlobWrite = 64` envelope, waits for `WriteAck`, then proceeds with the user's blob. **In-process only**: a `std::unordered_set<std::array<uint8_t,32>>` of "known-PUBK'd" namespaces within this invocation; no persistence to disk. Cost: one extra RTT per process per namespace written; one PUBK blob per fresh namespace across the network (idempotent per Phase 122 D-04 on repeat invocations — node accepts bit-identical PUBK as dedup, or overwrites for KEM rotation). Claude's discretion: probe-failure recovery (proceed with PUBK emit optimistically, since the node will either accept or reject cleanly; no retry loop).

- **D-01a:** Auto-PUBK fires for OWNER writes only, never for delegate writes. Delegate writes target a namespace that the delegate does not own — PUBK registration for that namespace is the owner's responsibility and must already be on the node (node rejects delegate-first-write with `PUBK_FIRST_VIOLATION`, which surfaces as a clean error to the delegate operator). The helper from D-03 identifies owner writes by `target_ns == SHA3(own_signing_pubkey)`.

### Delegate vs Owner Identification

- **D-02:** Delegate vs owner is **implicit**, derived at the helper layer by comparing `target_namespace == SHA3(identity.signing_pubkey())`. No new CLI flag (`--as`, `--delegate-for`, etc.). Rationale: `cdb` identity model is one signing key per `~/.cdb/` config; target namespace is resolved from subcommand context (`--share @contact`, positional hex, `cdb put` default = own namespace). `signer_hint = SHA3(own_signing_pubkey)` regardless of owner/delegate — the distinction only matters on the node's verify path (owner_pubkeys DBI vs delegation_map lookup, handled by 122-04's engine.cpp changes).

### BlobData Migration & Central Helper

- **D-03:** Single helper in `cli/src/wire.{h,cpp}`:
  ```cpp
  struct NamespacedBlob {
      std::array<uint8_t, 32> target_namespace{};
      BlobData blob;
  };
  NamespacedBlob build_owned_blob(const Identity& id,
                                   std::span<const uint8_t, 32> target_namespace,
                                   std::span<const uint8_t> data,
                                   uint32_t ttl,
                                   uint64_t timestamp);
  ```
  The helper: (a) computes `signer_hint = SHA3(id.signing_pubkey())`, (b) builds `signing_input = SHA3(target_namespace || data || ttl_be32 || timestamp_be64)` via the renamed `build_signing_input`, (c) signs the 32-byte digest with ML-DSA-87 via the identity, (d) populates `BlobData { signer_hint, data, ttl, timestamp, signature }`, (e) returns `NamespacedBlob { target_namespace, blob }`. `NamespacedBlob` name mirrors `db/sync/sync_protocol.h`'s struct so the CLI vocabulary matches the node's. Every current `blob.namespace_id = ns; blob.pubkey = id.signing_pubkey(); /* sign */` sequence collapses to one call.

- **D-03a:** `cli/src/wire.h`'s `BlobData` struct becomes `{ std::array<uint8_t,32> signer_hint; std::vector<uint8_t> data; uint32_t ttl; uint64_t timestamp; std::vector<uint8_t> signature; }` — no `namespace_id`, no `pubkey`. All references to the removed fields across `cli/src/` and `cli/tests/test_wire.cpp` must be replaced.

- **D-03b:** `encode_blob` / `decode_blob` in `cli/src/wire.cpp` must emit/parse the post-122 FlatBuffer Blob table: `table Blob { signer_hint:[ubyte]; data:[ubyte]; ttl:uint32; timestamp:uint64; signature:[ubyte]; }`. Byte-for-byte matches `db/schemas/blob.fbs`.

### BlobWrite Envelope at All Write Sites

- **D-04:** All blob writes go through the new `BlobWrite = 64` envelope, never `Data = 8`. CLI adds:
  - `MsgType::BlobWrite = 64` in the enum in `cli/src/wire.h`.
  - `std::vector<uint8_t> encode_blob_write_body(std::span<const uint8_t, 32> target_namespace, const BlobData& blob)` that produces a `BlobWriteBody` FlatBuffer matching `db/schemas/transport.fbs`.
  - Every `conn.send(MsgType::Data, encode_blob(blob), rid)` call becomes `conn.send(MsgType::BlobWrite, encode_blob_write_body(ns_blob.target_namespace, ns_blob.blob), rid)`.

- **D-04a:** The pre-122 `MsgType::Data = 8` value is **deleted from the CLI enum**. Consistent with no-backward-compat memory + Phase 122-05's "Pre-122 Data=8 dispatcher branch DELETED" on the node side.

- **D-04b:** Tombstones (single `cmd::rm` against a hash target) also ride the `BlobWrite` envelope — structurally identical signed blobs, only the `data` magic differs (DEADBEEF vs PUBK vs user data vs BOMB). Matches 122-05 Task 1(c): node's `Delete` handler already reuses `BlobWriteBody`. The CLI side has historically used `MsgType::Delete = 17` as a distinct send type for single-target tombstones — replace with the unified `BlobWrite = 64` path. Planner: confirm whether `MsgType::Delete = 17` is still needed on the CLI wire (may be unused post-unification; delete if so).

### Error Surface for PUBK Rejections

- **D-05:** `cdb` maps node errors to user-facing wording (never expose `ERROR_PUBK_FIRST_VIOLATION` / `ERROR_PUBK_MISMATCH` bytes or phase numbers — enforces `feedback_no_phase_leaks_in_user_strings.md`):
  - `ERROR_PUBK_FIRST_VIOLATION (0x07)` during auto-PUBK flow should never reach the user — auto-PUBK's probe-then-emit prevents it. If it fires (probe race or node-side edge case), surface as: `"Error: namespace not yet initialized on node <host>. Auto-PUBK failed; try running 'cdb publish' first."`. Exit nonzero.
  - `ERROR_PUBK_MISMATCH (0x08)` is never auto-recoverable (different signing key already owns the namespace). Surface as: `"Error: namespace <ns_short> is owned by a different key on node <host>. Cannot write."`. Exit nonzero.
  - Unknown/future `ERROR_*` codes surface via existing ErrorResponse handling, with byte value in debug logs only.

### BOMB Cascade Across CPAR Manifests (deferred from Phase 123-03)

- **D-06:** `cmd::rm_batch` must cascade tombstones across CPAR manifests when any target is a chunked manifest. For each target in the batch: if `ExistsRequest → type_filter reveals CPAR`, fetch + decrypt the manifest, collect its chunk_hashes, add them to the BOMB target list. Result: one BOMB per invocation covering **manifests + their chunks** (unified batching goal per Phase 123 D-06). Explicit scope item from 123-03-SUMMARY line 213: "`cmd::rm_batch` does not cascade chunked CPAR manifests; Phase 124 is a natural home for the unified cascade+BOMB path." Claude's discretion: exact control flow (parallel manifest fetches via pipelining, error handling when a manifest fetch fails, partial-cascade semantics).

### Live-Node E2E Orchestration

- **D-07:** User builds the post-124 CLI + node binaries to both local and home (`192.168.1.73`) and wipes each node's data dir before redeploy (per Phase 122 D-11). `cdb --node local` / `cdb --node home` selects the target, configured in `~/.cdb/config.json` (existing mechanism). After user confirms both nodes are up, Claude runs the E2E matrix directly (SSH to home when needed; local is already reachable). Matches `feedback_self_verify_checkpoints.md`. User is responsible for build + deploy; Claude is responsible for running verification.

- **D-08:** E2E matrix (phase-done gates on all of these passing):
  1. **SC#7 literal, both nodes:** `cdb publish` → `cdb put file` → `cdb get <hash>` → `cdb put --name foo file` → `cdb get foo` → `cdb rm <hash1> <hash2>` (batched) → `cdb ls`. Run against local AND home.
  2. **Cross-node sync:** put a blob on local, verify it appears in `cdb ls --node home` (and vice versa). Put via home, rm via local, verify tombstone propagates.
  3. **BOMB propagation:** multi-target `cdb rm` on local, verify BOMB appears on home via `cdb ls --type BOMB --node home`.
  4. **Chunked (>500 MiB):** put a 750 MiB file via local, download via home after sync. Validates Phase 119 CDAT/CPAR under the new envelope.
  5. **Delegate (`--share @contact`):** from local using a delegate identity, `cdb put --share @contact file`; verify the content reaches the owner's namespace on home.
  6. **`--replace`:** `cdb put --name foo file_v1`, then `cdb put --name foo --replace file_v2`; verify `cdb get foo` returns v2; verify a BOMB-of-1 (or single tombstone, per 123 Claude's Discretion) is present against file_v1's content hash.
  7. **D-06 cascade:** `cdb put` a chunked file, `cdb rm <manifest_hash>` (via batch with other targets), verify all referenced CDAT chunks are also tombstoned.

- **D-08a:** E2E outputs captured in `.planning/phases/124-cli-adaptation-to-new-mvp-protocol/124-E2E.md`. Failures = phase not done.

### Test Migration Scope

- **D-09:** Surgical test updates + new coverage:
  - **Modify:** `cli/tests/test_wire.cpp` blob-roundtrip TEST_CASE (lines ~147-170) to populate new schema (`blob.signer_hint.fill(...)` instead of `blob.namespace_id.fill(...) + blob.pubkey.resize(2592, ...)`).
  - **Add to `cli/tests/test_wire.cpp`:**
    - `BlobWriteBody` envelope encode/decode roundtrip (target_namespace + inner Blob).
    - `build_owned_blob` helper: asserts resulting `signer_hint == SHA3(id.signing_pubkey())`, resulting `signing_input` byte-matches the post-122 canonical form, signature verifies with the identity's pubkey.
    - `make_name_data` / `parse_name_payload` roundtrip — any name length, target_hash byte-preservation, magic-mismatch rejection.
    - `make_bomb_data` roundtrip — count field encoding, 32-byte target hash concatenation.
  - **Add new file** `cli/tests/test_auto_pubk.cpp` (or TEST_CASE in test_wire.cpp — planner picks): exercise the in-process PUBK cache + probe logic via a mockable transport or a tight unit-sized harness. Must verify: first write triggers probe, empty probe response triggers PUBK emit, second write to same namespace skips probe.
  - **Unchanged:** `test_identity.cpp` (`Identity::namespace_id()` is CLI-local, unchanged), `test_envelope.cpp`, `test_contacts.cpp`, `test_connection_pipelining.cpp`.
  - **Updated via helper absorption:** `test_chunked.cpp` (3 blob-construction sites route through `build_owned_blob` after migration).

### Claude's Discretion

- Exact return type of `build_owned_blob` — `NamespacedBlob` struct (recommended, mirrors `db/sync/sync_protocol.h`) vs `std::pair<...>`.
- Structure of the in-process auto-PUBK cache (a `std::unordered_set<std::array<uint8_t,32>>` on the `Connection` or a cdb-lifetime static — pick what minimizes coupling).
- Whether to delete `MsgType::Delete = 17` from `cli/src/wire.h`'s enum after unification (D-04b) — if no CLI call sites still use it, delete; if the Delete transport path carries different semantics not foldable into `BlobWrite`, keep.
- Where the auto-PUBK probe lives — `Connection` method, a free function in `commands.cpp`, or a tiny new module like `cli/src/pubk_presence.cpp`. `feedback_no_duplicate_code.md` argues for one site.
- Exact wording of user-facing error messages for the two new error codes, within the constraints of D-05.
- Whether `test_auto_pubk.cpp` lives as a new file or a TEST_CASE set inside `test_wire.cpp`. Prefer a new file once the TEST_CASE count crosses ~5.
- Error-path behavior of D-06 cascade when a manifest fetch fails mid-batch (fail the whole rm, partial BOMB with a warning, or treat the manifest as opaque and BOMB only the manifest hash — planner decides).
- Whether D-01's probe uses a synchronous ListRequest (one RTT before first write) or an optimistic pipelined form (fire probe + user write together, cancel user write if probe returns empty and switch to PUBK-then-retry). Prefer the synchronous form for correctness; revisit if E2E shows the extra RTT is noticeable.

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Project Policy / Memory (non-negotiable constraints)
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/feedback_no_backward_compat.md` — No backward compat on either binary; protocol changes are fine.
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/feedback_no_duplicate_code.md` — Utilities go into shared headers (`cli/src/wire.h`, `db/util/`); never copy-paste. Core motivation for D-03's central helper.
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/feedback_no_phase_leaks_in_user_strings.md` — Never leak GSD phase numbers / protocol-internal error codes into cdb help text or error messages.
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/feedback_self_verify_checkpoints.md` — Claude runs verify steps himself when infra is reachable; don't block on user unnecessarily. Drives D-07 orchestration.
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/feedback_delegate_tests_to_user.md` — Orchestrator-level build/test runs should be delegated to the user. Applies to CMake build on both nodes (D-07).
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/feedback_no_python_sdk.md` — `cdb` is the only client. Do not design for hypothetical SDKs.
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/project_phase122_pubk_first_invariant.md` — PUBK-first is a node protocol invariant; CLI auto-PUBK (D-01) is the cooperative half of that invariant, not a replacement for node enforcement.
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/project_phase123_bomb_ttl_zero.md` — BOMB ttl=0 enforced at ingest; CLI must emit BOMBs with ttl=0 (already done in commands.cpp; auto-PUBK flow must not regress this).

### Roadmap
- `.planning/ROADMAP.md` § Phase 124 — Goal, Success Criteria #1-7, depends-on Phase 123.

### Prior Phase Context
- `.planning/phases/122-schema-signing-cleanup-strip-namespace-and-compress-pubkey/122-CONTEXT.md` — D-01 signing canonical form, D-02 signer_hint derivation, D-03 PUBK-first invariant (node-enforced), D-04 first-PUBK-wins / idempotent rules for auto-PUBK, D-07/08 BlobWrite envelope shape, D-10 Storage API rename, D-11 manual wipe.
- `.planning/phases/122-schema-signing-cleanup-strip-namespace-and-compress-pubkey/122-05-SUMMARY.md` — `BlobWrite = 64` dispatcher + BlobWriteBody envelope, `Data = 8` branch deleted on node side, sync wire format per-blob `[ns:32B]` prefix. CLI mirrors the same envelope shape.
- `.planning/phases/122-schema-signing-cleanup-strip-namespace-and-compress-pubkey/122-04-SUMMARY.md` — IngestError enum with `pubk_first_violation` + `pubk_mismatch` → `ERROR_PUBK_FIRST_VIOLATION = 0x07` / `ERROR_PUBK_MISMATCH = 0x08` wire codes. D-05 user-facing wording maps against these.
- `.planning/phases/123-tombstone-batching-and-name-tagged-overwrite/123-CONTEXT.md` — D-01..D-15 NAME/BOMB semantics, D-06 per-command batching, D-09 stateless no-cache name resolution, D-13/D-14 node validation scope, D-15 --replace semantics.
- `.planning/phases/123-tombstone-batching-and-name-tagged-overwrite/123-VERIFICATION.md` — Confirms Phase 123 reuses Phase 117 `ListRequest + type_filter` for name enumeration (no new node-side TransportMsgType). D-01 auto-PUBK probe reuses the same primitive.
- `.planning/phases/123-tombstone-batching-and-name-tagged-overwrite/123-03-SUMMARY.md` §"cmd::rm_batch does not cascade chunked CPAR manifests" (line 213-218) — explicit Phase 124 scope addition (D-06).

### Existing Code — CLI wire + envelope
- `cli/src/wire.h` — current `BlobData` struct (schema to change), `MsgType` enum (add BlobWrite=64, remove Data=8), `build_signing_input` (parameter rename), NAME/BOMB magic + helpers (already landed in Phase 123).
- `cli/src/wire.cpp:22` — FlatBuffer Blob schema comment (currently `namespace_id:[ubyte]; pubkey:[ubyte]; ...`) must become `signer_hint:[ubyte]; data:[ubyte]; ...`.
- `cli/src/wire.cpp:147-212` — `encode_blob` + `decode_blob` FlatBuffer emission/parse; must match post-122 blob.fbs.
- `cli/src/wire.cpp:316-410` (make_name_data, make_bomb_data, parse_name_payload) — Phase 123 helpers; no changes needed, but they must be callable from the new helper flow unchanged.

### Existing Code — CLI blob-construction call sites
- `cli/src/commands.cpp` — the 39 sites that currently set `blob.namespace_id + blob.pubkey`:
  - `:54, :379, :389, :587, :2169, :2243, :2299, :2493` — reads `id.namespace_id()` (Identity-local, unchanged).
  - `:514-515, :548-549, :716-717, :1233-1234, :1703-1704, :1749-1750, :2255-2256, :2387-2388, :2521-2522` — blob construction sites; route through D-03 helper.
  - `cmd::put` (file put): `:514+` region.
  - `cmd::delegate` / `cmd::revoke`: `:1233+`, `:1703+`.
  - `cmd::rm` (single-target): `:1749+`.
  - `cmd::put --name` (Phase 123): `:2255+`.
  - `cmd::rm_batch`: `:2387+` (D-06 cascade added here).
  - `cmd::publish` (PUBK): `:2521+` (will also be invoked by auto-PUBK from D-01 under the hood; unify).
- `cli/src/chunked.cpp:105-106, 126-127, 322-323` — 3 blob-construction sites (chunked CDAT + manifest build). Route through D-03 helper.
- `cli/src/connection.cpp:99` — decodes pubkey from handshake; unrelated to blob schema, no change needed.
- `cli/src/identity.cpp` — `Identity::namespace_id()` unchanged (SHA3 of own signing_pubkey, CLI-local concept).
- `cli/src/main.cpp:360` — usage strings; verify they don't reference `namespace_id` or old-pubkey semantics after migration.

### Existing Code — CLI ListRequest name resolution (template for auto-PUBK probe)
- `cli/src/commands.cpp:163` — `conn.send(MsgType::ListRequest, list_payload, 1)` — existing client pattern for Phase 117 `ListRequest + type_filter`.
- `cli/src/commands.cpp:492-540` (`resolve_name_to_target_hash`) — Phase 123 name-resolution flow that enumerates NAME blobs via `ListRequest + type_filter=NAME_MAGIC_CLI`. Structural template for D-01 auto-PUBK probe (same flow, `type_filter = PUBK_MAGIC_CLI`, limit=1).
- `cli/src/commands.cpp:1446-1529` — second ListRequest site used by `cmd::get_by_name`. Shows the expected request/response layout.

### Existing Code — node wire artifacts CLI must match byte-for-byte
- `db/schemas/blob.fbs` — post-122 Blob table (`signer_hint:[ubyte]; data:[ubyte]; ttl:uint32; timestamp:uint64; signature:[ubyte];`). CLI's `encode_blob` / `decode_blob` emit/parse identical bytes.
- `db/schemas/transport.fbs:83-87` — `BlobWriteBody { target_namespace:[ubyte]; blob:Blob; }`. CLI's `encode_blob_write_body` emits identical FlatBuffer.
- `db/schemas/transport.fbs:70` — `TransportMsgType.BlobWrite = 64`. CLI's `MsgType::BlobWrite` must use the same value.
- `db/wire/codec.h:42-47` — `encode_blob_write_body(target_namespace, blob)` node-side signature; CLI helper mirrors it.
- `db/wire/codec.h:50-57` — `build_signing_input(target_namespace, data, ttl, timestamp)` node-side signature; CLI's renamed helper mirrors it byte-for-byte.
- `db/wire/codec.h` PUBKEY_MAGIC + PUBKEY_DATA_SIZE constants — CLI's `cli/src/wire.h:PUBKEY_MAGIC + PUBKEY_DATA_SIZE` already match (verified).

### Existing Code — node error codes CLI must handle
- `db/peer/message_dispatcher.cpp` (post-122) — emits `ERROR_PUBK_FIRST_VIOLATION = 0x07` and `ERROR_PUBK_MISMATCH = 0x08`. CLI's `ErrorResponse` handler in `commands.cpp` maps these per D-05.

### Test files
- `cli/tests/test_wire.cpp:147-170` — blob encode/decode roundtrip (schema-change target).
- `cli/tests/test_identity.cpp` — unchanged, reviewed.
- `cli/tests/test_chunked.cpp` — exercises chunked blob construction; verify it still passes after helper migration.

### Live-node operator docs
- `~/.cdb/config.json` (operator's machine) — `--node local` / `--node home` selector, existing mechanism. Claude verifies both node keys are correct post-redeploy.
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/reference_live_node.md` — live node at 192.168.1.73 for testing.

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **Identity class (`cli/src/identity.{h,cpp}`)** — already provides `signing_pubkey()`, `sign()`, `namespace_id()` (SHA3 of own signing pubkey). D-03's helper composes these without Identity-class changes.
- **`build_signing_input` in `cli/src/wire.cpp`** — byte-identical semantics post-rename (Phase 122 D-01); only the parameter name changes to `target_namespace`.
- **ListRequest + type_filter (Phase 117)** — already wired into `commands.cpp`. D-01 auto-PUBK probe reuses the exact request/response layout Phase 123 uses for name enumeration.
- **NAME + BOMB helpers (`make_name_data`, `make_bomb_data`, `parse_name_payload`)** — Phase 123 work, already in `cli/src/wire.cpp:316-410`. Ride the new envelope unchanged via D-03 helper.
- **Phase 119 chunked helpers (`chunked::put_chunked`, `chunked::get_chunked`, `chunked::rm_chunked`, Sha3Hasher)** — unchanged except for the 3 blob-construction call sites in `chunked.cpp` that migrate to D-03 helper.
- **Connection class (`cli/src/connection.{h,cpp}`)** — per-connection PQ transport, pipelined send/recv. Natural home for the in-process auto-PUBK namespace cache (D-01).
- **Contacts DB + `--share @group` resolution (Phase 116)** — already resolves contact names to target namespaces. Unchanged; post-122 target_namespace still comes from the same source.
- **`MsgType::Delete = 17` handling** — may become redundant after D-04b unification; planner confirms and deletes if unused.

### Established Patterns
- Big-endian everywhere. `timestamp_be64`, `ttl_be32` in signing input.
- ChaCha20-Poly1305 AEAD frame encrypt/decrypt on the wire (unchanged).
- FlatBuffers encoder + Verifier at trust boundaries (mirror 122-05 dispatcher's pattern on the CLI decode path for BlobWriteBody responses if any).
- Pipelined send/recv with per-rid correlation (Phase 120). Auto-PUBK probe + PUBK emit fit the existing pipeline model.
- Error-response routing: `ErrorResponse = 63` decoded, error bytes mapped to messages.
- `feedback_no_duplicate_code.md` enforced: one helper (D-03), one probe site, one error mapping table.

### Integration Points
- `cli/src/wire.{h,cpp}` — schema + envelope changes (D-03a, D-03b, D-04).
- `cli/src/commands.cpp` — ~39 call sites migrated to D-03 helper; auto-PUBK probe wired in at the first owner-write per namespace; D-06 BOMB-cascade added to `cmd::rm_batch`; D-05 error mapping expanded.
- `cli/src/chunked.cpp` — 3 blob-construction sites migrated to D-03 helper.
- `cli/src/connection.{h,cpp}` — new in-process `std::unordered_set<std::array<uint8_t,32>>` "pubk_seen" field (D-01), exposed via a small helper like `bool ensure_pubk(target_namespace, identity)` or similar.
- `cli/src/main.cpp:360` — usage text sanity check (no leaked internal names).
- `cli/tests/test_wire.cpp` — new TEST_CASEs (envelope, helper, NAME/BOMB roundtrips, auto-PUBK).
- `~/.cdb/config.json` — no schema change; `--node local` / `--node home` selector unchanged.
- Live nodes (local + `192.168.1.73`) — out-of-band redeploy by user; Claude runs E2E after deploy.

</code_context>

<specifics>
## Specific Ideas

- **Single `build_owned_blob` helper** (D-03) is the keystone; it eliminates 39× copy-paste of the `namespace_id + pubkey + sign` sequence. Mirrors node's `sync_protocol.h::NamespacedBlob` vocabulary so CLI↔node mental model is symmetric.
- **Auto-PUBK probe reuses Phase 117 ListRequest + type_filter** (D-01) — zero new node-side surface. Same pattern Phase 123 verified for NAME enumeration (reference: 123-03-SUMMARY + 123-VERIFICATION line 50 + 53).
- **Multi-node sync in E2E** (D-08 #2) is stronger than ROADMAP SC#7 literal — validates Phase 122's per-blob `[ns:32B]` sync prefix against real peer replication, not just single-node writes.
- **BOMB-cascade across CPAR** (D-06) closes an open defect flagged by Phase 123-03 as "Phase 124 is a natural home". Without this, `cdb rm <manifest>` in a batch leaves orphaned chunks on every node.
- **`MsgType::Data = 8` deletion** (D-04a) is the matching cleanup to node's Phase 122-05 "Pre-122 Data=8 branch DELETED". Keeps the CLI and node free of the old magic entirely — no shim, no transition path.
- **Error wording (D-05)** deliberately avoids the user-visible strings `PUBK_FIRST_VIOLATION` or `PUBK_MISMATCH` per `feedback_no_phase_leaks_in_user_strings.md`.

</specifics>

<deferred>
## Deferred Ideas

- **Persistent PUBK cache at `~/.chromatindb/pubk_cache.json`** — rejected in favor of in-process probe (D-01). Could be added later as a latency optimization if the probe RTT is measured and shown to matter; mirrors the same deferral logic as Phase 123 D-09 (no-name-cache).
- **`--as <owner_ns>` / `--delegate-for` explicit delegate flag** — rejected (D-02). If a future use case needs per-invocation delegate targeting beyond what `--share @contact` provides, revisit.
- **`default_delegate_namespace` in config.json** — service-account-style delegate machines. YAGNI for MVP; revisit if chromatindb ever has dedicated delegate-only operator machines.
- **Local-fork-a-node integration test harness** — overlaps with backlog phase 999.4 (cdb regression test suite). Phase 124 adds unit + mock tests only; full integration harness is its own phase.
- **Pipelined probe + optimistic write** — an optimization that fires the PUBK probe and the user's BlobWrite in parallel, canceling the write if the probe says PUBK is missing. Claude's discretion during planning — prefer the synchronous form first.
- **`MsgType::Delete = 17` removal from CLI** — depends on whether unification (D-04b) leaves any Delete callers. Claude's discretion.
- **`cdb status` / `cdb info --pubk` to query whether a namespace is PUBK'd on a given node** — operator diagnostic. Not needed for 124; could land as a small follow-on.
- **ErrorResponse mapping table as a shared header** — if the cli/node both grow bigger error-code tables, extract to a shared `cli/src/error_codes.h`. YAGNI for two new codes.

</deferred>

---

*Phase: 124-cli-adaptation-to-new-mvp-protocol*
*Context gathered: 2026-04-21*
