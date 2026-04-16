---
phase: 118-configurable-constants-peer-management
plan: 02
subsystem: peer-management-cli
tags: [peer-management, subcommands, uds-client, sighup]
dependency_graph:
  requires: [118-01]
  provides: [add-peer-cmd, remove-peer-cmd, list-peers-cmd]
  affects: [main, config]
tech_stack:
  added: []
  patterns: [subcommand-dispatch, json-read-modify-write, uds-trustedhello-client, pidfile-sighup]
key_files:
  created: []
  modified:
    - db/main.cpp
    - db/tests/config/test_config.cpp
decisions:
  - Used raw nlohmann::json for config editing (preserves all fields, no round-trip loss through Config struct)
  - UDS client implements full TrustedHello + AEAD handshake inline in main.cpp (~200 lines) rather than factoring out shared code with CLI
  - Used enum constants from transport_generated.h directly instead of static_cast from int
metrics:
  duration_seconds: 974
  completed: 2026-04-16T12:47:56Z
  tasks_completed: 2
  tasks_total: 2
  files_modified: 2
---

# Phase 118 Plan 02: Peer Management Subcommands Summary

3 peer management subcommands (add-peer, remove-peer, list-peers) implemented on chromatindb binary with JSON config editing, SIGHUP auto-signaling via pidfile, and full TrustedHello UDS client for live peer queries.

## What Changed

### Task 1: add-peer and remove-peer subcommands with SIGHUP
**Commit:** eb487b35

Added `cmd_add_peer` and `cmd_remove_peer` to db/main.cpp following the existing subcommand dispatch pattern. Both use raw nlohmann::json parse-modify-dump to edit bootstrap_peers in config.json (preserving all other fields). add-peer checks for duplicates before adding. remove-peer finds and erases the matching entry. Both read the pidfile from data_dir/chromatindb.pid and send SIGHUP to the running node if the PID is alive. Updated print_usage with all 3 new commands and peer management options. Added 4 unit tests ([peer_cmd] tag) covering: add to empty config, add preserves existing fields, remove from array, remove preserves other fields.

**Files:** db/main.cpp, db/tests/config/test_config.cpp

### Task 2: list-peers subcommand with UDS query and config fallback
**Commit:** 6844c8ca

Implemented `cmd_list_peers` with a full synchronous UDS client that performs TrustedHello handshake (nonce exchange, HKDF-SHA256 key derivation with pubkey salt), AEAD-encrypted auth exchange (SHA3-256 session fingerprint, ML-DSA-87 signature verification), drains SyncNamespaceAnnounce, then sends PeerInfoRequest (type 55) and parses the binary PeerInfoResponse (type 56). Connected peers are merged with config bootstrap_peers to show disconnected entries. Falls back to config-only display when UDS is not configured, socket file doesn't exist, or any handshake step fails (D-14). Added format_duration helper for human-readable uptime display. Added required includes (sodium, oqs, asio/local, auth_helpers, protocol, hash, endian).

**Files:** db/main.cpp

## Verification Results

- Build: PASS (cmake --build . -j$(nproc) exits 0)
- peer_cmd tests: 4 test cases, 15 assertions, all passed
- Config tests: 138 test cases, 251 assertions, all passed
- Full suite: 349 test cases, 348 passed, 1 failed (pre-existing SIGSEGV in "closed mode rejects unauthorized peer" -- not caused by these changes)
- Help text shows all 3 new commands: PASS
- list-peers fallback (--data-dir /tmp): exits 0, shows "Node is not running" with table headers

## Deviations from Plan

None - plan executed exactly as written.

## Known Stubs

None. All 3 subcommands are fully implemented.

## Threat Flags

None found. All new surface (config file editing, pidfile SIGHUP, UDS client with AEAD) is covered by the plan's threat model (T-118-04 through T-118-07).

## Self-Check: PASSED

- db/main.cpp exists on disk
- db/tests/config/test_config.cpp exists on disk
- Commit eb487b35 (Task 1) found in git log
- Commit 6844c8ca (Task 2) found in git log
