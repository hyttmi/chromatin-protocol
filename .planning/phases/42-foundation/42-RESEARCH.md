# Phase 42: Foundation - Research

**Researched:** 2026-03-19
**Domain:** CMake build system, config validation, timer management (C++20/Asio)
**Confidence:** HIGH

## Summary

Phase 42 covers three independent infrastructure tasks: CMake version injection (OPS-01), config validation at startup (OPS-02), and timer cancellation consolidation (OPS-06). All three use the existing stack with zero new dependencies. The codebase is well-understood from 41 prior phases.

The version injection replaces a hardcoded `db/version.h` (stuck at "0.6.0" since v0.7.0) with a CMake `configure_file()` generated header. Config validation adds a `validate_config()` function to catch invalid numeric ranges, type mismatches, and bad log levels before any component construction. Timer cleanup extracts a `cancel_all_timers()` method from duplicated code in `stop()` and `on_shutdown` (lines 198-208 and 235-244 of peer_manager.cpp).

**Primary recommendation:** Implement as three independent tasks. Version injection touches CMake + version.h + main.cpp. Config validation touches config.h/config.cpp + main.cpp + tests. Timer cleanup touches peer_manager.h/peer_manager.cpp only.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
None -- all decisions delegated to Claude's discretion.

### Claude's Discretion
User delegated all decisions for this infrastructure phase. Claude has full flexibility on:

**Version injection (OPS-01):**
- CMake `configure_file()` to generate `version.h` from template, replacing hardcoded `db/version.h`
- Version string format, git hash inclusion for debug builds, and version macro naming
- Whether to embed git short hash (e.g., `0.9.0+abc1234`) or keep it pure semver

**Config validation (OPS-02):**
- Validation strategy: accumulate all errors vs fail-fast (choose based on operator ergonomics)
- Numeric range enforcement for all Config fields (max_peers, sync_interval_seconds, rate limits, quotas, worker_threads, etc.)
- Unknown key handling (warn vs ignore -- no new keys should cause hard errors pre-1.0)
- Type mismatch handling (string where int expected, etc.)
- Error message format and output destination (stderr)
- Exit code on validation failure

**Timer cleanup (OPS-06):**
- Extract `cancel_all_timers()` member function from duplicated code in `stop()` and `on_shutdown`
- Both paths call the single method -- no behavioral change, pure refactor

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| OPS-01 | Version string injected by CMake at build time (version.h removed, no manual version bumps) | CMake `configure_file()` with `version.h.in` template, `project(VERSION 0.9.0)`, git hash via `execute_process()` |
| OPS-02 | Node rejects invalid config at startup with human-readable error messages (ranges, types, formats) | New `validate_config()` function, field-by-field range checks, called after `parse_args()` before component construction |
| OPS-06 | All timers cancelled consistently in both stop() and on_shutdown paths | Extract `cancel_all_timers()` private method, replace duplicate code at lines 198-208 and 235-244 |
</phase_requirements>

## Standard Stack

### Core
| Library | Version | Purpose | Why Standard |
|---------|---------|---------|--------------|
| CMake | 3.20+ | Build system, version injection | Already used, `configure_file()` is built-in |
| nlohmann/json | 3.11.3 | Config parsing, type checking | Already used, `value()` throws `type_error` on mismatch |
| spdlog | 1.15.1 | Logging (startup messages, version output) | Already used |
| Asio | 1.38.0 | Timer management (steady_timer) | Already used |
| Catch2 | 3.7.1 | Testing | Already used |

### Supporting
No new libraries needed. All three tasks use existing infrastructure.

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| configure_file | cmake_string(CONFIGURE) | CMake 3.26+ only, configure_file is better established |
| Hand-written validation | pboettch/json-schema-validator | New dependency for 20 fields of range checks -- YAGNI |

**Installation:**
No new dependencies.

## Architecture Patterns

### Recommended Changes
```
db/
  version.h.in          # NEW: CMake template (replaces version.h)
  version.h             # GENERATED: add to .gitignore
  config/
    config.h            # ADD: validate_config() declaration
    config.cpp          # ADD: validate_config() implementation
  peer/
    peer_manager.h      # ADD: cancel_all_timers() private declaration
    peer_manager.cpp    # REFACTOR: extract cancel_all_timers()
  main.cpp              # MODIFY: call validate_config() after parse_args()
  CMakeLists.txt        # MODIFY: add configure_file(), VERSION in project()
CMakeLists.txt          # MODIFY: add VERSION to project() directive
```

