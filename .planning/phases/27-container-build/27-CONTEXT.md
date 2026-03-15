# Phase 27: Container Build - Context

**Gathered:** 2026-03-15
**Status:** Ready for planning

<domain>
## Phase Boundary

Multi-stage Dockerfile producing Release binaries (`chromatindb` and `chromatindb_loadgen`) in debian:bookworm-slim runtime image. The container starts, listens on a configured port, and accepts connections from host-based peers. This phase covers the Dockerfile, .dockerignore, and basic container operability — not multi-node topology (Phase 29) or load generation code (Phase 28).

</domain>

<decisions>
## Implementation Decisions

### Container data layout
- Data directory at `/data` inside the container, declared as VOLUME
- Config passed via volume mount: host config.json mounted into `/data/config.json`
- Daemon invoked with `--config /data/config.json --data-dir /data`
- Non-root user: create `chromatindb` user/group, data dir owned by this user

### Entrypoint behavior
- ENTRYPOINT + CMD pattern: `ENTRYPOINT ["chromatindb"]`, `CMD ["run", "--data-dir", "/data", "--log-level", "debug"]`
- Default log level: `debug` (benchmark/validation context — more visibility by default)
- Auto-generates identity on first run (load_or_generate handles this already)
- User can override CMD: `docker run chromatindb keygen --data-dir /data`
- TCP health check on the listen port (lightweight, no extra tooling)

### Build layer caching
- CMakeLists-first layer strategy: copy only CMakeLists.txt + db/CMakeLists.txt first, run cmake configure to fetch deps, then copy source and build
- Source-only changes don't re-download the 8 FetchContent dependencies
- Binaries stripped in the final image (smaller, debug symbols not needed for benchmarks)
- .dockerignore excludes build/, .planning/, .git/, .claude/, *.md

### Claude's Discretion
- Port number selection (user said "you decide")
- Compiler version selection (whichever gcc works cleanest with C++20 coroutines on bookworm)
- Build stage base image specifics
- Exact HEALTHCHECK interval/timeout/retries
- cmake parallelism flags in Docker build

</decisions>

<specifics>
## Specific Ideas

- Log level debug by default because this is a benchmarking/validation milestone, not production deployment
- ENTRYPOINT/CMD split allows both `docker run chromatindb` (daemon) and `docker run chromatindb keygen` (utility)

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `db/main.cpp`: Daemon CLI already supports `--config`, `--data-dir`, `--log-level` — container just needs to pass these
- `load_or_generate()` in identity module handles first-run keygen automatically

### Established Patterns
- All deps via FetchContent with pinned versions (8 repos) — Dockerfile must replicate this
- `db/CMakeLists.txt` has guarded `if(NOT TARGET ...)` blocks for composability — copy both CMakeLists files for the cache layer
- FlatBuffers schema compilation happens during build via custom commands

### Integration Points
- Daemon listens on bind_address from config (default 0.0.0.0:port)
- Data dir contains: node.key, node.pub, mdbx database files
- Config file is JSON with bootstrap_peers, allowed_keys, max_peers, sync_interval, etc.
- `chromatindb_loadgen` binary doesn't exist yet — Phase 28 adds it. Dockerfile may need update after Phase 28.

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 27-container-build*
*Context gathered: 2026-03-15*
