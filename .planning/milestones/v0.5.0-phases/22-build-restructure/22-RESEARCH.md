# Phase 22: Build Restructure - Research

**Researched:** 2026-03-14
**Domain:** CMake build system restructuring (subdirectory componentization)
**Confidence:** HIGH

## Summary

Phase 22 restructures the build so that `db/` is a self-contained CMake component with its own `project(chromatindb-core)` and `CMakeLists.txt`. The root `CMakeLists.txt` consumes it via `add_subdirectory(db)`. Schemas move from `schemas/` to `db/schemas/`. All existing tests, bench, and daemon binary stay at root as library consumers. Zero test regressions.

This is a mechanical restructuring phase with no new features. The current `CMakeLists.txt` is 235 lines, well-organized in sections (dependencies, codegen, library, daemon, bench, tests). The refactoring splits it into two files: `db/CMakeLists.txt` owns the library target and its FetchContent dependencies (with `if(NOT TARGET ...)` guards), and root owns consumers (daemon, bench, tests) plus its own FetchContent declarations that run first.

**Primary recommendation:** Split the existing CMakeLists.txt into two files. db/CMakeLists.txt declares `project(chromatindb-core)`, all dependency FetchContent blocks (guarded), FlatBuffers codegen, and the `chromatindb_lib` STATIC library target. Root CMakeLists.txt declares its own FetchContent blocks (running first), then `add_subdirectory(db)`, then daemon/bench/test targets.

<user_constraints>
## User Constraints (from CONTEXT.md)

### Locked Decisions
- db/ has its own `project(chromatindb-core)` and can be built standalone with `cmake -S db/ -B build`
- db/CMakeLists.txt declares all FetchContent dependencies with `if(NOT TARGET x)` guards to avoid double-fetching when consumed via add_subdirectory
- db/ declares the library target only -- daemon binary (main.cpp), bench, and tests remain at root
- When consumed via add_subdirectory from root, root provides dependencies first so db/ guards skip re-fetching
- Schemas move from root `schemas/` into `db/schemas/` (blob.fbs, transport.fbs)
- `version.h` stays in db/
- `README.md` and `PROTOCOL.md` stay in db/
- bench/ stays at root (consumer of the library)
- tests/ stay at root (consumer of the library)
- Library target name stays `chromatindb_lib` -- no rename
- Include path convention unchanged: `#include "db/crypto/hash.h"` (root of include tree is the parent of db/)
- Daemon binary stays named `chromatindb`
- db/ project name: `chromatindb-core` (avoids variable collision with root `chromatindb`)
- db/CMakeLists.txt owns the flatc custom_commands (schemas are in db/schemas/ now)
- Generated headers stay in `db/wire/` alongside handwritten wire code (current location)
- Generated headers remain committed to git (available without flatc)
- db/CMakeLists.txt FetchContent's flatbuffers with guard: `if(NOT TARGET flatbuffers)`

### Claude's Discretion
- Whether standalone db/ build exposes sanitizer options (ENABLE_ASAN)
- Exact CMake version minimum for db/CMakeLists.txt
- Whether to add install() rules to db/

### Deferred Ideas (OUT OF SCOPE)
None -- discussion stayed within phase scope
</user_constraints>

<phase_requirements>
## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| BUILD-01 | CMakeLists.txt restructured so db/ is a self-contained CMake component | All research findings directly support this: split CMakeLists.txt, if(NOT TARGET) guards, schema relocation, include path preservation |
</phase_requirements>

## Standard Stack

### Core
| Tool | Version | Purpose | Why Standard |
|------|---------|---------|--------------|
| CMake | 3.20+ | Build system | Already in use, FetchContent requires 3.14+, project() in subdirectories works since 3.0 |

### Supporting
No new libraries or tools needed. This phase only restructures existing CMake files.

### Alternatives Considered
None -- locked decisions specify the approach. No new tools needed.

## Architecture Patterns

