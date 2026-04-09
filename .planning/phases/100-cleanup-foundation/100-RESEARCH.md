# Phase 100: Cleanup & Foundation - Research

**Researched:** 2026-04-09
**Domain:** Old code deletion, relay binary scaffolding, per-client send queue with C++20 coroutines
**Confidence:** HIGH

## Summary

Phase 100 is a two-part phase: (1) surgical deletion of old relay/ and sdk/python/ directories plus scrubbing all stale references from docs, build files, and test suites, and (2) scaffolding a new relay binary with a per-client Session class featuring a bounded deque-based send queue with drain coroutine -- the same proven pattern from db/net/connection.h but as independent code.

The cleanup scope is well-defined. The old relay consists of 10 source files (relay_main.cpp, 2 config, 2 identity, 2 core, 1 CMakeLists.txt) totaling ~1,500 LOC, plus 4 test files (836 LOC) in db/tests/relay/. The SDK is larger (~4,600 LOC + 656 tests) but is a simple directory deletion. Stale references exist in: root CMakeLists.txt (lines 165-177), db/CMakeLists.txt (lines 247-249 test files + line 258 chromatindb_relay_lib linkage), .gitignore (sdk/python/.venv), dist/install.sh (relay binary and keygen), dist/config/relay.json, dist/systemd/chromatindb-relay.service, and db/PROTOCOL.md (SDK client notes section). The Dockerfile does NOT reference relay.

The scaffold scope is minimal: a main() that loads JSON config, sets up spdlog, handles SIGTERM/SIGHUP, creates a Session class with bounded send queue, and exits cleanly. No WebSocket, no TLS, no accept loop, no UDS -- those are Phases 101-104. The relay's CMakeLists.txt must be standalone (fetch its own Asio, spdlog, nlohmann/json, Catch2 via FetchContent) because db/ will eventually move to a separate repo.

**Primary recommendation:** Execute cleanup first (Plan 100-01), then scaffold (Plan 100-02). The cleanup removes build targets that would otherwise cause compile errors. The scaffold introduces a fresh relay/ directory with standalone CMake and the Session send queue primitive.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **D-01:** Copy relay_identity.h/cpp into the new relay tree (do NOT move or share from db/) -- db/ will move to a separate repo, so relay needs its own copy.
- **D-02:** Delete all old relay test files in db/tests/relay/ (test_relay_config, test_relay_identity, test_message_filter, test_relay_session). Clean break -- old tests aren't useful for the new architecture.
- **D-03:** Scrub ALL references to old relay/SDK from docs -- PROTOCOL.md, db/README.md, .planning/ research docs. Full cleanup, not just directory deletion.
- **D-04:** Remove relay-related entries from root CMakeLists.txt (add_subdirectory(relay), chromatindb_relay executable, chromatindb_relay_lib linkage) and from db/CMakeLists.txt (relay test files, relay_lib dependency).
- **D-05:** Nested subdirectories under relay/: ws/, core/, translate/, config/, identity/, tests/. Mirrors db/ conventions.
- **D-06:** C++ namespace: `chromatindb::relay::` with sub-namespaces matching directories (::ws, ::core, ::translate, ::config, ::identity).
- **D-07:** Relay tests live in relay/tests/ with their own Catch2 test executable. Self-contained, separate from db/ tests.
- **D-08:** Separate send queue implementation from db/net/connection.h -- same proven pattern (deque + drain coroutine) but independent code. No shared code with db/.
- **D-09:** Send queue cap is configurable via relay config, default 256. Lower than db/'s 1024 because relay must support thousands of concurrent WebSocket clients.
- **D-10:** Overflow behavior: disconnect immediately. Log + close connection. Client reconnects and resubscribes. No silent message dropping.
- **D-11:** Relay binary at end of Phase 100: main() loads JSON config (bind addr, UDS path, queue limits), handles SIGTERM/SIGHUP, Session class with send queue compiles. Exits cleanly. No accept loop (Phase 101).
- **D-12:** Standalone CMakeLists.txt for relay/ -- fetches its own deps (Asio, spdlog, nlohmann/json via FetchContent). Can be extracted to a private repo after v3.0.0 ships. Build here during development, move to private repo later.
- **D-13:** spdlog for structured logging, same pattern as db/ node.

