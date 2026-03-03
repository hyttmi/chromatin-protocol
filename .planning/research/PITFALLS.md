# Pitfalls Research

**Domain:** Decentralized PQ-secure signed blob store (chromatindb daemon)
**Researched:** 2026-03-03
**Confidence:** MEDIUM-HIGH (training data for libmdbx/LMDB, liboqs, FlatBuffers, P2P networking patterns; no web verification available)

## Critical Pitfalls

### 1. FlatBuffers Non-Determinism Breaking Signature Verification

**What goes wrong:**
FlatBuffers does NOT guarantee deterministic byte output by default. Field ordering, padding, alignment, and vtable deduplication can produce different byte sequences for logically identical data across platforms, compiler versions, or FlatBuffers library versions. If you sign the serialized bytes, a blob serialized on one machine may fail verification when re-serialized on another.

**Why it happens:**
Developers assume "same input = same bytes." FlatBuffers optimizes for zero-copy speed, not canonical form. The `ForceDefaults` option helps (includes zero/default values) but does not solve vtable layout differences. The `object API` (Pack/UnPack) introduces another layer of non-determinism.

**How to avoid:**
- Sign over a **canonical hash input**, not the raw FlatBuffers bytes. Define the signed content explicitly: `SHA3-256(namespace || data || ttl || timestamp)` using a fixed byte concatenation, NOT the serialized FlatBuffer
- The FlatBuffer is the **transport envelope** containing the signature. The signature covers a canonical byte string that is independent of serialization format
- Write a round-trip test: serialize, deserialize, re-serialize, compare bytes. Run this across platforms in CI
- Set `ForceDefaults(true)` on the FlatBufferBuilder always, even if you don't sign the raw bytes -- it prevents subtle field-omission bugs
- If you ever need to verify that a blob is untampered in storage, compare the content hash (SHA3-256 of the canonical input), not the raw stored bytes

**Warning signs:**
- Signature verification failures that appear intermittently or only on certain platforms
- Hash mismatches after deserialize/re-serialize round-trip
- Tests passing on x86 but failing on ARM (different alignment/padding behavior)

**Phase to address:**
Phase 1 (blob format and crypto layer). This is a day-one design decision. If you sign raw FlatBuffer bytes, you are locked into a fragile path.

---

### 2. libmdbx Single-Writer Bottleneck Under Concurrent Ingest

**What goes wrong:**
libmdbx (like LMDB) uses a single-writer, multiple-reader model. Only one write transaction can be active at a time. If the daemon receives blobs from multiple peers simultaneously and each triggers a write transaction, they serialize on a global lock. Under high ingest load, write throughput is gated by one transaction at a time, while reader transactions (serving sync queries) can pile up if a long write holds the lock.

**Why it happens:**
Developers coming from RocksDB or SQLite-WAL expect concurrent writers. libmdbx's B+ tree with copy-on-write requires single-writer for consistency. This is a feature (simplicity, no write-ahead log corruption), but it constrains throughput patterns.

**How to avoid:**
- **Batch writes**: Accumulate incoming blobs in a memory buffer, then flush to libmdbx in a single write transaction at intervals (e.g., every 100ms or every N blobs). One transaction with 100 puts is far faster than 100 transactions with 1 put each
- **Write thread**: Dedicate a single thread to all libmdbx writes. Other threads push to a lock-free queue (e.g., `std::atomic` ring buffer or `moodycamel::ConcurrentQueue`). The write thread drains the queue in batches
- **Keep write transactions short**: Do signature verification BEFORE entering the write transaction. The write transaction should only do the actual put, index updates, and commit -- no crypto
- **Separate read transactions for queries**: Reads never block on writes in libmdbx. Use explicit read transactions for sync/query serving

**Warning signs:**
- Write latency spikes under load (P99 jumps when multiple peers sync simultaneously)
- CPU underutilization (one core busy, others idle) during heavy ingest
- Write ACK latency increasing linearly with peer count

**Phase to address:**
Phase 2 (storage engine). Design the write pipeline before writing storage code. Retrofitting batch writes is painful because it changes the concurrency model.

---

### 3. ML-DSA-87 Verification Cost Dominating Sync

**What goes wrong:**
ML-DSA-87 signature verification is computationally expensive compared to classical signatures (Ed25519 verifies in ~50us; ML-DSA-87 is roughly 10-20x slower). When syncing thousands of blobs from a peer, each blob requires a full ML-DSA-87 verification. A sync of 10,000 blobs can take 10-30 seconds of pure CPU on a server, much longer on lower-end hardware. During this time, the node is unresponsive to other peers.