### Recommended File Structure (After Phase 22)
```
chromatin-protocol/
  CMakeLists.txt          # Root: project(chromatindb), deps, add_subdirectory(db), daemon/bench/tests
  db/
    CMakeLists.txt         # Sub: project(chromatindb-core), guarded deps, codegen, chromatindb_lib target
    schemas/
      blob.fbs             # Moved from root schemas/
      transport.fbs        # Moved from root schemas/
    wire/
      blob_generated.h     # Generated (committed, stays here)
      transport_generated.h
      codec.h
      codec.cpp
    crypto/
    config/
    logging/
    identity/
    storage/
    engine/
    net/
    peer/
    sync/
    acl/
    main.cpp               # Stays here, but daemon binary target is at ROOT
    version.h
    README.md
    PROTOCOL.md
  bench/
    bench_main.cpp
  tests/
    ...all test files...
```

### Pattern 1: Guarded FetchContent for Composable Subdirectories

**What:** Each dependency FetchContent block in `db/CMakeLists.txt` is wrapped in `if(NOT TARGET <target_name>)` so that when the root has already fetched it, the subdirectory skips the redundant fetch.

**When to use:** Whenever a subdirectory needs to be both standalone-buildable AND consumable via `add_subdirectory`.

**Example:**
```cmake
# db/CMakeLists.txt
if(NOT TARGET oqs)
  # liboqs algorithm stripping cache variables
  set(OQS_BUILD_ONLY_LIB ON CACHE BOOL "" FORCE)
  # ... all OQS cache variables ...
  FetchContent_Declare(liboqs
    GIT_REPOSITORY https://github.com/open-quantum-safe/liboqs.git
    GIT_TAG        0.15.0
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(liboqs)
  FetchContent_GetProperties(liboqs)
  target_include_directories(oqs PUBLIC
    $<BUILD_INTERFACE:${liboqs_BINARY_DIR}/include>
  )
endif()
```

**Why `if(NOT TARGET)` and not `if(NOT <pkg>_FOUND)`:** FetchContent does not set `<pkg>_FOUND`. The target name is the authoritative signal that a dependency is already available. This is the standard CMake idiom for composable subdirectories.

### Pattern 2: Subdirectory project() with Standalone Build Support

**What:** `db/CMakeLists.txt` starts with its own `cmake_minimum_required` and `project()` so it can be built independently.

**Example:**
```cmake
# db/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(chromatindb-core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
```

**Key detail:** When consumed via `add_subdirectory()`, the child's `project()` call creates a separate scope with its own `PROJECT_NAME`, `PROJECT_SOURCE_DIR`, etc. It does NOT conflict with the root's `project(chromatindb)` because CMake project variables are scoped. The root's `CMAKE_*` variables are inherited by the child unless overridden.

### Pattern 3: Include Path Preservation via Parent Directory

**What:** All source files use `#include "db/crypto/hash.h"` -- the include root is the parent of `db/`. This means `target_include_directories` must point to the parent of `db/`, not to `db/` itself.

**Example:**
```cmake
# db/CMakeLists.txt
target_include_directories(chromatindb_lib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)
```

**Critical:** When building standalone (`cmake -S db/ -B build`), `CMAKE_CURRENT_SOURCE_DIR/..` points to the correct parent. When consumed via `add_subdirectory(db)`, it also points to the root, which is where the include path `db/...` resolves from. This pattern keeps all `#include "db/..."` directives working without modification.

### Pattern 4: FlatBuffers Codegen with Relocated Schemas

**What:** Schema files move to `db/schemas/`. The `custom_command` uses `CMAKE_CURRENT_SOURCE_DIR` to reference them (works for both standalone and subdirectory builds).

**Example:**
```cmake
# db/CMakeLists.txt
set(FLATBUFFERS_SCHEMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/schemas)
set(FLATBUFFERS_GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/wire)

if(EXISTS ${FLATBUFFERS_SCHEMA_DIR}/blob.fbs)
  add_custom_command(
    OUTPUT ${FLATBUFFERS_GENERATED_DIR}/blob_generated.h
    COMMAND flatc --cpp --gen-object-api
            -o ${FLATBUFFERS_GENERATED_DIR}
            ${FLATBUFFERS_SCHEMA_DIR}/blob.fbs
    DEPENDS ${FLATBUFFERS_SCHEMA_DIR}/blob.fbs flatc
    COMMENT "Generating FlatBuffers blob header"
  )
  add_custom_target(flatbuffers_blob_generated
    DEPENDS ${FLATBUFFERS_GENERATED_DIR}/blob_generated.h
  )
endif()
```

