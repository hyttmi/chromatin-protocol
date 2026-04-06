# Stack Research

**Domain:** ACL revocation, key versioning, group membership revocation for chromatindb
**Researched:** 2026-04-06
**Confidence:** HIGH

## Key Finding: No New Dependencies Required

The existing chromatindb crypto stack already contains every primitive needed for ACL revocation, key versioning in envelope encryption, and group membership revocation. This is a pure application-layer feature set built on top of proven cryptographic foundations.

**The project constraint "No new dependencies" is not a limitation -- it is a confirmation that the original stack was well-chosen.**

## Existing Stack (Validated -- DO NOT CHANGE)

| Technology | Version | Purpose | Status |
|------------|---------|---------|--------|
| liboqs (ML-DSA-87) | latest via FetchContent | Signing: namespace ownership, delegation blobs, tombstones, UserEntry KEM-sig | Sufficient for all revocation signing |
| liboqs (ML-KEM-1024) | latest via FetchContent | Envelope encryption: per-recipient key encapsulation | Sufficient for key versioning |
| libsodium (ChaCha20-Poly1305) | latest via FetchContent | AEAD: transport, envelope DEK wrapping, DARE | Sufficient -- no new AEAD needed |
| libsodium (HKDF-SHA256) | latest via FetchContent | Key derivation: transport, DARE, envelope KEK | Sufficient -- new HKDF labels only |
| hashlib (SHA3-256) | Python stdlib | Hashing: namespace derivation, pk_hash, blob addressing | Sufficient |
| PyNaCl | pip dependency | Python AEAD bindings | Sufficient |
| liboqs-python | pip dependency | Python ML-DSA-87 + ML-KEM-1024 bindings | Sufficient |
| flatbuffers | pip dependency | Wire encoding for blob payloads | Sufficient |
| brotli | pip dependency | Envelope compression (suite 0x02) | Sufficient |
| libmdbx | latest via FetchContent | Storage: blob_map, delegation_map, tombstone_map | Sufficient -- delegation_map already tracks revocations |

## What Each Feature Needs (Stack Analysis)

### 1. ACL Revocation

**Already implemented in C++ node (confirmed by test_engine.cpp lines 1090-1118):**
- Owner tombstones a delegation blob -> `has_valid_delegation()` returns false
- Delegate writes are rejected with `IngestError::no_delegation`
- Re-delegation after revocation works (new timestamp = new blob hash)
- Delegate-written blobs survive revocation (data persists, write access revoked)

**What's missing (SDK-only work, no new crypto):**

| Gap | What to Build | Primitives Used |
|-----|---------------|-----------------|
| SDK revocation helper | `Directory.revoke_delegate()` that tombstones the delegation blob | ML-DSA-87 sign + existing `delete_blob()` |
| Delegation blob hash tracking | Directory must cache delegation blob_hashes per delegate | SHA3-256 for pk_hash lookup key |
| Revocation status query | `Directory.is_revoked(identity)` checking tombstone existence | Existing `has_tombstone_for()` or blob read |

**Crypto primitives needed:** None new. ML-DSA-87 signing (for tombstone creation) and SHA3-256 (for delegation index key) are already in the stack.

### 2. Key Versioning in Envelope Encryption

**Current state:** Each Identity has exactly one ML-KEM-1024 keypair. The UserEntry format stores one `kem_pk` per user. There is no concept of key generations.

**What's needed for key rotation:**

| Component | What to Build | Primitives Used | Integration Point |
|-----------|---------------|-----------------|-------------------|
| Versioned UserEntry | New blob type `KROT` with `[magic:4][version:1][key_version:4 BE][kem_pk:1568][kem_sig:variable]` | ML-DSA-87 sign (cross-sign new KEM pk), ML-KEM-1024 keygen | `_directory.py` encode/decode |
| Key version index in Directory | Cache maps `(pubkey_hash, version) -> kem_pk` | SHA3-256 for index keys | `Directory._populate_cache()` |
| Versioned envelope encrypt | `envelope_encrypt()` uses latest KEM pubkey per recipient | ML-KEM-1024 encap (unchanged) | `_envelope.py` recipient resolution |
| Old key retention for decrypt | Identity holds historical KEM keypairs for decrypting old envelopes | ML-KEM-1024 decap (unchanged) | `identity.py` + `_envelope.py` |

**Design decision (opinionated): Use new blob type, not UserEntry v2.**