**Why it happens:**
PQ signatures are inherently larger and slower than classical ones. ML-DSA-87 is NIST Category 5 (maximum security). The cost is the tradeoff for quantum resistance.

**How to avoid:**
- **Verification result cache**: Store a `(blob_hash -> verified)` mapping. Once a blob's signature is verified, never verify it again. This is safe because blobs are content-addressed -- same hash means same content means same signature
- **Async verification pipeline**: Accept blobs into a staging area, verify in a thread pool, promote to main storage on success. Don't block the network I/O thread on verification
- **Rate-limit inbound sync**: Accept at most N blobs per sync round before yielding to serve other peers. Prevents one greedy sync from starving the node
- **Skip expired on receive**: If `timestamp + ttl < now`, don't bother verifying. The blob is already dead
- **Profile verification threads**: Use `std::thread::hardware_concurrency()` workers. Verification is CPU-bound and embarrassingly parallel -- each blob is independent

**Warning signs:**
- Node goes unresponsive during sync with a large peer
- CPU pegged at 100% on one core (verification not parallelized)
- Peer connections timing out while one peer's sync is being processed

**Phase to address:**
Phase 2 (storage/verification) for the cache. Phase 3 (sync engine) for the async pipeline and rate limiting.

---

### 4. PQ-Encrypted Transport Key Exchange Without Authentication

**What goes wrong:**
ML-KEM-1024 provides key encapsulation (Diffie-Hellman equivalent), but by itself does not authenticate peers. A man-in-the-middle can intercept the initial key exchange, establish separate ML-KEM sessions with both sides, and relay/modify all traffic. The channel is encrypted but to the wrong party.

**Why it happens:**
Classical TLS solves this with certificate chains and CAs. In a decentralized system with no CA, authentication must be handled differently. Developers focus on getting encryption working and defer authentication, leaving a window where the transport is MITM-vulnerable.

**How to avoid:**
- **Sign the key exchange**: After ML-KEM encapsulation, both sides sign their ephemeral public key with their ML-DSA-87 identity key. Each side verifies the peer's signature against a known or pinned public key
- **Channel binding**: Derive a "session fingerprint" from the ML-KEM shared secret (e.g., `SHA3-256("session" || shared_secret)`). Both sides sign this fingerprint. This proves the signing key holder is the same party that performed the key exchange
- **Trust-on-first-use (TOFU)**: For bootstrap connections where you don't know the peer's key, pin the key on first successful connection. Alert on key change (SSH model)
- **Mutual authentication is mandatory**: Do not ship a transport layer that only encrypts without authenticating. Even for v0.1.

**Warning signs:**
- Encrypted connections succeed to any endpoint (including adversarial ones) without verification failure
- No peer identity validation after key exchange completes
- Tests that only check "connection encrypted" without checking "connection authenticated"

**Phase to address:**
Phase 3 (transport). Must be designed into the transport protocol from the start. Adding authentication to an unauthenticated protocol is a protocol-breaking change.

---

### 5. Unbounded Blob Storage from Malicious Namespace Spam

**What goes wrong:**
Any entity can generate a keypair and start writing blobs to its own namespace. The node must accept and store these blobs (valid signature, valid namespace ownership). An attacker generates thousands of keypairs, each writing max-size blobs with long TTLs. The node's disk fills with valid-but-worthless data. Unlike traditional DoS, these blobs pass all cryptographic verification.

**Why it happens:**
The system is permissionless by design -- anyone can write. There is no concept of "authorized namespaces" at the database layer. The same property that makes the system censorship-resistant also makes it spam-vulnerable.

**How to avoid:**
- **Per-node storage quotas**: Set a maximum database size (e.g., 10GB). When approaching the limit, evict blobs with the nearest expiry first (they would die soonest anyway)
- **Per-namespace storage limits**: Cap the total bytes any single namespace can consume on this node (e.g., 50MB). Reject new blobs from that namespace once exceeded. Legitimate namespaces are small; spam namespaces hit the cap quickly
- **Configurable namespace allowlist/blocklist**: Node operators can configure which namespaces to replicate. Default: replicate all. Operators who see abuse can blocklist specific namespaces
- **TTL ceiling enforcement**: Node can reject blobs with TTL above a configured maximum (e.g., 30 days). Prevents "permanent spam" even when TTL=0 is technically valid
- **Short default TTL is a feature**: 7-day default TTL means spam self-cleans. Attacker must continuously write to maintain presence

