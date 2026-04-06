# Feature Research: Revocation & Key Lifecycle

**Domain:** ACL revocation, encryption key versioning, group membership revocation in encrypted decentralized storage
**Researched:** 2026-04-06
**Confidence:** HIGH (existing primitives well-understood, patterns well-established in industry)

## Context: What Already Exists

Before mapping features, the existing primitives that this milestone builds on:

| Primitive | Status | Where |
|-----------|--------|-------|
| Delegation blobs (signed, namespace-scoped) | SHIPPED v3.0 | `db/wire/codec.h`, `db/engine/engine.cpp` |
| Tombstones (owner-only, replicated) | SHIPPED v3.0 | `db/wire/codec.h`, `db/storage/storage.cpp` |
| Delegation index (O(1) lookup via `delegation_map`) | SHIPPED v3.0 | `db/storage/storage.cpp:911` |
| Delegation revocation via tombstone | SHIPPED v3.0 | `storage.cpp:876-886` -- tombstoning delegation blob erases `delegation_map` entry |
| Envelope encryption (ML-KEM-1024 + ChaCha20-Poly1305) | SHIPPED v1.7.0 | `sdk/python/chromatindb/_envelope.py` |
| Directory (UserEntry, pubkey discovery, delegation) | SHIPPED v1.7.0 | `sdk/python/chromatindb/_directory.py` |
| Groups (named member lists as blobs, latest-timestamp-wins) | SHIPPED v1.7.0 | `sdk/python/chromatindb/_directory.py` |
| `write_encrypted` / `read_encrypted` / `write_to_group` | SHIPPED v1.7.0 | `sdk/python/chromatindb/client.py` |
| `has_valid_delegation()` check on ingest hot path | SHIPPED v3.0 | `db/engine/engine.cpp:169` |
| SIGHUP reload of `allowed_keys` + disconnect revoked peers | SHIPPED v2.0 | `db/peer/peer_manager.cpp:2884` |

**Key insight:** Delegation revocation at the node layer already works. Tombstoning a delegation blob removes the `delegation_map` entry, and subsequent writes from the revoked delegate are rejected. What is missing is: (1) SDK-level awareness of revocation, (2) encryption key versioning so revoked members cannot decrypt new data, and (3) group membership revocation that ties these two together.

## Feature Landscape

### Table Stakes (Users Expect These)

Features that anyone building revocation into an encrypted storage system would expect.

| Feature | Why Expected | Complexity | Notes |
|---------|--------------|------------|-------|
| **SDK delegation revocation helper** | Node already enforces revocation via tombstone, but SDK has no `revoke_delegate()` method. Admin must manually construct tombstone data and call `write_blob`. Incomplete API surface. | LOW | Admin calls `directory.revoke_delegate(identity)`. Constructs tombstone targeting the delegation blob hash. Requires knowing the delegation blob hash -- either from cache or from `DelegationList` query. |
| **Revocation confirmation** | After revoking, admin needs to verify the revocation took effect (delegation erased from index). Without confirmation, there is no way to distinguish "revocation in flight" from "revocation failed". | LOW | `has_valid_delegation` already exists in storage. SDK needs a way to check -- either via `DelegationList` query (already exists in v1.4.0) or a new `DelegationExists` query. Reusing `DelegationList` is sufficient. |
| **Encryption key versioning (key epochs)** | After revoking a member, new data must be encrypted with a key the revoked member does not possess. Without key epochs, the revoked member can still decrypt any new data encrypted to the same group. This is the core security property of revocation. | MEDIUM | Per-namespace key versioning. New epoch = new KEM keypair published to directory. Old epochs remain readable with old keys. AWS KMS, Azure Key Vault, and all serious envelope encryption systems use this pattern. |
| **Group membership revocation with key rotation** | Removing a member from a group list (already possible via `Directory.remove_member`) is insufficient without rotating the group encryption context. The removed member still holds the old group members' KEM pubkeys and can decrypt any envelope addressed to them. Must tie member removal to key epoch bump. | MEDIUM | Signal clears all Sender Keys on member removal. MLS re-derives epoch secrets. For chromatindb's per-blob envelope model: remove member from group, bump admin KEM epoch, re-publish directory entry. New `write_to_group` uses new epoch keys. |
| **Old data remains readable** | PROJECT.md constraint: "Old data stays readable with old keys (no re-encryption)." Every key versioning system (AWS KMS, GCP KMS, Azure) retains old key versions for decryption. Re-encrypting all existing data is prohibitively expensive in a decentralized system. | LOW | `read_encrypted` must try current key first, then fall back to previous versions. Envelope format already includes `kem_pk_hash` per recipient -- identity with old KEM keypair can still decrypt old envelopes. |
| **Admin KEM key rotation** | Admin must be able to rotate their own KEM keypair (publish new UserEntry with new KEM pubkey). This is the mechanism that makes key versioning work -- after rotation, new envelopes use the new KEM key. | LOW | Write new UserEntry to directory with new KEM keypair. Old UserEntry still exists (immutable blobs). Directory cache uses latest-timestamp-wins (already implemented for groups). Need same pattern for UserEntry. |

