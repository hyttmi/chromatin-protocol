# Pitfalls Research

**Domain:** Decentralized replicated key-value database with post-quantum cryptography
**Researched:** 2026-03-03
**Confidence:** MEDIUM-HIGH (patterns documented in distributed systems literature and real-world incidents)

## Critical Pitfalls

### 1. HLC Clock Skew Causing Silent Data Loss

**The Problem:** A device with a clock set far in the future generates operations with inflated HLC timestamps. Since LWW uses HLC for ordering, these operations "win" against all legitimate operations. Worse: HLC merge propagates the inflated clock to other peers, poisoning the namespace's HLC ceiling permanently.

**Warning Signs:**
- Sudden jump in HLC values for a namespace
- Operations from one device consistently winning LWW
- HLC counter wrapping or hitting ceiling values

**Prevention:**
- **Bounded skew detection**: On HLC merge, reject remote timestamps more than MAX_SKEW (5 minutes) ahead of local wall clock. Log warning, don't merge the inflated clock
- **Per-namespace HLC ceiling**: Track highest HLC per namespace. Alert if ceiling jumps by more than MAX_SKEW in one operation
- **Don't propagate poisoned clocks**: Accept the operation (valid signature) but don't advance local HLC to match

**Phase:** Must be in Phase 1 (HLC implementation). Retrofitting skew detection is much harder.

**Severity:** CRITICAL — can cause permanent data loss with no recovery path.

---

### 2. Append-Only Log Growth Without Pruning

**The Problem:** Every operation is preserved. ML-DSA-87 signatures are 4627 bytes each. 10,000 operations = ~50MB in signatures alone. On mobile, unsustainable.

**Warning Signs:**
- Storage growing linearly with no plateau
- Sync times increasing as operation count grows
- Mobile clients running out of storage

**Prevention:**
- **TTL-based expiry solves this structurally**: Operations expire and get pruned. Storage is bounded by (write rate × average TTL). This is a core design feature, not an afterthought
- **Profile keys (TTL=0) are small and few**: Public keys, bio, relay hints — a handful of keys per identity. Unbounded growth only from ephemeral data, which expires
- **Reserve SNAPSHOT op_type in format**: Even if deferred, the operation format should support snapshots for future compaction of long-lived namespaces
- **Expiry index in libmdbx**: Secondary index sorted by expiry timestamp for efficient batch pruning

**Phase:** TTL in Phase 2 (operation format). Expiry scanning in Phase 2 (storage). Snapshot type reserved in format but implemented later.

**Severity:** CRITICAL — but largely solved by TTL-first design.

---

### 3. Signature Verification Bottleneck

**The Problem:** ML-DSA-87 verification takes ~1-3ms per operation on ARM (mobile). Syncing 10,000 operations = 10-30 seconds of pure CPU. Device is unresponsive during initial sync. Battery drain severe.

**Warning Signs:**
- Sync blocking the main thread
- Battery complaints
- Initial namespace sync taking minutes

**Prevention:**
- **Verification cache**: Once an op hash is verified, cache the result in libmdbx. Never re-verify the same hash
- **TTL reduces the problem**: Fewer ops to verify — expired ops don't sync. Active set is bounded by TTL
- **Async verification pipeline**: Verify in background thread. Show "syncing" state, reject on failure
- **Batch verification**: Check if liboqs supports ML-DSA-87 batch verification (verify N signatures faster than N × single)
- **Signed snapshots (future)**: Verify one snapshot signature instead of N individual ops — O(1) vs O(n)

**Phase:** Verification cache in Phase 2. Async pipeline in Phase 4 (sync engine).

**Severity:** CRITICAL — makes system unusable on mobile without mitigation.

---

### 4. Equivocation / Fork Attacks

**The Problem:** A malicious namespace owner sends different operations to different relays. Relay A has op X for key "foo", Relay B has op Y for key "foo", both with same sequence number but different values/hashes. Peers syncing from different relays see different states.

**Warning Signs:**
- Two operations from same author with same (namespace, seq) but different hashes
- State divergence between peers on different relay sets

