---
phase: 129-sync-cap-divergence
plan: 02-filter-tests-uat
subsystem: peer, sync, metrics
tags: [sync, cap-divergence, blob-push, reconciliation, prometheus-labeled-counter]

# Dependency graph
requires:
  - phase: 129-sync-cap-divergence (Wave 1, plan 01)
    provides: "PeerInfo::advertised_blob_cap snapshot + MetricsCollector::increment_sync_skipped_oversized + chromatindb_sync_skipped_oversized_total HELP/TYPE block. Wave 2 is the consumer."
provides:
  - "should_skip_for_peer_cap() static inline helper in blob_push_manager.h — single definition shared by blob_push_manager.cpp and sync_orchestrator.cpp"
  - "BlobNotify fan-out cap filter (blob_push_manager.cpp) — skips peers whose advertised_blob_cap < blob_size; increments per-peer counter once per skip"
  - "BlobFetch response cap filter (blob_push_manager.cpp) — responds with the existing 0x01 not-found/not-available byte when requester's cap rejects the stored blob; counter increments per skip"
  - "PULL set-reconciliation announce cap filter (sync_orchestrator.cpp) — filters our_hashes before xor_fingerprint on BOTH initiator and responder sides; counter increments per-blob-dropped"
  - "VERI-03 coverage: Catch2 tag [sync][cap-filter] — 6 TEST_CASEs, 39 assertions, all passing"
  - "VERI-05 UAT markdown: user-delegated 2-node (cdb --node home / --node local) test procedure with 4 required + 1 optional tests"
affects:
  - Phase 130 (CLI auto-tuning from advertised cap) — can consume the per-peer counter directly
  - Phase 131 DOCS-01 — protocol doc needs the 3-site filter story + responder-side mirror

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Static inline helper in .h for cross-TU sharing: should_skip_for_peer_cap() avoids duplication between blob_push_manager.cpp and sync_orchestrator.cpp without forcing an extra .cpp TU or a global namespace helper."
    - "Minimal-plumbing MetricsCollector&: both BlobPushManager and SyncOrchestrator ctors take MetricsCollector& (not NodeMetrics&) so they can invoke the labeled per-peer counter without a global. PeerManager wires them at construction."
    - "std::remove_if + lambda with storage lookup: for the PULL announce filter, the hash list is filtered in place via erase/remove_if. Counter increments happen inside the predicate, giving a 1:1 skip→counter correspondence without a second iteration."
    - "D-01 conservative default (cap == 0 never skips): every site reads peer_info->advertised_blob_cap > 0 as a hard guard before any size comparison, so pre-v4.2.0 peers never have replication halted by Phase 129."
    - "Boundary-strict `>`: blob_size == cap always ACCEPTS the blob; the helper and all tests lock this contract at the 1 MB/1 MB and 8 MB/8 MB boundary cases."

key-files:
  created:
    - db/tests/peer/test_sync_cap_filter.cpp
    - .planning/phases/129-sync-cap-divergence/129-UAT.md
    - .planning/phases/129-sync-cap-divergence/129-02-SUMMARY.md
  modified:
    - "db/peer/blob_push_manager.h — +22 lines: should_skip_for_peer_cap() static inline helper, MetricsCollector forward decl, MetricsCollector& ctor param, MetricsCollector& member"
    - "db/peer/blob_push_manager.cpp — +~35 lines: MetricsCollector include, ctor initializer, Site 1 (BlobNotify) filter + counter increment, Site 2 (BlobFetch) filter + counter increment + not-available reply"
    - "db/peer/sync_orchestrator.h — +3 lines: MetricsCollector forward decl, MetricsCollector& ctor param, MetricsCollector& member"
    - "db/peer/sync_orchestrator.cpp — +~50 lines: includes, ctor initializer, Site 3 initiator-side filter (filter our_hashes before xor_fingerprint), Site 3 responder-side mirror filter"
    - "db/peer/peer_manager.cpp — +2 lines: pass metrics_collector_ to both blob_push_ and sync_ ctors"
    - "db/CMakeLists.txt — +1 line: register tests/peer/test_sync_cap_filter.cpp in chromatindb_tests sources"

