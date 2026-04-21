# Phase 124: CLI Adaptation to New MVP Protocol — Research

**Researched:** 2026-04-21
**Domain:** C++20 client code migration — wire schema, envelope routing, error surface, E2E orchestration
**Confidence:** HIGH (codebase fully inspected; node-side contracts cross-verified)

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

**D-01 (Auto-PUBK strategy):** Per-session, per-namespace, on-first-owner-write auto-PUBK. Probe via `ListRequest + type_filter=PUBK_MAGIC_CLI + namespace=target_namespace + limit=1`. If response has zero blobs, emit PUBK (data = `[PUBK:4][signing_pk:2592][kem_pk:1568]` = 4164 bytes, ttl=0, timestamp=now) via `BlobWrite=64`, wait for `WriteAck`, then proceed with user's blob. **In-process only**: `std::unordered_set<std::array<uint8_t,32>>` of known-PUBK'd namespaces within this invocation; no persistence.

**D-01a:** Auto-PUBK fires for OWNER writes only, never for delegate writes. Owner determined by `target_ns == SHA3(own_signing_pubkey)`. Delegate writes that hit a fresh-namespace node must fail cleanly via `PUBK_FIRST_VIOLATION`.

**D-02:** Delegate vs owner is implicit, derived at the helper layer. No new CLI flag. `signer_hint = SHA3(own_signing_pubkey)` always; distinction only matters on the node's verify path.

**D-03:** Single helper `build_owned_blob(id, target_namespace, data, ttl, timestamp) -> NamespacedBlob { target_namespace, BlobData blob }` in `cli/src/wire.{h,cpp}`. Computes `signer_hint = SHA3(id.signing_pubkey())`, builds signing input, signs, populates BlobData.

**D-03a:** `BlobData` struct becomes `{ std::array<uint8_t,32> signer_hint; std::vector<uint8_t> data; uint32_t ttl; uint64_t timestamp; std::vector<uint8_t> signature; }` — no `namespace_id`, no `pubkey`.

**D-03b:** `encode_blob` / `decode_blob` emit/parse post-122 FlatBuffer Blob table matching `db/schemas/blob.fbs`.

**D-04:** All blob writes go through `BlobWrite=64` envelope. Add `MsgType::BlobWrite=64`. Add `encode_blob_write_body(target_namespace, blob)` producing `BlobWriteBody` matching `db/schemas/transport.fbs`.

**D-04a:** `MsgType::Data=8` value DELETED from CLI enum.

**D-04b:** Tombstones (single `cmd::rm`) also ride `BlobWrite` envelope. CLI `MsgType::Delete=17` Claude's-discretion delete/keep.

**D-05:** CLI maps `ERROR_PUBK_FIRST_VIOLATION (0x07)` and `ERROR_PUBK_MISMATCH (0x08)` to user-facing wording (no leaks of internal tokens or phase numbers). Exact strings specified:
- 0x07 (auto-PUBK race): `"Error: namespace not yet initialized on node <host>. Auto-PUBK failed; try running 'cdb publish' first."`
- 0x08: `"Error: namespace <ns_short> is owned by a different key on node <host>. Cannot write."`

**D-06:** `cmd::rm_batch` cascades tombstones across CPAR manifests. For each target: if type is CPAR, fetch + decrypt manifest, add its chunk_hashes to BOMB targets. Result: one BOMB covering manifests + chunks.

**D-07:** User builds + deploys to local and home (192.168.1.73), wipes data dirs. Claude runs E2E after deploy (SSH to home when needed). User = build/deploy; Claude = verification.

**D-08:** E2E matrix — 7 gate items (SC#7 literal both nodes, cross-node sync, BOMB propagation, chunked >500 MiB, delegate `--share @contact`, `--replace`, D-06 cascade). Outputs in `.planning/phases/124-.../124-E2E.md`.

**D-09:** Surgical test updates to `test_wire.cpp` (blob roundtrip schema) + new TEST_CASEs for `BlobWriteBody` envelope, `build_owned_blob`, NAME/BOMB roundtrips, auto-PUBK probe logic.

### Claude's Discretion

- Return type of `build_owned_blob` (recommend `NamespacedBlob` struct vs `std::pair`).
- In-process auto-PUBK cache location (`Connection` member vs static).
- Delete `MsgType::Delete=17` from CLI enum (see Q3 below — evidence says KEEP).
- Where auto-PUBK probe lives (`Connection` method vs free function vs new module).
- Exact error-message wording within D-05 constraints.
- `test_auto_pubk.cpp` new file vs TEST_CASE in `test_wire.cpp`.
- D-06 cascade error-path behavior when a manifest fetch fails mid-batch.
- Synchronous vs optimistic pipelined probe.

### Deferred Ideas (OUT OF SCOPE)

- Persistent PUBK cache at `~/.chromatindb/pubk_cache.json`.
- `--as <owner_ns>` / `--delegate-for` explicit flag.
- `default_delegate_namespace` in config.json.
- Local-fork-a-node integration test harness (overlaps backlog 999.4).
- Pipelined probe + optimistic write.
- `MsgType::Delete=17` unconditional removal.
- `cdb status` / `cdb info --pubk` operator diagnostic.
- ErrorResponse mapping table extracted to shared header.
</user_constraints>

<phase_requirements>
## Phase Requirements

Roadmap gives no REQ-IDs for phase 124. Success Criteria SC#1..SC#7 (from `.planning/ROADMAP.md § Phase 124`) serve as the requirement set.

| ID | Description | Research Support |
|----|-------------|------------------|
| SC#1 | First `cdb` write to any fresh namespace auto-publishes PUBK before user's blob | Q4 (ListRequest template), Q5 (Connection lifecycle), Q1 (9 owner-write sites route via helper), Q12 (Identity::namespace_id unchanged) |
| SC#2 | `build_blob()` emits `signer_hint:32` only; old inline-pubkey path deleted entirely | Q1 (12 construction sites), Q2 (wire.h/wire.cpp shape today vs target), D-03a/D-03b |
| SC#3 | `build_signing_input()` matches final 122 canonical form | Q2 (semantic unchanged, only parameter rename `namespace_id` → `target_namespace`), `db/wire/codec.h:56-60` cross-ref |
| SC#4 | Delegate writes use `SHA3(delegate_pubkey)` as signer_hint; node resolves via delegation_map | Q1 (all owner + delegate writes funnel through D-03 helper, `signer_hint = SHA3(id.signing_pubkey())`), Q12 (Identity invariant) |
| SC#5 | `cdb put --name` / `cdb get <name>` / batched `cdb rm` all working E2E | Q1 (call-sites already wired for NAME/BOMB), Q6 (D-06 cascade evidence), D-08 E2E gates #1, #6, #7 |
| SC#6 | All existing CLI Catch2 tests pass under new wire format; new tests cover auto-PUBK, NAME, BOMB paths | Q8 (existing test harness patterns; `ScriptedSource` mock is reusable), D-09 |
| SC#7 | Live-node E2E verification against 192.168.1.73 (post-122+123 node): put → get → put --name → get <name> → rm → ls all correct | Q9 (`~/.cdb/config.json` already configured for `local`+`home`), D-07/D-08 |
</phase_requirements>

## Executive Summary

- **CONTEXT.md's "39 call sites" claim is inflated ~3x.** Grep of the actual codebase finds **12 `BlobData` construction sites total** (9 in `commands.cpp`, 3 in `chunked.cpp`). CONTEXT seems to have counted every `id.namespace_id()` / `id.signing_pubkey()` / `id.sign()` reference line, not distinct construction sites. The central helper (D-03) replaces **12 call sites**, not 39. `cli/src/commands.cpp:513,547,715,1232,1702,1748,2254,2386,2520` + `cli/src/chunked.cpp:104,125,321`.

- **The key template for auto-PUBK already exists.** `cli/src/commands.cpp:153-198` (`find_pubkey_blob`) is a complete ListRequest+type_filter=PUBK_MAGIC+ReadResponse flow using flags bit 1 (0x02). The `cmd::publish` pre-check at `:2500` already uses it. D-01 is structurally a **rewrite of the probe half of `find_pubkey_blob` as a cache-aware helper** plus a PUBK emit — not new architecture.

- **Error-code surface is entirely new.** All 5 current `ErrorResponse` handling sites in `commands.cpp` (`:1256, :1859, :2043, :2097, :2195`) print the same generic `"Error: node rejected request"` — they **never inspect the error-code byte**. D-05 needs to add a new helper that decodes the 2-byte `ErrorResponse` payload `[error_code:1][original_type:1]` and branches on codes `0x07` and `0x08` (and plausibly the 0x09/0x0A/0x0B BOMB codes from Phase 123). This is a greenfield mapping.

- **`MsgType::Delete=17` CANNOT be blanket-deleted.** The node dispatcher still routes `TransportMsgType_Delete` through `BlobWriteBody` envelope verification but emits `DeleteAck=18` (not `WriteAck=30`). CLI callers explicitly check for `DeleteAck` at `commands.cpp:560, 1261, 1775, 2404` + `chunked.cpp:425, 451`. If we switch every CLI tombstone send to `BlobWrite=64`, the node emits `WriteAck=30` (not `DeleteAck=18`) — which **breaks every `DeleteAck`-asserting call site**. Recommend **KEEP `MsgType::Delete=17` on CLI** until node side also unifies its ack type (which is out of scope for 124).

- **`ConnectOpts` is the natural carrier for the auto-PUBK cache, NOT `Connection`.** Connection is per-invocation (created+destroyed within each `cmd::*` function) so a member on it lasts exactly as long as one connection. `cmd::put` creates one Connection for the whole file loop (`commands.cpp:590`), so a Connection-scoped cache works for the batched-put case but NOT for any hypothetical future multi-connection flow. Cleaner option: a **thread-local static in a new `pubk_presence.{h,cpp}` module** that the helper reads/writes, keyed by target_namespace.

## Architectural Responsibility Map

| Capability | Primary Tier | Secondary Tier | Rationale |
|------------|-------------|----------------|-----------|
| Blob schema (signer_hint, no namespace_id) | CLI wire layer (`cli/src/wire.{h,cpp}`) | — | Byte-identical FlatBuffer emission matching node's `db/schemas/blob.fbs`; CLI and node maintain independent encoders per project convention |
| BlobWrite envelope build | CLI wire layer (`cli/src/wire.{h,cpp}`) | — | New `encode_blob_write_body` mirrors `db/wire/codec.h::encode_blob_write_envelope`; envelope shape is a wire contract |
| Owner/delegate classification | CLI helper layer (inside D-03 helper) | — | Implicit from `target_ns == SHA3(id.signing_pubkey())` comparison; no routing decision happens elsewhere |
| Auto-PUBK probe + emit | CLI command layer | CLI connection layer (cache state) | Probe uses existing `ListRequest+type_filter` primitive; emit uses the new D-03 helper; cache state needs a per-invocation home |
| Error-code decoding | CLI command layer | — | Node emits codes; CLI maps to user-facing strings — never leak internal codes or phase numbers |
| D-06 CPAR cascade | CLI command layer (`cmd::rm_batch`) | CLI chunked layer (`chunked::rm_chunked` logic extraction) | Batch orchestration is command-layer; the existing manifest decrypt+chunk-hash extraction is chunked-layer and should be factored out |
| E2E orchestration (local + home) | CLI user flow (Claude runs via `--node local` / `--node home`) | — | Already wired through `~/.cdb/config.json`; E2E is a verification activity, not a build activity |

## Per-Question Findings

### Q1. Call-site audit — the "39 call sites" claim is wrong

**Authoritative count, verified by grep of `BlobData\s+\w+`:**

| File | Line | Context | Current blob payload | Destination |
|------|------|---------|---------------------|-------------|
| `cli/src/commands.cpp` | 513 | `submit_name_blob` static helper | NAME payload | `MsgType::Data`, `conn.send` |
| `cli/src/commands.cpp` | 547 | `submit_bomb_blob` static helper | BOMB payload | `MsgType::Delete`, `conn.send` |
| `cli/src/commands.cpp` | 715 | `cmd::put` pipeline body (inside while loop) | CENV envelope data | `MsgType::Data`, `conn.send_async` |
| `cli/src/commands.cpp` | 1232 | `cmd::rm` fallback single-tombstone path | `make_tombstone_data(hash)` | `MsgType::Delete`, `conn.send` |
| `cli/src/commands.cpp` | 1702 | `cmd::reshare` — new CENV blob | Re-encrypted CENV envelope | `MsgType::Data`, `conn2.send` |
| `cli/src/commands.cpp` | 1748 | `cmd::reshare` — tombstone of old blob | `make_tombstone_data(old_hash)` | `MsgType::Delete`, `conn3.send` |
| `cli/src/commands.cpp` | 2254 | `cmd::delegate` loop body | `make_delegation_data(delegate_pk)` | `MsgType::Data`, `conn.send` |
| `cli/src/commands.cpp` | 2386 | `cmd::revoke` loop body | `make_tombstone_data(delegation_blob_hash)` | `MsgType::Delete`, `conn.send` |
| `cli/src/commands.cpp` | 2520 | `cmd::publish` | `make_pubkey_data(signing_pk, kem_pk)` | `MsgType::Data`, `conn.send` |
| `cli/src/chunked.cpp` | 104 | `build_cdat_blob_flatbuf` (file-local static helper) | `[CDAT:4]+CENV` | Helper returns `encode_blob` bytes; caller sends via `MsgType::Data` |
| `cli/src/chunked.cpp` | 125 | `build_tombstone_flatbuf` (file-local static helper) | `make_tombstone_data(target)` | Helper returns bytes; caller sends via `MsgType::Delete` |
| `cli/src/chunked.cpp` | 321 | `put_chunked` manifest build | `[CPAR:4]+CENV(manifest)` | `MsgType::Data`, `conn.send_async` |

**Total: 12 construction sites.** CONTEXT's "~39 call sites" is wrong — it conflated distinct blob constructions with identity-method references (`id.namespace_id()` appears ~10 times at 54, 379, 389, 587, 2169, 2243, 2299, 2493 etc., but most of those are READING the caller's own namespace, not constructing blobs).

