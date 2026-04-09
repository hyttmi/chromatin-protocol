# Phase 100: Cleanup & Foundation - Context

**Gathered:** 2026-04-09
**Status:** Ready for planning

<domain>
## Phase Boundary

Delete old relay/ and sdk/python/ directories, scrub all references from docs and build files, then scaffold a new relay binary with per-client send queue primitive and structured logging. The relay compiles and starts (exits cleanly with no config). No WebSocket, no auth, no UDS — those are Phases 101-104.

</domain>

<decisions>
## Implementation Decisions

### Cleanup Scope
- **D-01:** Copy relay_identity.h/cpp into the new relay tree (do NOT move or share from db/) — db/ will move to a separate repo, so relay needs its own copy.
- **D-02:** Delete all old relay test files in db/tests/relay/ (test_relay_config, test_relay_identity, test_message_filter, test_relay_session). Clean break — old tests aren't useful for the new architecture.
- **D-03:** Scrub ALL references to old relay/SDK from docs — PROTOCOL.md, db/README.md, .planning/ research docs. Full cleanup, not just directory deletion.
- **D-04:** Remove relay-related entries from root CMakeLists.txt (add_subdirectory(relay), chromatindb_relay executable, chromatindb_relay_lib linkage) and from db/CMakeLists.txt (relay test files, relay_lib dependency).

### New Relay Directory Layout
- **D-05:** Nested subdirectories under relay/: ws/, core/, translate/, config/, identity/, tests/. Mirrors db/ conventions.
- **D-06:** C++ namespace: `chromatindb::relay::` with sub-namespaces matching directories (::ws, ::core, ::translate, ::config, ::identity).
- **D-07:** Relay tests live in relay/tests/ with their own Catch2 test executable. Self-contained, separate from db/ tests.

### Send Queue Design
- **D-08:** Separate send queue implementation from db/net/connection.h — same proven pattern (deque + drain coroutine) but independent code. No shared code with db/.
- **D-09:** Send queue cap is configurable via relay config, default 256. Lower than db/'s 1024 because relay must support thousands of concurrent WebSocket clients.
- **D-10:** Overflow behavior: disconnect immediately. Log + close connection. Client reconnects and resubscribes. No silent message dropping.

### Scaffold Scope
- **D-11:** Relay binary at end of Phase 100: main() loads JSON config (bind addr, UDS path, queue limits), handles SIGTERM/SIGHUP, Session class with send queue compiles. Exits cleanly. No accept loop (Phase 101).
- **D-12:** Standalone CMakeLists.txt for relay/ — fetches its own deps (Asio, spdlog, nlohmann/json via FetchContent). Can be extracted to a private repo after v3.0.0 ships. Build here during development, move to private repo later.
- **D-13:** spdlog for structured logging, same pattern as db/ node.

### Claude's Discretion
- Config file format details (field names, defaults) — as long as it's JSON and covers the fields needed.
- Internal class API design for Session and send queue — as long as it matches the decided pattern.

</decisions>

<canonical_refs>
## Canonical References

**Downstream agents MUST read these before planning or implementing.**

### Architecture
- `.planning/research/ARCHITECTURE.md` — Proposed component layout, directory structure, and component responsibilities for relay v2
- `.planning/research/STACK.md` — Technology stack decisions (standalone Asio, system OpenSSL, FetchContent)

### Protocol
- `db/PROTOCOL.md` — Wire format spec, message types, relay-allowed type list

### Existing Patterns
- `db/net/connection.h` — Send queue pattern (deque + drain coroutine + 1024 cap) to use as reference for relay's send queue design
- `db/main.cpp` — Node binary entry point pattern (arg parsing, config loading, signal handling, spdlog setup)
- `relay/identity/relay_identity.h` — ML-DSA-87 relay identity loading (to be copied into new relay tree)

### Build
- `CMakeLists.txt` — Root CMake file with relay references to remove
- `db/CMakeLists.txt` — Contains relay test file references and relay_lib dependency to remove
- `relay/CMakeLists.txt` — Old relay CMake to delete and replace with standalone version

### Requirements
- `.planning/REQUIREMENTS.md` — Phase 100 requirements: CLEAN-01, CLEAN-02, CLEAN-03, SESS-01, SESS-02, OPS-04

</canonical_refs>

<code_context>
## Existing Code Insights

### Reusable Assets
- `relay/identity/relay_identity.h/cpp` — ML-DSA-87 identity loading (~55 lines header). Copy into new relay tree.
- `relay/core/message_filter.h/cpp` — Message type blocklist logic. May be useful as reference for Phase 102, but delete now.

### Established Patterns
- Send queue: `std::deque<PendingMessage>` + drain coroutine + cap + disconnect on overflow (db/net/connection.h:176)
- Signal handling: SIGTERM for shutdown, SIGHUP for config reload (db/main.cpp)
- Config loading: JSON file via nlohmann/json (db/config/config.cpp)
- Structured logging: spdlog with log levels trace|debug|info|warn|error

### Integration Points
- Root CMakeLists.txt line 165: `add_subdirectory(relay)` — must be updated to point to new relay
- Root CMakeLists.txt line 176-177: chromatindb_relay executable target — must be recreated for new relay
- db/CMakeLists.txt lines 247-249, 258: old relay test references — must be removed

</code_context>

<specifics>
## Specific Ideas

- db/ will move to a separate open-source repo after v3.0.0, so relay code must NOT depend on db/ headers or link against db/ libraries. Copy what's needed (relay_identity), don't share.
- Relay must be designed for thousands of concurrent connections (not dozens like the node). This affects send queue cap defaults and memory awareness.
- Build relay here in this repo during v3.0.0, then extract to private repo post-ship. Standalone CMake makes this extraction clean.

</specifics>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 100-cleanup-foundation*
*Context gathered: 2026-04-09*
