# Technology Stack: v0.9.0 Connection Resilience & Hardening

**Project:** chromatindb v0.9.0
**Researched:** 2026-03-19
**Confidence:** HIGH

## Executive Summary

v0.9.0 is a hardening milestone. The core thesis: **zero new dependencies**. Every feature in scope -- connection resilience, structured logging, file logging, config validation, CMake version injection, crash recovery audit, cursor compaction, tombstone GC fix, startup integrity scan -- is achievable with the existing stack. The codebase already has the right libraries; it needs deeper use of their capabilities.

The only recommended change is a **spdlog version bump** from v1.15.1 to v1.15.1+ (staying on v1.15.x is fine; upgrading to v1.16.0 or v1.17.0 is optional and low-risk). spdlog v1.15.1 already supports file sinks, rotating file sinks, multi-sink loggers, and JSON pattern formatting -- all needed for structured/file logging. No API changes needed from a bump.

Connection resilience (auto-reconnect with exponential backoff, keepalive timeout, ACL-aware suppression) uses `asio::steady_timer` -- already the established pattern in the codebase (timer-cancel pattern for sync inbox, expiry timer, sync timer). No new networking primitives needed.

CMake version injection uses `configure_file()` -- a built-in CMake command. No external tooling.

Config validation is pure C++ logic using nlohmann/json (already a dependency). No JSON Schema library needed -- the config struct is simple enough that hand-written validation is clearer, faster, and avoids a new dependency.

libmdbx crash recovery audit is a code review task, not a library addition. The current configuration (`durability::robust_synchronous`, `mode::write_mapped_io`) is the correct default for data integrity. The audit verifies this is used correctly and tests unclean shutdown scenarios.

## Recommended Stack

### No New Dependencies

| Feature | Implementation | Existing Dependency | Why No New Dep |
|---------|---------------|-------------------|---------------|
| Auto-reconnect + backoff | `asio::steady_timer` coroutine loop | Standalone Asio 1.38.0 | Timer-cancel pattern already proven in codebase |
| Keepalive timeout | `asio::steady_timer` per-connection watchdog | Standalone Asio 1.38.0 | Same as sync inbox timer pattern |
| File logging | `spdlog::sinks::rotating_file_sink_mt` | spdlog 1.15.1 | Built-in sink, just needs wiring |
| Structured log format | `spdlog::set_pattern()` with JSON template | spdlog 1.15.1 | Pattern string change, no code dep |
| Config validation | Hand-written checks in `config.cpp` | nlohmann/json 3.11.3 | ~100 LOC of range/type checks beats a schema validator dep |
| CMake version injection | `configure_file()` + `execute_process(git describe)` | CMake 3.20 (built-in) | Standard CMake feature |
| Cursor compaction | Storage API + steady_timer | libmdbx 0.13.11 + Asio | Already have `cleanup_stale_cursors()` and `delete_peer_cursors()` |
| Tombstone GC fix | Debug + fix existing `run_expiry_scan()` | libmdbx 0.13.11 | Bug in existing code, not a missing capability |
| Startup integrity scan | Read transaction scan on open | libmdbx 0.13.11 | Same pattern as existing `validate_no_unencrypted_data()` |
| Crash recovery audit | Code review + test | libmdbx 0.13.11 | Verify existing flags, add kill-9 tests |
| Delegation quota verification | Logic fix in `BlobEngine::ingest()` | Existing code | Ensure delegate writes count against owner namespace quota |
| Timer cleanup | Audit `on_shutdown` paths | Standalone Asio 1.38.0 | Cancel all active timers in drain |
| Metrics logging | Additional `spdlog::info()` calls | spdlog 1.15.1 | Emit counters already tracked in-memory |

### Version Recommendations

| Technology | Current | Recommended | Action | Rationale |
|------------|---------|-------------|--------|-----------|
| spdlog | v1.15.1 | v1.15.1 (keep) or v1.17.0 (optional) | Optional bump | v1.15.1 has all needed features. v1.17.0 brings fmt 12.1.0 bump and minor fixes. Low risk either way. |
| libmdbx | v0.13.11 | v0.13.11 (keep) | No change | Latest stable release as of 2026-01. v0.13.11 is the final open-source release before MithrilDB transition. Pinning is correct. |
| Standalone Asio | 1.38.0 | 1.38.0 (keep) | No change | All needed coroutine/timer features present. |
| nlohmann/json | 3.11.3 | 3.11.3 (keep) | No change | Config parsing works. No new JSON features needed. |
| Catch2 | v3.7.1 | v3.7.1 (keep) | No change | Test framework is stable. |
| All others | Current | Current | No change | liboqs, libsodium, FlatBuffers, xxHash unchanged. |

