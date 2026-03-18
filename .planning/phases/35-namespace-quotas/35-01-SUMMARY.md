---
phase: 35-namespace-quotas
plan: 01
subsystem: database
tags: [libmdbx, quota, config, flatbuffers, namespace]

# Dependency graph
requires:
  - phase: 16-storage-capacity
    provides: max_storage_bytes capacity pattern, StorageFull wire message
  - phase: 24-encryption-at-rest
    provides: DARE envelope format, encrypt_value/decrypt_value helpers
provides:
  - NamespaceQuota struct and get/rebuild API in storage.h
  - quota_map sub-database with O(1) namespace aggregate reads
  - Atomic increment/decrement in store_blob, delete_blob_data, run_expiry_scan
  - Startup rebuild from blobs_map for accuracy
  - Config struct with namespace_quota_bytes, namespace_quota_count, namespace_quotas map
  - IngestError::quota_exceeded enum variant
  - TransportMsgType_QuotaExceeded = 26 wire message type
affects: [35-02-engine-enforcement, 35-03-peer-sighup]

# Tech tracking
tech-stack:
  added: []
  patterns: [quota sub-database aggregate pattern, tombstone exemption pattern]

key-files:
  created: []
  modified:
    - db/storage/storage.h
    - db/storage/storage.cpp
    - db/config/config.h
    - db/config/config.cpp
    - db/engine/engine.h
    - db/schemas/transport.fbs
    - db/wire/transport_generated.h
    - db/tests/storage/test_storage.cpp
    - db/tests/config/test_config.cpp

key-decisions:
  - "Quota aggregate tracks encrypted envelope size (actual disk cost, not wire-level encoded size)"
  - "Tombstones exempt from quota increment (never counted, consistent with Phase 16 capacity exemption)"
  - "Startup rebuild counts all blobs_map entries including tombstones (tiny overhead, no decrypt needed)"
  - "Quota entry erased when both bytes and count reach 0 (keeps sub-database clean)"
  - "max_maps increased from 7 to 8 (7 named sub-databases + 1 default)"

patterns-established:
  - "Quota sub-database: [namespace:32] -> [total_bytes_be:8][blob_count_be:8]"
  - "Atomic quota update in write transaction: read-modify-write in same mdbx txn"
  - "Per-namespace config override via std::optional (has_value = explicitly set, no value = inherit global)"

requirements-completed: [QUOTA-03, QUOTA-04]

# Metrics
duration: 26min
completed: 2026-03-18
---

# Phase 35 Plan 01: Quota Storage Foundation Summary

**Materialized namespace quota aggregates in libmdbx sub-database with O(1) reads, atomic increment/decrement in write transactions, startup rebuild, config parsing with per-namespace overrides, and IngestError/wire enum extensions**

## Performance

- **Duration:** 26 min
- **Started:** 2026-03-18T13:42:56Z
- **Completed:** 2026-03-18T14:09:27Z
- **Tasks:** 2 (TDD: RED-GREEN each)
- **Files modified:** 9

## Accomplishments
- Quota sub-database (7th named sub-db) with encode/decode helpers for [total_bytes_be:8][blob_count_be:8]
- store_blob atomically increments quota (tombstones exempt), delete_blob_data and run_expiry_scan atomically decrement
- Startup rebuild_quota_aggregates computes accurate totals from blobs_map on every open
- Config struct extended with namespace_quota_bytes, namespace_quota_count, namespace_quotas map with per-namespace overrides
- IngestError::quota_exceeded and TransportMsgType_QuotaExceeded = 26 ready for downstream plans
- 15 new tests (8 storage quota + 7 config quota), 353 total tests passing with zero regressions

## Task Commits

Each task was committed atomically (TDD RED then GREEN):

1. **Task 1: Quota sub-database in Storage + aggregate operations**
   - `01f5934` (test: failing quota tests - RED)
   - `2ee4ff0` (feat: implement quota sub-database and aggregates - GREEN)
2. **Task 2: Config struct extension + JSON parsing + enum values**
   - `ca018ca` (test: failing config quota tests - RED)
   - `7479c82` (feat: implement quota config parsing - GREEN)
   - `db1475e` (chore: regenerate transport_generated.h)

## Files Created/Modified
- `db/storage/storage.h` - NamespaceQuota struct, get/rebuild API declarations
- `db/storage/storage.cpp` - quota_map sub-database, increment/decrement in write txns, rebuild
- `db/config/config.h` - namespace_quota_bytes, namespace_quota_count, namespace_quotas fields
- `db/config/config.cpp` - JSON parsing for quota config with validation
- `db/engine/engine.h` - IngestError::quota_exceeded enum variant
- `db/schemas/transport.fbs` - QuotaExceeded = 26 message type
- `db/wire/transport_generated.h` - Regenerated FlatBuffers header
- `db/tests/storage/test_storage.cpp` - 8 quota aggregate tests
- `db/tests/config/test_config.cpp` - 7 quota config tests

## Decisions Made
- Encrypted envelope size (not wire-encoded size) used as byte quota measurement -- tracks actual disk cost
- Tombstones never counted in quota aggregate -- owners must always be able to delete at any quota level
- Quota entry erased from sub-database when namespace reaches 0/0 -- keeps quota_map clean
- max_maps increased from 7 to 8 to accommodate 7th named sub-database (Pitfall 5 from RESEARCH.md)
- Startup rebuild counts everything (including tombstones) from blobs_map -- tiny overcount is acceptable, no decryption needed

## Deviations from Plan

### Auto-fixed Issues

**1. [Rule 3 - Blocking] Regenerated transport_generated.h**
- **Found during:** Task 2 (post-verification)
- **Issue:** cmake build system didn't auto-trigger FlatBuffers regeneration for transport.fbs change
- **Fix:** Manually triggered `cmake --build . --target flatbuffers_transport_generated`
- **Files modified:** db/wire/transport_generated.h
- **Verification:** QuotaExceeded = 26 present in generated header, build succeeds
- **Committed in:** db1475e

---

**Total deviations:** 1 auto-fixed (1 blocking)
**Impact on plan:** Necessary for downstream plans to use TransportMsgType_QuotaExceeded. No scope creep.

## Issues Encountered
None beyond the FlatBuffers regeneration noted above.

## User Setup Required
None - no external service configuration required.

## Next Phase Readiness
- Quota storage foundation complete: all contracts exist for 35-02 (engine enforcement)
- Config struct ready for PeerManager to pass quota limits to BlobEngine
- IngestError::quota_exceeded ready for BlobEngine::ingest() Step 2a check
- TransportMsgType_QuotaExceeded ready for PeerManager wire message handling
- No blockers for 35-02 or 35-03

## Self-Check: PASSED

All 9 files verified present. All 5 commits verified in git log.

---
*Phase: 35-namespace-quotas*
*Completed: 2026-03-18*
