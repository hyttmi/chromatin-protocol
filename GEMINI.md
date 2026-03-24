# GEMINI.md -- chromatindb

Instructions for AI assistants working on this project. Read this before doing anything.

## Working Philosophy

**Think first, act second.** Do not rush into implementation. Before writing code:

1. **Understand the problem.** Read the relevant source files. Understand how the existing code works before changing it.
2. **Verify your assumptions.** If you think a function exists, grep for it. If you think a file has a certain structure, read it. Do not guess.
3. **Ask when uncertain.** If the task is ambiguous or you're unsure about the right approach, ask. A wrong implementation costs more than a question.
4. **Plan non-trivial changes.** For anything beyond a one-file edit, outline your approach before writing code.
5. **Verify after implementation.** Build and run tests to confirm your change works. Never claim "done" without evidence.

This project was built with a deterministic, deliberate workflow. Every phase was planned, reviewed, executed, and verified. Maintain that standard.

## What This Is

chromatindb is a decentralized, post-quantum secure database node. It stores signed blobs in cryptographically-owned namespaces and replicates them across peers. NOT a library -- a standalone daemon/binary.

Three-layer architecture (bottom-up):
- **Layer 1 (SHIPPED v1.1.0): chromatindb** -- production-hardened database node. C++20, 500+ unit tests, 54 Docker integration tests. ASAN/TSAN/UBSAN clean.
- **Layer 2 (SHIPPED v1.2.0): Relay** -- PQ-authenticated message filter + UDS forwarder. Same wire protocol as node. Accepts TCP clients, PQ handshake, filters message types, forwards to node via UDS.
- **Layer 3 (FUTURE): Client/SDK** -- Python SDK for developers.

**Product direction:** Storage vault -- seamless blob replication between nodes with fetch-from-anywhere. PQ crypto is a compliance selling point.

## Tech Stack

| Component | Library | Purpose |
|-----------|---------|---------|
| Language | C++20 | Standard throughout |
| Build | CMake + FetchContent | All deps fetched at build time (always latest versions) |
| PQ crypto | liboqs | ML-DSA-87 (signing), ML-KEM-1024 (key exchange), SHA3-256 (hashing) |
| Symmetric crypto | libsodium | ChaCha20-Poly1305 (AEAD), HKDF-SHA256 (KDF) |
| Storage | libmdbx | LMDB-compatible, crash-safe key-value store |
| Wire format | FlatBuffers | Deterministic encoding for signing |
| Networking | Standalone Asio | C++20 coroutines, thread_pool |
| Sync fingerprints | xxHash (XXH3) | Fast non-cryptographic hash |
| Testing | Catch2 | Unit tests in db/tests/ |
| Logging | spdlog | Structured + file logging |
| Config | nlohmann/json | JSON config parsing |

**Explicitly excluded:** No OpenSSL (libssl removed from Dockerfile), no DHT (proven unreliable in prior projects), no Boost.

## Repository Layout

```
CMakeLists.txt          # Root build -- fetches all deps, adds db/ and relay/
db/                     # Database node (Layer 1)
  CMakeLists.txt        # chromatindb_lib target
  main.cpp              # Daemon entry point
  engine/               # Blob validation and ingestion (BlobEngine)
  storage/              # libmdbx storage layer (Storage, pimpl)
  crypto/               # PQ crypto (identity, handshake, thread_pool offload)
  net/                  # Networking (Connection, Server)
  peer/                 # Peer management (PeerManager, sync, PEX)
  wire/                 # FlatBuffers schemas + codec
  config/               # Config parsing + validation
  util/                 # Shared utilities (hex.h)
  identity/             # Node identity (ML-DSA-87 keypair)
  tests/                # All unit tests (Catch2)
    test_helpers.h      # Shared test utilities
relay/                  # Relay (Layer 2)
  relay_main.cpp        # Relay entry point
  config/               # Relay config
  core/                 # RelaySession, message filter
  identity/             # Relay identity adapter
loadgen/                # Load generator for benchmarks
tools/                  # Crypto verification tool
tests/                  # Docker integration tests (54 scenarios)
deploy/                 # Docker + compose files
sanitizers/             # ASAN/TSAN/UBSAN suppression files
.planning/              # GSD planning artifacts (roadmap, phases, requirements)
```

## Coding Style & Conventions