**Warning signs:**
- Disk usage growing faster than expected for legitimate traffic
- Many namespaces with very few, very large blobs
- Storage dominated by a small number of namespaces

**Phase to address:**
Phase 2 (storage engine) for per-node limits. Phase 4 (peer management) for namespace allowlist/blocklist.

---

### 6. Hash-List Diff Sync Not Scaling Beyond Small Networks

**What goes wrong:**
Hash-list diff sync works by both peers exchanging their complete list of blob hashes, then computing the set difference. With 100,000 blobs at 32 bytes each, that is 3.2MB per sync exchange. With 10 peers syncing every 30 seconds, that is 64MB/minute of just hash lists, before any actual blob data. The hash exchange itself becomes the bandwidth bottleneck.

**Why it happens:**
Hash-list diff is the simplest possible sync algorithm: "here is everything I have, what do you need?" It works perfectly for small sets but is O(n) in the total number of blobs, not O(diff) in the number of missing blobs.

**How to avoid:**
- **Start with hash-list diff, but design for upgrade**: The sync protocol should have a version/method negotiation. v1 = hash-list diff. v2 = range-based reconciliation (Negentropy-style). Peers negotiate the best method both support
- **Incremental sync with seq_num**: Instead of exchanging ALL hashes, use per-namespace seq_num to bound the exchange: "give me namespace X, hashes since seq_num Y." This turns O(total_blobs) into O(new_blobs_since_last_sync)
- **Fingerprint-first optimization**: Before exchanging hash lists, exchange a single fingerprint (XXH3 of all blob hashes in sorted order). If fingerprints match, skip the exchange entirely. This makes the "already in sync" case O(1)
- **Plan for Negentropy**: Range-based set reconciliation is O(diff) regardless of total set size. Research Negentropy C/C++ implementations before Phase 3. Even if you start with hash-list diff, the protocol framing should accommodate range messages

**Warning signs:**
- Sync bandwidth growing with total blob count rather than new blob count
- Network utilization high even when nodes are fully synced
- Sync taking longer as the database grows, even with few changes

**Phase to address:**
Phase 3 (sync engine). seq_num-based incremental sync from the start. Negentropy upgrade path designed but deferred.

---

### 7. Timestamp-Based TTL Expiry Without Clock Agreement

**What goes wrong:**
Blob expiry is computed as `timestamp + ttl`. The `timestamp` is set by the blob creator (wall clock time of creation). If the creator's clock is ahead by 1 hour, blobs appear to have 1 hour more life than intended. If a receiving node's clock is behind, it may prune blobs that the creator considers still alive. Nodes with different wall clocks disagree on which blobs are expired.

**Why it happens:**
There is no HLC or logical clock in the design -- timestamps are wall clock. In a decentralized system, wall clocks are never perfectly synchronized. NTP helps but does not guarantee sub-second accuracy across all nodes.

**How to avoid:**
- **Generous expiry grace period**: When checking expiry, add a grace period (e.g., 5 minutes). Don't prune a blob until `timestamp + ttl + grace < now`. This absorbs typical clock skew
- **Don't rely on timestamp for ordering**: Timestamp is for TTL computation only. Blob ordering within a namespace uses seq_num (assigned by the receiving node, always monotonic). This separates the "ordering" concern (local, reliable) from the "expiry" concern (approximate, clock-dependent)
- **Reject far-future timestamps**: If `timestamp > now + MAX_FUTURE_SKEW` (e.g., 10 minutes), reject the blob. Prevents attackers from setting timestamps years in the future to create effectively permanent blobs that bypass TTL
- **Log clock skew**: When receiving blobs, log `abs(blob_timestamp - now)`. Alert if median skew exceeds a threshold. Helps detect nodes with bad clocks

**Warning signs:**
- Blobs disappearing on some nodes before others
- Blob counts diverging between nodes that should be in sync
- Blobs arriving with timestamps significantly ahead of the receiving node's clock

**Phase to address:**
Phase 2 (storage and ingest). Timestamp validation must be in the ingest path from day one.

## Technical Debt Patterns

