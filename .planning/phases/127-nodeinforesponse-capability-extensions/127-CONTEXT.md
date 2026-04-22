# Phase 127: NodeInfoResponse Capability Extensions — Context

**Gathered:** 2026-04-22
**Status:** Ready for planning

## Phase Boundary

Extend the `NodeInfoResponse` wire format with 4 new capability fields so a freshly-connected `cdb` or peer can discover the node's blob cap, frame cap, rate limit, and subscription cap in a single round-trip. Pre-MVP, protocol-breaking, no compat shim.

Phase 127 ships the **wire format extension only**. It populates the 4 fields from existing constants/config values as they are today. Subsequent phases consume them:

- Phase 128 makes `max_blob_data_bytes` track `config.blob_max_bytes` (default 4 MiB, SIGHUP-reloadable) and shrinks `max_frame_bytes` to 2 MiB.
- Phase 129 snapshots `max_blob_data_bytes` into peer `PeerInfo` for sync-cap filtering.
- Phase 130 derives CLI chunk sizing from `max_blob_data_bytes`.

Requirements covered: NODEINFO-01, NODEINFO-02, NODEINFO-03, NODEINFO-04, VERI-02.

## Implementation Decisions

### Wire field layout

- **D-01:** The 4 new fixed-size fields are inserted **BEFORE** `[types_count:1][supported_types:N]`, keeping all fixed fields contiguous and the single variable-length section last. Protocol-breaking change is free here since the whole v4.2.0 wire format is breaking anyway.

- **D-02:** Intra-group field order follows the REQUIREMENTS.md enumeration order (NODEINFO-01..04):
  1. `max_blob_data_bytes` (u64 BE, 8 bytes)
  2. `max_frame_bytes` (u32 BE, 4 bytes)
  3. `rate_limit_bytes_per_sec` (u64 BE, 8 bytes) — **see D-03**
  4. `max_subscriptions_per_connection` (u32 BE, 4 bytes)

  Total added: 24 bytes before the existing `[types_count:1][supported_types:N]` tail.

- **Final wire layout after Phase 127:**
  ```
  [version_len:1][version:version_len bytes]
  [uptime:8 BE uint64]
  [peer_count:4 BE uint32]
  [namespace_count:4 BE uint32]
  [total_blobs:8 BE uint64]
  [storage_used:8 BE uint64]
  [storage_max:8 BE uint64]
  [max_blob_data_bytes:8 BE uint64]         ← NEW (NODEINFO-01)
  [max_frame_bytes:4 BE uint32]              ← NEW (NODEINFO-02)
  [rate_limit_bytes_per_sec:8 BE uint64]     ← NEW (NODEINFO-03, see D-03)
  [max_subscriptions_per_connection:4 BE uint32] ← NEW (NODEINFO-04)
  [types_count:1 uint8]
  [supported_types:types_count bytes]
  ```

### Rate-limit field name + type (REQ deviation)

- **D-03:** The wire field specified in REQUIREMENTS.md NODEINFO-03 as `rate_limit_messages_per_second` (u32 BE) is **renamed and re-typed** to `rate_limit_bytes_per_sec` (u64 BE) to match the actual enforced config field at `db/config/config.h:26`. Rationale:
  - `config.rate_limit_bytes_per_sec` is the value the node actually enforces (via `MessageDispatcher::set_rate_limits` and `SyncOrchestrator::set_rate_limit`). The wire field should expose what's actually in effect, not a ghost metric.
  - No code path tracks messages/sec anywhere. Exposing a `messages_per_second` wire field would either be a stub (always 0) or a semantic lie (bytes-as-messages).
  - u32 bytes/sec tops out at ~4 GiB/s which is borderline today; u64 future-proofs without cost.
  - Phase 128's `chromatindb_config_*` Prometheus gauge will reuse the same name `chromatindb_config_rate_limit_bytes_per_sec` — wire/metrics/config symmetry.
- **Action:** REQUIREMENTS.md NODEINFO-03 line will be updated in Phase 127's docs-update step to reflect the new name and type. This is the only REQ text change in Phase 127.

### Field value sources (Phase 127)

- **D-04:** Each of the 4 new fields is populated from the current source of truth **as it exists today**. Phase 127 does not introduce the Phase 128 config knob. Specifically:
  | Wire field | Source in Phase 127 | Source after Phase 128 |
  |---|---|---|
  | `max_blob_data_bytes` | `db/net/framing.h::MAX_BLOB_DATA_SIZE` (500 MiB constant) | `config.blob_max_bytes` |
  | `max_frame_bytes` | `db/net/framing.h::MAX_FRAME_SIZE` (110 MiB constant) | `MAX_FRAME_SIZE` (2 MiB, post-shrink) |
  | `rate_limit_bytes_per_sec` | `config.rate_limit_bytes_per_sec` (already in config) | unchanged |
  | `max_subscriptions_per_connection` | `config.max_subscriptions_per_connection` (already in config) | unchanged |
