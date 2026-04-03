---
phase: 80-targeted-blob-fetch
verified: 2026-04-03T10:35:00Z
status: gaps_found
score: 9/11 must-haves verified
re_verification: false
gaps:
  - truth: "REQUIREMENTS.md marks PUSH-05 and PUSH-06 as complete"
    status: failed
    reason: "Implementation exists and works, but REQUIREMENTS.md still shows '- [ ]' and 'Pending' for PUSH-05 and PUSH-06"
    artifacts:
      - path: ".planning/REQUIREMENTS.md"
        issue: "Lines 16-17 still show '- [ ]' for PUSH-05 and PUSH-06; lines 92-93 still show 'Pending'"
    missing:
      - "Mark PUSH-05 as [x] in the requirements list"
      - "Mark PUSH-06 as [x] in the requirements list"
      - "Update traceability table to show Status: Complete for PUSH-05 and PUSH-06"
human_verification:
  - test: "BlobFetch propagation end-to-end on live KVM swarm"
    expected: "Writing a blob to node A propagates to C via A->B sync + B->C BlobFetch within ~5 seconds"
    why_human: "Live KVM nodes (192.168.1.200-202) needed to confirm real-network behavior"
---

# Phase 80: Targeted Blob Fetch Verification Report

**Phase Goal:** Peers can fetch a specific blob by hash after receiving a push notification, without triggering full reconciliation
**Verified:** 2026-04-03T10:35:00Z
**Status:** gaps_found
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | BlobFetch=60 and BlobFetchResponse=61 exist in TransportMsgType enum | VERIFIED | `transport.fbs` line 80-81; `transport_generated.h` lines 83-84 |
| 2 | Relay blocks BlobFetch and BlobFetchResponse from client connections | VERIFIED | `message_filter.cpp` lines 39-40 case labels; tests 578-583 pass 100% |
| 3 | Peer receiving BlobNotify for unknown blob sends BlobFetch and receives the blob | VERIFIED | `on_blob_notify()` at line 2996; round-trip test assertions pass (5/5) |
| 4 | BlobFetch is handled inline in the message loop without sync session handshake | VERIFIED | `on_peer_message()` dispatches to `handle_blob_fetch()` directly at line 680-682; no SyncSession involved |
| 5 | BlobFetchResponse not-found returns status byte 0x01 without error or disconnect | VERIFIED | `handle_blob_fetch_response()` at line 3062-3065: status 0x01 = debug log + return |
| 6 | Peer that already has the notified blob does not send BlobFetch | VERIFIED | `on_blob_notify()` line 3014: `if (storage_.has_blob(ns, hash)) return;` |
| 7 | Concurrent BlobNotify for the same hash does not trigger duplicate fetches | VERIFIED | `on_blob_notify()` lines 3017-3018: pending_fetches_ dedup check |
| 8 | BlobFetch is suppressed during active sync with the notifying peer | VERIFIED | `on_blob_notify()` lines 3007-3011: `if (peer->syncing) return;` |
| 9 | Pending fetch set is cleaned up on peer disconnect | VERIFIED | `on_peer_disconnected()` lines 469-473: iterates pending_fetches_, erases entries for disconnected peer |
| 10 | Existing tests still pass (no regressions in relay filter) | VERIFIED | Tests 578-583 pass 100% (6/6) |
| 11 | REQUIREMENTS.md marks PUSH-05 and PUSH-06 as complete | FAILED | Lines 16-17 still `- [ ]`; lines 92-93 still "Pending" — code implemented but docs not updated |

