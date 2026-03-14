---
phase: 18-abuse-prevention-topology
verified: 2026-03-12T05:35:00Z
status: passed
score: 9/9 must-haves verified
re_verification:
  previous_status: gaps_found
  previous_score: 8/9
  gaps_closed:
    - "A peer exceeding the configured byte rate on Data/Delete messages is disconnected immediately (E2E test now proves full disconnect path: rate_limited >= 1 and peer_count == 0 after oversized Data message)"
    - "Stale comment 'Rate limit rejections (Phase 18, stub at 0)' on rate_limited counter corrected to 'Rate limit disconnections' in peer_manager.h"
  gaps_remaining: []
  regressions: []
---

# Phase 18: Abuse Prevention & Topology Verification Report

**Phase Goal:** Open nodes resist write-flooding abuse and operators control which namespaces replicate
**Verified:** 2026-03-12T05:35:00Z
**Status:** passed
**Re-verification:** Yes -- after gap closure via Plan 03

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | A peer exceeding the configured byte rate on Data/Delete messages is disconnected immediately | VERIFIED | `peer_manager.cpp:385-395` rate check + `close_gracefully()`. E2E test `[peer][ratelimit][disconnect]` at line 1589: sends 7528-byte Data payload against 100B burst, asserts `rate_limited >= 1` and `peer_count == 0`. Observed WARN log in test run: "rate limit exceeded by peer 127.0.0.1:47832 (7528 bytes, limit 100B/s), disconnecting". Test passes. |
| 2 | Sync BlobTransfer messages are never rate-limited | VERIFIED | Rate check at `peer_manager.cpp:385` conditioned on `TransportMsgType_Data || TransportMsgType_Delete` only. Sync-bypass test passes with `rate_limited == 0` after full sync with 100B burst. |
| 3 | Rate limit parameters are configurable via JSON config and SIGHUP-reloadable | VERIFIED | `config.h:23-24` fields; `config.cpp:34-35` parsing; `peer_manager.cpp:1041-1048` SIGHUP reload. Config tests and `[ratelimit][reload]` test pass. |
| 4 | rate_limit_bytes_per_sec=0 disables rate limiting (backward compatible default) | VERIFIED | `config.h:23` default 0; `peer_manager.cpp:386` guard on `> 0`. All tests with default config unaffected. |
| 5 | metrics_.rate_limited counter increments once per disconnection event | VERIFIED | `peer_manager.cpp:390` `++metrics_.rate_limited` before `close_gracefully()`. E2E test at line 1660 asserts `rate_limited >= 1` and it passes. |
| 6 | Operator can configure sync_namespaces as a list of 64-char hex namespace hashes | VERIFIED | `config.h:25` field; `config.cpp:45-52` parsing with `validate_allowed_keys` reuse; `[nsfilter][config]` tests pass. |
| 7 | Namespace filter is applied at sync Phase A so filtered namespace IDs are never sent to peers | VERIFIED | `peer_manager.cpp:568-572` (initiator) and `peer_manager.cpp:769-773` (responder) both apply `std::erase_if` with `sync_namespaces_`. E2E nsfilter test passes. |
| 8 | Namespace filter is applied at Data/Delete ingest so filtered namespace blobs are silently dropped | VERIFIED | `peer_manager.cpp:402-408` (Delete) and `peer_manager.cpp:461-466` (Data); silent return with DEBUG log. E2E test confirms filtered blob not ingested. |
| 9 | Empty sync_namespaces means replicate all namespaces (backward compatible default) | VERIFIED | All filter checks guarded by `!sync_namespaces_.empty()`; `config.h:25` default empty vector. 14 assertions in 5 nsfilter test cases pass. |

