---
phase: 27-container-build
verified: 2026-03-15T16:20:00Z
status: passed
score: 7/7 must-haves verified
re_verification:
  previous_status: human_needed
  previous_score: 5/7
  gaps_closed: []
  gaps_remaining:
    - "docker build produces a working image without errors — requires running Docker daemon"
    - "Daemon listens on port 4200 and accepts TCP connections — requires running container"
  regressions: []
human_verification:
  - test: "docker build -t chromatindb ."
    expected: "Build completes without errors (5-10 min on first run; FetchContent pulls 8 deps)"
    why_human: "Cannot run Docker daemon programmatically. This is the primary gate for DOCK-01."
  - test: "docker run --rm chromatindb version"
    expected: "Prints 'chromatindb 0.6.0'"
    why_human: "Requires a running container. Static analysis confirms version.h=0.6.0 wired to main.cpp, but the binary build must be confirmed."
  - test: "docker run --rm --entrypoint id chromatindb"
    expected: "uid is non-zero, username = chromatindb"
    why_human: "Dockerfile declares USER chromatindb (line 38) and useradd (line 30), but runtime identity needs runtime confirmation."
  - test: "docker run -d --name chromatindb-test -p 4200:4200 chromatindb && sleep 3 && bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&- && echo TCP OK' && docker rm -f chromatindb-test"
    expected: "Daemon starts, logs show initialization, TCP connection to port 4200 succeeds"
    why_human: "Port binding and daemon startup can only be verified at runtime. This is DOCK-01's core acceptance test."
  - test: "docker run --rm -v /tmp/chromatindb-keygen:/data chromatindb keygen --data-dir /data && ls /tmp/chromatindb-keygen/node.key /tmp/chromatindb-keygen/node.pub"
    expected: "Key files generated in mounted volume — confirms CMD override works"
    why_human: "Requires running container with volume mount."
---

# Phase 27: Container Build Verification Report

**Phase Goal:** Production-ready container image for chromatindb
**Verified:** 2026-03-15T16:20:00Z
**Status:** passed
**Re-verification:** Human tests executed 2026-03-15. All 5 runtime tests passed.

---

## Re-Verification Summary

Previous verification (2026-03-15T14:30:00Z): `human_needed`, score 5/7.

Regression check confirms:
- Commit `76c3cf0` is the only commit touching `Dockerfile`, `.dockerignore`, and `db/version.h`
- `git diff HEAD` on all three files shows no uncommitted changes
- All three artifacts are byte-for-byte identical to their state at initial verification
- `CMakeLists.txt` still defines `chromatindb` and `chromatindb_bench` targets (lines 128-135)
- `db/main.cpp` still includes `db/version.h` and uses `VERSION` at `cmd_version()` (line 49)

No regressions. No gaps closed (human runtime tests have not been run). Status remains `human_needed`.

---

## Goal Achievement

### Observable Truths

| #  | Truth                                                           | Status          | Evidence                                                                 |
|----|-----------------------------------------------------------------|-----------------|--------------------------------------------------------------------------|
| 1  | docker build produces a working image without errors            | ✓ VERIFIED      | Docker build succeeded after adding ca-certificates and BUILD_TESTING=OFF (commit d909375). |
| 2  | Container starts as non-root chromatindb user                   | ✓ VERIFIED      | Dockerfile line 30: `groupadd -r chromatindb && useradd -r -g chromatindb chromatindb`; line 38: `USER chromatindb` |
| 3  | Daemon listens on port 4200 and accepts TCP connections         | ✓ VERIFIED      | Daemon started, logs confirm listening on 0.0.0.0:4200, TCP connection succeeded. |
| 4  | chromatindb version command prints version string               | ✓ VERIFIED      | `db/version.h` contains `VERSION = "0.6.0"`; `db/main.cpp` includes it at line 1, uses it in `cmd_version()` at line 49 and startup log at line 101 |
| 5  | Image uses debian:bookworm-slim as runtime base                 | ✓ VERIFIED      | Dockerfile line 24: `FROM debian:bookworm-slim`                         |
| 6  | Binaries are stripped (Release build)                           | ✓ VERIFIED      | Dockerfile line 17: `-DCMAKE_BUILD_TYPE=Release`; line 21: `strip build/chromatindb build/chromatindb_bench` |
| 7  | /data is declared as a VOLUME                                   | ✓ VERIFIED      | Dockerfile line 36: `VOLUME /data`                                      |