| Shortcut | Immediate Benefit | Long-term Cost | When Acceptable |
|----------|-------------------|----------------|-----------------|
| Single-threaded event loop | Simpler to reason about, no mutex bugs | Cannot use multiple cores for verification/sync | Pre-MVP only. Replace with thread pool before any perf testing |
| No write batching | Simpler storage code, immediate durability | Write throughput limited to ~1000 tx/sec on SSD | MVP if write rates are low. Must fix before multi-peer sync |
| Raw TCP without framing | Faster to prototype | Message boundaries lost, partial reads cause parsing failures | Never. Frame messages from day one (length-prefix or FlatBuffers size-prefix) |
| Hardcoded config values | No config parsing code needed | Cannot tune per-deployment, requires recompile | Pre-MVP only. Must be configurable before deploying multiple nodes |
| No verification cache | Simpler ingest path | Redundant verification on blobs received from multiple peers, during restart | MVP is acceptable. Must add before any sync testing with real data volumes |
| Skipping peer authentication | Transport "works" faster, fewer moving parts | MITM vulnerability on every connection | Never. Authenticate from the first version that accepts peer connections |

## Integration Gotchas

### libmdbx

| Common Mistake | Correct Approach |
|----------------|------------------|
| Opening the database with default map size, hitting `MDBX_MAP_FULL` when it grows | Set `mdbx_env_set_geometry()` with a large upper bound (e.g., 64GB). libmdbx grows the file dynamically but respects the upper limit. The file does not consume disk until data is written |
| Using `mdbx_env_open()` without `MDBX_WRITEMAP` and expecting good write performance | Use `MDBX_WRITEMAP` for significantly better write performance (avoids double-buffering). Acceptable risk: process crash during write can leave partially written page, but libmdbx's crash recovery handles this |
| Holding read transactions open for a long time | Stale read transactions prevent libmdbx from reclaiming old pages. This causes the database file to grow without bound. Always close read transactions promptly. Use RAII wrappers (transaction object with destructor) |
| Calling `mdbx_dbi_open()` inside a normal transaction | DBI handles should be opened once (in a dedicated transaction at startup) and reused for the lifetime of the environment. Opening inside a data transaction can cause subtle locking issues |
| Storing large values (>page size) without considering overflow pages | Values larger than ~4000 bytes (depending on page size) use overflow pages. ML-DSA-87 signatures are 4627 bytes and pubkeys are 2592 bytes -- each blob value will likely overflow. This is fine for reads (zero-copy still works) but impacts write amplification. Consider storing signature and pubkey separately from data if write perf matters |

### liboqs (ML-DSA-87 / ML-KEM-1024)

| Common Mistake | Correct Approach |
|----------------|------------------|
| Not checking return codes from `OQS_SIG_verify()` | `OQS_SIG_verify()` returns `OQS_SUCCESS` or `OQS_ERROR`. A common C mistake is treating the return as boolean (nonzero = true = success). `OQS_SUCCESS` is `0`. Always check `== OQS_SUCCESS` explicitly |
| Reusing ML-KEM decapsulation keys | ML-KEM is designed for ephemeral key exchange. Generate a fresh keypair for each connection. Reusing decapsulation keys across sessions reduces security and can enable certain attacks |
| Not calling `OQS_SIG_free()` / `OQS_KEM_free()` | liboqs allocates internal state. Leaking these in a long-running daemon means unbounded memory growth |
| Assuming constant-time operations | liboqs aims for constant-time crypto but this depends on the backend (internal, OpenSSL, etc.). Verify which backend is active. Side-channel resistance is only as good as the weakest link |
| Using liboqs SHA3 for all hashing | liboqs's SHA3 implementation may not be optimized for bulk hashing. For content-addressing thousands of blobs, consider using OpenSSL's SHA3 (which uses hardware acceleration where available). Use liboqs for signature/KEM operations only |

### FlatBuffers