**Score:** 9/9 truths verified

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/config/config.h` | rate_limit_bytes_per_sec, rate_limit_burst, sync_namespaces fields | VERIFIED | All 3 fields present |
| `db/config/config.cpp` | JSON parsing for rate limit and sync_namespaces fields | VERIFIED | Parsing and validation present |
| `db/peer/peer_manager.h` | bucket_tokens/bucket_last_refill in PeerInfo; rate_limit*/sync_namespaces_ in PeerManager; corrected comment on rate_limited | VERIFIED | All fields present; comment reads "Rate limit disconnections" (stale "stub at 0" removed) |
| `db/peer/peer_manager.cpp` | try_consume_tokens, hex_to_namespace, rate check in on_peer_message, bucket init, namespace filter at Phase A/ingest, reload | VERIFIED | All functions present and wired |
| `tests/config/test_config.cpp` | Config parsing tests for rate limit and sync_namespaces | VERIFIED | 3 rate limit tests + 4 sync_namespaces tests pass (67 assertions in 33 config test cases total) |
| `tests/peer/test_peer_manager.cpp` | E2E test verifying rate limit disconnect (rate_limited >= 1, peer_count == 0) | VERIFIED | "PeerManager rate limiting disconnects peer exceeding burst" at line 1589; tags `[peer][ratelimit][disconnect]`; passes |
| `tests/peer/test_peer_manager.cpp` | E2E test verifying namespace filter at sync | VERIFIED | "PeerManager namespace filter excludes filtered namespaces" at line 1673; tags `[peer][nsfilter]`; passes |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|----|--------|---------|
| `peer_manager.cpp (on_peer_message)` | `try_consume_tokens helper` | Rate check before Data/Delete processing | WIRED | Lines 385-395: guard on `TransportMsgType_Data\|Delete`, calls `try_consume_tokens`, `close_gracefully()` on exceed |
| `peer_manager.cpp (on_peer_connected)` | `PeerInfo bucket_tokens initialization` | Set bucket_tokens = burst, bucket_last_refill = now | WIRED | Lines 231-234: `info.bucket_tokens = rate_limit_burst_` and timestamp set |
| `peer_manager.cpp (reload_config)` | `Config rate_limit_bytes_per_sec` | SIGHUP reload updates rate limit params | WIRED | Lines 1041-1048: both `rate_limit_bytes_per_sec_` and `rate_limit_burst_` updated |
| `peer_manager.cpp (run_sync_with_peer Phase A)` | `sync_namespaces_ filter` | std::erase_if on list_namespaces() result | WIRED | Lines 568-572: `erase_if` with `sync_namespaces_.find` guard |
| `peer_manager.cpp (handle_sync_as_responder Phase A)` | `sync_namespaces_ filter` | std::erase_if on list_namespaces() result | WIRED | Lines 769-773: identical pattern to initiator |
| `peer_manager.cpp (on_peer_message Data/Delete)` | `sync_namespaces_ filter` | Check namespace_id before engine_.ingest/delete_blob | WIRED | Lines 402-408 (Delete) and 461-466 (Data): checks before engine operations |
| `peer_manager.cpp (reload_config)` | `Config sync_namespaces` | SIGHUP reload rebuilds sync_namespaces_ set | WIRED | Lines 1050-1065: validates, clears, rebuilds from hex strings |

**Note on Plan 02 deviation:** Phase C (blob request stage) filtering was added to both `run_sync_with_peer` (lines 626-628) and `handle_sync_as_responder` (lines 814-816). This is correct -- Phase A filter alone is insufficient since the peer's namespace list is unfiltered; Phase C prevents requesting blobs from filtered namespaces.

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|----------|
| PROT-01 | 18-01 | Per-connection token bucket rate limiter applies to Data/Delete messages (not sync BlobTransfer) | SATISFIED | Rate check on `TransportMsgType_Data\|Delete` only; sync bypass confirmed by passing test |
| PROT-02 | 18-01 | Rate limit exceeded triggers immediate disconnect (no backpressure delay) | SATISFIED | `++metrics_.rate_limited` then `close_gracefully()` at lines 390-395. E2E disconnect test passes: `rate_limited >= 1` and `peer_count == 0` asserted and verified. (Note: requirement text said "strike system" but all plans and the implementation explicitly use immediate disconnect without strike involvement -- the intent is satisfied.) |
| PROT-03 | 18-01 | Rate limit parameters configurable (rate_limit_bytes_per_sec, rate_limit_burst) | SATISFIED | Both fields in Config struct, JSON parsed, SIGHUP reloaded; config tests pass |
| PROT-04 | 18-02 | Operator can configure sync_namespaces to filter which namespaces the node replicates | SATISFIED | `config.h:25`, `config.cpp:45-52`, PeerManager builds set; config tests pass |
| PROT-05 | 18-02 | Namespace filter applied at sync Phase A (namespace list assembly) | SATISFIED | Phase A filter in both initiator and responder; also at Phase C for consistency |
| PROT-06 | 18-02 | Empty sync_namespaces means replicate all (backward compatible default) | SATISFIED | All filter blocks guarded by `!sync_namespaces_.empty()`; E2E test passes |

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| `tests/peer/test_peer_manager.cpp` | 1441 | Stale test comment: `// rate_limited starts at 0 (Phase 18 stub)` -- the counter is now wired and tested | Info | No functional impact; test comment only, not in headers or implementation |

The original anti-pattern from the initial verification (stale comment in `db/peer/peer_manager.h` line 68) has been fully resolved: the comment now reads "Rate limit disconnections".

### Human Verification Required

None. All checks are verifiable programmatically for this phase.

### Test Suite Results

| Test group | Command | Result | Assertions |
|------------|---------|--------|------------|
| `[ratelimit]` (all 6 tests) | `./build/chromatindb_tests "[ratelimit]"` | ALL PASS | 17 |
| `[nsfilter]` (all 5 tests) | `./build/chromatindb_tests "[nsfilter]"` | ALL PASS | 14 |
| `[config]` (all 33 tests) | `./build/chromatindb_tests "[config]"` | ALL PASS | 67 |
| `[peer]` (28 tests total) | `./build/chromatindb_tests "[peer]"` | 27/28 PASS | 175/176 |

The single peer test failure is a pre-existing SIGSEGV in "PeerManager storage full signaling" (line 1211), documented in `deferred-items.md`. It predates Phase 18 and is not caused by any Phase 18 changes.

### Gap Closure Verification

**Gap closed (Plan 03):** "A peer exceeding the configured byte rate on Data/Delete messages is disconnected immediately"

Plan 03 added test "PeerManager rate limiting disconnects peer exceeding burst" (`[peer][ratelimit][disconnect]`, port 14350):
1. Configures PeerManager with `rate_limit_bytes_per_sec=100` and `rate_limit_burst=100`
2. Connects a raw outbound `Connection` with a separate `NodeIdentity` (completes full PQ handshake)
3. After a 2-second timer, sends a Data message containing an ML-DSA-87-signed blob (7528 bytes encoded -- far exceeds the 100-byte burst)
4. Asserts `metrics().rate_limited >= 1` (disconnect counter incremented)
5. Asserts `peer_count() == 0` (peer was ejected)

Both assertions pass. The WARN log confirms the full code path fired: `on_peer_message` -> `try_consume_tokens` returns false -> `++metrics_.rate_limited` -> `close_gracefully()`.

**Anti-pattern fixed (Plan 03):** `peer_manager.h` `rate_limited` comment corrected from "Rate limit rejections (Phase 18, stub at 0)" to "Rate limit disconnections".

---

_Verified: 2026-03-12T05:35:00Z_
_Verifier: Claude (gsd-verifier)_