### Claude's Discretion
- Config file format details (field names, defaults) -- as long as it's JSON and covers the fields needed.
- Internal class API design for Session and send queue -- as long as it matches the decided pattern.

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|------------------|
| CLEAN-01 | Old relay/ directory deleted | Relay directory has 10 source files, 1 CMakeLists.txt. Straight deletion. relay_identity.h/cpp must be copied first (D-01). |
| CLEAN-02 | Old sdk/python/ directory deleted | SDK has ~4,600 LOC + tests. Clean `rm -rf sdk/`. Also remove .gitignore entry for sdk/python/.venv. |
| CLEAN-03 | Old relay/SDK Docker artifacts, test references, and doc references removed | Stale references identified in 8 locations: root CMakeLists.txt, db/CMakeLists.txt, .gitignore, dist/install.sh, dist/config/relay.json, dist/systemd/chromatindb-relay.service, db/PROTOCOL.md, db/README.md. |
| SESS-01 | Per-client bounded send queue with drain coroutine | Send queue pattern fully documented from db/net/connection.h (lines 170-179, 834-898). Adapted for relay: configurable cap (default 256), independent code, string-based messages. |
| SESS-02 | Backpressure: disconnect slow clients on queue overflow | Overflow behavior: immediate disconnect + log (D-10). Pattern: check size >= cap, log warning, close connection. Same as connection.cpp line 836-841. |
| OPS-04 | Structured logging via spdlog | spdlog 1.15.1 with console + optional rotating file sink. Pattern from db/logging/logging.h/cpp (89 LOC). JSON format support. |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| Standalone Asio | 1.38.0 | io_context, steady_timer, coroutines, signal_set | Same as db/ node. Fetched via OlivierLDff/asio.cmake. Provides C++20 coroutine support. |
| spdlog | 1.15.1 | Structured logging (console + rotating file) | Same as db/ node. Proven pattern in db/logging/. |
| nlohmann/json | 3.11.3 | Config file parsing | Same as db/ node. URL download, lightweight. |
| Catch2 | 3.7.1 | Unit test framework | Same as db/ tests. FetchContent. |

### Supporting (Phase 100 only)
| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| liboqs | 0.15.0 | ML-DSA-87 signing (relay identity -- copied, not used until Phase 102) | Identity keygen/load. Needed by copied relay_identity code. |

### Not Needed in Phase 100
| Library | When Needed | Phase |
|---------|-------------|-------|
| OpenSSL | TLS termination + WS handshake SHA-1/Base64 | Phase 101 |
| FlatBuffers | JSON-to-FlatBuffers translation | Phase 103 |
| libsodium | Random challenge bytes (auth) | Phase 102 |

**Installation:**
```bash
# No new system deps for Phase 100
# All deps fetched via FetchContent in relay/CMakeLists.txt
```

## Architecture Patterns

### Recommended Project Structure (Phase 100 End State)
```
relay/
  CMakeLists.txt              # Standalone, fetches own deps
  relay_main.cpp              # Entry point: config, signals, exit
  config/
    relay_config.h             # RelayConfig struct (JSON fields)
    relay_config.cpp           # JSON config loader
  core/
    session.h                  # Per-client session with send queue
    session.cpp                # Deque + drain coroutine + cap
  identity/
    relay_identity.h           # ML-DSA-87 identity (copied from old relay)
    relay_identity.cpp         # Key load/save/generate
  ws/                          # Empty placeholder (Phase 101)
  translate/                   # Empty placeholder (Phase 103)
  tests/
    CMakeLists.txt             # Catch2 test executable
    test_session.cpp           # Send queue unit tests
    test_relay_config.cpp      # Config loading tests
```

