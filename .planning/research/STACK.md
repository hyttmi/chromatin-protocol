# Stack Research

**Domain:** Decentralized PQ-secure database node (standalone daemon)
**Researched:** 2026-03-03
**Confidence:** HIGH (versions verified via git tags, crypto stack proven in PQCC, storage/serialization well-established)

## Recommended Stack

### Core Technologies

| Technology | Version | Purpose | Why Recommended |
|------------|---------|---------|-----------------|
| C++20 | GCC 13+ / Clang 16+ | Language standard | Concepts, coroutines, ranges, std::span, constexpr improvements. C++20 is the sweet spot -- C++23 support still spotty on older compilers. User's system has GCC 15.2 and Clang 21.1, both fully C++20-capable |
| CMake | 3.24+ | Build system | FetchContent improvements (FIND_PACKAGE_ARGS, OVERRIDE_FIND_PACKAGE), presets support. User has CMake 4.2.3. Already proven in previous projects |
| liboqs | 0.15.0 | PQ crypto: ML-DSA-87, ML-KEM-1024, SHA3-256 | The only production-grade, NIST-compliant PQ crypto library. Implements all FIPS 203/204/205 algorithms. Already proven in PQCC project. No credible alternative exists |
| OpenSSL | 3.x | AES-256-GCM symmetric encryption only | Hardware-accelerated AES-NI on x86. Only used for symmetric crypto -- PQ algorithms come from liboqs. System has 3.6.1 |
| libmdbx | 0.13.11 | Embedded key-value storage engine | Zero-copy reads via mmap, ACID transactions, single-writer/multi-reader MVCC. Automatic page reclamation (critical for TTL-based blob pruning). Better crash recovery than LMDB. Native CMake support. Used by Erigon (Ethereum client) at scale |
| FlatBuffers | v25.12.19 | Binary wire format and storage serialization | Zero-copy deserialization (pairs perfectly with libmdbx mmap reads), deterministic byte representation required for signing verification, native large byte vector support (4627-byte ML-DSA-87 signatures). Schema evolution for future compatibility |
| Standalone Asio | 1.36.0 | Async TCP networking and event loop | Header-only, no Boost dependency. C++20 coroutine support via co_await. Proven TCP server/client with io_context event loop. Handles timers, signals, and socket I/O in a single event loop. The standard choice for C++ network daemons |

### Supporting Libraries

| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| xxHash (XXH3) | 0.8.3 | Fast non-crypto hashing | Sync fingerprints for hash-list diff reconciliation. Speed matters here, not cryptographic strength. SHA3 remains the security hash |
| Catch2 | v3.13.0 | Testing framework | Unit and integration tests. Mature, well-documented, supports BDD-style and data-driven tests |
| spdlog | v1.17.0 | Structured logging | Operational and debug logging for a daemon. Fast, header-only option available, fmt-based formatting. Already used in previous projects |
| nlohmann/json | v3.12.0 | JSON parsing | Config file parsing, debug/admin output. NOT for wire format (FlatBuffers handles that) |

### Development Tools

| Tool | Purpose | Notes |
|------|---------|-------|
| CMake FetchContent | Dependency management | All deps pulled at configure time. Pin exact versions via GIT_TAG. No system package dependencies |
| clang-format / clang-tidy | Code quality | Enforce consistent style. clang-tidy catches C++20 anti-patterns, unused includes, modernization opportunities |
| ASan + UBSan | Memory and undefined behavior safety | Compile with -fsanitize=address,undefined in debug builds. Memory bugs in crypto/database code are security vulnerabilities |
| Valgrind | Heap analysis | Complement to ASan for leak detection. Cannot run simultaneously with ASan |
| ctest | Test runner | CMake-integrated, runs Catch2 tests. Use `ctest --output-on-failure` |

## Networking Deep Dive

The biggest gap in previous research. chromatindb is now a standalone daemon, not a library. It needs:
- TCP server: accept incoming peer connections
- TCP client: initiate outbound connections to peers
- Custom PQ handshake: ML-KEM-1024 key exchange, then AES-256-GCM encrypted channel
- Concurrent connections: multiple peers simultaneously
- Timers: TTL pruning, sync scheduling, connection keepalive
- Signal handling: clean shutdown on SIGTERM/SIGINT

### Why Standalone Asio

