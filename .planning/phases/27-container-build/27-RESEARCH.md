# Phase 27: Container Build - Research

**Researched:** 2026-03-15
**Domain:** Docker multi-stage build for C++20 project with FetchContent dependencies
**Confidence:** HIGH

## Summary

Phase 27 creates a multi-stage Dockerfile that builds chromatindb and chromatindb_loadgen binaries from source using FetchContent-managed dependencies, then copies them into a minimal debian:bookworm-slim runtime image. The build is straightforward: all 8 dependencies are already pinned in CMakeLists.txt with GIT_SHALLOW, and the daemon CLI already supports the exact flags needed for container operation (`--config`, `--data-dir`, `--log-level`).

The primary technical concerns are: (1) GCC 12 on bookworm has a known `-Wrestrict` false positive bug with C++20 and `-O2`/`-O3`, which affects Release builds but is a warning not an error -- suppress it or use GCC from a newer base; (2) the binary links against `libcrypto.so.3` at runtime because liboqs defaults to `OQS_USE_OPENSSL=ON`, so the runtime image needs `libssl3`; (3) the "copy CMakeLists first, configure, then copy source" layer caching strategy will fail because CMake validates source file existence at configure time -- use `--mount=type=cache` for the FetchContent directory instead.

**Primary recommendation:** Use `debian:bookworm` (not slim) as build stage, `debian:bookworm-slim` as runtime stage. GCC 12 from bookworm is sufficient for C++20 coroutines. Use `--mount=type=cache` for FetchContent caching. Port 4200 (matches existing default). Strip binaries in final image.

<user_constraints>

## User Constraints (from CONTEXT.md)

