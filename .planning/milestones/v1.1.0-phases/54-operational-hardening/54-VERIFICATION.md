---
phase: 54-operational-hardening
verified: 2026-03-22T11:06:08Z
status: passed
score: 13/13 must-haves verified
re_verification: false
---

# Phase 54: Operational Hardening Verification Report

**Phase Goal:** Node operators have finer control over GC timing, protection against malformed timestamps, and actionable error messages on sync rejection
**Verified:** 2026-03-22T11:06:08Z
**Status:** passed
**Re-verification:** No — initial verification

## Goal Achievement

### Observable Truths

| # | Truth | Status | Evidence |
|---|-------|--------|----------|
| 1 | Operator can set expiry_scan_interval_seconds in config and the node scans at that interval | VERIFIED | `config.h:45` field, `config.cpp:52` parsed, `peer_manager.cpp:1838` uses member |
| 2 | SIGHUP reloads expiry_scan_interval_seconds and restarts the timer with the new value | VERIFIED | `peer_manager.cpp:1802-1805` reloads value and calls `expiry_timer_->cancel()` |
| 3 | validate_config rejects expiry_scan_interval_seconds below 10 | VERIFIED | `config.cpp:285-287` enforces `< 10` check, 10 config tests pass |
| 4 | SyncRejected reason byte maps to human-readable string via shared constexpr table | VERIFIED | `sync_reject.h:21-33` constexpr switch, 11 assertions pass in [syncreason] tests |
| 5 | Initiator logs rejection reason at info level, receiver (sender) logs at debug level | VERIFIED | `peer_manager.cpp:814` info for initiator; `peer_manager.cpp:487,527` debug for sender |
| 6 | All 8 reason codes exist: cooldown, session_limit, byte_rate, storage_full, quota_exceeded, namespace_not_found, blob_too_large, timestamp_rejected | VERIFIED | `sync_reject.h:10-17` all 8 constexpr uint8_t constants defined |
| 7 | Node rejects blobs with timestamps more than 1 hour in the future | VERIFIED | `engine.cpp:125,130-133` Step 0c, 22 engine timestamp assertions pass |
| 8 | Node rejects blobs with timestamps more than 30 days in the past | VERIFIED | `engine.cpp:126,135-138` Step 0c, 22 engine timestamp assertions pass |
| 9 | Timestamp validation happens before any crypto work (Step 0 placement) | VERIFIED | `engine.cpp:123` comment "Step 0c: Timestamp validation (cheap integer compare, before any crypto)" — placed after size/capacity checks, before structural/crypto |
| 10 | Timestamp validation applies to both direct ingest and sync ingest paths | VERIFIED | `engine.cpp:123-138` in `ingest()`, `engine.cpp:302-317` in `delete_blob()`, `sync_protocol.cpp:101-103` handles timestamp_rejected in sync path |
| 11 | Rejection includes actionable reason string in IngestResult | VERIFIED | `engine.cpp:132` "timestamp too far in future (more than 1 hour ahead)", `engine.cpp:137` "timestamp too far in past (more than 30 days ago)" |
| 12 | Sync-path timestamp rejections handled (debug log, skip blob, no session abort) | VERIFIED | `sync_protocol.cpp:101-103` debug log and skip; `peer_manager.cpp:712-716` debug log Data handler |
| 13 | PROTOCOL.md documents all 8 sync reject reason codes and timestamp validation rules | VERIFIED | `PROTOCOL.md:352-361` 8-code table, `PROTOCOL.md:365-382` Timestamp Validation section with thresholds |

**Score:** 13/13 truths verified

### Required Artifacts

| Artifact | Expected | Status | Details |
|----------|----------|--------|---------|
| `db/peer/sync_reject.h` | Shared constexpr sync rejection reason codes and byte-to-string mapping | VERIFIED | Created; 8 codes + `sync_reject_reason_string()` in `chromatindb::peer` namespace |
| `db/config/config.h` | `expiry_scan_interval_seconds` field in Config struct | VERIFIED | Line 45: `uint32_t expiry_scan_interval_seconds = 60;` |
| `db/config/config.cpp` | Parsing, validation, and known_keys entry for new config field | VERIFIED | Load at line 52, known_keys at line 69, validate at lines 285-287 |
| `db/peer/peer_manager.cpp` | Expiry scan uses config interval, SIGHUP reloads it, rejection sites use shared header | VERIFIED | Lines 1838, 1802-1805, include at line 2 |
| `db/engine/engine.h` | `timestamp_rejected` IngestError enum value | VERIFIED | Line 32: `timestamp_rejected` in enum |
| `db/engine/engine.cpp` | Step 0c timestamp validation in `ingest()` and `delete_blob()` | VERIFIED | Lines 123-138 (ingest), 302-317 (delete_blob) |
| `db/peer/peer_manager.h` | `expiry_scan_interval_seconds_` SIGHUP-reloadable member | VERIFIED | Line 307: member with comment |
| `db/main.cpp` | Startup log for expiry scan interval | VERIFIED | Line 132: `spdlog::info("expiry scan interval: {}s", ...)` |
| `db/PROTOCOL.md` | All 8 sync reject codes and Timestamp Validation section | VERIFIED | Lines 352-382 |