### Differentiators (Competitive Advantage)

Features that go beyond table stakes and would distinguish chromatindb from simpler encrypted storage systems.

| Feature | Value Proposition | Complexity | Notes |
|---------|-------------------|------------|-------|
| **Atomic revoke-and-rotate** | Single admin operation that: (1) tombstones delegation, (2) removes member from group, (3) rotates admin KEM key, (4) re-publishes group. Prevents the window where revocation happened but key has not rotated. | MEDIUM | SDK-level orchestration method. `directory.revoke_member(group_name, member)` does all four steps. No new wire types needed. |
| **Revocation propagation notification** | Subscribers to the directory namespace get notified when a revocation happens, allowing connected clients to refresh their directory cache and pick up new KEM keys immediately. | LOW | Already works via existing Subscribe/Notification for directory namespace. `_check_invalidation()` already drains notifications and sets `_dirty`. Zero new code -- just documentation that this is the expected pattern. |
| **Key history chain** | Directory entries form a discoverable history: UserEntry v1, v2, v3... all in the same namespace. `read_encrypted` can walk the chain to find the right decryption key for any epoch. | MEDIUM | Content-addressed dedup means each UserEntry version is a unique blob (different KEM keypair = different data = different hash). Need an index or convention for walking history. Could use a `key_version` field in UserEntry or rely on timestamp ordering. |
| **Delegation audit trail** | Tombstoned delegation blobs still exist as tombstones. An admin can list all past delegations (active and revoked) for compliance. | LOW | Tombstones are permanent (TTL=0). The delegation blob hash is in the tombstone data. `DelegationList` shows active; tombstone scan shows revoked. SDK helper for audit view. |

### Anti-Features (Commonly Requested, Often Problematic)

