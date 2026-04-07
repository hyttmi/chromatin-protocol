---
phase: 91-sdk-delegation-revocation
plan: 02
subsystem: sdk
tags: [python, integration-test, delegation, revocation, kvm]

# Dependency graph
requires:
  - phase: 91-sdk-delegation-revocation
    provides: revoke_delegation(), list_delegates(), DelegationNotFoundError from Plan 01
provides:
  - Integration test for delegation revocation propagation against live KVM swarm
affects: [sdk-docs, phase-verification]

# Tech tracking
tech-stack:
  added: []
  patterns:
    - "Integration test lifecycle: delegate -> write -> revoke -> wait -> rejected write -> absent from list"

key-files:
  created: []
  modified:
    - sdk/python/tests/test_integration.py
    - sdk/python/chromatindb/client.py

key-decisions:
  - "write_blob needs namespace kwarg for delegated writes -- delegates must target owner's namespace"
  - "5-second sleep for tombstone propagation across LAN swarm (conservative for BlobNotify sub-second delivery)"
  - "Node connection dedup must skip UDS sessions -- relay opens separate UDS per client with same identity"

patterns-established:
  - "Delegation revocation integration test pattern: owner creates Directory, delegates, revokes, verifies rejection"

requirements-completed: [REV-02]

# Metrics
duration: ~90min (including relay bug diagnosis and fix)
completed: 2026-04-06
---

# Phase 91 Plan 02: Delegation Revocation Integration Test Summary

**Integration test for delegation revocation propagation lifecycle against live 3-node KVM swarm**

## Performance

- **Duration:** ~90 min (including relay multi-client bug diagnosis and fix)
- **Started:** 2026-04-06T07:59:16Z
- **Completed:** 2026-04-06
- **Tasks:** 2/2
- **Files modified:** 3

## Accomplishments
- Integration test `test_delegation_revocation_propagation()` passes against live KVM swarm
- Test exercises full delegate-write-revoke-reject lifecycle with Directory, DeleteResult, and ProtocolError assertions
- All 548 unit tests pass with no regressions
- 24/25 integration tests pass (1 pre-existing `test_namespace_list` pagination issue)

## Bugs Found and Fixed During Verification

### 1. write_blob missing namespace kwarg (SDK)
- `write_blob` always wrote to `self._identity.namespace`, making delegated writes impossible
- Fix: added `*, namespace: bytes | None = None` parameter
- Commit: `dc8f0c6`

### 2. Node connection dedup kills relay multi-client sessions (C++ node)
- Relay opens separate UDS connection per client session, all sharing the relay's identity
- Node's duplicate-connection logic closed earlier sessions when new clients connected
- Any request on connection A timed out once connection B was open on the same relay
- Fix: skip dedup for UDS connections (`if (!conn->is_uds())`)
- Commit: `429cccc`

## Task Commits

1. **Task 1: Integration test for delegation revocation propagation** - `bdbd9d8`
2. **Task 2: Verify integration test against live KVM swarm** - Verified, 24/25 integration tests pass

## Bug fix commits (found during verification)
3. **write_blob namespace kwarg** - `dc8f0c6`
4. **UDS connection dedup skip** - `429cccc`

## Files Created/Modified
- `sdk/python/tests/test_integration.py` - Added test_delegation_revocation_propagation()
- `sdk/python/chromatindb/client.py` - Added namespace kwarg to write_blob
- `db/peer/peer_manager.cpp` - Skip connection dedup for UDS sessions

## Deviations from Plan

- write_blob required namespace kwarg (not anticipated in plan)
- Node UDS connection dedup bug required C++ fix (not anticipated -- zero C++ changes was the phase goal, but this was a pre-existing bug surfaced by testing)

## Known Stubs

None.

## Issues Encountered
- .200 relay UDS link intermittently broken (operational, not code -- needs SIGHUP to reconnect)

## Self-Check: PASSED

All verification criteria met:
- [x] Integration test exercises full delegation lifecycle
- [x] Revoked delegate write rejected with ProtocolError
- [x] list_delegates confirms delegate absent after revocation
- [x] Existing integration tests unaffected (24/25 pass, 1 pre-existing)
- [x] All 548 unit tests pass

---
*Phase: 91-sdk-delegation-revocation*
*Completed: 2026-04-06*