### Pattern 1: Deque + Drain Coroutine Send Queue (from db/net/connection.h)

**What:** A bounded `std::deque` of outbound messages, drained by a single coroutine that serializes all writes. Producers enqueue and await a per-message completion signal. The drain coroutine pops messages, writes them, and signals completion.

**When to use:** Always for per-client outbound message ordering.

**Reference implementation (db/net/connection.h lines 170-179, connection.cpp lines 834-898):**

```cpp
// Phase 100 relay adaptation (independent code, same pattern)
namespace chromatindb::relay::core {

class Session {
public:
    // Configurable cap, default 256 (D-09)
    explicit Session(asio::any_io_executor executor, size_t max_queue = 256);

    // Enqueue a message. Returns false if queue full (triggers disconnect).
    asio::awaitable<bool> enqueue(std::string message);

    // Start drain coroutine (co_spawned alongside session lifecycle).
    asio::awaitable<void> drain_send_queue();

    // Close the session (cancels drain, signals all waiters).
    void close();

    bool is_closed() const;

private:
    struct PendingMessage {
        std::string data;
        asio::steady_timer* completion;  // Owned by enqueue coroutine's stack
        bool* result_ptr;                // Points to local in enqueue
    };

    std::deque<PendingMessage> send_queue_;
    asio::steady_timer send_signal_;     // Timer-cancel wakeup pattern
    size_t max_queue_;
    bool closed_ = false;
};

} // namespace chromatindb::relay::core
```

**Critical details from the proven pattern:**
1. `send_signal_` uses timer-cancel as a wakeup mechanism: producer calls `send_signal_.cancel()` after push; drain coroutine waits on timer with long expiry, wakes on cancel
2. Per-message completion uses a `steady_timer` on the enqueue caller's stack + a `bool*` result pointer. Drain coroutine cancels the timer to signal completion.
3. On write failure, drain loop signals all remaining messages as failed and exits
4. On close, drain loop signals all remaining messages as failed

### Pattern 2: Signal Handling via Asio signal_set

**What:** Use `asio::signal_set` for SIGTERM (clean shutdown) and SIGHUP (config reload). Runs as a coroutine on the io_context.

**Reference (db/main.cpp + PeerManager signal handling):**

```cpp
asio::signal_set signals(ioc, SIGTERM, SIGINT, SIGHUP);
signals.async_wait([&](auto ec, int sig) {
    if (sig == SIGHUP) {
        // Reload config
        spdlog::info("SIGHUP: reloading config");
    } else {
        // SIGTERM/SIGINT: shutdown
        spdlog::info("signal {}: shutting down", sig);
        ioc.stop();
    }
});
```

### Pattern 3: JSON Config Loading

**What:** Load relay config from a JSON file using nlohmann/json. Validate all required fields. Throw on missing/invalid config.

**Reference (relay/config/relay_config.h -- old config struct):**

```cpp
struct RelayConfig {
    std::string bind_address = "0.0.0.0";
    uint32_t bind_port = 4201;
    std::string uds_path;                    // Required
    std::string identity_key_path;           // Required
    std::string log_level = "info";
    std::string log_file;
    uint32_t max_send_queue = 256;           // NEW: per-client send queue cap (D-09)
    // Future phases add: cert_path, key_path, max_connections, rate_limits
};
```

