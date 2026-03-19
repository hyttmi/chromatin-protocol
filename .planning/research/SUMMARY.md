# Project Research Summary

**Project:** chromatindb v0.9.0 — Connection Resilience & Hardening
**Domain:** C++20 async daemon hardening — operational reliability and storage integrity
**Researched:** 2026-03-19
**Confidence:** HIGH

## Executive Summary

v0.9.0 is a pure hardening milestone on a mature codebase. All four research files converge on the same core finding: every feature in scope is achievable with the existing stack — zero new dependencies required. The existing Asio timer patterns, spdlog sink architecture, libmdbx ACID guarantees, and CMake built-ins already provide everything needed. This is a milestone about using existing capabilities more completely, not adding new ones.

The recommended approach falls into four natural phases: (1) infrastructure foundation (version injection, config validation, timer refactoring), (2) storage and logging improvements (structured logs, file sink, cursor compaction, integrity scan), (3) network resilience (keepalive timeout, ACL-aware reconnect), and (4) verification (crash audit, delegation quota tests, docs). This ordering is dependency-driven — config validation must precede all other work because new config fields are needed throughout, and logging must improve before debugging network state machines. The tombstone GC "issue" is likely a measurement problem (mmap file-size semantics), not a real bug.

Key risks are concentrated in Phase 3. Reconnect logic must handle ACL-rejection detection, jitter, and connection-storm prevention after network partitions. The `acl_rejected_` flag pattern and `awaitable operator||` for concurrent coroutines are proven codebase patterns and should be followed exactly. The coroutine lifetime / timer lifetime pitfall (stack-use-after-return on shutdown) is the single most dangerous failure mode and is already mitigated by the existing timer-cancel pattern — new timers must be added to `cancel_all_timers()` to maintain this invariant.

## Key Findings

### Recommended Stack

The existing stack handles everything. No new `FetchContent_Declare` blocks. The only optional action is bumping spdlog from v1.15.1 to v1.17.0 — but v1.15.1 already has all needed capabilities (`rotating_file_sink_mt`, JSON pattern formatting, multi-sink loggers). Keep all other dependencies pinned at current versions. libmdbx v0.13.11 is the final open-source release before MithrilDB transition — staying pinned is correct.

**Core technologies (all existing):**
- Standalone Asio 1.38.0 — `steady_timer` coroutine loops for reconnect and keepalive — timer-cancel pattern already proven throughout codebase
- spdlog 1.15.1 — `rotating_file_sink_mt` + JSON pattern string — built-in feature, needs wiring only
- libmdbx 0.13.11 — `robust_synchronous` + `write_mapped_io` — correct crash-safe config, audit confirms no changes needed
- nlohmann/json 3.11.3 — hand-written `validate_config()` — ~100 LOC beats a schema-validator dependency
- CMake 3.20 — `configure_file()` + `execute_process(git describe)` — fixes stale version.h stuck at 0.6.0

### Expected Features

All features are appropriately sized for a hardening milestone. Nothing should be deferred.

**Must have (table stakes — daemon is not deployable without these):**
- Auto-reconnect on disconnect — peers come and go; permanent peer loss on disconnect is unacceptable
- Dead peer detection (keepalive timeout) — stale connections waste sync slots and block progress
- File logging — operators cannot monitor a daemon that only logs to stderr
- Config validation fail-fast — daemon must reject bad config at startup, not crash 10 minutes later
- Version identification — fix stale version.h (stuck at "0.6.0" for three milestones)

**Should have (operational differentiators):**
- Structured JSON log format — machine-parseable logs for monitoring pipelines
- Startup integrity scan — cross-reference sub-databases on open, self-healing for orphaned entries
- Cursor compaction (age-based) — prevent unbounded cursor storage growth from one-time peers
- Tombstone GC investigation — confirm expected mmap behavior or find real bug
- libmdbx crash recovery audit — empirical kill-9 confirmation of ACID guarantees
- Delegation quota verification — confirm delegate writes correctly count against owner quota
- Complete metrics logging — add missing byte/tombstone counters to periodic log line
- Timer cleanup refactoring — `cancel_all_timers()` as single cancellation point

**Defer (none — all features in scope):**
- Ping/pong protocol messages — sync cycle already provides keepalive traffic, protocol change unnecessary
- Prometheus/gRPC health endpoint — HTTP dependency, attack surface, out of scope
- JSON Schema config validation — overkill for 20 fields with range checks
- Hot config reload for non-ACL fields — SIGHUP already handles ACL; restart is fine for the rest

### Architecture Approach

