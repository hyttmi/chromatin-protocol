# Phase 22: Build Restructure - Context

**Gathered:** 2026-03-14
**Status:** Ready for planning

<domain>
## Phase Boundary

Make db/ a self-contained CMake component that can be built and tested independently. Root CMakeLists.txt consumes it via add_subdirectory. All existing tests must compile and pass without modification.

</domain>

<decisions>
## Implementation Decisions

### Self-containment level
- db/ has its own `project(chromatindb-core)` and can be built standalone with `cmake -S db/ -B build`
- db/CMakeLists.txt declares all FetchContent dependencies with `if(NOT TARGET x)` guards to avoid double-fetching when consumed via add_subdirectory
- db/ declares the library target only — daemon binary (main.cpp), bench, and tests remain at root
- When consumed via add_subdirectory from root, root provides dependencies first so db/ guards skip re-fetching

### What belongs in db/
- Schemas move from root `schemas/` into `db/schemas/` (blob.fbs, transport.fbs)
- `version.h` stays in db/
- `README.md` and `PROTOCOL.md` stay in db/
- bench/ stays at root (consumer of the library)
- tests/ stay at root (consumer of the library)

### Target naming
- Library target name stays `chromatindb_lib` — no rename
- Include path convention unchanged: `#include "db/crypto/hash.h"` (root of include tree is the parent of db/)
- Daemon binary stays named `chromatindb`
- db/ project name: `chromatindb-core` (avoids variable collision with root `chromatindb`)

### FlatBuffers codegen
- db/CMakeLists.txt owns the flatc custom_commands (schemas are in db/schemas/ now)
- Generated headers stay in `db/wire/` alongside handwritten wire code (current location)
- Generated headers remain committed to git (available without flatc)
- db/CMakeLists.txt FetchContent's flatbuffers with guard: `if(NOT TARGET flatbuffers)`

### Claude's Discretion
- Whether standalone db/ build exposes sanitizer options (ENABLE_ASAN)
- Exact CMake version minimum for db/CMakeLists.txt
- Whether to add install() rules to db/

</decisions>

<specifics>
## Specific Ideas

No specific requirements — open to standard approaches. Success criteria are clear: db/ is self-contained, root consumes it via add_subdirectory, zero test regressions.

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- Root CMakeLists.txt (235 lines): All dependency declarations, build flags, and target definitions to be refactored
- Existing FetchContent declarations: liboqs, sodium, flatbuffers, Catch2, spdlog, json, libmdbx, asiocmake

### Established Patterns
- FetchContent for all deps (always latest versions, GIT_SHALLOW)
- liboqs algorithm stripping via cache variables
- FlatBuffers codegen via custom_command + custom_target
- Single STATIC library target linking all deps as PUBLIC

### Integration Points
- Root CMakeLists.txt `add_subdirectory(db)` replaces inline library definition
- Root still owns: daemon binary (db/main.cpp), bench binary (bench/), test binary (tests/), CTest setup
- Root FetchContent runs first, db/ guards detect targets already exist and skip

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 22-build-restructure*
*Context gathered: 2026-03-14*