### Pattern 1: CMake Version Injection
**What:** Replace hardcoded `db/version.h` with CMake `configure_file()` generated header.
**When to use:** Any time a build-time constant needs to be embedded in source code.

Template file `db/version.h.in`:
```cpp
#pragma once

#define CHROMATINDB_VERSION "@CHROMATINDB_VERSION@"
#define CHROMATINDB_VERSION_MAJOR @CHROMATINDB_VERSION_MAJOR@
#define CHROMATINDB_VERSION_MINOR @CHROMATINDB_VERSION_MINOR@
#define CHROMATINDB_VERSION_PATCH @CHROMATINDB_VERSION_PATCH@
#define CHROMATINDB_GIT_HASH "@CHROMATINDB_GIT_HASH@"

static constexpr const char* VERSION = CHROMATINDB_VERSION;
```

CMake additions (in root `CMakeLists.txt`):
```cmake
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
```

In `db/CMakeLists.txt`:
```cmake
project(chromatindb-core VERSION 0.9.0 LANGUAGES C CXX)

configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/version.h.in
    ${CMAKE_CURRENT_SOURCE_DIR}/version.h
    @ONLY
)
```

**Key decisions:**
- Output generated `version.h` to source dir so `#include "db/version.h"` resolves without include path changes. main.cpp does not need modification of the include line.
- `@ONLY` prevents CMake from expanding `${...}` in the template (safety).
- `ERROR_QUIET` on `git describe` so Docker builds without `.git` dir still work (falls back to "unknown").
- The `VERSION` constant name stays the same -- main.cpp uses `VERSION` in `print_usage()` and `cmd_version()`, no change needed.
- Add `db/version.h` to `.gitignore` since it is now generated.

### Pattern 2: Config Validation (Fail-Fast with Accumulation)
**What:** Validate all config fields after loading, accumulate all errors, print all at once.
**When to use:** Startup validation where the operator benefits from seeing all problems at once.

```cpp
// In config.h
void validate_config(const Config& cfg);

// In config.cpp
void validate_config(const Config& cfg) {
    std::vector<std::string> errors;

    if (cfg.max_peers == 0) {
        errors.push_back("max_peers must be >= 1 (got 0)");
    }
    if (cfg.sync_interval_seconds == 0) {
        errors.push_back("sync_interval_seconds must be >= 1 (got 0)");
    }
    // ... all fields ...

    if (!errors.empty()) {
        std::string msg = "Configuration errors:\n";
        for (const auto& e : errors) {
            msg += "  - " + e + "\n";
        }
        throw std::runtime_error(msg);
    }
}
```

**Why accumulate, not fail-fast:** An operator fixing config errors benefits from seeing ALL problems at once, rather than fixing one, restarting, finding the next. This is a startup-only operation with ~20 cheap checks -- no performance concern.

**Validation table (all numeric Config fields):**

| Field | Type | Constraint | Rationale |
|-------|------|-----------|-----------|
| `max_peers` | uint32_t | >= 1 | 0 peers = node can never sync |
| `sync_interval_seconds` | uint32_t | >= 1 | 0 = infinite spin |
| `max_storage_bytes` | uint64_t | 0 or >= 1048576 (1 MiB) | Tiny limits cause immediate rejection of any blob |
| `rate_limit_bytes_per_sec` | uint64_t | 0 or >= 1024 | < 1 KiB/s effectively disables all transfers |
| `rate_limit_burst` | uint64_t | If rate_limit_bytes_per_sec > 0: must be >= rate_limit_bytes_per_sec | Burst < rate means every transfer is throttled |
| `full_resync_interval` | uint32_t | >= 1 | 0 = never resync |
| `cursor_stale_seconds` | uint64_t | >= 60 | < 60s = cursors expire before first sync |
| `worker_threads` | uint32_t | 0 or 1-256 | > 256 is almost certainly a config typo |
| `sync_cooldown_seconds` | uint32_t | no constraint (0 = disabled is valid) | All values reasonable |
| `max_sync_sessions` | uint32_t | >= 1 | 0 = reject all syncs |
| `log_level` | string | one of: trace, debug, info, warn, warning, error, err, critical | Invalid level silently defaults to info -- should be caught |
| `bind_address` | string | must contain `:`, port part must be 1-65535 | Invalid bind = startup crash |