**Score:** 10/11 truths verified (1 documentation gap)

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/schemas/transport.fbs` | BlobFetch=60 and BlobFetchResponse=61 enum values | VERIFIED | Lines 80-81 confirmed |
| `db/wire/transport_generated.h` | Generated header with TransportMsgType_BlobFetch/BlobFetchResponse | VERIFIED | Lines 83-84; also TransportMsgType_MAX updated |
| `relay/core/message_filter.cpp` | Blocklist entries for types 60 and 61 | VERIFIED | Lines 39-41: case labels returning false |
| `db/tests/relay/test_message_filter.cpp` | Test assertions for new blocked types | VERIFIED | Lines 74-75: CHECK_FALSE for both types |
| `db/peer/peer_manager.h` | pending_fetches_ map, on_blob_notify(), handle_blob_fetch(), handle_blob_fetch_response() | VERIFIED | All found at lines 279-348 |
| `db/peer/peer_manager.cpp` | BlobNotify/BlobFetch/BlobFetchResponse handlers, pending set cleanup | VERIFIED | Lines 2996-3109 plus disconnect cleanup at 469-473 |
| `db/tests/peer/test_peer_manager.cpp` | Integration tests tagged [blobfetch] | VERIFIED | 3 tests at lines 4034-4260; all assertions pass |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `db/schemas/transport.fbs` | `db/wire/transport_generated.h` | flatc code generation | VERIFIED | `TransportMsgType_BlobFetch = 60` at header line 83 |
| `relay/core/message_filter.cpp` | `db/wire/transport_generated.h` | switch case entries | VERIFIED | `case TransportMsgType_BlobFetch:` at line 39 |
| `on_blob_notify()` | `storage_.has_blob()` | dedup check before fetch | VERIFIED | Line 3014 |
| `handle_blob_fetch()` | `storage_.get_blob()` | blob retrieval for response | VERIFIED | Line 3042 |
| `handle_blob_fetch_response()` | `engine_.ingest()` | blob ingestion after fetch | VERIFIED | Line 3075 |
| `handle_blob_fetch_response()` | `on_blob_ingested()` | notification fan-out after successful ingest | VERIFIED | Lines 3086-3092 |

### Data-Flow Trace (Level 4)

BlobFetch is a network protocol handler — not a UI component rendering state. Data flow is protocol-level: BlobNotify triggers storage lookup, which drives BlobFetch request, which returns raw bytes, which go through engine_.ingest(). This is verified by the round-trip integration test (12-byte blob arrives at node C with correct data: `c_blob->data == blob.data`).

| Artifact | Data Variable | Source | Produces Real Data | Status |
|----------|---------------|--------|--------------------|--------|
| `handle_blob_fetch()` | `blob = storage_.get_blob(ns, hash)` | libmdbx via Storage | Yes — real DB lookup | FLOWING |
| `handle_blob_fetch_response()` | `result = engine_.ingest(blob, conn)` | wire::decode_blob + engine | Yes — real ingest with crypto verify | FLOWING |
| `on_blob_notify()` | `storage_.has_blob(ns, hash)` | libmdbx via Storage | Yes — real key-only lookup | FLOWING |

### Behavioral Spot-Checks

Tests run in-process with real io_context, real storage, real crypto — not mocked.

| Behavior | Test | Result | Status |
|----------|------|--------|--------|
| BlobFetch round-trip: blob propagates A->B->C | Test #381 (5 assertions) | All assertions pass | PASS |
| BlobFetch dedup: C with existing blob skips fetch | Test #382 (4 assertions) | All assertions pass | PASS |
| Connectivity after BlobFetch cycles | Test #383 (3 assertions) | All assertions pass | PASS |
| Relay blocks BlobFetch/BlobFetchResponse from clients | Tests #578-583 (6 tests) | 100% pass | PASS |

Note: ctest marks tests #381-383 as FAILED due to LeakSanitizer reporting leaks from `OQS_SIG_ml_dsa_87_new` (liboqs) and Asio resolver. The same leaks appear in test #379 (phase 79, landed before phase 80). These are pre-existing test harness leaks in third-party libraries, not regressions from phase 80. All test assertions pass.

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|----------|
| WIRE-02 | 80-01-PLAN.md | BlobFetch (type 60) wire type | SATISFIED | `transport.fbs` line 80, `transport_generated.h` line 83 |
| WIRE-03 | 80-01-PLAN.md | BlobFetchResponse (type 61) wire type | SATISFIED | `transport.fbs` line 81, `transport_generated.h` line 84 |
| PUSH-05 | 80-02-PLAN.md | Peer receiving BlobNotify can fetch blob via BlobFetch | SATISFIED (code) / NOT UPDATED (docs) | `on_blob_notify()` + `handle_blob_fetch_response()` implemented and tested; REQUIREMENTS.md not updated |
| PUSH-06 | 80-02-PLAN.md | BlobFetch handled inline in message loop | SATISFIED (code) / NOT UPDATED (docs) | `on_peer_message()` dispatches inline at lines 680-688; REQUIREMENTS.md not updated |

WIRE-02 and WIRE-03 are correctly marked `[x]` / Complete in REQUIREMENTS.md.
PUSH-05 and PUSH-06 are implemented and tested but still marked `[ ]` / Pending in REQUIREMENTS.md.

### Anti-Patterns Found

| File | Line | Pattern | Severity | Impact |
|------|------|---------|----------|--------|
| `.planning/REQUIREMENTS.md` | 16-17, 92-93 | PUSH-05 and PUSH-06 show `[ ]` / Pending despite implementation | Warning | Requirements tracking is misleading — next milestone audit will see these as unimplemented |

No code anti-patterns found in modified files. No TODO/FIXME/placeholder comments in handler implementations. No stub returns. All handlers produce real output from real data sources.

### Human Verification Required

#### 1. Live KVM BlobFetch propagation

**Test:** On the 3-node KVM swarm (192.168.1.200-202), write a blob to node .200 and observe whether it propagates to .202 within ~5-10 seconds via the BlobNotify->BlobFetch path.
**Expected:** Node .202 has the blob (can ExistsRequest or Read it) within a few seconds without waiting for the 600s safety-net sync interval.
**Why human:** KVM swarm requires SSH access and manual observation; can't verify real-network timing programmatically.

### Gaps Summary

One gap blocks clean goal achievement:

**REQUIREMENTS.md documentation not updated (PUSH-05, PUSH-06):** Both requirements are implemented (BlobFetch handlers are live, all assertions pass) but REQUIREMENTS.md still marks them as `[ ]` and "Pending". WIRE-02 and WIRE-03 were correctly marked as complete. This is a post-implementation documentation update that was missed.

The fix is trivial: change `- [ ]` to `- [x]` for PUSH-05 and PUSH-06 at lines 16-17, and update lines 92-93 from "Pending" to "Complete" in the traceability table.

The phase goal itself — "Peers can fetch a specific blob by hash after receiving a push notification, without triggering full reconciliation" — is fully implemented and verified by code and tests.

---

_Verified: 2026-04-03T10:35:00Z_
_Verifier: Claude (gsd-verifier)_
