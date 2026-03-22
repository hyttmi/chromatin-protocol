# Phase 53: Release Cleanup & Documentation - Context

**Gathered:** 2026-03-22
**Status:** Ready for planning

<domain>
## Phase Boundary

Repository reflects shipped v1.0.0 reality -- tagged, documented, stale artifacts removed, version bumped to 1.1.0. Archive completed milestone phases. No new capabilities.

</domain>

<decisions>
## Implementation Decisions

### Stale artifact removal
- Remove `deploy/test-crash-recovery.sh` (bash crash recovery test, replaced by Docker integration suite)
- Remove `db/TESTS.md` (stale design doc)
- Remove `scripts/run-e2e-reliability.sh` (one-time v1.0.0 validation tool, no longer needed)
- Do NOT remove `.planning/milestones/v1.0.0-*` files -- these are standard archives, not stale

### Milestone phase archiving
- Archive v1.0.0 phases (46-52) into `.planning/milestones/v1.0.0-phases/`
- Also archive all other unarchived completed milestone phases: v0.4.0 (16-21), v0.7.0 (32-37), v0.8.0 (38-41)
- Follow same pattern as existing archives (v1.0-phases/, v2.0-phases/, etc.)

### db/README.md updates
- Add new **Testing** section placed after Build section (Build -> Testing -> Usage flow)
- Testing section covers: unit test count + how to run, sanitizer status (ASAN/TSAN/UBSAN clean), Docker integration suite (54 tests, 12 categories), stress/chaos/fuzz summary (5-node SIGKILL churn, 1000-namespace scaling, protocol fuzzing)
- Update Build section to include sanitizer build instructions (`-DSANITIZER=asan`)
- Existing feature descriptions and config reference are accurate for v1.0.0, no other changes needed

### Top-level README.md
- Keep minimal (intro paragraph + pointer to db/README.md + license)
- Add a version line (e.g., "Current release: v1.0.0") to anchor to shipped state

### Version bump
- Bump CMake `project(VERSION ...)` from 1.0.0 to 1.1.0 in top-level CMakeLists.txt

### Git tag
- v1.0.0 tag already exists on the shipped commit -- no action needed (REL-01 already satisfied)

### Claude's Discretion
- Exact wording and formatting of the Testing section
- Whether to add a CHANGELOG.md or release notes (not required)
- Order of operations (deletions first vs docs first)

</decisions>

<code_context>
## Existing Code Insights

### Reusable Assets
- `db/README.md` (328 lines): comprehensive docs with Architecture, Building, Config, Scenarios, Features sections -- only needs Testing section addition and Build section update
- `README.md` (11 lines): minimal pointer file -- needs version line only
- Top-level `CMakeLists.txt`: contains `project(chromatindb VERSION 1.0.0 LANGUAGES C CXX)` -- bump target

### Established Patterns
- Milestone archives follow `v{X}-phases/`, `v{X}-ROADMAP.md`, `v{X}-REQUIREMENTS.md` pattern in `.planning/milestones/`
- db/ is self-contained CMake component with its own `CMakeLists.txt` (no VERSION)
- Version injection via `configure_file` from `version.h.in` to `version.h`

### Integration Points
- CMake version flows to `db/version.h` via configure_file
- `git describe` should return v1.0.0 on tagged commit (already works)

</code_context>

<specifics>
## Specific Ideas

No specific requirements -- open to standard approaches for the documentation content and file organization.

</specifics>

<deferred>
## Deferred Ideas

None -- discussion stayed within phase scope

</deferred>

---

*Phase: 53-release-cleanup-documentation*
*Context gathered: 2026-03-22*