**Type mismatch handling:** The nlohmann/json `j.value()` method already throws `nlohmann::json::type_error` (error code 302) when a key exists but has the wrong type (e.g., string where int expected). Currently this propagates as an opaque library exception. The solution: wrap `load_config()` to catch `nlohmann::json::type_error` and rethrow as `std::runtime_error` with a human-readable message that includes the field name and expected type.

**Unknown key handling:** Warn on stderr but do not reject. Pre-1.0, config format may change and operators may have forward-looking config. Use `j.items()` to iterate all keys and check against a known-key set.

### Pattern 3: Timer Cancellation Consolidation
**What:** Extract duplicated timer cancellation into a single `cancel_all_timers()` method.
**When to use:** Any time the same cancel sequence appears in multiple places.

```cpp
// In peer_manager.h (private)
void cancel_all_timers();

// In peer_manager.cpp
void PeerManager::cancel_all_timers() {
    if (expiry_timer_) expiry_timer_->cancel();
    if (sync_timer_) sync_timer_->cancel();
    if (pex_timer_) pex_timer_->cancel();
    if (flush_timer_) flush_timer_->cancel();
    if (metrics_timer_) metrics_timer_->cancel();
}
```

Both `stop()` and the `on_shutdown` lambda call `cancel_all_timers()` instead of the duplicated 5-line sequence.

**Critical:** This is a pure refactor. No behavioral change. The same 5 timers are cancelled in the same order. The `stopping_ = true` and `sighup_signal_.cancel()` / `sigusr1_signal_.cancel()` remain separate because they serve different purposes (signals vs timers) and adding more timers in Phase 44 (keepalive) should only need to add a line to `cancel_all_timers()`.

### Anti-Patterns to Avoid
- **Generating version.h to build dir without adding include path:** The `#include "db/version.h"` in main.cpp must resolve. Either generate to source dir (simpler) or add build dir to include paths (cleaner but more CMake changes).
- **Fail-fast config validation that stops at first error:** Operators have to restart N times to find N errors. Accumulate and report all at once.
- **Validating config fields that have no meaningful constraint:** `namespace_quota_bytes` and `namespace_quota_count` accept any value including 0 (unlimited). Do not add artificial constraints.
- **Throwing from inside `on_shutdown` lambda:** The lambda runs on the io_context thread. Exceptions there are unrecoverable. `cancel_all_timers()` must not throw.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| JSON Schema validation | Custom schema validator | ~100 LOC of `if` statements in `validate_config()` | 20 fields with simple range checks. Schema validator is a new dependency for no benefit |
| Version parsing from git tags | Custom tag parser | CMake `project(VERSION X.Y.Z)` | CMake splits into MAJOR/MINOR/PATCH automatically |
| Git hash embedding | Manual echo/sed scripts | CMake `execute_process(COMMAND git describe)` | Built-in, cross-platform, handles missing .git gracefully |

## Common Pitfalls

### Pitfall 1: Version Injection Breaks Docker Builds
**What goes wrong:** `execute_process(git describe)` fails when building from tarball or Docker `COPY` (no `.git` directory). CMake configure fails entirely.
**Why it happens:** `git describe` requires a git repository.
**How to avoid:** Use `ERROR_QUIET` on the `execute_process` call. Fallback: `if(NOT CHROMATINDB_GIT_HASH) set(CHROMATINDB_GIT_HASH "unknown") endif()`. The version number comes from `project(VERSION)`, not git. Git hash is supplementary.
**Warning signs:** Docker build fails at CMake configure step.

### Pitfall 2: configure_file Only Runs at Configure Time
**What goes wrong:** Developer changes version in CMakeLists.txt, runs `cmake --build .` without re-running cmake configure. Version.h is stale.
**Why it happens:** `configure_file()` runs during cmake configure, not during build.
**How to avoid:** This is acceptable for version numbers (which rarely change mid-development). The `git describe` hash updates on every configure. For guaranteed freshness, could use a custom command, but it adds complexity for no practical benefit.
**Warning signs:** `chromatindb version` shows old version after bumping CMakeLists.txt.