**Key detail:** `CMAKE_CURRENT_SOURCE_DIR` is always the directory containing the CMakeLists.txt being processed, regardless of whether it is standalone or consumed via `add_subdirectory`. This makes the codegen paths correct in both modes.

### Pattern 5: Root Consumes Subdirectory

**What:** Root CMakeLists.txt fetches all deps first (so targets exist), then `add_subdirectory(db)` where guards skip re-fetching. Root then defines consumer targets.

**Example:**
```cmake
# Root CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(chromatindb LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Sanitizer option
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

include(FetchContent)
# ... all FetchContent declarations and MakeAvailable ...

add_subdirectory(db)

# Daemon
add_executable(chromatindb db/main.cpp)
target_link_libraries(chromatindb PRIVATE chromatindb_lib)

# Bench
add_executable(chromatindb_bench bench/bench_main.cpp)
target_link_libraries(chromatindb_bench PRIVATE chromatindb_lib)

# Tests
add_executable(chromatindb_tests ...)
target_link_libraries(chromatindb_tests PRIVATE chromatindb_lib Catch2::Catch2WithMain)
include(CTest)
include(Catch)
catch_discover_tests(chromatindb_tests)
```

### Anti-Patterns to Avoid
- **Using `CMAKE_SOURCE_DIR` in subdirectory:** Always use `CMAKE_CURRENT_SOURCE_DIR`. `CMAKE_SOURCE_DIR` always points to the top-level source dir, which breaks standalone builds.
- **FetchContent without target guards:** Leads to "target already defined" errors when consumed via `add_subdirectory` from a parent that already fetched the same dependency.
- **Moving include path to `db/` instead of parent:** Would require rewriting every `#include "db/..."` in the entire codebase. Locked decision says include paths stay unchanged.
- **Splitting FetchContent_MakeAvailable into separate calls per dep in the subdirectory:** The current root uses a single `FetchContent_MakeAvailable(liboqs sodium flatbuffers ...)` call. In the subdirectory, each dependency needs its own `MakeAvailable` inside its `if(NOT TARGET)` guard, since each guard is independent.
- **Forgetting liboqs binary include dir:** The current CMakeLists.txt has a special `target_include_directories(oqs PUBLIC ...)` for liboqs build-dir headers. This must be inside the `if(NOT TARGET oqs)` guard in db/CMakeLists.txt.
- **Using `${CMAKE_BUILD_TYPE}` in db/CMakeLists.txt for standalone:** The standalone build should inherit or set build type. The user's root sets `CMAKE_BUILD_TYPE Debug` -- the subdirectory should NOT hardcode this (it's inherited from root or set by the standalone user).

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| Dependency deduplication | Custom find-or-fetch macros | `if(NOT TARGET x)` guard | Standard CMake idiom, zero overhead, no maintenance |
| Schema path resolution | Hardcoded absolute paths | `CMAKE_CURRENT_SOURCE_DIR` | Works in both standalone and subdirectory modes automatically |

**Key insight:** This phase is pure CMake restructuring. There is nothing to hand-roll -- CMake provides all needed mechanisms natively.

## Common Pitfalls

### Pitfall 1: Variable Name Collision Between Root and Subdirectory
**What goes wrong:** Both root and db/ use the same CMake variable names (e.g., `FLATBUFFERS_SCHEMA_DIR`). Since `add_subdirectory` does NOT create a new variable scope by default (only `project()` creates scope for project-level variables, not regular variables), root variables leak into the subdirectory or vice versa.
**Why it happens:** Regular `set()` variables in CMakeLists.txt are inherited by child directories and can be overwritten.
**How to avoid:** Use `CMAKE_CURRENT_SOURCE_DIR`-based paths in db/CMakeLists.txt. Root should NOT define `FLATBUFFERS_SCHEMA_DIR` anymore since codegen moved to db/. No variable sharing needed.
**Warning signs:** Codegen outputs appearing in wrong directories.

### Pitfall 2: FetchContent_MakeAvailable Inside Guard
**What goes wrong:** Putting `FetchContent_Declare` outside the guard but `FetchContent_MakeAvailable` inside, or vice versa. Both must be together inside the guard.
**Why it happens:** Misunderstanding FetchContent lifecycle.
**How to avoid:** Always put both `Declare` and `MakeAvailable` inside the `if(NOT TARGET)` block. The cache variables (like OQS algorithm stripping) should also go inside the guard since they only matter when actually fetching.
**Warning signs:** CMake warnings about re-declaring content, or missing targets.

