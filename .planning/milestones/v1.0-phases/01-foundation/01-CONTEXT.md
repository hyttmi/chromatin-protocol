# Phase 1: Foundation - Context

**Gathered:** 2026-03-03
**Status:** Ready for planning

<domain>
## Phase Boundary

Crypto primitives (ML-DSA-87, ML-KEM-1024, SHA3-256, ChaCha20-Poly1305), FlatBuffers wire format schemas, JSON config loading, spdlog structured logging, and node identity (keypair generation + namespace derivation). No networking, no storage engine, no blob ingest pipeline.

</domain>

<decisions>
## Implementation Decisions

### Symmetric crypto library
- Use **libsodium** for AEAD and KDF
- Use **ChaCha20-Poly1305** (not AES-256-GCM) for channel encryption — software-fast, constant-time, no hardware dependency
- **liboqs** handles all PQ primitives: ML-DSA-87 (signing), ML-KEM-1024 (key exchange), SHA3-256 (hashing)
- Thin C++ RAII wrappers around liboqs and libsodium C APIs — don't over-abstract, keep it simple

### KDF
- Claude's discretion on KDF choice — HKDF-SHA3-256 or HKDF-SHA256, pick based on what libsodium supports natively

### Canonical signing format
- Sign **canonical concatenation**: `SHA3-256(namespace || data || ttl || timestamp)` — independent of wire format
- Do NOT sign raw FlatBuffer bytes — this would tie signing to serialization format forever
- Blob hash (content-addressed dedup key) covers **full blob including signature**: `SHA3-256(entire blob)` — different signatures produce different hashes
- **No timestamp validation** on ingest — accept any timestamp, clock is writer's problem
- FlatBuffers role: **transport AND on-disk storage format** — single format everywhere, signing is independent

### Project structure & build
- **Flat src/ layout** with subdirs per module: `src/crypto/`, `src/storage/`, `src/net/`, etc.
- C++ namespace: **`chromatin::`** — `chromatin::crypto`, `chromatin::storage`, etc.
- Tests in **`tests/` top-level** directory mirroring src/ structure: `tests/crypto/`, `tests/storage/`, etc.
- CMake organization: Claude's discretion

### Node identity & config
- Keypair stored as **raw binary files**: `node.key` (private) + `node.pub` (public) in data directory
- **Auto-generate keypair on first start** if none exists, PLUS provide `chromatindb keygen` subcommand for manual generation
- **Separate config path from data dir**: `--config` flag for config file, `--data-dir` flag for storage/keys. Both have sensible defaults
- Default log level: **Info** — connections, sync events, errors. Overridable via config or CLI flag

### Claude's Discretion
- CMake module organization (single vs per-module)
- KDF algorithm choice (HKDF-SHA3-256 vs HKDF-SHA256)
- Exact RAII wrapper design for crypto APIs
- Config file default locations
- spdlog format pattern and sink configuration

</decisions>

<specifics>
## Specific Ideas

- BRIEF specifies exact blob wire format: namespace (32B), pubkey (2592B), data (variable), ttl (u32), timestamp (u64), signature (4627B)
- Signing input is the canonical concatenation, NOT the FlatBuffer encoding — this is the #1 architectural decision
- ChaCha20-Poly1305 replaces AES-256-GCM from the original BRIEF — update all downstream docs accordingly
- liboqs + libsodium = complete crypto stack, no OpenSSL needed anywhere

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- None — greenfield project, no existing code

### Established Patterns
- None — Phase 1 establishes all patterns

### Integration Points
- Crypto layer will be consumed by: Storage (Phase 2) for hashing, Blob Engine (Phase 3) for signing/verification, Networking (Phase 4) for key exchange + channel encryption
- Config and logging are cross-cutting — every subsequent phase depends on them
- FlatBuffers schemas define the wire format consumed by all network-facing phases

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope

</deferred>

---

*Phase: 01-foundation*
*Context gathered: 2026-03-03*