## Feature-by-Feature Stack Details

### 1. Connection Resilience (Auto-Reconnect + Exponential Backoff)

**Implementation:** Coroutine loop in `PeerManager` using `asio::steady_timer`.

**Pattern:**
```cpp
// Reconnect coroutine per outbound peer
asio::awaitable<void> reconnect_loop(std::string endpoint) {
    uint32_t attempt = 0;
    while (!draining_) {
        auto delay = std::min(base_delay * (1 << attempt), max_delay);
        asio::steady_timer timer(co_await asio::this_coro::executor);
        timer.expires_after(delay);
        co_await timer.async_wait(asio::use_awaitable);
        // Attempt connection...
        if (success) attempt = 0; else ++attempt;
    }
}
```

**Key design points:**
- Base delay: 1s. Max delay cap: 60s. Backoff: exponential (1, 2, 4, 8, 16, 32, 60, 60...).
- ACL-aware suppression: if peer was rejected by ACL (silent TCP close from remote), suppress reconnect entirely (or use very long backoff like 10min). Detection: connection succeeds at TCP level but remote closes immediately after handshake.
- Jitter: add random 0-25% jitter to prevent thundering herd on network partition recovery.
- Reset on successful sync: attempt counter resets to 0 after a successful message exchange, not just TCP connect.
- Cancel on shutdown: the timer is cancelled in `on_shutdown`, breaking the loop.

**No new deps.** `asio::steady_timer` is the same primitive used for sync interval timer and expiry timer.

### 2. Keepalive Timeout (Dead Peer Detection)

**Implementation:** Per-connection `asio::steady_timer` watchdog, reset on any received message.

**Pattern:**
```cpp
// In PeerInfo or connection message loop:
asio::steady_timer keepalive_timer(executor);
keepalive_timer.expires_after(std::chrono::seconds(120));
// On every received message: keepalive_timer.expires_after(120s);
// If timer fires: peer is dead, close connection.
```

**Key design points:**
- Timeout: 120s (2 minutes). Configurable via `keepalive_timeout_seconds` config field.
- No ping/pong protocol messages: the existing sync cycle (default 60s) generates traffic. If no message arrives in 2x the sync interval, peer is dead.
- TCP keepalive as fallback: optionally set `asio::socket_base::keep_alive(true)` on sockets, but application-level timeout is primary.
- Timer reset on ANY received frame (data, sync, PEX, etc.), not just keepalive-specific messages.

**No new deps.** Same timer pattern as existing codebase.

### 3. Structured Log Format

**Implementation:** JSON pattern string in `spdlog::set_pattern()`.

**Current pattern:** `[%Y-%m-%d %H:%M:%S.%e] [%l] [%n] %v`

**Proposed structured pattern (when enabled):**
```
{"ts":"%Y-%m-%dT%H:%M:%S.%fZ","level":"%l","logger":"%n","tid":"%t","msg":"%v"}
```

**Key design points:**
- Config field: `"log_format": "text"` (default) or `"log_format": "json"`. Text format stays human-readable for development; JSON for production/monitoring.
- spdlog's `%f` gives microsecond precision in ISO 8601 format.
- Thread ID (`%t`) included because the codebase uses `asio::thread_pool` for crypto offload.
- No custom formatter class needed -- spdlog's pattern syntax is sufficient for flat JSON lines.
- Escaping: spdlog does NOT JSON-escape the `%v` payload. Messages must not contain unescaped quotes/newlines. Since all log messages are programmer-controlled format strings (not user input), this is acceptable. If a message includes user data (hex pubkeys, addresses), those are already safe ASCII.

**No new deps.** Pattern change only. spdlog v1.15.1 supports this.

### 4. File Logging

**Implementation:** Multi-sink logger with `spdlog::sinks::rotating_file_sink_mt` + existing `stdout_color_sink_mt`.