| Common Mistake | Correct Approach |
|----------------|------------------|
| Using default `CreateString`/`CreateVector` inside a table creation | All child objects (strings, vectors, nested tables) must be created BEFORE `Start`/`End` of the parent table. FlatBuffers builds bottom-up. Creating children mid-table is undefined behavior |
| Assuming field order in the buffer matches schema order | FlatBuffers uses vtables for field access. The physical byte layout depends on creation order and vtable deduplication. Never parse FlatBuffers by byte offset manually -- always use generated accessors |
| Not calling `builder.ForceDefaults(true)` | By default, fields with default values are omitted from the buffer. This saves space but means two buffers for "the same" data can have different bytes (one omits the field, one includes it). Always force defaults when deterministic bytes matter |
| Trusting FlatBuffers input without verification | `flatbuffers::Verifier` checks bounds and internal consistency. Always verify untrusted input before accessing. Unverified malicious buffers can cause out-of-bounds reads |
| Schema evolution breaking existing signed data | Adding/removing fields changes the serialization. If signatures cover raw FlatBuffer bytes, schema evolution breaks verification of old blobs. Sign a canonical hash input instead |

## Performance Traps

| Trap | Symptoms | Prevention | When It Breaks |
|------|----------|------------|----------------|
| Verifying every blob on every sync | CPU pegged during sync, sync takes minutes | Verification cache keyed by blob hash. Verify once, cache forever (blob is immutable) | >1,000 blobs per sync round |
| Expiry scan on every write | Write latency spikes periodically, jittery ACKs | Run expiry scan on a timer (e.g., every 60 seconds) in a separate read+write transaction, not inline with ingest | >10,000 stored blobs |
| Full hash-list exchange for sync | Bandwidth dominated by sync overhead, not blob data | Incremental sync via seq_num: only exchange hashes for blobs since last sync checkpoint | >10,000 total blobs per node |
| String comparison for namespace matching | CPU overhead on hot path | Namespaces are 32 bytes. Use `memcmp` or compare as `uint64_t[4]`. Never convert to hex string for comparison | Any load -- this is the hot path |
| Allocating per-blob during sync | GC pressure (in managed languages) or heap fragmentation (in C++) | Pre-allocate blob buffers, use arena allocators for FlatBuffers (`FlatBufferBuilder` reuse), pool verification contexts | >100 blobs/second sustained ingest |
| libmdbx read transaction per query | Transaction overhead for high-frequency reads | Use a long-lived read transaction for burst reads (e.g., during sync serving), refresh periodically with `mdbx_txn_renew()`. But don't hold too long (see stale read transaction gotcha) | >1,000 reads/second |

## Security Mistakes

| Mistake | Risk | Prevention |
|---------|------|------------|
| Signing the FlatBuffer bytes instead of a canonical hash | Platform-dependent serialization differences cause signature failures or allow signature malleability | Define canonical signed content as a fixed byte concatenation, independent of serialization format |
| Not validating `SHA3-256(pubkey) == namespace` on ingest | Accepting blobs where namespace does not match the public key. Attacker writes to any namespace | This check is the FIRST thing in the ingest path. Before sig verification, before storage. Reject immediately on mismatch |
| Accepting blobs with future timestamps without bounds | Attacker sets timestamp to year 2100, creating effectively permanent blobs that ignore TTL | Reject blobs where `timestamp > now + MAX_FUTURE_SKEW`. Suggested: 10 minutes |
| Not zeroing private key memory after use | Key material persists in memory, vulnerable to cold boot attacks or memory dumps | Use `OPENSSL_cleanse()` or `explicit_bzero()` on key buffers. Consider `mlock()` for key material pages to prevent swapping to disk |
| TCP connection without encryption as fallback | "Plaintext fallback" for debugging leaks into production. Blob content, signatures, and peer topology exposed | No plaintext mode. Not even behind a flag. If PQ handshake fails, connection fails. Period |
| Accepting arbitrarily large blobs | Memory exhaustion via a single oversized blob. Attacker sends a "blob" claiming 4GB data field | Enforce max blob size (e.g., 64KB data + overhead). Reject before reading the full blob into memory. Check the size field in the FlatBuffer header first |
| Not rate-limiting connections per IP | Single IP opens thousands of connections, exhausting file descriptors | Per-IP connection limit (e.g., 10). Per-IP ingest rate limit (e.g., 100 blobs/minute) |

## "Looks Done But Isn't" Checklist

