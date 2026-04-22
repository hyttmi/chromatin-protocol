# Phase 123: Tombstone Batching + Name-Tagged Overwrite — Context

**Gathered:** 2026-04-20
**Status:** Ready for planning

<domain>
## Phase Boundary

Ship two new blob magics — `NAME` (mutable-name overwrite) and `BOMB` (batched tombstone) — and the CLI flows that use them. Goals: `cdb put --name foo` writes a human-named blob; `cdb get foo` resolves the name; overwrite semantics are deterministic; `cdb rm A B C...` produces a single BOMB blob amortizing one PQ signature across N targets (expected 200–300× storage reduction vs. N single tombstones). Node-side work is deliberately minimal — limited to (a) BOMB ingest validation (ttl=0, header sanity, owner-only), and (b) a thin generic `ListByMagic(ns, magic_prefix)` transport endpoint so the CLI can enumerate NAME blobs without a full namespace scan.

**This phase DOES:**
- Define `NAME_MAGIC` (0x4E414D45, `"NAME"`) and `BOMB_MAGIC` (TBD, likely 0x424F4D42, `"BOMB"`) in both `db/wire/codec.h` and `cli/src/wire.h`.
- Define `is_name()` / `is_bomb()` helpers alongside existing `is_tombstone` / `is_delegation` / `is_pubkey_blob`.
- Add node-side `BOMB` ingest validation: `ttl == 0` (mandatory, same reason as single tombstones), structural sanity (`data.size() == 8 + count*32`), and delegate-rejection (`is_bomb && source == delegate → reject`, mirrors engine.cpp:275 tombstone-delegate rejection).
- Add a new `ListByMagic` TransportMsgType that takes `(namespace:32, magic_prefix:4)` and returns the matching blobs (semantically opaque — prefix compare only, no payload parsing).
- Add `cdb put --name <name> [--replace] <file>` → writes content blob + NAME blob; with `--replace`, also emits a single-target BOMB for the prior NAME's content.
- Add `cdb get <name>` — stateless lookup via `ListByMagic` + sort by (timestamp DESC, content_hash DESC), fetches the content blob of the winning NAME.
- Change `cdb rm` to accept multiple targets in one invocation and emit ONE `BOMB` blob per invocation covering all provided targets.
- Add Catch2 coverage for: BOMB ttl=0 enforcement, BOMB-delegate rejection, BOMB structural validation, NAME overwrite deterministic resolution (timestamp tiebreak + content_hash tiebreak), `cdb put --name` / `--replace` end-to-end.

**This phase does NOT:**
- Build a persistent `~/.chromatindb/name_cache.json` — user chose stateless-always-enumerate (see D-09 below).
- Build a long-running batching daemon for `cdb rm` — user chose per-command-only (see D-06 below).
- Add a server-side NAME index DBI (explicit "future 999.x phase" per ROADMAP).
- Allow delegates to emit BOMB blobs (consistent with single-tombstone rule at engine.cpp:275).
- Add explicit `seq:N` field to NAME payload — `blob.timestamp` serves as the seq (D-01).
- Break Phase 122's wire format or signing form — NAME and BOMB are opaque-body signed blobs that ride the post-122 BlobWrite envelope unchanged.
- Touch PROTOCOL.md / README.md (Phase 125).
- Test against the live `192.168.1.73` node until post-124 (CLI + node are redeployed together after 124 lands).

</domain>

<decisions>
## Implementation Decisions

### NAME Magic + Overwrite Semantics

- **D-01:** Seq source = `blob.timestamp` (uint64 seconds). No explicit seq field in the NAME payload. `cdb put --name foo` produces a NAME blob whose outer `blob.timestamp` is the authoritative seq. Resolution rule: among all NAME blobs whose payload names `foo`, the winner has the highest `blob.timestamp`. Already signed, already monotonic under sane clocks, zero new state.

- **D-02:** Tiebreak when two NAME blobs share the same `blob.timestamp` (i.e., same second) = `content_hash` DESC (lexicographic descending). Deterministic across all readers; no node coordination needed. SHA3 output distribution makes ties arbitrarily but uniformly resolved.

- **D-03:** NAME payload layout = `[magic:4][name_len:2 BE][name:N bytes][target_content_hash:32]`. Minimal per ROADMAP. Resolution uses outer `blob.timestamp` (D-01) + `content_hash` tiebreak (D-02). No internal seq field.