**Key code change in `logging.cpp`:**
```cpp
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

void init(const std::string& level, const std::string& log_file) {
    auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();

    std::vector<spdlog::sink_ptr> sinks = {console_sink};

    if (!log_file.empty()) {
        // 10 MiB per file, 3 rotated files
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            log_file, 10 * 1024 * 1024, 3);
        sinks.push_back(file_sink);
    }

    auto logger = std::make_shared<spdlog::logger>("chromatindb", sinks.begin(), sinks.end());
    spdlog::set_default_logger(logger);
}
```

**Config fields:**
- `"log_file": ""` (default: empty = no file logging, stderr only)
- `"log_max_size_mb": 10` (max file size before rotation, default 10 MiB)
- `"log_max_files": 3` (number of rotated files to keep)

**Key design points:**
- `rotating_file_sink_mt` (thread-safe) because crypto pool threads may log.
- Rotation prevents unbounded disk usage -- essential for a daemon.
- Both sinks share the same pattern (text or JSON per `log_format`).
- File sink level can match console or be set independently (console=warn, file=debug for production).

**No new deps.** `rotating_file_sink_mt` is built into spdlog. Just needs the include.

### 5. CMake Version Injection

**Implementation:** `configure_file()` with `git describe` in CMakeLists.txt.

**New file: `db/version.h.in` (template):**
```cpp
#pragma once

// Auto-generated by CMake. Do not edit.
#define CHROMATINDB_VERSION "@CHROMATINDB_VERSION@"
#define CHROMATINDB_VERSION_MAJOR @CHROMATINDB_VERSION_MAJOR@
#define CHROMATINDB_VERSION_MINOR @CHROMATINDB_VERSION_MINOR@
#define CHROMATINDB_VERSION_PATCH @CHROMATINDB_VERSION_PATCH@
#define CHROMATINDB_GIT_HASH "@CHROMATINDB_GIT_HASH@"

static constexpr const char* VERSION = CHROMATINDB_VERSION;
```

**CMakeLists.txt additions:**
```cmake
# Version from project() directive
project(chromatindb VERSION 0.9.0 LANGUAGES C CXX)

# Git hash for build identification
execute_process(
    COMMAND git describe --always --dirty
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE CHROMATINDB_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT CHROMATINDB_GIT_HASH)
    set(CHROMATINDB_GIT_HASH "unknown")
endif()

set(CHROMATINDB_VERSION "${PROJECT_VERSION}")
set(CHROMATINDB_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CHROMATINDB_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CHROMATINDB_VERSION_PATCH ${PROJECT_VERSION_PATCH})

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/db/version.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/db/version.h
    @ONLY
)
```

**Key design points:**
- Replace hand-edited `db/version.h` (currently stuck at "0.6.0") with generated file.
- `version.h.in` is the source of truth, `version.h` is generated (add to `.gitignore`).
- `git describe --always --dirty` gives short hash + dirty flag for dev builds.
- `project(VERSION)` in CMake is the canonical version source. Tags match: `v0.9.0`.
- `@ONLY` prevents CMake from expanding `${...}` variables in the template (safety).
- Output to source dir (not build dir) so `#include "db/version.h"` works without include path changes. Alternatively, output to build dir and add it to include paths -- either approach works.

**No new deps.** `configure_file()` and `execute_process()` are built-in CMake commands.

### 6. Config Validation (Fail-Fast)

**Implementation:** Validation function in `config.cpp` called after `load_config()`.

**What to validate:**

