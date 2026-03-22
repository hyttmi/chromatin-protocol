# Phase 20: Metrics Completeness & Consistency - Research

**Researched:** 2026-03-13
**Domain:** Operational observability (log output completeness) and shutdown consistency
**Confidence:** HIGH

## Summary

Phase 20 closes the METRICS-LOG-INCOMPLETE gap identified in the v0.4.0 milestone audit. Three `NodeMetrics` counters (`rate_limited`, `peers_connected_total`, `peers_disconnected_total`) are already correctly incremented in the implementation but are never emitted in log output. The fix is a single format-string edit in `log_metrics_line()` (which automatically covers both periodic 60s output and SIGUSR1 dump via the shared call path). A secondary fix adds 4 missing timer cancels to the `on_shutdown_` lambda to match `PeerManager::stop()`. A tertiary cleanup removes the single stale "Phase 18 stub" comment in test code.

All three changes are purely additive or subtractive edits to existing lines -- no new files, no new functions, no architectural changes. The codebase scan confirmed exactly one stale "Phase N stub" comment (at `tests/peer/test_peer_manager.cpp:1416`). No stale phase stubs exist in implementation files (`db/`).

**Primary recommendation:** Single plan with 3 tasks (format string update, timer cancel addition, stale comment removal). Each is independent and verifiable in isolation.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- **Metrics log format**: Semantic grouping -- `connected_total` and `disconnected_total` next to live peer count; `rate_limited` near `rejections`. Shorten field names. Rename existing `connections` to `peers`. Only update `log_metrics_line()` -- SIGUSR1 `dump_metrics()` inherits automatically.
- **Shutdown timer cancellation**: Inline 4 missing timer cancels directly in `on_shutdown_` lambda (not via `stop()` or helper). Place after existing `expiry_timer_` cancel, preserving save-first order. Match `PeerManager::stop()` cancel order: expiry, sync, pex, flush, metrics. Add inline comment: `// Cancel all timers — must match PeerManager::stop()`
- **Stale comment cleanup**: Scan ALL source files (db/ and tests/) for stale "Phase N stub" references. Delete stale comment lines entirely (don't replace). Fix any stale stubs found in implementation files within this phase.

### Claude's Discretion
- Exact position of new fields in the format string (within the semantic grouping constraint)
- Whether to scan for other patterns beyond "Phase.*stub" (e.g., TODO references to completed phases)

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope.
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| OPS-06 | SIGUSR1 dumps current metrics via spdlog (follows sighup_loop coroutine pattern) | `dump_metrics()` calls `log_metrics_line()` -- updating the format string in `log_metrics_line()` automatically covers SIGUSR1 output. No changes to `dump_metrics()` needed. |
| OPS-07 | Metrics logged periodically (60s timer) via spdlog | `metrics_timer_loop()` calls `log_metrics_line()` every 60s. Updating the format string makes all 6 counters observable. |
</phase_requirements>

## Standard Stack

Not applicable -- this phase modifies existing C++20 code only. No new libraries, no new dependencies.

### Existing Code (No Changes to Stack)
| Component | Location | Purpose | Relevance |
|-----------|----------|---------|-----------|
| spdlog | `db/peer/peer_manager.cpp` | Log output | Format string is the edit target |
| Asio steady_timer | `db/peer/peer_manager.h` | Timer-cancel pattern | 5 timer pointers already declared |
| Catch2 | `tests/peer/test_peer_manager.cpp` | Test framework | Stale comment cleanup target |

## Architecture Patterns

### Pattern 1: Shared Log Line Function
**What:** `log_metrics_line()` is a single function called from both `metrics_timer_loop()` (periodic) and `dump_metrics()` (SIGUSR1). Editing once covers both output paths.
**Why it matters:** The CONTEXT.md decision "Only update `log_metrics_line()` -- SIGUSR1 `dump_metrics()` inherits automatically" relies on this architecture.
**Current call graph:**
```
metrics_timer_loop() ──→ log_metrics_line()  ←── dump_metrics() ←── sigusr1_loop()
```

### Pattern 2: Timer-Cancel Pattern
**What:** Each coroutine loop creates a stack-allocated `asio::steady_timer`, stores its address in a member pointer (`*_timer_`), waits, then nulls the pointer. External cancellation checks the pointer before calling `cancel()`.
**Current code (peer_manager.cpp:180-189):**
```cpp
void PeerManager::stop() {
    stopping_ = true;
    sighup_signal_.cancel();
    sigusr1_signal_.cancel();
    if (expiry_timer_) expiry_timer_->cancel();
    if (sync_timer_) sync_timer_->cancel();
    if (pex_timer_) pex_timer_->cancel();
    if (flush_timer_) flush_timer_->cancel();
    if (metrics_timer_) metrics_timer_->cancel();
    server_.stop();
}
```
**Current `on_shutdown_` lambda (peer_manager.cpp:148-154) -- the gap:**
```cpp
server_.set_on_shutdown([this]() {
    stopping_ = true;
    save_persisted_peers();  // Save while connection list is still accurate
    sighup_signal_.cancel();
    sigusr1_signal_.cancel();
    if (expiry_timer_) expiry_timer_->cancel();
    // MISSING: sync_timer_, pex_timer_, flush_timer_, metrics_timer_
});
```

### Pattern 3: Key-Value Log Format
**What:** Metrics line uses `spdlog::info("metrics: key=value key=value ...")` flat format.
**Current format string (peer_manager.cpp:1621-1629):**
```cpp
spdlog::info("metrics: connections={} blobs={} storage={:.1f}MiB "
             "syncs={} ingests={} rejections={} uptime={}",
             peers_.size(),
             blob_count,
             storage_mib,
             metrics_.syncs,
             metrics_.ingests,
             metrics_.rejections,
             uptime);
```
**Required changes per CONTEXT.md decisions:**
1. Rename `connections` to `peers` (live peer count)
2. Add `connected_total` and `disconnected_total` adjacent to `peers`
3. Add `rate_limited` near `rejections`
4. Field names are shortened forms (not full struct member names)

### Anti-Patterns to Avoid
- **Modifying dump_metrics():** The user explicitly locked "Only update `log_metrics_line()`". Do not touch `dump_metrics()`.
- **Extracting a shutdown helper:** The user explicitly locked "Inline the 4 missing timer cancels directly in the `on_shutdown_` lambda (do not call `stop()` or extract a helper)".
- **Replacing stale comments with updated text:** The user explicitly locked "Delete stale comment lines entirely (don't replace with updated text)".

## Don't Hand-Roll

Not applicable -- this phase involves only format string edits and timer cancel additions. No complex problems requiring library solutions.

## Common Pitfalls

### Pitfall 1: Format String Argument Count Mismatch
**What goes wrong:** spdlog format string placeholders (`{}`) don't match the number of arguments, causing runtime crash or garbled output.
**Why it happens:** Adding 3 new fields to the format string requires exactly 3 new arguments in the correct order.
**How to avoid:** Count placeholders and arguments. Current: 7 placeholders, 7 arguments. After: 10 placeholders, 10 arguments.
**Warning signs:** Compilation succeeds but runtime log output shows wrong values or crashes.

### Pitfall 2: Timer Cancel Order Divergence
**What goes wrong:** `on_shutdown_` and `stop()` cancel timers in different orders, making future maintenance confusing.
**Why it happens:** Copy-pasting timer cancels without checking reference order.
**How to avoid:** The CONTEXT.md locks the order: expiry, sync, pex, flush, metrics -- matching `PeerManager::stop()` exactly. Add the inline comment `// Cancel all timers — must match PeerManager::stop()` as specified.

### Pitfall 3: Renaming `connections` in Wrong Place
**What goes wrong:** Renaming the field label `connections` to `peers` could accidentally change the variable name or affect test assertions.
**Why it happens:** Confusion between the log label string and the C++ variable name.
**How to avoid:** Only the string literal label changes (`"connections="` becomes `"peers="`). The C++ expression `peers_.size()` is already the correct variable name and does not change.

## Code Examples

### Updated log_metrics_line() Format String
```cpp
void PeerManager::log_metrics_line() {
    auto storage_bytes = storage_.used_bytes();
    auto storage_mib = static_cast<double>(storage_bytes) / (1024.0 * 1024.0);
    auto uptime = compute_uptime_seconds();

    uint64_t blob_count = 0;
    auto namespaces = storage_.list_namespaces();
    for (const auto& ns : namespaces) {
        blob_count += ns.latest_seq_num;
    }

    spdlog::info("metrics: peers={} connected_total={} disconnected_total={} "
                 "blobs={} storage={:.1f}MiB "
                 "syncs={} ingests={} rejections={} rate_limited={} uptime={}",
                 peers_.size(),
                 metrics_.peers_connected_total,
                 metrics_.peers_disconnected_total,
                 blob_count,
                 storage_mib,
                 metrics_.syncs,
                 metrics_.ingests,
                 metrics_.rejections,
                 metrics_.rate_limited,
                 uptime);
}
```
**Semantic grouping rationale:**
- `peers`, `connected_total`, `disconnected_total` -- peer lifecycle group
- `blobs`, `storage` -- data group
- `syncs`, `ingests`, `rejections`, `rate_limited` -- operation/error group
- `uptime` -- always last (bookend)

### Updated on_shutdown_ Lambda
```cpp
server_.set_on_shutdown([this]() {
    stopping_ = true;
    save_persisted_peers();  // Save while connection list is still accurate
    sighup_signal_.cancel();
    sigusr1_signal_.cancel();
    // Cancel all timers — must match PeerManager::stop()
    if (expiry_timer_) expiry_timer_->cancel();
    if (sync_timer_) sync_timer_->cancel();
    if (pex_timer_) pex_timer_->cancel();
    if (flush_timer_) flush_timer_->cancel();
    if (metrics_timer_) metrics_timer_->cancel();
});
```

### Stale Comment Removal
**Before (test_peer_manager.cpp:1416):**
```cpp
    // rate_limited starts at 0 (Phase 18 stub)
    REQUIRE(pm1.metrics().rate_limited == 0);
```
**After:**
```cpp
    REQUIRE(pm1.metrics().rate_limited == 0);
```

## Codebase Scan Results

### "Phase N stub" Pattern Scan
Scanned all files under `db/` and `tests/` for the pattern `Phase.*stub` (case-insensitive).

| File | Line | Content | Action |
|------|------|---------|--------|
| `tests/peer/test_peer_manager.cpp` | 1416 | `// rate_limited starts at 0 (Phase 18 stub)` | DELETE line |

**Total: 1 stale stub found.** No stale stubs in `db/` implementation files.

### "Phase N" References (Not Stubs)
These are legitimate section comments documenting which phase introduced a feature. They are NOT stubs:

| File | Line | Content | Status |
|------|------|---------|--------|
| `db/peer/peer_manager.h` | 55 | `// Phase 16: Storage capacity signaling...` | Legitimate documentation |
| `db/peer/peer_manager.h` | 57 | `// Phase 18: Token bucket rate limiting...` | Legitimate documentation |
| `db/peer/peer_manager.h` | 265-266 | `// 0 = disabled (Phase 18)` | Legitimate documentation |
| `tests/peer/test_peer_manager.cpp` | 615, 715, 1121, 1206, 1340 | `// Phase N: ...` section headers | Legitimate test section headers |

### TODO/FIXME/HACK Scan
Scanned all files under `db/` and `tests/` -- no instances found. Codebase is clean.

## Open Questions

None. This phase is fully constrained by user decisions and the codebase is well-understood. All edit targets are precisely identified with line numbers.

## Sources

### Primary (HIGH confidence)
- Direct source code inspection of `db/peer/peer_manager.cpp` (lines 148-154, 180-189, 1566-1630)
- Direct source code inspection of `db/peer/peer_manager.h` (lines 64-71, 260-264)
- Direct source code inspection of `tests/peer/test_peer_manager.cpp` (line 1416)
- `.planning/v0.4.0-MILESTONE-AUDIT.md` -- gap definition METRICS-LOG-INCOMPLETE
- `.planning/phases/20-metrics-completeness-consistency/20-CONTEXT.md` -- user decisions

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH -- no new libraries, existing code only
- Architecture: HIGH -- all edit targets verified with line numbers via direct source inspection
- Pitfalls: HIGH -- changes are mechanical (format string, timer cancels, comment deletion)

**Research date:** 2026-03-13
**Valid until:** 2026-04-13 (stable -- no external dependencies)