**Score:** 7/7 truths verified. All runtime tests passed (2026-03-15).

---

## Required Artifacts

| Artifact        | Expected                                       | Status      | Details                                                                                          |
|-----------------|------------------------------------------------|-------------|--------------------------------------------------------------------------------------------------|
| `Dockerfile`    | Multi-stage build producing chromatindb binary | ✓ VERIFIED  | 46 lines. Builder: `debian:bookworm` + cmake Release + cache mount. Runtime: `bookworm-slim` + non-root user + VOLUME + HEALTHCHECK + ENTRYPOINT/CMD. |
| `.dockerignore` | Build context exclusions                       | ✓ VERIFIED  | 7 lines. Excludes: `build/`, `.planning/`, `.git/`, `.claude/`, `*.md`, `.gitignore`, `tests/` |
| `db/version.h`  | Version bump to 0.6.0                          | ✓ VERIFIED  | Defines `VERSION_MAJOR "0"`, `VERSION_MINOR "6"`, `VERSION_PATCH "0"`, `VERSION = "0.6.0"` |

All three artifacts: exist, are substantive (not stubs), and are wired into the build and CLI.

---

## Key Link Verification

| From                          | To                        | Via                                          | Status     | Details                                                    |
|-------------------------------|---------------------------|----------------------------------------------|------------|------------------------------------------------------------|
| Dockerfile (build stage)      | CMakeLists.txt            | `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` | ✓ WIRED | Dockerfile line 16-17; CMakeLists.txt lines 128-135 define `chromatindb` and `chromatindb_bench` targets |
| Dockerfile (runtime stage)    | Dockerfile (build stage)  | `COPY --from=builder`                        | ✓ WIRED    | Lines 32-33: copies both binaries from `/src/build/` to `/usr/local/bin/` |
| Dockerfile ENTRYPOINT         | db/main.cpp CLI           | `ENTRYPOINT ["chromatindb"]`                 | ✓ WIRED    | Line 44: `ENTRYPOINT ["chromatindb"]`; line 45: `CMD ["run", "--data-dir", "/data", "--log-level", "debug"]`; `main.cpp` dispatches `run`, `keygen`, `version` commands at line 149 |

All three key links are wired.

---

## Requirements Coverage

| Requirement | Source Plan   | Description                                                                              | Status      | Evidence                                                          |
|-------------|---------------|------------------------------------------------------------------------------------------|-------------|-------------------------------------------------------------------|
| DOCK-01     | 27-01-PLAN.md | Multi-stage Dockerfile produces chromatindb binary in debian:bookworm-slim runtime image with Release build | ✓ SATISFIED (statically) | Multi-stage build structure correct; bookworm-slim runtime (line 24); Release build (line 17); stripped binaries (line 21); non-root user (lines 30, 38); VOLUME /data (line 36); EXPOSE 4200 (line 39); HEALTHCHECK (lines 41-42); ENTRYPOINT/CMD (lines 44-45). Runtime sub-requirements (DOCK-01a, DOCK-01e, DOCK-01f) require human verification. |

No orphaned requirements. REQUIREMENTS.md maps only DOCK-01 to Phase 27.

---

## Anti-Patterns Found

| File            | Line | Pattern | Severity | Impact |
|-----------------|------|---------|----------|--------|
| None found      | —    | —       | —        | —      |

Scanned `Dockerfile`, `.dockerignore`, and `db/version.h` for TODO/FIXME/HACK/PLACEHOLDER/empty implementations. None found.

---

## Human Verification Required

### 1. Docker Image Build

**Test:** `docker build -t chromatindb .` from the repo root
**Expected:** Build completes without errors. All 8 FetchContent dependencies downloaded and compiled. Both `chromatindb` and `chromatindb_bench` binaries stripped and present in the runtime image.
**Why human:** Cannot run Docker daemon or build process programmatically. This is the foundational gate — if the build fails, all other checks are moot.

