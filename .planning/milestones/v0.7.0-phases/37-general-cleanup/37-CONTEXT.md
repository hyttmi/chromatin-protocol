# Phase 37: General Cleanup - Context

**Gathered:** 2026-03-18
**Status:** Ready for planning

<domain>
## Phase Boundary

Remove stale artifacts from previous milestones, update documentation to reflect all v0.7.0 changes, and sweep dead code. The old standalone benchmark binary is replaced by the Docker benchmark suite (v0.6.0). Both READMEs and PROTOCOL.md are updated to current state.

</domain>

<decisions>
## Implementation Decisions

### Benchmark removal (CLEAN-02)
- Delete `bench/` directory entirely (contains only `bench_main.cpp`)
- Remove `chromatindb_bench` target from top-level `CMakeLists.txt`
- Remove the entire Performance section from README (both tables and "Running Benchmarks" subsection) — clean removal, no replacement pointer to Docker suite

### README updates (CLEAN-03)
- Update **both** top-level `README.md` and `db/README.md` consistently
- Add "Sync Resumption" feature bullet — brief, emphasizing O(new) instead of O(total)
- Add "Namespace Quotas" feature bullet
- Add new config fields inline in the existing JSON config example: `full_resync_interval`, `cursor_stale_seconds`, `namespace_quota_bytes`, `namespace_quota_count`, `namespace_quotas`
- Add field descriptions below the config example for all new fields
- Update SIGHUP section to list new reloadable fields: quota config and cursor config
- Update Architecture section to silently reflect hash-then-sign (no migration note — pre-MVP)
- Wire protocol message type count: Claude checks actual count and updates README + PROTOCOL.md to match

### PROTOCOL.md update
- Check and update PROTOCOL.md for accuracy after hash-then-sign (Phase 33), QuotaExceeded message type (Phase 35), and any other v0.7.0 wire protocol changes

### Stale artifact sweep (CLEAN-04)
- Deep audit: unused includes, dead code paths, stale comments referencing removed features, orphaned test helpers
- If code is provably unreachable or references removed features, delete directly (tests catch regressions)
- Scope includes: `bench/`, top-level `CMakeLists.txt`, all C++ source in `db/`, `deploy/`, `Dockerfile`, `loadgen/`
- `.planning/` directory is excluded — leave planning artifacts untouched
- Check Dockerfile for references to removed artifacts (bench/ copies, chromatindb_bench build)
- Check deploy/ for stale configs, outdated scripts, references to removed features
- Check loadgen/ for stale code from v0.6.0/v0.7.0 modifications

### Claude's Discretion
- Wire protocol message type count (verify actual count, update accordingly)
- Specific dead code findings during deep audit (delete directly per user preference)
- Order of cleanup operations
- Whether deploy/ untracked configs should be gitignored or committed

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- Docker benchmark suite in `deploy/` replaces standalone `chromatindb_bench`
- `loadgen/` binary remains (used by Docker benchmarks)

### Established Patterns
- README structure: Crypto Stack → Architecture → Building → Usage → Configuration → Signals → Wire Protocol → Scenarios → Features → License
- Config fields documented as JSON example + bullet descriptions
- SIGHUP section lists all hot-reloadable fields

### Integration Points
- Top-level `CMakeLists.txt` defines all binary targets (daemon, bench, loadgen)
- `db/CMakeLists.txt` defines the library component
- Dockerfile copies source and builds targets
- `deploy/run-benchmark.sh` orchestrates Docker benchmarks

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 37-general-cleanup*
*Context gathered: 2026-03-18*