Why: A "key rotation" blob (`KROT`) is a separate blob from the UserEntry. This means:
- Old UserEntry blobs remain valid and don't need updating
- Key rotation is append-only (new blob published, old one stays)
- Latest-timestamp-wins resolution (same pattern as GroupEntry)
- No migration path needed (pre-MVP constraint)

**Proposed `KROT` binary format:**
```
[magic:4 "KROT"][version:1 0x01][key_version:4 BE][kem_pk:1568][kem_sig:variable]
```

Where `kem_sig` is ML-DSA-87 sign of `[key_version_be:4][kem_pk:1568]` -- binding the version number to the key prevents version downgrade attacks.

**Crypto primitives needed:** None new. ML-KEM-1024 keygen + ML-DSA-87 cross-signing are both in the stack. The HKDF label `chromatindb-envelope-kek-v1` remains unchanged because the KEM shared secret is already unique per encapsulation.

### 3. Group Membership Revocation

**Current state:**
- `Directory.remove_member()` writes a new GroupEntry blob without the removed member
- `write_to_group()` resolves membership at call time from the latest GroupEntry
- **Problem:** Removed member can still decrypt any envelope where they were a recipient (old data readable with old keys)

**Project constraint (from PROJECT.md):** "Old data stays readable with old keys (no re-encryption)"

This means the only thing needed is:
1. After `remove_member()`, new `write_to_group()` calls exclude the removed member (already works -- latest GroupEntry wins)
2. If the removed member had access to shared symmetric keys, rotate them (not applicable -- chromatindb uses per-blob envelope encryption, not shared group keys)
3. If the removed member's KEM pubkey should be excluded from future envelopes, rotate the admin's group key (not applicable -- KEM encap is per-recipient, there is no group key)

| Gap | What to Build | Primitives Used |
|-----|---------------|-----------------|
| Post-removal key rotation advisory | After `remove_member()`, recommend key rotation to all remaining members if forward secrecy desired | None -- documentation only |
| Revocation timestamp tracking | GroupEntry gains `revocation_log` or separate revocation blob for audit | ML-DSA-87 sign (existing) |
| write_to_group with freshness check | Force directory cache refresh before encrypting to ensure revoked member excluded | Existing pub/sub invalidation |

**Crypto primitives needed:** None new. The per-blob envelope encryption model (decided in v1.7.0 Phase 77 -- "Groups as blobs, NOT shared keys") was specifically designed to avoid key rotation complexity on membership changes. The decision was correct and pays off here.

## Recommended Stack (Unchanged)

### Core Technologies (NO CHANGES)

| Technology | Version | Purpose | Why No Change Needed |
|------------|---------|---------|---------------------|
| liboqs ML-DSA-87 | latest | Signing revocation tombstones, cross-signing rotated KEM pubkeys | Same operations as delegation creation |
| liboqs ML-KEM-1024 | latest | New keypair generation on rotation, encap to latest key per recipient | `generate_keypair()` already exists |
| libsodium ChaCha20-Poly1305 | latest | AEAD for envelope DEK wrapping (unchanged) | KEK is unique per encapsulation regardless of key version |
| libsodium HKDF-SHA256 | latest | KEK derivation (unchanged) | Label `chromatindb-envelope-kek-v1` stays the same |
| hashlib SHA3-256 | stdlib | pk_hash for directory index, namespace derivation | Existing usage pattern |

### Supporting Libraries (NO CHANGES)

| Library | Version | Purpose | When to Use |
|---------|---------|---------|-------------|
| PyNaCl | existing | Python AEAD + HKDF bindings | All SDK crypto operations |
| liboqs-python | existing | Python ML-DSA-87 + ML-KEM-1024 | Identity operations, envelope encrypt |
| flatbuffers | existing | Wire encoding for blobs | All blob payloads |
| brotli | existing | Envelope compression (suite 0x02) | Large encrypted payloads |

### New HKDF Labels (Application-Layer Only)

No new HKDF labels are required. The existing `chromatindb-envelope-kek-v1` label works for all key versions because:

1. The KEM shared secret is unique per ML-KEM encapsulation (different KEM pubkey = different shared secret)
2. The HKDF info label is a domain separator, not a key-version identifier
3. Changing the label per key version would add complexity with zero security benefit

**Important: Do NOT create a `chromatindb-envelope-kek-v2` label.** The versioning happens at the key level (which KEM pubkey to use), not at the KDF level.

## What NOT to Use