| Feature | Why Requested | Why Problematic | Alternative |
|---------|---------------|-----------------|-------------|
| **Re-encryption of existing data on revocation** | "Revoked member can still read old data" -- sounds like a security gap. | Catastrophically expensive in a decentralized replicated system. Every blob encrypted to the group must be fetched, decrypted, re-encrypted with new key, and re-published. Violates PROJECT.md constraint. Creates huge write amplification. AWS/GCP/Azure all avoid this -- they keep old key versions. | Accept that old data is readable with old keys. This is the universal industry pattern. Document it clearly. Focus security guarantee on: "revoked members cannot read NEW data." |
| **Distributed revocation consensus** | "All nodes must agree on revocation before it takes effect." | chromatindb is eventually consistent by design. Requiring consensus would need a new protocol layer (Raft/PBFT), violating YAGNI and the "intentionally dumb database" principle. | Revocation propagates via normal blob replication (tombstone sync). Eventually consistent. Acceptable latency for revocation -- measured in seconds, not minutes. |
| **Automatic key rotation on timer** | "Rotate keys every N days automatically." | Adds timer complexity, requires background key management, and the rotation itself has no value without a revocation event. Rotating keys without revoking anyone is security theater in this context. | Rotate keys explicitly when a revocation happens. If compliance demands periodic rotation, add as a future SDK convenience -- but it is not table stakes. |
| **Proxy re-encryption** | Academic papers propose allowing a proxy to transform ciphertexts from one key to another without seeing plaintext. | Requires new cryptographic primitives (PRE schemes), no PQ-secure PRE scheme is standardized or in liboqs, and violates "no new dependencies." Adds massive complexity for marginal benefit when envelope encryption already solves the problem. | Per-blob envelope encryption with key versioning. New data uses new keys. Old data stays under old keys. |
| **Revocation CRL/OCSP-style checking** | "Check if a key is revoked before every decrypt." | Adds network dependency to every `read_encrypted` call. Breaks offline decryption. Adds latency. chromatindb blobs are self-verifiable -- the envelope either decrypts or it does not. | If the recipient has the KEM secret key, they can decrypt. Revocation prevents future writes and future encryption-to-group, not past decryption. This is the correct security model. |
| **Node-enforced encryption** | "Nodes should refuse to store unencrypted blobs." | Violates "intentionally dumb database" principle. Nodes cannot inspect encrypted content. Some namespaces may intentionally store plaintext (public data). | Encryption is an SDK-layer concern. Nodes store signed blobs. Period. |

## Feature Dependencies

```
[SDK delegation revocation helper]
    |
    +--requires--> [DelegationList query] (ALREADY EXISTS v1.4.0 Phase 66)
    |
    +--requires--> [Tombstone construction] (ALREADY EXISTS v3.0)

[Encryption key versioning]
    |
    +--requires--> [Admin KEM key rotation]
    |                  |
    |                  +--requires--> [UserEntry with versioning support]
    |                  |
    |                  +--requires--> [Directory latest-entry-wins for UserEntry] (partial -- exists for groups)
    |
    +--requires--> [Multi-version KEM key storage in Identity/Directory]
    |
    +--enhances--> [read_encrypted fallback to old keys]

[Group membership revocation with key rotation]
    |
    +--requires--> [SDK delegation revocation helper]
    |
    +--requires--> [Encryption key versioning]
    |
    +--requires--> [Directory.remove_member] (ALREADY EXISTS v1.7.0)
    |
    +--enhances--> [Atomic revoke-and-rotate]

[Atomic revoke-and-rotate]
    |
    +--requires--> [SDK delegation revocation helper]
    +--requires--> [Encryption key versioning]
    +--requires--> [Group membership revocation]

[Key history chain]
    |
    +--requires--> [Encryption key versioning]
    +--enhances--> [read_encrypted fallback to old keys]
```

### Dependency Notes

- **Key versioning requires admin KEM rotation:** Without the ability to publish a new KEM keypair, there is no "new epoch" to encrypt to.
- **Group revocation requires key versioning:** Removing a member from the group blob is pointless without also rotating the encryption key -- the removed member still has the old keys.
- **Atomic revoke-and-rotate requires all three:** It is the composition of delegation revocation + group member removal + key rotation into one SDK call.
- **Key history chain enhances read_encrypted:** Without history, old data encrypted to old KEM keys becomes unreadable when the directory cache only has the latest UserEntry.

## MVP Definition

### Launch With (v2.1.1)

Minimum set to close the revocation story.

