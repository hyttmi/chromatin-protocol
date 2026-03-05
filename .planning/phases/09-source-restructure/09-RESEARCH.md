# Phase 9: Source Restructure - Research

**Researched:** 2026-03-05
**Domain:** C++ codebase mechanical restructure (directory move + namespace rename)
**Confidence:** HIGH

## Summary

Phase 9 is a purely mechanical restructure with zero feature changes. The work breaks into three orthogonal operations: (1) move `src/` to `db/`, (2) rename the `chromatin::` C++ namespace to `chromatindb::`, and (3) regenerate FlatBuffers headers from updated schemas. All three operations are text transformations applied across a well-understood, enumerable set of files.

The codebase is small (40 source files, 18 test files, 2 schemas) with explicit CMakeLists.txt file listings (no globs), making the restructure deterministic and verifiable. The key risk is partial application -- missing a single `chromatin::` reference or include path causes a compile error, which is actually a safety net: the compiler catches every mistake.

**Primary recommendation:** Do directory move first (mechanical git mv), then namespace rename (global find-replace), then regenerate FlatBuffers. Compile after each step to catch errors incrementally.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- `db/` at project root replaces `src/` entirely
- Keep flat module structure: db/config, db/crypto, db/engine, db/identity, db/logging, db/net, db/peer, db/storage, db/sync, db/wire
- `main.cpp` stays inside `db/` as `db/main.cpp`
- `src/` is deleted completely -- no symlinks, no remnants
- Include root is the project root (CMake `target_include_directories` points to `${CMAKE_CURRENT_SOURCE_DIR}`)
- All includes use `db/` prefix: `#include "db/crypto/hash.h"`
- Tests also use the `db/` prefix -- same convention everywhere
- Generated headers stay in `db/wire/` (blob_generated.h, transport_generated.h)
- Schema namespace changes from `chromatin.wire` to `chromatindb.wire`
- Generated C++ namespace becomes `chromatindb::wire` (consistent with all other namespaces)

### Claude's Discretion
- Migration order (namespace first vs directory first vs all at once)
- How to update CMakeLists.txt (single rewrite vs incremental edits)
- Whether to do a single commit or multiple commits for the restructure

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| STRUCT-01 | Source files moved to `/db` directory layout with updated CMakeLists.txt | Directory move via `git mv src db`, CMakeLists.txt path updates (all `src/` to `db/`), include directory change from `src` to project root. See Architecture Patterns. |
| STRUCT-02 | C++ namespace renamed from `chromatin::` to `chromatindb::` across all source, headers, and FlatBuffers schemas | Global find-replace `chromatin::` to `chromatindb::` in 40 source + 18 test files. FlatBuffers schema namespace change + regeneration. See Change Inventory. |
| STRUCT-03 | All 155 existing tests pass after restructure with clean build | Clean build verification: `rm -rf build && cmake -B build && cmake --build build && cd build && ctest`. Compiler catches all missed references. |
</phase_requirements>

## Standard Stack

Not applicable -- this phase uses no new libraries. The existing stack (CMake, FlatBuffers compiler) is the only tooling involved.

### FlatBuffers Compiler (flatc)
| Property | Value |
|----------|-------|
| Version | 25.2.10 (fetched via FetchContent) |
| Schema dir | `schemas/` |
| Output dir | Currently `src/wire/`, changing to `db/wire/` |
| Flags | `--cpp --gen-object-api` |

The flatc binary is built as part of FetchContent. The CMakeLists.txt already has custom commands that invoke `flatc` to regenerate headers from `.fbs` schemas.

## Architecture Patterns

### Recommended Migration Order

**Step 1: Directory move (STRUCT-01)**

Move all files first because this is the most disruptive git operation and should be isolated.

```bash
git mv src db
```