### Pitfall 3: Catch2 include(Catch) Fails in Standalone db/ Build
**What goes wrong:** Trying to `include(Catch)` in db/CMakeLists.txt when Catch2 isn't available.
**Why it happens:** Tests are at root, not in db/. But if someone adds test infrastructure to db/ later.
**How to avoid:** db/CMakeLists.txt should NOT include Catch2 or CTest -- it declares only the library target. Tests remain at root. This is a locked decision.
**Warning signs:** Build errors about missing Catch module.

### Pitfall 4: FlatBuffers custom_command Depends on flatc Target
**What goes wrong:** `DEPENDS flatc` in `add_custom_command` references the flatc executable target. If FlatBuffers was fetched by root and not by db/, the target still exists and this works. But if building standalone and FlatBuffers guard runs, `flatc` target is also created. No issue.
**Why it happens:** N/A -- this works correctly with the guard pattern.
**How to avoid:** Keep `DEPENDS flatc` as-is. The target name is available regardless of which level fetched it.

### Pitfall 5: Schema Files git mv Creates Broken Intermediate State
**What goes wrong:** After `git mv schemas/ db/schemas/`, the root CMakeLists.txt (if not yet updated) references the old location. Build breaks.
**Why it happens:** File moves and CMake changes must be atomic (same commit).
**How to avoid:** Move schemas AND update both CMakeLists.txt files in the same commit. Or better: create db/CMakeLists.txt with the new schema paths, update root to use add_subdirectory, and move schemas -- all in one atomic step.
**Warning signs:** File-not-found errors during cmake configure.

### Pitfall 6: CMAKE_BUILD_TYPE Not Set for Standalone Build
**What goes wrong:** Building standalone (`cmake -S db/ -B build`) without specifying build type results in no optimization flags and no debug info (empty CMAKE_BUILD_TYPE on Make generators).
**Why it happens:** The root sets `CMAKE_BUILD_TYPE Debug` but the subdirectory should not hardcode it.
**How to avoid:** Do not set `CMAKE_BUILD_TYPE` in db/CMakeLists.txt. Standalone users pass it via `-DCMAKE_BUILD_TYPE=Debug`. When consumed via `add_subdirectory`, it inherits root's setting.

## Code Examples

