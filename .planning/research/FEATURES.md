# Feature Landscape: Client-Side PQ Envelope Encryption

**Domain:** Client-side encryption with pubkey directory, groups, and envelope encryption for a PQ-secure blob store
**Researched:** 2026-03-31
**Overall confidence:** HIGH (core patterns well-understood, PQ-specific integration MEDIUM)

## Context: What Already Exists

The Python SDK (v1.6.0) already provides:
- `ChromatinClient` with 15 async methods (write_blob, read_blob, delete_blob, list_blobs, exists, 10 query methods, pub/sub)
- `Identity` class managing ML-DSA-87 keypairs (generate, load, save, sign, verify)
- ML-KEM-1024 handshake for transport encryption (liboqs-python `KeyEncapsulation`)
- ChaCha20-Poly1305 AEAD (PyNaCl bindings)
- HKDF-SHA256 (pure-Python stdlib implementation, byte-identical to libsodium)
- SHA3-256 (hashlib stdlib)
- Namespace ownership: `SHA3-256(pubkey) = namespace`
- Delegation: owner writes `[0xDE 0x1E 0x6A 0x7E][delegate_pubkey:2592]` blob to grant write access
- Tombstones: owner writes `[0xDE 0xAD 0xBE 0xEF][target_hash:32]` to delete blobs
- Pub/sub notifications on namespace changes

**Key constraint:** All new features are pure SDK-side. No C++ node changes. The node stores signed blobs; it never interprets their contents. The encryption layer sits entirely in the SDK, making the node a zero-knowledge store.

## Table Stakes

Features that any client-side encryption system must have. Without these, the encryption is either broken, unusable, or a toy.