**Prevention:**
- **Content-addressed operation IDs**: Sync by op hash, not by (key, timestamp). Both conflicting ops get replicated to all peers
- **Deterministic resolution**: Both ops are valid (both signed). LWW picks winner deterministically (highest HLC, then lowest hash). All peers converge regardless of receive order
- **Equivocation detection**: Two ops with same (author, namespace, seq) but different hashes = equivocation. Flag the author. Applications decide policy
- **Don't use seq for dedup**: Seq detects gaps (missing ops), not duplicates. Content hash is the dedup key

**Phase:** Built into Phase 2 (operation format). Detection in Phase 3.

**Severity:** CRITICAL — but LWW + content-addressing handles it naturally.

---

### 5. Range-Based Reconciliation Ordering Bugs

**The Problem:** Set reconciliation requires both peers to sort operations identically. Any ambiguity in sort order (two ops with identical HLC) means different range fingerprints. Sync never converges or silently misses operations.

**Warning Signs:**
- Sync rounds that never terminate
- Operations present on relay but never synced to client
- Fingerprint mismatches that persist after full sync

**Prevention:**
- **Strict canonical ordering**: Sort key is `(HLC: uint64, SHA3_hash: bytes[32])`. HLC is unique-ish (16-bit counter per ms), hash is unique per op. Together guaranteed unique — no ties
- **Property-based testing**: Generate random op sets, split across peers, verify reconciliation produces identical sets. Thousands of iterations with randomized timing
- **Use proven algorithm**: Negentropy protocol is well-specified and tested in production (Nostr). Port it rather than inventing new reconciliation

**Phase:** Phase 4 (sync engine). Must be thoroughly tested.

**Severity:** CRITICAL — silent data loss if reconciliation has ordering bugs.

---

### 6. Capability Revocation Timing

**The Problem:** Owner revokes a grant. REVOKE propagates to relays. But the revoked grantee can still write to relays that haven't seen the revocation. No coordinator to enforce instant revocation.

**Warning Signs:**
- Operations accepted after revocation timestamp
- Inconsistent permission state across relays

**Prevention:**
- **Mandatory TTL on all grants**: Capabilities expire. Even without explicit revocation, access is time-bounded. Default: 24 hours
- **Accept-then-verify**: Operations from grantees validated against grant state at the operation's HLC timestamp (not receive time). Revocation is retroactive
- **Short-lived grants with auto-renewal**: Chain of 1-hour grants instead of one long grant. Revocation = stop issuing renewals. Window bounded by grant duration

**Phase:** Phase 2 (namespace/AuthZ). Grant TTL must be in the operation format from day one.

**Severity:** CRITICAL — capability security is meaningless without bounded revocation.

## Moderate Pitfalls

### 7. PQ Signature Size Overhead

**The Problem:** ML-DSA-87 signatures are 4627 bytes. A "set key=value" where value is 10 bytes produces ~5KB operation. Wire overhead is 99%+ metadata for small values.

**Prevention:**
- Accept for MVP — PQ security is non-negotiable
- TTL-based expiry means less data in transit overall
- Consider ML-DSA-65 (3309-byte sigs, NIST Cat 3) as configurable option
- Batch operations: multiple SET/DELETE in a single signed operation (future)

**Phase:** Format decision in Phase 2.

---

### 8. Sync Protocol Deadlocks and Livelock

**The Problem:** Two peers simultaneously initiate reconciliation. Both send RANGE messages. Both respond with finer splits. Exchange oscillates without converging.

**Prevention:**
- **Client-server sync model**: Client initiates, relay responds. No peer-to-peer sync in v1
- **Timeout on sync rounds**: If reconciliation doesn't converge in N rounds (e.g., 10), fall back to full hash list exchange
- **Idempotent messages**: Any sync message safely replayable. No session state beyond current exchange

**Phase:** Phase 4 (sync engine design).

---

### 9. Inbox Spam / DoS via Open-Write Namespaces