### Locked Decisions
- Data directory at `/data` inside the container, declared as VOLUME
- Config passed via volume mount: host config.json mounted into `/data/config.json`
- Daemon invoked with `--config /data/config.json --data-dir /data`
- Non-root user: create `chromatindb` user/group, data dir owned by this user
- ENTRYPOINT + CMD pattern: `ENTRYPOINT ["chromatindb"]`, `CMD ["run", "--data-dir", "/data", "--log-level", "debug"]`
- Default log level: `debug` (benchmark/validation context)
- Auto-generates identity on first run (load_or_generate handles this already)
- User can override CMD: `docker run chromatindb keygen --data-dir /data`
- TCP health check on the listen port (lightweight, no extra tooling)
- CMakeLists-first layer strategy for build caching (intent: source-only changes don't re-download deps)
- Binaries stripped in the final image
- .dockerignore excludes build/, .planning/, .git/, .claude/, *.md

### Claude's Discretion
- Port number selection
- Compiler version selection (whichever gcc works cleanest with C++20 coroutines on bookworm)
- Build stage base image specifics
- Exact HEALTHCHECK interval/timeout/retries
- cmake parallelism flags in Docker build

### Deferred Ideas (OUT OF SCOPE)
None

</user_constraints>

<phase_requirements>

## Phase Requirements

| ID | Description | Research Support |
|----|-------------|-----------------|
| DOCK-01 | Multi-stage Dockerfile produces chromatindb and chromatindb_loadgen binaries in debian:bookworm-slim runtime image with Release build | Full research coverage: base images, compiler, dependencies, multi-stage pattern, stripping, runtime libs |

</phase_requirements>

## Standard Stack

### Core
| Tool | Version | Purpose | Why Standard |
|------|---------|---------|--------------|
| debian:bookworm | latest | Build stage base image | Matches runtime bookworm-slim; GCC 12 has full C++20 coroutine support |
| debian:bookworm-slim | latest | Runtime stage base image | User-specified; minimal footprint (~80 MB) |
| GCC 12 | 12.2 (bookworm default) | C++20 compiler | Default in bookworm, full coroutine support, project already builds with it |
| CMake | 3.25 (bookworm) | Build system | Project requires >= 3.20; bookworm provides 3.25 |

### Build Dependencies (apt packages for build stage)
| Package | Purpose |
|---------|---------|
| build-essential | GCC, g++, make |
| cmake | Build system (3.25 in bookworm) |
| git | FetchContent git clones (7 of 8 deps use git) |
| libssl-dev | liboqs build dependency (OQS_USE_OPENSSL=ON default) |
| pkg-config | libsodium-cmake wrapper needs it |
| ninja-build | Optional: faster builds than make (recommended) |

### Runtime Dependencies (apt packages for runtime stage)
| Package | Purpose |
|---------|---------|
| libssl3 | libcrypto.so.3 -- liboqs runtime dependency |
| libstdc++6 | C++ standard library (already in bookworm-slim) |

### Alternatives Considered
| Instead of | Could Use | Tradeoff |
|------------|-----------|----------|
| GCC 12 (bookworm) | GCC 14 (debian:trixie build stage) | Eliminates -Wrestrict false positive, but trixie is newer/less tested as build base |
| OQS_USE_OPENSSL=ON | OQS_USE_OPENSSL=OFF | Eliminates libssl3 runtime dep but changes crypto backend from proven config |
| ninja-build | make (default) | Make is already installed via build-essential; ninja ~30% faster for this project size |

## Architecture Patterns

### Recommended Dockerfile Structure
```
Dockerfile              # Multi-stage build
.dockerignore           # Excludes build artifacts, planning docs, git
```

### Pattern 1: Multi-Stage Build with Cache Mount
**What:** Two-stage Dockerfile -- build stage compiles from source, runtime stage copies only binaries and runtime libs.
**When to use:** Always for C++ projects -- build tools add ~1 GB to image size.

```dockerfile
# syntax=docker/dockerfile:1

# ---- Build stage ----
FROM debian:bookworm AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git libssl-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY db/ db/
COPY bench/ bench/

RUN --mount=type=cache,target=/src/build/_deps \
    cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=Release \
      -DFETCHCONTENT_QUIET=ON \
    && cmake --build build --target chromatindb chromatindb_loadgen

RUN strip build/chromatindb build/chromatindb_loadgen

# ---- Runtime stage ----
FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    && rm -rf /var/lib/apt/lists/*

RUN groupadd -r chromatindb && useradd -r -g chromatindb chromatindb

COPY --from=builder /src/build/chromatindb /usr/local/bin/
COPY --from=builder /src/build/chromatindb_loadgen /usr/local/bin/

RUN mkdir -p /data && chown chromatindb:chromatindb /data
VOLUME /data

USER chromatindb
EXPOSE 4200

HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
    CMD bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-' || exit 1

ENTRYPOINT ["chromatindb"]
CMD ["run", "--data-dir", "/data", "--log-level", "debug"]
```

### Pattern 2: TCP Health Check without Extra Tools
**What:** Use bash's built-in `/dev/tcp` pseudo-device for health checks instead of curl/wget/netcat.
**When to use:** Minimal images where you don't want to install extra packages.

```dockerfile
HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
    CMD bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&-' || exit 1
```

Note: This requires bash in the runtime image. debian:bookworm-slim includes bash.

### Pattern 3: Non-Root User with Volume Ownership
**What:** Create dedicated user/group, ensure data volume is owned by that user.
**When to use:** Always for daemon containers -- principle of least privilege.

```dockerfile
RUN groupadd -r chromatindb && useradd -r -g chromatindb chromatindb
RUN mkdir -p /data && chown chromatindb:chromatindb /data
VOLUME /data
USER chromatindb
```

### Anti-Patterns to Avoid
- **Running as root in production:** Always create a non-root user for daemon processes.
- **Installing build tools in runtime image:** Multi-stage build eliminates this.
- **Using `latest` tag without pinning:** The project uses `debian:bookworm-slim` (codename-pinned), which is correct.
- **Forgetting to strip binaries:** Debug chromatindb binary is 27 MB; stripped will be ~5-8 MB.
- **Using `cmake --build . --parallel`:** Project memory says this eats all RAM. In Docker, use explicit `-j` or no parallelism flag (single job is safer in constrained containers, and build is one-time cost).

## Don't Hand-Roll

| Problem | Don't Build | Use Instead | Why |
|---------|-------------|-------------|-----|
| TCP health check | Custom health endpoint binary | Bash /dev/tcp | Zero dependencies, built into debian:bookworm-slim |
| User creation | Manual uid/gid management | groupadd/useradd -r | System user conventions, correct /etc/passwd entries |
| Binary stripping | Build with CMAKE_INSTALL_STRIP | `strip` command post-build | Simpler, works regardless of cmake install config |
| Dependency caching | Custom download scripts | --mount=type=cache | Docker BuildKit native, persists across builds |

## Common Pitfalls

### Pitfall 1: CMakeLists-First Layer Strategy Fails at Configure Time
**What goes wrong:** Copying only CMakeLists.txt files and running `cmake configure` to pre-fetch dependencies will fail because CMake validates that source files listed in `add_library()` and `add_executable()` exist at configure time, not just build time.
**Why it happens:** The root CMakeLists.txt references `db/main.cpp` in `add_executable(chromatindb db/main.cpp)` and `db/CMakeLists.txt` lists ~18 .cpp files in `add_library(chromatindb_lib ...)`. CMake errors out if these don't exist.
**How to avoid:** Use `--mount=type=cache,target=/src/build/_deps` on the build RUN command instead. This caches FetchContent downloads across Docker builds even when the layer is invalidated by source changes. Achieves the same goal (source-only changes don't re-download 8 deps) without the configure failure.
**Warning signs:** `CMake Error: Cannot find source file: db/main.cpp` during Docker build.

### Pitfall 2: Missing libcrypto.so.3 in Runtime Image
**What goes wrong:** Container crashes on startup with "error while loading shared libraries: libcrypto.so.3: cannot open shared object file."
**Why it happens:** liboqs links against OpenSSL's libcrypto by default (`OQS_USE_OPENSSL=ON`). The build stage has libssl-dev but the runtime stage has no SSL libraries.
**How to avoid:** Install `libssl3` in the runtime stage. Verified by running `ldd` on the current build -- the only non-standard runtime dependency is `libcrypto.so.3`.
**Warning signs:** Binary starts fine locally but crashes in container.

### Pitfall 3: GCC 12 -Wrestrict False Positive with -O2/-O3
**What goes wrong:** Compiler warnings (not errors) about impossible memory overlap in code like `std::string s = "a"`. Only triggers with C++20 + optimization enabled.
**Why it happens:** GCC Bug #105329 in GCC 12.2 (bookworm default). Fixed in GCC 13.
**How to avoid:** Add `-Wno-restrict` to CMAKE_CXX_FLAGS for the Docker build, or accept the warnings (they don't affect correctness). If `-Werror` is ever enabled, this would become a build failure.
**Warning signs:** Warnings during Release build that don't appear in Debug builds.

### Pitfall 4: FlatBuffers Schema Compilation Requires Source Paths
**What goes wrong:** FlatBuffers codegen custom commands reference `db/schemas/blob.fbs` and `db/schemas/transport.fbs`. If schema files aren't copied, generated headers won't be created.
**Why it happens:** The `db/CMakeLists.txt` has `if(EXISTS ${FLATBUFFERS_SCHEMA_DIR}/blob.fbs)` guards, so missing schemas silently skip codegen rather than erroring -- but then compilation fails when source files include the generated headers.
**How to avoid:** Ensure the COPY commands include `db/schemas/` directory. The full `COPY db/ db/` handles this.

### Pitfall 5: chromatindb_loadgen Binary Does Not Exist Yet
**What goes wrong:** The Dockerfile tries to build/copy `chromatindb_loadgen` but the target doesn't exist in CMakeLists.txt -- Phase 28 adds it.
**Why it happens:** Phase 28 (Load Generation) creates the loadgen binary. Phase 27 runs first.
**How to avoid:** Either (a) build only `chromatindb` and `chromatindb_bench` in Phase 27 and update the Dockerfile in Phase 28 to add loadgen, or (b) add a placeholder loadgen target in Phase 27. Option (a) is cleaner -- build what exists, iterate.
**Warning signs:** `cmake --build build --target chromatindb_loadgen` fails with "No rule to make target."

### Pitfall 6: Data Directory Permissions with Volume Mounts
**What goes wrong:** Container starts as `chromatindb` user but can't write to `/data` when a host volume is mounted, because the host directory may be owned by a different uid.
**Why it happens:** Docker volume mounts preserve host permissions. The container user's uid may not match the host directory owner.
**How to avoid:** For named volumes (docker-compose), Docker handles ownership. For bind mounts, document that the host directory must be writable by uid of the chromatindb user (typically 999 or similar system uid). This is standard Docker behavior.

## Code Examples

### Verified: Daemon CLI Interface
```cpp
// Source: db/main.cpp (lines 32-46)
// Usage: chromatindb <command> [options]
// Commands: run, keygen, version
// Run options: --config <path>, --data-dir <path>, --log-level <level>
// Keygen options: --data-dir <path>, --force
```

The ENTRYPOINT/CMD pattern maps directly:
- `ENTRYPOINT ["chromatindb"]` -- the binary
- `CMD ["run", "--data-dir", "/data", "--log-level", "debug"]` -- default args
- Override: `docker run chromatindb keygen --data-dir /data`
- Config: `docker run -v ./config.json:/data/config.json chromatindb run --config /data/config.json --data-dir /data`

### Verified: Default Bind Address
```cpp
// Source: db/config/config.h (line 12)
std::string bind_address = "0.0.0.0:4200";
```

Port 4200 is the default. The daemon already binds to 0.0.0.0, which is correct for container networking. No changes needed.

### Verified: Runtime Library Dependencies
```
# From: ldd build/chromatindb (host build)
libcrypto.so.3    # liboqs -> OpenSSL
libstdc++.so.6    # C++ standard library (in bookworm-slim)
libm.so.6         # Math library (in bookworm-slim)
libgcc_s.so.1     # GCC runtime (in bookworm-slim)
libc.so.6         # C library (in bookworm-slim)
```

Only `libcrypto.so.3` needs explicit installation via `libssl3` package.

### Verified: FetchContent Dependencies (8 total)
```
# From: CMakeLists.txt
liboqs        0.15.0        GIT_SHALLOW (ML-DSA-87, ML-KEM-1024)
sodium        master        GIT_SHALLOW (ChaCha20-Poly1305, HKDF)
flatbuffers   v25.2.10      GIT_SHALLOW (wire format + flatc codegen)
Catch2        v3.7.1        GIT_SHALLOW (test framework -- NOT needed for daemon)
spdlog        v1.15.1       GIT_SHALLOW (logging)
json          v3.11.3       URL download (config parsing, ~200KB)
libmdbx       v0.13.11      GIT_SHALLOW (persistent storage)
asiocmake     main          GIT_SHALLOW (async networking)
```

All deps are fetched during cmake configure. 7 use git clone, 1 uses URL download. Total fetch time: ~30-60s on first build.

## State of the Art

| Old Approach | Current Approach | When Changed | Impact |
|--------------|------------------|--------------|--------|
| COPY CMakeLists + configure for dep caching | --mount=type=cache for _deps dir | Docker BuildKit (2020+) | More reliable, no source file existence issues |
| Multi-RUN for apt layers | Single RUN with && chain | Docker best practice | Fewer layers, smaller image |
| curl/wget for health checks | bash /dev/tcp | Always available | No extra packages in slim image |

## Discretion Recommendations

### Port Number: 4200
**Rationale:** Already the default in `config.h` (`bind_address = "0.0.0.0:4200"`). Using a different port would require config overrides for no benefit. 4200 is above 1024 (no root needed), not commonly used by other services.

### Compiler: GCC 12 from bookworm default packages
**Rationale:** Already proven to build this project. Full C++20 coroutine support (enabled by default since GCC 11). The known Bug #105329 (-Wrestrict false positive) only produces warnings, not errors. Adding `-Wno-restrict` for Release builds is a trivial mitigation. Using a different base image (trixie) for GCC 14 adds unnecessary complexity.

### Build Stage Base: debian:bookworm (not slim)
**Rationale:** The non-slim variant includes more pre-installed packages, reducing apt install time. The build stage is discarded anyway, so image size doesn't matter. Using bookworm (same release as runtime) ensures ABI compatibility.

### HEALTHCHECK Timing: interval=30s, timeout=5s, retries=3
**Rationale:** 30s interval avoids excessive overhead while catching failures within ~90s. 5s timeout is generous for a TCP connect. 3 retries prevents flapping. Start period of 10s gives the daemon time to initialize and bind.

### CMake Parallelism: No --parallel flag (single-threaded build)
**Rationale:** User preference says "Never use `cmake --build . --parallel` -- eats all memory." In Docker, memory constraints are even tighter. The build is a one-time cost. Single-threaded is safe and reliable. Alternative: `cmake --build build -j2` if build time is a concern (liboqs alone takes ~5 min single-threaded).

## Open Questions

1. **chromatindb_loadgen does not exist yet**
   - What we know: Phase 28 creates the loadgen binary. Phase 27's DOCK-01 requirement says the image must include it.
   - What's unclear: Should Phase 27 create a placeholder, or should the Dockerfile be updated in Phase 28?
   - Recommendation: Build what exists now (chromatindb + chromatindb_bench). Phase 28 updates the Dockerfile to add the loadgen target. This keeps Phase 27 self-contained and avoids creating dead code. The requirement text says "produces chromatindb and chromatindb_loadgen" -- interpret as "the final Dockerfile" not "the Phase 27 Dockerfile."

2. **CMakeLists-first layer caching won't work as described**
   - What we know: CMake validates source file existence at configure time. Copying only CMakeLists.txt files and running configure will fail.
   - What's unclear: Whether the user strongly prefers the layer approach or just wants the outcome (fast rebuilds on source changes).
   - Recommendation: Use `--mount=type=cache` for the FetchContent directory. Achieves identical caching behavior without the configure-time failure. Document this deviation from the CONTEXT.md description.

## Sources

### Primary (HIGH confidence)
- Project source code: CMakeLists.txt, db/CMakeLists.txt, db/main.cpp, db/config/config.h -- verified build targets, CLI flags, default port, dependencies
- `ldd build/chromatindb` -- verified runtime library dependencies (libcrypto.so.3)
- `file build/chromatindb` -- verified ELF x86-64, not stripped, 27 MB debug build

### Secondary (MEDIUM confidence)
- [Debian Bookworm GCC packages](https://packages.debian.org/bookworm/gcc) -- GCC 12 is default compiler
- [GCC 12 Bug #105329](https://dev.to/pgradot/just-in-case-debian-bookworm-comes-with-a-buggy-gcc-2e9b) -- -Wrestrict false positive with C++20 + O2/O3
- [Docker multi-stage builds](https://docs.docker.com/build/building/multi-stage/) -- official documentation
- [Docker cache mounts](https://docs.docker.com/build/cache/optimize/) -- --mount=type=cache for build dependencies
- [liboqs getting started](https://openquantumsafe.org/liboqs/getting-started.html) -- build dependencies, OQS_USE_OPENSSL option
- [Debian bookworm libssl3](https://packages.debian.org/bookworm/libssl3) -- runtime libcrypto package
- [Debian 13 trixie release](https://www.debian.org/News/2025/20250809) -- GCC 14 available but unnecessary

### Tertiary (LOW confidence)
- None -- all findings verified with primary or secondary sources

## Metadata

**Confidence breakdown:**
- Standard stack: HIGH - verified against actual project build, ldd output, package repositories
- Architecture: HIGH - multi-stage Docker is well-established; patterns verified against Docker official docs
- Pitfalls: HIGH - each pitfall verified by examining actual source code and build artifacts
- Discretion items: HIGH - recommendations based on existing project defaults and user preferences

**Research date:** 2026-03-15
**Valid until:** 2026-04-15 (stable domain, debian bookworm LTS)