### Key Link Verification

| From | To | Via | Status | Details |
|------|----|-----|--------|---------|
| `db/peer/peer_manager.cpp` | `db/peer/sync_reject.h` | `#include` + `sync_reject_reason_string` | WIRED | Include at line 2; used at lines 489, 529, 813 |
| `db/peer/peer_manager.cpp` | `db/config/config.h` | `config_.expiry_scan_interval_seconds` in `expiry_scan_loop` | WIRED | Line 1838: `std::chrono::seconds(expiry_scan_interval_seconds_)` |
| `db/config/config.cpp` | `db/config/config.h` | `validate_config` checks new field range | WIRED | Lines 285-287: enforces `>= 10` |
| `db/engine/engine.cpp` | `db/engine/engine.h` | `IngestError::timestamp_rejected` enum value | WIRED | Lines 132, 137: `IngestResult::rejection(IngestError::timestamp_rejected, ...)` |
| `db/peer/peer_manager.cpp` | `db/peer/sync_reject.h` | `SYNC_REJECT_TIMESTAMP_REJECTED` accessible for future session-level use | WIRED | Constant defined in included header; per-blob rejections handled via debug log + skip |
| `db/engine/engine.cpp` | system clock | `std::chrono::system_clock::now()` for current time | WIRED | Lines 128-129, 307-308 |
| `db/sync/sync_protocol.cpp` | `db/engine/engine.h` | `IngestError::timestamp_rejected` in sync ingest path | WIRED | Line 101: `*result.error == engine::IngestError::timestamp_rejected` |

### Requirements Coverage

| Requirement | Source Plan | Description | Status | Evidence |
|-------------|-------------|-------------|--------|---------|
| OPS-01 | 54-01-PLAN.md | Node operator can configure expiry scan interval via config field (replacing hardcoded 60s) | SATISFIED | `config.h` field, `config.cpp` parse+validate, `peer_manager.cpp` wired + SIGHUP reload |
| OPS-02 | 54-02-PLAN.md | Node rejects blobs with timestamps too far in the future or past on ingest | SATISFIED | Step 0c in `engine.cpp` ingest() and delete_blob(); 8 tests pass |
| OPS-03 | 54-01-PLAN.md | SyncRejected messages include human-readable reason strings for operator debugging | SATISFIED | `sync_reject.h` shared header; all send sites use constants + reason_string |
| DOCS-03 | 54-02-PLAN.md | db/PROTOCOL.md updated with sync reject reason strings | SATISFIED | `PROTOCOL.md` has 8-code table and Timestamp Validation section |

No orphaned requirements — all 4 phase-54 requirements (OPS-01, OPS-02, OPS-03, DOCS-03) claimed and verified.

### Anti-Patterns Found

None. No TODOs, FIXMEs, placeholders, or stub implementations found in modified files.

### Notable Deviations (Non-Blocking)

**Plan 02 truth: "Sync-path timestamp rejections use SYNC_REJECT_TIMESTAMP_REJECTED code"**

The plan text itself clarified mid-task that sending `SYNC_REJECT_TIMESTAMP_REJECTED` is only appropriate at session acceptance time, not per-blob during an active sync. The implemented behavior (debug log + skip) is correct and explicitly documented in `PROTOCOL.md:380`. This is an intentional, self-correcting deviation — not a gap.

**Pre-existing peer test flakiness**

2 of 14 peer manager tests fail due to port conflicts in test infrastructure (timing-sensitive network tests). This is a known pre-existing issue documented in project MEMORY and the SUMMARY.md. Not introduced by phase 54.

**`SYNC_REJECT_SESSION_LIMIT` defined but not sent**

The session limit check at `peer_manager.cpp:510-516` silently drops rather than sending `SYNC_REJECT_SESSION_LIMIT`. This is pre-existing behavior — the plan only required that all 8 reason codes EXIST in the header, which they do. Not a gap.

### Human Verification Required

None. All phase-54 behaviors are programmatically verifiable.

---

_Verified: 2026-03-22T11:06:08Z_
_Verifier: Claude (gsd-verifier)_