### Anti-Patterns to Avoid
- **Linking chromatindb_lib in relay CMake:** The relay must NOT depend on db/ headers or libraries. It fetches its own deps and has its own copies of needed code. db/ will move to a separate repo.
- **Sharing db/net/connection.h send queue code:** D-08 explicitly requires separate implementation. Copy the pattern, not the code.
- **Using `std::vector` for send_queue_:** Must be `std::deque` -- stable references across push/pop, no invalidation of pointers to existing elements.
- **Silent message dropping on overflow:** D-10 requires immediate disconnect. No lossy queue.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| JSON parsing | Custom parser | nlohmann/json 3.11.3 | Already a project dependency. Proven. |
| Structured logging | Custom log framework | spdlog 1.15.1 | Already used in db/. Console + file rotation built in. |
| Async IO + coroutines | Manual thread management | Standalone Asio 1.38.0 | C++20 coroutines, signal handling, timers all integrated. |
| Test framework | Assertions + main() | Catch2 3.7.1 | Test discovery, sections, generators. Already used in db/. |

**Key insight:** The relay scaffold uses exactly the same infrastructure libraries as the node. The only thing that's different is the application logic. No new deps for Phase 100.

## Common Pitfalls

### Pitfall 1: Relay CMake Linking Against chromatindb_lib
**What goes wrong:** New relay CMakeLists.txt does `target_link_libraries(... chromatindb_lib)` because the old relay did this. Then db/ cannot be extracted to a separate repo without breaking relay builds.
**Why it happens:** Old relay/CMakeLists.txt line 12: `target_link_libraries(chromatindb_relay_lib PUBLIC chromatindb_lib)`. Muscle memory.
**How to avoid:** D-12 is explicit: standalone CMake, fetch own deps. Copy what's needed (relay_identity), don't link db/.
**Warning signs:** Any `#include "db/..."` in new relay code.

### Pitfall 2: Relay Identity Includes db/ Headers
**What goes wrong:** The copied relay_identity.h includes `db/crypto/signing.h`, `db/crypto/hash.h`, and `db/identity/identity.h`. These are db/ headers -- the relay cannot use them.
**Why it happens:** The existing relay_identity.h was written when relay linked chromatindb_lib.
**How to avoid:** When copying relay_identity, rewrite it to use liboqs directly (OQS_SIG_* functions) and implement SHA3-256 via liboqs's SHA3 (oqs/sha3.h). Remove the `to_node_identity()` method (not needed in v2 relay). Keep the same public API shape but with relay-local crypto primitives.
**Warning signs:** Compilation errors about missing db/ headers after relay/ is rebuilt standalone.

### Pitfall 3: Forgetting db/CMakeLists.txt Relay Test References
**What goes wrong:** Old relay/ directory deleted, but db/CMakeLists.txt still lists `tests/relay/test_relay_config.cpp` etc. (lines 247-249) and links `chromatindb_relay_lib` (line 258). Build fails with "file not found."
**Why it happens:** Two CMakeLists.txt files reference old relay (root and db/).
**How to avoid:** D-04 explicitly lists both. Must edit BOTH files.
**Warning signs:** `cmake -S . -B build` fails after relay/ deletion.

### Pitfall 4: Stale dist/ References
**What goes wrong:** dist/install.sh still expects `chromatindb_relay` binary argument, dist/systemd/chromatindb-relay.service points to old binary, dist/config/relay.json has old config format.
**Why it happens:** dist/ is often forgotten during cleanup. D-03 says "ALL references" but developers focus on code files.
**How to avoid:** Search for "relay" in dist/ directory. Update install.sh, systemd service, and config template for new relay binary name and config format.
**Warning signs:** Install script fails on fresh deployment.

### Pitfall 5: Deque Invalidation Across co_await
**What goes wrong:** Storing an iterator or pointer into the deque, calling co_await, then using the iterator. The deque may have been modified by another coroutine during suspension.
**Why it happens:** C++20 coroutines suspend execution at co_await points. Other coroutines on the same strand can modify shared state.
**How to avoid:** The proven pattern uses a `bool*` and `steady_timer*` that are stack-local to the enqueue caller (not pointing into the deque). The drain loop moves the front element out of the deque before any co_await. After co_await, never access any element that was in the deque before suspension.
**Warning signs:** TSAN reports, use-after-free in ASAN, mysterious data corruption.