- [ ] **SDK delegation revocation helper** -- `directory.revoke_delegate(identity)` that finds delegation blob hash and tombstones it. LOW complexity, HIGH value.
- [ ] **Admin KEM key rotation** -- `identity.rotate_kem()` generates new KEM keypair; `directory.register()` with new identity publishes updated UserEntry. Need UserEntry latest-timestamp-wins in directory cache (already done for groups).
- [ ] **Encryption key versioning** -- Add `key_version` field to UserEntry format (USERENTRY_VERSION bump to 0x02). Directory tracks key history per user. `write_encrypted` uses latest KEM key. `read_encrypted` tries all known KEM keys for recipient identity.
- [ ] **Group membership revocation + key rotation** -- `directory.revoke_member(group_name, member)` that removes member from group, tombstones their delegation, and rotates admin KEM key in one call.
- [ ] **Documentation** -- PROTOCOL.md update for UserEntry v2 format, key versioning semantics, revocation flow.

### Add After Validation (v2.1.x)

Features to add once core revocation is working.

- [ ] **Delegation audit trail** -- SDK helper to list active + revoked delegations with timestamps. Trigger: compliance use case feedback.
- [ ] **Key rotation audit log** -- Track all KEM key rotations per namespace with timestamps. Trigger: enterprise audit requirements.
- [ ] **Revocation event callbacks** -- SDK `on_revocation` callback when directory cache detects a delegation tombstone. Trigger: real-time notification use case.

### Future Consideration (v3+)

Features to defer until product-market fit is established.

- [ ] **Periodic automatic key rotation** -- Timer-based rotation for compliance. Why defer: no value without active revocation; adds complexity.
- [ ] **Re-encryption utility** -- Batch re-encrypt old data under new keys. Why defer: expensive, violates constraints, only needed for extreme compliance scenarios.
- [ ] **Cross-SDK key versioning** -- C/C++/Rust/JS SDKs all support key epochs. Why defer: Python SDK only currently exists.

## Feature Prioritization Matrix

| Feature | User Value | Implementation Cost | Priority |
|---------|------------|---------------------|----------|
| SDK delegation revocation helper | HIGH | LOW | P1 |
| Admin KEM key rotation | HIGH | LOW | P1 |
| Encryption key versioning (UserEntry v2) | HIGH | MEDIUM | P1 |
| Group membership revocation + key rotation | HIGH | MEDIUM | P1 |
| Old data readable with old keys | HIGH | LOW | P1 |
| Documentation (PROTOCOL.md, SDK docs) | HIGH | LOW | P1 |
| Atomic revoke-and-rotate | MEDIUM | MEDIUM | P2 |
| Revocation propagation notification | MEDIUM | LOW (already works) | P2 |
| Key history chain | MEDIUM | MEDIUM | P2 |
| Delegation audit trail | LOW | LOW | P3 |
| Revocation event callbacks | LOW | MEDIUM | P3 |

**Priority key:**
- P1: Must have for this milestone (closes the revocation story)
- P2: Should have, makes the feature production-quality
- P3: Nice to have, future consideration

## Competitor/Prior Art Analysis

| Feature | Signal/WhatsApp | MLS (RFC 9420) | AWS KMS | chromatindb Approach |
|---------|-----------------|----------------|---------|---------------------|
| Member removal | Clear all Sender Keys, re-derive | Commit removes member, new epoch_secret | IAM policy revocation | Tombstone delegation blob, remove from group blob |
| Key rotation on removal | All members re-derive sender keys | New tree secret, re-derive | Create new key version (automatic) | Rotate admin KEM keypair, re-publish UserEntry |
| Old data access | Members keep old message keys | Old epoch keys retained | Old key versions kept for decrypt | Old UserEntry blobs remain, old KEM keys in directory history |
| Re-encryption | Never | Never (old epochs frozen) | Manual (download + re-upload) | Never (constraint) |
| Forward secrecy | Double Ratchet (per-message) | TreeKEM (per-epoch) | N/A (KEK versioning) | Per-blob random DEK (already achieved) -- revocation adds per-epoch KEM keys |
| Revocation latency | Instant (online members) | Next Commit (async) | Instant (IAM) | Eventually consistent (blob replication, seconds) |

### Key Takeaway from Prior Art