**The Problem:** If namespaces accept writes from strangers, an attacker can flood with garbage. Valid signature (attacker has own key), passes verification. Owner's storage fills with spam.

**Prevention:**
- **Capability-only inboxes for MVP**: Only grantees can write. Strangers cannot. Eliminates spam at cost of requiring explicit contact addition
- **PoW stamps for strangers (v0.2)**: Proof-of-work on inbox writes from non-grantees
- **Owner-side pruning**: Owner publishes DELETE tombstones for spam
- **Rate limits in grants**: max_ops field caps writes per grantee
- **TTL helps**: Spam expires. Attacker must continuously spend effort to maintain spam volume

**Phase:** Capability-only in Phase 2. PoW in v0.2.

---

### 10. Multi-Device Split-Brain

**The Problem:** Same identity on two devices. Both offline. Both write same key. On reconnect, LWW picks one — other is "lost" (in log but not in materialized state).

**Prevention:**
- **By design for LWW**: Documented tradeoff. One wins
- **Expose conflict history in API**: Even with LWW winner, expose "losing" ops for applications that want conflict UI
- **Key prefixes per device**: `notes/device-a/draft` vs `notes/device-b/draft` — let app merge

**Phase:** Multi-device in later phase. Conflict history API in Phase 3.

---

### 11. Replay Attacks

**The Problem:** Attacker replays valid operation to relays.

**Prevention:**
- **Non-issue by design**: Operations are content-addressed. Replaying existing operation is a no-op — same hash = already stored. This is free deduplication, not an attack
- FlatBuffers deterministic encoding prevents same logical write producing different hashes

**Phase:** Handled by design in Phase 2.

---

### 12. Inadequate Distributed Testing

**The Problem:** Unit tests pass. Integration with one client + one relay passes. Real-world multi-client, multi-relay, partitions, and clock skew reveal bugs.

**Prevention:**
- **Simulation testing**: Deterministic simulator with virtual clients and relays, controllable timing, partitions, clock skew (FoundationDB-inspired)
- **Property-based sync tests**: Random op sequences, partition across N replicas, sync in random orders, verify convergence
- **Test multi-client from day one**: Don't defer to "later"

**Phase:** Testing infrastructure from Phase 2 onward. Property-based sync tests in Phase 4.

## Minor Pitfalls

### 13. Serialization Format Lock-In

**The Problem:** Once operations are signed and distributed, serialization format is frozen. Changing FlatBuffers → CBOR later means existing ops become unverifiable.

**Prevention:**
- **Version byte in operation format**: First byte = format version. Future versions can use different serialization
- **Canonical form specification**: Document exactly how ops are serialized for hashing/signing
- **Round-trip test**: Serialize → hash → sign → deserialize → re-serialize → verify hash matches

**Phase:** Phase 2 (operation format). Version byte must be in v0.1.

---

### 14. Relay Discovery and Failover

**The Problem:** Client connects to hardcoded relay list. Relay goes down. No way to find alternatives.

**Prevention:**
- **Relay hints in profile namespace**: Owner publishes preferred relays as profile data. Peers learn relay URLs from profile
- **Hardcoded bootstrap relays for MVP**: Acceptable for v0.1
- **Relay list in config, not code**: Caller provides relay URLs. Library doesn't discover relays

**Phase:** Not in MVP. Application concern. Profile relay hints help naturally.

---

### 15. Deletion Semantics in Append-Only Logs

**The Problem:** DELETE = tombstone. Original operation still in log / on relays. "Delete" doesn't mean "gone."

**Prevention:**
- **TTL solves this**: All data expires. Deleted or not, it's gone after TTL. For sensitive data, shorter TTL
- **Encrypt values**: If encrypted, deleting the key (or letting it expire) = crypto-shredding
- **Document clearly**: DELETE means "superseded." TTL means "eventually gone"

**Phase:** Document in v0.1. Largely solved by ephemeral-by-default design.

---
*Pitfalls research for: decentralized replicated KV database with PQ crypto*
*Researched: 2026-03-03*
