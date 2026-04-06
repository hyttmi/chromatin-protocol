# Pitfalls Research

**Domain:** ACL revocation, key versioning, and group membership revocation for a decentralized PQ-encrypted eventually-consistent blob store (chromatindb v2.1.1)
**Researched:** 2026-04-05
**Confidence:** HIGH (codebase-verified, protocol-level analysis of existing mechanisms, informed by distributed systems revocation literature)

## Critical Pitfalls

### Pitfall 1: Revocation propagation window -- delegate writes accepted on stale nodes

**What goes wrong:**
Owner tombstones a delegation blob on Node A. The tombstone takes time to replicate to Node B and Node C. During this propagation window, the revoked delegate can still write to Node B or Node C because those nodes still have the delegation entry in their `delegation_map`. The existing Docker integration test (test_acl04_revocation.sh) uses a 30-second sleep to account for this, but that is a test convenience, not a guarantee. In production with network partitions, this window can be minutes or longer.

**Why it happens:**
Delegation revocation uses the same eventually-consistent tombstone replication as all other data. The tombstone must: (1) replicate via BlobNotify/BlobFetch or safety-net reconciliation, (2) trigger `delete_blob_data()` on the remote node, which (3) erases the `delegation_map` entry. Any delay in steps 1-3 leaves a window where the delegation appears valid.