**Use standalone Asio (not Boost.Asio).** The standalone version is header-only, no Boost dependency, identical API. Version 1.36.0 is current (verified via git tags).

Rationale:
1. **C++20 coroutines**: `asio::co_spawn` + `co_await` makes async TCP code readable without callback hell. The PQ handshake sequence (send KEM ciphertext, receive shared secret, derive session keys, switch to encrypted channel) maps cleanly to sequential coroutine code
2. **Single event loop**: `asio::io_context` handles sockets, timers, and signals in one thread or a thread pool. No separate event loop library needed
3. **Raw TCP**: Asio gives raw `tcp::socket` and `tcp::acceptor` -- no HTTP/WebSocket overhead. chromatindb needs a custom binary protocol (FlatBuffers over TCP), not HTTP
4. **Mature and proven**: Used by virtually every non-trivial C++ network application. Beast (HTTP) and gRPC both build on Asio
5. **Timer support**: `asio::steady_timer` for scheduling sync rounds, TTL pruning, and keepalive pings
6. **Strand-based concurrency**: `asio::strand` serializes handlers per-connection without manual locking

### Why NOT the alternatives

| Alternative | Why Not For This Project |
|-------------|--------------------------|
| libuv | C library, not C++. Would need manual C++ wrapper. No coroutine support. More boilerplate for the same functionality |
| liburing / io_uring directly | Linux-only. Maximum performance but chromatindb doesn't need millions of connections -- tens to hundreds of peers. Asio can use io_uring as a backend on Linux anyway |
| Raw epoll + POSIX sockets | More code, more bugs, reinventing what Asio already provides. No timer/signal integration |
| Boost.Beast | HTTP/WebSocket library built on Boost.Asio. chromatindb doesn't need HTTP at Layer 1 |
| gRPC | Forces HTTP/2 + Protobuf. Wrong fit for custom PQ-encrypted binary protocol |
| libp2p | DHT-centric P2P framework. Project explicitly avoids DHT. Too much irrelevant complexity |
| ZeroMQ | Message queue patterns (pub/sub, req/rep). Not a raw TCP library. Wrong abstraction level |

### Asio Integration Pattern

```cmake
FetchContent_Declare(asio
  GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
  GIT_TAG asio-1-36-0)
FetchContent_MakeAvailable(asio)

# Asio is header-only, create interface target
add_library(asio INTERFACE)
target_include_directories(asio INTERFACE ${asio_SOURCE_DIR}/asio/include)
target_compile_definitions(asio INTERFACE ASIO_STANDALONE ASIO_NO_DEPRECATED)
# Enable C++20 coroutine support
target_compile_definitions(asio INTERFACE ASIO_HAS_CO_AWAIT)
```

### Coroutine-Based Connection Pattern

```cpp
// Example: PQ handshake as a coroutine
asio::awaitable<void> handle_peer(tcp::socket socket) {
    // 1. Read ML-KEM-1024 ciphertext from peer
    auto kem_ct = co_await async_read_message(socket);

    // 2. Decapsulate to get shared secret
    auto shared_secret = ml_kem_decaps(kem_ct, our_sk);

    // 3. Derive AES-256-GCM session keys from shared secret
    auto session = derive_session_keys(shared_secret);

    // 4. Now all communication is encrypted
    while (true) {
        auto msg = co_await async_read_encrypted(socket, session);
        co_await process_message(msg, socket, session);
    }
}

// Accept loop
asio::awaitable<void> listener(tcp::acceptor& acceptor) {
    while (true) {
        auto socket = co_await acceptor.async_accept(asio::use_awaitable);
        asio::co_spawn(acceptor.get_executor(),
                       handle_peer(std::move(socket)),
                       asio::detached);
    }
}
```

## Installation (CMake FetchContent)