- **D-04:** Name charset + length = UTF-8, `name_len ∈ [1, 65535]` bytes, **no further restrictions**. User explicitly chose maximum flexibility over POSIX-safe subset. Names may contain any bytes. CLI callers are responsible for shell-escaping at the invocation site; the codec treats `name` as opaque bytes for all comparisons (memcmp).

- **D-15:** Overwrite policy = **opt-in `--replace` flag**. `cdb put --name foo file` writes content + NAME only, leaves prior content reachable by hash. `cdb put --name foo --replace file` writes content + NAME AND emits a single-target BOMB (or a plain tombstone — planner's call) for the prior content's hash. Explicit, non-surprising, preserves history by default.

### BOMB Magic + Batching Policy

- **D-05:** BOMB target encoding = `target_content_hash:32` only. Payload layout: `[magic:4][count:4 BE][target_hash:32 × count]`. Self-contained — no seq→hash lookup needed on the verify path. Matches single-tombstone layout semantics (one target → N targets). Per-target cost is 32 bytes, amortized against one PQ signature (2420 bytes) the amortization ratio at count=64 is ~30×; at count=1000 it's ~475×.

- **D-06:** Batch store = **per-command only**. `cdb rm A B C` produces ONE BOMB covering {A, B, C}. Separate shell invocations (`cdb rm A; cdb rm B`) produce TWO separate BOMBs — NO cross-invocation coalescing, NO daemon, NO state file. YAGNI; a user wanting batch-efficiency passes many targets in one call.

- **D-07:** Within one invocation = one BOMB per command regardless of count. `cdb rm <500 targets>` → ONE BOMB with count=500 (~16 KB payload, well under MAX_BLOB_DATA_SIZE 500 MiB). No intra-invocation K-chunking — contradicts the amortization goal.

- **D-08:** No time-based flush trigger in per-command mode (D-06 makes the timer vacuous). `cdb rm` is synchronous: parse argv → build target list → sign ONE BOMB → submit → exit.

### Name Resolution

- **D-09:** **No name cache.** CLI is stateless — every `cdb get foo` enumerates NAME blobs in the namespace on every read. Deviates from the ROADMAP text ("resolves via local `~/.chromatindb/name_cache.json` with enumeration fallback") — user chose no-cache. Rationale: no stale-cache footguns, no invalidation logic, no fresh-machine warmup, no cache-file schema versioning. Trade-off is read latency, which depends on D-10's enumeration endpoint performance.

- **D-10:** Enumeration API = new node-side `ListByMagic(namespace:32, magic_prefix:4)` TransportMsgType. Server scans the namespace's blob DBI with a prefix comparison on `blob.data[0..4]`, returns matching blobs (or hashes — planner decides) up to a reasonable page size. **Semantically opaque**: the node does not parse NAME payload, does not understand "name resolution" — it just filters by the first 4 bytes of blob.data. This preserves the ROADMAP's "node treats NAME/BOMB as opaque" invariant (opaque = doesn't interpret, not = zero node code change).

### Delegate Permissions

- **D-11:** Delegates **CAN** write NAME blobs in the owner's namespace. Node does not reject NAME-from-delegate at ingest. Rationale: a delegate authorized to write content blobs should be able to label them; the signed blob carries the delegate's `signer_hint` so the owner can audit who named what. Matches "trackable, not blocked".

- **D-12:** Delegates **CANNOT** emit BOMB blobs. Extends the existing tombstone-owner-only rule (engine.cpp:275) to batched tombstones. Deletion (single or batched) is owner-privileged. Node enforces at ingest: `(source == delegate) && is_bomb(blob.data) → reject` with a protocol-level error (mirrors the `delegates cannot create tombstone blobs` rejection).

### Node Validation Scope

- **D-13:** On every BOMB ingest the node validates:
  1. **`ttl == 0` (mandatory).** Reject any BOMB with `ttl != 0` — same reasoning as single-tombstone ttl=0: an expiring BOMB would allow peers that pruned the BOMB to re-propagate the supposedly-deleted blobs. Rule lives at the same ingest code path as the existing tombstone ttl check (engine.cpp:149 region).
  2. **Header structural sanity.** Reject if `data.size() < 8` (no room for magic+count) OR `data.size() != 8 + count*32` (declared count doesn't match payload length). Cheap and prevents malformed BOMBs from reaching Storage.
  3. **Delegate rejection (D-12).** `is_bomb(blob.data) && source == delegate → reject`.

- **D-14:** Node **does NOT** verify each BOMB target_hash is a known blob. BOMB can legitimately target blobs the node hasn't received yet (e.g., pre-marking during sync catch-up). Verification would be O(N) cursor lookups per BOMB for zero real correctness benefit.

### Claude's Discretion

- Exact BOMB magic bytes (proposal: `0x424F4D42` "BOMB"). Any 4-byte constant that doesn't collide with existing magics works.
- Exact NAME magic bytes — ROADMAP already specifies `0x4E414D45` "NAME".
- Exact name of the new TransportMsgType for D-10 (`ListByMagic = 65` vs `ListBlobsByPrefix = 65` vs extending an existing List request with a `magic_prefix` field). Planner decides based on dispatcher symmetry.
- Whether D-15's overwrite path emits a BOMB (count=1) or a single-target tombstone. BOMB-of-1 is 4 bytes larger (count field) but keeps one code path; single tombstone reuses existing TOMBSTONE_MAGIC + tombstone handling. Planner picks whichever minimizes code duplication (`feedback_no_duplicate_code.md`).
- Exact page size / pagination semantics of ListByMagic. A simple count-capped single-response works for MVP; cursor-based paging can be added when a real user hits the cap.
- Test file naming — follow the phase 122 convention (`db/tests/<area>/test_<feature>.cpp` with `[phase123]` tags).
- Whether `is_name()` / `is_bomb()` go into `db/util/magic_check.h` (shared header) or stay inline in `codec.h`. Existing magics are inline in codec.h — stay consistent unless the function count crosses an obvious threshold.
- Error-code values for new BOMB rejection reasons in `db/peer/error_codes.h` (`ERROR_BOMB_TTL_NONZERO`, `ERROR_BOMB_MALFORMED`, `ERROR_BOMB_DELEGATE_NOT_ALLOWED` — planner picks bytes).

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Roadmap + Phase Constraints (non-negotiable)
- `.planning/ROADMAP.md` § Phase 123 — Goal, Success Criteria #1–7, depends-on Phase 122.
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/project_phase123_bomb_ttl_zero.md` — BOMB must be ttl=0, enforced at ingest. Non-negotiable.
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/feedback_no_backward_compat.md` — No backward compat on either binary.
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/feedback_no_duplicate_code.md` — Utilities into shared headers, not copy-pasted.
- `~/.claude/projects/-home-mika-dev-chromatin-protocol/memory/feedback_delegate_tests_to_user.md` — Orchestrator-level `cmake --build` / `chromatindb_tests` runs → hand to user.

### Prior Phase Context
- `.planning/phases/122-schema-signing-cleanup-strip-namespace-and-compress-pubkey/122-CONTEXT.md` — D-01..D-13. Phase 122 signing canonical form (`SHA3(target_namespace || data || ttl || timestamp)`) and BlobWrite envelope apply to NAME + BOMB unchanged. Ingest dispatch path (engine.cpp Step 1.5 PUBK-first, Step 2 verify) is the site for new BOMB validation.
- `.planning/phases/122-schema-signing-cleanup-strip-namespace-and-compress-pubkey/122-04-SUMMARY.md` — engine API post-122 (`BlobEngine::ingest(target_namespace, blob, source) → IngestResult`), `IngestError` enum, wire-error codes.
- `.planning/phases/122-schema-signing-cleanup-strip-namespace-and-compress-pubkey/122-05-SUMMARY.md` — BlobWrite envelope + per-blob `[ns:32B]` sync framing. NAME and BOMB blobs ride this envelope unchanged.

### Existing Code — codec + wire
- `db/wire/codec.h` — existing magic constants and `is_*` helpers. New NAME and BOMB magics + helpers go here.
  - `TOMBSTONE_MAGIC = {0xDE, 0xAD, 0xBE, 0xEF}` at codec.h:92 — structural template for NAME/BOMB magic constants.
  - `DELEGATION_MAGIC = {0xDE, 0x1E, 0x6A, 0x7E}` at codec.h:112 — same.
  - `PUBKEY_MAGIC = {0x50, 0x55, 0x42, 0x4B}` at codec.h:135 — same.
  - `is_tombstone(span)` at codec.h:98 — template for `is_name()` / `is_bomb()` signature shape.
  - `is_pubkey_blob(span)` at codec.h:141 — inline helper analog.
- `cli/src/wire.h` — CLI-side mirror of codec magics. NAME_MAGIC and BOMB_MAGIC must be defined identically here.

### Existing Code — engine ingest (site for new validation)
- `db/engine/engine.cpp:149` region — current `blob.ttl > 0` check for tombstones. BOMB ttl=0 enforcement extends the same pattern.
- `db/engine/engine.cpp:160-166` region — max_ttl enforcement with tombstone exemption. BOMB is also permanent — likely exempted by the same rule (`!is_tombstone(data) && !is_bomb(data)`).
- `db/engine/engine.cpp:274-279` — `delegates cannot create tombstone blobs` rejection. D-12 extends this to `is_bomb`.
- `db/engine/engine.cpp` Step 1.5 (Phase 122) — PUBK-first gate. New BOMB validation fits BEFORE signature verify (like PUBK-first), since malformed BOMBs are cheap to reject pre-crypto.

### Existing Code — transport dispatcher
- `db/peer/message_dispatcher.cpp` — TransportMsgType switch. Add new `ListByMagic` branch (D-10). Phase 122's `BlobWrite = 64` added at this layer; D-10 adds another type at `65` or similar.
- `db/schemas/transport.fbs` — add a new `ListByMagicRequest` table + `ListByMagicResponse` table if using FlatBuffers for the new message body. Or hand-rolled wire framing, following the `encode_blob_transfer`-style idiom (`[ns:32][magic:4]` request, `[count:u32BE][hash:32 × count]` response).

### Existing Code — CLI command surface
- `cli/src/commands.cpp` / `cli/src/commands.h` — entry points for `cdb put` and `cdb rm`. New code: `--name`, `--replace` flags on `put`; multi-target argv parsing on `rm` plus single-BOMB emission.
- `cli/src/main.cpp:360` — usage text for `cdb put` (currently `cdb put <file>... [--share ...] [--ttl ...] [--stdin]`). Extend with `--name` and `--replace`.
- New CLI: `cdb get <name>` subcommand for NAME resolution via D-10 ListByMagic.
- `cli/src/wire.h` — mirror the new magics + add codec helpers for building NAME / BOMB payloads.

### Existing Code — tests
- `db/tests/test_helpers.h` — post-122 helpers (`make_signed_blob`, `make_tombstone_blob`, `make_pubk_blob`, `register_pubk`). Add `make_name_blob(ns, id, name, target_hash)` and `make_bomb_blob(ns, id, target_hashes[])` following the same pattern.
- `db/tests/engine/test_pubk_first.cpp` — structural template for the new `test_bomb_validation.cpp` (if the planner splits it) — one concern per test file.
- `db/tests/engine/test_delegate_replay.cpp` — structural template for delegate-rejection tests on BOMB.

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- **`wire::is_tombstone / is_delegation / is_pubkey_blob`** helper idiom in `db/wire/codec.h` — `is_name()` and `is_bomb()` go alongside, identical shape.
- **Magic-constant definition idiom** (inline `constexpr std::array<uint8_t, 4>`) — copy pattern.
- **`db/tests/test_helpers.h` post-122 make_*_blob helpers** — add `make_name_blob` + `make_bomb_blob` following the same shape (build BlobData with the right magic, sign, return).
- **Phase 122's `BlobWrite` envelope** — NAME and BOMB blobs ride this unchanged. No new transport envelope required for the write path.
- **Tombstone ttl=0 enforcement (engine.cpp:149)** — structural template for BOMB ttl=0 enforcement.
- **Tombstone-delegate rejection (engine.cpp:275)** — structural template for BOMB-delegate rejection (D-12).
- **Existing dispatcher pattern (message_dispatcher.cpp)** — new `ListByMagic` branch follows the established switch-case + fbs-decode + reply shape.

### Established Patterns
- Big-endian everywhere. NAME `name_len:2` and BOMB `count:4` are BE.
- All blob timestamps are uint64 seconds.
- ttl=0 is permanent; NAME is semantically permanent too (user can let it live indefinitely, overwrites win by timestamp). ttl>0 for NAME is allowed — it's a mutable pointer whose expiry is up to the writer.
- Signed blobs ride the post-122 BlobWrite envelope; NAME and BOMB inherit that unchanged.
- STORAGE_THREAD_CHECK on all Storage public methods; new `list_by_magic` helper (if added to Storage surface) must follow suit.
- Tests tagged `[phase123][...]`. Subset run: `./build/db/chromatindb_tests "[phase123]"`.
- Post-back-to-ioc idiom after any new `crypto::offload` on the verify path (inherited from Phase 121 D-07).

### Integration Points
- `db/engine/engine.cpp` ingest — add BOMB validation (D-13) BEFORE signature verify (cheap reject first), after PUBK-first gate.
- `db/wire/codec.h` — new magics + helpers.
- `cli/src/wire.h` — mirror the new magics.
- `db/peer/message_dispatcher.cpp` — new `ListByMagic` TransportMsgType branch.
- `db/schemas/transport.fbs` — new request/response tables for `ListByMagic` OR hand-rolled wire framing. Planner decides; FlatBuffers for symmetry with BlobWrite, hand-rolled for code compactness.
- `db/storage/storage.cpp` — possibly a new `list_by_magic(ns, magic_prefix, limit, cursor_token) → vector<blob>` public method. STORAGE_THREAD_CHECK, try/catch, cursor iteration over `blobs_map` with prefix compare. Planner decides whether to add a Storage method or do the iteration inline in the dispatcher.
- `cli/src/commands.cpp` — `cdb put --name` / `cdb put --name --replace` / `cdb get` / multi-target `cdb rm`.

</code_context>

<specifics>
## Specific Ideas

- **ROADMAP deviation (D-09):** ROADMAP says "resolves name via local `~/.chromatindb/name_cache.json` with NAME-blob enumeration fallback". User chose no-cache. Planning must treat the cache as deferred and implement only the enumeration path.
- **ROADMAP clarification (D-10):** ROADMAP says "Node treats NAME and BOMB as opaque signed blobs — no node code changes required for correctness". User's choice of `ListByMagic` as the enumeration primitive IS a node code change — but it's **semantically opaque** (prefix compare, no payload parsing). Planning should document this as "node stays opaque to NAME/BOMB semantics; thin generic prefix filter is the only added server capability".
- **BOMB-of-1 vs single tombstone for D-15 `--replace`:** ROADMAP doesn't prescribe. Planner decides based on code-duplication math — BOMB-of-1 uses the new BOMB path (one code path for all deletion), a single tombstone reuses existing TOMBSTONE_MAGIC. Leaning BOMB-of-1 for uniformity.
- **BOMB magic byte choice:** Use `0x42 0x4F 0x4D 0x42` ("BOMB") for mnemonic parity with TOMBSTONE's "DEADBEEF" aesthetic. Planner may pick otherwise if it collides with something.
- **Signing form unchanged:** NAME and BOMB blobs sign with the Phase 122 canonical form `signing_input = SHA3(target_namespace || data || ttl || timestamp)`. `data` is the NAME or BOMB payload. No new sponge variants.

</specifics>

<deferred>
## Deferred Ideas

- **Local name cache** (`~/.chromatindb/name_cache.json` per ROADMAP text) — could return as an opt-in latency optimization in a future phase once `ListByMagic` perf is measured. Not in Phase 123 scope.
- **Server-side NAME index DBI** for O(1) resolution — explicit "future 999.x phase" per ROADMAP. Defer.
- **Long-lived batching daemon for `cdb rm`** — rejected for YAGNI. If cross-invocation batching ever becomes needed, it's its own phase.
- **Delegation-gated BOMB** (delegates can BOMB only their own blobs) — rejected for complexity. Revisit if a concrete use case shows up.
- **Explicit `seq:8` field in NAME payload** — unnecessary given D-01 (`blob.timestamp` serves as seq). Could be added later if a protocol evolution requires a seq independent of wall-clock time.
- **Node verification that BOMB targets exist** — rejected as "wrong and expensive" (D-14). BOMBs legitimately pre-mark not-yet-received blobs.
- **Multi-BOMB splitting within one `cdb rm` invocation** — YAGNI. `cdb rm <500 targets>` emits one BOMB (D-07).

</deferred>

---

*Phase: 123-tombstone-batching-and-name-tagged-overwrite*
*Context gathered: 2026-04-20*
