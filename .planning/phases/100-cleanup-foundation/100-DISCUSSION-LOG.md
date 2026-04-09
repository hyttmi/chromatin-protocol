# Phase 100: Cleanup & Foundation - Discussion Log

> **Audit trail only.** Do not use as input to planning, research, or execution agents.
> Decisions are captured in CONTEXT.md — this log preserves the alternatives considered.

**Date:** 2026-04-09
**Phase:** 100-cleanup-foundation
**Areas discussed:** Cleanup scope, New relay directory layout, Send queue design, Scaffold scope

---

## Cleanup Scope

### Identity Migration

| Option | Description | Selected |
|--------|-------------|----------|
| Migrate relay_identity | Move to new relay/ tree, update includes | |
| Rewrite from scratch | Start fresh for WebSocket challenge-response | |
| You decide | Claude picks based on code quality | |

**User's choice:** Copy (not move) — db will be moved to a different repo soon so relay_identity needs to be copied, not shared.
**Notes:** User explicitly noted db/ is moving to a separate repo. This means no shared code between db/ and relay/.

### Old Relay Tests

| Option | Description | Selected |
|--------|-------------|----------|
| Delete all old relay tests | Clean break, new architecture | ✓ |
| Keep identity test as reference | Copy test_relay_identity.cpp | |
| You decide | Claude evaluates reusable logic | |

**User's choice:** Delete all old relay tests
**Notes:** None

### Doc Cleanup

| Option | Description | Selected |
|--------|-------------|----------|
| Scrub all references | Remove/update every mention in PROTOCOL.md, README, .planning/ | ✓ |
| Scrub code-facing only | Clean PROTOCOL.md/README, leave .planning/ archives | |
| Minimal — just delete dirs | Delete directories only, fix refs later | |

**User's choice:** Scrub all references
**Notes:** None

---

## New Relay Directory Layout

### Directory Structure

| Option | Description | Selected |
|--------|-------------|----------|
| Nested subdirectories | relay/ws/, core/, translate/, config/, identity/ — matches db/ | ✓ |
| Flat structure | All .h/.cpp directly in relay/ | |
| You decide | Claude picks based on file count | |

**User's choice:** Nested subdirectories
**Notes:** None

### C++ Namespace

| Option | Description | Selected |
|--------|-------------|----------|
| chromatindb::relay | Sub-namespaces matching directories. Consistent with db/ | ✓ |
| relay:: | Standalone top-level namespace | |
| You decide | Claude picks based on include deps | |

**User's choice:** chromatindb::relay
**Notes:** None

### Test Location

| Option | Description | Selected |
|--------|-------------|----------|
| relay/tests/ | Self-contained, own Catch2 executable | ✓ |
| tests/relay/ | Top-level tests directory | |
| You decide | Claude picks based on CMake structure | |

**User's choice:** relay/tests/
**Notes:** None

---

## Send Queue Design

### Queue Pattern

| Option | Description | Selected |
|--------|-------------|----------|
| Same pattern, separate impl | deque + drain coroutine + configurable cap | |
| Different design | WebSocket-specific queue model | |
| You decide | Claude evaluates WebSocket differences | |

**User's choice:** Separate implementation, same pattern, but must support thousands of connections.
**Notes:** User emphasized relay faces thousands of clients, not dozens like the node. Memory awareness is critical.

### Queue Cap

| Option | Description | Selected |
|--------|-------------|----------|
| 256 messages | Lower than db/'s 1024 for memory | |
| 64 messages | Aggressive disconnection | |
| Configurable | Default 256, tunable via config | ✓ |
| You decide | Claude picks based on analysis | |

**User's choice:** Configurable
**Notes:** None

### Overflow Behavior

| Option | Description | Selected |
|--------|-------------|----------|
| Disconnect immediately | Log + close, client reconnects | ✓ |
| Drop oldest messages | Keep connection, discard backlog | |
| You decide | Claude picks based on protocol semantics | |

**User's choice:** Disconnect immediately
**Notes:** None

---

## Scaffold Scope

### Binary Scope

| Option | Description | Selected |
|--------|-------------|----------|
| Config + signal + session stub | main() loads config, SIGTERM/SIGHUP, Session compiles, exits cleanly | ✓ |
| Config + accept loop skeleton | Also raw TCP accept loop, overlaps Phase 101 | |
| Minimal main() only | Just main() with args/config/log/exit | |

**User's choice:** Config + signal + session stub
**Notes:** None

### CMake Approach

| Option | Description | Selected |
|--------|-------------|----------|
| add_subdirectory from root | Shares FetchContent deps | |
| Standalone CMakeLists.txt | Own top-level CMake, fetches own deps | ✓ |
| You decide | Claude picks based on build structure | |

**User's choice:** Standalone CMakeLists.txt — user wants it extractable to private repo.
**Notes:** User initially considered separate private repo from day one, but decided to build here and move later. Standalone CMake makes extraction clean.

### Repo Strategy

| Option | Description | Selected |
|--------|-------------|----------|
| Separate private repo now | Create private repo immediately | |
| Build here, move later | Keep in this repo during v3.0.0, standalone CMake for extraction | ✓ |
| You decide | Claude picks based on friction | |

**User's choice:** Build here, move later
**Notes:** None

### Logging

| Option | Description | Selected |
|--------|-------------|----------|
| spdlog, same pattern | Consistent with db/ node | ✓ |
| You decide | Claude picks | |

**User's choice:** spdlog, same pattern
**Notes:** None

---

## Claude's Discretion

- Config file format details (field names, defaults)
- Internal class API design for Session and send queue

## Deferred Ideas

None — discussion stayed within phase scope.
