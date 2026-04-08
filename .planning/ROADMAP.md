# Roadmap: v2.2.0 Node Hardening

## Overview

Fix all known correctness bugs, integer overflow vulnerabilities, crypto safety gaps, duplicate code, and architectural debt in the C++ database layer. Start by centralizing duplicate code into shared utilities, then split the PeerManager god object, then harden protocol parsing and crypto, enforce TTL in all paths, and finally fix sync/resource/concurrency correctness. Every phase must pass ASAN/TSAN/UBSAN with all 615+ existing tests.

## Phases

- [x] **Phase 95: Code Deduplication** - Centralize 64+ duplicate encoding/decoding patterns into shared utility headers (completed 2026-04-07)
- [x] **Phase 96: PeerManager Architecture** - Split PeerManager god object into focused components (completed 2026-04-08)
- [x] **Phase 97: Protocol & Crypto Safety** - Harden all protocol parsing and cryptographic handshake paths (completed 2026-04-08)
- [ ] **Phase 98: TTL Enforcement** - Enforce expiry checks in every query and fetch path
- [ ] **Phase 99: Sync, Resource & Concurrency Correctness** - Fix sync leaks, resource limit races, and coroutine safety

## Phase Details

### Phase 95: Code Deduplication
**Goal**: All duplicate encoding/decoding patterns are replaced by shared, tested utility functions
**Depends on**: Nothing (first phase)
**Requirements**: DEDUP-01, DEDUP-02, DEDUP-03, DEDUP-04, DEDUP-05
**Success Criteria** (what must be TRUE):
  1. A single header provides read/write helpers for u16_be, u32_be, u64_be and all protocol code uses them (zero inline BE encoding loops remain)
  2. connection.cpp auth payload encode/decode calls shared functions from handshake.cpp (zero inline copies remain)
  3. Signature verification with thread pool offload is a single shared method called from all 4+ verification sites
  4. Namespace+hash extraction from message payloads uses a single helper (zero inline 32-byte memcpy patterns remain)
  5. All 615+ existing unit tests pass under ASAN/TSAN/UBSAN with no regressions
**Plans:** 3/3 plans complete

Plans:
- [x] 95-01-PLAN.md -- Create endian.h + blob_helpers.h utilities with tests; replace consumers in sync, storage, framing
- [x] 95-02-PLAN.md -- Replace all inline patterns in peer_manager.cpp and codec.cpp
- [x] 95-03-PLAN.md -- Create auth_helpers.h + verify_helpers.h; replace auth/verify patterns in connection.cpp; sanitizer gate

### Phase 96: PeerManager Architecture
**Goal**: PeerManager is decomposed into focused components with clear responsibilities
**Depends on**: Phase 95
**Requirements**: ARCH-01
**Success Criteria** (what must be TRUE):
  1. Connection lifecycle management, message dispatch, sync orchestration, and metrics collection are in separate classes or modules (not one 3000+ line file)
  2. Each component has a testable interface that can be unit-tested independently
  3. PeerManager public API remains unchanged (no caller-side changes needed)
  4. All 615+ existing unit tests and Docker integration tests pass under ASAN/TSAN/UBSAN
**Plans:** 3/3 plans complete

Plans:
- [x] 96-01-PLAN.md -- Extract peer_types.h, MetricsCollector, and PexManager from PeerManager
- [x] 96-02-PLAN.md -- Extract ConnectionManager (owns peers_) and BlobPushManager from PeerManager
- [x] 96-03-PLAN.md -- Extract SyncOrchestrator and MessageDispatcher; final facade cleanup; sanitizer gate

### Phase 97: Protocol & Crypto Safety
**Goal**: All protocol parsing paths reject malformed input before processing, and all cryptographic handshake paths enforce identity binding
**Depends on**: Phase 95
**Requirements**: PROTO-01, PROTO-02, PROTO-03, PROTO-04, CRYPTO-01, CRYPTO-02, CRYPTO-03
**Success Criteria** (what must be TRUE):
  1. Integer arithmetic in all protocol decode/encode functions uses overflow-checked helpers that reject on overflow instead of wrapping
  2. Auth payload validation rejects any pubkey whose size does not exactly match the ML-DSA-87 constant
  3. AEAD nonce counter kills the connection cleanly before reaching the 2^64 limit (no silent wraparound)
  4. Lightweight handshake verifies peer identity (not just transport trust), and PQ handshake initiator verifies responder pubkey binding
  5. All new validation paths have unit tests that trigger the reject/kill codepath, passing under ASAN/TSAN/UBSAN
**Plans:** 3/3 plans complete

Plans:
- [x] 97-01-PLAN.md -- Overflow-checked arithmetic helpers + wire into all protocol decode/encode paths
- [x] 97-02-PLAN.md -- Pubkey validation, AEAD AD bounds, nonce exhaustion, PQ handshake binding test
- [x] 97-03-PLAN.md -- Lightweight handshake AuthSignature exchange + sanitizer gate

### Phase 98: TTL Enforcement
**Goal**: No expired blob is ever served to any client or peer through any code path
**Depends on**: Phase 95
**Requirements**: TTL-01, TTL-02, TTL-03
**Success Criteria** (what must be TRUE):
  1. BlobFetch handler checks blob expiry before serving and returns appropriate rejection for expired blobs
  2. All six query handlers (Read, List, Stats, Exists, BatchRead, TimeRange) filter expired blobs from results
  3. Expiry timestamp calculation uses saturating arithmetic so timestamp + ttl never overflows uint64
  4. Unit tests prove each path rejects/filters expired blobs, passing under ASAN/TSAN/UBSAN
**Plans**: TBD

### Phase 99: Sync, Resource & Concurrency Correctness
**Goal**: Sync state is leak-free, resource limits are race-free, and coroutine counters are safe across co_await boundaries
**Depends on**: Phase 96, Phase 97
**Requirements**: SYNC-01, SYNC-02, SYNC-03, RES-01, RES-02, RES-03, RES-04, CORO-01
**Success Criteria** (what must be TRUE):
  1. pending_fetches_ entries are cleaned on both successful and rejected ingests, and keys include namespace to prevent cross-namespace hash collision
  2. Phase B reconciliation takes a consistent hash snapshot that cannot be invalidated by concurrent writes
  3. Per-peer subscription count is enforced at the node level with a rejection message sent to the client on limit breach
  4. Bootstrap peer detection considers host+port (not just host), TOCTOU race on capacity/quota is eliminated with atomic check-and-reserve, and quota rebuild iterator bug is fixed
  5. Send/recv counters use proper atomic operations or are confined to a single strand across co_await boundaries, verified by TSAN
**Plans**: TBD

## Progress

**Execution Order:**
Phases execute in numeric order: 95 -> 96 -> 97 -> 98 -> 99
(Phase 97 and 98 can execute in parallel after 95; Phase 99 requires both 96 and 97)

| Phase | Plans Complete | Status | Completed |
|-------|----------------|--------|-----------|
| 95. Code Deduplication | 3/3 | Complete    | 2026-04-07 |
| 96. PeerManager Architecture | 3/3 | Complete    | 2026-04-08 |
| 97. Protocol & Crypto Safety | 3/3 | Complete    | 2026-04-08 |
| 98. TTL Enforcement | 0/TBD | Not started | - |
| 99. Sync, Resource & Concurrency Correctness | 0/TBD | Not started | - |