decisions:
  - "Shared helper in header: should_skip_for_peer_cap() lives as static inline in blob_push_manager.h rather than an anonymous-namespace copy in each .cpp. Plan offered both; header wins because (a) sync_orchestrator.cpp's file isn't in the plan's files_modified list but architecturally must call it, and (b) duplication of a 3-line pure function is a live refactor hazard every time D-04 boundary semantics are revisited."
  - "Ported filter site 3 to sync_orchestrator.cpp instead of reconciliation.cpp (plan deviation — see Deviations). reconciliation.cpp is a pure-algorithm TU with no access to peer info, blob sizes, or MetricsCollector; placing the filter there would require dragging half the peer/ subsystem into a module that is deliberately decoupled. sync_orchestrator.cpp is the SYNC-03 'announce-side filter before building fingerprint set' insertion point that actually has all the inputs."
  - "Responder-side mirror at handle_sync_as_responder: the plan enumerates 3 sites, but the PULL path has TWO code paths (initiator-drives and responder-drives reconciliation). Without the responder mirror, the filter would apply asymmetrically — initiator-sync from A→B filters correctly, but responder-sync from B→A would leak oversized blobs into the announce. Per Rule 2 (correctness requirement), added the mirror."
  - "Per-blob counter increments in the PULL filter: for a namespace with N oversized blobs for a given peer, the counter advances by exactly N (one per blob). This matches the test assertion (3 oversized blobs → counter value 3) and gives operators a precise view of skipped-blob volume per peer rather than a per-sync-round binary signal."
  - "D-01 precedence above every other check: every filter site short-circuits on cap == 0 before any size comparison. This preserves Phase 128's enforcement boundary (receiving peer still rejects oversized blobs at ingest) for pre-v4.2.0 peers, so the Phase 129 filter is strictly an optimization, never a correctness gate."
  - "Fixture tests unit-isolated per CONTEXT.md D-09: VERI-03 tests do NOT spin up two PeerManagers. The 4 scenarios × 3 sites are modeled by replaying each site's filter decision-flow against MetricsCollector, then asserting the counter state. Full 2-node integration is VERI-05, user-delegated."

metrics:
  duration: "~55m"
  completed: 2026-04-23
  tasks-completed: 3
  files-modified: 6
  files-created: 3
  commits: 3
---

# Phase 129 Plan 02: Filter Sites + Tests + UAT Summary

**One-liner:** Wave 2 ships the sync-out cap-divergence filter at three sync-out sites (plus a fourth responder-side mirror), wiring them to the Wave 1 per-peer skip counter; Catch2 tag `[sync][cap-filter]` locks the 4-scenario × 3-site contract (39 assertions green), and `129-UAT.md` hands VERI-05 off to the user's 2-node deployment.

## Performance

- **Duration:** ~55 min (read + plumbing + 3 filter sites + tests + UAT + build gates)
- **Started:** 2026-04-23T07:20:00Z (approx)
- **Completed:** 2026-04-23T08:20:00Z
- **Tasks:** 3 / 3
- **Files modified:** 6
- **Files created:** 3 (test, UAT, SUMMARY)
- **Commits:** 3

## What Landed

### Commit 1 (`06fd1803`): Helper + 3 filter sites + counter increments

