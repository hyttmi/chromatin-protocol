# Phase 123: Tombstone Batching + Name-Tagged Overwrite — Research

**Researched:** 2026-04-20
**Domain:** wire format extension, ingest validation, CLI command surface, test-infrastructure extension
**Confidence:** HIGH (every claim below is verified against current code — file:line citations are live as of the Phase 122-05 tip on master)

## Summary

Phase 123 introduces two new signed-blob magics (NAME and BOMB) plus minimal node-side surface for batched deletion and a prefix-based enumeration endpoint. The node work is 4 small edits (two `is_*` helpers in `db/wire/codec.h`, one structural-validation block inserted into `engine::BlobEngine::ingest` between the existing Step 1.5 (PUBK-first) and Step 2 (signer resolution), one delegate-rejection block mirrored onto `is_bomb`, and one `ListRequest`-based enumeration path — which already exists at the server from Phase 117 TYPE-03). The bulk of the work is CLI: NAME/BOMB emission on `cdb put --name`, deterministic resolution in a new `cdb get <name>`, multi-target argv parsing + single-BOMB emission in `cdb rm`, and a new `cdb put --replace` flow that atomically emits content + NAME + BOMB-of-1 (or plain tombstone — planner's call).

**Critical constraints that shape the plan:**
1. The CLI wire module (`cli/src/wire.h`, line 132-139) still carries the **pre-122** `BlobData` shape (`namespace_id`, `pubkey`). `cli/src/commands.cpp` still emits pre-122 blobs on every write path (see `rm` at `commands.cpp:1038-1046`). Phase 124 is scheduled to adapt the CLI to the post-122 protocol. This means Phase 123's CLI work must **either** (a) ride in on the pre-122 CLI shape and be adapted in Phase 124, or (b) co-land the post-122 CLI adaptation as part of Phase 123. The CONTEXT.md is unambiguous on this: "Test against the live 192.168.1.73 node until post-124 (CLI + node are redeployed together after 124 lands)." — so phase 123 CLI is permitted to remain on the pre-122 wire, and Phase 124 cleans both 122 + 123 CLI deltas at once. **The planner should confirm this tradeoff explicitly.** [VERIFIED: cli/src/wire.h:132-139; cli/src/commands.cpp:1038-1046; ROADMAP.md § Phase 124 line 156-161]
2. A `ListByMagic` transport endpoint is **already partially built** — Phase 117 TYPE-01..04 added 4-byte type indexing to `seq_map` and a `type_filter` flag to `ListRequest`. The dispatcher filters by `blob_type` prefix directly from the seq index (no blob decryption, no full-blob read). See `message_dispatcher.cpp:486-499, 520-549`. D-10's "new `ListByMagic` TransportMsgType" is therefore a **design choice** in Claude's discretion: reuse `ListRequest + type_filter` (zero new transport types; paginated hashes returned; client fetches individual NAME blobs with `ReadRequest` to get the internal target_hash + timestamp for sort) vs add a dedicated `ListByMagic=65` type that returns decoded blob bodies in one round-trip. Recommendation below. [VERIFIED: db/peer/message_dispatcher.cpp:473-585; db/storage/storage.cpp:463-471 and 875-933]

**Primary recommendation:** REUSE the existing `ListRequest + type_filter` mechanism instead of adding a new `ListByMagic=65` TransportMsgType. Rationale: zero new transport surface, zero new dispatcher code, zero new storage methods, and the client-side cost (one `ReadRequest` per NAME candidate to fetch the payload for sort) is trivial since the number of NAME blobs in a namespace is bounded by the number of distinct user-visible names — typically <100, not the full blob count. The "new TransportMsgType" framing in CONTEXT.md D-10 is discretionary ("Planner decides based on dispatcher symmetry" — D-10 in Claude's Discretion section).

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions

- **D-01:** Seq source = `blob.timestamp` (uint64 seconds). No explicit seq field in the NAME payload. Resolution rule: among all NAME blobs whose payload names `foo`, the winner has the highest `blob.timestamp`.
- **D-02:** Tiebreak when two NAME blobs share the same `blob.timestamp` = `content_hash` DESC (lexicographic descending).
- **D-03:** NAME payload layout = `[magic:4][name_len:2 BE][name:N bytes][target_content_hash:32]`.
- **D-04:** Name charset + length = UTF-8, `name_len ∈ [1, 65535]` bytes, no further restrictions. Names may contain any bytes; codec treats `name` as opaque bytes (memcmp).
- **D-05:** BOMB target encoding = `target_content_hash:32` only. Payload layout: `[magic:4][count:4 BE][target_hash:32 × count]`.
- **D-06:** Batch store = per-command only. `cdb rm A B C` produces ONE BOMB. Separate invocations produce separate BOMBs — NO daemon, NO state file.
- **D-07:** Within one invocation = one BOMB per command regardless of count.
- **D-08:** No time-based flush trigger (D-06 makes it vacuous). `cdb rm` is synchronous.
- **D-09:** NO name cache. CLI is stateless — every `cdb get foo` enumerates NAME blobs on every read.
- **D-10:** Enumeration API = `ListByMagic(namespace:32, magic_prefix:4)` TransportMsgType. Semantically opaque — node filters by first 4 bytes of blob.data.
- **D-11:** Delegates CAN write NAME blobs.
- **D-12:** Delegates CANNOT emit BOMB blobs (extends the existing tombstone-owner-only rule).
- **D-13:** On every BOMB ingest the node validates: (1) `ttl == 0` mandatory; (2) structural sanity — `data.size() >= 8` and `data.size() == 8 + count*32`; (3) delegate rejection.
- **D-14:** Node does NOT verify each BOMB target_hash is a known blob.
- **D-15:** Overwrite policy = opt-in `--replace` flag. Without `--replace`, prior content remains reachable by hash.

### Claude's Discretion

- Exact BOMB magic bytes (proposal: `0x424F4D42` "BOMB"). Any 4-byte constant that doesn't collide.
- Exact NAME magic bytes — ROADMAP already specifies `0x4E414D45` "NAME".
- Exact name of the new TransportMsgType for D-10 (`ListByMagic = 65` vs extending ListRequest with a `magic_prefix` field). **Planner decides based on dispatcher symmetry.**
- Whether D-15's overwrite path emits a BOMB (count=1) or a single-target tombstone.
- Pagination semantics of ListByMagic. A simple count-capped single-response works for MVP.
- Test file naming — follow the phase 122 convention (`db/tests/<area>/test_<feature>.cpp` with `[phase123]` tags).
- Whether `is_name()` / `is_bomb()` go into `db/util/magic_check.h` (shared header) or stay inline in `codec.h`. Existing magics are inline in codec.h — stay consistent unless threshold crossed.
- Error-code values for new BOMB rejection reasons in `db/peer/error_codes.h` (`ERROR_BOMB_TTL_NONZERO`, `ERROR_BOMB_MALFORMED`, `ERROR_BOMB_DELEGATE_NOT_ALLOWED`).

### Deferred Ideas (OUT OF SCOPE)

- Local name cache (`~/.chromatindb/name_cache.json`) — deferred.
- Server-side NAME index DBI for O(1) resolution — future 999.x.
- Long-lived batching daemon for `cdb rm` — rejected YAGNI.
- Delegation-gated BOMB — rejected.
- Explicit `seq:8` field in NAME payload — unnecessary given D-01.
- Node verification that BOMB targets exist — rejected (D-14).
- Multi-BOMB splitting within one `cdb rm` invocation — rejected (D-07).

</user_constraints>

<phase_requirements>
## Phase Requirements

No REQUIREMENTS.md IDs map to Phase 123 (Success Criteria #1–7 from ROADMAP are the contract). Each SC is restated below with the research finding that enables implementation:

| SC | Description | Research Support |
|----|-------------|------------------|
| 1 | NAME magic defined with D-03 payload in both codec.h and wire.h | §Standard Stack (magic constants); §Integration Points (codec.h:92-152 is the canonical site). |
| 2 | BOMB magic defined with D-05 payload; BOMB MUST have ttl=0 enforced at ingest | §Architecture Patterns (ingest pipeline); §Don't Hand-Roll (reuse engine.cpp:149-158 / :161-172 pattern); §Common Pitfalls (Pitfall 1). |
| 3 | `cdb put --name foo file` writes content blob + NAME blob | §CLI Surface Extension; §Integration Points (`cli/src/main.cpp:358-410`, `cli/src/commands.cpp:483+`). |
| 4 | `cdb get foo` resolves name via enumeration (NO cache per D-09) | §Architectural Responsibility Map; §Don't Hand-Roll (reuse ListRequest+type_filter — Phase 117 TYPE-03); §Common Pitfalls (Pitfall 3). |
| 5 | Overwrite = new NAME blob with higher timestamp; optional BOMB per D-15 | §Design Patterns (Pattern 1 resolution algorithm). |
| 6 | `cdb rm` multi-target produces ONE BOMB per invocation | §CLI Surface Extension (`cdb rm` argv parse); §Integration Points (`commands.cpp:907-1092`). |
| 7 | Node treats NAME/BOMB opaquely; only BOMB gets validation (D-13) | §Architecture Patterns (validation block site); §Code Examples. |

</phase_requirements>

## Architectural Responsibility Map

Phase 123 spans four tiers. The discipline of this phase is keeping as much as possible OUT of the node.

| Capability | Primary Tier | Secondary Tier | Rationale |
|------------|-------------|----------------|-----------|
| Magic constants + `is_name` / `is_bomb` helpers | wire layer (`db/wire/codec.h` + `cli/src/wire.h`) | — | Both binaries must agree on byte-level identification. Follows existing `is_tombstone` / `is_delegation` / `is_pubkey_blob` pattern at codec.h:92-152. |
| NAME blob construction (signing, envelope, submission) | CLI (`cli/src/commands.cpp` `cmd::put`) | — | NAME is a signed user-data blob; signing happens where the ML-DSA signing key lives (the CLI has the identity). |
| BOMB blob construction (signing, envelope, submission) | CLI (`cli/src/commands.cpp` `cmd::rm`) | — | Same as NAME — signing is a client operation. |
| NAME resolution (enumeration + sort + fetch) | CLI (new `cdb get <name>`) | — | D-09 stateless: every read re-enumerates. D-01/D-02 tiebreak is deterministic from data the client already fetches. |
| PUBK-first invariant for NAME / BOMB first-writes | Node ingest (`db/engine/engine.cpp` Step 1.5) | — | Already in place — NAME and BOMB ride the existing gate at engine.cpp:187-195. No change required; fresh namespace still requires PUBK first regardless of magic. |
| BOMB structural validation (ttl=0, data.size==8+count*32) | Node ingest (`db/engine/engine.cpp` NEW Step 1.7 or fold into Step 1) | — | Cheap integer checks; MUST run before crypto::offload (Pitfall-#6 adversarial-flood defense, same as the PUBK-first gate). Mirror of the tombstone ttl=0 check at engine.cpp:149-158. |
| BOMB delegate-rejection | Node ingest (`db/engine/engine.cpp` Step 2 — delegate branch) | — | Mirrors existing `is_delegation` / `is_tombstone` rejection at engine.cpp:268-279. |
| NAME enumeration (server-side prefix filter) | Node dispatcher (`ListRequest` branch) | Storage (`seq_map` read) | ALREADY EXISTS as of Phase 117: `ListRequest` accepts a 4-byte `type_filter` and the server filters `seq_map` entries by the pre-indexed 4-byte type prefix. D-10 can be satisfied without new code. |
| Name-cache persistence | (none — deferred per D-09) | — | Explicit non-goal. |

**Boundary insight:** Every new signed-blob invariant (ttl=0, delegate-restriction) extends an existing pattern already established in engine.cpp for tombstones. The research question is where each new check drops in among the existing numbered steps (Step 0..Step 4).

## Standard Stack

### Core — all already present, no new dependencies

| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| liboqs ML-DSA-87 | (pinned in CMake) | Signature over NAME/BOMB payload | Every signed blob already uses this; hash-then-sign over `build_signing_input`. [VERIFIED: db/crypto/signing.h] |
| libmdbx | (pinned in CMake) | blobs_map / seq_map reads for enumeration | Unchanged — enumeration uses existing `get_blob_refs_since` path. [VERIFIED: db/storage/storage.cpp:875-933] |
| FlatBuffers | (pinned) | `BlobWriteBody` envelope carries NAME/BOMB through existing BlobWrite=64 dispatch | NAME/BOMB are plain BlobData; signing sponge is unchanged (Phase 122 `SHA3(target_namespace ‖ data ‖ ttl ‖ timestamp)`). [VERIFIED: db/schemas/transport.fbs:82-86] |
| Catch2 | (pinned) | New test files under `db/tests/engine/` and `db/tests/wire/` | Matches phase 122 convention. [VERIFIED: db/tests/engine/test_pubk_first.cpp] |
| nlohmann/json | (pinned) | N/A for Phase 123 (cache deferred by D-09) | Not used. |

### Supporting

None. No new library introductions are anticipated; everything slots into existing infrastructure.

### Alternatives Considered

| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| Reusing `ListRequest + type_filter` for D-10 | New `ListByMagic=65` TransportMsgType with body returning full blob bodies | New type avoids one round-trip per NAME candidate (which is fine because NAME count per namespace is small); more transport surface; requires new FlatBuffers table OR hand-rolled wire (per phase 122's `encode_blob_transfer` idiom). Recommendation: reuse existing. See §Architecture Patterns Pattern 2. |
| Single tombstone for `--replace` | BOMB-of-1 | BOMB-of-1 is 4 bytes larger than a single-target tombstone (count field), but keeps one deletion code path. Tombstone is 36 bytes data; BOMB-of-1 is 8+32=40 bytes. Recommendation: BOMB-of-1 for code unity (`feedback_no_duplicate_code.md`). |
| Ship without name cache (D-09) | Local `~/.cdb/name_cache.json` | User explicitly chose stateless; ROADMAP's cache text is a ROADMAP deviation (D-09). |

## Architecture Patterns

### System Architecture Diagram

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                                   CLI (cdb)                                  │
│                                                                              │
│  cdb put --name foo file            cdb get foo                cdb rm A B C  │
│         │                                 │                          │        │
│         ▼                                 ▼                          ▼        │
│  [build content blob]              [ListRequest +           [build BOMB data  │
│  [build NAME data:                  type_filter=NAME]        [magic:4]       │
│   [NAME:4][len:2][name][hash:32]]          │                 [count:4BE]     │
│         │                                   ▼                 [hash:32]×N]   │
│   [sign both blobs]              [read-back NAME blobs        │              │
│         │                         one-by-one via              [sign, ttl=0]  │
│         │                         ReadRequest]                 │              │
│         │                         [parse [name][target_hash]]  │              │
│         │                         [sort: ts DESC,              │              │
│         │                               content_hash DESC]    │              │
│         │                         [winner.target_hash ->       │              │
│         │                          ReadRequest] ──────────────→│ (cmd::get)  │
│         │                                 │                    │              │
│         ▼                                 ▼                    ▼              │
│   send via BlobWrite=64           (local decode only)    send via Delete=17  │
│   envelope                                                 envelope          │
│   (target_namespace + blob)                                (target_namespace │
│                                                            + BOMB blob)      │
└─────────┬────────────────────────────────────────────────────────┬───────────┘
          │                                                         │
          ▼ PQ-encrypted transport                                  ▼
┌──────────────────────────────────────────────────────────────────────────────┐
│                                     NODE                                     │
│                                                                              │
│  message_dispatcher.cpp                                                      │
│  ├─ TransportMsgType_BlobWrite=64 ────▶ engine_.ingest(target_ns, blob, conn)│
│  ├─ TransportMsgType_Delete=17    ────▶ engine_.delete_blob(target_ns, blob) │
│  ├─ TransportMsgType_ListRequest=33 ──▶ storage_.get_blob_refs_since         │
│  │                                      (+ type_filter via seq_map.value)    │
│  │                                      ─ Phase 117 already filters by 4-byte│
│  │                                        blob_type stored in seq_map ─      │
│  └─ TransportMsgType_ReadRequest=31 ──▶ storage_.get_blob                    │
│                                                                              │
│  engine::BlobEngine::ingest (engine.cpp:105)                                 │
│  ├─ Step 0   size / capacity / timestamp / ttl bounds                        │
│  ├─ Step 0e  max_ttl with tombstone exemption :161                           │
│  │           └─ NEW: add is_bomb exemption (BOMB is permanent like tombstone)│
│  ├─ Step 1   empty signature check :175                                      │
│  ├─ Step 1.5 PUBK-first gate (Phase 122 D-03) :187-195                       │
│  ├─ [NEW Step 1.7] BOMB structural validation (D-13)                         │
│  │  ├─ if is_bomb(blob.data):                                                │
│  │  │   if blob.ttl != 0  → IngestError::invalid_ttl / ERROR_BOMB_TTL_NONZERO│
│  │  │   if !validate_bomb_structure(blob.data) → malformed_blob              │
│  │  │   (runs BEFORE crypto::offload — adversarial-flood defense)            │
│  ├─ Step 2   resolve signer_hint (owner_pubkeys or delegation_map) :207-264  │
│  │           └─ EXISTING delegate branch :266-280 already rejects tombstone  │
│  │               and delegation from delegates — ADD is_bomb to this list    │
│  ├─ Step 2.5 dedup check via blob_hash :315                                  │
│  ├─ Step 3   ML-DSA-87 signature verify :331                                 │
│  ├─ Step 3.5 tombstone side-effect (delete target) :351                      │
│  │           └─ NEW: BOMB side-effect (iterate count*32 target hashes,       │
│  │               call storage_.delete_blob_data for each)                    │
│  ├─ Step 4.5 PUBK registration :373 (unchanged)                              │
│  └─ Step 4   store_blob :388                                                 │
└──────────────────────────────────────────────────────────────────────────────┘
```

Every arrow above follows the existing phase-122 wire / dispatcher / engine architecture. The additions are (1) two magic constants, (2) two `is_*` helpers, (3) one structural-validation block at Step 1.7, (4) one BOMB exemption in Step 0e's max_ttl check, (5) one BOMB tombstone side-effect in Step 3.5, (6) one `is_bomb` addition to the delegate-rejection list at Step 2. Node-side CODE delta is under ~80 lines.

### Recommended Project Structure

Zero new files required on the node side. New files on the CLI side are `cdb get <name>` (new subcommand), new test files under `db/tests/engine/` and `db/tests/wire/`.

```
db/
├── wire/codec.h                   # ADD NAME_MAGIC, BOMB_MAGIC, is_name, is_bomb,
│                                  #   NAME_MIN_DATA_SIZE, BOMB_MIN_DATA_SIZE,
│                                  #   validate_bomb_structure, extract_bomb_targets
├── wire/codec.cpp                 # Implement validate_bomb_structure + extract_bomb_targets
├── engine/engine.cpp              # Insert Step 1.7 BOMB structural validation
│                                  #   Add is_bomb to delegate-rejection list (Step 2 trailer)
│                                  #   Add BOMB exemption to Step 0e max_ttl (tombstones-like)
│                                  #   Add BOMB side-effect in Step 3.5 (delete N targets)
├── peer/error_codes.h             # ADD ERROR_BOMB_TTL_NONZERO, ERROR_BOMB_MALFORMED,
│                                  #   ERROR_BOMB_DELEGATE_NOT_ALLOWED (0x09..0x0B)
├── tests/engine/
│   ├── test_bomb_validation.cpp       # NEW - Catch2 tests for D-13 (ttl=0, structure,
│   │                                  #   delegate-reject)
│   └── test_bomb_side_effects.cpp     # NEW - BOMB deletes all targets; dedup works;
│                                      #   idempotent re-ingest
├── tests/wire/
│   └── test_name_bomb_codec.cpp       # NEW - magic-constant tests, is_name / is_bomb,
│                                      #   structural validation, extract helpers
└── tests/test_helpers.h               # ADD make_name_blob, make_bomb_blob

cli/src/
├── wire.h                         # MIRROR NAME_MAGIC, BOMB_MAGIC, is_name, is_bomb
├── wire.cpp                       # (mirror implementations if needed)
├── commands.h                     # ADD put(..., --name, --replace),
│                                  #   get_by_name(...), rm multi-target signature
├── commands.cpp                   # EXTEND put: handle --name / --replace (build NAME blob,
│                                  #   optionally BOMB-of-1)
│                                  # NEW: get_by_name (enumerate via ListRequest+type_filter,
│                                  #   fetch candidates via ReadRequest, sort, fetch winner)
│                                  # EXTEND rm: accept multi-target, build one BOMB
└── main.cpp                       # EXTEND put argv parse: --name, --replace (line 358-410)
                                   # EXTEND rm argv parse: multi-target (line 503-552)
                                   # NEW get-by-name branch (when argv[0] is not 64-hex,
                                   #   treat as name) OR new `cdb getbyname <name>` subcommand
```

### Pattern 1: Deterministic Name Resolution (D-01 + D-02)

**What:** Enumerate all NAME blobs in target_namespace, sort by (timestamp DESC, content_hash DESC), fetch the winner's target_content_hash.

**When to use:** Every `cdb get <name>` invocation (D-09 stateless).

**Algorithm:**

```cpp
// Pseudocode for cli/src/commands.cpp:get_by_name
int get_by_name(Identity& id, std::span<const uint8_t, 32> ns,
                const std::string& name,
                const ConnectOpts& opts)
{
    // Step 1: Enumerate NAME blobs in ns.
    //   ListRequest with type_filter = NAME_MAGIC returns
    //   [hash:32 | seq:8 | type:4 | size:8 | ts:8] per match.
    auto name_refs = list_with_type_filter(id, ns, NAME_MAGIC, opts);

    // Step 2: Fetch each NAME blob body via ReadRequest; parse name+target_hash.
    struct NameEntry {
        std::array<uint8_t, 32> content_hash;  // hash of the NAME blob itself
        uint64_t timestamp;                     // outer blob.timestamp (the seq per D-01)
        std::array<uint8_t, 32> target_hash;    // from NAME payload
    };
    std::vector<NameEntry> matches;
    for (auto& ref : name_refs) {
        auto blob = read_blob(id, ns, ref.blob_hash, opts);
        if (!blob) continue;
        auto parsed = parse_name_payload(blob->data);
        if (!parsed) continue;
        if (parsed->name != name) continue;  // memcmp — D-04 opaque bytes
        matches.push_back({
            ref.blob_hash, blob->timestamp, parsed->target_hash
        });
    }

    if (matches.empty()) { /* not found */ return 1; }

    // Step 3: D-01 timestamp DESC, D-02 content_hash DESC tiebreak.
    std::sort(matches.begin(), matches.end(),
        [](const NameEntry& a, const NameEntry& b){
            if (a.timestamp != b.timestamp) return a.timestamp > b.timestamp;
            return std::memcmp(a.content_hash.data(), b.content_hash.data(), 32) > 0;
        });

    // Step 4: Winner's target_hash is the content blob; fetch via ReadRequest + decode CENV.
    return cmd::get(id, {to_hex(matches[0].target_hash)}, /* ns_hex */ ..., opts);
}
```

### Pattern 2: BOMB Structural Validation (D-13) — fits between Step 1.5 and Step 2

**What:** Cheap integer check before any crypto::offload, mirroring the PUBK-first gate's discipline.

**Source:** [VERIFIED: `db/engine/engine.cpp:181-195` shows the Phase 122 D-03 PUBK-first pattern that serves as the structural template.]

```cpp
// NEW Step 1.7 (Phase 123 D-13): BOMB structural validation.
// Runs BEFORE Step 2 signer resolution (→ BEFORE any crypto::offload).
// Non-BOMB blobs fall through unchanged.
if (wire::is_bomb(blob.data)) {
    // D-13(1): ttl must be 0 (same invariant as single-tombstones; see
    //          engine.cpp:149 for the single-tombstone analog and
    //          project_phase123_bomb_ttl_zero.md for the reasoning).
    if (blob.ttl != 0) {
        co_return IngestResult::rejection(IngestError::invalid_ttl,
            "BOMB must have ttl=0 (permanent)");
    }
    // D-13(2): header sanity — declared count matches payload length.
    //   Layout: [magic:4][count:4 BE][target_hash:32]*count
    if (blob.data.size() < 8) {
        co_return IngestResult::rejection(IngestError::malformed_blob,
            "BOMB data too short for header");
    }
    uint32_t count = chromatindb::util::read_u32_be(blob.data.data() + 4);
    size_t expected = size_t{8} + size_t{count} * 32u;
    if (blob.data.size() != expected) {
        co_return IngestResult::rejection(IngestError::malformed_blob,
            "BOMB data size mismatch");
    }
    // count == 0 is allowed structurally — a BOMB-of-0 is a no-op the signer
    // is free to produce; we accept it and let the "no targets to delete"
    // side-effect naturally produce a stored BOMB with no deletion.
    // (Planner may choose to reject count==0 as malformed. No strong reason
    // either way — doesn't affect correctness. Recommendation: accept.)
}
```

### Pattern 3: BOMB Delegate-Rejection — extends the existing list at Step 2 trailer

**Source:** [VERIFIED: `db/engine/engine.cpp:266-280`]

```cpp
// EXISTING Step 2 trailer (engine.cpp:266-280):
if (is_delegate) {
    if (wire::is_delegation(blob.data)) { /* reject */ }
    if (wire::is_tombstone(blob.data))  { /* reject */ }
    // ADD for Phase 123 D-12:
    if (wire::is_bomb(blob.data)) {
        co_return IngestResult::rejection(IngestError::no_delegation,
            "delegates cannot create BOMB blobs");
    }
}
```

### Pattern 4: BOMB Tombstone Side-Effect — fits into Step 3.5

**Source:** [VERIFIED: `db/engine/engine.cpp:349-366`]

The existing single-tombstone side-effect calls `storage_.delete_blob_data(target_namespace, target_hash)` for the one target. BOMB iterates:

```cpp
// EXISTING Step 3.5 (engine.cpp:349-366) — tombstone handling.
// Extend with BOMB branch:
if (wire::is_tombstone(blob.data)) {
    auto target_hash = wire::extract_tombstone_target(blob.data);
    storage_.delete_blob_data(target_namespace, target_hash);
}
else if (wire::is_bomb(blob.data)) {
    auto targets = wire::extract_bomb_targets(blob.data);  // vector<array<u8,32>>
    for (const auto& th : targets) {
        storage_.delete_blob_data(target_namespace, th);
    }
    // Regular-blob-block-by-tombstone is skipped (BOMB itself is a batched
    // tombstone). No need to check has_tombstone_for.
}
else {
    // existing regular-blob block-by-tombstone check (unchanged)
    if (storage_.has_tombstone_for(target_namespace, content_hash)) {
        co_return IngestResult::rejection(IngestError::tombstoned,
            "blocked by tombstone");
    }
}
```

**Caveat:** `delete_blob_data` is iterated on the io_context thread (Phase 121 STORAGE_THREAD_CHECK discipline — engine.cpp is already on the executor at Step 3.5 after the post-back-to-ioc at :340). No offload needed; each delete is a single MDBX txn. For large BOMBs (hundreds or thousands of targets) this runs synchronously in one pass. MDBX can batch — planner may choose to fold all deletes into one write txn via a new `Storage::delete_blobs_data_batch(ns, hashes[])` helper for efficiency. **Not required for correctness; YAGNI until measured.**

### Anti-Patterns to Avoid

- **Do NOT duplicate the PUBK-first gate for BOMB.** BOMB does NOT bypass PUBK-first — a BOMB to a fresh namespace should fall through to the existing gate at engine.cpp:187-195 and be rejected with `pubk_first_violation`. No new gate needed; BOMB on a fresh namespace is a nonsensical attack and the existing gate already handles it.
- **Do NOT add a new TransportMsgType if ListRequest+type_filter suffices.** See §Primary Recommendation.
- **Do NOT parse NAME / BOMB payload on the node.** Node stays semantically opaque (per the ROADMAP "Node treats NAME and BOMB as opaque" clause). The single exception is BOMB structural validation (D-13) — but that's byte-length math on the header, NOT payload parsing.
- **Do NOT add a daemon / state-file for `cdb rm` batching.** D-06 locked per-command-only.
- **Do NOT hand-roll deterministic signing for NAME blobs.** ML-DSA-87 is non-deterministic; two NAME blobs with identical logical content produce different content_hash. This is fine — D-02 tiebreak handles it.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| 4-byte prefix filter on blobs in a namespace | A new `ListByMagic=65` TransportMsgType | Existing `ListRequest` with `type_filter` flag (Phase 117 TYPE-03) | Server already indexes `blob_type` in seq_map value (storage.cpp:469); dispatcher already filters. Adding a new type doubles the work for zero correctness gain. [VERIFIED: message_dispatcher.cpp:486-499] |
| BOMB target iteration on the node | A coroutine that offloads per-target deletion to a thread pool | Synchronous loop on the executor, one `storage_.delete_blob_data` per target | Phase 121 STORAGE_THREAD_CHECK; batch-txn optimization is YAGNI. |
| Cross-invocation batching for `cdb rm` | Long-running daemon + state file | Per-command only (D-06) | Explicit user decision. |
| Name cache | `~/.cdb/name_cache.json` + invalidation logic | Re-enumerate on every `cdb get` (D-09) | Explicit user decision; stateless avoids cache-consistency class of bugs. |
| Deterministic NAME signing | Re-sign same payload → same blob | Let ML-DSA-87 non-determinism coexist with D-02 content_hash tiebreak | Project memory: "ML-DSA-87 signatures are non-deterministic — same data produces different blob_hash each time." |
| BOMB target existence check | Per-target `has_blob` pre-check | Skip (D-14) | Explicit user decision; BOMBs legitimately pre-mark not-yet-received blobs. |

**Key insight:** Every "don't hand-roll" above is either an explicit user decision (D-06, D-09, D-14) or a Phase 117 mechanism that already solves the problem. Phase 123 is almost entirely a composition phase.

## Runtime State Inventory

Phase 123 is NOT a rename/refactor/migration phase — it adds new blob magics and CLI commands without modifying any existing stored data. The canonical question ("what runtime state embeds the old string?") doesn't apply. Omitting this section per the research template.

**Nothing to migrate:** Fresh builds of the node + CLI handle the new magics natively. Old data dirs continue to function: existing blobs don't have NAME/BOMB magics, the new `is_name` / `is_bomb` helpers return false on them, and the existing code paths stay unchanged.

**One subtlety for the operator:** On an operator-wiped-between-122-and-123 deployment (which is already required by Phase 122 D-11), no concerns. On a running post-122 node before Phase 123 is deployed, incoming NAME blobs from a post-123 CLI would pass `is_pubkey_blob` (false) and sail through engine.cpp as regular signed user data; BOMB blobs likewise would pass signature verify and store — but the BOMB side-effect (delete N targets) wouldn't fire, leaving the BOMB stored but ineffective. Since Phase 124 co-deploys CLI+node, this scenario is ruled out in practice.

## Common Pitfalls

### Pitfall 1: BOMB ttl!=0 not enforced → delete propagation breaks under partial sync

**What goes wrong:** An expiring BOMB pruned by some peers but not all allows the non-pruned peers to re-propagate content the BOMB deleted.
**Why it happens:** Writer forgets ttl=0; node accepts the BOMB; BOMB expires on peer A; peer A re-syncs from peer B which still has the target; target re-appears.
**How to avoid:** Enforce ttl==0 at ingest (D-13). Reject with `ERROR_BOMB_TTL_NONZERO`. Same reasoning as single-tombstone ttl=0 (v0.5.0 hardened in v2.2.0, per project memory).
**Warning signs:** Any test that accepts a BOMB with ttl>0 is a red flag.

### Pitfall 2: Structural validation placed AFTER crypto::offload → DoS vector

**What goes wrong:** Adversary sends malformed BOMBs in flood; node burns ML-DSA-87 verify CPU (~5-10ms each) before discovering the blob is structurally bad.
**Why it happens:** Developer drops the check into Step 3.5 (after verify).
**How to avoid:** Mirror the Phase 122 Step 1.5 PUBK-first discipline — structural checks BEFORE offload. NEW Step 1.7 in engine.cpp.
**Warning signs:** `grep crypto::offload` appears BEFORE the BOMB check in engine.cpp.

### Pitfall 3: NAME resolution returns wrong blob under clock skew or simultaneous writes

**What goes wrong:** Two `cdb put --name foo` invocations race; both write in the same clock second. Under D-01 alone, resolution is ambiguous.
**Why it happens:** Clock resolution is seconds.
**How to avoid:** D-02 content_hash DESC tiebreak. ML-DSA-87 non-determinism ensures content_hashes are different even for semantically identical payloads, so the tiebreak always produces a winner deterministically across readers.
**Warning signs:** Any resolution code that returns `matches[0]` without sorting is broken.

### Pitfall 4: CLI `cdb rm` argv parsing accepts only one target (current code!)

**What goes wrong:** Phase 123 requires multi-target `cdb rm A B C D`, but `cli/src/main.cpp:503-552` (verified) only accepts a single `hash_hex` argv (extra non-flag tokens produce "Unknown rm option"). Reusing the current parser with no changes would emit N separate BOMBs on a shell glob and defeat the batching goal.
**Why it happens:** Current parser was built for the 999.2 backlog shape (one target pre-checked with Exists).
**How to avoid:** Rewrite the `cdb rm` argv loop to collect `std::vector<std::string> hash_hexes` (mirror `cdb get`'s pattern at `main.cpp:430-467`). One-target case still works (count=1 BOMB).
**Warning signs:** Tests that pass "two targets" and observe two separate BOMB blobs.

### Pitfall 5: NAME magic collision with existing magics

**What goes wrong:** Someone picks a 4-byte constant that collides with CENV / TOMB / DLGT / PUBK / CDAT / CPAR / (future) envelope variants. `extract_blob_type` (Phase 117) returns the colliding prefix, Phase 117's ls-filter miscategorizes.
**Why it happens:** Random choice.
**How to avoid:** `NAME_MAGIC = 0x4E414D45` "NAME" (ROADMAP-specified); `BOMB_MAGIC = 0x424F4D42` "BOMB". Verify against the full magic table:
- CENV = 0x43 0x45 0x4E 0x56 ("CENV") [VERIFIED: cli/src/wire.h:268]
- PUBK = 0x50 0x55 0x42 0x4B ("PUBK") [VERIFIED: codec.h:135]
- TOMB = 0xDE 0xAD 0xBE 0xEF [VERIFIED: codec.h:92]
- DLGT = 0xDE 0x1E 0x6A 0x7E [VERIFIED: codec.h:112]
- CDAT = 0x43 0x44 0x41 0x54 ("CDAT") [VERIFIED: cli/src/wire.h:277]
- CPAR = 0x43 0x50 0x41 0x52 ("CPAR") [VERIFIED: cli/src/wire.h:282]
- NAME = 0x4E 0x41 0x4D 0x45 ("NAME") — no collision
- BOMB = 0x42 0x4F 0x4D 0x42 ("BOMB") — no collision
**Warning signs:** Unit test that enumerates all known magics and asserts pairwise distinctness should pass.

### Pitfall 6: Phase 117 ls-filter hides NAME blobs from `cdb ls` by default

**What goes wrong:** User `cdb put --name foo file`; `cdb ls` shows only the content blob, not the NAME blob. User is confused.
**Why it happens:** `cli/src/wire.h:303-308` (`is_hidden_type`) currently hides PUBK / CDAT / DLGT. If NAME is added to this list, default `cdb ls` hides it. If NOT added, default ls shows NAME rows that aren't user-meaningful (raw NAME blobs aren't downloadable as files).
**Recommendation:** Add NAME to `is_hidden_type` (default hide). Named content is still visible by its content hash (unhidden); the NAME pointer is infrastructure. Keep BOMB hidden (same as TOMB).
**Caveat:** CONTEXT.md does not explicitly decide this — **planner should confirm** whether NAME and BOMB join the default-hide list. TOMB is currently visible by default (`is_hidden_type` checks PUBK, CDAT, DLGT only — NOT TOMB). So BOMB visibility should match: visible by default, hidden with `ls` type-filter. **Note: This is a Phase 117 ls-filter UX decision that rides along; could also defer to Phase 125 docs phase if the planner chooses.**

### Pitfall 7: D-04 arbitrary name bytes break in JSON / URL / printf

**What goes wrong:** D-04 allows arbitrary UTF-8 + non-UTF-8 bytes in names. A name like `"foo\x00bar"` or `"\xff\xfe"` breaks every tool that assumes null-terminated or printable names.
**Why it happens:** User chose maximum flexibility.
**How to avoid:** CLI display layer must render names safely (e.g., escape non-printable via `\xNN`). The codec/storage treats names as opaque bytes — `memcmp` only. Display is the CLI's job, not the protocol's.
**Warning signs:** Any `printf("%s", name)` at the CLI. Use a safe display helper instead.

## Code Examples

### Example 1: Magic-constant definitions (db/wire/codec.h insertion template)

Follow the exact idiom at codec.h:92-152. Source: [VERIFIED: db/wire/codec.h:92-105, 110-128, 130-152]

```cpp
// =============================================================================
// NAME (mutable name pointer — Phase 123)
// =============================================================================

/// 4-byte magic prefix identifying a NAME pointer blob.
inline constexpr std::array<uint8_t, 4> NAME_MAGIC = {0x4E, 0x41, 0x4D, 0x45}; // "NAME"

/// NAME payload minimum size: magic + name_len(0) + target_hash = 4 + 2 + 0 + 32 = 38.
/// (name_len == 0 is allowed structurally; name_len == 0 means "empty name".)
inline constexpr size_t NAME_MIN_DATA_SIZE = 4 + 2 + 0 + 32;

/// Check if blob data is a NAME blob (magic prefix + structurally well-formed).
bool is_name(std::span<const uint8_t> data);

/// Parsed NAME payload: the name as opaque bytes + the target content hash.
/// Returns nullopt if data is not a NAME blob or is structurally malformed.
struct NamePayload {
    std::span<const uint8_t> name;  // points into `data`; valid only while `data` lives
    std::array<uint8_t, 32> target_hash;
};
std::optional<NamePayload> parse_name_payload(std::span<const uint8_t> data);

/// Build NAME payload bytes: [NAME:4][name_len:2 BE][name][target_hash:32].
/// @throws std::invalid_argument if name.size() > 65535.
std::vector<uint8_t> make_name_data(std::span<const uint8_t> name,
                                     std::span<const uint8_t, 32> target_hash);

// =============================================================================
// BOMB (batched tombstone — Phase 123)
// =============================================================================

/// 4-byte magic prefix identifying a BOMB (batched-tombstone) blob.
inline constexpr std::array<uint8_t, 4> BOMB_MAGIC = {0x42, 0x4F, 0x4D, 0x42}; // "BOMB"

/// BOMB payload minimum size: magic + count(0) = 4 + 4 = 8. (BOMB-of-0 is allowed.)
inline constexpr size_t BOMB_MIN_DATA_SIZE = 4 + 4;

/// Check if blob data is a BOMB (magic prefix + structurally well-formed: count
/// matches data.size()). Returns false if data.size() < 8 or count mismatch.
bool is_bomb(std::span<const uint8_t> data);

/// Validate BOMB structural invariants (D-13(2)):
///   data.size() >= 8
///   data.size() == 8 + read_u32_be(data+4) * 32
bool validate_bomb_structure(std::span<const uint8_t> data);

/// Extract BOMB target hashes. @pre is_bomb(data).
/// Returns references INTO `data` (caller holds for lifetime of `data`).
/// For an owning copy, use the overload returning vector<array<u8,32>>.
std::vector<std::array<uint8_t, 32>> extract_bomb_targets(std::span<const uint8_t> data);

/// Build BOMB payload bytes: [BOMB:4][count:4 BE][target_hash:32]*count.
/// @throws std::invalid_argument if targets.size() would exceed UINT32_MAX.
std::vector<uint8_t> make_bomb_data(std::span<const std::array<uint8_t, 32>> targets);
```

### Example 2: Test helper extension (db/tests/test_helpers.h)

Follow the Phase 122 `make_signed_tombstone` + `make_pubk_blob` idioms. Source: [VERIFIED: db/tests/test_helpers.h:100-117, 165-202]

```cpp
/// Build a properly signed NAME blob for `name` → `target_hash` under `id`.
inline wire::BlobData make_name_blob(
    const identity::NodeIdentity& id,
    std::span<const uint8_t> name,
    std::span<const uint8_t, 32> target_hash,
    uint32_t ttl = 0,  // NAME defaults to permanent
    uint64_t timestamp = TS_AUTO)
{
    wire::BlobData blob;
    auto hint = crypto::sha3_256(id.public_key());
    std::memcpy(blob.signer_hint.data(), hint.data(), 32);
    blob.data = wire::make_name_data(name, target_hash);
    blob.ttl = ttl;
    blob.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;
    auto si = wire::build_signing_input(
        id.namespace_id(), blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(si);
    return blob;
}

/// Build a properly signed BOMB blob covering `targets` under `id`.
inline wire::BlobData make_bomb_blob(
    const identity::NodeIdentity& id,
    std::span<const std::array<uint8_t, 32>> targets,
    uint64_t timestamp = TS_AUTO)
{
    wire::BlobData blob;
    auto hint = crypto::sha3_256(id.public_key());
    std::memcpy(blob.signer_hint.data(), hint.data(), 32);
    blob.data = wire::make_bomb_data(targets);
    blob.ttl = 0;  // MUST be permanent (D-13)
    blob.timestamp = (timestamp == TS_AUTO) ? current_timestamp() : timestamp;
    auto si = wire::build_signing_input(
        id.namespace_id(), blob.data, blob.ttl, blob.timestamp);
    blob.signature = id.sign(si);
    return blob;
}
```

### Example 3: Ingest validation block placement (db/engine/engine.cpp Step 1.7)

Place between the existing Step 1.5 block (engine.cpp:181-195) and the existing Step 2 block (engine.cpp:197). See §Architecture Patterns Pattern 2 for the block content.

### Example 4: CLI multi-target `cdb rm` argv parse (cli/src/main.cpp)

Mirror the `cdb get` pattern at `main.cpp:430-467`. Source: [VERIFIED: cli/src/main.cpp:419-495]

```cpp
// Replace cli/src/main.cpp:508-540 with:
std::vector<std::string> hash_hexes;
std::string namespace_hex;
bool skip_confirm = false;
bool rm_force = false;

while (arg_idx < argc) {
    const char* a = argv[arg_idx];
    if (std::strcmp(a, "--namespace") == 0 || std::strcmp(a, "-n") == 0 ||
        std::strcmp(a, "--from") == 0) {
        if (arg_idx + 1 >= argc) { /* error */ return 1; }
        namespace_hex = argv[++arg_idx]; ++arg_idx;
    } else if (std::strcmp(a, "-y") == 0 || std::strcmp(a, "--yes") == 0) {
        skip_confirm = true; ++arg_idx;
    } else if (std::strcmp(a, "--force") == 0) {
        rm_force = true; ++arg_idx;
    } else if (a[0] != '-') {
        if (std::strlen(a) != 64) { /* error */ return 1; }
        hash_hexes.emplace_back(a); ++arg_idx;
    } else { /* unknown */ return 1; }
}

if (hash_hexes.empty()) { /* error */ return 1; }

// ... confirmation prompt (tweak to list all targets) ...
return cmd::rm(identity_dir_str, hash_hexes, namespace_hex, rm_force, opts);
// cmd::rm signature changes from (..., const std::string& hash_hex, ...)
// to (..., const std::vector<std::string>& hash_hexes, ...).
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| `cdb rm A; cdb rm B; cdb rm C` (N tombstones, N signatures) | `cdb rm A B C` (1 BOMB, 1 signature) | Phase 123 | ~30× storage saving at count=64; ~475× at count=1000. Amortization ratio = (2420 / (4 + count*32)); at count=1 it's ~61×; single-target tombstone still beats BOMB-of-1 by only 4 bytes. |
| `cdb put --name foo file` — didn't exist | NAME pointer blob with deterministic resolution | Phase 123 | Mutable human-named references. |
| `cdb rm` on a chunked manifest (Phase 119) cascades via `rm_chunked` (cli/src/commands.cpp:1018) | Unchanged — Phase 123's multi-target `cdb rm` applies to user-level content hashes, not to the internal chunk-cascade path | N/A | Phase 119's CPAR/CDAT tombstone cascade keeps its current per-target send loop (line 1856-1892). The multi-target BOMB path is the new outer `cdb rm A B C` shape. Planner may later unify; NOT in scope for Phase 123. |

**Deprecated/outdated:** None introduced in Phase 123. Phase 122 already deleted `Data=8` TransportMsgType branch.

## Assumptions Log

| # | Claim | Section | Risk if Wrong |
|---|-------|---------|---------------|
| A1 | Reusing `ListRequest + type_filter` suffices for D-10 (enumerate NAME blobs in ns) | §Primary Recommendation; §Don't Hand-Roll | If planner strongly prefers a dedicated `ListByMagic=65` for auditability / dispatcher symmetry: that's also fine; ~40 LOC new dispatcher branch + new FlatBuffers table OR hand-rolled wire. No correctness difference. |
| A2 | BOMB count==0 is accepted (no lower bound check) | §Architecture Patterns Pattern 2 | If planner prefers rejecting count==0 as malformed, add `if (count == 0) → malformed_blob`. Cost: one line. No strong arg either way. |
| A3 | BOMB of N targets deletes targets inline on ioc thread (no batch-txn helper) | §Architecture Patterns Pattern 4 | If N is large (~10k), per-target MDBX txns may be slow. Planner can optimize by adding `Storage::delete_blobs_data_batch`. Not required for correctness. |
| A4 | NAME should be added to `is_hidden_type` default-hide list (Phase 117) | §Common Pitfalls Pitfall 6 | Minor UX divergence. If planner defers this to Phase 125, the default `cdb ls` shows NAME rows that aren't user-meaningful until Phase 125 updates the hide list. |
| A5 | Phase 123's CLI work stays on pre-122 CLI wire format; Phase 124 handles joint 122+123 CLI adaptation | §Summary critical constraint #1 | If planner attempts to update the CLI wire to post-122 inside Phase 123, scope doubles and Phase 124 becomes nearly empty. Either is defensible. RECOMMEND: stay on pre-122 for Phase 123 CLI per ROADMAP phase split. |
| A6 | Phase 123 CLI tests exercise the CLI binary without hitting a live node (subprocess + loopback test node, or in-process engine tests only) | §Validation Architecture | User's CONTEXT.md explicitly says "Test against the live 192.168.1.73 node until post-124." Phase 123 test coverage is therefore unit (engine + wire) + CLI subprocess against a local test node fixture OR engine-only coverage. Planner decides based on existing `cli/tests/` presence (verify: `ls cli/tests/` below). |
| A7 | `cdb rm --force` / `-y` semantics carry forward to multi-target rm unchanged | §Code Examples Example 4 | Multi-target `--force` skips all pre-checks (D-14 says node doesn't verify; CLI pre-check is separate). Confirm logical expectation with the user / planner. |

## Open Questions

1. **Dedicated `ListByMagic=65` TransportMsgType vs reuse `ListRequest`?**
   - What we know: Phase 117 TYPE-03 added `type_filter` to ListRequest; dispatcher already filters on `seq_map.value[32..36]`; returns `[hash|seq|type|size|ts]` per entry.
   - What's unclear: Whether user's D-10 text "new TransportMsgType" was strict (must be new) or illustrative (any enumeration-by-prefix path is fine). CONTEXT.md "Claude's Discretion" lists the naming choice but not "reuse vs new" — the assumption was a new type.
   - Recommendation: Plan on reusing ListRequest; call it out to user during planning so the decision is explicit.

2. **BOMB count==0 — accept or reject?**
   - What we know: D-13 says reject malformed. Count==0 is structurally valid (data.size==8).
   - What's unclear: Whether a "BOMB with no targets" is semantically malformed.
   - Recommendation: Accept for now (YAGNI); can be tightened later.

3. **`cdb rm --replace` (D-15 override path) — BOMB-of-1 or single tombstone?**
   - What we know: BOMB-of-1 is 4 bytes more than a tombstone; single code path wins on `no_duplicate_code`.
   - What's unclear: Whether the planner/user values the 4-byte density over code unity.
   - Recommendation: BOMB-of-1 for uniformity. Planner's call.

4. **Should NAME and BOMB join `is_hidden_type` default-hide list?**
   - What we know: Phase 117 hides PUBK / CDAT / DLGT.
   - What's unclear: Whether NAME (and BOMB) belong with the infrastructure-blob group.
   - Recommendation: Yes for NAME (infrastructure pointer). No for BOMB (mirrors TOMB's default visibility).

5. **CLI tests infrastructure — does `cli/tests/` exist?**
   - What we know: Phase 999.4 backlog said `cli/tests` exists but post-sweep behavior has no coverage.
   - What's unclear: Current state of cli/tests directory.
   - Recommendation: Planner verifies `ls cli/tests/` during plan-01, determines whether Phase 123 adds a first CLI-E2E test fixture or defers to 999.4. If deferred, engine-level coverage is sufficient.

## Environment Availability

Skipped — Phase 123 introduces no new external dependencies. All required tooling (liboqs, libmdbx, FlatBuffers, Catch2, spdlog) is already pinned in the CMake tree and built on previous phases. No new runtime, service, or CLI utility requirements.

## Validation Architecture

> Nyquist validation is active (`.planning/config.json` has no `workflow.nyquist_validation: false`; default is enabled).

### Test Framework

| Property | Value |
|----------|-------|
| Framework | Catch2 v3 (pinned in CMakeLists.txt via FetchContent) |
| Config file | `CMakeLists.txt` + `db/tests/CMakeLists.txt` (no separate catch2 config) |
| Quick run command | `./build/db/chromatindb_tests "[phase123]"` (runs only Phase 123 tagged tests) |
| Full suite command | `./build/db/chromatindb_tests` (runs entire Catch2 suite) — **hand to user per `feedback_delegate_tests_to_user.md`** |
| Build command | `cmake --build build -j$(nproc)` (NEVER `--parallel` per user preference) — **hand to user** |

### Phase Requirements → Test Map

| SC | Behavior | Test Type | Automated Command | File |
|----|----------|-----------|-------------------|------|
| SC-1 | NAME_MAGIC defined in both codec.h and cli/src/wire.h with identical bytes | unit (wire) | `./build/db/chromatindb_tests "[phase123][wire]"` | `db/tests/wire/test_name_bomb_codec.cpp` (NEW) |
| SC-1 | `is_name(data)` returns true on well-formed NAME; false on short / magic-mismatch | unit (wire) | same | `db/tests/wire/test_name_bomb_codec.cpp` |
| SC-1 | `parse_name_payload` / `make_name_data` round-trip arbitrary bytes including non-UTF-8 | unit (wire) | same | `db/tests/wire/test_name_bomb_codec.cpp` |
| SC-2 | BOMB_MAGIC defined; `is_bomb` structural validation (size + count consistency) | unit (wire) | same | `db/tests/wire/test_name_bomb_codec.cpp` |
| SC-2 | `validate_bomb_structure` rejects `data.size() < 8`, count*32+8 mismatch | unit (wire) | same | `db/tests/wire/test_name_bomb_codec.cpp` |
| SC-2 | BOMB with ttl > 0 rejected at ingest with IngestError::invalid_ttl / ERROR_BOMB_TTL_NONZERO | unit (engine) | `./build/db/chromatindb_tests "[phase123][bomb][engine]"` | `db/tests/engine/test_bomb_validation.cpp` (NEW) |
| SC-2 | BOMB with malformed header rejected pre-offload | unit (engine) | same | `db/tests/engine/test_bomb_validation.cpp` |
| SC-2 | BOMB from delegate rejected with IngestError::no_delegation | unit (engine) | same | `db/tests/engine/test_bomb_validation.cpp` — template: `test_delegate_replay.cpp` |
| SC-2 | Well-formed BOMB(count=3) deletes all 3 targets; dedup-safe re-ingest | unit (engine) | `./build/db/chromatindb_tests "[phase123][bomb][engine]"` | `db/tests/engine/test_bomb_side_effects.cpp` (NEW) |
| SC-5 | NAME overwrite resolution: ts DESC, content_hash DESC tiebreak | unit (engine or wire-level resolution helper) | `./build/db/chromatindb_tests "[phase123][name][resolution]"` | `db/tests/engine/test_name_resolution.cpp` (NEW) — in-process, uses make_name_blob |
| SC-6 | `cdb rm A B C` emits ONE BOMB covering all 3 targets (argv → BOMB count=3) | CLI unit OR integration | TBD — planner decides between: (a) test cmd::rm in-process with a fake Connection, (b) subprocess-drive cdb against a test node fixture | `cli/tests/test_rm_multi_target.cpp` (NEW if cli/tests/ supports in-process testing) OR defer to 999.4 |
| SC-3 | `cdb put --name foo file` emits content + NAME blobs | CLI unit | same as above | `cli/tests/test_put_name.cpp` |
| SC-4 | `cdb get foo` resolves via enumeration + winning target | CLI integration | subprocess-drive against test node fixture | `cli/tests/test_get_by_name.cpp` |
| SC-7 | Node accepts NAME/BOMB opaquely — no custom routing, both ride BlobWrite envelope | integration / sync | regression via existing `[phase122]` sync tests + new `[phase123]` engine tests | — |

### Sampling Rate

- **Per task commit:** `./build/db/chromatindb_tests "[phase123]"` — fast subset (< 5s expected based on Phase 122 patterns). Running this between tasks is cheap and is the "quick run" unit.
- **Per wave merge:** Full Catch2 suite — **hand to user** per `feedback_delegate_tests_to_user.md`. Ask: "please run `./build/db/chromatindb_tests` and paste output."
- **Phase gate:** Full suite green before `/gsd-verify-work`. Same hand-to-user rule applies.

### Wave 0 Gaps

- [ ] `db/tests/wire/test_name_bomb_codec.cpp` — covers SC-1, SC-2 codec layer
- [ ] `db/tests/engine/test_bomb_validation.cpp` — covers SC-2 D-13 ingest rejection
- [ ] `db/tests/engine/test_bomb_side_effects.cpp` — covers SC-2 BOMB deletes N targets
- [ ] `db/tests/engine/test_name_resolution.cpp` — covers SC-4 / SC-5 resolution algorithm (in-process, calls CLI resolution helper directly OR engine-level simulation)
- [ ] `db/tests/test_helpers.h` — ADD `make_name_blob`, `make_bomb_blob` (template in §Code Examples Example 2)
- [ ] `db/CMakeLists.txt` test target — register the new test files into `chromatindb_tests` executable (mirrors how Phase 122 test files were registered; planner confirms by inspection)

### Framework/Tests Already Present

- [x] Catch2 target exists (`./build/db/chromatindb_tests`) — verified via phase 122 SUMMARYs
- [x] `[phase122]` tag convention established → continue with `[phase123]`
- [x] `make_signed_blob`, `make_signed_tombstone`, `make_signed_delegation`, `make_pubk_blob`, `register_pubk` helpers available
- [x] TempDir, run_async, current_timestamp infra available

## Project Constraints (from CLAUDE.md + memory)

No `CLAUDE.md` exists in the repository root (verified by `ls` — parent directories contain only `.claude/settings.local.json` and `.claude/worktrees`, no project-level `CLAUDE.md`). Project directives come from `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/`:

| Directive | Binding on Phase 123 |
|-----------|---------------------|
| `feedback_no_duplicate_code.md` — Utilities go into shared headers; never copy-paste | `is_name` / `is_bomb` / `validate_bomb_structure` / parse / make helpers live in `db/wire/codec.h` + `codec.cpp`. Do NOT inline in engine.cpp or message_dispatcher.cpp. Mirror to `cli/src/wire.h` + `wire.cpp`. |
| `feedback_no_backward_compat.md` — Delete replaced code; protocol changes are fine | Phase 123 adds new surface only; no deletion required. No compat shims. |
| `feedback_delegate_tests_to_user.md` — Orchestrator-level `cmake --build` / `chromatindb_tests` runs → hand to user | Every time the plan needs a full test run, ASK the user ("please run X and paste output"). Subagent plan executors MAY run builds/tests in their own scope. |
| `project_phase123_bomb_ttl_zero.md` — BOMB ttl=0 enforced at ingest | Non-negotiable; D-13(1). |
| `feedback_no_python_sdk.md` — `cdb` is the only client; don't design for other SDKs | No hypothetical clients. |
| `project_db_active_dev.md` — DB node in active dev; no backward compat | Implicit in feedback_no_backward_compat.md. |
| YAGNI, no parallel cmake build | `cmake --build build -j$(nproc)` only; no speculative features (D-09 cache, D-06 daemon, D-14 existence checks). |
| `db/util/` headers, not inline in main.cpp | Shared helpers (if any cross both CLI and node) live in `db/wire/codec.h` or `db/util/`. |

## Security Domain

Security enforcement is not explicitly toggled by `.planning/config.json`. Assuming enabled per the research template default.

### Applicable ASVS Categories

| ASVS Category | Applies | Standard Control |
|---------------|---------|-----------------|
| V2 Authentication | yes | ML-DSA-87 signature on every NAME / BOMB blob; Phase 122 signing form (SHA3(target_namespace ‖ data ‖ ttl ‖ timestamp)). No new auth surface. |
| V3 Session Management | no | N/A — blob writes are idempotent; no session state. |
| V4 Access Control | yes | Owner-only for BOMB (D-12 = extend engine.cpp:266-280 delegate-rejection list); NAME allowed for delegates (D-11). |
| V5 Input Validation | yes | D-13 structural validation on BOMB ingest BEFORE crypto offload (adversarial-flood defense); NAME payload validation happens CLIENT-SIDE (parse_name_payload) — node treats NAME as opaque per ROADMAP SC-7. |
| V6 Cryptography | yes | Reuses ML-DSA-87 (signatures), SHA3-256 (content hash, signing input). No new crypto primitives. Never hand-roll. |

### Known Threat Patterns for Phase 123

| Pattern | STRIDE | Standard Mitigation |
|---------|--------|---------------------|
| Adversarial flood of malformed BOMBs (DoS via crypto CPU burn) | Denial-of-Service | D-13 structural validation pre-offload at Step 1.7 (mirror of Phase 122 Step 1.5 PUBK-first adversarial-flood defense at engine.cpp:187-195). |
| BOMB ttl>0 enables delete-propagation failure under partial sync pruning | Tampering / integrity | D-13(1) reject ttl != 0; same invariant as single-tombstone ttl=0. [VERIFIED: project_phase123_bomb_ttl_zero.md] |
| Delegate attempts to delete owner's blobs via BOMB | Elevation of Privilege | D-12 delegate-reject at Step 2 trailer (mirror of engine.cpp:274-279). |
| NAME pointer with forged target — delegate or attacker makes `foo` point at arbitrary hash | Tampering | NAME blob is signature-verified by the owner's (or delegate's) ML-DSA-87 key; the signing sponge binds `target_namespace`, so cross-namespace replay is blocked (D-01 absorption per Phase 122 D-13). |
| NAME content_hash-tiebreak hash-collision attack (attacker finds two NAME bodies with same content_hash) | Integrity | SHA3-256 collision resistance ~2^128 (D-02 relies on this). Not a realistic attack surface. |
| BOMB with `count` field overflowing → integer overflow in size check | Input Validation | D-13(2) structural check uses `size_t` arithmetic; `expected = 8 + count*32` is checked via `data.size() != expected`. Max count is bounded by `MAX_BLOB_DATA_SIZE = 500 MiB`, i.e., `count ≤ (500MiB - 8) / 32 ≈ 16.4M` targets — structurally well-defined, no overflow risk on 64-bit. |
| ListRequest with `type_filter = NAME_MAGIC` used for namespace reconnaissance | Information Disclosure | Existing ListRequest is already namespace-scoped; no new surface. |

## Sources

### Primary (HIGH confidence — verified against current codebase on master tip)

- `db/wire/codec.h` (whole file, 168 lines) — magic constants, is_* helpers, BlobData shape.
- `db/engine/engine.cpp` (lines 105-582) — full ingest + delete_blob pipeline with annotated steps.
- `db/engine/engine.h` (whole file) — IngestError enum, public API.
- `db/storage/storage.h` (whole file) — StoreResult, store_blob family, PrecomputedBlob.
- `db/storage/storage.cpp` (lines 100-317, 440-500, 820-977) — encryption, seq_map format with `blob_type:4`, cursor iteration.
- `db/peer/message_dispatcher.cpp` (lines 1-120, 325-585, 1379-1497) — BlobWrite + Delete + ListRequest handling.
- `db/peer/error_codes.h` (whole file) — ERROR_PUBK_* codes, error_code_string.
- `db/schemas/blob.fbs` + `db/schemas/transport.fbs` — post-122 Blob shape + BlobWriteBody envelope.
- `db/tests/test_helpers.h` (whole file) — make_*_blob helpers, ns_span, register_pubk, run_async.
- `db/tests/engine/test_pubk_first.cpp` + `test_delegate_replay.cpp` — structural templates.
- `cli/src/wire.h` (whole file, 348 lines) — pre-122 CLI wire shape (see caveat in Summary constraint #1).
- `cli/src/main.cpp` (lines 340-552) — argv parsing for put/get/rm.
- `cli/src/commands.cpp` (lines 907-1092, 1260-1482) — current put/get/rm/ls/list_hashes implementations.
- `cli/src/commands.h` — cmd::rm / cmd::put / cmd::get signatures.
- `.planning/phases/122-.../122-CONTEXT.md`, `122-04-SUMMARY.md`, `122-05-SUMMARY.md` — post-122 engine/transport architecture.
- `.planning/ROADMAP.md` § Phase 123 — success criteria 1-7.
- `.planning/REQUIREMENTS.md` — no IDs mapped to Phase 123 (SCs are the contract).
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/project_phase123_bomb_ttl_zero.md` — BOMB ttl=0 invariant.
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/feedback_no_duplicate_code.md` — shared-header rule.
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/feedback_no_backward_compat.md` — no compat shims.
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/feedback_delegate_tests_to_user.md` — test-run delegation rule.

### Secondary (MEDIUM confidence — inferred from context)

- Phase 117 TYPE-01..04 indexing mechanics — inferred from `storage.cpp:464-471` (seq_map stores `[hash:32][type:4]`) and `message_dispatcher.cpp:486-499, 520-549` (type_filter branch). Not directly verified against Phase 117 SUMMARY (SUMMARY not read during this research).

### Tertiary (LOW confidence)

- None. Every claim in this document is grounded in a concrete code citation or an explicit user decision in CONTEXT.md.

## Metadata

**Confidence breakdown:**

- Standard stack: HIGH — every library is already present and has been stable across phases 111-122.
- Architecture (ingest step placement, delegate-reject pattern, side-effect pattern): HIGH — derived directly from engine.cpp line-level evidence.
- Pitfalls: HIGH — every pitfall is grounded in existing code behavior or locked user decisions.
- CLI adaptation path: MEDIUM — Summary constraint #1 (Phase 123 stays on pre-122 CLI vs co-lands 122 CLI adaptation) is a planner decision and the RESEARCH's recommendation is explicit but reviewable.
- ListByMagic vs ListRequest reuse: MEDIUM — A1 is a real assumption. Both designs are correct; the research recommends reuse but flags the choice.

**Research date:** 2026-04-20
**Valid until:** 2026-05-20 (30 days — the relevant code areas are mature and unlikely to churn; extend only if Phase 124 lands before Phase 123 planning completes).

---
*Phase: 123-tombstone-batching-and-name-tagged-overwrite*
*Research gathered: 2026-04-20*