### Pitfall 6: Send Signal Timer Must Use Long Expiry
**What goes wrong:** Using `send_signal_.expires_at(std::chrono::steady_clock::time_point::max())` which can overflow on some platforms.
**Why it happens:** Trying to create an "infinite" wait.
**How to avoid:** Use `send_signal_.expires_after(std::chrono::hours(24))` like the proven pattern. 24 hours is effectively infinite for a send queue drain loop.
**Warning signs:** Timer fires immediately or overflows.

## Code Examples

### Send Queue Enqueue (from db/net/connection.cpp, lines 834-852)
```cpp
// Source: db/net/connection.cpp
asio::awaitable<bool> Connection::enqueue_send(std::vector<uint8_t> encoded) {
    if (closed_ || closing_) co_return false;
    if (send_queue_.size() >= MAX_SEND_QUEUE) {
        spdlog::warn("send queue full ({} messages), disconnecting {}",
                     MAX_SEND_QUEUE, remote_addr_);
        close();
        co_return false;
    }
    bool result = false;
    asio::steady_timer completion(socket_.get_executor());
    completion.expires_after(std::chrono::hours(24));

    send_queue_.push_back({std::move(encoded), &completion, &result});
    send_signal_.cancel();  // Wake drain coroutine

    auto [ec] = co_await completion.async_wait(use_nothrow);
    // ec == operation_aborted means drain coroutine processed our message
    co_return result;
}
```

### Drain Coroutine (from db/net/connection.cpp, lines 854-898)
```cpp
// Source: db/net/connection.cpp
asio::awaitable<void> Connection::drain_send_queue() {
    auto self = shared_from_this();
    drain_running_ = true;

    while (!closed_) {
        while (!send_queue_.empty() && !closed_) {
            auto msg = std::move(send_queue_.front());
            send_queue_.pop_front();

            bool ok = co_await send_encrypted(msg.encoded);  // Relay: send_ws_text()

            if (msg.result_ptr) *msg.result_ptr = ok;
            if (msg.completion) msg.completion->cancel();

            if (!ok) {
                while (!send_queue_.empty()) {
                    auto& m = send_queue_.front();
                    if (m.result_ptr) *m.result_ptr = false;
                    if (m.completion) m.completion->cancel();
                    send_queue_.pop_front();
                }
                break;
            }
        }

        if (closed_) break;

        send_signal_.expires_after(std::chrono::hours(24));
        auto [ec] = co_await send_signal_.async_wait(use_nothrow);
    }

    while (!send_queue_.empty()) {
        auto& m = send_queue_.front();
        if (m.result_ptr) *m.result_ptr = false;
        if (m.completion) m.completion->cancel();
        send_queue_.pop_front();
    }

    drain_running_ = false;
}
```

### Logging Initialization (from db/logging/logging.cpp, lines 22-64)
```cpp
// Source: db/logging/logging.cpp
// Relay should follow same pattern: shared_sinks, console+file, parse_level
chromatindb::logging::init(config.log_level,
                            config.log_file,
                            config.log_max_size_mb,
                            config.log_max_files,
                            config.log_format);
```

## Cleanup Reference: All Stale References to Remove

| Location | What to Change | Type |
|----------|---------------|------|
| `CMakeLists.txt` line 165 | `add_subdirectory(relay)` -- update to point to new relay or temporarily remove if building relay separately | Build |
| `CMakeLists.txt` lines 176-177 | `add_executable(chromatindb_relay relay/relay_main.cpp)` + link -- rebuild for new relay | Build |
| `db/CMakeLists.txt` lines 247-249 | `tests/relay/test_relay_config.cpp`, `test_relay_identity.cpp`, `test_message_filter.cpp` -- remove | Build |
| `db/CMakeLists.txt` line 258 | `chromatindb_relay_lib` linkage in test target -- remove | Build |
| `.gitignore` line 8 | `sdk/python/.venv` -- remove | Config |
| `dist/install.sh` | References to chromatindb_relay binary, relay keygen, relay.key -- update for new relay | Dist |
| `dist/config/relay.json` | Old config format (uds_path, identity_key_path, log_file) -- replace with new format | Dist |
| `dist/systemd/chromatindb-relay.service` | ExecStart points to old binary with old CLI args -- update | Dist |
| `db/PROTOCOL.md` lines 1020-1050 | "SDK Client Notes" section references Python SDK specifics -- revise to be generic or remove SDK-specific content | Docs |
| `db/README.md` lines 163, 188, 264, 455 | References to relay/UDS client keys/SDK -- revise wording | Docs |
| `db/tests/relay/` directory | 4 test files (836 LOC) -- delete entirely | Tests |