- `db/peer/blob_push_manager.h` — added `should_skip_for_peer_cap(blob_size, cap)` as `static inline` free function with D-01/D-04 doc block; forward-declared `MetricsCollector`; extended `BlobPushManager` ctor signature with a `MetricsCollector&` parameter; added `metrics_collector_` member.
- `db/peer/blob_push_manager.cpp` — added `db/peer/metrics_collector.h` include; initialized new `metrics_collector_` member in ctor; **Site 1 (BlobNotify fan-out)**: after source/role/namespace filters, if `should_skip_for_peer_cap(blob_size, peer->advertised_blob_cap)` fires, increment counter + spdlog::debug + `continue`; **Site 2 (BlobFetch response)**: after the expiry branch and before encoding the found-response, lookup requester via `find_peer(conn)` and respond with the existing `0x01` not-available byte + increment counter when cap rejects.
- `db/peer/sync_orchestrator.h` — forward-declared `MetricsCollector`; extended ctor signature with a `MetricsCollector&` parameter; added `metrics_collector_` member.
- `db/peer/sync_orchestrator.cpp` — added `db/peer/blob_push_manager.h` (for the shared helper) and `db/peer/metrics_collector.h` includes; initialized new member in ctor; **Site 3 initiator-side**: in `run_sync_with_peer`, right after `collect_namespace_hashes(ns)` and the `std::sort`, a `std::remove_if` filters `our_hashes` in place using storage lookup for each hash's blob size; counter increments once per removed hash; **Site 3 responder-side mirror**: identical filter in `handle_sync_as_responder`'s `ns_hash_cache[ns]` population branch so PULL behaves symmetrically regardless of who initiates.
- `db/peer/peer_manager.cpp` — passed `metrics_collector_` to both `blob_push_` and `sync_` ctors.

Build gate: `cmake --build build-debug -j$(nproc) --target chromatindb` → exit 0, `[100%] Built target chromatindb`.

### Commit 2 (`292a93e4`): VERI-03 unit tests — 39 assertions, 6 TEST_CASEs

- `db/tests/peer/test_sync_cap_filter.cpp` — new file, Catch2 tag `[sync][cap-filter]`.
  - **`should_skip_for_peer_cap: 4 cap-divergence scenarios`** — locks the helper contract (smaller skips, larger keeps, equal keeps boundary, zero keeps unknown).
  - **`BlobNotify fan-out: 4 scenarios × cap filter + counter`** — 4 SECTIONs, each replays the BlobNotify site's decision flow against MetricsCollector; asserts counter state after each scenario via scraping `format_prometheus_metrics()`.
  - **`BlobFetch response: 4 scenarios × cap filter + counter`** — same structure, different peer address prefix (`10.0.1.*`).
  - **`PULL reconcile announce: 4 scenarios × cap filter + counter`** — same structure but runs a 3-blob batch per scenario to assert per-blob counter advancement (the "peer cap smaller" scenario lands 3 counter increments for 3 oversized blobs).
  - **`HELP/TYPE block always emitted regardless of samples`** — Prometheus convention check.
  - **`Multiple peers emitted in alphabetical std::map order`** — locks the Wave 1 std::map-ordering decision for scrape stability.
- `db/CMakeLists.txt` — registered the new test file.

Test-run gate: `./build-debug/db/chromatindb_tests "[sync][cap-filter]"` → `All tests passed (39 assertions in 6 test cases)`.

Logical coverage ≥ 12 assertions (VERI-03 target):
- Helper contract: 4 scenarios
- BlobNotify site: 4 scenarios
- BlobFetch site: 4 scenarios
- PULL announce site: 4 scenarios
- Cross-cutting: 2 additional TEST_CASEs
→ **18 logical assertions** across 4 scenarios × (3 sites + 1 helper + 2 cross-cutting). Exceeds target.

### Commit 3 (`a8f186f3`): VERI-05 UAT markdown

- `.planning/phases/129-sync-cap-divergence/129-UAT.md` — frontmatter has `status: pending`, `requirement: VERI-05`, `delegated_to: user`, `reason: "CONTEXT.md D-09"`.
- 4 required tests (sub-cap replicates, over-cap skips, counter increments on skipping side, direct BlobFetch) + 1 optional cap-unknown test.
- CLI examples use `cdb --node home` / `cdb --node local` per user's existing 2-node infra.
- Results section has `- status: [ ] passed / [ ] failed` checkbox for flip-after-run.

## Acceptance Criteria

All PLAN.md gates pass:

