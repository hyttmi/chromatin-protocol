# Architecture: ACL Revocation, Key Versioning & Group Key Rotation

**Domain:** Access control lifecycle for blob-based encrypted storage
**Researched:** 2026-04-05
**Confidence:** HIGH (based on full codebase analysis + protocol research)

## Executive Summary

This document defines how ACL revocation, envelope key versioning, and group membership revocation integrate with chromatindb's existing blob-based architecture. The core insight: **the existing primitives (delegation blobs, tombstones, per-blob envelope encryption, group membership blobs) already provide 90% of the machinery.** The remaining work is (1) exposing delegation revocation in the SDK, (2) adding a key_version tag to envelopes so readers can locate the right KEM keypair after rotation, and (3) giving the admin a "remove member + re-encrypt for remaining members" workflow in groups. No new node-side wire types are needed. No new MDBX sub-databases. No new dependencies.

The critical architectural constraint: **old data stays readable with old keys.** This is explicitly stated in the milestone constraints. There is no re-encryption of existing blobs. When a member is revoked from a group or a KEM keypair is rotated, only new blobs are encrypted with the new key set / new KEM key. The old blobs remain decryptable by anyone who held the key at time of encryption.

## Existing Architecture (Baseline)

### What Already Works

**ACL revocation at node level:**
- Delegation blob: `[0xDE 0x1E 0x6A 0x7E][delegate_pubkey:2592]` stored in owner namespace
- Tombstoning the delegation blob removes the delegate from `delegation_map` (MDBX index)
- Engine ingest checks `has_valid_delegation()` on every delegate write -- O(1) btree lookup
- Re-delegation after tombstone creates a new delegation blob (new hash, not blocked by old tombstone)
- Tests already cover: revocation blocks writes, re-delegation works, delegate-written blobs survive revocation, independent delegate revocation
- Sync replicates tombstones to all nodes, so revocation propagates network-wide

**Envelope encryption at SDK level:**
- Per-blob random DEK, wrapped per-recipient via ML-KEM-1024 encapsulation
- Envelope format: `[CENV][version:1][suite:1][count:2 BE][nonce:12][N x stanza:1648][ciphertext+tag]`
- Stanzas sorted by `SHA3-256(kem_pk)` for O(log N) binary search on decrypt
- Sender auto-included as recipient (cannot lock yourself out)
- Suite 0x01 (plain) and 0x02 (Brotli-compressed) supported

**Groups at SDK level:**
- GroupEntry blob: `[GRPE][version:1][name_len:2 BE][name][member_count:2 BE][N x member_hash:32]`
- Members identified by `SHA3-256(signing_pk)` hashes
- `write_to_group()`: resolves members from directory cache, calls `write_encrypted()` with resolved identities
- Latest-timestamp-wins for group blob resolution (multiple versions coexist, newest wins)
- `remove_member()`: read-modify-write pattern, writes new GRPE blob with member removed

**Directory at SDK level:**
- Admin-owned namespace, delegation-based write access
- UserEntry blobs with signing_pk + kem_pk + display_name + kem_sig
- Cache populated lazily, invalidated via pub/sub notifications

### What's Missing

| Gap | Layer | Impact |
|-----|-------|--------|
| No `revoke_delegation()` in SDK | SDK | Admin cannot revoke write access programmatically |
| No KEM key rotation support | SDK | Compromised KEM key = all future data readable by attacker |
| No key_version in envelopes | SDK | After KEM rotation, decrypt doesn't know which keypair to try |
| No group revocation workflow | SDK | Removing a member doesn't prevent them from reading new data encrypted to old group |
| No `list_delegations()` convenience in Directory | SDK | Admin cannot see who has write access |

## Recommended Architecture

### Component 1: SDK Delegation Revocation

**What changes:** Add `revoke_delegation()` to `Directory` class.

**Data flow:**
```
Admin calls directory.revoke_delegation(delegate_identity)
  -> directory lists delegations for namespace (DelegationListRequest type 51)
  -> finds delegation blob_hash for this delegate's pk_hash
  -> calls client.delete_blob(delegation_blob_hash) (tombstone)
  -> delegation_map entry removed on all nodes via tombstone replication
  -> delegate's subsequent writes rejected with IngestError::no_delegation
```