### db/CMakeLists.txt (Complete Skeleton)
```cmake
cmake_minimum_required(VERSION 3.20)
project(chromatindb-core LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# =============================================================================
# Dependencies (guarded for composability)
# =============================================================================

# -- liboqs (ML-DSA-87, ML-KEM-1024, SHA3-256)
if(NOT TARGET oqs)
  set(OQS_BUILD_ONLY_LIB ON CACHE BOOL "" FORCE)
  set(OQS_MINIMAL_BUILD "" CACHE STRING "" FORCE)
  set(OQS_ENABLE_KEM_BIKE OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_KEM_FRODOKEM OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_KEM_NTRUPRIME OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_KEM_NTRU OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_KEM_CLASSIC_MCELIECE OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_KEM_HQC OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_KEM_KYBER OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_KEM_ML_KEM ON CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_ML_DSA ON CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_FALCON OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_SPHINCS OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_MAYO OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_CROSS OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_UOV OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_SNOVA OFF CACHE BOOL "" FORCE)
  set(OQS_ENABLE_SIG_SLH_DSA OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(liboqs
    GIT_REPOSITORY https://github.com/open-quantum-safe/liboqs.git
    GIT_TAG        0.15.0
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(liboqs)
  FetchContent_GetProperties(liboqs)
  target_include_directories(oqs PUBLIC
    $<BUILD_INTERFACE:${liboqs_BINARY_DIR}/include>
  )
endif()

# -- libsodium (ChaCha20-Poly1305, HKDF-SHA256)
if(NOT TARGET sodium)
  set(SODIUM_DISABLE_TESTS ON CACHE BOOL "" FORCE)
  FetchContent_Declare(sodium
    GIT_REPOSITORY https://github.com/robinlinden/libsodium-cmake.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(sodium)
endif()

# -- FlatBuffers (wire format + codegen)
if(NOT TARGET flatbuffers)
  set(FLATBUFFERS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(FLATBUFFERS_BUILD_FLATC ON CACHE BOOL "" FORCE)
  set(FLATBUFFERS_BUILD_FLATHASH OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(flatbuffers
    GIT_REPOSITORY https://github.com/google/flatbuffers.git
    GIT_TAG        v25.2.10
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(flatbuffers)
endif()

# -- spdlog (logging)
if(NOT TARGET spdlog::spdlog)
  set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(spdlog
    GIT_REPOSITORY https://github.com/gabime/spdlog.git
    GIT_TAG        v1.15.1
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(spdlog)
endif()

# -- nlohmann/json (config parsing)
if(NOT TARGET nlohmann_json::nlohmann_json)
  FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  )
  FetchContent_MakeAvailable(json)
endif()

# -- libmdbx (storage)
if(NOT TARGET mdbx-static)
  set(MDBX_BUILD_CXX ON CACHE BOOL "" FORCE)
  set(MDBX_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(MDBX_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
  set(MDBX_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
  set(MDBX_INSTALL_STATIC OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(libmdbx
    GIT_REPOSITORY https://github.com/erthink/libmdbx.git
    GIT_TAG        v0.13.11
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(libmdbx)
endif()

# -- Standalone Asio (networking)
if(NOT TARGET asio::asio)
  set(ASIO_TAG "asio-1-38-0" CACHE STRING "" FORCE)
  FetchContent_Declare(asiocmake
    GIT_REPOSITORY https://github.com/OlivierLDff/asio.cmake
    GIT_TAG        main
    GIT_SHALLOW    TRUE
  )
  FetchContent_MakeAvailable(asiocmake)
endif()

# =============================================================================
# FlatBuffers schema compilation
# =============================================================================
set(FLATBUFFERS_SCHEMA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/schemas)
set(FLATBUFFERS_GENERATED_DIR ${CMAKE_CURRENT_SOURCE_DIR}/wire)

if(EXISTS ${FLATBUFFERS_SCHEMA_DIR}/blob.fbs)
  add_custom_command(
    OUTPUT ${FLATBUFFERS_GENERATED_DIR}/blob_generated.h
    COMMAND flatc --cpp --gen-object-api
            -o ${FLATBUFFERS_GENERATED_DIR}
            ${FLATBUFFERS_SCHEMA_DIR}/blob.fbs
    DEPENDS ${FLATBUFFERS_SCHEMA_DIR}/blob.fbs flatc
    COMMENT "Generating FlatBuffers blob header"
  )
  add_custom_target(flatbuffers_blob_generated
    DEPENDS ${FLATBUFFERS_GENERATED_DIR}/blob_generated.h
  )
endif()

if(EXISTS ${FLATBUFFERS_SCHEMA_DIR}/transport.fbs)
  add_custom_command(
    OUTPUT ${FLATBUFFERS_GENERATED_DIR}/transport_generated.h
    COMMAND flatc --cpp --gen-object-api
            -o ${FLATBUFFERS_GENERATED_DIR}
            ${FLATBUFFERS_SCHEMA_DIR}/transport.fbs
    DEPENDS ${FLATBUFFERS_SCHEMA_DIR}/transport.fbs flatc
    COMMENT "Generating FlatBuffers transport header"
  )
  add_custom_target(flatbuffers_transport_generated
    DEPENDS ${FLATBUFFERS_GENERATED_DIR}/transport_generated.h
  )
endif()

# =============================================================================
# Library target
# =============================================================================
add_library(chromatindb_lib STATIC
  crypto/hash.cpp
  crypto/signing.cpp
  crypto/kem.cpp
  crypto/aead.cpp
  crypto/kdf.cpp
  wire/codec.cpp
  config/config.cpp
  logging/logging.cpp
  identity/identity.cpp
  storage/storage.cpp
  engine/engine.cpp
  net/framing.cpp
  net/protocol.cpp
  net/handshake.cpp
  net/connection.cpp
  net/server.cpp
  sync/sync_protocol.cpp
  peer/peer_manager.cpp
  acl/access_control.cpp
)

target_include_directories(chromatindb_lib PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/..>
)

target_link_libraries(chromatindb_lib PUBLIC
  oqs
  sodium
  flatbuffers
  spdlog::spdlog
  nlohmann_json::nlohmann_json
  mdbx-static
  asio::asio
)
```