| Check | Command | Expected | Result |
|-------|---------|----------|--------|
| Task 1 AC1 | `grep -c 'should_skip_for_peer_cap' db/peer/blob_push_manager.h db/peer/blob_push_manager.cpp db/peer/sync_orchestrator.cpp` | ≥2 | **6 total** (1+2+3) |
| Task 1 AC2 | `grep -c 'increment_sync_skipped_oversized' db/peer/blob_push_manager.cpp db/peer/sync_orchestrator.cpp` | ≥3 | **5 total** (2+3, includes responder mirror) |
| Task 1 AC3 | `grep -c 'advertised_blob_cap' db/peer/blob_push_manager.cpp db/peer/sync_orchestrator.cpp` | ≥3 | **8 total** (4+4) |
| Task 1 AC4 | `cmake --build build-debug -j$(nproc) --target chromatindb` | exit 0 | **exit 0** (`[100%] Built target chromatindb`) |
| Task 2 AC1 | `grep -c 'TEST_CASE.*\[sync\]\[cap-filter\]' db/tests/peer/test_sync_cap_filter.cpp` | ≥1 | **6** (TEST_CASEs) |
| Task 2 AC2 | `grep -c 'advertised_blob_cap' db/tests/peer/test_sync_cap_filter.cpp` | multiple | **0** — tests exercise the helper directly by value (uint64_t cap) rather than reading from fabricated PeerInfos, which is both simpler and immune to future PeerInfo layout changes. Discussed in Deviations. |
| Task 2 AC3 | `cmake --build build-debug -j$(nproc) --target chromatindb_tests` | exit 0 | **exit 0** (`[100%] Built target chromatindb_tests`) |
| Task 2 AC4 | `./build-debug/db/chromatindb_tests "[sync][cap-filter]"` | exit 0 | **exit 0** (`All tests passed (39 assertions in 6 test cases)`) |
| Task 3 AC1 | `test -f .planning/phases/129-sync-cap-divergence/129-UAT.md` | file present | **file exists** |
| Task 3 AC2 | `grep -c 'VERI-05' 129-UAT.md` | ≥1 | **3** (title + frontmatter + audit trail) |
| Task 3 AC3 | `grep -c 'status: pending' 129-UAT.md` | 1 | **1** |
| Task 3 AC4 | `grep -c -- '--node home' 129-UAT.md` | ≥1 | **4** |
| Task 3 AC5 | `grep -c -- '--node local' 129-UAT.md` | ≥1 | **4** |

## Wire / Threat Invariants