| Avoid | Why | Use Instead |
|-------|-----|-------------|
| Shared group symmetric key | Requires key rotation protocol on every membership change; violates "no new crypto" constraint | Per-blob envelope encryption (already implemented) |
| Re-encryption on revocation | Project constraint says "old data stays readable with old keys"; re-encryption is O(N) in blob count | Accept old-data-readable, encrypt new data to new membership |
| New cipher suite for key versions | Overloads suite semantics; suite identifies crypto algorithms, not key generations | Store key_version in directory blob, not envelope header |
| ENVELOPE_VERSION bump to 0x02 | Version byte is for format changes, not key lifecycle changes; bumping it would break all existing envelopes | Keep version 0x01; key_version is metadata in directory, not envelope |
| OpenSSL for any crypto | Project constraint: "No OpenSSL. Prefer minimal deps -- liboqs for PQ, libsodium for symmetric" | Continue using liboqs + libsodium |
| External key management (KMS) | Unnecessary complexity; node-local master key + per-identity KEM keypairs is sufficient | Identity file-based key management |

## Architecture Integration Points

### C++ Node (Minimal Changes)

The C++ node needs **zero crypto changes** for this milestone. The revocation mechanism (tombstone delegation blob -> `has_valid_delegation()` returns false) is already implemented and tested. Key versioning and group membership are SDK-only concerns -- the node stores blobs and verifies ownership, it does not interpret envelope encryption.

Possible C++ change: a new query type to check "is this pubkey revoked in this namespace?" -- but this can be answered by the SDK via `has_tombstone_for()` or delegation list query, so it's optional.

### Python SDK (Application Logic Changes)

| Module | Change | Complexity |
|--------|--------|------------|
| `_directory.py` | Add KROT blob encode/decode, versioned key index in cache, `revoke_delegate()` helper | Medium |
| `_envelope.py` | No changes to encrypt/decrypt -- recipient resolution happens before envelope_encrypt | None |
| `identity.py` | Add `rotate_kem()` method that generates new ML-KEM keypair, increments version | Low |
| `client.py` | Add `revoke_delegation()` convenience method wrapping delete_blob | Low |

### Wire Format (NO CHANGES)

The envelope wire format `[magic:4][version:1][suite:1][count:2 BE][nonce:12][stanzas][ciphertext]` does not change. Key versioning is a directory-layer concern: the envelope always encrypts to the latest KEM pubkey for each recipient. Decryption tries all known KEM keypairs (current + historical).

## Version Compatibility

| Package A | Compatible With | Notes |
|-----------|-----------------|-------|
| KROT blob (new) | UserEntry blob (existing) | Coexist in same directory namespace; KROT supplements, doesn't replace |
| Identity with multiple KEM keys | envelope_decrypt (existing) | Decrypt tries current key first, then historical keys on NotARecipientError |
| Rotated KEM pubkey | Old envelopes | Old envelopes encrypted to old KEM pk still decryptable if Identity retains old secret key |
| Removed group member | write_to_group | Excluded from new envelopes automatically via latest GroupEntry resolution |

## Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Identity loses old KEM secret key after rotation | Cannot decrypt old envelopes | Identity.rotate_kem() MUST preserve old keypair; save all historical .kem files |
| Directory cache stale during revocation window | Revoked member included in new encryption | Force cache refresh before write_to_group when revocation is recent |
| Concurrent key rotation by same identity | Two KROT blobs with same version number | Use timestamp + sequence for ordering, not just version number; latest-timestamp-wins |
| KROT blob replication lag | Remote nodes serve stale KEM pubkey | Accept eventual consistency; encrypting to old key is still secure (recipient can decrypt with old or new key) |

## Sources

- `db/tests/engine/test_engine.cpp` lines 1090-1250 -- ACL revocation via tombstone, verified working
- `db/storage/storage.cpp` lines 911-926 -- `has_valid_delegation()` implementation
- `sdk/python/chromatindb/_envelope.py` -- current envelope encrypt/decrypt, no key version concept
- `sdk/python/chromatindb/_directory.py` -- UserEntry/GroupEntry encode/decode, Directory cache
- `sdk/python/chromatindb/identity.py` -- single KEM keypair per Identity
- `db/PROTOCOL.md` lines 975-1079 -- envelope binary format spec, HKDF label registry
- `.planning/PROJECT.md` lines 13-26 -- v2.1.1 target features and constraints
- MEMORY.md key decisions -- "Groups as blobs (NOT shared keys)", "KEM-then-Wrap mandatory"

**Confidence: HIGH** -- All findings verified against source code. No external research needed because the feature set is built entirely on existing, validated primitives. The "no new dependencies" constraint is satisfied by design.

---
*Stack research for: chromatindb v2.1.1 -- Revocation & Key Lifecycle*
*Researched: 2026-04-06*