**Integration points:**
- `Directory.revoke_delegation(delegate_identity: Identity) -> DeleteResult` -- new method
- Uses existing `client.delete_blob()` (tombstone) -- no node changes
- Uses existing `DelegationListRequest/Response` (types 51/52) to find the blob_hash
- Node enforcement already works (tombstone removes delegation_map entry)

**No node changes required.** The node already:
1. Removes delegation_map entry when tombstone stored (storage.cpp line ~877)
2. Checks delegation_map on every delegate write (engine.cpp line ~169)
3. Replicates tombstones via sync

**New SDK method signature:**
```python
async def revoke_delegation(self, delegate_identity: Identity) -> DeleteResult:
    """Revoke write access from a delegate by tombstoning their delegation blob.

    Args:
        delegate_identity: Identity of the delegate to revoke.

    Returns:
        DeleteResult with tombstone hash and seq_num.

    Raises:
        DirectoryError: If not admin, or delegation not found.
    """
```

### Component 2: KEM Key Versioning in Envelopes

**Problem:** When a user rotates their KEM keypair (generates new ML-KEM-1024 keys), the envelope decrypt path does binary search by `SHA3-256(kem_pk)`. If the user's current KEM key is version N, they need to try previous keys for older envelopes. Without any versioning signal, this requires O(versions) decrypt attempts.

**Design decision: key_version in UserEntry, NOT in envelope header.**

Rationale: The envelope header already has a `pk_hash` per stanza. A rotated user publishes a new UserEntry (version N+1) to the directory with their new KEM pubkey. Old UserEntry blobs remain in storage. The SDK already caches directory entries. Key resolution happens at encrypt time (directory lookup) and at decrypt time (identity selection).

**Architecture:**

1. **UserEntry v2 format:** Add `key_version: uint16 BE` after the version byte:
   ```
   [UENT][version:0x02][key_version:2 BE][signing_pk:2592][kem_pk:1568][name_len:2 BE][name][kem_sig]
   ```
   - `version` = 0x02 (UserEntry format version, not the envelope version)
   - `key_version` = monotonically increasing per-user KEM generation (starts at 1)

2. **Identity rotation workflow:**
   ```
   User generates new KEM keypair
     -> Publishes new UserEntry v2 with key_version=N+1
     -> Old UserEntry remains (not tombstoned -- needed for old envelope decrypt)
     -> Directory cache indexes by pubkey_hash: latest entry wins for encrypt
     -> Identity keeps old KEM keys in a key_ring for decrypt
   ```

3. **SDK Identity key ring:**
   ```python
   class Identity:
       # Existing
       _signing_key: SigningKey
       _kem: KeyEncapsulation | None

       # New: historical KEM keys for decryption
       _kem_ring: dict[bytes, KeyEncapsulation]  # pk_hash -> KEM
   ```

4. **Modified decrypt path:**
   ```
   envelope_decrypt(data, identity)
     -> parse pk_hashes from stanzas
     -> try identity.current KEM key (binary search by pk_hash)
     -> if NotARecipientError and identity has key_ring:
        -> check each historical KEM key hash against stanza list
     -> return first successful decryption
   ```

5. **Envelope format: UNCHANGED.** The stanza `pk_hash` already identifies which KEM key was used. No envelope header changes needed. The `pk_hash` IS the key version identifier.

**Integration points:**
- `Identity.rotate_kem() -> Identity` -- generates new KEM keypair, moves old to ring
- `Identity.save()` / `Identity.load()` -- must persist key ring
- `Directory._populate_cache()` -- must handle UserEntry v2 (backward compatible: v1 = key_version 0)
- `envelope_decrypt()` -- try key ring on NotARecipientError (O(key_versions) worst case, typically 1-2)

**Why NOT put key_version in envelope header:**
- The stanza pk_hash already uniquely identifies the KEM key used
- Adding header fields breaks the envelope format (version bump, compatibility)
- The decrypter always knows which key matched (by pk_hash), no extra field needed
- YAGNI: key rotation is infrequent, O(ring_size) search over 1-3 keys is negligible

### Component 3: Group Membership Revocation

**Problem:** When a member is removed from a group, they can still decrypt any envelope that was encrypted to the old member list (where they were included). For new data, the admin removes the member from the group blob, and future `write_to_group()` calls will not include the removed member.