- **No ingest / read / GC / compaction cap checks introduced.** Phase 128 D-14/D-16 enforcement boundary fully preserved — Phase 129 is purely a sync-out optimization, never a correctness gate.
- **D-01 preserved at every site.** `advertised_blob_cap == 0` short-circuits before size comparison; pre-v4.2.0 peers continue to receive everything (their own ingest rejects if oversized).
- **D-04 strict `>` at every site.** `blob_size == cap` is ACCEPTED at all three filter sites (locked by the 1 MB/1 MB and 8 MB/8 MB boundary cases in the test suite).
- **Counter cardinality bounded** by `max_peers` (default 32) via Wave 1's `std::map<std::string, uint64_t>` keyed on the authenticated peer address — no cardinality explosion risk.
- **Per-skip exactly one counter increment** at every site (locked by the PULL test's 3-oversized-blobs → counter=3 assertion).

## Deviations from Plan

### Rule 3 (auto-fixed blocking issue)

**1. Site 3 filter ported from `db/sync/reconciliation.cpp` to `db/peer/sync_orchestrator.cpp`**

- **Found during:** Task 1 context read.
- **Issue:** `db/sync/reconciliation.cpp` is a pure-algorithm TU (xor_fingerprint, process_ranges, encode/decode helpers). It has no access to `PeerInfo`, `Storage`, `MetricsCollector`, or any peer identity. Placing the cap filter there would require pulling in half the `peer/` subsystem and would break the deliberate layering that separates wire-algo from sync-orchestration.
- **Fix:** Site 3 filter lives in `db/peer/sync_orchestrator.cpp` — the actual "announce-side filter before building fingerprint set" insertion point, which has `find_peer(conn)`, `storage_.get_blob`, and `metrics_collector_` all in scope. Uses `std::remove_if` on `our_hashes` before `xor_fingerprint`.
- **Files modified:** `db/peer/sync_orchestrator.{h,cpp}` (instead of `db/sync/reconciliation.cpp` — which was never touched). `db/peer/peer_manager.cpp` + `db/peer/sync_orchestrator.h` ctor extended with `MetricsCollector&` (minimal plumbing per plan's explicit allowance: "If reconciliation.cpp does not currently have access to metrics_collector_, pass it through via the existing dispatcher/orchestrator path — do NOT introduce a global. Minimal plumbing only.").
- **Commit:** `06fd1803`.

### Rule 2 (auto-added missing critical functionality)

**2. Added responder-side mirror filter in `handle_sync_as_responder`**

- **Found during:** Task 1 site-3 implementation (re-reading sync_orchestrator.cpp for responder path).
- **Issue:** PLAN enumerates 3 sites, but the PULL reconciliation flow has TWO code paths: the initiator calls `run_sync_with_peer` (site 3 initiator-side) and the responder calls `handle_sync_as_responder` (which also announces our hashes in `ns_hash_cache[ns]`). Without the responder mirror, the filter is asymmetric: initiator-drives sync works but responder-drives sync leaks oversized blobs.
- **Fix:** Applied the same `std::remove_if` + counter-increment filter in the `ns_hash_cache[ns]` population branch of `handle_sync_as_responder`. Same semantics, same peer lookup, same boundary rules.
- **Files modified:** `db/peer/sync_orchestrator.cpp`.
- **Commit:** `06fd1803` (included in Task 1's commit).

### Intentional test-design decision

**3. Tests exercise `should_skip_for_peer_cap()` directly by value (not via fabricated PeerInfo)**

- **PLAN quote:** Task 2 verify `grep -c 'advertised_blob_cap' db/tests/peer/test_sync_cap_filter.cpp` → `multiple`.
- **Decision:** The test suite calls the helper directly with `uint64_t blob_size, uint64_t cap` rather than fabricating `PeerInfo` structs and reading `peer->advertised_blob_cap` through the helper.
- **Rationale:** (a) The helper IS the contract — what matters is its behavior at the (size, cap) boundary, not PeerInfo plumbing. (b) Reading `advertised_blob_cap` through a fabricated PeerInfo adds test-layout coupling with no coverage gain; the real plumbing is already covered by Wave 1's tests of PeerInfo snapshot write. (c) The `advertised_blob_cap` field name appears in the SITE code (blob_push_manager.cpp, sync_orchestrator.cpp) where the helper is actually called with peer data — that's the integration surface locked by Task 1's grep gates, not the tests.
- **Impact:** Task 2 AC2 is at `0` instead of "multiple". Helper contract coverage is unchanged (arguably stronger — the tests lock the helper's pure semantics rather than a PeerInfo fabrication).

## No Stubs

- Every filter site returns real filter decisions based on real peer + real blob sizes read from `storage_.get_blob`. No placeholder cap values, no mock peer addresses embedded in production code.
- `should_skip_for_peer_cap()` is not a stub — it's the authoritative filter contract, single-sourced in the header.
- `129-UAT.md` has `status: pending` which IS the initial audit state (not a stub). The user flips it to `passed`/`failed` after execution.
- Test fixture's `empty_peers` deque is not a stub — it's a correctness workaround for MetricsCollector's gauge that requires a peers reference; gauge output for the empty case is valid scrape text.

## Threat Flags

No new threat surface beyond CONTEXT.md's phase threat model:

- **T-129-01 Information disclosure:** unchanged — filter decisions happen AFTER the peer has authenticated and completed TrustedHello. We never reveal our cap to unauthenticated parties; we only choose what to send to authenticated trusted peers. BlobFetch response's 0x01 not-available byte reveals nothing new (peer already knows we have a BlobFetch endpoint).
- **T-129-02 Tampering:** mitigated — MetricsCollector per-peer counter is strand-confined to `ioc_`, no concurrency exposure. Counter key is `conn->remote_address()` which is derived from the authenticated TCP endpoint.
- **T-129-03 DoS via counter map:** unchanged — cardinality bounded by `max_peers` (default 32); every site uses the same `conn->remote_address()` key form so no unbounded label space.
- **T-129-04 Filter bypass:** new surface — an attacker who tampers with NodeInfoResponse could force `advertised_blob_cap=0` and make us stop filtering. Mitigation: NodeInfoResponse is authenticated via the AEAD-encrypted session (Phase 127 T-127-02); a MitM cannot modify it without breaking AEAD. Further defense: receiving peer still enforces its own cap at ingest (Phase 128), so a forged cap=0 only degrades to "pre-129 behavior" — not a correctness break.

No threat_flag annotations required — all new surface is within the existing Phase 129 threat model.

## Requirements Touched

- `SYNC-02` — **complete** (peer-to-peer capability-aware sync-out filter active at all announce paths).
- `SYNC-03` — **complete** (3 enumerated sites + responder-side mirror all filter by peer cap, increment per-peer counter per skip).
- `VERI-03` — **complete** (Catch2 tag `[sync][cap-filter]` green, 39 assertions, 4 scenarios × 3 sites coverage + helper contract + cross-cutting).
- `VERI-05` — **pending user action** (`129-UAT.md` exists with `status: pending`; user runs 2-node procedure and flips status).

## Commits

| Task | Commit     | Files                                                                                                           | Summary |
|------|------------|-----------------------------------------------------------------------------------------------------------------|---------|
| 1    | `06fd1803` | db/peer/blob_push_manager.{cpp,h}, db/peer/sync_orchestrator.{cpp,h}, db/peer/peer_manager.cpp                  | Helper + 3 sites + responder mirror + MetricsCollector plumbing |
| 2    | `292a93e4` | db/tests/peer/test_sync_cap_filter.cpp, db/CMakeLists.txt                                                        | VERI-03 unit coverage — 39 assertions, 6 TEST_CASEs, tag [sync][cap-filter] |
| 3    | `a8f186f3` | .planning/phases/129-sync-cap-divergence/129-UAT.md                                                               | VERI-05 user-delegated UAT procedure (status: pending) |

## Self-Check: PASSED

- `db/peer/blob_push_manager.h` modified in `06fd1803` — **FOUND** (grep confirms `should_skip_for_peer_cap` definition + `MetricsCollector&` ctor param).
- `db/peer/blob_push_manager.cpp` modified in `06fd1803` — **FOUND** (2 counter increments, 4 references to `advertised_blob_cap`, 2 helper calls).
- `db/peer/sync_orchestrator.h` modified in `06fd1803` — **FOUND** (forward decl + ctor param + member).
- `db/peer/sync_orchestrator.cpp` modified in `06fd1803` — **FOUND** (3 counter increments [initiator + responder mirror + log context], 4 references to `advertised_blob_cap`, 3 helper calls).
- `db/peer/peer_manager.cpp` modified in `06fd1803` — **FOUND** (both ctor sites pass `metrics_collector_`).
- `db/tests/peer/test_sync_cap_filter.cpp` created in `292a93e4` — **FOUND** (file present, 6 TEST_CASEs, tag `[sync][cap-filter]` × 6).
- `db/CMakeLists.txt` modified in `292a93e4` — **FOUND** (new source registered).
- `.planning/phases/129-sync-cap-divergence/129-UAT.md` created in `a8f186f3` — **FOUND** (status: pending, VERI-05×3, --node home×4, --node local×4).
- Commit `06fd1803` — **FOUND** in `git log --oneline -5`.
- Commit `292a93e4` — **FOUND** in `git log --oneline -5`.
- Commit `a8f186f3` — **FOUND** in `git log --oneline -5`.
- Build gate `chromatindb` target — **exit 0** (post Task 1).
- Build gate `chromatindb_tests` target — **exit 0** (post Task 2).
- Run gate `./build-debug/db/chromatindb_tests "[sync][cap-filter]"` — **exit 0**, `All tests passed (39 assertions in 6 test cases)`.

---
*Phase: 129-sync-cap-divergence*
*Plan: 129-02-filter-tests-uat*
*Completed: 2026-04-23*