## Relay Identity Copy: Adaptation Needed

The existing `relay/identity/relay_identity.h` includes:
- `db/crypto/signing.h` -- wraps liboqs OQS_SIG (ML-DSA-87)
- `db/crypto/hash.h` -- wraps liboqs SHA3-256
- `db/identity/identity.h` -- NodeIdentity class

For the standalone relay, the copied relay_identity must:
1. Use liboqs directly: `OQS_SIG_new("ML-DSA-87")` for signing, `OQS_SHA3_sha3_256()` for hashing
2. Remove `#include "db/..."` headers entirely
3. Remove `to_node_identity()` method (not needed in v2)
4. Implement its own `Signer` wrapper or inline the liboqs calls
5. Use `SecureBytes` equivalent or `std::vector<uint8_t>` for secret key storage

The relay_identity is ~175 LOC (55 header + 120 impl). The crypto wrapper around liboqs is straightforward -- ML-DSA-87 keypair generation, sign, and verify are direct OQS_SIG_* function calls.

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Old relay links chromatindb_lib | New relay has standalone CMake | Phase 100 | Enables repo separation |
| Old relay uses per-client UDS | New relay uses multiplexed UDS | Phase 100 design | Single UDS connection, request_id routing (Phase 103) |
| Python SDK (sdk/python/) | No SDK -- any WebSocket client works | Phase 100 | Clients only need WebSocket + JSON + liboqs |
| FlatBuffers over TCP for clients | JSON over WebSocket for clients | Phase 100 design | Removes client-side FlatBuffers dependency |

**Deprecated/outdated:**
- relay/core/relay_session.h/cpp -- old per-client UDS session model. Delete.
- relay/core/message_filter.h/cpp -- message type blocklist. Useful concept for Phase 102, but old code not reusable. Delete.
- sdk/python/ -- entire Python SDK. Delete. No backward compat needed (pre-MVP).

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 3.7.1 (FetchContent) |
| Config file | relay/tests/CMakeLists.txt (to be created in Wave 0) |
| Quick run command | `cd build && ctest -R relay -j1 --output-on-failure` |
| Full suite command | `cd build && ctest --output-on-failure` |

### Phase Requirements to Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| CLEAN-01 | Old relay/ directory deleted | smoke | `test ! -d relay_old/` (verified by build succeeding without old relay) | N/A (manual verification) |
| CLEAN-02 | Old sdk/python/ directory deleted | smoke | `test ! -d sdk/python/` | N/A (manual verification) |
| CLEAN-03 | Stale references removed | smoke | `cmake -S . -B build` succeeds, `grep -r 'chromatindb_relay_lib' db/CMakeLists.txt` returns empty | N/A (build verification) |
| SESS-01 | Per-client bounded send queue with drain coroutine | unit | `cd build && ctest -R test_session --output-on-failure` | Wave 0 |
| SESS-02 | Disconnect slow clients on queue overflow | unit | `cd build && ctest -R test_session --output-on-failure` | Wave 0 |
| OPS-04 | Structured logging via spdlog | unit | `cd build && ctest -R test_relay_config --output-on-failure` | Wave 0 |

