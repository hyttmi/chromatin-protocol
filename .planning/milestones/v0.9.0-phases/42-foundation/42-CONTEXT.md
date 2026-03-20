# Phase 42: Foundation - Context

**Gathered:** 2026-03-19
**Status:** Ready for planning

<domain>
## Phase Boundary

Node starts with correct version identification (CMake-injected, not hardcoded), rejects invalid configuration at startup with human-readable errors, and has a single consistent timer cancellation path for safe shutdown. Three requirements: OPS-01, OPS-02, OPS-06.

</domain>

<decisions>
## Implementation Decisions

### Claude's Discretion

User delegated all decisions for this infrastructure phase. Claude has full flexibility on:

**Version injection (OPS-01):**
- CMake `configure_file()` to generate `version.h` from template, replacing hardcoded `db/version.h`
- Version string format, git hash inclusion for debug builds, and version macro naming
- Whether to embed git short hash (e.g., `0.9.0+abc1234`) or keep it pure semver

**Config validation (OPS-02):**
- Validation strategy: accumulate all errors vs fail-fast (choose based on operator ergonomics)
- Numeric range enforcement for all Config fields (max_peers, sync_interval_seconds, rate limits, quotas, worker_threads, etc.)
- Unknown key handling (warn vs ignore — no new keys should cause hard errors pre-1.0)
- Type mismatch handling (string where int expected, etc.)
- Error message format and output destination (stderr)
- Exit code on validation failure

**Timer cleanup (OPS-06):**
- Extract `cancel_all_timers()` member function from duplicated code in `stop()` and `on_shutdown`
- Both paths call the single method — no behavioral change, pure refactor

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches. User trusts Claude's judgment on all implementation details for this infrastructure phase.

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `db/version.h`: Hardcoded at 0.6.0 — will be replaced by CMake-generated header
- `db/config/config.h`: Config struct with 20+ fields, all with defaults
- `db/config/config.cpp`: `load_config()` already validates `allowed_keys` (hex format), `trusted_peers` (IP format), and `namespace_quotas` keys — extend this pattern for numeric fields
- `validate_allowed_keys()` and `validate_trusted_peers()` — existing validation function pattern to follow

### Established Patterns
- Config loading: nlohmann/json `j.value()` with defaults, then explicit validation functions
- Error reporting: `throw std::runtime_error()` with descriptive messages
- Timer ownership: `std::unique_ptr<asio::steady_timer>` members, nullable, cancel via `->cancel()`
- spdlog for all logging output

### Integration Points
- `db/main.cpp:35`: Uses `VERSION` constant in `print_usage()` and `cmd_version()`
- `db/peer/peer_manager.cpp:198-208`: `on_shutdown` callback cancels 5 timers (expiry, sync, pex, flush, metrics)
- `db/peer/peer_manager.cpp:235-244`: `stop()` cancels same 5 timers — duplication target
- `db/CMakeLists.txt`: Build system entry point for version injection

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 42-foundation*
*Context gathered: 2026-03-19*
