# Phase 9: Source Restructure - Context

**Gathered:** 2026-03-05
**Status:** Ready for planning

<domain>
## Phase Boundary

Rename the C++ namespace from `chromatin::` to `chromatindb::` and move all source files from `src/` to `db/` directory layout. Pure mechanical restructure — no feature changes, no API changes. All 155 existing tests must pass after a clean build.

</domain>

<decisions>
## Implementation Decisions

### Directory layout
- `db/` at project root replaces `src/` entirely
- Keep flat module structure: db/config, db/crypto, db/engine, db/identity, db/logging, db/net, db/peer, db/storage, db/sync, db/wire
- `main.cpp` stays inside `db/` as `db/main.cpp`
- `src/` is deleted completely — no symlinks, no remnants

### Include path convention
- Include root is the project root (CMake `target_include_directories` points to `${CMAKE_CURRENT_SOURCE_DIR}`)
- All includes use `db/` prefix: `#include "db/crypto/hash.h"`
- Tests also use the `db/` prefix — same convention everywhere

### FlatBuffers
- Generated headers stay in `db/wire/` (blob_generated.h, transport_generated.h)
- Schema namespace changes from `chromatin.wire` to `chromatindb.wire`
- Generated C++ namespace becomes `chromatindb::wire` (consistent with all other namespaces)

### Claude's Discretion
- Migration order (namespace first vs directory first vs all at once)
- How to update CMakeLists.txt (single rewrite vs incremental edits)
- Whether to do a single commit or multiple commits for the restructure

</decisions>

<specifics>
## Specific Ideas

No specific requirements — clean mechanical rename following the conventions above.

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- No reusable assets — this phase is a restructure, not a feature

### Established Patterns
- 39 source files across 10 flat modules (config, crypto, engine, identity, logging, net, peer, storage, sync, wire)
- All namespaces follow `chromatin::{module}` pattern (e.g., `chromatin::net`, `chromatin::peer`)
- FlatBuffer schemas use `namespace chromatin.wire;`
- CMakeLists.txt lists every source file explicitly (no globs)
- Tests mirror src structure under `tests/` with matching subdirectories

### Integration Points
- CMakeLists.txt: all `src/` paths → `db/` paths, include directories updated
- FlatBuffer schema compilation: output dir changes from `src/wire` to `db/wire`
- Every `#include` in source and test files needs the `db/` prefix
- Every `namespace chromatin::` becomes `chromatindb::` in 39 source files
- Every `chromatin::` qualified reference in 18 test files
- `namespace chromatin.wire` in 2 FlatBuffer schemas

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 09-source-restructure*
*Context gathered: 2026-03-05*
