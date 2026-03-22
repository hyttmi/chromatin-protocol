# Phase 46: Sanitizers & Bug Fix - Context

**Gathered:** 2026-03-20
**Status:** Ready for planning

<domain>
## Phase Boundary

All existing Catch2 unit tests pass under ASAN, TSAN, and UBSAN with zero findings in our code, and the pre-existing PEX coroutine SIGSEGV (test_daemon.cpp:296) is resolved. This is local build/test work — Docker integration tests are separate phases (47-52).

</domain>

<decisions>
## Implementation Decisions

### Bug discovery and fix scope
- Fix ALL sanitizer findings in our code (db/) — zero findings means zero findings
- Third-party dep findings (liboqs, libmdbx, etc.) are suppressed — upstream's problem. Only our code must be clean
- If a fix requires significant refactor (e.g., coroutine lifetime redesign), do the proper fix — no band-aids, no minimal patches. Fix the root cause even if it takes longer

### CMake sanitizer API
- Replace existing `ENABLE_ASAN` with a single `SANITIZER` enum option: `cmake -DSANITIZER=asan|tsan|ubsan`
- Prevents accidentally enabling incompatible sanitizers (ASAN + TSAN are mutually exclusive)
- Sanitizer flags stay in top-level CMakeLists.txt (not in db/ — per existing key decision)

### PEX fix reliability bar
- PEX test must pass 50 consecutive runs (not 10 as originally in success criteria) AND pass under TSAN with zero findings
- All [daemon][e2e] tagged tests get the same 50-run reliability check, not just PEX
- Any flaky test discovered during the 50-run validation blocks this phase — gets fixed here before moving on

### Validation approach
- Scripted validation: bash script that runs E2E tests 50 times with pass/fail tracking and reporting
- Reusable for future phases

### Claude's Discretion
- Suppression file format and organization for third-party findings
- Specific sanitizer compiler flags beyond the standard -fsanitize= (e.g., -fno-omit-frame-pointer, detect_leaks)
- Order of sanitizer runs (ASAN first, TSAN, UBSAN — or parallel)
- PEX SIGSEGV fix approach (coroutine lifetime fix strategy)

</decisions>

<code_context>
## Existing Code Insights

### Reusable Assets
- `ENABLE_ASAN` already exists in top-level CMakeLists.txt (lines 28-32) — will be replaced by `SANITIZER` option
- Catch2 test suite: 408+ tests across db/tests/

### Established Patterns
- Sanitizer flags are top-level consumer concerns, not in db/CMakeLists.txt
- `CMAKE_BUILD_TYPE Debug` is hardcoded (line 23) — good for sanitizer runs
- FetchContent for all deps — sanitizer flags propagate to deps automatically via add_compile_options

### Integration Points
- PEX test at db/tests/test_daemon.cpp:296 — the known SIGSEGV target
- PeerManager coroutine lifecycle (start/stop/teardown) — likely root cause area
- All [daemon][e2e] tests share the same async teardown pattern (pm.stop() + ioc.run_for)

</code_context>

<specifics>
## Specific Ideas

- User has KVM capability for running nodes — useful for Docker integration test phases (47-52), not needed for this phase
- "50 + tsan for sure" — user wants high confidence on coroutine-related fixes
- Proper fixes, not patches — reflects project-wide "no shortcuts" philosophy

</specifics>

<deferred>
## Deferred Ideas

- KVM-based node testing (SSH to real VMs instead of Docker containers) — consider for Phases 47-52 integration tests
- CI/CD pipeline for automated sanitizer runs — TEST-01 in future requirements

</deferred>

---

*Phase: 46-sanitizers-bug-fix*
*Context gathered: 2026-03-20*
