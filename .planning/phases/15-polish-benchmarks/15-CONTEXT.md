# Phase 15: Polish & Benchmarks - Context

**Gathered:** 2026-03-08
**Status:** Ready for planning

<domain>
## Phase Boundary

Documentation and performance validation for the complete v3.0 feature set. README.md documents how to build, configure, and run chromatindb nodes. Performance test suite produces concrete numbers for key operations.

</domain>

<decisions>
## Implementation Decisions

### README structure & audience
- Primary audience: node operators (people who want to run chromatindb nodes)
- Depth: moderate — brief explanation of key concepts (namespaces, blobs, sync, delegation, pub/sub, tombstones) alongside practical usage
- Config documentation: just an example JSON config (no reference table — people can figure out the keys from context)
- Crypto stack: mention ML-DSA-87, ML-KEM-1024, ChaCha20-Poly1305 as a selling point — explain what each protects (signing, key exchange, transport)
- Brief architecture section: high-level paragraph explaining namespaces, blobs, sync, PQ transport (no wire format details)

### README scenarios
- Three config-based scenarios: single node, two-node sync, closed mode with ACLs
- v3.0 features (delegation, pub/sub, deletion): just mention they exist with brief descriptions, no detailed scenarios
- No CLI client exists — interaction examples are config + daemon behavior, not programmatic API

### Benchmark binary
- Separate benchmark binary (`chromatindb_bench`), not integrated into Catch2 test suite
- Keeps test suite fast, benchmarks run independently

### Benchmark scope
- Core data path: blob ingest (sign + store), blob retrieval, sync throughput (blobs/sec between two nodes)
- Crypto operations: ML-DSA-87 sign/verify, ML-KEM-1024 encaps/decaps, ChaCha20-Poly1305 encrypt/decrypt, SHA3-256 hash
- Network operations: PQ handshake time (KEM + AEAD setup), notification dispatch latency (pub/sub)
- Blob sizes: Claude's discretion — pick sizes that show interesting performance characteristics

### Benchmark reporting
- Results go in a "Performance" section in README.md (not a separate file)
- Hardware context: just a disclaimer ("Results measured on [machine]. Your numbers will vary.")
- Benchmark binary outputs markdown-formatted table — run benchmark, copy output into README
- README includes clear instructions: how to build the benchmark binary, run it, and interpret output

### Claude's Discretion
- Exact blob sizes for benchmarks
- README section ordering and formatting
- Benchmark iteration counts and warm-up
- Architecture section wording and detail level

</decisions>

<specifics>
## Specific Ideas

- PQ crypto is a selling point — surface it prominently, not buried
- Benchmark output should be copy-paste ready for README (markdown table format)
- Three demo scenarios cover the core operator journey: solo node → multi-node sync → access control

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `db/main.cpp`: Daemon entry point with `run`, `keygen`, `version` commands — README documents these directly
- `db/config/config.cpp`: JSON config parsing with all supported keys (bind_address, data_dir, bootstrap_peers, allowed_keys, max_peers, sync_interval_seconds, log_level)
- `db/crypto/`: Complete crypto module (hash, signing, kem, aead, kdf) — benchmark targets
- `db/engine/engine.cpp`: BlobEngine with ingest/retrieve — benchmark targets
- `db/sync/sync_protocol.cpp`: Sync protocol — benchmark target for throughput
- `db/net/handshake.cpp`: PQ handshake — benchmark target for latency
- `tests/`: 196 Catch2 tests across all modules — patterns for setting up test fixtures

### Established Patterns
- Catch2 v3 for testing (BENCHMARK macro available but we're using a separate binary)
- FetchContent for all dependencies (CMake pattern)
- Static library (`chromatindb_lib`) linked by both daemon and test binary — benchmark binary will follow same pattern

### Integration Points
- New `chromatindb_bench` target in CMakeLists.txt, linking `chromatindb_lib` and Catch2
- README.md at project root (doesn't exist yet)
- Benchmark results section in README.md

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 15-polish-benchmarks*
*Context gathered: 2026-03-08*