**CONTEXT line refs that don't match:** Lines `:514-515, :548-549, :716-717, :1233-1234, :1703-1704, :1749-1750, :2255-2256, :2387-2388, :2521-2522` → these are the 9 `commands.cpp` sites (off-by-one from the `BlobData ... {}` declaration line, but pointing at the `blob.namespace_id` / `blob.pubkey` assignment pair right below it). That part of CONTEXT is accurate.

**Each site is NEARLY identical, with three variations:**

1. **`blob.namespace_id = ns;`** (line 1233, 1703, 1749 — where `ns` is already a `std::array<uint8_t,32>`)
2. **`std::memcpy(blob.namespace_id.data(), ns.data(), 32);`** (all other sites — where `ns` is a `std::span<const uint8_t,32>`)
3. **`submit_name_blob` / `submit_bomb_blob` at 513/547** already wrap the pattern into static helpers — D-03's `build_owned_blob` is essentially generalizing these two.

**Conclusion:** The D-03 helper replaces **12 distinct constructions**. The signing-input computation (`build_signing_input(ns, data, ttl, ts)` + `id.sign(signing_input)` + `blob.signature = std::move(signature)`) is the same 3-line prefix at every single site. One helper absorbs all of them.

**Risks:** `cmd::reshare` at 1702/1748 uses **three separate Connections** (conn1, conn2, conn3 — legacy pattern). The auto-PUBK cache scope needs to survive across those 3 connections in a single invocation, which is an argument for **invocation-scoped static** over Connection-scoped.

### Q2. Current wire.h/wire.cpp shape vs target

**Current `cli/src/wire.h:132-139` `BlobData`:**
```cpp
struct BlobData {
    std::array<uint8_t, 32> namespace_id{};
    std::vector<uint8_t> pubkey;       // REMOVE
    std::vector<uint8_t> data;
    uint32_t ttl = 0;
    uint64_t timestamp = 0;
    std::vector<uint8_t> signature;
};
```

**Target (byte-identical to `db/wire/codec.h:18-24`):**
```cpp
struct BlobData {
    std::array<uint8_t, 32> signer_hint{};  // renamed; 32 bytes always
    std::vector<uint8_t> data;
    uint32_t ttl = 0;
    uint64_t timestamp = 0;
    std::vector<uint8_t> signature;
};
```

**FlatBuffer vtable changes — `cli/src/wire.cpp:24-31`:**
```cpp
namespace blob_vt {
    constexpr flatbuffers::voffset_t NAMESPACE_ID = 4;  // DELETE (or RENAME to SIGNER_HINT)
    constexpr flatbuffers::voffset_t PUBKEY       = 6;  // DELETE — field gone
    constexpr flatbuffers::voffset_t DATA         = 8;  // → becomes 6 (slot shift)
    constexpr flatbuffers::voffset_t TTL          = 10; // → 8
    constexpr flatbuffers::voffset_t TIMESTAMP    = 12; // → 10
    constexpr flatbuffers::voffset_t SIGNATURE    = 14; // → 12
}
```

Cross-reference the node's generated `db/wire/blob_generated.h` if needed — both sides hand-code their vtables. **Critical:** vtable offsets must match `db/schemas/blob.fbs` (5 fields, slots 4/6/8/10/12). The CLI currently has 6 fields with offsets 4/6/8/10/12/14; after removing 2 and keeping 5, offsets become 4/6/8/10/12. If slot 4 is `signer_hint` (the first field in the new schema), that matches node. **Must verify against node-generated `blob_generated.h` before coding.**

**`encode_blob` implementation changes (`wire.cpp:170-193`):**
- Remove `builder.CreateVector(blob.namespace_id.data(), blob.namespace_id.size())` call
- Remove `builder.CreateVector(blob.pubkey.data(), blob.pubkey.size())` call
- Add `builder.CreateVector(blob.signer_hint.data(), blob.signer_hint.size())` call (note 32-byte fixed)
- Remove `builder.AddOffset(blob_vt::NAMESPACE_ID, ns)`
- Remove `builder.AddOffset(blob_vt::PUBKEY, pk)`
- Add `builder.AddOffset(blob_vt::SIGNER_HINT, sh)`

**`decode_blob` (wire.cpp:195-234):**
- Remove pubkey read (`table->GetPointer(blob_vt::PUBKEY)` → assigns to `result.pubkey`)
- Remove namespace_id read block (lines 210-213)
- Add signer_hint read: `auto* sh = table->GetPointer<const flatbuffers::Vector<uint8_t>*>(blob_vt::SIGNER_HINT); if (sh && sh->size() == 32) std::memcpy(result.signer_hint.data(), sh->data(), 32);`

