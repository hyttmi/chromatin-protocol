---
phase: 127-nodeinforesponse-capability-extensions
plan: 01-encoder
subsystem: wire-protocol
tags: [nodeinfo, wire-format, capability-advertisement, big-endian, framing]

# Dependency graph
requires:
  - phase: 126-pre-shrink-audit
    provides: "proof that non-chunked single-frame responses fit in the shrunken frame budget with headroom; gates wire-format extensions that enlarge fixed sections"
provides:
  - "NodeInfoResponse wire layout extended by +24 bytes in the fixed section (u64 blob, u32 frame, u64 rate, u32 subs) inserted BETWEEN storage_max and types_count"
  - "Node-side encoder half of the v4.2.0 capability-discovery contract — CLI decode (127-03) and Catch2 byte-level assertions (127-04) build on this"
affects:
  - 127-02-requirements-doc
  - 127-03-cli-decode
  - 127-04-encoder-tests
  - 128-blob-cap-config-frame-shrink
  - 129-sync-cap-divergence
  - 130-cli-auto-tuning
  - 131-documentation-reconciliation

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Fixed-field capability advertisement — new caps appended to the fixed section of an existing response, leaving the single variable-length tail (supported_types[]) last. Preserves single-pass decoding."
    - "Source-of-truth rule (D-04/D-05): advertised caps read directly from the compiled-in framing.h constant, no config indirection, no new constructor param, no new lambda. Phase 128 will move blob cap to config — this plan anchors the field-shape contract first."

key-files:
  created: []
  modified:
    - "db/peer/message_dispatcher.cpp - +1 include (db/net/framing.h), +20/-2 lines in the TransportMsgType_NodeInfoRequest handler: resp_size row + 4 new store_*_be calls writing (blob u64, frame u32, rate u64, subs u32) between storage_max and types_count"

key-decisions:
  - "Insertion point is BEFORE types_count/supported_types tail (D-01) — keeps the single variable-length section last; decoders stay single-pass."
  - "Intra-group order blob(u64) -> frame(u32) -> rate(u64) -> subs(u32) (D-02) — +24 bytes total, alignment-friendly (u64-u32-u64-u32)."
  - "Rate field is rate_limit_bytes_per_sec (u64 BE) per D-03, overriding REQUIREMENTS.md NODEINFO-03's original u32 messages/sec draft. The REQ-text rewrite is Plan 127-02's job."
  - "Caps sourced from framing.h, NOT config (D-04/D-05). Phase 128 BLOB-01/FRAME-01 will move the source to config; this plan intentionally avoids the migration so the wire-layout contract lands atomically first."
  - "supported[] array untouched per D-14 — no new message types in Phase 127."
  - "PROTOCOL.md untouched per D-13 — Phase 131 DOCS-03 owns the protocol doc refresh; doc drift is acceptable within Phase 127."

patterns-established:
  - "Big-endian fixed-width writes via chromatindb::util::store_u64_be / store_u32_be in pre-sized std::vector<uint8_t>. No inline bit-shifting (feedback_no_duplicate_code.md)."
  - "Capability field comments in source code are allowed to cite requirement IDs (NODEINFO-01..04). These are internal source comments, NOT user-facing strings — feedback_no_phase_leaks_in_user_strings.md governs stdout/help/errors only."

requirements-completed: [NODEINFO-01, NODEINFO-02, NODEINFO-03, NODEINFO-04]

# Metrics
duration: 6min
completed: 2026-04-22
---

# Phase 127 Plan 01: NodeInfoResponse Capability Encoder Summary

**NodeInfoResponse encoder extended by +24 bytes: max_blob_data_bytes (u64 BE) + max_frame_bytes (u32 BE) + rate_limit_bytes_per_sec (u64 BE) + max_subscriptions_per_connection (u32 BE), inserted BETWEEN storage_max and the types_count/supported_types tail.**

## Performance

- **Duration:** ~6 min (edit + build) — CMake configuration took an additional ~3 min one-time in a fresh worktree
- **Started:** 2026-04-22T11:29:38Z
- **Completed:** 2026-04-22T11:35:00Z
- **Tasks:** 1 / 1
- **Files modified:** 1

## Accomplishments

- `db/peer/message_dispatcher.cpp` TransportMsgType_NodeInfoRequest handler now writes the 4 new capability fields in the documented order (blob u64, frame u32, rate u64, subs u32) between `storage_max` and `types_count`.
- `resp_size` calculation extended by the `+ 8 + 4 + 8 + 4` group with inline comments citing NODEINFO-01..04.
- `#include "db/net/framing.h"` added exactly once (line 12), inserted between `db/logging/logging.h` and `db/storage/storage.h` to preserve ascending path order.
- `chromatindb` target builds clean with `cmake --build build-debug -j$(nproc) --target chromatindb` (exit 0). Links to the final `chromatindb` executable.
- No touch to: `db/peer/message_dispatcher.h` (header signature unchanged), `db/peer/peer_manager.cpp` (no wiring change), `db/net/framing.h` (constants unchanged — Phase 128 moves them), `db/PROTOCOL.md` (Phase 131 owns the refresh), nor any test file (Plan 127-04 owns test extension).