| Field | Validation | Current State |
|-------|-----------|---------------|
| `bind_address` | Must contain `:` separator, port must be 1-65535 | No validation |
| `max_peers` | Must be >= 1 | No validation |
| `sync_interval_seconds` | Must be >= 5 (prevent thrashing) | No validation |
| `max_storage_bytes` | 0 or >= 1 MiB (prevent nonsensical tiny limits) | No validation |
| `rate_limit_bytes_per_sec` | 0 or >= 1024 (at least 1 KiB/s if enabled) | No validation |
| `rate_limit_burst` | If rate limiting enabled, burst >= rate (at least 1s worth) | No validation |
| `full_resync_interval` | Must be >= 1 | No validation |
| `cursor_stale_seconds` | Must be >= 60 | No validation |
| `worker_threads` | 0 or 1-256 (sane range) | Clamped at runtime but not validated |
| `sync_cooldown_seconds` | 0 or >= 5 | No validation |
| `max_sync_sessions` | Must be >= 1 | No validation |
| `namespace_quota_bytes` | No constraint (0 = unlimited is valid) | OK |
| `namespace_quota_count` | No constraint (0 = unlimited is valid) | OK |
| `log_level` | Must be one of: trace, debug, info, warn, warning, error, err, critical | Silently defaults to info |
| `log_file` | If non-empty, parent directory must exist or be creatable | New field |
| `log_format` | Must be "text" or "json" | New field |
| `keepalive_timeout_seconds` | 0 (disabled) or >= 30 | New field |

**Approach:** Single `validate_config(const Config&)` function that throws `std::runtime_error` with a human-readable message on the first invalid field. Called in `cmd_run()` immediately after `parse_args()`, before any component construction. Fail fast, fail loud.

**Why NOT json-schema-validator:** That library (pboettch/json-schema-validator) adds a FetchContent dependency, requires JSON Schema draft-7 definition maintenance, and is overkill for ~20 fields with simple range checks. The validation logic is ~100 LOC of `if` statements. YAGNI.

**No new deps.** Pure C++ logic using existing nlohmann/json for type checking.

### 7. libmdbx Crash Recovery Audit

**Current configuration (verified from `storage.cpp:178-179`):**
```cpp
operate_params.mode = mdbx::env::mode::write_mapped_io;
operate_params.durability = mdbx::env::durability::robust_synchronous;
```

**Analysis:**

| Setting | Value | Meaning | Correctness |
|---------|-------|---------|-------------|
| `mode` | `write_mapped_io` | Memory-mapped writes (WRITEMAP) | CORRECT -- fastest write mode that still supports crash safety |
| `durability` | `robust_synchronous` | Full fsync after every commit | CORRECT -- maximum durability. Data survives power loss. |

**libmdbx durability modes (from most to least safe):**

| Mode | Crash Safety | Write Speed | Notes |
|------|-------------|-------------|-------|
| `robust_synchronous` | Full (current) | Slowest | Data always consistent after crash. Never lose committed transactions. |
| `half_synchronous_weak_last` | Good | Medium | May lose last transaction after crash but DB is consistent. |
| `lazy_weak_tail` | OK | Fast | May lose several recent transactions. DB consistent. |
| `whole_fragile` | UNSAFE | Fastest | DB may corrupt on crash. Never use for production. |

**Audit tasks (code review, no deps):**
1. Verify `robust_synchronous` is used everywhere (not overridden).
2. Verify no `MDBX_UTTERLY_NOSYNC` or `MDBX_NOSYNC` flags anywhere.
3. Test: kill -9 during write transaction, verify DB opens cleanly.
4. Test: kill -9 during commit, verify no data corruption.
5. Verify `mdbx::env_managed` destructor is called on graceful shutdown (flushes).
6. Document: libmdbx's copy-on-write B-tree means no WAL needed. Committed transactions are always durable. Uncommitted transactions are lost on crash (expected and correct).

**No new deps.** Code review + test scenarios.

### 8. Startup Integrity Scan

**Implementation:** Read-only transaction scan at startup, after `env_managed` opens.

**What to verify:**
1. All blobs in `blobs_map` decrypt successfully (DARE envelope valid, AEAD tag verifies).
2. All `seq_map` entries point to existing blobs in `blobs_map` (or are zero-hash sentinels for deletions).
3. All `expiry_map` entries reference existing blobs.
4. All `quota_map` aggregates match actual blob data (already done: `rebuild_quota_aggregates()`).
5. `delegation_map` entries reference existing delegation blobs.

**Existing precedent:** `validate_no_unencrypted_data()` already scans `blobs_map` at startup. The integrity scan extends this pattern.

**Key design points:**
- Full scan at startup is acceptable: even at 100K blobs, a read-only cursor scan over keys (not values) completes in milliseconds. Value-level validation (decrypt check) is heavier but runs once.
- Log warnings for inconsistencies rather than aborting (self-healing: delete orphaned index entries).
- Optionally skip with `--skip-integrity-check` CLI flag for fast restart after known-clean shutdown.