### Sampling Rate
- **Per task commit:** `cmake -S . -B build && cmake --build build`
- **Per wave merge:** `cd build && ctest --output-on-failure`
- **Phase gate:** Full build + all tests green before /gsd:verify-work

### Wave 0 Gaps
- [ ] `relay/tests/CMakeLists.txt` -- Catch2 test executable for relay tests
- [ ] `relay/tests/test_session.cpp` -- covers SESS-01, SESS-02 (send queue, overflow disconnect)
- [ ] `relay/tests/test_relay_config.cpp` -- covers config loading, validation
- [ ] `relay/CMakeLists.txt` -- standalone FetchContent build (Asio, spdlog, nlohmann/json, Catch2)

## Environment Availability

| Dependency | Required By | Available | Version | Fallback |
|------------|------------|-----------|---------|----------|
| CMake | Build system | Yes | 4.3.1 | -- |
| g++ (C++20) | Compilation | Yes | 15.2.1 | -- |
| Git | FetchContent downloads | Yes | (system) | -- |
| OpenSSL | NOT needed Phase 100 | Yes | 3.6.2 | -- |

**Missing dependencies with no fallback:** None.
**Missing dependencies with fallback:** None.

## Open Questions

1. **relay_identity crypto dependency scope**
   - What we know: relay_identity uses `crypto::Signer` (wraps liboqs ML-DSA-87) and `crypto::sha3_256` (wraps liboqs SHA3). The copy needs its own crypto wrappers.
   - What's unclear: Should the relay implement a minimal Signer class (like db/crypto/signing.h but in relay/crypto/) or use liboqs functions directly in relay_identity.cpp?
   - Recommendation: Create a minimal relay/crypto/ with just the needed wrappers. Keeps relay_identity clean and enables reuse when Phase 102 adds auth verification. liboqs is needed by the relay anyway (for challenge-response auth in Phase 102).

2. **dist/ file updates: now or later?**
   - What we know: dist/install.sh, dist/config/relay.json, and dist/systemd/chromatindb-relay.service reference old relay binary. D-03 says scrub ALL references.
   - What's unclear: Should dist/ be updated now to reference the new relay, or just have old references removed (since the new relay binary name/CLI may change)?
   - Recommendation: Update dist/ to reference the new relay binary. The binary is still called `chromatindb_relay` (same name, different code). Config format changes are minimal for Phase 100 (just add max_send_queue field). Full config update when TLS fields are added in Phase 101.

## Sources

### Primary (HIGH confidence)
- `db/net/connection.h` lines 170-179 -- Send queue data structures (PendingMessage, deque, cap)
- `db/net/connection.cpp` lines 834-898 -- enqueue_send + drain_send_queue implementation
- `relay/identity/relay_identity.h/cpp` -- Source code to be copied and adapted
- `relay/CMakeLists.txt` -- Old relay CMake (to be replaced)
- `CMakeLists.txt` -- Root CMake with relay references (lines 165-177)
- `db/CMakeLists.txt` -- db CMake with relay test references (lines 247-258)
- `db/logging/logging.h/cpp` -- Logging initialization pattern
- `db/main.cpp` -- Node binary entry point pattern
- `.planning/phases/100-cleanup-foundation/100-CONTEXT.md` -- User decisions (D-01 through D-13)
- `.planning/research/ARCHITECTURE.md` -- Relay v2 component layout
- `.planning/research/STACK.md` -- Technology stack decisions

### Secondary (MEDIUM confidence)
- `dist/install.sh`, `dist/config/relay.json`, `dist/systemd/chromatindb-relay.service` -- Stale relay references found via grep

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- all libraries already used in the project, versions verified
- Architecture: HIGH -- directory layout decided in CONTEXT.md, send queue pattern proven in production
- Pitfalls: HIGH -- based on direct source code analysis of existing relay and build system

**Research date:** 2026-04-09
**Valid until:** 2026-05-09 (stable -- no external dependency changes expected)