**`build_signing_input` (wire.cpp:240-265):** No semantic change. Parameter rename only:
```cpp
std::array<uint8_t, 32> build_signing_input(
    std::span<const uint8_t, 32> target_namespace,  // was: namespace_id
    std::span<const uint8_t> data,
    uint32_t ttl,
    uint64_t timestamp);
```
The body already absorbs `target_namespace.data(), target_namespace.size()` → 32 bytes, followed by data → ttl_be → ts_be. Byte output IDENTICAL to today for same inputs. (Node's `db/wire/codec.h:50-60` comment explicitly states: "byte output IDENTICAL to pre-122 for the same input bytes; only the parameter name changes.")

**`MsgType` enum changes (`wire.h:69-88`):**
- DELETE `Data = 8` (D-04a)
- KEEP `Delete = 17` (see Q3 below — necessary for DeleteAck routing)
- ADD `BlobWrite = 64` after `ErrorResponse = 63`

**New helper to add:**
```cpp
std::vector<uint8_t> encode_blob_write_body(
    std::span<const uint8_t, 32> target_namespace,
    const BlobData& blob);
```
Must match `db/wire/codec.h:46-48` byte-for-byte (which internally calls `encode_blob_write_envelope` — note the naming inconsistency: CONTEXT says `encode_blob_write_body` but node's function is `encode_blob_write_envelope`; recommend **CLI uses `encode_blob_write_body` matching the FlatBuffer table name `BlobWriteBody`** for symmetry with node's `BlobWriteBody` type; the wrapper function name is incidental).

### Q3. `MsgType::Data = 8` and `MsgType::Delete = 17` post-unification

**`Data = 8` callers (8 sites — all send paths):**

| File:line | Function | Send call |
|-----------|----------|-----------|
| `commands.cpp:523` | `submit_name_blob` | `conn.send(MsgType::Data, ...)` |
| `commands.cpp:726` | `cmd::put` | `conn.send_async(MsgType::Data, ...)` |
| `commands.cpp:1718` | `cmd::reshare` | `conn2.send(MsgType::Data, ...)` |
| `commands.cpp:2264` | `cmd::delegate` | `conn.send(MsgType::Data, ...)` |
| `commands.cpp:2536` | `cmd::publish` | `conn.send(MsgType::Data, ...)` |
| `chunked.cpp:227,245` | `put_chunked` (Phase A + retry) | `conn.send_async(MsgType::Data, ...)` |
| `chunked.cpp:331` | `put_chunked` manifest | `conn.send_async(MsgType::Data, ...)` |

**All 8 sites MUST migrate to `MsgType::BlobWrite = 64` per D-04.** Enum value removal (D-04a) is then safe.

**`Delete = 17` callers (9 sites):**

| File:line | Function | Send/recv | Ack type expected |
|-----------|----------|-----------|-------------------|
| `commands.cpp:557` | `submit_bomb_blob` | `conn.send` | `DeleteAck` or `WriteAck` (both accepted at :560) |
| `commands.cpp:1242` | `cmd::rm` single-tombstone | `conn.send` | `DeleteAck` at :1261 |
| `commands.cpp:1765` | `cmd::reshare` tombstone of old | `conn3.send` | `DeleteAck` at :1775 |
| `commands.cpp:2396` | `cmd::revoke` | `conn.send` | `DeleteAck` at :2404 |
| `chunked.cpp:393` | `rm_chunked` chunk tombstones | `conn.send_async` | `DeleteAck` at :425 |
| `chunked.cpp:446` | `rm_chunked` manifest tombstone | `conn.send_async` | `DeleteAck` at :451 |

**Critical evidence that `Delete = 17` must stay:**

The node dispatcher at `db/peer/message_dispatcher.cpp:374` for `TransportMsgType_Delete` sends `TransportMsgType_DeleteAck` back:
```cpp
co_await conn->send_message(wire::TransportMsgType_DeleteAck,
                             std::span<const uint8_t>(ack_payload), request_id);
```

For `TransportMsgType_BlobWrite` at `:1429`, the node sends `TransportMsgType_WriteAck`:
```cpp
co_await conn->send_message(wire::TransportMsgType_WriteAck,
                             std::span<const uint8_t>(ack_payload), request_id);
```

**Therefore:** If we rewrite every tombstone call site to send via `BlobWrite=64`, every single `DeleteAck` check becomes a `WriteAck` check. That's 6 `resp->type != DeleteAck` comparison sites + error messages saying "DeleteAck" that become lies. Cross-cutting change.

**Recommendation:** **Keep `MsgType::Delete = 17`.** The CONTEXT's D-04b says "Tombstones also ride the BlobWrite envelope" which is true at the FlatBuffer level — both `Delete` and `BlobWrite` at the node side accept `BlobWriteBody` as payload. But the **TransportMsgType choice picks the ack type**, and CLI distinguishes `DeleteAck` from `WriteAck`. Unifying on BlobWrite alone requires either:
- (a) Changing node to emit `WriteAck` for Delete too (out of scope for 124 — it's a node change; contradicts CONTEXT's `This phase does NOT` list).
- (b) Changing CLI to accept `WriteAck` uniformly for tombstones (6 sites of drift, doesn't match node responses on the wire).

**Resolution:** `MsgType::Delete = 17` stays in the CLI enum. Every existing `conn.send(MsgType::Delete, flatbuf, rid)` call will now send a `BlobWriteBody`-shaped payload (via the D-03 helper's output fed through `encode_blob_write_body`), not a raw `encode_blob` payload. The envelope payload changes; the `MsgType` used on the wire does not. **This is the single most important clarification the planner needs** — CONTEXT D-04b is ambiguous here, and the obvious reading (replace Delete with BlobWrite) is wrong.

### Q4. ListRequest + type_filter template for auto-PUBK

**Canonical template: `cli/src/commands.cpp:153-198` (`find_pubkey_blob`).** This function already does exactly what D-01's probe needs (minus caching). Complete layout:

**ListRequest payload (49 bytes total, per Phase 117 D-07/D-09):**
```
[0..31]   namespace_id           (32 bytes)
[32..39]  since_seq              (8 bytes BE) — zero for probe
[40..43]  limit                  (4 bytes BE) — "1" for PUBK probe
[44]      flags                  (1 byte)    — 0x02 = type_filter present
[45..48]  type_filter            (4 bytes)   — PUBKEY_MAGIC = "PUBK"
```

**ListResponse layout:**
```
[0..3]                              count (4 bytes BE)
[4..(4+count*60-1)]                 entries (count × 60 bytes)
  per entry:
    [0..31]   blob_hash           (32 bytes)
    [32..39]  seq                 (8 bytes BE)
    [40..43]  type                (4 bytes) — blob's 4-byte magic prefix
    [44..51]  data_size           (8 bytes BE)
    [52..59]  timestamp           (8 bytes BE)
[4+count*60]                        has_more (1 byte)
```

**NOTE: Entry size is 60 bytes, not 44.** `LIST_ENTRY_SIZE` is hardcoded at `commands.cpp:109` as `60`. Phase 117 added size + timestamp columns. CONTEXT correctly references this.

**Critical: node `MAX_LIST_LIMIT = 100`** (`db/peer/message_dispatcher.cpp:507`). Auto-PUBK probe requests limit=1; node will return at most 1 entry if present. No pagination needed for the probe — if count=0, no PUBK exists.

**Two ListRequest sites in CLI today:**

1. **`find_pubkey_blob` (`:156-163`)** — limit=100, flag=0x02 (type_filter), PUBK filter. Used by `cmd::publish` (`:2500`) as pre-check.
2. **`enumerate_name_blobs` (`:1437-1489`)** — limit=1000 (server clamps to 100), paginated loop over `since_seq`, flag=0x02, NAME filter. Used by `resolve_name_to_target_hash` for Phase 123 name enumeration.

**Difference:** `find_pubkey_blob` does NOT paginate; it reads only the first page and treats "empty" as "no PUBK exists." This is what D-01's probe wants — single-page, limit=1, boolean-ish result.

**Implementation template for auto-PUBK probe (per D-01):**
```cpp
static bool namespace_has_pubk(Connection& conn,
                                std::span<const uint8_t, 32> ns,
                                uint32_t rid) {
    std::vector<uint8_t> payload(49, 0);
    std::memcpy(payload.data(), ns.data(), 32);
    // since_seq = 0 (already zero)
    store_u32_be(payload.data() + 40, 1);       // limit=1
    payload[44] = 0x02;                          // type_filter present
    std::memcpy(payload.data() + 45, PUBKEY_MAGIC.data(), 4);

    if (!conn.send(MsgType::ListRequest, payload, rid)) return false;  // treat as "unknown"
    auto resp = conn.recv();
    if (!resp || resp->type != static_cast<uint8_t>(MsgType::ListResponse) ||
        resp->payload.size() < 5) return false;

    uint32_t count = load_u32_be(resp->payload.data());
    return count > 0;
}
```

Then: if `namespace_has_pubk` returns false AND `target_ns == SHA3(id.signing_pubkey())`, emit a PUBK blob via `build_owned_blob` → `BlobWrite=64` → wait for `WriteAck` → then proceed with user blob.

**Hidden subtlety: the probe uses a CLI-local RID allocation that must not collide with the caller's RID counter.** `enumerate_name_blobs` handles this by starting at `0x1000` (`commands.cpp:1443`). Auto-PUBK probe should follow suit — e.g., start at `0x3000` or accept a `uint32_t& rid` and advance.

### Q5. Connection / in-process state — lifecycle and threading

**Connection lifecycle:** Per-invocation. Every `cmd::*` function constructs a new `Connection conn(id)` on the stack, connects, runs its flow, and closes. `cli/src/commands.cpp:590,877,1108,1341,1592,1636,1712,1758,1840,1968,2023,2077,2175,2237,2301,2430,2498,2530,2574` — ~20 sites create local `Connection` instances.

**Multi-connection invocations:**
- `cmd::reshare` (`:1636,1712,1758`) creates `conn1`, `conn2`, `conn3` sequentially — three connections across one invocation.
- `cmd::publish` (`:2498,2530`) creates `check_conn` (for PUBK pre-check), closes, then creates `conn` for the write.
- Other commands use one Connection per invocation.

**Threading model:** `cdb` is single-threaded (evidence: `Connection::recv_for`'s comment about `Single-sender/single-reader invariant (PIPE-02)`, `cli/src/connection.h:62`). Each `cdb` invocation runs one asio `io_context` (`Connection::ioc_`) in what's effectively blocking mode from the caller's perspective — all `send`/`recv` calls are synchronous wrappers around asio futures.

**No existing per-connection cache for PUBK state.** No field named `pubk_seen`, `pubk_ensured`, or similar. Grep confirmed.

**Implications for D-01's in-process cache:**

| Scope option | Pro | Con |
|--------------|-----|-----|
| **Connection member (CONTEXT suggestion)** | Minimal coupling; destroyed with Connection | Dies across `cmd::reshare`'s 3 connections; dies across `cmd::publish`'s 2 connections |
| **`ConnectOpts` field** | Lives for the whole invocation | Mixing connection config with runtime state is ugly |
| **Thread-local static in a new module (`cli/src/pubk_presence.{h,cpp}`)** | Lives across all Connections within one invocation; explicit module boundary | Slight globals smell, but `cdb` is single-threaded so thread_local = process_local |
| **Static inside the D-03 helper itself** | No new module | Static inside a wire helper mixes concerns (wire encoding ≠ network probe state) |

**Recommendation:** `cli/src/pubk_presence.{h,cpp}` with a function `bool ensure_pubk(Identity& id, Connection& conn, std::span<const uint8_t,32> target_ns)` that owns the static `std::unordered_set<std::array<uint8_t,32>>` and performs the probe-then-emit logic. Thread-local or file-scope static; both work since cdb is process-per-invocation.

**One caveat:** `cmd::reshare`'s second+third connections (put + delete) would also trigger auto-PUBK on the user's own namespace. With an invocation-scoped cache, the first `find_pubkey_blob`-equivalent call caches the result and the later connections skip re-probing. 

### Q6. D-06 BOMB cascade — CPAR manifest shape

**The machinery already exists — it just hasn't been lifted into `rm_batch`.**

**Current single-target `cmd::rm` flow (`commands.cpp:1102-1221`):**
1. `ExistsRequest` pre-check (unless `--force`).
2. `ReadRequest` to fetch full blob (extra RTT, documented at :1159 as accepted cost).
3. `decode_blob` → check `target_blob->data[0..3]` magic:
   - CDAT magic → error ("Remove the CPAR manifest instead").
   - CPAR magic → envelope::decrypt the manifest bytes (lines 1191-1196, `id.kem_seckey()`, `id.kem_pubkey()`), then `decode_manifest_payload(*plain)` (wire.cpp:438), then delegate to `chunked::rm_chunked(id, ns, *manifest, target_hash, conn, opts)`.
   - Other → fallthrough to single-tombstone path.

**`chunked::rm_chunked` (`chunked.cpp:362-458`):** Pipelined fan-out of per-chunk `Delete` messages, then a final `Delete` for the manifest itself. Uses the existing Phase 120 `send_async` + `recv_next` primitive.

**Manifest decrypt is already in place.** The existing flow decrypts manifests inside `cmd::rm` single-target. D-06 just needs to:

1. **Factor the "fetch-and-classify-target" block at `commands.cpp:1162-1221` into a shared helper** (e.g., `classify_rm_target(conn, id, ns, target_hash) → {Plain, CDAT, CPAR_with_chunk_hashes}`). Apply `feedback_no_duplicate_code.md`.
2. **In `cmd::rm_batch`**, for each target: classify it first. Collect all CPAR chunk hashes into an expanded target list. Build ONE BOMB with `(manifests + all chunk hashes)`.

**Pipelining the manifest fetches:** Phase 120's `send_async` + `recv_for` works — each `ReadRequest` can be pipelined with a batch-local rid map, mirror `cmd::get` at `:893-976`. Depth = `Connection::kPipelineDepth = 8`.

**Claude's-discretion resolution — partial cascade semantics:**

Three options:
1. **Fail the whole batch** if any manifest fetch fails → conservative but loses other targets on one bad manifest.
2. **BOMB only the manifest hash** (treat as opaque) if cascade fetch fails → orphaned chunks on the peer (the bug this phase fixes!).
3. **Partial cascade + warning** — BOMB what we could cascade, warn the user about the failed ones → practical but requires nuanced UX.

**Recommendation:** Option 3, with a structured log line per failure. User-visible summary: `"cascade: N manifests fully tombstoned, M manifests failed to fetch (manifest-only tombstone emitted)"`. Parked as discretion; planner picks.

**Pipelining decision:** Existing `rm_batch` at `:1353-1386` runs `ExistsRequest` in a serial loop (not pipelined). Adding classification+pipelining requires touching this loop. Alternative: do classification+cascade-expansion pre-loop, then fire the BOMB once. Per-file cost is ~2 RTTs per CPAR target (ExistsRequest + ReadRequest); pipelining saves most of that.

### Q7. Error-response mapping — greenfield work

**Node error-code wire layout (`db/peer/message_dispatcher.cpp:61-78`):**
```
ErrorResponse payload = [error_code:1][original_type:1]  // 2 bytes total
```

**Node error codes (`db/peer/error_codes.h:10-20`):**
| Code | Name | Phase | Trigger |
|------|------|-------|---------|
| 0x01 | `ERROR_MALFORMED_PAYLOAD` | existing | payload too short/malformed |
| 0x02 | `ERROR_UNKNOWN_TYPE` | existing | TransportMsgType not recognized |
| 0x03 | `ERROR_DECODE_FAILED` | existing | FlatBuffer verify failed |
| 0x04 | `ERROR_VALIDATION_FAILED` | existing | signature/constraints failed |
| 0x05 | `ERROR_INTERNAL` | existing | server-side exception |
| 0x06 | `ERROR_TIMEOUT` | existing | timeout |
| **0x07** | **`ERROR_PUBK_FIRST_VIOLATION`** | **Phase 122** | **fresh namespace, non-PUBK first write** |
| **0x08** | **`ERROR_PUBK_MISMATCH`** | **Phase 122** | **PUBK sent by different signing key** |
| **0x09** | **`ERROR_BOMB_TTL_NONZERO`** | **Phase 123** | **BOMB with ttl != 0** |
| **0x0A** | **`ERROR_BOMB_MALFORMED`** | **Phase 123** | **BOMB structural invalid** |
| **0x0B** | **`ERROR_BOMB_DELEGATE_NOT_ALLOWED`** | **Phase 123** | **delegate tried to BOMB** |

**Current CLI handling of `ErrorResponse` — 5 sites, all identical, NONE inspect the code byte:**

| Site | File:line | Code |
|------|-----------|------|
| `cmd::rm` post-DeleteAck check | `commands.cpp:1256` | `std::fprintf(stderr, "Error: node rejected request\n"); return 1;` |
| `cmd::ls` | `commands.cpp:1859` | identical string |
| `cmd::exists` | `commands.cpp:2043` | identical string |
| `cmd::info` | `commands.cpp:2097` | identical string |
| `cmd::stats` | `commands.cpp:2195` | identical string |

**Observations:**
- No call site in `cmd::put`, `cmd::publish`, `cmd::delegate`, `cmd::revoke`, `cmd::rm_batch`, `chunked::put_chunked`, or `chunked::rm_chunked` handles `ErrorResponse` **at all**. They check `resp->type != WriteAck` or `!= DeleteAck` and treat anything else as "bad response." **Error codes never reach the user as specific messages.**
- After Phase 124, the auto-PUBK probe prevents `ERROR_PUBK_FIRST_VIOLATION` on the happy path for owner writes, but delegate writes to an unpublished namespace or owner-PUBK-after-registration can still trigger it.

**D-05 implementation: add a centralized error decoder.** Recommend a new helper in `cli/src/commands.cpp` (or a tiny new `cli/src/error_map.{h,cpp}`):

```cpp
/// Decode an ErrorResponse payload into a user-facing error message.
/// Never leak phase numbers or internal tokens (feedback_no_phase_leaks_in_user_strings.md).
/// Returns (human_message, exit_code). Exit code always nonzero.
std::pair<std::string, int> decode_error_response(std::span<const uint8_t> payload,
                                                   const std::string& host_hint,
                                                   std::span<const uint8_t, 32> ns_hint);
```

Cases:
- `payload.size() < 2` → `"Error: node returned malformed ErrorResponse"` (`1`)
- `code == 0x07` → D-05 string for PUBK_FIRST_VIOLATION
- `code == 0x08` → D-05 string for PUBK_MISMATCH
- `code == 0x09` → `"Error: batch deletion rejected (BOMB must be permanent)"` (defense-in-depth; should never fire since CLI always emits BOMB with ttl=0)
- `code == 0x0A` → `"Error: batch deletion rejected (malformed BOMB payload)"`
- `code == 0x0B` → `"Error: delegates cannot perform batch deletion on this node"`
- default → `"Error: node rejected request (code 0x%02X)"` — debug-level log includes the `error_code_name` style string but never bleeds into the user-facing message unless the user passes `-v` or similar (confirm CLI has no phase-leaking `--verbose` output; audit main.cpp).

**Every existing `Error: node rejected request` string in commands.cpp must be replaced** with a call to `decode_error_response`. That's the 5 explicit sites + every site that currently says `"bad response"` and ALSO receives an `ErrorResponse` type.

### Q8. Test harness patterns — Catch2 precedent

**File structure:** Each `cli/tests/test_*.cpp` starts with:
```cpp
#include <catch2/catch_test_macros.hpp>
#include "cli/src/<module>.h"
// optional: sodium, cstring, etc.
using namespace chromatindb::cli;
TEST_CASE("<module>: <description>", "[<tag>]") { ... REQUIRE(...); ... }
```

Tags in use: `[wire]`, `[pipeline]`, `[envelope]`, `[identity]`, `[contacts]`, `[chunked]`. Auto-PUBK tests should probably use `[wire]` (schema parts) + a new `[pubk]` tag for the cache logic.

**Mockable transport pattern — `ScriptedSource` in `cli/tests/pipeline_test_support.h`.** This is EXACTLY what D-09's auto-PUBK tests need:

```cpp
struct ScriptedSource {
    std::deque<DecodedTransport> queue;
    bool dead = false;
    int call_count = 0;
    std::optional<DecodedTransport> operator()();  // FIFO drain; nullopt when empty
};
```

With a `make_reply(rid, type, payload)` factory. Tests in `test_connection_pipelining.cpp` exercise `pipeline::pump_recv_for` and `pipeline::pump_recv_any` via this mock. The auto-PUBK probe logic, IF extracted into a pure function that takes a `Source&&` callable (like `pump_recv_*`), could be tested the same way:

```cpp
// Hypothetical testable shape:
template <typename Sender, typename Receiver>
bool probe_pubk(std::span<const uint8_t, 32> ns, Sender&& send, Receiver&& recv);
```

**Alternative:** A tighter integration-style test that takes a `Connection&` and passes a stub Connection. But Connection has a real asio::io_context and socket — hard to stub. The Source-callable pattern mirrors how 120-01 made pipelining testable.

**Recommendation:** Extract the auto-PUBK probe into `cli/src/pubk_presence.h` as a template taking `Sender` + `Receiver` callables (or a wrapper struct). Write unit tests in `test_auto_pubk.cpp` (new file) or a `[pubk]`-tagged block in `test_wire.cpp` — planner picks per CONTEXT D-09's suggestion (crossover ~5 TEST_CASEs).

**Test coverage targets for the probe logic:**
1. First write triggers probe; empty ListResponse triggers PUBK emit.
2. First write triggers probe; non-empty ListResponse skips PUBK emit.
3. Second write to same namespace within invocation skips probe (cache hit).
4. Write to different namespace within invocation triggers new probe.
5. Delegate write (`target_ns != SHA3(own_sp)`) skips probe entirely, even on first invocation.
6. Probe transport error (source dead) propagates as hard error to caller (CONTEXT D-01 says "optimistic emit on probe failure" — verify actual behavior matches).

### Q9. Live-node E2E tooling — `--node local` / `--node home`

**Config file `~/.cdb/config.json` (confirmed present at user's machine):**
```json
{
  "nodes": {
    "home": "192.168.1.73",
    "local": "127.0.0.1"
  },
  "default_node": "home"
}
```

**Resolution order (`cli/src/main.cpp:192-215`):** `--node <name>` flag > `default_node` from config > legacy flat `host`/`port`.

**Port resolution:** `parse_host_port("192.168.1.73", opts.host, opts.port)` — function sets `opts.host = "192.168.1.73"` and `opts.port = <default>`. (The default port is likely 6742; need to spot-check `cli/src/main.cpp` for `opts.port` initialization.)

**How Claude runs E2E against `home`:**
- From Claude's local shell: `cdb --node home <cmd>` establishes a TCP + PQ handshake to `192.168.1.73:6742`. No SSH required — `cdb` is a direct TCP client.
- From local: `cdb --node local <cmd>` hits `127.0.0.1`.

**Tested endpoint (from memory `reference_live_node.md`):** Node at `192.168.1.73` is reachable for testing. Phase 999.8 backlog notes PQ handshake can stall without bounded timeout — E2E should set a reasonable overall timeout per command.

**E2E matrix invocation examples:**
```bash
# E2E item 1 (SC#7 literal, both nodes)
cdb --node local publish
cdb --node local put /tmp/file1.bin          # expect: hash1
cdb --node local get <hash1> --out /tmp/out1 # expect: byte-identical
cdb --node local put --name foo /tmp/file1.bin   # expect: NAME binding
cdb --node local get foo --out /tmp/out2          # expect: byte-identical
cdb --node local rm <hash1> <hashN>            # batched BOMB
cdb --node local ls                             # expect: no hidden PUBK/CDAT/DLGT/NAME

# repeat every line with --node home

# E2E item 2 (cross-node sync)
cdb --node local put /tmp/xfer.bin            # hash X
# wait for sync interval
cdb --node home ls | grep <hash X>             # expect: present
```

**D-08 items 4, 5, 7 require additional state:**
- Item 4 (chunked): need a >500 MiB file on disk.
- Item 5 (delegate): need a delegate identity; the owner must have already registered both PUBK + delegation on the home node.
- Item 7 (D-06 cascade): put a chunked file → batched `rm` with other targets → verify chunks gone via `ls --type CDAT` (or `ls --raw`).

**Ingested from MEMORY:** User is responsible for build + deploy; Claude self-verifies against reachable infrastructure. For the home node, `cdb --node home` over TCP handles everything — no SSH needed except if manually restarting the daemon remotely (out of 124 scope).

### Q10. Validation Architecture (Nyquist)

See dedicated `## Validation Architecture` section below.

### Q11. Pitfalls / landmines

**Project-memory constraints that affect this phase:**

1. **Coroutine container invalidation:** Every `co_await` is a potential container invalidation point (MEMORY). The CLI is effectively single-threaded but uses coroutines under the hood in `Connection::send_async`. Auto-PUBK's probe + emit sequence must not hold iterators or spans into containers across awaits. Since CLI's auto-PUBK is synchronous (probe → wait → emit → wait → user blob), this is naturally avoided IF the probe uses `conn.send()` + `conn.recv()` (fully blocking from caller) — NOT `send_async` + `recv_for`. **Recommend synchronous primitives for D-01, matching CONTEXT's "Prefer the synchronous form for correctness."**

2. **ML-DSA-87 signatures non-deterministic (MEMORY):** Same data signed twice produces different signatures → different blob_hash. Every PUBK emission produces a fresh blob_hash. Per Phase 122 D-04 (idempotent PUBK accept), node accepts repeat PUBKs from the same signing key but the hashes differ. This is fine for the auto-PUBK flow — we're not hash-asserting; we're type-filtering.

3. **FlatBuffer Builder lifetime (MEMORY):** Don't pass a `FlatBufferBuilder` into an async call; the builder owns the bytes. The existing `encode_blob` / `encode_transport` return `std::vector<uint8_t>` by value, which is safe — they detach the bytes from the builder. The new `encode_blob_write_body` should follow the same pattern.

4. **Per-connection send queue drain (MEMORY):** The node sends `SyncNamespaceAnnounce(62)` after every `TrustedHello` — clients must drain. CLI already handles this: `Connection::drain_announce()` (`connection.cpp:603`). No regression risk in Phase 124 as long as new code doesn't start a fresh Connection without calling `drain_announce` in the handshake path. Handshake paths are not touched.

5. **Wire format payload sizes MUST match node dispatcher checks (MEMORY):** For the new `BlobWrite=64` envelope, the node's `message_dispatcher.cpp:1385-1507` verifies payload with `flatbuffers::Verifier::VerifyBuffer<wire::BlobWriteBody>`. The CLI's `encode_blob_write_body` must produce bytes that pass this verifier — hand-coded vtable offsets must match `BlobWriteBody`'s two slots (`target_namespace`, `blob`). Cross-reference `db/wire/transport_generated.h:372-389` for exact offsets.

6. **Big-endian everywhere (MEMORY):** `store_u64_be(payload + 32, since_seq)` in the probe, `store_u32_be(payload + 40, 1)` for limit. Already covered by `cli/src/wire.h`'s BE helpers.

7. **Ordering constraint: auto-PUBK emit MUST complete (WriteAck received) before user blob is sent.** Otherwise the node rejects the user blob with `ERROR_PUBK_FIRST_VIOLATION` because the PUBK hasn't been committed yet. This is a hard invariant: **not pipelined**. The sequence is strictly serial within an invocation: probe → [if absent: sign+send PUBK, wait for WriteAck] → sign+send user blob.

8. **ttl=0 for BOMB (MEMORY + `project_phase123_bomb_ttl_zero.md`):** All BOMBs must have ttl=0. CLI already enforces this at `commands.cpp:551` (`submit_bomb_blob`). Auto-PUBK flow must not somehow turn a PUBK into a BOMB or vice versa — they have distinct magic bytes; no crossover risk.

9. **PUBK-first is node-enforced (MEMORY `project_phase122_pubk_first_invariant.md`):** Auto-PUBK is the cooperative half, not a replacement. Even if CLI's probe returns "PUBK exists," the node can reject anyway if its view has drifted (sync lag, concurrent peer writes). Always surface the node's response; never assume probe success means write success.

10. **Non-deterministic FlatBuffer across languages (MEMORY):** Irrelevant here (CLI and node are both C++), but the WriteAck parsing uses server-returned `blob_hash` from `resp->payload.data()[0..32]`, not client-computed. Already correct at every site.

**`cli/`-specific patterns:**
- `Connection::recv_next()` decrements `in_flight_`; `Connection::recv_for(rid)` also decrements on match. Don't mix them in the same drain — use one per batch.
- All CLI `MsgType` enum values are 1-byte; match `TransportMsgType` values in `db/schemas/transport.fbs`.
- Contact lookup via `ContactDB` at `identity_dir + "/contacts.db"` — not used in blob construction, but downstream tasks may confuse "namespace resolution" (receiver) with "signer_hint" (sender).

### Q12. Identity / signing invariants

**`Identity::namespace_id()` (`identity.h:37-39`, `identity.cpp:28-30`):**
```cpp
std::span<const uint8_t, 32> namespace_id() const {
    return std::span<const uint8_t, 32>(namespace_id_.data(), 32);
}
// derive_namespace in .cpp: OQS_SHA3_sha3_256(out.data(), signing_pk.data(), signing_pk.size());
```

Confirmed: namespace = SHA3-256(signing_pubkey), 32 bytes. Unchanged by Phase 122/123/124. Test coverage in `test_identity.cpp:82-131` (two distinct identities produce different namespaces).

**`id.sign(msg)` (`identity.cpp:116-136`):** ML-DSA-87 via `OQS_SIG_sign`, returns `std::vector<uint8_t>` of length `sig->length_signature` (up to 4627 bytes). This is the byte layout `build_owned_blob` needs for `BlobData.signature`.

**`id.signing_pubkey()` (`identity.h:33`):** 2592 bytes, the ML-DSA-87 public key.

**D-03 helper composes these correctly:**
```cpp
NamespacedBlob build_owned_blob(const Identity& id,
                                 std::span<const uint8_t, 32> target_namespace,
                                 std::span<const uint8_t> data,
                                 uint32_t ttl,
                                 uint64_t timestamp) {
    auto signer_hint = sha3_256(id.signing_pubkey());  // 32 bytes; SAME as id.namespace_id() for owner writes
    auto signing_input = build_signing_input(target_namespace, data, ttl, timestamp);
    auto signature = id.sign(signing_input);
    BlobData blob{};
    blob.signer_hint = signer_hint;
    blob.data.assign(data.begin(), data.end());
    blob.ttl = ttl;
    blob.timestamp = timestamp;
    blob.signature = std::move(signature);
    std::array<uint8_t, 32> ns_arr{};
    std::memcpy(ns_arr.data(), target_namespace.data(), 32);
    return {ns_arr, std::move(blob)};
}
```

For owner writes (`target_namespace == id.namespace_id()`), `signer_hint == target_namespace`. For delegate writes, they differ. This is the implicit D-02 distinction.

**Sanity checks the planner must build in:**
- Assert `id.signing_pubkey().size() == 2592` in the helper (debug-mode).
- Assert the returned signature is not empty.
- Assert `blob.ttl == ttl && blob.timestamp == timestamp` (mechanical but cheap).

## Pattern Inventory

**Reusable code for Phase 124:**

| Asset | Location | Use in Phase 124 |
|-------|----------|------------------|
| `find_pubkey_blob` | `commands.cpp:153-198` | Structural template for auto-PUBK probe; may also be simplifiable to call the new probe helper |
| `submit_name_blob` / `submit_bomb_blob` | `commands.cpp:502-568` | Already wrap the pattern D-03 generalizes; refactor to call `build_owned_blob` |
| `build_cdat_blob_flatbuf` / `build_tombstone_flatbuf` | `chunked.cpp:86-133` | File-local static helpers; refactor to call `build_owned_blob` |
| `enumerate_name_blobs` + pagination | `commands.cpp:1437-1489` | Pagination pattern; auto-PUBK probe is single-page so doesn't need this |
| `resolve_name_to_target_hash` | `commands.cpp:1540-1573` | Uses ListRequest+type_filter pattern; symmetric with PUBK probe |
| `Connection::kPipelineDepth` | `connection.h:23` | Constant = 8; D-06 cascade should use for manifest fetches |
| `pipeline::pump_recv_any` / `pump_recv_for` | `pipeline_pump.h` | Free-function pump; testable via ScriptedSource |
| `ScriptedSource` | `pipeline_test_support.h:38-57` | Mock transport for testing auto-PUBK probe |
| `Identity::namespace_id()`, `Identity::sign()` | `identity.h` | D-03 helper composes these unchanged |
| `make_pubkey_data(signing_pk, kem_pk)` | `wire.cpp:299-307` | Auto-PUBK emit reuses unchanged |
| `make_tombstone_data` / `make_delegation_data` / `make_name_data` / `make_bomb_data` | `wire.cpp:277-357` | All payload builders unchanged; ride the D-03 helper |
| `encode_blob_write_envelope` (node) | `db/wire/codec.h:42-48` | CLI's new `encode_blob_write_body` mirrors this byte-for-byte |
| `ConnectOpts`, `--node` resolution | `commands.h:10, main.cpp:184-230` | E2E orchestration unchanged |
| `drain_announce()` | `connection.cpp:603` | Handshake path unchanged |

**Project constraints (from MEMORY):**
- No backward compat (both binaries).
- No duplicate code (shared headers).
- No phase numbers in user-facing strings.
- Orchestrator-level build/test runs delegated to user.
- Claude self-verifies when infra reachable.
- cdb is the only client (no hypothetical SDKs).
- YAGNI, clean code; no quick fixes.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3 (header: `<catch2/catch_test_macros.hpp>`) |
| Config file | `cli/tests/CMakeLists.txt` (CTest target: `chromatindb_cli_tests`) |
| Quick run command | `cmake --build build -j$(nproc) --target chromatindb_cli_tests && ./build/cli/chromatindb_cli_tests "[wire]"` |
| Full suite command | `cmake --build build -j$(nproc) --target chromatindb_cli_tests && ./build/cli/chromatindb_cli_tests` |

### Phase Requirements → Test Map

| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| SC#1 | Auto-PUBK fires once per invocation per namespace on first owner write | unit | `./build/cli/chromatindb_cli_tests "[pubk]"` | ❌ Wave 0 (new `test_auto_pubk.cpp`) |
| SC#1 | Auto-PUBK probe sends correctly formatted ListRequest (flags=0x02, type=PUBK, limit=1) | unit | `./build/cli/chromatindb_cli_tests "[pubk]"` | ❌ Wave 0 |
| SC#1 | Auto-PUBK probe response parse: count=0 → emit; count>0 → skip | unit | `./build/cli/chromatindb_cli_tests "[pubk]"` | ❌ Wave 0 |
| SC#1 | Auto-PUBK second write to same ns skips probe (cache hit) | unit | `./build/cli/chromatindb_cli_tests "[pubk]"` | ❌ Wave 0 |
| SC#1 | Delegate write (target_ns != own_ns) never triggers auto-PUBK | unit | `./build/cli/chromatindb_cli_tests "[pubk]"` | ❌ Wave 0 |
| SC#1 | Live E2E: fresh namespace on home node, first `cdb put` succeeds without manual `cdb publish` | e2e | Manual run via `cdb --node home put /tmp/file.bin` after wipe-and-redeploy | manual |
| SC#2 | `encode_blob`/`decode_blob` roundtrip with new 5-field schema preserves signer_hint, data, ttl, timestamp, signature | unit | `./build/cli/chromatindb_cli_tests "wire: encode_blob*"` | ✅ (modify existing TEST_CASE at test_wire.cpp:145) |
| SC#2 | `encode_blob` rejects any field shaped like old pubkey (grep regression: no `pubkey` in encoded bytes) | unit | Visual assertion in test_wire.cpp | ❌ Wave 0 |
| SC#2 | Post-migration grep: `grep -rn "blob\.namespace_id\|blob\.pubkey" cli/src/` returns 0 hits | static | Shell command in phase verification | N/A |
| SC#3 | `build_signing_input` byte-output matches pre-rename value for same inputs (regression snapshot) | unit | `./build/cli/chromatindb_cli_tests "wire: build_signing_input*"` | ✅ (already exists at test_wire.cpp:189) |
| SC#3 | CLI-generated signing_input matches node-side `db/wire/codec.h::build_signing_input` for identical inputs | unit | Cross-verify with a golden vector (hardcoded 32-byte hex) | ❌ Wave 0 (add to test_wire.cpp) |
| SC#4 | `build_owned_blob(id, own_ns, ...)` → `signer_hint == id.namespace_id()` | unit | `./build/cli/chromatindb_cli_tests "[wire]"` | ❌ Wave 0 |
| SC#4 | `build_owned_blob(id, other_ns, ...)` → `signer_hint == SHA3(id.signing_pubkey())` != `other_ns` | unit | `./build/cli/chromatindb_cli_tests "[wire]"` | ❌ Wave 0 |
| SC#4 | `build_owned_blob(id, ns, data, ttl, ts).blob.signature` verifies with `id.signing_pubkey()` against `build_signing_input(ns, data, ttl, ts)` | unit | `./build/cli/chromatindb_cli_tests "[wire]"` | ❌ Wave 0 |
| SC#5 | `cdb put --name foo <file>` writes content + NAME blobs; `cdb get foo` resolves to content | integration | Live E2E (both nodes) | existing (Phase 123 coverage) |
| SC#5 | `cdb rm <h1> <h2> <h3>` writes a single BOMB covering all three | integration | Live E2E `ls --type BOMB` shows one entry with count=3 | existing |
| SC#5 | `cdb rm <chunked_manifest_hash>` cascades chunk tombstones (D-06) into the same BOMB | unit (manifest expansion) + integration (live) | Unit: new test for `classify_rm_target`. Live: put chunked, rm, verify chunks in `ls --raw --type TOMB` | ❌ Wave 0 (unit) |
| SC#5 | `make_bomb_data` roundtrip (count field, target concatenation) | unit | `./build/cli/chromatindb_cli_tests "[wire]"` | ❌ Wave 0 |
| SC#5 | `parse_name_payload` roundtrip (name length, target hash, magic mismatch rejection) | unit | `./build/cli/chromatindb_cli_tests "[wire]"` | ❌ Wave 0 |
| SC#6 | All pre-existing tests pass under new schema (identity, envelope, contacts, chunked, pipelining unchanged; wire modified) | unit | `./build/cli/chromatindb_cli_tests` (full suite) | ✅ |
| SC#7 | E2E matrix (D-08 items 1-7) all pass against local + home nodes post-redeploy | e2e | 7 items, documented in 124-E2E.md | manual (Claude runs) |
| SC#7 | ErrorResponse code 0x07 surfaces as D-05 string, not raw bytes | integration | Trigger via delegate write to uninitialized namespace on local node | manual |
| SC#7 | ErrorResponse code 0x08 surfaces as D-05 string | integration | Trigger via second identity writing PUBK to same namespace | manual |

### Sampling Rate
- **Per task commit:** `./build/cli/chromatindb_cli_tests "[wire]"` or `"[pubk]"` depending on module touched (~2s each)
- **Per wave merge:** `./build/cli/chromatindb_cli_tests` (full CLI suite; delegate node tests to user per `feedback_delegate_tests_to_user.md`)
- **Phase gate:** Full CLI + node suite green + live E2E matrix (D-08) green before `/gsd-verify-work`

### Wave 0 Gaps

- [ ] `cli/tests/test_auto_pubk.cpp` — covers SC#1 probe+cache logic (new file; depends on whether count crosses 5 TEST_CASEs per CONTEXT D-09's guidance)
- [ ] `cli/tests/test_wire.cpp` — extended: `BlobWriteBody` envelope roundtrip, `build_owned_blob` helper, `make_bomb_data` roundtrip, `parse_name_payload` roundtrip, golden-vector cross-verification against node's `build_signing_input`
- [ ] `cli/src/pubk_presence.{h,cpp}` — new module (or alternative: inline in `commands.cpp`) housing the in-process cache + probe helper extracted for testability
- [ ] `cli/src/error_map.{h,cpp}` (optional; may inline in commands.cpp) — centralizes D-05 error-code decoding
- [ ] Factored helper for `classify_rm_target` (D-06 cascade) — optional but clean
- [ ] Framework install: none needed (Catch2 v3 already in use)
- [ ] Live-node E2E harness: none beyond manual `cdb --node {local,home} ...` invocations; results documented in `124-E2E.md`

## Security Domain

Project uses `security_enforcement` implicitly (no override in `.planning/config.json`). ASVS mapping for this phase:

### Applicable ASVS Categories

| ASVS Category | Applies | Standard Control |
|---------------|---------|-----------------|
| V2 Authentication | yes | ML-DSA-87 (liboqs) — unchanged, already in place; D-03 helper uses `Identity::sign` directly |
| V3 Session Management | yes | PQ handshake (ML-KEM-1024 + AuthSignature with role byte) — unchanged; Phase 124 does not touch handshake |
| V4 Access Control | yes | Owner vs delegate enforced on node via `owner_pubkeys` DBI + `delegation_map` (Phase 122 work). CLI cooperatively passes `signer_hint = SHA3(signing_pk)`; node verifies. |
| V5 Input Validation | yes | FlatBuffer Verifier at `BlobWriteBody` boundary (node). CLI emits bytes; node verifies. CLI must validate `ErrorResponse` payload size before indexing bytes (D-05 defensive `payload.size() < 2` guard). |
| V6 Cryptography | yes | ML-DSA-87 signatures (non-deterministic), SHA3-256 for `signer_hint` and signing input, ChaCha20-Poly1305 AEAD for transport. All via liboqs / libsodium; NEVER hand-roll. |

### Known Threat Patterns for CLI + PQ-wire stack

| Pattern | STRIDE | Standard Mitigation |
|---------|--------|---------------------|
| Cross-namespace replay (delegate signs blob for wrong target) | Tampering | `build_signing_input` commits to `target_namespace` per Phase 122 D-01; D-03 helper uses correct target |
| Old-format blob accepted | Tampering | No backward compat (MEMORY); `Data=8` dispatcher branch deleted on node; CLI `MsgType::Data=8` removed (D-04a) |
| PUBK rebinding (attacker writes PUBK to victim's namespace) | Spoofing | Node-enforced first-PUBK-wins; node rejects `ERROR_PUBK_MISMATCH` on second-writer PUBK; CLI surfaces via D-05 |
| Information leak in error strings | Information Disclosure | D-05 user-facing strings deliberately avoid `PUBK_FIRST_VIOLATION` / `PUBK_MISMATCH` / phase numbers per `feedback_no_phase_leaks_in_user_strings.md` |
| Coroutine container invalidation (dangling span across co_await) | Tampering / crash | Auto-PUBK probe uses synchronous `conn.send()` + `conn.recv()`; no `send_async` in the probe path |
| BOMB DoS via large count | Denial of Service | CLI sorts + dedups targets at `commands.cpp:1326-1339`; node limits enforced per Phase 123 |
| Delegate attempts BOMB | Elevation of Privilege | Node rejects `ERROR_BOMB_DELEGATE_NOT_ALLOWED = 0x0B`; CLI surfaces via D-05 (new mapping) |
| Auto-PUBK race (probe→emit window; peer writes PUBK first) | Concurrency | Node rejects `ERROR_PUBK_MISMATCH` if different signing key beat us; D-05 surfaces. No retry loop; user runs `cdb publish` manually on conflict. |

## Risks / Unknowns / Claude's-Discretion Items

1. **Auto-PUBK cache scope — recommend invocation-scoped static module.** CONTEXT D-01 and Claude's-discretion allow either `Connection` member or static. Evidence from Q5: `cmd::reshare` and `cmd::publish` create multiple Connections per invocation. A Connection-scoped cache re-probes at each new Connection, wasting RTTs. Planner should codify this as `cli/src/pubk_presence.{h,cpp}` with file-scope static `std::unordered_set<std::array<uint8_t,32>>`, cleared at process start (implicit) and exposed via `ensure_pubk(id, conn, target_ns)`.

2. **`MsgType::Delete = 17` keep/remove — recommend KEEP.** See Q3. Node emits different ack type (`DeleteAck` vs `WriteAck`) based on `TransportMsgType`. Unifying on `BlobWrite` requires node changes (out of scope). CLI keeps `Delete = 17` enum value; sends of tombstones continue to use it; the payload format changes (now BlobWriteBody, not raw Blob). CONTEXT D-04a says only `Data = 8` is deleted, which matches this evidence.

3. **BlobWrite envelope function naming — pick `encode_blob_write_body` (matches table name) over `encode_blob_write_envelope` (node's name).** CONTEXT uses both names inconsistently; resolve in favor of `_body` for CLI self-consistency with the `BlobWriteBody` type.

4. **D-06 cascade partial-failure semantics — recommend warn-and-continue (option 3 in Q6).** CONTEXT lists all three options as Claude's discretion; warn-and-continue gives the user the most useful behavior without failing a large batch on one unrecoverable manifest.

5. **Error-mapping location — inline in `commands.cpp` for now.** CONTEXT defers this as Claude's discretion. New module is premature (only 5 codes to map); inline helper + grep-regression check for "Error: node rejected request" string suffices.

6. **ErrorResponse payload beyond 2 bytes — defensive parse.** Current node emits exactly 2 bytes. Future changes could add detail. CLI decoder should parse `payload[0]` (code) + `payload[1]` (original_type) and ignore trailing bytes gracefully.

7. **Tests for the D-06 cascade partial-failure path need a mocked Source.** `ScriptedSource` is reply-ordered; the cascade flow interleaves ReadRequest (for manifest) + ReadResponse (manifest bytes). Building the test requires careful mock queue construction. Planner should scope this as a separate task after the cascade logic lands.

8. **Golden-vector cross-verification with node side.** A hand-constructed test vector `(target_ns, data, ttl, timestamp) → expected 32-byte SHA3 digest` validates that CLI's `build_signing_input` byte output matches node's `db/wire/codec.cpp::build_signing_input` post-rename. Recommend adding a `[wire][golden]` TEST_CASE with hardcoded hex. If the node side exposes an equivalent test (cross-check `db/tests/`), mirror it.

9. **`MAX_LIST_LIMIT = 100` on node — irrelevant for auto-PUBK probe (limit=1), but relevant for the probe-based "enumerate all PUBKs" edge case.** No enumeration is needed; we only care if ≥1 exists. If a future optimization wants to pre-seed the cache with all known PUBKs, it would have to paginate — out of scope for 124.

10. **Live-node port confirmation.** `~/.cdb/config.json` has `"home": "192.168.1.73"` and `"local": "127.0.0.1"` but no explicit port. `parse_host_port` presumably applies a default. Before E2E run, Claude should verify `cdb --node home info` works — that's the simplest connectivity test and validates PUBK, ACL, and the post-redeploy data wipe all at once.

## Explicit Confirmation / Refutation of CONTEXT.md Claims

| CONTEXT claim | Status | Evidence |
|---------------|--------|----------|
| "~39 call sites in `cli/src/commands.cpp` + ~3 in `cli/src/chunked.cpp`" | ❌ **REFUTED** | Actual count: 9 in commands.cpp + 3 in chunked.cpp = **12 total**. See Q1 table. |
| Specific line refs `:514-515, :548-549, :716-717, :1233-1234, :1703-1704, :1749-1750, :2255-2256, :2387-2388, :2521-2522` | ✓ CONFIRMED | All 9 match `blob.namespace_id` + `blob.pubkey` assignment pairs (the line AFTER the `BlobData blob{};` declaration). |
| `cli/src/chunked.cpp:105-106, 126-127, 322-323` (3 sites) | ✓ CONFIRMED | Matches exactly. |
| `cli/src/commands.cpp:163` — existing `conn.send(MsgType::ListRequest, list_payload, 1)` | ✓ CONFIRMED | Line 163 in `find_pubkey_blob`. |
| `cli/src/commands.cpp:492-540` — `resolve_name_to_target_hash` | ✓ CONFIRMED (partial) | `resolve_name_to_target_hash` has TWO declarations — forward at `:492-494`, definition at `:1540-1573`. The pattern is in `enumerate_name_blobs` (`:1437-1489`) called by the definition. |
| `cli/src/commands.cpp:1446-1529` — second ListRequest site | ✓ CONFIRMED | That's the `enumerate_name_blobs` body. |
| `cli/src/wire.cpp:22` — Blob schema comment | ✓ CONFIRMED | Exact line: vtable comment for the 6-field post-phase-121 layout. |
| `cli/src/wire.cpp:147-212` — encode/decode regions | ✓ CONFIRMED | `encode_blob` is 170-193; `decode_blob` is 195-234. CONTEXT slightly off (:147 is not `encode_blob` — that's the test file). |
| `cli/src/wire.cpp:316-410` — NAME/BOMB helpers | ✓ CONFIRMED | `make_name_data` at 316, `make_bomb_data` at 337, `parse_name_payload` at 359, Sha3Hasher starts at 382, `encode_manifest_payload` at 408. |
| "`MsgType::Delete = 17` may become redundant after D-04b unification" | ⚠️ DISAGREE | Evidence says KEEP. Node's `TransportMsgType_Delete` drives `DeleteAck` reply type, distinct from `BlobWrite` → `WriteAck`. Removing Delete requires 6+ CLI sites' ack-type assertions to change, plus node changes. Out of 124 scope. |
| "Node sends `SyncNamespaceAnnounce(62)` after every `TrustedHello`" | ✓ CONFIRMED | `connection.cpp:603` has `drain_announce()`. |
| `ERROR_PUBK_FIRST_VIOLATION = 0x07`, `ERROR_PUBK_MISMATCH = 0x08` | ✓ CONFIRMED | `db/peer/error_codes.h:16-17`. |
| Phase 123 Phase 124 handoff at 123-03-SUMMARY.md line 213 | ✓ CONFIRMED | Matches the `cmd::rm_batch does not cascade chunked CPAR manifests` note. |
| `LIST_ENTRY_SIZE = 60` (hash + seq + type + size + ts) | ✓ CONFIRMED | `commands.cpp:109`. |
| ListRequest payload size 49 bytes with flags+type_filter | ✓ CONFIRMED | `commands.cpp:156`, node side `message_dispatcher.cpp:498-505`. |
| `~/.cdb/config.json` with `--node local` / `--node home` | ✓ CONFIRMED | File verified: `{"nodes": {"home": "192.168.1.73", "local": "127.0.0.1"}, "default_node": "home"}`. |
| `encode_blob_write_body` function name | ⚠️ INCONSISTENT | Node uses `encode_blob_write_envelope` (`db/wire/codec.h:46`). Recommend CLI uses `encode_blob_write_body` for symmetry with FlatBuffer `BlobWriteBody` type name. Planner: explicitly document the name choice. |
| D-01a: Delegate-first-write to un-PUBK'd namespace fails cleanly | ✓ CONFIRMED | Node `engine.cpp:192` rejects `pubk_first_violation` for any non-PUBK first write, including delegate. CLI surfaces via D-05. |
| `NamespacedBlob` struct vocabulary mirrors `db/sync/sync_protocol.h` | ⚠️ UNVERIFIED | Did not read `db/sync/sync_protocol.h`; CONTEXT asserts this is the name. Planner should verify if the exact struct name is important; otherwise `NamespacedBlob` is fine as a CLI-local struct. |

## Sources

### Primary (HIGH confidence)
- `cli/src/wire.h` (391 lines), `cli/src/wire.cpp` (483 lines) — complete schema/encoder source
- `cli/src/commands.cpp` (2794 lines) — all blob construction + send sites
- `cli/src/chunked.cpp` (745 lines) — 3 blob construction sites + rm_chunked reference
- `cli/src/identity.{h,cpp}` — Identity invariants
- `cli/src/connection.{h,cpp}` — lifecycle, pipelining, drain_announce
- `cli/src/pipeline_pump.h` — testable pump templates
- `cli/tests/test_wire.cpp`, `cli/tests/test_connection_pipelining.cpp`, `cli/tests/pipeline_test_support.h` — test patterns
- `cli/src/main.cpp:180-230` — `--node` resolution
- `db/schemas/blob.fbs`, `db/schemas/transport.fbs` — wire contracts
- `db/wire/codec.h` — node-side encoder signatures to mirror
- `db/peer/error_codes.h` — wire error codes
- `db/peer/message_dispatcher.cpp:61-78,320-417,1385-1507` — send_error_response + BlobWrite + Delete dispatchers
- `db/engine/engine.cpp:192,423,431` — IngestError emission points
- `~/.cdb/config.json` — live operator config

### Secondary (MEDIUM confidence)
- `.planning/ROADMAP.md § Phase 124` — SC#1-7 (no explicit REQ-IDs)
- `.planning/phases/122-.../122-*-SUMMARY.md` — Phase 122 landing points (read via CONTEXT summaries)
- `.planning/phases/123-.../123-*-SUMMARY.md`, `123-CONTEXT.md` — Phase 123 handoff
- MEMORY `reference_live_node.md`, `project_phase122_pubk_first_invariant.md`, `project_phase123_bomb_ttl_zero.md`, `feedback_no_*.md` series

### Tertiary (LOW confidence / unverified)
- Specific node-internal paths not directly read (e.g., `db/sync/sync_protocol.h` for `NamespacedBlob` vocabulary) — tagged as UNVERIFIED in the CONTEXT-confirmation table
- CLI default port value — not explicitly verified; `main.cpp:parse_host_port` default inferred but not read line-by-line

## Assumptions Log

| # | Claim | Section | Risk if Wrong |
|---|-------|---------|---------------|
| A1 | `NamespacedBlob` struct name in `db/sync/sync_protocol.h` matches CONTEXT's claim | Risks/Unknowns #10 | Minor — CLI can pick its own struct name without needing to mirror exactly |
| A2 | Node's `MAX_LIST_LIMIT = 100` applies to ALL ListRequest, including when flags bit 1 (type_filter) is set | Q4 | Low — auto-PUBK uses limit=1 which is well under the cap |
| A3 | `cdb` default TCP port is `6742` | Q9 | Low — live E2E will reveal it immediately |
| A4 | No invocation uses `cmd::*` functions in a multi-threaded context (thread-local = process-local) | Q5 / D-01 scope | Medium — if a threaded test harness exists or is added, file-scope static becomes thread-unsafe; `thread_local` keyword makes it safe either way |
| A5 | Phase 122's schema landing on the node side is complete and deployed to the dev machines | SC#7 E2E | High (but handled by D-07 — user deploys before Claude runs E2E) |
| A6 | `~/.cdb/config.json` port-less node entries ("192.168.1.73" without `:port`) resolve to the default port via `parse_host_port` | Q9 | Low — would need to `grep` main.cpp to confirm |
| A7 | `MsgType::Delete = 17` keep recommendation is correct — no node change in 124 | Q3, Risks/Unknowns #2 | High if wrong — would cascade into 6+ CLI sites. But evidence (node still emits DeleteAck for Delete) strongly supports keep. |

## Open Questions

1. **Auto-PUBK on `cmd::publish` itself?** `cmd::publish` (`:2490-2557`) IS the canonical PUBK writer. If we route it through `build_owned_blob` + auto-PUBK, the helper would try to auto-PUBK before the user's PUBK. Classic chicken-and-egg. **Resolution:** `cmd::publish` must bypass the auto-PUBK helper — it writes its PUBK directly. The in-process cache should be populated by `cmd::publish`'s successful WriteAck so subsequent writes in the same invocation (unlikely but possible) skip the probe. Planner task.

2. **What about `cmd::reshare`'s three-connection flow?** Auto-PUBK cache scoped to invocation means the first Connection triggers probe+emit if needed; Connections 2 and 3 see cache hit and skip. Confirms file-scope static is the right scope.

3. **Should delegate error strings include the operator-visible namespace hex?** D-05's "`Error: namespace <ns_short> is owned by a different key on node <host>`" is one style; alternative is `"Error: write to namespace <hex> rejected: this namespace belongs to a different key"`. Minor UX polish — planner picks.

4. **Does `cdb rm_batch` need a `--cascade` opt-in, or should it always cascade CPAR?** CONTEXT D-06 implies always-on. Risk: user running `cdb rm <bunch of hashes, some happen to be CPAR>` doesn't expect a multi-minute cascade of chunk fetches. **Recommend always-on** for correctness (otherwise the D-06 fix leaks orphaned chunks), but document the new behavior in Phase 125's cli/README.md update.

## Metadata

**Confidence breakdown:**
- Standard stack / schema migration: HIGH — every relevant file read; node-side contracts cross-referenced via source.
- Auto-PUBK architecture: HIGH — template (`find_pubkey_blob`) already exists; cache scope resolved by evidence (multi-Connection invocations).
- Error-code mapping: HIGH — exhaustive grep of existing `ErrorResponse` sites + node error_codes.h.
- D-06 cascade: MEDIUM-HIGH — machinery exists in `cmd::rm` single-target; lifting to batch has well-defined integration points but partial-failure UX is Claude's discretion.
- `MsgType::Delete = 17` decision: HIGH — DeleteAck vs WriteAck evidence is unambiguous.
- E2E tooling: HIGH — live config file inspected.
- 12-vs-39 call-site discrepancy: HIGH (VERIFIED via grep).

**Research date:** 2026-04-21
**Valid until:** 2026-05-21 (stable — no fast-moving dependencies). If node-side Phase 122+123 changes land after this date, re-verify `db/wire/codec.h` and `db/peer/error_codes.h` before executing.
