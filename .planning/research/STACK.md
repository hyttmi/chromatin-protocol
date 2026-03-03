# Stack Research

**Domain:** Decentralized replicated key-value database with post-quantum cryptography
**Researched:** 2026-03-03
**Confidence:** HIGH (crypto stack proven in chromatin/PQCC, storage/serialization well-established)

## Recommended Stack

### Core Technologies

| Technology | Version | Purpose | Why Recommended |
|------------|---------|---------|-----------------|
| C++20 | GCC 13+ / Clang 16+ | Language standard | Concepts, ranges, std::span, constexpr. C++20 is the sweet spot — C++23 adoption too thin |
| CMake | 3.24+ | Build system | FetchContent improvements, presets. Already proven in chromatin/PQCC |
| liboqs | 0.15.0 | PQ crypto (ML-DSA-87, ML-KEM-1024, SHA3-256) | Only production-grade PQ library. Already used in PQCC. FetchContent with explicit include path fix for generated headers |
| OpenSSL | 3.x | AES-256-GCM symmetric encryption | Hardware-accelerated AES-NI. Only for symmetric crypto — PQ algorithms from liboqs |
| libmdbx | 0.13.11 | Embedded key-value storage engine | Zero-copy reads via mmap, ACID, single-writer/multi-reader MVCC. Native CMake build, automatic page reclamation (critical for append-only logs with TTL-based pruning), better corruption recovery than LMDB. Used by Erigon at scale |
| FlatBuffers | 25.12.19 | Binary serialization | Zero-copy access (pairs with libmdbx zero-copy reads), deterministic byte representation for signing, schema evolution, handles large byte vectors (4627-byte ML-DSA-87 signatures) natively |

### Supporting Libraries

| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| xxHash (XXH3) | 0.8.3 | Fast non-crypto hashing for sync fingerprints | Range-based set reconciliation — bucket fingerprints need speed, not security. SHA3 remains the security hash |
| Catch2 | 3.13.0 | Testing framework | Unit and integration tests. Already familiar from PQCC |
| spdlog | 1.17.0 | Structured logging | Debug and operational logging. Already used in chromatin daemon |
| nlohmann/json | 3.12.0 | JSON parsing | Config files, debug output, interop. NOT for wire format (use FlatBuffers) |

### Development Tools

| Tool | Purpose | Notes |
|------|---------|-------|
| CMake FetchContent | Dependency management | All deps pulled at configure time. Pin exact versions via GIT_TAG |
| clang-format | Code formatting | Enforce consistent style |
| Valgrind / ASan | Memory safety | Critical for crypto/database library — memory bugs are security bugs |
| ctest | Test runner | Integrated with CMake, runs Catch2 tests |

## Installation

```cmake
include(FetchContent)

FetchContent_Declare(liboqs
  GIT_REPOSITORY https://github.com/open-quantum-safe/liboqs.git
  GIT_TAG 0.15.0)

FetchContent_Declare(libmdbx
  GIT_REPOSITORY https://github.com/erthink/libmdbx.git
  GIT_TAG v0.13.11)

FetchContent_Declare(flatbuffers
  GIT_REPOSITORY https://github.com/google/flatbuffers.git
  GIT_TAG v25.12.19)

FetchContent_Declare(xxhash
  GIT_REPOSITORY https://github.com/Cyan4973/xxHash.git
  GIT_TAG v0.8.3)

FetchContent_Declare(catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG v3.13.0)

FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.17.0)

FetchContent_Declare(nlohmann_json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0)

# Note: liboqs needs explicit include path fix for FetchContent (generated headers in build dir)
# Note: libmdbx has native CMake support — FetchContent just works
# Note: LMDB wrapper for libmdbx not needed — it has its own C++ API
```

## Alternatives Considered