This single command preserves git history for all 40 files. Then update:
1. CMakeLists.txt: all `src/` path references become `db/`
2. CMakeLists.txt: `target_include_directories` changes from `${CMAKE_CURRENT_SOURCE_DIR}/src` to `${CMAKE_CURRENT_SOURCE_DIR}`
3. All `#include` directives in source files: add `db/` prefix (currently `"crypto/hash.h"` becomes `"db/crypto/hash.h"`)
4. All `#include` directives in test files: add `db/` prefix (same pattern)

**Step 2: Namespace rename (STRUCT-02)**

After the directory move compiles cleanly, do the namespace rename as a separate logical step:
1. Global replace `namespace chromatin::` with `namespace chromatindb::` in all hand-written source files
2. Global replace `namespace chromatin {` with `namespace chromatindb {` (found in wire generated headers, but those get regenerated)
3. Global replace `} // namespace chromatin::` with `} // namespace chromatindb::` (closing comments)
4. Global replace `chromatin::` with `chromatindb::` in all test files (qualified references)
5. Global replace `using namespace chromatin::` with `using namespace chromatindb::` in tests
6. Global replace `using chromatin::` with `using chromatindb::` in tests

**Step 3: FlatBuffers schema update + regeneration (part of STRUCT-02)**

1. Change `namespace chromatin.wire;` to `namespace chromatindb.wire;` in both `schemas/blob.fbs` and `schemas/transport.fbs`
2. Regenerate headers (the CMake custom commands handle this, but the generated files are checked in so they must be regenerated and committed)
3. The generated headers will automatically get the new `chromatindb::wire` namespace and updated include guards (`FLATBUFFERS_GENERATED_BLOB_CHROMATINDB_WIRE_H_`)

### Change Inventory

**Files affected by directory move (STRUCT-01):**

| Category | Count | Change |
|----------|-------|--------|
| Source files (git mv) | 40 | `src/X` to `db/X` |
| `#include` in source | 72 | Add `db/` prefix |
| `#include` in tests | 53 | Add `db/` prefix |
| CMakeLists.txt paths | ~30 | `src/` to `db/` |
| CMakeLists.txt include dir | 1 | `src` to project root |
| FlatBuffers output dir | 1 | `src/wire` to `db/wire` |

**Files affected by namespace rename (STRUCT-02):**

| Category | Count | Change |
|----------|-------|--------|
| Namespace declarations in source | 78 | `chromatin::` to `chromatindb::` |
| Qualified refs in source (non-generated) | ~90 | `chromatin::` to `chromatindb::` |
| Qualified refs in tests | ~142 | `chromatin::` to `chromatindb::` |
| FlatBuffers schemas | 2 | `chromatin.wire` to `chromatindb.wire` |
| Generated headers (regenerated) | 2 | Automatic after schema change |
| CMakeLists.txt target names | 0 | Already named `chromatindb_lib`, `chromatindb` -- no change needed |

**Files NOT affected:**
- CMakeLists.txt target names: already `chromatindb_lib`, `chromatindb`, `chromatindb_tests` -- correct as-is
- External library includes (`<asio.hpp>`, `<spdlog/spdlog.h>`, `"flatbuffers/flatbuffers.h"`) -- unchanged
- `schemas/` directory location -- stays at project root
- `tests/` directory location -- stays at project root

### Include Path Transition

**Current** (include root = `src/`):
```cpp
// In src/engine/engine.cpp:
#include "engine/engine.h"
#include "storage/storage.h"
#include "wire/codec.h"
```

**After** (include root = project root):
```cpp
// In db/engine/engine.cpp:
#include "db/engine/engine.h"
#include "db/storage/storage.h"
#include "db/wire/codec.h"
```

The key CMake change:
```cmake
# Before:
target_include_directories(chromatindb_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/src
)

# After:
target_include_directories(chromatindb_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
```

### Header Guard Note

