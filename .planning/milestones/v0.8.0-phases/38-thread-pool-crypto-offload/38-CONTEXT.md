# Phase 38: Thread Pool Crypto Offload - Context

**Gathered:** 2026-03-19
**Status:** Ready for planning

<domain>
## Phase Boundary

ML-DSA-87 signature verification and SHA3-256 content hashing dispatched to asio::thread_pool workers, freeing the event loop from CPU-bound crypto operations. Connection-scoped AEAD state (ChaCha20-Poly1305 nonce counters) never touched by workers. Thread pool worker count configurable via config JSON.

</domain>

<decisions>
## Implementation Decisions

### Config field naming
- Field name: `worker_threads` in config JSON
- Default: `std::thread::hardware_concurrency()` when omitted
- `0` = auto-detect (maps to `hardware_concurrency()`, same as omitting)
- Clamped to `hardware_concurrency()` with warning if value exceeds it
- Documented in README config table
- Logged at startup with "(auto-detected)" or "(configured)" suffix
- Startup-only — NOT SIGHUP-reloadable (restart required to change)

### Offload scope
- ALL `Signer::verify()` calls offloaded — ingest, delete, AND handshake auth (uniform model, no conditional logic)
- ALL `sha3_256()` calls offloaded — including small ones like pubkey-to-namespace derivation (uniform model)
- Content hash (`blob_hash`) dispatched as a SEPARATE earlier offload — result returns to event loop for dedup check (`has_blob`). If duplicate, skip verify dispatch entirely
- Signing input computation (`build_signing_input`) + `Signer::verify()` BUNDLED in a single thread pool dispatch — one round-trip, hash output feeds directly into verify

### Thread pool lifecycle
- Pool owned in `main()`, passed by reference to consumers (same pattern as `io_context`)
- Shutdown ordering: `ioc.run()` completes first (graceful connection drain), then `pool.join()` waits for in-flight crypto

### Claude's Discretion
- Whether to use a thin wrapper class or raw `asio::thread_pool` + header-only offload helper
- Exact offload helper API (free function template vs class method)
- How thread pool reference is threaded through to Connection (via Server? via lambda capture?)
- Task ordering and plan decomposition

</decisions>

<specifics>
## Specific Ideas

- Two-dispatch pattern per new blob ingest: (1) offload blob_hash, return to event loop for dedup check, (2) offload build_signing_input + verify as bundled unit. Duplicates pay only one dispatch.
- thread_local OQS_SIG* in Signer::verify() already prepared in Phase 33 — works correctly with thread pool (one context per worker thread).

</specifics>

<code_context>
## Existing Code Insights

### Reusable Assets
- `Signer::verify()` (db/crypto/signing.cpp:74): Static method, thread_local OQS_SIG* — already thread-safe for pool workers
- `wire::blob_hash()` (db/wire/codec.cpp): Computes SHA3-256 of encoded blob — pure function, safe to offload
- `wire::build_signing_input()` (db/wire/codec.cpp): Incremental SHA3-256 over canonical fields — pure function, safe to offload
- `crypto::sha3_256()` (db/crypto/hash.cpp): Takes span, returns array<32> — pure function, safe to offload

### Established Patterns
- `io_context` owned in main(), passed by ref to Server/PeerManager — same pattern for thread pool
- Timer-cancel pattern for async coordination (steady_timer on stack, pointer in PeerInfo)
- Coroutine-based async throughout (co_await for all I/O)

### Integration Points
- `BlobEngine::ingest()` (db/engine/engine.cpp:92): Steps 2.5 (blob_hash) and 3 (verify) need offload. Engine needs thread pool reference.
- `BlobEngine::delete_blob()` (db/engine/engine.cpp:254): Step 3 (verify) needs offload.
- `Connection::do_handshake_*` (db/net/connection.cpp): 4 call sites for Signer::verify. Connection needs thread pool reference.
- `PeerManager::on_peer_message` (db/peer/peer_manager.cpp:526): Calls engine_.ingest() — already on event loop, engine handles offload internally.
- `main()` (db/main.cpp): Creates io_context, config, engine, server — thread pool created here alongside these.
- Config: `db/config/config.h` — add `uint32_t worker_threads` field.

</code_context>

<deferred>
## Deferred Ideas

None — discussion stayed within phase scope.

</deferred>

---

*Phase: 38-thread-pool-crypto-offload*
*Context gathered: 2026-03-19*
