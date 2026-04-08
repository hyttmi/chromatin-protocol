# Requirements: v2.2.0 Node Hardening

## Protocol Safety

- [ ] **PROTO-01**: All integer arithmetic in protocol parsing uses overflow-checked helpers (decode_namespace_list, decode_blob_request, encode_namespace_list, decode_blob_transfer, encode_blob_transfer)
- [x] **PROTO-02**: Auth payload pubkey size validated against exact expected constant (ML-DSA-87 public key size) in all handshake paths
- [x] **PROTO-03**: FlatBuffer decode validates pubkey and data field sizes before passing to signature verification
- [x] **PROTO-04**: AEAD associated data length bounded to prevent unbounded allocation

## Crypto Safety

- [x] **CRYPTO-01**: AEAD nonce counters kill the connection before reaching 2^64 to prevent nonce reuse
- [x] **CRYPTO-02**: PQ handshake initiator verifies responder pubkey binding
- [ ] **CRYPTO-03**: Lightweight handshake authenticates peer identity (not just transport trust)

## TTL Enforcement

- [ ] **TTL-01**: BlobFetch handler checks expiry before serving blobs
- [ ] **TTL-02**: All query paths (Read, List, Stats, Exists, BatchRead, TimeRange) filter expired blobs from results
- [ ] **TTL-03**: Expiry timestamp calculation uses saturating arithmetic (no uint64 overflow on timestamp + ttl)

## Sync Correctness

- [ ] **SYNC-01**: pending_fetches_ entries cleaned on rejected ingest (not just successful)
- [ ] **SYNC-02**: pending_fetches_ key includes namespace to prevent cross-namespace hash collision
- [ ] **SYNC-03**: Phase B reconciliation takes consistent hash snapshot (no stale reads from concurrent writes)

## Resource Limits

- [ ] **RES-01**: Per-peer subscription count limit enforced at node level (with rejection message)
- [ ] **RES-02**: Bootstrap peer detection considers port, not just host
- [ ] **RES-03**: TOCTOU race on capacity and quota checks eliminated (atomic check-and-reserve)
- [ ] **RES-04**: Quota rebuild clear loop fixed (iterator invalidation on skip-every-other bug)

## Coroutine Safety

- [ ] **CORO-01**: Send/recv counters use proper atomic or single-thread guarantees across co_await boundaries

## Code Deduplication

- [x] **DEDUP-01**: Centralized encoding utility header (write/read_u64_be, write/read_u32_be) replaces all 40+ inline BE encoding loops
- [x] **DEDUP-02**: connection.cpp uses existing encode_auth_payload/decode_auth_payload from handshake.cpp (removes 4 inline copies)
- [x] **DEDUP-03**: Signature verification with thread pool offload extracted to shared method (removes 4 copies)
- [x] **DEDUP-04**: Namespace/hash extraction helper replaces 10+ inline memcpy patterns
- [x] **DEDUP-05**: Blob reference encoding helper replaces 6+ inline patterns

## Architecture

- [x] **ARCH-01**: PeerManager split into focused components (connection management, message dispatch, sync orchestration, metrics)

## Future Requirements

- Re-encryption of old data on key rotation (deferred — only if compliance use case demands it)
- Permanent ban list / revocation list (deferred — tombstone-based revocation sufficient for now)

## Out of Scope

- SDK changes — this milestone is C++ node only
- New wire protocol types — fix existing, don't add new
- Performance optimization beyond correctness fixes (O(n) seq scan acceptable at current scale)

## Traceability

| Requirement | Phase | Status |
|-------------|-------|--------|
| DEDUP-01 | Phase 95 | Complete |
| DEDUP-02 | Phase 95 | Complete |
| DEDUP-03 | Phase 95 | Complete |
| DEDUP-04 | Phase 95 | Complete |
| DEDUP-05 | Phase 95 | Complete |
| ARCH-01 | Phase 96 | Complete |
| PROTO-01 | Phase 97 | Pending |
| PROTO-02 | Phase 97 | Complete |
| PROTO-03 | Phase 97 | Complete |
| PROTO-04 | Phase 97 | Complete |
| CRYPTO-01 | Phase 97 | Complete |
| CRYPTO-02 | Phase 97 | Complete |
| CRYPTO-03 | Phase 97 | Pending |
| TTL-01 | Phase 98 | Pending |
| TTL-02 | Phase 98 | Pending |
| TTL-03 | Phase 98 | Pending |
| SYNC-01 | Phase 99 | Pending |
| SYNC-02 | Phase 99 | Pending |
| SYNC-03 | Phase 99 | Pending |
| RES-01 | Phase 99 | Pending |
| RES-02 | Phase 99 | Pending |
| RES-03 | Phase 99 | Pending |
| RES-04 | Phase 99 | Pending |
| CORO-01 | Phase 99 | Pending |

**Coverage:**
- v2.2.0 requirements: 24 total
- Mapped to phases: 24/24 (100%)
- Unmapped: 0