```cmake
cmake_minimum_required(VERSION 3.24)
project(chromatindb CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# ---- Core Dependencies ----

# PQ Crypto
FetchContent_Declare(liboqs
  GIT_REPOSITORY https://github.com/open-quantum-safe/liboqs.git
  GIT_TAG 0.15.0)

# Embedded Database
FetchContent_Declare(libmdbx
  GIT_REPOSITORY https://github.com/erthink/libmdbx.git
  GIT_TAG v0.13.11)

# Wire Format
FetchContent_Declare(flatbuffers
  GIT_REPOSITORY https://github.com/google/flatbuffers.git
  GIT_TAG v25.12.19)

# Async Networking
FetchContent_Declare(asio
  GIT_REPOSITORY https://github.com/chriskohlhoff/asio.git
  GIT_TAG asio-1-36-0)

# ---- Supporting Dependencies ----

# Fast Hashing (sync fingerprints)
FetchContent_Declare(xxhash
  GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
  GIT_TAG v0.8.3
  SOURCE_SUBDIR cmake_unofficial)

# Testing
FetchContent_Declare(catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.13.0)

# Logging
FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.17.0)

# Config Parsing
FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0)

# ---- Notes ----
# liboqs: Needs explicit include path fix for generated headers in build dir
# libmdbx: Native CMake support -- FetchContent just works
# Asio: Header-only, needs INTERFACE target with ASIO_STANDALONE define
# xxHash: Use cmake_unofficial subdirectory for proper CMake target
# FlatBuffers: Pin flatc codegen version in CI to match library version
```

## Alternatives Considered

| Recommended | Alternative | When to Use Alternative |
|-------------|-------------|-------------------------|
| Standalone Asio | libuv | If you need a C-compatible event loop or are building a polyglot system. libuv is battle-tested (Node.js) but C, not C++ |
| Standalone Asio | Raw epoll + io_uring | If you need maximum Linux-specific performance at millions of connections. Overkill for chromatindb's peer count |
| libmdbx | LMDB | If you want maximum battle-testedness and don't care about page reclamation or CMake integration. LMDB is maintenance-mode but rock-solid |
| libmdbx | RocksDB | If you have write-heavy LSM workloads. chromatindb is append-only with read-heavy sync -- mmap zero-copy wins over LSM compaction |
| libmdbx | SQLite | If you need relational queries or complex indexing. KV operations don't need SQL overhead |
| FlatBuffers | CBOR (RFC 8949) | If cross-language interop matters more than performance. CBOR has deterministic encoding mode (Section 4.2), wider language support. Fallback if FlatBuffers canonicality causes signing issues |
| FlatBuffers | Cap'n Proto | Similar zero-copy design but smaller ecosystem, less tooling, no deterministic encoding guarantee |
| liboqs | Vendored reference implementations | If you need only one or two algorithms and want to minimize dependency surface. But liboqs is maintained by a dedicated team and tracks NIST standards updates |
| XXH3 | BLAKE3 | If you want a hash that's both fast AND cryptographic. Overkill for sync fingerprints where speed is the only concern |

## What NOT to Use

| Avoid | Why | Use Instead |
|-------|-----|-------------|
| Protocol Buffers | Non-deterministic serialization. Two serializations of the same message produce different bytes, breaking content-addressed hashing and signature verification | FlatBuffers (deterministic) or CBOR deterministic mode |
| Boost | C++20 covers everything chromatindb needs. Boost adds massive compile times and dependency surface for no benefit | C++20 standard library + standalone Asio |
| gRPC | Forces HTTP/2 + Protobuf. Wrong transport for a custom PQ-encrypted binary protocol between peers | FlatBuffers messages over Asio TCP |
| libp2p | DHT-centric P2P framework. chromatindb explicitly avoids DHT. libp2p's NAT traversal and routing are irrelevant overhead | Standalone Asio + custom peer management |
| JSON on wire | Too large for binary data (base64 overhead). ML-DSA-87 signatures are 4627 bytes -- JSON would bloat to ~6.2KB. No schema, slow parsing | FlatBuffers binary encoding |
| MessagePack | No zero-copy, no schema, no deterministic encoding. Doesn't pair with libmdbx zero-copy reads | FlatBuffers |
| RocksDB | LSM tree with background compaction -- wrong profile for append-only blobs with read-heavy sync. Compaction causes write amplification and unpredictable latency | libmdbx (B+ tree, zero-copy mmap, no background threads) |
| Boost.Beast / WebSocket | HTTP/WebSocket belongs in Layer 2 (Relay), not Layer 1 (database node). The node-to-node protocol is raw TCP with PQ encryption | Standalone Asio raw TCP |

## Stack Patterns by Variant

**For the daemon (primary target):**
- Asio io_context as the main event loop
- C++20 coroutines for async connection handling
- spdlog for operational logging (log levels, structured output)
- nlohmann/json for config file parsing
- Signal handling via Asio (SIGTERM, SIGINT for clean shutdown)