All changes integrate cleanly into the existing three-tier component model (main.cpp -> Config/Logging/Storage/BlobEngine -> PeerManager -> Server/Connection). The single io_context thread constraint means all new timers must use the established timer-cancel pattern. The key architectural change is adding `heartbeat_loop()` to Connection and racing it against `message_loop()` using `asio::experimental::awaitable_operators::operator||` — the required header is already included in connection.h, making this a low-risk change. Config validation is structured as a separate pass after all config sources merge (file + CLI overrides), before any component construction.

**Major components affected:**
1. `config/` — add `validate_config()` + new fields for logging, keepalive, reconnect
2. `logging/` — multi-sink init (console + rotating file), JSON format switch
3. `net/connection.*` — add `heartbeat_loop()`, `last_activity_` tracking, `acl_rejected_` flag
4. `net/server.*` — check `acl_rejected_` in reconnect_loop, enter extended backoff on ACL pattern
5. `peer/peer_manager.*` — `cancel_all_timers()` extraction, cursor compaction timer, expanded metrics
6. `storage/` — `compact_stale_cursors(age)`, `run_integrity_scan()`, call `rebuild_quota_aggregates()` at startup
7. `CMakeLists.txt` — `project(VERSION 0.9.0)`, `configure_file(version.h.in)`

### Critical Pitfalls

1. **Reconnect storm after network partition** — add 0-25% random jitter to exponential backoff; stagger initial bootstrap connections by 0-2s per peer; sync rate limiter (existing) handles post-connect throttling
2. **Coroutine stack-use-after-return on shutdown** — cancel all timers in `on_shutdown()` before `ioc.stop()`; check `operation_aborted` after every `co_await` timer; use member functions not lambdas for reconnect/keepalive coroutines
3. **ACL-rejected peer infinite reconnect loop** — add `acl_rejected_` flag to Connection; detect pattern (TCP + handshake OK but closes <5s with zero messages); enter 600s extended backoff after 3 consecutive rejects; reset on SIGHUP
4. **File logging silent startup failure** — validate `log_file` parent directory in `validate_config()`; catch `spdlog_ex` in `logging::init()` and fall back to console-only with stderr warning
5. **Tombstone GC misdiagnosis** — check `used_bytes()` via `mdbx_env_info`, not file size; mmap databases reuse freed pages but don't shrink files; only if `used_bytes()` doesn't decrease after GC is there a real bug

## Implications for Roadmap

Based on combined research, a four-phase structure is strongly recommended. The ordering is dependency-driven, not arbitrary.