## Task Commits

1. **Task 1: Extend NodeInfoResponse encoder with 4 new capability fields** — `d418b667` (feat)

## Files Created/Modified

- `db/peer/message_dispatcher.cpp` — +20 / -2 lines. Two semantic edits: (1) `#include "db/net/framing.h"` added between line 11 and line 13; (2) inside the TransportMsgType_NodeInfoRequest handler (lines 661-747 post-plan), the `resp_size` row grew by 24 bytes (`+ 8 + 4 + 8 + 4`) and 4 new `store_*_be` calls were inserted between the existing `storage_max` write (line 717-718) and the `types_count` write (line 736).

## Decisions Made

None beyond the plan's codified decisions (D-01 insertion position, D-02 field order, D-03 rate-field type, D-04/D-05 source-of-truth rule, D-13 PROTOCOL.md untouched, D-14 supported[] untouched). Plan executed verbatim.

## Deviations from Plan

None — plan executed exactly as written. All 9 acceptance-criteria grep/file checks passed:

- AC1 include count == 1 — PASS
- AC2a-d each of the 4 new `store_*_be` calls appears exactly once — PASS
- AC3 `+ 8 + 4 + 8 + 4` row present — PASS
- AC4 `db/peer/message_dispatcher.h` unmodified — PASS (empty `git diff --stat`)
- AC5 no NEW inline bit-packing (two pre-existing hits at `db/peer/message_dispatcher.cpp:992` MetadataResponse and `:1277` PeerInfoResponse — both pre-Phase-127 code paths, unchanged by this plan)
- AC6 `db/PROTOCOL.md` unmodified — PASS (empty `git diff --stat`)
- AC7 `supported[]` array unchanged (`5, 6, 7, 8,` marker present exactly once) — PASS
- AC8 `cmake --build build-debug -j$(nproc) --target chromatindb` exit 0 — PASS
- AC9 field write order inside NodeInfoResponse handler: `MAX_BLOB_DATA_SIZE` at line 721 < `MAX_FRAME_SIZE` at line 725 < `rate_limit_bytes_per_sec_` at line 729 < `max_subscriptions_` at line 733 — PASS (strictly increasing). Note: earlier file-wide occurrences of `rate_limit_bytes_per_sec_` (lines 131, 150, 153, 157 in `set_rate_limits` / `check_rate_limit`) and `max_subscriptions_` (lines 257, 258, 262 in SubscribeRequest handler) are PRE-EXISTING references outside the NodeInfoResponse handler — not relevant to the D-02 order check.

## Issues Encountered

None. The worktree had no pre-existing `build-debug` directory, so `cmake -S . -B build-debug -DCMAKE_BUILD_TYPE=Debug` was run once to configure (one-time cost). Subsequent `cmake --build` was clean. No deltas, no warnings specific to the edited lines.

## Next Phase Readiness

- **Plan 127-02** (requirements-doc, Wave 1 parallel with this plan) can rewrite REQUIREMENTS.md NODEINFO-03 text to match the D-03 u64-bytes-per-sec shape that this plan actually ships.
- **Plan 127-03** (CLI decode, Wave 2) has a stable node-side layout to decode against. The exact post-`storage_max` byte layout is: `[blob:8][frame:4][rate:8][subs:4]` in that order.
- **Plan 127-04** (Catch2 tests, Wave 2) has a stable encoder to assert byte positions and total `+24` payload delta against. Plan 127-04 owns VERI-02 (wire-size and field-order assertions on the [peer][nodeinfo] fixture).
- **Phase 128** (blob cap config + frame shrink) can change the source-of-truth for the first two fields from `framing.h` constants to config accessors WITHOUT touching the wire shape — Plan 127-01 isolates that migration from the wire-layout decision.
- No blockers for downstream plans. Protocol-breaking change acknowledged per REQUIREMENTS.md line 94 — pre-v4.2.0 clients will fail to decode the extended response, which is the expected pre-MVP posture (feedback_no_backward_compat.md).

## Self-Check: PASSED

- File `db/peer/message_dispatcher.cpp` — FOUND (modified, verified by `git diff --stat` and direct read of edited regions lines 1-25 and 685-744).
- Commit `d418b667` — FOUND (`git log --oneline | grep d418b667` returns the commit).
- Acceptance criteria AC1-AC9 — all pass (see Deviations section above for per-criterion status).
- Build acceptance `cmake --build build-debug -j$(nproc) --target chromatindb` — exit 0, `chromatindb` executable linked.

---
*Phase: 127-nodeinforesponse-capability-extensions*
*Plan: 127-01-encoder*
*Completed: 2026-04-22*