**If adding admin/debug interface later:**
- Add a Unix domain socket or secondary TCP port for admin commands
- Still use Asio -- just another acceptor on the same io_context
- JSON for admin protocol (human-readable, low volume)

**If cross-platform (macOS/Windows) needed later:**
- Asio abstracts platform differences (epoll on Linux, kqueue on macOS, IOCP on Windows)
- liboqs and libmdbx both support Linux/macOS/Windows
- No Linux-specific APIs used in the recommended stack

## Version Compatibility

| Package | Compatible With | Notes |
|---------|-----------------|-------|
| liboqs 0.15.0 | OpenSSL 3.x | liboqs can use OpenSSL as crypto backend for classical primitives. Keep both updated together |
| liboqs 0.15.0 | C++20 | C library with C headers. Wrap in C++ RAII classes for safety |
| libmdbx 0.13.11 | Any C++20 compiler | C library with optional C++ header wrapper. Native CMake. No compatibility concerns |
| FlatBuffers v25.12.19 | flatc codegen | Generated C++ code is forward-compatible. Pin flatc version to match library version in CI |
| Standalone Asio 1.36.0 | C++20 coroutines | Requires -std=c++20 and -fcoroutines (GCC). Clang 16+ and GCC 13+ both support this |
| Standalone Asio 1.36.0 | OpenSSL 3.x | Asio has optional SSL support but chromatindb uses custom PQ crypto, not TLS. Define ASIO_STANDALONE only |
| xxHash 0.8.3 | XXH3 stable API | XXH3 API marked stable since 0.8.0. Frozen, no breaking changes expected |
| spdlog v1.17.0 | fmt library | spdlog bundles fmt internally. No separate fmt dependency needed |

## Version Verification Method

All versions verified on 2026-03-03 via `git ls-remote --tags` against official repositories:

| Library | Repo | Latest Stable | Pinned |
|---------|------|---------------|--------|
| liboqs | open-quantum-safe/liboqs | 0.15.0 | 0.15.0 |
| libmdbx | erthink/libmdbx | v0.13.11 | v0.13.11 |
| FlatBuffers | google/flatbuffers | v25.12.19 | v25.12.19 |
| Standalone Asio | chriskohlhoff/asio | asio-1-36-0 | asio-1-36-0 |
| xxHash | Cyan4973/xxHash | v0.8.3 | v0.8.3 |
| Catch2 | catchorg/Catch2 | v3.13.0 | v3.13.0 |
| spdlog | gabime/spdlog | v1.17.0 | v1.17.0 |
| nlohmann/json | nlohmann/json | v3.12.0 | v3.12.0 |

System toolchain (Arch Linux):
- GCC 15.2.1 (2026-02-09)
- Clang 21.1.8
- CMake 4.2.3
- OpenSSL 3.6.1

## Sources

- liboqs releases (git ls-remote): 0.15.0 confirmed as latest stable -- **HIGH confidence**
- libmdbx releases (git ls-remote): v0.13.11 confirmed as latest stable -- **HIGH confidence**
- FlatBuffers releases (git ls-remote): v25.12.19 confirmed as latest -- **HIGH confidence**
- Standalone Asio releases (git ls-remote): asio-1-36-0 confirmed as latest -- **HIGH confidence**
- xxHash releases (git ls-remote): v0.8.3 confirmed as latest stable -- **HIGH confidence**
- Catch2 releases (git ls-remote): v3.13.0 confirmed as latest -- **HIGH confidence**
- spdlog releases (git ls-remote): v1.17.0 confirmed as latest -- **HIGH confidence**
- nlohmann/json releases (git ls-remote): v3.12.0 confirmed as latest -- **HIGH confidence**
- DNA Messenger CMakeLists.txt: reference for PQ crypto integration patterns -- **HIGH confidence**
- PQCC project experience: liboqs + OpenSSL integration proven -- **HIGH confidence**
- Asio C++20 coroutine support: based on training data knowledge of Asio's co_await integration. Verified Asio 1.36.0 exists but could not web-verify specific coroutine API details -- **MEDIUM confidence** (flag: verify coroutine API before implementation)
- System package versions (pacman -Q): OpenSSL 3.6.1, Asio 1.36.0, libuv 1.52.0, xxHash 0.8.3 all confirmed installed -- **HIGH confidence**

---
*Stack research for: chromatindb -- decentralized PQ-secure database node*
*Researched: 2026-03-03*