- **D-05:** The dispatcher encoder accesses `MAX_BLOB_DATA_SIZE` and `MAX_FRAME_SIZE` directly from `db/net/framing.h`. No new constructor parameter, no config wiring changes. Phase 128's FRAME-01/BLOB-01 will change the source when it lands — the encoder delta is one line at that point.

### CLI rendering (`cdb info`)

- **D-06:** `cdb info` displays **all 4 new caps** in human-readable form, appended after the existing `Quota:` line. Humanized byte sizes use the existing `humanize_bytes` helper. Subscriptions stays as an integer.

  ```
  Version:    2.3.0
  Uptime:     3h 12m
  Peers:      4
  Namespaces: 12
  Blobs:      8421
  Used:       142 MiB
  Quota:      unlimited
  Max blob:   500 MiB                 ← NEW
  Max frame:  110 MiB                 ← NEW
  Rate limit: unlimited               ← NEW (0 → "unlimited")
  Max subs:   256                     ← NEW (0 → "unlimited")
  ```
- **D-07:** Zero-value handling: `rate_limit_bytes_per_sec == 0` prints `unlimited`; `max_subscriptions_per_connection == 0` prints `unlimited`. `max_blob_data_bytes` and `max_frame_bytes` cannot be 0 (always non-zero in practice) — print humanized bytes.
- **D-08:** CLI auto-tune scaffolding is **not** in Phase 127. The `info` command decodes all 4 fields, but no caching into `ConnectOpts`/`Session` state happens here — Phase 130 (CLI-01) adds the session cache. Phase 127 just proves the decoder reads the fields correctly.

### Test design

- **D-09:** VERI-02 is satisfied by **one integration TEST_CASE** extending the existing `NodeInfoRequest returns version and node state` fixture at `db/tests/peer/test_peer_manager.cpp:2773`. The test fires a real `NodeInfoRequest` through the real dispatcher, captures the `NodeInfoResponse` off the wire, and asserts every new field decodes to the configured value. Boundary coverage follows:
  - Default Config values: `MAX_BLOB_DATA_SIZE` = 500 MiB, `MAX_FRAME_SIZE` = 110 MiB, `rate_limit_bytes_per_sec` = 0, `max_subscriptions_per_connection` = 256.
  - Zero-value boundary: set `config.rate_limit_bytes_per_sec = 0` and `config.max_subscriptions_per_connection = 0`, assert they encode and decode as 0.
  - Max-value boundary: set `config.rate_limit_bytes_per_sec = UINT64_MAX` and `config.max_subscriptions_per_connection = UINT32_MAX`, assert they round-trip byte-exact.