### Phase 1: Foundation
**Rationale:** Pure infrastructure changes with no behavioral impact. Must exist before adding new config fields or timers. Zero risk of breaking existing functionality.
**Delivers:** Correct version in logs, fail-fast config errors, single timer cancellation point
**Addresses:** Version identification (table stakes), config validation (table stakes), timer cleanup (safety net for phases 2-3)
**Avoids:** Version injection breaking Docker builds (ERROR_QUIET + "unknown" fallback); over-strict validation rejecting benchmark configs (warn, don't reject, for unusual-but-valid values); dual timer cancellation anti-pattern (extract `cancel_all_timers()` first)

### Phase 2: Storage & Logging
**Rationale:** Improves operational tooling before touching the network state machine. Better logs = better debugging of Phase 3. Storage work is fully independent of network changes.
**Delivers:** File + structured logging, cursor compaction, tombstone GC investigation result, startup integrity scan, complete metrics
**Addresses:** File logging (table stakes), structured format (differentiator), cursor compaction (differentiator), metrics (differentiator)
**Avoids:** Log rotation file permissions in Docker (validate path in config, fallback to console on failure); JSON broken by unescaped content (audit: only hex/IP data in logs); cursor compaction deleting active peer cursors (check connected peers before compacting)

### Phase 3: Network Resilience
**Rationale:** Highest-risk changes; benefits from Phase 1 (correct config validation, timer refactoring) and Phase 2 (better logs for debugging). Network state machine changes require the most care.
**Delivers:** Keepalive timeout (dead peer detection via `heartbeat_loop() || message_loop()`), ACL-aware reconnect with exponential backoff + jitter
**Addresses:** Auto-reconnect (table stakes), dead peer detection (table stakes)
**Avoids:** Reconnect storm (jitter + staggered startup); ACL infinite loop (`acl_rejected_` flag + extended backoff); coroutine lifetime pitfall (timer-cancel pattern, member functions not lambdas); keepalive too aggressive for large blob sync (90s timeout > 2x typical 100 MiB transfer time)

### Phase 4: Verification & Documentation
**Rationale:** Validates correctness of all prior phases. Kill-9 tests and delegation quota tests confirm ACID guarantees empirically. Documentation closes the milestone.
**Delivers:** libmdbx crash recovery audit (kill-9 test suite), delegation quota behavior confirmed with tests, updated README/config docs
**Addresses:** libmdbx crash recovery (differentiator), delegation quota verification (differentiator)
**Avoids:** Stale reader slots after kill-9 (verify auto-cleanup on env_open); delegation quota bypass (verify ingest resolves delegate to owner for quota accounting)

### Phase Ordering Rationale

- Config validation and timer cleanup precede everything because new `log_file`, `keepalive_timeout_seconds`, and `reconnect_max_delay_seconds` config fields are needed in Phases 2 and 3, and new timers need `cancel_all_timers()` to be in place
- Logging improvements come before network changes because diagnosing reconnect state machine issues requires good log output from the start
- Network resilience is last among implementation phases because it touches the most complex state machine (Connection/Server/PeerManager interaction) and benefits from all prior tooling
- The tombstone GC investigation belongs in Phase 2 because the startup integrity scan may reveal the root cause, and architecture analysis strongly suggests this is a documentation fix rather than a code fix

### Research Flags

Phases likely needing deeper research during planning:
- **Phase 3 (Network Resilience):** The exact wiring of `acl_rejected_` through the Server callback chain should be traced during plan writing. The architecture file documents the existing bug (handshake_ok set before ACL check), and the fix is clear, but the connection lifecycle state transitions are subtle enough to warrant careful plan-time tracing before writing requirements.

Phases with standard patterns (skip research-phase):
- **Phase 1 (Foundation):** CMake configure_file and C++ validation functions are well-documented standard patterns. HIGH confidence.
- **Phase 2 (Storage & Logging):** spdlog rotating file sinks are a core feature. libmdbx scan patterns follow existing `validate_no_unencrypted_data()` precedent. HIGH confidence.
- **Phase 4 (Verification):** Test writing against documented behavior. No implementation ambiguity.

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | All features verified against existing spdlog, Asio, libmdbx capabilities. Zero new deps confirmed. Source code reviewed directly. |
| Features | HIGH | Feature set is conservative (hardening, not new functionality). All features sized small-medium. Anti-features are well-justified. |
| Architecture | HIGH | Based on direct source code analysis of all affected files. Existing patterns (timer-cancel, awaitable_operators, member function coroutines) are proven in the codebase. |
| Pitfalls | HIGH | Critical pitfalls derived from actual code bugs (reconnect loop ACL issue is a documented existing defect in server.cpp) and well-documented Asio/libmdbx behaviors. |

**Overall confidence:** HIGH

### Gaps to Address

- **Tombstone GC root cause:** Architecture analysis identifies three likely explanations (TTL=0 by design, seq_map sentinel accumulation, mmap file-size semantics). The actual root cause must be confirmed during Phase 2 implementation by adding `used_bytes()` instrumentation. Resolution: document and close if mmap behavior; fix if actual data not being deleted.
- **JSON log escaping:** Architecture recommends a custom `spdlog::custom_flag_formatter` for safe JSON escaping of `%v`. STACK.md says the simple pattern string is sufficient (all log data is hex/IP, already safe ASCII). Decide during Phase 2 implementation — if any log message includes non-safe content, use the custom formatter.
- **Keepalive timeout calibration:** The proposed 90s timeout is well above typical transfer times (100 MiB at 33 blobs/sec = ~3s). Confirm acceptable during Phase 3 plan writing. The configurable `keepalive_timeout_seconds` field means this can be tuned by operators without a code change.

## Sources

### Primary (HIGH confidence)
- Source code: `server.cpp`, `connection.cpp`, `peer_manager.cpp`, `storage.cpp`, `engine.cpp`, `config.cpp`, `logging.cpp`, `main.cpp`, `version.h`, `CMakeLists.txt` — direct analysis of all affected components
- [libmdbx GitHub — ACID and crash recovery](https://github.com/erthink/libmdbx) — durability modes, reader slots, mmap semantics
- [spdlog Wiki — Sinks](https://github.com/gabime/spdlog/wiki/Sinks) — rotating_file_sink_mt, multi-sink loggers
- [CMake configure_file documentation](https://www.marcusfolkesson.se/blog/git-version-in-cmake/) — version injection pattern
- [Asio C++20 Coroutines](https://think-async.com/Asio/asio-1.22.0/doc/asio/overview/core/cpp20_coroutines.html) — awaitable_operators, timer cancellation

### Secondary (MEDIUM confidence)
- [spdlog JSON logging guide](https://github.com/gabime/spdlog/issues/1797) — JSON pattern string approach, escaping behavior
- [Asio Timeouts and Cancellation](https://cppalliance.org/asio/2023/01/02/Asio201Timeouts.html) — reconnect coroutine patterns

### Tertiary (LOW confidence)
- None — all findings verified against source code or primary documentation

---
*Research completed: 2026-03-19*
*Ready for roadmap: yes*
