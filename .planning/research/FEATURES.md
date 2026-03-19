# Feature Landscape: v0.9.0 Connection Resilience & Hardening

**Domain:** Decentralized database node -- operational hardening
**Researched:** 2026-03-19

## Table Stakes

Features that any production-grade daemon must have. Missing = not deployable.

| Feature | Why Expected | Complexity | Notes |
|---------|--------------|------------|-------|
| Auto-reconnect on disconnect | Peers come and go; network partitions are normal. A daemon that loses peers permanently is useless. | Medium | Exponential backoff + ACL-aware suppression. Core feature. |
| Dead peer detection | Stale connections waste resources and block sync slots. | Low | Keepalive timer, reset on any message. Simple watchdog. |
| File logging | Operators cannot monitor a daemon that only logs to stderr. Logs must survive process restart. | Low | spdlog rotating_file_sink_mt. ~30 LOC change. |
| Config validation | Daemon must reject invalid config at startup, not crash 10 minutes later with a cryptic error. | Low | ~100 LOC of range/type checks. |
| Version identification | Operators must know what version they are running. Log output must include version. | Low | CMake configure_file. Fixes stale version.h (stuck at 0.6.0). |
| Graceful error messages | Config errors, identity file missing, storage corruption -- all must produce human-readable messages, not stack traces. | Low | Mostly exists. Config validation closes the gap. |

## Differentiators

Features that go beyond minimum viable daemon. Valuable for reliability/operations.

| Feature | Value Proposition | Complexity | Notes |
|---------|-------------------|------------|-------|
| Structured log format (JSON) | Machine-parseable logs for monitoring pipelines (ELK, Loki, CloudWatch). | Low | Pattern string change in spdlog. |
| Startup integrity scan | Self-healing: detect and log storage inconsistencies at startup before serving data. | Medium | Read-only scan of all sub-databases. Extends existing encrypted data check. |
| Cursor compaction | Prevent unbounded cursor storage growth from peers that connect once and never return. | Low | Timer + existing cleanup_stale_cursors() API. |
| Tombstone GC fix | Storage reclamation. Without this, disk usage grows monotonically even after deletions. | Medium | Investigation + fix. May be mmap file behavior (not a real bug). |
| libmdbx crash recovery audit | Verified confidence that unclean shutdown cannot corrupt data. | Medium | Code review + kill-9 test suite. No code changes expected. |
| Delegation quota verification | Ensure delegate writes correctly count against owner namespace quota. | Low | Logic check in ingest path. |
| Complete metrics logging | Emit all tracked counters (sync stats, connection stats, storage stats) periodically. | Low | Add spdlog::info calls for metrics already tracked in memory. |
| Timer cleanup on shutdown | Prevent "use after free" on steady_timer objects during drain. | Low | Cancel all timers in on_shutdown. Audit + fix. |

## Anti-Features

Features to explicitly NOT build in v0.9.0.

| Anti-Feature | Why Avoid | What to Do Instead |
|--------------|-----------|-------------------|
| Ping/pong protocol messages | Adds wire protocol complexity. Sync cycle already provides keepalive. Would require new FlatBuffer message type and handlers. | Use application-level timeout based on existing message traffic. |
| Prometheus metrics endpoint | Requires HTTP server dependency and exposes another port. Binary-protocol-only is a core constraint. | Log-based metrics. Operators parse structured logs. |
| gRPC health check API | Same as above: HTTP dependency, attack surface. | Log-based health status + process supervision (systemd, Docker healthcheck via exit code). |
| JSON Schema config validation | New dependency for 20 fields of range checking. | Hand-written validation in config.cpp. |
| Automatic config migration | No previous config format exists to migrate from. Config is still pre-1.0 and may change. | Fail-fast on unknown fields. Document config changes in release notes. |
| Database compaction command | mmap databases don't shrink files; pages are reused. A compaction command would need to copy the entire database. | Accept mmap file size behavior. Document it. |
| Log aggregation integration | Direct integration with ELK/Loki/etc. is an operator concern, not a daemon concern. | Structured JSON logs are the integration point. Operators configure log shipping. |
| Hot config reload of new fields | SIGHUP already reloads ACL. Adding hot reload for logging, keepalive, etc. is scope creep. | Restart daemon for non-ACL config changes. |

## Feature Dependencies

```
Config validation -> All features (must validate before component creation)
Version injection -> Config validation (version in error messages)
File logging -> Config validation (validate log_file path)
Structured logging -> File logging (same sink infrastructure)
Auto-reconnect -> Keepalive timeout (detect dead peers before reconnecting)
Keepalive timeout -> Timer cleanup (new timers need shutdown path)
Cursor compaction -> Timer cleanup (compaction timer needs shutdown path)
Startup integrity scan -> Tombstone GC fix (scan may reveal GC issues)
Delegation quota verification -> (independent, can be done anytime)
Metrics logging -> (independent, can be done anytime)
libmdbx crash recovery audit -> (independent, code review task)
Documentation updates -> All features (document after implementing)
```

## MVP Recommendation

Prioritize (build in this order):

1. **CMake version injection** -- Fixes stale version.h. Zero risk. Enables correct version logging for all subsequent work.
2. **Config validation** -- Fail-fast foundation. Must exist before adding new config fields.
3. **File logging + structured format** -- Operational visibility. Needed to debug everything else.
4. **Keepalive timeout** -- Simple timer watchdog. Prerequisite for reconnect logic.
5. **Auto-reconnect with exponential backoff** -- Core resilience feature. Depends on keepalive.
6. **Timer cleanup** -- Safety net for new timers added above.
7. **Cursor compaction** -- Low-risk, uses existing API.
8. **Tombstone GC investigation** -- May be "not a bug" (mmap behavior).
9. **Startup integrity scan** -- Extends existing pattern.
10. **Delegation quota verification** -- Quick logic check.
11. **Complete metrics logging** -- Quick wins, add counters.
12. **libmdbx crash recovery audit** -- Code review + tests.
13. **Documentation updates** -- Last, after all features stable.

Defer: None. All features are in scope and appropriately sized for the milestone.

## Sources

- [spdlog Wiki - Sinks](https://github.com/gabime/spdlog/wiki/Sinks)
- [libmdbx README](https://github.com/erthink/libmdbx/blob/master/README.md)
- [Asio C++20 Coroutines Timeout Example](https://beta.boost.org/doc/libs/1_82_0/doc/html/boost_asio/example/cpp20/coroutines/timeout.cpp)