All hand-written headers use `#pragma once` -- no include guard names to update. Only the FlatBuffers generated headers use `#ifndef` guards (`FLATBUFFERS_GENERATED_BLOB_CHROMATIN_WIRE_H_`), and those are regenerated automatically.

### Anti-Patterns to Avoid
- **Partial rename:** Doing find-replace on only some files. Use exhaustive file lists, not "hope we got them all."
- **Editing generated files manually:** The `*_generated.h` files must be regenerated from schemas, not hand-edited. Manual edits will be overwritten by the next flatc run.
- **Forgetting the include directory change:** Moving files to `db/` without changing the CMake include root means includes like `"crypto/hash.h"` won't resolve. The include root must be project root so `"db/crypto/hash.h"` works.

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| FlatBuffer namespace change | Manual edit of generated headers | Regenerate from updated schemas via flatc | Generated files have internal qualified refs that must be consistent |
| File move with history | Manual copy + delete | `git mv src db` | Preserves git blame/history |

**Key insight:** The FlatBuffers generated headers contain ~28 qualified `chromatin::wire::` references inside the generated code (e.g., `chromatin::wire::CreateBlob`, `chromatin::wire::Blob`). These must come from regeneration, not manual editing, because the include guard names, namespace structure, and all internal references must be consistent.

## Common Pitfalls

### Pitfall 1: Include Path Mismatch After Move
**What goes wrong:** After `git mv src db`, all includes like `#include "crypto/hash.h"` break because the include root was `src/` and the files are now in `db/`.
**Why it happens:** The old include root pointed at `src/`. With `db/` as the directory, either the include root must change to project root OR includes must drop their module prefix.
**How to avoid:** Change include root to project root first, then update all includes to use `db/` prefix. This matches the locked decision.
**Warning signs:** `fatal error: 'crypto/hash.h' file not found` at compile time.

### Pitfall 2: FlatBuffers Schema Regeneration Timing
**What goes wrong:** Updating schemas but not regenerating headers, or regenerating before the output directory exists.
**Why it happens:** The generated headers are checked into the repo. After `git mv`, the old output directory `src/wire/` no longer exists.
**How to avoid:** Update the CMakeLists.txt `FLATBUFFERS_GENERATED_DIR` to `db/wire/` before regenerating. Or regenerate manually: `flatc --cpp --gen-object-api -o db/wire/ schemas/blob.fbs schemas/transport.fbs`.
**Warning signs:** Stale `*_generated.h` with old namespace, or missing generated headers.

### Pitfall 3: Namespace in Closing Comments
**What goes wrong:** Global replace catches `namespace chromatin::` but misses `} // namespace chromatin::` closing comments, leaving them inconsistent.
**Why it happens:** Closing comments are cosmetic -- they don't affect compilation but leave the code misleading.
**How to avoid:** Include closing comment patterns in the find-replace: `} // namespace chromatin` should become `} // namespace chromatindb`.
**Warning signs:** `grep -r "chromatin" src/` (or `db/` after move) returns only closing comments.

### Pitfall 4: Nested Namespace Syntax vs Brace Syntax
**What goes wrong:** The generated FlatBuffer headers use the old-style `namespace chromatin { namespace wire {` syntax (two separate declarations), while hand-written code uses C++17 nested `namespace chromatin::wire {`. Both must be updated.
**Why it happens:** flatc generates C++11-compatible code with separate namespace blocks.
**How to avoid:** The generated headers are regenerated from schemas (handled by schema namespace change). Hand-written code uses `chromatin::module` consistently -- a single `chromatin::` to `chromatindb::` replacement handles all cases.
**Warning signs:** Compile errors about undefined types in `chromatindb::wire` namespace.

### Pitfall 5: Incomplete Cleanup of Old `src/` Directory
**What goes wrong:** `git mv src db` moves files but may leave empty directories or dot files behind if the src directory has any.
**Why it happens:** `git mv` only moves tracked files.
**How to avoid:** After `git mv src db`, explicitly `rm -rf src/` and verify it's gone. The locked decision says "src/ is deleted completely -- no symlinks, no remnants."
**Warning signs:** `ls src/` returns anything.