### C++ Style
- **C++20 throughout.** Use `std::span`, `std::optional`, `co_await`, `constexpr`, structured bindings.
- **Naming:** `snake_case` for functions, variables, files. `PascalCase` for classes/structs/enums. `UPPER_CASE` for constants/macros. Trailing underscore for member variables (`stopped_`, `client_conn_`).
- **Headers:** `#pragma once`. Project includes with quotes (`"db/engine/engine.h"`), library includes with angle brackets (`<asio.hpp>`). Project includes first, then library includes, each group sorted alphabetically.
- **Namespaces:** `chromatindb::` prefix. Nested by component: `chromatindb::engine`, `chromatindb::storage`, `chromatindb::net`, `chromatindb::relay::core`, `chromatindb::util`, `chromatindb::test`.
- **Comments:** `///` for Doxygen-style doc comments on public API. Inline comments only where logic isn't self-evident. No fluff.
- **Error handling:** Exceptions for configuration errors. Error codes/enums for runtime paths (see `IngestError`). No exceptions in hot paths.
- **Smart pointers:** `std::shared_ptr` for connection objects (shared ownership across coroutines). `std::unique_ptr` or raw refs for single-owner relationships.
- **Coroutines:** `asio::awaitable<T>` for all async operations. Every `co_await` is a potential container invalidation point -- capture by value, not reference.

### Build
- **Never use `cmake --build . --parallel`** -- it eats all memory on this machine. Always use `cmake --build .` (sequential).
- **Build from `build/` directory:** `cd build && cmake .. && cmake --build .`
- **Run tests:** `cd build && ctest --output-on-failure` or `./build/db/chromatindb_tests`
- **Sanitizers:** `cmake .. -DSANITIZER=asan` (or `tsan`, `ubsan`). Mutually exclusive.

### Code Quality Rules
- **Zero duplication.** All shared utilities go in `db/util/` (production) or `db/tests/test_helpers.h` (tests). If you see the same function in two files, extract it. User was burned by 7K lines of duplication in a past project.
- **No inefficient code or shortcuts. Ever.** Code must be correct and efficient. No lazy workarounds.
- **YAGNI.** Don't add features, configurability, or abstractions that aren't needed right now.
- **No `|| true` to suppress errors.** Commands that should fail must fail loudly.
- **Step 0 pattern:** Cheapest validation first (integer compare) before expensive operations (crypto).
- **Header-only for small utilities** to avoid ODR violations across modules (db/, relay/, tools/ all link chromatindb_lib).

### Key Technical Decisions (locked)
- **Hash-then-sign:** ML-DSA-87 signs/verifies `SHA3-256(namespace||data||ttl||timestamp)` -- the 32-byte digest, not the raw concatenation.
- **ChaCha20-Poly1305** over AES-256-GCM (software-fast, no hardware dep).
- **Sequential sync protocol** (Phase A/B/C) avoids TCP deadlock.
- **One-blob-at-a-time sync:** bounded memory, single blob in flight per connection.
- **Deque for coroutine-accessed containers** (not vector) -- prevents invalidation on push_back during coroutine suspension.
- **Timer-cancel pattern** for async message queues.
- **TTL is writer-controlled** (in signed blob data). No node-level TTL config.
- **No protocol-affecting config options.** Config is for node-local operational concerns only.
- **Implicit closed mode** from non-empty `allowed_keys` (no separate toggle).
- **SIGHUP** via dedicated coroutine member function (not lambda -- stack-use-after-return).
- **Coroutine params by value** (not const ref) -- C++ coroutine frames copy the reference, not the value.

## What NOT to Do

- Do not add OpenSSL or any new external dependencies without explicit approval.
- Do not implement DHT or gossip protocols.
- Do not add backward-compatibility shims -- this is pre-release.
- Do not add HTTP/REST APIs.
- Do not "clean up" or refactor code you weren't asked to touch.
- Do not add docstrings, comments, or type annotations to code you didn't change.
- Do not mock the database in tests -- use real storage instances.
- Do not create documentation files unless explicitly asked.
- Do not run `cmake --build . --parallel`.

## Project Status

v1.2.0 milestone complete (2026-03-23). 60 phases, 125 plans across 12 milestones.
All phases shipped. Database layer (v1.1.0), relay layer (v1.2.0), and codebase dedup audit done.

Planning artifacts live in `.planning/` -- roadmap, requirements, phase plans, verification reports. These are historical records. Read them for context but don't modify without reason.

## Known Issues

- Benchmark `get_blob_count()` sums `latest_seq_num` across namespaces -- overcounts in multi-namespace scenarios. Cosmetic only.
- Pre-existing full-suite hang in release build when running all 500+ tests together (port conflict in test infrastructure) -- deferred, not a code bug.
- DEDUP-02 (test helper consolidation) was in progress -- plan 60-02 may need completion. Check `.planning/phases/60-codebase-deduplication-audit/` for status.
