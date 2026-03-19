# Research Summary: v0.8.0 Protocol Scalability

**Domain:** Sync set reconciliation, sync rate limiting, and thread pool crypto offload for a decentralized PQ-secure blob store
**Researched:** 2026-03-19
**Overall confidence:** HIGH

## Executive Summary

chromatindb v0.8.0 fixes a fundamental protocol scaling flaw: the sync protocol exchanges the full hash list for every namespace with changes, which is O(N) in total blobs and has a hard cliff at ~3.4M blobs per namespace (110 MiB MAX_FRAME_SIZE). This is not an optimization -- it is a correctness issue that makes the protocol unusable at scale.

The recommended approach is **negentropy** -- a header-only C++ library implementing Range-Based Set Reconciliation (RBSR). negentropy replaces the full hash list exchange with an O(differences) protocol requiring O(log N) round-trips. It is battle-tested in the Nostr ecosystem (strfry relay syncs 10M+ element datasets in production), MIT-licensed, and requires only a trivial 5-line patch to replace its SHA-256 dependency with the existing SHA3-256 from liboqs. For a namespace with 1M blobs and 10 differences, wire overhead drops from 32 MB to ~100 KB.

The milestone also adds sync rate limiting (closing an abuse vector where sync messages bypass all rate limiting) and thread pool crypto offload (unblocking the io_context thread during ML-DSA-87 verification). Rate limiting requires no new libraries -- just extending the existing token bucket and adding a sync initiation cooldown. Crypto offload uses `asio::thread_pool` already bundled in Standalone Asio 1.38.0.

**One new dependency: negentropy (header-only, vendored, MIT). Zero new compiled dependencies. Zero version bumps for existing libraries.**

The primary risks are: AEAD nonce counter access from thread pool workers (catastrophic if Connection references leak to pool threads), libmdbx transaction thread affinity violations (pool threads must never access Storage), and reconciliation state consistency during concurrent blob ingestion (solved by single-threaded io_context -- no co_await between reads). All three have clear mitigations documented in PITFALLS.md.

## Key Findings

**Stack:** negentropy (header-only, vendored with SHA3-256 patch) for set reconciliation + asio::thread_pool for crypto offload + existing token bucket extension for sync rate limiting. One new header-only dep, zero compiled deps.

**Architecture:** SyncProtocol gains negentropy reconciliation per namespace. PeerManager gains thread pool for crypto offload and sync cooldown enforcement. New Reconcile wire message type replaces HashList. NamespaceList exchange remains for cursor-based skip decisions.

**Critical pitfall:** Thread pool workers must NEVER touch Connection (AEAD nonces) or Storage (libmdbx thread affinity). The offload boundary is "bytes in, bool/hash out" -- enforce via value-copy lambda captures, never reference captures.

## Implications for Roadmap

Based on research, suggested 3-phase structure:

1. **Set Reconciliation (negentropy integration)** - Fixes the fundamental protocol flaw
   - Addresses: O(N) hash list exchange, MAX_FRAME_SIZE cliff at ~3.4M blobs
   - Avoids: Persistent Merkle tree (write amplification), IBLT (capacity estimation failure)
   - Includes: Vendor negentropy with SHA3-256 patch, new Reconcile message type, per-namespace reconciliation loop, integration with v0.7.0 sync cursors
   - Risk: MEDIUM (wire protocol changes, multi-round-trip sync flow)

2. **Sync Rate Limiting** - Closes abuse vector
   - Addresses: SyncRequest bypasses all rate limiting, CPU/bandwidth amplification
   - Avoids: Over-aggressive limiting (rate limit initiation, not transfer)
   - Includes: Sync cooldown per peer, extend token bucket to sync messages, concurrent sync limit
   - Risk: LOW (extends existing infrastructure)

3. **Thread Pool Crypto Offload** - Breaks single-thread throughput ceiling
   - Addresses: ML-DSA-87 verify blocks io_context for 20-30ms per 1 MiB blob
   - Avoids: Passing Connection refs to pool (AEAD nonce disaster), Storage access from pool (libmdbx thread mismatch)
   - Includes: asio::thread_pool, two-dispatch executor switching pattern, offload helper function
   - Risk: MEDIUM (executor switching correctness, shutdown ordering)

**Phase ordering rationale:**
- Set reconciliation first: fixes protocol correctness, largest change, independent of rate limiting and thread pool
- Sync rate limiting second: low effort, high security value, benefits from reconciliation being in place (rate-limits the new message types)
- Thread pool last: highest risk, benefits from stable protocol (offloads the new reconciliation-driven ingest path)

**Research flags for phases:**
- Phase 1 (Set Reconciliation): Needs careful plan for negentropy Vector construction (timestamp mapping from blobs, zero-hash sentinel filtering), Reconcile message encoding, and multi-round-trip timeout handling
- Phase 2 (Sync Rate Limiting): Standard patterns, unlikely to need research
- Phase 3 (Thread Pool): Needs explicit design for the offload helper function, shutdown ordering, and thread-safety verification

## Confidence Assessment

| Area | Confidence | Notes |
|------|------------|-------|
| Stack | HIGH | negentropy is production-proven (strfry, 10M+ elements). SHA-256 replacement verified to single call site. asio::thread_pool confirmed in Asio 1.38.0. |
| Features | HIGH | Feature set derived from concrete protocol analysis (O(N) hash exchange, rate limiting gap at peer_manager.cpp:436, single-threaded crypto bottleneck). |
| Architecture | HIGH | negentropy Vector backend avoids write amplification. Two-dispatch pattern for crypto offload is documented Asio pattern. Rate limiting extends existing token bucket. |
| Pitfalls | HIGH | 14 pitfalls catalogued with specific prevention strategies. Critical pitfalls (AEAD nonce, libmdbx thread affinity) verified against codebase. |

## Gaps to Address

- **negentropy timestamp mapping**: Blobs have a `timestamp` field (uint64, writer-set). Need to verify this provides sufficient ordering for negentropy (what happens when multiple blobs have identical timestamps? negentropy sorts by timestamp then lexically by ID -- acceptable).
- **Zero-hash sentinel filtering**: `get_hashes_by_namespace()` returns zero-hash sentinels from deleted blobs. These must be filtered before inserting into negentropy Vector. Need to verify this is done correctly.
- **Reconciliation timeout handling**: Multi-round-trip reconciliation needs per-round timeouts, not just overall sync timeout. Need to define timeout strategy during planning.
- **Thread pool sizing tuning**: Default of `hardware_concurrency() - 1` may not be optimal. Benchmark after implementation.
- **negentropy version pinning**: Currently "latest master" -- should pin to a specific commit hash when vendoring for reproducibility.