**Current state already handles this partially:**
- `remove_member()` writes new GRPE blob without the removed member
- `write_to_group()` resolves current members at call time
- Future writes exclude the removed member

**What's actually missing:** Forward secrecy for group data. After a member is removed, new data encrypted with `write_to_group()` goes to the remaining members -- the removed member is NOT a recipient. This already works because `write_to_group()` resolves the current member list, not a historical one.

**The real gap is notification, not cryptography:**
1. Other SDK clients don't know a member was removed unless they re-check the group
2. The admin removing a member should optionally notify remaining members
3. Applications may want to know "this blob was encrypted with group version X"

**Architecture:**

1. **GroupEntry v2 format:** Add `version_counter: uint32 BE` for group epoch tracking:
   ```
   [GRPE][version:0x02][version_counter:4 BE][name_len:2 BE][name][member_count:2 BE][N x member_hash:32]
   ```
   - `version_counter` increments on every membership change
   - Useful for applications to detect stale group state

2. **Revoke-and-rotate workflow:**
   ```
   Admin calls directory.remove_member(group_name, member)
     -> current: writes new GRPE blob without member (already implemented)
     -> new: increments version_counter in new GRPE blob
     -> no key rotation needed: each blob has its own DEK, wrapped per-recipient

   Remaining members:
     -> directory cache invalidated via pub/sub notification
     -> next write_to_group() resolves updated member list
     -> removed member not included as recipient
   ```

3. **No re-encryption of existing data.** This is by design:
   - Each blob has its own random DEK
   - The DEK is wrapped per-recipient via ML-KEM
   - Removing a member from future recipient lists is sufficient
   - Old data remains accessible to old members (stated constraint)

4. **Group key concept: explicitly NOT needed.** The "groups as blobs" design (v1.7.0 decision) means there is no shared group key to rotate. Each blob is independently encrypted to the current member set. Removing a member from the group blob is sufficient to exclude them from future data. This is simpler than MLS/Signal group key ratcheting and appropriate for a storage system (not a messaging system).

**Integration points:**
- `GroupEntry` dataclass: add `version_counter: int` field
- `encode_group_entry()` / `decode_group_entry()`: handle v2 format (backward compat: v1 = counter 0)
- `Directory.remove_member()`: increment version_counter
- `Directory.add_member()`: increment version_counter
- No node changes, no envelope changes, no new wire types

### Component 4: Directory Delegation Awareness

**What changes:** Surface delegation status in the Directory class for admin tooling.

**New methods:**
```python
async def list_delegates(self) -> list[DelegateInfo]:
    """List all delegates for the directory namespace."""

async def is_delegated(self, identity: Identity) -> bool:
    """Check if an identity has write delegation."""
```

**Uses existing:** `DelegationListRequest/Response` (types 51/52) already on the wire.

## Component Boundaries

| Component | Layer | Modifies | New Files | Depends On |
|-----------|-------|----------|-----------|------------|
| Delegation revocation | SDK | `_directory.py`, `client.py` | None | Existing tombstone + delegation_list |
| KEM key versioning | SDK | `identity.py`, `_envelope.py`, `_directory.py` | None | UserEntry v2 format |
| Group revocation | SDK | `_directory.py` | None | GroupEntry v2 format |
| Directory delegation | SDK | `_directory.py` | None | Existing DelegationList wire type |
| PROTOCOL.md update | Docs | `PROTOCOL.md` | None | All above |

**No C++ node changes.** All features are SDK-level. The node already enforces delegation revocation via tombstones. The node does not understand or validate envelope encryption (by design -- zero-knowledge storage). Groups are plain blobs to the node.

## Data Flow Changes

### Delegation Revocation Flow (NEW)

```
Admin SDK                  Relay              Node
   |                         |                  |
   |-- DelegationListReq --->|-- (forward) ---->|
   |<-- DelegationListResp --|<-- (forward) ----|
   |                         |                  |
   |  (find matching delegation blob_hash)      |
   |                         |                  |
   |-- Delete (tombstone) -->|-- (forward) ---->|
   |                         |                  | storage: remove delegation_map entry
   |                         |                  | storage: store tombstone blob
   |<-- DeleteAck -----------|<-- (forward) ----|
   |                         |                  |
   | (tombstone replicates to all nodes via sync)
```