### Pitfall 3: nlohmann/json type_error Leaks Through
**What goes wrong:** Config file has `"max_peers": "thirty-two"` (string instead of int). `j.value("max_peers", cfg.max_peers)` throws `nlohmann::json::type_error` with message like `[json.exception.type_error.302] type must be number, but is string`. The error message is technically correct but does not tell the operator WHICH config field or WHAT to do.
**Why it happens:** `j.value()` returns the default when the key is MISSING, but throws when the key EXISTS with the wrong type.
**How to avoid:** Wrap the `j.value()` calls in a try-catch for `nlohmann::json::type_error`, catching the field name from context and rethrowing as `std::runtime_error` with `"config field 'max_peers': expected integer, got string"`.
**Warning signs:** Test with string-typed numeric fields.

### Pitfall 4: Config Validation Rejects Valid Benchmark Configs
**What goes wrong:** Benchmark uses `sync_interval_seconds: 1` for fast sync. Validation rejects it because range check requires >= 5.
**Why it happens:** Overly strict validation ranges designed for "production" but applied universally.
**How to avoid:** Set minimums to the lowest operationally valid value, not the lowest "recommended" value. `sync_interval_seconds >= 1` (not >= 5). Use `spdlog::warn()` for values below recommended ranges (e.g., < 5) but do not reject.
**Warning signs:** Existing tests fail because they use low sync intervals (e.g., `sync_interval_seconds = 1` in test_daemon.cpp E2E tests).

### Pitfall 5: Generated version.h Committed to Git
**What goes wrong:** Developer runs cmake configure, `version.h` is generated in source tree, gets committed. Now `version.h` is both tracked and generated. Future cmake runs overwrite it, causing dirty tree.
**How to avoid:** Add `db/version.h` to `.gitignore` and `git rm --cached db/version.h` to stop tracking it. Delete the current hardcoded file.
**Warning signs:** `git status` shows `version.h` as modified after cmake configure.

## Code Examples

### Current version.h (to be replaced)
```cpp
// Source: db/version.h (current, hardcoded at 0.6.0)
#define VERSION_MAJOR "0"
#define VERSION_MINOR "6"
#define VERSION_PATCH "0"

static constexpr const char* VERSION = VERSION_MAJOR "." VERSION_MINOR "." VERSION_PATCH;
```

### Current timer duplication (to be consolidated)
```cpp
// Source: db/peer/peer_manager.cpp lines 198-208 (on_shutdown lambda)
if (expiry_timer_) expiry_timer_->cancel();
if (sync_timer_) sync_timer_->cancel();
if (pex_timer_) pex_timer_->cancel();
if (flush_timer_) flush_timer_->cancel();
if (metrics_timer_) metrics_timer_->cancel();

// Source: db/peer/peer_manager.cpp lines 239-243 (stop())
if (expiry_timer_) expiry_timer_->cancel();
if (sync_timer_) sync_timer_->cancel();
if (pex_timer_) pex_timer_->cancel();
if (flush_timer_) flush_timer_->cancel();
if (metrics_timer_) metrics_timer_->cancel();
```

### Current config loading (to be extended with validation)
```cpp
// Source: db/config/config.cpp -- j.value() pattern
cfg.max_peers = j.value("max_peers", cfg.max_peers);
cfg.sync_interval_seconds = j.value("sync_interval_seconds", cfg.sync_interval_seconds);
// ... 14 more fields using same pattern
```

### main.cpp version usage (no change needed to include)
```cpp
// Source: db/main.cpp lines 35, 51, 103
std::cerr << "chromatindb " << VERSION << "\n\n"  // print_usage()
std::cout << "chromatindb " << VERSION << std::endl;  // cmd_version()
spdlog::info("chromatindb {}", VERSION);  // cmd_run()
```