Every production system (Signal, MLS, AWS, GCP, Azure) follows the same pattern:
1. **Remove access** (revoke key/membership)
2. **Rotate encryption key** (new epoch/version)
3. **Keep old keys for decryption** (never re-encrypt)

chromatindb's existing primitives (tombstone delegation, group blobs, envelope encryption) map cleanly onto this pattern. The missing piece is the SDK orchestration that ties them together and the key versioning metadata in the directory.

## Implementation Sketch: Key Versioning

The core technical question is how to implement key epochs in the existing system without new wire types or C++ node changes.

**Approach: Directory-only key versioning (SDK layer)**

1. **UserEntry v2 format:** Add `key_version: uint32 BE` field after display_name. Backward-compatible: v1 entries have implicit key_version=0.

2. **Identity.rotate_kem():** Generate new ML-KEM-1024 keypair. Increment key_version. Sign new KEM pubkey with same signing key. Old KEM secret key retained in identity for decryption.

3. **Directory tracks history:** Cache stores list of `(key_version, kem_pubkey)` per user, not just latest. Latest used for encryption. All versions tried for decryption.

4. **write_encrypted:** Uses latest KEM pubkey from directory for each recipient. No change to envelope format -- the `kem_pk_hash` in each stanza identifies which key version was used.

5. **read_encrypted:** Current flow tries to decrypt with current KEM key. If `NotARecipientError`, try older KEM keys from directory history. The `kem_pk_hash` binary search in `envelope_decrypt` means only the matching key version is tried.

6. **No node changes required.** Nodes store signed blobs. They do not interpret UserEntry format. Key versioning is entirely SDK-layer.

**Why this works:** The envelope format already uses `kem_pk_hash` (SHA3-256 of KEM pubkey) to identify recipients. When the admin rotates KEM key, their new `kem_pk_hash` is different. Old envelopes have the old hash. `envelope_decrypt` does binary search on `kem_pk_hash` -- it will find the match only if the identity has the right KEM key version loaded. By keeping all KEM key versions in the identity, decryption of any epoch works.

## Sources

- [Envelope encryption - Google Cloud KMS](https://cloud.google.com/kms/docs/envelope-encryption)
- [Key rotation for AWS KMS - AWS Prescriptive Guidance](https://docs.aws.amazon.com/prescriptive-guidance/latest/aws-kms-best-practices/data-protection-key-rotation.html)
- [Encryption Key Rotation Best Practices - Kiteworks](https://www.kiteworks.com/regulatory-compliance/encryption-key-rotation-strategies/)
- [Secure Key-Updating for Lazy Revocation - Backes, Cachin, Oprea (ePrint 2005/334)](https://eprint.iacr.org/2005/334.pdf)
- [MLS Protocol - RFC 9420](https://datatracker.ietf.org/doc/rfc9420/)
- [MLS Architecture - RFC 9750](https://datatracker.ietf.org/doc/rfc9750/)
- [Signal Double Ratchet specification](https://signal.org/docs/specifications/doubleratchet/)
- [WhatsUpp with Sender Keys? - ePrint 2023/1385](https://eprint.iacr.org/2023/1385.pdf)
- [Enhancing Security through Certificates, Envelope Encryption and Key Rotation - DEV Community](https://dev.to/0xog_pg/enhancing-security-through-certificates-envelope-encryption-and-key-rotation-602)
- [Key rotation in AWS and GCP KMS - Medium](https://medium.com/@madhurajayashanka/key-rotation-in-aws-and-gcp-kms-what-really-happens-to-your-encrypted-data-7d2a12b07303)
- [Revisiting Updatable Encryption - ePrint 2021/268](https://eprint.iacr.org/2021/268)
- [Data encryption - HashiCorp Boundary](https://developer.hashicorp.com/boundary/docs/secure/encryption/data-encryption)

---
*Feature research for: Revocation & Key Lifecycle (chromatindb v2.1.1)*
*Researched: 2026-04-06*