- **D-10:** The existing TEST_CASE at line 2773 also gets a wire-size assertion: after the 4 new fields land, the response payload grew by exactly 24 bytes in the fixed section. Hard-coded byte-count check ensures the field positions don't drift.
- **D-11:** No separate CLI-side decoder unit test. The CLI decoder is exercised transitively — Phase 127 does not add a Catch2-style CLI harness. If the decoder regression bites in a later phase, a focused CLI test can be added then. This honors `feedback_delegate_tests_to_user.md` (orchestrator doesn't spin heavy test harnesses for trivial code paths).

### Passing fixes

- **D-12:** The stale CLI comment block at `cli/src/commands.cpp:2233-2236` references `[git_hash_len:1][git_hash_str]` fields that were never encoded on the node side and never read by the decoder (pre-existing dead comment). Since Phase 127 rewrites this comment block to document the new fields, the dead git_hash reference is removed in passing. No functional change — just comment accuracy.
- **D-13:** `db/PROTOCOL.md` §NodeInfoResponse wire table (lines 1085-1106) is **NOT updated in Phase 127**. DOCS-03 in Phase 131 is the canonical home for the full PROTOCOL.md wire-format rewrite. Phase 127 plans leave PROTOCOL.md alone to avoid duplicate doc churn.

### Dispatcher supported[] array

- **D-14:** The `supported[]` array at `db/peer/message_dispatcher.cpp:678-687` lists `TransportMsgType` enum values. Phase 127 adds no new message types — just extends an existing response payload. `supported[]` remains unchanged.

### Live-node coordination (pre-MVP)

- **D-15:** Phase 127 is protocol-breaking. After Phase 127 ships on both `db/` and `cli/`, a v4.1.x `cdb` talking to a v4.2.0 node (or vice versa) will fail at `NodeInfoResponse` decode — the old decoder stops at the `types_count` byte position which is now an offset inside `max_blob_data_bytes`. This is expected per REQUIREMENTS.md line 94 ("No compat shim") and project memory `feedback_no_backward_compat.md`.
- **D-16:** The live node at `192.168.1.73` stays on its current version until Phase 128+ lands together. Testing Phase 127 against the live node is **expected to fail** on `cdb info` until the node is redeployed. Phase 127 verification uses only local test harnesses (Catch2 integration in `test_peer_manager.cpp` per D-09). Live-node roundtrip verification is VERI-06 in Phase 130 (CLI auto-tune).

### Claude's Discretion

- Exact phrasing of the new `cdb info` output lines (keep concise, align columns).
- Whether to add a test helper (e.g., `make_test_config_for_nodeinfo()`) or inline the boundary values in each TEST_CASE.
- Whether the new wire fields use existing `chromatindb::util::store_u32_be` / `store_u64_be` helpers — they should (consistency), no discretion really.
- Naming of the new TEST_CASE tags (expect `[peer][nodeinfo]` to remain the anchor tag, matching the existing test at line 2773).

## Specific Ideas

No concrete references — this is a mechanical wire-format extension. The user's early question about "what is this phase actually adding vs existing config fields" was answered by the codebase scout that surfaced `config.rate_limit_bytes_per_sec` and `config.max_subscriptions_per_connection` already in place (the decision in D-03 flows from that finding).

## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Wire protocol (node side — db/)
- `db/peer/message_dispatcher.cpp:661-733` — current `NodeInfoRequest` handler and `NodeInfoResponse` encoder
- `db/wire/transport_generated.h:68-69` — `TransportMsgType_NodeInfoRequest`=39, `TransportMsgType_NodeInfoResponse`=40
- `db/net/framing.h:14-15` — `MAX_FRAME_SIZE = 110 * 1024 * 1024` (source for `max_frame_bytes` in D-04)
- `db/net/framing.h:18` — `MAX_BLOB_DATA_SIZE = 500ULL * 1024 * 1024` (source for `max_blob_data_bytes` in D-04)
- `db/config/config.h:23` — `max_subscriptions_per_connection` (source for NODEINFO-04)
- `db/config/config.h:26` — `rate_limit_bytes_per_sec` (source for NODEINFO-03 per D-03)
- `db/util/endian.h` — `store_u32_be`, `store_u64_be` used by the encoder
- `db/PROTOCOL.md:1085-1106` — existing `NodeInfoResponse` wire-format table (DO NOT edit per D-13; Phase 131 owns PROTOCOL.md changes)

### Wire protocol (CLI side — cli/)
- `cli/src/wire.h:87-88` — `MsgType::NodeInfoRequest`=39, `MsgType::NodeInfoResponse`=40 (must stay in sync with db enum)
- `cli/src/commands.cpp:2188-2287` — `info` command: sends `NodeInfoRequest`, decodes `NodeInfoResponse`, renders to stdout
- `cli/src/commands.cpp:2233-2236` — stale `git_hash` comment block (fix in passing per D-12)
- `cli/src/util.h` — `humanize_bytes` / `humanize_uptime` helpers for `cdb info` rendering

### Tests
- `db/tests/peer/test_peer_manager.cpp:2773-2908` — existing `NodeInfoRequest returns version and node state` TEST_CASE (extend per D-09)

### v4.2.0 scope
- `.planning/REQUIREMENTS.md` NODEINFO-01..04, VERI-02 — requirement text this phase delivers
- `.planning/REQUIREMENTS.md` NODEINFO-03 — will be updated in passing to reflect D-03 (name and type change)
- `.planning/ROADMAP.md` Phase 127 block — goal, depends-on, success criteria
- `.planning/phases/126-pre-shrink-audit/126-CONTEXT.md` — streaming invariant context; this phase does not touch the invariant but is gated by it

### Project memory
- `feedback_no_backward_compat.md` — "No backward compat on either binary; protocol changes are fine" → justifies D-15
- `feedback_no_phase_leaks_in_user_strings.md` → `cdb info` output strings in D-06 must NOT contain phase tokens, REQ-IDs, or version numbers
- `feedback_no_duplicate_code.md` → encoder uses existing `store_u64_be` / `store_u32_be` helpers, no inline byte-packing

## Existing Code Insights

### Reusable Assets
- `chromatindb::util::store_u64_be` / `store_u32_be` — already used by the existing encoder. The 4 new fields use the same helpers.
- `MessageDispatcher::config_max_storage_bytes_` lambda pattern — the dispatcher already captures config getters as lambdas. The 2 new config-sourced fields (`rate_limit_bytes_per_sec`, `max_subscriptions_per_connection`) will be injected the same way, avoiding a tight coupling between the dispatcher and the `Config` struct.
- `humanize_bytes` / `humanize_uptime` — existing CLI helpers. `cdb info` new lines use them for blob/frame/rate-limit (all byte-quantified) and a plain integer for subscriptions.
- Existing `NodeInfoRequest returns version and node state` TEST_CASE fixture at `test_peer_manager.cpp:2773` — captures `NodeInfoResponse` off the wire and does byte-offset decode. Extending is strictly additive.

### Established Patterns
- All wire encoding is big-endian via `store_*_be` helpers (no host-byte ordering anywhere). The 4 new fields follow suit.
- All fixed-size numeric fields in existing responses are either u32 BE (4B) or u64 BE (8B). The new fields match this precedent.
- Variable-length fields (strings, arrays) always come LAST, preceded by an explicit length byte or uint. Preserving this in D-01 means future additions can also slot in before `types_count`.
- Dispatcher handlers use `co_spawn` + `co_await send_message` for async response delivery. Encoder changes stay inside the existing try/catch ERROR_INTERNAL wrapper.

### Integration Points
- **Node encoder** — `db/peer/message_dispatcher.cpp:661-733`. Need to:
  1. Pass or capture `max_subscriptions_per_connection` and `rate_limit_bytes_per_sec` from `Config` (the dispatcher already has `config_max_storage_bytes_` lambda — add two more).
  2. Extend the `resp_size` calculation: `+8 + 4 + 8 + 4 = +24 bytes`.
  3. Write the 4 new fields between `storage_max` and `types_count` using existing `store_*_be` helpers.
- **CLI decoder** — `cli/src/commands.cpp:2233-2287`. Need to:
  1. Rewrite the layout comment block (fix D-12 in passing).
  2. Add 4 new reads (`read_u64`, `read_u32`, `read_u64`, `read_u32`) between `storage_max` and `types_count`.
  3. Add 4 new `std::printf` lines after `Quota:` per D-06 + D-07.
- **Tests** — `db/tests/peer/test_peer_manager.cpp:2773`. Extend the existing fixture per D-09 + D-10.

### Known Pitfalls (from Accumulated Context)
- **Wire/enum drift:** `cli/src/wire.h` MsgType enum must match `db/wire/transport_generated.h` TransportMsgType enum. Not touched in this phase but verify during planner's read_first — any drift = silent protocol break.
- **Encoder size miscalculation:** the resp_size computation at `db/peer/message_dispatcher.cpp:690-692` is hand-rolled. A `+24` miscalc silently buffer-overflows `response.data()`. Plan must include an explicit assertion or test that catches this.
- **CLI: supported_types section is variable-length.** After D-01's reordering, the 4 new fields land BEFORE types_count. The existing CLI decoder reads the supported_types loop LAST so no change there — just don't accidentally reorder the reads.

## Out of Scope

- Making `max_blob_data_bytes` track `config.blob_max_bytes` — that's Phase 128 (BLOB-01). Phase 127 reads the `MAX_BLOB_DATA_SIZE` constant.
- Shrinking `MAX_FRAME_SIZE` from 110 MiB to 2 MiB — that's Phase 128 (FRAME-01). Phase 127 reports the current 110 MiB value.
- `chromatindb_config_*` Prometheus gauges — that's Phase 128 (METRICS-01..02).
- Peer `PeerInfo.advertised_blob_cap` snapshotting at handshake — that's Phase 129 (SYNC-01).
- CLI session cache of `max_blob_data_bytes` / auto-tune wiring — that's Phase 130 (CLI-01..05).
- PROTOCOL.md rewrites — Phase 131 (DOCS-03). Phase 127 leaves PROTOCOL.md alone per D-13.
- Adding a separate pre-handshake NodeInfoRequest path (i.e., capability discovery before PQ handshake completes). Out of MVP scope; clients do a full handshake, then send `NodeInfoRequest`.
- Compat shim for pre-v4.2.0 wire format. Explicit REQ line 94 and `feedback_no_backward_compat.md`.

## Deferred Ideas

- `rate_limit_messages_per_second` as a separate field: if operators later want a per-message rate metric distinct from bytes/sec, it would be a follow-on wire extension (not this phase; not MVP).
- CLI pre-handshake capability ping: useful for cheap capability checks, but requires a new pre-auth message type. Document in BACKLOG.md post-MVP.
- PROTOCOL.md wire-table visual ASCII diagram: the existing table is fine; Phase 131 can decide whether to add a visual byte map.

## Next Step

`/gsd-plan-phase 127` — produce PLAN.md covering: (1) dispatcher encoder extension, (2) CLI decoder extension + `cdb info` rendering, (3) REQ text update for NODEINFO-03, (4) extension of the existing NodeInfoRequest TEST_CASE with the 3 field-value scenarios (default, zero boundary, max boundary) + wire-size assertion.