### Existing validation pattern to follow
```cpp
// Source: db/config/config.cpp -- validate_allowed_keys()
void validate_allowed_keys(const std::vector<std::string>& keys) {
    for (const auto& key : keys) {
        if (key.size() != 64) {
            throw std::runtime_error(
                "Invalid allowed_key '" + key + "': expected 64 hex characters, got " +
                std::to_string(key.size()));
        }
        // ... hex character validation
    }
}
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Hardcoded version.h | CMake configure_file() | Standard practice | Version always correct at build time |
| Silent config defaults | Fail-fast validation | Industry standard | Operators catch misconfig before runtime |
| Duplicated cancel code | Single cancel_all_timers() | DRY principle | New timers (keepalive in Phase 44) only need one line |

**Deprecated/outdated:**
- `db/version.h` (hardcoded): Stuck at "0.6.0" since v0.7.0. To be replaced by generated file.

## Open Questions

None. All three tasks are straightforward with clear implementation paths. The project research (STACK.md, ARCHITECTURE.md, PITFALLS.md) already covers the detailed design.

## Validation Architecture

### Test Framework
| Property | Value |
|----------|-------|
| Framework | Catch2 v3.7.1 |
| Config file | db/CMakeLists.txt (lines 196-238) |
| Quick run command | `cd build && ctest --test-dir db -R "config" --output-on-failure` |
| Full suite command | `cd build && ctest --test-dir db --output-on-failure` |

### Phase Requirements -> Test Map
| Req ID | Behavior | Test Type | Automated Command | File Exists? |
|--------|----------|-----------|-------------------|-------------|
| OPS-01 | Version string matches CMake project version | unit | `cd build && ctest --test-dir db -R "version" --output-on-failure` | No -- Wave 0 |
| OPS-02 | Invalid config rejected with error message | unit | `cd build && ctest --test-dir db -R "config" --output-on-failure` | Partial -- db/tests/config/test_config.cpp exists but has no range validation tests |
| OPS-02 | Type mismatch caught with readable error | unit | `cd build && ctest --test-dir db -R "config" --output-on-failure` | No -- Wave 0 |
| OPS-06 | cancel_all_timers() called in stop() and on_shutdown | integration | `cd build && ctest --test-dir db -R "daemon" --output-on-failure` | Partial -- test_daemon.cpp tests start/stop but does not explicitly verify timer cancel path |

### Sampling Rate
- **Per task commit:** `cd build && ctest --test-dir db -R "config|version|daemon" --output-on-failure`
- **Per wave merge:** `cd build && ctest --test-dir db --output-on-failure`
- **Phase gate:** Full suite green before `/gsd:verify-work`

### Wave 0 Gaps
- [ ] New tests in `db/tests/config/test_config.cpp` -- covers OPS-02 (range validation, type mismatch, unknown key warning)
- [ ] Version test (either in test_config.cpp or a new test file) -- covers OPS-01 (version macro defined, non-empty)
- [ ] OPS-06 is a pure refactor of existing code -- existing E2E tests in test_daemon.cpp (start/stop/sync) cover this implicitly. No new test file needed.

*(Existing test infrastructure covers timer cancel behavior through E2E daemon tests. Config test file exists and will be extended.)*

## Sources

### Primary (HIGH confidence)
- Codebase analysis: `db/version.h`, `db/config/config.h`, `db/config/config.cpp`, `db/main.cpp`, `db/peer/peer_manager.h`, `db/peer/peer_manager.cpp`, `CMakeLists.txt`, `db/CMakeLists.txt`
- Project research: `.planning/research/STACK.md` (version injection design, config validation table), `.planning/research/ARCHITECTURE.md` (integration approach), `.planning/research/PITFALLS.md` (version injection Docker pitfall, config validation pitfall)
- [CMake configure_file documentation](https://cmake.org/cmake/help/latest/command/configure_file.html)
- [nlohmann/json type_error documentation](https://json.nlohmann.me/api/basic_json/type_error/)

### Secondary (MEDIUM confidence)
- [nlohmann/json value() type mismatch behavior](https://github.com/nlohmann/json/issues/278) -- confirmed: `value()` throws `type_error` (302) when key exists with wrong type, returns default when key is missing
- [CMake configure_file best practices](https://cliutils.gitlab.io/modern-cmake/chapters/basics/comms.html) -- use `@ONLY` flag, `execute_process` for git hash

### Tertiary (LOW confidence)
None -- all findings verified against codebase and official documentation.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- zero new dependencies, all existing tools
- Architecture: HIGH -- patterns verified in codebase, project research already documents exact approach
- Pitfalls: HIGH -- verified against actual code (Docker build, test configs, nlohmann type behavior)

**Research date:** 2026-03-19
**Valid until:** 2026-04-19 (stable domain, no moving parts)