### KEM Rotation Flow (NEW)

```
User SDK (rotating)         Relay              Node
   |                         |                  |
   | identity.rotate_kem()   |                  |
   |  -> new ML-KEM-1024     |                  |
   |  -> old key in ring     |                  |
   |                         |                  |
   | directory.register(name)|                  |
   |-- Data (UserEntry v2) ->|-- (forward) ---->|
   |<-- WriteAck ------------|<-- (forward) ----|
   |                         |                  |
   | (new UserEntry in directory namespace)
   | (old UserEntry still in storage -- not deleted)
   | (other users see new KEM key on next cache refresh)

User SDK (decrypting old envelope)
   |
   | envelope_decrypt(data, identity)
   |  -> stanza pk_hash matches OLD kem key
   |  -> identity.key_ring[old_pk_hash] used
   |  -> decryption succeeds
```

### Group Revocation Flow (EXISTING + MINOR CHANGE)

```
Admin SDK                  Relay              Node
   |                         |                  |
   | directory.remove_member(group, member)      |
   |  -> read current GRPE blob                  |
   |  -> filter out member hash                  |
   |  -> increment version_counter               |
   |  -> encode new GRPE v2 blob                 |
   |                         |                  |
   |-- Data (GRPE v2) ----->|-- (forward) ---->|
   |<-- WriteAck ------------|<-- (forward) ----|
   |                         |                  |
   | (pub/sub notification -> other SDK caches invalidated)
   |                         |                  |
   | Other SDK: write_to_group(data, group, dir, ttl)
   |  -> resolves CURRENT members (removed member excluded)
   |  -> envelope_encrypt with remaining members only
   |  -> removed member CANNOT decrypt new blobs
```

## Anti-Patterns to Avoid

### Anti-Pattern 1: Shared Group Key
**What:** A single symmetric key shared by all group members, rotated on membership change.
**Why bad:** Requires key distribution channel, re-encryption of in-flight data, complex ratcheting (MLS-style). The chromatindb architecture already solves this with per-blob DEK wrapping.
**Instead:** Per-blob DEK + per-recipient KEM wrapping. "Groups as blobs" (v1.7.0 decision) is correct.

### Anti-Pattern 2: Re-encrypting Old Data on Revocation
**What:** When a member is revoked, re-encrypt all existing group blobs without that member's stanza.
**Why bad:** O(blobs) re-encryption, requires reading+decrypting+re-encrypting every blob, generates new blob hashes (breaks references), network amplification.
**Instead:** Accept that old data stays readable with old keys (stated constraint). Only new data excludes revoked members.

### Anti-Pattern 3: Tombstoning Old UserEntry on KEM Rotation
**What:** Delete the old UserEntry when publishing a new one.
**Why bad:** Old envelopes reference the old KEM pk_hash. If the old UserEntry is gone, other clients cannot discover the old public key for verification. The user's own key_ring handles decryption.
**Instead:** Keep old UserEntries. Directory cache uses latest-timestamp-wins for the primary index (used by encryptors). The old entries remain for historical reference.

### Anti-Pattern 4: Node-Side Envelope Validation
**What:** Having the node parse or validate envelope encryption contents.
**Why bad:** Violates zero-knowledge storage principle. Node stores opaque signed blobs. Envelope encryption is SDK-level.
**Instead:** All envelope logic stays in SDK. Node is intentionally dumb.

### Anti-Pattern 5: Delegation Revocation via Config (SIGHUP)
**What:** Revoking delegation by modifying node config instead of tombstoning.
**Why bad:** Config-based ACL is per-node, not replicated. Delegation blobs replicate via sync. Tombstone-based revocation propagates to all nodes automatically.
**Instead:** Always use tombstone-based revocation for delegation. Config-based `allowed_keys` is for node access control (who can connect), not namespace delegation.

## Backward Compatibility

**Pre-production constraint:** No backward compatibility needed. However, the design is naturally backward-compatible:

| Format | v1 (existing) | v2 (new) | Compatibility |
|--------|---------------|----------|---------------|
| UserEntry | `version=0x01`, no key_version | `version=0x02`, key_version field | v2 decoder handles v1 (key_version=0). v1 decoder rejects v2 (version mismatch). |
| GroupEntry | `version=0x01`, no version_counter | `version=0x02`, version_counter field | v2 decoder handles v1 (counter=0). v1 decoder rejects v2 (version mismatch). |
| Envelope | Unchanged | Unchanged | No changes to envelope format |
| Delegation | Unchanged | Unchanged | No changes to delegation blob format |
| Wire types | Unchanged | Unchanged | No new message types needed |

## Suggested Build Order

Based on dependency analysis:

### Phase 1: Delegation Revocation (SDK)
**Rationale:** Smallest scope, uses only existing primitives, unblocks admin workflow. Zero risk.
- Add `Directory.revoke_delegation(delegate_identity)` method
- Add `Directory.list_delegates()` convenience method
- Tests: revoke blocks future delegate writes, revoke + re-delegate works
- Dependencies: None (uses existing DelegationList + delete_blob)

### Phase 2: KEM Key Versioning (SDK)
**Rationale:** Most complex component, but independent of Phase 1. Requires UserEntry v2, Identity key ring, and modified decrypt path.
- UserEntry v2 format with key_version field
- `Identity.rotate_kem()` method + key ring storage
- `Identity.save()` / `Identity.load()` persistence of key ring
- Modified `envelope_decrypt()` to try key ring on miss
- Directory cache: handle UserEntry v2 (backward compat with v1)
- Tests: rotate + encrypt to new key, decrypt old envelope with old key, key ring persistence
- Dependencies: None (independent of Phase 1)

### Phase 3: Group Membership Revocation (SDK)
**Rationale:** Builds on Phase 2 (uses key versioning concepts). GroupEntry v2 format.
- GroupEntry v2 format with version_counter field
- `remove_member()` increments version_counter
- `add_member()` increments version_counter
- Tests: removed member cannot decrypt new group data, existing data still readable
- Dependencies: Phase 2 (key rotation concepts validated)

### Phase 4: Documentation & Protocol Update
**Rationale:** Always last. Documents what was actually built.
- PROTOCOL.md: UserEntry v2 format, GroupEntry v2 format, revocation workflow
- SDK README: key rotation guide, group revocation guide, delegation management
- Dependencies: Phases 1-3 complete

**Phase ordering rationale:**
- Phase 1 first: simplest, highest immediate value (admin can revoke delegates)
- Phase 2 second: enables key lifecycle (rotation, which groups depend on conceptually)
- Phase 3 third: group revocation is straightforward once key versioning is understood
- Phase 4 last: document actual behavior, not planned behavior

## Scalability Considerations

| Concern | At 10 users | At 1K users | At 10K users |
|---------|-------------|-------------|--------------|
| Key ring size | 1-2 keys per user | 1-2 keys per user | 1-2 keys per user |
| Group blob size | ~330 bytes | ~32K bytes | ~320K bytes |
| Envelope stanza count | 10 stanzas (16.5 KB) | Approach 256 cap (422 KB) | Exceeds 256 cap -- subgroups needed |
| Directory scan | ~50 blobs | ~5K blobs | ~50K blobs -- pagination critical |
| Delegation list | ~10 entries | ~1K entries | ~10K entries -- pagination needed |

**10K user group is the architectural limit** (256 recipient cap in envelope format). For larger groups, applications would need to shard into subgroups or use a broadcast encryption scheme. This is acceptable for the stated "enterprise storage vault" product direction.

## Sources

- Codebase analysis: `db/engine/engine.cpp`, `db/storage/storage.cpp`, `db/wire/codec.h`
- SDK analysis: `sdk/python/chromatindb/_envelope.py`, `sdk/python/chromatindb/_directory.py`, `sdk/python/chromatindb/client.py`
- Protocol spec: `db/PROTOCOL.md` (delegation, envelope, tombstone sections)
- Test coverage: `db/tests/engine/test_engine.cpp` (delegation revocation tests)
- [MLS Architecture (RFC 9750)](https://www.rfc-editor.org/rfc/rfc9750.html) -- group key rotation patterns (referenced for anti-pattern analysis)
- [Signal Protocol Documentation](https://signal.org/docs/) -- forward secrecy patterns (referenced for comparison)