### Root CMakeLists.txt (After Refactoring -- Key Sections)
```cmake
cmake_minimum_required(VERSION 3.20)
project(chromatindb LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_BUILD_TYPE Debug)

option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

include(FetchContent)

# All FetchContent declarations (same as current, verbatim)
# ... liboqs, sodium, flatbuffers, Catch2, spdlog, json, libmdbx, asiocmake ...
FetchContent_MakeAvailable(liboqs sodium flatbuffers Catch2 spdlog json libmdbx asiocmake)

# liboqs build-dir include (same as current)
FetchContent_GetProperties(liboqs)
target_include_directories(oqs PUBLIC
  $<BUILD_INTERFACE:${liboqs_BINARY_DIR}/include>
)

# Library component (guards in db/CMakeLists.txt skip re-fetching)
add_subdirectory(db)

# Daemon binary
add_executable(chromatindb db/main.cpp)
target_link_libraries(chromatindb PRIVATE chromatindb_lib)

# Benchmark binary
add_executable(chromatindb_bench bench/bench_main.cpp)
target_link_libraries(chromatindb_bench PRIVATE chromatindb_lib)

# Test target
add_executable(chromatindb_tests
  tests/crypto/test_hash.cpp
  # ... all test files ...
)
target_link_libraries(chromatindb_tests PRIVATE chromatindb_lib Catch2::Catch2WithMain)

include(CTest)
include(Catch)
catch_discover_tests(chromatindb_tests)
```

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| Single monolithic CMakeLists.txt | Subdirectory components with guarded FetchContent | CMake 3.14+ (FetchContent module) | Enables independent builds and reuse |
| `find_package` for all deps | FetchContent for source-level deps | CMake 3.11-3.14 | No system-installed deps needed |
| `ExternalProject_Add` | FetchContent | CMake 3.14 | Configure-time availability (not build-time) |

**Deprecated/outdated:**
- `ExternalProject_Add` for source deps: FetchContent supersedes it (targets available at configure time vs build time)

## Open Questions

1. **Sanitizer options in standalone db/ build**
   - What we know: Root currently has `option(ENABLE_ASAN ...)`. Standalone db/ could also benefit.
   - What's unclear: Whether it's worth the duplication or adds noise.
   - Recommendation: Skip for now (YAGNI). Sanitizers are a consumer concern -- root provides them. If someone builds db/ standalone with sanitizers, they can pass `-DCMAKE_CXX_FLAGS="-fsanitize=address"` directly.

2. **CMake minimum version for db/CMakeLists.txt**
   - What we know: Root uses 3.20. FetchContent needs 3.14. `CMAKE_CXX_STANDARD 20` needs 3.12.
   - Recommendation: Use 3.20 (match root). No reason to diverge.

3. **install() rules for db/**
   - What we know: No install rules exist today. This is a pre-MVP project.
   - Recommendation: Skip (YAGNI). No consumers need install() yet.

4. **Source paths in db/CMakeLists.txt: relative or absolute?**
   - What we know: Source files in `add_library` can use relative paths (relative to `CMAKE_CURRENT_SOURCE_DIR`). Since db/CMakeLists.txt is in `db/`, files are `crypto/hash.cpp` not `db/crypto/hash.cpp`.
   - Recommendation: Use relative paths (cleaner, standard). Already shown in code example above.

## Sources

### Primary (HIGH confidence)
- Current project CMakeLists.txt (235 lines) -- directly inspected, all patterns verified
- Current project source tree -- all include paths verified via grep
- CMake documentation: `add_subdirectory`, `FetchContent`, `project()` scoping, `CMAKE_CURRENT_SOURCE_DIR` -- well-known stable CMake behavior (unchanged since CMake 3.14+)

### Secondary (MEDIUM confidence)
- `if(NOT TARGET)` guard pattern -- widely documented CMake idiom for composable subdirectories

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - pure CMake, no new tools, all patterns verified against existing codebase
- Architecture: HIGH - mechanical restructuring with locked decisions covering every significant choice
- Pitfalls: HIGH - all identified pitfalls are well-known CMake issues, verified against project structure

**Research date:** 2026-03-14
**Valid until:** 2026-04-14 (stable -- CMake build patterns don't change rapidly)