**No new deps.** libmdbx read transaction + existing crypto.

### 9. Cursor Compaction

**Implementation:** Periodic `asio::steady_timer` that calls existing `cleanup_stale_cursors()`.

**Existing API (already in storage.h):**
- `cleanup_stale_cursors(known_peer_hashes)` -- deletes cursors for unknown peers.
- `delete_peer_cursors(peer_hash)` -- deletes all cursors for a specific peer.
- `list_cursor_peers()` -- lists all peers with stored cursors.

**What's needed:**
- Timer-driven compaction: every N minutes (e.g., 60), scan cursors. Delete cursors where `last_sync_timestamp` is older than `cursor_stale_seconds` (already a config field, default 3600s).
- On peer disconnect: mark peer as "last seen" time. If peer hasn't reconnected within stale threshold, cursors are eligible for cleanup.

**No new deps.** Combines existing storage API + timer pattern.

### 10. Tombstone GC Fix

**Known issue:** "Tombstone GC not reclaiming storage in Docker benchmarks (storage grows instead of shrinking)."

**Investigation approach (no new deps):**
1. Trace `run_expiry_scan()` execution: is it finding expired tombstones? Are tombstones being stored with TTL > 0?
2. Check if tombstone blobs have their own expiry entries in `expiry_map`.
3. Verify `delete_blob_data()` actually removes blob bytes from `blobs_map` and the mdbx database file shrinks (or at least frees pages internally -- libmdbx may not shrink the file but should reuse pages).
4. Key insight: libmdbx with `write_mapped_io` mode may not physically shrink the database file even after deletions. The freed pages are reused for new writes. This is **expected behavior** for mmap-based databases. The "storage grows" observation may not be a bug -- it's how mmap databases work. The file size represents high-water mark, not current data size.
5. If actual data is not being deleted: trace through tombstone creation -> expiry entry -> expiry scan -> delete_blob_data chain.

**No new deps.** Debugging existing code.

## Alternatives Considered

| Category | Recommended | Alternative | Why Not |
|----------|-------------|-------------|---------|
| Structured logging | spdlog JSON pattern | structlog (spdlog+json fork) | Extra dependency for something spdlog handles natively with a pattern string. |
| Structured logging | spdlog JSON pattern | structured_spdlog (fork) | Unmaintained fork. Not worth the risk. |
| Config validation | Hand-written checks | pboettch/json-schema-validator | New FetchContent dep, JSON Schema draft-7 maintenance, overkill for ~20 fields. |
| Config validation | Hand-written checks | nlohmann/json SAX parser | Over-engineering. Current DOM approach is fine for small configs. |
| File logging | spdlog rotating_file_sink | spdlog daily_file_sink | Daily rotation is less useful for a daemon that may run for months. Size-based rotation is more predictable. |
| File logging | spdlog rotating_file_sink | spdlog basic_file_sink | No rotation = unbounded file growth. Unacceptable for a daemon. |
| Keepalive | Application-level timer | TCP SO_KEEPALIVE | TCP keepalive has OS-level defaults (often 2 hours), not configurable per-socket on all platforms without setsockopt. Application-level timeout is more predictable and testable. |
| Keepalive | Application-level timer | Custom ping/pong wire messages | Requires protocol change (new message types). Sync cycle already generates periodic traffic. Unnecessary complexity. |
| Version injection | CMake configure_file | Compile-time __DATE__/__TIME__ | Not reproducible. Different every build. Can't extract semver from it. |
| Version injection | CMake configure_file | External script generating version.h | Extra build step. CMake handles this natively. |
| Reconnect backoff | Exponential with jitter | Linear backoff | Too slow to recover from brief outages. Exponential reaches steady state faster. |
| Reconnect backoff | Exponential with jitter | Fixed interval | Thundering herd problem when multiple peers recover simultaneously. |

## What NOT to Add

| Technology | Why Not |
|------------|---------|
| JSON Schema validator | Overkill for config validation. Hand-written is clearer. |
| Boost.Log | Entire Boost dependency for something spdlog handles. |
| gRPC/HTTP health checks | Out of scope. Binary protocol only. |
| Prometheus client library | Out of scope for v0.9.0. Metrics are log-based. |
| Custom log framework | spdlog is proven. Don't reinvent. |
| fmt (standalone) | Already bundled inside spdlog. |
| Any new FetchContent dependency | Zero new deps is the goal. |