| Recommended | Alternative | When to Use Alternative |
|-------------|-------------|-------------------------|
| libmdbx | LMDB | Maximum battle-testedness, don't care about CMake integration or page reclamation. LMDB is maintenance-mode but rock-solid |
| libmdbx | RocksDB | Write-heavy workloads with LSM compaction benefits. CPUNK-DB is append-only + read-heavy sync, so mmap zero-copy wins |
| libmdbx | SQLite | Relational queries or complex indexing. KV operations don't need SQL overhead |
| FlatBuffers | CBOR (RFC 8949 §4.2) | Cross-language interop matters more than performance. CBOR has deterministic encoding mode, wider language support. Fallback if FlatBuffers canonicality causes signing issues |
| FlatBuffers | Cap'n Proto | Similar zero-copy design but less mature tooling, no deterministic encoding guarantee |
| XXH3 | BLAKE3 | Want a hash that's both fast and cryptographic. Overkill for sync fingerprints |
| Custom HLC | External lib | HLC is ~100 lines of C++ from Kulkarni et al. (2014). Too small for a dependency |

## What NOT to Use

| Avoid | Why | Use Instead |
|-------|-----|-------------|
| Protocol Buffers | Non-deterministic serialization breaks content-addressed hashing. Two serializations of same message produce different bytes → different hashes → broken verification | FlatBuffers or CBOR deterministic mode |
| RocksDB | LSM tree with background compaction — wrong profile for append-only logs with read-heavy sync. Compaction causes write amplification and unpredictable latency on mobile | libmdbx (B+ tree, zero-copy mmap, no background threads) |
| Boost | C++20 covers everything CPUNK-DB needs. Boost adds massive compile times and dependency surface | C++20 standard library |
| gRPC | CPUNK-DB is transport-agnostic. gRPC forces HTTP/2, adds protobuf dependency | FlatBuffers messages over caller-provided transport |
| libp2p | Designed for P2P DHT networking. CPUNK-DB uses relay-based replication — libp2p's NAT traversal and DHT routing are irrelevant overhead | Custom sync protocol over caller-provided connections |
| JSON on wire | Too large for binary data (base64 overhead), no schema, parsing overhead. ML-DSA-87 signatures are 4627 bytes — JSON encoding would be ~6.2KB | FlatBuffers binary encoding |
| MessagePack | No zero-copy, no schema, no deterministic encoding guarantee. Doesn't pair with libmdbx zero-copy reads | FlatBuffers |

## Stack Patterns by Variant

**If targeting embedded/IoT (future):**
- Drop spdlog, use printf-based logging
- Consider CBOR instead of FlatBuffers (smaller generated code)
- libmdbx still works on embedded Linux

**If adding a daemon wrapper later:**
- Add IXWebSocket (already proven in chromatin daemon) for WebSocket transport
- Add CLI framework (argv dispatch, already proven in chromatin CLI)
- spdlog becomes mandatory for operational logging

**If cross-language bindings needed:**
- CBOR wire format is easier to implement in other languages than FlatBuffers
- Consider C API surface (not C++) for FFI compatibility
- FlatBuffers has codegen for most languages, but custom CBOR is simpler

## Version Compatibility

| Package | Compatible With | Notes |
|---------|-----------------|-------|
| liboqs 0.15.0 | OpenSSL 3.x | liboqs can use OpenSSL as crypto backend. Keep both updated together |
| libmdbx 0.13.11 | Any C++20 compiler | C library with C++ header wrapper. Native CMake. No compatibility concerns |
| FlatBuffers 25.12.19 | flatc codegen | Generated code is forward-compatible. Pin flatc version in CI |
| xxHash 0.8.3 | XXH3 stable API | XXH3 marked stable in 0.8.0. API frozen |

## Sources

- liboqs documentation and PQCC project experience — HIGH confidence
- libmdbx documentation (erthink/libmdbx) — HIGH confidence
- Erigon project (libmdbx at scale) — HIGH confidence
- FlatBuffers documentation (Google) — HIGH confidence
- Negentropy protocol specification (Nostr NIPs) — MEDIUM confidence
- Kulkarni et al. "Logical Physical Clocks" (2014) — HIGH confidence

---
*Stack research for: decentralized replicated KV database with PQ crypto*
*Researched: 2026-03-03*
