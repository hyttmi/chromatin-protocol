---
phase: 93-group-membership-revocation
plan: 02
subsystem: sdk
tags: [python, groups, integration, revocation, e2e]

requires:
  - phase: 93-group-membership-revocation
    plan: 01
    provides: write_to_group() with directory.refresh() for member exclusion
provides:
  - E2E integration test proving group membership revocation lifecycle (GRP-01, GRP-02)
  - Bug fix: register() now writes to directory_namespace (not member's own namespace)
  - Bug fix: _populate_cache() uses >= for group timestamp (same-second overwrites work)
affects: [documentation]

key-files:
  modified:
    - sdk/python/tests/test_integration.py
    - sdk/python/chromatindb/_directory.py
  created: []

verification:
  automated:
    - command: "cd sdk/python && CHROMATINDB_RELAY_HOST=192.168.1.201 .venv/bin/pytest tests/test_integration.py -v -k group_membership -m integration"
      expected: "1 passed"
    - command: "cd sdk/python && .venv/bin/pytest tests/ -x -q --ignore=tests/test_integration.py"
      expected: "589 passed"
---

## Summary

E2E integration test proving group membership revocation works against the live KVM swarm.

## What Was Built

`test_group_membership_revocation()` in `test_integration.py` — full lifecycle test:
1. Admin creates group with 3 members (admin + member_a + member_b)
2. Delegates write access, members self-register in admin's directory namespace
3. Writes encrypted data to group — all 3 decrypt successfully
4. Admin removes member_b from group
5. Writes new encrypted data — only admin + member_a decrypt
6. member_b gets `NotARecipientError` on new data (GRP-01)
7. member_b CAN still decrypt old data — forward exclusion per D-04

## Bugs Found & Fixed

### 1. `register()` namespace bug
`Directory.register()` called `write_blob(entry_data, ttl=0)` without `namespace=self._directory_namespace`. Cross-namespace self-registration (member registering in admin's directory) wrote the UserEntry to the member's own namespace, making it invisible to `_populate_cache()`.

**Fix:** Pass `namespace=self._directory_namespace` to `write_blob()`.

### 2. Group timestamp resolution bug
`_populate_cache()` used strict `>` for group entry timestamp comparison. When `create_group` and `remove_member` happened in the same second, the old 3-member group entry won because it was processed first and the newer 2-member blob didn't beat it (equal timestamp).

**Fix:** Changed to `>=` so later sequence number (newer blob) wins on tie.

## Deviations

- **register() API**: Plan assumed `directory.register(identity, name)` but actual signature is `register(display_name)` using `self._identity`. Test uses delegation + self-registration pattern.
- **Two directory bugs**: Found and fixed during integration testing (not anticipated by plan).

## Self-Check: PASSED
- [x] Integration test passes against KVM swarm (.201)
- [x] 589 unit tests pass, 0 regressions
- [x] Removed member cannot decrypt new data (NotARecipientError)
- [x] Removed member can still decrypt old data (D-04)