| Feature | Why Expected | Complexity | Dependencies |
|---------|--------------|------------|--------------|
| Envelope encryption format | Standard pattern: symmetric DEK encrypts data, asymmetric KEK wraps DEK per-recipient. Every serious encryption system uses this (age, AWS KMS, KBFS, Signal). | MEDIUM | Existing: `aead_encrypt/decrypt`, `hkdf_derive`, `sha3_256`. New: ML-KEM-1024 encap/decap via `oqs.KeyEncapsulation` (already used in handshake). |
| Multi-recipient key wrapping | A blob encrypted for N recipients must wrap the DEK N times (one per recipient's KEM public key). Single-recipient is useless for collaboration. age wraps file key per-recipient in stanzas; CMS has RecipientInfo structures. | MEDIUM | ML-KEM-1024 `encaps(pubkey)` returns `(ciphertext, shared_secret)`. Wrap DEK with each recipient's shared_secret. Each wrap = 1568 bytes of KEM ciphertext + 32 bytes AEAD-wrapped DEK + 16 bytes tag. |
| Encrypted blob binary format | Self-describing binary header with version, recipient count, per-recipient wrapped keys, then AEAD ciphertext. Must be parseable without external state. Every encrypted format (age, AWS SDK, Cryptomator) puts all decryption metadata in the blob itself. | MEDIUM | Define: `[version:1][recipient_count:2][recipient_stanzas][nonce:12][ciphertext+tag]`. Each stanza: `[kem_pubkey_hash:32][kem_ciphertext:1568][wrapped_dek:48]`. |
| Pubkey directory (key discovery) | Users must find each other's KEM public keys to encrypt to them. Without a directory, you need out-of-band key exchange for every recipient -- unusable. Keybase, Signal, and every E2EE system has key discovery. | HIGH | New: ML-KEM-1024 keypair generation per identity. Directory entries stored as blobs in a shared namespace. Uses existing delegation for write access. |
| Encrypted write helper | `client.write_encrypted(data, recipients, ttl)` -- one-call API that handles key generation, multi-recipient wrapping, encryption, and blob write. Without this, developers manually construct envelope format -- error-prone and defeats the purpose. | LOW | Composes: DEK generation, per-recipient ML-KEM encap, AEAD encrypt, format assembly, `write_blob()`. |
| Encrypted read helper | `client.read_encrypted(namespace, blob_hash)` -- reads blob, identifies recipient stanza for local identity, decapsulates DEK, decrypts payload. Returns plaintext or raises if not a recipient. | LOW | Composes: `read_blob()`, format parse, ML-KEM decap, AEAD decrypt. |
| Identity with KEM keypair | Each identity needs both ML-DSA-87 (signing, already exists) and ML-KEM-1024 (encryption, new). The signing key proves who you are; the KEM key lets others encrypt to you. Two separate keys because signing keys must not be used for encryption (cryptographic hygiene). | MEDIUM | Add `kem_public_key` and KEM secret key to `Identity`. Generate alongside existing ML-DSA-87 keypair. Save/load .kem / .kem.pub files alongside .key / .pub. |

### Detailed Analysis: Envelope Encryption Format

**How age does it (the gold standard for simplicity):**
- Header: version line, then one "stanza" per recipient wrapping the same 128-bit file key
- Each X25519 stanza: ephemeral pubkey + AEAD(wrap_key, file_key) where wrap_key = HKDF(DH_shared_secret, ephemeral_share || recipient)
- Payload: HKDF(file_key, nonce, "payload") derives payload key, then ChaCha20-Poly1305 in 64 KiB chunks
- Fixed nonce of zeros for key wrapping (one unique ephemeral key per recipient)

**How this maps to chromatindb's PQ stack:**
- Instead of X25519 DH, use ML-KEM-1024 encapsulation: `encaps(recipient_kem_pk)` -> `(ciphertext, shared_secret)`
- Wrap DEK: `HKDF(shared_secret, "chromatindb-envelope-v1", 32)` -> `wrap_key`, then `AEAD(wrap_key, dek, nonce=zeros)`
- The ML-KEM ciphertext is per-recipient (1568 bytes each, fixed size, no ambiguity)
- Payload encryption: ChaCha20-Poly1305 with random 12-byte nonce (already have `aead_encrypt`)

**Recommended format (binary, self-contained):**
```
[version: 1 byte = 0x01]
[recipient_count: 2 bytes big-endian]
For each recipient:
  [kem_pubkey_hash: 32 bytes]     -- SHA3-256 of recipient's KEM pubkey (for identification)
  [kem_ciphertext: 1568 bytes]    -- ML-KEM-1024 encapsulation output
  [wrapped_dek: 48 bytes]         -- AEAD(wrap_key, dek:32) = ciphertext:32 + tag:16
[nonce: 12 bytes]                 -- random, for payload encryption
[ciphertext: variable]            -- AEAD(dek, plaintext, nonce, ad=header_bytes)
```

Total overhead per recipient: 32 + 1568 + 48 = 1648 bytes.
Fixed overhead: 1 + 2 + 12 + 16 (AEAD tag) = 31 bytes.
Single-recipient encrypted blob: 31 + 1648 = 1679 bytes overhead.

**Why this format:** Compact binary (not text like age -- blobs are binary anyway), self-describing, no external state needed for decryption, and the header is authenticated as AD to the payload AEAD (binds recipients to ciphertext, prevents stanza substitution).

### Detailed Analysis: Pubkey Directory

The directory is the hardest feature. It determines how users discover each other.

**How Keybase does it:** Global Merkle tree of public keys, server-mediated but cryptographically auditable. Heavy infrastructure.

**How Signal does it:** Phone number -> identity key mapping on Signal servers. Contact discovery via SGX enclaves (complex).

**How this should work for chromatindb (SIMPLE, no new infrastructure):**

An "org directory" is a namespace owned by an admin identity. Users register by writing their public key info as a blob to that namespace (via delegation -- admin delegates write access to new members). Discovery is just `list_blobs(directory_namespace)` + `read_blob()` to get pubkeys.

**Directory entry format (blob data):**
```
[magic: 4 bytes = 0xD1 0xEC 0x70 0x01]  -- "DIRECT01" nibble-ish
[entry_type: 1 byte]                     -- 0x01 = user pubkey entry
[signing_pubkey: 2592 bytes]             -- ML-DSA-87 pubkey (already known from blob signature)
[kem_pubkey: 1568 bytes]                 -- ML-KEM-1024 encapsulation key (NEW)
[display_name_len: 2 bytes big-endian]
[display_name: variable, UTF-8]
```

**Key discovery flow:**
1. Admin creates org namespace, delegates write access to users
2. User generates Identity (ML-DSA-87 + ML-KEM-1024 keypair)
3. User writes directory entry blob to org namespace (self-registration)
4. Other users list org namespace, read entries, cache KEM pubkeys locally
5. To encrypt for "alice": look up alice's KEM pubkey from directory, ML-KEM encaps to it

**Why blobs-as-directory:** Zero new infrastructure. Directory entries are signed blobs that replicate across nodes automatically. Revocation = tombstone the user's directory entry blob. The node already handles replication, expiry, and access control.

### Detailed Analysis: Multi-Recipient Key Wrapping

**The pattern (from age, CMS, KBFS, and all envelope encryption systems):**
1. Generate random 32-byte DEK (data encryption key)
2. For each recipient: `(ct, ss) = ML-KEM-1024.encaps(recipient.kem_pubkey)`, then `wrap_key = HKDF(ss, info, 32)`, then `wrapped = AEAD(wrap_key, dek)`
3. Encrypt payload with DEK
4. Pack all KEM ciphertexts + wrapped DEKs into header

**Recipient identification:** Each stanza includes `SHA3-256(recipient.kem_pubkey)` so the decryptor can scan stanzas for theirs without trying every decapsulation. age uses the recipient public key as an identifier in the stanza args.

**Decryption flow:**
1. Parse header, scan stanzas for `SHA3-256(my_kem_pubkey)`
2. If found: `ss = ML-KEM-1024.decaps(my_kem_secret, kem_ciphertext)`
3. `wrap_key = HKDF(ss, info, 32)`, then `dek = AEAD_decrypt(wrap_key, wrapped_dek)`
4. Decrypt payload with DEK

## Differentiators

Features that set this apart from generic encryption libraries. Not expected, but valued.

| Feature | Value Proposition | Complexity | Dependencies |
|---------|-------------------|------------|--------------|
| Named groups in directory | Instead of listing N pubkeys per write, name a group ("engineering") and the SDK resolves member KEM pubkeys at encrypt time. Keybase teams, Signal groups, and GPG groups all provide this abstraction. | MEDIUM | Group entry in directory: `[magic][type=0x02][group_name][member_kem_pubkey_hashes]`. SDK resolves hashes to full KEM pubkeys from directory cache. |
| Auto-encrypt-to-group helper | `client.write_to_group(data, "engineering", ttl)` -- resolves group members, wraps DEK for each, writes encrypted blob. One-liner for the common case. | LOW | Composes: group resolution + `write_encrypted()`. |
| Self-encrypting write | `client.write_encrypted(data, ttl=3600)` with no explicit recipients encrypts to self only. Useful for private encrypted storage where only the writer can read back. | LOW | Default recipient = own KEM pubkey. Single-recipient envelope. |
| Pubkey caching with invalidation | Cache directory entries locally, refresh on pub/sub notification. Avoids re-fetching directory on every encrypt. | LOW | In-memory dict `{kem_pubkey_hash: kem_pubkey}`. Subscribe to directory namespace. Invalidate on tombstone notifications. |
| Streaming encryption for large blobs | For blobs approaching 100 MiB, encrypt in chunks (like age's 64 KiB STREAM construction) to avoid holding 200+ MiB in memory (plaintext + ciphertext). | HIGH | STREAM construction: chunk counter in nonce, final-chunk marker. Complex: different decryption flow, seekability concerns, chunk authentication. |
| Re-encryption helper for revocation | When a member is removed from a group, re-encrypt existing blobs for remaining members. Automates the "read, decrypt, re-encrypt, write, tombstone old" cycle. | MEDIUM | Composes existing operations. Warning: O(blobs * members) -- expensive for large datasets. Practically useful only for small datasets or critical secrets. |

### Detailed Analysis: Groups

**How Signal does groups (Sender Keys):**
- Each member generates a sender key (chain_key + signature_key)
- Sender key distributed to all members via pairwise encrypted channels
- Messages encrypted with sender's chain key, broadcast to group
- On member removal: all members rotate sender keys

**How Keybase does groups:**
- Team has per-team key, encrypted to each member's per-user key
- Key rotation on membership change (new key, re-encrypted for remaining members)
- Role-based: readers, writers, admins
- Subteams with independent keys

**What chromatindb should do (MUCH simpler than either):**

Groups are just named membership lists stored as blobs in the directory namespace. No shared group key. No key rotation protocol. When encrypting to a group, the SDK resolves the current member list and wraps the DEK for each member's KEM pubkey individually.

**Why no shared group key:**
1. chromatindb stores blobs, not streams -- there is no message ordering to optimize
2. Envelope encryption already handles multi-recipient efficiently (1648 bytes per recipient)
3. Shared group keys require key rotation on every membership change -- massive complexity
4. Per-blob envelope encryption means removing a member from the group ONLY affects future blobs (old blobs they already have keys for remain accessible -- this is correct, they already read them)

**Group entry format (blob data in directory namespace):**
```
[magic: 4 bytes = 0xD1 0xEC 0x70 0x01]
[entry_type: 1 byte = 0x02]             -- group entry
[group_name_len: 2 bytes big-endian]
[group_name: variable, UTF-8]
[member_count: 2 bytes big-endian]
For each member:
  [kem_pubkey_hash: 32 bytes]           -- SHA3-256 of member's KEM pubkey
```

**Group operations:**
- Create group: admin writes group blob to directory namespace
- Add member: admin writes updated group blob (new version), tombstones old
- Remove member: same -- write new group blob without removed member, tombstone old
- Resolve group: read group blob, look up each member's KEM pubkey from directory cache
- Encrypt to group: resolve members, wrap DEK for each

**Maximum group size consideration:**
At 1648 bytes per recipient, a 100-member group adds ~161 KiB of header to every encrypted blob. With 100 MiB max blob size, this is negligible. A 1000-member group adds ~1.6 MiB, still fine. The practical limit is ML-KEM encapsulation time (~1ms per recipient on modern hardware), so 1000 recipients = ~1 second. Acceptable for batch operations, but encrypt-heavy workloads with very large groups should be noted.

## Anti-Features

Features to explicitly NOT build. Each one either adds complexity without proportional value, invites security mistakes, or conflicts with the system's design principles.

| Anti-Feature | Why Avoid | What to Do Instead |
|--------------|-----------|-------------------|
| Shared group symmetric key | Requires key rotation protocol, re-encryption on every membership change, complex state management. Signal and Keybase invest enormous effort here because they need it for real-time messaging. chromatindb stores blobs -- per-blob envelope encryption is simpler and more secure. | Per-blob DEK wrapped individually for each group member. |
| Key escrow / recovery | Admin ability to decrypt all blobs. Destroys zero-knowledge property. If the admin can read everything, you do not have client-side encryption -- you have server-side encryption with extra steps. | If a user loses their key, their data is gone. This is a feature, not a bug. Document it clearly. |
| Proxy re-encryption | Academic technique where a proxy transforms ciphertext from one key to another without decrypting. Complex crypto, limited library support, and unnecessary when the client can just re-encrypt. | Client-side re-encryption helper: read, decrypt, re-encrypt for new recipients, write new blob. |
| Forward secrecy per blob | Ratcheting the encryption key per blob (like Signal's double ratchet). Only makes sense for ordered message streams. Blobs are unordered, independently readable. | Fresh random DEK per blob provides per-blob key independence. That is sufficient. |
| Homomorphic or searchable encryption | Ability to search or compute on encrypted data. Enormously complex, terrible performance, and not needed for a blob store. | Decrypt client-side, search locally. The SDK can provide `list_blobs()` with metadata filtering -- the blob hashes and timestamps are not encrypted. |
| Certificate authority / PKI hierarchy | X.509-style trust chains for pubkey validation. Massive complexity, requires CA infrastructure, certificate parsing, revocation lists. | Self-certifying keys: SHA3-256(pubkey) = namespace = identity. Trust is based on knowing someone's namespace, not on certificate chains. The directory is a simple key-value store, not a trust hierarchy. |
| Passphrase-based encryption | age supports scrypt-based encryption from a password. This requires strong passwords, is slow (by design), and mixes authentication models. chromatindb identities are already keypair-based. | All encryption uses KEM pubkeys. If someone wants passphrase protection, they encrypt their secret key file locally (out of scope for the SDK). |
| Automatic re-encryption on group change | When a member is removed, automatically re-encrypting all historical blobs. This is O(blobs * members), potentially hours of computation, and the removed member already read those blobs. | Provide a manual re-encryption helper for specific blobs that need it. Accept that past blobs with revoked members remain accessible to those members (they already had access). |
| Encrypted metadata / encrypted blob hashes | Encrypting blob sizes, timestamps, or access patterns. Metadata encryption is a separate (hard) problem. The node needs blob hashes and timestamps for sync, expiry, and queries. | Accept that the node sees blob metadata (size, timestamp, namespace, hash). Only the blob *data* is encrypted. This is the standard model (even Signal leaks metadata to servers). |

## Feature Dependencies

```
Identity with KEM keypair
  |
  +---> Pubkey directory entries (users publish KEM pubkeys)
  |       |
  |       +---> User discovery (list/fetch from directory)
  |       |       |
  |       |       +---> Multi-recipient envelope encryption
  |       |               |
  |       |               +---> write_encrypted() / read_encrypted()
  |       |               |
  |       |               +---> Named groups (reference members by KEM pubkey hash)
  |       |                       |
  |       |                       +---> write_to_group() helper
  |       |                       |
  |       |                       +---> Group management (create/add/remove)
  |       |
  |       +---> Self-registration (write own entry to directory)
  |
  +---> Encrypted blob format (uses KEM for key wrapping)
  |
  +---> Revocation (tombstone directory entry, existing primitive)
```

**Critical path:** Identity KEM keypair -> Encrypted blob format -> Pubkey directory -> Encrypted write/read helpers -> Groups

## MVP Recommendation

**Phase 1: Crypto primitives + encrypted blob format**
1. Add ML-KEM-1024 keypair to Identity (generation, save/load, encapsulate/decapsulate)
2. Define and implement encrypted blob binary format (envelope encryption)
3. Implement `encrypt_blob(plaintext, recipient_kem_pubkeys) -> bytes` and `decrypt_blob(encrypted_bytes, my_kem_identity) -> bytes`
4. Thorough test coverage including cross-validation with known test vectors

*Rationale:* The crypto layer must be rock-solid before building anything on top. This phase has zero dependency on the network -- pure local encryption/decryption.

**Phase 2: Directory + user management**
1. Define directory entry blob format (user pubkey entries, group entries)
2. Implement `OrgDirectory` class that manages a directory namespace
3. Self-registration: `directory.register(client, identity, display_name)`
4. User discovery: `directory.list_users(client)`, `directory.get_user(client, name_or_hash)`
5. Pubkey caching with pub/sub invalidation

*Rationale:* The directory is what makes encryption usable. Without it, users must exchange KEM pubkeys out-of-band.

**Phase 3: Encrypted client helpers + groups**
1. `client.write_encrypted(data, recipients_or_group, ttl)`
2. `client.read_encrypted(namespace, blob_hash)`
3. Group management: `directory.create_group()`, `directory.add_member()`, `directory.remove_member()`
4. Group resolution: `directory.resolve_group(client, group_name) -> list[kem_pubkey]`
5. `client.write_to_group(data, group_name, directory, ttl)`

*Rationale:* Groups build on the directory and encryption primitives. The helpers compose everything into a simple API.

**Defer:** Streaming encryption for large blobs (only needed if 100 MiB encrypted blobs are common), re-encryption helper (nice-to-have, not MVP), pubkey caching optimization (can start with fetch-every-time).

## Complexity Budget

| Feature | Estimated New LOC | Test Count | Risk |
|---------|-------------------|------------|------|
| Identity KEM keypair | ~120 | ~20 | LOW -- same liboqs pattern as ML-DSA-87 |
| Encrypted blob format | ~250 | ~40 | MEDIUM -- binary format correctness, edge cases |
| Directory entry format | ~150 | ~25 | LOW -- just blob data encoding/decoding |
| OrgDirectory class | ~300 | ~35 | MEDIUM -- network-dependent, delegation setup |
| write/read_encrypted | ~150 | ~30 | LOW -- composes existing primitives |
| Group management | ~200 | ~30 | LOW -- directory CRUD operations |
| Group resolution + helpers | ~100 | ~20 | LOW -- lookup and compose |
| **Total** | **~1270** | **~200** | |

## Key Design Decisions to Lock

1. **Separate KEM key from signing key.** Never use ML-DSA-87 for encryption. Never use ML-KEM-1024 for signing. Two-key identity is standard practice (GPG has signing + encryption subkeys, age has separate identity types).

2. **Per-blob random DEK, not per-namespace key.** The product direction doc suggested per-namespace key with HKDF(master, salt=blob_hash). This is fragile -- compromise of the namespace master key exposes all blobs. Per-blob random DEK means compromising one blob's key reveals nothing about other blobs. This is how age works, and it is the correct pattern.

3. **No shared group key.** Groups are just named member lists. Each encrypted blob wraps the DEK individually for each member. This eliminates key rotation complexity entirely.

4. **Directory entries are regular signed blobs.** No new protocol, no new message types, no node changes. A directory is a namespace with a known format for its blob data.

5. **Header authenticated as AEAD associated data.** The recipients-and-wrapped-keys header is bound to the ciphertext via AEAD's AD parameter. This prevents an attacker from swapping recipient stanzas between blobs (stanza substitution attack).

6. **ML-KEM encapsulation per-recipient, not shared.** Each recipient gets their own ephemeral ML-KEM encapsulation. No key reuse across recipients. This provides IND-CCA2 security per the ML-KEM standard.

## Sources

- [age encryption specification (C2SP)](https://github.com/C2SP/C2SP/blob/main/age.md) -- envelope format, multi-recipient stanzas, STREAM payload encryption [HIGH confidence]
- [age GitHub repository](https://github.com/FiloSottile/age) -- design philosophy, simplicity principles [HIGH confidence]
- [Google Cloud envelope encryption docs](https://docs.cloud.google.com/kms/docs/envelope-encryption) -- DEK/KEK pattern, wrapping flow [HIGH confidence]
- [Keybase KBFS crypto spec](https://book.keybase.io/docs/crypto/kbfs) -- per-block keys, TLF key distribution, rekeying [HIGH confidence]
- [Keybase teams design](https://book.keybase.io/docs/teams/design) -- team key management, XOR key splitting, role-based access [MEDIUM confidence]
- [Signal Protocol group encryption (Sender Keys)](https://signal.org/docs/) -- group key distribution, rotation on membership change [HIGH confidence]
- [libsodium sealed boxes](https://libsodium.gitbook.io/doc/public-key_cryptography/sealed_boxes) -- anonymous encryption with ephemeral keypair [HIGH confidence]
- [liboqs-python GitHub](https://github.com/open-quantum-safe/liboqs-python) -- KeyEncapsulation API for ML-KEM-1024 [HIGH confidence, verified against existing handshake code]
- [NIST FIPS 203 (ML-KEM)](https://csrc.nist.gov/pubs/fips/203/final) -- ML-KEM-1024 security level, key/ciphertext sizes [HIGH confidence]
- [AWS Encryption SDK message format](https://docs.aws.amazon.com/encryption-sdk/latest/developer-guide/message-format.html) -- header structure with encrypted data keys [MEDIUM confidence]
- [RFC 5869 HKDF](https://datatracker.ietf.org/doc/html/rfc5869) -- KDF for key derivation from shared secrets [HIGH confidence, already implemented in SDK]