- [ ] **Blob ingest:** Often missing namespace-pubkey validation -- verify `SHA3-256(pubkey) == namespace` is checked before signature verification
- [ ] **Signature verification:** Often missing return code check -- verify `OQS_SIG_verify()` result is compared with `== OQS_SUCCESS`, not boolean truthiness
- [ ] **PQ transport:** Often missing authentication -- verify post-handshake authentication (signed key exchange) is implemented, not just encryption
- [ ] **Expiry:** Often missing the grace period -- verify nodes don't prune blobs that other nodes consider alive (clock skew handling)
- [ ] **Sync:** Often missing "already in sync" fast path -- verify that two synced nodes exchange O(1) data (fingerprint comparison), not O(n) hash lists
- [ ] **Storage:** Often missing geometry configuration -- verify `mdbx_env_set_geometry()` is called with appropriate upper bounds, not relying on defaults
- [ ] **Wire protocol:** Often missing message framing -- verify TCP stream has length-prefix framing, not relying on "one send = one message"
- [ ] **Peer management:** Often missing connection limits -- verify per-IP limits prevent resource exhaustion
- [ ] **Config:** Often missing runtime tunability -- verify max blob size, TTL ceiling, storage limits, peer limits are configurable, not compiled in
- [ ] **Graceful shutdown:** Often missing transaction cleanup -- verify all libmdbx transactions are committed/aborted and environment is closed on SIGTERM

## Recovery Strategies

| Pitfall | Recovery Cost | Recovery Steps |
|---------|---------------|----------------|
| FlatBuffers signing (non-deterministic bytes) | HIGH | Requires protocol version bump. Old blobs become unverifiable if canonical form changes. Must support both old and new verification paths during migration |
| libmdbx write bottleneck | MEDIUM | Refactor ingest to batch writes. Requires adding a write queue and changing the concurrency model, but does not change the protocol |
| Verification CPU bottleneck | LOW | Add verification cache (new libmdbx sub-database, ~50 LOC). Add thread pool. No protocol changes needed |
| Unauthenticated transport | HIGH | Protocol-breaking change. All existing connections must be re-established. Peers running old version cannot connect to new version |
| Storage spam from malicious namespaces | MEDIUM | Add per-namespace quotas and blocklist. Existing spam blobs expire naturally via TTL. Manual intervention: wipe and re-sync from trusted peer |
| Hash-list sync scaling | MEDIUM | Upgrade to incremental (seq_num-based) sync. Protocol change but additive -- new method alongside old. Negotiate on connection |
| Clock skew causing expiry disagreement | LOW | Add grace period to expiry checks. Retroactive: blobs that were prematurely pruned will be re-synced from peers that still have them |

## Pitfall-to-Phase Mapping

| Pitfall | Prevention Phase | Verification |
|---------|------------------|--------------|
| FlatBuffers non-determinism | Phase 1 (blob format) | Round-trip test: serialize, deserialize, re-serialize, compare bytes. Cross-platform CI (x86 + ARM) |
| libmdbx write bottleneck | Phase 2 (storage engine) | Benchmark: sustained 1000 blob/sec ingest with 5 concurrent peers. Measure P99 write latency |
| ML-DSA-87 verification cost | Phase 2 (verification cache), Phase 3 (async pipeline) | Benchmark: sync 10,000 blobs. Time must be <5 seconds with cache warm, <30 seconds cold |
| Transport authentication | Phase 3 (transport layer) | Test: MITM proxy between two nodes. Connection must fail or be detected |
| Storage spam | Phase 2 (storage limits), Phase 4 (blocklist) | Test: generate 10,000 namespaces with max-size blobs. Node must reject when quota exceeded, not crash or OOM |
| Sync scaling | Phase 3 (sync engine) | Benchmark: two nodes with 100K blobs, 10 blobs different. Sync must transfer O(diff) data, not O(total) |
| Timestamp/clock skew | Phase 2 (ingest validation) | Test: submit blob with timestamp 1 hour in future. Must be rejected. Submit with timestamp 5 seconds in future. Must be accepted |

## Sources

- libmdbx documentation and LMDB architecture knowledge -- MEDIUM confidence (training data, no web verification)
- liboqs API patterns from PQCC project experience documented in project memory -- HIGH confidence (project history)
- FlatBuffers deterministic encoding limitations -- MEDIUM confidence (training data, well-documented issue in FlatBuffers community)
- P2P networking patterns (Bitcoin, Nostr, IPFS) -- MEDIUM confidence (training data, widely documented)
- ML-DSA / ML-KEM NIST standards behavior -- MEDIUM confidence (training data from FIPS 204/203 specifications)
- Previous chromatindb project failure modes (DHT complexity, dependency hell) -- HIGH confidence (project memory)

---
*Pitfalls research for: chromatindb -- decentralized PQ-secure signed blob store daemon*
*Researched: 2026-03-03*