## Code Examples

### CMakeLists.txt Path Update Pattern
```cmake
# FlatBuffers generated dir
set(FLATBUFFERS_GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/db/wire)

# Library target
add_library(chromatindb_lib STATIC
  db/crypto/hash.cpp
  db/crypto/signing.cpp
  db/crypto/kem.cpp
  db/crypto/aead.cpp
  db/crypto/kdf.cpp
  db/wire/codec.cpp
  db/config/config.cpp
  db/logging/logging.cpp
  db/identity/identity.cpp
  db/storage/storage.cpp
  db/engine/engine.cpp
  db/net/framing.cpp
  db/net/protocol.cpp
  db/net/handshake.cpp
  db/net/connection.cpp
  db/net/server.cpp
  db/sync/sync_protocol.cpp
  db/peer/peer_manager.cpp
)

target_include_directories(chromatindb_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)

# Daemon binary
add_executable(chromatindb db/main.cpp)

# Tests remain under tests/ -- unchanged paths
```

### Namespace Rename Pattern
```cpp
// Before:
namespace chromatin::crypto {
// ...
} // namespace chromatin::crypto

// After:
namespace chromatindb::crypto {
// ...
} // namespace chromatindb::crypto
```

### FlatBuffers Schema Namespace Change
```fbs
// Before (schemas/blob.fbs):
namespace chromatin.wire;

// After:
namespace chromatindb.wire;
```

### Regeneration Command
```bash
# After schemas are updated and db/wire/ exists:
flatc --cpp --gen-object-api -o db/wire/ schemas/blob.fbs
flatc --cpp --gen-object-api -o db/wire/ schemas/transport.fbs
```

Or let CMake handle it during the build (the custom commands already do this, just with updated paths).

### Include Update Pattern
```cpp
// Before (in db/engine/engine.cpp after git mv):
#include "engine/engine.h"      // BROKEN -- old include root

// After:
#include "db/engine/engine.h"   // WORKS -- new include root is project root
```

### Test Namespace Update Pattern
```cpp
// Before:
using namespace chromatin::crypto;
using chromatin::engine::BlobEngine;
auto id = chromatin::identity::NodeIdentity::generate();

// After:
using namespace chromatindb::crypto;
using chromatindb::engine::BlobEngine;
auto id = chromatindb::identity::NodeIdentity::generate();
```

## State of the Art

Not applicable -- this is a mechanical restructure, not a technology choice.

## Open Questions

1. **FlatBuffers regeneration method**
   - What we know: The CMakeLists.txt has custom commands that build flatc via FetchContent and use it to regenerate headers. After the directory move, these commands will target `db/wire/` instead of `src/wire/`.
   - What's unclear: Whether to regenerate during the restructure (by running cmake+build) or to run flatc manually and commit the result. Both work.
   - Recommendation: Run flatc manually after updating schemas, since CMake may not have a working build directory during the restructure. Alternatively, let the clean build at the end handle regeneration -- but then the generated files should be committed after the build succeeds.

## Sources

### Primary (HIGH confidence)
- Direct codebase inspection: all 40 source files, 18 test files, 2 schemas, CMakeLists.txt
- Exact counts verified via grep across the codebase

### Secondary (MEDIUM confidence)
- FlatBuffers namespace behavior: verified by inspecting the current generated output against schema input. `namespace X.Y;` in `.fbs` produces `namespace X { namespace Y {` in C++.

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - no new libraries, only existing FlatBuffers compiler
- Architecture: HIGH - exhaustive file inventory from direct codebase inspection
- Pitfalls: HIGH - all identified from actual code patterns in the codebase

**Research date:** 2026-03-05
**Valid until:** indefinite -- mechanical restructure facts don't go stale