This is inherent to eventually-consistent systems -- there is no global clock or consensus mechanism. The same problem exists in distributed ACL systems (see: Hoop.dev's analysis of "access revocation privilege escalation" and Protocol Labs' CRDT research issue #8 on decentralized access control).

**How to avoid:**
Accept the propagation window as a design constraint, but minimize and document it:
1. **Document the invariant explicitly in PROTOCOL.md:** "Revocation takes effect locally immediately and propagates with eventual consistency. During propagation, revoked delegates may still write to nodes that have not received the tombstone."
2. **Minimize the window:** BlobNotify push (Phase 79) delivers tombstones in near-real-time to connected peers. The propagation window is bounded by: (a) network latency for connected peers (sub-second), (b) safety-net interval (600s default) for disconnected peers. The window is NOT unbounded.
3. **Do NOT attempt strong consistency here.** Adding a consensus protocol for revocation would violate the project's core design (no DHT, no consensus, eventual consistency). The tombstone-based mechanism is correct for the system's consistency model.
4. **Post-revocation audit:** The owner can query all nodes (via TimeRange + namespace filter) to detect if any blobs were written by the revoked delegate after the revocation timestamp. This is a detection mechanism, not prevention.

**Warning signs:**
- Any design that claims "revocation is instant across all nodes"
- Tests that depend on fixed sleep durations for propagation (fragile)
- Proposals for a revocation consensus protocol (scope creep, violates architecture)
- Missing documentation about the propagation window

**Phase to address:**
ACL revocation phase (first phase of milestone). Document the propagation window explicitly. Test with deliberately delayed sync to verify behavior during the window.

---

### Pitfall 2: Tombstone-delegation ordering race during concurrent sync

**What goes wrong:**
Node A has: delegation blob (seq=5) and tombstone of that delegation (seq=8). Node B is syncing from Node A. If Node B receives and processes the tombstone (seq=8) before the delegation blob (seq=5) -- which CAN happen because sync uses hash-set reconciliation, not sequential delivery -- the tombstone calls `delete_blob_data()` for the delegation blob, but the delegation blob does not exist yet on Node B. The `delete_blob_data()` returns false (not found). Then the delegation blob (seq=5) arrives and is stored, populating `delegation_map`. Result: the delegation is ACTIVE on Node B even though it should be revoked.

**Why it happens:**
The current sync protocol uses XOR-fingerprint set reconciliation which identifies MISSING blobs by hash, not by sequence order. Phase C (blob transfer) sends blobs in whatever order the missing set is enumerated. Tombstones and their targets can arrive in any order. The engine handles this for regular blobs: step 3.5 checks for tombstones before storing regular blobs ("blob blocked by tombstone"). But the delegation blob is NOT a regular blob being tombstoned -- the tombstone targets the DELEGATION blob itself. When the tombstone arrives first, `delete_blob_data()` finds nothing. When the delegation blob arrives second, its tombstone is already stored in `tombstone_map`, so the "blob blocked by tombstone" check at step 3.5 catches it... but ONLY for regular blobs. A delegation blob IS a regular blob from the storage perspective.

Actually, re-reading the code more carefully: step 3.5 in `ingest_blob()` checks `has_tombstone_for()` for ALL non-tombstone blobs, including delegation blobs. So if the tombstone arrives first (populating `tombstone_map`), and then the delegation blob arrives, it will be blocked by "blob blocked by tombstone" at line 261-264. The `delegation_map` will NOT be populated because `store_blob()` is never called.

This means the current code ALREADY handles this race correctly. But only because tombstones are permanent (TTL=0) and the tombstone check happens before storage.

**The real risk is:** if someone proposes making tombstones expirable (e.g., "delegation tombstones should expire after 30 days to save space"), the race re-opens. After the tombstone expires, the delegation blob could arrive via delayed sync and be stored, re-activating the revoked delegation.

**How to avoid:**
1. **Delegation revocation tombstones MUST be permanent (TTL=0).** This is already the case (MEMORY.md: "Tombstones permanent (TTL=0)"). Never change this for delegation-targeting tombstones.
2. **Add a test for out-of-order sync:** Create a delegation blob and its tombstone, then present them to a fresh node in tombstone-first order. Verify the delegation blob is blocked.
3. **Add a code comment at the tombstone check** explaining that this check is load-bearing for delegation revocation correctness, not just data deletion.

**Warning signs:**
- Proposals to add TTL to tombstones (especially delegation tombstones)
- Proposals to garbage-collect "old" tombstones
- Missing tests for out-of-order sync of delegation + tombstone pairs
- Any change to the ingest ordering that puts tombstone check AFTER store

**Phase to address:**
ACL revocation phase. Add explicit regression test for this ordering race. Add a code comment at the tombstone check site.

---

### Pitfall 3: Key versioning without epoch boundary -- stale envelope ciphertext readable by revoked member

**What goes wrong:**
Alice creates a group with members [Bob, Charlie]. Alice encrypts data for the group (envelope with 3 recipients: Alice, Bob, Charlie). Alice removes Charlie from the group. Alice rotates the group key and encrypts NEW data for [Alice, Bob] only. But Charlie still has the old encrypted blobs from before his removal. Those blobs are permanently readable by Charlie because his KEM keypair can still decrypt the old envelope stanzas.

This is NOT a bug -- it is expected behavior when "old data stays readable with old keys" (PROJECT.md constraint). But it becomes a pitfall if the documentation or API gives users the impression that removing a member retroactively revokes read access.

**Why it happens:**
Envelope encryption is per-blob: each blob is encrypted independently with a fresh DEK. The recipient list is baked into the envelope at write time. Removing a member from a group prevents them from being included as a recipient in FUTURE encryptions, but does NOT affect already-written ciphertext. This is identical to how Signal and MLS handle member removal -- future messages use new keys, past messages remain decryptable (Signal: "removing a member induces a ratchet for future messages").

Re-encrypting old data (ciphertext update) would require reading every old blob, decrypting it, and re-encrypting without the revoked member's stanza. This is: (a) expensive (O(all_blobs) crypto operations), (b) requires write access to all blobs (some may be in other namespaces), (c) changes blob hashes (breaking content-addressed references), (d) explicitly out of scope per PROJECT.md ("Old data stays readable with old keys").

**How to avoid:**
1. **Document the read-access boundary clearly:** "Group member removal prevents inclusion in future encryptions. Previously encrypted data remains readable by the removed member."
2. **Name the SDK method precisely:** `remove_member()` not `revoke_access()`. The former implies future-only; the latter implies retroactive.
3. **Add a `key_version` field to envelope format** (even if v1 only uses one version) so that in the future, a reader can know which era a blob belongs to without decrypting it. This is a forward-compatibility hook, not immediate functionality.
4. **Consider a "re-encrypt" helper in the SDK** as a future convenience (marked as expensive/optional), but do NOT block the milestone on it.

**Warning signs:**
- SDK method names implying retroactive revocation (e.g., `revoke_read_access()`)
- Tests asserting that a removed member cannot read old data (wrong assertion)
- User-facing docs claiming "complete access revocation"
- Proposals for automatic re-encryption on member removal (scope creep)

**Phase to address:**
Key versioning phase. Document the read-access boundary. Name SDK methods correctly. Add key_version field to envelope format for forward compatibility.

---

### Pitfall 4: Group membership read-modify-write race under concurrent admin operations

**What goes wrong:**
Two admin sessions concurrently modify the same group. Admin A reads group "engineering" with members [X, Y, Z], plans to add member W. Admin B reads the same group, plans to remove member Z. Both read the same group blob. Admin A writes [X, Y, Z, W]. Admin B writes [X, Y]. Result: member W is lost because Admin B's write overwrote Admin A's.

This is a classic lost-update problem. The current `add_member()` and `remove_member()` in `_directory.py` use a read-modify-write pattern (read current group, modify member list, write new group blob) with no concurrency control.

**Why it happens:**
Groups are stored as immutable blobs with latest-timestamp-wins resolution. The `_populate_cache()` method uses `group_entry.timestamp > existing.timestamp` to pick the "latest" group blob. Two concurrent writes both read the same state and produce conflicting new states. The one with the later timestamp wins, silently discarding the other's changes.

This is a known problem in CRDT literature: "concurrent update interpretation" under eventual consistency. Without a merge function (like a CRDT G-Set for additions), concurrent modifications are not composable.

**How to avoid:**
1. **Accept single-admin operation as the normal mode.** For a pre-MVP product, this is acceptable. Document that concurrent group modifications from multiple admin sessions may conflict.
2. **Add a compare-and-swap guard:** Include the previous blob_hash in the group write. The `write_blob()` succeeds regardless (it is content-addressed), but the SDK can detect conflicts by checking if the previous blob_hash matches expected. Expose this as an optimistic concurrency check.
3. **Consider: separate add/remove blobs instead of full-state writes.** Instead of writing a complete member list, write an "add member X" or "remove member Y" operation blob. On read, replay all operations to compute current membership. This is a CRDT-like approach but significantly more complex and likely out of scope.
4. **For key rotation: the race is less dangerous.** If two admins both rotate the key, both rotations produce valid new keys. The group will use whichever key version is later. No data loss occurs.

**Warning signs:**
- No documentation about concurrent group modification behavior
- Tests only using single-admin scenarios
- SDK methods that appear atomic but are actually read-modify-write
- Group state silently going backward (members disappearing)

**Phase to address:**
Group membership revocation phase. Document the single-admin assumption. Add a warning log in the SDK when a group write's timestamp is very close to the existing group's timestamp (potential conflict indicator).

---

### Pitfall 5: Key version stored in blob data, not discoverable without decryption

**What goes wrong:**
If key_version is embedded inside the encrypted envelope payload (after AEAD encryption), a reader must attempt decryption with every possible key version to find the right one. With 10 key rotations, that is 10 KEM decapsulation + HKDF + AEAD attempts per blob read. ML-KEM-1024 decap is cheap (~0.03ms), but at scale (1000 blobs x 10 versions = 10,000 decap attempts), this adds up.

**Why it happens:**
Natural instinct is to put version metadata inside the encrypted payload ("it is secret data, encrypt everything"). But key version is not secret -- it is routing metadata that tells the reader WHICH key to use for decryption.

**How to avoid:**
1. **Put key_version in the envelope header (plaintext portion).** The current envelope format has: `[magic:4][version:1][suite:1][count:2 BE][nonce:12]`. Add `[key_version:4 BE]` to the header. This extends the fixed header from 20 to 24 bytes. Readers check `key_version` before attempting decryption, selecting the correct KEM keypair immediately. O(1) key lookup instead of O(versions).
2. **key_version is authenticated but not secret.** It is part of the AEAD additional data (AD) because it is in the header, which is used as AD during encryption. Tampering with key_version causes AEAD verification failure. This is the correct construction: authenticated metadata in the header.
3. **Backward compatibility:** Since this is pre-MVP ("no backward compat needed"), bump the envelope version from 0x01 to 0x02 and include key_version in the new format. The reader can handle both versions (0x01 = no key_version = version 0 implied).

**Warning signs:**
- key_version inside the encrypted payload
- Reader code that loops over all possible key versions trying each one
- Missing key_version in the envelope header
- key_version not included in AEAD additional data

**Phase to address:**
Key versioning phase. Design the extended envelope header FIRST, before implementing key rotation logic.

---

### Pitfall 6: Key rotation without re-registration -- directory has stale KEM pubkey

**What goes wrong:**
User Alice rotates her KEM keypair (generates new ML-KEM-1024 keypair). She starts encrypting with her new key. But other users' directory caches still have her OLD KEM pubkey. When Bob encrypts data for Alice using the cached old pubkey, Alice cannot decrypt it because she no longer has the old KEM secret key (she rotated it).

This is the mirror of the revocation problem: instead of a revoked member still having access, a legitimate member LOSES access because the sender used a stale public key.

**Why it happens:**
The directory cache (`_populate_cache()`) is invalidated by pub/sub notifications, but only for the directory namespace. If Alice re-registers (writes a new UserEntry blob with her new KEM pubkey), the directory cache IS invalidated. But if Alice does not re-register, or if re-registration fails, or if the notification is lost/delayed, other users keep using the old pubkey.

Even if Alice does re-register, there is a race window: Bob reads Alice's old pubkey from cache, Alice re-registers, Bob encrypts with old pubkey, Alice can no longer decrypt.

**How to avoid:**
1. **Make key rotation atomic with re-registration.** The SDK's `rotate_keys()` method (to be built) must: (a) generate new KEM keypair, (b) write new UserEntry blob to directory, (c) keep the old KEM secret key available for decryption of old data until a configurable grace period expires. Steps (a) and (b) must be a single SDK operation.
2. **Sender-side staleness detection:** When encrypting for a recipient, the SDK could check if the directory entry's blob timestamp is "recent enough" (e.g., within the last hour). If not, force a cache refresh before encrypting. This adds latency but prevents using very stale keys.
3. **Keep old KEM secret keys.** After rotation, the old KEM secret key must NOT be deleted. Store it alongside the new one, indexed by key_version. The identity file should support multiple KEM keypairs. `envelope_decrypt()` should try the key matching the stanza's `kem_pk_hash` first, then fall back to older keys.
4. **Add `kem_pk_hash` to DirectoryEntry.** Currently the directory entry stores the full `kem_pk`. Also store (or compute on access) the `sha3_256(kem_pk)` so the SDK can quickly check if a cached entry matches the envelope stanza without doing a full key comparison.

**Warning signs:**
- `rotate_keys()` that discards the old KEM secret key
- No mechanism to keep old KEM keys for backward decryption
- Encryption failure after rotation due to stale directory cache
- Missing re-registration step in key rotation flow

**Phase to address:**
Key versioning phase. Design the multi-key identity format before implementing rotation. The SDK identity must support N KEM keypairs (current + historical).

---

### Pitfall 7: Revocation blob format ambiguity -- is it a new blob type or reuse tombstone?

**What goes wrong:**
The milestone description says "ACL revocation via signed revocation blobs (node-enforced)." This could mean: (a) reuse existing tombstone mechanism (tombstone the delegation blob), which already works and is tested (test_acl04_revocation.sh), or (b) create a NEW "revocation blob" type with different semantics. If the implementation creates a new blob type but the existing tombstone mechanism already does the job, the result is duplicate mechanisms, confusing protocol documentation, and potential inconsistency.

**Why it happens:**
The existing delegation revocation mechanism (tombstone the delegation blob -> `delete_blob_data` erases `delegation_map` entry) works correctly and is already tested in Docker integration tests. It is natural to think "revocation needs a revocation blob" without realizing the mechanism already exists.

**How to avoid:**
1. **Audit the existing mechanism first.** The tombstone-based delegation revocation path is: owner creates tombstone targeting delegation blob hash -> tombstone replicates -> each node's engine calls `delete_blob_data()` -> `delegation_map` entry erased. This is correct, tested, and works across the network.
2. **If the existing mechanism is sufficient, document it as the revocation mechanism.** Do not create a new blob type.
3. **A new revocation blob type is only needed if:** (a) you need revocation metadata (e.g., reason, timestamp, admin identity) beyond what a tombstone provides, (b) you need revocation to be reversible (tombstones are permanent), or (c) you need different authorization rules (e.g., an admin who is not the namespace owner can revoke).
4. **Decision checkpoint:** Before writing any code, answer: "What does a revocation blob provide that a tombstone of the delegation blob does not?" If the answer is "nothing," reuse tombstones.

**Warning signs:**
- New wire type numbers allocated for revocation without clear justification
- Duplicate code paths for revocation vs. tombstone
- Inconsistent behavior between "delete delegation" and "revoke delegation"
- PROTOCOL.md describing two mechanisms for the same operation

**Phase to address:**
ACL revocation phase (first task). Make the build-vs-reuse decision before writing any code.

---

### Pitfall 8: Envelope key_version without per-namespace key management -- scope creep into KMS

**What goes wrong:**
Adding key_version to the envelope format naturally leads to questions: "Where are key versions stored? How does the sender know the current version? How does a new group member get historical keys?" Before you know it, you are building a key management service (KMS) inside the SDK -- key directories, key version registries, key distribution protocols. This is massive scope creep for a milestone that should be incremental.

**Why it happens:**
Key versioning is a "pull thread" problem. Once you start, every design question leads to another. "How does the sender know the version?" -> "Add a key version registry." -> "Where is the registry stored?" -> "As a blob in the namespace." -> "How do new members get old keys?" -> "Key escrow protocol." -> "What if the escrow blob is compromised?" -> ad infinitum.

NuCypher's KMS paper and MLS's ratchet tree both demonstrate that full key lifecycle management is a major subsystem, not a feature you bolt on.

**How to avoid:**
1. **Scope key_version to the SIMPLEST useful form.** key_version is a monotonically increasing integer per namespace. It is stored in the envelope header. The sender uses the current key version. Old versions are retained for decryption only.
2. **Key version = KEM keypair generation ordinal.** Version 0 is the original KEM keypair. Version 1 is the first rotation. The SDK identity file stores `[{version: 0, kem_sk, kem_pk}, {version: 1, kem_sk, kem_pk}]`. No separate registry needed.
3. **No key distribution protocol.** A new group member does NOT get historical keys. They can only read data encrypted after they joined (encrypted with their KEM pubkey). Old data encrypted before their membership is inaccessible to them. This is a deliberate simplification that matches the threat model.
4. **Explicitly mark as out of scope:** key escrow, proxy re-encryption, key recovery, automatic re-encryption on rotation.

**Warning signs:**
- Design discussions about "key registry blobs" or "key distribution protocols"
- Any mention of proxy re-encryption for this milestone
- Key version logic exceeding 200 lines in the SDK
- Phase plans with more than 2 phases dedicated to key versioning

**Phase to address:**
Key versioning phase. Write the scope boundary FIRST in the phase plan. "key_version is an integer in the envelope header and an array of KEM keypairs in the identity. Nothing more."

---

### Pitfall 9: Group key rotation without membership change -- unnecessary complexity

**What goes wrong:**
Implementing "rotate group key" as a separate operation from "remove member from group" leads to two code paths that must be kept in sync. If key rotation is independent of membership changes, you need: (a) a way to distribute new keys to all current members, (b) a way to signal "use key version N for new data," (c) a key version discovery protocol so readers know which version to try.

But in chromatindb's design, groups are just named member lists. Encryption happens per-blob with a fresh DEK wrapped per-recipient. There IS no persistent "group key" to rotate. Each `write_to_group()` call generates a new DEK and wraps it for current members. "Rotating the group key" is meaningless because there is no group key -- there are only per-blob DEKs.

**Why it happens:**
Mental model from traditional group encryption (like Signal's Sender Keys or MLS's group epoch key). In those systems, there is a shared group secret that is used to derive per-message keys. In chromatindb, there is no shared secret -- every blob is independently encrypted for each recipient via KEM-then-Wrap.

**How to avoid:**
1. **Recognize that group key rotation is already implicit.** When you remove a member from a group and then call `write_to_group()`, the new blob's DEK is wrapped only for current members. The removed member has no stanza. This IS key rotation -- it just happens at the blob level, not the group level.
2. **The only thing that needs "rotation" is individual KEM keypairs.** If a user suspects their KEM secret key is compromised, they rotate their KEM keypair (new ML-KEM-1024 keygen) and re-register in the directory. Future encryptions use the new pubkey.
3. **Remove "group key rotation" from the milestone scope.** Replace it with: "member removal from groups (existing `remove_member()`) + individual KEM key rotation + documentation of the per-blob encryption model."
4. **If a customer truly needs epoch-based key rotation** (e.g., compliance requirement for key rotation every 90 days), that is a future feature implemented as: periodic KEM keypair rotation by each member, which naturally transitions all future encryptions to new keys.

**Warning signs:**
- Design documents describing a "group key" or "shared group secret"
- Code that derives a DEK from a group secret rather than per-blob random DEK
- A `rotate_group_key()` method that does something other than removing/re-adding members
- Key version associated with groups rather than with individual identities

**Phase to address:**
Group membership revocation phase. Clarify in the design that "group key rotation" = "remove member + future writes exclude them." No separate rotation protocol.

---

### Pitfall 10: Delegation re-creation after revocation -- tombstone does not prevent new delegation

**What goes wrong:**
Owner revokes delegate Alice by tombstoning the delegation blob. Later, owner accidentally (or intentionally) creates a NEW delegation blob for Alice with a new blob hash. The old tombstone only targets the OLD delegation blob hash. The new delegation blob is accepted and populates `delegation_map` again. Alice is now re-authorized.

This is correct behavior if the owner intends to re-authorize Alice. But it is a pitfall if the owner expects "once revoked, always revoked" semantics.

**Why it happens:**
Tombstones are content-addressed: they target a specific blob hash. A new delegation blob for the same delegate has a different hash (different timestamp -> different canonical signing input -> different signature -> different blob hash). The tombstone for the old delegation blob does not affect the new one.

**How to avoid:**
1. **This is working as designed.** Document it: "Revoking a delegation (tombstoning the delegation blob) prevents that specific delegation from being replicated. Creating a new delegation for the same delegate is a new authorization that is independent of the previous revocation."
2. **If "permanent ban" semantics are needed:** Add a "revocation list" blob type that contains a list of permanently banned delegate pubkey hashes. The node checks this list during ingest in addition to the delegation_map. This is a separate feature from tombstone-based revocation and should be a distinct milestone item if needed.
3. **SDK guard:** The `delegate()` method in the Directory class could check if there was a recent tombstone for the same delegate (by querying the namespace for tombstones targeting delegation blobs with the delegate's pubkey). Log a warning: "Re-delegating to a previously revoked delegate."

**Warning signs:**
- Assumptions that revocation is permanent and irrevocable
- No documentation about re-delegation behavior
- Tests that verify revocation but do not test re-delegation
- "Revocation list" scope creep into the basic revocation phase

**Phase to address:**
ACL revocation phase. Document re-delegation semantics. Add a test for revoke-then-re-delegate flow.

---

## Technical Debt Patterns

| Shortcut | Immediate Benefit | Long-term Cost | When Acceptable |
|----------|-------------------|----------------|-----------------|
| Storing key_version only in the encrypted payload (not header) | Simpler format, fewer changes | O(versions) decryption attempts per blob read. Unusable at >5 key versions. | Never -- key_version in plaintext header is the correct construction |
| Single KEM keypair per identity (discard old on rotation) | Simpler identity file | Cannot decrypt any data encrypted with old pubkey. Catastrophic data loss. | Never -- old keys must be retained |
| Group "key rotation" as a separate protocol | Matches traditional group messaging mental model | Unnecessary complexity. chromatindb has no group key. Per-blob encryption already provides isolation. | Never for this system. May be needed if moving to shared-secret group encryption (no plans for this) |
| Skip key_version in envelope header for v1 | Smaller header, fewer format changes | If key rotation is added later, all existing envelopes must be assumed version 0, and the reader must trial-decrypt without knowing the version. Workable but ugly. | Acceptable ONLY if key rotation is deferred to a future milestone. Not acceptable if this milestone includes key rotation. |
| Reuse existing tombstone for delegation revocation | Zero new wire types, zero new code paths | Tombstone semantics may not cover all future revocation requirements (e.g., revocation with metadata/reason) | Acceptable for this milestone. Tombstone already works. Add a new type only when requirements demand it. |
| No concurrency control on group modifications | Simpler implementation, no locking | Lost updates on concurrent admin operations. Silent member list regression. | Acceptable for MVP with single-admin assumption documented |

## Integration Gotchas

| Integration | Common Mistake | Correct Approach |
|-------------|----------------|------------------|
| Delegation revocation + sync | Assuming tombstone propagation is synchronous. Writing tests with fixed sleep durations. | Use wait-for-condition polling (check blob count / delegation count) with timeout. Document the propagation window. |
| Key rotation + directory cache | Rotating KEM key without re-registering in directory. Other users encrypt with stale pubkey. | Make rotation + re-registration atomic in the SDK. Sender checks directory entry freshness before encrypt. |
| Envelope key_version + backward compat | Bumping envelope version without handling old format. Old SDK versions crash on new envelopes. | Pre-MVP: no backward compat needed (PROJECT.md constraint). But DO include a version check that returns MalformedEnvelopeError for unrecognized versions, not a crash. |
| Group remove_member + write_to_group | Removing a member and immediately encrypting for the group. Directory cache may still include removed member. | Call `directory.refresh()` after `remove_member()` before `write_to_group()`. Or: flush dirty flag synchronously in `remove_member()`. |
| Storage integrity scan + new indexes | Adding a new sub-database (e.g., revocation list) without updating startup integrity scan | Add new sub-database to the integrity scan in `Storage::Impl` constructor AND the scan log format. Currently scans 7 sub-databases. |
| Tombstone of delegation + quota | Tombstone is quota-exempt but the delegation blob it deletes IS counted. After revocation, namespace quota decreases. | This is correct behavior -- the delegation blob's size is subtracted from quota in `delete_blob_data()`. No action needed, but add a test to verify quota accuracy after delegation revocation. |

## Performance Traps

| Trap | Symptoms | Prevention | When It Breaks |
|------|----------|------------|----------------|
| Directory cache full rebuild on every group mutation | Every `add_member()` / `remove_member()` sets `_dirty = True`, triggering full namespace scan on next read | Implement targeted cache updates (update only the affected group entry, not full rebuild). Or: accept for MVP since groups are infrequently modified. | > 10 group mutations per minute with >100 directory entries |
| Trial decryption across all key versions | Reader loops through all key versions trying each one | Put key_version in envelope header for O(1) key selection | > 5 key versions (50+ KEM decap attempts per blob for 10 recipients x 5 versions) |
| KEM keypair generation on every rotation | ML-KEM-1024 keygen is ~0.1ms -- fast, but the identity file write is not | Cache new keypair in memory, write to disk asynchronously. But ensure fsync before advertising new pubkey. | Not a real performance issue at expected rotation frequency (monthly/quarterly) |
| Delegation count query per ingest | `has_valid_delegation()` does an MDBX read transaction per non-owner write | Already O(1) btree lookup. Not a trap -- included here to prevent "optimization" that would make it slower. | Never -- this is already optimal |

## Security Mistakes

| Mistake | Risk | Prevention |
|---------|------|------------|
| key_version in plaintext without AEAD authentication | Attacker modifies key_version field to force decryption with wrong key (which fails) or to force trial decryption (DoS) | key_version is in the envelope header which is AEAD additional data. Tampering causes authentication failure. This is already the correct construction if key_version is added to the existing header+AD pattern. |
| Keeping revoked KEM secret key accessible in memory | Compromised process dumps revoked key from memory, enabling decryption of old data | Only load old KEM keys on demand for decryption. Keep them in the encrypted identity file. Clear from memory after use. For pre-MVP, in-memory storage is acceptable with a TODO. |
| Delegation tombstone not requiring owner signature | Anyone who learns the delegation blob hash could create a tombstone revoking it | Already prevented: tombstone creation requires namespace ownership check in engine (step 2). Delegates cannot create tombstones (explicit check at line 185). |
| Group membership blob without integrity protection | Attacker modifies group membership list to add themselves | Groups are blobs in the admin's namespace, signed by admin's ML-DSA-87 key. Modification requires admin's signing secret key. Already protected by namespace ownership model. |
| Stale directory cache used for encryption decisions | Sender encrypts for old group membership after a member was removed | Add "max cache age" parameter to `write_to_group()`. Force refresh if cache is older than threshold (e.g., 60s). Document that SDK caching trades freshness for latency. |

## "Looks Done But Isn't" Checklist

- [ ] **Delegation revocation:** Tombstone of delegation blob erases `delegation_map` entry on ALL nodes (not just the node where tombstone was created) -- verify via multi-node Docker test
- [ ] **Delegation revocation:** Out-of-order sync (tombstone arrives before delegation blob) correctly blocks the delegation blob -- verify with explicit ordering test
- [ ] **Delegation revocation:** Re-delegation after revocation works (new delegation blob with new hash) -- verify positive test
- [ ] **Key versioning:** Envelope header includes key_version in AEAD additional data -- verify that modifying key_version causes DecryptionError
- [ ] **Key versioning:** Identity file supports multiple KEM keypairs -- verify that old keypair can decrypt old envelopes after rotation
- [ ] **Key versioning:** Envelope version 0x01 (no key_version) still decryptable after code change -- backward read compatibility
- [ ] **Group removal:** `remove_member()` followed by `write_to_group()` does NOT include removed member as recipient -- verify the ciphertext has N-1 stanzas
- [ ] **Group removal:** Removed member cannot decrypt data written after removal -- verify NotARecipientError
- [ ] **Group removal:** Removed member CAN still decrypt data written before removal -- verify this is documented as expected behavior
- [ ] **PROTOCOL.md:** Revocation propagation window documented with expected bounds
- [ ] **PROTOCOL.md:** Envelope format updated with key_version field
- [ ] **PROTOCOL.md:** Group membership change semantics documented (no retroactive revocation)

## Recovery Strategies

| Pitfall | Recovery Cost | Recovery Steps |
|---------|---------------|----------------|
| Revoked delegate writes during propagation window | LOW | Blobs written by revoked delegate are valid signed blobs. Owner can tombstone them individually if needed. No data corruption. |
| Tombstone-delegation ordering race (if tombstone TTL added) | HIGH | Requires manual cleanup: identify delegation blobs that should be revoked but are active. Create new tombstones. May require touching every node. Prevention is critical. |
| Stale directory cache causes encryption with old pubkey | MEDIUM | Recipient cannot decrypt. Sender must re-encrypt with correct pubkey and re-write the blob. Old blob is wasted storage but not a security risk (recipient cannot decrypt it). |
| Lost update on concurrent group modification | LOW | Re-apply the lost modification. No data corruption -- just a stale group membership list. |
| Key rotation without re-registration | HIGH | All subsequent encryptions by others use old pubkey. Must: (1) re-register with new pubkey, (2) notify all senders to refresh directory cache, (3) old encrypted data may need re-encryption. |
| Scope creep into KMS | HIGH | Stop, descope, defer to future milestone. The longer KMS-like features are developed, the harder it is to cut scope. Set a firm time-box. |

## Pitfall-to-Phase Mapping

| Pitfall | Prevention Phase | Verification |
|---------|------------------|--------------|
| Revocation propagation window (P1) | ACL revocation | PROTOCOL.md documents propagation delay bounds. Multi-node Docker test with delayed sync. |
| Tombstone-delegation ordering race (P2) | ACL revocation | Unit test: tombstone-first then delegation-blob delivery. Verify delegation blocked. Code comment at tombstone check. |
| Stale ciphertext readable by revoked member (P3) | Key versioning | SDK docs state "no retroactive read revocation." Test: removed member reads old data successfully. |
| Group read-modify-write race (P4) | Group membership revocation | Single-admin assumption documented. Warning log on close-timestamp group writes. |
| key_version in encrypted payload (P5) | Key versioning | Envelope header format review. key_version in plaintext header, included in AD. |
| Key rotation without re-registration (P6) | Key versioning | SDK `rotate_keys()` method atomically generates new KEM + writes UserEntry. Integration test for rotation flow. |
| Revocation blob type ambiguity (P7) | ACL revocation | Design decision documented BEFORE coding: reuse tombstone vs. new type. Milestone plan captures rationale. |
| Scope creep into KMS (P8) | Key versioning | Scope boundary in phase plan. key_version = integer + array of keypairs. No registry, no distribution protocol. |
| Unnecessary group key rotation protocol (P9) | Group membership revocation | Phase plan explicitly states no group key rotation protocol. Per-blob encryption model documented. |
| Re-delegation after revocation (P10) | ACL revocation | Test: revoke delegate, create new delegation for same delegate, verify new delegation works. Document semantics. |

## Sources

- Codebase analysis: `db/engine/engine.cpp` (ingest pipeline, tombstone check at step 3.5, delegation check at step 2), `db/storage/storage.cpp` (delegation_map CRUD, delete_blob_data delegation cleanup), `sdk/python/chromatindb/_envelope.py` (envelope format, KEM-then-Wrap), `sdk/python/chromatindb/_directory.py` (group read-modify-write, cache invalidation), `tests/integration/test_acl04_revocation.sh` (existing delegation revocation Docker test)
- Distributed access control: [Decentralised Access Control in CRDTs (Protocol Labs Research #8)](https://github.com/protocol/research/issues/8) -- concurrent update interpretation, convergence under revocation
- Revocation propagation: [Access Revocation Privilege Escalation (Hoop.dev)](https://hoop.dev/blog/access-revocation-privilege-escalation-a-serious-security-gap-you-cant-ignore) -- stale permissions attack window
- Eventual consistency pitfalls: [Production Bugs: Eventually Consistent Doesn't Mean Eventually Correct](https://medium.com/works-on-my-machine/production-bugs-no-one-teaches-you-2-eventually-consistent-doesnt-mean-eventually-correct-78111bc1aa14) -- permanent data incorrectness
- Tombstone ordering: [S3 eventual consistency tombstone race (HumanCellAtlas #956)](https://github.com/HumanCellAtlas/data-store/issues/956) -- tombstone vs. PUT ordering
- Key rotation: [Encryption Key Rotation is Useless (StrongSalt)](https://www.strongsalt.com/encryption-key-rotation-is-useless%E2%80%8A-%E2%80%8Aheres-why/) -- rotating KEK vs. DEK distinction
- Envelope encryption key management: [Google Cloud KMS Envelope Encryption](https://docs.cloud.google.com/kms/docs/envelope-encryption) -- key version management patterns
- Group messaging revocation: [MLS Architecture (RFC 9750)](https://www.rfc-editor.org/rfc/rfc9750.html) -- member removal provides new entropy to all except removed member
- Signal protocol: [Signal Advanced Ratcheting](https://signal.org/blog/advanced-ratcheting/) -- revocation induces ratchet for future messages
- CRDT tombstone accumulation: [CRDTs: The CRDT Dictionary (Ian Duncan)](https://www.iankduncan.com/engineering/2025-11-27-crdt-dictionary/) -- tombstone growth, metadata unbounded
- Capability revocation: [Confused Deputy Problem (BeyondTrust)](https://www.beyondtrust.com/blog/entry/confused-deputy-problem) -- stale capability reuse
- Revocable storage ABE: Sahai, Seyalioglu, Waters (Crypto 2012) -- revocation of past ciphertexts requires ciphertext-update, not just key revocation

---
*Pitfalls research for: chromatindb v2.1.1 -- ACL revocation, key versioning, group membership revocation*
*Researched: 2026-04-05*