## Integration Points

### New Config Fields (config.h additions)

```cpp
// Connection resilience
uint32_t keepalive_timeout_seconds = 120;  // 0 = disabled
uint32_t reconnect_base_delay_seconds = 1;
uint32_t reconnect_max_delay_seconds = 60;

// Logging
std::string log_file;                       // Empty = stderr only
std::string log_format = "text";            // "text" or "json"
uint32_t log_max_size_mb = 10;
uint32_t log_max_files = 3;
```

### logging::init() Signature Change

```cpp
// Current:
void init(const std::string& level = "info");

// New:
struct LogConfig {
    std::string level = "info";
    std::string format = "text";   // "text" or "json"
    std::string file;              // empty = no file
    uint32_t max_size_mb = 10;
    uint32_t max_files = 3;
};
void init(const LogConfig& config);
```

### version.h Change

```
// Current (hand-edited, stale at 0.6.0):
#define VERSION_MAJOR "0"
#define VERSION_MINOR "6"
#define VERSION_PATCH "0"

// New (CMake-generated from version.h.in):
#define CHROMATINDB_VERSION "0.9.0"
#define CHROMATINDB_GIT_HASH "acefff8-dirty"
```

## Installation

No changes to installation. No new `FetchContent_Declare` blocks.

```bash
# Existing (unchanged):
cmake -B build
cmake --build build
```

Optional spdlog bump (if desired):
```cmake
# In db/CMakeLists.txt, change:
GIT_TAG v1.15.1
# To:
GIT_TAG v1.17.0
```

## Confidence Assessment

| Area | Confidence | Reason |
|------|-----------|--------|
| No new deps needed | HIGH | All features verified achievable with existing stack. spdlog file sinks, Asio timers, CMake configure_file -- all well-documented, widely used. |
| spdlog JSON pattern | HIGH | Verified via spdlog wiki and issue #1797. Pattern string approach is documented and supported. |
| spdlog file sink | HIGH | `rotating_file_sink_mt` is a core spdlog feature, documented in wiki and examples. |
| CMake version injection | HIGH | `configure_file()` + `execute_process(git describe)` is the standard CMake pattern. Dozens of references confirm. |
| libmdbx crash safety | HIGH | `robust_synchronous` + `write_mapped_io` verified as correct combination from libmdbx README. Copy-on-write B-tree means no WAL needed. |
| libmdbx file size behavior | MEDIUM | mmap databases typically don't shrink files. Need to verify this is the tombstone GC "bug" explanation. May require `mdbx_env_shrink()` or accept as expected behavior. |
| Config validation approach | HIGH | Hand-written validation for ~20 fields is straightforward. No ambiguity. |
| Keepalive without ping/pong | HIGH | Sync cycle generates traffic every 60s. 120s timeout means peer is truly dead if no messages arrive. |

## Sources

- [spdlog Wiki - Sinks](https://github.com/gabime/spdlog/wiki/Sinks)
- [spdlog Wiki - JSON Logging Setup](https://github.com/gabime/spdlog/issues/1797)
- [spdlog v1.15.1 Example Code](https://github.com/gabime/spdlog/blob/v1.x/example/example.cpp)
- [spdlog Releases](https://github.com/gabime/spdlog/releases)
- [libmdbx README - Crash Recovery](https://github.com/erthink/libmdbx/blob/master/README.md)
- [libmdbx GitHub - ACID Guarantees](https://github.com/erthink/libmdbx)
- [CMake Version Injection via Git](https://www.marcusfolkesson.se/blog/git-version-in-cmake/)
- [CMake configure_file Best Practices](https://dev.to/khozaei/automating-semver-with-git-and-cmake-2hji)
- [Asio C++20 Coroutines - Timeout Example](https://beta.boost.org/doc/libs/1_82_0/doc/html/boost_asio/example/cpp20/coroutines/timeout.cpp)
- [Asio 201 - Timeouts and Cancellation](https://cppalliance.org/asio/2023/01/02/Asio201Timeouts.html)