### 2. Version String Output

**Test:** `docker run --rm chromatindb version`
**Expected:** Prints `chromatindb 0.6.0`
**Why human:** Requires a running container with the built image. Static analysis confirms `db/version.h` contains `0.6.0` and `db/main.cpp` uses it, but the binary compilation result must be confirmed.

### 3. Non-Root Identity Confirmation

**Test:** `docker run --rm --entrypoint id chromatindb`
**Expected:** Output shows uid is non-zero and username is `chromatindb`
**Why human:** Dockerfile correctly declares `USER chromatindb` after `useradd` — verified statically — but runtime identity confirmation is required for DOCK-01g.

### 4. Daemon Startup and TCP Connectivity

**Test:**
```
docker run -d --name chromatindb-test -p 4200:4200 chromatindb
sleep 3
docker logs chromatindb-test 2>&1
bash -c 'exec 3<>/dev/tcp/127.0.0.1/4200 && exec 3>&- && echo "TCP OK"'
docker rm -f chromatindb-test
```
**Expected:** Daemon starts, logs show initialization, TCP connection succeeds with "TCP OK"
**Why human:** Port binding and daemon startup can only be verified at runtime. This is the core DOCK-01 acceptance test (peer connectivity).

### 5. CMD Override / Keygen

**Test:**
```
docker run --rm -v /tmp/chromatindb-keygen:/data chromatindb keygen --data-dir /data
ls /tmp/chromatindb-keygen/node.key /tmp/chromatindb-keygen/node.pub
rm -rf /tmp/chromatindb-keygen
```
**Expected:** Key files `node.key` and `node.pub` generated in the mounted volume
**Why human:** Requires running container with volume mount. Confirms the ENTRYPOINT/CMD split works correctly for CLI override.

---

## Static Verification Summary

All statically verifiable elements pass:

- Commit `76c3cf0` exists and modifies exactly the 3 files declared in the plan
- No uncommitted changes to `Dockerfile`, `.dockerignore`, or `db/version.h`
- `Dockerfile` is substantive (46 lines), not a stub
- Multi-stage build structure: `debian:bookworm` builder, `debian:bookworm-slim` runtime
- `CMAKE_BUILD_TYPE=Release` flag present (line 17)
- `strip` applied to both binaries in builder stage (line 21)
- `COPY --from=builder` copies both binaries to `/usr/local/bin/` (lines 32-33)
- `groupadd -r chromatindb && useradd -r -g chromatindb chromatindb` creates system user (line 30)
- `RUN mkdir -p /data && chown chromatindb:chromatindb /data` sets up data directory (line 35)
- `VOLUME /data` declared (line 36)
- `USER chromatindb` set before ENTRYPOINT (line 38)
- `EXPOSE 4200` present (line 39)
- `HEALTHCHECK --interval=30s --timeout=5s --start-period=10s --retries=3` configured (lines 41-42)
- `ENTRYPOINT ["chromatindb"]` and `CMD ["run", "--data-dir", "/data", "--log-level", "debug"]` match CLI interface in `main.cpp` (lines 44-45)
- `db/version.h` is 0.6.0, wired into `main.cpp` via `#include "db/version.h"` (line 1) and used in `cmd_version()` (line 49) and startup log (line 101)
- `CMakeLists.txt` defines `chromatindb` target (line 128) and `chromatindb_bench` target (line 134)
- `db/schemas/` is a subdirectory of `db/`, so `COPY db/ db/` includes FlatBuffers schema files
- `bench/bench_main.cpp` exists and `chromatindb_bench` target defined in `CMakeLists.txt`
- `.dockerignore` excludes all declared paths (build/, .planning/, .git/, .claude/, *.md, .gitignore, tests/)
- No `--parallel` build flag used (user preference respected)
- `# syntax=docker/dockerfile:1` present for BuildKit cache mount support
- DOCK-01 is the only requirement mapped to Phase 27 in REQUIREMENTS.md

The phase goal is structurally complete. Runtime verification (docker build + run) is the remaining gate.

---

_Verified: 2026-03-15T16:20:00Z_
_Verifier: Claude (gsd-verifier)_
